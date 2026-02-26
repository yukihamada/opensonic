//
//  SpeakersController.swift
//  SolunaReceiver
//
//  Manages a list of remote speakers (each backed by a DaemonClient WebSocket connection).
//

import Foundation
import SwiftUI

@MainActor
final class SpeakersController: ObservableObject {

    struct Speaker: Identifiable, Codable {
        var id:   UUID   = UUID()
        var name: String
        var host: String
    }

    @Published var speakers: [Speaker] = []

    // NOT @Published — each RemoteSpeakerRow observes its daemon directly
    var clients: [UUID: DaemonClient] = [:]

    private static let storageKey = "soluna_speakers_v1"

    init() { load() }

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

    private func attach(_ s: Speaker) {
        let d = DaemonClient()
        d.connect(host: s.host)
        clients[s.id] = d
    }

    private func persist() {
        if let data = try? JSONEncoder().encode(speakers) {
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
