//
//  SolunaReceiverApp.swift
//  SolunaReceiverMac
//
//  macOS app for receiving Soluna network audio streams
//

import SwiftUI

@main
struct SolunaReceiverApp: App {
    // Use singletons to avoid @StateObject lifecycle issues across
    // WindowGroup + MenuBarExtra scenes (macOS 14 use-after-free crash).
    @StateObject private var receiver = AudioReceiver.shared
    @StateObject private var speakers = SpeakersController.shared
    @StateObject private var remoteControl = RemoteControlServer()

    var body: some Scene {
        WindowGroup {
            ContentView(receiver: receiver, speakers: speakers)
                .frame(minWidth: 400, minHeight: 600)
                .onAppear { remoteControl.start(receiver: receiver) }
        }
        .commands {
            CommandMenu("Playback") {
                Button(receiver.isPlaying ? "Stop" : "Play") {
                    receiver.toggle()
                }
                .keyboardShortcut(.space, modifiers: [])

                Divider()

                Button("Volume Up") {
                    receiver.volume = min(1.0, receiver.volume + 0.05)
                }
                .keyboardShortcut(.upArrow, modifiers: [.command])

                Button("Volume Down") {
                    receiver.volume = max(0.0, receiver.volume - 0.05)
                }
                .keyboardShortcut(.downArrow, modifiers: [.command])

                Button(receiver.isMuted ? "Unmute" : "Mute") {
                    receiver.isMuted.toggle()
                }
                .keyboardShortcut("m", modifiers: [.command, .shift])

                Divider()

                Button("Sync All Delays") {
                    speakers.recalculateAllDelays()
                }
                .keyboardShortcut("s", modifiers: [.command, .shift])
            }
        }

        // Menu bar mini player
        // IMPORTANT: Use a static label (Image only) to avoid accessing
        // @StateObject in the MenuBarExtra label closure, which can crash
        // on macOS 14 when the scene context evaluates before @StateObject
        // storage is fully initialized (EXC_BAD_ACCESS at 0xaaaa...).
        MenuBarExtra {
            MenuBarMiniPlayer()
        } label: {
            Image(systemName: "speaker.wave.2.fill")
        }
    }
}

// MARK: - Menu Bar Mini Player

struct MenuBarMiniPlayer: View {
    // Reference singletons directly — these are never deallocated,
    // so there's no risk of accessing freed memory during scene setup.
    @ObservedObject private var receiver = AudioReceiver.shared
    @ObservedObject private var speakers = SpeakersController.shared

    var body: some View {
        VStack(spacing: 8) {
            // Status
            HStack {
                Circle()
                    .fill(statusColor)
                    .frame(width: 8, height: 8)
                Text(receiver.state.rawValue)
                    .font(.headline)
                Spacer()
                if receiver.packetsReceived > 0 {
                    Text(formatNum(receiver.packetsReceived))
                        .font(.caption.monospacedDigit())
                        .foregroundColor(.secondary)
                }
            }

            Divider()

            // Play/Stop
            Button {
                receiver.toggle()
            } label: {
                Label(receiver.isPlaying ? "Stop" : "Play",
                      systemImage: receiver.isPlaying ? "stop.fill" : "play.fill")
            }

            Divider()

            // Volume
            HStack {
                Button {
                    receiver.isMuted.toggle()
                } label: {
                    Image(systemName: receiver.isMuted ? "speaker.slash.fill" : "speaker.wave.2.fill")
                }
                Slider(value: Binding(
                    get: { receiver.volume },
                    set: { v in
                        receiver.volume = v
                        if receiver.isMuted { receiver.isMuted = false }
                    }
                ), in: 0...1)
                Text("\(Int(receiver.volume * 100))%")
                    .font(.caption.monospacedDigit())
                    .frame(width: 35, alignment: .trailing)
            }

            // Active devices count
            let activeCount = receiver.activeOutputs.count
            if activeCount > 0 {
                Divider()
                HStack {
                    Image(systemName: "speaker.2.fill")
                        .foregroundColor(.secondary)
                    Text("\(activeCount) extra output\(activeCount > 1 ? "s" : "")")
                        .font(.caption)
                        .foregroundColor(.secondary)
                    Spacer()
                    Button("Sync") {
                        speakers.recalculateAllDelays()
                    }
                    .font(.caption)
                }
            }

            Divider()

            Button("Quit Soluna") {
                NSApplication.shared.terminate(nil)
            }
        }
        .padding(12)
        .frame(width: 260)
    }

    private var statusColor: Color {
        switch receiver.state {
        case .receiving:  return .green
        case .connecting: return .orange
        case .error:      return .red
        case .stopped:    return .gray
        }
    }

    private func formatNum(_ n: UInt64) -> String {
        n >= 1_000_000 ? String(format: "%.1fM", Double(n) / 1_000_000)
        : n >= 1_000   ? String(format: "%.1fK", Double(n) / 1_000)
        : "\(n)"
    }
}
