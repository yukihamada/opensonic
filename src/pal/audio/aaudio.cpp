/**
 * Soluna — Android Audio Backend
 *
 * AAudio for Android 8.0+ (preferred, low-latency)
 * OpenSL ES fallback for older Android versions
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/pal/audio.h>

#if defined(__ANDROID__)

#include <android/api-level.h>

// AAudio requires API level 26 (Android 8.0)
#if __ANDROID_API__ >= 26
#define SOLUNA_HAS_AAUDIO 1
#include <aaudio/AAudio.h>
#endif

// OpenSL ES fallback for older devices
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace soluna::pal {

#ifdef SOLUNA_HAS_AAUDIO

/**
 * AAudio implementation for Android 8.0+
 * Provides low-latency audio with callback-based I/O
 */
class AAudioDevice : public AudioDevice {
public:
    ~AAudioDevice() override {
        close();
    }

    bool open_input(const std::string& device_id, const AudioStreamConfig& config) override {
        return open_stream(device_id, config, AAUDIO_DIRECTION_INPUT);
    }

    bool open_output(const std::string& device_id, const AudioStreamConfig& config) override {
        return open_stream(device_id, config, AAUDIO_DIRECTION_OUTPUT);
    }

    void close() override {
        stop();
        if (stream_) {
            AAudioStream_close(stream_);
            stream_ = nullptr;
        }
    }

    bool start(AudioCallback callback) override {
        if (!stream_ || running_.load()) return false;
        callback_ = std::move(callback);
        running_.store(true);

        aaudio_result_t result = AAudioStream_requestStart(stream_);
        if (result != AAUDIO_OK) {
            fprintf(stderr, "AAudio start error: %s\n", AAudio_convertResultToText(result));
            running_.store(false);
            return false;
        }
        return true;
    }

    void stop() override {
        if (!running_.load()) return;
        running_.store(false);

        if (stream_) {
            AAudioStream_requestStop(stream_);
            // Wait for stream to stop
            aaudio_stream_state_t state = AAUDIO_STREAM_STATE_UNKNOWN;
            AAudioStream_waitForStateChange(stream_, AAUDIO_STREAM_STATE_STOPPING,
                                            &state, 1000000000LL); // 1 second timeout
        }
    }

    bool is_running() const override {
        return running_.load();
    }

    const AudioStreamConfig& config() const override {
        return config_;
    }

private:
    bool open_stream(const std::string& device_id, const AudioStreamConfig& config,
                     aaudio_direction_t direction) {
        config_ = config;
        is_capture_ = (direction == AAUDIO_DIRECTION_INPUT);

        AAudioStreamBuilder* builder = nullptr;
        aaudio_result_t result = AAudio_createStreamBuilder(&builder);
        if (result != AAUDIO_OK) {
            fprintf(stderr, "AAudio createStreamBuilder error: %s\n",
                    AAudio_convertResultToText(result));
            return false;
        }

        // Configure stream
        AAudioStreamBuilder_setDirection(builder, direction);
        AAudioStreamBuilder_setSampleRate(builder, config.sample_rate);
        AAudioStreamBuilder_setChannelCount(builder, config.channels);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
        AAudioStreamBuilder_setFramesPerDataCallback(builder, config.frames_per_buffer);

        // Set device ID if specified
        if (!device_id.empty()) {
            int32_t dev_id = std::stoi(device_id);
            AAudioStreamBuilder_setDeviceId(builder, dev_id);
        }

        // Set callback
        AAudioStreamBuilder_setDataCallback(builder, data_callback, this);
        AAudioStreamBuilder_setErrorCallback(builder, error_callback, this);

        // Open stream
        result = AAudioStreamBuilder_openStream(builder, &stream_);
        AAudioStreamBuilder_delete(builder);

        if (result != AAUDIO_OK) {
            fprintf(stderr, "AAudio openStream error: %s\n",
                    AAudio_convertResultToText(result));
            return false;
        }

        // Verify configuration
        if (AAudioStream_getSampleRate(stream_) != static_cast<int32_t>(config.sample_rate)) {
            fprintf(stderr, "AAudio: sample rate mismatch\n");
        }

        return true;
    }

