# SolunaSDK Architecture

## Overview

SolunaSDK is a layered audio streaming SDK built on OSTP (Open Sonic Transport Protocol), an RTP-based protocol designed for real-time audio relay over UDP. The SDK handles connection management, audio decoding/encoding, peer discovery, and SwiftUI integration in a single Swift package with zero external dependencies.

---

## Layer Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                         Application Layer                       │
│                                                                 │
│  SwiftUI Views:                                                 │
│  SolunaPlayerView, SolunaChannelPicker, SolunaVolumeSlider,     │
│  SolunaStatusBadge, SolunaFanRankBadge                          │
├─────────────────────────────────────────────────────────────────┤
│                        SolunaClient (Facade)                    │
│                                                                 │
│  @MainActor, ObservableObject                                   │
│  Orchestrates all sub-components. Single entry point for apps.  │
│  Published state: isConnected, isReceivingAudio, channel,       │
│                   isMicTransmitting, isPeerDiscoveryActive,     │
│                   listenMinutes, fanRank                        │
├──────────┬──────────┬──────────┬──────────┬─────────────────────┤
│ Audio    │ Network  │ Transmit │ Discover │ Services            │
│ Engine   │ Layer    │ Layer    │ Layer    │ Layer               │
│          │          │          │          │                     │
│ Audio    │ Relay    │ Mic      │ Peer     │ Analytics           │
│ Player   │ Conn.    │ Transmit │ Discovery│ Tracker             │
│          │          │          │ (proto)  │                     │
│ ADPCM    │ Conn.    │ DJ Deck  │          │ NowPlaying          │
│ Codec    │ Pool     │ Ctrl     │ Multi    │ Manager             │
│          │          │          │ peer     │                     │
│ Audio    │ Multi    │ Noise    │ (iOS)    │ Offline             │
│ Analyzer │ Region   │ Cancel   │          │ Cache               │
│          │ Router   │          │ Bonjour  │                     │
│ Async    │          │ Echo     │ (macOS)  │ E2E                 │
│ Audio    │ Load     │ Cancel   │          │ Encryption          │
│ Stream   │ Balancer │          │ Device   │                     │
│          │          │ Auto     │ Sync     │ Auth                │
│          │ Proxy    │ Gain     │ Manager  │ Manager             │
│          │ Tunnel   │ Ctrl     │          │                     │
│          │          │          │ Multi    │ Health              │
│          │ BW       │ Audio    │ Output   │ Check               │
│          │ Ctrl     │ Watermark│ (macOS)  │                     │
├──────────┴──────────┴──────────┴──────────┴─────────────────────┤
│                     Protocol Layer                              │
│                                                                 │
│  OSTPacketParser      Parse OSTP/RTP packets from wire format   │
│  OSTPacketBuilder     Build OSTP/RTP packets for transmission   │
│  OSTConstants         Protocol constants and payload types      │
│  OSTPacket            Parsed packet struct                      │
├─────────────────────────────────────────────────────────────────┤
│                     Transport Layer                              │
│                                                                 │
│  BSD Sockets (UDP)    socket() / sendto() / recvfrom()          │
│  WebSocket            wss://relay.solun.art/ws/audio            │
└─────────────────────────────────────────────────────────────────┘
```

---

## Receive Data Flow

The primary use case: receiving audio from the relay and playing it.

```
                   Relay Server
                   relay.solun.art:5100 (UDP)
                        │
                        │ UDP packet (RTP framed)
                        ▼
              ┌──────────────────┐
              │ RelayConnection  │
              │                  │
              │ recvfrom() loop  │
              │ on background    │
              │ Thread (.user    │
              │ Interactive QoS) │
              └────────┬─────────┘
                       │ onPacket callback
                       ▼
              ┌──────────────────┐
              │ SolunaClient     │
              │ .handlePacket()  │
              └────────┬─────────┘
                       │
                       ▼
              ┌──────────────────┐
              │ OSTPacketParser  │
              │ .parse(data)     │
              │                  │
              │ Validates:       │
              │ - RTP version    │
              │ - OSTP profile   │
              │ - CRC-32 trailer │
              │                  │
              │ Extracts:        │
              │ - payloadType    │
              │ - channels       │
              │ - deckId         │
              │ - seqNumber      │
              │ - timestamp      │
              │ - payload data   │
              └────────┬─────────┘
                       │ OSTPacket
                       ▼
              ┌──────────────────┐
              │ Codec Dispatch   │
              │                  │
              │ PT 115/116       │
              │  → ADPCMCodec    │
              │    .decode()     │
              │                  │
              │ PT 98            │
              │  → Opus via      │
              │    AVAudio       │
              │    Converter     │
              │                  │
              │ PT 119           │
              │  → LC3 (planned) │
              │                  │
              │ Default          │
              │  → Raw int32 LE  │
              │    to float32    │
              └────────┬─────────┘
                       │ AVAudioPCMBuffer
                       │ (48kHz stereo float32)
                       ▼
              ┌──────────────────┐
              │ AudioPlayer      │
              │ .scheduleBuffer()│
              │                  │
              │ AVAudioEngine    │
              │  └─ playerNode   │
              │     └─ mainMixer │
              │        └─ output │
              └────────┬─────────┘
                       │
                       ▼
                 CoreAudio HAL
                 → Speakers / AirPods / etc.
