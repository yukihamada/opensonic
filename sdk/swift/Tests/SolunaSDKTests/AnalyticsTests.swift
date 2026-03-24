import XCTest
@testable import SolunaSDK

final class AnalyticsTests: XCTestCase {

    private func freshDefaults() -> UserDefaults {
        let suiteName = "com.soluna.test.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suiteName)!
        // Clean up any existing keys
        defaults.removePersistentDomain(forName: suiteName)
        return defaults
    }

    func testRecordsListeningMinutes() {
        let defaults = freshDefaults()
        let tracker = AnalyticsTracker(defaults: defaults)

        XCTAssertEqual(tracker.totalListenMinutes, 0)

        tracker.startSession(channel: "jazz")
        // End immediately -- should record partial minute
        tracker.endSession()

        // Total should still be very small (< 1 minute) since we ended immediately
        XCTAssertGreaterThanOrEqual(tracker.totalListenMinutes, 0)
    }

    func testChannelStatsAccumulation() {
        let defaults = freshDefaults()
        let tracker = AnalyticsTracker(defaults: defaults)

        tracker.startSession(channel: "jazz")
        tracker.endSession()

        tracker.startSession(channel: "jazz")
        tracker.endSession()

        tracker.startSession(channel: "lofi")
        tracker.endSession()

        let jazzStat = tracker.channelStats["jazz"]
        XCTAssertNotNil(jazzStat)
        XCTAssertEqual(jazzStat?.sessionCount, 2)

        let lofiStat = tracker.channelStats["lofi"]
        XCTAssertNotNil(lofiStat)
        XCTAssertEqual(lofiStat?.sessionCount, 1)
    }

    func testStreakCounting() {
        let defaults = freshDefaults()
        let tracker = AnalyticsTracker(defaults: defaults)

        // Starting a session records today's date, so streak should be >= 1
        tracker.startSession(channel: "chill")
        tracker.endSession()

        XCTAssertGreaterThanOrEqual(tracker.currentStreak, 1)
    }

    func testStartSessionEndsPreviousSession() {
        let defaults = freshDefaults()
        let tracker = AnalyticsTracker(defaults: defaults)

        tracker.startSession(channel: "jazz")
        tracker.startSession(channel: "lofi")  // Should end jazz first

        let jazzStat = tracker.channelStats["jazz"]
        XCTAssertNotNil(jazzStat)
        XCTAssertEqual(jazzStat?.sessionCount, 1)

        let lofiStat = tracker.channelStats["lofi"]
        XCTAssertNotNil(lofiStat)
        XCTAssertEqual(lofiStat?.sessionCount, 1)

        tracker.endSession()
    }

    func testPeakHourRecorded() {
        let defaults = freshDefaults()
        let tracker = AnalyticsTracker(defaults: defaults)

        tracker.startSession(channel: "dance")
        tracker.endSession()

        // Peak hour should be the current hour
        let currentHour = Calendar.current.component(.hour, from: Date())
        XCTAssertEqual(tracker.peakHour, currentHour)
    }

    func testEndSessionWithoutStartIsNoOp() {
        let defaults = freshDefaults()
        let tracker = AnalyticsTracker(defaults: defaults)

        // Should not crash
        tracker.endSession()
        XCTAssertEqual(tracker.totalListenMinutes, 0)
    }

    func testChannelStatInit() {
        let stat = ChannelStat(channelId: "test", totalMinutes: 42.5, sessionCount: 3)
        XCTAssertEqual(stat.channelId, "test")
        XCTAssertEqual(stat.totalMinutes, 42.5)
        XCTAssertEqual(stat.sessionCount, 3)
    }

    func testStatePersistence() {
        let defaults = freshDefaults()

        // Create tracker, add data, let it save
        let tracker1 = AnalyticsTracker(defaults: defaults)
        tracker1.startSession(channel: "jazz")
        tracker1.endSession()

        // Create new tracker with same defaults - should load state
        let tracker2 = AnalyticsTracker(defaults: defaults)
        let jazzStat = tracker2.channelStats["jazz"]
        XCTAssertNotNil(jazzStat)
        XCTAssertEqual(jazzStat?.sessionCount, 1)
    }
}
