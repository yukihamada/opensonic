//
//  SolunaLiveActivity.swift
//  Soluna
//
//  Live Activity (Dynamic Island + Lock Screen) attributes and manager.
//  The actual Live Activity UI requires a WidgetKit extension target,
//  but ActivityKit APIs for start/update/stop work from the main app.
//

import Foundation
import SwiftUI
import ActivityKit

// MARK: - Activity Attributes

struct SolunaRadioAttributes: ActivityAttributes {
    public struct ContentState: Codable, Hashable {
        var channel: String
        var isPlaying: Bool
        var packetsReceived: UInt64
    }

    var channelEmoji: String
}

// MARK: - Live Activity Manager

@available(iOS 16.2, *)
@MainActor
class LiveActivityManager: ObservableObject {
    static let shared = LiveActivityManager()

    private var activity: Activity<SolunaRadioAttributes>?

    /// Whether Live Activities are supported and enabled on this device
    var isSupported: Bool {
        ActivityAuthorizationInfo().areActivitiesEnabled
    }

    /// Start a new Live Activity for the given channel
    func start(channel: String, emoji: String) {
        guard isSupported else { return }

        // End any existing activity first
        stop()

        let attributes = SolunaRadioAttributes(channelEmoji: emoji)
        let initialState = SolunaRadioAttributes.ContentState(
            channel: channel,
            isPlaying: true,
            packetsReceived: 0
        )

        do {
            let content = ActivityContent(state: initialState, staleDate: nil)
            activity = try Activity<SolunaRadioAttributes>.request(
                attributes: attributes,
                content: content,
                pushType: nil
            )
            print("[LiveActivity] Started for channel: \(channel)")
        } catch {
            print("[LiveActivity] Failed to start: \(error)")
        }
    }

    /// Update the Live Activity with current playback state
    func update(channel: String, isPlaying: Bool, packets: UInt64) {
        guard let activity else { return }

        let updatedState = SolunaRadioAttributes.ContentState(
            channel: channel,
            isPlaying: isPlaying,
            packetsReceived: packets
        )

        Task {
            let content = ActivityContent(state: updatedState, staleDate: nil)
            await activity.update(content)
        }
    }

    /// End the Live Activity
    func stop() {
        guard let activity else { return }

        let finalState = SolunaRadioAttributes.ContentState(
            channel: "",
            isPlaying: false,
            packetsReceived: 0
        )

        Task {
            let content = ActivityContent(state: finalState, staleDate: nil)
            await activity.end(content, dismissalPolicy: .immediate)
            print("[LiveActivity] Stopped")
        }
        self.activity = nil
    }
}
