import Foundation
#if os(iOS)
import UIKit
#endif
#if os(macOS)
import AppKit
#endif

/// Haptic feedback events.
public enum HapticEvent: Sendable {
    case connected
    case disconnected
    case beatDrop
    case channelSwitch
    case error
}

/// VoiceOver and accessibility support for Soluna SDK.
///
/// Provides accessibility announcements for connection state changes,
/// channel switches, and track changes. Also triggers haptic feedback
/// on supported devices.
///
/// Usage:
/// ```swift
/// let a11y = AccessibilityHelper()
/// a11y.announceConnectionState(.connected)
/// a11y.provideHapticFeedback(for: .connected)
/// ```
public final class AccessibilityHelper {

    // MARK: - Init

    public init() {}

    // MARK: - Announcements

    /// Announce a channel change via VoiceOver.
    ///
    /// - Parameter channel: The channel that was switched to.
    public func announceChannelChange(_ channel: SolunaChannel) {
        let message = "\(channel.name) channel selected"
        postAccessibilityAnnouncement(message)
    }

    /// Announce a connection state change via VoiceOver.
    ///
    /// - Parameter state: The new connection state.
    public func announceConnectionState(_ state: SolunaConnectionState) {
        let message: String
        switch state {
        case .connected:
            message = "Connected to Soluna relay"
        case .connecting:
            message = "Connecting to Soluna relay"
        case .disconnected:
            message = "Disconnected from Soluna relay"
        case .error(let detail):
            message = "Connection error: \(detail)"
        }
        postAccessibilityAnnouncement(message)
    }

    /// Announce a track change via VoiceOver.
    ///
    /// - Parameter title: The title of the new track.
    public func announceTrackChange(_ title: String) {
        let message = "Now playing: \(title)"
        postAccessibilityAnnouncement(message)
    }

    // MARK: - Haptic Feedback

    /// Provide haptic feedback for the given event.
    ///
    /// - Parameter event: The event type for haptic feedback.
    public func provideHapticFeedback(for event: HapticEvent) {
        #if os(iOS)
        switch event {
        case .connected:
            let generator = UINotificationFeedbackGenerator()
            generator.notificationOccurred(.success)
        case .disconnected:
            let generator = UINotificationFeedbackGenerator()
            generator.notificationOccurred(.warning)
        case .beatDrop:
            let generator = UIImpactFeedbackGenerator(style: .heavy)
            generator.impactOccurred()
        case .channelSwitch:
            let generator = UIImpactFeedbackGenerator(style: .medium)
            generator.impactOccurred()
        case .error:
            let generator = UINotificationFeedbackGenerator()
            generator.notificationOccurred(.error)
        }
        #endif
    }

    // MARK: - Private

    private func postAccessibilityAnnouncement(_ message: String) {
        #if os(iOS)
        DispatchQueue.main.async {
            UIAccessibility.post(notification: .announcement, argument: message)
        }
        #elseif os(macOS)
        DispatchQueue.main.async {
            NSAccessibility.post(element: NSApp as Any, notification: .announcementRequested, userInfo: [
                .announcement: message,
                .priority: NSAccessibilityPriorityLevel.high.rawValue
            ])
        }
        #endif
    }
}
