//
//  ContentView.swift
//  SolunaReceiverMac
//
//  Spotify-style player — channels -> now playing -> controls (matching iOS design)
//

import SwiftUI
import UniformTypeIdentifiers

// MARK: - Extended Channel List

private let allChannels: [SolunaChannel] = [
    SolunaChannel(id: "soluna",  label: "Soluna",   icon: "sun.and.horizon.fill",  color: .solunaSol,           description: "The flagship mix"),
    SolunaChannel(id: "jazz",    label: "Jazz",     icon: "pianokeys",             color: .orange,              description: "Smooth jazz piano"),
    SolunaChannel(id: "lofi",    label: "Lo-Fi",    icon: "headphones",            color: .purple,              description: "Chill beats to relax"),
    SolunaChannel(id: "chill",   label: "Chill",    icon: "leaf.fill",             color: .solunaLuna,          description: "Easy listening vibes"),
    SolunaChannel(id: "dance",   label: "Dance",    icon: "bolt.heart.fill",       color: .solunaSolEnd,        description: "High-energy grooves"),
    SolunaChannel(id: "bjj",     label: "BJJ",      icon: "figure.martial.arts",   color: .solunaGradientMid,   description: "Training beats"),
    SolunaChannel(id: "yuki",    label: "Yuki",     icon: "snowflake",             color: .solunaLunaEnd,       description: "Yuki's personal mix"),
    SolunaChannel(id: "ambient", label: "Ambient",  icon: "cloud.fill",            color: .cyan,                description: "Atmospheric soundscapes"),
    SolunaChannel(id: "rock",    label: "Rock",     icon: "guitars.fill",          color: .red,                 description: "Classic & modern rock"),
    SolunaChannel(id: "edm",     label: "EDM",      icon: "waveform.path",         color: .mint,                description: "Electronic dance music"),
    SolunaChannel(id: "classical", label: "Classical", icon: "music.quarternote.3", color: .brown,               description: "Orchestral masterworks"),
    SolunaChannel(id: "hiphop",  label: "Hip-Hop",  icon: "mic.fill",              color: .yellow,              description: "Beats & rhymes"),
]

// MARK: - Root

struct ContentView: View {
    @ObservedObject var receiver: AudioReceiver
    @ObservedObject var speakers: SpeakersController
    @StateObject private var relay    = PeerRelayManager.shared
    @State private var groupCode       = ""
    @State private var showSettings   = false
    @State private var showFestivalMode = false
    @State private var showAddSpeaker = false
    @State private var addSpeakerTab  = 2     // 0 = local, 1 = network, 2 = channel
    @State private var newName        = ""
    @State private var newHost        = ""
    @State private var masterVolume: Float = 1.0
    @State private var masterMuted    = false
    @State private var masterDelayMs: Int = 40
    @State private var showSavePreset = false
    @State private var presetName     = ""
    @State private var showCreateGroup = false
    @State private var groupName       = ""

    @AppStorage("autoConnect") private var autoConnect = true
    @AppStorage("streamMode") private var streamMode = "sync"
    @AppStorage("channel") private var channel = "soluna"
    @AppStorage("recentChannels") private var recentChannelsData = ""

    @State private var isEditingChannel = false
    @State private var editedChannel    = ""
    @State private var sendExpanded     = false
    @State private var talkMode          = false

    @StateObject private var playerModel = PlayerModel()
    @StateObject private var deviceBrowser = DeviceBrowser()
    @State private var connectedDeviceHost: String? = nil

    @StateObject private var globalRegistry = GlobalDeviceRegistry()
    @State private var showDevicePicker = false
    @State private var isPulsing = false
    @State private var showDebug = false

    @State private var playerExpanded = false
    @State private var selectedGroupDevices: Set<UInt32> = []
    @State private var showMoreSection = false
    @State private var channelSearch = ""
    @State private var showBrowseAll = false
    @State private var showChannelSettings = false
    @State private var channelToEdit = ""

    /// Top-level tab: Listen vs Broadcast
    @State private var topTab: TopTab = .listen
    enum TopTab: String, CaseIterable { case listen = "Listen", broadcast = "Broadcast" }

    /// Broadcast channel name (defaults to current listening channel)
    @State private var broadcastChannel = ""

    // MARK: - Derived State

    private var isPlaying: Bool { receiver.state == .receiving }

    private var recentChannels: [String] {
        recentChannelsData
            .components(separatedBy: ",")
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
    }

    private var filteredChannels: [SolunaChannel] {
        let query = channelSearch.trimmingCharacters(in: .whitespaces).lowercased()
        if query.isEmpty { return showBrowseAll ? allChannels : Array(allChannels.prefix(6)) }
        return allChannels.filter {
            $0.label.lowercased().contains(query) ||
            $0.id.lowercased().contains(query) ||
            $0.description.lowercased().contains(query)
        }
    }

    // MARK: - Device Functions

