/**
 * SolunaPlugin.mm — CoreAudio Audio Server Plug-in
 *
 * Implements an AudioServerPlugIn that appears as "Soluna" virtual output device
 * in macOS System Settings. Audio written to this device is forwarded via POSIX
 * shared memory (/soluna_audio) to solunad for multicast transmission and local
 * speaker playback.
 *
 * Object hierarchy:
 *   kAudioObjectPlugInObject (ID=1)
 *     └── Soluna device         (ID=2)
 *           └── output stream   (ID=3)
 *
 * SPDX-License-Identifier: MIT
 */

#import <CoreAudio/AudioServerPlugIn.h>
#import <CoreFoundation/CoreFoundation.h>
#import <mach/mach_time.h>
#import <pthread.h>
#import <sched.h>

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soluna_shm.h"

// ── Object IDs ───────────────────────────────────────────────────────────────

enum : AudioObjectID {
    kSolunaPlugInID = 1,
    kSolunaDeviceID = 2,
    kSolunaStreamID = 3,
    kSolunaVolumeID = 4,  // master output level control
};

static const Float32 kVolMin_dB = -96.0f;
static const Float32 kVolMax_dB =   0.0f;

// ── Audio constants ──────────────────────────────────────────────────────────

static const uint32_t kSampleRate       = 48000;
static const uint32_t kChannels         = 2;
static const uint32_t kBitsPerChannel   = 32;
static const uint32_t kBytesPerFrame    = kChannels * sizeof(float);
static const uint32_t kBytesPerPacket   = kBytesPerFrame;
static const uint32_t kFramesPerPacket  = 1;

// Zero-timestamp period: coreaudiod queries GetZeroTimeStamp periodically.
// We advance the anchor every kZeroTSPeriod frames.
static const uint64_t kZeroTSPeriod     = 8192; // ~170ms @ 48kHz

// ── Driver struct ─────────────────────────────────────────────────────────────

struct SolunaDriver {
    AudioServerPlugInDriverInterface*   mInterface;  // must be first (single pointer → vtable struct)

    CFUUIDRef                           mTypeUUID;
    CFUUIDRef                           mFactoryUUID;

    volatile int32_t                    mRefCount;

    // IO state
    _Atomic(uint32_t)                   mIOClientCount;
    _Atomic(bool)                       mIORunning;

    // Zero-timestamp state (written by IO thread, read by GetZeroTimeStamp)
    volatile uint64_t                   mAnchorHostTime;
    volatile uint64_t                   mAnchorSampleTime;
    pthread_mutex_t                     mTSMutex;

    // IO thread
    pthread_t                           mIOThread;

    // Shared memory
    SolunaShmMap                        mShm;
    _Atomic(bool)                       mShmReady;

    // Volume control (0.0–1.0 linear scalar)
    Float32                             mVolume;

    // Host ref for property-change notifications
    AudioServerPlugInHostRef            mHost;

    // Format
    AudioStreamBasicDescription         mFormat;
};

// Forward-declare factory
extern "C" void* SolunaPlugin_Create(CFAllocatorRef allocator, CFUUIDRef typeUUID);

// ── Utility: mach_timebase ───────────────────────────────────────────────────

static mach_timebase_info_data_t gTimebase;
static pthread_once_t            gTimebaseOnce = PTHREAD_ONCE_INIT;

static void init_timebase(void) {
    mach_timebase_info(&gTimebase);
}

