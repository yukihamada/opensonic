import Foundation
import AVFoundation
import Combine
#if canImport(Speech)
import Speech
#endif

/// Real-time speech-to-text transcription using on-device SFSpeechRecognizer.
///
/// Supports multiple languages and provides streaming transcription results.
/// Audio samples (float32, 48kHz) are converted to AVAudioPCMBuffer and fed
/// to the speech recognition request.
///
/// Usage:
/// ```swift
/// let transcriber = SpeechTranscriber()
/// transcriber.language = "ja-JP"
/// transcriber.onTranscript = { text, isFinal in print(text) }
/// transcriber.start()
/// transcriber.feedAudio(samples: audioSamples, sampleRate: 48000)
/// transcriber.stop()
/// ```
public final class SpeechTranscriber: ObservableObject {

    // MARK: - Published State

    /// Whether transcription is currently active.
    @Published public private(set) var isTranscribing: Bool = false

    /// The latest transcription text.
    @Published public private(set) var currentTranscript: String = ""

    // MARK: - Configuration

    /// BCP 47 language code for recognition (default "ja-JP").
    public var language: String = "ja-JP"

    /// Called when a transcription result is available.
    /// Parameters: (text, isFinal).
    public var onTranscript: ((String, Bool) -> Void)?

    /// Available language codes supported by the device.
    public var supportedLanguages: [String] {
        #if canImport(Speech)
        return SFSpeechRecognizer.supportedLocales().map { $0.identifier }.sorted()
        #else
        return []
        #endif
    }

    // MARK: - Private

    #if canImport(Speech)
    private var recognizer: SFSpeechRecognizer?
    private var recognitionRequest: SFSpeechAudioBufferRecognitionRequest?
    private var recognitionTask: SFSpeechRecognitionTask?
    #endif

    private let audioFormat: AVAudioFormat

    // MARK: - Init

    public init() {
        audioFormat = AVAudioFormat(
            commonFormat: .pcmFormatFloat32,
            sampleRate: 48000,
            channels: 1,
            interleaved: false
        )!
    }

    // MARK: - Public API

    /// Start the speech transcription session.
    public func start() {
        guard !isTranscribing else { return }

        #if canImport(Speech)
        let locale = Locale(identifier: language)
        recognizer = SFSpeechRecognizer(locale: locale)

        guard let recognizer, recognizer.isAvailable else {
            print("[SolunaSDK] SpeechRecognizer not available for \(language)")
            return
        }

        let request = SFSpeechAudioBufferRecognitionRequest()
        request.shouldReportPartialResults = true

        if #available(iOS 16, macOS 13, *) {
            request.addsPunctuation = true
        }

        recognitionRequest = request

        recognitionTask = recognizer.recognitionTask(with: request) { [weak self] result, error in
            guard let self else { return }

            if let result {
                let text = result.bestTranscription.formattedString
                let isFinal = result.isFinal

                DispatchQueue.main.async {
                    self.currentTranscript = text
                    self.onTranscript?(text, isFinal)
                }

                if isFinal {
                    self.cleanupRecognition()
                }
            }

            if let error {
                print("[SolunaSDK] Speech recognition error: \(error.localizedDescription)")
                self.cleanupRecognition()
            }
        }

        isTranscribing = true
        #else
        print("[SolunaSDK] Speech framework not available on this platform")
        #endif
    }

    /// Stop the speech transcription session.
    public func stop() {
        guard isTranscribing else { return }
        #if canImport(Speech)
        recognitionRequest?.endAudio()
        recognitionTask?.cancel()
        cleanupRecognition()
        #endif
    }

    /// Feed audio samples to the transcriber.
    ///
    /// - Parameters:
    ///   - samples: Float32 mono audio samples.
    ///   - sampleRate: Sample rate of the audio (typically 48000).
    public func feedAudio(samples: [Float], sampleRate: Double) {
        #if canImport(Speech)
        guard isTranscribing, let request = recognitionRequest else { return }

        let format = AVAudioFormat(
            commonFormat: .pcmFormatFloat32,
            sampleRate: sampleRate,
            channels: 1,
            interleaved: false
        )!

        guard let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(samples.count)) else {
            return
        }
        buffer.frameLength = AVAudioFrameCount(samples.count)

        if let channelData = buffer.floatChannelData?[0] {
            for i in 0..<samples.count {
                channelData[i] = samples[i]
            }
        }

        request.append(buffer)
        #endif
    }

    // MARK: - Private

    #if canImport(Speech)
    private func cleanupRecognition() {
        recognitionRequest = nil
        recognitionTask = nil
        DispatchQueue.main.async { [weak self] in
            self?.isTranscribing = false
        }
    }
    #endif
}
