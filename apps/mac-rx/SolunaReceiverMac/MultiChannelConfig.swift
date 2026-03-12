//
//  MultiChannelConfig.swift
//  SolunaReceiverMac
//
//  Configuration for multi-channel DJ mode.
//  Allows routing different audio sources to different Soluna channels.
//

import Foundation

struct ChannelRoute: Identifiable, Codable {
    let id: UUID
    var channelName: String
    var audioSource: String  // CoreAudio device UID or "system" for ScreenCaptureKit
    var isEnabled: Bool

    init(channelName: String, audioSource: String, isEnabled: Bool = true) {
        self.id = UUID()
        self.channelName = channelName
        self.audioSource = audioSource
        self.isEnabled = isEnabled
    }
}

@MainActor
final class MultiChannelConfig: ObservableObject {
    @Published var routes: [ChannelRoute] = []

    private let storageKey = "multiChannelRoutes"

    init() {
        load()
    }

    func addRoute(channel: String, source: String) {
        routes.append(ChannelRoute(channelName: channel, audioSource: source))
        save()
    }

    func removeRoute(at offsets: IndexSet) {
        routes.remove(atOffsets: offsets)
        save()
    }

    func save() {
        if let data = try? JSONEncoder().encode(routes) {
            UserDefaults.standard.set(data, forKey: storageKey)
        }
    }

    func load() {
        guard let data = UserDefaults.standard.data(forKey: storageKey),
              let decoded = try? JSONDecoder().decode([ChannelRoute].self, from: data) else { return }
        routes = decoded
    }

    /// List available CoreAudio devices
    static func availableAudioDevices() -> [(name: String, uid: String)] {
        // This would use CoreAudio AudioObjectGetPropertyData
        // For now return a placeholder that can be filled in with actual CoreAudio enumeration
        return [
            (name: "System Audio (ScreenCaptureKit)", uid: "system"),
            (name: "BlackHole 2ch", uid: "BlackHole2ch_UID"),
        ]
    }
}
