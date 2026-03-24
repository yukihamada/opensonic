import Foundation
import Accelerate

/// Extracted watermark data from an audio stream.
public struct WatermarkPayload: Sendable {
    /// The user ID embedded in the watermark (32 bits).
    public let userId: UInt32
    /// The timestamp embedded in the watermark (32 bits, Unix epoch seconds).
    public let timestamp: UInt32
    /// Detection confidence (0.0 to 1.0).
    public let confidence: Float
}

/// Invisible audio watermarking for leak detection using spread-spectrum technique.
///
/// Embeds a 64-bit identifier (32-bit userId + 32-bit timestamp) into audio
/// streams by spreading the watermark across frequency bands using a pseudo-random
/// noise (PN) sequence. The watermark is imperceptible to listeners but can be
/// reliably extracted for forensic analysis.
///
/// Operates in the frequency domain using FFT (via Accelerate framework) to
/// modify spectral magnitudes below the audibility threshold.
///
/// Usage:
/// ```swift
/// let watermark = AudioWatermark(channelKey: 0xDEADBEEF)
///
/// // Embedding (transmitter side)
/// var samples = [Float](...)
/// watermark.embed(samples: &samples, userId: 12345, timestamp: UInt32(Date().timeIntervalSince1970))
///
/// // Extraction (receiver side)
/// if let payload = watermark.extract(samples: samples) {
///     print("User: \(payload.userId), Confidence: \(payload.confidence)")
/// }
/// ```
public final class AudioWatermark {

    // MARK: - Public Properties

    /// Whether watermark embedding is currently active.
    public private(set) var isEmbedding: Bool = false

    /// Whether watermark extraction is currently active.
    public private(set) var isExtracting: Bool = false

    // MARK: - Configuration

    /// FFT size (number of samples per frame). Must be a power of 2.
    public let fftSize: Int

    /// Watermark embedding strength. Lower values are less audible but harder to extract.
    public var embeddingStrength: Float = 0.005

    /// Minimum confidence threshold for valid extraction.
    public var extractionThreshold: Float = 0.3

    // MARK: - Private

    private let channelKey: UInt32
    private let log2n: vDSP_Length
    private let fftSetup: FFTSetup
    private let pnSequence: [Float]

    /// Number of bits in the watermark payload.
    private static let payloadBits = 64

    /// Number of frequency bins used per watermark bit for spreading.
    private static let binsPerBit = 4

    // MARK: - Init

    /// Create a watermark encoder/decoder.
    ///
    /// - Parameters:
    ///   - channelKey: Seed for the pseudo-random spreading sequence. Must match between embedder and extractor.
    ///   - fftSize: FFT frame size (default 2048, must be power of 2).
    public init(channelKey: UInt32, fftSize: Int = 2048) {
        precondition(fftSize > 0 && (fftSize & (fftSize - 1)) == 0, "fftSize must be a power of 2")
        precondition(fftSize / 2 >= Self.payloadBits * Self.binsPerBit, "fftSize too small for watermark payload")

        self.channelKey = channelKey
        self.fftSize = fftSize
        self.log2n = vDSP_Length(log2(Double(fftSize)))
        self.fftSetup = vDSP_create_fftsetup(log2n, FFTRadix(kFFTRadix2))!
        self.pnSequence = Self.generatePNSequence(seed: channelKey, length: Self.payloadBits * Self.binsPerBit)
    }

    deinit {
        vDSP_destroy_fftsetup(fftSetup)
    }

    // MARK: - Embed

    /// Embed a watermark into audio samples.
    ///
    /// Modifies the samples in-place by adding a spread-spectrum watermark
    /// in the frequency domain. The watermark contains the userId and timestamp
    /// encoded as 64 bits spread across multiple frequency bins.
    ///
    /// - Parameters:
    ///   - samples: Audio samples to watermark (modified in-place). Length should be >= fftSize.
    ///   - userId: 32-bit user identifier to embed.
    ///   - timestamp: 32-bit timestamp to embed (typically Unix epoch seconds).
    public func embed(samples: inout [Float], userId: UInt32, timestamp: UInt32) {
        isEmbedding = true
        defer { isEmbedding = false }

        guard samples.count >= fftSize else { return }

        // Encode payload as 64 bits: [userId: 32 bits][timestamp: 32 bits]
        let bits = encodeBits(userId: userId, timestamp: timestamp)

        // Process in frames
        var offset = 0
        while offset + fftSize <= samples.count {
            embedFrame(samples: &samples, offset: offset, bits: bits)
            offset += fftSize
        }
    }

    // MARK: - Extract

    /// Extract a watermark from audio samples.
    ///
    /// Correlates the received audio against the known PN sequence to recover
    /// the embedded userId and timestamp.
    ///
    /// - Parameter samples: Audio samples to analyze. Length should be >= fftSize.
    /// - Returns: The extracted watermark payload, or `nil` if confidence is below threshold.
    public func extract(samples: [Float]) -> WatermarkPayload? {
        isExtracting = true
        defer { isExtracting = false }

        guard samples.count >= fftSize else { return nil }

        // Accumulate bit correlations across frames
        var bitAccumulator = [Float](repeating: 0, count: Self.payloadBits)
        var frameCount: Float = 0

        var offset = 0
        while offset + fftSize <= samples.count {
            if let frameBits = extractFrame(samples: samples, offset: offset) {
                for i in 0..<Self.payloadBits {
                    bitAccumulator[i] += frameBits[i]
                }
                frameCount += 1
            }
            offset += fftSize
        }

        guard frameCount > 0 else { return nil }

        // Average and decide bits
        var decodedBits = [UInt8](repeating: 0, count: Self.payloadBits)
        var totalConfidence: Float = 0

        for i in 0..<Self.payloadBits {
            let avg = bitAccumulator[i] / frameCount
            decodedBits[i] = avg > 0 ? 1 : 0
            totalConfidence += abs(avg)
        }

        let confidence = totalConfidence / Float(Self.payloadBits)

        guard confidence >= extractionThreshold else { return nil }

        // Decode bits back to userId and timestamp
        let (userId, timestamp) = decodeBits(decodedBits)

        return WatermarkPayload(userId: userId, timestamp: timestamp, confidence: min(confidence, 1.0))
    }

