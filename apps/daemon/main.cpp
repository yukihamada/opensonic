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
#include <soluna/pipeline/pipeline.h>
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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <arpa/inet.h>
#endif

#ifdef __APPLE__
#include "../plugin/soluna_shm.h"
#include <dns_sd.h>
#include <sys/select.h>
#endif

static std::atomic<bool>     g_running{true};
static std::atomic<float>    g_rx_volume{1.0f};
static std::atomic<bool>     g_rx_muted{false};
static std::atomic<uint64_t> g_packets{0};
static std::atomic<uint64_t> g_seq_errors{0};
static std::atomic<size_t>   g_buf_fill{0};
static std::atomic<size_t>   g_buf_cap{0};
static std::atomic<uint32_t> g_buf_target_ms{20};   // target jitter buffer

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
static std::atomic<uint64_t> g_mon_packets{0};
static std::atomic<uint32_t> g_mon_target_ms{20};
static std::atomic<uint32_t> g_speaker_delay_ms{0};  // extra delay to sync with receivers

#include <mutex>
struct MonitorReq { bool pending = false; std::string device; };
static std::mutex     g_mon_mutex;
static MonitorReq     g_mon_start_req;
static std::atomic<bool> g_mon_stop_req{false};

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
            "\\\"buf_target_ms\\\":%u}\"}",
            id,
            (unsigned long long)g_packets.load(),
            (unsigned long long)g_seq_errors.load(),
            g_buf_fill.load(), g_buf_cap.load(),
            (double)g_rx_volume.load(),
            g_rx_muted.load() ? "true" : "false",
            g_buf_target_ms.load());
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
                g_buf_target_ms.store(std::max(5u, std::min(500u, ms)));
            } catch (...) {}
        }
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    // ── Monitor commands (TX-mode only) ────────────────────────────────────
    } else if (cmd == "monitor.stats") {
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"success\":true,\"data\":"
            "\"{\\\"supported\\\":%s,\\\"running\\\":%s,"
            "\\\"volume\\\":%.3f,\\\"muted\\\":%s,"
            "\\\"packets\\\":%llu,\\\"delay_ms\\\":%u}\"}",
            id,
            g_mon_supported.load() ? "true" : "false",
            g_mon_active.load()    ? "true" : "false",
            (double)g_mon_volume.load(),
            g_mon_muted.load() ? "true" : "false",
            (unsigned long long)g_mon_packets.load(),
            g_speaker_delay_ms.load());
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
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "monitor.set_mute") {
        p = msg.find("\"muted\":");
        if (p != std::string::npos)
            g_mon_muted.store(msg.substr(p + 8, 4) == "true");
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "monitor.set_buffer") {
        p = msg.find("\"ms\":");
        if (p != std::string::npos)
            try { g_mon_target_ms.store(std::max(5u, std::min(500u, (uint32_t)std::stoul(msg.substr(p + 5))))); } catch (...) {}
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
    } else if (cmd == "monitor.set_delay") {
        p = msg.find("\"ms\":");
        if (p != std::string::npos)
            try { g_speaker_delay_ms.store(std::min(500u, (uint32_t)std::stoul(msg.substr(p + 5)))); } catch (...) {}
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
    } else {
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"success\":false,\"data\":\"unknown command\"}", id);
    }
    return buf;
}

static void start_ws_server(soluna::control::WebSocketServer& srv) {
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

struct DaemonConfig {
    bool tx_mode = false;
    bool rx_mode = false;
    bool aes67_mode = false;
    bool tunnel = false;  // start cloudflared/ngrok tunnel
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
        "  --dtls            Enable DTLS encryption\n"
        "  --cert FILE       DTLS certificate file (PEM)\n"
        "  --key FILE        DTLS private key file (PEM)\n"
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

// ── Monitor thread (runs alongside TX, receives own multicast and plays locally) ──
static void monitor_thread_fn(const DaemonConfig& cfg) {
    using namespace soluna;
    using namespace soluna::pal;
    using namespace soluna::pipeline;
    using namespace soluna::transport;

    constexpr size_t kRingFrames    = 240 * 40;   // 200ms capacity
    constexpr uint32_t kFramesPerPkt = 240;
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
        socket->set_recv_timeout_ms(5);

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
                if (ostp_parse_packet(recv_buf.data(), n, rtp, ostp, pl, pl_sz)) {
                    frames = pl_sz / frame_size;
                    ring.write(pl, frames);
                }
            }

            if (frames > 0) {
                g_mon_packets.fetch_add(1);
            }
        }

        g_mon_active.store(false);
        audio->stop();
        printf("Monitor: stopped\n");
    }
}

