//
//  ChannelBrowserView.swift
//  Soluna
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
    SolunaChannel(id: "soluna",  label: "Soluna",   icon: "sun.and.horizon.fill",  color: .solunaSol,           description: "The flagship mix"),
    SolunaChannel(id: "jazz",    label: "Jazz",     icon: "pianokeys",             color: .orange,              description: "Smooth jazz piano"),
    SolunaChannel(id: "lofi",    label: "Lo-Fi",    icon: "headphones",            color: .purple,              description: "Chill beats to relax"),
    SolunaChannel(id: "chill",   label: "Chill",    icon: "leaf.fill",             color: .solunaLuna,          description: "Easy listening vibes"),
    SolunaChannel(id: "dance",   label: "Dance",    icon: "bolt.heart.fill",       color: .solunaSolEnd,        description: "High-energy grooves"),
    SolunaChannel(id: "bjj",     label: "BJJ",      icon: "figure.martial.arts",   color: .solunaGradientMid,   description: "Training beats"),
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

// MARK: - Channel Onboarding View (first launch)

struct ChannelOnboardingView: View {
    let onComplete: () -> Void
    @State private var selected: String? = nil

    private let columns = [
        GridItem(.flexible(), spacing: 16),
        GridItem(.flexible(), spacing: 16),
    ]

    var body: some View {
        ZStack {
            LinearGradient(
                colors: [Color(white: 0.04), Color(white: 0.08)],
                startPoint: .top, endPoint: .bottom
            )
            .ignoresSafeArea()

            VStack(spacing: 32) {
                Spacer()

                // Header
                VStack(spacing: 10) {
                    Text("SOLUNA")
                        .font(.system(size: 36, weight: .bold, design: .rounded))
                        .foregroundStyle(
                            LinearGradient(
                                colors: [Color(red: 0.5, green: 0.3, blue: 1.0),
                                         Color(red: 0.2, green: 0.6, blue: 1.0)],
                                startPoint: .leading, endPoint: .trailing
                            )
                        )
                    Text("チャンネルを選んでください")
                        .font(.system(size: 17, weight: .medium))
                        .foregroundColor(.white.opacity(0.6))
                }

                // Channel grid
                LazyVGrid(columns: columns, spacing: 16) {
                    ForEach(presetChannels) { ch in
                        OnboardingChannelCard(
                            channel: ch,
                            isSelected: selected == ch.id
                        ) {
                            selected = ch.id
                            UserDefaults.standard.set(ch.id, forKey: "channel")
                        }
                    }
                }
                .padding(.horizontal, 24)

                // Start button
                Button(action: {
                    if selected == nil {
                        // Default to first channel if none selected
                        UserDefaults.standard.set(presetChannels[0].id, forKey: "channel")
                    }
                    onComplete()
                }) {
                    HStack(spacing: 8) {
                        Text(selected != nil ? "Start Listening" : "Skip")
                            .font(.system(size: 17, weight: .semibold))
                        if selected != nil {
                            Image(systemName: "play.fill")
                                .font(.system(size: 14, weight: .semibold))
                        }
                    }
                    .foregroundColor(.white)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 16)
                    .background(
                        selected != nil
                            ? LinearGradient(colors: [Color(red: 0.5, green: 0.3, blue: 1.0),
                                                       Color(red: 0.2, green: 0.6, blue: 1.0)],
                                             startPoint: .leading, endPoint: .trailing)
                            : LinearGradient(colors: [Color.white.opacity(0.12), Color.white.opacity(0.12)],
                                             startPoint: .leading, endPoint: .trailing)
                    )
                    .clipShape(RoundedRectangle(cornerRadius: 16))
                }
                .padding(.horizontal, 24)
                .animation(.spring(response: 0.3, dampingFraction: 0.7), value: selected)

                Spacer()
            }
        }
        .preferredColorScheme(.dark)
    }
}

// MARK: - Onboarding Channel Card

private struct OnboardingChannelCard: View {
    let channel: SolunaChannel
    let isSelected: Bool
    let onTap: () -> Void

    var body: some View {
        Button(action: onTap) {
            VStack(spacing: 12) {
                ZStack {
                    Circle()
                        .fill(isSelected ? channel.color.opacity(0.25) : Color.white.opacity(0.06))
                        .frame(width: 64, height: 64)
                    Image(systemName: channel.icon)
                        .font(.system(size: 28))
                        .foregroundStyle(isSelected ? channel.color : Color.white.opacity(0.5))
                }

                VStack(spacing: 4) {
                    Text(channel.label)
                        .font(.system(size: 15, weight: .bold))
                        .foregroundColor(isSelected ? .white : .white.opacity(0.7))
                    Text(channel.description)
                        .font(.system(size: 11))
                        .foregroundColor(isSelected ? .white.opacity(0.7) : .white.opacity(0.35))
                        .lineLimit(1)
                        .multilineTextAlignment(.center)
                }
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 20)
            .background(
                RoundedRectangle(cornerRadius: 20)
                    .fill(isSelected
                          ? channel.color.opacity(0.15)
                          : Color.white.opacity(0.04))
            )
            .overlay(
                RoundedRectangle(cornerRadius: 20)
                    .strokeBorder(
                        isSelected ? channel.color.opacity(0.6) : Color.white.opacity(0.08),
                        lineWidth: isSelected ? 1.5 : 0.5
                    )
            )
            .scaleEffect(isSelected ? 1.03 : 1.0)
            .animation(.spring(response: 0.25, dampingFraction: 0.7), value: isSelected)
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
