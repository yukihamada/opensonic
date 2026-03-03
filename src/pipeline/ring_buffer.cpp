#include <soluna/pipeline/ring_buffer.h>
#include <algorithm>
#include <cassert>

namespace soluna::pipeline {

size_t RingBuffer::next_power_of_two(size_t v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    v++;
    return v;
}

RingBuffer::RingBuffer(size_t capacity_frames, size_t frame_size)
    : capacity_(next_power_of_two(capacity_frames < 2 ? 2 : capacity_frames))
    , mask_(capacity_ - 1)
    , frame_size_(frame_size)
    , buffer_(std::make_unique<uint8_t[]>(capacity_ * frame_size_))
{
}

RingBuffer::~RingBuffer() = default;

size_t RingBuffer::write(const void* data, size_t frame_count) {
    const size_t wr = write_pos_.load(std::memory_order_relaxed);
    const size_t rd = read_pos_.load(std::memory_order_acquire);

    const size_t avail = capacity_ - (wr - rd);
    const size_t to_write = std::min(frame_count, avail);
    if (to_write == 0) return 0;

    const size_t wr_idx = wr & mask_;
    const auto* src = static_cast<const uint8_t*>(data);

    // Check if write wraps around
    const size_t first_chunk = std::min(to_write, capacity_ - wr_idx);
    std::memcpy(buffer_.get() + wr_idx * frame_size_, src, first_chunk * frame_size_);

    if (first_chunk < to_write) {
        const size_t second_chunk = to_write - first_chunk;
        std::memcpy(buffer_.get(), src + first_chunk * frame_size_, second_chunk * frame_size_);
    }

    write_pos_.store(wr + to_write, std::memory_order_release);
    return to_write;
}

size_t RingBuffer::read(void* data, size_t frame_count) {
    const size_t wr = write_pos_.load(std::memory_order_acquire);
    const size_t rd = read_pos_.load(std::memory_order_relaxed);

    const size_t avail = wr - rd;
    const size_t to_read = std::min(frame_count, avail);
    if (to_read == 0) return 0;

    const size_t rd_idx = rd & mask_;
    auto* dst = static_cast<uint8_t*>(data);

    const size_t first_chunk = std::min(to_read, capacity_ - rd_idx);
    std::memcpy(dst, buffer_.get() + rd_idx * frame_size_, first_chunk * frame_size_);

    if (first_chunk < to_read) {
        const size_t second_chunk = to_read - first_chunk;
        std::memcpy(dst + first_chunk * frame_size_, buffer_.get(), second_chunk * frame_size_);
    }

    read_pos_.store(rd + to_read, std::memory_order_release);
    return to_read;
}

size_t RingBuffer::peek(void* data, size_t frame_count) const {
    const size_t wr = write_pos_.load(std::memory_order_acquire);
    const size_t rd = read_pos_.load(std::memory_order_relaxed);

    const size_t avail = wr - rd;
    const size_t to_peek = std::min(frame_count, avail);
    if (to_peek == 0) return 0;

    const size_t rd_idx = rd & mask_;
    auto* dst = static_cast<uint8_t*>(data);

    const size_t first_chunk = std::min(to_peek, capacity_ - rd_idx);
    std::memcpy(dst, buffer_.get() + rd_idx * frame_size_, first_chunk * frame_size_);

    if (first_chunk < to_peek) {
        const size_t second_chunk = to_peek - first_chunk;
        std::memcpy(dst + first_chunk * frame_size_, buffer_.get(), second_chunk * frame_size_);
    }

    return to_peek;
}

size_t RingBuffer::available_read() const {
    const size_t wr = write_pos_.load(std::memory_order_acquire);
    const size_t rd = read_pos_.load(std::memory_order_relaxed);
    return wr - rd;
}

size_t RingBuffer::available_write() const {
    const size_t wr = write_pos_.load(std::memory_order_relaxed);
    const size_t rd = read_pos_.load(std::memory_order_acquire);
    return capacity_ - (wr - rd);
}

size_t RingBuffer::discard(size_t frame_count) {
    const size_t wr = write_pos_.load(std::memory_order_acquire);
    const size_t rd = read_pos_.load(std::memory_order_relaxed);

    const size_t avail = wr - rd;
    const size_t to_discard = std::min(frame_count, avail);
    if (to_discard == 0) return 0;

    read_pos_.store(rd + to_discard, std::memory_order_release);
    return to_discard;
}

void RingBuffer::reset() {
    write_pos_.store(0, std::memory_order_relaxed);
    read_pos_.store(0, std::memory_order_relaxed);
}

} // namespace soluna::pipeline
