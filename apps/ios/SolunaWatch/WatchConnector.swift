import Foundation
import WatchConnectivity

class WatchConnector: NSObject, ObservableObject, WCSessionDelegate {
    @Published var isPlaying: Bool = false
    @Published var channel: String = ""
    @Published var volume: Double = 0.8
    @Published var latencyMs: Float = 0
    @Published var signalQuality: Int = 4
    @Published var nowPlayingTitle: String?
    @Published var nowPlayingArtist: String?
    @Published var recentChannels: [String] = []

    override init() {
        super.init()
        if WCSession.isSupported() {
            let session = WCSession.default
            session.delegate = self
            session.activate()
        }
    }

    func sendCommand(_ command: String) {
        guard WCSession.default.isReachable else { return }
        WCSession.default.sendMessage(["cmd": command], replyHandler: nil)
    }

    // MARK: - WCSessionDelegate

    func session(_ session: WCSession, activationDidCompleteWith activationState: WCSessionActivationState, error: Error?) {
        // Request initial state
        if activationState == .activated {
            sendCommand("status")
        }
    }

    func session(_ session: WCSession, didReceiveApplicationContext applicationContext: [String: Any]) {
        DispatchQueue.main.async {
            self.updateState(applicationContext)
        }
    }

    func session(_ session: WCSession, didReceiveMessage message: [String: Any]) {
        DispatchQueue.main.async {
            self.updateState(message)
        }
    }

    private func updateState(_ dict: [String: Any]) {
        if let playing = dict["isPlaying"] as? Bool { isPlaying = playing }
        if let ch = dict["channel"] as? String { channel = ch }
        if let vol = dict["volume"] as? Double { volume = vol }
        if let lat = dict["latencyMs"] as? Float { latencyMs = lat }
        if let sig = dict["signalQuality"] as? Int { signalQuality = sig }
        if let title = dict["nowPlayingTitle"] as? String { nowPlayingTitle = title }
        if let artist = dict["nowPlayingArtist"] as? String { nowPlayingArtist = artist }
        if let channels = dict["recentChannels"] as? [String] { recentChannels = channels }
    }
}
