/**
 * Soluna — iOS/macOS CoreAudio Backend
 *
 * Audio Unit API for low-latency audio on Apple platforms.
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/pal/audio.h>

#if defined(__APPLE__)

#include <TargetConditionals.h>

#if TARGET_OS_IPHONE || TARGET_OS_MAC

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>

#if TARGET_OS_IPHONE
#include <AVFoundation/AVFoundation.h>
#endif

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace soluna::pal {

/**
 * CoreAudio implementation using Audio Unit API
 * Works on both iOS and macOS
 */
class CoreAudioDevice : public AudioDevice {
public:
    ~CoreAudioDevice() override {
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
        if (audio_unit_) {
            AudioComponentInstanceDispose(audio_unit_);
            audio_unit_ = nullptr;
        }
    }

    bool start(AudioCallback callback) override {
        if (!audio_unit_ || running_.load()) return false;
        callback_ = std::move(callback);
        running_.store(true);

        OSStatus status = AudioOutputUnitStart(audio_unit_);
        if (status != noErr) {
            fprintf(stderr, "CoreAudio start error: %d\n", (int)status);
            running_.store(false);
            return false;
        }
        return true;
    }

    void stop() override {
        if (!running_.load()) return;
        running_.store(false);

        if (audio_unit_) {
            AudioOutputUnitStop(audio_unit_);
        }
    }

    bool is_running() const override {
        return running_.load();
    }

    const AudioStreamConfig& config() const override {
        return config_;
    }

private:
#if TARGET_OS_MAC && !TARGET_OS_IPHONE
    // Find a device by name, returns 0 if not found
    static AudioDeviceID find_device_by_name(const std::string& name, bool is_input) {
        AudioObjectPropertyAddress property_address = {
            kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };

        UInt32 data_size = 0;
        OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,
                                                          &property_address,
                                                          0, nullptr,
                                                          &data_size);
        if (status != noErr) return 0;

        UInt32 device_count = data_size / sizeof(AudioDeviceID);
        std::vector<AudioDeviceID> device_ids(device_count);

        status = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                            &property_address,
                                            0, nullptr,
                                            &data_size,
                                            device_ids.data());
        if (status != noErr) return 0;

        for (AudioDeviceID dev_id : device_ids) {
            // Get device name
            CFStringRef name_ref = nullptr;
            data_size = sizeof(name_ref);
            property_address.mSelector = kAudioDevicePropertyDeviceNameCFString;
            property_address.mScope = kAudioObjectPropertyScopeGlobal;

            status = AudioObjectGetPropertyData(dev_id, &property_address,
                                                0, nullptr, &data_size, &name_ref);
            if (status == noErr && name_ref) {
                char name_buf[256];
                CFStringGetCString(name_ref, name_buf, sizeof(name_buf), kCFStringEncodingUTF8);
                CFRelease(name_ref);

                if (name == name_buf) {
                    // Verify device has the right direction
                    property_address.mSelector = kAudioDevicePropertyStreamConfiguration;
                    property_address.mScope = is_input ?
                        kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput;
                    data_size = 0;
                    status = AudioObjectGetPropertyDataSize(dev_id, &property_address,
                                                            0, nullptr, &data_size);
                    if (status == noErr && data_size > sizeof(AudioBufferList)) {
                        return dev_id;
                    }
                }
            }
        }
        return 0;
    }
