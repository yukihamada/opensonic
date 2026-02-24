#include <soluna/pal/audio.h>

#ifdef SOLUNA_HAS_ALSA
#include <alsa/asoundlib.h>
#endif

#include <cstdio>
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>

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

        // Try formats in order of preference: FLOAT_LE > S32_LE > S24_LE > S16_LE
        struct FormatOption {
            snd_pcm_format_t fmt;
            int bytes_per_sample;
            const char* name;
        };
        static const FormatOption formats[] = {
            {SND_PCM_FORMAT_FLOAT_LE, 4, "FLOAT_LE"},
            {SND_PCM_FORMAT_S32_LE, 4, "S32_LE"},
            {SND_PCM_FORMAT_S24_LE, 4, "S24_LE"},
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

        snd_pcm_uframes_t buffer_size = period * 4;
        snd_pcm_hw_params_set_buffer_size_near(handle_, params, &buffer_size);

        err = snd_pcm_hw_params(handle_, params);
        if (err < 0) {
            fprintf(stderr, "ALSA hw_params error: %s\n", snd_strerror(err));
            snd_pcm_close(handle_);
            handle_ = nullptr;
            return false;
        }

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
            for (size_t i = 0; i < samples; i++)
                dst[i] = static_cast<int32_t>(float_buf[i] * 2147483647.0f);
            break;
        }
        case SND_PCM_FORMAT_S24_LE: {
            auto* dst = static_cast<int32_t*>(hw_buf);
            for (size_t i = 0; i < samples; i++)
                dst[i] = static_cast<int32_t>(float_buf[i] * 8388607.0f) << 8;
            break;
        }
        case SND_PCM_FORMAT_S16_LE: {
            auto* dst = static_cast<int16_t*>(hw_buf);
            for (size_t i = 0; i < samples; i++)
                dst[i] = static_cast<int16_t>(float_buf[i] * 32767.0f);
            break;
        }
        default: break;
        }
    }

    void audio_loop() {
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
                if (hw_format_ == SND_PCM_FORMAT_FLOAT_LE) {
                    snd_pcm_sframes_t n = snd_pcm_writei(handle_, float_buf.data(), frames);
                    if (n == -EPIPE) { snd_pcm_prepare(handle_); }
                } else {
                    float_to_hw(float_buf.data(), hw_buf.data(), samples);
                    snd_pcm_sframes_t n = snd_pcm_writei(handle_, hw_buf.data(), frames);
                    if (n == -EPIPE) { snd_pcm_prepare(handle_); }
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
