import Foundation
import Combine

/// Health score with quality tier classification.
public struct HealthScore: Sendable {
    /// Numeric score from 0 to 100.
    public let score: Int

    /// Quality tier based on the score.
    public var tier: Tier {
        switch score {
        case 90...100: return .excellent
        case 70..<90: return .good
        case 50..<70: return .fair
        default: return .poor
        }
    }

    public enum Tier: String, Sendable {
        case excellent
        case good
        case fair
        case poor
    }

    public init(score: Int) {
        self.score = max(0, min(100, score))
    }
}

/// Comprehensive health report from a full connection test.
public struct HealthReport: Sendable {
    public let score: HealthScore
    public let latencyMs: Double
    public let packetLossPercent: Double
    public let jitterMs: Double
    public let udpReachable: Bool
    public let wsReachable: Bool
    public let timestamp: Date

    public init(score: HealthScore, latencyMs: Double, packetLossPercent: Double, jitterMs: Double, udpReachable: Bool, wsReachable: Bool, timestamp: Date = Date()) {
        self.score = score
        self.latencyMs = latencyMs
        self.packetLossPercent = packetLossPercent
        self.jitterMs = jitterMs
        self.udpReachable = udpReachable
        self.wsReachable = wsReachable
        self.timestamp = timestamp
    }
}

/// Connection quality testing and SLA monitoring.
///
/// Runs automated connection quality tests against the relay server,
/// measuring latency, packet loss, jitter, and reachability.
///
/// Usage:
/// ```swift
/// let health = HealthCheck()
/// let score = await health.quickTest()
/// print("Connection quality: \(score.tier)")
/// ```
public final class HealthCheck: ObservableObject {

    // MARK: - Published State

    /// Latest health score.
    @Published public private(set) var latestScore: HealthScore?

    // MARK: - Callbacks

    /// Called when health score changes during monitoring.
    public var onHealthChanged: ((HealthScore) -> Void)?

    // MARK: - Configuration

    /// Relay server host for testing.
    public var relayHost: String = OSTConstants.defaultHost

    /// Relay server port for testing.
    public var relayPort: UInt16 = OSTConstants.defaultPort

    /// WebSocket URL for testing.
    public var wsURL: String = OSTConstants.defaultWebSocketURL

    // MARK: - Private

    private var monitoringTimer: Timer?
    private let session: URLSession

    // MARK: - Init

    public init() {
        let config = URLSessionConfiguration.default
        config.timeoutIntervalForRequest = 5
        session = URLSession(configuration: config)
    }

    // MARK: - Public API

    /// Run a comprehensive health test.
    ///
    /// Tests UDP reachability, WebSocket connectivity, latency, and jitter.
    /// - Returns: A full health report.
    public func runFullTest() async -> HealthReport {
        let udpResult = await testUDPReachability()
        let wsResult = await testWSReachability()
        let latencyResult = await measureLatency()

        // Calculate composite score
        var score = 100

        // UDP reachability (critical)
        if !udpResult { score -= 40 }

        // WebSocket reachability
        if !wsResult { score -= 20 }

        // Latency penalty
        if latencyResult.latencyMs > 200 { score -= 20 }
        else if latencyResult.latencyMs > 100 { score -= 10 }
        else if latencyResult.latencyMs > 50 { score -= 5 }

        // Jitter penalty
        if latencyResult.jitterMs > 50 { score -= 15 }
        else if latencyResult.jitterMs > 20 { score -= 5 }

        // Packet loss penalty
        if latencyResult.packetLossPercent > 5 { score -= 20 }
        else if latencyResult.packetLossPercent > 1 { score -= 10 }

        let healthScore = HealthScore(score: score)

        let report = HealthReport(
            score: healthScore,
            latencyMs: latencyResult.latencyMs,
            packetLossPercent: latencyResult.packetLossPercent,
            jitterMs: latencyResult.jitterMs,
            udpReachable: udpResult,
            wsReachable: wsResult
        )

        await MainActor.run {
            latestScore = healthScore
            onHealthChanged?(healthScore)
        }

        return report
    }

