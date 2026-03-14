




Network Working Group                                         Open Sonic
Request for Comments: OSTP-1                                  Workgroup
Category: Experimental                                     Version: 0.9
                                                           2026-03-14

              Open Sonic Transport Protocol (OSTP)
              =====================================
         A Swarm-Native Audio Streaming Protocol with
              Protocol-Level Economic Incentives

                        DRAFT SPECIFICATION

Status of This Document

   This document specifies an experimental protocol. Distribution of
   this document is unlimited.

Copyright Notice

   Copyright (C) 2026 Open Sonic Workgroup. All rights reserved.

---

## Abstract

   The Open Sonic Transport Protocol (OSTP) is a UDP-based audio
   transport protocol designed for large-scale, one-to-many live audio
   distribution. OSTP extends RTP [RFC 3550] with three orthogonal
   innovations: (1) a swarm distribution layer that allows listeners to
   optionally serve as relay nodes, reducing infrastructure cost at
   scale; (2) a protocol-level economic layer supporting micropayment
   charges, royalty distribution, and copyright fingerprinting without
   an application-layer intermediary; and (3) a simple channel-name
   addressing scheme compatible with both LAN multicast and WAN peer-
   to-peer operation.

   OSTP is NOT a replacement for WebRTC or AES67. It is complementary:
   WebRTC targets small-group bidirectional conferencing; AES67 targets
   professional LAN studio routing. OSTP targets the specific problem
   of distributing high-quality audio to thousands of simultaneous
   listeners over the public Internet with economic accountability.

---

## Table of Contents

   1.  Introduction
       1.1.  Motivation and Problem Statement
       1.2.  Why Not WebRTC
       1.3.  Why Not AES67
       1.4.  Design Goals
   2.  Terminology
   3.  Architecture Overview
   4.  Packet Format
       4.1.  OSTP Packet Structure
       4.2.  RTP Base Header (12 bytes)
       4.3.  RTP Extension Header (4 bytes)
       4.4.  OSTP Extension Block (8 bytes)
       4.5.  Audio Payload
       4.6.  CRC-32 Trailer (4 bytes)
       4.7.  Complete Packet Diagram
       4.8.  Payload Type Assignments
   5.  Channel Addressing
       5.1.  Channel Name Format
       5.2.  Channel Resolution
       5.3.  Wildcard and Hierarchical Channels
   6.  Connection Modes
       6.1.  Mode Selection Algorithm
       6.2.  LAN Multicast Mode
       6.3.  P2P Mode (STUN/TURN)
       6.4.  Relay Mode
       6.5.  Mode Transitions
   7.  Swarm Distribution Protocol
       7.1.  Overview and Rationale
       7.2.  Node Roles
       7.3.  Swarm Activation Threshold
       7.4.  Tree Construction
       7.5.  Signaling Messages
       7.6.  Node State Machine
       7.7.  Parent Failover
       7.8.  Mobile Device Handling
       7.9.  Capacity Advertisement
       7.10. Swarm Teardown
   8.  Economic Layer
       8.1.  Wallet Model
       8.2.  Wallet Commands
       8.3.  CHARGE Command Security
       8.4.  Copyright Fingerprinting
       8.5.  Royalty Distribution
       8.6.  Economic Message Framing
   9.  Audio Codecs
       9.1.  Mandatory Codec: Opus
       9.2.  Optional Codec: FLAC
       9.3.  Codec Negotiation
   10. Congestion Control and Quality Adaptation
       10.1. Receiver Reports
       10.2. Bitrate Ladder
       10.3. FEC and Packet Recovery
   11. Security Considerations
       11.1. DTLS-SRTP
       11.2. Session Tokens
       11.3. Rate Limiting
       11.4. Economic Security
       11.5. Fingerprint Privacy
   12. IANA Considerations
   13. References
   14. Authors' Addresses

---

## 1. Introduction

### 1.1. Motivation and Problem Statement

   Live audio streaming at scale presents a persistent infrastructure
   challenge. A single source broadcasting to 10,000 simultaneous
   listeners must maintain 10,000 × bitrate_kbps of outbound bandwidth.
   At 128 kbps Opus stereo, that is 1.28 Gbps from a single origin —
   prohibitively expensive for independent artists and small venues.

   CDN-based HTTP Live Streaming (HLS) and DASH address this at the
   cost of latency: HLS segments introduce 3–30 seconds of delay,
   making real-time DJ performance monitoring, synchronized light shows,
   and low-latency venue audio infeasible.

   OSTP addresses both problems simultaneously:
   - Swarm distribution shifts relay bandwidth to willing listener nodes,
     reducing origin egress by up to 70–80% at large scale.
   - RTP-based UDP transport achieves sub-100ms end-to-end latency from
     source to leaf node under normal network conditions.
   - The economic layer makes listener-relay contribution measurable and
     compensable at protocol level, enabling sustainable incentive
     models without external settlement systems.

### 1.2. Why Not WebRTC

   WebRTC [RFC 8835] is an excellent protocol for its target use case:
   small-group, bidirectional, browser-to-browser communication. It is
   not designed for one-to-many distribution and has fundamental
   architectural limitations for that use case:

   | Property                | WebRTC              | OSTP                    |
   |-------------------------|---------------------|-------------------------|
   | Primary topology        | Mesh / SFU          | Swarm tree              |
   | Max practical receivers | ~50 via SFU         | Unlimited (fanout)      |
   | Economic layer          | None                | Protocol-native         |
   | Channel addressing      | SDP offer/answer    | Name-based, 1 string    |
   | Swarm formation         | No                  | Yes (fanout-4 tree)     |
   | Mobile relay inhibit    | No                  | Yes (auto leaf assign)  |
   | Copyright tracking      | No                  | Built-in fingerprint    |
   | LAN multicast           | No                  | Yes (224.0.0.0/4)       |

   WebRTC's SDP negotiation round-trip is also heavyweight for the
   join-stream use case. OSTP uses a single JOIN datagram and begins
   receiving audio within 1 RTT.

   WebRTC's ICE/DTLS setup typically requires 200–500ms before audio
   flows. OSTP's relay mode can deliver first audio packet in < 50ms.

### 1.3. Why Not AES67

   AES67 [AES67-2015] is the professional audio industry's LAN
   interoperability standard, built on RTP with PTP (IEEE 1588)
   synchronization. It is excellent for studio environments and is not
   designed for:

   - WAN operation (relies on LAN multicast, QoS DSCP marking)
   - P2P traversal (no STUN/TURN/ICE)
   - Swarm distribution
   - Economic accounting
   - Consumer/mobile device support

   OSTP's packet format is RTP-compatible and shares AES67's choice of
   RTP as the base transport, but adds the extension header profile
   0x4F53 ("OS") to carry OSTP-specific metadata. An AES67 receiver
   SHOULD silently drop OSTP extension blocks it does not recognize.

### 1.4. Design Goals

   G1. First-packet latency < 100ms (relay mode), < 5ms (LAN multicast)
   G2. Support 1 source → 100,000+ receivers via swarm
   G3. Royalty metering without external API calls
   G4. Single-string channel addressing ("soluna/stage-a")
   G5. Browser-native decoding (Opus + WebCodecs API)
   G6. Graceful degradation: relay mode works with no swarm support
   G7. Mobile-first: cellular detection inhibits relay duty
   G8. Privacy: fingerprinting uses distributed consensus, no central DB

---

## 2. Terminology

   The key words "MUST", "MUST NOT", "REQUIRED", "SHALL", "SHALL NOT",
   "SHOULD", "SHOULD NOT", "RECOMMENDED", "MAY", and "OPTIONAL" in this
   document are to be interpreted as described in [RFC 2119].

   Source
      The node originating the audio stream. There is exactly one
      Source per channel. The Source transmits to one or more top-level
      Relay Nodes or directly to Leaf Nodes if the group is small.

   Relay Node
      A listener node that has volunteered to forward received audio
      packets to downstream children. A Relay Node has both upstream
      parents and downstream children in the swarm tree.

   Leaf Node
      A listener node that only receives audio and does not forward to
      any children. Leaf Nodes are the terminal consumers of the stream.

   Swarm Coordinator (SC)
      A server component that maintains the swarm topology, assigns
      parent-child relationships, and handles node join/leave events.
      The SC SHOULD be deployed with low latency to the Source.

   Channel
      A named audio stream identified by a UTF-8 string of at most 64
      characters. A channel has exactly one active Source at a time.

   SSRC
      Synchronization Source identifier, as defined in [RFC 3550].
      The Source uses a fixed SSRC per session. Relay Nodes preserve
      the Source's SSRC when forwarding.

   stream_id
      A 16-bit identifier assigned by the Source at session start.
      Disambiguates multiple sessions on the same channel (e.g.,
      after a source restart).

   seq_ext
      A 16-bit sequence number extension. Combined with the 16-bit RTP
      sequence number, provides a 32-bit extended sequence counter
      allowing up to 2^32 packets without wraparound ambiguity.

   media_timestamp
      A 32-bit media clock timestamp relative to session start,
      in units of 1/48000 seconds (for 48 kHz Opus).

   Fingerprint Hash
      A 64-bit hash derived from a 30-second audio window, used for
      distributed copyright detection.

   HMAC
      Hash-based Message Authentication Code as defined in [RFC 2104],
      used with SHA-256 for CHARGE command authentication.

