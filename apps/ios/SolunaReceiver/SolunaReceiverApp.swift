//
//  SolunaReceiverApp.swift
//  SolunaReceiver
//
//  iOS app for receiving Soluna network audio streams
//

import SwiftUI
import AVFoundation

@main
struct SolunaReceiverApp: App {
    @Environment(\.scenePhase) private var scenePhase
    @StateObject private var deepLink = DeepLinkManager()

    init() {
        // Configure audio session early for background playback
        let session = AVAudioSession.sharedInstance()
        do {
            try session.setCategory(.playback, mode: .default, options: [.duckOthers])
            try session.setActive(true)
        } catch {
            print("[SolunaApp] AudioSession init error: \(error)")
        }
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(deepLink)
                .onOpenURL { url in
                    deepLink.handle(url: url)
                }
                .onContinueUserActivity(NSUserActivityTypeBrowsingWeb) { activity in
                    if let url = activity.webpageURL {
                        deepLink.handle(url: url)
                    }
                }
        }
        .onChange(of: scenePhase) { phase in
            if phase == .background {
                // Keep audio session active in background
                try? AVAudioSession.sharedInstance().setActive(true)
            }
        }
    }
}

/// Manages deep link / Universal Link channel navigation
class DeepLinkManager: ObservableObject {
    @Published var pendingChannel: String?

    func handle(url: URL) {
        // Universal Link: https://relay.solun.art/c/<channel>
        if let host = url.host,
           (host == "relay.solun.art" || host == "solun.art"),
           url.pathComponents.count >= 3,
           url.pathComponents[1] == "c" {
            let channel = url.pathComponents[2].removingPercentEncoding ?? url.pathComponents[2]
            print("[DeepLink] Universal Link channel: \(channel)")
            pendingChannel = channel
            return
        }

        // Custom URL scheme: soluna://channel/<name>
        if url.scheme == "soluna", url.host == "channel" {
            let channel = url.path.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
            if !channel.isEmpty {
                print("[DeepLink] URL scheme channel: \(channel)")
                pendingChannel = channel
            }
            return
        }

        print("[DeepLink] Unhandled URL: \(url)")
    }
}
