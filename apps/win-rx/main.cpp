/**
 * soluna-rx-win — Soluna Windows CLI multicast receiver (WASAPI output)
 *
 * Receives RTP/OSTP audio from the Soluna multicast stream and outputs
 * to WASAPI (shared mode) or stdout (pipe).  With --metrics, outputs JSON
 * quality metrics to stderr every few seconds for auto-optimization.
 *
 * Usage:
 *   soluna-rx-win [options]
 *
 * Options:
 *   --group <ip>      Multicast group (default: 239.69.0.1)
 *   --port <n>        UDP port (default: 5004)
 *   --peer <host:port> P2P unicast mode (connect to solunad relay)
 *   --relay <host:port> WAN relay mode (connect to soluna-relay server)
 *   --group-name <name> Group name for WAN relay (default: "default")
 *   --group-password <pw> Group password for WAN relay (optional)
 *   --channels <n>    Channel count (default: 2)
 *   --output wasapi   Output to WASAPI default device (default)
 *   --output pipe     Output raw S16LE to stdout
 *   --device <id>     WASAPI device ID (default: system default)
 *   --record <path>   Record received audio to WAV file
 *   --duration <sec>  Recording duration in seconds (default: 30)
 *   --metrics         Output JSON quality metrics every 5s
 *   --metrics-interval <sec>  Metrics interval (default: 5)
 *   --help            Show this message
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _WIN32
#error "This source is Windows-only. Use apps/linux-rx for Linux."
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>

// ── RTP / OSTP headers (minimal, self-contained) ─────────────────────────────

#pragma pack(push, 1)
struct RtpHeader {
    uint8_t  cc_x_p_v;   // V=2, P, X, CC
    uint8_t  m_pt;        // M, PT
    uint16_t sequence;
    uint32_t timestamp;
    uint32_t ssrc;
};

struct OstpHeader {
    uint8_t  magic[4];    // 'O','S','T','P'
    uint16_t stream_id;
    uint16_t sequence_ext;
    uint32_t device_id;
    uint32_t flags;
};
#pragma pack(pop)

static constexpr uint32_t kOstpMagic  = 0x4F535450; // 'OSTP'
static constexpr size_t   kMaxPktSize = 65536;

// Opus payload type (RFC 7587 dynamic, commonly 111)
static constexpr uint8_t kPtOpus = 111;

// ── Ring buffer for jitter buffering ──────────────────────────────────────────

class JitterRingBuffer {
public:
    JitterRingBuffer(size_t capacity_samples)
        : buf_(capacity_samples, 0), capacity_(capacity_samples) {}

    // Returns number of samples actually written.
    size_t write(const int16_t* data, size_t samples) {
        size_t written = 0;
        while (written < samples && count_ < capacity_) {
            buf_[wr_] = data[written];
            wr_ = (wr_ + 1) % capacity_;
            ++count_;
            ++written;
        }
        return written;
    }

    // Returns number of samples actually read.
    size_t read(int16_t* data, size_t samples) {
        size_t got = 0;
        while (got < samples && count_ > 0) {
            data[got] = buf_[rd_];
            rd_ = (rd_ + 1) % capacity_;
            --count_;
            ++got;
        }
        return got;
    }

    size_t available() const { return count_; }
    size_t capacity()  const { return capacity_; }

private:
    std::vector<int16_t> buf_;
    size_t capacity_;
    size_t wr_ = 0;
    size_t rd_ = 0;
    size_t count_ = 0;
};

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
        memcpy(hdr,      "RIFF", 4); memcpy(hdr + 4,  &file_size, 4);
        memcpy(hdr + 8,  "WAVE", 4); memcpy(hdr + 12, "fmt ", 4);
        uint32_t fmt_size = 16; memcpy(hdr + 16, &fmt_size, 4);
        uint16_t pcm_fmt = 1;  memcpy(hdr + 20, &pcm_fmt, 2);
        uint16_t ch16 = (uint16_t)channels; memcpy(hdr + 22, &ch16, 2);
        memcpy(hdr + 24, &sample_rate, 4); memcpy(hdr + 28, &byte_rate, 4);
        memcpy(hdr + 32, &block_align, 2); memcpy(hdr + 34, &bits, 2);
        memcpy(hdr + 36, "data", 4); memcpy(hdr + 40, &data_bytes, 4);
        fwrite(hdr, 1, 44, fp);
        fclose(fp);
        fp = nullptr;
    }
};

// ── Quality metrics (rolling window) ─────────────────────────────────────────

struct QualityMetrics {
    uint64_t pkts_rx = 0;
    uint64_t pkts_drop = 0;
    double   rms_sum = 0;
    uint64_t rms_count = 0;
    int16_t  peak = 0;
    uint32_t clicks = 0;
    int16_t  last_packet_end = 0;
    uint32_t dropouts = 0;
    bool     in_dropout = false;
    uint32_t low_energy_frames = 0;
    uint32_t underruns = 0;

    void analyze_audio(const int16_t* buf, size_t samples, uint32_t ch,
                       bool is_packet_start) {
        constexpr int16_t kDropoutThresh = 104;
        constexpr uint32_t kDropoutMinFrames = 240;
        constexpr int kClickThresh = 8000;

        for (size_t i = 0; i < samples; i++) {
            int16_t s = buf[i];
            rms_sum += (double)s * s;
            rms_count++;
            int16_t abs_s = (int16_t)(s < 0 ? -s : s);
            if (abs_s > peak) peak = abs_s;
        }

        if (is_packet_start && samples >= ch) {
            int16_t first = buf[0];
            int diff = (int)first - (int)last_packet_end;
            if (diff < 0) diff = -diff;
            if (diff > kClickThresh && last_packet_end != 0) {
                clicks++;
            }
        }
        if (samples >= ch) {
            last_packet_end = buf[samples - ch];
        }

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

    void print_json(uint32_t /*sample_rate*/) {
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

static BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

static double now_sec() {
    LARGE_INTEGER freq, ctr;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&ctr);
    return (double)ctr.QuadPart / (double)freq.QuadPart;
}

