# OpenSonic Transport Protocol (OSTP) Specification

**Version**: 1.0
**Status**: Draft
**Date**: 2026-03-12

## 1. Overview

OSTP is a real-time audio transport protocol built on RTP (RFC 3550). It supports:

- **Low-latency PCM streaming** over LAN (multicast) and WAN (relay)
- **Multi-device synchronized playback** via wall-clock timestamps
- **Hybrid P2P/server topology** — automatic mesh for small groups, relay for scale
- **Channel ownership and permissions** — owner controls who can broadcast
- **AES67 interoperability** for professional audio networks

### 1.1 Topology Model

```
┌─────────────────────────────────────────────────────────┐
│                    Connection Modes                       │
├──────────┬──────────────────────────────────────────────┤
│ Mode     │ Description                                   │
├──────────┼──────────────────────────────────────────────┤
│ LAN      │ UDP multicast 239.69.0.1:5004                │
│          │ Zero-config, all devices on same subnet       │
│          │ Best latency (<5ms)                           │
├──────────┼──────────────────────────────────────────────┤
│ P2P      │ Direct UDP between peers via NAT traversal    │
│          │ Uses PEER: hints from relay for hole-punching │
│          │ Ideal for 2-4 devices across networks         │
├──────────┼──────────────────────────────────────────────┤
│ Relay    │ Central UDP relay server (port 5100)          │
│          │ All audio routed through server               │
│          │ Required for 5+ devices or symmetric NAT      │
├──────────┼──────────────────────────────────────────────┤
│ Hybrid   │ P2P for nearby peers + relay for remote       │
│          │ MultipeerConnectivity (iOS/Mac) + WAN relay   │
│          │ Automatic fallback to relay if P2P fails      │
└──────────┴──────────────────────────────────────────────┘
```

## 2. OSTP Packet Format

### 2.1 Packet Layout

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
├─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┤
│V=2│P│X=1│ CC  │M│     PT      │       Sequence Number             │  RTP Header
├─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┤  (12 bytes)
│                         RTP Timestamp                             │
├─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┤
│                              SSRC                                 │
├─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┤
│  Profile = 0x4F53 ("OS")      │  Extension Length = 2             │  RTP Extension
├─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┤  Header (4 bytes)
│         Stream ID             │       Sequence Extension          │  OSTP Extension
├─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┤  (8 bytes)
│                       Media Timestamp                             │
├─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┤
│                                                                   │
│                     Audio Payload (variable)                      │  Payload
│                                                                   │
├─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┤
│                         CRC-32                                    │  Trailer (4 bytes)
└─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┘

Total header: 24 bytes  |  Max payload: 12,288 bytes  |  CRC: 4 bytes
Max packet: 12,316 bytes
```

All multi-byte fields are **network byte order** (big-endian).

### 2.2 Field Definitions

| Field | Bytes | Description |
|-------|-------|-------------|
| Version (V) | 2 bits | Always `2` (RTP v2) |
| Padding (P) | 1 bit | `0` |
| Extension (X) | 1 bit | Always `1` (OSTP extension present) |
| CSRC Count (CC) | 4 bits | `0` |
| Marker (M) | 1 bit | `0` (reserved) |
| Payload Type (PT) | 7 bits | See §2.3 |
| Sequence Number | 2 bytes | Lower 16 bits of 32-bit sequence |
| RTP Timestamp | 4 bytes | Sample count at 48kHz |
| SSRC | 4 bytes | Synchronization source identifier |
| Profile | 2 bytes | `0x4F53` ("OS") — identifies OSTP extension |
| Extension Length | 2 bytes | `2` (2 × 32-bit words = 8 bytes) |
| Stream ID | 2 bytes | Logical stream identifier |
| Sequence Extension | 2 bytes | Upper 16 bits of 32-bit sequence |
| Media Timestamp | 4 bytes | Wall-clock nanoseconds (lower 32 bits of epoch ns) |
| CRC-32 | 4 bytes | IEEE 802.3 over payload only |

### 2.3 Payload Types

| PT | Format | Sample Size | Description |
|----|--------|-------------|-------------|
| 96 | PCM S24 | 4 bytes (int32, 24-bit range) | Primary audio format |
| 97 | Float32 | 4 bytes (IEEE 754) | High-precision audio |
| 98 | Opus | variable | Compressed audio |
| 10 | AES67 L24 | 3 bytes (24-bit big-endian) | AES67 compatibility |
| 11 | AES67 L16 | 2 bytes (16-bit big-endian) | AES67 compatibility |
| 126 | NACK | variable | Retransmission request (WiFi) |
| 127 | FEC | variable | Forward error correction parity |

### 2.4 CRC-32 Algorithm

IEEE 802.3 polynomial `0xEDB88320` (reflected), computed over payload bytes only:

```c
uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}
```

### 2.5 Packet Timing Tiers

| Tier | Interval | Frames @ 48kHz | Use Case |
|------|----------|----------------|----------|
| Ultra | 125 µs | 6 | GbE studio |
| Low | 250 µs | 12 | GbE pro |
| Standard | 1 ms | 48 | GbE / 100M |
| WiFi | 2 ms | 96 | WiFi 5/6 |
| LAN | 5 ms | 240 | Home network |
| Robust | 10 ms | 480 | High jitter / WAN |

Default for relay/DJ mode: **Robust (480 frames, 10ms)**

## 3. Relay Signaling Protocol

UDP text-based commands on port **5100**. All commands are newline-terminated.

### 3.1 Connection

```
→ JOIN:<group>:<password>:<device_name>\n
← OK:joined\n
← ROLE:<owner|dj|listener>\n
← META:<json>\n              (if cached)
← FILE:<filename>\n          (if cached)
← SYNC:<action>:<pos>:<ts>\n (if cached)
← PEER:<ip>:<port>\n         (for each existing member)

→ HELLO\n                    (every 5s, keep-alive)

→ PING:<target_ip>:<target_port>:<timestamp_ms>\n
← (relay forwards to target as): PING:<sender_ip>:<sender_port>:<timestamp_ms>\n
← (target responds directly to sender): PONG:<timestamp_ms>\n

→ ROUTE:<peer_ip>:<peer_port>:<p2p|relay>\n   (informational, no relay response)
```

### 3.2 Channel Management

```
→ CHECK:<name>\n
← OK:available\n | OK:taken\n

→ CLAIM:<name>:<device>:<txn>\n
← OK:claimed\n | OK:renewed\n | ERR:taken\n | ERR:reserved_name\n

→ RELEASE:<name>:<device>\n
← OK:released\n
```

### 3.3 Role Management

```
→ GRANT:<dj|listener>:<device_name>\n    (owner only)
← OK:granted\n
   → target receives: ROLE:<dj|listener>\n

→ MEMBERS\n
← MEMBERS:{"group":"...","mode":"public|private","members":[{"device":"...","role":"...","addr":"..."}]}\n

