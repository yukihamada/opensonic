//
//  AudioReceiverBridge.mm
//  SolunaReceiver
//
//  Objective-C++ bridge for C++ RTP receiver and CoreAudio output
//

#import "AudioReceiverBridge.h"

#include "web_embedded.h"
#include <soluna/soluna.h>
#include <soluna/pal/audio.h>
#include <soluna/pal/net.h>
#include <soluna/transport/rtp.h>
#include <soluna/transport/ostp.h>
#include <soluna/pipeline/ring_buffer.h>
#include <soluna/control/websocket_server.h>

#include <AudioToolbox/AudioToolbox.h>
#include <Accelerate/Accelerate.h>

#include <atomic>
#include <thread>
#include <mutex>
#include <memory>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

using namespace soluna;

// ============================================================================
// Internal C++ Implementation
// ============================================================================

namespace {

/// Receive mode for RTP reception
enum class ReceiveMode {
    OSTP,
    AES67,
    Auto
};

/// RTP payload types (AES67 standard)
constexpr uint8_t kPayloadTypeL24_AES67 = 10;  // AES67 24-bit
constexpr uint8_t kPayloadTypeL16_AES67 = 11;  // AES67 16-bit
constexpr uint8_t kPayloadTypeL24 = 98;  // OSTP default
constexpr uint8_t kPayloadTypeL16 = 11;

/// Check if RTP packet is AES67 standard (no OSTP extension)
inline bool aes67_is_standard_packet(const transport::RtpHeader& hdr) {
    // AES67 uses PT 10 (L24) or PT 11 (L16) and no extension header
    return (hdr.pt == kPayloadTypeL24_AES67 || hdr.pt == kPayloadTypeL16_AES67) && hdr.extension == 0;
}

/// Simple RTP receiver (embedded to avoid header dependency issues)
class SimpleRtpReceiver {
public:
    struct Config {
        uint16_t listen_port = kPortRTPBase;
        std::string multicast_group = kMulticastAudio;
        uint32_t channels = 1;
        SampleFormat format = SampleFormat::S24_LE;
        ReceiveMode mode = ReceiveMode::Auto;
    };

    struct Stats {
        uint64_t packets_received = 0;
        uint64_t packets_dropped = 0;
        uint64_t packets_concealed = 0;
        uint64_t sequence_errors = 0;
        uint64_t aes67_packets = 0;
        uint64_t ostp_packets = 0;
        int32_t last_sequence = -1;
    };

    explicit SimpleRtpReceiver(const Config& config)
        : config_(config)
        , frame_size_(sample_size(config.format) * config.channels)
        , recv_buf_(transport::kMaxPacketSize)
        , audio_buf_(transport::kMaxPayloadSize / sizeof(int32_t))
    {}

    Stats stats_snapshot() const { return stats_; }

    bool init() {
        socket_ = pal::UdpSocket::create();
        if (!socket_) return false;
        if (!socket_->bind(config_.listen_port)) return false;
        if (!socket_->join_multicast(config_.multicast_group)) return false;
        socket_->set_recv_timeout_ms(1);
        return true;
    }

    // Relay callback: invoked with raw bytes for every received packet
    std::function<void(const uint8_t*, size_t)> relay_callback;

    // Fan-out callback: invoked after audio is written to the primary ring buffer
    // Parameters: (audio_data, frame_count) — int32_t interleaved samples
    std::function<void(const void*, size_t)> on_audio_written;

    bool receive_packet(pipeline::RingBuffer& ring) {
        pal::SocketAddress src;
        int received = socket_->recv_from_nonblock(recv_buf_.data(), recv_buf_.size(), src);
        if (received <= 0) return false;

        // Forward raw bytes to nearby peers when in relay mode
        if (relay_callback) {
            relay_callback(recv_buf_.data(), static_cast<size_t>(received));
        }

        // Check if this is an AES67 packet or OSTP packet
        bool is_aes67 = false;
        if (config_.mode == ReceiveMode::Auto &&
            static_cast<size_t>(received) >= sizeof(transport::RtpHeader)) {
            const auto* rtp = reinterpret_cast<const transport::RtpHeader*>(recv_buf_.data());
            is_aes67 = aes67_is_standard_packet(*rtp);
        } else if (config_.mode == ReceiveMode::AES67) {
            is_aes67 = true;
        }

        if (is_aes67) {
            return receive_aes67_packet(recv_buf_.data(), static_cast<size_t>(received), ring);
        } else {
            return receive_ostp_packet(recv_buf_.data(), static_cast<size_t>(received), ring);
        }
    }

    const Stats& stats() const { return stats_; }

    /// Inject a raw packet from a relay peer (bypasses UDP socket)
    bool inject_raw_packet(const uint8_t* data, size_t len, pipeline::RingBuffer& ring) {
        bool is_aes67 = false;
        if (config_.mode == ReceiveMode::Auto && len >= sizeof(transport::RtpHeader)) {
            const auto* rtp = reinterpret_cast<const transport::RtpHeader*>(data);
            is_aes67 = aes67_is_standard_packet(*rtp);
        } else if (config_.mode == ReceiveMode::AES67) {
            is_aes67 = true;
        }
        if (is_aes67) return receive_aes67_packet(data, len, ring);
        else          return receive_ostp_packet(data, len, ring);
    }

private:
    bool receive_ostp_packet(const uint8_t* data, size_t len, pipeline::RingBuffer& ring) {
        transport::RtpHeader rtp;
        transport::OstpHeader ostp;
        const uint8_t* payload = nullptr;
        size_t payload_size = 0;

        if (transport::ostp_parse_packet(data, len, rtp, ostp, payload, payload_size) != 0) {
            return false;
        }

        // Sequence check — returns gap count (positive = missing packets)
        uint32_t full_seq = (static_cast<uint32_t>(ostp.sequence_ext) << 16) | rtp.sequence;
        int32_t gap = check_sequence(full_seq);

        stats_.packets_received++;
        stats_.ostp_packets++;

        // Discard duplicate packets (gap <= 0 means same or older sequence)
        if (gap < 0) return true;  // duplicate — already received

        // OSTP payload is int32_t (4 bytes/sample, native byte order) — not S24_LE 3-byte
        size_t frames = payload_size / (sizeof(int32_t) * config_.channels);

        // PLC: conceal gaps of ≤2 packets by repeating the last known frame
        if (gap > 0 && gap <= 2 && frames > 0 && !last_frame_.empty()) {
            for (int32_t i = 0; i < gap; i++) {
                ring.write(last_frame_.data(), frames);
                // Fan-out PLC frames to extra sinks
                if (on_audio_written) {
                    on_audio_written(last_frame_.data(), frames);
                }
            }
            stats_.packets_concealed += static_cast<uint64_t>(gap);
        }

        ring.write(payload, frames);

        // Fan-out to extra sinks
        if (on_audio_written && frames > 0) {
            on_audio_written(payload, frames);
        }

        // Save last frame for PLC
        if (frames > 0) {
            last_frame_.assign(
                reinterpret_cast<const int32_t*>(payload),
                reinterpret_cast<const int32_t*>(payload) + frames * config_.channels);
        }

        return true;
    }

    bool receive_aes67_packet(const uint8_t* data, size_t len, pipeline::RingBuffer& ring) {
        if (len < sizeof(transport::RtpHeader)) {
            return false;
        }

        transport::RtpHeader rtp;
        std::memcpy(&rtp, data, sizeof(transport::RtpHeader));

        uint16_t sequence = ntohs(rtp.sequence);

        // Sequence check (16-bit only for AES67)
        if (stats_.last_sequence >= 0) {
            int32_t last16 = stats_.last_sequence & 0xFFFF;
            int32_t diff = static_cast<int32_t>(sequence) - last16;
            if (diff < -32768) diff += 65536;
            if (diff > 32768) diff -= 65536;
            if (diff != 1) {
                stats_.sequence_errors++;
                if (diff > 0) {
                    stats_.packets_dropped += static_cast<uint64_t>(diff - 1);
                }
            }
        }
        stats_.last_sequence = sequence;
        stats_.packets_received++;
        stats_.aes67_packets++;

        // Extract payload
        const uint8_t* payload = data + sizeof(transport::RtpHeader);
        size_t payload_size = len - sizeof(transport::RtpHeader);

        // Convert payload based on payload type
        size_t samples = 0;
        if (rtp.pt == kPayloadTypeL24) {
            // L24: 24-bit packed big-endian -> S24_LE
            samples = payload_size / 3;
            for (size_t i = 0; i < samples && i < audio_buf_.size(); i++) {
                int32_t sample = (static_cast<int32_t>(payload[i * 3]) << 16)
                               | (static_cast<int32_t>(payload[i * 3 + 1]) << 8)
                               | static_cast<int32_t>(payload[i * 3 + 2]);
                if (sample & 0x800000) {
                    sample |= 0xFF000000;
                }
                audio_buf_[i] = sample;
            }
        } else if (rtp.pt == kPayloadTypeL16) {
            // L16: 16-bit big-endian -> S24_LE
            samples = payload_size / 2;
            const int16_t* src = reinterpret_cast<const int16_t*>(payload);
            for (size_t i = 0; i < samples && i < audio_buf_.size(); i++) {
                int16_t be_sample = src[i];
                int16_t sample_le = static_cast<int16_t>((be_sample >> 8) | (be_sample << 8));
                audio_buf_[i] = static_cast<int32_t>(sample_le) << 8;
            }
        } else {
            return false;
        }

        size_t frames = samples / config_.channels;
        ring.write(audio_buf_.data(), frames);

        // Fan-out to extra sinks
        if (on_audio_written && frames > 0) {
            on_audio_written(audio_buf_.data(), frames);
        }

        return true;
    }

    // Returns the gap (missing packet count > 0) or 0 if in sequence
    int32_t check_sequence(uint32_t full_seq) {
        int32_t gap = 0;
        if (stats_.last_sequence >= 0) {
            int32_t expected = stats_.last_sequence + 1;
            if (static_cast<int32_t>(full_seq) != expected) {
                stats_.sequence_errors++;
                gap = static_cast<int32_t>(full_seq) - expected;
                if (gap > 0) {
                    stats_.packets_dropped += static_cast<uint64_t>(gap);
                }
            }
        }
        stats_.last_sequence = static_cast<int32_t>(full_seq);
        return gap;
    }

