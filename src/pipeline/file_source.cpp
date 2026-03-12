/**
 * FileSource — WAV (built-in) + MP3 (minimp3) audio file decoder.
 *
 * WAV: handles RIFF/WAVE PCM 16/24/32-bit int and float32.
 * MP3: minimp3 (single-header, MIT). Enabled via -DSOLUNA_HAS_MINIMP3.
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/pipeline/file_source.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>
#include <string>

#ifdef SOLUNA_HAS_MINIMP3
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT   // decode to float directly
#include <minimp3.h>
#endif

namespace soluna::pipeline {

// ─── Tiny WAV RIFF parser ────────────────────────────────────────────────────

namespace {

inline uint16_t le16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
inline uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8)
                                               | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

struct WavInfo {
    uint32_t sample_rate    = 0;
    uint16_t channels       = 0;
    uint16_t bits_per_sample= 0;
    uint16_t audio_format   = 0; // 1=PCM, 3=float32
    uint64_t data_offset    = 0; // byte offset in file to first sample
    uint64_t data_bytes     = 0;
    uint64_t total_frames   = 0;
};

bool parse_wav_header(std::ifstream& f, WavInfo& out) {
    uint8_t buf[12];
    if (!f.read(reinterpret_cast<char*>(buf), 12)) return false;
    if (std::memcmp(buf, "RIFF", 4) != 0) return false;
    if (std::memcmp(buf + 8, "WAVE", 4) != 0) return false;

    bool got_fmt = false;

    while (f) {
        uint8_t chunk_hdr[8];
        if (!f.read(reinterpret_cast<char*>(chunk_hdr), 8)) break;
        uint32_t chunk_id   = le32(chunk_hdr);
        uint32_t chunk_size = le32(chunk_hdr + 4);

        // "fmt "
        if (chunk_id == 0x20746D66u) {
            if (chunk_size < 16) return false;
            uint8_t fmt[16];
            if (!f.read(reinterpret_cast<char*>(fmt), 16)) return false;
            out.audio_format    = le16(fmt + 0);
            out.channels        = le16(fmt + 2);
            out.sample_rate     = le32(fmt + 4);
            // skip byte_rate(4) + block_align(2)
            out.bits_per_sample = le16(fmt + 14);
            got_fmt = true;
            // Skip rest of fmt chunk if extended
            if (chunk_size > 16) f.seekg(chunk_size - 16, std::ios::cur);
        }
        // "data"
        else if (chunk_id == 0x61746164u) {
            out.data_offset = static_cast<uint64_t>(f.tellg());
            out.data_bytes  = chunk_size;
            if (got_fmt && out.bits_per_sample > 0 && out.channels > 0) {
                uint32_t bytes_per_frame = out.channels * (out.bits_per_sample / 8);
                out.total_frames = out.data_bytes / bytes_per_frame;
            }
            return got_fmt;
        }
        else {
            // Skip unknown chunk (align to even byte boundary)
            f.seekg((chunk_size + 1) & ~1u, std::ios::cur);
        }
    }
    return false;
}

} // anonymous namespace

// ─── FileSource::Impl ────────────────────────────────────────────────────────

struct FileSource::Impl {
    FileSourceInfo  info;
    uint32_t        out_rate   = 48000;
    uint32_t        out_ch     = 2;
    bool            eof_       = false;

    // ── WAV state ─────────────────────────────────────────────
    std::ifstream   wav_file;
    WavInfo         wav_info;
    uint64_t        wav_frames_read = 0;

    // ── MP3 state ─────────────────────────────────────────────
#ifdef SOLUNA_HAS_MINIMP3
    mp3dec_t                mp3dec;
    std::vector<uint8_t>    mp3_data;   // whole file in memory
    size_t                  mp3_offset = 0;
    std::vector<float>      mp3_leftover;   // partial decoded frame
    uint32_t                mp3_native_rate = 0;
    uint32_t                mp3_native_ch   = 0;
    uint64_t                mp3_frames_out  = 0;
#endif

    // ── Simple float→S24 conversion buffer ────────────────────
    std::vector<float> float_buf;

    // ── Up/down-mix (mono↔stereo) ──────────────────────────────
    // Applied after format decode, before returning

    bool open_wav(const std::string& path) {
        wav_file.open(path, std::ios::binary);
        if (!wav_file) return false;
        if (!parse_wav_header(wav_file, wav_info)) return false;
        // Validate
        if (wav_info.sample_rate == 0 || wav_info.channels == 0) return false;
        if (wav_info.audio_format != 1 && wav_info.audio_format != 3) return false; // PCM or float
        if (wav_info.bits_per_sample != 16 && wav_info.bits_per_sample != 24
            && wav_info.bits_per_sample != 32) return false;

        info.format       = FileAudioFormat::WAV;
        info.native_rate  = wav_info.sample_rate;
        info.native_ch    = wav_info.channels;
        info.total_frames = wav_info.total_frames;
        info.duration_sec = (out_rate > 0 && info.total_frames > 0)
            ? (double)info.total_frames / wav_info.sample_rate : 0.0;
        return true;
    }

    // Read WAV frames as float (native channels, native sample rate)
    // Returns frame count written to float_buf
    size_t read_wav_float(size_t frame_count) {
        uint32_t bps   = wav_info.bits_per_sample;
        uint32_t ch    = wav_info.channels;
        uint32_t bpf   = ch * (bps / 8); // bytes per frame
        size_t   avail = (wav_info.data_bytes > wav_frames_read * bpf)
                          ? (wav_info.data_bytes - wav_frames_read * bpf) / bpf
                          : 0;
        size_t   n     = std::min(frame_count, avail);
        if (n == 0) { eof_ = true; return 0; }

        size_t bytes = n * bpf;
        std::vector<uint8_t> raw(bytes);
        wav_file.seekg(static_cast<std::streamoff>(wav_info.data_offset + wav_frames_read * bpf));
        if (!wav_file.read(reinterpret_cast<char*>(raw.data()), (std::streamsize)bytes)) {
            size_t got = wav_file.gcount();
            n   = got / bpf;
            bytes = n * bpf;
        }
        wav_frames_read += n;
        if (wav_frames_read >= wav_info.total_frames) eof_ = true;

        size_t total_samples = n * ch;
        float_buf.resize(total_samples);
        const uint8_t* src = raw.data();

        if (wav_info.audio_format == 3 && bps == 32) {
            // float32 native
            for (size_t i = 0; i < total_samples; i++) {
                float v;
                std::memcpy(&v, src + i * 4, 4);
                float_buf[i] = v;
            }
        } else if (bps == 16) {
            for (size_t i = 0; i < total_samples; i++) {
                int16_t s = (int16_t)le16(src + i * 2);
                float_buf[i] = s * (1.0f / 32768.0f);
            }
        } else if (bps == 24) {
            for (size_t i = 0; i < total_samples; i++) {
                const uint8_t* p = src + i * 3;
                int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
                if (v & 0x800000) v |= 0xFF000000; // sign-extend
                float_buf[i] = v * (1.0f / 8388608.0f);
            }
        } else { // 32-bit PCM
            for (size_t i = 0; i < total_samples; i++) {
                int32_t s;
                std::memcpy(&s, src + i * 4, 4);
                float_buf[i] = s * (1.0f / 2147483648.0f);
            }
        }
        return n;
    }

#ifdef SOLUNA_HAS_MINIMP3
    bool open_mp3(const std::string& path) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return false;
        std::streamsize sz = f.tellg();
        f.seekg(0);
        mp3_data.resize(sz);
        if (!f.read(reinterpret_cast<char*>(mp3_data.data()), sz)) return false;

        mp3dec_init(&mp3dec);
        mp3_offset = 0;

        // Probe first frame to get sample rate & channels
        mp3dec_frame_info_t probe_info{};
        float pcm_probe[MINIMP3_MAX_SAMPLES_PER_FRAME];
        int probe_samples = mp3dec_decode_frame(&mp3dec, mp3_data.data(),
                                                (int)mp3_data.size(), pcm_probe, &probe_info);
        if (probe_info.frame_bytes == 0) return false;

        mp3_native_rate = (uint32_t)probe_info.hz;
        mp3_native_ch   = (uint32_t)probe_info.channels;

        // Reset decoder for actual playback
        mp3dec_init(&mp3dec);

        // Estimate duration: avg bitrate from first frame
        // frame_bytes includes header; frame covers 1152 samples for MPEG-1 Layer3
        double fps = (double)mp3_native_rate / 1152.0;
        size_t est_frames = (size_t)((double)mp3_data.size()
                                     / probe_info.frame_bytes * 1152);

        info.format       = FileAudioFormat::MP3;
        info.native_rate  = mp3_native_rate;
        info.native_ch    = mp3_native_ch;
        info.total_frames = est_frames; // approximate
        info.duration_sec = (fps > 0) ? est_frames / (double)mp3_native_rate : 0.0;
        return true;
    }

    // Decode next MP3 frames into float_buf (native channels/rate)
    size_t read_mp3_float(size_t frame_count) {
        float_buf.clear();
        size_t frames_so_far = 0;

        // Drain leftover from previous call
        if (!mp3_leftover.empty()) {
            size_t lo_frames = mp3_leftover.size() / mp3_native_ch;
            size_t take = std::min(lo_frames, frame_count);
            float_buf.insert(float_buf.end(),
                             mp3_leftover.begin(),
                             mp3_leftover.begin() + take * mp3_native_ch);
            frames_so_far += take;
            mp3_leftover.erase(mp3_leftover.begin(),
                               mp3_leftover.begin() + take * mp3_native_ch);
        }

        while (frames_so_far < frame_count && mp3_offset < mp3_data.size()) {
            float pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
            mp3dec_frame_info_t fi{};
            int samples = mp3dec_decode_frame(
                &mp3dec,
                mp3_data.data() + mp3_offset,
                (int)(mp3_data.size() - mp3_offset),
                pcm, &fi);

            if (fi.frame_bytes == 0 && samples == 0) { eof_ = true; break; }
            mp3_offset += fi.frame_bytes;

            if (samples > 0) {
                size_t frames_in_chunk = (size_t)samples; // samples per channel
                size_t need = frame_count - frames_so_far;
                size_t take = std::min(frames_in_chunk, need);
                float_buf.insert(float_buf.end(), pcm, pcm + take * mp3_native_ch);
                frames_so_far += take;
                // Store overflow
                if (take < frames_in_chunk) {
                    mp3_leftover.assign(pcm + take * mp3_native_ch,
                                        pcm + frames_in_chunk * mp3_native_ch);
                }
            }
        }

        if (mp3_offset >= mp3_data.size() && mp3_leftover.empty()) eof_ = true;
        mp3_frames_out += frames_so_far;
        return frames_so_far;
    }
#endif // SOLUNA_HAS_MINIMP3

    // Convert float (interleaved, native_ch) to int32_t S24_LE (out_ch)
    // and write into dst[0..frame_count*out_ch-1]
    void float_to_s24(int32_t* dst, size_t frame_count, uint32_t native_ch_) const {
        for (size_t f = 0; f < frame_count; f++) {
            for (uint32_t c = 0; c < out_ch; c++) {
                float s;
                if (native_ch_ == 1) {
                    // mono → stereo: duplicate
                    s = float_buf[f];
                } else if (out_ch == 1 && native_ch_ == 2) {
                    // stereo → mono: average
                    s = (float_buf[f * 2] + float_buf[f * 2 + 1]) * 0.5f;
                } else {
                    // channel mapping (take min of available vs requested)
                    uint32_t src_c = (c < native_ch_) ? c : native_ch_ - 1;
                    s = float_buf[f * native_ch_ + src_c];
                }
                s = std::fmax(-1.0f, std::fmin(1.0f, s));
                dst[f * out_ch + c] = (int32_t)(s * 8388607.0f);
            }
        }
    }
};

// ─── FileSource public API ───────────────────────────────────────────────────

FileSource::FileSource() : impl_(std::make_unique<Impl>()) {}
FileSource::~FileSource() { close(); }

static FileAudioFormat detect_format(const std::string& path) {
    // By extension
    auto dot = path.rfind('.');
    if (dot != std::string::npos) {
        std::string ext = path.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == "wav" || ext == "wave") return FileAudioFormat::WAV;
        if (ext == "mp3")                  return FileAudioFormat::MP3;
    }
    // By magic bytes
    std::ifstream f(path, std::ios::binary);
    if (f) {
        uint8_t magic[4]{};
        f.read(reinterpret_cast<char*>(magic), 4);
        if (magic[0] == 'R' && magic[1] == 'I' && magic[2] == 'F' && magic[3] == 'F')
            return FileAudioFormat::WAV;
        // MP3 sync word: 0xFF 0xFB / 0xFF 0xF3 / 0xFF 0xE3, or ID3 header
        if (magic[0] == 0xFF && (magic[1] & 0xE0) == 0xE0) return FileAudioFormat::MP3;
        if (magic[0] == 'I' && magic[1] == 'D' && magic[2] == '3') return FileAudioFormat::MP3;
    }
    return FileAudioFormat::Unknown;
}

bool FileSource::open(const std::string& path, uint32_t target_rate, uint32_t target_ch) {
    close();
    impl_->out_rate = (target_rate == 0) ? 48000 : target_rate;
    impl_->out_ch   = (target_ch   == 0) ? 2     : target_ch;
    impl_->eof_     = false;

    FileAudioFormat fmt = detect_format(path);

    if (fmt == FileAudioFormat::WAV) {
        return impl_->open_wav(path);
    }

#ifdef SOLUNA_HAS_MINIMP3
    if (fmt == FileAudioFormat::MP3) {
        return impl_->open_mp3(path);
    }
#endif

    fprintf(stderr, "[FileSource] Unsupported format or minimp3 not built: %s\n", path.c_str());
    return false;
}

size_t FileSource::read_frames(int32_t* buf, size_t frame_count) {
    if (!impl_ || impl_->eof_) return 0;

    size_t native_ch = impl_->info.native_ch;
    size_t got = 0;

    if (impl_->info.format == FileAudioFormat::WAV) {
        got = impl_->read_wav_float(frame_count);
    }
#ifdef SOLUNA_HAS_MINIMP3
    else if (impl_->info.format == FileAudioFormat::MP3) {
        got = impl_->read_mp3_float(frame_count);
        native_ch = impl_->mp3_native_ch;
    }
#endif

    if (got == 0) return 0;
    impl_->float_to_s24(buf, got, (uint32_t)native_ch);
    return got;
}

bool FileSource::seek_ms(uint64_t ms) {
    if (!impl_) return false;
    if (impl_->info.format == FileAudioFormat::WAV) {
        uint64_t frame = (uint64_t)ms * impl_->wav_info.sample_rate / 1000;
        frame = std::min(frame, impl_->wav_info.total_frames);
        impl_->wav_frames_read = frame;
        impl_->eof_ = (frame >= impl_->wav_info.total_frames);
        return true;
    }
#ifdef SOLUNA_HAS_MINIMP3
    if (impl_->info.format == FileAudioFormat::MP3) {
        // Approximate: assume constant bitrate for position estimate
        if (impl_->info.duration_sec > 0 && impl_->mp3_data.size() > 0) {
            double ratio = (double)ms / (impl_->info.duration_sec * 1000.0);
            ratio = std::max(0.0, std::min(1.0, ratio));
            impl_->mp3_offset = (size_t)(ratio * impl_->mp3_data.size());
            impl_->mp3_leftover.clear();
            impl_->eof_ = false;
            mp3dec_init(&impl_->mp3dec);
            impl_->mp3_frames_out = (uint64_t)(ratio * impl_->info.total_frames);
        }
        return true;
    }
#endif
    return false;
}

uint64_t FileSource::position_ms() const {
    if (!impl_) return 0;
    if (impl_->info.format == FileAudioFormat::WAV && impl_->wav_info.sample_rate > 0)
        return impl_->wav_frames_read * 1000 / impl_->wav_info.sample_rate;
#ifdef SOLUNA_HAS_MINIMP3
    if (impl_->info.format == FileAudioFormat::MP3 && impl_->mp3_native_rate > 0)
        return impl_->mp3_frames_out * 1000 / impl_->mp3_native_rate;
#endif
    return 0;
}

uint64_t FileSource::duration_ms() const {
    if (!impl_) return 0;
    return (uint64_t)(impl_->info.duration_sec * 1000.0);
}

bool FileSource::is_open() const {
    if (!impl_) return false;
    if (impl_->info.format == FileAudioFormat::WAV) return impl_->wav_file.is_open();
#ifdef SOLUNA_HAS_MINIMP3
    if (impl_->info.format == FileAudioFormat::MP3) return !impl_->mp3_data.empty();
#endif
    return false;
}

bool FileSource::is_eof() const { return impl_ ? impl_->eof_ : true; }

void FileSource::close() {
    if (!impl_) return;
    impl_->wav_file.close();
#ifdef SOLUNA_HAS_MINIMP3
    impl_->mp3_data.clear();
    impl_->mp3_leftover.clear();
    impl_->mp3_offset = 0;
#endif
    impl_->eof_ = true;
}

uint32_t FileSource::sample_rate() const { return impl_ ? impl_->out_rate : 0; }
uint32_t FileSource::channels()    const { return impl_ ? impl_->out_ch   : 0; }

const char* FileSource::format_name() const {
    if (!impl_) return "Unknown";
    switch (impl_->info.format) {
        case FileAudioFormat::WAV: return "WAV";
        case FileAudioFormat::MP3: return "MP3";
        default:                   return "Unknown";
    }
}

const FileSourceInfo& FileSource::info() const {
    static FileSourceInfo empty;
    return impl_ ? impl_->info : empty;
}

} // namespace soluna::pipeline
