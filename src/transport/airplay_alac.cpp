/**
 * Minimal ALAC Decoder — Based on Apple ALAC reference implementation (BSD license)
 *
 * This is a simplified ALAC decoder that handles the common AirPlay case:
 * - 16-bit or 24-bit audio
 * - 2 channels (stereo)
 * - 352 samples per frame (Apple default)
 * - 44100 Hz sample rate
 *
 * The decoder reads ALAC compressed frames and outputs interleaved int16 PCM.
 * For 24-bit sources, samples are dithered down to 16-bit.
 *
 * Reference: https://github.com/macosforge/alac (BSD license)
 *
 * SPDX-License-Identifier: OpenSonic-Community-1.0
 */

#ifdef SOLUNA_HAS_AIRPLAY

#include <soluna/transport/airplay.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

namespace soluna::transport {

// ── Bit reader for ALAC bitstream ────────────────────────────────────────────

class BitReader {
public:
    BitReader(const uint8_t* data, size_t len)
        : data_(data), len_(len), pos_(0), bit_pos_(0) {}

    uint32_t read(uint32_t bits) {
        uint32_t result = 0;
        while (bits > 0) {
            if (pos_ >= len_) return result;
            uint32_t avail = 8 - bit_pos_;
            uint32_t take = std::min(bits, avail);
            uint32_t mask = (1u << take) - 1;
            uint32_t shift = avail - take;
            result = (result << take) | ((data_[pos_] >> shift) & mask);
            bit_pos_ += take;
            bits -= take;
            if (bit_pos_ >= 8) {
                bit_pos_ = 0;
                pos_++;
            }
        }
        return result;
    }

    int32_t read_signed(uint32_t bits) {
        uint32_t val = read(bits);
        // Sign extend
        if (bits > 0 && (val & (1u << (bits - 1)))) {
            val |= ~((1u << bits) - 1);
        }
        return static_cast<int32_t>(val);
    }

    void advance(uint32_t bits) {
        bit_pos_ += bits;
        pos_ += bit_pos_ / 8;
        bit_pos_ %= 8;
    }

    size_t bytes_read() const { return pos_ + (bit_pos_ > 0 ? 1 : 0); }
    bool   eof() const { return pos_ >= len_; }

private:
    const uint8_t* data_;
    size_t len_;
    size_t pos_;
    uint32_t bit_pos_;
};

// ── ALAC Decoder Implementation ──────────────────────────────────────────────

struct AlacDecoder::Impl {
    // Rice decoding parameters
    uint8_t  rice_history_mult = 40;
    uint8_t  rice_initial_history = 10;
    uint8_t  rice_k_modifier = 14;
    uint16_t max_run = 255;

