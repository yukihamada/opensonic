//
//  ContentView.swift
//  SolunaReceiver
//

import SwiftUI
import MultipeerConnectivity

// MARK: - Root

struct ContentView: View {
    @EnvironmentObject var deepLink: DeepLinkManager
    @StateObject private var receiver    = AudioReceiver()
    @StateObject private var speakers    = SpeakersController()
    @StateObject private var relay       = PeerRelayManager.shared
    @StateObject private var playerModel = PlayerModel()
    @State private var showSettings   = false
    @State private var showAddSpeaker = false
    @State private var newName        = ""
    @State private var newHost        = ""
    @State private var masterVolume: Float = 1.0
    @State private var masterMuted    = false
    @State private var masterDelayMs: Int = 40
    @AppStorage("streamMode") private var streamMode = "sync"
    @State private var groupCode     = ""
    @State private var showQR        = false
    @State private var pttPressed    = false

    var body: some View {
        TabView {
            // ── Receiver tab ──────────────────────────────────────────────
            NavigationView {
            ScrollView {
                VStack(spacing: 20) {
                    heroSection
                    if receiver.state == .receiving {
                        audioVisualizer
                    }
                    nowPlayingSection
                    wanGroupSection
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
            .background(Color(.systemGroupedBackground).ignoresSafeArea())
            .navigationTitle("Soluna")
            .navigationBarItems(trailing:
                HStack(spacing: 12) {
                    Button(action: { showQR = true }) {
                        Image(systemName: "qrcode")
                            .foregroundColor(.secondary)
                    }
                    Button(action: { showSettings = true }) {
                        Image(systemName: "gearshape")
                            .foregroundColor(.secondary)
                    }
                }
            )
            .sheet(isPresented: $showSettings) { SettingsView(receiver: receiver) }
            .sheet(isPresented: $showQR) {
                ChannelQRView(channel: UserDefaults.standard.string(forKey: "channel") ?? "soluna")
            }
            .sheet(isPresented: $showAddSpeaker, onDismiss: { newName = ""; newHost = "" }) {
                addSpeakerSheet
            }
            .onAppear {
                speakers.audioReceiver = receiver
                playerModel.speakersController = speakers
                loadSavedSettings()
                receiver.autoStart()
                Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { _ in
                    Task { @MainActor in
                        speakers.applyServerRxDelay()
                        // Rebind player if current daemon disconnected
                        if !(playerModel.daemon?.isConnected ?? false) {
                            playerModel.rebindIfNeeded(speakers.primaryDaemon)
                        }
                    }
                }
                // Bind player to first available daemon
                playerModel.daemon = speakers.primaryDaemon
            }
            .onChange(of: speakers.speakers.count) { _ in
                playerModel.rebindIfNeeded(speakers.primaryDaemon)
            }
            .onChange(of: deepLink.pendingChannel) { channel in
                guard let channel = channel, !channel.isEmpty else { return }
                deepLink.pendingChannel = nil
                // Switch to deep-linked channel and restart audio immediately
                UserDefaults.standard.set(channel, forKey: "channel")
                receiver.stop()
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
                    receiver.start()
                }
            }
            .animation(.spring(response: 0.35, dampingFraction: 0.8), value: receiver.state.rawValue)
        }
        .navigationViewStyle(.stack)
        .tabItem { Label("Receiver", systemImage: "antenna.radiowaves.left.and.right") }

        // ── Player tab ────────────────────────────────────────────────────
        NavigationView {
            PlayerView(model: playerModel)
                .navigationTitle("Player")
                .navigationBarTitleDisplayMode(.inline)
        }
        .navigationViewStyle(.stack)
        .tabItem { Label("Player", systemImage: "music.note") }
        }  // TabView
    }

    // MARK: - WAN Group Code

    @ViewBuilder
    private var wanGroupSection: some View {
        VStack(spacing: 10) {
            switch receiver.relayState {
            case .disconnected:
                VStack(spacing: 8) {
                    // Random channel button (free)
                    Button(action: {
                        let hex = (0..<3).map { _ in String(format: "%02x", UInt8.random(in: 0...255)) }.joined()
                        groupCode = hex
                        UserDefaults.standard.set(hex, forKey: "channel")
                        if receiver.state == .stopped || receiver.state == .error {
                            receiver.start()
                        } else {
                            receiver.connectRelay(group: hex)
                        }
                    }) {
                        HStack {
                            Image(systemName: "shuffle")
                            Text("ランダムチャンネル（無料）")
                                .font(.footnote.weight(.semibold))
                        }
                        .foregroundColor(.white)
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 10)
                        .background(LinearGradient(colors: [.green, .cyan], startPoint: .leading, endPoint: .trailing))
                        .clipShape(RoundedRectangle(cornerRadius: 8))
                    }

                    HStack(spacing: 8) {
                        Image(systemName: "globe")
                            .foregroundColor(.blue)
                        TextField("チャンネル名", text: $groupCode)
                            .textFieldStyle(.roundedBorder)
                            .autocorrectionDisabled()
                            .autocapitalization(.none)
                        Button(action: {
                            let code = groupCode.trimmingCharacters(in: .whitespaces)
                            guard !code.isEmpty else { return }
                            UserDefaults.standard.set(code, forKey: "channel")
                            if receiver.state == .stopped || receiver.state == .error {
                                receiver.start()
                            } else {
                                receiver.connectRelay(group: code)
                            }
                        }) {
                            Text("Listen")
                                .font(.footnote.weight(.semibold))
                                .foregroundColor(.white)
                                .padding(.horizontal, 14)
                                .padding(.vertical, 7)
                                .background(groupCode.trimmingCharacters(in: .whitespaces).isEmpty ? Color.gray : Color.blue)
                                .clipShape(Capsule())
                        }
                        .disabled(groupCode.trimmingCharacters(in: .whitespaces).isEmpty)
                    }
                }

            case .connecting:
                HStack(spacing: 8) {
                    ProgressView().scaleEffect(0.8)
                    Text("Joining...")
                        .font(.footnote.weight(.medium))
                        .foregroundColor(.secondary)
                    Spacer()
                }

            case .connected:
                HStack(spacing: 8) {
                    Image(systemName: "globe")
                        .foregroundColor(.green)
                    Text(receiver.relayGroup ?? "WAN")
                        .font(.footnote.weight(.semibold))
                        .foregroundColor(.green)
                        .padding(.horizontal, 8)
                        .padding(.vertical, 3)
                        .background(Color.green.opacity(0.12))
                        .clipShape(Capsule())
                    Spacer()
                    Button("Leave") {
                        receiver.disconnectRelay()
                        receiver.stop()
                    }
                        .font(.footnote.weight(.medium))
                        .foregroundColor(.red)
                }

            case .error:
                HStack(spacing: 8) {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundColor(.red)
                    Text(receiver.relayError ?? "Connection failed")
                        .font(.footnote)
                        .foregroundColor(.red)
                    Spacer()
                    Button("Retry") {
                        let code = groupCode.trimmingCharacters(in: .whitespaces)
                        guard !code.isEmpty else { return }
                        if receiver.state == .stopped || receiver.state == .error {
                            UserDefaults.standard.set(code, forKey: "channel")
                            receiver.start()
                        } else {
                            receiver.connectRelay(group: code)
                        }
                    }
                    .font(.footnote.weight(.medium))
                }
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 12)
        .background(Color(.secondarySystemGroupedBackground))
        .clipShape(RoundedRectangle(cornerRadius: 16))
    }

    // MARK: - Relay Banner

    @ViewBuilder
    private var relayBanner: some View {
        if relay.isScanning {
            HStack(spacing: 8) {
                ProgressView()
                    .scaleEffect(0.8)
                Text("近くの端末を検索中…")
                    .font(.footnote.weight(.medium))
                    .foregroundColor(.secondary)
                Spacer()
                Text(relay.channel)
                    .font(.caption2)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 3)
                    .background(Color(.tertiarySystemFill))
                    .foregroundColor(.secondary)
                    .clipShape(Capsule())
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 10)
            .background(Color(.tertiarySystemFill))
            .clipShape(RoundedRectangle(cornerRadius: 12))
            .transition(.move(edge: .top).combined(with: .opacity))
        }

        switch relay.role {
        case .direct where relay.connectedPeerCount > 0:
            HStack(spacing: 8) {
                Image(systemName: "antenna.radiowaves.left.and.right")
                    .foregroundColor(.green)
                Text("\(relay.connectedPeerCount) 台に中継中")
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
                    Text("中継受信中")
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
        VStack(spacing: 16) {
            ZStack {
                Circle()
                    .fill(heroAccent.opacity(0.12))
                    .frame(width: 80, height: 80)
                    .shadow(color: heroAccent.opacity(0.25), radius: 16, y: 4)

                if receiver.state == .connecting {
                    ProgressView()
                        .scaleEffect(1.3)
                        .accentColor(heroAccent)
                } else {
                    Image(systemName: heroIcon)
                        .font(.system(size: 30, weight: .semibold))
                        .foregroundColor(heroAccent)
                }
            }

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

            // Mic TX toggle + PTT + stop/start
            HStack(spacing: 16) {
                if receiver.state == .receiving {
                    if receiver.isPTTMode {
                        // PTT hold-to-talk mic button
                        HStack(spacing: 4) {
                            Image(systemName: pttPressed ? "mic.fill" : "mic.slash.fill")
                            Text(pttPressed ? "Talking..." : "Hold to Talk")
                                .font(.caption.weight(.medium))
                        }
                        .foregroundColor(pttPressed ? .red : .secondary)
                        .padding(.horizontal, 10)
                        .padding(.vertical, 5)
                        .background(pttPressed ? Color.red.opacity(0.12) : Color(.tertiarySystemFill))
                        .clipShape(Capsule())
                        .gesture(
                            DragGesture(minimumDistance: 0)
                                .onChanged { _ in
                                    if !pttPressed {
                                        pttPressed = true
                                        if !receiver.isMicTransmitting { receiver.toggleMic() }
                                    }
                                }
                                .onEnded { _ in
                                    pttPressed = false
                                    if receiver.isMicTransmitting { receiver.toggleMic() }
                                }
                        )
                    } else {
                        // Normal toggle mic button
                        Button(action: { receiver.toggleMic() }) {
                            HStack(spacing: 4) {
                                Image(systemName: receiver.isMicTransmitting ? "mic.fill" : "mic.slash.fill")
                                Text(receiver.isMicTransmitting ? "Mic ON" : "Mic OFF")
                                    .font(.caption.weight(.medium))
                            }
                            .foregroundColor(receiver.isMicTransmitting ? .red : .secondary)
                            .padding(.horizontal, 10)
                            .padding(.vertical, 5)
                            .background(receiver.isMicTransmitting ? Color.red.opacity(0.12) : Color(.tertiarySystemFill))
                            .clipShape(Capsule())
                        }
                    }

                    // PTT mode toggle
                    Button(action: { receiver.isPTTMode.toggle() }) {
                        HStack(spacing: 4) {
                            Image(systemName: receiver.isPTTMode ? "hand.tap.fill" : "hand.tap")
                            Text(receiver.isPTTMode ? "PTT" : "Hold")
                                .font(.caption.weight(.medium))
                        }
                        .foregroundColor(receiver.isPTTMode ? .orange : .secondary)
                        .padding(.horizontal, 10)
                        .padding(.vertical, 5)
                        .background(receiver.isPTTMode ? Color.orange.opacity(0.12) : Color(.tertiarySystemFill))
                        .clipShape(Capsule())
                    }
                }

                if receiver.state == .receiving {
                    Button(action: { receiver.isSyncMode.toggle() }) {
                        HStack(spacing: 4) {
                            Image(systemName: receiver.isSyncMode ? "metronome.fill" : "metronome")
                            Text(receiver.isSyncMode ? "Sync" : "Fast")
                                .font(.caption.weight(.medium))
                        }
                        .foregroundColor(receiver.isSyncMode ? .blue : .secondary)
                        .padding(.horizontal, 10)
                        .padding(.vertical, 5)
                        .background(receiver.isSyncMode ? Color.blue.opacity(0.12) : Color(.tertiarySystemFill))
                        .clipShape(Capsule())
                    }
                }

                if receiver.state == .receiving {
                    Button(action: { receiver.stop() }) {
                        Text("Stop")
                            .font(.caption.weight(.medium))
                            .foregroundColor(.secondary)
                    }
                } else if receiver.state == .stopped || receiver.state == .error {
                    Button(action: { receiver.start() }) {
                        Text("Reconnect")
                            .font(.caption.weight(.medium))
                            .foregroundColor(.blue)
                    }
                }
            }

            // Mic input level meter
            if receiver.isMicTransmitting {
                MicLevelMeter(level: receiver.micInputLevel)
                    .frame(height: 8)
                    .padding(.horizontal, 32)
            }

            // Stream mode toggle (Sync / Jam)
            Picker("Stream Mode", selection: $streamMode) {
                Text("Sync").tag("sync")
                Text("Jam").tag("jam")
            }
            .pickerStyle(.segmented)
            .frame(width: 180)

            // Network quality badge
            if receiver.state == .receiving {
                HStack(spacing: 6) {
                    signalBars(quality: signalQuality)
                    Text(String(format: "%.0fms", receiver.networkLatencyMs))
                        .font(.caption2.monospacedDigit())
                        .foregroundColor(.secondary)
                }
            }
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 24)
        .background(Color(.secondarySystemGroupedBackground))
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
            if receiver.isMicTransmitting {
                StatPill(value: formatNum(receiver.txPacketsSent), label: "tx", color: .red)
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
                        .background(Color(.tertiarySystemFill))
                        .clipShape(Circle())
                }
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

            // Local iPhone
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
        }
        .background(Color(.secondarySystemGroupedBackground))
        .cornerRadius(20)
    }

    // MARK: - Add speaker sheet

    private var addSpeakerSheet: some View {
        NavigationView {
            Form {
                Section(header: Text("接続先")) {
                    TextField("名前（例: Mac, リビング）", text: $newName)
                    TextField("IPアドレス / ホスト", text: $newHost)
                        .keyboardType(.URL)
                        .autocorrectionDisabled()
                        .autocapitalization(.none)
                }
            }
            .navigationTitle("スピーカーを追加")
            .navigationBarTitleDisplayMode(.inline)
            .navigationBarItems(
                leading: Button("キャンセル") { showAddSpeaker = false },
                trailing: Button("追加") {
                    speakers.add(name: newName, host: newHost)
                    showAddSpeaker = false
                }
                .disabled(newHost.isEmpty)
            )
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

    private var heroIcon: String {
        switch receiver.state {
        case .receiving:  return "waveform"
        case .connecting: return "antenna.radiowaves.left.and.right"
        case .error:      return "exclamationmark.triangle.fill"
        case .stopped:    return "moon.zzz.fill"
        }
    }

    // MARK: - Audio Visualizer

    private var audioVisualizer: some View {
        HStack(alignment: .bottom, spacing: 3) {
            ForEach(0..<16, id: \.self) { i in
                let level = receiver.outputLevel
                let phase = Double(i) * 0.4 + Date().timeIntervalSince1970 * 3.0
                let barHeight = max(4.0, CGFloat(level) * 60.0 * CGFloat(0.5 + 0.5 * sin(phase)))
                RoundedRectangle(cornerRadius: 2)
                    .fill(
                        LinearGradient(
                            colors: [.blue, .cyan, .green],
                            startPoint: .bottom,
                            endPoint: .top
                        )
                    )
                    .frame(maxWidth: .infinity)
                    .frame(height: barHeight)
                    .animation(.easeOut(duration: 0.05), value: level)
            }
        }
        .frame(height: 60)
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
        .background(Color(.secondarySystemGroupedBackground))
        .clipShape(RoundedRectangle(cornerRadius: 16))
    }

    // MARK: - Now Playing

    @ViewBuilder
    private var nowPlayingSection: some View {
        if let title = receiver.nowPlayingTitle {
            VStack(spacing: 8) {
                if let artworkURL = receiver.nowPlayingArtwork {
                    AsyncImage(url: artworkURL) { image in
                        image.resizable().scaledToFill()
                    } placeholder: {
                        Color(.tertiarySystemFill)
                    }
                    .frame(width: 80, height: 80)
                    .cornerRadius(12)
                }

                Text(title)
                    .font(.subheadline.weight(.semibold))
                    .lineLimit(1)

                if let artist = receiver.nowPlayingArtist {
                    Text(artist)
                        .font(.caption)
                        .foregroundColor(.secondary)
                        .lineLimit(1)
                }
            }
            .padding()
            .frame(maxWidth: .infinity)
            .background(Color(.secondarySystemBackground))
            .cornerRadius(16)
            .padding(.horizontal)
        }
    }

    // MARK: - Signal Quality

    private var signalQuality: Int {
        if receiver.packetLossPercent > 5 || receiver.networkLatencyMs > 500 { return 1 }
        if receiver.packetLossPercent > 2 || receiver.networkLatencyMs > 200 { return 2 }
        if receiver.packetLossPercent > 0.5 || receiver.networkLatencyMs > 50 { return 3 }
        return 4
    }

    @ViewBuilder
    private func signalBars(quality: Int) -> some View {
        HStack(spacing: 1.5) {
            ForEach(1...4, id: \.self) { bar in
                RoundedRectangle(cornerRadius: 1)
                    .fill(bar <= quality ? signalColor(quality) : Color.gray.opacity(0.3))
                    .frame(width: 3, height: CGFloat(bar) * 4 + 4)
            }
        }
    }

    private func signalColor(_ quality: Int) -> Color {
        switch quality {
        case 1: return .red
        case 2: return .orange
        case 3: return .yellow
        default: return .green
        }
    }

    // MARK: - Helpers

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

// MARK: - MicLevelMeter

private struct MicLevelMeter: View {
    let level: Float

    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                RoundedRectangle(cornerRadius: 4)
                    .fill(Color(.tertiarySystemFill))
                RoundedRectangle(cornerRadius: 4)
                    .fill(meterColor)
                    .frame(width: max(0, geo.size.width * CGFloat(min(level, 1.0))))
                    .animation(.linear(duration: 0.1), value: level)
            }
        }
    }

    private var meterColor: Color {
        if level > 0.8 { return .red }
        if level > 0.4 { return .yellow }
        return .green
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
        .background(color.map { $0.opacity(0.1) } ?? Color(.tertiarySystemFill))
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
            speakerIcon(systemName: "iphone", connected: receiver.state == .receiving)

            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Text("This iPhone")
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
            UIImpactFeedbackGenerator(style: .light).impactOccurred()
            receiver.isMuted.toggle()
        } label: {
            Image(systemName: receiver.isMuted ? "speaker.slash.fill" : volumeIcon)
                .font(.system(size: 13, weight: .medium))
                .foregroundColor(receiver.isMuted ? .red : .secondary)
                .frame(width: 28, height: 28)
                .background(receiver.isMuted ? Color.red.opacity(0.1) : Color(.tertiarySystemFill))
                .clipShape(Circle())
        }
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

                // Channel control
                if daemon.isConnected {
                    channelRow
                }

                HStack {
                    Spacer()
                    Button(action: onRemove) {
                        Label("削除", systemImage: "trash")
                            .font(.caption)
                            .foregroundColor(Color.red.opacity(0.7))
                    }
                }
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 14)
    }

    @State private var editingChannel = ""

    private var channelRow: some View {
        HStack(spacing: 6) {
            Image(systemName: "antenna.radiowaves.left.and.right")
                .font(.system(size: 11))
                .foregroundColor(daemon.channelConnected ? .green : .secondary)
            Text(daemon.channel.isEmpty ? "default" : daemon.channel)
                .font(.system(size: 12, weight: .medium, design: .monospaced))
                .foregroundColor(daemon.channelConnected ? .green : .secondary)
            Spacer()
            TextField("ch名", text: $editingChannel)
                .font(.system(size: 12))
                .textFieldStyle(.roundedBorder)
                .autocorrectionDisabled()
                .autocapitalization(.none)
                .frame(width: 100)
            Button("変更") {
                let ch = editingChannel.trimmingCharacters(in: .whitespaces)
                guard !ch.isEmpty else { return }
                daemon.setChannel(ch)
                editingChannel = ""
            }
            .font(.system(size: 11, weight: .semibold))
            .disabled(editingChannel.trimmingCharacters(in: .whitespaces).isEmpty)
        }
    }

    private var muteButton: some View {
        Button {
            UIImpactFeedbackGenerator(style: .light).impactOccurred()
            daemon.setMonitorMute(!daemon.monitorMuted)
        } label: {
            Image(systemName: daemon.monitorMuted ? "speaker.slash.fill" : remoteVolumeIcon)
                .font(.system(size: 13, weight: .medium))
                .foregroundColor(daemon.monitorMuted ? .red : .secondary)
                .frame(width: 28, height: 28)
                .background(daemon.monitorMuted ? Color.red.opacity(0.1) : Color(.tertiarySystemFill))
                .clipShape(Circle())
        }
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
            .foregroundColor(connected ? .primary : Color(.tertiaryLabel))
            .frame(width: 32, height: 32)
        Circle()
            .fill(connected ? Color.green : Color(.systemFill))
            .frame(width: 8, height: 8)
            .overlay(Circle().stroke(Color(.secondarySystemGroupedBackground), lineWidth: 1.5))
    }
}

#Preview {
    ContentView()
}
