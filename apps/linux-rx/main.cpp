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
    uint32_t media_timestamp; // wall-clock capture time (ns since epoch, network byte order)
} __attribute__((packed));

static constexpr uint32_t kOstpMagic = 0x4F535450; // 'OSTP'
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

        // Output
        if (!st->use_alsa) {
            fwrite(out_buf.data(), sizeof(int16_t) * ch, frame_count, stdout);
            fflush(stdout);
        }
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
static constexpr uint16_t kOstpProfile      = 0x4F53;  // "OS"
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
                "  --output pipe          Output raw S16LE to stdout\n"
                "  --device <name>        ALSA device     (default: default)\n"
                "  --buffer <ms>          Buffer target   (default: 60, sync'd with iOS/Mac)\n"
                "  --sync                 Enable media_timestamp sync mode\n"
                "  --sync-delay <ms>      Target end-to-end delay (default: 200)\n"
                "  --record <path>        Record to WAV   (for analysis)\n"
                "  --duration <sec>       Record duration  (default: 30)\n"
                "  --record-dir <dir>     Record to dir as rx_<timestamp>.wav\n"
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

    while (g_running) {
        // Heartbeat
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

    // Wait for playback thread to finish
    playback.join();

    if (wav.fp) {
        wav.close();
        fprintf(stderr, "[rx] Recording saved\n");
    }

    fprintf(stderr, "\n[rx] Stopped. Received %llu packets, dropped %llu, underruns %u\n",
        (unsigned long long)total_rx, (unsigned long long)total_drop,
        pstate.underruns.load());

    close(sock);
    return 0;
}
