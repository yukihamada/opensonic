/**
 * Unit tests for Adaptive Jitter Buffer
 * SPDX-License-Identifier: MIT
 */

#include <soluna/wifi/jitter_buffer.h>
#include <gtest/gtest.h>
#include <cstring>
#include <vector>

using namespace soluna::wifi;

class JitterBufferTest : public ::testing::Test {
protected:
    JitterBufferConfig config;

    void SetUp() override {
        config.sample_rate = 48000;
        config.channels = 1;
        config.frame_size = 4;
        config.min_depth_ms = 2.0;
        config.max_depth_ms = 20.0;
        config.initial_depth_ms = 4.0;
    }

    // Generate a fake audio packet (96 float samples = 2ms at 48kHz)
    std::vector<float> make_packet(float value = 0.5f) {
        std::vector<float> pkt(96, value);
        return pkt;
    }
};

TEST_F(JitterBufferTest, CreateAndReset) {
    JitterBuffer jb(config);
    auto stats = jb.stats();
    EXPECT_EQ(stats.packets_received, 0u);
    EXPECT_EQ(stats.buffer_occupancy, 0u);
    EXPECT_DOUBLE_EQ(stats.target_depth_ms, config.initial_depth_ms);
}

TEST_F(JitterBufferTest, PushPackets) {
    JitterBuffer jb(config);
    auto pkt = make_packet();

    for (uint16_t i = 0; i < 10; i++) {
        int64_t ts = i * 2000000LL; // 2ms intervals
        jb.push(i, ts, pkt.data(), pkt.size() * sizeof(float));
    }

    auto stats = jb.stats();
    EXPECT_EQ(stats.packets_received, 10u);
    EXPECT_EQ(stats.buffer_occupancy, 10u);
}

TEST_F(JitterBufferTest, PopInOrder) {
    JitterBuffer jb(config);
    auto pkt = make_packet(1.0f);

    // Push enough packets to fill the jitter buffer depth
    for (uint16_t i = 0; i < 10; i++) {
        int64_t ts = i * 2000000LL;
        jb.push(i, ts, pkt.data(), pkt.size() * sizeof(float));
    }

    // Should be able to pop once buffer is ready
    std::vector<float> out(96);
    size_t got = jb.pop(out.data(), out.size() * sizeof(float));
    EXPECT_GT(got, 0u);

    // Data should match
    EXPECT_FLOAT_EQ(out[0], 1.0f);
}

TEST_F(JitterBufferTest, OutOfOrderReorder) {
    JitterBuffer jb(config);
    auto pkt = make_packet();

    // Push packets out of order
    int64_t base_ts = 0;
    jb.push(0, base_ts, pkt.data(), pkt.size() * sizeof(float));
    jb.push(2, base_ts + 4000000LL, pkt.data(), pkt.size() * sizeof(float));
    jb.push(1, base_ts + 2000000LL, pkt.data(), pkt.size() * sizeof(float));
    jb.push(3, base_ts + 6000000LL, pkt.data(), pkt.size() * sizeof(float));
    jb.push(4, base_ts + 8000000LL, pkt.data(), pkt.size() * sizeof(float));

    auto stats = jb.stats();
    EXPECT_EQ(stats.packets_received, 5u);
    EXPECT_EQ(stats.buffer_occupancy, 5u);
}

TEST_F(JitterBufferTest, LatePacketDrop) {
    JitterBuffer jb(config);
    auto pkt = make_packet();

    // Push and consume some packets
    for (uint16_t i = 0; i < 10; i++) {
        jb.push(i, i * 2000000LL, pkt.data(), pkt.size() * sizeof(float));
    }

    std::vector<float> out(96);
    for (int i = 0; i < 5; i++) {
        jb.pop(out.data(), out.size() * sizeof(float));
    }

    // Push a late packet (seq=0, already consumed)
    jb.push(0, 0, pkt.data(), pkt.size() * sizeof(float));

    auto stats = jb.stats();
    EXPECT_GT(stats.packets_dropped_late, 0u);
}

