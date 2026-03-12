/**
 * solunad — Soluna Network Audio Daemon
 *
 * Network audio daemon with YAML configuration support.
 * Usage:
 *   solunad --config /etc/soluna/config.yaml
 *   solunad --tx --device hw:0 --dest 239.69.0.1:5004
 *   solunad --rx --device hw:0 --port 5004
 *
 * SPDX-License-Identifier: OpenSonic-Community-1.0
 */

#include <soluna/soluna.h>
#include <soluna/security/license.h>
#include <soluna/pal/audio.h>
#include <soluna/pal/net.h>
#include <soluna/pal/time.h>
#include <soluna/pal/thread.h>
#include <soluna/pipeline/ring_buffer.h>
#include <soluna/pipeline/file_source.h>
#include <soluna/pipeline/playout_buffer.h>
#include <soluna/pipeline/pipeline.h>
#include <soluna/sync/ptp_engine.h>
#include <soluna/sync/drift_dll.h>
#include <soluna/transport/ostp.h>
#include <soluna/transport/packet_scheduler.h>
#include <soluna/transport/transport_manager.h>
#include <soluna/transport/aes67.h>
#include <soluna/config/config.h>
#include <soluna/control/websocket_server.h>
#include <soluna/util/wav_writer.h>
#include "web_embedded.h"

#include <soluna/wifi/fec.h>

#ifdef SOLUNA_HAS_OPUS
#include <soluna/codec/opus_wrapper.h>
#endif

#include <soluna/pipeline/dsp_chain.h>
#include <soluna/pipeline/builtin_plugins.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Network interface detection (WiFi vs wired)
#include <arpa/inet.h>
#include <netdb.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <unistd.h>
#ifdef __APPLE__
#include <net/if_media.h>
#endif
#ifdef __linux__
#include <linux/wireless.h>
#endif

static std::string detect_outgoing_interface(const char* multicast_ip) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return "";
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5004);
    inet_pton(AF_INET, multicast_ip, &addr.sin_addr);
    connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));

    struct sockaddr_in local{};
    socklen_t len = sizeof(local);
    getsockname(sock, reinterpret_cast<struct sockaddr*>(&local), &len);
    close(sock);

    struct ifaddrs* ifas = nullptr;
    if (getifaddrs(&ifas) != 0) return "";
    std::string result;
    for (auto* ifa = ifas; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        auto* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        if (sa->sin_addr.s_addr == local.sin_addr.s_addr) {
            result = ifa->ifa_name;
            break;
        }
    }
    freeifaddrs(ifas);
    return result;
}

static bool is_interface_wifi(const std::string& ifname) {
    if (ifname.empty()) return false;
#ifdef __APPLE__
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return false;
    struct ifmediareq ifmr{};
    strlcpy(ifmr.ifm_name, ifname.c_str(), sizeof(ifmr.ifm_name));
    bool wifi = false;
    if (ioctl(sock, SIOCGIFMEDIA, &ifmr) == 0) {
        wifi = (IFM_TYPE(ifmr.ifm_current) == IFM_IEEE80211);
    }
    close(sock);
    return wifi;
#elif defined(__linux__)
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return false;
    struct iwreq wrq{};
    strncpy(wrq.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
    bool wifi = (ioctl(sock, SIOCGIWNAME, &wrq) == 0);
    close(sock);
    return wifi;
#else
    return false;
#endif
}

#ifdef __APPLE__
#include "soluna/soluna_shm.h"
#include <dns_sd.h>
#include <sys/select.h>
// ── Mac system volume helpers ────────────────────────────────────────────────
// Find the default *system* output device (physical speakers), skipping virtual
// devices like Soluna/BlackHole when they are the default output.
#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioToolbox.h>

static AudioDeviceID find_physical_output_device() {
    // Try kAudioHardwarePropertyDefaultSystemOutputDevice first
    // (this is the device for system sounds, usually the physical speaker)
    AudioDeviceID dev = kAudioObjectUnknown;
    UInt32 sz = sizeof(dev);
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDefaultSystemOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &sz, &dev) == noErr
        && dev != kAudioObjectUnknown) {
        // Check if it has volume control
        AudioObjectPropertyAddress vaddr = {
            kAudioDevicePropertyVolumeScalar,
            kAudioObjectPropertyScopeOutput, 1
        };
        if (AudioObjectHasProperty(dev, &vaddr)) return dev;
    }

    // Fallback: enumerate all devices and find one with volume control
    addr.mSelector = kAudioHardwarePropertyDevices;
    sz = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &sz);
    if (sz == 0) return kAudioObjectUnknown;

    std::vector<AudioDeviceID> devs(sz / sizeof(AudioDeviceID));
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &sz, devs.data());

    for (auto d : devs) {
        // Must have output channels
        AudioObjectPropertyAddress chAddr = {
            kAudioDevicePropertyStreamConfiguration,
            kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        UInt32 csz = 0;
        AudioObjectGetPropertyDataSize(d, &chAddr, 0, nullptr, &csz);
        if (csz == 0) continue;
        std::vector<uint8_t> cbuf(csz);
        auto* abl = reinterpret_cast<AudioBufferList*>(cbuf.data());
        if (AudioObjectGetPropertyData(d, &chAddr, 0, nullptr, &csz, abl) != noErr) continue;
        UInt32 outCh = 0;
        for (UInt32 i = 0; i < abl->mNumberBuffers; i++) outCh += abl->mBuffers[i].mNumberChannels;
        if (outCh == 0) continue;

        // Must have volume control (physical device)
        AudioObjectPropertyAddress vaddr = {
            kAudioDevicePropertyVolumeScalar,
            kAudioObjectPropertyScopeOutput, 1
        };
        if (AudioObjectHasProperty(d, &vaddr)) return d;
    }
    return kAudioObjectUnknown;
}

static float get_system_volume() {
    AudioDeviceID dev = find_physical_output_device();
    if (dev == kAudioObjectUnknown) return -1.0f;
    Float32 vol = 0.0f;
    UInt32 sz = sizeof(vol);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyVolumeScalar,
        kAudioObjectPropertyScopeOutput, 1
    };
    if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &sz, &vol) == noErr)
        return vol;
    return -1.0f;
}

static bool set_system_volume(float vol) {
    AudioDeviceID dev = find_physical_output_device();
    if (dev == kAudioObjectUnknown) return false;
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    bool ok = false;
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyVolumeScalar,
        kAudioObjectPropertyScopeOutput, 0
    };
    for (UInt32 ch = 1; ch <= 2; ch++) {
        addr.mElement = ch;
        if (AudioObjectSetPropertyData(dev, &addr, 0, nullptr, sizeof(vol), &vol) == noErr)
            ok = true;
    }
    return ok;
}

static bool get_system_mute() {
    AudioDeviceID dev = find_physical_output_device();
    if (dev == kAudioObjectUnknown) return false;
    UInt32 muted = 0;
    UInt32 sz = sizeof(muted);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyMute,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };
    AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &sz, &muted);
    return muted != 0;
}

static bool set_system_mute(bool mute) {
    AudioDeviceID dev = find_physical_output_device();
    if (dev == kAudioObjectUnknown) return false;
    UInt32 muted = mute ? 1 : 0;
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyMute,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };
    return AudioObjectSetPropertyData(dev, &addr, 0, nullptr, sizeof(muted), &muted) == noErr;
}

// ── Soluna device volume listener ────────────────────────────────────────────
// When the user presses Mac keyboard volume keys, macOS changes the Soluna
// virtual device volume. We listen for that change and apply it to
// g_mon_volume (local speaker output only), keeping network TX at full volume.

// Forward declarations for globals defined below
static std::atomic<float>& soluna_vol_ref();
static std::atomic<bool>&  soluna_mute_ref();

static AudioDeviceID find_soluna_device() {
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 sz = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &sz);
    if (sz == 0) return kAudioObjectUnknown;

    std::vector<AudioDeviceID> devs(sz / sizeof(AudioDeviceID));
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &sz, devs.data());

    for (auto d : devs) {
        CFStringRef name = nullptr;
        UInt32 nsz = sizeof(name);
        AudioObjectPropertyAddress naddr = {
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        if (AudioObjectGetPropertyData(d, &naddr, 0, nullptr, &nsz, &name) != noErr || !name)
            continue;

        char buf[256];
        bool is_soluna = false;
        if (CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8)) {
            is_soluna = (strstr(buf, "Soluna") != nullptr);
        }
        CFRelease(name);
        if (is_soluna) return d;
    }
    return kAudioObjectUnknown;
}

static OSStatus soluna_volume_changed(AudioObjectID inObjectID,
                                       UInt32 inNumberAddresses,
                                       const AudioObjectPropertyAddress* inAddresses,
                                       void* inClientData) {
    (void)inObjectID; (void)inNumberAddresses; (void)inClientData;
    for (UInt32 i = 0; i < inNumberAddresses; i++) {
        if (inAddresses[i].mSelector == kAudioDevicePropertyVolumeScalar) {
            Float32 vol = 1.0f;
            UInt32 sz = sizeof(vol);
            AudioObjectPropertyAddress addr = inAddresses[i];
            if (AudioObjectGetPropertyData(inObjectID, &addr, 0, nullptr, &sz, &vol) == noErr) {
                soluna_vol_ref().store(vol);
                fprintf(stderr, "[volume] Mac keyboard → %.0f%%\n", vol * 100.0f);
            }
        } else if (inAddresses[i].mSelector == kAudioDevicePropertyMute) {
            UInt32 muted = 0;
            UInt32 sz = sizeof(muted);
            AudioObjectPropertyAddress addr = inAddresses[i];
            if (AudioObjectGetPropertyData(inObjectID, &addr, 0, nullptr, &sz, &muted) == noErr) {
                soluna_mute_ref().store(muted != 0);
                fprintf(stderr, "[volume] Mac keyboard → %s\n", muted ? "muted" : "unmuted");
            }
        }
    }
    return noErr;
}

static void start_soluna_volume_listener() {
    AudioDeviceID dev = find_soluna_device();
    if (dev == kAudioObjectUnknown) {
        fprintf(stderr, "[volume] Soluna device not found, keyboard volume disabled\n");
        return;
    }

    // Listen for volume changes on output scope, channel 1
    AudioObjectPropertyAddress vol_addr = {
        kAudioDevicePropertyVolumeScalar,
        kAudioObjectPropertyScopeOutput, 1
    };
    AudioObjectAddPropertyListener(dev, &vol_addr, soluna_volume_changed, nullptr);

    // Listen for mute changes
    AudioObjectPropertyAddress mute_addr = {
        kAudioDevicePropertyMute,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };
    AudioObjectAddPropertyListener(dev, &mute_addr, soluna_volume_changed, nullptr);

    // Read initial volume
    Float32 vol = 1.0f;
    UInt32 sz = sizeof(vol);
    if (AudioObjectGetPropertyData(dev, &vol_addr, 0, nullptr, &sz, &vol) == noErr) {
        soluna_vol_ref().store(vol);
    }

    fprintf(stderr, "[volume] Listening for Mac keyboard volume (Soluna device %u, vol=%.0f%%)\n",
            (unsigned)dev, vol * 100.0f);
}
#endif

static std::atomic<bool>     g_running{true};
static std::atomic<float>    g_rx_volume{0.00375f}; // slider 50% with quadratic curve (max 0.015)
static std::atomic<bool>     g_rx_muted{false};

// ── Stream mode (Sync vs Jam) ────────────────────────────────────────────────
static std::atomic<soluna::StreamMode> g_stream_mode{soluna::StreamMode::Sync};
static std::atomic<uint64_t> g_packets{0};
static std::atomic<uint64_t> g_seq_errors{0};
static std::atomic<size_t>   g_buf_fill{0};
static std::atomic<size_t>   g_buf_cap{0};
static std::atomic<uint32_t> g_buf_target_ms{20};   // target jitter buffer

// PLC (Packet Loss Concealment) statistics
static std::atomic<uint64_t> g_crc_errors{0};
static std::atomic<uint64_t> g_plc_frames{0};
static std::atomic<uint64_t> g_lost_packets{0};

// ── File Player globals ───────────────────────────────────────────────────────
static std::atomic<bool>     g_player_active{false};
static std::atomic<bool>     g_player_paused{false};
// PTP ns timestamp when playback stream started (0 = not started)
static std::atomic<int64_t>  g_player_stream_start_ns{0};
// Decoded frames output so far (for current-position calculation)
static std::atomic<int64_t>  g_player_frame_pos{0};
// Ring buffer pointer (set by run_rx / run_tx before player can be used)
static soluna::pipeline::RingBuffer* g_player_ring_ptr = nullptr;
// Upload state (protected by g_player_mutex)
static std::mutex            g_player_mutex;
static std::string           g_player_file_path;
static std::string           g_player_file_name;
static uint32_t              g_player_file_size = 0;
// Duration reported back to UI
static std::atomic<uint64_t> g_player_duration_ms{0};
// Player thread handle
static std::thread           g_player_thread;

// Config globals (set by run_tx / run_rx so ws_handle can reference them)
static uint32_t g_cfg_channels    = 1;
static uint32_t g_cfg_sample_rate = 48000;
static uint16_t g_cfg_port        = 5004;
static char     g_cfg_multicast[64] = "239.69.0.1";

// ── Monitor (TX-only local playback) ──────────────────────────────────────────
static std::atomic<bool>     g_mon_supported{false};
static std::atomic<bool>     g_mon_active{false};
static std::atomic<float>    g_mon_volume{1.0f};
static std::atomic<bool>     g_mon_muted{false};

// Implementations for soluna_volume_changed callback (defined above in __APPLE__ block)
#ifdef __APPLE__
static std::atomic<float>& soluna_vol_ref()  { return g_mon_volume; }
static std::atomic<bool>&  soluna_mute_ref() { return g_mon_muted; }
#endif

static std::atomic<uint64_t> g_mon_packets{0};
static std::atomic<uint32_t> g_mon_target_ms{20};
static std::atomic<uint32_t> g_speaker_delay_ms{40}; // default 40ms to match iPhone jitter buffer

#include <mutex>
struct MonitorReq { bool pending = false; std::string device; };
static std::mutex     g_mon_mutex;
static MonitorReq     g_mon_start_req;
static std::atomic<bool> g_mon_stop_req{false};

// ── Input passthrough (external input → SHM → monitor) ───────────────────────
static std::atomic<bool>     g_input_active{false};
static std::atomic<float>    g_input_volume{5.0f};
static std::atomic<uint32_t> g_input_channel{2};  // 0-indexed, default ch3
struct InputReq { bool pending = false; std::string device; uint32_t channel = 2; };
static std::mutex     g_input_mutex;
static InputReq       g_input_start_req;
static std::atomic<bool> g_input_stop_req{false};

// ── Browser audio streaming ───────────────────────────────────────────────────
static std::atomic<bool>     g_audio_streaming{false};
static soluna::control::WebSocketServer* g_ws_server_ptr = nullptr;

// ── Monitor speaker underrun counter ─────────────────────────────────────────
static std::atomic<uint64_t> g_mon_underruns{0};

// ── Multi-track recording ────────────────────────────────────────────────────
static std::string             g_record_dir;         // --record-dir path (empty = disabled)
static std::mutex              g_rec_mutex;
static soluna::util::WavWriter g_rec_tx;             // TX mic capture
static soluna::util::WavWriter g_rec_monitor;        // monitor playback capture
static std::atomic<bool>       g_rec_active{false};  // true while recording

// ── Global RX delay (pushed to all receivers, 0 = device-local) ──────────────
static std::atomic<uint32_t> g_rx_delay_ms{0};

// ── Auto-tune (mic-based noise detection → buffer adjustment) ────────────────
static std::atomic<bool>     g_tune_active{false};
static std::atomic<bool>     g_tune_stop_req{false};
static std::atomic<bool>     g_tune_start_req{false};
static std::atomic<uint32_t> g_tune_clicks{0};     // total clicks detected
static std::atomic<uint32_t> g_tune_dropouts{0};   // total silence gaps detected
static std::atomic<float>    g_tune_rms_db{-100.0f}; // current RMS in dBFS
static std::atomic<uint32_t> g_tune_adjustments{0}; // total buffer increases

// ── WiFi reliability features (toggleable via Web UI) ────────────────────────
static std::atomic<bool> g_wifi_dup_send{true};      // duplicate packet send (TX)
static std::atomic<bool> g_wifi_fec{true};            // FEC XOR parity (TX+RX)
static std::atomic<bool> g_wifi_nack{true};           // NACK retransmission (TX+RX)
static std::atomic<bool> g_wifi_wsola_plc{true};      // WSOLA PLC (RX)
static std::atomic<bool> g_wifi_adaptive_jitter{true}; // adaptive jitter buffer (RX)
static std::atomic<bool> g_wifi_dedup{true};           // duplicate packet detection (RX)

// ── DSP chain pointer (set by run_rx so ws_handle can access it) ─────────────
static soluna::pipeline::DspChain* g_dsp_chain_ptr = nullptr;

// ── Unicast relay (P2P) — forward TX packets to registered peers ──────────────
static constexpr uint16_t kRelayPort = 5099;
struct RelayPeer {
    sockaddr_in addr;
    std::chrono::steady_clock::time_point last_seen;
};
static std::mutex g_relay_mutex;
static std::vector<RelayPeer> g_relay_peers;
static int g_relay_sock = -1;
static std::atomic<bool> g_relay_running{false};

static void relay_forward(const uint8_t* data, size_t len) {
    if (g_relay_sock < 0) return;
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_relay_mutex);
    // Remove stale peers (>15s no heartbeat)
    g_relay_peers.erase(
        std::remove_if(g_relay_peers.begin(), g_relay_peers.end(),
            [&](const RelayPeer& p) {
                return std::chrono::duration_cast<std::chrono::seconds>(
                    now - p.last_seen).count() > 15;
            }),
        g_relay_peers.end());
    // Send to all active peers
    for (const auto& peer : g_relay_peers) {
        sendto(g_relay_sock, data, len, 0,
               (const sockaddr*)&peer.addr, sizeof(peer.addr));
    }
}

static void relay_listener_thread() {
    uint8_t buf[64];
    while (g_relay_running.load()) {
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(g_relay_sock, buf, sizeof(buf), 0,
                             (sockaddr*)&from, &from_len);
        if (n <= 0) continue;

        // Any packet = peer registration / heartbeat
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(g_relay_mutex);
        bool found = false;
        for (auto& peer : g_relay_peers) {
            if (peer.addr.sin_addr.s_addr == from.sin_addr.s_addr &&
                peer.addr.sin_port == from.sin_port) {
                peer.last_seen = now;
                found = true;
                break;
            }
        }
        if (!found) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            fprintf(stderr, "[relay] New peer: %s:%u\n", ip, ntohs(from.sin_port));
            g_relay_peers.push_back({from, now});
        }
    }
}

// ── WAN relay (forward TX to remote soluna-relay server) ─────────────────────
static int g_wan_relay_sock = -1;
static sockaddr_in g_wan_relay_addr{};
static std::atomic<bool> g_wan_relay_running{false};
static std::chrono::steady_clock::time_point g_wan_relay_last_hello;
static std::mutex g_wan_peers_mutex;
static std::vector<sockaddr_in> g_wan_peers;

// Current WAN relay config (for channel.get / channel.set)
static std::mutex g_wan_cfg_mutex;
static std::string g_wan_cfg_host = "soluna-relay.fly.dev";
static uint16_t    g_wan_cfg_port = 5100;
static std::string g_wan_cfg_group = "default";
static std::string g_wan_cfg_password;

static void wan_relay_add_peer(const sockaddr_in& peer) {
    std::lock_guard<std::mutex> lk(g_wan_peers_mutex);
    for (const auto& p : g_wan_peers) {
        if (p.sin_addr.s_addr == peer.sin_addr.s_addr && p.sin_port == peer.sin_port)
            return;
    }
    g_wan_peers.push_back(peer);
}

static void wan_relay_init(const std::string& host, uint16_t port,
                           const std::string& group, const std::string& password) {
    // Save config for channel.get / channel.set
    {
        std::lock_guard<std::mutex> lk(g_wan_cfg_mutex);
        g_wan_cfg_host = host;
        g_wan_cfg_port = port;
        g_wan_cfg_group = group;
        g_wan_cfg_password = password;
    }
    g_wan_relay_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_wan_relay_sock < 0) {
        fprintf(stderr, "[wan-relay] Failed to create socket\n");
        return;
    }

    g_wan_relay_addr.sin_family = AF_INET;
    g_wan_relay_addr.sin_port = htons(port);
    // Try IP first, fall back to DNS resolution
    if (inet_pton(AF_INET, host.c_str(), &g_wan_relay_addr.sin_addr) <= 0) {
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
            fprintf(stderr, "[wan-relay] Cannot resolve host: %s\n", host.c_str());
            close(g_wan_relay_sock);
            g_wan_relay_sock = -1;
            return;
        }
        g_wan_relay_addr.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }

    // Send JOIN message
    std::string join_msg = "JOIN:" + group;
    if (!password.empty()) join_msg += ":" + password;
    join_msg += "\n";
    sendto(g_wan_relay_sock, join_msg.c_str(), join_msg.size(), 0,
           (const sockaddr*)&g_wan_relay_addr, sizeof(g_wan_relay_addr));

    g_wan_relay_running.store(true);
    g_wan_relay_last_hello = std::chrono::steady_clock::now();

    fprintf(stderr, "[wan-relay] Connected to %s:%u group='%s'\n",
            host.c_str(), port, group.c_str());
}

static void wan_relay_forward(const uint8_t* data, size_t len) {
    if (g_wan_relay_sock < 0 || !g_wan_relay_running.load()) return;

    // Send to all direct peers (P2P)
    {
        std::lock_guard<std::mutex> lk(g_wan_peers_mutex);
        for (const auto& peer : g_wan_peers) {
            sendto(g_wan_relay_sock, data, len, 0,
                   (const sockaddr*)&peer, sizeof(peer));
        }
    }
    // Also send via relay as fallback
    sendto(g_wan_relay_sock, data, len, 0,
           (const sockaddr*)&g_wan_relay_addr, sizeof(g_wan_relay_addr));

    // Send HELLO heartbeat every 5 seconds
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - g_wan_relay_last_hello).count();
    if (elapsed >= 5) {
        const char hello[] = "HELLO\n";
        sendto(g_wan_relay_sock, hello, strlen(hello), 0,
               (const sockaddr*)&g_wan_relay_addr, sizeof(g_wan_relay_addr));
        g_wan_relay_last_hello = now;
    }
}

static void wan_relay_shutdown() {
    g_wan_relay_running.store(false);
    {
        std::lock_guard<std::mutex> lk(g_wan_peers_mutex);
        g_wan_peers.clear();
    }
    if (g_wan_relay_sock >= 0) {
        close(g_wan_relay_sock);
        g_wan_relay_sock = -1;
        fprintf(stderr, "[wan-relay] Disconnected\n");
    }
}

/// WAN relay RX thread: receives OSTP/RTP packets from relay and writes to ring buffer
static void wan_relay_rx_thread(soluna::pipeline::RingBuffer& ring, uint32_t channels) {
    if (g_wan_relay_sock < 0) return;

    // Set recv timeout 1s so we can check g_wan_relay_running
    struct timeval tv{1, 0};
    setsockopt(g_wan_relay_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[65536];
    std::vector<int32_t> audio_buf(16384);
    const size_t frame_size = channels * sizeof(int32_t);

    fprintf(stderr, "[wan-relay] RX thread started\n");

    while (g_wan_relay_running.load()) {
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(g_wan_relay_sock, buf, sizeof(buf), 0,
                             reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n <= 0) continue;

        // Handle PEER messages for P2P hole punching
        if (n >= 6 && memcmp(buf, "PEER:", 5) == 0) {
            std::string msg(reinterpret_cast<char*>(buf), n);
            // Strip trailing newline
            while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
                msg.pop_back();
            // Parse "PEER:ip:port"
            std::string addr_part = msg.substr(5);
            auto colon = addr_part.rfind(':');
            if (colon != std::string::npos) {
                std::string ip = addr_part.substr(0, colon);
                uint16_t port = static_cast<uint16_t>(std::stoi(addr_part.substr(colon + 1)));
                sockaddr_in peer{};
                peer.sin_family = AF_INET;
                peer.sin_port = htons(port);
                inet_pton(AF_INET, ip.c_str(), &peer.sin_addr);
                wan_relay_add_peer(peer);
                // Send PUNCH packet to open NAT
                const char punch[] = "PUNCH";
                sendto(g_wan_relay_sock, punch, 5, 0,
                       (const sockaddr*)&peer, sizeof(peer));
                fprintf(stderr, "[wan-relay] P2P peer discovered: %s:%u\n", ip.c_str(), port);
            }
            continue;
        }

        // Skip non-OSTP/RTP packets
        if (n < 12 || (buf[0] & 0xC0) != 0x80) continue;

        // Parse OSTP packet
        const soluna::transport::RtpHeader* rtp = reinterpret_cast<const soluna::transport::RtpHeader*>(buf);
        bool is_ostp = (rtp->pt == 96); // OSTP payload type

        if (is_ostp) {
            // OSTP: 12-byte RTP header + 8-byte OSTP extension
            const size_t hdr_size = sizeof(soluna::transport::RtpHeader) + 8;
            if (static_cast<size_t>(n) <= hdr_size) continue;
            const int32_t* samples = reinterpret_cast<const int32_t*>(buf + hdr_size);
            size_t payload_bytes = static_cast<size_t>(n) - hdr_size;
            size_t frames = payload_bytes / frame_size;
            if (frames > 0 && frames <= audio_buf.size() / channels) {
                std::memcpy(audio_buf.data(), samples, frames * frame_size);
                ring.write(audio_buf.data(), frames);
            }
        }

        // Send HELLO heartbeat every 5 seconds (shared with TX path)
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - g_wan_relay_last_hello).count();
        if (elapsed >= 5) {
            const char hello[] = "HELLO\n";
            sendto(g_wan_relay_sock, hello, strlen(hello), 0,
                   (const sockaddr*)&g_wan_relay_addr, sizeof(g_wan_relay_addr));
            g_wan_relay_last_hello = now;
        }
    }
    fprintf(stderr, "[wan-relay] RX thread stopped\n");
}

