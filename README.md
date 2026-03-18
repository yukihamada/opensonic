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

## Downloads

| Platform | Download | Requirements |
|----------|----------|--------------|
| **macOS** | [**Soluna-mac.pkg**](https://github.com/yukihamada/opensonic/releases/latest/download/Soluna-mac.pkg) · [DMG](https://github.com/yukihamada/opensonic/releases/latest/download/Soluna-mac.dmg) | macOS 13+, Apple Silicon or Intel |
| **iOS / iPadOS** | [**Join TestFlight**](https://testflight.apple.com/join/PYbefDSE) · [App Store](https://apps.apple.com/app/id6759962263) | iOS 16+ |
| **Linux / RPi** | `curl -fsSL https://solun.art/install-rx.sh \| sudo bash` | Debian/Ubuntu/Raspbian |
| **Web (Browser)** | [**solun.art/dashboard**](https://solun.art/dashboard) — no install | Chrome 94+ / Firefox 115+ / Safari 16.4+ |
| **Android** | [Build from source](#android) — Android Studio | API 26+ (Android 8.0+) |
| **Windows** | [Build from source](#windows) — Visual Studio | VS 2019+, CMake 3.16+ |
| **ESP32** | [Build with ESP-IDF](#esp32) | ESP-IDF 5.x, ESP32-S3 |

---

## What is OpenSonic?

OpenSonic (branded **Soluna**) is an open-source network audio system that streams system audio from a Mac to iPhones, Raspberry Pis, browsers, Windows PCs, ESP32 microcontrollers — and back. It supports **full-duplex bidirectional audio** for real-time jam sessions, whole-home audio, silent discos, and professional broadcast.

Unlike proprietary systems (Dante, AES67 hardware), OpenSonic requires no special hardware or expensive licenses. Select the Soluna virtual audio device as your system output and you are streaming.

Key numbers:
- **~20ms** end-to-end latency in Jam mode
- **PTP-synchronized** multi-room playback in Sync mode
- **~0%** packet loss with Dynamic XOR FEC + NACK on lossy WiFi
- **3 billion** listeners supported via 3-tier cascade + Gossip P2P swarm relay
- **DJ dual-deck** with equal-power crossfader — broadcast directly from the app

---

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                        OpenSonic Stack                           │
├──────────────────────────────────────────────────────────────────┤
│   Mac / solunad (TX)                                             │
│   ┌──────────────────────────────────────────────────────────┐   │
│   │  Soluna.driver (virtual audio device, CoreAudio HAL)     │   │
│   │       └─► shared memory ─► solunad TX pipeline           │   │
│   │  Microphone ──────────────► Mic TX (bidirectional)       │   │
│   └──────────────────────────────────────────────────────────┘   │
│              │    OSTP over UDP                                   │
│     ┌────────┼────────────────────────────────────┐              │
│     │  ┌─────▼──────┐  ┌──────────┐  ┌─────────┐ │              │
│     │  │  LAN       │  │  P2P     │  │  WAN    │ │              │
│     │  │  Multicast │  │  Direct  │  │  Relay  │ │              │
│     │  │  :5004     │  │  UDP     │  │  :5100  │ │              │
│     │  │  <5ms      │  │  hole-   │  │  3-tier │ │              │
│     │  └─────┬──────┘  └────┬─────┘  └────┬────┘ │              │
│     └────────┼──────────────┼──────────────┼──────┘              │
│              │              │              │                      │
│   ┌──────────▼──┐  ┌────────▼────┐  ┌─────▼──────────────────┐  │
│   │ iPhone/iPad │  │ Raspberry Pi│  │ Browser / Windows /    │  │
│   │ iOS Swift   │  │ Linux C++   │  │ Android / ESP32        │  │
│   │ RX + Mic TX │  │ solunad RX  │  │ WebSocket / WASAPI /   │  │
│   └─────────────┘  └─────────────┘  └────────────────────────┘  │
└──────────────────────────────────────────────────────────────────┘
```

---

## Quick Start

### macOS

**1. Install** — [Download Soluna-mac.pkg](https://github.com/yukihamada/opensonic/releases/latest/download/Soluna-mac.pkg)

```bash
# Or via terminal:
curl -LO https://github.com/yukihamada/opensonic/releases/latest/download/Soluna-mac.pkg
open Soluna-mac.pkg
```

**2. Allow driver** — System Settings → Privacy & Security → click "Allow" next to Soluna

**3. Set output** — System Settings → Sound → Output → **Soluna**

**4. Start streaming** — Open `/Applications/Soluna.app` → enable **Audio TX**

**5. Listen** — On any other device, visit [solun.art/dashboard](https://solun.art/dashboard) or install the iOS app

Status: `solctl status` · Logs: `tail -f /tmp/solunad.log`

---

### iOS / iPadOS

> **[Join TestFlight →](https://testflight.apple.com/join/PYbefDSE)** · [App Store](https://apps.apple.com/app/id6759962263)

1. Open the TestFlight link on your iPhone/iPad
2. Install **Soluna Rx**
3. Connect to the same Wi-Fi — auto-discovers the Mac via Bonjour
4. Tap the mic button for bidirectional audio
5. Enter a relay channel name to connect over the internet

---

### Linux / Raspberry Pi {#linux}

```bash
# One-line install: clones repo, builds with CMake, installs as systemd service
curl -fsSL https://solun.art/install-rx.sh | sudo bash
```

After install:

```bash
systemctl status soluna           # check service
journalctl -u soluna -f           # live logs

# Connect to a relay channel
soluna --channel my-channel --relay relay.solun.art:5100 --output alsa

# LAN multicast (same subnet as Mac)
soluna --output alsa

# PipeWire output
soluna --output pipewire
```

RT scheduling for glitch-free audio on RPi:
```bash
sudo setcap cap_sys_nice=ep /usr/local/bin/soluna
```

---

### Web Browser

Visit **[solun.art/dashboard](https://solun.art/dashboard)** — no install required.

- Enter a channel name to receive any active OSTP stream
- Click the mic icon to transmit (requires mic permission)
- Works in Chrome 94+, Firefox 115+, Safari 16.4+

---

### Android {#android}

Pre-built APK is not yet available. Build from source:

```bash
git clone https://github.com/yukihamada/opensonic.git
# In Android Studio: File → Open → apps/android/
# Build → Make Project
# Run on device (API 26+, Android 8.0+)
```

Features: RTP RX/TX, XOR FEC decode + recovery, NACK sender, RTCP receiver reports, Opus codec.

---

### Windows {#windows}

Pre-built installer is not yet available. Build from source:

```bash
git clone https://github.com/yukihamada/opensonic.git
cd opensonic
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target soluna-rx-win --target soluna-tx-win

# Receive
.\build\soluna-rx-win.exe --channel my-channel --output wasapi

# Transmit (system loopback)
.\build\soluna-tx-win.exe --channel my-channel --input wasapi-loopback
```

Requirements: Visual Studio 2019+, CMake 3.16+.

---

### ESP32 {#esp32}

```bash
. $IDF_PATH/export.sh
git clone https://github.com/yukihamada/opensonic.git
cd opensonic/apps/esp32

# Configure WiFi + relay
idf.py menuconfig   # → Soluna → WiFi SSID / Password / Relay host / Channel

idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Features: I2S+DMA audio I/O (ESP32-S3), XOR FEC TX+RX, NACK cache (16-slot), RTCP 5s reports, mDNS discovery, PTP clock follower, OTA firmware update. Glitch-free audio with no dynamic memory allocation in the audio path.

---

## OSTP Protocol

The **Open Sonic Transport Protocol (OSTP)** is a UDP-based audio transport built on RTP [RFC 3550].

```
OSTP Packet (UDP payload)
┌────────────────────────────────────────────────────────┐
│ RTP Base Header       12 bytes  (RFC 3550)             │
│ RTP Extension Header   4 bytes  (profile = 0x4F53)     │
│ OSTP Extension Block   8 bytes  (stream_id, seq_ext,   │
│                                  media_timestamp)       │
│ Audio Payload        variable   (Opus / PCM-16 / FLAC) │
│ CRC-32 Trailer         4 bytes                         │
└────────────────────────────────────────────────────────┘
  Total fixed overhead: 28 bytes
```

Connection modes, auto-selected:

| Mode | Multicast Group / Port | Latency | Use Case |
|------|------------------------|---------|----------|
| LAN Multicast | 239.69.0.1:5004 | <5ms | Same-subnet |
| P2P Direct | UDP hole-punch (STUN) | 10–30ms | 2–4 devices |
| WAN Relay | relay.solun.art:5100 | 20–80ms | Any network |
| Hybrid | P2P + relay fallback | adaptive | Automatic |

Full spec: **[solun.art/protocol](https://solun.art/protocol)** (OSTP v0.9.3)
Internet-Draft: **[draft-hamada-opensonic-ostp-00](https://solun.art/docs/draft-hamada-opensonic-ostp-00.html)**

### What's new in v0.9.3

| Fix | Description |
|-----|-------------|
| `media_timestamp` ms | Was nanoseconds (4.3s rollover bug) → now milliseconds (49-day window) |
| Dynamic FEC | Group size N auto-adjusts: 0%→off, 0-2%→N=10, 2-5%→N=5, 5-10%→N=3, >10%→N=2 |
| RTCP jitter buffer | RFC 3550 §6.4.1 inter-arrival jitter drives `target_fill_frames` |
| TURN fallback | `TURN_ALLOC` / `TURN_OK` for Symmetric NAT traversal |
| Gossip + dual-parent | 8-candidate peer table; `PARENT_FAIL` for <80ms churn recovery |
| RTCP APP SWCH | PT=204 synchronized file switch across all relay members |
| TIP verification | Solana RPC `getTransaction` validates on-chain tx before credit |
| Control/data HOL | File chunks → port 8401; control JSON → port 8400 (no HOL blocking) |

---

## Implementation Matrix

| Platform | RX | TX | FEC | NACK | DTLS | RTCP | TURN | DJ Deck |
|----------|----|----|-----|------|------|------|------|---------|
| Mac (solunad) | ✅ | ✅ | ✅ | ✅ | 🔜 | ✅ | ✅ | ✅ |
| iOS (Swift) | ✅ | ✅ | ✅ | ✅ | 🔜 | ✅ | ✅ | ✅ |
| Android (Kotlin) | ✅ | ✅ | ✅ | ✅ | 🔜 | ✅ | 🔜 | 🔜 |
| Windows | ✅ | ✅ | ✅ | ✅ | 🔜 | ✅ | 🔜 | 🔜 |
| Web (Browser) | ✅ | ✅ | ✅ | ✅ | 🔜 | ✅ | 🔜 | 🔜 |
| Linux / RPi | ✅ | ✅ | ✅ | ✅ | 🔜 | ✅ | 🔜 | 🔜 |
| ESP32 | ✅ | ✅ | ✅ | ✅ | 🔜 | ✅ | — | — |

✅ Implemented · 🔜 Planned · — Not applicable

**FEC**: Dynamic group size (N=0–10) auto-adapts to RTCP-reported loss rate (OSTP v0.9.3 §3.7)
**RTCP**: Drives both bitrate adaptation and jitter buffer target (RFC 3550 §6.4.1)
**TURN**: Relay fallback when STUN hole-punch fails (OSTP v0.9.3 §5.x)

---

## Audio Engine

| Component | Implementation |
|-----------|---------------|
| Jitter buffer | Adaptive, RTCP inter-arrival jitter driven (RFC 3550 §6.4.1) |
| Clock sync | OSTP `media_timestamp` (ms, 49-day rollover) + Adriaensen DLL |
| Packet Loss Concealment | 4-stage: Opus FEC → Opus PLC → WSOLA → silence fade |
| Codec | Opus 32–320 kbps (adaptive), PCM-16, PCM-24, FLAC |
| Congestion control | RFC 8085 bitrate ladder + Dynamic FEC (N=0/10/5/3/2 by loss%) |
| DJ dual-deck | Equal-power crossfade, ExtAudioFile decode, OSTP broadcast |

---

## WAN Relay

Hosted relay at `relay.solun.art:5100` is free to use for testing.

```bash
# Self-host (VPS)
soluna-relay --port 5100

# 3-tier scale-out
soluna-relay --origin  --port 5100
soluna-relay --region  --upstream origin.example.com:5100
soluna-relay --edge    --upstream region-nrt.example.com:5100 --workers 8
```

Relay features (v0.9.3):
- **TURN fallback** — `TURN_ALLOC` / `TURN_OK` when STUN hole-punch fails
- **Gossip peer discovery** — `GOSSIP_PEERS` returns 8 candidates; `PARENT_FAIL` triggers <80ms re-parent
- **RTCP APP SWCH** (PT=204) — synchronized file switch forwarded to all group members
- **TIP on-chain verification** — Solana RPC `getTransaction` validates `tx_signature` before crediting
- **Control/data plane** — audio WebSocket (`/ws/audio`) separate from control UDP

---

## Build from Source

```bash
git clone https://github.com/yukihamada/opensonic.git
cd opensonic
cmake -B build -DSOLUNA_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

| CMake Option | Default | Description |
|--------------|---------|-------------|
| `SOLUNA_BUILD_TESTS` | ON | Unit and integration tests |
| `SOLUNA_ENABLE_AES67` | ON | AES67 professional audio interop |
| `SOLUNA_ENABLE_RAVENNA` | OFF | Ravenna protocol |
| `SOLUNA_ENABLE_AIRPLAY` | OFF | AirPlay 2 receiver |
| `SOLUNA_ENABLE_DTLS` | OFF | DTLS 1.2 encryption (OpenSSL) |
| `SOLUNA_BUILD_VST` | OFF | VST3 plugin for DAW |

---

## Troubleshooting

**"Soluna" missing from Sound preferences (macOS)**
```bash
bash apps/plugin/install.sh && sudo killall coreaudiod
```

**iPhone audio stutters**
→ Settings → Buffer Size → 60–120 ms, or switch to WAN relay mode.

**Raspberry Pi packet loss on WiFi**
```bash
# Multicast is unreliable on WiFi. Use relay:
soluna --channel my-channel --relay relay.solun.art:5100
```

**Mac service not starting after reboot**
```bash
launchctl print gui/$UID/io.soluna.daemon
bash apps/daemon/install-service.sh
```

---

## Contributing

Contributions welcome — see [CONTRIBUTING.md](CONTRIBUTING.md).

```bash
git clone https://github.com/yukihamada/opensonic.git
cd opensonic
cmake -B build -DSOLUNA_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
git checkout -b feature/your-feature
```

Branch naming: `feature/`, `fix/`, `docs/`, `refactor/`

---

## All Links

| Resource | URL |
|----------|-----|
| **Mac installer (.pkg)** | https://github.com/yukihamada/opensonic/releases/latest/download/Soluna-mac.pkg |
| **Mac installer (.dmg)** | https://github.com/yukihamada/opensonic/releases/latest/download/Soluna-mac.dmg |
| **iOS TestFlight** | https://testflight.apple.com/join/PYbefDSE |
| **iOS App Store** | https://apps.apple.com/app/id6759962263 |
| **Linux install script** | `curl -fsSL https://solun.art/install-rx.sh \| sudo bash` |
| **Web demo & dashboard** | https://solun.art/dashboard |
| **Protocol specification** | https://solun.art/protocol |
| **Internet-Draft (IETF)** | https://solun.art/docs/draft-hamada-opensonic-ostp-00.html |
| **WAN relay endpoint** | relay.solun.art:5100 (UDP) |
| **All releases** | https://github.com/yukihamada/opensonic/releases |

---

## License

MIT License — see [LICENSE](LICENSE).

Copyright (c) 2024–2026 Yuki Hamada and OpenSonic contributors.