---

## 3. Architecture Overview

```
   ┌─────────┐     OSTP/UDP      ┌──────────────────────┐
   │ Source  │──────────────────▶│  Swarm Coordinator   │
   │ (DJ)    │                   │  (wss://relay.solun)  │
   └────┬────┘                   └──────────┬───────────┘
        │ OSTP packets                      │ SWARM_* signaling
        │ (UDP multicast or unicast)         │ (WebSocket JSON)
        ▼                                   ▼
   ┌─────────────────────────────────────────────────────┐
   │                    RELAY NODES                       │
   │                                                      │
   │  ┌──────────┐    ┌──────────┐    ┌──────────┐       │
   │  │ Relay-A  │    │ Relay-B  │    │ Relay-C  │       │
   │  │ (fanout) │    │ (fanout) │    │ (fanout) │       │
   │  └────┬─────┘    └────┬─────┘    └────┬─────┘       │
   └───────┼──────────────┼───────────────┼─────────────┘
           │              │               │
           ▼              ▼               ▼
   ┌─────────────────────────────────────────────────────┐
   │                    LEAF NODES                        │
   │                                                      │
   │  Leaf  Leaf  Leaf  Leaf  Leaf  Leaf  Leaf  Leaf      │
   │  (browser, mobile, desktop receivers)                │
   └─────────────────────────────────────────────────────┘
```

   The Swarm Coordinator is responsible only for signaling and topology;
   it does NOT proxy audio data. Audio flows directly between nodes via
   OSTP/UDP. This separation of control plane (WebSocket) and data
   plane (UDP) is central to OSTP's scalability.

   The Swarm Coordinator MUST be horizontally scalable. Multiple SC
   instances MAY be deployed; they share topology state via an external
   store (e.g., Redis pub/sub). A single SC can reasonably handle 50,000
   concurrent channel members.

---

## 4. Packet Format

### 4.1. OSTP Packet Structure

   An OSTP packet consists of four consecutive byte sequences:

   ```
   [ RTP Base Header ] [ RTP Extension Header ] [ OSTP Extension Block ]
   [ Audio Payload ] [ CRC-32 Trailer ]
   ```

   Total minimum overhead (no payload): 12 + 4 + 8 + 4 = 28 bytes.
   With a 20ms Opus frame at 128 kbps: payload ≈ 320 bytes.
   Total packet ≈ 348 bytes, well within the 1200-byte safe MTU for UDP.

### 4.2. RTP Base Header (12 bytes)

   ```
    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |V=2|P|X=1|  CC   |M|     PT    |       sequence number         |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                           timestamp                           |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |           synchronization source (SSRC) identifier           |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   ```

   Fields:
   - V (2 bits): RTP version, MUST be 2.
   - P (1 bit): Padding. Set to 0 unless padding bytes are appended.
   - X (1 bit): Extension present. MUST be 1 for OSTP packets.
   - CC (4 bits): Contributing source count. Set to 0.
   - M (1 bit): Marker. Set to 1 on the first packet of a new audio
     talk-spurt or after a silence period > 200ms.
   - PT (7 bits): Payload type. See Section 4.8.
   - Sequence number (16 bits): Monotonically increasing per SSRC.
     Wraps from 65535 to 0. See seq_ext for 32-bit space.
   - Timestamp (32 bits): RTP media clock timestamp. For Opus at 48 kHz
     with 20ms frames, increments by 960 per packet.
   - SSRC (32 bits): Random value chosen by the Source at session start.
     Relay Nodes MUST preserve the Source's SSRC.

### 4.3. RTP Extension Header (4 bytes)

   OSTP uses the RTP header extension mechanism [RFC 3550 §5.3.1]:

   ```
    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |   profile = 0x4F53 ("OS")    |    extension length = 2       |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   ```

   - profile (16 bits): Fixed value 0x4F53 (ASCII "OS" for Open Sonic).
     Receivers that do not recognize this profile SHOULD drop the packet.
   - extension length (16 bits): Number of 32-bit words in the OSTP
     Extension Block that follows. Fixed value 2 (= 8 bytes / 4).

### 4.4. OSTP Extension Block (8 bytes)

   ```
    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |           stream_id           |           seq_ext             |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                       media_timestamp                         |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   ```

   Fields:

   - stream_id (16 bits, unsigned, network byte order):
     Session identifier assigned by the Source at stream start. The
     Source MUST generate a new random stream_id at each session start.
     Receivers MUST discard packets with an unexpected stream_id after
     receiving the first packet (protects against session hijack).

   - seq_ext (16 bits, unsigned, network byte order):
     Upper 16 bits of the 32-bit extended sequence number. Combined with
     the lower 16 bits in the RTP base header, the full sequence number
     is: (seq_ext << 16) | rtp_sequence_number.
     The extended sequence number increments monotonically from 0.
     Relay Nodes MUST NOT modify seq_ext.

   - media_timestamp (32 bits, unsigned, network byte order):
     Absolute media clock timestamp relative to the session epoch, in
     1/48000 second units. This provides a 32-bit receiver clock
     independent of the RTP timestamp (which wraps). For a 48 kHz Opus
     stream with 20ms frames: media_timestamp = frame_index * 960.
     Receivers SHOULD use media_timestamp for jitter buffer alignment
     rather than wall clock.

### 4.5. Audio Payload

   The audio payload immediately follows the OSTP Extension Block.
   Payload format is determined by the PT field. For Opus (mandatory),
   each packet carries exactly one Opus frame. The Source MUST NOT
   fragment a single Opus frame across multiple OSTP packets.

   Payload length is implicitly determined by:
     payload_length = udp_length - 8 (UDP header)
                      - 12 (RTP base)
                      - 4  (RTP extension header)
                      - 8  (OSTP extension block)
                      - 4  (CRC-32 trailer)

   Maximum recommended payload size: 1172 bytes (to stay within 1200B
   safe UDP MTU). Implementations SHOULD use Opus VBR and target packets
   well below this limit for resilience.

### 4.6. CRC-32 Trailer (4 bytes)

   A CRC-32 (IEEE 802.3 polynomial, 0xEDB88320) computed over all
   preceding bytes of the packet: RTP header + RTP extension header +
   OSTP extension block + audio payload.

   ```
    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                         CRC-32 value                         |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   ```

   Receivers MUST verify the CRC-32 and MUST silently discard packets
   where the CRC-32 does not match. The CRC-32 is appended in network
   byte order (big-endian).

   Rationale: UDP checksums are optional and disabled by default on many
   platforms. The OSTP CRC-32 provides a mandatory integrity check that
   protects against memory corruption, switch bugs, and accidental
   cross-channel packet delivery.

### 4.7. Complete Packet Diagram

   ```
   Offset  Length  Field
   ------  ------  -----
       0       1   V=2, P, X=1, CC=0
       1       1   M, PT
       2       2   RTP Sequence Number (lower 16 bits)
       4       4   RTP Timestamp (48 kHz clock)
       8       4   SSRC
      12       2   Extension Profile (0x4F53)
      14       2   Extension Length (0x0002)
      16       2   stream_id
      18       2   seq_ext (upper 16 bits)
      20       4   media_timestamp
      24       N   Audio Payload (Opus frame)
   24+N       4   CRC-32
   ```

   Total packet size: 28 + N bytes where N is the audio payload length.

