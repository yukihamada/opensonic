import Foundation
import Accelerate
import Combine

/// Automatic gain control for consistent audio volume across sources.
///
/// Uses an RMS-based envelope follower with configurable attack/release times
/// to smoothly adjust gain, normalizing audio levels to a target dB level.
///
/// Usage:
/// ```swift
/// let agc = AutoGainControl()
/// agc.targetLevelDb = -18.0
/// agc.process(samples: &audioBuffer)
/// ```
public final class AutoGainControl: ObservableObject {

    // MARK: - Published State

    /// Whether automatic gain control is active.
    @Published public var isEnabled: Bool = false

    /// Target output level in dB (default -18.0 dB).
    @Published public var targetLevelDb: Float = -18.0

    /// Maximum gain that can be applied in dB (default 30.0 dB).
    @Published public var maxGainDb: Float = 30.0

    /// Attack time in milliseconds — how fast gain decreases (default 10.0 ms).
    @Published public var attackMs: Float = 10.0

    /// Release time in milliseconds — how fast gain increases (default 100.0 ms).
    @Published public var releaseMs: Float = 100.0

    /// Current gain being applied in dB.
    @Published public private(set) var currentGainDb: Float = 0.0

    // MARK: - Private

    /// Current envelope level (linear).
    private var envelope: Float = 0.0

    /// Sample rate for time constant calculations.
    private let sampleRate: Float = 48000.0

    // MARK: - Init

    public init() {}

    // MARK: - Public API

    /// Process audio samples in-place, applying automatic gain control.
    ///
    /// - Parameter samples: Audio samples to process (modified in-place).
    public func process(samples: inout [Float]) {
        guard isEnabled, !samples.isEmpty else { return }

        let targetLinear = powf(10.0, targetLevelDb / 20.0)
        let maxGainLinear = powf(10.0, maxGainDb / 20.0)

        // Compute time constants from ms to per-sample coefficients
        let attackCoeff = expf(-1.0 / (attackMs * 0.001 * sampleRate))
        let releaseCoeff = expf(-1.0 / (releaseMs * 0.001 * sampleRate))

        var appliedGainDb: Float = 0.0

        for i in 0..<samples.count {
            let absSample = abs(samples[i])

            // Envelope follower with separate attack/release
            if absSample > envelope {
                envelope = attackCoeff * envelope + (1.0 - attackCoeff) * absSample
            } else {
                envelope = releaseCoeff * envelope + (1.0 - releaseCoeff) * absSample
            }

            // Compute desired gain
            var gain: Float = 1.0
            if envelope > 1.0e-6 {
                gain = targetLinear / envelope
            }

            // Clamp gain to maximum
            gain = min(gain, maxGainLinear)

            // Apply gain
            samples[i] *= gain

            // Track applied gain for UI
            if i == samples.count - 1 {
                appliedGainDb = gain > 0 ? 20.0 * log10f(gain) : 0.0
            }
        }

        // Update published state
        let dbValue = appliedGainDb
        DispatchQueue.main.async { [weak self] in
            self?.currentGainDb = dbValue
        }
    }
}
