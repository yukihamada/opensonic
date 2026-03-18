//
//  SettingsView.swift
//  SolunaReceiverMac
//
//  Settings screen for configuring audio reception
//

import SwiftUI

struct SettingsView: View {
    @ObservedObject var receiver: AudioReceiver
    @StateObject private var channelStore = ChannelStore()
    @Environment(\.dismiss) private var dismiss

    @AppStorage("multicastGroup") private var multicastGroup = "239.69.0.1"
    @AppStorage("port") private var port = 5004
    @AppStorage("channels") private var channels = 1
    @AppStorage("autoConnect") private var autoConnect = true
    @AppStorage("streamMode") private var streamMode = "sync"
    @AppStorage("channel") private var channel = "soluna"
    @State private var tempMulticastGroup: String = ""
    @State private var tempPort: String = ""
    @State private var tempChannels: Int = 1
    @State private var tempChannel: String = ""
    @State private var showingResetAlert = false
    @State private var showChannelPurchase = false
    @State private var showLogin = false
    @StateObject private var auth = AuthManager.shared

    var body: some View {
        VStack(spacing: 0) {
            Text("Settings")
                .font(.headline)
                .padding(.top, 16)
                .padding(.bottom, 8)

            Form {
                // MARK: - Essential

                Section(header: Text("Channel")) {
                    HStack {
                        Image(systemName: "dot.radiowaves.left.and.right")
                            .foregroundColor(.solunaGradientStart)
                            .frame(width: 28)
                        Text(tempChannel.isEmpty ? "soluna" : tempChannel)
                        Spacer()
                        if channelStore.isPurchased {
                            Text("Custom")
                                .font(.caption2.weight(.semibold))
                                .foregroundColor(.white)
                                .padding(.horizontal, 8)
                                .padding(.vertical, 3)
                                .background(Color.blue)
                                .clipShape(Capsule())
                        }
                    }

                    if channelStore.isPurchased {
                        Button {
                            showChannelPurchase = true
                        } label: {
                            Label("Change Channel Name", systemImage: "pencil")
                                .font(.subheadline)
                        }
                        .buttonStyle(.plain)
                    } else {
                        Button {
                            showChannelPurchase = true
                        } label: {
                            HStack {
                                Image(systemName: "star.fill")
                                    .foregroundColor(.yellow)
                                Text("Get Custom Channel")
                                    .font(.subheadline)
                                Spacer()
                                Image(systemName: "chevron.right")
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                            }
                        }
                        .buttonStyle(.plain)
                    }

                    Text("Nearby devices on the same channel share audio automatically.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                .sheet(isPresented: $showChannelPurchase) {
                    ChannelPurchaseView(store: channelStore, activeChannel: $tempChannel)
                        .frame(width: 400, height: 500)
                }

                Section(header: Text("Output Channels")) {
                    Picker(selection: $tempChannels, label:
                        HStack {
                            Image(systemName: "speaker.wave.2")
                                .foregroundColor(.orange)
                                .frame(width: 28)
                            Text("Channels")
                        }
                    ) {
                        Text("Mono (1)").tag(1)
                        Text("Stereo (2)").tag(2)
                        Text("5.1 Surround (6)").tag(6)
                        Text("7.1 Surround (8)").tag(8)
                    }

                    if tempChannels > 2 {
                        Text(channelLayoutDescription(tempChannels))
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                }

                Section(header: Text("Stream Mode")) {
                    Picker(selection: $streamMode, label:
                        HStack {
                            Image(systemName: "waveform.path")
                                .foregroundColor(.solunaGradientMid)
                                .frame(width: 28)
                            Text("Mode")
                        }
                    ) {
                        Text("Sync — Multi-room aligned").tag("sync")
                        Text("Jam — Low latency (~20ms)").tag("jam")
                    }

                    if streamMode == "sync" {
                        Text("All speakers play in perfect sync. Best for whole-home audio.")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    } else {
                        Text("Ultra-low latency for real-time jam sessions and collaboration.")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                }

                Section(header: Text("Connection")) {
                    Toggle(isOn: $autoConnect) {
                        HStack {
                            Image(systemName: "play.circle")
                                .foregroundColor(.solunaGradientStart)
                                .frame(width: 28)
                            Text("Connect Mode")
                        }
                    }

                    Text("Automatically connect on launch and reconnect if disconnected.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }

                // MARK: - Advanced

                Section {
                    DisclosureGroup("Advanced") {
                        HStack {
                            Image(systemName: "network")
                                .foregroundColor(.blue)
                                .frame(width: 28)
                            TextField("Multicast Group", text: $tempMulticastGroup)
                                .disableAutocorrection(true)
                        }

                        HStack {
                            Image(systemName: "antenna.radiowaves.left.and.right")
                                .foregroundColor(.green)
                                .frame(width: 28)
                            TextField("Port", text: $tempPort)
                        }

                        Text("Default: 239.69.0.1:5004")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                }

                // MARK: - Account

                Section(header: Text("Account")) {
                    if auth.isAuthenticated, let email = auth.userEmail {
                        HStack {
                            Image(systemName: "person.crop.circle.fill")
                                .foregroundColor(.solunaLive)
                                .frame(width: 28)
                            VStack(alignment: .leading, spacing: 2) {
                                Text(email)
                                    .font(.body)
                                Text("Device \(auth.shortDeviceId)")
                                    .font(.caption2.monospaced())
                                    .foregroundColor(.secondary)
                            }
                            Spacer()
                            Button("Logout") { auth.logout() }
                                .foregroundColor(.red)
                        }
                    } else {
                        Button {
                            showLogin = true
                        } label: {
                            HStack {
                                Image(systemName: "envelope.badge")
                                    .foregroundColor(.solunaGradientStart)
                                    .frame(width: 28)
                                Text("Login with Email")
                            }
                        }
                        .buttonStyle(.plain)
                    }
                }

                // MARK: - Reset

                Section {
                    Button(action: {
                        showingResetAlert = true
                    }) {
                        HStack {
                            Image(systemName: "arrow.counterclockwise")
                                .frame(width: 28)
                            Text("Reset to Defaults")
                        }
                        .foregroundColor(.red)
                    }
                    .buttonStyle(.plain)
                }
            }
            .formStyle(.grouped)

            HStack {
                Button("Cancel") {
                    dismiss()
                }
                .keyboardShortcut(.cancelAction)
                Spacer()
                Button("Save") {
                    saveSettings()
                    dismiss()
                }
                .keyboardShortcut(.defaultAction)
            }
            .padding(.horizontal, 16)
            .padding(.bottom, 16)
        }
        .frame(width: 420, height: 560)
        .alert("Reset Settings", isPresented: $showingResetAlert) {
            Button("Cancel", role: .cancel) {}
            Button("Reset", role: .destructive) {
                resetToDefaults()
            }
        } message: {
            Text("This will reset all settings to their default values.")
        }
        .sheet(isPresented: $showLogin) {
            EmailLoginView(auth: auth)
        }
        .onAppear {
            loadCurrentSettings()
        }
    }

    private func loadCurrentSettings() {
        tempMulticastGroup = multicastGroup
        tempPort = String(port)
        tempChannels = channels
        if let purchased = channelStore.purchasedChannelName, !purchased.isEmpty {
            tempChannel = purchased
        } else {
            tempChannel = channel
        }
    }

    private func saveSettings() {
        multicastGroup = tempMulticastGroup.isEmpty ? "239.69.0.1" : tempMulticastGroup
        port = Int(tempPort) ?? 5004
        channels = tempChannels
        channel = tempChannel.isEmpty ? "soluna" : tempChannel

        // Apply to receiver
        receiver.multicastGroup = multicastGroup
        receiver.port = UInt16(port)
        receiver.channels = UInt32(channels)
    }

    private func resetToDefaults() {
        tempMulticastGroup = "239.69.0.1"
        tempPort = "5004"
        tempChannels = 1
        tempChannel = "soluna"
        autoConnect = true
        streamMode = "sync"
    }

    private func channelLayoutDescription(_ ch: Int) -> String {
        switch ch {
        case 6:  return "FL · FR · C · LFE · SL · SR"
        case 8:  return "FL · FR · C · LFE · SL · SR · BL · BR"
        default: return "\(ch) channels"
        }
    }
}

#Preview {
    SettingsView(receiver: AudioReceiver())
}
