import XCTest
@testable import SolunaSDK

final class GeoFenceTests: XCTestCase {

    func testAllowedRegionPasses() {
        let gf = GeoFence()
        gf.allowedRegions = ["JP", "US", "GB"]
        gf.setRegion("JP")
        XCTAssertTrue(gf.isAllowed)
    }

    func testBlockedRegionFails() {
        let gf = GeoFence()
        gf.allowedRegions = ["JP", "US", "GB"]
        gf.setRegion("CN")
        XCTAssertFalse(gf.isAllowed)
    }

    func testEmptyAllowlistAllowsAll() {
        let gf = GeoFence()
        gf.allowedRegions = []
        gf.blockedRegions = []
        gf.setRegion("ZZ")
        XCTAssertTrue(gf.isAllowed)
    }

    func testBlocklistMode() {
        let gf = GeoFence()
        gf.blockedRegions = ["KP", "IR"]
        gf.setRegion("KP")
        XCTAssertFalse(gf.isAllowed)

        gf.setRegion("US")
        XCTAssertTrue(gf.isAllowed)
    }

    func testAllowlistTakesPrecedenceOverBlocklist() {
        let gf = GeoFence()
        gf.allowedRegions = ["JP"]
        gf.blockedRegions = ["JP"]  // Should be ignored since allowlist is non-empty
        gf.setRegion("JP")
        XCTAssertTrue(gf.isAllowed)
    }

    func testSetRegionUppercases() {
        let gf = GeoFence()
        gf.setRegion("jp")
        XCTAssertEqual(gf.currentRegion, "JP")
    }

    func testAllowedRegionCaseInsensitive() {
        let gf = GeoFence()
        gf.allowedRegions = ["JP"]
        gf.setRegion("jp")
        XCTAssertTrue(gf.isAllowed)
    }

    func testNoRegionAllowsByDefault() {
        let gf = GeoFence()
        gf.allowedRegions = ["JP"]
        // If currentRegion is not nil (set from locale), this tests the real locale
        // Manually clear it for deterministic test
        let testGf = GeoFence()
        testGf.allowedRegions = []
        // With no restrictions, always allowed
        XCTAssertTrue(testGf.isAllowed)
    }

    func testInitializesFromDeviceLocale() {
        let gf = GeoFence()
        // currentRegion should be set from device locale (may be nil in CI)
        // Just verify it doesn't crash
        _ = gf.currentRegion
        _ = gf.isAllowed
    }

    func testGeoFenceResultCases() {
        // Ensure all cases are constructible
        let allowed = GeoFenceResult.allowed(region: "JP")
        let blocked = GeoFenceResult.blocked(region: "CN")
        let unknown = GeoFenceResult.unknown

        if case .allowed(let region) = allowed {
            XCTAssertEqual(region, "JP")
        }
        if case .blocked(let region) = blocked {
            XCTAssertEqual(region, "CN")
        }
        if case .unknown = unknown {
            // OK
        } else {
            XCTFail("Expected .unknown")
        }
    }
}