static uint64_t frames_to_nanos(uint64_t frames) {
    pthread_once(&gTimebaseOnce, init_timebase);
    // nanos = frames * 1e9 / kSampleRate
    // convert to mach ticks: mach_ticks = nanos * denom / numer
    uint64_t nanos = (frames * 1'000'000'000ULL) / kSampleRate;
    return nanos * gTimebase.denom / gTimebase.numer;
}

// ── IO Thread ────────────────────────────────────────────────────────────────

static void* io_thread_fn(void* arg) {
    SolunaDriver* drv = (SolunaDriver*)arg;

    // Set real-time scheduling
    struct sched_param sp;
    sp.sched_priority = 96;
    pthread_setschedparam(pthread_self(), SCHED_RR, &sp);

    uint64_t host_now = mach_absolute_time();
    uint64_t anchor   = host_now;
    uint64_t sample_anchor = 0;

    pthread_mutex_lock(&drv->mTSMutex);
    drv->mAnchorHostTime   = anchor;
    drv->mAnchorSampleTime = sample_anchor;
    pthread_mutex_unlock(&drv->mTSMutex);

    while (atomic_load(&drv->mIORunning)) {
        // Sleep until next period
        anchor += frames_to_nanos(kZeroTSPeriod);
        mach_wait_until(anchor);

        sample_anchor += kZeroTSPeriod;

        pthread_mutex_lock(&drv->mTSMutex);
        drv->mAnchorHostTime   = anchor;
        drv->mAnchorSampleTime = sample_anchor;
        pthread_mutex_unlock(&drv->mTSMutex);
    }

    return nullptr;
}

// ── vtable implementations ────────────────────────────────────────────────────

static HRESULT Soluna_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface)
{
    if (!inDriver || !outInterface) return E_POINTER;

    SolunaDriver* drv = (SolunaDriver*)inDriver;
    CFUUIDRef uuid = CFUUIDCreateFromUUIDBytes(kCFAllocatorDefault, inUUID);
    HRESULT result = E_NOINTERFACE;

    if (CFEqual(uuid, IUnknownUUID) ||
        CFEqual(uuid, kAudioServerPlugInDriverInterfaceUUID)) {
        drv->mRefCount++;
        *outInterface = inDriver;
        result = S_OK;
    }
    CFRelease(uuid);
    return result;
}

static ULONG Soluna_AddRef(void* inDriver)
{
    if (!inDriver) return 0;
    SolunaDriver* drv = (SolunaDriver*)inDriver;
    return (ULONG)++drv->mRefCount;
}

static ULONG Soluna_Release(void* inDriver)
{
    if (!inDriver) return 0;
    SolunaDriver* drv = (SolunaDriver*)inDriver;
    ULONG rc = (ULONG)--drv->mRefCount;
    if (rc == 0) {
        if (drv->mTypeUUID)    CFRelease(drv->mTypeUUID);
        if (drv->mFactoryUUID) CFRelease(drv->mFactoryUUID);
        pthread_mutex_destroy(&drv->mTSMutex);
        soluna_shm_close(&drv->mShm);
        soluna_shm_unlink();
        free(drv);
    }
    return rc;
}

static OSStatus Soluna_Initialize(AudioServerPlugInDriverRef inDriver,
                                   AudioServerPlugInHostRef   inHost)
{
    if (!inDriver) return kAudioHardwareIllegalOperationError;
    SolunaDriver* drv = (SolunaDriver*)inDriver;
    drv->mHost = inHost;

    // SHM is created by solunad (which runs as the login user and has no sandbox).
    // The driver only opens an already-existing SHM.  If solunad is not yet running,
    // DoIOOperation will retry lazily every ~100ms.
    if (soluna_shm_open(&drv->mShm, O_RDWR) == 0 &&
        soluna_shm_validate(&drv->mShm) == 0) {
        atomic_store(&drv->mShmReady, true);
        fprintf(stderr, "[Soluna] Initialized, SHM attached (%s)\n", soluna_shm_path());
    } else {
        fprintf(stderr, "[Soluna] Initialized, SHM not yet available (start solunad)\n");
    }
    return noErr;
}

