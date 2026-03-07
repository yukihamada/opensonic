//
//  AudioReceiver.swift
//  SolunaReceiverMac
//
//  Swift wrapper for the Objective-C++ audio receiver bridge
//

import Foundation
import Combine
import Network
import MediaPlayer
import AVFoundation

/// Transport type (mirrors SolunaTransportType)
enum LocalTransportType: String {
    case builtIn   = "Built-in"
    case usb       = "USB"
    case bluetooth = "Bluetooth"
    case airPlay   = "AirPlay"
    case virtual_  = "Virtual"
    case unknown   = "Unknown"

    init(from objc: SolunaTransportType) {
        switch objc {
        case .builtIn:   self = .builtIn
        case .usb:       self = .usb
        case .bluetooth: self = .bluetooth
        case .airPlay:   self = .airPlay
        case .virtual:   self = .virtual_
        case .unknown:   self = .unknown
        @unknown default: self = .unknown
        }
    }

    var iconName: String {
        switch self {
        case .builtIn:   return "desktopcomputer"
        case .usb:       return "cable.connector"
        case .bluetooth: return "wave.3.right"
        case .airPlay:   return "airplayaudio"
        case .virtual_:  return "waveform"
        case .unknown:   return "speaker.fill"
        }
    }
}

/// A local audio output device discovered via CoreAudio
struct LocalOutputDevice: Identifiable {
    let id: UInt32           // CoreAudio device ID
    let name: String
    let transportType: LocalTransportType
    let hardwareLatencyMs: Float
    let outputChannels: UInt32
    let nativeSampleRate: Double  // Hz (e.g. 44100, 48000, 96000)
    var sinkIndex: Int = -1  // -1 = not active
    var volume: Float = 1.0
    var muted: Bool = false
    var delayMs: Float = 0
    var manualOffsetMs: Float = 0  // ±50ms fine-tuning offset
    var balance: Float = 0         // -1.0 = left, 0.0 = center, 1.0 = right

    var isActive: Bool { sinkIndex >= 0 }
}

/// A saved routing preset (active devices + per-device settings)
struct AudioRoutingPreset: Identifiable, Codable {
    var id: UUID = UUID()
    var name: String
    var deviceConfigs: [DeviceConfig]

    struct DeviceConfig: Codable {
        let deviceId: UInt32
        var volume: Float
        var muted: Bool
        var manualOffsetMs: Float
    }
}

/// A named group of speakers (zone)
struct SpeakerGroup: Identifiable, Codable {
    var id: UUID = UUID()
    var name: String
    var deviceNames: [String]  // by name for cross-session persistence
    var volume: Float = 1.0
    var muted: Bool = false
}

/// Observable wrapper for SolunaAudioReceiver
@MainActor
final class AudioReceiver: ObservableObject {

    static let shared = AudioReceiver()

    /// Connection state
    enum State: String {
        case stopped = "Stopped"
        case connecting = "Connecting..."
        case receiving = "Receiving"
        case error = "Error"

        init(from objcState: SolunaReceiverState) {
            switch objcState {
            case .stopped:
                self = .stopped
            case .connecting:
                self = .connecting
            case .receiving:
                self = .receiving
            case .error:
                self = .error
            @unknown default:
                self = .stopped
            }
        }
    }

    /// Current connection state
    @Published private(set) var state: State = .stopped

    /// Number of packets received
    @Published private(set) var packetsReceived: UInt64 = 0

    /// Number of packets dropped
    @Published private(set) var packetsDropped: UInt64 = 0

    /// Number of packets concealed by PLC
    @Published private(set) var packetsConcealed: UInt64 = 0

    /// Device health (good / stressed / silenced based on underrun rate)
    @Published private(set) var deviceHealth: SolunaDeviceHealth = .good

    /// Current volume (0.0 - 1.0)
    @Published var volume: Float = 1.0 {
        didSet {
            if !isMuted {
                receiver.volume = volume
            }
        }
    }

    /// Mute state — preserves volume level
    @Published var isMuted: Bool = false {
        didSet {
            receiver.volume = isMuted ? 0 : volume
        }
    }

