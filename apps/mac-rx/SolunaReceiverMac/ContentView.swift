//
//  ContentView.swift
//  SolunaReceiverMac
//

import SwiftUI

// MARK: - Root

struct ContentView: View {
    @StateObject private var receiver = AudioReceiver()
    @StateObject private var speakers = SpeakersController()
    @StateObject private var relay    = PeerRelayManager.shared
    @State private var showSettings   = false
    @State private var showAddSpeaker = false
    @State private var newName        = ""
    @State private var newHost        = ""
    @State private var masterVolume: Float = 1.0
    @State private var masterMuted    = false
    @State private var masterDelayMs: Int = 40

    @AppStorage("autoConnect") private var autoConnect = false

    var body: some View {
        ScrollView {
            VStack(spacing: 20) {
                heroSection
                relayBanner
                if receiver.state == .receiving || receiver.packetsReceived > 0 {
                    statsRow
                        .transition(.move(edge: .top).combined(with: .opacity))
                }
                speakersCard
            }
            .padding(.horizontal, 16)
            .padding(.top, 8)
            .padding(.bottom, 32)
        }
        .background(Color(nsColor: .windowBackgroundColor))
        .toolbar {
            ToolbarItem(placement: .automatic) {
                Button(action: { showSettings = true }) {
                    Image(systemName: "gearshape")
                        .foregroundColor(.secondary)
                }
            }
        }
        .navigationTitle("Soluna")
        .sheet(isPresented: $showSettings) { SettingsView(receiver: receiver) }
        .sheet(isPresented: $showAddSpeaker, onDismiss: { newName = ""; newHost = "" }) {
            addSpeakerSheet
        }
        .onAppear {
            speakers.audioReceiver = receiver
            loadSavedSettings()
            if autoConnect { receiver.start() }
            Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { _ in
                Task { @MainActor in speakers.applyServerRxDelay() }
            }
        }
        .animation(.spring(response: 0.35, dampingFraction: 0.8), value: receiver.state.rawValue)
    }

    // MARK: - Relay Banner

    @ViewBuilder
    private var relayBanner: some View {
        if relay.isScanning {
            HStack(spacing: 8) {
                ProgressView()
                    .scaleEffect(0.8)
                Text("Near devices scanning...")
                    .font(.footnote.weight(.medium))
                    .foregroundColor(.secondary)
                Spacer()
                Text(relay.channel)
                    .font(.caption2)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 3)
                    .background(Color(nsColor: .tertiaryLabelColor).opacity(0.2))
                    .foregroundColor(.secondary)
                    .clipShape(Capsule())
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 10)
            .background(Color(nsColor: .tertiaryLabelColor).opacity(0.1))
            .clipShape(RoundedRectangle(cornerRadius: 12))
            .transition(.move(edge: .top).combined(with: .opacity))
        }

        switch relay.role {
        case .direct where relay.connectedPeerCount > 0:
            HStack(spacing: 8) {
                Image(systemName: "antenna.radiowaves.left.and.right")
                    .foregroundColor(.green)
                Text("\(relay.connectedPeerCount) devices relaying")
                    .font(.footnote.weight(.semibold))
                    .foregroundColor(.green)
                Spacer()
                Text("Relay")
                    .font(.caption2)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 3)
                    .background(Color.green.opacity(0.15))
                    .foregroundColor(.green)
                    .clipShape(Capsule())
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 10)
            .background(Color.green.opacity(0.08))
            .clipShape(RoundedRectangle(cornerRadius: 12))
            .transition(.move(edge: .top).combined(with: .opacity))

        case .peer(let source):
            HStack(spacing: 8) {
                Image(systemName: "antenna.radiowaves.left.and.right")
                    .foregroundColor(.orange)
                VStack(alignment: .leading, spacing: 1) {
                    Text("Relay receiving")
                        .font(.footnote.weight(.semibold))
                        .foregroundColor(.orange)
                    Text(source)
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                Spacer()
                Text("Peer")
                    .font(.caption2)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 3)
                    .background(Color.orange.opacity(0.15))
                    .foregroundColor(.orange)
                    .clipShape(Capsule())
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 10)
            .background(Color.orange.opacity(0.08))
            .clipShape(RoundedRectangle(cornerRadius: 12))
            .transition(.move(edge: .top).combined(with: .opacity))

