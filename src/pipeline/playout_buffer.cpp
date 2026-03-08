/**
 * PTP-synchronized Playout Buffer
 * SPDX-License-Identifier: MIT
 */

#include <soluna/pipeline/playout_buffer.h>
#include <soluna/pipeline/pipeline.h>
#include <algorithm>
#include <cstring>

namespace soluna::pipeline {

PlayoutBuffer::PlayoutBuffer(const PlayoutBufferConfig& config)
    : config_(config)
    , ring_(config.capacity_packets)
    , mode_(config.mode)
{
}

int64_t PlayoutBuffer::media_ts_to_playout_ns(uint32_t media_ts) const {
    // Convert media timestamp to playout time
    // media_ts is in nanoseconds (from OSTP header)
    int64_t media_ns = static_cast<int64_t>(media_ts);

    // Playout time = base_ptp + (media_ns - base_media) + playout_delay
    return base_ptp_ns_ + (media_ns - base_media_ns_) + config_.playout_delay_ns;
}

bool PlayoutBuffer::insert(const PlayoutPacket& packet) {
    std::lock_guard<std::mutex> lock(mutex_);

    stats_.packets_received++;

    // Set base time reference from first packet
    if (!base_set_) {
        base_media_ns_ = static_cast<int64_t>(packet.media_timestamp);
        base_ptp_ns_ = static_cast<int64_t>(packet.media_timestamp); // initially media ≈ ptp
        base_set_ = true;
    }

    // Check if buffer is full
    size_t count = 0;
    for (const auto& p : ring_) {
        if (p.valid) count++;
    }

    if (count >= config_.capacity_packets) {
        stats_.packets_dropped_overflow++;
        return false;
    }

    // Insert at write position
    ring_[write_idx_] = packet;
    ring_[write_idx_].valid = true;
    write_idx_ = (write_idx_ + 1) % ring_.size();

    stats_.current_depth = static_cast<int32_t>(count + 1);
    return true;
}

bool PlayoutBuffer::read_at(int64_t ptp_now_ns, PlayoutPacket& out) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!base_set_) return false;

    int best_idx = -1;
    uint32_t best_seq = UINT32_MAX;

    if (mode_ == StreamMode::Jam) {
        // Jam mode: bypass PTP alignment, return oldest valid packet (FIFO)
        for (size_t i = 0; i < ring_.size(); i++) {
            if (!ring_[i].valid) continue;
            if (ring_[i].sequence < best_seq) {
                best_seq = ring_[i].sequence;
                best_idx = static_cast<int>(i);
            }
        }
    } else {
        // Sync mode: find the oldest valid packet whose playout time has arrived
        for (size_t i = 0; i < ring_.size(); i++) {
            if (!ring_[i].valid) continue;

            int64_t playout_ns = media_ts_to_playout_ns(ring_[i].media_timestamp);

            if (playout_ns <= ptp_now_ns) {
                // This packet should be playing now
                if (ring_[i].sequence < best_seq) {
                    best_seq = ring_[i].sequence;
                    best_idx = static_cast<int>(i);
                }
            }
        }
    }

    if (best_idx < 0) {
        stats_.underruns++;
        return false;
    }

    out = ring_[static_cast<size_t>(best_idx)];
    ring_[static_cast<size_t>(best_idx)].valid = false;

    // Drop any packets that are too late (older than the one we're playing)
    for (auto& p : ring_) {
        if (p.valid && p.sequence < best_seq) {
            p.valid = false;
            stats_.packets_dropped_late++;
        }
    }

    stats_.packets_played++;

    // Update depth
    int32_t depth = 0;
    for (const auto& p : ring_) {
        if (p.valid) depth++;
    }
    stats_.current_depth = depth;

    return true;
}

size_t PlayoutBuffer::read_frames(int64_t ptp_now_ns, float* output,
                                    size_t frame_count, uint32_t channels) {
    PlayoutPacket pkt;
    if (!read_at(ptp_now_ns, pkt)) {
        // Underrun: zero-fill
        std::memset(output, 0, frame_count * channels * sizeof(float));
        return 0;
    }

    // Convert S24 payload to float
    size_t total_samples = pkt.audio_data.size() / sizeof(int32_t);
    size_t available_frames = total_samples / channels;
    size_t frames_to_copy = std::min(frame_count, available_frames);

    const auto* s24_data = reinterpret_cast<const int32_t*>(pkt.audio_data.data());
    s24_to_float(s24_data, output, frames_to_copy * channels);

    // Zero-fill remainder
    if (frames_to_copy < frame_count) {
        std::memset(output + frames_to_copy * channels, 0,
            (frame_count - frames_to_copy) * channels * sizeof(float));
    }

    return frames_to_copy;
}

void PlayoutBuffer::set_mode(StreamMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    mode_ = mode;
}

void PlayoutBuffer::set_playout_delay(int64_t delay_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.playout_delay_ns = delay_ns;
}

PlayoutBufferStats PlayoutBuffer::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void PlayoutBuffer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& p : ring_) {
        p.valid = false;
    }
    write_idx_ = 0;
    stats_ = {};
    base_set_ = false;
}

} // namespace soluna::pipeline
