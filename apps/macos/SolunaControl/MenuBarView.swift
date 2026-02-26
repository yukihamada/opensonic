import SwiftUI

struct MenuBarView: View {
    @ObservedObject var daemon: DaemonClient
    @State private var host = UserDefaults.standard.string(forKey: "macHost") ?? "localhost"
    @State private var isEditingHost = false

    var body: some View {
        VStack(spacing: 0) {
            // ── Header ───────────────────────────────────────────────────
            HStack(spacing: 8) {
                Circle()
                    .fill(daemon.isConnected ? Color.green : Color.gray)
                    .frame(width: 8, height: 8)
                Text("Soluna")
                    .font(.headline)
                Spacer()
                Text(daemon.isConnected ? "Connected" : "Offline")
                    .font(.caption)
                    .foregroundColor(daemon.isConnected ? .green : .secondary)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)

            Divider()

            // ── Controls ─────────────────────────────────────────────────
            VStack(spacing: 12) {
                // Volume
                HStack(spacing: 10) {
                    Image(systemName: daemon.monitorMuted ? "speaker.slash.fill" : "speaker.wave.2.fill")
                        .foregroundColor(daemon.monitorMuted ? .red : .secondary)
                        .frame(width: 20)
                        .onTapGesture { daemon.setMonitorMute(!daemon.monitorMuted) }

                    Slider(value: Binding(
                        get: { daemon.monitorVolume },
                        set: { v in
                            if daemon.monitorMuted { daemon.setMonitorMute(false) }
                            daemon.setMonitorVolume(v)
                        }
                    ), in: 0...1)

                    Text("\(Int(daemon.monitorVolume * 100))%")
                        .font(.system(.caption, design: .monospaced))
                        .foregroundColor(.secondary)
                        .frame(width: 36)
                }

                // Delay
                HStack(spacing: 10) {
                    Image(systemName: "timer")
                        .foregroundColor(.secondary)
                        .frame(width: 20)

                    Slider(value: Binding(
                        get: { Double(daemon.monitorDelayMs) },
                        set: { daemon.setMonitorDelay(Int($0)) }
                    ), in: 0...200, step: 5)

                    Text("\(daemon.monitorDelayMs)ms")
                        .font(.system(.caption, design: .monospaced))
                        .foregroundColor(.secondary)
                        .frame(width: 36)
                }

                // Last sync
                if daemon.measuredLatencyMs > 0 {
                    HStack {
                        Image(systemName: "arrow.triangle.2.circlepath")
                            .font(.caption)
                            .foregroundColor(.green)
                        Text("Last sync: \(daemon.measuredLatencyMs)ms")
                            .font(.caption)
                            .foregroundColor(.secondary)
                        Spacer()
                        Button("Sync now") { daemon.performSync() }
                            .font(.caption)
                            .buttonStyle(.borderless)
                            .foregroundColor(.blue)
                    }
                }
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)
            .disabled(!daemon.isConnected)

            Divider()

            // ── Host ─────────────────────────────────────────────────────
            HStack(spacing: 8) {
                Image(systemName: "network")
                    .foregroundColor(.secondary)
                    .frame(width: 16)
                if isEditingHost {
                    TextField("host:8400", text: $host, onCommit: {
                        UserDefaults.standard.set(host, forKey: "macHost")
                        daemon.connect(host: host)
                        isEditingHost = false
                    })
                    .textFieldStyle(.roundedBorder)
                    .font(.caption)
                } else {
                    Text(host.isEmpty ? "Not set" : host)
                        .font(.caption)
                        .foregroundColor(.secondary)
                        .lineLimit(1)
                    Spacer()
                    Button("Edit") { isEditingHost = true }
                        .font(.caption)
                        .buttonStyle(.borderless)
                        .foregroundColor(.blue)
                }
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 8)

            Divider()

            // ── Footer ───────────────────────────────────────────────────
            HStack {
                Button("Open Web UI") {
                    if let url = URL(string: "http://\(host.components(separatedBy: ":").first ?? "localhost"):8400") {
                        NSWorkspace.shared.open(url)
                    }
                }
                .font(.caption)
                .buttonStyle(.borderless)
                .foregroundColor(.blue)

                Spacer()

                Button("Quit") { NSApp.terminate(nil) }
                    .font(.caption)
                    .buttonStyle(.borderless)
                    .foregroundColor(.secondary)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 10)
        }
        .onAppear {
            let h = UserDefaults.standard.string(forKey: "macHost") ?? ""
            if !h.isEmpty {
                host = h
                daemon.connect(host: h)
            }
        }
    }
}
