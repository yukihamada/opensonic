//
//  AudioFingerprint.swift
//  SolunaReceiver
//
//  Computes a 64-bit audio fingerprint from received audio and
//  reports it to the relay server every 30 seconds.
//
//  Algorithm:
//    - Collect 5 seconds of mono audio at 48 kHz (240,000 samples)
//    - Divide into 32 time windows (7,500 samples each)
//    - FFT each window, extract energy in 8 frequency bands
//    - Compare adjacent band energies to produce 2 bits per window
//    - Result: 32 windows x 2 bits = 64-bit hash
//

import Foundation
import Accelerate
#if os(iOS)
import UIKit
#endif

final class AudioFingerprint {
    static let shared = AudioFingerprint()

    // MARK: - Constants

    private let sampleRate: Int = 48000
    private let windowDuration: Double = 5.0          // seconds of audio for one fingerprint
    private let reportInterval: Double = 30.0         // send report every 30s
    private let windowSamples: Int = 7500             // 240000 / 32
    private let numWindows: Int = 32
    private let fingerprintSamples: Int = 240000      // 48000 * 5

    /// 8 frequency band edges in Hz
    private let bandFreqs: [Double] = [200, 400, 800, 1600, 3200, 6400, 12800, 25600]

    // MARK: - Circular Buffer

    private let bufferCapacity = 48000 * 10           // 10 seconds
    private var sampleBuffer: [Float]
    private var writePos: Int = 0
    private var samplesWritten: Int = 0
    private let lock = os_unfair_lock_t.allocate(capacity: 1)

    // MARK: - FFT Setup

    private let fftSize: Int = 8192                   // power-of-2 >= windowSamples
    private let log2n: vDSP_Length
    private let fftSetup: FFTSetup

    // MARK: - State

    private var timer: DispatchSourceTimer?
    private var channelName: String = ""
    private var deviceId: String = ""
    private let relayHost: String = "relay.solun.art"
    private let sendQueue = DispatchQueue(label: "com.soluna.fingerprint.send", qos: .utility)

    // MARK: - Init

    private init() {
        sampleBuffer = [Float](repeating: 0, count: bufferCapacity)
        lock.initialize(to: os_unfair_lock())
        log2n = vDSP_Length(log2(Double(fftSize)))
        fftSetup = vDSP_create_fftsetup(log2n, FFTRadix(kFFTRadix2))!
    }

    deinit {
        vDSP_destroy_fftsetup(fftSetup)
        lock.deallocate()
    }

    // MARK: - Public API

    /// Start fingerprinting for the given channel. Called from AudioReceiver.start().
    func start(channel: String) {
        stop()
        channelName = channel
        #if os(iOS)
        deviceId = UIDevice.current.identifierForVendor?.uuidString ?? UUID().uuidString
        #else
        deviceId = UUID().uuidString
        #endif

        os_unfair_lock_lock(lock)
        writePos = 0
        samplesWritten = 0
        os_unfair_lock_unlock(lock)

        let source = DispatchSource.makeTimerSource(queue: sendQueue)
        source.schedule(deadline: .now() + reportInterval, repeating: reportInterval)
        source.setEventHandler { [weak self] in
            self?.reportFingerprint()
        }
        source.resume()
        timer = source
        print("[AudioFingerprint] Started for channel: \(channel)")
    }

    /// Stop fingerprinting. Called from AudioReceiver.stop().
    func stop() {
        timer?.cancel()
        timer = nil
    }

    /// Feed audio samples from the audio render callback.
    /// Called from the bridge's sample tap — must be lock-free-safe (os_unfair_lock is fine
    /// on the audio thread as long as contention is minimal and hold time is tiny).
    func addSamples(_ samples: UnsafePointer<Float>, count: Int) {
        os_unfair_lock_lock(lock)
        let cap = bufferCapacity
        var pos = writePos
        for i in 0..<count {
            sampleBuffer[pos] = samples[i]
            pos += 1
            if pos >= cap { pos = 0 }
        }
        writePos = pos
        samplesWritten += count
        os_unfair_lock_unlock(lock)
    }

    // MARK: - Fingerprint Computation

