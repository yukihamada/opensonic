#ifdef SOLUNA_HAS_PIPEWIRE

#include <soluna/pal/pipewire.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

// ── Lock-free SPSC ring buffer (PipeWire internal) ──────────────────────────
// Single-producer (caller's write()), single-consumer (PipeWire on_process).

namespace {

class PwRingBuffer {
public:
    PwRingBuffer() = default;

    void allocate(size_t capacity_frames, uint32_t channels) {
        channels_ = channels;
        capacity_ = next_pow2(capacity_frames);
        mask_ = capacity_ - 1;
        buf_.resize(capacity_ * channels);
        write_pos_.store(0, std::memory_order_relaxed);
        read_pos_.store(0, std::memory_order_relaxed);
    }

    size_t write(const int32_t* data, size_t frames) {
        size_t wr = write_pos_.load(std::memory_order_relaxed);
        size_t rd = read_pos_.load(std::memory_order_acquire);
        size_t avail = capacity_ - (wr - rd);
        size_t to_write = frames < avail ? frames : avail;
        for (size_t f = 0; f < to_write; f++) {
            size_t idx = (wr + f) & mask_;
            for (uint32_t ch = 0; ch < channels_; ch++) {
                buf_[idx * channels_ + ch] = data[f * channels_ + ch];
            }
        }
        write_pos_.store(wr + to_write, std::memory_order_release);
        return to_write;
    }

    size_t read(int32_t* data, size_t frames) {
        size_t rd = read_pos_.load(std::memory_order_relaxed);
        size_t wr = write_pos_.load(std::memory_order_acquire);
        size_t avail = wr - rd;
        size_t to_read = frames < avail ? frames : avail;
        for (size_t f = 0; f < to_read; f++) {
            size_t idx = (rd + f) & mask_;
            for (uint32_t ch = 0; ch < channels_; ch++) {
                data[f * channels_ + ch] = buf_[idx * channels_ + ch];
            }
        }
        read_pos_.store(rd + to_read, std::memory_order_release);
        return to_read;
    }

    size_t available_read() const {
        size_t wr = write_pos_.load(std::memory_order_acquire);
        size_t rd = read_pos_.load(std::memory_order_relaxed);
        return wr - rd;
    }

private:
    static size_t next_pow2(size_t v) {
        v--;
        v |= v >> 1; v |= v >> 2; v |= v >> 4;
        v |= v >> 8; v |= v >> 16;
        return v + 1;
    }

    uint32_t channels_ = 1;
    size_t   capacity_ = 0;
    size_t   mask_ = 0;
    std::vector<int32_t> buf_;
    std::atomic<size_t> write_pos_{0};
    std::atomic<size_t> read_pos_{0};
};

} // anonymous namespace

namespace soluna::pal {

// ── PipeWire implementation ─────────────────────────────────────────────────

struct PipeWireAudioSink::Impl {
    pw_thread_loop* loop = nullptr;
    pw_stream*      stream = nullptr;
    PwRingBuffer    ring;
    uint32_t        channels = 1;
    uint32_t        frames_per_buffer = 480;
    bool            opened = false;

    static void on_process(void* userdata) {
        auto* self = static_cast<Impl*>(userdata);

        pw_buffer* b = pw_stream_dequeue_buffer(self->stream);
        if (!b) return;

        spa_buffer* buf = b->buffer;
        auto* dst = static_cast<int32_t*>(buf->datas[0].data);
        if (!dst) {
            pw_stream_queue_buffer(self->stream, b);
            return;
        }

        uint32_t max_frames = buf->datas[0].maxsize /
                              (sizeof(int32_t) * self->channels);
        uint32_t n_frames = self->frames_per_buffer;
        if (n_frames > max_frames) n_frames = max_frames;

        size_t got = self->ring.read(dst, n_frames);

        // Zero-fill remainder if ring buffer underrun
        if (got < n_frames) {
            std::memset(dst + got * self->channels, 0,
                        (n_frames - got) * self->channels * sizeof(int32_t));
        }

        buf->datas[0].chunk->offset = 0;
        buf->datas[0].chunk->stride = static_cast<int32_t>(sizeof(int32_t) * self->channels);
        buf->datas[0].chunk->size   = n_frames * self->channels * sizeof(int32_t);

        pw_stream_queue_buffer(self->stream, b);
    }
};

static const pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = PipeWireAudioSink::Impl::on_process,
};