// ── Audio repair (declicker + crossfade) ─────────────────────────────────────
static std::atomic<uint64_t> g_repair_clicks{0};    // total clicks repaired
static std::atomic<uint64_t> g_repair_fades{0};     // total crossfades applied
static std::atomic<bool>     g_repair_enabled{false}; // enable/disable repair (OFF by default — prevents false positive transient destruction)

// ── Noise tuning parameters (adjustable via WS) ─────────────────────────────
static std::atomic<float>    g_noise_sigma{6.0f};       // click detection: N× RMS
static std::atomic<float>    g_noise_floor{0.005f};     // deglitch min threshold
static std::atomic<float>    g_crossfade_thresh{0.02f}; // discontinuity threshold
static std::atomic<uint32_t> g_crossfade_frames{16};    // fade length in frames
static std::atomic<uint32_t> g_tune_step_up{2};         // ms to add on noise
static std::atomic<uint32_t> g_tune_step_down{1};       // ms to subtract on stable
static std::atomic<uint32_t> g_tune_stable_sec{5};      // seconds before decrease

// Deglitch: detect and repair click spikes (1-4 sample bursts) in interleaved audio.
// Returns number of repaired samples.
static uint32_t deglitch_buffer(float* buf, uint32_t frame_count, uint32_t channels) {
    if (frame_count < 5) return 0;
    uint32_t repairs = 0;
    uint32_t samples = frame_count * channels;

    for (uint32_t ch = 0; ch < channels; ch++) {
        // Per-channel RMS for adaptive threshold
        float sum_sq = 0.0f;
        for (uint32_t i = ch; i < samples; i += channels)
            sum_sq += buf[i] * buf[i];
        float rms = std::sqrt(sum_sq / frame_count);
        float sigma = g_noise_sigma.load(std::memory_order_relaxed);
        float floor = g_noise_floor.load(std::memory_order_relaxed);
        float threshold = std::fmax(floor, rms * sigma);

        for (uint32_t f = 1; f < frame_count - 1; f++) {
            uint32_t idx  = f * channels + ch;
            uint32_t prev = (f - 1) * channels + ch;
            float d_in = std::fabs(buf[idx] - buf[prev]);

            if (d_in > threshold) {
                // Scan ahead to find end of glitch (up to 4 samples)
                uint32_t glitch_end = f + 1;
                for (; glitch_end < f + 5 && glitch_end < frame_count - 1; glitch_end++) {
                    uint32_t next_idx = glitch_end * channels + ch;
                    uint32_t after    = (glitch_end + 1) * channels + ch;
                    float d_out = std::fabs(buf[after] - buf[next_idx]);
                    if (d_out < threshold * 0.3f) break;
                }
                if (glitch_end < frame_count - 1) {
                    // Interpolate across the glitch region
                    float start_val = buf[prev];
                    float end_val   = buf[(glitch_end + 1) * channels + ch];
                    uint32_t span = glitch_end - f + 1;
                    for (uint32_t g = 0; g < span && f + g < frame_count; g++) {
                        float t = (float)(g + 1) / (float)(span + 1);
                        buf[(f + g) * channels + ch] = start_val * (1.0f - t) + end_val * t;
                        repairs++;
                    }
                    f = glitch_end; // skip past repaired region
                }
            }
        }
    }
    return repairs;
}

// Crossfade: smooth discontinuity at the start of a buffer using the
// last sample from the previous callback. Prevents clicks at buffer boundaries.
static void crossfade_boundary(float* buf, uint32_t frame_count, uint32_t channels,
                               float* prev_last_samples, bool* had_audio) {
    // Use up to 1/4 of buffer for crossfade (adaptive), minimum from config
    uint32_t fade_frames = g_crossfade_frames.load(std::memory_order_relaxed);
    uint32_t max_fade = frame_count / 4;
    if (max_fade > fade_frames) fade_frames = max_fade;
    if (fade_frames < 4) fade_frames = 4;
    if (frame_count < fade_frames) return;
    float cf_thresh = g_crossfade_thresh.load(std::memory_order_relaxed);

    for (uint32_t ch = 0; ch < channels; ch++) {
        float prev = prev_last_samples[ch];
        float first = buf[ch];
        float diff = std::fabs(first - prev);

        // Only crossfade if there's a significant discontinuity
        if (*had_audio && diff > cf_thresh) {
            for (uint32_t f = 0; f < fade_frames; f++) {
                // Smooth cosine-shaped crossfade (less audible than linear)
                float t = 0.5f * (1.0f - std::cos(3.14159265f * (float)(f + 1) / (float)(fade_frames + 1)));
                uint32_t idx = f * channels + ch;
                buf[idx] = prev * (1.0f - t) + buf[idx] * t;
            }
            g_repair_fades.fetch_add(1, std::memory_order_relaxed);
        }

        // Store last sample for next callback
        prev_last_samples[ch] = buf[(frame_count - 1) * channels + ch];
    }
    *had_audio = true;
}

// ── Latency measurement (frames in each buffer stage) ────────────────────────
static std::atomic<uint32_t> g_lat_shm_frames{0};     // SHM available (Plugin→Daemon)
static std::atomic<uint32_t> g_lat_tx_ring_frames{0};  // TX ring buffer fill
static std::atomic<uint32_t> g_lat_spk_ring_frames{0}; // Speaker ring buffer fill
static std::atomic<uint32_t> g_lat_mon_ring_frames{0}; // Monitor ring buffer fill
static std::atomic<uint32_t> g_lat_rx_ring_frames{0};  // RX ring buffer fill

// ── Persistent config (~/.config/solunad/config.json) ────────────────────────

static std::string persist_config_path() {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/.config/solunad/config.json";
}

static void persist_config_save() {
    std::string path = persist_config_path();
    // Ensure directory exists
    std::string dir = path.substr(0, path.rfind('/'));
    std::string mkdir_cmd = "mkdir -p '" + dir + "'";
    ::system(mkdir_cmd.c_str());

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"speaker_delay_ms\":%u,"
        "\"monitor_volume\":%.3f,"
        "\"monitor_muted\":%s,"
        "\"monitor_buffer_ms\":%u,"
        "\"rx_delay_ms\":%u,"
        "\"buf_target_ms\":%u,"
        "\"wifi_dup_send\":%s,"
        "\"wifi_fec\":%s,"
        "\"wifi_nack\":%s,"
        "\"wifi_wsola_plc\":%s,"
        "\"wifi_adaptive_jitter\":%s,"
        "\"wifi_dedup\":%s,"
        "\"stream_mode\":\"%s\"}\n",
        g_speaker_delay_ms.load(),
        (double)g_mon_volume.load(),
        g_mon_muted.load() ? "true" : "false",
        g_mon_target_ms.load(),
        g_rx_delay_ms.load(),
        g_buf_target_ms.load(),
        g_wifi_dup_send.load() ? "true" : "false",
        g_wifi_fec.load() ? "true" : "false",
        g_wifi_nack.load() ? "true" : "false",
        g_wifi_wsola_plc.load() ? "true" : "false",
        g_wifi_adaptive_jitter.load() ? "true" : "false",
        g_wifi_dedup.load() ? "true" : "false",
        (g_stream_mode.load() == soluna::StreamMode::Jam) ? "jam" : "sync");

    std::ofstream f(path);
    if (f.is_open()) f << buf;
}

static void persist_config_load() {
    std::ifstream f(persist_config_path());
    if (!f.is_open()) return;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string json = ss.str();

    // Parse simple key:value pairs without external JSON library
    auto get_uint = [&](const char* key, uint32_t def) -> uint32_t {
        std::string k = std::string("\"") + key + "\":";
        auto pos = json.find(k);
        if (pos == std::string::npos) return def;
        try { return (uint32_t)std::stoul(json.substr(pos + k.size())); } catch (...) { return def; }
    };
    auto get_float = [&](const char* key, float def) -> float {
        std::string k = std::string("\"") + key + "\":";
        auto pos = json.find(k);
        if (pos == std::string::npos) return def;
        try { return std::stof(json.substr(pos + k.size())); } catch (...) { return def; }
    };
    auto get_bool = [&](const char* key, bool def) -> bool {
        std::string k = std::string("\"") + key + "\":";
        auto pos = json.find(k);
        if (pos == std::string::npos) return def;
        auto vpos = pos + k.size();
        return json.substr(vpos, 4) == "true";
    };

    g_speaker_delay_ms.store(std::min(2000u, get_uint("speaker_delay_ms", 40)));
    g_mon_volume.store(std::max(0.0f, std::min(1.0f, get_float("monitor_volume", 1.0f))));
    g_mon_muted.store(get_bool("monitor_muted", false));
    g_mon_target_ms.store(std::max(5u, std::min(2000u, get_uint("monitor_buffer_ms", 20))));
    g_rx_delay_ms.store(std::min(2000u, get_uint("rx_delay_ms", 0)));

    // WiFi feature toggles
    g_wifi_dup_send.store(get_bool("wifi_dup_send", true));
    g_wifi_fec.store(get_bool("wifi_fec", true));
    g_wifi_nack.store(get_bool("wifi_nack", true));
    g_wifi_wsola_plc.store(get_bool("wifi_wsola_plc", true));
    g_wifi_adaptive_jitter.store(get_bool("wifi_adaptive_jitter", true));
    g_wifi_dedup.store(get_bool("wifi_dedup", true));

    // buf_target_ms: only load if saved (otherwise use profile default)
    uint32_t saved_buf = get_uint("buf_target_ms", 0);
    if (saved_buf > 0) g_buf_target_ms.store(std::min(2000u, saved_buf));

    // Stream mode
    auto get_str = [&](const char* key, const std::string& def) -> std::string {
        std::string k = std::string("\"") + key + "\":\"";
        auto pos = json.find(k);
        if (pos == std::string::npos) return def;
        pos += k.size();
        auto end = json.find('"', pos);
        if (end == std::string::npos) return def;
        return json.substr(pos, end - pos);
    };
    std::string saved_mode = get_str("stream_mode", "sync");
    if (saved_mode == "jam") {
        g_stream_mode.store(soluna::StreamMode::Jam);
    } else {
        g_stream_mode.store(soluna::StreamMode::Sync);
    }

    fprintf(stderr, "[config] Loaded from %s\n", persist_config_path().c_str());
}

// ── Tunnel (cloudflared / ngrok) ──────────────────────────────────────────────
static std::mutex  g_tunnel_mutex;
static std::string g_tunnel_url;   // set by tunnel_thread_fn when URL is known

static void tunnel_thread_fn() {
    FILE* pipe = popen("cloudflared tunnel --url http://localhost:8400 2>&1", "r");
    if (!pipe) pipe = popen("ngrok http 8400 --log=stdout 2>&1", "r");
    if (!pipe) { fprintf(stderr, "[tunnel] Neither cloudflared nor ngrok found\n"); return; }
    char line[512];
    while (fgets(line, sizeof(line), pipe)) {
        std::string s(line);
        for (const char* pat : {"https://", "http://"}) {
            auto pos = s.find(pat);
            if (pos == std::string::npos) continue;
            auto end = s.find_first_of(" \t\n\r|", pos + 8);
            std::string url = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            if (url.size() > 12 && url.find("localhost") == std::string::npos) {
                std::lock_guard<std::mutex> lk(g_tunnel_mutex);
                if (g_tunnel_url != url) {
                    g_tunnel_url = url;
                    fprintf(stderr, "\n[tunnel] Public URL: %s\n", url.c_str());
                    fprintf(stderr, "[tunnel] Share this URL with your iPhone\n\n");
                }
                break;
            }
        }
    }
    pclose(pipe);
}

static void signal_handler(int) {
    g_running.store(false);
}

// ── Multi-track recording helpers ────────────────────────────────────────────

static std::string recording_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    char ts[32];
    snprintf(ts, sizeof(ts), "%04d%02d%02d_%02d%02d%02d",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return ts;
}

static bool recording_start(const std::string& dir) {
    std::lock_guard<std::mutex> lk(g_rec_mutex);
    if (g_rec_active.load()) return false;

    std::string ts = recording_timestamp();
    std::string tx_path  = dir + "/tx_"      + ts + ".wav";
    std::string mon_path = dir + "/monitor_" + ts + ".wav";

    bool any = false;
    if (g_rec_tx.open(tx_path, g_cfg_sample_rate, g_cfg_channels, 16)) {
        fprintf(stderr, "[rec] TX recording: %s\n", tx_path.c_str());
        any = true;
    }
    if (g_rec_monitor.open(mon_path, g_cfg_sample_rate, g_cfg_channels, 16)) {
        fprintf(stderr, "[rec] Monitor recording: %s\n", mon_path.c_str());
        any = true;
    }
    if (any) g_rec_active.store(true);
    return any;
}

static void recording_stop() {
    std::lock_guard<std::mutex> lk(g_rec_mutex);
    if (!g_rec_active.load()) return;
    if (g_rec_tx.is_open()) {
        fprintf(stderr, "[rec] TX done: %llu frames → %s\n",
                (unsigned long long)g_rec_tx.frames_written(),
                g_rec_tx.path().c_str());
        g_rec_tx.close();
    }
    if (g_rec_monitor.is_open()) {
        fprintf(stderr, "[rec] Monitor done: %llu frames → %s\n",
                (unsigned long long)g_rec_monitor.frames_written(),
                g_rec_monitor.path().c_str());
        g_rec_monitor.close();
    }
    g_rec_active.store(false);
}

static std::string ws_handle(const std::string& msg) {
    int id = 0;
    auto p = msg.find("\"id\":");
    if (p != std::string::npos) try { id = std::stoi(msg.substr(p + 5)); } catch (...) {}

    std::string cmd;
    p = msg.find("\"command\":\"");
    if (p != std::string::npos) {
        auto s = p + 11, e = msg.find('"', s);
        if (e != std::string::npos) cmd = msg.substr(s, e - s);
    }

    char buf[1024];
    if (cmd == "rx.stats" || cmd == "system.stats") {
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"success\":true,\"data\":"
            "\"{\\\"packets\\\":%llu,\\\"errors\\\":%llu,"
            "\\\"buf_fill\\\":%zu,\\\"buf_cap\\\":%zu,"
            "\\\"volume\\\":%.3f,\\\"muted\\\":%s,"
            "\\\"buf_target_ms\\\":%u,"
            "\\\"crc_errors\\\":%llu,\\\"plc_frames\\\":%llu,"
            "\\\"lost_packets\\\":%llu}\"}",
            id,
            (unsigned long long)g_packets.load(),
            (unsigned long long)g_seq_errors.load(),
            g_buf_fill.load(), g_buf_cap.load(),
            (double)g_rx_volume.load(),
            g_rx_muted.load() ? "true" : "false",
            g_buf_target_ms.load(),
            (unsigned long long)g_crc_errors.load(),
            (unsigned long long)g_plc_frames.load(),
            (unsigned long long)g_lost_packets.load());
    } else if (cmd == "rx.set_volume") {
        p = msg.find("\"volume\":");
        if (p != std::string::npos) {
            try {
                float v = std::stof(msg.substr(p + 9));
                g_rx_volume.store(std::fmax(0.0f, std::fmin(1.0f, v)));
            } catch (...) {}
        }
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "rx.set_mute") {
        p = msg.find("\"muted\":");
        if (p != std::string::npos)
            g_rx_muted.store(msg.substr(p + 8, 4) == "true");
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "rx.set_buffer") {
        p = msg.find("\"ms\":");
        if (p != std::string::npos) {
            try {
                uint32_t ms = static_cast<uint32_t>(std::stoul(msg.substr(p + 5)));
                g_buf_target_ms.store(std::max(1u, std::min(2000u, ms)));
                // Disable adaptive jitter when user manually sets buffer
                // (adaptive would overwrite the manual value every packet)
                g_wifi_adaptive_jitter.store(false);
            } catch (...) {}
        }
        persist_config_save();
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    // ── Monitor commands (TX-mode only) ────────────────────────────────────
    } else if (cmd == "monitor.stats") {
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"success\":true,\"data\":"
            "\"{\\\"supported\\\":%s,\\\"running\\\":%s,"
            "\\\"volume\\\":%.3f,\\\"muted\\\":%s,"
            "\\\"packets\\\":%llu,\\\"delay_ms\\\":%u,"
            "\\\"rx_delay_ms\\\":%u,"
            "\\\"underruns\\\":%llu}\"}",
            id,
            g_mon_supported.load() ? "true" : "false",
            g_mon_active.load()    ? "true" : "false",
            (double)g_mon_volume.load(),
            g_mon_muted.load() ? "true" : "false",
            (unsigned long long)g_mon_packets.load(),
            g_speaker_delay_ms.load(),
            g_rx_delay_ms.load(),
            (unsigned long long)g_mon_underruns.load());
    } else if (cmd == "monitor.start") {
        std::string dev;
        p = msg.find("\"device\":\"");
        if (p != std::string::npos) {
            auto s = p + 10, e = msg.find('"', s);
            if (e != std::string::npos) dev = msg.substr(s, e - s);
        }
        {
            std::lock_guard<std::mutex> lk(g_mon_mutex);
            g_mon_start_req = { true, dev };
        }
        g_mon_stop_req.store(false);
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "monitor.stop") {
        g_mon_stop_req.store(true);
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "monitor.set_volume") {
        p = msg.find("\"volume\":");
        if (p != std::string::npos)
            try { g_mon_volume.store(std::fmax(0.0f, std::fmin(1.0f, std::stof(msg.substr(p + 9))))); } catch (...) {}
        persist_config_save();
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "monitor.set_mute") {
        p = msg.find("\"muted\":");
        if (p != std::string::npos)
            g_mon_muted.store(msg.substr(p + 8, 4) == "true");
        persist_config_save();
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "monitor.set_buffer") {
        p = msg.find("\"ms\":");
        if (p != std::string::npos)
            try { g_mon_target_ms.store(std::max(5u, std::min(2000u, (uint32_t)std::stoul(msg.substr(p + 5))))); } catch (...) {}
        persist_config_save();
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "time.ping") {
        // RTT measurement — client records t1 before sending, measures t4-t1 on response
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"{\\\"pong\\\":true}\"}", id);
    } else if (cmd == "monitor.set_delay") {
        p = msg.find("\"ms\":");
        if (p != std::string::npos)
            try { g_speaker_delay_ms.store(std::min(2000u, (uint32_t)std::stoul(msg.substr(p + 5)))); } catch (...) {}
        persist_config_save();
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "rx.set_global_delay") {
        p = msg.find("\"ms\":");
        if (p != std::string::npos)
            try { g_rx_delay_ms.store(std::min(2000u, (uint32_t)std::stoul(msg.substr(p + 5)))); } catch (...) {}
        persist_config_save();
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "monitor.list_devices") {
        using namespace soluna::pal;
        auto devs = AudioDevice::enumerate();
        char devbuf[2048];
        int pos = snprintf(devbuf, sizeof(devbuf), "[");
        bool first = true;
        for (auto& d : devs) {
            if (d.max_output_channels == 0) continue;
            if (!first) devbuf[pos++] = ',';
            first = false;
            pos += snprintf(devbuf + pos, sizeof(devbuf) - pos, "\\\"%s\\\"", d.name.c_str());
        }
        pos += snprintf(devbuf + pos, sizeof(devbuf) - pos, "]");
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"%s\"}", id, devbuf);
    // ── Browser audio streaming ─────────────────────────────────────────────
    } else if (cmd == "audio.subscribe") {
        g_audio_streaming.store(true);
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"success\":true,\"data\":"
            "\"{\\\"sample_rate\\\":%u,\\\"channels\\\":%u}\"}",
            id, g_cfg_sample_rate, g_cfg_channels);
    } else if (cmd == "audio.unsubscribe") {
        g_audio_streaming.store(false);
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "system.info") {
        std::string turl;
        { std::lock_guard<std::mutex> lk(g_tunnel_mutex); turl = g_tunnel_url; }
        // Build JSON manually to avoid escaping nightmares
        char info[512];
        snprintf(info, sizeof(info),
            "{\"tunnel_url\":\"%s\","
            "\"multicast\":\"%s\","
            "\"port\":%u,"
            "\"channels\":%u,"
            "\"sample_rate\":%u}",
            turl.c_str(), g_cfg_multicast,
            g_cfg_port, g_cfg_channels, g_cfg_sample_rate);
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"%s\"}", id, info);
    // ── Mac system volume control ────────────────────────────────────────────
