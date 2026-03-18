//
//  SettingsView.swift
//  SolunaReceiver
//
//  Settings screen for configuring audio reception
//

import SwiftUI

struct SettingsView: View {
    @ObservedObject var receiver: AudioReceiver
    @StateObject private var channelStore = ChannelStore()
    @Environment(\.presentationMode) private var presentationMode

    @AppStorage("multicastGroup") private var multicastGroup = "239.69.0.1"
    @AppStorage("port") private var port = 5004
    @AppStorage("channels") private var channels = 1
    @AppStorage("channel") private var channel = "soluna"
    @AppStorage("streamMode") private var streamMode = "sync"
    @AppStorage("receiveMode") private var receiveMode = "auto"
    @AppStorage("fileSyncEnabled") private var fileSyncEnabled = true
    @AppStorage("preferStereo") private var preferStereo = true
    @AppStorage("connectMode") private var connectMode = true
    @AppStorage("autoRecordEnabled") private var autoRecordEnabled = false
    @AppStorage("autoTranscribeEnabled") private var autoTranscribeEnabled = false
    @AppStorage("relayHost") private var relayHost = ""
    @State private var tempMulticastGroup: String = ""
    @State private var tempPort: String = ""
    @State private var tempChannels: Int = 1
    @State private var tempChannel: String = ""
    @State private var tempRelayHost: String = ""
    @State private var showingResetAlert = false
    @State private var showChannelPurchase = false
    @State private var showLogin = false
    @StateObject private var auth = AuthManager.shared

