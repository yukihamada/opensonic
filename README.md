# ◈ Soluna

**Mac の音を、どこでも鳴らす。どこからでも話せる。**

Soluna は Mac のシステム音声を iPhone・Raspberry Pi・ブラウザへ低遅延で配信し、マイク音声の双方向送信にも対応するオープンソースのネットワークオーディオシステムです。BlackHole や複雑な設定は不要。**Soluna 仮想デバイスをシステム出力に選ぶだけ**で動きます。

```
Mac スピーカー ──┐                                    ┌→ UDP マルチキャスト
                 ├── Soluna（仮想デバイス）→ solunad ─┼→ P2P ユニキャストリレー
iPhone/RPi  ────┘                                    └→ WebSocket → ブラウザ

iPhone/Mac マイク → OSTP マルチキャスト → 全レシーバーで再生（双方向）
```

---

## ワンコマンドインストール（Mac）

```bash
# クローン済みの場合
bash scripts/install-mac.sh

# ネットから直接インストール
curl -fsSL https://raw.githubusercontent.com/yukihamada/opensonic/master/scripts/install-mac.sh | bash
```

これだけで以下が自動セットアップされます:
1. **solunad** ビルド＆インストール (`~/.local/bin/`)
2. **Soluna.driver** 仮想オーディオデバイス (`/Library/Audio/Plug-Ins/HAL/`)
3. **LaunchAgent** ログイン時自動起動 (`~/Library/LaunchAgents/`)
4. **coreaudiod** 再起動

インストール後:
- **システム設定 → サウンド → 出力 → Soluna** を選択
- **http://localhost:8400** でダッシュボードを開く

### アンインストール

```bash
bash scripts/uninstall-mac.sh
```

### 手動セットアップ

<details>
<summary>ステップごとに手動で行う場合</summary>

#### 1. ビルド

```bash
git clone https://github.com/yukihamada/opensonic.git
cd opensonic
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

#### 2. ドライバをインストール

```bash
bash apps/plugin/install.sh
```

> **初回のみ** — システム設定 → プライバシーとセキュリティ で「許可」をクリック → `sudo killall coreaudiod`

#### 3. 出力先を変える

**システム設定 → サウンド → 出力 → Soluna** を選択

#### 4. デーモンを起動

```bash
# テスト（1 回だけ）
./build/solunad --tx --device soluna --speaker ""

