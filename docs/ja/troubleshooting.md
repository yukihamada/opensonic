# トラブルシューティングガイド

Solunaの一般的な問題と解決方法です。

## クイック診断

```bash
# サービス状態を確認
sudo systemctl status soluna

# 最近のログを表示
sudo journalctl -u soluna -n 100

# オーディオデバイスを確認
solunad --list-devices

# ネットワークを確認
solunad --status

# オーディオ出力をテスト
speaker-test -c 2 -t wav
```

## オーディオの問題

### オーディオ出力なし

**症状:** サービスは動作しているが音が出ない

**解決方法:**

1. **オーディオデバイスを確認:**
   ```bash
   # デバイス一覧
   aplay -l

   # 直接再生をテスト
   speaker-test -c 2

   # 設定のデバイス名を確認
   cat /etc/soluna/config.yaml | grep audio
   ```

2. **権限を確認:**
   ```bash
   # ユーザーをaudioグループに追加
   sudo usermod -a -G audio $USER
   # またはサービスユーザーの場合
   sudo usermod -a -G audio soluna

   # サービスを再起動
   sudo systemctl restart soluna
   ```

3. **ALSAミキサーを確認:**
   ```bash
   alsamixer
   # Mキーでミュート解除、矢印キーで音量調整
   ```

4. **ストリームがアクティブか確認:**
   ```bash
   solctl streams
   solctl routes
   ```

### オーディオの途切れ / ドロップアウト

**症状:** クリック音、ポップ音、途切れ

**解決方法:**

1. **バッファサイズを増加:**
   ```yaml
   # config.yaml内
   audio:
     frames_per_packet: 96   # 48から増加
     buffer_packets: 12      # 8から増加
   ```

2. **CPUガバナーを確認:**
   ```bash
   # パフォーマンスモードに設定
   echo "performance" | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
   ```

3. **リアルタイム優先度を有効化:**
   ```bash
   # /etc/security/limits.conf を編集
   @audio - rtprio 99
   @audio - memlock unlimited
   ```

4. **ネットワーク品質を確認:**
   ```bash
   # パケットロスを監視
   watch -n 1 'solctl status'

   # WiFiなら有線接続を使用
   ```

5. **干渉を確認:**
   ```bash
   # メトリクスを表示
   curl http://localhost:9100/metrics | grep soluna_rtp_packets_lost
   ```

### 高レイテンシ

**症状:** ソースと出力間で顕著な遅延

**解決方法:**

1. **バッファを削減:**
   ```yaml
   audio:
     frames_per_packet: 48   # 48kHzで1ms
     buffer_packets: 4       # 最小安全値
   ```

2. **有線ネットワークを使用:**
   - WiFiからEthernetに切り替え
   - レイテンシ: WiFi 約20-50ms、Ethernet 約2-5ms

3. **PTP同期を確認:**
   ```bash
   solctl status
   # ptp_offset_ns は < 1000000 (1ms) であるべき
   ```

4. **WiFi省電力を無効化:**
   ```bash
   sudo iw wlan0 set power_save off
   ```

### 歪んだオーディオ

**症状:** クリッピング、歪み、ピッチのずれ

**解決方法:**

1. **ゲインレベルを確認:**
   ```bash
   solctl routes
   # ゲインが > 0 dB なら下げる
   solctl route gain --source X --sink Y --db -6
   ```

2. **サンプルレートの一致を確認:**
   ```yaml
   # すべてのデバイスが同じレートを使用する必要あり
   audio:
     sample_rate: 48000
   ```

3. **メーターでクリッピングを確認:**
   ```bash
   solctl meters
   # peak_db は < -3 dB であるべき
   ```

## ネットワークの問題

### デバイスが検出されない

**症状:** `solctl devices`が空または不足

**解決方法:**

1. **ネットワーク接続を確認:**
   ```bash
   # IPアドレスを確認
   ip addr show

   # 他のデバイスにping
   ping 192.168.1.x
   ```

2. **マルチキャストを確認:**
   ```bash
   # マルチキャストルーティングを確認
   ip maddr show

   # マルチキャストをテスト（送信側）
   echo "test" | socat - UDP4-DATAGRAM:239.69.0.1:5353

   # マルチキャストをテスト（受信側）
   socat UDP4-RECVFROM:5353,ip-add-membership=239.69.0.1:eth0 -
   ```

3. **ファイアウォールを確認:**
   ```bash
   # Solunaポートを許可
   sudo ufw allow 8400/tcp  # Control
   sudo ufw allow 5004/udp  # RTP
   sudo ufw allow 319/udp   # PTP Event
   sudo ufw allow 320/udp   # PTP General
   sudo ufw allow 5353/udp  # mDNS
   ```

4. **mDNSを確認:**
   ```bash
   # avahiツールをインストール
   sudo apt install avahi-utils

   # Solunaサービスを検索
   avahi-browse -r _soluna._tcp
   ```

### PTPが同期しない

**症状:** `ptp_synced: false`または大きなオフセット

**解決方法:**

1. **PTPトラフィックを確認:**
   ```bash
   # PTPパケットをキャプチャ
   sudo tcpdump -i eth0 port 319 or port 320
   ```

2. **マルチキャストメンバーシップを確認:**
   ```bash
   netstat -g | grep 224.0.1.129
   ```

3. **複数のPTPマスターがないか確認:**
   - グランドマスターは1台のみ
   - priority1の値が低い方が勝つ

4. **ネットワーク輻輳を減らす:**
   - オーディオ用専用VLANを使用
   - QoS（DSCP EF）を有効化

