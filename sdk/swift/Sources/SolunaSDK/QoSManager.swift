import Foundation

// MARK: - Network Quality

/// Assessed network quality level based on packet loss, jitter, and RTT.
public enum NetworkQuality: String, Sendable, CaseIterable, Comparable {
    case excellent = "Excellent"
    case good      = "Good"
    case fair      = "Fair"
    case poor      = "Poor"

    public static func < (lhs: NetworkQuality, rhs: NetworkQuality) -> Bool {
        let order: [NetworkQuality] = [.poor, .fair, .good, .excellent]
        return (order.firstIndex(of: lhs) ?? 0) < (order.firstIndex(of: rhs) ?? 0)
    }
}

// MARK: - Audio Codec

/// Supported audio codecs, ordered from highest to lowest quality.
public enum AudioCodec: String, Sendable, CaseIterable {
    /// 24-bit signed integer PCM (highest quality, ~2.3 Mbps stereo @ 48kHz).
    case s24 = "S24"

    /// Opus compressed (good quality, ~128 kbps).
    case opus = "Opus"

    /// LC3 Bluetooth LE Audio (good quality, low latency, ~96 kbps).
    case lc3 = "LC3"

    /// IMA-ADPCM (acceptable quality, ~384 kbps stereo).
    case adpcm = "ADPCM"

    /// OSTP payload type for this codec.
    public var payloadType: UInt8 {
        switch self {
        case .s24:   return 97
        case .opus:  return OSTConstants.ptOpus
        case .lc3:   return OSTConstants.ptLC3
        case .adpcm: return OSTConstants.ptADPCMStereo
        }
    }

    /// Approximate bitrate in kbps for stereo @ 48kHz.
    public var approximateBitrateKbps: Int {
        switch self {
        case .s24:   return 2304
        case .opus:  return 128
        case .lc3:   return 96
        case .adpcm: return 384
        }
    }
}

// MARK: - QoS Stats

/// Network quality statistics computed from packet reception.
public struct QoSStats: Sendable {
    /// Estimated packet loss as a percentage (0.0 - 100.0).
    public var packetLossPercent: Double = 0

    /// Jitter (inter-packet delay variation) in milliseconds.
    public var jitterMs: Double = 0

    /// Estimated available bandwidth in kbps.
    public var estimatedBandwidthKbps: Double = 0

    /// Round-trip time in milliseconds (from heartbeat echo).
    public var rttMs: Double = 0

    public init() {}
}

// MARK: - QoSManager

/// Adaptive quality of service manager for the Soluna relay connection.
///
/// Monitors network quality by analyzing packet sequence numbers and timing,
/// then automatically adjusts the audio codec to match available bandwidth.
///
/// Usage:
/// ```swift
/// let qos = QoSManager()
/// qos.onQualityChanged = { quality in
///     print("Network quality: \(quality.rawValue)")
/// }
/// // Feed packets as they arrive:
/// qos.recordPacket(sequenceNumber: seq, size: data.count)
/// ```
@MainActor
public final class QoSManager: ObservableObject {

    // MARK: - Published State

    /// Current assessed network quality.
    @Published public var currentQuality: NetworkQuality = .excellent

    /// Current audio codec (auto-selected or manually overridden).
    @Published public var currentCodec: AudioCodec = .s24

    /// Latest network quality statistics.
    @Published public var stats: QoSStats = QoSStats()

    // MARK: - Configuration

    /// When true, the manager automatically switches codec based on network quality.
    public var autoAdjust: Bool = true

    /// Override auto codec selection. Set to nil to re-enable automatic adjustment.
    public var preferredCodec: AudioCodec? {
        didSet {
            if let codec = preferredCodec {
                currentCodec = codec
            }
        }
    }

    /// Callback invoked when network quality changes.
    public var onQualityChanged: ((NetworkQuality) -> Void)?

    // MARK: - Private State

    private var lastSequenceNumber: UInt16?
    private var totalPackets: Int = 0
    private var lostPackets: Int = 0

    /// Rolling window of inter-arrival times (milliseconds).
    private var arrivalTimes: [CFAbsoluteTime] = []
    private var packetSizes: [Int] = []

    /// Maximum number of samples to keep in the rolling window.
    private let windowSize = 100

    /// Evaluation timer for periodic quality assessment.
    private var evaluationTimer: Timer?

    // MARK: - Init / Deinit

    public init() {}

    // MARK: - Start / Stop

