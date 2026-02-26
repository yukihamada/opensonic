# Soluna — ネットワークオーディオシステム

<p align="center">
  <img src="docs/logo.svg" alt="Soluna Logo" width="200">
</p>

[![Build Status](https://github.com/yukihamada/opensonic/actions/workflows/ci.yml/badge.svg)](https://github.com/yukihamada/opensonic/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

**Soluna** は、Mac のシステム音声をネットワーク経由で iPhone / Raspberry Pi などに低遅延配信しながら、Mac 本体のスピーカーでも同時再生できるオープンソースのネットワークオーディオシステムです。

BlackHole や Multi-Output Device の手動設定は不要です。Soluna という仮想オーディオデバイスをシステム出力に指定するだけで動作します。

---

## 仕組み

```
システム出力: "Soluna"（仮想デバイス）
       ↓
Soluna.driver  （CoreAudio HAL プラグイン）
       ↓ 共有メモリ（$TMPDIR/soluna_audio.shm）
solunad --tx --device soluna
   ├── UDP マルチキャスト → iPhone / Raspberry Pi
   └── CoreAudio 出力    → Mac 本体スピーカー（低遅延）
```

---

## 現在の対応状況

| プラットフォーム | 役割 | 状態 |
|----------------|------|------|
| macOS | 送信（TX） | ✅ 動作確認済み — Soluna 仮想デバイス経由でシステム音声を配信 |
| Raspberry Pi | 受信（RX） | ✅ 動作確認済み — ALSA / ES9038Q2M DAC 確認 |
| iPhone (iOS) | 受信（RX） | 🔨 開発中 |
| ESP32 | 受信（RX） | 🔨 開発中 |

---

## クイックスタート（macOS TX）

### 1. ビルド

```bash
git clone https://github.com/yukihamada/opensonic.git
cd opensonic
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 2. Soluna ドライバのインストール

```bash
bash apps/plugin/install.sh
```

> 初回インストール後、**システム設定 → プライバシーとセキュリティ** に「許可」ボタンが表示されます。クリックしてから再度 `killall coreaudiod` してください。

### 3. 出力先を "Soluna" に変更

**システム設定 → サウンド → 出力** で **Soluna** を選択。

### 4. solunad を起動

```bash
./build/solunad --tx --device soluna --speaker ""
```

- `--speaker ""` : デフォルトデバイス（Mac スピーカー）で同時再生
- `--speaker "BlackHole 2ch"` : 別の出力デバイスを指定する場合

音楽を再生すると、Mac スピーカーと UDP マルチキャスト（239.69.0.1:5004）の両方に流れます。

---

## クイックスタート（Raspberry Pi RX）

```bash
sudo apt-get install -y libasound2-dev cmake g++
git clone https://github.com/yukihamada/opensonic.git
cd opensonic && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4
sudo cp solunad /usr/bin/solunad

# 受信開始
solunad --rx --device default --channels 2
```

### リアルタイム最適化（ドロップアウト対策）

```bash
# SCHED_FIFO を root なしで使えるようにする
sudo setcap cap_sys_nice=ep /usr/bin/solunad

# RT スロットリング無効化
echo 'kernel.sched_rt_runtime_us=-1' | sudo tee -a /etc/sysctl.conf
sudo sysctl -p

# CPU ガバナーをパフォーマンスモードに
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

### systemd サービス

```bash
sudo cp deploy/rpi/soluna.service /etc/systemd/system/
sudo systemctl enable --now soluna
```

---

## アーキテクチャ詳細

### CoreAudio HAL プラグイン（Soluna.driver）

- `/Library/Audio/Plug-Ins/HAL/Soluna.driver` にインストール
- 仮想デバイス「Soluna」を macOS に登録（2ch / 48kHz / float32）
- `DoIOOperation` で受け取った PCM データを共有メモリリングバッファに書き込む

### 共有メモリ（soluna_shm.h）

- パス: `$TMPDIR/soluna_audio.shm`（POSIX SHM ではなく通常ファイル）
  - CoreAudio ドライバのサンドボックスが `shm_open` をブロックするため、TMPDIR 内のファイルを使用
- サイズ: SolunaShmHeader（64 バイト）+ float32 リング（16384 フレーム × 2ch）≈ 131KB
- ロックフリー SPSC: `__atomic_load_n` / `__atomic_store_n` による write_pos / read_pos

### solunad TX（--device soluna モード）

1. SHM ファイルを作成し `soluna_shm_init_header()` でヘッダ初期化
2. SHM リーダースレッドが 256 フレームずつ読み込み
   - → float32 → S24LE 変換 → UDP TX リングバッファ
   - → float32 のまま → スピーカー再生リングバッファ
3. TX スレッド: RTP パケットを 239.69.0.1:5004 へマルチキャスト送信
4. スピーカーコールバック: CoreAudio 出力に直接書き込み

---

## ビルドオプション

```bash
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DSOLUNA_BUILD_TESTS=ON
```

| オプション | デフォルト | 説明 |
|-----------|----------|------|
| `SOLUNA_BUILD_TESTS` | ON | ユニットテストをビルド |
| `SOLUNA_ENABLE_OPUS` | OFF | Opus コーデック有効化 |
| `SOLUNA_ENABLE_AES67` | OFF | AES67 互換モード |
| `SOLUNA_ENABLE_DTLS` | OFF | DTLS 暗号化 |

---

## 他製品との比較

| 機能 | Soluna | Dante | AES67 | BlackHole+Multi |
|------|--------|-------|-------|-----------------|
| ライセンス | MIT（OSS） | 有償 | 標準規格 | MIT |
| 費用 | 無料 | 高額 | 機器依存 | 無料 |
| 仮想デバイス | ✅（不要） | ❌ | ❌ | 手動設定必要 |
| ネットワーク配信 | ✅ | ✅ | ✅ | ❌ |
| Mac スピーカー同時再生 | ✅ | ❌ | ❌ | 設定が複雑 |
| WiFi 対応 | ✅ | 限定的 | ❌ | N/A |
| 組み込み（RPi/ESP32）| ✅ | ❌ | ❌ | N/A |

---

## トラブルシューティング

### "Soluna" がサウンド設定に表示されない

```bash
# ドライバを再インストール
bash apps/plugin/install.sh

# システム設定 → プライバシーとセキュリティ で「許可」をクリック後
sudo killall coreaudiod
```

### 音が出ない（write_pos が 0 のまま）

solunad を先に起動してから音楽を再生してください。ドライバは SHM が存在しない場合、約 2〜3 秒ごとに再接続を試みます。

```bash
# SHM ファイルの存在確認
ls -la $TMPDIR/soluna_audio.shm

# solunad ログで write_pos を確認
./build/solunad --tx --device soluna --speaker "" 2>&1 | head -5
```

### Raspberry Pi でドロップアウトが発生する

「リアルタイム最適化」セクションの `setcap` と `sched_rt_runtime_us` 設定を適用してください。

---

## ライセンス

MIT License — 詳細は [LICENSE](LICENSE) を参照。