    private func connectToDevice(_ device: SolunaLocalDevice) {
        connectedDeviceHost = device.host
        if !receiver.isPlaying { receiver.start() }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
            receiver.connectRelay(group: channel, host: device.host, port: UInt16(device.port))
        }
    }

    private func disconnectDevice() {
        connectedDeviceHost = nil
        receiver.connectRelay(group: channel)
    }

    private func connectToGlobalDevice(_ device: GlobalDevice) {
        connectedDeviceHost = device.relayHost
        if !receiver.isPlaying { receiver.start() }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
            receiver.connectRelay(group: device.group, host: device.relayHost, port: device.relayPort)
        }
    }

    private func addRecentChannel(_ ch: String) {
        var list = recentChannels.filter { $0 != ch }
        list.insert(ch, at: 0)
        if list.count > 5 { list = Array(list.prefix(5)) }
        recentChannelsData = list.joined(separator: ",")
    }

    private func switchToChannel(_ ch: String) {
        let trimmed = ch.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }
        channel = trimmed
        groupCode = trimmed
        addRecentChannel(trimmed)
        if receiver.state != .receiving { receiver.start() }
        receiver.connectRelay(group: trimmed)
    }

    private func togglePlayback() {
        if receiver.state == .error { receiver.start() }
        else { receiver.toggle() }
    }

    private func loadSavedSettings() {
        let d = UserDefaults.standard
        if let g = d.string(forKey: "multicastGroup"), !g.isEmpty { receiver.multicastGroup = g }
        let port = d.integer(forKey: "port")
        if port > 0 { receiver.port = UInt16(port) }
        let ch = d.integer(forKey: "channels")
        if ch >= 1 { receiver.channels = UInt32(ch) }
    }

    // MARK: - Body

    var body: some View {
        NavigationSplitView {
            sidebarView
        } detail: {
            detailView
        }
        .preferredColorScheme(.dark)
        .sheet(isPresented: $showDevicePicker) {
            GlobalDevicePickerView(registry: globalRegistry) { device in
                connectToGlobalDevice(device)
            }
        }
        .sheet(isPresented: $showSettings) { SettingsView(receiver: receiver) }
        .sheet(isPresented: $showFestivalMode) {
            FestivalModeView(receiver: receiver)
                .frame(minWidth: 600, minHeight: 400)
        }
        .sheet(isPresented: $showAddSpeaker, onDismiss: { newName = ""; newHost = "" }) {
            addSpeakerSheet
        }
        .sheet(isPresented: $showSavePreset, onDismiss: { presetName = "" }) {
            savePresetSheet
        }
        .sheet(isPresented: $showCreateGroup) {
            createGroupSheet
        }
        .sheet(isPresented: $showChannelSettings) {
            ChannelSettingsView(channel: channelToEdit)
        }
        .task {
            speakers.audioReceiver = receiver
            playerModel.speakersController = speakers
            playerModel.daemon = speakers.primaryDaemon
            loadSavedSettings()
            if autoConnect {
                receiver.start()
                // Ensure SDK receiver starts even if AudioReceiver state is stale
                let sdk = SDKAudioReceiver.shared
                if sdk.state == .stopped || sdk.state == .error {
                    sdk.channel = channel
                    sdk.relayHost = "relay.solun.art"
                    sdk.start()
                }
            }
            Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { _ in
                Task { @MainActor in
                    speakers.applyServerRxDelay()
                    if !(playerModel.daemon?.isConnected ?? false) {
                        playerModel.rebindIfNeeded(speakers.primaryDaemon)
                    }
                    if !receiver.activeOutputs.isEmpty {
                        speakers.recalculateAllDelays()
                        receiver.sampleLatencies()
                    }
                }
            }
        }
        .onChange(of: speakers.speakers.count) { _ in
            playerModel.rebindIfNeeded(speakers.primaryDaemon)
        }
        .animation(.spring(response: 0.35, dampingFraction: 0.8), value: receiver.state.rawValue)
    }

    // MARK: - Sidebar

    private var sidebarView: some View {
        List {
            Section("Channels") {
                ForEach(allChannels) { ch in
                    Button { switchToChannel(ch.id) } label: {
                        HStack(spacing: 10) {
                            Image(systemName: ch.icon)
                                .font(.system(size: 14))
                                .foregroundColor(ch.color)
                                .frame(width: 24)
                            Text(ch.label)
                                .foregroundColor(channel == ch.id ? .white : .white.opacity(0.8))
                            Spacer()
                            if channel == ch.id && isPlaying {
                                Image(systemName: "speaker.wave.2.fill")
                                    .font(.system(size: 10))
                                    .foregroundColor(.solunaSol)
                            }
                        }
                    }
                    .buttonStyle(.plain)
                    .contextMenu {
                        Button {
                            channelToEdit = ch.id
                            showChannelSettings = true
                        } label: {
                            Label("Channel Settings", systemImage: "gearshape")
                        }
                    }
                    .listRowBackground(
                        channel == ch.id
                            ? RoundedRectangle(cornerRadius: 6).fill(Color.white.opacity(0.1))
                            : nil
                    )
                }
            }

            // Custom channel quick-join
            Section("Custom") {
                HStack(spacing: 8) {
                    if isEditingChannel {
                        TextField("Channel name", text: $editedChannel, onCommit: {
                            switchToChannel(editedChannel)
                            isEditingChannel = false
                        })
                        .textFieldStyle(.roundedBorder)
                        .font(.system(size: 12, design: .monospaced))

                        Button {
                            switchToChannel(editedChannel)
                            isEditingChannel = false
                        } label: {
                            Image(systemName: "checkmark.circle.fill")
                                .foregroundColor(.solunaLive)
                        }
                        .buttonStyle(.plain)

                        Button { isEditingChannel = false } label: {
                            Image(systemName: "xmark.circle.fill")
                                .foregroundColor(.secondary)
                        }
                        .buttonStyle(.plain)
                    } else {
                        Button {
                            editedChannel = ""
                            isEditingChannel = true
                        } label: {
                            Label("Join Custom", systemImage: "plus")
                                .font(.system(size: 13))
                        }
                        .buttonStyle(.plain)
                    }
                }
            }

            if !recentChannels.isEmpty {
                let customRecent = recentChannels.filter { name in !allChannels.contains { $0.id == name } }
                if !customRecent.isEmpty {
                    Section("Recent") {
                        ForEach(customRecent, id: \.self) { name in
                            Button { switchToChannel(name) } label: {
                                HStack(spacing: 10) {
                                    Image(systemName: "clock")
                                        .font(.system(size: 14))
                                        .foregroundColor(.white.opacity(0.5))
                                        .frame(width: 24)
                                    Text(name.capitalized)
                                        .foregroundColor(.white.opacity(0.8))
                                }
                            }
                            .buttonStyle(.plain)
                        }
                    }
                }
            }

            Section {
                Button { showSettings = true } label: {
                    Label("Settings", systemImage: "gearshape")
                }
                .buttonStyle(.plain)
                Button { showFestivalMode = true } label: {
                    Label("Festival Mode", systemImage: "sparkles")
                }
                .buttonStyle(.plain)
            }
        }
        .listStyle(.sidebar)
        .navigationTitle("SOLUNA")
        .frame(minWidth: 180)
    }

    // MARK: - Detail View

    private var detailView: some View {
        ZStack {
            LinearGradient.solunaBg.ignoresSafeArea()

            ScrollView(showsIndicators: false) {
                VStack(spacing: 24) {
                    // Now Playing Hero
                    nowPlayingHero
                        .padding(.horizontal, 32)
                        .padding(.top, 20)

                    // Latency warning
                    if SDKAudioReceiver.shared.latencyExceeded {
                        HStack(spacing: 8) {
                            Image(systemName: "exclamationmark.triangle.fill")
                                .foregroundColor(.yellow)
                                .font(.system(size: 13))
                            Text("Bluetooth latency too high for sync — use wired output or increase channel latency target")
                                .font(.system(size: 11))
                                .foregroundColor(.white.opacity(0.8))
                            Spacer()
                        }
                        .padding(10)
                        .background(Color.orange.opacity(0.15))
                        .clipShape(RoundedRectangle(cornerRadius: 8))
                        .padding(.horizontal, 32)
                    }

                    // Broadcast section (when in broadcast tab)
                    if topTab == .broadcast {
                        broadcastSection
                            .padding(.horizontal, 32)
                    }

                    // Bottom controls (volume, record, stats)
                    bottomControls
                        .padding(.horizontal, 32)

                    // Speakers section (collapsed by default)
                    if !speakers.speakers.isEmpty || !receiver.availableDevices.filter(\.isActive).isEmpty {
                        speakersSection
                            .padding(.horizontal, 32)
                    }

                    // More section (send, player, talk mode)
                    moreSectionCompact
                        .padding(.horizontal, 32)
                        .padding(.bottom, 32)
                }
            }

            // Debug overlay
            if showDebug {
                VStack {
                    HStack {
                        Spacer()
                        debugOverlayInline
                            .frame(maxWidth: 320)
                            .padding(12)
                    }
                    Spacer()
                }
            }
        }
        .toolbar {
            ToolbarItemGroup(placement: .automatic) {
                Picker("", selection: $topTab) {
                    ForEach(TopTab.allCases, id: \.self) { tab in
                        Text(tab.rawValue).tag(tab)
                    }
                }
                .pickerStyle(.segmented)
                .frame(width: 200)

                Spacer()

                Button { showDevicePicker = true } label: {
                    Image(systemName: "globe")
                }
                .help("Devices")

                Button { showDebug.toggle() } label: {
                    Image(systemName: "ant")
                }
                .help("Debug")
            }
        }
    }

    // MARK: - Now Playing Hero

    private var nowPlayingHero: some View {
        let ch = allChannels.first { $0.id == channel }

        return VStack(spacing: 24) {
            // Channel artwork
            ZStack {
                Circle()
                    .fill(
                        RadialGradient(
                            colors: [
                                (ch?.color ?? .solunaLuna).opacity(0.5),
                                (ch?.color ?? .solunaLuna).opacity(0.05)
                            ],
                            center: .center,
                            startRadius: 30,
                            endRadius: 140
                        )
                    )
                    .frame(width: 240, height: 240)

                if isPlaying {
                    WaveformVisualizer(level: Float(receiver.isMicTransmitting ? receiver.micInputLevel : 0.5))
                        .frame(width: 160, height: 80)
                        .opacity(0.6)
                } else {
                    Image(systemName: ch?.icon ?? "music.note")
                        .font(.system(size: 72, weight: .ultraLight))
                        .foregroundStyle(
                            LinearGradient(
                                colors: [.white, .white.opacity(0.6)],
                                startPoint: .top,
                                endPoint: .bottom
                            )
                        )
                }
            }
            .shadow(color: (ch?.color ?? .solunaLuna).opacity(0.3), radius: 40)

            // Channel info
            VStack(spacing: 6) {
                Text(ch?.label ?? channel.capitalized)
                    .font(.system(size: 28, weight: .bold))
                    .foregroundColor(.white)
                Text(isPlaying ? "Now streaming" : (ch?.description ?? "Custom channel"))
                    .font(.system(size: 14))
                    .foregroundColor(.white.opacity(0.5))
            }

            // Spectrum (only when receiving)
            if isPlaying {
                SpectrumView(receiver: receiver)
                    .frame(height: 40)
                    .padding(.horizontal, 20)
                    .transition(.move(edge: .top).combined(with: .opacity))
            }

            // Transport controls
            HStack(spacing: 36) {
                Button {
                    let ids = allChannels.map(\.id)
                    if let idx = ids.firstIndex(of: channel) {
                        switchToChannel(ids[(idx - 1 + ids.count) % ids.count])
                    }
                } label: {
                    Image(systemName: "backward.fill")
                        .font(.system(size: 20))
                        .foregroundColor(.white.opacity(0.7))
                }
                .buttonStyle(.plain)

                Button(action: togglePlayback) {
                    ZStack {
                        if receiver.state == .connecting {
                            ProgressView()
                                .tint(.white)
                                .scaleEffect(1.5)
                                .frame(width: 56, height: 56)
                        } else {
                            Circle()
                                .fill(.white)
                                .frame(width: 56, height: 56)
                            Image(systemName: isPlaying ? "pause.fill" : "play.fill")
                                .font(.system(size: 22))
                                .foregroundColor(.black)
                                .offset(x: isPlaying ? 0 : 2)
                        }
                    }
                }
                .buttonStyle(.plain)
                .keyboardShortcut(.space, modifiers: [])

                Button {
                    let ids = allChannels.map(\.id)
                    if let idx = ids.firstIndex(of: channel) {
                        switchToChannel(ids[(idx + 1) % ids.count])
                    }
                } label: {
                    Image(systemName: "forward.fill")
                        .font(.system(size: 20))
                        .foregroundColor(.white.opacity(0.7))
                }
                .buttonStyle(.plain)
            }

            // Volume
            HStack(spacing: 12) {
                Image(systemName: masterMuted ? "speaker.slash.fill" : "speaker.fill")
                    .font(.system(size: 11))
                    .foregroundColor(.white.opacity(0.4))
                    .onTapGesture { masterMuted.toggle(); speakers.setAllMute(masterMuted) }
                Slider(value: Binding(
                    get: { masterVolume },
                    set: { v in
                        masterVolume = v
                        speakers.setAllVolume(v)
                    }
                ), in: 0...1)
                .tint(.white.opacity(0.5))
                .frame(maxWidth: 280)
                Image(systemName: "speaker.wave.3.fill")
                    .font(.system(size: 11))
                    .foregroundColor(.white.opacity(0.4))
            }

            // Connection status
            HStack(spacing: 8) {
                Circle()
                    .fill(isPlaying ? .green : .white.opacity(0.3))
                    .frame(width: 6, height: 6)
                Text(isPlaying ? "Connected to relay.solun.art" : "Not playing")
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundColor(.white.opacity(0.4))
                if isPlaying {
                    Text("--")
                        .foregroundColor(.white.opacity(0.2))
                    Text("\(receiver.bufferMs)ms buf")
                        .font(.system(size: 11, weight: .medium, design: .monospaced))
                        .foregroundColor(.white.opacity(0.4))
                }
            }

            // Error detail
            if receiver.state == .error {
                VStack(spacing: 4) {
                    if let msg = receiver.errorMessage, !msg.isEmpty {
                        Text(msg)
                            .font(.system(size: 12))
                            .foregroundColor(.red.opacity(0.9))
                            .multilineTextAlignment(.center)
                    }
                    Text("Click play to retry")
                        .font(.system(size: 11))
                        .foregroundColor(.secondary)
                }
                .padding(.horizontal, 16)
                .transition(.opacity)
            }
        }
        .padding(.vertical, 32)
    }

    // MARK: - Speakers Section (DisclosureGroup)

    private var speakersSection: some View {
        DisclosureGroup {
            VStack(spacing: 0) {
                speakersCard
            }
        } label: {
            HStack(spacing: 8) {
                Image(systemName: "hifispeaker.2.fill")
                    .foregroundColor(.solunaLuna)
                Text("Speakers")
                    .font(.headline)
                    .foregroundColor(.white)
                Spacer()
                Text("\(speakers.speakers.count + receiver.availableDevices.filter(\.isActive).count)")
                    .font(.caption.weight(.bold))
                    .foregroundColor(.white.opacity(0.5))
                    .padding(.horizontal, 8)
                    .padding(.vertical, 3)
                    .background(Color.white.opacity(0.1))
                    .clipShape(Capsule())
            }
        }
        .tint(.white.opacity(0.6))
        .padding(16)
        .glassCard()
    }

    // MARK: - More Section (compact for split view)

    private var moreSectionCompact: some View {
        VStack(spacing: 12) {
            // Device browser
            DisclosureGroup {
                DeviceBrowserView(
                    browser: deviceBrowser,
                    connectedDeviceHost: connectedDeviceHost,
                    onSelect: { connectToDevice($0) },
                    onDisconnect: { disconnectDevice() }
                )
                .padding(.top, 8)
            } label: {
                Label("Nearby Devices", systemImage: "antenna.radiowaves.left.and.right")
                    .font(.headline)
                    .foregroundColor(.white)
            }
            .tint(.white.opacity(0.6))
            .padding(12)
            .glassCard()

            // Send section
            sendSection

            // Player section
            playerSectionCollapsible

            // Device connection banner
            if let host = connectedDeviceHost {
                let devName = deviceBrowser.devices.first(where: { $0.host == host })?.name ?? host
                HStack(spacing: 10) {
                    Image(systemName: "laptopcomputer.and.iphone")
                        .foregroundColor(.solunaLive)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(devName).font(.system(size: 14, weight: .bold)).foregroundColor(.white)
                        Text("Direct connection").font(.caption2).foregroundColor(.white.opacity(0.4))
                    }
                    Spacer()
                    Button { disconnectDevice() } label: {
                        Text("Disconnect")
                            .font(.caption.bold())
                            .foregroundColor(.solunaMic)
                            .padding(.horizontal, 10)
                            .padding(.vertical, 5)
                            .background(Color.solunaMic.opacity(0.15))
                            .clipShape(Capsule())
                    }
                    .buttonStyle(.plain)
                }
                .padding(12)
                .glassCard()
            }

            // Relay banner
            relayBanner

            // Remote Volume Control (group members)
            if isPlaying && !receiver.groupMembers.isEmpty {
                RemoteVolumeSection(receiver: receiver)
            }

            // Talk mode
            if isPlaying {
                Button(action: {
                    talkMode.toggle()
                    receiver.setTalkMode(talkMode)
                }) {
                    HStack(spacing: 6) {
                        Image(systemName: talkMode ? "person.3.fill" : "person.3")
                            .font(.system(size: 13))
                        Text("Talk Mode")
                            .font(.system(size: 12, weight: .medium))
                        if talkMode {
                            Text("ON").font(.caption2.weight(.bold))
                                .padding(.horizontal, 6).padding(.vertical, 2)
                                .background(Color.solunaGradientStart.opacity(0.2))
                                .foregroundColor(.solunaGradientStart)
                                .clipShape(Capsule())
                        }
                    }
                    .foregroundColor(talkMode ? .solunaGradientStart : .white.opacity(0.5))
                    .frame(maxWidth: .infinity)
                    .padding(.horizontal, 12).padding(.vertical, 10)
                    .background(talkMode ? Color.solunaGradientStart.opacity(0.1) : Color.clear)
                    .glassCard(cornerRadius: 12)
                }
                .buttonStyle(.plain)
            }
        }
    }

    // MARK: - Header Bar

    private var headerBar: some View {
        HStack(spacing: 12) {
            Text("SOLUNA")
                .font(.system(size: 24, weight: .bold, design: .rounded))
                .foregroundStyle(LinearGradient.solLunaGradient)
                .onTapGesture(count: 3) { showDebug.toggle() }

            Spacer()

            headerButton(icon: "globe", color: .solunaLuna) { showDevicePicker = true }
            headerButton(icon: "sparkles", color: .solunaSol) { showFestivalMode = true }
            headerButton(icon: "ant", color: .red.opacity(0.6)) { showDebug.toggle() }
            headerButton(icon: "gearshape", color: .white.opacity(0.6)) { showSettings = true }
        }
    }

    private func headerButton(icon: String, color: Color, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: icon)
                .font(.system(size: 14, weight: .medium))
                .foregroundColor(color)
                .frame(width: 32, height: 32)
                .background(Color.white.opacity(0.08))
                .clipShape(Circle())
        }
        .buttonStyle(.plain)
    }

    // MARK: - Top Tab Picker

    private var topTabPicker: some View {
        Picker("", selection: $topTab) {
            ForEach(TopTab.allCases, id: \.self) { tab in
                HStack(spacing: 4) {
                    Image(systemName: tab == .listen ? "headphones" : "mic.fill")
                    Text(tab.rawValue)
                }
                .tag(tab)
            }
        }
        .pickerStyle(.segmented)
    }

    // MARK: - Broadcast Section

    private var broadcastSection: some View {
        VStack(spacing: 20) {
            // Broadcast header card
            VStack(spacing: 16) {
                Image(systemName: "antenna.radiowaves.left.and.right")
                    .font(.system(size: 36))
                    .foregroundStyle(
                        receiver.isMicTransmitting || receiver.isShmTransmitting
                            ? LinearGradient.solGradient
                            : LinearGradient(colors: [.white.opacity(0.3), .white.opacity(0.15)], startPoint: .top, endPoint: .bottom)
                    )
                    .shadow(color: receiver.isMicTransmitting ? .solunaSol.opacity(0.5) : .clear, radius: 10)

                Text(receiver.isMicTransmitting || receiver.isShmTransmitting ? "Broadcasting" : "Ready to Broadcast")
                    .font(.system(size: 22, weight: .bold))
                    .foregroundColor(.white)

                // Channel name field
                VStack(alignment: .leading, spacing: 6) {
                    Text("Channel")
                        .font(.system(size: 12, weight: .medium))
                        .foregroundColor(.white.opacity(0.5))
                    HStack(spacing: 8) {
                        Image(systemName: "number")
                            .font(.system(size: 13))
                            .foregroundColor(.white.opacity(0.3))
                        TextField("Channel name", text: $broadcastChannel)
                            .textFieldStyle(.plain)
                            .font(.system(size: 15, weight: .medium))
                            .foregroundColor(.white)
                            .onAppear { if broadcastChannel.isEmpty { broadcastChannel = channel } }
                        if !broadcastChannel.isEmpty {
                            Button {
                                broadcastChannel = ""
                            } label: {
                                Image(systemName: "xmark.circle.fill")
                                    .font(.system(size: 12))
                                    .foregroundColor(.white.opacity(0.3))
                            }
                            .buttonStyle(.plain)
                        }
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 10)
                    .background(Color.white.opacity(0.08))
                    .clipShape(RoundedRectangle(cornerRadius: 10))
                }

                // Status line
                if receiver.isMicTransmitting || receiver.isShmTransmitting {
                    HStack(spacing: 8) {
                        Circle()
                            .fill(Color.solunaMic)
                            .frame(width: 8, height: 8)
                            .shadow(color: .solunaMic.opacity(0.6), radius: 4)
                        Text("Broadcasting to:")
                            .font(.system(size: 13, weight: .medium))
                            .foregroundColor(.white.opacity(0.6))
                        Text(broadcastChannel.isEmpty ? channel : broadcastChannel)
                            .font(.system(size: 13, weight: .bold, design: .monospaced))
                            .foregroundColor(.solunaSol)
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 8)
                    .background(Color.solunaMic.opacity(0.1))
                    .clipShape(Capsule())
                }
            }
            .padding(20)
            .glassCard()

            // Big Mic Button
            VStack(spacing: 16) {
                Button {
                    // Ensure we are connected to the right channel before toggling mic
                    let ch = broadcastChannel.isEmpty ? channel : broadcastChannel
                    if !receiver.isPlaying { receiver.start() }
                    if ch != channel {
                        switchToChannel(ch)
                    }
                    receiver.toggleMic()
                } label: {
                    ZStack {
                        Circle()
                            .fill(
                                receiver.isMicTransmitting
                                    ? LinearGradient(colors: [.solunaMic, .solunaMic.opacity(0.7)], startPoint: .top, endPoint: .bottom)
                                    : LinearGradient(colors: [Color.white.opacity(0.12), Color.white.opacity(0.06)], startPoint: .top, endPoint: .bottom)
                            )
                            .frame(width: 100, height: 100)
                            .shadow(color: receiver.isMicTransmitting ? .solunaMic.opacity(0.4) : .clear, radius: 16)
                            .overlay(
                                Circle()
                                    .strokeBorder(
                                        receiver.isMicTransmitting ? Color.white.opacity(0.3) : Color.white.opacity(0.1),
                                        lineWidth: 2
                                    )
                            )

                        Image(systemName: receiver.isMicTransmitting ? "mic.fill" : "mic.slash.fill")
                            .font(.system(size: 40, weight: .medium))
                            .foregroundColor(receiver.isMicTransmitting ? .white : .white.opacity(0.5))
                    }
                }
                .buttonStyle(.plain)

                Text(receiver.isMicTransmitting ? "Tap to stop" : "Tap to broadcast")
                    .font(.system(size: 13, weight: .medium))
                    .foregroundColor(.white.opacity(0.4))

                // Mic level meter
                if receiver.isMicTransmitting {
                    VStack(spacing: 6) {
                        Text("Mic Level")
                            .font(.system(size: 11, weight: .medium))
                            .foregroundColor(.white.opacity(0.4))
                        MicLevelMeter(level: receiver.micInputLevel)
                            .frame(height: 8)
                            .clipShape(Capsule())
                        HStack {
                            Text("TX: \(formatNum(receiver.txPacketsSent)) pkts")
                                .font(.system(size: 10, weight: .medium, design: .monospaced))
                                .foregroundColor(.white.opacity(0.3))
                            Spacer()
                        }
                    }
                    .padding(.horizontal, 20)
                    .transition(.opacity.combined(with: .move(edge: .top)))
                }
            }
            .padding(20)
            .glassCard()

            // System Audio Transmit
            VStack(spacing: 12) {
                HStack(spacing: 12) {
                    Button {
                        let ch = broadcastChannel.isEmpty ? channel : broadcastChannel
                        if !receiver.isPlaying { receiver.start() }
                        if ch != channel {
                            switchToChannel(ch)
                        }
                        receiver.toggleShmTransmit()
                    } label: {
                        HStack(spacing: 8) {
                            Image(systemName: receiver.isShmTransmitting ? "speaker.wave.2.fill" : "speaker.slash.fill")
                                .font(.system(size: 16))
                            Text(receiver.isShmTransmitting ? "System Audio ON" : "System Audio")
                                .font(.system(size: 14, weight: .semibold))
                        }
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 14)
                        .foregroundColor(receiver.isShmTransmitting ? .white : .white.opacity(0.6))
                        .background(
                            receiver.isShmTransmitting
                                ? LinearGradient(colors: [.orange, .orange.opacity(0.7)], startPoint: .leading, endPoint: .trailing)
                                : LinearGradient(colors: [Color.white.opacity(0.08), Color.white.opacity(0.04)], startPoint: .leading, endPoint: .trailing)
                        )
                        .clipShape(RoundedRectangle(cornerRadius: 12))
                        .overlay(
                            RoundedRectangle(cornerRadius: 12)
                                .strokeBorder(
                                    receiver.isShmTransmitting ? Color.white.opacity(0.2) : Color.white.opacity(0.06),
                                    lineWidth: 0.5
                                )
                        )
                    }
                    .buttonStyle(.plain)
                }

                if receiver.isShmTransmitting {
                    VStack(spacing: 4) {
                        MicLevelMeter(level: receiver.shmTxLevel)
                            .frame(height: 6)
                            .clipShape(Capsule())
                        HStack {
                            Text("TX: \(formatNum(receiver.shmTxPacketsSent)) pkts")
                                .font(.system(size: 10, weight: .medium, design: .monospaced))
                                .foregroundColor(.white.opacity(0.3))
                            Spacer()
                        }
                    }
                    .transition(.opacity)
                }

                // Sync mode picker
                if receiver.isMicTransmitting || receiver.isShmTransmitting {
                    HStack(spacing: 12) {
                        Text("Mode")
                            .font(.system(size: 12, weight: .medium))
                            .foregroundColor(.white.opacity(0.4))
                        Picker("", selection: $streamMode) {
                            Text("Sync").tag("sync")
                            Text("Jam (Low Latency)").tag("jam")
                        }
                        .pickerStyle(.segmented)
                        .frame(maxWidth: 200)
                    }
                }
            }
            .padding(16)
            .glassCard()
        }
    }

    // MARK: - 1. Channel Grid

    private var channelGrid: some View {
        VStack(alignment: .leading, spacing: 12) {
            // Title row
            HStack {
                Text("Channels")
                    .font(.system(size: 18, weight: .bold))
                    .foregroundColor(.white)
                Spacer()
                Button { showBrowseAll.toggle() } label: {
                    Text(showBrowseAll ? "Show Less" : "Browse All")
                        .font(.system(size: 12, weight: .medium))
                        .foregroundColor(.solunaLuna)
                        .padding(.horizontal, 10)
                        .padding(.vertical, 5)
                        .background(Color.solunaLuna.opacity(0.15))
                        .clipShape(Capsule())
                }
                .buttonStyle(.plain)
            }

            // Search / filter
            HStack(spacing: 8) {
                Image(systemName: "magnifyingglass")
                    .font(.system(size: 12))
                    .foregroundColor(.white.opacity(0.4))
                TextField("Search channels...", text: $channelSearch)
                    .textFieldStyle(.plain)
                    .font(.system(size: 13))
                    .foregroundColor(.white)
                if !channelSearch.isEmpty {
                    Button { channelSearch = "" } label: {
                        Image(systemName: "xmark.circle.fill")
                            .font(.system(size: 12))
                            .foregroundColor(.white.opacity(0.4))
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            .background(Color.white.opacity(0.06))
            .clipShape(RoundedRectangle(cornerRadius: 10))

            // Custom channel quick-join
            HStack(spacing: 8) {
                if isEditingChannel {
                    TextField("Channel name", text: $editedChannel, onCommit: {
                        switchToChannel(editedChannel)
                        isEditingChannel = false
                    })
                    .textFieldStyle(.roundedBorder)
                    .font(.system(size: 12, design: .monospaced))
                    .frame(maxWidth: 160)

                    Button {
                        switchToChannel(editedChannel)
                        isEditingChannel = false
                    } label: {
                        Image(systemName: "checkmark.circle.fill")
                            .foregroundColor(.solunaLive)
                    }
                    .buttonStyle(.plain)

                    Button { isEditingChannel = false } label: {
                        Image(systemName: "xmark.circle.fill")
                            .foregroundColor(.secondary)
                    }
                    .buttonStyle(.plain)
                } else {
                    Button {
                        editedChannel = ""
                        isEditingChannel = true
                    } label: {
                        HStack(spacing: 4) {
                            Image(systemName: "plus")
                                .font(.system(size: 11, weight: .bold))
                            Text("Custom")
                                .font(.system(size: 12, weight: .medium))
                        }
                        .foregroundColor(.solunaLuna)
                        .padding(.horizontal, 10)
                        .padding(.vertical, 5)
                        .background(Color.solunaLuna.opacity(0.15))
                        .clipShape(Capsule())
                    }
                    .buttonStyle(.plain)
                }

                Spacer()
            }

            // Grid
            LazyVGrid(columns: [
                GridItem(.flexible(), spacing: 12),
                GridItem(.flexible(), spacing: 12),
                GridItem(.flexible(), spacing: 12)
            ], spacing: 12) {
                ForEach(filteredChannels) { ch in
                    Button { switchToChannel(ch.id) } label: {
                        VStack(alignment: .leading, spacing: 6) {
                            HStack {
                                Image(systemName: ch.icon)
                                    .font(.title2)
                                    .foregroundColor(channel == ch.id ? .white : ch.color)
                                Spacer()
                                if channel == ch.id && isPlaying {
                                    Image(systemName: "antenna.radiowaves.left.and.right")
                                        .font(.caption)
                                        .foregroundColor(.white.opacity(0.8))
                                }
                            }
                            Text(ch.label)
                                .font(.system(size: 15, weight: .bold))
                                .foregroundColor(channel == ch.id ? .white : .white.opacity(0.9))
                            Text(ch.description)
                                .font(.caption2)
                                .foregroundColor(channel == ch.id ? .white.opacity(0.7) : .white.opacity(0.4))
                                .lineLimit(1)
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(14)
                        .background(
                            Group {
                                if channel == ch.id {
                                    RoundedRectangle(cornerRadius: 16)
                                        .fill(ch.color.gradient)
                                } else {
                                    RoundedRectangle(cornerRadius: 16)
                                        .fill(Color.white.opacity(0.06))
                                }
                            }
                        )
                        .overlay(
                            RoundedRectangle(cornerRadius: 16)
                                .strokeBorder(
                                    channel == ch.id ? Color.white.opacity(0.2) : Color.white.opacity(0.06),
                                    lineWidth: 0.5
                                )
                        )
                    }
                    .buttonStyle(.plain)
                }
            }

            // Recent custom channels
            let customRecent = recentChannels.filter { ch in !allChannels.contains(where: { $0.id == ch }) }
            if !customRecent.isEmpty {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 8) {
                        ForEach(customRecent, id: \.self) { ch in
                            Button { switchToChannel(ch) } label: {
                                Text(ch)
                                    .font(.system(size: 12, weight: ch == channel ? .bold : .medium, design: .monospaced))
                                    .foregroundColor(ch == channel ? .white : .white.opacity(0.5))
                                    .padding(.horizontal, 12)
                                    .padding(.vertical, 6)
                                    .background(ch == channel ? Color.solunaGradientMid : Color.white.opacity(0.06))
                                    .clipShape(Capsule())
                            }
                            .buttonStyle(.plain)
                        }
                    }
                }
            }
        }
    }

    // MARK: - 2. Now Playing

    private var nowPlayingArea: some View {
        VStack(spacing: 16) {
            // Visualizer / album art
            ZStack {
                Group {
                    if isPlaying {
                        RoundedRectangle(cornerRadius: 20)
                            .fill(LinearGradient.solLunaGradient)
                            .frame(height: 180)
                    } else {
                        RoundedRectangle(cornerRadius: 20)
                            .fill(Color.white.opacity(0.04))
                            .frame(height: 180)
                    }
                }

                if isPlaying {
                    WaveformVisualizer(level: Float(receiver.isMicTransmitting ? receiver.micInputLevel : 0.5))
                        .frame(height: 100)
                        .padding(.horizontal, 20)
                        .opacity(0.8)
                } else {
                    VStack(spacing: 8) {
                        Image(systemName: "waveform")
                            .font(.system(size: 40))
                            .foregroundColor(.white.opacity(0.15))
                        Text("Tap a channel to start")
                            .font(.subheadline)
                            .foregroundColor(.white.opacity(0.25))
                    }
                }
            }
            .clipShape(RoundedRectangle(cornerRadius: 20))

            // Song info
            VStack(spacing: 4) {
                Text(channel.capitalized)
                    .font(.system(size: 22, weight: .bold))
                    .foregroundColor(.white)
                    .lineLimit(1)

                Text(isPlaying ? "Now streaming" : "Ready to play")
                    .font(.system(size: 15))
                    .foregroundColor(.white.opacity(0.5))
                    .lineLimit(1)
            }

            // Play / Pause button row
            HStack(spacing: 32) {
                Button {} label: {
                    Image(systemName: "backward.fill")
                        .font(.system(size: 20))
                        .foregroundColor(.white.opacity(0.4))
                }
                .buttonStyle(.plain)

                // Main play/pause — large and prominent
                Button(action: togglePlayback) {
                    ZStack {
                        if receiver.state == .connecting {
                            ProgressView()
                                .tint(.white)
                                .scaleEffect(1.5)
                                .frame(width: 72, height: 72)
                        } else {
                            Image(systemName: isPlaying ? "pause.circle.fill" : "play.circle.fill")
                                .font(.system(size: 72))
                                .foregroundStyle(isPlaying ? LinearGradient.solGradient : LinearGradient.lunaGradient)
                                .shadow(color: isPlaying ? .solunaSol.opacity(0.4) : .solunaLuna.opacity(0.3), radius: 12)
                        }
                    }
                }
                .buttonStyle(.plain)

                Button {} label: {
                    Image(systemName: "forward.fill")
                        .font(.system(size: 20))
                        .foregroundColor(.white.opacity(0.4))
                }
                .buttonStyle(.plain)
            }

            // Status line
            if isPlaying {
                HStack(spacing: 8) {
                    Circle()
                        .fill(Color.solunaLive)
                        .frame(width: 6, height: 6)
                        .shadow(color: .solunaLive.opacity(0.6), radius: 4)
                    Text("Listening")
                        .font(.system(size: 13, weight: .medium))
                        .foregroundColor(.solunaLive)
                    Text("--")
                        .foregroundColor(.white.opacity(0.3))
                    Text("\(receiver.bufferMs)ms buf")
                        .font(.system(size: 12, weight: .medium, design: .monospaced))
                        .foregroundColor(.white.opacity(0.4))
                }
            }

            // Error detail
            if receiver.state == .error {
                VStack(spacing: 4) {
                    if let msg = receiver.errorMessage, !msg.isEmpty {
                        Text(msg)
                            .font(.system(size: 12))
                            .foregroundColor(.red.opacity(0.9))
                            .multilineTextAlignment(.center)
                    }
                    Text("Click play to retry")
                        .font(.system(size: 11))
                        .foregroundColor(.secondary)
                }
                .padding(.horizontal, 16)
                .transition(.opacity)
            }

            // Spectrum (only when receiving)
            if isPlaying {
                SpectrumView(receiver: receiver)
                    .transition(.move(edge: .top).combined(with: .opacity))
            }
        }
        .padding(16)
        .glassCard()
    }

    // MARK: - 3. Bottom Controls

    private var bottomControls: some View {
        HStack(spacing: 10) {
            // Record button
            Button {
                if receiver.isRecording { receiver.stopRecording() }
                else { receiver.startRecording() }
            } label: {
                HStack(spacing: 6) {
                    Image(systemName: receiver.isRecording ? "record.circle.fill" : "record.circle")
                        .font(.system(size: 14))
                        .foregroundColor(receiver.isRecording ? .red : .white.opacity(0.6))
                    Text(receiver.isRecording ? "Recording" : "Record")
                        .font(.system(size: 12, weight: .medium))
                        .foregroundColor(receiver.isRecording ? .red : .white.opacity(0.6))
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                .background((receiver.isRecording ? Color.red : Color.white).opacity(0.1))
                .clipShape(Capsule())
            }
            .buttonStyle(.plain)
            .help(receiver.isRecording ? "Stop Recording" : "Record to WAV")

            Spacer()

            // Stats pills (compact)
            if isPlaying {
                HStack(spacing: 6) {
                    miniStat(formatNum(receiver.packetsReceived), color: .green)
                    if receiver.packetsDropped > 0 {
                        miniStat(String(format: "%.1f%%", Double(receiver.packetsDropped) / max(1, Double(receiver.packetsReceived)) * 100), color: .orange)
                    }
                    if receiver.isSyncMode {
                        miniStat("\(receiver.syncDelayMs)ms", color: .blue)
                    }
                }
            }
        }
        .padding(.horizontal, 4)
    }

    private func miniStat(_ text: String, color: Color) -> some View {
        Text(text)
            .font(.system(size: 10, weight: .medium, design: .monospaced))
            .foregroundColor(color)
            .padding(.horizontal, 6)
            .padding(.vertical, 3)
            .background(color.opacity(0.1))
            .clipShape(Capsule())
    }

    // MARK: - More Section (Mac-specific: speakers, daemon, send, player)

    private var moreSection: some View {
        VStack(spacing: 12) {
            Button(action: { withAnimation(.spring(response: 0.3)) { showMoreSection.toggle() } }) {
                HStack {
                    Image(systemName: "ellipsis.circle.fill")
                        .foregroundColor(.solunaGradientMid)
                    Text("More")
                        .font(.headline)
                        .foregroundColor(showMoreSection ? .primary : .secondary)
                    Spacer()
                    Image(systemName: showMoreSection ? "chevron.up" : "chevron.down")
                        .font(.caption).foregroundColor(.secondary)
                }
                .padding(.horizontal, 16).padding(.vertical, 12)
            }
            .buttonStyle(.plain)
            .glassCard()

            if showMoreSection {
                // Device browser
                DeviceBrowserView(
                    browser: deviceBrowser,
                    connectedDeviceHost: connectedDeviceHost,
                    onSelect: { connectToDevice($0) },
                    onDisconnect: { disconnectDevice() }
                )
                .padding(12)
                .background(Color.white.opacity(0.04))
                .clipShape(RoundedRectangle(cornerRadius: 16))

                // Send section
                sendSection

                // Speakers card
                speakersCard

                // Player section
                playerSectionCollapsible

                // Talk mode
                if isPlaying {
                    Button(action: {
                        talkMode.toggle()
                        receiver.setTalkMode(talkMode)
                    }) {
                        HStack(spacing: 6) {
                            Image(systemName: talkMode ? "person.3.fill" : "person.3")
                                .font(.system(size: 13))
                            Text("Talk Mode")
                                .font(.system(size: 12, weight: .medium))
                            if talkMode {
                                Text("ON").font(.caption2.weight(.bold))
                                    .padding(.horizontal, 6).padding(.vertical, 2)
                                    .background(Color.solunaGradientStart.opacity(0.2))
                                    .foregroundColor(.solunaGradientStart)
                                    .clipShape(Capsule())
                            }
                        }
                        .foregroundColor(talkMode ? .solunaGradientStart : .white.opacity(0.5))
                        .frame(maxWidth: .infinity)
                        .padding(.horizontal, 12).padding(.vertical, 10)
                        .background(talkMode ? Color.solunaGradientStart.opacity(0.1) : Color.clear)
                        .glassCard(cornerRadius: 12)
                    }
                    .buttonStyle(.plain)
                }
            }
        }
    }

    // MARK: - Send Section (collapsible)

    private var anySending: Bool {
        receiver.isMicTransmitting || receiver.isShmTransmitting
    }

    private var sendSection: some View {
        VStack(spacing: 0) {
            Button(action: { withAnimation(.spring(response: 0.3)) { sendExpanded.toggle() } }) {
                HStack {
                    Image(systemName: "arrow.up.circle.fill")
                        .foregroundColor(anySending ? .orange : .secondary)
                    Text("Send")
                        .font(.headline)
                        .foregroundColor(anySending ? .primary : .secondary)
                    Spacer()
                    if anySending {
                        Text("ON").font(.caption.weight(.bold))
                            .padding(.horizontal, 8).padding(.vertical, 2)
                            .background(Color.orange.opacity(0.2))
                            .foregroundColor(.orange)
                            .clipShape(Capsule())
                    }
                    Image(systemName: sendExpanded ? "chevron.up" : "chevron.down")
                        .font(.caption).foregroundColor(.secondary)
                }
                .padding(.horizontal, 16).padding(.vertical, 12)
            }
            .buttonStyle(.plain)

            if sendExpanded {
                Divider().padding(.horizontal, 16)
                VStack(spacing: 12) {
                    HStack(spacing: 10) {
                        sendToggle(title: "Mic", icon: "mic.fill",
                                   active: receiver.isMicTransmitting, color: .red,
                                   action: { receiver.toggleMic() })
                        sendToggle(title: "System Audio", icon: "speaker.wave.2.fill",
                                   active: receiver.isShmTransmitting, color: .orange,
                                   action: { receiver.toggleShmTransmit() })
                    }
                    if receiver.isMicTransmitting {
                        MicLevelMeter(level: receiver.micInputLevel).frame(height: 6)
                    }
                    if receiver.isShmTransmitting {
                        MicLevelMeter(level: receiver.shmTxLevel).frame(height: 6).tint(.orange)
                    }
                    if anySending {
                        Picker("", selection: $streamMode) {
                            Text("Sync").tag("sync")
                            Text("Jam").tag("jam")
                        }
                        .pickerStyle(.segmented)
                        .frame(width: 140)
                    }
                }
                .padding(.horizontal, 16).padding(.vertical, 12)
            }
        }
        .glassCard()
        .cornerRadius(12)
    }

    private func sendToggle(title: String, icon: String, active: Bool, color: Color, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 6) {
                Image(systemName: active ? icon : icon.replacingOccurrences(of: ".fill", with: ".slash.fill"))
                    .font(.system(size: 14))
                Text(title).font(.system(size: 12, weight: .medium))
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 10)
            .foregroundColor(active ? .white : .secondary)
            .background(active ? color : Color(nsColor: .tertiaryLabelColor).opacity(0.15))
            .cornerRadius(8)
        }
        .buttonStyle(.plain)
    }

    // MARK: - Player Section (collapsible)

    private var playerSectionCollapsible: some View {
        VStack(spacing: 0) {
            Button(action: { withAnimation(.spring(response: 0.3)) { playerExpanded.toggle() } }) {
                HStack {
                    Label("Music Player", systemImage: "music.note")
                        .font(.headline)
                        .foregroundColor(playerExpanded ? .primary : .secondary)
                    Spacer()
                    Image(systemName: playerExpanded ? "chevron.up" : "chevron.down")
                        .font(.caption).foregroundColor(.secondary)
                }
                .padding(.horizontal, 16).padding(.vertical, 12)
            }
            .buttonStyle(.plain)

            if playerExpanded {
                Divider().padding(.horizontal, 16)
                PlayerView(model: playerModel)
                    .padding(.horizontal, 0)
            }
        }
        .glassCard()
        .cornerRadius(20)
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
                    .foregroundColor(.solunaLive)
                Text("\(relay.connectedPeerCount) devices relaying")
                    .font(.footnote.weight(.semibold))
                    .foregroundColor(.solunaLive)
                Spacer()
                Text("Relay")
                    .font(.caption2)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 3)
                    .background(Color.solunaLive.opacity(0.15))
                    .foregroundColor(.solunaLive)
                    .clipShape(Capsule())
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 10)
            .background(Color.solunaLive.opacity(0.08))
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

    // MARK: - Debug Overlay (inline, not sheet)

    private var debugOverlayInline: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text("Debug")
                    .font(.system(size: 13, weight: .bold))
                    .foregroundColor(.white)
                Spacer()
                Button { showDebug = false } label: {
                    Image(systemName: "xmark.circle.fill")
                        .font(.system(size: 14))
                        .foregroundColor(.white.opacity(0.5))
                }
                .buttonStyle(.plain)
            }
            Divider()
            Group {
                debugLine("State", receiver.state.rawValue)
                debugLine("Channel", channel)
                debugLine("Packets", formatNum(receiver.packetsReceived))
                debugLine("Dropped", formatNum(receiver.packetsDropped))
                debugLine("Buffer", "\(receiver.bufferMs)ms")
                if receiver.isSyncMode { debugLine("Sync", "\(receiver.syncDelayMs)ms") }
                if receiver.isMicTransmitting { debugLine("TX", formatNum(receiver.txPacketsSent)) }
                debugLine("Speakers", "\(speakers.speakers.count)")
            }
        }
        .padding(12)
        .background(Color.black.opacity(0.85))
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(Color.white.opacity(0.1)))
    }

    private func debugLine(_ label: String, _ value: String) -> some View {
        HStack {
            Text(label)
                .font(.system(size: 11, weight: .medium, design: .monospaced))
                .foregroundColor(.white.opacity(0.5))
            Spacer()
            Text(value)
                .font(.system(size: 11, weight: .semibold, design: .monospaced))
                .foregroundColor(.white.opacity(0.8))
        }
    }

    // MARK: - Speakers Card

    private var speakersCard: some View {
        VStack(spacing: 0) {
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

            if !receiver.presets.isEmpty || !receiver.activeOutputs.isEmpty {
                PresetRow(
                    presets: receiver.presets,
                    hasActiveDevices: !receiver.activeOutputs.isEmpty,
                    onApply: { preset in receiver.applyPreset(preset) },
                    onDelete: { id in receiver.deletePreset(id) },
                    onSave: { showSavePreset = true },
                    onExport: { exportPresets(receiver: receiver) },
                    onImport: { importPresets(receiver: receiver) }
                )
                Divider().padding(.horizontal, 16)
            }

            LocalSpeakerRow(receiver: receiver)

            ForEach(receiver.availableDevices.filter { $0.isActive }) { device in
                Divider().padding(.horizontal, 16)
                LocalDeviceRow(
                    device: device,
                    measuredLatencyMs: receiver.measuredLatencyMs(for: device.id),
                    receiver: receiver,
                    onToggle: {
                        receiver.disableDevice(device.id)
                        speakers.recalculateAllDelays()
                    },
                    onVolume: { v in receiver.setDeviceVolume(device.id, volume: v) },
                    onMute: { m in receiver.setDeviceMuted(device.id, muted: m) },
                    onOffset: { ms in receiver.setDeviceManualOffset(device.id, ms: ms) },
                    onBalance: { b in receiver.setDeviceBalance(device.id, balance: b) },
                    onExclusive: { e in receiver.setDeviceExclusive(device.id, exclusive: e) },
                    onEQ: { band, gain in receiver.setDeviceEQ(device.id, band: band, gain: gain) },
                    onCompressor: { t, r, a, rl, en in receiver.setDeviceCompressor(device.id, threshold: t, ratio: r, attack: a, release: rl, enabled: en) },
                    onCrossover: { mode, freq in receiver.setDeviceCrossover(device.id, mode: mode, frequency: freq) },
                    onSpatial: { en, w, cf in receiver.setDeviceSpatial(device.id, enabled: en, width: w, crossfeed: cf) }
                )
            }

            ForEach(speakers.speakers) { speaker in
                if let daemon = speakers.client(for: speaker.id) {
                    Divider().padding(.horizontal, 16)
                    RemoteSpeakerRow(
                        name: speaker.name,
                        speakerChannel: speaker.channel,
                        daemon: daemon,
                        onRemove: { speakers.remove(speaker.id) },
                        onChannelChange: { ch in speakers.setChannel(for: speaker.id, channel: ch) }
                    )
                }
            }

            if !receiver.speakerGroups.isEmpty {
                Divider().padding(.horizontal, 16)
                VStack(alignment: .leading, spacing: 6) {
                    Text("Groups")
                        .font(.caption.weight(.semibold))
                        .foregroundColor(.secondary)
                        .padding(.horizontal, 16)
                    ForEach(receiver.speakerGroups) { group in
                        SpeakerGroupRow(
                            group: group,
                            onVolume: { v in receiver.setGroupVolume(group.id, volume: v) },
                            onMute: { m in receiver.setGroupMuted(group.id, muted: m) },
                            onDelete: { receiver.deleteGroup(group.id) }
                        )
                    }
                }
                .padding(.vertical, 6)
            }

            Divider().padding(.horizontal, 16)

            HStack {
                Button(action: { showAddSpeaker = true }) {
                    Label("Add Speaker", systemImage: "plus.circle.fill")
                        .font(.subheadline)
                        .foregroundColor(.blue)
                }
                .buttonStyle(.plain)
                Spacer()
                if receiver.activeOutputs.count >= 2 {
                    Button(action: { showCreateGroup = true }) {
                        Label("Group", systemImage: "rectangle.3.group")
                            .font(.caption)
                            .foregroundColor(.solunaGradientStart)
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 14)
        }
        .glassCard()
        .cornerRadius(20)
    }

    // MARK: - Save Preset Sheet

    private var savePresetSheet: some View {
        VStack(spacing: 16) {
            Text("Save Preset")
                .font(.headline)
                .padding(.top, 16)
            TextField("Preset name (e.g. Living Room)", text: $presetName)
                .textFieldStyle(.roundedBorder)
                .padding(.horizontal, 16)
            HStack {
                Button("Cancel") { showSavePreset = false }
                    .keyboardShortcut(.cancelAction)
                Spacer()
                Button("Save") {
                    receiver.savePreset(name: presetName)
                    showSavePreset = false
                }
                .keyboardShortcut(.defaultAction)
                .disabled(presetName.isEmpty)
            }
            .padding(.horizontal, 16)
            .padding(.bottom, 16)
        }
        .frame(width: 320)
    }

    // MARK: - Create Group Sheet

    private var createGroupSheet: some View {
        VStack(spacing: 12) {
            Text("Create Speaker Group")
                .font(.headline)
                .padding(.top, 16)
            TextField("Group name", text: $groupName)
                .textFieldStyle(.roundedBorder)
                .padding(.horizontal, 16)
            List {
                ForEach(receiver.availableDevices.filter(\.isActive)) { device in
                    Button {
                        if selectedGroupDevices.contains(device.id) {
                            selectedGroupDevices.remove(device.id)
                        } else {
                            selectedGroupDevices.insert(device.id)
                        }
                    } label: {
                        HStack {
                            Image(systemName: selectedGroupDevices.contains(device.id) ? "checkmark.circle.fill" : "circle")
                                .foregroundColor(selectedGroupDevices.contains(device.id) ? .blue : .secondary)
                            Text(device.name)
                            Spacer()
                            Text(device.transportType.rawValue)
                                .font(.caption)
                                .foregroundColor(.secondary)
                        }
                    }
                    .buttonStyle(.plain)
                }
            }
            .frame(height: 150)
            HStack {
                Button("Cancel") {
                    showCreateGroup = false
                    groupName = ""
                    selectedGroupDevices = []
                }
                Spacer()
                Button("Create") {
                    let names = receiver.availableDevices
                        .filter { selectedGroupDevices.contains($0.id) }
                        .map(\.name)
                    if !groupName.isEmpty && !names.isEmpty {
                        receiver.createGroup(name: groupName, deviceNames: names)
                    }
                    showCreateGroup = false
                    groupName = ""
                    selectedGroupDevices = []
                }
                .disabled(groupName.isEmpty || selectedGroupDevices.isEmpty)
            }
            .padding(.horizontal, 16)
            .padding(.bottom, 16)
        }
        .frame(width: 320)
    }

    // MARK: - Add Speaker Sheet

    private var addSpeakerSheet: some View {
        VStack(spacing: 0) {
            Text("Add Speaker")
                .font(.headline)
                .padding(.top, 16)
                .padding(.bottom, 12)

            Picker("", selection: $addSpeakerTab) {
                Text("Channel").tag(2)
                Text("Local").tag(0)
                Text("Network").tag(1)
            }
            .pickerStyle(.segmented)
            .padding(.horizontal, 16)
            .padding(.bottom, 12)

            if addSpeakerTab == 2 {
                VStack(spacing: 12) {
                    Image(systemName: "globe")
                        .font(.system(size: 32))
                        .foregroundColor(.solunaGradientStart)
                    Text("Enter a channel name to connect\nvia WAN relay")
                        .font(.caption)
                        .foregroundColor(.secondary)
                        .multilineTextAlignment(.center)
                    TextField("Channel name (e.g. ambient-tokyo)", text: $groupCode)
                        .textFieldStyle(.roundedBorder)
                        .padding(.horizontal, 16)
                    Button("Connect") {
                        let code = groupCode.trimmingCharacters(in: .whitespacesAndNewlines)
                        guard !code.isEmpty else { return }
                        if receiver.state != .receiving { receiver.start() }
                        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                            receiver.connectRelay(group: code)
                        }
                        showAddSpeaker = false
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.purple)
                    .disabled(groupCode.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                }
                .padding(.vertical, 16)
            } else if addSpeakerTab == 0 {
                let inactiveDevices = receiver.availableDevices.filter { !$0.isActive }
                if inactiveDevices.isEmpty {
                    VStack(spacing: 8) {
                        Image(systemName: "speaker.badge.exclamationmark")
                            .font(.title2)
                            .foregroundColor(.secondary)
                        Text("No additional devices found")
                            .font(.subheadline)
                            .foregroundColor(.secondary)
                        Text("Connect a Bluetooth or AirPlay speaker")
                            .font(.caption)
                            .foregroundColor(.secondary.opacity(0.7))
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 24)
                } else {
                    ScrollView {
                        VStack(spacing: 0) {
                            ForEach(inactiveDevices) { device in
                                Button {
                                    receiver.enableDevice(device.id)
                                    speakers.recalculateAllDelays()
                                    showAddSpeaker = false
                                } label: {
                                    HStack(spacing: 12) {
                                        Image(systemName: device.transportType.iconName)
                                            .font(.system(size: 18))
                                            .foregroundColor(.blue)
                                            .frame(width: 32)
                                        VStack(alignment: .leading, spacing: 2) {
                                            Text(device.name)
                                                .font(.subheadline.weight(.medium))
                                                .foregroundColor(.primary)
                                            HStack(spacing: 6) {
                                                Text(device.transportType.rawValue)
                                                    .font(.caption2)
                                                    .foregroundColor(.secondary)
                                                if device.hardwareLatencyMs > 0 {
                                                    Text("\(String(format: "%.0f", device.hardwareLatencyMs))ms")
                                                        .font(.caption2)
                                                        .foregroundColor(.orange)
                                                }
                                            }
                                        }
                                        Spacer()
                                        Button {
                                            receiver.toggleFavorite(device.name)
                                        } label: {
                                            Image(systemName: receiver.favoriteDeviceNames.contains(device.name) ? "star.fill" : "star")
                                                .foregroundColor(receiver.favoriteDeviceNames.contains(device.name) ? .yellow : .secondary.opacity(0.4))
                                                .font(.system(size: 14))
                                        }
                                        .buttonStyle(.plain)
                                        .help("Auto-enable on connect")
                                        Image(systemName: "plus.circle.fill")
                                            .foregroundColor(.blue)
                                    }
                                    .padding(.horizontal, 16)
                                    .padding(.vertical, 10)
                                }
                                .buttonStyle(.plain)
                                Divider().padding(.horizontal, 16)
                            }
                        }
                    }
                    .frame(maxHeight: 200)
                }
            } else {
                Form {
                    TextField("Name (e.g. Mac, Living Room)", text: $newName)
                    TextField("IP Address / Host", text: $newHost)
                        .disableAutocorrection(true)
                }
                .padding(.horizontal, 16)

                HStack {
                    Spacer()
                    Button("Add") {
                        speakers.add(name: newName, host: newHost)
                        showAddSpeaker = false
                    }
                    .keyboardShortcut(.defaultAction)
                    .disabled(newHost.isEmpty)
                }
                .padding(.horizontal, 16)
            }

            Divider()

            Button("Cancel") { showAddSpeaker = false }
                .keyboardShortcut(.cancelAction)
                .frame(maxWidth: .infinity)
                .padding(.vertical, 10)
        }
        .frame(width: 380)
        .onAppear { receiver.refreshDevices() }
    }

    // MARK: - Preset Import/Export

    private func exportPresets(receiver: AudioReceiver) {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.json]
        panel.nameFieldStringValue = "soluna-presets.json"
        panel.title = "Export Presets"
        if panel.runModal() == .OK, let url = panel.url {
            try? receiver.exportPresets(to: url)
        }
    }

    private func importPresets(receiver: AudioReceiver) {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.json]
        panel.title = "Import Presets"
        if panel.runModal() == .OK, let url = panel.url {
            try? receiver.importPresets(from: url)
        }
    }

    // MARK: - Helpers

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
                    .fill(Color(nsColor: .separatorColor))
                RoundedRectangle(cornerRadius: 3)
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
    @State private var rms: Float = 0
    @State private var peak: Float = 0
    @State private var balance: Float = 0

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

                if receiver.state == .receiving {
                    LevelMeter(rms: rms, peak: peak)
                        .onReceive(Timer.publish(every: 0.05, on: .main, in: .common).autoconnect()) { _ in
                            rms = receiver.primaryLevelRms()
                            peak = receiver.primaryLevelPeak()
                        }
                }

                // L/R balance
                HStack(spacing: 6) {
                    Text("L")
                        .font(.system(size: 9, weight: .bold))
                        .foregroundColor(.secondary)
                    Slider(value: $balance, in: -1...1, step: 0.05)
                        .onChange(of: balance) { b in receiver.setPrimaryBalance(b) }
                    Text("R")
                        .font(.system(size: 9, weight: .bold))
                        .foregroundColor(.secondary)
                }

                // 3-band EQ
                EQSliderGroup { band, gain in
                    receiver.setPrimaryEQ(band: band, gain: gain)
                }

                // Compressor
                CompressorControls { thresh, ratio, att, rel, en in
                    receiver.setPrimaryCompressor(threshold: thresh, ratio: ratio, attack: att, release: rel, enabled: en)
                }

                // Crossover (LPF/HPF)
                CrossoverControls { mode, freq in
                    receiver.setPrimaryCrossover(mode: mode, frequency: freq)
                }

                // Spatial Audio
                SpatialControls { en, w, cf in
                    receiver.setPrimarySpatial(enabled: en, width: w, crossfeed: cf)
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

// MARK: - LocalDeviceRow (BT / AirPlay / USB extra output)

private struct LocalDeviceRow: View {
    let device: LocalOutputDevice
    let measuredLatencyMs: Float
    let receiver: AudioReceiver
    let onToggle: () -> Void
    let onVolume: (Float) -> Void
    let onMute: (Bool) -> Void
    let onOffset: (Float) -> Void
    let onBalance: (Float) -> Void
    let onExclusive: (Bool) -> Void
    let onEQ: (Int, Float) -> Void
    let onCompressor: (Float, Float, Float, Float, Bool) -> Void
    let onCrossover: (Int, Float) -> Void
    let onSpatial: (Bool, Float, Float) -> Void

    @State private var vol: Float
    @State private var muted: Bool
    @State private var offsetMs: Float
    @State private var bal: Float
    @State private var exclusive: Bool = false
    @State private var rms: Float = 0
    @State private var peak: Float = 0

    init(device: LocalOutputDevice, measuredLatencyMs: Float = 0,
         receiver: AudioReceiver,
         onToggle: @escaping () -> Void,
         onVolume: @escaping (Float) -> Void, onMute: @escaping (Bool) -> Void,
         onOffset: @escaping (Float) -> Void = { _ in },
         onBalance: @escaping (Float) -> Void = { _ in },
         onExclusive: @escaping (Bool) -> Void = { _ in },
         onEQ: @escaping (Int, Float) -> Void = { _, _ in },
         onCompressor: @escaping (Float, Float, Float, Float, Bool) -> Void = { _, _, _, _, _ in },
         onCrossover: @escaping (Int, Float) -> Void = { _, _ in },
         onSpatial: @escaping (Bool, Float, Float) -> Void = { _, _, _ in }) {
        self.device = device
        self.measuredLatencyMs = measuredLatencyMs
        self.receiver = receiver
        self.onToggle = onToggle
        self.onVolume = onVolume
        self.onMute = onMute
        self.onOffset = onOffset
        self.onBalance = onBalance
        self.onExclusive = onExclusive
        self.onEQ = onEQ
        self.onCompressor = onCompressor
        self.onCrossover = onCrossover
        self.onSpatial = onSpatial
        _vol = State(initialValue: device.volume)
        _muted = State(initialValue: device.muted)
        _offsetMs = State(initialValue: device.manualOffsetMs)
        _bal = State(initialValue: device.balance)
    }

    var body: some View {
        HStack(spacing: 12) {
            speakerIcon(systemName: device.transportType.iconName, connected: true)

            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Text(device.name)
                        .font(.subheadline.weight(.semibold))
                    Text(device.transportType.rawValue)
                        .font(.caption2.weight(.medium))
                        .foregroundColor(.white)
                        .padding(.horizontal, 6)
                        .padding(.vertical, 2)
                        .background(badgeColor)
                        .clipShape(Capsule())
                    if measuredLatencyMs > 1 || device.hardwareLatencyMs > 1 {
                        let latency = measuredLatencyMs > 0 ? measuredLatencyMs : device.hardwareLatencyMs
                        let isMeasured = measuredLatencyMs > 0
                        Text("\(String(format: "%.0f", latency))ms")
                            .font(.system(size: 11, weight: .medium, design: .monospaced))
                            .foregroundColor(isMeasured ? .green : .orange)
                            .padding(.horizontal, 7)
                            .padding(.vertical, 3)
                            .background((isMeasured ? Color.solunaLive : Color.orange).opacity(0.1))
                            .clipShape(Capsule())
                    }
                    if device.nativeSampleRate > 0 && device.nativeSampleRate != 48000 {
                        Text(formatSampleRate(device.nativeSampleRate))
                            .font(.system(size: 11, weight: .medium, design: .monospaced))
                            .foregroundColor(.cyan)
                            .padding(.horizontal, 7)
                            .padding(.vertical, 3)
                            .background(Color.cyan.opacity(0.1))
                            .clipShape(Capsule())
                    }
                    Spacer()
                    muteButton
                    Text(muted ? "Muted" : "\(Int(vol * 100))%")
                        .font(.system(size: 12, design: .monospaced))
                        .foregroundColor(muted ? .red : .secondary)
                        .frame(width: 42, alignment: .trailing)
                }

                Slider(value: $vol, in: 0...1)
                    .onChange(of: vol) { v in
                        if muted { muted = false; onMute(false) }
                        onVolume(v)
                    }

                LevelMeter(rms: rms, peak: peak)
                    .onReceive(Timer.publish(every: 0.05, on: .main, in: .common).autoconnect()) { _ in
                        rms = receiver.deviceLevelRms(for: device.id)
                        peak = receiver.deviceLevelPeak(for: device.id)
                    }

                // Manual delay offset slider (±50ms)
                HStack(spacing: 6) {
                    Image(systemName: "timer")
                        .font(.system(size: 10))
                        .foregroundColor(.secondary)
                    Slider(value: $offsetMs, in: -50...50, step: 1)
                        .onChange(of: offsetMs) { v in onOffset(v) }
                    Text("\(offsetMs >= 0 ? "+" : "")\(Int(offsetMs))ms")
                        .font(.system(size: 10, weight: .medium, design: .monospaced))
                        .foregroundColor(offsetMs == 0 ? .secondary : .orange)
                        .frame(width: 48, alignment: .trailing)
                }

                // L/R balance
                HStack(spacing: 6) {
                    Text("L")
                        .font(.system(size: 9, weight: .bold))
                        .foregroundColor(.secondary)
                    Slider(value: $bal, in: -1...1, step: 0.05)
                        .onChange(of: bal) { b in onBalance(b) }
                    Text("R")
                        .font(.system(size: 9, weight: .bold))
                        .foregroundColor(.secondary)
                }

                // 3-band EQ
                EQSliderGroup(onBandChange: onEQ)

                // Compressor
                CompressorControls(onChange: onCompressor)

                // Crossover (LPF/HPF)
                CrossoverControls(onChange: onCrossover)

                // Spatial Audio
                SpatialControls(onChange: onSpatial)

                HStack {
                    if device.transportType == .usb {
                        Button {
                            exclusive.toggle()
                            onExclusive(exclusive)
                        } label: {
                            Label(exclusive ? "Exclusive" : "Shared",
                                  systemImage: exclusive ? "lock.fill" : "lock.open")
                                .font(.caption)
                                .foregroundColor(exclusive ? .green : .secondary)
                        }
                        .buttonStyle(.plain)
                    }
                    Spacer()
                    Button(action: onToggle) {
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
            muted.toggle()
            onMute(muted)
        } label: {
            Image(systemName: muted ? "speaker.slash.fill" : volumeIcon)
                .font(.system(size: 13, weight: .medium))
                .foregroundColor(muted ? .red : .secondary)
                .frame(width: 28, height: 28)
                .background(muted ? Color.red.opacity(0.1) : Color(nsColor: .tertiaryLabelColor).opacity(0.2))
                .clipShape(Circle())
        }
        .buttonStyle(.plain)
    }

    private var volumeIcon: String {
        vol < 0.01 ? "speaker.fill"
        : vol < 0.5 ? "speaker.wave.1.fill"
        : "speaker.wave.3.fill"
    }

    private var badgeColor: Color {
        switch device.transportType {
        case .bluetooth: return .blue
        case .airPlay:   return .purple
        case .usb:       return .green
        default:         return .gray
        }
    }

    private func formatSampleRate(_ rate: Double) -> String {
        if rate >= 1000 {
            let khz = rate / 1000.0
            return khz.truncatingRemainder(dividingBy: 1) == 0
                ? "\(Int(khz))kHz"
                : String(format: "%.1fkHz", khz)
        }
        return "\(Int(rate))Hz"
    }
}

// MARK: - RemoteSpeakerRow

private struct RemoteSpeakerRow: View {
    let name: String
    let speakerChannel: String
    @ObservedObject var daemon: DaemonClient
    let onRemove: () -> Void
    let onChannelChange: (String) -> Void

    @State private var isEditingChannel = false
    @State private var editedChannel    = ""

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
                            .foregroundColor(.solunaLive)
                            .padding(.horizontal, 7)
                            .padding(.vertical, 3)
                            .background(Color.solunaLive.opacity(0.1))
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

                // Per-speaker channel
                HStack(spacing: 6) {
                    Image(systemName: "dot.radiowaves.left.and.right")
                        .font(.system(size: 9))
                        .foregroundColor(.purple.opacity(0.7))

                    if isEditingChannel {
                        TextField("channel", text: $editedChannel, onCommit: {
                            let ch = editedChannel.trimmingCharacters(in: .whitespacesAndNewlines)
                            if !ch.isEmpty { onChannelChange(ch) }
                            isEditingChannel = false
                        })
                        .textFieldStyle(.roundedBorder)
                        .font(.system(size: 10, design: .monospaced))
                        .frame(maxWidth: 100)

                        Button {
                            let ch = editedChannel.trimmingCharacters(in: .whitespacesAndNewlines)
                            if !ch.isEmpty { onChannelChange(ch) }
                            isEditingChannel = false
                        } label: {
                            Image(systemName: "checkmark")
                                .font(.system(size: 9, weight: .bold))
                                .foregroundColor(.solunaLive)
                        }
                        .buttonStyle(.plain)
                    } else {
                        Text(speakerChannel.isEmpty ? "default" : speakerChannel)
                            .font(.system(size: 10, design: .monospaced))
                            .foregroundColor(speakerChannel.isEmpty ? .secondary : .purple)

                        Button {
                            editedChannel = speakerChannel
                            isEditingChannel = true
                        } label: {
                            Image(systemName: "pencil")
                                .font(.system(size: 9))
                                .foregroundColor(.secondary)
                        }
                        .buttonStyle(.plain)
                        .disabled(!daemon.isConnected)
                    }

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
            .fill(connected ? Color.solunaLive : Color(nsColor: .separatorColor))
            .frame(width: 8, height: 8)
            .overlay(Circle().stroke(Color.solunaSurface, lineWidth: 1.5))
    }
}

// MARK: - LatencyGraphView

private struct LatencyGraphView: View {
    let history: [UInt32: [Float]]
    let devices: [LocalOutputDevice]

    private let graphColors: [Color] = [.blue, .purple, .green, .orange, .pink, .cyan]

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Image(systemName: "chart.xyaxis.line")
                    .foregroundColor(.blue)
                Text("Latency")
                    .font(.caption.weight(.semibold))
                    .foregroundColor(.secondary)
                Spacer()
                // Legend
                ForEach(Array(legendItems.enumerated()), id: \.offset) { idx, item in
                    HStack(spacing: 3) {
                        Circle().fill(item.color).frame(width: 6, height: 6)
                        Text(item.name)
                            .font(.system(size: 9))
                            .foregroundColor(.secondary)
                    }
                }
            }

            // Graph area
            GeometryReader { geo in
                let w = geo.size.width
                let h = geo.size.height
                let maxMs = allValues.max() ?? 100
                let scale = maxMs > 0 ? h / CGFloat(maxMs * 1.1) : 1

                ForEach(Array(sortedEntries.enumerated()), id: \.offset) { idx, entry in
                    let color = graphColors[idx % graphColors.count]
                    let samples = entry.value
                    let count = samples.count
                    if count > 1 {
                        Path { path in
                            for (i, val) in samples.enumerated() {
                                let x = w * CGFloat(i) / CGFloat(count - 1)
                                let y = h - CGFloat(val) * scale
                                if i == 0 { path.move(to: CGPoint(x: x, y: y)) }
                                else { path.addLine(to: CGPoint(x: x, y: y)) }
                            }
                        }
                        .stroke(color, lineWidth: 1.5)
                    }
                }

                // Y-axis labels
                VStack {
                    Text("\(Int(maxMs))ms")
                        .font(.system(size: 8, design: .monospaced))
                        .foregroundColor(.secondary.opacity(0.5))
                    Spacer()
                    Text("0")
                        .font(.system(size: 8, design: .monospaced))
                        .foregroundColor(.secondary.opacity(0.5))
                }
            }
            .frame(height: 60)
        }
        .padding(12)
        .glassCard()
        .cornerRadius(12)
    }

    private var sortedEntries: [(key: UInt32, value: [Float])] {
        history.sorted { $0.key < $1.key }
    }

    private var allValues: [Float] {
        history.values.flatMap { $0 }
    }

    private var legendItems: [(name: String, color: Color)] {
        sortedEntries.enumerated().map { idx, entry in
            let name = devices.first(where: { $0.id == entry.key })?.name ?? "Device \(entry.key)"
            let shortName = name.count > 12 ? String(name.prefix(12)) + "..." : name
            return (shortName, graphColors[idx % graphColors.count])
        }
    }
}