PipeWireAudioSink::PipeWireAudioSink() = default;

PipeWireAudioSink::~PipeWireAudioSink() {
    close();
}

bool PipeWireAudioSink::open(const Config& cfg) {
    if (impl_ && impl_->opened) return false;

    pw_init(nullptr, nullptr);

    impl_ = new Impl();
    impl_->channels = cfg.channels;
    impl_->frames_per_buffer = cfg.frames_per_buffer;

    // Ring buffer: hold ~200ms worth of audio (generous headroom)
    size_t ring_frames = cfg.sample_rate / 5;
    if (ring_frames < cfg.frames_per_buffer * 8)
        ring_frames = cfg.frames_per_buffer * 8;
    impl_->ring.allocate(ring_frames, cfg.channels);

    impl_->loop = pw_thread_loop_new(cfg.stream_name.c_str(), nullptr);
    if (!impl_->loop) {
        fprintf(stderr, "[pipewire] Failed to create thread loop\n");
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    // Build stream properties
    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,     "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE,     "Music",
        PW_KEY_APP_NAME,       cfg.stream_name.c_str(),
        nullptr);

    impl_->stream = pw_stream_new_simple(
        pw_thread_loop_get_loop(impl_->loop),
        cfg.stream_name.c_str(),
        props,
        &stream_events,
        impl_);

    if (!impl_->stream) {
        fprintf(stderr, "[pipewire] Failed to create stream\n");
        pw_thread_loop_destroy(impl_->loop);
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    // Set up audio format: S32 interleaved
    uint8_t params_buf[1024];
    spa_pod_builder builder;
    spa_pod_builder_init(&builder, params_buf, sizeof(params_buf));

    spa_audio_info_raw audio_info;
    spa_zero(audio_info);
    audio_info.format   = SPA_AUDIO_FORMAT_S32;
    audio_info.rate     = cfg.sample_rate;
    audio_info.channels = cfg.channels;

    const spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &audio_info);

    int res = pw_stream_connect(
        impl_->stream,
        PW_DIRECTION_OUTPUT,
        PW_ID_ANY,
        static_cast<pw_stream_flags>(
            PW_STREAM_FLAG_AUTOCONNECT |
            PW_STREAM_FLAG_MAP_BUFFERS |
            PW_STREAM_FLAG_RT_PROCESS),
        params, 1);

    if (res < 0) {
        fprintf(stderr, "[pipewire] Stream connect failed: %s\n", spa_strerror(res));
        pw_stream_destroy(impl_->stream);
        pw_thread_loop_destroy(impl_->loop);
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    pw_thread_loop_start(impl_->loop);
    impl_->opened = true;

    fprintf(stderr, "[pipewire] Opened: %uHz %uch S32, buffer %u frames\n",
            cfg.sample_rate, cfg.channels, cfg.frames_per_buffer);
    return true;
}

void PipeWireAudioSink::close() {
    if (!impl_) return;

    if (impl_->loop) {
        pw_thread_loop_stop(impl_->loop);
    }
    if (impl_->stream) {
        pw_stream_destroy(impl_->stream);
    }
    if (impl_->loop) {
        pw_thread_loop_destroy(impl_->loop);
    }

    delete impl_;
    impl_ = nullptr;

    pw_deinit();
}

size_t PipeWireAudioSink::write(const int32_t* data, size_t frames) {
    if (!impl_ || !impl_->opened) return 0;
    return impl_->ring.write(data, frames);
}

bool PipeWireAudioSink::is_open() const {
    return impl_ && impl_->opened;
}

} // namespace soluna::pal

#endif // SOLUNA_HAS_PIPEWIRE