static OSStatus Soluna_CreateDevice(AudioServerPlugInDriverRef inDriver,
                                     CFDictionaryRef            inDescription,
                                     const AudioServerPlugInClientInfo* inClientInfo,
                                     AudioObjectID*             outDeviceObjectID)
{
    (void)inDriver; (void)inDescription; (void)inClientInfo; (void)outDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus Soluna_DestroyDevice(AudioServerPlugInDriverRef inDriver,
                                      AudioObjectID              inDeviceObjectID)
{
    (void)inDriver; (void)inDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus Soluna_AddDeviceClient(AudioServerPlugInDriverRef       inDriver,
                                        AudioObjectID                    inDeviceObjectID,
                                        const AudioServerPlugInClientInfo* inClientInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientInfo;
    return noErr;
}

static OSStatus Soluna_RemoveDeviceClient(AudioServerPlugInDriverRef       inDriver,
                                           AudioObjectID                    inDeviceObjectID,
                                           const AudioServerPlugInClientInfo* inClientInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientInfo;
    return noErr;
}

static OSStatus Soluna_PerformDeviceConfigurationChange(
    AudioServerPlugInDriverRef inDriver,
    AudioObjectID              inDeviceObjectID,
    UInt64                     inChangeAction,
    void*                      inChangeInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inChangeAction; (void)inChangeInfo;
    return noErr;
}

static OSStatus Soluna_AbortDeviceConfigurationChange(
    AudioServerPlugInDriverRef inDriver,
    AudioObjectID              inDeviceObjectID,
    UInt64                     inChangeAction,
    void*                      inChangeInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inChangeAction; (void)inChangeInfo;
    return noErr;
}

// ── HasProperty ──────────────────────────────────────────────────────────────

static Boolean Soluna_HasProperty(AudioServerPlugInDriverRef  inDriver,
                                   AudioObjectID               inObjectID,
                                   pid_t                       inClientPID,
                                   const AudioObjectPropertyAddress* inAddress)
{
    (void)inDriver; (void)inClientPID;

    switch (inObjectID) {
    case kSolunaPlugInID:
        switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyResourceBundle:
            return true;
        }
        return false;

    case kSolunaDeviceID:
        switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyPreferredChannelsForStereo:
        case kAudioDevicePropertyPreferredChannelLayout:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyClockIsStable:
            return true;
        }
        return false;

    case kSolunaStreamID:
        switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            return true;
        }
        return false;

    case kSolunaVolumeID:
        switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioLevelControlPropertyScalarValue:
        case kAudioLevelControlPropertyDecibelValue:
        case kAudioLevelControlPropertyDecibelRange:
            return true;
        }
        return false;
    }
    return false;
}

// ── IsPropertySettable ────────────────────────────────────────────────────────

static OSStatus Soluna_IsPropertySettable(AudioServerPlugInDriverRef inDriver,
                                           AudioObjectID inObjectID,
                                           pid_t inClientPID,
                                           const AudioObjectPropertyAddress* inAddress,
                                           Boolean* outIsSettable)
{
    (void)inDriver; (void)inClientPID;
    if (!outIsSettable) return kAudioHardwareIllegalOperationError;
    *outIsSettable = false;

    if (inObjectID == kSolunaDeviceID) {
        if (inAddress->mSelector == kAudioDevicePropertyNominalSampleRate)
            *outIsSettable = true;
    } else if (inObjectID == kSolunaStreamID) {
        if (inAddress->mSelector == kAudioStreamPropertyVirtualFormat ||
            inAddress->mSelector == kAudioStreamPropertyPhysicalFormat)
            *outIsSettable = true;
    } else if (inObjectID == kSolunaVolumeID) {
        if (inAddress->mSelector == kAudioLevelControlPropertyScalarValue ||
            inAddress->mSelector == kAudioLevelControlPropertyDecibelValue)
            *outIsSettable = true;
    }
    return noErr;
}

// ── GetPropertyDataSize ───────────────────────────────────────────────────────