#ifdef __APPLE__
    } else if (cmd == "system.volume") {
        float vol = get_system_volume();
        bool muted = get_system_mute();
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"success\":true,\"data\":"
            "\"{\\\"volume\\\":%.3f,\\\"muted\\\":%s}\"}",
            id, (double)vol, muted ? "true" : "false");
    } else if (cmd == "system.set_volume") {
        p = msg.find("\"volume\":");
        if (p != std::string::npos) {
            try {
                float v = std::stof(msg.substr(p + 9));
                set_system_volume(v);
            } catch (...) {}
        }
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "system.set_mute") {
        p = msg.find("\"muted\":");
        if (p != std::string::npos)
            set_system_mute(msg.substr(p + 8, 4) == "true");
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    // ── Input passthrough (Babyface → SHM) ──────────────────────────────────
    } else if (cmd == "input.start") {
        std::string dev;
        uint32_t channel = 2; // default ch3 (0-indexed)
        p = msg.find("\"device\":\"");
        if (p != std::string::npos) {
            auto s = p + 10, e = msg.find('"', s);
            if (e != std::string::npos) dev = msg.substr(s, e - s);
        }
        p = msg.find("\"channel\":");
        if (p != std::string::npos)
            try { channel = (uint32_t)std::stoul(msg.substr(p + 10)); } catch (...) {}
        g_input_channel.store(channel);
        {
            std::lock_guard<std::mutex> lk(g_input_mutex);
            g_input_start_req = { true, dev, channel };
        }
        g_input_stop_req.store(false);
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "input.stop") {
        g_input_stop_req.store(true);
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "input.set_volume") {
        p = msg.find("\"volume\":");
        if (p != std::string::npos)
            try { g_input_volume.store(std::fmax(0.0f, std::fmin(20.0f, std::stof(msg.substr(p + 9))))); } catch (...) {}
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "input.set_channel") {
        p = msg.find("\"channel\":");
        if (p != std::string::npos)
            try { g_input_channel.store((uint32_t)std::stoul(msg.substr(p + 10))); } catch (...) {}
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "input.stats") {
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"success\":true,\"data\":"
            "\"{\\\"active\\\":%s,\\\"volume\\\":%.1f,\\\"channel\\\":%u}\"}",
            id, g_input_active.load() ? "true" : "false",
            (double)g_input_volume.load(), g_input_channel.load());
    } else if (cmd == "input.list_devices") {
        using namespace soluna::pal;
        auto devs = AudioDevice::enumerate();
        char devbuf[2048];
        int pos = snprintf(devbuf, sizeof(devbuf), "[");
        bool first = true;
        for (auto& d : devs) {
            if (d.max_input_channels == 0) continue;
            if (!first) devbuf[pos++] = ',';
            first = false;
            pos += snprintf(devbuf + pos, sizeof(devbuf) - pos,
                "{\\\"name\\\":\\\"%s\\\",\\\"channels\\\":%u}",
                d.name.c_str(), d.max_input_channels);
        }
        pos += snprintf(devbuf + pos, sizeof(devbuf) - pos, "]");
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"%s\"}", id, devbuf);
#endif
    // ── Auto-tune commands ──────────────────────────────────────────────────
    } else if (cmd == "tune.start") {
        g_tune_stop_req.store(false);
        g_tune_start_req.store(true);
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "tune.stop") {
        g_tune_stop_req.store(true);
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "tune.status") {
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"success\":true,\"data\":"
            "\"{\\\"active\\\":%s,\\\"rms_db\\\":%.1f,"
            "\\\"clicks\\\":%u,\\\"dropouts\\\":%u,"
            "\\\"adjustments\\\":%u,"
            "\\\"buf_target_ms\\\":%u,\\\"mon_target_ms\\\":%u,"
            "\\\"repair_enabled\\\":%s,"
            "\\\"repair_clicks\\\":%llu,\\\"repair_fades\\\":%llu,"
            "\\\"crc_errors\\\":%llu,\\\"plc_frames\\\":%llu,"
            "\\\"lost_packets\\\":%llu,"
            "\\\"wifi_dup_send\\\":%s,\\\"wifi_fec\\\":%s,"
            "\\\"wifi_nack\\\":%s,\\\"wifi_wsola_plc\\\":%s,"
            "\\\"wifi_adaptive_jitter\\\":%s,\\\"wifi_dedup\\\":%s}\"}",
            id,
            g_tune_active.load() ? "true" : "false",
            (double)g_tune_rms_db.load(),
            g_tune_clicks.load(),
            g_tune_dropouts.load(),
            g_tune_adjustments.load(),
            g_buf_target_ms.load(),
            g_mon_target_ms.load(),
            g_repair_enabled.load() ? "true" : "false",
            (unsigned long long)g_repair_clicks.load(),
            (unsigned long long)g_repair_fades.load(),
            (unsigned long long)g_crc_errors.load(),
            (unsigned long long)g_plc_frames.load(),
            (unsigned long long)g_lost_packets.load(),
            g_wifi_dup_send.load() ? "true" : "false",
            g_wifi_fec.load() ? "true" : "false",
            g_wifi_nack.load() ? "true" : "false",
            g_wifi_wsola_plc.load() ? "true" : "false",
            g_wifi_adaptive_jitter.load() ? "true" : "false",
            g_wifi_dedup.load() ? "true" : "false");
    } else if (cmd == "repair.enable") {
        g_repair_enabled.store(true);
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "repair.disable") {
        g_repair_enabled.store(false);
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "repair.reset") {
        g_repair_clicks.store(0);
        g_repair_fades.store(0);
        g_tune_clicks.store(0);
        g_tune_dropouts.store(0);
        g_tune_adjustments.store(0);
        g_crc_errors.store(0);
        g_plc_frames.store(0);
        g_lost_packets.store(0);
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "plc.stats") {
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"success\":true,\"data\":"
            "\"{\\\"crc_errors\\\":%llu,\\\"plc_frames\\\":%llu,"
            "\\\"lost_packets\\\":%llu}\"}",
            id,
            (unsigned long long)g_crc_errors.load(),
            (unsigned long long)g_plc_frames.load(),
            (unsigned long long)g_lost_packets.load());
    } else if (cmd == "plc.reset") {
        g_crc_errors.store(0);
        g_plc_frames.store(0);
        g_lost_packets.store(0);
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "noise.get") {
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"success\":true,\"data\":"
            "\"{\\\"sigma\\\":%.2f,"
            "\\\"floor\\\":%.4f,"
            "\\\"crossfade_thresh\\\":%.4f,"
            "\\\"crossfade_frames\\\":%u,"
            "\\\"step_up\\\":%u,"
            "\\\"step_down\\\":%u,"
            "\\\"stable_sec\\\":%u}\"}",
            id,
            (double)g_noise_sigma.load(),
            (double)g_noise_floor.load(),
            (double)g_crossfade_thresh.load(),
            g_crossfade_frames.load(),
            g_tune_step_up.load(),
            g_tune_step_down.load(),
            g_tune_stable_sec.load());
    } else if (cmd == "noise.set") {
        // Parse each optional param: sigma, floor, crossfade_thresh, crossfade_frames,
        // step_up, step_down, stable_sec
        auto get_f = [&](const char* key) -> float {
            std::string k = std::string("\"") + key + "\":";
            auto pos = msg.find(k);
            if (pos == std::string::npos) return -1.0f;
            try { return std::stof(msg.substr(pos + k.size())); } catch (...) { return -1.0f; }
        };
        auto get_u = [&](const char* key) -> int {
            std::string k = std::string("\"") + key + "\":";
            auto pos = msg.find(k);
            if (pos == std::string::npos) return -1;
            try { return std::stoi(msg.substr(pos + k.size())); } catch (...) { return -1; }
        };
        float v;
        int iv;
        v = get_f("sigma");       if (v >= 1.0f && v <= 20.0f) g_noise_sigma.store(v);
        v = get_f("floor");       if (v >= 0.0f && v <= 0.5f)  g_noise_floor.store(v);
        v = get_f("crossfade_thresh"); if (v >= 0.0f && v <= 0.5f) g_crossfade_thresh.store(v);
        iv = get_u("crossfade_frames"); if (iv >= 2 && iv <= 256) g_crossfade_frames.store((uint32_t)iv);
        iv = get_u("step_up");    if (iv >= 1 && iv <= 20) g_tune_step_up.store((uint32_t)iv);
        iv = get_u("step_down");  if (iv >= 1 && iv <= 20) g_tune_step_down.store((uint32_t)iv);
        iv = get_u("stable_sec"); if (iv >= 1 && iv <= 60) g_tune_stable_sec.store((uint32_t)iv);
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "wifi.set") {
        // Toggle WiFi features: {"command":"wifi.set","params":{"dup_send":true,"fec":false,...}}
        auto get_b = [&](const char* key) -> int {
            std::string k = std::string("\"") + key + "\":";
            auto pos = msg.find(k);
            if (pos == std::string::npos) return -1;
            auto val = msg.substr(pos + k.size(), 5);
            if (val.find("true") == 0) return 1;
            if (val.find("false") == 0) return 0;
            return -1;
        };
        int v;
        v = get_b("dup_send");        if (v >= 0) g_wifi_dup_send.store(v == 1);
        v = get_b("fec");             if (v >= 0) g_wifi_fec.store(v == 1);
        v = get_b("nack");            if (v >= 0) g_wifi_nack.store(v == 1);
        v = get_b("wsola_plc");       if (v >= 0) g_wifi_wsola_plc.store(v == 1);
        v = get_b("adaptive_jitter"); if (v >= 0) g_wifi_adaptive_jitter.store(v == 1);
        v = get_b("dedup");           if (v >= 0) g_wifi_dedup.store(v == 1);
        persist_config_save();
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "relay.stats") {
        std::lock_guard<std::mutex> lock(g_relay_mutex);
        auto now = std::chrono::steady_clock::now();
        std::string peers_json = "[";
        for (size_t i = 0; i < g_relay_peers.size(); i++) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &g_relay_peers[i].addr.sin_addr, ip, sizeof(ip));
            auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - g_relay_peers[i].last_seen).count();
            if (i > 0) peers_json += ",";
            peers_json += "{\"ip\":\"" + std::string(ip) + "\",\"port\":"
                + std::to_string(ntohs(g_relay_peers[i].addr.sin_port))
                + ",\"age_ms\":" + std::to_string(age) + "}";
        }
        peers_json += "]";
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"success\":true,\"data\":\"{\\\"enabled\\\":%s,"
            "\\\"port\\\":%d,\\\"peer_count\\\":%zu,\\\"peers\\\":%s}\"}",
            id, (g_relay_sock >= 0 ? "true" : "false"),
            (g_relay_sock >= 0 ? kRelayPort : 0),
            g_relay_peers.size(), peers_json.c_str());
    } else if (cmd == "dsp.list") {
        // List all DSP plugins with their parameters and bypass state
        std::string json = "[";
        if (g_dsp_chain_ptr) {
            auto names = g_dsp_chain_ptr->get_plugin_names();
            for (size_t i = 0; i < names.size(); i++) {
                auto* plugin = g_dsp_chain_ptr->get_plugin(names[i]);
                bool bypassed = g_dsp_chain_ptr->is_bypassed(names[i]);
                if (i > 0) json += ",";
                json += "{\\\"name\\\":\\\"" + names[i] + "\\\","
                      + "\\\"bypassed\\\":" + (bypassed ? "true" : "false") + ","
                      + "\\\"params\\\":[";
                if (plugin) {
                    for (size_t p = 0; p < plugin->param_count(); p++) {
                        if (p > 0) json += ",";
                        json += "{\\\"name\\\":\\\"";
                        json += plugin->param_name(p);
                        json += "\\\",\\\"value\\\":";
                        char vbuf[32];
                        snprintf(vbuf, sizeof(vbuf), "%.4f", plugin->param_value(p));
                        json += vbuf;
                        json += "}";
                    }
                }
                json += "]}";
            }
        }
        json += "]";
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"success\":true,\"data\":\"%s\"}", id, json.c_str());
    } else if (cmd == "dsp.set") {
        // Set DSP plugin parameter: {"command":"dsp.set","plugin":"EQ","param":"low_gain_db","value":3.0}
        auto get_str = [&](const char* key) -> std::string {
            std::string k = std::string("\"") + key + "\":\"";
            auto p = msg.find(k);
            if (p == std::string::npos) return "";
            p += k.size();
            auto e = msg.find('"', p);
            return (e != std::string::npos) ? msg.substr(p, e - p) : "";
        };
        auto get_num = [&](const char* key) -> float {
            std::string k = std::string("\"") + key + "\":";
            auto p = msg.find(k);
            if (p == std::string::npos) return 0.0f;
            return std::strtof(msg.c_str() + p + k.size(), nullptr);
        };
        std::string plugin_name = get_str("plugin");
        std::string param_name = get_str("param");
        float value = get_num("value");
        bool ok = false;
        if (g_dsp_chain_ptr && !plugin_name.empty() && !param_name.empty()) {
            auto* plugin = g_dsp_chain_ptr->get_plugin(plugin_name);
            if (plugin) {
                for (size_t p = 0; p < plugin->param_count(); p++) {
                    if (param_name == plugin->param_name(p)) {
                        plugin->set_param(p, value);
                        ok = true;
                        break;
                    }
                }
            }
        }
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"success\":%s,\"data\":\"\"}", id, ok ? "true" : "false");
    } else if (cmd == "dsp.bypass") {
        // Toggle bypass: {"command":"dsp.bypass","plugin":"Compressor","bypassed":false}
        auto get_str = [&](const char* key) -> std::string {
            std::string k = std::string("\"") + key + "\":\"";
            auto p = msg.find(k);
            if (p == std::string::npos) return "";
            p += k.size();
            auto e = msg.find('"', p);
            return (e != std::string::npos) ? msg.substr(p, e - p) : "";
        };
        std::string plugin_name = get_str("plugin");
        bool bypassed = (msg.find("\"bypassed\":true") != std::string::npos);
        bool ok = false;
        if (g_dsp_chain_ptr && !plugin_name.empty()) {
            ok = g_dsp_chain_ptr->set_bypass(plugin_name, bypassed);
        }
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"success\":%s,\"data\":\"\"}", id, ok ? "true" : "false");
    // ── Stream mode commands ──────────────────────────────────────────────
    } else if (cmd == "mode.get") {
        const char* mode = (g_stream_mode.load() == soluna::StreamMode::Jam) ? "jam" : "sync";
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"success\":true,\"data\":\"{\\\"mode\\\":\\\"%s\\\"}\"}", id, mode);
    } else if (cmd == "mode.set") {
        std::string mode;
        p = msg.find("\"mode\":\"");
        if (p != std::string::npos) {
            auto s = p + 8, e = msg.find('"', s);
            if (e != std::string::npos) mode = msg.substr(s, e - s);
        }
        if (mode == "sync" || mode == "jam") {
            soluna::StreamMode new_mode = (mode == "jam")
                ? soluna::StreamMode::Jam : soluna::StreamMode::Sync;
            g_stream_mode.store(new_mode);
            persist_config_save();
            snprintf(buf, sizeof(buf),
                "{\"id\":%d,\"success\":true,\"data\":\"{\\\"mode\\\":\\\"%s\\\"}\"}", id, mode.c_str());
            // Broadcast mode change to all connected clients
            if (g_ws_server_ptr) {
                char bcast[128];
                snprintf(bcast, sizeof(bcast),
                    "{\"event\":\"mode.changed\",\"data\":{\"mode\":\"%s\"}}", mode.c_str());
                g_ws_server_ptr->broadcast(std::string(bcast));
            }
        } else {
            snprintf(buf, sizeof(buf),
                "{\"id\":%d,\"success\":false,\"data\":\"invalid mode (expected sync or jam)\"}", id);
        }
    } else if (cmd == "latency") {
        // Calculate each stage latency in ms from frame counts
        float sr = (float)g_cfg_sample_rate;
        float shm_ms     = g_lat_shm_frames.load()     * 1000.0f / sr;
        float tx_ring_ms = g_lat_tx_ring_frames.load()  * 1000.0f / sr;
        float spk_ms     = g_lat_spk_ring_frames.load() * 1000.0f / sr;
        float mon_ms     = g_lat_mon_ring_frames.load() * 1000.0f / sr;
        float rx_ms      = g_lat_rx_ring_frames.load()  * 1000.0f / sr;
        float spk_delay  = (float)g_speaker_delay_ms.load();
        float total_local = shm_ms + spk_ms + spk_delay;
        float total_mon   = shm_ms + tx_ring_ms + mon_ms;
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"success\":true,\"data\":"
            "\"{\\\"shm_ms\\\":%.2f,"
            "\\\"tx_ring_ms\\\":%.2f,"
            "\\\"spk_ring_ms\\\":%.2f,"
            "\\\"spk_delay_ms\\\":%.0f,"
            "\\\"mon_ring_ms\\\":%.2f,"
            "\\\"rx_ring_ms\\\":%.2f,"
            "\\\"total_local_ms\\\":%.2f,"
            "\\\"total_monitor_ms\\\":%.2f,"
            "\\\"buf_target_ms\\\":%u,"
            "\\\"mon_target_ms\\\":%u}\"}",
            id, shm_ms, tx_ring_ms, spk_ms, spk_delay,
            mon_ms, rx_ms, total_local, total_mon,
            g_buf_target_ms.load(), g_mon_target_ms.load());
    // ── Multi-track recording commands ────────────────────────────────────
    } else if (cmd == "recording.start") {
        std::string dir;
        p = msg.find("\"dir\":\"");
        if (p != std::string::npos) {
            auto s = p + 7, e = msg.find('"', s);
            if (e != std::string::npos) dir = msg.substr(s, e - s);
        }
        if (dir.empty()) dir = g_record_dir;
        if (dir.empty()) {
            snprintf(buf, sizeof(buf),
                "{\"id\":%d,\"success\":false,\"data\":\"no record dir set\"}", id);
        } else {
            bool ok = recording_start(dir);
            snprintf(buf, sizeof(buf),
                "{\"id\":%d,\"success\":%s,\"data\":\"\"}", id, ok ? "true" : "false");
        }
    } else if (cmd == "recording.stop") {
        recording_stop();
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "recording.status") {
        bool active = g_rec_active.load();
        std::string tx_file, mon_file;
        uint64_t tx_frames = 0, mon_frames = 0;
        {
            std::lock_guard<std::mutex> lk(g_rec_mutex);
            if (g_rec_tx.is_open()) {
                tx_file = g_rec_tx.path();
                tx_frames = g_rec_tx.frames_written();
            }
            if (g_rec_monitor.is_open()) {
                mon_file = g_rec_monitor.path();
                mon_frames = g_rec_monitor.frames_written();
            }
        }
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"success\":true,\"data\":"
            "\"{\\\"active\\\":%s,"
            "\\\"tx_file\\\":\\\"%s\\\","
            "\\\"tx_frames\\\":%llu,"
            "\\\"monitor_file\\\":\\\"%s\\\","
            "\\\"monitor_frames\\\":%llu}\"}",
            id, active ? "true" : "false",
            tx_file.c_str(), (unsigned long long)tx_frames,
            mon_file.c_str(), (unsigned long long)mon_frames);
    // ── File Player commands ───────────────────────────────────────────────
    } else if (cmd == "player.status") {
        uint64_t pos_ms  = 0;
        uint64_t dur_ms  = g_player_duration_ms.load();
        bool     active  = g_player_active.load();
        bool     paused  = g_player_paused.load();
        if (active && g_player_ring_ptr && g_cfg_sample_rate > 0) {
            int64_t frames = g_player_frame_pos.load();
            pos_ms = (uint64_t)(frames * 1000 / g_cfg_sample_rate);
        }
        std::string fname;
        { std::lock_guard<std::mutex> lk(g_player_mutex); fname = g_player_file_name; }
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"success\":true,\"data\":"
            "\"{\\\"active\\\":%s,"
            "\\\"paused\\\":%s,"
            "\\\"pos_ms\\\":%llu,"
            "\\\"dur_ms\\\":%llu,"
            "\\\"file\\\":\\\"%s\\\"}\"}",
            id,
            active  ? "true" : "false",
            paused  ? "true" : "false",
            (unsigned long long)pos_ms,
            (unsigned long long)dur_ms,
            fname.c_str());
    } else if (cmd == "player.play") {
        bool ok = false;
        { std::lock_guard<std::mutex> lk(g_player_mutex); ok = !g_player_file_path.empty(); }
        if (ok && !g_player_active.load()) {
            g_player_active.store(true);
            g_player_paused.store(false);
            g_player_frame_pos.store(0);
            // Player thread is started by start_ws_server caller (run_rx/run_tx)
            // via the player_start_fn callback; if ring is set, start directly.
            if (g_player_ring_ptr) {
                if (g_player_thread.joinable()) g_player_thread.join();
                g_player_thread = std::thread([]() {
                    using namespace soluna::pipeline;
                    FileSource src;
                    std::string path, name;
                    {
                        std::lock_guard<std::mutex> lk(g_player_mutex);
                        path = g_player_file_path;
                        name = g_player_file_name;
                    }
                    if (!src.open(path, g_cfg_sample_rate, g_cfg_channels)) {
                        fprintf(stderr, "[player] Failed to open: %s\n", path.c_str());
                        g_player_active.store(false);
                        if (g_ws_server_ptr)
                            g_ws_server_ptr->broadcast("{\"event\":\"player.error\",\"msg\":\"Cannot open file\"}");
                        return;
                    }
                    g_player_duration_ms.store(src.duration_ms());

                    // Broadcast stream start event
                    int64_t start_ns = 0; // TODO: PTP integration
                    {
                        char ev[256];
                        snprintf(ev, sizeof(ev),
                            "{\"event\":\"player.stream_start\","
                            "\"name\":\"%s\","
                            "\"dur_ms\":%llu,"
                            "\"fmt\":\"%s\","
                            "\"ptp_start_ns\":%lld}",
                            name.c_str(),
                            (unsigned long long)src.duration_ms(),
                            src.format_name(),
                            (long long)start_ns);
                        if (g_ws_server_ptr) g_ws_server_ptr->broadcast(ev);
                    }

                    // Send the compressed file to all WS clients so they can switch later
                    {
                        std::ifstream ffile(path, std::ios::binary);
                        uint32_t fsize = 0;
                        {
                            std::lock_guard<std::mutex> lk(g_player_mutex);
                            fsize = g_player_file_size;
                        }
                        if (ffile && g_ws_server_ptr) {
                            // Announce
                            char ev[256];
                            snprintf(ev, sizeof(ev),
                                "{\"event\":\"player.file_start\",\"name\":\"%s\",\"size\":%u}",
                                name.c_str(), fsize);
                            g_ws_server_ptr->broadcast(ev);

                            // Send 32KB chunks as binary, prefixed with magic [0xFA,0xFB,hi,lo]
                            constexpr size_t kChunk = 32768;
                            std::vector<uint8_t> chunk(kChunk + 4);
                            chunk[0] = 0xFA; chunk[1] = 0xFB;
                            while (ffile && g_player_active.load()) {
                                ffile.read(reinterpret_cast<char*>(chunk.data() + 4), kChunk);
                                size_t n = (size_t)ffile.gcount();
                                if (n == 0) break;
                                chunk[2] = (uint8_t)((n >> 8) & 0xFF);
                                chunk[3] = (uint8_t)(n & 0xFF);
                                g_ws_server_ptr->broadcast_binary(chunk.data(), n + 4);
                                // Small throttle to avoid flooding WebSocket
                                std::this_thread::sleep_for(std::chrono::microseconds(200));
                            }
                            g_ws_server_ptr->broadcast("{\"event\":\"player.file_done\"}");
                        }
                    }

                    // Decode + stream PCM to ring buffer
                    const size_t kChunkFrames = 480; // 10ms @ 48kHz
                    std::vector<int32_t> pcm(kChunkFrames * g_cfg_channels);

                    while (g_player_active.load() && !src.is_eof()) {
                        if (g_player_paused.load()) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                            continue;
                        }
                        size_t got = src.read_frames(pcm.data(), kChunkFrames);
                        if (got == 0) break;

                        // Back-pressure: wait for ring space
                        if (g_player_ring_ptr) {
                            int spin = 0;
                            while (g_player_ring_ptr->available_write() < got
                                   && g_player_active.load() && spin++ < 2000) {
                                std::this_thread::sleep_for(std::chrono::microseconds(200));
                            }
                            g_player_ring_ptr->write(pcm.data(), got);
                        }

                        // Also stream to browser as S16LE (same as audio.subscribe)
                        if (g_audio_streaming.load() && g_ws_server_ptr) {
                            constexpr uint32_t kWsFrames = 960;
                            thread_local std::vector<int16_t> ws_acc;
                            thread_local uint32_t ws_acc_f = 0;
                            size_t prev = ws_acc.size();
                            ws_acc.resize(prev + got * g_cfg_channels);
                            for (size_t i = 0; i < got * g_cfg_channels; i++) {
                                float f = pcm[i] * (1.0f / 8388607.0f);
                                ws_acc[prev + i] = (int16_t)(f * 32767.0f);
                            }
                            ws_acc_f += (uint32_t)got;
                            if (ws_acc_f >= kWsFrames) {
                                g_ws_server_ptr->broadcast_binary(
                                    reinterpret_cast<const uint8_t*>(ws_acc.data()),
                                    ws_acc.size() * sizeof(int16_t));
                                ws_acc.clear();
                                ws_acc_f = 0;
                            }
                        }

                        g_player_frame_pos.fetch_add((int64_t)got);
                    }

                    g_player_active.store(false);
                    if (g_ws_server_ptr)
                        g_ws_server_ptr->broadcast("{\"event\":\"player.done\"}");
                    fprintf(stderr, "[player] Playback finished: %s\n", name.c_str());
                });
            }
            snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
        } else {
            snprintf(buf, sizeof(buf),
                "{\"id\":%d,\"success\":false,\"data\":\"%s\"}",
                id, g_player_active.load() ? "already playing" : "no file loaded");
        }
    } else if (cmd == "player.pause") {
        g_player_paused.store(!g_player_paused.load());
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"success\":true,\"data\":\"{\\\"paused\\\":%s}\"}",
            id, g_player_paused.load() ? "true" : "false");
    } else if (cmd == "player.stop") {
        g_player_active.store(false);
        g_player_paused.store(false);
        g_player_frame_pos.store(0);
        if (g_ws_server_ptr) g_ws_server_ptr->broadcast("{\"event\":\"player.stopped\"}");
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "player.seek") {
        // {"command":"player.seek","pos_ms":12345}
        uint64_t pos_ms = 0;
        auto ppos = msg.find("\"pos_ms\":");
        if (ppos != std::string::npos) {
            try { pos_ms = std::stoull(msg.substr(ppos + 9)); } catch (...) {}
        }
        // Seek: update frame_pos (approximate; actual seek happens in player thread)
        if (g_cfg_sample_rate > 0)
            g_player_frame_pos.store((int64_t)(pos_ms * g_cfg_sample_rate / 1000));
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "player.file_ready") {
        // Browser has received the complete file — send switch command
        // Current position + 2s switch delay
        uint64_t pos_ms = 0;
        if (g_cfg_sample_rate > 0)
            pos_ms = (uint64_t)(g_player_frame_pos.load() * 1000 / g_cfg_sample_rate);
        const uint32_t switch_delay_ms = 2000;
        uint64_t switch_pos_ms = pos_ms + switch_delay_ms;
        if (g_ws_server_ptr) {
            char ev[256];
            snprintf(ev, sizeof(ev),
                "{\"event\":\"player.switch\","
                "\"switch_delay_ms\":%u,"
                "\"file_pos_ms\":%llu}",
                switch_delay_ms,
                (unsigned long long)switch_pos_ms);
            g_ws_server_ptr->broadcast(ev);
        }
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);

    // ── Channel control (WAN relay group) ────────────────────────────────────
    } else if (cmd == "channel.get") {
        std::lock_guard<std::mutex> lk(g_wan_cfg_mutex);
        char json[512];
        snprintf(json, sizeof(json),
            "{\\\"channel\\\":\\\"%s\\\",\\\"host\\\":\\\"%s\\\","
            "\\\"port\\\":%u,\\\"connected\\\":%s}",
            g_wan_cfg_group.c_str(), g_wan_cfg_host.c_str(),
            g_wan_cfg_port, g_wan_relay_running.load() ? "true" : "false");
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"%s\"}", id, json);

    } else if (cmd == "channel.set") {
        // {"command":"channel.set","channel":"ambient-tokyo"}
        auto get_str = [&](const char* key) -> std::string {
            std::string k = std::string("\"") + key + "\":\"";
            auto p = msg.find(k);
            if (p == std::string::npos) return "";
            auto s = p + k.size();
            auto e = msg.find('"', s);
            return (e != std::string::npos) ? msg.substr(s, e - s) : "";
        };
        std::string new_channel = get_str("channel");
        if (new_channel.empty()) {
            snprintf(buf, sizeof(buf),
                "{\"id\":%d,\"success\":false,\"data\":\"missing channel\"}", id);
        } else {
            // Disconnect existing WAN relay and reconnect with new channel
            wan_relay_shutdown();
            std::string host, password;
            uint16_t port;
            {
                std::lock_guard<std::mutex> lk(g_wan_cfg_mutex);
                host = g_wan_cfg_host;
                port = g_wan_cfg_port;
                password = g_wan_cfg_password;
            }
            wan_relay_init(host, port, new_channel, password);
            fprintf(stderr, "[channel] Switched to channel: %s\n", new_channel.c_str());
            // Broadcast channel change event to all connected clients
            if (g_ws_server_ptr) {
                char ev[256];
                snprintf(ev, sizeof(ev),
                    "{\"event\":\"channel.changed\",\"channel\":\"%s\"}",
                    new_channel.c_str());
                g_ws_server_ptr->broadcast(ev);
            }
            snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
        }

    } else {
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"success\":false,\"data\":\"unknown command\"}", id);
    }
    return buf;
}

