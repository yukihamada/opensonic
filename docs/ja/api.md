# APIリファレンス

Solunaは制御と監視用のREST APIとWebSocketインターフェースを提供します。

## ベースURL

```
http://<host>:8400/api/v1
```

デフォルトポートは8400で、`network.control_port`で設定可能です。

## 認証

`security.auth_enabled`がtrueの場合、ヘッダーにAPIキーを含めます：

```
Authorization: Bearer <api-key>
```

またはBasic認証を使用：
```
Authorization: Basic <base64(device_id:psk)>
```

## REST APIエンドポイント

### システム

#### GET /status

システムステータスの概要を取得。

**レスポンス:**
```json
{
  "version": "0.1.0",
  "uptime_seconds": 3600,
  "devices": 5,
  "streams": 2,
  "routes": 4,
  "ptp_synced": true,
  "ptp_offset_ns": 1234
}
```

#### GET /version

バージョン情報を取得。

**レスポンス:**
```json
{
  "version": "0.1.0",
  "build": "2024-01-15",
  "features": ["opus", "dtls", "aes67"]
}
```

### デバイス

#### GET /devices

発見されたすべてのデバイスを一覧表示。

**レスポンス:**
```json
[
  {
    "id": "ab12cd34",
    "name": "studio-main",
    "host": "192.168.1.100",
    "inputs": 2,
    "outputs": 2,
    "local": true,
    "sample_rate": 48000,
    "ptp_synced": true
  },
  {
    "id": "ef56gh78",
    "name": "living-room",
    "host": "192.168.1.101",
    "inputs": 0,
    "outputs": 2,
    "local": false,
    "sample_rate": 48000,
    "ptp_synced": true
  }
]
```

#### GET /devices/{id}

特定のデバイスの詳細を取得。

**レスポンス:**
```json
{
  "id": "ab12cd34",
  "name": "studio-main",
  "host": "192.168.1.100",
  "inputs": 2,
  "outputs": 2,
  "local": true,
  "sample_rate": 48000,
  "channels": [
    {"index": 0, "name": "Left", "type": "input"},
    {"index": 1, "name": "Right", "type": "input"},
    {"index": 0, "name": "Left", "type": "output"},
    {"index": 1, "name": "Right", "type": "output"}
  ],
  "stats": {
    "packets_tx": 123456,
    "packets_rx": 0,
    "packets_lost": 0,
    "underruns": 0,
    "overruns": 0
  }
}
```

### ストリーム

#### GET /streams

すべてのアクティブなストリームを一覧表示。

**レスポンス:**
```json
[
  {
    "id": 1,
    "source": "studio-main",
    "sink": "living-room",
    "channels": 2,
    "port": 5004,
    "state": "active",
    "stats": {
      "packets_sent": 123456,
      "packets_lost": 0,
      "latency_ms": 4.2
    }
  }
]
```

#### POST /streams

新しいストリームを作成。

**リクエスト:**
```json
{
  "source": "studio-main",
  "sink": "living-room",
  "channels": 2
}
```

**レスポンス:**
```json
{
  "stream_id": 1,
  "port": 5004
}
```

#### DELETE /streams/{id}

ストリームを削除。

**レスポンス:**
```json
{
  "success": true
}
```

### ルート

#### GET /routes

すべてのオーディオルートを一覧表示。

**レスポンス:**
```json
[
  {
    "source": "studio-main:0",
    "sink": "living-room:0",
    "gain_db": -6.0,
    "muted": false
  },
  {
    "source": "studio-main:1",
    "sink": "living-room:1",
    "gain_db": -6.0,
    "muted": false
  }
]
```

#### POST /routes

新しいルートを追加。

**リクエスト:**
```json
{
  "source": "studio-main:0",
  "sink": "living-room:0",
  "gain_db": 0.0
}
```

**レスポンス:**
```json
{
  "success": true
}
```

#### DELETE /routes

ルートを削除。

**リクエスト:**
```json
{
  "source": "studio-main:0",
  "sink": "living-room:0"
}
```

#### PATCH /routes

ルートを変更。

**リクエスト:**
```json
{
  "source": "studio-main:0",
  "sink": "living-room:0",
  "gain_db": -3.0,
  "muted": false
}
```

### メーター

#### GET /meters/{channel}

オーディオレベルメーターを取得。

**レスポンス:**
```json
{
  "channel": "studio-main:0",
  "peak_db": -12.3,
  "rms_db": -18.5,
  "clip_count": 0
}
```

#### GET /meters

すべてのメーターを取得。

**レスポンス:**
```json
[
  {
    "channel": "studio-main:0",
    "peak_db": -12.3,
    "rms_db": -18.5,
    "clip_count": 0
  },
  {
    "channel": "studio-main:1",
    "peak_db": -14.1,
    "rms_db": -20.2,
    "clip_count": 0
  }
]
```

### 設定

#### GET /config

現在の設定を取得。

