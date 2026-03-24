import Foundation
import AVFoundation
#if os(iOS)
import UIKit
#endif

/// Main entry point for the Soluna audio relay SDK.
///
/// Connects to the Soluna relay server over UDP using BSD sockets, receives
/// OSTP/RTP audio packets, decodes them, and plays them through AVAudioEngine.
///
/// Supports:
/// - Relay connection with automatic heartbeat
/// - P2P peer discovery (iOS: MultipeerConnectivity, macOS: NWBrowser/NWListener)
/// - Mic transmit to relay
/// - DJ dual-deck with crossfade
/// - Multi-output device routing (macOS only)
///
/// Usage:
/// ```swift
/// let client = SolunaClient()
/// client.connect(channel: "soluna")
/// // ...
/// client.disconnect()
/// ```
@MainActor
public final class SolunaClient: ObservableObject {

    // MARK: - Published State

    /// Whether the client is connected to the relay.
    @Published public var isConnected = false

    /// Whether audio is currently being received and played.
    @Published public var isReceivingAudio = false

    /// The current channel name.
    @Published public var channel = ""

    /// The current channel metadata (nil for custom channels).
    @Published public var currentChannel: SolunaChannel?

    /// Whether the karaoke mic monitor is active.
    @Published public var isMicMonitoring = false

    /// Whether mic is transmitting to the relay.
    @Published public var isMicTransmitting = false

    /// Whether P2P discovery is active.
    @Published public var isPeerDiscoveryActive = false

    /// Cumulative listening minutes for the current channel.
    @Published public var listenMinutes: Double = 0

    /// Current fan rank based on listening minutes.
    @Published public var fanRank: SolunaFanRank = .newFan

    // MARK: - Delegate

    /// Optional delegate for receiving raw decoded audio data.
    public weak var delegate: SolunaClientDelegate?

    // MARK: - Sub-components

    /// Mic transmitter for sending mic audio through the relay.
    public let micTransmitter = MicTransmitter()

    /// DJ deck controller for dual-deck playback with crossfade.
    public let djController = DJDeckController()

    /// P2P peer discovery instance (platform-specific).
    #if os(iOS)
    public let peerDiscovery = MultipeerDiscovery()
    #elseif os(macOS)
    public let peerDiscovery = BonjourDiscovery()

    /// Multi-output device manager (macOS only).
    public let multiOutput = MultiOutputManager()
    #endif

    // MARK: - Private

    private var connection: RelayConnection?
    private let audioPlayer = AudioPlayer()
    private var deviceName: String
    private var listenTimer: Timer?

    // MARK: - Init

    public init() {
        #if os(iOS)
        deviceName = UIDevice.current.name
        #else
        deviceName = Host.current().localizedName ?? "SolunaSDK"
        #endif
    }

    // MARK: - Channel List

    /// All available channels.
    public var channels: [SolunaChannel] { SolunaChannels.all }

    // MARK: - Connect / Disconnect

    /// Connect to the Soluna relay and start receiving audio.
    ///
    /// - Parameters:
    ///   - channel: The channel name to join.
    ///   - host: Relay server hostname (default: relay.solun.art).
    ///   - port: Relay server port (default: 5100).
    public func connect(channel: String, host: String = OSTConstants.defaultHost, port: UInt16 = OSTConstants.defaultPort) {
        guard !isConnected else { return }

        self.channel = channel
        self.currentChannel = SolunaChannels.channel(for: channel)

        #if os(iOS)
        UIApplication.shared.isIdleTimerDisabled = true
        #endif

        audioPlayer.start()

        let conn = RelayConnection(
            channel: channel,
            host: host,
            port: port,
            deviceName: deviceName
        )

        conn.onPacket = { [weak self] data in
            self?.handlePacket(data)
        }

        if conn.connect() {
            self.connection = conn
            self.isConnected = true
            startListenTimer()
            delegate?.solunaClient(self, didChangeState: .connected)
        } else {
            audioPlayer.stop()
            delegate?.solunaClient(self, didChangeState: .error("Connection failed"))
        }
    }

    /// Disconnect from the relay and stop audio playback.
    public func disconnect() {
        guard isConnected else { return }

        // Stop sub-components
        stopMicTransmit()
        stopDJDecks()
        stopPeerDiscovery()

        stopListenTimer()
        connection?.disconnect()
        connection = nil
        audioPlayer.stop()

        isConnected = false
        isReceivingAudio = false

        #if os(iOS)
        UIApplication.shared.isIdleTimerDisabled = false
        #endif

        delegate?.solunaClient(self, didChangeState: .disconnected)
    }

    /// Switch to a different channel. Reconnects automatically.
    ///
    /// - Parameter name: The new channel name.
    public func setChannel(_ name: String) {
        let wasConnected = isConnected
        let currentHost = OSTConstants.defaultHost
        let currentPort = OSTConstants.defaultPort

        if wasConnected { disconnect() }
        channel = name
        currentChannel = SolunaChannels.channel(for: name)
        if wasConnected { connect(channel: name, host: currentHost, port: currentPort) }
    }

    // MARK: - Mic Monitoring (Karaoke Mode)

    /// Toggle microphone monitoring — mixes mic input into the audio output
    /// at low latency for karaoke-style monitoring.
    public func toggleMicMonitoring() {
        if isMicMonitoring {
            stopMicMonitoring()
        } else {
            startMicMonitoring()
        }
    }

    private func startMicMonitoring() {
        audioPlayer.startMicMonitoring()
        isMicMonitoring = true
    }

    private func stopMicMonitoring() {
        audioPlayer.stopMicMonitoring()
        isMicMonitoring = false
    }

    // MARK: - Mic Transmit

