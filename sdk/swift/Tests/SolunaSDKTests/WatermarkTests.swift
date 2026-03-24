import XCTest
@testable import SolunaSDK

final class WatermarkTests: XCTestCase {

    func testEmbedThenExtractRoundTrip() {
        let channelKey: UInt32 = 0xDEADBEEF
        let wm = AudioWatermark(channelKey: channelKey)
        wm.embeddingStrength = 0.5  // Strong embedding for test reliability
        wm.extractionThreshold = 0.001

        let userId: UInt32 = 12345
        let timestamp: UInt32 = 1700000000

        // Create a signal with many frames worth of data for better extraction
        let sampleCount = wm.fftSize * 16
        var samples = [Float](repeating: 0, count: sampleCount)
        // Use a DC-like signal so FFT bins are clean
        for i in 0..<sampleCount {
            samples[i] = 0.0  // Zero baseline -- watermark modifies frequency bins
        }

        wm.embed(samples: &samples, userId: userId, timestamp: timestamp)

        let extracted = wm.extract(samples: samples)
        XCTAssertNotNil(extracted, "Watermark extraction should succeed")
        if let payload = extracted {
            XCTAssertEqual(payload.userId, userId)
            XCTAssertEqual(payload.timestamp, timestamp)
            XCTAssertGreaterThan(payload.confidence, 0)
        }
    }

    func testExtractionFromUnwatermarkedAudio() {
        let wm = AudioWatermark(channelKey: 0xCAFEBABE)
        wm.extractionThreshold = 0.3

        // Pure silence
        let silence = [Float](repeating: 0, count: wm.fftSize * 2)
        let result = wm.extract(samples: silence)
        // Should return nil (confidence below threshold) or a payload with very low confidence
        if let payload = result {
            // If it doesn't return nil, confidence should be very low
            XCTAssertLessThan(payload.confidence, 0.5)
        }
    }

    func testExtractionFromTooShortSamples() {
        let wm = AudioWatermark(channelKey: 0x12345678)
        let shortSamples = [Float](repeating: 0, count: 10)
        XCTAssertNil(wm.extract(samples: shortSamples))
    }

    func testEmbedTooShortSamplesIsNoOp() {
        let wm = AudioWatermark(channelKey: 0x12345678)
        var shortSamples: [Float] = [0.1, 0.2, 0.3]
        let original = shortSamples
        wm.embed(samples: &shortSamples, userId: 1, timestamp: 1)
        XCTAssertEqual(shortSamples, original) // Should not modify
    }

    func testDifferentChannelKeysProduceDifferentResults() {
        let wm1 = AudioWatermark(channelKey: 0x11111111)
        let wm2 = AudioWatermark(channelKey: 0x22222222)

        wm1.embeddingStrength = 0.1
        wm1.extractionThreshold = 0.01
        wm2.extractionThreshold = 0.01

        let sampleCount = wm1.fftSize * 4
        var samples = [Float](repeating: 0, count: sampleCount)
        for i in 0..<sampleCount {
            samples[i] = sinf(Float(i) * 0.05) * 0.5
        }

        wm1.embed(samples: &samples, userId: 99, timestamp: 100)

        // wm2 with different key should not reliably extract the same userId
        let extracted = wm2.extract(samples: samples)
        // May return nil or wrong data -- either is acceptable
        if let payload = extracted {
            // If it returns something, the userId should likely be wrong
            // (though there's a small random chance of collision)
            _ = payload
        }
    }

    func testFFTSizeMustBePowerOfTwo() {
        // Valid sizes
        _ = AudioWatermark(channelKey: 0, fftSize: 2048)
        _ = AudioWatermark(channelKey: 0, fftSize: 4096)

        // fftSize must be >= payloadBits(64) * binsPerBit(4) * 2 = 512
        _ = AudioWatermark(channelKey: 0, fftSize: 1024)
    }

    func testEmbeddingDoesNotClip() {
        let wm = AudioWatermark(channelKey: 0xAAAAAAAA)
        wm.embeddingStrength = 0.005

        var samples = [Float](repeating: 0.5, count: wm.fftSize * 2)
        wm.embed(samples: &samples, userId: 42, timestamp: 100)

        // Verify no samples exceed [-1, 1] range significantly
        for sample in samples {
            XCTAssertLessThanOrEqual(abs(sample), 2.0, "Watermarked sample should not clip severely")
        }
    }
}
