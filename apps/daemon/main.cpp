/**
 * solunad — Soluna Network Audio Daemon
 *
 * Network audio daemon with YAML configuration support.
 * Usage:
 *   solunad --config /etc/soluna/config.yaml
 *   solunad --tx --device hw:0 --dest 239.69.0.1:5004
 *   solunad --rx --device hw:0 --port 5004
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/soluna.h>
#include <soluna/pal/audio.h>
#include <soluna/pal/net.h>
#include <soluna/pal/time.h>
#include <soluna/pal/thread.h>
#include <soluna/pipeline/ring_buffer.h>
#include <soluna/pipeline/playout_buffer.h>
#include <soluna/pipeline/pipeline.h>
#include <soluna/sync/ptp_engine.h>
#include <soluna/transport/ostp.h>
#include <soluna/transport/packet_scheduler.h>
#include <soluna/transport/transport_manager.h>
#include <soluna/transport/aes67.h>
#include <soluna/config/config.h>
#include <soluna/control/websocket_server.h>
#include "web_embedded.h"

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
#include <net/if.h>
#include <ifaddrs.h>
#include <sys/ioctl.h>
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
#include "../plugin/soluna_shm.h"
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
static std::atomic<float>    g_rx_volume{1.0f};
static std::atomic<bool>     g_rx_muted{false};
static std::atomic<uint64_t> g_packets{0};
static std::atomic<uint64_t> g_seq_errors{0};
static std::atomic<size_t>   g_buf_fill{0};
static std::atomic<size_t>   g_buf_cap{0};
static std::atomic<uint32_t> g_buf_target_ms{20};   // target jitter buffer

// PLC (Packet Loss Concealment) statistics
static std::atomic<uint64_t> g_crc_errors{0};
static std::atomic<uint64_t> g_plc_frames{0};
static std::atomic<uint64_t> g_lost_packets{0};

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

// ── Audio repair (declicker + crossfade) ─────────────────────────────────────
static std::atomic<uint64_t> g_repair_clicks{0};    // total clicks repaired
static std::atomic<uint64_t> g_repair_fades{0};     // total crossfades applied
static std::atomic<bool>     g_repair_enabled{true}; // enable/disable repair

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

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"speaker_delay_ms\":%u,"
        "\"monitor_volume\":%.3f,"
        "\"monitor_muted\":%s,"
        "\"monitor_buffer_ms\":%u,"
        "\"rx_delay_ms\":%u}\n",
        g_speaker_delay_ms.load(),
        (double)g_mon_volume.load(),
        g_mon_muted.load() ? "true" : "false",
        g_mon_target_ms.load(),
        g_rx_delay_ms.load());

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
            } catch (...) {}
        }
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
            "\\\"lost_packets\\\":%llu}\"}",
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
            (unsigned long long)g_lost_packets.load());
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
    } else {
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"success\":false,\"data\":\"unknown command\"}", id);
    }
    return buf;
}

static void start_ws_server(soluna::control::WebSocketServer& srv) {
    g_ws_server_ptr = &srv;
    srv.set_web_files(
        reinterpret_cast<const soluna::control::WebFile*>(embedded_web_files),
        embedded_web_file_count);
    srv.set_message_callback(ws_handle);
    if (srv.start(8400))
        printf("Web UI: http://localhost:8400\n");
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
        return {96, soluna::PacketTier::WiFi, 3, 2, 8, 10, 5, "wifi-latency"};
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
        "  --ultra-low       Ultra-low latency (~1ms, GbE only)\n"
        "  --low-latency     AES67-grade low latency (~2ms, wired LAN only)\n"
        "  --wifi-latency    WiFi optimized latency (~10ms, stable on WiFi)\n"
        "  --dtls            Enable DTLS encryption\n"
        "  --cert FILE       DTLS certificate file (PEM)\n"
        "  --key FILE        DTLS private key file (PEM)\n"
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
        } else if (arg == "--dtls") {
            cfg.security.dtls_enabled = true;
        } else if (arg == "--cert" && i + 1 < argc) {
            cfg.security.certificate_path = argv[++i];
        } else if (arg == "--key" && i + 1 < argc) {
            cfg.security.private_key_path = argv[++i];
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
        });

        g_mon_active.store(true);
        g_mon_stop_req.store(false);
        printf("Monitor: started on '%s'\n", dev_name.c_str());

        // --- Receive loop ---
        std::vector<uint8_t> recv_buf(kMaxPacketSize);
        std::vector<int32_t> audio_buf(kMaxPayloadSize / sizeof(int32_t));

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

    // Spawn monitor management thread
    std::thread mon_thread(monitor_thread_fn, std::cref(cfg));

    // Spawn auto-tune thread (mic-based noise detection)
    std::thread tune_thread(tune_thread_fn, std::cref(cfg));
    if (cfg.auto_tune) {
        fprintf(stderr, "[tune] Auto-tune enabled (default ON)\n");
        g_tune_start_req.store(true);
    }

    soluna::control::WebSocketServer ws_srv;
    start_ws_server(ws_srv);
#ifdef __APPLE__
    start_mdns_advertisement();
    start_soluna_volume_listener();
#endif

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

    // Ring buffer: 8 packets worth
    RingBuffer ring(kFramesPerPacket * 8, frame_size);

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

        // ── Local speaker output ────────────────────────────────────────────
        std::atomic<bool> sp_prefilled{false};
        speaker_audio = AudioDevice::create();
        if (!speaker_audio) {
            fprintf(stderr, "Warning: cannot create speaker audio device\n");
        } else {
            AudioStreamConfig sp_cfg;
            sp_cfg.sample_rate      = cfg.sample_rate;
            sp_cfg.channels         = cfg.channels;
            sp_cfg.frames_per_buffer = kFramesPerPacket;
            sp_cfg.format = SampleFormat::S24_LE; // driver uses float32 for output

            if (!speaker_audio->open_output(cfg.local_speaker_device, sp_cfg)) {
                fprintf(stderr, "Warning: cannot open speaker '%s'\n",
                        cfg.local_speaker_device.c_str());
                speaker_audio.reset();
            } else {
                const size_t sp_channels = cfg.channels;
                const uint32_t sp_rate = cfg.sample_rate;
                speaker_audio->start([&sp_prefilled, &speaker_ring, sp_channels,
                                      sp_rate](float* buf, uint32_t fc) {
                    // Repair state (persists across callbacks via static)
                    static float prev_samples[8] = {};
                    static bool had_audio = false;
                    size_t samples = fc * sp_channels;
                    // Prefill = max(4 callbacks, configured delay)
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
                    // speaker_ring stores float frames as raw bytes
                    // We borrow the int32_t ring interface but store floats
                    if (speaker_ring.available_read() < fc) {
                        std::memset(buf, 0, samples * sizeof(float));
                        sp_prefilled.store(false);
                        g_mon_underruns.fetch_add(1, std::memory_order_relaxed);
                        had_audio = false;
                        return;
                    }
                    // Read float frames directly into output buffer
                    speaker_ring.read(reinterpret_cast<int32_t*>(buf), fc);
                    // Apply monitor gain
                    float gain = g_mon_muted.load() ? 0.0f : g_mon_volume.load();
                    if (gain != 1.0f) {
                        for (size_t i = 0; i < samples; i++) buf[i] *= gain;
                    }
                    // Audio repair: declicker + crossfade
                    if (g_repair_enabled.load(std::memory_order_relaxed)) {
                        uint32_t fixed = deglitch_buffer(buf, fc, (uint32_t)sp_channels);
                        if (fixed > 0)
                            g_repair_clicks.fetch_add(fixed, std::memory_order_relaxed);
                        crossfade_boundary(buf, fc, (uint32_t)sp_channels,
                                           prev_samples, &had_audio);
                    }
                });
                printf("solunad SHM: local speaker '%s' opened\n",
                       cfg.local_speaker_device.empty()
                           ? "(default)" : cfg.local_speaker_device.c_str());
            }
        }

        // ── SHM reader thread ───────────────────────────────────────────────
        // Reads float32 frames from SHM, converts to S24 for TX ring,
        // and also feeds the speaker ring.
        shm_reader_thread = std::thread([&]() {
            constexpr uint32_t kReadChunk = 256; // frames per iteration
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
            fprintf(stderr, "Error: cannot open audio input device '%s'\n",
                    cfg.audio_device.c_str());
            return 1;
        }

        // Conversion buffer (float from input → S24 for network)
        std::vector<int32_t> conv_buf(kFramesPerPacket * cfg.channels);

        // Audio callback: capture → convert → ring buffer
        audio->start([&](float* buffer, uint32_t frame_count) {
            size_t samples = frame_count * cfg.channels;
            float_to_s24(buffer, conv_buf.data(), samples);
            ring.write(conv_buf.data(), frame_count);
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

    // Set RT priority on TX thread in low-latency mode
    if (cfg.low_latency) {
        pal::Thread::set_realtime_priority();
    }

    while (g_running.load()) {
        scheduler.wait_next();

        if (ring.available_read() < kFramesPerPacket) {
            continue; // underrun, skip
        }

        ring.read(audio_buf.data(), kFramesPerPacket);

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

            pkt_size = ostp_build_packet(
                packet_buf.data(), packet_buf.size(),
                cfg.ssrc, seq_lo, rtp_timestamp,
                kPayloadTypePCM24,
                cfg.stream_id, seq_hi, media_ts,
                audio_buf.data(), payload_size
            );
        }

        if (pkt_size > 0) {
            socket->send_to(packet_buf.data(), pkt_size, dest);
        }

        sequence++;
        // Only manually increment timestamps when PTP is not driving them
        if (!(ptp && ptp->sync_info().synchronized)) {
            rtp_timestamp += kFramesPerPacket;
            media_ts += static_cast<uint32_t>(
                (static_cast<uint64_t>(kFramesPerPacket) * 1'000'000'000ULL) / cfg.sample_rate);
        }

        if (sequence % 1000 == 0) {
            g_packets.store(sequence);
            printf("\rTX: %lu packets sent", static_cast<unsigned long>(sequence));
            fflush(stdout);
        }
    }

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
    start_ws_server(ws_srv);
#ifdef __APPLE__
    start_mdns_advertisement();
#endif

    // Apply latency profile
    const auto lp = get_latency_params(cfg.latency_profile);
    const uint32_t kFramesPerPacket = lp.frames_per_packet;
    const size_t frame_size = sizeof(int32_t) * cfg.channels;

    // Ring buffer: 40 packets capacity
    const uint32_t kRingPackets = 40;
    const uint32_t kPrefillPackets = lp.prefill_packets;
    const uint32_t kRefillThreshold = lp.refill_threshold;
    RingBuffer ring(kFramesPerPacket * kRingPackets, frame_size);
    std::atomic<bool> prefilled{false};

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

    AudioStreamConfig audio_cfg;
    audio_cfg.sample_rate = cfg.sample_rate;
    audio_cfg.channels = cfg.channels;
    audio_cfg.frames_per_buffer = kFramesPerPacket;

    if (!audio->open_output(cfg.audio_device, audio_cfg)) {
        fprintf(stderr, "Error: cannot open audio output device '%s'\n", cfg.audio_device.c_str());
        return 1;
    }

    // Conversion buffer (S24 from network → float for playback)
    std::vector<float> conv_buf(kFramesPerPacket * cfg.channels);

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

        // Standard RingBuffer path
        // State persisted across callbacks (no heap allocs in RT path)
        static std::vector<int32_t> s24_buf;
        static std::vector<float> prev_good_buf;
        static float rx_prev[8] = {};
        static bool rx_had_audio = false;
        static uint32_t consecutive_underruns = 0;

        if (s24_buf.size() < samples) s24_buf.resize(samples);
        if (prev_good_buf.size() < samples) prev_good_buf.resize(samples);

        // Wait for prefill before starting playback to absorb jitter
        if (!prefilled.load()) {
            if (ring.available_read() < kFramesPerPacket * kPrefillPackets) {
                std::memset(buffer, 0, samples * sizeof(float));
                return;
            }
            prefilled.store(true);
            consecutive_underruns = 0;
        }

        size_t read = ring.read(s24_buf.data(), frame_count);
        if (read < frame_count) {
            // Underrun: use previous good buffer with fade-out instead of silence
            consecutive_underruns++;
            if (rx_had_audio && consecutive_underruns <= 3) {
                float fade_base = 1.0f - 0.3f * (consecutive_underruns - 1);
                for (size_t i = 0; i < samples; i++) {
                    float fade = fade_base * (1.0f - (float)i / (float)samples);
                    buffer[i] = prev_good_buf[i] * fade;
                }
            } else {
                std::memset(buffer, 0, samples * sizeof(float));
            }
            if (consecutive_underruns > 5) prefilled.store(false);
            g_plc_frames.fetch_add(frame_count, std::memory_order_relaxed);
        } else {
            consecutive_underruns = 0;
            if (ring.available_read() < kFramesPerPacket * kRefillThreshold) {
                // Buffer running low — re-prefill to avoid imminent dropout
                prefilled.store(false);
            }
            s24_to_float(s24_buf.data(), buffer, samples);
        }

        // Apply volume / mute
        float gain = g_rx_muted.load() ? 0.0f : g_rx_volume.load();
        if (gain != 1.0f) {
            for (size_t i = 0; i < samples; i++) buffer[i] *= gain;
        }
        // Audio repair: deglitch + crossfade
        if (g_repair_enabled.load(std::memory_order_relaxed)) {
            uint32_t fixed = deglitch_buffer(buffer, frame_count, cfg.channels);
            if (fixed > 0)
                g_repair_clicks.fetch_add(fixed, std::memory_order_relaxed);
            crossfade_boundary(buffer, frame_count, cfg.channels,
                               rx_prev, &rx_had_audio);
        }
        // Save good buffer for PLC on next underrun
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

    // PLC: previous packet payload for interpolation
    std::vector<int32_t> prev_payload;
    uint32_t plc_consecutive = 0; // consecutive PLC insertions (max 3)

    // Set RT priority on RX receive thread in low-latency mode
    if (cfg.low_latency) {
        pal::Thread::set_realtime_priority();
    }

    while (g_running.load()) {
        SocketAddress src;
        int n = socket->recv_from(recv_buf.data(), recv_buf.size(), src);
        if (n <= 0) continue;

        // Check packet type
        if (static_cast<size_t>(n) < sizeof(RtpHeader)) continue;

        const RtpHeader* rtp_ptr = reinterpret_cast<const RtpHeader*>(recv_buf.data());
        bool is_aes67 = aes67_is_standard_packet(*rtp_ptr);

        size_t frames = 0;
        uint32_t full_seq = 0;

        if (is_aes67) {
            // AES67 packet
            RtpHeader rtp;
            std::memcpy(&rtp, recv_buf.data(), sizeof(RtpHeader));

            uint16_t sequence = ntohs(rtp.sequence);
            full_seq = sequence;

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

            if (parse_rc == -2) {
                // CRC mismatch — payload corrupted, apply PLC
                g_crc_errors.fetch_add(1, std::memory_order_relaxed);
                g_lost_packets.fetch_add(1, std::memory_order_relaxed);
                size_t plc_frames = kFramesPerPacket;
                if (!prev_payload.empty() && prev_payload.size() >= plc_frames * cfg.channels) {
                    // Fade out previous payload
                    std::vector<int32_t> plc_buf(plc_frames * cfg.channels);
                    for (size_t i = 0; i < plc_buf.size(); i++) {
                        float fade = 1.0f - static_cast<float>(i) / static_cast<float>(plc_buf.size());
                        plc_buf[i] = static_cast<int32_t>(static_cast<float>(prev_payload[i]) * fade);
                    }
                    ring.write(plc_buf.data(), plc_frames);
                } else {
                    // No previous data: insert silence
                    std::vector<int32_t> silence(plc_frames * cfg.channels, 0);
                    ring.write(silence.data(), plc_frames);
                }
                g_plc_frames.fetch_add(plc_frames, std::memory_order_relaxed);
                prev_payload.clear(); // Don't reuse faded data
                ostp_packets++;
            } else {
                // Valid packet — write to ring buffer
                frames = payload_size / frame_size;
                ring.write(payload, frames);

                // Save payload for PLC interpolation
                {
                    size_t samples = frames * cfg.channels;
                    if (prev_payload.size() != samples) prev_payload.resize(samples);
                    std::memcpy(prev_payload.data(), payload, samples * sizeof(int32_t));
                }

                // Also insert into PlayoutBuffer if active
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

        // Trim ring buffer to target latency (drain excess frames)
        {
            size_t avail = ring.available_read();
            uint32_t target = g_buf_target_ms.load() * (cfg.sample_rate / 1000u);
            if (avail > target) {
                static thread_local std::vector<int32_t> drain_buf(512);
                size_t excess = avail - target;
                while (excess > 0) {
                    size_t chunk = std::min(excess, drain_buf.size() / cfg.channels);
                    if (chunk == 0) break;
                    size_t dr = ring.read(drain_buf.data(), chunk);
                    if (dr == 0) break;
                    excess -= dr;
                }
            }
        }

        // Sequence check + PLC for gaps
        if (last_seq >= 0) {
            int32_t cur_lo = static_cast<int32_t>(full_seq & 0xFFFF);
            int32_t expected = (last_seq + 1) & 0xFFFF;
            if (cur_lo != expected) {
                // Sequence gap detected — count missing packets
                int32_t gap = (cur_lo - expected + 0x10000) & 0xFFFF;
                if (gap > 0 && gap <= 100) { // reasonable gap (not reordering wrap)
                    sequence_errors += gap;
                    // PLC: fill up to 3 missing packets
                    uint32_t plc_count = std::min(static_cast<uint32_t>(gap), 3u);
                    for (uint32_t p = 0; p < plc_count; p++) {
                        g_lost_packets.fetch_add(1, std::memory_order_relaxed);
                        size_t plc_fr = kFramesPerPacket;
                        if (!prev_payload.empty() && prev_payload.size() >= plc_fr * cfg.channels && plc_consecutive < 3) {
                            std::vector<int32_t> plc_buf(plc_fr * cfg.channels);
                            float base_fade = 1.0f - 0.3f * static_cast<float>(plc_consecutive);
                            for (size_t i = 0; i < plc_buf.size(); i++) {
                                float fade = base_fade * (1.0f - static_cast<float>(i) / static_cast<float>(plc_buf.size()));
                                plc_buf[i] = static_cast<int32_t>(static_cast<float>(prev_payload[i]) * fade);
                            }
                            ring.write(plc_buf.data(), plc_fr);
                            plc_consecutive++;
                        } else {
                            std::vector<int32_t> silence(plc_fr * cfg.channels, 0);
                            ring.write(silence.data(), plc_fr);
                        }
                        g_plc_frames.fetch_add(plc_fr, std::memory_order_relaxed);
                    }
                    if (static_cast<uint32_t>(gap) > 3) {
                        // Beyond 3 consecutive losses — clear prev for safety
                        prev_payload.clear();
                        plc_consecutive = 0;
                    }
                } else {
                    sequence_errors++;
                }
            } else {
                plc_consecutive = 0; // reset on successful receipt
            }
        }
        last_seq = static_cast<int32_t>(full_seq);
        packets_received++;

        if (packets_received % 200 == 0) {
            g_packets.store(packets_received);
            g_seq_errors.store(sequence_errors);
            g_buf_fill.store(ring.available_read());
                g_lat_rx_ring_frames.store((uint32_t)ring.available_read());
            g_buf_cap.store(ring.capacity());
        }

        if (packets_received % 1000 == 0) {
            printf("\rRX: %lu pkts (OSTP:%lu AES67:%lu), %lu seq errors, ring: %zu/%zu",
                static_cast<unsigned long>(packets_received),
                static_cast<unsigned long>(ostp_packets),
                static_cast<unsigned long>(aes67_packets),
                static_cast<unsigned long>(sequence_errors),
                ring.available_read(), ring.capacity());
            fflush(stdout);
        }
    }

    audio->stop();
    if (ptp) ptp->stop();
    printf("\nRX stopped. Packets: %lu (OSTP:%lu, AES67:%lu), Errors: %lu\n",
        static_cast<unsigned long>(packets_received),
        static_cast<unsigned long>(ostp_packets),
        static_cast<unsigned long>(aes67_packets),
        static_cast<unsigned long>(sequence_errors));
    return 0;
}

int main(int argc, char** argv) {
    DaemonConfig cfg;
    if (!parse_args(argc, argv, cfg)) {
        print_usage(argv[0]);
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("Soluna Daemon v%d.%d.%d\n",
        SOLUNA_VERSION_MAJOR, SOLUNA_VERSION_MINOR, SOLUNA_VERSION_PATCH);

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