static OSStatus Soluna_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver,
                                            AudioObjectID inObjectID,
                                            pid_t inClientPID,
                                            const AudioObjectPropertyAddress* inAddress,
                                            UInt32 inQualifierDataSize,
                                            const void* inQualifierData,
                                            UInt32* outDataSize)
{
    (void)inDriver; (void)inClientPID;
    (void)inQualifierDataSize; (void)inQualifierData;
    if (!outDataSize) return kAudioHardwareIllegalOperationError;

    switch (inObjectID) {
    // ── PlugIn ──
    case kSolunaPlugInID:
        switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:       *outDataSize = sizeof(AudioClassID); return noErr;
        case kAudioObjectPropertyOwner:       *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioObjectPropertyManufacturer:*outDataSize = sizeof(CFStringRef); return noErr;
        case kAudioObjectPropertyOwnedObjects:*outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioPlugInPropertyDeviceList:  *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioPlugInPropertyTranslateUIDToDevice: *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioPlugInPropertyResourceBundle: *outDataSize = sizeof(CFStringRef); return noErr;
        }
        break;

    // ── Device ──
    case kSolunaDeviceID:
        switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:       *outDataSize = sizeof(AudioClassID); return noErr;
        case kAudioObjectPropertyOwner:       *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:*outDataSize = sizeof(CFStringRef); return noErr;
        case kAudioObjectPropertyOwnedObjects:*outDataSize = 2 * sizeof(AudioObjectID); return noErr;
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:    *outDataSize = sizeof(CFStringRef); return noErr;
        case kAudioDevicePropertyTransportType:   *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyRelatedDevices:  *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioDevicePropertyClockDomain:     *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyClockIsStable:   *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:    *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyStreams:          *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioObjectPropertyControlList:      *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioDevicePropertyNominalSampleRate:*outDataSize = sizeof(Float64); return noErr;
        case kAudioDevicePropertyAvailableNominalSampleRates:
            *outDataSize = sizeof(AudioValueRange); return noErr;
        case kAudioDevicePropertyPreferredChannelsForStereo:
            *outDataSize = 2 * sizeof(UInt32); return noErr;
        case kAudioDevicePropertyPreferredChannelLayout:
            *outDataSize = offsetof(AudioChannelLayout, mChannelDescriptions[kChannels]); return noErr;
        case kAudioDevicePropertyZeroTimeStampPeriod: *outDataSize = sizeof(UInt32); return noErr;
        }
        break;

    // ── Stream ──
    case kSolunaStreamID:
        switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:       *outDataSize = sizeof(AudioClassID); return noErr;
        case kAudioObjectPropertyOwner:       *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioObjectPropertyOwnedObjects:*outDataSize = 0; return noErr;
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:      *outDataSize = sizeof(UInt32); return noErr;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            *outDataSize = sizeof(AudioStreamBasicDescription); return noErr;
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            *outDataSize = sizeof(AudioStreamRangedDescription); return noErr;
        }
        break;

    // ── Volume control ──
    case kSolunaVolumeID:
        switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:          *outDataSize = sizeof(AudioClassID); return noErr;
        case kAudioObjectPropertyOwner:          *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioObjectPropertyName:           *outDataSize = sizeof(CFStringRef); return noErr;
        case kAudioObjectPropertyOwnedObjects:   *outDataSize = 0; return noErr;
        case kAudioLevelControlPropertyScalarValue:
        case kAudioLevelControlPropertyDecibelValue: *outDataSize = sizeof(Float32); return noErr;
        case kAudioLevelControlPropertyDecibelRange: *outDataSize = sizeof(AudioValueRange); return noErr;
        }
        break;
    }
    return kAudioHardwareUnknownPropertyError;
}

// ── Helper: fill ASBD ─────────────────────────────────────────────────────────

static void fill_format(AudioStreamBasicDescription* fmt)
{
    fmt->mSampleRate       = kSampleRate;
    fmt->mFormatID         = kAudioFormatLinearPCM;
    fmt->mFormatFlags      = kAudioFormatFlagsNativeFloatPacked;
    fmt->mBitsPerChannel   = kBitsPerChannel;
    fmt->mChannelsPerFrame = kChannels;
    fmt->mFramesPerPacket  = kFramesPerPacket;
    fmt->mBytesPerFrame    = kBytesPerFrame;
    fmt->mBytesPerPacket   = kBytesPerPacket;
}

// ── GetPropertyData ───────────────────────────────────────────────────────────