→ MODE:<private|public>\n    (owner or DJ on free channels)
← MODE:<private|public>\n    (broadcast to all members)
```

Channel modes:

| Mode | Relay forwarding | Discovery listing | Copyright detection | Use case |
|------|-----------------|-------------------|-------------------|----------|
| `public` | Yes (default) | Yes | Yes | Public DJ sets, radio stations |
| `private` | Fallback only | No | No | Home audio, friends, practice |

- Owner/DJ can toggle at any time via `MODE:` command
- On mode change, all members receive the new `MODE:` notification
- Private mode triggers `PEER:` address exchange for P2P hole-punching
- LAN multicast and P2P direct connections never involve the relay, so copyright detection never applies regardless of mode setting

### 3.4 Broadcast Control (requires DJ or Owner role)

```
→ META:<json>\n           (broadcast metadata to group)
→ FILE:<filename>\n       (announce file for download)
→ SYNC:play:<pos_ms>:<wall_clock_ms>\n
→ SYNC:pause:<pos_ms>:<wall_clock_ms>\n
→ SYNC:seek:<pos_ms>:<wall_clock_ms>\n
→ <OSTP audio packet>    (binary, 0x80 prefix)
```

### 3.5 Playback Features

```
→ READY:<filename>\n      (receiver: file download complete)
→ REPLAY:<seconds>\n      (request timeshift, max 300s)
← OK:replay:<count>\n
→ RECORD:<group>\n        (start server-side recording)
← OK:recording\n
→ RECORD_STOP:<group>\n
← OK:record_stopped\n
```

### 3.6 Error Responses

| Error | Meaning |
|-------|---------|
| `ERR:empty_group` | No group name in JOIN |
| `ERR:channel_reserved` | Channel owned by another device |
| `ERR:wrong_password` | Incorrect group password |
| `ERR:group_full` | Max members reached (default: 16) |
| `ERR:max_groups` | Max groups reached (default: 100) |
| `ERR:no_permission` | Insufficient role for operation |
| `ERR:not_owner` | GRANT requires Owner role |
| `ERR:not_in_group` | Sender not in any group |
| `ERR:member_not_found` | Target device not in group |
| `ERR:invalid_format` | Malformed command |
| `ERR:invalid_name` | Channel name validation failed |
| `ERR:invalid_role` | Unknown role string |
| `ERR:taken` | Channel claimed by another device |
| `ERR:reserved_name` | Built-in reserved name |

## 4. Channel Ownership Model

### 4.1 Channel Types

| Type | Format | Cost | Permissions |
|------|--------|------|-------------|
| Random | 6-char hex (e.g. `a3f9b2`) | Free | Everyone = DJ |
| Built-in | `soluna`, `default`, `test`, `admin`, `music`, `jazz`, `tokyo`, etc. | Free | Everyone = DJ |
| Custom | 3-20 chars, alphanumeric + dash | Annual purchase | Owner controls |

### 4.2 Roles

| Role | Audio TX | META/FILE/SYNC | GRANT/REVOKE | Priority |
|------|----------|----------------|--------------|----------|
| Listener | — | — | — | 0 |
| DJ | ✓ | ✓ | — | 1 |
| Owner | ✓ | ✓ | ✓ | 2 |

### 4.3 Role Assignment Rules

- **Free/random channels**: all members join as DJ
- **Owned channels**: owner device gets Owner, first member gets DJ, others get Listener
- Owner can promote/demote via `GRANT:` command
- Role persists for session duration (re-evaluated on rejoin)

### 4.4 Channel Name Rules

- Length: 3-20 characters
- Allowed: `a-z`, `A-Z`, `0-9`, `-`
- All-hex names reserved for random generation
- Reserved premium names: see Appendix A
- Expiry: 365 days from CLAIM/renewal

## 5. Synchronized Playback

### 5.1 Media Timestamp

The `media_timestamp` field carries the lower 32 bits of the sender's wall-clock time in nanoseconds at the moment of audio capture/encode.

```
media_timestamp = (uint32_t)(nanoseconds_since_epoch & 0xFFFFFFFF)
```

### 5.2 Receiver Sync Algorithm

Each receiver computes ideal buffer depth from the media timestamp:

```
network_delay_ns = (now_ns_32bit - media_timestamp)  // wrapping subtraction
network_delay_ms = network_delay_ns / 1e6
ideal_buffer_ms  = target_delay_ms - network_delay_ms
ideal_buffer_ms  = clamp(ideal_buffer_ms, 10.0, 500.0)

// Smooth with EMA (alpha = 0.01)
target_fill += 0.01 * (ideal_frames - target_fill)
```

### 5.3 Unified Playback Parameters

These MUST be identical across all receiver implementations:

| Parameter | Value | Description |
|-----------|-------|-------------|
| Sample rate | 48000 Hz | Fixed |
| Target fill | 2880 frames (60 ms) | Default buffer target |
| Ring buffer | 192000 frames (4 s) | Capacity (power of 2) |
| Drift trigger | target × 3 | When to start discarding |
| Drift speed | frame_count / 80 + 1 | Frames to discard per callback |
| Drift crossfade | 48 frames | Smooth after discard |
| Fade in | 0.002 | Volume ramp-up coefficient |
| Fade out | 0.004 | Volume ramp-down coefficient |
| Sample scale | 8388608.0 (2²³) | S24 ↔ float conversion |

### 5.4 File-Sync (Hybrid DJ Mode)

DJ mode combines instant PCM streaming with file-based sync:

```
1. DJ sends FILE:<name>        → receivers start downloading
2. DJ streams PCM via OSTP     → receivers play immediately
3. After 3s: DJ sends SYNC:play:<pos_ms>:<wall_ms>
   → file-sync receivers switch to local file (perfect sync)
   → PCM receivers continue streaming (good enough sync)
```

## 6. Network Ports

| Port | Protocol | Purpose |
|------|----------|---------|
| 5004-5199 | UDP | RTP/OSTP audio (multicast or unicast) |
| 5100 | UDP | Relay signaling + audio forwarding |
| 5102 | TCP | Relay HTTP API |
| 319/320 | UDP | PTP clock sync (AES67) |
| 5353 | UDP | mDNS discovery |
| 8400 | TCP | Control plane |
| 8401 | UDP | Control plane |

### 6.1 Multicast Addresses

| Address | Purpose |
|---------|---------|
| 239.69.0.1 | Audio streams (LAN) |
| 224.0.1.129 | PTP clock (AES67) |
| 224.0.0.251 | mDNS |
| 224.2.127.254 | SAP announcements (AES67) |

## 7. Relay HTTP API

Base URL: `http://<relay>:5102`

### 7.1 Channel Endpoints

```
GET  /api/channel/check?name=<name>
     → {"available": true|false, "taken": true|false}

POST /api/channel/claim
     Body: {"name":"...","device":"...","session_id":"cs_..."}
     → {"status":"claimed"|"renewed","name":"..."}

POST /api/channel/claim-stripe
     Body: {"name":"...","device":"...","customer":"cus_...","subscription":"sub_..."}
     → {"status":"claimed"|"renewed","name":"..."}
```

### 7.2 Music Endpoints

```
GET  /api/music/         → {"files":["track.mp3",...]}
GET  /api/music/<file>   → binary audio file
```