#endif

    bool open_device(const std::string& device_id, const AudioStreamConfig& config, bool capture) {
        config_ = config;
        is_capture_ = capture;

        // Allocate conversion buffer
        conversion_buffer_.resize(config.frames_per_buffer * config.channels);

#if TARGET_OS_IPHONE
        // Configure audio session for iOS
        if (!configure_ios_audio_session(capture)) {
            return false;
        }
#endif

        // Describe the Audio Unit
        AudioComponentDescription desc = {};
#if TARGET_OS_IPHONE
        desc.componentType = kAudioUnitType_Output;
        desc.componentSubType = kAudioUnitSubType_RemoteIO;
#else
        desc.componentType = kAudioUnitType_Output;
        desc.componentSubType = kAudioUnitSubType_HALOutput;
#endif
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;

        AudioComponent component = AudioComponentFindNext(nullptr, &desc);
        if (!component) {
            fprintf(stderr, "CoreAudio: Audio component not found\n");
            return false;
        }

        OSStatus status = AudioComponentInstanceNew(component, &audio_unit_);
        if (status != noErr) {
            fprintf(stderr, "CoreAudio: Failed to create audio unit: %d\n", (int)status);
            return false;
        }

        // Enable input or output
        UInt32 enable_io = 1;
        UInt32 disable_io = 0;

        if (capture) {
            // Enable input
            status = AudioUnitSetProperty(audio_unit_,
                                          kAudioOutputUnitProperty_EnableIO,
                                          kAudioUnitScope_Input,
                                          1, // Input bus
                                          &enable_io,
                                          sizeof(enable_io));
            if (status != noErr) {
                fprintf(stderr, "CoreAudio: Failed to enable input: %d\n", (int)status);
                AudioComponentInstanceDispose(audio_unit_);
                audio_unit_ = nullptr;
                return false;
            }

            // Disable output
            status = AudioUnitSetProperty(audio_unit_,
                                          kAudioOutputUnitProperty_EnableIO,
                                          kAudioUnitScope_Output,
                                          0, // Output bus
                                          &disable_io,
                                          sizeof(disable_io));
        } else {
            // Enable output (already enabled by default on output bus)
            // Disable input
            status = AudioUnitSetProperty(audio_unit_,
                                          kAudioOutputUnitProperty_EnableIO,
                                          kAudioUnitScope_Input,
                                          1,
                                          &disable_io,
                                          sizeof(disable_io));
        }

#if TARGET_OS_MAC && !TARGET_OS_IPHONE
        // Set device on macOS
        if (!device_id.empty() && device_id != "default") {
            AudioDeviceID dev_id = 0;

            // Try to parse as numeric ID first
            try {
                dev_id = static_cast<AudioDeviceID>(std::stoul(device_id));
            } catch (const std::exception&) {
                // Not a number, try to find by name
                dev_id = find_device_by_name(device_id, capture);
            }

            if (dev_id != 0) {
                status = AudioUnitSetProperty(audio_unit_,
                                              kAudioOutputUnitProperty_CurrentDevice,
                                              kAudioUnitScope_Global,
                                              0,
                                              &dev_id,
                                              sizeof(dev_id));
                if (status != noErr) {
                    fprintf(stderr, "CoreAudio: Failed to set device: %d\n", (int)status);
                }
            }
        }
#else
        (void)device_id;
#endif

        // Set stream format
        AudioStreamBasicDescription stream_format = {};
        stream_format.mSampleRate = config.sample_rate;
        stream_format.mFormatID = kAudioFormatLinearPCM;
        stream_format.mFormatFlags = kAudioFormatFlagIsFloat |
                                     kAudioFormatFlagIsPacked |
                                     kAudioFormatFlagIsNonInterleaved;
        stream_format.mBytesPerPacket = sizeof(float);
        stream_format.mFramesPerPacket = 1;
        stream_format.mBytesPerFrame = sizeof(float);
        stream_format.mChannelsPerFrame = config.channels;
        stream_format.mBitsPerChannel = 32;

        // Set format on appropriate scope/element
        UInt32 scope = capture ? kAudioUnitScope_Output : kAudioUnitScope_Input;
        UInt32 element = capture ? 1 : 0;

        status = AudioUnitSetProperty(audio_unit_,
                                      kAudioUnitProperty_StreamFormat,
                                      scope,
                                      element,
                                      &stream_format,
                                      sizeof(stream_format));
        if (status != noErr) {
            fprintf(stderr, "CoreAudio: Failed to set stream format: %d\n", (int)status);
            AudioComponentInstanceDispose(audio_unit_);
            audio_unit_ = nullptr;
            return false;
        }

        // Set buffer size
        UInt32 buffer_frames = config.frames_per_buffer;
        status = AudioUnitSetProperty(audio_unit_,
                                      kAudioUnitProperty_MaximumFramesPerSlice,
                                      kAudioUnitScope_Global,
                                      0,
                                      &buffer_frames,
                                      sizeof(buffer_frames));

        // Set callback
        AURenderCallbackStruct callback_struct = {};
        if (capture) {
            callback_struct.inputProc = input_callback;
            callback_struct.inputProcRefCon = this;
            status = AudioUnitSetProperty(audio_unit_,
                                          kAudioOutputUnitProperty_SetInputCallback,
                                          kAudioUnitScope_Global,
                                          0,
                                          &callback_struct,
                                          sizeof(callback_struct));
        } else {
            callback_struct.inputProc = render_callback;
            callback_struct.inputProcRefCon = this;
            status = AudioUnitSetProperty(audio_unit_,
                                          kAudioUnitProperty_SetRenderCallback,
                                          kAudioUnitScope_Input,
                                          0,
                                          &callback_struct,
                                          sizeof(callback_struct));
        }

        if (status != noErr) {
            fprintf(stderr, "CoreAudio: Failed to set callback: %d\n", (int)status);
            AudioComponentInstanceDispose(audio_unit_);
            audio_unit_ = nullptr;
            return false;
        }

        // Initialize
        status = AudioUnitInitialize(audio_unit_);
        if (status != noErr) {
            fprintf(stderr, "CoreAudio: Failed to initialize: %d\n", (int)status);
            AudioComponentInstanceDispose(audio_unit_);
            audio_unit_ = nullptr;
            return false;
        }

        return true;
    }

