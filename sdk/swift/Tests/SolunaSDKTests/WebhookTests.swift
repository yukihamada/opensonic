import XCTest
@testable import SolunaSDK

final class WebhookTests: XCTestCase {

    func testMetaMessageTriggersTrackChanged() {
        let wh = WebhookManager(channel: "jazz")
        var receivedTrack: TrackInfo?

        wh.onTrackChanged = { track in
            receivedTrack = track
        }

        wh.handleControlMessage("META:Miles Davis - So What\n")

        XCTAssertNotNil(receivedTrack)
        XCTAssertEqual(receivedTrack?.title, "So What")
        XCTAssertEqual(receivedTrack?.artist, "Miles Davis")
        XCTAssertEqual(receivedTrack?.channel, "jazz")
    }

    func testMetaMessageTitleOnly() {
        let wh = WebhookManager(channel: "lofi")
        var receivedTrack: TrackInfo?

        wh.onTrackChanged = { track in
            receivedTrack = track
        }

        wh.handleControlMessage("META:Chill Beats Vol 3\n")

        XCTAssertNotNil(receivedTrack)
        XCTAssertEqual(receivedTrack?.title, "Chill Beats Vol 3")
        XCTAssertNil(receivedTrack?.artist)
        XCTAssertEqual(receivedTrack?.channel, "lofi")
    }

    func testListenersMessageParsing() {
        let wh = WebhookManager(channel: "dance")
        var receivedCount: Int?

        wh.onListenerCountChanged = { count in
            receivedCount = count
        }

        wh.handleControlMessage("LISTENERS:42\n")

        XCTAssertEqual(receivedCount, 42)
    }

    func testListenersInvalidCountIgnored() {
        let wh = WebhookManager(channel: "dance")
        var callCount = 0

        wh.onListenerCountChanged = { _ in
            callCount += 1
        }

        wh.handleControlMessage("LISTENERS:not-a-number\n")
        XCTAssertEqual(callCount, 0)
    }

    func testJoinedMessage() {
        let wh = WebhookManager(channel: "jazz")
        var joinedName: String?

        wh.onMemberJoined = { name in
            joinedName = name
        }

        wh.handleControlMessage("JOINED:DJ Yuki\n")

        XCTAssertEqual(joinedName, "DJ Yuki")
    }

    func testLeftMessage() {
        let wh = WebhookManager(channel: "jazz")
        var leftName: String?

        wh.onMemberLeft = { name in
            leftName = name
        }

        wh.handleControlMessage("LEFT:DJ Yuki\n")

        XCTAssertEqual(leftName, "DJ Yuki")
    }

    func testEmptyJoinedIgnored() {
        let wh = WebhookManager(channel: "jazz")
        var callCount = 0

        wh.onMemberJoined = { _ in callCount += 1 }
        wh.handleControlMessage("JOINED: \n")

        XCTAssertEqual(callCount, 0)
    }

    func testEmptyLeftIgnored() {
        let wh = WebhookManager(channel: "jazz")
        var callCount = 0

        wh.onMemberLeft = { _ in callCount += 1 }
        wh.handleControlMessage("LEFT: \n")

        XCTAssertEqual(callCount, 0)
    }

    func testMultipleMessagesInOneString() {
        let wh = WebhookManager(channel: "jazz")
        var trackCount = 0
        var listenerCount = 0

        wh.onTrackChanged = { _ in trackCount += 1 }
        wh.onListenerCountChanged = { _ in listenerCount += 1 }

        wh.handleControlMessage("META:Track Title\nLISTENERS:10\n")

        XCTAssertEqual(trackCount, 1)
        XCTAssertEqual(listenerCount, 1)
    }

    func testUnknownMessageIgnored() {
        let wh = WebhookManager(channel: "jazz")
        var callbackCount = 0

        wh.onTrackChanged = { _ in callbackCount += 1 }
        wh.onListenerCountChanged = { _ in callbackCount += 1 }
        wh.onMemberJoined = { _ in callbackCount += 1 }
        wh.onMemberLeft = { _ in callbackCount += 1 }

        wh.handleControlMessage("UNKNOWN:some data\n")
        wh.handleControlMessage("HELLO\n")

        XCTAssertEqual(callbackCount, 0)
    }

    func testTrackInfoInit() {
        let info = TrackInfo(title: "Test", artist: "Artist", channel: "jazz")
        XCTAssertEqual(info.title, "Test")
        XCTAssertEqual(info.artist, "Artist")
        XCTAssertEqual(info.channel, "jazz")
    }
}
