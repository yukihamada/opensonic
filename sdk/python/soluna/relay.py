"""
Low-level BSD socket connection to the Soluna relay server.
Uses UDP sockets, matching the Swift/C++ implementation.
"""

from __future__ import annotations

import socket
import struct
import threading
import time
from typing import Callable

from .parser import OSTConstants


class RelayConnection:
    """UDP socket connection to the Soluna relay."""

    def __init__(
        self,
        channel: str,
        host: str = OSTConstants.DEFAULT_HOST,
        port: int = OSTConstants.DEFAULT_PORT,
        device_name: str = "SolunaSDK-Python",
    ) -> None:
        self.channel = channel
        self.host = host
        self.port = port
        self.device_name = device_name

        self._sock: socket.socket | None = None
        self._running = False
        self._recv_thread: threading.Thread | None = None
        self._heartbeat_thread: threading.Thread | None = None
        self._addr: tuple[str, int] | None = None

        self.on_packet: Callable[[bytes], None] | None = None
        self.on_control_message: Callable[[str], None] | None = None

    def connect(self) -> bool:
        """Open the UDP socket, send HELLO/JOIN, and start the receive loop.
        Returns False if DNS resolution or socket creation fails.
        """
        if self._running:
            return True

        try:
            # DNS resolve
            info = socket.getaddrinfo(self.host, self.port, socket.AF_INET, socket.SOCK_DGRAM)
            if not info:
                return False
            self._addr = info[0][4]
        except socket.gaierror:
            return False

        # Create UDP socket
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.settimeout(1.0)

        # Send HELLO x3 (100ms apart)
        for i in range(3):
            self._send_message("HELLO\n")
            if i < 2:
                time.sleep(0.1)

        # JOIN
        self._send_message(f"JOIN:{self.channel}::{self.device_name}\n")

        self._running = True

        # Start receive thread
        self._recv_thread = threading.Thread(target=self._recv_loop, daemon=True)
        self._recv_thread.start()

        # Start heartbeat thread
        self._heartbeat_thread = threading.Thread(target=self._heartbeat_loop, daemon=True)
        self._heartbeat_thread.start()

        return True

    def disconnect(self) -> None:
        """Close the connection."""
        self._running = False

        if self._sock:
            self._sock.close()
            self._sock = None

        if self._recv_thread:
            self._recv_thread.join(timeout=2.0)
            self._recv_thread = None

        if self._heartbeat_thread:
            self._heartbeat_thread.join(timeout=2.0)
            self._heartbeat_thread = None

    def _send_message(self, message: str) -> None:
        if self._sock and self._addr:
            try:
                self._sock.sendto(message.encode("utf-8"), self._addr)
            except OSError:
                pass

    def _recv_loop(self) -> None:
        while self._running and self._sock:
            try:
                data, _ = self._sock.recvfrom(OSTConstants.RECV_BUFFER_SIZE)
            except socket.timeout:
                continue
            except OSError:
                break

            if len(data) < OSTConstants.RTP_HEADER_SIZE:
                continue

            # RTP/OSTP audio packet: (byte[0] & 0xC0) == 0x80
            if (data[0] & 0xC0) == 0x80:
                if self.on_packet:
                    self.on_packet(data)
            else:
                # Text control message
                if self.on_control_message:
                    try:
                        msg = data.decode("utf-8")
                        self.on_control_message(msg)
                    except UnicodeDecodeError:
                        pass

    def _heartbeat_loop(self) -> None:
        while self._running:
            time.sleep(OSTConstants.HEARTBEAT_INTERVAL)
            if not self._running:
                break
            self._send_message("HELLO\n")
            self._send_message(f"JOIN:{self.channel}::{self.device_name}\n")
