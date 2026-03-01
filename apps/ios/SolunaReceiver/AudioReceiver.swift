//
//  AudioReceiver.swift
//  SolunaReceiver
//
//  Swift wrapper for the Objective-C++ audio receiver bridge
//

import Foundation
import Combine

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

    init() {
        receiver = SolunaAudioReceiver.sharedInstance()
        delegateHandler = DelegateHandler()
        delegateHandler.audioReceiver = self
        receiver.delegate = delegateHandler
    }

    /// Start receiving audio (and P2P relay discovery)
    func start() {
        errorMessage = nil
        _ = receiver.start()
        PeerRelayManager.shared.start()
        // Evaluate relay role after 6 seconds of data
        Task {
            try? await Task.sleep(nanoseconds: 6_000_000_000)
            await evaluateRelayRole()
        }
    }

    /// Stop receiving audio
    func stop() {
        receiver.stop()
        PeerRelayManager.shared.stop()
    }

    /// Toggle play/stop
    func toggle() {
        if isPlaying {
            stop()
        } else {
            start()
        }
    }

    // MARK: - Relay role evaluation

    /// Called 6 sec after start; re-evaluates every stats update if needed.
    private func evaluateRelayRole() async {
        let relay = PeerRelayManager.shared
        let total = packetsReceived + packetsDropped
        guard total > 0 else { return }

        let lossRate = Double(packetsDropped) / Double(total)
        let peers    = relay.connectedPeerCount

        switch relay.role {
        case .direct:
            if lossRate < 0.05, peers > 0 {
                // Good connection + peers nearby → become relay
                relay.promoteToRelay()
            } else if lossRate > 0.30, peers > 0 {
                // Bad connection + peers nearby → let a peer relay feed us
                // We'll receive via MCSession (already joined) — wait for packets
                // promoteToPeer is called by PeerRelayManager when it gets packets
            }
        case .relay:
            if lossRate > 0.20 {
                // Our own connection degraded; step down
                relay.demoteToDirectWithMessage(nil)
            }
        case .peer:
            break  // Managed by MCSessionDelegate
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
