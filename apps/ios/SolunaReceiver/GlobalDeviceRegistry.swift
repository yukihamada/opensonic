//
//  GlobalDeviceRegistry.swift
//  Soluna
//
//  Query relay for all active TX devices (DJ/Owner) across all groups.
//

import Foundation
import Network

struct GlobalDevice: Identifiable, Equatable {
    let id: String          // device_id (UUID)
    let name: String        // device_name
    let group: String       // relay group/channel
    let role: String        // "owner" or "dj"
    let addr: String        // ip:port of the device
    var relayHost: String   // relay server queried
    var relayPort: UInt16
}

@MainActor
class GlobalDeviceRegistry: ObservableObject {
    @Published var devices: [GlobalDevice] = []
    @Published var isLoading = false
    @Published var error: String? = nil

    // Known relay servers to query (can be extended by user)
    static let defaultRelays: [(host: String, port: UInt16)] = [
        ("46.225.77.119", 5100),
        ("relay.solun.art", 5100),
    ]

    func refresh(relays: [(host: String, port: UInt16)] = defaultRelays) {
        guard !isLoading else { return }
        isLoading = true
        error = nil
        devices = []

        Task {
            var found: [GlobalDevice] = []
            await withTaskGroup(of: [GlobalDevice].self) { group in
                for relay in relays {
                    group.addTask {
                        await Self.queryRelay(host: relay.host, port: relay.port)
                    }
                }
                for await result in group {
                    found.append(contentsOf: result)
                }
            }
            // Deduplicate by device_id
            var seen = Set<String>()
            self.devices = found.filter { seen.insert($0.id).inserted }
            self.isLoading = false
            if self.devices.isEmpty {
                self.error = "No active devices found"
            }
        }
    }

    private static func queryRelay(host: String, port: UInt16) async -> [GlobalDevice] {
        await withCheckedContinuation { continuation in
            let queue = DispatchQueue(label: "global-device-query")
            let connection = NWConnection(
                host: NWEndpoint.Host(host),
                port: NWEndpoint.Port(rawValue: port)!,
                using: .udp
            )
            var replied = false
            let timer = DispatchWorkItem {
                if !replied { continuation.resume(returning: []) }
            }
            connection.stateUpdateHandler = { state in
                switch state {
                case .ready:
                    let msg = "GLOBAL_DEVICES\n".data(using: .utf8)!
                    connection.send(content: msg, completion: .contentProcessed { _ in })
                    connection.receive(minimumIncompleteLength: 1, maximumLength: 65536) { data, _, _, _ in
                        replied = true
                        timer.cancel()
                        connection.cancel()
                        guard let data, let text = String(data: data, encoding: .utf8) else {
                            continuation.resume(returning: [])
                            return
                        }
                        continuation.resume(returning: Self.parse(text, relayHost: host, relayPort: port))
                    }
                case .failed, .cancelled:
                    if !replied {
                        replied = true
                        timer.cancel()
                        continuation.resume(returning: [])
                    }
                default: break
                }
            }
            connection.start(queue: queue)
            queue.asyncAfter(deadline: .now() + 3, execute: timer)
        }
    }

    private static func parse(_ text: String, relayHost: String, relayPort: UInt16) -> [GlobalDevice] {
        // Response: DEVICES:[{"name":"...","id":"...","group":"...","role":"...","addr":"..."},...]\n
        guard let range = text.range(of: "DEVICES:[") else { return [] }
        let json = "[" + text[range.upperBound...]
            .replacingOccurrences(of: "\n", with: "")
        guard let data = json.data(using: .utf8),
              let array = try? JSONSerialization.jsonObject(with: data) as? [[String: String]]
        else { return [] }

        return array.compactMap { dict in
            guard let id = dict["id"], let name = dict["name"],
                  let group = dict["group"], let role = dict["role"],
                  let addr = dict["addr"] else { return nil }
            return GlobalDevice(id: id, name: name, group: group, role: role,
                                addr: addr, relayHost: relayHost, relayPort: relayPort)
        }
    }
}