    Config config_;
    size_t frame_size_;
    std::unique_ptr<pal::UdpSocket> socket_;
    std::vector<uint8_t> recv_buf_;
    std::vector<int32_t> audio_buf_;
    std::vector<int32_t> last_frame_;  // PLC: last received frame for concealment
    Stats stats_;
};

// ============================================================================
// Biquad filter for parametric EQ
// ============================================================================

struct BiquadCoeffs {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

struct BiquadState {
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
};

/// Compute peaking EQ filter coefficients
inline BiquadCoeffs peaking_eq(float freq_hz, float gain_db, float q, float sample_rate = 48000.0f) {
    if (std::fabs(gain_db) < 0.01f) return {}; // bypass
    float A = std::pow(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * M_PI * freq_hz / sample_rate;
    float alpha = std::sin(w0) / (2.0f * q);
    float a0 = 1.0f + alpha / A;
    BiquadCoeffs c;
    c.b0 = (1.0f + alpha * A) / a0;
    c.b1 = (-2.0f * std::cos(w0)) / a0;
    c.b2 = (1.0f - alpha * A) / a0;
    c.a1 = c.b1;  // same as -2*cos(w0)/a0
    c.a2 = (1.0f - alpha / A) / a0;
    return c;
}

/// Apply biquad filter to a single sample
inline float biquad_process(const BiquadCoeffs& c, BiquadState& s, float x) {
    float y = c.b0 * x + c.b1 * s.x1 + c.b2 * s.x2 - c.a1 * s.y1 - c.a2 * s.y2;
    s.x2 = s.x1; s.x1 = x;
    s.y2 = s.y1; s.y1 = y;
    return y;
}

/// Compute Butterworth low-pass filter coefficients
inline BiquadCoeffs lowpass_biquad(float freq_hz, float q = 0.7071f, float sample_rate = 48000.0f) {
    float w0 = 2.0f * M_PI * freq_hz / sample_rate;
    float alpha = std::sin(w0) / (2.0f * q);
    float cos_w0 = std::cos(w0);
    float a0 = 1.0f + alpha;
    BiquadCoeffs c;
    c.b0 = ((1.0f - cos_w0) / 2.0f) / a0;
    c.b1 = (1.0f - cos_w0) / a0;
    c.b2 = c.b0;
    c.a1 = (-2.0f * cos_w0) / a0;
    c.a2 = (1.0f - alpha) / a0;
    return c;
}

/// Compute Butterworth high-pass filter coefficients
inline BiquadCoeffs highpass_biquad(float freq_hz, float q = 0.7071f, float sample_rate = 48000.0f) {
    float w0 = 2.0f * M_PI * freq_hz / sample_rate;
    float alpha = std::sin(w0) / (2.0f * q);
    float cos_w0 = std::cos(w0);
    float a0 = 1.0f + alpha;
    BiquadCoeffs c;
    c.b0 = ((1.0f + cos_w0) / 2.0f) / a0;
    c.b1 = -(1.0f + cos_w0) / a0;
    c.b2 = c.b0;
    c.a1 = (-2.0f * cos_w0) / a0;
    c.a2 = (1.0f - alpha) / a0;
    return c;
}

// ============================================================================
// Crossover filter — LPF/HPF with configurable frequency (2nd-order Butterworth)
// ============================================================================

struct CrossoverFilter {
    enum class Mode { Off = 0, LowPass = 1, HighPass = 2 };

    std::atomic<int>   mode{0};         // 0=off, 1=lpf, 2=hpf
    std::atomic<float> freq{80.0f};     // crossover frequency (20-20000 Hz)

    BiquadCoeffs coeffs = {};
    BiquadState  state[2] = {};         // per-channel state (max 2)

    void update_coeffs() {
        float f = freq.load(std::memory_order_relaxed);
        int   m = mode.load(std::memory_order_relaxed);
        if (m == 1)      coeffs = lowpass_biquad(f);
        else if (m == 2) coeffs = highpass_biquad(f);
    }

    float process(float sample, int channel) {
        int m = mode.load(std::memory_order_relaxed);
        if (m == 0) return sample;
        return biquad_process(coeffs, state[channel & 1], sample);
    }
};

/// 3-band parametric EQ (Low / Mid / High)
struct EQ3Band {
    static constexpr int kBands = 3;
    static constexpr float kDefaultFreqs[3] = {200.0f, 1000.0f, 5000.0f};
    static constexpr float kDefaultQ = 1.0f;

    std::atomic<float> gains[3] = {{0}, {0}, {0}};  // dB (-12..+12)
    BiquadCoeffs coeffs[3] = {};
    // Per-channel state (max 2 channels)
    BiquadState state[3][2] = {};

    void update_coeffs() {
        for (int i = 0; i < kBands; i++) {
            coeffs[i] = peaking_eq(kDefaultFreqs[i], gains[i].load(std::memory_order_relaxed), kDefaultQ);
        }
    }

    float process(float sample, int channel) {
        float s = sample;
        for (int i = 0; i < kBands; i++) {
            if (std::fabs(gains[i].load(std::memory_order_relaxed)) > 0.01f) {
                s = biquad_process(coeffs[i], state[i][channel & 1], s);
            }
        }
        return s;
    }
};

constexpr float EQ3Band::kDefaultFreqs[3];

// ============================================================================
// Compressor / Limiter
// ============================================================================

struct Compressor {
    std::atomic<float> threshold{-20.0f};  // dB (-60..0)
    std::atomic<float> ratio{4.0f};        // 1:1 .. 20:1
    std::atomic<float> attack_ms{10.0f};   // ms (0.1-100)
    std::atomic<float> release_ms{100.0f}; // ms (10-1000)
    std::atomic<bool>  enabled{false};

    // Envelope follower state (audio-callback-only)
    float envelope_db = -96.0f;

    /// Process a single sample. Returns compressed sample.
    float process(float sample, float sample_rate = 48000.0f) {
        if (!enabled.load(std::memory_order_relaxed)) return sample;

        float thresh = threshold.load(std::memory_order_relaxed);
        float rat    = ratio.load(std::memory_order_relaxed);
        float att_ms = attack_ms.load(std::memory_order_relaxed);
        float rel_ms = release_ms.load(std::memory_order_relaxed);

        // Input level in dB
        float abs_s = std::fabs(sample);
        float input_db = (abs_s > 1e-10f) ? 20.0f * std::log10(abs_s) : -96.0f;

        // Envelope follower (peak detector with attack/release)
        float att_coeff = std::exp(-1.0f / (att_ms * sample_rate / 1000.0f));
        float rel_coeff = std::exp(-1.0f / (rel_ms * sample_rate / 1000.0f));

        if (input_db > envelope_db)
            envelope_db = att_coeff * envelope_db + (1.0f - att_coeff) * input_db;
        else
            envelope_db = rel_coeff * envelope_db + (1.0f - rel_coeff) * input_db;

        // Gain computation
        float gain_db = 0.0f;
        if (envelope_db > thresh) {
            float over = envelope_db - thresh;
            gain_db = over * (1.0f / rat - 1.0f);
        }

        float gain = std::pow(10.0f, gain_db / 20.0f);
        return sample * gain;
    }
};

// ============================================================================
// Stereo Spatializer — virtual surround via mid/side + cross-feed delay
// ============================================================================

struct StereoSpatializer {
    std::atomic<bool>  enabled{false};
    std::atomic<float> width{1.0f};      // 0.0=mono, 1.0=stereo, 2.0=wide
    std::atomic<float> crossfeed{0.0f};  // 0.0-0.5, amount of opposite channel mixed in

    // Cross-feed delay line (~0.3ms at 48kHz = 14 samples for HRTF ITD simulation)
    static constexpr int kDelayLen = 16;
    float delay_l[kDelayLen] = {};
    float delay_r[kDelayLen] = {};
    int delay_pos = 0;

    /// Process a stereo pair in-place. Call once per frame with L and R samples.
    void process(float& left, float& right) {
        if (!enabled.load(std::memory_order_relaxed)) return;

        float w = width.load(std::memory_order_relaxed);
        float cf = crossfeed.load(std::memory_order_relaxed);

        // Mid-side processing for width control
        float mid  = (left + right) * 0.5f;
        float side = (left - right) * 0.5f;
        side *= w;  // scale side signal

        // Reconstruct L/R from mid/side
        float l = mid + side;
        float r = mid - side;

        // Cross-feed: mix delayed opposite channel for HRTF-like effect
        if (cf > 0.001f) {
            int read_pos = (delay_pos + kDelayLen - 14) % kDelayLen;  // ~0.29ms delay
            float delayed_l = delay_l[read_pos];
            float delayed_r = delay_r[read_pos];

            // Store current samples in delay line
            delay_l[delay_pos] = l;
            delay_r[delay_pos] = r;
            delay_pos = (delay_pos + 1) % kDelayLen;

            // Mix: add attenuated, delayed opposite channel
            l += delayed_r * cf * -0.7f;  // inverted for out-of-head effect
            r += delayed_l * cf * -0.7f;
        }

        left  = l;
        right = r;
    }
};

// ============================================================================
// Spectrum Analyzer — 32-band FFT (1024-point, Accelerate vDSP)
// ============================================================================

class SpectrumAnalyzer {
public:
    static constexpr int kFFTSize = 1024;
    static constexpr int kFFTLog2 = 10;  // log2(1024)
    static constexpr int kBands = 32;
    static constexpr float kSampleRate = 48000.0f;

    SpectrumAnalyzer() {
        fft_setup_ = vDSP_create_fftsetup(kFFTLog2, kFFTRadix2);
        window_.resize(kFFTSize);
        vDSP_hann_window(window_.data(), kFFTSize, vDSP_HANN_NORM);
        input_buf_.resize(kFFTSize, 0);
        windowed_.resize(kFFTSize, 0);
        split_.realp = reinterpret_cast<float*>(malloc(kFFTSize / 2 * sizeof(float)));
        split_.imagp = reinterpret_cast<float*>(malloc(kFFTSize / 2 * sizeof(float)));
        magnitudes_.resize(kFFTSize / 2, 0);
        write_pos_ = 0;
        for (auto& b : bands_) b.store(0, std::memory_order_relaxed);
    }

    ~SpectrumAnalyzer() {
        if (fft_setup_) vDSP_destroy_fftsetup(fft_setup_);
        free(split_.realp);
        free(split_.imagp);
    }

    /// Feed mono samples from audio callback. Processes FFT when buffer is full.
    void feed(const float* samples, uint32_t count, uint32_t channels) {
        for (uint32_t i = 0; i < count; i++) {
            // Mix to mono
            float s = samples[i * channels];
            if (channels >= 2) s = (s + samples[i * channels + 1]) * 0.5f;
            input_buf_[write_pos_++] = s;
            if (write_pos_ >= kFFTSize) {
                process_fft();
                write_pos_ = 0;
            }
        }
    }

    /// Get band magnitudes (0.0-1.0, 32 bands)
    void get_bands(float* out) const {
        for (int i = 0; i < kBands; i++) {
            out[i] = bands_[i].load(std::memory_order_relaxed);
        }
    }

private:
    void process_fft() {
        // Apply Hann window
        vDSP_vmul(input_buf_.data(), 1, window_.data(), 1, windowed_.data(), 1, kFFTSize);

        // Pack for real FFT
        vDSP_ctoz(reinterpret_cast<const DSPComplex*>(windowed_.data()), 2,
                  &split_, 1, kFFTSize / 2);

        // Forward FFT
        vDSP_fft_zrip(fft_setup_, &split_, 1, kFFTLog2, kFFTDirection_Forward);

        // Magnitudes (squared)
        vDSP_zvmags(&split_, 1, magnitudes_.data(), 1, kFFTSize / 2);

        // Scale
        float scale = 1.0f / static_cast<float>(kFFTSize);
        vDSP_vsmul(magnitudes_.data(), 1, &scale, magnitudes_.data(), 1, kFFTSize / 2);

        // Map FFT bins to 32 bands (logarithmic spacing)
        // Band i covers frequency range: 20*2^(i*log2(20000/20)/32) to 20*2^((i+1)*log2(20000/20)/32)
        const float fft_bin_width = kSampleRate / kFFTSize;  // ~46.875 Hz per bin
        const float log_ratio = std::log2(20000.0f / 20.0f) / kBands;

        for (int b = 0; b < kBands; b++) {
            float f_lo = 20.0f * std::pow(2.0f, b * log_ratio);
            float f_hi = 20.0f * std::pow(2.0f, (b + 1) * log_ratio);
            int bin_lo = std::max(1, static_cast<int>(f_lo / fft_bin_width));
            int bin_hi = std::min(kFFTSize / 2 - 1, static_cast<int>(f_hi / fft_bin_width));
            if (bin_hi < bin_lo) bin_hi = bin_lo;

            float sum = 0;
            for (int k = bin_lo; k <= bin_hi; k++) {
                sum += magnitudes_[k];
            }
            float avg = sum / std::max(1, bin_hi - bin_lo + 1);
            // Convert to dB-ish scale (0.0-1.0)
            float db = 10.0f * std::log10(avg + 1e-10f);
            float normalized = std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);

            // EMA smoothing (fast attack, slow decay)
            float prev = bands_[b].load(std::memory_order_relaxed);
            float smoothed = (normalized > prev)
                ? prev * 0.5f + normalized * 0.5f    // fast attack
                : prev * 0.85f + normalized * 0.15f; // slow decay
            bands_[b].store(smoothed, std::memory_order_relaxed);
        }
    }

    FFTSetup fft_setup_;
    std::vector<float> window_;
    std::vector<float> input_buf_;
    std::vector<float> windowed_;
    DSPSplitComplex split_;
    std::vector<float> magnitudes_;
    uint32_t write_pos_;
    std::atomic<float> bands_[kBands];
};

// ============================================================================
// OutputSink — one per extra output device (BT, AirPlay, USB, etc.)
// ============================================================================

class OutputSink {
public:
    OutputSink(uint32_t device_id, uint32_t channels)
        : device_id_(device_id)
        , channels_(channels)
        , volume_(1.0f)
        , muted_(false)
        , delay_frames_(0)
        , ring_(192000, channels * sizeof(int32_t))  // 4 sec capacity (AirPlay safe)
        , read_buf_(4096 * channels)
        , drain_buf_(4096 * channels)
        , held_sample_(channels, 0)
        , ramp_(0.0f)
    {}

