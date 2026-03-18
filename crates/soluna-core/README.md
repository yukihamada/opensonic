# soluna-core

Ultra-simple Rust SDK for real-time P2P audio over OSTP/RTP.

## Quick Start

```toml
# Cargo.toml
[dependencies]
soluna-core = { path = "../soluna-core" }
```

### 3 lines to join a mesh

```rust
use soluna_core::easy::Node;

fn main() {
    // Join a channel — zero config, zero server
    let node = Node::join("my-jam-session").unwrap();

    // Audio callback (real-time safe, lock-free)
    loop {
        // Send your mic audio
        let mic_samples: Vec<f32> = capture_audio(); // your audio capture
        node.send_audio(&mic_samples);

        // Receive everyone else's audio
        let mut out = [0.0f32; 256];
        let n = node.recv_audio(&mut out);
        play_audio(&out[..n]); // your audio output
    }
}
```

### Guitar streaming (Babyface Pro → mesh)

```rust
use soluna_core::easy::{Node, Config};

let config = Config {
    sample_rate: 48000,
    samples_per_packet: 96,  // 2ms packets (WiFi tier, lowest latency)
    ..Default::default()
};

let node = Node::join_with_config("guitar-jam", config)?;

// In your audio callback:
node.send_audio(&guitar_samples);
```

### Festival mode (receive from STAGE)

```rust
let node = Node::join("main-stage")?;

loop {
    let mut buf = [0.0f32; 480]; // 10ms @ 48kHz
    let n = node.recv_audio(&mut buf);
    if n > 0 {
        output_to_speaker(&buf[..n]);
    }
}
```

## API

```rust
// Join with defaults (48kHz, mono, 239.69.0.1:5004)
let node = Node::join("channel-name")?;

// Send audio (f32, any buffer size, auto-chunked to packets)
node.send_audio(&samples);

// Receive audio (lock-free, real-time safe)
let n = node.recv_audio(&mut buffer);

// Check available samples
let available = node.available();

// Leave
node.leave();
```

## Design Principles

- **Zero-copy audio path**: Lock-free SPSC ring buffer between network and audio threads
- **Zero-config**: Channel name is the only required parameter
- **Zero-server**: UDP multicast, no central server needed (LAN)
- **OSTP/RTP compatible**: Works with all OpenSonic devices (Mac app, iOS app, ESP32)
- **Real-time safe**: `send_audio()` and `recv_audio()` never allocate, never lock

## Protocol

OSTP (OpenSonic Transport Protocol) over RTP:
- Multicast: `239.69.0.1:5004`
- Payload: f32 LE or s24 LE or s16 BE (AES67 compat)
- Header: RTP (12B) + OSTP extension (12B)
- Packet tiers: 125μs (Ultra) to 10ms (Robust)

## Performance

| Metric | Value |
|--------|-------|
| Latency (LAN) | ~5ms (240 samples @ 48kHz) |
| Latency (WiFi) | ~10ms (480 samples) |
| CPU per stream | ~0.1% (no encoding) |
| Bandwidth | 192 kbps (mono f32) / 32 kbps (Opus) |
| Max peers | Unlimited (multicast) |