TEST_F(JitterBufferTest, NotReadyUntilDepthMet) {
    config.initial_depth_ms = 4.0; // need 2 packets (each 2ms)
    JitterBuffer jb(config);
    auto pkt = make_packet();

    // Push just 1 packet (2ms) — not enough for 4ms depth
    jb.push(0, 0, pkt.data(), pkt.size() * sizeof(float));
    EXPECT_FALSE(jb.ready());

    // Push more packets to meet depth
    jb.push(1, 2000000LL, pkt.data(), pkt.size() * sizeof(float));
    jb.push(2, 4000000LL, pkt.data(), pkt.size() * sizeof(float));
    // 3 packets * 2ms = 6ms >= 4ms target
    EXPECT_TRUE(jb.ready());
}

TEST_F(JitterBufferTest, AdaptiveDepthIncrease) {
    config.initial_depth_ms = 2.0;
    config.depth_increase_factor = 2.0;
    JitterBuffer jb(config);

    double initial = jb.target_depth_ms();
    EXPECT_DOUBLE_EQ(initial, 2.0);

    // Pop from empty buffer triggers underrun → depth increase
    std::vector<float> out(96);
    jb.pop(out.data(), out.size() * sizeof(float));

    double after = jb.target_depth_ms();
    EXPECT_GT(after, initial);
}

TEST_F(JitterBufferTest, JitterEstimation) {
    JitterBuffer jb(config);
    auto pkt = make_packet();

    // Push with varying jitter
    jb.push(0, 0, pkt.data(), pkt.size() * sizeof(float));
    jb.push(1, 2000000LL, pkt.data(), pkt.size() * sizeof(float)); // normal
    jb.push(2, 5000000LL, pkt.data(), pkt.size() * sizeof(float)); // jittery
    jb.push(3, 7000000LL, pkt.data(), pkt.size() * sizeof(float)); // normal
    jb.push(4, 12000000LL, pkt.data(), pkt.size() * sizeof(float)); // jittery

    auto stats = jb.stats();
    EXPECT_GT(stats.jitter_ms, 0.0);
}

TEST_F(JitterBufferTest, Reset) {
    JitterBuffer jb(config);
    auto pkt = make_packet();

    for (uint16_t i = 0; i < 5; i++) {
        jb.push(i, i * 2000000LL, pkt.data(), pkt.size() * sizeof(float));
    }

    jb.reset();
    auto stats = jb.stats();
    EXPECT_EQ(stats.packets_received, 0u);
    EXPECT_EQ(stats.buffer_occupancy, 0u);
}

TEST_F(JitterBufferTest, PacketLossDetection) {
    config.initial_depth_ms = 2.0; // low depth so buffer is ready quickly
    JitterBuffer jb(config);
    auto pkt = make_packet();

    // Push enough to fill buffer
    for (uint16_t i = 0; i < 10; i++) {
        jb.push(i, i * 2000000LL, pkt.data(), pkt.size() * sizeof(float));
    }

    std::vector<float> out(96);
    // Pop all available
    int popped = 0;
    for (int i = 0; i < 10; i++) {
        size_t got = jb.pop(out.data(), out.size() * sizeof(float));
        if (got > 0) popped++;
    }

    // Now push with a gap (seq 10 missing, push 11-15)
    for (uint16_t i = 11; i <= 15; i++) {
        jb.push(i, i * 2000000LL, pkt.data(), pkt.size() * sizeof(float));
    }

    // Pop should encounter missing seq 10
    size_t got = jb.pop(out.data(), out.size() * sizeof(float));

    auto stats = jb.stats();
    // Either underrun or loss should be detected
    EXPECT_TRUE(stats.underruns > 0 || stats.packet_loss_rate > 0.0);
}
