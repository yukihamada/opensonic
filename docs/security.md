# OSTP Security Analysis

**Status**: DRAFT — Version 0.1, 2026-03-14
**Scope**: Threat model for OSTP-1 relay deployment (solun.art)

---

## 1. Attack Surface Summary

```
[Sender / solunad]
        │ UDP 5100 (OSTP)
        ▼
[Relay: relay.solun.art]  ←── TCP 5101 (Control / WebSocket)
        │                 ←── HTTPS /metrics, /c/<channel>
        ▼
[Receivers: iOS / Web / Linux]
```

Three trust boundaries:
1. Sender → Relay (UDP, unauthenticated in current production)
2. Relay → Receivers (WebSocket, TLS)
3. Control commands (TCP 5101, HMAC-SHA256 on CHARGE only)

---

## 2. Threat Model

### T1 — Packet Injection (MEDIUM)
**Attack**: Attacker sends crafted OSTP packets to the relay on port 5100,
            injecting audio into an active channel.
**Mitigation today**: None (UDP is connectionless and source-spoofable).
**Recommended**: Sender authentication via HMAC-SHA256 on the RTP SSRC +
                 seq in the extension header. Channel "ownership" token
                 issued on JOIN (TCP 5101).
**Status**: UNMITIGATED in production.

### T2 — Channel Squatting (LOW-MEDIUM)
**Attack**: Attacker JOINs a channel name before the legitimate sender,
            pre-occupying it.
**Mitigation today**: First JOIN wins; no reservation system.
**Recommended**: Optional channel passwords passed in JOIN command.
                 For private channels: HMAC(channel_name, secret) as
                 the actual channel identifier never sent in plaintext.
**Status**: UNMITIGATED in production.

### T3 — CRC Forgery (LOW)
**Attack**: Attacker modifies payload and recomputes CRC-32, passing
            it to receivers as legitimate audio.
**Note**: CRC-32 is an integrity check, NOT a MAC. It detects accidental
          corruption but not malicious tampering.
**Mitigation today**: For privacy-sensitive channels, DTLS-SRTP
                      (--dtls-cert / --dtls-key flags) encrypts the
                      entire OSTP packet.
**Status**: MITIGATED when DTLS-SRTP is enabled; UNMITIGATED otherwise.

### T4 — Replay Attack (LOW)
**Attack**: Attacker records a session and replays old OSTP packets.
**Impact**: Receivers hear replayed audio; jitter buffer may accept it
            if seq/ts are plausible.
**Mitigation today**: None in protocol. Jitter buffers typically reject
                      packets with out-of-range timestamps.
**Recommended**: DTLS-SRTP replay protection (RFC 3711 §3.3) or a
                 short-window sequence number dedup cache on the relay.
**Status**: PARTIALLY MITIGATED by jitter buffer heuristics.

### T5 — CHARGE Command Replay (HIGH for economic layer)
**Attack**: Replay a captured HMAC-authenticated CHARGE command to
            double-charge a listener.
**Mitigation today**: HMAC-SHA256 is required on CHARGE, but there is
                      no nonce or timestamp in the current wire format,
                      making replay possible if the connection is MITM'd.
**Recommended**: Add a u64 monotonic nonce field to CHARGE. Server
                 rejects nonces ≤ last_seen_nonce per wallet.
**Status**: UNMITIGATED — economic layer is pre-production anyway.

### T6 — Relay Amplification / DoS (MEDIUM)
**Attack**: Attacker sends small OSTP packets to the relay that are
            forwarded to many receivers (amplification).
**Impact**: Relay becomes an amplifier; legitimate receivers flooded.
**Mitigation today**: Rate limiting per source IP (relay has basic
                      per-IP packet rate limiting in production).
**Recommended**: Sender rate limits enforced per SSRC, channel-level
                 sender verification.
**Status**: PARTIALLY MITIGATED.

### T7 — Metrics Endpoint Information Leak (LOW)
**Attack**: `/metrics` endpoint exposes channel names, SSRC values,
            wallet addresses, and packet counts publicly.
**Recommended**: Restrict `/metrics` to internal network or add
                 Bearer token auth.
**Status**: UNMITIGATED — metrics are intentionally public for now.

---

## 3. DTLS-SRTP Mode

When `--dtls-cert` and `--dtls-key` are provided to the relay, all
OSTP packets are wrapped in DTLS-SRTP (RFC 5764). This provides:

- **Confidentiality**: AES-128-GCM payload encryption
- **Integrity**: SRTP authentication tag (replaces CRC-32 in secure mode)
- **Replay protection**: SRTP sequence window (64-packet window)

Production deployment (`relay.solun.art`) does NOT currently use
DTLS-SRTP. Enabling it requires distributing the relay TLS certificate
to all senders and receivers, which is not yet implemented in solunad
or the iOS app.

---

## 4. Recommended Mitigations by Priority

| Priority | Mitigation | Effort | Impact |
|----------|-----------|--------|--------|
| P0 | Sender auth token on JOIN (TCP 5101) | Medium | Fixes T1, T2 |
| P1 | Nonce in CHARGE command | Low | Fixes T5 |
| P2 | `/metrics` auth | Low | Fixes T7 |
| P3 | DTLS-SRTP in production | High | Fixes T3, T4 |
| P4 | Channel passwords | Medium | Improves T2 |

---

## 5. Out of Scope

- Application-layer DRM (handled by rights holder integration, not OSTP)
- Solana wallet security (standard Web3 threat model applies)
- iOS/macOS app sandbox (handled by OS and App Store review)

---

*This document is maintained by the Open Sonic Workgroup.
Last updated: 2026-03-14*
