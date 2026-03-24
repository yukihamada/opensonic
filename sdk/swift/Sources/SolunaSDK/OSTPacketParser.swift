import Foundation

/// Parser for OSTP/RTP packets received from the Soluna relay.
public enum OSTPacketParser {

    /// Attempt to parse a raw UDP datagram as an OSTP/RTP audio packet.
    ///
    /// Returns nil for non-audio packets (control messages, too-short packets, etc.).
    ///
    /// Packet structure:
    /// - RTP header: 12 bytes (version=2, first 2 bits of byte 0 = 0b10)
    /// - Optional RTP extension: 4-byte header (profile + length) + extension data
    ///   - OSTP profile = 0x4F53 ("OS")
    ///   - Extension data starts with stream_id (2 bytes, big-endian)
    ///     - Bits [15:14] = deck_id (2 bit)
    ///     - Bits [13:10] = channel count (4 bit)
    ///     - Bits  [9:0]  = stream index (10 bit)
    /// - Audio payload
    /// - CRC-32 trailer: last 4 bytes (stripped before decode)
    public static func parse(_ data: Data) -> OSTPacket? {
        guard data.count >= OSTConstants.rtpHeaderSize else { return nil }

        // Check RTP version bits: (byte[0] & 0xC0) == 0x80
        guard (data[0] & 0xC0) == 0x80 else { return nil }

        let payloadType = data[1] & 0x7F
        let hasExtension = (data[0] & 0x10) != 0

        // Sequence number (big-endian, bytes 2-3)
        let sequenceNumber = (UInt16(data[2]) << 8) | UInt16(data[3])

        // Timestamp (big-endian, bytes 4-7)
        let timestamp = (UInt32(data[4]) << 24) | (UInt32(data[5]) << 16) |
                        (UInt32(data[6]) << 8) | UInt32(data[7])

        var payloadOffset = OSTConstants.rtpHeaderSize
        var channels = 2  // default stereo
        var deckId = 0

        if hasExtension && data.count >= 16 {
            // Extension header at byte 12-15:
            //   [12-13] = profile (0x4F53 = "OS" for OSTP)
            //   [14-15] = length in 32-bit words
            let extProfile = (UInt16(data[12]) << 8) | UInt16(data[13])
            let extLenWords = (Int(data[14]) << 8) | Int(data[15])
            let extBytes = extLenWords * 4

            payloadOffset = OSTConstants.rtpHeaderSize + 4 + extBytes

            // OSTP: stream_id is first 2 bytes of extension data (byte 16-17)
            if extProfile == OSTConstants.ostpProfile && data.count >= 18 {
                let streamId = (Int(data[16]) << 8) | Int(data[17])
                // New layout: [15:14]=deck_id, [13:10]=ch_count, [9:0]=stream_idx
                let chNew = (streamId >> 10) & 0xF
                // Legacy layout: [15:12]=ch_count, [11:0]=stream_id
                let chLegacy = (streamId >> 12) & 0xF
                // Use new layout if ch_count > 0, else try legacy
                if chNew > 0 {
                    channels = chNew
                } else if chLegacy > 0 {
                    channels = chLegacy
                }
                deckId = (streamId >> 14) & 0x3
            }
        }

        // Strip CRC-32 trailer (last 4 bytes)
        var payloadEnd = data.count
        if payloadEnd - payloadOffset > OSTConstants.crcTrailerSize {
            payloadEnd -= OSTConstants.crcTrailerSize
        }

        guard payloadEnd > payloadOffset else { return nil }

        let payload = data[payloadOffset..<payloadEnd]

        return OSTPacket(
            payloadType: payloadType,
            channels: channels,
            deckId: deckId,
            payload: Data(payload),
            sequenceNumber: sequenceNumber,
            timestamp: timestamp
        )
    }
}