    // MARK: - Private — FFT Frame Processing

    private func embedFrame(samples: inout [Float], offset: Int, bits: [UInt8]) {
        let halfN = fftSize / 2

        // Copy frame
        var realPart = [Float](repeating: 0, count: halfN)
        var imagPart = [Float](repeating: 0, count: halfN)

        // Prepare split complex for forward FFT
        var splitComplex = DSPSplitComplex(realp: &realPart, imagp: &imagPart)

        samples.withUnsafeBufferPointer { buf in
            let start = buf.baseAddress! + offset
            start.withMemoryRebound(to: DSPComplex.self, capacity: halfN) { complexPtr in
                vDSP_ctoz(complexPtr, 2, &splitComplex, 1, vDSP_Length(halfN))
            }
        }

        // Forward FFT
        vDSP_fft_zrip(fftSetup, &splitComplex, 1, log2n, FFTDirection(FFT_FORWARD))

        // Embed watermark bits into frequency bins
        let startBin = 20  // Skip DC and very low frequencies
        for bitIndex in 0..<Self.payloadBits {
            let bitValue: Float = bits[bitIndex] == 1 ? 1.0 : -1.0
            for spread in 0..<Self.binsPerBit {
                let binIndex = startBin + bitIndex * Self.binsPerBit + spread
                guard binIndex < halfN else { break }
                let pnIndex = bitIndex * Self.binsPerBit + spread
                let modification = embeddingStrength * bitValue * pnSequence[pnIndex]
                realPart[binIndex] += modification
            }
        }

        // Inverse FFT
        vDSP_fft_zrip(fftSetup, &splitComplex, 1, log2n, FFTDirection(FFT_INVERSE))

        // Scale and write back
        var scale = 1.0 / Float(fftSize * 2)
        var tempOut = [Float](repeating: 0, count: fftSize)
        tempOut.withUnsafeMutableBufferPointer { buf in
            buf.baseAddress!.withMemoryRebound(to: DSPComplex.self, capacity: halfN) { complexPtr in
                vDSP_ztoc(&splitComplex, 1, complexPtr, 2, vDSP_Length(halfN))
            }
        }

        vDSP_vsmul(tempOut, 1, &scale, &tempOut, 1, vDSP_Length(fftSize))

        for i in 0..<fftSize {
            samples[offset + i] = tempOut[i]
        }
    }

    private func extractFrame(samples: [Float], offset: Int) -> [Float]? {
        let halfN = fftSize / 2

        var realPart = [Float](repeating: 0, count: halfN)
        var imagPart = [Float](repeating: 0, count: halfN)
        var splitComplex = DSPSplitComplex(realp: &realPart, imagp: &imagPart)

        samples.withUnsafeBufferPointer { buf in
            let start = buf.baseAddress! + offset
            start.withMemoryRebound(to: DSPComplex.self, capacity: halfN) { complexPtr in
                vDSP_ctoz(complexPtr, 2, &splitComplex, 1, vDSP_Length(halfN))
            }
        }

        // Forward FFT
        vDSP_fft_zrip(fftSetup, &splitComplex, 1, log2n, FFTDirection(FFT_FORWARD))

        // Extract bits by correlating with PN sequence
        let startBin = 20
        var bitCorrelations = [Float](repeating: 0, count: Self.payloadBits)

        for bitIndex in 0..<Self.payloadBits {
            var correlation: Float = 0
            for spread in 0..<Self.binsPerBit {
                let binIndex = startBin + bitIndex * Self.binsPerBit + spread
                guard binIndex < halfN else { break }
                let pnIndex = bitIndex * Self.binsPerBit + spread
                correlation += realPart[binIndex] * pnSequence[pnIndex]
            }
            bitCorrelations[bitIndex] = correlation
        }

        return bitCorrelations
    }

    // MARK: - Private — Bit Encoding / Decoding

    private func encodeBits(userId: UInt32, timestamp: UInt32) -> [UInt8] {
        var bits = [UInt8](repeating: 0, count: Self.payloadBits)

        for i in 0..<32 {
            bits[i] = UInt8((userId >> i) & 1)
            bits[32 + i] = UInt8((timestamp >> i) & 1)
        }

        return bits
    }

    private func decodeBits(_ bits: [UInt8]) -> (userId: UInt32, timestamp: UInt32) {
        var userId: UInt32 = 0
        var timestamp: UInt32 = 0

        for i in 0..<32 {
            if bits[i] == 1 { userId |= (1 << i) }
            if bits[32 + i] == 1 { timestamp |= (1 << i) }
        }

        return (userId, timestamp)
    }

    // MARK: - Private — PN Sequence Generation

    /// Generate a pseudo-random noise sequence using a linear congruential generator.
    private static func generatePNSequence(seed: UInt32, length: Int) -> [Float] {
        var state = seed == 0 ? 1 : seed
        var sequence = [Float](repeating: 0, count: length)

        for i in 0..<length {
            // LCG: Numerical Recipes parameters
            state = state &* 1664525 &+ 1013904223
            // Map to +1 or -1
            sequence[i] = (state & 0x80000000) != 0 ? 1.0 : -1.0
        }

        return sequence
    }
}
