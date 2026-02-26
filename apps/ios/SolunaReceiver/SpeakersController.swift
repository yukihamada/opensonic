//
//  SpeakersController.swift
//  SolunaReceiver
//
//  Manages a list of remote speakers (each backed by a DaemonClient WebSocket connection).
//  Auto-discovers Soluna daemons on the local network via Bonjour (_soluna._tcp).
//

import Foundation
import SwiftUI

// MARK: - Bonjour Discovery

private final class BonjourDiscovery: NSObject, NetServiceBrowserDelegate, NetServiceDelegate {
    private let browser = NetServiceBrowser()
    private var resolving: [NetService] = []

    /// Called on the main thread when a service is resolved to (name, hostName, port).
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
        guard let host = sender.hostName else { return }
        let name = sender.name
        resolving.removeAll { $0 == sender }
        DispatchQueue.main.async { self.onFound(name, host) }
    }

    func netService(_ sender: NetService, didNotResolve errorDict: [String: NSNumber]) {
        resolving.removeAll { $0 == sender }
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
