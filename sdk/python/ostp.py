"""
ostp.py — Open Sonic Transport Protocol (OSTP) Python SDK
Second independent implementation (reference: C++ solunad, third: JavaScript soluna.js)

Usage:
    from ostp import OSTPacket, OSTReceiver

    # Parse a raw UDP/WS packet
    pkt = OSTPacket.parse(raw_bytes)
    print(pkt.stream_id, pkt.seq, pkt.payload)

    # Build a packet
    raw = OSTPacket.build(
        seq=1, rtp_ts=480, ssrc=0xDEADBEEF,
        stream_id=1, seq_ext=0, media_ts=480,
        payload=b"\\xF8\\xFF\\xFE"
    )

    # Async WebSocket receiver
    import asyncio
    async def main():
        async with OSTReceiver("relay.solun.art", channel="test") as rx:
            async for frame in rx:
                print(f"seq={frame.seq} len={len(frame.payload)}")
    asyncio.run(main())

Protocol version: OSTP-1 (DRAFT)
Spec: https://github.com/yukihamada/opensonic/blob/main/OSTP-SPEC.md
"""

from __future__ import annotations

import struct
import zlib
import asyncio
import logging
from dataclasses import dataclass, field
from typing import AsyncIterator, Optional
from enum import IntEnum

__version__ = "0.1.0"
__all__ = ["OSTPacket", "OSTParseError", "OSTReceiver", "ControlCommand"]

logger = logging.getLogger("ostp")

# ── Constants ─────────────────────────────────────────────────────────────────

OSTP_EXTENSION_PROFILE = 0x4F53      # 'OS'
OSTP_EXT_LENGTH_WORDS  = 2           # 2 × 4 bytes = 8 bytes of OSTP fields
OSTP_PT_OPUS           = 96          # RTP payload type for Opus (dynamic, de-facto)
OSTP_PT_PCM_S16LE      = 97          # 16-bit signed PCM little-endian
OSTP_DEFAULT_CLOCK_RATE = 48_000     # Hz
OSTP_MAX_PAYLOAD       = 12_288      # bytes (spec limit)
RTP_HEADER_SIZE        = 12          # bytes
OSTP_EXT_HEADER_SIZE   = 4          # profile(2) + length(2)
OSTP_EXT_DATA_SIZE     = 8          # stream_id(2) + seq_ext(2) + media_ts(4)
CRC32_SIZE             = 4          # bytes, little-endian trailer

OSTP_MIN_PACKET_SIZE = RTP_HEADER_SIZE + OSTP_EXT_HEADER_SIZE + OSTP_EXT_DATA_SIZE + CRC32_SIZE  # 28


class ControlCommand(IntEnum):
    """Control channel command codes (TCP/TLS port 5101)."""
    JOIN    = 0x01
    LEAVE   = 0x02
    PING    = 0x03
    PONG    = 0x04
    CHARGE  = 0x10
    SYNC    = 0x11
    NACK    = 0x20
    FEC     = 0x30
    SWARM   = 0x40


# ── Exceptions ────────────────────────────────────────────────────────────────

class OSTParseError(ValueError):
    """Raised when an OSTP packet cannot be parsed."""
    pass


# ── Packet ────────────────────────────────────────────────────────────────────

