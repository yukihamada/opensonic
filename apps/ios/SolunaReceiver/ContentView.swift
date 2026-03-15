//
//  ContentView.swift
//  SolunaReceiver
//
//  Modern 2025 design — glass morphism, gradient accents, dark-first
//

import SwiftUI
import MultipeerConnectivity
import UniformTypeIdentifiers

// MARK: - Root

struct ContentView: View {
    @EnvironmentObject var deepLink: DeepLinkManager
    @StateObject private var receiver    = AudioReceiver()
    @StateObject private var speakers    = SpeakersController()
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
    @State private var showPlayer    = false
    @State private var showDJPicker  = false
    @State private var showDJDeckView = false
    @State private var talkMode      = false
    @State private var quickChannelInput = ""
    @State private var showChannelCreate = false
    @State private var connectedDeviceHost: String? = nil
    @AppStorage("recentChannels") private var recentChannelsJSON: String = "[]"
    @StateObject private var channelStore = ChannelStore()
    @StateObject private var deviceBrowser = DeviceBrowser()

    private var recentChannels: [String] {
        (try? JSONDecoder().decode([String].self, from: Data(recentChannelsJSON.utf8))) ?? []
    }

    private func connectToDevice(_ device: SolunaLocalDevice) {
        connectedDeviceHost = device.host
        let ch = UserDefaults.standard.string(forKey: "channel") ?? "soluna"
        if receiver.state == .stopped || receiver.state == .error {
            // Start receiver first, then connect to device relay
            receiver.start()
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                receiver.connectRelay(group: ch, host: device.host, port: UInt16(device.port))
            }
        } else {
            receiver.connectRelay(group: ch, host: device.host, port: UInt16(device.port))
        }
    }

    private func disconnectDevice() {
        connectedDeviceHost = nil
        // Reconnect to WAN relay with current channel
        let ch = UserDefaults.standard.string(forKey: "channel") ?? "soluna"
        receiver.connectRelay(group: ch)
    }

    private func deleteRecentChannel(_ name: String) {
        var recent = recentChannels.filter { $0 != name }
        recentChannelsJSON = (try? String(data: JSONEncoder().encode(recent), encoding: .utf8)) ?? "[]"
    }

    private func switchChannel(_ name: String) {
        let ch = name.trimmingCharacters(in: .whitespaces)
        guard !ch.isEmpty else { return }
        UserDefaults.standard.set(ch, forKey: "channel")
        groupCode = ch
        var recent = recentChannels.filter { $0 != ch }
        recent.insert(ch, at: 0)
        if recent.count > 5 { recent = Array(recent.prefix(5)) }
        recentChannelsJSON = (try? String(data: JSONEncoder().encode(recent), encoding: .utf8)) ?? "[]"
        if receiver.state == .stopped || receiver.state == .error {
            receiver.start()
        } else {
            receiver.connectRelay(group: ch)
        }
    }

    var body: some View {
        ZStack {
            // Background gradient
            LinearGradient.solunaBg
                .ignoresSafeArea()

            ScrollView(showsIndicators: false) {
                VStack(spacing: 20) {
                    headerBar
                    // Device browser always visible
                    DeviceBrowserView(
                        browser: deviceBrowser,
                        connectedDeviceHost: connectedDeviceHost,
                        onSelect: { connectToDevice($0) },
                        onDisconnect: { disconnectDevice() }
                    )
                    .padding(16)
                    .glassCard()
                    // Channel/play/mic controls hidden when a device is connected
                    if connectedDeviceHost == nil {
                        ChannelBrowserView(
                            currentChannel: currentChannelName,
                            onSelect: { switchChannel($0) }
                        )
                        channelSection
                        heroPlayButton
                        if receiver.state == .receiving {
                            visualizerSection
                            micControls
                            nowPlayingSection
                            NowPlayingView(
                                channel: UserDefaults.standard.string(forKey: "channel") ?? "soluna",
                                isReceiving: true
                            )
                        }
                    } else {
                        deviceConnectedInfo
                    }
                    volumeControl
                    speakersCard
                    playerBar
                }
                .padding(.horizontal, 16)
                .padding(.top, 8)
                .padding(.bottom, 40)
            }
        }
        .preferredColorScheme(.dark)
        .sheet(isPresented: $showSettings) { SettingsView(receiver: receiver) }
        .sheet(isPresented: $showQR) {
            ChannelQRView(channel: UserDefaults.standard.string(forKey: "channel") ?? "soluna")
        }
        .sheet(isPresented: $showAddSpeaker, onDismiss: { newName = ""; newHost = "" }) {
            addSpeakerSheet
        }
        .sheet(isPresented: $showPlayer) {
            PlayerView(model: playerModel)
        }
        .sheet(isPresented: $showChannelCreate) {
            ChannelPurchaseView(
                store: channelStore,
                activeChannel: Binding(
                    get: { currentChannelName },
                    set: { switchChannel($0) }
                )
            )
        }
        .onAppear {
            speakers.audioReceiver = receiver
            playerModel.speakersController = speakers
            loadSavedSettings()
            receiver.autoStart()
            // Wire RTCP receiver reports: AudioReceiver → primary DaemonClient
            receiver.daemonClient = speakers.primaryDaemon
            Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { _ in
                Task { @MainActor in
                    speakers.applyServerRxDelay()
                    if !(playerModel.daemon?.isConnected ?? false) {
                        playerModel.rebindIfNeeded(speakers.primaryDaemon)
                    }
                    // Re-wire if primary daemon changed (e.g. reconnect)
                    receiver.daemonClient = speakers.primaryDaemon
                }
            }
            playerModel.daemon = speakers.primaryDaemon
        }
        .onChange(of: speakers.speakers.count) { _ in
            playerModel.rebindIfNeeded(speakers.primaryDaemon)
        }
        .onChange(of: deepLink.pendingChannel) { channel in
            guard let channel = channel, !channel.isEmpty else { return }
            deepLink.pendingChannel = nil
            switchChannel(channel)
        }
        .animation(.spring(response: 0.35, dampingFraction: 0.8), value: receiver.state.rawValue)
    }

    // MARK: - Header

    private var headerBar: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text("SOLUNA")
                    .font(.system(size: 28, weight: .bold, design: .rounded))
                    .foregroundStyle(LinearGradient.solunaAccent)
                Text(receiver.state == .receiving ? "Streaming" : "Ready")
                    .font(.system(size: 13, weight: .medium))
                    .foregroundColor(.secondary)
            }
            Spacer()
            Button(action: { showQR = true }) {
                Image(systemName: "qrcode")
                    .font(.system(size: 16, weight: .medium))
                    .foregroundColor(.white.opacity(0.7))
                    .frame(width: 36, height: 36)
                    .background(Color.white.opacity(0.08))
                    .clipShape(Circle())
            }
            Button(action: { showSettings = true }) {
                Image(systemName: "gearshape")
                    .font(.system(size: 16, weight: .medium))
                    .foregroundColor(.white.opacity(0.7))
                    .frame(width: 36, height: 36)
                    .background(Color.white.opacity(0.08))
                    .clipShape(Circle())
            }
        }
        .padding(.top, 4)
    }

    // MARK: - Channel Section

    private var currentChannelName: String {
        UserDefaults.standard.string(forKey: "channel") ?? "soluna"
    }

    private var isRelayConnected: Bool {
        receiver.relayState == .connected
    }

    private var channelPill: some View {
        let dotColor: Color = isRelayConnected ? .solunaLive : .gray
        let bgColor: Color = isRelayConnected ? Color.solunaLive.opacity(0.15) : Color.white.opacity(0.06)
        let borderColor: Color = isRelayConnected ? Color.solunaLive.opacity(0.3) : Color.white.opacity(0.08)

        return HStack(spacing: 6) {
            Circle().fill(dotColor).frame(width: 7, height: 7)
            Text(currentChannelName)
                .font(.system(size: 14, weight: .bold, design: .monospaced))
                .foregroundColor(.white)
                .lineLimit(1)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
        .background(Capsule().fill(bgColor).overlay(Capsule().stroke(borderColor, lineWidth: 0.5)))
    }

    private var channelInput: some View {
        HStack(spacing: 0) {
            TextField("Channel", text: $quickChannelInput)
                .font(.system(size: 14))
                .foregroundColor(.white)
                .autocorrectionDisabled()
                .autocapitalization(.none)
                .submitLabel(.go)
                .onSubmit {
                    switchChannel(quickChannelInput)
                    quickChannelInput = ""
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 8)

            Button(action: {
                switchChannel(quickChannelInput)
                quickChannelInput = ""
            }) {
                Image(systemName: "arrow.right.circle.fill")
                    .font(.system(size: 22))
                    .foregroundColor(
                        quickChannelInput.trimmingCharacters(in: .whitespaces).isEmpty
                            ? .gray : .solunaGradientMid
                    )
            }
            .disabled(quickChannelInput.trimmingCharacters(in: .whitespaces).isEmpty)
            .padding(.trailing, 8)
        }
        .background(
            Capsule().fill(Color.white.opacity(0.06))
                .overlay(Capsule().stroke(Color.white.opacity(0.08), lineWidth: 0.5))
        )
    }

    // Banner shown when directly connected to a nearby device
    private var deviceConnectedInfo: some View {
        VStack(spacing: 12) {
            HStack(spacing: 10) {
                Image(systemName: "laptopcomputer.and.iphone")
                    .font(.system(size: 28))
                    .foregroundColor(.solunaLive)
                VStack(alignment: .leading, spacing: 4) {
                    let devName = deviceBrowser.devices.first(where: { $0.host == connectedDeviceHost })?.name ?? (connectedDeviceHost ?? "Device")
                    Text("#\(devName)")
                        .font(.system(size: 17, weight: .bold, design: .monospaced))
                        .foregroundColor(.white)
                    Text("デバイスの音声を受信中")
                        .font(.caption)
                        .foregroundColor(.white.opacity(0.5))
                }
                Spacer()
            }
            .padding(16)
            .background(Color.solunaLive.opacity(0.1))
            .clipShape(RoundedRectangle(cornerRadius: 14))
        }
    }

    private var channelSection: some View {
        VStack(spacing: 10) {
            HStack(spacing: 10) {
                channelPill
                channelInput
            }

            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 8) {
                    ForEach(recentChannels, id: \.self) { ch in
                        RecentChannelChip(
                            name: ch,
                            isCurrent: ch == currentChannelName,
                            action: { switchChannel(ch) }
                        )
                        .contextMenu {
                            Button(role: .destructive) {
                                deleteRecentChannel(ch)
                            } label: {
                                Label("Remove", systemImage: "trash")
                            }
                        }
                    }

                    // Create custom channel button
                    Button(action: { showChannelCreate = true }) {
                        HStack(spacing: 5) {
                            Image(systemName: "plus")
                                .font(.system(size: 11, weight: .bold))
                            Text("Create")
                                .font(.system(size: 12, weight: .medium))
                        }
                        .foregroundColor(.solunaGradientMid)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 6)
                        .background(Color.solunaGradientMid.opacity(0.12))
                        .clipShape(Capsule())
                        .overlay(Capsule().stroke(Color.solunaGradientMid.opacity(0.3), lineWidth: 0.5))
                    }
                }
            }
        }
        .padding(16)
        .glassCard()
    }

    // MARK: - Hero Play Button

    private var heroPlayButton: some View {
        VStack(spacing: 16) {
            Button(action: {
                if receiver.state == .receiving { receiver.stop() }
                else { receiver.start() }
            }) {
                ZStack {
                    // Outer glow ring
                    GradientRing(isActive: receiver.state == .receiving)
                        .frame(width: 100, height: 100)
                        .opacity(receiver.state == .receiving ? 1 : 0.3)

                    // Inner circle
                    Circle()
                        .fill(
                            receiver.state == .receiving
                                ? Color.solunaLive.opacity(0.15)
                                : Color.white.opacity(0.06)
                        )
                        .frame(width: 88, height: 88)

                    if receiver.state == .connecting {
                        ProgressView()
                            .tint(.white)
                            .scaleEffect(1.3)
                    } else {
                        Group {
                            if receiver.state == .receiving {
                                Image(systemName: "stop.fill")
                                    .font(.system(size: 32, weight: .semibold))
                                    .foregroundColor(.solunaLive)
                            } else {
                                Image(systemName: "play.fill")
                                    .font(.system(size: 32, weight: .semibold))
                                    .foregroundStyle(LinearGradient.solunaAccent)
                            }
                        }
                    }
                }
            }
            .buttonStyle(.plain)

            // Status + stats
            HStack(spacing: 8) {
                Circle()
                    .fill(statusColor)
                    .frame(width: 6, height: 6)
                    .shadow(color: statusColor.opacity(0.6), radius: 4)
                Text(statusText)
                    .font(.system(size: 13, weight: .medium))
                    .foregroundColor(statusColor)
            }

            if receiver.state == .receiving {
                HStack(spacing: 12) {
                    signalBars(quality: signalQuality)
                    Text(String(format: "%.0fms", receiver.networkLatencyMs))
                        .font(.system(size: 11, weight: .medium, design: .monospaced))
                        .foregroundColor(.white.opacity(0.5))
                    Text("\(formatNum(receiver.packetsReceived)) pkts")
                        .font(.system(size: 11, weight: .medium, design: .monospaced))
                        .foregroundColor(.white.opacity(0.5))
                }
            }
        }
        .padding(.vertical, 8)
    }

    // MARK: - Visualizer

    private var visualizerSection: some View {
        WaveformVisualizer(level: receiver.outputLevel)
            .frame(height: 64)
            .padding(.horizontal, 8)
            .padding(.vertical, 12)
            .glassCard()
    }

    // MARK: - Mic Controls

    private var micControls: some View {
        VStack(spacing: 12) {
            // Button row
            HStack(spacing: 6) {
                if receiver.isPTTMode {
                    // Push-to-talk
                    HStack(spacing: 5) {
                        Image(systemName: pttPressed ? "mic.fill" : "mic.slash.fill")
                        Text(pttPressed ? "Talking..." : "Hold to Talk")
                            .font(.system(size: 13, weight: .medium))
                    }
                    .foregroundColor(pttPressed ? .solunaMic : .white.opacity(0.5))
                    .padding(.horizontal, 16)
                    .padding(.vertical, 10)
                    .background(pttPressed ? Color.solunaMic.opacity(0.15) : Color.white.opacity(0.06))
                    .clipShape(Capsule())
                    .overlay(
                        Capsule().stroke(pttPressed ? Color.solunaMic.opacity(0.3) : .clear, lineWidth: 0.5)
                    )
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
                    Button(action: { receiver.toggleMic() }) {
                        HStack(spacing: 5) {
                            Image(systemName: receiver.isMicTransmitting ? "mic.fill" : "mic.slash.fill")
                            Text(receiver.isMicTransmitting ? "Mic ON" : "Mic OFF")
                                .font(.system(size: 13, weight: .medium))
                        }
                    }
                    .buttonStyle(PillButtonStyle(color: .solunaMic, isActive: receiver.isMicTransmitting))
                }

                Button(action: { receiver.isPTTMode.toggle() }) {
                    HStack(spacing: 4) {
                        Image(systemName: receiver.isPTTMode ? "hand.tap.fill" : "hand.tap")
                        Text("PTT")
                            .font(.system(size: 13, weight: .medium))
                    }
                }
                .buttonStyle(PillButtonStyle(color: .orange, isActive: receiver.isPTTMode))

                Button(action: { receiver.isSyncMode.toggle() }) {
                    HStack(spacing: 4) {
                        Image(systemName: receiver.isSyncMode ? "metronome.fill" : "metronome")
                        Text("Sync")
                            .font(.system(size: 13, weight: .medium))
                    }
                }
                .buttonStyle(PillButtonStyle(color: .solunaGradientMid, isActive: receiver.isSyncMode))

                Button(action: {
                    talkMode.toggle()
                    receiver.setTalkMode(talkMode)
                }) {
                    HStack(spacing: 4) {
                        Image(systemName: talkMode ? "person.3.fill" : "person.3")
                        Text("Talk")
                            .font(.system(size: 13, weight: .medium))
                    }
                }
                .buttonStyle(PillButtonStyle(color: .solunaGradientStart, isActive: talkMode))

                Button(action: {
                    if receiver.isDJActive { receiver.stopDJBroadcast() }
                    else { showDJDeckView = true }
                }) {
                    HStack(spacing: 4) {
                        Image(systemName: receiver.isDJActive ? "stop.circle.fill" : "music.note.list")
                        Text(receiver.isDJActive ? "Stop DJ" : "DJ")
                            .font(.system(size: 13, weight: .medium))
                    }
                }
                .buttonStyle(PillButtonStyle(color: .purple, isActive: receiver.isDJActive || receiver.isDualDeckActive))
            }

            // Mic level meter
            if receiver.isMicTransmitting {
                MicLevelMeter(level: receiver.micInputLevel)
                    .frame(height: 6)
                    .padding(.horizontal, 24)
            }

            // Single-deck DJ progress bar
            if receiver.isDJActive {
                VStack(spacing: 4) {
                    HStack {
                        Text(receiver.djCurrentTrack ?? "Broadcasting...")
                            .font(.caption)
                            .foregroundColor(.purple)
                            .lineLimit(1)
                            .truncationMode(.middle)
                        Spacer()
                        Text("\(Int(receiver.djProgress * 100))%")
                            .font(.caption.monospacedDigit())
                            .foregroundColor(.purple)
                    }
                    ProgressView(value: Double(receiver.djProgress))
                        .tint(.purple)
                        .animation(.linear(duration: 0.4), value: receiver.djProgress)
                }
                .padding(.horizontal, 4)
            }
        }
        .padding(14)
        .glassCard()
        .sheet(isPresented: $showDJDeckView) {
            DJDeckView(receiver: receiver)
        }
    }

    // MARK: - Volume Control

    private var volumeControl: some View {
        HStack(spacing: 10) {
            Image(systemName: "speaker.fill")
                .font(.system(size: 12))
                .foregroundColor(.white.opacity(0.4))
            Slider(value: $masterVolume, in: 0...1)
                .tint(LinearGradient.solunaAccent)
            Image(systemName: "speaker.wave.3.fill")
                .font(.system(size: 12))
                .foregroundColor(.white.opacity(0.4))
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
        .glassCard()
    }

    // MARK: - Now Playing

    @ViewBuilder
    private var nowPlayingSection: some View {
        if let title = receiver.nowPlayingTitle {
            HStack(spacing: 14) {
                if let artworkURL = receiver.nowPlayingArtwork {
                    AsyncImage(url: artworkURL) { image in
                        image.resizable().scaledToFill()
                    } placeholder: {
                        RoundedRectangle(cornerRadius: 10)
                            .fill(Color.white.opacity(0.06))
                    }
                    .frame(width: 56, height: 56)
                    .cornerRadius(10)
                }

                VStack(alignment: .leading, spacing: 4) {
                    Text(title)
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundColor(.white)
                        .lineLimit(1)

                    if let artist = receiver.nowPlayingArtist {
                        Text(artist)
                            .font(.system(size: 13))
                            .foregroundColor(.white.opacity(0.5))
                            .lineLimit(1)
                    }
                }
                Spacer()
            }
            .padding(14)
            .glassCard()
        }
    }

    // MARK: - Speakers Card

    private var speakersCard: some View {
        VStack(spacing: 0) {
            // Header
            HStack {
                Text("Speakers")
                    .font(.system(size: 18, weight: .bold))
                    .foregroundColor(.white)
                Spacer()
                Button(action: { showAddSpeaker = true }) {
                    Image(systemName: "plus")
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundStyle(LinearGradient.solunaAccent)
                        .frame(width: 30, height: 30)
                        .background(Color.white.opacity(0.06))
                        .clipShape(Circle())
                }
            }
            .padding(.horizontal, 16)
            .padding(.top, 16)
            .padding(.bottom, 12)

            Divider().overlay(Color.white.opacity(0.06))

            // Master volume + delay
            if speakers.anyConnected {
                MasterRow(volume: $masterVolume, muted: $masterMuted) { v in
                    speakers.setAllVolume(v)
                } onMute: { m in
                    speakers.setAllMute(m)
                }
                Divider().overlay(Color.white.opacity(0.06))
                MasterDelayRow(delayMs: $masterDelayMs) { ms in
                    speakers.setAllDelay(ms)
                }
                Divider().overlay(Color.white.opacity(0.06))
            }

            // Local iPhone
            LocalSpeakerRow(receiver: receiver)

            // Remote speakers
            ForEach(speakers.speakers) { speaker in
                if let daemon = speakers.client(for: speaker.id) {
                    Divider().overlay(Color.white.opacity(0.06))
                    RemoteSpeakerRow(
                        name: speaker.name,
                        daemon: daemon,
                        onRemove: { speakers.remove(speaker.id) }
                    )
                }
            }

            Divider().overlay(Color.white.opacity(0.06))

            Button(action: { showAddSpeaker = true }) {
                Label("Add Speaker", systemImage: "plus.circle.fill")
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundStyle(LinearGradient.solunaAccent)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 14)
            }
        }
        .glassCard()
    }

    // MARK: - Player Bar

    private var playerBar: some View {
        Button(action: { showPlayer = true }) {
            HStack(spacing: 12) {
                Image(systemName: "music.note")
                    .font(.system(size: 14))
                    .foregroundColor(.solunaGradientEnd)
                    .frame(width: 36, height: 36)
                    .background(Color.solunaGradientEnd.opacity(0.12))
                    .clipShape(RoundedRectangle(cornerRadius: 8))
                Text("Music Player")
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundColor(.white)
                Spacer()
                Image(systemName: "chevron.right")
                    .font(.system(size: 12))
                    .foregroundColor(.white.opacity(0.3))
            }
            .padding(14)
        }
        .buttonStyle(.plain)
        .glassCard()
    }

    // MARK: - Add Speaker Sheet

    private var addSpeakerSheet: some View {
        NavigationView {
            Form {
                Section(header: Text("Connection")) {
                    TextField("Name (e.g. Mac, Living Room)", text: $newName)
                    TextField("IP Address / Host", text: $newHost)
                        .keyboardType(.URL)
                        .autocorrectionDisabled()
                        .autocapitalization(.none)
                }
            }
            .navigationTitle("Add Speaker")
            .navigationBarTitleDisplayMode(.inline)
            .navigationBarItems(
                leading: Button("Cancel") { showAddSpeaker = false },
                trailing: Button("Add") {
                    speakers.add(name: newName, host: newHost)
                    showAddSpeaker = false
                }
                .disabled(newHost.isEmpty)
            )
        }
    }

    // MARK: - Signal Quality

    private var statusColor: Color {
        switch receiver.state {
        case .receiving:  return .solunaLive
        case .connecting: return .orange
        case .error:      return .solunaMic
        case .stopped:    return .gray
        }
    }

    private var statusText: String {
        switch receiver.state {
        case .receiving:  return "Listening"
        case .connecting: return "Connecting..."
        case .error:      return "Error"
        case .stopped:    return "Stopped"
        }
    }

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
                    .fill(bar <= quality ? signalColor(quality) : Color.white.opacity(0.1))
                    .frame(width: 3, height: CGFloat(bar) * 4 + 4)
            }
        }
    }

    private func signalColor(_ quality: Int) -> Color {
        switch quality {
        case 1: return .solunaMic
        case 2: return .orange
        case 3: return .yellow
        default: return .solunaLive
        }
    }

    // MARK: - Helpers

    private func loadSavedSettings() {
        let d = UserDefaults.standard
        if let g = d.string(forKey: "multicastGroup"), !g.isEmpty { receiver.multicastGroup = g }
        let port = d.integer(forKey: "port")
        if port > 0 { receiver.port = UInt16(port) }
        let ch = d.integer(forKey: "channels")
        if ch >= 1 { receiver.channels = UInt32(ch) }
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
                RoundedRectangle(cornerRadius: 3)
                    .fill(Color.white.opacity(0.06))
                RoundedRectangle(cornerRadius: 3)
                    .fill(meterGradient)
                    .frame(width: max(0, geo.size.width * CGFloat(min(level, 1.0))))
                    .animation(.linear(duration: 0.1), value: level)
            }
        }
    }

    private var meterGradient: LinearGradient {
        LinearGradient(
            colors: [.solunaLive, .yellow, .solunaMic],
            startPoint: .leading, endPoint: .trailing
        )
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
                    .foregroundColor(muted ? .solunaMic : .solunaGradientMid)
                    .frame(width: 32, height: 32)
                    .background(muted ? Color.solunaMic.opacity(0.12) : Color.solunaGradientMid.opacity(0.12))
                    .clipShape(Circle())
            }

            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text("All Speakers")
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundColor(.white.opacity(0.5))
                    Spacer()
                    Text(muted ? "Muted" : "\(Int(volume * 100))%")
                        .font(.system(size: 12, design: .monospaced))
                        .foregroundColor(muted ? .solunaMic : .white.opacity(0.5))
                }
                Slider(value: $volume, in: 0...1)
                    .tint(.solunaGradientMid)
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
                .background(Color.orange.opacity(0.12))
                .clipShape(Circle())

            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text("Global Delay")
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundColor(.white.opacity(0.5))
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
                .tint(.orange)
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
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundColor(.white)
                    if receiver.deviceHealth == .silenced {
                        StatusBadge(text: "Auto-silenced", color: .solunaMic)
                    } else if receiver.deviceHealth == .stressed {
                        StatusBadge(text: "Buffering", color: .orange)
                    }
                    Spacer()
                    muteButton
                    Text(receiver.isMuted ? "Muted" : "\(Int(receiver.volume * 100))%")
                        .font(.system(size: 12, design: .monospaced))
                        .foregroundColor(receiver.isMuted ? .solunaMic : .white.opacity(0.4))
                        .frame(width: 42, alignment: .trailing)
                }

                Slider(value: $receiver.volume, in: 0...1)
                    .tint(.solunaGradientMid)
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
                .foregroundColor(receiver.isMuted ? .solunaMic : .white.opacity(0.5))
                .frame(width: 28, height: 28)
                .background(receiver.isMuted ? Color.solunaMic.opacity(0.12) : Color.white.opacity(0.06))
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
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundColor(daemon.isConnected ? .white : .white.opacity(0.4))
                    Spacer()
                    if daemon.measuredLatencyMs > 0 {
                        Text("\(daemon.measuredLatencyMs)ms")
                            .font(.system(size: 11, weight: .medium, design: .monospaced))
                            .foregroundColor(.solunaLive)
                            .padding(.horizontal, 8)
                            .padding(.vertical, 3)
                            .background(Color.solunaLive.opacity(0.12))
                            .clipShape(Capsule())
                    }
                    muteButton
                    Text(daemon.monitorMuted ? "Muted" : "\(Int(daemon.monitorVolume * 100))%")
                        .font(.system(size: 12, design: .monospaced))
                        .foregroundColor(daemon.monitorMuted ? .solunaMic : .white.opacity(0.4))
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
                .tint(.solunaGradientMid)
                .disabled(!daemon.isConnected)

                // Channel control
                if daemon.isConnected {
                    channelRow
                }

                HStack {
                    Spacer()
                    Button(action: onRemove) {
                        Label("Remove", systemImage: "trash")
                            .font(.system(size: 12))
                            .foregroundColor(.solunaMic.opacity(0.7))
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
                .foregroundColor(daemon.channelConnected ? .solunaLive : .white.opacity(0.3))
            Text(daemon.channel.isEmpty ? "default" : daemon.channel)
                .font(.system(size: 12, weight: .medium, design: .monospaced))
                .foregroundColor(daemon.channelConnected ? .solunaLive : .white.opacity(0.3))
            Spacer()
            TextField("ch", text: $editingChannel)
                .font(.system(size: 12))
                .textFieldStyle(.roundedBorder)
                .autocorrectionDisabled()
                .autocapitalization(.none)
                .frame(width: 100)
            Button("Switch") {
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
                .foregroundColor(daemon.monitorMuted ? .solunaMic : .white.opacity(0.5))
                .frame(width: 28, height: 28)
                .background(daemon.monitorMuted ? Color.solunaMic.opacity(0.12) : Color.white.opacity(0.06))
                .clipShape(Circle())
        }
    }

    private var remoteVolumeIcon: String {
        daemon.monitorVolume < 0.01 ? "speaker.fill"
        : daemon.monitorVolume < 0.5 ? "speaker.wave.1.fill"
        : "speaker.wave.3.fill"
    }
}

