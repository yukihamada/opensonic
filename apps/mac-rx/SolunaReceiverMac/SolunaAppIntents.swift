//
//  SolunaAppIntents.swift
//  SolunaReceiverMac
//
//  Siri Shortcuts / App Intents for "Hey Siri, play Soluna" etc.
//

import AppIntents

// MARK: - Play Intent

struct PlaySolunaIntent: AppIntent {
    static var title: LocalizedStringResource = "Play Soluna Rx"
    static var description = IntentDescription("Start receiving audio")
    static var openAppWhenRun = true

    @MainActor
    func perform() async throws -> some IntentResult {
        let receiver = SolunaAudioReceiver.sharedInstance()
        if receiver.state != .receiving {
            receiver.start()
        }
        return .result()
    }
}

// MARK: - Stop Intent

struct StopSolunaIntent: AppIntent {
    static var title: LocalizedStringResource = "Stop Soluna Rx"
    static var description = IntentDescription("Stop receiving audio")

    @MainActor
    func perform() async throws -> some IntentResult {
        let receiver = SolunaAudioReceiver.sharedInstance()
        receiver.stop()
        return .result()
    }
}

// MARK: - Toggle Intent

struct ToggleSolunaIntent: AppIntent {
    static var title: LocalizedStringResource = "Toggle Soluna Rx"
    static var description = IntentDescription("Toggle audio playback on/off")
    static var openAppWhenRun = true

    @MainActor
    func perform() async throws -> some IntentResult {
        let receiver = SolunaAudioReceiver.sharedInstance()
        if receiver.state == .receiving {
            receiver.stop()
        } else {
            receiver.start()
        }
        return .result()
    }
}

// MARK: - Set Volume Intent

struct SetVolumeSolunaIntent: AppIntent {
    static var title: LocalizedStringResource = "Set Soluna Volume"
    static var description = IntentDescription("Set the volume level (0-100)")

    @Parameter(title: "Volume", description: "Volume percentage (0-100)")
    var volumePercent: Int

    @MainActor
    func perform() async throws -> some IntentResult {
        let receiver = SolunaAudioReceiver.sharedInstance()
        receiver.volume = Float(max(0, min(100, volumePercent))) / 100.0
        return .result()
    }
}

// MARK: - Mute Intent

struct MuteSolunaIntent: AppIntent {
    static var title: LocalizedStringResource = "Mute Soluna Rx"
    static var description = IntentDescription("Mute or unmute audio")

    @Parameter(title: "Muted")
    var muted: Bool

    @MainActor
    func perform() async throws -> some IntentResult {
        let receiver = SolunaAudioReceiver.sharedInstance()
        receiver.muted = muted
        return .result()
    }
}

// MARK: - App Shortcuts Provider

struct SolunaShortcutsProvider: AppShortcutsProvider {
    static var appShortcuts: [AppShortcut] {
        AppShortcut(
            intent: PlaySolunaIntent(),
            phrases: [
                "Play \(.applicationName)",
                "Start \(.applicationName)",
                "Start receiving with \(.applicationName)"
            ],
            shortTitle: "Play",
            systemImageName: "play.fill"
        )
        AppShortcut(
            intent: StopSolunaIntent(),
            phrases: [
                "Stop \(.applicationName)",
                "Pause \(.applicationName)"
            ],
            shortTitle: "Stop",
            systemImageName: "stop.fill"
        )
        AppShortcut(
            intent: ToggleSolunaIntent(),
            phrases: [
                "Toggle \(.applicationName)"
            ],
            shortTitle: "Toggle",
            systemImageName: "playpause.fill"
        )
    }
}