        case .direct:
            EmptyView()
        }
    }

    // MARK: - Hero

    private var heroSection: some View {
        VStack(spacing: 20) {
            Button(action: togglePlayback) {
                ZStack {
                    Circle()
                        .fill(heroAccent.opacity(0.12))
                        .frame(width: 100, height: 100)
                        .shadow(color: heroAccent.opacity(0.25), radius: 16, y: 4)

                    if receiver.state == .connecting {
                        ProgressView()
                            .scaleEffect(1.3)
                            .accentColor(heroAccent)
                    } else {
                        Image(systemName: heroIcon)
                            .font(.system(size: 36, weight: .semibold))
                            .foregroundColor(heroAccent)
                    }
                }
            }
            .buttonStyle(.plain)
            .disabled(receiver.state == .connecting)

            // Status pill
            HStack(spacing: 6) {
                Circle()
                    .fill(heroAccent)
                    .frame(width: 7, height: 7)
                Text(receiver.state.rawValue)
                    .font(.subheadline.weight(.medium))
                    .foregroundColor(heroAccent)
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 6)
            .background(heroAccent.opacity(0.1))
            .clipShape(Capsule())
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 28)
        .background(Color(nsColor: .controlBackgroundColor))
        .cornerRadius(20)
    }

    // MARK: - Stats

    private var statsRow: some View {
        HStack(spacing: 8) {
            StatPill(value: formatNum(receiver.packetsReceived), label: "pkts", color: .green)
            if receiver.packetsDropped > 0 {
                let pct = receiver.packetsReceived > 0
                    ? String(format: "%.1f%%", Double(receiver.packetsDropped) / Double(receiver.packetsReceived) * 100)
                    : "—"
                StatPill(value: pct, label: "drop", color: .orange)
            }
            StatPill(value: "\(receiver.bufferMs)ms", label: "buf", color: nil)
            if receiver.packetsConcealed > 0 {
                StatPill(value: formatNum(receiver.packetsConcealed), label: "plc", color: .yellow)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    // MARK: - Speakers card

    private var speakersCard: some View {
        VStack(spacing: 0) {
            // Header
            HStack {
                Text("Speakers")
                    .font(.headline)
                Spacer()
                Button(action: { showAddSpeaker = true }) {
                    Image(systemName: "plus")
                        .font(.subheadline.weight(.semibold))
                        .foregroundColor(.blue)
                        .frame(width: 28, height: 28)
                        .background(Color(nsColor: .tertiaryLabelColor).opacity(0.2))
                        .clipShape(Circle())
                }
                .buttonStyle(.plain)
            }
            .padding(.horizontal, 16)
            .padding(.top, 16)
            .padding(.bottom, 12)

            Divider().padding(.horizontal, 16)

            // Master volume + delay
            if speakers.anyConnected {
                MasterRow(volume: $masterVolume, muted: $masterMuted) { v in
                    speakers.setAllVolume(v)
                } onMute: { m in
                    speakers.setAllMute(m)
                }
                Divider().padding(.horizontal, 16)
                MasterDelayRow(delayMs: $masterDelayMs) { ms in
                    speakers.setAllDelay(ms)
                }
                Divider().padding(.horizontal, 16)
            }

            // Local Mac
            LocalSpeakerRow(receiver: receiver)

            // Remote speakers
            ForEach(speakers.speakers) { speaker in
                if let daemon = speakers.client(for: speaker.id) {
                    Divider().padding(.horizontal, 16)
                    RemoteSpeakerRow(
                        name: speaker.name,
                        daemon: daemon,
                        onRemove: { speakers.remove(speaker.id) }
                    )
                }
            }

            Divider().padding(.horizontal, 16)

            Button(action: { showAddSpeaker = true }) {
                Label("Add Speaker", systemImage: "plus.circle.fill")
                    .font(.subheadline)
                    .foregroundColor(.blue)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 14)
            }
            .buttonStyle(.plain)
        }
        .background(Color(nsColor: .controlBackgroundColor))
        .cornerRadius(20)
    }

    // MARK: - Add speaker sheet

    private var addSpeakerSheet: some View {
        VStack(spacing: 16) {
            Text("Add Speaker")
                .font(.headline)
                .padding(.top, 16)

            Form {
                TextField("Name (e.g. Mac, Living Room)", text: $newName)
                TextField("IP Address / Host", text: $newHost)
                    .disableAutocorrection(true)
            }
            .padding(.horizontal, 16)

            HStack {
                Button("Cancel") { showAddSpeaker = false }
                    .keyboardShortcut(.cancelAction)
                Spacer()
                Button("Add") {
                    speakers.add(name: newName, host: newHost)
                    showAddSpeaker = false
                }
                .keyboardShortcut(.defaultAction)
                .disabled(newHost.isEmpty)
            }
            .padding(.horizontal, 16)
            .padding(.bottom, 16)
        }
        .frame(width: 350)
    }

    // MARK: - Computed

    private var heroAccent: Color {
        switch receiver.state {
        case .receiving:  return .green
        case .connecting: return .orange
        case .error:      return .red
        case .stopped:    return .blue
        }
    }

    private var heroIcon: String {
        switch receiver.state {
        case .receiving: return "stop.fill"
        case .error:     return "arrow.clockwise"
        default:         return "play.fill"
        }
    }

    // MARK: - Helpers

    private func togglePlayback() {
        if receiver.state == .error {
            receiver.start()
        } else {
            receiver.toggle()
        }
    }

    private func loadSavedSettings() {
        let d = UserDefaults.standard
        if let g = d.string(forKey: "multicastGroup"), !g.isEmpty { receiver.multicastGroup = g }
        let port = d.integer(forKey: "port")
        if port > 0 { receiver.port = UInt16(port) }
        let ch = d.integer(forKey: "channels")
        if ch >= 2 { receiver.channels = UInt32(ch) }
    }

    private func formatNum(_ n: UInt64) -> String {
        n >= 1_000_000 ? String(format: "%.1fM", Double(n) / 1_000_000)
        : n >= 1_000   ? String(format: "%.1fK", Double(n) / 1_000)
        : "\(n)"
    }
}

// MARK: - StatPill

private struct StatPill: View {
    let value: String
    let label: String
    let color: Color?

    var body: some View {
        VStack(spacing: 2) {
            Text(value)
                .font(.system(size: 15, weight: .semibold, design: .monospaced))
                .foregroundColor(color ?? .secondary)
            Text(label.uppercased())
                .font(.system(size: 9, weight: .medium))
                .foregroundColor(.secondary)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(color.map { $0.opacity(0.1) } ?? Color(nsColor: .tertiaryLabelColor).opacity(0.2))
        .cornerRadius(10)
    }
}

// MARK: - MasterRow

private struct MasterRow: View {
    @Binding var volume: Float
    @Binding var muted: Bool
    let onVolume: (Float) -> Void
    let onMute: (Bool) -> Void

    var body: some View {
        HStack(spacing: 12) {
            Button {
                muted.toggle()
                onMute(muted)
            } label: {
                Image(systemName: muted ? "speaker.slash.fill" : "speaker.wave.3.fill")
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundColor(muted ? .red : .blue)
                    .frame(width: 32, height: 32)
                    .background(muted ? Color.red.opacity(0.1) : Color.blue.opacity(0.1))
                    .clipShape(Circle())
            }
            .buttonStyle(.plain)

            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text("All Speakers")
                        .font(.caption.weight(.semibold))
                        .foregroundColor(.secondary)
                    Spacer()
                    Text(muted ? "Muted" : "\(Int(volume * 100))%")
                        .font(.system(size: 12, design: .monospaced))
                        .foregroundColor(muted ? .red : .secondary)
                }
                Slider(value: $volume, in: 0...1)
                    .accentColor(.blue)
                    .onChange(of: volume) { v in
                        if muted { muted = false; onMute(false) }
                        onVolume(v)
                    }
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
    }
}

// MARK: - MasterDelayRow

private struct MasterDelayRow: View {
    @Binding var delayMs: Int
    let onDelay: (Int) -> Void

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: "timer")
                .font(.system(size: 14, weight: .semibold))
                .foregroundColor(.orange)
                .frame(width: 32, height: 32)
                .background(Color.orange.opacity(0.1))
                .clipShape(Circle())

            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text("Global Delay")
                        .font(.caption.weight(.semibold))
                        .foregroundColor(.secondary)
                    Spacer()
                    Text("\(delayMs)ms")
                        .font(.system(size: 12, design: .monospaced))
                        .foregroundColor(.orange)
                }
                Slider(value: Binding(
                    get: { Double(delayMs) },
                    set: { ms in
                        delayMs = Int(ms)
                        onDelay(delayMs)
                    }
                ), in: 0...2000, step: 10)
                .accentColor(.orange)
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
    }
}

