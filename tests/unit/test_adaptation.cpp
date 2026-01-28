/**
 * Unit tests for WiFi Adaptation Controller
 * SPDX-License-Identifier: MIT
 */

#include <soluna/wifi/adaptation.h>
#include <gtest/gtest.h>

using namespace soluna::wifi;

class AdaptationTest : public ::testing::Test {
protected:
    AdaptationConfig config;
};

TEST_F(AdaptationTest, InitialState) {
    AdaptationController ctrl(config);
    auto state = ctrl.state();
    EXPECT_EQ(state.quality, WiFiQualityLevel::Good);
}

TEST_F(AdaptationTest, ExcellentQuality) {
    AdaptationController ctrl(config);
    WiFiQualityMetrics m;
    m.jitter_ms = 0.5;
    m.packet_loss_rate = 0.0001;
    ctrl.update(m);

    EXPECT_EQ(ctrl.quality_level(), WiFiQualityLevel::Excellent);
    auto fec = ctrl.recommended_fec();
    EXPECT_EQ(fec.mode, FecMode::None);
}

TEST_F(AdaptationTest, GoodQuality) {
    AdaptationController ctrl(config);
    WiFiQualityMetrics m;
    m.jitter_ms = 2.0;
    m.packet_loss_rate = 0.005;
    ctrl.update(m);

    EXPECT_EQ(ctrl.quality_level(), WiFiQualityLevel::Good);
    auto fec = ctrl.recommended_fec();
    EXPECT_EQ(fec.mode, FecMode::XorParity);
}

TEST_F(AdaptationTest, FairQuality) {
    AdaptationController ctrl(config);
    WiFiQualityMetrics m;
    m.jitter_ms = 4.0;
    m.packet_loss_rate = 0.02;
    ctrl.update(m);

    EXPECT_EQ(ctrl.quality_level(), WiFiQualityLevel::Fair);
    EXPECT_GT(ctrl.recommended_jitter_depth_ms(), 4.0);
}

TEST_F(AdaptationTest, PoorQuality) {
    AdaptationController ctrl(config);
    WiFiQualityMetrics m;
    m.jitter_ms = 8.0;
    m.packet_loss_rate = 0.04;
    ctrl.update(m);

    EXPECT_EQ(ctrl.quality_level(), WiFiQualityLevel::Poor);
    auto fec = ctrl.recommended_fec();
    EXPECT_EQ(fec.mode, FecMode::ReedSolomon);
}

TEST_F(AdaptationTest, CriticalQuality) {
    AdaptationController ctrl(config);
    WiFiQualityMetrics m;
    m.jitter_ms = 15.0;
    m.packet_loss_rate = 0.08;
    ctrl.update(m);

    EXPECT_EQ(ctrl.quality_level(), WiFiQualityLevel::Critical);
    EXPECT_GE(ctrl.recommended_jitter_depth_ms(), 10.0);
    EXPECT_LE(ctrl.recommended_opus_bitrate(), config.opus_bitrate_min);
}

TEST_F(AdaptationTest, QualityCallback) {
    AdaptationController ctrl(config);
    bool called = false;
    WiFiQualityLevel reported_level;

    ctrl.set_callback([&](const AdaptationState& state) {
        called = true;
        reported_level = state.quality;
    });

    WiFiQualityMetrics m;
    m.jitter_ms = 15.0;
    m.packet_loss_rate = 0.1;
    ctrl.update(m);

    EXPECT_TRUE(called);
    EXPECT_EQ(reported_level, WiFiQualityLevel::Critical);
}

TEST_F(AdaptationTest, OpusBitrateAdaptation) {
    AdaptationController ctrl(config);

    WiFiQualityMetrics m;
    m.jitter_ms = 0.5;
    m.packet_loss_rate = 0.0;
    ctrl.update(m);
    uint32_t excellent_bitrate = ctrl.recommended_opus_bitrate();

    m.jitter_ms = 15.0;
    m.packet_loss_rate = 0.1;
    ctrl.update(m);
    uint32_t critical_bitrate = ctrl.recommended_opus_bitrate();

    EXPECT_GT(excellent_bitrate, critical_bitrate);
}

TEST_F(AdaptationTest, JitterBufferDepthScaling) {
    AdaptationController ctrl(config);

    WiFiQualityMetrics m;
    m.jitter_ms = 0.5;
    m.packet_loss_rate = 0.0;
    ctrl.update(m);
    double excellent_depth = ctrl.recommended_jitter_depth_ms();

    m.jitter_ms = 15.0;
    m.packet_loss_rate = 0.1;
    ctrl.update(m);
    double critical_depth = ctrl.recommended_jitter_depth_ms();

    EXPECT_LT(excellent_depth, critical_depth);
}

TEST_F(AdaptationTest, RecordLoss) {
    AdaptationController ctrl(config);
    ctrl.record_loss(5, 100);
    // No crash, internal state updated
    auto state = ctrl.state();
    (void)state;
}

TEST_F(AdaptationTest, RecordJitter) {
    AdaptationController ctrl(config);
    ctrl.record_jitter(3.5);
    ctrl.record_jitter(4.0);
    // No crash
    auto state = ctrl.state();
    (void)state;
}

TEST_F(AdaptationTest, Reset) {
    AdaptationController ctrl(config);

    WiFiQualityMetrics m;
    m.jitter_ms = 15.0;
    m.packet_loss_rate = 0.1;
    ctrl.update(m);

    ctrl.reset();
    auto state = ctrl.state();
    EXPECT_EQ(state.quality, WiFiQualityLevel::Good);
}