static void start_ws_server(soluna::control::WebSocketServer& srv,
                            bool https_enabled = false,
                            const std::string& cert_path = "",
                            const std::string& key_path = "") {
    g_ws_server_ptr = &srv;
    srv.set_web_files(
        reinterpret_cast<const soluna::control::WebFile*>(embedded_web_files),
        embedded_web_file_count);
    srv.set_message_callback(ws_handle);

    // HTTP POST handler: /api/player/upload — receives raw audio file bytes
    srv.set_http_post_handler([](const std::string& path,
                                 const std::vector<uint8_t>& body,
                                 std::string& out_ct) -> std::string {
        // path may include query string: /api/player/upload?name=...
        if (path.rfind("/api/player/upload", 0) != 0 || body.empty()) return "";
        out_ct = "application/json";

        // Detect format from magic bytes
        std::string ext = "bin";
        if (body.size() >= 4) {
            if (body[0]=='R' && body[1]=='I' && body[2]=='F' && body[3]=='F') ext = "wav";
            else if (body[0]==0xFF && (body[1]&0xE0)==0xE0) ext = "mp3";
            else if (body[0]=='I' && body[1]=='D' && body[2]=='3') ext = "mp3";
        }

        // Extract filename from query string: /api/player/upload?name=track.mp3
        std::string name = "upload." + ext;
        auto qpos = path.find("?name=");
        if (qpos != std::string::npos) {
            name = path.substr(qpos + 6);
            // Basic URL-decode of spaces
            for (size_t i = 0; i < name.size(); i++) {
                if (name[i] == '+') name[i] = ' ';
                if (name[i] == '%' && i + 2 < name.size()) {
                    char hex[3] = {name[i+1], name[i+2], 0};
                    name[i] = (char)std::stoi(hex, nullptr, 16);
                    name.erase(i+1, 2);
                }
            }
        }

        // Stop any current playback
        g_player_active.store(false);
        if (g_player_thread.joinable()) g_player_thread.join();

        // Write to temp file
        std::string tmp_path = "/tmp/soluna_player_" + name;
        {
            std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
            if (!out || !out.write(reinterpret_cast<const char*>(body.data()), body.size())) {
                return "{\"ok\":false,\"error\":\"write failed\"}";
            }
        }

        {
            std::lock_guard<std::mutex> lk(g_player_mutex);
            g_player_file_path = tmp_path;
            g_player_file_name = name;
            g_player_file_size = (uint32_t)body.size();
        }
        g_player_frame_pos.store(0);

        // Probe duration
        {
            soluna::pipeline::FileSource probe;
            if (probe.open(tmp_path, g_cfg_sample_rate, g_cfg_channels)) {
                g_player_duration_ms.store(probe.duration_ms());
            }
        }

        char resp[256];
        snprintf(resp, sizeof(resp),
            "{\"ok\":true,\"name\":\"%s\",\"size\":%u,\"dur_ms\":%llu,\"fmt\":\"%s\"}",
            name.c_str(), (uint32_t)body.size(),
            (unsigned long long)g_player_duration_ms.load(),
            (ext == "mp3" ? "MP3" : "WAV"));
        return resp;
    });

    if (https_enabled && !cert_path.empty() && !key_path.empty()) {
        if (srv.enable_tls(cert_path, key_path)) {
            fprintf(stderr, "HTTPS/WSS enabled on port 8400\n");
        }
    }

    if (srv.start(8400)) {
        const char* proto = https_enabled ? "https" : "http";
        printf("Web UI: %s://localhost:8400\n", proto);
    }
}

#ifdef __APPLE__
static DNSServiceRef g_mdns = nullptr;
static void start_mdns_advertisement() {
    uint16_t netport = htons(8400);
    DNSServiceErrorType err = DNSServiceRegister(
        &g_mdns, 0, 0,
        "Soluna",          // service instance name
        "_soluna._tcp",    // service type
        nullptr,           // domain  (default = .local)
        nullptr,           // host    (default = this machine)
        netport,
        0, nullptr,        // TXT record (none)
        nullptr, nullptr); // callback (none needed)
    if (err != kDNSServiceErr_NoError) {
        fprintf(stderr, "[mdns] DNSServiceRegister failed: %d\n", err);
        return;
    }
    // Pump the mDNS socket in a background thread
    std::thread([] {
        int fd = DNSServiceRefSockFD(g_mdns);
        while (g_mdns && fd >= 0) {
            fd_set r; FD_ZERO(&r); FD_SET(fd, &r);
            struct timeval tv{1, 0};
            if (select(fd + 1, &r, nullptr, nullptr, &tv) > 0)
                DNSServiceProcessResult(g_mdns);
        }
    }).detach();
    printf("[mdns] Advertising _soluna._tcp on port 8400\n");
}
#endif

// Latency profile for different network conditions
enum class LatencyProfile {
    Auto,        // detect WiFi vs wired at startup
    Default,     // 240f/5ms packets, 20ms buffer (WiFi-safe)
    WiFi,        // 96f/2ms packets, 8ms buffer (WiFi optimized)
    LowLatency,  // 48f/1ms packets, 2ms buffer (wired LAN only)
    UltraLow,    // 12f/250us packets, 1ms buffer (GbE, minimum latency)
};

struct LatencyParams {
    uint32_t frames_per_packet;
    soluna::PacketTier packet_tier;
    uint32_t prefill_packets;
    uint32_t refill_threshold;
    uint32_t buf_target_ms;
    uint32_t mon_target_ms;
    uint32_t recv_timeout_ms;
    const char* label;
};

static LatencyParams get_latency_params(LatencyProfile profile) {
    switch (profile) {
    case LatencyProfile::UltraLow:
        //        frames  tier                  prefill refill buf mon timeout label
        return {48, soluna::PacketTier::Standard, 1, 1, 1, 3, 1, "ultra-low"};
    case LatencyProfile::LowLatency:
        return {48, soluna::PacketTier::Standard, 2, 1, 2, 5, 1, "low-latency"};
    case LatencyProfile::WiFi:
        return {96, soluna::PacketTier::WiFi, 125, 1, 100, 500, 5, "wifi-latency"};
    default:
        return {240, soluna::PacketTier::LAN, 4, 2, 20, 20, 10, "default"};
    }
}

// Resolve Auto profile by detecting network interface type
static LatencyProfile resolve_latency_profile(LatencyProfile profile, const std::string& multicast_ip) {
    if (profile != LatencyProfile::Auto) return profile;

    std::string iface = detect_outgoing_interface(multicast_ip.c_str());
    if (iface.empty()) {
        fprintf(stderr, "[auto] Cannot detect network interface, using default profile\n");
        return LatencyProfile::Default;
    }

    bool wifi = is_interface_wifi(iface);
    if (wifi) {
        fprintf(stderr, "[auto] Detected WiFi interface '%s' → wifi-latency profile\n", iface.c_str());
        return LatencyProfile::WiFi;
    } else {
        fprintf(stderr, "[auto] Detected wired interface '%s' → low-latency profile\n", iface.c_str());
        return LatencyProfile::LowLatency;
    }
}

struct DaemonConfig {
    bool tx_mode = false;
    bool rx_mode = false;
    bool aes67_mode = false;
    bool tunnel = false;  // start cloudflared/ngrok tunnel
    bool low_latency = false; // AES67/Dante-grade low latency (wired LAN only)
    LatencyProfile latency_profile = LatencyProfile::Auto;
    std::string audio_device;
    std::string dest_ip = soluna::kMulticastAudio;
    uint16_t dest_port = soluna::kPortRTPBase;
    uint16_t listen_port = soluna::kPortRTPBase;
    uint32_t sample_rate = soluna::kDefaultSampleRate;
    uint32_t channels = 1;
    uint32_t ssrc = 0x4F534E43; // "OSNC"
    uint16_t stream_id = 1;
    std::string config_file;

    // Local speaker device for SHM (soluna) TX mode.
    // Empty string = system default output.
    std::string local_speaker_device;

    // Auto-tune: mic-based noise detection (default ON)
    bool auto_tune = true;

    // TX audio recording for quality comparison
    std::string record_tx_path;
    uint32_t record_tx_duration = 30; // seconds

    // Unicast relay for P2P (bypass multicast packet loss)
    uint16_t relay_port = 5099;
    bool relay_enabled = true; // auto-start relay on TX

    // Codec: "pcm" (default) or "opus"
    std::string codec = "pcm";

    // Stream mode: "sync" (default) or "jam"
    std::string mode_str = "sync";

    // WAN relay (forward TX to remote soluna-relay server)
    std::string wan_relay_host;       // empty = disabled
    uint16_t    wan_relay_port = 5100;
    std::string wan_relay_group = "default";
    std::string wan_relay_password;

    // HTTPS/TLS for WebSocket server
    bool https_enabled = false;

    // Security settings from config
    soluna::config::SecurityConfig security;

    // Apply settings from YAML config
    void apply_yaml_config(const soluna::config::Config& cfg) {
        if (!cfg.device.audio_device.empty() && cfg.device.audio_device != "default") {
            audio_device = cfg.device.audio_device;
        }
        if (cfg.network.rtp_base_port != 5004) {
            dest_port = cfg.network.rtp_base_port;
            listen_port = cfg.network.rtp_base_port;
        }
        if (!cfg.network.multicast_audio.empty()) {
            dest_ip = cfg.network.multicast_audio;
        }
        if (cfg.audio.sample_rate != 48000) {
            sample_rate = cfg.audio.sample_rate;
        }
        if (cfg.audio.channels != 2) {
            channels = cfg.audio.channels;
        }
        // Apply security settings
        security = cfg.security;
        // Apply stream mode
        if (cfg.mode == soluna::StreamMode::Jam) {
            mode_str = "jam";
        }
    }
};

static void print_usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --config FILE     Load configuration from YAML file\n"
        "  --tx              Transmit mode (capture → network)\n"
        "  --rx              Receive mode (network → playback)\n"
        "  --aes67-mode      Use AES67-compatible RTP (no OSTP extensions)\n"
        "  --device DEV      Audio device (e.g., hw:0, default, soluna)\n"
        "  --speaker DEV     Local speaker device for --device soluna (default: \"\")\n"
        "  --dest IP:PORT    Destination (TX mode, default: 239.69.0.1:5004)\n"
        "  --port PORT       Listen port (RX mode, default: 5004)\n"
        "  --rate RATE       Sample rate (default: 48000)\n"
        "  --channels N      Channel count (default: 1)\n"
        "  --codec pcm|opus  Audio codec (default: pcm)\n"
        "  --mode sync|jam   Stream mode: sync (multi-room) or jam (low-latency)\n"
        "  --ultra-low       Ultra-low latency (~1ms, GbE only)\n"
        "  --low-latency     AES67-grade low latency (~2ms, wired LAN only)\n"
        "  --wifi-latency    WiFi optimized latency (~10ms, stable on WiFi)\n"
        "  --https           Enable HTTPS/WSS for WebSocket server\n"
        "  --dtls            Enable DTLS encryption\n"
        "  --cert FILE       TLS/DTLS certificate file (PEM)\n"
        "  --key FILE        TLS/DTLS private key file (PEM)\n"
        "  --relay-port PORT Unicast relay port for P2P peers (default: 5099)\n"
        "  --no-relay        Disable unicast relay\n"
        "  --wan-relay HOST:PORT  WAN relay server address (default port: 5100)\n"
        "  --wan-group NAME  WAN relay group name (default: default)\n"
        "  --wan-password PW WAN relay group password (optional)\n"
        "  --record-tx FILE  Record TX audio to WAV file (for comparison)\n"
        "  --record-dur SEC  Recording duration in seconds (default: 30)\n"
        "  --record-dir DIR  Multi-track recording directory (tx + monitor WAVs)\n"
        "  --auto-tune       Enable mic-based auto buffer tuning (default: on)\n"
        "  --no-auto-tune    Disable mic-based auto buffer tuning\n"
        "  --list-devices    List available audio devices\n"
        "  --generate-config Print default configuration to stdout\n"
        "  --help            Show this help\n",
        prog);
}

static bool parse_args(int argc, char** argv, DaemonConfig& cfg) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--tx") {
            cfg.tx_mode = true;
        } else if (arg == "--rx") {
            cfg.rx_mode = true;
        } else if (arg == "--aes67-mode") {
            cfg.aes67_mode = true;
        } else if (arg == "--ultra-low") {
            cfg.low_latency = true;
            cfg.latency_profile = LatencyProfile::UltraLow;
        } else if (arg == "--low-latency") {
            cfg.low_latency = true;
            cfg.latency_profile = LatencyProfile::LowLatency;
        } else if (arg == "--wifi-latency") {
            cfg.latency_profile = LatencyProfile::WiFi;
        } else if (arg == "--auto-tune") {
            cfg.auto_tune = true;
        } else if (arg == "--no-auto-tune") {
            cfg.auto_tune = false;
        } else if (arg == "--tunnel") {
            cfg.tunnel = true;
        } else if (arg == "--https") {
            cfg.https_enabled = true;
        } else if (arg == "--dtls") {
            cfg.security.dtls_enabled = true;
        } else if (arg == "--cert" && i + 1 < argc) {
            cfg.security.certificate_path = argv[++i];
        } else if (arg == "--key" && i + 1 < argc) {
            cfg.security.private_key_path = argv[++i];
        } else if (arg == "--relay-port" && i + 1 < argc) {
            cfg.relay_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--no-relay") {
            cfg.relay_enabled = false;
        } else if (arg == "--wan-relay" && i + 1 < argc) {
            std::string hp = argv[++i];
            auto colon = hp.rfind(':');
            if (colon != std::string::npos) {
                cfg.wan_relay_host = hp.substr(0, colon);
                cfg.wan_relay_port = static_cast<uint16_t>(std::stoi(hp.substr(colon + 1)));
            } else {
                cfg.wan_relay_host = hp;
            }
        } else if (arg == "--wan-group" && i + 1 < argc) {
            cfg.wan_relay_group = argv[++i];
        } else if (arg == "--wan-password" && i + 1 < argc) {
            cfg.wan_relay_password = argv[++i];
        } else if (arg == "--record-tx" && i + 1 < argc) {
            cfg.record_tx_path = argv[++i];
        } else if (arg == "--record-dur" && i + 1 < argc) {
            cfg.record_tx_duration = static_cast<uint32_t>(std::stoi(argv[++i]));
        } else if (arg == "--record-dir" && i + 1 < argc) {
            g_record_dir = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            cfg.config_file = argv[++i];
        } else if (arg == "--device" && i + 1 < argc) {
            cfg.audio_device = argv[++i];
        } else if (arg == "--speaker" && i + 1 < argc) {
            cfg.local_speaker_device = argv[++i];
        } else if (arg == "--dest" && i + 1 < argc) {
            std::string dest = argv[++i];
            auto colon = dest.rfind(':');
            if (colon != std::string::npos) {
                cfg.dest_ip = dest.substr(0, colon);
                cfg.dest_port = static_cast<uint16_t>(std::stoi(dest.substr(colon + 1)));
            } else {
                cfg.dest_ip = dest;
            }
        } else if (arg == "--port" && i + 1 < argc) {
            cfg.listen_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--rate" && i + 1 < argc) {
            cfg.sample_rate = static_cast<uint32_t>(std::stoi(argv[++i]));
        } else if (arg == "--channels" && i + 1 < argc) {
            cfg.channels = static_cast<uint32_t>(std::stoi(argv[++i]));
        } else if (arg == "--codec" && i + 1 < argc) {
            cfg.codec = argv[++i];
            if (cfg.codec != "pcm" && cfg.codec != "opus") {
                fprintf(stderr, "Unknown codec: %s (expected pcm or opus)\n", cfg.codec.c_str());
                return false;
            }
        } else if (arg == "--mode" && i + 1 < argc) {
            cfg.mode_str = argv[++i];
            if (cfg.mode_str != "sync" && cfg.mode_str != "jam") {
                fprintf(stderr, "Unknown mode: %s (expected sync or jam)\n", cfg.mode_str.c_str());
                return false;
            }
        } else if (arg == "--list-devices") {
            auto devices = soluna::pal::AudioDevice::enumerate();
            printf("Available audio devices:\n");
            for (const auto& d : devices) {
                printf("  %-30s  in:%u out:%u  [%s]\n",
                    d.name.c_str(), d.max_input_channels, d.max_output_channels, d.id.c_str());
            }
            std::exit(0);
        } else if (arg == "--generate-config") {
            auto default_cfg = soluna::config::Config::defaults();
            printf("%s", default_cfg.to_yaml().c_str());
            std::exit(0);
        } else if (arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            return false;
        }
    }

    // Load config file if specified or search default paths
    if (!cfg.config_file.empty()) {
        auto result = soluna::config::ConfigLoader::load(cfg.config_file);
        if (!result.ok()) {
            fprintf(stderr, "Error loading config: %s\n", result.error().to_string().c_str());
            return false;
        }
        cfg.apply_yaml_config(result.value());
    } else {
        // Try default paths
        auto result = soluna::config::ConfigLoader::load_with_fallbacks(
            soluna::config::ConfigLoader::default_paths());
        if (result.ok()) {
            cfg.apply_yaml_config(result.value());
        }
    }

    // Convert mode string to StreamMode and set global
    soluna::StreamMode stream_mode = (cfg.mode_str == "jam")
        ? soluna::StreamMode::Jam : soluna::StreamMode::Sync;
    g_stream_mode.store(stream_mode);

    if (!cfg.tx_mode && !cfg.rx_mode) {
        fprintf(stderr, "Error: specify --tx or --rx\n");
        return false;
    }

    return true;
}

