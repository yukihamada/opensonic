/**
 * soluna-rx — Soluna Linux/RPi CLI multicast receiver
 *
 * Receives RTP/OSTP audio from the Soluna multicast stream and outputs
 * to ALSA or stdout (pipe).  With --metrics, outputs JSON quality metrics
 * to stderr every few seconds for auto-optimization.
 *
 * Usage:
 *   soluna-rx [options]
 *
 * Options:
 *   --group <ip>      Multicast group (default: 239.69.0.1)
 *   --port <n>        UDP port (default: 5004)
 *   --peer <host:port> P2P unicast mode (connect to solunad relay)
 *   --relay <host:port> WAN relay mode (connect to soluna-relay server)
 *   --group-name <name> Group name for WAN relay (default: "default")
 *   --group-password <pw> Group password for WAN relay (optional)
 *   --channels <n>    Channel count (default: 2)
 *   --output alsa     Output to ALSA default device
 *   --output pipe     Output raw S16LE to stdout
 *   --device <name>   ALSA device name (default: default)
 *   --record <path>   Record received audio to WAV file
 *   --duration <sec>  Recording duration in seconds (default: 30)
 *   --metrics         Output JSON quality metrics every 5s
 *   --metrics-interval <sec>  Metrics interval (default: 5)
 *   --help            Show this message
 *
 * SPDX-License-Identifier: MIT
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>

#ifdef __linux__
#include <alsa/asoundlib.h>
#include <chrono>
#include <thread>
#endif

// ── RTP / OSTP headers (minimal, self-contained) ─────────────────────────────

struct RtpHeader {
    uint8_t  cc_x_p_v;   // V=2, P, X, CC
    uint8_t  m_pt;        // M, PT
    uint16_t sequence;
    uint32_t timestamp;
    uint32_t ssrc;
} __attribute__((packed));

struct OstpHeader {
    uint8_t  magic[4];    // 'O','S','T','P'
    uint16_t stream_id;
    uint16_t sequence_ext;
    uint32_t device_id;
    uint32_t flags;
} __attribute__((packed));

static constexpr uint32_t kOstpMagic = 0x4F535450; // 'OSTP'
static constexpr size_t   kMaxPktSize = 65536;

// ── WAV file writer (minimal, no external deps) ──────────────────────────────

struct WavWriter {
    FILE*    fp = nullptr;
    uint32_t data_bytes = 0;
    uint32_t channels = 2;
    uint32_t sample_rate = 48000;

    bool open(const char* path, uint32_t ch, uint32_t rate) {
        fp = fopen(path, "wb");
        if (!fp) return false;
        channels = ch;
        sample_rate = rate;
        data_bytes = 0;
        uint8_t hdr[44] = {};
        fwrite(hdr, 1, 44, fp);
        return true;
    }

    void write(const int16_t* buf, size_t frames) {
        if (!fp) return;
        size_t bytes = frames * channels * sizeof(int16_t);
        fwrite(buf, 1, bytes, fp);
        data_bytes += (uint32_t)bytes;
    }

    void close() {
        if (!fp) return;
        fseek(fp, 0, SEEK_SET);
        uint32_t file_size = 36 + data_bytes;
        uint16_t bits = 16;
        uint16_t block_align = (uint16_t)(channels * bits / 8);
        uint32_t byte_rate = sample_rate * block_align;
        uint8_t hdr[44];
        memcpy(hdr,      "RIFF", 4); memcpy(hdr+4,  &file_size, 4);
        memcpy(hdr+8,    "WAVE", 4); memcpy(hdr+12, "fmt ", 4);
        uint32_t fmt_size = 16; memcpy(hdr+16, &fmt_size, 4);
        uint16_t pcm_fmt = 1;  memcpy(hdr+20, &pcm_fmt, 2);
        uint16_t ch16 = (uint16_t)channels; memcpy(hdr+22, &ch16, 2);
        memcpy(hdr+24, &sample_rate, 4); memcpy(hdr+28, &byte_rate, 4);
        memcpy(hdr+32, &block_align, 2); memcpy(hdr+34, &bits, 2);
        memcpy(hdr+36, "data", 4); memcpy(hdr+40, &data_bytes, 4);
        fwrite(hdr, 1, 44, fp);
        fclose(fp);
        fp = nullptr;
    }
};

// ── Quality metrics (rolling window) ─────────────────────────────────────────

struct QualityMetrics {
    // Packet stats (per interval)
    uint64_t pkts_rx = 0;
    uint64_t pkts_drop = 0;

    // Audio analysis
    double   rms_sum = 0;      // sum of squared samples
    uint64_t rms_count = 0;    // total samples counted
    int16_t  peak = 0;         // peak absolute sample value

    // Click detection at packet boundaries only
    uint32_t clicks = 0;
    int16_t  last_packet_end = 0;  // last sample of previous packet

    // Dropout detection: consecutive low-energy windows
    uint32_t dropouts = 0;
    bool     in_dropout = false;
    uint32_t low_energy_frames = 0;

    // Underrun detection (ALSA)
    uint32_t underruns = 0;

    void analyze_audio(const int16_t* buf, size_t samples, uint32_t ch,
                        bool is_packet_start) {
        // Dropout threshold: -50dBFS ≈ 104 for S16
        constexpr int16_t kDropoutThresh = 104;
        constexpr uint32_t kDropoutMinFrames = 240; // 5ms @ 48kHz
        // Click threshold at packet boundary: 25% of full scale
        constexpr int kClickThresh = 8000;

        // RMS + peak
        for (size_t i = 0; i < samples; i++) {
            int16_t s = buf[i];
            rms_sum += (double)s * s;
            rms_count++;
            int16_t abs_s = (int16_t)(s < 0 ? -s : s);
            if (abs_s > peak) peak = abs_s;
        }

        // Click detection: only at packet boundaries (ch0)
        if (is_packet_start && samples >= ch) {
            int16_t first = buf[0]; // first sample of this packet
            int diff = (int)first - (int)last_packet_end;
            if (diff < 0) diff = -diff;
            if (diff > kClickThresh && last_packet_end != 0) {
                clicks++;
            }
        }
        // Remember last sample for next packet boundary check
        if (samples >= ch) {
            last_packet_end = buf[samples - ch]; // last ch0 sample
        }

        // Dropout detection: check if this chunk is very quiet
        double chunk_rms = 0;
        for (size_t i = 0; i < samples; i++) {
            chunk_rms += (double)buf[i] * buf[i];
        }
        chunk_rms = sqrt(chunk_rms / (double)samples);

        if (chunk_rms < kDropoutThresh) {
            low_energy_frames += (uint32_t)(samples / ch);
            if (!in_dropout && low_energy_frames > kDropoutMinFrames) {
                in_dropout = true;
                dropouts++;
            }
        } else {
            in_dropout = false;
            low_energy_frames = 0;
        }
    }

    void print_json(uint32_t sample_rate) {
        double rms = rms_count > 0 ? sqrt(rms_sum / rms_count) : 0;
        double rms_db = rms > 0 ? 20.0 * log10(rms / 32768.0) : -120.0;
        double peak_db = peak > 0 ? 20.0 * log10((double)peak / 32768.0) : -120.0;
        double loss_pct = (pkts_rx + pkts_drop) > 0
            ? 100.0 * pkts_drop / (pkts_rx + pkts_drop) : 0;

        fprintf(stderr,
            "{\"type\":\"metrics\","
            "\"pkts_rx\":%llu,\"pkts_drop\":%llu,\"loss_pct\":%.2f,"
            "\"rms_db\":%.1f,\"peak_db\":%.1f,"
            "\"clicks\":%u,\"dropouts\":%u,\"underruns\":%u}\n",
            (unsigned long long)pkts_rx, (unsigned long long)pkts_drop, loss_pct,
            rms_db, peak_db,
            clicks, dropouts, underruns);
        fflush(stderr);
    }

    void reset() {
        pkts_rx = 0;
        pkts_drop = 0;
        rms_sum = 0;
        rms_count = 0;
        peak = 0;
        clicks = 0;
        dropouts = 0;
        underruns = 0;
        in_dropout = false;
        low_energy_frames = 0;
    }
};

// ── Globals ───────────────────────────────────────────────────────────────────

static volatile bool g_running = true;

static void handle_signal(int) { g_running = false; }

static double now_sec() {
    struct timeval t;
    gettimeofday(&t, nullptr);
    return t.tv_sec + t.tv_usec * 1e-6;
}

// ── ALSA output ───────────────────────────────────────────────────────────────

#ifdef __linux__
static snd_pcm_t* alsa_open(const char* device, unsigned rate, unsigned channels) {
    snd_pcm_t* pcm = nullptr;
    if (snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        fprintf(stderr, "[rx] ALSA open '%s' failed\n", device);
        return nullptr;
    }
    snd_pcm_hw_params_t* hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm, hw, channels);
    snd_pcm_hw_params_set_rate(pcm, hw, rate, 0);
    snd_pcm_hw_params(pcm, hw);
    snd_pcm_prepare(pcm);
    return pcm;
}

static bool alsa_write_checked(snd_pcm_t* pcm, const int16_t* buf,
                                snd_pcm_uframes_t frames, uint32_t& underruns) {
    snd_pcm_sframes_t n = snd_pcm_writei(pcm, buf, frames);
    if (n == -EPIPE) {
        underruns++;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        snd_pcm_prepare(pcm);
        snd_pcm_writei(pcm, buf, frames);
        return false; // underrun occurred
    }
    return true;
}
#endif

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    std::string group    = "239.69.0.1";
    uint16_t    port     = 5004;
    uint32_t    channels = 2;
    uint32_t    rate     = 48000;
    bool        use_alsa = true;
    std::string alsa_dev = "default";
    std::string record_path;
    uint32_t    record_duration = 30;
    bool        metrics_enabled = false;
    uint32_t    metrics_interval = 5;
    std::string peer_host;       // P2P mode: solunad relay host
    uint16_t    peer_port = 5099; // P2P mode: solunad relay port
    std::string relay_host;      // WAN relay mode: relay server host
    uint16_t    relay_port = 5100; // WAN relay mode: relay server port
    std::string relay_group = "default"; // WAN relay: group name
    std::string relay_password;          // WAN relay: group password

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if      (a == "--group")    group    = next();
        else if (a == "--port")     port     = (uint16_t)atoi(next());
        else if (a == "--channels") channels = (uint32_t)atoi(next());
        else if (a == "--device")   alsa_dev = next();
        else if (a == "--record")   record_path = next();
        else if (a == "--duration") record_duration = (uint32_t)atoi(next());
        else if (a == "--metrics")  metrics_enabled = true;
        else if (a == "--metrics-interval") metrics_interval = (uint32_t)atoi(next());
        else if (a == "--peer") {
            std::string hp = next();
            auto colon = hp.rfind(':');
            if (colon != std::string::npos) {
                peer_host = hp.substr(0, colon);
                peer_port = (uint16_t)atoi(hp.substr(colon + 1).c_str());
            } else {
                peer_host = hp;
            }
        }
        else if (a == "--relay") {
            std::string hp = next();
            auto colon = hp.rfind(':');
            if (colon != std::string::npos) {
                relay_host = hp.substr(0, colon);
                relay_port = (uint16_t)atoi(hp.substr(colon + 1).c_str());
            } else {
                relay_host = hp;
            }
        }
        else if (a == "--group-name")     relay_group = next();
        else if (a == "--group-password") relay_password = next();
        else if (a == "--output") {
            std::string mode = next();
            if (mode == "alsa") {
                use_alsa = true;
            } else if (mode == "pipe") {
                use_alsa = false;
            } else {
                fprintf(stderr, "[rx] Unknown --output mode '%s'\n", mode.c_str());
                return 1;
            }
        }
        else if (a == "--help") {
            fprintf(stdout,
                "soluna-rx — Soluna multicast audio receiver\n\n"
                "  --group <ip>           Multicast group (default: 239.69.0.1)\n"
                "  --port <n>             UDP port        (default: 5004)\n"
                "  --peer <host:port>     P2P unicast via solunad relay (default port: 5099)\n"
                "  --relay <host:port>    WAN relay mode via soluna-relay (default port: 5100)\n"
                "  --group-name <name>    Group name for WAN relay (default: default)\n"
                "  --group-password <pw>  Group password for WAN relay (optional)\n"
                "  --channels <n>         Channels        (default: 2)\n"
                "  --output alsa          Output to ALSA  (default)\n"
                "  --output pipe          Output raw S16LE to stdout\n"
                "  --device <name>        ALSA device     (default: default)\n"
                "  --record <path>        Record to WAV   (for analysis)\n"
                "  --duration <sec>       Record duration  (default: 30)\n"
                "  --metrics              Output JSON quality metrics to stderr\n"
                "  --metrics-interval <s> Metrics interval (default: 5)\n"
            );
            return 0;
        }
    }

    // ── Open socket: WAN relay, P2P unicast, or multicast ────────────────
    bool peer_mode = !peer_host.empty();
    bool relay_mode = !relay_host.empty();
    sockaddr_in peer_addr{};
    sockaddr_in relay_addr{};

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (relay_mode) {
        // WAN relay mode: bind to any port, send JOIN to relay server
        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        bind_addr.sin_port = 0;
        if (bind(sock, (sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
            perror("bind"); close(sock); return 1;
        }
        relay_addr.sin_family = AF_INET;
        relay_addr.sin_port = htons(relay_port);
        if (inet_pton(AF_INET, relay_host.c_str(), &relay_addr.sin_addr) <= 0) {
            fprintf(stderr, "[rx] Invalid relay host: %s\n", relay_host.c_str());
            close(sock); return 1;
        }
        // Send JOIN message
        std::string join_msg = "JOIN:" + relay_group;
        if (!relay_password.empty()) join_msg += ":" + relay_password;
        join_msg += "\n";
        sendto(sock, join_msg.c_str(), join_msg.size(), 0,
               (sockaddr*)&relay_addr, sizeof(relay_addr));
        fprintf(stderr, "[rx] WAN relay mode: server=%s:%u group='%s'\n",
                relay_host.c_str(), relay_port, relay_group.c_str());
    } else if (peer_mode) {
        // P2P mode: bind to any port, resolve relay host
        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        bind_addr.sin_port = 0; // OS picks port
        if (bind(sock, (sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
            perror("bind"); close(sock); return 1;
        }
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_port = htons(peer_port);
        if (inet_pton(AF_INET, peer_host.c_str(), &peer_addr.sin_addr) <= 0) {
            fprintf(stderr, "[rx] Invalid peer host: %s\n", peer_host.c_str());
            close(sock); return 1;
        }
        // Send initial "hello" to register with relay
        const char hello[] = "hello";
        sendto(sock, hello, sizeof(hello), 0,
               (sockaddr*)&peer_addr, sizeof(peer_addr));
        fprintf(stderr, "[rx] P2P mode: relay=%s:%u\n", peer_host.c_str(), peer_port);
    } else {
        // Multicast mode
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("bind"); close(sock); return 1;
        }
        ip_mreq mreq{};
        inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = INADDR_ANY;
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            perror("IP_ADD_MEMBERSHIP"); close(sock); return 1;
        }
    }

    timeval tv{ 0, 100000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

#ifdef __linux__
    snd_pcm_t* pcm = nullptr;
    if (use_alsa) {
        pcm = alsa_open(alsa_dev.c_str(), rate, channels);
        if (!pcm) { close(sock); return 1; }
        fprintf(stderr, "[rx] ALSA output: %s  %uHz %uch\n", alsa_dev.c_str(), rate, channels);
    } else {
        fprintf(stderr, "[rx] Pipe output (raw S16LE %uHz %uch)\n", rate, channels);
    }
#else
    if (use_alsa) {
        fprintf(stderr, "[rx] ALSA not available on this platform; using pipe\n");
        use_alsa = false;
    }
#endif

    // WAV recording
    WavWriter wav;
    uint64_t record_frames_max = 0;
    uint64_t record_frames_written = 0;
    if (!record_path.empty()) {
        if (!wav.open(record_path.c_str(), channels, rate)) {
            fprintf(stderr, "[rx] Cannot open WAV file '%s'\n", record_path.c_str());
        } else {
            record_frames_max = (uint64_t)record_duration * rate;
            fprintf(stderr, "[rx] Recording to %s (%u seconds)\n",
                    record_path.c_str(), record_duration);
        }
    }

    // Quality metrics
    QualityMetrics metrics;
    double last_metrics_time = now_sec();

    if (relay_mode) {
        fprintf(stderr, "[rx] WAN relay: %s:%u group='%s'%s\n",
                relay_host.c_str(), relay_port, relay_group.c_str(),
                metrics_enabled ? " [metrics ON]" : "");
    } else if (peer_mode) {
        fprintf(stderr, "[rx] P2P relay: %s:%u%s\n", peer_host.c_str(), peer_port,
                metrics_enabled ? " [metrics ON]" : "");
    } else {
        fprintf(stderr, "[rx] Listening on %s:%u%s\n", group.c_str(), port,
                metrics_enabled ? " [metrics ON]" : "");
    }

    static uint8_t  pkt[kMaxPktSize];
    static int16_t  out_buf[8192];

    int32_t last_seq = -1;
    uint64_t total_rx = 0, total_drop = 0;
    double last_hello_time = now_sec(); // P2P heartbeat timer

    while (g_running) {
        // Heartbeat: send keepalive every 5s
        if (relay_mode) {
            double now = now_sec();
            if (now - last_hello_time >= 5.0) {
                const char hello[] = "HELLO\n";
                sendto(sock, hello, strlen(hello), 0,
                       (sockaddr*)&relay_addr, sizeof(relay_addr));
                last_hello_time = now;
            }
        } else if (peer_mode) {
            double now = now_sec();
            if (now - last_hello_time >= 5.0) {
                const char hello[] = "hello";
                sendto(sock, hello, sizeof(hello), 0,
                       (sockaddr*)&peer_addr, sizeof(peer_addr));
                last_hello_time = now;
            }
        }

        int n = (int)recv(sock, pkt, sizeof(pkt), 0);
        if (n <= 0) {
            // Check if it's time to emit metrics (even during silence)
            if (metrics_enabled) {
                double now = now_sec();
                if (now - last_metrics_time >= metrics_interval) {
                    metrics.print_json(rate);
                    metrics.reset();
                    last_metrics_time = now;
                }
            }
            continue;
        }
        if ((size_t)n < sizeof(RtpHeader)) continue;

        const RtpHeader* rtp = reinterpret_cast<const RtpHeader*>(pkt);

        // Detect OSTP vs plain RTP
        bool is_ostp = false;
        size_t payload_off = sizeof(RtpHeader);
        if ((size_t)n >= sizeof(RtpHeader) + sizeof(OstpHeader)) {
            const OstpHeader* ostp = reinterpret_cast<const OstpHeader*>(pkt + sizeof(RtpHeader));
            uint32_t magic;
            memcpy(&magic, ostp->magic, 4);
            if (magic == htonl(kOstpMagic)) {
                is_ostp = true;
                payload_off = sizeof(RtpHeader) + sizeof(OstpHeader);
            }
        }

        if (payload_off >= (size_t)n) continue;

        const uint8_t* payload   = pkt + payload_off;
        size_t         payload_n = (size_t)n - payload_off;

        // Sequence tracking (handles dup_send duplicates)
        uint16_t seq = ntohs(rtp->sequence);
        if (last_seq >= 0) {
            int diff = (int)(uint16_t)(seq - (uint16_t)last_seq);
            if (diff == 0) {
                // Duplicate packet (dup_send) — skip
                continue;
            }
            if (diff > 1 && diff < 100) {
                uint64_t lost = (uint64_t)(diff - 1);
                total_drop += lost;
                metrics.pkts_drop += lost;
            }
        }
        last_seq = seq;
        total_rx++;
        metrics.pkts_rx++;

        // Decode samples
        size_t frames;
        if (is_ostp) {
            frames = payload_n / (sizeof(int32_t) * channels);
            const int32_t* src = reinterpret_cast<const int32_t*>(payload);
            size_t samples = frames * channels;
            if (samples > sizeof(out_buf) / sizeof(int16_t)) samples = sizeof(out_buf) / sizeof(int16_t);
            for (size_t i = 0; i < samples; i++) {
                out_buf[i] = (int16_t)(src[i] >> 8);
            }
            frames = samples / channels;
        } else {
            frames = payload_n / (sizeof(int16_t) * channels);
            const int16_t* src = reinterpret_cast<const int16_t*>(payload);
            size_t samples = frames * channels;
            if (samples > sizeof(out_buf) / sizeof(int16_t)) samples = sizeof(out_buf) / sizeof(int16_t);
            for (size_t i = 0; i < samples; i++) {
                out_buf[i] = (int16_t)ntohs((uint16_t)src[i]);
            }
            frames = samples / channels;
        }

        if (frames == 0) continue;

        // Quality analysis
        if (metrics_enabled) {
            metrics.analyze_audio(out_buf, frames * channels, channels, true);

            double now = now_sec();
            if (now - last_metrics_time >= metrics_interval) {
                metrics.print_json(rate);
                metrics.reset();
                last_metrics_time = now;
            }
        }

        // Write to WAV if recording
        if (wav.fp && record_frames_written < record_frames_max) {
            size_t remaining = (size_t)(record_frames_max - record_frames_written);
            size_t to_write = frames < remaining ? frames : remaining;
            wav.write(out_buf, to_write);
            record_frames_written += to_write;
            if (record_frames_written >= record_frames_max) {
                wav.close();
                fprintf(stderr, "[rx] Recording complete: %llu frames\n",
                        (unsigned long long)record_frames_written);
            }
        }

        if (!use_alsa) {
            fwrite(out_buf, sizeof(int16_t) * channels, frames, stdout);
        } else {
#ifdef __linux__
            alsa_write_checked(pcm, out_buf, (snd_pcm_uframes_t)frames, metrics.underruns);
#endif
        }
    }

    if (wav.fp) {
        wav.close();
        fprintf(stderr, "[rx] Recording saved: %llu frames\n",
                (unsigned long long)record_frames_written);
    }

    fprintf(stderr, "\n[rx] Stopped. Received %llu packets, dropped %llu\n",
        (unsigned long long)total_rx, (unsigned long long)total_drop);

#ifdef __linux__
    if (pcm) { snd_pcm_drain(pcm); snd_pcm_close(pcm); }
#endif
    close(sock);
    return 0;
}
