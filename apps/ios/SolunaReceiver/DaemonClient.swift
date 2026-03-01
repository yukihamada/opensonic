//
//  DaemonClient.swift
//  SolunaReceiver / SolunaControl
//
//  WebSocket client for solunad (ws://<host>:8400/ws or wss://<tunnel-url>/ws)
//  Supports local IP and internet tunnel (cloudflared / ngrok).
//  Auto-sync: measures WS RTT to estimate one-way audio latency, then sets
//  Mac speaker delay and iPhone jitter buffer to match.
//

import Foundation

@MainActor
final class DaemonClient: ObservableObject {

    // MARK: - Published State

    @Published private(set) var isConnected = false
    @Published private(set) var tunnelURL   = ""

    // monitor speaker (TX-mode)
    @Published private(set) var monitorSupported = false
    @Published private(set) var monitorRunning   = false
    @Published              var monitorVolume: Float = 1.0
    @Published              var monitorMuted      = false
    @Published private(set) var monitorPackets: UInt64 = 0
    @Published              var monitorBufferMs: Int = 20
    @Published              var monitorDelayMs:  Int = 0

    /// Global RX delay (ms) pushed by solunad to all iOS receivers. 0 = use local setting.
    @Published private(set) var rxDelayMs: Int = 0

    /// Measured one-way latency (ms). Drives auto-sync. 0 = not yet measured.
    @Published private(set) var measuredLatencyMs: Int = 0

    // available output devices
    @Published private(set) var devices: [String] = []
    @Published              var selectedDevice = ""

    /// Called when auto-sync completes with the measured latency (ms).
    /// Use this to update the local AudioReceiver's jitter buffer.
    var onSyncLatency: ((Int) -> Void)?

    // MARK: - Private

    private var task:          URLSessionWebSocketTask?
    private var msgId    =     0
    private var timer:         Timer?
    private var syncTimer:     Timer?
    private var retryTimer:    Timer?
    private var lastHost =     ""
    private var pingStartTime: Date?

    // MARK: - Connection

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

    // MARK: - Monitor commands

    func startMonitor(device: String) {
        guard !device.isEmpty else { return }
        send(#"{"id":\#(nextId()),"command":"monitor.start","device":"\#(device)"}"#)
        monitorRunning = true
    }

    func stopMonitor() {
        send(#"{"id":\#(nextId()),"command":"monitor.stop"}"#)
        monitorRunning = false
    }

    func setMonitorVolume(_ v: Float) {
        monitorVolume = v
        send(#"{"id":\#(nextId()),"command":"monitor.set_volume","volume":\#(String(format:"%.3f",v))}"#)
    }

    func setMonitorMute(_ m: Bool) {
        monitorMuted = m
        send(#"{"id":\#(nextId()),"command":"monitor.set_mute","muted":\#(m ? "true" : "false")}"#)
    }

    func setMonitorBuffer(_ ms: Int) {
        monitorBufferMs = ms
        send(#"{"id":\#(nextId()),"command":"monitor.set_buffer","ms":\#(ms)}"#)
    }

    func setMonitorDelay(_ ms: Int) {
        monitorDelayMs = ms
        send(#"{"id":\#(nextId()),"command":"monitor.set_delay","ms":\#(ms)}"#)
    }

    // MARK: - Auto-sync

    /// Send a WS ping to measure round-trip time.
    /// On reply: one_way = RTT/2 + safety margin.
    /// - Sets Mac speaker delay via monitor.set_delay
    /// - Calls onSyncLatency to update iPhone jitter buffer
    func performSync() {
        guard isConnected else { return }
        pingStartTime = Date()
        send(#"{"id":\#(nextId()),"command":"time.ping"}"#)
    }

    // MARK: - Private helpers

    private func _connect(host: String) {
        guard let url = makeWSURL(host: host) else { return }
        let cfg = URLSessionConfiguration.default
        cfg.timeoutIntervalForRequest = 10
        let s = URLSession(configuration: cfg)
        task = s.webSocketTask(with: url)
        task?.resume()
        receiveLoop()
        fetchAll()
        // Poll stats every 3s
        timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in self?.fetchAll() }
        }
        // Auto-sync: first ping after 1s, then every 30s
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
        let isIP   = bare.allSatisfy({ $0.isNumber || $0 == "." || $0 == ":" })
        let isLocal = isIP || bare.lowercased().hasSuffix(".local")
        let scheme  = isLocal ? "ws"  : "wss"
        let portStr = isLocal ? ":8400" : ""
        return URL(string: "\(scheme)://\(bare)\(portStr)/ws")
    }

    private func fetchAll() {
        send(#"{"id":\#(nextId()),"command":"monitor.stats"}"#)
        send(#"{"id":\#(nextId()),"command":"monitor.list_devices"}"#)
        send(#"{"id":\#(nextId()),"command":"system.info"}"#)
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
            let inner   = raw.data(using: .utf8)
        else { return }

        // list_devices → JSON array
        if let arr = try? JSONSerialization.jsonObject(with: inner) as? [String] {
            devices = arr
            if selectedDevice.isEmpty, let first = arr.first { selectedDevice = first }
            return
        }

        guard let d = try? JSONSerialization.jsonObject(with: inner) as? [String: Any] else { return }

        if d["pong"] != nil {
            // time.ping response — compute RTT and apply auto-sync
            guard let t1 = pingStartTime else { return }
            pingStartTime = nil
            let rttMs = (Date().timeIntervalSince1970 - t1.timeIntervalSince1970) * 1000
            // one-way delay = RTT/2; add 15ms safety margin; minimum 20ms
            let latency = max(20, Int(rttMs / 2) + 15)
            measuredLatencyMs = latency
            // Apply to Mac speaker delay (this DaemonClient is connected to solunad)
            setMonitorDelay(latency)
            // Apply to iPhone jitter buffer via callback
            onSyncLatency?(latency)

        } else if d["supported"] != nil {
            // monitor.stats
            monitorSupported = d["supported"] as? Bool ?? false
            monitorRunning   = d["running"]   as? Bool ?? false
            if let v = d["volume"] as? Double { monitorVolume = Float(v) }
            monitorMuted   = d["muted"]   as? Bool ?? false
            monitorPackets = UInt64(d["packets"] as? Int ?? 0)
            if let delay = d["delay_ms"] as? Int { monitorDelayMs = delay }

        } else if d["tunnel_url"] != nil {
            // system.info
            tunnelURL = d["tunnel_url"] as? String ?? ""
        }
    }
}
