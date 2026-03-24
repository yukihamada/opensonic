"""
Optional audio playback using sounddevice.
Requires: pip install sounddevice
"""

from __future__ import annotations

import array
import struct
from typing import TYPE_CHECKING

from .adpcm import decode_adpcm_payload
from .parser import OSTConstants, OSTPacket

if TYPE_CHECKING:
    import numpy as np

SAMPLE_RATE = 48000


class AudioPlayer:
    """Plays decoded PCM audio through sounddevice (PortAudio).
    Format: 48kHz stereo float32.
    """

    def __init__(self) -> None:
        self._stream = None
        self._sd = None

    def start(self) -> None:
        """Start the audio output stream."""
        try:
            import sounddevice as sd  # type: ignore
        except ImportError:
            raise ImportError("pip install sounddevice")

        self._sd = sd
        self._stream = sd.OutputStream(
            samplerate=SAMPLE_RATE,
            channels=2,
            dtype="float32",
            blocksize=0,
        )
        self._stream.start()

    def stop(self) -> None:
        """Stop the audio output stream."""
        if self._stream:
            self._stream.stop()
            self._stream.close()
            self._stream = None

    @property
    def is_playing(self) -> bool:
        return self._stream is not None and self._stream.active

    def play_packet(self, packet: OSTPacket) -> None:
        """Decode and play an OSTP packet."""
        if not self._stream:
            return

        import numpy as np  # type: ignore

        if packet.payload_type in (OSTConstants.PT_ADPCM_STEREO, OSTConstants.PT_ADPCM_MONO):
            samples = self._decode_adpcm_to_float(packet.payload, packet.channels)
        elif packet.payload_type == OSTConstants.PT_OPUS:
            samples = self._decode_opus_to_float(packet.payload, packet.channels)
        elif packet.payload_type == OSTConstants.PT_LC3:
            samples = self._decode_lc3_to_float(packet.payload, packet.channels)
        else:
            samples = self._decode_int32le_to_float(packet.payload, packet.channels)

        if samples is None or len(samples) == 0:
            return

        channels = min(packet.channels, 2)
        frames = len(samples) // channels

        if frames == 0:
            return

        # Reshape to (frames, channels)
        interleaved = np.array(samples[:frames * channels], dtype=np.float32)
        if channels == 1:
            # Mono -> stereo
            mono = interleaved.reshape(-1, 1)
            stereo = np.column_stack([mono, mono])
        else:
            stereo = interleaved.reshape(-1, channels)[:, :2]

        try:
            self._stream.write(stereo)
        except Exception:
            pass

    def _decode_int32le_to_float(self, payload: bytes, channels: int) -> list[float] | None:
        """Decode int32 LE interleaved payload to float32 samples."""
        total_samples = len(payload) // 4
        if total_samples == 0:
            return None

        scale = 1.0 / 2147483647.0
        samples = []
        for i in range(total_samples):
            offset = i * 4
            val = struct.unpack_from("<i", payload, offset)[0]
            samples.append(val * scale)
        return samples

    def _decode_opus_to_float(self, payload: bytes, channels: int) -> list[float] | None:
        """Decode Opus payload to float32 samples.

        Requires: pip install opuslib
        """
        try:
            import opuslib  # type: ignore
        except ImportError:
            print("[SolunaSDK] Opus codec requires opuslib — pip install opuslib")
            return None

        try:
            decoder = opuslib.Decoder(48000, channels)
            # 960 frames = 20ms at 48kHz
            pcm = decoder.decode(payload, 960)
            # opuslib returns bytes (int16 LE), convert to float32
            total_samples = len(pcm) // 2
            samples = []
            for i in range(total_samples):
                val = struct.unpack_from("<h", pcm, i * 2)[0]
                samples.append(val / 32768.0)
            return samples
        except Exception as e:
            print(f"[SolunaSDK] Opus decode error: {e}")
            return None

    def _decode_lc3_to_float(self, payload: bytes, channels: int) -> list[float] | None:
        """Decode LC3 payload to float32 samples.

        LC3 decode requires liblc3 (Google, Apache 2.0).
        https://github.com/google/liblc3
        TODO: Add Python bindings for liblc3.
        """
        print("[SolunaSDK] LC3 codec not yet supported — install liblc3")
        return None

    def _decode_adpcm_to_float(self, payload: bytes, channels: int) -> list[float] | None:
        """Decode ADPCM payload to float32 samples."""
        pcm16 = decode_adpcm_payload(payload)
        if pcm16 is None:
            return None

        return [s / 32768.0 for s in pcm16]
