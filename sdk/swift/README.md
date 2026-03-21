# SolunaSDK

```
  ____        _
 / ___|  ___ | |_   _ _ __   __ _
 \___ \ / _ \| | | | | '_ \ / _` |
  ___) | (_) | | |_| | | | | (_| |
 |____/ \___/|_|\__,_|_| |_|\__,_|  SDK for Swift
```

![Swift 5.9+](https://img.shields.io/badge/Swift-5.9+-orange)
![iOS 16+](https://img.shields.io/badge/iOS-16%2B-blue)
![macOS 13+](https://img.shields.io/badge/macOS-13%2B-blue)
![SPM Compatible](https://img.shields.io/badge/SPM-Compatible-green)
![License](https://img.shields.io/badge/License-Apache%202.0-blue)

**Full-featured audio streaming SDK for iOS and macOS.**

SolunaSDK connects your app to the Soluna relay network, enabling real-time audio streaming, P2P discovery, DJ mixing, multi-device sync, and end-to-end encrypted audio channels -- all in pure Swift with zero external dependencies.

---

## Quick Start

```swift
import SolunaSDK

let client = SolunaClient()
client.connect(channel: "jazz")

// In SwiftUI:
SolunaPlayerView(client: client)
SolunaChannelPicker(client: client)
```

That's it. Five lines to stream live audio.

---

## Features

### Core Audio
- **AudioPlayer** -- AVAudioEngine-based playback (48kHz stereo float32)
- **ADPCMCodec** -- IMA-ADPCM encoder/decoder for bandwidth-efficient audio
- **OSTPacketParser / OSTPacketBuilder** -- OSTP/RTP packet serialization
- **AsyncAudioStream** -- Swift concurrency `AsyncSequence` for audio data
- **AudioAnalyzer** -- FFT-based beat detection and frequency band levels (bass/mid/high)

### Networking
- **RelayConnection** -- BSD socket UDP connection to relay servers
- **ConnectionPool** -- Manage multiple simultaneous relay connections
- **MultiRegionRouter** -- Automatic routing to the closest relay region
- **LoadBalancer** -- Client-side load balancing across relay instances
- **ProxyTunnel** -- Tunnel audio through HTTP/SOCKS proxies for restricted networks
- **BandwidthController** -- Adaptive bitrate based on network conditions
- **WebhookManager** -- Outbound webhooks for channel events

### Playback & Transmit
- **MicTransmitter** -- Capture mic audio and send OSTP packets to relay
- **DJDeckController** -- Dual-deck DJ controller with equal-power crossfade
- **NowPlayingManager** -- Lock Screen / Control Center media integration
- **AudioRecorder** -- Record received audio to file
- **OfflineCache** -- LRU disk cache for offline playback (.caf files)
- **NoiseCanceller** -- Real-time noise cancellation for mic input
- **EchoCanceller** -- Acoustic echo cancellation
- **AutoGainControl** -- Automatic mic gain adjustment
- **AudioWatermark** -- Embed/extract inaudible watermarks in audio streams
- **TextToSpeech** -- System TTS announcements over audio channels
- **VoiceCommands** -- Voice-activated channel control
- **SpeechTranscriber** -- Live speech-to-text using Apple Speech framework

### Discovery & Sync
- **MultipeerDiscovery** (iOS) -- Discover peers via MultipeerConnectivity
- **BonjourDiscovery** (macOS) -- Discover peers via NWBrowser/Bonjour
- **DeviceSyncManager** -- NTP-like wall-clock sync for multi-device playout
- **MultiOutputManager** (macOS) -- Route audio to multiple CoreAudio output devices

### Security & Privacy
- **E2EEncryption** -- AES-GCM 256-bit encryption with ECDH P256 key exchange
- **AuthManager** -- Token-based authentication for relay access
- **GDPRManager** -- Data privacy controls and consent management
- **AuditLogger** -- Security event audit trail

### Analytics & Monitoring
- **AnalyticsTracker** -- Per-channel listening stats, streaks, peak hours
- **MetricsExporter** -- Export Prometheus/OpenMetrics format metrics
- **HealthCheck** -- SDK health and connectivity diagnostics

### Platform & Integration
- **RemoteConfig** -- Server-driven feature flags and configuration
- **IntegrationHub** -- Connect to third-party services (Spotify, Apple Music, etc.)
- **BillingSDK** -- In-app purchase and subscription management
- **CopyrightManager** -- Content fingerprinting and rights management
- **AdminAPI** -- Server administration and channel management
- **GeoFence** -- Location-based channel access control
- **AccessibilityHelper** -- VoiceOver and accessibility support
- **Localization** -- Multi-language string management

### SwiftUI Components
- **SolunaPlayerView** -- Compact mini-player bar with play/pause and volume
- **SolunaChannelPicker** -- Grid of available channels with selection
- **SolunaVolumeSlider** -- Custom wave-style volume slider
- **SolunaStatusBadge** -- Connection status dot indicator (green/yellow/red)
- **SolunaFanRankBadge** -- Fan rank display with emoji badge

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        Your App                             │
│                                                             │
│   SolunaPlayerView  SolunaChannelPicker  SolunaFanRankBadge │
├─────────────────────────────────────────────────────────────┤
│                      SolunaClient                           │
│         (ObservableObject, @MainActor)                      │
├──────────┬──────────┬──────────┬──────────┬─────────────────┤
│ Audio    │ Network  │ Transmit │ Discover │ Analytics       │
│          │          │          │          │                 │
│ Audio    │ Relay    │ Mic      │ Peer     │ Analytics       │
│ Player   │ Connect  │ Transmit │ Discovery│ Tracker         │
│          │          │          │          │                 │
│ ADPCM    │ Conn     │ DJ Deck  │ Device   │ Now Playing     │
│ Codec    │ Pool     │ Ctrl     │ Sync Mgr │ Manager         │
│          │          │          │          │                 │
│ Audio    │ Multi    │ Noise    │ Bonjour  │ Offline         │
│ Analyzer │ Region   │ Cancel   │ (macOS)  │ Cache           │
├──────────┴──────────┴──────────┴──────────┴─────────────────┤
│ Security: E2EEncryption  AuthManager  AuditLogger           │
├─────────────────────────────────────────────────────────────┤
│ OSTP Protocol: OSTPacketParser / OSTPacketBuilder           │
│ Transport: UDP (BSD sockets) / WebSocket                    │
└─────────────────────────────────────────────────────────────┘
```

### Data Flow (Receive)

```
Relay Server relay.solun.art:5100 (AWS Tokyo, UDP)
    │
    ▼
RelayConnection (recvfrom loop)
    │
    ▼
OSTPacketParser.parse(data)
    │
    ├── ADPCM → ADPCMCodec.decodePayload()
    ├── Opus  → AVAudioConverter (AudioToolbox)
    ├── LC3   → (liblc3 bridge, planned)
    └── Raw   → S24-in-S32LE → float32 (scale 1.0/2^23)
    │
    ▼
SDKAudioReceiver (AVAudioSourceNode, pull-based)
    ├── Lock-free SPSC mono ring buffer (192K Float, 4s @ 48kHz)
    └── 100ms prefill threshold before playback
    │
    ▼
AVAudioEngine → CoreAudio → Speakers
```

### Data Flow (Transmit)

```
Microphone / DJ Deck
    │
    ▼
AVAudioEngine inputNode tap (48kHz float32)
    │
    ▼
MicTransmitter / DJDeckController
    │ Convert to int32 LE / int16 LE
    ▼
OSTPacketBuilder.buildPacket()
    │ RTP header + OSTP extension + payload + CRC32
    ▼
RelayConnection.sendRawData()
    │
    ▼
Relay Server → All peers on channel
```

---

## Installation

### Swift Package Manager (Recommended)

Add to your `Package.swift`:

```swift
dependencies: [
    .package(url: "https://github.com/yukihamada/opensonic.git", from: "1.0.0")
]
```

Or in Xcode: **File > Add Package Dependencies** and enter the repository URL.

### CocoaPods

```ruby
pod 'SolunaSDK', '~> 1.0'
```

### Carthage

```
github "yukihamada/opensonic" ~> 1.0
```

### XCFramework

Download the pre-built `SolunaSDK.xcframework` from the [GitHub Releases](https://github.com/yukihamada/opensonic/releases) page and drag it into your Xcode project.

---

## API Examples

### Basic Playback

```swift
import SolunaSDK

@StateObject private var client = SolunaClient()

var body: some View {
    VStack {
        SolunaPlayerView(client: client)
        SolunaChannelPicker(client: client)
    }
    .onAppear {
        client.connect(channel: "jazz")
    }
    .onDisappear {
        client.disconnect()
    }
}
```

### Channel Switching

```swift
// Switch channel (auto-reconnects)
client.setChannel("lofi")

// Access channel metadata
if let ch = client.currentChannel {
    print("\(ch.emoji) \(ch.name)")  // "Lo-Fi"
}

// List all channels
for channel in client.channels {
    print("\(channel.emoji) \(channel.name) — \(channel.color)")
}
```

### Mic Transmit

```swift
// Start sending mic audio to the relay
client.startMicTransmit()

// Check transmit state
if client.isMicTransmitting {
    // Show peak level meter
    let level = client.micTransmitter.peakLevel
}

// Stop transmitting
client.stopMicTransmit()
```

### DJ Deck

```swift
// Load tracks into dual decks
client.loadDeckA(url: trackAURL)
client.loadDeckB(url: trackBURL)

// Crossfade: 0.0 = full A, 0.5 = equal mix, 1.0 = full B
client.setDJCrossfader(0.7)

// Direct deck access
client.djController.deckA.pause()
client.djController.deckB.seek(to: 0.5) // 50%
print(client.djController.deckA.progress) // 0.0 - 1.0

// Stop all decks
client.stopDJDecks()
```

### P2P Discovery

```swift
// Start scanning for nearby peers
client.startPeerDiscovery()

// Handle discovered peers
client.peerDiscovery.onPeerFound = { peerId, data in
    print("Found peer: \(peerId)")
}

client.peerDiscovery.onPeerLost = { peerId in
    print("Lost peer: \(peerId)")
}

// Stop scanning
client.stopPeerDiscovery()
```

### Multi-Device Sync

```swift
let sync = DeviceSyncManager()
sync.attach(connection: relayConnection)
sync.enableSync()

// Read sync offset for audio scheduling
print("Clock offset: \(sync.syncOffset)s")
print("Group max delay: \(sync.maxDelayMs)ms")

// Handle relay control messages
relayConnection.onControlMessage = { message in
    sync.handleControlMessage(message)
}
```

### E2E Encryption

```swift
let encryption = E2EEncryption()

// Generate key pair and share public key
let publicKey = encryption.generateKeyPair()
// Send publicKey to peer via relay control channel...

// When peer's public key arrives:
encryption.enable(peerPublicKey: peerPublicKeyData)

// Encrypt outgoing audio
let encrypted = try encryption.encrypt(payload: audioData)

// Decrypt incoming audio
if let decrypted = encryption.decrypt(payload: encryptedData) {
    // Process decrypted audio
}
```

### SwiftUI Integration

```swift
import SwiftUI
import SolunaSDK

struct RadioView: View {
    @StateObject private var client = SolunaClient()

    var body: some View {
        VStack(spacing: 20) {
            // Channel grid
            SolunaChannelPicker(client: client)

            // Mini player bar
            SolunaPlayerView(client: client)

            // Status indicator
            HStack {
                SolunaStatusBadge(client: client)
                Text(client.isReceivingAudio ? "Streaming" : "Idle")
            }

            // Fan rank badge
            SolunaFanRankBadge(client: client)
        }
        .padding()
    }
}
```

### Offline Cache

```swift
let cache = OfflineCache()
cache.isEnabled = true
cache.maxCacheSize = 200 * 1024 * 1024 // 200 MB

// Write audio buffers as they arrive
cache.writeBuffer(pcmBuffer, channel: "jazz", title: "Late Night Jazz")

// List cached tracks
let tracks = cache.cachedTracks(for: "jazz")

// Play offline
if let track = tracks.first {
    cache.playOffline(track: track, engine: audioEngine, playerNode: playerNode)
}

// Clear all cached data
cache.clearCache()
```

### Analytics

```swift
let tracker = AnalyticsTracker()
tracker.startSession(channel: "jazz")

// ... user listens ...

tracker.endSession()

print("Total: \(tracker.totalListenMinutes) min")
print("Streak: \(tracker.currentStreak) days")
print("Peak hour: \(tracker.peakHour):00")
print("Jazz sessions: \(tracker.channelStats["jazz"]?.sessionCount ?? 0)")
```

### Audio Analysis (Beat Detection)

```swift
let analyzer = AudioAnalyzer()
analyzer.onBeat = {
    // Trigger LED, haptic, or visual effect
}

// Feed audio from delegate
func solunaClient(_ client: SolunaClient, didReceiveAudio samples: [Float],
                   channels: Int, sampleRate: Double) {
    analyzer.feed(samples: samples, channels: channels, sampleRate: sampleRate)
}

// Use levels in SwiftUI
Circle()
    .scaleEffect(CGFloat(analyzer.bassLevel) * 2)
    .animation(.easeOut(duration: 0.1), value: analyzer.bassLevel)
```

### Multi-Output (macOS)

```swift
#if os(macOS)
// List output devices
client.refreshOutputDevices()
for device in client.multiOutput.availableDevices {
    print("\(device.name) — \(device.outputChannels)ch, \(device.transportType)")
}

// Route to multiple devices simultaneously
client.addOutputDevice(deviceID: device.id)
client.setOutputDeviceVolume(deviceID: device.id, volume: 0.8)
#endif
```

### NowPlaying (Lock Screen / Control Center)

```swift
let nowPlaying = NowPlayingManager()

nowPlaying.setupRemoteCommands(
    onPlay:  { client.connect(channel: client.channel) },
    onPause: { client.disconnect() },
    onNext:  { client.setChannel("jazz") }
)

if let channel = client.currentChannel {
    nowPlaying.update(channel: channel, title: "Live Radio")
}
nowPlaying.setPlaybackState(client.isConnected)
```

### Delegate for Raw Audio

```swift
class AudioProcessor: SolunaClientDelegate {
    func solunaClient(_ client: SolunaClient,
                      didReceiveAudio samples: [Float],
                      channels: Int, sampleRate: Double) {
        // Process raw PCM: visualization, recording, analysis, etc.
    }

    func solunaClient(_ client: SolunaClient,
                      didChangeState state: SolunaConnectionState) {
        switch state {
        case .connected:    print("Connected")
        case .disconnected: print("Disconnected")
        case .error(let msg): print("Error: \(msg)")
        case .connecting:   print("Connecting...")
        }
    }
}
```

---

## Requirements

| Platform | Minimum Version |
|----------|----------------|
| iOS      | 16.0           |
| macOS    | 13.0           |
| Swift    | 5.9            |
| Xcode    | 15.0           |

### System Frameworks Used

- **AVFoundation** -- Audio engine, session management, file I/O
- **CryptoKit** -- AES-GCM encryption, ECDH key exchange
- **MediaPlayer** -- Now Playing info and remote commands
- **Accelerate** -- vDSP FFT for audio analysis
- **Speech** -- Live speech-to-text transcription
- **StoreKit** -- In-app purchases (BillingSDK)
- **Network** -- NWBrowser/NWListener for Bonjour discovery (macOS)
- **MultipeerConnectivity** -- Peer discovery (iOS)

---

## Available Channels

| Channel | Emoji | Color   |
|---------|-------|---------|
| BJJ     | (martial arts)  | #E53E3E |
| Soluna  | (spiral) | #ED8936 |
| Jazz    | (sax)  | #D69E2E |
| Chill   | (sunset)  | #38B2AC |
| Lo-Fi   | (radio)  | #805AD5 |
| Dance   | (dance)  | #D53F8C |
| Yuki    | (snowflake)  | #63B3ED |

All 7 radio channels are free (kFreeNames) -- anyone joining gets DJ role automatically.

Custom channels are supported by passing any string to `connect(channel:)`.

---

## Protocol

SolunaSDK uses the **OSTP (Open Sonic Transport Protocol)**, an RTP-based protocol with custom header extensions:

- **Profile ID**: `0x4F53` ("OS")
- **Default relay**: `relay.solun.art:5100` (UDP)
- **WebSocket fallback**: `wss://relay.solun.art/ws/audio`
- **Supported codecs**: Raw S24-in-S32LE (PT=96, 24-bit audio in 32-bit int container), IMA-ADPCM, Opus (RFC 6716), LC3 (planned)
- **Sample rate**: 48kHz
- **Heartbeat**: Every 5 seconds (relay timeout: 60s)

---

## License

```
Copyright 2024-2026 Enabler DAO

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

---

## Contributing

Contributions are welcome. Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/my-feature`)
3. Commit your changes
4. Open a pull request

For questions, open an issue on [GitHub](https://github.com/yukihamada/opensonic/issues).