    /// Jitter buffer target in ms (5–200 ms, default 100 ms)
    @Published var bufferMs: UInt32 = 40 {
        didSet {
            receiver.bufferTargetMs = bufferMs
        }
    }

    /// Whether mic transmit is active
    @Published private(set) var isMicTransmitting: Bool = false

    /// TX packets sent
    @Published private(set) var txPacketsSent: UInt64 = 0

    /// Mic input level (0.0 - 1.0) for UI meter
    @Published private(set) var micInputLevel: Float = 0.0

    /// Error message if any
    @Published private(set) var errorMessage: String?

    /// Available local output devices (BT, AirPlay, USB, etc.)
    @Published var availableDevices: [LocalOutputDevice] = []

    /// Currently active extra output device IDs
    @Published private(set) var activeOutputs: Set<UInt32> = []

    /// Primary output hardware latency in ms (~5ms for built-in)
    var primaryLatencyMs: Float = 5.0

    /// Latency history samples per device (device ID → recent latency values)
    @Published var latencyHistory: [UInt32: [Float]] = [:]
    private static let maxHistorySamples = 60

    /// Multicast group address
    var multicastGroup: String {
        get { receiver.multicastGroup }
        set { receiver.multicastGroup = newValue }
    }

    /// RTP port
    var port: UInt16 {
        get { receiver.port }
        set { receiver.port = newValue }
    }

    /// Number of channels
    var channels: UInt32 {
        get { receiver.channels }
        set { receiver.channels = newValue }
    }

    /// Whether currently playing
    var isPlaying: Bool {
        state == .receiving || state == .connecting
    }

    private static let savedDevicesKey = "soluna_active_local_devices"
    private static let manualOffsetsKey = "soluna_manual_offsets"
    private static let presetsKey = "soluna_routing_presets"
    private static let favoritesKey = "soluna_favorite_devices"

    /// Favorite device names (auto-enable on connect)
    @Published var favoriteDeviceNames: Set<String> = []

    /// Speaker groups (zones)
    @Published var speakerGroups: [SpeakerGroup] = []
    private static let groupsKey = "soluna_speaker_groups"

    /// Saved routing presets
    @Published var presets: [AudioRoutingPreset] = []

    private let receiver: SolunaAudioReceiver
    private let delegateHandler: DelegateHandler
    private let networkMonitor = NWPathMonitor()
    private var wasPlaying = false

    init() {
        receiver = SolunaAudioReceiver.sharedInstance()
        delegateHandler = DelegateHandler()
        delegateHandler.audioReceiver = self
        receiver.delegate = delegateHandler

        // Load presets, favorites, groups
        loadPresets()
        if let names = UserDefaults.standard.stringArray(forKey: Self.favoritesKey) {
            favoriteDeviceNames = Set(names)
        }
        if let data = UserDefaults.standard.data(forKey: Self.groupsKey),
           let saved = try? JSONDecoder().decode([SpeakerGroup].self, from: data) {
            speakerGroups = saved
        }

        // Initial device scan
        refreshDevices()

        // Listen for hot-plug events
        receiver.setDeviceChangeCallback { [weak self] in
            Task { @MainActor in
                self?.refreshDevices()
            }
        }

        // Network auto-reconnect
        networkMonitor.pathUpdateHandler = { [weak self] path in
            Task { @MainActor in
                guard let self else { return }
                if path.status == .satisfied && self.wasPlaying && !self.isPlaying {
                    self.start()
                } else if path.status != .satisfied && self.isPlaying {
                    self.wasPlaying = true
                    self.stop()
                }
            }
        }
        networkMonitor.start(queue: DispatchQueue(label: "soluna.netmon"))

        // Now Playing / media key support
        setupNowPlaying()
    }

    private func setupNowPlaying() {
        let center = MPRemoteCommandCenter.shared()
        center.playCommand.addTarget { [weak self] _ in
            guard let self, !self.isPlaying else { return .commandFailed }
            self.start()
            return .success
        }
        center.pauseCommand.addTarget { [weak self] _ in
            guard let self, self.isPlaying else { return .commandFailed }
            self.toggle()
            return .success
        }
        center.togglePlayPauseCommand.addTarget { [weak self] _ in
            self?.toggle()
            return .success
        }
        center.changePlaybackPositionCommand.isEnabled = false
        center.nextTrackCommand.isEnabled = false
        center.previousTrackCommand.isEnabled = false
    }

