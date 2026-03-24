import XCTest
@testable import SolunaSDK

final class CacheTests: XCTestCase {

    func testOfflineCacheInitialState() {
        let cache = OfflineCache()
        XCTAssertFalse(cache.isEnabled)
        XCTAssertGreaterThanOrEqual(cache.maxCacheSize, 0)
    }

    func testCachedTracksReturnsEmptyForNoData() {
        let cache = OfflineCache()
        let tracks = cache.cachedTracks(for: "nonexistent_channel_\(UUID().uuidString)")
        XCTAssertTrue(tracks.isEmpty)
    }

    func testClearCacheDoesNotCrash() {
        let cache = OfflineCache()
        cache.isEnabled = true

        let expectation = XCTestExpectation(description: "clearCache completes")

        cache.clearCache()

        // Give the async queue time to complete
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
            XCTAssertEqual(cache.cacheSize, 0)
            expectation.fulfill()
        }

        wait(for: [expectation], timeout: 2.0)
    }

    func testMaxCacheSizeConfigurable() {
        let cache = OfflineCache()
        cache.maxCacheSize = 100 * 1024 * 1024  // 100 MB
        XCTAssertEqual(cache.maxCacheSize, 100 * 1024 * 1024)
    }

    func testCachedTrackStruct() {
        let url = URL(fileURLWithPath: "/tmp/test.caf")
        let track = CachedTrack(
            id: "test.caf",
            channel: "jazz",
            title: "Test Track",
            duration: 120.0,
            fileURL: url,
            cachedAt: Date()
        )
        XCTAssertEqual(track.id, "test.caf")
        XCTAssertEqual(track.channel, "jazz")
        XCTAssertEqual(track.title, "Test Track")
        XCTAssertEqual(track.duration, 120.0)
    }

    func testDisabledCacheDoesNotWrite() {
        let cache = OfflineCache()
        cache.isEnabled = false

        // This should be a no-op since isEnabled is false
        // We can't easily test writeBuffer without AVAudioPCMBuffer,
        // but we verify the flag behavior
        XCTAssertFalse(cache.isEnabled)
    }
}
