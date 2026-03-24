#if os(macOS)
import Foundation
import CoreAudio

// MARK: - AudioOutputDevice

/// Information about a macOS audio output device.
///
/// Mirrors the C++ SolunaDeviceInfo but implemented in pure Swift
/// using CoreAudio's AudioObjectGetPropertyData APIs.
public struct AudioOutputDevice: Identifiable, Hashable, Sendable {
    /// CoreAudio device ID.
    public let id: AudioDeviceID

    /// Human-readable device name.
    public let name: String

    /// Number of output channels.
    public let outputChannels: UInt32

    /// Transport type (built-in, USB, Bluetooth, AirPlay, virtual).
    public let transportType: AudioOutputTransportType

    /// Hardware latency in frames.
    public let hardwareLatencyFrames: UInt32

    /// Safety offset in frames.
    public let safetyOffsetFrames: UInt32

    /// Native sample rate in Hz.
    public let nativeSampleRate: Double

    /// Approximate hardware latency in milliseconds (at 48kHz).
    public var hardwareLatencyMs: Float {
        Float(hardwareLatencyFrames) / 48.0
    }
}

// MARK: - Transport Type

/// Transport type of an audio output device.
public enum AudioOutputTransportType: Int, Sendable {
    case builtIn = 0
    case usb = 1
    case bluetooth = 2
    case airPlay = 3
    case virtual_ = 4
    case unknown = 255
}

// MARK: - MultiOutputManager

/// Manages multiple audio output devices on macOS.
///
/// Uses CoreAudio APIs to enumerate output devices, add/remove outputs,
/// and control per-device volume. This is a pure Swift implementation
/// matching the C++ addOutputDevice/removeOutputDevice bridge methods.
public final class MultiOutputManager: ObservableObject {

    /// All currently available output devices.
    @Published public private(set) var availableDevices: [AudioOutputDevice] = []

    /// Device IDs of currently active outputs.
    @Published public private(set) var activeOutputIDs: Set<AudioDeviceID> = []

    /// Per-device volume (0.0 to 1.0). Keyed by device ID.
    @Published public var deviceVolumes: [AudioDeviceID: Float] = [:]

    public init() {
        refreshDevices()
    }

    // MARK: - Device Enumeration

    /// Refresh the list of available output devices from CoreAudio.
    public func refreshDevices() {
        availableDevices = Self.listDevices()
    }

    /// List all available audio output devices.
    public static func listDevices() -> [AudioOutputDevice] {
        var result: [AudioOutputDevice] = []

        // Get all audio devices
        var prop = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var dataSize: UInt32 = 0
        var status = AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &prop, 0, nil, &dataSize)
        guard status == noErr, dataSize > 0 else { return [] }

