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
    private var interruptionObserver: Any?

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
        }
    }

    /// Stop receiving audio
    func stop() {
        state = .stopped
        receiver.stop()
        PeerRelayManager.shared.stop()
        UIApplication.shared.isIdleTimerDisabled = false
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }

    /// Toggle play/stop
    func toggle() {
        if isPlaying {
            stop()
        } else {
            start()
        }
    }

    // MARK: - Auto-Reconnect

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
                if type == .ended {
                    print("[AudioReceiver] Audio interruption ended — resuming")
                    try? AVAudioSession.sharedInstance().setActive(true)
                    if self.wasPlayingBeforeDisconnect {
                        self.wasPlayingBeforeDisconnect = false
                        self.start()
                    }
                } else if type == .began {
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
