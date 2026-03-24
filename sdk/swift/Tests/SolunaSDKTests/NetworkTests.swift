import XCTest
@testable import SolunaSDK

@MainActor
final class NetworkTests: XCTestCase {

    // MARK: - LoadBalancer

    func testRoundRobinSelection() {
        let lb = LoadBalancer(strategy: .roundRobin)
        let ep1 = Endpoint(host: "a.relay.solun.art", port: 5100)
        let ep2 = Endpoint(host: "b.relay.solun.art", port: 5100)
        let ep3 = Endpoint(host: "c.relay.solun.art", port: 5100)

        lb.addEndpoint(ep1)
        lb.addEndpoint(ep2)
        lb.addEndpoint(ep3)

        let selected1 = lb.selectEndpoint()
        let selected2 = lb.selectEndpoint()
        let selected3 = lb.selectEndpoint()
        let selected4 = lb.selectEndpoint()

        XCTAssertEqual(selected1, ep1)
        XCTAssertEqual(selected2, ep2)
        XCTAssertEqual(selected3, ep3)
        XCTAssertEqual(selected4, ep1)  // wraps around
    }

    func testCircuitBreakerTripsAfterThreshold() {
        let lb = LoadBalancer(strategy: .roundRobin)
        lb.circuitBreakerThreshold = 3

        let ep1 = Endpoint(host: "relay1.solun.art", port: 5100)
        let ep2 = Endpoint(host: "relay2.solun.art", port: 5100)
        lb.addEndpoint(ep1)
        lb.addEndpoint(ep2)

        // Report 3 failures on ep1
        lb.reportFailure(endpoint: ep1)
        lb.reportFailure(endpoint: ep1)
        lb.reportFailure(endpoint: ep1)

        // ep1 should now be cooling down
        XCTAssertTrue(lb.endpoints[0].isCoolingDown)
        XCTAssertEqual(lb.endpoints[0].failureCount, 3)
    }

    func testCircuitBreakerSkipsCoolingDownEndpoints() {
        let lb = LoadBalancer(strategy: .roundRobin)
        lb.circuitBreakerThreshold = 2

        let ep1 = Endpoint(host: "relay1.solun.art", port: 5100)
        let ep2 = Endpoint(host: "relay2.solun.art", port: 5100)
        lb.addEndpoint(ep1)
        lb.addEndpoint(ep2)

        // Trip circuit breaker on ep1
        lb.reportFailure(endpoint: ep1)
        lb.reportFailure(endpoint: ep1)

        // All selections should return ep2 (the only healthy one)
        for _ in 0..<5 {
            let selected = lb.selectEndpoint()
            XCTAssertEqual(selected, ep2)
        }
    }

    func testSuccessResetsFailureCount() {
        let lb = LoadBalancer(strategy: .roundRobin)
        let ep = Endpoint(host: "relay.solun.art", port: 5100)
        lb.addEndpoint(ep)

        lb.reportFailure(endpoint: ep)
        lb.reportFailure(endpoint: ep)
        XCTAssertEqual(lb.endpoints[0].failureCount, 2)

        lb.reportSuccess(endpoint: ep, latencyMs: 50.0)
        XCTAssertEqual(lb.endpoints[0].failureCount, 0)
        XCTAssertFalse(lb.endpoints[0].isCoolingDown)
    }

    func testLeastLatencySelection() {
        let lb = LoadBalancer(strategy: .leastLatency)
        let ep1 = Endpoint(host: "slow.relay.solun.art", port: 5100)
        let ep2 = Endpoint(host: "fast.relay.solun.art", port: 5100)

        lb.addEndpoint(ep1)
        lb.addEndpoint(ep2)

        lb.reportSuccess(endpoint: ep1, latencyMs: 100.0)
        lb.reportSuccess(endpoint: ep2, latencyMs: 20.0)

        let selected = lb.selectEndpoint()
        XCTAssertEqual(selected, ep2)
    }

    func testAddDuplicateEndpointIgnored() {
        let lb = LoadBalancer()
        let ep = Endpoint(host: "relay.solun.art", port: 5100)
        lb.addEndpoint(ep)
        lb.addEndpoint(ep)
        XCTAssertEqual(lb.endpoints.count, 1)
    }

    func testRemoveEndpoint() {
        let lb = LoadBalancer()
        let ep = Endpoint(host: "relay.solun.art", port: 5100)
        lb.addEndpoint(ep)
        XCTAssertEqual(lb.endpoints.count, 1)
        lb.removeEndpoint(ep)
        XCTAssertEqual(lb.endpoints.count, 0)
    }

