/**
 * Stress Test: 64-channel routing load test
 *
 * Verifies the routing matrix and pipeline can handle 64+ channels
 * with mix and gain operations under sustained load.
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/soluna.h>
#include <soluna/control/routing.h>
#include <soluna/pipeline/ring_buffer.h>
#include <gtest/gtest.h>
#include <vector>
#include <cmath>
#include <random>
#include <chrono>

using namespace soluna;
using namespace soluna::control;
using namespace soluna::pipeline;

class MultichannelStressTest : public ::testing::Test {
protected:
    RoutingMatrix routing_;
};

TEST_F(MultichannelStressTest, Route64Channels) {
    // Create 64 source → 64 sink routes
    for (uint32_t ch = 1; ch <= kMaxChannels; ch++) {
        ChannelId src{"src_dev", ch};
        ChannelId dst{"dst_dev", ch};
        ASSERT_TRUE(routing_.add_route(src, dst, 0.0f))
            << "Failed to add route for channel " << ch;
    }

    EXPECT_EQ(routing_.route_count(), kMaxChannels);
}

TEST_F(MultichannelStressTest, Route64ChannelsMixToOne) {
    // 64 sources → 1 sink (mixer scenario)
    ChannelId sink{"mixer", 1};
    for (uint32_t ch = 1; ch <= kMaxChannels; ch++) {
        ChannelId src{"input", ch};
        float gain_db = -6.0f; // each at -6dB
        ASSERT_TRUE(routing_.add_route(src, sink, gain_db));
    }

    EXPECT_EQ(routing_.route_count(), kMaxChannels);

    auto sink_routes = routing_.get_sink_routes(sink);
    EXPECT_EQ(sink_routes.size(), kMaxChannels);
}

TEST_F(MultichannelStressTest, ApplyRouting64Channels) {
    constexpr size_t kFrames = 256;

    // Create 64x64 routing (each src to matching dst)
    for (uint32_t ch = 1; ch <= kMaxChannels; ch++) {
        ChannelId src{"src", ch};
        ChannelId dst{"dst", ch};
        routing_.add_route(src, dst, 0.0f);
    }

    // Prepare source buffers with sine waves
    std::vector<std::vector<float>> src_bufs(kMaxChannels, std::vector<float>(kFrames));
    std::vector<std::vector<float>> dst_bufs(kMaxChannels, std::vector<float>(kFrames, 0.0f));

    std::map<ChannelId, const float*> sources;
    std::map<ChannelId, float*> sinks;

    for (uint32_t ch = 1; ch <= kMaxChannels; ch++) {
        float freq = 100.0f + ch * 10.0f;
        for (size_t i = 0; i < kFrames; i++) {
            src_bufs[ch - 1][i] = std::sin(2.0f * 3.14159f * freq * i / 48000.0f);
        }
        sources[ChannelId{"src", ch}] = src_bufs[ch - 1].data();
        sinks[ChannelId{"dst", ch}] = dst_bufs[ch - 1].data();
    }

    // Run 1000 iterations of routing apply
    auto start = std::chrono::steady_clock::now();
    for (int iter = 0; iter < 1000; iter++) {
        // Zero output
        for (auto& buf : dst_bufs) {
            std::fill(buf.begin(), buf.end(), 0.0f);
        }
        routing_.apply(sources, sinks, kFrames);
    }
    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // Verify output matches input (unity gain)
    for (uint32_t ch = 0; ch < kMaxChannels; ch++) {
        for (size_t i = 0; i < kFrames; i++) {
            ASSERT_NEAR(dst_bufs[ch][i], src_bufs[ch][i], 1e-6f)
                << "Mismatch at ch=" << ch << " frame=" << i;
        }
    }

    printf("64ch x 256 frames x 1000 iterations: %lld ms\n", (long long)ms);
}

TEST_F(MultichannelStressTest, RingBuffer64Channels) {
    constexpr size_t kFrames = 48;
    constexpr size_t kFrameSize = kMaxChannels * sizeof(float);

    RingBuffer ring(kFrames * 8, kFrameSize);

    std::vector<float> write_buf(kFrames * kMaxChannels);
    std::vector<float> read_buf(kFrames * kMaxChannels);

    // Fill with test pattern
    for (size_t i = 0; i < write_buf.size(); i++) {
        write_buf[i] = static_cast<float>(i) / write_buf.size();
    }

    // Sustained write/read cycles
    for (int cycle = 0; cycle < 10000; cycle++) {
        size_t written = ring.write(write_buf.data(), kFrames);
        ASSERT_EQ(written, kFrames) << "Write failed at cycle " << cycle;

        size_t read = ring.read(read_buf.data(), kFrames);
        ASSERT_EQ(read, kFrames) << "Read failed at cycle " << cycle;
    }

    // Verify last read matches write
    for (size_t i = 0; i < write_buf.size(); i++) {
        EXPECT_FLOAT_EQ(read_buf[i], write_buf[i]);
    }
}

TEST_F(MultichannelStressTest, GainChangesUnderLoad) {
    constexpr size_t kFrames = 48;
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> gain_dist(-60.0f, 12.0f);

    // Create 64 routes
    for (uint32_t ch = 1; ch <= kMaxChannels; ch++) {
        routing_.add_route(ChannelId{"src", ch}, ChannelId{"dst", ch}, 0.0f);
    }

    // Rapid gain changes
    for (int iter = 0; iter < 10000; iter++) {
        uint32_t ch = (rng() % kMaxChannels) + 1;
        float gain = gain_dist(rng);
        routing_.set_gain(ChannelId{"src", ch}, ChannelId{"dst", ch}, gain);
    }

    EXPECT_EQ(routing_.route_count(), kMaxChannels);
}