    ~OutputSink() { stop(); }

    bool start() {
        AudioComponentDescription desc = {};
        desc.componentType = kAudioUnitType_Output;
        desc.componentSubType = kAudioUnitSubType_HALOutput;
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;

        AudioComponent component = AudioComponentFindNext(nullptr, &desc);
        if (!component) return false;

        OSStatus status = AudioComponentInstanceNew(component, &audio_unit_);
        if (status != noErr) return false;

        // Set device
        status = AudioUnitSetProperty(audio_unit_,
            kAudioOutputUnitProperty_CurrentDevice,
            kAudioUnitScope_Global, 0,
            &device_id_, sizeof(device_id_));
        if (status != noErr) {
            AudioComponentInstanceDispose(audio_unit_);
            audio_unit_ = nullptr;
            return false;
        }

        // Set format: 32-bit float interleaved
        AudioStreamBasicDescription fmt = {};
        fmt.mSampleRate = 48000;
        fmt.mFormatID = kAudioFormatLinearPCM;
        fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        fmt.mBytesPerPacket = sizeof(float) * channels_;
        fmt.mFramesPerPacket = 1;
        fmt.mBytesPerFrame = sizeof(float) * channels_;
        fmt.mChannelsPerFrame = channels_;
        fmt.mBitsPerChannel = 32;

        status = AudioUnitSetProperty(audio_unit_,
            kAudioUnitProperty_StreamFormat,
            kAudioUnitScope_Input, 0,
            &fmt, sizeof(fmt));
        if (status != noErr) {
            AudioComponentInstanceDispose(audio_unit_);
            audio_unit_ = nullptr;
            return false;
        }

        // Set buffer size
        UInt32 buf_frames = 256;
        AudioUnitSetProperty(audio_unit_,
            kAudioUnitProperty_MaximumFramesPerSlice,
            kAudioUnitScope_Global, 0,
            &buf_frames, sizeof(buf_frames));

        // Set HW buffer size on device
        AudioObjectPropertyAddress buf_addr = {
            kAudioDevicePropertyBufferFrameSize,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioObjectSetPropertyData(device_id_, &buf_addr, 0, nullptr, sizeof(buf_frames), &buf_frames);

        // Render callback
        AURenderCallbackStruct cbs = {};
        cbs.inputProc = render_cb;
        cbs.inputProcRefCon = this;
        status = AudioUnitSetProperty(audio_unit_,
            kAudioUnitProperty_SetRenderCallback,
            kAudioUnitScope_Input, 0,
            &cbs, sizeof(cbs));
        if (status != noErr) {
            AudioComponentInstanceDispose(audio_unit_);
            audio_unit_ = nullptr;
            return false;
        }

        status = AudioUnitInitialize(audio_unit_);
        if (status != noErr) {
            AudioComponentInstanceDispose(audio_unit_);
            audio_unit_ = nullptr;
            return false;
        }

        running_.store(true);
        status = AudioOutputUnitStart(audio_unit_);
        if (status != noErr) {
            running_.store(false);
            AudioComponentInstanceDispose(audio_unit_);
            audio_unit_ = nullptr;
            return false;
        }
        return true;
    }

    void stop() {
        running_.store(false);
        if (audio_unit_) {
            AudioOutputUnitStop(audio_unit_);
            AudioComponentInstanceDispose(audio_unit_);
            audio_unit_ = nullptr;
        }
    }

    /// Write audio data (called from receive thread via fan-out)
    void write(const void* data, size_t frames) {
        ring_.write(data, frames);
        // Record write timestamp for latency measurement
        last_write_us_.store(now_us(), std::memory_order_relaxed);
    }

    void set_volume(float v) { volume_.store(std::max(0.0f, std::min(1.0f, v))); }
    float volume() const { return volume_.load(); }
    void set_muted(bool m) { muted_.store(m); }
    bool is_muted() const { return muted_.load(); }
    void set_delay_frames(uint32_t d) { delay_frames_.store(d); }
    void set_balance(float b) { balance_.store(std::clamp(b, -1.0f, 1.0f)); }

    /// Configure compressor (threshold dB, ratio, attack ms, release ms, enabled)
    void set_compressor(float thresh, float rat, float att, float rel, bool en) {
        compressor_.threshold.store(std::clamp(thresh, -60.0f, 0.0f));
        compressor_.ratio.store(std::clamp(rat, 1.0f, 20.0f));
        compressor_.attack_ms.store(std::clamp(att, 0.1f, 100.0f));
        compressor_.release_ms.store(std::clamp(rel, 10.0f, 1000.0f));
        compressor_.enabled.store(en);
    }

    /// Set crossover filter mode (0=off, 1=LPF, 2=HPF) and frequency (Hz)
    void set_crossover(int mode, float freq_hz) {
        crossover_.mode.store(std::clamp(mode, 0, 2));
        crossover_.freq.store(std::clamp(freq_hz, 20.0f, 20000.0f));
        crossover_.update_coeffs();
    }

    /// Set spatial audio (enabled, width 0-2, crossfeed 0-0.5)
    void set_spatial(bool en, float w, float cf) {
        spatializer_.enabled.store(en);
        spatializer_.width.store(std::clamp(w, 0.0f, 2.0f));
        spatializer_.crossfeed.store(std::clamp(cf, 0.0f, 0.5f));
    }

    /// Set EQ gain for a band (0=low, 1=mid, 2=high), gain in dB (-12..+12)
    void set_eq(int band, float gain_db) {
        if (band >= 0 && band < 3) {
            eq_.gains[band].store(std::clamp(gain_db, -12.0f, 12.0f));
            eq_.update_coeffs();
        }
    }

    /// Set exclusive (hog) mode for this device
    bool set_exclusive(bool exclusive) {
        AudioObjectPropertyAddress addr = {
            kAudioDevicePropertyHogMode,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        if (exclusive) {
            pid_t pid = getpid();
            OSStatus status = AudioObjectSetPropertyData(device_id_, &addr, 0, nullptr, sizeof(pid_t), &pid);
            return status == noErr;
        } else {
            pid_t pid = -1;
            OSStatus status = AudioObjectSetPropertyData(device_id_, &addr, 0, nullptr, sizeof(pid_t), &pid);
            return status == noErr;
        }
    }
    uint32_t delay_frames() const { return delay_frames_.load(); }
    uint32_t get_device_id() const { return device_id_; }
    bool is_running() const { return running_.load(); }

    /// Measured end-to-end latency in ms (smoothed EMA).
    float measured_latency_ms() const { return measured_latency_ms_.load(std::memory_order_relaxed); }

    /// Audio level: RMS (0.0-1.0, EMA smoothed)
    float level_rms() const { return level_rms_.load(std::memory_order_relaxed); }
    /// Audio level: Peak (0.0-1.0, with decay)
    float level_peak() const { return level_peak_.load(std::memory_order_relaxed); }

private:
    static uint64_t now_us() {
        using namespace std::chrono;
        return (uint64_t)duration_cast<microseconds>(
            steady_clock::now().time_since_epoch()).count();
    }

    static OSStatus render_cb(void* ref,
                               AudioUnitRenderActionFlags*,
                               const AudioTimeStamp* inTimeStamp,
                               UInt32,
                               UInt32 frame_count,
                               AudioBufferList* ioData) {
        auto* self = static_cast<OutputSink*>(ref);
        auto* dst = static_cast<float*>(ioData->mBuffers[0].mData);
        const uint32_t total = frame_count * self->channels_;

        if (!self->running_.load()) {
            std::memset(dst, 0, total * sizeof(float));
            return noErr;
        }

        const float vol = self->muted_.load() ? 0.0f : self->volume_.load();

        // Target fill = base (40ms) + delay compensation
        const uint32_t base_fill = std::max(frame_count * 4, 1920u);
        const uint32_t target = base_fill + self->delay_frames_.load();

        // Gradual drift correction — same as primary
        {
            size_t avail_now = self->ring_.available_read();
            if (self->prefilled_ && avail_now > static_cast<size_t>(target) + frame_count * 2) {
                size_t drift = std::min(avail_now - static_cast<size_t>(target),
                                        static_cast<size_t>(frame_count / 10 + 1));
                self->ring_.discard(drift);
            }
        }

        const size_t avail = self->ring_.available_read();
        constexpr float kFadeIn  = 0.001f;
        constexpr float kFadeOut = 0.005f;

        if (!self->prefilled_) {
            if (avail < target) {
                self->ramp_ *= (1.0f - kFadeOut);
                std::memset(dst, 0, total * sizeof(float));
                return noErr;
            }
            self->prefilled_ = true;
        }

        if (avail < frame_count) {
            self->prefilled_ = false;
            for (uint32_t i = 0; i < frame_count; i++) {
                self->ramp_ *= (1.0f - kFadeOut);
                for (uint32_t ch = 0; ch < self->channels_; ch++) {
                    dst[i * self->channels_ + ch] = self->held_sample_[ch] * self->ramp_;
                }
            }
            return noErr;
        }

        // Normal playback
        self->ring_.read(self->read_buf_.data(), frame_count);
        const int32_t* src = self->read_buf_.data();
        const float bal = self->balance_.load(std::memory_order_relaxed);
        // Constant-power L/R gain: balance -1..0..+1
        const float gain_l = (self->channels_ >= 2 && bal > 0) ? (1.0f - bal) : 1.0f;
        const float gain_r = (self->channels_ >= 2 && bal < 0) ? (1.0f + bal) : 1.0f;
        for (uint32_t i = 0; i < frame_count; i++) {
            self->ramp_ += kFadeIn * (vol - self->ramp_);
            for (uint32_t ch = 0; ch < self->channels_; ch++) {
                const uint32_t idx = i * self->channels_ + ch;
                float s = static_cast<float>(src[idx]) / 8388607.0f;
                if (s > 0.9f)       s = 0.9f + 0.1f * std::tanh((s - 0.9f) * 5.0f);
                else if (s < -0.9f) s = -0.9f + 0.1f * std::tanh((s + 0.9f) * 5.0f);
                // 3-band EQ
                s = self->eq_.process(s, ch);
                // Compressor
                s = self->compressor_.process(s);
                // Crossover filter (LPF/HPF)
                s = self->crossover_.process(s, ch);
                float ch_gain = (ch == 0) ? gain_l : (ch == 1) ? gain_r : 1.0f;
                const float out = s * self->ramp_ * ch_gain;
                dst[idx] = out;
                self->held_sample_[ch] = out;
            }
        }

        // Stereo spatializer post-processing (operates on stereo pairs)
        if (self->channels_ >= 2 && self->spatializer_.enabled.load(std::memory_order_relaxed)) {
            for (uint32_t f = 0; f < frame_count; f++) {
                self->spatializer_.process(dst[f * self->channels_], dst[f * self->channels_ + 1]);
            }
        }

        // Measure actual latency: time from last write() to this render callback,
        // plus CoreAudio's reported output latency (HW buffer + transport).
        // EMA smoothing (alpha=0.05) to avoid jitter.
        {
            uint64_t write_ts = self->last_write_us_.load(std::memory_order_relaxed);
            if (write_ts > 0) {
                uint64_t render_ts = now_us();
                float buffer_ms = static_cast<float>(avail) / 48.0f;  // frames in buffer → ms
                float hw_ms = 0.0f;
                // Query device output latency + safety offset (includes BT/AirPlay transport)
                if (inTimeStamp && (inTimeStamp->mFlags & kAudioTimeStampHostTimeValid)) {
                    UInt32 latency_frames = 0, safety_frames = 0;
                    UInt32 sz = sizeof(UInt32);
                    AudioObjectPropertyAddress laddr = {
                        kAudioDevicePropertyLatency,
                        kAudioDevicePropertyScopeOutput,
                        kAudioObjectPropertyElementMain
                    };
                    AudioObjectGetPropertyData(self->device_id_, &laddr, 0, nullptr, &sz, &latency_frames);
                    AudioObjectPropertyAddress saddr = {
                        kAudioDevicePropertySafetyOffset,
                        kAudioDevicePropertyScopeOutput,
                        kAudioObjectPropertyElementMain
                    };
                    sz = sizeof(UInt32);
                    AudioObjectGetPropertyData(self->device_id_, &saddr, 0, nullptr, &sz, &safety_frames);
                    hw_ms = static_cast<float>(latency_frames + safety_frames) / 48.0f;
                }
                float total_ms = buffer_ms + hw_ms;
                float prev = self->measured_latency_ms_.load(std::memory_order_relaxed);
                float smoothed = (prev <= 0) ? total_ms : prev * 0.95f + total_ms * 0.05f;
                self->measured_latency_ms_.store(smoothed, std::memory_order_relaxed);
            }
        }

        // VU meter: compute RMS and track peak with decay
        {
            float sum_sq = 0;
            float peak = 0;
            for (uint32_t i = 0; i < total; i++) {
                float s = std::fabs(dst[i]);
                sum_sq += dst[i] * dst[i];
                if (s > peak) peak = s;
            }
            float rms = std::sqrt(sum_sq / static_cast<float>(total));
            // EMA smoothing (α=0.3 for responsive meter)
            float prev_rms = self->level_rms_.load(std::memory_order_relaxed);
            self->level_rms_.store(prev_rms * 0.7f + rms * 0.3f, std::memory_order_relaxed);
            // Peak hold with decay
            float prev_peak = self->level_peak_.load(std::memory_order_relaxed);
            if (peak > prev_peak) {
                self->level_peak_.store(peak, std::memory_order_relaxed);
            } else {
                self->level_peak_.store(prev_peak * 0.95f, std::memory_order_relaxed);
            }
        }

        return noErr;
    }

    uint32_t device_id_;
    uint32_t channels_;
    AudioUnit audio_unit_ = nullptr;
    std::atomic<float>    volume_;
    std::atomic<bool>     muted_;
    std::atomic<bool>     running_{false};
    std::atomic<uint32_t> delay_frames_;
    std::atomic<uint64_t> last_write_us_{0};       // timestamp of last write()
    std::atomic<float>    measured_latency_ms_{0};  // EMA-smoothed measured latency
    std::atomic<float>    level_rms_{0};            // VU meter RMS (0-1)
    std::atomic<float>    level_peak_{0};           // VU meter peak (0-1, with decay)
    std::atomic<float>    balance_{0};              // L/R balance: -1=left, 0=center, 1=right
    EQ3Band               eq_;                      // 3-band parametric EQ
    Compressor            compressor_;              // Dynamics compressor
    CrossoverFilter       crossover_;               // LPF/HPF crossover filter
    StereoSpatializer     spatializer_;             // Virtual surround / stereo widener

    pipeline::RingBuffer  ring_;
    std::vector<int32_t>  read_buf_;
    std::vector<int32_t>  drain_buf_;

    // Audio-callback-only state
    bool                  prefilled_ = false;
    float                 ramp_;
    std::vector<float>    held_sample_;
};

/// Internal receiver implementation
class ReceiverImpl {
public:
    ReceiverImpl(const std::string& multicast_group, uint16_t port, uint32_t channels)
        : multicast_group_(multicast_group)
        , port_(port)
        , channels_(channels)
        , volume_(1.0f)
        , muted_(false)
        , running_(false)
        , target_fill_frames_(3840)  // 80ms default — absorbs WiFi jitter
        , ring_buffer_(48000, channels * sizeof(int32_t))  // 1s capacity (next pow2 = 65536)
        , read_buffer_(4096 * channels)
        , drain_buf_(4096 * channels)
        , held_sample_(channels, 0)
        , ramp_(0.0f)
    {}

    ~ReceiverImpl() {
        stop();
    }

    bool start() {
        if (running_.load()) return false;

        // Create RTP receiver
        SimpleRtpReceiver::Config rx_config;
        rx_config.listen_port = port_;
        rx_config.multicast_group = multicast_group_;
        rx_config.channels = channels_;
        rx_config.format = SampleFormat::S24_LE;
        rx_config.mode = ReceiveMode::Auto;

        rtp_receiver_ = std::make_unique<SimpleRtpReceiver>(rx_config);
        if (!rtp_receiver_->init()) {
            return false;
        }

        // Wire fan-out: replicate audio data to all extra output sinks
        rtp_receiver_->on_audio_written = [this](const void* data, size_t frames) {
            std::lock_guard<std::mutex> lock(sinks_mutex_);
            for (auto& sink : extra_sinks_) {
                sink->write(data, frames);
            }
        };

        // Create audio output device
        audio_device_ = pal::AudioDevice::create();
        if (!audio_device_) {
            fprintf(stderr, "[SolunaRx] Failed to create audio device\n");
            return false;
        }

        pal::AudioStreamConfig audio_config;
        audio_config.sample_rate = kDefaultSampleRate;
        audio_config.channels = channels_;
        audio_config.frames_per_buffer = 256;  // ~5ms
        audio_config.format = SampleFormat::S24_LE;

        fprintf(stderr, "[SolunaRx] Opening audio output: %uHz, %uch, %u frames/buf\n",
                audio_config.sample_rate, audio_config.channels, audio_config.frames_per_buffer);

        if (!audio_device_->open_output("default", audio_config)) {
            fprintf(stderr, "[SolunaRx] Failed to open audio output\n");
            return false;
        }

        running_.store(true);

        // Start audio playback FIRST so the render callback is active before data arrives
        auto callback = [this](float* buffer, uint32_t frame_count) {
            audio_callback(buffer, frame_count);
        };

        if (!audio_device_->start(callback)) {
            fprintf(stderr, "[SolunaRx] Failed to start audio device\n");
            running_.store(false);
            return false;
        }

        // Flush ALL stale packets from the UDP socket buffer.
        // The socket may have accumulated seconds of audio while the AudioUnit was initializing.
        if (rtp_receiver_) {
            pipeline::RingBuffer discard_buf(65536, channels_ * sizeof(int32_t));
            int flushed = 0;
            for (int i = 0; i < 100000; i++) {
                if (!rtp_receiver_->receive_packet(discard_buf)) break;
                flushed++;
                if (discard_buf.available_write() < 1024) discard_buf.reset();
            }
            if (flushed > 0) {
                fprintf(stderr, "[SolunaRx] Flushed %d stale packets from socket\n", flushed);
            }
        }

        // Reset ring buffer and state for a clean start
        ring_buffer_.reset();
        prefilled_ = false;
        ramp_ = 0.0f;
        audio_cb_count_.store(0);

        // NOW start the receive thread with a clean slate
        receive_thread_ = std::thread([this]() {
            receive_loop();
        });
        fprintf(stderr, "[SolunaRx] Audio started successfully (multicast=%s port=%u ch=%u)\n",
                multicast_group_.c_str(), port_, channels_);

        // Start WebSocket control server on port 8400
        ws_server_.set_web_files(
            reinterpret_cast<const soluna::control::WebFile*>(embedded_web_files),
            embedded_web_file_count);
        ws_server_.set_message_callback([this](const std::string& msg) -> std::string {
            return handle_ws_command(msg);
        });
        ws_server_.start(8400);

        return true;
    }

    void stop() {
        if (!running_.load()) return;

        running_.store(false);

        // Stop extra sinks first
        {
            std::lock_guard<std::mutex> lock(sinks_mutex_);
            for (auto& sink : extra_sinks_) sink->stop();
            extra_sinks_.clear();
        }

        if (audio_device_) {
            audio_device_->stop();
            audio_device_->close();
        }

        if (receive_thread_.joinable()) {
            receive_thread_.join();
        }

        rtp_receiver_.reset();
        audio_device_.reset();
        ring_buffer_.reset();
    }

    bool is_running() const {
        return running_.load();
    }

    void set_volume(float volume) {
        volume_.store(std::max(0.0f, std::min(1.0f, volume)));
    }

    float volume() const { return volume_.load(); }

    void set_muted(bool muted) { muted_.store(muted); }
    bool is_muted() const { return muted_.load(); }

    void set_buffer_ms(uint32_t ms) {
        ms = std::max(5u, std::min(2000u, ms));
        target_fill_frames_.store(ms * 48u);
    }
    uint32_t buffer_ms() const { return target_fill_frames_.load() / 48u; }

    SimpleRtpReceiver::Stats stats() const {
        if (rtp_receiver_) return rtp_receiver_->stats_snapshot();
        return {};
    }

    int device_health() const {
        return health_.load(std::memory_order_relaxed);
    }

    void set_network_disabled(bool d) { network_disabled_.store(d); }
    bool is_network_disabled() const { return network_disabled_.load(); }

    // ── Relay support ──────────────────────────────────────────────────────

    void set_relay_callback(std::function<void(const uint8_t*, size_t)> cb) {
        relay_callback_ = std::move(cb);
        if (rtp_receiver_) rtp_receiver_->relay_callback = relay_callback_;
    }

    void inject_raw_packet(const uint8_t* data, size_t len) {
        if (rtp_receiver_) rtp_receiver_->inject_raw_packet(data, len, ring_buffer_);
    }

    // ── Multi-output support ─────────────────────────────────────────────

    /// Add an extra output device. Returns sink index (>= 0) or -1 on failure.
    int add_output(uint32_t device_id) {
        auto sink = std::make_unique<OutputSink>(device_id, channels_);
        if (!sink->start()) return -1;
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        int idx = static_cast<int>(extra_sinks_.size());
        extra_sinks_.push_back(std::move(sink));
        return idx;
    }

    /// Remove an output sink by index
    void remove_output(int idx) {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        if (idx >= 0 && idx < static_cast<int>(extra_sinks_.size())) {
            extra_sinks_[idx]->stop();
            extra_sinks_.erase(extra_sinks_.begin() + idx);
        }
    }

    /// Remove output by CoreAudio device ID
    void remove_output_by_device(uint32_t device_id) {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        extra_sinks_.erase(
            std::remove_if(extra_sinks_.begin(), extra_sinks_.end(),
                [device_id](const std::unique_ptr<OutputSink>& s) {
                    if (s->get_device_id() == device_id) { s->stop(); return true; }
                    return false;
                }),
            extra_sinks_.end());
    }

    void set_output_volume(int idx, float v) {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        if (idx >= 0 && idx < static_cast<int>(extra_sinks_.size()))
            extra_sinks_[idx]->set_volume(v);
    }

    void set_output_muted(int idx, bool m) {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        if (idx >= 0 && idx < static_cast<int>(extra_sinks_.size()))
            extra_sinks_[idx]->set_muted(m);
    }

    void set_output_delay(int idx, uint32_t frames) {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        if (idx >= 0 && idx < static_cast<int>(extra_sinks_.size()))
            extra_sinks_[idx]->set_delay_frames(frames);
    }

    /// Set delay on primary output (for unified sync)
    void set_primary_delay(uint32_t frames) {
        primary_delay_frames_.store(frames);
    }
    uint32_t primary_delay() const { return primary_delay_frames_.load(); }

    /// Set L/R balance on primary output
    void set_primary_balance(float b) { primary_balance_.store(std::clamp(b, -1.0f, 1.0f)); }

    /// Set L/R balance on an extra output
    void set_output_balance(int idx, float b) {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        if (idx >= 0 && idx < static_cast<int>(extra_sinks_.size()))
            extra_sinks_[idx]->set_balance(b);
    }

    /// Set EQ band gain on primary output (band 0=low, 1=mid, 2=high; dB -12..+12)
    void set_primary_eq(int band, float gain_db) {
        if (band >= 0 && band < 3) {
            primary_eq_.gains[band].store(std::clamp(gain_db, -12.0f, 12.0f));
            primary_eq_.update_coeffs();
        }
    }

    /// Set EQ band gain on an extra output
    void set_output_eq(int idx, int band, float gain_db) {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        if (idx >= 0 && idx < static_cast<int>(extra_sinks_.size()))
            extra_sinks_[idx]->set_eq(band, gain_db);
    }

    /// Set compressor on primary output
    void set_primary_compressor(float thresh, float rat, float att, float rel, bool en) {
        primary_compressor_.threshold.store(std::clamp(thresh, -60.0f, 0.0f));
        primary_compressor_.ratio.store(std::clamp(rat, 1.0f, 20.0f));
        primary_compressor_.attack_ms.store(std::clamp(att, 0.1f, 100.0f));
        primary_compressor_.release_ms.store(std::clamp(rel, 10.0f, 1000.0f));
        primary_compressor_.enabled.store(en);
    }

    /// Set compressor on an extra output
    void set_output_compressor(int idx, float thresh, float rat, float att, float rel, bool en) {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        if (idx >= 0 && idx < static_cast<int>(extra_sinks_.size()))
            extra_sinks_[idx]->set_compressor(thresh, rat, att, rel, en);
    }

    /// Set crossover on primary output (mode: 0=off, 1=LPF, 2=HPF; freq in Hz)
    void set_primary_crossover(int mode, float freq_hz) {
        primary_crossover_.mode.store(std::clamp(mode, 0, 2));
        primary_crossover_.freq.store(std::clamp(freq_hz, 20.0f, 20000.0f));
        primary_crossover_.update_coeffs();
    }

    /// Set crossover on an extra output
    void set_output_crossover(int idx, int mode, float freq_hz) {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        if (idx >= 0 && idx < static_cast<int>(extra_sinks_.size()))
            extra_sinks_[idx]->set_crossover(mode, freq_hz);
    }

    /// Set spatial audio on primary output
    void set_primary_spatial(bool en, float width, float crossfeed) {
        primary_spatializer_.enabled.store(en);
        primary_spatializer_.width.store(std::clamp(width, 0.0f, 2.0f));
        primary_spatializer_.crossfeed.store(std::clamp(crossfeed, 0.0f, 0.5f));
    }

    /// Set spatial audio on an extra output
    void set_output_spatial(int idx, bool en, float width, float crossfeed) {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        if (idx >= 0 && idx < static_cast<int>(extra_sinks_.size()))
            extra_sinks_[idx]->set_spatial(en, width, crossfeed);
    }

    /// Set exclusive (hog) mode on an extra output
    bool set_output_exclusive(int idx, bool exclusive) {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        if (idx >= 0 && idx < static_cast<int>(extra_sinks_.size()))
            return extra_sinks_[idx]->set_exclusive(exclusive);
        return false;
    }

    int output_count() {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        return static_cast<int>(extra_sinks_.size());
    }

    /// Get device ID of an output sink
    uint32_t output_device_id(int idx) {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        if (idx >= 0 && idx < static_cast<int>(extra_sinks_.size()))
            return extra_sinks_[idx]->get_device_id();
        return 0;
    }

    /// Get measured latency of an output sink (ms, EMA-smoothed)
    float output_measured_latency(int idx) {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        if (idx >= 0 && idx < static_cast<int>(extra_sinks_.size()))
            return extra_sinks_[idx]->measured_latency_ms();
        return 0;
    }

    /// Get measured latency by device ID
    float output_measured_latency_by_device(uint32_t device_id) {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        for (auto& sink : extra_sinks_) {
            if (sink->get_device_id() == device_id)
                return sink->measured_latency_ms();
        }
        return 0;
    }

    // ── Recording ─────────────────────────────────────────────────────────
    void set_record_callback(std::function<void(const float*, uint32_t)> cb) {
        std::lock_guard<std::mutex> lock(record_mutex_);
        record_callback_ = std::move(cb);
    }

    // ── VU meter ──────────────────────────────────────────────────────────
    float primary_rms() const { return primary_level_rms_.load(std::memory_order_relaxed); }
    float primary_peak() const { return primary_level_peak_.load(std::memory_order_relaxed); }

    // Spectrum analyzer: get 32-band magnitudes (0.0-1.0)
    void get_spectrum_bands(float* out) const { spectrum_.get_bands(out); }

    float output_rms_by_device(uint32_t device_id) {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        for (auto& sink : extra_sinks_) {
            if (sink->get_device_id() == device_id)
                return sink->level_rms();
        }
        return 0;
    }
    float output_peak_by_device(uint32_t device_id) {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        for (auto& sink : extra_sinks_) {
            if (sink->get_device_id() == device_id)
                return sink->level_peak();
        }
        return 0;
    }

private:
    // ── Health tracking helpers (audio-callback thread only) ──────────────

    static uint64_t now_ms_() {
        using namespace std::chrono;
        return (uint64_t)duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()).count();
    }

    void record_underrun_now() {
        uint64_t now = now_ms_();
        if (health_window_start_ms_ == 0 || now - health_window_start_ms_ >= 30000) {
            health_window_start_ms_ = now;
            health_underruns_in_window_ = 0;
        }
        health_underruns_in_window_++;
        last_underrun_ms_ = now;

        int cur = health_.load(std::memory_order_relaxed);
        if (health_underruns_in_window_ >= 20 && cur < 2) {
            // Extreme underruns: silence device to prevent noise
            health_.store(2, std::memory_order_relaxed);
            health_silenced_.store(true, std::memory_order_relaxed);
        } else if (health_underruns_in_window_ >= 5 && cur < 1) {
            // Moderate underruns: stressed — auto-increase buffer 50%, cap at 2000ms
            health_.store(1, std::memory_order_relaxed);
            uint32_t cur_frames = target_fill_frames_.load(std::memory_order_relaxed);
            uint32_t new_frames = std::min(cur_frames + cur_frames / 2, 96000u);
            target_fill_frames_.store(new_frames, std::memory_order_relaxed);
        }
    }

    void maybe_check_recovery() {
        if (health_.load(std::memory_order_relaxed) == 0) return;
        if (++recovery_check_counter_ < 200) return;  // ~1 s at 5ms/callback
        recovery_check_counter_ = 0;
        if (last_underrun_ms_ == 0) return;
        if (now_ms_() - last_underrun_ms_ >= 60000) {
            // 60 seconds clean: restore normal operation
            health_.store(0, std::memory_order_relaxed);
            health_silenced_.store(false, std::memory_order_relaxed);
            health_window_start_ms_ = 0;
            health_underruns_in_window_ = 0;
            last_underrun_ms_ = 0;
        }
    }

    void receive_loop() {
        // ONLY writes to ring_buffer_ — never reads (RingBuffer is SPSC).
        // Drain happens exclusively in audio_callback to avoid data race.
        // Propagate relay callback now that rtp_receiver_ is initialized.
        if (relay_callback_ && rtp_receiver_) {
            rtp_receiver_->relay_callback = relay_callback_;
        }
        uint64_t loop_count = 0;
        uint64_t last_log_pkts = 0;
        while (running_.load()) {
            // When network_disabled_, audio arrives via inject_raw_packet instead
            if (!network_disabled_.load() && rtp_receiver_) {
                for (int i = 0; i < 10 && running_.load(); i++) {
                    if (!rtp_receiver_->receive_packet(ring_buffer_)) break;
                }
            }
            // Debug: log every ~5 seconds
            if (++loop_count % 50000 == 0) {
                auto st = stats();
                size_t fill = ring_buffer_.available_read();
                fprintf(stderr, "[SolunaRx] pkts=%llu fill=%zu vol=%.2f prefilled=%d cb=%llu\n",
                        (unsigned long long)st.packets_received, fill,
                        (double)volume_.load(), (int)prefilled_,
                        (unsigned long long)audio_cb_count_.load(std::memory_order_relaxed));
                last_log_pkts = st.packets_received;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    std::string handle_ws_command(const std::string& msg) {
        int id = 0;
        auto p = msg.find("\"id\":");
        if (p != std::string::npos) try { id = std::stoi(msg.substr(p + 5)); } catch (...) {}

        std::string cmd;
        p = msg.find("\"command\":\"");
        if (p != std::string::npos) {
            auto s = p + 11, e = msg.find('"', s);
            if (e != std::string::npos) cmd = msg.substr(s, e - s);
        }

        char buf[512];
        if (cmd == "rx.stats" || cmd == "system.stats") {
            auto st = stats();
            size_t fill = ring_buffer_.available_read();
            uint32_t target_ms = target_fill_frames_.load() / 48u;
            snprintf(buf, sizeof(buf),
                "{\"id\":%d,\"success\":true,\"data\":"
                "\"{\\\"packets\\\":%llu,\\\"errors\\\":%llu,"
                "\\\"buf_fill\\\":%zu,\\\"buf_cap\\\":4096,"
                "\\\"volume\\\":%.3f,\\\"muted\\\":%s,"
                "\\\"buf_target_ms\\\":%u}\"}",
                id,
                (unsigned long long)st.packets_received,
                (unsigned long long)st.sequence_errors,
                fill,
                (double)volume_.load(),
                muted_.load() ? "true" : "false",
                target_ms);
        } else if (cmd == "rx.set_buffer") {
            p = msg.find("\"ms\":");
            if (p != std::string::npos) {
                try {
                    uint32_t ms = static_cast<uint32_t>(std::stoul(msg.substr(p + 5)));
                    ms = std::max(5u, std::min(200u, ms));
                    target_fill_frames_.store(ms * 48u);
                } catch (...) {}
            }
            snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
        } else if (cmd == "rx.set_volume") {
            p = msg.find("\"volume\":");
            if (p != std::string::npos) {
                try { set_volume(std::stof(msg.substr(p + 9))); } catch (...) {}
            }
            snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
        } else if (cmd == "rx.set_mute") {
            p = msg.find("\"muted\":");
            if (p != std::string::npos)
                set_muted(msg.substr(p + 8, 4) == "true");
            snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);

        // ── monitor.* aliases (Mac DaemonClient compatibility) ──────────
        } else if (cmd == "monitor.stats") {
            auto st = stats();
            uint32_t bms = target_fill_frames_.load() / 48u;
            snprintf(buf, sizeof(buf),
                "{\"id\":%d,\"success\":true,\"data\":"
                "\"{\\\"supported\\\":true,\\\"running\\\":true,"
                "\\\"volume\\\":%.3f,\\\"muted\\\":%s,"
                "\\\"packets\\\":%llu,\\\"buf_ms\\\":%u}\"}",
                id,
                (double)volume_.load(),
                muted_.load() ? "true" : "false",
                (unsigned long long)st.packets_received,
                bms);
        } else if (cmd == "monitor.set_volume") {
            p = msg.find("\"volume\":");
            if (p != std::string::npos) {
                try { set_volume(std::stof(msg.substr(p + 9))); } catch (...) {}
            }
            snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
        } else if (cmd == "monitor.set_mute") {
            p = msg.find("\"muted\":");
            if (p != std::string::npos)
                set_muted(msg.substr(p + 8, 4) == "true");
            snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
        } else if (cmd == "monitor.set_buffer") {
            p = msg.find("\"ms\":");
            if (p != std::string::npos) {
                try {
                    uint32_t ms = static_cast<uint32_t>(std::stoul(msg.substr(p + 5)));
                    set_buffer_ms(ms);
                } catch (...) {}
            }
            snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
        } else if (cmd == "monitor.list_devices") {
            auto devices = pal::AudioDevice::enumerate();
            std::string json_arr = "[";
            bool first = true;
            for (const auto& d : devices) {
                if (d.max_output_channels == 0) continue;
                if (!first) json_arr += ",";
                first = false;
                const char* ttype = "unknown";
                switch (d.transport_type) {
                    case pal::TransportType::BuiltIn: ttype = "builtin"; break;
                    case pal::TransportType::USB: ttype = "usb"; break;
                    case pal::TransportType::Bluetooth: ttype = "bluetooth"; break;
                    case pal::TransportType::AirPlay: ttype = "airplay"; break;
                    case pal::TransportType::Virtual: ttype = "virtual"; break;
                    default: break;
                }
                char entry[256];
                snprintf(entry, sizeof(entry),
                    "{\\\"id\\\":%s,\\\"name\\\":\\\"%s\\\","
                    "\\\"type\\\":\\\"%s\\\",\\\"channels\\\":%u,"
                    "\\\"latency_ms\\\":%.1f}",
                    d.id.c_str(), d.name.c_str(), ttype,
                    d.max_output_channels,
                    (float)(d.hardware_latency_frames + d.safety_offset_frames) / 48.0f);
                json_arr += entry;
            }
            json_arr += "]";
            // Use dynamic buffer for potentially long device list
            std::string resp = "{\"id\":" + std::to_string(id) +
                ",\"success\":true,\"data\":\"" + json_arr + "\"}";
            return resp;
        } else if (cmd == "system.info") {
            snprintf(buf, sizeof(buf),
                "{\"id\":%d,\"success\":true,\"data\":"
                "\"{\\\"tunnel_url\\\":\\\"\\\"}\"}",
                id);
        } else {
            snprintf(buf, sizeof(buf),
                "{\"id\":%d,\"success\":false,\"data\":\"unknown command\"}", id);
        }
        return buf;
    }

    void audio_callback(float* buffer, uint32_t frame_count) {
        audio_cb_count_.fetch_add(1, std::memory_order_relaxed);
        const float vol = (muted_.load() || health_silenced_.load(std::memory_order_relaxed))
                          ? 0.0f : volume_.load();
        const uint32_t total_samples = frame_count * channels_;

        // Adaptive target: always >= frame_count*4 to prevent immediate underrun
        uint32_t target = target_fill_frames_.load() + primary_delay_frames_.load();
        const uint32_t min_target = frame_count * 4;
        if (target < min_target) {
            target = min_target;
        }

        // ── Latency management ────────────────────────────────────────────────
        // Gradual drift correction: when buffer exceeds target, discard a few
        // frames per callback to gently bring it back. This avoids the audible
        // glitches caused by discarding large chunks at once.
        {
            size_t avail_now = ring_buffer_.available_read();
            if (prefilled_ && avail_now > static_cast<size_t>(target) + frame_count * 2) {
                // Soft drift: discard up to 10% of frame_count per callback (max ~25 frames)
                // This creates an imperceptible speed-up instead of audible gaps
                size_t drift = std::min(avail_now - static_cast<size_t>(target),
                                        static_cast<size_t>(frame_count / 10 + 1));
                ring_buffer_.discard(drift);
            }
        }

        const size_t avail = ring_buffer_.available_read();
        const bool has_data = (avail >= frame_count);

        constexpr float kFadeIn  = 0.001f;
        constexpr float kFadeOut = 0.005f;

        if (!prefilled_) {
            if (avail < target) {
                ramp_ *= (1.0f - kFadeOut);
                std::memset(buffer, 0, total_samples * sizeof(float));
                return;
            }
            prefilled_ = true;
            // On first fill: discard excess beyond 3× target to minimize initial latency.
            const size_t ideal_fill = static_cast<size_t>(target) * 3;
            if (avail > ideal_fill + frame_count) {
                ring_buffer_.discard(avail - ideal_fill);
            }
        }

        if (!has_data) {
            prefilled_ = false;
            record_underrun_now();
            for (uint32_t i = 0; i < frame_count; i++) {
                ramp_ *= (1.0f - kFadeOut);
                for (uint32_t ch = 0; ch < channels_; ch++) {
                    buffer[i * channels_ + ch] = held_sample_[ch] * ramp_;
                }
            }
            return;
        }

        // Periodically check if health can be recovered
        maybe_check_recovery();

        // Normal playback with soft limiter to prevent clipping
        ring_buffer_.read(read_buffer_.data(), frame_count);
        const int32_t* src = read_buffer_.data();
        const float bal = primary_balance_.load(std::memory_order_relaxed);
        const float gain_l = (channels_ >= 2 && bal > 0) ? (1.0f - bal) : 1.0f;
        const float gain_r = (channels_ >= 2 && bal < 0) ? (1.0f + bal) : 1.0f;
        for (uint32_t i = 0; i < frame_count; i++) {
            ramp_ += kFadeIn * (vol - ramp_);
            for (uint32_t ch = 0; ch < channels_; ch++) {
                const uint32_t idx = i * channels_ + ch;
                float s = static_cast<float>(src[idx]) / 8388607.0f;
                // Soft limiter: tanh-style knee at ±0.9 to prevent hard clipping
                if (s > 0.9f)       s = 0.9f + 0.1f * std::tanh((s - 0.9f) * 5.0f);
                else if (s < -0.9f) s = -0.9f + 0.1f * std::tanh((s + 0.9f) * 5.0f);
                // 3-band EQ
                s = primary_eq_.process(s, ch);
                // Compressor
                s = primary_compressor_.process(s);
                // Crossover filter (LPF/HPF)
                s = primary_crossover_.process(s, ch);
                float ch_gain = (ch == 0) ? gain_l : (ch == 1) ? gain_r : 1.0f;
                const float out = s * ramp_ * ch_gain;
                buffer[idx] = out;
                held_sample_[ch] = out;
            }
        }

        // Stereo spatializer post-processing (operates on stereo pairs)
        if (channels_ >= 2 && primary_spatializer_.enabled.load(std::memory_order_relaxed)) {
            for (uint32_t f = 0; f < frame_count; f++) {
                primary_spatializer_.process(buffer[f * channels_], buffer[f * channels_ + 1]);
            }
        }

        // VU meter for primary output
        {
            float sum_sq = 0, peak = 0;
            for (uint32_t i = 0; i < total_samples; i++) {
                float s = std::fabs(buffer[i]);
                sum_sq += buffer[i] * buffer[i];
                if (s > peak) peak = s;
            }
            float rms = std::sqrt(sum_sq / static_cast<float>(total_samples));
            float prev_rms = primary_level_rms_.load(std::memory_order_relaxed);
            primary_level_rms_.store(prev_rms * 0.7f + rms * 0.3f, std::memory_order_relaxed);
            float prev_peak = primary_level_peak_.load(std::memory_order_relaxed);
            primary_level_peak_.store(peak > prev_peak ? peak : prev_peak * 0.95f, std::memory_order_relaxed);
        }

        // Spectrum analyzer: feed rendered samples
        spectrum_.feed(buffer, frame_count, channels_);

        // Recording: write rendered samples
        {
            std::lock_guard<std::mutex> lock(record_mutex_);
            if (record_callback_) {
                record_callback_(buffer, total_samples);
            }
        }
    }

    std::function<void(const uint8_t*, size_t)> relay_callback_;

    std::string multicast_group_;
    uint16_t port_;
    uint32_t channels_;
    std::atomic<float>    volume_;
    std::atomic<bool>     muted_;
    std::atomic<bool>     running_;
    std::atomic<uint32_t> target_fill_frames_;
    std::atomic<bool>     network_disabled_{false};
    std::atomic<uint64_t> audio_cb_count_{0};   ///< audio callback invocation counter (debug)
    // Health tracking atomics (written audio-cb, read ObjC):
    std::atomic<int>      health_{0};           ///< 0=good 1=stressed 2=silenced
    std::atomic<bool>     health_silenced_{false};
    // VU meter atomics (written audio-cb, read ObjC):
    std::atomic<float>    primary_level_rms_{0};
    std::atomic<float>    primary_level_peak_{0};
    // audio_callback-only state (no atomics needed):
    bool                  prefilled_ = false;
    float                 ramp_      = 0.0f;
    std::vector<float>    held_sample_;
    // Health tracking — audio-callback-only (no atomic needed):
    uint64_t health_window_start_ms_    = 0;
    uint32_t health_underruns_in_window_ = 0;
    uint64_t last_underrun_ms_          = 0;
    uint32_t recovery_check_counter_    = 0;

    std::atomic<uint32_t> primary_delay_frames_{0};
    std::atomic<float>    primary_balance_{0};        // L/R balance for primary output
    EQ3Band               primary_eq_;                // 3-band parametric EQ for primary
    Compressor            primary_compressor_;        // Dynamics compressor for primary
    CrossoverFilter       primary_crossover_;         // Crossover filter for primary
    StereoSpatializer     primary_spatializer_;       // Virtual surround for primary
    SpectrumAnalyzer       spectrum_;                  // FFT spectrum analyzer

    // Recording callback (called from audio callback with float samples)
    std::function<void(const float*, uint32_t)> record_callback_;
    std::mutex record_mutex_;

    std::unique_ptr<SimpleRtpReceiver> rtp_receiver_;
    std::unique_ptr<pal::AudioDevice>  audio_device_;
    pipeline::RingBuffer  ring_buffer_;
    std::vector<int32_t>  read_buffer_;
    std::vector<int32_t>  drain_buf_;

    std::thread receive_thread_;
    soluna::control::WebSocketServer ws_server_;

    // Multi-output sinks (BT, AirPlay, USB, etc.)
    std::mutex sinks_mutex_;
    std::vector<std::unique_ptr<OutputSink>> extra_sinks_;
};

} // anonymous namespace


// ============================================================================
// Objective-C Implementation
// ============================================================================

// ============================================================================
// SolunaDeviceInfo Implementation
// ============================================================================

@implementation SolunaDeviceInfo {
    uint32_t _deviceId;
    NSString *_name;
    SolunaTransportType _transportType;
    uint32_t _outputChannels;
    uint32_t _hardwareLatencyFrames;
    uint32_t _safetyOffsetFrames;
    double _nativeSampleRate;
}

- (instancetype)initWithInfo:(const pal::AudioDeviceInfo&)info {
    self = [super init];
    if (self) {
        try {
            _deviceId = static_cast<uint32_t>(std::stoul(info.id));
        } catch (...) {
            _deviceId = 0;  // Non-numeric IDs (e.g. "default_output") get ID 0
        }
        _name = [NSString stringWithUTF8String:info.name.c_str()];
        _outputChannels = info.max_output_channels;
        _hardwareLatencyFrames = info.hardware_latency_frames;
        _safetyOffsetFrames = info.safety_offset_frames;
        // Query native sample rate
        {
            Float64 rate = 0;
            UInt32 sz = sizeof(Float64);
            AudioObjectPropertyAddress addr = {
                kAudioDevicePropertyNominalSampleRate,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            AudioObjectGetPropertyData(_deviceId, &addr, 0, nullptr, &sz, &rate);
            _nativeSampleRate = rate;
        }
        switch (info.transport_type) {
            case pal::TransportType::BuiltIn:   _transportType = SolunaTransportTypeBuiltIn; break;
            case pal::TransportType::USB:       _transportType = SolunaTransportTypeUsb; break;
            case pal::TransportType::Bluetooth: _transportType = SolunaTransportTypeBluetooth; break;
            case pal::TransportType::AirPlay:   _transportType = SolunaTransportTypeAirPlay; break;
            case pal::TransportType::Virtual:   _transportType = SolunaTransportTypeVirtual; break;
            default:                            _transportType = SolunaTransportTypeUnknown; break;
        }
    }
    return self;
}

- (instancetype)initWithDeviceId:(uint32_t)devId
                            name:(NSString *)name
                   outputChannels:(uint32_t)outCh
                   transportType:(SolunaTransportType)tt
            hardwareLatencyFrames:(uint32_t)latFrames
               safetyOffsetFrames:(uint32_t)safetyFrames
                 nativeSampleRate:(double)rate {
    self = [super init];
    if (self) {
        _deviceId = devId;
        _name = [name copy];
        _outputChannels = outCh;
        _transportType = tt;
        _hardwareLatencyFrames = latFrames;
        _safetyOffsetFrames = safetyFrames;
        _nativeSampleRate = rate;
    }
    return self;
}

- (uint32_t)deviceId { return _deviceId; }
- (NSString *)name { return _name; }
- (SolunaTransportType)transportType { return _transportType; }
- (uint32_t)outputChannels { return _outputChannels; }
- (uint32_t)hardwareLatencyFrames { return _hardwareLatencyFrames; }
- (uint32_t)safetyOffsetFrames { return _safetyOffsetFrames; }
- (float)hardwareLatencyMs {
    return static_cast<float>(_hardwareLatencyFrames + _safetyOffsetFrames) / 48.0f;
}
- (double)nativeSampleRate { return _nativeSampleRate; }

@end


@implementation SolunaReceiverStats {
    uint64_t _packetsReceived;
    uint64_t _packetsDropped;
    uint64_t _packetsConcealed;
    uint64_t _sequenceErrors;
    uint64_t _aes67Packets;
    uint64_t _ostpPackets;
}

- (instancetype)initWithStats:(const SimpleRtpReceiver::Stats&)stats {
    self = [super init];
    if (self) {
        _packetsReceived  = stats.packets_received;
        _packetsDropped   = stats.packets_dropped;
        _packetsConcealed = stats.packets_concealed;
        _sequenceErrors   = stats.sequence_errors;
        _aes67Packets     = stats.aes67_packets;
        _ostpPackets      = stats.ostp_packets;
    }
    return self;
}

- (uint64_t)packetsReceived  { return _packetsReceived; }
- (uint64_t)packetsDropped   { return _packetsDropped; }
- (uint64_t)packetsConcealed { return _packetsConcealed; }
- (uint64_t)sequenceErrors   { return _sequenceErrors; }
- (uint64_t)aes67Packets     { return _aes67Packets; }
- (uint64_t)ostpPackets      { return _ostpPackets; }

@end


@interface SolunaAudioReceiver () {
    std::unique_ptr<ReceiverImpl> _impl;
    NSTimer *_statsTimer;
    uint32_t _bufferTargetMs;
    ExtAudioFileRef _recordFile;
    BOOL _isRecording;
}
@end

@implementation SolunaAudioReceiver

+ (instancetype)sharedInstance {
    static SolunaAudioReceiver *shared = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        shared = [[SolunaAudioReceiver alloc] init];
    });
    return shared;
}

- (instancetype)init {
    return [self initWithMulticastGroup:@"239.69.0.1" port:5004 channels:2];
}

- (instancetype)initWithMulticastGroup:(NSString *)group
                                  port:(uint16_t)port
                              channels:(uint32_t)channels {
    self = [super init];
    if (self) {
        _multicastGroup = [group copy];
        _port = port;
        _channels = channels;
        _volume = 1.0f;
        _bufferTargetMs = 40;
        _state = SolunaReceiverStateStopped;
    }
    return self;
}

- (void)dealloc {
    [self stop];
}

- (BOOL)start {
    if (_state != SolunaReceiverStateStopped) {
        return NO;
    }

    [self willChangeValueForKey:@"state"];
    _state = SolunaReceiverStateConnecting;
    [self didChangeValueForKey:@"state"];

    if ([_delegate respondsToSelector:@selector(receiverDidChange:)]) {
        [_delegate receiverDidChange:_state];
    }

    // Create implementation
    _impl = std::make_unique<ReceiverImpl>(
        std::string([_multicastGroup UTF8String]),
        _port,
        _channels
    );
    _impl->set_volume(_volume);
    _impl->set_buffer_ms(_bufferTargetMs);

    if (!_impl->start()) {
        _impl.reset();

        [self willChangeValueForKey:@"state"];
        _state = SolunaReceiverStateError;
        [self didChangeValueForKey:@"state"];

        if ([_delegate respondsToSelector:@selector(receiverDidChange:)]) {
            [_delegate receiverDidChange:_state];
        }

        NSError *error = [NSError errorWithDomain:@"SolunaReceiver"
                                             code:-1
                                         userInfo:@{NSLocalizedDescriptionKey: @"Failed to start receiver"}];
        if ([_delegate respondsToSelector:@selector(receiverDidEncounter:)]) {
            [_delegate receiverDidEncounter:error];
        }

        return NO;
    }

    [self willChangeValueForKey:@"state"];
    _state = SolunaReceiverStateReceiving;
    [self didChangeValueForKey:@"state"];

    if ([_delegate respondsToSelector:@selector(receiverDidChange:)]) {
        [_delegate receiverDidChange:_state];
    }

    // Start stats timer
    _statsTimer = [NSTimer scheduledTimerWithTimeInterval:0.5
                                                   target:self
                                                 selector:@selector(updateStats)
                                                 userInfo:nil
                                                  repeats:YES];

    return YES;
}

- (void)stop {
    [_statsTimer invalidate];
    _statsTimer = nil;

    if (_impl) {
        _impl->stop();
        _impl.reset();
    }

    [self willChangeValueForKey:@"state"];
    _state = SolunaReceiverStateStopped;
    [self didChangeValueForKey:@"state"];

    if ([_delegate respondsToSelector:@selector(receiverDidChange:)]) {
        [_delegate receiverDidChange:_state];
    }
}

- (void)setVolume:(float)volume {
    _volume = std::max(0.0f, std::min(1.0f, volume));
    if (_impl) _impl->set_volume(_volume);
}

- (BOOL)muted {
    return _impl ? (BOOL)_impl->is_muted() : NO;
}

- (void)setMuted:(BOOL)muted {
    if (_impl) _impl->set_muted((bool)muted);
}

- (uint32_t)bufferTargetMs {
    return _impl ? _impl->buffer_ms() : _bufferTargetMs;
}

- (void)setBufferTargetMs:(uint32_t)ms {
    _bufferTargetMs = std::max(5u, std::min(2000u, ms));
    if (_impl) _impl->set_buffer_ms(_bufferTargetMs);
}

- (SolunaDeviceHealth)deviceHealth {
    return _impl ? (SolunaDeviceHealth)_impl->device_health() : SolunaDeviceHealthGood;
}

- (BOOL)networkDisabled {
    return _impl ? (BOOL)_impl->is_network_disabled() : NO;
}

- (void)setNetworkDisabled:(BOOL)disabled {
    if (_impl) _impl->set_network_disabled((bool)disabled);
}

- (SolunaReceiverStats *)currentStats {
    if (_impl) {
        auto stats = _impl->stats();
        return [[SolunaReceiverStats alloc] initWithStats:stats];
    }
    return [[SolunaReceiverStats alloc] init];
}

- (void)updateStats {
    if ([_delegate respondsToSelector:@selector(receiverDidUpdate:)]) {
        [_delegate receiverDidUpdate:[self currentStats]];
    }
}

// ── Local output devices ────────────────────────────────────────────────────

+ (NSArray<SolunaDeviceInfo *> *)availableOutputDevices {
    NSMutableArray<SolunaDeviceInfo *> *result = [NSMutableArray array];

    // Query CoreAudio directly (bypass PAL to avoid C++ ABI issues)
    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 dataSize = 0;
    OSStatus st = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize);
    if (st != noErr || dataSize == 0) return @[];

