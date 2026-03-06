#pragma once

/**
 * Minimal VST3 wrapper for Soluna network audio receiver
 *
 * Implements the bare minimum VST3 COM interfaces required for a DAW
 * to discover, instantiate, and use this plugin as an audio generator:
 *
 *   IPluginFactory  — Exported via GetPluginFactory()
 *   IComponent      — Audio processor (IAudioProcessor)
 *   IEditController — Parameter editing
 *
 * This wrapper avoids depending on the full VST3 SDK by defining the
 * required interface structures inline. The GUIDs and vtable layout
 * match the official Steinberg VST3 specification.
 *
 * SPDX-License-Identifier: MIT
 */

#include <cstdint>
#include <cstring>
#include <atomic>

// ── VST3 Type Definitions ───────────────────────────────────────────────────

namespace Steinberg {

using int8    = int8_t;
using int16   = int16_t;
using int32   = int32_t;
using int64   = int64_t;
using uint8   = uint8_t;
using uint16  = uint16_t;
using uint32  = uint32_t;
using uint64  = uint64_t;
using TBool   = int32;
using tresult = int32;
using char16  = char16_t;
using TChar   = char16;
using FIDString = const int8*;
using char8   = char;

// Result codes
constexpr tresult kResultOk    = 0;
constexpr tresult kResultFalse = 1;
constexpr tresult kResultTrue  = 0;
constexpr tresult kNoInterface = -1;
constexpr tresult kNotImplemented = -1;
constexpr tresult kInvalidArgument = -2;

// FUID (128-bit unique identifier, same layout as COM GUID)
struct FUID {
    uint8 data[16];