// MARK: - PresetRow

private struct PresetRow: View {
    let presets: [AudioRoutingPreset]
    let hasActiveDevices: Bool
    let onApply: (AudioRoutingPreset) -> Void
    let onDelete: (UUID) -> Void
    let onSave: () -> Void
    let onExport: () -> Void
    let onImport: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Image(systemName: "list.bullet.rectangle")
                    .font(.system(size: 12))
                    .foregroundColor(.solunaGradientStart)
                Text("Presets")
                    .font(.caption.weight(.semibold))
                    .foregroundColor(.secondary)
                Spacer()
                Button(action: onImport) {
                    Image(systemName: "square.and.arrow.down")
                        .font(.system(size: 11))
                        .foregroundColor(.secondary)
                }
                .buttonStyle(.plain)
                .help("Import presets")
                if !presets.isEmpty {
                    Button(action: onExport) {
                        Image(systemName: "square.and.arrow.up")
                            .font(.system(size: 11))
                            .foregroundColor(.secondary)
                    }
                    .buttonStyle(.plain)
                    .help("Export presets")
                }
                if hasActiveDevices {
                    Button(action: onSave) {
                        Label("Save", systemImage: "plus.circle")
                            .font(.caption.weight(.medium))
                            .foregroundColor(.solunaGradientStart)
                    }
                    .buttonStyle(.plain)
                }
            }

            if presets.isEmpty {
                Text("No saved presets")
                    .font(.caption)
                    .foregroundColor(.secondary.opacity(0.6))
            } else {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 8) {
                        ForEach(presets) { preset in
                            Button { onApply(preset) } label: {
                                HStack(spacing: 4) {
                                    Text(preset.name)
                                        .font(.caption.weight(.medium))
                                    Text("\(preset.deviceConfigs.count)")
                                        .font(.system(size: 9, weight: .bold, design: .monospaced))
                                        .foregroundColor(.white)
                                        .frame(width: 16, height: 16)
                                        .background(Color.solunaGradientStart)
                                        .clipShape(Circle())
                                }
                                .padding(.horizontal, 10)
                                .padding(.vertical, 6)
                                .background(Color.solunaGradientStart.opacity(0.1))
                                .clipShape(Capsule())
                            }
                            .buttonStyle(.plain)
                            .contextMenu {
                                Button(role: .destructive) {
                                    onDelete(preset.id)
                                } label: {
                                    Label("Delete", systemImage: "trash")
                                }
                            }
                        }
                    }
                }
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
    }
}