### 7.3 Admin Endpoints

```
GET  /api/admin/channels?key=<admin_key>
     → {"channels":[...],"total":N,"active":N}

POST /api/admin/release
     Body: {"key":"...","name":"..."}
     → {"released":"..."}
```

### 7.4 Stripe Webhook

```
POST /api/stripe/wh-<secret>
     Events: customer.subscription.deleted, .paused, invoice.payment_failed
     → {"received":true}
```

## 8. WiFi Reliability

### 8.1 FEC (Forward Error Correction)

```
Mode: XOR parity (default) or Reed-Solomon
Group: 5 data packets + 1 parity (configurable)
PT: 127 for parity packets
Recovery: single packet loss per group (XOR), multi-loss (RS)
```

### 8.2 NACK (Negative Acknowledgment)

```
PT: 126
Purpose: Request retransmission of specific sequence numbers
Format: RTP header with PT=126, payload = list of missing sequence numbers
```

## 9. Connection Selection (P2P/Relay Auto-Select)

### 9.1 Algorithm

When a client joins a group, it receives `PEER:<ip>:<port>` for each existing member. The client then races P2P vs relay:

```
1. Send PING via relay (relay forwards to target)
2. Simultaneously send direct UDP to peer's IP:port
3. Peer responds with PONG via both paths
4. Client measures RTT for each path
5. Use the path with lower RTT
6. Fall back to relay if P2P PONG not received within 2s
```

### 9.2 Protocol Commands

```
→ PING:<target_addr>:<timestamp_ms>\n
  Relay forwards to target as: PING:<sender_addr>:<timestamp_ms>\n
← PONG:<timestamp_ms>\n  (target responds directly to sender)

→ ROUTE:<peer_addr>:p2p\n     (informational: using direct P2P)
→ ROUTE:<peer_addr>:relay\n   (informational: using relay)
```

### 9.3 Deduplication

Audio may arrive via both paths during transition. Receivers track a circular buffer of 64 recent sequence numbers and drop duplicates.

### 9.4 Keepalive

- P2P path: send direct PING every 10s
- If 3 consecutive PINGs get no PONG, fall back to relay
- Relay path always works as fallback

## 10. Text Channel (Lyrics / Chat / Karaoke)

### 10.1 TEXT Command

```
→ TEXT:<json>\n
  json: {
    "text": "歌詞テキスト",
    "type": "lyric|chat|info",
    "ts": 1234567890000,       // wall-clock ms (for sync display)
    "duration": 3000            // display duration in ms
  }
```

### 10.2 Text Types

| Type | Permission | Cached | Description |
|------|-----------|--------|-------------|
| `lyric` | DJ/Owner | Yes (`last_text`) | Timed karaoke lyrics |
| `chat` | All roles | No | User chat messages |
| `info` | DJ/Owner | No | System info/announcements |

### 10.3 Behavior

- `lyric` type: stored as `group.last_text`, sent to new joiners
- `chat` type: forwarded to group, not cached
- `ts` field enables receivers to display text at the correct wall-clock time (synced with audio)
- `duration` field: how long to display the text (0 = persistent until next TEXT)

## 11. DJ + Mic Mixing

### 11.1 Overview

DJs can talk over music by mixing microphone audio into the DJ stream. This is done **sender-side** — one mixed OSTP stream is sent, so receivers need no changes.

### 11.2 Mix Signal

```
→ MIX:on\n      (I'm sending mixed DJ+mic audio)
→ MIX:off\n     (I'm sending only one source)
← MIX:<device>:on\n   (relay notifies group)
← MIX:<device>:off\n  (relay notifies group)
```

### 11.3 Mixing Algorithm (Sender-side)

```
output = music_pcm * music_gain + mic_pcm * mic_gain
output = clamp(output, -1.0, 1.0)

Default gains:
  music_gain = 0.7  (auto-duck when mic active)
  mic_gain   = 1.0
```

### 11.4 Mix Status in MEMBERS

```json
{
  "group": "channel",
  "members": [
    {"device": "dj-mac", "role": "dj", "mixing": true, "addr": "1.2.3.4:5678"}
  ]
}
```

## 12. Relay Cascade (Planet-Scale)

OSTP supports planet-scale audio distribution via a multi-tier relay cascade.
A single DJ stream can reach billions of concurrent listeners.

### 12.1 Topology

```
                        DJ (source)
                           │
                    ┌──────┴──────┐
                    │   ORIGIN    │  1 instance
                    │   (tier 1)  │  Receives DJ audio
                    └──┬───┬───┬──┘
                       │   │   │
              ┌────────┘   │   └────────┐
              ▼            ▼            ▼
         ┌────────┐  ┌────────┐  ┌────────┐
         │REGION 1│  │REGION 2│  │REGION N│  ~20 instances
         │(tier 2)│  │(tier 2)│  │(tier 2)│  One per geographic region
         └─┬──┬──┬┘  └────────┘  └────────┘
           │  │  │
     ┌─────┘  │  └─────┐
     ▼        ▼        ▼
  ┌──────┐┌──────┐┌──────┐
  │EDGE 1││EDGE 2││EDGE M│  ~10K instances total
  │(tier3)││(tier3)││(tier3)│  ~500 per region
  └──┬───┘└──────┘└──────┘
     │
     ▼
  Listeners (up to 300K per edge)
```

### 12.2 Tier Roles

| Tier | Role | Fan-out | Typical Count |
|------|------|---------|---------------|
| Origin (1) | Receives DJ audio, distributes to regions | 1 → 20 | 1 |
| Region (2) | Geographic hub, distributes to local edges | 1 → 500 | ~20 |
| Edge (3) | Last mile, distributes to listeners | 1 → 300K | ~10K |
| Standalone | Single relay (backward compatible, no cascade) | 1 → N | — |

### 12.3 Cascade Protocol

Downstream relays subscribe to groups on their upstream using text commands on port 5100.

#### Subscribe

```
→ CASCADE_JOIN:<group>:<peer_id>:<secret>\n
← OK:cascade_joined\n
```

A downstream relay sends this when a local listener joins a group that requires upstream subscription. `peer_id` is a unique relay identifier (random 16-hex). `secret` is the shared cascade authentication token.

#### Keepalive

```
→ CASCADE_HELLO:<peer_id>\n
```

Sent every 5 seconds. Upstream removes stale downstream peers after 15 seconds without a heartbeat.

#### Unsubscribe

```
→ CASCADE_LEAVE:<group>:<peer_id>\n
← OK:cascade_left\n
```

#### Stats Report

```
→ CASCADE_STATS:<peer_id>:{"listeners":<N>,"groups":["<g1>","<g2>"],"bw_mbps":<BW>}\n
```

Sent every 30 seconds. Upstream aggregates for monitoring and load balancing.

### 12.4 Audio Forwarding

Audio packets flow **downstream only** — from origin → region → edge → listener.

- Upstream relays forward raw OSTP packets (same format as §2) to each subscribed downstream relay via UDP.
- Downstream relays receive these packets and forward to their own local members and further downstream.
- **No framing or encapsulation** — downstream relays are identified by their `sockaddr_in` (registered during CASCADE_JOIN).

