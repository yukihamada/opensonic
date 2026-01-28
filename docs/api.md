# API Reference

Soluna provides a REST API and WebSocket interface for control and monitoring.

## Base URL

```
http://<host>:8400/api/v1
```

Default port is 8400, configurable via `network.control_port`.

## Authentication

When `security.auth_enabled` is true, include the API key in headers:

```
Authorization: Bearer <api-key>
```

Or use Basic Auth:
```
Authorization: Basic <base64(device_id:psk)>
```

## REST API Endpoints

### System

#### GET /status

Get system status summary.

**Response:**
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

Get version information.

**Response:**
```json
{
  "version": "0.1.0",
  "build": "2024-01-15",
  "features": ["opus", "dtls", "aes67"]
}
```

### Devices

#### GET /devices

List all discovered devices.

**Response:**
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

Get specific device details.

**Response:**
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

### Streams

#### GET /streams

List all active streams.

**Response:**
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

Create a new stream.

**Request:**
```json
{
  "source": "studio-main",
  "sink": "living-room",
  "channels": 2
}
```

**Response:**
```json
{
  "stream_id": 1,
  "port": 5004
}
```

#### DELETE /streams/{id}

Destroy a stream.

**Response:**
```json
{
  "success": true
}
```

### Routes

#### GET /routes

List all audio routes.

**Response:**
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

Add a new route.

**Request:**
```json
{
  "source": "studio-main:0",
  "sink": "living-room:0",
  "gain_db": 0.0
}
```

**Response:**
```json
{
  "success": true
}
```

#### DELETE /routes

Remove a route.

**Request:**
```json
{
  "source": "studio-main:0",
  "sink": "living-room:0"
}
```

#### PATCH /routes

Modify a route.

**Request:**
```json
{
  "source": "studio-main:0",
  "sink": "living-room:0",
  "gain_db": -3.0,
  "muted": false
}
```

### Meters

#### GET /meters/{channel}

Get audio level meters.

**Response:**
```json
{
  "channel": "studio-main:0",
  "peak_db": -12.3,
  "rms_db": -18.5,
  "clip_count": 0
}
```

#### GET /meters

Get all meters.

**Response:**
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

### Configuration

#### GET /config

Get current configuration.

**Response:**
```yaml
device:
  name: "studio-main"
  audio: "hw:0"
...
```

#### PUT /config

Update configuration (requires restart).

**Request:**
```yaml
device:
  name: "studio-main-v2"
```

#### POST /config/reload

Reload configuration from file.

## WebSocket API

Connect to `ws://<host>:8400/ws` for real-time updates.

### Message Format

```json
{
  "type": "subscribe|unsubscribe|command|event",
  "topic": "meters|devices|streams|routes",
  "data": {}
}
```

### Subscribe to Meters

```json
{"type": "subscribe", "topic": "meters"}
```

Receives updates:
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

### Subscribe to Device Events

```json
{"type": "subscribe", "topic": "devices"}
```

Receives events:
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

### Send Commands

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

Response:
```json
{
  "type": "response",
  "success": true,
  "id": "cmd-123"
}
```

## CLI Commands

The `solctl` CLI uses the REST API internally.

```bash
# List devices
solctl devices

# List streams
solctl streams

# Create stream
solctl stream create --source studio-main --sink living-room --channels 2

# Destroy stream
solctl stream destroy 1

# List routes
solctl routes

# Add route
solctl route add --source studio-main:0 --sink living-room:0 --gain -6

# Remove route
solctl route remove --source studio-main:0 --sink living-room:0

# Set gain
solctl route gain --source studio-main:0 --sink living-room:0 --db -3

# Mute
solctl route mute --source studio-main:0 --sink living-room:0

# Get meters
solctl meters

# Get status
solctl status

# Connect to specific host
solctl --host 192.168.1.100 --port 8400 devices
```

## Error Responses

All endpoints return errors in this format:

```json
{
  "success": false,
  "error": "stream not found",
  "code": 404
}
```

Common error codes:
| Code | Meaning |
|------|---------|
| 400 | Bad request (invalid parameters) |
| 401 | Unauthorized (auth required) |
| 403 | Forbidden (insufficient permissions) |
| 404 | Not found |
| 409 | Conflict (resource already exists) |
| 500 | Internal server error |

## Rate Limiting

Default limits:
- REST API: 100 requests/second per client
- WebSocket: 50 messages/second per connection
- Meter updates: 20 Hz maximum

## Examples

### Python

```python
import requests

BASE = "http://192.168.1.100:8400/api/v1"

# List devices
devices = requests.get(f"{BASE}/devices").json()
print(devices)

# Create stream
resp = requests.post(f"{BASE}/streams", json={
    "source": "studio-main",
    "sink": "living-room",
    "channels": 2
})
stream_id = resp.json()["stream_id"]

# Add route
requests.post(f"{BASE}/routes", json={
    "source": "studio-main:0",
    "sink": "living-room:0",
    "gain_db": -6.0
})
```

### JavaScript

```javascript
const BASE = 'http://192.168.1.100:8400/api/v1';

// WebSocket for real-time meters
const ws = new WebSocket('ws://192.168.1.100:8400/ws');
ws.onopen = () => {
  ws.send(JSON.stringify({type: 'subscribe', topic: 'meters'}));
};
ws.onmessage = (event) => {
  const data = JSON.parse(event.data);
  console.log('Meters:', data.data);
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
# Get status
curl http://localhost:8400/api/v1/status

# List devices
curl http://localhost:8400/api/v1/devices

# Create stream
curl -X POST http://localhost:8400/api/v1/streams \
  -H "Content-Type: application/json" \
  -d '{"source":"studio-main","sink":"living-room","channels":2}'

# Add route with auth
curl -X POST http://localhost:8400/api/v1/routes \
  -H "Authorization: Bearer my-api-key" \
  -H "Content-Type: application/json" \
  -d '{"source":"studio-main:0","sink":"living-room:0","gain_db":-6}'
```