// MARK: - LevelMeter

// MARK: - SpeakerGroupRow

private struct SpeakerGroupRow: View {
    let group: SpeakerGroup
    let onVolume: (Float) -> Void
    let onMute: (Bool) -> Void
    let onDelete: () -> Void
    @State private var vol: Float
    @State private var muted: Bool

    init(group: SpeakerGroup, onVolume: @escaping (Float) -> Void, onMute: @escaping (Bool) -> Void, onDelete: @escaping () -> Void) {
        self.group = group
        self.onVolume = onVolume
        self.onMute = onMute
        self.onDelete = onDelete
        _vol = State(initialValue: group.volume)
        _muted = State(initialValue: group.muted)
    }

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: "rectangle.3.group.fill")
                .font(.system(size: 14))
                .foregroundColor(.solunaGradientStart)
                .frame(width: 28)
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text(group.name)
                        .font(.caption.weight(.semibold))
                    Text("\(group.deviceNames.count)")
                        .font(.system(size: 9, weight: .bold))
                        .foregroundColor(.white)
                        .frame(width: 14, height: 14)
                        .background(Color.solunaGradientStart)
                        .clipShape(Circle())
                    Spacer()
                    Button { muted.toggle(); onMute(muted) } label: {
                        Image(systemName: muted ? "speaker.slash.fill" : "speaker.wave.2.fill")
                            .font(.system(size: 11))
                            .foregroundColor(muted ? .red : .secondary)
                    }
                    .buttonStyle(.plain)
                    Button(action: onDelete) {
                        Image(systemName: "xmark.circle")
                            .font(.system(size: 11))
                            .foregroundColor(.red.opacity(0.6))
                    }
                    .buttonStyle(.plain)
                }
                Slider(value: $vol, in: 0...1)
                    .onChange(of: vol) { v in onVolume(v) }
                    .controlSize(.small)
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 4)
    }
}

