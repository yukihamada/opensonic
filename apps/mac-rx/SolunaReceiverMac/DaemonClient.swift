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

    // stream mode: "sync" (multi-room PTP aligned) or "jam" (low-latency ~20ms)
    @Published var streamMode: String = "sync"

    // available output devices
    @Published private(set) var devices: [String] = []
    @Published              var selectedDevice = ""

    // ── Player ──────────────────────────────────────────────────────────────
    @Published private(set) var playerActive:     Bool   = false
    @Published private(set) var playerPaused:     Bool   = false
    @Published private(set) var playerName:       String = ""
    @Published private(set) var playerFmt:        String = ""
    @Published private(set) var playerDurMs:      UInt64 = 0
    @Published private(set) var playerPosMs:      UInt64 = 0
    @Published private(set) var fileXferProgress: Double = 0

    /// Called with (fileData, fileName) when the full file has been received.
    var onFileReceived: ((Data, String) -> Void)?
    /// Called with (delayMs, posMs) on player.switch event.
    var onPlayerSwitch: ((UInt32, UInt64) -> Void)?

    /// Called when auto-sync completes with the measured latency (ms).
    /// Use this to update the local AudioReceiver's jitter buffer.
    var onSyncLatency: ((Int) -> Void)?

    // MARK: - Private

    private var task:          URLSessionWebSocketTask?
    private var msgId    =     0
    private var timer:         Timer?
    private var syncTimer:     Timer?
    private var retryTimer:    Timer?
    private(set) var lastHost  = ""
    private var pingStartTime: Date?

    // file accumulation for player
    private var _fileBuf      = Data()
    private var _fileExpected = 0
    private var _fileName     = ""

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

    func setGlobalRxDelay(_ ms: Int) {
        send(#"{"id":\#(nextId()),"command":"rx.set_global_delay","ms":\#(ms)}"#)
    }

    // MARK: - Player Commands

    func playerPlay()      { send(#"{"id":\#(nextId()),"command":"player.play"}"#) }
    func playerPause()     { send(#"{"id":\#(nextId()),"command":"player.pause"}"#) }
    func playerStop()      { send(#"{"id":\#(nextId()),"command":"player.stop"}"#); playerActive = false }
    func playerFileReady() { send(#"{"id":\#(nextId()),"command":"player.file_ready"}"#) }
    func playerStatus()    { send(#"{"id":\#(nextId()),"command":"player.status"}"#) }
    func playerSeek(ms: UInt64) {
        send(#"{"id":\#(nextId()),"command":"player.seek","pos_ms":\#(ms)}"#)
    }

    func uploadFile(_ data: Data, name: String, onProgress: ((Double) -> Void)? = nil) async {
        guard let url = httpUploadURL(name: name) else { return }
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.setValue("application/octet-stream", forHTTPHeaderField: "Content-Type")
        req.timeoutInterval = 120
        let delegate = UploadDelegate()
        delegate.onProgress = onProgress
        let session = URLSession(configuration: .default, delegate: delegate, delegateQueue: nil)
        _ = try? await session.upload(for: req, from: data)
    }

    private func httpUploadURL(name: String) -> URL? {
        let h = lastHost.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !h.isEmpty else { return nil }
        let stripped = h.hasPrefix("ws://")  ? String(h.dropFirst(5)) :
                       h.hasPrefix("wss://") ? String(h.dropFirst(6)) : h
        let bare    = stripped.components(separatedBy: "/").first ?? stripped
        let isIP    = bare.allSatisfy { $0.isNumber || $0 == "." || $0 == ":" }
        let isLocal = isIP || bare.lowercased().hasSuffix(".local")
        let scheme  = isLocal ? "http" : "https"
        let port    = isLocal ? ":8400" : ""
        let enc     = name.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed) ?? name
        return URL(string: "\(scheme)://\(bare)\(port)/api/player/upload?name=\(enc)")
    }

    // MARK: - Channel

    func setChannel(_ ch: String) {
        send(#"{"id":\#(nextId()),"command":"channel.set","channel":"\#(ch)"}"#)
    }

    // MARK: - Stream mode

    func setMode(_ mode: String) {
        streamMode = mode
        send(#"{"id":\#(nextId()),"command":"mode.set","mode":"\#(mode)"}"#)
    }

    func getMode() {
        send(#"{"id":\#(nextId()),"command":"mode.get"}"#)
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
        getMode()
        playerStatus()
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
                switch message {
                case .string(let text):
                    Task { @MainActor [weak self] in
                        guard let self else { return }
                        self.isConnected = true
                        self.parse(text)
                    }
                case .data(let data):
                    Task { @MainActor [weak self] in self?.handleBinary(data) }
                @unknown default: break
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

    @MainActor private func handleBinary(_ data: Data) {
        guard data.count >= 2 else { return }
        if data[0] == 0xFA && data[1] == 0xFB {
            // File chunk: [0xFA, 0xFB, size_hi, size_lo, payload…]
            let payload = data.count > 4 ? data[4...] : Data()
            _fileBuf.append(payload)
            if _fileExpected > 0 {
                fileXferProgress = min(1.0, Double(_fileBuf.count) / Double(_fileExpected))
            }
        }
        // Other binary frames (PCM audio) handled by AudioReceiver via RTP
    }

    private func handleEvent(_ event: String, json: [String: Any]) {
        switch event {
        case "player.stream_start":
            playerName   = json["name"]  as? String ?? ""
            playerFmt    = json["fmt"]   as? String ?? ""
            playerDurMs  = UInt64((json["dur_ms"] as? Int) ?? 0)
            playerActive = true
            playerPaused = false
            _fileBuf.removeAll()
            _fileExpected = 0
            fileXferProgress = 0

        case "player.file_start":
            _fileName    = json["name"] as? String ?? playerName
            _fileExpected = (json["size"] as? Int) ?? 0
            _fileBuf.removeAll()
            fileXferProgress = 0

        case "player.file_done":
            fileXferProgress = 1.0
            onFileReceived?(_fileBuf, _fileName)

        case "player.switch":
            let delay = UInt32((json["switch_delay_ms"] as? Int) ?? 2000)
            let pos   = UInt64((json["file_pos_ms"]     as? Int) ?? 0)
            onPlayerSwitch?(delay, pos)

        case "player.done", "player.stopped":
            playerActive = false

        default: break
        }
    }

    private func parse(_ text: String) {
        guard
            let data = text.data(using: .utf8),
            let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return }

        // Broadcast events (player.stream_start, etc.)
        if let event = json["event"] as? String {
            handleEvent(event, json: json)
            return
        }

        guard
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
            guard let t1 = pingStartTime else { return }
            pingStartTime = nil
            let rttMs = (Date().timeIntervalSince1970 - t1.timeIntervalSince1970) * 1000
            let latency = max(20, Int(rttMs / 2) + 15)
            measuredLatencyMs = latency
            setMonitorDelay(latency)
            onSyncLatency?(latency)

        } else if d["supported"] != nil {
            monitorSupported = d["supported"] as? Bool ?? false
            monitorRunning   = d["running"]   as? Bool ?? false
            if let v = d["volume"] as? Double { monitorVolume = Float(v) }
            monitorMuted   = d["muted"]   as? Bool ?? false
            monitorPackets = UInt64(d["packets"] as? Int ?? 0)
            if let delay = d["delay_ms"] as? Int { monitorDelayMs = delay }
            if let rxd = d["rx_delay_ms"] as? Int { rxDelayMs = rxd }

        } else if d["tunnel_url"] != nil {
            tunnelURL = d["tunnel_url"] as? String ?? ""

        } else if let mode = d["mode"] as? String {
            streamMode = mode

        } else if let active = d["active"] as? Bool, d["pos_ms"] != nil {
            // player.status response
            playerActive = active
            playerPaused = d["paused"] as? Bool ?? false
            playerPosMs  = UInt64((d["pos_ms"] as? Int) ?? 0)
            playerDurMs  = UInt64((d["dur_ms"] as? Int) ?? 0)
            playerName   = d["file"] as? String ?? ""
        }
    }
}

// MARK: - Upload progress delegate

private final class UploadDelegate: NSObject, URLSessionTaskDelegate {
    var onProgress: ((Double) -> Void)?

    func urlSession(_ session: URLSession, task: URLSessionTask,
                    didSendBodyData bytesSent: Int64,
                    totalBytesSent: Int64, totalBytesExpectedToSend: Int64) {
        guard totalBytesExpectedToSend > 0 else { return }
        let pct = Double(totalBytesSent) / Double(totalBytesExpectedToSend)
        DispatchQueue.main.async { self.onProgress?(pct) }
    }
}
