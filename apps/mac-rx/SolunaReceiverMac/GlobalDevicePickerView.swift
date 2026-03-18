//
//  GlobalDevicePickerView.swift
//  SolunaReceiverMac
//
//  Global device picker — browse TX devices across all relay servers.
//

import SwiftUI

struct GlobalDevicePickerView: View {
    @ObservedObject var registry: GlobalDeviceRegistry
    let onSelect: (GlobalDevice) -> Void
    @Environment(\.dismiss) private var dismiss

    @State private var customRelayHost = ""
    @State private var showCustomRelay = false

    var body: some View {
        VStack(spacing: 0) {
            // Header
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text("デバイスを選択")
                        .font(.title2.bold())
                    Text("グローバル登録済みデバイス")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                Spacer()
                Button(action: { registry.refresh() }) {
                    Image(systemName: "arrow.clockwise")
                        .font(.system(size: 14, weight: .medium))
                        .rotationEffect(.degrees(registry.isLoading ? 360 : 0))
                        .animation(
                            registry.isLoading
                                ? .linear(duration: 1).repeatForever(autoreverses: false)
                                : .default,
                            value: registry.isLoading
                        )
                }
                .buttonStyle(.plain)
                .foregroundColor(.secondary)
            }
            .padding(.horizontal, 20)
            .padding(.top, 20)
            .padding(.bottom, 16)

            Divider()

            if registry.isLoading {
                Spacer()
                ProgressView("リレーに問い合わせ中...")
                    .padding()
                Spacer()
            } else if let err = registry.error, registry.devices.isEmpty {
                Spacer()
                VStack(spacing: 12) {
                    Image(systemName: "wifi.exclamationmark")
                        .font(.system(size: 36))
                        .foregroundColor(.secondary)
                    Text(err)
                        .foregroundColor(.secondary)
                        .multilineTextAlignment(.center)
                    Button("再試行") { registry.refresh() }
                }
                .padding()
                Spacer()
            } else {
                ScrollView {
                    LazyVStack(spacing: 8) {
                        ForEach(registry.devices) { device in
                            GlobalDeviceRow(device: device) {
                                onSelect(device)
                                dismiss()
                            }
                        }

                        // Custom relay
                        customRelaySection
                    }
                    .padding(.horizontal, 16)
                    .padding(.vertical, 12)
                }
            }

            Divider()

            HStack {
                Spacer()
                Button("キャンセル") { dismiss() }
                    .keyboardShortcut(.cancelAction)
            }
            .padding(.horizontal, 20)
            .padding(.vertical, 12)
        }
        .frame(width: 400, height: 480)
        .background(Color(NSColor.windowBackgroundColor))
        .onAppear {
            if registry.devices.isEmpty && !registry.isLoading {
                registry.refresh()
            }
        }
    }

    private var customRelaySection: some View {
        VStack(spacing: 6) {
            if showCustomRelay {
                HStack(spacing: 8) {
                    TextField("relay.example.com:5100", text: $customRelayHost)
                        .textFieldStyle(.roundedBorder)
                        .font(.system(size: 13, design: .monospaced))

                    Button("検索") {
                        let parts = customRelayHost.split(separator: ":")
                        let host = String(parts.first ?? "")
                        let port = UInt16(parts.last ?? "5100") ?? 5100
                        guard !host.isEmpty else { return }
                        registry.refresh(relays: [(host, port)])
                        showCustomRelay = false
                    }
                    .buttonStyle(.borderedProminent)
                }
            }

            Button(action: { withAnimation { showCustomRelay.toggle() } }) {
                Label(showCustomRelay ? "キャンセル" : "リレーを追加", systemImage: showCustomRelay ? "xmark" : "plus.circle")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
            }
            .buttonStyle(.plain)
        }
        .padding(.top, 4)
    }
}

// MARK: - Row

private struct GlobalDeviceRow: View {
    let device: GlobalDevice
    let onTap: () -> Void

    var body: some View {
        Button(action: onTap) {
            HStack(spacing: 12) {
                // Icon
                ZStack {
                    Circle()
                        .fill(iconColor.opacity(0.15))
                        .frame(width: 40, height: 40)
                    Image(systemName: iconName)
                        .font(.system(size: 17, weight: .medium))
                        .foregroundColor(iconColor)
                }

                // Info
                VStack(alignment: .leading, spacing: 2) {
                    Text(device.name)
                        .font(.system(size: 14, weight: .semibold))
                        .lineLimit(1)
                    HStack(spacing: 6) {
                        Image(systemName: "dot.radiowaves.left.and.right")
                            .font(.system(size: 10))
                            .foregroundColor(.green)
                        Text(device.group)
                            .font(.system(size: 11, design: .monospaced))
                            .foregroundColor(.green)
                        Text("•").foregroundColor(.secondary)
                        Text(device.relayHost)
                            .font(.system(size: 11))
                            .foregroundColor(.secondary)
                    }
                }

                Spacer()

                // Role badge
                Text(device.role == "owner" ? "TX" : "DJ")
                    .font(.system(size: 11, weight: .bold))
                    .foregroundColor(iconColor)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 3)
                    .background(iconColor.opacity(0.12))
                    .cornerRadius(5)

                Image(systemName: "chevron.right")
                    .font(.system(size: 11))
                    .foregroundColor(.secondary)
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 10)
            .background(Color(NSColor.controlBackgroundColor))
            .cornerRadius(10)
        }
        .buttonStyle(.plain)
    }

    private var iconName: String {
        let n = device.name.lowercased()
        if n.contains("mac") || n.contains("macbook") { return "laptopcomputer" }
        if n.contains("iphone") || n.contains("ipad") { return "iphone" }
        if n.contains("dj") || n.contains("mix") { return "headphones" }
        return "speaker.wave.2"
    }

    private var iconColor: Color {
        device.role == "owner" ? .purple : .blue
    }
}
