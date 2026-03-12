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
    @AppStorage("channels") private var channels = 2
    @AppStorage("channel") private var channel = "soluna"
    @AppStorage("streamMode") private var streamMode = "sync"
    @State private var tempMulticastGroup: String = ""
    @State private var tempPort: String = ""
    @State private var tempChannels: Int = 1
    @State private var tempChannel: String = ""
    @State private var showingResetAlert = false
    @State private var showChannelPurchase = false

    var body: some View {
        NavigationView {
            Form {
                Section(header: Text("Network"),
                        footer: Text("Default: 239.69.0.1:5004 (Soluna multicast)")) {
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
                }

                Section(header: Text("Channel"),
                        footer: Text("Nearby devices on the same channel share audio automatically.")) {
                    HStack {
                        Image(systemName: "dot.radiowaves.left.and.right")
                            .foregroundColor(.purple)
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

                Section(header: Text("Audio")) {
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
                    }
                }

                Section(header: Text("Stream Mode")) {
                    Picker(selection: $streamMode, label:
                        HStack {
                            Image(systemName: "waveform.path")
                                .foregroundColor(.purple)
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

                Section {
                    VStack(alignment: .leading, spacing: 8) {
                        Text("About Soluna Rx")
                            .font(.headline)
                        Text("Version 1.0.0")
                            .font(.subheadline)
                            .foregroundColor(.secondary)
                        Text("Receives network audio via RTP multicast. Supports OSTP and AES67 protocols.")
                            .font(.caption)
                            .foregroundColor(.secondary)

                        Link(destination: URL(string: "https://github.com/yukihamada/opensonic")!) {
                            HStack {
                                Image(systemName: "link")
                                Text("View on GitHub")
                            }
                            .font(.caption)
                        }
                        .padding(.top, 4)
                    }
                    .padding(.vertical, 4)
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
            .onAppear {
                loadCurrentSettings()
            }
        }
    }

    private func loadCurrentSettings() {
        tempMulticastGroup = multicastGroup
        tempPort = String(port)
        tempChannels = channels
        // Use purchased channel name if available
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

        // Haptic feedback
        let generator = UINotificationFeedbackGenerator()
        generator.notificationOccurred(.success)
    }

    private func resetToDefaults() {
        tempMulticastGroup = "239.69.0.1"
        tempPort = "5004"
        tempChannels = 2
        tempChannel = "soluna"
        streamMode = "sync"
        // Haptic feedback
        let generator = UIImpactFeedbackGenerator(style: .medium)
        generator.impactOccurred()
    }
}

#Preview {
    SettingsView(receiver: AudioReceiver())
}
