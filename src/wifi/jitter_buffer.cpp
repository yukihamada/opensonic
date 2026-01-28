/**
 * Adaptive Jitter Buffer Implementation
 * SPDX-License-Identifier: MIT
 */

#include <soluna/wifi/jitter_buffer.h>
#include <soluna/soluna.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace soluna::wifi {

JitterBuffer::JitterBuffer(const JitterBufferConfig& config)
    : config_(config)
    , target_depth_ms_(config.initial_depth_ms)
{
}

JitterBuffer::~JitterBuffer() = default;

int16_t JitterBuffer::seq_diff(uint16_t a, uint16_t b) {
    return static_cast<int16_t>(a - b);
}

bool JitterBuffer::is_too_old(uint16_t seq) const {
    if (!started_) return false;
    return seq_diff(next_sequence_, seq) > 0;
}

void JitterBuffer::push(uint16_t sequence, int64_t timestamp_ns,
                        const void* data, size_t data_size) {
    std::lock_guard<std::mutex> lock(mutex_);

    total_received_++;

    // Track inter-packet jitter (RFC 3550 style)
    if (prev_arrival_ns_ > 0) {
        double interval_ms = static_cast<double>(timestamp_ns - prev_arrival_ns_) / 1e6;
        // Expected interval based on packet rate
        double expected_ms = static_cast<double>(
            soluna::samples_per_packet(soluna::PacketTier::WiFi)) /
            config_.sample_rate * 1000.0;
        double deviation = std::abs(interval_ms - expected_ms);
        jitter_estimate_ms_ = jitter_estimate_ms_ * (1.0 - config_.jitter_smoothing)
                            + deviation * config_.jitter_smoothing;
        if (deviation > max_jitter_ms_) max_jitter_ms_ = deviation;
    }
    prev_arrival_ns_ = timestamp_ns;

    // Drop packets that are too old
    if (started_ && is_too_old(sequence)) {
        late_drops_++;
        return;
    }

    // Initialize on first packet
    if (!started_) {
        started_ = true;
        next_sequence_ = sequence;
        first_arrival_ns_ = timestamp_ns;
    }

    // Limit buffer size to prevent unbounded growth
    size_t max_packets = static_cast<size_t>(
        config_.max_depth_ms * config_.sample_rate /
        (soluna::samples_per_packet(soluna::PacketTier::WiFi) * 1000.0) * 2);
    if (packets_.size() >= max_packets) {
        overflow_drops_++;
        return;
    }

    Packet pkt;
    pkt.sequence = sequence;
    pkt.arrival_ns = timestamp_ns;
    pkt.data.resize(data_size);
    std::memcpy(pkt.data.data(), data, data_size);

    packets_[sequence] = std::move(pkt);
    last_arrival_ns_ = timestamp_ns;
}

size_t JitterBuffer::pop(void* out_buf, size_t buf_size) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!started_ || packets_.empty()) {
        underruns_++;
        adapt_depth();
        return 0;
    }

    // Wait until buffer has accumulated enough depth
    if (!ready()) {
        return 0;
    }

    auto it = packets_.find(next_sequence_);
    if (it == packets_.end()) {
        // Missing packet — count as loss and skip
        total_lost_++;
        total_expected_++;
        next_sequence_++;
        underruns_++;
        adapt_depth();
        return 0;
    }

    size_t copy_size = std::min(it->second.data.size(), buf_size);
    std::memcpy(out_buf, it->second.data.data(), copy_size);

    packets_.erase(it);
    next_sequence_++;
    total_played_++;
    total_expected_++;

    // Gradually decrease target depth on successful playout
    target_depth_ms_ = std::max(
        config_.min_depth_ms,
        target_depth_ms_ - config_.depth_decrease_rate);

    return copy_size;
}

bool JitterBuffer::ready() const {
    if (!started_ || packets_.empty()) return false;

    // Calculate current buffer depth in packets
    size_t count = packets_.size();
    double packet_duration_ms = static_cast<double>(
        soluna::samples_per_packet(soluna::PacketTier::WiFi)) /
        config_.sample_rate * 1000.0;
    double buffer_ms = count * packet_duration_ms;

    return buffer_ms >= target_depth_ms_;
}

void JitterBuffer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    packets_.clear();
    started_ = false;
    next_sequence_ = 0;
    first_arrival_ns_ = 0;
    last_arrival_ns_ = 0;
    target_depth_ms_ = config_.initial_depth_ms;
    jitter_estimate_ms_ = 0.0;
    max_jitter_ms_ = 0.0;
    total_received_ = 0;
    total_played_ = 0;
    late_drops_ = 0;
    overflow_drops_ = 0;
    underruns_ = 0;
    total_expected_ = 0;
    total_lost_ = 0;
    prev_arrival_ns_ = 0;
    prev_transit_ns_ = 0;
}

JitterBufferStats JitterBuffer::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    JitterBufferStats s;
    double packet_duration_ms = static_cast<double>(
        soluna::samples_per_packet(soluna::PacketTier::WiFi)) /
        config_.sample_rate * 1000.0;
    s.current_depth_ms = packets_.size() * packet_duration_ms;
    s.target_depth_ms = target_depth_ms_;
    s.jitter_ms = jitter_estimate_ms_;
    s.max_jitter_ms = max_jitter_ms_;
    s.packets_received = total_received_;
    s.packets_played = total_played_;
    s.packets_dropped_late = late_drops_;
    s.packets_dropped_overflow = overflow_drops_;
    s.underruns = underruns_;
    s.buffer_occupancy = static_cast<uint32_t>(packets_.size());
    s.packet_loss_rate = (total_expected_ > 0)
        ? static_cast<double>(total_lost_) / total_expected_
        : 0.0;
    return s;
}

double JitterBuffer::target_depth_ms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return target_depth_ms_;
}

void JitterBuffer::adapt_depth() {
    // Increase buffer depth on underrun
    target_depth_ms_ = std::min(
        config_.max_depth_ms,
        target_depth_ms_ * config_.depth_increase_factor);
}

} // namespace soluna::wifi
