//
//  ContentView.swift
//  SolunaReceiver
//
//  Main UI for Soluna network audio receiver
//

import SwiftUI

struct ContentView: View {
    @StateObject private var receiver = AudioReceiver()
    @StateObject private var speakers = SpeakersController()
    @State private var showingSettings  = false
    @State private var showingError     = false
    @State private var showAddSpeaker   = false
    @State private var newSpeakerName   = ""
    @State private var newSpeakerHost   = ""
    @State private var masterVolume: Float = 1.0
    @State private var masterMuted = false

    @AppStorage("autoConnect") private var autoConnect = false

    var body: some View {
        NavigationView {
            VStack(spacing: 24) {
                Spacer()

                // ── Status indicator ───────────────────────────────────────
                VStack(spacing: 16) {
                    ZStack {
                        Circle()
                            .fill(statusGradient)
                            .frame(width: 160, height: 160)
                            .shadow(color: statusColor.opacity(0.5), radius: 20)

                        Circle()
                            .fill(Color(.systemBackground))
                            .frame(width: 140, height: 140)

                        Button(action: {
                            withAnimation(.spring(response: 0.3, dampingFraction: 0.7)) {
                                togglePlayback()
                            }
                        }) {
                            Image(systemName: buttonIcon)
                                .font(.system(size: 50))
                                .foregroundColor(buttonColor)
                                .offset(x: receiver.isPlaying ? 0 : 4)
                        }
                        .disabled(receiver.state == .connecting)
                    }

                    HStack(spacing: 8) {
                        Circle()
                            .fill(statusColor)
                            .frame(width: 8, height: 8)
                        Text(receiver.state.rawValue)
                            .font(.headline)
                            .foregroundColor(.secondary)
                    }

                    if receiver.state == .error {
                        Text("Tap to retry")
                            .font(.caption)
                            .foregroundColor(.secondary)
                            .padding(.top, 4)
                    }
                }

                // ── Stats ──────────────────────────────────────────────────
                if receiver.state == .receiving || receiver.packetsReceived > 0 {
                    statsCard
                        .transition(.move(edge: .bottom).combined(with: .opacity))
                }

                Spacer()

                // ── Speakers ───────────────────────────────────────────────
                speakersSection

                Spacer(minLength: 0)
            }
            .padding()
            .navigationTitle("Soluna Rx")
            .navigationBarItems(trailing:
                Button(action: { showingSettings = true }) {
                    Image(systemName: "gearshape")
                }
            )
            .sheet(isPresented: $showingSettings) {
                SettingsView(receiver: receiver)
            }
            .sheet(isPresented: $showAddSpeaker, onDismiss: {
                newSpeakerName = ""
                newSpeakerHost = ""
            }) {
                addSpeakerSheet
            }
            .alert(isPresented: $showingError) {
                Alert(
                    title: Text("Connection Error"),
                    message: Text(receiver.errorMessage ?? "Failed to connect to audio stream."),
                    primaryButton: .default(Text("Settings")) { showingSettings = true },
                    secondaryButton: .cancel()
                )
            }
            .onChange(of: receiver.errorMessage) { newValue in
                if newValue != nil { showingError = true }
            }
            .onAppear {
                speakers.audioReceiver = receiver
                loadSavedSettings()
                if autoConnect { receiver.start() }
            }
        }
    }

    // MARK: - Speakers section

    private var speakersSection: some View {
        VStack(spacing: 0) {
            // ── iPhone (local receiver) ──────────────────────────────────
            SpeakerRow(label: "iPhone", icon: "iphone", isConnected: receiver.state == .receiving) {
                // Volume + mute
                HStack(spacing: 10) {
                    Button(action: {
                        UIImpactFeedbackGenerator(style: .light).impactOccurred()
                        receiver.isMuted.toggle()
                    }) {
                        Image(systemName: receiver.isMuted ? "speaker.slash.fill" : localVolumeIcon)
                            .foregroundColor(receiver.isMuted ? .red : .secondary)
                            .frame(width: 24)
                            .contentShape(Rectangle())
                    }
                    Button(action: {
                        UIImpactFeedbackGenerator(style: .light).impactOccurred()
                        if receiver.isMuted { receiver.isMuted = false }
                        receiver.volume = max(0, receiver.volume - 0.1)
                    }) {
                        Image(systemName: "minus.circle.fill").foregroundColor(.secondary)
                    }
                    Slider(value: $receiver.volume, in: 0...1)
                        .onChange(of: receiver.volume) { _ in
                            if receiver.isMuted { receiver.isMuted = false }
                        }
                    Button(action: {
                        UIImpactFeedbackGenerator(style: .light).impactOccurred()
                        if receiver.isMuted { receiver.isMuted = false }
                        receiver.volume = min(1, receiver.volume + 0.1)
                    }) {
                        Image(systemName: "plus.circle.fill").foregroundColor(.secondary)
                    }
                    Text(receiver.isMuted ? "Mute" : "\(Int(receiver.volume * 100))%")
                        .font(.system(.caption, design: .monospaced))
                        .foregroundColor(receiver.isMuted ? .red : .secondary)
                        .frame(width: 44)
                }
                // Latency
                HStack(spacing: 10) {
                    Image(systemName: "waveform.path.ecg")
                        .foregroundColor(.secondary)
                        .frame(width: 24)
                    Slider(value: Binding(
                        get: { Double(receiver.bufferMs) },
                        set: { receiver.bufferMs = UInt32($0) }
                    ), in: 5...200, step: 5)
                    .accentColor(.secondary)
                    Text("\(receiver.bufferMs)ms")
                        .font(.system(.caption, design: .monospaced))
                        .foregroundColor(.secondary)
                        .frame(width: 44)
                }
            }

            // ── Remote speakers ──────────────────────────────────────────
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

            // ── Add speaker ──────────────────────────────────────────────
            Divider().padding(.horizontal, 16)
            Button(action: { showAddSpeaker = true }) {
                Label("スピーカーを追加", systemImage: "plus.circle")
                    .font(.subheadline)
                    .foregroundColor(.blue)
            }
            .padding(.vertical, 12)
        }
        .padding(.horizontal, 16)
        .padding(.bottom, 8)
    }