static OSStatus Soluna_GetPropertyData(AudioServerPlugInDriverRef inDriver,
                                        AudioObjectID inObjectID,
                                        pid_t inClientPID,
                                        const AudioObjectPropertyAddress* inAddress,
                                        UInt32 inQualifierDataSize,
                                        const void* inQualifierData,
                                        UInt32 inDataSize,
                                        UInt32* outDataSize,
                                        void* outData)
{
    (void)inDriver; (void)inClientPID;
    (void)inQualifierDataSize; (void)inQualifierData; (void)inDataSize;
    if (!outDataSize || !outData) return kAudioHardwareIllegalOperationError;

    switch (inObjectID) {
    // ── PlugIn properties ──
    case kSolunaPlugInID:
        switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
            *outDataSize = sizeof(AudioClassID);
            *((AudioClassID*)outData) = kAudioObjectClassID;
            return noErr;
        case kAudioObjectPropertyClass:
            *outDataSize = sizeof(AudioClassID);
            *((AudioClassID*)outData) = kAudioPlugInClassID;
            return noErr;
        case kAudioObjectPropertyOwner:
            *outDataSize = sizeof(AudioObjectID);
            *((AudioObjectID*)outData) = 0; // kAudioObjectSystemObject = 0
            return noErr;
        case kAudioObjectPropertyManufacturer:
            *outDataSize = sizeof(CFStringRef);
            *((CFStringRef*)outData) = CFSTR("Soluna");
            return noErr;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
            *outDataSize = sizeof(AudioObjectID);
            *((AudioObjectID*)outData) = kSolunaDeviceID;
            return noErr;
        case kAudioPlugInPropertyTranslateUIDToDevice:
            *outDataSize = sizeof(AudioObjectID);
            *((AudioObjectID*)outData) = kSolunaDeviceID;
            return noErr;
        case kAudioPlugInPropertyResourceBundle:
            *outDataSize = sizeof(CFStringRef);
            *((CFStringRef*)outData) = CFSTR("");
            return noErr;
        }
        break;

    // ── Device properties ──
    case kSolunaDeviceID:
        switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
            *outDataSize = sizeof(AudioClassID);
            *((AudioClassID*)outData) = kAudioObjectClassID;
            return noErr;
        case kAudioObjectPropertyClass:
            *outDataSize = sizeof(AudioClassID);
            *((AudioClassID*)outData) = kAudioDeviceClassID;
            return noErr;
        case kAudioObjectPropertyOwner:
            *outDataSize = sizeof(AudioObjectID);
            *((AudioObjectID*)outData) = kSolunaPlugInID;
            return noErr;
        case kAudioObjectPropertyName:
            *outDataSize = sizeof(CFStringRef);
            *((CFStringRef*)outData) = CFSTR("Soluna");
            return noErr;
        case kAudioObjectPropertyManufacturer:
            *outDataSize = sizeof(CFStringRef);
            *((CFStringRef*)outData) = CFSTR("Soluna");
            return noErr;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
            *outDataSize = sizeof(AudioObjectID);
            *((AudioObjectID*)outData) = kSolunaStreamID;
            return noErr;
        case kAudioDevicePropertyDeviceUID:
            *outDataSize = sizeof(CFStringRef);
            *((CFStringRef*)outData) = CFSTR("audio.soluna.Soluna:output");
            return noErr;
        case kAudioDevicePropertyModelUID:
            *outDataSize = sizeof(CFStringRef);
            *((CFStringRef*)outData) = CFSTR("audio.soluna.Soluna:model");
            return noErr;
        case kAudioDevicePropertyTransportType:
            *outDataSize = sizeof(UInt32);
            *((UInt32*)outData) = kAudioDeviceTransportTypeVirtual;
            return noErr;
        case kAudioDevicePropertyRelatedDevices:
            *outDataSize = sizeof(AudioObjectID);
            *((AudioObjectID*)outData) = kSolunaDeviceID;
            return noErr;
        case kAudioDevicePropertyClockDomain:
            *outDataSize = sizeof(UInt32);
            *((UInt32*)outData) = 0;
            return noErr;
        case kAudioDevicePropertyDeviceIsAlive:
            *outDataSize = sizeof(UInt32);
            *((UInt32*)outData) = 1;
            return noErr;
        case kAudioDevicePropertyDeviceIsRunning:
            *outDataSize = sizeof(UInt32);
            *((UInt32*)outData) = (UInt32)atomic_load(
                &((SolunaDriver*)inDriver)->mIOClientCount);
            return noErr;
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            *outDataSize = sizeof(UInt32);
            *((UInt32*)outData) = 1;
            return noErr;
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
            *outDataSize = sizeof(UInt32);
            *((UInt32*)outData) = 0;
            return noErr;
        case kAudioObjectPropertyControlList:
            *outDataSize = 0;
            return noErr;
        case kAudioDevicePropertyNominalSampleRate:
            *outDataSize = sizeof(Float64);
            *((Float64*)outData) = kSampleRate;
            return noErr;
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            *outDataSize = sizeof(AudioValueRange);
            AudioValueRange* range = (AudioValueRange*)outData;
            range->mMinimum = kSampleRate;
            range->mMaximum = kSampleRate;
            return noErr;
        }
        case kAudioDevicePropertyIsHidden:
            *outDataSize = sizeof(UInt32);
            *((UInt32*)outData) = 0;
            return noErr;
        case kAudioDevicePropertyClockIsStable:
            *outDataSize = sizeof(UInt32);
            *((UInt32*)outData) = 1;
            return noErr;
        case kAudioDevicePropertyPreferredChannelsForStereo:
            *outDataSize = 2 * sizeof(UInt32);
            ((UInt32*)outData)[0] = 1;
            ((UInt32*)outData)[1] = 2;
            return noErr;
        case kAudioDevicePropertyPreferredChannelLayout: {
            UInt32 sz = (UInt32)offsetof(AudioChannelLayout, mChannelDescriptions[kChannels]);
            *outDataSize = sz;
            AudioChannelLayout* layout = (AudioChannelLayout*)outData;
            memset(layout, 0, sz);
            layout->mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
            return noErr;
        }
        case kAudioDevicePropertyZeroTimeStampPeriod:
            *outDataSize = sizeof(UInt32);
            *((UInt32*)outData) = kZeroTSPeriod;
            return noErr;
        }
        break;

    // ── Stream properties ──
    case kSolunaStreamID:
        switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
            *outDataSize = sizeof(AudioClassID);
            *((AudioClassID*)outData) = kAudioObjectClassID;
            return noErr;
        case kAudioObjectPropertyClass:
            *outDataSize = sizeof(AudioClassID);
            *((AudioClassID*)outData) = kAudioStreamClassID;
            return noErr;
        case kAudioObjectPropertyOwner:
            *outDataSize = sizeof(AudioObjectID);
            *((AudioObjectID*)outData) = kSolunaDeviceID;
            return noErr;
        case kAudioObjectPropertyOwnedObjects:
            *outDataSize = 0;
            return noErr;
        case kAudioStreamPropertyIsActive:
            *outDataSize = sizeof(UInt32);
            *((UInt32*)outData) = 1;
            return noErr;
        case kAudioStreamPropertyDirection:
            *outDataSize = sizeof(UInt32);
            *((UInt32*)outData) = 0; // output
            return noErr;
        case kAudioStreamPropertyTerminalType:
            *outDataSize = sizeof(UInt32);
            *((UInt32*)outData) = kAudioStreamTerminalTypeSpeaker;
            return noErr;
        case kAudioStreamPropertyStartingChannel:
            *outDataSize = sizeof(UInt32);
            *((UInt32*)outData) = 1;
            return noErr;
        case kAudioStreamPropertyLatency:
            *outDataSize = sizeof(UInt32);
            *((UInt32*)outData) = 0;
            return noErr;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: {
            *outDataSize = sizeof(AudioStreamBasicDescription);
            fill_format((AudioStreamBasicDescription*)outData);
            return noErr;
        }
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            *outDataSize = sizeof(AudioStreamRangedDescription);
            AudioStreamRangedDescription* d = (AudioStreamRangedDescription*)outData;
            fill_format(&d->mFormat);
            d->mSampleRateRange.mMinimum = kSampleRate;
            d->mSampleRateRange.mMaximum = kSampleRate;
            return noErr;
        }
        }
        break;
    }
    return kAudioHardwareUnknownPropertyError;
}