// MARK: - Remote Volume Control

private struct RemoteVolumeSection: View {
    @ObservedObject var receiver: AudioReceiver
    @State private var volumes: [String: Double] = [:]

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Label("Remote Volume", systemImage: "speaker.wave.2.fill")
                .font(.system(size: 11, weight: .semibold))
                .foregroundColor(.white.opacity(0.5))
                .padding(.horizontal, 8)
            ForEach(receiver.groupMembers) { member in
                HStack(spacing: 8) {
                    Image(systemName: member.role == "dj" || member.role == "owner"
                          ? "music.mic" : "iphone")
                        .font(.system(size: 11))
                        .foregroundColor(.white.opacity(0.5))
                        .frame(width: 14)
                    Text(member.name)
                        .font(.system(size: 11))
                        .foregroundColor(.white.opacity(0.7))
                        .lineLimit(1)
                        .frame(width: 60, alignment: .leading)
                    Slider(
                        value: Binding(
                            get: { volumes[member.deviceId] ?? 80 },
                            set: { newVal in
                                volumes[member.deviceId] = newVal
                                receiver.sendVolumeToDevice(member.deviceId, level: Int(newVal))
                            }
                        ),
                        in: 0...100
                    )
                    .tint(.solunaGradientMid)
                    Text("\(Int(volumes[member.deviceId] ?? 80))%")
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundColor(.white.opacity(0.4))
                        .frame(width: 32, alignment: .trailing)
                }
                .padding(.horizontal, 8)
            }
        }
        .padding(.vertical, 4)
    }
}