    // MARK: - Add Speaker sheet

    private var addSpeakerSheet: some View {
        NavigationView {
            Form {
                Section(header: Text("接続先")) {
                    TextField("名前 (例: Mac, リビング)", text: $newSpeakerName)
                    TextField("IP アドレス / ホスト", text: $newSpeakerHost)
                        .keyboardType(.URL)
                        .autocapitalization(.none)
                        .disableAutocorrection(true)
                }
            }
            .navigationTitle("スピーカーを追加")
            .navigationBarTitleDisplayMode(.inline)
            .navigationBarItems(
                leading: Button("キャンセル") { showAddSpeaker = false },
                trailing: Button("追加") {
                    speakers.add(name: newSpeakerName, host: newSpeakerHost)
                    showAddSpeaker = false
                }
                .disabled(newSpeakerHost.isEmpty)
            )
        }
    }

    // MARK: - Stats card

    private var statsCard: some View {
        VStack(spacing: 12) {
            HStack(spacing: 24) {
                statItem(value: formatNumber(receiver.packetsReceived), label: "Received", color: .green)
                if receiver.packetsDropped > 0 {
                    let dropPct = receiver.packetsReceived > 0
                        ? String(format: "%.1f%%", Double(receiver.packetsDropped) / Double(receiver.packetsReceived) * 100)
                        : "—"
                    statItem(value: dropPct, label: "Drop%", color: .orange)
                }
                statItem(value: "\(receiver.bufferMs)ms", label: "Buffer", color: .secondary)
                if receiver.packetsConcealed > 0 {
                    statItem(value: formatNumber(receiver.packetsConcealed), label: "PLC", color: .yellow)
                }
            }
            Text("\(receiver.multicastGroup):\(receiver.port)")
                .font(.caption)
                .foregroundColor(.secondary)
                .padding(.top, 4)
        }
        .padding()
        .background(Color(.secondarySystemBackground))
        .cornerRadius(16)
    }

    private func statItem(value: String, label: String, color: Color) -> some View {
        VStack(spacing: 4) {
            Text(value)
                .font(.system(.title, design: .monospaced))
                .fontWeight(.bold)
                .foregroundColor(color)
            Text(label)
                .font(.caption)
                .foregroundColor(.secondary)
        }
    }

    // MARK: - Computed Properties

    private var statusColor: Color {
        switch receiver.state {
        case .receiving:  return .green
        case .connecting: return .orange
        case .error:      return .red
        case .stopped:    return .gray
        }
    }

    private var statusGradient: LinearGradient {
        LinearGradient(
            colors: [statusColor.opacity(0.3), statusColor.opacity(0.1)],
            startPoint: .topLeading,
            endPoint: .bottomTrailing
        )
    }

    private var buttonIcon: String {
        switch receiver.state {
        case .receiving:  return "stop.fill"
        case .connecting: return "ellipsis"
        case .error:      return "arrow.clockwise"
        case .stopped:    return "play.fill"
        }
    }

    private var buttonColor: Color {
        switch receiver.state {
        case .receiving:  return .red
        case .connecting: return .orange
        case .error:      return .blue
        case .stopped:    return .blue
        }
    }

    private var localVolumeIcon: String {
        receiver.volume < 0.33 ? "speaker.fill"
        : receiver.volume < 0.66 ? "speaker.wave.1.fill"
        : "speaker.wave.3.fill"
    }

    // MARK: - Methods

