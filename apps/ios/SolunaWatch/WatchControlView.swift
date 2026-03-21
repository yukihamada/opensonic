import SwiftUI
import WatchConnectivity

struct WatchControlView: View {
    @StateObject private var connector = WatchConnector()

    private struct RadioChannel: Identifiable {
        let id: String
        let name: String
        let emoji: String
    }

    private static let allChannels: [RadioChannel] = [
        RadioChannel(id: "soluna", name: "Soluna", emoji: "🌀"),
        RadioChannel(id: "jazz",   name: "Jazz",   emoji: "🎷"),
        RadioChannel(id: "lofi",   name: "Lo-Fi",  emoji: "📻"),
        RadioChannel(id: "chill",  name: "Chill",  emoji: "🌅"),
        RadioChannel(id: "dance",  name: "Dance",  emoji: "💃"),
        RadioChannel(id: "bjj",    name: "BJJ",    emoji: "🥋"),
        RadioChannel(id: "yuki",   name: "Yuki",   emoji: "❄️"),
    ]

    var body: some View {
        ScrollView {
            VStack(spacing: 16) {
                // Status
                HStack(spacing: 6) {
                    Circle()
                        .fill(connector.isPlaying ? Color.green : Color.gray)
                        .frame(width: 8, height: 8)
                    Text(connector.isPlaying ? "Receiving" : "Stopped")
                        .font(.caption2)
                        .foregroundColor(.secondary)
                }

                // Channel name
                Text(connector.channel.isEmpty ? "—" : connector.channel)
                    .font(.headline)
                    .lineLimit(1)

                // Now Playing
                if let title = connector.nowPlayingTitle {
                    VStack(spacing: 2) {
                        Text(title)
                            .font(.caption)
                            .lineLimit(1)
                        if let artist = connector.nowPlayingArtist {
                            Text(artist)
                                .font(.caption2)
                                .foregroundColor(.secondary)
                                .lineLimit(1)
                        }
                    }
                }

                // Play / Stop
                Button {
                    connector.sendCommand(connector.isPlaying ? "stop" : "play")
                } label: {
                    Image(systemName: connector.isPlaying ? "stop.fill" : "play.fill")
                        .font(.title2)
                        .frame(width: 60, height: 60)
                        .background(connector.isPlaying ? Color.red.opacity(0.2) : Color.green.opacity(0.2))
                        .clipShape(Circle())
                }
                .buttonStyle(.plain)

                // Volume
                HStack {
                    Image(systemName: "speaker.fill")
                        .font(.caption2)
                    Slider(value: $connector.volume, in: 0...1) { editing in
                        if !editing {
                            connector.sendCommand("volume:\(connector.volume)")
                        }
                    }
                    Image(systemName: "speaker.wave.3.fill")
                        .font(.caption2)
                }

                // Signal quality
                HStack(spacing: 4) {
                    ForEach(1...4, id: \.self) { bar in
                        RoundedRectangle(cornerRadius: 1)
                            .fill(bar <= connector.signalQuality ? qualityColor(connector.signalQuality) : Color.gray.opacity(0.3))
                            .frame(width: 4, height: CGFloat(bar) * 3 + 4)
                    }
                    if connector.latencyMs > 0 {
                        Text(String(format: "%.0fms", connector.latencyMs))
                            .font(.system(size: 10, design: .monospaced))
                            .foregroundColor(.secondary)
                    }
                }

                // Channel switcher (all 7 radio channels)
                Divider()
                Text("Channels")
                    .font(.caption2)
                    .foregroundColor(.secondary)
                ForEach(Self.allChannels, id: \.id) { ch in
                    Button {
                        connector.sendCommand("channel:\(ch.id)")
                    } label: {
                        HStack {
                            Text("\(ch.emoji) \(ch.name)")
                                .font(.caption)
                            Spacer()
                            if ch.id == connector.channel {
                                Image(systemName: "checkmark")
                                    .font(.caption2)
                                    .foregroundColor(.blue)
                            }
                        }
                    }
                }
            }
            .padding()
        }
        .navigationTitle("Soluna")
    }

    private func qualityColor(_ q: Int) -> Color {
        switch q {
        case 1: return .red
        case 2: return .orange
        case 3: return .yellow
        default: return .green
        }
    }
}
