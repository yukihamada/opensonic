import Foundation
import Combine

/// A detected copyrighted track.
public struct CopyrightTrack: Sendable {
    public let title: String
    public let artist: String
    public let fingerprint: String
    public let detectedAt: Date

    public init(title: String, artist: String, fingerprint: String, detectedAt: Date = Date()) {
        self.title = title
        self.artist = artist
        self.fingerprint = fingerprint
        self.detectedAt = detectedAt
    }
}

/// Usage record for a copyrighted track.
public struct CopyrightUsage: Sendable {
    public let track: CopyrightTrack
    public let channel: String
    public let durationSeconds: Double
    public let listenersCount: Int
}

/// Action required for a copyright notice.
public enum CopyrightAction: Sendable {
    case acknowledge
    case skip
}

/// A copyright notice from the relay server.
public struct CopyrightNotice: Sendable {
    public let track: CopyrightTrack
    public let message: String
    public let action: CopyrightAction
}

/// Copyright and royalty tracking for licensing compliance.
///
/// Monitors audio streams for copyrighted content by parsing relay server
/// messages (COPYRIGHT_WARN and COPYRIGHT_MATCH), tracks usage, and
/// provides reporting for licensing compliance.
///
/// Usage:
/// ```swift
/// let cm = CopyrightManager()
/// cm.isTrackingEnabled = true
/// cm.onCopyrightNotice = { notice in
///     print("Copyright: \(notice.track.title) - \(notice.message)")
/// }
/// ```
public final class CopyrightManager: ObservableObject {

    // MARK: - Published State

    /// Whether copyright tracking is enabled.
    @Published public var isTrackingEnabled: Bool = false

    /// The currently detected track (if any).
    @Published public private(set) var currentTrack: CopyrightTrack?

    // MARK: - Callbacks

    /// Called when a copyright notice is received from the relay.
    public var onCopyrightNotice: ((CopyrightNotice) -> Void)?

    // MARK: - Private

    private var usageRecords: [CopyrightUsage] = []
    private var acknowledgedTrackIds: Set<String> = []
    private let queue = DispatchQueue(label: "com.soluna.copyright", qos: .utility)

    // MARK: - Init

    public init() {}

    // MARK: - Public API

    /// Get usage report for all tracked content since a given date.
    ///
    /// - Parameter since: Start date for the report.
    /// - Returns: Array of usage records.
    public func usageReport(since: Date) -> [CopyrightUsage] {
        queue.sync {
            usageRecords.filter { $0.track.detectedAt >= since }
        }
    }

    /// Acknowledge a copyright notice for a track.
    ///
    /// - Parameter trackId: The fingerprint ID of the track to acknowledge.
    public func acknowledgeNotice(trackId: String) {
        queue.async { [weak self] in
            self?.acknowledgedTrackIds.insert(trackId)
        }
    }

    /// Process a relay message for copyright-related content.
    ///
    /// Call this when receiving messages from the relay server.
    /// Recognized message types: `COPYRIGHT_WARN` and `COPYRIGHT_MATCH`.
    ///
    /// - Parameter message: The raw relay message string.
    public func processRelayMessage(_ message: String) {
        guard isTrackingEnabled else { return }

        queue.async { [weak self] in
            self?.parseMessage(message)
        }
    }

    /// Record a usage entry for the current track on a channel.
    ///
    /// - Parameters:
    ///   - channel: The channel name.
    ///   - durationSeconds: How long the track played.
    ///   - listenersCount: Number of listeners during playback.
    public func recordUsage(channel: String, durationSeconds: Double, listenersCount: Int) {
        guard let track = currentTrack else { return }

        let usage = CopyrightUsage(
            track: track,
            channel: channel,
            durationSeconds: durationSeconds,
            listenersCount: listenersCount
        )

        queue.async { [weak self] in
            self?.usageRecords.append(usage)
        }
    }

    // MARK: - Private

    private func parseMessage(_ message: String) {
        // Expected format: "COPYRIGHT_WARN|title|artist|fingerprint|message"
        // or: "COPYRIGHT_MATCH|title|artist|fingerprint"
        let parts = message.split(separator: "|").map(String.init)

        guard parts.count >= 4 else { return }

        let type = parts[0]
        let title = parts[1]
        let artist = parts[2]
        let fingerprint = parts[3]

        let track = CopyrightTrack(
            title: title,
            artist: artist,
            fingerprint: fingerprint
        )

        DispatchQueue.main.async { [weak self] in
            self?.currentTrack = track
        }

        switch type {
        case "COPYRIGHT_WARN":
            let msg = parts.count > 4 ? parts[4] : "Copyrighted content detected"
            let notice = CopyrightNotice(
                track: track,
                message: msg,
                action: .acknowledge
            )
            DispatchQueue.main.async { [weak self] in
                self?.onCopyrightNotice?(notice)
            }

        case "COPYRIGHT_MATCH":
            let notice = CopyrightNotice(
                track: track,
                message: "Content matched copyright database",
                action: .skip
            )
            DispatchQueue.main.async { [weak self] in
                self?.onCopyrightNotice?(notice)
            }

        default:
            break
        }
    }
}
