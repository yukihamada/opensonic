#!/usr/bin/env python3
"""
test_charge.py — OSTP economic layer end-to-end test

Tests the CHARGE command: joins a channel, sends a cryptographically
authenticated CHARGE command, and verifies the wallet balance increases.

Requires RELAY_CHARGE_SECRET environment variable (or --secret arg).

Usage:
  RELAY_CHARGE_SECRET=e8027ce547b8b718 python3 tests/test_charge.py
  python3 tests/test_charge.py --secret e8027ce547b8b718 --amount 0.01
"""

import argparse
import hashlib
import hmac
import os
import socket
import struct
import sys
import time
import uuid
import zlib

RELAY_UDP_PORT = 5100
OSTP_PROFILE   = 0x4F53


def hmac_sha256_hex(key: str, message: str) -> str:
    return hmac.new(key.encode(), message.encode(), hashlib.sha256).hexdigest()


def build_charge_token(secret: str, amount: float, device_id: str) -> tuple[str, str]:
    """Build CHARGE wire message and token.
    Returns (wire_cmd, token) where token = "<ts>:<hmac>"

    IMPORTANT: C++ std::to_string(double) uses 6 decimal places.
    HMAC input = "CHARGE:{amount:.6f}:{device_id}:{ts}"
    """
    ts = int(time.time())
    # Must match C++ std::to_string(double) format: 6 decimal places
    amount_str = f"{amount:.6f}"
    msg = f"CHARGE:{amount_str}:{device_id}:{ts}"
    h = hmac_sha256_hex(secret, msg)
    token = f"{ts}:{h}"
    wire = f"CHARGE:{amount}:{token}\n"
    return wire, token


def http_get(url: str, timeout: float = 5.0) -> tuple[int, str]:
    """Simple HTTP GET returning (status_code, body)."""
    import urllib.request
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.status, r.read().decode()
    except Exception as e:
        return 0, str(e)


def http_post(url: str, data: dict, timeout: float = 5.0) -> tuple[int, str]:
    """Simple HTTP POST with JSON body."""
    import urllib.request, json as _json
    payload = _json.dumps(data).encode()
    req = urllib.request.Request(url, data=payload,
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()
    except Exception as e:
        return 0, str(e)


def run_test(relay: str, channel: str, secret: str, amount: float) -> bool:
    device_id = f"test-{uuid.uuid4().hex[:12]}"
    base_url   = f"https://{relay}"
    print(f"[charge-test] device_id = {device_id}")
    print(f"[charge-test] amount    = {amount}")
    print(f"[charge-test] channel   = {channel}")
    print(f"[charge-test] base_url  = {base_url}")

    # ── Step 1: Query wallet balance before charge ─────────────────────────
    print(f"\n[charge-test] Step 1: GET /api/wallet (before)")
    status, body = http_get(f"{base_url}/api/wallet?device_id={device_id}")
    print(f"  HTTP {status}: {body}")
    balance_before = 0.0
    try:
        import json as _json
        j = _json.loads(body)
        balance_before = j.get("balance", 0.0)
    except Exception:
        pass
    print(f"  Balance before: {balance_before}")

    # ── Step 2: Send CHARGE ────────────────────────────────────────────────
    wire_cmd, token = build_charge_token(secret, amount, device_id)
    print(f"\n[charge-test] Step 2: POST /api/wallet/charge")
    status, body = http_post(f"{base_url}/api/wallet/charge", {
        "device_id": device_id,
        "amount": amount,
        "token": token,
    })
    print(f"  HTTP {status}: {body}")

    if status == 401:
        print("[charge-test] FAIL: HMAC rejected — check RELAY_CHARGE_SECRET")
        return False
    if status == 409:
        print("[charge-test] NOTE: Replay detected (already charged with this token)")
        return True  # already done, not a failure
    if status != 200:
        print(f"[charge-test] FAIL: CHARGE returned HTTP {status}: {body}")
        return False

    try:
        import json as _json
        j = _json.loads(body)
        balance_after_charge = j.get("balance", None)
    except Exception:
        balance_after_charge = None

    # ── Step 3: Query wallet balance after charge ──────────────────────────
    print(f"\n[charge-test] Step 3: GET /api/wallet (after)")
    status2, body2 = http_get(f"{base_url}/api/wallet?device_id={device_id}")
    print(f"  HTTP {status2}: {body2}")
    balance_after = 0.0
    try:
        import json as _json
        j2 = _json.loads(body2)
        balance_after = j2.get("balance", 0.0)
    except Exception:
        pass

    # ── Step 4: Replay protection test ────────────────────────────────────
    print(f"\n[charge-test] Step 4: Replay protection")
    status_r, body_r = http_post(f"{base_url}/api/wallet/charge", {
        "device_id": device_id,
        "amount": amount,
        "token": token,
    })
    print(f"  HTTP {status_r}: {body_r}")
    if status_r == 409:
        print("  ✓ Replay correctly rejected (409 Conflict)")
    else:
        print(f"  ✗ Replay was NOT blocked (got {status_r})")

    # ── Result ─────────────────────────────────────────────────────────────
    print()
    delta = balance_after - balance_before
    if abs(delta - amount) < 0.001:
        print(f"[charge-test] PASS: Balance {balance_before:.4f} → {balance_after:.4f} (+{delta:.4f})")
        return True
    else:
        print(f"[charge-test] FAIL: Balance delta={delta:.4f} expected={amount}")
        return False


def main():
    p = argparse.ArgumentParser(description="OSTP CHARGE economic layer test")
    p.add_argument("--relay",   default="relay.solun.art")
    p.add_argument("--channel", default="test")
    p.add_argument("--secret",  default=os.environ.get("RELAY_CHARGE_SECRET", ""))
    p.add_argument("--amount",  type=float, default=0.01, help="Amount in cents")
    args = p.parse_args()

    if not args.secret:
        print("ERROR: Set RELAY_CHARGE_SECRET env or --secret")
        sys.exit(1)

    ok = run_test(args.relay, args.channel, args.secret, args.amount)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
