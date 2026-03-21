//
//  SolunaWidgetData.swift
//  Soluna
//
//  Shared data store for the Home Screen Widget.
//  The main app writes playback state to a shared App Group UserDefaults.
//  A future Widget Extension target reads from the same suite to display
//  the current channel and playback status.
//

import Foundation

/// Keys for shared widget data in UserDefaults (App Group suite)
enum SolunaWidgetDataKey: String {
    case channel          = "widget_channel"
    case channelEmoji     = "widget_channel_emoji"
    case isPlaying        = "widget_is_playing"
    case packetsReceived  = "widget_packets_received"
    case lastUpdated      = "widget_last_updated"
    case trackTitle       = "widget_track_title"
    case trackArtist      = "widget_track_artist"
}

/// Writes playback state to a shared App Group for widget consumption
final class SolunaWidgetData {
    static let shared = SolunaWidgetData()

    /// App Group identifier — must match the Widget Extension's App Group
    static let appGroupID = "group.com.soluna.shared"

    private let defaults: UserDefaults?

    private init() {
        defaults = UserDefaults(suiteName: Self.appGroupID)
        if defaults == nil {
            print("[WidgetData] Warning: Could not create UserDefaults for suite \(Self.appGroupID). "
                + "Ensure the App Group is configured in the Signing & Capabilities tab.")
        }
    }

    /// Update the shared widget data with current playback state
    func update(
        channel: String,
        emoji: String = "",
        isPlaying: Bool,
        packetsReceived: UInt64 = 0,
        trackTitle: String? = nil,
        trackArtist: String? = nil
    ) {
        guard let defaults else { return }

        defaults.set(channel, forKey: SolunaWidgetDataKey.channel.rawValue)
        defaults.set(emoji, forKey: SolunaWidgetDataKey.channelEmoji.rawValue)
        defaults.set(isPlaying, forKey: SolunaWidgetDataKey.isPlaying.rawValue)
        defaults.set(packetsReceived, forKey: SolunaWidgetDataKey.packetsReceived.rawValue)
        defaults.set(Date().timeIntervalSince1970, forKey: SolunaWidgetDataKey.lastUpdated.rawValue)

        if let title = trackTitle {
            defaults.set(title, forKey: SolunaWidgetDataKey.trackTitle.rawValue)
        }
        if let artist = trackArtist {
            defaults.set(artist, forKey: SolunaWidgetDataKey.trackArtist.rawValue)
        }

        // Request widget timeline reload (requires WidgetKit import in widget target)
        // WidgetCenter.shared.reloadAllTimelines()
    }

    /// Clear widget data (e.g. when playback stops)
    func clear() {
        guard let defaults else { return }

        defaults.removeObject(forKey: SolunaWidgetDataKey.channel.rawValue)
        defaults.removeObject(forKey: SolunaWidgetDataKey.channelEmoji.rawValue)
        defaults.set(false, forKey: SolunaWidgetDataKey.isPlaying.rawValue)
        defaults.set(0, forKey: SolunaWidgetDataKey.packetsReceived.rawValue)
        defaults.set(Date().timeIntervalSince1970, forKey: SolunaWidgetDataKey.lastUpdated.rawValue)
        defaults.removeObject(forKey: SolunaWidgetDataKey.trackTitle.rawValue)
        defaults.removeObject(forKey: SolunaWidgetDataKey.trackArtist.rawValue)
    }

    // MARK: - Read (for widget target)

    /// Read current channel name
    var channel: String {
        defaults?.string(forKey: SolunaWidgetDataKey.channel.rawValue) ?? "soluna"
    }

    /// Read current playing state
    var isPlaying: Bool {
        defaults?.bool(forKey: SolunaWidgetDataKey.isPlaying.rawValue) ?? false
    }

    /// Read last update timestamp
    var lastUpdated: Date {
        let ts = defaults?.double(forKey: SolunaWidgetDataKey.lastUpdated.rawValue) ?? 0
        return Date(timeIntervalSince1970: ts)
    }
}
