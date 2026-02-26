//
//  DaemonClient.swift
//  SolunaReceiver / SolunaControl
//
//  WebSocket client for solunad (ws://<host>:8400/ws or wss://<tunnel-url>/ws)
//  Supports local IP and internet tunnel (cloudflared / ngrok).
//

import Foundation

@MainActor
final class DaemonClient: ObservableObject {

    // MARK: - Published State

    @Published private(set) var isConnected = false
    @Published private(set) var tunnelURL   = ""   // from system.info

    // monitor speaker (TX-mode)
    @Published private(set) var monitorSupported = false
    @Published private(set) var monitorRunning   = false
    @Published              var monitorVolume: Float = 1.0
    @Published              var monitorMuted      = false
    @Published private(set) var monitorPackets: UInt64 = 0
    @Published              var monitorBufferMs: Int = 20

    // available output devices
    @Published private(set) var devices: [String] = []
    @Published              var selectedDevice = ""

    // MARK: - Private

    private var task:         URLSessionWebSocketTask?
    private let session =     URLSession(configuration: .default)
    private var msgId   =     0
    private var timer:        Timer?
    private var retryTimer:   Timer?
    private var lastHost =    ""

    // MARK: - Connection

    /// Connect to a solunad daemon.
    /// - host: IP address, hostname, or tunnel domain (e.g. abc.trycloudflare.com)
    func connect(host: String) {
        guard !host.isEmpty else { return }
        disconnect()
        lastHost = host
        _connect(host: host)
    }

    func disconnect() {
        retryTimer?.invalidate(); retryTimer = nil
        timer?.invalidate(); timer = nil
        task?.cancel(with: .goingAway, reason: nil); task = nil
        isConnected = false
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

    // MARK: - Private helpers

    private func _connect(host: String) {
        guard let url = makeWSURL(host: host) else { return }
        let cfg = URLSessionConfiguration.default
        cfg.timeoutIntervalForRequest = 10
        let s = URLSession(configuration: cfg)
        task = s.webSocketTask(with: url)
        task?.resume()
        isConnected = true
        receiveLoop()
        fetchAll()
        timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in self?.fetchAll() }
        }
    }

    /// Build WebSocket URL from user input:
    ///   - Raw IP / hostname (e.g. "192.168.1.10") → ws://host:8400/ws
    ///   - Domain with dots not containing port (e.g. "abc.trycloudflare.com") → wss://host/ws
    ///   - Already a ws:// or wss:// URL → use as-is
    private func makeWSURL(host: String) -> URL? {
        let h = host.trimmingCharacters(in: .whitespacesAndNewlines)
        if h.hasPrefix("ws://") || h.hasPrefix("wss://") {
            return URL(string: h.hasSuffix("/ws") ? h : h + "/ws")
        }
        // Strip trailing slash / path
        let bare = h.components(separatedBy: "/").first ?? h
        // If it looks like a public domain (contains a dot and isn't a raw IP with port)
        let isIP = bare.allSatisfy({ $0.isNumber || $0 == "." || $0 == ":" })
        let scheme = isIP ? "ws" : "wss"
        let portStr = isIP ? ":8400" : ""
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
                    Task { @MainActor [weak self] in self?.parse(text) }
                }
                self.receiveLoop()
            case .failure:
                Task { @MainActor [weak self] in
                    guard let self else { return }
                    self.isConnected = false
                    self.timer?.invalidate()
                    // Auto-reconnect after 5s
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

        // stats / info → JSON object
        guard let d = try? JSONSerialization.jsonObject(with: inner) as? [String: Any] else { return }

        if d["supported"] != nil {
            // monitor.stats
            monitorSupported = d["supported"] as? Bool ?? false
            monitorRunning   = d["running"]   as? Bool ?? false
            if let v = d["volume"] as? Double { monitorVolume = Float(v) }
            monitorMuted     = d["muted"]     as? Bool ?? false
            monitorPackets   = UInt64(d["packets"] as? Int ?? 0)
        } else if d["tunnel_url"] != nil {
            // system.info
            tunnelURL = d["tunnel_url"] as? String ?? ""
        }
    }
}
