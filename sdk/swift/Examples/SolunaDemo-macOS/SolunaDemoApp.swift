import SwiftUI
import SolunaSDK

@main
struct SolunaDemoApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
        .windowStyle(.titleBar)
        .defaultSize(width: 500, height: 600)
    }
}

struct ContentView: View {
    @StateObject private var client = SolunaClient()
    @State private var djFileA: URL?
    @State private var djFileB: URL?
    @State private var crossfader: Float = 0.5

    var body: some View {
        VStack(spacing: 20) {
            // Player bar
            SolunaPlayerView(client: client)

            // Channel picker
            SolunaChannelPicker(client: client)

            Divider()

            // Multi-output device list
            Text("Output Devices")
                .font(.headline)

            ForEach(client.multiOutput.availableDevices) { device in
                HStack {
                    let isActive = client.multiOutput.activeOutputIDs.contains(device.id)
                    Toggle(isOn: Binding(
                        get: { isActive },
                        set: { on in
                            if on { client.addOutputDevice(deviceID: device.id) }
                            else  { client.removeOutputDevice(deviceID: device.id) }
                        }
                    )) {
                        Text(device.name)
                            .font(.body)
                    }

                    if isActive {
                        Slider(
                            value: Binding(
                                get: { Double(client.multiOutput.deviceVolumes[device.id] ?? 1.0) },
                                set: { client.setOutputDeviceVolume(deviceID: device.id, volume: Float($0)) }
                            ),
                            in: 0...1
                        )
                        .frame(width: 100)
                    }
                }
            }

            Divider()

            // DJ Deck demo
            Text("DJ Deck")
                .font(.headline)

            HStack(spacing: 16) {
                Button("Load Deck A") {
                    let panel = NSOpenPanel()
                    panel.allowedContentTypes = [.audio]
                    if panel.runModal() == .OK, let url = panel.url {
                        client.loadDeckA(url: url)
                    }
                }

                Slider(value: Binding(
                    get: { Double(crossfader) },
                    set: { crossfader = Float($0); client.setDJCrossfader(crossfader) }
                ), in: 0...1)
                .frame(width: 140)

                Button("Load Deck B") {
                    let panel = NSOpenPanel()
                    panel.allowedContentTypes = [.audio]
                    if panel.runModal() == .OK, let url = panel.url {
                        client.loadDeckB(url: url)
                    }
                }
            }

            HStack {
                Text("A")
                    .foregroundStyle(crossfader < 0.5 ? .primary : .secondary)
                Spacer()
                Text("B")
                    .foregroundStyle(crossfader > 0.5 ? .primary : .secondary)
            }
            .font(.caption)
            .padding(.horizontal, 40)

            Spacer()
        }
        .padding()
        .onAppear {
            client.connect(channel: "soluna")
            client.refreshOutputDevices()
        }
        .onDisappear {
            client.disconnect()
        }
    }
}
