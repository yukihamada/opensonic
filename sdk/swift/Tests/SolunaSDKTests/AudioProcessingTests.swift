import XCTest
@testable import SolunaSDK

final class AudioProcessingTests: XCTestCase {

    // MARK: - AutoGainControl

    func testAGCQuietSignalGetsBoosted() {
        let agc = AutoGainControl()
        agc.isEnabled = true
        agc.targetLevelDb = -18.0
        agc.maxGainDb = 30.0

        // Very quiet signal (amplitude ~0.001)
        var samples = [Float](repeating: 0.001, count: 4800)
        let originalRMS = rms(samples)

        agc.process(samples: &samples)

        let boostedRMS = rms(samples)
        XCTAssertGreaterThan(boostedRMS, originalRMS, "Quiet signal should be boosted")
    }

    func testAGCLoudSignalGetsReduced() {
        let agc = AutoGainControl()
        agc.isEnabled = true
        agc.targetLevelDb = -18.0
        agc.maxGainDb = 30.0

        // Loud signal (amplitude ~0.9)
        var samples = [Float](repeating: 0.9, count: 4800)
        let originalRMS = rms(samples)

        agc.process(samples: &samples)

        let processedRMS = rms(samples)
        XCTAssertLessThan(processedRMS, originalRMS, "Loud signal should be reduced")
    }

    func testAGCDisabledDoesNothing() {
        let agc = AutoGainControl()
        agc.isEnabled = false

        var samples: [Float] = [0.5, -0.5, 0.3, -0.3]
        let original = samples

        agc.process(samples: &samples)

        XCTAssertEqual(samples, original)
    }

    func testAGCEmptySamples() {
        let agc = AutoGainControl()
        agc.isEnabled = true
        var samples: [Float] = []
        agc.process(samples: &samples)
        XCTAssertTrue(samples.isEmpty)
    }

    // MARK: - NoiseCanceller

    func testNoiseCancellerDisabledDoesNothing() {
        let nc = NoiseCanceller()
        nc.isEnabled = false

        var samples: [Float] = [0.1, 0.2, 0.3, 0.4]
        let original = samples
        nc.process(samples: &samples)
        XCTAssertEqual(samples, original)
    }

    func testNoiseCancellerProcessesWithoutCrash() {
        let nc = NoiseCanceller()
        nc.isEnabled = true
        nc.aggressiveness = 0.7

        // Feed several frames worth of data (need at least 512 for FFT)
        var samples = [Float](repeating: 0, count: 1024)
        // Add some noise
        for i in 0..<samples.count {
            samples[i] = Float.random(in: -0.01...0.01)
        }

        // Should not crash
        nc.process(samples: &samples)
    }

    // MARK: - EchoCanceller

    func testEchoCancellerDisabledDoesNothing() {
        let aec = EchoCanceller()
        aec.isEnabled = false

        var micSamples: [Float] = [0.5, -0.5, 0.3, -0.3]
        let reference: [Float] = [0.1, 0.2, 0.3, 0.4]
        let original = micSamples

        aec.process(micSamples: &micSamples, referenceSamples: reference)
        XCTAssertEqual(micSamples, original)
    }

    func testEchoCancellerSubtractsEcho() {
        let aec = EchoCanceller(filterLength: 64)
        aec.isEnabled = true
        aec.stepSize = 0.5

        // Create a reference signal
        let reference = (0..<256).map { Float(sin(Double($0) * 0.1)) * 0.5 }
        // Mic = reference + some original speech
        var micSamples = reference.map { $0 + Float.random(in: -0.01...0.01) }

        let micRMSBefore = rms(micSamples)

        // Run multiple iterations to let the filter converge
        for _ in 0..<10 {
            var mic = reference.map { $0 + Float.random(in: -0.01...0.01) }
            aec.process(micSamples: &mic, referenceSamples: reference)
            micSamples = mic
        }

        let micRMSAfter = rms(micSamples)
        // After convergence, the echo should be partially cancelled
        XCTAssertLessThan(micRMSAfter, micRMSBefore, "Echo canceller should reduce echo energy")
    }

    func testEchoCancellerEmptySamples() {
        let aec = EchoCanceller()
        aec.isEnabled = true
        var mic: [Float] = []
        aec.process(micSamples: &mic, referenceSamples: [])
        XCTAssertTrue(mic.isEmpty)
    }

    // MARK: - Helpers

    private func rms(_ samples: [Float]) -> Float {
        guard !samples.isEmpty else { return 0 }
        let sumSquares = samples.reduce(Float(0)) { $0 + $1 * $1 }
        return sqrtf(sumSquares / Float(samples.count))
    }
}