```

---

## Transmit Data Flow

Sending microphone audio or DJ deck output to the relay.

### Mic Transmit

```
                 Microphone
                     │
                     ▼
              ┌──────────────────┐
              │ AVAudioEngine    │
              │ .inputNode       │
              │ installTap()     │
              │                  │
              │ Captures at      │
              │ device sample    │
              │ rate, mono       │
              │ float32          │
              └────────┬─────────┘
                       │ AVAudioPCMBuffer
                       ▼
              ┌──────────────────┐
              │ MicTransmitter   │
              │                  │
              │ Float → S24 int32│
              │ Accumulate into  │
              │ 240-frame packets│
              │ (5ms at 48kHz)   │
              │                  │
              │ Peak level meter │
              └────────┬─────────┘
                       │ [Int32] payload
                       ▼
              ┌──────────────────┐
              │ OSTPacketBuilder │
              │ .buildPacket()   │
              │                  │
              │ RTP header (12B) │
              │ OSTP extension   │
              │ Payload          │
              │ CRC-32 (4B)     │
              └────────┬─────────┘
                       │ Data
                       ▼
              ┌──────────────────┐
              │ RelayConnection  │
              │ .sendRawData()   │
              │                  │
              │ sendto() via     │
              │ BSD UDP socket   │
              └────────┬─────────┘
                       │
                       ▼
                 Relay Server
                 → All peers on channel
```

### DJ Deck Transmit

```
     Audio File A          Audio File B
          │                     │
          ▼                     ▼
     ┌─────────┐          ┌─────────┐
     │ Deck A  │          │ Deck B  │
     │ AVAudio │          │ AVAudio │
     │ File    │          │ File    │
     │ .read() │          │ .read() │
     └────┬────┘          └────┬────┘
          │ float32 frames     │ float32 frames
          └────────┬───────────┘
                   │
                   ▼
          ┌──────────────────┐
          │ DJDeckController │
          │ .mixLoop()       │
          │                  │
          │ Equal-power      │
          │ crossfade:       │
          │ A = cos(cf*pi/2) │
          │ B = sin(cf*pi/2) │
          │                  │
          │ 960 frames/packet│
          │ (20ms at 48kHz)  │
          │                  │
          │ Mix → int16 LE   │
          └────────┬─────────┘
                   │
                   ▼
          OSTPacketBuilder → RelayConnection → Relay
```

---

## Security Flow

End-to-end encryption using ECDH key exchange and AES-GCM.

```
     Client A                              Client B
        │                                     │
        │  generateKeyPair()                  │  generateKeyPair()
        │  P256.KeyAgreement.PrivateKey       │  P256.KeyAgreement.PrivateKey
        │                                     │
        │───── KEY_OFFER:<pubKeyA_base64> ────→│
        │                                     │
        │←──── KEY_ACCEPT:<pubKeyB_base64> ───│
        │                                     │
        │  deriveSessionKey(pubKeyB)           │  deriveSessionKey(pubKeyA)
        │  ECDH → SharedSecret                │  ECDH → SharedSecret
        │  HKDF-SHA256 → 256-bit AES key      │  HKDF-SHA256 → 256-bit AES key
        │  (same key on both sides)            │  (same key on both sides)
        │                                     │
        │  encrypt(audio):                    │  decrypt(encrypted):
        │  AES-GCM-256                        │  AES-GCM-256
        │  Nonce: monotonic counter (12B)     │  Nonce from packet prefix (12B)
        │  Output: nonce || ciphertext || tag │  Input: nonce || ciphertext || tag
        │                                     │
        │────── encrypted audio packets ──────→│
        │←───── encrypted audio packets ───────│
