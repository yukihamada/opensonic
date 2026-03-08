/**
 * Lock-free Jitter Buffer Implementation
 *
 * Fixed-size circular slot array indexed by sequence number.
 * No mutex, no heap allocation on push/pop paths.
 *
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
    // All slots start unoccupied (default-initialized atomics)

    // Apply Jam mode defaults if configured
    if (config.mode == StreamMode::Jam) {
        set_mode(StreamMode::Jam);
    }
}

JitterBuffer::~JitterBuffer() = default;

int16_t JitterBuffer::seq_diff(uint16_t a, uint16_t b) {
    return static_cast<int16_t>(a - b);
}

void JitterBuffer::push(uint16_t sequence, int64_t timestamp_ns,
                        const void* data, size_t data_size) {
    total_received_.fetch_add(1, std::memory_order_relaxed);

    // Track inter-packet jitter (RFC 3550 style)
    if (prev_arrival_ns_ > 0) {
        double interval_ms = static_cast<double>(timestamp_ns - prev_arrival_ns_) / 1e6;
        double expected_ms = static_cast<double>(
            soluna::samples_per_packet(soluna::PacketTier::WiFi)) /
            config_.sample_rate * 1000.0;
        double deviation = std::abs(interval_ms - expected_ms);
        jitter_estimate_ms_ = jitter_estimate_ms_ * (1.0 - config_.jitter_smoothing)
                            + deviation * config_.jitter_smoothing;
        if (deviation > max_jitter_ms_) max_jitter_ms_ = deviation;
    }
    prev_arrival_ns_ = timestamp_ns;

    // Drop packets that are too old (behind read pointer)
    if (started_ && seq_diff(next_sequence_, sequence) > 0) {
        late_drops_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Clamp payload to slot capacity
    if (data_size > sizeof(Slot::data)) {
        data_size = sizeof(Slot::data);
    }

    // Initialize on first packet
    if (!started_) {
        started_ = true;
        next_sequence_ = sequence;
    }

    // Write into slot indexed by sequence
    size_t idx = sequence & kSlotMask;
    Slot& slot = slots_[idx];

    // If slot is already occupied by a different packet, it's an overflow
    if (slot.occupied.load(std::memory_order_acquire)) {
        if (slot.sequence != sequence) {
            overflow_drops_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Same sequence = duplicate, ignore
        return;
    }

    slot.sequence = sequence;
    slot.data_size = data_size;
    std::memcpy(slot.data, data, data_size);
    slot.occupied.store(true, std::memory_order_release);
    occupancy_.fetch_add(1, std::memory_order_relaxed);
}

size_t JitterBuffer::pop(void* out_buf, size_t buf_size) {
    if (!started_) {
        underruns_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    // Wait until buffer has accumulated enough depth
    if (!ready()) {
        // Check if next_sequence_ is a gap (missing packet with future
        // packets present). Without this, the buffer stalls permanently
        // when a packet is lost.
        size_t idx0 = next_sequence_ & kSlotMask;
        Slot& s0 = slots_[idx0];
        if (!s0.occupied.load(std::memory_order_acquire) ||
            s0.sequence != next_sequence_) {
            for (size_t i = 1; i <= 16; i++) {
                size_t fi = (next_sequence_ + i) & kSlotMask;
                if (slots_[fi].occupied.load(std::memory_order_relaxed) &&
                    slots_[fi].sequence == static_cast<uint16_t>(next_sequence_ + i)) {
                    // Future packet exists → skip the missing one
                    total_lost_.fetch_add(1, std::memory_order_relaxed);
                    total_expected_.fetch_add(1, std::memory_order_relaxed);
                    next_sequence_++;
                    underruns_.fetch_add(1, std::memory_order_relaxed);
                    adapt_depth();
                    return 0;
                }
            }
        }
        return 0;
    }

    size_t idx = next_sequence_ & kSlotMask;
    Slot& slot = slots_[idx];

    if (!slot.occupied.load(std::memory_order_acquire) ||
        slot.sequence != next_sequence_) {
        // Missing packet — count as loss and skip
        total_lost_.fetch_add(1, std::memory_order_relaxed);
        total_expected_.fetch_add(1, std::memory_order_relaxed);
        next_sequence_++;
        underruns_.fetch_add(1, std::memory_order_relaxed);
        adapt_depth();
        return 0;
    }

    size_t copy_size = std::min(slot.data_size, buf_size);
    std::memcpy(out_buf, slot.data, copy_size);

    slot.occupied.store(false, std::memory_order_release);
    occupancy_.fetch_sub(1, std::memory_order_relaxed);
    next_sequence_++;
    total_played_.fetch_add(1, std::memory_order_relaxed);
    total_expected_.fetch_add(1, std::memory_order_relaxed);

    // Gradually decrease target depth on successful playout
    double cur = target_depth_ms_.load(std::memory_order_relaxed);
    double next = std::max(config_.min_depth_ms, cur - config_.depth_decrease_rate);
    target_depth_ms_.store(next, std::memory_order_relaxed);

    return copy_size;
}

bool JitterBuffer::ready() const {
    if (!started_) return false;

    // Count consecutive occupied slots from next_sequence_
    uint32_t count = 0;
    for (size_t i = 0; i < kSlotCount; i++) {
        size_t idx = (next_sequence_ + i) & kSlotMask;
        const Slot& slot = slots_[idx];
        if (!slot.occupied.load(std::memory_order_relaxed) ||
            slot.sequence != static_cast<uint16_t>(next_sequence_ + i)) {
            break;
        }
        count++;
    }

    double packet_duration_ms = static_cast<double>(
        soluna::samples_per_packet(soluna::PacketTier::WiFi)) /
        config_.sample_rate * 1000.0;
    double buffer_ms = count * packet_duration_ms;
    double target = target_depth_ms_.load(std::memory_order_relaxed);

    return buffer_ms >= target;
}

void JitterBuffer::reset() {
    for (size_t i = 0; i < kSlotCount; i++) {
        slots_[i].occupied.store(false, std::memory_order_relaxed);
    }
    started_ = false;
    next_sequence_ = 0;
    target_depth_ms_.store(config_.initial_depth_ms, std::memory_order_relaxed);
    jitter_estimate_ms_ = 0.0;
    max_jitter_ms_ = 0.0;
    prev_arrival_ns_ = 0;
    total_received_.store(0, std::memory_order_relaxed);
    total_played_.store(0, std::memory_order_relaxed);
    late_drops_.store(0, std::memory_order_relaxed);
    overflow_drops_.store(0, std::memory_order_relaxed);
    underruns_.store(0, std::memory_order_relaxed);
    total_expected_.store(0, std::memory_order_relaxed);
    total_lost_.store(0, std::memory_order_relaxed);
    occupancy_.store(0, std::memory_order_relaxed);
}

JitterBufferStats JitterBuffer::stats() const {
    JitterBufferStats s;
    uint32_t occ = occupancy_.load(std::memory_order_relaxed);
    double packet_duration_ms = static_cast<double>(
        soluna::samples_per_packet(soluna::PacketTier::WiFi)) /
        config_.sample_rate * 1000.0;
    s.current_depth_ms = occ * packet_duration_ms;
    s.target_depth_ms = target_depth_ms_.load(std::memory_order_relaxed);
    s.jitter_ms = jitter_estimate_ms_;
    s.max_jitter_ms = max_jitter_ms_;
    s.packets_received = total_received_.load(std::memory_order_relaxed);
    s.packets_played = total_played_.load(std::memory_order_relaxed);
    s.packets_dropped_late = late_drops_.load(std::memory_order_relaxed);
    s.packets_dropped_overflow = overflow_drops_.load(std::memory_order_relaxed);
    s.underruns = underruns_.load(std::memory_order_relaxed);
    s.buffer_occupancy = occ;
    uint64_t expected = total_expected_.load(std::memory_order_relaxed);
    uint64_t lost = total_lost_.load(std::memory_order_relaxed);
    s.packet_loss_rate = (expected > 0) ? static_cast<double>(lost) / expected : 0.0;
    return s;
}

double JitterBuffer::target_depth_ms() const {
    return target_depth_ms_.load(std::memory_order_relaxed);
}

void JitterBuffer::set_mode(StreamMode mode) {
    if (mode == StreamMode::Jam) {
        config_.min_depth_ms = 0.5;
        config_.max_depth_ms = 4.0;
        config_.initial_depth_ms = 1.0;
        config_.depth_decrease_rate = 0.01;  // faster convergence to minimum
        target_depth_ms_.store(1.0, std::memory_order_relaxed);
    } else {
        config_.min_depth_ms = 2.0;
        config_.max_depth_ms = 20.0;
        config_.initial_depth_ms = 4.0;
        config_.depth_decrease_rate = 0.001;
        target_depth_ms_.store(4.0, std::memory_order_relaxed);
    }
}

void JitterBuffer::adapt_depth() {
    // Increase buffer depth on underrun
    double cur = target_depth_ms_.load(std::memory_order_relaxed);
    double next = std::min(config_.max_depth_ms, cur * config_.depth_increase_factor);
    target_depth_ms_.store(next, std::memory_order_relaxed);
}

} // namespace soluna::wifi
