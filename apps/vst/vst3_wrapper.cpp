/**
 * VST3 entry point and wrapper implementation for Soluna network audio receiver
 *
 * Implements:
 *   SolunaProcessor  — IComponent + IAudioProcessor
 *   SolunaController — IEditController (parameter UI)
 *   SolunaFactory    — IPluginFactory
 *
 * Exported symbol: GetPluginFactory()
 *
 * SPDX-License-Identifier: MIT
 */

#include "vst3_wrapper.h"
#include "soluna_plugin.h"

#include <cstdio>
#include <cstring>
#include <cmath>

using namespace Steinberg;
using namespace Steinberg::Vst;

// ── Utility: Copy ASCII to String128 (UTF-16) ──────────────────────────────

static void str_to_128(String128 dst, const char* src) {
    size_t len = std::strlen(src);
    if (len > 127) len = 127;
    for (size_t i = 0; i < len; ++i) {
        dst[i] = static_cast<TChar>(src[i]);
    }
    dst[len] = 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// SolunaProcessor — Audio processing component
// ═══════════════════════════════════════════════════════════════════════════

class SolunaProcessor : public FUnknown {
public:
    SolunaProcessor() {
        receiver_ = std::make_unique<soluna::vst::SolunaVstReceiver>();
    }

    ~SolunaProcessor() override {
        receiver_->stop();
    }

    // ── FUnknown ────────────────────────────────────────────────────────

    tresult queryInterface(const FUID& iid, void** obj) override {
        if (!obj) return kInvalidArgument;

        if (iid == IID_FUnknown || iid == IID_IComponent ||
            iid == IID_IAudioProcessor || iid == IID_IPluginBase) {
            *obj = static_cast<void*>(this);
            addRef();
            return kResultOk;
        }

        *obj = nullptr;
        return kNoInterface;
    }

    uint32 addRef() override {
        return ++ref_count_;
    }

    uint32 release() override {
        uint32 r = --ref_count_;
        if (r == 0) delete this;
        return r;
    }

    // ── IPluginBase ─────────────────────────────────────────────────────

    tresult initialize(FUnknown* /*context*/) {
        if (initialized_) return kResultFalse;
        initialized_ = true;
        return kResultOk;
    }

    tresult terminate() {
        receiver_->stop();
        initialized_ = false;
        return kResultOk;
    }

    // ── IComponent ──────────────────────────────────────────────────────

    tresult getControllerClassId(FUID& classId) {
        classId = kSolunaControllerCID;
        return kResultOk;
    }

    tresult setIoMode(int32 /*mode*/) {
        return kResultOk;
    }

    int32 getBusCount(int32 type, int32 dir) {
        // One stereo audio output bus, no inputs, no event buses
        if (type == kAudio && dir == kOutput) return 1;
        return 0;
    }

    tresult getBusInfo(int32 type, int32 dir, int32 index, BusInfo& bus) {
        if (type == kAudio && dir == kOutput && index == 0) {
            bus.mediaType = kAudio;
            bus.direction = kOutput;
            bus.channelCount = 2;
            str_to_128(bus.name, "Soluna Out");
            bus.busType = kMain;
            bus.flags = BusInfo::kDefaultActive;
            return kResultOk;
        }
        return kInvalidArgument;
    }

    tresult getRoutingInfo(RoutingInfo& /*inInfo*/, RoutingInfo& /*outInfo*/) {
        return kNotImplemented;
    }

    tresult activateBus(int32 type, int32 dir, int32 index, TBool state) {
        if (type == kAudio && dir == kOutput && index == 0) {
            bus_active_ = (state != 0);
            return kResultOk;
        }
        return kInvalidArgument;
    }

    tresult setActive(TBool state) {
        if (state) {
            // Start receiving network audio
            soluna::vst::SolunaVstReceiver::Config cfg;
            cfg.sample_rate = static_cast<uint32_t>(sample_rate_);
            cfg.channels = 2;
            // Use defaults (multicast 239.69.0.1:5004)
            if (!receiver_->start(cfg)) {
                fprintf(stderr, "[SolunaVST3] Failed to start receiver\n");
                return kResultFalse;
            }
        } else {
            receiver_->stop();
        }
        return kResultOk;
    }

    tresult setState(void* /*state*/) {
        // TODO: Restore saved state (multicast group, port, etc.)
        return kResultOk;
    }

    tresult getState(void* /*state*/) {
        // TODO: Save state
        return kResultOk;
    }

    // ── IAudioProcessor ─────────────────────────────────────────────────

    tresult setBusArrangements(int64* /*inputs*/, int32 /*numIns*/,
                               int64* outputs, int32 numOuts) {
        if (numOuts >= 1 && (outputs[0] == SpeakerArr::kStereo ||
                             outputs[0] == SpeakerArr::kMono)) {
            return kResultOk;
        }
        return kResultFalse;
    }

    tresult getBusArrangement(int32 dir, int32 index, int64& arr) {
        if (dir == kOutput && index == 0) {
            arr = SpeakerArr::kStereo;
            return kResultOk;
        }
        return kInvalidArgument;
    }

    tresult canProcessSampleSize(int32 symbolicSampleSize) {
        // Support 32-bit float only
        return (symbolicSampleSize == kSample32) ? kResultOk : kResultFalse;
    }

    uint32 getLatencySamples() {
        // Report latency based on buffer setting
        float ms = receiver_->buffer_ms();
        return static_cast<uint32>(ms * 0.001f * static_cast<float>(sample_rate_));
    }

    tresult setupProcessing(ProcessSetup& setup) {
        sample_rate_ = setup.sampleRate;
        max_block_size_ = setup.maxSamplesPerBlock;
        return kResultOk;
    }

    tresult setProcessing(TBool /*state*/) {
        return kResultOk;
    }

    tresult process(ProcessData& data) {
        if (data.numOutputs < 1) return kResultOk;
        if (data.numSamples <= 0) return kResultOk;

        AudioBusBuffers& out = data.outputs[0];
        if (out.numChannels < 1) return kResultOk;

        float* left  = out.channelBuffers32[0];
        float* right = (out.numChannels >= 2) ? out.channelBuffers32[1] : nullptr;

        receiver_->process(left, right, static_cast<uint32_t>(data.numSamples));

        // Clear silence flags — we always produce output
        out.silenceFlags = 0;

        return kResultOk;
    }

    uint32 getTailSamples() {
        return 0;
    }

    // ── Access for parameter sync ───────────────────────────────────────

    soluna::vst::SolunaVstReceiver* receiver() { return receiver_.get(); }

private:
    std::atomic<uint32> ref_count_{1};
    bool initialized_ = false;
    bool bus_active_ = true;
    double sample_rate_ = 48000.0;
    int32 max_block_size_ = 512;

    std::unique_ptr<soluna::vst::SolunaVstReceiver> receiver_;
};

// ═══════════════════════════════════════════════════════════════════════════
// SolunaController — Parameter editing
// ═══════════════════════════════════════════════════════════════════════════

class SolunaController : public FUnknown {
public:
    SolunaController() = default;
    ~SolunaController() override = default;

    // ── FUnknown ────────────────────────────────────────────────────────

    tresult queryInterface(const FUID& iid, void** obj) override {
        if (!obj) return kInvalidArgument;

        if (iid == IID_FUnknown || iid == IID_IEditController ||
            iid == IID_IPluginBase) {
            *obj = static_cast<void*>(this);
            addRef();
            return kResultOk;
        }

        *obj = nullptr;
        return kNoInterface;
    }

    uint32 addRef() override {
        return ++ref_count_;
    }

    uint32 release() override {
        uint32 r = --ref_count_;
        if (r == 0) delete this;
        return r;
    }

    // ── IPluginBase ─────────────────────────────────────────────────────

    tresult initialize(FUnknown* /*context*/) {
        return kResultOk;
    }

    tresult terminate() {
        return kResultOk;
    }

    // ── IEditController ─────────────────────────────────────────────────

    tresult setComponentState(void* /*state*/) {
        return kResultOk;
    }

    tresult setState(void* /*state*/) {
        return kResultOk;
    }

    tresult getState(void* /*state*/) {
        return kResultOk;
    }

    int32 getParameterCount() {
        return soluna::vst::kParamCount;
    }

    tresult getParameterInfo(int32 paramIndex, ParameterInfo& info) {
        std::memset(&info, 0, sizeof(info));

        switch (paramIndex) {
            case 0:
                info.id = soluna::vst::kParamVolume;
                str_to_128(info.title, "Volume");
                str_to_128(info.shortTitle, "Vol");
                str_to_128(info.units, "");
                info.stepCount = 0;
                info.defaultNormalizedValue = 1.0;
                info.unitId = 0;
                info.flags = ParameterInfo::kCanAutomate;
                return kResultOk;

            case 1:
                info.id = soluna::vst::kParamMute;
                str_to_128(info.title, "Mute");
                str_to_128(info.shortTitle, "Mute");
                str_to_128(info.units, "");
                info.stepCount = 1;
                info.defaultNormalizedValue = 0.0;
                info.unitId = 0;
                info.flags = ParameterInfo::kCanAutomate;
                return kResultOk;

            case 2:
                info.id = soluna::vst::kParamBufferMs;
                str_to_128(info.title, "Buffer Size");
                str_to_128(info.shortTitle, "Buf");
                str_to_128(info.units, "ms");
                info.stepCount = 0;
                info.defaultNormalizedValue = 0.0816;  // (50-10)/(500-10) normalized
                info.unitId = 0;
                info.flags = ParameterInfo::kCanAutomate;
                return kResultOk;

            default:
                return kInvalidArgument;
        }
    }

    ParamValue normalizedParamToPlain(ParamID id, ParamValue normalized) {
        switch (id) {
            case soluna::vst::kParamVolume:   return normalized;           // 0..1
            case soluna::vst::kParamMute:     return normalized >= 0.5 ? 1.0 : 0.0;
            case soluna::vst::kParamBufferMs: return 10.0 + normalized * 490.0;  // 10..500
            default: return normalized;
        }
    }

    ParamValue plainParamToNormalized(ParamID id, ParamValue plain) {
        switch (id) {
            case soluna::vst::kParamVolume:   return plain;
            case soluna::vst::kParamMute:     return plain >= 0.5 ? 1.0 : 0.0;
            case soluna::vst::kParamBufferMs: return (plain - 10.0) / 490.0;
            default: return plain;
        }
    }

    ParamValue getParamNormalized(ParamID id) {
        switch (id) {
            case soluna::vst::kParamVolume:   return params_[0];
            case soluna::vst::kParamMute:     return params_[1];
            case soluna::vst::kParamBufferMs: return params_[2];
            default: return 0.0;
        }
    }

    tresult setParamNormalized(ParamID id, ParamValue value) {
        switch (id) {
            case soluna::vst::kParamVolume:   params_[0] = value; return kResultOk;
            case soluna::vst::kParamMute:     params_[1] = value; return kResultOk;
            case soluna::vst::kParamBufferMs: params_[2] = value; return kResultOk;
            default: return kInvalidArgument;
        }
    }

    tresult setComponentHandler(void* /*handler*/) {
        return kResultOk;
    }

    void* createView(FIDString /*name*/) {
        // No custom UI — DAW will use generic parameter knobs
        return nullptr;
    }

private:
    std::atomic<uint32> ref_count_{1};
    double params_[3] = {1.0, 0.0, 0.0816};  // volume, mute, buffer
};

// ═══════════════════════════════════════════════════════════════════════════
// SolunaFactory — IPluginFactory
// ═══════════════════════════════════════════════════════════════════════════

class SolunaFactory : public FUnknown {
public:
    SolunaFactory() = default;
    ~SolunaFactory() override = default;

    // ── FUnknown ────────────────────────────────────────────────────────

    tresult queryInterface(const FUID& iid, void** obj) override {
        if (!obj) return kInvalidArgument;

        if (iid == IID_FUnknown || iid == IID_IPluginFactory) {
            *obj = static_cast<void*>(this);
            addRef();
            return kResultOk;
        }

        *obj = nullptr;
        return kNoInterface;
    }

    uint32 addRef() override {
        return ++ref_count_;
    }

    uint32 release() override {
        uint32 r = --ref_count_;
        // Factory is a singleton, never delete
        return r;
    }

    // ── IPluginFactory ──────────────────────────────────────────────────

    tresult getFactoryInfo(PFactoryInfo& info) {
        std::memset(&info, 0, sizeof(info));
        std::strncpy(info.vendor, "Soluna", sizeof(info.vendor) - 1);
        std::strncpy(info.url, "https://github.com/soluna-audio", sizeof(info.url) - 1);
        std::strncpy(info.email, "info@soluna.audio", sizeof(info.email) - 1);
        info.flags = PFactoryInfo::kUnicode;
        return kResultOk;
    }

    int32 countClasses() {
        return 2;  // Processor + Controller
    }

    tresult getClassInfo(int32 index, PClassInfo& info) {
        std::memset(&info, 0, sizeof(info));

        switch (index) {
            case 0:
                info.cid = kSolunaProcessorCID;
                info.cardinality = 0x7FFFFFFF;  // kManyInstances
                std::strncpy(info.category, "Audio Module Class",
                             sizeof(info.category) - 1);
                std::strncpy(info.name, "Soluna Network Receiver",
                             sizeof(info.name) - 1);
                return kResultOk;

            case 1:
                info.cid = kSolunaControllerCID;
                info.cardinality = 0x7FFFFFFF;
                std::strncpy(info.category, "Component Controller Class",
                             sizeof(info.category) - 1);
                std::strncpy(info.name, "Soluna Controller",
                             sizeof(info.name) - 1);
                return kResultOk;

            default:
                return kInvalidArgument;
        }
    }

    tresult createInstance(const FUID& cid, const FUID& iid, void** obj) {
        if (!obj) return kInvalidArgument;

        if (cid == kSolunaProcessorCID) {
            auto* proc = new SolunaProcessor();
            tresult r = proc->queryInterface(iid, obj);
            proc->release();
            return r;
        }

        if (cid == kSolunaControllerCID) {
            auto* ctrl = new SolunaController();
            tresult r = ctrl->queryInterface(iid, obj);
            ctrl->release();
            return r;
        }

        *obj = nullptr;
        return kNoInterface;
    }

private:
    std::atomic<uint32> ref_count_{1};
};

// ═══════════════════════════════════════════════════════════════════════════
// Exported entry points
// ═══════════════════════════════════════════════════════════════════════════

static SolunaFactory g_factory;

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

/**
 * VST3 host calls this to get the plugin factory.
 * This is the single required export for a VST3 module.
 */
EXPORT Steinberg::FUnknown* GetPluginFactory() {
    g_factory.addRef();
    return &g_factory;
}

#ifdef _WIN32
// Windows DLL entry
EXPORT bool InitDll()  { return true; }
EXPORT bool ExitDll()  { return true; }
#else
// macOS / Linux module entry
EXPORT bool ModuleEntry(void*)  { return true; }
EXPORT bool ModuleExit()        { return true; }
#endif

} // extern "C"