// ── SetPropertyData ───────────────────────────────────────────────────────────

static OSStatus Soluna_SetPropertyData(AudioServerPlugInDriverRef inDriver,
                                        AudioObjectID inObjectID,
                                        pid_t inClientPID,
                                        const AudioObjectPropertyAddress* inAddress,
                                        UInt32 inQualifierDataSize,
                                        const void* inQualifierData,
                                        UInt32 inDataSize,
                                        const void* inData)
{
    (void)inDriver; (void)inObjectID; (void)inClientPID;
    (void)inAddress; (void)inQualifierDataSize; (void)inQualifierData;
    (void)inDataSize; (void)inData;
    // We accept format changes silently (only one format supported anyway)
    return noErr;
}

// ── StartIO / StopIO ──────────────────────────────────────────────────────────

static OSStatus Soluna_StartIO(AudioServerPlugInDriverRef inDriver,
                                AudioObjectID              inDeviceObjectID,
                                UInt32                     inClientID)
{
    (void)inDeviceObjectID; (void)inClientID;
    SolunaDriver* drv = (SolunaDriver*)inDriver;

    uint32_t prev = atomic_fetch_add(&drv->mIOClientCount, 1u);
    if (prev == 0) {
        // First client: start IO thread
        atomic_store(&drv->mIORunning, true);
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_create(&drv->mIOThread, &attr, io_thread_fn, drv);
        pthread_attr_destroy(&attr);
        fprintf(stderr, "[Soluna] IO started\n");
    }
    return noErr;
}

