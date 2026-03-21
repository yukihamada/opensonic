//
//  ContentView.swift
//  Soluna — Spotify-style player with Listen / Mic / Profile tabs

import SwiftUI
import MultipeerConnectivity
import UniformTypeIdentifiers
import MediaPlayer

enum SolunaTab: String, CaseIterable {
    case listen, mic, profile
    var icon: String {
        switch self { case .listen: return "music.note"; case .mic: return "mic"; case .profile: return "person.fill" }
    }
    var label: String {
        switch self { case .listen: return "Listen"; case .mic: return "Mic"; case .profile: return "Profile" }
    }
}

struct ContentView: View {
    @EnvironmentObject var deepLink: DeepLinkManager
    @StateObject private var receiver = AudioReceiver()
    @StateObject private var speakers = SpeakersController()
    @StateObject private var playerModel = PlayerModel()
    @State private var showSettings = false
    @State private var showDebug = false
    @State private var showAddSpeaker = false
    @State private var newName = ""
    @State private var newHost = ""
    @State private var masterVolume: Float = 1.0
    @State private var masterMuted = false
    @State private var masterDelayMs: Int = 40
    @AppStorage("streamMode") private var streamMode = "sync"
    @AppStorage("connectMode") private var connectMode = true
    @State private var groupCode = ""
    @State private var showQR = false
    @State private var pttPressed = false
    @State private var showPlayer = false
    @State private var showDJPicker = false
    @State private var showDJDeckView = false
    @State private var talkMode = false
    @State private var quickChannelInput = ""
    @State private var showChannelCreate = false
    @State private var connectedDeviceHost: String? = nil
    @AppStorage("recentChannels") private var recentChannelsJSON: String = "[]"
    @StateObject private var channelStore = ChannelStore()
    @StateObject private var deviceBrowser = DeviceBrowser()
    @StateObject private var globalRegistry = GlobalDeviceRegistry()
    @State private var showDevicePicker = false
    @State private var showFestivalMode = false
    @StateObject private var auth = AuthManager.shared
    @State private var showLogin = false
    @State private var showFanRank = false
    @State private var showSubscription = false
    @ObservedObject private var fanRankManager = FanRankManager.shared
    @State private var lastListenRecordTime: Date = .distantPast
    @State private var selectedTab: SolunaTab = .listen
    @State private var micChannel: String = ""
    @State private var micMode: Int = 0  // 0=Local(LAN), 1=Global(Relay), 2=Karaoke(ローカルミックス)

    private var recentChannels: [String] {
        (try? JSONDecoder().decode([String].self, from: Data(recentChannelsJSON.utf8))) ?? []
    }
    private var currentChannelName: String { UserDefaults.standard.string(forKey: "channel") ?? "soluna" }
    private var isPlaying: Bool { receiver.state == .receiving }
    private var isMicActive: Bool { receiver.isMicTransmitting || receiver.isMicMonitoring }

    // MARK: - Actions

