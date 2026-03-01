//
//  PeerRelayManager.swift
//  SolunaReceiver
//
//  Discovery-first P2P audio relay using MultipeerConnectivity.
//
//  Flow:
//    1. User taps Play → scanForPeers(channel:) browses for 3 sec
//    2. If a peer on the same channel is found → connect, enter .peer mode
//       (audio arrives via MCSession → injectRawPacket, multicast disabled)
//    3. If no peer found → AudioReceiver starts multicast, then
//       becomeDirectRelay() advertises so others can connect
//
//  Roles:
//    .direct  — receiving from multicast + forwarding raw packets to peers
//    .peer    — receiving from a relay iPhone (multicast disabled)
//

import Foundation
import MultipeerConnectivity
import UIKit

// MARK: - Role

enum RelayRole: Equatable {
    case direct          // Normal UDP path + relay to connected peers
    case peer(String)    // Receiving from relay (stores relay display name)

    static func == (lhs: RelayRole, rhs: RelayRole) -> Bool {
        switch (lhs, rhs) {
        case (.direct, .direct): return true
        case (.peer(let a), .peer(let b)): return a == b
        default: return false
        }
    }
}

// MARK: - Manager

@MainActor
final class PeerRelayManager: NSObject, ObservableObject {

    static let shared = PeerRelayManager()

    // Bonjour service type (max 15 chars, DNS label rules)
    private let serviceType = "soluna-relay"

    private lazy var myPeerID = MCPeerID(displayName: UIDevice.current.name)

    @Published private(set) var role: RelayRole = .direct
    @Published private(set) var connectedPeerCount: Int = 0
    @Published private(set) var isScanning: Bool = false
    @Published private(set) var channel: String = ""

    private var session: MCSession?
    private var advertiser: MCNearbyServiceAdvertiser?
    private var browser: MCNearbyServiceBrowser?
    private var scanTimer: Timer?
    private var scanCompletion: ((Bool) -> Void)?

    private override init() { super.init() }

    // MARK: - Phase 1: Scan for nearby peers (before starting multicast)

    /// Browse for nearby peers on the given channel. Returns `true` if a peer was found and connected.
    func scanForPeers(channel: String, timeout: TimeInterval = 3) async -> Bool {
        self.channel = channel
        stop()

        return await withCheckedContinuation { continuation in
            let s = MCSession(peer: myPeerID,
                              securityIdentity: nil,
                              encryptionPreference: .none)
            s.delegate = self
            session = s

            isScanning = true

            scanCompletion = { found in
                continuation.resume(returning: found)
            }

            // Browse for peers advertising the same channel
            let br = MCNearbyServiceBrowser(peer: myPeerID, serviceType: serviceType)
            br.delegate = self
            br.startBrowsingForPeers()
            browser = br

            // If no relay found within timeout → not found
            scanTimer = Timer.scheduledTimer(withTimeInterval: timeout, repeats: false) { [weak self] _ in
                Task { @MainActor [weak self] in
                    guard let self, self.isScanning else { return }
                    self.isScanning = false
                    let cb = self.scanCompletion
                    self.scanCompletion = nil
                    cb?(false)
                }
            }
        }
    }

    // MARK: - Phase 2a: Become direct relay (multicast receiver + forwarding)

    /// Call after multicast reception is stable. Starts advertising on this channel
    /// and enables raw-packet forwarding to connected peers.
    func becomeDirectRelay() {
        role = .direct
        isScanning = false

        // Advertise with channel so new peers can find us
        let info: [String: String] = ["ch": channel]
        let adv = MCNearbyServiceAdvertiser(peer: myPeerID,
                                            discoveryInfo: info,
                                            serviceType: serviceType)
        adv.delegate = self
        adv.startAdvertisingPeer()
        advertiser = adv

        // Also keep browsing for new peers (they might arrive later)
        if browser == nil {
            let br = MCNearbyServiceBrowser(peer: myPeerID, serviceType: serviceType)
            br.delegate = self
            br.startBrowsingForPeers()
            browser = br
        }

        // Forward every received packet to connected peers
        SolunaAudioReceiver.sharedInstance().setRelayCallback { [weak self] data in
            Task { @MainActor [weak self] in
                self?.broadcastPacket(data)
            }
        }
    }

    // MARK: - Phase 2b: Enter peer mode (auto, called when scan finds a relay)

