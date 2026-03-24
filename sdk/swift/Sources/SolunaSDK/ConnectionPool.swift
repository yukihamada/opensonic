import Foundation

// MARK: - Connection Health

/// Health status of a pooled relay connection.
public struct ConnectionHealth: Sendable, Identifiable {
    /// Unique connection identifier.
    public let id: String

    /// Measured latency in milliseconds.
    public let latencyMs: Double

    /// Total packets received on this connection.
    public let packetsReceived: Int

    /// Connection uptime in seconds.
    public let uptimeSeconds: TimeInterval

    /// Current status of the connection.
    public let status: ConnectionStatus
}

/// Status of a pooled connection.
public enum ConnectionStatus: String, Sendable {
    case active      = "Active"
    case idle        = "Idle"
    case reconnecting = "Reconnecting"
    case failed      = "Failed"
}

// MARK: - PooledConnection

/// Internal wrapper around a RelayConnection with pool metadata.
final class PooledConnection: @unchecked Sendable {
    let id: String
    let connection: RelayConnection
    let createdAt: Date
    var lastUsed: Date
    var packetsReceived: Int = 0
    var status: ConnectionStatus = .idle
    var channel: String?

    init(id: String, connection: RelayConnection) {
        self.id = id
        self.connection = connection
        self.createdAt = Date()
        self.lastUsed = Date()
    }

    var uptimeSeconds: TimeInterval {
        Date().timeIntervalSince(createdAt)
    }

    func toHealth(latencyMs: Double) -> ConnectionHealth {
        ConnectionHealth(
            id: id,
            latencyMs: latencyMs,
            packetsReceived: packetsReceived,
            uptimeSeconds: uptimeSeconds,
            status: status
        )
    }
}

// MARK: - ConnectionPool

/// Manages a pool of relay connections for load distribution and high availability.
///
/// Supports connection recycling, health monitoring, and automatic reconnection.
/// Useful for scaling to large numbers of simultaneous listeners by distributing
/// load across multiple relay connections.
///
/// Usage:
/// ```swift
/// let pool = ConnectionPool(host: "relay.solun.art", port: 5100)
/// let conn = pool.acquire(channel: "soluna")
/// // Use conn...
/// pool.release(conn)
/// ```
@MainActor
public final class ConnectionPool: ObservableObject {

    // MARK: - Published State

    /// Number of currently active connections in the pool.
    @Published public var activeConnections: Int = 0

    // MARK: - Configuration

    /// Maximum number of connections in the pool.
    public var maxConnections: Int

    /// Relay server hostname.
    public let host: String

    /// Relay server port.
    public let port: UInt16

    /// Device name used in JOIN messages.
    public var deviceName: String

    /// Time after which idle connections are recycled (seconds).
    public var idleTimeoutSeconds: TimeInterval = 60

    // MARK: - Private State

    private var connections: [PooledConnection] = []
    private var nextId: Int = 0
    private var healthTimer: Timer?

    // MARK: - Init

    /// Create a connection pool targeting a specific relay server.
    ///
    /// - Parameters:
    ///   - host: Relay server hostname.
    ///   - port: Relay server port.
    ///   - maxConnections: Maximum pool size (default 3).
    ///   - deviceName: Device name for JOIN messages.
    public init(
        host: String = OSTConstants.defaultHost,
        port: UInt16 = OSTConstants.defaultPort,
        maxConnections: Int = 3,
        deviceName: String = "SolunaSDK-Pool"
    ) {
        self.host = host
        self.port = port
        self.maxConnections = maxConnections
        self.deviceName = deviceName
    }

    deinit {
        healthTimer?.invalidate()
    }

    // MARK: - Public API

    /// Acquire a relay connection for the given channel.
    ///
    /// Returns an existing idle connection if available, or creates a new one
    /// if the pool has capacity. Uses round-robin selection among idle connections.
    ///
    /// - Parameter channel: The channel to join.
    /// - Returns: A connected `RelayConnection`, or nil if the pool is exhausted.
    public func acquire(channel: String) -> RelayConnection? {
        // Try to reuse an idle connection on the same channel
        if let existing = connections.first(where: { $0.status == .idle && $0.channel == channel }) {
            existing.status = .active
            existing.lastUsed = Date()
            updateActiveCount()
            return existing.connection
        }

        // Try to reuse any idle connection (will need re-JOIN)
        if let idle = connections.first(where: { $0.status == .idle }) {
            idle.status = .active
            idle.channel = channel
            idle.lastUsed = Date()
            idle.connection.sendControlMessage("JOIN:\(channel)::\(deviceName)\n")
            updateActiveCount()
            return idle.connection
        }

        // Create new connection if under limit
        guard connections.count < maxConnections else { return nil }

        let conn = RelayConnection(
            channel: channel,
            host: host,
            port: port,
            deviceName: "\(deviceName)-\(nextId)"
        )

        guard conn.connect() else { return nil }

        let pooled = PooledConnection(id: "conn-\(nextId)", connection: conn)
        pooled.status = .active
        pooled.channel = channel
        nextId += 1

        connections.append(pooled)
        updateActiveCount()

        // Start health monitoring if not already running
        startHealthMonitorIfNeeded()

        return conn
    }

    /// Release a connection back to the pool.
    ///
    /// The connection becomes idle and available for reuse.
    ///
    /// - Parameter connection: The connection to release.
    public func release(_ connection: RelayConnection) {
        if let pooled = connections.first(where: { $0.connection === connection }) {
            pooled.status = .idle
            pooled.lastUsed = Date()
            updateActiveCount()
        }
    }

    /// Perform a health check on all pooled connections.
    ///
    /// - Returns: Health status for each connection in the pool.
    public func healthCheck() async -> [ConnectionHealth] {
        connections.map { pooled in
            pooled.toHealth(latencyMs: 0) // Latency requires probe; report 0 for now
        }
    }

    /// Drain all connections and shut down the pool.
    public func drain() {
        for pooled in connections {
            pooled.connection.disconnect()
        }
        connections.removeAll()
        healthTimer?.invalidate()
        healthTimer = nil
        updateActiveCount()
    }

    /// Remove and disconnect idle connections that have exceeded the idle timeout.
    public func evictIdle() {
        let now = Date()
        let expired = connections.filter {
            $0.status == .idle && now.timeIntervalSince($0.lastUsed) > idleTimeoutSeconds
        }
        for pooled in expired {
            pooled.connection.disconnect()
            connections.removeAll { $0.id == pooled.id }
        }
        updateActiveCount()
    }

    // MARK: - Private

    private func updateActiveCount() {
        activeConnections = connections.filter { $0.status == .active }.count
    }

    private func startHealthMonitorIfNeeded() {
        guard healthTimer == nil else { return }
        healthTimer = Timer.scheduledTimer(withTimeInterval: 30, repeats: true) { [weak self] _ in
            Task { @MainActor in
                self?.evictIdle()
                self?.reconnectFailed()
            }
        }
    }

    /// Attempt to reconnect connections in a failed state.
    private func reconnectFailed() {
        for pooled in connections where pooled.status == .failed {
            pooled.status = .reconnecting

            let newConn = RelayConnection(
                channel: pooled.channel ?? "",
                host: host,
                port: port,
                deviceName: "\(deviceName)-\(pooled.id)"
            )

            if newConn.connect() {
                pooled.status = .idle
            } else {
                pooled.status = .failed
            }
        }
    }
}
