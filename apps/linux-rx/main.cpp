/**
 * soluna — Unified Soluna CLI (RX receiver + DJ broadcaster)
 *
 * Modes:
 *   RX (default):  Receive RTP/OSTP audio → ALSA / stdout
 *   DJ (--dj):     Broadcast music files → relay via OSTP (hybrid PCM + file-sync)
 *
 * RX Usage:
 *   soluna [options]
 *   soluna --relay host:5100 --group-name music
 *
 * DJ Usage:
 *   soluna --dj --dir /data/music --relay host:5100 --channel soluna
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
#include <ctime>
#include <chrono>
#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <random>

#ifdef __linux__
#include <alsa/asoundlib.h>
#endif

#ifdef SOLUNA_HAS_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#endif

#ifdef SOLUNA_HAS_DLNA
#include <soluna/transport/dlna.h>
#endif

#ifdef SOLUNA_HAS_AIRPLAY
#include <soluna/transport/airplay.h>
#endif

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>

// ── RTP / OSTP headers (minimal, self-contained) ─────────────────────────────

struct RtpHeader {
    uint8_t  cc_x_p_v;   // V=2, P, X, CC
    uint8_t  m_pt;        // M, PT
    uint16_t sequence;
    uint32_t timestamp;
    uint32_t ssrc;
} __attribute__((packed));

struct RtpExtensionHeader {
    uint16_t profile_specific;  // 0x4F53 ("OS") for OSTP
    uint16_t length;            // 2 (32-bit words)
} __attribute__((packed));

static_assert(sizeof(RtpExtensionHeader) == 4, "RTP ext header must be 4 bytes");

struct OstpHeader {
    uint16_t stream_id;
    uint16_t sequence_ext;
    uint32_t media_timestamp; // wall-clock capture time (ns since epoch, network byte order)
} __attribute__((packed));

static_assert(sizeof(OstpHeader) == 8, "OSTP header must be 8 bytes");

static constexpr uint16_t kOstpProfile = 0x4F53; // "OS"
static constexpr size_t   kMaxPktSize = 65536;

// ── Sync parameters (matched with iOS/Mac) ────────────────────────────────────
// These MUST stay identical across all platforms for cross-device sync.

static constexpr uint32_t kDefaultRate     = 48000;
static constexpr uint32_t kTargetFillMs    = 60;    // 60ms — matched with iOS/Mac
static constexpr uint32_t kRingCapFrames   = 192000; // 4s capacity — matched with iOS/Mac
static constexpr uint32_t kAlsaPeriodFrames = 512;   // ~10.7ms @ 48kHz — matched with iOS/Mac
static constexpr float    kFadeIn          = 0.002f; // matched with iOS/Mac
static constexpr float    kFadeOut         = 0.004f; // matched with iOS/Mac
static constexpr float    kSampleScale     = 8388608.0f; // 2^23 — matched with iOS/Mac

// ── Lock-free SPSC ring buffer ────────────────────────────────────────────────
// Single-producer (network thread), single-consumer (ALSA playback thread).

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
        size_t to_write = std::min(frames, avail);
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
        size_t to_read = std::min(frames, avail);
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
        size_t to_discard = std::min(frames, avail);
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
        pkts_rx = 0; pkts_drop = 0;
        rms_sum = 0; rms_count = 0; peak = 0;
        clicks = 0; dropouts = 0; underruns = 0;
        in_dropout = false; low_energy_frames = 0;
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

// ── NTP Clock Sync state (matched with iOS/Mac WanRelayClient) ───────────────
static int64_t  g_clock_offset_ns   = 0;   // relay_time - local_time (ns)
static uint32_t g_sync_ping_count   = 0;   // number of pong samples received
static uint32_t g_last_media_ts     = 0;   // last OSTP media_timestamp (ms, 32-bit wrap)
static uint32_t g_sync_samples_count = 0;  // EMA convergence counter for buffer depth

// ── NTP Clock Sync: send ping (PT=125) — matched with iOS WanRelayClient ────
static void send_sync_ping(int sock, const sockaddr_in& relay_addr) {
    uint8_t pkt[25] = {};
    pkt[0] = 0x7D;  // PT=125 sync marker

    // T1 = local CLOCK_REALTIME nanoseconds (64-bit LE)
    struct timespec now_ts;
    clock_gettime(CLOCK_REALTIME, &now_ts);
    uint64_t t1_ns = (uint64_t)now_ts.tv_sec * 1000000000ULL + (uint64_t)now_ts.tv_nsec;
    memcpy(pkt + 1, &t1_ns, 8);
    // T2, T3 zeroed — relay will fill them

    sendto(sock, pkt, 25, 0,
           (const sockaddr*)&relay_addr, sizeof(relay_addr));
}

// ── NTP Clock Sync: handle pong — matched with iOS WanRelayClient ────────────
static void handle_sync_pong(const uint8_t* data, size_t len) {
    if (len < 25 || data[0] != 0x7D) return;

    // Extract T1, T2, T3 (all 64-bit LE nanoseconds)
    uint64_t t1_ns, t2_ns, t3_ns;
    memcpy(&t1_ns, data + 1, 8);
    memcpy(&t2_ns, data + 9, 8);
    memcpy(&t3_ns, data + 17, 8);

    // T4 = local receive time
    struct timespec now_ts;
    clock_gettime(CLOCK_REALTIME, &now_ts);
    uint64_t t4_ns = (uint64_t)now_ts.tv_sec * 1000000000ULL + (uint64_t)now_ts.tv_nsec;

    // Validate: T2 and T3 must be non-zero (relay filled them)
    if (t2_ns == 0 || t3_ns == 0) return;

    // NTP offset = ((T2-T1) + (T3-T4)) / 2
    int64_t offset = ((int64_t)(t2_ns - t1_ns) + (int64_t)(t3_ns - t4_ns)) / 2;
    int64_t rtt = (int64_t)(t4_ns - t1_ns) - (int64_t)(t3_ns - t2_ns);

    // Reject outliers: RTT > 500ms is unreliable
    if (rtt < 0 || rtt > 500000000LL) return;

    // EMA smoothing (α=0.3 for first 10 samples, then 0.1) — matched with iOS
    int64_t prev = g_clock_offset_ns;
    g_sync_ping_count++;

    // First measurement: jump directly
    if (g_sync_ping_count == 1) {
        g_clock_offset_ns = offset;
    } else if (g_sync_ping_count <= 10) {
        double alpha = 0.3;
        g_clock_offset_ns = (int64_t)(prev * (1.0 - alpha) + offset * alpha);
    } else {
        double alpha = 0.1;
        g_clock_offset_ns = (int64_t)(prev * (1.0 - alpha) + offset * alpha);
    }

    if (g_sync_ping_count <= 3) {
        fprintf(stderr, "[clock-sync] offset=%.2fms rtt=%.2fms (#%u)\n",
                offset / 1e6, rtt / 1e6, g_sync_ping_count);
    }
}

// ── DELAY reporting: send net delay to relay — matched with iOS ──────────────
static void send_delay_report(int sock, const sockaddr_in& relay_addr, uint32_t net_delay_ms) {
    char buf[64];
    snprintf(buf, sizeof(buf), "DELAY:%u\n", net_delay_ms);
    sendto(sock, buf, strlen(buf), 0,
           (const sockaddr*)&relay_addr, sizeof(relay_addr));
}

// ── ALSA helpers ──────────────────────────────────────────────────────────────

#ifdef __linux__
static snd_pcm_t* alsa_open(const char* device, unsigned rate, unsigned channels,
                              snd_pcm_uframes_t period_frames) {
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
    // Set period size close to our target for consistent callback timing
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &period_frames, nullptr);
    // 4 periods in buffer
    snd_pcm_uframes_t buffer_frames = period_frames * 4;
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer_frames);
    if (snd_pcm_hw_params(pcm, hw) < 0) {
        fprintf(stderr, "[rx] ALSA hw_params failed\n");
        snd_pcm_close(pcm);
        return nullptr;
    }
    snd_pcm_prepare(pcm);

    // Report actual params
    snd_pcm_uframes_t actual_period, actual_buffer;
    snd_pcm_hw_params_get_period_size(hw, &actual_period, nullptr);
    snd_pcm_hw_params_get_buffer_size(hw, &actual_buffer);
    fprintf(stderr, "[rx] ALSA period=%lu buffer=%lu frames\n",
            (unsigned long)actual_period, (unsigned long)actual_buffer);

    return pcm;
}
#endif

// ── Playback thread (consumes ring buffer → ALSA/pipe) ───────────────────────
// This thread mirrors the audio_callback in iOS/Mac AudioReceiverBridge.mm
// with identical parameters for cross-device sync.

struct PlaybackState {
    SpscRingBuffer* ring = nullptr;
    uint32_t channels = 2;
    uint32_t rate = kDefaultRate;
    uint32_t target_fill_frames = 0; // set from --buffer
    bool     use_alsa = true;
    bool     use_pipewire = false;
    std::atomic<float> volume{1.0f};
    std::atomic<bool>  muted{false};

    // Metrics (written by playback thread, read by main for reporting)
    std::atomic<uint32_t> underruns{0};
    std::atomic<uint64_t> frames_played{0};

    // WAV recording (accessed only by playback thread)
    WavWriter* wav = nullptr;
    uint64_t record_frames_max = 0;
    uint64_t record_frames_written = 0;

#ifdef SOLUNA_HAS_AIRPLAY
    // AirPlay TX: forward audio to local speakers (non-owning pointer)
    soluna::transport::AirPlaySender* airplay_sender = nullptr;
#endif
};

static void playback_thread_func(PlaybackState* st) {
    const uint32_t frame_count = kAlsaPeriodFrames;
    const uint32_t ch = st->channels;
    const uint32_t total_samples = frame_count * ch;

    std::vector<int32_t> read_buf(total_samples);
    std::vector<int16_t> out_buf(total_samples);
    std::vector<float>   held_sample(ch, 0.0f);

    bool  prefilled = false;
    float ramp = 0.0f;
    int   drift_xfade = 0;

#ifdef __linux__
    snd_pcm_t* pcm = nullptr;
    if (st->use_alsa) {
        pcm = alsa_open("default", st->rate, ch, frame_count);
        if (!pcm) {
            fprintf(stderr, "[rx] ALSA playback thread: open failed, exiting\n");
            return;
        }
    }
#endif

#ifdef SOLUNA_HAS_PIPEWIRE
    // PipeWire output: secondary ring buffer fed by this thread, drained by PW callback
    SpscRingBuffer pw_ring(kRingCapFrames, ch);
    pw_thread_loop* pw_loop = nullptr;
    pw_stream* pw_stream_handle = nullptr;

    struct PwSinkCtx {
        SpscRingBuffer* ring;
        pw_stream*      stream;
        uint32_t        channels;
        uint32_t        frames_per_buffer;
    };
    static PwSinkCtx pw_ctx; // static so the callback can reference it safely

    if (st->use_pipewire) {
        pw_init(nullptr, nullptr);

        pw_ctx.ring = &pw_ring;
        pw_ctx.stream = nullptr;
        pw_ctx.channels = ch;
        pw_ctx.frames_per_buffer = frame_count;

        pw_loop = pw_thread_loop_new("soluna-rx", nullptr);
        if (!pw_loop) {
            fprintf(stderr, "[rx] PipeWire: failed to create thread loop\n");
            return;
        }

        static const pw_stream_events pw_sink_events = {
            .version = PW_VERSION_STREAM_EVENTS,
            .process = [](void* userdata) {
                auto* ctx = static_cast<PwSinkCtx*>(userdata);
                pw_buffer* b = pw_stream_dequeue_buffer(ctx->stream);
                if (!b) return;

                spa_buffer* buf = b->buffer;
                auto* dst = static_cast<int16_t*>(buf->datas[0].data);
                if (!dst) {
                    pw_stream_queue_buffer(ctx->stream, b);
                    return;
                }

                uint32_t max_frames = buf->datas[0].maxsize /
                                      (sizeof(int16_t) * ctx->channels);
                uint32_t n_frames = ctx->frames_per_buffer;
                if (n_frames > max_frames) n_frames = max_frames;

                // Ring stores int16_t values widened to int32_t; narrow back
                std::vector<int32_t> tmp(n_frames * ctx->channels);
                size_t got = ctx->ring->read(tmp.data(), n_frames);
                for (size_t i = 0; i < got * ctx->channels; i++) {
                    dst[i] = static_cast<int16_t>(tmp[i]);
                }
                if (got < n_frames) {
                    std::memset(dst + got * ctx->channels, 0,
                                (n_frames - got) * ctx->channels * sizeof(int16_t));
                }

                buf->datas[0].chunk->offset = 0;
                buf->datas[0].chunk->stride = static_cast<int32_t>(sizeof(int16_t) * ctx->channels);
                buf->datas[0].chunk->size   = n_frames * ctx->channels * sizeof(int16_t);
                pw_stream_queue_buffer(ctx->stream, b);
            },
        };

        pw_properties* props = pw_properties_new(
            PW_KEY_MEDIA_TYPE,     "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE,     "Music",
            PW_KEY_APP_NAME,       "Soluna",
            nullptr);

        pw_stream_handle = pw_stream_new_simple(
            pw_thread_loop_get_loop(pw_loop),
            "soluna-rx",
            props,
            &pw_sink_events,
            &pw_ctx);

        if (!pw_stream_handle) {
            fprintf(stderr, "[rx] PipeWire: failed to create stream\n");
            pw_thread_loop_destroy(pw_loop);
            return;
        }
        pw_ctx.stream = pw_stream_handle;

        uint8_t params_buf[1024];
        spa_pod_builder builder;
        spa_pod_builder_init(&builder, params_buf, sizeof(params_buf));

        spa_audio_info_raw audio_info;
        spa_zero(audio_info);
        audio_info.format   = SPA_AUDIO_FORMAT_S16;
        audio_info.rate     = st->rate;
        audio_info.channels = ch;

        const spa_pod* params[1];
        params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &audio_info);

        int res = pw_stream_connect(
            pw_stream_handle,
            PW_DIRECTION_OUTPUT,
            PW_ID_ANY,
            static_cast<pw_stream_flags>(
                PW_STREAM_FLAG_AUTOCONNECT |
                PW_STREAM_FLAG_MAP_BUFFERS |
                PW_STREAM_FLAG_RT_PROCESS),
            params, 1);

        if (res < 0) {
            fprintf(stderr, "[rx] PipeWire: stream connect failed: %s\n", spa_strerror(res));
            pw_stream_destroy(pw_stream_handle);
            pw_thread_loop_destroy(pw_loop);
            return;
        }

        pw_thread_loop_start(pw_loop);
        fprintf(stderr, "[rx] PipeWire output: %uHz %uch S16\n", st->rate, ch);
    }
#endif

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
                size_t drift = std::min(excess, static_cast<size_t>(frame_count / 80 + 1));
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

#ifdef SOLUNA_HAS_AIRPLAY
        // AirPlay TX: forward audio to local AirPlay speakers
        if (st->airplay_sender && st->airplay_sender->is_running()) {
            st->airplay_sender->send_audio(out_buf.data(), frame_count,
                                            static_cast<uint8_t>(ch), st->rate);
        }
#endif

        // Output
        if (!st->use_alsa && !st->use_pipewire) {
            fwrite(out_buf.data(), sizeof(int16_t) * ch, frame_count, stdout);
            fflush(stdout);
        }
#ifdef SOLUNA_HAS_PIPEWIRE
        else if (st->use_pipewire) {
            // Widen int16_t → int32_t for the SPSC ring buffer, then PW callback narrows back
            std::vector<int32_t> pw_tmp(frame_count * ch);
            for (uint32_t i = 0; i < frame_count * ch; i++) {
                pw_tmp[i] = static_cast<int32_t>(out_buf[i]);
            }
            pw_ring.write(pw_tmp.data(), frame_count);
            // PipeWire drives timing via its own thread; yield briefly to avoid busy-spin
            std::this_thread::sleep_for(std::chrono::microseconds(
                static_cast<uint64_t>(frame_count) * 1000000 / st->rate));
        }
#endif
#ifdef __linux__
        else if (pcm) {
            snd_pcm_sframes_t n = snd_pcm_writei(pcm, out_buf.data(),
                                                   (snd_pcm_uframes_t)frame_count);
            if (n == -EPIPE) {
                st->underruns.fetch_add(1, std::memory_order_relaxed);
                snd_pcm_prepare(pcm);
                snd_pcm_writei(pcm, out_buf.data(), (snd_pcm_uframes_t)frame_count);
            }
        }
#endif
    }

#ifdef __linux__
    if (pcm) { snd_pcm_drain(pcm); snd_pcm_close(pcm); }
#endif
#ifdef SOLUNA_HAS_PIPEWIRE
    if (pw_loop) {
        pw_thread_loop_stop(pw_loop);
        if (pw_stream_handle) pw_stream_destroy(pw_stream_handle);
        pw_thread_loop_destroy(pw_loop);
        pw_deinit();
    }
#endif
}

// ══════════════════════════════════════════════════════════════════════════════
// DJ MODE — Broadcast music files via OSTP (hybrid PCM + file-sync)
// ══════════════════════════════════════════════════════════════════════════════

// ── CRC-32 (IEEE 802.3) ──────────────────────────────────────────────────────

static uint32_t crc32_calc(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}

// ── OSTP packet builder (for DJ TX) ─────────────────────────────────────────

static constexpr size_t   kRtpHeaderSize    = 12;
static constexpr size_t   kRtpExtHeaderSize = 4;
static constexpr size_t   kOstpExtDataSize  = 8;
static constexpr size_t   kTotalTxHeader    = kRtpHeaderSize + kRtpExtHeaderSize + kOstpExtDataSize;
static constexpr size_t   kCrcTrailerSize   = 4;
// kOstpProfile already defined above with struct declarations
static constexpr uint8_t  kPayloadTypePCM24 = 96;
static constexpr uint32_t kFramesPerPacket  = 480;      // 10ms @ 48kHz
static constexpr uint32_t kTxPayloadBytes   = kFramesPerPacket * 2 * 4; // stereo int32

static size_t build_ostp_packet(
    uint8_t* buf, size_t buf_size,
    uint32_t ssrc, uint16_t seq, uint32_t rtp_ts, uint8_t pt,
    uint16_t stream_id, uint16_t seq_ext, uint32_t media_ts,
    const void* payload, size_t payload_size)
{
    size_t total = kTotalTxHeader + payload_size + kCrcTrailerSize;
    if (buf_size < total) return 0;

    std::memset(buf, 0, kTotalTxHeader);
    buf[0] = (2 << 6) | (1 << 4);  // V=2, X=1
    buf[1] = pt;

    uint16_t seq_n = htons(seq);
    std::memcpy(buf + 2, &seq_n, 2);
    uint32_t ts_n = htonl(rtp_ts);
    std::memcpy(buf + 4, &ts_n, 4);
    uint32_t ssrc_n = htonl(ssrc);
    std::memcpy(buf + 8, &ssrc_n, 4);

    // RTP extension header
    uint16_t profile_n = htons(kOstpProfile);
    uint16_t extlen_n = htons(2);  // 2 x 32-bit words
    std::memcpy(buf + 12, &profile_n, 2);
    std::memcpy(buf + 14, &extlen_n, 2);

    // OSTP extension data
    uint16_t sid_n = htons(stream_id);
    uint16_t sext_n = htons(seq_ext);
    uint32_t mts_n = htonl(media_ts);
    std::memcpy(buf + 16, &sid_n, 2);
    std::memcpy(buf + 18, &sext_n, 2);
    std::memcpy(buf + 20, &mts_n, 4);

    if (payload && payload_size > 0)
        std::memcpy(buf + kTotalTxHeader, payload, payload_size);

    uint32_t c = crc32_calc(buf + kTotalTxHeader, payload_size);
    uint32_t c_n = htonl(c);
    std::memcpy(buf + kTotalTxHeader + payload_size, &c_n, 4);

    return total;
}

// ── List audio files in directory ────────────────────────────────────────────

static std::vector<std::string> list_audio_files(const std::string& dir) {
    std::vector<std::string> files;
    DIR* d = opendir(dir.c_str());
    if (!d) return files;
    struct dirent* entry;
    while ((entry = readdir(d))) {
        std::string name = entry->d_name;
        // Check 4-char extensions
        if (name.size() > 4) {
            std::string ext = name.substr(name.size() - 4);
            for (auto& c : ext) c = (char)tolower(c);
            if (ext == ".mp3" || ext == ".wav" || ext == ".m4a" || ext == ".aac")
                files.push_back(dir + "/" + name);
        }
        // Check 5-char extensions
        if (name.size() > 5) {
            std::string ext5 = name.substr(name.size() - 5);
            for (auto& c : ext5) c = (char)tolower(c);
            if (ext5 == ".flac" || ext5 == ".aiff" || ext5 == ".alac")
                files.push_back(dir + "/" + name);
        }
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    return files;
}

// ── Get file duration via ffprobe ────────────────────────────────────────────

static double get_duration_sec(const std::string& filepath) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "ffprobe -v error -show_entries format=duration "
        "-of default=noprint_wrappers=1:nokey=1 '%s' 2>/dev/null",
        filepath.c_str());
    FILE* fp = popen(cmd, "r");
    if (!fp) return 180.0;
    char buf[64]{};
    double dur = 180.0;
    if (fgets(buf, sizeof(buf), fp))
        dur = atof(buf) > 0 ? atof(buf) : 180.0;
    pclose(fp);
    return dur;
}

// ── Wall clock (milliseconds since epoch) ────────────────────────────────────

static uint64_t wall_clock_ms() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

// ── DJ main loop ─────────────────────────────────────────────────────────────

static int dj_main(const std::string& music_dir,
                    const std::string& relay_host, uint16_t relay_port,
                    const std::string& channel) {
    auto files = list_audio_files(music_dir);
    if (files.empty()) {
        fprintf(stderr, "[dj] No audio files in %s\n", music_dir.c_str());
        return 1;
    }
    fprintf(stderr, "[dj] Found %zu tracks (hybrid PCM + file-sync)\n", files.size());

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    sockaddr_in relay_addr{};
    relay_addr.sin_family = AF_INET;
    relay_addr.sin_port = htons(relay_port);
    inet_pton(AF_INET, relay_host.c_str(), &relay_addr.sin_addr);

    auto send_cmd = [&](const std::string& cmd) {
        sendto(sock, cmd.c_str(), cmd.size(), 0,
               (sockaddr*)&relay_addr, sizeof(relay_addr));
    };

    // Join channel
    std::string join_msg = "JOIN:" + channel + ":\n";
    send_cmd(join_msg);
    {
        struct timeval tv{3, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char buf[256];
        recvfrom(sock, buf, sizeof(buf), 0, nullptr, nullptr);
    }
    fprintf(stderr, "[dj] Joined channel '%s'\n", channel.c_str());

    // OSTP state
    uint8_t packet[kTotalTxHeader + kTxPayloadBytes + kCrcTrailerSize];
    uint32_t ssrc = 0x444A4D58; // 'DJMX'
    uint16_t seq = 0;
    uint32_t rtp_ts = 0;
    uint64_t total_frames = 0;
    auto stream_start = std::chrono::steady_clock::now();
    auto last_hello = stream_start;

    std::mt19937 rng(std::random_device{}());
    auto playlist = files;

    static constexpr int kDownloadWaitSec = 3;

    while (g_running) {
        std::shuffle(playlist.begin(), playlist.end(), rng);

        for (size_t fi = 0; fi < playlist.size() && g_running; fi++) {
            const std::string& filepath = playlist[fi];
            std::string fname = filepath.substr(filepath.rfind('/') + 1);
            double duration = get_duration_sec(filepath);

            fprintf(stderr, "[dj] Track %zu/%zu: %s (%.1fs)\n",
                    fi + 1, playlist.size(), fname.c_str(), duration);

            // Notify receivers: download file + metadata
            send_cmd("FILE:" + fname + "\n");
            send_cmd("META:{\"track\":\"" + fname + "\",\"source\":\"dj\"}\n");

            // Open ffmpeg for PCM decode
            char cmd[1024];
            snprintf(cmd, sizeof(cmd),
                "ffmpeg -v error -i '%s' -f s32le -acodec pcm_s32le "
                "-ar 48000 -ac 2 - 2>/dev/null",
                filepath.c_str());
            FILE* fp = popen(cmd, "r");
            if (!fp) {
                fprintf(stderr, "[dj] Failed to decode: %s\n", filepath.c_str());
                continue;
            }

            auto track_start = std::chrono::steady_clock::now();
            uint64_t track_frames = 0;
            bool sync_sent = false;

            int32_t pcm_buf[kFramesPerPacket * 2];
            while (g_running) {
                size_t read_samples = fread(pcm_buf, 4, kFramesPerPacket * 2, fp);
                if (read_samples == 0) break;

                size_t frames = read_samples / 2;
                if (frames < kFramesPerPacket)
                    memset(pcm_buf + read_samples, 0,
                           (kFramesPerPacket * 2 - read_samples) * 4);

                // Shift to 24-bit range
                int32_t payload[kFramesPerPacket * 2];
                for (uint32_t i = 0; i < kFramesPerPacket * 2; i++)
                    payload[i] = pcm_buf[i] >> 8;

                uint32_t media_ts = (uint32_t)(total_frames * 1000000000ULL / 48000);
                size_t pkt_size = build_ostp_packet(
                    packet, sizeof(packet),
                    ssrc, seq, rtp_ts, kPayloadTypePCM24,
                    1, (uint16_t)(seq >> 16), media_ts,
                    payload, kTxPayloadBytes);

                seq++;
                rtp_ts += kFramesPerPacket;
                total_frames += kFramesPerPacket;
                track_frames += kFramesPerPacket;

                if (pkt_size > 0)
                    sendto(sock, packet, pkt_size, 0,
                           (sockaddr*)&relay_addr, sizeof(relay_addr));

                // Pace to real-time
                auto target = stream_start + std::chrono::microseconds(
                    total_frames * 1000000ULL / 48000);
                auto now = std::chrono::steady_clock::now();
                if (target > now) std::this_thread::sleep_until(target);

                // Heartbeat
                now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - last_hello).count() >= 5) {
                    send_cmd("HELLO\n");
                    last_hello = now;
                }

                // After download wait, send SYNC:play for file-sync receivers
                if (!sync_sent) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        now - track_start).count();
                    if (elapsed >= kDownloadWaitSec) {
                        uint64_t pos_ms = (track_frames * 1000) / 48000;
                        uint64_t play_at = wall_clock_ms() + 300;
                        char sync_buf[128];
                        snprintf(sync_buf, sizeof(sync_buf), "SYNC:play:%llu:%llu\n",
                                 (unsigned long long)pos_ms,
                                 (unsigned long long)play_at);
                        send_cmd(std::string(sync_buf));
                        fprintf(stderr, "[dj] SYNC:play pos=%llums\n",
                                (unsigned long long)pos_ms);
                        sync_sent = true;
                    }
                }
            }

            pclose(fp);
            std::this_thread::sleep_for(std::chrono::seconds(2));

            // Reset timing for next track
            stream_start = std::chrono::steady_clock::now();
            total_frames = 0;

            if (g_running) send_cmd(join_msg);
        }
    }

    close(sock);
    fprintf(stderr, "[dj] Stopped.\n");
    return 0;
}

// ══════════════════════════════════════════════════════════════════════════════
// RX MODE — Receive RTP/OSTP audio → ALSA / pipe
// ══════════════════════════════════════════════════════════════════════════════

// Forward declaration
static int rx_main(int argc, char** argv);

// ── Main entry point ─────────────────────────────────────────────────────────

// ── Minimal SHA-1 (RFC 3174) for WebSocket handshake ──────────────────────────

namespace sha1 {
static void sha1_transform(uint32_t state[5], const uint8_t buf[64]) {
    uint32_t a, b, c, d, e, w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)buf[i*4]<<24 | (uint32_t)buf[i*4+1]<<16 |
               (uint32_t)buf[i*4+2]<<8 | buf[i*4+3];
    for (int i = 16; i < 80; i++) {
        uint32_t t = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
        w[i] = (t << 1) | (t >> 31);
    }
    a=state[0]; b=state[1]; c=state[2]; d=state[3]; e=state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if      (i < 20) { f = (b&c) | ((~b)&d);     k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;             k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b&c) | (b&d) | (c&d); k = 0x8F1BBCDC; }
        else              { f = b ^ c ^ d;             k = 0xCA62C1D6; }
        uint32_t t2 = ((a<<5)|(a>>27)) + f + e + k + w[i];
        e=d; d=c; c=(b<<30)|(b>>2); b=a; a=t2;
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d; state[4]+=e;
}
static void compute(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t state[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t buf[64]; size_t i = 0;
    for (; i + 64 <= len; i += 64) sha1_transform(state, data + i);
    size_t rem = len - i;
    memcpy(buf, data + i, rem);
    buf[rem++] = 0x80;
    if (rem > 56) { memset(buf + rem, 0, 64 - rem); sha1_transform(state, buf); rem = 0; }
    memset(buf + rem, 0, 56 - rem);
    uint64_t bits = (uint64_t)len * 8;
    for (int j = 0; j < 8; j++) buf[56 + j] = (uint8_t)(bits >> (56 - j*8));
    sha1_transform(state, buf);
    for (int j = 0; j < 5; j++) {
        out[j*4] = state[j]>>24; out[j*4+1] = state[j]>>16;
        out[j*4+2] = state[j]>>8; out[j*4+3] = state[j];
    }
}
} // namespace sha1

static std::string base64_encode(const uint8_t* data, size_t len) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string r; r.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)data[i] << 16;
        if (i+1 < len) n |= (uint32_t)data[i+1] << 8;
        if (i+2 < len) n |= data[i+2];
        r += t[(n>>18)&63]; r += t[(n>>12)&63];
        r += (i+1 < len) ? t[(n>>6)&63] : '=';
        r += (i+2 < len) ? t[n&63] : '=';
    }
    return r;
}

// ── Minimal WebSocket control server (port 8400) ──────────────────────────────
// Compatible with DaemonClient.swift on iOS/Mac.
// Runs in a background thread. Supports volume, mute, status queries.

struct WsControlServer {
    int listen_fd = -1;
    std::atomic<bool>* running = nullptr;
    PlaybackState* pstate = nullptr;
    std::string device_name;
    uint32_t channels = 2;
    uint32_t rate = 48000;

    // State shared with main
    std::atomic<uint64_t>* total_rx = nullptr;
    std::atomic<uint64_t>* total_drop = nullptr;

    bool start(uint16_t port) {
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) return false;
        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
            close(listen_fd); listen_fd = -1; return false;
        }
        listen(listen_fd, 4);
        // Non-blocking for poll
        fcntl(listen_fd, F_SETFL, O_NONBLOCK);
        return true;
    }

    void run() {
        while (running->load()) {
            pollfd pfd{listen_fd, POLLIN, 0};
            int r = poll(&pfd, 1, 500); // 500ms timeout
            if (r <= 0) continue;
            sockaddr_in client_addr{};
            socklen_t clen = sizeof(client_addr);
            int cfd = accept(listen_fd, (sockaddr*)&client_addr, &clen);
            if (cfd < 0) continue;
            handle_client(cfd);
        }
        if (listen_fd >= 0) close(listen_fd);
    }

    void handle_client(int fd) {
        // Read HTTP upgrade request
        char buf[4096];
        int n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { close(fd); return; }
        buf[n] = '\0';

        // Check for WebSocket upgrade
        std::string req(buf);

        // Handle plain HTTP GET /ws (for health check)
        if (req.find("Upgrade: websocket") == std::string::npos &&
            req.find("upgrade: websocket") == std::string::npos) {
            // Return simple JSON status
            char body[256];
            snprintf(body, sizeof(body),
                "{\"name\":\"%s\",\"platform\":\"linux\",\"mode\":\"rx\","
                "\"channels\":%u,\"rate\":%u,\"version\":\"0.3.1\"}",
                device_name.c_str(), channels, rate);
            char resp[512];
            snprintf(resp, sizeof(resp),
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: %zu\r\nAccess-Control-Allow-Origin: *\r\n\r\n%s",
                strlen(body), body);
            send(fd, resp, strlen(resp), 0);
            close(fd);
            return;
        }

        // Extract Sec-WebSocket-Key
        std::string ws_key;
        auto pos = req.find("Sec-WebSocket-Key:");
        if (pos == std::string::npos) pos = req.find("sec-websocket-key:");
        if (pos != std::string::npos) {
            auto start = pos + 18;
            while (start < req.size() && req[start] == ' ') start++;
            auto end = req.find("\r\n", start);
            if (end != std::string::npos) ws_key = req.substr(start, end - start);
        }
        if (ws_key.empty()) { close(fd); return; }

        // Compute accept key
        std::string concat = ws_key + "258EAFA5-E914-47DA-95CA-5AB5DC085B11";
        uint8_t hash[20];
        sha1::compute((const uint8_t*)concat.c_str(), concat.size(), hash);
        std::string accept = base64_encode(hash, 20);

        // Send upgrade response
        char upgrade[512];
        snprintf(upgrade, sizeof(upgrade),
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n\r\n", accept.c_str());
        send(fd, upgrade, strlen(upgrade), 0);

        fprintf(stderr, "[ws] Client connected\n");

        // Send initial status
        send_ws_json(fd, "{\"type\":\"status\",\"monitor_supported\":true,"
            "\"monitor_running\":true,\"monitor_volume\":1.0,\"monitor_muted\":false}");

        // WebSocket message loop
        ws_loop(fd);
        close(fd);
        fprintf(stderr, "[ws] Client disconnected\n");
    }

    void ws_loop(int fd) {
        uint8_t buf[4096];
        while (running->load()) {
            pollfd pfd{fd, POLLIN, 0};
            int r = poll(&pfd, 1, 1000);
            if (r < 0) break;
            if (r == 0) continue; // timeout — send ping
            if (pfd.revents & (POLLERR | POLLHUP)) break;

            int n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;

            // Parse WebSocket frame
            if (n < 2) continue;
            uint8_t opcode = buf[0] & 0x0F;
            bool masked = buf[1] & 0x80;
            uint64_t payload_len = buf[1] & 0x7F;
            size_t offset = 2;

            if (payload_len == 126) {
                if (n < 4) continue;
                payload_len = (uint16_t)buf[2] << 8 | buf[3];
                offset = 4;
            } else if (payload_len == 127) {
                if (n < 10) continue;
                payload_len = 0;
                for (int i = 0; i < 8; i++)
                    payload_len = (payload_len << 8) | buf[offset + i];
                offset = 10;
            }

            uint8_t mask[4] = {};
            if (masked) {
                if ((size_t)n < offset + 4) continue;
                memcpy(mask, buf + offset, 4);
                offset += 4;
            }

            if ((size_t)n < offset + payload_len) continue;
            // Unmask
            for (uint64_t i = 0; i < payload_len; i++)
                buf[offset + i] ^= mask[i % 4];

            if (opcode == 0x08) break; // close
            if (opcode == 0x09) { // ping → pong
                send_ws_frame(fd, 0x0A, buf + offset, payload_len);
                continue;
            }
            if (opcode == 0x0A) continue; // pong — ignore

            if (opcode == 0x01) { // text
                std::string msg((char*)buf + offset, payload_len);
                handle_ws_message(fd, msg);
            }
        }
    }

    void handle_ws_message(int fd, const std::string& msg) {
        // Parse simple JSON — look for "method" field
        auto find_str = [&](const std::string& key) -> std::string {
            auto pos = msg.find("\"" + key + "\"");
            if (pos == std::string::npos) return "";
            pos = msg.find(':', pos + key.size() + 2);
            if (pos == std::string::npos) return "";
            pos++;
            while (pos < msg.size() && (msg[pos] == ' ' || msg[pos] == '"')) pos++;
            auto end = msg.find_first_of("\",}", pos);
            if (end == std::string::npos) end = msg.size();
            return msg.substr(pos, end - pos);
        };
        auto find_num = [&](const std::string& key) -> double {
            std::string s = find_str(key);
            return s.empty() ? 0 : atof(s.c_str());
        };

        std::string method = find_str("method");
        int id = (int)find_num("id");

        char resp[512];
        if (method == "monitor.status" || method == "status.get") {
            float vol = pstate ? pstate->volume.load() : 1.0f;
            bool mute = pstate ? pstate->muted.load() : false;
            snprintf(resp, sizeof(resp),
                "{\"id\":%d,\"result\":{\"monitor_supported\":true,"
                "\"monitor_running\":true,\"monitor_volume\":%.2f,"
                "\"monitor_muted\":%s,\"monitor_packets\":0}}",
                id, vol, mute ? "true" : "false");
            send_ws_json(fd, resp);
        }
        else if (method == "monitor.set_volume") {
            float v = (float)find_num("volume");
            if (v < 0) v = 0; if (v > 1) v = 1;
            if (pstate) pstate->volume.store(v);
            snprintf(resp, sizeof(resp), "{\"id\":%d,\"result\":{\"ok\":true}}", id);
            send_ws_json(fd, resp);
        }
        else if (method == "monitor.set_mute") {
            std::string ms = find_str("muted");
            bool m = (ms == "true" || ms == "1");
            if (pstate) pstate->muted.store(m);
            snprintf(resp, sizeof(resp), "{\"id\":%d,\"result\":{\"ok\":true}}", id);
            send_ws_json(fd, resp);
        }
        else if (method == "monitor.start" || method == "monitor.stop") {
            snprintf(resp, sizeof(resp), "{\"id\":%d,\"result\":{\"ok\":true}}", id);
            send_ws_json(fd, resp);
        }
        else if (method == "mode.get") {
            snprintf(resp, sizeof(resp),
                "{\"id\":%d,\"result\":{\"mode\":\"sync\"}}", id);
            send_ws_json(fd, resp);
        }
        else if (method == "devices.list") {
            snprintf(resp, sizeof(resp),
                "{\"id\":%d,\"result\":{\"devices\":[\"%s\"]}}",
                id, device_name.c_str());
            send_ws_json(fd, resp);
        }
        else {
            // Unknown method — return ok to avoid errors
            snprintf(resp, sizeof(resp), "{\"id\":%d,\"result\":{\"ok\":true}}", id);
            send_ws_json(fd, resp);
        }
    }

    void send_ws_json(int fd, const char* json) {
        send_ws_frame(fd, 0x01, (const uint8_t*)json, strlen(json));
    }

    void send_ws_frame(int fd, uint8_t opcode, const uint8_t* data, size_t len) {
        uint8_t hdr[10];
        size_t hdr_len;
        hdr[0] = 0x80 | opcode; // FIN + opcode
        if (len < 126) {
            hdr[1] = (uint8_t)len;
            hdr_len = 2;
        } else if (len < 65536) {
            hdr[1] = 126;
            hdr[2] = (uint8_t)(len >> 8);
            hdr[3] = (uint8_t)len;
            hdr_len = 4;
        } else {
            hdr[1] = 127;
            for (int i = 0; i < 8; i++)
                hdr[2 + i] = (uint8_t)(len >> (56 - i*8));
            hdr_len = 10;
        }
        send(fd, hdr, hdr_len, MSG_NOSIGNAL);
        if (len > 0) send(fd, data, len, MSG_NOSIGNAL);
    }
};

static WsControlServer g_ws_server;

int main(int argc, char** argv) {
    // Check for --dj mode
    bool dj_mode = false;
    std::string dj_dir = "/data/music";
    std::string dj_relay_host = "127.0.0.1";
    uint16_t    dj_relay_port = 5100;
    std::string dj_channel = "soluna";

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--dj") {
            dj_mode = true;
        } else if (a == "--help" && dj_mode) {
            fprintf(stdout,
                "soluna --dj — DJ mode: broadcast music via OSTP\n\n"
                "  --dj                   Enable DJ mode\n"
                "  --dir <path>           Music directory (default: /data/music)\n"
                "  --relay <host:port>    Relay server (default: 127.0.0.1:5100)\n"
                "  --channel <name>       Channel name (default: soluna)\n"
            );
            return 0;
        }
    }

    if (dj_mode) {
        for (int i = 1; i < argc; i++) {
            std::string a = argv[i];
            auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
            if (a == "--dir") dj_dir = next();
            else if (a == "--relay") {
                std::string hp = next();
                auto colon = hp.rfind(':');
                if (colon != std::string::npos) {
                    dj_relay_host = hp.substr(0, colon);
                    dj_relay_port = (uint16_t)atoi(hp.substr(colon + 1).c_str());
                } else {
                    dj_relay_host = hp;
                }
            }
            else if (a == "--channel") dj_channel = next();
        }

        signal(SIGINT,  handle_signal);
        signal(SIGTERM, handle_signal);
        return dj_main(dj_dir, dj_relay_host, dj_relay_port, dj_channel);
    }

    return rx_main(argc, argv);
}

// ── RX main ──────────────────────────────────────────────────────────────────

static int rx_main(int argc, char** argv) {
    std::string group    = "239.69.0.1";
    uint16_t    port     = 5004;
    uint32_t    channels = 2;
    uint32_t    rate     = kDefaultRate;
    bool        use_alsa = true;
    bool        use_pipewire = false;
    bool        output_explicit = false; // true if user passed --output or --pipewire
    std::string alsa_dev = "default";
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
    bool        dlna_enabled = false;
    std::string dlna_name = "Soluna";
    bool        airplay_enabled = false;
    bool        airplay_tx = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if      (a == "--group")    group    = next();
        else if (a == "--port")     port     = (uint16_t)atoi(next());
        else if (a == "--channels") channels = (uint32_t)atoi(next());
        else if (a == "--device")   alsa_dev = next();
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
        else if (a == "--dlna")          dlna_enabled = true;
        else if (a == "--dlna-name")     dlna_name = next();
        else if (a == "--airplay")       airplay_enabled = true;
        else if (a == "--airplay-tx")    airplay_tx = true;
        else if (a == "--pipewire") {
            use_pipewire = true;
            use_alsa = false;
            output_explicit = true;
        }
        else if (a == "--output") {
            std::string mode = next();
            output_explicit = true;
            if (mode == "alsa") {
                use_alsa = true;
                use_pipewire = false;
            } else if (mode == "pipewire") {
                use_pipewire = true;
                use_alsa = false;
            } else if (mode == "pipe") {
                use_alsa = false;
                use_pipewire = false;
            } else {
                fprintf(stderr, "[rx] Unknown --output mode '%s'\n", mode.c_str());
                return 1;
            }
        }
        else if (a == "--help") {
            fprintf(stdout,
                "soluna — Unified Soluna CLI (RX + DJ)\n\n"
                "Modes:\n"
                "  (default)              RX mode: receive and play audio\n"
                "  --dj                   DJ mode: broadcast music files\n\n"
                "RX Options:\n"
                "  --group <ip>           Multicast group (default: 239.69.0.1)\n"
                "  --port <n>             UDP port        (default: 5004)\n"
                "  --peer <host:port>     P2P unicast via solunad relay\n"
                "  --relay <host:port>    WAN relay mode via soluna-relay\n"
                "  --group-name <name>    Group name for WAN relay (default: default)\n"
                "  --group-password <pw>  Group password for WAN relay (optional)\n"
                "  --channels <n>         Channels        (default: 2)\n"
                "  --output alsa          Output to ALSA  (default)\n"
                "  --output pipewire      Output via PipeWire\n"
                "  --output pipe          Output raw S16LE to stdout\n"
                "  --pipewire             Shorthand for --output pipewire\n"
                "  --device <name>        ALSA device     (default: default)\n"
                "  --buffer <ms>          Buffer target   (default: 60, sync'd with iOS/Mac)\n"
                "  --sync                 Enable media_timestamp sync mode\n"
                "  --sync-delay <ms>      Target end-to-end delay (default: 200)\n"
                "  --record <path>        Record to WAV   (for analysis)\n"
                "  --duration <sec>       Record duration  (default: 30)\n"
                "  --record-dir <dir>     Record to dir as rx_<timestamp>.wav\n"
                "  --metrics              Output JSON quality metrics to stderr\n"
                "  --metrics-interval <s> Metrics interval (default: 5)\n"
                "  --dlna                 Enable DLNA/UPnP Media Renderer (requires SOLUNA_HAS_DLNA)\n"
                "  --dlna-name <name>     DLNA friendly name (default: Soluna)\n"
                "  --airplay              Enable AirPlay 2 receiver (requires SOLUNA_HAS_AIRPLAY)\n"
                "  --airplay-tx           Forward audio to local AirPlay speakers\n"
            );
            return 0;
        }
    }

    // ── PipeWire auto-detection (if no explicit --output given) ───────────
#ifdef SOLUNA_HAS_PIPEWIRE
    if (!output_explicit) {
        // Try PipeWire first: check if PIPEWIRE_RUNTIME_DIR or XDG_RUNTIME_DIR/pipewire-0 exists
        const char* pw_runtime = getenv("PIPEWIRE_RUNTIME_DIR");
        const char* xdg_runtime = getenv("XDG_RUNTIME_DIR");
        bool pw_available = false;
        if (pw_runtime) {
            pw_available = true;
        } else if (xdg_runtime) {
            std::string pw_socket = std::string(xdg_runtime) + "/pipewire-0";
            struct stat st_buf;
            pw_available = (stat(pw_socket.c_str(), &st_buf) == 0);
        }
        if (pw_available) {
            use_pipewire = true;
            use_alsa = false;
            fprintf(stderr, "[rx] Auto-detected PipeWire, using PipeWire output\n");
        }
    }
    if (use_pipewire) {
        fprintf(stderr, "[rx] PipeWire output selected\n");
    }
#else
    if (use_pipewire) {
        fprintf(stderr, "[rx] PipeWire requested but not compiled in (SOLUNA_ENABLE_PIPEWIRE=OFF)\n");
        return 1;
    }
#endif

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
        std::string join_msg = "JOIN:" + relay_group;
        if (!relay_password.empty()) join_msg += ":" + relay_password;
        join_msg += "\n";
        sendto(sock, join_msg.c_str(), join_msg.size(), 0,
               (sockaddr*)&relay_addr, sizeof(relay_addr));
        fprintf(stderr, "[rx] WAN relay: %s:%u group='%s'\n",
                relay_host.c_str(), relay_port, relay_group.c_str());
    } else if (peer_mode) {
        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        bind_addr.sin_port = 0;
        if (bind(sock, (sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
            perror("bind"); close(sock); return 1;
        }
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_port = htons(peer_port);
        if (inet_pton(AF_INET, peer_host.c_str(), &peer_addr.sin_addr) <= 0) {
            fprintf(stderr, "[rx] Invalid peer host: %s\n", peer_host.c_str());
            close(sock); return 1;
        }
        const char hello[] = "hello";
        sendto(sock, hello, sizeof(hello), 0,
               (sockaddr*)&peer_addr, sizeof(peer_addr));
        fprintf(stderr, "[rx] P2P mode: relay=%s:%u\n", peer_host.c_str(), peer_port);
    } else {
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

    // ── Ring buffer + playback thread ────────────────────────────────────
    SpscRingBuffer ring(kRingCapFrames, channels);
    uint32_t target_fill_frames = (buffer_ms * rate) / 1000; // 60ms = 2880 frames

    fprintf(stderr, "[rx] Ring buffer: %u frames (%.1fs), target fill: %u frames (%ums)\n",
            kRingCapFrames, (float)kRingCapFrames / rate,
            target_fill_frames, buffer_ms);
    if (sync_mode) {
        fprintf(stderr, "[rx] Sync mode ON: target end-to-end delay %ums\n", sync_delay_ms);
    }

#ifdef SOLUNA_HAS_DLNA
    std::unique_ptr<soluna::transport::DlnaRenderer> dlna_renderer;
    if (dlna_enabled) {
        dlna_renderer = std::make_unique<soluna::transport::DlnaRenderer>();
        soluna::transport::DlnaRenderer::Config dlna_cfg;
        dlna_cfg.friendly_name = dlna_name;
        dlna_cfg.sample_rate = rate;
        dlna_cfg.channels = channels;
        auto* ring_ptr = &ring;
        auto dlna_audio_cb = [ring_ptr, channels](const int32_t* samples, size_t frames,
                                                    uint32_t /*ch*/, uint32_t /*r*/) {
            ring_ptr->write(samples, frames * channels);
        };
        if (dlna_renderer->start(dlna_cfg, dlna_audio_cb)) {
            fprintf(stderr, "[rx] DLNA renderer started on port %u\n",
                    dlna_renderer->http_port());
        } else {
            fprintf(stderr, "[rx] Failed to start DLNA renderer\n");
            dlna_renderer.reset();
        }
    }