// MARK: - LocalSpeakerRow

private struct LocalSpeakerRow: View {
    @ObservedObject var receiver: AudioReceiver

    var body: some View {
        HStack(spacing: 12) {
            speakerIcon(systemName: "desktopcomputer", connected: receiver.state == .receiving)

            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Text("This Mac")
                        .font(.subheadline.weight(.semibold))
                    if receiver.deviceHealth == .silenced {
                        Text("Auto-silenced")
                            .font(.caption2.weight(.semibold))
                            .foregroundColor(.red)
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(Color.red.opacity(0.12))
                            .clipShape(Capsule())
                    } else if receiver.deviceHealth == .stressed {
                        Text("Buffering")
                            .font(.caption2.weight(.semibold))
                            .foregroundColor(.orange)
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(Color.orange.opacity(0.12))
                            .clipShape(Capsule())
                    }
                    Spacer()
                    muteButton
                    Text(receiver.isMuted ? "Muted" : "\(Int(receiver.volume * 100))%")
                        .font(.system(size: 12, design: .monospaced))
                        .foregroundColor(receiver.isMuted ? .red : .secondary)
                        .frame(width: 42, alignment: .trailing)
                }

                Slider(value: $receiver.volume, in: 0...1)
                    .onChange(of: receiver.volume) { _ in
                        if receiver.isMuted { receiver.isMuted = false }
                    }
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 14)
    }

    private var muteButton: some View {
        Button {
            receiver.isMuted.toggle()
        } label: {
            Image(systemName: receiver.isMuted ? "speaker.slash.fill" : volumeIcon)
                .font(.system(size: 13, weight: .medium))
                .foregroundColor(receiver.isMuted ? .red : .secondary)
                .frame(width: 28, height: 28)
                .background(receiver.isMuted ? Color.red.opacity(0.1) : Color(nsColor: .tertiaryLabelColor).opacity(0.2))
                .clipShape(Circle())
        }
        .buttonStyle(.plain)
    }

    private var volumeIcon: String {
        receiver.volume < 0.01 ? "speaker.fill"
        : receiver.volume < 0.5 ? "speaker.wave.1.fill"
        : "speaker.wave.3.fill"
    }
}