    /// Start transmitting microphone audio through the relay.
    ///
    /// Captures mic input using AVAudioEngine and sends OSTP packets
    /// to the relay server, allowing other listeners to hear this device's mic.
    @discardableResult
    public func startMicTransmit() -> Bool {
        guard let connection, isConnected else { return false }
        guard !isMicTransmitting else { return true }

        #if os(iOS)
        // Switch audio session to playAndRecord for mic access
        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.playAndRecord, mode: .default,
                                    options: [.defaultToSpeaker, .allowBluetooth])
            try session.setActive(true)
        } catch {
            print("[SolunaSDK] Mic session error: \(error)")
            return false
        }
        #endif

        if micTransmitter.start(connection: connection) {
            isMicTransmitting = true
            return true
        }
        return false
    }

    /// Stop transmitting microphone audio.
    public func stopMicTransmit() {
        guard isMicTransmitting else { return }
        micTransmitter.stop()
        isMicTransmitting = false

        #if os(iOS)
        // Revert to playback-only session
        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.playback, mode: .default,
                                    options: [.defaultToSpeaker, .mixWithOthers, .allowBluetooth])
            try session.setActive(true)
        } catch {
            print("[SolunaSDK] Session revert error: \(error)")
        }
        #endif
    }

    /// Toggle mic transmit on/off.
    public func toggleMicTransmit() {
        if isMicTransmitting {
            stopMicTransmit()
        } else {
            startMicTransmit()
        }
    }

    // MARK: - DJ Deck Controls

    /// Load an audio file into DJ Deck A and start playing.
    ///
    /// - Parameter url: URL of the audio file.
    /// - Returns: True if the file was loaded and playback started.
    @discardableResult
    public func loadDeckA(url: URL) -> Bool {
        guard let connection, isConnected else { return false }
        if !djController.isActive {
            djController.start(connection: connection)
        }
        return djController.loadDeckA(url: url)
    }

    /// Load an audio file into DJ Deck B and start playing.
    ///
    /// - Parameter url: URL of the audio file.
    /// - Returns: True if the file was loaded and playback started.
    @discardableResult
    public func loadDeckB(url: URL) -> Bool {
        guard let connection, isConnected else { return false }
        if !djController.isActive {
            djController.start(connection: connection)
        }
        return djController.loadDeckB(url: url)
    }

    /// Set the DJ crossfader position (0.0 = full A, 0.5 = equal, 1.0 = full B).
    public func setDJCrossfader(_ value: Float) {
        djController.crossfader = value
    }

    /// Stop all DJ deck playback.
    public func stopDJDecks() {
        djController.stop()
    }

    // MARK: - P2P Discovery

    /// Start scanning for nearby peers on the current channel.
    ///
    /// On iOS, uses MultipeerConnectivity. On macOS, uses NWBrowser/Bonjour.
    /// Found peers will be available through the `peerDiscovery` property.
    public func startPeerDiscovery() {
        guard !isPeerDiscoveryActive else { return }

        #if os(iOS)
        peerDiscovery.channel = channel
        #elseif os(macOS)
        peerDiscovery.channel = channel
        #endif

        peerDiscovery.startScan()
        isPeerDiscoveryActive = true
    }

    /// Stop scanning for peers.
    public func stopPeerDiscovery() {
        guard isPeerDiscoveryActive else { return }
        peerDiscovery.stopScan()
        isPeerDiscoveryActive = false
    }

    #if os(macOS)
    // MARK: - Multi-Output (macOS only)

    /// Refresh the list of available audio output devices.
    public func refreshOutputDevices() {
        multiOutput.refreshDevices()
    }

    /// Add an audio output device by its CoreAudio device ID.
    public func addOutputDevice(deviceID: AudioDeviceID) {
        multiOutput.addOutput(deviceID: deviceID)
    }

    /// Remove an audio output device by its CoreAudio device ID.
    public func removeOutputDevice(deviceID: AudioDeviceID) {
        multiOutput.removeOutput(deviceID: deviceID)
    }

    /// Set the volume for a specific output device.
    public func setOutputDeviceVolume(deviceID: AudioDeviceID, volume: Float) {
        multiOutput.setVolume(deviceID: deviceID, volume: volume)
    }
    #endif

    // MARK: - Listen Timer & Fan Rank

    private func startListenTimer() {
        listenTimer = Timer.scheduledTimer(withTimeInterval: 60, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                self.listenMinutes += 1
                self.fanRank = SolunaFanRank.from(minutes: self.listenMinutes)
            }
        }
    }

    private func stopListenTimer() {
        listenTimer?.invalidate()
        listenTimer = nil
    }

    // MARK: - Packet Handling

    private func handlePacket(_ data: Data) {
        guard let packet = OSTPacketParser.parse(data) else { return }

        let channels = packet.channels
        let payloadBytes = ArraySlice(Array(packet.payload))

        // Write directly to ring buffer (no AVAudioPCMBuffer allocation)
        switch packet.payloadType {
        case OSTConstants.ptADPCMStereo, OSTConstants.ptADPCMMono:
            audioPlayer.writeADPCMPayload(payloadBytes, channels: channels)
        case OSTConstants.ptOpus, OSTConstants.ptOpusStereo, OSTConstants.ptOpusMono:
            audioPlayer.writeOpusPayload(payloadBytes, channels: max(channels, 1))
        case OSTConstants.ptS24, OSTConstants.ptFloat32:
            audioPlayer.writeS24Payload(payloadBytes, channels: channels)
        default:
            break // Unknown PT — skip
        }

        if !isReceivingAudio {
            DispatchQueue.main.async { [weak self] in
                self?.isReceivingAudio = true
            }
        }
    }
}
