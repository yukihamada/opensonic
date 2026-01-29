# Soluna Receiver for iOS

iPhoneやiPadでネットワークオーディオを受信するためのiOSアプリ。

## 機能

- RTPマルチキャストオーディオ受信
- AES67/OSTP プロトコル対応
- シンプルなUIで再生/停止
- 音量調整
- バックグラウンド再生対応

## 必要環境

- macOS with Xcode 15+
- iOS 14.0+
- Apple Developer Account (TestFlight配布用)

## ビルド

### 1. ライブラリのビルド

```bash
cd opensonic
mkdir build-ios && cd build-ios
cmake .. -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
xcodebuild -project soluna.xcodeproj -scheme soluna_core -configuration Release -sdk iphoneos -arch arm64 build
```

### 2. アプリのビルド

```bash
cd apps/ios
open SolunaReceiver.xcodeproj
# Xcodeでビルド＆実行
```

## Fastlane でのTestFlight配布

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

### TestFlightへアップロード

```bash
bundle exec fastlane beta
```

### ローカルビルドのみ

```bash
bundle exec fastlane build_local
```

## 使い方

1. MacでSolunaデーモンを起動:
   ```bash
   solunad --tx --device "MacBook Pro Microphone"
   ```

2. iPhoneでアプリを起動

3. 再生ボタンをタップ

4. 同一ネットワーク上であればオーディオが聴こえます

## 設定

アプリ内の設定画面で以下を変更可能:

- **マルチキャストグループ**: デフォルト `239.69.0.1`
- **ポート**: デフォルト `5004`
- **チャンネル数**: モノラル/ステレオ
- **自動接続**: アプリ起動時に自動で接続

## ライセンス

MIT License
