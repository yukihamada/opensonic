import Foundation
import AVFoundation
import Combine

/// Text-to-speech synthesis for announcements using AVSpeechSynthesizer.
///
/// Generates spoken audio from text strings, supporting multiple languages.
/// Can output directly to speakers or render to an AVAudioPCMBuffer for
/// mixing into the audio stream.
///
/// Usage:
/// ```swift
/// let tts = TextToSpeech()
/// tts.speak("Welcome to Soluna", language: "en-US")
/// ```
public final class TextToSpeech: NSObject, ObservableObject {

    // MARK: - Published State

    /// Whether speech synthesis is currently active.
    @Published public private(set) var isSpeaking: Bool = false

    // MARK: - Configuration

    /// Speech rate (0.0 = slowest, 1.0 = fastest). Default is AVSpeechUtteranceDefaultSpeechRate.
    public var rate: Float = AVSpeechUtteranceDefaultSpeechRate

    /// Voice pitch multiplier (0.5 = low, 2.0 = high). Default is 1.0.
    public var pitch: Float = 1.0

    /// Speech volume (0.0 = silent, 1.0 = full volume). Default is 1.0.
    public var volume: Float = 1.0

    // MARK: - Private

    private let synthesizer = AVSpeechSynthesizer()
    private var bufferCompletion: ((AVAudioPCMBuffer?) -> Void)?

    // MARK: - Init

    public override init() {
        super.init()
        synthesizer.delegate = self
    }

    // MARK: - Public API

    /// Speak the given text aloud.
    ///
    /// - Parameters:
    ///   - text: The text to speak.
    ///   - language: BCP 47 language code (default "ja-JP").
    public func speak(_ text: String, language: String = "ja-JP") {
        let utterance = AVSpeechUtterance(string: text)
        utterance.voice = AVSpeechSynthesisVoice(language: language)
        utterance.rate = rate
        utterance.pitchMultiplier = pitch
        utterance.volume = volume

        isSpeaking = true
        synthesizer.speak(utterance)
    }

    /// Render text to an AVAudioPCMBuffer for mixing into the audio stream.
    ///
    /// - Parameter text: The text to synthesize.
    /// - Returns: A PCM buffer containing the rendered speech, or nil on failure.
    public func speakToBuffer(_ text: String, language: String = "ja-JP") -> AVAudioPCMBuffer? {
        let utterance = AVSpeechUtterance(string: text)
        utterance.voice = AVSpeechSynthesisVoice(language: language)
        utterance.rate = rate
        utterance.pitchMultiplier = pitch
        utterance.volume = volume

        var collectedBuffers: [AVAudioPCMBuffer] = []
        let semaphore = DispatchSemaphore(value: 0)

        synthesizer.write(utterance) { buffer in
            guard let pcmBuffer = buffer as? AVAudioPCMBuffer else {
                semaphore.signal()
                return
            }
            if pcmBuffer.frameLength > 0 {
                collectedBuffers.append(pcmBuffer)
            } else {
                // Empty buffer signals completion
                semaphore.signal()
            }
        }

        _ = semaphore.wait(timeout: .now() + 30)

        return mergeBuffers(collectedBuffers)
    }

    /// Stop any current speech output.
    public func stop() {
        synthesizer.stopSpeaking(at: .immediate)
        isSpeaking = false
    }

    // MARK: - Private

    private func mergeBuffers(_ buffers: [AVAudioPCMBuffer]) -> AVAudioPCMBuffer? {
        guard !buffers.isEmpty, let format = buffers.first?.format else { return nil }

        let totalFrames = buffers.reduce(0) { $0 + Int($1.frameLength) }
        guard totalFrames > 0,
              let merged = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(totalFrames)) else {
            return nil
        }

        var offset = 0
        for buf in buffers {
            let frameCount = Int(buf.frameLength)
            for ch in 0..<Int(format.channelCount) {
                guard let srcData = buf.floatChannelData?[ch],
                      let dstData = merged.floatChannelData?[ch] else { continue }
                memcpy(dstData.advanced(by: offset), srcData, frameCount * MemoryLayout<Float>.size)
            }
            offset += frameCount
        }
        merged.frameLength = AVAudioFrameCount(totalFrames)

        return merged
    }
}

// MARK: - AVSpeechSynthesizerDelegate

extension TextToSpeech: AVSpeechSynthesizerDelegate {

    public func speechSynthesizer(_ synthesizer: AVSpeechSynthesizer, didFinish utterance: AVSpeechUtterance) {
        DispatchQueue.main.async { [weak self] in
            self?.isSpeaking = false
        }
    }

    public func speechSynthesizer(_ synthesizer: AVSpeechSynthesizer, didCancel utterance: AVSpeechUtterance) {
        DispatchQueue.main.async { [weak self] in
            self?.isSpeaking = false
        }
    }
}