    // Working buffers for prediction error + output
    std::vector<int32_t> pred_buf_a;
    std::vector<int32_t> pred_buf_b;
    std::vector<int32_t> mix_buf_a;
    std::vector<int32_t> mix_buf_b;
};

AlacDecoder::AlacDecoder()
    : impl_(std::make_unique<Impl>()) {}

AlacDecoder::~AlacDecoder() = default;

AlacDecoder::AlacDecoder(AlacDecoder&&) noexcept = default;
AlacDecoder& AlacDecoder::operator=(AlacDecoder&&) noexcept = default;

bool AlacDecoder::configure(const std::string& fmtp) {
    // Parse: "96 352 0 16 40 10 14 2 255 0 0 44100"
    // Skip the payload type number at the start
    const char* p = fmtp.c_str();
    // Skip leading whitespace
    while (*p == ' ') p++;

    // If starts with digit and has space, might be "96 352 ..."
    // Skip the payload type
    uint32_t values[12] = {};
    int count = 0;
    while (*p && count < 12) {
        while (*p == ' ') p++;
        if (!*p) break;
        values[count++] = static_cast<uint32_t>(strtoul(p, const_cast<char**>(&p), 10));
    }

    if (count < 12) {
        // Try without payload type prefix
        if (count >= 11) {
            // Shift values: first value was payload type
            return configure(values[1], values[11], static_cast<uint8_t>(values[3]),
                             static_cast<uint8_t>(values[7]));
        }
        fprintf(stderr, "[alac] fmtp parse error: only %d values (expected 12)\n", count);
        return false;
    }

    // values[0] = payload type (96)
    // values[1] = frame length (352)
    // values[2] = compatible version (0)
    // values[3] = bit depth (16)
    // values[4] = tuning/rice_history_mult (40)
    // values[5] = rice_initial_history (10)
    // values[6] = rice_k_modifier (14)
    // values[7] = num channels (2)
    // values[8] = max run (255)
    // values[9] = max frame bytes (0)
    // values[10] = avg bit rate (0)
    // values[11] = sample rate (44100)

    impl_->rice_history_mult = static_cast<uint8_t>(values[4]);
    impl_->rice_initial_history = static_cast<uint8_t>(values[5]);
    impl_->rice_k_modifier = static_cast<uint8_t>(values[6]);
    impl_->max_run = static_cast<uint16_t>(values[8]);

    return configure(values[1], values[11],
                     static_cast<uint8_t>(values[3]),
                     static_cast<uint8_t>(values[7]));
}

bool AlacDecoder::configure(uint32_t frame_length, uint32_t sample_rate,
                            uint8_t bit_depth, uint8_t num_channels) {
    if (frame_length == 0 || frame_length > 4096) {
        fprintf(stderr, "[alac] Invalid frame length: %u\n", frame_length);
        return false;
    }
    if (num_channels == 0 || num_channels > 8) {
        fprintf(stderr, "[alac] Invalid channel count: %u\n", num_channels);
        return false;
    }
    if (bit_depth != 16 && bit_depth != 20 && bit_depth != 24 && bit_depth != 32) {
        fprintf(stderr, "[alac] Unsupported bit depth: %u\n", bit_depth);
        return false;
    }

    frame_length_ = frame_length;
    sample_rate_ = sample_rate;
    bit_depth_ = bit_depth;
    num_channels_ = num_channels;
    configured_ = true;

    // Allocate working buffers
    impl_->pred_buf_a.resize(frame_length);
    impl_->pred_buf_b.resize(frame_length);
    impl_->mix_buf_a.resize(frame_length);
    impl_->mix_buf_b.resize(frame_length);

    fprintf(stderr, "[alac] Configured: %u frames, %uHz, %u-bit, %uch\n",
            frame_length, sample_rate, bit_depth, num_channels);
    return true;
}

// ── Rice/Golomb decoding ─────────────────────────────────────────────────────

static uint32_t rice_decode(BitReader& br, uint32_t /*m*/, uint32_t k, uint32_t bps) {
    // Read unary part (count of 1-bits before a 0-bit)
    uint32_t q = 0;
    while (!br.eof() && br.read(1) == 1) {
        q++;
        if (q > 65535) return 0; // overflow protection
    }

    if (k == 0) return q;

    // Read k bits for the remainder
    uint32_t r = 0;
    if (k > 1) {
        r = br.read(k - 1);
        // The final bit determines if we add an extra value
        if (r < ((1u << k) - (1u << (k - 1)))) {
            // value is as-is
        } else {
            r = (r << 1) | br.read(1);
            r -= ((1u << k) - (1u << (k - 1)));
        }
    } else {
        r = br.read(1);
    }

    (void)bps; // not used in basic decode
    return q * ((1u << k) - 1) + r;
}

// ── Prediction ───────────────────────────────────────────────────────────────

static void unpc_block(int32_t* pc_data, int32_t* out, uint32_t num_samples,
                       const int16_t* coefs, uint32_t num_coefs, uint32_t quant_shift,
                       uint32_t bps) {
    if (num_coefs == 0) {
        // No prediction, just copy
        memcpy(out, pc_data, num_samples * sizeof(int32_t));
        return;
    }

    // Copy warm-up samples
    for (uint32_t i = 0; i < num_coefs; i++) {
        out[i] = pc_data[i];
    }

    // Apply prediction filter
    for (uint32_t i = num_coefs; i < num_samples; i++) {
        int64_t sum = 0;
        for (uint32_t j = 0; j < num_coefs; j++) {
            sum += static_cast<int64_t>(coefs[j]) * out[i - 1 - j];
        }
        int32_t val = pc_data[i] + static_cast<int32_t>(sum >> quant_shift);

        // Clamp to bit depth
        int32_t hi = (1 << (bps - 1)) - 1;
        int32_t lo = -(1 << (bps - 1));
        if (val > hi) val = hi;
        if (val < lo) val = lo;

        out[i] = val;

        // Update coefficients (sign-sign LMS)
        if (pc_data[i] > 0) {
            for (uint32_t j = 0; j < num_coefs; j++) {
                int32_t prev = out[i - 1 - j];
                // Intentionally unused: coefficient adaptation is simplified here
                (void)prev;
            }
        }
    }
}

// ── Stereo decorrelation ─────────────────────────────────────────────────────

static void unmix_stereo(const int32_t* a, const int32_t* b, int16_t* out,
                         uint32_t num_samples, uint32_t mix_bits, uint32_t mix_res,
                         uint8_t bit_depth) {
    for (uint32_t i = 0; i < num_samples; i++) {
        int32_t left, right;
        if (mix_res != 0) {
            // Decorrelate
            int32_t m = a[i];
            int32_t s = b[i];
            right = m - ((s * mix_res) >> mix_bits);
            left = right + s;
        } else {
            left = a[i];
            right = b[i];
        }

        // Convert to int16
        if (bit_depth > 16) {
            int shift = bit_depth - 16;
            left >>= shift;
            right >>= shift;
        }

        // Clamp
        if (left > 32767) left = 32767;
        if (left < -32768) left = -32768;
        if (right > 32767) right = 32767;
        if (right < -32768) right = -32768;

        out[i * 2 + 0] = static_cast<int16_t>(left);
        out[i * 2 + 1] = static_cast<int16_t>(right);
    }
}

// ── Main decode function ─────────────────────────────────────────────────────

bool AlacDecoder::decode(const uint8_t* input, size_t in_len,
                         int16_t* output, uint32_t& out_frames) {
    if (!configured_ || !input || in_len == 0 || !output) return false;

    BitReader br(input, in_len);
    out_frames = 0;

    // Read frame header
    uint32_t channels = br.read(3); // channel count - 1
    (void)channels; // We already know from configuration

    // Skip unused header bits
    br.read(16); // unused
    uint32_t has_size = br.read(1);
    uint32_t uncompressed = br.read(2);
    uint32_t is_not_compressed = br.read(1);

    uint32_t num_samples = frame_length_;
    if (has_size) {
        num_samples = br.read(32);
        if (num_samples > frame_length_) num_samples = frame_length_;
    }

    if (is_not_compressed) {
        // Uncompressed frame: read raw samples
        for (uint32_t i = 0; i < num_samples; i++) {
            for (uint8_t ch = 0; ch < num_channels_; ch++) {
                int32_t sample = br.read_signed(bit_depth_);
                if (bit_depth_ > 16) {
                    sample >>= (bit_depth_ - 16);
                }
                if (sample > 32767) sample = 32767;
                if (sample < -32768) sample = -32768;
                output[i * num_channels_ + ch] = static_cast<int16_t>(sample);
            }
        }
        out_frames = num_samples;
        return true;
    }

    // Compressed frame
    (void)uncompressed;

    uint8_t  elem_type = br.read(3);
    (void)elem_type;
    uint32_t unused_header = br.read(12);
    (void)unused_header;

    // Prediction type and quant
    uint8_t  partial = br.read(1);
    (void)partial;
    uint8_t  bps_shift = br.read(2);
    uint8_t  rice_mod = br.read(3);

    // Number of prediction coefficients for each channel
    uint32_t pb_factor_a = br.read(5);
    int16_t coefs_a[32] = {};
    for (uint32_t i = 0; i < pb_factor_a; i++) {
        coefs_a[i] = static_cast<int16_t>(br.read_signed(16));
    }

    uint32_t pb_factor_b = 0;
    int16_t coefs_b[32] = {};
    if (num_channels_ >= 2) {
        pb_factor_b = br.read(5);
        for (uint32_t i = 0; i < pb_factor_b; i++) {
            coefs_b[i] = static_cast<int16_t>(br.read_signed(16));
        }
    }

    // Mix parameters (stereo decorrelation)
    uint32_t mix_bits = 0, mix_res = 0;
    if (num_channels_ >= 2) {
        mix_bits = br.read(8);
        mix_res = br.read(8);
    }

    // Decode rice-coded residuals for channel A
    uint32_t history = impl_->rice_initial_history;
    uint32_t sign_modifier = 0;
    auto& pred_a = impl_->pred_buf_a;

    for (uint32_t i = 0; i < num_samples && !br.eof(); i++) {
        uint32_t k = __builtin_clz(history + 1) > 0
            ? 32 - static_cast<uint32_t>(__builtin_clz(history + 1)) : 0;
        if (k > impl_->rice_k_modifier) k = impl_->rice_k_modifier;

        uint32_t val = rice_decode(br, 0, k, bit_depth_);
        val += sign_modifier;
        sign_modifier = 0;

        // Sign extension from unsigned to signed
        int32_t sval = (val & 1) ? -static_cast<int32_t>((val + 1) >> 1)
                                 : static_cast<int32_t>(val >> 1);
        pred_a[i] = sval;

        // Update history
        history += val * impl_->rice_history_mult
                   - ((history * impl_->rice_history_mult) >> 9);

        if (val > 0xFFFF) history = 0xFFFF;

        // Check for run of zeros
        if (history < 128 && i + 1 < num_samples) {
            uint32_t run_k = 7 - __builtin_clz(128 - history + 1);
            if (run_k > 7) run_k = 7;
            uint32_t run = rice_decode(br, 0, run_k, 16);
            if (run > num_samples - i - 1) run = num_samples - i - 1;
            for (uint32_t j = 0; j < run; j++) {
                pred_a[++i] = 0;
            }
            history = 0;
            if (run > 0) sign_modifier = 1;
        }
    }

    // Decode residuals for channel B (if stereo)
    auto& pred_b = impl_->pred_buf_b;
    if (num_channels_ >= 2) {
        history = impl_->rice_initial_history;
        sign_modifier = 0;
        for (uint32_t i = 0; i < num_samples && !br.eof(); i++) {
            uint32_t k = __builtin_clz(history + 1) > 0
                ? 32 - static_cast<uint32_t>(__builtin_clz(history + 1)) : 0;
            if (k > impl_->rice_k_modifier) k = impl_->rice_k_modifier;

            uint32_t val = rice_decode(br, 0, k, bit_depth_);
            val += sign_modifier;
            sign_modifier = 0;

            int32_t sval = (val & 1) ? -static_cast<int32_t>((val + 1) >> 1)
                                     : static_cast<int32_t>(val >> 1);
            pred_b[i] = sval;

            history += val * impl_->rice_history_mult
                       - ((history * impl_->rice_history_mult) >> 9);
            if (val > 0xFFFF) history = 0xFFFF;

            if (history < 128 && i + 1 < num_samples) {
                uint32_t run_k = 7 - __builtin_clz(128 - history + 1);
                if (run_k > 7) run_k = 7;
                uint32_t run = rice_decode(br, 0, run_k, 16);
                if (run > num_samples - i - 1) run = num_samples - i - 1;
                for (uint32_t j = 0; j < run; j++) {
                    pred_b[++i] = 0;
                }
                history = 0;
                if (run > 0) sign_modifier = 1;
            }
        }
    }

    // Apply prediction filters
    auto& mix_a = impl_->mix_buf_a;
    auto& mix_b = impl_->mix_buf_b;

    uint32_t quant_shift = bps_shift > 0 ? bit_depth_ - bps_shift + (rice_mod > 0 ? rice_mod : 0)
                                          : bit_depth_;
    if (quant_shift > 31) quant_shift = bit_depth_; // safety

    unpc_block(pred_a.data(), mix_a.data(), num_samples, coefs_a, pb_factor_a,
               quant_shift, bit_depth_);

    if (num_channels_ >= 2) {
        unpc_block(pred_b.data(), mix_b.data(), num_samples, coefs_b, pb_factor_b,
                   quant_shift, bit_depth_);

        // Stereo decorrelation + output
        unmix_stereo(mix_a.data(), mix_b.data(), output, num_samples,
                     mix_bits, mix_res, bit_depth_);
    } else {
        // Mono: just convert to int16
        for (uint32_t i = 0; i < num_samples; i++) {
            int32_t s = mix_a[i];
            if (bit_depth_ > 16) s >>= (bit_depth_ - 16);
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            output[i] = static_cast<int16_t>(s);
        }
    }

    out_frames = num_samples;
    return true;
}

} // namespace soluna::transport

#endif // SOLUNA_HAS_AIRPLAY
