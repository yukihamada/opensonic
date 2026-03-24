#if os(macOS)
import Foundation
import Network

/// macOS peer discovery using Network.framework (NWBrowser + NWListener).
///
/// Based on the Mac PeerRelayManager reference implementation.
/// Uses NWBrowser for Bonjour discovery and NWListener for receiving
/// UDP packets from peers on port 5099.
public final class BonjourDiscovery: PeerDiscovery, ObservableObject {

    // MARK: - PeerDiscovery Protocol

    public var onPeerFound: ((String, Data) -> Void)?
    public var onPeerLost: ((String) -> Void)?

    // MARK: - Published State

    @Published public private(set) var isScanning = false
    @Published public private(set) var discoveredPeers: [DiscoveredPeer] = []

    // MARK: - Configuration

    private let serviceType = "_soluna-relay._udp."
    private let domain = "local."
    private let relayPort: UInt16 = 5099

    /// The channel to filter peers by. Set before calling startScan().
    public var channel: String = ""

    // MARK: - Private

    private var browser: NWBrowser?
    private var listener: NWListener?
    private var peerConnections: [NWEndpoint: NWConnection] = [:]
    private let queue = DispatchQueue(label: "soluna.bonjour.discovery", qos: .userInteractive)

    // MARK: - Init

    public init() {}

    // MARK: - PeerDiscovery

    public func startScan() {
        guard !isScanning else { return }
        isScanning = true

        let params = NWParameters.udp
        let browser = NWBrowser(for: .bonjour(type: serviceType, domain: domain), using: params)

        browser.browseResultsChangedHandler = { [weak self] results, changes in
            guard let self else { return }
            for change in changes {
                switch change {
                case .added(let result):
                    if case .service(let name, _, _, _) = result.endpoint {
                        let peer = DiscoveredPeer(id: name, displayName: name, channel: nil)
                        DispatchQueue.main.async {
                            if !self.discoveredPeers.contains(where: { $0.id == name }) {
                                self.discoveredPeers.append(peer)
                            }
                        }
                        self.onPeerFound?(name, Data())
                    }
                case .removed(let result):
                    if case .service(let name, _, _, _) = result.endpoint {
                        DispatchQueue.main.async {
                            self.discoveredPeers.removeAll { $0.id == name }
                        }
                        self.onPeerLost?(name)
                    }
                default:
                    break
                }
            }
        }

        browser.stateUpdateHandler = { [weak self] state in
            if case .failed = state {
                DispatchQueue.main.async {
                    self?.isScanning = false
                }
            }
        }

        browser.start(queue: queue)
        self.browser = browser
    }

    public func stopScan() {
        browser?.cancel()
        browser = nil
        stopListening()

        isScanning = false
        discoveredPeers = []
    }

    // MARK: - Listener (Direct Relay Mode)

    /// Start listening on the relay port and advertise via Bonjour.
    /// Used when this device is the direct relay forwarding packets to peers.
    public func startListening() {
        do {
            let params = NWParameters.udp
            listener = try NWListener(using: params, on: NWEndpoint.Port(integerLiteral: relayPort))
        } catch {
            print("[SolunaSDK] Failed to create NWListener: \(error)")
            return
        }

        let name = Host.current().localizedName ?? "Mac"
        listener?.service = NWListener.Service(name: "Soluna-\(name)", type: serviceType, domain: domain)

        listener?.newConnectionHandler = { [weak self] conn in
            self?.handleIncomingPeer(conn)
        }

        listener?.stateUpdateHandler = { [weak self] state in
            if case .failed = state {
                self?.stopListening()
            }
        }

        listener?.start(queue: queue)
    }

    /// Stop the listener and disconnect all peers.
    public func stopListening() {
        listener?.cancel()
        listener = nil

        for (_, conn) in peerConnections {
            conn.cancel()
        }
        peerConnections.removeAll()
    }

    /// Send data to all connected peers.
    public func broadcastData(_ data: Data) {
        for (_, conn) in peerConnections {
            conn.send(content: data, completion: .contentProcessed({ _ in }))
        }
    }

    /// Connect to a discovered peer endpoint for receiving relay packets.
    public func connectToPeer(endpoint: NWEndpoint, onData: @escaping (Data) -> Void) {
        let params = NWParameters.udp
        let conn = NWConnection(to: endpoint, using: params)

        conn.stateUpdateHandler = { state in
            switch state {
            case .ready:
                // Send hello to register with the relay
                conn.send(content: "hello".data(using: .utf8)!, completion: .contentProcessed({ _ in }))
            case .failed, .cancelled:
                break
            default:
                break
            }
        }

        conn.start(queue: queue)

        // Receive relay packets
        func receiveLoop() {
            conn.receiveMessage { data, _, _, error in
                guard error == nil else { return }
                if let data { onData(data) }
                receiveLoop()
            }
        }
        receiveLoop()
    }

    // MARK: - Private

    private func handleIncomingPeer(_ conn: NWConnection) {
        conn.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            switch state {
            case .ready:
                self.peerConnections[conn.endpoint] = conn
            case .failed, .cancelled:
                self.peerConnections.removeValue(forKey: conn.endpoint)
            default:
                break
            }
        }
        conn.start(queue: queue)

        // Keep connection alive by reading
        func receiveLoop() {
            conn.receiveMessage { [weak self] _, _, _, error in
                guard self != nil, error == nil else { return }
                receiveLoop()
            }
        }
        receiveLoop()
    }
}

#endif // os(macOS)
