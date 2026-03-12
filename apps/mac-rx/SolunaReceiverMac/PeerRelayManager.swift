//
//  PeerRelayManager.swift
//  SolunaReceiverMac
//
//  Mac P2P relay using Bonjour discovery + UDP unicast.
//  One Mac receives multicast RTP and relays packets to peers via UDP.
//  Peers inject received packets into the audio pipeline via injectRawPacket:.
//

import Foundation
import Network

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

// MARK: - Manager

@MainActor
final class PeerRelayManager: NSObject, ObservableObject {

    static let shared = PeerRelayManager()

    @Published private(set) var role: RelayRole = .direct
    @Published private(set) var connectedPeerCount: Int = 0
    @Published private(set) var isScanning: Bool = false
    @Published private(set) var channel: String = ""

    private let serviceType = "_soluna-relay._udp."
    private let domain = "local."
    private let relayPort: UInt16 = 5099

    // Direct relay mode: advertise + forward packets to peers
    private var advertiser: NetServicePublisher?
    private var listener: NWListener?
    private var peerEndpoints: [NWEndpoint: NWConnection] = [:]

    // Peer mode: browse + receive packets from relay
    private var browser: NWBrowser?
    private var relayConnection: NWConnection?

    private let queue = DispatchQueue(label: "soluna.relay", qos: .userInteractive)

    private override init() { super.init() }

    // MARK: - Public API

    /// Scan for a relay peer on the LAN. Returns true if one is found.
    func scanForPeers(channel: String, timeout: TimeInterval = 3) async -> Bool {
        self.channel = channel
        isScanning = true

        let found = await withCheckedContinuation { (cont: CheckedContinuation<Bool, Never>) in
            let lock = NSLock()
            var _resumed = false
            func tryResume(_ value: Bool, cancel: NWBrowser?) -> Bool {
                lock.lock()
                defer { lock.unlock() }
                guard !_resumed else { return false }
                _resumed = true
                cancel?.cancel()
                cont.resume(returning: value)
                return true
            }

            let params = NWParameters.udp
            let browser = NWBrowser(for: .bonjour(type: serviceType, domain: domain), using: params)

            browser.browseResultsChangedHandler = { [weak self] results, _ in
                for result in results {
                    if case .service(let name, _, _, _) = result.endpoint {
                        if tryResume(true, cancel: browser) {
                            self?.connectToPeer(endpoint: result.endpoint, name: name)
                        }
                        return
                    }
                }
            }

            browser.stateUpdateHandler = { state in
                if case .failed = state {
                    _ = tryResume(false, cancel: browser)
                }
            }

            browser.start(queue: self.queue)

            // Timeout
            self.queue.asyncAfter(deadline: .now() + timeout) {
                _ = tryResume(false, cancel: browser)
            }
        }

        isScanning = false
        return found
    }

    /// Start direct relay mode: receive multicast RTP and forward to peers.
    func becomeDirectRelay() {
        stopInternal()
        role = .direct

        // Start NWListener for incoming peer connections
        do {
            let params = NWParameters.udp
            listener = try NWListener(using: params, on: NWEndpoint.Port(integerLiteral: relayPort))
        } catch {
            return
        }

        // Advertise via Bonjour
        let name = Host.current().localizedName ?? "Mac"
        listener?.service = NWListener.Service(name: "Soluna-\(name)", type: serviceType, domain: domain)

        listener?.newConnectionHandler = { [weak self] conn in
            Task { @MainActor in
                self?.handleIncomingPeer(conn)
            }
        }

        listener?.stateUpdateHandler = { [weak self] state in
            if case .failed = state {
                Task { @MainActor in self?.stop() }
            }
        }

        listener?.start(queue: queue)

        // Wire relay callback: forward every RTP packet to connected peers
        let receiver = SolunaAudioReceiver.sharedInstance()
        receiver.setRelayCallback { [weak self] data in
            self?.forwardPacket(data)
        }
    }