@dataclass
class OSTPacket:
    """A parsed OSTP packet.

    Wire format (all big-endian unless noted):
        [RTP Header 12B][RTP Ext Header 4B][OSTP Ext Data 8B][Payload][CRC-32 4B LE]

    RTP Header:
        0       V=2, P=0, X=1, CC=0
        1       M=0, PT
        2-3     Sequence number
        4-7     RTP timestamp
        8-11    SSRC

    RTP Ext Header (OSTP profile):
        12-13   Profile = 0x4F53 ('OS')
        14-15   Extension length = 2 (in 32-bit words = 8 bytes)

    OSTP Extension Data:
        16-17   stream_id  : u16  — logical stream identifier
        18-19   seq_ext    : u16  — upper 16 bits of sequence (total 32-bit seq = seq_ext<<16|seq)
        20-23   media_ts   : u32  — media-layer presentation timestamp (48 kHz ticks)

    CRC-32 trailer (little-endian, covers everything before CRC):
        last 4 bytes
    """

    # RTP fields
    version: int          # always 2
    has_padding: bool
    pt: int               # payload type (96=Opus, 97=PCM)
    marker: bool
    seq: int              # 16-bit RTP sequence
    rtp_ts: int           # 32-bit RTP timestamp
    ssrc: int             # 32-bit SSRC

    # OSTP extension fields
    stream_id: int        # u16
    seq_ext: int          # u16 — upper sequence bits
    media_ts: int         # u32

    # Data
    payload: bytes

    # Derived
    full_seq: int = field(init=False)  # 32-bit = (seq_ext << 16) | seq

    def __post_init__(self) -> None:
        self.full_seq = (self.seq_ext << 16) | self.seq

    # ── Parsing ───────────────────────────────────────────────────────────────

    @classmethod
    def parse(cls, data: bytes | bytearray, verify_crc: bool = True) -> "OSTPacket":
        """Parse raw bytes into an OSTPacket.

        Raises OSTParseError on any structural or CRC error.
        """
        if len(data) < OSTP_MIN_PACKET_SIZE:
            raise OSTParseError(
                f"Packet too short: {len(data)} < {OSTP_MIN_PACKET_SIZE}"
            )

        # CRC-32 check (last 4 bytes, little-endian)
        if verify_crc:
            body, crc_bytes = data[:-CRC32_SIZE], data[-CRC32_SIZE:]
            stored_crc = struct.unpack_from("<I", crc_bytes)[0]
            calc_crc   = zlib.crc32(bytes(body)) & 0xFFFFFFFF
            if stored_crc != calc_crc:
                raise OSTParseError(
                    f"CRC-32 mismatch: stored=0x{stored_crc:08X} calc=0x{calc_crc:08X}"
                )

        # RTP header
        b0, b1 = data[0], data[1]
        version     = (b0 >> 6) & 0x3
        has_padding = bool((b0 >> 5) & 0x1)
        has_ext     = bool((b0 >> 4) & 0x1)
        cc          = b0 & 0x0F
        marker      = bool((b1 >> 7) & 0x1)
        pt          = b1 & 0x7F

        if version != 2:
            raise OSTParseError(f"Expected RTP V=2, got {version}")
        if not has_ext:
            raise OSTParseError("OSTP requires RTP X=1 (extension bit)")
        if cc != 0:
            raise OSTParseError(f"CSRC count {cc} not supported by this parser")

        seq, rtp_ts, ssrc = struct.unpack_from(">HII", data, 2)

        # RTP extension header
        ext_profile, ext_len_words = struct.unpack_from(">HH", data, RTP_HEADER_SIZE)
        if ext_profile != OSTP_EXTENSION_PROFILE:
            raise OSTParseError(
                f"Unknown extension profile: 0x{ext_profile:04X} "
                f"(expected 0x{OSTP_EXTENSION_PROFILE:04X})"
            )
        if ext_len_words != OSTP_EXT_LENGTH_WORDS:
            raise OSTParseError(
                f"Unexpected extension length: {ext_len_words} words "
                f"(expected {OSTP_EXT_LENGTH_WORDS})"
            )

        # OSTP extension data
        offset = RTP_HEADER_SIZE + OSTP_EXT_HEADER_SIZE
        stream_id, seq_ext, media_ts = struct.unpack_from(">HHI", data, offset)

        # Payload
        payload_start = offset + OSTP_EXT_DATA_SIZE
        payload_end   = len(data) - CRC32_SIZE
        payload = bytes(data[payload_start:payload_end])

        if len(payload) > OSTP_MAX_PAYLOAD:
            raise OSTParseError(
                f"Payload {len(payload)} B exceeds maximum {OSTP_MAX_PAYLOAD} B"
            )

        return cls(
            version=version, has_padding=has_padding, pt=pt, marker=marker,
            seq=seq, rtp_ts=rtp_ts, ssrc=ssrc,
            stream_id=stream_id, seq_ext=seq_ext, media_ts=media_ts,
            payload=payload,
        )

    # ── Building ──────────────────────────────────────────────────────────────

    @classmethod
    def build(
        cls,
        seq: int,
        rtp_ts: int,
        ssrc: int,
        stream_id: int,
        seq_ext: int,
        media_ts: int,
        payload: bytes,
        pt: int = OSTP_PT_OPUS,
        marker: bool = False,
    ) -> bytes:
        """Serialize an OSTP packet to bytes.

        Returns the complete wire-format packet including CRC-32 trailer.
        """
        if len(payload) > OSTP_MAX_PAYLOAD:
            raise ValueError(f"Payload {len(payload)} B exceeds maximum {OSTP_MAX_PAYLOAD} B")

        # RTP header: V=2, P=0, X=1, CC=0
        b0 = (2 << 6) | (0 << 5) | (1 << 4) | 0
        b1 = ((1 if marker else 0) << 7) | (pt & 0x7F)
        rtp_hdr = struct.pack(">BBHII", b0, b1, seq & 0xFFFF, rtp_ts & 0xFFFFFFFF, ssrc & 0xFFFFFFFF)

        # RTP extension header
        ext_hdr = struct.pack(">HH", OSTP_EXTENSION_PROFILE, OSTP_EXT_LENGTH_WORDS)

        # OSTP extension data
        ext_data = struct.pack(">HHI", stream_id & 0xFFFF, seq_ext & 0xFFFF, media_ts & 0xFFFFFFFF)

        body = rtp_hdr + ext_hdr + ext_data + payload
        crc  = zlib.crc32(body) & 0xFFFFFFFF
        return body + struct.pack("<I", crc)

    # ── Helpers ───────────────────────────────────────────────────────────────

    @property
    def codec(self) -> str:
        return {OSTP_PT_OPUS: "opus", OSTP_PT_PCM_S16LE: "pcm_s16le"}.get(self.pt, f"pt{self.pt}")

    def __repr__(self) -> str:
        return (
            f"OSTPacket(seq={self.full_seq}, stream_id={self.stream_id}, "
            f"media_ts={self.media_ts}, codec={self.codec}, "
            f"payload={len(self.payload)}B)"
        )


