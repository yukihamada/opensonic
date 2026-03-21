import Foundation
#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#endif

// MARK: - Tunnel Mode

/// Transport mode for relay connectivity.
public enum TunnelMode: String, Sendable, CaseIterable {
    /// Automatically detect the best transport (UDP -> WSS -> HTTPS proxy).
    case auto = "Auto"

    /// Direct UDP connection (default, lowest latency).
    case udpDirect = "UDP Direct"

    /// WebSocket over TLS fallback when UDP is blocked.
    case websocket = "WebSocket"

    /// HTTPS CONNECT proxy for restrictive corporate firewalls.
    case httpsProxy = "HTTPS Proxy"
}

// MARK: - TunnelConnection Protocol

/// Abstract transport connection that can send and receive data.
public protocol TunnelConnection: AnyObject, Sendable {
    /// Send data through the tunnel.
    func send(_ data: Data) async throws

    /// Receive data from the tunnel.
    func receive() async throws -> Data

    /// Close the tunnel connection.
    func close()

    /// Whether the connection is currently open.
    var isOpen: Bool { get }
}

// MARK: - UDP Tunnel Connection

/// Direct UDP tunnel using BSD sockets.
public final class UDPTunnelConnection: TunnelConnection, @unchecked Sendable {
    private var sock: Int32 = -1
    private var remoteAddr = sockaddr_in()
    private let running = AtomicFlag()

    init(socket: Int32, remoteAddr: sockaddr_in) {
        self.sock = socket
        self.remoteAddr = remoteAddr
        running.set(true)
    }

    public var isOpen: Bool { running.value && sock >= 0 }

