#include <soluna/pal/audio.h>

#ifdef SOLUNA_HAS_COREAUDIO
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include <atomic>
#include <cstdio>
#include <vector>

namespace soluna::pal {

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
            running_.store(false);
            return false;
        }
        return true;
    }

    void stop() override {
        if (audio_unit_ && running_.load()) {
            AudioOutputUnitStop(audio_unit_);
        }
        running_.store(false);
    }

    bool is_running() const override {
        return running_.load();
    }

    const AudioStreamConfig& config() const override {
        return config_;
    }

private:
    bool open_device(const std::string& device_id, const AudioStreamConfig& config, bool is_input) {
        config_ = config;
        is_input_ = is_input;

        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Output;
        desc.componentSubType = kAudioUnitSubType_HALOutput;
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;

        AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
        if (!comp) return false;

        OSStatus status = AudioComponentInstanceNew(comp, &audio_unit_);
        if (status != noErr) return false;

        // Enable input/output
        UInt32 enable_io = 1;
        UInt32 disable_io = 0;
        if (is_input) {
            AudioUnitSetProperty(audio_unit_, kAudioOutputUnitProperty_EnableIO,
                kAudioUnitScope_Input, 1, &enable_io, sizeof(enable_io));
            AudioUnitSetProperty(audio_unit_, kAudioOutputUnitProperty_EnableIO,
                kAudioUnitScope_Output, 0, &disable_io, sizeof(disable_io));
        }

        // Set device if specified
        if (!device_id.empty()) {
            AudioDeviceID dev_id = static_cast<AudioDeviceID>(std::stoul(device_id));
            AudioUnitSetProperty(audio_unit_, kAudioOutputUnitProperty_CurrentDevice,
                kAudioUnitScope_Global, 0, &dev_id, sizeof(dev_id));
        }

        // Set format
        AudioStreamBasicDescription fmt{};
        fmt.mSampleRate = config.sample_rate;
        fmt.mFormatID = kAudioFormatLinearPCM;
        fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        fmt.mBitsPerChannel = 32;
        fmt.mChannelsPerFrame = config.channels;
        fmt.mFramesPerPacket = 1;
        fmt.mBytesPerFrame = 4 * config.channels;
        fmt.mBytesPerPacket = fmt.mBytesPerFrame;

        UInt32 scope = is_input ? kAudioUnitScope_Output : kAudioUnitScope_Input;
        UInt32 element = is_input ? 1 : 0;
        AudioUnitSetProperty(audio_unit_, kAudioUnitProperty_StreamFormat,
            scope, element, &fmt, sizeof(fmt));

        // Set callback
        AURenderCallbackStruct cb{};
        if (is_input) {
            cb.inputProc = input_callback;
            cb.inputProcRefCon = this;
            AudioUnitSetProperty(audio_unit_, kAudioOutputUnitProperty_SetInputCallback,
                kAudioUnitScope_Global, 0, &cb, sizeof(cb));
        } else {
            cb.inputProc = output_callback;
            cb.inputProcRefCon = this;
            AudioUnitSetProperty(audio_unit_, kAudioUnitProperty_SetRenderCallback,
                kAudioUnitScope_Input, 0, &cb, sizeof(cb));
        }

        // Set buffer size
        UInt32 buffer_frames = config.frames_per_buffer;
        AudioUnitSetProperty(audio_unit_, kAudioDevicePropertyBufferFrameSize,
            kAudioUnitScope_Global, 0, &buffer_frames, sizeof(buffer_frames));

        status = AudioUnitInitialize(audio_unit_);
        if (status != noErr) {
            AudioComponentInstanceDispose(audio_unit_);
            audio_unit_ = nullptr;
            return false;
        }

        return true;
    }

    static OSStatus output_callback(void* ref_con, AudioUnitRenderActionFlags*,
        const AudioTimeStamp*, UInt32, UInt32 frames,
        AudioBufferList* buf_list)
    {
        auto* self = static_cast<CoreAudioDevice*>(ref_con);
        if (self->callback_ && self->running_.load()) {
            auto* buffer = static_cast<float*>(buf_list->mBuffers[0].mData);
            self->callback_(buffer, frames);
        }
        return noErr;
    }

    static OSStatus input_callback(void* ref_con, AudioUnitRenderActionFlags* flags,
        const AudioTimeStamp* timestamp, UInt32 bus, UInt32 frames,
        AudioBufferList*)
    {
        auto* self = static_cast<CoreAudioDevice*>(ref_con);
        if (!self->running_.load()) return noErr;

        std::vector<float> buf(frames * self->config_.channels);
        AudioBufferList buf_list{};
        buf_list.mNumberBuffers = 1;
        buf_list.mBuffers[0].mNumberChannels = self->config_.channels;
        buf_list.mBuffers[0].mDataByteSize = static_cast<UInt32>(buf.size() * sizeof(float));
        buf_list.mBuffers[0].mData = buf.data();

        OSStatus status = AudioUnitRender(self->audio_unit_, flags, timestamp, bus, frames, &buf_list);
        if (status == noErr && self->callback_) {
            self->callback_(buf.data(), frames);
        }
        return noErr;
    }

    AudioUnit audio_unit_ = nullptr;
    AudioStreamConfig config_;
    AudioCallback callback_;
    std::atomic<bool> running_{false};
    bool is_input_ = false;
};

