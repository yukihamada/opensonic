//
//  ContentView.swift
//  SolunaReceiverMac
//

import SwiftUI

// MARK: - Root

struct ContentView: View {
    @ObservedObject var receiver: AudioReceiver
    @ObservedObject var speakers: SpeakersController
    @StateObject private var relay    = PeerRelayManager.shared
    @State private var groupCode       = ""
    @State private var showSettings   = false
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

    var body: some View {
        ScrollView {
            VStack(spacing: 16) {
                // Device browser always visible
                DeviceBrowserView(
                    browser: deviceBrowser,
                    connectedDeviceHost: connectedDeviceHost,
                    onSelect: { connectToDevice($0) },
                    onDisconnect: { disconnectDevice() }
                )
                .padding(12)
                .background(Color.white.opacity(0.04))
                .clipShape(RoundedRectangle(cornerRadius: 16))

                if connectedDeviceHost == nil {
                    ChannelBrowserView(
                        currentChannel: channel,
                        onSelect: { switchToChannel($0) }
                    )
                    quickChannelBar
                    relayBanner
                    playSection
                    if receiver.state == .receiving {
                        statsRow
                    }
                    sendSection
                } else {
                    // Device-connected info banner
                    HStack(spacing: 10) {
                        Image(systemName: "laptopcomputer.and.iphone")
                            .foregroundColor(.solunaLive)
                        let devName = deviceBrowser.devices.first(where: { $0.host == connectedDeviceHost })?.name ?? (connectedDeviceHost ?? "Device")
                        Text("#\(devName)")
                            .font(.system(size: 14, weight: .bold, design: .monospaced))
                            .foregroundColor(.white)
                        Text("接続中 — 解除するまで同じ音声が流れます")
                            .font(.caption)
                            .foregroundColor(.white.opacity(0.5))
                        Spacer()
                    }
                    .padding(12)
                    .background(Color.solunaLive.opacity(0.08))
                    .clipShape(RoundedRectangle(cornerRadius: 12))
                }
                speakersCard
                playerSectionCollapsible
            }
            .padding(.horizontal, 16)
            .padding(.top, 8)
            .padding(.bottom, 32)
        }
        .background(LinearGradient.solunaBg)
        .preferredColorScheme(.dark)
        .navigationTitle("Soluna")
        .toolbar {
            ToolbarItem(placement: .automatic) {
                Button(action: { showDevicePicker = true }) {
                    Image(systemName: "globe")
                        .foregroundColor(.purple)
                }
                .help("グローバルデバイスを選択")
            }
            ToolbarItem(placement: .automatic) {
                Button(action: { showSettings = true }) {
                    Image(systemName: "gearshape").foregroundColor(.secondary)
                }
            }
        }
        .sheet(isPresented: $showDevicePicker) {
            GlobalDevicePickerView(registry: globalRegistry) { device in
                connectToGlobalDevice(device)
            }
        }
        .sheet(isPresented: $showSettings) { SettingsView(receiver: receiver) }
        .sheet(isPresented: $showAddSpeaker, onDismiss: { newName = ""; newHost = "" }) {
            addSpeakerSheet
        }
        .sheet(isPresented: $showSavePreset, onDismiss: { presetName = "" }) {
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
        .onAppear {
            speakers.audioReceiver = receiver
            playerModel.speakersController = speakers
            playerModel.daemon = speakers.primaryDaemon
            loadSavedSettings()
            if autoConnect { receiver.start() }
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

    // MARK: - Play Section (hero)

    private var playSection: some View {
        VStack(spacing: 16) {
            // Play/stop button with gradient ring
            Button(action: togglePlayback) {
                ZStack {
                    GradientRing(isActive: receiver.state == .receiving)
                        .frame(width: 88, height: 88)
                        .opacity(receiver.state == .receiving ? 1 : 0.3)

                    Circle()
                        .fill(
                            receiver.state == .receiving
                                ? Color.solunaLive.opacity(0.15)
                                : Color.white.opacity(0.06)
                        )
                        .frame(width: 76, height: 76)

                    if receiver.state == .connecting {
                        ProgressView()
                            .scaleEffect(1.2)
                            .tint(.white)
                    } else {
                        Image(systemName: heroIcon)
                            .font(.system(size: 28, weight: .semibold))
                            .foregroundColor(receiver.state == .receiving ? .solunaLive : .solunaGradientMid)
                    }
                }
            }
            .buttonStyle(.plain)
            .opacity(receiver.state == .connecting ? (isPulsing ? 0.5 : 1.0) : 1.0)
            .onChange(of: receiver.state) { newState in
                if newState == .connecting {
                    withAnimation(.easeInOut(duration: 1).repeatForever(autoreverses: true)) {
                        isPulsing = true
                    }
                } else {
                    withAnimation(.default) {
                        isPulsing = false
                    }
                }
            }
            .disabled(receiver.state == .connecting)

            // Status pill
            HStack(spacing: 6) {
                Circle().fill(heroAccent).frame(width: 7, height: 7)
                    .shadow(color: heroAccent.opacity(0.6), radius: 4)
                Text(receiver.state.rawValue)
                    .font(.system(size: 13, weight: .medium))
                    .foregroundColor(heroAccent)
            }
            .padding(.horizontal, 14).padding(.vertical, 6)
            .background(heroAccent.opacity(0.1))
            .clipShape(Capsule())

            // Error detail (actionable message when in error state)
            if receiver.state == .error {
                VStack(spacing: 4) {
                    if let msg = receiver.errorMessage, !msg.isEmpty {
                        Text(msg)
                            .font(.system(size: 12))
                            .foregroundColor(.red.opacity(0.9))
                            .multilineTextAlignment(.center)
                    }
                    Text("Tap the button above to retry")
                        .font(.system(size: 11))
                        .foregroundColor(.secondary)
                }
                .padding(.horizontal, 16)
                .transition(.opacity)
            }

            // Spectrum (only when receiving)
            if receiver.state == .receiving {
                SpectrumView(receiver: receiver)
                    .transition(.move(edge: .top).combined(with: .opacity))
            }

            // Volume
            HStack(spacing: 8) {
                Image(systemName: "speaker.fill").font(.system(size: 12)).foregroundColor(.white.opacity(0.4))
                Slider(value: Binding(
                    get: { masterVolume },
                    set: { v in
                        masterVolume = v
                        speakers.setAllVolume(v)
                    }
                ), in: 0...1)
                .tint(.solunaGradientMid)
                Image(systemName: "speaker.wave.3.fill").font(.system(size: 12)).foregroundColor(.white.opacity(0.4))
            }
            .padding(.horizontal, 8)

            // Remote Volume Control (group members)
            if receiver.state == .receiving && !receiver.groupMembers.isEmpty {
                RemoteVolumeSection(receiver: receiver)
            }

            // Talk Mode toggle
            if receiver.state == .receiving {
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
                    .padding(.horizontal, 12).padding(.vertical, 6)
                    .background(talkMode ? Color.solunaGradientStart.opacity(0.1) : Color.clear)
                    .cornerRadius(8)
                }
                .buttonStyle(.plain)
            }
        }
        .padding(.vertical, 20)
        .frame(maxWidth: .infinity)
        .glassCard()
    }

    // MARK: - Send Section (collapsible)

    private var anySending: Bool {
        receiver.isMicTransmitting || receiver.isShmTransmitting
    }

    private var sendSection: some View {
        VStack(spacing: 0) {
            // Header (tap to expand)
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
                    // Mic + System Audio toggles
                    HStack(spacing: 10) {
                        sendToggle(title: "Mic", icon: "mic.fill",
                                   active: receiver.isMicTransmitting, color: .red,
                                   action: { receiver.toggleMic() })
                        sendToggle(title: "System Audio", icon: "speaker.wave.2.fill",
                                   active: receiver.isShmTransmitting, color: .orange,
                                   action: { receiver.toggleShmTransmit() })
                    }

                    // Level meters
                    if receiver.isMicTransmitting {
                        MicLevelMeter(level: receiver.micInputLevel).frame(height: 6)
                    }
                    if receiver.isShmTransmitting {
                        MicLevelMeter(level: receiver.shmTxLevel).frame(height: 6).tint(.orange)
                    }

                    // Sync/Jam toggle (only when sending)
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

    @State private var playerExpanded = false

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

    // MARK: - Quick Channel Bar

    private var recentChannels: [String] {
        recentChannelsData
            .components(separatedBy: ",")
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
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
        // Direct channel switch: connectRelay auto-disconnects old channel
        if receiver.state != .receiving { receiver.start() }
        receiver.connectRelay(group: trimmed)
    }

    private var quickChannelBar: some View {
        VStack(spacing: 8) {
            HStack(spacing: 8) {
                Image(systemName: "dot.radiowaves.left.and.right")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundColor(.solunaGradientStart)

                if isEditingChannel {
                    TextField("Channel", text: $editedChannel, onCommit: {
                        switchToChannel(editedChannel)
                        isEditingChannel = false
                    })
                    .textFieldStyle(.roundedBorder)
                    .font(.system(size: 12, design: .monospaced))
                    .frame(maxWidth: 140)

                    Button {
                        switchToChannel(editedChannel)
                        isEditingChannel = false
                    } label: {
                        Image(systemName: "checkmark.circle.fill")
                            .foregroundColor(.solunaLive)
                    }
                    .buttonStyle(.plain)

                    Button {
                        isEditingChannel = false
                    } label: {
                        Image(systemName: "xmark.circle.fill")
                            .foregroundColor(.secondary)
                    }
                    .buttonStyle(.plain)
                } else {
                    Text(channel)
                        .font(.system(size: 12, weight: .semibold, design: .monospaced))
                        .foregroundColor(.solunaGradientStart)
                        .padding(.horizontal, 10)
                        .padding(.vertical, 4)
                        .background(Color.solunaGradientStart.opacity(0.1))
                        .clipShape(Capsule())

                    Button {
                        editedChannel = channel
                        isEditingChannel = true
                    } label: {
                        Image(systemName: "pencil")
                            .font(.system(size: 11))
                            .foregroundColor(.secondary)
                    }
                    .buttonStyle(.plain)
                    .help("Edit channel name")
                }

                Spacer()

                Button {
                    editedChannel = ""
                    isEditingChannel = true
                } label: {
                    HStack(spacing: 3) {
                        Image(systemName: "arrow.triangle.2.circlepath")
                            .font(.system(size: 10))
                        Text("Switch")
                            .font(.caption2.weight(.medium))
                    }
                    .foregroundColor(.solunaGradientStart)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 4)
                    .background(Color.solunaGradientStart.opacity(0.08))
                    .clipShape(Capsule())
                }
                .buttonStyle(.plain)
            }

            // Recent channels chips
            if !recentChannels.isEmpty && !isEditingChannel {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 6) {
                        Text("Recent:")
                            .font(.system(size: 10))
                            .foregroundColor(.secondary)
                        ForEach(recentChannels, id: \.self) { ch in
                            Button {
                                switchToChannel(ch)
                            } label: {
                                Text(ch)
                                    .font(.system(size: 10, weight: ch == channel ? .bold : .regular, design: .monospaced))
                                    .foregroundColor(ch == channel ? .purple : .secondary)
                                    .padding(.horizontal, 8)
                                    .padding(.vertical, 3)
                                    .background(ch == channel ? Color.solunaGradientStart.opacity(0.12) : Color(nsColor: .tertiaryLabelColor).opacity(0.15))
                                    .clipShape(Capsule())
                            }
                            .buttonStyle(.plain)
                        }
                    }
                }
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .glassCard()
        .clipShape(RoundedRectangle(cornerRadius: 12))
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
            if receiver.isSyncMode {
                StatPill(value: "\(receiver.syncDelayMs)ms", label: "sync", color: .blue)
            }
            Spacer()
            Button {
                if receiver.isRecording {
                    receiver.stopRecording()
                } else {
                    receiver.startRecording()
                }
            } label: {
                Image(systemName: receiver.isRecording ? "record.circle.fill" : "record.circle")
                    .font(.system(size: 18))
                    .foregroundColor(receiver.isRecording ? .red : .secondary)
            }
            .buttonStyle(.plain)
            .help(receiver.isRecording ? "Stop Recording" : "Record to WAV")
        }
        .frame(maxWidth: .infinity, alignment: .leading)
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

            // Routing presets
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

            // Local Mac (primary output)
            LocalSpeakerRow(receiver: receiver)

            // Local devices (BT / AirPlay / USB)
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

            // Remote speakers
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

            // Speaker groups
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
        .sheet(isPresented: $showCreateGroup) {
            createGroupSheet
        }
    }

    @State private var selectedGroupDevices: Set<UInt32> = []

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

    // MARK: - Add speaker sheet

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
                // WAN Channel tab — connect by channel name only
                VStack(spacing: 12) {
                    Image(systemName: "globe")
                        .font(.system(size: 32))
                        .foregroundColor(.solunaGradientStart)
                    Text("チャンネル名を入力するだけで\nWANリレー経由で接続できます")
                        .font(.caption)
                        .foregroundColor(.secondary)
                        .multilineTextAlignment(.center)
                    TextField("チャンネル名 (例: ambient-tokyo)", text: $groupCode)
                        .textFieldStyle(.roundedBorder)
                        .padding(.horizontal, 16)
                    Button("接続") {
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
                // Local devices tab
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
                // Network tab (existing)
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
        if ch >= 1 { receiver.channels = UInt32(ch) }
    }

    private func channelLayoutLabel(_ ch: Int) -> String {
        switch ch {
        case 1:  return "Mono"
        case 2:  return "Stereo"
        case 6:  return "5.1 (FL FR C LFE SL SR)"
        case 8:  return "7.1 (FL FR C LFE SL SR BL BR)"
        default: return "\(ch)ch"
        }
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