### 接続拒否

**症状:** コントロールAPIに接続できない

**解決方法:**

1. **サービスが動作しているか確認:**
   ```bash
   sudo systemctl status soluna
   ```

2. **ポートバインドを確認:**
   ```bash
   ss -tlnp | grep 8400
   ```

3. **ファイアウォールを確認:**
   ```bash
   sudo ufw status
   sudo iptables -L -n | grep 8400
   ```

4. **バインドアドレスを確認:**
   ```yaml
   # 設定で、localhostのみにバインドされていないことを確認
   network:
     control_port: 8400
     # bind: 0.0.0.0  # すべてのインターフェース
   ```

## サービスの問題

### サービスが起動しない

**症状:** `systemctl start soluna`が失敗

**解決方法:**

1. **ログを確認:**
   ```bash
   sudo journalctl -u soluna -n 50 --no-pager
   ```

2. **手動起動をテスト:**
   ```bash
   sudo -u soluna /usr/bin/solunad --config /etc/soluna/config.yaml
   ```

3. **設定を検証:**
   ```bash
   solunad --config /etc/soluna/config.yaml --validate
   ```

4. **ファイル権限を確認:**
   ```bash
   ls -la /etc/soluna/
   # solunaユーザーが読めること
   ```

5. **オーディオデバイスの存在を確認:**
   ```bash
   aplay -l | grep -i <device-name>
   ```

### サービスがクラッシュ

**症状:** サービスが予期せず停止

**解決方法:**

1. **セグフォルトを確認:**
   ```bash
   dmesg | grep -i soluna
   sudo journalctl -u soluna | grep -i "signal\|crash\|fault"
   ```

2. **コアダンプを有効化:**
   ```bash
   # サービスファイルに追加
   [Service]
   LimitCORE=infinity

   # リロードして再起動
   sudo systemctl daemon-reload
   sudo systemctl restart soluna
   ```

3. **メモリを確認:**
   ```bash
   free -m
   # Solunaは最低約50MB RAMが必要
   ```

4. **デバッグログで実行:**
   ```yaml
   logging:
     level: "debug"
   ```

### 高CPU使用率

**症状:** CPUが常に100%

**解決方法:**

1. **オーディオパラメータを確認:**
   ```yaml
   # 処理負荷を削減
   audio:
     channels: 2         # チャンネル数を減らす
     frames_per_packet: 96  # パケットを大きく
   ```

2. **未使用機能を無効化:**
   ```yaml
   metrics:
     enabled: false
   logging:
     level: "warn"
   ```

3. **ログで無限ループを確認:**
   ```bash
   sudo journalctl -u soluna -f
   # 繰り返しエラーを探す
   ```

## ESP32の問題

### ESP32がWiFiに接続しない

**解決方法:**

1. 2.4GHzネットワークを確認（ESP32は5GHz非対応）
2. SSID/パスワードを確認（大文字小文字区別）
3. 工場出荷時リセットして再設定：
   ```
   factory_reset
   reboot
   ```
4. ルーターの接続デバイス数制限を確認

### ESP32オーディオの途切れ

**解決方法:**

1. 目標レイテンシを増加：
   ```
   latency 30
   save
   ```
2. FECを有効化：
   ```
   fec 1
   save
   ```
3. WiFiルーターに近づく
4. ルーターで5GHz帯を使用（干渉が少ない）

### ESP32 Web UIにアクセスできない

**解決方法:**

1. シリアルコンソールでESP32のIPアドレスを確認：
   ```
   status
   ```
2. 同じネットワーク/サブネット上か確認
3. 別のブラウザを試す
4. コンピューターのファイアウォールを確認

## Raspberry Piの問題

### RPiオーディオのノイズ

**解決方法:**

1. パフォーマンスCPUガバナーを使用：
   ```bash
   echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
   ```

2. WiFi電源管理を無効化：
   ```bash
   sudo iw wlan0 set power_save off
   ```

3. GPUメモリを増加（CPU負荷を軽減）：
   ```
   # /boot/config.txt 内
   gpu_mem=128
   ```

4. 内蔵の代わりにUSBオーディオを使用：
   ```yaml
   device:
     audio: "hw:1"  # USBデバイス
   ```

### RPiサービスが再起動ループ

**解決方法:**

1. 起動時にオーディオデバイスが存在するか確認：
   ```bash
   # サービスに遅延を追加
   sudo systemctl edit soluna

   [Service]
   ExecStartPre=/bin/sleep 5
   ```

2. 依存関係を確認：
   ```bash
   sudo systemctl list-dependencies soluna
   ```

## ヘルプを得る

### 診断情報を収集

```bash
# システム情報
uname -a
cat /etc/os-release

# Solunaバージョン
solunad --version

# 設定
cat /etc/soluna/config.yaml

# ログ（最後の100行）
sudo journalctl -u soluna -n 100 --no-pager

# オーディオデバイス
aplay -l
arecord -l

# ネットワーク
ip addr show
ip route show

# メトリクス
curl http://localhost:9100/metrics 2>/dev/null | grep soluna_
```

### 問題を報告

1. 上記の診断情報を収集
2. 期待される動作と実際の動作を説明
3. 再現手順を含める
4. Issueを開く: https://github.com/example/soluna/issues

### コミュニティ

- Discord: [soluna.dev/discord](https://soluna.dev/discord)
- フォーラム: [forum.soluna.dev](https://forum.soluna.dev)
- IRC: Libera.Chat の #soluna
