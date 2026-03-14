# ostp.py — OSTP Python SDK

Third independent implementation of the Open Sonic Transport Protocol.

## Install

No external dependencies for packet parsing/building (stdlib only).
For the async `OSTReceiver` (WebSocket listener):

```bash
pip install websockets
```

## Quick Start

```python
from ostp import OSTPacket

# Parse a raw packet
pkt = OSTPacket.parse(bytes.fromhex("90600001000001E0DEADBEEF..."))
print(pkt.stream_id, pkt.seq, pkt.payload.hex())

# Build a packet
raw = OSTPacket.build(
    seq=1, rtp_ts=480, ssrc=0xDEADBEEF,
    stream_id=1, seq_ext=0, media_ts=480,
    payload=b"\xF8\xFF\xFE"  # Opus silence
)

# Listen to a relay channel
import asyncio
async def main():
    async with OSTReceiver("relay.solun.art", channel="test") as rx:
        async for pkt in rx:
            print(pkt)
asyncio.run(main())
```

## CLI

```bash
# Run conformance tests
python3 ostp.py test

# Parse a hex packet
python3 ostp.py parse 90600001000001E0DEADBEEF4F53000200010000000001E0F8FFFE17F498ED

# Listen to a channel (prints first 10 packets)
python3 ostp.py listen zamna --count 10
```

## Spec

[OSTP-SPEC.md](../../OSTP-SPEC.md) — Full RFC-style specification
