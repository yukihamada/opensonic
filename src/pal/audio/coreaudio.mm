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
#include <AVFoundation/AVFoundation.h>

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
            AudioUnitUninitialize(audio_unit_);
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
                    if (status == noErr && data_size >= sizeof(AudioBufferList)) {
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

        // Allocate conversion buffer — oversized to handle iOS delivering
        // more frames than requested (IOBufferDuration is just a preference)
        conversion_buffer_.resize(std::max(config.frames_per_buffer, 8192u) * config.channels);

#if TARGET_OS_MAC && !TARGET_OS_IPHONE
        // Check microphone permission for capture mode on macOS.
        // Wrap in @autoreleasepool for thread safety (background threads
        // may not have an autorelease pool).
        if (capture) {
            @autoreleasepool {
                if (@available(macOS 10.14, *)) {
                    AVAuthorizationStatus auth = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
                    if (auth == AVAuthorizationStatusNotDetermined) {
                        fprintf(stderr, "CoreAudio: Requesting microphone access...\n");
                        dispatch_semaphore_t sema = dispatch_semaphore_create(0);
                        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                            completionHandler:^(BOOL granted) {
                                (void)granted;
                                dispatch_semaphore_signal(sema);
                            }];
                        dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);
                        auth = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
                    }
                    if (auth != AVAuthorizationStatusAuthorized) {
                        fprintf(stderr, "CoreAudio: Microphone access DENIED (status=%d).\n"
                            "  Grant access: System Settings → Privacy & Security → Microphone → Terminal\n",
                            (int)auth);
                        return false;
                    }
                    fprintf(stderr, "CoreAudio: Microphone access granted\n");
                }
            }
        }
#endif

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
        // Use DefaultOutput for playback (simpler, avoids stale IOProc issues).
        // HALOutput is only needed for capture (explicit device selection).
        desc.componentSubType = capture ? kAudioUnitSubType_HALOutput
                                        : kAudioUnitSubType_DefaultOutput;
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
        // Note: DefaultOutput (used for playback on macOS) doesn't support
        // EnableIO — it only has an output bus. Only configure IO for HALOutput.
        UInt32 enable_io = 1;
        UInt32 disable_io = 0;

        if (capture) {
            // Enable input bus
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

#if TARGET_OS_MAC && !TARGET_OS_IPHONE
            // macOS HALOutput: Disable output bus (capture-only)
            // Do NOT disable output on iOS RemoteIO — it breaks the audio unit
            status = AudioUnitSetProperty(audio_unit_,
                                          kAudioOutputUnitProperty_EnableIO,
                                          kAudioUnitScope_Output,
                                          0, // Output bus
                                          &disable_io,
                                          sizeof(disable_io));
#endif
        }
        // For output (DefaultOutput on macOS / RemoteIO on iOS): no EnableIO needed