        let count = Int(dataSize) / MemoryLayout<AudioDeviceID>.size
        var deviceIDs = [AudioDeviceID](repeating: 0, count: count)
        status = AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &prop, 0, nil, &dataSize, &deviceIDs)
        guard status == noErr else { return [] }

        for devID in deviceIDs {
            // Check output channels
            var chProp = AudioObjectPropertyAddress(
                mSelector: kAudioDevicePropertyStreamConfiguration,
                mScope: kAudioDevicePropertyScopeOutput,
                mElement: kAudioObjectPropertyElementMain
            )
            var chSize: UInt32 = 0
            status = AudioObjectGetPropertyDataSize(devID, &chProp, 0, nil, &chSize)
            guard status == noErr, chSize > 0 else { continue }

            let chBuf = UnsafeMutableRawPointer.allocate(byteCount: Int(chSize), alignment: MemoryLayout<AudioBufferList>.alignment)
            defer { chBuf.deallocate() }
            status = AudioObjectGetPropertyData(devID, &chProp, 0, nil, &chSize, chBuf)
            guard status == noErr else { continue }

            let bufList = chBuf.assumingMemoryBound(to: AudioBufferList.self)
            var outCh: UInt32 = 0
            let bufferCount = Int(bufList.pointee.mNumberBuffers)
            // Access the first buffer directly, then use pointer arithmetic for rest
            if bufferCount > 0 {
                withUnsafeMutablePointer(to: &bufList.pointee.mBuffers) { firstBuf in
                    for i in 0..<bufferCount {
                        outCh += firstBuf[i].mNumberChannels
                    }
                }
            }
            guard outCh > 0 else { continue }

            // Get device name
            var nameProp = AudioObjectPropertyAddress(
                mSelector: kAudioDevicePropertyDeviceNameCFString,
                mScope: kAudioObjectPropertyScopeGlobal,
                mElement: kAudioObjectPropertyElementMain
            )
            let name: String
            do {
                var nameRef: Unmanaged<CFString>?
                var nameSize = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
                let status2 = AudioObjectGetPropertyData(devID, &nameProp, 0, nil, &nameSize, &nameRef)
                if status2 == noErr, let ref = nameRef {
                    name = ref.takeRetainedValue() as String
                } else {
                    name = "Unknown"
                }
            }

            // Get transport type
            var transport: UInt32 = 0
            var tSz = UInt32(MemoryLayout<UInt32>.size)
            var tProp = AudioObjectPropertyAddress(
                mSelector: kAudioDevicePropertyTransportType,
                mScope: kAudioObjectPropertyScopeGlobal,
                mElement: kAudioObjectPropertyElementMain
            )
            AudioObjectGetPropertyData(devID, &tProp, 0, nil, &tSz, &transport)

            let transportType: AudioOutputTransportType
            switch transport {
            case kAudioDeviceTransportTypeBuiltIn:
                transportType = .builtIn
            case kAudioDeviceTransportTypeUSB:
                transportType = .usb
            case kAudioDeviceTransportTypeBluetooth, kAudioDeviceTransportTypeBluetoothLE:
                transportType = .bluetooth
            case kAudioDeviceTransportTypeAirPlay:
                transportType = .airPlay
            case kAudioDeviceTransportTypeVirtual, kAudioDeviceTransportTypeAggregate:
                transportType = .virtual_
            default:
                transportType = .unknown
            }

            // Get hardware latency + safety offset
            var latency: UInt32 = 0
            var safety: UInt32 = 0
            var sz = UInt32(MemoryLayout<UInt32>.size)
            var lProp = AudioObjectPropertyAddress(
                mSelector: kAudioDevicePropertyLatency,
                mScope: kAudioDevicePropertyScopeOutput,
                mElement: kAudioObjectPropertyElementMain
            )
            AudioObjectGetPropertyData(devID, &lProp, 0, nil, &sz, &latency)
            sz = UInt32(MemoryLayout<UInt32>.size)
            var sProp = AudioObjectPropertyAddress(
                mSelector: kAudioDevicePropertySafetyOffset,
                mScope: kAudioDevicePropertyScopeOutput,
                mElement: kAudioObjectPropertyElementMain
            )
            AudioObjectGetPropertyData(devID, &sProp, 0, nil, &sz, &safety)

            // Get native sample rate
            var rate: Float64 = 0
            sz = UInt32(MemoryLayout<Float64>.size)
            var rProp = AudioObjectPropertyAddress(
                mSelector: kAudioDevicePropertyNominalSampleRate,
                mScope: kAudioObjectPropertyScopeGlobal,
                mElement: kAudioObjectPropertyElementMain
            )
            AudioObjectGetPropertyData(devID, &rProp, 0, nil, &sz, &rate)

            result.append(AudioOutputDevice(
                id: devID,
                name: name,
                outputChannels: outCh,
                transportType: transportType,
                hardwareLatencyFrames: latency,
                safetyOffsetFrames: safety,
                nativeSampleRate: rate
            ))
        }

        return result
    }

    // MARK: - Output Management

    /// Add a device as an active output.
    ///
    /// - Parameter deviceID: The CoreAudio device ID to add.
    public func addOutput(deviceID: AudioDeviceID) {
        activeOutputIDs.insert(deviceID)
        if deviceVolumes[deviceID] == nil {
            deviceVolumes[deviceID] = 1.0
        }
    }

    /// Remove a device from the active outputs.
    ///
    /// - Parameter deviceID: The CoreAudio device ID to remove.
    public func removeOutput(deviceID: AudioDeviceID) {
        activeOutputIDs.remove(deviceID)
        deviceVolumes.removeValue(forKey: deviceID)
    }

    /// Set the volume for a specific output device.
    ///
    /// - Parameters:
    ///   - deviceID: The CoreAudio device ID.
    ///   - volume: Volume level from 0.0 (silent) to 1.0 (full).
    public func setVolume(deviceID: AudioDeviceID, volume: Float) {
        let clamped = max(0.0, min(1.0, volume))
        deviceVolumes[deviceID] = clamped

        // Apply volume to the CoreAudio device directly
        var vol = Float32(clamped)
        var volumeProp = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyVolumeScalar,
            mScope: kAudioDevicePropertyScopeOutput,
            mElement: kAudioObjectPropertyElementMain
        )

        // Try master channel first (element 0), then individual channels
        let hasMasterVolume = AudioObjectHasProperty(deviceID, &volumeProp)
        if hasMasterVolume {
            AudioObjectSetPropertyData(deviceID, &volumeProp, 0, nil,
                                       UInt32(MemoryLayout<Float32>.size), &vol)
        }

        // Also set channels 1 and 2 (L/R)
        for ch: UInt32 in 1...2 {
            volumeProp.mElement = ch
            if AudioObjectHasProperty(deviceID, &volumeProp) {
                AudioObjectSetPropertyData(deviceID, &volumeProp, 0, nil,
                                           UInt32(MemoryLayout<Float32>.size), &vol)
            }
        }
    }

    /// Get the current default output device ID.
    public static var defaultOutputDeviceID: AudioDeviceID {
        var deviceID: AudioDeviceID = 0
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        var prop = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &prop, 0, nil, &size, &deviceID)
        return deviceID
    }
}

#endif // os(macOS)
