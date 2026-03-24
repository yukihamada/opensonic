import Foundation

// MARK: - PeerDiscovery Protocol

/// Protocol abstraction for peer discovery on local networks.
///
/// Two platform-specific implementations are provided:
/// - iOS: MultipeerConnectivity (MCNearbyServiceBrowser + MCNearbyServiceAdvertiser)
/// - macOS: Network.framework (NWBrowser + NWListener)
public protocol PeerDiscovery: AnyObject {
    /// Called when a peer is discovered. Parameters: (peerID, discoveryInfo).
    var onPeerFound: ((String, Data) -> Void)? { get set }

    /// Called when a previously discovered peer is lost.
    var onPeerLost: ((String) -> Void)? { get set }

    /// Start scanning for nearby peers.
    func startScan()

    /// Stop scanning for nearby peers.
    func stopScan()
}

// MARK: - Discovered Peer

/// Information about a discovered Soluna peer on the local network.
public struct DiscoveredPeer: Identifiable, Sendable {
    /// Unique identifier for the peer (display name or endpoint description).
    public let id: String

    /// Human-readable display name of the peer.
    public let displayName: String

    /// The channel the peer is broadcasting on, if known.
    public let channel: String?

    public init(id: String, displayName: String, channel: String? = nil) {
        self.id = id
        self.displayName = displayName
        self.channel = channel
    }
}