#if TARGET_OS_MAC && !TARGET_OS_IPHONE
        // Set device on macOS (HALOutput only — DefaultOutput handles device automatically)
        if (capture) {
            AudioDeviceID dev_id = 0;

            if (!device_id.empty() && device_id != "default") {
                // Explicit device requested — parse as ID or name
                try {
                    dev_id = static_cast<AudioDeviceID>(std::stoul(device_id));
                } catch (const std::exception&) {
                    dev_id = find_device_by_name(device_id, capture);
                }
                if (dev_id == 0) {
                    fprintf(stderr, "CoreAudio: device '%s' not found, using system default\n", device_id.c_str());
                }
            }

            if (dev_id == 0) {
                // For capture with no explicit device: use DefaultInputDevice
                AudioObjectPropertyAddress addr = {
                    kAudioHardwarePropertyDefaultInputDevice,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                UInt32 sz = sizeof(dev_id);
                AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &sz, &dev_id);
            }

            if (dev_id != 0) {
                // Log device name for debugging
                CFStringRef dev_name_ref = nullptr;
                UInt32 dev_name_sz = sizeof(dev_name_ref);
                AudioObjectPropertyAddress name_addr = {
                    kAudioDevicePropertyDeviceNameCFString,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                if (AudioObjectGetPropertyData(dev_id, &name_addr, 0, nullptr, &dev_name_sz, &dev_name_ref) == noErr && dev_name_ref) {
                    char dev_name_buf[256];
                    CFStringGetCString(dev_name_ref, dev_name_buf, sizeof(dev_name_buf), kCFStringEncodingUTF8);
                    CFRelease(dev_name_ref);
                    fprintf(stderr, "CoreAudio: Using device %u: '%s'\n", dev_id, dev_name_buf);
                } else {
                    fprintf(stderr, "CoreAudio: Using device %u\n", dev_id);
                }

                // Ensure device sample rate matches our stream.
                // IMPORTANT: Always force-set the rate (even if it appears to match)
                // because HALOutput's IOProc can fail to start after a process restart
                // if the device state is stale. Toggling the rate forces CoreAudio to
                // reinitialize its IO path.
                {
                    Float64 desired_rate = static_cast<Float64>(config.sample_rate);
                    Float64 current_rate = 0;
                    UInt32 rate_sz = sizeof(current_rate);
                    AudioObjectPropertyAddress rate_addr = {
                        kAudioDevicePropertyNominalSampleRate,
                        kAudioObjectPropertyScopeGlobal,
                        kAudioObjectPropertyElementMain
                    };
                    AudioObjectGetPropertyData(dev_id, &rate_addr, 0, nullptr, &rate_sz, &current_rate);
                    fprintf(stderr, "CoreAudio: Device sample rate: %.0f, desired: %.0f\n", current_rate, desired_rate);

                    if (current_rate == desired_rate) {
                        // Toggle to a different rate and back to force IOProc reinitialization
                        Float64 alt_rate = (desired_rate == 48000.0) ? 44100.0 : 48000.0;
                        AudioObjectSetPropertyData(dev_id, &rate_addr, 0, nullptr, sizeof(alt_rate), &alt_rate);
                        usleep(50000); // 50ms for the rate change to take effect
                    }
                    OSStatus rate_status = AudioObjectSetPropertyData(dev_id, &rate_addr, 0, nullptr, sizeof(desired_rate), &desired_rate);
                    if (rate_status == noErr) {
                        fprintf(stderr, "CoreAudio: Device sample rate set to %.0f\n", desired_rate);
                    } else {
                        fprintf(stderr, "CoreAudio: Could not set sample rate (err %d), will use %.0f\n", (int)rate_status, current_rate);
                        config_.sample_rate = static_cast<uint32_t>(current_rate);
                    }
                    usleep(50000); // 50ms settle time after rate change
                }

                status = AudioUnitSetProperty(audio_unit_,
                                              kAudioOutputUnitProperty_CurrentDevice,
                                              kAudioUnitScope_Global,
                                              0,
                                              &dev_id,
                                              sizeof(dev_id));
                if (status != noErr) {
                    fprintf(stderr, "CoreAudio: Failed to set device %u: %d\n", dev_id, (int)status);
                }
            } else {
                fprintf(stderr, "CoreAudio: WARNING — dev_id=0, no explicit device set (will use system default input)\n");
            }
        } else {
            fprintf(stderr, "CoreAudio: Using DefaultOutput (system default playback device)\n");
        }
#else
        (void)device_id;
#endif

        // Set stream format (use config_.sample_rate which may have been adjusted to match device)
        AudioStreamBasicDescription stream_format = {};
        stream_format.mSampleRate = config_.sample_rate ? config_.sample_rate : config.sample_rate;
        stream_format.mFormatID = kAudioFormatLinearPCM;
        stream_format.mFormatFlags = kAudioFormatFlagIsFloat |
                                     kAudioFormatFlagIsPacked;
        stream_format.mBytesPerPacket = sizeof(float) * config.channels;
        stream_format.mFramesPerPacket = 1;
        stream_format.mBytesPerFrame = sizeof(float) * config.channels;
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

#if TARGET_OS_IPHONE
        // Read back actual format to verify iOS honored our request
        {
            AudioStreamBasicDescription actual = {};
            UInt32 sz = sizeof(actual);
            AudioUnitGetProperty(audio_unit_, kAudioUnitProperty_StreamFormat,
                                 scope, element, &actual, &sz);
            NSLog(@"[CoreAudio] iOS stream format: rate=%.0f ch=%u bpf=%u flags=0x%x nonInterleaved=%d",
                  actual.mSampleRate, (unsigned)actual.mChannelsPerFrame,
                  (unsigned)actual.mBytesPerFrame, (unsigned)actual.mFormatFlags,
                  (actual.mFormatFlags & kAudioFormatFlagIsNonInterleaved) ? 1 : 0);
        }
#endif

        // Set buffer size on the AudioUnit
        UInt32 buffer_frames = config.frames_per_buffer;
        status = AudioUnitSetProperty(audio_unit_,
                                      kAudioUnitProperty_MaximumFramesPerSlice,
                                      kAudioUnitScope_Global,
                                      0,
                                      &buffer_frames,
                                      sizeof(buffer_frames));
        if (status != noErr) {
            fprintf(stderr, "CoreAudio: MaximumFramesPerSlice set warning: %d\n", (int)status);
        }

#if TARGET_OS_MAC && !TARGET_OS_IPHONE
        // On macOS HALOutput, set the hardware buffer size on the device.
        // DefaultOutput doesn't support CurrentDevice property.
        if (capture) {
            AudioDeviceID hw_dev = 0;
            if (!device_id.empty() && device_id != "default") {
                try {
                    hw_dev = static_cast<AudioDeviceID>(std::stoul(device_id));
                } catch (const std::exception&) {
                    hw_dev = find_device_by_name(device_id, capture);
                }
            } else {
                // For default capture, query the resolved device
                UInt32 hw_sz = sizeof(hw_dev);
                AudioUnitGetProperty(audio_unit_,
                    kAudioOutputUnitProperty_CurrentDevice,
                    kAudioUnitScope_Global, 0, &hw_dev, &hw_sz);
            }
            if (hw_dev != 0) {
                UInt32 hw_buf = config.frames_per_buffer;
                AudioObjectPropertyAddress buf_addr = {
                    kAudioDevicePropertyBufferFrameSize,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                AudioObjectSetPropertyData(hw_dev, &buf_addr,
                    0, nullptr, sizeof(hw_buf), &hw_buf);
            }
        }
#endif

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

        // Skip reconfiguration if session is already in the right category.
        // This prevents redundant setCategory calls (e.g. when Swift already
        // configured .playAndRecord before calling startMicTransmit) that
        // trigger interruption notifications and IO buffer duration changes.
        if ([session.category isEqualToString:category]) {
            fprintf(stderr, "CoreAudio iOS: session already %s, skipping reconfiguration\n",
                    [category UTF8String]);
            // Still read actual sample rate to keep config in sync
            double actual_rate = session.sampleRate;
            if (static_cast<uint32_t>(actual_rate) != config_.sample_rate) {
                config_.sample_rate = static_cast<uint32_t>(actual_rate);
            }
            return true;
        }

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

        // Check ACTUAL sample rate (may differ from preferred)
        double actual_rate = session.sampleRate;
        double actual_buf_dur = session.IOBufferDuration;
        fprintf(stderr, "CoreAudio iOS: actual sampleRate=%.0f (requested %u), IOBufferDuration=%.4f (%.1f frames)\n",
                actual_rate, config_.sample_rate, actual_buf_dur,
                actual_rate * actual_buf_dur);

        // If actual rate differs, update config to match actual hardware
        if (static_cast<uint32_t>(actual_rate) != config_.sample_rate) {
            fprintf(stderr, "CoreAudio iOS: WARNING — sample rate mismatch! Adjusting config to %.0f\n", actual_rate);
            config_.sample_rate = static_cast<uint32_t>(actual_rate);
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
        const uint32_t ch = device->config_.channels;
        const size_t total_samples = inNumberFrames * ch;

        // Log format once for diagnostics (using NSLog so it's captured by device console)
        if (device->render_log_count_++ == 0) {
#if TARGET_OS_IPHONE
            NSLog(@"[CoreAudio] render_callback: nBuffers=%u nFrames=%u channels=%u buf0size=%u buf0ch=%u",
                    (unsigned)ioData->mNumberBuffers, (unsigned)inNumberFrames, ch,
                    (unsigned)ioData->mBuffers[0].mDataByteSize,
                    (unsigned)ioData->mBuffers[0].mNumberChannels);
#else
            fprintf(stderr, "[CoreAudio] render_callback: nBuffers=%u nFrames=%u channels=%u buf0size=%u\n",
                    (unsigned)ioData->mNumberBuffers, (unsigned)inNumberFrames, ch,
                    (unsigned)ioData->mBuffers[0].mDataByteSize);
#endif
        }

        if (!device->running_.load() || !device->callback_) {
            for (UInt32 b = 0; b < ioData->mNumberBuffers; b++) {
                if (ioData->mBuffers[b].mData)
                    std::memset(ioData->mBuffers[b].mData, 0, ioData->mBuffers[b].mDataByteSize);
            }
            return noErr;
        }

        if (ioData->mNumberBuffers == 1) {
            // Interleaved: single buffer, write directly
            device->callback_(static_cast<float*>(ioData->mBuffers[0].mData), inNumberFrames);
        } else {
            // Non-interleaved: write to temp buffer then de-interleave
            if (device->conversion_buffer_.size() < total_samples)
                device->conversion_buffer_.resize(total_samples);
            device->callback_(device->conversion_buffer_.data(), inNumberFrames);
            for (UInt32 b = 0; b < ioData->mNumberBuffers && b < ch; b++) {
                auto* dst = static_cast<float*>(ioData->mBuffers[b].mData);
                if (!dst) continue;
                const uint32_t buf_ch = ioData->mBuffers[b].mNumberChannels;
                for (UInt32 i = 0; i < inNumberFrames; i++) {
                    for (uint32_t c = 0; c < buf_ch && (b * buf_ch + c) < ch; c++) {
                        dst[i * buf_ch + c] = device->conversion_buffer_[i * ch + b * buf_ch + c];
                    }
                }
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

        // Interleaved: single buffer with all channels
        // Clamp frame count to conversion buffer capacity to prevent overflow
        const size_t total_samples = inNumberFrames * device->config_.channels;
        if (total_samples > device->conversion_buffer_.size()) {
            inNumberFrames = static_cast<UInt32>(device->conversion_buffer_.size() / device->config_.channels);
        }
        const size_t clamped_total = inNumberFrames * device->config_.channels;
        AudioBufferList buffer_list;
        buffer_list.mNumberBuffers = 1;
        buffer_list.mBuffers[0].mNumberChannels = device->config_.channels;
        buffer_list.mBuffers[0].mDataByteSize = static_cast<UInt32>(clamped_total * sizeof(float));
        buffer_list.mBuffers[0].mData = device->conversion_buffer_.data();

        // Render input
        OSStatus status = AudioUnitRender(device->audio_unit_,
                                          ioActionFlags,
                                          inTimeStamp,
                                          inBusNumber,
                                          inNumberFrames,
                                          &buffer_list);
        if (status != noErr) {
            if (device->render_err_count_++ < 5) {
                fprintf(stderr, "CoreAudio: AudioUnitRender error: %d\n", (int)status);
            }
            return status;
        }

        // Data is already interleaved in conversion_buffer_
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
    std::atomic<int> render_err_count_{0};
    std::atomic<int> render_log_count_{0};
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

        // Get transport type
        {
            UInt32 transport = 0;
            UInt32 sz = sizeof(transport);
            AudioObjectPropertyAddress taddr = {
                kAudioDevicePropertyTransportType,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            if (AudioObjectGetPropertyData(dev_id, &taddr, 0, nullptr, &sz, &transport) == noErr) {
                switch (transport) {
                    case kAudioDeviceTransportTypeBuiltIn:
                        info.transport_type = TransportType::BuiltIn; break;
                    case kAudioDeviceTransportTypeUSB:
                        info.transport_type = TransportType::USB; break;
                    case kAudioDeviceTransportTypeBluetooth:
                    case kAudioDeviceTransportTypeBluetoothLE:
                        info.transport_type = TransportType::Bluetooth; break;
                    case kAudioDeviceTransportTypeAirPlay:
                        info.transport_type = TransportType::AirPlay; break;
                    case kAudioDeviceTransportTypeVirtual:
                    case kAudioDeviceTransportTypeAggregate:
                        info.transport_type = TransportType::Virtual; break;
                    default:
                        info.transport_type = TransportType::Unknown; break;
                }
            }
        }

        // Get hardware latency (output scope)
        {
            UInt32 latency = 0;
            UInt32 sz = sizeof(latency);
            AudioObjectPropertyAddress laddr = {
                kAudioDevicePropertyLatency,
                kAudioDevicePropertyScopeOutput,
                kAudioObjectPropertyElementMain
            };
            if (AudioObjectGetPropertyData(dev_id, &laddr, 0, nullptr, &sz, &latency) == noErr) {
                info.hardware_latency_frames = latency;
            }
        }

        // Get safety offset (output scope)
        {
            UInt32 offset = 0;
            UInt32 sz = sizeof(offset);
            AudioObjectPropertyAddress saddr = {
                kAudioDevicePropertySafetyOffset,
                kAudioDevicePropertyScopeOutput,
                kAudioObjectPropertyElementMain
            };
            if (AudioObjectGetPropertyData(dev_id, &saddr, 0, nullptr, &sz, &offset) == noErr) {
                info.safety_offset_frames = offset;
            }
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
