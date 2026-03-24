# Getting Started with SolunaSDK

This guide walks you through adding SolunaSDK to your project and streaming your first audio channel in under 5 minutes.

---

## Step 1: Add Package Dependency

### Xcode (Recommended)

1. Open your project in Xcode
2. Go to **File > Add Package Dependencies...**
3. Enter the repository URL:
   ```
   https://github.com/yukihamada/opensonic.git
   ```
4. Set the version rule to **Up to Next Major Version** from `1.0.0`
5. Click **Add Package**
6. Select **SolunaSDK** as the product to add to your target

### Package.swift

If you use a `Package.swift` file, add the dependency:

```swift
// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "MyApp",
    platforms: [.iOS(.v16), .macOS(.v13)],
    dependencies: [
        .package(url: "https://github.com/yukihamada/opensonic.git", from: "1.0.0"),
    ],
    targets: [
        .executableTarget(
            name: "MyApp",
            dependencies: [
                .product(name: "SolunaSDK", package: "opensonic"),
            ]
        ),
    ]
)
```

---

## Step 2: Import SolunaSDK

In any Swift file where you want to use the SDK:

```swift
import SolunaSDK
```

This single import gives you access to all SDK types: `SolunaClient`, `SolunaPlayerView`, `SolunaChannelPicker`, and every other component.

---

## Step 3: Create a SolunaClient

`SolunaClient` is the main entry point. It's an `ObservableObject` designed for SwiftUI, but works with UIKit too.

```swift
import SwiftUI
import SolunaSDK

struct ContentView: View {
    @StateObject private var client = SolunaClient()

    var body: some View {
        Text("Hello, Soluna!")
    }
}
```

The client manages:
- Relay server connection (UDP)
- Audio engine setup and playback
- Heartbeat keep-alive
- Channel metadata
- Fan rank tracking

---

## Step 4: Connect to a Channel

Call `connect(channel:)` to start streaming audio from a channel:

```swift
struct ContentView: View {
    @StateObject private var client = SolunaClient()

    var body: some View {
        VStack {
            Text(client.isConnected ? "Connected" : "Disconnected")
            Text(client.isReceivingAudio ? "Streaming audio" : "Waiting for audio...")

            Button(client.isConnected ? "Disconnect" : "Connect") {
                if client.isConnected {
                    client.disconnect()
                } else {
                    client.connect(channel: "jazz")
                }
            }
        }
        .onAppear {
            client.connect(channel: "jazz")
        }
        .onDisappear {
            client.disconnect()
        }
    }
}
```

### Available Channels

| Channel ID | Name   | Description           |
|-----------|--------|-----------------------|
| `bjj`     | BJJ    | Brazilian Jiu-Jitsu   |
| `soluna`  | Soluna | Soluna original       |
| `jazz`    | Jazz   | Jazz music            |
| `chill`   | Chill  | Chill / ambient       |
| `lofi`    | Lo-Fi  | Lo-fi hip hop         |
| `dance`   | Dance  | Dance / electronic    |
| `yuki`    | Yuki   | Yuki's personal       |

You can also use any custom string as a channel name.

---

## Step 5: Add SwiftUI Player Views

SolunaSDK includes ready-made SwiftUI components. Drop them into your view hierarchy:

```swift
struct ContentView: View {
    @StateObject private var client = SolunaClient()

    var body: some View {
        VStack(spacing: 20) {
            // Channel grid -- tap to switch channels
            SolunaChannelPicker(client: client)

            Spacer()

            // Compact player bar with play/pause and volume
            SolunaPlayerView(client: client)

            // Connection status dot (green/yellow/red)
            HStack {
                SolunaStatusBadge(client: client)
                Text(statusText)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            // Fan rank badge
            SolunaFanRankBadge(client: client)
        }
        .padding()
        .onAppear {
            client.connect(channel: "jazz")
        }
        .onDisappear {
            client.disconnect()
        }
    }

    private var statusText: String {
        if client.isReceivingAudio { return "Streaming" }
        if client.isConnected { return "Connected" }
        return "Disconnected"
    }
}
```

