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
        }
        .onChange(of: scenePhase) { phase in
            if phase == .background {
                // Keep audio session active in background
                try? AVAudioSession.sharedInstance().setActive(true)
            }
        }
    }
}
