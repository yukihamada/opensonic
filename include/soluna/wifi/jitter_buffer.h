#pragma once

/**
 * Adaptive Jitter Buffer — WiFi audio buffering
 *
 * Reorders and buffers incoming packets to absorb network jitter.
 * Dynamically adjusts depth based on observed jitter statistics.
 *
 * SPDX-License-Identifier: MIT
 */

#include <cstdint>
#include <cstddef>
#include <map>
#include <mutex>
#include <vector>
#include <functional>

namespace soluna::wifi {

struct JitterBufferConfig {
    uint32_t sample_rate = 48000;
    uint32_t channels = 1;
    uint32_t frame_size = 4;          // bytes per sample (float32)

    // Buffer depth range in milliseconds
    double min_depth_ms = 2.0;
    double max_depth_ms = 20.0;
    double initial_depth_ms = 4.0;

    // Adaptation parameters
    double depth_increase_factor = 1.5;    // multiply depth on underrun
    double depth_decrease_rate = 0.001;    // ms per successful read
    double jitter_smoothing = 0.05;        // EMA alpha for jitter estimate
};

struct JitterBufferStats {
    double current_depth_ms = 0.0;
    double target_depth_ms = 0.0;
    double jitter_ms = 0.0;           // estimated jitter
    double max_jitter_ms = 0.0;
    uint64_t packets_received = 0;
    uint64_t packets_played = 0;
    uint64_t packets_dropped_late = 0;
    uint64_t packets_dropped_overflow = 0;
    uint64_t underruns = 0;
    uint32_t buffer_occupancy = 0;     // current packet count in buffer
    double packet_loss_rate = 0.0;     // 0.0-1.0
};

class JitterBuffer {
public:
    explicit JitterBuffer(const JitterBufferConfig& config = {});
    ~JitterBuffer();

    JitterBuffer(const JitterBuffer&) = delete;
    JitterBuffer& operator=(const JitterBuffer&) = delete;

    /**
     * Push a received packet into the buffer.
     * sequence: RTP sequence number (for ordering)
     * timestamp_ns: arrival time in nanoseconds (monotonic)
     * data: audio payload (float samples)
     * data_size: payload size in bytes
     */
    void push(uint16_t sequence, int64_t timestamp_ns,
              const void* data, size_t data_size);

    /**
     * Pop the next packet for playout.
     * Returns number of bytes copied, or 0 if buffer is empty/not ready.
     * out_buf must be large enough for one packet.
     */
    size_t pop(void* out_buf, size_t buf_size);

    /**
     * Check if buffer has enough data for playout.
     */
    bool ready() const;

    /** Reset the buffer. */
    void reset();

    /** Get current statistics. */
    JitterBufferStats stats() const;

    /** Get current target depth in milliseconds. */
    double target_depth_ms() const;

private:
    struct Packet {
        uint16_t sequence;
        int64_t arrival_ns;
        std::vector<uint8_t> data;
    };

    JitterBufferConfig config_;
    mutable std::mutex mutex_;

    std::map<uint16_t, Packet> packets_;
    uint16_t next_sequence_ = 0;
    bool started_ = false;

    // Timing
    int64_t first_arrival_ns_ = 0;
    int64_t last_arrival_ns_ = 0;

    // Adaptive depth
    double target_depth_ms_;
    double jitter_estimate_ms_ = 0.0;
    double max_jitter_ms_ = 0.0;

    // Statistics
    uint64_t total_received_ = 0;
    uint64_t total_played_ = 0;
    uint64_t late_drops_ = 0;
    uint64_t overflow_drops_ = 0;
    uint64_t underruns_ = 0;
    uint64_t total_expected_ = 0;
    uint64_t total_lost_ = 0;

    // Jitter calculation
    int64_t prev_arrival_ns_ = 0;
    int64_t prev_transit_ns_ = 0;

    void adapt_depth();
    bool is_too_old(uint16_t seq) const;
    static int16_t seq_diff(uint16_t a, uint16_t b);
};

} // namespace soluna::wifi