    UInt32 count = dataSize / sizeof(AudioDeviceID);
    std::vector<AudioDeviceID> devIds(count);
    st = AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize, devIds.data());
    if (st != noErr) return @[];

    for (AudioDeviceID devId : devIds) {
        // Check output channels
        AudioObjectPropertyAddress chProp = {
            kAudioDevicePropertyStreamConfiguration,
            kAudioDevicePropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        UInt32 chSize = 0;
        st = AudioObjectGetPropertyDataSize(devId, &chProp, 0, nullptr, &chSize);
        if (st != noErr || chSize == 0) continue;

        std::vector<uint8_t> chBuf(chSize);
        auto *bufList = reinterpret_cast<AudioBufferList*>(chBuf.data());
        st = AudioObjectGetPropertyData(devId, &chProp, 0, nullptr, &chSize, bufList);
        if (st != noErr) continue;

        uint32_t outCh = 0;
        for (UInt32 i = 0; i < bufList->mNumberBuffers; i++)
            outCh += bufList->mBuffers[i].mNumberChannels;
        if (outCh == 0) continue;

        // Get device name
        CFStringRef nameRef = nullptr;
        UInt32 nameSize = sizeof(nameRef);
        AudioObjectPropertyAddress nameProp = {
            kAudioDevicePropertyDeviceNameCFString,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        NSString *name = @"Unknown";
        if (AudioObjectGetPropertyData(devId, &nameProp, 0, nullptr, &nameSize, &nameRef) == noErr && nameRef) {
            name = (__bridge_transfer NSString *)nameRef;
        }

        // Get transport type
        UInt32 transport = 0;
        UInt32 tSz = sizeof(transport);
        AudioObjectPropertyAddress tProp = {kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        AudioObjectGetPropertyData(devId, &tProp, 0, nullptr, &tSz, &transport);

        SolunaTransportType stt = SolunaTransportTypeUnknown;
        switch (transport) {
            case kAudioDeviceTransportTypeBuiltIn:   stt = SolunaTransportTypeBuiltIn; break;
            case kAudioDeviceTransportTypeUSB:       stt = SolunaTransportTypeUsb; break;
            case kAudioDeviceTransportTypeBluetooth:
            case kAudioDeviceTransportTypeBluetoothLE: stt = SolunaTransportTypeBluetooth; break;
            case kAudioDeviceTransportTypeAirPlay:   stt = SolunaTransportTypeAirPlay; break;
            case kAudioDeviceTransportTypeVirtual:
            case kAudioDeviceTransportTypeAggregate: stt = SolunaTransportTypeVirtual; break;
            default: break;
        }

        // Get hardware latency + safety offset
        UInt32 latency = 0, safety = 0, sz = sizeof(UInt32);
        AudioObjectPropertyAddress lProp = {kAudioDevicePropertyLatency, kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain};
        AudioObjectGetPropertyData(devId, &lProp, 0, nullptr, &sz, &latency);
        sz = sizeof(UInt32);
        AudioObjectPropertyAddress sProp = {kAudioDevicePropertySafetyOffset, kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain};
        AudioObjectGetPropertyData(devId, &sProp, 0, nullptr, &sz, &safety);

        // Get native sample rate
        Float64 rate = 0;
        sz = sizeof(Float64);
        AudioObjectPropertyAddress rProp = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        AudioObjectGetPropertyData(devId, &rProp, 0, nullptr, &sz, &rate);

        SolunaDeviceInfo *di = [[SolunaDeviceInfo alloc] initWithDeviceId:devId
                                                                    name:name
                                                          outputChannels:outCh
                                                           transportType:stt
                                                   hardwareLatencyFrames:latency
                                                      safetyOffsetFrames:safety
                                                        nativeSampleRate:rate];
        [result addObject:di];
    }
    return [result copy];
}

- (int)addOutputDevice:(uint32_t)deviceId {
    if (!_impl) return -1;
    return _impl->add_output(deviceId);
}

- (void)removeOutputDevice:(uint32_t)deviceId {
    if (_impl) _impl->remove_output_by_device(deviceId);
}

- (void)setVolume:(float)volume forOutput:(int)sinkIndex {
    if (_impl) _impl->set_output_volume(sinkIndex, volume);
}

- (void)setMuted:(BOOL)muted forOutput:(int)sinkIndex {
    if (_impl) _impl->set_output_muted(sinkIndex, (bool)muted);
}

- (void)setDelay:(uint32_t)frames forOutput:(int)sinkIndex {
    if (_impl) _impl->set_output_delay(sinkIndex, frames);
}

- (void)setPrimaryDelay:(uint32_t)frames {
    if (_impl) _impl->set_primary_delay(frames);
}

- (void)setBalance:(float)balance forOutput:(int)sinkIndex {
    if (_impl) _impl->set_output_balance(sinkIndex, balance);
}

- (void)setPrimaryBalance:(float)balance {
    if (_impl) _impl->set_primary_balance(balance);
}

- (BOOL)setExclusive:(BOOL)exclusive forOutput:(int)sinkIndex {
    return _impl ? _impl->set_output_exclusive(sinkIndex, exclusive) : NO;
}

- (void)setEQBand:(int)band gain:(float)gainDb forOutput:(int)sinkIndex {
    if (_impl) _impl->set_output_eq(sinkIndex, band, gainDb);
}

- (void)setPrimaryEQBand:(int)band gain:(float)gainDb {
    if (_impl) _impl->set_primary_eq(band, gainDb);
}

- (void)setPrimaryCompressorThreshold:(float)thresh ratio:(float)ratio attack:(float)attackMs release:(float)releaseMs enabled:(BOOL)enabled {
    if (_impl) _impl->set_primary_compressor(thresh, ratio, attackMs, releaseMs, enabled);
}

- (void)setCompressorThreshold:(float)thresh ratio:(float)ratio attack:(float)attackMs release:(float)releaseMs enabled:(BOOL)enabled forOutput:(int)sinkIndex {
    if (_impl) _impl->set_output_compressor(sinkIndex, thresh, ratio, attackMs, releaseMs, enabled);
}

- (void)setPrimaryCrossoverMode:(int)mode frequency:(float)freqHz {
    if (_impl) _impl->set_primary_crossover(mode, freqHz);
}

- (void)setCrossoverMode:(int)mode frequency:(float)freqHz forOutput:(int)sinkIndex {
    if (_impl) _impl->set_output_crossover(sinkIndex, mode, freqHz);
}

- (void)setPrimarySpatialEnabled:(BOOL)enabled width:(float)width crossfeed:(float)crossfeed {
    if (_impl) _impl->set_primary_spatial(enabled, width, crossfeed);
}

- (void)setSpatialEnabled:(BOOL)enabled width:(float)width crossfeed:(float)crossfeed forOutput:(int)sinkIndex {
    if (_impl) _impl->set_output_spatial(sinkIndex, enabled, width, crossfeed);
}

- (int)outputCount {
    return _impl ? _impl->output_count() : 0;
}

- (float)measuredLatencyForDevice:(uint32_t)deviceId {
    return _impl ? _impl->output_measured_latency_by_device(deviceId) : 0;
}

- (float)primaryLevelRms { return _impl ? _impl->primary_rms() : 0; }
- (float)primaryLevelPeak { return _impl ? _impl->primary_peak() : 0; }

- (NSArray<NSNumber *> *)spectrumBands {
    float bands[32] = {};
    if (_impl) _impl->get_spectrum_bands(bands);
    NSMutableArray *arr = [NSMutableArray arrayWithCapacity:32];
    for (int i = 0; i < 32; i++) [arr addObject:@(bands[i])];
    return arr;
}
- (float)levelRmsForDevice:(uint32_t)deviceId {
    return _impl ? _impl->output_rms_by_device(deviceId) : 0;
}
- (float)levelPeakForDevice:(uint32_t)deviceId {
    return _impl ? _impl->output_peak_by_device(deviceId) : 0;
}

- (void)setDeviceChangeCallback:(nullable void(^)(void))callback {
    static AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    // Remove previous listener
    static AudioObjectPropertyListenerBlock prevBlock = nil;
    if (prevBlock) {
        AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &addr,
            dispatch_get_main_queue(), prevBlock);
        prevBlock = nil;
    }
    if (callback) {
        AudioObjectPropertyListenerBlock block = ^(UInt32, const AudioObjectPropertyAddress*) {
            callback();
        };
        AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &addr,
            dispatch_get_main_queue(), block);
        prevBlock = block;
    }
}

// ── P2P Relay ───────────────────────────────────────────────────────────────

- (void)setRelayCallback:(nullable void(^)(NSData *))callback {
    if (_impl) {
        if (callback) {
            _impl->set_relay_callback([callback](const uint8_t* data, size_t len) {
                NSData *packet = [NSData dataWithBytes:data length:len];
                callback(packet);
            });
        } else {
            _impl->set_relay_callback(nullptr);
        }
    }
}

- (void)injectRawPacket:(NSData *)data {
    if (_impl && data.length > 0) {
        _impl->inject_raw_packet(static_cast<const uint8_t*>(data.bytes), data.length);
    }
}

// ── Recording ──────────────────────────────────────────────────────────────

- (BOOL)startRecordingToFile:(NSString *)path {
    if (_isRecording) return NO;

    NSURL *url = [NSURL fileURLWithPath:path];
    uint32_t ch = _channels;

    // Client (input) format: 32-bit float interleaved
    AudioStreamBasicDescription clientFormat = {};
    clientFormat.mSampleRate = 48000;
    clientFormat.mFormatID = kAudioFormatLinearPCM;
    clientFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    clientFormat.mBitsPerChannel = 32;
    clientFormat.mChannelsPerFrame = ch;
    clientFormat.mFramesPerPacket = 1;
    clientFormat.mBytesPerFrame = ch * 4;
    clientFormat.mBytesPerPacket = ch * 4;

    // File format: 16-bit WAV
    AudioStreamBasicDescription fileFormat = {};
    fileFormat.mSampleRate = 48000;
    fileFormat.mFormatID = kAudioFormatLinearPCM;
    fileFormat.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fileFormat.mBitsPerChannel = 16;
    fileFormat.mChannelsPerFrame = ch;
    fileFormat.mFramesPerPacket = 1;
    fileFormat.mBytesPerFrame = ch * 2;
    fileFormat.mBytesPerPacket = ch * 2;

    OSStatus status = ExtAudioFileCreateWithURL(
        (__bridge CFURLRef)url,
        kAudioFileWAVEType,
        &fileFormat,
        nullptr,
        kAudioFileFlags_EraseFile,
        &_recordFile
    );
    if (status != noErr) return NO;

    status = ExtAudioFileSetProperty(
        _recordFile,
        kExtAudioFileProperty_ClientDataFormat,
        sizeof(clientFormat),
        &clientFormat
    );
    if (status != noErr) {
        ExtAudioFileDispose(_recordFile);
        _recordFile = nullptr;
        return NO;
    }

    _isRecording = YES;

    // Set record callback on impl
    ExtAudioFileRef file = _recordFile;
    _impl->set_record_callback([file, ch](const float* samples, uint32_t count) {
        AudioBufferList abl;
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = ch;
        abl.mBuffers[0].mDataByteSize = count * sizeof(float);
        abl.mBuffers[0].mData = (void*)samples;
        UInt32 frames = count / ch;
        ExtAudioFileWriteAsync(file, frames, &abl);
    });

    return YES;
}

- (void)stopRecording {
    if (!_isRecording) return;
    _isRecording = NO;

    if (_impl) {
        _impl->set_record_callback(nullptr);
    }

    if (_recordFile) {
        ExtAudioFileDispose(_recordFile);
        _recordFile = nullptr;
    }
}

- (BOOL)isRecording {
    return _isRecording;
}

@end