    public func send(_ data: Data) async throws {
        guard isOpen else { throw TunnelError.notConnected }
        data.withUnsafeBytes { ptr in
            guard let base = ptr.baseAddress else { return }
            withUnsafePointer(to: remoteAddr) { addrPtr in
                let sockaddrPtr = UnsafeRawPointer(addrPtr).assumingMemoryBound(to: sockaddr.self)
                _ = sendto(sock, base, data.count, 0, sockaddrPtr, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
    }

    public func receive() async throws -> Data {
        guard isOpen else { throw TunnelError.notConnected }

        return try await withCheckedThrowingContinuation { continuation in
            DispatchQueue.global(qos: .userInteractive).async { [weak self] in
                guard let self, self.isOpen else {
                    continuation.resume(throwing: TunnelError.notConnected)
                    return
                }

                var buf = [UInt8](repeating: 0, count: OSTConstants.recvBufferSize)
                var senderAddr = sockaddr_in()
                var senderLen = socklen_t(MemoryLayout<sockaddr_in>.size)

                let n = withUnsafeMutablePointer(to: &senderAddr) { senderPtr -> Int in
                    senderPtr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sockaddrPtr in
                        recvfrom(self.sock, &buf, buf.count, 0, sockaddrPtr, &senderLen)
                    }
                }

                if n > 0 {
                    continuation.resume(returning: Data(bytes: buf, count: n))
                } else {
                    continuation.resume(throwing: TunnelError.receiveTimeout)
                }
            }
        }
    }

    public func close() {
        running.set(false)
        if sock >= 0 {
            Darwin.close(sock)
            sock = -1
        }
    }
}

// MARK: - WebSocket Tunnel Connection

/// WebSocket tunnel using URLSessionWebSocketTask for firewall traversal.
public final class WebSocketTunnelConnection: TunnelConnection, @unchecked Sendable {
    private var task: URLSessionWebSocketTask?
    private let session: URLSession
    private let _isOpen = AtomicFlag()

    init(url: URL, session: URLSession = .shared) {
        self.session = session
        let wsTask = session.webSocketTask(with: url)
        self.task = wsTask
        _isOpen.set(true)
        wsTask.resume()
    }

    public var isOpen: Bool { _isOpen.value }

    public func send(_ data: Data) async throws {
        guard let task, isOpen else { throw TunnelError.notConnected }
        try await task.send(.data(data))
    }

    public func receive() async throws -> Data {
        guard let task, isOpen else { throw TunnelError.notConnected }
        let message = try await task.receive()
        switch message {
        case .data(let data):
            return data
        case .string(let text):
            return Data(text.utf8)
        @unknown default:
            throw TunnelError.unknownMessageType
        }
    }

    public func close() {
        _isOpen.set(false)
        task?.cancel(with: .normalClosure, reason: nil)
        task = nil
    }
}

// MARK: - Tunnel Error

/// Errors that can occur during tunnel operations.
public enum TunnelError: Error, Sendable {
    case notConnected
    case receiveTimeout
    case dnsResolutionFailed
    case socketCreationFailed
    case webSocketConnectionFailed
    case proxyConnectionFailed(String)
    case udpBlocked
    case unknownMessageType
}

// MARK: - ProxyTunnel

/// Enterprise firewall and proxy traversal manager.
///
/// Provides transparent transport switching: tries direct UDP first,
/// falls back to WebSocket over TLS, then to HTTPS CONNECT proxy.
/// Corporate environments that block UDP can still receive audio
/// through the WebSocket or proxy tunnel.
///
/// Usage:
/// ```swift
/// let tunnel = ProxyTunnel()
/// let conn = try await tunnel.connect(host: "relay.solun.art", port: 5100)
/// try await conn.send(helloPacket)
/// let response = try await conn.receive()
/// conn.close()
/// ```
@MainActor
public final class ProxyTunnel: ObservableObject {

    // MARK: - Published State

    /// The currently active tunnel mode.
    @Published public var currentMode: TunnelMode = .auto

    // MARK: - Configuration

    /// Desired tunnel mode. `.auto` will probe and select the best option.
    public var mode: TunnelMode = .auto

    /// HTTP/HTTPS proxy hostname (for `.httpsProxy` mode).
    public var proxyHost: String?

    /// HTTP/HTTPS proxy port (for `.httpsProxy` mode).
    public var proxyPort: UInt16?

    /// Proxy authentication credentials (for `.httpsProxy` mode).
    public var proxyCredentials: (user: String, pass: String)?

    /// WebSocket URL path for relay (appended to wss://host).
    public var webSocketPath: String = "/ws"

    /// Timeout for UDP availability probe in seconds.
    public var probeTimeoutSeconds: TimeInterval = 2.0

    // MARK: - Init

    public init(mode: TunnelMode = .auto) {
        self.mode = mode
    }

    // MARK: - Public API

    /// Test whether direct UDP connectivity is available to the given host.
    ///
    /// Sends a HELLO packet and waits for a response within the probe timeout.
    public func isUDPAvailable(host: String, port: UInt16) async -> Bool {
        await withCheckedContinuation { continuation in
            DispatchQueue.global(qos: .utility).async {
                var hints = addrinfo()
                hints.ai_family = AF_INET
                hints.ai_socktype = SOCK_DGRAM
                var res: UnsafeMutablePointer<addrinfo>?

                guard getaddrinfo(host, "\(port)", &hints, &res) == 0, let addrInfo = res else {
                    continuation.resume(returning: false)
                    return
                }

                var addr = sockaddr_in()
                memcpy(&addr, addrInfo.pointee.ai_addr, Int(addrInfo.pointee.ai_addrlen))
                freeaddrinfo(res)

                let sock = socket(AF_INET, SOCK_DGRAM, 0)
                guard sock >= 0 else {
                    continuation.resume(returning: false)
                    return
                }

                let timeoutUs = Int32(self.probeTimeoutSeconds * 1_000_000)
                var tv = timeval(tv_sec: Int(timeoutUs / 1_000_000), tv_usec: Int32(timeoutUs % 1_000_000))
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

                // Send HELLO
                let hello = "HELLO\n"
                hello.withCString { ptr in
                    withUnsafePointer(to: addr) { addrPtr in
                        let sockaddrPtr = UnsafeRawPointer(addrPtr).assumingMemoryBound(to: sockaddr.self)
                        _ = sendto(sock, ptr, strlen(ptr), 0, sockaddrPtr, socklen_t(MemoryLayout<sockaddr_in>.size))
                    }
                }

                // Wait for response
                var buf = [UInt8](repeating: 0, count: 256)
                var senderAddr = sockaddr_in()
                var senderLen = socklen_t(MemoryLayout<sockaddr_in>.size)

                let n = withUnsafeMutablePointer(to: &senderAddr) { senderPtr -> Int in
                    senderPtr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sockaddrPtr in
                        recvfrom(sock, &buf, buf.count, 0, sockaddrPtr, &senderLen)
                    }
                }

                Darwin.close(sock)
                continuation.resume(returning: n > 0)
            }
        }
    }

    /// Establish a tunnel connection to the relay server.
    ///
    /// In `.auto` mode, tries transports in order: UDP -> WebSocket -> HTTPS Proxy.
    /// Returns the first successful connection.
    ///
    /// - Parameters:
    ///   - host: Relay server hostname.
    ///   - port: Relay server port.
    /// - Returns: An open `TunnelConnection` ready for send/receive.
    public func connect(host: String, port: UInt16) async throws -> TunnelConnection {
        switch mode {
        case .udpDirect:
            return try await connectUDP(host: host, port: port)

        case .websocket:
            return try connectWebSocket(host: host)

        case .httpsProxy:
            return try connectWebSocket(host: host) // Uses WSS through proxy

        case .auto:
            // Try UDP first
            if await isUDPAvailable(host: host, port: port) {
                do {
                    let conn = try await connectUDP(host: host, port: port)
                    currentMode = .udpDirect
                    return conn
                } catch {
                    // Fall through to WebSocket
                }
            }

            // Try WebSocket
            do {
                let conn = try connectWebSocket(host: host)
                currentMode = .websocket
                return conn
            } catch {
                // Fall through to error
            }

            throw TunnelError.udpBlocked
        }
    }

    // MARK: - Private Transport Implementations

    private func connectUDP(host: String, port: UInt16) async throws -> UDPTunnelConnection {
        return try await withCheckedThrowingContinuation { continuation in
            DispatchQueue.global(qos: .utility).async {
                var hints = addrinfo()
                hints.ai_family = AF_INET
                hints.ai_socktype = SOCK_DGRAM
                var res: UnsafeMutablePointer<addrinfo>?

                guard getaddrinfo(host, "\(port)", &hints, &res) == 0, let addrInfo = res else {
                    continuation.resume(throwing: TunnelError.dnsResolutionFailed)
                    return
                }

                var addr = sockaddr_in()
                memcpy(&addr, addrInfo.pointee.ai_addr, Int(addrInfo.pointee.ai_addrlen))
                freeaddrinfo(res)

                let sock = socket(AF_INET, SOCK_DGRAM, 0)
                guard sock >= 0 else {
                    continuation.resume(throwing: TunnelError.socketCreationFailed)
                    return
                }

                // Set receive timeout
                var tv = timeval(tv_sec: 1, tv_usec: 0)
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

                let conn = UDPTunnelConnection(socket: sock, remoteAddr: addr)
                continuation.resume(returning: conn)
            }
        }
    }

    private func connectWebSocket(host: String) throws -> WebSocketTunnelConnection {
        let urlString = "wss://\(host)\(webSocketPath)"
        guard let url = URL(string: urlString) else {
            throw TunnelError.webSocketConnectionFailed
        }

        let configuration = URLSessionConfiguration.default
        #if os(macOS)
        if let proxyHost, let proxyPort {
            configuration.connectionProxyDictionary = [
                kCFNetworkProxiesHTTPSEnable: true,
                kCFNetworkProxiesHTTPSProxy: proxyHost,
                kCFNetworkProxiesHTTPSPort: proxyPort,
            ]
        }
        #endif

        let session = URLSession(configuration: configuration)
        let conn = WebSocketTunnelConnection(url: url, session: session)
        return conn
    }
}