    static aaudio_data_callback_result_t data_callback(
            AAudioStream* stream, void* user_data,
            void* audio_data, int32_t num_frames) {
        auto* device = static_cast<AAudioDevice*>(user_data);
        if (!device->running_.load()) {
            return AAUDIO_CALLBACK_RESULT_STOP;
        }

        auto* buffer = static_cast<float*>(audio_data);

        if (device->is_capture_) {
            // Input: pass captured data to callback
            if (device->callback_) {
                device->callback_(buffer, static_cast<uint32_t>(num_frames));
            }
        } else {
            // Output: fill buffer with callback data
            if (device->callback_) {
                device->callback_(buffer, static_cast<uint32_t>(num_frames));
            } else {
                // Silence if no callback
                std::memset(buffer, 0, num_frames * device->config_.channels * sizeof(float));
            }
        }

        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    static void error_callback(AAudioStream* stream, void* user_data, aaudio_result_t error) {
        auto* device = static_cast<AAudioDevice*>(user_data);
        fprintf(stderr, "AAudio error: %s\n", AAudio_convertResultToText(error));

        // Handle disconnect by stopping
        if (error == AAUDIO_ERROR_DISCONNECTED) {
            device->running_.store(false);
        }
    }

    AAudioStream* stream_ = nullptr;
    AudioStreamConfig config_;
    AudioCallback callback_;
    std::atomic<bool> running_{false};
    bool is_capture_ = false;
};

std::vector<AudioDeviceInfo> AudioDevice::enumerate() {
    std::vector<AudioDeviceInfo> devices;

    // AAudio doesn't provide device enumeration directly
    // Add default devices
    AudioDeviceInfo default_output;
    default_output.id = "0";
    default_output.name = "Default Output";
    default_output.max_input_channels = 0;
    default_output.max_output_channels = 2;
    default_output.supported_sample_rates = {44100, 48000};
    devices.push_back(std::move(default_output));

    AudioDeviceInfo default_input;
    default_input.id = "1";
    default_input.name = "Default Input";
    default_input.max_input_channels = 2;
    default_input.max_output_channels = 0;
    default_input.supported_sample_rates = {44100, 48000};
    devices.push_back(std::move(default_input));

    return devices;
}

std::unique_ptr<AudioDevice> AudioDevice::create() {
    return std::make_unique<AAudioDevice>();
}

#else // OpenSL ES fallback

/**
 * OpenSL ES implementation for older Android devices (< 8.0)
 */
class OpenSLDevice : public AudioDevice {
public:
    ~OpenSLDevice() override {
        close();
    }

    bool open_input(const std::string& device_id, const AudioStreamConfig& config) override {
        return open_device(device_id, config, true);
    }

    bool open_output(const std::string& device_id, const AudioStreamConfig& config) override {
        return open_device(device_id, config, false);
    }

    void close() override {
        stop();

        if (player_obj_) {
            (*player_obj_)->Destroy(player_obj_);
            player_obj_ = nullptr;
            player_play_ = nullptr;
            player_queue_ = nullptr;
        }

        if (recorder_obj_) {
            (*recorder_obj_)->Destroy(recorder_obj_);
            recorder_obj_ = nullptr;
            recorder_record_ = nullptr;
            recorder_queue_ = nullptr;
        }

        if (output_mix_) {
            (*output_mix_)->Destroy(output_mix_);
            output_mix_ = nullptr;
        }

        if (engine_obj_) {
            (*engine_obj_)->Destroy(engine_obj_);
            engine_obj_ = nullptr;
            engine_ = nullptr;
        }
    }

    bool start(AudioCallback callback) override {
        if (running_.load()) return false;
        callback_ = std::move(callback);
        running_.store(true);

        if (is_capture_) {
            // Enqueue empty buffer to start recording
            (*recorder_queue_)->Enqueue(recorder_queue_,
                                         buffers_[current_buffer_].data(),
                                         buffer_size_bytes_);
            (*recorder_record_)->SetRecordState(recorder_record_, SL_RECORDSTATE_RECORDING);
        } else {
            // Fill initial buffers
            for (int i = 0; i < kNumBuffers; i++) {
                fill_buffer(i);
                (*player_queue_)->Enqueue(player_queue_,
                                          buffers_[i].data(),
                                          buffer_size_bytes_);
            }
            (*player_play_)->SetPlayState(player_play_, SL_PLAYSTATE_PLAYING);
        }

        return true;
    }

    void stop() override {
        if (!running_.load()) return;
        running_.store(false);

        if (is_capture_ && recorder_record_) {
            (*recorder_record_)->SetRecordState(recorder_record_, SL_RECORDSTATE_STOPPED);
        }
        if (!is_capture_ && player_play_) {
            (*player_play_)->SetPlayState(player_play_, SL_PLAYSTATE_STOPPED);
        }
    }

    bool is_running() const override {
        return running_.load();
    }

    const AudioStreamConfig& config() const override {
        return config_;
    }

private:
    static constexpr int kNumBuffers = 2;