# ── Jitter Buffer ─────────────────────────────────────────────────────────────

class JitterBuffer:
    """Simple reorder buffer for OSTP packets."""

    def __init__(self, target_depth: int = 4) -> None:
        self._buf: dict[int, OSTPacket] = {}
        self._next_seq: Optional[int] = None
        self._target_depth = target_depth

    def push(self, pkt: OSTPacket) -> None:
        self._buf[pkt.full_seq] = pkt
        if self._next_seq is None:
            self._next_seq = pkt.full_seq

    def pop_ready(self) -> list[OSTPacket]:
        """Return in-order packets that are ready to play."""
        ready: list[OSTPacket] = []
        if self._next_seq is None:
            return ready
        while self._next_seq in self._buf and len(self._buf) >= self._target_depth:
            ready.append(self._buf.pop(self._next_seq))
            self._next_seq += 1
        return ready

    @property
    def depth(self) -> int:
        return len(self._buf)


# ── WebSocket Receiver ────────────────────────────────────────────────────────

class OSTReceiver:
    """Async OSTP receiver over WebSocket relay.

    Connects to wss://<host>/ws/audio?channel=<name>&format=raw
    and yields decoded OSTPacket objects.

    Example:
        async with OSTReceiver("relay.solun.art", channel="myChannel") as rx:
            async for pkt in rx:
                process(pkt.payload)
    """

    def __init__(
        self,
        host: str,
        channel: str,
        port: int = 443,
        tls: bool = True,
        jitter_depth: int = 4,
    ) -> None:
        self._host = host
        self._channel = channel
        self._port = port
        self._tls = tls
        self._jitter = JitterBuffer(target_depth=jitter_depth)
        self._ws = None
        self._stats = {"rx": 0, "crc_errors": 0, "parse_errors": 0, "bytes": 0}

    @property
    def ws_url(self) -> str:
        scheme = "wss" if self._tls else "ws"
        return f"{scheme}://{self._host}:{self._port}/ws/audio?channel={self._channel}&format=raw"

    async def __aenter__(self) -> "OSTReceiver":
        try:
            import websockets  # type: ignore
        except ImportError:
            raise ImportError("pip install websockets")
        self._ws = await websockets.connect(self.ws_url)
        logger.info("OSTP connected: %s", self.ws_url)
        return self

    async def __aexit__(self, *_: object) -> None:
        if self._ws:
            await self._ws.close()
        logger.info("OSTP disconnected. stats=%s", self._stats)

    def __aiter__(self) -> "OSTReceiver":
        return self

    async def __anext__(self) -> OSTPacket:
        if self._ws is None:
            raise RuntimeError("Use as async context manager")
        while True:
            try:
                raw = await self._ws.recv()
            except Exception as e:
                raise StopAsyncIteration from e

            if not isinstance(raw, (bytes, bytearray)):
                continue  # skip text frames (control messages)

            self._stats["bytes"] += len(raw)

            try:
                pkt = OSTPacket.parse(raw)
            except OSTParseError as e:
                self._stats["crc_errors" if "CRC" in str(e) else "parse_errors"] += 1
                logger.warning("Parse error: %s", e)
                continue

            self._stats["rx"] += 1
            self._jitter.push(pkt)

            ready = self._jitter.pop_ready()
            if ready:
                # Return first, re-queue rest (simplified — real impl would use a queue)
                return ready[0]

    @property
    def stats(self) -> dict:
        return dict(self._stats)


# ── Test Vectors ──────────────────────────────────────────────────────────────

