//
//  SystemAudioCapture.swift
//  SolunaReceiverMac
//
//  Captures system audio using ScreenCaptureKit (macOS 13+)
//  and computes peak levels + FFT spectrum for VU meters.
//  Eliminates the need for BlackHole virtual audio device.
//

import Foundation
import ScreenCaptureKit
import AVFoundation
import CoreMedia
import Accelerate

@available(macOS 13.0, *)
@MainActor
final class SystemAudioCapture: NSObject, ObservableObject {

    static let shared = SystemAudioCapture()

    @Published var isCapturing: Bool = false
    @Published var errorMessage: String?
    @Published var outputLevelL: Float = 0
    @Published var outputLevelR: Float = 0
    @Published var spectrumBands: [Float] = Array(repeating: 0, count: 32)

    private var stream: SCStream?
    private var streamOutput: SystemAudioStreamOutput?

    /// Callback to send captured audio data to the TX pipeline (set from outside)
    nonisolated(unsafe) var onAudioBuffer: ((CMSampleBuffer) -> Void)?

    func startCapture() async {
        guard !isCapturing else { return }

        do {
            let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: false)

            guard let display = content.displays.first else {
                errorMessage = "No display found"
                return
            }

            // Configure: audio capture with minimal video overhead
            let config = SCStreamConfiguration()
            config.capturesAudio = true
            config.excludesCurrentProcessAudio = true  // Don't capture our own playback
            config.channelCount = 2
            config.sampleRate = 48000
            // Minimize video overhead (we only want audio)
            config.width = 2
            config.height = 2
            config.minimumFrameInterval = CMTime(value: 1, timescale: 1)  // 1 fps min

            // Create content filter (full display to capture all system audio)
            let filter = SCContentFilter(display: display, excludingWindows: [])

            let stream = SCStream(filter: filter, configuration: config, delegate: nil)

            // The stream output handler owns its own FFT setup to avoid main-actor issues
            let output = SystemAudioStreamOutput { [weak self] peakL, peakR, bands in
                DispatchQueue.main.async {
                    self?.outputLevelL = peakL
                    self?.outputLevelR = peakR
                    self?.spectrumBands = bands
                }
            }
            output.onAudioBufferForward = { [weak self] buf in
                self?.onAudioBuffer?(buf)
            }
            self.streamOutput = output

            try stream.addStreamOutput(output, type: .audio, sampleHandlerQueue: .global(qos: .userInteractive))

            try await stream.startCapture()

            self.stream = stream
            self.isCapturing = true
            self.errorMessage = nil

            print("[SystemAudioCapture] Started capturing system audio")

        } catch {
            errorMessage = "Capture failed: \(error.localizedDescription)"
            print("[SystemAudioCapture] Error: \(error)")
        }
    }

    func stopCapture() async {
        guard isCapturing, let stream else { return }
        do {
            try await stream.stopCapture()
        } catch {}
        self.stream = nil
        self.streamOutput = nil
        self.isCapturing = false
        self.outputLevelL = 0
        self.outputLevelR = 0
        self.spectrumBands = Array(repeating: 0, count: 32)
        print("[SystemAudioCapture] Stopped")
    }
}

// MARK: - Stream Output Handler (owns its own FFT setup, runs on audio queue)

@available(macOS 13.0, *)
private class SystemAudioStreamOutput: NSObject, SCStreamOutput {

    /// Callback with (peakL, peakR, spectrumBands) — called on audio queue
    let onLevels: (Float, Float, [Float]) -> Void
    /// Forward raw CMSampleBuffer to TX pipeline
    var onAudioBufferForward: ((CMSampleBuffer) -> Void)?

    // FFT infrastructure (owned by this handler, no main-actor dependency)
    private let fftSize = 2048
    private let fftLog2n = vDSP_Length(11)
    private var fftSetup: OpaquePointer?
    private var fftWindow = [Float](repeating: 0, count: 2048)

    init(onLevels: @escaping (Float, Float, [Float]) -> Void) {
        self.onLevels = onLevels
        super.init()
        fftSetup = vDSP_create_fftsetup(fftLog2n, FFTRadix(kFFTRadix2))
        vDSP_hann_window(&fftWindow, vDSP_Length(fftSize), Int32(vDSP_HANN_NORM))
    }

    deinit {
        if let setup = fftSetup {
            vDSP_destroy_fftsetup(setup)
        }
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .audio else { return }

        // Forward raw buffer to TX pipeline callback
        onAudioBufferForward?(sampleBuffer)

        // Extract audio data for level metering + FFT
        guard let blockBuffer = sampleBuffer.dataBuffer else { return }
        var length = 0
        var dataPointer: UnsafeMutablePointer<Int8>?
        CMBlockBufferGetDataPointer(blockBuffer, atOffset: 0, lengthAtOffsetOut: nil, totalLengthOut: &length, dataPointerOut: &dataPointer)

        guard let dataPointer, length > 0 else { return }

        let floatCount = length / MemoryLayout<Float>.size
        let floatPtr = UnsafeRawPointer(dataPointer).assumingMemoryBound(to: Float.self)

        // ScreenCaptureKit delivers Float32 interleaved stereo at configured sample rate
        let channels = 2
        let frames = floatCount / channels
        guard frames > 0 else { return }

        // Compute peak levels (interleaved L R L R ...)
        var peakL: Float = 0
        var peakR: Float = 0
        for i in 0..<frames {
            let absL = abs(floatPtr[i * channels])
            if absL > peakL { peakL = absL }
            let absR = abs(floatPtr[i * channels + 1])
            if absR > peakR { peakR = absR }
        }

        // Extract left channel (mono) for FFT — copy to contiguous buffer
        let fftFrames = min(frames, fftSize)
        var mono = [Float](repeating: 0, count: fftFrames)
        for i in 0..<fftFrames {
            mono[i] = floatPtr[i * channels]
        }

        // Compute FFT spectrum (reuse SDKAudioReceiver's nonisolated static method)
        let bands = mono.withUnsafeBufferPointer { buf -> [Float] in
            guard let ptr = buf.baseAddress else { return Array(repeating: 0, count: 32) }
            return SDKAudioReceiver.computeSpectrum(
                ptr, frameCount: fftFrames,
                fftSize: fftSize, log2n: fftLog2n,
                fftSetup: fftSetup, window: fftWindow,
                sampleRate: 48000
            )
        }

        onLevels(peakL, peakR, bands)
    }
}
