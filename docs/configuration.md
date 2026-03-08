# Configuration Reference

Soluna uses YAML configuration files. The default location is `/etc/soluna/config.yaml`.

## Quick Start

Minimal configuration for receiver (sync mode, default):
```yaml
device:
  name: "living-room"
  audio: "default"

audio:
  sample_rate: 48000
  channels: 2
```

Minimal configuration for transmitter:
```yaml
device:
  name: "studio-main"
  audio: "hw:0"

network:
  rtp_base: 5004
```

Low-latency jam session:
```yaml
mode: jam

device:
  name: "jam-room"
  audio: "hw:0"
```

## Complete Reference

### mode

Stream mode determines the latency vs synchronization tradeoff.

```yaml
mode: sync           # "sync" or "jam" (default: "sync")
```

| Value | Description |
|-------|-------------|
| `sync` | **Sync mode** (default). PTP-aligned multi-room playback. All devices play audio in perfect synchronization. Best for whole-home audio. |
| `jam` | **Jam mode**. Ultra-low-latency (~20ms end-to-end). Skips PTP alignment and minimizes buffering. Best for real-time jam sessions and live collaboration. |

This value can also be set via:
- **CLI flag**: `solunad --mode sync` or `solunad --mode jam` (overrides YAML)
- **WebSocket**: `{"command":"mode.set","mode":"jam"}` (runtime switch, no restart needed)
- **Web UI**: Stream Mode picker in the control panel at `http://<device-ip>:8400/`

### device

Device identification and hardware settings.

```yaml
device:
  name: "my-device"        # Device name for discovery (default: "soluna-device")
  audio: "default"         # ALSA device name (Linux), CoreAudio ID (macOS)
  interface: ""            # Network interface (empty = auto-detect)
```

**Audio device names:**
- Linux: `default`, `hw:0`, `hw:1`, `plughw:0,0`, `hw:sndrpihifiberry`
- macOS: `default`, or device UID from `solunad --list-devices`
- Windows: `default`, or device name from `solunad --list-devices`

### network

Network configuration for streaming and control.

```yaml
network:
  control_port: 8400       # REST API/WebSocket port (default: 8400)
  rtp_base: 5004           # Base RTP port (default: 5004)
  multicast_audio: "239.69.0.1"   # Audio multicast address
  multicast_ptp: "224.0.1.129"    # PTP multicast address
  dscp: 46                 # DSCP marking (46 = EF, 34 = AF41)
```

**DSCP values:**
| Value | Class | Use Case |
|-------|-------|----------|
| 46 | EF | Real-time audio (recommended) |
| 34 | AF41 | Multimedia streaming |
| 0 | BE | Best effort (default) |

### audio

Audio stream parameters.

```yaml
audio:
  sample_rate: 48000       # Sample rate in Hz (default: 48000)
  channels: 2              # Number of channels (1-64, default: 2)
  bit_depth: 24            # Bit depth (16, 24, 32, default: 24)
  frames_per_packet: 48    # Samples per RTP packet (default: 48 = 1ms)
  buffer_packets: 8        # Ring buffer size in packets (default: 8)
```

**Latency calculation:**
```
Latency (ms) = frames_per_packet / sample_rate * 1000 * buffer_packets
Example: 48 / 48000 * 1000 * 8 = 8ms total buffer
```

**Recommended settings:**
| Network | frames_per_packet | buffer_packets | Latency |
|---------|-------------------|----------------|---------|
| Wired LAN | 48 | 4 | 4ms |
| WiFi 5GHz | 96 | 8 | 16ms |
| WiFi 2.4GHz | 144 | 12 | 36ms |

### security

Authentication and encryption settings.