// MARK: - Spectrum Analyzer (32-band FFT)

private struct SpectrumView: View {
    @ObservedObject var receiver: AudioReceiver
    @State private var bands: [Float] = Array(repeating: 0, count: 32)

    private static let labels = ["20", "", "", "", "60", "", "", "", "200", "", "", "", "600", "", "", "", "2K", "", "", "", "6K", "", "", "", "20K", "", "", "", "", "", "", ""]

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Spectrum")
                .font(.caption.weight(.semibold))
                .foregroundColor(.secondary)

            GeometryReader { geo in
                let barW = max(2, (geo.size.width - CGFloat(31)) / 32)
                let h = geo.size.height
                HStack(alignment: .bottom, spacing: 1) {
                    ForEach(0..<32, id: \.self) { i in
                        RoundedRectangle(cornerRadius: 1.5)
                            .fill(barColor(bands[i]))
                            .frame(width: barW, height: max(2, h * CGFloat(bands[i])))
                    }
                }
            }
            .frame(height: 60)

            HStack {
                Text("20Hz").font(.system(size: 8, design: .monospaced)).foregroundColor(.secondary)
                Spacer()
                Text("1kHz").font(.system(size: 8, design: .monospaced)).foregroundColor(.secondary)
                Spacer()
                Text("20kHz").font(.system(size: 8, design: .monospaced)).foregroundColor(.secondary)
            }
        }
        .padding(16)
        .glassCard()
        .cornerRadius(12)
        .onReceive(Timer.publish(every: 0.05, on: .main, in: .common).autoconnect()) { _ in
            bands = receiver.spectrumBands()
        }
    }

    private func barColor(_ v: Float) -> Color {
        if v > 0.85 { return .red }
        if v > 0.6 { return .orange }
        return .green
    }
}

