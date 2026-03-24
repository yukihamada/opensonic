import XCTest
@testable import SolunaSDK

final class OSTPacketTests: XCTestCase {

    // MARK: - OSTPacketParser

    func testParseValidOSTPPacket() {
        // Build a valid OSTP packet, then parse it
        let payload = Data([0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08])
        let packet = OSTPacketBuilder.buildPacket(
            payload: payload,
            payloadType: 96,
            sequenceNumber: 1234,
            timestamp: 48000,
            ssrc: 0xDEADBEEF,
            channels: 2,
            deckId: 1
        )

        let parsed = OSTPacketParser.parse(packet)
        XCTAssertNotNil(parsed)
        XCTAssertEqual(parsed?.payloadType, 96)
        XCTAssertEqual(parsed?.sequenceNumber, 1234)
        XCTAssertEqual(parsed?.timestamp, 48000)
        XCTAssertEqual(parsed?.channels, 2)
        XCTAssertEqual(parsed?.deckId, 1)
        XCTAssertEqual(parsed?.payload, payload)
    }

    func testParseLegacyRTPPacketNoExtension() {
        // Build a minimal RTP packet without extension bit set
        // V=2, P=0, X=0, CC=0 -> 0x80
        var pkt = [UInt8](repeating: 0, count: 20)
        pkt[0] = 0x80  // No extension
        pkt[1] = 96    // PT=96
        pkt[2] = 0x00; pkt[3] = 0x0A  // seq=10
        pkt[4] = 0x00; pkt[5] = 0x00; pkt[6] = 0xBB; pkt[7] = 0x80  // timestamp=48000
        // Bytes 8-11: SSRC
        // Bytes 12-19: payload (8 bytes, no CRC since payload <= crcTrailerSize check)
        for i in 12..<20 { pkt[i] = UInt8(i) }

        let data = Data(pkt)
        let parsed = OSTPacketParser.parse(data)
        XCTAssertNotNil(parsed)
        XCTAssertEqual(parsed?.sequenceNumber, 10)
        XCTAssertEqual(parsed?.channels, 2) // default stereo
        XCTAssertEqual(parsed?.deckId, 0)
    }

    func testParseTooShortPacketReturnsNil() {
        let shortData = Data([0x80, 0x60, 0x00])  // Only 3 bytes
        XCTAssertNil(OSTPacketParser.parse(shortData))
    }

    func testParseNonRTPPacketReturnsNil() {
        // First two bits of byte[0] should be 0b10 (version 2)
        // Use 0b00 instead
        var pkt = [UInt8](repeating: 0, count: 20)
        pkt[0] = 0x00  // Version 0
        let data = Data(pkt)
        XCTAssertNil(OSTPacketParser.parse(data))
    }

    func testChannelCountExtraction() {
        // Build packets with different channel counts
        for channels in [1, 2, 4, 6] {
            let payload = Data([0xAA, 0xBB, 0xCC, 0xDD, 0xEE])
            let packet = OSTPacketBuilder.buildPacket(
                payload: payload,
                sequenceNumber: 1,
                timestamp: 0,
                ssrc: 0,
                channels: channels
            )
            let parsed = OSTPacketParser.parse(packet)
            XCTAssertNotNil(parsed)
            XCTAssertEqual(parsed?.channels, channels, "Expected \(channels) channels")
        }
    }

    func testDeckIdExtraction() {
        for deckId in 0...3 {
            let payload = Data([0x01, 0x02, 0x03, 0x04, 0x05])
            let packet = OSTPacketBuilder.buildPacket(
                payload: payload,
                sequenceNumber: 1,
                timestamp: 0,
                ssrc: 0,
                channels: 2,
                deckId: deckId
            )
            let parsed = OSTPacketParser.parse(packet)
            XCTAssertNotNil(parsed)
            XCTAssertEqual(parsed?.deckId, deckId, "Expected deckId \(deckId)")
        }
    }

    func testSequenceNumberAndTimestamp() {
        let payload = Data([0xFF])
        let packet = OSTPacketBuilder.buildPacket(
            payload: payload,
            sequenceNumber: 65535,
            timestamp: 0xFFFFFFFF,
            ssrc: 12345
        )
        let parsed = OSTPacketParser.parse(packet)
        XCTAssertNotNil(parsed)
        XCTAssertEqual(parsed?.sequenceNumber, 65535)
        XCTAssertEqual(parsed?.timestamp, 0xFFFFFFFF)
    }

    // MARK: - OSTPacketBuilder

    func testBuildPacketRoundTrip() {
        let originalPayload = Data((0..<100).map { UInt8($0 & 0xFF) })
        let packet = OSTPacketBuilder.buildPacket(
            payload: originalPayload,
            payloadType: 115,
            sequenceNumber: 42,
            timestamp: 96000,
            ssrc: 0x12345678,
            channels: 6,
            deckId: 2
        )

        // Verify total size: header(24) + payload(100) + CRC(4) = 128
        XCTAssertEqual(packet.count, 128)

        let parsed = OSTPacketParser.parse(packet)
        XCTAssertNotNil(parsed)
        XCTAssertEqual(parsed?.payloadType, 115)
        XCTAssertEqual(parsed?.sequenceNumber, 42)
        XCTAssertEqual(parsed?.timestamp, 96000)
        XCTAssertEqual(parsed?.channels, 6)
        XCTAssertEqual(parsed?.deckId, 2)
        XCTAssertEqual(parsed?.payload, originalPayload)
    }

    func testCRC32TrailerGeneration() {
        let payload = Data([0xDE, 0xAD, 0xBE, 0xEF])
        let packet = OSTPacketBuilder.buildPacket(
            payload: payload,
            sequenceNumber: 0,
            timestamp: 0,
            ssrc: 0
        )

        // Extract CRC from last 4 bytes
        let crcBytes = packet.suffix(4)
        let crc = (UInt32(crcBytes[crcBytes.startIndex]) << 24) |
                  (UInt32(crcBytes[crcBytes.startIndex + 1]) << 16) |
                  (UInt32(crcBytes[crcBytes.startIndex + 2]) << 8) |
                  UInt32(crcBytes[crcBytes.startIndex + 3])

        // Recompute CRC over header + payload
        let headerAndPayload = Array(packet.prefix(packet.count - 4))
        let expectedCRC = OSTPacketBuilder.computeCRC32(headerAndPayload, count: headerAndPayload.count)
        XCTAssertEqual(crc, expectedCRC)
    }

    func testCRC32KnownValue() {
        // CRC-32 of empty data
        let emptyCRC = OSTPacketBuilder.computeCRC32([], count: 0)
        XCTAssertEqual(emptyCRC, 0x00000000)

        // CRC-32 of "123456789" should be 0xCBF43926
        let testData: [UInt8] = Array("123456789".utf8)
        let crc = OSTPacketBuilder.computeCRC32(testData, count: testData.count)
        XCTAssertEqual(crc, 0xCBF43926)
    }

    func testBuilderExtensionProfile() {
        let packet = OSTPacketBuilder.buildPacket(
            payload: Data([0x00]),
            sequenceNumber: 0,
            timestamp: 0,
            ssrc: 0
        )
        // Byte 12-13 should be OSTP profile 0x4F53
        XCTAssertEqual(packet[12], 0x4F)
        XCTAssertEqual(packet[13], 0x53)
        // Extension bit should be set (byte 0 & 0x10)
        XCTAssertTrue((packet[0] & 0x10) != 0)
    }
}
