//
//  DJModeView.swift
//  SolunaReceiverMac
//
//  UI for multi-channel DJ mode — route different audio sources
//  to different Soluna channels simultaneously.
//

import SwiftUI

struct DJModeView: View {
    @StateObject private var config = MultiChannelConfig()
    @State private var newChannel: String = ""
    @State private var newSource: String = "system"

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Label("DJ Mode — Multi-Channel Routing", systemImage: "music.mic")
                .font(.headline)

            Text("Route different audio sources to separate Soluna channels.")
                .font(.caption)
                .foregroundColor(.secondary)

            // Existing routes
            if config.routes.isEmpty {
                Text("No routes configured. Add one below.")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .padding(.vertical, 8)
            } else {
                List {
                    ForEach(config.routes) { route in
                        HStack {
                            Image(systemName: route.isEnabled ? "antenna.radiowaves.left.and.right" : "antenna.radiowaves.left.and.right.slash")
                                .foregroundColor(route.isEnabled ? .green : .gray)

                            VStack(alignment: .leading) {
                                Text(route.channelName)
                                    .font(.body.weight(.medium))
                                Text(route.audioSource == "system" ? "System Audio" : route.audioSource)
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                            }

                            Spacer()

                            Toggle("", isOn: Binding(
                                get: { route.isEnabled },
                                set: { newValue in
                                    if let idx = config.routes.firstIndex(where: { $0.id == route.id }) {
                                        config.routes[idx].isEnabled = newValue
                                        config.save()
                                    }
                                }
                            ))
                            .labelsHidden()
                        }
                    }
                    .onDelete { config.removeRoute(at: $0) }
                }
                .frame(minHeight: 100, maxHeight: 250)
            }

            Divider()

            // Add new route
            HStack {
                TextField("Channel name", text: $newChannel)
                    .textFieldStyle(.roundedBorder)
                    .frame(maxWidth: 150)

                Picker("Source", selection: $newSource) {
                    Text("System Audio").tag("system")
                    Text("BlackHole 2ch").tag("BlackHole2ch_UID")
                }
                .frame(maxWidth: 180)

                Button("Add") {
                    guard !newChannel.isEmpty else { return }
                    config.addRoute(channel: newChannel, source: newSource)
                    newChannel = ""
                }
                .disabled(newChannel.isEmpty)
            }
        }
        .padding()
    }
}
