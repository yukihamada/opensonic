//
//  SettingsView.swift
//  SolunaReceiver
//
//  Settings screen for configuring audio reception
//

import SwiftUI

struct SettingsView: View {
    @ObservedObject var receiver: AudioReceiver
    @Environment(\.presentationMode) private var presentationMode

    @AppStorage("multicastGroup") private var multicastGroup = "239.69.0.1"
    @AppStorage("port") private var port = 5004
    @AppStorage("channels") private var channels = 1
    @AppStorage("autoConnect") private var autoConnect = false
    @State private var tempMulticastGroup: String = ""
    @State private var tempPort: String = ""
    @State private var tempChannels: Int = 1
    @State private var showingResetAlert = false

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

                Section(
                    header: Text("Mac Control"),
                    footer: Text("IP address of the Mac running solunad. Enables speaker and buffer controls.")
                ) {
                    HStack {
                        Image(systemName: "desktopcomputer")
                            .foregroundColor(.purple)
                            .frame(width: 28)
                        TextField("Mac IP (e.g. 192.168.1.10)", text: $tempMacHost)
                            .keyboardType(.decimalPad)
                            .autocapitalization(.none)
                            .disableAutocorrection(true)
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
        tempMacHost = macHost
    }

    private func saveSettings() {
        multicastGroup = tempMulticastGroup.isEmpty ? "239.69.0.1" : tempMulticastGroup
        port = Int(tempPort) ?? 5004
        channels = tempChannels
        macHost = tempMacHost

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
        tempChannels = 1
        tempMacHost = ""
        autoConnect = false

        // Haptic feedback
        let generator = UIImpactFeedbackGenerator(style: .medium)
        generator.impactOccurred()
    }
}

#Preview {
    SettingsView(receiver: AudioReceiver())
}