#else
    if (dlna_enabled) {
        fprintf(stderr, "[rx] DLNA support not compiled in (build with -DSOLUNA_ENABLE_DLNA=ON)\n");
    }
#endif

#ifdef SOLUNA_HAS_AIRPLAY
    std::unique_ptr<soluna::transport::AirPlayReceiver> airplay_rx;
    if (airplay_enabled) {
        airplay_rx = std::make_unique<soluna::transport::AirPlayReceiver>();
        airplay_rx->set_device_name("Soluna");
        auto* ring_ptr = &ring;
        airplay_rx->set_audio_callback(
            [ring_ptr, channels](const int16_t* pcm, uint32_t frames,
                                  uint8_t ap_channels, uint32_t /*sample_rate*/) {
                // Convert int16 → int32 and write to ring buffer
                // TODO: resample if sample rate differs from receiver rate
                std::vector<int32_t> buf(frames * channels);
                if (ap_channels == channels) {
                    for (uint32_t i = 0; i < frames * channels; i++) {
                        buf[i] = static_cast<int32_t>(pcm[i]) << 16;
                    }
                } else if (ap_channels == 1 && channels == 2) {
                    for (uint32_t i = 0; i < frames; i++) {
                        int32_t s = static_cast<int32_t>(pcm[i]) << 16;
                        buf[i * 2 + 0] = s;
                        buf[i * 2 + 1] = s;
                    }
                } else if (ap_channels == 2 && channels == 1) {
                    for (uint32_t i = 0; i < frames; i++) {
                        buf[i] = (static_cast<int32_t>(pcm[i * 2]) + pcm[i * 2 + 1]) << 15;
                    }
                } else {
                    for (uint32_t i = 0; i < frames * ap_channels && i < frames * channels; i++) {
                        buf[i] = static_cast<int32_t>(pcm[i]) << 16;
                    }
                }
                ring_ptr->write(buf.data(), frames * channels);
            });
        if (airplay_rx->start(7000)) {
            fprintf(stderr, "[rx] AirPlay receiver started on port 7000\n");
        } else {
            fprintf(stderr, "[rx] Failed to start AirPlay receiver\n");
            airplay_rx.reset();
        }
    }
