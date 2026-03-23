//
//  AudioSourceManager.swift
//  SolunaReceiverMac
//
//  Enumerates available audio input sources (microphones, virtual devices,
//  system audio via ScreenCaptureKit) for the Pro Dashboard broadcaster picker.
//

import Foundation
import AVFoundation
import CoreAudio

@MainActor
class AudioSourceManager: ObservableObject {
    static let shared = AudioSourceManager()

    struct AudioSource: Identifiable, Hashable {
        let id: String
        let name: String
        let type: SourceType

        enum SourceType: String, Hashable {
            case microphone
            case systemAudio
            case inputDevice
        }
    }

    @Published var sources: [AudioSource] = []
    @Published var selectedSource: AudioSource?
    @Published var isCapturing = false

    private init() {
        refresh()
    }

    func refresh() {
        var result: [AudioSource] = []

        // System Audio (ScreenCaptureKit, macOS 13+)
        if #available(macOS 13.0, *) {
            result.append(AudioSource(
                id: "system-audio",
                name: "System Audio",
                type: .systemAudio
            ))
        }

        // Enumerate CoreAudio input devices
        var propAddress = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var size: UInt32 = 0
        var status = AudioObjectGetPropertyDataSize(
            AudioObjectID(kAudioObjectSystemObject),
            &propAddress, 0, nil, &size
        )
        guard status == noErr, size > 0 else { return }

        let count = Int(size) / MemoryLayout<AudioDeviceID>.size
        var deviceIDs = [AudioDeviceID](repeating: 0, count: count)
        status = AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &propAddress, 0, nil, &size, &deviceIDs
        )
        guard status == noErr else { return }

        for deviceID in deviceIDs {
            guard hasInputChannels(deviceID) else { continue }
            guard let name = deviceName(deviceID) else { continue }

            let lowerName = name.lowercased()
            let isMic = lowerName.contains("microphone")
                || lowerName.contains("macbook")
                || lowerName.contains("built-in")

            result.append(AudioSource(
                id: "device-\(deviceID)",
                name: name,
                type: isMic ? .microphone : .inputDevice
            ))
        }

        sources = result
        // Preserve selection if still valid, otherwise pick first
        if let current = selectedSource, !result.contains(where: { $0.id == current.id }) {
            selectedSource = result.first
        } else if selectedSource == nil {
            selectedSource = result.first
        }
    }

    // MARK: - CoreAudio Helpers

    private func hasInputChannels(_ deviceID: AudioDeviceID) -> Bool {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyStreamConfiguration,
            mScope: kAudioDevicePropertyScopeInput,
            mElement: kAudioObjectPropertyElementMain
        )
        var size: UInt32 = 0
        let status = AudioObjectGetPropertyDataSize(deviceID, &address, 0, nil, &size)
        guard status == noErr, size > 0 else { return false }

        let bufferListPtr = UnsafeMutableRawPointer.allocate(
            byteCount: Int(size),
            alignment: MemoryLayout<AudioBufferList>.alignment
        )
        defer { bufferListPtr.deallocate() }

        var mutableSize = size
        let getStatus = AudioObjectGetPropertyData(deviceID, &address, 0, nil, &mutableSize, bufferListPtr)
        guard getStatus == noErr else { return false }

        let bufferList = bufferListPtr.assumingMemoryBound(to: AudioBufferList.self)
        let bufferCount = Int(bufferList.pointee.mNumberBuffers)
        guard bufferCount > 0 else { return false }

        let buffers = UnsafeMutableAudioBufferListPointer(bufferList)
        let totalChannels = buffers.reduce(0) { $0 + Int($1.mNumberChannels) }
        return totalChannels > 0
    }

    private func deviceName(_ deviceID: AudioDeviceID) -> String? {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioObjectPropertyName,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var name: CFString = "" as CFString
        var size = UInt32(MemoryLayout<CFString>.size)
        let status = AudioObjectGetPropertyData(deviceID, &address, 0, nil, &size, &name)
        guard status == noErr else { return nil }
        let result = name as String
        return result.isEmpty ? nil : result
    }
}
