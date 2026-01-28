#!/usr/bin/env python3
"""
Soluna ESP32 OTA Upload Tool

Sends firmware binary to an ESP32 running Soluna via UDP control protocol.

Usage:
    python3 esp32_ota.py <device_ip> <firmware.bin>

Protocol:
    1. Send OTA_BEGIN (0xF1) with firmware size → expect OTA_READY (0xF4)
    2. Send OTA_WRITE (0xF2) chunks (1024B + seq#) → expect OTA_ACK (0xF5)
    3. Send OTA_FINISH (0xF3) → device reboots

SPDX-License-Identifier: MIT
"""

import argparse
import os
import socket
import struct
import sys
import time

# Protocol constants
CTRL_MAGIC      = 0x53
CTRL_PORT       = 8401
CHUNK_SIZE      = 1024
TIMEOUT_SEC     = 5.0
MAX_RETRIES     = 3

# Command IDs
CMD_OTA_BEGIN   = 0xF1
CMD_OTA_CHUNK   = 0xF2
CMD_OTA_END     = 0xF3
CMD_OTA_READY   = 0xF4
CMD_OTA_ACK     = 0xF5
CMD_OTA_ERROR   = 0xF6


def build_packet(cmd, payload=b''):
    """Build a Soluna control packet."""
    hdr = struct.pack('>BBH', CTRL_MAGIC, cmd, len(payload))
    return hdr + payload


def parse_response(data):
    """Parse a Soluna control response. Returns (cmd, payload)."""
    if len(data) < 4:
        return None, None
    magic, cmd, plen = struct.unpack('>BBH', data[:4])
    if magic != CTRL_MAGIC:
        return None, None
    payload = data[4:4+plen]
    return cmd, payload


def send_and_wait(sock, addr, packet, expected_cmd, retries=MAX_RETRIES):
    """Send packet and wait for expected response."""
    for attempt in range(retries):
        sock.sendto(packet, addr)
        try:
            data, _ = sock.recvfrom(4096)
            cmd, payload = parse_response(data)
            if cmd == expected_cmd:
                return payload
            if cmd == CMD_OTA_ERROR:
                err = payload[0] if payload else 0xFF
                raise RuntimeError(f"Device returned OTA error: 0x{err:02X}")
        except socket.timeout:
            if attempt < retries - 1:
                print(f"  Timeout, retry {attempt + 1}/{retries}...")
            else:
                raise TimeoutError(f"No response after {retries} attempts")
    return None


def upload_firmware(device_ip, firmware_path):
    """Upload firmware to ESP32 via OTA."""
    # Read firmware
    firmware_size = os.path.getsize(firmware_path)
    with open(firmware_path, 'rb') as f:
        firmware = f.read()

    print(f"Firmware: {firmware_path} ({firmware_size} bytes)")
    print(f"Device:   {device_ip}:{CTRL_PORT}")
    print()

    # Create UDP socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(TIMEOUT_SEC)
    addr = (device_ip, CTRL_PORT)

    try:
        # Step 1: OTA_BEGIN
        print("Step 1: Sending OTA_BEGIN...")
        begin_payload = struct.pack('>I', firmware_size)
        send_and_wait(sock, addr, build_packet(CMD_OTA_BEGIN, begin_payload),
                      CMD_OTA_READY)
        print("  Device ready for OTA")

        # Step 2: Send chunks
        total_chunks = (firmware_size + CHUNK_SIZE - 1) // CHUNK_SIZE
        print(f"Step 2: Sending {total_chunks} chunks...")

        start_time = time.time()
        for seq in range(total_chunks):
            offset = seq * CHUNK_SIZE
            chunk = firmware[offset:offset + CHUNK_SIZE]
            chunk_payload = struct.pack('>I', seq) + chunk

            send_and_wait(sock, addr, build_packet(CMD_OTA_CHUNK, chunk_payload),
                          CMD_OTA_ACK)

            # Progress
            progress = (seq + 1) * 100 // total_chunks
            elapsed = time.time() - start_time
            speed = (offset + len(chunk)) / elapsed / 1024 if elapsed > 0 else 0
            print(f"\r  [{progress:3d}%] chunk {seq+1}/{total_chunks}"
                  f" ({speed:.1f} KB/s)", end='', flush=True)

        elapsed = time.time() - start_time
        print(f"\n  Transfer complete: {elapsed:.1f}s")

        # Step 3: OTA_FINISH
        print("Step 3: Sending OTA_FINISH...")
        sock.sendto(build_packet(CMD_OTA_END), addr)

        # Device will reboot — don't wait for response
        print()
        print("OTA upload successful!")
        print("Device is rebooting with new firmware...")

    except TimeoutError as e:
        print(f"\nError: {e}", file=sys.stderr)
        return 1
    except RuntimeError as e:
        print(f"\nError: {e}", file=sys.stderr)
        return 1
    finally:
        sock.close()

    return 0


def main():
    parser = argparse.ArgumentParser(
        description='Soluna ESP32 OTA Firmware Upload Tool')
    parser.add_argument('device_ip', help='ESP32 IP address')
    parser.add_argument('firmware', help='Firmware binary (.bin) path')
    parser.add_argument('--port', type=int, default=CTRL_PORT,
                        help=f'Control port (default: {CTRL_PORT})')
    parser.add_argument('--chunk-size', type=int, default=CHUNK_SIZE,
                        help=f'Chunk size (default: {CHUNK_SIZE})')
    parser.add_argument('--timeout', type=float, default=TIMEOUT_SEC,
                        help=f'Timeout seconds (default: {TIMEOUT_SEC})')

    args = parser.parse_args()

    if not os.path.isfile(args.firmware):
        print(f"Error: file not found: {args.firmware}", file=sys.stderr)
        return 1

    global CTRL_PORT, CHUNK_SIZE, TIMEOUT_SEC
    CTRL_PORT = args.port
    CHUNK_SIZE = args.chunk_size
    TIMEOUT_SEC = args.timeout

    return upload_firmware(args.device_ip, args.firmware)


if __name__ == '__main__':
    sys.exit(main())