# Mac ログイン時に自動起動（推奨）
bash apps/daemon/install-service.sh
```

</details>

起動したら **http://localhost:8400** でダッシュボードを開けます。

---

## 受信側のセットアップ

### iPhone

1. `apps/ios/SolunaReceiver.xcodeproj` を Xcode で開いてビルド
2. Mac と同じ Wi-Fi に繋ぐだけで **Bonjour 自動検出**
3. 繋がったら遅延が自動キャリブレーションされます
4. 🎤 マイクボタンで双方向音声送信（他のレシーバーで再生）

### macOS レシーバー

1. `apps/mac-rx/SolunaReceiverMac.xcodeproj` を Xcode で開いてビルド
2. Start ボタンでマルチキャスト受信開始
3. 🎤 マイクボタンで Mac のマイク音声を送信
4. BT/AirPlay/USB スピーカーを追加して同期再生

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
| **🎤 マイク双方向送信** | iPhone/Mac のマイク音声を OSTP マルチキャストで送信。全レシーバーで再生 |
| **BT / AirPlay 同期出力** | Bluetooth・AirPlay・USB スピーカーを追加して全デバイス同期再生 |
| **同期再生モード** | OSTP タイムスタンプ + NTP で全レシーバーの再生タイミングを完全同期 |
| **P2P ユニキャストリレー** | WiFi マルチキャストのパケットロスを回避。UDP ユニキャストで安定配信 |
| **自動品質最適化** | RX メトリクスに基づきバッファ・FEC・PLC を自動チューニング |
| **TX/RX 録音 & 比較** | 送受信の WAV を録音し、SNR・スペクトル・ドロップアウトを分析 |
| **実測遅延補正** | デバイスごとの実遅延を EMA 計測し、最も遅いデバイスに自動同期 |
| **VU メーター** | 各出力デバイスの RMS/Peak レベルを 20fps でリアルタイム表示 |
| **手動遅延オフセット** | デバイスごと ±50ms の微調整スライダー（永続化あり） |
| **ルーティングプリセット** | デバイスグループを名前付きで保存・ワンタップ切替・削除 |
| **Mac P2P リレー** | Bonjour + UDP で Mac 間パケット転送。マルチキャスト不達時も再生可 |
| **Opus コーデック** | Opus 圧縮で帯域を 1/10 に削減（48kHz ステレオ: 128kbps）|
| **DSP エフェクト** | コンプレッサー・3バンド EQ・リバーブを内蔵。WebSocket で操作 |
| **WAN リレーサーバー** | インターネット越しにグループ接続。パスワード保護付き |
| **ロックフリージッタバッファ** | 512 スロット固定配列、atomic 操作のみ。ヒープ割り当て・ミューテックスなし |
| **Adriaensen DLL** | Delay-Locked Loop でクロックドリフトを ±500ppm 精度で推定。ピッチ揺れゼロ |
| **Opus FEC** | Opus 帯域内 FEC でパケットロス時に次パケットから欠落フレームを復元 |
| **4 段 PLC** | Opus FEC → Opus PLC → WSOLA → 無音の優先順位で欠落補間 |
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
| macOS | 受信（RX） + マイク送信 | ✅ `apps/mac-rx/` |
| iPhone (iOS) | 受信（RX） + マイク送信 | ✅ `apps/ios/` |
| Raspberry Pi / Linux | 受信（RX） | ✅ `soluna-rx` |
| Windows | 受信（RX） | ✅ `soluna-rx-win` (WASAPI) |
| ブラウザ | 受信（RX） | ✅ Dashboard → Browser タブ |
| DAW (VST3) | 受信（RX） | ✅ `apps/vst/` |
| ESP32 | 受信（RX） | 🔨 開発中 |

---

## プロジェクト構成

```
apps/
  daemon/         solunad — TX/RX デーモン（C++）
  ios/            iPhone レシーバーアプリ（Swift）+ マイク送信
  mac/            macOS Menu bar アプリ（Swift SPM）
  mac-rx/         macOS レシーバーアプリ（Swift）+ マイク送信
  linux-rx/       Linux/RPi CLI レシーバー（C++）
  plugin/         CoreAudio HAL ドライバ（Soluna.driver）
deploy/
  rpi/            soluna-rx.service + install-rx.sh
src/              libsoluna_core（コアライブラリ）
include/soluna/
  sync/drift_dll.h  Adriaensen DLL（クロック同期）
  wifi/jitter_buffer.h  ロックフリージッタバッファ
third_party/
  r8brain/          r8brain-free-src sinc リサンプラー
tools/
  auto_optimize.py  自動品質最適化スクリプト
  audio_compare.py  TX/RX 音質比較分析ツール
web/              Web UI（index.html / app.js / style.css / guide.html）
```

---

## 音質エンジン（v2 — SonoBus 比較ベース）

SonoBus のソースコードと 2024〜2026 年の最新論文を解析し、以下を実装。

| 改善項目 | Before | After |
|---------|--------|-------|
| ジッタバッファ | `std::map` + `std::mutex`（ヒープ・優先度逆転） | ロックフリー 512 スロット固定配列 `atomic<bool>` |
| クロック同期 | PI 制御 Kp=0.3（ピッチ揺れ大） | Adriaensen DLL (BW=0.01Hz) + buffer PI (Kp=0.01) |
| PLC | WSOLA のみ | Opus FEC → Opus PLC → WSOLA → 無音（4 段） |
| Opus FEC | なし | `use_fec=true`, `packet_loss_pct=5` |
| デグリッチ | デフォルト ON（誤検出でトランジェント破壊） | デフォルト OFF |
| オーバーフロー排出 | 75% でドレイン → グリッチ | 95% → 50% に緩和 |

### Adriaensen DLL

Fons Adriaensen の "Using a DLL to filter time" (2005) に基づく 2 次遅延ロックループ。
コールバック間隔のジッタから真のサンプルレートを推定し、±500ppm の精度でリサンプリング比を出力。

```
BW = 0.01 Hz（収束 2〜5 秒、ピッチ揺れ不可聴）
ω = 2π × BW × (block_size / rate)
b = √2 × ω,  c = ω²
ratio = nominal_rate / estimated_rate  (clamped to ±500ppm)
```

### PLC 優先チェーン

```
Gap == 1: Opus FEC decode（次パケットのFECデータから復元）
Gap ≥ 1: Opus PLC（opus_decode_float(NULL, 0) — 心理音響補間）
Gap ≥ 3: WSOLA（ピッチ同期オーバーラップ加算）
最終手段: 無音フェードアウト
```

---

## Opus コーデック

PCM（非圧縮）とOpus（圧縮）を選択可能。WiFi/WAN環境では帯域削減に有効。

```bash
# Opus で送信（デフォルトは PCM）
solunad --tx --device soluna --codec opus

