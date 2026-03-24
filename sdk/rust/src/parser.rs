//! Parser for OSTP/RTP packets received from the Soluna relay.

/// OSTP protocol constants matching the C++ implementation.
pub mod constants {
    /// RTP header extension profile for OSTP ("OS" = 0x4F53).
    pub const OSTP_PROFILE: u16 = 0x4F53;
    /// Default relay host.
    pub const DEFAULT_HOST: &str = "relay.solun.art";
    /// Default relay port.
    pub const DEFAULT_PORT: u16 = 5100;
    /// Heartbeat interval in seconds.
    pub const HEARTBEAT_INTERVAL_SECS: u64 = 4;
    /// Receive buffer size.
    pub const RECV_BUFFER_SIZE: usize = 16384;
    /// RTP header size in bytes.
    pub const RTP_HEADER_SIZE: usize = 12;
    /// CRC-32 trailer size in bytes.
    pub const CRC_TRAILER_SIZE: usize = 4;
    /// ADPCM stereo payload type.
    pub const PT_ADPCM_STEREO: u8 = 115;
    /// ADPCM mono payload type.
    pub const PT_ADPCM_MONO: u8 = 116;
    /// Opus compressed payload type (RFC 6716).
    pub const PT_OPUS: u8 = 98;
    /// LC3 Bluetooth LE Audio payload type (liblc3, Apache 2.0).
    pub const PT_LC3: u8 = 119;
}

/// A parsed OSTP/RTP audio packet.
#[derive(Debug, Clone)]
pub struct OSTPacket {
    /// RTP payload type.
    pub payload_type: u8,
    /// Number of audio channels (from stream_id bits [13:10]).
    pub channels: usize,
    /// Deck ID (from stream_id bits [15:14]).
    pub deck_id: u8,
    /// Audio payload data (after header, before CRC trailer).
    pub payload: Vec<u8>,
    /// RTP sequence number.
    pub sequence_number: u16,
    /// RTP timestamp.
    pub timestamp: u32,
}

/// Parse a raw UDP datagram as an OSTP/RTP audio packet.
/// Returns None for non-audio packets.
pub fn parse_ost_packet(data: &[u8]) -> Option<OSTPacket> {
    if data.len() < constants::RTP_HEADER_SIZE {
        return None;
    }

    // Check RTP version bits: (byte[0] & 0xC0) == 0x80
    if (data[0] & 0xC0) != 0x80 {
        return None;
    }

    let payload_type = data[1] & 0x7F;
    let has_extension = (data[0] & 0x10) != 0;

    // Sequence number (big-endian, bytes 2-3)
    let sequence_number = u16::from_be_bytes([data[2], data[3]]);

    // Timestamp (big-endian, bytes 4-7)
    let timestamp = u32::from_be_bytes([data[4], data[5], data[6], data[7]]);

    let mut payload_offset = constants::RTP_HEADER_SIZE;
    let mut channels: usize = 2; // default stereo
    let mut deck_id: u8 = 0;

    if has_extension && data.len() >= 16 {
        let ext_profile = u16::from_be_bytes([data[12], data[13]]);
        let ext_len_words = u16::from_be_bytes([data[14], data[15]]) as usize;
        let ext_bytes = ext_len_words * 4;

        payload_offset = constants::RTP_HEADER_SIZE + 4 + ext_bytes;

        // OSTP: stream_id is first 2 bytes of extension data (byte 16-17)
        if ext_profile == constants::OSTP_PROFILE && data.len() >= 18 {
            let stream_id = u16::from_be_bytes([data[16], data[17]]) as usize;
            let ch = (stream_id >> 10) & 0xF;
            if ch > 0 {
                channels = ch;
            }
            deck_id = ((stream_id >> 14) & 0x3) as u8;
        }
    }

    // Strip CRC-32 trailer (last 4 bytes)
    let mut payload_end = data.len();
    if payload_end - payload_offset > constants::CRC_TRAILER_SIZE {
        payload_end -= constants::CRC_TRAILER_SIZE;
    }

    if payload_end <= payload_offset {
        return None;
    }

    let payload = data[payload_offset..payload_end].to_vec();

    Some(OSTPacket {
        payload_type,
        channels,
        deck_id,
        payload,
        sequence_number,
        timestamp,
    })
}