    /// Stop all relay activity
    func stop() {
        stopInternal()
        role = .direct
        connectedPeerCount = 0
        isScanning = false
    }

    // MARK: - Direct relay internals

    private func handleIncomingPeer(_ conn: NWConnection) {
        conn.stateUpdateHandler = { [weak self] state in
            Task { @MainActor in
                guard let self else { return }
                switch state {
                case .ready:
                    self.peerEndpoints[conn.endpoint] = conn
                    self.connectedPeerCount = self.peerEndpoints.count
                case .failed, .cancelled:
                    self.peerEndpoints.removeValue(forKey: conn.endpoint)
                    self.connectedPeerCount = self.peerEndpoints.count
                default:
                    break
                }
            }
        }
        conn.start(queue: queue)

        // Start receiving (keep connection alive)
        receiveFromPeer(conn)
    }

    private nonisolated func receiveFromPeer(_ conn: NWConnection) {
        conn.receiveMessage { [weak self] data, _, _, error in
            if error != nil { return }
            // Peers may send heartbeat; ignore data
            self?.receiveFromPeer(conn)
        }
    }

    private func forwardPacket(_ data: Data) {
        // Send to all connected peers via UDP
        for (_, conn) in peerEndpoints {
            conn.send(content: data, completion: .contentProcessed({ _ in }))
        }
    }

    // MARK: - Peer mode internals

    private nonisolated func connectToPeer(endpoint: NWEndpoint, name: String) {
        Task { @MainActor in
            self.stopInternal()
            self.role = .peer(name)

            // Disable network multicast on receiver — only get packets via relay
            let receiver = SolunaAudioReceiver.sharedInstance()
            receiver.relayNetworkDisabled = true

            let params = NWParameters.udp
            let conn = NWConnection(to: endpoint, using: params)

            conn.stateUpdateHandler = { [weak self] state in
                Task { @MainActor in
                    guard let self else { return }
                    switch state {
                    case .ready:
                        self.connectedPeerCount = 1
                        // Send a hello to register with the relay
                        conn.send(content: "hello".data(using: .utf8)!, completion: .contentProcessed({ _ in }))
                        self.receiveRelayPackets(conn)
                    case .failed, .cancelled:
                        self.role = .direct
                        self.connectedPeerCount = 0
                        receiver.relayNetworkDisabled = false
                    default:
                        break
                    }
                }
            }

            self.relayConnection = conn
            conn.start(queue: self.queue)
        }
    }

    private nonisolated func receiveRelayPackets(_ conn: NWConnection) {
        conn.receiveMessage { [weak self] data, _, _, error in
            guard let self, error == nil else { return }
            if let data {
                let receiver = SolunaAudioReceiver.sharedInstance()
                receiver.injectRawPacket(data)
            }
            self.receiveRelayPackets(conn)
        }
    }

    // MARK: - Cleanup

    private func stopInternal() {
        // Stop advertising/listening
        listener?.cancel()
        listener = nil

        // Disconnect all peers
        for (_, conn) in peerEndpoints {
            conn.cancel()
        }
        peerEndpoints.removeAll()

        // Stop browsing
        browser?.cancel()
        browser = nil

        // Stop relay connection
        relayConnection?.cancel()
        relayConnection = nil

        // Clear relay callback
        let receiver = SolunaAudioReceiver.sharedInstance()
        receiver.setRelayCallback(nil)
        receiver.relayNetworkDisabled = false
    }
}

// MARK: - NetServicePublisher helper

private class NetServicePublisher: NSObject, NetServiceDelegate {
    let service: NetService

    init(name: String, type: String, domain: String, port: Int32) {
        service = NetService(domain: domain, type: type, name: name, port: port)
        super.init()
        service.delegate = self
    }

    func publish() {
        service.publish()
    }

    func stop() {
        service.stop()
    }
}
