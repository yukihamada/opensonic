#if os(iOS)
import Foundation
import MultipeerConnectivity
import UIKit

/// iOS peer discovery using MultipeerConnectivity framework.
///
/// Based on the iOS PeerRelayManager reference implementation.
/// Uses MCNearbyServiceBrowser to find peers and MCNearbyServiceAdvertiser
/// to make this device visible on the local network.
public final class MultipeerDiscovery: NSObject, PeerDiscovery, ObservableObject {

    // MARK: - PeerDiscovery Protocol

    public var onPeerFound: ((String, Data) -> Void)?
    public var onPeerLost: ((String) -> Void)?

    // MARK: - Published State

    @Published public private(set) var isScanning = false
    @Published public private(set) var discoveredPeers: [DiscoveredPeer] = []

    // MARK: - Configuration

    /// Bonjour service type (max 15 chars, DNS label rules).
    private let serviceType = "soluna-relay"

    /// The channel to filter peers by. Set before calling startScan().
    public var channel: String = ""

    // MARK: - Private

    private lazy var myPeerID = MCPeerID(displayName: UIDevice.current.name)
    private var session: MCSession?
    private var browser: MCNearbyServiceBrowser?
    private var advertiser: MCNearbyServiceAdvertiser?

    /// Active MCSession for data transfer with connected peers.
    public private(set) var activeSession: MCSession?

    // MARK: - Init

    public override init() {
        super.init()
    }

    // MARK: - PeerDiscovery

    public func startScan() {
        guard !isScanning else { return }

        let session = MCSession(peer: myPeerID, securityIdentity: nil, encryptionPreference: .none)
        session.delegate = self
        self.session = session
        self.activeSession = session

        let browser = MCNearbyServiceBrowser(peer: myPeerID, serviceType: serviceType)
        browser.delegate = self
        browser.startBrowsingForPeers()
        self.browser = browser

        isScanning = true
    }

    public func stopScan() {
        browser?.stopBrowsingForPeers()
        browser = nil
        advertiser?.stopAdvertisingPeer()
        advertiser = nil
        session?.disconnect()
        session = nil
        activeSession = nil

        isScanning = false
        discoveredPeers = []
    }

    // MARK: - Advertise

    /// Start advertising this device as a relay on the given channel.
    /// Peers scanning on the same channel will find this device.
    public func startAdvertising(channel: String) {
        self.channel = channel

        let info: [String: String] = ["ch": channel]
        let adv = MCNearbyServiceAdvertiser(peer: myPeerID, discoveryInfo: info, serviceType: serviceType)
        adv.delegate = self
        adv.startAdvertisingPeer()
        self.advertiser = adv
    }

    /// Stop advertising.
    public func stopAdvertising() {
        advertiser?.stopAdvertisingPeer()
        advertiser = nil
    }

    /// Send raw data to all connected peers (unreliable/UDP-like).
    public func broadcastData(_ data: Data) {
        guard let session, !session.connectedPeers.isEmpty else { return }
        try? session.send(data, toPeers: session.connectedPeers, with: .unreliable)
    }
}

// MARK: - MCSessionDelegate

extension MultipeerDiscovery: MCSessionDelegate {

    public func session(_ session: MCSession, peer peerID: MCPeerID, didChange state: MCSessionState) {
        // State changes are handled by the higher-level SolunaClient
    }

    public func session(_ session: MCSession, didReceive data: Data, fromPeer peerID: MCPeerID) {
        // Forward received data to the onPeerFound callback as a data event.
        // In practice, received audio packets are injected into the audio pipeline.
        onPeerFound?(peerID.displayName, data)
    }

    public func session(_ session: MCSession, didReceive stream: InputStream,
                        withName streamName: String, fromPeer peerID: MCPeerID) {}

    public func session(_ session: MCSession, didStartReceivingResourceWithName resourceName: String,
                        fromPeer peerID: MCPeerID, with progress: Progress) {}

    public func session(_ session: MCSession, didFinishReceivingResourceWithName resourceName: String,
                        fromPeer peerID: MCPeerID, at localURL: URL?, withError error: Error?) {}
}

// MARK: - MCNearbyServiceBrowserDelegate

extension MultipeerDiscovery: MCNearbyServiceBrowserDelegate {

    public func browser(_ browser: MCNearbyServiceBrowser, foundPeer peerID: MCPeerID,
                        withDiscoveryInfo info: [String: String]?) {
        let peerChannel = info?["ch"]

        // Only connect to peers on the same channel (if channel is set)
        if !channel.isEmpty, peerChannel != channel { return }

        let peer = DiscoveredPeer(id: peerID.displayName, displayName: peerID.displayName, channel: peerChannel)

        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            if !self.discoveredPeers.contains(where: { $0.id == peer.id }) {
                self.discoveredPeers.append(peer)
            }
        }

        // Auto-invite the peer to our session
        if let session, !session.connectedPeers.contains(peerID) {
            browser.invitePeer(peerID, to: session, withContext: nil, timeout: 10)
        }

        // Encode discovery info as Data for the callback
        let infoData = (try? JSONSerialization.data(withJSONObject: info ?? [:], options: [])) ?? Data()
        onPeerFound?(peerID.displayName, infoData)
    }

    public func browser(_ browser: MCNearbyServiceBrowser, lostPeer peerID: MCPeerID) {
        DispatchQueue.main.async { [weak self] in
            self?.discoveredPeers.removeAll { $0.id == peerID.displayName }
        }
        onPeerLost?(peerID.displayName)
    }
}

// MARK: - MCNearbyServiceAdvertiserDelegate

extension MultipeerDiscovery: MCNearbyServiceAdvertiserDelegate {

    public func advertiser(_ advertiser: MCNearbyServiceAdvertiser,
                           didReceiveInvitationFromPeer peerID: MCPeerID,
                           withContext context: Data?,
                           invitationHandler: @escaping (Bool, MCSession?) -> Void) {
        // Auto-accept all invitations
        invitationHandler(true, session)
    }
}

#endif // os(iOS)
