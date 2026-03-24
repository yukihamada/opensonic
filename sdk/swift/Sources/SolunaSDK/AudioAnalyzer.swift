import Foundation
import Accelerate
import Combine

/// FFT-based audio analyzer for beat detection and frequency band levels.
///
/// Taps into audio data to provide real-time beat detection and
/// frequency band levels (bass, mid, high) for driving visual effects,
/// LED control, and haptic feedback.
///
/// Updates at approximately 30 fps (~33ms intervals).
///
/// Usage:
/// ```swift
/// let analyzer = AudioAnalyzer()
/// analyzer.onBeat = { print("Beat!") }
/// analyzer.feed(samples: floatSamples, channels: 2, sampleRate: 48000)
/// // Use analyzer.bassLevel, analyzer.rmsLevel etc. in SwiftUI
/// ```
public final class AudioAnalyzer: ObservableObject {

    // MARK: - Published State

    /// Whether a beat was detected in the most recent analysis frame.
    @Published public private(set) var beatDetected: Bool = false

    /// Current bass frequency band level (0.0 - 1.0). Roughly 20-250 Hz.
    @Published public private(set) var bassLevel: Float = 0

    /// Current mid frequency band level (0.0 - 1.0). Roughly 250-4000 Hz.
    @Published public private(set) var midLevel: Float = 0

    /// Current high frequency band level (0.0 - 1.0). Roughly 4000-20000 Hz.
    @Published public private(set) var highLevel: Float = 0

    /// RMS level of the current frame (0.0 - 1.0).
    @Published public private(set) var rmsLevel: Float = 0

    // MARK: - Callbacks

    /// Called on beat detection. Fires on the main thread.
    public var onBeat: (() -> Void)?

    // MARK: - Private

    private let fftSize = 1024
    private let analysisQueue = DispatchQueue(label: "com.soluna.audioanalyzer", qos: .userInteractive)

    /// vDSP FFT setup (reused across frames).
    private var fftSetup: vDSP_DFT_Setup?

    /// Sample accumulation buffer.
    private var sampleBuffer: [Float] = []

    /// Beat detection: running average of bass energy.
    private var bassEnergyHistory: [Float] = []
    private let beatHistorySize = 30
    private var beatCooldown: Int = 0

    /// Throttle: minimum interval between analysis frames.
    private var lastAnalysisTime: TimeInterval = 0
    private let analysisInterval: TimeInterval = 1.0 / 30.0 // ~30 fps

    // MARK: - Init

