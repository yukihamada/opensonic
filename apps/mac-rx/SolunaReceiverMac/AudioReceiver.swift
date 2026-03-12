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

    /// Synchronized playback mode — all receivers output at same wall-clock time
    /// Default: true (perfectly synced playback across devices)
    /// When toggled off (Jam/Fast mode), buffer is minimized for lowest latency
    @Published var isSyncMode: Bool = true {
        didSet {
            receiver.syncMode = isSyncMode
            if !isSyncMode {
                bufferMs = 5
            } else {
                bufferMs = 60  // matched with iOS for sync
            }
        }
    }

    /// Target end-to-end delay in ms (50-1000) for sync mode
    @Published var syncDelayMs: UInt32 = 200 {
        didSet { receiver.syncDelayMs = syncDelayMs }
    }

    /// Whether mic transmit is active
    @Published private(set) var isMicTransmitting: Bool = false

    /// TX packets sent
    @Published private(set) var txPacketsSent: UInt64 = 0

    /// Mic input level (0.0 - 1.0) for UI meter
    @Published private(set) var micInputLevel: Float = 0.0

    // ── System Audio Transmit (Soluna Virtual Device) ──────────────────

    /// Whether system audio transmit is active
    @Published private(set) var isShmTransmitting: Bool = false

    /// System audio TX packets sent
    @Published private(set) var shmTxPacketsSent: UInt64 = 0

    /// System audio TX level (0.0 - 1.0) for UI meter
    @Published private(set) var shmTxLevel: Float = 0.0

    /// Error message if any
    @Published private(set) var errorMessage: String?

    // ── WAN Relay (Group Code) ──────────────────────────────────────────

    /// WAN relay connection state
    enum RelayState: String {
        case disconnected = "Disconnected"
        case connecting   = "Connecting..."
        case connected    = "Connected"
        case error        = "Error"

        init(from objc: SolunaRelayState) {
            switch objc {
            case .disconnected: self = .disconnected
            case .connecting:   self = .connecting
            case .connected:    self = .connected
            case .error:        self = .error
            @unknown default:   self = .disconnected
            }
        }
    }

    @Published private(set) var relayState: RelayState = .disconnected
    @Published private(set) var relayGroup: String?
    @Published private(set) var relayError: String?

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

        // Setup metadata and file-sync callbacks (stored in C++ for when relay connects)
        setupMetaCallback()

        // Auto-connect to WAN relay for default channel
        let ch = UserDefaults.standard.string(forKey: "channel") ?? "soluna"
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { [weak self] in
            guard let self, self.state == .receiving, self.relayState == .disconnected else { return }
            self.connectRelay(group: ch)
        }
    }

    /// Stop receiving audio
    func stop() {
        // Stop transmitters if active
        if isMicTransmitting {
            receiver.stopMicTransmit()
            isMicTransmitting = false
        }
        if isShmTransmitting {
            receiver.stopShmTransmit()
            isShmTransmitting = false
        }

        // Disconnect WAN relay
        disconnectRelay()

        // Clean up file-sync
        stopFileSyncPump()
        fileSyncQueue.sync { fileSyncAudioFile = nil }
        currentSyncFile = nil
        pendingSyncCmd = nil
        receiver.networkDisabled = false
        receiver.setMetaCallback(nil)
        receiver.setFileCallback(nil)
        receiver.setSyncCallback(nil)

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

    /// Toggle system audio transmit (Soluna virtual device) on/off
    func toggleShmTransmit() {
        if isShmTransmitting {
            receiver.stopShmTransmit()
            isShmTransmitting = false
        } else {
            if receiver.startShmTransmit() {
                isShmTransmitting = true
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

    // MARK: - WAN Relay

    /// Connect to WAN relay server with group code
    func connectRelay(group: String, password: String = "",
                      host: String = "46.225.77.119", port: UInt16 = 5100) {
        NSLog("[FileSync] connectRelay called, isPlaying=\(isPlaying), state=\(state), relayState=\(relayState)")
        guard isPlaying else {
            NSLog("[FileSync] connectRelay skipped: not playing")
            return
        }
        relayState = .connecting
        relayError = nil

        // Run connection on background to avoid blocking UI during DNS/JOIN
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self else { return }
            let ok = self.receiver.connect(toRelay: host, port: port, group: group, password: password)
            NSLog("[FileSync] connectRelay result: \(ok)")
            Task { @MainActor in
                self.updateRelayState()
            }
        }
    }

    /// Disconnect from WAN relay
    func disconnectRelay() {
        receiver.disconnectRelay()
        updateRelayState()
    }

    /// Update relay state from bridge (called from stats timer)
    func updateRelayState() {
        relayState = RelayState(from: receiver.relayState)
        relayGroup = receiver.relayGroup
        relayError = receiver.relayError
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

    // MARK: - Metadata & File Sync

    private func setupMetaCallback() {
        receiver.setMetaCallback { [weak self] jsonStr in
            guard let self, let data = jsonStr.data(using: .utf8),
                  let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return }
            // Update now playing info from META messages
            let title = json["title"] as? String ?? json["track"] as? String
            let artist = json["artist"] as? String
            var info = MPNowPlayingInfoCenter.default().nowPlayingInfo ?? [:]
            info[MPMediaItemPropertyTitle] = title ?? "Soluna Rx"
            info[MPMediaItemPropertyArtist] = artist ?? "Receiving Audio"
            MPNowPlayingInfoCenter.default().nowPlayingInfo = info
        }

        // FILE: callback — download music file for file-sync mode
        receiver.setFileCallback { [weak self] filename in
            guard let self else { return }
            NSLog("[FileSync] FILE received: \(filename)")
            self.downloadAndPrepare(filename: filename)
        }

        // SYNC: callback — play/pause/seek in file-sync mode
        receiver.setSyncCallback { [weak self] syncCmd in
            guard let self else { return }
            NSLog("[FileSync] SYNC received: \(syncCmd)")
            self.handleSyncCommand(syncCmd)
        }
        NSLog("[FileSync] Callbacks registered")
    }

    // MARK: - File Sync Mode

    /// Serial queue protecting all file-sync mutable state:
    /// fileSyncAudioFile, fileSyncConverter, fileSyncPlaying, fileSyncOutputFormat
    private let fileSyncQueue = DispatchQueue(label: "com.soluna.filesync", qos: .userInteractive)

    private var currentSyncFile: String?
    private var pendingSyncCmd: String?
    private var fileSyncTimer: DispatchSourceTimer?
    // Protected by fileSyncQueue:
    private var fileSyncAudioFile: AVAudioFile?
    private var fileSyncConverter: AVAudioConverter?
    private var fileSyncOutputFormat: AVAudioFormat?
    private var fileSyncPlaying: Bool = false
    private static let kPipelineSampleRate: Double = 48000

    private func downloadAndPrepare(filename: String) {
        NSLog("[FileSync] downloadAndPrepare: %@", filename)
        let encoded = filename.addingPercentEncoding(withAllowedCharacters: .alphanumerics.union(CharacterSet(charactersIn: "-._~"))) ?? filename
        guard let url = URL(string: "http://46.225.77.119:5102/api/music/\(encoded)") else {
            NSLog("[FileSync] URL encoding failed for: %@", filename)
            return
        }

        // New track: stop current file-sync pump
        stopFileSyncPump()
        fileSyncQueue.sync {
            fileSyncAudioFile = nil
        }
        pendingSyncCmd = nil
        // Re-enable PCM streaming during download gap
        receiver.networkDisabled = false
        NSLog("[FileSync] PCM streaming re-enabled for track transition")

        currentSyncFile = filename

        // Check if already cached
        let cacheDir = FileManager.default.temporaryDirectory.appendingPathComponent("soluna-music")
        try? FileManager.default.createDirectory(at: cacheDir, withIntermediateDirectories: true)
        let localFile = cacheDir.appendingPathComponent(filename)

        if FileManager.default.fileExists(atPath: localFile.path) {
            NSLog("[FileSync] Cached: \(localFile.path)")
            prepareAudioFile(url: localFile)
            receiver.sendReady(filename)
            return
        }

        // Download
        NSLog("[FileSync] Downloading: %@", url.absoluteString)
        URLSession.shared.downloadTask(with: url) { [weak self] tempURL, response, error in
            if let error {
                NSLog("[FileSync] Download error: \(error)")
                return
            }
            guard let self, let tempURL else {
                NSLog("[FileSync] Download failed: no tempURL")
                return
            }
            let httpCode = (response as? HTTPURLResponse)?.statusCode ?? 0
            NSLog("[FileSync] Downloaded: HTTP \(httpCode)")
            guard httpCode == 200 else {
                NSLog("[FileSync] Skipping non-200 response")
                return
            }
            do {
                try FileManager.default.moveItem(at: tempURL, to: localFile)
            } catch {
                NSLog("[FileSync] Move error: \(error)")
            }
            DispatchQueue.main.async {
                self.prepareAudioFile(url: localFile)
                self.receiver.sendReady(filename)
            }
        }.resume()
    }

    private func prepareAudioFile(url: URL) {
        NSLog("[FileSync] prepareAudioFile: %@", url.lastPathComponent)
        do {
            let audioFile = try AVAudioFile(forReading: url)
            let srcFmt = audioFile.processingFormat
            let dstChannels = UInt32(receiver.channels)
            guard let dstFmt = AVAudioFormat(standardFormatWithSampleRate: Self.kPipelineSampleRate,
                                              channels: dstChannels) else {
                NSLog("[FileSync] Failed to create output format")
                return
            }

            let converter: AVAudioConverter?
            if srcFmt.sampleRate != Self.kPipelineSampleRate || srcFmt.channelCount != dstChannels {
                converter = AVAudioConverter(from: srcFmt, to: dstFmt)
                NSLog("[FileSync] Converter: %.0fHz/%dch -> %.0fHz/%dch",
                      srcFmt.sampleRate, srcFmt.channelCount, dstFmt.sampleRate, dstChannels)
            } else {
                converter = nil
                NSLog("[FileSync] No conversion needed")
            }

            fileSyncQueue.sync {
                fileSyncAudioFile = audioFile
                fileSyncConverter = converter
                fileSyncOutputFormat = dstFmt
            }

            NSLog("[FileSync] Audio file ready: %.0f Hz, %d ch, %lld frames",
                  srcFmt.sampleRate, srcFmt.channelCount, audioFile.length)

            // Replay pending SYNC if any
            if let pending = pendingSyncCmd {
                NSLog("[FileSync] Replaying pending sync")
                pendingSyncCmd = nil
                handleSyncCommand(pending)
            }
        } catch {
            NSLog("[FileSync] Failed to open audio file: %@", error.localizedDescription)
        }
    }

    /// Start a high-frequency timer that reads PCM from the audio file and injects into ring buffer
    private func startFileSyncPump() {
        stopFileSyncPump()

        // Snapshot state under lock
        let (audioFile, dstFmt): (AVAudioFile?, AVAudioFormat?) = fileSyncQueue.sync {
            return (fileSyncAudioFile, fileSyncOutputFormat)
        }
        guard let audioFile, let dstFmt else { return }

        // Output at 48000Hz, 10ms per pump = 480 frames
        let outFramesPerPump = AVAudioFrameCount(Self.kPipelineSampleRate * 0.01)
        let dstChannels = dstFmt.channelCount

        fileSyncQueue.sync {
            fileSyncPlaying = true
        }
        // Flush stale PCM data from ring buffer before prefilling with file-sync audio
        receiver.flushRingBuffer()
        receiver.networkDisabled = true
        NSLog("[FileSync] Pump started: %d out frames/pump @ 48kHz (buffer flushed)", outFramesPerPump)

        // Prefill: inject 200ms worth of audio upfront to satisfy ring buffer target
        let prefillPumps = 20  // 20 × 10ms = 200ms
        for _ in 0..<prefillPumps {
            let (pAudioFile, pConverter): (AVAudioFile?, AVAudioConverter?) = fileSyncQueue.sync {
                return (fileSyncAudioFile, fileSyncConverter)
            }
            guard let pAudioFile else { break }
            let outBuf: AVAudioPCMBuffer
            if let converter = pConverter {
                guard let outputBuffer = AVAudioPCMBuffer(pcmFormat: dstFmt,
                                                           frameCapacity: outFramesPerPump) else { break }
                let _ = converter.convert(to: outputBuffer, error: nil) { inNumPackets, outStatus in
                    guard let inputBuffer = AVAudioPCMBuffer(pcmFormat: pAudioFile.processingFormat,
                                                              frameCapacity: inNumPackets) else {
                        outStatus.pointee = .noDataNow
                        return nil
                    }
                    do {
                        try pAudioFile.read(into: inputBuffer, frameCount: inNumPackets)
                        if inputBuffer.frameLength == 0 {
                            outStatus.pointee = .endOfStream
                            return nil
                        }
                        outStatus.pointee = .haveData
                        return inputBuffer
                    } catch {
                        outStatus.pointee = .endOfStream
                        return nil
                    }
                }
                if outputBuffer.frameLength == 0 { break }
                outBuf = outputBuffer
            } else {
                guard let directBuf = AVAudioPCMBuffer(pcmFormat: dstFmt,
                                                        frameCapacity: outFramesPerPump) else { break }
                do {
                    try pAudioFile.read(into: directBuf, frameCount: outFramesPerPump)
                    if directBuf.frameLength == 0 { break }
                } catch { break }
                outBuf = directBuf
            }
            let frames = Int(outBuf.frameLength)
            let chCount = Int(dstChannels)
            var int32Buf = [Int32](repeating: 0, count: frames * chCount)
            for ch in 0..<chCount {
                guard let chData = outBuf.floatChannelData?[ch] else { continue }
                for i in 0..<frames {
                    let sample = chData[i]
                    let scaled = max(-1.0, min(1.0, sample)) * 8388608.0
                    int32Buf[i * chCount + ch] = Int32(clamping: Int64(scaled))
                }
            }
            let data = Data(bytes: &int32Buf, count: int32Buf.count * MemoryLayout<Int32>.size)
            receiver.injectPcmSamples(data, frameCount: UInt(frames))
        }
        NSLog("[FileSync] Prefilled %d pumps (200ms)", prefillPumps)

        let timer = DispatchSource.makeTimerSource(queue: fileSyncQueue)
        timer.schedule(deadline: .now(), repeating: .milliseconds(10), leeway: .milliseconds(1))
        timer.setEventHandler { [weak self] in
            // Already on fileSyncQueue -- safe to access file-sync state directly
            guard let self, self.fileSyncPlaying, let audioFile = self.fileSyncAudioFile else { return }

            let outBuf: AVAudioPCMBuffer

            if let converter = self.fileSyncConverter {
                // Need sample rate conversion
                guard let outputBuffer = AVAudioPCMBuffer(pcmFormat: dstFmt,
                                                           frameCapacity: outFramesPerPump) else { return }
                var gotData = false
                let status = converter.convert(to: outputBuffer, error: nil) { inNumPackets, outStatus in
                    guard let inputBuffer = AVAudioPCMBuffer(pcmFormat: audioFile.processingFormat,
                                                              frameCapacity: inNumPackets) else {
                        outStatus.pointee = .noDataNow
                        return nil
                    }
                    do {
                        try audioFile.read(into: inputBuffer, frameCount: inNumPackets)
                        if inputBuffer.frameLength == 0 {
                            outStatus.pointee = .endOfStream
                            return nil
                        }
                        gotData = true
                        outStatus.pointee = .haveData
                        return inputBuffer
                    } catch {
                        outStatus.pointee = .endOfStream
                        return nil
                    }
                }

                if status == .error || (!gotData && outputBuffer.frameLength == 0) {
                    // Dispatch to main to avoid deadlock (we are on fileSyncQueue)
                    DispatchQueue.main.async {
                        NSLog("[FileSync] End of track, stopping pump")
                        self.stopFileSyncPump()
                        self.receiver.networkDisabled = false
                    }
                    return
                }
                outBuf = outputBuffer
            } else {
                // No conversion needed (already 48kHz)
                guard let directBuf = AVAudioPCMBuffer(pcmFormat: dstFmt,
                                                        frameCapacity: outFramesPerPump) else { return }
                do {
                    try audioFile.read(into: directBuf, frameCount: outFramesPerPump)
                    if directBuf.frameLength == 0 {
                        DispatchQueue.main.async {
                            NSLog("[FileSync] End of track, stopping pump")
                            self.stopFileSyncPump()
                            self.receiver.networkDisabled = false
                        }
                        return
                    }
                } catch {
                    DispatchQueue.main.async {
                        self.stopFileSyncPump()
                        self.receiver.networkDisabled = false
                    }
                    return
                }
                outBuf = directBuf
            }

            // Convert float32 non-interleaved -> int32 interleaved for ring buffer
            let frames = Int(outBuf.frameLength)
            let chCount = Int(dstChannels)
            var int32Buf = [Int32](repeating: 0, count: frames * chCount)

            for ch in 0..<chCount {
                guard let chData = outBuf.floatChannelData?[ch] else { continue }
                for i in 0..<frames {
                    let sample = chData[i]
                    let scaled = max(-1.0, min(1.0, sample)) * 8388608.0
                    int32Buf[i * chCount + ch] = Int32(clamping: Int64(scaled))
                }
            }

            let data = Data(bytes: &int32Buf, count: int32Buf.count * MemoryLayout<Int32>.size)
            self.receiver.injectPcmSamples(data, frameCount: UInt(frames))
        }
        timer.resume()
        fileSyncTimer = timer
    }

    private func stopFileSyncPump() {
        fileSyncQueue.sync {
            fileSyncPlaying = false
        }
        fileSyncTimer?.cancel()
        fileSyncTimer = nil
    }

    private func handleSyncCommand(_ cmd: String) {
        let hasFile = fileSyncQueue.sync { fileSyncAudioFile != nil }
        NSLog("[FileSync] handleSync: %@, hasFile=%d", cmd, hasFile ? 1 : 0)

        // If audio file isn't loaded yet, store command for replay after download
        guard hasFile else {
            NSLog("[FileSync] Audio file not ready, storing pending sync")
            pendingSyncCmd = cmd
            return
        }

        let parts = cmd.split(separator: ":")
        guard let action = parts.first else { return }

        switch action {
        case "play":
            // Format: play:<pos_ms>:<wall_clock_ms>
            guard parts.count >= 3,
                  let posMs = Double(parts[1]),
                  let wallMs = Double(parts[2]) else { return }

            let nowMs = Date().timeIntervalSince1970 * 1000
            let elapsedMs = nowMs - wallMs
            // Add prefill offset: the ring buffer will be prefilled with 200ms of audio
            // before playback starts, so we need to seek 200ms ahead to compensate.
            let prefillMs: Double = 200.0
            let currentPosMs = posMs + max(0, elapsedMs) + prefillMs

            let (srcRate, totalFrames) = fileSyncQueue.sync { () -> (Double, AVAudioFramePosition) in
                guard let af = fileSyncAudioFile else { return (48000, 0) }
                return (af.processingFormat.sampleRate, af.length)
            }
            let durationMs = Double(totalFrames) / srcRate * 1000.0
            NSLog("[FileSync] play: seekTo=%.0fms (elapsed=%.0f + prefill=%.0f), duration=%.0fms",
                  currentPosMs, elapsedMs, prefillMs, durationMs)

            if currentPosMs >= durationMs {
                NSLog("[FileSync] Seek past end of track, keeping PCM streaming")
                return
            }

            // Seek to position in source audio file (use source sample rate)
            let seekFrame = AVAudioFramePosition(currentPosMs / 1000.0 * srcRate)
            fileSyncQueue.sync {
                fileSyncAudioFile?.framePosition = seekFrame
                fileSyncConverter?.reset()
            }
            NSLog("[FileSync] Seeking to frame %lld / %lld", seekFrame, totalFrames)

            startFileSyncPump()

        case "pause":
            stopFileSyncPump()
            receiver.networkDisabled = false

        case "seek":
            guard parts.count >= 2, let posMs = Double(parts[1]) else { return }
            fileSyncQueue.sync {
                guard let audioFile = fileSyncAudioFile else { return }
                let seekFrame = AVAudioFramePosition(posMs / 1000.0 * audioFile.processingFormat.sampleRate)
                audioFile.framePosition = seekFrame
                fileSyncConverter?.reset()
            }

        case "skip":
            stopFileSyncPump()
            fileSyncQueue.sync {
                fileSyncAudioFile = nil
            }
            receiver.networkDisabled = false
            NSLog("[FileSync] PCM streaming re-enabled (file-sync ended)")

        default:
            break
        }
    }

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
        self.isShmTransmitting = receiver.isShmTransmitting
        self.shmTxPacketsSent  = receiver.shmTxPacketsSent
        self.shmTxLevel        = receiver.shmTxLevel
        // Poll WAN relay state
        updateRelayState()
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