```

Key properties:
- **Forward secrecy**: Ephemeral key pairs; new keys per session
- **Replay protection**: Monotonically incrementing nonce counter
- **Salt**: `"SolunaSDK-E2E-v1"` for domain separation
- **Key exchange**: Via relay control channel text messages

---

## Scale Architecture

For production deployments with multiple relay regions.

```
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│ Relay (nrt)  │     │ Relay (lax)  │     │ Relay (ams)  │
│ Tokyo        │     │ Los Angeles  │     │ Amsterdam    │
│ :5100 UDP    │     │ :5100 UDP    │     │ :5100 UDP    │
└──────┬───────┘     └──────┬───────┘     └──────┬───────┘
       │                    │                    │
       └────────────────────┼────────────────────┘
                            │
                    ┌───────┴───────┐
                    │ MultiRegion   │
                    │ Router        │
                    │               │
                    │ Latency probe │
                    │ per region    │
                    │ → pick lowest │
                    └───────┬───────┘
                            │
                    ┌───────┴───────┐
                    │ LoadBalancer  │
                    │               │
                    │ Round-robin / │
                    │ least-conn   │
                    │ across region │
                    │ instances     │
                    └───────┬───────┘
                            │
                    ┌───────┴───────┐
                    │ ConnectionPool│
                    │               │
                    │ Maintains N   │
                    │ active relay  │
                    │ connections   │
                    │ with auto     │
                    │ reconnect     │
                    └───────┬───────┘
                            │
                    ┌───────┴───────┐
                    │ Relay         │
                    │ Connection    │
                    │               │
                    │ BSD UDP socket│
                    │ HELLO/JOIN    │
                    │ heartbeat     │
                    └───────────────┘
```

---

## Connection Lifecycle

```
                    ┌─────────┐
                    │  IDLE   │
                    └────┬────┘
                         │ connect(channel:)
                         ▼
                    ┌─────────┐
                    │RESOLVING│  getaddrinfo()
                    └────┬────┘
                         │ success
                         ▼
                    ┌─────────┐
                    │ SOCKET  │  socket(AF_INET, SOCK_DGRAM)
                    │ CREATED │
                    └────┬────┘
                         │
                         ▼
                    ┌─────────┐
                    │ HELLO   │  Send HELLO x3 (100ms apart)
                    │ SENT    │
                    └────┬────┘
                         │
                         ▼
                    ┌─────────┐
                    │  JOIN   │  Send JOIN:<channel>::<deviceName>
                    │  SENT   │
                    └────┬────┘
                         │ recv thread started
                         │ heartbeat timer started (5s interval)
                         ▼
                    ┌─────────┐
         ┌─────────│CONNECTED│←──────────────┐
         │         └────┬────┘               │
         │              │                    │
         │   ┌──────────┴──────────┐         │
         │   │ Heartbeat every 5s  │         │
         │   │ HELLO + JOIN        │─────────┘
         │   └─────────────────────┘
         │
         │ disconnect()
         ▼
    ┌──────────┐
    │DISCONNECT│  close(socket), stop timers/threads
    └──────────┘
```

---

## Thread Model

```
Main Thread (@MainActor)
├── SolunaClient (all published state updates)
├── SwiftUI views (SolunaPlayerView, etc.)
├── Heartbeat Timer (sends HELLO/JOIN)
└── Listen Timer (updates listenMinutes, fanRank)

Background Thread (.userInteractive)
├── RelayConnection recv loop (recvfrom)
└── DJDeckController mix loop (read + mix + send)

Utility Queue
├── OfflineCache (disk I/O)
└── AnalyticsTracker (persistence)

User Interactive Queue
└── AudioAnalyzer (FFT computation)

Audio Thread (managed by CoreAudio)
├── AVAudioEngine render callback
├── AudioPlayer scheduling
└── MicTransmitter input tap
```

---

## Module Dependency Graph

```
SolunaClient
├── AudioPlayer
│   └── ADPCMCodec
├── RelayConnection
│   └── AtomicFlag
├── OSTPacketParser
├── MicTransmitter
│   └── OSTPacketBuilder
├── DJDeckController
│   ├── Deck (x2)
│   └── OSTPacketBuilder
├── MultipeerDiscovery (iOS) / BonjourDiscovery (macOS)
│   └── PeerDiscovery (protocol)
├── MultiOutputManager (macOS only)
├── DeviceSyncManager
├── E2EEncryption
├── OfflineCache
│   └── CacheWriter (internal)
├── AnalyticsTracker
├── AudioAnalyzer
├── NowPlayingManager
└── SolunaTypes
    ├── SolunaChannel / SolunaChannels
    ├── SolunaConnectionState
    ├── SolunaFanRank
    ├── OSTConstants
    ├── OSTPacket
    └── ADPCMState
```