    private func enterPeerMode(relayName: String) {
        role = .peer(relayName)
        isScanning = false
        scanTimer?.invalidate()

        // Disable multicast — audio arrives only via injectRawPacket
        SolunaAudioReceiver.sharedInstance().networkDisabled = true

        let cb = scanCompletion
        scanCompletion = nil
        cb?(true)
    }

    // MARK: - Lifecycle

    func stop() {
        scanTimer?.invalidate(); scanTimer = nil
        advertiser?.stopAdvertisingPeer(); advertiser = nil
        browser?.stopBrowsingForPeers(); browser = nil
        session?.disconnect(); session = nil

        role = .direct
        connectedPeerCount = 0
        isScanning = false

        // Resume any pending scan
        let cb = scanCompletion
        scanCompletion = nil
        cb?(false)

        SolunaAudioReceiver.sharedInstance().setRelayCallback(nil)
        SolunaAudioReceiver.sharedInstance().networkDisabled = false
    }

    // MARK: - Broadcast (relay → peers)

    private func broadcastPacket(_ data: Data) {
        guard let session, !session.connectedPeers.isEmpty else { return }
        try? session.send(data, toPeers: session.connectedPeers, with: .unreliable)
    }
}

// MARK: - MCSessionDelegate

extension PeerRelayManager: MCSessionDelegate {

    nonisolated func session(_ session: MCSession,
                             peer peerID: MCPeerID,
                             didChange state: MCSessionState) {
        Task { @MainActor in
            self.connectedPeerCount = session.connectedPeers.count

            if state == .connected && self.isScanning {
                // Found and connected to a relay during scan → enter peer mode
                self.enterPeerMode(relayName: peerID.displayName)
            }

            if state == .notConnected {
                // If our relay disconnected → fall back to direct multicast
                if case .peer(let name) = self.role, name == peerID.displayName {
                    self.role = .direct
                    SolunaAudioReceiver.sharedInstance().networkDisabled = false
                    // Re-start as direct relay (multicast should still be running)
                    self.becomeDirectRelay()
                }
            }
        }
    }

    // Received audio data from relay peer
    nonisolated func session(_ session: MCSession,
                             didReceive data: Data,
                             fromPeer peerID: MCPeerID) {
        SolunaAudioReceiver.sharedInstance().injectRawPacket(data)
    }

    nonisolated func session(_ session: MCSession,
                             didReceive stream: InputStream,
                             withName streamName: String,
                             fromPeer peerID: MCPeerID) {}

    nonisolated func session(_ session: MCSession,
                             didStartReceivingResourceWithName resourceName: String,
                             fromPeer peerID: MCPeerID,
                             with progress: Progress) {}

    nonisolated func session(_ session: MCSession,
                             didFinishReceivingResourceWithName resourceName: String,
                             fromPeer peerID: MCPeerID,
                             at localURL: URL?,
                             withError error: Error?) {}
}

// MARK: - MCNearbyServiceAdvertiserDelegate

extension PeerRelayManager: MCNearbyServiceAdvertiserDelegate {

    nonisolated func advertiser(_ advertiser: MCNearbyServiceAdvertiser,
                                didReceiveInvitationFromPeer peerID: MCPeerID,
                                withContext context: Data?,
                                invitationHandler: @escaping (Bool, MCSession?) -> Void) {
        Task { @MainActor in
            // Auto-accept: no sender permission needed
            invitationHandler(true, self.session)
        }
    }
}

// MARK: - MCNearbyServiceBrowserDelegate

extension PeerRelayManager: MCNearbyServiceBrowserDelegate {

    nonisolated func browser(_ browser: MCNearbyServiceBrowser,
                             foundPeer peerID: MCPeerID,
                             withDiscoveryInfo info: [String: String]?) {
        Task { @MainActor in
            // Only connect to peers on the same channel
            guard let info, info["ch"] == self.channel else { return }
            guard let session = self.session else { return }

            if !session.connectedPeers.contains(peerID) {
                browser.invitePeer(peerID, to: session, withContext: nil, timeout: 10)
            }
        }
    }

    nonisolated func browser(_ browser: MCNearbyServiceBrowser,
                             lostPeer peerID: MCPeerID) {
        Task { @MainActor in
            if case .peer(let name) = self.role, name == peerID.displayName {
                self.role = .direct
                SolunaAudioReceiver.sharedInstance().networkDisabled = false
                self.becomeDirectRelay()
            }
        }
    }
}
