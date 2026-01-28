/**
 * Unit tests for RoutingMatrix
 * SPDX-License-Identifier: MIT
 */

#include <soluna/control/routing.h>
#include <gtest/gtest.h>
#include <cmath>
#include <vector>

using namespace soluna::control;

class RoutingMatrixTest : public ::testing::Test {
protected:
    RoutingMatrix matrix;
    ChannelId srcA{"devA", 1};
    ChannelId srcB{"devA", 2};
    ChannelId dstA{"devB", 1};
    ChannelId dstB{"devB", 2};
};

TEST_F(RoutingMatrixTest, AddRoute) {
    EXPECT_TRUE(matrix.add_route(srcA, dstA, 0.0f));
    EXPECT_EQ(matrix.route_count(), 1u);
    EXPECT_TRUE(matrix.has_route(srcA, dstA));
}

TEST_F(RoutingMatrixTest, AddDuplicateRoute) {
    EXPECT_TRUE(matrix.add_route(srcA, dstA));
    EXPECT_FALSE(matrix.add_route(srcA, dstA));
    EXPECT_EQ(matrix.route_count(), 1u);
}

TEST_F(RoutingMatrixTest, RemoveRoute) {
    matrix.add_route(srcA, dstA);
    EXPECT_TRUE(matrix.remove_route(srcA, dstA));
    EXPECT_FALSE(matrix.has_route(srcA, dstA));
    EXPECT_EQ(matrix.route_count(), 0u);
}

TEST_F(RoutingMatrixTest, RemoveNonexistent) {
    EXPECT_FALSE(matrix.remove_route(srcA, dstA));
}

TEST_F(RoutingMatrixTest, SetGain) {
    matrix.add_route(srcA, dstA, 0.0f);
    EXPECT_TRUE(matrix.set_gain(srcA, dstA, -6.0f));
    auto routes = matrix.list_routes();
    ASSERT_EQ(routes.size(), 1u);
    EXPECT_FLOAT_EQ(routes[0].gain_db, -6.0f);
}

TEST_F(RoutingMatrixTest, SetGainNonexistent) {
    EXPECT_FALSE(matrix.set_gain(srcA, dstA, -6.0f));
}

TEST_F(RoutingMatrixTest, SetMute) {
    matrix.add_route(srcA, dstA);
    EXPECT_TRUE(matrix.set_mute(srcA, dstA, true));
    auto routes = matrix.list_routes();
    ASSERT_EQ(routes.size(), 1u);
    EXPECT_TRUE(routes[0].muted);
}

TEST_F(RoutingMatrixTest, ListRoutes) {
    matrix.add_route(srcA, dstA);
    matrix.add_route(srcA, dstB);
    matrix.add_route(srcB, dstA, -3.0f);
    EXPECT_EQ(matrix.route_count(), 3u);
    auto routes = matrix.list_routes();
    EXPECT_EQ(routes.size(), 3u);
}

TEST_F(RoutingMatrixTest, GetSourceRoutes) {
    matrix.add_route(srcA, dstA);
    matrix.add_route(srcA, dstB);
    matrix.add_route(srcB, dstA);
    auto routes = matrix.get_source_routes(srcA);
    EXPECT_EQ(routes.size(), 2u);
}

TEST_F(RoutingMatrixTest, GetSinkRoutes) {
    matrix.add_route(srcA, dstA);
    matrix.add_route(srcB, dstA);
    auto routes = matrix.get_sink_routes(dstA);
    EXPECT_EQ(routes.size(), 2u);
}

TEST_F(RoutingMatrixTest, RemoveDeviceRoutes) {
    matrix.add_route(srcA, dstA);
    matrix.add_route(srcA, dstB);
    matrix.add_route(srcB, dstA);
    matrix.remove_device_routes("devA");
    EXPECT_EQ(matrix.route_count(), 0u);
}

TEST_F(RoutingMatrixTest, Clear) {
    matrix.add_route(srcA, dstA);
    matrix.add_route(srcB, dstB);
    matrix.clear();
    EXPECT_EQ(matrix.route_count(), 0u);
}

TEST_F(RoutingMatrixTest, ChangeCallback) {
    int add_count = 0;
    int remove_count = 0;
    matrix.set_change_callback([&](const Route&, bool added) {
        if (added) add_count++;
        else remove_count++;
    });
    matrix.add_route(srcA, dstA);
    matrix.remove_route(srcA, dstA);
    EXPECT_EQ(add_count, 1);
    EXPECT_EQ(remove_count, 1);
}

TEST_F(RoutingMatrixTest, ApplyMixing) {
    matrix.add_route(srcA, dstA, 0.0f);  // unity gain

    const size_t frames = 48;
    std::vector<float> src_buf(frames, 0.5f);
    std::vector<float> dst_buf(frames, 0.0f);

    std::map<ChannelId, const float*> sources = {{srcA, src_buf.data()}};
    std::map<ChannelId, float*> sinks = {{dstA, dst_buf.data()}};

    matrix.apply(sources, sinks, frames);

    for (size_t i = 0; i < frames; i++) {
        EXPECT_FLOAT_EQ(dst_buf[i], 0.5f);
    }
}

