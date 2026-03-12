/**
 * soluna-rx-win — Soluna Windows CLI multicast receiver (WASAPI output)
 *
 * Receives RTP/OSTP audio from the Soluna multicast stream and outputs
 * to WASAPI (shared mode) or stdout (pipe).  Uses a ring buffer + separate
 * playback thread with drift correction matching iOS/Mac/Linux for
 * cross-device sync.
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
 *   --buffer <ms>     Buffer target in ms (default: 60, matched with iOS/Mac)
 *   --sync            Enable media_timestamp-based sync mode
 *   --sync-delay <ms> Target end-to-end delay in ms (default: 200)
 *   --record <path>   Record received audio to WAV file
 *   --record-dir <dir> Record to directory as rx_<timestamp>.wav
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
#include <ctime>
#include <chrono>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <thread>

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
    uint32_t media_timestamp; // wall-clock capture time (ns since epoch, network byte order)
};
#pragma pack(pop)

static constexpr uint32_t kOstpMagic  = 0x4F535450; // 'OSTP'
static constexpr size_t   kMaxPktSize = 65536;

// Opus payload type (RFC 7587 dynamic, commonly 111)
static constexpr uint8_t kPtOpus = 111;

// ── Sync parameters (matched with iOS/Mac/Linux) ────────────────────────────
// These MUST stay identical across all platforms for cross-device sync.

static constexpr uint32_t kDefaultRate       = 48000;
static constexpr uint32_t kTargetFillMs      = 60;     // 60ms — matched with iOS/Mac
static constexpr uint32_t kRingCapFrames     = 192000;  // 4s capacity — matched with iOS/Mac
static constexpr uint32_t kAlsaPeriodFrames  = 512;     // ~10.7ms @ 48kHz — matched with iOS/Mac
static constexpr float    kFadeIn            = 0.002f;  // matched with iOS/Mac
static constexpr float    kFadeOut           = 0.004f;  // matched with iOS/Mac
static constexpr float    kSampleScale       = 8388608.0f; // 2^23 — matched with iOS/Mac

// ── Lock-free SPSC ring buffer ────────────────────────────────────────────────
// Single-producer (network thread), single-consumer (WASAPI playback thread).

class SpscRingBuffer {
public:
    SpscRingBuffer(size_t capacity_frames, uint32_t channels)
        : channels_(channels)
        , capacity_(next_pow2(capacity_frames))
        , mask_(capacity_ - 1)
        , buf_(capacity_ * channels)
    {}

    // Write frames into ring buffer. Returns frames actually written.
    size_t write(const int32_t* data, size_t frames) {
        size_t wr = write_pos_.load(std::memory_order_relaxed);
        size_t rd = read_pos_.load(std::memory_order_acquire);
        size_t avail = capacity_ - (wr - rd);
        size_t to_write = (std::min)(frames, avail);
        for (size_t f = 0; f < to_write; f++) {
            size_t idx = (wr + f) & mask_;
            for (uint32_t ch = 0; ch < channels_; ch++) {
                buf_[idx * channels_ + ch] = data[f * channels_ + ch];
            }
        }
        write_pos_.store(wr + to_write, std::memory_order_release);
        return to_write;
    }

    // Read frames from ring buffer. Returns frames actually read.
    size_t read(int32_t* data, size_t frames) {
        size_t rd = read_pos_.load(std::memory_order_relaxed);
        size_t wr = write_pos_.load(std::memory_order_acquire);
        size_t avail = wr - rd;
        size_t to_read = (std::min)(frames, avail);
        for (size_t f = 0; f < to_read; f++) {
            size_t idx = (rd + f) & mask_;
            for (uint32_t ch = 0; ch < channels_; ch++) {
                data[f * channels_ + ch] = buf_[idx * channels_ + ch];
            }
        }
        read_pos_.store(rd + to_read, std::memory_order_release);
        return to_read;
    }

    // Discard frames from read side without copying
    void discard(size_t frames) {
        size_t rd = read_pos_.load(std::memory_order_relaxed);
        size_t wr = write_pos_.load(std::memory_order_acquire);
        size_t avail = wr - rd;
        size_t to_discard = (std::min)(frames, avail);
        read_pos_.store(rd + to_discard, std::memory_order_release);
    }

    size_t available_read() const {
        size_t wr = write_pos_.load(std::memory_order_acquire);
        size_t rd = read_pos_.load(std::memory_order_relaxed);
        return wr - rd;
    }

    void reset() {
        write_pos_.store(0, std::memory_order_seq_cst);
        read_pos_.store(0, std::memory_order_seq_cst);
    }

private:
    static size_t next_pow2(size_t v) {
        v--;
        v |= v >> 1; v |= v >> 2; v |= v >> 4;
        v |= v >> 8; v |= v >> 16;
        return v + 1;
    }

    uint32_t channels_;
    size_t   capacity_;
    size_t   mask_;
    std::vector<int32_t> buf_;
    std::atomic<size_t> write_pos_{0};
    std::atomic<size_t> read_pos_{0};
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
    WORD                bits_per_sample = 16;
    bool                is_float     = false;

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
                    channels        = mix_fmt->nChannels;
                    sample_rate     = mix_fmt->nSamplesPerSec;
                    bits_per_sample = mix_fmt->wBitsPerSample;
                    is_float        = (mix_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
                    // Check WAVEFORMATEXTENSIBLE for float sub-format
                    if (mix_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix_fmt->cbSize >= 22) {
                        WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix_fmt);
                        // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {00000003-0000-0010-8000-00aa00389b71}
                        static const GUID kFloat = {0x00000003, 0x0000, 0x0010,
                            {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
                        is_float = (memcmp(&ext->SubFormat, &kFloat, sizeof(GUID)) == 0);
                    }
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

    // Write S16LE frames into the WASAPI render buffer, converting to device
    // format (S16/S32/float) as needed.  Returns frames written.
    UINT32 write_frames(const int16_t* data, UINT32 frames) {
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

        if (is_float && bits_per_sample == 32) {
            // Convert S16 -> float32 for WASAPI device
            float* dst = reinterpret_cast<float*>(buf);
            for (UINT32 i = 0; i < to_write * channels; i++) {
                dst[i] = (float)data[i] / 32768.0f;
            }
        } else if (bits_per_sample == 32) {
            // Convert S16 -> S32 for WASAPI device
            int32_t* dst = reinterpret_cast<int32_t*>(buf);
            for (UINT32 i = 0; i < to_write * channels; i++) {
                dst[i] = (int32_t)data[i] << 16;
            }
        } else {
            // S16 — direct copy
            memcpy(buf, data, to_write * channels * sizeof(int16_t));
        }

        render->ReleaseBuffer(to_write, 0);
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

// ── Playback thread (consumes ring buffer -> WASAPI/pipe) ─────────────────────
// This thread mirrors the audio_callback in iOS/Mac AudioReceiverBridge.mm
// and the playback_thread_func in linux-rx with identical parameters for
// cross-device sync.

struct PlaybackState {
    SpscRingBuffer* ring = nullptr;
    uint32_t channels = 2;
    uint32_t rate = kDefaultRate;
    uint32_t target_fill_frames = 0; // set from --buffer
    bool     use_wasapi = true;
    std::string wasapi_dev;
    std::atomic<float> volume{1.0f};
    std::atomic<bool>  muted{false};

    // Metrics (written by playback thread, read by main for reporting)
    std::atomic<uint32_t> underruns{0};
    std::atomic<uint64_t> frames_played{0};

    // WAV recording (accessed only by playback thread)
    WavWriter* wav = nullptr;
    uint64_t record_frames_max = 0;
    uint64_t record_frames_written = 0;
};

static void playback_thread_func(PlaybackState* st) {
    // Initialize COM on this thread for WASAPI
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        fprintf(stderr, "[rx] Playback thread: CoInitializeEx failed: 0x%08lx\n", hr);
        return;
    }

    const uint32_t frame_count = kAlsaPeriodFrames;
    const uint32_t ch = st->channels;
    const uint32_t total_samples = frame_count * ch;

    std::vector<int32_t> read_buf(total_samples);
    std::vector<int16_t> out_buf(total_samples);
    std::vector<float>   held_sample(ch, 0.0f);

    bool  prefilled = false;
    float ramp = 0.0f;
    int   drift_xfade = 0;

    // Open WASAPI on the playback thread
    WasapiOutput wasapi;
    if (st->use_wasapi) {
        if (!wasapi.open(st->wasapi_dev, ch, st->rate)) {
            fprintf(stderr, "[rx] Playback thread: WASAPI open failed, exiting\n");
            CoUninitialize();
            return;
        }
        if (!wasapi.start()) {
            fprintf(stderr, "[rx] Playback thread: WASAPI start failed, exiting\n");
            wasapi.close();
            CoUninitialize();
            return;
        }
        fprintf(stderr, "[rx] WASAPI output: %uHz %uch %ubit%s (buffer %u frames)\n",
                wasapi.sample_rate, wasapi.channels, wasapi.bits_per_sample,
                wasapi.is_float ? " float" : "",
                wasapi.buffer_frames);
    }

    while (g_running) {
        const float vol = st->muted.load() ? 0.0f : st->volume.load();

        // Adaptive target: >= frame_count*3 — matched with iOS/Mac
        uint32_t target = st->target_fill_frames;
        const uint32_t min_target = frame_count * 3;
        if (target < min_target) target = min_target;

        // ── Gradual drift correction — matched with iOS/Mac ──────────
        // Trigger at 3x target, discard frame_count/80+1 per period
        {
            size_t avail_now = st->ring->available_read();
            if (prefilled && avail_now > static_cast<size_t>(target) * 3) {
                size_t excess = avail_now - static_cast<size_t>(target) * 2;
                size_t drift = (std::min)(excess, static_cast<size_t>(frame_count / 80 + 1));
                st->ring->discard(drift);
                drift_xfade = 48;
            }
        }

        const size_t avail = st->ring->available_read();

        // ── Initial prefill (NOT reset on underrun) — matched with iOS/Mac ──
        if (!prefilled) {
            if (avail < min_target) {
                // Not enough data yet — sleep briefly to avoid busy-waiting
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            prefilled = true;
            ramp = 0.0f;
        }

        // ── Underrun: play what we have + fade out — matched with iOS/Mac ──
        if (avail < frame_count) {
            st->underruns.fetch_add(1, std::memory_order_relaxed);
            const size_t have = avail;
            size_t filled = 0;

            if (have > 0) {
                st->ring->read(read_buf.data(), have);
                for (size_t i = 0; i < have; i++) {
                    ramp += kFadeIn * (vol - ramp);
                    for (uint32_t c = 0; c < ch; c++) {
                        uint32_t idx = (uint32_t)i * ch + c;
                        float s = static_cast<float>(read_buf[idx]) / kSampleScale;
                        if (s > 1.0f) s = 1.0f;
                        else if (s < -1.0f) s = -1.0f;
                        float out = s * ramp;
                        out_buf[idx] = (int16_t)(out * 32767.0f);
                        held_sample[c] = out;
                    }
                }
                filled = have;
            }
            // Fade out remainder
            for (size_t i = filled; i < frame_count; i++) {
                ramp *= (1.0f - kFadeOut);
                for (uint32_t c = 0; c < ch; c++) {
                    out_buf[i * ch + c] = (int16_t)(held_sample[c] * ramp * 32767.0f);
                }
            }
        } else {
            // ── Normal playback ──────────────────────────────────────────
            st->ring->read(read_buf.data(), frame_count);
            for (uint32_t i = 0; i < frame_count; i++) {
                ramp += kFadeIn * (vol - ramp);
                for (uint32_t c = 0; c < ch; c++) {
                    uint32_t idx = i * ch + c;
                    float s = static_cast<float>(read_buf[idx]) / kSampleScale;
                    if (s > 1.0f) s = 1.0f;
                    else if (s < -1.0f) s = -1.0f;
                    float out = s * ramp;
                    // Drift crossfade — matched with iOS/Mac
                    if (drift_xfade > 0) {
                        float alpha = 1.0f - static_cast<float>(drift_xfade) / 49.0f;
                        out = out * alpha + held_sample[c] * (1.0f - alpha);
                    }
                    out_buf[idx] = (int16_t)(out * 32767.0f);
                    held_sample[c] = out;
                }
                if (drift_xfade > 0) drift_xfade--;
            }
        }

        st->frames_played.fetch_add(frame_count, std::memory_order_relaxed);

        // WAV recording
        if (st->wav && st->wav->fp && st->record_frames_written < st->record_frames_max) {
            size_t remaining = (size_t)(st->record_frames_max - st->record_frames_written);
            size_t to_write = frame_count < remaining ? frame_count : remaining;
            st->wav->write(out_buf.data(), to_write);
            st->record_frames_written += to_write;
            if (st->record_frames_written >= st->record_frames_max) {
                st->wav->close();
                fprintf(stderr, "[rx] Recording complete: %llu frames\n",
                        (unsigned long long)st->record_frames_written);
            }
        }

        // Output
        if (!st->use_wasapi) {
            fwrite(out_buf.data(), sizeof(int16_t) * ch, frame_count, stdout);
            fflush(stdout);
        } else {
            // Write to WASAPI, retrying if buffer is full
            UINT32 written = 0;
            UINT32 remaining = frame_count;
            const int16_t* ptr = out_buf.data();
            int retries = 0;
            while (remaining > 0 && g_running && retries < 50) {
                UINT32 n = wasapi.write_frames(ptr, remaining);
                if (n > 0) {
                    written += n;
                    remaining -= n;
                    ptr += n * ch;
                    retries = 0;
                } else {
                    // WASAPI buffer full — sleep briefly and retry
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    retries++;
                }
            }
            if (remaining > 0) {
                // Could not write all frames — count as underrun
                st->underruns.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    if (st->use_wasapi) {
        wasapi.close();
    }
    CoUninitialize();
}

// ── Main (network receive thread) ─────────────────────────────────────────────

int main(int argc, char** argv) {
    std::string group    = "239.69.0.1";
    uint16_t    port     = 5004;
    uint32_t    channels = 2;
    uint32_t    rate     = kDefaultRate;
    bool        use_wasapi = true;
    std::string wasapi_dev;              // empty = system default
    std::string record_path;
    uint32_t    record_duration = 30;
    std::string record_dir;
    bool        metrics_enabled = false;
    uint32_t    metrics_interval = 5;
    uint32_t    buffer_ms = kTargetFillMs;  // 60ms — matched with iOS/Mac
    bool        sync_mode = false;
    uint32_t    sync_delay_ms = 200;
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
        else if (a == "--buffer")   buffer_ms = (uint32_t)atoi(next());
        else if (a == "--record")   record_path = next();
        else if (a == "--duration") record_duration = (uint32_t)atoi(next());
        else if (a == "--record-dir") record_dir = next();
        else if (a == "--sync")     sync_mode = true;
        else if (a == "--sync-delay") sync_delay_ms = (uint32_t)atoi(next());
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
                "soluna-rx-win — Soluna Windows multicast audio receiver (sync-capable)\n\n"
                "  --group <ip>           Multicast group (default: 239.69.0.1)\n"
                "  --port <n>             UDP port        (default: 5004)\n"
                "  --peer <host:port>     P2P unicast via solunad relay\n"
                "  --relay <host:port>    WAN relay mode via soluna-relay\n"
                "  --group-name <name>    Group name for WAN relay (default: default)\n"
                "  --group-password <pw>  Group password for WAN relay (optional)\n"
                "  --channels <n>         Channels        (default: 2)\n"
                "  --output wasapi        Output to WASAPI (default)\n"
                "  --output pipe          Output raw S16LE to stdout\n"
                "  --device <id>          WASAPI device ID (default: system default)\n"
                "  --buffer <ms>          Buffer target   (default: 60, sync'd with iOS/Mac)\n"
                "  --sync                 Enable media_timestamp sync mode\n"
                "  --sync-delay <ms>      Target end-to-end delay (default: 200)\n"
                "  --record <path>        Record to WAV    (for analysis)\n"
                "  --record-dir <dir>     Record to dir as rx_<timestamp>.wav\n"
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

    // ── Ring buffer + playback thread ────────────────────────────────────
    SpscRingBuffer ring(kRingCapFrames, channels);
    uint32_t target_fill_frames = (buffer_ms * rate) / 1000; // 60ms = 2880 frames

    fprintf(stderr, "[rx] Ring buffer: %u frames (%.1fs), target fill: %u frames (%ums)\n",
            kRingCapFrames, (float)kRingCapFrames / rate,
            target_fill_frames, buffer_ms);
    if (sync_mode) {
        fprintf(stderr, "[rx] Sync mode ON: target end-to-end delay %ums\n", sync_delay_ms);
    }

    // --record-dir: auto-generate timestamped filename
    if (record_path.empty() && !record_dir.empty()) {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        struct tm tm_buf;
        localtime_s(&tm_buf, &t);
        char ts[256];
        snprintf(ts, sizeof(ts), "%s\\rx_%04d%02d%02d_%02d%02d%02d.wav",
                 record_dir.c_str(),
                 tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                 tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
        record_path = ts;
    }

    WavWriter wav;
    uint64_t record_frames_max = 0;
    if (!record_path.empty()) {
        if (!wav.open(record_path.c_str(), channels, rate)) {
            fprintf(stderr, "[rx] Cannot open WAV file '%s'\n", record_path.c_str());
        } else {
            record_frames_max = (uint64_t)record_duration * rate;
            fprintf(stderr, "[rx] Recording to %s (%u seconds)\n",
                    record_path.c_str(), record_duration);
        }
    }

    PlaybackState pstate;
    pstate.ring = &ring;
    pstate.channels = channels;
    pstate.rate = rate;
    pstate.target_fill_frames = target_fill_frames;
    pstate.use_wasapi = use_wasapi;
    pstate.wasapi_dev = wasapi_dev;
    pstate.wav = &wav;
    pstate.record_frames_max = record_frames_max;

    std::thread playback(playback_thread_func, &pstate);

    if (relay_mode) {
        fprintf(stderr, "[rx] Listening (WAN relay)%s\n",
                metrics_enabled ? " [metrics ON]" : "");
    } else if (peer_mode) {
        fprintf(stderr, "[rx] Listening (P2P relay)%s\n",
                metrics_enabled ? " [metrics ON]" : "");
    } else {
        fprintf(stderr, "[rx] Listening on %s:%u%s\n", group.c_str(), port,
                metrics_enabled ? " [metrics ON]" : "");
    }

    // ── Receive loop (network thread) ────────────────────────────────────

    static uint8_t pkt[kMaxPktSize];
    // Temporary buffer for decoded int32 samples before writing to ring
    static int32_t decode_buf[8192];
    // Temporary buffer for metrics (S16 conversion)
    static int16_t metrics_buf[8192];

    QualityMetrics metrics;
    double last_metrics_time = now_sec();
    int32_t last_seq = -1;
    uint64_t total_rx = 0, total_drop = 0;
    double last_hello_time = now_sec();
    double sync_ema_fill_frames = (double)target_fill_frames; // EMA for sync mode
    bool opus_warned = false;

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
            if (metrics_enabled) {
                double now = now_sec();
                if (now - last_metrics_time >= metrics_interval) {
                    metrics.underruns = pstate.underruns.load(std::memory_order_relaxed);
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

        // ── Sync mode: adjust target fill from OSTP media_timestamp ──
        if (sync_mode && is_ostp) {
            const OstpHeader* ostp = reinterpret_cast<const OstpHeader*>(pkt + sizeof(RtpHeader));
            uint32_t media_ts_raw = ntohl(ostp->media_timestamp);
            if (media_ts_raw != 0) {
                uint64_t now_ns = (uint64_t)(now_sec() * 1e9);
                uint32_t now_32 = (uint32_t)(now_ns & 0xFFFFFFFF);
                int32_t network_delay_ns = (int32_t)(now_32 - media_ts_raw);
                if (network_delay_ns < 0) network_delay_ns = 0;
                double network_delay_ms = (double)network_delay_ns / 1e6;
                double ideal_buffer_ms = (double)sync_delay_ms - network_delay_ms;
                if (ideal_buffer_ms < 10.0) ideal_buffer_ms = 10.0;
                if (ideal_buffer_ms > 500.0) ideal_buffer_ms = 500.0;
                double ideal_frames = ideal_buffer_ms * rate / 1000.0;
                sync_ema_fill_frames += 0.01 * (ideal_frames - sync_ema_fill_frames);
                pstate.target_fill_frames = (uint32_t)sync_ema_fill_frames;
            }
        }

        const uint8_t* payload   = pkt + payload_off;
        size_t         payload_n = (size_t)n - payload_off;

        // ── Sequence tracking ────────────────────────────────────────────
        uint16_t seq = ntohs(rtp->sequence);
        if (last_seq >= 0) {
            int diff = (int)(uint16_t)(seq - (uint16_t)last_seq);
            if (diff == 0) continue; // duplicate
            if (diff > 1 && diff < 100) {
                uint64_t lost = (uint64_t)(diff - 1);
                total_drop += lost;
                metrics.pkts_drop += lost;
            }
        }
        last_seq = seq;
        total_rx++;
        metrics.pkts_rx++;

        // ── Decode samples into int32 (ring buffer native format — matched with iOS/Mac) ──
        size_t frames;
        if (is_ostp) {
            // OSTP: int32 samples (24-bit range), keep as-is
            frames = payload_n / (sizeof(int32_t) * channels);
            size_t samples = frames * channels;
            size_t max_samples = sizeof(decode_buf) / sizeof(int32_t);
            if (samples > max_samples) samples = max_samples;
            memcpy(decode_buf, payload, samples * sizeof(int32_t));
            frames = samples / channels;
        } else {
            // RTP: int16 samples, convert to int32 (shift left 8 to match 24-bit range)
            frames = payload_n / (sizeof(int16_t) * channels);
            const int16_t* src = reinterpret_cast<const int16_t*>(payload);
            size_t samples = frames * channels;
            size_t max_samples = sizeof(decode_buf) / sizeof(int32_t);
            if (samples > max_samples) samples = max_samples;
            for (size_t i = 0; i < samples; i++) {
                decode_buf[i] = (int32_t)((int16_t)ntohs((uint16_t)src[i])) << 8;
            }
            frames = samples / channels;
        }

        if (frames == 0) continue;

        // Write to ring buffer (producer side)
        size_t written = ring.write(decode_buf, frames);
        if (written < frames) {
            // Ring overflow — discard oldest data to make room
            ring.discard(frames - written);
            ring.write(decode_buf + written * channels, frames - written);
        }

        // Quality metrics (convert to S16 for analysis)
        if (metrics_enabled) {
            size_t samples = frames * channels;
            for (size_t i = 0; i < samples && i < sizeof(metrics_buf)/sizeof(int16_t); i++) {
                metrics_buf[i] = (int16_t)(decode_buf[i] >> 8);
            }
            metrics.analyze_audio(metrics_buf, samples, channels, true);

            double now = now_sec();
            if (now - last_metrics_time >= metrics_interval) {
                metrics.underruns = pstate.underruns.load(std::memory_order_relaxed);
                metrics.print_json(rate);
                metrics.reset();
                last_metrics_time = now;
            }
        }
    }

    // Wait for playback thread to finish
    playback.join();

    if (wav.fp) {
        wav.close();
        fprintf(stderr, "[rx] Recording saved\n");
    }

    fprintf(stderr, "\n[rx] Stopped. Received %llu packets, dropped %llu, underruns %u\n",
        (unsigned long long)total_rx, (unsigned long long)total_drop,
        pstate.underruns.load());

    closesocket(sock);
    CoUninitialize();
    WSACleanup();
    return 0;
}