    bool open_device(const std::string& device_id, const AudioStreamConfig& config, bool capture) {
        (void)device_id; // OpenSL ES uses default devices
        config_ = config;
        is_capture_ = capture;

        buffer_size_bytes_ = config.frames_per_buffer * config.channels * sizeof(int16_t);
        for (auto& buf : buffers_) {
            buf.resize(config.frames_per_buffer * config.channels);
        }
        float_buffer_.resize(config.frames_per_buffer * config.channels);

        // Create engine
        SLresult result = slCreateEngine(&engine_obj_, 0, nullptr, 0, nullptr, nullptr);
        if (result != SL_RESULT_SUCCESS) return false;

        result = (*engine_obj_)->Realize(engine_obj_, SL_BOOLEAN_FALSE);
        if (result != SL_RESULT_SUCCESS) return false;

        result = (*engine_obj_)->GetInterface(engine_obj_, SL_IID_ENGINE, &engine_);
        if (result != SL_RESULT_SUCCESS) return false;

        if (capture) {
            return create_recorder();
        } else {
            return create_player();
        }
    }

    bool create_player() {
        // Create output mix
        SLresult result = (*engine_)->CreateOutputMix(engine_, &output_mix_, 0, nullptr, nullptr);
        if (result != SL_RESULT_SUCCESS) return false;

        result = (*output_mix_)->Realize(output_mix_, SL_BOOLEAN_FALSE);
        if (result != SL_RESULT_SUCCESS) return false;

        // Configure audio source
        SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {
            SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, kNumBuffers
        };

        SLDataFormat_PCM format_pcm = {
            SL_DATAFORMAT_PCM,
            config_.channels,
            config_.sample_rate * 1000, // milliHz
            SL_PCMSAMPLEFORMAT_FIXED_16,
            SL_PCMSAMPLEFORMAT_FIXED_16,
            config_.channels == 1 ? SL_SPEAKER_FRONT_CENTER :
                (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT),
            SL_BYTEORDER_LITTLEENDIAN
        };

        SLDataSource audio_src = {&loc_bufq, &format_pcm};

        // Configure audio sink
        SLDataLocator_OutputMix loc_outmix = {SL_DATALOCATOR_OUTPUTMIX, output_mix_};
        SLDataSink audio_snk = {&loc_outmix, nullptr};

        // Create player
        const SLInterfaceID ids[] = {SL_IID_BUFFERQUEUE};
        const SLboolean req[] = {SL_BOOLEAN_TRUE};

        result = (*engine_)->CreateAudioPlayer(engine_, &player_obj_,
                                                &audio_src, &audio_snk,
                                                1, ids, req);
        if (result != SL_RESULT_SUCCESS) return false;

        result = (*player_obj_)->Realize(player_obj_, SL_BOOLEAN_FALSE);
        if (result != SL_RESULT_SUCCESS) return false;

        result = (*player_obj_)->GetInterface(player_obj_, SL_IID_PLAY, &player_play_);
        if (result != SL_RESULT_SUCCESS) return false;

        result = (*player_obj_)->GetInterface(player_obj_, SL_IID_BUFFERQUEUE, &player_queue_);
        if (result != SL_RESULT_SUCCESS) return false;

        // Register callback
        result = (*player_queue_)->RegisterCallback(player_queue_, player_callback, this);
        return result == SL_RESULT_SUCCESS;
    }

    bool create_recorder() {
        // Configure audio source (microphone)
        SLDataLocator_IODevice loc_dev = {
            SL_DATALOCATOR_IODEVICE,
            SL_IODEVICE_AUDIOINPUT,
            SL_DEFAULTDEVICEID_AUDIOINPUT,
            nullptr
        };
        SLDataSource audio_src = {&loc_dev, nullptr};

        // Configure audio sink
        SLDataLocator_AndroidSimpleBufferQueue loc_bq = {
            SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, kNumBuffers
        };

        SLDataFormat_PCM format_pcm = {
            SL_DATAFORMAT_PCM,
            config_.channels,
            config_.sample_rate * 1000,
            SL_PCMSAMPLEFORMAT_FIXED_16,
            SL_PCMSAMPLEFORMAT_FIXED_16,
            config_.channels == 1 ? SL_SPEAKER_FRONT_CENTER :
                (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT),
            SL_BYTEORDER_LITTLEENDIAN
        };

        SLDataSink audio_snk = {&loc_bq, &format_pcm};

        // Create recorder
        const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
        const SLboolean req[] = {SL_BOOLEAN_TRUE};

        SLresult result = (*engine_)->CreateAudioRecorder(engine_, &recorder_obj_,
                                                          &audio_src, &audio_snk,
                                                          1, ids, req);
        if (result != SL_RESULT_SUCCESS) return false;

        result = (*recorder_obj_)->Realize(recorder_obj_, SL_BOOLEAN_FALSE);
        if (result != SL_RESULT_SUCCESS) return false;

        result = (*recorder_obj_)->GetInterface(recorder_obj_, SL_IID_RECORD, &recorder_record_);
        if (result != SL_RESULT_SUCCESS) return false;

        result = (*recorder_obj_)->GetInterface(recorder_obj_,
                                                 SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                                 &recorder_queue_);
        if (result != SL_RESULT_SUCCESS) return false;

        // Register callback
        result = (*recorder_queue_)->RegisterCallback(recorder_queue_, recorder_callback, this);
        return result == SL_RESULT_SUCCESS;
    }

