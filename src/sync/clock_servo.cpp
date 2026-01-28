/**
 * Clock Servo — PI Controller Implementation
 * SPDX-License-Identifier: MIT
 */

#include <soluna/sync/clock_servo.h>
#include <algorithm>
#include <cmath>

namespace soluna::sync {

ClockServo::ClockServo(const ClockServoConfig& config)
    : config_(config)
{
}

double ClockServo::feed_offset(double offset_ns) {
    state_.raw_offset_ns = offset_ns;
    state_.sample_count++;

    // Exponential moving average filter
    if (state_.sample_count == 1) {
        state_.offset_ns = offset_ns;
    } else {
        state_.offset_ns = config_.filter_weight * offset_ns +
                           (1.0 - config_.filter_weight) * state_.offset_ns;
    }

    // PI controller
    double p_term = config_.kp * state_.offset_ns;
    state_.integral += config_.ki * state_.offset_ns;

    // Anti-windup: clamp integral
    double max_integral = config_.max_freq_adj_ppb / config_.ki;
    state_.integral = std::clamp(state_.integral, -max_integral, max_integral);

    double i_term = state_.integral;
    state_.freq_adj_ppb = -(p_term + i_term);

    // Clamp output
    state_.freq_adj_ppb = std::clamp(state_.freq_adj_ppb,
        -config_.max_freq_adj_ppb, config_.max_freq_adj_ppb);

    // Convergence detection
    if (std::abs(state_.offset_ns) < config_.converged_threshold_ns) {
        state_.converged_count++;
        if (state_.converged_count >= 10) {
            state_.converged = true;
        }
    } else {
        state_.converged_count = 0;
        state_.converged = false;
    }

    return state_.freq_adj_ppb;
}

void ClockServo::feed_delay(double delay_ns) {
    // Simple exponential filter on path delay
    if (state_.path_delay_ns == 0.0) {
        state_.path_delay_ns = delay_ns;
    } else {
        state_.path_delay_ns = 0.1 * delay_ns + 0.9 * state_.path_delay_ns;
    }
}

int64_t ClockServo::check_step(double offset_ns) const {
    if (std::abs(offset_ns) > static_cast<double>(config_.step_threshold_ns)) {
        return static_cast<int64_t>(offset_ns);
    }
    return 0;
}

void ClockServo::reset() {
    state_ = {};
}

} // namespace soluna::sync
