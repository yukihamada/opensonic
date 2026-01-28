#include <soluna/pal/time.h>
#include <gtest/gtest.h>

using namespace soluna::pal;

TEST(Clock, MonotonicIncreases) {
    auto& clock = Clock::instance();
    auto t1 = clock.monotonic_now();
    auto t2 = clock.monotonic_now();

    EXPECT_GE(t2.to_ns(), t1.to_ns());
}

TEST(Clock, RealtimeReasonable) {
    auto& clock = Clock::instance();
    auto t = clock.realtime_now();

    // Should be after 2024-01-01 (sanity check)
    EXPECT_GT(t.seconds, 1704067200LL);
}

TEST(Clock, SleepNs) {
    auto& clock = Clock::instance();
    auto start = clock.monotonic_now();

    clock.sleep_ns(1'000'000); // 1ms

    auto end = clock.monotonic_now();
    int64_t elapsed = end.to_ns() - start.to_ns();

    // Should have slept at least ~900us (allowing for scheduling jitter)
    EXPECT_GT(elapsed, 500'000);
    // Should not have slept more than 50ms (generous upper bound)
    EXPECT_LT(elapsed, 50'000'000);
}

TEST(Timestamp, FromNs) {
    auto t = Timestamp::from_ns(1'500'000'000LL);
    EXPECT_EQ(t.seconds, 1);
    EXPECT_EQ(t.nanoseconds, 500'000'000);
}

TEST(Timestamp, FromNsNegative) {
    auto t = Timestamp::from_ns(-500'000'000LL);
    EXPECT_EQ(t.seconds, -1);
    EXPECT_EQ(t.nanoseconds, 500'000'000);
}

TEST(Timestamp, ToNsRoundtrip) {
    int64_t original = 123'456'789'012LL;
    auto t = Timestamp::from_ns(original);
    EXPECT_EQ(t.to_ns(), original);
}