// MARK: - 3-Band EQ Sliders

private struct EQSliderGroup: View {
    @State private var low: Float = 0
    @State private var mid: Float = 0
    @State private var high: Float = 0
    @State private var expanded = false
    let onBandChange: (Int, Float) -> Void  // (band, gain_db)

    var body: some View {
        VStack(spacing: 4) {
            Button {
                withAnimation(.easeInOut(duration: 0.2)) { expanded.toggle() }
            } label: {
                HStack(spacing: 4) {
                    Image(systemName: "slider.vertical.3")
                        .font(.system(size: 10))
                    Text("EQ")
                        .font(.system(size: 10, weight: .medium))
                    Spacer()
                    if !expanded && (low != 0 || mid != 0 || high != 0) {
                        Text("\(formatDb(low))/\(formatDb(mid))/\(formatDb(high))")
                            .font(.system(size: 9, design: .monospaced))
                            .foregroundColor(.orange)
                    }
                    Image(systemName: expanded ? "chevron.up" : "chevron.down")
                        .font(.system(size: 9))
                }
                .foregroundColor(.secondary)
            }
            .buttonStyle(.plain)

            if expanded {
                eqBand(label: "200", value: $low, band: 0)
                eqBand(label: "1K", value: $mid, band: 1)
                eqBand(label: "5K", value: $high, band: 2)

                if low != 0 || mid != 0 || high != 0 {
                    Button("Reset") {
                        low = 0; mid = 0; high = 0
                        onBandChange(0, 0); onBandChange(1, 0); onBandChange(2, 0)
                    }
                    .font(.caption2)
                    .foregroundColor(.secondary)
                }
            }
        }
    }

