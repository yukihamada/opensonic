# Changelog

All notable changes to SolunaSDK will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-03-21

### Added

#### Core Audio
- `AudioPlayer` -- AVAudioEngine-based 48kHz stereo float32 playback with mic monitoring (karaoke mode)
- `ADPCMCodec` -- IMA-ADPCM encoder/decoder matching the C++ implementation
- `OSTPacketParser` -- Parse incoming OSTP/RTP packets (ADPCM, Opus, LC3, raw int32)
- `OSTPacketBuilder` -- Build outgoing OSTP/RTP packets with CRC-32 trailer
- `AsyncAudioStream` -- Swift concurrency `AsyncSequence` wrapper for audio data
- `AudioAnalyzer` -- FFT-based beat detection and frequency band levels (bass/mid/high) at ~30 fps

#### Networking
- `RelayConnection` -- BSD socket UDP connection with HELLO/JOIN handshake and heartbeat
- `ConnectionPool` -- Manage multiple simultaneous relay connections
- `MultiRegionRouter` -- Automatic routing to the closest relay region (nrt/lax/ams)
- `LoadBalancer` -- Client-side load balancing across relay instances
- `ProxyTunnel` -- HTTP/SOCKS proxy tunneling for restricted networks
- `BandwidthController` -- Adaptive bitrate based on network conditions
- `WebhookManager` -- Outbound webhooks for channel events

#### Playback & Transmit
- `MicTransmitter` -- Mic capture and OSTP transmission with peak level metering
- `DJDeckController` -- Dual-deck controller with equal-power crossfade (960 frames/20ms packets)
- `Deck` -- Single audio file playback deck with play/pause/seek
- `NowPlayingManager` -- Lock Screen / Control Center integration with remote commands
- `AudioRecorder` -- Record received audio to file
- `OfflineCache` -- LRU disk cache for offline playback (.caf format, configurable max size)
- `NoiseCanceller` -- Real-time noise cancellation for mic input
- `EchoCanceller` -- Acoustic echo cancellation
- `AutoGainControl` -- Automatic microphone gain adjustment
- `AudioWatermark` -- Inaudible watermark embedding and extraction
- `TextToSpeech` -- System TTS announcements over audio channels
- `VoiceCommands` -- Voice-activated channel control
- `SpeechTranscriber` -- Live speech-to-text via Apple Speech framework

#### Discovery & Sync
- `PeerDiscovery` -- Protocol abstraction for P2P peer discovery
- `MultipeerDiscovery` (iOS) -- MultipeerConnectivity-based peer discovery
- `BonjourDiscovery` (macOS) -- NWBrowser/NWListener-based peer discovery
- `DeviceSyncManager` -- NTP-like wall-clock sync with DELAY/PONG/MAXDELAY messages
- `MultiOutputManager` (macOS) -- CoreAudio multi-device output routing with per-device volume

#### Security & Privacy
- `E2EEncryption` -- AES-GCM 256-bit with ECDH P256 key exchange and monotonic nonce
- `AuthManager` -- Token-based relay authentication
- `GDPRManager` -- Data privacy controls and user consent management
- `AuditLogger` -- Security event audit trail

#### Analytics & Monitoring
- `AnalyticsTracker` -- Per-channel listening stats, session counts, streaks, peak hours (UserDefaults)
- `MetricsExporter` -- Prometheus/OpenMetrics format metrics export
- `HealthCheck` -- SDK health and connectivity diagnostics

#### Platform & Integration
- `RemoteConfig` -- Server-driven feature flags and configuration
- `IntegrationHub` -- Third-party service connectors
- `BillingSDK` -- In-app purchase and subscription management via StoreKit
- `CopyrightManager` -- Content fingerprinting and rights management
- `AdminAPI` -- Server administration and channel management API
- `GeoFence` -- Location-based channel access control
- `AccessibilityHelper` -- VoiceOver and accessibility support
- `Localization` -- Multi-language string management

#### Types & Constants
- `SolunaClient` -- Main entry point (`@MainActor`, `ObservableObject`)
- `SolunaTypes` -- `SolunaConnectionState`, `SolunaChannel`, `SolunaChannels`, `SolunaFanRank`, `OSTConstants`, `OSTPacket`, `ADPCMState`
- `SolunaClientDelegate` -- Protocol for receiving raw audio data and connection state changes
- `DiscoveredPeer` -- P2P peer information
- `AudioOutputDevice` / `AudioOutputTransportType` (macOS) -- CoreAudio device metadata
- `CachedTrack` -- Offline cache track metadata
- `ChannelStat` -- Per-channel analytics data

#### SwiftUI Components
- `SolunaPlayerView` -- Mini-player bar (emoji, name, play/pause, volume)
- `SolunaChannelPicker` -- Adaptive grid of channels with selection highlight
- `SolunaVolumeSlider` -- Custom volume slider with speaker icon
- `SolunaStatusBadge` -- Connection status dot (green/yellow/red)
- `SolunaFanRankBadge` -- Fan rank emoji and label badge

#### Channels
- 7 built-in channels: BJJ, Soluna, Jazz, Chill, Lo-Fi, Dance, Yuki
- Custom channel support via any string identifier

[1.0.0]: https://github.com/yukihamada/opensonic/releases/tag/v1.0.0
