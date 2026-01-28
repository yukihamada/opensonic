# ESP32ファームウェアガイド

ESP32マイクロコントローラーへのSolunaのビルド、書き込み、設定について説明します。

## 対応ハードウェア

| ボード | 状態 | 備考 |
|-------|------|------|
| ESP32-DevKitC | ✅ | 初心者向け推奨 |
| ESP32-S3-DevKitC | ✅ | 最高性能（デュアルコア、PSRAM）|
| ESP32-WROVER | ✅ | 8MB PSRAMでバッファリング |
| ESP32-C3 | ⚠️ | シングルコア、性能限定 |
| ESP32-S2 | ⚠️ | シングルコア、Bluetoothなし |

## ハードウェアセットアップ

### 配線図（I2S DAC）

```
ESP32          PCM5102A DAC
------         ------------
GPIO25  -----> BCLK (ビットクロック)
GPIO26  -----> LRCK (ワードセレクト)
GPIO22  -----> DIN  (データ入力)
3.3V    -----> VCC
GND     -----> GND
GND     -----> SCK (GNDに接続)
```

### 梅コース（〜2,000円）

| 部品 | 用途 | リンク |
|-----|------|--------|
| ESP32-DevKitC | マイコン | [AliExpress](https://aliexpress.com/item/1005001267643044.html) |
| PCM5102Aモジュール | DAC | [AliExpress](https://aliexpress.com/item/32836612292.html) |
| ジャンパーワイヤー | 接続 | 任意 |
| USBケーブル | 電源+プログラミング | 任意 |

### 竹コース（〜4,000円）

| 部品 | 用途 | リンク |
|-----|------|--------|
| ESP32-S3-DevKitC | マイコン（高性能）| [AliExpress](https://aliexpress.com/item/1005004452396855.html) |
| PCM5102Aモジュール | DAC | [AliExpress](https://aliexpress.com/item/32836612292.html) |
| LM386アンプ | アンプ（任意）| [AliExpress](https://aliexpress.com/item/32833637279.html) |
| 3Dプリントケース | 筐体 | [Printables](https://printables.com) |

### 松コース（〜10,000円）

| 部品 | 用途 | リンク |
|-----|------|--------|
| ESP32-S3-WROOM-1 | マイコン | [Mouser](https://mouser.com) |
| ES9038Q2M DAC | Hi-Fi DAC | [AliExpress](https://aliexpress.com/item/1005003493783197.html) |
| TPA3116D2アンプ | Class-Dアンプ | [AliExpress](https://aliexpress.com/item/1005003108391596.html) |
| アルミケース | EMIシールド | Amazon |

## ファームウェアのビルド

### 必要なもの

ESP-IDF v5.0以上をインストール：

```bash
# Linux/macOS
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh
source export.sh

# Windows: ESP-IDF Tools Installerを使用
```

### ビルド手順

```bash
# ESP32ファームウェアに移動
cd apps/esp32

# ターゲットボードを設定
idf.py set-target esp32  # または esp32s3

# 設定（任意）
idf.py menuconfig

# ビルド
idf.py build

# 書き込み（/dev/ttyUSB0は実際のポートに置き換え）
idf.py -p /dev/ttyUSB0 flash

# シリアル出力をモニター
idf.py -p /dev/ttyUSB0 monitor
```

### ビルド設定

`idf.py menuconfig`内：

```
Soluna Configuration --->
    [*] Enable WiFi
    [ ] Enable Bluetooth Audio (A2DP)
    [*] Enable FEC (Forward Error Correction)
    (2)  Number of audio channels
    (48000) Sample rate

Component config --->
    ESP32-specific --->
        (240) CPU frequency
    Wi-Fi --->
        [*] WiFi AMPDU TX
        [*] WiFi AMPDU RX
```

## ビルド済みファームウェアの書き込み

### esptool.pyを使用

```bash
# esptoolをインストール
pip install esptool

# ファームウェアをダウンロード
wget https://github.com/example/soluna/releases/download/v0.1.0/soluna-esp32.bin

# 書き込み
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
    write_flash -z 0x10000 soluna-esp32.bin
```

### Webフラッシャーを使用

[https://soluna.dev/flash](https://soluna.dev/flash)にアクセスして指示に従ってください。
（Web Serial APIをサポートするChrome/Edgeが必要）

## 初期設定

### 方法1: Web UI（推奨）

1. ESP32の電源を入れる
2. WiFi AP `Soluna-XXXXXX`に接続（パスワード: `soluna123`）
3. ブラウザで`http://192.168.4.1`を開く
4. WiFiとデバイス設定を構成
5. 保存して再起動

### 方法2: シリアルコンソール

```bash
# シリアルモニターで接続
idf.py -p /dev/ttyUSB0 monitor

# またはscreenを使用
screen /dev/ttyUSB0 115200
```

コマンド：
```
help                    - 利用可能なコマンドを表示
wifi <ssid> <password>  - WiFi認証情報を設定
name <device-name>      - デバイス名を設定
mode <tx|rx|txrx>       - 動作モードを設定
channel <1-8>           - チャンネル数を設定
save                    - 設定をNVSに保存
reboot                  - デバイスを再起動
status                  - 現在の状態を表示
```

### 方法3: NVSパーティション

書き込み前にNVSを事前設定：

```bash
# nvs_data.csvを作成
namespace,key,type,value
soluna,wifi_ssid,string,MyWiFi
soluna,wifi_pass,string,MyPassword
soluna,device_name,string,living-room
soluna,mode,u8,1
soluna,channels,u8,2

# バイナリを生成
python $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py \
    generate nvs_data.csv nvs.bin 0x6000

# NVSパーティションを書き込み
esptool.py write_flash 0x9000 nvs.bin
```

## 設定リファレンス

| パラメータ | 型 | デフォルト | 説明 |
|-----------|-----|---------|------|
| `wifi_ssid` | string | "" | WiFiネットワーク名 |
| `wifi_pass` | string | "" | WiFiパスワード |
| `device_name` | string | "esp32-soluna" | 発見用デバイス名 |
| `mode` | u8 | 1 (RX) | 0=TX, 1=RX, 2=TXRX |
| `channels` | u8 | 2 | オーディオチャンネル（1-8）|
| `rtp_port` | u16 | 5004 | RTP受信ポート |
| `fec_enabled` | u8 | 1 | FEC有効（0/1）|
| `target_latency_ms` | float | 20.0 | 目標再生レイテンシ |

## 動作モード

### RXモード（レシーバー）

ネットワークからオーディオを受信し、I2S DACに出力。

```
ネットワーク → RTP/OSTP → ジッターバッファ → I2S DAC → スピーカー
```

用途：ワイヤレススピーカー、ヘッドフォン、PAシステム

### TXモード（トランスミッター）

I2S ADCからオーディオをキャプチャし、ネットワークに送信。

```
マイク → I2S ADC → RTP/OSTP → ネットワーク
```

用途：ワイヤレスマイク、楽器

### TXRXモード（双方向）

TXとRXを同時実行。インターコムシステムに便利。

## P2Pスタンドアロンモード

ESP32はデスクトップデーモンなしでデバイス間直接ストリーミングが可能。

### シナリオ：ESP32 2台

**トランスミッター（マイク付き）:**
- モード：TX
- 自動的にPTPリーダーになる
- マルチキャストで存在をアナウンス

**レシーバー（スピーカー付き）:**
- モード：RX
- トランスミッターを自動検出
- PTPリーダーに同期

### シナリオ：TX 1台、RX複数台

**トランスミッター:**
- マルチキャストアドレスに送信
- PTPグランドマスターとして動作

**レシーバー（複数）:**
- マルチキャストグループに参加
- すべて同じPTPクロックに同期
- 同期再生

### P2P用設定

TXデバイスで：
```
mode tx
name studio-mic
channel 1
save
reboot
```

RXデバイスで：
```
mode rx
name kitchen-speaker
channel 2
save
reboot
```

## Webインターフェース

WiFi接続後、`http://<esp32-ip>/`でアクセス。

### ダッシュボード

- デバイス状態（名前、モード、IP）
- PTP同期状態とオフセット
- パケット統計（TX/RX/損失/回復）
- オーディオバッファレベル
- 空きヒープメモリ

### 設定

- WiFi認証情報
- デバイス名
- 動作モード
- チャンネル数
- FEC有効/無効
- 目標レイテンシ

### アクション

- 設定を保存
- デバイスを再起動
- 工場出荷時リセット
- OTAファームウェア更新

## OTA更新

### Web UI経由

1. 新しいファームウェア`.bin`ファイルをダウンロード
2. Webインターフェースを開く
3. 設定 → ファームウェア更新に移動
4. ファイルを選択してアップロード
5. 再起動を待つ

### コマンドライン経由

```bash
# espota.pyを使用
python $IDF_PATH/components/esptool_py/esptool/espota.py \
    -i 192.168.1.50 -p 3232 \
    -f build/soluna.bin
```

### HTTP経由

```bash
curl -X POST http://192.168.1.50/api/ota \
    -F "firmware=@build/soluna.bin"
```

## トラブルシューティング

### オーディオ出力なし

1. I2S配線を確認（BCLK、LRCK、DIN）
2. DACに電源が供給されているか確認
3. シリアルログでエラーを確認：
   ```
   idf.py monitor
   ```
4. ネットワーク接続を確認：
   ```
   status
   ```

### WiFiに接続できない

1. 認証情報を確認（大文字小文字区別）
2. 2.4GHzネットワークを確認（ESP32は5GHz非対応）
3. ルーターが新規接続を許可しているか確認
4. 工場出荷時リセットを試す：
   ```
   factory_reset
   reboot
   ```

### オーディオの途切れ/ドロップアウト

1. WiFi信号強度を確認
2. 目標レイテンシを増加：
   ```
   latency 30
   save
   ```
3. FECを有効化：
   ```
   fec 1
   save
   ```
4. ルーターに近づくか、Ethernet（ESP32-PoE）を使用

### PTPが同期しない

1. ルーターでマルチキャストが有効か確認
2. すべてのデバイスが同じサブネット上にあるか確認
3. ファイアウォールがUDP 319/320をブロックしていないか確認
4. PTPステータスを表示：
   ```
   ptp_status
   ```

### デバイスが検出されない

1. ネットワークでmDNSが有効か確認
2. マルチキャストルーティングを確認
3. デバイスが同じVLAN上にあるか確認
4. 手動接続を試す：
   ```bash
   # デスクトップで
   solunad --connect 192.168.1.50
   ```

## パフォーマンスチューニング

### 最低レイテンシ用

```
fec 0             # FECを無効化
latency 10        # 10ms目標
channel 1         # モノラルのみ
save
```

### 信頼性重視（WiFi）

```
fec 1             # FECを有効化
latency 30        # 30msバッファ
save
```

### メモリ最適化

```
channel 2         # チャンネル数を制限
# より多くのチャンネルにはPSRAM付きESP32-WROVERを使用
```

## 仕様

| パラメータ | 値 |
|-----------|-----|
| サンプルレート | 48000 Hz |
| ビット深度 | 24ビット |
| 最大チャンネル | 8（PSRAM使用時）|
| レイテンシ | 10-50ms（設定可能）|
| 電源 | USB経由5V、約300mA（通常）|
| WiFi | 2.4GHz 802.11b/g/n |
