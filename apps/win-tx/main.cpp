/**
 * soluna-tx-win — Soluna Windows system audio transmitter (OSTP/WAN relay)
 *
 * Captures system audio via WASAPI loopback (or microphone) and streams it
 * as OSTP packets to a WAN relay server or UDP multicast group.
 * Fully compatible with iOS/Mac/Linux receivers.
 *
 * Usage:
 *   soluna-tx-win --relay relay.solun.art:5100 --group-name myroom
 *   soluna-tx-win --relay relay.solun.art:5100 --group-name myroom --group-password secret
 *   soluna-tx-win --multicast 239.69.0.1 --port 5004          (LAN multicast)
 *   soluna-tx-win --mic                                        (capture mic instead of loopback)
 *   soluna-tx-win --device <id>                                (specific capture device)
 *   soluna-tx-win --channels 1                                 (mono output)
 *
 * Options:
 *   --relay <host:port>       WAN relay server (default: relay.solun.art:5100)
 *   --group-name <name>       Channel name (default: soluna)
 *   --group-password <pw>     Channel password (optional)
 *   --multicast <ip>          Multicast group for LAN mode (disables relay)
 *   --port <n>                UDP port for multicast (default: 5004)
 *   --mic                     Capture microphone instead of system audio
 *   --device <id>             Capture device ID (empty = system default)
 *   --channels <n>            Channel count: 1=mono, 2=stereo (default: 2)
 *   --bitrate <kbps>          TX bitrate, 0=raw PCM (default: 0)
 *   --help                    Show this message
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _WIN32
#error "This source is Windows-only."
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
#include <avrt.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <string>
#include <vector>
#include <atomic>
#include <thread>

// ── OSTP packet structures (matched with win-rx / iOS / Mac) ─────────────────

#pragma pack(push, 1)
struct RtpHeader {
    uint8_t  cc_x_p_v;   // V=2 (0x80)
    uint8_t  m_pt;        // PT=96 for OSTP
    uint16_t sequence;
    uint32_t timestamp;
    uint32_t ssrc;
};

struct OstpHeader {
    uint8_t  magic[4];    // 'O','S','T','P'
    uint16_t stream_id;   // upper 4 bits = channel count
    uint16_t sequence_ext;
    uint32_t device_id;
    uint32_t flags;
    uint32_t media_timestamp; // lower 32 bits of ns since Unix epoch
};
#pragma pack(pop)

static constexpr uint32_t kOstpMagic     = 0x4F535450; // 'OSTP'
static constexpr uint8_t  kPtOstp        = 96;
static constexpr uint32_t kDefaultRate   = 48000;
static constexpr uint32_t kFramesPerPkt  = 480;        // 10ms at 48kHz
static constexpr float    kSampleScale   = 8388608.0f; // 2^23 — matched iOS/Mac

// ── CRC-32 (IEEE 802.3) ───────────────────────────────────────────────────────

static uint32_t crc32_table[256];
static bool crc32_inited = false;

static void crc32_init() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_inited = true;
}

static uint32_t crc32_calc(const uint8_t* data, size_t len) {
    if (!crc32_inited) crc32_init();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// ── Globals ───────────────────────────────────────────────────────────────────

static volatile bool g_running = true;

static BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

static uint64_t now_ns() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t100 = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    // FILETIME is 100ns intervals since 1601-01-01; convert to Unix epoch ns
    return (t100 - 116444736000000000ULL) * 100;
}

static double now_sec() {
    LARGE_INTEGER freq, ctr;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&ctr);
    return (double)ctr.QuadPart / (double)freq.QuadPart;
}

// ── WASAPI loopback / capture ─────────────────────────────────────────────────

struct WasapiCapture {
    IMMDevice*          device        = nullptr;
    IAudioClient*       audio_client  = nullptr;
    IAudioCaptureClient* capture      = nullptr;
    WAVEFORMATEX*       mix_fmt       = nullptr;
    uint32_t            channels      = 2;
    uint32_t            sample_rate   = 48000;
    bool                is_float      = true;
    uint16_t            bits          = 32;

    // Open loopback (loopback=true) or mic (loopback=false)
    bool open(const std::string& device_id, uint32_t out_channels, bool loopback) {
        channels = out_channels;

        IMMDeviceEnumerator* enumerator = nullptr;
        HRESULT hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr) || !enumerator) {
            fprintf(stderr, "[tx] CoCreateInstance(MMDeviceEnumerator) failed: 0x%08lx\n", hr);
            return false;
        }

        if (device_id.empty()) {
            // Loopback → use default render endpoint; mic → default capture endpoint
            EDataFlow flow = loopback ? eRender : eCapture;
            hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device);
        } else {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(), -1, nullptr, 0);
            std::vector<wchar_t> wid(wlen);
            MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(), -1, wid.data(), wlen);
            hr = enumerator->GetDevice(wid.data(), &device);
        }
        enumerator->Release();
        if (FAILED(hr) || !device) {
            fprintf(stderr, "[tx] Failed to get audio device: 0x%08lx\n", hr);
            return false;
        }

        hr = device->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL,
            nullptr, reinterpret_cast<void**>(&audio_client));
        if (FAILED(hr) || !audio_client) {
            fprintf(stderr, "[tx] Failed to activate IAudioClient: 0x%08lx\n", hr);
            return false;
        }

        hr = audio_client->GetMixFormat(&mix_fmt);
        if (FAILED(hr) || !mix_fmt) {
            fprintf(stderr, "[tx] GetMixFormat failed: 0x%08lx\n", hr);
            return false;
        }

        sample_rate = mix_fmt->nSamplesPerSec;
        // Detect float vs int (WAVEFORMATEXTENSIBLE uses WAVE_FORMAT_EXTENSIBLE tag)
        if (mix_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            is_float = true;
            bits = 32;
        } else if (mix_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            const WAVEFORMATEXTENSIBLE* ext =
                reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix_fmt);
            is_float = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
            bits = mix_fmt->wBitsPerSample;
        } else {
            is_float = false;
            bits = mix_fmt->wBitsPerSample;
        }

        fprintf(stderr, "[tx] Device format: %u Hz, %u ch, %u bit, %s\n",
                mix_fmt->nSamplesPerSec, mix_fmt->nChannels, bits,
                is_float ? "float" : "int");

        if (sample_rate != kDefaultRate) {
            fprintf(stderr, "[tx] WARNING: device is %u Hz, OSTP expects 48000 Hz.\n"
                            "             Audio may drift. Use a 48 kHz output device for best results.\n",
                    sample_rate);
        }

        DWORD stream_flags = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
        REFERENCE_TIME buf_duration = 200000; // 20ms in 100ns units

        hr = audio_client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            stream_flags,
            buf_duration, 0,
            mix_fmt, nullptr);
        if (FAILED(hr)) {
            fprintf(stderr, "[tx] IAudioClient::Initialize failed: 0x%08lx\n", hr);
            return false;
        }

        hr = audio_client->GetService(
            __uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(&capture));
        if (FAILED(hr) || !capture) {
            fprintf(stderr, "[tx] GetService(IAudioCaptureClient) failed: 0x%08lx\n", hr);
            return false;
        }

        return true;
    }

    bool start() {
        HRESULT hr = audio_client->Start();
        return SUCCEEDED(hr);
    }

    void close() {
        if (audio_client) { audio_client->Stop(); audio_client->Release(); audio_client = nullptr; }
        if (capture)      { capture->Release();      capture = nullptr; }
        if (device)       { device->Release();        device  = nullptr; }
        if (mix_fmt)      { CoTaskMemFree(mix_fmt);   mix_fmt = nullptr; }
    }

    // Read available frames, convert to float32 interleaved at native rate.
    // Returns number of frames written to out_buf.
    size_t read_frames(std::vector<float>& out_buf) {
        size_t total = 0;
        UINT32 pkt_frames = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&pkt_frames)) && pkt_frames > 0) {
            BYTE*  data;
            UINT32 frames;
            DWORD  flags;
            HRESULT hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr) || frames == 0) break;

            bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            uint32_t src_ch = mix_fmt->nChannels;

            size_t base = out_buf.size();
            out_buf.resize(base + frames * channels);

            for (UINT32 f = 0; f < frames; f++) {
                for (uint32_t c = 0; c < channels; c++) {
                    float s = 0.0f;
                    if (!silent) {
                        uint32_t src_c = (c < src_ch) ? c : 0;
                        if (is_float) {
                            const float* fp = reinterpret_cast<const float*>(data);
                            s = fp[f * src_ch + src_c];
                        } else if (bits == 16) {
                            const int16_t* ip = reinterpret_cast<const int16_t*>(data);
                            s = (float)ip[f * src_ch + src_c] / 32768.0f;
                        } else if (bits == 24) {
                            const uint8_t* bp = data + (f * src_ch + src_c) * 3;
                            int32_t v = (int32_t)(((uint32_t)bp[2] << 16) |
                                                  ((uint32_t)bp[1] << 8)  |
                                                   (uint32_t)bp[0]);
                            if (v & 0x800000) v |= 0xFF000000;
                            s = (float)v / 8388608.0f;
                        } else if (bits == 32 && !is_float) {
                            const int32_t* ip = reinterpret_cast<const int32_t*>(data);
                            s = (float)ip[f * src_ch + src_c] / 2147483648.0f;
                        }
                    }
                    out_buf[base + f * channels + c] = s;
                }
            }
            total += frames;
            capture->ReleaseBuffer(frames);
        }
        return total;
    }
};

// ── Build OSTP packet ─────────────────────────────────────────────────────────

static size_t build_ostp_packet(
    uint8_t* buf, size_t buf_size,
    uint16_t seq, uint32_t rtp_ts, uint32_t ssrc,
    uint32_t channels,
    const float* samples, uint32_t frame_count)
{
    size_t payload_bytes = (size_t)frame_count * channels * sizeof(int32_t);
    size_t pkt_size = sizeof(RtpHeader) + sizeof(OstpHeader) + payload_bytes + 4; // +4 CRC
    if (pkt_size > buf_size) return 0;

    // RTP header
    RtpHeader* rtp = reinterpret_cast<RtpHeader*>(buf);
    rtp->cc_x_p_v  = 0x80; // V=2, P=0, X=0, CC=0
    rtp->m_pt      = kPtOstp;
    rtp->sequence  = htons(seq);
    rtp->timestamp = htonl(rtp_ts);
    rtp->ssrc      = htonl(ssrc);

    // OSTP header
    OstpHeader* ostp = reinterpret_cast<OstpHeader*>(buf + sizeof(RtpHeader));
    ostp->magic[0] = 'O'; ostp->magic[1] = 'S';
    ostp->magic[2] = 'T'; ostp->magic[3] = 'P';
    // Upper 4 bits of stream_id = channel count
    ostp->stream_id      = htons((uint16_t)(channels << 12));
    ostp->sequence_ext   = htons((uint16_t)(seq >> 16)); // extended seq (16-bit here)
    ostp->device_id      = htonl(ssrc);
    ostp->flags          = 0;
    ostp->media_timestamp = htonl((uint32_t)(now_ns() & 0xFFFFFFFFu));

    // PCM payload: float → int32 (24-bit range)
    int32_t* pcm = reinterpret_cast<int32_t*>(buf + sizeof(RtpHeader) + sizeof(OstpHeader));
    size_t sample_count = (size_t)frame_count * channels;
    for (size_t i = 0; i < sample_count; i++) {
        float s = samples[i];
        if (s >  1.0f) s =  1.0f;
        if (s < -1.0f) s = -1.0f;
        pcm[i] = (int32_t)(s * kSampleScale);
    }

    // CRC-32 over entire packet before CRC field
    size_t crc_offset = sizeof(RtpHeader) + sizeof(OstpHeader) + payload_bytes;
    uint32_t crc = crc32_calc(buf, crc_offset);
    memcpy(buf + crc_offset, &crc, 4);

    return pkt_size;
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    std::string relay_host     = "relay.solun.art";
    uint16_t    relay_port     = 5100;
    std::string relay_group    = "soluna";
    std::string relay_password;
    bool        use_relay      = true;
    std::string mcast_group    = "239.69.0.1";
    uint16_t    mcast_port     = 5004;
    bool        use_multicast  = false;
    bool        use_mic        = false;
    std::string device_id;
    uint32_t    channels       = 2;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if      (a == "--relay") {
            std::string hp = next();
            auto colon = hp.rfind(':');
            if (colon != std::string::npos) {
                relay_host = hp.substr(0, colon);
                relay_port = (uint16_t)atoi(hp.substr(colon + 1).c_str());
            } else {
                relay_host = hp;
            }
            use_relay = true; use_multicast = false;
        }
        else if (a == "--group-name")     relay_group    = next();
        else if (a == "--group-password") relay_password = next();
        else if (a == "--multicast") {
            mcast_group   = next();
            use_multicast = true; use_relay = false;
        }
        else if (a == "--port")     mcast_port = (uint16_t)atoi(next());
        else if (a == "--channels") channels   = (uint32_t)atoi(next());
        else if (a == "--device")   device_id  = next();
        else if (a == "--mic")      use_mic    = true;
        else if (a == "--help") {
            fprintf(stdout,
                "soluna-tx-win — Soluna Windows system audio transmitter\n\n"
                "  --relay <host:port>       WAN relay (default: relay.solun.art:5100)\n"
                "  --group-name <name>       Channel name (default: soluna)\n"
                "  --group-password <pw>     Channel password (optional)\n"
                "  --multicast <ip>          LAN multicast group (disables relay)\n"
                "  --port <n>                UDP port for multicast (default: 5004)\n"
                "  --mic                     Capture microphone instead of system audio\n"
                "  --device <id>             Capture device ID\n"
                "  --channels <n>            Output channels: 1=mono, 2=stereo (default: 2)\n"
            );
            return 0;
        }
    }

    // ── Initialize Winsock ──────────────────────────────────────────────
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[tx] WSAStartup failed\n");
        return 1;
    }

    // ── Initialize COM ──────────────────────────────────────────────────
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        fprintf(stderr, "[tx] CoInitializeEx failed: 0x%08lx\n", hr);
        WSACleanup();
        return 1;
    }

    // ── Open UDP socket ─────────────────────────────────────────────────
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        fprintf(stderr, "[tx] socket() failed: %d\n", WSAGetLastError());
        CoUninitialize(); WSACleanup();
        return 1;
    }

    sockaddr_in dest_addr{};
    bool relay_mode = use_relay && !relay_host.empty();

    if (relay_mode) {
        sockaddr_in bind_addr{};
        bind_addr.sin_family      = AF_INET;
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        bind_addr.sin_port        = 0;
        bind(sock, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr));

        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port   = htons(relay_port);
        if (inet_pton(AF_INET, relay_host.c_str(), &dest_addr.sin_addr) <= 0) {
            // Try hostname resolution
            ADDRINFOA hints{}, *res = nullptr;
            hints.ai_family   = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            char port_str[16];
            snprintf(port_str, sizeof(port_str), "%u", relay_port);
            if (getaddrinfo(relay_host.c_str(), port_str, &hints, &res) != 0 || !res) {
                fprintf(stderr, "[tx] Cannot resolve relay host: %s\n", relay_host.c_str());
                closesocket(sock); CoUninitialize(); WSACleanup();
                return 1;
            }
            dest_addr = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
            freeaddrinfo(res);
        }

        // JOIN the relay group
        std::string join_msg = "JOIN:" + relay_group;
        if (!relay_password.empty()) join_msg += ":" + relay_password;
        join_msg += "\n";
        sendto(sock, join_msg.c_str(), (int)join_msg.size(), 0,
               reinterpret_cast<sockaddr*>(&dest_addr), sizeof(dest_addr));
        fprintf(stderr, "[tx] WAN relay: %s:%u group='%s'\n",
                relay_host.c_str(), relay_port, relay_group.c_str());
    } else {
        // Multicast TX
        sockaddr_in bind_addr{};
        bind_addr.sin_family      = AF_INET;
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        bind_addr.sin_port        = htons(mcast_port);
        bind(sock, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr));

        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port   = htons(mcast_port);
        inet_pton(AF_INET, mcast_group.c_str(), &dest_addr.sin_addr);

        // Set multicast TTL
        DWORD ttl = 4;
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL,
                   reinterpret_cast<const char*>(&ttl), sizeof(ttl));
        fprintf(stderr, "[tx] Multicast: %s:%u\n", mcast_group.c_str(), mcast_port);
    }

    SetConsoleCtrlHandler(console_handler, TRUE);

    // ── Open WASAPI capture ─────────────────────────────────────────────
    WasapiCapture cap;
    bool loopback = !use_mic;
    if (!cap.open(device_id, channels, loopback)) {
        fprintf(stderr, "[tx] Failed to open %s capture\n", loopback ? "loopback" : "mic");
        closesocket(sock); CoUninitialize(); WSACleanup();
        return 1;
    }
    if (!cap.start()) {
        fprintf(stderr, "[tx] Failed to start audio capture\n");
        cap.close(); closesocket(sock); CoUninitialize(); WSACleanup();
        return 1;
    }

    fprintf(stderr, "[tx] Capturing %s (%u ch, %u Hz) → streaming as OSTP\n",
            loopback ? "system audio" : "microphone",
            channels, cap.sample_rate);

    // ── Boost thread priority (MMCSS) for low-latency capture ──────────
    DWORD task_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsA("Pro Audio", &task_index);

    // ── TX loop ─────────────────────────────────────────────────────────
    uint16_t  seq          = (uint16_t)(now_ns() & 0xFFFF); // random start
    uint32_t  rtp_ts       = (uint32_t)(now_ns() & 0xFFFFFFFF);
    uint32_t  ssrc         = (uint32_t)(now_ns() ^ (now_ns() >> 17));
    double    last_hello   = now_sec();
    double    last_stat    = now_sec();
    uint64_t  pkts_sent    = 0;

    std::vector<float> accum; // accumulator buffer
    accum.reserve(kFramesPerPkt * channels * 4);

    static uint8_t pkt_buf[65536];

    // Rate adjustment: if device is not 48kHz, we need to adapt the frame count per packet
    // to maintain ~10ms intervals
    uint32_t frames_per_pkt = (uint32_t)((double)kFramesPerPkt *
                                          (double)cap.sample_rate / (double)kDefaultRate);
    if (frames_per_pkt < 48) frames_per_pkt = 48;

    while (g_running) {
        // ── Keepalive ───────────────────────────────────────────────────
        if (relay_mode) {
            double now = now_sec();
            if (now - last_hello >= 5.0) {
                const char hello[] = "HELLO\n";
                sendto(sock, hello, (int)strlen(hello), 0,
                       reinterpret_cast<sockaddr*>(&dest_addr), sizeof(dest_addr));
                last_hello = now;
            }
        }

        // ── Capture ─────────────────────────────────────────────────────
        cap.read_frames(accum);

        // When loopback is silent (nothing playing), WASAPI may not deliver frames.
        // Sleep briefly to avoid busy-looping.
        if (accum.size() < frames_per_pkt * channels) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            cap.read_frames(accum);
        }

        // ── Packetize ───────────────────────────────────────────────────
        while (accum.size() >= frames_per_pkt * channels) {
            size_t pkt_len = build_ostp_packet(
                pkt_buf, sizeof(pkt_buf),
                seq, rtp_ts, ssrc,
                channels,
                accum.data(), frames_per_pkt);

            if (pkt_len > 0) {
                sendto(sock, reinterpret_cast<const char*>(pkt_buf), (int)pkt_len, 0,
                       reinterpret_cast<sockaddr*>(&dest_addr), sizeof(dest_addr));
                pkts_sent++;
                seq++;
                rtp_ts += kFramesPerPkt; // advance by 48kHz nominal frames
            }

            // Consume frames from accumulator
            accum.erase(accum.begin(), accum.begin() + frames_per_pkt * channels);
        }

        // ── Stats every 10s ─────────────────────────────────────────────
        double now = now_sec();
        if (now - last_stat >= 10.0) {
            fprintf(stderr, "[tx] Packets sent: %llu (%.1f pkt/s)\n",
                    (unsigned long long)pkts_sent,
                    (double)pkts_sent / (now - last_stat + 1e-9));
            pkts_sent = 0;
            last_stat = now;
        }
    }

    fprintf(stderr, "\n[tx] Stopping.\n");

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    cap.close();
    closesocket(sock);
    CoUninitialize();
    WSACleanup();
    return 0;
}
