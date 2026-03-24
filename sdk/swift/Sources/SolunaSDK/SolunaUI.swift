import SwiftUI

// MARK: - SolunaPlayerView

/// A compact mini-player bar showing channel emoji, name, play/pause button, and volume.
///
/// Usage:
/// ```swift
/// SolunaPlayerView(client: client)
/// ```
public struct SolunaPlayerView: View {
    @ObservedObject var client: SolunaClient

    @State private var volume: Double = 0.8

    public init(client: SolunaClient) {
        self.client = client
    }

    public var body: some View {
        HStack(spacing: 12) {
            // Channel emoji + name
            if let ch = client.currentChannel {
                Text(ch.emoji)
                    .font(.title2)
                Text(ch.name)
                    .font(.headline)
                    .lineLimit(1)
            } else {
                Text(client.channel)
                    .font(.headline)
                    .lineLimit(1)
            }

            Spacer()

            // Connection status dot
            SolunaStatusBadge(client: client)

            // Play / Pause
            Button {
                if client.isConnected {
                    client.disconnect()
                } else {
                    client.connect(channel: client.channel)
                }
            } label: {
                Image(systemName: client.isConnected ? "pause.fill" : "play.fill")
                    .font(.title3)
            }
            .buttonStyle(.plain)

            // Volume
            SolunaVolumeSlider(volume: $volume)
                .frame(width: 80)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
        .background(.ultraThinMaterial)
        .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
    }
}

// MARK: - SolunaChannelPicker

/// A grid of available channels. Tapping a channel switches playback.
///
/// Usage:
/// ```swift
/// SolunaChannelPicker(client: client)
/// ```
public struct SolunaChannelPicker: View {
    @ObservedObject var client: SolunaClient

    private let columns = [
        GridItem(.adaptive(minimum: 100), spacing: 12)
    ]

    public init(client: SolunaClient) {
        self.client = client
    }

    public var body: some View {
        LazyVGrid(columns: columns, spacing: 12) {
            ForEach(client.channels) { channel in
                Button {
                    client.setChannel(channel.id)
                } label: {
                    VStack(spacing: 6) {
                        Text(channel.emoji)
                            .font(.largeTitle)
                        Text(channel.name)
                            .font(.caption)
                            .fontWeight(.medium)
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 12)
                    .background(
                        RoundedRectangle(cornerRadius: 12, style: .continuous)
                            .fill(channel.id == client.channel
                                  ? Color.accentColor.opacity(0.2)
                                  : Color.secondary.opacity(0.1))
                    )
                    .overlay(
                        RoundedRectangle(cornerRadius: 12, style: .continuous)
                            .stroke(channel.id == client.channel
                                    ? Color.accentColor
                                    : Color.clear, lineWidth: 2)
                    )
                }
                .buttonStyle(.plain)
            }
        }
    }
}

// MARK: - SolunaVolumeSlider

/// A custom volume slider with a subtle wave-style track.
public struct SolunaVolumeSlider: View {
    @Binding var volume: Double

    public init(volume: Binding<Double>) {
        self._volume = volume
    }

    public var body: some View {
        HStack(spacing: 4) {
            Image(systemName: volume > 0 ? "speaker.fill" : "speaker.slash.fill")
                .font(.caption2)
                .foregroundStyle(.secondary)

            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    // Track background
                    Capsule()
                        .fill(Color.secondary.opacity(0.2))
                        .frame(height: 4)

                    // Filled portion
                    Capsule()
                        .fill(Color.accentColor)
                        .frame(width: geo.size.width * CGFloat(volume), height: 4)
                }
                .frame(maxHeight: .infinity)
                .contentShape(Rectangle())
                .gesture(
                    DragGesture(minimumDistance: 0)
                        .onChanged { value in
                            let ratio = value.location.x / geo.size.width
                            volume = min(max(Double(ratio), 0), 1)
                        }
                )
            }
        }
    }
}

// MARK: - SolunaStatusBadge

/// A small dot indicator for connection status.
///
/// - Green: receiving audio
/// - Yellow: connected but not yet receiving
/// - Red: disconnected or error
public struct SolunaStatusBadge: View {
    @ObservedObject var client: SolunaClient

    public init(client: SolunaClient) {
        self.client = client
    }

    public var body: some View {
        Circle()
            .fill(statusColor)
            .frame(width: 8, height: 8)
    }

    private var statusColor: Color {
        if client.isReceivingAudio {
            return .green
        } else if client.isConnected {
            return .yellow
        } else {
            return .red
        }
    }
}

// MARK: - SolunaFanRankBadge

/// Displays the current fan rank emoji and label.
public struct SolunaFanRankBadge: View {
    @ObservedObject var client: SolunaClient

    public init(client: SolunaClient) {
        self.client = client
    }

    public var body: some View {
        HStack(spacing: 4) {
            Text(client.fanRank.badge)
            Text(client.fanRank.rawValue)
                .font(.caption)
                .fontWeight(.semibold)
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
        .background(Color.secondary.opacity(0.1))
        .clipShape(Capsule())
    }
}
