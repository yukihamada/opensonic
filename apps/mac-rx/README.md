# Soluna for Mac

macOS 用ネットワークオーディオアプリ。**これ 1 つで送信・受信・WAN 接続すべてに対応。solunad は不要。**

```
┌─ Soluna アプリ ─────────────────────────────────────────────┐
│                                                             │
│  Soluna.driver（仮想デバイス）─→ 共有メモリ ─→ Audio TX     │
│  マイク入力 ─────────────────────────────→ Mic TX           │
│  ネットワーク受信 ───────────────────────→ スピーカー出力    │
│  WAN グループ接続（P2P ホールパンチング）                    │
│                                                             │
│  BT / AirPlay / USB スピーカー同期再生                      │
│  3バンドEQ / コンプレッサー / 空間オーディオ                │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

## Features

| Feature | Description |
|---------|------------|
| **システム音声送信 (Audio TX)** | Soluna.driver の共有メモリから読み取り → OSTP で送信（solunad 不要） |
| **マイク送信 (Mic TX)** | Mac のマイクを OSTP マルチキャスト + WAN P2P で送信 |
| **WAN P2P グループ接続** | グループコードでインターネット越し P2P 接続（UDP ホールパンチング） |
| Multicast RTP/OSTP | 239.69.0.1:5004 で自動受信 |
| Bonjour Auto-Discovery | LAN 上の solunad を自動検出 |
| Multi-Speaker Control | 複数スピーカーの一括ボリューム・ミュート・ディレイ制御 |
| **Bluetooth / AirPlay 同期出力** | BT・AirPlay・USB スピーカーを追加して全デバイス同期再生 |
| **実測遅延補正** | EMA 平滑化でデバイスごとの実遅延を計測し、最も遅いデバイスに自動同期 |
| **VU メーター** | 各出力の RMS/Peak レベルをリアルタイム表示（20fps, 緑→橙→赤） |
| **手動遅延オフセット** | デバイスごと ±50ms 微調整スライダー（UserDefaults 永続化） |
| **ルーティングプリセット** | アクティブデバイス構成を名前付き保存・ワンタップ復元・右クリック削除 |
| **Mac P2P リレー** | Bonjour `_soluna-relay._udp` + NWConnection UDP でパケット転送 |
| **ホットプラグ検出** | CoreAudio デバイス変更を自動検出。BT/AirPlay の接続・切断に即応 |
| **デバイス永続化** | 有効なデバイス・手動オフセットを UserDefaults に保存・起動時復元 |
| **オートリコネクト** | NWPathMonitor でネットワーク復帰時に自動再接続 |
| **キーボードショートカット** | Space=再生/停止, ⌘↑↓=音量, ⇧⌘M=ミュート, ⇧⌘S=同期 |
| **L/R バランス** | プライマリ・各デバイスに独立 L/R パンニング（constant-power） |
| **Exclusive Mode** | USB DAC 向けホグモード（他アプリの出力をブロック） |
| **メニューバープレーヤー** | MenuBarExtra で常駐、ステータス・音量・同期をクイック操作 |
| **レイテンシグラフ** | デバイスごとの遅延推移をリアルタイム折れ線グラフ表示 |
| **オーディオ録音** | プライマリ出力を WAV (48kHz/16bit) でファイル録音 |
| **3バンド EQ** | 200Hz / 1kHz / 5kHz パラメトリックEQ（±12dB、全出力独立） |
| **スペクトラムアナライザー** | Accelerate vDSP 32バンド FFT（対数周波数マッピング 20Hz-20kHz） |
| **コンプレッサー/リミッター** | 出力ごとのダイナミクス圧縮（スレッショルド / レシオ / アタック / リリース） |
| **プリセット Import/Export** | ルーティングプリセットを JSON で書き出し・読み込み |
| **お気に入りデバイス** | 星マーク登録でホットプラグ時に自動有効化 |
| **Now Playing / Siri** | メディアキー対応 + AppIntents で Siri ショートカット |
| **スピーカーグループ** | 名前付きゾーン（キッチン等）でグループ音量・ミュート制御 |
| **HTTP リモート API** | ポート 9400 の REST API（Stream Deck / Home Assistant 対応） |
| **サンプルレート表示** | デバイスのネイティブレート（44.1kHz / 96kHz 等）をバッジ表示 |
| **マルチチャンネル** | 5.1 / 7.1 サラウンド対応（チャンネルレイアウト表示付き） |
| **クロスオーバーフィルタ** | LPF / HPF（Butterworth 2次）でサブ / サテライト周波数分割 |
| **空間オーディオ** | Mid/Side ステレオワイドナー + HRTF クロスフィードによるバーチャルサラウンド |
| Auto-Sync | WebSocket RTT 計測で自動レイテンシ同期 |
| Packet Loss Compensation (PLC) | パケットロスを補完して途切れない再生（extra sinks にも伝搬） |
| Real-time Stats | パケット数、ドロップ率、バッファ、PLC カウント表示 |
| Adaptive Jitter Buffer | ネットワーク状態に応じた自動バッファ調整 |
| Peer Relay | Bonjour+UDP で Mac 間パケットリレー（マルチキャスト不達時の代替） |
| Native SwiftUI | macOS 13+ ネイティブ UI |

## Requirements

- macOS 13.0 (Ventura) 以降
- Apple Silicon (arm64)
- Soluna.driver（仮想オーディオデバイス）のインストール（システム音声送信に必要）

## Build

### 1. C++ ライブラリビルド

```bash
cd /path/to/opensonic
mkdir -p build-mac && cd build-mac
cmake .. -DCMAKE_OSX_ARCHITECTURES="arm64" -DCMAKE_OSX_DEPLOYMENT_TARGET="13.0"
make -j$(sysctl -n hw.ncpu) soluna_core
```

### 2. Xcode でビルド

```bash
cd apps/mac-rx
open SolunaReceiverMac.xcodeproj
# Xcode > Product > Build (⌘B)
```

### 3. ローカルテスト（署名なし）

```bash
cd apps/mac-rx
bundle exec fastlane build_local
```

## Fastlane

### セットアップ

```bash
# 環境変数を設定
export APP_STORE_CONNECT_API_KEY_ID="YOUR_KEY_ID"
export APP_STORE_CONNECT_ISSUER_ID="YOUR_ISSUER_ID"
export APP_STORE_CONNECT_API_KEY_PATH="/path/to/AuthKey.p8"
```

### コマンド

| Lane | Description |
|------|------------|
| `fastlane build_lib` | soluna_core を arm64 でビルド |
| `fastlane build_local` | ローカルテスト用ビルド（署名なし） |
| `fastlane beta` | ビルド → TestFlight アップロード |
| `fastlane release` | ビルド → App Store 提出 |

### TestFlight アップロード

```bash
cd apps/mac-rx
bundle exec fastlane beta
```

## Project Structure

```
apps/mac-rx/
├── SolunaReceiverMac.xcodeproj/
├── SolunaReceiverMac/
│   ├── SolunaReceiverApp.swift      # @main エントリポイント
│   ├── ContentView.swift             # メイン UI (Hero, Stats, Speakers)
│   ├── SettingsView.swift            # 設定画面
│   ├── AudioReceiver.swift           # CoreAudio 再生エンジン
│   ├── DaemonClient.swift            # WebSocket クライアント (solunad 通信)
│   ├── SpeakersController.swift      # スピーカー管理・Bonjour 検出
│   ├── PeerRelayManager.swift        # P2P リレー (macOS Bonjour+UDP)
│   ├── RemoteControlServer.swift     # HTTP リモート API (ポート 9400)
│   ├── SolunaAppIntents.swift        # Siri ショートカット (AppIntents)
│   ├── Bridge/
│   │   ├── AudioReceiverBridge.h     # C++ ブリッジヘッダ
│   │   ├── AudioReceiverBridge.mm    # Obj-C++ ブリッジ実装
│   │   └── SolunaReceiver-Bridging-Header.h
│   ├── Assets.xcassets/              # アイコン・カラー
│   ├── Info.plist
│   ├── SolunaReceiverMac.entitlements
│   └── PrivacyInfo.xcprivacy
└── fastlane/
    ├── Fastfile                      # CI/CD 定義
    ├── Deliverfile                   # App Store メタデータ
    └── metadata/en-US/              # ストア説明文