// MARK: - Status Badge

private struct StatusBadge: View {
    let text: String
    let color: Color

    var body: some View {
        Text(text)
            .font(.system(size: 10, weight: .semibold))
            .foregroundColor(color)
            .padding(.horizontal, 7)
            .padding(.vertical, 2)
            .background(color.opacity(0.12))
            .clipShape(Capsule())
    }
}

// MARK: - RecentChannelChip

private struct RecentChannelChip: View {
    let name: String
    let isCurrent: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Text(name)
                .font(.system(size: 12, weight: isCurrent ? .bold : .medium, design: .monospaced))
                .foregroundColor(isCurrent ? .white : .white.opacity(0.5))
                .padding(.horizontal, 12)
                .padding(.vertical, 6)
                .background(isCurrent ? Color.solunaGradientMid : Color.white.opacity(0.06))
                .clipShape(Capsule())
        }
    }
}

// MARK: - Shared helper

private func speakerIcon(systemName: String, connected: Bool) -> some View {
    ZStack(alignment: .bottomTrailing) {
        Image(systemName: systemName)
            .font(.system(size: 20))
            .foregroundColor(connected ? .white : .white.opacity(0.3))
            .frame(width: 32, height: 32)
        Circle()
            .fill(connected ? Color.solunaLive : Color.white.opacity(0.1))
            .frame(width: 8, height: 8)
            .overlay(Circle().stroke(Color.solunaSurface, lineWidth: 1.5))
    }
}

#Preview {
    ContentView()
}
