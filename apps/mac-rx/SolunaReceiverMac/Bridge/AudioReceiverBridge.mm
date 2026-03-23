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
#include <soluna/pipeline/pipeline.h>
#include <soluna/transport/packet_scheduler.h>
#include <soluna/control/websocket_server.h>

#include <AudioToolbox/AudioToolbox.h>
#include <Accelerate/Accelerate.h>
#import <AVFoundation/AVFoundation.h>

#include "../../../../include/soluna/soluna_shm.h"

#include <atomic>
#include <thread>
#include <mutex>
#include <memory>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
#include <random>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
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

    /// Last received OSTP media_timestamp (wall-clock nanoseconds, lower 32 bits)
    std::atomic<uint32_t> last_media_timestamp{0};

    /// NTP-based sync: pointer to ReceiverImpl's target_fill_frames_ (set externally)
    std::atomic<uint32_t>* sync_target_frames_ = nullptr;
    double sync_playout_delay_ms_ = 80.0;  // default playout delay

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

        // Store media_timestamp for sync mode
        last_media_timestamp.store(ostp.media_timestamp, std::memory_order_relaxed);

        // Discard duplicate packets (gap <= 0 means same or older sequence)
        if (gap < 0) return true;  // duplicate — already received

        // Auto-detect TX channel count from stream_id upper 4 bits
        uint32_t deck_id = (ostp.stream_id >> 14) & 0x3;
        uint32_t tx_channels = (ostp.stream_id >> 10) & 0xF;
        if (tx_channels == 0) tx_channels = 2;  // backward compat

        const uint32_t ring_ch = config_.channels;  // ring buffer channel width

        // §4.9 IMA-ADPCM decode (PT=115 stereo, PT=116 mono)
        if (rtp.pt == 115 || rtp.pt == 116) {
            if (payload_size < 4) return false;
            // Always read state header from each packet (enables recovery after loss)
            adpcm_state_.valprev = static_cast<int16_t>(payload[0] | (payload[1] << 8));
            adpcm_state_.index = std::clamp(static_cast<int>(payload[2]), 0, 88);
            size_t num_samples = (payload_size - 4) * 2;
            size_t adpcm_frames = num_samples / tx_channels;
            // Decode nibbles → 24-bit int32_t for ring buffer
            for (size_t i = 0; i < num_samples && i < adpcm_decode_buf_.size(); i++) {
                uint8_t nib = (i & 1) ? ((payload[4 + i/2] >> 4) & 0x0F)
                                       : (payload[4 + i/2] & 0x0F);
                // IMA-ADPCM decode inline
                int step = ima_step_table_[adpcm_state_.index];
                int dq = step >> 3;
                if (nib & 4) dq += step;
                if (nib & 2) dq += (step >> 1);
                if (nib & 1) dq += (step >> 2);
                adpcm_state_.valprev += (nib & 8) ? -dq : dq;
                if (adpcm_state_.valprev > 32767) adpcm_state_.valprev = 32767;
                if (adpcm_state_.valprev < -32768) adpcm_state_.valprev = -32768;
                adpcm_state_.index += ima_index_table_[nib];
                if (adpcm_state_.index < 0) adpcm_state_.index = 0;
                if (adpcm_state_.index > 88) adpcm_state_.index = 88;
                // 16-bit → 24-bit (shift left 8)
                adpcm_decode_buf_[i] = static_cast<int32_t>(adpcm_state_.valprev) << 8;
            }
            // Write to ring buffer (expand mono → stereo if needed)
            if (tx_channels == ring_ch) {
                ring.write(adpcm_decode_buf_.data(), adpcm_frames);
            } else {
                for (size_t f = 0; f < adpcm_frames; f++) {
                    for (uint32_t c = 0; c < ring_ch; c++) {
                        uint32_t sc = (c < tx_channels) ? c : 0;
                        audio_buf_[f * ring_ch + c] = adpcm_decode_buf_[f * tx_channels + sc];
                    }
                }
                ring.write(audio_buf_.data(), adpcm_frames);
            }
            if (on_audio_written && adpcm_frames > 0)
                on_audio_written(adpcm_decode_buf_.data(), adpcm_frames);
            return true;
        }

        // Raw PCM: seed ADPCM state from last sample (§4.9 Raw First)
        if (rtp.pt == 96 && payload_size >= sizeof(int32_t)) {
            int32_t last = reinterpret_cast<const int32_t*>(payload)[payload_size/sizeof(int32_t) - 1];
            adpcm_state_.valprev = static_cast<int16_t>(last >> 8); // 24-bit → 16-bit
            adpcm_state_.index = 0;
        }

        // OSTP payload is int32_t (4 bytes/sample, native byte order)
        size_t frames = payload_size / (sizeof(int32_t) * tx_channels);

        // PLC: conceal gaps of ≤2 packets by repeating the last known frame
        if (gap > 0 && gap <= 2 && frames > 0 && !last_frame_.empty()) {
            size_t plc_frames = last_frame_.size() / ring_ch;
            for (int32_t i = 0; i < gap; i++) {
                ring.write(last_frame_.data(), plc_frames);
                if (on_audio_written) {
                    on_audio_written(last_frame_.data(), plc_frames);
                }
            }
            stats_.packets_concealed += static_cast<uint64_t>(gap);
        }

        // Expand tx_channels → ring_ch (duplicate mono to all channels, zero-pad surround)
        const int32_t* src_samples = reinterpret_cast<const int32_t*>(payload);
        if (tx_channels == ring_ch) {
            ring.write(payload, frames);
        } else {
            for (size_t f = 0; f < frames; f++) {
                for (uint32_t c = 0; c < tx_channels && c < ring_ch; c++)
                    audio_buf_[f * ring_ch + c] = src_samples[f * tx_channels + c];
                // When mono, duplicate to all channels for proper stereo output
                if (tx_channels == 1) {
                    for (uint32_t c = 1; c < ring_ch; c++)
                        audio_buf_[f * ring_ch + c] = src_samples[f * tx_channels];
                } else {
                    for (uint32_t c = tx_channels; c < ring_ch; c++)
                        audio_buf_[f * ring_ch + c] = 0;
                }
            }
            ring.write(audio_buf_.data(), frames);
        }

        // Fan-out to extra sinks (in ring_ch-wide format)
        if (on_audio_written && frames > 0) {
            if (tx_channels == ring_ch) {
                on_audio_written(payload, frames);
            } else {
                on_audio_written(audio_buf_.data(), frames);
            }
        }

        // Save last frame for PLC (in ring_ch-wide format)
        if (frames > 0) {
            if (tx_channels == ring_ch) {
                last_frame_.assign(src_samples, src_samples + frames * ring_ch);
            } else {
                last_frame_.resize(frames * ring_ch);
                for (size_t f = 0; f < frames; f++) {
                    for (uint32_t c = 0; c < tx_channels && c < ring_ch; c++)
                        last_frame_[f * ring_ch + c] = src_samples[f * tx_channels + c];
                    if (tx_channels == 1) {
                        for (uint32_t c = 1; c < ring_ch; c++)
                            last_frame_[f * ring_ch + c] = src_samples[f * tx_channels];
                    } else {
                        for (uint32_t c = tx_channels; c < ring_ch; c++)
                            last_frame_[f * ring_ch + c] = 0;
                    }
                }
            }
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
    // §4.9 ADPCM decode state + buffer
    struct { int32_t valprev = 0; int32_t index = 0; } adpcm_state_;
    std::vector<int32_t> adpcm_decode_buf_ = std::vector<int32_t>(2048);
    static constexpr int16_t ima_step_table_[89] = {
        7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,
        50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,
        253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,
        1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,
        3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,
        10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,
        27086,29794,32767};
    static constexpr int8_t ima_index_table_[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
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
        split_real_.resize(kFFTSize / 2, 0);
        split_imag_.resize(kFFTSize / 2, 0);
        split_.realp = split_real_.data();
        split_.imagp = split_imag_.data();
        magnitudes_.resize(kFFTSize / 2, 0);
        write_pos_ = 0;
        for (auto& b : bands_) b.store(0, std::memory_order_relaxed);
    }

    ~SpectrumAnalyzer() {
        if (fft_setup_) vDSP_destroy_fftsetup(fft_setup_);
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
    std::vector<float> split_real_;
    std::vector<float> split_imag_;
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
        // Use HALOutput for explicit device routing (OutputSink targets a specific device_id)
        desc.componentSubType = kAudioUnitSubType_HALOutput;
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;

        AudioComponent component = AudioComponentFindNext(nullptr, &desc);
        if (!component) {
            fprintf(stderr, "[OutputSink] HALOutput component not found\n");
            return false;
        }

        OSStatus status = AudioComponentInstanceNew(component, &audio_unit_);
        if (status != noErr) {
            fprintf(stderr, "[OutputSink] Failed to create audio unit: %d\n", (int)status);
            return false;
        }

        // Route to the specific device
        status = AudioUnitSetProperty(audio_unit_,
            kAudioOutputUnitProperty_CurrentDevice,
            kAudioUnitScope_Global, 0,
            &device_id_, sizeof(device_id_));
        if (status != noErr) {
            fprintf(stderr, "[OutputSink] Failed to set device %u: %d\n", device_id_, (int)status);
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
            fprintf(stderr, "[OutputSink] Failed to set format: %d\n", (int)status);
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

        // Render callback
        AURenderCallbackStruct cbs = {};
        cbs.inputProc = render_cb;
        cbs.inputProcRefCon = this;
        status = AudioUnitSetProperty(audio_unit_,
            kAudioUnitProperty_SetRenderCallback,
            kAudioUnitScope_Input, 0,
            &cbs, sizeof(cbs));
        if (status != noErr) {
            fprintf(stderr, "[OutputSink] Failed to set render callback: %d\n", (int)status);
            AudioComponentInstanceDispose(audio_unit_);
            audio_unit_ = nullptr;
            return false;
        }

        status = AudioUnitInitialize(audio_unit_);
        if (status != noErr) {
            fprintf(stderr, "[OutputSink] Failed to initialize: %d\n", (int)status);
            AudioComponentInstanceDispose(audio_unit_);
            audio_unit_ = nullptr;
            return false;
        }

        running_.store(true);
        status = AudioOutputUnitStart(audio_unit_);
        if (status != noErr) {
            fprintf(stderr, "[OutputSink] Failed to start: %d\n", (int)status);
            running_.store(false);
            AudioComponentInstanceDispose(audio_unit_);
            audio_unit_ = nullptr;
            return false;
        }
        fprintf(stderr, "[OutputSink] Started successfully (HALOutput, device=%u, %uch)\n", device_id_, channels_);
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

        // Play whatever is available — no prefill gate.
        // This ensures continuous playback even with thin buffers.
        if (avail == 0) {
            // Nothing to play — output held sample with fade
            for (uint32_t i = 0; i < frame_count; i++) {
                self->ramp_ *= (1.0f - kFadeOut);
                for (uint32_t ch = 0; ch < self->channels_; ch++) {
                    dst[i * self->channels_ + ch] = self->held_sample_[ch] * self->ramp_;
                }
            }
            return noErr;
        }

        uint32_t playable = std::min(static_cast<uint32_t>(avail), frame_count);
        if (playable < frame_count) {
            // Partial buffer — play what we have, hold last sample for rest
            self->ring_.read(self->read_buf_.data(), playable);
            const int32_t* src = self->read_buf_.data();
            float vol = self->volume_.load(std::memory_order_relaxed);
            for (uint32_t i = 0; i < playable; i++) {
                self->ramp_ = std::min(1.0f, self->ramp_ + kFadeIn);
                for (uint32_t ch = 0; ch < self->channels_; ch++) {
                    float s = static_cast<float>(src[i * self->channels_ + ch]) / 8388608.0f;
                    dst[i * self->channels_ + ch] = s * self->ramp_ * vol;
                    self->held_sample_[ch] = s * self->ramp_ * vol;
                }
            }
            for (uint32_t i = playable; i < frame_count; i++) {
                self->ramp_ *= (1.0f - kFadeOut);
                for (uint32_t ch = 0; ch < self->channels_; ch++) {
                    dst[i * self->channels_ + ch] = self->held_sample_[ch] * self->ramp_;
                }
            }
            self->prefilled_ = true;
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

// ============================================================================
// WanRelayClient — UDP relay for internet-based group audio (SonoBus-style)
// ============================================================================

class WanRelayClient {
public:
    enum class State { Disconnected, Connecting, Connected, Error };

    using RxCallback = std::function<void(const uint8_t*, size_t)>;
    using MetaCallback = std::function<void(const std::string&)>;
    using FileCallback = std::function<void(const std::string&)>;
    using SyncCallback = std::function<void(const std::string&)>;
    using MaxDelayCallback = std::function<void(uint32_t)>;
    using VolumeCallback = std::function<void(float)>;
    using MembersCallback = std::function<void(const std::string&)>;

    WanRelayClient() = default;
    ~WanRelayClient() { disconnect(); }

    bool connect(const std::string& host, uint16_t port,
                 const std::string& group, const std::string& password,
                 const std::string& device_name,
                 const std::string& device_id = "") {
        // Auto-disconnect if already connected (enables channel switching)
        if (state_.load() == State::Connected || state_.load() == State::Connecting) {
            fprintf(stderr, "[relay] Auto-disconnect for channel switch → '%s'\n", group.c_str());
            disconnect();
        }

        device_id_ = device_id;
        state_.store(State::Connecting, std::memory_order_relaxed);
        group_ = group;
        error_.clear();

        // DNS resolve
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%u", port);

        int err = getaddrinfo(host.c_str(), port_str, &hints, &res);
        if (err != 0 || !res) {
            error_ = std::string("DNS resolve failed: ") + gai_strerror(err);
            state_.store(State::Error, std::memory_order_relaxed);
            return false;
        }
        relay_addr_ = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
        freeaddrinfo(res);

        // Create UDP socket
        udp_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_sock_ < 0) {
            error_ = "socket() failed";
            state_.store(State::Error, std::memory_order_relaxed);
            return false;
        }

        // Set recv timeout 200ms for fast JOIN handshake
        struct timeval tv{0, 200000};
        setsockopt(udp_sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Send JOIN (needed for mic/system audio TX)
        std::string join_msg = "JOIN:" + group;
        join_msg += ":" + password;
        join_msg += ":" + device_name;
        if (!device_id_.empty()) join_msg += ":" + device_id_;
        join_msg += "\n";
        sendto(udp_sock_, join_msg.c_str(), join_msg.size(), 0,
               reinterpret_cast<const sockaddr*>(&relay_addr_), sizeof(relay_addr_));

        // Wait for OK:joined (up to 5 seconds), also collect PEER messages
        // Need enough attempts to handle groups with many existing members
        char buf[256];
        bool joined = false;
        for (int attempt = 0; attempt < 30 && !joined; attempt++) {
            sockaddr_in from{};
            socklen_t from_len = sizeof(from);
            ssize_t n = recvfrom(udp_sock_, buf, sizeof(buf) - 1, 0,
                                 reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n > 0) {
                buf[n] = '\0';
                if (strncmp(buf, "OK:joined", 9) == 0) {
                    joined = true;
                } else if (strncmp(buf, "ERR:", 4) == 0) {
                    std::string e(buf, n);
                    while (!e.empty() && (e.back() == '\n' || e.back() == '\r')) e.pop_back();
                    error_ = e;
                    ::close(udp_sock_);
                    udp_sock_ = -1;
                    state_.store(State::Error, std::memory_order_relaxed);
                    return false;
                } else if (n >= 6 && strncmp(buf, "PEER:", 5) == 0) {
                    std::string msg(buf, n);
                    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
                        msg.pop_back();
                    auto colon = msg.rfind(':');
                    if (colon != std::string::npos && colon > 5) {
                        std::string ip = msg.substr(5, colon - 5);
                        uint16_t pport = (uint16_t)atoi(msg.substr(colon + 1).c_str());
                        sockaddr_in peer{};
                        peer.sin_family = AF_INET;
                        peer.sin_port = htons(pport);
                        inet_pton(AF_INET, ip.c_str(), &peer.sin_addr);
                        add_peer(peer);
                        fprintf(stderr, "[wan-p2p] Peer discovered (during join): %s:%u\n", ip.c_str(), pport);
                    }
                }
                // Handle FILE/SYNC/META that arrive during join handshake
                else if (n >= 5 && strncmp(buf, "FILE:", 5) == 0) {
                    std::string fname(buf + 5, n - 5);
                    while (!fname.empty() && (fname.back() == '\n' || fname.back() == '\r')) fname.pop_back();
                    std::lock_guard<std::mutex> lk(cb_mutex_);
                    if (file_callback_) file_callback_(fname);
                }
                else if (n >= 5 && strncmp(buf, "SYNC:", 5) == 0) {
                    std::string sync(buf + 5, n - 5);
                    while (!sync.empty() && (sync.back() == '\n' || sync.back() == '\r')) sync.pop_back();
                    std::lock_guard<std::mutex> lk(cb_mutex_);
                    if (sync_callback_) sync_callback_(sync);
                }
                else if (n >= 9 && strncmp(buf, "MAXDELAY:", 9) == 0) {
                    char tmp[32] = {};
                    size_t copy_len = std::min((size_t)n - 9, sizeof(tmp) - 1);
                    memcpy(tmp, buf + 9, copy_len);
                    uint32_t max_ms = (uint32_t)atoi(tmp);
                    if (max_ms > 0) {
                        std::lock_guard<std::mutex> lk(cb_mutex_);
                        if (maxdelay_callback_) maxdelay_callback_(max_ms);
                    }
                }
                else if (n >= 5 && strncmp(buf, "META:", 5) == 0) {
                    std::string meta(buf + 5, n - 5);
                    while (!meta.empty() && (meta.back() == '\n' || meta.back() == '\r')) meta.pop_back();
                    std::lock_guard<std::mutex> lk(cb_mutex_);
                    if (meta_callback_) meta_callback_(meta);
                }
            }
        }

        if (!joined) {
            error_ = "JOIN timeout (no response from relay)";
            ::close(udp_sock_);
            udp_sock_ = -1;
            state_.store(State::Error, std::memory_order_relaxed);
            return false;
        }

        state_.store(State::Connected, std::memory_order_relaxed);
        running_.store(true);

        // Start recv thread
        recv_thread_ = std::thread([this]() { recv_loop(); });

        fprintf(stderr, "[WanRelay] Connected to group '%s'\n", group_.c_str());
        return true;
    }

    void disconnect() {
        running_.store(false);
        // Close socket FIRST to unblock recvfrom() immediately
        if (udp_sock_ >= 0) {
            ::close(udp_sock_);
            udp_sock_ = -1;
        }
        if (recv_thread_.joinable()) {
            recv_thread_.join();
        }
        {
            std::lock_guard<std::mutex> lk(peers_mutex_);
            peers_.clear();
        }
        state_.store(State::Disconnected, std::memory_order_relaxed);
        group_.clear();
    }

    /// Send an OSTP audio packet to peers (P2P) and relay (fallback)
    void send_audio(const uint8_t* data, size_t len) {
        if (udp_sock_ < 0 || state_.load() != State::Connected) return;
        // Send to all direct peers (P2P)
        {
            std::lock_guard<std::mutex> lk(peers_mutex_);
            for (const auto& peer : peers_) {
                sendto(udp_sock_, data, len, 0,
                       reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
            }
        }
        // Also send via relay as fallback
        sendto(udp_sock_, data, len, 0,
               reinterpret_cast<const sockaddr*>(&relay_addr_), sizeof(relay_addr_));
    }

    void set_rx_callback(RxCallback cb) {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        rx_callback_ = std::move(cb);
    }

    void set_meta_callback(MetaCallback cb) {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        meta_callback_ = std::move(cb);
    }

    void set_file_callback(FileCallback cb) {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        file_callback_ = std::move(cb);
    }

    void set_sync_callback(SyncCallback cb) {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        sync_callback_ = std::move(cb);
    }

    void set_maxdelay_callback(MaxDelayCallback cb) {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        maxdelay_callback_ = std::move(cb);
    }

    void set_members_callback(MembersCallback cb) {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        members_callback_ = std::move(cb);
    }

    int64_t clock_offset_ns() const { return clock_offset_ns_.load(std::memory_order_relaxed); }

    void send_ready(const std::string& filename) {
        if (udp_sock_ < 0 || state_.load() != State::Connected) return;
        std::string msg = "READY:" + filename + "\n";
        sendto(udp_sock_, msg.c_str(), msg.size(), 0,
               reinterpret_cast<const sockaddr*>(&relay_addr_), sizeof(relay_addr_));
    }

    /// Send a raw text command to relay (e.g. "FILE:song.mp3\n", "META:{...}\n")
    void send_command(const std::string& cmd) {
        if (udp_sock_ < 0 || state_.load() != State::Connected) return;
        sendto(udp_sock_, cmd.c_str(), cmd.size(), 0,
               reinterpret_cast<const sockaddr*>(&relay_addr_), sizeof(relay_addr_));
    }

    State state() const { return state_.load(std::memory_order_relaxed); }
    const std::string& group() const { return group_; }
    const std::string& error() const { return error_; }
    const std::string& device_id() const { return device_id_; }

    void send_mic_allow(const std::string& target_device_id) {
        send_command(std::string("MIC_ALLOW:") + target_device_id + "\n");
    }

    void send_mic_deny(const std::string& target_device_id) {
        send_command(std::string("MIC_DENY:") + target_device_id + "\n");
    }

    void send_mic_list() {
        send_command("MIC_LIST\n");
    }

    void send_members() {
        send_command("MEMBERS\n");
    }

    /// Send DELAY report to relay (for sync mode coordination)
    void send_delay(uint32_t net_delay_ms) {
        char buf[64];
        snprintf(buf, sizeof(buf), "DELAY:%u\n", net_delay_ms);
        send_command(std::string(buf));
    }

    void set_volume_callback(VolumeCallback cb) {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        volume_callback_ = std::move(cb);
    }

    /// Send VOLUME command to relay targeting a specific device
    void send_volume(const std::string& target_device_id, int level) {
        char buf[128];
        snprintf(buf, sizeof(buf), "VOLUME:%s:%d\n", target_device_id.c_str(), level);
        send_command(std::string(buf));
    }

private:
    void recv_loop() {
        fprintf(stderr, "[recv_loop] STARTED\n");
        // Set receive timeout so heartbeat can fire between packets
        struct timeval tv{1, 0};
        setsockopt(udp_sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        uint8_t buf[65536];
        auto last_hello = std::chrono::steady_clock::now();
        uint64_t total_rx = 0;
        auto last_log = std::chrono::steady_clock::now();

        while (running_.load()) {
            sockaddr_in from{};
            socklen_t from_len = sizeof(from);
            ssize_t n = recvfrom(udp_sock_, buf, sizeof(buf), 0,
                                 reinterpret_cast<sockaddr*>(&from), &from_len);

            if (n > 0) {
                // PEER message: "PEER:ip:port\n"
                if (n >= 6 && memcmp(buf, "PEER:", 5) == 0) {
                    std::string msg((const char*)buf, n);
                    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
                        msg.pop_back();
                    auto colon = msg.rfind(':');
                    if (colon != std::string::npos && colon > 5) {
                        std::string ip = msg.substr(5, colon - 5);
                        uint16_t pport = (uint16_t)atoi(msg.substr(colon + 1).c_str());
                        sockaddr_in peer{};
                        peer.sin_family = AF_INET;
                        peer.sin_port = htons(pport);
                        inet_pton(AF_INET, ip.c_str(), &peer.sin_addr);
                        add_peer(peer);
                        fprintf(stderr, "[wan-p2p] Peer discovered: %s:%u\n", ip.c_str(), pport);
                        // Send a punch packet to open NAT
                        const char* punch = "PUNCH\n";
                        sendto(udp_sock_, punch, 6, 0,
                               reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
                    }
                }
                // META message: metadata broadcast from relay
                else if (n >= 5 && memcmp(buf, "META:", 5) == 0) {
                    std::string meta((const char*)buf + 5, n - 5);
                    while (!meta.empty() && (meta.back() == '\n' || meta.back() == '\r'))
                        meta.pop_back();
                    std::lock_guard<std::mutex> lk(cb_mutex_);
                    if (meta_callback_) meta_callback_(meta);
                }
                // FILE: message — file sync mode, download and prepare
                else if (n >= 5 && memcmp(buf, "FILE:", 5) == 0) {
                    std::string filename((const char*)buf + 5, n - 5);
                    while (!filename.empty() && (filename.back() == '\n' || filename.back() == '\r'))
                        filename.pop_back();
                    std::lock_guard<std::mutex> lk(cb_mutex_);
                    if (file_callback_) file_callback_(filename);
                }
                // SYNC: message — file sync mode, play/pause/seek
                else if (n >= 5 && memcmp(buf, "SYNC:", 5) == 0) {
                    std::string sync((const char*)buf + 5, n - 5);
                    while (!sync.empty() && (sync.back() == '\n' || sync.back() == '\r'))
                        sync.pop_back();
                    std::lock_guard<std::mutex> lk(cb_mutex_);
                    if (sync_callback_) sync_callback_(sync);
                }
                // MEMBERS: response — JSON list of group members
                else if (n >= 8 && memcmp(buf, "MEMBERS:", 8) == 0) {
                    std::string json((const char*)buf + 8, n - 8);
                    while (!json.empty() && (json.back() == '\n' || json.back() == '\r'))
                        json.pop_back();
                    std::lock_guard<std::mutex> lk(cb_mutex_);
                    if (members_callback_) members_callback_(json);
                }
                // MAXDELAY: sync mode group-wide max delay
                else if (n >= 9 && memcmp(buf, "MAXDELAY:", 9) == 0) {
                    char tmp[32] = {};
                    size_t copy_len = std::min((size_t)n - 9, sizeof(tmp) - 1);
                    memcpy(tmp, buf + 9, copy_len);
                    uint32_t max_ms = (uint32_t)atoi(tmp);
                    if (max_ms > 0) {
                        std::lock_guard<std::mutex> lk(cb_mutex_);
                        if (maxdelay_callback_) maxdelay_callback_(max_ms);
                    }
                }
                // VOLUME_SET: remote volume control from another device via relay
                else if (n >= 11 && memcmp(buf, "VOLUME_SET:", 11) == 0) {
                    std::string val((const char*)buf + 11, n - 11);
                    while (!val.empty() && (val.back() == '\n' || val.back() == '\r'))
                        val.pop_back();
                    int level = std::atoi(val.c_str());
                    float vol = std::max(0.0f, std::min(1.0f, level / 100.0f));
                    std::lock_guard<std::mutex> lk(cb_mutex_);
                    if (volume_callback_) volume_callback_(vol);
                }
                // SWARM_ASSIGN: relay tells us to forward audio to child nodes (P2P mesh)
                else if (n >= 13 && memcmp(buf, "SWARM_ASSIGN:", 13) == 0) {
                    std::string payload((const char*)buf + 13, n - 13);
                    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
                        payload.pop_back();
                    auto colon = payload.rfind(':');
                    if (colon != std::string::npos && colon > 0) {
                        std::string ip = payload.substr(0, colon);
                        uint16_t cport = (uint16_t)atoi(payload.substr(colon + 1).c_str());
                        sockaddr_in child{};
                        child.sin_family = AF_INET;
                        child.sin_port = htons(cport);
                        inet_pton(AF_INET, ip.c_str(), &child.sin_addr);
                        std::lock_guard<std::mutex> lk(children_mutex_);
                        if (swarm_children_.size() < 4) {
                            swarm_children_.push_back(child);
                            fprintf(stderr, "[swarm] Assigned child: %s:%u (total: %zu)\n", ip.c_str(), cport, swarm_children_.size());
                        }
                    }
                }
                // NTP sync pong (PT=125, first byte 0x7D)
                else if (n >= 25 && buf[0] == 0x7D) {
                    handle_sync_pong(buf, (size_t)n);
                }
                // RTP/OSTP audio packet from relay or peer
                else if (n >= 12 && (buf[0] & 0xC0) == 0x80) {
                    // Forward to swarm children (P2P mesh)
                    if (!swarm_children_.empty()) {
                        std::lock_guard<std::mutex> lk(children_mutex_);
                        for (const auto& child : swarm_children_) {
                            ::sendto(udp_sock_, buf, n, 0, (const sockaddr*)&child, sizeof(child));
                        }
                    }
                    total_rx++;
                    std::lock_guard<std::mutex> lock(cb_mutex_);
                    if (rx_callback_) {
                        rx_callback_(buf, static_cast<size_t>(n));
                    }
                }
            }

            // Log recv stats every 5 seconds
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log).count() >= 5) {
                fprintf(stderr, "[recv_loop] alive: %llu audio pkts, running=%d\n", total_rx, running_.load());
                last_log = now;
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_hello).count();
            if (elapsed >= 5) {
                const char* hello = "HELLO\n";
                sendto(udp_sock_, hello, strlen(hello), 0,
                       reinterpret_cast<const sockaddr*>(&relay_addr_), sizeof(relay_addr_));
                send_sync_ping();
                last_hello = now;
            }
        }
    }

    void send_sync_ping() {
        if (udp_sock_ < 0 || state_.load() != State::Connected) return;
        uint8_t pkt[25] = {};
        pkt[0] = 0x7D;
        struct timespec now_ts;
        clock_gettime(CLOCK_REALTIME, &now_ts);
        uint64_t t1_ns = (uint64_t)now_ts.tv_sec * 1000000000ULL + (uint64_t)now_ts.tv_nsec;
        memcpy(pkt + 1, &t1_ns, 8);
        sendto(udp_sock_, pkt, 25, 0,
               reinterpret_cast<const sockaddr*>(&relay_addr_), sizeof(relay_addr_));
    }

    void handle_sync_pong(const uint8_t* data, size_t len) {
        if (len < 25 || data[0] != 0x7D) return;
        uint64_t t1_ns, t2_ns, t3_ns;
        memcpy(&t1_ns, data + 1, 8);
        memcpy(&t2_ns, data + 9, 8);
        memcpy(&t3_ns, data + 17, 8);
        struct timespec now_ts;
        clock_gettime(CLOCK_REALTIME, &now_ts);
        uint64_t t4_ns = (uint64_t)now_ts.tv_sec * 1000000000ULL + (uint64_t)now_ts.tv_nsec;
        if (t2_ns == 0 || t3_ns == 0) return;
        int64_t offset = ((int64_t)(t2_ns - t1_ns) + (int64_t)(t3_ns - t4_ns)) / 2;
        int64_t rtt = (int64_t)(t4_ns - t1_ns) - (int64_t)(t3_ns - t2_ns);
        if (rtt < 0 || rtt > 500000000LL) return;
        int64_t prev = clock_offset_ns_.load(std::memory_order_relaxed);
        double alpha = (sync_ping_count_ < 5) ? 0.3 : 0.1;
        if (sync_ping_count_ < 5) sync_ping_count_++;
        int64_t smoothed = (int64_t)(prev * (1.0 - alpha) + offset * alpha);
        if (sync_ping_count_ == 1) smoothed = offset;
        clock_offset_ns_.store(smoothed, std::memory_order_relaxed);
        if (sync_ping_count_ <= 3)
            fprintf(stderr, "[clock-sync] offset=%.2fms rtt=%.2fms (#%u)\n",
                    offset / 1e6, rtt / 1e6, sync_ping_count_);
    }

    void add_peer(const sockaddr_in& peer) {
        std::lock_guard<std::mutex> lk(peers_mutex_);
        for (const auto& p : peers_) {
            if (p.sin_addr.s_addr == peer.sin_addr.s_addr && p.sin_port == peer.sin_port)
                return;
        }
        peers_.push_back(peer);
    }

    std::atomic<State> state_{State::Disconnected};
    std::atomic<bool> running_{false};
    int udp_sock_ = -1;
    sockaddr_in relay_addr_{};
    std::string group_;
    std::string error_;
    std::string device_id_;
    std::thread recv_thread_;
    std::mutex cb_mutex_;
    RxCallback rx_callback_;
    MetaCallback meta_callback_;
    FileCallback file_callback_;
    SyncCallback sync_callback_;
    std::mutex peers_mutex_;
    std::vector<sockaddr_in> peers_;
    std::mutex children_mutex_;
    std::vector<sockaddr_in> swarm_children_;  // P2P mesh: forward audio to child nodes
    MaxDelayCallback maxdelay_callback_;
    VolumeCallback volume_callback_;
    MembersCallback members_callback_;
    std::atomic<int64_t> clock_offset_ns_{0};
    uint32_t sync_ping_count_ = 0;
};

/// Internal microphone transmitter implementation
class TransmitterImpl {
public:
    TransmitterImpl(const std::string& dest_ip, uint16_t dest_port, uint32_t channels)
        : dest_ip_(dest_ip)
        , dest_port_(dest_port)
        , channels_(channels)
        , running_(false)
        , packets_sent_(0)
        , ring_buffer_(kDefaultSampleRate, channels * sizeof(int32_t))
        , ssrc_(arc4random())
    {}

    ~TransmitterImpl() { stop(); }

    bool start() {
        if (running_.load()) return false;

        audio_device_ = pal::AudioDevice::create();
        if (!audio_device_) return false;

        pal::AudioStreamConfig audio_cfg;
        audio_cfg.sample_rate = kDefaultSampleRate;
        audio_cfg.channels = 1;  // Mono mic
        audio_cfg.frames_per_buffer = 240;
        audio_cfg.format = SampleFormat::S24_LE;

        if (!audio_device_->open_input("", audio_cfg)) {
            fprintf(stderr, "[SolunaTx] Failed to open mic input\n");
            audio_device_.reset();
            return false;
        }

        socket_ = pal::UdpSocket::create();
        if (!socket_) {
            audio_device_.reset();
            return false;
        }

        running_.store(true);
        ring_buffer_.reset();
        packets_sent_.store(0);

        // Oversized to handle OS delivering more frames than requested
        conv_buf_.resize(4096 * channels_);

        audio_device_->start([this](float* buffer, uint32_t frame_count) {
            mic_callback(buffer, frame_count);
        });

        tx_thread_ = std::thread([this]() { tx_loop(); });

        return true;
    }

    void stop() {
        if (!running_.load()) return;
        running_.store(false);

        if (audio_device_) {
            audio_device_->stop();
            audio_device_->close();
            audio_device_.reset();
        }

        if (tx_thread_.joinable()) {
            tx_thread_.join();
        }

        socket_.reset();
        ring_buffer_.reset();
    }

    bool is_running() const { return running_.load(); }
    uint64_t packets_sent() const { return packets_sent_.load(); }
    float peak_level() const { return peak_level_.load(std::memory_order_relaxed); }

    /// Callback invoked with each built OSTP packet (for WAN relay forwarding)
    std::function<void(const uint8_t*, size_t)> tx_relay_callback;

private:
    void mic_callback(float* buffer, uint32_t frame_count) {
        // Track peak level for UI meter
        float peak = 0.0f;
        for (uint32_t i = 0; i < frame_count; i++) {
            float abs_val = std::fabs(buffer[i]);
            if (abs_val > peak) peak = abs_val;
        }
        float prev = peak_level_.load(std::memory_order_relaxed);
        if (peak > prev) {
            peak_level_.store(peak, std::memory_order_relaxed);
        } else {
            peak_level_.store(prev * 0.85f, std::memory_order_relaxed);
        }

        size_t out_idx = 0;
        for (uint32_t i = 0; i < frame_count; i++) {
            int32_t sample = static_cast<int32_t>(buffer[i] * 8388607.0f);
            for (uint32_t ch = 0; ch < channels_; ch++) {
                conv_buf_[out_idx++] = sample;
            }
        }
        ring_buffer_.write(conv_buf_.data(), frame_count);
    }

    void tx_loop() {
        constexpr uint32_t kFramesPerPacket = 240;
        const size_t frame_size = channels_ * sizeof(int32_t);

        transport::PacketScheduler scheduler(PacketTier::LAN, kDefaultSampleRate);
        scheduler.reset();

        std::vector<int32_t> audio_buf(kFramesPerPacket * channels_);
        std::vector<uint8_t> packet_buf(transport::kMaxPacketSize);

        pal::SocketAddress dest;
        dest.ip = dest_ip_;
        dest.port = dest_port_;

        uint32_t sequence = 0;
        uint32_t rtp_timestamp = 0;

        while (running_.load()) {
            scheduler.wait_next();

            if (ring_buffer_.available_read() < kFramesPerPacket) {
                std::memset(audio_buf.data(), 0, kFramesPerPacket * channels_ * sizeof(int32_t));
            } else {
                ring_buffer_.read(audio_buf.data(), kFramesPerPacket);
            }

            uint16_t seq_lo = static_cast<uint16_t>(sequence & 0xFFFF);
            uint16_t seq_hi = static_cast<uint16_t>((sequence >> 16) & 0xFFFF);

            // Wall-clock nanoseconds (lower 32 bits) for cross-device sync
            struct timespec wall_ts;
            clock_gettime(CLOCK_REALTIME, &wall_ts);
            uint32_t media_ts = static_cast<uint32_t>(
                (static_cast<uint64_t>(wall_ts.tv_sec) * 1'000'000'000ULL + wall_ts.tv_nsec)
                & 0xFFFFFFFF);

            size_t pkt_size = transport::ostp_build_packet(
                packet_buf.data(), packet_buf.size(),
                ssrc_, seq_lo, rtp_timestamp,
                96,  // kPayloadTypePCM24
                1,   // stream_id
                seq_hi,
                media_ts,
                audio_buf.data(),
                kFramesPerPacket * frame_size
            );

            if (pkt_size > 0) {
                socket_->send_to(packet_buf.data(), pkt_size, dest);
                packets_sent_.fetch_add(1);
                // Forward to WAN relay if connected
                if (tx_relay_callback) {
                    tx_relay_callback(packet_buf.data(), pkt_size);
                }
            }

            sequence++;
            rtp_timestamp += kFramesPerPacket;
        }
    }

    std::string dest_ip_;
    uint16_t dest_port_;
    uint32_t channels_;
    std::atomic<bool> running_;
    std::atomic<uint64_t> packets_sent_;
    std::atomic<float> peak_level_{0.0f};

    pipeline::RingBuffer ring_buffer_;
    uint32_t ssrc_;

    std::unique_ptr<pal::AudioDevice> audio_device_;
    std::unique_ptr<pal::UdpSocket> socket_;
    std::vector<int32_t> conv_buf_;
    std::thread tx_thread_;
};

/// System audio transmitter — reads from Soluna.driver shared memory and sends OSTP
class ShmTransmitter {
public:
    ShmTransmitter(const std::string& dest_ip, uint16_t dest_port, uint32_t channels)
        : dest_ip_(dest_ip)
        , dest_port_(dest_port)
        , channels_(channels)
        , running_(false)
        , packets_sent_(0)
        , ssrc_(arc4random())
    {
        shm_map_.hdr = nullptr;
        shm_map_.ring = nullptr;
    }

    ~ShmTransmitter() { stop(); }

    bool start() {
        if (running_.load()) return false;

        // Open existing SHM (created by solunad or install.sh)
        if (soluna_shm_open(&shm_map_, O_RDWR) != 0) {
            fprintf(stderr, "[ShmTx] Cannot open SHM %s: %s\n",
                    soluna_shm_path(), strerror(errno));
            // Try creating it
            if (soluna_shm_open(&shm_map_, O_RDWR | O_CREAT) != 0) {
                fprintf(stderr, "[ShmTx] Cannot create SHM either\n");
                return false;
            }
            soluna_shm_init_header(&shm_map_);
        }
        if (soluna_shm_validate(&shm_map_) != 0) {
            fprintf(stderr, "[ShmTx] SHM validation failed\n");
            soluna_shm_close(&shm_map_);
            return false;
        }

        socket_ = pal::UdpSocket::create();
        if (!socket_) {
            soluna_shm_close(&shm_map_);
            return false;
        }

        running_.store(true);
        packets_sent_.store(0);
        tx_thread_ = std::thread([this]() { tx_loop(); });

        fprintf(stderr, "[ShmTx] Started (SHM → OSTP multicast %s:%u)\n",
                dest_ip_.c_str(), dest_port_);
        return true;
    }

    void stop() {
        if (!running_.load()) return;
        running_.store(false);
        if (tx_thread_.joinable()) tx_thread_.join();
        socket_.reset();
        if (shm_map_.hdr) soluna_shm_close(&shm_map_);
        fprintf(stderr, "[ShmTx] Stopped\n");
    }

    bool is_running() const { return running_.load(); }
    uint64_t packets_sent() const { return packets_sent_.load(); }
    float peak_level() const { return peak_level_.load(std::memory_order_relaxed); }

    /// Callback for WAN relay forwarding
    std::function<void(const uint8_t*, size_t)> tx_relay_callback;

private:
    void tx_loop() {
        constexpr uint32_t kFramesPerPacket = 240;
        const size_t frame_size = channels_ * sizeof(int32_t);

        transport::PacketScheduler scheduler(PacketTier::LAN, kDefaultSampleRate);
        scheduler.reset();

        std::vector<float>   flt_buf(kFramesPerPacket * channels_);
        std::vector<int32_t> s24_buf(kFramesPerPacket * channels_);
        std::vector<uint8_t> packet_buf(transport::kMaxPacketSize);

        pal::SocketAddress dest;
        dest.ip = dest_ip_;
        dest.port = dest_port_;

        uint32_t sequence = 0;
        uint32_t rtp_timestamp = 0;

        while (running_.load()) {
            scheduler.wait_next();

            // Read from SHM
            uint32_t avail = (uint32_t)soluna_shm_available_read(&shm_map_);
            if (avail < kFramesPerPacket) {
                // Not enough data — send silence
                std::memset(s24_buf.data(), 0, kFramesPerPacket * frame_size);
            } else {
                soluna_shm_read(&shm_map_, flt_buf.data(), kFramesPerPacket);

                // Track peak level
                float peak = 0.0f;
                for (uint32_t i = 0; i < kFramesPerPacket * channels_; i++) {
                    float a = std::fabs(flt_buf[i]);
                    if (a > peak) peak = a;
                }
                float prev = peak_level_.load(std::memory_order_relaxed);
                peak_level_.store(peak > prev ? peak : prev * 0.85f, std::memory_order_relaxed);

                // float32 → S24
                for (uint32_t i = 0; i < kFramesPerPacket * channels_; i++) {
                    float v = flt_buf[i];
                    if (v > 1.0f) v = 1.0f;
                    if (v < -1.0f) v = -1.0f;
                    s24_buf[i] = static_cast<int32_t>(v * 8388607.0f);
                }
            }

            // Build OSTP packet
            uint16_t seq_lo = static_cast<uint16_t>(sequence & 0xFFFF);
            uint16_t seq_hi = static_cast<uint16_t>((sequence >> 16) & 0xFFFF);

            struct timespec wall_ts;
            clock_gettime(CLOCK_REALTIME, &wall_ts);
            uint32_t media_ts = static_cast<uint32_t>(
                (static_cast<uint64_t>(wall_ts.tv_sec) * 1'000'000'000ULL + wall_ts.tv_nsec)
                & 0xFFFFFFFF);

            size_t pkt_size = transport::ostp_build_packet(
                packet_buf.data(), packet_buf.size(),
                ssrc_, seq_lo, rtp_timestamp,
                96, 1, seq_hi, media_ts,
                s24_buf.data(), kFramesPerPacket * frame_size
            );

            if (pkt_size > 0) {
                socket_->send_to(packet_buf.data(), pkt_size, dest);
                packets_sent_.fetch_add(1);
                if (tx_relay_callback) {
                    tx_relay_callback(packet_buf.data(), pkt_size);
                }
            }

            sequence++;
            rtp_timestamp += kFramesPerPacket;
        }
    }

    std::string dest_ip_;
    uint16_t dest_port_;
    uint32_t channels_;
    std::atomic<bool> running_;
    std::atomic<uint64_t> packets_sent_;
    std::atomic<float> peak_level_{0.0f};
    uint32_t ssrc_;

    SolunaShmMap shm_map_;
    std::unique_ptr<pal::UdpSocket> socket_;
    std::thread tx_thread_;
};

// ============================================================================
// DJBroadcaster — decode audio files and stream via OSTP to relay
// ============================================================================

class DJBroadcaster {
public:
    DJBroadcaster() = default;
    ~DJBroadcaster() { stop_mic_capture(); stop(); }

    // ── Mic mixing ──────────────────────────────────────────────────────
    std::atomic<bool> mic_mix_enabled_{false};
    std::atomic<float> mic_gain_{1.0f};
    std::atomic<float> music_gain_{0.7f};

    void set_mic_mix(bool enabled) {
        if (enabled && !mic_queue_) {
            start_mic_capture();
        } else if (!enabled && mic_queue_) {
            stop_mic_capture();
        }
        mic_mix_enabled_ = enabled;
    }

    bool start_file(const std::string& filepath) {
        if (running_.load()) return false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            playlist_.clear();
            playlist_.push_back(filepath);
            playlist_index_ = 0;
        }
        running_.store(true);
        skip_flag_.store(false);
        thread_ = std::thread([this]() { broadcast_loop(); });
        return true;
    }

    bool start_directory(const std::string& dirpath, bool shuffle) {
        if (running_.load()) return false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            playlist_.clear();

            // Scan directory for audio files using NSFileManager
            @autoreleasepool {
                NSArray* exts = @[@"mp3", @"m4a", @"wav", @"aac", @"flac", @"aiff", @"alac"];
                NSArray* contents = [[NSFileManager defaultManager]
                    contentsOfDirectoryAtPath:@(dirpath.c_str()) error:nil];
                for (NSString* name in contents) {
                    if ([exts containsObject:name.pathExtension.lowercaseString]) {
                        playlist_.push_back(dirpath + "/" + [name UTF8String]);
                    }
                }
            }

            if (playlist_.empty()) {
                fprintf(stderr, "[DJ] No audio files found in %s\n", dirpath.c_str());
                return false;
            }

            if (shuffle) {
                std::shuffle(playlist_.begin(), playlist_.end(),
                            std::mt19937{std::random_device{}()});
            }
            playlist_index_ = 0;
        }
        running_.store(true);
        skip_flag_.store(false);
        thread_ = std::thread([this]() { broadcast_loop(); });
        return true;
    }

    void stop() {
        if (!running_.load()) return;
        running_.store(false);
        if (thread_.joinable()) thread_.join();
        {
            std::lock_guard<std::mutex> lk(mutex_);
            current_track_.clear();
        }
        progress_.store(0.0f);
        fprintf(stderr, "[DJ] Stopped\n");
    }

    void skip() {
        skip_flag_.store(true);
    }

    bool is_active() const { return running_.load(); }

    std::string current_track() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return current_track_;
    }

    float progress() const { return progress_.load(std::memory_order_relaxed); }

    /// Callback to send OSTP audio packets via relay
    std::function<void(const uint8_t*, size_t)> tx_relay_callback;

    /// Callback to send text commands (FILE:/META:/SYNC:) via relay
    std::function<void(const std::string&)> send_relay_cmd;

private:
    // ── Mic capture internals ───────────────────────────────────────────
    AudioQueueRef mic_queue_ = nullptr;
    std::vector<float> mic_ring_;  // stereo ring buffer
    std::atomic<size_t> mic_write_pos_{0};
    size_t mic_read_pos_ = 0;
    static constexpr size_t kMicRingSize = 48000;  // 1 second @ 48kHz

    void start_mic_capture() {
        mic_ring_.assign(kMicRingSize * 2, 0.0f);  // stereo
        mic_write_pos_.store(0, std::memory_order_relaxed);
        mic_read_pos_ = 0;

        AudioStreamBasicDescription fmt{};
        fmt.mSampleRate = 48000;
        fmt.mFormatID = kAudioFormatLinearPCM;
        fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        fmt.mBytesPerPacket = 4;
        fmt.mFramesPerPacket = 1;
        fmt.mBytesPerFrame = 4;
        fmt.mChannelsPerFrame = 1;  // mono mic
        fmt.mBitsPerChannel = 32;

        OSStatus st = AudioQueueNewInput(&fmt, mic_aq_callback, this,
                                         nullptr, nullptr, 0, &mic_queue_);
        if (st != noErr) {
            fprintf(stderr, "[DJ-Mic] AudioQueueNewInput failed: %d\n", (int)st);
            mic_queue_ = nullptr;
            return;
        }

        // 3 buffers of 480 frames (10ms) each
        for (int i = 0; i < 3; i++) {
            AudioQueueBufferRef buf;
            AudioQueueAllocateBuffer(mic_queue_, 480 * 4, &buf);
            AudioQueueEnqueueBuffer(mic_queue_, buf, 0, nullptr);
        }
        st = AudioQueueStart(mic_queue_, nullptr);
        if (st != noErr) {
            fprintf(stderr, "[DJ-Mic] AudioQueueStart failed: %d\n", (int)st);
            AudioQueueDispose(mic_queue_, true);
            mic_queue_ = nullptr;
            return;
        }
        fprintf(stderr, "[DJ-Mic] Mic capture started\n");
    }

    void stop_mic_capture() {
        if (mic_queue_) {
            AudioQueueStop(mic_queue_, true);
            AudioQueueDispose(mic_queue_, true);
            mic_queue_ = nullptr;
            fprintf(stderr, "[DJ-Mic] Mic capture stopped\n");
        }
    }

    static void mic_aq_callback(void* ctx, AudioQueueRef queue,
                                AudioQueueBufferRef buf,
                                const AudioTimeStamp*, UInt32,
                                const AudioStreamPacketDescription*) {
        auto* self = static_cast<DJBroadcaster*>(ctx);
        const float* data = (const float*)buf->mAudioData;
        size_t frames = buf->mAudioDataByteSize / sizeof(float);
        size_t wr = self->mic_write_pos_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < frames; i++) {
            size_t idx = (wr + i) % self->kMicRingSize;
            self->mic_ring_[idx * 2]     = data[i];  // L = mono
            self->mic_ring_[idx * 2 + 1] = data[i];  // R = mono
        }
        self->mic_write_pos_.store(wr + frames, std::memory_order_release);
        AudioQueueEnqueueBuffer(queue, buf, 0, nullptr);
    }

    /// Mix mic samples into interleaved S24 payload buffer (in-place)
    void mix_mic_into_payload(int32_t* payload, AVAudioFrameCount frames) {
        if (!mic_mix_enabled_.load(std::memory_order_relaxed)) return;

        float mg  = mic_gain_.load(std::memory_order_relaxed);
        float mug = music_gain_.load(std::memory_order_relaxed);
        size_t mic_avail = mic_write_pos_.load(std::memory_order_acquire) - mic_read_pos_;
        size_t mic_frames = std::min(mic_avail, (size_t)frames);

        for (AVAudioFrameCount i = 0; i < frames; i++) {
            for (int ch = 0; ch < 2; ch++) {
                float music = (float)payload[i * 2 + ch] / 8388607.0f * mug;
                float mic = 0.0f;
                if (i < mic_frames) {
                    size_t idx = (mic_read_pos_ + i) % kMicRingSize;
                    mic = mic_ring_[idx * 2 + ch] * mg;
                }
                float mixed = music + mic;
                if (mixed > 1.0f) mixed = 1.0f;
                if (mixed < -1.0f) mixed = -1.0f;
                payload[i * 2 + ch] = (int32_t)(mixed * 8388607.0f);
            }
        }
        mic_read_pos_ += mic_frames;
    }

    void broadcast_loop() {
        while (running_.load()) {
            std::string filepath;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                if (playlist_index_ >= playlist_.size()) break;
                filepath = playlist_[playlist_index_];
                playlist_index_++;
            }

            broadcast_file(filepath);

            if (!running_.load()) break;
        }
        running_.store(false);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            current_track_.clear();
        }
        progress_.store(0.0f);
        fprintf(stderr, "[DJ] Playlist finished\n");
    }

    void broadcast_file(const std::string& filepath) {
        @autoreleasepool {
            // Extract filename for display/relay commands
            NSString* nsPath = @(filepath.c_str());
            NSString* filename = [nsPath lastPathComponent];
            {
                std::lock_guard<std::mutex> lk(mutex_);
                current_track_ = [filename UTF8String];
            }
            progress_.store(0.0f);

            fprintf(stderr, "[DJ] Broadcasting: %s\n", filepath.c_str());

            // Send FILE: command to relay
            if (send_relay_cmd) {
                std::string cmd = "FILE:" + std::string([filename UTF8String]) + "\n";
                send_relay_cmd(cmd);
            }

            // Open audio file with AVAudioFile
            NSError* error = nil;
            NSURL* url = [NSURL fileURLWithPath:nsPath];
            AVAudioFile* file = [[AVAudioFile alloc] initForReading:url error:&error];
            if (!file || error) {
                fprintf(stderr, "[DJ] Failed to open %s: %s\n",
                        filepath.c_str(),
                        error ? [[error localizedDescription] UTF8String] : "unknown");
                return;
            }

            // Set up processing format: 48kHz stereo float32
            AVAudioFormat* procFormat = [[AVAudioFormat alloc]
                initWithCommonFormat:AVAudioPCMFormatFloat32
                          sampleRate:48000
                            channels:2
                         interleaved:NO];

            AVAudioFrameCount totalFrames = (AVAudioFrameCount)file.length;
            double srcRate = file.processingFormat.sampleRate;
            // Total frames in output rate
            AVAudioFrameCount totalOutFrames = (AVAudioFrameCount)(
                (double)totalFrames * 48000.0 / srcRate);

            // Read in chunks of 480 frames (10ms at 48kHz) for OSTP packets
            constexpr AVAudioFrameCount kChunkFrames = 480;
            AVAudioPCMBuffer* readBuf = [[AVAudioPCMBuffer alloc]
                initWithPCMFormat:file.processingFormat
                    frameCapacity:kChunkFrames * 4];

            // Converter for sample rate / format conversion
            AVAudioConverter* converter = nil;
            AVAudioPCMBuffer* convBuf = nil;
            if (file.processingFormat.sampleRate != 48000 ||
                file.processingFormat.channelCount != 2) {
                converter = [[AVAudioConverter alloc]
                    initFromFormat:file.processingFormat
                         toFormat:procFormat];
                convBuf = [[AVAudioPCMBuffer alloc]
                    initWithPCMFormat:procFormat
                        frameCapacity:kChunkFrames * 4];
            }

            // OSTP packet state
            std::vector<int32_t> s24_buf(kChunkFrames * 2);
            std::vector<uint8_t> packet_buf(transport::kMaxPacketSize);
            uint32_t rtp_ts = 0;

            // Pacing: 10ms per 480-frame chunk at 48kHz
            auto next_send = std::chrono::steady_clock::now();
            const auto chunk_duration = std::chrono::microseconds(10000); // 10ms

            AVAudioFrameCount framesRead = 0;
            skip_flag_.store(false);

            // Send META with track info
            if (send_relay_cmd) {
                std::string meta = "META:{\"track\":\"" +
                    std::string([filename UTF8String]) + "\"}\n";
                send_relay_cmd(meta);
            }

            while (running_.load() && !skip_flag_.load()) {
                // Read a chunk from the file
                readBuf.frameLength = 0;
                BOOL ok = [file readIntoBuffer:readBuf frameCount:kChunkFrames error:&error];
                if (!ok || readBuf.frameLength == 0) break; // EOF or error

                // Get float samples (converting if needed)
                const float* left = nullptr;
                const float* right = nullptr;
                AVAudioFrameCount frames = 0;

                if (converter) {
                    convBuf.frameLength = 0;
                    [converter convertToBuffer:convBuf
                                         error:&error
                            withInputFromBlock:^AVAudioBuffer*(AVAudioPacketCount inCount,
                                                              AVAudioConverterInputStatus* outStatus) {
                        *outStatus = AVAudioConverterInputStatus_HaveData;
                        return readBuf;
                    }];
                    frames = convBuf.frameLength;
                    left = convBuf.floatChannelData[0];
                    right = (convBuf.format.channelCount >= 2) ?
                            convBuf.floatChannelData[1] : convBuf.floatChannelData[0];
                } else {
                    frames = readBuf.frameLength;
                    left = readBuf.floatChannelData[0];
                    right = (readBuf.format.channelCount >= 2) ?
                            readBuf.floatChannelData[1] : readBuf.floatChannelData[0];
                }

                if (frames == 0) break;

                // Convert float32 non-interleaved → interleaved int32 S24
                for (AVAudioFrameCount i = 0; i < frames; i++) {
                    float lv = left[i];
                    float rv = right[i];
                    if (lv > 1.0f) lv = 1.0f;
                    if (lv < -1.0f) lv = -1.0f;
                    if (rv > 1.0f) rv = 1.0f;
                    if (rv < -1.0f) rv = -1.0f;
                    s24_buf[i * 2]     = static_cast<int32_t>(lv * 8388607.0f);
                    s24_buf[i * 2 + 1] = static_cast<int32_t>(rv * 8388607.0f);
                }

                // Mix mic audio into music if enabled
                mix_mic_into_payload(s24_buf.data(), frames);

                // Build OSTP packet
                uint16_t seq_lo = static_cast<uint16_t>(seq_ & 0xFFFF);
                uint16_t seq_hi = static_cast<uint16_t>((seq_ >> 16) & 0xFFFF);

                struct timespec wall_ts;
                clock_gettime(CLOCK_REALTIME, &wall_ts);
                uint32_t media_ts = static_cast<uint32_t>(
                    (static_cast<uint64_t>(wall_ts.tv_sec) * 1'000'000'000ULL +
                     wall_ts.tv_nsec) & 0xFFFFFFFF);

                size_t pkt_size = transport::ostp_build_packet(
                    packet_buf.data(), packet_buf.size(),
                    ssrc_, seq_lo, rtp_ts,
                    96, 1, seq_hi, media_ts,
                    s24_buf.data(),
                    frames * 2 * sizeof(int32_t)
                );

                if (pkt_size > 0 && tx_relay_callback) {
                    tx_relay_callback(packet_buf.data(), pkt_size);
                }

                seq_++;
                rtp_ts += frames;
                framesRead += readBuf.frameLength;

                // Update progress
                if (totalOutFrames > 0) {
                    progress_.store(
                        std::min(1.0f, static_cast<float>(framesRead) /
                                       static_cast<float>(totalFrames)),
                        std::memory_order_relaxed);
                }

                // Pace in real-time
                next_send += chunk_duration;
                std::this_thread::sleep_until(next_send);
            }

            // Send SYNC: command when track ends
            if (send_relay_cmd) {
                std::string cmd = "SYNC:end\n";
                send_relay_cmd(cmd);
            }

            fprintf(stderr, "[DJ] Finished: %s (frames=%u)\n",
                    filepath.c_str(), (unsigned)framesRead);
        } // @autoreleasepool
    }

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> skip_flag_{false};
    mutable std::mutex mutex_;
    std::vector<std::string> playlist_;
    size_t playlist_index_ = 0;
    std::string current_track_;
    std::atomic<float> progress_{0.0f};

    // OSTP state
    uint32_t ssrc_ = 0x444A4D58; // 'DJMX'
    uint32_t seq_ = 0;
    uint32_t rtp_ts_ = 0;
};

// ── DJController — dual-deck with equal-power crossfader ──────────────────
class DJController {
public:
    explicit DJController(WanRelayClient* relay)
        : relay_(relay), crossfader_(0.5f), running_(false)
        , ssrc_(arc4random())
        , seq_(0), timestamp_(0)
    {}
    ~DJController() { stop(); }

    struct Deck {
        std::string filepath;
        std::string track_name;
        std::atomic<float> progress{0.0f};
        std::atomic<bool> playing{false};
        std::atomic<bool> pause{false};

        ExtAudioFileRef ext_file = nullptr;
        int64_t total_frames = 0;
        int64_t played_frames = 0;

        bool open(const std::string& path) {
            CFURLRef url = CFURLCreateFromFileSystemRepresentation(
                nullptr, (const UInt8*)path.c_str(), path.size(), false);
            OSStatus err = ExtAudioFileOpenURL(url, &ext_file);
            CFRelease(url);
            if (err != noErr || !ext_file) return false;

            AudioStreamBasicDescription out_fmt{};
            out_fmt.mSampleRate = 48000;
            out_fmt.mFormatID = kAudioFormatLinearPCM;
            out_fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
            out_fmt.mBytesPerPacket = 8;
            out_fmt.mFramesPerPacket = 1;
            out_fmt.mBytesPerFrame = 8;
            out_fmt.mChannelsPerFrame = 2;
            out_fmt.mBitsPerChannel = 32;
            ExtAudioFileSetProperty(ext_file, kExtAudioFileProperty_ClientDataFormat,
                                    sizeof(out_fmt), &out_fmt);

            int64_t file_frames = 0;
            UInt32 prop_size = sizeof(file_frames);
            ExtAudioFileGetProperty(ext_file, kExtAudioFileProperty_FileLengthFrames,
                                    &prop_size, &file_frames);
            total_frames = file_frames;
            played_frames = 0;
            return true;
        }

        int64_t read_frames(float* buf, int64_t frame_count) {
            if (!ext_file || pause.load()) return 0;
            AudioBufferList abl{};
            abl.mNumberBuffers = 1;
            abl.mBuffers[0].mNumberChannels = 2;
            abl.mBuffers[0].mDataByteSize = (UInt32)(frame_count * 8);
            abl.mBuffers[0].mData = buf;
            UInt32 frames = (UInt32)frame_count;
            ExtAudioFileRead(ext_file, &frames, &abl);
            played_frames += frames;
            if (total_frames > 0) {
                progress.store((float)played_frames / (float)total_frames,
                               std::memory_order_relaxed);
            }
            return frames;
        }

        bool is_done() const {
            return total_frames > 0 && played_frames >= total_frames;
        }

        void close() {
            if (ext_file) { ExtAudioFileDispose(ext_file); ext_file = nullptr; }
            playing.store(false);
        }
    };

    bool start_deck_a(const std::string& path) {
        deck_a_.close();
        if (!deck_a_.open(path)) return false;
        auto slash = path.rfind('/');
        deck_a_.track_name = (slash != std::string::npos) ? path.substr(slash+1) : path;
        deck_a_.playing.store(true);
        deck_a_.pause.store(false);
        if (!running_.load()) start_mix_thread();
        return true;
    }

    bool start_deck_b(const std::string& path) {
        deck_b_.close();
        if (!deck_b_.open(path)) return false;
        auto slash = path.rfind('/');
        deck_b_.track_name = (slash != std::string::npos) ? path.substr(slash+1) : path;
        deck_b_.playing.store(true);
        deck_b_.pause.store(false);
        if (!running_.load()) start_mix_thread();
        return true;
    }

    void toggle_deck_a() { deck_a_.pause.store(!deck_a_.pause.load()); }
    void toggle_deck_b() { deck_b_.pause.store(!deck_b_.pause.load()); }

    void set_crossfader(float v) { crossfader_.store(std::max(0.f, std::min(1.f, v))); }
    float get_crossfader() const { return crossfader_.load(); }

    float deck_a_progress() const { return deck_a_.progress.load(); }
    float deck_b_progress() const { return deck_b_.progress.load(); }
    bool deck_a_playing() const { return deck_a_.playing.load() && !deck_a_.pause.load(); }
    bool deck_b_playing() const { return deck_b_.playing.load() && !deck_b_.pause.load(); }
    std::string deck_a_track() const { return deck_a_.track_name; }
    std::string deck_b_track() const { return deck_b_.track_name; }
    bool is_active() const { return running_.load(); }

    void stop() {
        running_.store(false);
        if (mix_thread_.joinable()) mix_thread_.join();
        deck_a_.close();
        deck_b_.close();
    }

private:
    WanRelayClient* relay_;
    std::atomic<float> crossfader_;
    std::atomic<bool> running_;
    uint32_t ssrc_, seq_;
    uint32_t timestamp_;
    Deck deck_a_, deck_b_;
    std::thread mix_thread_;

    void start_mix_thread() {
        running_.store(true);
        mix_thread_ = std::thread([this]() { mix_loop(); });
    }

    void mix_loop() {
        static constexpr int kFrames = 960;
        static constexpr int kCh = 2;
        static constexpr double kInterval = (double)kFrames / 48000.0;

        std::vector<float> buf_a(kFrames * kCh, 0.f);
        std::vector<float> buf_b(kFrames * kCh, 0.f);
        std::vector<int16_t> mix_out(kFrames * kCh, 0);

        auto next_time = std::chrono::steady_clock::now();

        while (running_.load()) {
            next_time += std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(kInterval));

            std::fill(buf_a.begin(), buf_a.end(), 0.f);
            std::fill(buf_b.begin(), buf_b.end(), 0.f);

            if (deck_a_.playing.load()) {
                deck_a_.read_frames(buf_a.data(), kFrames);
                if (deck_a_.is_done()) { deck_a_.close(); }
            }
            if (deck_b_.playing.load()) {
                deck_b_.read_frames(buf_b.data(), kFrames);
                if (deck_b_.is_done()) { deck_b_.close(); }
            }

            float cf = crossfader_.load();
            float gain_a = cosf(cf * (float)M_PI_2);
            float gain_b = sinf(cf * (float)M_PI_2);

            for (int i = 0; i < kFrames * kCh; i++) {
                float s = buf_a[i] * gain_a + buf_b[i] * gain_b;
                s = std::max(-1.f, std::min(1.f, s));
                mix_out[i] = (int16_t)(s * 32767.f);
            }

            if (deck_a_.playing.load() || deck_b_.playing.load()) {
                send_ostp(mix_out.data(), kFrames * kCh);
            }

            std::this_thread::sleep_until(next_time);
        }
    }

    void send_ostp(const int16_t* pcm, int sample_count) {
        if (!relay_) return;

        const int kHeaderSize = 24;
        const int payload_bytes = sample_count * (int)sizeof(int16_t);
        const int total = kHeaderSize + payload_bytes + 4;

        std::vector<uint8_t> pkt(total, 0);

        pkt[0] = 0x90; pkt[1] = 96;
        pkt[2] = (seq_ >> 8) & 0xFF; pkt[3] = seq_ & 0xFF;
        pkt[4] = (timestamp_ >> 24) & 0xFF; pkt[5] = (timestamp_ >> 16) & 0xFF;
        pkt[6] = (timestamp_ >> 8)  & 0xFF; pkt[7] =  timestamp_ & 0xFF;
        pkt[8]  = (ssrc_ >> 24) & 0xFF; pkt[9]  = (ssrc_ >> 16) & 0xFF;
        pkt[10] = (ssrc_ >> 8)  & 0xFF; pkt[11] =  ssrc_ & 0xFF;

        pkt[12] = 0x4F; pkt[13] = 0x53;
        pkt[14] = 0x00; pkt[15] = 0x02;

        pkt[16] = 0x02; pkt[17] = 0x00;
        pkt[18] = 0x00; pkt[19] = 0x00;
        pkt[20] = (timestamp_ >> 24) & 0xFF; pkt[21] = (timestamp_ >> 16) & 0xFF;
        pkt[22] = (timestamp_ >> 8)  & 0xFF; pkt[23] =  timestamp_ & 0xFF;

        memcpy(pkt.data() + 24, pcm, payload_bytes);

        uint32_t crc = 0xFFFFFFFF;
        for (int i = 0; i < 24 + payload_bytes; i++) {
            crc ^= pkt[i];
            for (int j = 0; j < 8; j++) crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320u : 0u);
        }
        crc = ~crc;
        pkt[24 + payload_bytes + 0] = (crc >> 24) & 0xFF;
        pkt[24 + payload_bytes + 1] = (crc >> 16) & 0xFF;
        pkt[24 + payload_bytes + 2] = (crc >> 8)  & 0xFF;
        pkt[24 + payload_bytes + 3] =  crc & 0xFF;

        seq_++;
        timestamp_ += 960;

        relay_->send_audio(pkt.data(), pkt.size());
    }
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
        , target_fill_frames_(2880)  // 60ms default — matched with iOS for sync
        , ring_buffer_(192000, channels * sizeof(int32_t))  // 4s capacity — matched with iOS
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
            // Multicast socket init failed (sandbox, port in use, or no network).
            // Continue in relay-only mode: WAN relay injects audio via inject_raw_packet
            // which bypasses the socket entirely. Disable network receive loop to avoid
            // calling receive_packet() on the uninitialized socket.
            fprintf(stderr, "[SolunaRx] Multicast init failed — relay-only mode\n");
            set_relay_network_disabled(true);
        }

        // Wire up NTP sync: receiver adjusts target_fill_frames_ based on media_timestamp
        rtp_receiver_->sync_target_frames_ = &target_fill_frames_;
        rtp_receiver_->sync_playout_delay_ms_ = 80.0;  // match iOS for cross-device sync

        // Wire fan-out: replicate audio data to all extra output sinks
        rtp_receiver_->on_audio_written = [this](const void* data, size_t frames) {
            std::lock_guard<std::mutex> lock(sinks_mutex_);
            for (auto& sink : extra_sinks_) {
                sink->write(data, frames);
            }
        };

        // Create audio output directly (inline DefaultOutput — bypasses pal::AudioDevice)
        fprintf(stderr, "[SolunaRx] Opening audio output: %uHz, %uch (inline DefaultOutput)\n",
                kDefaultSampleRate, channels_);
        {
            AudioComponentDescription desc = {};
            desc.componentType = kAudioUnitType_Output;
            desc.componentSubType = kAudioUnitSubType_DefaultOutput;
            desc.componentManufacturer = kAudioUnitManufacturer_Apple;

            AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
            if (!comp) {
                fprintf(stderr, "[SolunaRx] DefaultOutput component not found\n");
                return false;
            }

            OSStatus st = AudioComponentInstanceNew(comp, &inline_audio_unit_);
            if (st != noErr) {
                fprintf(stderr, "[SolunaRx] Failed to create audio unit: %d\n", (int)st);
                return false;
            }

            AudioStreamBasicDescription fmt = {};
            fmt.mSampleRate = kDefaultSampleRate;
            fmt.mFormatID = kAudioFormatLinearPCM;
            fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
            fmt.mBytesPerPacket = sizeof(float) * channels_;
            fmt.mFramesPerPacket = 1;
            fmt.mBytesPerFrame = sizeof(float) * channels_;
            fmt.mChannelsPerFrame = channels_;
            fmt.mBitsPerChannel = 32;

            st = AudioUnitSetProperty(inline_audio_unit_,
                kAudioUnitProperty_StreamFormat,
                kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));
            if (st != noErr) {
                fprintf(stderr, "[SolunaRx] Failed to set format: %d\n", (int)st);
                AudioComponentInstanceDispose(inline_audio_unit_);
                inline_audio_unit_ = nullptr;
                return false;
            }

            AURenderCallbackStruct cbs = {};
            cbs.inputProc = inline_render_cb;
            cbs.inputProcRefCon = this;
            st = AudioUnitSetProperty(inline_audio_unit_,
                kAudioUnitProperty_SetRenderCallback,
                kAudioUnitScope_Input, 0, &cbs, sizeof(cbs));
            if (st != noErr) {
                fprintf(stderr, "[SolunaRx] Failed to set callback: %d\n", (int)st);
                AudioComponentInstanceDispose(inline_audio_unit_);
                inline_audio_unit_ = nullptr;
                return false;
            }

            st = AudioUnitInitialize(inline_audio_unit_);
            if (st != noErr) {
                fprintf(stderr, "[SolunaRx] Failed to init audio unit: %d\n", (int)st);
                AudioComponentInstanceDispose(inline_audio_unit_);
                inline_audio_unit_ = nullptr;
                return false;
            }
        }

        running_.store(true);

        // Start audio playback
        {
            OSStatus st = AudioOutputUnitStart(inline_audio_unit_);
            if (st != noErr) {
                fprintf(stderr, "[SolunaRx] Failed to start audio unit: %d\n", (int)st);
                running_.store(false);
                return false;
            }
            fprintf(stderr, "[SolunaRx] Inline audio unit started successfully\n");
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

        // Disconnect WAN relay
        wan_relay_disconnect();

        // Stop WebSocket server first (while message callback is still valid)
        ws_server_.stop();

        // Stop extra sinks
        {
            std::lock_guard<std::mutex> lock(sinks_mutex_);
            for (auto& sink : extra_sinks_) sink->stop();
            extra_sinks_.clear();
        }

        if (inline_audio_unit_) {
            AudioOutputUnitStop(inline_audio_unit_);
            AudioComponentInstanceDispose(inline_audio_unit_);
            inline_audio_unit_ = nullptr;
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

    // ── Sync mode ─────────────────────────────────────────────────────────
    void set_sync_mode(bool enabled) {
        sync_mode_.store(enabled);
        if (enabled) {
            // Set a generous buffer for sync (200ms)
            target_fill_frames_.store(sync_delay_ms_.load() * 48u);
        }
    }
    bool is_sync_mode() const { return sync_mode_.load(); }

    // ── Talk mode (multi-speaker) ──────────────────────────────────────────
    void set_talk_mode(bool enabled) { talk_mode_active_.store(enabled, std::memory_order_relaxed); }
    bool is_talk_mode() const { return talk_mode_active_.load(std::memory_order_relaxed); }

    // ── Loudness normalization (EBU R128) ─────────────────────────────────
    void set_loudness_norm(bool enabled) { loudness_norm_enabled_.store(enabled); }
    bool is_loudness_norm() const { return loudness_norm_enabled_.load(); }

    void set_sync_delay_ms(uint32_t ms) {
        ms = std::max(50u, std::min(1000u, ms));
        sync_delay_ms_.store(ms);
        if (sync_mode_.load()) {
            target_fill_frames_.store(ms * 48u);
        }
    }
    uint32_t sync_delay_ms() const { return sync_delay_ms_.load(); }

    SimpleRtpReceiver::Stats stats() const {
        if (rtp_receiver_) return rtp_receiver_->stats_snapshot();
        return {};
    }

    int device_health() const {
        return health_.load(std::memory_order_relaxed);
    }

    void set_relay_network_disabled(bool d) { relay_network_disabled_.store(d); }
    void set_filesync_network_disabled(bool d) { filesync_network_disabled_.store(d); }
    bool is_network_disabled() const {
        return relay_network_disabled_.load(std::memory_order_relaxed) ||
               filesync_network_disabled_.load(std::memory_order_relaxed);
    }

    // ── Relay support ──────────────────────────────────────────────────────

    void set_relay_callback(std::function<void(const uint8_t*, size_t)> cb) {
        relay_callback_ = std::move(cb);
        if (rtp_receiver_) rtp_receiver_->relay_callback = relay_callback_;
    }

    void inject_raw_packet(const uint8_t* data, size_t len) {
        if (!rtp_receiver_) return;
        // On first relay packet: flush stale data & boost buffer to 500ms for WAN jitter
        if (!relay_first_packet_received_.load(std::memory_order_relaxed)) {
            relay_first_packet_received_.store(true, std::memory_order_relaxed);
            flush_requested_.store(true, std::memory_order_release);
            target_fill_frames_.store(1440, std::memory_order_relaxed);  // 30ms prefill
            health_underruns_in_window_ = 0;
            last_underrun_ms_ = 0;
            fprintf(stderr, "[relay] First packet — target=1440 (30ms)\n");
        }
        relay_inject_count_++;
        rtp_receiver_->inject_raw_packet(data, len, ring_buffer_);

        // RTCP inter-arrival jitter (RFC 3550)
        if (len >= 8 && (data[0] & 0xC0) == 0x80) {
            uint32_t rtp_ts;
            std::memcpy(&rtp_ts, data + 4, 4);
            rtp_ts = ntohl(rtp_ts);
            struct timespec now_ts;
            clock_gettime(CLOCK_REALTIME, &now_ts);
            uint64_t now_ns = static_cast<uint64_t>(now_ts.tv_sec) * 1'000'000'000ULL + now_ts.tv_nsec;
            if (ia_last_arrival_ns_ != 0 && ia_last_rtp_ts_ != 0) {
                double arrival_diff_ms = static_cast<double>(now_ns - ia_last_arrival_ns_) / 1'000'000.0;
                double rtp_diff_ms    = static_cast<double>(static_cast<int32_t>(rtp_ts - ia_last_rtp_ts_)) / 48.0;
                double d = std::abs(arrival_diff_ms - rtp_diff_ms);
                ia_jitter_ema_ms_ += (d - ia_jitter_ema_ms_) / 16.0;
            }
            ia_last_arrival_ns_ = now_ns;
            ia_last_rtp_ts_     = rtp_ts;
        }

        // Periodic stats (every ~5s at 500pps)
        // NOTE: jitter-adaptive buffer disabled for relay mode —
        // input rate == output rate (48kHz), so target must stay low
        // to prevent prefill stall. Underruns handled by PLC in audio callback.
        if (relay_inject_count_ % 2500 == 0) {
            size_t fill   = ring_buffer_.available_read();
            uint32_t tgt  = target_fill_frames_.load();
            fprintf(stderr, "[relay] fill=%zu target=%u jitter=%.1fms\n",
                    fill, tgt, ia_jitter_ema_ms_);
        }
    }

    /// Inject decoded PCM samples directly into the ring buffer.
    /// @param samples Interleaved int32_t samples (24-bit range: -8388608..8388607)
    /// @param frame_count Number of frames (each frame = channels_ samples)
    void inject_pcm_samples(const int32_t* samples, size_t frame_count) {
        ring_buffer_.write(samples, frame_count);
    }

    /// Request a ring-buffer flush from the audio callback thread (thread-safe).
    /// The actual reset happens at the top of the next audio_callback to avoid
    /// racing with the audio thread's reads of ring_buffer_, prefilled_, and ramp_.
    void flush_ring_buffer() {
        flush_requested_.store(true, std::memory_order_release);
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

    // ── WAN Relay ────────────────────────────────────────────────────────

    bool wan_relay_connect(const std::string& host, uint16_t port,
                           const std::string& group, const std::string& password,
                           const std::string& device_name,
                           const std::string& device_id = "") {
        if (!wan_relay_) wan_relay_ = std::make_unique<WanRelayClient>();
        // Set RX callback: inject via ReceiverImpl (handles buffer reset, jitter, ADPCM decode)
        wan_relay_->set_rx_callback([this](const uint8_t* data, size_t len) {
            inject_raw_packet(data, len);
        });
        // Apply stored callbacks BEFORE connect so they're active when
        // the relay sends FILE:/SYNC: to new joiners immediately after JOIN
        if (stored_meta_cb_) wan_relay_->set_meta_callback(stored_meta_cb_);
        if (stored_file_cb_) wan_relay_->set_file_callback(stored_file_cb_);
        if (stored_sync_cb_) wan_relay_->set_sync_callback(stored_sync_cb_);
        if (stored_members_cb_) wan_relay_->set_members_callback(stored_members_cb_);
        // MAXDELAY: adapt sync_delay_ms_ to group-wide max
        wan_relay_->set_maxdelay_callback([this](uint32_t max_ms) {
            uint32_t capped = std::min(max_ms, 2000u);
            uint32_t current = sync_delay_ms_.load(std::memory_order_relaxed);
            if (capped != current) {
                sync_delay_ms_.store(capped, std::memory_order_relaxed);
                fprintf(stderr, "[sync] MAXDELAY received: %u ms (was %u ms)\n", capped, current);
            }
        });
        // VOLUME_SET: remote volume control from another device
        wan_relay_->set_volume_callback([this](float vol) {
            volume_.store(vol, std::memory_order_relaxed);
            fprintf(stderr, "[relay] Remote volume set to %.0f%%\n", vol * 100.0f);
        });
        // Reset first-packet flag so buffer is re-initialized on (re)connect
        relay_first_packet_received_.store(false, std::memory_order_relaxed);
        bool ok = wan_relay_->connect(host, port, group, password, device_name, device_id);
        if (ok) {
            set_relay_network_disabled(true);
            // Disable sync mode for relay — use fixed buffer instead
            sync_mode_.store(false, std::memory_order_relaxed);
            sync_delay_ms_.store(200, std::memory_order_relaxed);
            // Flush old channel audio from ring buffer
            flush_requested_.store(true, std::memory_order_release);
            target_fill_frames_.store(1440, std::memory_order_relaxed);  // 30ms — prefill fast, jitter adapts up
            health_.store(0, std::memory_order_relaxed);
            health_silenced_.store(false, std::memory_order_relaxed);
            prefilled_ = false;
            sync_samples_count_ = 0;
            fprintf(stderr, "[relay] Connected — target=1440 (30ms), sync off\n");
        }
        return ok;
    }

    void wan_relay_disconnect() {
        if (wan_relay_) wan_relay_->disconnect();
        set_relay_network_disabled(false);  // restore multicast
    }

    WanRelayClient::State wan_relay_state() const {
        return wan_relay_ ? wan_relay_->state() : WanRelayClient::State::Disconnected;
    }

    std::string wan_relay_group() const {
        return wan_relay_ ? wan_relay_->group() : "";
    }

    std::string wan_relay_error() const {
        return wan_relay_ ? wan_relay_->error() : "";
    }

    /// Send raw audio packet to WAN relay (called from TX path)
    void wan_relay_send_audio(const uint8_t* data, size_t len) {
        if (wan_relay_ && wan_relay_->state() == WanRelayClient::State::Connected) {
            wan_relay_->send_audio(data, len);
        }
    }

    void wan_relay_set_meta_callback(WanRelayClient::MetaCallback cb) {
        stored_meta_cb_ = cb;
        if (wan_relay_) wan_relay_->set_meta_callback(std::move(cb));
    }

    void wan_relay_set_file_callback(WanRelayClient::FileCallback cb) {
        stored_file_cb_ = cb;
        if (wan_relay_) wan_relay_->set_file_callback(std::move(cb));
    }

    void wan_relay_set_sync_callback(WanRelayClient::SyncCallback cb) {
        stored_sync_cb_ = cb;
        if (wan_relay_) wan_relay_->set_sync_callback(std::move(cb));
    }

    void wan_relay_set_members_callback(WanRelayClient::MembersCallback cb) {
        stored_members_cb_ = cb;
        if (wan_relay_) wan_relay_->set_members_callback(std::move(cb));
    }

    void wan_relay_send_ready(const std::string& filename) {
        if (wan_relay_) wan_relay_->send_ready(filename);
    }

    void wan_relay_mic_allow(const std::string& device_id) {
        if (wan_relay_) wan_relay_->send_mic_allow(device_id);
    }

    void wan_relay_mic_deny(const std::string& device_id) {
        if (wan_relay_) wan_relay_->send_mic_deny(device_id);
    }

    void wan_relay_mic_list() {
        if (wan_relay_) wan_relay_->send_mic_list();
    }

    void wan_relay_members() {
        if (wan_relay_) wan_relay_->send_members();
    }

    void wan_relay_send_volume(const std::string& target_device_id, int level) {
        if (wan_relay_) wan_relay_->send_volume(target_device_id, level);
    }

    /// Send a raw text command via WAN relay (for DJ mode FILE:/META:/SYNC: commands)
    void wan_relay_send_cmd(const std::string& cmd) {
        if (wan_relay_ && wan_relay_->state() == WanRelayClient::State::Connected) {
            wan_relay_->send_command(cmd);
        }
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

    WanRelayClient* wan_relay() { return wan_relay_.get(); }

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
        if (now_ms_() - last_underrun_ms_ >= 5000) {
            // 5 seconds clean: restore normal operation
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
            // When network disabled, audio arrives via inject_raw_packet instead
            if (!is_network_disabled() && rtp_receiver_) {
                for (int i = 0; i < 10 && running_.load(); i++) {
                    if (!rtp_receiver_->receive_packet(ring_buffer_)) break;
                }
            }

            // ── Sync mode: adjust buffer target from OSTP wall-clock timestamps ──
            // Run every 50 injected packets (~0.1s at 500pps) — same as iOS
            if ((relay_inject_count_ % 50 == 0) &&
                sync_mode_.load(std::memory_order_relaxed) && rtp_receiver_) {
                uint32_t media_ts = rtp_receiver_->last_media_timestamp.load(std::memory_order_relaxed);
                if (media_ts != 0) {
                    struct timespec now_ts;
                    clock_gettime(CLOCK_REALTIME, &now_ts);
                    uint32_t now_ms32 = static_cast<uint32_t>(
                        (static_cast<uint64_t>(now_ts.tv_sec) * 1000ULL +
                         static_cast<uint64_t>(now_ts.tv_nsec) / 1'000'000ULL)
                        & 0xFFFFFFFF);
                    // Apply NTP clock offset correction (relay_time - local_time)
                    int64_t offset_ns = wan_relay_ ? wan_relay_->clock_offset_ns() : 0;
                    int32_t offset_ms = static_cast<int32_t>(offset_ns / 1'000'000LL);
                    int32_t net_delay_ms = static_cast<int32_t>(now_ms32 - media_ts) + offset_ms;
                    if (net_delay_ms >= 0 && net_delay_ms < 2000) {
                        uint32_t total_delay_ms = sync_delay_ms_.load();
                        int32_t buffer_ms = static_cast<int32_t>(total_delay_ms) - net_delay_ms;
                        if (buffer_ms < 5) buffer_ms = 5;
                        uint32_t target = static_cast<uint32_t>(buffer_ms * 48);
                        uint32_t prev = target_fill_frames_.load();
                        int32_t diff = static_cast<int32_t>(target) - static_cast<int32_t>(prev);
                        // Adaptive EMA: fast initial lock-on, slow when stable
                        double alpha = (sync_samples_count_ < 50) ? 0.20
                                     : (std::abs(diff) > 2400)    ? 0.15
                                     : (std::abs(diff) > 480)     ? 0.08
                                                                   : 0.02;
                        if (sync_samples_count_ < 50) sync_samples_count_++;
                        uint32_t smoothed = static_cast<uint32_t>(prev * (1.0 - alpha) + target * alpha);
                        // 500ms floor in WAN relay mode to prevent underruns
                        smoothed = std::max(smoothed, 24000u);
                        target_fill_frames_.store(smoothed);
                    }
                }
            }

            // Report network delay to relay every ~5 seconds for sync coordination
            if (loop_count % 50000 == 0 && wan_relay_ && sync_mode_.load(std::memory_order_relaxed)) {
                uint32_t media_ts = rtp_receiver_ ? rtp_receiver_->last_media_timestamp.load(std::memory_order_relaxed) : 0;
                if (media_ts != 0) {
                    struct timespec rpt_ts;
                    clock_gettime(CLOCK_REALTIME, &rpt_ts);
                    uint32_t rpt_ms32 = static_cast<uint32_t>(
                        (static_cast<uint64_t>(rpt_ts.tv_sec) * 1000ULL +
                         static_cast<uint64_t>(rpt_ts.tv_nsec) / 1'000'000ULL) & 0xFFFFFFFF);
                    int64_t offset_ns = wan_relay_->clock_offset_ns();
                    int32_t offset_ms = static_cast<int32_t>(offset_ns / 1'000'000LL);
                    int32_t nd_ms = static_cast<int32_t>(rpt_ms32 - media_ts) + offset_ms;
                    if (nd_ms >= 0 && nd_ms < 2000) {
                        wan_relay_->send_delay(static_cast<uint32_t>(nd_ms));
                    }
                }
            }

            // Debug: log every ~5 seconds
            if (++loop_count % 50000 == 0) {
                auto st = stats();
                size_t fill = ring_buffer_.available_read();
                fprintf(stderr, "[SolunaRx] pkts=%llu fill=%zu target=%u vol=%.2f prefilled=%d cb=%llu%s\n",
                        (unsigned long long)st.packets_received, fill,
                        target_fill_frames_.load(),
                        (double)volume_.load(), (int)prefilled_,
                        (unsigned long long)audio_cb_count_.load(std::memory_order_relaxed),
                        sync_mode_.load() ? " [SYNC]" : "");
                last_log_pkts = st.packets_received;
            }
            // When relay mode (network disabled), audio arrives via inject_raw_packet
            // on the relay thread — this loop only does sync/stats, so sleep longer.
            if (is_network_disabled()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
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
        // Handle deferred flush request from main thread (see flush_ring_buffer())
        if (flush_requested_.load(std::memory_order_acquire)) {
            flush_requested_.store(false, std::memory_order_relaxed);
            // Safe consumer-side drain: advance read_pos to current write_pos.
            // Do NOT use ring_buffer_.reset() — it resets both write_pos and read_pos,
            // which is unsafe while the relay thread is concurrently writing.
            size_t to_drain = ring_buffer_.available_read();
            if (to_drain > 0) ring_buffer_.discard(to_drain);
            prefilled_ = false;
            ramp_ = 0.0f;
        }

        audio_cb_count_.fetch_add(1, std::memory_order_relaxed);
        const float vol = (muted_.load() || health_silenced_.load(std::memory_order_relaxed))
                          ? 0.0f : volume_.load();
        const uint32_t total_samples = frame_count * channels_;

        // Adaptive target: always >= frame_count*3 to prevent immediate underrun
        uint32_t target = target_fill_frames_.load() + primary_delay_frames_.load();
        const uint32_t min_target = frame_count * 3;
        if (target < min_target) target = min_target;

        // ── Drift correction with overflow protection ────────────────────
        {
            size_t avail_now = ring_buffer_.available_read();
            size_t capacity = ring_buffer_.capacity();

            // Emergency: if buffer is >80% full, flush to target to prevent crash
            if (avail_now > capacity * 4 / 5) {
                size_t drain_to = static_cast<size_t>(target);
                if (avail_now > drain_to) {
                    ring_buffer_.discard(avail_now - drain_to);
                }
                drift_xfade_ = 96;
            }
            // Normal: gradual drift when overfilled (3x target)
            else if (prefilled_ && avail_now > static_cast<size_t>(target) * 3) {
                size_t excess = avail_now - static_cast<size_t>(target) * 2;
                size_t drift = std::min(excess, static_cast<size_t>(frame_count / 4 + 1));
                ring_buffer_.discard(drift);
                drift_xfade_ = 48;
            }
        }

        const size_t avail = ring_buffer_.available_read();

        constexpr float kFadeIn  = 0.002f;  // matched with iOS
        constexpr float kFadeOut = 0.004f;

        // Balance gains (needed by both underrun and normal paths)
        const float bal = primary_balance_.load(std::memory_order_relaxed);
        const float gain_l = (channels_ >= 2 && bal > 0) ? (1.0f - bal) : 1.0f;
        const float gain_r = (channels_ >= 2 && bal < 0) ? (1.0f + bal) : 1.0f;

        // ── Initial prefill (only at startup, NOT reset on underrun) ───────
        if (!prefilled_) {
            if (avail < target) {
                std::memset(buffer, 0, total_samples * sizeof(float));
                return;
            }
            prefilled_ = true;
            ramp_ = 0.0f;  // ensure clean fade-in
            // Buffer refilled — immediately clear silenced state so audio resumes
            if (health_silenced_.load(std::memory_order_relaxed)) {
                health_silenced_.store(false, std::memory_order_relaxed);
                health_.store(0, std::memory_order_relaxed);
                health_underruns_in_window_ = 0;
                last_underrun_ms_ = 0;
                fprintf(stderr, "[health] Buffer refilled — silence cleared\n");
            }
        }

        // ── Underrun: play what we have, then fade out remainder ──────────
        // Matched with iOS: no prefill reset on underrun to avoid 80ms gaps.
        if (avail < frame_count) {
            record_underrun_now();
            maybe_check_recovery();
            const size_t have = avail;
            if (have > 0) {
                ring_buffer_.read(read_buffer_.data(), have);
                const int32_t* src = read_buffer_.data();
                for (uint32_t i = 0; i < have; i++) {
                    ramp_ += kFadeIn * (vol - ramp_);
                    for (uint32_t ch = 0; ch < channels_; ch++) {
                        const uint32_t idx = i * channels_ + ch;
                        float s = static_cast<float>(src[idx]) / 8388608.0f;
                        if (s > 1.0f) s = 1.0f;
                        else if (s < -1.0f) s = -1.0f;
                        float ch_gain = (ch == 0) ? gain_l : (ch == 1) ? gain_r : 1.0f;
                        float out = s * ramp_ * ch_gain;
                        buffer[idx] = out;
                        held_sample_[ch] = out;
                    }
                }
            }
            for (uint32_t i = static_cast<uint32_t>(have); i < frame_count; i++) {
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
        for (uint32_t i = 0; i < frame_count; i++) {
            ramp_ += kFadeIn * (vol - ramp_);
            for (uint32_t ch = 0; ch < channels_; ch++) {
                const uint32_t idx = i * channels_ + ch;
                float s = static_cast<float>(src[idx]) / 8388608.0f;  // matched with iOS
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
                float out = s * ramp_ * ch_gain;
                // Drift crossfade to smooth discontinuities after discard
                if (drift_xfade_ > 0) {
                    float alpha = 1.0f - static_cast<float>(drift_xfade_) / 49.0f;
                    out = out * alpha + held_sample_[ch] * (1.0f - alpha);
                }
                buffer[idx] = out;
                held_sample_[ch] = out;
            }
            if (drift_xfade_ > 0) drift_xfade_--;
        }

        // ── EBU R128 loudness normalization ──────────────────────────────
        if (loudness_norm_enabled_.load(std::memory_order_relaxed)) {
            // Simplified K-weighted loudness measurement — mean square power
            float sum_sq = 0;
            for (uint32_t i = 0; i < total_samples; i++) {
                float s = buffer[i];
                sum_sq += s * s;
            }
            float mean_sq = sum_sq / total_samples;

            // Store power (mean square) in sliding window (~400ms at ~120 callbacks/sec)
            // Power-domain averaging is correct for EBU R128 (not dB averaging)
            loudness_window_[loudness_window_pos_] = mean_sq;
            loudness_window_pos_ = (loudness_window_pos_ + 1) % 48;
            if (loudness_window_pos_ == 0) loudness_window_full_ = true;

            // Calculate integrated loudness — average power then convert to dB
            int count = loudness_window_full_ ? 48 : loudness_window_pos_;
            if (count > 0) {
                float power_sum = 0;
                for (int i = 0; i < count; i++) power_sum += loudness_window_[i];
                float avg_power = power_sum / count;
                float avg_db = (avg_power > 1e-20f) ? 10.0f * log10f(avg_power) : -100.0f;

                // Target -23 LUFS, calculate needed gain
                float target_db = -23.0f;
                float gain_db = target_db - avg_db;
                // Clamp gain to reasonable range (-12 to +12 dB)
                if (gain_db < -12.0f) gain_db = -12.0f;
                if (gain_db > 12.0f) gain_db = 12.0f;

                float target_gain = powf(10.0f, gain_db / 20.0f);
                // Smooth gain changes (EMA)
                loudness_current_gain_ = loudness_current_gain_ * 0.95f + target_gain * 0.05f;
            }

            // Apply gain
            for (uint32_t i = 0; i < total_samples; i++) {
                buffer[i] *= loudness_current_gain_;
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

        // Spectrum analyzer: feed every 4th callback (~60Hz visual update rate)
        if ((audio_cb_count_.load(std::memory_order_relaxed) & 3) == 0) {
            spectrum_.feed(buffer, frame_count, channels_);
        }

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
    std::atomic<bool>     sync_mode_{true};
    std::atomic<uint32_t> sync_delay_ms_{80};      // 80ms — matched with iOS for cross-device sync
    std::atomic<bool>     relay_network_disabled_{false};
    std::atomic<bool>     filesync_network_disabled_{false};
    std::atomic<bool>     talk_mode_active_{false};  // multi-speaker mode
    std::atomic<bool>     flush_requested_{false};
    std::atomic<uint64_t> audio_cb_count_{0};   ///< audio callback invocation counter (debug)
    // Loudness normalization (EBU R128):
    std::atomic<bool>     loudness_norm_enabled_{false};
    // Health tracking atomics (written audio-cb, read ObjC):
    std::atomic<int>      health_{0};           ///< 0=good 1=stressed 2=silenced
    std::atomic<bool>     health_silenced_{false};
    // VU meter atomics (written audio-cb, read ObjC):
    std::atomic<float>    primary_level_rms_{0};
    std::atomic<float>    primary_level_peak_{0};
    // audio_callback-only state (no atomics needed):
    bool                  prefilled_ = false;
    float                 ramp_      = 0.0f;
    int                   drift_xfade_ = 0;  // crossfade counter after drift discard
    std::vector<float>    held_sample_;
    // Loudness normalization state (audio-callback-only):
    float loudness_current_gain_ = 1.0f;
    float loudness_window_[48]{};           // 400ms window at ~120 callbacks/sec
    int   loudness_window_pos_ = 0;
    bool  loudness_window_full_ = false;
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

    // WAN relay jitter tracking (RFC 3550 inter-arrival jitter)
    std::atomic<bool> relay_first_packet_received_{false};
    uint64_t relay_inject_count_ = 0;
    uint32_t sync_samples_count_ = 0;  // for adaptive EMA alpha
    uint64_t ia_last_arrival_ns_ = 0;
    uint32_t ia_last_rtp_ts_     = 0;
    double   ia_jitter_ema_ms_   = 0.0;

    // Recording callback (called from audio callback with float samples)
    std::function<void(const float*, uint32_t)> record_callback_;
    std::mutex record_mutex_;

    static OSStatus inline_render_cb(void* ref,
                                     AudioUnitRenderActionFlags*,
                                     const AudioTimeStamp*,
                                     UInt32,
                                     UInt32 frame_count,
                                     AudioBufferList* ioData) {
        auto* self = static_cast<ReceiverImpl*>(ref);
        auto* dst = static_cast<float*>(ioData->mBuffers[0].mData);
        const uint32_t total = frame_count * self->channels_;
        if (!self->running_.load()) {
            std::memset(dst, 0, total * sizeof(float));
            return noErr;
        }
        self->audio_callback(dst, frame_count);
        return noErr;
    }

    std::unique_ptr<SimpleRtpReceiver> rtp_receiver_;
    std::unique_ptr<pal::AudioDevice>  audio_device_;  // unused (kept for future)
    AudioUnit inline_audio_unit_ = nullptr;
    pipeline::RingBuffer  ring_buffer_;
    std::vector<int32_t>  read_buffer_;
    std::vector<int32_t>  drain_buf_;

    std::thread receive_thread_;
    soluna::control::WebSocketServer ws_server_;

    // Multi-output sinks (BT, AirPlay, USB, etc.)
    std::mutex sinks_mutex_;
    std::vector<std::unique_ptr<OutputSink>> extra_sinks_;

    // WAN relay client
    std::unique_ptr<WanRelayClient> wan_relay_;
    WanRelayClient::MetaCallback stored_meta_cb_;
    WanRelayClient::FileCallback stored_file_cb_;
    WanRelayClient::SyncCallback stored_sync_cb_;
    WanRelayClient::MembersCallback stored_members_cb_;
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
    std::unique_ptr<TransmitterImpl> _txImpl;
    std::unique_ptr<ShmTransmitter> _shmTxImpl;
    std::unique_ptr<DJBroadcaster> _djImpl;
    std::unique_ptr<DJController> _djCtrlImpl;
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
    // Stop transmitters if active
    [self stopMicTransmit];
    [self stopShmTransmit];

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
    if (_impl) _impl->set_filesync_network_disabled((bool)disabled);
}

- (BOOL)relayNetworkDisabled {
    return _impl ? (BOOL)_impl->is_network_disabled() : NO;
}

- (void)setRelayNetworkDisabled:(BOOL)disabled {
    if (_impl) _impl->set_relay_network_disabled((bool)disabled);
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

- (void)injectPcmSamples:(NSData *)data frameCount:(NSUInteger)frameCount {
    if (_impl && data.length > 0) {
        _impl->inject_pcm_samples(static_cast<const int32_t*>(data.bytes), frameCount);
    }
}

- (void)flushRingBuffer {
    if (_impl) {
        _impl->flush_ring_buffer();
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

// ── Sync Mode ───────────────────────────────────────────────────────────────

- (BOOL)syncMode {
    return _impl ? _impl->is_sync_mode() : NO;
}

- (void)setSyncMode:(BOOL)syncMode {
    if (_impl) _impl->set_sync_mode(syncMode);
}

- (uint32_t)syncDelayMs {
    return _impl ? _impl->sync_delay_ms() : 200;
}

- (void)setSyncDelayMs:(uint32_t)syncDelayMs {
    if (_impl) _impl->set_sync_delay_ms(syncDelayMs);
}

// ── Loudness Normalization ──────────────────────────────────────────────────

- (void)setLoudnessNormEnabled:(BOOL)enabled {
    if (_impl) _impl->set_loudness_norm(enabled);
}

- (BOOL)loudnessNormEnabled {
    return _impl ? _impl->is_loudness_norm() : NO;
}

// ── Talk Mode (multi-speaker) ────────────────────────────────────────────────

- (void)setTalkMode:(BOOL)enabled {
    if (_impl) {
        _impl->set_talk_mode(enabled);
    }
}

// ── Mic Transmit (TX) ───────────────────────────────────────────────────────

- (BOOL)isMicTransmitting {
    return _txImpl && _txImpl->is_running();
}

- (uint64_t)txPacketsSent {
    return _txImpl ? _txImpl->packets_sent() : 0;
}

- (float)micInputLevel {
    return _txImpl ? _txImpl->peak_level() : 0.0f;
}

- (BOOL)startMicTransmit {
    if (_txImpl && _txImpl->is_running()) return YES;

    _txImpl = std::make_unique<TransmitterImpl>(
        std::string([_multicastGroup UTF8String]),
        _port,
        _channels
    );

    // Always wire WAN relay forwarding — wan_relay_send_audio checks Connected state internally
    if (_impl) {
        auto* impl = _impl.get();
        _txImpl->tx_relay_callback = [impl](const uint8_t* data, size_t len) {
            impl->wan_relay_send_audio(data, len);
        };
    }

    if (!_txImpl->start()) {
        fprintf(stderr, "[SolunaTx] Failed to start transmitter\n");
        _txImpl.reset();
        return NO;
    }

    fprintf(stderr, "[SolunaTx] Mic transmit started\n");
    return YES;
}

- (void)stopMicTransmit {
    if (_txImpl) {
        _txImpl->stop();
        _txImpl.reset();
    }
    fprintf(stderr, "[SolunaTx] Mic transmit stopped\n");
}

// ── WAN Relay (Group Code) ───────────────────────────────────────────────

- (BOOL)connectToRelay:(NSString *)host
                  port:(uint16_t)port
                 group:(NSString *)group
              password:(NSString *)password {
    return [self connectToRelay:host port:port group:group password:password deviceId:@""];
}

- (BOOL)connectToRelay:(NSString *)host
                  port:(uint16_t)port
                 group:(NSString *)group
              password:(NSString *)password
              deviceId:(NSString *)deviceId {
    if (!_impl) return NO;

    // Get device name (Mac hostname)
    NSString *deviceName = [[NSHost currentHost] localizedName] ?: @"Mac";

    bool ok = _impl->wan_relay_connect(
        std::string([host UTF8String]),
        port,
        std::string([group UTF8String]),
        std::string([password UTF8String]),
        std::string([deviceName UTF8String]),
        std::string([deviceId UTF8String])
    );

    // Wire TX relay callbacks if transmitters are active
    if (ok) {
        auto* impl = _impl.get();
        if (_txImpl) {
            _txImpl->tx_relay_callback = [impl](const uint8_t* data, size_t len) {
                impl->wan_relay_send_audio(data, len);
            };
        }
        if (_shmTxImpl) {
            _shmTxImpl->tx_relay_callback = [impl](const uint8_t* data, size_t len) {
                impl->wan_relay_send_audio(data, len);
            };
        }
        if (_djImpl) {
            _djImpl->tx_relay_callback = [impl](const uint8_t* data, size_t len) {
                impl->wan_relay_send_audio(data, len);
            };
            _djImpl->send_relay_cmd = [impl](const std::string& cmd) {
                impl->wan_relay_send_cmd(cmd);
            };
        }
    }

    return ok ? YES : NO;
}

- (void)sendMicAllow:(NSString *)deviceId {
    if (_impl) _impl->wan_relay_mic_allow(std::string([deviceId UTF8String]));
}

- (void)sendMicDeny:(NSString *)deviceId {
    if (_impl) _impl->wan_relay_mic_deny(std::string([deviceId UTF8String]));
}

- (void)requestMicList {
    if (_impl) _impl->wan_relay_mic_list();
}

- (void)requestMembers {
    if (_impl) _impl->wan_relay_members();
}

- (void)sendVolumeToDevice:(NSString *)deviceId level:(int)level {
    if (_impl) _impl->wan_relay_send_volume(std::string([deviceId UTF8String]), level);
}

- (void)disconnectRelay {
    // Remove TX relay callbacks
    if (_txImpl) {
        _txImpl->tx_relay_callback = nullptr;
    }
    if (_shmTxImpl) {
        _shmTxImpl->tx_relay_callback = nullptr;
    }
    if (_djImpl) {
        _djImpl->tx_relay_callback = nullptr;
        _djImpl->send_relay_cmd = nullptr;
    }
    if (_impl) _impl->wan_relay_disconnect();
}

- (SolunaRelayState)relayState {
    if (!_impl) return SolunaRelayStateDisconnected;
    switch (_impl->wan_relay_state()) {
        case WanRelayClient::State::Disconnected: return SolunaRelayStateDisconnected;
        case WanRelayClient::State::Connecting:    return SolunaRelayStateConnecting;
        case WanRelayClient::State::Connected:     return SolunaRelayStateConnected;
        case WanRelayClient::State::Error:         return SolunaRelayStateError;
    }
    return SolunaRelayStateDisconnected;
}

- (NSString *)relayGroup {
    if (!_impl) return nil;
    auto g = _impl->wan_relay_group();
    return g.empty() ? nil : [NSString stringWithUTF8String:g.c_str()];
}

- (NSString *)relayError {
    if (!_impl) return nil;
    auto e = _impl->wan_relay_error();
    return e.empty() ? nil : [NSString stringWithUTF8String:e.c_str()];
}

// ── Metadata / File-Sync Callbacks ──────────────────────────────────────────

- (void)setMetaCallback:(nullable void(^)(NSString *))callback {
    if (_impl) {
        if (callback) {
            _impl->wan_relay_set_meta_callback([callback](const std::string& json) {
                NSString *str = [NSString stringWithUTF8String:json.c_str()];
                dispatch_async(dispatch_get_main_queue(), ^{ callback(str); });
            });
        } else {
            _impl->wan_relay_set_meta_callback(nullptr);
        }
    }
}

- (void)setFileCallback:(nullable void(^)(NSString *))callback {
    if (_impl) {
        if (callback) {
            _impl->wan_relay_set_file_callback([callback](const std::string& filename) {
                NSString *str = [NSString stringWithUTF8String:filename.c_str()];
                dispatch_async(dispatch_get_main_queue(), ^{ callback(str); });
            });
        } else {
            _impl->wan_relay_set_file_callback(nullptr);
        }
    }
}

- (void)setSyncCallback:(nullable void(^)(NSString *))callback {
    if (_impl) {
        if (callback) {
            _impl->wan_relay_set_sync_callback([callback](const std::string& sync) {
                NSString *str = [NSString stringWithUTF8String:sync.c_str()];
                dispatch_async(dispatch_get_main_queue(), ^{ callback(str); });
            });
        } else {
            _impl->wan_relay_set_sync_callback(nullptr);
        }
    }
}

- (void)setMembersCallback:(nullable void(^)(NSString *))callback {
    if (_impl) {
        if (callback) {
            _impl->wan_relay_set_members_callback([callback](const std::string& json) {
                NSString *str = [NSString stringWithUTF8String:json.c_str()];
                dispatch_async(dispatch_get_main_queue(), ^{ callback(str); });
            });
        } else {
            _impl->wan_relay_set_members_callback(nullptr);
        }
    }
}

- (void)sendReady:(NSString *)filename {
    if (_impl) {
        _impl->wan_relay_send_ready(std::string(filename.UTF8String));
    }
}

// ── System Audio Transmit (SHM) ─────────────────────────────────────────────

- (BOOL)isShmTransmitting {
    return _shmTxImpl && _shmTxImpl->is_running();
}

- (uint64_t)shmTxPacketsSent {
    return _shmTxImpl ? _shmTxImpl->packets_sent() : 0;
}

- (float)shmTxLevel {
    return _shmTxImpl ? _shmTxImpl->peak_level() : 0.0f;
}

- (BOOL)startShmTransmit {
    if (_shmTxImpl && _shmTxImpl->is_running()) return YES;

    _shmTxImpl = std::make_unique<ShmTransmitter>(
        std::string([_multicastGroup UTF8String]),
        _port,
        _channels
    );

    // Always wire WAN relay forwarding — wan_relay_send_audio checks Connected state internally
    if (_impl) {
        auto* impl = _impl.get();
        _shmTxImpl->tx_relay_callback = [impl](const uint8_t* data, size_t len) {
            impl->wan_relay_send_audio(data, len);
        };
    }

    if (!_shmTxImpl->start()) {
        fprintf(stderr, "[ShmTx] Failed to start\n");
        _shmTxImpl.reset();
        return NO;
    }
    return YES;
}

- (void)stopShmTransmit {
    if (_shmTxImpl) {
        _shmTxImpl->stop();
        _shmTxImpl.reset();
    }
}

// ── DJ Mode ─────────────────────────────────────────────────────────────────

- (BOOL)isDJActive {
    return _djImpl && _djImpl->is_active();
}

- (NSString *)djCurrentTrack {
    if (!_djImpl) return nil;
    auto t = _djImpl->current_track();
    return t.empty() ? nil : [NSString stringWithUTF8String:t.c_str()];
}

- (float)djProgress {
    return _djImpl ? _djImpl->progress() : 0.0f;
}

- (BOOL)startDJBroadcast:(NSString *)filePath {
    // Stop existing DJ session if any
    if (_djImpl) _djImpl->stop();

    _djImpl = std::make_unique<DJBroadcaster>();

    // Wire relay callbacks using ReceiverImpl's WAN relay
    if (_impl) {
        auto* impl = _impl.get();
        _djImpl->tx_relay_callback = [impl](const uint8_t* data, size_t len) {
            impl->wan_relay_send_audio(data, len);
        };
        _djImpl->send_relay_cmd = [impl](const std::string& cmd) {
            impl->wan_relay_send_cmd(cmd);
        };
    }

    return _djImpl->start_file([filePath UTF8String]) ? YES : NO;
}

- (BOOL)startDJDirectory:(NSString *)dirPath shuffle:(BOOL)shuffle {
    // Stop existing DJ session if any
    if (_djImpl) _djImpl->stop();

    _djImpl = std::make_unique<DJBroadcaster>();

    // Wire relay callbacks using ReceiverImpl's WAN relay
    if (_impl) {
        auto* impl = _impl.get();
        _djImpl->tx_relay_callback = [impl](const uint8_t* data, size_t len) {
            impl->wan_relay_send_audio(data, len);
        };
        _djImpl->send_relay_cmd = [impl](const std::string& cmd) {
            impl->wan_relay_send_cmd(cmd);
        };
    }

    return _djImpl->start_directory([dirPath UTF8String], (bool)shuffle) ? YES : NO;
}

- (void)stopDJ {
    if (_djImpl) {
        _djImpl->stop();
        _djImpl.reset();
    }
    fprintf(stderr, "[DJ] DJ mode stopped\n");
}

- (void)skipDJTrack {
    if (_djImpl) {
        _djImpl->skip();
    }
}

// ── DJ Mic Mix ──────────────────────────────────────────────────────────────

- (void)setDjMicMixEnabled:(BOOL)enabled {
    if (_djImpl) _djImpl->set_mic_mix(enabled);
}

- (BOOL)djMicMixEnabled {
    return _djImpl ? _djImpl->mic_mix_enabled_.load() : NO;
}

- (void)setDjMicGain:(float)gain {
    if (_djImpl) _djImpl->mic_gain_.store(gain, std::memory_order_relaxed);
}

- (float)djMicGain {
    return _djImpl ? _djImpl->mic_gain_.load(std::memory_order_relaxed) : 1.0f;
}

- (void)setDjMusicGain:(float)gain {
    if (_djImpl) _djImpl->music_gain_.store(gain, std::memory_order_relaxed);
}

- (float)djMusicGain {
    return _djImpl ? _djImpl->music_gain_.load(std::memory_order_relaxed) : 0.7f;
}

// ── DJ Dual-Deck Mode ────────────────────────────────────────────────────────

- (BOOL)startDeckA:(NSString *)filePath {
    if (!_impl) return NO;
    WanRelayClient* relay = _impl->wan_relay();
    if (!relay || relay->state() != WanRelayClient::State::Connected) {
        fprintf(stderr, "[DJCtrl] Cannot start deck A: relay not connected\n");
        return NO;
    }
    if (!_djCtrlImpl) _djCtrlImpl = std::make_unique<DJController>(relay);
    return _djCtrlImpl->start_deck_a(filePath.UTF8String) ? YES : NO;
}

- (BOOL)startDeckB:(NSString *)filePath {
    if (!_impl) return NO;
    WanRelayClient* relay = _impl->wan_relay();
    if (!relay || relay->state() != WanRelayClient::State::Connected) {
        fprintf(stderr, "[DJCtrl] Cannot start deck B: relay not connected\n");
        return NO;
    }
    if (!_djCtrlImpl) _djCtrlImpl = std::make_unique<DJController>(relay);
    return _djCtrlImpl->start_deck_b(filePath.UTF8String) ? YES : NO;
}

- (void)toggleDeckA {
    if (_djCtrlImpl) _djCtrlImpl->toggle_deck_a();
}

- (void)toggleDeckB {
    if (_djCtrlImpl) _djCtrlImpl->toggle_deck_b();
}

- (void)stopDualDeck {
    _djCtrlImpl.reset();
}

- (void)setDjCrossfader:(float)v {
    if (_djCtrlImpl) _djCtrlImpl->set_crossfader(v);
}

- (float)djCrossfader {
    return _djCtrlImpl ? _djCtrlImpl->get_crossfader() : 0.5f;
}

- (BOOL)isDualDeckActive {
    return _djCtrlImpl && _djCtrlImpl->is_active() ? YES : NO;
}

- (NSString *)deckATrack {
    if (!_djCtrlImpl) return nil;
    auto name = _djCtrlImpl->deck_a_track();
    return name.empty() ? nil : @(name.c_str());
}

- (NSString *)deckBTrack {
    if (!_djCtrlImpl) return nil;
    auto name = _djCtrlImpl->deck_b_track();
    return name.empty() ? nil : @(name.c_str());
}

- (float)deckAProgress {
    return _djCtrlImpl ? _djCtrlImpl->deck_a_progress() : 0.0f;
}

- (float)deckBProgress {
    return _djCtrlImpl ? _djCtrlImpl->deck_b_progress() : 0.0f;
}

- (BOOL)deckAPlaying {
    return _djCtrlImpl && _djCtrlImpl->deck_a_playing() ? YES : NO;
}

- (BOOL)deckBPlaying {
    return _djCtrlImpl && _djCtrlImpl->deck_b_playing() ? YES : NO;
}

@end