static OSStatus Soluna_StopIO(AudioServerPlugInDriverRef inDriver,
                               AudioObjectID              inDeviceObjectID,
                               UInt32                     inClientID)
{
    (void)inDeviceObjectID; (void)inClientID;
    SolunaDriver* drv = (SolunaDriver*)inDriver;

    uint32_t prev = atomic_fetch_sub(&drv->mIOClientCount, 1u);
    if (prev == 1) {
        // Last client: stop IO thread
        atomic_store(&drv->mIORunning, false);
        pthread_join(drv->mIOThread, nullptr);
        fprintf(stderr, "[Soluna] IO stopped\n");
    }
    return noErr;
}

// ── GetZeroTimeStamp ──────────────────────────────────────────────────────────

static OSStatus Soluna_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver,
                                         AudioObjectID              inDeviceObjectID,
                                         UInt32                     inClientID,
                                         Float64*                   outSampleTime,
                                         UInt64*                    outHostTime,
                                         UInt64*                    outSeed)
{
    (void)inDeviceObjectID; (void)inClientID;
    SolunaDriver* drv = (SolunaDriver*)inDriver;

    pthread_mutex_lock(&drv->mTSMutex);
    *outSampleTime = (Float64)drv->mAnchorSampleTime;
    *outHostTime   = drv->mAnchorHostTime;
    pthread_mutex_unlock(&drv->mTSMutex);

    *outSeed = 1;
    return noErr;
}

// ── WillDoIOOperation / BeginIOOperation / EndIOOperation ─────────────────────

static OSStatus Soluna_WillDoIOOperation(AudioServerPlugInDriverRef inDriver,
                                          AudioObjectID              inDeviceObjectID,
                                          UInt32                     inClientID,
                                          UInt32                     inOperationID,
                                          Boolean*                   outWillDo,
                                          Boolean*                   outWillDoInPlace)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;
    *outWillDo        = (inOperationID == kAudioServerPlugInIOOperationWriteMix);
    *outWillDoInPlace = true;
    return noErr;
}

