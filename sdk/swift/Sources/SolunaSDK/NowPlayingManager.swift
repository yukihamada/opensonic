import Foundation
import MediaPlayer
#if os(iOS)
import UIKit
#endif

/// Manages Lock Screen and Control Center now-playing integration.
///
/// Shows channel name, emoji, and "Soluna Radio" as artist in the system media controls.
/// Supports play/pause/next channel remote commands.
///
/// Usage:
/// ```swift
/// let nowPlaying = NowPlayingManager()
/// nowPlaying.setupRemoteCommands(
///     onPlay:  { client.connect(channel: "soluna") },
///     onPause: { client.disconnect() },
///     onNext:  { client.setChannel("jazz") }
/// )
/// nowPlaying.update(channel: channel, title: "Live Radio")
/// ```
public final class NowPlayingManager {

    // MARK: - Private

    private let infoCenter = MPNowPlayingInfoCenter.default()
    private let commandCenter = MPRemoteCommandCenter.shared()

    private var playTarget: Any?
    private var pauseTarget: Any?
    private var nextTarget: Any?

    // MARK: - Init

    public init() {}

    deinit {
        removeRemoteCommands()
    }

    // MARK: - Public API

    /// Update the now-playing metadata displayed on Lock Screen / Control Center.
    ///
    /// - Parameters:
    ///   - channel: The current `SolunaChannel` (provides emoji and name).
    ///   - title: Optional title override (e.g. from a META: packet). Defaults to channel name.
    public func update(channel: SolunaChannel, title: String? = nil) {
        var info = [String: Any]()
        info[MPMediaItemPropertyTitle] = title ?? "\(channel.emoji) \(channel.name)"
        info[MPMediaItemPropertyArtist] = "Soluna Radio"
        info[MPMediaItemPropertyAlbumTitle] = channel.name
        info[MPNowPlayingInfoPropertyIsLiveStream] = true
        info[MPNowPlayingInfoPropertyPlaybackRate] = 1.0

        infoCenter.nowPlayingInfo = info
    }

    /// Set the playback state shown in the system media controls.
    ///
    /// - Parameter playing: `true` for playing, `false` for paused.
    public func setPlaybackState(_ playing: Bool) {
        infoCenter.playbackState = playing ? .playing : .paused
    }

    /// Register remote command handlers for play, pause, and next track.
    ///
    /// - Parameters:
    ///   - onPlay: Called when the user taps play in Control Center / Lock Screen.
    ///   - onPause: Called when the user taps pause.
    ///   - onNext: Called when the user taps next track (skip to next channel).
    public func setupRemoteCommands(
        onPlay: @escaping () -> Void,
        onPause: @escaping () -> Void,
        onNext: @escaping () -> Void
    ) {
        removeRemoteCommands()

        playTarget = commandCenter.playCommand.addTarget { _ in
            onPlay()
            return .success
        }
        commandCenter.playCommand.isEnabled = true

        pauseTarget = commandCenter.pauseCommand.addTarget { _ in
            onPause()
            return .success
        }
        commandCenter.pauseCommand.isEnabled = true

        nextTarget = commandCenter.nextTrackCommand.addTarget { _ in
            onNext()
            return .success
        }
        commandCenter.nextTrackCommand.isEnabled = true

        // Disable unused commands
        commandCenter.previousTrackCommand.isEnabled = false
        commandCenter.seekForwardCommand.isEnabled = false
        commandCenter.seekBackwardCommand.isEnabled = false
        commandCenter.changePlaybackPositionCommand.isEnabled = false

        #if os(iOS)
        UIApplication.shared.beginReceivingRemoteControlEvents()
        #endif
    }

    // MARK: - Private

    private func removeRemoteCommands() {
        if let target = playTarget {
            commandCenter.playCommand.removeTarget(target)
            playTarget = nil
        }
        if let target = pauseTarget {
            commandCenter.pauseCommand.removeTarget(target)
            pauseTarget = nil
        }
        if let target = nextTarget {
            commandCenter.nextTrackCommand.removeTarget(target)
            nextTarget = nil
        }
    }
}