    /// Start periodic quality evaluation.
    public func start() {
        evaluationTimer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
            Task { @MainActor in
                self?.evaluate()
            }
        }
    }

    /// Stop periodic quality evaluation and reset state.
    public func stop() {
        evaluationTimer?.invalidate()
        evaluationTimer = nil
        reset()
    }

    /// Reset all statistics.
    public func reset() {
        lastSequenceNumber = nil
        totalPackets = 0
        lostPackets = 0
        arrivalTimes.removeAll()
        packetSizes.removeAll()
        stats = QoSStats()
        currentQuality = .excellent
        if preferredCodec == nil {
            currentCodec = .s24
        }
    }

    // MARK: - Packet Recording

    /// Record an incoming packet for quality analysis.
    ///
    /// Call this for every received audio packet to keep statistics up to date.
    ///
    /// - Parameters:
    ///   - sequenceNumber: RTP sequence number from the packet header.
    ///   - size: Packet size in bytes.
    public func recordPacket(sequenceNumber: UInt16, size: Int) {
        let now = CFAbsoluteTimeGetCurrent()

        // Detect gaps in sequence numbers (packet loss)
        if let lastSeq = lastSequenceNumber {
            let expected = lastSeq &+ 1
            if sequenceNumber != expected {
                // Handle wrap-around: UInt16 overflow
                let gap: Int
                if sequenceNumber > expected {
                    gap = Int(sequenceNumber) - Int(expected)
                } else {
                    gap = Int(sequenceNumber) + 65536 - Int(expected)
                }
                // Cap gap to avoid false positives on large jumps (e.g. stream restart)
                if gap > 0 && gap < 100 {
                    lostPackets += gap
                }
            }
        }
        lastSequenceNumber = sequenceNumber
        totalPackets += 1

        // Record arrival time and size
        arrivalTimes.append(now)
        packetSizes.append(size)

        // Trim rolling window
        if arrivalTimes.count > windowSize {
            arrivalTimes.removeFirst(arrivalTimes.count - windowSize)
            packetSizes.removeFirst(packetSizes.count - windowSize)
        }
    }

    /// Record a heartbeat RTT measurement.
    ///
    /// - Parameter rttMs: Round-trip time in milliseconds.
    public func recordRTT(_ rttMs: Double) {
        stats.rttMs = rttMs
    }

    // MARK: - Evaluation

    /// Evaluate current network quality and adjust codec if needed.
    private func evaluate() {
        // Packet loss
        let totalObserved = totalPackets + lostPackets
        let lossPercent = totalObserved > 0 ? (Double(lostPackets) / Double(totalObserved)) * 100.0 : 0
        stats.packetLossPercent = lossPercent

        // Jitter (mean deviation of inter-arrival times)
        if arrivalTimes.count >= 2 {
            var intervals: [Double] = []
            for i in 1..<arrivalTimes.count {
                intervals.append((arrivalTimes[i] - arrivalTimes[i - 1]) * 1000.0)
            }
            let mean = intervals.reduce(0, +) / Double(intervals.count)
            let jitter = intervals.map { abs($0 - mean) }.reduce(0, +) / Double(intervals.count)
            stats.jitterMs = jitter
        }

        // Bandwidth estimation (bytes in window / time span)
        if arrivalTimes.count >= 2, let first = arrivalTimes.first, let last = arrivalTimes.last {
            let span = last - first
            if span > 0 {
                let totalBytes = packetSizes.reduce(0, +)
                stats.estimatedBandwidthKbps = (Double(totalBytes) * 8.0) / (span * 1000.0)
            }
        }

        // Assess quality
        let newQuality: NetworkQuality
        if lossPercent < 0.5 && stats.jitterMs < 5 && stats.rttMs < 50 {
            newQuality = .excellent
        } else if lossPercent < 2 && stats.jitterMs < 20 && stats.rttMs < 100 {
            newQuality = .good
        } else if lossPercent < 5 && stats.jitterMs < 50 && stats.rttMs < 200 {
            newQuality = .fair
        } else {
            newQuality = .poor
        }

        if newQuality != currentQuality {
            currentQuality = newQuality
            onQualityChanged?(newQuality)
        }

        // Auto-adjust codec
        if autoAdjust && preferredCodec == nil {
            let recommended: AudioCodec
            switch newQuality {
            case .excellent: recommended = .s24
            case .good:      recommended = .opus
            case .fair:      recommended = .adpcm
            case .poor:      recommended = .adpcm
            }
            if recommended != currentCodec {
                currentCodec = recommended
            }
        }
    }
}
