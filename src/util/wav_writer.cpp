/**
 * WavWriter implementation
 *
 * SPDX-License-Identifier: OpenSonic-Community-1.0
 */

#include <soluna/util/wav_writer.h>
#include <cstring>

namespace soluna {
namespace util {

WavWriter::~WavWriter() {
    // close() is safe to call even if already closed
    close();
}

WavWriter::WavWriter(WavWriter&& other) noexcept {
    std::lock_guard<std::mutex> lk(other.mutex_);
    fp_              = other.fp_;
    path_            = std::move(other.path_);
    sample_rate_     = other.sample_rate_;
    channels_        = other.channels_;
    bits_per_sample_ = other.bits_per_sample_;
    data_bytes_      = other.data_bytes_;
    frames_written_  = other.frames_written_;

    other.fp_             = nullptr;
    other.data_bytes_     = 0;
    other.frames_written_ = 0;
}

WavWriter& WavWriter::operator=(WavWriter&& other) noexcept {
    if (this == &other) return *this;
    close();
    std::lock_guard<std::mutex> lk(other.mutex_);
    fp_              = other.fp_;
    path_            = std::move(other.path_);
    sample_rate_     = other.sample_rate_;
    channels_        = other.channels_;
    bits_per_sample_ = other.bits_per_sample_;
    data_bytes_      = other.data_bytes_;
    frames_written_  = other.frames_written_;

    other.fp_             = nullptr;
    other.data_bytes_     = 0;
    other.frames_written_ = 0;
    return *this;
}

bool WavWriter::open(const std::string& path, uint32_t sample_rate,
                     uint32_t channels, uint32_t bits_per_sample) {
    std::lock_guard<std::mutex> lk(mutex_);

    // Close any previously open file
    if (fp_) {
        write_header();
        fclose(fp_);
        fp_ = nullptr;
    }

    if (channels == 0 || sample_rate == 0) return false;
    if (bits_per_sample != 8 && bits_per_sample != 16 &&
        bits_per_sample != 24 && bits_per_sample != 32) {
        return false;
    }

    fp_ = fopen(path.c_str(), "wb");
    if (!fp_) return false;

    path_            = path;
    sample_rate_     = sample_rate;
    channels_        = channels;
    bits_per_sample_ = bits_per_sample;
    data_bytes_      = 0;
    frames_written_  = 0;

    // Write a placeholder 44-byte header (will be finalised in close())
    uint8_t hdr[44] = {};
    fwrite(hdr, 1, 44, fp_);
    return true;
}

size_t WavWriter::write(const void* data, size_t frames) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!fp_ || frames == 0 || !data) return 0;

    uint32_t bytes_per_frame = channels_ * (bits_per_sample_ / 8);
    size_t bytes = frames * bytes_per_frame;
    size_t written = fwrite(data, 1, bytes, fp_);
    size_t frames_out = written / bytes_per_frame;

    data_bytes_    += static_cast<uint32_t>(frames_out * bytes_per_frame);
    frames_written_ += frames_out;
    return frames_out;
}

void WavWriter::close() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!fp_) return;

    write_header();
    fclose(fp_);
    fp_ = nullptr;
}

bool WavWriter::is_open() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return fp_ != nullptr;
}

uint64_t WavWriter::frames_written() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return frames_written_;
}

// --- private ---

void WavWriter::write_header() {
    if (!fp_) return;

    fseek(fp_, 0, SEEK_SET);

    uint32_t file_size   = 36 + data_bytes_;
    uint16_t audio_fmt   = 1; // PCM
    uint16_t ch16        = static_cast<uint16_t>(channels_);
    uint16_t bits16      = static_cast<uint16_t>(bits_per_sample_);
    uint16_t block_align = static_cast<uint16_t>(channels_ * (bits_per_sample_ / 8));
    uint32_t byte_rate   = sample_rate_ * block_align;
    uint32_t fmt_size    = 16;

    uint8_t hdr[44];
    std::memcpy(hdr + 0,  "RIFF", 4);
    std::memcpy(hdr + 4,  &file_size, 4);
    std::memcpy(hdr + 8,  "WAVE", 4);
    std::memcpy(hdr + 12, "fmt ", 4);
    std::memcpy(hdr + 16, &fmt_size, 4);
    std::memcpy(hdr + 20, &audio_fmt, 2);
    std::memcpy(hdr + 22, &ch16, 2);
    std::memcpy(hdr + 24, &sample_rate_, 4);
    std::memcpy(hdr + 28, &byte_rate, 4);
    std::memcpy(hdr + 32, &block_align, 2);
    std::memcpy(hdr + 34, &bits16, 2);
    std::memcpy(hdr + 36, "data", 4);
    std::memcpy(hdr + 40, &data_bytes_, 4);
    fwrite(hdr, 1, 44, fp_);

    // Seek back to end so further writes (if any) append correctly
    fseek(fp_, 0, SEEK_END);
}

} // namespace util
} // namespace soluna
