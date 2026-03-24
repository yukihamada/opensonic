import Foundation

/// User consent status for data collection under GDPR.
public enum ConsentStatus: String, Codable, Sendable {
    /// User has not been asked for consent yet.
    case notAsked
    /// User has granted consent.
    case granted
    /// User has denied consent.
    case denied
    /// User previously granted consent but has withdrawn it.
    case withdrawn
}

/// A category of data collected by the SDK.
public struct DataCategory: Sendable {
    /// Machine-readable identifier.
    public let name: String
    /// Human-readable description of what data is collected.
    public let description: String
    /// Whether this category is required for core SDK functionality.
    public let isRequired: Bool

    public init(name: String, description: String, isRequired: Bool) {
        self.name = name
        self.description = description
        self.isRequired = isRequired
    }
}

/// GDPR and privacy compliance manager for the Soluna SDK.
///
/// Manages user consent for data collection, provides data export and deletion
/// capabilities, and integrates with `AnalyticsTracker` and `OfflineCache` to
/// ensure complete data removal when consent is withdrawn.
///
/// Usage:
/// ```swift
/// let gdpr = GDPRManager()
///
/// // Check consent before collecting data
/// if gdpr.consentStatus == .notAsked {
///     // Show consent UI
///     gdpr.requestConsent()  // User granted
/// }
///
/// // Export all user data (Subject Access Request)
/// let export = gdpr.exportUserData()
///
/// // Delete everything (Right to Erasure)
/// gdpr.deleteAllUserData()
/// ```
public final class GDPRManager: ObservableObject {

    // MARK: - Published State

    /// Current consent status.
    @Published public private(set) var consentStatus: ConsentStatus = .notAsked

    // MARK: - Data Categories

    /// All categories of data collected by the SDK.
    public let dataCategories: [DataCategory] = [
        DataCategory(
            name: "analytics",
            description: "Listening statistics: per-channel duration, session counts, streak data, and peak listening hours.",
            isRequired: false
        ),
        DataCategory(
            name: "cache",
            description: "Offline audio cache: recorded audio segments stored locally for offline playback.",
            isRequired: false
        ),
        DataCategory(
            name: "audit_logs",
            description: "Audit logs: connection events, authentication attempts, and error records for security monitoring.",
            isRequired: false
        ),
        DataCategory(
            name: "preferences",
            description: "User preferences: selected channel, volume settings, and UI state.",
            isRequired: true
        ),
        DataCategory(
            name: "connection",
            description: "Connection metadata: relay server address, device name, and session identifiers.",
            isRequired: true
        ),
    ]

    // MARK: - Integration Points

    /// Reference to the analytics tracker for data clearing. Set by the host app.
    public weak var analyticsTracker: AnalyticsTracker?

    /// Reference to the offline cache for data clearing. Set by the host app.
    public var offlineCache: OfflineCache?

    // MARK: - Private

    private let defaults: UserDefaults
    private let consentKey = "com.soluna.gdpr.consentStatus"
    private let consentDateKey = "com.soluna.gdpr.consentDate"
    private let withdrawDateKey = "com.soluna.gdpr.withdrawDate"

    // MARK: - Init

