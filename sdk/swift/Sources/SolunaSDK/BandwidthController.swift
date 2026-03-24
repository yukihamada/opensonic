import Foundation

// MARK: - Bandwidth Stats

/// Real-time bandwidth usage statistics.
public struct BandwidthStats: Sendable {
    /// Total bytes sent since the controller was started/reset.
    public var sentBytesTotal: Int = 0

    /// Total bytes received since the controller was started/reset.
    public var receivedBytesTotal: Int = 0

    /// Current outgoing bandwidth in kbps (rolling 1-second window).
    public var sentKbps: Double = 0

    /// Current incoming bandwidth in kbps (rolling 1-second window).
    public var receivedKbps: Double = 0

    /// Peak outgoing bandwidth observed in kbps.
    public var peakSentKbps: Double = 0

    /// Peak incoming bandwidth observed in kbps.
    public var peakReceivedKbps: Double = 0

    public init() {}
}

// MARK: - BandwidthController

/// Network bandwidth manager with token-bucket rate limiting.
///
/// Limits outgoing bandwidth on metered or corporate networks. Uses a token bucket
/// algorithm: tokens refill at `maxBandwidthKbps` rate, and each packet consumes
/// tokens equal to its size. Burst allowance permits short spikes above the limit.
///
/// Usage:
/// ```swift
/// let bw = BandwidthController()
/// bw.maxBandwidthKbps = 500  // Limit to 500 kbps outgoing
/// if bw.shouldSend(packetSize: 1200) {
///     connection.sendRawData(packet)
///     bw.recordSent(bytes: 1200)
/// }
/// ```
@MainActor
public final class BandwidthController: ObservableObject {

    // MARK: - Published State

    /// Current bandwidth usage statistics.
    @Published public var stats: BandwidthStats = BandwidthStats()

    /// Current outgoing bandwidth usage in kbps.
    @Published public var currentUsageKbps: Double = 0

    // MARK: - Configuration

    /// Maximum outgoing bandwidth in kbps. Set to 0 for unlimited.
    public var maxBandwidthKbps: Int = 0

    /// Burst allowance above the rate limit in kbps. Allows short traffic spikes.
    public var burstAllowanceKbps: Int = 100

    // MARK: - Token Bucket State

    /// Available tokens in bits (refilled at maxBandwidthKbps rate).
    private var tokenBits: Double = 0

    /// Maximum token bucket capacity in bits.
    private var bucketCapacityBits: Double {
        Double(maxBandwidthKbps + burstAllowanceKbps) * 1000.0 // bits for 1 second
    }

    /// Timestamp of last token refill.
    private var lastRefillTime: CFAbsoluteTime = CFAbsoluteTimeGetCurrent()

    // MARK: - Rolling Window

    /// Timestamped byte records for the 1-second rolling window.
    private var sentWindow: [(time: CFAbsoluteTime, bytes: Int)] = []
    private var receivedWindow: [(time: CFAbsoluteTime, bytes: Int)] = []

    /// Rolling window duration in seconds.
    private let windowDuration: TimeInterval = 1.0

    /// Periodic stats update timer.
    private var updateTimer: Timer?

    // MARK: - Init

    public init() {
        tokenBits = bucketCapacityBits
    }

    // MARK: - Start / Stop

    /// Start periodic bandwidth statistics updates.
    public func start() {
        lastRefillTime = CFAbsoluteTimeGetCurrent()
        tokenBits = bucketCapacityBits

        updateTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor in
                self?.updateStats()
            }
        }
    }

    /// Stop bandwidth monitoring.
    public func stop() {
        updateTimer?.invalidate()
        updateTimer = nil
    }

    /// Reset all statistics and refill the token bucket.
    public func reset() {
        stats = BandwidthStats()
        currentUsageKbps = 0
        sentWindow.removeAll()
        receivedWindow.removeAll()
        tokenBits = bucketCapacityBits
        lastRefillTime = CFAbsoluteTimeGetCurrent()
    }

    // MARK: - Rate Limiting

    /// Check whether a packet of the given size can be sent within the rate limit.
    ///
    /// If `maxBandwidthKbps` is 0 (unlimited), always returns true.
    /// Otherwise, checks the token bucket for available capacity.
    ///
    /// - Parameter packetSize: Size of the packet in bytes.
    /// - Returns: True if the packet can be sent without exceeding the rate limit.
    public func shouldSend(packetSize: Int) -> Bool {
        guard maxBandwidthKbps > 0 else { return true }

        refillTokens()

        let bitsNeeded = Double(packetSize * 8)
        return tokenBits >= bitsNeeded
    }

    /// Record that bytes were sent. Consumes tokens from the bucket.
    ///
    /// - Parameter bytes: Number of bytes sent.
    public func recordSent(bytes: Int) {
        let now = CFAbsoluteTimeGetCurrent()

        // Consume tokens
        if maxBandwidthKbps > 0 {
            refillTokens()
            tokenBits = max(0, tokenBits - Double(bytes * 8))
        }

        stats.sentBytesTotal += bytes
        sentWindow.append((time: now, bytes: bytes))
    }

    /// Record that bytes were received (for statistics only, no rate limiting).
    ///
    /// - Parameter bytes: Number of bytes received.
    public func recordReceived(bytes: Int) {
        let now = CFAbsoluteTimeGetCurrent()
        stats.receivedBytesTotal += bytes
        receivedWindow.append((time: now, bytes: bytes))
    }

    // MARK: - Private

    /// Refill tokens based on elapsed time since last refill.
    private func refillTokens() {
        let now = CFAbsoluteTimeGetCurrent()
        let elapsed = now - lastRefillTime
        lastRefillTime = now

        let refillBits = Double(maxBandwidthKbps) * 1000.0 * elapsed
        tokenBits = min(bucketCapacityBits, tokenBits + refillBits)
    }

    /// Update rolling-window statistics.
    private func updateStats() {
        let now = CFAbsoluteTimeGetCurrent()
        let cutoff = now - windowDuration

        // Trim old entries
        sentWindow.removeAll { $0.time < cutoff }
        receivedWindow.removeAll { $0.time < cutoff }

        // Calculate current rates
        let sentBytes = sentWindow.reduce(0) { $0 + $1.bytes }
        let receivedBytes = receivedWindow.reduce(0) { $0 + $1.bytes }

        let sentKbps = Double(sentBytes * 8) / (windowDuration * 1000.0)
        let receivedKbps = Double(receivedBytes * 8) / (windowDuration * 1000.0)

        stats.sentKbps = sentKbps
        stats.receivedKbps = receivedKbps
        stats.peakSentKbps = max(stats.peakSentKbps, sentKbps)
        stats.peakReceivedKbps = max(stats.peakReceivedKbps, receivedKbps)

        currentUsageKbps = sentKbps
    }
}
