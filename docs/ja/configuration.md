# 設定リファレンス

SolunaはYAML設定ファイルを使用します。デフォルトの場所は`/etc/soluna/config.yaml`です。

## クイックスタート

レシーバー用の最小設定：
```yaml
device:
  name: "living-room"
  audio: "default"

audio:
  sample_rate: 48000
  channels: 2
```

トランスミッター用の最小設定：
```yaml
device:
  name: "studio-main"
  audio: "hw:0"

network:
  rtp_base: 5004
```

## 完全リファレンス

### device

デバイス識別とハードウェア設定。

```yaml
device:
  name: "my-device"        # 発見用デバイス名（デフォルト: "soluna-device"）
  audio: "default"         # ALSAデバイス名（Linux）、CoreAudio ID（macOS）
  interface: ""            # ネットワークインターフェース（空 = 自動検出）
```

**オーディオデバイス名:**
- Linux: `default`, `hw:0`, `hw:1`, `plughw:0,0`, `hw:sndrpihifiberry`
- macOS: `default`、または`solunad --list-devices`で表示されるデバイスUID
- Windows: `default`、または`solunad --list-devices`で表示されるデバイス名

### network

ストリーミングと制御用のネットワーク設定。

```yaml
network:
  control_port: 8400       # REST API/WebSocketポート（デフォルト: 8400）
  rtp_base: 5004           # ベースRTPポート（デフォルト: 5004）
  multicast_audio: "239.69.0.1"   # オーディオマルチキャストアドレス
  multicast_ptp: "224.0.1.129"    # PTPマルチキャストアドレス
  dscp: 46                 # DSCPマーキング（46 = EF、34 = AF41）
```

**DSCP値:**
| 値 | クラス | 用途 |
|----|-------|------|
| 46 | EF | リアルタイムオーディオ（推奨）|
| 34 | AF41 | マルチメディアストリーミング |
| 0 | BE | ベストエフォート（デフォルト）|

### audio

オーディオストリームパラメータ。

```yaml
audio:
  sample_rate: 48000       # サンプルレート（Hz）（デフォルト: 48000）
  channels: 2              # チャンネル数（1-64、デフォルト: 2）
  bit_depth: 24            # ビット深度（16、24、32、デフォルト: 24）
  frames_per_packet: 48    # RTPパケットあたりのサンプル数（デフォルト: 48 = 1ms）
  buffer_packets: 8        # リングバッファサイズ（パケット数）（デフォルト: 8）
```

**レイテンシ計算:**
```
レイテンシ (ms) = frames_per_packet / sample_rate * 1000 * buffer_packets
例: 48 / 48000 * 1000 * 8 = 8ms 合計バッファ
```

**推奨設定:**
| ネットワーク | frames_per_packet | buffer_packets | レイテンシ |
|------------|-------------------|----------------|----------|
| 有線LAN | 48 | 4 | 4ms |
| WiFi 5GHz | 96 | 8 | 16ms |
| WiFi 2.4GHz | 144 | 12 | 36ms |

### security

認証と暗号化の設定。

```yaml
security:
  dtls_enabled: false      # DTLS暗号化を有効化
  auth_enabled: false      # デバイス認証を有効化

  # TLS証明書（DTLS用）
  certificate_path: "/etc/soluna/cert.pem"
  private_key_path: "/etc/soluna/key.pem"

  # デバイス認証情報
  devices:
    - id: "studio-console"
      psk: "sha256:abc123..."    # PSKのSHA-256ハッシュ
      roles: [admin]

  # ロール定義
  roles:
    admin:
      - stream_create
      - stream_destroy
      - route_modify
      - config_write
    operator:
      - stream_create
      - route_modify
    viewer:
      - stream_list
      - route_list
```

**PSKハッシュの生成:**
```bash
echo -n "my-secret-key" | sha256sum | cut -d' ' -f1
```

### metrics

Prometheusメトリクスエクスポーター。

```yaml
metrics:
  enabled: true            # メトリクスエンドポイントを有効化
  port: 9100               # メトリクスHTTPポート
  path: "/metrics"         # エンドポイントパス
  scrape_interval_ms: 5000 # 更新間隔
```

**利用可能なメトリクス:**
```
soluna_audio_frames_processed_total    # 処理済みオーディオフレーム数
soluna_rtp_packets_sent_total          # 送信RTPパケット数
soluna_rtp_packets_received_total      # 受信RTPパケット数
soluna_rtp_packets_lost_total          # 損失RTPパケット数
soluna_ptp_offset_ns                   # PTPオフセット（ナノ秒）
soluna_ptp_synced                      # PTP同期状態
soluna_uptime_seconds                  # 稼働時間（秒）
soluna_active_streams                  # アクティブストリーム数
soluna_buffer_underruns_total          # バッファアンダーラン数
soluna_buffer_overruns_total           # バッファオーバーラン数
```

