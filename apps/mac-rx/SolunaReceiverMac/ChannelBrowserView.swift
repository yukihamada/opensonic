//
//  ChannelBrowserView.swift
//  SolunaReceiver
//
//  Preset channel browser — shared design for iOS & Mac
//

import SwiftUI

// MARK: - Channel Model

struct SolunaChannel: Identifiable {
    let id: String          // channel name (used for relay group)
    let label: String       // display name
    let icon: String        // SF Symbol name
    let color: Color        // accent color
    let description: String // short description
}

let presetChannels: [SolunaChannel] = [
    SolunaChannel(id: "lofi",    label: "Lo-Fi",    icon: "headphones",          color: .purple,  description: "Chill beats to relax"),
    SolunaChannel(id: "jazz",    label: "Jazz",     icon: "pianokeys",           color: .orange,  description: "Smooth jazz piano"),
    SolunaChannel(id: "ambient", label: "Ambient",  icon: "leaf.fill",           color: .teal,    description: "Atmospheric soundscapes"),
    SolunaChannel(id: "soluna",  label: "BJJ",      icon: "figure.martial.arts", color: .blue,    description: "BJJ training beats"),
]

// MARK: - Channel Browser View

struct ChannelBrowserView: View {
    let currentChannel: String
    let onSelect: (String) -> Void

    private let columns = [
        GridItem(.flexible(), spacing: 12),
        GridItem(.flexible(), spacing: 12),
    ]

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Image(systemName: "radio")
                    .foregroundStyle(.secondary)
                Text("Channels")
                    .font(.headline)
                    .foregroundStyle(.primary)
                Spacer()
            }
            .padding(.horizontal, 4)

            LazyVGrid(columns: columns, spacing: 12) {
                ForEach(presetChannels) { ch in
                    ChannelCard(
                        channel: ch,
                        isActive: currentChannel == ch.id,
                        onTap: { onSelect(ch.id) }
                    )
                }
            }
        }
    }
}

// MARK: - Channel Card

struct ChannelCard: View {
    let channel: SolunaChannel
    let isActive: Bool
    let onTap: () -> Void

    var body: some View {
        Button(action: onTap) {
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Image(systemName: channel.icon)
                        .font(.title2)
                        .foregroundStyle(isActive ? .white : channel.color)
                    Spacer()
                    if isActive {
                        Image(systemName: "antenna.radiowaves.left.and.right")
                            .font(.caption)
                            .foregroundStyle(.green)
                    }
                }

                Text(channel.label)
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(isActive ? .white : .primary)

                Text(channel.description)
                    .font(.caption2)
                    .foregroundStyle(isActive ? .white.opacity(0.8) : .secondary)
                    .lineLimit(1)
            }
            .padding(12)
            .background(
                RoundedRectangle(cornerRadius: 14)
                    .fill(isActive
                          ? channel.color.gradient
                          : Color.gray.opacity(0.15).gradient)
            )
            .overlay(
                RoundedRectangle(cornerRadius: 14)
                    .strokeBorder(isActive ? Color.white.opacity(0.3) : Color.clear, lineWidth: 1)
            )
        }
        .buttonStyle(.plain)
    }
}

#if DEBUG
struct ChannelBrowserView_Previews: PreviewProvider {
    static var previews: some View {
        ChannelBrowserView(currentChannel: "lofi") { _ in }
            .padding()
            .background(Color.black)
            .preferredColorScheme(.dark)
    }
}
#endif