#if TARGET_OS_IPHONE
    bool configure_ios_audio_session(bool capture) {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        NSError* error = nil;

        // Set category
        AVAudioSessionCategory category = capture ?
            AVAudioSessionCategoryPlayAndRecord : AVAudioSessionCategoryPlayback;
        AVAudioSessionCategoryOptions options = capture ?
            (AVAudioSessionCategoryOptionDefaultToSpeaker |
             AVAudioSessionCategoryOptionAllowBluetooth) : 0;

        if (![session setCategory:category withOptions:options error:&error]) {
            fprintf(stderr, "CoreAudio: Failed to set audio session category: %s\n",
                    error.localizedDescription.UTF8String);
            return false;
        }

        // Set preferred sample rate
        if (![session setPreferredSampleRate:config_.sample_rate error:&error]) {
            fprintf(stderr, "CoreAudio: Failed to set sample rate: %s\n",
                    error.localizedDescription.UTF8String);
        }

        // Set preferred buffer duration (latency)
        NSTimeInterval buffer_duration = (double)config_.frames_per_buffer / config_.sample_rate;
        if (![session setPreferredIOBufferDuration:buffer_duration error:&error]) {
            fprintf(stderr, "CoreAudio: Failed to set buffer duration: %s\n",
                    error.localizedDescription.UTF8String);
        }

        // Activate session
        if (![session setActive:YES error:&error]) {
            fprintf(stderr, "CoreAudio: Failed to activate audio session: %s\n",
                    error.localizedDescription.UTF8String);
            return false;
        }

        return true;
    }