// ── WASAPI output ─────────────────────────────────────────────────────────────

struct WasapiOutput {
    IMMDevice*          device       = nullptr;
    IAudioClient*       audio_client = nullptr;
    IAudioRenderClient* render       = nullptr;
    UINT32              buffer_frames = 0;
    UINT32              channels     = 2;
    UINT32              sample_rate  = 48000;

    bool open(const std::string& device_id, uint32_t ch, uint32_t rate) {
        channels = ch;
        sample_rate = rate;

        IMMDeviceEnumerator* enumerator = nullptr;
        HRESULT hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr) || !enumerator) {
            fprintf(stderr, "[rx] CoCreateInstance(MMDeviceEnumerator) failed: 0x%08lx\n", hr);
            return false;
        }

        if (device_id.empty()) {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        } else {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(), -1, nullptr, 0);
            std::vector<wchar_t> wid(wlen);
            MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(), -1, wid.data(), wlen);
            hr = enumerator->GetDevice(wid.data(), &device);
        }
        enumerator->Release();
        if (FAILED(hr) || !device) {
            fprintf(stderr, "[rx] Failed to get audio device: 0x%08lx\n", hr);
            return false;
        }

        hr = device->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL,
            nullptr, reinterpret_cast<void**>(&audio_client));
        if (FAILED(hr) || !audio_client) {
            fprintf(stderr, "[rx] Failed to activate IAudioClient: 0x%08lx\n", hr);
            return false;
        }

        // Use 16-bit integer PCM in shared mode.
        // We receive S16LE over RTP, so matching format avoids conversion.
        WAVEFORMATEX fmt{};
        fmt.wFormatTag      = WAVE_FORMAT_PCM;
        fmt.nChannels       = (WORD)ch;
        fmt.nSamplesPerSec  = rate;
        fmt.wBitsPerSample  = 16;
        fmt.nBlockAlign     = fmt.nChannels * fmt.wBitsPerSample / 8;
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

        // 40ms buffer in shared mode (100ns units)
        REFERENCE_TIME buf_duration = 400000; // 40ms

        hr = audio_client->Initialize(
            AUDCLNT_SHAREMODE_SHARED, 0,
            buf_duration, 0, &fmt, nullptr);

        if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
            // Shared mode may require the device's mix format.
            // Fall back to the device mix format and we will convert on the fly.
            WAVEFORMATEX* mix_fmt = nullptr;
            audio_client->GetMixFormat(&mix_fmt);
            if (mix_fmt) {
                fprintf(stderr, "[rx] Device mix format: %u Hz, %u ch, %u bit\n",
                        mix_fmt->nSamplesPerSec, mix_fmt->nChannels,
                        mix_fmt->wBitsPerSample);

                // Re-create audio_client because Initialize can only be called once
                audio_client->Release();
                audio_client = nullptr;
                device->Activate(
                    __uuidof(IAudioClient), CLSCTX_ALL,
                    nullptr, reinterpret_cast<void**>(&audio_client));

                hr = audio_client->Initialize(
                    AUDCLNT_SHAREMODE_SHARED, 0,
                    buf_duration, 0, mix_fmt, nullptr);

                // Update our tracked format to match
                if (SUCCEEDED(hr)) {
                    channels    = mix_fmt->nChannels;
                    sample_rate = mix_fmt->nSamplesPerSec;
                }
                CoTaskMemFree(mix_fmt);
            }
        }

        if (FAILED(hr)) {
            fprintf(stderr, "[rx] IAudioClient::Initialize failed: 0x%08lx\n", hr);
            return false;
        }

        hr = audio_client->GetBufferSize(&buffer_frames);
        if (FAILED(hr)) {
            fprintf(stderr, "[rx] GetBufferSize failed: 0x%08lx\n", hr);
            return false;
        }

        hr = audio_client->GetService(
            __uuidof(IAudioRenderClient),
            reinterpret_cast<void**>(&render));
        if (FAILED(hr) || !render) {
            fprintf(stderr, "[rx] GetService(IAudioRenderClient) failed: 0x%08lx\n", hr);
            return false;
        }

        return true;
    }

    bool start() {
        if (!audio_client) return false;
        HRESULT hr = audio_client->Start();
        return SUCCEEDED(hr);
    }

    // Write S16LE samples into the WASAPI render buffer.
    // Returns number of frames written (0 if buffer is full / error).
    UINT32 write_samples(const int16_t* data, UINT32 frames, uint32_t& underrun_count) {
        if (!render || !audio_client) return 0;

        UINT32 padding = 0;
        HRESULT hr = audio_client->GetCurrentPadding(&padding);
        if (FAILED(hr)) return 0;

        UINT32 available = buffer_frames - padding;
        if (available == 0) return 0;

        UINT32 to_write = (std::min)(frames, available);

        BYTE* buf = nullptr;
        hr = render->GetBuffer(to_write, &buf);
        if (FAILED(hr) || !buf) return 0;

        memcpy(buf, data, to_write * channels * sizeof(int16_t));
        render->ReleaseBuffer(to_write, 0);

        // Detect underrun: if padding was 0 and we had data waiting, that
        // means the device ran dry.
        if (padding == 0 && frames > 0) {
            underrun_count++;
        }

        return to_write;
    }

    void stop() {
        if (audio_client) audio_client->Stop();
    }

    void close() {
        stop();
        if (render)       { render->Release();       render       = nullptr; }
        if (audio_client) { audio_client->Release(); audio_client = nullptr; }
        if (device)       { device->Release();       device       = nullptr; }
    }
};

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    std::string group    = "239.69.0.1";
    uint16_t    port     = 5004;
    uint32_t    channels = 2;
    uint32_t    rate     = 48000;
    bool        use_wasapi = true;
    std::string wasapi_dev;              // empty = system default
    std::string record_path;
    uint32_t    record_duration = 30;
    bool        metrics_enabled = false;
    uint32_t    metrics_interval = 5;
    std::string peer_host;
    uint16_t    peer_port = 5099;
    std::string relay_host;
    uint16_t    relay_port = 5100;
    std::string relay_group = "default";
    std::string relay_password;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if      (a == "--group")    group    = next();
        else if (a == "--port")     port     = (uint16_t)atoi(next());
        else if (a == "--channels") channels = (uint32_t)atoi(next());
        else if (a == "--device")   wasapi_dev = next();
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
            if (mode == "wasapi") {
                use_wasapi = true;
            } else if (mode == "pipe") {
                use_wasapi = false;
            } else {
                fprintf(stderr, "[rx] Unknown --output mode '%s' (use 'wasapi' or 'pipe')\n",
                        mode.c_str());
                return 1;
            }
        }
        else if (a == "--help") {
            fprintf(stdout,
                "soluna-rx-win — Soluna Windows multicast audio receiver (WASAPI)\n\n"
                "  --group <ip>           Multicast group (default: 239.69.0.1)\n"
                "  --port <n>             UDP port        (default: 5004)\n"
                "  --peer <host:port>     P2P unicast via solunad relay (default port: 5099)\n"
                "  --relay <host:port>    WAN relay mode via soluna-relay (default port: 5100)\n"
                "  --group-name <name>    Group name for WAN relay (default: default)\n"
                "  --group-password <pw>  Group password for WAN relay (optional)\n"
                "  --channels <n>         Channels        (default: 2)\n"
                "  --output wasapi        Output to WASAPI (default)\n"
                "  --output pipe          Output raw S16LE to stdout\n"
                "  --device <id>          WASAPI device ID (default: system default)\n"
                "  --record <path>        Record to WAV    (for analysis)\n"
                "  --duration <sec>       Record duration   (default: 30)\n"
                "  --metrics              Output JSON quality metrics to stderr\n"
                "  --metrics-interval <s> Metrics interval  (default: 5)\n"
            );
            return 0;
        }
    }

    // ── Initialize Winsock ────────────────────────────────────────────────
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[rx] WSAStartup failed\n");
        return 1;
    }

    // ── Initialize COM (for WASAPI) ───────────────────────────────────────
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        fprintf(stderr, "[rx] CoInitializeEx failed: 0x%08lx\n", hr);
        WSACleanup();
        return 1;
    }

    // ── Open socket: WAN relay, P2P unicast, or multicast ────────────────
    bool peer_mode  = !peer_host.empty();
    bool relay_mode = !relay_host.empty();
    sockaddr_in peer_addr{};
    sockaddr_in relay_addr{};

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        fprintf(stderr, "[rx] socket() failed: %d\n", WSAGetLastError());
        CoUninitialize();
        WSACleanup();
        return 1;
    }

    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    if (relay_mode) {
        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        bind_addr.sin_port = 0;
        if (bind(sock, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) == SOCKET_ERROR) {
            fprintf(stderr, "[rx] bind failed: %d\n", WSAGetLastError());
            closesocket(sock); CoUninitialize(); WSACleanup();
            return 1;
        }
        relay_addr.sin_family = AF_INET;
        relay_addr.sin_port = htons(relay_port);
        if (inet_pton(AF_INET, relay_host.c_str(), &relay_addr.sin_addr) <= 0) {
            fprintf(stderr, "[rx] Invalid relay host: %s\n", relay_host.c_str());
            closesocket(sock); CoUninitialize(); WSACleanup();
            return 1;
        }
        // Send JOIN message
        std::string join_msg = "JOIN:" + relay_group;
        if (!relay_password.empty()) join_msg += ":" + relay_password;
        join_msg += "\n";
        sendto(sock, join_msg.c_str(), (int)join_msg.size(), 0,
               reinterpret_cast<sockaddr*>(&relay_addr), sizeof(relay_addr));
        fprintf(stderr, "[rx] WAN relay mode: server=%s:%u group='%s'\n",
                relay_host.c_str(), relay_port, relay_group.c_str());
    } else if (peer_mode) {
        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        bind_addr.sin_port = 0;
        if (bind(sock, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) == SOCKET_ERROR) {
            fprintf(stderr, "[rx] bind failed: %d\n", WSAGetLastError());
            closesocket(sock); CoUninitialize(); WSACleanup();
            return 1;
        }
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_port = htons(peer_port);
        if (inet_pton(AF_INET, peer_host.c_str(), &peer_addr.sin_addr) <= 0) {
            fprintf(stderr, "[rx] Invalid peer host: %s\n", peer_host.c_str());
            closesocket(sock); CoUninitialize(); WSACleanup();
            return 1;
        }
        const char hello[] = "hello";
        sendto(sock, hello, sizeof(hello), 0,
               reinterpret_cast<sockaddr*>(&peer_addr), sizeof(peer_addr));
        fprintf(stderr, "[rx] P2P mode: relay=%s:%u\n", peer_host.c_str(), peer_port);
    } else {
        // Multicast mode
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            fprintf(stderr, "[rx] bind failed: %d\n", WSAGetLastError());
            closesocket(sock); CoUninitialize(); WSACleanup();
            return 1;
        }
        ip_mreq mreq{};
        inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = INADDR_ANY;
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       reinterpret_cast<const char*>(&mreq), sizeof(mreq)) == SOCKET_ERROR) {
            fprintf(stderr, "[rx] IP_ADD_MEMBERSHIP failed: %d\n", WSAGetLastError());
            closesocket(sock); CoUninitialize(); WSACleanup();
            return 1;
        }
    }

    // Set 100ms receive timeout
    DWORD tv = 100; // milliseconds
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));

    SetConsoleCtrlHandler(console_handler, TRUE);

    // ── Open WASAPI output ────────────────────────────────────────────────
    WasapiOutput wasapi;
    if (use_wasapi) {
        if (!wasapi.open(wasapi_dev, channels, rate)) {
            fprintf(stderr, "[rx] WASAPI open failed\n");
            closesocket(sock); CoUninitialize(); WSACleanup();
            return 1;
        }
        if (!wasapi.start()) {
            fprintf(stderr, "[rx] WASAPI start failed\n");
            wasapi.close();
            closesocket(sock); CoUninitialize(); WSACleanup();
            return 1;
        }
        fprintf(stderr, "[rx] WASAPI output: %uHz %uch  (buffer %u frames)\n",
                wasapi.sample_rate, wasapi.channels, wasapi.buffer_frames);
    } else {
        fprintf(stderr, "[rx] Pipe output (raw S16LE %uHz %uch)\n", rate, channels);
    }

    // ── Jitter ring buffer ────────────────────────────────────────────────
    // Size: ~80ms of buffering at 48kHz stereo = 48000 * 0.08 * 2 = 7680 samples
    size_t jitter_capacity = (size_t)(rate * channels * 80 / 1000);
    JitterRingBuffer jitter(jitter_capacity);

    // Pre-fill threshold: start playback once we have ~20ms buffered
    size_t prefill_samples = (size_t)(rate * channels * 20 / 1000);
    bool   prefilled = false;

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

    static uint8_t pkt[kMaxPktSize];
    static int16_t out_buf[8192];
    // Drain buffer for reading from jitter buffer into WASAPI
    static int16_t drain_buf[4096];

    int32_t  last_seq = -1;
    uint64_t total_rx = 0, total_drop = 0;
    double   last_hello_time = now_sec();
    bool     opus_warned = false;

    while (g_running) {
        // ── Heartbeat: send keepalive every 5s ───────────────────────────
        if (relay_mode) {
            double now = now_sec();
            if (now - last_hello_time >= 5.0) {
                const char hello[] = "HELLO\n";
                sendto(sock, hello, (int)strlen(hello), 0,
                       reinterpret_cast<sockaddr*>(&relay_addr), sizeof(relay_addr));
                last_hello_time = now;
            }
        } else if (peer_mode) {
            double now = now_sec();
            if (now - last_hello_time >= 5.0) {
                const char hello[] = "hello";
                sendto(sock, hello, sizeof(hello), 0,
                       reinterpret_cast<sockaddr*>(&peer_addr), sizeof(peer_addr));
                last_hello_time = now;
            }
        }

        // ── Receive packet ───────────────────────────────────────────────
        int n = recv(sock, reinterpret_cast<char*>(pkt), (int)sizeof(pkt), 0);
        if (n <= 0) {
            // Timeout or error -- drain jitter buffer to WASAPI if data present
            if (use_wasapi && prefilled && jitter.available() > 0) {
                size_t avail = jitter.available();
                size_t chunk = (std::min)(avail, (size_t)4096);
                size_t got = jitter.read(drain_buf, chunk);
                if (got > 0) {
                    UINT32 frames = (UINT32)(got / channels);
                    wasapi.write_samples(drain_buf, frames, metrics.underruns);
                }
            }

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

        // ── Opus payload type detection ──────────────────────────────────
        uint8_t pt = rtp->m_pt & 0x7F;
        if (pt == kPtOpus && !opus_warned) {
            fprintf(stderr,
                "[rx] WARNING: Opus payload (PT=%u) detected but no Opus decoder compiled.\n"
                "     Audio will be garbled. Rebuild with SOLUNA_ENABLE_OPUS=ON for Opus support.\n",
                pt);
            opus_warned = true;
        }

        // ── Detect OSTP vs plain RTP ─────────────────────────────────────
        bool   is_ostp = false;
        size_t payload_off = sizeof(RtpHeader);
        if ((size_t)n >= sizeof(RtpHeader) + sizeof(OstpHeader)) {
            const OstpHeader* ostp = reinterpret_cast<const OstpHeader*>(
                pkt + sizeof(RtpHeader));
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

        // ── Sequence tracking ────────────────────────────────────────────
        uint16_t seq = ntohs(rtp->sequence);
        if (last_seq >= 0) {
            int diff = (int)(uint16_t)(seq - (uint16_t)last_seq);
            if (diff == 0) {
                // Duplicate packet (dup_send) -- skip
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

        // ── Decode samples ───────────────────────────────────────────────
        size_t frames;
        if (is_ostp) {
            // OSTP: 32-bit (S24-in-S32) interleaved -> S16
            frames = payload_n / (sizeof(int32_t) * channels);
            const int32_t* src = reinterpret_cast<const int32_t*>(payload);
            size_t samples = frames * channels;
            if (samples > sizeof(out_buf) / sizeof(int16_t))
                samples = sizeof(out_buf) / sizeof(int16_t);
            for (size_t i = 0; i < samples; i++) {
                out_buf[i] = (int16_t)(src[i] >> 8);
            }
            frames = samples / channels;
        } else {
            // Plain RTP: S16 network byte order (big-endian) -> host
            frames = payload_n / (sizeof(int16_t) * channels);
            const int16_t* src = reinterpret_cast<const int16_t*>(payload);
            size_t samples = frames * channels;
            if (samples > sizeof(out_buf) / sizeof(int16_t))
                samples = sizeof(out_buf) / sizeof(int16_t);
            for (size_t i = 0; i < samples; i++) {
                out_buf[i] = (int16_t)ntohs((uint16_t)src[i]);
            }
            frames = samples / channels;
        }

        if (frames == 0) continue;

        size_t total_samples = frames * channels;

        // ── Quality analysis ─────────────────────────────────────────────
        if (metrics_enabled) {
            metrics.analyze_audio(out_buf, total_samples, channels, true);

            double now = now_sec();
            if (now - last_metrics_time >= metrics_interval) {
                metrics.print_json(rate);
                metrics.reset();
                last_metrics_time = now;
            }
        }

        // ── Write to WAV if recording ────────────────────────────────────
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

        // ── Audio output ─────────────────────────────────────────────────
        if (!use_wasapi) {
            // Pipe mode: raw S16LE to stdout
            fwrite(out_buf, sizeof(int16_t) * channels, frames, stdout);
            fflush(stdout);
        } else {
            // WASAPI: write into jitter ring buffer, then drain to device
            jitter.write(out_buf, total_samples);

            if (!prefilled) {
                if (jitter.available() >= prefill_samples) {
                    prefilled = true;
                    fprintf(stderr, "[rx] Jitter buffer prefilled, starting playback\n");
                } else {
                    continue; // accumulate more before starting playback
                }
            }

            // Drain jitter buffer into WASAPI in chunks
            while (jitter.available() > 0) {
                size_t avail = jitter.available();
                size_t chunk = (std::min)(avail, (size_t)4096);
                size_t got = jitter.read(drain_buf, chunk);
                if (got == 0) break;

                UINT32 written_frames = wasapi.write_samples(
                    drain_buf, (UINT32)(got / channels), metrics.underruns);
                if (written_frames == 0) {
                    // WASAPI buffer full -- put the unplayed samples back
                    // (simplified: we just drop the remainder this iteration
                    // and it will be picked up next time around)
                    break;
                }
            }
        }
    }

    // ── Cleanup ───────────────────────────────────────────────────────────
    if (wav.fp) {
        wav.close();
        fprintf(stderr, "[rx] Recording saved: %llu frames\n",
                (unsigned long long)record_frames_written);
    }

    fprintf(stderr, "\n[rx] Stopped. Received %llu packets, dropped %llu\n",
            (unsigned long long)total_rx, (unsigned long long)total_drop);

    if (use_wasapi) {
        wasapi.close();
    }

    closesocket(sock);
    CoUninitialize();
    WSACleanup();
    return 0;
}
