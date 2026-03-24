import Foundation

/// Metadata about the currently playing track.
public struct TrackInfo: Sendable {
    /// Track title.
    public let title: String
    /// Artist name, if available.
    public let artist: String?
    /// Channel this track is playing on.
    public let channel: String
    /// When this track info was received.
    public let timestamp: Date

    public init(title: String, artist: String? = nil, channel: String, timestamp: Date = Date()) {
        self.title = title
        self.artist = artist
        self.channel = channel
        self.timestamp = timestamp
    }
}

/// Subscribes to relay control messages and dispatches event callbacks.
///
/// Parses control messages from `RelayConnection.onControlMessage` and
/// fires typed callbacks for track changes, listener count updates,
/// and member join/leave events.
///
/// Usage:
/// ```swift
/// let webhooks = WebhookManager(channel: "jazz")
/// webhooks.onTrackChanged = { track in
///     print("Now playing: \(track.title)")
/// }
/// webhooks.onListenerCountChanged = { count in
///     print("\(count) listeners")
/// }
/// // Wire up to relay:
/// relay.onControlMessage = { msg in
///     webhooks.handleControlMessage(msg)
/// }
/// ```
public final class WebhookManager {

    // MARK: - Callbacks

    /// Called when a new track starts playing (META: message received).
    public var onTrackChanged: ((TrackInfo) -> Void)?

    /// Called when the listener count changes (LISTENERS: message).
    public var onListenerCountChanged: ((Int) -> Void)?

    /// Called when a member joins the channel (JOINED: message).
    public var onMemberJoined: ((String) -> Void)?

    /// Called when a member leaves the channel (LEFT: message).
    public var onMemberLeft: ((String) -> Void)?

    // MARK: - Private

    private let channel: String

    // MARK: - Init

    /// Create a webhook manager for a given channel.
    ///
    /// - Parameter channel: The channel name to associate with events.
    public init(channel: String) {
        self.channel = channel
    }

    // MARK: - Public API

    /// Handle a raw control message from the relay connection.
    ///
    /// Recognized message formats:
    /// - `META:<title>` or `META:<artist> - <title>` — track change
    /// - `LISTENERS:<count>` — listener count update
    /// - `JOINED:<name>` — member joined
    /// - `LEFT:<name>` — member left
    ///
    /// - Parameter message: The raw control message string.
    public func handleControlMessage(_ message: String) {
        let lines = message.split(separator: "\n", omittingEmptySubsequences: true)
        for line in lines {
            let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
            parseLine(trimmed)
        }
    }

    // MARK: - Private Parsing

    private func parseLine(_ line: String) {
        if line.hasPrefix("META:") {
            let content = String(line.dropFirst(5)).trimmingCharacters(in: .whitespaces)
            let trackInfo = parseTrackInfo(content)
            onTrackChanged?(trackInfo)

        } else if line.hasPrefix("LISTENERS:") {
            let countStr = String(line.dropFirst(10)).trimmingCharacters(in: .whitespaces)
            if let count = Int(countStr) {
                onListenerCountChanged?(count)
            }

        } else if line.hasPrefix("JOINED:") {
            let name = String(line.dropFirst(7)).trimmingCharacters(in: .whitespaces)
            if !name.isEmpty {
                onMemberJoined?(name)
            }

        } else if line.hasPrefix("LEFT:") {
            let name = String(line.dropFirst(5)).trimmingCharacters(in: .whitespaces)
            if !name.isEmpty {
                onMemberLeft?(name)
            }
        }
    }

    private func parseTrackInfo(_ content: String) -> TrackInfo {
        // Try "Artist - Title" format
        if let separatorRange = content.range(of: " - ") {
            let artist = String(content[content.startIndex..<separatorRange.lowerBound])
            let title = String(content[separatorRange.upperBound...])
            return TrackInfo(title: title, artist: artist, channel: channel)
        }
        // Just a title
        return TrackInfo(title: content, channel: channel)
    }
}