    var body: some View {
        NavigationView {
            Form {
                // MARK: - Essential

                Section(header: Text("Channel"),
                        footer: Text("Nearby devices on the same channel share audio automatically.")) {
                    HStack {
                        Image(systemName: "dot.radiowaves.left.and.right")
                            .foregroundColor(.solunaGradientStart)
                            .frame(width: 28)
                        Text(tempChannel.isEmpty ? "soluna" : tempChannel)
                            .font(.body)
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
                    }
                }
                .sheet(isPresented: $showChannelPurchase) {
                    ChannelPurchaseView(store: channelStore, activeChannel: $tempChannel)
                }

                Section(header: Text("Connection"),
                        footer: Text("When enabled, automatically connects to the last-used channel on launch and reconnects if disconnected.")) {
                    Toggle(isOn: $connectMode) {
                        HStack {
                            Image(systemName: "play.circle")
                                .foregroundColor(.solunaGradientStart)
                                .frame(width: 28)
                            Text("Connect Mode")
                        }
                    }
                }

                Section(header: Text("Output"),
                        footer: Text("Mono uses less CPU. Surround requires matching TX source.")) {
                    Picker(selection: $tempChannels, label:
                        HStack {
                            Image(systemName: "speaker.wave.2")
                                .foregroundColor(.orange)
                                .frame(width: 28)
                            Text("Output Channels")
                        }
                    ) {
                        Text("Mono (1ch)").tag(1)
                        Text("Stereo (2ch)").tag(2)
                        Text("5.1 Surround (6ch)").tag(6)
                        Text("7.1 Surround (8ch)").tag(8)
                    }
                }

                Section(header: Text("Stream Mode")) {
                    Picker(selection: $streamMode, label:
                        HStack {
                            Image(systemName: "waveform.path")
                                .foregroundColor(.solunaGradientStart)
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

                Section(header: Text("Receive"),
                        footer: Text("When enabled, music is downloaded and cached locally for better quality. Turn off to use real-time streaming only.")) {
                    Toggle(isOn: $fileSyncEnabled) {
                        HStack {
                            Image(systemName: "arrow.down.circle.fill")
                                .foregroundColor(.solunaGradientEnd)
                                .frame(width: 28)
                            Text("Download & Play")
                        }
                    }

                    Toggle(isOn: $preferStereo) {
                        HStack {
                            Image(systemName: "ear")
                                .foregroundColor(.solunaGradientMid)
                                .frame(width: 28)
                            Text("Prefer Stereo")
                        }
                    }
                }

                Section(header: Text("Recording & Transcription"),
                        footer: Text("Auto Record saves received audio to the Documents folder. Auto Transcribe uses Apple Speech to transcribe received audio in real time.")) {
                    Toggle(isOn: $autoRecordEnabled) {
                        HStack {
                            Image(systemName: "record.circle")
                                .foregroundColor(.red)
                                .frame(width: 28)
                            Text("Auto Record")
                        }
                    }

                    Toggle(isOn: $autoTranscribeEnabled) {
                        HStack {
                            Image(systemName: "text.bubble")
                                .foregroundColor(.blue)
                                .frame(width: 28)
                            Text("Auto Transcribe")
                        }
                    }
                }

                // MARK: - Advanced

                Section {
                    DisclosureGroup("Advanced") {
                        HStack {
                            Image(systemName: "network")
                                .foregroundColor(.blue)
                                .frame(width: 28)
                            TextField("Multicast Group", text: $tempMulticastGroup)
                                .keyboardType(.decimalPad)
                                .autocapitalization(.none)
                        }

                        HStack {
                            Image(systemName: "antenna.radiowaves.left.and.right")
                                .foregroundColor(.green)
                                .frame(width: 28)
                            TextField("Port", text: $tempPort)
                                .keyboardType(.numberPad)
                        }

                        Text("Default: 239.69.0.1:5004")
                            .font(.caption)
                            .foregroundColor(.secondary)

                        HStack {
                            Image(systemName: "laptopcomputer")
                                .foregroundColor(.solunaGradientStart)
                                .frame(width: 28)
                            TextField("Direct Relay (e.g. 192.168.0.194)", text: $tempRelayHost)
                                .keyboardType(.decimalPad)
                                .autocapitalization(.none)
                        }

                        Text("Enter Mac IP for direct connection. Leave empty for auto-discovery.")
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
                        }
                        Button {
                            auth.logout()
                        } label: {
                            HStack {
                                Image(systemName: "rectangle.portrait.and.arrow.right")
                                    .frame(width: 28)
                                Text("Logout")
                            }
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
                                Spacer()
                                Image(systemName: "chevron.right")
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                            }
                        }
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
                }
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .navigationBarItems(
                leading: Button("Cancel") {
                    presentationMode.wrappedValue.dismiss()
                },
                trailing: Button(action: {
                    saveSettings()
                    presentationMode.wrappedValue.dismiss()
                }) {
                    Text("Save")
                        .fontWeight(.semibold)
                }
            )
            .alert(isPresented: $showingResetAlert) {
                Alert(
                    title: Text("Reset Settings"),
                    message: Text("This will reset all settings to their default values."),
                    primaryButton: .destructive(Text("Reset")) {
                        resetToDefaults()
                    },
                    secondaryButton: .cancel()
                )
            }
            .sheet(isPresented: $showLogin) {
                EmailLoginView(auth: auth)
            }
            .onAppear {
                loadCurrentSettings()
            }
        }
    }

    private func loadCurrentSettings() {
        tempMulticastGroup = multicastGroup
        tempPort = String(port)
        tempChannels = channels
        tempRelayHost = relayHost
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
        relayHost = tempRelayHost.trimmingCharacters(in: .whitespaces)

        receiver.multicastGroup = multicastGroup
        receiver.port = UInt16(port)
        receiver.channels = UInt32(channels)

        let generator = UINotificationFeedbackGenerator()
        generator.notificationOccurred(.success)
    }

    private func resetToDefaults() {
        tempMulticastGroup = "239.69.0.1"
        tempPort = "5004"
        tempChannels = 1
        tempChannel = "soluna"
        tempRelayHost = ""
        streamMode = "sync"
        connectMode = true
        fileSyncEnabled = true
        preferStereo = true

        let generator = UIImpactFeedbackGenerator(style: .medium)
        generator.impactOccurred()
    }
}

#Preview {
    SettingsView(receiver: AudioReceiver())
}
