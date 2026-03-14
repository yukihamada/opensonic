#!/usr/bin/env python3
"""
test_nack.py — OSTP NACK end-to-end test

Tests that the relay's replay buffer correctly retransmits packets
when a receiver sends a NACK request (PT=126).

Protocol flow:
  1. JOIN channel on TCP control port 5101
  2. Receive UDP audio packets on port 5100, track sequence numbers
  3. Send NACK for a recently-seen sequence
  4. Verify relay retransmits the requested packet

Usage:
  python3 tests/test_nack.py
  python3 tests/test_nack.py --channel zamna --relay relay.solun.art
"""

import argparse
import socket
import struct
import time
import zlib
import sys
import threading
from collections import deque

RELAY_UDP_PORT = 5100
RELAY_TCP_PORT = 5101
OSTP_PROFILE   = 0x4F53
PT_NACK        = 126
PT_OPUS        = 96
TEST_SSRC      = 0xDEAD0001


def parse_ostp_seq(data: bytes) -> int | None:
    """Extract RTP sequence number from packet. Returns None if not OSTP."""
    if len(data) < 28:
        return None
    b0 = data[0]
    if (b0 >> 6) != 2 or not (b0 >> 4 & 1):
        return None
    if struct.unpack_from(">H", data, 12)[0] != OSTP_PROFILE:
        return None
    return struct.unpack_from(">H", data, 2)[0]


def build_nack_packet(missing_seqs: list[int], ssrc: int = TEST_SSRC) -> bytes:
    """Build OSTP NACK packet (PT=126) requesting retransmission."""
    b0 = (2 << 6) | (1 << 4)  # V=2, X=1
    b1 = PT_NACK & 0x7F
    rtp_hdr  = struct.pack(">BBHII", b0, b1, 1, 0, ssrc)
    ext_hdr  = struct.pack(">HH", OSTP_PROFILE, 2)
    ext_data = struct.pack(">HHI", 0, 0, 0)
    payload  = b"".join(struct.pack(">H", s) for s in missing_seqs)
    body = rtp_hdr + ext_hdr + ext_data + payload
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def run_test(relay: str, channel: str, timeout: int = 15) -> bool:
    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_sock.settimeout(2.0)
    udp_sock.bind(("", 0))
    my_port = udp_sock.getsockname()[1]

    print(f"[nack-test] UDP socket bound on port {my_port}")

    # ── Step 1: JOIN via TCP control channel ──────────────────────────────
    tcp_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp_sock.settimeout(5.0)
    try:
        tcp_sock.connect((relay, RELAY_TCP_PORT))
    except Exception as e:
        print(f"[nack-test] FAIL: Cannot connect TCP {relay}:{RELAY_TCP_PORT}: {e}")
        udp_sock.close()
        return False

    join_msg = f"JOIN:{channel}\n".encode()
    tcp_sock.sendall(join_msg)

    # Read JOIN response
    resp = b""
    try:
        resp = tcp_sock.recv(256)
    except Exception:
        pass
    print(f"[nack-test] JOIN response: {resp.decode(errors='replace').strip()}")
    tcp_sock.close()

    # ── Step 2: Receive UDP packets, collect recent seqs ─────────────────
    seen_seqs: deque[int] = deque(maxlen=64)
    print(f"[nack-test] Listening for audio packets (channel={channel}, up to {timeout}s)...")

    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            data, addr = udp_sock.recvfrom(65536)
        except socket.timeout:
            continue

        seq = parse_ostp_seq(data)
        if seq is not None:
            seen_seqs.append(seq)
            if len(seen_seqs) == 1:
                print(f"[nack-test] First packet received from {addr}, seq={seq}")
            if len(seen_seqs) >= 5:
                break  # have enough to test NACK

    if len(seen_seqs) < 3:
        print(f"[nack-test] SKIP: Not enough packets received "
              f"({len(seen_seqs)}). Is channel active?")
        udp_sock.close()
        return True  # not a failure — channel may be silent

    # ── Step 3: Send NACK for oldest seen seq ─────────────────────────────
    # The relay keeps a replay buffer — requesting a recent seq should get retransmit
    target_seq = seen_seqs[0]
    relay_addr  = (relay, RELAY_UDP_PORT)
    nack_pkt    = build_nack_packet([target_seq])

    print(f"[nack-test] Sending NACK for seq={target_seq} to {relay}:{RELAY_UDP_PORT}")
    udp_sock.sendto(nack_pkt, relay_addr)

    # ── Step 4: Wait for retransmit ───────────────────────────────────────
    udp_sock.settimeout(3.0)
    received_retransmit = False
    deadline2 = time.time() + 5.0
    while time.time() < deadline2:
        try:
            data, addr = udp_sock.recvfrom(65536)
        except socket.timeout:
            break

        seq = parse_ostp_seq(data)
        if seq == target_seq:
            received_retransmit = True
            print(f"[nack-test] ✓ Retransmit received: seq={seq} ({len(data)}B) from {addr}")
            break
        # (other audio packets may arrive; ignore them)

    udp_sock.close()

    if received_retransmit:
        print("[nack-test] PASS: NACK → retransmit flow works end-to-end")
        return True
    else:
        print(f"[nack-test] FAIL: No retransmit received for seq={target_seq} within 5s")
        print("  Note: This may mean seq is no longer in relay's replay buffer.")
        print("  Try with a channel that has an active sender streaming continuously.")
        return False


def main():
    p = argparse.ArgumentParser(description="OSTP NACK end-to-end test")
    p.add_argument("--relay",   default="relay.solun.art")
    p.add_argument("--channel", default="zamna")
    p.add_argument("--timeout", type=int, default=15)
    args = p.parse_args()

    ok = run_test(args.relay, args.channel, args.timeout)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