// ── Auto-tune thread (mic-based noise detection → automatic buffer adjustment) ──
static void tune_thread_fn(const DaemonConfig& cfg) {
    using namespace soluna::pal;

    const auto lp = get_latency_params(cfg.latency_profile);
    const uint32_t min_buf_ms = lp.buf_target_ms;  // lower bound = profile default
    const uint32_t min_mon_ms = lp.mon_target_ms;
    constexpr uint32_t kMaxMs = 50;

    while (g_running.load()) {
        // Wait for start request
        if (!g_tune_start_req.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        g_tune_start_req.store(false);
        g_tune_stop_req.store(false);
        g_tune_active.store(true);
        g_tune_clicks.store(0);
        g_tune_dropouts.store(0);
        g_tune_adjustments.store(0);
        g_tune_rms_db.store(-100.0f);

        fprintf(stderr, "[tune] Starting mic-based auto-tuning (min buf=%ums, min mon=%ums, max=%ums)\n",
                min_buf_ms, min_mon_ms, kMaxMs);

        // Open mic
        auto mic = AudioDevice::create();
        if (!mic) {
            fprintf(stderr, "[tune] Cannot create audio device\n");
            g_tune_active.store(false);
            continue;
        }
        AudioStreamConfig mic_cfg;
        mic_cfg.sample_rate = 48000;
        mic_cfg.channels = 1;
        mic_cfg.frames_per_buffer = 256;  // ~5.3ms chunks

        if (!mic->open_input("", mic_cfg)) {
            fprintf(stderr, "[tune] Cannot open default mic\n");
            g_tune_active.store(false);
            continue;
        }

        // Analysis state
        constexpr uint32_t kWindowFrames = 48000;  // 1 second at 48kHz
        constexpr uint32_t kWindowSlots  = kWindowFrames / 256;  // ~187 slots
        std::vector<float> rms_window(kWindowSlots, 0.0f);
        uint32_t window_idx = 0;
        float prev_rms = 0.0f;

        uint32_t clicks_total = 0;
        uint32_t dropouts_total = 0;
        uint32_t adjustments_total = 0;
        uint32_t stable_callbacks = 0;

        // Noise flag: set in callback, consumed in analysis
        std::atomic<bool> noise_detected{false};
        std::atomic<uint32_t> cb_clicks{0};
        std::atomic<uint32_t> cb_dropouts{0};
        std::atomic<float>    cb_rms{0.0f};

        bool started = mic->start([&](float* buffer, uint32_t frame_count) {
            // Calculate RMS
            float sum_sq = 0.0f;
            for (uint32_t i = 0; i < frame_count; i++)
                sum_sq += buffer[i] * buffer[i];
            float rms = std::sqrt(sum_sq / frame_count);
            cb_rms.store(rms);

            // Click detection: sample-to-sample jumps > Nσ RMS
            float sigma = g_noise_sigma.load(std::memory_order_relaxed);
            float threshold = rms * sigma;
            if (threshold < 0.001f) threshold = 0.001f;  // absolute minimum
            uint32_t clicks = 0;
            for (uint32_t i = 1; i < frame_count; i++) {
                if (std::fabs(buffer[i] - buffer[i - 1]) > threshold)
                    clicks++;
            }
            if (clicks > 0) {
                cb_clicks.fetch_add(clicks);
                noise_detected.store(true);
            }

            // Silence gap: RMS dropped from >-40dBFS to <-60dBFS
            float rms_db = (rms > 1e-10f) ? 20.0f * std::log10(rms) : -100.0f;
            float prev = prev_rms;
            float prev_db = (prev > 1e-10f) ? 20.0f * std::log10(prev) : -100.0f;
            if (prev_db > -40.0f && rms_db < -60.0f) {
                cb_dropouts.fetch_add(1);
                noise_detected.store(true);
            }

            // Rolling window
            rms_window[window_idx % kWindowSlots] = rms;
            window_idx++;
            prev_rms = rms;
        });

        if (!started) {
            fprintf(stderr, "[tune] Cannot start mic capture\n");
            mic->close();
            g_tune_active.store(false);
            continue;
        }

        fprintf(stderr, "[tune] Mic opened, listening for noise...\n");

        // Main analysis loop — check every 200ms
        while (g_running.load() && !g_tune_stop_req.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            float rms = cb_rms.load();
            float rms_db = (rms > 1e-10f) ? 20.0f * std::log10(rms) : -100.0f;
            g_tune_rms_db.store(rms_db);

            // Check if noise was detected
            uint32_t new_clicks = cb_clicks.exchange(0);
            uint32_t new_drops  = cb_dropouts.exchange(0);
            clicks_total += new_clicks;
            dropouts_total += new_drops;
            g_tune_clicks.store(clicks_total);
            g_tune_dropouts.store(dropouts_total);

            if (noise_detected.exchange(false)) {
                // Phase 1: Tighten repair params first (lower latency impact)
                constexpr float kMinSigma = 2.0f;
                constexpr float kMinFloor = 0.001f;
                constexpr float kMinCfThresh = 0.005f;
                constexpr uint32_t kMaxCfFrames = 64;

                float cur_sigma = g_noise_sigma.load();
                float cur_cf_thresh = g_crossfade_thresh.load();
                uint32_t cur_cf_frames = g_crossfade_frames.load();
                bool repair_adjusted = false;

                // Lower sigma → more aggressive click removal
                if (cur_sigma > kMinSigma) {
                    float new_sigma = std::fmax(kMinSigma, cur_sigma - 0.5f);
                    g_noise_sigma.store(new_sigma);
                    repair_adjusted = true;
                }
                // Lower crossfade threshold → catch more boundary glitches
                if (cur_cf_thresh > kMinCfThresh) {
                    float new_cf = std::fmax(kMinCfThresh, cur_cf_thresh - 0.005f);
                    g_crossfade_thresh.store(new_cf);
                    repair_adjusted = true;
                }
                // Increase crossfade frames → smoother transitions
                if (cur_cf_frames < kMaxCfFrames) {
                    g_crossfade_frames.store(std::min(kMaxCfFrames, cur_cf_frames + 4));
                    repair_adjusted = true;
                }

                if (repair_adjusted) {
                    adjustments_total++;
                    g_tune_adjustments.store(adjustments_total);
                    fprintf(stderr, "[tune] Noise→repair: sigma=%.1f cf_thresh=%.3f cf_frames=%u (clicks=%u drops=%u)\n",
                            (double)g_noise_sigma.load(), (double)g_crossfade_thresh.load(),
                            g_crossfade_frames.load(), new_clicks, new_drops);
                }

                // Phase 2: If repair already at max aggressiveness, increase buffers
                if (!repair_adjusted || new_clicks > 5 || new_drops > 0) {
                    uint32_t step_up = g_tune_step_up.load(std::memory_order_relaxed);
                    uint32_t cur_buf = g_buf_target_ms.load();
                    uint32_t cur_mon = g_mon_target_ms.load();
                    uint32_t new_buf = std::min(kMaxMs, cur_buf + step_up);
                    uint32_t new_mon = std::min(kMaxMs, cur_mon + step_up);

                    if (new_buf != cur_buf || new_mon != cur_mon) {
                        g_buf_target_ms.store(new_buf);
                        g_mon_target_ms.store(new_mon);
                        if (!repair_adjusted) {
                            adjustments_total++;
                            g_tune_adjustments.store(adjustments_total);
                        }
                        fprintf(stderr, "[tune] Noise→buffer: buf=%ums mon=%ums\n", new_buf, new_mon);
                    }
                }
                stable_callbacks = 0;
            } else {
                stable_callbacks++;

                // Stable for N seconds → relax params
                uint32_t stable_iters = g_tune_stable_sec.load(std::memory_order_relaxed) * 5;
                if (stable_iters < 1) stable_iters = 1;
                if (stable_callbacks >= stable_iters) {
                    // Phase 1: Decrease buffers first (reduce latency)
                    uint32_t step_dn = g_tune_step_down.load(std::memory_order_relaxed);
                    uint32_t cur_buf = g_buf_target_ms.load();
                    uint32_t cur_mon = g_mon_target_ms.load();
                    bool buf_changed = false;
                    if (cur_buf > min_buf_ms) {
                        g_buf_target_ms.store(cur_buf > min_buf_ms + step_dn - 1 ? cur_buf - step_dn : min_buf_ms);
                        buf_changed = true;
                    }
                    if (cur_mon > min_mon_ms) {
                        g_mon_target_ms.store(cur_mon > min_mon_ms + step_dn - 1 ? cur_mon - step_dn : min_mon_ms);
                        buf_changed = true;
                    }
                    if (buf_changed) {
                        fprintf(stderr, "[tune] Stable→buffer: buf=%ums mon=%ums\n",
                                g_buf_target_ms.load(), g_mon_target_ms.load());
                    }

                    // Phase 2: Once buffers at minimum, relax repair params
                    if (!buf_changed) {
                        constexpr float kDefaultSigma = 6.0f;
                        constexpr float kDefaultCfThresh = 0.02f;
                        constexpr uint32_t kDefaultCfFrames = 16;
                        float cur_sigma = g_noise_sigma.load();
                        float cur_cf_thresh = g_crossfade_thresh.load();
                        uint32_t cur_cf_frames = g_crossfade_frames.load();
                        bool relaxed = false;

                        if (cur_sigma < kDefaultSigma) {
                            g_noise_sigma.store(std::fmin(kDefaultSigma, cur_sigma + 0.25f));
                            relaxed = true;
                        }
                        if (cur_cf_thresh < kDefaultCfThresh) {
                            g_crossfade_thresh.store(std::fmin(kDefaultCfThresh, cur_cf_thresh + 0.002f));
                            relaxed = true;
                        }
                        if (cur_cf_frames > kDefaultCfFrames) {
                            g_crossfade_frames.store(std::max(kDefaultCfFrames, cur_cf_frames - 2));
                            relaxed = true;
                        }
                        if (relaxed) {
                            fprintf(stderr, "[tune] Stable→repair: sigma=%.1f cf_thresh=%.3f cf_frames=%u\n",
                                    (double)g_noise_sigma.load(), (double)g_crossfade_thresh.load(),
                                    g_crossfade_frames.load());
                        }
                    }
                    stable_callbacks = 0;
                }
            }
        }

        mic->stop();
        mic->close();
        g_tune_active.store(false);
        fprintf(stderr, "[tune] Stopped (clicks=%u dropouts=%u adjustments=%u)\n",
                clicks_total, dropouts_total, adjustments_total);
    }
}

// ── Monitor thread (runs alongside TX, receives own multicast and plays locally) ──
static void monitor_thread_fn(const DaemonConfig& cfg) {
    using namespace soluna;
    using namespace soluna::pal;
    using namespace soluna::pipeline;
    using namespace soluna::transport;

    const auto mon_lp = get_latency_params(cfg.latency_profile);
    const uint32_t kFramesPerPkt = mon_lp.frames_per_packet;
    const size_t kRingFrames    = kFramesPerPkt * 40;   // capacity
    const size_t frame_size = sizeof(int32_t) * cfg.channels;

    while (g_running.load()) {
        // Wait for a start request
        std::string dev_name;
        {
            std::lock_guard<std::mutex> lk(g_mon_mutex);
            if (g_mon_start_req.pending) {
                dev_name = g_mon_start_req.device;
                g_mon_start_req.pending = false;
            }
        }
        if (dev_name.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // --- Open socket ---
        auto socket = UdpSocket::create();
        if (!socket || !socket->bind(cfg.listen_port) ||
            !socket->join_multicast(cfg.dest_ip)) {
            fprintf(stderr, "Monitor: cannot bind to %s:%u\n",
                    cfg.dest_ip.c_str(), cfg.listen_port);
            continue;
        }
        socket->set_recv_timeout_ms(mon_lp.recv_timeout_ms);

        // --- Open audio output ---
        RingBuffer ring(kRingFrames, frame_size);
        std::atomic<bool> prefilled{false};
        // cb_buf must be large enough for any fc CoreAudio may use (up to 4096)
        std::vector<int32_t> cb_buf(4096 * cfg.channels);

        auto audio = AudioDevice::create();
        if (!audio) continue;

        AudioStreamConfig acfg;
        acfg.sample_rate    = cfg.sample_rate;
        acfg.channels       = cfg.channels;
        acfg.frames_per_buffer = kFramesPerPkt;
        if (!audio->open_output(dev_name, acfg)) {
            fprintf(stderr, "Monitor: cannot open '%s'\n", dev_name.c_str());
            continue;
        }

        audio->start([&](float* buf, uint32_t fc) {
            size_t samples = fc * cfg.channels;

            // Latency trim: if ring has grown beyond target, skip excess from reader side
            {
                uint32_t target = g_mon_target_ms.load() * (cfg.sample_rate / 1000u);
                const uint32_t min_target = fc * 4;
                if (target < min_target) target = min_target;
                size_t avail = ring.available_read();
                if (avail > target + fc) {
                    size_t excess = avail - target - fc;
                    // Discard in chunks, staying within cb_buf capacity
                    while (excess > 0) {
                        size_t chunk = std::min(excess, cb_buf.size() / cfg.channels);
                        size_t dr = ring.read(cb_buf.data(), chunk);
                        if (dr == 0) break;
                        excess = (excess > dr) ? excess - dr : 0;
                    }
                }
            }

            if (!prefilled.load()) {
                if (ring.available_read() < fc * 4) {
                    std::memset(buf, 0, samples * sizeof(float));
                    return;
                }
                prefilled.store(true);
            }
            if (ring.available_read() < fc) {
                std::memset(buf, 0, samples * sizeof(float));
                prefilled.store(false);
                return;
            }
            ring.read(cb_buf.data(), fc);
            float gain = g_mon_muted.load() ? 0.0f : g_mon_volume.load();
            for (size_t i = 0; i < samples; i++) {
                float s = static_cast<float>(cb_buf[i]) / 8388608.0f;
                buf[i] = (s > 1.0f ? 1.0f : s < -1.0f ? -1.0f : s) * gain;
            }
            // Audio repair
            if (g_repair_enabled.load(std::memory_order_relaxed)) {
                static float mon_prev[8] = {};
                static bool mon_had_audio = false;
                uint32_t fixed = deglitch_buffer(buf, fc, cfg.channels);
                if (fixed > 0)
                    g_repair_clicks.fetch_add(fixed, std::memory_order_relaxed);
                crossfade_boundary(buf, fc, cfg.channels,
                                   mon_prev, &mon_had_audio);
            }

            // Multi-track recording: monitor track
            if (g_rec_active.load(std::memory_order_relaxed) && g_rec_monitor.is_open()) {
                thread_local std::vector<int16_t> mt_mon_buf;
                mt_mon_buf.resize(samples);
                for (size_t i = 0; i < samples; i++)
                    mt_mon_buf[i] = static_cast<int16_t>(buf[i] * 32767.0f);
                g_rec_monitor.write(mt_mon_buf.data(), fc);
            }
        });

        g_mon_active.store(true);
        g_mon_stop_req.store(false);
        printf("Monitor: started on '%s'\n", dev_name.c_str());

        // --- Receive loop ---
        std::vector<uint8_t> recv_buf(kMaxPacketSize);
        std::vector<int32_t> audio_buf(kMaxPayloadSize / sizeof(int32_t));
        int32_t mon_last_seq = -1;  // for duplicate detection

        while (g_running.load() && !g_mon_stop_req.load()) {
            // Check for restart request
            {
                std::lock_guard<std::mutex> lk(g_mon_mutex);
                if (g_mon_start_req.pending) break;  // restart with new device
            }

            SocketAddress src;
            int n = socket->recv_from(recv_buf.data(), recv_buf.size(), src);
            if (n <= 0) continue;
            if (static_cast<size_t>(n) < sizeof(RtpHeader)) continue;

            const RtpHeader* rtp_ptr = reinterpret_cast<const RtpHeader*>(recv_buf.data());
            bool is_aes67 = aes67_is_standard_packet(*rtp_ptr);
            size_t frames = 0;

            if (is_aes67) {
                RtpHeader rtp; std::memcpy(&rtp, recv_buf.data(), sizeof(RtpHeader));
                // Duplicate detection for AES67
                uint16_t seq = ntohs(rtp.sequence);
                if (mon_last_seq >= 0) {
                    int32_t diff = static_cast<int32_t>(seq) - (mon_last_seq & 0xFFFF);
                    if (diff < -32768) diff += 65536;
                    if (diff > 32768) diff -= 65536;
                    if (diff <= 0) { continue; } // duplicate or old
                }
                mon_last_seq = seq;

                const uint8_t* pl = recv_buf.data() + sizeof(RtpHeader);
                size_t pl_sz = static_cast<size_t>(n) - sizeof(RtpHeader);
                if (rtp.pt == kPayloadTypeAES67_L24) {
                    size_t s = pl_sz / 3;
                    for (size_t i = 0; i < s && i < audio_buf.size(); i++) {
                        int32_t v = (static_cast<int32_t>(pl[i*3])<<16)
                                  | (static_cast<int32_t>(pl[i*3+1])<<8)
                                  |  static_cast<int32_t>(pl[i*3+2]);
                        if (v & 0x800000) v |= 0xFF000000;
                        audio_buf[i] = v;
                    }
                    frames = s / cfg.channels;
                    ring.write(audio_buf.data(), frames);
                }
            } else {
                RtpHeader rtp; OstpHeader ostp;
                const uint8_t* pl = nullptr; size_t pl_sz = 0;
                if (ostp_parse_packet(recv_buf.data(), n, rtp, ostp, pl, pl_sz) == 0) {
                    // Duplicate detection for OSTP
                    uint32_t full_seq = (static_cast<uint32_t>(ostp.sequence_ext) << 16) | rtp.sequence;
                    if (mon_last_seq >= 0 && static_cast<int32_t>(full_seq) <= mon_last_seq) {
                        continue; // duplicate or old
                    }
                    mon_last_seq = static_cast<int32_t>(full_seq);

                    frames = pl_sz / frame_size;
                    ring.write(pl, frames);
                }
            }

            if (frames > 0) {
                g_mon_packets.fetch_add(1);
                g_lat_mon_ring_frames.store((uint32_t)ring.available_read());
            }
        }

        g_mon_active.store(false);
        audio->stop();
        printf("Monitor: stopped\n");
    }
}

// ── Input passthrough thread (captures external input → writes to SHM) ───────
#ifdef __APPLE__
static void input_passthrough_thread_fn(SolunaShmMap* shm, uint32_t out_channels) {
    using namespace soluna::pal;

    while (g_running.load()) {
        // Wait for start request
        std::string dev_name;
        uint32_t channel = 0;
        {
            std::lock_guard<std::mutex> lk(g_input_mutex);
            if (g_input_start_req.pending) {
                dev_name = g_input_start_req.device;
                channel  = g_input_start_req.channel;
                g_input_start_req.pending = false;
            }
        }
        if (dev_name.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        g_input_channel.store(channel);

        auto audio = AudioDevice::create();
        if (!audio) continue;

        // Query device channel count before opening
        uint32_t dev_channels = 14; // fallback for Babyface
        {
            auto devs = AudioDevice::enumerate();
            for (auto& d : devs) {
                if (d.name == dev_name && d.max_input_channels > 0) {
                    dev_channels = d.max_input_channels;
                    break;
                }
            }
        }

        AudioStreamConfig acfg;
        acfg.sample_rate       = 48000;
        acfg.channels          = dev_channels;
        acfg.frames_per_buffer = 256;

        if (!audio->open_input(dev_name, acfg)) {
            fprintf(stderr, "[input] Cannot open '%s' (%uch @ %uHz)\n",
                    dev_name.c_str(), dev_channels, acfg.sample_rate);
            // Retry with 44100Hz (Babyface default)
            acfg.sample_rate = 44100;
            audio = AudioDevice::create();
            if (!audio || !audio->open_input(dev_name, acfg)) {
                fprintf(stderr, "[input] Cannot open '%s' at 44100 either\n", dev_name.c_str());
                continue;
            }
        }

        uint32_t in_channels = acfg.channels;
        fprintf(stderr, "[input] Opened '%s': %uch @ %uHz\n",
                dev_name.c_str(), in_channels, acfg.sample_rate);

        // Float buffer for writing stereo to SHM
        std::vector<float> stereo_buf(4096 * out_channels);

        audio->start([&](float* buf, uint32_t fc) {
            uint32_t ch = g_input_channel.load();
            float vol   = g_input_volume.load();

            // Extract the selected channel and write as stereo to SHM
            for (uint32_t f = 0; f < fc && f < stereo_buf.size() / out_channels; f++) {
                float sample = 0.0f;
                if (ch < in_channels) {
                    sample = buf[f * in_channels + ch] * vol;
                    if (sample > 1.0f) sample = 1.0f;
                    if (sample < -1.0f) sample = -1.0f;
                }
                for (uint32_t c = 0; c < out_channels; c++)
                    stereo_buf[f * out_channels + c] = sample;
            }
            soluna_shm_write(shm, stereo_buf.data(), fc);
        });

        g_input_active.store(true);
        g_input_stop_req.store(false);
        fprintf(stderr, "[input] Started: '%s' ch%u vol=%.1f\n",
                dev_name.c_str(), channel + 1, g_input_volume.load());

        while (g_running.load() && !g_input_stop_req.load()) {
            {
                std::lock_guard<std::mutex> lk(g_input_mutex);
                if (g_input_start_req.pending) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        audio->stop();
        g_input_active.store(false);
        fprintf(stderr, "[input] Stopped\n");
    }
}
#endif

// ── WSOLA PLC (Waveform Similarity Overlap-Add Packet Loss Concealment) ──────
// Detects pitch period via autocorrelation on the previous good payload,
// then generates replacement samples by repeating that period with crossfade.
// Operates on S24 data stored as int32_t.
static void wsola_plc(const int32_t* prev, int32_t* out,
                       size_t frames, uint32_t channels) {
    const size_t total = frames * channels;
    if (!prev || total == 0) {
        std::memset(out, 0, total * sizeof(int32_t));
        return;
    }

    // Normalized autocorrelation pitch detection (channel 0)
    // Search: 1ms..20ms at 48kHz = 48..960 samples
    const size_t min_period = 48;
    const size_t max_period = std::min(static_cast<size_t>(960), frames / 2);
    const size_t analysis_len = std::min(frames, max_period * 2);

    size_t best_period = min_period;
    double best_corr = -1.0;

    if (analysis_len > min_period && max_period > min_period) {
        const size_t offset = (frames > analysis_len) ? (frames - analysis_len) : 0;
        for (size_t lag = min_period; lag <= max_period; lag++) {
            double sum = 0.0, energy_a = 0.0, energy_b = 0.0;
            size_t count = analysis_len - lag;
            if (count > 512) count = 512;
            for (size_t i = 0; i < count; i++) {
                double a = static_cast<double>(prev[(offset + i) * channels]);
                double b = static_cast<double>(prev[(offset + i + lag) * channels]);
                sum += a * b;
                energy_a += a * a;
                energy_b += b * b;
            }
            double denom = std::sqrt(energy_a * energy_b);
            double corr = (denom > 1.0) ? (sum / denom) : 0.0;
            if (corr > best_corr) {
                best_corr = corr;
                best_period = lag;
            }
        }
    }

    // Generate: repeat last period with cosine overlap-add at boundaries.
    // No fade-out — maintain full amplitude. PLC→real crossfade at ring
    // write level handles the transition back to real audio.
    const size_t period = best_period;
    const size_t cf_len = std::min(period / 4, static_cast<size_t>(24));
    const size_t tmpl = frames - period; // template start in prev

    for (size_t f = 0; f < frames; f++) {
        size_t pos = f % period;

        for (uint32_t ch = 0; ch < channels; ch++) {
            float sample = static_cast<float>(prev[(tmpl + pos) * channels + ch]);

            // Cosine overlap-add at each period wrap (after first cycle)
            if (f >= period && pos < cf_len && cf_len > 0) {
                float t = 0.5f * (1.0f - std::cos(3.14159265f
                    * static_cast<float>(pos) / static_cast<float>(cf_len)));
                // Outgoing: tail of previous cycle at (period - cf_len + pos)
                float tail = static_cast<float>(
                    prev[(tmpl + period - cf_len + pos) * channels + ch]);
                sample = tail * (1.0f - t) + sample * t;
            }

            out[f * channels + ch] = static_cast<int32_t>(sample);
        }
    }
}

// ── FEC constants for daemon integration ─────────────────────────────────────
static constexpr uint8_t  kFecGroupSize = 4;   // 4 data packets per FEC group
static constexpr uint16_t kNackPort     = 5005; // NACK feedback port = RTP + 1

// ── TX packet cache for NACK retransmission ──────────────────────────────────
struct TxPacketCache {
    static constexpr size_t kCacheSize = 256; // cache last 256 packets
    struct Entry {
        uint32_t sequence = 0;
        size_t   size = 0;
        uint8_t  data[1500];
    };
    Entry entries[kCacheSize];
    size_t write_pos = 0;

    void store(uint32_t seq, const uint8_t* pkt, size_t len) {
        auto& e = entries[write_pos % kCacheSize];
        e.sequence = seq;
        e.size = std::min(len, sizeof(e.data));
        std::memcpy(e.data, pkt, e.size);
        write_pos++;
    }

    const Entry* find(uint32_t seq) const {
        for (size_t i = 0; i < kCacheSize; i++) {
            if (entries[i].sequence == seq && entries[i].size > 0)
                return &entries[i];
        }
        return nullptr;
    }
};

static int run_tx(DaemonConfig cfg) {
    using namespace soluna;
    using namespace soluna::pal;
    using namespace soluna::pipeline;
    using namespace soluna::transport;

    // Auto-detect latency profile from network interface
    cfg.latency_profile = resolve_latency_profile(cfg.latency_profile, cfg.dest_ip);
    cfg.low_latency = (cfg.latency_profile == LatencyProfile::LowLatency ||
                       cfg.latency_profile == LatencyProfile::UltraLow);

    // Expose config so ws_handle can use it for monitor
    g_cfg_channels    = cfg.channels;
    g_cfg_sample_rate = cfg.sample_rate;
    g_cfg_port        = cfg.dest_port;
    snprintf(g_cfg_multicast, sizeof(g_cfg_multicast), "%s", cfg.dest_ip.c_str());
    g_mon_supported.store(true);

    // Restore persisted settings (volume, delay, mute, buffer)
    persist_config_load();

    // Auto-start multi-track recording if --record-dir was given
    if (!g_record_dir.empty()) {
        recording_start(g_record_dir);
    }

    // Spawn monitor management thread
    std::thread mon_thread(monitor_thread_fn, std::cref(cfg));

    // Spawn auto-tune thread (mic-based noise detection)
    std::thread tune_thread(tune_thread_fn, std::cref(cfg));
    if (cfg.auto_tune) {
        fprintf(stderr, "[tune] Auto-tune enabled (default ON)\n");
        g_tune_start_req.store(true);
    }

    soluna::control::WebSocketServer ws_srv;
    start_ws_server(ws_srv, cfg.https_enabled,
                    cfg.security.certificate_path, cfg.security.private_key_path);
#ifdef __APPLE__
    start_mdns_advertisement();
    start_soluna_volume_listener();
#endif

    // ── Start unicast relay for P2P peers ──
    std::thread relay_thread;
    if (cfg.relay_enabled) {
        g_relay_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (g_relay_sock >= 0) {
            int reuse = 1;
            setsockopt(g_relay_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            sockaddr_in bind_addr{};
            bind_addr.sin_family = AF_INET;
            bind_addr.sin_addr.s_addr = INADDR_ANY;
            bind_addr.sin_port = htons(cfg.relay_port);
            if (bind(g_relay_sock, (sockaddr*)&bind_addr, sizeof(bind_addr)) == 0) {
                // Set recv timeout so listener thread can check g_relay_running
                struct timeval tv{1, 0};
                setsockopt(g_relay_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                g_relay_running.store(true);
                relay_thread = std::thread(relay_listener_thread);
                fprintf(stderr, "[relay] Unicast relay listening on port %u\n", cfg.relay_port);
            } else {
                fprintf(stderr, "[relay] Failed to bind port %u: %s\n",
                        cfg.relay_port, strerror(errno));
                close(g_relay_sock);
                g_relay_sock = -1;
            }
        }
    }

    // ── Start WAN relay (forward TX to remote soluna-relay) ──
    if (!cfg.wan_relay_host.empty()) {
        wan_relay_init(cfg.wan_relay_host, cfg.wan_relay_port,
                       cfg.wan_relay_group, cfg.wan_relay_password);
    }

    // Apply latency profile
    const auto lp = get_latency_params(cfg.latency_profile);
    const uint32_t kFramesPerPacket = lp.frames_per_packet;
    const size_t frame_size = sizeof(int32_t) * cfg.channels; // S24 in 32-bit container

    if (cfg.latency_profile != LatencyProfile::Default) {
        g_buf_target_ms.store(lp.buf_target_ms);
        g_mon_target_ms.store(lp.mon_target_ms);
        fprintf(stderr, "[%s] TX: %u frames/pkt (%.1fms), buf_target=%ums, mon_target=%ums\n",
                lp.label, kFramesPerPacket,
                (float)kFramesPerPacket / cfg.sample_rate * 1000.0f,
                lp.buf_target_ms, lp.mon_target_ms);
    }

    // Opus encoder (TX)
#ifdef SOLUNA_HAS_OPUS
    std::unique_ptr<soluna::codec::OpusEncoder> opus_enc;
    if (cfg.codec == "opus") {
        soluna::codec::OpusEncoderConfig ocfg;
        ocfg.sample_rate = cfg.sample_rate;
        ocfg.channels = cfg.channels;
        ocfg.bitrate = 128000;
        ocfg.application = soluna::codec::OpusApplication::Audio;
        ocfg.frame_size_samples = kFramesPerPacket;
        ocfg.use_fec = true;             // Opus in-band FEC for packet loss resilience
        ocfg.packet_loss_pct = 5;        // hint: expect ~5% loss on WiFi
        opus_enc = std::make_unique<soluna::codec::OpusEncoder>(ocfg);
        fprintf(stderr, "[tx] Opus encoder: %u kbps, FEC=on, loss_hint=%d%%\n",
                ocfg.bitrate / 1000, ocfg.packet_loss_pct);
    }
#endif

    // PTP engine for clock synchronization (low-latency mode)
    std::unique_ptr<sync::PtpEngine> ptp;
    if (cfg.low_latency) {
        sync::PtpConfig ptp_cfg;
        ptp_cfg.log_sync_interval = -3; // 125ms sync
        ptp = std::make_unique<sync::PtpEngine>(ptp_cfg);
        if (ptp->start()) {
            fprintf(stderr, "[low-latency] PTP engine started (TX)\n");
        } else {
            fprintf(stderr, "[low-latency] PTP engine failed to start (continuing without PTP)\n");
            ptp.reset();
        }
    }

    // Ring buffer: 40 packets worth (200ms @ 48kHz) — enough to absorb SHM read jitter
    RingBuffer ring(kFramesPerPacket * 40, frame_size);

    // Audio device
    auto audio = AudioDevice::create();
    if (!audio) {
        fprintf(stderr, "Error: cannot create audio device\n");
        return 1;
    }

    AudioStreamConfig audio_cfg;
    audio_cfg.sample_rate = cfg.sample_rate;
    audio_cfg.channels = cfg.channels;
    audio_cfg.frames_per_buffer = kFramesPerPacket;
    audio_cfg.format = SampleFormat::S24_LE;

    // ── SHM (soluna virtual device) path ────────────────────────────────────
#ifdef __APPLE__
    const bool use_shm = (cfg.audio_device == "soluna");
#else
    const bool use_shm = false;
#endif

    // Speaker ring for local playback (SHM mode)
    constexpr size_t kSpeakerRingFrames = 4096;
    RingBuffer speaker_ring(kSpeakerRingFrames, sizeof(float) * cfg.channels);

    // SHM state (populated below if use_shm)
#ifdef __APPLE__
    SolunaShmMap shm_map{};
    std::atomic<bool> shm_open_ok{false};
    std::unique_ptr<AudioDevice> speaker_audio;
    std::thread shm_reader_thread;
    std::thread input_thread;

    if (use_shm) {
        // Open (or create) the SHM backing file.
        // We do NOT unlink first: if the CoreAudio driver already has this
        // file mmap-ed, removing + recreating it would give the driver an
        // orphan mapping.  Using O_RDWR|O_CREAT on the same path reuses the
        // existing inode, so the driver's live mmap sees the reset header.
        if (soluna_shm_open(&shm_map, O_RDWR | O_CREAT) != 0) {
            fprintf(stderr, "Error: cannot open Soluna SHM (%s): %s\n",
                            soluna_shm_path(), strerror(errno));
            return 1;
        }
        soluna_shm_init_header(&shm_map);
        if (soluna_shm_validate(&shm_map) != 0) {
            fprintf(stderr, "Error: Soluna SHM init failed\n");
            soluna_shm_close(&shm_map);
            return 1;
        }
        shm_open_ok.store(true);
        fprintf(stderr, "[solunad] SHM created (%s, %zu bytes)\n",
                soluna_shm_path(), (size_t)SOLUNA_SHM_BYTES);

        // ── Local speaker output (with timeout guard) ────────────────────────
        // CoreAudio AudioComponentInstanceNew can hang if coreaudiod is stuck
        // (e.g. after zombie processes). Run speaker init in a thread with timeout.
        std::atomic<bool> sp_prefilled{false};
        {
            std::atomic<bool> sp_init_done{false};
            std::thread sp_init_thread([&]() {
                auto sp_dev = AudioDevice::create();
                if (!sp_dev) {
                    fprintf(stderr, "Warning: cannot create speaker audio device\n");
                    sp_init_done.store(true);
                    return;
                }

                AudioStreamConfig sp_cfg;
                sp_cfg.sample_rate      = cfg.sample_rate;
                sp_cfg.channels         = cfg.channels;
                sp_cfg.frames_per_buffer = kFramesPerPacket;
                sp_cfg.format = SampleFormat::S24_LE;

                if (!sp_dev->open_output(cfg.local_speaker_device, sp_cfg)) {
                    fprintf(stderr, "Warning: cannot open speaker '%s'\n",
                            cfg.local_speaker_device.c_str());
                    sp_init_done.store(true);
                    return;
                }

                const size_t sp_channels = cfg.channels;
                const uint32_t sp_rate = cfg.sample_rate;
                sp_dev->start([&sp_prefilled, &speaker_ring, sp_channels,
                                      sp_rate](float* buf, uint32_t fc) {
                    static float prev_samples[8] = {};
                    static bool had_audio = false;
                    size_t samples = fc * sp_channels;
                    const uint32_t delay_frames = std::max(
                        fc * 4u,
                        g_speaker_delay_ms.load() * (sp_rate / 1000u));
                    if (!sp_prefilled.load()) {
                        if (speaker_ring.available_read() < delay_frames) {
                            std::memset(buf, 0, samples * sizeof(float));
                            return;
                        }
                        sp_prefilled.store(true);
                    }
                    if (speaker_ring.available_read() < fc) {
                        std::memset(buf, 0, samples * sizeof(float));
                        sp_prefilled.store(false);
                        g_mon_underruns.fetch_add(1, std::memory_order_relaxed);
                        had_audio = false;
                        return;
                    }
                    speaker_ring.read(reinterpret_cast<int32_t*>(buf), fc);
                    float gain = g_mon_muted.load() ? 0.0f : g_mon_volume.load();
                    if (gain != 1.0f) {
                        for (size_t i = 0; i < samples; i++) buf[i] *= gain;
                    }
                    if (g_repair_enabled.load(std::memory_order_relaxed)) {
                        uint32_t fixed = deglitch_buffer(buf, fc, (uint32_t)sp_channels);
                        if (fixed > 0)
                            g_repair_clicks.fetch_add(fixed, std::memory_order_relaxed);
                        crossfade_boundary(buf, fc, (uint32_t)sp_channels,
                                           prev_samples, &had_audio);
                    }
                });
                speaker_audio = std::move(sp_dev);
                fprintf(stderr, "[speaker] Local speaker '%s' opened\n",
                       cfg.local_speaker_device.empty()
                           ? "(default)" : cfg.local_speaker_device.c_str());
                sp_init_done.store(true);
            });

            // Wait up to 3 seconds for speaker init
            for (int i = 0; i < 30 && !sp_init_done.load(); i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!sp_init_done.load()) {
                fprintf(stderr, "Warning: speaker init timed out (coreaudiod stuck?), skipping\n");
                sp_init_thread.detach(); // let it finish in background
            } else {
                sp_init_thread.join();
            }
        }

        // ── SHM reader thread ───────────────────────────────────────────────
        // Reads float32 frames from SHM, converts to S24 for TX ring,
        // and also feeds the speaker ring.
        shm_reader_thread = std::thread([&]() {
            const uint32_t kReadChunk = kFramesPerPacket; // align to TX packet size
            std::vector<float>   flt_buf(kReadChunk * cfg.channels);
            std::vector<int32_t> s24_buf(kReadChunk * cfg.channels);

            while (g_running.load()) {
                uint32_t avail = (uint32_t)soluna_shm_available_read(&shm_map);
                if (avail < kReadChunk) {
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                    continue;
                }
                uint32_t rd = soluna_shm_read(&shm_map, flt_buf.data(), kReadChunk);
                if (rd == 0) continue;

                // Update latency metrics
                g_lat_shm_frames.store((uint32_t)soluna_shm_available_read(&shm_map));
                g_lat_tx_ring_frames.store((uint32_t)ring.available_read());
                g_lat_spk_ring_frames.store((uint32_t)speaker_ring.available_read());

                // float32 → S24 → TX ring
                float_to_s24(flt_buf.data(), s24_buf.data(), rd * cfg.channels);
                ring.write(s24_buf.data(), rd);

                // float32 → browser audio (batched S16LE over WebSocket binary)
                // Accumulate 960 frames (~20ms at 48kHz) before sending to reduce WS overhead
                if (g_audio_streaming.load() && g_ws_server_ptr) {
                    constexpr uint32_t kWsChunkFrames = 960;
                    thread_local std::vector<int16_t> ws_accum;
                    thread_local uint32_t ws_accum_frames = 0;
                    size_t prev = ws_accum.size();
                    ws_accum.resize(prev + rd * cfg.channels);
                    for (uint32_t i = 0; i < rd * cfg.channels; i++)
                        ws_accum[prev + i] = static_cast<int16_t>(flt_buf[i] * 32767.0f);
                    ws_accum_frames += rd;
                    if (ws_accum_frames >= kWsChunkFrames) {
                        g_ws_server_ptr->broadcast_binary(
                            reinterpret_cast<const uint8_t*>(ws_accum.data()),
                            ws_accum.size() * sizeof(int16_t));
                        ws_accum.clear();
                        ws_accum_frames = 0;
                    }
                }

                // Multi-track recording: TX track (SHM path)
                if (g_rec_active.load(std::memory_order_relaxed) && g_rec_tx.is_open()) {
                    thread_local std::vector<int16_t> mt_shm_buf;
                    mt_shm_buf.resize(rd * cfg.channels);
                    for (uint32_t i = 0; i < rd * cfg.channels; i++)
                        mt_shm_buf[i] = static_cast<int16_t>(flt_buf[i] * 32767.0f);
                    g_rec_tx.write(mt_shm_buf.data(), rd);
                }

                // float32 → speaker ring (stores float frames via int32_t alias)
                static_assert(sizeof(float) == sizeof(int32_t),
                              "float/int32_t size mismatch");
                speaker_ring.write(
                    reinterpret_cast<const int32_t*>(flt_buf.data()), rd);
            }
        });

        // ── Input passthrough thread (e.g. Babyface → SHM) ────────────────
        input_thread = std::thread(input_passthrough_thread_fn, &shm_map, cfg.channels);

        printf("solunad TX (SHM): Soluna.driver → %s:%u (%uHz, %uch)\n",
               cfg.dest_ip.c_str(), cfg.dest_port,
               cfg.sample_rate, cfg.channels);
    } else
#endif
    {
        // ── Normal audio input path ─────────────────────────────────────────
        if (!audio->open_input(cfg.audio_device, audio_cfg)) {
            // Try fallback sample rates: 48000 → 44100 → 96000
            static const uint32_t fallback_rates[] = {48000, 44100, 96000};
            bool opened = false;
            for (uint32_t rate : fallback_rates) {
                if (rate == cfg.sample_rate) continue;
                audio_cfg.sample_rate = rate;
                audio = AudioDevice::create();
                if (audio && audio->open_input(cfg.audio_device, audio_cfg)) {
                    fprintf(stderr, "TX: fallback to %uHz (requested %uHz)\n",
                            rate, cfg.sample_rate);
                    cfg.sample_rate = rate;  // Update config for packet scheduler
                    opened = true;
                    break;
                }
            }
            if (!opened) {
                fprintf(stderr, "Error: cannot open audio input device '%s'\n",
                        cfg.audio_device.c_str());
                return 1;
            }
        }

        // Conversion buffer (float from input → S24 for network)
        std::vector<int32_t> conv_buf(kFramesPerPacket * cfg.channels);

        // TX WAV recording (for quality comparison with RX)
        FILE* tx_wav_fp = nullptr;
        uint32_t tx_wav_data_bytes = 0;
        uint64_t tx_wav_frames_max = 0;
        uint64_t tx_wav_frames_written = 0;
        if (!cfg.record_tx_path.empty()) {
            tx_wav_fp = fopen(cfg.record_tx_path.c_str(), "wb");
            if (tx_wav_fp) {
                // Write placeholder header
                uint8_t hdr[44] = {};
                fwrite(hdr, 1, 44, tx_wav_fp);
                tx_wav_frames_max = (uint64_t)cfg.record_tx_duration * cfg.sample_rate;
                fprintf(stderr, "[tx] Recording to %s (%u seconds)\n",
                        cfg.record_tx_path.c_str(), cfg.record_tx_duration);
            } else {
                fprintf(stderr, "[tx] Cannot open WAV file '%s'\n", cfg.record_tx_path.c_str());
            }
        }

        // Audio callback: capture → convert → ring buffer
        audio->start([&](float* buffer, uint32_t frame_count) {
            size_t samples = frame_count * cfg.channels;
            float_to_s24(buffer, conv_buf.data(), samples);
            ring.write(conv_buf.data(), frame_count);

            // Record TX audio to WAV (S16LE)
            if (tx_wav_fp && tx_wav_frames_written < tx_wav_frames_max) {
                thread_local std::vector<int16_t> rec_buf;
                rec_buf.resize(samples);
                for (size_t i = 0; i < samples; i++)
                    rec_buf[i] = static_cast<int16_t>(buffer[i] * 32767.0f);
                size_t remaining = (size_t)(tx_wav_frames_max - tx_wav_frames_written);
                size_t to_write = frame_count < remaining ? frame_count : remaining;
                fwrite(rec_buf.data(), sizeof(int16_t) * cfg.channels, to_write, tx_wav_fp);
                tx_wav_data_bytes += (uint32_t)(to_write * cfg.channels * sizeof(int16_t));
                tx_wav_frames_written += to_write;
                if (tx_wav_frames_written >= tx_wav_frames_max) {
                    // Finalize WAV header
                    fseek(tx_wav_fp, 0, SEEK_SET);
                    uint32_t file_size = 36 + tx_wav_data_bytes;
                    uint16_t bits = 16;
                    uint16_t ch16 = (uint16_t)cfg.channels;
                    uint16_t block_align = ch16 * (bits / 8);
                    uint32_t byte_rate = cfg.sample_rate * block_align;
                    uint8_t h[44];
                    memcpy(h,      "RIFF", 4); memcpy(h+4,  &file_size, 4);
                    memcpy(h+8,    "WAVE", 4); memcpy(h+12, "fmt ", 4);
                    uint32_t fmt_sz = 16; memcpy(h+16, &fmt_sz, 4);
                    uint16_t pcm_fmt = 1; memcpy(h+20, &pcm_fmt, 2);
                    memcpy(h+22, &ch16, 2); memcpy(h+24, &cfg.sample_rate, 4);
                    memcpy(h+28, &byte_rate, 4); memcpy(h+32, &block_align, 2);
                    memcpy(h+34, &bits, 2);
                    memcpy(h+36, "data", 4); memcpy(h+40, &tx_wav_data_bytes, 4);
                    fwrite(h, 1, 44, tx_wav_fp);
                    fclose(tx_wav_fp);
                    tx_wav_fp = nullptr;
                    fprintf(stderr, "[tx] Recording complete: %llu frames\n",
                            (unsigned long long)tx_wav_frames_written);
                }
            }

            // Multi-track recording: TX track
            if (g_rec_active.load(std::memory_order_relaxed) && g_rec_tx.is_open()) {
                thread_local std::vector<int16_t> mt_buf;
                mt_buf.resize(samples);
                for (size_t i = 0; i < samples; i++)
                    mt_buf[i] = static_cast<int16_t>(buffer[i] * 32767.0f);
                g_rec_tx.write(mt_buf.data(), frame_count);
            }

            // Browser audio streaming (batched, same as SHM path)
            if (g_audio_streaming.load() && g_ws_server_ptr) {
                constexpr uint32_t kWsChunkFrames = 960;
                thread_local std::vector<int16_t> ws_accum;
                thread_local uint32_t ws_accum_frames = 0;
                size_t prev = ws_accum.size();
                ws_accum.resize(prev + samples);
                for (size_t i = 0; i < samples; i++)
                    ws_accum[prev + i] = static_cast<int16_t>(buffer[i] * 32767.0f);
                ws_accum_frames += frame_count;
                if (ws_accum_frames >= kWsChunkFrames) {
                    g_ws_server_ptr->broadcast_binary(
                        reinterpret_cast<const uint8_t*>(ws_accum.data()),
                        ws_accum.size() * sizeof(int16_t));
                    ws_accum.clear();
                    ws_accum_frames = 0;
                }
            }
        });
    }

    // Create transport manager for optional DTLS
    TransportManager transport_mgr(cfg.security);

    // Create transport socket
    std::unique_ptr<TransportSocket> socket;
    if (cfg.security.dtls_enabled) {
        printf("solunad: DTLS encryption enabled\n");
        SocketAddress dest{cfg.dest_ip, cfg.dest_port};
        socket = transport_mgr.establish_secure_channel(dest);
        if (!socket) {
            fprintf(stderr, "Error: DTLS handshake failed\n");
            return 1;
        }
    } else {
        socket = transport_mgr.create_tx_socket();
        if (!socket) {
            fprintf(stderr, "Error: cannot create transport socket\n");
            return 1;
        }
    }
    socket->set_dscp(46);

    SocketAddress dest{cfg.dest_ip, cfg.dest_port};

    // TX loop — packet tier from latency profile
    PacketScheduler scheduler(lp.packet_tier, cfg.sample_rate);
    scheduler.reset();

    std::vector<uint8_t> packet_buf(kMaxPacketSize);
    std::vector<int32_t> audio_buf(kFramesPerPacket * cfg.channels);
    uint64_t sequence = 0;
    uint32_t rtp_timestamp = 0;
    uint32_t media_ts = 0;

    const char* mode_str = cfg.aes67_mode ? "AES67" : "OSTP";
    const char* security_str = cfg.security.dtls_enabled ? " [DTLS]" : "";
    if (!use_shm) {
        printf("solunad TX (%s%s): %s → %s:%u (%uHz, %uch)\n",
            mode_str, security_str,
            cfg.audio_device.c_str(), cfg.dest_ip.c_str(), cfg.dest_port,
            cfg.sample_rate, cfg.channels);
    }

    // ── WiFi reliability: FEC encoder + packet cache + NACK listener ────────
    const bool wifi_mode = (cfg.latency_profile == LatencyProfile::WiFi ||
                            cfg.latency_profile == LatencyProfile::Default);

    // FEC encoder (XOR parity, group_size=4)
    wifi::FecConfig fec_cfg;
    fec_cfg.mode = wifi::FecMode::XorParity;
    fec_cfg.group_size = kFecGroupSize;
    fec_cfg.parity_count = 1;
    fec_cfg.max_packet_size = kMaxPayloadSize;
    wifi::FecEncoder fec_encoder(fec_cfg);

    // TX packet cache for NACK retransmission
    TxPacketCache tx_cache;

    // NACK listener thread — listens for retransmission requests from RX
    std::atomic<bool> nack_stop{false};
    std::thread nack_thread;
    if (wifi_mode) {
        nack_thread = std::thread([&]() {
            auto nack_sock = transport_mgr.create_rx_socket();
            if (!nack_sock) return;
            uint16_t nack_port = cfg.dest_port + 1;
            if (!nack_sock->bind(nack_port)) {
                fprintf(stderr, "[nack] Cannot bind port %u\n", nack_port);
                return;
            }
            nack_sock->set_recv_timeout_ms(100);
            fprintf(stderr, "[nack] Listener started on port %u\n", nack_port);

            uint8_t nack_buf[64];
            while (!nack_stop.load()) {
                SocketAddress nack_src;
                int nr = nack_sock->recv_from(nack_buf, sizeof(nack_buf), nack_src);
                if (nr < 6) continue; // min: 2-byte magic + 4-byte seq

                // NACK format: 0x4E41 ("NA") + uint32_t missing_seq (network order)
                if (nack_buf[0] != 0x4E || nack_buf[1] != 0x41) continue;

                // Process multiple NACK entries (4 bytes each)
                for (int off = 2; off + 4 <= nr; off += 4) {
                    uint32_t missing_seq;
                    std::memcpy(&missing_seq, nack_buf + off, 4);
                    missing_seq = ntohl(missing_seq);

                    const auto* entry = tx_cache.find(missing_seq);
                    if (entry) {
                        socket->send_to(entry->data, entry->size, dest);
                    }
                }
            }
        });
    }

    if (wifi_mode) {
        fprintf(stderr, "[wifi] Duplicate TX + FEC(XOR,k=%u) + NACK enabled\n", kFecGroupSize);
    }

    // Set RT priority on TX thread in low-latency mode
    if (cfg.low_latency) {
        pal::Thread::set_realtime_priority();
    }

    // FEC parity packet buffer
    std::vector<uint8_t> fec_pkt_buf(kMaxPacketSize);

    while (g_running.load()) {
        scheduler.wait_next();

        if (ring.available_read() < kFramesPerPacket) {
            // Underrun: send silence instead of skipping to maintain timing
            std::memset(audio_buf.data(), 0, kFramesPerPacket * cfg.channels * sizeof(int32_t));
        } else {
            ring.read(audio_buf.data(), kFramesPerPacket);
        }

        // Use PTP-synchronized timestamps when available
        if (ptp && ptp->sync_info().synchronized) {
            int64_t ptp_ns = ptp->get_media_clock_ns();
            rtp_timestamp = sync::PtpEngine::media_clock_to_rtp_timestamp(ptp_ns, cfg.sample_rate);
            media_ts = static_cast<uint32_t>(ptp_ns & 0xFFFFFFFF);
        }

        size_t pkt_size = 0;

        if (cfg.aes67_mode) {
            // Build AES67-compatible RTP packet
            uint8_t payload_type = kPayloadTypeAES67_L24;
            size_t bytes_per_sample = 3; // 24-bit packed

            // Convert to big-endian 24-bit packed
            std::vector<uint8_t> payload(kFramesPerPacket * cfg.channels * bytes_per_sample);
            uint8_t* out = payload.data();
            for (size_t i = 0; i < kFramesPerPacket * cfg.channels; i++) {
                int32_t sample = audio_buf[i];
                *out++ = static_cast<uint8_t>((sample >> 16) & 0xFF);
                *out++ = static_cast<uint8_t>((sample >> 8) & 0xFF);
                *out++ = static_cast<uint8_t>(sample & 0xFF);
            }

            pkt_size = aes67_build_rtp_packet(
                packet_buf.data(), packet_buf.size(),
                cfg.ssrc, static_cast<uint16_t>(sequence & 0xFFFF),
                rtp_timestamp, payload_type,
                payload.data(), payload.size()
            );
        } else {
            // Build OSTP packet
            size_t payload_size = kFramesPerPacket * frame_size;
            uint16_t seq_lo = static_cast<uint16_t>(sequence & 0xFFFF);
            uint16_t seq_hi = static_cast<uint16_t>((sequence >> 16) & 0xFFFF);

            uint8_t payload_type = kPayloadTypePCM24;
            const void* send_payload = audio_buf.data();
            size_t send_payload_size = payload_size;

#ifdef SOLUNA_HAS_OPUS
            std::vector<uint8_t> opus_data;
            if (opus_enc) {
                std::vector<float> float_buf(kFramesPerPacket * cfg.channels);
                for (size_t i = 0; i < float_buf.size(); i++) {
                    float_buf[i] = static_cast<float>(audio_buf[i]) / 8388608.0f;
                }
                auto result = opus_enc->encode(float_buf.data(), kFramesPerPacket);
                if (result.success && !result.data.empty()) {
                    opus_data = std::move(result.data);
                    payload_type = kPayloadTypeOpus;
                    send_payload = opus_data.data();
                    send_payload_size = opus_data.size();
                }
            }
#endif

            pkt_size = ostp_build_packet(
                packet_buf.data(), packet_buf.size(),
                cfg.ssrc, seq_lo, rtp_timestamp,
                payload_type,
                cfg.stream_id, seq_hi, media_ts,
                send_payload, send_payload_size
            );
        }

        if (pkt_size > 0) {
            socket->send_to(packet_buf.data(), pkt_size, dest);

            // Forward to unicast relay peers (P2P)
            relay_forward(packet_buf.data(), pkt_size);

            // Forward to WAN relay server
            wan_relay_forward(packet_buf.data(), pkt_size);

            if (wifi_mode) {
                // ── Duplicate send (toggleable via Web UI) ──
                if (g_wifi_dup_send.load(std::memory_order_relaxed)) {
                    socket->send_to(packet_buf.data(), pkt_size, dest);
                }

                // ── Cache for NACK retransmission ──
                if (g_wifi_nack.load(std::memory_order_relaxed)) {
                    tx_cache.store(static_cast<uint32_t>(sequence), packet_buf.data(), pkt_size);
                }

                // ── FEC: send parity every kFecGroupSize packets ──
                if (g_wifi_fec.load(std::memory_order_relaxed)) {
                    size_t audio_payload_size = kFramesPerPacket * frame_size;
                    if (fec_encoder.feed(audio_buf.data(), audio_payload_size)) {
                        for (const auto& parity : fec_encoder.get_parity()) {
                            uint32_t fec_group = parity.fec_group_id;
                            size_t hdr = 6;
                            size_t fec_size = hdr + parity.data.size();
                            if (fec_size <= fec_pkt_buf.size()) {
                                fec_pkt_buf[0] = 0xFE;
                                fec_pkt_buf[1] = 0x43;
                                uint32_t gid_be = htonl(fec_group);
                                std::memcpy(fec_pkt_buf.data() + 2, &gid_be, 4);
                                std::memcpy(fec_pkt_buf.data() + hdr,
                                            parity.data.data(), parity.data.size());
                                socket->send_to(fec_pkt_buf.data(), fec_size, dest);
                            }
                        }
                    }
                }
            }
        }

        sequence++;
        // Only manually increment timestamps when PTP is not driving them
        if (!(ptp && ptp->sync_info().synchronized)) {
            rtp_timestamp += kFramesPerPacket;
            // Use wall-clock nanoseconds for media_timestamp
            // This enables NTP-based synchronized playback across all receivers
            struct timespec wall_ts;
            clock_gettime(CLOCK_REALTIME, &wall_ts);
            media_ts = static_cast<uint32_t>(
                (static_cast<uint64_t>(wall_ts.tv_sec) * 1'000'000'000ULL + wall_ts.tv_nsec)
                & 0xFFFFFFFF);
        }

        if (sequence % 1000 == 0) {
            g_packets.store(sequence);
            printf("%sTX: %lu packets sent",
                isatty(STDOUT_FILENO) ? "\r" : "\n",
                static_cast<unsigned long>(sequence));
            fflush(stdout);
        }
    }

    // Stop NACK listener
    nack_stop.store(true);
    if (nack_thread.joinable()) nack_thread.join();

    // Stop PTP engine
    if (ptp) ptp->stop();

    printf("\nTX stopped. Total packets: %lu\n", static_cast<unsigned long>(sequence));

#ifdef __APPLE__
    if (use_shm) {
        if (shm_reader_thread.joinable())
            shm_reader_thread.join();
        g_input_stop_req.store(true);
        if (input_thread.joinable())
            input_thread.join();
        if (speaker_audio)
            speaker_audio->stop();
        soluna_shm_close(&shm_map);
    } else
#endif
    {
        audio->stop();
    }

    g_tune_stop_req.store(true);
    tune_thread.join();
    g_mon_stop_req.store(true);
    mon_thread.join();

    // Stop relay
    g_relay_running.store(false);
    if (relay_thread.joinable()) relay_thread.join();
    if (g_relay_sock >= 0) { close(g_relay_sock); g_relay_sock = -1; }

    // Stop WAN relay
    wan_relay_shutdown();

    // Finalize multi-track recordings
    recording_stop();

    // Stop file player
    g_player_active.store(false);
    if (g_player_thread.joinable()) g_player_thread.join();
    g_player_ring_ptr = nullptr;

    return 0;
}

static int run_rx(DaemonConfig cfg) {
    // Auto-detect latency profile from network interface
    cfg.latency_profile = resolve_latency_profile(cfg.latency_profile, cfg.dest_ip);
    cfg.low_latency = (cfg.latency_profile == LatencyProfile::LowLatency ||
                       cfg.latency_profile == LatencyProfile::UltraLow);

    g_cfg_channels    = cfg.channels;
    g_cfg_sample_rate = cfg.sample_rate;
    g_cfg_port        = cfg.listen_port;
    snprintf(g_cfg_multicast, sizeof(g_cfg_multicast), "%s", cfg.dest_ip.c_str());
    using namespace soluna;
    using namespace soluna::pal;
    using namespace soluna::pipeline;
    using namespace soluna::transport;

    soluna::control::WebSocketServer ws_srv;
    start_ws_server(ws_srv, cfg.https_enabled,
                    cfg.security.certificate_path, cfg.security.private_key_path);
#ifdef __APPLE__
    start_mdns_advertisement();
#endif

    // Apply latency profile
    const auto lp = get_latency_params(cfg.latency_profile);
    const uint32_t kFramesPerPacket = lp.frames_per_packet;
    const size_t frame_size = sizeof(int32_t) * cfg.channels;

    // Ring buffer: large enough for WiFi jitter bursts (1+ second capacity)
    // WiFi can buffer-bloat 200-500ms of packets then deliver in burst.
    // Small ring → overflow → lost packets → underrun → noise cycle.
    const bool is_wifi = (cfg.latency_profile == LatencyProfile::WiFi ||
                          cfg.latency_profile == LatencyProfile::Default);
    const uint32_t kRingPackets = is_wifi ? 500 : 100;
    const uint32_t kPrefillPackets = lp.prefill_packets;
    const uint32_t kRefillThreshold = lp.refill_threshold;
    RingBuffer ring(kFramesPerPacket * kRingPackets, frame_size);
    g_player_ring_ptr = &ring;   // expose to file player
    std::atomic<bool> prefilled{false};

    // Opus decoder (RX) — lazily initialized on first Opus packet
#ifdef SOLUNA_HAS_OPUS
    std::unique_ptr<soluna::codec::OpusDecoder> opus_dec;
#endif

    // DSP chain (compressor, EQ, reverb — all bypassed by default)
    soluna::pipeline::DspChain dsp_chain;
    dsp_chain.init(cfg.sample_rate, cfg.channels, kFramesPerPacket);
    dsp_chain.add_plugin("Compressor", soluna::pipeline::create_compressor());
    dsp_chain.add_plugin("EQ", soluna::pipeline::create_eq());
    dsp_chain.add_plugin("Reverb", soluna::pipeline::create_reverb());
    dsp_chain.set_bypass("Compressor", true);
    dsp_chain.set_bypass("EQ", true);
    dsp_chain.set_bypass("Reverb", true);
    g_dsp_chain_ptr = &dsp_chain;

    // PTP engine + PlayoutBuffer (low-latency wired only)
    std::unique_ptr<sync::PtpEngine> ptp;
    std::unique_ptr<PlayoutBuffer> playout;
    if (cfg.latency_profile != LatencyProfile::Default) {
        g_buf_target_ms.store(lp.buf_target_ms);
        fprintf(stderr, "[%s] RX: %u frames/pkt (%.1fms), prefill=%u pkts, buf_target=%ums\n",
                lp.label, kFramesPerPacket,
                (float)kFramesPerPacket / cfg.sample_rate * 1000.0f,
                kPrefillPackets, lp.buf_target_ms);
    }
    if (cfg.low_latency) {
        // PTP + PlayoutBuffer only for wired low-latency
        sync::PtpConfig ptp_cfg;
        ptp_cfg.log_sync_interval = -3;
        ptp = std::make_unique<sync::PtpEngine>(ptp_cfg);
        if (ptp->start()) {
            fprintf(stderr, "[low-latency] PTP engine started (RX)\n");

            PlayoutBufferConfig pb_cfg;
            pb_cfg.capacity_packets = 64;
            pb_cfg.sample_rate = cfg.sample_rate;
            pb_cfg.channels = cfg.channels;
            pb_cfg.frame_size = sizeof(int32_t);
            pb_cfg.target_depth_packets = 2;
            pb_cfg.playout_delay_ns = 1'000'000; // 1ms
            pb_cfg.mode = g_stream_mode.load();
            playout = std::make_unique<PlayoutBuffer>(pb_cfg);
            fprintf(stderr, "[low-latency] PlayoutBuffer active (1ms playout delay)\n");
        } else {
            fprintf(stderr, "[low-latency] PTP engine failed (falling back to ring buffer)\n");
            ptp.reset();
        }
    }

    // Audio device
    auto audio = AudioDevice::create();
    if (!audio) {
        fprintf(stderr, "Error: cannot create audio device\n");
        return 1;
    }

    // ALSA period: use larger period for WiFi to reduce callback frequency.
    // Packets arrive as 96 frames into ring buffer, but ALSA pulls 480 frames (10ms) at a time.
    // This dramatically reduces scheduling sensitivity on Raspberry Pi over WiFi.
    const uint32_t kAlsaPeriod = (kFramesPerPacket <= 96) ? 480 : kFramesPerPacket;

    AudioStreamConfig audio_cfg;
    audio_cfg.sample_rate = cfg.sample_rate;
    audio_cfg.channels = cfg.channels;
    audio_cfg.frames_per_buffer = kAlsaPeriod;

    if (!audio->open_output(cfg.audio_device, audio_cfg)) {
        // Try fallback sample rates: 48000 → 44100 → 96000
        static const uint32_t fallback_rates[] = {48000, 44100, 96000};
        bool opened = false;
        for (uint32_t rate : fallback_rates) {
            if (rate == cfg.sample_rate) continue;
            audio_cfg.sample_rate = rate;
            audio = AudioDevice::create();
            if (audio && audio->open_output(cfg.audio_device, audio_cfg)) {
                fprintf(stderr, "RX: fallback to %uHz (requested %uHz)\n",
                        rate, cfg.sample_rate);
                cfg.sample_rate = rate;  // Update for downstream calculations
                opened = true;
                break;
            }
        }
        if (!opened) {
            fprintf(stderr, "Error: cannot open audio output device '%s'\n",
                    cfg.audio_device.c_str());
            return 1;
        }
    }
    // Update global rate in case fallback changed it
    g_cfg_sample_rate = cfg.sample_rate;

    // Conversion buffer (S24 from network → float for playback)
    std::vector<float> conv_buf(kAlsaPeriod * cfg.channels);

    // Audio callback: ring buffer (or PlayoutBuffer) → convert → playback
    static uint64_t sine_phase = 0;
    bool sine_test = (std::getenv("SOLUNA_SINE_TEST") != nullptr);
    audio->start([&](float* buffer, uint32_t frame_count) {
        size_t samples = frame_count * cfg.channels;

        if (sine_test) {
            // Debug: generate 440Hz sine wave to test ALSA output
            for (uint32_t i = 0; i < frame_count; i++) {
                float s = 0.5f * sinf(2.0f * 3.14159265f * 440.0f * sine_phase / 48000.0f);
                for (uint32_t ch = 0; ch < cfg.channels; ch++) {
                    buffer[i * cfg.channels + ch] = s;
                }
                sine_phase++;
            }
            return;
        }

        // PTP-synchronized PlayoutBuffer path (low-latency)
        if (playout && ptp && ptp->sync_info().synchronized) {
            int64_t ptp_ns = ptp->get_media_clock_ns();
            size_t frames_read = playout->read_frames(ptp_ns, buffer, frame_count, cfg.channels);
            if (frames_read == 0) {
                // Underrun in playout buffer — silence
                std::memset(buffer, 0, samples * sizeof(float));
            }
            // Apply volume / mute
            float gain = g_rx_muted.load() ? 0.0f : g_rx_volume.load();
            if (gain != 1.0f) {
                for (size_t i = 0; i < samples; i++) buffer[i] *= gain;
            }
            return;
        }

        // ── High-quality WiFi RingBuffer playback ─────────────────────
        // 1. Full prefill (500ms) → ring starts at target → WiFi jitter absorbed
        // 2. ASRC ±1 frame with linear interpolation (no clicks from dup/skip)
        // 3. Underrun → re-prefill with smooth fade-out/fade-in
        // 4. Crossfade at all callback boundaries (cosine window)
        static std::vector<int32_t> s24_buf;
        static std::vector<float> prev_good_buf;
        static std::vector<float> fade_in_buf;   // for smooth fade-in after prefill
        static bool rx_had_audio = false;
        static uint32_t consecutive_underruns = 0;
        static uint32_t fade_in_remaining = 0;    // frames left in fade-in ramp

        const size_t max_read = static_cast<size_t>(frame_count) + 20;
        if (s24_buf.size() < max_read * cfg.channels)
            s24_buf.resize(max_read * cfg.channels);
        if (prev_good_buf.size() < samples)
            prev_good_buf.resize(samples);
        if (fade_in_buf.size() < samples)
            fade_in_buf.resize(samples, 0.0f);

        // ── Prefill: wait for buf_target before starting ─────────────
        if (!prefilled.load()) {
            size_t prefill_target = static_cast<size_t>(
                g_buf_target_ms.load()) * (cfg.sample_rate / 1000u);
            if (prefill_target < static_cast<size_t>(cfg.sample_rate) / 10)
                prefill_target = static_cast<size_t>(cfg.sample_rate) / 10;
            if (ring.available_read() < prefill_target) {
                std::memset(buffer, 0, samples * sizeof(float));
                return;
            }
            prefilled.store(true);
            consecutive_underruns = 0;
            // Schedule fade-in: full callback (10ms) cosine ramp from silence
            fade_in_remaining = frame_count;
        }

        size_t avail = ring.available_read();
        size_t target_frames = static_cast<size_t>(
            g_buf_target_ms.load()) * (cfg.sample_rate / 1000u);
        if (target_frames < static_cast<size_t>(frame_count) * 3)
            target_frames = static_cast<size_t>(frame_count) * 3;

        // ── Overflow: bias ASRC to gradually drain (no hard discard) ──
        // Instead of discarding frames (which causes glitches), we
        // let the ASRC PI controller handle it naturally. The PI
        // controller will read extra frames per callback to bring
        // the buffer back to target. Only do an emergency drain if
        // the ring is critically full (>95%) to prevent total stall.
        size_t cap = ring.capacity();
        if (avail > cap * 19 / 20) {
            // Critical: ring about to wrap — discard to 50% to prevent data loss
            size_t drain_to = cap / 2;
            if (avail > drain_to) {
                ring.discard(avail - drain_to);
                avail = ring.available_read();
            }
        }

        // ── ASRC: Adriaensen DLL + buffer-level PI ────────────────────
        // Two-layer clock drift compensation:
        //
        // Layer 1: DLL (Delay-Locked Loop) — Adriaensen 2005
        //   Estimates the true ratio between TX and RX clocks from
        //   callback timestamps. Very narrow bandwidth (0.01 Hz) so
        //   the correction is smooth and inaudible. Handles steady-state
        //   clock drift (typically ±50 ppm).
        //
        // Layer 2: Buffer-level PI (very gentle)
        //   Corrects transient buffer excursions (WiFi jitter bursts)
        //   that the DLL can't handle because they're not clock drift.
        //   Much gentler than the old PI (Kp=0.01) — the DLL does the
        //   heavy lifting.
        //
        // Max ±4 frames/callback (±0.8% pitch at 480 frames).
        static soluna::sync::DriftDLL drift_dll;
        static bool dll_inited = false;
        static double asrc_frac = 0.0;  // fractional frame accumulator

        if (!dll_inited) {
            drift_dll.init(static_cast<double>(cfg.sample_rate),
                          frame_count, 0.01);  // 0.01 Hz bandwidth
            dll_inited = true;
        }

        // Get monotonic time for DLL
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double now_sec = static_cast<double>(ts.tv_sec)
                       + static_cast<double>(ts.tv_nsec) * 1e-9;
        double dll_ratio = drift_dll.update(now_sec);

        size_t want = frame_count;
        if (avail >= static_cast<size_t>(frame_count) && target_frames > 0) {
            // DLL-based drift correction (smooth, handles steady-state)
            double drift_adj = (dll_ratio - 1.0) * static_cast<double>(frame_count);

            // Gentle buffer-level PI for transient excursions
            double error = static_cast<double>(avail) - static_cast<double>(target_frames);
            double norm_error = error / static_cast<double>(target_frames);

            // Dead zone: ±5% of target
            constexpr double kDeadZone = 0.05;
            if (norm_error > -kDeadZone && norm_error < kDeadZone)
                norm_error = 0.0;
            else if (norm_error > 0.0)
                norm_error -= kDeadZone;
            else
                norm_error += kDeadZone;

            // Very gentle PI (the DLL handles most of the work)
            constexpr double kP = 0.01;
            double buf_adj = norm_error * kP;

            // Combine DLL drift + buffer PI
            asrc_frac += drift_adj + buf_adj;

            // Clamp total adjustment
            if (asrc_frac < -4.0) asrc_frac = -4.0;
            if (asrc_frac >  4.0) asrc_frac =  4.0;

            // Extract integer frame adjustment
            if (asrc_frac <= -1.0) {
                int adj = static_cast<int>(std::floor(asrc_frac));
                if (adj < -4) adj = -4;
                want = static_cast<size_t>(std::max(1, static_cast<int>(frame_count) + adj));
                asrc_frac -= adj;
            } else if (asrc_frac >= 1.0) {
                int adj = static_cast<int>(std::ceil(asrc_frac));
                if (adj > 4) adj = 4;
                want = static_cast<size_t>(static_cast<int>(frame_count) + adj);
                asrc_frac -= adj;
            }
        }

        // ── Underrun → re-prefill with smooth fade-out ───────────────
        if (avail < static_cast<size_t>(frame_count)) {
            prefilled.store(false);
            consecutive_underruns++;
            if (rx_had_audio && consecutive_underruns <= 5) {
                float base_fade = 1.0f - 0.2f * (consecutive_underruns - 1);
                if (base_fade < 0.0f) base_fade = 0.0f;
                for (size_t i = 0; i < samples; i++) {
                    float t = (float)i / (float)samples;
                    float env = base_fade * 0.5f * (1.0f + std::cos(3.14159265f * t));
                    buffer[i] = prev_good_buf[i] * env;
                }
            } else {
                std::memset(buffer, 0, samples * sizeof(float));
            }
            g_plc_frames.fetch_add(frame_count, std::memory_order_relaxed);
        } else {
            // ── Normal playback with cubic Hermite ASRC ─────────────
            size_t read = ring.read(s24_buf.data(), want);
            consecutive_underruns = 0;

            if (read == 0) {
                std::memset(buffer, 0, samples * sizeof(float));
            } else if (read == static_cast<size_t>(frame_count)) {
                // Exact match: no resampling needed
                s24_to_float(s24_buf.data(), buffer, frame_count * cfg.channels);
            } else if (read < 2) {
                s24_to_float(s24_buf.data(), buffer, cfg.channels);
                for (uint32_t f = 1; f < frame_count; f++)
                    for (uint32_t ch = 0; ch < cfg.channels; ch++)
                        buffer[f * cfg.channels + ch] = buffer[ch];
            } else {
                // Cubic Hermite (Catmull-Rom) resampling:
                // 4-point interpolation preserves waveform shape far better
                // than linear. First/last output = first/last input (no
                // boundary discontinuity between callbacks).
                static std::vector<float> asrc_src;
                size_t rd_samp = read * cfg.channels;
                if (asrc_src.size() < rd_samp) asrc_src.resize(rd_samp);
                s24_to_float(s24_buf.data(), asrc_src.data(), rd_samp);

                float ratio = static_cast<float>(read - 1)
                            / static_cast<float>(frame_count - 1);
                int32_t rd = static_cast<int32_t>(read);

                for (uint32_t f = 0; f < frame_count; f++) {
                    float src_pos = static_cast<float>(f) * ratio;
                    int32_t si = static_cast<int32_t>(src_pos);
                    float frac = src_pos - static_cast<float>(si);

                    // 4 sample indices with boundary clamping
                    int32_t i0 = (si > 0) ? si - 1 : 0;
                    int32_t i1 = si;
                    int32_t i2 = (si + 1 < rd) ? si + 1 : rd - 1;
                    int32_t i3 = (si + 2 < rd) ? si + 2 : rd - 1;

                    for (uint32_t ch = 0; ch < cfg.channels; ch++) {
                        float y0 = asrc_src[i0 * cfg.channels + ch];
                        float y1 = asrc_src[i1 * cfg.channels + ch];
                        float y2 = asrc_src[i2 * cfg.channels + ch];
                        float y3 = asrc_src[i3 * cfg.channels + ch];
                        // Catmull-Rom spline
                        float c1 = 0.5f * (y2 - y0);
                        float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
                        float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
                        float v = ((c3 * frac + c2) * frac + c1) * frac + y1;
                        // Soft clamp: prevent DAC clipping only
                        if (v > 1.0f) v = 1.0f;
                        else if (v < -1.0f) v = -1.0f;
                        buffer[f * cfg.channels + ch] = v;
                    }
                }

                // Only count real underreads as PLC (not ±4 ASRC corrections)
                if (read + 5 < static_cast<size_t>(frame_count))
                    g_plc_frames.fetch_add(frame_count - read, std::memory_order_relaxed);
            }
        }

        // ── Fade-in after prefill (prevent click on playback resume) ─
        if (fade_in_remaining > 0 && consecutive_underruns == 0) {
            uint32_t fade_start = (fade_in_remaining > frame_count)
                ? 0 : frame_count - fade_in_remaining;
            for (uint32_t f = 0; f < frame_count; f++) {
                float env = 1.0f;
                if (f < fade_in_remaining) {
                    // Cosine ramp: 0 → 1 over full fade-in duration
                    float progress = 1.0f - (float)(fade_in_remaining - f)
                                    / static_cast<float>(frame_count);
                    if (progress < 0.0f) progress = 0.0f;
                    env = 0.5f * (1.0f - std::cos(3.14159265f * progress));
                }
                for (uint32_t ch = 0; ch < cfg.channels; ch++)
                    buffer[f * cfg.channels + ch] *= env;
            }
            fade_in_remaining = (fade_in_remaining > frame_count)
                ? fade_in_remaining - frame_count : 0;
        }

        // ── DSP chain (compressor → EQ → reverb) ─────────────────────
        if (g_dsp_chain_ptr && consecutive_underruns == 0) {
            g_dsp_chain_ptr->process(buffer, frame_count);
        }

        // ── Volume / mute + soft limiter ─────────────────────────────
        // tanh soft limiter: prevents hard clipping when gain > 1.0
        // Knee at ±0.85: below = linear (transparent), above = tanh curve
        float gain = g_rx_muted.load() ? 0.0f : g_rx_volume.load();
        if (gain != 1.0f) {
            for (size_t i = 0; i < samples; i++) buffer[i] *= gain;
        }
        if (gain > 0.5f) {
            for (size_t i = 0; i < samples; i++) {
                float x = buffer[i];
                if (x > 0.85f)
                    buffer[i] = 0.85f + 0.15f * std::tanh((x - 0.85f) / 0.15f);
                else if (x < -0.85f)
                    buffer[i] = -0.85f - 0.15f * std::tanh((-x - 0.85f) / 0.15f);
            }
        }

        // Track audio state for fade-out on underrun
        // (Callback boundary crossfade REMOVED: ring data is continuous,
        //  PLC→real crossfade now handled at ring write level)
        rx_had_audio = (consecutive_underruns == 0);

        // Save good buffer for PLC (only when we had real audio)
        if (consecutive_underruns == 0)
            std::memcpy(prev_good_buf.data(), buffer, samples * sizeof(float));
    });

    // Create transport manager for optional DTLS
    TransportManager transport_mgr(cfg.security);

    // Create transport socket
    auto socket = transport_mgr.create_rx_socket();
    if (!socket) {
        fprintf(stderr, "Error: cannot create transport socket\n");
        return 1;
    }

    if (!socket->bind(cfg.listen_port)) {
        fprintf(stderr, "Error: cannot bind to port %u\n", cfg.listen_port);
        return 1;
    }
    socket->join_multicast(cfg.dest_ip);
    socket->set_recv_timeout_ms(lp.recv_timeout_ms);

    // ── Start WAN relay for RX (join group to receive remote audio) ──
    if (!cfg.wan_relay_host.empty()) {
        wan_relay_init(cfg.wan_relay_host, cfg.wan_relay_port,
                       cfg.wan_relay_group, cfg.wan_relay_password);
    }

    const char* mode_str = cfg.aes67_mode ? "AES67" : "Auto";
    const char* ll_str = cfg.low_latency ? " [LOW-LATENCY]" : "";
    const char* security_str = cfg.security.dtls_enabled ? " [DTLS]" : "";
    printf("solunad RX (%s%s%s): %s:%u → %s (%uHz, %uch)\n",
        mode_str, ll_str, security_str,
        cfg.dest_ip.c_str(), cfg.listen_port, cfg.audio_device.c_str(),
        cfg.sample_rate, cfg.channels);

    std::vector<uint8_t> recv_buf(kMaxPacketSize);
    std::vector<int32_t> audio_buf(kMaxPayloadSize / sizeof(int32_t));
    uint64_t packets_received = 0;
    uint64_t aes67_packets = 0;
    uint64_t ostp_packets = 0;
    uint64_t sequence_errors = 0;
    int32_t last_seq = -1;
    uint64_t fec_recoveries = 0;
    uint64_t duplicate_drops = 0;

    // PLC: previous packet payload for WSOLA interpolation
    std::vector<int32_t> prev_payload;
    uint32_t plc_consecutive = 0; // consecutive PLC insertions (max 3)
    bool prev_write_was_plc = false;   // last ring write was PLC data
    int32_t plc_last_frame[8] = {};    // last frame of PLC per channel (for crossfade)

    // ── WiFi reliability features ────────────────────────────────────────────
    const bool wifi_mode = (cfg.latency_profile == LatencyProfile::WiFi ||
                            cfg.latency_profile == LatencyProfile::Default);

    // FEC decoder (matches TX encoder config)
    wifi::FecConfig fec_cfg;
    fec_cfg.mode = wifi::FecMode::XorParity;
    fec_cfg.group_size = kFecGroupSize;
    fec_cfg.parity_count = 1;
    fec_cfg.max_packet_size = kMaxPayloadSize;
    wifi::FecDecoder fec_decoder(fec_cfg);
    uint64_t fec_data_seq_base = 0; // sequence of first packet in current tracking window

    // Adaptive jitter buffer state
    double jitter_ema_ms = 0.0;       // EMA of arrival jitter
    int64_t last_arrival_us = 0;      // last packet arrival time (microseconds)
    int64_t expected_interval_us = 0; // expected inter-packet interval
    const uint32_t base_buf_target_ms = lp.buf_target_ms;

    if (wifi_mode) {
        expected_interval_us = static_cast<int64_t>(kFramesPerPacket) * 1'000'000LL / cfg.sample_rate;
        fprintf(stderr, "[wifi] RX: Dedup + FEC(XOR,k=%u) + WSOLA-PLC + AdaptiveJitter + NACK\n",
                kFecGroupSize);
    }

    // NACK sender socket (reuses same socket type, sends to TX source)
    SocketAddress tx_addr; // populated on first received packet
    bool tx_addr_known = false;

    // Set RT priority on RX receive thread in low-latency mode
    if (cfg.low_latency) {
        pal::Thread::set_realtime_priority();
    }

    // Start WAN relay RX thread (receives audio from relay server)
    std::thread wan_rx_thread;
    if (!cfg.wan_relay_host.empty() && g_wan_relay_running.load()) {
        wan_rx_thread = std::thread(wan_relay_rx_thread, std::ref(ring), cfg.channels);
    }

    while (g_running.load()) {
        SocketAddress src;
        int n = socket->recv_from(recv_buf.data(), recv_buf.size(), src);
        if (n <= 0) continue;

        // Remember TX source address for NACK
        if (wifi_mode && !tx_addr_known) {
            tx_addr = src;
            tx_addr.port = cfg.listen_port + 1; // NACK port
            tx_addr_known = true;
        }

        // Check packet type
        if (static_cast<size_t>(n) < sizeof(RtpHeader)) continue;

        const RtpHeader* rtp_ptr = reinterpret_cast<const RtpHeader*>(recv_buf.data());
        bool is_aes67 = aes67_is_standard_packet(*rtp_ptr);

        size_t frames = 0;
        uint32_t full_seq = 0;

        // ── Handle FEC parity packets (raw UDP: 0xFE 0x43 + group_id + data) ─
        // First byte 0xFE = RTP version 3, so old receivers skip it.
        if (wifi_mode && n >= 6 && recv_buf[0] == 0xFE && recv_buf[1] == 0x43) {
            uint32_t fec_group;
            std::memcpy(&fec_group, recv_buf.data() + 2, 4);
            fec_group = ntohl(fec_group);
            const uint8_t* payload = recv_buf.data() + 6;
            size_t payload_size = static_cast<size_t>(n) - 6;
            fec_decoder.feed(fec_group, kFecGroupSize, true, payload, payload_size);
            packets_received++;
            continue; // FEC parity — don't process as audio
        }

        if (is_aes67) {
            // AES67 packet
            RtpHeader rtp;
            std::memcpy(&rtp, recv_buf.data(), sizeof(RtpHeader));

            uint16_t sequence = ntohs(rtp.sequence);
            full_seq = sequence;

            // ── Duplicate detection (toggleable) ─────────────────────────────
            if (wifi_mode && g_wifi_dedup.load(std::memory_order_relaxed) &&
                last_seq >= 0 &&
                static_cast<int32_t>(full_seq & 0xFFFF) == last_seq) {
                duplicate_drops++;
                packets_received++;
                continue; // skip duplicate
            }

            const uint8_t* payload = recv_buf.data() + sizeof(RtpHeader);
            size_t payload_size = static_cast<size_t>(n) - sizeof(RtpHeader);

            // Convert payload
            if (rtp.pt == kPayloadTypeAES67_L24) {
                size_t samples = payload_size / 3;
                for (size_t i = 0; i < samples && i < audio_buf.size(); i++) {
                    int32_t sample = (static_cast<int32_t>(payload[i * 3]) << 16)
                                   | (static_cast<int32_t>(payload[i * 3 + 1]) << 8)
                                   | static_cast<int32_t>(payload[i * 3 + 2]);
                    if (sample & 0x800000) sample |= 0xFF000000;
                    audio_buf[i] = sample;
                }
                frames = samples / cfg.channels;
            } else if (rtp.pt == kPayloadTypeAES67_L16) {
                size_t samples = payload_size / 2;
                const int16_t* src_samples = reinterpret_cast<const int16_t*>(payload);
                for (size_t i = 0; i < samples && i < audio_buf.size(); i++) {
                    int16_t be_sample = src_samples[i];
                    int16_t sample = static_cast<int16_t>((be_sample >> 8) | (be_sample << 8));
                    audio_buf[i] = static_cast<int32_t>(sample) << 8;
                }
                frames = samples / cfg.channels;
            }

            ring.write(audio_buf.data(), frames);

            // Also insert into PlayoutBuffer if active
            if (playout && frames > 0) {
                PlayoutPacket pp;
                pp.sequence = full_seq & 0xFFFF;
                pp.rtp_timestamp = ntohl(rtp.timestamp);
                pp.media_timestamp = pp.rtp_timestamp;
                pp.audio_data.assign(
                    reinterpret_cast<const uint8_t*>(audio_buf.data()),
                    reinterpret_cast<const uint8_t*>(audio_buf.data()) + frames * frame_size);
                pp.valid = true;
                playout->insert(pp);
            }

            aes67_packets++;
        } else {
            // OSTP packet
            RtpHeader rtp;
            OstpHeader ostp;
            const uint8_t* payload = nullptr;
            size_t payload_size = 0;

            int parse_rc = ostp_parse_packet(recv_buf.data(), static_cast<size_t>(n),
                                              rtp, ostp, payload, payload_size);

            if (parse_rc == -1) {
                continue; // format error — skip
            }

            full_seq = (static_cast<uint32_t>(ostp.sequence_ext) << 16) | rtp.sequence;

            // ── Duplicate detection (toggleable) ─────────────────────────────
            if (wifi_mode && g_wifi_dedup.load(std::memory_order_relaxed) &&
                last_seq >= 0 &&
                static_cast<int32_t>(full_seq & 0xFFFF) == last_seq) {
                duplicate_drops++;
                packets_received++;
                continue; // skip duplicate
            }

            if (parse_rc == -2) {
                // CRC mismatch — payload corrupted, apply PLC
                g_crc_errors.fetch_add(1, std::memory_order_relaxed);
                g_lost_packets.fetch_add(1, std::memory_order_relaxed);
                size_t plc_frames = kFramesPerPacket;
                bool crc_plc_done = false;
#ifdef SOLUNA_HAS_OPUS
                // Prefer Opus PLC for CRC failures too
                if (opus_dec) {
                    auto plc_result = opus_dec->decode_plc(plc_frames);
                    if (plc_result.success && plc_result.frames_decoded > 0) {
                        std::vector<int32_t> plc_buf(plc_result.frames_decoded * cfg.channels);
                        for (size_t i = 0; i < plc_result.frames_decoded * cfg.channels; i++)
                            plc_buf[i] = static_cast<int32_t>(plc_result.samples[i] * 8388608.0f);
                        ring.write(plc_buf.data(), plc_result.frames_decoded);
                        for (uint32_t ch = 0; ch < cfg.channels && ch < 8; ch++)
                            plc_last_frame[ch] = plc_buf[(plc_result.frames_decoded - 1) * cfg.channels + ch];
                        prev_write_was_plc = true;
                        crc_plc_done = true;
                    }
                }
#endif
                if (!crc_plc_done && !prev_payload.empty() && prev_payload.size() >= plc_frames * cfg.channels) {
                    std::vector<int32_t> plc_buf(plc_frames * cfg.channels);
                    wsola_plc(prev_payload.data(), plc_buf.data(), plc_frames, cfg.channels);
                    ring.write(plc_buf.data(), plc_frames);
                    for (uint32_t ch = 0; ch < cfg.channels && ch < 8; ch++)
                        plc_last_frame[ch] = plc_buf[(plc_frames - 1) * cfg.channels + ch];
                    prev_write_was_plc = true;
                } else if (!crc_plc_done) {
                    std::vector<int32_t> silence(plc_frames * cfg.channels, 0);
                    ring.write(silence.data(), plc_frames);
                    std::memset(plc_last_frame, 0, sizeof(plc_last_frame));
                    prev_write_was_plc = true;
                }
                g_plc_frames.fetch_add(plc_frames, std::memory_order_relaxed);
                prev_payload.clear();
                ostp_packets++;
            } else {
                // Valid packet — decode payload to PCM
#ifdef SOLUNA_HAS_OPUS
                if (rtp.pt == kPayloadTypeOpus) {
                    if (!opus_dec) {
                        soluna::codec::OpusDecoderConfig dcfg;
                        dcfg.sample_rate = cfg.sample_rate;
                        dcfg.channels = cfg.channels;
                        opus_dec = std::make_unique<soluna::codec::OpusDecoder>(dcfg);
                        fprintf(stderr, "[rx] Opus decoder initialized\n");
                    }
                    auto result = opus_dec->decode(payload, payload_size, kFramesPerPacket);
                    if (result.success) {
                        for (size_t i = 0; i < result.frames_decoded * cfg.channels; i++) {
                            audio_buf[i] = static_cast<int32_t>(result.samples[i] * 8388608.0f);
                        }
                        payload = reinterpret_cast<const uint8_t*>(audio_buf.data());
                        payload_size = result.frames_decoded * frame_size;
                    } else {
                        continue;
                    }
                }
#endif
                // Fill gaps BEFORE writing (correct audio ordering)
                frames = payload_size / frame_size;

                // ── Gap check + PLC: fill missing packets before current ──
                if (last_seq >= 0) {
                    int32_t cur_lo = static_cast<int32_t>(full_seq & 0xFFFF);
                    int32_t expected = (last_seq + 1) & 0xFFFF;
                    if (cur_lo != expected) {
                        int32_t gap = (cur_lo - expected + 0x10000) & 0xFFFF;
                        if (gap > 0 && gap <= 100) {
                            sequence_errors += gap;

                            // FEC recovery
                            bool fec_recovered = false;
                            if (wifi_mode && g_wifi_fec.load(std::memory_order_relaxed)) {
                                for (int32_t miss = 0; miss < gap && miss < 4; miss++) {
                                    uint32_t miss_seq = static_cast<uint32_t>((expected + miss) & 0xFFFF);
                                    uint32_t fec_group = miss_seq / kFecGroupSize;
                                    if (fec_decoder.can_recover(fec_group)) {
                                        auto recovered = fec_decoder.recover(fec_group);
                                        for (const auto& rpkt : recovered) {
                                            if (!rpkt.data.empty()) {
                                                size_t rec_frames = rpkt.data.size() / frame_size;
                                                ring.write(rpkt.data.data(), rec_frames);
                                                fec_recoveries++;
                                                fec_recovered = true;
                                            }
                                        }
                                    }
                                }

                                // NACK for missing sequences
                                if (!fec_recovered && tx_addr_known &&
                                    g_wifi_nack.load(std::memory_order_relaxed)) {
                                    uint8_t nack_buf[2 + 4 * 4];
                                    nack_buf[0] = 0x4E; // 'N'
                                    nack_buf[1] = 0x41; // 'A'
                                    size_t nack_len = 2;
                                    for (int32_t miss = 0; miss < gap && miss < 4; miss++) {
                                        uint32_t miss_seq = static_cast<uint32_t>((expected + miss) & 0xFFFF);
                                        uint32_t net_seq = htonl(miss_seq);
                                        std::memcpy(nack_buf + nack_len, &net_seq, 4);
                                        nack_len += 4;
                                    }
                                    socket->send_to(nack_buf, nack_len, tx_addr);
                                }
                            }

                            // PLC for unrecovered gaps
                            // Priority: Opus FEC > Opus PLC > WSOLA > silence
                            // Opus FEC: use current packet's embedded FEC to recover
                            // the immediately preceding lost frame (gap==1 only).
                            // Opus PLC: decoder internal state for concealment.
                            if (!fec_recovered) {
                                uint32_t plc_count = std::min(static_cast<uint32_t>(gap), 3u);
                                for (uint32_t p = 0; p < plc_count; p++) {
                                    g_lost_packets.fetch_add(1, std::memory_order_relaxed);
                                    size_t plc_fr = kFramesPerPacket;
                                    bool plc_done = false;

#ifdef SOLUNA_HAS_OPUS
                                    // Opus FEC decode: recover lost frame from current packet
                                    // Only works for gap==1 (immediately preceding frame)
                                    if (opus_dec && p == 0 && gap == 1 &&
                                        rtp.pt == kPayloadTypeOpus &&
                                        payload && payload_size > 0) {
                                        auto fec_result = opus_dec->decode_fec(
                                            payload, payload_size, plc_fr);
                                        if (fec_result.success && fec_result.frames_decoded > 0) {
                                            std::vector<int32_t> plc_buf(fec_result.frames_decoded * cfg.channels);
                                            for (size_t i = 0; i < fec_result.frames_decoded * cfg.channels; i++)
                                                plc_buf[i] = static_cast<int32_t>(fec_result.samples[i] * 8388608.0f);
                                            ring.write(plc_buf.data(), fec_result.frames_decoded);
                                            for (uint32_t ch = 0; ch < cfg.channels && ch < 8; ch++)
                                                plc_last_frame[ch] = plc_buf[(fec_result.frames_decoded - 1) * cfg.channels + ch];
                                            prev_write_was_plc = true;
                                            plc_consecutive++;
                                            plc_done = true;
                                        }
                                    }

                                    // Opus PLC: decode with NULL input (fallback)
                                    if (!plc_done && opus_dec && plc_consecutive < 3) {
                                        auto plc_result = opus_dec->decode_plc(plc_fr);
                                        if (plc_result.success && plc_result.frames_decoded > 0) {
                                            std::vector<int32_t> plc_buf(plc_result.frames_decoded * cfg.channels);
                                            for (size_t i = 0; i < plc_result.frames_decoded * cfg.channels; i++) {
                                                plc_buf[i] = static_cast<int32_t>(plc_result.samples[i] * 8388608.0f);
                                            }
                                            ring.write(plc_buf.data(), plc_result.frames_decoded);
                                            for (uint32_t ch = 0; ch < cfg.channels && ch < 8; ch++)
                                                plc_last_frame[ch] = plc_buf[(plc_result.frames_decoded - 1) * cfg.channels + ch];
                                            prev_write_was_plc = true;
                                            plc_consecutive++;
                                            plc_done = true;
                                        }
                                    }
#endif
                                    // WSOLA fallback (non-Opus streams)
                                    if (!plc_done &&
                                        g_wifi_wsola_plc.load(std::memory_order_relaxed) &&
                                        !prev_payload.empty() &&
                                        prev_payload.size() >= plc_fr * cfg.channels &&
                                        plc_consecutive < 3) {
                                        std::vector<int32_t> plc_buf(plc_fr * cfg.channels);
                                        wsola_plc(prev_payload.data(), plc_buf.data(),
                                                  plc_fr, cfg.channels);
                                        ring.write(plc_buf.data(), plc_fr);
                                        for (uint32_t ch = 0; ch < cfg.channels && ch < 8; ch++)
                                            plc_last_frame[ch] = plc_buf[(plc_fr - 1) * cfg.channels + ch];
                                        prev_write_was_plc = true;
                                        plc_consecutive++;
                                        plc_done = true;
                                    }

                                    // Last resort: silence
                                    if (!plc_done) {
                                        std::vector<int32_t> silence(plc_fr * cfg.channels, 0);
                                        ring.write(silence.data(), plc_fr);
                                        std::memset(plc_last_frame, 0, sizeof(plc_last_frame));
                                        prev_write_was_plc = true;
                                    }
                                    g_plc_frames.fetch_add(plc_fr, std::memory_order_relaxed);
                                }
                                if (static_cast<uint32_t>(gap) > 3) {
                                    prev_payload.clear();
                                    plc_consecutive = 0;
                                }
                            }
                        } else {
                            sequence_errors++;
                        }
                    } else {
                        plc_consecutive = 0;
                    }
                }

                // ── Write current packet (crossfade if after PLC) ──
                if (prev_write_was_plc && frames > 0) {
                    constexpr size_t kPclCfFrames = 16; // ~0.33ms at 48kHz
                    size_t cf = std::min(frames, kPclCfFrames);
                    std::vector<uint8_t> xfade(frames * frame_size);
                    std::memcpy(xfade.data(), payload, frames * frame_size);
                    auto* xf = reinterpret_cast<int32_t*>(xfade.data());
                    for (size_t f = 0; f < cf; f++) {
                        float t = 0.5f * (1.0f - std::cos(3.14159265f
                            * static_cast<float>(f + 1) / static_cast<float>(cf + 1)));
                        for (uint32_t ch = 0; ch < cfg.channels; ch++) {
                            size_t idx = f * cfg.channels + ch;
                            float plc_v = static_cast<float>(plc_last_frame[ch]);
                            float real_v = static_cast<float>(xf[idx]);
                            xf[idx] = static_cast<int32_t>(plc_v * (1.0f - t) + real_v * t);
                        }
                    }
                    ring.write(xfade.data(), frames);
                    prev_write_was_plc = false;
                } else {
                    ring.write(payload, frames);
                    prev_write_was_plc = false;
                }

                // Save payload for WSOLA PLC interpolation
                {
                    size_t samples = frames * cfg.channels;
                    if (prev_payload.size() != samples) prev_payload.resize(samples);
                    std::memcpy(prev_payload.data(), payload, samples * sizeof(int32_t));
                }

                // Feed to FEC decoder for recovery of future gaps
                if (wifi_mode && frames > 0) {
                    uint32_t fec_group = static_cast<uint32_t>(full_seq / kFecGroupSize);
                    uint8_t fec_index = static_cast<uint8_t>(full_seq % kFecGroupSize);
                    fec_decoder.feed(fec_group, fec_index, false, payload, payload_size);
                    fec_decoder.prune(16);
                }

                // Insert into PlayoutBuffer if active
                if (playout && frames > 0) {
                    PlayoutPacket pp;
                    pp.sequence = full_seq & 0xFFFF;
                    pp.rtp_timestamp = rtp.timestamp;
                    pp.media_timestamp = ostp.media_timestamp;
                    pp.audio_data.assign(payload, payload + payload_size);
                    pp.valid = true;
                    playout->insert(pp);
                }

                ostp_packets++;
            }
        }

        // ── Adaptive jitter buffer (toggleable) ──────────────────────────────
        if (wifi_mode && g_wifi_adaptive_jitter.load(std::memory_order_relaxed)) {
            auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (last_arrival_us > 0 && expected_interval_us > 0) {
                int64_t actual_interval = now_us - last_arrival_us;
                double jitter_ms = std::abs(actual_interval - expected_interval_us) / 1000.0;
                // EMA with alpha=0.05 (smooth), but jump up fast on spikes
                const double alpha = (jitter_ms > jitter_ema_ms) ? 0.3 : 0.02;
                jitter_ema_ms = jitter_ema_ms * (1.0 - alpha) + jitter_ms * alpha;
                // buf_target = max(base, jitter * 3), clamped to [base, base*2]
                // Never go below base (WiFi profile sets the safe minimum)
                uint32_t adaptive_target = static_cast<uint32_t>(
                    std::max(static_cast<double>(base_buf_target_ms), jitter_ema_ms * 3.0));
                uint32_t max_target = base_buf_target_ms * 2;
                if (max_target > 2000) max_target = 2000;
                if (adaptive_target > max_target) adaptive_target = max_target;
                g_buf_target_ms.store(adaptive_target);
            }
            last_arrival_us = now_us;
        }

        // No trim: let ring float freely. With 65536-frame capacity (1.3s),
        // WiFi bursts are absorbed without overflow. If ring somehow fills,
        // ring.write() returns 0 (packet dropped) — correct streaming behavior.

        // AES67 gap counting (OSTP gaps handled inline before ring write)
        if (is_aes67 && last_seq >= 0) {
            int32_t cur_lo = static_cast<int32_t>(full_seq & 0xFFFF);
            int32_t expected = (last_seq + 1) & 0xFFFF;
            if (cur_lo != expected) {
                int32_t gap = (cur_lo - expected + 0x10000) & 0xFFFF;
                if (gap > 0 && gap <= 100)
                    sequence_errors += gap;
                else
                    sequence_errors++;
            }
        }
        last_seq = static_cast<int32_t>(full_seq & 0xFFFF);
        packets_received++;

        if (packets_received % 200 == 0) {
            g_packets.store(packets_received);
            g_seq_errors.store(sequence_errors);
            g_buf_fill.store(ring.available_read());
            g_lat_rx_ring_frames.store((uint32_t)ring.available_read());
            g_buf_cap.store(ring.capacity());
        }

        if (packets_received % 1000 == 0) {
            printf("%sRX: %lu pkts, seq_err:%lu, dup:%lu, fec:%lu, jitter:%.1fms, buf:%ums, ring:%zu/%zu",
                isatty(STDOUT_FILENO) ? "\r" : "\n",
                static_cast<unsigned long>(packets_received),
                static_cast<unsigned long>(sequence_errors),
                static_cast<unsigned long>(duplicate_drops),
                static_cast<unsigned long>(fec_recoveries),
                jitter_ema_ms,
                g_buf_target_ms.load(),
                ring.available_read(), ring.capacity());
            fflush(stdout);
        }
    }

    // Stop WAN relay RX thread
    wan_relay_shutdown();
    if (wan_rx_thread.joinable()) wan_rx_thread.join();

    audio->stop();
    if (ptp) ptp->stop();
    printf("\nRX stopped. Packets: %lu (OSTP:%lu, AES67:%lu), Errors: %lu, Dup: %lu, FEC: %lu\n",
        static_cast<unsigned long>(packets_received),
        static_cast<unsigned long>(ostp_packets),
        static_cast<unsigned long>(aes67_packets),
        static_cast<unsigned long>(sequence_errors),
        static_cast<unsigned long>(duplicate_drops),
        static_cast<unsigned long>(fec_recoveries));
    g_dsp_chain_ptr = nullptr;
    return 0;
}

int main(int argc, char** argv) {
    DaemonConfig cfg;
    if (!parse_args(argc, argv, cfg)) {
        print_usage(argv[0]);
        return 1;
    }

    // Prevent multiple instances via lock file
    const char* lock_path = "/tmp/solunad.lock";
    int lock_fd = open(lock_path, O_CREAT | O_RDWR, 0600);
    if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "[solunad] Another instance is already running. Exiting.\n");
        close(lock_fd);
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("Soluna Daemon v%d.%d.%d\n",
        SOLUNA_VERSION_MAJOR, SOLUNA_VERSION_MINOR, SOLUNA_VERSION_PATCH);

    // License check
    {
        auto key = soluna::security::load_license_key();
        if (key.empty()) {
            printf("License: Free tier (up to 1,000 participants)\n");
        } else {
            auto info = soluna::security::validate_license_key(key);
            if (!info.valid) {
                fprintf(stderr, "License: invalid key\n");
            } else if (!soluna::security::is_license_active(info)) {
                fprintf(stderr, "License: expired (%s)\n", info.expires.c_str());
            } else {
                printf("License: %s — %s (up to %u participants, expires %s)\n",
                    soluna::security::tier_name(info.tier),
                    info.licensee.c_str(),
                    info.max_participants,
                    info.expires.empty() ? "never" : info.expires.c_str());
            }
        }
    }

    // Start tunnel if requested
    std::thread tun_thread;
    if (cfg.tunnel) {
        tun_thread = std::thread(tunnel_thread_fn);
        tun_thread.detach();
    }

    if (cfg.tx_mode) {
        return run_tx(cfg);
    } else {
        return run_rx(cfg);
    }
}
