/**
 * Reverb Plugin — Built-in Schroeder reverb for Soluna
 *
 * Simple algorithmic reverb:
 *   4 parallel comb filters -> 2 series allpass filters
 *
 * Audio is summed to mono, processed through the reverb network,
 * then mixed back with the dry signal across all channels.
 *
 * Parameters:
 *   0: mix        (0 to 1, default 0.3)   — wet/dry ratio
 *   1: decay      (0.1 to 10, default 1.5) — RT60-like decay time in seconds
 *   2: pre_delay_ms (0 to 100, default 20)  — initial delay before reverb onset
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/pipeline/dsp_plugin.h>
#include <cmath>
#include <algorithm>
#include <vector>
#include <cstring>

namespace soluna {
namespace pipeline {

/**
 * Simple delay line with fixed maximum length.
 */
class DelayLine {
public:
    void resize(size_t max_samples) {
        buffer_.assign(max_samples, 0.0f);
        write_pos_ = 0;
        size_ = max_samples;
    }

    void clear() {
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        write_pos_ = 0;
    }

    void write(float sample) {
        if (size_ == 0) return;
        buffer_[write_pos_] = sample;
        write_pos_ = (write_pos_ + 1) % size_;
    }

    float read(size_t delay) const {
        if (size_ == 0 || delay == 0) return 0.0f;
        delay = std::min(delay, size_);
        size_t pos = (write_pos_ + size_ - delay) % size_;
        return buffer_[pos];
    }

    float read_and_write(float input, size_t delay) {
        float out = read(delay);
        write(input);
        return out;
    }

private:
    std::vector<float> buffer_;
    size_t write_pos_ = 0;
    size_t size_ = 0;
};

/**
 * Comb filter: y[n] = x[n-D] + g * y[n-D]
 *
 * Feedback delay line.
 */
struct CombFilter {
    DelayLine delay;
    size_t delay_samples = 0;
    float feedback = 0.0f;

    void init(size_t max_samples) {
        delay.resize(max_samples);
    }

    void clear() {
        delay.clear();
    }

    float process(float input) {
        float delayed = delay.read(delay_samples);
        float output = delayed;
        delay.write(input + feedback * delayed);
        return output;
    }
};

/**
 * Allpass filter: y[n] = -g*x[n] + x[n-D] + g*y[n-D]
 *
 * Schroeder allpass structure.
 */
struct AllpassFilter {
    DelayLine delay;
    size_t delay_samples = 0;
    float gain = 0.0f;

    void init(size_t max_samples) {
        delay.resize(max_samples);
    }

    void clear() {
        delay.clear();
    }

    float process(float input) {
        float delayed = delay.read(delay_samples);
        float output = -gain * input + delayed;
        delay.write(input + gain * delayed);
        return output;
    }
};

class ReverbPlugin : public DspPlugin {
public:
    const char* name() const override { return "Reverb"; }

    bool init(uint32_t sample_rate, uint32_t /*channels*/) override {
        sample_rate_ = sample_rate;

        // Maximum delay: 10s decay + 100ms pre-delay
        size_t max_comb = static_cast<size_t>(sample_rate * 0.1f);  // ~100ms per comb
        size_t max_allpass = static_cast<size_t>(sample_rate * 0.02f); // ~20ms per allpass
        size_t max_pre = static_cast<size_t>(sample_rate * 0.1f);  // 100ms max pre-delay

        for (auto& c : combs_) c.init(max_comb);
        for (auto& a : allpasses_) a.init(max_allpass);
        pre_delay_.resize(max_pre);

        update_parameters();
        return true;
    }

    void process(float* buffer, size_t frames, uint32_t channels) override {
        for (size_t f = 0; f < frames; f++) {
            // Sum to mono
            float mono = 0.0f;
            for (uint32_t c = 0; c < channels; c++) {
                mono += buffer[f * channels + c];
            }
            mono /= static_cast<float>(channels);

            // Pre-delay
            float delayed = pre_delay_.read_and_write(mono, pre_delay_samples_);

            // 4 parallel comb filters, summed
            float comb_sum = 0.0f;
            for (auto& c : combs_) {
                comb_sum += c.process(delayed);
            }
            comb_sum *= 0.25f;  // Average the 4 combs

            // 2 series allpass filters
            float wet = comb_sum;
            for (auto& a : allpasses_) {
                wet = a.process(wet);
            }

            // Mix wet/dry and apply to all channels
            for (uint32_t c = 0; c < channels; c++) {
                float dry = buffer[f * channels + c];
                buffer[f * channels + c] = dry * (1.0f - mix_) + wet * mix_;
            }
        }
    }

    void reset() override {
        for (auto& c : combs_) c.clear();
        for (auto& a : allpasses_) a.clear();
        pre_delay_.clear();
    }

    size_t param_count() const override { return 3; }

    const char* param_name(size_t index) const override {
        switch (index) {
            case 0: return "mix";
            case 1: return "decay";
            case 2: return "pre_delay_ms";
            default: return "";
        }
    }

    float param_value(size_t index) const override {
        switch (index) {
            case 0: return mix_;
            case 1: return decay_;
            case 2: return pre_delay_ms_;
            default: return 0.0f;
        }
    }

    void set_param(size_t index, float value) override {
        switch (index) {
            case 0:
                mix_ = std::max(0.0f, std::min(1.0f, value));
                break;
            case 1:
                decay_ = std::max(0.1f, std::min(10.0f, value));
                update_parameters();
                break;
            case 2:
                pre_delay_ms_ = std::max(0.0f, std::min(100.0f, value));
                pre_delay_samples_ = static_cast<size_t>(
                    sample_rate_ * pre_delay_ms_ / 1000.0f);
                break;
            default:
                break;
        }
    }

private:
    void update_parameters() {
        // Schroeder comb filter delay times (in ms) — mutually prime ratios
        // to avoid metallic resonances
        static const float comb_delays_ms[4] = { 29.7f, 37.1f, 41.1f, 43.7f };
        static const float allpass_delays_ms[2] = { 5.0f, 1.7f };

        // Compute comb feedback from decay time
        // g = 10^(-3 * D / (decay * sr))  where D = delay in samples
        for (size_t i = 0; i < 4; i++) {
            size_t d = static_cast<size_t>(sample_rate_ * comb_delays_ms[i] / 1000.0f);
            combs_[i].delay_samples = d;
            if (decay_ > 0.0f && d > 0) {
                combs_[i].feedback = std::pow(10.0f,
                    -3.0f * static_cast<float>(d) /
                    (decay_ * static_cast<float>(sample_rate_)));
            } else {
                combs_[i].feedback = 0.0f;
            }
        }

        // Allpass coefficients (fixed gain, variable delay)
        for (size_t i = 0; i < 2; i++) {
            allpasses_[i].delay_samples = static_cast<size_t>(
                sample_rate_ * allpass_delays_ms[i] / 1000.0f);
            allpasses_[i].gain = 0.7f;
        }

        pre_delay_samples_ = static_cast<size_t>(
            sample_rate_ * pre_delay_ms_ / 1000.0f);
    }

    float mix_ = 0.3f;
    float decay_ = 1.5f;
    float pre_delay_ms_ = 20.0f;
    size_t pre_delay_samples_ = 0;

    CombFilter combs_[4];
    AllpassFilter allpasses_[2];
    DelayLine pre_delay_;
    uint32_t sample_rate_ = 48000;
};

std::unique_ptr<DspPlugin> create_reverb() {
    return std::make_unique<ReverbPlugin>();
}

} // namespace pipeline
} // namespace soluna
