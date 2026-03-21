//
//  SocialListening.swift
//  Soluna
//
//  Real-time listener count + presence per channel
//

import SwiftUI

// MARK: - Social Listening Manager

@MainActor
class SocialListeningManager: ObservableObject {
    static let shared = SocialListeningManager()

    @Published var listenerCounts: [String: Int] = [:]  // channel -> count
    @Published var currentChannelListeners: [ListenerInfo] = []

    struct ListenerInfo: Identifiable {
        let id: String  // device ID
        let name: String
        let role: String  // "dj" or "listener"
    }

    private var pollTimer: Timer?

    /// Start polling the relay for listener counts across all channels
    func startPolling() {
        stopPolling()
        // Fetch immediately, then every 10 seconds
        Task { await fetchCounts() }
        pollTimer = Timer.scheduledTimer(withTimeInterval: 10, repeats: true) { [weak self] _ in
            Task { @MainActor in
                await self?.fetchCounts()
            }
        }
    }

    /// Stop polling
    func stopPolling() {
        pollTimer?.invalidate()
        pollTimer = nil
    }

    private func fetchCounts() async {
        guard let url = URL(string: "https://relay.solun.art/api/channels") else { return }
        do {
            let (data, response) = try await URLSession.shared.data(from: url)
            guard let httpResponse = response as? HTTPURLResponse,
                  (200...299).contains(httpResponse.statusCode) else { return }
            if let json = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] {
                var counts: [String: Int] = [:]
                var listeners: [ListenerInfo] = []
                for ch in json {
                    if let name = ch["name"] as? String, let count = ch["listeners"] as? Int {
                        counts[name] = count
                    }
                    // Parse members if present for current channel detail
                    if let members = ch["members"] as? [[String: Any]] {
                        for member in members {
                            let deviceId = member["device_id"] as? String ?? UUID().uuidString
                            let displayName = member["name"] as? String ?? "Listener"
                            let role = member["role"] as? String ?? "listener"
                            listeners.append(ListenerInfo(id: deviceId, name: displayName, role: role))
                        }
                    }
                }
                self.listenerCounts = counts
                if !listeners.isEmpty {
                    self.currentChannelListeners = listeners
                }
            }
        } catch {
            // Silently fail — listener counts are best-effort
        }
    }

    /// Get listener count for a specific channel
    func count(for channel: String) -> Int {
        listenerCounts[channel] ?? 0
    }
}

// MARK: - Listener Badge View

struct ListenerBadge: View {
    let count: Int

    var body: some View {
        if count > 0 {
            HStack(spacing: 2) {
                Image(systemName: "person.2.fill")
                    .font(.system(size: 9))
                Text("\(count)")
                    .font(.system(size: 10, weight: .medium, design: .rounded))
            }
            .foregroundColor(.white.opacity(0.7))
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(Color.white.opacity(0.1))
            .clipShape(Capsule())
        }
    }
}

#if DEBUG
struct ListenerBadge_Previews: PreviewProvider {
    static var previews: some View {
        HStack(spacing: 12) {
            ListenerBadge(count: 0)
            ListenerBadge(count: 3)
            ListenerBadge(count: 42)
        }
        .padding()
        .background(Color.black)
        .preferredColorScheme(.dark)
    }
}
#endif