**レスポンス:**
```yaml
device:
  name: "studio-main"
  audio: "hw:0"
...
```

#### PUT /config

設定を更新（再起動が必要）。

**リクエスト:**
```yaml
device:
  name: "studio-main-v2"
```

#### POST /config/reload

ファイルから設定を再読み込み。

## WebSocket API

リアルタイム更新のため`ws://<host>:8400/ws`に接続。

### メッセージ形式

```json
{
  "type": "subscribe|unsubscribe|command|event",
  "topic": "meters|devices|streams|routes",
  "data": {}
}
```

### メーターを購読

```json
{"type": "subscribe", "topic": "meters"}
```

更新を受信:
```json
{
  "type": "event",
  "topic": "meters",
  "data": {
    "studio-main:0": {"peak_db": -12.3, "rms_db": -18.5},
    "studio-main:1": {"peak_db": -14.1, "rms_db": -20.2}
  }
}
```

### デバイスイベントを購読

```json
{"type": "subscribe", "topic": "devices"}
```

イベントを受信:
```json
{
  "type": "event",
  "topic": "devices",
  "event": "connected",
  "data": {
    "id": "new-device",
    "name": "New Device",
    "host": "192.168.1.105"
  }
}
```

### コマンドを送信

```json
{
  "type": "command",
  "command": "route_set_gain",
  "params": {
    "source": "studio-main:0",
    "sink": "living-room:0",
    "gain_db": -6.0
  }
}
```

レスポンス:
```json
{
  "type": "response",
  "success": true,
  "id": "cmd-123"
}
```

## CLIコマンド

`solctl` CLIは内部でREST APIを使用します。

```bash
# デバイス一覧
solctl devices

# ストリーム一覧
solctl streams

# ストリーム作成
solctl stream create --source studio-main --sink living-room --channels 2

# ストリーム削除
solctl stream destroy 1

# ルート一覧
solctl routes

# ルート追加
solctl route add --source studio-main:0 --sink living-room:0 --gain -6

# ルート削除
solctl route remove --source studio-main:0 --sink living-room:0

# ゲイン設定
solctl route gain --source studio-main:0 --sink living-room:0 --db -3

# ミュート
solctl route mute --source studio-main:0 --sink living-room:0

# メーター取得
solctl meters

# ステータス取得
solctl status

# 特定のホストに接続
solctl --host 192.168.1.100 --port 8400 devices
```

## エラーレスポンス

すべてのエンドポイントはこの形式でエラーを返します：

```json
{
  "success": false,
  "error": "stream not found",
  "code": 404
}
```

一般的なエラーコード:
| コード | 意味 |
|--------|------|
| 400 | 不正なリクエスト（無効なパラメータ）|
| 401 | 認証エラー（認証が必要）|
| 403 | 禁止（権限不足）|
| 404 | 見つからない |
| 409 | 競合（リソースが既に存在）|
| 500 | 内部サーバーエラー |

## レート制限

デフォルト制限:
- REST API: クライアントあたり100リクエスト/秒
- WebSocket: 接続あたり50メッセージ/秒
- メーター更新: 最大20 Hz

## 例

### Python

```python
import requests

BASE = "http://192.168.1.100:8400/api/v1"

# デバイス一覧
devices = requests.get(f"{BASE}/devices").json()
print(devices)

# ストリーム作成
resp = requests.post(f"{BASE}/streams", json={
    "source": "studio-main",
    "sink": "living-room",
    "channels": 2
})
stream_id = resp.json()["stream_id"]

# ルート追加
requests.post(f"{BASE}/routes", json={
    "source": "studio-main:0",
    "sink": "living-room:0",
    "gain_db": -6.0
})
```

### JavaScript

```javascript
const BASE = 'http://192.168.1.100:8400/api/v1';

// リアルタイムメーター用WebSocket
const ws = new WebSocket('ws://192.168.1.100:8400/ws');
ws.onopen = () => {
  ws.send(JSON.stringify({type: 'subscribe', topic: 'meters'}));
};
ws.onmessage = (event) => {
  const data = JSON.parse(event.data);
  console.log('メーター:', data.data);
};

// REST API
async function createStream() {
  const resp = await fetch(`${BASE}/streams`, {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({
      source: 'studio-main',
      sink: 'living-room',
      channels: 2
    })
  });
  return resp.json();
}
```

### curl

```bash
# ステータス取得
curl http://localhost:8400/api/v1/status

# デバイス一覧
curl http://localhost:8400/api/v1/devices

# ストリーム作成
curl -X POST http://localhost:8400/api/v1/streams \
  -H "Content-Type: application/json" \
  -d '{"source":"studio-main","sink":"living-room","channels":2}'

# 認証付きでルート追加
curl -X POST http://localhost:8400/api/v1/routes \
  -H "Authorization: Bearer my-api-key" \
  -H "Content-Type: application/json" \
  -d '{"source":"studio-main:0","sink":"living-room:0","gain_db":-6}'
```