    private func connectToDevice(_ d: SolunaLocalDevice) {
        connectedDeviceHost = d.host; receiver.disconnectRelay()
        if receiver.state == .stopped || receiver.state == .error { receiver.start() }
    }
    private func disconnectDevice() {
        connectedDeviceHost = nil
        receiver.connectRelay(group: UserDefaults.standard.string(forKey: "channel") ?? "soluna")
    }
    private func connectToGlobalDevice(_ d: GlobalDevice) {
        connectedDeviceHost = d.relayHost
        if receiver.state == .stopped || receiver.state == .error { receiver.start() }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
            receiver.connectRelay(group: d.group, host: d.relayHost, port: d.relayPort)
        }
    }
    private func deleteRecentChannel(_ name: String) {
        let recent = recentChannels.filter { $0 != name }
        recentChannelsJSON = (try? String(data: JSONEncoder().encode(recent), encoding: .utf8)) ?? "[]"
    }
    private func switchChannel(_ name: String) {
        let ch = name.trimmingCharacters(in: .whitespaces); guard !ch.isEmpty else { return }
        UserDefaults.standard.set(ch, forKey: "channel"); groupCode = ch
        var recent = recentChannels.filter { $0 != ch }; recent.insert(ch, at: 0)
        if recent.count > 5 { recent = Array(recent.prefix(5)) }
        recentChannelsJSON = (try? String(data: JSONEncoder().encode(recent), encoding: .utf8)) ?? "[]"
        if receiver.state == .stopped || receiver.state == .error { receiver.start() }
        else { receiver.connectRelay(group: ch) }
        updateNowPlaying()
    }
    private func togglePlayback() {
        if receiver.state == .receiving { receiver.stop() } else { receiver.start() }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { updateNowPlaying() }
    }
    private func toggleMic() {
        if receiver.state == .stopped || receiver.state == .error { receiver.start() }
        let ch = micChannel.trimmingCharacters(in: .whitespaces)
        if !ch.isEmpty && ch != currentChannelName { switchChannel(ch) }
        if micMode == 2 {
            // Karaoke mode: local mic monitoring only (no relay send)
            receiver.micGlobal = false
            receiver.micGlobal = false
            receiver.toggleMicMonitor()
        } else {
            receiver.micGlobal = (micMode == 1)
            receiver.toggleMic()
        }
    }
    private func loadSavedSettings() {
        let d = UserDefaults.standard
        if let g = d.string(forKey: "multicastGroup"), !g.isEmpty { receiver.multicastGroup = g }
        let port = d.integer(forKey: "port"); if port > 0 { receiver.port = UInt16(port) }
        let ch = d.integer(forKey: "channels"); if ch >= 1 { receiver.channels = UInt32(ch) }
    }

    // MARK: - Now Playing & Media Keys

    private func setupNowPlaying() {
        let center = MPRemoteCommandCenter.shared()
        center.playCommand.addTarget { _ in
            if receiver.state != .receiving { receiver.start() }
            return .success
        }
        center.pauseCommand.addTarget { _ in
            if receiver.state == .receiving { receiver.stop() }
            return .success
        }
        center.togglePlayPauseCommand.addTarget { _ in
            togglePlayback()
            return .success
        }
        // Next/previous channel via media keys
        let channelIds = ["soluna", "jazz", "lofi", "chill", "dance", "bjj", "yuki"]
        center.nextTrackCommand.isEnabled = true
        center.nextTrackCommand.addTarget { _ in
            if let idx = channelIds.firstIndex(of: currentChannelName) {
                switchChannel(channelIds[(idx + 1) % channelIds.count])
            }
            return .success
        }
        center.previousTrackCommand.isEnabled = true
        center.previousTrackCommand.addTarget { _ in
            if let idx = channelIds.firstIndex(of: currentChannelName) {
                switchChannel(channelIds[(idx - 1 + channelIds.count) % channelIds.count])
            }
            return .success
        }
        center.changePlaybackPositionCommand.isEnabled = false
        updateNowPlaying()
    }

    private func updateNowPlaying() {
        let channelLabels: [String: String] = [
            "soluna": "Soluna", "jazz": "Jazz", "lofi": "Lo-Fi",
            "chill": "Chill", "dance": "Dance", "bjj": "BJJ", "yuki": "Yuki"
        ]
        var info = [String: Any]()
        info[MPMediaItemPropertyTitle] = "Soluna Radio"
        info[MPMediaItemPropertyArtist] = channelLabels[currentChannelName] ?? currentChannelName
        info[MPNowPlayingInfoPropertyPlaybackRate] = isPlaying ? 1.0 : 0.0
        MPNowPlayingInfoCenter.default().nowPlayingInfo = info
        MPNowPlayingInfoCenter.default().playbackState = isPlaying ? .playing : .paused
    }

    // MARK: - Body

    var body: some View {
        ZStack {
            LinearGradient.solunaBg.ignoresSafeArea()
            VStack(spacing: 0) {
                ScrollView(showsIndicators: false) {
                    VStack(spacing: 0) {
                        headerBar.padding(.horizontal, 16).padding(.top, 4).padding(.bottom, 6)
                        switch selectedTab {
                        case .listen: listenTab
                        case .mic: micTab
                        case .profile: profileTab
                        }
                    }
                }
                tabBar
            }
        }
        .preferredColorScheme(.dark)
        .sheet(isPresented: $showDevicePicker) { DevicePickerView(registry: globalRegistry) { connectToGlobalDevice($0) } }
        .sheet(isPresented: $showLogin) { EmailLoginView(auth: auth) }
        .fullScreenCover(isPresented: $showFestivalMode) { FestivalModeView(receiver: receiver) }
        .sheet(isPresented: $showDebug) { DebugOverlayView(receiver: receiver) }
        .sheet(isPresented: $showSettings) { SettingsView(receiver: receiver) }
        .sheet(isPresented: $showFanRank) { NavigationStack { FanRankView() } }
        .sheet(isPresented: $showSubscription) { SubscriptionView(store: channelStore) }
        .sheet(isPresented: $showQR) { ChannelQRView(channel: currentChannelName) }
        .sheet(isPresented: $showAddSpeaker, onDismiss: { newName = ""; newHost = "" }) { addSpeakerSheet }
        .sheet(isPresented: $showPlayer) { PlayerView(model: playerModel) }
        .sheet(isPresented: $showChannelCreate) {
            ChannelPurchaseView(store: channelStore,
                                activeChannel: Binding(get: { currentChannelName }, set: { switchChannel($0) }))
        }
        .sheet(isPresented: $showDJDeckView) { DJDeckView(receiver: receiver) }
        .onAppear {
            speakers.audioReceiver = receiver; playerModel.speakersController = speakers
            loadSavedSettings(); micChannel = currentChannelName

            // Restore saved volume
            let savedVol = UserDefaults.standard.float(forKey: "soluna_volume")
            if savedVol > 0 { receiver.volume = savedVol }

            if connectMode { receiver.autoStart() }
            receiver.daemonClient = speakers.primaryDaemon

            // Now Playing + media keys (AirPods, lock screen, Control Center)
            setupNowPlaying()
            Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { _ in
                Task { @MainActor in
                    speakers.applyServerRxDelay()
                    if !(playerModel.daemon?.isConnected ?? false) { playerModel.rebindIfNeeded(speakers.primaryDaemon) }
                    receiver.daemonClient = speakers.primaryDaemon
                    if receiver.state == .receiving, Date().timeIntervalSince(lastListenRecordTime) >= 30 {
                        lastListenRecordTime = Date()
                        FanRankManager.shared.recordListen(channel: UserDefaults.standard.string(forKey: "channel") ?? "soluna")
                    }
                }
            }
            playerModel.daemon = speakers.primaryDaemon
        }
        .onChange(of: speakers.speakers.count) { _ in playerModel.rebindIfNeeded(speakers.primaryDaemon) }
        .onChange(of: deepLink.pendingChannel) { ch in
            guard let ch, !ch.isEmpty else { return }; deepLink.pendingChannel = nil; switchChannel(ch)
        }
        .animation(.spring(response: 0.35, dampingFraction: 0.8), value: receiver.state.rawValue)
    }

    // MARK: - Header

    private var headerBar: some View {
        HStack(spacing: 12) {
            Text("SOLUNA").font(.system(size: 17, weight: .bold, design: .rounded))
                .foregroundStyle(LinearGradient.solLunaGradient)
                .onTapGesture(count: 3) { showDebug = true }
            Spacer()
            headerBtn("globe", .solunaLuna) { showDevicePicker = true }
            headerBtn("qrcode", .white.opacity(0.6)) { showQR = true }
            headerBtn("gearshape", .white.opacity(0.6)) { showSettings = true }
        }
    }
    private func headerBtn(_ icon: String, _ color: Color, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: icon).font(.system(size: 12, weight: .medium)).foregroundColor(color)
                .frame(width: 28, height: 28).background(Color.white.opacity(0.08)).clipShape(Circle())
        }
    }

    // MARK: - Tab Bar

    private var tabBar: some View {
        HStack(spacing: 0) {
            ForEach(SolunaTab.allCases, id: \.self) { tab in
                Button {
                    withAnimation(.spring(response: 0.3, dampingFraction: 0.8)) { selectedTab = tab }
                } label: {
                    VStack(spacing: 4) {
                        Image(systemName: selectedTab == tab && tab == .mic ? "mic.fill" : tab.icon)
                            .font(.system(size: 17, weight: selectedTab == tab ? .semibold : .regular))
                            .foregroundColor(selectedTab == tab ? .white : .white.opacity(0.35))
                            .frame(height: 20)
                        Text(tab.label).font(.system(size: 9, weight: selectedTab == tab ? .bold : .medium))
                            .foregroundColor(selectedTab == tab ? .white : .white.opacity(0.35))
                    }.frame(maxWidth: .infinity).padding(.top, 4).padding(.bottom, 4)
                }
            }
        }
        .padding(.bottom, 6)
        .background(Rectangle().fill(.ultraThinMaterial)
            .overlay(Rectangle().fill(Color.white.opacity(0.04)))
            .ignoresSafeArea(edges: .bottom))
    }

    // MARK: - Listen Tab

    private var listenTab: some View {
        VStack(spacing: 0) {
            channelGrid.padding(.horizontal, 16).padding(.bottom, 8)
            nowPlayingArea.padding(.horizontal, 16).padding(.bottom, 8)
            volumeControl.padding(.horizontal, 16).padding(.bottom, 6)
            quickActions.padding(.horizontal, 16).padding(.bottom, 6)
            deviceBanner.padding(.horizontal, 16).padding(.bottom, 40)
        }
    }

    // MARK: - Channel Grid

    private var channelGrid: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Channels").font(.system(size: 15, weight: .bold)).foregroundColor(.white)
                Spacer()
                Button { showChannelCreate = true } label: {
                    HStack(spacing: 4) {
                        Image(systemName: "plus").font(.system(size: 11, weight: .bold))
                        Text("Create").font(.system(size: 12, weight: .medium))
                    }.foregroundColor(.solunaLuna).padding(.horizontal, 10).padding(.vertical, 5)
                        .background(Color.solunaLuna.opacity(0.15)).clipShape(Capsule())
                }
            }
            LazyVGrid(columns: [GridItem(.flexible(), spacing: 10), GridItem(.flexible(), spacing: 10)], spacing: 8) {
                ForEach(presetChannels) { ch in
                    let active = currentChannelName == ch.id
                    Button { switchChannel(ch.id) } label: {
                        VStack(alignment: .leading, spacing: 4) {
                            HStack {
                                Image(systemName: ch.icon).font(.system(size: 14))
                                    .foregroundColor(active ? .white : ch.color)
                                Spacer()
                                if active && isPlaying {
                                    Image(systemName: "antenna.radiowaves.left.and.right")
                                        .font(.caption2).foregroundColor(.white.opacity(0.8))
                                }
                            }
                            Text(ch.label).font(.system(size: 12, weight: .bold))
                                .foregroundColor(active ? .white : .white.opacity(0.9))
                            Text(ch.description).font(.system(size: 9))
                                .foregroundColor(active ? .white.opacity(0.7) : .white.opacity(0.4)).lineLimit(1)
                        }
                        .frame(maxWidth: .infinity, alignment: .leading).padding(.horizontal, 10).padding(.vertical, 7)
                        .background(RoundedRectangle(cornerRadius: 12)
                            .fill(active ? AnyShapeStyle(ch.color.gradient) : AnyShapeStyle(Color.white.opacity(0.06))))
                        .overlay(RoundedRectangle(cornerRadius: 12)
                            .strokeBorder(active ? Color.white.opacity(0.2) : Color.white.opacity(0.06), lineWidth: 0.5))
                        .shadow(color: active ? ch.color.opacity(0.3) : .clear, radius: 8, y: 4)
                    }.buttonStyle(.plain)
                }
            }
            let custom = recentChannels.filter { ch in !presetChannels.contains(where: { $0.id == ch }) }
            if !custom.isEmpty {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 8) {
                        ForEach(custom, id: \.self) { ch in
                            Button { switchChannel(ch) } label: {
                                Text(ch).font(.system(size: 12, weight: ch == currentChannelName ? .bold : .medium, design: .monospaced))
                                    .foregroundColor(ch == currentChannelName ? .white : .white.opacity(0.5))
                                    .padding(.horizontal, 12).padding(.vertical, 6)
                                    .background(ch == currentChannelName ? Color.solunaGradientMid : Color.white.opacity(0.06))
                                    .clipShape(Capsule())
                            }.contextMenu {
                                Button(role: .destructive) { deleteRecentChannel(ch) } label: { Label("Remove", systemImage: "trash") }
                            }
                        }
                    }
                }
            }
        }
    }

    // MARK: - Now Playing

    private var nowPlayingArea: some View {
        VStack(spacing: 16) {
            ZStack {
                RoundedRectangle(cornerRadius: 20)
                    .fill(isPlaying ? AnyShapeStyle(LinearGradient.solLunaGradient) : AnyShapeStyle(Color.white.opacity(0.04)))
                    .frame(height: 200)
                if !isPlaying {
                    VStack(spacing: 8) {
                        ZStack {
                            Circle().fill(RadialGradient(colors: [.solunaGradientStart.opacity(0.3), .solunaGradientEnd.opacity(0.1), .clear],
                                                         center: .center, startRadius: 10, endRadius: 50))
                                .frame(width: 80, height: 80)
                            Image(systemName: "waveform").font(.system(size: 32)).foregroundColor(.white.opacity(0.2))
                        }
                        Text("Tap a channel to start").font(.subheadline).foregroundColor(.white.opacity(0.25))
                    }
                }
                if isPlaying {
                    HStack(spacing: 16) {
                        ZStack {
                            Circle().fill(RadialGradient(colors: [.solunaSol.opacity(0.6), .solunaLuna.opacity(0.3), .clear],
                                                         center: .center, startRadius: 5, endRadius: 40))
                                .frame(width: 72, height: 72)
                            if let url = receiver.nowPlayingArtwork {
                                AsyncImage(url: url) { $0.resizable().scaledToFill() } placeholder: {
                                    Image(systemName: "music.note").font(.system(size: 24)).foregroundColor(.white.opacity(0.5))
                                }.frame(width: 60, height: 60).clipShape(Circle())
                            } else {
                                Image(systemName: "music.note").font(.system(size: 24)).foregroundColor(.white.opacity(0.6))
                            }
                        }.shadow(color: .black.opacity(0.3), radius: 8)
                        WaveformVisualizer(level: receiver.outputLevel).frame(height: 100).opacity(0.8)
                    }.padding(.horizontal, 20)
                }
            }.clipShape(RoundedRectangle(cornerRadius: 20))

            VStack(spacing: 4) {
                Text(receiver.nowPlayingTitle ?? currentChannelName.capitalized)
                    .font(.system(size: 22, weight: .bold)).foregroundColor(.white).lineLimit(1)
                Text(receiver.nowPlayingArtist ?? (isPlaying ? "Now streaming" : "Ready to play"))
                    .font(.system(size: 15)).foregroundColor(.white.opacity(0.5)).lineLimit(1)
            }
            HStack(spacing: 32) {
                Button {} label: { Image(systemName: "backward.fill").font(.system(size: 20)).foregroundColor(.white.opacity(0.4)) }
                Button(action: togglePlayback) {
                    ZStack {
                        if receiver.state == .connecting {
                            ProgressView().tint(.white).scaleEffect(1.2).frame(width: 72, height: 72)
                        } else {
                            Circle().fill(isPlaying ? LinearGradient.solGradient : LinearGradient.lunaGradient)
                                .frame(width: 72, height: 72)
                                .shadow(color: isPlaying ? .solunaSol.opacity(0.4) : .solunaLuna.opacity(0.4), radius: 12, y: 4)
                            Image(systemName: isPlaying ? "pause.fill" : "play.fill")
                                .font(.system(size: 28, weight: .bold)).foregroundColor(.white)
                                .offset(x: isPlaying ? 0 : 2)
                        }
                    }
                }.buttonStyle(.plain)
                Button {} label: { Image(systemName: "forward.fill").font(.system(size: 20)).foregroundColor(.white.opacity(0.4)) }
            }
            if isPlaying {
                HStack(spacing: 8) {
                    Circle().fill(Color.solunaLive).frame(width: 6, height: 6).shadow(color: .solunaLive.opacity(0.6), radius: 4)
                    Text("Listening").font(.system(size: 13, weight: .medium)).foregroundColor(.solunaLive)
                    Text("--").foregroundColor(.white.opacity(0.3))
                    Text(String(format: "%.0fms", receiver.networkLatencyMs))
                        .font(.system(size: 12, weight: .medium, design: .monospaced)).foregroundColor(.white.opacity(0.4))
                }
                NowPlayingView(channel: currentChannelName, isReceiving: true)
            }
        }.padding(16).glassCard()
    }

    // MARK: - Volume

    private var volumeControl: some View {
        HStack(spacing: 10) {
            Button { masterMuted.toggle(); receiver.isMuted = masterMuted } label: {
                Image(systemName: masterMuted ? "speaker.slash.fill" : "speaker.fill")
                    .font(.system(size: 14)).foregroundColor(masterMuted ? .solunaMic : .white.opacity(0.5))
                    .frame(width: 28, height: 28)
            }
            Slider(value: $masterVolume, in: 0...1) { _ in
                if masterMuted { masterMuted = false; receiver.isMuted = false }
                receiver.volume = masterVolume
                UserDefaults.standard.set(masterVolume, forKey: "soluna_volume")
            }.tint(LinearGradient.solLunaGradient)
            Image(systemName: "speaker.wave.3.fill").font(.system(size: 12)).foregroundColor(.white.opacity(0.4))
        }.padding(.horizontal, 16).padding(.vertical, 10).glassCard()
    }

    // MARK: - Quick Actions

    private var quickActions: some View {
        HStack(spacing: 10) {
            Button { showFestivalMode = true } label: {
                Image(systemName: "sparkles").font(.system(size: 14)).foregroundColor(.solunaSol)
                    .frame(width: 36, height: 36).background(Color.solunaSol.opacity(0.12)).clipShape(Circle())
            }
            Button { showPlayer = true } label: {
                Image(systemName: "music.note.list").font(.system(size: 14)).foregroundColor(.solunaLuna)
                    .frame(width: 36, height: 36).background(Color.solunaLuna.opacity(0.12)).clipShape(Circle())
            }
            Spacer()
            if channelStore.currentPlan == .free {
                Button { showSubscription = true } label: {
                    Text("PRO").font(.system(size: 11, weight: .bold)).foregroundColor(.white)
                        .padding(.horizontal, 10).padding(.vertical, 6)
                        .background(LinearGradient.solGradient).clipShape(Capsule())
                }
            }
        }
    }

    @ViewBuilder private var deviceBanner: some View {
        if let host = connectedDeviceHost {
            let name = deviceBrowser.devices.first(where: { $0.host == host })?.name ?? host
            HStack(spacing: 10) {
                Image(systemName: "laptopcomputer.and.iphone").foregroundColor(.solunaLive)
                VStack(alignment: .leading, spacing: 2) {
                    Text(name).font(.system(size: 14, weight: .bold)).foregroundColor(.white)
                    Text("Direct connection").font(.caption2).foregroundColor(.white.opacity(0.4))
                }
                Spacer()
                Button { disconnectDevice() } label: {
                    Text("Disconnect").font(.caption.bold()).foregroundColor(.solunaMic)
                        .padding(.horizontal, 10).padding(.vertical, 5)
                        .background(Color.solunaMic.opacity(0.15)).clipShape(Capsule())
                }
            }.padding(12).glassCard()
        }
    }

    // MARK: - Mic Tab

    private var micTab: some View {
        VStack(spacing: 24) {
            Spacer().frame(height: 40)
            Text("Broadcast").font(.system(size: 28, weight: .bold)).foregroundColor(.white)
            Text("Share your voice on a channel").font(.subheadline).foregroundColor(.white.opacity(0.4))
            Spacer().frame(height: 16)
            Button(action: toggleMic) {
                ZStack {
                    Circle().fill(isMicActive ? Color.solunaMic.opacity(0.2) : Color.white.opacity(0.06))
                        .frame(width: 140, height: 140)
                    Circle().fill(isMicActive ? Color.solunaMic : Color.white.opacity(0.1))
                        .frame(width: 100, height: 100)
                        .shadow(color: isMicActive ? .solunaMic.opacity(0.5) : .clear, radius: 20, y: 4)
                    Image(systemName: isMicActive ? "mic.fill" : "mic")
                        .font(.system(size: 42, weight: .medium))
                        .foregroundColor(isMicActive ? .white : .white.opacity(0.6))
                }
            }.buttonStyle(.plain)
            Text(isMicActive ? (micMode == 2 ? "KARAOKE" : "LIVE") : "Tap to start")
                .font(.system(size: 15, weight: isMicActive ? .bold : .medium))
                .foregroundColor(isMicActive ? .solunaMic : .white.opacity(0.4))
            if isMicActive {
                HStack(spacing: 3) {
                    ForEach(0..<16, id: \.self) { i in
                        RoundedRectangle(cornerRadius: 2)
                            .fill(CGFloat(i) / 16.0 < CGFloat(receiver.micInputLevel) ? Color.solunaMic : Color.white.opacity(0.1))
                            .frame(width: 8, height: 20)
                    }
                }.padding(.horizontal, 40)
            }
            Spacer().frame(height: 16)
            VStack(spacing: 8) {
                Text("Channel").font(.system(size: 13, weight: .medium)).foregroundColor(.white.opacity(0.4))
                TextField("Channel name", text: $micChannel)
                    .font(.system(size: 16, weight: .medium, design: .monospaced)).multilineTextAlignment(.center)
                    .padding(.horizontal, 16).padding(.vertical, 10)
                    .background(Color.white.opacity(0.06)).clipShape(RoundedRectangle(cornerRadius: 10))
                    .frame(maxWidth: 220)
            }

            // Mic mode selector
            Picker("Mode", selection: $micMode) {
                Label("LAN", systemImage: "wifi").tag(0)
                Label("Global", systemImage: "globe").tag(1)
                Label("Karaoke", systemImage: "music.mic").tag(2)
            }
            .pickerStyle(.segmented)
            .frame(maxWidth: 280)

            Text(micMode == 0 ? "Same WiFi only"
                 : micMode == 1 ? "Via relay server"
                 : "Sing along — local mix")
                .font(.system(size: 12))
                .foregroundColor(.white.opacity(0.4))

            Spacer().frame(height: 40)
        }.padding(.horizontal, 16)
    }

    // MARK: - Profile Tab

    private var profileTab: some View {
        VStack(spacing: 20) {
            Spacer().frame(height: 20)
            VStack(spacing: 12) {
                Text(fanRankManager.currentRank.icon).font(.system(size: 48))
                Text(fanRankManager.currentRank.name).font(.system(size: 22, weight: .bold)).foregroundColor(.white)
                Text("Fan Rank").font(.subheadline).foregroundColor(.white.opacity(0.4))
                Button { showFanRank = true } label: {
                    Text("View Details").font(.system(size: 14, weight: .semibold)).foregroundColor(.solunaLuna)
                        .padding(.horizontal, 20).padding(.vertical, 10)
                        .background(Color.solunaLuna.opacity(0.15)).clipShape(Capsule())
                }
            }.frame(maxWidth: .infinity).padding(24).glassCard()
            VStack(spacing: 12) {
                HStack {
                    Image(systemName: "crown.fill").font(.system(size: 18)).foregroundColor(.solunaSol)
                    Text("Subscription").font(.system(size: 17, weight: .bold)).foregroundColor(.white)
                    Spacer()
                    Text(channelStore.currentPlan == .free ? "Free" : "Pro")
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundColor(channelStore.currentPlan == .free ? .white.opacity(0.5) : .solunaSol)
                        .padding(.horizontal, 10).padding(.vertical, 4)
                        .background(channelStore.currentPlan == .free ? Color.white.opacity(0.08) : Color.solunaSol.opacity(0.15))
                        .clipShape(Capsule())
                }
                if channelStore.currentPlan == .free {
                    Button { showSubscription = true } label: {
                        Text("Upgrade to Pro").font(.system(size: 14, weight: .semibold)).foregroundColor(.white)
                            .frame(maxWidth: .infinity).padding(.vertical, 12)
                            .background(LinearGradient.solGradient).clipShape(RoundedRectangle(cornerRadius: 12))
                    }
                }
            }.padding(16).glassCard()
            VStack(spacing: 0) {
                profileRow("person.circle", "Account") { if !auth.isAuthenticated { showLogin = true } }
                Divider().background(Color.white.opacity(0.06))
                profileRow("gearshape", "Settings") { showSettings = true }
            }.glassCard(cornerRadius: 16)
            Spacer().frame(height: 40)
        }.padding(.horizontal, 16)
    }

    private func profileRow(_ icon: String, _ label: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 12) {
                Image(systemName: icon).font(.system(size: 16)).foregroundColor(.white.opacity(0.5)).frame(width: 28)
                Text(label).font(.system(size: 15, weight: .medium)).foregroundColor(.white)
                Spacer()
                Image(systemName: "chevron.right").font(.system(size: 12, weight: .semibold)).foregroundColor(.white.opacity(0.25))
            }.padding(.horizontal, 16).padding(.vertical, 14)
        }
    }

    private var addSpeakerSheet: some View {
        NavigationView {
            Form {
                Section(header: Text("Connection")) {
                    TextField("Name (e.g. Mac, Living Room)", text: $newName)
                    TextField("IP Address / Host", text: $newHost).keyboardType(.URL)
                        .autocorrectionDisabled().autocapitalization(.none)
                }
            }
            .navigationTitle("Add Speaker").navigationBarTitleDisplayMode(.inline)
            .navigationBarItems(
                leading: Button("Cancel") { showAddSpeaker = false },
                trailing: Button("Add") { speakers.add(name: newName, host: newHost); showAddSpeaker = false }.disabled(newHost.isEmpty))
        }
    }
}

#Preview { ContentView() }
