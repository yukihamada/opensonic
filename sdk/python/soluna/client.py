"""
Main entry point for the Soluna audio relay SDK (Python).

Usage:
    from soluna import SolunaClient

    client = SolunaClient()
    client.on_audio(lambda samples, channels: ...)
    client.connect("my-channel")
    # ...
    client.disconnect()
"""

from __future__ import annotations

import platform
from typing import Callable

from .parser import OSTConstants, OSTPacket, parse_ost_packet
from .relay import RelayConnection
from .player import AudioPlayer


class SolunaClient:
    """Connects to the Soluna relay server over UDP, receives OSTP/RTP
    audio packets, decodes them, and optionally plays them.
    """

    def __init__(self, playback: bool = False) -> None:
        """
        Args:
            playback: If True, automatically play received audio through sounddevice.
                      Requires `pip install sounddevice`.
        """
        self._connection: RelayConnection | None = None
        self._audio_player: AudioPlayer | None = None
        self._playback = playback
        self._channel = ""
        self._connected = False
        self._device_name = platform.node() or "SolunaSDK-Python"

        self._on_audio: Callable[[list[float], int], None] | None = None
        self._on_packet: Callable[[OSTPacket], None] | None = None
        self._on_state: Callable[[str], None] | None = None

    @property
    def is_connected(self) -> bool:
        return self._connected

    @property
    def channel(self) -> str:
        return self._channel

    def on_audio(self, callback: Callable[[list[float], int], None]) -> None:
        """Register a callback for decoded float32 PCM audio.
        callback(samples: list[float], channels: int)
        """
        self._on_audio = callback

    def on_packet(self, callback: Callable[[OSTPacket], None]) -> None:
        """Register a callback for parsed OSTP packets."""
        self._on_packet = callback

    def on_state(self, callback: Callable[[str], None]) -> None:
        """Register a callback for state changes."""
        self._on_state = callback

    def connect(
        self,
        channel: str,
        host: str = OSTConstants.DEFAULT_HOST,
        port: int = OSTConstants.DEFAULT_PORT,
    ) -> bool:
        """Connect to the Soluna relay and start receiving audio.
        Returns True on success.
        """
        if self._connected:
            return True

        self._channel = channel

        if self._playback:
            self._audio_player = AudioPlayer()
            self._audio_player.start()

        conn = RelayConnection(
            channel=channel,
            host=host,
            port=port,
            device_name=self._device_name,
        )
        conn.on_packet = self._handle_packet

        if conn.connect():
            self._connection = conn
            self._connected = True
            if self._on_state:
                self._on_state("connected")
            return True
        else:
            if self._audio_player:
                self._audio_player.stop()
                self._audio_player = None
            if self._on_state:
                self._on_state("error")
            return False

    def disconnect(self) -> None:
        """Disconnect from the relay and stop audio playback."""
        if not self._connected:
            return

        if self._connection:
            self._connection.disconnect()
            self._connection = None

        if self._audio_player:
            self._audio_player.stop()
            self._audio_player = None

        self._connected = False
        self._channel = ""
        if self._on_state:
            self._on_state("disconnected")

    def set_channel(self, name: str) -> None:
        """Switch to a different channel. Reconnects automatically."""
        was_connected = self._connected
        host = OSTConstants.DEFAULT_HOST
        port = OSTConstants.DEFAULT_PORT

        if was_connected:
            self.disconnect()
        if was_connected:
            self.connect(name, host, port)

    def _handle_packet(self, data: bytes) -> None:
        packet = parse_ost_packet(data)
        if packet is None:
            return

        if self._on_packet:
            self._on_packet(packet)

        if self._audio_player:
            self._audio_player.play_packet(packet)

        if self._on_audio:
            import struct as _struct

            from .adpcm import decode_adpcm_payload

            if packet.payload_type in (OSTConstants.PT_ADPCM_STEREO, OSTConstants.PT_ADPCM_MONO):
                pcm16 = decode_adpcm_payload(packet.payload)
                if pcm16:
                    samples = [s / 32768.0 for s in pcm16]
                    self._on_audio(samples, packet.channels)
            else:
                total = len(packet.payload) // 4
                if total > 0:
                    scale = 1.0 / 2147483647.0
                    samples = []
                    for i in range(total):
                        val = _struct.unpack_from("<i", packet.payload, i * 4)[0]
                        samples.append(val * scale)
                    self._on_audio(samples, packet.channels)