    private func eqBand(label: String, value: Binding<Float>, band: Int) -> some View {
        HStack(spacing: 6) {
            Text(label)
                .font(.system(size: 9, weight: .bold, design: .monospaced))
                .foregroundColor(.secondary)
                .frame(width: 22, alignment: .trailing)
            Slider(value: value, in: -12...12, step: 0.5)
                .onChange(of: value.wrappedValue) { v in onBandChange(band, v) }
            Text(formatDb(value.wrappedValue))
                .font(.system(size: 9, design: .monospaced))
                .foregroundColor(value.wrappedValue == 0 ? .secondary : .orange)
                .frame(width: 32, alignment: .trailing)
        }
    }

    private func formatDb(_ v: Float) -> String {
        v == 0 ? "0" : String(format: "%+.0f", v)
    }
}

// MARK: - Compressor Controls

private struct CompressorControls: View {
    @State private var enabled = false
    @State private var threshold: Float = -20
    @State private var ratio: Float = 4
    @State private var attack: Float = 10
    @State private var release: Float = 100
    @State private var expanded = false
    let onChange: (Float, Float, Float, Float, Bool) -> Void

    var body: some View {
        VStack(spacing: 4) {
            Button {
                withAnimation(.easeInOut(duration: 0.2)) { expanded.toggle() }
            } label: {
                HStack(spacing: 4) {
                    Image(systemName: "waveform.path")
                        .font(.system(size: 10))
                    Text("Comp")
                        .font(.system(size: 10, weight: .medium))
                    Spacer()
                    if enabled && !expanded {
                        Text("\(Int(threshold))dB \(String(format: "%.0f", ratio)):1")
                            .font(.system(size: 9, design: .monospaced))
                            .foregroundColor(.cyan)
                    }
                    Image(systemName: expanded ? "chevron.up" : "chevron.down")
                        .font(.system(size: 9))
                }
                .foregroundColor(.secondary)
            }
            .buttonStyle(.plain)

            if expanded {
                Toggle("Enabled", isOn: $enabled)
                    .font(.caption)
                    .onChange(of: enabled) { _ in fire() }

                compSlider(label: "Thresh", value: $threshold, range: -60...0, unit: "dB")
                compSlider(label: "Ratio", value: $ratio, range: 1...20, unit: ":1")
                compSlider(label: "Attack", value: $attack, range: 0.1...100, unit: "ms")
                compSlider(label: "Release", value: $release, range: 10...1000, unit: "ms")
            }
        }
    }

    private func compSlider(label: String, value: Binding<Float>, range: ClosedRange<Float>, unit: String) -> some View {
        HStack(spacing: 6) {
            Text(label)
                .font(.system(size: 9, weight: .bold))
                .foregroundColor(.secondary)
                .frame(width: 40, alignment: .trailing)
            Slider(value: value, in: range)
                .onChange(of: value.wrappedValue) { _ in fire() }
            Text("\(String(format: "%.0f", value.wrappedValue))\(unit)")
                .font(.system(size: 9, design: .monospaced))
                .foregroundColor(.secondary)
                .frame(width: 42, alignment: .trailing)
        }
    }

    private func fire() {
        onChange(threshold, ratio, attack, release, enabled)
    }
}

// MARK: - Crossover Controls

private struct CrossoverControls: View {
    @State private var mode: Int = 0  // 0=off, 1=LPF, 2=HPF
    @State private var freq: Float = 80
    @State private var expanded = false
    let onChange: (Int, Float) -> Void

    var body: some View {
        VStack(spacing: 4) {
            Button {
                withAnimation(.easeInOut(duration: 0.2)) { expanded.toggle() }
            } label: {
                HStack(spacing: 4) {
                    Image(systemName: "tuningfork")
                        .font(.system(size: 10))
                    Text("Crossover")
                        .font(.system(size: 10, weight: .medium))
                    Spacer()
                    if mode != 0 && !expanded {
                        Text("\(mode == 1 ? "LPF" : "HPF") \(Int(freq))Hz")
                            .font(.system(size: 9, design: .monospaced))
                            .foregroundColor(.solunaGradientStart)
                    }
                    Image(systemName: expanded ? "chevron.up" : "chevron.down")
                        .font(.system(size: 9))
                }
                .foregroundColor(.secondary)
            }
            .buttonStyle(.plain)

            if expanded {
                Picker("Filter", selection: $mode) {
                    Text("Off").tag(0)
                    Text("Low Pass").tag(1)
                    Text("High Pass").tag(2)
                }
                .pickerStyle(.segmented)
                .onChange(of: mode) { _ in onChange(mode, freq) }

                if mode != 0 {
                    HStack(spacing: 6) {
                        Text("Freq")
                            .font(.system(size: 9, weight: .bold))
                            .foregroundColor(.secondary)
                            .frame(width: 30, alignment: .trailing)
                        Slider(value: $freq, in: 20...20000)
                            .onChange(of: freq) { _ in onChange(mode, freq) }
                        Text("\(Int(freq))Hz")
                            .font(.system(size: 9, design: .monospaced))
                            .foregroundColor(.secondary)
                            .frame(width: 50, alignment: .trailing)
                    }

                    // Quick presets
                    HStack(spacing: 8) {
                        ForEach([("Sub", Float(80)), ("Low", Float(200)), ("Mid", Float(1000)), ("High", Float(5000))], id: \.0) { preset in
                            Button(preset.0) {
                                freq = preset.1
                                onChange(mode, freq)
                            }
                            .font(.system(size: 9, weight: .medium))
                            .buttonStyle(.plain)
                            .foregroundColor(freq == preset.1 ? .purple : .secondary)
                        }
                    }
                }
            }
        }
    }
}

// MARK: - Spatial Audio Controls

private struct SpatialControls: View {
    @State private var enabled = false
    @State private var width: Float = 1.0
    @State private var crossfeed: Float = 0.15
    @State private var expanded = false
    let onChange: (Bool, Float, Float) -> Void

    var body: some View {
        VStack(spacing: 4) {
            Button {
                withAnimation(.easeInOut(duration: 0.2)) { expanded.toggle() }
            } label: {
                HStack(spacing: 4) {
                    Image(systemName: "ear")
                        .font(.system(size: 10))
                    Text("Spatial")
                        .font(.system(size: 10, weight: .medium))
                    Spacer()
                    if enabled && !expanded {
                        Text("W:\(String(format: "%.1f", width)) CF:\(String(format: "%.0f", crossfeed * 100))%")
                            .font(.system(size: 9, design: .monospaced))
                            .foregroundColor(.indigo)
                    }
                    Image(systemName: expanded ? "chevron.up" : "chevron.down")
                        .font(.system(size: 9))
                }
                .foregroundColor(.secondary)
            }
            .buttonStyle(.plain)

            if expanded {
                Toggle("Enabled", isOn: $enabled)
                    .font(.caption)
                    .onChange(of: enabled) { _ in fire() }

                HStack(spacing: 6) {
                    Text("Width")
                        .font(.system(size: 9, weight: .bold))
                        .foregroundColor(.secondary)
                        .frame(width: 45, alignment: .trailing)
                    Slider(value: $width, in: 0...2, step: 0.05)
                        .onChange(of: width) { _ in fire() }
                    Text(width < 0.05 ? "Mono" : width < 1.05 && width > 0.95 ? "Stereo" : String(format: "%.1fx", width))
                        .font(.system(size: 9, design: .monospaced))
                        .foregroundColor(.secondary)
                        .frame(width: 42, alignment: .trailing)
                }

                HStack(spacing: 6) {
                    Text("X-Feed")
                        .font(.system(size: 9, weight: .bold))
                        .foregroundColor(.secondary)
                        .frame(width: 45, alignment: .trailing)
                    Slider(value: $crossfeed, in: 0...0.5, step: 0.01)
                        .onChange(of: crossfeed) { _ in fire() }
                    Text("\(Int(crossfeed * 100))%")
                        .font(.system(size: 9, design: .monospaced))
                        .foregroundColor(.secondary)
                        .frame(width: 42, alignment: .trailing)
                }
            }
        }
    }

    private func fire() {
        onChange(enabled, width, crossfeed)
    }
}

private struct LevelMeter: View {
    let rms: Float
    let peak: Float

    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width
            ZStack(alignment: .leading) {
                // Background
                Capsule()
                    .fill(Color(nsColor: .separatorColor).opacity(0.3))
                // RMS bar
                Capsule()
                    .fill(rmsColor)
                    .frame(width: max(0, w * CGFloat(rms)))
                // Peak indicator
                if peak > 0.01 {
                    Capsule()
                        .fill(peak > 0.9 ? Color.red : Color.white.opacity(0.8))
                        .frame(width: 2)
                        .offset(x: max(0, w * CGFloat(peak) - 2))
                }
            }
        }
        .frame(height: 4)
        .clipShape(Capsule())
    }

    private var rmsColor: Color {
        if rms > 0.9 { return .red }
        if rms > 0.7 { return .orange }
        return .green
    }
}

#Preview {
    ContentView(receiver: AudioReceiver(), speakers: SpeakersController())
}
import SwiftUI

struct FestivalModeView: View {
    @ObservedObject var receiver: AudioReceiver
    @Environment(\.dismiss) private var dismiss
    @State private var pulse: CGFloat = 1.0
    @State private var hue: Double = 0.6
    @State private var level: Float = 0.0
    @State private var timer: Timer?

    private var channel: String {
        UserDefaults.standard.string(forKey: "channel") ?? "soluna"
    }

    var body: some View {
        ZStack {
            LinearGradient(
                colors: [
                    Color(hue: hue, saturation: 0.85, brightness: max(0.15, Double(level) * 0.9)),
                    Color(hue: hue + 0.15, saturation: 0.9, brightness: max(0.05, Double(level) * 0.6))
                ],
                startPoint: .topLeading,
                endPoint: .bottomTrailing
            )
            .scaleEffect(pulse)

            VStack(spacing: 24) {
                Spacer()

                Circle()
                    .fill(.white.opacity(Double(level) * 0.9 + 0.1))
                    .frame(width: 160, height: 160)
                    .scaleEffect(pulse)
                    .shadow(color: .white.opacity(0.4), radius: 30)

                Text(channel)
                    .font(.system(size: 64, weight: .black, design: .rounded))
                    .foregroundColor(.white)
                    .shadow(color: .black.opacity(0.6), radius: 10)

                if !receiver.groupMembers.isEmpty {
                    Text("\(receiver.groupMembers.count + 1) listeners")
                        .font(.title3.weight(.medium))
                        .foregroundColor(.white.opacity(0.7))
                }

                Spacer()
            }

            VStack {
                HStack {
                    Spacer()
                    Button { dismiss() } label: {
                        Image(systemName: "xmark.circle.fill")
                            .font(.system(size: 28))
                            .foregroundColor(.white.opacity(0.5))
                    }
                    .buttonStyle(.plain)
                    .padding(20)
                }
                Spacer()
            }
        }
        .onAppear {
            timer = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { _ in
                // Use spectrum bands average as energy level
                let bands = receiver.spectrumBands()
                let avg = bands.isEmpty ? Float(0) : bands.reduce(0, +) / Float(bands.count)
                Task { @MainActor in
                    level = avg
                    withAnimation(.easeOut(duration: 0.06)) {
                        pulse = 1.0 + CGFloat(avg) * 0.4
                        hue = 0.55 + Double(avg) * 0.35
                    }
                }
            }
        }
        .onDisappear {
            timer?.invalidate()
            timer = nil
        }
    }
}