static int run_tx(const DaemonConfig& cfg) {
    using namespace soluna;
    using namespace soluna::pal;
    using namespace soluna::pipeline;
    using namespace soluna::transport;

    // Expose config so ws_handle can use it for monitor
    g_cfg_channels    = cfg.channels;
    g_cfg_sample_rate = cfg.sample_rate;
    g_cfg_port        = cfg.dest_port;
    snprintf(g_cfg_multicast, sizeof(g_cfg_multicast), "%s", cfg.dest_ip.c_str());
    g_mon_supported.store(true);

    // Spawn monitor management thread
    std::thread mon_thread(monitor_thread_fn, std::cref(cfg));

    soluna::control::WebSocketServer ws_srv;
    start_ws_server(ws_srv);
#ifdef __APPLE__
    start_mdns_advertisement();
#endif

    constexpr uint32_t kFramesPerPacket = 240; // 5ms at 48kHz
    const size_t frame_size = sizeof(int32_t) * cfg.channels; // S24 in 32-bit container

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
                        return;
                    }
                    // Read float frames directly into output buffer
                    speaker_ring.read(reinterpret_cast<int32_t*>(buf), fc);
                    // Apply monitor gain
                    float gain = g_mon_muted.load() ? 0.0f : g_mon_volume.load();
                    if (gain != 1.0f) {
                        for (size_t i = 0; i < samples; i++) buf[i] *= gain;
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

                // float32 → S24 → TX ring
                float_to_s24(flt_buf.data(), s24_buf.data(), rd * cfg.channels);
                ring.write(s24_buf.data(), rd);

                // float32 → speaker ring (stores float frames via int32_t alias)
                static_assert(sizeof(float) == sizeof(int32_t),
                              "float/int32_t size mismatch");
                speaker_ring.write(
                    reinterpret_cast<const int32_t*>(flt_buf.data()), rd);
            }
        });

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

    // TX loop
    PacketScheduler scheduler(PacketTier::LAN, cfg.sample_rate);
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

    while (g_running.load()) {
        scheduler.wait_next();

        if (ring.available_read() < kFramesPerPacket) {
            continue; // underrun, skip
        }

        ring.read(audio_buf.data(), kFramesPerPacket);

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
        rtp_timestamp += kFramesPerPacket;
        media_ts += static_cast<uint32_t>(
            (static_cast<uint64_t>(kFramesPerPacket) * 1'000'000'000ULL) / cfg.sample_rate);

        if (sequence % 1000 == 0) {
            g_packets.store(sequence);
            printf("\rTX: %lu packets sent", static_cast<unsigned long>(sequence));
            fflush(stdout);
        }
    }

    printf("\nTX stopped. Total packets: %lu\n", static_cast<unsigned long>(sequence));

#ifdef __APPLE__
    if (use_shm) {
        if (shm_reader_thread.joinable())
            shm_reader_thread.join();
        if (speaker_audio)
            speaker_audio->stop();
        soluna_shm_close(&shm_map);
    } else
#endif
    {
        audio->stop();
    }

    g_mon_stop_req.store(true);
    mon_thread.join();
    return 0;
}

static int run_rx(const DaemonConfig& cfg) {
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

    constexpr uint32_t kFramesPerPacket = 240; // 5ms packets
    const size_t frame_size = sizeof(int32_t) * cfg.channels;

    // Ring buffer: 40 packets = 200ms total capacity
    constexpr uint32_t kRingPackets = 40;
    constexpr uint32_t kPrefillPackets = 4;  // pre-fill 20ms before starting playback
    constexpr uint32_t kRefillThreshold = 2; // re-prefill if buffer drops below 10ms
    RingBuffer ring(kFramesPerPacket * kRingPackets, frame_size);
    std::atomic<bool> prefilled{false};

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

    // Audio callback: ring buffer → convert → playback
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

        // Wait for prefill before starting playback to absorb jitter
        if (!prefilled.load()) {
            if (ring.available_read() < kFramesPerPacket * kPrefillPackets) {
                std::memset(buffer, 0, samples * sizeof(float));
                return;
            }
            prefilled.store(true);
        }

        std::vector<int32_t> s24_buf(samples);

        size_t read = ring.read(s24_buf.data(), frame_count);
        if (read < frame_count) {
            // Underrun: zero-fill and re-trigger prefill
            std::memset(s24_buf.data() + read * cfg.channels, 0,
                (frame_count - read) * frame_size);
            prefilled.store(false);
        } else if (ring.available_read() < kFramesPerPacket * kRefillThreshold) {
            // Buffer running low — re-prefill to avoid imminent dropout
            prefilled.store(false);
        }
        s24_to_float(s24_buf.data(), buffer, samples);

        // Apply volume / mute
        float gain = g_rx_muted.load() ? 0.0f : g_rx_volume.load();
        if (gain != 1.0f) {
            for (size_t i = 0; i < samples; i++) buffer[i] *= gain;
        }
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
    socket->set_recv_timeout_ms(10);

    const char* mode_str = cfg.aes67_mode ? "AES67" : "Auto";
    const char* security_str = cfg.security.dtls_enabled ? " [DTLS]" : "";
    printf("solunad RX (%s%s): %s:%u → %s (%uHz, %uch)\n",
        mode_str, security_str,
        cfg.dest_ip.c_str(), cfg.listen_port, cfg.audio_device.c_str(),
        cfg.sample_rate, cfg.channels);

    std::vector<uint8_t> recv_buf(kMaxPacketSize);
    std::vector<int32_t> audio_buf(kMaxPayloadSize / sizeof(int32_t));
    uint64_t packets_received = 0;
    uint64_t aes67_packets = 0;
    uint64_t ostp_packets = 0;
    uint64_t sequence_errors = 0;
    int32_t last_seq = -1;

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
            aes67_packets++;
        } else {
            // OSTP packet
            RtpHeader rtp;
            OstpHeader ostp;
            const uint8_t* payload = nullptr;
            size_t payload_size = 0;

            if (!ostp_parse_packet(recv_buf.data(), static_cast<size_t>(n),
                                   rtp, ostp, payload, payload_size)) {
                continue;
            }

            full_seq = (static_cast<uint32_t>(ostp.sequence_ext) << 16) | rtp.sequence;
            frames = payload_size / frame_size;
            ring.write(payload, frames);
            ostp_packets++;
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

        // Sequence check
        if (last_seq >= 0) {
            int32_t expected = (last_seq + 1) & 0xFFFF;
            if (static_cast<int32_t>(full_seq & 0xFFFF) != expected) {
                sequence_errors++;
            }
        }
        last_seq = static_cast<int32_t>(full_seq);
        packets_received++;

        if (packets_received % 200 == 0) {
            g_packets.store(packets_received);
            g_seq_errors.store(sequence_errors);
            g_buf_fill.store(ring.available_read());
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
