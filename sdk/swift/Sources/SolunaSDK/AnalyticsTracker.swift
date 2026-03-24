import Foundation

/// Per-channel listening statistics.
public struct ChannelStat: Codable, Sendable {
    /// Channel identifier.
    public let channelId: String
    /// Total minutes listened on this channel.
    public var totalMinutes: Double
    /// Number of listening sessions on this channel.
    public var sessionCount: Int
    /// Timestamp of the last listening event.
    public var lastListened: Date

    public init(channelId: String, totalMinutes: Double = 0, sessionCount: Int = 0, lastListened: Date = Date()) {
        self.channelId = channelId
        self.totalMinutes = totalMinutes
        self.sessionCount = sessionCount
        self.lastListened = lastListened
    }
}

/// Tracks listening analytics stored in UserDefaults.
///
/// Records per-channel listening duration, session counts, streaks,
/// and peak listening hours. No external service required.
///
/// Usage:
/// ```swift
/// let tracker = AnalyticsTracker()
/// tracker.startSession(channel: "jazz")
/// // ... user listens ...
/// tracker.endSession()
/// print(tracker.totalListenMinutes) // e.g. 42.5
/// print(tracker.currentStreak)       // e.g. 3 (days)
/// ```
public final class AnalyticsTracker: ObservableObject {

    // MARK: - Published State

    /// Total listening minutes across all channels.
    @Published public private(set) var totalListenMinutes: Double = 0

    /// Per-channel statistics.
    @Published public private(set) var channelStats: [String: ChannelStat] = [:]

    /// Number of consecutive days with at least one listening session.
    @Published public private(set) var currentStreak: Int = 0

    /// Most common listening hour (0-23).
    @Published public private(set) var peakHour: Int = 0

    // MARK: - Private

    private let defaults: UserDefaults
    private let statsKey = "com.soluna.analytics.channelStats"
    private let hoursKey = "com.soluna.analytics.hourCounts"
    private let streakKey = "com.soluna.analytics.streakDates"

    /// Active session tracking.
    private var activeChannel: String?
    private var sessionStart: Date?
    private var sessionTimer: Timer?

    // MARK: - Init

    /// Create an analytics tracker.
    ///
    /// - Parameter defaults: UserDefaults instance to use (default: .standard).
    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        loadState()
    }

    deinit {
        endSession()
    }

    // MARK: - Session Management

    /// Start a new listening session for the given channel.
    ///
    /// - Parameter channel: The channel identifier.
    public func startSession(channel: String) {
        // End any existing session first
        if activeChannel != nil {
            endSession()
        }

        activeChannel = channel
        sessionStart = Date()

        // Increment session count
        var stat = channelStats[channel] ?? ChannelStat(channelId: channel)
        stat.sessionCount += 1
        stat.lastListened = Date()
        channelStats[channel] = stat

        // Record the listening hour
        recordHour(Calendar.current.component(.hour, from: Date()))

        // Update streak
        recordListeningDate(Date())

        // Periodic minute counter (every 60 seconds)
        sessionTimer = Timer.scheduledTimer(withTimeInterval: 60, repeats: true) { [weak self] _ in
            self?.tickMinute()
        }

        saveState()
    }

    /// End the current listening session and persist stats.
    public func endSession() {
        guard let channel = activeChannel, let start = sessionStart else { return }

        sessionTimer?.invalidate()
        sessionTimer = nil

        // Add remaining partial minute
        let elapsed = Date().timeIntervalSince(start)
        let minutes = elapsed / 60.0
        let wholeMinutesCounted = floor(minutes)
        let remainder = minutes - wholeMinutesCounted

        if remainder > 0 {
            addMinutes(remainder, to: channel)
        }

        activeChannel = nil
        sessionStart = nil

        saveState()
    }

    // MARK: - Private

    private func tickMinute() {
        guard let channel = activeChannel else { return }
        addMinutes(1, to: channel)
        saveState()
    }

    private func addMinutes(_ minutes: Double, to channel: String) {
        totalListenMinutes += minutes

        var stat = channelStats[channel] ?? ChannelStat(channelId: channel)
        stat.totalMinutes += minutes
        stat.lastListened = Date()
        channelStats[channel] = stat
    }

    private func recordHour(_ hour: Int) {
        var hourCounts = defaults.dictionary(forKey: hoursKey) as? [String: Int] ?? [:]
        let key = "\(hour)"
        hourCounts[key] = (hourCounts[key] ?? 0) + 1
        defaults.set(hourCounts, forKey: hoursKey)

        // Recalculate peak hour
        if let topKey = hourCounts.max(by: { $0.value < $1.value })?.key, let hour = Int(topKey) {
            peakHour = hour
        }
    }

    private func recordListeningDate(_ date: Date) {
        let calendar = Calendar.current
        let today = calendar.startOfDay(for: date)

        var dates = loadStreakDates()
        if !dates.contains(today) {
            dates.append(today)
            dates.sort()
            saveStreakDates(dates)
        }

        // Calculate current streak
        var streak = 1
        var checkDate = calendar.date(byAdding: .day, value: -1, to: today)!
        while dates.contains(checkDate) {
            streak += 1
            checkDate = calendar.date(byAdding: .day, value: -1, to: checkDate)!
        }
        currentStreak = streak
    }

    // MARK: - Persistence

    private func saveState() {
        let encoder = JSONEncoder()
        if let data = try? encoder.encode(channelStats) {
            defaults.set(data, forKey: statsKey)
        }
        defaults.set(totalListenMinutes, forKey: "com.soluna.analytics.totalMinutes")
    }

    private func loadState() {
        totalListenMinutes = defaults.double(forKey: "com.soluna.analytics.totalMinutes")

        if let data = defaults.data(forKey: statsKey) {
            let decoder = JSONDecoder()
            channelStats = (try? decoder.decode([String: ChannelStat].self, from: data)) ?? [:]
        }

        // Recalculate peak hour
        let hourCounts = defaults.dictionary(forKey: hoursKey) as? [String: Int] ?? [:]
        if let topKey = hourCounts.max(by: { $0.value < $1.value })?.key, let hour = Int(topKey) {
            peakHour = hour
        }

        // Recalculate streak
        let dates = loadStreakDates()
        if !dates.isEmpty {
            let calendar = Calendar.current
            let today = calendar.startOfDay(for: Date())
            if dates.contains(today) || dates.contains(calendar.date(byAdding: .day, value: -1, to: today)!) {
                var streak = 0
                var checkDate = today
                while dates.contains(checkDate) {
                    streak += 1
                    checkDate = calendar.date(byAdding: .day, value: -1, to: checkDate)!
                }
                currentStreak = streak
            }
        }
    }

    private func loadStreakDates() -> [Date] {
        guard let timestamps = defaults.array(forKey: streakKey) as? [TimeInterval] else { return [] }
        return timestamps.map { Date(timeIntervalSince1970: $0) }
    }

    private func saveStreakDates(_ dates: [Date]) {
        let timestamps = dates.map { $0.timeIntervalSince1970 }
        defaults.set(timestamps, forKey: streakKey)
    }
}
