# ◈ Soluna

**Mac の音を、どこでも鳴らす。**

Soluna は Mac のシステム音声を iPhone・Raspberry Pi・ブラウザへ低遅延で配信するオープンソースのネットワークオーディオシステムです。BlackHole や複雑な設定は不要。**Soluna 仮想デバイスをシステム出力に選ぶだけ**で動きます。

```
Mac スピーカー ──┐
                 ├── Soluna（仮想デバイス）→ solunad → UDP マルチキャスト
iPhone/RPi  ────┘                                  └→ WebSocket → ブラウザ
```

---

## 5 分で動かす（Mac TX）

### 1. ビルド

```bash
git clone https://github.com/yukihamada/opensonic.git
cd opensonic
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### 2. ドライバをインストール

```bash
bash apps/plugin/install.sh
```

> **初回のみ** — システム設定 → プライバシーとセキュリティ で「許可」をクリック → `sudo killall coreaudiod`

### 3. 出力先を変える

**システム設定 → サウンド → 出力 → Soluna** を選択

### 4. デーモンを起動

```bash
# テスト（1 回だけ）
./build/solunad --tx --device soluna --speaker ""

# Mac ログイン時に自動起動（推奨）
bash apps/daemon/install-service.sh
```

起動したら **http://localhost:8400** でダッシュボードを開けます。

---

## 受信側のセットアップ

### iPhone

1. `apps/ios/SolunaReceiver.xcodeproj` を Xcode で開いてビルド
2. Mac と同じ Wi-Fi に繋ぐだけで **Bonjour 自動検出**
3. 繋がったら遅延が自動キャリブレーションされます

### Raspberry Pi / Linux

```bash
# ワンコマンドインストール（RPi）
sudo bash deploy/rpi/install-rx.sh

# または手動で
soluna-rx --output alsa            # ALSA 出力
soluna-rx --output pipe | aplay -f S16_LE -r 48000 -c 2   # パイプ
```

### ブラウザ

ダッシュボードの **Browser タブ** から即座に試聴。インストール不要。

---

## 主な機能

| 機能 | 説明 |
|------|------|
| 仮想オーディオデバイス | CoreAudio HAL プラグイン。システム出力を Soluna に切り替えるだけ |
| Mac スピーカー同時再生 | ネットワーク配信と Mac 本体スピーカーを同時に出力 |
| **BT / AirPlay 同期出力** | Bluetooth・AirPlay・USB スピーカーを追加して全デバイス同期再生 |
| **実測遅延補正** | デバイスごとの実遅延を EMA 計測し、最も遅いデバイスに自動同期 |
| **VU メーター** | 各出力デバイスの RMS/Peak レベルを 20fps でリアルタイム表示 |
| **手動遅延オフセット** | デバイスごと ±50ms の微調整スライダー（永続化あり） |
| **ルーティングプリセット** | デバイスグループを名前付きで保存・ワンタップ切替・削除 |
| **Mac P2P リレー** | Bonjour + UDP で Mac 間パケット転送。マルチキャスト不達時も再生可 |
| 自動遅延同期 | WebSocket RTT 計測でリモートスピーカーの遅延を自動最適化 |
| パケットロス補間 (PLC) | 最大 2 パケット欠落を前フレームで補完。プツッとしにくい |
| Bonjour 自動検出 | 同一ネットワークの solunad を iPhone が自動で見つける |
| ホットプラグ検出 | BT/AirPlay の接続・切断を自動検出してスピーカー一覧を更新 |
| 設定の永続化 | 音量・遅延・ミュートを `~/.config/solunad/config.json` に保存 |
| Menu bar アプリ | macOS メニューバーから操作（`apps/mac/`） |
| Web UI | `localhost:8400` でブラウザから全操作 |
| launchd 自動起動 | `install-service.sh` で Mac 再起動後も自動起動 |

---

## 対応プラットフォーム

| プラットフォーム | 役割 | 状態 |
|----------------|------|------|
| macOS | 送信（TX） | ✅ 動作確認済み |
| macOS | Menu bar コントロール | ✅ `apps/mac/` |
| iPhone (iOS) | 受信（RX） | ✅ `apps/ios/` |
| Raspberry Pi / Linux | 受信（RX） | ✅ `soluna-rx` |
| ブラウザ | 受信（RX） | ✅ Dashboard → Browser タブ |
| ESP32 | 受信（RX） | 🔨 開発中 |

---

## プロジェクト構成

```
apps/
  daemon/         solunad — TX/RX デーモン（C++）
  ios/            iPhone レシーバーアプリ（Swift）
  mac/            macOS Menu bar アプリ（Swift SPM）
  linux-rx/       Linux/RPi CLI レシーバー（C++）
  plugin/         CoreAudio HAL ドライバ（Soluna.driver）
deploy/
  rpi/            soluna-rx.service + install-rx.sh
src/              libsoluna_core（コアライブラリ）
web/              Web UI（index.html / app.js / style.css / guide.html）
```

---

## ビルドオプション

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DSOLUNA_BUILD_TESTS=ON
```

| オプション | デフォルト | 説明 |
|-----------|----------|------|
| `SOLUNA_BUILD_TESTS` | ON | ユニット・統合テストをビルド |
| `SOLUNA_ENABLE_OPUS` | OFF | Opus コーデック有効化 |
| `SOLUNA_ENABLE_AES67` | OFF | AES67 互換モード |
| `SOLUNA_ENABLE_DTLS` | OFF | DTLS 暗号化 |

---

## トラブルシューティング

**「Soluna」がサウンド設定に出ない**
```bash
bash apps/plugin/install.sh
sudo killall coreaudiod
```

**音が出ない（write_pos が 0）**
→ solunad を先に起動してから音楽を再生してください。

**iPhone の音がプツプツ切れる**
→ Settings → Buffer Size を 60〜120ms に上げてください。

**Raspberry Pi でドロップアウトが発生する**
```bash
sudo setcap cap_sys_nice=ep /usr/local/bin/soluna-rx
echo 'kernel.sched_rt_runtime_us=-1' | sudo tee -a /etc/sysctl.conf && sudo sysctl -p
```

**Mac 再起動後に起動しない**
```bash
launchctl print gui/$UID/io.soluna.daemon   # 状態確認
bash apps/daemon/install-service.sh          # 再登録
```

詳細なセットアップ手順は **http://localhost:8400/guide.html** または `docs/` フォルダを参照。

---

## launchd チートシート

```bash
launchctl print gui/$UID/io.soluna.daemon   # 状態確認
launchctl stop  gui/$UID/io.soluna.daemon   # 停止
launchctl start gui/$UID/io.soluna.daemon   # 開始
launchctl bootout gui/$UID/io.soluna.daemon # 削除
tail -f /tmp/solunad.log                    # ログ
```

---

## ライセンス

MIT License
