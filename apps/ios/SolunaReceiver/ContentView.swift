//
//  ContentView.swift
//  SolunaReceiver
//
//  Main UI for Soluna network audio receiver
//

import SwiftUI

struct ContentView: View {
    @StateObject private var receiver = AudioReceiver()
    @StateObject private var daemon   = DaemonClient()
    @State private var showingSettings = false
    @State private var showingError = false

    @AppStorage("autoConnect") private var autoConnect = false
    @AppStorage("macHost")     private var macHost     = ""

    var body: some View {
        NavigationView {
            VStack(spacing: 24) {
                Spacer()

                // Status indicator
                VStack(spacing: 16) {
                    ZStack {
                        Circle()
                            .fill(statusGradient)
                            .frame(width: 160, height: 160)
                            .shadow(color: statusColor.opacity(0.5), radius: 20)

                        Circle()
                            .fill(Color(.systemBackground))
                            .frame(width: 140, height: 140)

                        // Play/Stop button
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

                    // Status text
                    HStack(spacing: 8) {
                        Circle()
                            .fill(statusColor)
                            .frame(width: 8, height: 8)

                        Text(receiver.state.rawValue)
                            .font(.headline)
                            .foregroundColor(.secondary)
                    }

                    // Tap to retry hint in error state
                    if receiver.state == .error {
                        Text("Tap to retry")
                            .font(.caption)
                            .foregroundColor(.secondary)
                            .padding(.top, 4)
                    }
                }

                // Stats card
                if receiver.state == .receiving || receiver.packetsReceived > 0 {
                    statsCard
                        .transition(.move(edge: .bottom).combined(with: .opacity))
                }

                Spacer()

                // Volume control
                volumeControl

                // Mac speaker controls (when macHost is configured)
                if !macHost.isEmpty {
                    macControls
                        .transition(.move(edge: .bottom).combined(with: .opacity))
                }

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
            .alert(isPresented: $showingError) {
                Alert(
                    title: Text("Connection Error"),
                    message: Text(receiver.errorMessage ?? "Failed to connect to audio stream."),
                    primaryButton: .default(Text("Settings")) {
                        showingSettings = true
                    },
                    secondaryButton: .cancel()
                )
            }
            .onChange(of: receiver.errorMessage) { newValue in
                if newValue != nil {
                    showingError = true
                }
            }
            .onAppear {
                loadSavedSettings()
                if autoConnect {
                    receiver.start()
                }
            }
        }
    }

    // MARK: - Subviews

    private var statsCard: some View {
        VStack(spacing: 12) {
            HStack(spacing: 24) {
                statItem(
                    value: formatNumber(receiver.packetsReceived),
                    label: "Received",
                    color: .green
                )

                if receiver.packetsDropped > 0 {
                    statItem(
                        value: formatNumber(receiver.packetsDropped),
                        label: "Dropped",
                        color: .orange
                    )
                }
            }

            // Connection info
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

    private var volumeControl: some View {
        VStack(spacing: 10) {
            HStack(spacing: 12) {
                // Mute toggle (tap the speaker icon)
                Button(action: {
                    let g = UIImpactFeedbackGenerator(style: .light)
                    g.impactOccurred()
                    receiver.isMuted.toggle()
                }) {
                    Image(systemName: receiver.isMuted ? "speaker.slash.fill" : volumeIcon)
                        .foregroundColor(receiver.isMuted ? .red : .secondary)
                        .frame(width: 28)
                        .contentShape(Rectangle())
                }

                // Volume -10%
                Button(action: {
                    let g = UIImpactFeedbackGenerator(style: .light)
                    g.impactOccurred()
                    if receiver.isMuted { receiver.isMuted = false }
                    receiver.volume = max(0, receiver.volume - 0.1)
                }) {
                    Image(systemName: "minus.circle.fill")
                        .font(.title3)
                        .foregroundColor(.secondary)
                }

                Slider(value: $receiver.volume, in: 0...1)
                    .accentColor(receiver.isMuted ? .gray : statusColor)
                    .onChange(of: receiver.volume) { _ in
                        if receiver.isMuted { receiver.isMuted = false }
                    }

                // Volume +10%
                Button(action: {
                    let g = UIImpactFeedbackGenerator(style: .light)
                    g.impactOccurred()
                    if receiver.isMuted { receiver.isMuted = false }
                    receiver.volume = min(1, receiver.volume + 0.1)
                }) {
                    Image(systemName: "plus.circle.fill")
                        .font(.title3)
                        .foregroundColor(.secondary)
                }

                Text(receiver.isMuted ? "Mute" : "\(Int(receiver.volume * 100))%")
                    .font(.system(.caption, design: .monospaced))
                    .foregroundColor(receiver.isMuted ? .red : .secondary)
                    .frame(width: 44)
            }
        }
        .padding(.horizontal, 24)
        .padding(.bottom, 24)
    }

    // MARK: - Computed Properties

    private var statusColor: Color {
        switch receiver.state {
        case .receiving: return .green
        case .connecting: return .orange
        case .error: return .red
        case .stopped: return .gray
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
        case .receiving: return "stop.fill"
        case .connecting: return "ellipsis"
        case .error: return "arrow.clockwise"  // Retry icon
        case .stopped: return "play.fill"
        }
    }

    private var buttonColor: Color {
        switch receiver.state {
        case .receiving: return .red
        case .connecting: return .orange
        case .error: return .blue  // Blue to indicate tap to retry
        case .stopped: return .blue
        }
    }

    private var volumeIcon: String {
        if receiver.volume == 0 {
            return "speaker.slash.fill"
        } else if receiver.volume < 0.33 {
            return "speaker.fill"
        } else if receiver.volume < 0.66 {
            return "speaker.wave.1.fill"
        } else {
            return "speaker.wave.3.fill"
        }
    }

    // MARK: - Methods

    private func togglePlayback() {
        let generator = UIImpactFeedbackGenerator(style: .medium)
        generator.impactOccurred()

        // In error state, clear error and retry
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
        if port > 0 {
            receiver.port = UInt16(port)
        }
        let channels = defaults.integer(forKey: "channels")
        if channels > 0 {
            receiver.channels = UInt32(channels)
        }
    }

    private func formatNumber(_ value: UInt64) -> String {
        if value >= 1_000_000 {
            return String(format: "%.1fM", Double(value) / 1_000_000)
        } else if value >= 1_000 {
            return String(format: "%.1fK", Double(value) / 1_000)
        } else {
            return "\(value)"
        }
    }
}

#Preview {
    ContentView()
}