#endif

    static OSStatus render_callback(void* inRefCon,
                                    AudioUnitRenderActionFlags* ioActionFlags,
                                    const AudioTimeStamp* inTimeStamp,
                                    UInt32 inBusNumber,
                                    UInt32 inNumberFrames,
                                    AudioBufferList* ioData) {
        (void)ioActionFlags;
        (void)inTimeStamp;
        (void)inBusNumber;

        auto* device = static_cast<CoreAudioDevice*>(inRefCon);
        if (!device->running_.load()) {
            // Fill with silence
            for (UInt32 i = 0; i < ioData->mNumberBuffers; i++) {
                std::memset(ioData->mBuffers[i].mData, 0, ioData->mBuffers[i].mDataByteSize);
            }
            return noErr;
        }

        // Get interleaved data from callback
        if (device->callback_) {
            device->callback_(device->conversion_buffer_.data(), inNumberFrames);
        } else {
            std::memset(device->conversion_buffer_.data(), 0,
                        inNumberFrames * device->config_.channels * sizeof(float));
        }

        // Deinterleave to non-interleaved buffer list
        for (UInt32 ch = 0; ch < ioData->mNumberBuffers && ch < device->config_.channels; ch++) {
            auto* dst = static_cast<float*>(ioData->mBuffers[ch].mData);
            for (UInt32 frame = 0; frame < inNumberFrames; frame++) {
                dst[frame] = device->conversion_buffer_[frame * device->config_.channels + ch];
            }
        }

        return noErr;
    }

    static OSStatus input_callback(void* inRefCon,
                                   AudioUnitRenderActionFlags* ioActionFlags,
                                   const AudioTimeStamp* inTimeStamp,
                                   UInt32 inBusNumber,
                                   UInt32 inNumberFrames,
                                   AudioBufferList* ioData) {
        (void)ioData;

        auto* device = static_cast<CoreAudioDevice*>(inRefCon);
        if (!device->running_.load()) {
            return noErr;
        }

        // Prepare buffer list for render
        AudioBufferList buffer_list;
        buffer_list.mNumberBuffers = device->config_.channels;

        std::vector<std::vector<float>> channel_buffers(device->config_.channels);
        for (UInt32 ch = 0; ch < device->config_.channels; ch++) {
            channel_buffers[ch].resize(inNumberFrames);
            buffer_list.mBuffers[ch].mNumberChannels = 1;
            buffer_list.mBuffers[ch].mDataByteSize = inNumberFrames * sizeof(float);
            buffer_list.mBuffers[ch].mData = channel_buffers[ch].data();
        }

        // Render input
        OSStatus status = AudioUnitRender(device->audio_unit_,
                                          ioActionFlags,
                                          inTimeStamp,
                                          inBusNumber,
                                          inNumberFrames,
                                          &buffer_list);
        if (status != noErr) {
            return status;
        }

        // Interleave to conversion buffer
        for (UInt32 frame = 0; frame < inNumberFrames; frame++) {
            for (UInt32 ch = 0; ch < device->config_.channels; ch++) {
                device->conversion_buffer_[frame * device->config_.channels + ch] =
                    channel_buffers[ch][frame];
            }
        }

        // Call user callback with interleaved data
        if (device->callback_) {
            device->callback_(device->conversion_buffer_.data(), inNumberFrames);
        }

        return noErr;
    }

    AudioUnit audio_unit_ = nullptr;
    AudioStreamConfig config_;
    AudioCallback callback_;
    std::atomic<bool> running_{false};
    bool is_capture_ = false;
    std::vector<float> conversion_buffer_;
};

