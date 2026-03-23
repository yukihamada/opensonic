//
//  ContentView.swift
//  Soluna — Spotify-style player with Listen / Mic / Profile tabs

import SwiftUI
import MultipeerConnectivity
import UniformTypeIdentifiers
import MediaPlayer
import ActivityKit
import GroupActivities

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
    @State private var showSilentDisco = false
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
    @ObservedObject private var sharePlayManager = SharePlayManager.shared
    @StateObject private var socialListening = SocialListeningManager.shared
    @StateObject private var aiAutoChannel = AIAutoChannel.shared
    @StateObject private var sleepTimer = SleepTimerManager.shared
    @StateObject private var alarmManager = SolunaAlarmManager.shared
    @State private var showSleepPicker = false
    @State private var showChat = false
    @StateObject private var chatManager = ChatManager.shared
    @State private var showTipping = false
    @State private var showBroadcast = false
    @StateObject private var tipManager = TipManager.shared
    @StateObject private var reactionManager = ReactionManager.shared
    @State private var showListeningReport = false
    @State private var showQuickCreate = false
    @State private var quickCreateName = ""
    @State private var showArtistDashboard = false
    @State private var showSongRequest = false
    @StateObject private var songRequestManager = SongRequestManager.shared
    @State private var showCrowdSynth = false
    @State private var showChannelSettings = false
    @State private var channelToEdit = ""

    private var recentChannels: [String] {
        (try? JSONDecoder().decode([String].self, from: Data(recentChannelsJSON.utf8))) ?? []
    }
    private var currentChannelName: String { UserDefaults.standard.string(forKey: "channel") ?? "soluna" }
    private var isPlaying: Bool { receiver.state == .receiving || (receiver.sdkReceiver?.isReceivingAudio ?? false) }
    private var isMicActive: Bool { receiver.isMicTransmitting || receiver.isMicMonitoring }

    /// Current DJ from social listening (first member with role "dj")
    private var currentDJ: SocialListeningManager.ListenerInfo? {
        socialListening.currentChannelListeners.first { $0.role == "dj" }
    }
    private var currentDJName: String { currentDJ?.name ?? "DJ \(currentChannelName.capitalized)" }
    private var currentDJDeviceId: String { currentDJ?.id ?? "" }

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
        updateLiveActivity()
        updateWidgetData()
        SharePlayManager.shared.sendChannelChange(ch)
    }
    private func togglePlayback() {
        if receiver.state == .receiving { receiver.stop() } else { receiver.start() }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
            updateNowPlaying()
            updateLiveActivity()
            updateWidgetData()
        }
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
    // MARK: - My Channels (channels I created — only these can broadcast)
    private static let myChannelsKey = "soluna_my_channels"
    private var myChannels: [String] {
        UserDefaults.standard.stringArray(forKey: Self.myChannelsKey) ?? []
    }
    private func saveMyChannel(_ name: String) {
        var list = myChannels
        if !list.contains(name) { list.append(name) }
        UserDefaults.standard.set(list, forKey: Self.myChannelsKey)
    }
    private func isMyChannel(_ name: String) -> Bool {
        myChannels.contains(name)
    }

    private let channelOrder = ["soluna", "jazz", "lofi", "chill", "dance", "bjj", "yuki"]
    private func switchNextChannel() {
        if let idx = channelOrder.firstIndex(of: currentChannelName) {
            switchChannel(channelOrder[(idx + 1) % channelOrder.count])
        }
    }
    private func switchPrevChannel() {
        if let idx = channelOrder.firstIndex(of: currentChannelName) {
            switchChannel(channelOrder[(idx - 1 + channelOrder.count) % channelOrder.count])
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

    // MARK: - Live Activity & Widget

    private var channelEmoji: String {
        let emojis: [String: String] = [
            "soluna": "☀️", "jazz": "🎹", "lofi": "🎧",
            "chill": "🍃", "dance": "⚡", "bjj": "🥋", "yuki": "❄️"
        ]
        return emojis[currentChannelName] ?? "📻"
    }

    private func updateLiveActivity() {
        if #available(iOS 16.2, *) {
            let mgr = LiveActivityManager.shared
            if isPlaying {
                if mgr.isSupported {
                    mgr.start(channel: currentChannelName, emoji: channelEmoji)
                }
                mgr.update(
                    channel: currentChannelName,
                    isPlaying: true,
                    packets: receiver.packetsReceived
                )
            } else {
                mgr.stop()
            }
        }
    }

    private func updateWidgetData() {
        let wd = SolunaWidgetData.shared
        if isPlaying {
            wd.update(
                channel: currentChannelName,
                emoji: channelEmoji,
                isPlaying: true,
                packetsReceived: receiver.packetsReceived,
                trackTitle: receiver.nowPlayingTitle,
                trackArtist: receiver.nowPlayingArtist
            )
        } else {
            wd.clear()
        }
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
            // Reaction overlay — floats above everything
            ReactionOverlayView()
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
        .sheet(isPresented: $showSilentDisco) {
            if #available(iOS 16.0, *) {
                SilentDiscoView(channel: currentChannelName) { ch in switchChannel(ch) }
            }
        }
        .sheet(isPresented: $showChat) {
            ChatView(sendUDP: { msg in
                receiver.sdkSendUDP(msg)
            })
        }
        .sheet(isPresented: $showTipping) {
            TippingView(
                djName: currentDJName,
                djDeviceId: currentDJDeviceId,
                tipManager: tipManager
            )
        }
        .sheet(isPresented: $showAddSpeaker, onDismiss: { newName = ""; newHost = "" }) { addSpeakerSheet }
        .sheet(isPresented: $showPlayer) { PlayerView(model: playerModel) }
        .sheet(isPresented: $showChannelCreate) {
            ChannelPurchaseView(store: channelStore,
                                activeChannel: Binding(get: { currentChannelName }, set: { switchChannel($0) }))
        }
        .sheet(isPresented: $showDJDeckView) { DJDeckView(receiver: receiver) }
        .sheet(isPresented: $showBroadcast) { BroadcastView() }
        .sheet(isPresented: $showSongRequest) {
            SongRequestView(sendUDP: { msg in receiver.sdkSendUDP(msg) })
        }
        .sheet(isPresented: $showListeningReport) {
            NavigationStack {
                ListeningReportView()
                    .navigationTitle("Listening Report")
                    .navigationBarTitleDisplayMode(.inline)
                    .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Done") { showListeningReport = false } } }
            }
            .preferredColorScheme(.dark)
        }
        .sheet(isPresented: $showArtistDashboard) {
            ArtistDashboardView()
                .preferredColorScheme(.dark)
        }
        .sheet(isPresented: $showCrowdSynth) {
            CrowdSynthView()
                .preferredColorScheme(.dark)
        }
        .sheet(isPresented: $showChannelSettings) {
            ChannelSettingsView(channel: channelToEdit)
                .preferredColorScheme(.dark)
        }
        .alert(channelStore.currentPlan == .free ? "Create Channel" : "Create Channel (PRO)", isPresented: $showQuickCreate) {
            if channelStore.currentPlan != .free {
                TextField("Channel name", text: $quickCreateName)
                    .autocorrectionDisabled()
                Button("Create") {
                    let name = quickCreateName.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
                    if !name.isEmpty { saveMyChannel(name); switchChannel(name) }
                }
            } else {
                Button("Create Random Channel") {
                    let id = String("abcdefghijklmnopqrstuvwxyz0123456789".shuffled().prefix(6))
                    let name = "live-\(id)"
                    saveMyChannel(name); switchChannel(name)
                }
                Button("Upgrade to PRO") { showSubscription = true }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text(channelStore.currentPlan == .free
                 ? "Free: random name. Upgrade to PRO for custom names."
                 : "Enter a name for your channel")
        }
        .confirmationDialog("Sleep Timer", isPresented: $showSleepPicker, titleVisibility: .visible) {
            Button("15 min") { sleepTimer.start(minutes: 15) }
            Button("30 min") { sleepTimer.start(minutes: 30) }
            Button("45 min") { sleepTimer.start(minutes: 45) }
            Button("1 hour") { sleepTimer.start(minutes: 60) }
            Button("2 hours") { sleepTimer.start(minutes: 120) }
            Button("Cancel", role: .cancel) {}
        }
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

            // SharePlay session observer
            SharePlayManager.shared.observeSessions()
            // Social Listening & AI Auto Channel
            socialListening.startPolling()
            if aiAutoChannel.isAutoMode { aiAutoChannel.start() }
            // Restore alarm if set
            alarmManager.restoreAlarm()
            Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { _ in
                Task { @MainActor in
                    speakers.applyServerRxDelay()
                    if !(playerModel.daemon?.isConnected ?? false) { playerModel.rebindIfNeeded(speakers.primaryDaemon) }
                    receiver.daemonClient = speakers.primaryDaemon
                    if receiver.state == .receiving, Date().timeIntervalSince(lastListenRecordTime) >= 30 {
                        lastListenRecordTime = Date()
                        FanRankManager.shared.recordListen(channel: UserDefaults.standard.string(forKey: "channel") ?? "soluna")
                    }
                    // Periodic Live Activity & Widget data refresh
                    updateLiveActivity()
                    updateWidgetData()
                }
            }
            playerModel.daemon = speakers.primaryDaemon
        }
        .onChange(of: speakers.speakers.count) { _ in playerModel.rebindIfNeeded(speakers.primaryDaemon) }
        .onChange(of: deepLink.pendingChannel) { ch in
            guard let ch, !ch.isEmpty else { return }; deepLink.pendingChannel = nil; switchChannel(ch)
        }
        .animation(.spring(response: 0.35, dampingFraction: 0.8), value: receiver.state.rawValue)
        // MARK: - Siri Intent Observers
        .onReceive(NotificationCenter.default.publisher(for: .solunaIntentPlay)) { notification in
            if let ch = notification.userInfo?["channel"] as? String {
                switchChannel(ch)
            } else if receiver.state == .stopped || receiver.state == .error {
                receiver.start()
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: .solunaIntentStop)) { _ in
            if receiver.state == .receiving { receiver.stop() }
        }
        .onReceive(NotificationCenter.default.publisher(for: .solunaIntentSwitchChannel)) { notification in
            if let ch = notification.userInfo?["channel"] as? String {
                switchChannel(ch)
            }
        }
        // MARK: - SharePlay Channel Sync
        .onReceive(NotificationCenter.default.publisher(for: .solunaChannelChanged)) { notification in
            if let ch = notification.userInfo?["channel"] as? String {
                switchChannel(ch)
            }
        }
        // MARK: - Handoff Activity
        .userActivity("art.solun.channel") { activity in
            activity.title = "Listening to \(currentChannelName.capitalized)"
            activity.userInfo = ["channel": currentChannelName]
            activity.isEligibleForHandoff = true
        }
        // MARK: - Sleep Timer Fade
        .onReceive(NotificationCenter.default.publisher(for: .solunaSleepFade)) { notification in
            if let fade = notification.userInfo?["fade"] as? Float {
                receiver.volume = masterVolume * fade
            }
        }
    }

    // MARK: - Header

    private var headerBar: some View {
        HStack(spacing: 12) {
            Text("SOLUNA").font(.system(size: 17, weight: .bold, design: .rounded))
                .foregroundStyle(LinearGradient.solLunaGradient)
                .onTapGesture(count: 3) { showDebug = true; showDebugPanel.toggle() }
            Spacer()
            headerBtn("gearshape", .white.opacity(0.6)) { showSettings = true }
        }
    }
    private var chatButton: some View {
        Button { showChat = true } label: {
            ZStack(alignment: .topTrailing) {
                Image(systemName: "bubble.left.fill").font(.system(size: 12, weight: .medium))
                    .foregroundColor(.solunaLuna)
                    .frame(width: 28, height: 28)
                    .background(Color.white.opacity(0.08)).clipShape(Circle())
                if chatManager.unreadCount > 0 {
                    Text("\(min(chatManager.unreadCount, 99))")
                        .font(.system(size: 9, weight: .bold))
                        .foregroundColor(.white)
                        .frame(minWidth: 14, minHeight: 14)
                        .background(Color.solunaMic)
                        .clipShape(Circle())
                        .offset(x: 4, y: -4)
                }
            }
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

    @State private var showDebugPanel = false

    private var listenTab: some View {
        VStack(spacing: 16) {
            nowPlayingHero.padding(.horizontal, 16)
            if receiver.sdkReceiver?.latencyExceeded == true {
                latencyWarningBanner.padding(.horizontal, 16)
            }
            channelGrid.padding(.horizontal, 16)
            quickChannelJoin.padding(.horizontal, 16)
            if isPlaying {
                liveFeed.padding(.horizontal, 16)
            }
            // Debug panel (tap 5 times on channel name to toggle)
            if showDebugPanel {
                connectionDebugPanel.padding(.horizontal, 16)
            }
        }.padding(.bottom, 16)
    }

    private var connectionDebugPanel: some View {
        let sdk = receiver.sdkReceiver
        return VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text("DEBUG").font(.system(size: 10, weight: .bold, design: .monospaced)).foregroundColor(.orange)
                Spacer()
                Button("Close") { showDebugPanel = false }.font(.caption)
            }
            Group {
                debugRow("Channel", currentChannelName)
                debugRow("SDK State", sdk?.state.rawValue ?? "nil")
                debugRow("Connected", "\(sdk?.isConnected ?? false)")
                debugRow("Receiving", "\(sdk?.isReceivingAudio ?? false)")
                debugRow("Packets", "\(sdk?.packetsReceived ?? 0)")
                debugRow("PPS", "\(sdk?.packetsPerSec ?? 0)")
                debugRow("Buffer", "\(sdk?.bufferFillMs ?? 0)ms")
                debugRow("Relay", receiver.relayState.rawValue)
                debugRow("Sync Offset", String(format: "%.1fms", sdk?.syncOffsetMs ?? 0))
                debugRow("Output Latency", String(format: "%.1fms", sdk?.outputLatencyMs ?? 0))
                debugRow("Prefill Target", "\(sdk?.dynamicPrefillThreshold ?? 0) samples")
                debugRow("Latency Target", "\(Int(sdk?.targetTotalLatencyMs ?? 0))ms")
                debugRow("Latency Exceeded", "\(sdk?.latencyExceeded ?? false)")
            }
        }
        .padding(10)
        .background(Color.black.opacity(0.8))
        .clipShape(RoundedRectangle(cornerRadius: 8))
        .font(.system(size: 11, design: .monospaced))
    }

    private func debugRow(_ label: String, _ value: String) -> some View {
        HStack {
            Text(label).foregroundColor(.white.opacity(0.5))
            Spacer()
            Text(value).foregroundColor(.green)
        }
    }

    private var latencyWarningBanner: some View {
        HStack(spacing: 8) {
            Image(systemName: "exclamationmark.triangle.fill")
                .foregroundColor(.yellow)
                .font(.system(size: 14))
            VStack(alignment: .leading, spacing: 2) {
                Text("Bluetooth latency too high for sync")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundColor(.white)
                Text("Use wired output or increase channel latency target")
                    .font(.system(size: 10))
                    .foregroundColor(.white.opacity(0.6))
            }
            Spacer()
        }
        .padding(10)
        .background(Color.orange.opacity(0.2))
        .clipShape(RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Color.orange.opacity(0.3), lineWidth: 1))
    }

    // MARK: - Now Playing Hero (Spotify-style big player card)

    private var nowPlayingHero: some View {
        VStack(spacing: 20) {
            // Channel icon — big and colorful
            let ch = presetChannels.first { $0.id == currentChannelName }
            ZStack {
                Circle()
                    .fill(
                        RadialGradient(
                            colors: [
                                (ch?.color ?? .solunaLuna).opacity(0.6),
                                (ch?.color ?? .solunaLuna).opacity(0.1)
                            ],
                            center: .center,
                            startRadius: 20,
                            endRadius: 100
                        )
                    )
                    .frame(width: 180, height: 180)

                Image(systemName: ch?.icon ?? "music.note")
                    .font(.system(size: 56, weight: .light))
                    .foregroundStyle(
                        LinearGradient(
                            colors: [.white, .white.opacity(0.7)],
                            startPoint: .top,
                            endPoint: .bottom
                        )
                    )

                if isPlaying {
                    AudioVisualizerView(barCount: 48, isPlaying: true)
                        .frame(height: 20)
                        .offset(y: 70)
                }
            }

            // Channel name + description
            VStack(spacing: 4) {
                Text(ch?.label ?? currentChannelName.capitalized)
                    .font(.system(size: 24, weight: .bold))
                    .foregroundColor(.white)
                Text(ch?.description ?? "Custom channel")
                    .font(.system(size: 13))
                    .foregroundColor(.white.opacity(0.5))
            }

            // Transport controls
            HStack(spacing: 32) {
                Button { switchPrevChannel() } label: {
                    Image(systemName: "backward.fill")
                        .font(.system(size: 22))
                        .foregroundColor(.white.opacity(0.7))
                }

                Button { togglePlayback() } label: {
                    ZStack {
                        Circle()
                            .fill(.white)
                            .frame(width: 64, height: 64)
                        if receiver.state == .connecting {
                            ProgressView().tint(.black).scaleEffect(0.9)
                        } else {
                            Image(systemName: isPlaying ? "pause.fill" : "play.fill")
                                .font(.system(size: 24))
                                .foregroundColor(.black)
                                .offset(x: isPlaying ? 0 : 2)
                        }
                    }
                }

                Button { switchNextChannel() } label: {
                    Image(systemName: "forward.fill")
                        .font(.system(size: 22))
                        .foregroundColor(.white.opacity(0.7))
                }
            }

            // Volume
            HStack(spacing: 10) {
                Image(systemName: "speaker.fill")
                    .font(.system(size: 10))
                    .foregroundColor(.white.opacity(0.4))
                Slider(value: $masterVolume, in: 0...1) { _ in
                    if masterMuted { masterMuted = false; receiver.isMuted = false }
                    receiver.volume = masterVolume
                    UserDefaults.standard.set(masterVolume, forKey: "soluna_volume")
                }
                .tint(.white.opacity(0.6))
                Image(systemName: "speaker.wave.3.fill")
                    .font(.system(size: 10))
                    .foregroundColor(.white.opacity(0.4))
            }
            .padding(.horizontal, 20)
        }
        .padding(.vertical, 24)
        .glassCard()
    }

    // MARK: - Live Feed (bottom card — the "wow" factor)

    private var liveFeed: some View {
        VStack(spacing: 8) {
            NowPlayingCard(
                title: receiver.nowPlayingTitle,
                artist: receiver.nowPlayingArtist
            )
            HStack(spacing: 6) {
                HStack(spacing: -6) {
                    ForEach(0..<min(5, max(1, socialListening.count(for: currentChannelName))), id: \.self) { i in
                        Circle()
                            .fill(Color(hue: Double(i) * 0.15, saturation: 0.6, brightness: 0.9))
                            .frame(width: 20, height: 20)
                            .overlay(Circle().stroke(Color.black, lineWidth: 1.5))
                    }
                }
                let count = socialListening.count(for: currentChannelName)
                if count > 0 {
                    Text("\(count) listening now")
                        .font(.system(size: 12, weight: .medium))
                        .foregroundColor(.white.opacity(0.6))
                } else {
                    Text("You're the first here")
                        .font(.system(size: 12, weight: .medium))
                        .foregroundColor(.white.opacity(0.4))
                }
                Spacer()
            }
        }
        .padding(12)
        .glassCard()
    }

    // MARK: - Channel Grid

    private var channelGrid: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Channels").font(.system(size: 15, weight: .bold)).foregroundColor(.white)
                Spacer()
                Button { quickCreateName = ""; showQuickCreate = true } label: {
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
                                ListenerBadge(count: socialListening.count(for: ch.id))
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
                    .contextMenu {
                        Button {
                            channelToEdit = ch.id
                            showChannelSettings = true
                        } label: {
                            Label("Channel Settings", systemImage: "gearshape")
                        }
                    }
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

    // MARK: - Quick Channel Join

    private var quickChannelJoin: some View {
        HStack(spacing: 8) {
            Image(systemName: "number")
                .font(.system(size: 12))
                .foregroundColor(.white.opacity(0.4))
            TextField("Channel name...", text: $quickChannelInput)
                .textFieldStyle(.plain)
                .font(.system(size: 14))
                .foregroundColor(.white)
                .autocorrectionDisabled()
                .textInputAutocapitalization(.never)
                .onSubmit {
                    let ch = quickChannelInput.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
                    if !ch.isEmpty { switchChannel(ch) }
                }
            if !quickChannelInput.isEmpty {
                Button {
                    let ch = quickChannelInput.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
                    if !ch.isEmpty { switchChannel(ch) }
                } label: {
                    Image(systemName: "play.circle.fill")
                        .font(.system(size: 22))
                        .foregroundColor(.white)
                }
            }
        }
        .padding(12)
        .background(Color.white.opacity(0.08))
        .clipShape(RoundedRectangle(cornerRadius: 10))
    }

    // MARK: - Now Playing

    private var nowPlayingArea: some View {
        // Spotify-style mini player: single row, fixed height
        HStack(spacing: 12) {
            // Play/Pause button
            Button(action: togglePlayback) {
                ZStack {
                    if receiver.state == .connecting {
                        ProgressView().tint(.white).scaleEffect(0.8)
                    } else {
                        Circle().fill(isPlaying ? LinearGradient.solGradient : LinearGradient.lunaGradient)
                            .frame(width: 44, height: 44)
                        Image(systemName: isPlaying ? "pause.fill" : "play.fill")
                            .font(.system(size: 18, weight: .bold)).foregroundColor(.white)
                            .offset(x: isPlaying ? 0 : 2)
                    }
                }.frame(width: 44, height: 44)
            }.buttonStyle(.plain)

            // Channel name + status
            VStack(alignment: .leading, spacing: 2) {
                Text(receiver.nowPlayingTitle ?? currentChannelName.capitalized)
                    .font(.system(size: 15, weight: .bold)).foregroundColor(.white).lineLimit(1)
                HStack(spacing: 4) {
                    if isPlaying {
                        Circle().fill(Color.solunaLive).frame(width: 5, height: 5)
                        Text("\(receiver.sdkReceiver?.bufferFillMs ?? 0)ms")
                            .font(.system(size: 10, design: .monospaced)).foregroundColor(.white.opacity(0.4))
                    } else {
                        Text("Ready").font(.system(size: 12)).foregroundColor(.white.opacity(0.4))
                    }
                }
            }

            Spacer()

            // Prev / Rewind / Next
            Button { switchPrevChannel() } label: {
                Image(systemName: "backward.fill").font(.system(size: 14)).foregroundColor(.white.opacity(0.4))
            }
            RewindButton(sendUDP: { msg in receiver.sdkSendUDP(msg) })
            Button { switchNextChannel() } label: {
                Image(systemName: "forward.fill").font(.system(size: 14)).foregroundColor(.white.opacity(0.4))
            }
        }.padding(12).glassCard()
        .onTapGesture(count: 2) {
            let emojis = ["🔥", "❤️", "🎵", "✨", "🙌"]
            let emoji = emojis.randomElement()!
            let x = CGFloat.random(in: 0.2...0.8)
            reactionManager.addLocal(emoji: emoji, x: x)
            receiver.sdkSendUDP("TEXT:react \(emoji)\n")
        }
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
        HStack(spacing: 12) {
            qBtn("heart.fill", .solunaSolEnd) { showTipping = true }
            qBtn("qrcode", .purple) { showSilentDisco = true }
            qBtn("moon.fill", sleepTimer.isActive ? .solunaLuna : .white.opacity(0.4)) {
                if sleepTimer.isActive { sleepTimer.stop() } else { showSleepPicker = true }
            }
            qBtn("antenna.radiowaves.left.and.right", .solunaSol) { showBroadcast = true }
            qBtn("music.note.list", .solunaLuna) { showSongRequest = true }
            TeleportButton(sendPacket: { packet in
                receiver.sdkSendUDPBytes(packet)
            })
            qBtn("pianokeys", .solunaGradientStart) { showCrowdSynth = true }
            Spacer()
        }
    }

    private func qBtn(_ icon: String, _ color: Color, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: icon).font(.system(size: 13)).foregroundColor(color)
                .frame(width: 32, height: 32).background(color.opacity(0.12)).clipShape(Circle())
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
                HStack(spacing: 12) {
                    Button { showFanRank = true } label: {
                        Text("View Details").font(.system(size: 14, weight: .semibold)).foregroundColor(.solunaLuna)
                            .padding(.horizontal, 20).padding(.vertical, 10)
                            .background(Color.solunaLuna.opacity(0.15)).clipShape(Capsule())
                    }
                    Button { showListeningReport = true } label: {
                        Text("Weekly Report").font(.system(size: 14, weight: .semibold)).foregroundColor(.solunaSol)
                            .padding(.horizontal, 20).padding(.vertical, 10)
                            .background(Color.solunaSol.opacity(0.15)).clipShape(Capsule())
                    }
                }
                Button { showArtistDashboard = true } label: {
                    HStack(spacing: 6) {
                        Image(systemName: "chart.bar.fill").font(.system(size: 12))
                        Text("Artist Dashboard").font(.system(size: 14, weight: .semibold))
                    }
                    .foregroundColor(.solunaLive)
                    .padding(.horizontal, 20).padding(.vertical, 10)
                    .background(Color.solunaLive.opacity(0.15)).clipShape(Capsule())
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
            // MARK: - Alarm
            VStack(spacing: 12) {
                HStack {
                    Image(systemName: "alarm.fill").font(.system(size: 18)).foregroundColor(.solunaSol)
                    Text("Wake Up Alarm").font(.system(size: 17, weight: .bold)).foregroundColor(.white)
                    Spacer()
                    if alarmManager.isAlarmSet {
                        Button { alarmManager.cancelAlarm() } label: {
                            Text("Cancel").font(.system(size: 13, weight: .semibold)).foregroundColor(.solunaMic)
                        }
                    }
                }
                if alarmManager.isAlarmSet, let time = alarmManager.alarmTime {
                    HStack {
                        Image(systemName: "bell.fill").font(.caption).foregroundColor(.solunaLive)
                        Text(time, style: .time).font(.system(size: 15, weight: .medium)).foregroundColor(.white)
                        Text("on").foregroundColor(.white.opacity(0.4)).font(.caption)
                        Text(alarmManager.alarmChannel.capitalized).font(.system(size: 13, weight: .bold)).foregroundColor(.solunaLuna)
                    }
                } else {
                    alarmSetupView
                }
            }.padding(16).glassCard()

            // MARK: - Quick Actions
            VStack(alignment: .leading, spacing: 12) {
                Text("Actions").font(.system(size: 15, weight: .bold)).foregroundColor(.white)
                LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible()), GridItem(.flexible()), GridItem(.flexible())], spacing: 12) {
                    profileActionBtn("bubble.left.fill", "Chat", .solunaLuna) { showChat = true }
                    profileActionBtn("heart.fill", "Tip", .solunaSolEnd) { showTipping = true }
                    profileActionBtn("music.note.list", "Request", .solunaLuna) { showSongRequest = true }
                    profileActionBtn("pianokeys", "Synth", .solunaGradientStart) { showCrowdSynth = true }
                    profileActionBtn("qrcode", "Silent Disco", .purple) { showSilentDisco = true }
                    profileActionBtn("antenna.radiowaves.left.and.right", "Broadcast", .solunaSol) { showBroadcast = true }
                    profileActionBtn("moon.fill", "Sleep", sleepTimer.isActive ? .solunaLuna : .white.opacity(0.4)) {
                        if sleepTimer.isActive { sleepTimer.stop() } else { showSleepPicker = true }
                    }
                    profileActionBtn("dial.low.fill", "DJ Deck", .solunaSol) { showDJDeckView = true }
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

    @State private var alarmPickerTime = Date()
    @State private var alarmPickerChannel = "chill"

    private var alarmSetupView: some View {
        VStack(spacing: 10) {
            DatePicker("Time", selection: $alarmPickerTime, displayedComponents: .hourAndMinute)
                .datePickerStyle(.compact)
                .labelsHidden()
                .colorScheme(.dark)
            Picker("Channel", selection: $alarmPickerChannel) {
                ForEach(presetChannels) { ch in
                    Text(ch.label).tag(ch.id)
                }
            }
            .pickerStyle(.segmented)
            Button {
                alarmManager.setAlarm(time: alarmPickerTime, channel: alarmPickerChannel)
            } label: {
                Text("Set Alarm").font(.system(size: 14, weight: .semibold)).foregroundColor(.white)
                    .frame(maxWidth: .infinity).padding(.vertical, 10)
                    .background(LinearGradient.solGradient).clipShape(RoundedRectangle(cornerRadius: 10))
            }
        }
    }

    private func profileActionBtn(_ icon: String, _ label: String, _ color: Color, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            VStack(spacing: 6) {
                Image(systemName: icon).font(.system(size: 16)).foregroundColor(color)
                    .frame(width: 40, height: 40).background(color.opacity(0.12)).clipShape(Circle())
                Text(label).font(.system(size: 10, weight: .medium)).foregroundColor(.white.opacity(0.6)).lineLimit(1)
            }
        }
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
