# Soluna Receiver for iOS

iPhone / iPad でネットワークオーディオを受信するための iOS アプリ。Mac の solunad が配信する RTP/OSTP マルチキャストストリームを受信し、低遅延で再生します。

## 機能

- **マルチキャスト受信** — RTP/OSTP（239.69.0.1:5004）ストリームを受信して再生
- **Bonjour 自動検出** — 同一ネットワーク上の solunad を自動検出・接続（手動追加も可能）
- **グローバル遅延コントロール** — iPhone と全リモートスピーカーを単一スライダーで同一遅延に同期
- **一括音量・ミュート** — 全スピーカーを一度に制御
- **PLC（パケットロス補間）** — 最大 2 パケット（≤20ms）の欠落を前フレームで自動補完
- **リアルタイム統計** — 受信パケット数・ドロップ率・バッファ量・PLC 補完数を表示
- **自動同期** — WebSocket RTT 計測でリモートスピーカーの遅延を自動最適化
- **バックグラウンド再生** — アプリがバックグラウンドでも音声を継続受信
- **設定保存** — マルチキャストグループ・ポート・チャンネル数・自動接続設定を永続化

## 必要環境

- Xcode 15+
- iOS 14.0+
- Apple Developer Account（TestFlight 配布用）
- 同一 LAN 上で稼働中の solunad

## ビルド

### 1. soluna_core ライブラリのビルド

```bash
cd /path/to/opensonic
mkdir build-ios && cd build-ios
cmake .. -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
xcodebuild -project soluna.xcodeproj -scheme soluna_core \
  -configuration Release -sdk iphoneos -arch arm64 build
```

### 2. アプリのビルド

```bash
cd apps/ios
open SolunaReceiver.xcodeproj
# Xcode でターゲットデバイスを選択してビルド & 実行
```

コマンドラインでのビルド確認:

```bash
xcodebuild -project SolunaReceiver.xcodeproj \
  -scheme SolunaReceiver \
  -destination 'generic/platform=iOS' \
  -configuration Debug build
```

## 使い方

1. Mac で solunad を起動:
   ```bash
   # 手動起動
   ./build/solunad --tx --device soluna --speaker ""

   # または launchd で自動起動（推奨）
   bash apps/daemon/install-service.sh
   ```

2. iPhone でアプリを起動

3. 画面中央の再生ボタンをタップ

4. 同一ネットワーク上であれば自動的に接続されオーディオが聴こえます

### リモートスピーカーの追加

Bonjour で検出された solunad は自動的に Speakers リストに表示されます。手動追加する場合は「+」ボタンから IP アドレスを入力してください。

### 遅延の調整

**Global Delay** スライダーを動かすと、iPhone のジッターバッファと全リモートスピーカーの遅延が同時に変更されます。Wi-Fi 環境や接続台数に応じて 20〜80ms 程度に調整してください。

## 設定

アプリ内の設定画面（右上の歯車アイコン）で変更可能:

| 設定項目 | デフォルト | 説明 |
|---------|----------|------|
| マルチキャストグループ | `239.69.0.1` | solunad の送信先グループ |
| ポート | `5004` | UDP ポート番号 |
| チャンネル数 | `2` | モノラル(1) / ステレオ(2) |
| 自動接続 | OFF | アプリ起動時に自動で受信開始 |

## Fastlane での TestFlight 配布

### セットアップ

```bash
cd apps/ios
bundle install
```

### 環境変数の設定

```bash
export APPLE_ID="your-apple-id@example.com"
export TEAM_ID="YOUR_TEAM_ID"
export APP_STORE_CONNECT_API_KEY_ID="XXXXXXXXXX"
export APP_STORE_CONNECT_API_ISSUER_ID="xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
export APP_STORE_CONNECT_API_KEY_CONTENT=$(base64 -i AuthKey_XXXXXXXXXX.p8)
```

### TestFlight へアップロード

```bash
bundle exec fastlane beta
```

### ローカルビルドのみ

```bash
bundle exec fastlane build_local
```

## アーキテクチャ

```
[solunad TX]
    └── UDP multicast 239.69.0.1:5004
           ↓
[SolunaAudioReceiver (C++)]  ← AudioReceiverBridge.mm
    ├── SimpleRtpReceiver    — RTP/OSTP パケット受信・デコード
    ├── RingBuffer           — PCM リングバッファ
    └── PLC                  — ギャップ検出・前フレーム補完
           ↓ (コールバック)
[AudioReceiver.swift]         — @Published stats → SwiftUI
    └── AVAudioEngine        — システム音声出力
           ↓
[ContentView.swift]           — SwiftUI UI
    └── SpeakersController   — DaemonClient (WebSocket) × N台
```

## ライセンス

MIT License
