import Foundation

// MARK: - Balancing Strategy

/// Strategy for selecting endpoints.
public enum BalancingStrategy: String, Sendable, CaseIterable {
    /// Cycle through endpoints in order.
    case roundRobin = "Round Robin"

    /// Always pick the endpoint with the lowest average latency.
    case leastLatency = "Least Latency"

    /// Weight-based selection considering latency and configured weight.
    case weighted = "Weighted"

    /// Pick a random endpoint.
    case random = "Random"
}

// MARK: - Endpoint

/// A relay endpoint with load balancing metadata.
public struct Endpoint: Identifiable, Sendable, Hashable {
    /// Unique identifier (host:port).
    public var id: String { "\(host):\(port)" }

    /// Endpoint hostname or IP.
    public let host: String

    /// Endpoint port.
    public let port: UInt16

    /// Static weight (higher = more likely to be selected). Default 1.
    public var weight: Int

    /// Number of consecutive failures.
    public var failureCount: Int = 0

    /// Average latency in milliseconds (0 if unknown).
    public var avgLatencyMs: Double = 0

    /// Whether this endpoint is in circuit-breaker cooldown.
    public var isCoolingDown: Bool = false

    /// When the cooldown period started (nil if not cooling down).
    public var cooldownStartedAt: Date?

    public init(host: String, port: UInt16, weight: Int = 1) {
        self.host = host
        self.port = port
        self.weight = weight
    }

    public func hash(into hasher: inout Hasher) {
        hasher.combine(host)
        hasher.combine(port)
    }

    public static func == (lhs: Endpoint, rhs: Endpoint) -> Bool {
        lhs.host == rhs.host && lhs.port == rhs.port
    }
}

// MARK: - LoadBalancer

/// Client-side load balancer for distributing connections across relay endpoints.
///
/// Supports multiple strategies (round-robin, least latency, weighted, random)
/// and includes a circuit breaker that marks endpoints as failed after consecutive
/// failures with a 30-second cooldown period.
///
/// Usage:
/// ```swift
/// let lb = LoadBalancer(strategy: .leastLatency)
/// lb.addEndpoint(Endpoint(host: "relay.solun.art", port: 5100))
/// lb.addEndpoint(Endpoint(host: "lax.relay.solun.art", port: 5100))
/// let ep = lb.selectEndpoint()
/// // ... use ep ...
/// lb.reportSuccess(endpoint: ep, latencyMs: 42.5)
/// ```
@MainActor
public final class LoadBalancer: ObservableObject {

    // MARK: - Published State

    /// All registered endpoints.
    @Published public var endpoints: [Endpoint] = []

    // MARK: - Configuration

    /// The balancing strategy to use.
    public var strategy: BalancingStrategy

    /// Number of consecutive failures before circuit breaker trips.
    public var circuitBreakerThreshold: Int = 3

    /// Cooldown period in seconds after circuit breaker trips.
    public var cooldownSeconds: TimeInterval = 30

    // MARK: - Private State

    private var roundRobinIndex: Int = 0

    // MARK: - Init

    public init(strategy: BalancingStrategy = .leastLatency) {
        self.strategy = strategy
    }

    // MARK: - Endpoint Management

    /// Add an endpoint to the load balancer.
    public func addEndpoint(_ endpoint: Endpoint) {
        guard !endpoints.contains(endpoint) else { return }
        endpoints.append(endpoint)
    }

    /// Remove an endpoint from the load balancer.
    public func removeEndpoint(_ endpoint: Endpoint) {
        endpoints.removeAll { $0 == endpoint }
    }

    // MARK: - Selection

    /// Select an endpoint based on the current strategy.
    ///
    /// Automatically skips endpoints that are in circuit-breaker cooldown.
    /// Returns nil if no healthy endpoints are available.
    public func selectEndpoint() -> Endpoint? {
        // Reset endpoints whose cooldown has expired
        resetExpiredCooldowns()

        let healthy = endpoints.filter { !$0.isCoolingDown }
        guard !healthy.isEmpty else { return endpoints.first } // Last resort

        switch strategy {
        case .roundRobin:
            return selectRoundRobin(from: healthy)
        case .leastLatency:
            return selectLeastLatency(from: healthy)
        case .weighted:
            return selectWeighted(from: healthy)
        case .random:
            return healthy.randomElement()
        }
    }

    // MARK: - Reporting

    /// Report a failed request to an endpoint.
    ///
    /// Increments the failure counter. After `circuitBreakerThreshold` consecutive
    /// failures, the endpoint enters cooldown and will not be selected.
    public func reportFailure(endpoint: Endpoint) {
        guard let idx = endpoints.firstIndex(of: endpoint) else { return }
        endpoints[idx].failureCount += 1

        if endpoints[idx].failureCount >= circuitBreakerThreshold {
            endpoints[idx].isCoolingDown = true
            endpoints[idx].cooldownStartedAt = Date()
        }
    }

    /// Report a successful request to an endpoint.
    ///
    /// Resets the failure counter and updates the running average latency.
    public func reportSuccess(endpoint: Endpoint, latencyMs: Double) {
        guard let idx = endpoints.firstIndex(of: endpoint) else { return }
        endpoints[idx].failureCount = 0
        endpoints[idx].isCoolingDown = false
        endpoints[idx].cooldownStartedAt = nil

        // Exponential moving average (alpha = 0.3)
        let prev = endpoints[idx].avgLatencyMs
        if prev == 0 {
            endpoints[idx].avgLatencyMs = latencyMs
        } else {
            endpoints[idx].avgLatencyMs = prev * 0.7 + latencyMs * 0.3
        }
    }

    // MARK: - Private Selection Strategies

    private func selectRoundRobin(from candidates: [Endpoint]) -> Endpoint {
        let idx = roundRobinIndex % candidates.count
        roundRobinIndex += 1
        return candidates[idx]
    }

    private func selectLeastLatency(from candidates: [Endpoint]) -> Endpoint {
        // Prefer endpoints with known latency; among unknowns, pick first
        let known = candidates.filter { $0.avgLatencyMs > 0 }
        if let best = known.min(by: { $0.avgLatencyMs < $1.avgLatencyMs }) {
            return best
        }
        return candidates[0]
    }

    private func selectWeighted(from candidates: [Endpoint]) -> Endpoint {
        // Weight inversely proportional to latency, multiplied by static weight
        let totalWeight = candidates.reduce(0.0) { sum, ep in
            let latencyFactor = ep.avgLatencyMs > 0 ? (1000.0 / ep.avgLatencyMs) : 1.0
            return sum + latencyFactor * Double(ep.weight)
        }

        guard totalWeight > 0 else { return candidates[0] }

        var random = Double.random(in: 0..<totalWeight)
        for ep in candidates {
            let latencyFactor = ep.avgLatencyMs > 0 ? (1000.0 / ep.avgLatencyMs) : 1.0
            let w = latencyFactor * Double(ep.weight)
            random -= w
            if random <= 0 { return ep }
        }

        return candidates[0]
    }

    private func resetExpiredCooldowns() {
        let now = Date()
        for i in endpoints.indices where endpoints[i].isCoolingDown {
            if let started = endpoints[i].cooldownStartedAt,
               now.timeIntervalSince(started) >= cooldownSeconds {
                endpoints[i].isCoolingDown = false
                endpoints[i].cooldownStartedAt = nil
                endpoints[i].failureCount = 0
            }
        }
    }
}