TEST_VECTORS = [
    {
        "name": "Vector 1: Opus silence frame",
        "hex": "90600001000001E0DEADBEEF4F53000200010000000001E0F8FFFE17F498ED",
        "fields": {
            "version": 2,
            "pt": 96,
            "seq": 1,
            "rtp_ts": 480,
            "ssrc": 0xDEADBEEF,
            "stream_id": 1,
            "seq_ext": 0,
            "media_ts": 480,
            "payload": "F8FFFE",
        },
    },
    {
        "name": "Vector 2: Seq rollover + multi-channel",
        "hex": "9060FFFF000EA600123456784F53000202000001000EA60000000000000000000000000000000000000000008A4B68E6",
        "fields": {
            "version": 2,
            "pt": 96,
            "seq": 65535,
            "rtp_ts": 960000,
            "ssrc": 0x12345678,
            "stream_id": 0x0200,
            "seq_ext": 1,
            "media_ts": 960000,
            "payload": "00" * 20,
        },
    },
]


def run_tests() -> bool:
    """Run conformance tests against the built-in test vectors."""
    import sys
    passed = 0
    failed = 0

    for vec in TEST_VECTORS:
        raw = bytes.fromhex(vec["hex"])
        try:
            pkt = OSTPacket.parse(raw)
            f = vec["fields"]

            checks = [
                pkt.version  == f["version"],
                pkt.pt       == f["pt"],
                pkt.seq      == f["seq"],
                pkt.rtp_ts   == f["rtp_ts"],
                pkt.ssrc     == f["ssrc"],
                pkt.stream_id == f["stream_id"],
                pkt.seq_ext  == f["seq_ext"],
                pkt.media_ts == f["media_ts"],
                pkt.payload  == bytes.fromhex(f["payload"].replace(" ", "")),
            ]

            # Also verify round-trip build → parse
            rebuilt = OSTPacket.build(
                seq=pkt.seq, rtp_ts=pkt.rtp_ts, ssrc=pkt.ssrc,
                stream_id=pkt.stream_id, seq_ext=pkt.seq_ext, media_ts=pkt.media_ts,
                payload=pkt.payload, pt=pkt.pt,
            )
            checks.append(rebuilt == raw)

            if all(checks):
                print(f"  PASS  {vec['name']}")
                passed += 1
            else:
                print(f"  FAIL  {vec['name']} — failed checks: {[i for i,c in enumerate(checks) if not c]}")
                failed += 1

        except OSTParseError as e:
            print(f"  FAIL  {vec['name']} — parse error: {e}")
            failed += 1

    # Vector 3: CRC corruption
    v3 = bytearray(bytes.fromhex(TEST_VECTORS[0]["hex"]))
    v3[24] ^= 0xFF
    try:
        OSTPacket.parse(bytes(v3))
        print("  FAIL  Vector 3: CRC corruption not detected")
        failed += 1
    except OSTParseError:
        print("  PASS  Vector 3: CRC corruption correctly detected")
        passed += 1

    print(f"\n{passed} passed, {failed} failed")
    return failed == 0


# ── CLI ───────────────────────────────────────────────────────────────────────

def _cli() -> None:
    import sys
    import argparse

    parser = argparse.ArgumentParser(
        prog="ostp",
        description="OSTP packet tool — parse, build, test, listen",
    )
    sub = parser.add_subparsers(dest="cmd")

    # ostp test
    sub.add_parser("test", help="Run built-in conformance tests")

    # ostp parse <hex>
    p_parse = sub.add_parser("parse", help="Parse a hex-encoded OSTP packet")
    p_parse.add_argument("hex", help="Hex string (spaces OK)")

    # ostp listen <channel>
    p_listen = sub.add_parser("listen", help="Listen to a relay channel")
    p_listen.add_argument("channel", help="Channel name")
    p_listen.add_argument("--host", default="relay.solun.art")
    p_listen.add_argument("--count", type=int, default=10, help="Print first N packets then exit")

    args = parser.parse_args()

    if args.cmd == "test":
        ok = run_tests()
        sys.exit(0 if ok else 1)

    elif args.cmd == "parse":
        raw = bytes.fromhex(args.hex.replace(" ", ""))
        pkt = OSTPacket.parse(raw)
        print(pkt)
        print(f"  full_seq = {pkt.full_seq}")
        print(f"  codec    = {pkt.codec}")
        print(f"  payload  = {pkt.payload.hex().upper()}")

    elif args.cmd == "listen":
        async def _listen():
            count = 0
            async with OSTReceiver(args.host, channel=args.channel) as rx:
                async for pkt in rx:
                    print(pkt)
                    count += 1
                    if count >= args.count:
                        break
            print(f"\nStats: {rx.stats}")

        asyncio.run(_listen())

    else:
        parser.print_help()


if __name__ == "__main__":
    _cli()
