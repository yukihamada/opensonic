//
//  MenuContent.swift
//  SolunaControl — macOS menu bar popover
//

import SwiftUI

struct MenuContent: View {
    @EnvironmentObject private var d: DaemonClient
    @AppStorage("macHost") private var host = "localhost"
    @State private var editingHost = false
    @State private var hostInput = ""

    var body: some View {
        VStack(spacing: 0) {
            // ── Header ────────────────────────────────────
            HStack {
                Image(systemName: "waveform")
                    .foregroundStyle(d.isConnected ? .green : .secondary)
                Text("Soluna")
                    .font(.headline)
                Spacer()
                statusDot
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)

            Divider()

            ScrollView {
                VStack(spacing: 0) {
                    // ── Speakers ──────────────────────────
                    section("Speakers") {
                        Toggle("Mac speakers", isOn: Binding(
                            get: { d.monitorRunning },
                            set: { on in
                                on ? d.startMonitor(device: d.selectedDevice.isEmpty
                                    ? (d.devices.first ?? "default")
                                    : d.selectedDevice)
                                   : d.stopMonitor()
                            }
                        ))
                        .disabled(!d.isConnected)

                        if d.monitorRunning || !d.devices.isEmpty {
                            // Volume
                            HStack(spacing: 8) {
                                Button(action: { d.setMonitorMute(!d.monitorMuted) }) {
                                    Image(systemName: d.monitorMuted
                                          ? "speaker.slash.fill"
                                          : volumeIcon)
                                        .foregroundStyle(d.monitorMuted ? .red : .secondary)
                                        .frame(width: 18)
                                }
                                .buttonStyle(.plain)

                                Slider(value: Binding(
                                    get: { d.monitorVolume },
                                    set: { d.setMonitorVolume($0) }
                                ), in: 0...1)
                                .controlSize(.small)

                                Text("\(Int(d.monitorVolume * 100))%")
                                    .monospacedDigit()
                                    .font(.caption)
                                    .frame(width: 32, alignment: .trailing)
                            }
                            .disabled(!d.monitorRunning)

                            // Device picker
                            if d.devices.count > 1 {
                                Picker("Device", selection: Binding(
                                    get: { d.selectedDevice },
                                    set: { dev in
                                        d.selectedDevice = dev
                                        if d.monitorRunning { d.startMonitor(device: dev) }
                                    }
                                )) {
                                    ForEach(d.devices, id: \.self) { Text($0).tag($0) }
                                }
                                .labelsHidden()
                                .controlSize(.small)
                            }
                        }
                    }

                    Divider().padding(.horizontal, 16)

                    // ── Buffer ────────────────────────────
                    section("Buffer") {
                        HStack {
                            Text("\(d.monitorBufferMs) ms")
                                .monospacedDigit()
                                .frame(width: 44, alignment: .leading)
                            Slider(value: Binding(
                                get: { Double(d.monitorBufferMs) },
                                set: { d.setMonitorBuffer(Int($0)) }
                            ), in: 5...100, step: 5)
                            .controlSize(.small)
                        }
                        .disabled(!d.isConnected)
                    }

                    Divider().padding(.horizontal, 16)

                    // ── Stats ─────────────────────────────
                    if d.monitorPackets > 0 {
                        section("Stats") {
                            statRow("Packets", value: formatNum(d.monitorPackets))
                        }
                        Divider().padding(.horizontal, 16)
                    }

                    // ── Connection ────────────────────────
                    section("Connection") {
                        if editingHost {
                            HStack {
                                TextField("hostname / IP", text: $hostInput)
                                    .textFieldStyle(.roundedBorder)
                                    .font(.callout)
                                Button("Save") {
                                    host = hostInput
                                    editingHost = false
                                    d.disconnect()
                                    d.connect(host: host)
                                }
                                .buttonStyle(.borderedProminent)
                                .controlSize(.small)
                            }
                        } else {
                            HStack {
                                Text(host)
                                    .font(.callout)
                                    .foregroundStyle(.secondary)
                                Spacer()
                                Button("Edit") {
                                    hostInput = host
                                    editingHost = true
                                }
                                .controlSize(.small)
                            }
                        }
                    }

                    // ── Quit ──────────────────────────────
                    Divider().padding(.horizontal, 16)
                    Button("Quit Soluna Control") {
                        NSApplication.shared.terminate(nil)
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(.secondary)
                    .font(.callout)
                    .padding(.horizontal, 16)
                    .padding(.vertical, 10)
                }
            }
        }
        .frame(width: 280)
        .onAppear { d.connect(host: host) }
    }

    // MARK: - Helpers

    private var statusDot: some View {
        HStack(spacing: 4) {
            Circle()
                .fill(d.isConnected ? .green : .gray)
                .frame(width: 7, height: 7)
            Text(d.isConnected ? "Live" : "Offline")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    private var volumeIcon: String {
        d.monitorVolume < 0.01 ? "speaker.fill"
        : d.monitorVolume < 0.5 ? "speaker.wave.1.fill"
        : "speaker.wave.2.fill"
    }

    private func formatNum(_ n: UInt64) -> String {
        n >= 1_000_000 ? String(format: "%.1fM", Double(n)/1_000_000)
        : n >= 1_000   ? String(format: "%.1fK", Double(n)/1_000)
        : "\(n)"
    }

    @ViewBuilder
    private func section<Content: View>(_ title: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title)
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
                .padding(.bottom, 2)
            content()
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
    }

    private func statRow(_ label: String, value: String) -> some View {
        HStack {
            Text(label).foregroundStyle(.secondary)
            Spacer()
            Text(value).monospacedDigit()
        }
        .font(.callout)
    }
}
