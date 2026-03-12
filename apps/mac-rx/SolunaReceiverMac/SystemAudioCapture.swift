//
//  SystemAudioCapture.swift
//  SolunaReceiverMac
//
//  Captures system audio using ScreenCaptureKit (macOS 13+)
//  and feeds it to the solunad TX pipeline.
//  Eliminates the need for BlackHole virtual audio device.
//

import Foundation
import ScreenCaptureKit
import AVFoundation
import CoreMedia

@available(macOS 13.0, *)
@MainActor
final class SystemAudioCapture: NSObject, ObservableObject {

    @Published var isCapturing: Bool = false
    @Published var errorMessage: String?

    private var stream: SCStream?
    private var streamOutput: AudioStreamOutput?

    /// Callback to send captured audio data to the TX pipeline
    var onAudioBuffer: ((CMSampleBuffer) -> Void)?

    func startCapture() async {
        guard !isCapturing else { return }

        do {
            // Get shareable content (we only need audio, no display)
            let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: false)

            guard let display = content.displays.first else {
                errorMessage = "No display found"
                return
            }

            // Configure: audio only, no video
            let config = SCStreamConfiguration()
            config.capturesAudio = true
            config.excludesCurrentProcessAudio = true  // Don't capture our own playback
            config.channelCount = 2
            config.sampleRate = 48000
            // Minimize video overhead (we only want audio)
            config.width = 2
            config.height = 2
            config.minimumFrameInterval = CMTime(value: 1, timescale: 1)  // 1 fps min

            // Create content filter (full display to capture all audio)
            let filter = SCContentFilter(display: display, excludingWindows: [])

            let stream = SCStream(filter: filter, configuration: config, delegate: nil)

            let output = AudioStreamOutput()
            output.onAudioBuffer = { [weak self] sampleBuffer in
                self?.onAudioBuffer?(sampleBuffer)
            }

            try stream.addStreamOutput(output, type: .audio, sampleHandlerQueue: .global(qos: .userInteractive))

            try await stream.startCapture()

            self.stream = stream
            self.streamOutput = output
            self.isCapturing = true
            self.errorMessage = nil

        } catch {
            errorMessage = "Capture failed: \(error.localizedDescription)"
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
    }
}

@available(macOS 13.0, *)
private class AudioStreamOutput: NSObject, SCStreamOutput {
    var onAudioBuffer: ((CMSampleBuffer) -> Void)?

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .audio else { return }
        onAudioBuffer?(sampleBuffer)
    }
}
