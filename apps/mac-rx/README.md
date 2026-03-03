# Soluna Rx for Mac

Mac App Store で配布される macOS 用ネットワークオーディオレシーバー。
Soluna TX（solunad）が配信するマルチキャスト RTP/OSTP ストリームを受信し、Mac のスピーカーから再生します。

```
[solunad TX] ──UDP multicast──▶ [soluna_core C++] ──▶ [AudioReceiver] ──▶ [CoreAudio] ──▶ 🔊
                239.69.0.1:5004          ▲                    ▲
                                   Bridging Header      SwiftUI UI
```

## Features

| Feature | Description |
|---------|------------|
| Multicast RTP/OSTP | 239.69.0.1:5004 で自動受信 |
| Bonjour Auto-Discovery | LAN 上の solunad を自動検出 |
| Multi-Speaker Control | 複数スピーカーの一括ボリューム・ミュート・ディレイ制御 |
| Auto-Sync | WebSocket RTT 計測で自動レイテンシ同期 |
| Packet Loss Compensation (PLC) | パケットロスを補完して途切れない再生 |
| Real-time Stats | パケット数、ドロップ率、バッファ、PLC カウント表示 |
| Adaptive Jitter Buffer | ネットワーク状態に応じた自動バッファ調整 |
| Peer Relay | 近くのデバイスを中継して受信エリア拡大 |
| Native SwiftUI | macOS 13+ ネイティブ UI |

## Requirements

- macOS 13.0 (Ventura) 以降
- Apple Silicon (arm64)
- Soluna TX（solunad）が同一ネットワーク上で稼働していること

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
│   ├── PeerRelayManager.swift        # P2P リレー (macOS スタブ)
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
| PeerRelayManager.swift | API 互換 | no-op スタブ (MultipeerConnectivity 不要) |

## Settings

| Key | Default | Description |
|-----|---------|------------|
| Multicast Group | 239.69.0.1 | マルチキャストグループアドレス |
| Port | 5004 | UDP ポート番号 |
| Channels | 2 | オーディオチャンネル数 |
| Auto Connect | OFF | 起動時に自動受信開始 |
| Buffer | 20ms | ジッターバッファサイズ |

## App Store

- **Bundle ID**: `com.soluna.SolunaReceiverMac`
- **Category**: Music / Utilities
- **Price**: Free
- **macOS**: 13.0+
- **Architecture**: arm64

## License

See [opensonic LICENSE](../../LICENSE) for details.