static OSStatus Soluna_BeginIOOperation(AudioServerPlugInDriverRef inDriver,
                                         AudioObjectID              inDeviceObjectID,
                                         UInt32                     inClientID,
                                         UInt32                     inOperationID,
                                         UInt32                     inIOBufferFrameSize,
                                         const AudioServerPlugInIOCycleInfo* inIOCycleInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;
    (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    return noErr;
}

static OSStatus Soluna_EndIOOperation(AudioServerPlugInDriverRef inDriver,
                                       AudioObjectID              inDeviceObjectID,
                                       UInt32                     inClientID,
                                       UInt32                     inOperationID,
                                       UInt32                     inIOBufferFrameSize,
                                       const AudioServerPlugInIOCycleInfo* inIOCycleInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;
    (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    return noErr;
}

// ── DoIOOperation ─────────────────────────────────────────────────────────────

static OSStatus Soluna_DoIOOperation(AudioServerPlugInDriverRef  inDriver,
                                      AudioObjectID               inDeviceObjectID,
                                      AudioObjectID               inStreamObjectID,
                                      UInt32                      inClientID,
                                      UInt32                      inOperationID,
                                      UInt32                      inIOBufferFrameSize,
                                      const AudioServerPlugInIOCycleInfo* inIOCycleInfo,
                                      void*                       ioMainBuffer,
                                      void*                       ioSecondaryBuffer)
{
    (void)inDeviceObjectID; (void)inStreamObjectID; (void)inClientID;
    (void)inIOCycleInfo; (void)ioSecondaryBuffer;

    if (inOperationID != kAudioServerPlugInIOOperationWriteMix)
        return noErr;

    SolunaDriver* drv = (SolunaDriver*)inDriver;
    if (!atomic_load(&drv->mShmReady)) {
        // Lazily retry attaching to SHM created by solunad (~every 100ms)
        static _Atomic(uint32_t) sRetryCounter = 0;
        uint32_t cnt = atomic_fetch_add(&sRetryCounter, 1u);
        if ((cnt & 0xFF) == 0) {  // every 256 calls ≈ 2-3s at 93 IO cycles/sec
            if (soluna_shm_open(&drv->mShm, O_RDWR) == 0 &&
                soluna_shm_validate(&drv->mShm) == 0) {
                atomic_store(&drv->mShmReady, true);
                fprintf(stderr, "[Soluna] SHM attached\n");
            }
        }
        return noErr;
    }

    soluna_shm_write(&drv->mShm,
                     (const float*)ioMainBuffer,
                     inIOBufferFrameSize);
    return noErr;
}

// ── vtable ────────────────────────────────────────────────────────────────────

static AudioServerPlugInDriverInterface gSolunaInterface = {
    /* _reserved                     */ nullptr,
    /* QueryInterface                */ Soluna_QueryInterface,
    /* AddRef                        */ Soluna_AddRef,
    /* Release                       */ Soluna_Release,
    /* Initialize                    */ Soluna_Initialize,
    /* CreateDevice                  */ Soluna_CreateDevice,
    /* DestroyDevice                 */ Soluna_DestroyDevice,
    /* AddDeviceClient               */ Soluna_AddDeviceClient,
    /* RemoveDeviceClient            */ Soluna_RemoveDeviceClient,
    /* PerformDeviceConfigurationChange */ Soluna_PerformDeviceConfigurationChange,
    /* AbortDeviceConfigurationChange   */ Soluna_AbortDeviceConfigurationChange,
    /* HasProperty                   */ Soluna_HasProperty,
    /* IsPropertySettable            */ Soluna_IsPropertySettable,
    /* GetPropertyDataSize           */ Soluna_GetPropertyDataSize,
    /* GetPropertyData               */ Soluna_GetPropertyData,
    /* SetPropertyData               */ Soluna_SetPropertyData,
    /* StartIO                       */ Soluna_StartIO,
    /* StopIO                        */ Soluna_StopIO,
    /* GetZeroTimeStamp              */ Soluna_GetZeroTimeStamp,
    /* WillDoIOOperation             */ Soluna_WillDoIOOperation,
    /* BeginIOOperation              */ Soluna_BeginIOOperation,
    /* DoIOOperation                 */ Soluna_DoIOOperation,
    /* EndIOOperation                */ Soluna_EndIOOperation,
};

// ── Factory ───────────────────────────────────────────────────────────────────

extern "C" void* SolunaPlugin_Create(CFAllocatorRef allocator, CFUUIDRef typeUUID)
{
    (void)allocator;

    if (!CFEqual(typeUUID, kAudioServerPlugInTypeUUID))
        return nullptr;

    SolunaDriver* drv = (SolunaDriver*)calloc(1, sizeof(SolunaDriver));
    if (!drv) return nullptr;

    // mInterface must point directly to the vtable struct (IUnknown single-pointer pattern)
    drv->mInterface    = &gSolunaInterface;
    drv->mRefCount     = 1;
    atomic_store(&drv->mIOClientCount, 0u);
    atomic_store(&drv->mIORunning,     false);
    atomic_store(&drv->mShmReady,      false);

    pthread_mutex_init(&drv->mTSMutex, nullptr);

    drv->mTypeUUID    = (CFUUIDRef)CFRetain(kAudioServerPlugInTypeUUID);
    drv->mFactoryUUID = CFUUIDCreateFromString(
        kCFAllocatorDefault,
        CFSTR("9C6BFC58-9D8D-483E-951B-EEBFDE441A99")); // matches Info.plist UUID

    fill_format(&drv->mFormat);

    fprintf(stderr, "[Soluna] Driver created\n");
    return drv;
}
