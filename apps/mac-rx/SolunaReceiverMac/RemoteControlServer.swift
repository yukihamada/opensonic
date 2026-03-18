//
//  RemoteControlServer.swift
//  SolunaReceiverMac
//
//  Lightweight HTTP API for external control (Streamdeck, Home Assistant, etc.)
//  Listens on port 9400. All endpoints return JSON.
//

import Foundation
import Network

@MainActor
final class RemoteControlServer: ObservableObject {
    @Published var isRunning = false
    private var listener: NWListener?
    private weak var receiver: AudioReceiver?
    private var bonjourService: NetService?

    static let port: UInt16 = 9400

    func start(receiver: AudioReceiver) {
        guard !isRunning else { return }
        self.receiver = receiver

        do {
            let params = NWParameters.tcp
            listener = try NWListener(using: params, on: NWEndpoint.Port(rawValue: Self.port)!)
        } catch {
            print("[RemoteControl] Failed to create listener: \(error)")
            return
        }

        listener?.newConnectionHandler = { [weak self] conn in
            conn.start(queue: .main)
            Task { @MainActor in
                self?.handleConnection(conn)
            }
        }
        listener?.stateUpdateHandler = { [weak self] state in
            Task { @MainActor in
                self?.isRunning = (state == .ready)
                if state == .ready {
                    self?.startBonjourAdvertising()
                }
            }
        }
        listener?.start(queue: .main)
    }

    private func startBonjourAdvertising() {
        let channel = UserDefaults.standard.string(forKey: "channel") ?? "soluna"
        let hostname = Host.current().localizedName ?? "Mac"
        bonjourService = NetService(domain: "local.", type: "_soluna._tcp.", name: hostname, port: Int32(Self.port))
        bonjourService?.setTXTRecord(NetService.data(fromTXTRecord: [
            "platform": "mac".data(using: .utf8)!,
            "mode": "rx".data(using: .utf8)!,
            "channel": channel.data(using: .utf8)!,
            "version": "0.4.5".data(using: .utf8)!
        ]))
        bonjourService?.publish()
        print("[Bonjour] Advertising _soluna._tcp on port \(Self.port): \(hostname)")
    }

    func stop() {
        bonjourService?.stop()
        bonjourService = nil
        listener?.cancel()
        listener = nil
        isRunning = false
    }

    private func handleConnection(_ conn: NWConnection) {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 4096) { [weak self] data, _, _, error in
            guard let self, let data, error == nil else {
                conn.cancel()
                return
            }
            guard let request = String(data: data, encoding: .utf8) else {
                conn.cancel()
                return
            }

            // Parse HTTP request line
            let lines = request.components(separatedBy: "\r\n")
            guard let firstLine = lines.first else { conn.cancel(); return }
            let parts = firstLine.components(separatedBy: " ")
            guard parts.count >= 2 else { conn.cancel(); return }
            let method = parts[0]
            let path = parts[1]

            Task { @MainActor in
                let response = self.routeRequest(method: method, path: path, body: request)
                let httpResponse = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n\(response)"
                conn.send(content: httpResponse.data(using: .utf8), completion: .contentProcessed { _ in
                    conn.cancel()
                })
            }
        }
    }

    private func routeRequest(method: String, path: String, body: String) -> String {
        guard let rx = receiver else { return "{\"error\":\"not ready\"}" }

        switch (method, path) {
        case ("GET", "/api/status"):
            return statusJSON(rx)

        case ("POST", "/api/play"):
            if !rx.isPlaying { rx.start() }
            return "{\"ok\":true,\"state\":\"playing\"}"

        case ("POST", "/api/stop"):
            if rx.isPlaying { rx.toggle() }
            return "{\"ok\":true,\"state\":\"stopped\"}"

        case ("POST", "/api/toggle"):
            rx.toggle()
            return "{\"ok\":true,\"state\":\"\(rx.isPlaying ? "playing" : "stopped")\"}"

        case ("POST", _ ) where path.hasPrefix("/api/volume/"):
            let valStr = path.replacingOccurrences(of: "/api/volume/", with: "")
            if let pct = Int(valStr) {
                rx.volume = Float(max(0, min(100, pct))) / 100.0
                return "{\"ok\":true,\"volume\":\(pct)}"
            }
            return "{\"error\":\"invalid volume\"}"

        case ("POST", "/api/mute"):
            rx.isMuted = true
            return "{\"ok\":true,\"muted\":true}"

        case ("POST", "/api/unmute"):
            rx.isMuted = false
            return "{\"ok\":true,\"muted\":false}"

        case ("GET", "/api/devices"):
            return devicesJSON(rx)

        case ("GET", "/api/presets"):
            return presetsJSON(rx)

        case ("POST", _ ) where path.hasPrefix("/api/preset/"):
            let name = path.replacingOccurrences(of: "/api/preset/", with: "")
                .removingPercentEncoding ?? ""
            if let preset = rx.presets.first(where: { $0.name == name }) {
                rx.applyPreset(preset)
                return "{\"ok\":true,\"preset\":\"\(name)\"}"
            }
            return "{\"error\":\"preset not found\"}"

        case ("OPTIONS", _):
            return "{\"ok\":true}"

        default:
            return "{\"error\":\"not found\",\"endpoints\":[\"/api/status\",\"/api/play\",\"/api/stop\",\"/api/toggle\",\"/api/volume/{0-100}\",\"/api/mute\",\"/api/unmute\",\"/api/devices\",\"/api/presets\",\"/api/preset/{name}\"]}"
        }
    }

    private func statusJSON(_ rx: AudioReceiver) -> String {
        let state = rx.state.rawValue
        let vol = Int(rx.volume * 100)
        let muted = rx.isMuted
        let pkts = rx.packetsReceived
        let outputs = rx.activeOutputs.count
        return "{\"state\":\"\(state)\",\"volume\":\(vol),\"muted\":\(muted),\"packetsReceived\":\(pkts),\"activeOutputs\":\(outputs)}"
    }

    private func devicesJSON(_ rx: AudioReceiver) -> String {
        let items = rx.availableDevices.map { d in
            "{\"id\":\(d.id),\"name\":\"\(d.name)\",\"type\":\"\(d.transportType.rawValue)\",\"active\":\(d.isActive)}"
        }
        return "[\(items.joined(separator: ","))]"
    }

    private func presetsJSON(_ rx: AudioReceiver) -> String {
        let items = rx.presets.map { p in
            "{\"name\":\"\(p.name)\",\"devices\":\(p.deviceConfigs.count)}"
        }
        return "[\(items.joined(separator: ","))]"
    }
}