```

## Multi-Speaker Sync Architecture

Bluetooth、AirPlay、USB スピーカーを追加して、Mac 本体スピーカーと同期再生できます。

```
RTP → SimpleRtpReceiver → ring_buffer_ (primary, This Mac)
                               ↓ on_audio_written callback
              ┌────────────────┼──────────────────┐
         OutputSink[0]    OutputSink[1]      OutputSink[N]
         (BT Speaker)     (AirPlay)          (USB DAC)
              ↓                ↓                  ↓
         AudioUnit 0      AudioUnit 1       AudioUnit N
         delay=160ms      delay=0ms         delay=1980ms
```

### 同期戦略

1. 各デバイスの遅延を実測（EMA α=0.05 で平滑化）
2. 最も遅いデバイス（通常 AirPlay ~2000ms）を基準（delay=0）
3. それより速いデバイスに `max - own` の補正 delay を付与
4. BT/AirPlay のホットプラグで自動再計算

### OutputSink

1デバイス = 1 OutputSink:
- 独自 RingBuffer（192K frames = 4秒、AirPlay 対応）
- 独自 AudioUnit HAL Output
- volume / mute / delay を atomic で制御
- ソフトリミッター + フェードイン/アウト（プライマリと同品質）

### VU メーター

各 OutputSink の render callback で RMS/Peak を計算し、atomic 変数で公開。
UI 側は 20fps (50ms) タイマーで値をポーリングして `LevelMeter` ビューに反映。

- RMS: √(Σsample²/N)、EMA α=0.3
- Peak: max(|sample|)、decay 0.95/frame
- 色: 緑 (<70%) → 橙 (70-90%) → 赤 (>90%)

### ルーティングプリセット

`AudioRoutingPreset` (Codable) でデバイス構成を保存:
```swift
struct AudioRoutingPreset: Codable {
    var name: String
    var deviceConfigs: [DeviceConfig]  // deviceId, volume, muted, manualOffsetMs
}
```
UserDefaults `soluna_routing_presets` に JSON 保存。
適用時: 全 extra output を一旦解除 → preset のデバイスを順番に有効化・設定復元 → 同期再計算。

### Mac P2P リレー

```
[Mac A: Direct] ──multicast RTP──▶ ring_buffer_
                                        ↓ relayCallback
                                   NWListener (UDP :5099)
                                        ↓ forwardPacket
                    ┌───────────────────┼──────────────┐
               [Mac B: Peer]      [Mac C: Peer]     ...
               NWConnection       NWConnection
                    ↓                   ↓
              injectRawPacket     injectRawPacket
