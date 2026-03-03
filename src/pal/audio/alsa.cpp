#include <soluna/pal/audio.h>

#ifdef SOLUNA_HAS_ALSA
#include <alsa/asoundlib.h>
#endif

#include <cstdio>
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace soluna::pal {

#ifdef SOLUNA_HAS_ALSA

class AlsaAudioDevice : public AudioDevice {
public:
    ~AlsaAudioDevice() override {
        close();
    }

    bool open_input(const std::string& device_id, const AudioStreamConfig& config) override {
        return open_device(device_id, config, SND_PCM_STREAM_CAPTURE);
    }

    bool open_output(const std::string& device_id, const AudioStreamConfig& config) override {
        return open_device(device_id, config, SND_PCM_STREAM_PLAYBACK);
    }

    void close() override {
        stop();
        if (handle_) {
            snd_pcm_close(handle_);
            handle_ = nullptr;
        }
    }

    bool start(AudioCallback callback) override {
        if (!handle_ || running_.load()) return false;
        callback_ = std::move(callback);
        running_.store(true);

        thread_ = std::thread([this]() { audio_loop(); });
        return true;
    }

    void stop() override {
        running_.store(false);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    bool is_running() const override {
        return running_.load();
    }

    const AudioStreamConfig& config() const override {
        return config_;
    }

private:
    bool open_device(const std::string& device_id, const AudioStreamConfig& config,
                     snd_pcm_stream_t stream) {
        config_ = config;
        is_capture_ = (stream == SND_PCM_STREAM_CAPTURE);

        const char* dev = device_id.empty() ? "default" : device_id.c_str();
        int err = snd_pcm_open(&handle_, dev, stream, 0);
        if (err < 0) {
            fprintf(stderr, "ALSA open error: %s\n", snd_strerror(err));
            return false;
        }

        snd_pcm_hw_params_t* params;
        snd_pcm_hw_params_alloca(&params);
        snd_pcm_hw_params_any(handle_, params);

        snd_pcm_hw_params_set_access(handle_, params, SND_PCM_ACCESS_RW_INTERLEAVED);

        // Try formats: FLOAT_LE > S24_3LE > S24_LE > S32_LE > S16_LE
        // S24_3LE (packed 24-bit) preferred for DACs with native 24-bit support
        // (+48dB dynamic range over S16). plughw: handles format conversion if needed.
        struct FormatOption {
            snd_pcm_format_t fmt;
            int bytes_per_sample;
            const char* name;
        };
        static const FormatOption formats[] = {
            {SND_PCM_FORMAT_FLOAT_LE, 4, "FLOAT_LE"},
            {SND_PCM_FORMAT_S24_3LE, 3, "S24_3LE"},
            {SND_PCM_FORMAT_S24_LE, 4, "S24_LE"},
            {SND_PCM_FORMAT_S32_LE, 4, "S32_LE"},
            {SND_PCM_FORMAT_S16_LE, 2, "S16_LE"},
        };
        hw_format_ = SND_PCM_FORMAT_UNKNOWN;
        for (const auto& f : formats) {
            snd_pcm_hw_params_any(handle_, params);
            snd_pcm_hw_params_set_access(handle_, params, SND_PCM_ACCESS_RW_INTERLEAVED);
            if (snd_pcm_hw_params_set_format(handle_, params, f.fmt) >= 0) {
                hw_format_ = f.fmt;
                bytes_per_sample_ = f.bytes_per_sample;
                fprintf(stderr, "ALSA: using format %s on '%s'\n", f.name, dev);
                break;
            }
        }
        if (hw_format_ == SND_PCM_FORMAT_UNKNOWN) {
            fprintf(stderr, "ALSA: no supported format on '%s'\n", dev);
            snd_pcm_close(handle_);
            handle_ = nullptr;
            return false;
        }

        snd_pcm_hw_params_set_channels(handle_, params, config.channels);

        unsigned int rate = config.sample_rate;
        snd_pcm_hw_params_set_rate_near(handle_, params, &rate, nullptr);

        snd_pcm_uframes_t period = config.frames_per_buffer;
        snd_pcm_hw_params_set_period_size_near(handle_, params, &period, nullptr);

        // Buffer multiplier: period<=48 → 2x (low-latency), larger → 8x (jitter headroom)
        unsigned int buf_mult = (period <= 48) ? 2 : 8;
        snd_pcm_uframes_t buffer_size = period * buf_mult;
        snd_pcm_hw_params_set_buffer_size_near(handle_, params, &buffer_size);

        err = snd_pcm_hw_params(handle_, params);
        if (err < 0) {
            fprintf(stderr, "ALSA hw_params error: %s\n", snd_strerror(err));
            snd_pcm_close(handle_);
            handle_ = nullptr;
            return false;
        }

        // Start playback after 1 period: minimizes silence gap on xrun recovery.
        // Without this, default start_threshold = buffer_size, meaning after an
        // xrun, ALSA waits until the entire buffer refills (80ms gap = audible click).
        if (stream == SND_PCM_STREAM_PLAYBACK) {
            snd_pcm_sw_params_t* sw_params;
            snd_pcm_sw_params_alloca(&sw_params);
            snd_pcm_sw_params_current(handle_, sw_params);
            snd_pcm_sw_params_set_start_threshold(handle_, sw_params, period);
            snd_pcm_sw_params(handle_, sw_params);
        }

        fprintf(stderr, "ALSA: period=%lu buffer=%lu (%ux) on '%s'\n",
                (unsigned long)period, (unsigned long)buffer_size, buf_mult, dev);

        return true;
    }

    // Convert hardware buffer to/from float
    void hw_to_float(const void* hw_buf, float* float_buf, size_t samples) {
        switch (hw_format_) {
        case SND_PCM_FORMAT_FLOAT_LE:
            std::memcpy(float_buf, hw_buf, samples * sizeof(float));
            break;
        case SND_PCM_FORMAT_S32_LE: {
            const auto* src = static_cast<const int32_t*>(hw_buf);
            for (size_t i = 0; i < samples; i++)
                float_buf[i] = static_cast<float>(src[i]) / 2147483647.0f;
            break;
        }
        case SND_PCM_FORMAT_S24_LE: {
            const auto* src = static_cast<const int32_t*>(hw_buf);
            for (size_t i = 0; i < samples; i++)
                float_buf[i] = static_cast<float>(src[i] >> 8) / 8388607.0f;
            break;
        }
        case SND_PCM_FORMAT_S24_3LE: {
            // Packed 24-bit: 3 bytes per sample, little-endian
            const auto* src = static_cast<const uint8_t*>(hw_buf);
            for (size_t i = 0; i < samples; i++) {
                int32_t s = src[i * 3] | (src[i * 3 + 1] << 8) | (src[i * 3 + 2] << 16);
                if (s & 0x800000) s |= 0xFF000000; // sign extend
                float_buf[i] = static_cast<float>(s) / 8388607.0f;
            }
            break;
        }
        case SND_PCM_FORMAT_S16_LE: {
            const auto* src = static_cast<const int16_t*>(hw_buf);
            for (size_t i = 0; i < samples; i++)
                float_buf[i] = static_cast<float>(src[i]) / 32767.0f;
            break;
        }
        default: break;
        }
    }

    void float_to_hw(const float* float_buf, void* hw_buf, size_t samples) {
        switch (hw_format_) {
        case SND_PCM_FORMAT_FLOAT_LE:
            std::memcpy(hw_buf, float_buf, samples * sizeof(float));
            break;
        case SND_PCM_FORMAT_S32_LE: {
            auto* dst = static_cast<int32_t*>(hw_buf);
            for (size_t i = 0; i < samples; i++) {
                float clamped = float_buf[i];
                if (clamped > 1.0f) clamped = 1.0f;
                else if (clamped < -1.0f) clamped = -1.0f;
                dst[i] = static_cast<int32_t>(clamped * 2147483647.0f);
            }
            break;
        }
        case SND_PCM_FORMAT_S24_LE: {
            auto* dst = static_cast<int32_t*>(hw_buf);
            for (size_t i = 0; i < samples; i++) {
                float clamped = float_buf[i];
                if (clamped > 1.0f) clamped = 1.0f;
                else if (clamped < -1.0f) clamped = -1.0f;
                dst[i] = static_cast<int32_t>(clamped * 8388607.0f) << 8;
            }
            break;
        }
        case SND_PCM_FORMAT_S24_3LE: {
            // Packed 24-bit: 3 bytes per sample, little-endian
            auto* dst = static_cast<uint8_t*>(hw_buf);
            for (size_t i = 0; i < samples; i++) {
                int32_t s = static_cast<int32_t>(float_buf[i] * 8388607.0f);
                if (s >  8388607) s =  8388607;
                if (s < -8388608) s = -8388608;
                dst[i * 3]     = static_cast<uint8_t>(s & 0xFF);
                dst[i * 3 + 1] = static_cast<uint8_t>((s >> 8) & 0xFF);
                dst[i * 3 + 2] = static_cast<uint8_t>((s >> 16) & 0xFF);
            }
            break;
        }
        case SND_PCM_FORMAT_S16_LE: {
            // TPDF dithering: reduces quantization distortion at low levels
            auto* dst = static_cast<int16_t*>(hw_buf);
            static uint32_t rs = 0x4D2A7F1Bu;
            for (size_t i = 0; i < samples; i++) {
                // Two xorshift32 values for triangular PDF noise
                rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
                float d1 = static_cast<float>(rs & 0xFFFF) * (1.0f / 65536.0f);
                rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
                float d2 = static_cast<float>(rs & 0xFFFF) * (1.0f / 65536.0f);
                float x = float_buf[i] * 32767.0f + (d1 - d2); // TPDF: [-1, +1]
                if (x >  32767.0f) x =  32767.0f;
                if (x < -32768.0f) x = -32768.0f;
                dst[i] = static_cast<int16_t>(x);
            }
            break;
        }
        default: break;
        }
    }

    void audio_loop() {
#ifdef __linux__
        // Real-time scheduling: prevent OS from preempting the audio thread
        struct sched_param sp{};
        sp.sched_priority = 80;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
            fprintf(stderr, "ALSA: SCHED_FIFO not granted (run: sudo setcap cap_sys_nice+ep solunad)\n");
        }
        // Pin to CPU core 3 (dedicated audio core, away from IRQ/system tasks)
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(3, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
        const size_t frames = config_.frames_per_buffer;
        const size_t samples = frames * config_.channels;
        std::vector<float> float_buf(samples);
        std::vector<uint8_t> hw_buf(samples * bytes_per_sample_);

        while (running_.load()) {
            if (is_capture_) {
                void* read_buf = (hw_format_ == SND_PCM_FORMAT_FLOAT_LE)
                    ? static_cast<void*>(float_buf.data()) : static_cast<void*>(hw_buf.data());
                snd_pcm_sframes_t n = snd_pcm_readi(handle_, read_buf, frames);
                if (n == -EPIPE) { snd_pcm_prepare(handle_); continue; }
                if (n > 0 && callback_) {
                    if (hw_format_ != SND_PCM_FORMAT_FLOAT_LE) {
                        hw_to_float(hw_buf.data(), float_buf.data(),
                            static_cast<size_t>(n) * config_.channels);
                    }
                    callback_(float_buf.data(), static_cast<uint32_t>(n));
                }
            } else {
                if (callback_) {
                    callback_(float_buf.data(), static_cast<uint32_t>(frames));
                }
                void* write_buf;
                if (hw_format_ == SND_PCM_FORMAT_FLOAT_LE) {
                    write_buf = float_buf.data();
                } else {
                    float_to_hw(float_buf.data(), hw_buf.data(), samples);
                    write_buf = hw_buf.data();
                }
                snd_pcm_sframes_t n = snd_pcm_writei(handle_, write_buf, frames);
                if (n == -EPIPE) {
                    xrun_count_++;
                    if (xrun_count_ <= 10 || (xrun_count_ % 100) == 0)
                        fprintf(stderr, "ALSA: xrun #%u (playback underrun)\n", xrun_count_);
                    snd_pcm_prepare(handle_);
                    snd_pcm_writei(handle_, write_buf, frames);
                }
            }
        }
    }

    snd_pcm_t* handle_ = nullptr;
    AudioStreamConfig config_;
    AudioCallback callback_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    bool is_capture_ = false;
    snd_pcm_format_t hw_format_ = SND_PCM_FORMAT_FLOAT_LE;
    int bytes_per_sample_ = 4;
    uint32_t xrun_count_ = 0;
};

std::vector<AudioDeviceInfo> AudioDevice::enumerate() {
    std::vector<AudioDeviceInfo> devices;

    void** hints = nullptr;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) {
        return devices;
    }

    for (void** hint = hints; *hint; ++hint) {
        char* name = snd_device_name_get_hint(*hint, "NAME");
        char* desc = snd_device_name_get_hint(*hint, "DESC");
        char* ioid = snd_device_name_get_hint(*hint, "IOID");

        if (name) {
            AudioDeviceInfo info;
            info.id = name;
            info.name = desc ? desc : name;
            info.max_input_channels = (!ioid || std::string(ioid) == "Input") ? 2 : 0;
            info.max_output_channels = (!ioid || std::string(ioid) == "Output") ? 2 : 0;
            info.supported_sample_rates = {44100, 48000, 96000};
            devices.push_back(std::move(info));
        }

        if (name) free(name);
        if (desc) free(desc);
        if (ioid) free(ioid);
    }

    snd_device_name_free_hint(hints);
    return devices;
}

std::unique_ptr<AudioDevice> AudioDevice::create() {
    return std::make_unique<AlsaAudioDevice>();
}

#endif // SOLUNA_HAS_ALSA

} // namespace soluna::pal
