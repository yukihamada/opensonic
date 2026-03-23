//
//  ProDashboardView.swift
//  SolunaReceiverMac
//
//  Professional broadcast dashboard for DJs and event managers.
//  Dark pro-audio theme with orange accents, inspired by koe.live/dashboard.html.
//

import SwiftUI
import AVFoundation
import UniformTypeIdentifiers

// MARK: - Color Constants

private extension Color {
    static let proBg        = Color(red: 0.04, green: 0.04, blue: 0.04)
    static let proCard      = Color(red: 0.10, green: 0.10, blue: 0.10)
    static let proBorder    = Color(red: 0.17, green: 0.17, blue: 0.17)
    static let proAccent    = Color(red: 0.976, green: 0.451, blue: 0.086)  // #f97316
    static let proGreen     = Color(red: 0.20, green: 0.83, blue: 0.45)
    static let proYellow    = Color(red: 0.98, green: 0.80, blue: 0.20)
    static let proRed       = Color(red: 0.95, green: 0.25, blue: 0.25)
    static let proText      = Color.white
    static let proTextDim   = Color.white.opacity(0.6)
}

// MARK: - ProDashboardView

struct ProDashboardView: View {
    @ObservedObject var receiver: AudioReceiver
    @StateObject private var registry = GlobalDeviceRegistry()
    @StateObject private var audioSourceManager = AudioSourceManager.shared
    @StateObject private var channelStore = ChannelStore()

    // State
    @State private var elapsedTime: TimeInterval = 0
    @State private var startTime = Date()
    @State private var vuLeft: Float = 0
    @State private var vuRight: Float = 0
    @State private var logEntries: [(id: UUID, date: Date, message: String, color: Color)] = []
    @State private var masterVolume: Float = 1.0
    @State private var isMuted = false
    @State private var isStreaming = false
    @State private var peakLevel: Float = -12.0
    @State private var jitterMs: Double = 0
    @State private var eqBands: [Float] = [0.4, 0.5, 0.65, 0.8, 0.9, 0.85, 0.7, 0.55, 0.45, 0.35]
    @AppStorage("channel") private var channel = "soluna"

    // Timers
    let vuTimer = Timer.publish(every: 0.05, on: .main, in: .common).autoconnect()
    let statsTimer = Timer.publish(every: 1.0, on: .main, in: .common).autoconnect()

    private let eqFrequencies = ["31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"]
    @State private var broadcastChannel: String = ""
    @State private var localLatencyMs: Double = 50
    @State private var wanLatencyMs: Double = 500

    // Playlist / file playback
    @State private var playlist: [(id: UUID, url: URL, name: String)] = []
    @State private var currentTrackIndex: Int? = nil
    @State private var isPlayingFile = false
    @State private var filePlaybackProgress: Double = 0
    @State private var audioPlayer: AVAudioPlayer? = nil
    let filePlaybackTimer = Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()

    var body: some View {
        VStack(spacing: 0) {
            topBar
            Divider().background(Color.proBorder)
            HStack(spacing: 0) {
                leftPanel
                    .frame(minWidth: 520)
                Divider().background(Color.proBorder)
                rightSidebar
                    .frame(width: 320)
            }
        }
        .background(Color.proBg)
        .frame(minWidth: 1000, minHeight: 600)
        .onAppear {
            startTime = Date()
            isStreaming = SDKAudioReceiver.shared.isPlaying
            masterVolume = SDKAudioReceiver.shared.volume
            isMuted = SDKAudioReceiver.shared.isMuted
            addLog("Dashboard opened", color: .proAccent)
            registry.refresh()
            audioSourceManager.refresh()
            if broadcastChannel.isEmpty {
                generateRandomChannel()
            }
        }
        .onReceive(vuTimer) { _ in
            updateVU()
        }
        .onReceive(statsTimer) { _ in
            updateStats()
        }
        .onReceive(filePlaybackTimer) { _ in
            checkFilePlayback()
        }
    }

    // MARK: - Top Bar

    @Environment(\.dismiss) private var dismiss

