//
//  DevicePickerView.swift
//  Soluna
//
//  Global device picker — browse TX devices across all relay servers.
//

import SwiftUI

struct DevicePickerView: View {
    @ObservedObject var registry: GlobalDeviceRegistry
    let onSelect: (GlobalDevice) -> Void
    @Environment(\.dismiss) private var dismiss

    // Custom relay entry
    @State private var customRelayHost = ""
    @State private var showCustomRelay = false

    var body: some View {
        NavigationView {
            ZStack {
                Color.black.ignoresSafeArea()

                VStack(spacing: 0) {
                    // Header
                    HStack {
                        VStack(alignment: .leading, spacing: 2) {
                            Text("デバイスを選択")
                                .font(.title2.bold())
                                .foregroundColor(.white)
                            Text("グローバル登録済みデバイス")
                                .font(.caption)
                                .foregroundColor(.secondary)
                        }
                        Spacer()
                        Button(action: { registry.refresh() }) {
                            Image(systemName: registry.isLoading ? "arrow.clockwise" : "arrow.clockwise")
                                .font(.system(size: 16, weight: .medium))
                                .foregroundColor(.white.opacity(0.7))
                                .rotationEffect(.degrees(registry.isLoading ? 360 : 0))
                                .animation(registry.isLoading ? .linear(duration: 1).repeatForever(autoreverses: false) : .default,
                                           value: registry.isLoading)
                        }
                    }
                    .padding(.horizontal, 20)
                    .padding(.top, 20)
                    .padding(.bottom, 16)

                    if registry.isLoading {
                        Spacer()
                        ProgressView()
                            .tint(.white)
                        Text("リレーに問い合わせ中...")
                            .font(.caption)
                            .foregroundColor(.secondary)
                            .padding(.top, 8)
                        Spacer()
                    } else if let err = registry.error, registry.devices.isEmpty {
                        Spacer()
                        Image(systemName: "wifi.exclamationmark")
                            .font(.system(size: 40))
                            .foregroundColor(.secondary)
                            .padding(.bottom, 12)
                        Text(err)
                            .font(.callout)
                            .foregroundColor(.secondary)
                            .multilineTextAlignment(.center)
                        Button("再試行") { registry.refresh() }
                            .buttonStyle(.bordered)
                            .tint(.white)
                            .padding(.top, 16)
                        Spacer()
                    } else {
                        ScrollView {
                            LazyVStack(spacing: 12) {
                                ForEach(registry.devices) { device in
                                    DeviceRow(device: device) {
                                        onSelect(device)
                                        dismiss()
                                    }
                                }

                                // Add custom relay
                                addCustomRelayButton
                            }
                            .padding(.horizontal, 16)
                            .padding(.vertical, 8)
                        }
                    }
                }
            }
            .navigationBarHidden(true)
        }
        .onAppear {
            if registry.devices.isEmpty && !registry.isLoading {
                registry.refresh()
            }
        }
    }

    private var addCustomRelayButton: some View {
        VStack(spacing: 8) {
            if showCustomRelay {
                HStack(spacing: 8) {
                    TextField("relay.example.com:5100", text: $customRelayHost)
                        .textFieldStyle(.plain)
                        .font(.system(size: 14, design: .monospaced))
                        .foregroundColor(.white)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 10)
                        .background(Color.white.opacity(0.08))
                        .cornerRadius(10)
                        .autocorrectionDisabled()
                        .textInputAutocapitalization(.never)

                    Button("検索") {
                        let parts = customRelayHost.split(separator: ":")
                        let host = String(parts.first ?? "")
                        let port = UInt16(parts.last ?? "5100") ?? 5100
                        guard !host.isEmpty else { return }
                        registry.refresh(relays: [(host, port)])
                        showCustomRelay = false
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.purple)
                }
                .padding(.horizontal, 4)
            }

            Button(action: { withAnimation { showCustomRelay.toggle() } }) {
                Label("リレーを追加", systemImage: "plus.circle")
                    .font(.subheadline)
                    .foregroundColor(.white.opacity(0.5))
            }
        }
        .padding(.top, 8)
    }
}

private struct DeviceRow: View {
    let device: GlobalDevice
    let onTap: () -> Void

    var body: some View {
        Button(action: onTap) {
            HStack(spacing: 14) {
                // Icon
                ZStack {
                    Circle()
                        .fill(iconColor.opacity(0.2))
                        .frame(width: 44, height: 44)
                    Image(systemName: iconName)
                        .font(.system(size: 18, weight: .medium))
                        .foregroundColor(iconColor)
                }

                // Info
                VStack(alignment: .leading, spacing: 3) {
                    Text(device.name)
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundColor(.white)
                        .lineLimit(1)
                    HStack(spacing: 6) {
                        Image(systemName: "dot.radiowaves.left.and.right")
                            .font(.system(size: 10))
                            .foregroundColor(.green)
                        Text(device.group)
                            .font(.system(size: 12, design: .monospaced))
                            .foregroundColor(.green.opacity(0.9))
                        Text("•")
                            .foregroundColor(.secondary)
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
                    .padding(.vertical, 4)
                    .background(iconColor.opacity(0.15))
                    .cornerRadius(6)

                Image(systemName: "chevron.right")
                    .font(.system(size: 12))
                    .foregroundColor(.secondary)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)
            .background(Color.white.opacity(0.06))
            .cornerRadius(14)
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