    bool operator==(const FUID& other) const {
        return std::memcmp(data, other.data, 16) == 0;
    }
};

// ── Base interfaces ─────────────────────────────────────────────────────────

class FUnknown {
public:
    virtual tresult queryInterface(const FUID& iid, void** obj) = 0;
    virtual uint32  addRef() = 0;
    virtual uint32  release() = 0;
    virtual ~FUnknown() = default;
};

// IIDs
// FUnknown: 00000000-0000-0000-C000-000000000046
constexpr FUID IID_FUnknown = {{
    0x00,0x00,0x00,0x00, 0x00,0x00, 0x00,0x00,
    0xC0,0x00, 0x00,0x00,0x00,0x00,0x00,0x46
}};

// ── Plugin interfaces ───────────────────────────────────────────────────────

namespace Vst {

using ParamID    = uint32;
using ParamValue = double;
using SampleRate = double;
using Sample32   = float;
using Sample64   = double;
using String128  = TChar[128];

constexpr int32 kMaxNameSize = 128;

// MediaType
enum MediaTypes : int32 {
    kAudio = 0,
    kEvent = 1,
};

// BusDirection
enum BusDirections : int32 {
    kInput  = 0,
    kOutput = 1,
};

// BusType
enum BusTypes : int32 {
    kMain = 0,
    kAux  = 1,
};

// Speaker arrangements (simplified)
enum SpeakerArr : int64 {
    kSpeakerL  = 1 << 0,
    kSpeakerR  = 1 << 1,
    kStereo    = kSpeakerL | kSpeakerR,
    kMono      = kSpeakerL,
};

// Symbolic sample size
enum SymbolicSampleSizes : int32 {
    kSample32 = 0,
    kSample64 = 1,
};

// Bus info
struct BusInfo {
    int32     mediaType;
    int32     direction;
    int32     channelCount;
    String128 name;
    int32     busType;
    uint32    flags;
    enum { kDefaultActive = 1 };
};

// ProcessSetup
struct ProcessSetup {
    int32      processMode;
    int32      symbolicSampleSize;
    int32      maxSamplesPerBlock;
    SampleRate sampleRate;
};

// AudioBusBuffers
struct AudioBusBuffers {
    int32     numChannels;
    uint64    silenceFlags;
    union {
        Sample32** channelBuffers32;
        Sample64** channelBuffers64;
    };
};

// ProcessData
struct ProcessData {
    int32            processMode;
    int32            symbolicSampleSize;
    int32            numSamples;
    int32            numInputs;
    int32            numOutputs;
    AudioBusBuffers* inputs;
    AudioBusBuffers* outputs;
    // We ignore parameter changes and events for simplicity
    void*            inputParameterChanges;
    void*            outputParameterChanges;
    void*            inputEvents;
    void*            outputEvents;
    void*            processContext;
};

// Parameter info
struct ParameterInfo {
    ParamID   id;
    String128 title;
    String128 shortTitle;
    String128 units;
    int32     stepCount;
    ParamValue defaultNormalizedValue;
    int32     unitId;
    int32     flags;
    enum {
        kCanAutomate  = 1 << 0,
        kIsReadOnly   = 1 << 1,
        kIsBypass     = 1 << 4,
    };
};

// Routing info
struct RoutingInfo {
    int32 mediaType;
    int32 busIndex;
    int32 channel;
};

// ── Interface IIDs ──────────────────────────────────────────────────────────
// These are Steinberg's official IIDs (COM_COMPATIBLE byte order)

// IComponent: E831FF31-F2D5-4301-928E-BBEE25697802
constexpr FUID IID_IComponent = {{
    0xE8,0x31,0xFF,0x31, 0xF2,0xD5, 0x43,0x01,
    0x92,0x8E, 0xBB,0xEE,0x25,0x69,0x78,0x02
}};

// IAudioProcessor: 42043F99-B7DA-453C-A569-E79D9AAEC33F
constexpr FUID IID_IAudioProcessor = {{
    0x42,0x04,0x3F,0x99, 0xB7,0xDA, 0x45,0x3C,
    0xA5,0x69, 0xE7,0x9D,0x9A,0xAE,0xC3,0x3F
}};

// IEditController: DCD7BBE3-7742-448D-A874-AACC979C759E
constexpr FUID IID_IEditController = {{
    0xDC,0xD7,0xBB,0xE3, 0x77,0x42, 0x44,0x8D,
    0xA8,0x74, 0xAA,0xCC,0x97,0x9C,0x75,0x9E
}};

// IPluginBase: 22888DDB-156E-45AE-8358-B34808190625
constexpr FUID IID_IPluginBase = {{
    0x22,0x88,0x8D,0xDB, 0x15,0x6E, 0x45,0xAE,
    0x83,0x58, 0xB3,0x48,0x08,0x19,0x06,0x25
}};

} // namespace Vst

// ── IPluginFactory ──────────────────────────────────────────────────────────

struct PClassInfo {
    FUID cid;
    int32 cardinality;
    char8 category[32];
    char8 name[64];
};

// IPluginFactory: 7A4D811C-5211-4A1F-AED9-D2EE0B43BF9F
constexpr FUID IID_IPluginFactory = {{
    0x7A,0x4D,0x81,0x1C, 0x52,0x11, 0x4A,0x1F,
    0xAE,0xD9, 0xD2,0xEE,0x0B,0x43,0xBF,0x9F
}};

struct PFactoryInfo {
    char8 vendor[64];
    char8 url[256];
    char8 email[128];
    int32 flags;
    enum { kUnicode = 1 << 4 };
};

} // namespace Steinberg

// ── Plugin CID ──────────────────────────────────────────────────────────────

// Soluna VST Receiver — unique class ID
// A1B2C3D4-E5F6-7890-ABCD-EF0123456789
constexpr Steinberg::FUID kSolunaProcessorCID = {{
    0xA1,0xB2,0xC3,0xD4, 0xE5,0xF6, 0x78,0x90,
    0xAB,0xCD, 0xEF,0x01,0x23,0x45,0x67,0x89
}};

// Soluna VST Controller CID
// A1B2C3D4-E5F6-7890-ABCD-EF0123456790
constexpr Steinberg::FUID kSolunaControllerCID = {{
    0xA1,0xB2,0xC3,0xD4, 0xE5,0xF6, 0x78,0x90,
    0xAB,0xCD, 0xEF,0x01,0x23,0x45,0x67,0x90
}};