    public init() {
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

    /// Feed interleaved float32 PCM samples for analysis.
    ///
    /// Call this from the audio receive path (e.g. `SolunaClientDelegate`).
    /// The analyzer will automatically throttle to ~30 fps.
    ///
    /// - Parameters:
    ///   - samples: Interleaved float32 PCM samples.
    ///   - channels: Number of audio channels.
    ///   - sampleRate: Sample rate (typically 48000).
    public func feed(samples: [Float], channels: Int, sampleRate: Double) {
        let now = ProcessInfo.processInfo.systemUptime
        guard now - lastAnalysisTime >= analysisInterval else { return }
        lastAnalysisTime = now

        analysisQueue.async { [weak self] in
            guard let self else { return }

            // Downmix to mono if stereo
            let mono: [Float]
            if channels >= 2 {
                let frameCount = samples.count / channels
                var mixed = [Float](repeating: 0, count: frameCount)
                for i in 0..<frameCount {
                    var sum: Float = 0
                    for ch in 0..<channels {
                        sum += samples[i * channels + ch]
                    }
                    mixed[i] = sum / Float(channels)
                }
                mono = mixed
            } else {
                mono = samples
            }

            self.analyze(mono, sampleRate: sampleRate)
        }
    }

    /// Reset the analyzer state (e.g. on channel switch).
    public func reset() {
        analysisQueue.async { [weak self] in
            self?.sampleBuffer.removeAll()
            self?.bassEnergyHistory.removeAll()
            self?.beatCooldown = 0
        }
        DispatchQueue.main.async { [weak self] in
            self?.beatDetected = false
            self?.bassLevel = 0
            self?.midLevel = 0
            self?.highLevel = 0
            self?.rmsLevel = 0
        }
    }

    // MARK: - Private Analysis

    private func analyze(_ mono: [Float], sampleRate: Double) {
        guard let fftSetup, mono.count >= fftSize else { return }

        // Take the last fftSize samples
        let startIdx = max(0, mono.count - fftSize)
        let frame = Array(mono[startIdx..<startIdx + fftSize])

        // Apply Hann window
        var windowed = [Float](repeating: 0, count: fftSize)
        var window = [Float](repeating: 0, count: fftSize)
        vDSP_hann_window(&window, vDSP_Length(fftSize), Int32(vDSP_HANN_NORM))
        vDSP_vmul(frame, 1, window, 1, &windowed, 1, vDSP_Length(fftSize))

        // Compute RMS
        var rms: Float = 0
        vDSP_rmsqv(frame, 1, &rms, vDSP_Length(fftSize))

        // FFT: prepare split complex
        var realInput = [Float](repeating: 0, count: fftSize)
        var imagInput = [Float](repeating: 0, count: fftSize)
        var realOutput = [Float](repeating: 0, count: fftSize)
        var imagOutput = [Float](repeating: 0, count: fftSize)

        realInput = windowed
        // imagInput stays zero

        vDSP_DFT_Execute(fftSetup, &realInput, &imagInput, &realOutput, &imagOutput)

        // Compute magnitudes for first half (Nyquist)
        let halfSize = fftSize / 2
        var magnitudes = [Float](repeating: 0, count: halfSize)
        for i in 0..<halfSize {
            magnitudes[i] = sqrtf(realOutput[i] * realOutput[i] + imagOutput[i] * imagOutput[i])
        }

        // Normalize magnitudes
        var maxMag: Float = 0
        vDSP_maxv(magnitudes, 1, &maxMag, vDSP_Length(halfSize))
        if maxMag > 0 {
            var scale = 1.0 / maxMag
            vDSP_vsmul(magnitudes, 1, &scale, &magnitudes, 1, vDSP_Length(halfSize))
        }

        // Frequency band boundaries (bin indices)
        let binResolution = sampleRate / Double(fftSize)
        let bassEnd = min(Int(250.0 / binResolution), halfSize)
        let midEnd = min(Int(4000.0 / binResolution), halfSize)

        // Average energy per band
        let bass = bandEnergy(magnitudes, from: 1, to: bassEnd)
        let mid = bandEnergy(magnitudes, from: bassEnd, to: midEnd)
        let high = bandEnergy(magnitudes, from: midEnd, to: halfSize)

        // Beat detection: compare current bass to running average
        bassEnergyHistory.append(bass)
        if bassEnergyHistory.count > beatHistorySize {
            bassEnergyHistory.removeFirst()
        }

        var avgBass: Float = 0
        vDSP_meanv(bassEnergyHistory, 1, &avgBass, vDSP_Length(bassEnergyHistory.count))

        let isBeat: Bool
        if beatCooldown > 0 {
            beatCooldown -= 1
            isBeat = false
        } else if bass > avgBass * 1.4 && bass > 0.15 {
            isBeat = true
            beatCooldown = 4 // ~130ms cooldown at 30fps
        } else {
            isBeat = false
        }

        // Publish on main thread
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.bassLevel = bass
            self.midLevel = mid
            self.highLevel = high
            self.rmsLevel = min(rms * 3.0, 1.0) // Scale up for visibility
            self.beatDetected = isBeat

            if isBeat {
                self.onBeat?()
            }
        }
    }

    private func bandEnergy(_ magnitudes: [Float], from: Int, to: Int) -> Float {
        guard to > from, from >= 0, to <= magnitudes.count else { return 0 }
        var mean: Float = 0
        vDSP_meanv(Array(magnitudes[from..<to]), 1, &mean, vDSP_Length(to - from))
        return mean
    }
}
