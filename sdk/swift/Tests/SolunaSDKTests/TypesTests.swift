import XCTest
@testable import SolunaSDK

final class TypesTests: XCTestCase {

    // MARK: - SolunaChannels

    func testChannelLookupByIdFound() {
        let jazz = SolunaChannels.channel(for: "jazz")
        XCTAssertNotNil(jazz)
        XCTAssertEqual(jazz?.name, "Jazz")
        XCTAssertEqual(jazz?.id, "jazz")
    }

    func testChannelLookupByIdNotFound() {
        XCTAssertNil(SolunaChannels.channel(for: "nonexistent"))
    }

    func testAllChannelsCount() {
        XCTAssertEqual(SolunaChannels.all.count, 7)
    }

    func testAllChannelIdsUnique() {
        let ids = SolunaChannels.all.map { $0.id }
        XCTAssertEqual(Set(ids).count, ids.count)
    }

    func testAllChannelLookups() {
        for channel in SolunaChannels.all {
            let found = SolunaChannels.channel(for: channel.id)
            XCTAssertNotNil(found)
            XCTAssertEqual(found?.name, channel.name)
        }
    }

    // MARK: - SolunaFanRank

    func testFanRankNewFan() {
        XCTAssertEqual(SolunaFanRank.from(minutes: 0), .newFan)
        XCTAssertEqual(SolunaFanRank.from(minutes: 15), .newFan)
        XCTAssertEqual(SolunaFanRank.from(minutes: 29.9), .newFan)
    }

    func testFanRankRegular() {
        XCTAssertEqual(SolunaFanRank.from(minutes: 30), .regular)
        XCTAssertEqual(SolunaFanRank.from(minutes: 60), .regular)
        XCTAssertEqual(SolunaFanRank.from(minutes: 119.9), .regular)
    }

    func testFanRankSuperfan() {
        XCTAssertEqual(SolunaFanRank.from(minutes: 120), .superfan)
        XCTAssertEqual(SolunaFanRank.from(minutes: 300), .superfan)
        XCTAssertEqual(SolunaFanRank.from(minutes: 599.9), .superfan)
    }

    func testFanRankLegend() {
        XCTAssertEqual(SolunaFanRank.from(minutes: 600), .legend)
        XCTAssertEqual(SolunaFanRank.from(minutes: 10000), .legend)
    }

    func testFanRankBadges() {
        XCTAssertFalse(SolunaFanRank.newFan.badge.isEmpty)
        XCTAssertFalse(SolunaFanRank.regular.badge.isEmpty)
        XCTAssertFalse(SolunaFanRank.superfan.badge.isEmpty)
        XCTAssertFalse(SolunaFanRank.legend.badge.isEmpty)
    }

    // MARK: - OSTConstants

    func testOSTConstantsValues() {
        XCTAssertEqual(OSTConstants.ostpProfile, 0x4F53)
        XCTAssertEqual(OSTConstants.rtpHeaderSize, 12)
        XCTAssertEqual(OSTConstants.crcTrailerSize, 4)
        XCTAssertEqual(OSTConstants.defaultPort, 5100)
        XCTAssertEqual(OSTConstants.defaultHost, "relay.solun.art")
        XCTAssertEqual(OSTConstants.heartbeatInterval, 5.0)
        XCTAssertEqual(OSTConstants.recvBufferSize, 16384)
        XCTAssertEqual(OSTConstants.ptADPCMStereo, 115)
        XCTAssertEqual(OSTConstants.ptADPCMMono, 116)
        XCTAssertEqual(OSTConstants.ptOpus, 98)
        XCTAssertEqual(OSTConstants.ptLC3, 119)
    }

    // MARK: - ADPCMState

    func testADPCMStateDefaults() {
        let state = ADPCMState()
        XCTAssertEqual(state.predicted, 0)
        XCTAssertEqual(state.stepIndex, 0)
    }

    func testADPCMStateClamps() {
        let state = ADPCMState(predicted: 100, stepIndex: 200)
        XCTAssertEqual(state.stepIndex, 88) // clamped to max
    }

    // MARK: - OSTPacket

    func testOSTPacketStruct() {
        let packet = OSTPacket(
            payloadType: 96,
            channels: 2,
            deckId: 1,
            payload: Data([0xFF]),
            sequenceNumber: 42,
            timestamp: 96000
        )
        XCTAssertEqual(packet.payloadType, 96)
        XCTAssertEqual(packet.channels, 2)
        XCTAssertEqual(packet.deckId, 1)
        XCTAssertEqual(packet.payload.count, 1)
        XCTAssertEqual(packet.sequenceNumber, 42)
        XCTAssertEqual(packet.timestamp, 96000)
    }

    // MARK: - SolunaConnectionState

    func testConnectionStateEquality() {
        // Basic smoke test that states can be created
        let _ = SolunaConnectionState.disconnected
        let _ = SolunaConnectionState.connecting
        let _ = SolunaConnectionState.connected
        let _ = SolunaConnectionState.error("test")
    }

    // MARK: - Endpoint

    func testEndpointId() {
        let ep = Endpoint(host: "relay.solun.art", port: 5100)
        XCTAssertEqual(ep.id, "relay.solun.art:5100")
    }

    func testEndpointEquality() {
        let ep1 = Endpoint(host: "relay.solun.art", port: 5100)
        let ep2 = Endpoint(host: "relay.solun.art", port: 5100)
        let ep3 = Endpoint(host: "other.solun.art", port: 5100)
        XCTAssertEqual(ep1, ep2)
        XCTAssertNotEqual(ep1, ep3)
    }

    func testEndpointHashable() {
        let ep1 = Endpoint(host: "relay.solun.art", port: 5100)
        let ep2 = Endpoint(host: "relay.solun.art", port: 5100)
        var set = Set<Endpoint>()
        set.insert(ep1)
        set.insert(ep2)
        XCTAssertEqual(set.count, 1)
    }
}
