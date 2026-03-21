# Soluna (OpenSonic) — AI Engineering Reference

## プロジェクト概要
Solunaはオープンソースのネットワークオーディオシステム。Mac→iPhone/ブラウザにリアルタイム音声配信。
OSTプロトコル（RTPベース）でP2Pメッシュ + WANリレー。

## アーキテクチャ

| コンポーネント | パス | 言語 | デプロイ先 |
|-------------|------|------|----------|
| **solunad** (TX daemon) | `apps/daemon/main.cpp` | C++ | Mac ローカル |
| **soluna-relay** (WAN relay) | `apps/relay/main.cpp` | C++ | AWS Tokyo (relay.solun.art:5100 UDP) |
| **soluna-quic-bridge** | `crates/soluna-quic-relay/` | Rust | AWS Tokyo (alongside relay) |
| **soluna-radio-cpp** (自動配信) | `apps/radio/` | C++ | AWS Tokyo (7 channels) |
| **Soluna** (Mac GUI) | `apps/mac-rx/` | Swift | .pkg/.dmg/.app |
| **SolunaReceiver** (iOS) | `apps/ios/` | Swift | TestFlight / App Store |
| **SolunaSDK** | `sdk/swift/` | Swift (SPM) | iOS / macOS shared library (57+ sources) |
| **Web** (landing + dashboard) | `web/`, `deploy/` | HTML/JS | Fly.io `soluna-web` |
| **Core library** | `CMakeLists.txt` | C++ | 全プラットフォーム共有 |

## ビルド & デプロイ

### Mac .pkg / .dmg / .app ビルド（署名付き）
```bash
# 署名付きビルド（推奨）
cd /Users/yuki/workspace/infra/opensonic
SOLUNA_VERSION=0.3.0 bash scripts/build-pkg.sh

# 署名なしビルド
SOLUNA_VERSION=0.3.0 bash scripts/build-pkg.sh --no-sign

# Universal (arm64 + x86_64) — opusのx86_64リンクエラー注意
SOLUNA_VERSION=0.3.0 bash scripts/build-pkg.sh --universal
```

**出力**: `Soluna-mac.pkg` (署名済み: Developer ID Installer)

**DMG / APP zip 作成**:
```bash
# APP zip
cd build-pkg/staging/payload/Applications
zip -r ../../../../Soluna-mac.app.zip Soluna.app
cd ../../../../

# DMG
DMG_DIR=$(mktemp -d)
cp -r build-pkg/staging/payload/Applications/Soluna.app "$DMG_DIR/"
ln -s /Applications "$DMG_DIR/Applications"
hdiutil create -volname "Soluna" -srcfolder "$DMG_DIR" -ov -format UDZO Soluna-mac.dmg
rm -rf "$DMG_DIR"
codesign --force --sign "Developer ID Application: Yuki Hamada (5BV85JW8US)" Soluna-mac.dmg
```

### GitHub Release
```bash
git tag v0.X.0 && git push origin v0.X.0
gh release create v0.X.0 Soluna-mac.pkg Soluna-mac.dmg Soluna-mac.app.zip \
  --title "Soluna v0.X.0" --notes "changelog here"
```

### iOS TestFlight / App Store
```bash
cd apps/ios
bundle exec fastlane beta     # TestFlight
bundle exec fastlane release  # App Store 申請
```

### Web (landing + persona pages) デプロイ
```bash
# web/ の変更を deploy/web/ にコピーしてデプロイ
cp -r web/* deploy/web/
cd deploy && fly deploy -a soluna-web
```

### Relay デプロイ（AWS Tokyo）
```bash
# Relay is on AWS Tokyo (relay.solun.art:5100 UDP)
# Hetzner VPS: DELETED. Fly.io relay: STOPPED. AWS is sole relay.
ssh ubuntu@relay.solun.art  # manage relay + radio processes
```

## Protocol Support

