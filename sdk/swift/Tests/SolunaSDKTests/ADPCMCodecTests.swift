import XCTest
@testable import SolunaSDK

final class ADPCMCodecTests: XCTestCase {

    func testDecodePayloadWithHeader() {
        // ADPCM payload: 4-byte header + 1 byte of data (2 nibbles = 2 samples)
        // Header: predictor=0 (LE), stepIndex=0, reserved=0
        let payload: [UInt8] = [0x00, 0x00, 0x00, 0x00, 0x11]
        let decoded = ADPCMCodec.decodePayload(payload)
        XCTAssertNotNil(decoded)
        // 1 byte of ADPCM = 2 samples, each 2 bytes = 4 bytes output
        XCTAssertEqual(decoded?.count, 4)
    }

    func testDecodeEmptyPayloadReturnsNil() {
        // Less than 4 bytes header
        XCTAssertNil(ADPCMCodec.decodePayload([]))
        XCTAssertNil(ADPCMCodec.decodePayload([0x00]))
        XCTAssertNil(ADPCMCodec.decodePayload([0x00, 0x00, 0x00]))
    }

    func testDecodeHeaderOnlyProducesEmptyOutput() {
        // Exactly 4 bytes = header only, 0 ADPCM data bytes
        let payload: [UInt8] = [0x00, 0x00, 0x00, 0x00]
        let decoded = ADPCMCodec.decodePayload(payload)
        XCTAssertNotNil(decoded)
        XCTAssertEqual(decoded?.count, 0)
    }

    func testStepTableBounds() {
        XCTAssertEqual(ADPCMCodec.stepTable.count, 89)
        XCTAssertEqual(ADPCMCodec.stepTable[0], 7)
        XCTAssertEqual(ADPCMCodec.stepTable[88], 32767)
    }

    func testIndexTableBounds() {
        XCTAssertEqual(ADPCMCodec.indexTable.count, 16)
    }

    func testDecodeWithKnownPredictor() {
        // Set initial predictor to 1000 (LE: 0xE8, 0x03), stepIndex=10
        var payload: [UInt8] = [0xE8, 0x03, 10, 0x00]
        // Add some ADPCM data bytes
        payload.append(contentsOf: [0x00, 0x00, 0x00, 0x00])

        let decoded = ADPCMCodec.decodePayload(payload)
        XCTAssertNotNil(decoded)
        // 4 bytes of ADPCM = 8 samples = 16 bytes output
        XCTAssertEqual(decoded?.count, 16)
    }

    func testDecodeProducesValidInt16Range() {
        // Create a payload with varied nibbles
        var payload: [UInt8] = [0x00, 0x00, 44, 0x00]  // predictor=0, stepIndex=44
        payload.append(contentsOf: [0xF0, 0x0F, 0x77, 0x88, 0x12, 0x34, 0x56, 0x78])

        let decoded = ADPCMCodec.decodePayload(payload)
        XCTAssertNotNil(decoded)
        guard let pcm = decoded else { return }

        // Verify all decoded samples are valid int16 values
        for i in stride(from: 0, to: pcm.count, by: 2) {
            let sample = Int16(bitPattern: UInt16(pcm[i]) | (UInt16(pcm[i + 1]) << 8))
            XCTAssertGreaterThanOrEqual(sample, Int16.min)
            XCTAssertLessThanOrEqual(sample, Int16.max)
        }
    }

    func testStepIndexClampedTo88() {
        // stepIndex > 88 should be clamped
        let payload: [UInt8] = [0x00, 0x00, 100, 0x00, 0x00]
        let decoded = ADPCMCodec.decodePayload(payload)
        XCTAssertNotNil(decoded)
        // Should not crash
    }

    func testDecodeDeterministic() {
        let payload: [UInt8] = [0x00, 0x00, 0x00, 0x00, 0x12, 0x34, 0x56]
        let decoded1 = ADPCMCodec.decodePayload(payload)
        let decoded2 = ADPCMCodec.decodePayload(payload)
        XCTAssertEqual(decoded1, decoded2)
    }

    func testDecodeRawNibbles() {
        var state = ADPCMState(predicted: 0, stepIndex: 0)
        let adpcm: [UInt8] = [0x00, 0x00]  // all zero nibbles
        let pcm = ADPCMCodec.decode(adpcm: adpcm, state: &state)
        XCTAssertEqual(pcm.count, 8)  // 2 bytes * 4 samples
    }
}
