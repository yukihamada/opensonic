//
//  AudioReceiver.swift
//  SolunaReceiver
//
//  Swift wrapper for the Objective-C++ audio receiver bridge
//

import Foundation
import Combine
import AVFoundation
import UIKit
import Network

/// Observable wrapper for SolunaAudioReceiver
@MainActor
final class AudioReceiver: ObservableObject {

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

    private let receiver: SolunaAudioReceiver
    private let delegateHandler: DelegateHandler
    private let networkMonitor = NWPathMonitor()
    private var wasPlayingBeforeDisconnect = false
    private var suppressInterruption = false
    private var interruptionObserver: Any?
    private var watchdogTimer: Timer?
    private var lastPacketCount: UInt64 = 0
    private var staleTicks: Int = 0

    init() {
        receiver = SolunaAudioReceiver.sharedInstance()
        delegateHandler = DelegateHandler()
        delegateHandler.audioReceiver = self
        receiver.delegate = delegateHandler
        setupNetworkMonitor()
        setupAudioInterruptionHandler()
    }

    /// Start receiving audio with discovery-first P2P.
    /// Scans for nearby peers on the same channel for 3 seconds.
    /// If a peer is found → receive from them (no multicast).
    /// If not → start multicast and become a relay for others.
    func start() {
        guard state == .stopped || state == .error else { return }
        errorMessage = nil
        state = .connecting   // visual feedback during scan

        // Configure audio session for reliable background playback
        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.playback, mode: .default, options: [.duckOthers])
            try session.setPreferredIOBufferDuration(0.01) // 10ms
            try session.setActive(true)
        } catch {
            print("[AudioReceiver] AVAudioSession error: \(error)")
        }

        // Prevent screen lock during playback
        UIApplication.shared.isIdleTimerDisabled = true

        let ch = UserDefaults.standard.string(forKey: "channel") ?? "soluna"

        Task {
            let foundPeer = await PeerRelayManager.shared.scanForPeers(channel: ch)

            guard state == .connecting else { return } // user stopped during scan

            let ok = receiver.start()   // starts audio output + ring buffer
            if !ok { return }           // bridge sets state → .error via delegate

            if foundPeer {
                // Peer mode — multicast already disabled by PeerRelayManager.
                // Audio arrives via injectRawPacket → ring buffer → audio callback.
            } else {
                // Direct mode — wait for stable multicast, then become relay
                try? await Task.sleep(nanoseconds: 6_000_000_000)
                guard state == .receiving else { return }
                PeerRelayManager.shared.becomeDirectRelay()
            }

            // Start watchdog for auto-reconnect
            startWatchdog()
        }
    }

    /// Stop receiving audio
    func stop() {
        // Stop mic TX if active
        if isMicTransmitting {
            receiver.stopMicTransmit()
            isMicTransmitting = false
        }

        stopWatchdog()
        state = .stopped
        receiver.stop()
        PeerRelayManager.shared.stop()
        UIApplication.shared.isIdleTimerDisabled = false
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }

    /// Toggle microphone transmit on/off
    func toggleMic() {
        if isMicTransmitting {
            suppressInterruption = true
            receiver.stopMicTransmit()
            isMicTransmitting = false
            // Restore playback-only session
            do {
                let session = AVAudioSession.sharedInstance()
                try session.setCategory(.playback, mode: .default, options: [.duckOthers])
                try session.setActive(true)
            } catch {
                print("[AudioReceiver] Session restore error: \(error)")
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + 3.0) { [weak self] in
                self?.suppressInterruption = false
            }
        } else {
            AVAudioSession.sharedInstance().requestRecordPermission { [weak self] granted in
                Task { @MainActor in
                    guard let self, granted else { return }
                    // Switch to playAndRecord BEFORE starting mic
                    self.suppressInterruption = true
                    do {
                        let session = AVAudioSession.sharedInstance()
                        try session.setCategory(.playAndRecord, mode: .default,
                                                options: [.defaultToSpeaker, .allowBluetooth])
                        try session.setActive(true)
                    } catch {
                        print("[AudioReceiver] Session error: \(error)")
                        self.suppressInterruption = false
                        return
                    }
                    if self.receiver.startMicTransmit() {
                        self.isMicTransmitting = true
                    }
                    DispatchQueue.main.asyncAfter(deadline: .now() + 3.0) { [weak self] in
                        self?.suppressInterruption = false
                    }
                }
            }
        }
    }

    /// Toggle play/stop
    func toggle() {
        if isPlaying {
            stop()
        } else {
            start()
        }
    }

    /// Auto-start on app launch (called from ContentView.onAppear)
    func autoStart() {
        guard state == .stopped else { return }
        start()
    }

    // MARK: - Watchdog (auto-reconnect on stream loss)

    private func startWatchdog() {
        stopWatchdog()
        lastPacketCount = packetsReceived
        staleTicks = 0
        watchdogTimer = Timer.scheduledTimer(withTimeInterval: 3.0, repeats: true) { [weak self] _ in
            Task { @MainActor in
                self?.watchdogTick()
            }
        }
    }

    private func stopWatchdog() {
        watchdogTimer?.invalidate()
        watchdogTimer = nil
        staleTicks = 0
    }

    private func watchdogTick() {
        guard state == .receiving || state == .connecting else { return }
        // Don't reconnect while mic is transmitting — reconnect kills the mic
        guard !isMicTransmitting else { return }

        if packetsReceived == lastPacketCount {
            staleTicks += 1
            // 3 ticks × 3s = 9 seconds with no new packets → reconnect
            if staleTicks >= 3 && state == .receiving {
                print("[AudioReceiver] Watchdog: no packets for \(staleTicks * 3)s — reconnecting")
                reconnect()
            }
        } else {
            staleTicks = 0
            lastPacketCount = packetsReceived
        }
    }

    /// Reconnect: stop then start with a brief delay
    private func reconnect() {
        stop()
        Task {
            try? await Task.sleep(nanoseconds: 1_000_000_000) // 1s
            start()
        }
    }

    // MARK: - Auto-Reconnect (network / interruption)

    private func setupNetworkMonitor() {
        networkMonitor.pathUpdateHandler = { [weak self] path in
            Task { @MainActor in
                guard let self else { return }
                if path.status == .satisfied && self.wasPlayingBeforeDisconnect {
                    print("[AudioReceiver] Network restored — reconnecting")
                    self.wasPlayingBeforeDisconnect = false
                    // Brief delay for WiFi to stabilize
                    try? await Task.sleep(nanoseconds: 1_500_000_000)
                    self.start()
                } else if path.status != .satisfied && self.isPlaying {
                    print("[AudioReceiver] Network lost — will reconnect when available")
                    self.wasPlayingBeforeDisconnect = true
                    self.stop()
                }
            }
        }
        networkMonitor.start(queue: DispatchQueue.global(qos: .utility))
    }

    private func setupAudioInterruptionHandler() {
        interruptionObserver = NotificationCenter.default.addObserver(
            forName: AVAudioSession.interruptionNotification,
            object: nil, queue: .main
        ) { [weak self] notification in
            guard let self,
                  let info = notification.userInfo,
                  let typeValue = info[AVAudioSessionInterruptionTypeKey] as? UInt,
                  let type = AVAudioSession.InterruptionType(rawValue: typeValue) else { return }

            Task { @MainActor in
                // Skip interruptions caused by our own session category switch (mic toggle)
                if self.suppressInterruption {
                    print("[AudioReceiver] Ignoring interruption (mic toggle in progress)")
                    return
                }
                if type == .ended {
                    print("[AudioReceiver] Audio interruption ended — resuming")
                    try? AVAudioSession.sharedInstance().setActive(true)
                    if self.wasPlayingBeforeDisconnect {
                        self.wasPlayingBeforeDisconnect = false
                        self.start()
                    }
                } else if type == .began {
                    // While mic is transmitting, ignore interruptions —
                    // the session change itself can trigger delayed interruptions.
                    // Real interruptions (phone call) will stop AudioUnits at the OS level.
                    if self.isMicTransmitting {
                        print("[AudioReceiver] Ignoring interruption (mic is transmitting)")
                        return
                    }
                    print("[AudioReceiver] Audio interruption began")
                    if self.isPlaying {
                        self.wasPlayingBeforeDisconnect = true
                        self.stop()
                    }
                }
            }
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
        // isMicTransmitting is managed by toggleMic()/stop() only.
        // Don't overwrite from bridge — it can cause false negatives
        // during session transitions.
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
