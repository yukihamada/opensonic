/**
 * EQ Plugin — Built-in 3-band parametric equalizer for Soluna
 *
 * Three biquad filter bands using Robert Bristow-Johnson's Audio EQ Cookbook:
 *   Band 0: Low shelf  (default 200 Hz, 0 dB, Q=0.707)
 *   Band 1: Peaking    (default 1000 Hz, 0 dB, Q=1.0)
 *   Band 2: High shelf (default 5000 Hz, 0 dB, Q=0.707)
 *
 * Parameters (9 total, 3 per band):
 *   0: low_gain_db    (-15 to 15)
 *   1: low_freq_hz    (20 to 2000)
 *   2: low_q          (0.1 to 10)
 *   3: mid_gain_db    (-15 to 15)
 *   4: mid_freq_hz    (100 to 10000)
 *   5: mid_q          (0.1 to 10)
 *   6: high_gain_db   (-15 to 15)
 *   7: high_freq_hz   (1000 to 20000)
 *   8: high_q         (0.1 to 10)
 *
 * Each channel maintains independent filter state (up to 8 channels).
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/pipeline/dsp_plugin.h>
#include <cmath>
#include <algorithm>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace soluna {
namespace pipeline {

static constexpr size_t kMaxChannels = 8;
static constexpr size_t kNumBands = 3;

/**
 * Biquad filter coefficients and per-channel state.
 */
struct BiquadFilter {
    // Coefficients (normalized: a0 = 1)
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;

    // Per-channel delay state (Direct Form II Transposed)
    struct State {
        float z1 = 0.0f;
        float z2 = 0.0f;
    };
    State state[kMaxChannels] = {};

    void reset() {
        for (auto& s : state) {
            s.z1 = 0.0f;
            s.z2 = 0.0f;
        }
    }

    float process_sample(float in, uint32_t ch) {
        State& s = state[ch];
        float out = b0 * in + s.z1;
        s.z1 = b1 * in - a1 * out + s.z2;
        s.z2 = b2 * in - a2 * out;
        return out;
    }
};

/**
 * Band parameters.
 */
enum class BandType { LowShelf, Peaking, HighShelf };

struct BandParams {
    float gain_db = 0.0f;
    float freq_hz = 1000.0f;
    float q = 0.707f;
    BandType type = BandType::Peaking;
};

/**
 * Compute biquad coefficients from RBJ cookbook formulas.
 */
static void compute_biquad(BiquadFilter& f, const BandParams& p, uint32_t sr) {
    float A = std::pow(10.0f, p.gain_db / 40.0f);  // sqrt of linear gain
    float w0 = 2.0f * static_cast<float>(M_PI) * p.freq_hz / static_cast<float>(sr);
    float cos_w0 = std::cos(w0);
    float sin_w0 = std::sin(w0);
    float alpha = sin_w0 / (2.0f * p.q);

    float a0, a1, a2, b0, b1, b2;

    switch (p.type) {
        case BandType::LowShelf: {
            float sqA = std::sqrt(A);
            float two_sqA_alpha = 2.0f * sqA * alpha;
            b0 = A * ((A + 1.0f) - (A - 1.0f) * cos_w0 + two_sqA_alpha);
            b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cos_w0);
            b2 = A * ((A + 1.0f) - (A - 1.0f) * cos_w0 - two_sqA_alpha);
            a0 = (A + 1.0f) + (A - 1.0f) * cos_w0 + two_sqA_alpha;
            a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cos_w0);
            a2 = (A + 1.0f) + (A - 1.0f) * cos_w0 - two_sqA_alpha;
            break;
        }
        case BandType::HighShelf: {
            float sqA = std::sqrt(A);
            float two_sqA_alpha = 2.0f * sqA * alpha;
            b0 = A * ((A + 1.0f) + (A - 1.0f) * cos_w0 + two_sqA_alpha);
            b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cos_w0);
            b2 = A * ((A + 1.0f) + (A - 1.0f) * cos_w0 - two_sqA_alpha);
            a0 = (A + 1.0f) - (A - 1.0f) * cos_w0 + two_sqA_alpha;
            a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cos_w0);
            a2 = (A + 1.0f) - (A - 1.0f) * cos_w0 - two_sqA_alpha;
            break;
        }
        case BandType::Peaking:
        default: {
            b0 = 1.0f + alpha * A;
            b1 = -2.0f * cos_w0;
            b2 = 1.0f - alpha * A;
            a0 = 1.0f + alpha / A;
            a1 = -2.0f * cos_w0;
            a2 = 1.0f - alpha / A;
            break;
        }
    }

    // Normalize by a0
    float inv_a0 = 1.0f / a0;
    f.b0 = b0 * inv_a0;
    f.b1 = b1 * inv_a0;
    f.b2 = b2 * inv_a0;
    f.a1 = a1 * inv_a0;
    f.a2 = a2 * inv_a0;
}