    void fill_buffer(int index) {
        if (callback_) {
            callback_(float_buffer_.data(), config_.frames_per_buffer);

            // Convert float to int16
            for (size_t i = 0; i < float_buffer_.size(); i++) {
                float sample = float_buffer_[i];
                if (sample > 1.0f) sample = 1.0f;
                if (sample < -1.0f) sample = -1.0f;
                buffers_[index][i] = static_cast<int16_t>(sample * 32767.0f);
            }
        } else {
            std::memset(buffers_[index].data(), 0, buffer_size_bytes_);
        }
    }

    void process_recorded(int index) {
        if (callback_) {
            // Convert int16 to float
            for (size_t i = 0; i < float_buffer_.size(); i++) {
                float_buffer_[i] = buffers_[index][i] / 32768.0f;
            }
            callback_(float_buffer_.data(), config_.frames_per_buffer);
        }
    }

    static void player_callback(SLAndroidSimpleBufferQueueItf bq, void* context) {
        auto* device = static_cast<OpenSLDevice*>(context);
        if (!device->running_.load()) return;

        device->current_buffer_ = (device->current_buffer_ + 1) % kNumBuffers;
        device->fill_buffer(device->current_buffer_);

        (*bq)->Enqueue(bq, device->buffers_[device->current_buffer_].data(),
                       device->buffer_size_bytes_);
    }

    static void recorder_callback(SLAndroidSimpleBufferQueueItf bq, void* context) {
        auto* device = static_cast<OpenSLDevice*>(context);
        if (!device->running_.load()) return;

        device->process_recorded(device->current_buffer_);
        device->current_buffer_ = (device->current_buffer_ + 1) % kNumBuffers;

        (*bq)->Enqueue(bq, device->buffers_[device->current_buffer_].data(),
                       device->buffer_size_bytes_);
    }

    // OpenSL ES objects
    SLObjectItf engine_obj_ = nullptr;
    SLEngineItf engine_ = nullptr;
    SLObjectItf output_mix_ = nullptr;
    SLObjectItf player_obj_ = nullptr;
    SLPlayItf player_play_ = nullptr;
    SLAndroidSimpleBufferQueueItf player_queue_ = nullptr;
    SLObjectItf recorder_obj_ = nullptr;
    SLRecordItf recorder_record_ = nullptr;
    SLAndroidSimpleBufferQueueItf recorder_queue_ = nullptr;

    // Buffers
    std::vector<int16_t> buffers_[kNumBuffers];
    std::vector<float> float_buffer_;
    size_t buffer_size_bytes_ = 0;
    int current_buffer_ = 0;

    AudioStreamConfig config_;
    AudioCallback callback_;
    std::atomic<bool> running_{false};
    bool is_capture_ = false;
};

std::vector<AudioDeviceInfo> AudioDevice::enumerate() {
    std::vector<AudioDeviceInfo> devices;

    AudioDeviceInfo default_output;
    default_output.id = "default_output";
    default_output.name = "Default Output";
    default_output.max_input_channels = 0;
    default_output.max_output_channels = 2;
    default_output.supported_sample_rates = {44100, 48000};
    devices.push_back(std::move(default_output));

    AudioDeviceInfo default_input;
    default_input.id = "default_input";
    default_input.name = "Default Input";
    default_input.max_input_channels = 2;
    default_input.max_output_channels = 0;
    default_input.supported_sample_rates = {44100, 48000};
    devices.push_back(std::move(default_input));

    return devices;
}

std::unique_ptr<AudioDevice> AudioDevice::create() {
    return std::make_unique<OpenSLDevice>();
}

#endif // SOLUNA_HAS_AAUDIO

} // namespace soluna::pal

#endif // __ANDROID__
