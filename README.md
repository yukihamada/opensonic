# Soluna — Open Network Audio System

<p align="center">
  <img src="docs/logo.svg" alt="Soluna Logo" width="200">
</p>

[![Build Status](https://github.com/yukihamada/opensonic/actions/workflows/ci.yml/badge.svg)](https://github.com/yukihamada/opensonic/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![iOS App](https://img.shields.io/badge/iOS-App_Store-blue.svg)](https://soluna.audio)

**Soluna** is an open-source, professional-grade network audio system for low-latency, synchronized audio streaming over IP networks.

**Soluna**（ソルーナ）は、オープンソースのプロフェッショナル・ネットワークオーディオシステムです。低遅延・高精度同期でIPネットワーク上のオーディオストリーミングを実現します。

---

## Features / 機能

- **Ultra-low latency**: Sub-millisecond latency on wired networks / 有線ネットワークでサブミリ秒のレイテンシ
- **PTP Synchronization**: IEEE 1588-2008 (PTPv2) for sample-accurate sync / サンプル精度の同期
- **Multi-platform**: Linux, macOS, Windows, ESP32, Raspberry Pi / マルチプラットフォーム対応
- **WiFi Support**: Adaptive streaming with FEC for wireless networks / WiFi対応（FEC付き適応ストリーミング）
- **AES67 Compatible**: Interoperates with Dante, Ravenna, etc. / AES67互換（Dante、Ravenna等と相互運用）
- **Web Control**: REST API + WebSocket + responsive Web UI / WebベースのコントロールUI
- **Prometheus Metrics**: Built-in operational monitoring / 運用監視用メトリクス内蔵

---

## Download / ダウンロード

### Current Status / 現在の状況

**macOS (TX)**: ✅ Working — `solunad --tx` with BlackHole 2ch captures system audio and streams over UDP multicast

**Raspberry Pi (RX)**: ✅ Working — `solunad --rx` with ALSA/ES9038Q2M DAC confirmed on RPi 4

**iOS**: 🔨 Xcode project ready, build from source

**Other platforms**: Coming soon

### Build from Source / ソースからビルド

```bash
git clone https://github.com/yukihamada/opensonic.git
cd opensonic
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
# Outputs: solunad (daemon), solctl (CLI)
```

### iOS App Build / iOSアプリビルド

```bash
# Build iOS library
cd opensonic
mkdir build-ios && cd build-ios
cmake .. -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
xcodebuild -project soluna.xcodeproj -scheme soluna_core -configuration Release -sdk iphoneos

# Open Xcode project
open apps/ios/SolunaReceiver.xcodeproj
```

---

## Quick Start / クイックスタート

### Installation / インストール

**Linux (Debian/Ubuntu):**
```bash
curl -sSL https://soluna.dev/install.sh | sudo bash
```

**macOS (Homebrew):**
```bash
brew tap soluna/tap
brew install soluna
```

**Build from source / ソースからビルド:**
```bash
git clone https://github.com/yukihamada/opensonic.git
cd soluna
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
sudo make install
```

### Basic Usage / 基本的な使い方

**Transmit system audio from Mac (via BlackHole) / Macのシステム音声を送信:**
```bash
# Set BlackHole 2ch as macOS default output device first
solunad --tx --device "BlackHole 2ch" --channels 2
# → streams to 239.69.0.1:5004 (UDP multicast)
```

**Receive audio on Raspberry Pi / Raspberry Piで受信:**
```bash
solunad --rx --device default --channels 2
# or specify ALSA device explicitly:
solunad --rx --device hw:1 --channels 2
```

**Loopback test on same machine / 同一マシンでループバックテスト:**
```bash
solunad --tx --device "BlackHole 2ch" --channels 2 &
solunad --rx --device "MacBook Air Speakers" --channels 2
```

**Use config file / 設定ファイルを使用:**
```bash
solunad --config /etc/soluna/config.yaml
```

---

## Raspberry Pi Setup / Raspberry Piセットアップ

### Build on RPi / RPiでビルド

```bash
sudo apt-get install -y libasound2-dev cmake g++
git clone https://github.com/yukihamada/opensonic.git
cd opensonic && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4
sudo cp solunad /usr/bin/solunad
```

### Real-time Audio Optimization / リアルタイム最適化

For dropout-free audio, apply these one-time settings:

```bash
# Allow solunad to set real-time scheduling without root
sudo setcap cap_sys_nice=ep /usr/bin/solunad

# Disable RT throttling (prevents SCHED_FIFO from being rate-limited)
echo 'kernel.sched_rt_runtime_us=-1' | sudo tee -a /etc/sysctl.conf
sudo sysctl -p

# Set CPU governor to performance
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

With these applied, `solunad` automatically:
- Runs the audio thread at `SCHED_FIFO` priority 80
- Pins the audio thread to CPU core 3 (dedicated, away from IRQ/OS tasks)

### Systemd Service / systemdサービス

```bash
sudo cp deploy/rpi/soluna.service /etc/systemd/system/
sudo systemctl enable soluna
sudo systemctl start soluna
```

---

## Configuration / 設定

Example `/etc/soluna/config.yaml`:

```yaml
device:
  name: "studio-main"
  audio: "hw:0"

network:
  control_port: 8400
  rtp_base: 5004

audio:
  sample_rate: 48000
  channels: 2
  bit_depth: 24

security:
  dtls: false
  auth_enabled: false

metrics:
  enabled: true
  port: 9100
```

See [Configuration Guide](docs/configuration.md) for full options.

---

## Platform Support / プラットフォーム対応

| Platform | Audio Backend | Status |
|----------|---------------|--------|
| macOS | CoreAudio | ✅ Working (TX + RX, BlackHole system capture) |
| Raspberry Pi | ALSA | ✅ Working (RX, ES9038Q2M DAC confirmed) |
| Linux (x64/ARM) | ALSA | ✅ Working |
| Windows | WASAPI | 🔨 In Development |
| ESP32 | I2S | 🔨 In Development |
| iOS | CoreAudio | 🔨 In Development (UI complete) |
| Android | AAudio/OpenSL | 📋 Planned |

---

## Comparison / 他製品との比較

| Feature | Soluna | Dante | AES67 | AVB | PipeWire |
|---------|--------|-------|-------|-----|----------|
| **License** | MIT (OSS) | Proprietary | Standard | Standard | LGPL |
| **Cost** | Free | $$$$ | Varies | Free | Free |
| **Latency (wired)** | ~15ms* | < 1ms | < 1ms | < 2ms | 5-20ms |
| **Latency (WiFi)** | ~52ms* | N/A | N/A | N/A | N/A |
| **Sync** | PTPv2 (planned) | PTPv2 | PTPv2 | gPTP | None |
| **WiFi** | ✅ | Limited | ❌ | ❌ | ✅ |
| **Embedded** | ESP32, RPi | Limited | ❌ | ❌ | ❌ |
| **Max Channels** | 64 | 512 | Unlimited | 8 | 256 |
| **Discovery** | mDNS | Dante Discovery | SAP | AVDECC | PipeWire |
| **Encryption** | DTLS | AES-256 | None | MACsec | None |
| **Web UI** | ✅ | ✅ | ❌ | ❌ | ✅ |
| **REST API** | ✅ | ❌ | ❌ | ❌ | D-Bus |

*Measured: 5ms packet + 20ms jitter buffer prefill. Wired estimate; WiFi 2.4GHz measured ~52ms.

### When to use Soluna / Solunaを選ぶ場面

- **Home/DIY projects**: Free, open-source, easy to deploy / 自作プロジェクト
- **Small studios**: Professional quality without licensing costs / 小規模スタジオ
- **Embedded audio**: ESP32 and Raspberry Pi support / 組み込みオーディオ
- **Custom integrations**: Full API access, modify source code / カスタム統合
- **Learning**: Understand network audio internals / ネットワークオーディオの学習

### When to use Dante/AES67 / Dante/AES67を選ぶ場面

- **Large installations**: 100+ channels, enterprise support / 大規模設置
- **Broadcast**: Certified equipment required / 放送業界
- **Interoperability**: Need to connect to existing Dante network / 既存機器との接続

---

## Hardware Recommendations / おすすめハードウェア

### ESP32 Audio Receiver / ESP32オーディオレシーバー

| Grade | Components | Price | Links |
|-------|------------|-------|-------|
| **梅 (Budget)** | ESP32-DevKitC + PCM5102A DAC | ~$15 | [ESP32](https://www.aliexpress.com/item/1005001267643044.html), [DAC](https://www.aliexpress.com/item/32836612292.html) |
| **竹 (Standard)** | ESP32-S3-DevKitC + PCM5102A + LM386 Amp | ~$25 | [ESP32-S3](https://www.aliexpress.com/item/1005004452396855.html), [Amp](https://www.aliexpress.com/item/32833637279.html) |
| **松 (Premium)** | ESP32-S3 + ES9038Q2M DAC + TPA3116 Amp | ~$60 | [DAC](https://www.aliexpress.com/item/1005003493783197.html), [Amp](https://www.aliexpress.com/item/1005003108391596.html) |

### Raspberry Pi Setup / Raspberry Piセットアップ

| Grade | Components | Price | Links |
|-------|------------|-------|-------|
| **梅 (Budget)** | RPi Zero 2W + USB Audio | ~$25 | [Pi Zero 2W](https://www.raspberrypi.com/products/raspberry-pi-zero-2-w/) |
| **竹 (Standard)** | RPi 4 2GB + HiFiBerry DAC+ | ~$80 | [HiFiBerry](https://www.hifiberry.com/shop/boards/hifiberry-dac-plus/) |
| **松 (Premium)** | RPi 4 4GB + HiFiBerry DAC2 Pro + Aluminum Case | ~$150 | [DAC2 Pro](https://www.hifiberry.com/shop/boards/hifiberry-dac2-pro/) |

---

## Architecture / アーキテクチャ

```
┌─────────────────────────────────────────────────────────────┐
│                      Control Plane                          │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────────┐   │
│  │ REST API│  │WebSocket│  │  mDNS   │  │   Web UI    │   │
│  └────┬────┘  └────┬────┘  └────┬────┘  └──────┬──────┘   │
│       └────────────┴────────────┴───────────────┘          │
├─────────────────────────────────────────────────────────────┤
│                       Data Plane                            │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────────┐   │
│  │  Audio  │→ │ Pipeline│→ │   RTP   │→ │   Network   │   │
│  │ Capture │  │  (DSP)  │  │  + OSTP │  │ (Multicast) │   │
│  └─────────┘  └─────────┘  └─────────┘  └─────────────┘   │
├─────────────────────────────────────────────────────────────┤
│                    Synchronization                          │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────────┐   │
│  │  PTPv2  │↔ │  BMCA   │↔ │  Clock  │↔ │  Adaptive   │   │
│  │ Engine  │  │         │  │  Servo  │  │    PLL      │   │
│  └─────────┘  └─────────┘  └─────────┘  └─────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

---

## Development / 開発

### Build with tests / テスト付きビルド

```bash
mkdir build && cd build
cmake .. -DSOLUNA_BUILD_TESTS=ON
make -j$(nproc)
ctest --output-on-failure
```

### Build options / ビルドオプション

| Option | Default | Description |
|--------|---------|-------------|
| `SOLUNA_BUILD_TESTS` | ON | Build unit tests |
| `SOLUNA_ENABLE_OPUS` | OFF | Enable Opus codec |
| `SOLUNA_ENABLE_AES67` | OFF | Enable AES67 compatibility |
| `SOLUNA_ENABLE_DTLS` | OFF | Enable DTLS encryption |
| `SOLUNA_ENABLE_AAC` | OFF | Enable AAC codec |
| `SOLUNA_ENABLE_FLAC` | OFF | Enable FLAC codec |

---

## Documentation / ドキュメント

- [Installation Guide](docs/installation.md) / [インストールガイド](docs/ja/installation.md)
- [Configuration Reference](docs/configuration.md) / [設定リファレンス](docs/ja/configuration.md)
- [API Reference](docs/api.md) / [APIリファレンス](docs/ja/api.md)
- [ESP32 Firmware](docs/esp32.md) / [ESP32ファームウェア](docs/ja/esp32.md)
- [Raspberry Pi Setup](docs/raspberry-pi.md) / [Raspberry Piセットアップ](docs/ja/raspberry-pi.md)
- [Troubleshooting](docs/troubleshooting.md) / [トラブルシューティング](docs/ja/troubleshooting.md)

---

## Contributing / コントリビューション

Contributions are welcome! Please read [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

コントリビューション歓迎です！ガイドラインは[CONTRIBUTING.md](CONTRIBUTING.md)をご覧ください。

---

## License / ライセンス

MIT License. See [LICENSE](LICENSE) for details.

---

## Acknowledgments / 謝辞

- Inspired by Dante, AES67, and AVB standards
- Uses IEEE 1588-2008 PTPv2 for synchronization
- FFmpeg for optional codec support