class EqPlugin : public DspPlugin {
public:
    EqPlugin() {
        // Default band setup
        bands_[0] = { 0.0f, 200.0f, 0.707f, BandType::LowShelf };
        bands_[1] = { 0.0f, 1000.0f, 1.0f, BandType::Peaking };
        bands_[2] = { 0.0f, 5000.0f, 0.707f, BandType::HighShelf };
    }

    const char* name() const override { return "EQ"; }

    bool init(uint32_t sample_rate, uint32_t /*channels*/) override {
        sample_rate_ = sample_rate;
        update_all_coefficients();
        reset();
        return true;
    }

    void process(float* buffer, size_t frames, uint32_t channels) override {
        uint32_t ch = std::min(channels, static_cast<uint32_t>(kMaxChannels));

        for (size_t f = 0; f < frames; f++) {
            for (uint32_t c = 0; c < ch; c++) {
                float sample = buffer[f * channels + c];

                // Process through each band in series
                for (size_t b = 0; b < kNumBands; b++) {
                    sample = filters_[b].process_sample(sample, c);
                }

                buffer[f * channels + c] = sample;
            }
        }
    }

    void reset() override {
        for (auto& filt : filters_) {
            filt.reset();
        }
    }

    // 9 parameters: 3 bands x 3 params (gain_db, freq_hz, Q)
    size_t param_count() const override { return 9; }

    const char* param_name(size_t index) const override {
        static const char* names[] = {
            "low_gain_db", "low_freq_hz", "low_q",
            "mid_gain_db", "mid_freq_hz", "mid_q",
            "high_gain_db", "high_freq_hz", "high_q"
        };
        if (index < 9) return names[index];
        return "";
    }

    float param_value(size_t index) const override {
        size_t band = index / 3;
        size_t param = index % 3;
        if (band >= kNumBands) return 0.0f;

        switch (param) {
            case 0: return bands_[band].gain_db;
            case 1: return bands_[band].freq_hz;
            case 2: return bands_[band].q;
            default: return 0.0f;
        }
    }

    void set_param(size_t index, float value) override {
        size_t band = index / 3;
        size_t param = index % 3;
        if (band >= kNumBands) return;

        switch (param) {
            case 0:  // gain_db
                bands_[band].gain_db = std::max(-15.0f, std::min(15.0f, value));
                break;
            case 1:  // freq_hz
                bands_[band].freq_hz = std::max(20.0f, std::min(20000.0f, value));
                break;
            case 2:  // Q
                bands_[band].q = std::max(0.1f, std::min(10.0f, value));
                break;
            default:
                return;
        }
        compute_biquad(filters_[band], bands_[band], sample_rate_);
    }

private:
    void update_all_coefficients() {
        for (size_t b = 0; b < kNumBands; b++) {
            compute_biquad(filters_[b], bands_[b], sample_rate_);
        }
    }

    BandParams bands_[kNumBands];
    BiquadFilter filters_[kNumBands];
    uint32_t sample_rate_ = 48000;
};

std::unique_ptr<DspPlugin> create_eq() {
    return std::make_unique<EqPlugin>();
}

} // namespace pipeline
} // namespace soluna