| Protocol | TX | RX | Platforms | Build Flag |
|----------|----|----|-----------|------------|
| OSTP | ✅ | ✅ | Mac, iOS, Linux, Web | Always on |
| AES67 | ✅ | ✅ | Mac, Linux | `SOLUNA_ENABLE_AES67=ON` (default) |
| Ravenna | ✅ | ✅ | Mac, Linux | `SOLUNA_ENABLE_RAVENNA=ON` |
| AirPlay 2 | ✅ | ✅ | Mac, Linux | `SOLUNA_ENABLE_AIRPLAY=ON` |
| DLNA/UPnP | ❌ (planned) | ✅ | Mac, Linux | `SOLUNA_ENABLE_DLNA=ON` |
| PipeWire | — | ✅ (output) | Linux only | `SOLUNA_ENABLE_PIPEWIRE=ON` |

### 全プロトコル有効ビルド
```bash
cmake -DSOLUNA_ENABLE_AES67=ON \
      -DSOLUNA_ENABLE_RAVENNA=ON \
      -DSOLUNA_ENABLE_AIRPLAY=ON \
      -DSOLUNA_ENABLE_DLNA=ON \
      -DSOLUNA_ENABLE_PIPEWIRE=ON \
      ..
cmake --build build
```

### チャンネル数
TX が OSTP stream_id ヘッダ bits[11:8] でチャンネル数をエンコード。RX は自動検出。
- 0=1ch (Mono), 1=2ch (Stereo/default), 2=6ch (5.1), 3=8ch (7.1)

### プロトコル要点
- **OSTP/RTP**: PT=96 for S24-in-S32LE (24-bit audio in 32-bit int container, NOT packed 24-bit)
- **CRC-32 trailer** (4 bytes) at end of every packet
- **OSTP extension profile**: 0x4F53 ("OS") with stream_id for channel count

## 署名 & Notarize

### コード署名ID
- **App署名**: `Developer ID Application: Yuki Hamada (5BV85JW8US)`
- **PKG署名**: `Developer ID Installer: Yuki Hamada (5BV85JW8US)`
- **iOS配布**: `Apple Distribution: Yuki Hamada (5BV85JW8US)`
- **Team ID**: `5BV85JW8US`

### Notarize セットアップ（初回のみ）
```bash
# appleid.apple.com でapp-specific passwordを生成してから:
xcrun notarytool store-credentials notarytool-profile \
  --apple-id mail@yukihamada.jp --team-id 5BV85JW8US
# → app-specific password を入力
```
設定後は `build-pkg.sh` が自動的にnotarizeする。

### PKG署名の確認
```bash
pkgutil --check-signature Soluna-mac.pkg
codesign --verify --deep --strict Soluna-mac.dmg
```

## App Store Connect
- **App ID**: `com.soluna.SolunaReceiver` (ID: 6759962263)
- **App名**: Soluna Rx
- **Team**: 5BV85JW8US
- **API Key**: 環境変数 `APP_STORE_CONNECT_API_KEY_ID`, `ISSUER_ID`, `KEY_PATH`
- **TestFlight公開リンク**: https://testflight.apple.com/join/PYbefDSE

## ネットワーク

| サービス | URL | 用途 |
|---------|-----|------|
| Web | https://soluna-web.fly.dev | Landing + Dashboard |
| Relay | relay.solun.art:5100 (UDP) | WAN音声リレー |
| Relay API | https://relay.solun.art | チャンネルページ `/c/<name>` |
| GitHub | github.com/yukihamada/opensonic | ソースコード + Releases |

### リレーインフラ
- **AWS Tokyo** — sole relay (relay.solun.art:5100 UDP)
- Hetzner VPS: DELETED
- Fly.io relay: STOPPED

## 収益分配
- 権利者: 70%
- プラットフォーム: 10%
- DJキャッシュバック: 20%

## iOS アプリ仕様

### 音声の流れ
- **送信 (TX)**: Mac で `solunad --tx` が必須。システムオーディオをキャプチャしてリレーに送信
- **受信 (RX)**: iPhone/ブラウザはチャンネル名だけでリレー経由で受信（受信側にsolunad不要）

### iOS受信フロー
1. アプリ起動 → `autoStart()` → `start()`
2. オーディオ出力を即座に開始
3. WANリレー (`relay.solun.art`) にチャンネル名で自動接続
4. 並行してP2Pピアスキャン（3秒）
5. 送信者がsolunadでそのチャンネルに音を流していれば聴こえる

