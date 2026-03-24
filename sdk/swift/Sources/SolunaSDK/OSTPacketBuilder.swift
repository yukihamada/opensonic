import Foundation

/// Builds OSTP/RTP audio packets for transmission.
///
/// Packet structure (matching the C++ implementation):
/// - 12-byte RTP header (version=2, extension bit set)
/// - 4-byte extension header (profile=0x4F53 "OS", length=2 words)
/// - 8-byte OSTP extension data (stream_id + padding + timestamp copy)
/// - Audio payload (S24/int16 PCM)
/// - 4-byte CRC-32 trailer
///
/// Total header size = 12 + 4 + 8 = 24 bytes before payload.
public enum OSTPacketBuilder {

    /// OSTP header size (RTP header + extension header + extension data).
    public static let headerSize = 24

    /// Build an OSTP packet from PCM audio data.
    ///
    /// - Parameters:
    ///   - payload: Raw audio payload bytes (e.g. S24 int32 LE or int16 LE).
    ///   - payloadType: RTP payload type (e.g. 96 for generic).
    ///   - sequenceNumber: Packet sequence number.
    ///   - timestamp: RTP timestamp (incremented by frames per packet).
    ///   - ssrc: Synchronization source identifier.
    ///   - channels: Number of audio channels (encoded in stream_id).
    ///   - deckId: DJ deck identifier (0-3, encoded in stream_id bits [15:14]).
    /// - Returns: Complete OSTP packet as Data, ready to send via UDP.
    public static func buildPacket(
        payload: Data,
        payloadType: UInt8 = 96,
        sequenceNumber: UInt16,
        timestamp: UInt32,
        ssrc: UInt32,
        channels: Int = 2,
        deckId: Int = 0
    ) -> Data {
        let totalSize = headerSize + payload.count + OSTConstants.crcTrailerSize
        var pkt = [UInt8](repeating: 0, count: totalSize)

        // RTP header (12 bytes)
        // Byte 0: V=2, P=0, X=1 (extension), CC=0 → 0x90
        pkt[0] = 0x90
        // Byte 1: M=0, PT
        pkt[1] = payloadType & 0x7F
        // Bytes 2-3: sequence number (big-endian)
        pkt[2] = UInt8((sequenceNumber >> 8) & 0xFF)
        pkt[3] = UInt8(sequenceNumber & 0xFF)
        // Bytes 4-7: timestamp (big-endian)
        pkt[4] = UInt8((timestamp >> 24) & 0xFF)
        pkt[5] = UInt8((timestamp >> 16) & 0xFF)
        pkt[6] = UInt8((timestamp >> 8) & 0xFF)
        pkt[7] = UInt8(timestamp & 0xFF)
        // Bytes 8-11: SSRC (big-endian)
        pkt[8]  = UInt8((ssrc >> 24) & 0xFF)
        pkt[9]  = UInt8((ssrc >> 16) & 0xFF)
        pkt[10] = UInt8((ssrc >> 8) & 0xFF)
        pkt[11] = UInt8(ssrc & 0xFF)

        // Extension header (4 bytes)
        // Bytes 12-13: profile = 0x4F53 ("OS")
        pkt[12] = 0x4F
        pkt[13] = 0x53
        // Bytes 14-15: extension length in 32-bit words = 2
        pkt[14] = 0x00
        pkt[15] = 0x02

        // Extension data (8 bytes = 2 words)
        // Bytes 16-17: stream_id — [15:14]=deckId, [13:10]=channels, [9:0]=streamIndex(0)
        let streamId = UInt16(((deckId & 0x3) << 14) | ((channels & 0xF) << 10))
        pkt[16] = UInt8((streamId >> 8) & 0xFF)
        pkt[17] = UInt8(streamId & 0xFF)
        // Bytes 18-19: reserved
        pkt[18] = 0x00
        pkt[19] = 0x00
        // Bytes 20-23: timestamp copy
        pkt[20] = pkt[4]
        pkt[21] = pkt[5]
        pkt[22] = pkt[6]
        pkt[23] = pkt[7]

        // Audio payload
        payload.withUnsafeBytes { ptr in
            guard let base = ptr.baseAddress else { return }
            _ = memcpy(&pkt[headerSize], base, payload.count)
        }

        // CRC-32 trailer (IEEE 802.3 polynomial, same as C++ impl)
        let crc = computeCRC32(pkt, count: headerSize + payload.count)
        let crcOffset = headerSize + payload.count
        pkt[crcOffset + 0] = UInt8((crc >> 24) & 0xFF)
        pkt[crcOffset + 1] = UInt8((crc >> 16) & 0xFF)
        pkt[crcOffset + 2] = UInt8((crc >> 8) & 0xFF)
        pkt[crcOffset + 3] = UInt8(crc & 0xFF)

        return Data(pkt)
    }

    /// Compute CRC-32 matching the C++ implementation.
    ///
    /// Uses IEEE 802.3 polynomial (0xEDB88320 reflected).
    public static func computeCRC32(_ data: [UInt8], count: Int) -> UInt32 {
        var crc: UInt32 = 0xFFFFFFFF
        for i in 0..<count {
            crc ^= UInt32(data[i])
            for _ in 0..<8 {
                crc = (crc >> 1) ^ ((crc & 1 != 0) ? 0xEDB88320 : 0)
            }
        }
        return ~crc
    }
}
