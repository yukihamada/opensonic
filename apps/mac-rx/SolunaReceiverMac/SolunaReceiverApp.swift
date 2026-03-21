//
//  SolunaReceiverApp.swift
//  SolunaReceiverMac
//
//  macOS app for receiving Soluna network audio streams
//

import SwiftUI
import ServiceManagement
import MediaPlayer

// MARK: - Channel definitions for shortcuts

private let channelShortcuts: [(id: String, label: String, key: KeyEquivalent)] = [
    ("soluna", "Soluna",  "1"),
    ("jazz",   "Jazz",    "2"),
    ("lofi",   "Lo-Fi",   "3"),
    ("chill",  "Chill",   "4"),
    ("dance",  "Dance",   "5"),
    ("bjj",    "BJJ",     "6"),
    ("yuki",   "Yuki",    "7"),
]

// MARK: - App Delegate

/// Keeps the app running when all windows are closed (menu bar stays active).
/// Also handles sleep/wake reconnection.
class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        false
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Restore volume from last session
        let saved = UserDefaults.standard.float(forKey: "soluna_volume")
        if saved > 0 {
            Task { @MainActor in
                AudioReceiver.shared.volume = saved
                SDKAudioReceiver.shared.volume = saved
            }
        }

        // Sleep/wake auto-reconnect
        NSWorkspace.shared.notificationCenter.addObserver(
            self, selector: #selector(handleWake),
            name: NSWorkspace.didWakeNotification, object: nil
        )

        // Start built-in web dashboard on port 8400
        Task { @MainActor in
            DashboardServer.shared.start()
        }
    }

    @objc private func handleWake() {
        Task { @MainActor in
            let sdk = SDKAudioReceiver.shared
            // If was playing before sleep, reconnect after short delay
            if sdk.isPlaying {
                sdk.stop()
                try? await Task.sleep(nanoseconds: 2_000_000_000)
                sdk.start()
            }
        }
    }

    func applicationDockMenu(_ sender: NSApplication) -> NSMenu? {
        let menu = NSMenu()
        menu.addItem(withTitle: "Show Soluna", action: #selector(showWindow), keyEquivalent: "")
        return menu
    }

    @objc func showWindow() {
        NSApp.activate(ignoringOtherApps: true)
        if let window = NSApp.windows.first(where: { $0.canBecomeMain }) {
            window.makeKeyAndOrderFront(nil)
        }
    }
}

// MARK: - App

@main
struct SolunaReceiverApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

    @StateObject private var receiver = AudioReceiver.shared
    @StateObject private var speakers = SpeakersController.shared
    @StateObject private var remoteControl = RemoteControlServer()
    @StateObject private var sdk = SDKAudioReceiver.shared

    @AppStorage("channel") private var channel = "soluna"

    var body: some Scene {
        WindowGroup {
            ContentView(receiver: receiver, speakers: speakers)
                .frame(minWidth: 400, idealWidth: 900, minHeight: 600, idealHeight: 700)
                .onAppear {
                    remoteControl.start(receiver: receiver)
                    setupNowPlaying()
                }
                // Save volume on change
                .onChange(of: receiver.volume) { v in
                    UserDefaults.standard.set(v, forKey: "soluna_volume")
                }
        }
        .commands {
            // Playback commands
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

            // Channel shortcuts: Cmd+1 through Cmd+7
            CommandMenu("Channels") {
                ForEach(channelShortcuts, id: \.id) { ch in
                    Button(ch.label) {
                        channel = ch.id
                        UserDefaults.standard.set(ch.id, forKey: "channel")
                        sdk.setChannel(ch.id)
                        updateNowPlaying(channel: ch.label)
                    }
                    .keyboardShortcut(ch.key, modifiers: [.command])
                }
            }
        }

        // Menu bar mini player
        MenuBarExtra {
            MenuBarMiniPlayer()
        } label: {
            // Show channel name next to icon
            Label(channelLabel(for: sdk.channel), systemImage: sdk.isPlaying ? "speaker.wave.2.fill" : "speaker.slash")
        }
    }

    private func channelLabel(for id: String) -> String {
        channelShortcuts.first(where: { $0.id == id })?.label ?? id
    }

    // MARK: - Now Playing (Control Center + media keys)

    private func setupNowPlaying() {
        let center = MPRemoteCommandCenter.shared()
        center.playCommand.addTarget { [self] _ in
            if !receiver.isPlaying { receiver.start() }
            return .success
        }
        center.pauseCommand.addTarget { [self] _ in
            if receiver.isPlaying { receiver.toggle() }
            return .success
        }
        center.togglePlayPauseCommand.addTarget { [self] _ in
            receiver.toggle()
            return .success
        }
        // Next/previous channel
        center.nextTrackCommand.isEnabled = true
        center.nextTrackCommand.addTarget { [self] _ in
            switchChannel(offset: 1)
            return .success
        }
        center.previousTrackCommand.isEnabled = true
        center.previousTrackCommand.addTarget { [self] _ in
            switchChannel(offset: -1)
            return .success
        }
        center.changePlaybackPositionCommand.isEnabled = false

        updateNowPlaying(channel: channelLabel(for: channel))
    }

    private func switchChannel(offset: Int) {
        let ids = channelShortcuts.map(\.id)
        guard let idx = ids.firstIndex(of: channel) else { return }
        let newIdx = (idx + offset + ids.count) % ids.count
        let newCh = ids[newIdx]
        channel = newCh
        UserDefaults.standard.set(newCh, forKey: "channel")
        sdk.setChannel(newCh)
        updateNowPlaying(channel: channelShortcuts[newIdx].label)
    }

    private func updateNowPlaying(channel: String) {
        var info = [String: Any]()
        info[MPMediaItemPropertyTitle] = "Soluna Radio"
        info[MPMediaItemPropertyArtist] = channel
        info[MPNowPlayingInfoPropertyPlaybackRate] = receiver.isPlaying ? 1.0 : 0.0
        MPNowPlayingInfoCenter.default().nowPlayingInfo = info
        MPNowPlayingInfoCenter.default().playbackState = receiver.isPlaying ? .playing : .paused
    }
}

