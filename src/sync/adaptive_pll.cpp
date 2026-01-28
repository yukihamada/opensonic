/**
 * Adaptive PLL — Media clock recovery
 * SPDX-License-Identifier: MIT
 */

#include <soluna/sync/adaptive_pll.h>
#include <algorithm>
#include <cmath>

namespace soluna::sync {

AdaptivePll::AdaptivePll(const AdaptivePllConfig& config)
    : config_(config)
{
    state_.bandwidth_hz = config_.initial_bandwidth_hz;
}

double AdaptivePll::feed(int64_t local_ns, int64_t media_ns) {
    state_.sample_count++;

    if (first_sample_) {
        last_local_ns_ = local_ns;
        last_media_ns_ = media_ns;
        first_sample_ = false;
        return 0.0;
    }

    // Compute expected vs actual arrival interval
    int64_t local_delta = local_ns - last_local_ns_;
    int64_t media_delta = media_ns - last_media_ns_;

    if (local_delta <= 0 || media_delta <= 0) {
        return state_.freq_offset_ppb;
    }

    // Phase error: difference between media clock and local clock progression
    double phase_error = static_cast<double>(media_delta - local_delta);

    // Update jitter estimate (exponential moving average of |phase_error|)
    double abs_error = std::abs(phase_error);
    if (state_.sample_count <= 2) {
        state_.jitter_estimate_ns = abs_error;
    } else {
        state_.jitter_estimate_ns = 0.05 * abs_error + 0.95 * state_.jitter_estimate_ns;
    }

    // Adaptive bandwidth: widen when jitter is high, narrow when stable
    if (state_.jitter_estimate_ns > config_.jitter_threshold_ns) {
        state_.bandwidth_hz = std::min(state_.bandwidth_hz * 1.01, config_.max_bandwidth_hz);
    } else {
        state_.bandwidth_hz = std::max(state_.bandwidth_hz * 0.999, config_.min_bandwidth_hz);
    }

    // 2nd-order PLL loop filter
    // omega_n = 2*pi*bandwidth
    double omega_n = 2.0 * M_PI * state_.bandwidth_hz;
    double damping = 0.707; // critically damped

    // PI gains derived from natural frequency
    double kp = 2.0 * damping * omega_n;
    double ki = omega_n * omega_n;

    // Time step (normalize to seconds)
    double dt = static_cast<double>(local_delta) / 1e9;

    // Phase error in ns
    state_.phase_error_ns = phase_error;

    // Frequency adjustment (ppb)
    state_.freq_offset_ppb += kp * phase_error + ki * phase_error * dt;

    // Clamp to reasonable range
    state_.freq_offset_ppb = std::clamp(state_.freq_offset_ppb, -500000.0, 500000.0);

    // Lock detection
    state_.locked = (state_.jitter_estimate_ns < config_.jitter_threshold_ns &&
                     std::abs(state_.freq_offset_ppb) < 50000.0);

    last_local_ns_ = local_ns;
    last_media_ns_ = media_ns;

    return state_.freq_offset_ppb;
}

void AdaptivePll::reset() {
    state_ = {};
    state_.bandwidth_hz = config_.initial_bandwidth_hz;
    first_sample_ = true;
    last_local_ns_ = 0;
    last_media_ns_ = 0;
}

} // namespace soluna::sync