#ifdef SOLUNA_HAS_AIRPLAY
    // AirPlay TX — forward received audio to local AirPlay speakers
    std::unique_ptr<soluna::transport::AirPlaySender> airplay_sender;
    if (airplay_tx) {
        airplay_sender = std::make_unique<soluna::transport::AirPlaySender>();
        if (!airplay_sender->start()) {
            fprintf(stderr, "[rx] Failed to start AirPlay TX sender\n");
            airplay_sender.reset();
        } else {
            fprintf(stderr, "[rx] AirPlay TX sender started — forwarding to local speakers\n");
        }
    }
#endif

#else
    if (airplay_enabled) {
        fprintf(stderr, "[rx] AirPlay support not compiled in (build with -DSOLUNA_ENABLE_AIRPLAY=ON)\n");
    }
    if (airplay_tx) {
        fprintf(stderr, "[rx] AirPlay TX support not compiled in (build with -DSOLUNA_ENABLE_AIRPLAY=ON)\n");
    }
#endif

    // --record-dir: auto-generate timestamped filename
    if (record_path.empty() && !record_dir.empty()) {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        struct tm tm_buf;
        localtime_r(&t, &tm_buf);
        char ts[128];
        snprintf(ts, sizeof(ts), "%s/rx_%04d%02d%02d_%02d%02d%02d.wav",
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
    pstate.use_alsa = use_alsa;
    pstate.use_pipewire = use_pipewire;
    pstate.wav = &wav;
    pstate.record_frames_max = record_frames_max;
#ifdef SOLUNA_HAS_AIRPLAY
    if (airplay_sender) {
        pstate.airplay_sender = airplay_sender.get();
    }
#endif

    std::thread playback(playback_thread_func, &pstate);

    // ── Start WebSocket control server (port 8400) ───────────────────────
    // Get hostname for device name
    char hostname_buf[256] = "soluna-rx";
    gethostname(hostname_buf, sizeof(hostname_buf));

    g_ws_server.running = (std::atomic<bool>*)&g_running;
    g_ws_server.pstate = &pstate;
    g_ws_server.device_name = hostname_buf;
    g_ws_server.channels = channels;
    g_ws_server.rate = rate;
    std::thread ws_thread;
    if (g_ws_server.start(8400)) {
        ws_thread = std::thread([&]{ g_ws_server.run(); });
        fprintf(stderr, "[rx] WebSocket control server on port 8400\n");
    } else {
        fprintf(stderr, "[rx] Warning: could not start WS control server on port 8400\n");
    }

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
    while (g_running) {
        // Heartbeat + sync ping (every 5s, same interval — matched with iOS)
        if (relay_mode) {
            double now = now_sec();
            if (now - last_hello_time >= 5.0) {
                const char hello[] = "HELLO\n";
                sendto(sock, hello, strlen(hello), 0,
                       (sockaddr*)&relay_addr, sizeof(relay_addr));
                // Send NTP clock sync ping alongside HELLO (PT=125)
                send_sync_ping(sock, relay_addr);
                // DELAY reporting: send net delay to relay every ~5s
                if (g_last_media_ts != 0) {
                    struct timespec rpt_ts;
                    clock_gettime(CLOCK_REALTIME, &rpt_ts);
                    uint32_t rpt_ms32 = (uint32_t)(
                        ((uint64_t)rpt_ts.tv_sec * 1000ULL +
                         (uint64_t)rpt_ts.tv_nsec / 1000000ULL) & 0xFFFFFFFF);
                    int32_t offset_ms = (int32_t)(g_clock_offset_ns / 1000000LL);
                    int32_t nd_ms = (int32_t)(rpt_ms32 - g_last_media_ts) + offset_ms;
                    if (nd_ms >= 0 && nd_ms < 2000) {
                        send_delay_report(sock, relay_addr, (uint32_t)nd_ms);
                    }
                }
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
        // ── Handle text messages and sync pong before RTP parsing ─────
        // Clock sync pong (PT=125, 0x7D marker, 25 bytes)
        if (n == 25 && pkt[0] == 0x7D) {
            handle_sync_pong(pkt, (size_t)n);
            continue;
        }
        // Text messages from relay: MAXDELAY:<ms>\n
        if (n >= 9 && memcmp(pkt, "MAXDELAY:", 9) == 0) {
            char val_buf[32] = {};
            size_t vlen = std::min((size_t)(n - 9), sizeof(val_buf) - 1);
            memcpy(val_buf, pkt + 9, vlen);
            // Strip trailing newline/CR
            for (size_t i = vlen; i > 0; i--) {
                if (val_buf[i-1] == '\n' || val_buf[i-1] == '\r') val_buf[i-1] = 0;
                else break;
            }
            uint32_t max_ms = (uint32_t)atoi(val_buf);
            if (max_ms > 2000) max_ms = 2000;
            if (max_ms != sync_delay_ms) {
                fprintf(stderr, "[sync] MAXDELAY received: %u ms (was %u ms)\n",
                        max_ms, sync_delay_ms);
                sync_delay_ms = max_ms;
            }
            continue;
        }
        // Skip other text messages (META:, FILE:, SYNC:, YOUR_ADDR:, etc.)
        if (n >= 4 && pkt[0] >= 'A' && pkt[0] <= 'Z') {
            // Not an RTP packet — text command from relay, skip
            continue;
        }

        if ((size_t)n < sizeof(RtpHeader)) continue;

        const RtpHeader* rtp = reinterpret_cast<const RtpHeader*>(pkt);

        // Detect OSTP vs plain RTP via RTP extension header
        bool is_ostp = false;
        size_t payload_off = sizeof(RtpHeader);
        bool has_ext = (rtp->cc_x_p_v & 0x10) != 0; // X bit
        if (has_ext && (size_t)n >= sizeof(RtpHeader) + sizeof(RtpExtensionHeader) + sizeof(OstpHeader)) {
            const RtpExtensionHeader* ext = reinterpret_cast<const RtpExtensionHeader*>(pkt + sizeof(RtpHeader));
            if (ntohs(ext->profile_specific) == kOstpProfile && ntohs(ext->length) == 2) {
                is_ostp = true;
                payload_off = sizeof(RtpHeader) + sizeof(RtpExtensionHeader) + sizeof(OstpHeader);
            }
        }

        if (payload_off >= (size_t)n) continue;

        // ── Sync mode: adjust target fill from OSTP media_timestamp ──
        // Matched with iOS AudioReceiverBridge.mm inject_raw_packet() sync algorithm.
        // Uses NTP-corrected clock offset for accurate cross-device sync.
        if (is_ostp) {
            const OstpHeader* ostp = reinterpret_cast<const OstpHeader*>(
                pkt + sizeof(RtpHeader) + sizeof(RtpExtensionHeader));
            uint32_t media_ts_raw = ntohl(ostp->media_timestamp);
            if (media_ts_raw != 0) {
                g_last_media_ts = media_ts_raw;  // store for DELAY reporting
            }
        }

        // Every 50 packets: run sync algorithm (matched with iOS ~0.25s interval)
        static uint32_t sync_pkt_counter = 0;
        sync_pkt_counter++;
        if (sync_mode && relay_mode && (sync_pkt_counter % 50 == 0) && g_last_media_ts != 0) {
            struct timespec now_ts;
            clock_gettime(CLOCK_REALTIME, &now_ts);
            uint32_t now_ms32 = (uint32_t)(
                ((uint64_t)now_ts.tv_sec * 1000ULL +
                 (uint64_t)now_ts.tv_nsec / 1000000ULL) & 0xFFFFFFFF);

            // Apply NTP clock offset correction (relay_time - local_time)
            int32_t offset_ms = (int32_t)(g_clock_offset_ns / 1000000LL);
            int32_t net_delay_ms = (int32_t)(now_ms32 - g_last_media_ts) + offset_ms;

            if (net_delay_ms >= 0 && net_delay_ms < 2000) {
                int32_t buffer_ms = (int32_t)sync_delay_ms - net_delay_ms;
                if (buffer_ms < 5) buffer_ms = 5;  // 5ms floor — matched with iOS
                uint32_t target = (uint32_t)(buffer_ms * 48);  // ms -> frames @48kHz

                // Adaptive EMA — matched with iOS: fast initially, slow when stable
                uint32_t prev = pstate.target_fill_frames;
                int32_t diff = (int32_t)target - (int32_t)prev;
                double alpha;
                if (g_sync_samples_count < 50) {
                    alpha = 0.20;  // First ~250ms: fast lock-on
                    g_sync_samples_count++;
                } else if (std::abs(diff) > 2400) {
                    alpha = 0.15;  // >50ms jump: re-converge quickly
                } else if (std::abs(diff) > 480) {
                    alpha = 0.08;  // 10-50ms drift: moderate correction
                } else {
                    alpha = 0.02;  // Stable: gentle smoothing
                }
                uint32_t smoothed = (uint32_t)(prev * (1.0 - alpha) + target * alpha);
                // WAN relay mode: enforce 500ms floor for stability — matched with iOS
                smoothed = std::max(smoothed, 24000u);
                pstate.target_fill_frames = smoothed;
            }
        }

        const uint8_t* payload   = pkt + payload_off;
        size_t         payload_n = (size_t)n - payload_off;

        // Sequence tracking
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

        // Decode samples into int32 (ring buffer native format — matched with iOS/Mac)
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

    // Wait for threads to finish
    playback.join();
    if (ws_thread.joinable()) ws_thread.join();

    if (wav.fp) {
        wav.close();
        fprintf(stderr, "[rx] Recording saved\n");
    }

    fprintf(stderr, "\n[rx] Stopped. Received %llu packets, dropped %llu, underruns %u\n",
        (unsigned long long)total_rx, (unsigned long long)total_drop,
        pstate.underruns.load());

    close(sock);

#ifdef SOLUNA_HAS_DLNA
    if (dlna_renderer) {
        dlna_renderer->stop();
        dlna_renderer.reset();
    }
#endif

#ifdef SOLUNA_HAS_AIRPLAY
    if (airplay_rx) {
        airplay_rx->stop();
        airplay_rx.reset();
    }
    if (airplay_sender) {
        airplay_sender->stop();
        airplay_sender.reset();
    }
#endif

    return 0;
}