// MARK: - Menu Bar Mini Player

struct MenuBarMiniPlayer: View {
    @ObservedObject private var receiver = AudioReceiver.shared
    @ObservedObject private var sdk = SDKAudioReceiver.shared
    @ObservedObject private var speakers = SpeakersController.shared
    @AppStorage("channel") private var channel = "soluna"

    var body: some View {
        VStack(spacing: 8) {
            // Status
            HStack {
                Circle()
                    .fill(statusColor)
                    .frame(width: 8, height: 8)
                Text(sdk.isPlaying ? "Receiving" : sdk.state.rawValue)
                    .font(.headline)
                Spacer()
                if sdk.packetsReceived > 0 {
                    Text(formatNum(sdk.packetsReceived))
                        .font(.caption.monospacedDigit())
                        .foregroundColor(.secondary)
                }
            }

            Divider()

            // Channel picker
            ForEach(channelShortcuts, id: \.id) { ch in
                Button {
                    channel = ch.id
                    UserDefaults.standard.set(ch.id, forKey: "channel")
                    sdk.setChannel(ch.id)
                } label: {
                    HStack {
                        Text(ch.label)
                        Spacer()
                        if channel == ch.id {
                            Image(systemName: "checkmark")
                        }
                    }
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
                    Button("Sync") { speakers.recalculateAllDelays() }
                        .font(.caption)
                }
            }

            Divider()

            // Dashboard URL
            HStack {
                Image(systemName: "network")
                    .foregroundColor(.secondary)
                Text("http://\(DashboardServer.localIPAddress()):\(DashboardServer.port)")
                    .font(.caption.monospacedDigit())
                    .foregroundColor(.secondary)
                    .textSelection(.enabled)
            }

            Divider()

            // Launch at Login toggle
            Toggle("Launch at Login", isOn: Binding(
                get: { SMAppService.mainApp.status == .enabled },
                set: { enable in
                    do {
                        if enable { try SMAppService.mainApp.register() }
                        else { try SMAppService.mainApp.unregister() }
                    } catch {
                        print("[Soluna] LaunchAtLogin error: \(error)")
                    }
                }
            ))
            .font(.caption)

            Divider()

            Button("Show Window") {
                NSApp.activate(ignoringOtherApps: true)
                if let window = NSApp.windows.first(where: { $0.canBecomeMain }) {
                    window.makeKeyAndOrderFront(nil)
                }
            }
            .keyboardShortcut("o", modifiers: [.command])

            Button("Quit Soluna") {
                NSApplication.shared.terminate(nil)
            }
        }
        .padding(12)
        .frame(width: 260)
    }

    private var statusColor: Color {
        if sdk.isReceivingAudio { return .green }
        switch sdk.state {
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
