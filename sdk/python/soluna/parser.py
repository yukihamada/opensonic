"""
Parser for OSTP/RTP packets received from the Soluna relay.
"""

from __future__ import annotations

from dataclasses import dataclass


class OSTConstants:
    """OSTP protocol constants matching the C++ implementation."""

    OSTP_PROFILE: int = 0x4F53
    DEFAULT_HOST: str = "relay.solun.art"
    DEFAULT_PORT: int = 5100
    HEARTBEAT_INTERVAL: float = 4.0
    RECV_BUFFER_SIZE: int = 16384
    RTP_HEADER_SIZE: int = 12
    CRC_TRAILER_SIZE: int = 4
    PT_ADPCM_STEREO: int = 115
    PT_ADPCM_MONO: int = 116
    PT_OPUS: int = 98
    PT_LC3: int = 119


@dataclass
class OSTPacket:
    """A parsed OSTP/RTP audio packet."""

    payload_type: int
    channels: int
    deck_id: int
    payload: bytes
    sequence_number: int
    timestamp: int


def parse_ost_packet(data: bytes | bytearray) -> OSTPacket | None:
    """Parse a raw UDP datagram as an OSTP/RTP audio packet.
    Returns None for non-audio packets.
    """
    if len(data) < OSTConstants.RTP_HEADER_SIZE:
        return None

    # Check RTP version bits: (byte[0] & 0xC0) == 0x80
    if (data[0] & 0xC0) != 0x80:
        return None

    payload_type = data[1] & 0x7F
    has_extension = (data[0] & 0x10) != 0

    # Sequence number (big-endian, bytes 2-3)
    sequence_number = (data[2] << 8) | data[3]

    # Timestamp (big-endian, bytes 4-7)
    timestamp = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7]

    payload_offset = OSTConstants.RTP_HEADER_SIZE
    channels = 2  # default stereo
    deck_id = 0

    if has_extension and len(data) >= 16:
        ext_profile = (data[12] << 8) | data[13]
        ext_len_words = (data[14] << 8) | data[15]
        ext_bytes = ext_len_words * 4

        payload_offset = OSTConstants.RTP_HEADER_SIZE + 4 + ext_bytes

        # OSTP: stream_id is first 2 bytes of extension data (byte 16-17)
        if ext_profile == OSTConstants.OSTP_PROFILE and len(data) >= 18:
            stream_id = (data[16] << 8) | data[17]
            ch = (stream_id >> 10) & 0xF
            if ch > 0:
                channels = ch
            deck_id = (stream_id >> 14) & 0x3

    # Strip CRC-32 trailer (last 4 bytes)
    payload_end = len(data)
    if payload_end - payload_offset > OSTConstants.CRC_TRAILER_SIZE:
        payload_end -= OSTConstants.CRC_TRAILER_SIZE

    if payload_end <= payload_offset:
        return None

    payload = bytes(data[payload_offset:payload_end])

    return OSTPacket(
        payload_type=payload_type,
        channels=channels,
        deck_id=deck_id,
        payload=payload,
        sequence_number=sequence_number,
        timestamp=timestamp,
    )
