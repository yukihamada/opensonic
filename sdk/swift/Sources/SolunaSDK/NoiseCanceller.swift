import Foundation
import Accelerate
import Combine

/// AI-based noise cancellation using spectral subtraction.
///
/// Estimates the noise floor from low-energy (silent) frames and subtracts
/// the noise spectrum from the signal using a Wiener-style gain mask.
///
/// Uses a 512-point vDSP FFT with overlap-add reconstruction.
///
/// Usage:
/// ```swift
/// let nc = NoiseCanceller()
/// nc.isEnabled = true
/// nc.aggressiveness = 0.7
/// nc.process(samples: &audioBuffer)
/// ```
public final class NoiseCanceller: ObservableObject {

    // MARK: - Published State

    /// Whether noise cancellation is active.
    @Published public var isEnabled: Bool = false

    /// How aggressively noise is removed (0.0 = gentle, 1.0 = maximum).
    @Published public var aggressiveness: Float = 0.5

    /// Estimated noise floor in dB.
    @Published public private(set) var estimatedNoiseFloor: Float = -60.0

    /// Current noise reduction amount in dB.
    @Published public private(set) var noiseReductionDb: Float = 0.0

    // MARK: - Private

    private let fftSize = 512
    private let hopSize = 256 // 50% overlap
    private var fftSetup: vDSP_DFT_Setup?

    /// Running noise spectrum estimate (magnitude per bin).
    private var noiseSpectrum: [Float]

    /// Overlap-add output buffer for reconstruction.
    private var overlapBuffer: [Float]

    /// Input accumulation buffer for overlap processing.
    private var inputBuffer: [Float] = []

    /// Number of frames used for noise estimation.
    private var noiseEstimationCount: Int = 0

    /// Smoothing factor for noise estimate update.
    private let noiseSmoothing: Float = 0.98

    /// Minimum energy threshold to consider a frame as "silent" for noise estimation.
    private var silenceThreshold: Float = 0.005

    /// Hann window (precomputed).
    private let window: [Float]

    // MARK: - Init

    public init() {
        let halfSize = fftSize / 2
        noiseSpectrum = [Float](repeating: 0, count: halfSize)
        overlapBuffer = [Float](repeating: 0, count: hopSize)

        var win = [Float](repeating: 0, count: fftSize)
        vDSP_hann_window(&win, vDSP_Length(fftSize), Int32(vDSP_HANN_NORM))
        window = win

        fftSetup = vDSP_DFT_zop_CreateSetup(
            nil,
            vDSP_Length(fftSize),
            .FORWARD
        )
    }

    deinit {
        if let setup = fftSetup {
            vDSP_DFT_DestroySetup(setup)
        }
    }

    // MARK: - Public API

    /// Process audio samples in-place, applying noise reduction.
    ///
    /// - Parameter samples: Audio samples to process (modified in-place).
    public func process(samples: inout [Float]) {
        guard isEnabled, !samples.isEmpty, let fftSetup else { return }

        inputBuffer.append(contentsOf: samples)

        var outputSamples: [Float] = []

        while inputBuffer.count >= fftSize {
            let frame = Array(inputBuffer.prefix(fftSize))
            let processed = processFrame(frame, fftSetup: fftSetup)
            outputSamples.append(contentsOf: processed)
            inputBuffer.removeFirst(hopSize)
        }

        if outputSamples.count >= samples.count {
            samples = Array(outputSamples.prefix(samples.count))
        } else if !outputSamples.isEmpty {
            for i in 0..<outputSamples.count {
                samples[i] = outputSamples[i]
            }
        }
    }

    // MARK: - Private