### 4.8. Payload Type Assignments

   OSTP reserves the following dynamic PT values in the range 96–127
   as defined by [RFC 3551]:

   | PT  | Codec | Clock Rate | Channels | Description              |
   |-----|-------|-----------|----------|--------------------------|
   | 111 | Opus  | 48000     | 2        | Stereo Opus (RECOMMENDED)|
   | 112 | Opus  | 48000     | 1        | Mono Opus                |
   | 113 | FLAC  | 48000     | 2        | Stereo FLAC (OPTIONAL)   |
   | 114 | FLAC  | 48000     | 1        | Mono FLAC (OPTIONAL)     |
   | 127 | OSTP  | N/A       | N/A      | OSTP control frames      |

   PT=127 (OSTP control) is used for out-of-band control messages
   delivered via the same UDP socket as audio. These include compressed
   fingerprint reports (see Section 8.4).

---

## 5. Channel Addressing

### 5.1. Channel Name Format

   A channel name is a UTF-8 string satisfying the following grammar:

   ```
   channel-name = segment *( "/" segment )
   segment      = 1*64( ALPHA / DIGIT / "-" / "." / "_" )
   ```

   Total length including separators MUST NOT exceed 64 characters.
   Channel names are case-insensitive; implementations MUST normalize
   to lowercase before comparison or lookup.

   Valid examples:
   - `soluna`
   - `soluna/stage-a`
   - `events/zamna/2026-03-14/main-stage`
   - `test.channel_01`

   Invalid examples (MUST be rejected):
   - `soluna stage` (space not allowed)
   - `SOLUNA/Stage` (uppercase: normalize to `soluna/stage`)
   - `a` * 65 (exceeds 64 character limit)
   - `../etc` (dot-dot not allowed in segment start)

### 5.2. Channel Resolution

   A receiver resolves a channel name to a network endpoint using the
   following procedure:

   1. Query the OSTP Bootstrap URL:
      `GET https://relay.solun.art/api/channel/{name}/bootstrap`

   2. The server responds with JSON:
      ```json
      {
        "channel": "soluna/stage-a",
        "stream_id": 42918,
        "ssrc": 3141592653,
        "modes": ["multicast", "p2p", "relay"],
        "multicast_addr": "239.255.1.42",
        "multicast_port": 5004,
        "stun_servers": ["stun:stun.solun.art:3478"],
        "turn_servers": ["turn:turn.solun.art:3478"],
        "relay_ws": "wss://relay.solun.art/ws/audio?channel=soluna%2Fstage-a",
        "sc_ws": "wss://relay.solun.art/ws/swarm?channel=soluna%2Fstage-a",
        "pt": 111,
        "sample_rate": 48000,
        "channels": 2,
        "frame_duration_ms": 20,
        "bitrate_kbps": 128,
        "started_at": "2026-03-14T20:00:00Z",
        "listeners": 4823
      }
      ```

   3. The receiver selects the best connection mode per Section 6.1.

   4. If no active channel exists, the server responds with HTTP 404.

   The bootstrap endpoint MUST be served over HTTPS. Responses SHOULD
   set `Cache-Control: no-cache` as channel metadata can change.

### 5.3. Wildcard and Hierarchical Channels

   Channel names support hierarchical subscription. A subscriber MAY
   express interest in all sub-channels of a prefix:

   - `soluna/*` matches `soluna/stage-a`, `soluna/stage-b`, etc.
   - `events/**` matches any depth under `events/`.

   The Swarm Coordinator resolves wildcard subscriptions server-side.
   The protocol does not expose wildcard syntax to UDP datagrams;
   wildcard expansion is a bootstrap-time operation only.

   A receiver receiving on a wildcard subscription will receive
   separate OSTP streams (different SSRC values) for each matched
   channel. The receiver MUST treat each SSRC as an independent stream.

---

## 6. Connection Modes

### 6.1. Mode Selection Algorithm

   Upon receiving the bootstrap response, the receiver selects a
   connection mode using the following ordered priority:

   ```
   1. If LAN multicast is available AND listener is on same LAN
      as source → LAN Multicast Mode (target latency < 5ms)

   2. If P2P feasible (STUN reachable, not behind symmetric NAT)
      AND group size <= 50 → P2P Mode (target latency < 50ms)

   3. Else → Relay Mode (target latency ~100ms)
   ```

   "Same LAN" is determined by comparing the receiver's default gateway
   against the multicast group's TTL scope. If the multicast address is
   in 239.255.0.0/16 (site-local) and the receiver's IP is in the same
   /24 as the source, LAN multicast is attempted.

   "P2P feasible" is determined by a STUN binding request. If the
   mapped address differs from the local address AND the NAT type is
   not symmetric (detected by probing two STUN servers), P2P is
   available.

   Mobile devices on cellular connections SHOULD prefer Relay Mode
   regardless, as UDP hole-punching is unreliable over carrier-grade
   NAT.

### 6.2. LAN Multicast Mode

   The Source sends OSTP packets to a multicast group address in the
   239.255.0.0/16 range. The specific address is derived deterministically
   from the channel name:

   ```
   multicast_addr = 239.255 . (crc16(channel_name) >> 8)
                              . (crc16(channel_name) & 0xFF)
   multicast_port = 5004  (default) or as specified in bootstrap
   ```

   Receivers join the multicast group using OS-level socket options
   (IP_ADD_MEMBERSHIP). IGMP v2/v3 is used for group management.

   LAN multicast packets are identical in format to unicast OSTP packets.
   The Source MUST set the IP TTL to 1 for site-local groups to prevent
   leakage beyond the LAN.

   Expected latency: < 5ms (LAN propagation + switch forwarding).
   Jitter: < 1ms on well-managed switches.

### 6.3. P2P Mode (STUN/TURN)

   For WAN delivery with group size ≤ 50 listeners, the Source
   establishes direct UDP connections to each receiver using ICE
   [RFC 8445]. The OSTP bootstrap server provides STUN and TURN URIs.

   ICE candidate exchange uses the Swarm Coordinator WebSocket:
   ```json
   {"type": "ice_candidate", "channel": "soluna/stage-a",
    "from": "src", "to": "leaf-uuid", "candidate": "..."}
   ```

   Once ICE connectivity checks succeed, OSTP packets flow directly
   peer-to-peer. The Source maintains one UDP socket per receiver.

   Expected latency: 20–50ms (1 × RTT from source to receiver).
   This mode is NOT suitable for large groups (CPU/socket overhead).

### 6.4. Relay Mode

   Relay mode uses a WebSocket connection to the relay server. The
   WebSocket URL format:

   ```
   wss://relay.solun.art/ws/audio?channel=<name>&format=<fmt>
   ```

   Supported `format` values:
   - `raw`: Relay strips all OSTP/RTP headers and sends bare Opus frames.
     Each WebSocket binary message is exactly one Opus frame.
   - `ostp`: Full OSTP packets as binary WebSocket frames.
   - `rtp`: RTP packets (without OSTP extension) for AES67 compatibility.

   The `format=raw` mode is RECOMMENDED for browser clients as it
   eliminates the need for RTP parsing in JavaScript. The relay performs
   header stripping server-side.

   When `format=raw` is used:
   - Each binary WebSocket message is one Opus audio frame.
   - A JSON text frame MAY be sent before audio begins:
     ```json
     {"type":"stream_info","sample_rate":48000,"channels":2,
      "frame_duration_ms":20,"bitrate_kbps":128,
      "stream_id":42918,"started_at":"2026-03-14T20:00:00Z"}
     ```
   - The receiver SHOULD use this to configure the audio decoder.

   Expected latency: 80–150ms (relay server processing + 1 RTT).

### 6.5. Mode Transitions

   A receiver MAY transition between modes without interrupting playback
   using the following procedure:

   1. Open a new connection in the target mode.
   2. Wait for audio to flow on the new connection.
   3. Align jitter buffers using media_timestamp.
   4. Switch audio output to new connection.
   5. Close old connection.

   Step 3 is critical: the receiver compares the media_timestamp values
   from both connections and introduces or removes jitter buffer samples
   to achieve seamless transition.

   Implementations SHOULD monitor network conditions and trigger mode
   transitions automatically when:
   - Packet loss rate > 5% for > 10 seconds → upgrade to better mode
   - Available bandwidth drops below 1.5× stream bitrate → downgrade

