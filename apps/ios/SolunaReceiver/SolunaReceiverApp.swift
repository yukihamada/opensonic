//
//  SolunaApp.swift
//  Soluna
//
//  iOS app for receiving Soluna network audio streams
//

import SwiftUI
import AVFoundation
import UIKit
import UserNotifications

// MARK: - AppDelegate (APNs)

class AppDelegate: NSObject, UIApplicationDelegate, UNUserNotificationCenterDelegate {
    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        UNUserNotificationCenter.current().delegate = self
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound, .badge]) { granted, _ in
            if granted {
                DispatchQueue.main.async { UIApplication.shared.registerForRemoteNotifications() }
            }
        }
        return true
    }

    func application(_ application: UIApplication,
                     didRegisterForRemoteNotificationsWithDeviceToken deviceToken: Data) {
        let tokenStr = deviceToken.map { String(format: "%02x", $0) }.joined()
        // Save token locally
        UserDefaults.standard.set(tokenStr, forKey: "apns_device_token")
        // Register with relay server
        Task {
            guard let token = UserDefaults.standard.string(forKey: "soluna_auth_token") else { return }
            guard let url = URL(string: "https://relay.solun.art/api/auth/register-push") else { return }
            var req = URLRequest(url: url)
            req.httpMethod = "POST"
            req.setValue("application/json", forHTTPHeaderField: "Content-Type")
            req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
            let body: [String: Any] = [
                "apns_token": tokenStr,
                "device_id": UIDevice.current.identifierForVendor?.uuidString ?? ""
            ]
            req.httpBody = try? JSONSerialization.data(withJSONObject: body)
            try? await URLSession.shared.data(for: req)
        }
    }

    func userNotificationCenter(_ center: UNUserNotificationCenter,
                                 willPresent notification: UNNotification,
                                 withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void) {
        completionHandler([.banner, .sound])
    }
}

// MARK: - App

@main
struct SolunaReceiverApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @Environment(\.scenePhase) private var scenePhase
    @StateObject private var deepLink = DeepLinkManager()

    init() {
        // Configure audio session early for background playback
        let session = AVAudioSession.sharedInstance()
        do {
            try session.setCategory(.playback, mode: .default, options: [.mixWithOthers, .allowBluetooth])
            try session.setActive(true)
        } catch {
            print("[SolunaApp] AudioSession init error: \(error)")
        }
    }

    @AppStorage("hasChosenChannel") private var hasChosenChannel = false

    var body: some Scene {
        WindowGroup {
            Group {
                if hasChosenChannel {
                    ContentView()
                        .environmentObject(deepLink)
                } else {
                    ChannelOnboardingView {
                        hasChosenChannel = true
                    }
                    .environmentObject(deepLink)
                }
            }
            .onOpenURL { url in
                deepLink.handle(url: url)
                hasChosenChannel = true
            }
            .onContinueUserActivity(NSUserActivityTypeBrowsingWeb) { activity in
                if let url = activity.webpageURL {
                    deepLink.handle(url: url)
                    hasChosenChannel = true
                }
            }
        }
        .onChange(of: scenePhase) { phase in
            switch phase {
            case .background:
                // Keep audio session active in background
                try? AVAudioSession.sharedInstance().setActive(true)
            case .active:
                // Re-activate audio session when returning from background
                try? AVAudioSession.sharedInstance().setActive(true)
            default:
                break
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