// MARK: - RemoteSpeakerRow

private struct RemoteSpeakerRow: View {
    let name: String
    @ObservedObject var daemon: DaemonClient
    let onRemove: () -> Void

    var body: some View {
        HStack(spacing: 12) {
            speakerIcon(systemName: "hifispeaker.fill", connected: daemon.isConnected)

            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Text(name)
                        .font(.subheadline.weight(.semibold))
                        .foregroundColor(daemon.isConnected ? .primary : .secondary)
                    Spacer()
                    if daemon.measuredLatencyMs > 0 {
                        Text("\(daemon.measuredLatencyMs)ms")
                            .font(.system(size: 11, weight: .medium, design: .monospaced))
                            .foregroundColor(.green)
                            .padding(.horizontal, 7)
                            .padding(.vertical, 3)
                            .background(Color.green.opacity(0.1))
                            .clipShape(Capsule())
                    }
                    muteButton
                    Text(daemon.monitorMuted ? "Muted" : "\(Int(daemon.monitorVolume * 100))%")
                        .font(.system(size: 12, design: .monospaced))
                        .foregroundColor(daemon.monitorMuted ? .red : .secondary)
                        .frame(width: 42, alignment: .trailing)
                }
                .disabled(!daemon.isConnected)

                Slider(value: Binding(
                    get: { daemon.monitorVolume },
                    set: { v in
                        if daemon.monitorMuted { daemon.setMonitorMute(false) }
                        daemon.setMonitorVolume(v)
                    }
                ), in: 0...1)
                .accentColor(daemon.isConnected ? .blue : .secondary)
                .disabled(!daemon.isConnected)

                HStack {
                    Spacer()
                    Button(action: onRemove) {
                        Label("Remove", systemImage: "trash")
                            .font(.caption)
                            .foregroundColor(Color.red.opacity(0.7))
                    }
                    .buttonStyle(.plain)
                }
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 14)
    }

    private var muteButton: some View {
        Button {
            daemon.setMonitorMute(!daemon.monitorMuted)
        } label: {
            Image(systemName: daemon.monitorMuted ? "speaker.slash.fill" : remoteVolumeIcon)
                .font(.system(size: 13, weight: .medium))
                .foregroundColor(daemon.monitorMuted ? .red : .secondary)
                .frame(width: 28, height: 28)
                .background(daemon.monitorMuted ? Color.red.opacity(0.1) : Color(nsColor: .tertiaryLabelColor).opacity(0.2))
                .clipShape(Circle())
        }
        .buttonStyle(.plain)
    }

    private var remoteVolumeIcon: String {
        daemon.monitorVolume < 0.01 ? "speaker.fill"
        : daemon.monitorVolume < 0.5 ? "speaker.wave.1.fill"
        : "speaker.wave.3.fill"
    }
}

// MARK: - Shared helper

private func speakerIcon(systemName: String, connected: Bool) -> some View {
    ZStack(alignment: .bottomTrailing) {
        Image(systemName: systemName)
            .font(.system(size: 20))
            .foregroundColor(connected ? .primary : Color(nsColor: .tertiaryLabelColor))
            .frame(width: 32, height: 32)
        Circle()
            .fill(connected ? Color.green : Color(nsColor: .separatorColor))
            .frame(width: 8, height: 8)
            .overlay(Circle().stroke(Color(nsColor: .controlBackgroundColor), lineWidth: 1.5))
    }
}

#Preview {
    ContentView()
}