```

- Bonjour `_soluna-relay._udp` でサービス公開/検出
- Peer は `networkDisabled=true` でマルチキャスト受信を停止し、relay 経由のみで受信
- マルチキャストが届かないサブネット間でも動作

### デバイス種別

| Transport | 検出方法 | 典型遅延 | アイコン |
|-----------|---------|---------|---------|
| Built-in | `kAudioDeviceTransportTypeBuiltIn` | ~5ms | `desktopcomputer` |
| USB | `kAudioDeviceTransportTypeUSB` | ~10ms | `cable.connector` |
| Bluetooth | `kAudioDeviceTransportTypeBluetooth` | 40-200ms | `wave.3.right` |
| AirPlay | `kAudioDeviceTransportTypeAirPlay` | ~2000ms | `airplayaudio` |

### DSP パイプライン

各出力（プライマリ + OutputSink）の audio callback で適用:
```
Input → Soft Limiter → 3-Band EQ → Compressor → Crossover (LPF/HPF) → L/R Balance → Spatializer → Output
```

| DSP | 説明 |
|-----|------|
| Soft Limiter | ±0.9 超えで tanh ソフトクリップ |
| 3-Band EQ | 200Hz / 1kHz / 5kHz ピーキング EQ（±12dB, biquad） |
| Compressor | エンベロープフォロワ + スレッショルド / レシオ / アタック / リリース |
| Crossover | Butterworth 2次 LPF or HPF（20-20kHz） |
| L/R Balance | constant-power パンニング（-1.0=左, 0=中, 1.0=右） |
| Spatializer | Mid/Side 幅制御 + HRTF クロスフィードディレイ（~0.3ms） |

### HTTP リモート API

ポート 9400 で JSON API を提供:

| Endpoint | Method | Description |
|----------|--------|------------|
| `/api/status` | GET | 状態・音量・パケット数 |
| `/api/play` | POST | 再生開始 |
| `/api/stop` | POST | 停止 |
| `/api/toggle` | POST | 再生/停止トグル |
| `/api/volume/{0-100}` | POST | 音量設定 |
| `/api/mute` / `unmute` | POST | ミュート ON/OFF |
| `/api/devices` | GET | デバイス一覧 |
| `/api/presets` | GET | プリセット一覧 |
| `/api/preset/{name}` | POST | プリセット適用 |

## Architecture

iOS 版 Soluna Rx と同一のコアコードを使用。macOS 固有の差分のみ変更:

| Layer | Shared with iOS | macOS Specific |
|-------|----------------|----------------|
| soluna_core (C++) | 100% | arm64 ビルドのみ |
| AudioReceiverBridge | 100% | — |
| AudioReceiver.swift | 100% | — |
| DaemonClient.swift | 100% | — |
| SpeakersController.swift | 100% | — |
| ContentView.swift | ~95% | `nsColor`, `desktopcomputer` icon |
| SettingsView.swift | ~95% | `nsColor` |
| PeerRelayManager.swift | API 互換 | Bonjour+UDP 実装 (NWConnection ベース) |

## Settings

| Key | Default | Description |
|-----|---------|------------|
| Multicast Group | 239.69.0.1 | マルチキャストグループアドレス |
| Port | 5004 | UDP ポート番号 |
| Channels | 2 | オーディオチャンネル数（1/2/6/8 = Mono/Stereo/5.1/7.1） |
| Auto Connect | OFF | 起動時に自動受信開始 |
| Buffer | 20ms | ジッターバッファサイズ |

## App Store

- **Bundle ID**: `com.soluna.Soluna`
- **Category**: Music / Utilities
- **Price**: Free
- **macOS**: 13.0+
- **Architecture**: arm64

## License

See [opensonic LICENSE](../../LICENSE) for details.
