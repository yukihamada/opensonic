/**
 * WiFi Adaptation Controller Implementation
 * SPDX-License-Identifier: MIT
 */

#include <soluna/wifi/adaptation.h>
#include <algorithm>
#include <cmath>

namespace soluna::wifi {

AdaptationController::AdaptationController(const AdaptationConfig& config)
    : config_(config)
{
    state_.fec_mode = FecMode::None;
    state_.fec_parity = 1;
    state_.packet_tier = PacketTier::WiFi;
    state_.opus_bitrate = config.opus_bitrate_max;
    state_.jitter_buffer_depth_ms = 4.0;
}

AdaptationController::~AdaptationController() = default;

WiFiQualityLevel AdaptationController::classify(const WiFiQualityMetrics& m) const {
    // Use the worse of jitter or loss to determine quality level
    WiFiQualityLevel jitter_level;
    if (m.jitter_ms <= config_.jitter_excellent_ms)
        jitter_level = WiFiQualityLevel::Excellent;
    else if (m.jitter_ms <= config_.jitter_good_ms)
        jitter_level = WiFiQualityLevel::Good;
    else if (m.jitter_ms <= config_.jitter_fair_ms)
        jitter_level = WiFiQualityLevel::Fair;
    else if (m.jitter_ms <= config_.jitter_poor_ms)
        jitter_level = WiFiQualityLevel::Poor;
    else
        jitter_level = WiFiQualityLevel::Critical;

    WiFiQualityLevel loss_level;
    if (m.packet_loss_rate <= config_.loss_excellent)
        loss_level = WiFiQualityLevel::Excellent;
    else if (m.packet_loss_rate <= config_.loss_good)
        loss_level = WiFiQualityLevel::Good;
    else if (m.packet_loss_rate <= config_.loss_fair)
        loss_level = WiFiQualityLevel::Fair;
    else if (m.packet_loss_rate <= config_.loss_poor)
        loss_level = WiFiQualityLevel::Poor;
    else
        loss_level = WiFiQualityLevel::Critical;

    // Take the worse level
    return static_cast<WiFiQualityLevel>(
        std::max(static_cast<uint8_t>(jitter_level),
                 static_cast<uint8_t>(loss_level)));
}

bool AdaptationController::update(const WiFiQualityMetrics& metrics) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Update EMA statistics
    jitter_ema_ms_ = jitter_ema_ms_ * 0.9 + metrics.jitter_ms * 0.1;
    loss_ema_ = loss_ema_ * 0.95 + metrics.packet_loss_rate * 0.05;

    auto level = classify(metrics);
    bool changed = (level != state_.quality);
    state_.quality = level;

    if (config_.auto_fec) {
        adapt(level);
    }

    if (changed && callback_) {
        callback_(state_);
    }

    return changed;
}

void AdaptationController::record_loss(uint64_t lost, uint64_t total) {
    std::lock_guard<std::mutex> lock(mutex_);
    lost_packets_ += lost;
    total_packets_ += total;

    double rate = (total_packets_ > 0)
        ? static_cast<double>(lost_packets_) / total_packets_
        : 0.0;
    loss_ema_ = loss_ema_ * 0.9 + rate * 0.1;
}

void AdaptationController::record_jitter(double jitter_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    jitter_ema_ms_ = jitter_ema_ms_ * 0.9 + jitter_ms * 0.1;
}

void AdaptationController::adapt(WiFiQualityLevel level) {
    state_.fec_mode = select_fec_mode(level);
    state_.fec_parity = select_fec_parity(level);
    state_.jitter_buffer_depth_ms = select_jitter_depth(level);
    state_.opus_bitrate = select_opus_bitrate(level);
}

FecMode AdaptationController::select_fec_mode(WiFiQualityLevel level) const {
    switch (level) {
        case WiFiQualityLevel::Excellent:
            return FecMode::None;
        case WiFiQualityLevel::Good:
            return FecMode::XorParity;
        case WiFiQualityLevel::Fair:
            return FecMode::XorParity;
        case WiFiQualityLevel::Poor:
            return FecMode::ReedSolomon;
        case WiFiQualityLevel::Critical:
            return FecMode::ReedSolomon;
    }
    return FecMode::XorParity;
}

uint8_t AdaptationController::select_fec_parity(WiFiQualityLevel level) const {
    switch (level) {
        case WiFiQualityLevel::Excellent:
        case WiFiQualityLevel::Good:
            return 1;
        case WiFiQualityLevel::Fair:
            return 1;
        case WiFiQualityLevel::Poor:
            return 2;
        case WiFiQualityLevel::Critical:
            return std::min(config_.rs_parity_max, static_cast<uint8_t>(4));
    }
    return 1;
}

double AdaptationController::select_jitter_depth(WiFiQualityLevel level) const {
    // Jam mode: always recommend minimum depth
    if (config_.mode == StreamMode::Jam) {
        return 1.0;
    }

    switch (level) {
        case WiFiQualityLevel::Excellent: return 2.0;
        case WiFiQualityLevel::Good:      return 4.0;
        case WiFiQualityLevel::Fair:       return 6.0;
        case WiFiQualityLevel::Poor:       return 10.0;
        case WiFiQualityLevel::Critical:  return 15.0;
    }
    return 4.0;
}

uint32_t AdaptationController::select_opus_bitrate(WiFiQualityLevel level) const {
    if (!config_.auto_opus_bitrate) return state_.opus_bitrate;

    switch (level) {
        case WiFiQualityLevel::Excellent: return config_.opus_bitrate_max;
        case WiFiQualityLevel::Good:
            return (config_.opus_bitrate_max + config_.opus_bitrate_min) / 2
                   + config_.opus_bitrate_min / 2;
        case WiFiQualityLevel::Fair:
            return (config_.opus_bitrate_max + config_.opus_bitrate_min) / 2;
        case WiFiQualityLevel::Poor:
            return config_.opus_bitrate_min + (config_.opus_bitrate_max - config_.opus_bitrate_min) / 4;
        case WiFiQualityLevel::Critical:
            return config_.opus_bitrate_min;
    }
    return 96000;
}

AdaptationState AdaptationController::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

WiFiQualityLevel AdaptationController::quality_level() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.quality;
}

FecConfig AdaptationController::recommended_fec() const {
    std::lock_guard<std::mutex> lock(mutex_);
    FecConfig cfg;
    cfg.mode = state_.fec_mode;
    cfg.group_size = config_.fec_group_size;
    cfg.parity_count = state_.fec_parity;
    return cfg;
}

double AdaptationController::recommended_jitter_depth_ms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.jitter_buffer_depth_ms;
}

uint32_t AdaptationController::recommended_opus_bitrate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.opus_bitrate;
}

void AdaptationController::set_mode(StreamMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.mode = mode;
    if (mode == StreamMode::Jam) {
        // Jam mode: always recommend minimum depth for lowest latency
        state_.jitter_buffer_depth_ms = 1.0;
    } else {
        // Sync mode: revert to normal adaptive behavior
        state_.jitter_buffer_depth_ms = select_jitter_depth(state_.quality);
    }
    if (callback_) {
        callback_(state_);
    }
}

void AdaptationController::set_callback(AdaptationCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(cb);
}

void AdaptationController::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = AdaptationState{};
    jitter_ema_ms_ = 0.0;
    loss_ema_ = 0.0;
    total_packets_ = 0;
    lost_packets_ = 0;
    last_eval_ns_ = 0;
}

} // namespace soluna::wifi