DJ audio flows **upstream** to the origin:
- If a DJ connects to an edge or region relay, the relay forwards audio packets upstream to its parent.
- Origin receives and distributes downward through the cascade.
- Alternative: DJ is redirected to connect directly to the origin for minimum latency.

### 12.5 Metadata Propagation

Non-audio commands (META, FILE, SYNC, TEXT, MIX, GRANT) from the DJ are forwarded through the cascade:

1. DJ sends command to origin
2. Origin forwards to all subscribed region relays
3. Region relays forward to all subscribed edge relays
4. Edge relays forward to local listeners

Each relay caches the latest META/FILE/SYNC/TEXT for new joiners, same as standalone mode.

### 12.6 Capacity Planning

With Opus compression at 128 kbps (16 KB/s per listener):

| Scale | Origin BW | Regions | Edges | Edge BW (each) |
|-------|-----------|---------|-------|-----------------|
| 10K | 320 KB/s | 1 | 10 | 16 MB/s |
| 100K | 320 KB/s | 5 | 100 | 16 MB/s |
| 1M | 320 KB/s | 20 | 1K | 16 MB/s |
| 100M | 3.2 MB/s | 20 | 10K | 160 MB/s |
| 1B | 32 MB/s | 20 | 100K | 160 MB/s |
| 3B | 32 MB/s | 20 | 300K | 160 MB/s |