    /// Create a GDPR manager.
    ///
    /// - Parameter defaults: UserDefaults instance for persisting consent state.
    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        loadConsentState()
    }

    // MARK: - Consent Management

    /// Record that the user has granted consent for data collection.
    ///
    /// Stores the consent timestamp for audit purposes.
    public func requestConsent() {
        consentStatus = .granted
        defaults.set(ConsentStatus.granted.rawValue, forKey: consentKey)
        defaults.set(Date().timeIntervalSince1970, forKey: consentDateKey)

        AuditLogger.shared.log(
            level: .info,
            category: .security,
            message: "GDPR consent granted"
        )
    }

    /// Record that the user has denied consent for data collection.
    public func denyConsent() {
        consentStatus = .denied
        defaults.set(ConsentStatus.denied.rawValue, forKey: consentKey)
        defaults.set(Date().timeIntervalSince1970, forKey: consentDateKey)

        AuditLogger.shared.log(
            level: .info,
            category: .security,
            message: "GDPR consent denied"
        )
    }

    /// Withdraw previously granted consent.
    ///
    /// Stops all optional data collection and deletes all non-essential stored data.
    /// This is equivalent to exercising the "Right to Object" under GDPR Article 21.
    public func withdrawConsent() {
        consentStatus = .withdrawn
        defaults.set(ConsentStatus.withdrawn.rawValue, forKey: consentKey)
        defaults.set(Date().timeIntervalSince1970, forKey: withdrawDateKey)

        // Clear non-essential data
        clearAnalyticsData()
        clearCacheData()

        AuditLogger.shared.log(
            level: .security,
            category: .security,
            message: "GDPR consent withdrawn — non-essential data deleted"
        )
    }

    // MARK: - Data Export (Subject Access Request)

    /// Export all stored user data as a JSON document.
    ///
    /// This fulfills the GDPR "Right of Access" (Article 15). The export includes
    /// analytics data, consent records, cached track metadata, and audit logs.
    ///
    /// - Returns: JSON-encoded data containing all user information.
    public func exportUserData() -> Data {
        var export: [String: Any] = [:]

        // Consent records
        var consent: [String: Any] = [
            "status": consentStatus.rawValue
        ]
        if let consentDate = defaults.object(forKey: consentDateKey) as? TimeInterval {
            consent["grantedAt"] = ISO8601DateFormatter().string(from: Date(timeIntervalSince1970: consentDate))
        }
        if let withdrawDate = defaults.object(forKey: withdrawDateKey) as? TimeInterval {
            consent["withdrawnAt"] = ISO8601DateFormatter().string(from: Date(timeIntervalSince1970: withdrawDate))
        }
        export["consent"] = consent

        // Analytics data
        if let tracker = analyticsTracker {
            var analytics: [String: Any] = [
                "totalListenMinutes": tracker.totalListenMinutes,
                "currentStreak": tracker.currentStreak,
                "peakHour": tracker.peakHour
            ]

            var channelData: [[String: Any]] = []
            for (_, stat) in tracker.channelStats {
                channelData.append([
                    "channelId": stat.channelId,
                    "totalMinutes": stat.totalMinutes,
                    "sessionCount": stat.sessionCount,
                    "lastListened": ISO8601DateFormatter().string(from: stat.lastListened)
                ])
            }
            analytics["channels"] = channelData
            export["analytics"] = analytics
        }

        // Cached tracks metadata (not the audio data itself)
        if let cache = offlineCache {
            var cachedInfo: [[String: Any]] = []
            for channel in SolunaChannels.all {
                let tracks = cache.cachedTracks(for: channel.id)
                for track in tracks {
                    cachedInfo.append([
                        "channel": track.channel,
                        "title": track.title,
                        "duration": track.duration,
                        "cachedAt": ISO8601DateFormatter().string(from: track.cachedAt)
                    ])
                }
            }
            if !cachedInfo.isEmpty {
                export["cachedTracks"] = cachedInfo
            }
        }

        // Audit logs (last 90 days)
        let logsData = AuditLogger.shared.exportLogs(since: Date().addingTimeInterval(-90 * 86400))
        if let logsJSON = try? JSONSerialization.jsonObject(with: logsData) {
            export["auditLogs"] = logsJSON
        }

        // Export metadata
        export["exportedAt"] = ISO8601DateFormatter().string(from: Date())
        export["sdkVersion"] = "SolunaSDK"

        return (try? JSONSerialization.data(withJSONObject: export, options: [.prettyPrinted, .sortedKeys])) ?? Data("{}" .utf8)
    }

    // MARK: - Data Deletion (Right to Erasure)

    /// Delete all user data stored by the SDK.
    ///
    /// This fulfills the GDPR "Right to Erasure" (Article 17). Clears analytics,
    /// cached audio, audit logs, and all SDK-related UserDefaults keys.
    public func deleteAllUserData() {
        AuditLogger.shared.log(
            level: .security,
            category: .security,
            message: "GDPR data deletion requested — erasing all user data"
        )

        clearAnalyticsData()
        clearCacheData()
        AuditLogger.shared.clearLogs()

        // Clear SDK UserDefaults keys
        let solunaKeys = defaults.dictionaryRepresentation().keys.filter { $0.hasPrefix("com.soluna.") }
        for key in solunaKeys {
            defaults.removeObject(forKey: key)
        }

        consentStatus = .notAsked
    }

    // MARK: - Private

    private func loadConsentState() {
        if let raw = defaults.string(forKey: consentKey),
           let status = ConsentStatus(rawValue: raw) {
            consentStatus = status
        }
    }

    private func clearAnalyticsData() {
        // Clear analytics UserDefaults keys
        let analyticsKeys = [
            "com.soluna.analytics.channelStats",
            "com.soluna.analytics.hourCounts",
            "com.soluna.analytics.streakDates",
            "com.soluna.analytics.totalMinutes"
        ]
        for key in analyticsKeys {
            defaults.removeObject(forKey: key)
        }
    }

    private func clearCacheData() {
        offlineCache?.clearCache()
    }
}
