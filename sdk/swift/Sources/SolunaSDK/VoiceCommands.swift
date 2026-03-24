import Foundation
import Combine
#if canImport(Speech)
import Speech
#endif

/// A registered voice command with phrase, aliases, and action.
public struct VoiceCommand: Identifiable {
    public let id: String
    public let phrase: String
    public let aliases: [String]
    public let action: () -> Void

    public init(phrase: String, aliases: [String] = [], action: @escaping () -> Void) {
        self.id = UUID().uuidString
        self.phrase = phrase
        self.aliases = aliases
        self.action = action
    }
}

/// Voice command recognition for hands-free operation.
///
/// Listens for predefined voice commands using SFSpeechRecognizer
/// and triggers registered actions when a command phrase is detected.
///
/// Usage:
/// ```swift
/// let vcr = VoiceCommandRecognizer()
/// vcr.register(phrase: "next channel") { switchChannel() }
/// vcr.startListening()
/// ```
public final class VoiceCommandRecognizer: ObservableObject {

    // MARK: - Published State

    /// Whether voice command listening is active.
    @Published public private(set) var isListening: Bool = false

    // MARK: - Configuration

    /// Language for voice recognition (default "ja-JP").
    public var language: String = "ja-JP"

    /// Registered voice commands.
    public private(set) var commands: [VoiceCommand] = []

    // MARK: - Private

    #if canImport(Speech)
    private var recognizer: SFSpeechRecognizer?
    private var recognitionRequest: SFSpeechAudioBufferRecognitionRequest?
    private var recognitionTask: SFSpeechRecognitionTask?
    private var audioEngine: AVAudioEngine?
    #endif

    /// Cooldown to prevent repeated triggering.
    private var lastCommandTime: Date = .distantPast
    private let commandCooldown: TimeInterval = 2.0

    // MARK: - Init

    public init() {
        registerBuiltInCommands()
    }

    // MARK: - Public API

    /// Register a voice command with a phrase and action.
    ///
    /// - Parameters:
    ///   - phrase: The trigger phrase to listen for.
    ///   - aliases: Alternative phrases that also trigger the command.
    ///   - action: The closure to execute when the command is recognized.
    public func register(phrase: String, aliases: [String] = [], action: @escaping () -> Void) {
        let command = VoiceCommand(phrase: phrase, aliases: aliases, action: action)
        commands.append(command)
    }

    /// Start listening for voice commands.
    public func startListening() {
        guard !isListening else { return }

        #if canImport(Speech)
        let locale = Locale(identifier: language)
        recognizer = SFSpeechRecognizer(locale: locale)

        guard let recognizer, recognizer.isAvailable else {
            print("[SolunaSDK] Voice command recognizer not available for \(language)")
            return
        }

        let engine = AVAudioEngine()
        let request = SFSpeechAudioBufferRecognitionRequest()
        request.shouldReportPartialResults = true

        recognitionTask = recognizer.recognitionTask(with: request) { [weak self] result, error in
            guard let self else { return }

            if let result {
                let text = result.bestTranscription.formattedString.lowercased()
                self.matchCommand(text)
            }

            if let error {
                print("[SolunaSDK] Voice command error: \(error.localizedDescription)")
                self.stopListening()
            }
        }

        let inputNode = engine.inputNode
        let recordingFormat = inputNode.outputFormat(forBus: 0)

        inputNode.installTap(onBus: 0, bufferSize: 1024, format: recordingFormat) { buffer, _ in
            request.append(buffer)
        }

        do {
            engine.prepare()
            try engine.start()
            audioEngine = engine
            recognitionRequest = request
            isListening = true
        } catch {
            print("[SolunaSDK] Voice command engine start error: \(error)")
        }
        #endif
    }

    /// Stop listening for voice commands.
    public func stopListening() {
        guard isListening else { return }

        #if canImport(Speech)
        audioEngine?.stop()
        audioEngine?.inputNode.removeTap(onBus: 0)
        audioEngine = nil
        recognitionRequest?.endAudio()
        recognitionTask?.cancel()
        recognitionRequest = nil
        recognitionTask = nil
        #endif

        isListening = false
    }

    // MARK: - Private

    private func registerBuiltInCommands() {
        // Built-in commands are registered as no-ops; users should override
        // by registering their own commands with the same phrases.
    }

    private func matchCommand(_ transcription: String) {
        let now = Date()
        guard now.timeIntervalSince(lastCommandTime) > commandCooldown else { return }

        for command in commands {
            let allPhrases = [command.phrase.lowercased()] + command.aliases.map { $0.lowercased() }
            for phrase in allPhrases {
                if transcription.contains(phrase) {
                    lastCommandTime = now
                    DispatchQueue.main.async {
                        command.action()
                    }
                    return
                }
            }
        }
    }
}