TEST_F(RoutingMatrixTest, ApplyMixingWithGain) {
    matrix.add_route(srcA, dstA, -6.0206f);  // ~0.5 linear

    const size_t frames = 48;
    std::vector<float> src_buf(frames, 1.0f);
    std::vector<float> dst_buf(frames, 0.0f);

    std::map<ChannelId, const float*> sources = {{srcA, src_buf.data()}};
    std::map<ChannelId, float*> sinks = {{dstA, dst_buf.data()}};

    matrix.apply(sources, sinks, frames);

    for (size_t i = 0; i < frames; i++) {
        EXPECT_NEAR(dst_buf[i], 0.5f, 0.001f);
    }
}

TEST_F(RoutingMatrixTest, ApplyMutedRoute) {
    matrix.add_route(srcA, dstA, 0.0f);
    matrix.set_mute(srcA, dstA, true);

    const size_t frames = 48;
    std::vector<float> src_buf(frames, 1.0f);
    std::vector<float> dst_buf(frames, 999.0f);

    std::map<ChannelId, const float*> sources = {{srcA, src_buf.data()}};
    std::map<ChannelId, float*> sinks = {{dstA, dst_buf.data()}};

    matrix.apply(sources, sinks, frames);

    for (size_t i = 0; i < frames; i++) {
        EXPECT_FLOAT_EQ(dst_buf[i], 0.0f);
    }
}

TEST_F(RoutingMatrixTest, ApplySumming) {
    // Two sources routed to same sink
    matrix.add_route(srcA, dstA, 0.0f);
    matrix.add_route(srcB, dstA, 0.0f);

    const size_t frames = 48;
    std::vector<float> src_a(frames, 0.3f);
    std::vector<float> src_b(frames, 0.2f);
    std::vector<float> dst_buf(frames, 0.0f);

    std::map<ChannelId, const float*> sources = {
        {srcA, src_a.data()}, {srcB, src_b.data()}
    };
    std::map<ChannelId, float*> sinks = {{dstA, dst_buf.data()}};

    matrix.apply(sources, sinks, frames);

    for (size_t i = 0; i < frames; i++) {
        EXPECT_NEAR(dst_buf[i], 0.5f, 0.0001f);
    }
}

TEST_F(RoutingMatrixTest, Metering) {
    // Generate test signal: 1kHz sine at 48kHz
    const size_t frames = 480;
    std::vector<float> data(frames);
    for (size_t i = 0; i < frames; i++) {
        data[i] = 0.5f * std::sin(2.0f * M_PI * 1000.0f * i / 48000.0f);
    }

    // Feed multiple blocks so EMA converges (rms_db uses 0.9/0.1 weighting)
    for (int n = 0; n < 100; n++) {
        matrix.update_meters(srcA, data.data(), frames);
    }
    auto meter = matrix.get_meter(srcA);

    EXPECT_GT(meter.peak_db, -10.0f);   // peak ~= -6dB
    EXPECT_LT(meter.peak_db, 0.0f);
    EXPECT_GT(meter.rms_db, -15.0f);
    EXPECT_LT(meter.rms_db, 0.0f);
    EXPECT_EQ(meter.clip_count, 0u);
}

TEST_F(RoutingMatrixTest, MeteringClip) {
    const size_t frames = 48;
    std::vector<float> data(frames, 1.5f);  // over 1.0 = clipping

    matrix.update_meters(srcA, data.data(), frames);
    auto meter = matrix.get_meter(srcA);
    EXPECT_GT(meter.clip_count, 0u);
}

TEST_F(RoutingMatrixTest, ResetMeters) {
    const size_t frames = 48;
    std::vector<float> data(frames, 0.5f);
    matrix.update_meters(srcA, data.data(), frames);
    matrix.reset_meters();
    auto meter = matrix.get_meter(srcA);
    EXPECT_FLOAT_EQ(meter.peak_db, -144.0f);
}

// ChannelId tests
TEST(ChannelIdTest, ParseAndToString) {
    auto ch = ChannelId::parse("myDevice:3");
    EXPECT_EQ(ch.device, "myDevice");
    EXPECT_EQ(ch.channel, 3u);
    EXPECT_EQ(ch.to_string(), "myDevice:3");
}

TEST(ChannelIdTest, Comparison) {
    ChannelId a{"dev1", 1};
    ChannelId b{"dev1", 2};
    ChannelId c{"dev2", 1};
    EXPECT_TRUE(a < b);
    EXPECT_TRUE(a < c);
    EXPECT_TRUE(a == a);
    EXPECT_FALSE(a == b);
}

// Route gain_linear test
TEST(RouteTest, GainLinear) {
    Route r;
    r.gain_db = 0.0f;
    r.muted = false;
    EXPECT_FLOAT_EQ(r.gain_linear(), 1.0f);

    r.gain_db = -20.0f;
    EXPECT_NEAR(r.gain_linear(), 0.1f, 0.001f);

    r.muted = true;
    EXPECT_FLOAT_EQ(r.gain_linear(), 0.0f);
}

// Crossfade test
TEST(CrossfadeTest, LinearCrossfade) {
    const size_t frames = 100;
    std::vector<float> old_buf(frames, 1.0f);
    std::vector<float> new_buf(frames, 0.0f);
    std::vector<float> output(frames);

    crossfade(old_buf.data(), new_buf.data(), output.data(), frames);

    EXPECT_NEAR(output[0], 1.0f, 0.02f);
    EXPECT_NEAR(output[frames / 2], 0.5f, 0.02f);
    EXPECT_NEAR(output[frames - 1], 0.01f, 0.02f);
}
