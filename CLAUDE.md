# Soluna (OpenSonic) — AI Engineering Reference

## プロジェクト概要
Solunaはオープンソースのネットワークオーディオシステム。Mac→iPhone/ブラウザにリアルタイム音声配信。
OSTプロトコル（RTPベース）でP2Pメッシュ + WANリレー。

## アーキテクチャ

| コンポーネント | パス | 言語 | デプロイ先 |
|-------------|------|------|----------|
| **solunad** (TX daemon) | `apps/daemon/main.cpp` | C++ | Mac ローカル |
| **soluna-relay** (WAN relay) | `apps/relay/main.cpp` | C++ | Fly.io `soluna-relay` |
| **Soluna.app** (Mac GUI) | `apps/mac-rx/` | Swift | .pkg/.dmg/.app |
| **SolunaReceiver** (iOS) | `apps/ios/` | Swift | TestFlight / App Store |
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

### Relay デプロイ（3リージョン）
```bash
cd apps/relay && fly deploy -a soluna-relay
# 追加リージョン: fly machine clone <id> --region lax/ams -a soluna-relay
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
TX が OSTP stream_id ヘッダの上位4ビットでチャンネル数をエンコード。RX は自動検出。
- 1ch (Mono), 2ch (Stereo/default), 6ch (5.1), 8ch (7.1)

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

### リレーリージョン
- **nrt** (Tokyo) — primary
- **lax** (Los Angeles)
- **ams** (Amsterdam)

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

## よくあるミス
- **x86_64ビルドのopusリンクエラー**: arm64ビルドのみで回避
- **PKG署名なし → Gatekeeper拒否**: `build-pkg.sh` はデフォルトで署名する
- **Notarize失敗**: `notarytool-profile` のcredentialsが未設定。上記セットアップ手順を参照
- **ペルソナページのリンク**: `yukihamada/opensonic` (enablerdaoではない)
- **Webデプロイ**: `web/` を直接デプロイせず `deploy/web/` にコピーしてから `deploy/` でデプロイ
- **リレーhost**: `relay.solun.art` (旧: 46.225.77.119)
- **DMGは*.dmgが.gitignoreされない**: `.gitignore` に `*.pkg` `*.zip` あるが `*.dmg` を追加すべき