# PCM で送信（従来どおり）
solunad --tx --device soluna --codec pcm
```

| 項目 | PCM | Opus |
|------|-----|------|
| 帯域（48kHz/2ch） | 3.07 Mbps | 128 kbps |
| 遅延 | 2ms | +5ms（エンコード） |
| 音質 | ロスレス | 透過品質 |

---

## DSP エフェクト

コンプレッサー・3バンドパラメトリックEQ・リバーブを内蔵。デフォルトは全バイパス。WebSocket API で制御。

```json
// エフェクト一覧取得
{"command":"dsp.list"}

// EQ の Low バンドゲインを +6dB に設定
{"command":"dsp.set","plugin":"EQ","param":"low_gain_db","value":6.0}

// コンプレッサーを有効化
{"command":"dsp.bypass","plugin":"Compressor","bypassed":false}
```

| プラグイン | パラメータ |
|-----------|-----------|
| Compressor | threshold_db, ratio, attack_ms, release_ms, makeup_db |
| EQ | low_gain_db, low_freq_hz, low_q, mid_gain_db, mid_freq_hz, mid_q, high_gain_db, high_freq_hz, high_q |
| Reverb | mix, decay, pre_delay_ms |

---

## WAN リレー（インターネット越し配信）

LAN 外のリスナーにもリレーサーバー経由で配信可能。

```bash
# リレーサーバーを起動（VPS やクラウドで）
soluna-relay --port 5100

# TX 側（solunad）→ リレーに転送
solunad --tx --device soluna --wan-relay <relay_host>:5100 \
  --wan-group myroom --wan-password secret123

# RX 側（soluna-rx）→ リレーから受信
soluna-rx --relay <relay_host>:5100 --group-name myroom --group-password secret123
```

---

## VST3 プラグイン（DAW 受信）

DAW 内で Soluna のネットワーク音声を直接受信できる VST3 プラグイン。

```bash
cmake -B build -DSOLUNA_BUILD_VST=ON
cmake --build build --target soluna_vst

# macOS: プラグインをインストール
cp -r build/Soluna.vst3 ~/Library/Audio/Plug-Ins/VST3/
```

DAW でインサートするとマルチキャスト `239.69.0.1:5004` を自動受信。Volume / Mute / Buffer Size パラメータ付き。

---

## Windows 受信

```bash
# Windows でビルド（Visual Studio / MSYS2）
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target soluna-rx-win

# マルチキャスト受信
soluna-rx-win --output wasapi

# P2P ユニキャスト受信
soluna-rx-win --peer <Mac_IP>:5099 --output wasapi
```

---

## マルチトラック録音

TX と Monitor の音声を同時に WAV ファイルへ記録。

```bash
# solunad: ディレクトリ指定で自動録音開始
solunad --tx --device soluna --record-dir /tmp/soluna-rec

# soluna-rx: タイムスタンプ付きで自動録音
soluna-rx --peer <Mac_IP>:5099 --record-dir /tmp/soluna-rec
```

WebSocket API でリモート制御:
```json
{"command":"recording.start","dir":"/tmp/rec"}
{"command":"recording.stop"}
{"command":"recording.status"}
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
| `SOLUNA_ENABLE_OPUS` | ON | Opus コーデック有効化 |
| `SOLUNA_ENABLE_AES67` | OFF | AES67 互換モード |
| `SOLUNA_ENABLE_DTLS` | OFF | DTLS 暗号化 |
| `SOLUNA_BUILD_VST` | OFF | VST3 プラグインをビルド |

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
