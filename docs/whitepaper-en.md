# Soluna: The Power Grid for Music

**Transparent Music Distribution via Open Sonic Transport Protocol (OSTP) and Subscription Model**

Inebra Inc.
March 19, 2026 — v0.11.0

---

## Abstract

The music industry is broken. Spotify pays artists $0.003 per stream. Listeners own nothing. Play counts are self-reported and easily gamed.

Soluna solves all three problems simultaneously:

1. **Proof of Listen (PoL)** — cryptographically verifiable proof of playback
2. **Relay Network** — P2P audio distribution network
3. **Fan Rank** — the more you listen, the higher your rank and the more perks you unlock

These are implemented at the **protocol layer**, not the application layer. Like a power grid connects generators to homes, Soluna is protocol infrastructure for music.

---

## 1. The Problem

### Play counts lie

Spotify's play counts are reported by Spotify's servers. There is no third-party verification. Bot farm fraud is estimated at $1 billion annually.

### Artists are exploited

~70% of streaming revenue is absorbed by labels and platforms. An independent artist needs 1 million monthly streams to earn a living wage.

### Revenue distribution is opaque

The basis for payments to artists is opaque and relies on platform self-reporting. There is no mechanism for third-party auditing.

---

## 2. The Solution

### 2.1 OSTP — Rules embedded in audio packets

OSTP (Open Sonic Transport Protocol) extends RTP [RFC 3550] as a UDP/QUIC-based audio distribution protocol:

- **24-byte header**: RTP(12B) + Extension(4B) + OSTP(8B) with stream_id, sequence, media_timestamp
- **LAN**: UDP multicast (zero additional latency)
- **WAN**: QUIC Unreliable Datagram [RFC 9221] (TLS 1.3 encryption, connection migration)
- **Scale**: P2P Swarm to 3 billion listeners

### 2.2 Proof of Listen

The listener's device builds a hash chain from received packets:

```
genesis = SHA-256("soluna:pol:v1:" || channel_name)
tip[n]  = SHA-256(tip[n-1] || seq || timestamp || ssrc || crc)
```

Periodically, a Merkle Root is generated and recorded on the blockchain. The artist builds the same chain from the sender side. Matching roots prove the listener received the authentic stream — verifiable by any third party. This makes **play count fraud impossible** and provides a transparent basis for subscription revenue distribution.

### 2.3 Relay Network

Nodes that forward audio packets form a P2P distribution network:

| Tier | Role |
|------|------|
| Origin | Artist/broadcaster source |
| Region | Inter-region relay |
| Edge | Local area distribution |
| P2P Swarm | Direct listener-to-listener relay |

A Koe device ($65) connected to Wi-Fi becomes an Edge node, contributing to network expansion. Relay contributions are recorded as in-app ENAI points.

### 2.4 Fan Rank

Based on PoL-verified listen counts, a listener's Fan Rank rises:

```
Rank            Cumulative Listens   Perks
Beginner        0+                   Basic listening
Regular         10+                  Exclusive content access
Core Fan        100+                 Early access, exclusive chat
Super Fan       1,000+               Backstage footage, credits
Legend          10,000+              Direct artist interaction
```

**Fans who listened early earn higher ranks.** Fan Rank is not a financial return — it deepens the relationship between fans and artists.

---

## 3. Economics

### Subscription Model

| Plan | Price | Features |
|------|-------|----------|
| Free | $0 | Ad-supported, 20 hours/month |
| Pro | ¥500/month (~$3.50) | Ad-free, high quality, unlimited |
| Studio | ¥2,000/month (~$14) | All Pro features + artist distribution tools + analytics |

### Artist Revenue Distribution

The subscription revenue pool is distributed based on PoL-verified play count ratios:

```
Total Revenue Pool = Total Subscription Revenue × 70%
Artist A's Payment = Total Pool × (A's Verified Plays / Total Verified Plays)
```

- **Artists**: 70% (distributed by PoL-verified play ratio)
- **Platform operations**: 20%
- **Network maintenance**: 10%

While traditional platforms give artists ~30%, Soluna returns **70% directly to artists**.

### ENAI Points

ENAI is an in-app point, not a cryptocurrency or token.

- **How to earn**: Listening time, relay network contribution, community activity
- **How to use**: Unlock exclusive content, boost Fan Rank, tip artists
- **Restrictions**: Cannot be sent externally, cannot be cashed out, no monetary value

### Blockchain Role: Transparency Only

Blockchain is used solely to guarantee transparency of artist payments:

- Public record of PoL Merkle Roots (third-party verification of play counts)
- Public audit log of revenue distribution reports
- Payment trail verifiable by anyone

**No token issuance, trading, or NFT sales occur on the blockchain.**

### Value Circulation

```
Listener → Listen → PoL → Fan Rank rises
            ↓
          Subscription revenue → Distributed by play ratio → Artist payment
            ↓
          Relay Node → Forward → Network expansion
            ↓
          Value circulates to everyone
```

---

## 4. Technical Architecture

```
┌──────────────────────────────────────────┐
│           OSTP Protocol Layer            │
├──────────┬───────────┬───────────────────┤
│ LAN      │ WAN       │ Transparency      │
│ UDP      │ QUIC      │ Blockchain        │
│ Multicast│ Datagram  │ PoL Audit Log     │
├──────────┴───────────┴───────────────────┤
│           Swarm Intelligence             │
│  Adaptive Bitrate / Transport / Routing  │
├──────────────────────────────────────────┤
│           Hardware (Koe Device)          │
│  ESP32-S3 / Raspberry Pi / Desktop      │
└──────────────────────────────────────────┘
```

### On-chain Functions (Transparency Audit Only)

1. `SubmitProof` — PoL Merkle Root submission (public record of playback proofs)
2. `PublishDistribution` — Revenue distribution report publication (verifiable by anyone)

---

## 5. Why Now

- **QUIC** [RFC 9000] Unreliable Datagram extension was standardized in 2024, enabling unified UDP/QUIC protocol design for the first time
- Music industry **streaming revenue distribution** discontent has reached a tipping point
- Blockchain transparency technology has matured, enabling low-cost **payment auditing**

---

## 6. Roadmap

| Phase | Content | Status |
|-------|---------|--------|
| P0 | OST Protocol + C++/Rust implementation | Done |
| P1 | QUIC Transport + PoL | Done |
| P2 | Fan Rank + Subscription infrastructure | Done |
| P3 | TestFlight + Beta testing | Done |
| P4 | iOS/Android release + Subscription payments launch | Next |
| P5 | Koe device mass production + festival proof | Planned |
| **ZAMNA Hawaii** | **2026.9.4 — Festival proof-of-concept event** | **Planned** |

---

## 7. Conclusion

Soluna is the power grid for music. Generation (artists), transmission (relay nodes), and metering (PoL) are implemented at the protocol layer, distributing subscription revenue transparently based on verified play counts.

The more you listen, the higher your Fan Rank. The more you relay, the stronger the network. The more you create, the more fairly you are rewarded.

This is how music was always meant to work.

---

*Inebra Inc.*
*OSTP Specification: https://solun.art/protocol*
*Source Code: https://github.com/yukihamada/opensonic (MIT License)*
*TestFlight: https://testflight.apple.com/join/PYbefDSE*
