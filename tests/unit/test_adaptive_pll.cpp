#include <soluna/sync/adaptive_pll.h>
#include <gtest/gtest.h>
#include <cmath>

using namespace soluna::sync;

TEST(AdaptivePll, InitialState) {
    AdaptivePll pll;
    EXPECT_FALSE(pll.state().locked);
    EXPECT_DOUBLE_EQ(pll.state().freq_offset_ppb, 0.0);
    EXPECT_EQ(pll.state().sample_count, 0u);
}

TEST(AdaptivePll, FirstSampleReturnsZero) {
    AdaptivePll pll;
    double adj = pll.feed(1000000, 1000000);
    EXPECT_DOUBLE_EQ(adj, 0.0);
    EXPECT_EQ(pll.state().sample_count, 1u);
}

TEST(AdaptivePll, PerfectClockNoAdjustment) {
    AdaptivePll pll;
    int64_t interval_ns = 1'000'000; // 1ms packets
    int64_t t = 0;

    // First sample
    pll.feed(t, t);

    // Feed perfectly aligned timestamps
    for (int i = 1; i <= 50; i++) {
        t += interval_ns;
        pll.feed(t, t);
    }

    // Should converge to near-zero adjustment
    EXPECT_LT(std::abs(pll.state().freq_offset_ppb), 1000.0);
}

TEST(AdaptivePll, DetectsFrequencyOffset) {
    AdaptivePll pll;
    int64_t local_t = 0;
    int64_t media_t = 0;
    int64_t interval_ns = 1'000'000;

    // First sample
    pll.feed(local_t, media_t);

    // Media clock runs 100ppm faster than local
    for (int i = 1; i <= 100; i++) {
        local_t += interval_ns;
        media_t += interval_ns + 100; // 100ns/ms = 100ppm
        pll.feed(local_t, media_t);
    }

    // PLL should detect positive frequency offset
    EXPECT_GT(pll.state().freq_offset_ppb, 0.0);
}

TEST(AdaptivePll, JitterWidensBandwidth) {
    AdaptivePllConfig config;
    config.initial_bandwidth_hz = 1.0;
    config.jitter_threshold_ns = 100000.0; // 100us
    AdaptivePll pll(config);

    int64_t t = 0;
    int64_t interval_ns = 1'000'000;

    pll.feed(t, t);

    // Feed with high jitter
    for (int i = 1; i <= 100; i++) {
        t += interval_ns;
        int64_t jitter = (i % 2 == 0) ? 500000 : -500000; // 500us jitter
        pll.feed(t + jitter, t);
    }

    // Bandwidth should have increased above initial
    EXPECT_GT(pll.state().bandwidth_hz, config.initial_bandwidth_hz);
}

TEST(AdaptivePll, Reset) {
    AdaptivePll pll;
    pll.feed(1000000, 1000000);
    pll.feed(2000000, 2000000);
    EXPECT_GT(pll.state().sample_count, 0u);

    pll.reset();
    EXPECT_EQ(pll.state().sample_count, 0u);
    EXPECT_DOUBLE_EQ(pll.state().freq_offset_ppb, 0.0);
}