### logging

ログ出力設定。

```yaml
logging:
  level: "info"            # ログレベル: debug, info, warn, error
  file: ""                 # ログファイルパス（空 = 標準出力）
  json_format: false       # JSONログ形式
  include_timestamp: true  # タイムスタンプを含める
```

### audit

セキュリティ監査ログ。

```yaml
audit:
  enabled: true
  file: "/var/log/soluna/audit.jsonl"
  events:                  # ログするイベント（空 = すべて）
    - auth_success
    - auth_failure
    - stream_created
    - stream_destroyed
    - config_changed
```

**監査ログ形式（JSON Lines）:**
```json
{"ts":1706443200000000000,"event":"auth_success","actor":"studio-console","ip":"192.168.1.100"}
{"ts":1706443201000000000,"event":"stream_created","actor":"studio-console","stream_id":1}
```

### routing

自動ストリーム設定用の自動ルーティングルール。

```yaml
routing:
  auto_rules:
    - name: "connect-speakers"
      trigger:
        type: device_connected    # device_connected, device_disconnected
        pattern: "esp32-speaker-*"  # Globパターン
      actions:
        - type: add_route
          source: "studio-main:0"
          sink: "$device:0"       # $device = マッチしたデバイス
          gain_db: -6.0

    - name: "backup-recorder"
      trigger:
        type: device_connected
        pattern: "backup-*"
      actions:
        - type: add_route
          source: "main-mix:0"
          sink: "$device:0"
          gain_db: 0.0
        - type: add_route
          source: "main-mix:1"
          sink: "$device:1"
          gain_db: 0.0
```

**トリガータイプ:**
- `device_connected` - デバイスがネットワークに出現
- `device_disconnected` - デバイスがネットワークから離脱
- `stream_started` - ストリーム開始
- `stream_stopped` - ストリーム終了

**アクションタイプ:**
- `add_route` - オーディオルートを作成
- `remove_route` - オーディオルートを削除
- `set_gain` - ルートゲインを調整

### plugins

ロードするDSPプラグイン。

```yaml
plugins:
  - path: "/usr/lib/soluna/plugins/limiter.so"
    params:
      threshold_db: "-3.0"
      attack_ms: "5"
      release_ms: "100"

  - path: "/usr/lib/soluna/plugins/eq.so"
    params:
      preset: "flat"
```

## 環境変数

設定は環境変数展開をサポート：

```yaml
device:
  name: "${HOSTNAME}"
  audio: "${SOLUNA_AUDIO_DEVICE:-default}"

security:
  psk: "${SOLUNA_PSK}"
```

構文:
- `${VAR}` - 環境変数を使用
- `${VAR:-default}` - 未設定の場合デフォルト値を使用

## 設定パス

Solunaは以下の順序で設定を検索：
1. コマンドライン: `--config /path/to/config.yaml`
2. `./config.yaml`（カレントディレクトリ）
3. `~/.config/soluna/config.yaml`
4. `/etc/soluna/config.yaml`

## 設定の検証

```bash
# 設定構文をチェック
solunad --config /etc/soluna/config.yaml --validate

# 有効な設定を表示
solunad --config /etc/soluna/config.yaml --dump-config
```

## 設定例

### ホームスタジオ TX

```yaml
device:
  name: "studio-main"
  audio: "hw:0"

network:
  control_port: 8400
  rtp_base: 5004

audio:
  sample_rate: 48000
  channels: 2
  bit_depth: 24
  frames_per_packet: 48

metrics:
  enabled: true
  port: 9100
```

### リビングルーム RX（WiFi）

```yaml
device:
  name: "living-room"
  audio: "default"

audio:
  sample_rate: 48000
  channels: 2
  frames_per_packet: 96   # WiFi用に2ms
  buffer_packets: 12      # バッファリングを増加

logging:
  level: "warn"
```

### セキュアインストール

```yaml
device:
  name: "secure-endpoint"
  audio: "hw:0"

security:
  dtls_enabled: true
  auth_enabled: true
  certificate_path: "/etc/soluna/cert.pem"
  private_key_path: "/etc/soluna/key.pem"
  devices:
    - id: "control-room"
      psk: "sha256:..."
      roles: [admin]

audit:
  enabled: true
  file: "/var/log/soluna/audit.jsonl"

logging:
  level: "info"
  file: "/var/log/soluna/soluna.log"
  json_format: true
```