    private func updateNowPlaying() {
        var info = [String: Any]()
        info[MPMediaItemPropertyTitle] = "Soluna Rx"
        info[MPMediaItemPropertyArtist] = isPlaying ? "Receiving Audio" : "Stopped"
        info[MPNowPlayingInfoPropertyPlaybackRate] = isPlaying ? 1.0 : 0.0
        info[MPNowPlayingInfoPropertyElapsedPlaybackTime] = 0
        MPNowPlayingInfoCenter.default().nowPlayingInfo = info
        MPNowPlayingInfoCenter.default().playbackState = isPlaying ? .playing : .paused
    }

    /// Start receiving audio.
    /// On macOS, MultipeerConnectivity is not available, so we skip peer scanning
    /// and go straight to multicast reception.
    func start() {
        guard state == .stopped || state == .error else { return }
        errorMessage = nil
        state = .connecting

        let ok = receiver.start()
        if !ok { return } // bridge sets state -> .error via delegate

        // Restore previously active local devices
        refreshDevices()
        restoreSavedDevices()
        updateNowPlaying()
    }

    /// Stop receiving audio
    func stop() {
        // Stop mic TX if active
        if isMicTransmitting {
            receiver.stopMicTransmit()
            isMicTransmitting = false
        }

        state = .stopped
        receiver.stop()
        PeerRelayManager.shared.stop()
        updateNowPlaying()
    }