    private func reportFingerprint() {
        // Need at least 5 seconds of audio
        os_unfair_lock_lock(lock)
        let written = samplesWritten
        let currentWritePos = writePos
        os_unfair_lock_unlock(lock)

        guard written >= fingerprintSamples else {
            print("[AudioFingerprint] Not enough samples yet (\(written)/\(fingerprintSamples))")
            return
        }

        // Copy the last 5 seconds of mono audio from circular buffer
        var mono = [Float](repeating: 0, count: fingerprintSamples)
        os_unfair_lock_lock(lock)
        var readPos = currentWritePos - fingerprintSamples
        if readPos < 0 { readPos += bufferCapacity }
        for i in 0..<fingerprintSamples {
            var idx = readPos + i
            if idx >= bufferCapacity { idx -= bufferCapacity }
            mono[i] = sampleBuffer[idx]
        }
        os_unfair_lock_unlock(lock)

        let hash = computeFingerprint(mono)
        let chroma = computeChromaprint(mono)
        sendReport(hash: hash, chromaprint: chroma)
    }

    private func computeFingerprint(_ samples: [Float]) -> UInt64 {
        var hash: UInt64 = 0
        let halfN = fftSize / 2

        // Pre-compute bin indices for each frequency band
        let binFreqResolution = Double(sampleRate) / Double(fftSize)
        let bandBins = bandFreqs.map { Int(round($0 / binFreqResolution)) }

        // Hann window
        var window = [Float](repeating: 0, count: fftSize)
        vDSP_hann_window(&window, vDSP_Length(fftSize), Int32(vDSP_HANN_NORM))

        // FFT split complex buffers
        var realp = [Float](repeating: 0, count: halfN)
        var imagp = [Float](repeating: 0, count: halfN)

        for w in 0..<numWindows {
            let offset = w * windowSamples

            // Zero-pad windowed samples into FFT input
            var windowed = [Float](repeating: 0, count: fftSize)
            for i in 0..<windowSamples {
                windowed[i] = samples[offset + i] * window[i]
            }

            // Pack into split complex & run FFT
            var magnitudes = [Float](repeating: 0, count: halfN)
            realp.withUnsafeMutableBufferPointer { rBuf in
                imagp.withUnsafeMutableBufferPointer { iBuf in
                    var split = DSPSplitComplex(realp: rBuf.baseAddress!, imagp: iBuf.baseAddress!)

                    // ctoz: interleaved → split complex
                    windowed.withUnsafeBufferPointer { buf in
                        buf.baseAddress!.withMemoryRebound(to: DSPComplex.self, capacity: halfN) { cplx in
                            vDSP_ctoz(cplx, 2, &split, 1, vDSP_Length(halfN))
                        }
                    }

                    // Forward FFT
                    vDSP_fft_zrip(fftSetup, &split, 1, log2n, FFTDirection(kFFTDirection_Forward))

                    // Compute magnitude squared
                    vDSP_zvmags(&split, 1, &magnitudes, 1, vDSP_Length(halfN))
                }
            }

            // Compute energy in each of the 8 frequency bands
            var bandEnergy = [Float](repeating: 0, count: 8)
            for b in 0..<8 {
                let lo = bandBins[b]
                let hi: Int
                if b < 7 {
                    hi = bandBins[b + 1]
                } else {
                    hi = min(halfN, bandBins[b] * 2)  // last band extends to 2x its start
                }
                guard lo < halfN && lo < hi else { continue }
                let upper = min(hi, halfN)
                var sum: Float = 0
                for k in lo..<upper {
                    sum += magnitudes[k]
                }
                bandEnergy[b] = sum / Float(max(1, upper - lo))
            }

            // Compare adjacent bands to produce 2 bits per window
            // bit0: band[0]>band[1] vs band[2]>band[3] vs band[4]>band[5]
            // bit1: band[1]>band[2] vs band[3]>band[4] vs band[5]>band[6]
            // Simplified: compare band pairs to get 2 bits
            let bit0: UInt64 = (bandEnergy[0] > bandEnergy[1] ? 1 : 0)
                             ^ (bandEnergy[2] > bandEnergy[3] ? 1 : 0)
                             ^ (bandEnergy[4] > bandEnergy[5] ? 1 : 0)
            let bit1: UInt64 = (bandEnergy[1] > bandEnergy[2] ? 1 : 0)
                             ^ (bandEnergy[3] > bandEnergy[4] ? 1 : 0)
                             ^ (bandEnergy[5] > bandEnergy[6] ? 1 : 0)

            let bitPos = w * 2
            hash |= (bit0 << bitPos)
            hash |= (bit1 << (bitPos + 1))
        }

        return hash
    }

