import Foundation
import Accelerate
import Combine

/// Acoustic echo cancellation using NLMS (Normalized Least Mean Squares) adaptive filter.
///
/// Cancels speaker output from microphone input to prevent feedback and howling.
/// The reference signal (what the speaker is playing) is used to estimate and
/// subtract the echo component from the microphone signal.
///
/// Usage:
/// ```swift
/// let aec = EchoCanceller()
/// aec.isEnabled = true
/// aec.process(micSamples: &micBuffer, referenceSamples: speakerBuffer)
/// ```
public final class EchoCanceller: ObservableObject {

    // MARK: - Published State

    /// Whether echo cancellation is active.
    @Published public var isEnabled: Bool = false

    /// Adaptive filter convergence level (0.0 = not converged, 1.0 = fully converged).
    @Published public private(set) var convergenceLevel: Float = 0.0

    // MARK: - Configuration

    /// Adaptive filter length in taps (default 2048 = ~42ms at 48kHz).
    public var filterLength: Int {
        didSet {
            if filterLength != oldValue {
                resetFilter()
            }
        }
    }

    /// NLMS step size (mu). Controls adaptation speed vs stability (default 0.5).
    public var stepSize: Float = 0.5

    // MARK: - Private

    /// Adaptive filter coefficients.
    private var filterCoeffs: [Float]

    /// Circular buffer for reference (speaker) samples.
    private var referenceBuffer: [Float]

    /// Write index into the circular reference buffer.
    private var refWriteIndex: Int = 0

    /// Regularization constant to prevent division by zero.
    private let epsilon: Float = 1.0e-8

    /// Running error energy for convergence tracking.
    private var errorEnergy: Float = 0.0

    /// Running reference energy for convergence tracking.
    private var referenceEnergy: Float = 0.0

    /// Smoothing factor for convergence metric.
    private let convergenceSmoothing: Float = 0.99

    // MARK: - Init

    public init(filterLength: Int = 2048) {
        self.filterLength = filterLength
        self.filterCoeffs = [Float](repeating: 0, count: filterLength)
        self.referenceBuffer = [Float](repeating: 0, count: filterLength)
    }

    // MARK: - Public API

    /// Process microphone samples in-place, cancelling echo from the reference signal.
    ///
    /// - Parameters:
    ///   - micSamples: Microphone input samples (modified in-place to remove echo).
    ///   - referenceSamples: Speaker output samples used as the echo reference.
    public func process(micSamples: inout [Float], referenceSamples: [Float]) {
        guard isEnabled, !micSamples.isEmpty else { return }

        let len = min(micSamples.count, referenceSamples.count)
        var totalError: Float = 0
        var totalRef: Float = 0

        for i in 0..<len {
            // Store reference sample in circular buffer
            referenceBuffer[refWriteIndex] = referenceSamples[i]
            refWriteIndex = (refWriteIndex + 1) % filterLength

            // Build the reference vector x[n] (most recent filterLength samples)
            var x = [Float](repeating: 0, count: filterLength)
            for j in 0..<filterLength {
                let idx = (refWriteIndex - 1 - j + filterLength * 2) % filterLength
                x[j] = referenceBuffer[idx]
            }

            // Compute estimated echo: y_hat = w^T * x
            var yHat: Float = 0
            vDSP_dotpr(filterCoeffs, 1, x, 1, &yHat, vDSP_Length(filterLength))

            // Error signal: e[n] = d[n] - y_hat (mic minus estimated echo)
            let error = micSamples[i] - yHat

            // Compute x^T * x (reference power)
            var xPower: Float = 0
            vDSP_dotpr(x, 1, x, 1, &xPower, vDSP_Length(filterLength))

            // NLMS update: w[n+1] = w[n] + mu * e[n] * x[n] / (x^T*x + eps)
            let scale = stepSize * error / (xPower + epsilon)
            for j in 0..<filterLength {
                filterCoeffs[j] += scale * x[j]
            }

            // Output the error signal (echo-cancelled mic)
            micSamples[i] = error

            totalError += error * error
            totalRef += referenceSamples[i] * referenceSamples[i]
        }

        // Update convergence metric
        errorEnergy = convergenceSmoothing * errorEnergy + (1.0 - convergenceSmoothing) * totalError / Float(max(len, 1))
        referenceEnergy = convergenceSmoothing * referenceEnergy + (1.0 - convergenceSmoothing) * totalRef / Float(max(len, 1))

        let convergence: Float
        if referenceEnergy > epsilon {
            // ERLE-based convergence: higher reduction = more converged
            let erle = referenceEnergy / (errorEnergy + epsilon)
            convergence = min(1.0 - 1.0 / (1.0 + erle * 0.1), 1.0)
        } else {
            convergence = 0.0
        }

        DispatchQueue.main.async { [weak self] in
            self?.convergenceLevel = max(0, convergence)
        }
    }

    // MARK: - Private

    private func resetFilter() {
        filterCoeffs = [Float](repeating: 0, count: filterLength)
        referenceBuffer = [Float](repeating: 0, count: filterLength)
        refWriteIndex = 0
        errorEnergy = 0
        referenceEnergy = 0
    }
}