### Deep Link
- Universal Link: `https://relay.solun.art/c/<channel>`
- URL scheme: `soluna://channel/<name>`
- Entitlements: `SolunaReceiver/SolunaReceiver.entitlements`

### Fastlane環境変数
- `APP_STORE_CONNECT_API_KEY_ID`
- `APP_STORE_CONNECT_ISSUER_ID`
- `APP_STORE_CONNECT_API_KEY_PATH`

## Web ページ構成

| パス | ファイル | nginx route |
|------|---------|-------------|
| `/` | `landing.html` | `location = /` |
| `/dashboard` | `index.html` | `location = /dashboard` |
| `/guide` | `guide.html` | `location = /guide` |
| `/for-djs` | `for-djs.html` | `location = /for-djs` |
| `/for-venues` | `for-venues.html` | `location = /for-venues` |
| `/for-events` | `for-events.html` | `location = /for-events` |
| `/for-developers` | `for-developers.html` | `location = /for-developers` |

## ラジオ配信インフラ (AWS Tokyo)

本番の音声配信は **AWS Tokyo** (relay.solun.art) で稼働。C++ バイナリ `soluna-radio-cpp` が各チャンネルを配信。

### プロセス構成
```
soluna-relay (port 5100)        ← WAN リレーサーバ本体
  ↑ localhost UDP
soluna-radio-cpp × 7            ← 各チャンネルの自動配信 (S24 PT=96)
  ↑ ffmpeg (MP3 → s32le 48kHz mono)
/data/music/{genre}/*.mp3       ← 音源ファイル
```

### チャンネル一覧（全て kFreeNames — 誰でも DJ ロール取得可）
| チャンネル | 音源ディレクトリ |
|-----------|----------------|
| bjj | `/data/music/bjj/` |
| soluna | `/data/music/soluna/` |
| jazz | `/data/music/jazz/` |
| chill | `/data/music/chill/` |
| lofi | `/data/music/lofi/` |
| dance | `/data/music/dance/` |
| yuki | `/data/music/yuki/` |

### 起動
- `soluna-radio-all.sh` が全チャンネルを一括起動
- 各 `soluna-radio-cpp --dir <dir> --relay 127.0.0.1:5100 --channel <name>`

### 注意
- 音源追加は AWS の `/data/music/<genre>/` に MP3 を置くだけ
- YouTube からの直接DL機能はない（Mac solunad のシステム音声キャプチャか、ローカルMP3再生）

## 音声再生アーキテクチャ (iOS/Mac)

iOS/Mac アプリはリレー音声の再生に **SDKAudioReceiver** (純 Swift) を使用。旧 C++ ブリッジは廃止。

### SDKAudioReceiver の設計
- **AVAudioSourceNode** (pull-based) でオーディオスレッドから直接サンプル供給
- **Lock-free SPSC mono ring buffer**: 192K Float (4秒 @ 48kHz)
- **S24-in-S32LE デコード**: 24-bit 音声が 32-bit int コンテナに格納。スケール `1.0 / 2^23`
- **100ms prefill threshold**: バッファが 100ms 分溜まるまで無音出力、その後再生開始
- C++ AudioReceiverBridge は LAN マルチキャスト用に残存

## よくあるミス
- **x86_64ビルドのopusリンクエラー**: arm64ビルドのみで回避
- **PKG署名なし → Gatekeeper拒否**: `build-pkg.sh` はデフォルトで署名する
- **Notarize失敗**: `notarytool-profile` のcredentialsが未設定。上記セットアップ手順を参照
- **ペルソナページのリンク**: `yukihamada/opensonic` (enablerdaoではない)
- **Webデプロイ**: `web/` を直接デプロイせず `deploy/web/` にコピーしてから `deploy/` でデプロイ
- **リレーhost**: `relay.solun.art` (AWS Tokyo。旧 Hetzner/Fly.io は廃止)
- **DMGは*.dmgが.gitignoreされない**: `.gitignore` に `*.pkg` `*.zip` あるが `*.dmg` を追加すべき
