//
//  MenuContent.swift
//  SolunaControl — macOS menu bar popover
//

import SwiftUI

struct MenuContent: View {
    @EnvironmentObject private var d: DaemonClient
    @AppStorage("macHost")   private var host      = "localhost"
    @AppStorage("phoneHost") private var phoneHost = ""
    @State private var editingHost      = false
    @State private var hostInput        = ""
    @State private var editingPhoneHost = false
    @State private var phoneHostInput   = ""

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
                    // ── Volume ────────────────────────────
                    section("Volume") {
                        // Mute + slider + step buttons
                        HStack(spacing: 6) {
                            // Mute toggle
                            Button(action: { d.setMonitorMute(!d.monitorMuted) }) {
                                Image(systemName: d.monitorMuted ? "speaker.slash.fill" : volumeIcon)
                                    .foregroundStyle(d.monitorMuted ? .red : .primary)
                                    .frame(width: 18)
                            }
                            .buttonStyle(.plain)
                            .help(d.monitorMuted ? "Unmute" : "Mute")

                            // −10%
                            Button(action: {
                                if d.monitorMuted { d.setMonitorMute(false) }
                                d.setMonitorVolume(max(0, d.monitorVolume - 0.1))
                            }) {
                                Image(systemName: "minus.circle.fill")
                                    .foregroundStyle(.secondary)
                            }
                            .buttonStyle(.plain)

                            Slider(value: Binding(
                                get: { d.monitorVolume },
                                set: { v in
                                    if d.monitorMuted { d.setMonitorMute(false) }
                                    d.setMonitorVolume(v)
                                }
                            ), in: 0...1)
                            .controlSize(.small)

                            // +10%
                            Button(action: {
                                if d.monitorMuted { d.setMonitorMute(false) }
                                d.setMonitorVolume(min(1, d.monitorVolume + 0.1))
                            }) {
                                Image(systemName: "plus.circle.fill")
                                    .foregroundStyle(.secondary)
                            }
                            .buttonStyle(.plain)

                            Text(d.monitorMuted ? "Mute" : "\(Int(d.monitorVolume * 100))%")
                                .monospacedDigit()
                                .font(.caption)
                                .foregroundStyle(d.monitorMuted ? .red : .secondary)
                                .frame(width: 36, alignment: .trailing)
                        }
                        .disabled(!d.isConnected)
                    }

                    Divider().padding(.horizontal, 16)

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

                        // Device picker (always visible when devices loaded)
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

                    // ── iPhone Receiver ───────────────────
                    Divider().padding(.horizontal, 16)
                    section("iPhone Receiver") {
                        // Phone host row
                        if editingPhoneHost {
                            HStack {
                                TextField("iPhone IP / tunnel", text: $phoneHostInput)
                                    .textFieldStyle(.roundedBorder)
                                    .font(.callout)
                                Button("Connect") {
                                    phoneHost = phoneHostInput
                                    editingPhoneHost = false
                                    d.disconnectPhone()
                                    if !phoneHost.isEmpty { d.connectPhone(host: phoneHost) }
                                }
                                .buttonStyle(.borderedProminent)
                                .controlSize(.small)
                            }
                        } else {
                            HStack {
                                Text(phoneHost.isEmpty ? "Not configured" : phoneHost)
                                    .font(.callout)
                                    .foregroundStyle(.secondary)
                                    .lineLimit(1)
                                    .truncationMode(.middle)
                                Spacer()
                                HStack(spacing: 4) {
                                    Circle()
                                        .fill(d.phoneConnected ? Color.green : Color.gray)
                                        .frame(width: 6, height: 6)
                                }
                                Button("Edit") {
                                    phoneHostInput = phoneHost
                                    editingPhoneHost = true
                                }
                                .controlSize(.small)
                            }
                        }

                        if d.phoneConnected {
                            // Volume + mute
                            HStack(spacing: 6) {
                                Button(action: { d.setPhoneMute(!d.phoneMuted) }) {
                                    Image(systemName: d.phoneMuted ? "speaker.slash.fill" : phoneVolumeIcon)
                                        .foregroundStyle(d.phoneMuted ? .red : .primary)
                                        .frame(width: 18)
                                }
                                .buttonStyle(.plain)
                                .help(d.phoneMuted ? "Unmute iPhone" : "Mute iPhone")

                                Slider(value: Binding(
                                    get: { d.phoneVolume },
                                    set: { v in
                                        if d.phoneMuted { d.setPhoneMute(false) }
                                        d.setPhoneVolume(v)
                                    }
                                ), in: 0...1)
                                .controlSize(.small)

                                Text(d.phoneMuted ? "Mute" : "\(Int(d.phoneVolume * 100))%")
                                    .monospacedDigit()
                                    .font(.caption)
                                    .foregroundStyle(d.phoneMuted ? .red : .secondary)
                                    .frame(width: 36, alignment: .trailing)
                            }

                            // Buffer
                            HStack {
                                Text("\(d.phoneBufferMs) ms")
                                    .monospacedDigit()
                                    .frame(width: 44, alignment: .leading)
                                Slider(value: Binding(
                                    get: { Double(d.phoneBufferMs) },
                                    set: { d.setPhoneBuffer(Int($0)) }
                                ), in: 5...200, step: 5)
                                .controlSize(.small)
                            }

                            if d.phonePackets > 0 {
                                statRow("Packets", value: formatNum(d.phonePackets))
                            }
                        }
                    }

                    // ── Tunnel URL ────────────────────────
                    if !d.tunnelURL.isEmpty {
                        Divider().padding(.horizontal, 16)
                        section("Internet URL") {
                            HStack(spacing: 6) {
                                Image(systemName: "globe")
                                    .foregroundStyle(.blue)
                                    .frame(width: 16)
                                Text(d.tunnelURL
                                    .replacingOccurrences(of: "https://", with: "")
                                    .replacingOccurrences(of: "http://", with: ""))
                                    .font(.caption.monospacedDigit())
                                    .foregroundStyle(.secondary)
                                    .lineLimit(1)
                                    .truncationMode(.middle)
                                Spacer()
                                Button {
                                    NSPasteboard.general.clearContents()
                                    NSPasteboard.general.setString(d.tunnelURL, forType: .string)
                                } label: {
                                    Image(systemName: "doc.on.doc")
                                        .foregroundStyle(.blue)
                                }
                                .buttonStyle(.plain)
                                .help("Copy tunnel URL for iPhone")
                            }
                            Text("Paste this in iPhone Settings → Mac IP")
                                .font(.caption2)
                                .foregroundStyle(.tertiary)
                        }
                    }

                    // ── Connection ────────────────────────
                    Divider().padding(.horizontal, 16)
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
