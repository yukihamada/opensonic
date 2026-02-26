//
//  SpeakersController.swift
//  SolunaReceiver
//
//  Manages a list of remote speakers (each backed by a DaemonClient WebSocket connection).
//  Auto-discovers Soluna daemons on the local network via Bonjour (_soluna._tcp).
//

import Foundation
import SwiftUI
import Darwin

// MARK: - Bonjour Discovery

private final class BonjourDiscovery: NSObject, NetServiceBrowserDelegate, NetServiceDelegate {
    private let browser = NetServiceBrowser()
    private var resolving: [NetService] = []

    /// Called on the main thread with (displayName, ipAddress).
    var onFound: (String, String) -> Void = { _, _ in }

    func start() {
        browser.delegate = self
        browser.searchForServices(ofType: "_soluna._tcp.", inDomain: "local.")
    }

    func netServiceBrowser(_ browser: NetServiceBrowser, didFind service: NetService, moreComing: Bool) {
        service.delegate = self
        resolving.append(service)
        service.resolve(withTimeout: 5.0)
    }

    func netServiceBrowser(_ browser: NetServiceBrowser, didRemove service: NetService, moreComing: Bool) {
        resolving.removeAll { $0 == service }
    }

    func netServiceDidResolveAddress(_ sender: NetService) {
        let name = sender.name
        // Prefer raw IPv4 address for reliable WS connection; fall back to hostName
        let host = ipv4Address(from: sender) ?? sender.hostName ?? ""
        resolving.removeAll { $0 == sender }
        guard !host.isEmpty else { return }
        DispatchQueue.main.async { self.onFound(name, host) }
    }

    func netService(_ sender: NetService, didNotResolve errorDict: [String: NSNumber]) {
        resolving.removeAll { $0 == sender }
    }

    // Extract the first IPv4 address from a resolved NetService
    private func ipv4Address(from service: NetService) -> String? {
        guard let addresses = service.addresses else { return nil }
        for data in addresses {
            let result = data.withUnsafeBytes { ptr -> String? in
                guard let base = ptr.baseAddress else { return nil }
                let ss = base.assumingMemoryBound(to: sockaddr_storage.self).pointee
                guard ss.ss_family == UInt8(AF_INET) else { return nil }
                var sin = sockaddr_in()
                withUnsafeMutableBytes(of: &sin) { dst in
                    dst.copyMemory(from: UnsafeRawBufferPointer(start: base,
                                                                count: MemoryLayout<sockaddr_in>.size))
                }
                var buf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
                var addr = sin.sin_addr
                inet_ntop(AF_INET, &addr, &buf, socklen_t(INET_ADDRSTRLEN))
                return String(cString: buf)
            }
            if let ip = result { return ip }
        }
        return nil
    }
}

// MARK: - SpeakersController

@MainActor
final class SpeakersController: ObservableObject {

    struct Speaker: Identifiable, Codable {
        var id:   UUID   = UUID()
        var name: String
        var host: String
        var autoDiscovered: Bool = false
    }

    @Published var speakers: [Speaker] = []

    // NOT @Published — each RemoteSpeakerRow observes its daemon directly
    var clients: [UUID: DaemonClient] = [:]

    /// Set by ContentView so auto-sync can update the local jitter buffer.
    weak var audioReceiver: AudioReceiver?

    private static let storageKey = "soluna_speakers_v1"
    private let discovery = BonjourDiscovery()

    init() {
        load()
        discovery.onFound = { [weak self] name, host in
            guard let self else { return }
            // Skip if a speaker with the same host is already known
            guard !self.speakers.contains(where: {
                $0.host.lowercased() == host.lowercased()
            }) else { return }
            self.addDiscovered(name: name, host: host)
        }
        discovery.start()
    }

    // MARK: - CRUD

    func add(name: String, host: String) {
        let s = Speaker(name: name.isEmpty ? host : name, host: host)
        attach(s)
        speakers.append(s)
        persist()
    }

    func remove(_ id: UUID) {
        clients[id]?.disconnect()
        clients.removeValue(forKey: id)
        speakers.removeAll { $0.id == id }
        persist()
    }

    func client(for id: UUID) -> DaemonClient? { clients[id] }

    // MARK: - Private

    private func addDiscovered(name: String, host: String) {
        var s = Speaker(name: name.isEmpty ? host : name, host: host)
        s.autoDiscovered = true
        attach(s)
        speakers.append(s)
        // Don't persist auto-discovered speakers — re-discover on next launch
    }

    private func attach(_ s: Speaker) {
        let d = DaemonClient()
        d.connect(host: s.host)
        clients[s.id] = d
    }

    private func persist() {
        // Only persist manually-added speakers (not auto-discovered)
        let toSave = speakers.filter { !$0.autoDiscovered }
        if let data = try? JSONEncoder().encode(toSave) {
            UserDefaults.standard.set(data, forKey: Self.storageKey)
        }
    }

    private func load() {
        if let data = UserDefaults.standard.data(forKey: Self.storageKey),
           let saved = try? JSONDecoder().decode([Speaker].self, from: data) {
            speakers = saved
            saved.forEach { attach($0) }
            return
        }
        // Migrate from legacy macHost single-entry setting
        let host = UserDefaults.standard.string(forKey: "macHost") ?? ""
        if !host.isEmpty {
            add(name: "Mac", host: host)
        }
    }
}
