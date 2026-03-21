import GroupActivities
import SwiftUI

// The group activity
struct SolunaListenActivity: GroupActivity {
    var metadata: GroupActivityMetadata {
        var meta = GroupActivityMetadata()
        meta.title = "Listen to Soluna Radio"
        meta.subtitle = "Tune in together"
        meta.type = .listenTogether
        return meta
    }

    let channel: String
}

// Manager that handles SharePlay sessions
@MainActor
class SharePlayManager: ObservableObject {
    static let shared = SharePlayManager()

    @Published var isSharePlaying = false
    @Published var participantCount = 0

    private var groupSession: GroupSession<SolunaListenActivity>?
    private var tasks = Set<Task<Void, Never>>()

    func startSharing(channel: String) async {
        let activity = SolunaListenActivity(channel: channel)
        // This will show the SharePlay picker if in FaceTime
        do {
            _ = try await activity.activate()
        } catch {
            print("[SharePlay] Activation failed: \(error)")
        }
    }

    func configureGroupSession(_ session: GroupSession<SolunaListenActivity>) {
        self.groupSession = session

        let messenger = GroupSessionMessenger(session: session)

        // Listen for channel changes from other participants
        let channelTask = Task { @MainActor in
            for await (channel, _) in messenger.messages(of: String.self) {
                // Switch to the channel that was shared
                UserDefaults.standard.set(channel, forKey: "channel")
                NotificationCenter.default.post(name: .solunaChannelChanged, object: nil, userInfo: ["channel": channel])
            }
        }
        tasks.insert(channelTask)

        // Track participants
        let participantTask = Task { @MainActor in
            for await participants in session.$activeParticipants.values {
                self.participantCount = participants.count
            }
        }
        tasks.insert(participantTask)

        let stateTask = Task { @MainActor in
            for await state in session.$state.values {
                switch state {
                case .joined:
                    isSharePlaying = true
                case .invalidated:
                    isSharePlaying = false
                    groupSession = nil
                    tasks.forEach { $0.cancel() }
                    tasks.removeAll()
                default:
                    break
                }
            }
        }
        tasks.insert(stateTask)

        session.join()
    }

    func sendChannelChange(_ channel: String) {
        guard let session = groupSession else { return }
        let messenger = GroupSessionMessenger(session: session)
        Task {
            try? await messenger.send(channel)
        }
    }

    func leave() {
        groupSession?.leave()
        groupSession = nil
        isSharePlaying = false
        tasks.forEach { $0.cancel() }
        tasks.removeAll()
    }

    /// Call this once on app launch to listen for incoming sessions
    func observeSessions() {
        Task { @MainActor in
            for await session in SolunaListenActivity.sessions() {
                configureGroupSession(session)
            }
        }
    }
}

extension Notification.Name {
    static let solunaChannelChanged = Notification.Name("solunaChannelChanged")
}
