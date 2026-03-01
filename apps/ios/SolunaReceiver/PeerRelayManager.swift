//
//  PeerRelayManager.swift
//  SolunaReceiver
//
//  P2P audio relay using MultipeerConnectivity.
//
//  Roles:
//    .direct — normal UDP multicast reception (no peers)
//    .relay  — receiving from solunad AND forwarding raw packets to peers nearby
//    .peer   — receiving audio from a relay iPhone (solunad unreachable / high loss)
//
//  Auto-role selection (triggered by AudioReceiver every 5 sec):
//    • loss < 5 %  AND peers exist  → become relay
//    • loss > 30 % AND relay found  → become peer
//    • relay lost                   → fall back to .direct
//

import Foundation
import MultipeerConnectivity
import UIKit

// MARK: - Role

enum RelayRole {
    case direct          // Normal UDP path
    case relay           // Forwarding to peers
    case peer(String)    // Receiving from relay (stores relay display name)
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

    // Raw-packet injection sink — set by AudioReceiver when we switch to peer mode
    var onRelayPacket: ((Data) -> Void)?

    private var session: MCSession?
    private var advertiser: MCNearbyServiceAdvertiser?
    private var browser: MCNearbyServiceBrowser?

    private override init() { super.init() }

    // MARK: - Lifecycle

    func start() {
        let s = MCSession(peer: myPeerID,
                          securityIdentity: nil,
                          encryptionPreference: .none)
        s.delegate = self
        session = s

        let adv = MCNearbyServiceAdvertiser(peer: myPeerID,
                                            discoveryInfo: nil,
                                            serviceType: serviceType)
        adv.delegate = self
        adv.startAdvertisingPeer()
        advertiser = adv

        let br = MCNearbyServiceBrowser(peer: myPeerID, serviceType: serviceType)
        br.delegate = self
        br.startBrowsingForPeers()
        browser = br
    }

    func stop() {
        advertiser?.stopAdvertisingPeer()
        browser?.stopBrowsingForPeers()
        session?.disconnect()
        session    = nil
        advertiser = nil
        browser    = nil
        role = .direct
        connectedPeerCount = 0
    }

    // MARK: - Role control (called from AudioReceiver)

    /// Promote to relay: activate raw-packet forwarding on the bridge
    func promoteToRelay() {
        guard case .direct = role else { return }
        role = .relay
        SolunaAudioReceiver.sharedInstance().setRelayCallback { [weak self] data in
            Task { @MainActor [weak self] in
                self?.broadcastPacket(data)
            }
        }
    }

    /// Promote to peer mode (receiving from relayName)
    func promoteToPeer(relayName: String) {
        role = .peer(relayName)
        // Stop own raw-packet forwarding
        SolunaAudioReceiver.sharedInstance().setRelayCallback(nil)
    }

    /// Fall back to direct UDP reception
    func demoteToDirectWithMessage(_ message: String?) {
        role = .direct
        SolunaAudioReceiver.sharedInstance().setRelayCallback(nil)
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

            if state == .notConnected {
                // If our relay peer disconnected, fall back
                if case .peer(let name) = self.role, name == peerID.displayName {
                    self.demoteToDirectWithMessage("リレー接続が切れました。直接受信に切り替えます。")
                }
            }
        }
    }

    // Received a relay packet from a peer (we are in .peer mode)
    nonisolated func session(_ session: MCSession,
                             didReceive data: Data,
                             fromPeer peerID: MCPeerID) {
        Task { @MainActor in
            guard case .peer = self.role else { return }
            SolunaAudioReceiver.sharedInstance().injectRawPacket(data)
        }
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
            // Accept: either we are a relay (add this device as listener),
            // or we allow them to connect so we can become their peer
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
            guard let session = self.session else { return }
            // Invite if we haven't seen this peer yet
            if !session.connectedPeers.contains(peerID) {
                browser.invitePeer(peerID, to: session, withContext: nil, timeout: 30)
            }
        }
    }

    nonisolated func browser(_ browser: MCNearbyServiceBrowser,
                             lostPeer peerID: MCPeerID) {
        Task { @MainActor in
            if case .peer(let name) = self.role, name == peerID.displayName {
                self.demoteToDirectWithMessage(nil)
            }
        }
    }
}