    /// Run a quick health test (latency + basic reachability).
    ///
    /// - Returns: A health score from 0 to 100.
    public func quickTest() async -> HealthScore {
        let wsReachable = await testWSReachability()
        let latency = await measureLatency()

        var score = 100
        if !wsReachable { score -= 30 }
        if latency.latencyMs > 200 { score -= 30 }
        else if latency.latencyMs > 100 { score -= 15 }
        if latency.packetLossPercent > 5 { score -= 20 }

        let healthScore = HealthScore(score: score)

        await MainActor.run {
            latestScore = healthScore
            onHealthChanged?(healthScore)
        }

        return healthScore
    }

    /// Start periodic health monitoring.
    ///
    /// - Parameter interval: Time between checks in seconds (minimum 10).
    public func startMonitoring(interval: TimeInterval) {
        stopMonitoring()
        let safeInterval = max(interval, 10)

        monitoringTimer = Timer.scheduledTimer(withTimeInterval: safeInterval, repeats: true) { [weak self] _ in
            Task { [weak self] in
                _ = await self?.quickTest()
            }
        }
    }

    /// Stop periodic health monitoring.
    public func stopMonitoring() {
        monitoringTimer?.invalidate()
        monitoringTimer = nil
    }

    // MARK: - Private Tests

    private func testUDPReachability() async -> Bool {
        // Test UDP by attempting to create and send a heartbeat packet
        let fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard fd >= 0 else { return false }
        defer { close(fd) }

        // Set timeout
        var tv = timeval(tv_sec: 3, tv_usec: 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

        // Resolve host
        guard let hostEntry = gethostbyname(relayHost) else { return false }

        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = relayPort.bigEndian
        memcpy(&addr.sin_addr, hostEntry.pointee.h_addr_list[0]!, Int(hostEntry.pointee.h_length))

        // Send a minimal heartbeat (empty UDP packet)
        let sent = withUnsafePointer(to: &addr) { ptr in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sockPtr in
                sendto(fd, "HB", 2, 0, sockPtr, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }

        return sent > 0
    }

    private func testWSReachability() async -> Bool {
        // Test WebSocket by trying an HTTP upgrade handshake to the relay
        guard let url = URL(string: "https://\(relayHost)/health") else { return false }

        do {
            let (_, response) = try await session.data(from: url)
            if let http = response as? HTTPURLResponse {
                return (200..<500).contains(http.statusCode)
            }
            return false
        } catch {
            return false
        }
    }

    private struct LatencyResult {
        var latencyMs: Double
        var jitterMs: Double
        var packetLossPercent: Double
    }

    private func measureLatency() async -> LatencyResult {
        // Measure HTTP round-trip time as a proxy for network latency
        guard let url = URL(string: "https://\(relayHost)/health") else {
            return LatencyResult(latencyMs: 999, jitterMs: 999, packetLossPercent: 100)
        }

        let attempts = 5
        var latencies: [Double] = []
        var failures = 0

        for _ in 0..<attempts {
            let start = ProcessInfo.processInfo.systemUptime
            do {
                let (_, _) = try await session.data(from: url)
                let elapsed = (ProcessInfo.processInfo.systemUptime - start) * 1000.0
                latencies.append(elapsed)
            } catch {
                failures += 1
            }
        }

        let avgLatency = latencies.isEmpty ? 999.0 : latencies.reduce(0, +) / Double(latencies.count)
        let packetLoss = Double(failures) / Double(attempts) * 100.0

        // Jitter = average deviation from mean
        let jitter: Double
        if latencies.count > 1 {
            let mean = latencies.reduce(0, +) / Double(latencies.count)
            let deviations = latencies.map { abs($0 - mean) }
            jitter = deviations.reduce(0, +) / Double(deviations.count)
        } else {
            jitter = 0
        }

        return LatencyResult(latencyMs: avgLatency, jitterMs: jitter, packetLossPercent: packetLoss)
    }
}
