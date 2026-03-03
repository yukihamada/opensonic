//
//  SettingsView.swift
//  SolunaReceiverMac
//
//  Settings screen for configuring audio reception
//

import SwiftUI

struct SettingsView: View {
    @ObservedObject var receiver: AudioReceiver
    @Environment(\.dismiss) private var dismiss

    @AppStorage("multicastGroup") private var multicastGroup = "239.69.0.1"
    @AppStorage("port") private var port = 5004
    @AppStorage("channels") private var channels = 2
    @AppStorage("autoConnect") private var autoConnect = false
    @AppStorage("channel") private var channel = "soluna"
    @State private var tempMulticastGroup: String = ""
    @State private var tempPort: String = ""
    @State private var tempChannels: Int = 1
    @State private var tempChannel: String = ""
    @State private var showingResetAlert = false

    var body: some View {
        VStack(spacing: 0) {
            Text("Settings")
                .font(.headline)
                .padding(.top, 16)
                .padding(.bottom, 8)

            Form {
                Section(header: Text("Network")) {
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

                    Text("Default: 239.69.0.1:5004 (Soluna multicast)")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }

                Section(header: Text("Channel")) {
                    HStack {
                        Image(systemName: "dot.radiowaves.left.and.right")
                            .foregroundColor(.purple)
                            .frame(width: 28)
                        TextField("Channel name", text: $tempChannel)
                            .disableAutocorrection(true)
                    }

                    Text("Nearby devices on the same channel share audio automatically.")
                        .font(.caption)
                        .foregroundColor(.secondary)
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

                Section(header: Text("Behavior")) {
                    Toggle(isOn: $autoConnect) {
                        HStack {
                            Image(systemName: "play.circle")
                                .foregroundColor(.purple)
                                .frame(width: 28)
                            Text("Auto-connect on launch")
                        }
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
                    .buttonStyle(.plain)
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
        .frame(width: 420, height: 520)
        .alert("Reset Settings", isPresented: $showingResetAlert) {
            Button("Cancel", role: .cancel) {}
            Button("Reset", role: .destructive) {
                resetToDefaults()
            }
        } message: {
            Text("This will reset all settings to their default values.")
        }
        .onAppear {
            loadCurrentSettings()
        }
    }

    private func loadCurrentSettings() {
        tempMulticastGroup = multicastGroup
        tempPort = String(port)
        tempChannels = channels
        tempChannel = channel
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
        tempChannels = 2
        tempChannel = "soluna"
        autoConnect = false
    }
}

#Preview {
    SettingsView(receiver: AudioReceiver())
}
