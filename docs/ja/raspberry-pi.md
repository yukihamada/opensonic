# Raspberry Piセットアップガイド

このガイドでは、Raspberry PiへのSolunaのインストールと設定について説明します。

## クイックインストール

```bash
curl -sSL https://soluna.dev/install.sh | sudo bash
```

これにより以下が実行されます：
1. Piのモデルとアーキテクチャを検出
2. 依存関係をインストール（libasound2、libssl3）
3. Solunaパッケージをダウンロード・インストール
4. オーディオデバイスを検出
5. デフォルト設定を生成
6. systemdサービスを有効化・起動

## 対応モデル

| モデル | 状態 | 備考 |
|-------|------|------|
| Raspberry Pi 5 | ✅ | 推奨 |
| Raspberry Pi 4 | ✅ | 推奨 |
| Raspberry Pi 3 | ✅ | 良好なパフォーマンス |
| Raspberry Pi Zero 2 W | ✅ | WiFiストリーミング |
| Raspberry Pi Zero W | ⚠️ | 限定的（シングルコア）|

## 手動インストール

### パッケージから

```bash
# パッケージをダウンロード
wget https://github.com/example/soluna/releases/download/v0.1.0/soluna_0.1.0_arm64.deb

# インストール
sudo dpkg -i soluna_0.1.0_arm64.deb

# 必要に応じて不足している依存関係をインストール
sudo apt-get install -f
```

### ソースから

```bash
# ビルド依存関係をインストール
sudo apt-get install git cmake g++ libasound2-dev libssl-dev

# クローンしてビルド
git clone https://github.com/example/soluna.git
cd soluna
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
sudo make install
```

## 設定

`/etc/soluna/config.yaml`を編集：

```yaml
device:
  name: "living-room-pi"
  audio: "hw:sndrpihifiberry"  # HiFiBerry DAC

audio:
  sample_rate: 48000
  channels: 2
```

## オーディオデバイスセットアップ

### 内蔵オーディオ

```yaml
device:
  audio: "hw:0"
```

### USBオーディオ

```bash
# デバイス一覧
aplay -l

# USBデバイスを使用（通常カード1）
device:
  audio: "hw:1"
```

### HiFiBerry DAC

1. `/boot/config.txt`を編集：

```
# 内蔵オーディオを無効化
dtparam=audio=off

# HiFiBerryを有効化
dtoverlay=hifiberry-dacplus
```

2. 再起動して設定：

```yaml
device:
  audio: "hw:sndrpihifiberry"
```

### IQaudio DAC

```
dtoverlay=iqaudio-dacplus
```

## サービス管理

```bash
# ステータス確認
sudo systemctl status soluna

# ログ表示
sudo journalctl -u soluna -f

# 再起動
sudo systemctl restart soluna

# 停止
sudo systemctl stop soluna

# 自動起動を無効化
sudo systemctl disable soluna
```

## パフォーマンスチューニング

### CPUガバナー

一貫した低レイテンシのため：

```bash
# パフォーマンスモードに設定
echo "performance" | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

永続化するには`/etc/rc.local`に追加。

### リアルタイム優先度

systemdサービスは`Nice=-10`と`LimitRTPRIO=99`を設定済み。さらに低いレイテンシのため：

```bash
# サービスを編集
sudo systemctl edit soluna

# 追加：
[Service]
CPUSchedulingPolicy=fifo
CPUSchedulingPriority=80
```

### メモリロック

サービスファイルの`LimitMEMLOCK=infinity`で有効済み。

## ネットワーク

### 有線（推奨）

最低レイテンシのためEthernetを使用。スイッチが対応していればジャンボフレームを有効化：

```bash
sudo ip link set eth0 mtu 9000
```

### WiFi

動作しますがレイテンシは高くなります。5GHz帯を使用：

```yaml
audio:
  frames_per_packet: 96  # WiFi用2ms
  buffer_packets: 16     # より多くのバッファリング
```

### 固定IP

`/etc/dhcpcd.conf`を編集：

```
interface eth0
static ip_address=192.168.1.50/24
static routers=192.168.1.1
static domain_name_servers=192.168.1.1
```

## 監視

### Prometheusメトリクス

デフォルトでポート9100で有効：

```bash
curl http://localhost:9100/metrics
```

### Grafanaダッシュボード

`docs/grafana-dashboard.json`からSolunaダッシュボードをインポート。

## トラブルシューティング

### オーディオ出力なし

1. ALSAを確認：
   ```bash
   aplay -l
   speaker-test -c 2
   ```

2. 権限を確認：
   ```bash
   sudo usermod -a -G audio soluna
   ```

3. 設定を確認：
   ```bash
   cat /etc/soluna/config.yaml
   ```

### 高レイテンシ

1. WiFiの代わりにEthernetを使用
2. `frames_per_packet`を減らす
3. パフォーマンスCPUガバナーを有効化

### サービスが起動しない

```bash
# ログを確認
sudo journalctl -u soluna -n 50

# 手動でテスト
sudo -u soluna /usr/bin/solunad --config /etc/soluna/config.yaml
```

### 権限拒否

```bash
# オーディオグループのメンバーシップを確認
sudo usermod -a -G audio soluna

# サービスを再起動
sudo systemctl restart soluna
```

## ヘッドレスセットアップ

モニターなしで実行する場合：

1. SSHを有効化：
   ```bash
   sudo raspi-config  # Interface Options > SSH
   ```

2. IPアドレスを確認：
   ```bash
   hostname -I
   ```

3. Web UIにアクセス：`http://<pi-ip>:8400/`

## アンインストール

```bash
curl -sSL https://soluna.dev/uninstall.sh | sudo bash
```

または手動：

```bash
sudo systemctl stop soluna
sudo systemctl disable soluna
sudo rm /etc/systemd/system/soluna.service
sudo dpkg -r soluna
```

## ハードウェア推奨

### 梅（Budget）〜4,000円

| 部品 | リンク |
|-----|--------|
| Raspberry Pi Zero 2 W | [公式](https://www.raspberrypi.com/products/raspberry-pi-zero-2-w/) |
| USBオーディオアダプター | Amazon |
| microSDカード 16GB | Amazon |

### 竹（Standard）〜12,000円

| 部品 | リンク |
|-----|--------|
| Raspberry Pi 4 2GB | [公式](https://www.raspberrypi.com/products/raspberry-pi-4-model-b/) |
| HiFiBerry DAC+ | [HiFiBerry](https://www.hifiberry.com/shop/boards/hifiberry-dac-plus/) |
| 公式電源 | [公式](https://www.raspberrypi.com/products/type-c-power-supply/) |
| microSDカード 32GB | Amazon |

### 松（Premium）〜25,000円

| 部品 | リンク |
|-----|--------|
| Raspberry Pi 4 4GB | [公式](https://www.raspberrypi.com/products/raspberry-pi-4-model-b/) |
| HiFiBerry DAC2 Pro | [HiFiBerry](https://www.hifiberry.com/shop/boards/hifiberry-dac2-pro/) |
| アルミケース（ヒートシンク）| [Argon](https://argon40.com/) |
| 公式電源 | [公式](https://www.raspberrypi.com/products/type-c-power-supply/) |
| microSDカード 64GB | Amazon |

## 次のステップ

- [設定リファレンス](configuration.md) - 詳細な設定オプション
- [トラブルシューティング](troubleshooting.md) - 問題解決
- [APIリファレンス](api.md) - プログラムからの制御