std::vector<AudioDeviceInfo> AudioDevice::enumerate() {
    std::vector<AudioDeviceInfo> devices;

    AudioObjectPropertyAddress prop{};
    prop.mSelector = kAudioHardwarePropertyDevices;
    prop.mScope = kAudioObjectPropertyScopeGlobal;
    prop.mElement = kAudioObjectPropertyElementMain;

    UInt32 size = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop, 0, nullptr, &size);
    size_t count = size / sizeof(AudioDeviceID);

    std::vector<AudioDeviceID> dev_ids(count);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, nullptr, &size, dev_ids.data());

    for (auto dev_id : dev_ids) {
        AudioDeviceInfo info;
        info.id = std::to_string(dev_id);

        // Get name
        CFStringRef name_ref = nullptr;
        prop.mSelector = kAudioObjectPropertyName;
        size = sizeof(name_ref);
        if (AudioObjectGetPropertyData(dev_id, &prop, 0, nullptr, &size, &name_ref) == noErr && name_ref) {
            char buf[256];
            CFStringGetCString(name_ref, buf, sizeof(buf), kCFStringEncodingUTF8);
            info.name = buf;
            CFRelease(name_ref);
        }

        // Get channel counts
        prop.mSelector = kAudioDevicePropertyStreamConfiguration;
        prop.mScope = kAudioDevicePropertyScopeInput;
        size = 0;
        AudioObjectGetPropertyDataSize(dev_id, &prop, 0, nullptr, &size);
        if (size > 0) {
            std::vector<uint8_t> buf(size);
            auto* list = reinterpret_cast<AudioBufferList*>(buf.data());
            AudioObjectGetPropertyData(dev_id, &prop, 0, nullptr, &size, list);
            for (UInt32 i = 0; i < list->mNumberBuffers; i++) {
                info.max_input_channels += list->mBuffers[i].mNumberChannels;
            }
        }

        prop.mScope = kAudioDevicePropertyScopeOutput;
        size = 0;
        AudioObjectGetPropertyDataSize(dev_id, &prop, 0, nullptr, &size);
        if (size > 0) {
            std::vector<uint8_t> buf(size);
            auto* list = reinterpret_cast<AudioBufferList*>(buf.data());
            AudioObjectGetPropertyData(dev_id, &prop, 0, nullptr, &size, list);
            for (UInt32 i = 0; i < list->mNumberBuffers; i++) {
                info.max_output_channels += list->mBuffers[i].mNumberChannels;
            }
        }

        info.supported_sample_rates = {44100, 48000, 96000};
        devices.push_back(std::move(info));
    }

    return devices;
}

std::unique_ptr<AudioDevice> AudioDevice::create() {
    return std::make_unique<CoreAudioDevice>();
}

} // namespace soluna::pal

#endif // SOLUNA_HAS_COREAUDIO