    // MARK: - Chromaprint Computation

    /// Compute 12-bin chroma features for each of the 32 time windows.
    /// Returns a base64-encoded string of 32x12 = 384 quantized uint8 values.
    private func computeChromaprint(_ samples: [Float]) -> String {
        let halfN = fftSize / 2
        let binFreqResolution = Double(sampleRate) / Double(fftSize)

        // Hann window
        var window = [Float](repeating: 0, count: fftSize)
        vDSP_hann_window(&window, vDSP_Length(fftSize), Int32(vDSP_HANN_NORM))

        var realp = [Float](repeating: 0, count: halfN)
        var imagp = [Float](repeating: 0, count: halfN)

        // 32 windows x 12 chroma bins
        var chromaData = [UInt8](repeating: 0, count: numWindows * 12)

        for w in 0..<numWindows {
            let offset = w * windowSamples

            var windowed = [Float](repeating: 0, count: fftSize)
            for i in 0..<windowSamples {
                windowed[i] = samples[offset + i] * window[i]
            }

            var magnitudes = [Float](repeating: 0, count: halfN)
            realp.withUnsafeMutableBufferPointer { rBuf in
                imagp.withUnsafeMutableBufferPointer { iBuf in
                    var split = DSPSplitComplex(realp: rBuf.baseAddress!, imagp: iBuf.baseAddress!)

                    windowed.withUnsafeBufferPointer { buf in
                        buf.baseAddress!.withMemoryRebound(to: DSPComplex.self, capacity: halfN) { cplx in
                            vDSP_ctoz(cplx, 2, &split, 1, vDSP_Length(halfN))
                        }
                    }

                    vDSP_fft_zrip(fftSetup, &split, 1, log2n, FFTDirection(kFFTDirection_Forward))
                    vDSP_zvmags(&split, 1, &magnitudes, 1, vDSP_Length(halfN))
                }
            }

            // Accumulate magnitudes into 12 chroma bins
            var chroma = [Float](repeating: 0, count: 12)

            // Start from bin corresponding to ~60 Hz (skip DC and very low bins)
            let minBin = max(1, Int(60.0 / binFreqResolution))
            let maxBin = min(halfN, Int(4200.0 / binFreqResolution))  // up to ~4.2 kHz

            for k in minBin..<maxBin {
                let freq = Double(k) * binFreqResolution
                guard freq > 0 else { continue }
                // Map frequency to pitch class: pitchClass = round(12 * log2(freq / 440)) mod 12
                let pitchRaw = 12.0 * log2(freq / 440.0)
                var pitchClass = Int(round(pitchRaw)) % 12
                if pitchClass < 0 { pitchClass += 12 }
                chroma[pitchClass] += magnitudes[k]
            }

            // Normalize chroma vector and quantize to uint8
            var maxVal: Float = 0
            vDSP_maxv(chroma, 1, &maxVal, vDSP_Length(12))
            let scale: Float = maxVal > 0 ? 255.0 / maxVal : 0

            for c in 0..<12 {
                let quantized = min(255, max(0, Int(chroma[c] * scale)))
                chromaData[w * 12 + c] = UInt8(quantized)
            }
        }

        return Data(chromaData).base64EncodedString()
    }

    // MARK: - Network

    private func sendReport(hash: UInt64, chromaprint: String) {
        let hexHash = String(format: "%016llx", hash)
        let payload: [String: Any] = [
            "channel": channelName,
            "device_id": deviceId,
            "fingerprint": hexHash,
            "chromaprint": chromaprint,
            "duration": Int(windowDuration),
            "timestamp": Int(Date().timeIntervalSince1970),
            "sample_rate": sampleRate,
            "window_sec": windowDuration
        ]

        guard let url = URL(string: "https://\(relayHost)/api/fingerprint"),
              let body = try? JSONSerialization.data(withJSONObject: payload) else { return }

        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = body
        request.timeoutInterval = 10

        URLSession.shared.dataTask(with: request) { _, response, error in
            if let error = error {
                print("[AudioFingerprint] Report failed: \(error.localizedDescription)")
                return
            }
            let status = (response as? HTTPURLResponse)?.statusCode ?? 0
            print("[AudioFingerprint] Report sent: \(hexHash) (HTTP \(status))")
        }.resume()
    }
}