### Component Summary

| Component              | Purpose                                          |
|-----------------------|--------------------------------------------------|
| `SolunaPlayerView`    | Mini-player bar (emoji, name, play/pause, volume)|
| `SolunaChannelPicker` | Adaptive grid of channels with selection state   |
| `SolunaVolumeSlider`  | Custom volume slider with speaker icon           |
| `SolunaStatusBadge`   | Green/yellow/red dot for connection status        |
| `SolunaFanRankBadge`  | Fan rank emoji and label in a capsule badge      |

---

## Complete Minimal App

Here is a complete, working iOS app in a single file:

```swift
import SwiftUI
import SolunaSDK

@main
struct MinimalSolunaApp: App {
    var body: some Scene {
        WindowGroup {
            RadioView()
        }
    }
}

struct RadioView: View {
    @StateObject private var client = SolunaClient()

    var body: some View {
        VStack(spacing: 20) {
            SolunaChannelPicker(client: client)
            Spacer()
            SolunaPlayerView(client: client)
        }
        .padding()
        .onAppear { client.connect(channel: "soluna") }
        .onDisappear { client.disconnect() }
    }
}
```

---

## What's Next

Now that you have basic playback working, explore these advanced features:

### Transmit Audio
Send your microphone to the relay so others on the channel can hear you:
```swift
client.startMicTransmit()
```
See the [README](../README.md#mic-transmit) for details.

### DJ Mixing
Load two audio files and crossfade between them:
```swift
client.loadDeckA(url: trackA)
client.loadDeckB(url: trackB)
client.setDJCrossfader(0.5)
```
See the [README](../README.md#dj-deck) for details.

### P2P Discovery
Find nearby devices on the same channel:
```swift
client.startPeerDiscovery()
```
See the [README](../README.md#p2p-discovery) for details.

### Multi-Device Sync
Synchronize audio playback across multiple devices:
```swift
let sync = DeviceSyncManager()
sync.attach(connection: relayConnection)
sync.enableSync()
```
See [ARCHITECTURE.md](ARCHITECTURE.md) for the sync protocol details.

### E2E Encryption
Encrypt audio between two peers:
```swift
let encryption = E2EEncryption()
encryption.generateKeyPair()
encryption.enable(peerPublicKey: peerKey)
```
See the [README](../README.md#e2e-encryption) for the full flow.

### Lock Screen Integration
Show playback controls on the Lock Screen and Control Center:
```swift
let nowPlaying = NowPlayingManager()
nowPlaying.setupRemoteCommands(onPlay: { ... }, onPause: { ... }, onNext: { ... })
```

### Offline Caching
Cache audio for offline playback:
```swift
let cache = OfflineCache()
cache.isEnabled = true
```

### Audio Analysis
Get real-time beat detection and frequency levels:
```swift
let analyzer = AudioAnalyzer()
analyzer.onBeat = { /* pulse LED or haptic */ }
```

### Multi-Output (macOS)
Route audio to multiple speakers:
```swift
client.refreshOutputDevices()
client.addOutputDevice(deviceID: device.id)
```

---

## Troubleshooting

### No audio playing
1. Verify the channel has an active transmitter (someone must be broadcasting)
2. Check that your device is not in silent mode (iOS)
3. Ensure no other app has exclusive audio session control

### Connection fails
1. Verify network connectivity
2. Check that UDP port 5100 is not blocked by a firewall
3. The default relay is `relay.solun.art:5100` -- verify DNS resolves

### Build errors
1. Ensure your deployment target is iOS 16+ or macOS 13+
2. Ensure Swift 5.9+ / Xcode 15+
3. Clean build folder: **Product > Clean Build Folder** (Shift+Cmd+K)

---

## Further Reading

- [README.md](../README.md) -- Full API reference with code examples
- [ARCHITECTURE.md](ARCHITECTURE.md) -- Internal architecture and data flows
- [MIGRATION.md](MIGRATION.md) -- Migrating from older Soluna implementations
- [CHANGELOG.md](../CHANGELOG.md) -- Release history
