#include <soluna/pal/audio.h>

#ifdef SOLUNA_HAS_ALSA
#include <alsa/asoundlib.h>
#endif

#include <cstdio>
#include <atomic>
#include <thread>

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

        // Try FLOAT_LE first, fall back to S32_LE for hardware devices (e.g. I2S DACs)
        use_float_ = true;
        err = snd_pcm_hw_params_set_format(handle_, params, SND_PCM_FORMAT_FLOAT_LE);
        if (err < 0) {
            use_float_ = false;
            err = snd_pcm_hw_params_set_format(handle_, params, SND_PCM_FORMAT_S32_LE);
            if (err < 0) {
                fprintf(stderr, "ALSA: no supported format (tried FLOAT_LE, S32_LE)\n");
                snd_pcm_close(handle_);
                handle_ = nullptr;
                return false;
            }
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

    void audio_loop() {
        const size_t frames = config_.frames_per_buffer;
        const size_t buf_size = frames * config_.channels;
        std::vector<float> float_buf(buf_size);
        std::vector<int32_t> s32_buf(use_float_ ? 0 : buf_size);

        while (running_.load()) {
            if (is_capture_) {
                if (use_float_) {
                    snd_pcm_sframes_t n = snd_pcm_readi(handle_, float_buf.data(), frames);
                    if (n == -EPIPE) { snd_pcm_prepare(handle_); continue; }
                    if (n > 0 && callback_) {
                        callback_(float_buf.data(), static_cast<uint32_t>(n));
                    }
                } else {
                    snd_pcm_sframes_t n = snd_pcm_readi(handle_, s32_buf.data(), frames);
                    if (n == -EPIPE) { snd_pcm_prepare(handle_); continue; }
                    if (n > 0 && callback_) {
                        size_t samples = static_cast<size_t>(n) * config_.channels;
                        for (size_t i = 0; i < samples; i++) {
                            float_buf[i] = static_cast<float>(s32_buf[i]) / 2147483647.0f;
                        }
                        callback_(float_buf.data(), static_cast<uint32_t>(n));
                    }
                }
            } else {
                if (callback_) {
                    callback_(float_buf.data(), static_cast<uint32_t>(frames));
                }
                if (use_float_) {
                    snd_pcm_sframes_t n = snd_pcm_writei(handle_, float_buf.data(), frames);
                    if (n == -EPIPE) { snd_pcm_prepare(handle_); }
                } else {
                    size_t samples = frames * config_.channels;
                    for (size_t i = 0; i < samples; i++) {
                        s32_buf[i] = static_cast<int32_t>(float_buf[i] * 2147483647.0f);
                    }
                    snd_pcm_sframes_t n = snd_pcm_writei(handle_, s32_buf.data(), frames);
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
    bool use_float_ = true;
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
