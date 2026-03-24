import SwiftUI
import SolunaSDK

@main
struct SolunaDemoApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}

struct ContentView: View {
    @StateObject private var client = SolunaClient()
    @State private var showChannels = false

    private let nowPlaying = NowPlayingManager()

    var body: some View {
        NavigationStack {
            VStack(spacing: 24) {
                // Status
                HStack {
                    SolunaStatusBadge(client: client)
                    Text(client.isReceivingAudio ? "Streaming" : client.isConnected ? "Connected" : "Idle")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                    Spacer()
                    SolunaFanRankBadge(client: client)
                }

                Spacer()

                // Current channel display
                if let ch = client.currentChannel {
                    Text(ch.emoji)
                        .font(.system(size: 80))
                    Text(ch.name)
                        .font(.largeTitle.bold())
                }

                Spacer()

                // Mic transmit toggle
                Button {
                    client.toggleMicTransmit()
                } label: {
                    Label(
                        client.isMicTransmitting ? "Stop Mic" : "Transmit Mic",
                        systemImage: client.isMicTransmitting ? "mic.fill" : "mic"
                    )
                    .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .tint(client.isMicTransmitting ? .red : .blue)

                // Channel picker
                SolunaChannelPicker(client: client)

                // Player bar
                SolunaPlayerView(client: client)
            }
            .padding()
            .navigationTitle("Soluna Demo")
            .navigationBarTitleDisplayMode(.inline)
        }
        .onAppear {
            client.connect(channel: "soluna")
            setupNowPlaying()
        }
        .onDisappear {
            client.disconnect()
        }
    }

    private func setupNowPlaying() {
        nowPlaying.setupRemoteCommands(
            onPlay:  { client.connect(channel: client.channel) },
            onPause: { client.disconnect() },
            onNext: {
                let ids = client.channels.map(\.id)
                if let idx = ids.firstIndex(of: client.channel) {
                    client.setChannel(ids[(idx + 1) % ids.count])
                }
            }
        )
        if let ch = client.currentChannel {
            nowPlaying.update(channel: ch)
        }
    }
}
