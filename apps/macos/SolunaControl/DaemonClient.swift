// DaemonClient.swift — macOS menu bar app
// WebSocket client for solunad (ws://<host>:8400/ws)
// Mirrors the iOS version with Foundation-only dependency.

import Foundation

@MainActor
final class DaemonClient: ObservableObject {

    @Published private(set) var isConnected = false
    @Published              var monitorVolume: Float = 1.0
    @Published              var monitorMuted = false
    @Published private(set) var monitorPackets: UInt64 = 0
    @Published              var monitorDelayMs: Int = 0
    @Published private(set) var measuredLatencyMs: Int = 0

    private var task:        URLSessionWebSocketTask?
    private var msgId    =   0
    private var timer:       Timer?
    private var syncTimer:   Timer?
    private var retryTimer:  Timer?
    private var lastHost =   ""
    private var pingStart:   Date?

    // MARK: - Public API

    func connect(host: String) {
        guard !host.isEmpty else { return }
        disconnect()
        lastHost = host
        _connect(host: host)
    }

    func disconnect() {
        retryTimer?.invalidate(); retryTimer = nil
        syncTimer?.invalidate();  syncTimer  = nil
        timer?.invalidate();      timer      = nil
        task?.cancel(with: .goingAway, reason: nil); task = nil
        isConnected = false
        measuredLatencyMs = 0
    }

    func setMonitorVolume(_ v: Float) {
        monitorVolume = max(0, min(1, v))
        send(#"{"id":\#(nextId()),"command":"monitor.set_volume","volume":\#(String(format:"%.3f",monitorVolume))}"#)
    }

    func setMonitorMute(_ m: Bool) {
        monitorMuted = m
        send(#"{"id":\#(nextId()),"command":"monitor.set_mute","muted":\#(m ? "true" : "false")}"#)
    }

    func setMonitorDelay(_ ms: Int) {
        monitorDelayMs = ms
        send(#"{"id":\#(nextId()),"command":"monitor.set_delay","ms":\#(ms)}"#)
    }

    func performSync() {
        guard isConnected else { return }
        pingStart = Date()
        send(#"{"id":\#(nextId()),"command":"time.ping"}"#)
    }

    // MARK: - Private

    private func _connect(host: String) {
        guard let url = makeWSURL(host: host) else { return }
        let cfg = URLSessionConfiguration.default
        cfg.timeoutIntervalForRequest = 10
        let s = URLSession(configuration: cfg)
        task = s.webSocketTask(with: url)
        task?.resume()
        receiveLoop()
        fetchStats()
        timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in self?.fetchStats() }
        }
        Timer.scheduledTimer(withTimeInterval: 1, repeats: false) { [weak self] _ in
            Task { @MainActor [weak self] in self?.performSync() }
        }
        syncTimer = Timer.scheduledTimer(withTimeInterval: 30, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in self?.performSync() }
        }
    }

    private func makeWSURL(host: String) -> URL? {
        let h = host.trimmingCharacters(in: .whitespacesAndNewlines)
        if h.hasPrefix("ws://") || h.hasPrefix("wss://") {
            return URL(string: h.hasSuffix("/ws") ? h : h + "/ws")
        }
        let bare = h.components(separatedBy: "/").first ?? h
        let isLocal = bare.allSatisfy({ $0.isNumber || $0 == "." || $0 == ":" })
            || bare.lowercased().hasSuffix(".local")
            || bare.lowercased() == "localhost"
        let scheme = isLocal ? "ws" : "wss"
        let portStr = isLocal && !bare.contains(":") ? ":8400" : ""
        return URL(string: "\(scheme)://\(bare)\(portStr)/ws")
    }

    private func fetchStats() {
        send(#"{"id":\#(nextId()),"command":"monitor.stats"}"#)
    }

    private func nextId() -> Int { msgId += 1; return msgId }

    private func send(_ msg: String) {
        task?.send(.string(msg)) { _ in }
    }

    private func receiveLoop() {
        task?.receive { [weak self] result in
            guard let self else { return }
            switch result {
            case .success(let message):
                if case .string(let text) = message {
                    Task { @MainActor [weak self] in
                        guard let self else { return }
                        self.isConnected = true
                        self.parse(text)
                    }
                }
                self.receiveLoop()
            case .failure:
                Task { @MainActor [weak self] in
                    guard let self else { return }
                    self.isConnected = false
                    self.timer?.invalidate()
                    self.syncTimer?.invalidate()
                    self.retryTimer = Timer.scheduledTimer(withTimeInterval: 5, repeats: false) { [weak self] _ in
                        Task { @MainActor [weak self] in
                            guard let self, !self.lastHost.isEmpty else { return }
                            self._connect(host: self.lastHost)
                        }
                    }
                }
            }
        }
    }

    private func parse(_ text: String) {
        guard
            let data    = text.data(using: .utf8),
            let json    = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
            let success = json["success"] as? Bool, success,
            let raw     = json["data"] as? String, !raw.isEmpty,
            let inner   = raw.data(using: .utf8),
            let d       = try? JSONSerialization.jsonObject(with: inner) as? [String: Any]
        else { return }

        if d["pong"] != nil {
            guard let t1 = pingStart else { return }
            pingStart = nil
            let rttMs = (Date().timeIntervalSince1970 - t1.timeIntervalSince1970) * 1000
            let latency = max(20, Int(rttMs / 2) + 15)
            measuredLatencyMs = latency
            setMonitorDelay(latency)
        } else if d["supported"] != nil {
            if let v = d["volume"] as? Double { monitorVolume = Float(v) }
            monitorMuted    = d["muted"]   as? Bool ?? false
            monitorPackets  = UInt64(d["packets"] as? Int ?? 0)
            if let delay = d["delay_ms"] as? Int { monitorDelayMs = delay }
        }
    }
}
