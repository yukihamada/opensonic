/**
 * soluna-rx — Soluna Linux/RPi CLI multicast receiver
 *
 * Receives RTP/OSTP audio from the Soluna multicast stream and outputs
 * to ALSA or stdout (pipe).
 *
 * Usage:
 *   soluna-rx [options]
 *
 * Options:
 *   --group <ip>      Multicast group (default: 239.69.0.1)
 *   --port <n>        UDP port (default: 5004)
 *   --channels <n>    Channel count (default: 2)
 *   --output alsa     Output to ALSA default device
 *   --output pipe     Output raw S16LE to stdout
 *   --device <name>   ALSA device name (default: default)
 *   --help            Show this message
 *
 * SPDX-License-Identifier: MIT
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// ── Globals ───────────────────────────────────────────────────────────────────

static volatile bool g_running = true;

static void handle_signal(int) { g_running = false; }

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

static void alsa_write(snd_pcm_t* pcm, const int16_t* buf, snd_pcm_uframes_t frames) {
    snd_pcm_sframes_t n = snd_pcm_writei(pcm, buf, frames);
    if (n == -EPIPE) {
        // Underrun: brief pause then recover to avoid tight retry loop
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        snd_pcm_prepare(pcm);
        snd_pcm_writei(pcm, buf, frames);
    }
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

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if      (a == "--group")    group    = next();
        else if (a == "--port")     port     = (uint16_t)atoi(next());
        else if (a == "--channels") channels = (uint32_t)atoi(next());
        else if (a == "--device")   alsa_dev = next();
        else if (a == "--output") {
            std::string mode = next();
            if (mode == "alsa") {
                use_alsa = true;
            } else if (mode == "pipe") {
                use_alsa = false;
            } else {
                fprintf(stderr, "[rx] Unknown --output mode '%s'; expected 'alsa' or 'pipe'\n",
                        mode.c_str());
                return 1;
            }
        }
        else if (a == "--help") {
            fprintf(stdout,
                "soluna-rx — Soluna multicast audio receiver\n\n"
                "  --group <ip>      Multicast group (default: 239.69.0.1)\n"
                "  --port <n>        UDP port        (default: 5004)\n"
                "  --channels <n>    Channels        (default: 2)\n"
                "  --output alsa     Output to ALSA  (default)\n"
                "  --output pipe     Output raw S16LE to stdout\n"
                "  --device <name>   ALSA device     (default: default)\n"
            );
            return 0;
        }
    }

    // Open UDP multicast socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

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

    // Recv timeout = 100ms so we can check g_running
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

    fprintf(stderr, "[rx] Listening on %s:%u\n", group.c_str(), port);

    static uint8_t  pkt[kMaxPktSize];
    static int16_t  out_buf[8192];

    int32_t last_seq = -1;
    uint64_t pkts_rx = 0, pkts_drop = 0;

    while (g_running) {
        int n = (int)recv(sock, pkt, sizeof(pkt), 0);
        if (n <= 0) continue;
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

        // Sequence tracking
        uint16_t seq = ntohs(rtp->sequence);
        if (last_seq >= 0) {
            int diff = (int)(uint16_t)(seq - (uint16_t)last_seq);
            if (diff > 1 && diff < 100) pkts_drop += (uint64_t)(diff - 1);
        }
        last_seq = seq;
        pkts_rx++;

        // Decode samples: OSTP = int32 native, plain RTP = int32 big-endian or S16
        size_t frames;
        if (is_ostp) {
            // OSTP payload: int32_t per sample, native byte order (S24 in 32-bit container)
            frames = payload_n / (sizeof(int32_t) * channels);
            const int32_t* src = reinterpret_cast<const int32_t*>(payload);
            size_t samples = frames * channels;
            if (samples > sizeof(out_buf) / sizeof(int16_t)) samples = sizeof(out_buf) / sizeof(int16_t);
            for (size_t i = 0; i < samples; i++) {
                out_buf[i] = (int16_t)(src[i] >> 8);  // S24→S16: drop low byte
            }
            frames = samples / channels;
        } else {
            // Plain RTP: assume L16 big-endian
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

        if (!use_alsa) {
            // Pipe: write raw S16LE to stdout
            fwrite(out_buf, sizeof(int16_t) * channels, frames, stdout);
        } else {
#ifdef __linux__
            alsa_write(pcm, out_buf, (snd_pcm_uframes_t)frames);
#endif
        }
    }

    fprintf(stderr, "\n[rx] Stopped. Received %llu packets, dropped %llu\n",
        (unsigned long long)pkts_rx, (unsigned long long)pkts_drop);

#ifdef __linux__
    if (pcm) { snd_pcm_drain(pcm); snd_pcm_close(pcm); }
#endif
    close(sock);
    return 0;
}
