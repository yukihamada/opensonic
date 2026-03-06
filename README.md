# ◈ Soluna

**Mac の音を、どこでも鳴らす。**

Soluna は Mac のシステム音声を iPhone・Raspberry Pi・ブラウザへ低遅延で配信するオープンソースのネットワークオーディオシステムです。BlackHole や複雑な設定は不要。**Soluna 仮想デバイスをシステム出力に選ぶだけ**で動きます。

```
Mac スピーカー ──┐
                 ├── Soluna（仮想デバイス）→ solunad ─┬→ UDP マルチキャスト
iPhone/RPi  ────┘                                    ├→ P2P ユニキャストリレー
                                                     └→ WebSocket → ブラウザ
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

# マルチキャスト受信（デフォルト）
soluna-rx --output alsa

# P2P ユニキャスト受信（WiFi でパケットロスが多い場合に推奨）
soluna-rx --peer <Mac_IP>:5099 --output alsa

# パイプ出力
soluna-rx --output pipe | aplay -f S16_LE -r 48000 -c 2
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
| **P2P ユニキャストリレー** | WiFi マルチキャストのパケットロスを回避。UDP ユニキャストで安定配信 |
| **自動品質最適化** | RX メトリクスに基づきバッファ・FEC・PLC を自動チューニング |
| **TX/RX 録音 & 比較** | 送受信の WAV を録音し、SNR・スペクトル・ドロップアウトを分析 |
| **実測遅延補正** | デバイスごとの実遅延を EMA 計測し、最も遅いデバイスに自動同期 |
| **VU メーター** | 各出力デバイスの RMS/Peak レベルを 20fps でリアルタイム表示 |
| **手動遅延オフセット** | デバイスごと ±50ms の微調整スライダー（永続化あり） |
| **ルーティングプリセット** | デバイスグループを名前付きで保存・ワンタップ切替・削除 |
| **Mac P2P リレー** | Bonjour + UDP で Mac 間パケット転送。マルチキャスト不達時も再生可 |
| パケットロス補間 (PLC) | WSOLA ベースの PLC で最大 2 パケット欠落を補完 |
| FEC (前方誤り訂正) | XOR パリティ (k=4) でパケットロス耐性を向上 |
| NACK 再送要求 | 受信側からの再送要求で欠落パケットを回復 |
| Bonjour 自動検出 | 同一ネットワークの solunad を iPhone が自動で見つける |
| ホットプラグ検出 | BT/AirPlay の接続・切断を自動検出してスピーカー一覧を更新 |
| 設定の永続化 | 音量・遅延・ミュートを `~/.config/solunad/config.json` に保存 |
| Menu bar アプリ | macOS メニューバーから操作（`apps/mac/`） |
| Web UI | `localhost:8400` でブラウザから全操作 |
| launchd 自動起動 | `install-service.sh` で Mac 再起動後も自動起動 |

---

## P2P ユニキャストリレー

WiFi マルチキャストは ACK がないため、環境によっては 50〜75% のパケットロスが発生します。P2P リレーは UDP ユニキャストで配信するため、WiFi レイヤーでの ACK・再送が働き、パケットロスが **0%** に改善されます。

```
solunad (TX)
  ├── マルチキャスト 239.69.0.1:5004  （従来）
  └── ユニキャストリレー :5099         （P2P）
        ↓ hello パケットで自動登録
  soluna-rx --peer <Mac_IP>:5099
```

| 指標 | マルチキャスト | P2P リレー |
|------|--------------|-----------|
| パケットロス | 50〜75% | **0%** |
| 最適バッファ | 200ms+ | **10ms** |
| 品質スコア | POOR | **GOOD** |

### solunad 側オプション

```bash
solunad --tx --device soluna          # リレーは自動でポート 5099 で起動
solunad --tx --relay-port 6000        # ポート変更
solunad --tx --no-relay               # リレー無効
```

### WebSocket API

```json
{"command":"relay.stats"}
→ {"enabled":true, "port":5099, "peer_count":2, "peers":[...]}
```

---

## 自動品質最適化

`tools/auto_optimize.py` はリアルタイムの RX メトリクスを監視し、solunad のパラメータを自動調整します。

```bash
# Raspi に SSH して P2P モードで最適化
python3 tools/auto_optimize.py --raspi pi@raspi.local --solunad-host localhost --peer 192.168.0.194:5099

# stdin モード（パイプ）
soluna-rx --metrics 2>&1 | python3 tools/auto_optimize.py --stdin --solunad-host localhost

# TX/RX の WAV ファイル比較で一括調整
python3 tools/auto_optimize.py --calibrate --tx tx.wav --rx rx.wav --solunad-host localhost
```

調整されるパラメータ:
- **バッファサイズ** — パケットロス時に増加、安定時に自動削減（低遅延化）
- **ノイズリペア** — σ（クリック検出感度）、クロスフェードフレーム数
- **WiFi 機能** — FEC / NACK / PLC / 重複送信 / 適応ジッタバッファ

---

## TX/RX 録音 & 音質比較

```bash
# TX 側（solunad）で送信音声を録音
solunad --tx --device soluna --record-tx tx.wav --record-dur 30

# RX 側（soluna-rx）で受信音声を録音
soluna-rx --peer 192.168.0.194:5099 --record rx.wav --duration 30

# 比較分析
python3 tools/audio_compare.py tx.wav rx.wav --plot
```

出力: SNR (dB)、スペクトル差異（7 バンド）、ドロップアウト検出、サンプルドリフト (ppm)

---

## RX 品質メトリクス

```bash
soluna-rx --metrics --metrics-interval 3
```

stderr に JSON で出力:

```json
{"type":"metrics","pkts_rx":500,"pkts_drop":0,"loss_pct":0.00,
 "rms_db":-25.9,"peak_db":-0.0,"clicks":0,"dropouts":0,"underruns":0}
```

---

## 対応プラットフォーム

| プラットフォーム | 役割 | 状態 |
|----------------|------|------|
| macOS | 送信（TX） | ✅ 動作確認済み |
| macOS | Menu bar コントロール | ✅ `apps/mac/` |
| macOS | 受信（RX） | ✅ `apps/mac-rx/` |
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
  mac-rx/         macOS レシーバーアプリ（Swift）
  linux-rx/       Linux/RPi CLI レシーバー（C++）
  plugin/         CoreAudio HAL ドライバ（Soluna.driver）
deploy/
  rpi/            soluna-rx.service + install-rx.sh
src/              libsoluna_core（コアライブラリ）
tools/
  auto_optimize.py  自動品質最適化スクリプト
  audio_compare.py  TX/RX 音質比較分析ツール
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

**Raspberry Pi でパケットロスが多い**
→ P2P リレーモードを使用してください:
```bash
soluna-rx --peer <Mac_IP>:5099 --output alsa
```

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
