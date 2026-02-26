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

    /// Start receiving audio
    func start() {
        errorMessage = nil
        _ = receiver.start()
    }

    /// Stop receiving audio
    func stop() {
        receiver.stop()
    }

    /// Toggle play/stop
    func toggle() {
        if isPlaying {
            stop()
        } else {
            start()
        }
    }

    // MARK: - Internal delegate handling

    fileprivate func handleStateChange(_ newState: SolunaReceiverState) {
        self.state = State(from: newState)
    }

    fileprivate func handleStatsUpdate(_ stats: SolunaReceiverStats) {
        self.packetsReceived = stats.packetsReceived
        self.packetsDropped = stats.packetsDropped
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