    private func processFrame(_ frame: [Float], fftSetup: vDSP_DFT_Setup) -> [Float] {
        let halfSize = fftSize / 2

        // Apply window
        var windowed = [Float](repeating: 0, count: fftSize)
        vDSP_vmul(frame, 1, window, 1, &windowed, 1, vDSP_Length(fftSize))

        // Forward FFT
        var realIn = windowed
        var imagIn = [Float](repeating: 0, count: fftSize)
        var realOut = [Float](repeating: 0, count: fftSize)
        var imagOut = [Float](repeating: 0, count: fftSize)

        vDSP_DFT_Execute(fftSetup, &realIn, &imagIn, &realOut, &imagOut)

        // Compute magnitude spectrum
        var magnitudes = [Float](repeating: 0, count: halfSize)
        for i in 0..<halfSize {
            magnitudes[i] = sqrtf(realOut[i] * realOut[i] + imagOut[i] * imagOut[i])
        }

        // Check if this is a silent frame for noise estimation
        var rms: Float = 0
        vDSP_rmsqv(frame, 1, &rms, vDSP_Length(fftSize))

        if rms < silenceThreshold {
            // Update noise estimate with exponential smoothing
            if noiseEstimationCount == 0 {
                noiseSpectrum = magnitudes
            } else {
                for i in 0..<halfSize {
                    noiseSpectrum[i] = noiseSmoothing * noiseSpectrum[i] + (1.0 - noiseSmoothing) * magnitudes[i]
                }
            }
            noiseEstimationCount += 1
        }

        // Spectral subtraction with Wiener-style gain
        var gain = [Float](repeating: 1.0, count: halfSize)
        let alpha = 1.0 + aggressiveness * 3.0 // Over-subtraction factor (1.0 - 4.0)
        let beta: Float = 0.01 + (1.0 - aggressiveness) * 0.05 // Spectral floor

        var totalNoiseReduction: Float = 0

        for i in 0..<halfSize {
            let signalPower = magnitudes[i] * magnitudes[i]
            let noisePower = noiseSpectrum[i] * noiseSpectrum[i] * alpha

            if signalPower > 0 {
                let snr = max(signalPower - noisePower, 0) / signalPower
                gain[i] = max(snr, beta)
                totalNoiseReduction += (1.0 - gain[i])
            }
        }

        // Apply gain mask to FFT output
        for i in 0..<halfSize {
            realOut[i] *= gain[i]
            imagOut[i] *= gain[i]
            // Mirror for negative frequencies
            if i > 0 && i < halfSize {
                realOut[fftSize - i] *= gain[i]
                imagOut[fftSize - i] *= gain[i]
            }
        }

        // Inverse FFT (using forward DFT with conjugate trick)
        var invRealOut = [Float](repeating: 0, count: fftSize)
        var invImagOut = [Float](repeating: 0, count: fftSize)

        // Conjugate
        var negImagOut = imagOut
        var negOne: Float = -1.0
        vDSP_vsmul(negImagOut, 1, &negOne, &negImagOut, 1, vDSP_Length(fftSize))

        if let inverseSetup = vDSP_DFT_zop_CreateSetup(nil, vDSP_Length(fftSize), .FORWARD) {
            vDSP_DFT_Execute(inverseSetup, &realOut, &negImagOut, &invRealOut, &invImagOut)
            vDSP_DFT_DestroySetup(inverseSetup)
        }

        // Normalize
        var scale = 1.0 / Float(fftSize)
        vDSP_vsmul(invRealOut, 1, &scale, &invRealOut, 1, vDSP_Length(fftSize))

        // Apply window again for overlap-add
        var output = [Float](repeating: 0, count: fftSize)
        vDSP_vmul(invRealOut, 1, window, 1, &output, 1, vDSP_Length(fftSize))

        // Overlap-add: combine with previous overlap
        var result = [Float](repeating: 0, count: hopSize)
        for i in 0..<hopSize {
            result[i] = output[i] + overlapBuffer[i]
        }
        overlapBuffer = Array(output[hopSize..<fftSize])

        // Update published metrics
        let avgReduction = halfSize > 0 ? totalNoiseReduction / Float(halfSize) : 0
        let noiseFloor = noiseSpectrum.reduce(0, +) / Float(max(halfSize, 1))
        let noiseFloorDb = noiseFloor > 0 ? 20.0 * log10f(noiseFloor) : -60.0

        DispatchQueue.main.async { [weak self] in
            self?.estimatedNoiseFloor = noiseFloorDb
            self?.noiseReductionDb = avgReduction * 30.0 // Approximate dB
        }

        return result
    }
}