    /// Toggle microphone transmit on/off
    func toggleMic() {
        if isMicTransmitting {
            receiver.stopMicTransmit()
            isMicTransmitting = false
        } else {
            // On macOS, request mic permission then start
            if #available(macOS 14.0, *) {
                AVAudioApplication.requestRecordPermission { [weak self] granted in
                    Task { @MainActor in
                        guard let self, granted else { return }
                        if self.receiver.startMicTransmit() {
                            self.isMicTransmitting = true
                        }
                    }
                }
            } else {
                // macOS 13 and earlier: just start (permission granted by system)
                if receiver.startMicTransmit() {
                    isMicTransmitting = true
                }
            }
        }
    }

    /// Toggle play/stop (user-initiated)
    func toggle() {
        if isPlaying {
            wasPlaying = false  // User chose to stop — don't auto-reconnect
            stop()
        } else {
            start()
        }
    }

    // MARK: - Local Output Devices

    /// Refresh the list of available local output devices
    func refreshDevices() {
        let devices = SolunaAudioReceiver.availableOutputDevices()
        let savedOffsets = UserDefaults.standard.dictionary(forKey: Self.manualOffsetsKey) as? [String: Float] ?? [:]
        availableDevices = devices.map { info in
            let existing = availableDevices.first(where: { $0.id == info.deviceId })
            let offset = existing?.manualOffsetMs ?? savedOffsets["\(info.deviceId)"] ?? 0
            return LocalOutputDevice(
                id: info.deviceId,
                name: info.name,
                transportType: LocalTransportType(from: info.transportType),
                hardwareLatencyMs: info.hardwareLatencyMs,
                outputChannels: info.outputChannels,
                nativeSampleRate: info.nativeSampleRate,
                sinkIndex: existing?.sinkIndex ?? -1,
                volume: existing?.volume ?? 1.0,
                muted: existing?.muted ?? false,
                delayMs: existing?.delayMs ?? 0,
                manualOffsetMs: offset
            )
        }
        // Remove active outputs for disconnected devices
        let ids = Set(availableDevices.map(\.id))
        for deviceId in activeOutputs {
            if !ids.contains(deviceId) {
                receiver.removeOutputDevice(deviceId)
                activeOutputs.remove(deviceId)
            }
        }

        // Auto-enable favorite devices that just connected
        if isPlaying {
            for dev in availableDevices {
                if favoriteDeviceNames.contains(dev.name) && !activeOutputs.contains(dev.id) {
                    enableDevice(dev.id)
                }
            }
        }
    }

    /// Enable an extra output device
    func enableDevice(_ deviceId: UInt32) {
        let idx = receiver.addOutputDevice(deviceId)
        if idx >= 0 {
            activeOutputs.insert(deviceId)
            if let i = availableDevices.firstIndex(where: { $0.id == deviceId }) {
                availableDevices[i].sinkIndex = Int(idx)
            }
            persistActiveDevices()
        }
    }

    /// Disable an extra output device
    func disableDevice(_ deviceId: UInt32) {
        receiver.removeOutputDevice(deviceId)
        activeOutputs.remove(deviceId)
        if let i = availableDevices.firstIndex(where: { $0.id == deviceId }) {
            availableDevices[i].sinkIndex = -1
        }
        persistActiveDevices()
    }

    /// Save active device IDs to UserDefaults
    private func persistActiveDevices() {
        let ids = activeOutputs.map { Int($0) }
        UserDefaults.standard.set(ids, forKey: Self.savedDevicesKey)
    }

    /// Toggle favorite status for a device (by name, for cross-session persistence)
    func toggleFavorite(_ name: String) {
        if favoriteDeviceNames.contains(name) {
            favoriteDeviceNames.remove(name)
        } else {
            favoriteDeviceNames.insert(name)
        }
        UserDefaults.standard.set(Array(favoriteDeviceNames), forKey: Self.favoritesKey)
    }

    // MARK: - Speaker Groups

    func createGroup(name: String, deviceNames: [String]) {
        speakerGroups.append(SpeakerGroup(name: name, deviceNames: deviceNames))
        persistGroups()
    }

    func deleteGroup(_ id: UUID) {
        speakerGroups.removeAll { $0.id == id }
        persistGroups()
    }

    func setGroupVolume(_ id: UUID, volume: Float) {
        guard let gi = speakerGroups.firstIndex(where: { $0.id == id }) else { return }
        speakerGroups[gi].volume = volume
        for name in speakerGroups[gi].deviceNames {
            if let dev = availableDevices.first(where: { $0.name == name && $0.isActive }) {
                setDeviceVolume(dev.id, volume: volume)
            }
        }
        persistGroups()
    }

    func setGroupMuted(_ id: UUID, muted: Bool) {
        guard let gi = speakerGroups.firstIndex(where: { $0.id == id }) else { return }
        speakerGroups[gi].muted = muted
        for name in speakerGroups[gi].deviceNames {
            if let dev = availableDevices.first(where: { $0.name == name && $0.isActive }) {
                setDeviceMuted(dev.id, muted: muted)
            }
        }
        persistGroups()
    }

    private func persistGroups() {
        if let data = try? JSONEncoder().encode(speakerGroups) {
            UserDefaults.standard.set(data, forKey: Self.groupsKey)
        }
    }

    /// Save manual offsets to UserDefaults (deviceId → ms)
    private func persistManualOffsets() {
        var dict: [String: Float] = [:]
        for dev in availableDevices where dev.manualOffsetMs != 0 {
            dict["\(dev.id)"] = dev.manualOffsetMs
        }
        UserDefaults.standard.set(dict, forKey: Self.manualOffsetsKey)
    }

    /// Restore previously active devices (call after refreshDevices)
    func restoreSavedDevices() {
        guard let saved = UserDefaults.standard.array(forKey: Self.savedDevicesKey) as? [Int] else { return }
        let available = Set(availableDevices.map(\.id))
        for id in saved {
            let deviceId = UInt32(id)
            if available.contains(deviceId) && !activeOutputs.contains(deviceId) {
                enableDevice(deviceId)
            }
        }
    }

    /// Set volume on an extra output device
    func setDeviceVolume(_ deviceId: UInt32, volume: Float) {
        if let i = availableDevices.firstIndex(where: { $0.id == deviceId }),
           availableDevices[i].sinkIndex >= 0 {
            availableDevices[i].volume = volume
            receiver.setVolume(volume, forOutput: Int32(availableDevices[i].sinkIndex))
        }
    }

    /// Set muted on an extra output device
    func setDeviceMuted(_ deviceId: UInt32, muted: Bool) {
        if let i = availableDevices.firstIndex(where: { $0.id == deviceId }),
           availableDevices[i].sinkIndex >= 0 {
            availableDevices[i].muted = muted
            receiver.setMuted(muted, forOutput: Int32(availableDevices[i].sinkIndex))
        }
    }

    /// Set delay frames on an extra output device
    func setDeviceDelayMs(_ deviceId: UInt32, ms: Float) {
        if let i = availableDevices.firstIndex(where: { $0.id == deviceId }),
           availableDevices[i].sinkIndex >= 0 {
            availableDevices[i].delayMs = ms
            let total = ms + availableDevices[i].manualOffsetMs
            let frames = UInt32(max(0, total) * 48.0)
            receiver.setDelay(frames, forOutput: Int32(availableDevices[i].sinkIndex))
        }
    }

    /// Set manual delay offset (±50ms) on an extra output device
    func setDeviceManualOffset(_ deviceId: UInt32, ms: Float) {
        if let i = availableDevices.firstIndex(where: { $0.id == deviceId }),
           availableDevices[i].sinkIndex >= 0 {
            availableDevices[i].manualOffsetMs = ms
            let total = availableDevices[i].delayMs + ms
            let frames = UInt32(max(0, total) * 48.0)
            receiver.setDelay(frames, forOutput: Int32(availableDevices[i].sinkIndex))
            persistManualOffsets()
        }
    }

    /// Set delay on primary output
    func setPrimaryDelayMs(_ ms: Float) {
        receiver.setPrimaryDelay(UInt32(ms * 48.0))
    }

    /// Get measured latency for a device (EMA-smoothed, includes BT/AirPlay transport)
    func measuredLatencyMs(for deviceId: UInt32) -> Float {
        return receiver.measuredLatency(forDevice: deviceId)
    }

    /// Recalculate sync delays so all outputs are aligned to the slowest.
    /// Uses measured latency (real-time EMA) when available, falls back to
    /// hardware-reported latency for newly added devices.
    func recalculateSyncDelays() {
        // Collect all latencies — prefer measured over reported
        var maxLatency = primaryLatencyMs
        for dev in availableDevices where dev.isActive {
            let measured = measuredLatencyMs(for: dev.id)
            let latency = measured > 0 ? measured : dev.hardwareLatencyMs
            maxLatency = max(maxLatency, latency)
        }

        // Set compensation: primary
        let primaryComp = maxLatency - primaryLatencyMs
        setPrimaryDelayMs(max(0, primaryComp))

        // Set compensation: extra outputs
        for dev in availableDevices where dev.isActive {
            let measured = measuredLatencyMs(for: dev.id)
            let latency = measured > 0 ? measured : dev.hardwareLatencyMs
            let comp = maxLatency - latency
            setDeviceDelayMs(dev.id, ms: max(0, comp))
        }
    }

    // MARK: - Recording

    /// Whether currently recording
    @Published private(set) var isRecording: Bool = false

    /// Start recording to a WAV file. Returns the file path on success.
    @discardableResult
    func startRecording() -> String? {
        let dir = FileManager.default.urls(for: .musicDirectory, in: .userDomainMask).first
            ?? FileManager.default.temporaryDirectory
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyy-MM-dd_HH-mm-ss"
        let filename = "Soluna_\(formatter.string(from: Date())).wav"
        let path = dir.appendingPathComponent(filename).path

        if receiver.startRecording(toFile: path) {
            isRecording = true
            return path
        }
        return nil
    }

    /// Stop recording
    func stopRecording() {
        receiver.stopRecording()
        isRecording = false
    }

    // MARK: - Latency History

    /// Record current latency samples for all active devices (call periodically)
    func sampleLatencies() {
        for dev in availableDevices where dev.isActive {
            let ms = measuredLatencyMs(for: dev.id)
            if ms > 0 {
                var history = latencyHistory[dev.id] ?? []
                history.append(ms)
                if history.count > Self.maxHistorySamples {
                    history.removeFirst(history.count - Self.maxHistorySamples)
                }
                latencyHistory[dev.id] = history
            }
        }
    }

    // MARK: - Exclusive Mode

    /// Set exclusive (hog) mode on an extra output device (USB DAC bit-perfect)
    @discardableResult
    func setDeviceExclusive(_ deviceId: UInt32, exclusive: Bool) -> Bool {
        if let i = availableDevices.firstIndex(where: { $0.id == deviceId }),
           availableDevices[i].sinkIndex >= 0 {
            return receiver.setExclusive(exclusive, forOutput: Int32(availableDevices[i].sinkIndex))
        }
        return false
    }

    // MARK: - L/R Balance

    /// Set L/R balance on primary output (-1.0 = left, 0.0 = center, 1.0 = right)
    func setPrimaryBalance(_ balance: Float) {
        receiver.setPrimaryBalance(balance)
    }

    /// Set L/R balance on an extra output device
    func setDeviceBalance(_ deviceId: UInt32, balance: Float) {
        if let i = availableDevices.firstIndex(where: { $0.id == deviceId }),
           availableDevices[i].sinkIndex >= 0 {
            receiver.setBalance(balance, forOutput: Int32(availableDevices[i].sinkIndex))
        }
    }

    // MARK: - 3-Band EQ

    /// Set EQ band gain on primary output (band: 0=low, 1=mid, 2=high; gain in dB, -12..+12)
    func setPrimaryEQ(band: Int, gain: Float) {
        receiver.setPrimaryEQBand(Int32(band), gain: gain)
    }

    /// Set EQ band gain on an extra output device
    func setDeviceEQ(_ deviceId: UInt32, band: Int, gain: Float) {
        if let i = availableDevices.firstIndex(where: { $0.id == deviceId }),
           availableDevices[i].sinkIndex >= 0 {
            receiver.setEQBand(Int32(band), gain: gain, forOutput: Int32(availableDevices[i].sinkIndex))
        }
    }

    // MARK: - Compressor

    /// Set compressor on primary output
    func setPrimaryCompressor(threshold: Float, ratio: Float, attack: Float, release: Float, enabled: Bool) {
        receiver.setPrimaryCompressorThreshold(threshold, ratio: ratio, attack: attack, release: release, enabled: enabled)
    }

    /// Set compressor on an extra output device
    func setDeviceCompressor(_ deviceId: UInt32, threshold: Float, ratio: Float, attack: Float, release: Float, enabled: Bool) {
        if let i = availableDevices.firstIndex(where: { $0.id == deviceId }),
           availableDevices[i].sinkIndex >= 0 {
            receiver.setCompressorThreshold(threshold, ratio: ratio, attack: attack, release: release, enabled: enabled, forOutput: Int32(availableDevices[i].sinkIndex))
        }
    }

    // MARK: - Crossover

    /// Set crossover on primary output (mode: 0=off, 1=LPF, 2=HPF)
    func setPrimaryCrossover(mode: Int, frequency: Float) {
        receiver.setPrimaryCrossoverMode(Int32(mode), frequency: frequency)
    }

    /// Set crossover on an extra output device
    func setDeviceCrossover(_ deviceId: UInt32, mode: Int, frequency: Float) {
        if let i = availableDevices.firstIndex(where: { $0.id == deviceId }),
           availableDevices[i].sinkIndex >= 0 {
            receiver.setCrossoverMode(Int32(mode), frequency: frequency, forOutput: Int32(availableDevices[i].sinkIndex))
        }
    }

    // MARK: - Spatial Audio

    /// Set spatial audio on primary output
    func setPrimarySpatial(enabled: Bool, width: Float, crossfeed: Float) {
        receiver.setPrimarySpatialEnabled(enabled, width: width, crossfeed: crossfeed)
    }

    /// Set spatial audio on an extra output device
    func setDeviceSpatial(_ deviceId: UInt32, enabled: Bool, width: Float, crossfeed: Float) {
        if let i = availableDevices.firstIndex(where: { $0.id == deviceId }),
           availableDevices[i].sinkIndex >= 0 {
            receiver.setSpatialEnabled(enabled, width: width, crossfeed: crossfeed, forOutput: Int32(availableDevices[i].sinkIndex))
        }
    }

    // MARK: - Routing Presets

    /// Save the current active device configuration as a preset
    func savePreset(name: String) {
        var configs: [AudioRoutingPreset.DeviceConfig] = []
        for dev in availableDevices where dev.isActive {
            configs.append(.init(
                deviceId: dev.id,
                volume: dev.volume,
                muted: dev.muted,
                manualOffsetMs: dev.manualOffsetMs
            ))
        }
        let preset = AudioRoutingPreset(name: name, deviceConfigs: configs)
        presets.append(preset)
        persistPresets()
    }

    /// Apply a saved preset (enable listed devices, disable others)
    func applyPreset(_ preset: AudioRoutingPreset) {
        let available = Set(availableDevices.map(\.id))
        // Disable all currently active extra outputs
        for deviceId in activeOutputs {
            disableDevice(deviceId)
        }
        // Enable and configure preset devices
        for config in preset.deviceConfigs {
            guard available.contains(config.deviceId) else { continue }
            enableDevice(config.deviceId)
            setDeviceVolume(config.deviceId, volume: config.volume)
            setDeviceMuted(config.deviceId, muted: config.muted)
            setDeviceManualOffset(config.deviceId, ms: config.manualOffsetMs)
        }
        recalculateSyncDelays()
    }

    /// Delete a preset
    func deletePreset(_ id: UUID) {
        presets.removeAll { $0.id == id }
        persistPresets()
    }

    private func persistPresets() {
        if let data = try? JSONEncoder().encode(presets) {
            UserDefaults.standard.set(data, forKey: Self.presetsKey)
        }
    }

    private func loadPresets() {
        if let data = UserDefaults.standard.data(forKey: Self.presetsKey),
           let saved = try? JSONDecoder().decode([AudioRoutingPreset].self, from: data) {
            presets = saved
        }
    }

    /// Export all presets to a JSON file URL
    func exportPresets(to url: URL) throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let data = try encoder.encode(presets)
        try data.write(to: url)
    }

    /// Import presets from a JSON file URL (merges, skips duplicates by name)
    func importPresets(from url: URL) throws {
        let data = try Data(contentsOf: url)
        let imported = try JSONDecoder().decode([AudioRoutingPreset].self, from: data)
        let existingNames = Set(presets.map(\.name))
        for p in imported where !existingNames.contains(p.name) {
            presets.append(p)
        }
        persistPresets()
    }

    // MARK: - VU Meter

    /// Primary output RMS level (0.0–1.0)
    func primaryLevelRms() -> Float { receiver.primaryLevelRms() }

    /// Primary output peak level (0.0–1.0, with decay)
    func primaryLevelPeak() -> Float { receiver.primaryLevelPeak() }

    /// Spectrum analyzer: 32-band FFT magnitudes (0.0-1.0)
    func spectrumBands() -> [Float] {
        receiver.spectrumBands().map { $0.floatValue }
    }

    /// Extra output RMS level by device ID
    func deviceLevelRms(for deviceId: UInt32) -> Float { receiver.levelRms(forDevice: deviceId) }

    /// Extra output peak level by device ID
    func deviceLevelPeak(for deviceId: UInt32) -> Float { receiver.levelPeak(forDevice: deviceId) }

    // MARK: - Internal delegate handling

    fileprivate func handleStateChange(_ newState: SolunaReceiverState) {
        self.state = State(from: newState)
    }

    fileprivate func handleStatsUpdate(_ stats: SolunaReceiverStats) {
        self.packetsReceived  = stats.packetsReceived
        self.packetsDropped   = stats.packetsDropped
        self.packetsConcealed = stats.packetsConcealed
        self.deviceHealth     = receiver.deviceHealth
        self.txPacketsSent    = receiver.txPacketsSent
        self.micInputLevel    = receiver.micInputLevel
        self.isMicTransmitting = receiver.isMicTransmitting
    }

    fileprivate func handleError(_ error: Error) {
        self.errorMessage = error.localizedDescription
    }
}

// MARK: - Delegate Handler

/// NSObject subclass to handle Objective-C delegate callbacks
private final class DelegateHandler: NSObject, SolunaReceiverDelegate {
    weak var audioReceiver: AudioReceiver?

    func receiverDidChange(_ state: SolunaReceiverState) {
        Task { @MainActor in
            audioReceiver?.handleStateChange(state)
        }
    }

    func receiverDidUpdate(_ stats: SolunaReceiverStats) {
        Task { @MainActor in
            audioReceiver?.handleStatsUpdate(stats)
        }
    }

    func receiverDidEncounter(_ error: Error) {
        Task { @MainActor in
            audioReceiver?.handleError(error)
        }
    }
}
