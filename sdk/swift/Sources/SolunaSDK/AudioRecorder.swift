import Foundation
import AVFoundation
import Combine

/// Result of a recording session.
public struct RecordingResult: Sendable {
    /// URL of the recorded audio file.
    public let fileURL: URL

    /// Duration of the recording in seconds.
    public let duration: TimeInterval

    /// Channel name that was recorded.
    public let channel: String

    /// When the recording started.
    public let startedAt: Date

    /// Optional transcript of the recording (if transcription was enabled).
    public let transcript: String?
}

/// Records received audio to file with optional live transcription.
///
/// Records audio as .caf format (48kHz, float32) and can optionally
/// use SpeechTranscriber for live transcription during recording.
///
/// Usage:
/// ```swift
/// let recorder = AudioRecorder()
/// let fileURL = recorder.startRecording(channel: "jazz")
/// // ... feed audio data ...
/// let result = recorder.stopRecording()
/// ```
public final class AudioRecorder: ObservableObject {

    // MARK: - Published State

    /// Whether recording is currently active.
    @Published public private(set) var isRecording: Bool = false

    /// Current recording duration in seconds.
    @Published public private(set) var currentDuration: TimeInterval = 0

    // MARK: - Private

    private var outputFile: AVAudioFile?
    private var outputURL: URL?
    private var recordingChannel: String = ""
    private var recordingStartedAt: Date?
    private var durationTimer: Timer?
    private var transcriber: SpeechTranscriber?
    private var accumulatedTranscript: String = ""

    private let recordingFormat: AVAudioFormat

    // MARK: - Init

    public init() {
        recordingFormat = AVAudioFormat(
            commonFormat: .pcmFormatFloat32,
            sampleRate: 48000,
            channels: 2,
            interleaved: false
        )!
    }

    // MARK: - Public API

    /// Start recording audio from the specified channel.
    ///
    /// - Parameters:
    ///   - channel: The channel name being recorded.
    ///   - enableTranscription: Whether to enable live transcription (default false).
    /// - Returns: The URL where the recording will be saved.
    @discardableResult
    public func startRecording(channel: String, enableTranscription: Bool = false) -> URL {
        stopRecording()

        let fileName = "soluna_\(channel)_\(ISO8601DateFormatter().string(from: Date())).caf"
            .replacingOccurrences(of: ":", with: "-")
        let directory = FileManager.default.temporaryDirectory
        let fileURL = directory.appendingPathComponent(fileName)

        do {
            outputFile = try AVAudioFile(
                forWriting: fileURL,
                settings: recordingFormat.settings,
                commonFormat: .pcmFormatFloat32,
                interleaved: false
            )
        } catch {
            print("[SolunaSDK] Failed to create recording file: \(error)")
            return fileURL
        }

        outputURL = fileURL
        recordingChannel = channel
        recordingStartedAt = Date()
        currentDuration = 0
        accumulatedTranscript = ""
        isRecording = true

        // Start duration timer
        durationTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            guard let self, let start = self.recordingStartedAt else { return }
            DispatchQueue.main.async {
                self.currentDuration = Date().timeIntervalSince(start)
            }
        }

        // Optionally start transcription
        if enableTranscription {
            let t = SpeechTranscriber()
            t.onTranscript = { [weak self] text, isFinal in
                if isFinal {
                    self?.accumulatedTranscript += text + " "
                }
            }
            t.start()
            transcriber = t
        }

        return fileURL
    }

    /// Write audio samples to the recording file.
    ///
    /// - Parameter buffer: PCM audio buffer to record.
    public func writeBuffer(_ buffer: AVAudioPCMBuffer) {
        guard isRecording, let file = outputFile else { return }

        do {
            try file.write(from: buffer)
        } catch {
            print("[SolunaSDK] Recording write error: \(error)")
        }

        // Feed to transcriber if active
        if let transcriber, let channelData = buffer.floatChannelData?[0] {
            let samples = Array(UnsafeBufferPointer(start: channelData, count: Int(buffer.frameLength)))
            transcriber.feedAudio(samples: samples, sampleRate: 48000)
        }
    }

    /// Stop recording and return the result.
    ///
    /// - Returns: The recording result with file URL, duration, and optional transcript.
    @discardableResult
    public func stopRecording() -> RecordingResult? {
        guard isRecording else { return nil }

        durationTimer?.invalidate()
        durationTimer = nil

        transcriber?.stop()
        transcriber = nil

        outputFile = nil

        let result = RecordingResult(
            fileURL: outputURL ?? URL(fileURLWithPath: "/dev/null"),
            duration: currentDuration,
            channel: recordingChannel,
            startedAt: recordingStartedAt ?? Date(),
            transcript: accumulatedTranscript.isEmpty ? nil : accumulatedTranscript.trimmingCharacters(in: .whitespaces)
        )

        isRecording = false
        currentDuration = 0

        return result
    }

    /// Transcribe an existing recording file.
    ///
    /// - Parameter url: URL of the audio file to transcribe.
    /// - Returns: The transcription text.
    public func transcribeRecording(url: URL) async -> String {
        // Use SpeechTranscriber in a batch mode
        let transcriber = SpeechTranscriber()
        var finalText = ""

        return await withCheckedContinuation { continuation in
            transcriber.onTranscript = { text, isFinal in
                if isFinal {
                    finalText = text
                    continuation.resume(returning: finalText)
                }
            }
            transcriber.start()

            // Read the file and feed samples
            do {
                let file = try AVAudioFile(forReading: url)
                let format = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: file.fileFormat.sampleRate, channels: 1, interleaved: false)!
                let frameCount = AVAudioFrameCount(file.length)
                guard let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: frameCount) else {
                    continuation.resume(returning: "")
                    return
                }
                try file.read(into: buffer)

                if let channelData = buffer.floatChannelData?[0] {
                    let samples = Array(UnsafeBufferPointer(start: channelData, count: Int(buffer.frameLength)))
                    transcriber.feedAudio(samples: samples, sampleRate: file.fileFormat.sampleRate)
                }

                // Give the recognizer time then stop
                DispatchQueue.main.asyncAfter(deadline: .now() + 5) {
                    if finalText.isEmpty {
                        transcriber.stop()
                        continuation.resume(returning: finalText)
                    }
                }
            } catch {
                print("[SolunaSDK] Transcription file read error: \(error)")
                continuation.resume(returning: "")
            }
        }
    }
}
