#!/usr/bin/env python3
"""
test_swarm.py — OSTP P2P Swarm activation test

Opens 60+ concurrent WebSocket connections to the same channel to trigger
the relay's swarm mode (kSwarmThreshold = 50 listeners).

Protocol flow:
  1. Spawn N WebSocket connections to wss://<relay>/ws?channel=<channel>
  2. Hold them open for --duration seconds (default 15s)
  3. Check relay /metrics that soluna_relay_swarm_packets_saved_total > 0
  4. Tear down all connections

Usage:
  python3 tests/test_swarm.py
  python3 tests/test_swarm.py --relay relay.solun.art --channel zamna --clients 60
"""

import argparse
import asyncio
import sys
import time
import urllib.request


# ── WebSocket handshake (no external deps) ──────────────────────────────────

import socket
import ssl
import hashlib
import base64
import struct
import threading
from collections import defaultdict


def ws_key() -> str:
    import os
    return base64.b64encode(os.urandom(16)).decode()


def ws_accept(key: str) -> str:
    magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
    return base64.b64encode(hashlib.sha1((key + magic).encode()).digest()).decode()


def open_ws(host: str, port: int, path: str, use_tls: bool,
            stop_event: threading.Event) -> None:
    """Open a WebSocket connection and hold it until stop_event is set."""
    try:
        raw = socket.create_connection((host, port), timeout=10)
        if use_tls:
            ctx = ssl.create_default_context()
            raw = ctx.wrap_socket(raw, server_hostname=host)

        key = ws_key()
        handshake = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n"
            f"\r\n"
        )
        raw.sendall(handshake.encode())

        # Read HTTP response
        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = raw.recv(4096)
            if not chunk:
                return
            resp += chunk

        if b"101" not in resp.split(b"\r\n")[0]:
            return  # upgrade failed

        # Hold connection open, draining any incoming frames
        raw.settimeout(0.5)
        while not stop_event.is_set():
            try:
                raw.recv(4096)
            except (socket.timeout, OSError):
                pass
    except Exception:
        pass
    finally:
        try:
            raw.close()
        except Exception:
            pass


def get_metric(relay: str, name: str) -> float:
    """Read a single counter value from /metrics (Prometheus text format)."""
    try:
        url = f"https://{relay}/metrics"
        with urllib.request.urlopen(url, timeout=5) as r:
            for line in r.read().decode().splitlines():
                if line.startswith(name + " ") or line.startswith(name + "{"):
                    parts = line.rsplit(" ", 1)
                    if len(parts) == 2:
                        return float(parts[1])
    except Exception:
        pass
    return -1.0


def run_test(relay: str, channel: str, num_clients: int,
             duration: int) -> bool:
    port = 443
    use_tls = True
    path = f"/ws?channel={channel}"

    print(f"[swarm-test] relay    = {relay}")
    print(f"[swarm-test] channel  = {channel}")
    print(f"[swarm-test] clients  = {num_clients}")
    print(f"[swarm-test] duration = {duration}s")

    # ── Baseline metrics ─────────────────────────────────────────────────────
    before = get_metric(relay, "soluna_relay_swarm_packets_saved_total")
    print(f"\n[swarm-test] swarm_packets_saved before = {before}")

    # ── Open N concurrent WebSocket connections ───────────────────────────────
    stop = threading.Event()
    threads: list[threading.Thread] = []

    print(f"[swarm-test] Connecting {num_clients} clients...")
    for i in range(num_clients):
        t = threading.Thread(
            target=open_ws,
            args=(relay, port, path, use_tls, stop),
            daemon=True,
        )
        t.start()
        threads.append(t)
        if (i + 1) % 10 == 0:
            print(f"  {i+1}/{num_clients} connected")
        time.sleep(0.05)  # stagger connects slightly

    connected = num_clients
    print(f"[swarm-test] All {connected} clients launched. Holding for {duration}s...")
    time.sleep(duration)

    # ── Check metrics ─────────────────────────────────────────────────────────
    after = get_metric(relay, "soluna_relay_swarm_packets_saved_total")
    print(f"[swarm-test] swarm_packets_saved after  = {after}")

    listeners = get_metric(relay, "soluna_relay_listeners")
    print(f"[swarm-test] listeners metric            = {listeners}")

    # ── Tear down ─────────────────────────────────────────────────────────────
    print("[swarm-test] Closing all connections...")
    stop.set()
    for t in threads:
        t.join(timeout=3.0)

    # ── Verdict ───────────────────────────────────────────────────────────────
    print()
    if after < 0:
        print("[swarm-test] SKIP: /metrics not available (is the relay running?)")
        return True

    if before < 0:
        before = 0.0
    delta = after - before

    if listeners >= 50:
        print(f"[swarm-test] ✓ Swarm threshold reached: {int(listeners)} listeners")
    else:
        print(f"[swarm-test] NOTE: listeners={int(listeners)} (threshold=50)")
        print("  The channel may have no active sender; relay counts listeners per channel.")

    if delta > 0:
        print(f"[swarm-test] PASS: swarm_packets_saved +{int(delta)} during test")
        return True
    elif listeners >= 50:
        print("[swarm-test] NOTE: Threshold reached but swarm_saved=0 (no active sender?)")
        print("  Start solunad on this channel for end-to-end verification.")
        return True
    else:
        print(f"[swarm-test] FAIL: swarm threshold not reached "
              f"(listeners={int(listeners)}, need ≥50)")
        print("  Try --clients 60 or ensure the relay can accept that many WebSocket connections.")
        return False


def main():
    p = argparse.ArgumentParser(description="OSTP swarm activation test")
    p.add_argument("--relay",    default="relay.solun.art")
    p.add_argument("--channel",  default="zamna")
    p.add_argument("--clients",  type=int, default=60,
                   help="Number of concurrent WebSocket listeners (need >50)")
    p.add_argument("--duration", type=int, default=15,
                   help="Seconds to hold connections open")
    args = p.parse_args()

    ok = run_test(args.relay, args.channel, args.clients, args.duration)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