std::vector<AudioDeviceInfo> AudioDevice::enumerate() {
    std::vector<AudioDeviceInfo> devices;

#if TARGET_OS_MAC && !TARGET_OS_IPHONE
    // macOS: enumerate actual devices
    AudioObjectPropertyAddress property_address = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 data_size = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,
                                                      &property_address,
                                                      0, nullptr,
                                                      &data_size);
    if (status != noErr) {
        return devices;
    }

    UInt32 device_count = data_size / sizeof(AudioDeviceID);
    std::vector<AudioDeviceID> device_ids(device_count);

    status = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                        &property_address,
                                        0, nullptr,
                                        &data_size,
                                        device_ids.data());
    if (status != noErr) {
        return devices;
    }

    for (AudioDeviceID dev_id : device_ids) {
        AudioDeviceInfo info;
        info.id = std::to_string(dev_id);

        // Get device name
        CFStringRef name_ref = nullptr;
        data_size = sizeof(name_ref);
        property_address.mSelector = kAudioDevicePropertyDeviceNameCFString;
        property_address.mScope = kAudioObjectPropertyScopeGlobal;

        status = AudioObjectGetPropertyData(dev_id, &property_address,
                                            0, nullptr, &data_size, &name_ref);
        if (status == noErr && name_ref) {
            char name_buf[256];
            CFStringGetCString(name_ref, name_buf, sizeof(name_buf), kCFStringEncodingUTF8);
            info.name = name_buf;
            CFRelease(name_ref);
        } else {
            info.name = "Unknown Device";
        }

        // Get input channels
        property_address.mSelector = kAudioDevicePropertyStreamConfiguration;
        property_address.mScope = kAudioDevicePropertyScopeInput;
        data_size = 0;
        status = AudioObjectGetPropertyDataSize(dev_id, &property_address, 0, nullptr, &data_size);
        if (status == noErr && data_size > 0) {
            std::vector<uint8_t> buffer(data_size);
            auto* buffer_list = reinterpret_cast<AudioBufferList*>(buffer.data());
            status = AudioObjectGetPropertyData(dev_id, &property_address,
                                                0, nullptr, &data_size, buffer_list);
            if (status == noErr) {
                info.max_input_channels = 0;
                for (UInt32 i = 0; i < buffer_list->mNumberBuffers; i++) {
                    info.max_input_channels += buffer_list->mBuffers[i].mNumberChannels;
                }
            }
        }

        // Get output channels
        property_address.mScope = kAudioDevicePropertyScopeOutput;
        data_size = 0;
        status = AudioObjectGetPropertyDataSize(dev_id, &property_address, 0, nullptr, &data_size);
        if (status == noErr && data_size > 0) {
            std::vector<uint8_t> buffer(data_size);
            auto* buffer_list = reinterpret_cast<AudioBufferList*>(buffer.data());
            status = AudioObjectGetPropertyData(dev_id, &property_address,
                                                0, nullptr, &data_size, buffer_list);
            if (status == noErr) {
                info.max_output_channels = 0;
                for (UInt32 i = 0; i < buffer_list->mNumberBuffers; i++) {
                    info.max_output_channels += buffer_list->mBuffers[i].mNumberChannels;
                }
            }
        }

        // Get supported sample rates
        property_address.mSelector = kAudioDevicePropertyAvailableNominalSampleRates;
        property_address.mScope = kAudioObjectPropertyScopeGlobal;
        data_size = 0;
        status = AudioObjectGetPropertyDataSize(dev_id, &property_address, 0, nullptr, &data_size);
        if (status == noErr && data_size > 0) {
            UInt32 range_count = data_size / sizeof(AudioValueRange);
            std::vector<AudioValueRange> ranges(range_count);
            status = AudioObjectGetPropertyData(dev_id, &property_address,
                                                0, nullptr, &data_size, ranges.data());
            if (status == noErr) {
                for (const auto& range : ranges) {
                    // Add common rates within the range
                    for (uint32_t rate : {44100u, 48000u, 88200u, 96000u, 176400u, 192000u}) {
                        if (rate >= range.mMinimum && rate <= range.mMaximum) {
                            info.supported_sample_rates.push_back(rate);
                        }
                    }
                }
            }
        }

        if (info.supported_sample_rates.empty()) {
            info.supported_sample_rates = {44100, 48000};
        }

        devices.push_back(std::move(info));
    }
#else
    // iOS: return default devices
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
#endif

    return devices;
}

std::unique_ptr<AudioDevice> AudioDevice::create() {
    return std::make_unique<CoreAudioDevice>();
}

} // namespace soluna::pal

#endif // TARGET_OS_IPHONE || TARGET_OS_MAC
#endif // __APPLE__