    private var topBar: some View {
        HStack(spacing: 16) {
            // Close button
            Button { dismiss() } label: {
                Image(systemName: "xmark")
                    .font(.system(size: 12, weight: .bold))
                    .foregroundColor(.proTextDim)
                    .frame(width: 28, height: 28)
                    .background(Color.proCard)
                    .clipShape(Circle())
            }
            .buttonStyle(.plain)
            .keyboardShortcut(.escape, modifiers: [])

            // Channel name
            Text(channel.uppercased())
                .font(.system(size: 16, weight: .bold, design: .monospaced))
                .foregroundColor(.proText)

            // LIVE badge
            if isStreaming {
                HStack(spacing: 6) {
                    Circle()
                        .fill(Color.proRed)
                        .frame(width: 8, height: 8)
                    Text("LIVE")
                        .font(.system(size: 11, weight: .heavy, design: .monospaced))
                        .foregroundColor(.proRed)
                    Text(formattedElapsed)
                        .font(.system(size: 11, weight: .medium, design: .monospaced))
                        .foregroundColor(.proTextDim)
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 4)
                .background(Color.proRed.opacity(0.15))
                .cornerRadius(4)
            }

            Spacer()

            // Listener count
            HStack(spacing: 4) {
                Image(systemName: "person.2.fill")
                    .font(.system(size: 11))
                Text("\(receiver.groupMembers.count)")
                    .font(.system(size: 12, weight: .semibold, design: .monospaced))
            }
            .foregroundColor(.proTextDim)

            // Average sync offset
            HStack(spacing: 4) {
                Image(systemName: "clock.arrow.2.circlepath")
                    .font(.system(size: 11))
                Text(String(format: "%.1fms", SDKAudioReceiver.shared.syncOffsetMs))
                    .font(.system(size: 12, weight: .medium, design: .monospaced))
            }
            .foregroundColor(abs(SDKAudioReceiver.shared.syncOffsetMs) < 5 ? .proGreen : .proYellow)

            // Master volume
            HStack(spacing: 6) {
                Image(systemName: isMuted ? "speaker.slash.fill" : "speaker.wave.2.fill")
                    .font(.system(size: 12))
                    .foregroundColor(isMuted ? .proRed : .proAccent)
                Slider(value: $masterVolume, in: 0...1)
                    .frame(width: 120)
                    .tint(.proAccent)
                    .onChange(of: masterVolume) { newVal in
                        SDKAudioReceiver.shared.volume = newVal
                    }
                Text("\(Int(masterVolume * 100))%")
                    .font(.system(size: 11, weight: .medium, design: .monospaced))
                    .foregroundColor(.proTextDim)
                    .frame(width: 32, alignment: .trailing)
            }

            // Stream toggle
            Button(action: {
                SDKAudioReceiver.shared.toggle()
                isStreaming = SDKAudioReceiver.shared.isPlaying
                addLog(isStreaming ? "Streaming started" : "Streaming stopped",
                       color: isStreaming ? .proGreen : .proRed)
            }) {
                HStack(spacing: 4) {
                    Image(systemName: isStreaming ? "stop.fill" : "play.fill")
                        .font(.system(size: 10))
                    Text(isStreaming ? "STOP" : "START")
                        .font(.system(size: 11, weight: .bold, design: .monospaced))
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 6)
                .foregroundColor(isStreaming ? .proRed : .proGreen)
                .background((isStreaming ? Color.proRed : Color.proGreen).opacity(0.15))
                .cornerRadius(4)
                .overlay(
                    RoundedRectangle(cornerRadius: 4)
                        .stroke((isStreaming ? Color.proRed : Color.proGreen).opacity(0.4))
                )
            }
            .buttonStyle(.plain)

            // Network mode indicator (auto-detected)
            HStack(spacing: 4) {
                Circle()
                    .fill(receiver.broadcastMode == .local ? Color.proGreen : Color.proAccent)
                    .frame(width: 6, height: 6)
                Text(receiver.broadcastMode == .local ? "LAN" : "WAN")
                    .font(.system(size: 10, weight: .bold, design: .monospaced))
                    .foregroundColor(.proTextDim)
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(Color.proCard)
            .cornerRadius(3)

            // Mic TX
            Button(action: {
                receiver.toggleMic()
                addLog(receiver.isMicTransmitting ? "Mic TX ON" : "Mic TX OFF",
                       color: receiver.isMicTransmitting ? .proGreen : .proRed)
            }) {
                HStack(spacing: 4) {
                    Image(systemName: receiver.isMicTransmitting ? "mic.fill" : "mic")
                        .font(.system(size: 10))
                    Text("MIC")
                        .font(.system(size: 11, weight: .bold, design: .monospaced))
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 6)
                .foregroundColor(receiver.isMicTransmitting ? .white : .proTextDim)
                .background(receiver.isMicTransmitting ? Color.proRed : Color.proCard)
                .cornerRadius(4)
                .overlay(RoundedRectangle(cornerRadius: 4).stroke(Color.proBorder))
            }
            .buttonStyle(.plain)

            // System Audio TX
            Button(action: {
                receiver.toggleShmTransmit()
                addLog(receiver.isShmTransmitting ? "System Audio TX ON" : "System Audio TX OFF",
                       color: receiver.isShmTransmitting ? .proGreen : .proRed)
            }) {
                HStack(spacing: 4) {
                    Image(systemName: receiver.isShmTransmitting ? "hifispeaker.fill" : "hifispeaker")
                        .font(.system(size: 10))
                    Text("SYS")
                        .font(.system(size: 11, weight: .bold, design: .monospaced))
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 6)
                .foregroundColor(receiver.isShmTransmitting ? .white : .proTextDim)
                .background(receiver.isShmTransmitting ? Color.proAccent : Color.proCard)
                .cornerRadius(4)
                .overlay(RoundedRectangle(cornerRadius: 4).stroke(Color.proBorder))
            }
            .buttonStyle(.plain)

            // Mute All
            Button(action: {
                isMuted.toggle()
                SDKAudioReceiver.shared.isMuted = isMuted
                addLog(isMuted ? "Muted all" : "Unmuted", color: isMuted ? .proRed : .proGreen)
            }) {
                HStack(spacing: 4) {
                    Image(systemName: isMuted ? "speaker.slash.fill" : "speaker.wave.2.fill")
                        .font(.system(size: 10))
                    Text("MUTE")
                        .font(.system(size: 11, weight: .bold, design: .monospaced))
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 6)
                .foregroundColor(isMuted ? .white : .proTextDim)
                .background(isMuted ? Color.proRed.opacity(0.8) : Color.proCard)
                .cornerRadius(4)
                .overlay(
                    RoundedRectangle(cornerRadius: 4)
                        .stroke(Color.proBorder)
                )
            }
            .buttonStyle(.plain)
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 12)
        .background(Color.proCard)
    }

    // MARK: - Left Panel

    private var leftPanel: some View {
        ScrollView {
            VStack(spacing: 16) {
                vuMetersSection
                nowPlayingSection
                channelsSection
                playlistSection
                devicesSection
            }
            .padding(16)
        }
    }

    // MARK: - VU Meters

    private var vuMetersSection: some View {
        proCard(title: "VU METERS", icon: "waveform") {
            VStack(spacing: 10) {
                vuMeterRow(label: "L", value: vuLeft)
                vuMeterRow(label: "R", value: vuRight)
            }
            .padding(.vertical, 4)
        }
    }

    private func vuMeterRow(label: String, value: Float) -> some View {
        HStack(spacing: 8) {
            Text(label)
                .font(.system(size: 13, weight: .bold, design: .monospaced))
                .foregroundColor(.proTextDim)
                .frame(width: 16)

            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    // Background
                    RoundedRectangle(cornerRadius: 3)
                        .fill(Color.proBg)

                    // Meter fill with green->yellow->red gradient
                    let fillWidth = CGFloat(value) * geo.size.width
                    HStack(spacing: 0) {
                        LinearGradient(
                            colors: [.proGreen, .proYellow, .proRed],
                            startPoint: .leading,
                            endPoint: .trailing
                        )
                        .frame(width: fillWidth)
                        .clipShape(RoundedRectangle(cornerRadius: 3))
                    }

                    // Tick marks
                    ForEach([0.25, 0.5, 0.75, 0.9], id: \.self) { tick in
                        Rectangle()
                            .fill(Color.proBorder.opacity(0.6))
                            .frame(width: 1, height: geo.size.height)
                            .offset(x: CGFloat(tick) * geo.size.width)
                    }
                }
            }
            .frame(height: 20)

            Text(String(format: "%+.1f", 20 * log10(max(value, 0.0001))))
                .font(.system(size: 10, weight: .medium, design: .monospaced))
                .foregroundColor(.proTextDim)
                .frame(width: 44, alignment: .trailing)
        }
    }

    // MARK: - Now Playing

    private var nowPlayingSection: some View {
        proCard(title: "NOW PLAYING", icon: "music.note") {
            HStack(spacing: 12) {
                RoundedRectangle(cornerRadius: 6)
                    .fill(
                        LinearGradient(
                            colors: [.proAccent.opacity(0.6), .proAccent.opacity(0.2)],
                            startPoint: .topLeading, endPoint: .bottomTrailing
                        )
                    )
                    .frame(width: 48, height: 48)
                    .overlay(
                        Image(systemName: "music.note")
                            .font(.system(size: 20))
                            .foregroundColor(.proAccent)
                    )

                VStack(alignment: .leading, spacing: 4) {
                    Text("Channel: \(channel)")
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundColor(.proText)
                    Text(isStreaming ? "Relay audio stream" : "Not streaming")
                        .font(.system(size: 12))
                        .foregroundColor(.proTextDim)
                }

                Spacer()

                // Transport controls
                HStack(spacing: 12) {
                    Button(action: { switchToPreviousChannel() }) {
                        Image(systemName: "backward.fill")
                            .font(.system(size: 14))
                    }
                    .buttonStyle(.plain)
                    .foregroundColor(.proTextDim)

                    Button(action: {
                        SDKAudioReceiver.shared.toggle()
                        isStreaming = SDKAudioReceiver.shared.isPlaying
                    }) {
                        Image(systemName: isStreaming ? "pause.fill" : "play.fill")
                            .font(.system(size: 18))
                            .foregroundColor(.proAccent)
                            .frame(width: 36, height: 36)
                            .background(Color.proAccent.opacity(0.15))
                            .clipShape(Circle())
                    }
                    .buttonStyle(.plain)

                    Button(action: { switchToNextChannel() }) {
                        Image(systemName: "forward.fill")
                            .font(.system(size: 14))
                    }
                    .buttonStyle(.plain)
                    .foregroundColor(.proTextDim)
                }
            }
        }
    }

    // MARK: - Channels

    @State private var showUpgrade = false

    private var isPro: Bool { channelStore.isPurchased }

    @State private var useCustomChannel = false
    private var purchasedChannel: String? { channelStore.purchasedChannelName }

    private var channelsSection: some View {
        proCard(title: "MY CHANNEL", icon: "antenna.radiowaves.left.and.right") {
            VStack(spacing: 12) {
                // Channel type switcher
                HStack(spacing: 6) {
                    channelTab("Random", icon: "dice", active: !useCustomChannel) {
                        useCustomChannel = false
                        generateRandomChannel()
                    }
                    channelTab(
                        isPro ? (purchasedChannel ?? "Custom") : "Custom ¥1,000/yr",
                        icon: "crown.fill",
                        active: useCustomChannel,
                        locked: !isPro
                    ) {
                        if isPro {
                            useCustomChannel = true
                            if let name = purchasedChannel {
                                broadcastChannel = name
                                setBroadcastChannel()
                            }
                        } else {
                            // Start purchase flow
                            Task {
                                do {
                                    if let tx = try await channelStore.purchase() {
                                        useCustomChannel = true
                                        addLog("Purchased custom channel!", color: .proGreen)
                                    }
                                } catch {
                                    addLog("Purchase failed: \(error.localizedDescription)", color: .proRed)
                                }
                            }
                        }
                    }
                }

                // Channel display
                if useCustomChannel && isPro {
                    // Custom channel input (Pro)
                    HStack(spacing: 8) {
                        Image(systemName: "crown.fill")
                            .font(.system(size: 10))
                            .foregroundColor(.yellow)
                        TextField("Channel name", text: $broadcastChannel)
                            .textFieldStyle(.plain)
                            .font(.system(size: 14, weight: .medium, design: .monospaced))
                            .foregroundColor(.proAccent)
                            .onSubmit {
                                channelStore.savePurchasedChannel(broadcastChannel)
                                setBroadcastChannel()
                            }
                        if channelStore.expiryDate != nil {
                            Text("~\(channelStore.expiryDate!, style: .date)")
                                .font(.system(size: 9, design: .monospaced))
                                .foregroundColor(.proTextDim)
                        }
                    }
                    .padding(8)
                    .background(Color.proBg)
                    .cornerRadius(4)
                    .overlay(RoundedRectangle(cornerRadius: 4).stroke(Color.proAccent.opacity(0.3)))
                } else {
                    // Random channel
                    HStack(spacing: 8) {
                        Image(systemName: "number")
                            .font(.system(size: 12))
                            .foregroundColor(.proTextDim)
                        Text(broadcastChannel)
                            .font(.system(size: 14, weight: .medium, design: .monospaced))
                            .foregroundColor(.proText)
                        Spacer()
                        Button { generateRandomChannel() } label: {
                            Image(systemName: "arrow.clockwise")
                                .font(.system(size: 11))
                                .foregroundColor(.proAccent)
                        }
                        .buttonStyle(.plain)
                    }
                    .padding(8)
                    .background(Color.proBg)
                    .cornerRadius(4)
                    .overlay(RoundedRectangle(cornerRadius: 4).stroke(Color.proBorder))
                }

                // Audio source picker
                HStack(spacing: 8) {
                    Image(systemName: iconForSourceType(audioSourceManager.selectedSource?.type))
                        .font(.system(size: 12))
                        .foregroundColor(.proAccent)
                    Picker("Source", selection: $audioSourceManager.selectedSource) {
                        ForEach(audioSourceManager.sources) { source in
                            HStack(spacing: 6) {
                                Image(systemName: iconForSourceType(source.type))
                                Text(source.name)
                            }.tag(Optional(source))
                        }
                    }
                    .labelsHidden()
                    Button(action: { audioSourceManager.refresh() }) {
                        Image(systemName: "arrow.clockwise")
                            .font(.system(size: 10))
                            .foregroundColor(.proTextDim)
                    }
                    .buttonStyle(.plain)
                }
                .padding(8)
                .background(Color.proBg)
                .cornerRadius(4)
                .overlay(RoundedRectangle(cornerRadius: 4).stroke(Color.proBorder))

                // Latency settings
                latencySettingsPanel

                // Current broadcast status
                HStack(spacing: 8) {
                    Circle()
                        .fill(isBroadcasting ? Color.proGreen : Color.proBorder)
                        .frame(width: 8, height: 8)
                    Text(isBroadcasting ? "Broadcasting on \(channel)" : "Not broadcasting")
                        .font(.system(size: 12, design: .monospaced))
                        .foregroundColor(isBroadcasting ? .proGreen : .proTextDim)
                    Spacer()
                    Text("\(receiver.groupMembers.count) listeners")
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundColor(.proTextDim)
                }

                // Go Live button
                Button {
                    if isBroadcasting {
                        stopBroadcast()
                    } else {
                        setBroadcastChannel()
                        startBroadcast()
                    }
                } label: {
                    HStack(spacing: 6) {
                        Image(systemName: isBroadcasting ? "stop.circle.fill" : "dot.circle.fill")
                        Text(isBroadcasting ? "STOP BROADCAST" : "GO LIVE")
                            .font(.system(size: 13, weight: .bold, design: .monospaced))
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 10)
                    .foregroundColor(isBroadcasting ? .proRed : .white)
                    .background(isBroadcasting ? Color.proRed.opacity(0.15) : Color.proAccent)
                    .cornerRadius(6)
                }
                .buttonStyle(.plain)
            }
        }
    }

    private var isBroadcasting: Bool {
        receiver.isMicTransmitting || receiver.isShmTransmitting
    }

    private func channelTab(_ label: String, icon: String, active: Bool, locked: Bool = false, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 4) {
                Image(systemName: icon)
                    .font(.system(size: 10))
                    .foregroundColor(locked ? .proTextDim : (active ? .white : .proTextDim))
                Text(label)
                    .font(.system(size: 11, weight: active ? .bold : .medium, design: .monospaced))
                    .foregroundColor(active ? .white : .proTextDim)
                if locked {
                    Image(systemName: "lock.fill")
                        .font(.system(size: 8))
                        .foregroundColor(.proTextDim)
                }
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 6)
            .background(active ? Color.proAccent.opacity(0.3) : Color.proBg)
            .cornerRadius(4)
            .overlay(RoundedRectangle(cornerRadius: 4).stroke(active ? Color.proAccent.opacity(0.5) : Color.proBorder))
        }
        .buttonStyle(.plain)
    }

    private var latencySettingsPanel: some View {
        VStack(spacing: 8) {
            // Header
            HStack {
                Image(systemName: "clock.arrow.2.circlepath")
                    .font(.system(size: 10))
                    .foregroundColor(.proAccent)
                Text("LATENCY TARGET")
                    .font(.system(size: 10, weight: .bold, design: .monospaced))
                    .foregroundColor(.proTextDim)
                Spacer()
            }

            // Local (LAN)
            HStack(spacing: 8) {
                HStack(spacing: 4) {
                    Circle().fill(Color.proGreen).frame(width: 6, height: 6)
                    Text("LAN")
                        .font(.system(size: 10, weight: .bold, design: .monospaced))
                        .foregroundColor(.proText)
                }
                .frame(width: 40, alignment: .leading)

                Slider(value: $localLatencyMs, in: 5...200, step: 5)
                    .tint(.proGreen)
                    .frame(maxWidth: .infinity)

                Text("\(Int(localLatencyMs))ms")
                    .font(.system(size: 11, weight: .medium, design: .monospaced))
                    .foregroundColor(.proGreen)
                    .frame(width: 45, alignment: .trailing)
            }

            // WAN (Remote)
            HStack(spacing: 8) {
                HStack(spacing: 4) {
                    Circle().fill(Color.proAccent).frame(width: 6, height: 6)
                    Text("WAN")
                        .font(.system(size: 10, weight: .bold, design: .monospaced))
                        .foregroundColor(.proText)
                }
                .frame(width: 40, alignment: .leading)

                Slider(value: $wanLatencyMs, in: 20...1000, step: 10)
                    .tint(.proAccent)
                    .frame(maxWidth: .infinity)

                Text("\(Int(wanLatencyMs))ms")
                    .font(.system(size: 11, weight: .medium, design: .monospaced))
                    .foregroundColor(.proAccent)
                    .frame(width: 45, alignment: .trailing)
            }

            // Quick presets
            HStack(spacing: 6) {
                latencyPreset("Real-time", local: 5, wan: 50, color: .proGreen)
                latencyPreset("Balanced", local: 20, wan: 300, color: .proAccent)
                latencyPreset("Stable", local: 50, wan: 500, color: .blue)
                Spacer()
            }
        }
        .padding(10)
        .background(Color.proBg)
        .cornerRadius(4)
        .overlay(RoundedRectangle(cornerRadius: 4).stroke(Color.proBorder))
        .onChange(of: localLatencyMs) { _ in applyLatencySettings() }
        .onChange(of: wanLatencyMs) { _ in applyLatencySettings() }
    }

    private func latencyPreset(_ label: String, local: Double, wan: Double, color: Color) -> some View {
        let isActive = abs(localLatencyMs - local) < 3 && abs(wanLatencyMs - wan) < 15
        return Button {
            localLatencyMs = local
            wanLatencyMs = wan
            addLog("Preset: \(label) (LAN \(Int(local))ms / WAN \(Int(wan))ms)", color: color)
        } label: {
            Text(label)
                .font(.system(size: 10, weight: .medium, design: .monospaced))
                .foregroundColor(isActive ? .white : color)
                .padding(.horizontal, 8)
                .padding(.vertical, 3)
                .background(isActive ? color.opacity(0.8) : color.opacity(0.1))
                .cornerRadius(3)
        }
        .buttonStyle(.plain)
    }

    private func applyLatencySettings() {
        // Update the channel config on the relay server
        let ch = broadcastChannel.isEmpty ? channel : broadcastChannel
        guard !ch.isEmpty else { return }
        guard let url = URL(string: "https://relay.solun.art/api/channel-config/\(ch)") else { return }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        let body: [String: Any] = [
            "latencyMs": Int(wanLatencyMs),
            "localLatencyMs": Int(localLatencyMs),
            "mode": localLatencyMs <= 20 ? "live" : "radio"
        ]
        request.httpBody = try? JSONSerialization.data(withJSONObject: body)
        URLSession.shared.dataTask(with: request) { _, _, _ in }.resume()

        // Also update local SDK target
        SDKAudioReceiver.shared.targetTotalLatencyMs = wanLatencyMs
    }

    private func generateRandomChannel() {
        let id = String("abcdefghijklmnopqrstuvwxyz0123456789".shuffled().prefix(6))
        broadcastChannel = "live-\(id)"
        setBroadcastChannel()
        addLog("Random channel: \(broadcastChannel)", color: .proAccent)
    }

    private func setBroadcastChannel() {
        let ch = broadcastChannel.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !ch.isEmpty else { return }
        channel = ch
        SDKAudioReceiver.shared.setChannel(ch)
        addLog("Channel set: \(ch)", color: .proAccent)
    }

    private func startBroadcast() {
        if broadcastChannel.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            broadcastChannel = channel
        }
        setBroadcastChannel()
        receiver.toggleShmTransmit()
        addLog("Broadcast started on \(channel)", color: .proGreen)
    }

    private func stopBroadcast() {
        if receiver.isMicTransmitting { receiver.toggleMic() }
        if receiver.isShmTransmitting { receiver.toggleShmTransmit() }
        addLog("Broadcast stopped", color: .proRed)
    }

    // MARK: - Playlist

    private var playlistSection: some View {
        proCard(title: "PLAYLIST", icon: "music.note.list") {
            VStack(spacing: 10) {
                // Add files button + track count
                HStack {
                    Button {
                        let panel = NSOpenPanel()
                        panel.allowedContentTypes = [
                            UTType(filenameExtension: "mp3")!,
                            UTType(filenameExtension: "wav")!,
                            UTType(filenameExtension: "m4a")!,
                            UTType(filenameExtension: "aac")!,
                            UTType(filenameExtension: "flac")!,
                            UTType(filenameExtension: "aiff")!,
                        ]
                        panel.allowsMultipleSelection = true
                        panel.canChooseDirectories = true
                        panel.begin { response in
                            if response == .OK {
                                DispatchQueue.main.async {
                                    for url in panel.urls {
                                        addToPlaylist(url)
                                    }
                                }
                            }
                        }
                    } label: {
                        HStack(spacing: 4) {
                            Image(systemName: "plus")
                            Text("Add Files")
                        }
                        .font(.system(size: 11, weight: .medium, design: .monospaced))
                        .padding(.horizontal, 10)
                        .padding(.vertical, 5)
                        .foregroundColor(.proAccent)
                        .background(Color.proAccent.opacity(0.1))
                        .cornerRadius(4)
                    }
                    .buttonStyle(.plain)

                    Spacer()

                    if !playlist.isEmpty {
                        Button {
                            audioPlayer?.stop()
                            isPlayingFile = false
                            currentTrackIndex = nil
                            playlist.removeAll()
                            addLog("Playlist cleared", color: .proTextDim)
                        } label: {
                            Text("Clear")
                                .font(.system(size: 10, weight: .medium, design: .monospaced))
                                .foregroundColor(.proTextDim)
                        }
                        .buttonStyle(.plain)
                    }

                    Text("\(playlist.count) tracks")
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundColor(.proTextDim)
                }

                // Playlist tracks
                if !playlist.isEmpty {
                    ScrollView {
                        VStack(spacing: 2) {
                            ForEach(Array(playlist.enumerated()), id: \.element.id) { index, track in
                                HStack(spacing: 8) {
                                    if currentTrackIndex == index && isPlayingFile {
                                        Image(systemName: "speaker.wave.2.fill")
                                            .font(.system(size: 10))
                                            .foregroundColor(.proAccent)
                                            .frame(width: 16)
                                    } else {
                                        Text("\(index + 1)")
                                            .font(.system(size: 10, design: .monospaced))
                                            .foregroundColor(.proTextDim)
                                            .frame(width: 16)
                                    }

                                    Text(track.name)
                                        .font(.system(size: 12, design: .monospaced))
                                        .foregroundColor(currentTrackIndex == index ? .proAccent : .proText)
                                        .lineLimit(1)

                                    Spacer()

                                    // Play this track
                                    Button { playTrack(at: index) } label: {
                                        Image(systemName: "play.fill")
                                            .font(.system(size: 9))
                                            .foregroundColor(.proTextDim)
                                    }
                                    .buttonStyle(.plain)

                                    // Remove
                                    Button {
                                        if currentTrackIndex == index {
                                            audioPlayer?.stop()
                                            isPlayingFile = false
                                            currentTrackIndex = nil
                                        } else if let cur = currentTrackIndex, index < cur {
                                            currentTrackIndex = cur - 1
                                        }
                                        playlist.remove(at: index)
                                    } label: {
                                        Image(systemName: "xmark")
                                            .font(.system(size: 9))
                                            .foregroundColor(.proTextDim)
                                    }
                                    .buttonStyle(.plain)
                                }
                                .padding(.horizontal, 8)
                                .padding(.vertical, 4)
                                .background(currentTrackIndex == index ? Color.proAccent.opacity(0.08) : Color.clear)
                                .cornerRadius(3)
                                .onTapGesture(count: 2) { playTrack(at: index) }
                            }
                        }
                    }
                    .frame(maxHeight: 150)

                    // Progress bar (when playing)
                    if isPlayingFile, let player = audioPlayer, player.duration > 0 {
                        VStack(spacing: 4) {
                            GeometryReader { geo in
                                ZStack(alignment: .leading) {
                                    RoundedRectangle(cornerRadius: 2)
                                        .fill(Color.proBg)
                                    RoundedRectangle(cornerRadius: 2)
                                        .fill(Color.proAccent)
                                        .frame(width: max(0, CGFloat(filePlaybackProgress) * geo.size.width))
                                }
                            }
                            .frame(height: 4)

                            HStack {
                                Text(formatTime(player.currentTime))
                                    .font(.system(size: 10, design: .monospaced))
                                    .foregroundColor(.proTextDim)
                                Spacer()
                                Text(formatTime(player.duration))
                                    .font(.system(size: 10, design: .monospaced))
                                    .foregroundColor(.proTextDim)
                            }
                        }
                    }

                    // Transport controls
                    HStack(spacing: 16) {
                        Button { previousTrack() } label: {
                            Image(systemName: "backward.fill").font(.system(size: 12)).foregroundColor(.proText)
                        }.buttonStyle(.plain)

                        Button { toggleFilePlayback() } label: {
                            Image(systemName: isPlayingFile ? "pause.fill" : "play.fill")
                                .font(.system(size: 14))
                                .foregroundColor(.proAccent)
                        }.buttonStyle(.plain)

                        Button { nextTrack() } label: {
                            Image(systemName: "forward.fill").font(.system(size: 12)).foregroundColor(.proText)
                        }.buttonStyle(.plain)

                        Spacer()

                        // Now playing name
                        if isPlayingFile, let idx = currentTrackIndex, idx < playlist.count {
                            Text(playlist[idx].name)
                                .font(.system(size: 10, design: .monospaced))
                                .foregroundColor(.proTextDim)
                                .lineLimit(1)
                        }
                    }
                }
            }
        }
        .onDrop(of: [.fileURL], isTargeted: nil) { providers in
            for provider in providers {
                _ = provider.loadObject(ofClass: URL.self) { url, _ in
                    if let url = url {
                        DispatchQueue.main.async { addToPlaylist(url) }
                    }
                }
            }
            return true
        }
    }

    // MARK: - File Playback Methods

    private func addToPlaylist(_ url: URL) {
        let audioExtensions = Set(["mp3", "wav", "m4a", "aac", "aiff", "flac"])
        if url.hasDirectoryPath {
            let fm = FileManager.default
            if let files = try? fm.contentsOfDirectory(at: url, includingPropertiesForKeys: nil) {
                for file in files.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
                    if audioExtensions.contains(file.pathExtension.lowercased()) {
                        playlist.append((id: UUID(), url: file, name: file.lastPathComponent))
                    }
                }
            }
        } else if audioExtensions.contains(url.pathExtension.lowercased()) {
            playlist.append((id: UUID(), url: url, name: url.lastPathComponent))
        }
    }

    private func playTrack(at index: Int) {
        guard index >= 0 && index < playlist.count else { return }
        audioPlayer?.stop()
        currentTrackIndex = index
        let url = playlist[index].url
        do {
            audioPlayer = try AVAudioPlayer(contentsOf: url)
            audioPlayer?.play()
            isPlayingFile = true
            filePlaybackProgress = 0
            addLog("Playing: \(playlist[index].name)", color: .proAccent)
        } catch {
            addLog("Playback error: \(error.localizedDescription)", color: .proRed)
            isPlayingFile = false
        }
    }

    private func toggleFilePlayback() {
        if isPlayingFile {
            audioPlayer?.pause()
            isPlayingFile = false
        } else if let idx = currentTrackIndex {
            if audioPlayer != nil {
                audioPlayer?.play()
                isPlayingFile = true
            } else {
                playTrack(at: idx)
            }
        } else if !playlist.isEmpty {
            playTrack(at: 0)
        }
    }

    private func nextTrack() {
        guard !playlist.isEmpty else { return }
        let next = ((currentTrackIndex ?? -1) + 1) % playlist.count
        playTrack(at: next)
    }

    private func previousTrack() {
        guard !playlist.isEmpty else { return }
        // If more than 3 seconds into the track, restart it; otherwise go to previous
        if let player = audioPlayer, player.currentTime > 3.0 {
            player.currentTime = 0
            return
        }
        let prev = ((currentTrackIndex ?? 1) - 1 + playlist.count) % playlist.count
        playTrack(at: prev)
    }

    private func checkFilePlayback() {
        guard isPlayingFile, let player = audioPlayer else { return }
        if player.duration > 0 {
            filePlaybackProgress = player.currentTime / player.duration
        }
        // Auto-advance when track finishes
        if !player.isPlaying && isPlayingFile {
            nextTrack()
        }
    }

    private func formatTime(_ time: TimeInterval) -> String {
        let m = Int(time) / 60
        let s = Int(time) % 60
        return String(format: "%d:%02d", m, s)
    }

    // MARK: - Devices

    private var devicesSection: some View {
        proCard(title: "DEVICES", icon: "laptopcomputer.and.iphone") {
            if registry.devices.isEmpty && receiver.groupMembers.isEmpty {
                HStack {
                    Text("No connected devices")
                        .font(.system(size: 12))
                        .foregroundColor(.proTextDim)
                    Spacer()
                    Button(action: { registry.refresh() }) {
                        Image(systemName: "arrow.clockwise")
                            .font(.system(size: 11))
                            .foregroundColor(.proAccent)
                    }
                    .buttonStyle(.plain)
                }
            } else {
                VStack(spacing: 6) {
                    // Show group members
                    ForEach(receiver.groupMembers) { member in
                        deviceRow(
                            name: member.name,
                            syncOffset: SDKAudioReceiver.shared.syncOffsetMs,
                            status: .synced
                        )
                    }
                    // Show global devices
                    ForEach(registry.devices) { device in
                        deviceRow(
                            name: device.name,
                            syncOffset: 0,
                            status: .synced
                        )
                    }
                }
            }
        }
    }

    private enum DeviceStatus {
        case synced, syncing, lost
        var color: Color {
            switch self {
            case .synced: return .proGreen
            case .syncing: return .proYellow
            case .lost: return .proRed
            }
        }
        var label: String {
            switch self {
            case .synced: return "Synced"
            case .syncing: return "Syncing"
            case .lost: return "Lost"
            }
        }
    }

    private func deviceRow(name: String, syncOffset: Double, status: DeviceStatus) -> some View {
        HStack(spacing: 10) {
            Circle()
                .fill(status.color)
                .frame(width: 8, height: 8)
            Text(name)
                .font(.system(size: 12, weight: .medium))
                .foregroundColor(.proText)
                .lineLimit(1)
            Spacer()
            Text(String(format: "%.1fms", syncOffset))
                .font(.system(size: 11, design: .monospaced))
                .foregroundColor(.proTextDim)
            Text(status.label)
                .font(.system(size: 10, weight: .medium, design: .monospaced))
                .foregroundColor(status.color)
                .padding(.horizontal, 6)
                .padding(.vertical, 2)
                .background(status.color.opacity(0.12))
                .cornerRadius(3)
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 5)
        .background(Color.proBg.opacity(0.5))
        .cornerRadius(4)
    }

    // MARK: - Right Sidebar

    private var rightSidebar: some View {
        ScrollView {
            VStack(spacing: 16) {
                eqSection
                compressorSection
                latencySection
                eventLogSection
            }
            .padding(16)
        }
        .background(Color.proCard.opacity(0.3))
    }

    // MARK: - EQ Visualization

    private var eqSection: some View {
        proCard(title: "MASTER EQ", icon: "slider.vertical.3") {
            HStack(alignment: .bottom, spacing: 4) {
                ForEach(0..<10, id: \.self) { i in
                    VStack(spacing: 4) {
                        // Bar
                        RoundedRectangle(cornerRadius: 2)
                            .fill(
                                LinearGradient(
                                    colors: [.proAccent, .proAccent.opacity(0.4)],
                                    startPoint: .top, endPoint: .bottom
                                )
                            )
                            .frame(width: 20, height: CGFloat(eqBands[i]) * 80)
                            .animation(.easeInOut(duration: 0.3), value: eqBands[i])

                        // Frequency label
                        Text(eqFrequencies[i])
                            .font(.system(size: 8, weight: .medium, design: .monospaced))
                            .foregroundColor(.proTextDim)
                    }
                }
            }
            .frame(height: 100)
        }
    }

    // MARK: - Compressor / Limiter

    private var compressorSection: some View {
        proCard(title: "COMPRESSOR / LIMITER", icon: "waveform.path.ecg") {
            VStack(spacing: 8) {
                // Peak level
                HStack {
                    Text("Peak")
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundColor(.proTextDim)
                    Spacer()
                    Text(String(format: "%.1f dB", peakLevel))
                        .font(.system(size: 12, weight: .bold, design: .monospaced))
                        .foregroundColor(peakLevel > -3 ? .proRed : peakLevel > -6 ? .proYellow : .proGreen)
                }

                // Level meter
                GeometryReader { geo in
                    ZStack(alignment: .leading) {
                        RoundedRectangle(cornerRadius: 3)
                            .fill(Color.proBg)
                        let normalized = max(0, min(1, (peakLevel + 60) / 60))
                        RoundedRectangle(cornerRadius: 3)
                            .fill(
                                LinearGradient(
                                    colors: [.proGreen, .proYellow, .proRed],
                                    startPoint: .leading, endPoint: .trailing
                                )
                            )
                            .frame(width: CGFloat(normalized) * geo.size.width)
                    }
                }
                .frame(height: 12)

                // Gate threshold
                HStack {
                    Text("Gate")
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundColor(.proTextDim)
                    Spacer()
                    Text("-40 dB")
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundColor(.proTextDim)
                    Rectangle()
                        .fill(Color.proAccent.opacity(0.4))
                        .frame(width: 40, height: 2)
                }
            }
        }
    }

    // MARK: - Latency Monitor

    private var latencySection: some View {
        proCard(title: "LATENCY MONITOR", icon: "network") {
            VStack(spacing: 8) {
                latencyRow(label: "Sync Offset",
                           value: String(format: "%.1f ms", SDKAudioReceiver.shared.syncOffsetMs),
                           color: abs(SDKAudioReceiver.shared.syncOffsetMs) < 5 ? .proGreen : .proYellow)

                latencyRow(label: "Buffer Fill",
                           value: "\(SDKAudioReceiver.shared.bufferFillMs) ms (\(bufferPercent)%)",
                           color: bufferPercent > 50 ? .proGreen : .proYellow)

                latencyRow(label: "Packets/sec",
                           value: "\(SDKAudioReceiver.shared.packetsPerSec)",
                           color: SDKAudioReceiver.shared.packetsPerSec > 100 ? .proGreen : .proRed)

                latencyRow(label: "Jitter",
                           value: String(format: "%.1f ms", jitterMs),
                           color: jitterMs < 5 ? .proGreen : .proYellow)

                latencyRow(label: "Output Latency",
                           value: String(format: "%.1f ms", SDKAudioReceiver.shared.outputLatencyMs),
                           color: SDKAudioReceiver.shared.latencyExceeded ? .proRed : .proGreen)
            }
        }
    }

    private func latencyRow(label: String, value: String, color: Color) -> some View {
        HStack {
            Text(label)
                .font(.system(size: 11, design: .monospaced))
                .foregroundColor(.proTextDim)
            Spacer()
            Text(value)
                .font(.system(size: 12, weight: .semibold, design: .monospaced))
                .foregroundColor(color)
        }
    }

    // MARK: - Event Log

    private var eventLogSection: some View {
        proCard(title: "EVENT LOG", icon: "list.bullet.rectangle") {
            if logEntries.isEmpty {
                Text("No events yet")
                    .font(.system(size: 11))
                    .foregroundColor(.proTextDim)
            } else {
                VStack(spacing: 3) {
                    ForEach(logEntries.suffix(20).reversed(), id: \.id) { entry in
                        HStack(spacing: 8) {
                            Text(timeFormatter.string(from: entry.date))
                                .font(.system(size: 10, design: .monospaced))
                                .foregroundColor(.proTextDim)
                            Circle()
                                .fill(entry.color)
                                .frame(width: 5, height: 5)
                            Text(entry.message)
                                .font(.system(size: 11))
                                .foregroundColor(.proText)
                                .lineLimit(1)
                            Spacer()
                        }
                    }
                }
            }
        }
    }

    // MARK: - Card Container

    @ViewBuilder
    private func proCard<Content: View>(title: String, icon: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 6) {
                Image(systemName: icon)
                    .font(.system(size: 10))
                    .foregroundColor(.proAccent)
                Text(title)
                    .font(.system(size: 11, weight: .bold, design: .monospaced))
                    .foregroundColor(.proAccent)
            }

            content()
        }
        .padding(12)
        .background(Color.proCard)
        .cornerRadius(8)
        .overlay(
            RoundedRectangle(cornerRadius: 8)
                .stroke(Color.proBorder, lineWidth: 0.5)
        )
    }

    // MARK: - Helpers

    private var formattedElapsed: String {
        let elapsed = Date().timeIntervalSince(startTime)
        let h = Int(elapsed) / 3600
        let m = (Int(elapsed) % 3600) / 60
        let s = Int(elapsed) % 60
        return String(format: "%02d:%02d:%02d", h, m, s)
    }

    private var bufferPercent: Int {
        // Buffer capacity is ~4000ms (192000 samples / 48000 Hz)
        let fill = SDKAudioReceiver.shared.bufferFillMs
        return min(100, fill * 100 / max(1, 4000))
    }

    private let timeFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss"
        return f
    }()

    private func addLog(_ message: String, color: Color) {
        logEntries.append((id: UUID(), date: Date(), message: message, color: color))
        if logEntries.count > 100 {
            logEntries.removeFirst(logEntries.count - 100)
        }
    }

    private func updateVU() {
        let sdk = SDKAudioReceiver.shared
        let rawL = sdk.outputLevelL
        let rawR = sdk.outputLevelR

        if rawL > 0.001 || rawR > 0.001 {
            // Real audio levels from engine tap
            vuLeft = vuLeft * 0.3 + rawL * 0.7
            vuRight = vuRight * 0.3 + rawR * 0.7

            // EQ bands: distribute level with frequency weighting + variation
            let base = max(rawL, rawR)
            let freqWeights: [Float] = [0.5, 0.6, 0.75, 0.9, 1.0, 0.95, 0.8, 0.65, 0.5, 0.4]
            for i in 0..<eqBands.count {
                let target = base * freqWeights[i] * Float.random(in: 0.6...1.0)
                eqBands[i] = eqBands[i] * 0.5 + target * 0.5
            }
        } else {
            // Fade out
            vuLeft *= 0.85
            vuRight *= 0.85
            for i in 0..<eqBands.count { eqBands[i] *= 0.88 }
        }

        let maxVu = max(vuLeft, vuRight)
        peakLevel = maxVu > 0.001 ? 20 * log10(maxVu) : -60
    }

    private func updateStats() {
        let sdk = SDKAudioReceiver.shared
        isStreaming = sdk.isPlaying

        // Estimate jitter from sync offset changes
        let currentOffset = abs(sdk.syncOffsetMs)
        jitterMs = jitterMs * 0.8 + currentOffset * 0.2

        // Log state changes
        if sdk.state == .receiving && !isStreaming {
            addLog("Receiving audio", color: .proGreen)
        }
    }

    private func iconForSourceType(_ type: AudioSourceManager.AudioSource.SourceType?) -> String {
        switch type {
        case .microphone:  return "mic.fill"
        case .systemAudio: return "desktopcomputer"
        case .inputDevice: return "waveform.path"
        case nil:          return "questionmark.circle"
        }
    }

    private func switchToNextChannel() {}
    private func switchToPreviousChannel() {}
}
