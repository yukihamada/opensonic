/**
 * DSP Plugin API Tests
 *
 * Tests plugin host loading, process chain, and parameter control.
 * Uses the built-in gain plugin for integration testing.
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/pipeline/dsp_plugin.h>
#include <gtest/gtest.h>
#include <cmath>
#include <vector>

using namespace soluna::pipeline;

// --- Mock Plugin (no shared library needed) ---

class TestPlugin : public DspPlugin {
public:
    const char* name() const override { return "TestPlugin"; }

    bool init(uint32_t sr, uint32_t ch) override {
        sample_rate_ = sr;
        channels_ = ch;
        initialized_ = true;
        return true;
    }

    void process(float* buffer, size_t frames, uint32_t channels) override {
        // Simple: multiply all samples by 2
        size_t total = frames * channels;
        for (size_t i = 0; i < total; i++) {
            buffer[i] *= 2.0f;
        }
        process_count_++;
    }

    void reset() override { process_count_ = 0; }

    size_t param_count() const override { return 1; }
    const char* param_name(size_t idx) const override {
        return idx == 0 ? "scale" : "";
    }
    float param_value(size_t idx) const override {
        return idx == 0 ? 2.0f : 0.0f;
    }
    void set_param(size_t, float) override {}

    bool initialized_ = false;
    uint32_t sample_rate_ = 0;
    uint32_t channels_ = 0;
    int process_count_ = 0;
};

// --- Plugin Interface Tests ---

TEST(DspPlugin, PluginName) {
    TestPlugin p;
    EXPECT_STREQ(p.name(), "TestPlugin");
}

TEST(DspPlugin, PluginInit) {
    TestPlugin p;
    EXPECT_TRUE(p.init(48000, 2));
    EXPECT_TRUE(p.initialized_);
    EXPECT_EQ(p.sample_rate_, 48000u);
    EXPECT_EQ(p.channels_, 2u);
}

TEST(DspPlugin, PluginProcess) {
    TestPlugin p;
    p.init(48000, 1);

    std::vector<float> buf = {0.5f, -0.5f, 0.25f, -0.25f};
    p.process(buf.data(), 4, 1);

    EXPECT_FLOAT_EQ(buf[0], 1.0f);
    EXPECT_FLOAT_EQ(buf[1], -1.0f);
    EXPECT_FLOAT_EQ(buf[2], 0.5f);
    EXPECT_FLOAT_EQ(buf[3], -0.5f);
    EXPECT_EQ(p.process_count_, 1);
}

TEST(DspPlugin, PluginReset) {
    TestPlugin p;
    p.process_count_ = 42;
    p.reset();
    EXPECT_EQ(p.process_count_, 0);
}

TEST(DspPlugin, PluginParams) {
    TestPlugin p;
    EXPECT_EQ(p.param_count(), 1u);
    EXPECT_STREQ(p.param_name(0), "scale");
    EXPECT_FLOAT_EQ(p.param_value(0), 2.0f);
}

// --- PluginHost Tests (without shared library loading) ---

TEST(PluginHost, EmptyHost) {
    PluginHost host;
    EXPECT_EQ(host.plugin_count(), 0u);
    EXPECT_EQ(host.plugin(0), nullptr);
}

TEST(PluginHost, InitEmptyHost) {
    PluginHost host;
    EXPECT_TRUE(host.init_all(48000, 2));
}

TEST(PluginHost, ProcessEmptyHost) {
    PluginHost host;
    std::vector<float> buf = {1.0f, 2.0f};
    host.process_all(buf.data(), 2, 1);
    // Should be unchanged
    EXPECT_FLOAT_EQ(buf[0], 1.0f);
    EXPECT_FLOAT_EQ(buf[1], 2.0f);
}

TEST(PluginHost, LoadNonexistent) {
    PluginHost host;
    EXPECT_FALSE(host.load("/nonexistent/path/plugin.so"));
    EXPECT_EQ(host.plugin_count(), 0u);
}

TEST(PluginHost, UnloadAll) {
    PluginHost host;
    host.unload_all(); // should not crash
    EXPECT_EQ(host.plugin_count(), 0u);
}

// --- Chain processing test (simulated) ---

TEST(DspPlugin, ChainProcessing) {
    // Simulate two plugins in chain: each doubles the signal
    TestPlugin p1, p2;
    p1.init(48000, 1);
    p2.init(48000, 1);

    std::vector<float> buf = {0.1f, 0.2f, 0.3f, 0.4f};

    p1.process(buf.data(), 4, 1);
    p2.process(buf.data(), 4, 1);

    // Each plugin doubles: 0.1 * 2 * 2 = 0.4
    EXPECT_FLOAT_EQ(buf[0], 0.4f);
    EXPECT_FLOAT_EQ(buf[1], 0.8f);
    EXPECT_FLOAT_EQ(buf[2], 1.2f);
    EXPECT_FLOAT_EQ(buf[3], 1.6f);
}

TEST(DspPlugin, StereoProcess) {
    TestPlugin p;
    p.init(48000, 2);

    // Interleaved stereo: L R L R
    std::vector<float> buf = {0.5f, -0.5f, 0.25f, -0.25f};
    p.process(buf.data(), 2, 2); // 2 frames, 2 channels

    EXPECT_FLOAT_EQ(buf[0], 1.0f);   // L
    EXPECT_FLOAT_EQ(buf[1], -1.0f);  // R
    EXPECT_FLOAT_EQ(buf[2], 0.5f);   // L
    EXPECT_FLOAT_EQ(buf[3], -0.5f);  // R
}
