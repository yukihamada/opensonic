# Migration Guide

This guide helps you migrate from previous Soluna audio implementations to the unified SolunaSDK.

---

## From Koe's SolunaManager to SolunaSDK

### Step 1: Replace the dependency

**Before (Koe):**
```swift
// Embedded in the Koe app, no separate package
import Foundation
```

**After (SolunaSDK):**
```swift
// Package.swift
.package(url: "https://github.com/yukihamada/opensonic.git", from: "1.0.0")
```

### Step 2: Replace imports

**Before:**
```swift
import Foundation
import AVFoundation
// Manual socket code in SolunaManager
```

**After:**
```swift
import SolunaSDK
```

### Step 3: Replace initialization

**Before (SolunaManager):**
```swift
class SolunaManager: ObservableObject {
    @Published var isConnected = false
    @Published var isReceiving = false
    private var socket: Int32 = -1
    private var audioEngine = AVAudioEngine()
    private var playerNode = AVAudioPlayerNode()

    func startListening(channel: String, host: String, port: UInt16) {
        // 100+ lines of socket setup, audio engine config, receive loop...
    }

    func stopListening() {
        // Manual cleanup...
    }
}
```

**After (SolunaClient):**
```swift
let client = SolunaClient()
client.connect(channel: "soluna")
// That's it. Connection, audio engine, heartbeat, etc. are all handled.

client.disconnect()
```

### Step 4: Replace UI code

**Before:**
```swift
// Custom player view with manual state binding
HStack {
    Text(isConnected ? "Connected" : "Disconnected")
    Button(isPlaying ? "Stop" : "Play") {
        isPlaying ? manager.stopListening() : manager.startListening(...)
    }
    Slider(value: $volume, in: 0...1)
}
```

**After:**
```swift
// Drop-in SwiftUI components
SolunaPlayerView(client: client)
SolunaChannelPicker(client: client)
SolunaStatusBadge(client: client)
```

### Step 5: Replace P2P discovery

**Before:**
```swift
// Manual MultipeerConnectivity setup
class PeerManager: NSObject, MCNearbyServiceBrowserDelegate, MCNearbyServiceAdvertiserDelegate {
    // 50+ lines of boilerplate...
}
```

**After:**
```swift
client.startPeerDiscovery()
client.peerDiscovery.onPeerFound = { peerId, data in
    print("Found: \(peerId)")
}
```

### Step 6: Remove old files

Delete these files from your Koe project:
- `SolunaManager.swift`
- `SolunaAudioPlayer.swift` (if separate)
- `SolunaPeerDiscovery.swift`
- `SolunaMultiOutput.swift`
- Any `OSTP*` or `ADPCM*` files

---

## From Soluna iOS AudioReceiver to SolunaSDK

### Side-by-side comparison

| Old (AudioReceiver)                          | New (SolunaSDK)                          |
|----------------------------------------------|------------------------------------------|
| `AudioReceiver(channel:host:port:)`          | `SolunaClient()` + `.connect(channel:)`  |
| `receiver.start()`                           | `client.connect(channel: "jazz")`        |
| `receiver.stop()`                            | `client.disconnect()`                    |
| `receiver.isReceiving`                       | `client.isReceivingAudio`                |
| `receiver.currentChannel`                    | `client.channel` / `client.currentChannel` |
| `receiver.switchChannel(to:)`                | `client.setChannel("lofi")`             |
| Manual `AVAudioSession` configuration        | Automatic (handled by SolunaClient)      |
| Manual heartbeat timer                       | Automatic (RelayConnection)              |
| `receiver.delegate = self`                   | `client.delegate = self`                 |

### Code migration

**Before:**
```swift
class ViewController: UIViewController, AudioReceiverDelegate {
    let receiver = AudioReceiver(channel: "soluna", host: "relay.solun.art", port: 5100)

    override func viewDidLoad() {
        super.viewDidLoad()
        receiver.delegate = self
        receiver.start()
    }

    func audioReceiver(_ receiver: AudioReceiver, didReceiveBuffer buffer: AVAudioPCMBuffer) {
        // Process audio...
    }
}
```

**After:**
```swift
struct ContentView: View {
    @StateObject private var client = SolunaClient()

    var body: some View {
        VStack {
            SolunaPlayerView(client: client)
            SolunaChannelPicker(client: client)
        }
        .onAppear { client.connect(channel: "soluna") }
        .onDisappear { client.disconnect() }
    }
}
```

---

## From Soluna Mac AudioReceiver to SolunaSDK

### Key differences

The macOS version had manual CoreAudio device enumeration and multi-output management. SolunaSDK wraps this in `MultiOutputManager`.

**Before:**
```swift
// Manual CoreAudio device enumeration
func listOutputDevices() -> [AudioDeviceID] {
    var prop = AudioObjectPropertyAddress(...)
    // 40+ lines of CoreAudio boilerplate
}

func addOutputDevice(_ deviceID: AudioDeviceID) {
    // Manual aggregate device creation...
}
```

**After:**
```swift
client.refreshOutputDevices()
for device in client.multiOutput.availableDevices {
    print("\(device.name) - \(device.outputChannels)ch")
}
client.addOutputDevice(deviceID: device.id)
client.setOutputDeviceVolume(deviceID: device.id, volume: 0.8)
```

---

## Step-by-Step Migration Checklist

### 1. Add SPM dependency

In Xcode: **File > Add Package Dependencies**

Repository URL:
```
https://github.com/yukihamada/opensonic.git
```

Version rule: **Up to Next Major** from `1.0.0`

### 2. Replace imports

Find and replace in your project:

```
// Find:
import AVFoundation  // (if only used for Soluna audio)

// Replace with:
import SolunaSDK
```

### 3. Swap initialization

Replace your custom manager/receiver class with `SolunaClient`:

```swift
// Old
let manager = SolunaManager()
manager.startListening(channel: "soluna", host: "relay.solun.art", port: 5100)

// New
let client = SolunaClient()
client.connect(channel: "soluna")
```

All defaults (host: `relay.solun.art`, port: `5100`) match the old implementation.

### 4. Remove old bridge files

Delete any of these files that were copied into your project:
- `AudioReceiver.swift`
- `SolunaManager.swift`
- `OSTPacketParser.swift` (old standalone version)
- `ADPCMDecoder.swift`
- `RelaySocket.swift`
- `PeerDiscoveryManager.swift`
- `MultiOutputBridge.swift`

### 5. Update SwiftUI views

Replace manual player UIs with SDK components:

```swift
// Old: custom HStack with buttons and sliders
// New:
SolunaPlayerView(client: client)
SolunaChannelPicker(client: client)
SolunaFanRankBadge(client: client)
```

### 6. Verify and test

Build and run. The SDK uses the same OSTP protocol and relay infrastructure, so existing relay servers require no changes.

Key things to verify:
- Audio playback works on the same channels
- P2P discovery finds the same peers
- Lock Screen / Control Center controls work
- Multi-output routing works (macOS)

---

## FAQ

**Q: Do I need to update the relay server?**
A: No. SolunaSDK uses the same OSTP protocol. Existing relay servers are fully compatible.

**Q: Can I use SolunaSDK alongside the old code?**
A: Not recommended. Both implementations will compete for the audio session and UDP socket. Migrate fully.

**Q: What about my custom ADPCM decoder?**
A: SolunaSDK includes `ADPCMCodec` which matches the C++ reference implementation. Remove your custom version.

**Q: Does SolunaSDK support the same audio formats?**
A: Yes. Raw int32, IMA-ADPCM (stereo/mono), and Opus are all supported. LC3 support is planned.
