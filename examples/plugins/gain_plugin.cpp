/**
 * Gain Plugin — Example DSP plugin for Soluna
 *
 * Applies a simple gain (in dB) to all audio channels.
 * Demonstrates the plugin C ABI.
 *
 * Build:
 *   g++ -shared -fPIC -o gain_plugin.so gain_plugin.cpp -I../../include
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/pipeline/dsp_plugin.h>
#include <cmath>

class GainPlugin : public soluna::pipeline::DspPlugin {
public:
    const char* name() const override { return "Gain"; }

    bool init(uint32_t /*sample_rate*/, uint32_t /*channels*/) override {
        return true;
    }

    void process(float* buffer, size_t frames, uint32_t channels) override {
        float linear = std::pow(10.0f, gain_db_ / 20.0f);
        size_t total = frames * channels;
        for (size_t i = 0; i < total; i++) {
            buffer[i] *= linear;
        }
    }

    void reset() override {}

    size_t param_count() const override { return 1; }

    const char* param_name(size_t index) const override {
        if (index == 0) return "gain_db";
        return "";
    }

    float param_value(size_t index) const override {
        if (index == 0) return gain_db_;
        return 0.0f;
    }

    void set_param(size_t index, float value) override {
        if (index == 0) gain_db_ = value;
    }

private:
    float gain_db_ = 0.0f;
};

extern "C" {

soluna::pipeline::DspPlugin* soluna_plugin_create() {
    return new GainPlugin();
}

void soluna_plugin_destroy(soluna::pipeline::DspPlugin* p) {
    delete p;
}

} // extern "C"
