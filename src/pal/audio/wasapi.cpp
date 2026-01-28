/**
 * WASAPI Audio Device Implementation (Windows)
 * SPDX-License-Identifier: MIT
 */

#include <soluna/pal/audio.h>

#ifdef SOLUNA_HAS_WASAPI

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <comdef.h>

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")

namespace soluna::pal {

class WasapiDevice : public AudioDevice {
public:
    WasapiDevice() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }

    ~WasapiDevice() override {
        close();
        CoUninitialize();
    }

    bool open_input(const std::string& device_id, const AudioStreamConfig& config) override {
        return open_device(device_id, config, true);
    }

    bool open_output(const std::string& device_id, const AudioStreamConfig& config) override {
        return open_device(device_id, config, false);
    }

    void close() override {
        stop();
        if (audio_client_) {
            audio_client_->Release();
            audio_client_ = nullptr;
        }
        if (device_) {
            device_->Release();
            device_ = nullptr;
        }
        render_client_ = nullptr;
        capture_client_ = nullptr;
    }

    bool start(AudioCallback callback) override {
        if (!audio_client_ || running_.load()) return false;
        callback_ = std::move(callback);
        running_.store(true);

        HRESULT hr = audio_client_->Start();
        if (FAILED(hr)) {
            running_.store(false);
            return false;
        }

        // Launch audio thread
        audio_thread_ = std::thread([this]() { audio_loop(); });
        return true;
    }

    void stop() override {
        running_.store(false);
        if (audio_thread_.joinable()) {
            audio_thread_.join();
        }
        if (audio_client_) {
            audio_client_->Stop();
        }
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

        IMMDeviceEnumerator* enumerator = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr)) return false;

        EDataFlow flow = is_input ? eCapture : eRender;

        if (device_id.empty()) {
            hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device_);
        } else {
            // Convert device_id to wide string
            int wlen = MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(), -1, nullptr, 0);
            std::vector<wchar_t> wid(wlen);
            MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(), -1, wid.data(), wlen);
            hr = enumerator->GetDevice(wid.data(), &device_);
        }
        enumerator->Release();
        if (FAILED(hr)) return false;

        hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
            nullptr, reinterpret_cast<void**>(&audio_client_));
        if (FAILED(hr)) return false;

        // Set format: 32-bit float
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        fmt.nChannels = static_cast<WORD>(config.channels);
        fmt.nSamplesPerSec = config.sample_rate;
        fmt.wBitsPerSample = 32;
        fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

        // Buffer duration in 100ns units
        REFERENCE_TIME buffer_duration = static_cast<REFERENCE_TIME>(
            10000000.0 * config.frames_per_buffer / config.sample_rate);
        if (buffer_duration < 10000) buffer_duration = 10000; // minimum 1ms

        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
            buffer_duration, 0, &fmt, nullptr);
        if (FAILED(hr)) {
            // Retry without event callback
            flags = 0;
            hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                buffer_duration, 0, &fmt, nullptr);
            if (FAILED(hr)) return false;
        }

        // Get buffer size
        hr = audio_client_->GetBufferSize(&buffer_frames_);
        if (FAILED(hr)) return false;

        if (is_input) {
            hr = audio_client_->GetService(__uuidof(IAudioCaptureClient),
                reinterpret_cast<void**>(&capture_client_));
        } else {
            hr = audio_client_->GetService(__uuidof(IAudioRenderClient),
                reinterpret_cast<void**>(&render_client_));
        }

        return SUCCEEDED(hr);
    }

    void audio_loop() {
        while (running_.load()) {
            if (is_input_) {
                process_capture();
            } else {
                process_render();
            }
            // Sleep for half a buffer period
            DWORD sleep_ms = static_cast<DWORD>(
                500.0 * config_.frames_per_buffer / config_.sample_rate);
            if (sleep_ms < 1) sleep_ms = 1;
            Sleep(sleep_ms);
        }
    }

    void process_render() {
        if (!render_client_) return;

        UINT32 padding = 0;
        audio_client_->GetCurrentPadding(&padding);
        UINT32 available = buffer_frames_ - padding;
        if (available == 0) return;

        UINT32 frames = (available < config_.frames_per_buffer)
            ? available : config_.frames_per_buffer;

        BYTE* data = nullptr;
        HRESULT hr = render_client_->GetBuffer(frames, &data);
        if (FAILED(hr)) return;

        if (callback_) {
            callback_(reinterpret_cast<float*>(data), frames);
        }

        render_client_->ReleaseBuffer(frames, 0);
    }

    void process_capture() {
        if (!capture_client_) return;

        UINT32 packet_length = 0;
        capture_client_->GetNextPacketSize(&packet_length);

        while (packet_length > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;

            HRESULT hr = capture_client_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            if (callback_ && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                callback_(reinterpret_cast<float*>(data), frames);
            }

            capture_client_->ReleaseBuffer(frames);
            capture_client_->GetNextPacketSize(&packet_length);
        }
    }

    IMMDevice* device_ = nullptr;
    IAudioClient* audio_client_ = nullptr;
    IAudioRenderClient* render_client_ = nullptr;
    IAudioCaptureClient* capture_client_ = nullptr;
    AudioStreamConfig config_;
    AudioCallback callback_;
    std::atomic<bool> running_{false};
    bool is_input_ = false;
    UINT32 buffer_frames_ = 0;
    std::thread audio_thread_;
};

std::vector<AudioDeviceInfo> AudioDevice::enumerate() {
    std::vector<AudioDeviceInfo> devices;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* enumerator = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (!enumerator) return devices;

    // Enumerate both directions
    for (EDataFlow flow : {eRender, eCapture}) {
        IMMDeviceCollection* collection = nullptr;
        enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
        if (!collection) continue;

        UINT count = 0;
        collection->GetCount(&count);

        for (UINT i = 0; i < count; i++) {
            IMMDevice* device = nullptr;
            collection->Item(i, &device);
            if (!device) continue;

            AudioDeviceInfo info{};

            // Get ID
            LPWSTR wid = nullptr;
            device->GetId(&wid);
            if (wid) {
                int len = WideCharToMultiByte(CP_UTF8, 0, wid, -1, nullptr, 0, nullptr, nullptr);
                std::vector<char> buf(len);
                WideCharToMultiByte(CP_UTF8, 0, wid, -1, buf.data(), len, nullptr, nullptr);
                info.id = buf.data();
                CoTaskMemFree(wid);
            }

            // Get name from properties
            IPropertyStore* props = nullptr;
            device->OpenPropertyStore(STGM_READ, &props);
            if (props) {
                PROPVARIANT var;
                PropVariantInit(&var);
                props->GetValue(PKEY_Device_FriendlyName, &var);
                if (var.vt == VT_LPWSTR && var.pwszVal) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                    std::vector<char> buf(len);
                    WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, buf.data(), len, nullptr, nullptr);
                    info.name = buf.data();
                }
                PropVariantClear(&var);
                props->Release();
            }

            if (flow == eCapture) {
                info.max_input_channels = 2; // default assumption
            } else {
                info.max_output_channels = 2;
            }

            info.supported_sample_rates = {44100, 48000, 96000};
            devices.push_back(std::move(info));
            device->Release();
        }
        collection->Release();
    }

    enumerator->Release();
    return devices;
}

std::unique_ptr<AudioDevice> AudioDevice::create() {
    return std::make_unique<WasapiDevice>();
}

} // namespace soluna::pal

#endif // SOLUNA_HAS_WASAPI
