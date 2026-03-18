/// OSTP (Soluna Transport Protocol) packet parser.
///
/// Packet layout:
///   RTP Header        (12 bytes)
///   RTP Extension Hdr ( 4 bytes)  — profile = 0x4F53 "OS", length = 2
///   OSTP Header       ( 8 bytes)
///   Audio Payload     (variable)
///   CRC-32 Trailer    ( 4 bytes, optional)

/// RTP header fields (12 bytes).
#[derive(Debug, Clone, Copy)]
pub struct RtpHeader {
    pub version: u8,
    pub padding: bool,
    pub extension: bool,
    pub cc: u8,
    pub marker: bool,
    pub payload_type: u8,
    pub sequence: u16,
    pub timestamp: u32,
    pub ssrc: u32,
}

/// OSTP extension header fields (8 bytes).
#[derive(Debug, Clone, Copy)]
pub struct OstpHeader {
    pub stream_id: u16,
    pub sequence_ext: u16,
    pub media_timestamp: u32,
}

/// Parsed OSTP packet.
#[derive(Debug)]
pub struct OstpPacket<'a> {
    pub rtp: RtpHeader,
    pub ostp: OstpHeader,
    pub payload: &'a [u8],
    /// Number of TX channels decoded from stream_id upper 4 bits.
    /// 0 = legacy (assume 2ch).
    pub tx_channels: u32,
}

/// Known payload types.
pub const PT_PCM24: u8 = 96;
pub const PT_F32: u8 = 97;
pub const PT_OPUS: u8 = 98;
pub const PT_AES67_L24: u8 = 10;
pub const PT_AES67_L16: u8 = 11;

const OSTP_PROFILE: u16 = 0x4F53; // "OS"
const RTP_HEADER_SIZE: usize = 12;
const RTP_EXT_HEADER_SIZE: usize = 4;
const OSTP_HEADER_SIZE: usize = 8;
const TOTAL_HEADER_SIZE: usize = RTP_HEADER_SIZE + RTP_EXT_HEADER_SIZE + OSTP_HEADER_SIZE; // 24
const CRC_TRAILER_SIZE: usize = 4;

/// Parse an OSTP/RTP packet from raw bytes.
///
/// Returns `None` if the packet is too small or has an invalid RTP version.
/// CRC-32 is verified when present.
pub fn parse(data: &[u8]) -> Option<OstpPacket<'_>> {
    if data.len() < TOTAL_HEADER_SIZE {
        return None;
    }

    // RTP header (12 bytes, network byte order)
    let b0 = data[0];
    let b1 = data[1];
    let version = (b0 >> 6) & 0x03;
    if version != 2 {
        return None;
    }

    let rtp = RtpHeader {
        version,
        padding: (b0 >> 5) & 1 != 0,
        extension: (b0 >> 4) & 1 != 0,
        cc: b0 & 0x0F,
        marker: (b1 >> 7) & 1 != 0,
        payload_type: b1 & 0x7F,
        sequence: u16::from_be_bytes([data[2], data[3]]),
        timestamp: u32::from_be_bytes([data[4], data[5], data[6], data[7]]),
        ssrc: u32::from_be_bytes([data[8], data[9], data[10], data[11]]),
    };

    if !rtp.extension {
        return None; // OSTP requires extension header
    }

    // Skip CSRC entries
    let csrc_offset = RTP_HEADER_SIZE + (rtp.cc as usize) * 4;
    if data.len() < csrc_offset + RTP_EXT_HEADER_SIZE + OSTP_HEADER_SIZE {
        return None;
    }

    // RTP Extension header (4 bytes)
    let ext_off = csrc_offset;
    let profile = u16::from_be_bytes([data[ext_off], data[ext_off + 1]]);
    let _ext_len = u16::from_be_bytes([data[ext_off + 2], data[ext_off + 3]]);

    if profile != OSTP_PROFILE {
        return None; // Not an OSTP packet
    }

    // OSTP header (8 bytes)
    let ostp_off = ext_off + RTP_EXT_HEADER_SIZE;
    let ostp = OstpHeader {
        stream_id: u16::from_be_bytes([data[ostp_off], data[ostp_off + 1]]),
        sequence_ext: u16::from_be_bytes([data[ostp_off + 2], data[ostp_off + 3]]),
        media_timestamp: u32::from_be_bytes([
            data[ostp_off + 4],
            data[ostp_off + 5],
            data[ostp_off + 6],
            data[ostp_off + 7],
        ]),
    };

    let payload_start = ostp_off + OSTP_HEADER_SIZE;

    // Determine payload end (check for CRC-32 trailer)
    // C++ stores CRC in big-endian, computed over payload only
    let (payload_end, has_crc) = if data.len() >= payload_start + CRC_TRAILER_SIZE + 1 {
        let crc_off = data.len() - CRC_TRAILER_SIZE;
        let stored_crc = u32::from_be_bytes([
            data[crc_off],
            data[crc_off + 1],
            data[crc_off + 2],
            data[crc_off + 3],
        ]);
        let payload_data = &data[payload_start..crc_off];
        let computed = crc32_ieee(payload_data);
        if stored_crc == computed {
            (crc_off, true)
        } else {
            // CRC doesn't match — treat entire remainder as payload (no CRC)
            (data.len(), false)
        }
    } else {
        (data.len(), false)
    };
    let _ = has_crc;

    if payload_end <= payload_start {
        return None;
    }

    let payload = &data[payload_start..payload_end];

    // Decode TX channel count from stream_id upper 4 bits
    let ch_code = (ostp.stream_id >> 12) & 0xF;
    let tx_channels = if ch_code == 0 { 2 } else { ch_code as u32 }; // 0 = legacy 2ch

    Some(OstpPacket {
        rtp,
        ostp,
        payload,
        tx_channels,
    })
}

/// IEEE 802.3 CRC-32.
fn crc32_ieee(data: &[u8]) -> u32 {
    let mut crc: u32 = 0xFFFF_FFFF;
    for &b in data {
        crc ^= b as u32;
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xEDB8_8320 & (0u32.wrapping_sub(crc & 1)));
        }
    }
    crc ^ 0xFFFF_FFFF // final XOR
}

/// Returns the total OSTP header size (for stripping headers from raw packets).
pub const fn header_size() -> usize {
    TOTAL_HEADER_SIZE
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_too_small() {
        assert!(parse(&[0u8; 10]).is_none());
    }

    #[test]
    fn test_crc32() {
        // Known CRC-32 of "123456789"
        let data = b"123456789";
        assert_eq!(crc32_ieee(data), 0xCBF4_3926);
    }
}
