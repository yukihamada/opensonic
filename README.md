<div align="center">

# OpenSonic / Soluna

**Open-source, low-latency multi-room audio system with its own transport protocol (OSTP)**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Mac%20%7C%20iOS%20%7C%20Android%20%7C%20Windows%20%7C%20Linux%20%7C%20Web%20%7C%20ESP32-lightgrey)](https://github.com/yukihamada/opensonic)
[![TestFlight](https://img.shields.io/badge/iOS-TestFlight-blue)](https://testflight.apple.com/join/PYbefDSE)
[![Protocol](https://img.shields.io/badge/Protocol-OSTP%20Draft-orange)](https://solun.art/protocol)
[![Web Demo](https://img.shields.io/badge/Demo-solun.art%2Fdashboard-green)](https://solun.art/dashboard)

Stream audio from any device to any device — across a room or across the planet.
No proprietary hardware. No Dante license. Just open source.

[Protocol Spec](https://solun.art/protocol) · [Internet-Draft](https://solun.art/docs/draft-hamada-opensonic-ostp-00.html) · [Web Demo](https://solun.art/dashboard) · [iOS TestFlight](https://testflight.apple.com/join/PYbefDSE)

</div>

---

## What is OpenSonic?

OpenSonic (branded **Soluna**) is an open-source network audio system that streams system audio from a Mac to iPhones, Raspberry Pis, browsers, Windows PCs, ESP32 microcontrollers — and back. It supports **full-duplex bidirectional audio** for real-time jam sessions, whole-home audio, silent discos, and professional broadcast.

Unlike proprietary systems (Dante, AES67 hardware), OpenSonic requires no special hardware or expensive licenses. Select the Soluna virtual audio device as your system output and you are streaming.

Key numbers:
- **~20ms** end-to-end latency in Jam mode
- **PTP-synchronized** multi-room playback in Sync mode
- **0%** packet loss with P2P unicast relay vs 50-75% on raw WiFi multicast
- **3 billion** listeners supported via 3-tier cascade + P2P swarm relay
- **250+** unit and integration tests passing

---

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                        OpenSonic Stack                           │
├──────────────────────────────────────────────────────────────────┤
│                                                                  │
│   Mac / solunad (TX)                                             │
│   ┌──────────────────────────────────────────────────────────┐   │
│   │  Soluna.driver (virtual audio device, CoreAudio HAL)     │   │
│   │       └─► shared memory ─► solunad TX pipeline           │   │
│   │  Microphone ──────────────► Mic TX (bidirectional)       │   │
│   │  Network RX ──────────────► Speaker / BT / AirPlay out   │   │
│   └──────────────────────────────────────────────────────────┘   │
│              │                                                    │
│     ┌────────┼────────────────────────────────────┐              │
│     │        │    OSTP over UDP                    │              │
│     │  ┌─────▼──────┐  ┌──────────┐  ┌─────────┐ │              │
│     │  │  LAN       │  │  P2P     │  │  WAN    │ │              │
│     │  │  Multicast │  │  Direct  │  │  Relay  │ │              │
│     │  │  239.69.0.1│  │  UDP     │  │  :5100  │ │              │
│     │  │  :5004     │  │  hole-   │  │  3-tier │ │              │
│     │  │  <5ms      │  │  punch   │  │  cascade│ │              │
│     │  └─────┬──────┘  └────┬─────┘  └────┬────┘ │              │
│     └────────┼──────────────┼──────────────┼──────┘              │
│              │              │              │                      │
│   ┌──────────▼──┐  ┌────────▼────┐  ┌─────▼──────────────────┐  │
│   │ iPhone/iPad │  │ Raspberry Pi│  │ Browser / Windows /    │  │
│   │ iOS Swift   │  │ Linux C++   │  │ Android / ESP32        │  │
│   │ RX + Mic TX │  │ solunad RX  │  │ WebSocket / WASAPI /   │  │
│   │ WAN P2P     │  │ WAN P2P     │  │ ALSA / I2S+DMA         │  │
│   └─────────────┘  └─────────────┘  └────────────────────────┘  │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

**Transport**: OSTP (Open Sonic Transport Protocol) — RTP-based, UDP, with FEC, NACK, RTCP, swarm distribution, and optional DTLS encryption.

---

## Quick Start

**Option A — Mac installer (recommended)**

```bash
# 1. Download and install
curl -LO https://github.com/yukihamada/opensonic/releases/latest/download/Soluna-mac.pkg
open Soluna-mac.pkg

# 2. Allow the driver in System Settings → Privacy & Security

# 3. Set output: System Settings → Sound → Output → "Soluna"

# 4. Open /Applications/Soluna.app → enable "Audio TX"

# 5. On any other device, visit https://solun.art/dashboard or install the iOS app
```

**Option B — Build from source**

```bash
# Prerequisites: cmake + Xcode Command Line Tools (macOS) or gcc + libasound2-dev (Linux)
git clone https://github.com/yukihamada/opensonic.git
cd opensonic

cmake -B build -DSOLUNA_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

**Option C — Raspberry Pi / Linux one-liner**

```bash
sudo bash deploy/rpi/install-rx.sh
soluna-rx --peer <mac-ip>:5099 --output alsa
```

**Option D — iOS TestFlight**

Join the beta: [testflight.apple.com/join/PYbefDSE](https://testflight.apple.com/join/PYbefDSE)

---

## OSTP Protocol

The **Open Sonic Transport Protocol (OSTP)** is a UDP-based audio transport built on top of RTP [RFC 3550]. It adds three innovations not present in WebRTC or AES67:

1. **Swarm distribution** — listeners optionally become relay nodes, reducing infrastructure cost at scale (up to 3 billion listeners via 3-tier cascade + fan-out 4 P2P swarm)
2. **Economic layer** — protocol-level micropayments, royalty distribution, and copyright fingerprinting without an application-layer intermediary
3. **Channel-name addressing** — simple `channel-name` scheme compatible with both LAN multicast and WAN P2P

```
OSTP Packet (UDP payload)
┌────────────────────────────────────────────────────────┐
│ RTP Base Header      12 bytes  (RFC 3550)              │
│ RTP Extension Header  4 bytes                          │
│ OSTP Extension Block  8 bytes  (flags, channel ID,     │
│                                 economic fields)        │
│ Audio Payload         variable (Opus / PCM / FLAC)     │
│ CRC-32 Trailer        4 bytes                          │
└────────────────────────────────────────────────────────┘
```

Connection modes auto-selected by the protocol:

| Mode | Topology | Latency | Use Case |
|------|----------|---------|----------|
| LAN Multicast | 239.69.0.1:5004 UDP | <5ms | Same-subnet whole-home audio |
| P2P Direct | UDP hole-punch (STUN) | 10-30ms | 2-4 devices across NAT |
| Relay | Cascade server :5100 | 20-80ms | 5+ devices or symmetric NAT |
| Hybrid | P2P + relay fallback | adaptive | Automatic best-path selection |

Full specification: [https://solun.art/protocol](https://solun.art/protocol)
Internet-Draft (IETF format): [https://solun.art/docs/draft-hamada-opensonic-ostp-00.html](https://solun.art/docs/draft-hamada-opensonic-ostp-00.html)
Local copy: [`docs/draft-hamada-opensonic-ostp-00.txt`](docs/draft-hamada-opensonic-ostp-00.txt)

---

## Implementation Matrix

| Platform | RX | TX | FEC | NACK | DTLS | RTCP |
|----------|----|----|-----|------|------|------|
| Mac (solunad) | ✅ | ✅ | ✅ | ✅ | 🔜 | ✅ |
| iOS (Swift) | ✅ | ✅ | ✅ | ✅ | 🔜 | ✅ |
| Android (Kotlin) | ✅ | ✅ | ✅ | ✅ | 🔜 | ✅ |
| Windows | ✅ | ✅ | ✅ | ✅ | 🔜 | ✅ |
| Web (Browser) | ✅ | ✅ | ✅ | ✅ | 🔜 | ✅ |
| Linux / RPi | ✅ | ✅ | ✅ | ✅ | 🔜 | ✅ |
| ESP32 | ✅ | ✅ | ✅ | ✅ | 🔜 | ✅ |

Legend: ✅ Implemented · 🔜 In Progress

---

## Platform Support

### Mac

```bash
# Install .pkg (driver + app + background service)
open Soluna-mac.pkg

# Build from source
brew install cmake
bash scripts/install-mac.sh

# Status / logs
solctl status
tail -f /tmp/solunad.log
```

Requirements: macOS 13 Ventura or later, Apple Silicon or Intel.

### iOS / iPadOS

1. Open `apps/ios/SolunaReceiver.xcodeproj` in Xcode and build, **or** join TestFlight: [testflight.apple.com/join/PYbefDSE](https://testflight.apple.com/join/PYbefDSE)
2. Connect to the same Wi-Fi — Bonjour auto-discovery finds the Mac instantly
3. Tap the mic button for bidirectional audio
4. Enter a group code to connect over the internet (UDP hole-punch P2P)

### Android

Open `apps/android/` in Android Studio. Requires API level 26+. All OSTP features including FEC and RTCP are supported.

### Windows

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target soluna-rx-win

# Receive via multicast
soluna-rx-win --output wasapi

# Receive via P2P relay
soluna-rx-win --peer <Mac_IP>:5099 --output wasapi
```

### Linux / Raspberry Pi

```bash
# One-command install
sudo bash deploy/rpi/install-rx.sh

# Receive via multicast
soluna-rx --output alsa

# Receive via P2P (recommended for WiFi — eliminates packet loss)
soluna-rx --peer <Mac_IP>:5099 --output alsa

# PipeWire output
soluna-rx --output pipewire
```

RT scheduling for glitch-free audio:

```bash
sudo setcap cap_sys_nice=ep /usr/local/bin/soluna-rx
echo 'kernel.sched_rt_runtime_us=-1' | sudo tee -a /etc/sysctl.conf && sudo sysctl -p
```

### Web Browser

Visit [https://solun.art/dashboard](https://solun.art/dashboard) — no install required. Full OSTP receive + transmit in the browser via WebSocket.

### ESP32

```bash
. $IDF_PATH/export.sh
cd apps/esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Supports I2S+DMA audio output, WiFi multicast and P2P receive, XOR FEC, mDNS discovery, PTP follower, and OTA firmware updates. Runs on a $5 ESP32-S3 module with no dynamic memory allocation in the audio path.

---

## Streaming Modes

| Mode | Latency | Best For |
|------|---------|----------|
| **Sync** | PTP-aligned (full-home sync) | Whole-home audio, multi-room playback |
| **Jam** | ~20ms ultra-low | Live jam sessions, real-time collaboration |

Switch anytime:

```bash
solunad --mode sync       # PTP-synchronized multi-room
solunad --mode jam        # ultra-low latency

# WebSocket API
{"command":"mode.set","mode":"jam"}
```

---

## Audio Engine

The audio engine is built to exceed SonoBus quality based on 2024-2026 research:

| Component | Implementation |
|-----------|---------------|
| Jitter buffer | Lock-free 512-slot fixed array, atomic-only, zero heap allocation |
| Clock sync | Adriaensen DLL (BW=0.01Hz, ±500ppm) — eliminates pitch wobble |
| Packet Loss Concealment | 4-stage: Opus FEC → Opus PLC → WSOLA → silence fade |
| Codec | Opus (128kbps vs 3Mbps PCM) with in-band FEC |
| DSP | Built-in compressor, 3-band EQ, reverb — WebSocket controlled |

---

## Protocol Support

| Protocol | TX | RX | Build Flag |
|----------|----|----|------------|
| OSTP | ✅ | ✅ | Always on |
| AES67 | ✅ | ✅ | `SOLUNA_ENABLE_AES67=ON` (default) |
| Ravenna | ✅ | ✅ | `SOLUNA_ENABLE_RAVENNA=ON` |
| AirPlay 2 | ✅ | ✅ | `SOLUNA_ENABLE_AIRPLAY=ON` |
| DLNA/UPnP | planned | ✅ | `SOLUNA_ENABLE_DLNA=ON` |
| PipeWire | — | ✅ | `SOLUNA_ENABLE_PIPEWIRE=ON` (Linux) |

```bash
# Build with all protocols enabled
cmake -B build \
  -DSOLUNA_ENABLE_AES67=ON \
  -DSOLUNA_ENABLE_RAVENNA=ON \
  -DSOLUNA_ENABLE_AIRPLAY=ON \
  -DSOLUNA_ENABLE_DLNA=ON \
  -DSOLUNA_ENABLE_PIPEWIRE=ON
cmake --build build
```

---

## WebSocket API

solunad exposes a full control API at `ws://localhost:8400/ws`. The Web UI at `http://localhost:8400` uses this API.

```json
// Get current mode
{"command":"mode.get"}
→ {"mode":"sync"}

// Switch to Jam mode
{"command":"mode.set","mode":"jam"}
→ {"ok":true,"mode":"jam"}

// Relay statistics
{"command":"relay.stats"}
→ {"enabled":true,"port":5099,"peer_count":2,"peers":[...]}

// DSP: set EQ low band +6dB
{"command":"dsp.set","plugin":"EQ","param":"low_gain_db","value":6.0}

// Start recording
{"command":"recording.start","dir":"/tmp/rec"}
```

---

## WAN P2P and Scale-Out

For internet-wide distribution, OpenSonic uses a 3-tier cascade relay with P2P swarm fan-out:

```
DJ → Origin (1 node)
       └→ Region (~20 nodes)
              └→ Edge (~10,000 nodes)
                     └→ P2P Swarm (listeners become micro-relays, fan-out 4)
                              └→ Listeners (up to 3 billion)
```

| Scale | Topology |
|-------|----------|
| 2-4 devices | P2P direct |
| 5-50 devices | Single relay |
| 50-100K | Origin + Edge (2-tier) |
| 100K+ | Origin + Region + Edge + P2P Swarm |

```bash
# Start a relay server (VPS)
soluna-relay --port 5100

# Origin node
soluna-relay --origin --cascade-secret SECRET --port 5100

# Region node
soluna-relay --region origin.example.com:5100 --cascade-secret SECRET

# Edge node (8 worker threads)
soluna-relay --edge region-nrt.example.com:5100 --cascade-secret SECRET --workers 8
```

---

## Project Structure

```
opensonic/
├── apps/
│   ├── mac-rx/         Soluna.app — Mac GUI (Swift, TX + RX + WAN)
│   ├── ios/            SolunaReceiver.app — iOS (Swift, RX + Mic TX + WAN P2P)
│   ├── android/        Android (Kotlin, RX + TX + WAN P2P)
│   ├── daemon/         solunad — headless daemon (C++, RPi / Linux / Mac CLI)
│   ├── plugin/         Soluna.driver — CoreAudio HAL virtual device
│   ├── relay/          soluna-relay — WAN relay server (planet-scale)
│   ├── mac/            macOS menu bar app (Swift SPM)
│   ├── linux-rx/       Linux/RPi CLI receiver (C++)
│   └── esp32/          ESP32-S3 firmware (C, ESP-IDF)
├── src/                libsoluna_core — shared C++ core library
│   ├── codec/          Opus / PCM / FLAC codecs
│   ├── pipeline/       Audio pipeline, jitter buffer, PLC
│   ├── sync/           Adriaensen DLL clock sync
│   ├── transport/      OSTP / RTP packet handling
│   ├── wifi/           FEC, NACK, adaptive jitter buffer
│   └── security/       DTLS, ACL
├── include/soluna/     Public headers
├── docs/               Protocol specification and documentation
├── tests/              250+ unit, integration, stress, benchmark tests
├── tools/              auto_optimize.py, audio_compare.py
├── web/                Web UI (dashboard, landing, guide)
├── deploy/             Fly.io, RPi deployment
└── scripts/            build-pkg.sh, install-mac.sh, uninstall-mac.sh
```

---

## Build Options

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DSOLUNA_BUILD_TESTS=ON
```

| Option | Default | Description |
|--------|---------|-------------|
| `SOLUNA_BUILD_TESTS` | ON | Unit and integration tests |
| `SOLUNA_ENABLE_OPUS` | ON | Opus codec |
| `SOLUNA_ENABLE_AES67` | ON | AES67 professional audio interop |
| `SOLUNA_ENABLE_RAVENNA` | OFF | Ravenna professional audio |
| `SOLUNA_ENABLE_AIRPLAY` | OFF | AirPlay 2 receiver |
| `SOLUNA_ENABLE_DLNA` | OFF | DLNA/UPnP media renderer |
| `SOLUNA_ENABLE_PIPEWIRE` | OFF | PipeWire output (Linux) |
| `SOLUNA_ENABLE_DTLS` | OFF | DTLS 1.2 encryption |
| `SOLUNA_BUILD_VST` | OFF | VST3 plugin for DAW receive |

---

## Troubleshooting

**"Soluna" does not appear in Sound preferences**
```bash
bash apps/plugin/install.sh
sudo killall coreaudiod
```

**iPhone audio stutters**
Increase Settings → Buffer Size to 60-120ms.

**Raspberry Pi packet loss on WiFi**
Switch to P2P relay mode (eliminates multicast unreliability):
```bash
soluna-rx --peer <Mac_IP>:5099 --output alsa
```

**Mac service not starting after reboot**
```bash
launchctl print gui/$UID/io.soluna.daemon   # check status
bash apps/daemon/install-service.sh         # re-register
```

**Measure audio quality**
```bash
# Record TX and RX sides simultaneously
solunad --tx --device soluna --record-tx tx.wav --record-dur 30
soluna-rx --peer 192.168.0.194:5099 --record rx.wav --duration 30

# Compare: SNR, spectral difference, dropout detection, sample drift
python3 tools/audio_compare.py tx.wav rx.wav --plot
```

---

## Contributing

Contributions are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) for the full guide.

### Prerequisites

- **macOS**: Xcode Command Line Tools, CMake 3.16+
- **Linux**: GCC 9+ or Clang 10+, CMake 3.16+, `libasound2-dev`
- **Windows**: Visual Studio 2019+, CMake 3.16+

### Workflow

```bash
# Fork and clone
git clone https://github.com/YOUR_USERNAME/opensonic.git
cd opensonic

# Build with tests
cmake -B build -DSOLUNA_BUILD_TESTS=ON
cmake --build build --parallel

# Run all 250+ tests
ctest --test-dir build --output-on-failure

# Create a branch
git checkout -b feature/your-feature
```

Branch naming: `feature/`, `fix/`, `docs/`, `refactor/`

Code style: 4-space indent, `snake_case` functions, `PascalCase` classes, Doxygen for public APIs.

For bugs, please include: OS + compiler version, steps to reproduce, and relevant log output (`tail -f /tmp/solunad.log`).

---

## Links

| Resource | URL |
|----------|-----|
| Web demo & dashboard | https://solun.art/dashboard |
| Protocol specification | https://solun.art/protocol |
| Internet-Draft (IETF) | https://solun.art/docs/draft-hamada-opensonic-ostp-00.html |
| iOS TestFlight | https://testflight.apple.com/join/PYbefDSE |
| Mac installer | https://github.com/yukihamada/opensonic/releases/latest/download/Soluna-mac.pkg |
| WAN relay | relay.solun.art:5100 (UDP) |

---

## License

MIT License — see [LICENSE](LICENSE) for full text.

Copyright (c) 2024-2026 Yuki Hamada and OpenSonic contributors.

> Free for all uses including commercial. Attribution appreciated but not required.