    func testSelectEndpointReturnsNilWhenEmpty() {
        let lb = LoadBalancer()
        XCTAssertNil(lb.selectEndpoint())
    }

    func testLatencyExponentialMovingAverage() {
        let lb = LoadBalancer()
        let ep = Endpoint(host: "relay.solun.art", port: 5100)
        lb.addEndpoint(ep)

        lb.reportSuccess(endpoint: ep, latencyMs: 100.0)
        XCTAssertEqual(lb.endpoints[0].avgLatencyMs, 100.0)

        lb.reportSuccess(endpoint: ep, latencyMs: 50.0)
        // EMA: 100 * 0.7 + 50 * 0.3 = 85
        XCTAssertEqual(lb.endpoints[0].avgLatencyMs, 85.0, accuracy: 0.1)
    }

    // MARK: - BandwidthController

    func testUnlimitedBandwidthAlwaysAllows() {
        let bw = BandwidthController()
        bw.maxBandwidthKbps = 0  // unlimited
        XCTAssertTrue(bw.shouldSend(packetSize: 100_000))
    }

    func testTokenBucketDeniesOverLimit() {
        let bw = BandwidthController()
        bw.maxBandwidthKbps = 100  // 100 kbps
        bw.burstAllowanceKbps = 0  // no burst

        // Token bucket starts with capacity for 1 second of data
        // 100 kbps = 100,000 bits = 12,500 bytes per second
        // Try to send a packet much larger than the bucket
        XCTAssertFalse(bw.shouldSend(packetSize: 50_000))
    }

    func testTokenBucketAllowsUnderLimit() {
        let bw = BandwidthController()
        bw.maxBandwidthKbps = 1000  // 1 Mbps
        bw.burstAllowanceKbps = 100

        // 1000 kbps = 1,000,000 bits + 100,000 burst = 1,100,000 bits = 137,500 bytes
        // Small packet should be allowed
        XCTAssertTrue(bw.shouldSend(packetSize: 1200))
    }

    func testRecordSentUpdatesTotals() {
        let bw = BandwidthController()
        bw.recordSent(bytes: 1000)
        bw.recordSent(bytes: 500)
        XCTAssertEqual(bw.stats.sentBytesTotal, 1500)
    }

    func testRecordReceivedUpdatesTotals() {
        let bw = BandwidthController()
        bw.recordReceived(bytes: 2000)
        bw.recordReceived(bytes: 3000)
        XCTAssertEqual(bw.stats.receivedBytesTotal, 5000)
    }

    func testResetClearsStats() {
        let bw = BandwidthController()
        bw.recordSent(bytes: 1000)
        bw.recordReceived(bytes: 2000)
        bw.reset()
        XCTAssertEqual(bw.stats.sentBytesTotal, 0)
        XCTAssertEqual(bw.stats.receivedBytesTotal, 0)
    }

    // MARK: - QoSManager

    func testQoSManagerInitialState() {
        let qos = QoSManager()
        XCTAssertEqual(qos.currentQuality, .excellent)
        XCTAssertEqual(qos.currentCodec, .s24)
    }

    func testQoSManagerResetClearsStats() {
        let qos = QoSManager()
        qos.recordPacket(sequenceNumber: 1, size: 100)
        qos.recordPacket(sequenceNumber: 5, size: 100)
        qos.reset()
        XCTAssertEqual(qos.currentQuality, .excellent)
        XCTAssertEqual(qos.stats.packetLossPercent, 0)
    }

    func testQoSManagerRecordRTT() {
        let qos = QoSManager()
        qos.recordRTT(42.5)
        XCTAssertEqual(qos.stats.rttMs, 42.5)
    }

    func testPreferredCodecOverridesAuto() {
        let qos = QoSManager()
        qos.preferredCodec = .adpcm
        XCTAssertEqual(qos.currentCodec, .adpcm)
    }

    func testNetworkQualityComparable() {
        XCTAssertTrue(NetworkQuality.poor < NetworkQuality.fair)
        XCTAssertTrue(NetworkQuality.fair < NetworkQuality.good)
        XCTAssertTrue(NetworkQuality.good < NetworkQuality.excellent)
    }

    func testAudioCodecPayloadTypes() {
        XCTAssertEqual(AudioCodec.s24.payloadType, 97)
        XCTAssertEqual(AudioCodec.opus.payloadType, OSTConstants.ptOpus)
        XCTAssertEqual(AudioCodec.lc3.payloadType, OSTConstants.ptLC3)
        XCTAssertEqual(AudioCodec.adpcm.payloadType, OSTConstants.ptADPCMStereo)
    }
}
