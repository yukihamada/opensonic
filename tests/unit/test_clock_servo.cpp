#include <soluna/sync/clock_servo.h>
#include <gtest/gtest.h>
#include <cmath>

using namespace soluna::sync;

TEST(ClockServo, InitialState) {
    ClockServo servo;
    EXPECT_DOUBLE_EQ(servo.state().offset_ns, 0.0);
    EXPECT_DOUBLE_EQ(servo.state().freq_adj_ppb, 0.0);
    EXPECT_FALSE(servo.state().converged);
    EXPECT_EQ(servo.state().sample_count, 0);
}

TEST(ClockServo, PositiveOffset) {
    ClockServo servo;
    // Feed a positive offset (local ahead of master)
    double adj = servo.feed_offset(1000.0); // 1us offset
    // Should recommend negative frequency adjustment to slow down
    EXPECT_LT(adj, 0.0);
    EXPECT_GT(servo.state().offset_ns, 0.0);
}

TEST(ClockServo, NegativeOffset) {
    ClockServo servo;
    double adj = servo.feed_offset(-1000.0);
    // Should recommend positive adjustment to speed up
    EXPECT_GT(adj, 0.0);
}

TEST(ClockServo, ConvergesToZero) {
    ClockServoConfig cfg;
    cfg.kp = 0.7;
    cfg.ki = 0.3;
    cfg.filter_weight = 0.5;
    ClockServo servo(cfg);

    // Simulate convergence with a realistic feedback model:
    // The servo outputs freq_adj in ppb. Over 125ms sync interval,
    // the clock correction = freq_adj_ppb * interval_s * 1e9 ns
    double offset = 10000.0; // 10us initial
    double interval_s = 0.125; // 125ms sync interval

    for (int i = 0; i < 500; i++) {
        double adj = servo.feed_offset(offset);
        // Apply correction: ppb → ns over interval
        offset += adj * interval_s;
    }

    // After many iterations, should be much smaller than initial
    EXPECT_LT(std::abs(servo.state().offset_ns), 100.0);
}

TEST(ClockServo, FreqAdjClamped) {
    ClockServoConfig cfg;
    cfg.max_freq_adj_ppb = 1000.0;
    ClockServo servo(cfg);

    // Feed huge offset
    double adj = servo.feed_offset(1e9); // 1 second!
    EXPECT_LE(std::abs(adj), cfg.max_freq_adj_ppb);
}

TEST(ClockServo, StepDetection) {
    ClockServoConfig cfg;
    cfg.step_threshold_ns = 100'000'000; // 100ms
    ClockServo servo(cfg);

    // Small offset — no step
    EXPECT_EQ(servo.check_step(1000.0), 0);

    // Large offset — step
    int64_t step = servo.check_step(500'000'000.0);
    EXPECT_EQ(step, 500'000'000);
}

TEST(ClockServo, ConvergenceDetection) {
    ClockServoConfig cfg;
    cfg.converged_threshold_ns = 1000.0; // 1us
    cfg.filter_weight = 1.0; // no filtering
    ClockServo servo(cfg);

    // Feed sub-threshold offsets
    for (int i = 0; i < 15; i++) {
        servo.feed_offset(500.0);
    }
    EXPECT_TRUE(servo.state().converged);
}

TEST(ClockServo, PathDelay) {
    ClockServo servo;
    servo.feed_delay(50000.0); // 50us
    EXPECT_NEAR(servo.state().path_delay_ns, 50000.0, 1.0);

    // Filtered update
    servo.feed_delay(60000.0);
    // Should be between 50k and 60k
    EXPECT_GT(servo.state().path_delay_ns, 50000.0);
    EXPECT_LT(servo.state().path_delay_ns, 60000.0);
}

TEST(ClockServo, Reset) {
    ClockServo servo;
    servo.feed_offset(5000.0);
    servo.feed_delay(10000.0);
    EXPECT_NE(servo.state().sample_count, 0);

    servo.reset();
    EXPECT_EQ(servo.state().sample_count, 0);
    EXPECT_DOUBLE_EQ(servo.state().offset_ns, 0.0);
    EXPECT_DOUBLE_EQ(servo.state().path_delay_ns, 0.0);
}
