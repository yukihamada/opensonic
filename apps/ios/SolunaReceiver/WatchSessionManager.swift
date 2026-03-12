import Foundation
import WatchConnectivity

@MainActor
final class WatchSessionManager: NSObject, ObservableObject {
    static let shared = WatchSessionManager()

    private var session: WCSession?
    weak var receiver: AudioReceiver?

    private override init() {
        super.init()
    }

    func activate(receiver: AudioReceiver) {
        self.receiver = receiver
        guard WCSession.isSupported() else { return }
        let s = WCSession.default
        s.delegate = self
        s.activate()
        session = s
    }

    func sendState() {
        guard let session, session.isReachable, let receiver else { return }
        let state: [String: Any] = [
            "isPlaying": receiver.state == .receiving,
            "channel": UserDefaults.standard.string(forKey: "channel") ?? "soluna",
            "volume": Double(receiver.volume),
            "latencyMs": receiver.networkLatencyMs,
            "signalQuality": signalQuality(receiver),
            "nowPlayingTitle": receiver.nowPlayingTitle ?? "",
            "nowPlayingArtist": receiver.nowPlayingArtist ?? "",
            "recentChannels": recentChannels()
        ]
        try? session.updateApplicationContext(state)
    }

    private func signalQuality(_ r: AudioReceiver) -> Int {
        if r.packetLossPercent > 5 || r.networkLatencyMs > 500 { return 1 }
        if r.packetLossPercent > 2 || r.networkLatencyMs > 200 { return 2 }
        if r.packetLossPercent > 0.5 || r.networkLatencyMs > 50 { return 3 }
        return 4
    }

    private func recentChannels() -> [String] {
        UserDefaults.standard.stringArray(forKey: "recentChannels") ?? []
    }
}

extension WatchSessionManager: WCSessionDelegate {
    nonisolated func session(_ session: WCSession, activationDidCompleteWith activationState: WCSessionActivationState, error: Error?) {}
    nonisolated func sessionDidBecomeInactive(_ session: WCSession) {}
    nonisolated func sessionDidDeactivate(_ session: WCSession) {
        session.activate()
    }

    nonisolated func session(_ session: WCSession, didReceiveMessage message: [String: Any]) {
        guard let cmd = message["cmd"] as? String else { return }
        Task { @MainActor in
            guard let receiver = self.receiver else { return }
            switch cmd {
            case "play":
                if receiver.state != .receiving {
                    receiver.start()
                }
            case "stop":
                receiver.stop()
            case "status":
                self.sendState()
            default:
                if cmd.hasPrefix("volume:"), let vol = Float(cmd.dropFirst(7)) {
                    receiver.volume = vol
                } else if cmd.hasPrefix("channel:") {
                    let ch = String(cmd.dropFirst(8))
                    UserDefaults.standard.set(ch, forKey: "channel")
                    // Restart with new channel
                    receiver.stop()
                    receiver.start()
                }
            }
            self.sendState()
        }
    }
}