Origin bandwidth is always trivial (# regions × 16 KB/s per group).
Edge bandwidth scales with listeners per edge. At 160 MB/s (~1.3 Gbps), a commodity 10GbE server can handle 300K+ listeners.

### 12.7 sendmmsg() Optimization

On Linux, edges use `sendmmsg(2)` to batch up to 1024 `sendto()` calls into a single syscall, reducing kernel overhead by ~10x. Combined with worker threads (default 4), a single edge can saturate a 10 Gbps NIC.

```c
// Pseudocode — all iovecs share the same packet buffer
struct mmsghdr msgs[1024];
for (int i = 0; i < batch_size; i++) {
    msgs[i].msg_hdr.msg_name = &destinations[i];
    msgs[i].msg_hdr.msg_iov = &iov;  // shared packet
    msgs[i].msg_hdr.msg_iovlen = 1;
}
sendmmsg(sock, msgs, batch_size, 0);
```

### 12.8 Deployment Modes

```
# Standalone (existing behavior, no change)
soluna-relay --port 5100

# Origin
soluna-relay --origin --cascade-secret <SECRET> --port 5100

# Region (connects to origin)
soluna-relay --region <origin_ip:5100> --cascade-secret <SECRET> --port 5100

# Edge (connects to region)
soluna-relay --edge <region_ip:5100> --cascade-secret <SECRET> --port 5100 --workers 8
```

### 12.9 Failure Handling

| Failure | Behavior |
|---------|----------|
| Edge dies | Listeners disconnect; reconnect to another edge via DNS/anycast |
| Region dies | Edges detect stale upstream (no audio for 5s); reconnect to another region |
| Origin dies | All audio stops; DJ reconnects to backup origin (manual failover) |
| Network partition | Each partition operates independently; reunion causes brief dedup via sequence numbers |

### 12.10 Geographic Routing

Listeners are routed to their nearest edge relay via:

1. **Anycast DNS** — same hostname resolves to nearest edge
2. **Fly.io regions** — deploy edges in `nrt`, `iad`, `lhr`, `sin`, `gru`, etc.
3. **Redirect** — relay can respond `REDIRECT:<closer_relay_host:port>\n` to send listeners to a less-loaded or closer edge

### 12.11 Automatic Tier Selection

For small channels (< 100 listeners), the cascade is unnecessary overhead. The system auto-selects:

| Listeners | Topology |
|-----------|----------|
| 2-4 | P2P direct (§9) |
| 5-100 | Single relay (standalone) |
| 100-100K | Origin + edges (2 tiers) |
| 100K+ | Origin + regions + edges (3 tiers) |

Promotion from standalone to cascade happens when listener count exceeds the threshold. The origin is elected from the existing relay, and edges are spun up dynamically.

## 13. P2P Swarm Distribution

Every listener can act as a micro-relay, forwarding audio to other listeners. This creates a peer-to-peer tree that scales to billions of listeners with near-zero server bandwidth cost.

### 13.1 Motivation

| Approach | Server bandwidth for 1B listeners | Cost |
|----------|-----------------------------------|------|
| Server relay | 1B × 16 KB/s = 16 PB/s | ~$billions/month |
| Cascade (§12) | ~100K edges × 16 KB/s = 1.6 GB/s | ~$millions/month |
| P2P Swarm | Fan-out × 16 KB/s = 64 KB/s | ~$0/month |

Audio's small bitrate (128 kbps with Opus) makes P2P practical — even mobile devices can relay to 4 peers.

### 13.2 Topology

```
           DJ (source)
            │
      ┌─────┼─────┐
      ▼     ▼     ▼
      A     B     C     ← depth 1 (root nodes, receive from relay)
     /|\   /|\   /|\
    D E F G H I J K L   ← depth 2 (receive from A/B/C, forward to children)
   /|\ ...              ← depth 3 ...
```

Fan-out of 4:
- Depth 1: 4 nodes
- Depth 5: 1,024 nodes
- Depth 10: ~1 million nodes
- Depth 16: ~4.2 billion nodes

Maximum latency at depth 16: 16 × 20ms = 320ms (acceptable for audio streaming).

### 13.3 Activation

The relay server automatically activates P2P swarm when a group exceeds a threshold (default: 50 members). Below this threshold, normal relay forwarding is used.

| Group Size | Mode |
|------------|------|
| 2-4 | P2P direct (§9) |
| 5-50 | Relay forwarding |
| 50+ | P2P Swarm |

### 13.4 Protocol Commands

#### Client → Relay

```
→ SWARM_READY\n
    Client indicates it can relay audio to other peers.
    Sent after JOIN. Clients behind symmetric NAT or on
    battery-saver mode should NOT send this.

→ SWARM_UNABLE\n
    Client cannot relay (symmetric NAT, mobile data saver,
    low battery, etc.). Will always be a leaf node.

→ SWARM_ACK:<parent_ip>:<parent_port>\n
    Client confirms it successfully receives audio from
    its assigned parent peer.

→ SWARM_LOST:<parent_ip>:<parent_port>\n
    Client lost audio from its parent (timeout, disconnect).
    Relay temporarily forwards audio directly and finds a
    new parent within 2 seconds.
```

#### Relay → Client

```
← SWARM_ASSIGN:<parent_ip>:<parent_port>:<child1_ip>:<child1_port>[:<childN_ip>:<childN_port>]\n
    Full assignment: receive audio from <parent>, forward to listed children.
    Parent 0.0.0.0:0 means receive directly from relay (root node).

← SWARM_ADD_CHILD:<child_ip>:<child_port>\n
    Add a new child to forward audio to.

← SWARM_REMOVE_CHILD:<child_ip>:<child_port>\n
    Stop forwarding to this child (it left or was reassigned).

← SWARM_PROMOTE:<new_parent_ip>:<new_parent_port>\n
    Your parent changed. Start receiving audio from the new parent.
    Sent when the old parent disconnected or was rebalanced.
```

### 13.5 Tree Construction Algorithm

The relay maintains a breadth-first tree per group:

1. **Root selection**: The DJ/Owner node is always root (depth 0). It receives audio from the relay and forwards to its children.
2. **BFS assignment**: New members are attached as children of the shallowest node that has fewer than `fan_out` (4) children and is relay-capable.
3. **Leaf-only nodes**: Members that sent `SWARM_UNABLE` are always placed as leaves (never assigned children).
4. **Incremental updates**: Individual joins/leaves don't rebuild the tree — only the affected branch is modified.

### 13.6 Failure Recovery

| Event | Recovery | Time |
|-------|----------|------|
| Leaf disconnects | Parent notified via SWARM_REMOVE_CHILD | Instant |
| Mid-node disconnects | Orphaned children sent SWARM_PROMOTE to a sibling or re-attached to another parent | < 2s |
| Root disconnects | Next relay-capable member promoted to root; relay sends audio directly during transition | < 2s |
| Client sends SWARM_LOST | Relay sends audio directly as fallback + finds new parent | < 2s |

During recovery (before new parent is assigned), the relay temporarily forwards audio to orphaned nodes directly, ensuring no audio gap longer than one packet interval (10ms).

### 13.7 Client Implementation

Each client in swarm mode:

1. **Receives** one OSTP audio stream from its assigned parent (or relay if root)
2. **Plays** the audio locally
3. **Forwards** the same raw OSTP packet to each assigned child via UDP `sendto()`
4. **Monitors** parent health — if no audio for 500ms, sends `SWARM_LOST` to relay

The forwarding is a simple memcpy — no decode/re-encode needed. Client CPU cost is negligible.

```
receive_packet(parent) → play_audio(packet) → for each child: sendto(child, packet)
```

### 13.8 NAT Traversal

P2P swarm requires peers to send UDP to each other. NAT traversal approach:

1. **Relay-mediated hole-punch**: When assigning parent-child, relay sends both addresses. Both peers send initial packets to each other through their NATs.
2. **STUN fallback**: If hole-punch fails within 1s, client sends `SWARM_UNABLE` and becomes leaf-only.
3. **Relay fallback**: If a client can't receive from any P2P peer, relay forwards directly (this client adds load but others compensate).

### 13.9 Hybrid: Cascade + Swarm

For maximum scale, combine cascade (§12) with swarm:

```
DJ → Origin → Region → Edge → [P2P Swarm of listeners]
```

Each edge relay coordinates a local swarm. This means:
- Edge bandwidth: only root nodes (4 per group) instead of all listeners
- 10K edges × 4 root nodes = 40K server-side connections for 3 billion listeners

### 13.10 Bandwidth Budget

| Role | Upload needed | Typical device |
|------|--------------|----------------|
| Leaf (receive only) | 0 | Any device |
| Relay node (fan-out 4) | 4 × 128 kbps = 512 kbps | WiFi phone, any broadband |
| Root node (fan-out 4) | 512 kbps | Good broadband |

512 kbps upload is available on virtually any internet connection (4G, WiFi, DSL).
Clients on metered connections or low battery can opt out with SWARM_UNABLE.

## 14. P2P File Distribution

When the DJ plays music files, the file itself can be distributed to listeners via P2P, eliminating real-time streaming entirely. Listeners play from their local copy, synchronized via SYNC commands (§5).

### 14.1 Why This Matters

| Scenario | Bandwidth per listener | 1M listeners |
|----------|----------------------|--------------|
| Real-time streaming (Opus 128kbps) | 16 KB/s constant | 16 TB/s |
| P2P swarm streaming (§13) | ~0 server, 64 KB/s peer upload | ~0 server |
| P2P file distribution + local play | One-time download, then 0 | ~0 total |

For a typical 4-minute song (5 MB with Opus), P2P file distribution completes in seconds. After that, SYNC:play triggers synchronized local playback with **zero ongoing bandwidth**.

Real-time streaming is only needed for:
- Live microphone talk (intermittent, typically <10% of DJ session)
- Content without a pre-encoded file

### 14.2 Distribution Methods

#### Method A: File URL (fastest, preferred)

```
→ FILE_URL:<url>:<sha256>:<size_bytes>\n
← (relay broadcasts to all members)
← FILE_URL:<url>:<sha256>:<size_bytes>\n
```

DJ provides a direct download URL (CDN, cloud storage, pre-shared). Each listener downloads independently. Works instantly for well-known music hosted on existing infrastructure.

#### Method B: P2P Chunk Distribution (no external server)

```
→ FILE_OFFER:<filename>:<size_bytes>:<sha256>:<num_chunks>\n
← FILE_MANIFEST:<filename>:<size_bytes>:<sha256>:<num_chunks>\n  (to all)
```

File is split into 64 KB chunks. DJ seeds initial chunks to first-wave peers, who then re-seed to others (BitTorrent-style exponential distribution).

### 14.3 P2P Chunk Protocol

#### Chunk Size

Fixed **64 KB** (65,536 bytes). A 5 MB file = 80 chunks.

#### Chunk Tracking

```
→ FILE_HAVE:<sha256>:<bitfield_hex>\n
    Hex-encoded bitfield of available chunks.
    Example: "ff0f" = chunks 0-11 available, 12-15 missing.

→ FILE_COMPLETE:<sha256>\n
    Client has all chunks, is now a full seeder.
```

#### Peer Discovery

```
← FILE_PEERS:<sha256>:<chunk_index>:<peer1_ip>:<peer1_port>[:<peer2_ip>:<peer2_port>...]\n
    Relay tells client which peers have a specific chunk.
    Client can request from any listed peer.
```

#### Peer-to-Peer Chunk Exchange

```
→ CHUNK_REQ:<sha256>:<chunk_index>\n    (to peer)
← CHUNK_DATA:<sha256>:<chunk_index>:<data_base64>\n    (from peer)
```

Chunks are base64-encoded for safe UDP transport. 64 KB → 88 KB encoded.

### 14.4 Distribution Strategy

The relay coordinates efficient "rarest-first" distribution:

1. **First wave**: DJ sends different chunks to different peers (stripe distribution)
   - Peer A gets chunks 0, 4, 8, ...
   - Peer B gets chunks 1, 5, 9, ...
   - Peer C gets chunks 2, 6, 10, ...
   - Peer D gets chunks 3, 7, 11, ...

2. **Cross-seeding**: First-wave peers trade chunks with each other
   - After ~1 second, all 4 have the complete file

3. **Exponential fan-out**: Each complete seeder seeds to 4 new peers
   - 4 → 16 → 64 → 256 → 1,024 → ...
   - 1 million peers in ~10 seconds
   - 1 billion peers in ~15 seconds

4. **URL acceleration**: If FILE_URL is also provided, peers download missing chunks via HTTP in parallel with P2P, whichever is faster.

### 14.5 Playback Flow

```
1. DJ: FILE_OFFER (or FILE_URL)     → P2P distribution begins
2. DJ: streams real-time audio       → listeners hear immediately
3. Background: file distributes P2P  → ~5-15 seconds for full propagation
4. DJ: SYNC:play:<position>:<ts>     → listeners switch to local file
5. Real-time stream stops            → bandwidth drops to zero
6. DJ talks into mic                 → brief real-time stream burst
7. DJ: next song FILE_OFFER          → repeat cycle
```

During step 2-3, listeners hear the song via real-time streaming. Once the file is distributed (step 4), they seamlessly switch to local playback. This hybrid approach ensures zero audio gap.

### 14.6 Completion Broadcast

When all chunks of a file are available on the relay (DJ seeding complete or enough peers seeding), the relay broadcasts:

```
← FILE_READY:<sha256>\n
```

This tells all members that the file can now be fully obtained via P2P peers or the relay. Clients that already have the file (via `FILE_COMPLETE`) ignore this.

### 14.8 File Verification

- **SHA-256 hash** provided in FILE_OFFER/FILE_URL for integrity
- Each chunk can be verified against partial hash (Merkle tree optional)
- Corrupted chunks are re-requested from different peers

### 14.9 Storage Management

Clients cache downloaded files with LRU eviction:
- Default cache: 500 MB (configurable)
- Files are identified by SHA-256 (deduplication)
- If a file is already cached from a previous session, FILE_COMPLETE is sent immediately

### 14.10 Combined Savings

For a 2-hour DJ set (30 songs, 150 MB total):

| Method | DJ upload | Server bandwidth | Listener download |
|--------|-----------|-----------------|-------------------|
| Real-time streaming | 150 MB (streamed) | 150 MB × N | 150 MB (streamed) |
| P2P file + sync | 150 MB (chunked) | ~0 | 150 MB (P2P) |
| FILE_URL + sync | ~0 (URL only) | ~0 | 150 MB (from CDN) |

With FILE_URL: **DJ uploads nothing, server transfers nothing, listeners download from CDN**.
With P2P chunks: **server transfers nothing**, only peer-to-peer traffic.

Real-time streaming is only needed for live mic talk (~5-10 minutes of a 2-hour set).

## 15. Copyright Detection & Royalty Tracking

OSTP includes built-in copyright detection and royalty tracking to ensure rights holders are compensated when their music is played. The system is designed for **zero audio performance impact** — detection runs asynchronously.

### 15.1 Design Principles

1. **Audio quality first** — fingerprinting never blocks or degrades the audio pipeline
2. **Relay-only** — detection runs only on relay servers. LAN multicast and P2P direct connections are not monitored. Your home, your rules.
3. **Transparent** — DJs and listeners see what's being played and who owns it
4. **Fair compensation** — royalties are tracked per-play, per-listener, per-minute
5. **Open database** — supports multiple fingerprint backends (ACRCloud, AudibleMagic, Chromaprint/AcoustID, or self-hosted)

**Scope:**

| Connection Mode | Copyright Detection | Reason |
|----------------|-------------------|--------|
| LAN multicast (§1.1) | No | Audio never touches relay |
| P2P direct (§9) | No | Audio never touches relay |
| Relay (§3) | Yes | Audio passes through relay server |
| Cascade (§12) | Yes (origin only) | Origin relay inspects audio once |
| P2P Swarm (§13) | Yes (before fan-out) | Relay sees audio from DJ before distributing to root nodes |

### 15.2 Detection Architecture

```
forward_audio()                          Fingerprint Thread
     │                                        │
     ├─ forward to members (unchanged)        │
     ├─ try_lock fingerprint buffer           │
     │   ├─ success: copy 480 samples         │
     │   └─ fail: skip (no wait)              │
     │                                        │
     │                              every 5 seconds:
     │                              ├─ read 5s of audio from ring buffer
     │                              ├─ compute spectral fingerprint
     │                              ├─ match against database
     │                              └─ if match → COPYRIGHT_DETECT
```

The fingerprint buffer is a lock-free ring (try_lock, never blocking). If the lock is held by the fingerprint thread, the audio packet is simply skipped — this happens at most once per 5 seconds and has zero impact on forwarding latency.

### 15.3 Protocol Commands

#### Relay → DJ

```
← COPYRIGHT_DETECT:<json>\n
    {
      "track": "Song Title",
      "artist": "Artist Name",
      "album": "Album Name",
      "isrc": "US-XXX-YY-NNNNN",
      "rights_holder": "Record Label Inc.",
      "confidence": 0.95,
      "action": "warn",
      "royalty_per_listener": "0.003"
    }

    Sent when a copyrighted track is detected.
    Action: "warn" (notify), "charge" (auto-deduct), "block" (stop playback).

← COPYRIGHT_WARN:<message>\n
    Human-readable warning text.

← COPYRIGHT_CHARGE:<amount_cents>:<currency>:<isrc>\n
    DJ is charged for the play. Amount = rate × listeners × duration.
```

#### DJ → Relay

```
→ COPYRIGHT_ACK\n
    DJ acknowledges the copyright detection and agrees to royalty charges.

→ COPYRIGHT_SKIP\n
    DJ will skip to the next track to minimize charges.
```

#### Relay → All Members

```
← COPYRIGHT_INFO:<json>\n
    {
      "track": "Song Title",
      "artist": "Artist Name",
      "album": "Album Name",
      "rights_holder": "Record Label Inc.",
      "now_playing": true
    }

    Informational "Now Playing" with proper attribution.
    Displayed to all listeners. NOT a warning — just credit.
```

### 15.4 Audio Fingerprinting

The fingerprint algorithm:

1. **Downsample**: Stereo S24 (48kHz) → Mono S16 (48kHz) by averaging L+R and truncating
2. **Window**: 5 seconds of audio (240,000 samples)
3. **Spectral analysis**: Divide into 32 windows, compute energy in 8 frequency bands (200Hz–25.6kHz)
4. **Hash**: For each window, compare band energies with previous window → 1 bit per increase → 64-bit fingerprint hash

This is a simplified version of Chromaprint. For production accuracy, the relay can be configured to use an external API:

```
--fingerprint-api https://api.acrcloud.com/v1/identify
--fingerprint-api-key <KEY>
```

External APIs provide >95% accuracy against catalogs of 100M+ tracks.

### 15.5 Royalty Calculation

Royalties use **tiered per-listener-per-minute pricing** with volume discounts (see §16.8 for full rate table).

```
charge_per_minute = calculate_royalty_per_min(listener_count)

Tiered rates:
  1–100 listeners:       $0.001 / person / min
  101–1,000:             $0.0005
  1,001–10,000:          $0.0002
  10,001–100,000:        $0.0001
  100,001+:              $0.00005
```

Examples (per minute):
| Listeners | Rate/min | 4-min song |
|-----------|----------|------------|
| 10 | $0.010 | $0.040 |
| 1,000 | $0.550 | $2.200 |
| 100,000 | $6.000 | $24.000 |
| 1,000,000 | $56.000 | $224.000 |

Plays shorter than 10 seconds are not counted (skip/preview).

### 15.6 Royalty Ledger

All royalty events are logged to a JSONL file (`/data/royalties.jsonl`):

```json
{"ts":1741737600,"group":"channel-name","dj":"DJ-Device","isrc":"US-XXX-YY-NNNNN","track":"Song Title","artist":"Artist","rights_holder":"Label Inc","duration_sec":240,"listeners":1500,"royalty_usd":13.5000}
```

This ledger is the source of truth for:
- Monthly royalty reports to rights holders
- DJ billing and invoicing
- Regulatory compliance (mechanical licenses, performance rights)

### 15.7 Revenue Distribution

```
┌────────────────────────────────────────┐
│  Royalty collected per play             │
│                                        │
│  70% → Rights holder (artist/label)    │
│  10% → Platform (Soluna)               │
│  20% → DJ (incentive for proper use)   │
└────────────────────────────────────────┘
```

The 70/10/20 split maximizes DJ incentives while maintaining industry-standard rights holder share.

### 15.8 Enforcement Levels

Configurable per channel:

| Level | Behavior |
|-------|----------|
| `monitor` | Detect and log only. No warnings. Default for free channels. |
| `warn` | Send COPYRIGHT_DETECT to DJ. Log royalties. |
| `charge` | Auto-charge DJ's account. Require COPYRIGHT_ACK or skip. |
| `block` | Stop audio forwarding after 30 seconds if DJ doesn't acknowledge. |

### 15.9 Integration with FILE_URL (§14)

When a DJ uses `FILE_URL` to distribute music:
- The URL itself may identify the track (e.g., Spotify/Apple Music link)
- No audio fingerprinting needed — metadata is already known
- Royalty tracking is immediate and 100% accurate

This is the preferred path: `FILE_URL` provides instant detection + proper credits.

### 15.10 Privacy

- Fingerprints are one-way hashes — original audio cannot be reconstructed
- Only aggregate play counts and royalty totals are shared with rights holders
- Individual listener identities are never exposed
- DJs see their own royalty charges in real-time

## 16. Billing, Wallets & Royalty Payouts

OSTP includes a built-in economic layer for fair compensation of music rights holders.

### 16.1 Business Model

```
┌──────────────────────────────────────────────────────────┐
│  DJ (broadcaster) → pre-charges wallet → plays music     │
│    ├─ Public channel + copyrighted music → royalty charge │
│    │    ├─ 70% → Rights holder (artist/label)            │
│    │    ├─ 20% → Platform (Soluna)                       │
│    │    └─ 10% → DJ cashback                             │
│    ├─ Private channel → no charge (no copyright check)   │
│    └─ LAN / P2P direct → no charge (no relay involved)   │
│                                                          │
│  Listeners → always free (no ads, no subscription)       │
│                                                          │
│  Tips → 100% to DJ (listeners can tip voluntarily)       │
└──────────────────────────────────────────────────────────┘
```

### 16.2 Why DJ Pays (Not Listeners)

| Model | Problem |
|-------|---------|
| Listener subscription | Kills growth. Audio should be free. |
| Ads | Destroys audio quality. Against Soluna's principles. |
| Flat DJ subscription | Unfair to small DJs. Big channels subsidize small ones. |
| **DJ pay-per-play** | **Fair. Proportional to usage. Listeners free.** |

DJs who play original/CC music or use private mode pay nothing.

### 16.3 Wallet Protocol

#### Query Balance

```
→ WALLET\n
← WALLET:{"balance":12.50,"total_charged":100.00,"total_earned":5.00,"total_royalties":82.50}\n
```

#### Add Funds

```
→ CHARGE:<amount_usd>:<payment_token>\n
← OK:charged:{"balance":112.50,"amount":100.00}\n
← ERR:payment_failed\n
```

`payment_token` is a reference from the payment gateway (Stripe, etc.). The relay verifies via webhook before crediting.

#### Withdraw Funds

```
→ WITHDRAW:<amount_usd>\n
← OK:withdrawn:{"amount":50.00,"remaining":62.50}\n
← ERR:insufficient_balance\n
← ERR:no_payout_account\n
```

Available to DJs (cashback earnings, tips) and rights holders (royalty earnings). Withdrawal is processed to the registered payout account.

#### Setup Payout Account

```
→ PAYOUT_SETUP:<stripe|paypal>:<account_id>\n
← OK:payout_setup\n
```

#### Tips

```
→ TIP:<amount_usd>\n
← OK:tipped:{"amount":1.00,"to":"DJ-Device"}\n
    DJ receives: TIP_RECEIVED:{"amount":1.00,"from":"Listener-Device"}\n
← ERR:insufficient_balance\n
```

Tips go 100% to the DJ. No platform cut. Listeners need a wallet with balance to tip.

#### Support (Fund DJ's Royalty Costs)

```
→ SUPPORT:<amount_usd>\n
← OK:supported:{"amount":5.00,"to":"DJ-Device","balance":45.00}\n
    All members receive: SUPPORT_RECEIVED:{"amount":5.00,"from":"Supporter","dj":"DJ","dj_balance":50.00}\n
← ERR:insufficient_balance\n
```

Listeners directly fund the DJ's wallet to cover royalty costs. Unlike tips (private), SUPPORT is broadcast to all members — everyone sees who's helping keep the music playing.

#### DJ Balance Broadcast

```
← BALANCE_UPDATE:{"dj":"DJ-Device","balance":12.50,"rate_per_min":0.15,"listeners":50,"track":"Song","artist":"Artist"}\n
```

Sent to ALL members every 60 seconds during copyrighted playback. Listeners see the DJ's balance draining in real-time, encouraging SUPPORT contributions.

#### Licensed Play (One-Click, No Detection)

```
→ LICENSED_PLAY:{"track":"Song","artist":"Artist","isrc":"XX-XXX-YY-NNNNN","license":"jasrac"}\n
← OK:licensed_play\n
    All members receive: COPYRIGHT_INFO:{"track":"Song","artist":"Artist",...}\n
```

DJ declares they hold a valid license (JASRAC, CC, blanket, original). Copyright detection is suppressed for this track. Logged for audit but no royalty charge. Enables one-click play of licensed content.

#### Transaction History

```
→ TRANSACTIONS:<count>\n
← TRANSACTIONS:[{"ts":1741737600,"type":"royalty","amount":0.45,"track":"Song","balance":12.05},...]
```

### 16.4 Real-Time Royalty Billing

Royalties are deducted in real-time, not at the end of a song:

1. Copyright detection identifies a track (§15)
2. Every 60 seconds during playback, the relay calculates using tiered pricing:
   ```
   charge = calculate_royalty_per_min(listener_count) × 1 minute
   ```
3. DJ's wallet is debited; rights holder, platform, and DJ cashback are credited
4. DJ receives `COPYRIGHT_CHARGE` with amount and remaining balance
5. On song end, final partial-minute is settled

This ensures rights holders see revenue in real-time.

### 16.5 Insufficient Balance Handling

| DJ Balance | Behavior |
|-----------|----------|
| > $1.00 | Normal operation, periodic charges |
| $0.01–$1.00 | `COPYRIGHT_WARN` with low balance alert |
| $0.00 | 5-minute grace period with warnings |
| $0.00 + 5 min | Auto-switch to private mode (`MODE:private`) |

The DJ can switch back to public by adding funds (`CHARGE`). Music never stops — only the channel mode changes.

### 16.6 Rights Holder Dashboard

Rights holders accumulate earnings per ISRC track:

```
→ RIGHTS_BALANCE:<isrc_or_name>\n
← RIGHTS_BALANCE:{"rights_holder":"Label Inc","pending":1250.00,"total_earned":15000.00,"tracks":[...]}\n
```

Withdrawals are available anytime via `WITHDRAW` with a registered payout account.

> **Note**: `RIGHTS_BALANCE` is planned for a future release. Currently, rights holder earnings are tracked in the royalty ledger (`/data/royalties.jsonl`) and can be queried offline.

### 16.7 Transaction Ledger

All financial events are logged to `/data/transactions.jsonl`:

```json
{"ts":1741737600,"tx_id":"tx_abc123","from":"DJ-Device","to":"rights:US-XXX-YY-NNNNN","amount":0.45,"type":"royalty","desc":"Song Title by Artist"}
{"ts":1741737600,"tx_id":"tx_abc124","from":"DJ-Device","to":"platform","amount":0.06,"type":"platform_fee","desc":"10% platform fee"}
{"ts":1741737600,"tx_id":"tx_abc125","from":"platform","to":"DJ-Device","amount":0.13,"type":"cashback","desc":"20% DJ cashback"}
{"ts":1741737660,"tx_id":"tx_abc126","from":"Listener1","to":"DJ-Device","amount":1.00,"type":"tip","desc":"Tip"}
```

This ledger is:
- **Append-only** — never modified after write
- **Real-time** — written on every financial event
- **Auditable** — complete history for regulatory compliance

### 16.8 Tiered Pricing

Per-listener rate decreases with audience size (volume discount), aligned with industry standards (SoundExchange, Spotify).

| Listeners | Rate/person/min | Equivalent per-stream |
|-----------|----------------|-----------------------|
| 1–100 | $0.001 | ~$0.004 (Spotify-level) |
| 101–1,000 | $0.0005 | ~$0.002 |
| 1,001–10,000 | $0.0002 | ~$0.0008 |
| 10,001–100,000 | $0.0001 | ~$0.0004 |
| 100,001+ | $0.00005 | ~$0.0002 |

#### Pricing Examples

| Scenario | Listeners | Duration | DJ Cost | Rights Holder (70%) |
|----------|-----------|----------|---------|-------------------|
| Small home party | 5 | 3 min | $0.015 | $0.011 |
| Bar DJ set | 50 | 4 hours | $12.00 | $8.40 |
| Online event | 1,000 | 2 hours | $72.00 | $50.40 |
| Festival stream | 100,000 | 1 hour | $360.00 | $252.00 |
| Major live | 1,000,000 | 1 hour | $1,560.00 | $1,092.00 |
| Global event | 30,000,000 | 1 hour | $3,060.00 | $2,142.00 |

Industry comparison:
- Bar DJ set (50人×4h): **$12** vs JASRAC月額 ¥2,000-6,000 (~$13-40) — 同水準
- SoundExchange webcasting rate: ~$0.0021/performance/listener — Soluna Tier 1-2 と同等

### 16.9 Free Scenarios (No Charge)

- **Private mode** → no copyright detection → no charge
- **LAN multicast** → no relay → no charge
- **P2P direct** → no relay → no charge
- **Original/CC music** → no copyright match → no charge
- **FILE_URL with licensed content** → royalty handled by the URL source (Spotify, etc.)

### 16.10 Pre-Seeded DJ Wallets

The relay ships with pre-created wallets for well-known DJs, each funded with $670 (~¥100,000). This allows famous DJs to start broadcasting immediately without setup. Pre-seeded accounts include:

- **DJ Yuki** (admin account)
- International DJs: Calvin Harris, Tiesto, David Guetta, Marshmello, Skrillex, Deadmau5, etc.
- Japanese DJs: Taku Takahashi, Ken Ishii, DJ Krush, etc.
- Total: ~50 accounts

Pre-seeded wallets are initialized on first relay startup and persisted to the wallets database.

## Appendix A: Built-in Free Channel Names

The following channel names are **free to use** by anyone (everyone gets DJ role) but **cannot be purchased** (CLAIM returns `ERR:reserved_name`).

**System**: soluna, default, test, admin, system, relay, server, opensonic, api, help, support, status, debug

**Common**: music, audio, home, live, studio, party, zen, bass, beat, jazz, rock, pop, mix, dj, room, cafe, bar, club, lounge, lobby, office, work, lab, gym, spa, pool, kitchen, bedroom, garden, garage, patio, upstairs, downstairs, main, master, guest

**Regional**: sakura, fuji, tokyo, kyoto, osaka, nara

## Appendix B: Platform Implementation Matrix

| Feature | iOS | Mac | Linux | Windows | ESP32 |
|---------|-----|-----|-------|---------|-------|
| OSTP RX | ✓ | ✓ | ✓ | ✓ | ✓ |
| OSTP TX (Mic) | ✓ | ✓ | — | — | — |
| OSTP TX (DJ) | ✓ | ✓ | ✓ | — | — |
| OSTP TX (System Audio) | — | ✓ | — | — | — |
| Multicast LAN | ✓ | ✓ | ✓ | ✓ | ✓ |
| WAN Relay | ✓ | ✓ | ✓ | ✓ | ✓ |
| P2P (MultipeerConnectivity) | ✓ | ✓ | — | — | — |
| Sync Mode | ✓ | ✓ | ✓ | ✓ | — |
| File-Sync | ✓ | ✓ | PCM only | PCM only | PCM only |
| 3-Band EQ | ✓ | ✓ | — | — | — |
| Compressor | ✓ | ✓ | — | — | — |
| Loudness Norm | ✓ | ✓ | — | — | — |
| Recording | ✓ | ✓ | ✓ | ✓ | — |
| Spatial Audio | ✓ | — | — | — | — |
| AES67 Compat | — | ✓ | ✓ | — | — |
| Cascade Origin | — | — | ✓ | ✓ | — |
| Cascade Region | — | — | ✓ | ✓ | — |
| Cascade Edge | — | — | ✓ | ✓ | — |
| sendmmsg() Batching | — | — | ✓ | — | — |
| P2P Swarm Relay | ✓ | ✓ | ✓ | ✓ | — |
| SWARM_READY/UNABLE | ✓ | ✓ | ✓ | ✓ | — |
| P2P File Distribution | ✓ | ✓ | ✓ | ✓ | — |
| FILE_URL Download | ✓ | ✓ | ✓ | ✓ | ✓ |
| Copyright Detection | ✓ | ✓ | ✓ | ✓ | — |
| Royalty Tracking | ✓ | ✓ | ✓ | ✓ | — |
| Wallet/Billing | ✓ | ✓ | ✓ | ✓ | — |
