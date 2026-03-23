//
//  ChannelSettingsView.swift
//  SolunaReceiver (iOS)
//
//  Channel admin settings: latency target + mode (live/radio).
//  POSTs config to relay.solun.art API.
//

import SwiftUI

struct ChannelSettingsView: View {
    let channel: String
    @State private var latencyMs: Double = 300
    @State private var mode: String = "radio"
    @State private var saving = false
    @State private var saved = false
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
                Section("Channel") {
                    HStack {
                        Text("Name")
                        Spacer()
                        Text(channel).foregroundColor(.secondary)
                    }
                    Picker("Mode", selection: $mode) {
                        Text("Radio").tag("radio")
                        Text("Live").tag("live")
                    }
                    .onChange(of: mode) { newMode in
                        if newMode == "live" && latencyMs > 100 { latencyMs = 50 }
                        if newMode == "radio" && latencyMs < 100 { latencyMs = 300 }
                    }
                }

                Section("Latency Target") {
                    VStack(alignment: .leading, spacing: 8) {
                        HStack {
                            Text("\(Int(latencyMs))ms")
                                .font(.system(size: 28, weight: .bold, design: .rounded))
                            Spacer()
                            if latencyMs <= 50 {
                                Text("Ultra Low").font(.caption).foregroundColor(.green)
                            } else if latencyMs <= 100 {
                                Text("Low").font(.caption).foregroundColor(.yellow)
                            } else {
                                Text("Stable").font(.caption).foregroundColor(.blue)
                            }
                        }
                        Slider(value: $latencyMs, in: 10...500, step: 10)
                    }

                    // Presets
                    HStack(spacing: 8) {
                        presetButton("Live", ms: 50, color: .green)
                        presetButton("DJ", ms: 80, color: .orange)
                        presetButton("Talk", ms: 100, color: .yellow)
                        presetButton("Radio", ms: 300, color: .blue)
                    }
                }

                Section {
                    Text("Lower latency = more real-time but may cause audio gaps on slow connections. Radio mode (300ms) is recommended for music streaming.\n\nIf a listener's Bluetooth latency exceeds this target, their device will play at the lowest achievable latency with a sync warning.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            }
            .navigationTitle("Channel Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button(saved ? "Saved!" : "Save") { save() }
                        .disabled(saving)
                        .bold()
                }
            }
            .onAppear { loadCurrentConfig() }
        }
    }

    private func presetButton(_ label: String, ms: Double, color: Color) -> some View {
        Button {
            latencyMs = ms
            mode = ms <= 100 ? "live" : "radio"
        } label: {
            Text(label)
                .font(.system(size: 12, weight: .medium))
                .foregroundColor(latencyMs == ms ? .white : color)
                .padding(.horizontal, 12)
                .padding(.vertical, 6)
                .background(latencyMs == ms ? color : color.opacity(0.15))
                .clipShape(Capsule())
        }
    }

    private func loadCurrentConfig() {
        guard let url = URL(string: "https://relay.solun.art/api/channel-config") else { return }
        URLSession.shared.dataTask(with: url) { data, _, _ in
            guard let data,
                  let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let channels = json["channels"] as? [String: [String: Any]],
                  let cfg = channels[channel] else { return }
            DispatchQueue.main.async {
                if let ms = cfg["latencyMs"] as? Int { latencyMs = Double(ms) }
                if let ms = cfg["latencyMs"] as? Double { latencyMs = ms }
                if let m = cfg["mode"] as? String { mode = m }
            }
        }.resume()
    }

    private func save() {
        saving = true
        guard let url = URL(string: "https://relay.solun.art/api/channel-config/\(channel)") else { return }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        let body: [String: Any] = ["latencyMs": Int(latencyMs), "mode": mode]
        request.httpBody = try? JSONSerialization.data(withJSONObject: body)
        URLSession.shared.dataTask(with: request) { _, response, _ in
            DispatchQueue.main.async {
                saving = false
                if let http = response as? HTTPURLResponse, http.statusCode == 200 {
                    saved = true
                    // Update local SDK config
                    SDKAudioReceiver.channelConfigs[channel] = body
                    // Force re-fetch of config
                    SDKAudioReceiver.configLoaded = false
                    SDKAudioReceiver.loadChannelConfig()
                    DispatchQueue.main.asyncAfter(deadline: .now() + 1) { dismiss() }
                }
            }
        }.resume()
    }
}