### 6.6. NAT Traversal via Server-Reflexive Address Discovery

   OSTP relays implement a lightweight STUN-like mechanism to help
   receivers discover their public IP:port. Upon JOIN, the relay MUST
   immediately respond with the client's server-reflexive address:

   ```
   Client → Relay  UDP:  JOIN:<channel>\n
   Relay  → Client UDP:  YOUR_ADDR:<public_ip>:<public_port>\n
   ```

   This allows clients to share their public address with peers for
   UDP hole-punching without requiring a separate STUN server.

   **WAN P2P Hole-Punching Procedure:**

   ```
   1. Client A JOINs channel → relay responds YOUR_ADDR:A_ip:A_port
   2. Client B JOINs channel → relay responds YOUR_ADDR:B_ip:B_port
   3. Relay sends PEER:B_ip:B_port to A, and PEER:A_ip:A_port to B
   4. Both clients simultaneously send UDP probes to each other's
      public address (simultaneous open, RFC 4787 §14)
   5. Once probes cross, direct UDP path is open
   6. ROUTE:A_ip:A_port:p2p (sent to relay to indicate preference)
   ```

   **Limitations:**
   - Symmetric NAT (common on LTE/5G carrier-grade NAT) prevents
     hole-punching. Clients behind symmetric NAT MUST use Relay Mode.
   - IPv6 clients do not require NAT traversal; direct UDP is possible.
   - The YOUR_ADDR response is NOT available when connecting via
     WebSocket (wss://) as the relay sees the TCP connection, not UDP.

### 6.7. NACK Retransmission via WebSocket

   WebSocket receivers (browsers, mobile apps) that cannot send raw
   UDP packets to the relay may request retransmission via a WebSocket
   text frame:

   ```
   Client → Relay (WebSocket text):  NACK:<seq1>,<seq2>,...\n
   Relay  → Client (WebSocket binary): <OSTP packet for seq1>
   ```

   The relay maintains a replay buffer of recent packets per channel.
   Sequence numbers are 16-bit RTP seq values. A receiver SHOULD
   request NACK within 200ms of detecting a gap (before jitter buffer
   expires the slot). The relay MUST NOT send more than 32 retransmits
   per NACK request to prevent abuse.

   NACK counters are tracked in Prometheus metrics:
   - `soluna_relay_nack_requests_total`
   - `soluna_relay_nack_retransmits_total`

### 6.8. Economic Layer HTTP API

   For environments where UDP is not bidirectional (e.g., clients behind
   Carrier-Grade NAT, or web browsers), the economic layer is accessible
   via HTTPS REST:

   **POST /api/wallet/charge**
   ```json
   Request:  {"device_id":"<id>", "amount":<float>, "token":"<ts>:<hmac>"}
   Response: {"ok":true, "balance":<float>, "charged":<float>}
   ```

   **GET /api/wallet?device_id=<id>**
   ```json
   Response: {"device_id":"<id>", "balance":<float>, "found":<bool>}
   ```

   HMAC computation (CHARGE):
   ```
   key     = RELAY_CHARGE_SECRET (shared secret)
   message = "CHARGE:" + std::to_string(amount) + ":" + device_id + ":" + timestamp
   token   = timestamp + ":" + HMAC-SHA256-hex(key, message)
   ```
   Note: `std::to_string(double)` uses 6 decimal places (e.g., `0.010000`).
   Replay protection: tokens are valid for 300 seconds; duplicate tokens
   within the window are rejected with HTTP 409.

---

## 7. Swarm Distribution Protocol

### 7.1. Overview and Rationale

   For large audiences (> 50 listeners), the Source cannot economically
   serve every listener directly. The Swarm Distribution Protocol (SDP)
   allows willing listener nodes to act as Relay Nodes, forwarding audio
   to downstream children.

   The swarm is a complete k-ary tree (default k=4, called "fanout-4")
   rooted at the Source. Each Relay Node receives from one parent and
   forwards to up to k-1 children (one slot is reserved for the
   secondary parent for failover). Effective fanout is thus 3.

   At depth d, the tree can support (k-1)^d leaf nodes. A 5-level
   fanout-3 tree supports 3^5 = 243 leaf branches, sufficient for
   most venues.

   The Swarm Coordinator assigns parent-child relationships. It does NOT
   forward audio; it only manages topology via WebSocket signaling.

### 7.2. Node Roles

   **Source**
   - Originates audio at the root of the swarm tree.
   - Maintains direct connections to tier-1 Relay Nodes.
   - Reports swarm health to the Swarm Coordinator.
   - MUST NOT act as a child in its own swarm.

   **Relay Node**
   - Receives OSTP packets from one primary parent and one secondary
     parent (for failover).
   - Forwards received packets to all registered children within 1ms.
   - Reports upstream packet loss, jitter, and bandwidth to the SC.
   - Eligible: desktop/server nodes with upload_kbps ≥ 2× stream_kbps.

   **Leaf Node**
   - Receives OSTP packets from one parent (Relay Node or Source).
   - Does NOT forward packets to any children.
   - Eligible: any receiver including mobile.
   - ALL mobile devices on cellular MUST be assigned as Leaf Nodes.

### 7.3. Swarm Activation Threshold

   The Swarm Coordinator MUST activate the swarm when:

   ```
   listener_count > SWARM_ACTIVATION_THRESHOLD  (default: 50)
   ```

   Below the threshold, the Source delivers directly to all listeners.
   At threshold crossing, the SC begins soliciting Relay Nodes via the
   SWARM_READY signaling message.

   The threshold MAY be configured per-channel in the bootstrap
   response. Channels expecting large audiences SHOULD set lower
   thresholds to pre-build the swarm before it is needed.

### 7.4. Tree Construction

   The SC constructs the fanout-4 tree using the following algorithm:

   ```
   Algorithm SWARM_BUILD(candidates, source):
     tier1 = select_relays(candidates, 4, prefer=high_bandwidth)
     assign(tier1, parent=source)
     tier2 = select_relays(candidates, 12, prefer=high_bandwidth,
                           exclude=tier1)
     assign(tier2, parent=round_robin(tier1))
     ...
     remaining = all remaining nodes
     assign(remaining, parent=least_loaded_relay(), role=LEAF)
   ```

   More precisely, the SC maintains a priority queue of available Relay
   Node candidates sorted by:
   1. Upload bandwidth (highest first)
   2. RTT to source (lowest first)
   3. Join time (earliest first, as tiebreaker)

   Each Relay Node slot is filled from this queue. Once a node is
   assigned as a Relay Node, it is removed from the leaf queue.

   The SC MUST send topology assignments before the source begins
   transmitting (for planned events) or within 5 seconds of threshold
   crossing (for organic growth).

### 7.5. Signaling Messages

   All signaling uses JSON over WebSocket to the SC endpoint.
   All messages include a `type` field and a `channel` field.

   **SWARM_QUERY** (SC → Node, sent after join)
   ```json
   {
     "type": "SWARM_QUERY",
     "channel": "soluna/stage-a",
     "request_id": "rq-abc123",
     "upload_kbps_min": 512
   }
   ```
   The SC asks the node whether it can act as a Relay Node.
   `upload_kbps_min` is the minimum upload bandwidth required.

   **SWARM_READY** (Node → SC, response to SWARM_QUERY)
   ```json
   {
     "type": "SWARM_READY",
     "channel": "soluna/stage-a",
     "request_id": "rq-abc123",
     "upload_kbps": 8000,
     "rtt_to_source_ms": 12,
     "nat_type": "full_cone",
     "os": "linux",
     "udp_port": 5005
   }
   ```
   The node volunteers as a Relay Node and reports its capabilities.
   `nat_type` MUST be one of: `full_cone`, `restricted`, `port_restricted`,
   `symmetric`, `unknown`. Symmetric NAT nodes SHOULD NOT volunteer.

   **SWARM_UNABLE** (Node → SC, response to SWARM_QUERY)
   ```json
   {
     "type": "SWARM_UNABLE",
     "channel": "soluna/stage-a",
     "request_id": "rq-abc123",
     "reason": "cellular"
   }
   ```
   The node declines Relay Node duty. `reason` MUST be one of:
   `cellular`, `low_upload`, `symmetric_nat`, `battery_saver`, `user_declined`.

   **SWARM_ASSIGN** (SC → Node, assignment order)
   ```json
   {
     "type": "SWARM_ASSIGN",
     "channel": "soluna/stage-a",
     "role": "relay",
     "primary_parent": {
       "node_id": "src",
       "addr": "203.0.113.10",
       "port": 5004
     },
     "secondary_parent": {
       "node_id": "relay-a2",
       "addr": "203.0.113.20",
       "port": 5005
     },
     "children": [],
     "stream_id": 42918,
     "ssrc": 3141592653
   }
   ```
   The SC assigns the node a role and provides parent/child addresses.
   The node MUST begin connecting to its primary parent within 500ms.

   **SWARM_ACK** (Node → SC, acknowledges assignment)
   ```json
   {
     "type": "SWARM_ACK",
     "channel": "soluna/stage-a",
     "role": "relay",
     "primary_connected": true,
     "secondary_connected": true,
     "latency_primary_ms": 14,
     "latency_secondary_ms": 18
   }
   ```
   Sent after the node has established connectivity to its parents.

   **SWARM_CHILD_ADD** (SC → Relay Node)
   ```json
   {
     "type": "SWARM_CHILD_ADD",
     "channel": "soluna/stage-a",
     "child": {
       "node_id": "leaf-xyz789",
       "addr": "198.51.100.55",
       "port": 5006
     }
   }
   ```
   Instructs a Relay Node to add a new child to its forward list.

   **SWARM_CHILD_REMOVE** (SC → Relay Node)
   ```json
   {
     "type": "SWARM_CHILD_REMOVE",
     "channel": "soluna/stage-a",
     "child_node_id": "leaf-xyz789"
   }
   ```

   **SWARM_LOST** (Node → SC, signals parent disconnection)
   ```json
   {
     "type": "SWARM_LOST",
     "channel": "soluna/stage-a",
     "lost_parent_id": "relay-a1",
     "last_seq": 1048576,
     "failover_to": "secondary"
   }
   ```
   The node has lost its primary parent and is failing over to the
   secondary. The SC MUST acknowledge with a SWARM_ASSIGN carrying a
   new secondary parent within 500ms of receiving SWARM_LOST.

   **SWARM_STATS** (Node → SC, periodic health report, every 10s)
   ```json
   {
     "type": "SWARM_STATS",
     "channel": "soluna/stage-a",
     "role": "relay",
     "packets_received": 50000,
     "packets_forwarded": 150000,
     "loss_rate_pct": 0.1,
     "jitter_ms": 2.3,
     "children_count": 3,
     "upload_kbps_actual": 384
   }
   ```

   **SWARM_TEARDOWN** (SC → All Nodes, stream ending)
   ```json
   {
     "type": "SWARM_TEARDOWN",
     "channel": "soluna/stage-a",
     "reason": "source_stopped",
     "reconnect_delay_s": 0
   }
   ```

### 7.6. Node State Machine

   Each node maintains the following state machine for swarm participation:

   ```
   States: IDLE, QUERIED, ASSIGNED, CONNECTING, ACTIVE, FAILED

   Transitions:
     IDLE       → QUERIED     : on SWARM_QUERY received
     QUERIED    → IDLE        : on SWARM_UNABLE sent (declined)
     QUERIED    → ASSIGNED    : on SWARM_ASSIGN received
     ASSIGNED   → CONNECTING  : on UDP connect attempt to primary parent
     CONNECTING → ACTIVE      : on first OSTP packet received + SWARM_ACK sent
     CONNECTING → FAILED      : on connect timeout (2000ms)
     ACTIVE     → FAILED      : on primary parent lost + secondary failed
     ACTIVE     → ACTIVE      : on SWARM_CHILD_ADD/REMOVE
     ACTIVE     → IDLE        : on SWARM_TEARDOWN received
     FAILED     → ASSIGNED    : on new SWARM_ASSIGN from SC (recovery)
     FAILED     → IDLE        : on timeout (30s, give up relay duty)
   ```

   Implementations MUST implement this state machine exactly. A node in
   FAILED state MUST NOT forward audio packets. It MUST continue
   receiving from its secondary parent (if still connected) to enable
   fast recovery when the SC assigns a new primary.

### 7.7. Parent Failover

   Each Relay Node and Leaf Node maintains a primary and secondary
   parent connection. The secondary parent receives the same stream
   independently, but the node only forwards/plays from the primary.

   Failover procedure:
   1. Primary parent is considered lost if no OSTP packet is received
      for > 50ms (2.5× the 20ms frame interval).
   2. The node immediately switches to the secondary parent's stream.
      Audio output switches without gap (secondary parent is current
      to within 50ms of primary, so jitter buffer absorbs the switch).
   3. The node sends SWARM_LOST to the SC.
   4. The SC responds with a SWARM_ASSIGN containing a new secondary
      parent within 500ms.
   5. The node connects to the new secondary parent while continuing
      to receive from the (now primary) former secondary.

   The 50ms failover target means listeners experience at most one
   missed frame (20ms) plus jitter buffer smoothing (typically 60ms).
   With a 120ms jitter buffer, failover is imperceptible.

   If both parents are lost simultaneously:
   1. The node immediately connects to the relay server as fallback.
   2. Sends SWARM_LOST with `failover_to: "relay_server"`.
   3. SC promotes the node to a higher position or demotes to Leaf.

### 7.8. Mobile Device Handling

   Mobile devices MUST be detected and assigned as Leaf Nodes regardless
   of their upload bandwidth. Detection criteria:

   - User-Agent string contains "Mobile", "Android", "iPhone", "iPad"
   - Bootstrap query parameter `?mobile=1` (set by client SDK)
   - SWARM_READY message contains `"os": "ios"` or `"os": "android"`
   - Available upload bandwidth < 1 Mbps

   Additionally, devices on cellular connections (detected via Network
   Information API in browsers, or OS network type APIs on native
   clients) MUST:
   - Send SWARM_UNABLE with `reason: "cellular"`
   - Prefer relay_ws over P2P connections (cellular NAT is asymmetric)
   - Use longer jitter buffers (200ms vs 60ms for WiFi)

### 7.9. Capacity Advertisement

   When a node sends SWARM_READY, it SHOULD measure and report:

   - `upload_kbps`: Available upload bandwidth (Mbps × 1000). MUST be
     measured, not assumed. Nodes SHOULD run a 2-second upload speed
     test to the relay server before volunteering.
   - `rtt_to_source_ms`: Round-trip time to the source's IP address,
     measured via ICMP or UDP echo.
   - `nat_type`: NAT classification from STUN binding tests.
   - `cpu_load_pct`: Current CPU usage (0–100). Nodes with > 80% CPU
     SHOULD NOT volunteer.
   - `battery_pct` (optional): For mobile devices that somehow volunteer.
     Nodes with battery < 20% SHOULD NOT volunteer.

### 7.10. Swarm Teardown

   When the Source stops streaming:
   1. Source sends a SWARM_TEARDOWN via WebSocket to SC.
   2. SC broadcasts SWARM_TEARDOWN to all connected nodes.
   3. Each node closes its UDP connections, frees relay resources,
      and returns to IDLE state.
   4. Nodes SHOULD wait `reconnect_delay_s` seconds before attempting
      to rejoin (for planned restarts).

   If the Source disconnects without sending SWARM_TEARDOWN (crash):
   1. SC detects absence of Source heartbeat (30s timeout).
   2. SC sends SWARM_TEARDOWN with `reason: "source_timeout"` and
      `reconnect_delay_s: 60` to all nodes.

---

## 8. Economic Layer

### 8.1. Wallet Model

   Each OSTP session participant has an associated wallet. The wallet is
   a server-side balance stored in the Swarm Coordinator's database,
   denominated in integer cents (USD × 100). The wallet is identified
   by a session token (see Section 11.2).

   Wallet balances are used for:
   - CHARGE: The SC charges a listener for stream access.
   - WITHDRAW: A relay node withdraws earned relay compensation.
   - TIP: A listener sends a voluntary tip to the Source.
   - SUPPORT: Equivalent to TIP but directed to a specific relay node.

   The wallet system is designed for micropayments in the 0.1–1.0 cent
   range per minute of stream consumption. It is NOT a blockchain-based
   system; it is a server-side ledger optimized for high-throughput
   audio billing.

   Integration with on-chain settlement (Solana, etc.) is OPTIONAL and
   out of scope for this specification. Implementations MAY periodically
   settle ledger balances on-chain.

### 8.2. Wallet Commands

   Wallet commands are sent as JSON over the Swarm Coordinator WebSocket.

   **CHARGE** (SC → Listener, billing debit)
   ```json
   {
     "type": "CHARGE",
     "channel": "soluna/stage-a",
     "session_token": "st-abc123def456",
     "amount_cents": 1,
     "reason": "stream_access_60s",
     "timestamp_unix": 1741996800,
     "nonce": "n-xyz789",
     "hmac": "3d7f2a..."
   }
   ```
   The SC charges the listener's wallet. The listener MUST verify the
   HMAC before accepting the charge (see Section 8.3).

   **CHARGE_ACK** (Listener → SC)
   ```json
   {
     "type": "CHARGE_ACK",
     "nonce": "n-xyz789",
     "accepted": true
   }
   ```
   If `accepted: false`, the SC SHOULD terminate the stream for that
   listener. The listener MUST provide a `reason` field if declining:
   `insufficient_funds`, `hmac_invalid`, `replay_detected`.

   **WITHDRAW** (Relay Node → SC, claim relay earnings)
   ```json
   {
     "type": "WITHDRAW",
     "channel": "soluna/stage-a",
     "session_token": "st-relay-node-111",
     "amount_cents": 50,
     "relay_stats": {
       "packets_forwarded": 750000,
       "bytes_forwarded": 262500000,
       "children_served": 3,
       "uptime_s": 3600
     }
   }
   ```

   **TIP** (Listener → SC → Source)
   ```json
   {
     "type": "TIP",
     "channel": "soluna/stage-a",
     "session_token": "st-listener-222",
     "amount_cents": 100,
     "message": "Amazing set!",
     "recipient": "source"
   }
   ```

   **SUPPORT** (Listener → SC → Relay Node)
   ```json
   {
     "type": "SUPPORT",
     "channel": "soluna/stage-a",
     "session_token": "st-listener-333",
     "amount_cents": 25,
     "recipient_node_id": "relay-a1"
   }
   ```

   **BALANCE_QUERY** (Node → SC)
   ```json
   {
     "type": "BALANCE_QUERY",
     "session_token": "st-abc123def456"
   }
   ```

   **BALANCE_RESPONSE** (SC → Node)
   ```json
   {
     "type": "BALANCE_RESPONSE",
     "balance_cents": 1050,
     "pending_cents": 75,
     "currency": "USD"
   }
   ```

### 8.3. CHARGE Command Security

   The CHARGE command MUST be authenticated using HMAC-SHA256 to prevent
   a malicious server from arbitrarily draining listener wallets.

   The HMAC is computed as follows:
   ```
   message = channel + ":" + session_token + ":" +
             amount_cents + ":" + reason + ":" +
             timestamp_unix + ":" + nonce
   hmac = HMAC-SHA256(wallet_shared_secret, message)
   ```

   The `wallet_shared_secret` is established during session setup (see
   Section 11.2) and is never transmitted in plaintext.

   Replay prevention:
   - The listener MUST reject any CHARGE with `timestamp_unix` more
     than 300 seconds in the past or 60 seconds in the future.
   - The listener MUST maintain a nonce cache for the past 600 seconds
     and reject any CHARGE with a previously-seen nonce.
   - The cache SHOULD use an LRU structure with a maximum of 1000 entries.

   Session token rotation:
   - The SC SHOULD rotate session tokens every 3600 seconds.
   - The new token is sent to the client as a `TOKEN_ROTATE` message
     before expiry. The client MUST use the new token for subsequent
     CHARGE_ACK responses.

### 8.4. Copyright Fingerprinting

   OSTP includes a distributed copyright detection system that does NOT
   require centralized audio fingerprinting services. The system is
   privacy-preserving: no raw audio leaves the listener's device.

   **Fingerprinting Algorithm:**

   1. Each listener computes a 64-bit audio fingerprint every 30 seconds.
      The fingerprint is derived from:
      - Compute energy bands: split the 30s audio window into 8
        frequency bands using a simple FFT (1024-point, overlapping).
      - For each of the 64 time slots × 8 bands: compute whether energy
        is increasing (1) or decreasing (0) vs previous slot.
      - Pack 64 bits: concatenate the 64 most significant band-transition
        bits from the central 8 bands.

   2. The listener sends the fingerprint to the SC via the control
      PT=127 channel (or WebSocket if relay mode):
      ```json
      {
        "type": "FINGERPRINT_REPORT",
        "channel": "soluna/stage-a",
        "window_start_ms": 30000,
        "fingerprint_hex": "4f8a2b1c9d7e3f00",
        "stream_id": 42918
      }
      ```

   3. The SC collects fingerprint reports from all listeners.
      A copyright match is declared when:
      - 2 or more listeners report the same fingerprint window, AND
      - The Hamming distance between any two reported fingerprints is ≤ 8
        (allowing for minor decoding differences due to packet loss).

   4. If a match is found, the SC queries a fingerprint database
      (external service) with the 64-bit hash to identify the track.

   **Consensus Requirement:**
   The 2-report minimum prevents a single malicious listener from
   triggering false copyright claims. The Hamming distance ≤ 8 threshold
   allows for up to 8 bit-flips caused by packet loss or DAC/ADC
   variations while still reliably identifying the same track.

   **Privacy Properties:**
   - Raw audio never leaves the listener's device.
   - The 64-bit fingerprint has ~2^64 possible values; collision
     probability for random audio is negligible.
   - Fingerprints are reported per-window, not continuously.
   - The SC MUST NOT store fingerprints longer than 48 hours.

### 8.5. Royalty Distribution

   When a copyright match is confirmed and a CHARGE is processed for
   stream access, the revenue is split as follows:

   | Recipient     | Share | Rationale                                    |
   |---------------|-------|----------------------------------------------|
   | Rights Holder | 70%   | Songwriter + label/publisher                 |
   | DJ Cashback   | 20%   | Performance royalty to the stream source     |
   | Platform      | 10%   | Infrastructure + coordination fee            |

   The rights holder allocation uses standard mechanical/performance
   royalty splitting rules per territory. The SC is responsible for
   tracking and settling these allocations.

   For streams where no copyright match is detected (original content
   or unrecognized tracks):
   - Rights Holder: 0%
   - DJ/Source: 85%
   - Platform: 15%

   Royalty settlement occurs at session end or every 24 hours for
   long-running streams. The SC MUST generate a signed royalty receipt
   for each settlement.

### 8.6. Economic Message Framing

   Economic messages MAY also be carried in-band via PT=127 control
   frames if the receiver is in UDP mode (not relay WebSocket mode).
   In that case, the audio payload of PT=127 packets contains a
   length-prefixed JSON blob:

   ```
   [ 2-byte length (big-endian) ] [ JSON UTF-8 bytes ]
   ```

   The CRC-32 trailer still applies. The stream_id and seq_ext fields
   in the OSTP extension are set to the stream's current values.
   In-band economic messages use a separate seq_ext counter space
   (top bit of seq_ext set to 1).

---

## 9. Audio Codecs

### 9.1. Mandatory Codec: Opus

   All OSTP implementations MUST support Opus [RFC 6716] with the
   following parameters:

   - Sample rate: 48000 Hz (MUST)
   - Channels: 2 (stereo, RECOMMENDED) or 1 (mono)
   - Frame duration: 20ms (RECOMMENDED), 10ms (OPTIONAL)
   - Bitrate: 32–320 kbps VBR (RECOMMENDED: 128 kbps stereo)
   - Application: OPUS_APPLICATION_AUDIO (NOT VOIP, NOT RESTRICTED_LOWDELAY)
   - Complexity: 10 (maximum, for best quality at target bitrate)
   - FEC: OPTIONAL (adds 4 bytes per frame, recovers from single loss)

   OPUS_APPLICATION_AUDIO is required because it applies all of Opus's
   audio quality features (bandwidth detection, signal-specific coding)
   rather than voice-optimized shortcuts.

### 9.2. Optional Codec: FLAC

   High-fidelity installations MAY use FLAC for lossless audio:
   - Sample rate: 48000 Hz or 44100 Hz
   - Bit depth: 24-bit (RECOMMENDED) or 16-bit
   - Frame size: 4096 samples (85.3ms at 48 kHz)
   - Compression level: 5 (RECOMMENDED, balances CPU and size)

   FLAC is NOT recommended for WAN delivery due to variable frame sizes
   and high bandwidth requirements (1–5 Mbps for 24-bit stereo).

### 9.3. Codec Negotiation

   Codec negotiation is performed at the HTTP bootstrap stage. The client
   sends an `Accept-Codecs` query parameter:

   ```
   GET /api/channel/soluna/stage-a/bootstrap?accept_codecs=opus,flac
   ```

   The server responds with `"pt": <chosen_payload_type>` in the JSON
   bootstrap response. If the server cannot satisfy any offered codec,
   it returns HTTP 406 Not Acceptable.

---

## 10. Congestion Control and Quality Adaptation

### 10.1. Receiver Reports

   Receivers MUST send RTCP Receiver Reports [RFC 3550 §6.4.2] every
   5 seconds. The RTCP port is the OSTP audio port + 1.

   Additionally, receivers MUST send OSTP-extended receiver reports
   via the Swarm Coordinator WebSocket every 10 seconds:

   ```json
   {
     "type": "RECEIVER_REPORT",
     "channel": "soluna/stage-a",
     "ssrc": 3141592653,
     "packets_received": 25000,
     "packets_lost": 12,
     "loss_rate_pct": 0.048,
     "jitter_ms": 1.8,
     "last_seq": 1048600,
     "rtt_ms": 45
   }
   ```

### 10.2. Bitrate Ladder

   Sources SHOULD support multiple bitrate tiers and switch based on
   aggregate receiver reports. Recommended Opus ladder:

   | Tier | Bitrate | Quality  | Use Case                    |
   |------|---------|----------|-----------------------------|
   | 0    | 32 kbps | Voice    | Emergency / very poor WAN   |
   | 1    | 64 kbps | Good     | Standard mobile              |
   | 2    | 128 kbps| Hi-Fi    | Default / WiFi desktop       |
   | 3    | 192 kbps| Studio   | Venue / wired connections    |
   | 4    | 320 kbps| Master   | Recording / archival         |

   The Source SHOULD downgrade if > 10% of receivers report > 3%
   packet loss sustained for > 30 seconds.

   Per-receiver adaptation: Relay Nodes MAY transcode between tiers if
   the relay hardware supports it. In practice, transcoding adds latency
   and CPU load; it is RECOMMENDED only for dedicated relay servers,
   not listener-volunteered relay nodes.

### 10.3. FEC and Packet Recovery

   OSTP supports two packet recovery mechanisms:

   **Opus In-Band FEC**: When the Opus encoder is configured with
   `OPUS_SET_INBAND_FEC(1)`, each packet carries a low-bitrate
   redundant encoding of the previous frame. Receivers that miss a
   packet can reconstruct it from the next packet's FEC data.
   This adds 6–10 kbps overhead and recovers from single-packet loss
   with no additional RTT.

   **RTP Retransmission (RFC 4588)**: For relay-mode connections over
   WebSocket, receivers MAY request retransmission of lost packets via:
   ```json
   {"type": "NACK", "channel": "soluna/stage-a",
    "ssrc": 3141592653, "seq": [1048590, 1048591]}
   ```
   The relay server SHOULD maintain a retransmission buffer of the last
   500 packets (10 seconds at 50 pps). RTX latency adds 1 × server RTT.
   RTX is NOT supported in UDP swarm mode (too late to be useful).

---

## 11. Security Considerations

### 11.1. DTLS-SRTP

   DTLS-SRTP [RFC 5764] encryption is OPTIONAL for OSTP UDP connections.
   When enabled, all audio payload bytes are encrypted using AES-128-GCM.
   The DTLS handshake uses the same STUN/TURN infrastructure as P2P mode.

   Relay Nodes MUST decrypt and re-encrypt when forwarding to children
   (media is decrypted at each hop). This prevents end-to-end encryption
   at the swarm layer but allows the relay node to verify CRC-32.

   End-to-end SRTP (E2E-SRTP) where relay nodes cannot decrypt is
   NOT supported in this version of OSTP.

   DTLS-SRTP is REQUIRED when OSTP is used for private/paid content
   to prevent passive eavesdropping. It is OPTIONAL for public broadcast
   streams where confidentiality is not required.

### 11.2. Session Tokens

   Session tokens are generated by the bootstrap server at join time.
   A session token is a 256-bit random value encoded as a 43-character
   base64url string.

   At bootstrap, the server returns:
   ```json
   {
     "session_token": "st-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
     "wallet_shared_secret": "wss-BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB",
     "token_expires_at": "2026-03-14T22:00:00Z"
   }
   ```

   The `wallet_shared_secret` is used only for HMAC computation
   (Section 8.3). It MUST be transmitted over HTTPS and MUST NOT be
   logged, stored in localStorage without encryption, or included in
   WebSocket messages.

   The session token is included in WebSocket protocol negotiation:
   ```
   GET /ws/audio?channel=soluna/stage-a&format=raw
   Authorization: Bearer st-AAAAAAA...
   ```

### 11.3. Rate Limiting

   The relay server MUST implement rate limiting:
   - Maximum 10 JOIN requests per IP per minute.
   - Maximum 1000 WebSocket connections per IP.
   - Maximum 100 FINGERPRINT_REPORT messages per session per hour.
   - Maximum 10 NACK requests per second per session.
   - Maximum 5 SWARM_READY messages per session.

   UDP flood prevention:
   - OSTP relay server MUST validate CRC-32 before processing.
   - Packets with invalid CRC-32 MUST be dropped without logging.
   - Maximum 10,000 UDP packets per second per source IP.

### 11.4. Economic Security

   The economic layer has several attack surfaces:

   **CHARGE replay**: Prevented by nonce caching and timestamp validation
   (Section 8.3). The 300-second window means a 5-minute network outage
   cannot be used to replay old charges.

   **Fake fingerprints**: A malicious listener could submit false
   fingerprints to trigger false copyright claims. Mitigated by the
   2-report consensus requirement and Hamming distance check.

   **Relay earnings fraud**: A malicious relay could claim to have
   forwarded packets it did not forward. Mitigated by:
   - SC monitoring downstream ACKs from children of the relay.
   - Spot-check probes where the SC connects as a listener to the relay.
   - Earnings are capped per relay slot based on declared fanout.

   **Wallet drain via SUPPORT storms**: Rate-limit TIP/SUPPORT messages
   to maximum 60 per hour per session.

### 11.5. Fingerprint Privacy

   Audio fingerprints reveal what content is being listened to. To
   protect listener privacy:
   - Fingerprints are NOT associated with user identity in the SC's
     database. They are associated only with ephemeral session tokens.
   - Session tokens are rotated hourly (Section 8.3).
   - Fingerprint reports are deleted after 48 hours.
   - The fingerprint database query uses a privacy-preserving API
     (k-anonymity hash prefix matching, similar to HaveIBeenPwned).

---

## 12. IANA Considerations

   OSTP requests the following RTP profile identifier registration:
   - Profile value 0x4F53 ("OS") for the OSTP RTP extension header.
   - PT values 111–114 and 127 as described in Section 4.8.

   OSTP requests reservation of UDP port 5004 (already assigned to RTP
   by IANA) as the default OSTP audio port, consistent with AES67.

   The OSTP protocol is identified by the URI:
   `https://opensonic.io/protocol/ostp/1`

---

## 13. References

### Normative References

   [RFC 2119]  Bradner, S., "Key words for use in RFCs to Indicate
               Requirement Levels", BCP 14, RFC 2119, March 1997.

   [RFC 2104]  Krawczyk, H., Bellare, M., and R. Canetti, "HMAC:
               Keyed-Hashing for Message Authentication", RFC 2104,
               February 1997.

   [RFC 3550]  Schulzrinne, H., et al., "RTP: A Transport Protocol for
               Real-Time Applications", RFC 3550, July 2003.

   [RFC 3551]  Schulzrinne, H. and S. Casner, "RTP Profile for Audio
               and Video Conferences", RFC 3551, July 2003.

   [RFC 4588]  Rey, J., et al., "RTP Retransmission Payload Format",
               RFC 4588, July 2006.

   [RFC 5764]  McGrew, D. and E. Rescorla, "Datagram Transport Layer
               Security (DTLS) Extension to Establish Keys for the
               Secure Real-time Transport Protocol (SRTP)", RFC 5764,
               May 2010.

   [RFC 6716]  Valin, J.-M., Vos, K., and T. Terriberry, "Definition
               of the Opus Audio Codec", RFC 6716, September 2012.

   [RFC 8445]  Keranen, A., Holmberg, C., and J. Rosenberg, "Interactive
               Connectivity Establishment (ICE)", RFC 8445, July 2018.

   [RFC 8835]  Alvestrand, H., "Transports for WebRTC", RFC 8835,
               January 2021.

### Informative References

   [AES67-2015] AES, "AES67-2015: AES standard for audio applications
                of networks — High-performance streaming audio-over-IP
                interoperability", 2015.

   [OPUS-SPEC] Opus Interactive Audio Codec, https://opus-codec.org/

   [WEBCODECS] W3C, "WebCodecs API",
               https://www.w3.org/TR/webcodecs/, 2023.

   [RFC 7826]  Schulzrinne, H., et al., "Real-Time Streaming Protocol
               Version 2.0", RFC 7826, December 2016.

   [SOLUNA]    Soluna Music Protocol, https://solun.art/

---

## 14. Authors' Addresses

   Open Sonic Workgroup
   https://opensonic.io/

   Email: spec@opensonic.io

---

## Appendix A: Implementation Notes

### A.1. Jitter Buffer Sizing

   Recommended jitter buffer sizes by mode:

   | Mode            | Buffer Size | Rationale                         |
   |-----------------|------------|-----------------------------------|
   | LAN Multicast   | 20ms       | 1 frame, LAN is stable            |
   | P2P             | 60ms       | 3 frames, internet jitter         |
   | Relay (WiFi)    | 80ms       | 4 frames, WebSocket overhead      |
   | Relay (Cellular)| 200ms      | 10 frames, carrier jitter         |
   | Swarm Relay     | 60ms       | 3 frames, per-hop adds 1ms        |

   Adaptive jitter buffers SHOULD monitor jitter over a 5-second window
   and adjust size to the 99th percentile jitter + 2 frames.

### A.2. Relay Node Selection Heuristics

   In practice, the best relay nodes are:
   - Desktop machines with wired Ethernet connections
   - Nodes with upload_kbps ≥ 4× stream_kbps (headroom for fanout-3)
   - Nodes with low CPU load (< 30%)
   - Nodes geographically close to high-density listener clusters

   The SC SHOULD use GeoIP to cluster relay assignments: a relay node
   in Tokyo should serve children in Asia, not Europe.

### A.3. CRC-32 Reference Implementation (C)

   ```c
   static uint32_t crc32_table[256];
   static int crc32_initialized = 0;

   static void crc32_init(void) {
       for (int i = 0; i < 256; i++) {
           uint32_t c = i;
           for (int j = 0; j < 8; j++)
               c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
           crc32_table[i] = c;
       }
       crc32_initialized = 1;
   }

   uint32_t ostp_crc32(const uint8_t *buf, size_t len) {
       if (!crc32_initialized) crc32_init();
       uint32_t c = 0xFFFFFFFF;
       for (size_t i = 0; i < len; i++)
           c = crc32_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
       return c ^ 0xFFFFFFFF;
   }
   ```

### A.4. OSTP Extension Block Encoding (C)

   ```c
   typedef struct {
       uint16_t stream_id;   /* network byte order */
       uint16_t seq_ext;     /* network byte order */
       uint32_t media_ts;    /* network byte order */
   } ostp_ext_t;

   void ostp_ext_write(uint8_t *buf, uint16_t stream_id,
                       uint32_t seq32, uint32_t media_ts) {
       /* RTP extension header */
       buf[0] = 0x4F; buf[1] = 0x53; /* profile "OS" */
       buf[2] = 0x00; buf[3] = 0x02; /* length = 2 words */
       /* OSTP extension block */
       buf[4] = (stream_id >> 8) & 0xFF;
       buf[5] = stream_id & 0xFF;
       buf[6] = (seq32 >> 24) & 0xFF;
       buf[7] = (seq32 >> 16) & 0xFF;
       buf[8]  = (media_ts >> 24) & 0xFF;
       buf[9]  = (media_ts >> 16) & 0xFF;
       buf[10] = (media_ts >>  8) & 0xFF;
       buf[11] = media_ts & 0xFF;
   }
   ```

---

## Appendix B. Test Vectors

   This appendix provides normative test vectors for OSTP conformance
   testing. An implementation MUST produce byte-identical output for
   Vector 1 and Vector 2 (build direction), and MUST reject Vector 3
   (CRC corruption) with an error.

   Verified by: C++ solunad (reference), Python ostp.py (sdk/python/),
   JavaScript soluna.js (web/).

### B.1. Vector 1 — Opus Silence Frame

   Input parameters:
   - RTP: seq=1, ts=480 (10ms @ 48kHz), ssrc=0xDEADBEEF, PT=96, X=1
   - OSTP: stream_id=1, seq_ext=0, media_ts=480
   - Payload: Opus silence frame [F8 FF FE] (3 bytes)

   Expected wire bytes (31 bytes, hex):
   ```
   9060 0001 000001E0 DEADBEEF   ← RTP header (12 bytes)
   4F53 0002                     ← RTP ext header: profile='OS', len=2 words
   0001 0000 000001E0             ← OSTP ext: stream_id=1, seq_ext=0, media_ts=480
   F8FFFE                        ← Opus silence payload (3 bytes)
   17F498ED                      ← CRC-32 (little-endian)
   ```
   Full hex: `90600001000001E0DEADBEEF4F53000200010000000001E0F8FFFE17F498ED`

   CRC-32 preimage: `90600001000001E0DEADBEEF4F53000200010000000001E0F8FFFE`
   CRC-32 value: `0xED98F417` (stored little-endian as `17 F4 98 ED`)

### B.2. Vector 2 — Sequence Rollover + Multi-Channel

   Input parameters:
   - RTP: seq=65535 (0xFFFF, pre-rollover), ts=960000, ssrc=0x12345678, PT=96
   - OSTP: stream_id=0x0200 (channel bits=0 in high nibble, stream=512),
           seq_ext=1 (full_seq = 0x0001FFFF = 131071), media_ts=960000
   - Payload: 20 zero bytes (dummy stereo Opus frame)

   Expected wire bytes (48 bytes, hex):
   ```
   9060 FFFF 000EA600 12345678   ← RTP header
   4F53 0002                     ← RTP ext header
   0200 0001 000EA600             ← OSTP ext
   00000000000000000000000000000000 00000000   ← 20B payload
   8A4B68E6                      ← CRC-32 (LE)
   ```
   Full hex: `9060FFFF000EA600123456784F53000202000001000EA60000000000000000000000000000000000000000008A4B68E6`

### B.3. Vector 3 — CRC Corruption Detection

   Take Vector 1 and flip bit 7 of byte 24 (first payload byte):
   Original byte 24: `0xF8` → Corrupted: `0x07`

   A conforming parser MUST:
   - Detect the CRC mismatch
   - Return/raise an error (NOT silently pass the packet upstream)
   - Log: "CRC mismatch: stored=0xED98F417 calc=<different value>"

### B.4. Running the Test Suite

   Python (requires no external dependencies beyond stdlib):
   ```bash
   python3 sdk/python/ostp.py test
   # Expected output:
   #   PASS  Vector 1: Opus silence frame
   #   PASS  Vector 2: Seq rollover + multi-channel
   #   PASS  Vector 3: CRC corruption correctly detected
   #   3 passed, 0 failed
   ```

   JavaScript (Node.js 18+):
   ```bash
   node -e "
   import('./web/soluna.js').then(m => {
     // soluna.js exports are browser-oriented; for Node test use the
     // test-vectors endpoint of the demo relay
     console.log('soluna.js loaded OK');
   });
   "
   ```

---

## Appendix C. Channel Naming Grammar

   Channel names MUST conform to the following ABNF grammar (RFC 5234):

   ```abnf
   channel-name = 1*63(ALPHA / DIGIT / "-" / "_" / ".")
   ALPHA        = %x41-5A / %x61-7A   ; A-Z / a-z
   DIGIT        = %x30-39             ; 0-9
   ```

   Rules:
   - Minimum 1 character, maximum 63 characters
   - Case-SENSITIVE: "MyChannel" ≠ "mychannel"
   - MUST NOT begin or end with "-", "_", or "."
   - Unicode and percent-encoding are NOT permitted at the protocol level
     (application layer may apply a transformation before use)

   Examples of valid channel names:
   - `test`
   - `zamna-hawaii-2026`
   - `DJ_Yuki_set1`

   Examples of invalid channel names:
   - `-start` (begins with hyphen)
   - `my channel` (contains space)
   - `abc/def` (contains slash)
   - `` (empty)

---

*End of OSTP-SPEC.md*
*Revision: 0.9 DRAFT — Not for production deployment without review*
