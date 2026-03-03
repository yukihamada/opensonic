//
//  PeerRelayManager.swift
//  SolunaReceiverMac
//
//  macOS stub for PeerRelayManager.
//  MultipeerConnectivity is not available on macOS, so this provides the same
//  public API as the iOS version but with no-op implementations.
//  On macOS, audio reception is always via direct multicast.
//

import Foundation

// MARK: - Role

enum RelayRole: Equatable {
    case direct
    case peer(String)

    static func == (lhs: RelayRole, rhs: RelayRole) -> Bool {
        switch (lhs, rhs) {
        case (.direct, .direct): return true
        case (.peer(let a), .peer(let b)): return a == b
        default: return false
        }
    }
}

// MARK: - Manager (macOS stub)

@MainActor
final class PeerRelayManager: NSObject, ObservableObject {

    static let shared = PeerRelayManager()

    @Published private(set) var role: RelayRole = .direct
    @Published private(set) var connectedPeerCount: Int = 0
    @Published private(set) var isScanning: Bool = false
    @Published private(set) var channel: String = ""

    private override init() { super.init() }

    /// No-op on macOS — MultipeerConnectivity is not available.
    /// Always returns false (no peer found).
    func scanForPeers(channel: String, timeout: TimeInterval = 3) async -> Bool {
        self.channel = channel
        return false
    }

    /// No-op on macOS.
    func becomeDirectRelay() {
        role = .direct
    }

    /// No-op on macOS.
    func stop() {
        role = .direct
        connectedPeerCount = 0
        isScanning = false
    }
}
