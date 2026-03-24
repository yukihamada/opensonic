import XCTest
@testable import SolunaSDK

final class LocalizationTests: XCTestCase {

    func testEnglishHasBasicKeys() {
        let l10n = SolunaLocalization()
        l10n.setLanguage("en")

        let requiredKeys = [
            "connected", "connecting", "disconnected", "error",
            "channel", "listeners", "now_playing", "mute", "unmute",
            "play", "stop", "settings", "volume", "quality"
        ]

        for key in requiredKeys {
            let value = l10n.localized(key)
            XCTAssertNotEqual(value, key, "English should have translation for '\(key)'")
        }
    }

    func testJapaneseHasBasicKeys() {
        let l10n = SolunaLocalization()
        l10n.setLanguage("ja")

        XCTAssertEqual(l10n.localized("connected"), "\u{63a5}\u{7d9a}\u{6e08}\u{307f}")
        XCTAssertEqual(l10n.localized("play"), "\u{518d}\u{751f}")
    }

    func testAllSupportedLanguagesHaveConnectedKey() {
        let l10n = SolunaLocalization()
        let languages = l10n.supportedLanguages

        // All 30 languages should be present
        XCTAssertEqual(languages.count, 30)

        for lang in languages {
            l10n.setLanguage(lang.code)
            let connected = l10n.localized("connected")
            XCTAssertNotEqual(connected, "connected",
                "Language '\(lang.code)' (\(lang.name)) should have 'connected' key, got raw key back")
        }
    }

    func testFallbackToEnglish() {
        let l10n = SolunaLocalization()
        l10n.setLanguage("ms")  // Malay has fewer keys

        // "now_playing" is not in Malay table, should fall back to English
        let nowPlaying = l10n.localized("now_playing")
        XCTAssertEqual(nowPlaying, "Now Playing")
    }

    func testFallbackToKeyWhenNotFoundAnywhere() {
        let l10n = SolunaLocalization()
        l10n.setLanguage("en")

        let result = l10n.localized("nonexistent_key_12345")
        XCTAssertEqual(result, "nonexistent_key_12345")
    }

    func testRTLDetectionForArabic() {
        let l10n = SolunaLocalization()
        l10n.setLanguage("ar")
        XCTAssertTrue(l10n.isRTL)
    }

    func testRTLDetectionForHebrew() {
        let l10n = SolunaLocalization()
        l10n.setLanguage("he")
        XCTAssertTrue(l10n.isRTL)
    }

    func testNonRTLLanguages() {
        let l10n = SolunaLocalization()

        for code in ["en", "ja", "ko", "de", "fr", "zh-Hans"] {
            l10n.setLanguage(code)
            XCTAssertFalse(l10n.isRTL, "\(code) should not be RTL")
        }
    }

    func testSetLanguageUpdatesCurrentLanguage() {
        let l10n = SolunaLocalization()
        l10n.setLanguage("fr")
        XCTAssertEqual(l10n.currentLanguage, "fr")

        l10n.setLanguage("ja")
        XCTAssertEqual(l10n.currentLanguage, "ja")
    }

    func testSupportedLanguagesAreSorted() {
        let l10n = SolunaLocalization()
        let codes = l10n.supportedLanguages.map { $0.code }
        XCTAssertEqual(codes, codes.sorted())
    }

    func testLocaleInfoRTLFlag() {
        let l10n = SolunaLocalization()
        let arabicInfo = l10n.supportedLanguages.first { $0.code == "ar" }
        XCTAssertNotNil(arabicInfo)
        XCTAssertTrue(arabicInfo!.isRTL)

        let englishInfo = l10n.supportedLanguages.first { $0.code == "en" }
        XCTAssertNotNil(englishInfo)
        XCTAssertFalse(englishInfo!.isRTL)
    }
}
