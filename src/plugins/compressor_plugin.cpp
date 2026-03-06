/**
 * Compressor Plugin — Built-in dynamics compressor for Soluna
 *
 * Simple feed-forward compressor with peak envelope follower,
 * smoothed attack/release, and makeup gain.
 *
 * Parameters:
 *   0: threshold_db  (-60 to 0, default -20)
 *   1: ratio         (1 to 20, default 4)
 *   2: attack_ms     (0.1 to 100, default 10)
 *   3: release_ms    (1 to 1000, default 100)
 *   4: makeup_db     (0 to 40, default 0)
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/pipeline/dsp_plugin.h>
#include <cmath>
#include <algorithm>

namespace soluna {
namespace pipeline {

class CompressorPlugin : public DspPlugin {
public:
    const char* name() const override { return "Compressor"; }

    bool init(uint32_t sample_rate, uint32_t /*channels*/) override {
        sample_rate_ = sample_rate;
        envelope_ = 0.0f;
        update_coefficients();
        return true;
    }

    void process(float* buffer, size_t frames, uint32_t channels) override {
        const float thresh_lin = std::pow(10.0f, threshold_db_ / 20.0f);
        const float makeup_lin = std::pow(10.0f, makeup_db_ / 20.0f);

        for (size_t f = 0; f < frames; f++) {
            // Compute peak across all channels for this frame
            float peak = 0.0f;
            for (uint32_t c = 0; c < channels; c++) {
                float s = std::fabs(buffer[f * channels + c]);
                if (s > peak) peak = s;
            }

            // Envelope follower (attack/release smoothing)
            if (peak > envelope_) {
                envelope_ = attack_coeff_ * envelope_ +
                            (1.0f - attack_coeff_) * peak;
            } else {
                envelope_ = release_coeff_ * envelope_ +
                            (1.0f - release_coeff_) * peak;
            }

            // Compute gain reduction
            float gain = 1.0f;
            if (envelope_ > thresh_lin && envelope_ > 1e-10f) {
                float over_db = 20.0f * std::log10(envelope_ / thresh_lin);
                float reduction_db = over_db * (1.0f - 1.0f / ratio_);
                gain = std::pow(10.0f, -reduction_db / 20.0f);
            }
            gain *= makeup_lin;

            // Apply uniform gain to all channels
            for (uint32_t c = 0; c < channels; c++) {
                buffer[f * channels + c] *= gain;
            }
        }
    }

    void reset() override {
        envelope_ = 0.0f;
    }

    size_t param_count() const override { return 5; }

    const char* param_name(size_t index) const override {
        switch (index) {
            case 0: return "threshold_db";
            case 1: return "ratio";
            case 2: return "attack_ms";
            case 3: return "release_ms";
            case 4: return "makeup_db";
            default: return "";
        }
    }

    float param_value(size_t index) const override {
        switch (index) {
            case 0: return threshold_db_;
            case 1: return ratio_;
            case 2: return attack_ms_;
            case 3: return release_ms_;
            case 4: return makeup_db_;
            default: return 0.0f;
        }
    }

    void set_param(size_t index, float value) override {
        switch (index) {
            case 0:
                threshold_db_ = std::max(-60.0f, std::min(0.0f, value));
                break;
            case 1:
                ratio_ = std::max(1.0f, std::min(20.0f, value));
                break;
            case 2:
                attack_ms_ = std::max(0.1f, std::min(100.0f, value));
                update_coefficients();
                break;
            case 3:
                release_ms_ = std::max(1.0f, std::min(1000.0f, value));
                update_coefficients();
                break;
            case 4:
                makeup_db_ = std::max(0.0f, std::min(40.0f, value));
                break;
            default:
                break;
        }
    }

private:
    void update_coefficients() {
        if (sample_rate_ == 0) return;
        float attack_samples = sample_rate_ * attack_ms_ / 1000.0f;
        float release_samples = sample_rate_ * release_ms_ / 1000.0f;
        attack_coeff_ = std::exp(-1.0f / attack_samples);
        release_coeff_ = std::exp(-1.0f / release_samples);
    }

    float threshold_db_ = -20.0f;
    float ratio_ = 4.0f;
    float attack_ms_ = 10.0f;
    float release_ms_ = 100.0f;
    float makeup_db_ = 0.0f;

    float envelope_ = 0.0f;
    float attack_coeff_ = 0.0f;
    float release_coeff_ = 0.0f;
    uint32_t sample_rate_ = 48000;
};

std::unique_ptr<DspPlugin> create_compressor() {
    return std::make_unique<CompressorPlugin>();
}

} // namespace pipeline
} // namespace soluna