```yaml
security:
  dtls_enabled: false      # Enable DTLS encryption
  auth_enabled: false      # Enable device authentication

  # TLS certificates (for DTLS)
  certificate_path: "/etc/soluna/cert.pem"
  private_key_path: "/etc/soluna/key.pem"

  # Device credentials (for auth)
  devices:
    - id: "studio-console"
      psk: "sha256:abc123..."    # SHA-256 hash of PSK
      roles: [admin]

  # Role definitions
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

**Generating PSK hash:**
```bash
echo -n "my-secret-key" | sha256sum | cut -d' ' -f1
```

### metrics

Prometheus metrics exporter.

```yaml
metrics:
  enabled: true            # Enable metrics endpoint
  port: 9100               # Metrics HTTP port
  path: "/metrics"         # Endpoint path
  scrape_interval_ms: 5000 # Update interval
```

**Available metrics:**
```
soluna_audio_frames_processed_total
soluna_rtp_packets_sent_total
soluna_rtp_packets_received_total
soluna_rtp_packets_lost_total
soluna_ptp_offset_ns
soluna_ptp_synced
soluna_uptime_seconds
soluna_active_streams
soluna_buffer_underruns_total
soluna_buffer_overruns_total
```

### logging

Log output configuration.

```yaml
logging:
  level: "info"            # Log level: debug, info, warn, error
  file: ""                 # Log file path (empty = stdout)
  json_format: false       # JSON log format
  include_timestamp: true  # Include timestamps
```

### audit

Security audit logging.

```yaml
audit:
  enabled: true
  file: "/var/log/soluna/audit.jsonl"
  events:                  # Events to log (empty = all)
    - auth_success
    - auth_failure
    - stream_created
    - stream_destroyed
    - config_changed
```

**Audit log format (JSON Lines):**
```json
{"ts":1706443200000000000,"event":"auth_success","actor":"studio-console","ip":"192.168.1.100"}
{"ts":1706443201000000000,"event":"stream_created","actor":"studio-console","stream_id":1}
```

### routing

Auto-routing rules for automatic stream setup.

```yaml
routing:
  auto_rules:
    - name: "connect-speakers"
      trigger:
        type: device_connected    # device_connected, device_disconnected
        pattern: "esp32-speaker-*"  # Glob pattern
      actions:
        - type: add_route
          source: "studio-main:0"
          sink: "$device:0"       # $device = matched device
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

**Trigger types:**
- `device_connected` - Device appears on network
- `device_disconnected` - Device leaves network
- `stream_started` - Stream begins
- `stream_stopped` - Stream ends

**Action types:**
- `add_route` - Create audio route
- `remove_route` - Delete audio route
- `set_gain` - Adjust route gain

### plugins

DSP plugins to load.

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

## Environment Variables

Configuration supports environment variable expansion:

```yaml
device:
  name: "${HOSTNAME}"
  audio: "${SOLUNA_AUDIO_DEVICE:-default}"

security:
  psk: "${SOLUNA_PSK}"
```

Syntax:
- `${VAR}` - Use environment variable
- `${VAR:-default}` - Use default if not set

## Configuration Paths

Soluna searches for config in order:
1. Command line: `--config /path/to/config.yaml`
2. `./config.yaml` (current directory)
3. `~/.config/soluna/config.yaml`
4. `/etc/soluna/config.yaml`

## Validating Configuration

```bash
# Check config syntax
solunad --config /etc/soluna/config.yaml --validate

# Show effective configuration
solunad --config /etc/soluna/config.yaml --dump-config
```

## Example Configurations

### Home Studio TX

```yaml
mode: sync               # Multi-room synchronized playback

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

### Living Room RX (WiFi)

```yaml
mode: sync               # Sync with other rooms

device:
  name: "living-room"
  audio: "default"

audio:
  sample_rate: 48000
  channels: 2
  frames_per_packet: 96   # 2ms for WiFi
  buffer_packets: 12      # More buffering

logging:
  level: "warn"
```

### Jam Session (Low-Latency)

```yaml
mode: jam                # Ultra-low-latency (~20ms) for live collaboration

device:
  name: "jam-room"
  audio: "hw:0"

audio:
  sample_rate: 48000
  channels: 2
  bit_depth: 24
  frames_per_packet: 48
  buffer_packets: 4      # Minimal buffering for jam mode
```

### Secure Installation

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