    private func togglePlayback() {
        UIImpactFeedbackGenerator(style: .medium).impactOccurred()
        if receiver.state == .error {
            showingError = false
            receiver.start()
        } else {
            receiver.toggle()
        }
    }

    private func loadSavedSettings() {
        let defaults = UserDefaults.standard
        if let group = defaults.string(forKey: "multicastGroup"), !group.isEmpty {
            receiver.multicastGroup = group
        }
        let port = defaults.integer(forKey: "port")
        if port > 0 { receiver.port = UInt16(port) }
        let channels = defaults.integer(forKey: "channels")
        // Soluna transmits stereo (2ch); only apply if explicitly set to ≥2
        if channels >= 2 { receiver.channels = UInt32(channels) }
    }

    private func formatNumber(_ value: UInt64) -> String {
        value >= 1_000_000 ? String(format: "%.1fM", Double(value) / 1_000_000)
        : value >= 1_000   ? String(format: "%.1fK", Double(value) / 1_000)
        : "\(value)"
    }
}

// MARK: - SpeakerRow (header + content)

private struct SpeakerRow<Content: View>: View {
    let label: String
    let icon: String
    let isConnected: Bool
    @ViewBuilder let content: () -> Content

    var body: some View {
        VStack(spacing: 10) {
            HStack(spacing: 6) {
                Image(systemName: icon)
                    .foregroundColor(.secondary)
                    .frame(width: 20)
                Text(label)
                    .font(.subheadline.weight(.semibold))
                Spacer()
                Circle()
                    .fill(isConnected ? Color.green : Color.gray)
                    .frame(width: 7, height: 7)
            }
            content()
        }
        .padding(.vertical, 10)
    }
}

// MARK: - RemoteSpeakerRow

private struct RemoteSpeakerRow: View {
    let name: String
    @ObservedObject var daemon: DaemonClient
    let onRemove: () -> Void

    var body: some View {
        SpeakerRow(
            label: name,
            icon: "hifispeaker.2.fill",
            isConnected: daemon.isConnected
        ) {
            // Volume + mute
            HStack(spacing: 10) {
                Button(action: { daemon.setMonitorMute(!daemon.monitorMuted) }) {
                    Image(systemName: daemon.monitorMuted ? "speaker.slash.fill" : remoteVolumeIcon)
                        .foregroundColor(daemon.monitorMuted ? .red : .secondary)
                        .frame(width: 24)
                }
                Button(action: {
                    if daemon.monitorMuted { daemon.setMonitorMute(false) }
                    daemon.setMonitorVolume(max(0, daemon.monitorVolume - 0.1))
                }) {
                    Image(systemName: "minus.circle.fill").foregroundColor(.secondary)
                }
                Slider(value: Binding(
                    get: { daemon.monitorVolume },
                    set: { v in
                        if daemon.monitorMuted { daemon.setMonitorMute(false) }
                        daemon.setMonitorVolume(v)
                    }
                ), in: 0...1)
                Button(action: {
                    if daemon.monitorMuted { daemon.setMonitorMute(false) }
                    daemon.setMonitorVolume(min(1, daemon.monitorVolume + 0.1))
                }) {
                    Image(systemName: "plus.circle.fill").foregroundColor(.secondary)
                }
                Text(daemon.monitorMuted ? "Mute" : "\(Int(daemon.monitorVolume * 100))%")
                    .font(.system(.caption, design: .monospaced))
                    .foregroundColor(daemon.monitorMuted ? .red : .secondary)
                    .frame(width: 44)
            }
            .disabled(!daemon.isConnected)

            // Speaker delay (for sync with receivers)
            HStack(spacing: 10) {
                Image(systemName: "timer")
                    .foregroundColor(.secondary)
                    .frame(width: 24)
                Slider(value: Binding(
                    get: { Double(daemon.monitorDelayMs) },
                    set: { daemon.setMonitorDelay(Int($0)) }
                ), in: 0...200, step: 5)
                .accentColor(.secondary)
                Text("\(daemon.monitorDelayMs)ms")
                    .font(.system(.caption, design: .monospaced))
                    .foregroundColor(.secondary)
                    .frame(width: 44)
                if daemon.measuredLatencyMs > 0 {
                    Text("🔄\(daemon.measuredLatencyMs)")
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundColor(.green)
                        .frame(width: 48)
                }
            }
            .disabled(!daemon.isConnected)

            // Remove button
            HStack {
                Spacer()
                Button(action: onRemove) {
                    Label("削除", systemImage: "trash")
                        .font(.caption)
                        .foregroundColor(.red)
                }
            }
        }
        .padding(.vertical, 10)
    }

    private var remoteVolumeIcon: String {
        daemon.monitorVolume < 0.01 ? "speaker.fill"
        : daemon.monitorVolume < 0.5 ? "speaker.wave.1.fill"
        : "speaker.wave.2.fill"
    }
}

#Preview {
    ContentView()
}
