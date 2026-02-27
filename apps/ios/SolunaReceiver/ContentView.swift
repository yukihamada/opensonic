//
//  ContentView.swift
//  SolunaReceiver
//

import SwiftUI

// MARK: - Root

struct ContentView: View {
    @StateObject private var receiver = AudioReceiver()
    @StateObject private var speakers = SpeakersController()
    @State private var showSettings   = false
    @State private var showAddSpeaker = false
    @State private var newName        = ""
    @State private var newHost        = ""
    @State private var masterVolume: Float = 1.0
    @State private var masterMuted    = false

    @AppStorage("autoConnect") private var autoConnect = false

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 20) {
                    heroSection
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
            .background(Color(.systemGroupedBackground))
            .navigationTitle("Soluna")
            .navigationBarTitleDisplayMode(.large)
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button { showSettings = true } label: {
                        Image(systemName: "gearshape")
                            .symbolVariant(.none)
                            .foregroundStyle(.secondary)
                    }
                }
            }
            .sheet(isPresented: $showSettings) { SettingsView(receiver: receiver) }
            .sheet(isPresented: $showAddSpeaker, onDismiss: { newName = ""; newHost = "" }) {
                addSpeakerSheet
            }
            .onAppear {
                speakers.audioReceiver = receiver
                loadSavedSettings()
                if autoConnect { receiver.start() }
            }
            .animation(.spring(response: 0.35, dampingFraction: 0.8), value: receiver.state)
        }
    }

    // MARK: - Hero

    private var heroSection: some View {
        VStack(spacing: 20) {
            // Play button
            Button(action: togglePlayback) {
                ZStack {
                    Circle()
                        .fill(heroButtonBg)
                        .frame(width: 100, height: 100)
                        .shadow(color: heroAccent.opacity(0.3), radius: 16, y: 4)

                    if receiver.state == .connecting {
                        ProgressView()
                            .scaleEffect(1.3)
                            .tint(heroAccent)
                    } else {
                        Image(systemName: heroIcon)
                            .font(.system(size: 36, weight: .semibold))
                            .foregroundStyle(heroAccent)
                            .contentTransition(.symbolEffect(.replace))
                    }
                }
            }
            .disabled(receiver.state == .connecting)
            .sensoryFeedback(.impact(flexibility: .soft), trigger: receiver.state)

            // Status pill
            Label {
                Text(receiver.state.rawValue)
                    .font(.subheadline.weight(.medium))
                    .foregroundStyle(heroAccent)
            } icon: {
                Circle()
                    .fill(heroAccent)
                    .frame(width: 7, height: 7)
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 6)
            .background(heroAccent.opacity(0.1), in: Capsule())
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 28)
        .background(Color(.secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 20, style: .continuous))
    }

    // MARK: - Stats

    private var statsRow: some View {
        HStack(spacing: 8) {
            StatPill(
                value: formatNum(receiver.packetsReceived),
                label: "pkts",
                color: .green
            )
            if receiver.packetsDropped > 0 {
                let pct = receiver.packetsReceived > 0
                    ? String(format: "%.1f%%", Double(receiver.packetsDropped) / Double(receiver.packetsReceived) * 100)
                    : "—"
                StatPill(value: pct, label: "drop", color: .orange)
            }
            StatPill(value: "\(receiver.bufferMs)ms", label: "buf", color: .secondary)
            if receiver.packetsConcealed > 0 {
                StatPill(value: formatNum(receiver.packetsConcealed), label: "plc", color: .yellow)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    // MARK: - Speakers card

    private var speakersCard: some View {
        VStack(spacing: 0) {
            // Section header
            HStack {
                Text("Speakers")
                    .font(.headline)
                Spacer()
                Button(action: { showAddSpeaker = true }) {
                    Image(systemName: "plus")
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(.blue)
                        .frame(width: 28, height: 28)
                        .background(Color(.tertiarySystemFill), in: Circle())
                }
            }
            .padding(.horizontal, 16)
            .padding(.top, 16)
            .padding(.bottom, 12)

            Divider().padding(.horizontal, 16)

            // Master volume (when remotes connected)
            if speakers.anyConnected {
                MasterRow(volume: $masterVolume, muted: $masterMuted) { v in
                    speakers.setAllVolume(v)
                } onMute: { m in
                    speakers.setAllMute(m)
                }
                Divider().padding(.horizontal, 16)
            }

            // Local iPhone row
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

            // Add button
            Button(action: { showAddSpeaker = true }) {
                Label("Add Speaker", systemImage: "plus.circle.fill")
                    .font(.subheadline)
                    .foregroundStyle(.blue)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 14)
            }
        }
        .background(Color(.secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 20, style: .continuous))
    }

    // MARK: - Add speaker sheet

    private var addSpeakerSheet: some View {
        NavigationStack {
            Form {
                Section("接続先") {
                    TextField("名前（例: Mac, リビング）", text: $newName)
                    TextField("IPアドレス / ホスト", text: $newHost)
                        .keyboardType(.URL)
                        .autocorrectionDisabled()
                        .textInputAutocapitalization(.never)
                }
            }
            .navigationTitle("スピーカーを追加")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("キャンセル") { showAddSpeaker = false }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("追加") {
                        speakers.add(name: newName, host: newHost)
                        showAddSpeaker = false
                    }
                    .disabled(newHost.isEmpty)
                }
            }
        }
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

    private var heroButtonBg: some ShapeStyle {
        heroAccent.opacity(0.12)
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
    let color: Color

    var body: some View {
        VStack(spacing: 2) {
            Text(value)
                .font(.system(.subheadline, design: .monospaced, weight: .semibold))
                .foregroundStyle(color == .secondary ? Color.secondary : color)
            Text(label)
                .font(.system(size: 10, weight: .medium))
                .foregroundStyle(.tertiary)
                .textCase(.uppercase)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(color == .secondary ? Color(.tertiarySystemFill) : color.opacity(0.1),
                    in: RoundedRectangle(cornerRadius: 10, style: .continuous))
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
                    .foregroundStyle(muted ? .red : .blue)
                    .frame(width: 32, height: 32)
                    .background(muted ? Color.red.opacity(0.1) : Color.blue.opacity(0.1), in: Circle())
            }

            VStack(alignment: .leading, spacing: 2) {
                HStack {
                    Text("All Speakers")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.secondary)
                    Spacer()
                    Text(muted ? "Muted" : "\(Int(volume * 100))%")
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(muted ? .red : .secondary)
                }
                Slider(value: $volume, in: 0...1)
                    .tint(.blue)
                    .onChange(of: volume) { _, v in
                        if muted { muted = false; onMute(false) }
                        onVolume(v)
                    }
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
            // Status + icon
            ZStack(alignment: .bottomTrailing) {
                Image(systemName: "iphone")
                    .font(.system(size: 20))
                    .foregroundStyle(.secondary)
                    .frame(width: 32, height: 32)
                Circle()
                    .fill(receiver.state == .receiving ? Color.green : Color(.systemFill))
                    .frame(width: 8, height: 8)
                    .overlay(Circle().stroke(Color(.secondarySystemGroupedBackground), lineWidth: 1.5))
            }

            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Text("This iPhone")
                        .font(.subheadline.weight(.semibold))
                    Spacer()
                    Button {
                        receiver.isMuted.toggle()
                    } label: {
                        Image(systemName: receiver.isMuted ? "speaker.slash.fill" : volumeIcon)
                            .font(.system(size: 13, weight: .medium))
                            .foregroundStyle(receiver.isMuted ? .red : .secondary)
                            .frame(width: 28, height: 28)
                            .background(receiver.isMuted ? Color.red.opacity(0.1) : Color(.tertiarySystemFill),
                                        in: Circle())
                    }
                    Text(receiver.isMuted ? "Muted" : "\(Int(receiver.volume * 100))%")
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(receiver.isMuted ? .red : .secondary)
                        .frame(width: 42, alignment: .trailing)
                }

                Slider(value: $receiver.volume, in: 0...1)
                    .tint(.primary)
                    .onChange(of: receiver.volume) { _, _ in
                        if receiver.isMuted { receiver.isMuted = false }
                    }

                // Buffer slider
                HStack(spacing: 6) {
                    Image(systemName: "waveform")
                        .font(.system(size: 10))
                        .foregroundStyle(.tertiary)
                    Slider(value: Binding(
                        get: { Double(receiver.bufferMs) },
                        set: { receiver.bufferMs = UInt32($0) }
                    ), in: 5...200, step: 5)
                    .tint(.tertiary)
                    Text("\(receiver.bufferMs)ms")
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundStyle(.tertiary)
                        .frame(width: 38, alignment: .trailing)
                }
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 14)
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
    @State private var showDelay = false

    var body: some View {
        HStack(spacing: 12) {
            // Icon + status
            ZStack(alignment: .bottomTrailing) {
                Image(systemName: "hifispeaker.fill")
                    .font(.system(size: 20))
                    .foregroundStyle(daemon.isConnected ? Color.primary : Color(.tertiaryLabel))
                    .frame(width: 32, height: 32)
                Circle()
                    .fill(daemon.isConnected ? Color.green : Color(.systemFill))
                    .frame(width: 8, height: 8)
                    .overlay(Circle().stroke(Color(.secondarySystemGroupedBackground), lineWidth: 1.5))
            }

            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Text(name)
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(daemon.isConnected ? .primary : .secondary)
                    Spacer()

                    // Auto-sync badge
                    if daemon.measuredLatencyMs > 0 {
                        Text("\(daemon.measuredLatencyMs)ms")
                            .font(.system(size: 11, design: .monospaced, weight: .medium))
                            .foregroundStyle(.green)
                            .padding(.horizontal, 7)
                            .padding(.vertical, 3)
                            .background(Color.green.opacity(0.1), in: Capsule())
                    }

                    Button {
                        daemon.setMonitorMute(!daemon.monitorMuted)
                    } label: {
                        Image(systemName: daemon.monitorMuted ? "speaker.slash.fill" : remoteVolumeIcon)
                            .font(.system(size: 13, weight: .medium))
                            .foregroundStyle(daemon.monitorMuted ? .red : .secondary)
                            .frame(width: 28, height: 28)
                            .background(daemon.monitorMuted ? Color.red.opacity(0.1) : Color(.tertiarySystemFill),
                                        in: Circle())
                    }
                    Text(daemon.monitorMuted ? "Muted" : "\(Int(daemon.monitorVolume * 100))%")
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(daemon.monitorMuted ? .red : .secondary)
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
                .tint(daemon.isConnected ? .blue : .secondary)
                .disabled(!daemon.isConnected)

                // Delay row (tap chevron to expand)
                Button {
                    withAnimation(.spring(response: 0.3)) { showDelay.toggle() }
                } label: {
                    HStack(spacing: 4) {
                        Image(systemName: "timer")
                            .font(.system(size: 10))
                            .foregroundStyle(.tertiary)
                        Text("Delay")
                            .font(.system(size: 11, weight: .medium))
                            .foregroundStyle(.tertiary)
                        Spacer()
                        Image(systemName: "chevron.right")
                            .font(.system(size: 9, weight: .semibold))
                            .foregroundStyle(.quaternary)
                            .rotationEffect(.degrees(showDelay ? 90 : 0))
                    }
                }

                if showDelay {
                    HStack(spacing: 6) {
                        Slider(value: Binding(
                            get: { Double(daemon.monitorDelayMs) },
                            set: { daemon.setMonitorDelay(Int($0)) }
                        ), in: 0...200, step: 5)
                        .tint(.orange)
                        Text("\(daemon.monitorDelayMs)ms")
                            .font(.system(size: 11, design: .monospaced))
                            .foregroundStyle(.secondary)
                            .frame(width: 38, alignment: .trailing)
                    }
                    .disabled(!daemon.isConnected)
                    .transition(.move(edge: .top).combined(with: .opacity))
                }

                // Remove
                HStack {
                    Spacer()
                    Button(role: .destructive, action: onRemove) {
                        Label("削除", systemImage: "trash")
                            .font(.caption)
                            .foregroundStyle(.red.opacity(0.7))
                    }
                }
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 14)
    }

    private var remoteVolumeIcon: String {
        daemon.monitorVolume < 0.01 ? "speaker.fill"
        : daemon.monitorVolume < 0.5 ? "speaker.wave.1.fill"
        : "speaker.wave.3.fill"
    }
}

#Preview {
    ContentView()
}
