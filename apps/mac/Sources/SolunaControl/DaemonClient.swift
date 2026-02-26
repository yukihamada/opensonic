//
//  DaemonClient.swift
//  SolunaControl (macOS)
//
//  WebSocket client for solunad — connects to localhost:8400/ws
//

import Foundation

@MainActor
final class DaemonClient: ObservableObject {

    // MARK: - Published State

    @Published private(set) var isConnected = false

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

    private var task:    URLSessionWebSocketTask?
    private let session = URLSession(configuration: .default)
    private var msgId   = 0
    private var timer:   Timer?

    // MARK: - Connection

    func connect(host: String = "localhost") {
        disconnect()
        guard let url = URL(string: "ws://\(host):8400/ws") else { return }
        task = session.webSocketTask(with: url)
        task?.resume()
        isConnected = true
        receiveLoop()
        fetchAll()
        timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in self?.fetchAll() }
        }
    }

    func disconnect() {
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

    private func fetchAll() {
        send(#"{"id":\#(nextId()),"command":"monitor.stats"}"#)
        send(#"{"id":\#(nextId()),"command":"monitor.list_devices"}"#)
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
                    self?.isConnected = false
                    self?.timer?.invalidate()
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

        if let arr = try? JSONSerialization.jsonObject(with: inner) as? [String] {
            devices = arr
            if selectedDevice.isEmpty, let first = arr.first { selectedDevice = first }
            return
        }

        guard let d = try? JSONSerialization.jsonObject(with: inner) as? [String: Any] else { return }

        if d["supported"] != nil {
            monitorSupported = d["supported"] as? Bool ?? false
            monitorRunning   = d["running"]   as? Bool ?? false
            if let v = d["volume"] as? Double { monitorVolume = Float(v) }
            monitorMuted     = d["muted"]     as? Bool ?? false
            monitorPackets   = UInt64(d["packets"] as? Int ?? 0)
        }
    }
}
