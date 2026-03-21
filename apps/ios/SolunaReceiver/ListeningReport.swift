//
//  ListeningReport.swift
//  SolunaReceiver
//
//  Weekly listening report — Spotify Wrapped style stats view.
//

import SwiftUI

struct ListeningReportView: View {
    @ObservedObject var fanRank = FanRankManager.shared

    /// Total minutes approximation: each "listen" event = ~30s of listening
    private var totalMinutes: Int {
        fanRank.totalListens / 2  // 30s per listen event
    }

    /// Top channel by listen count
    private var topChannel: String {
        fanRank.channelListens.max(by: { $0.value < $1.value })?.key ?? "soluna"
    }

    /// Streak days (stored in UserDefaults)
    private var streakDays: Int {
        UserDefaults.standard.integer(forKey: "soluna_streak_days")
    }

    /// Current badge emoji
    private var currentBadge: String {
        fanRank.currentRank.icon
    }

    var body: some View {
        VStack(spacing: 16) {
            Text("This Week")
                .font(.title2.bold())
                .foregroundColor(.white)

            // Big number
            Text(formatTime(totalMinutes))
                .font(.system(size: 48, weight: .black, design: .rounded))
                .foregroundStyle(LinearGradient.solLunaGradient)

            Text("listening time")
                .font(.caption)
                .foregroundColor(.secondary)

            // Stats grid
            HStack(spacing: 20) {
                statCard("Top Channel", topChannel.capitalized)
                statCard("Streak", "\(streakDays) days")
                statCard("Rank", currentBadge)
            }

            Spacer().frame(height: 8)

            // Share card
            Button {
                shareStats()
            } label: {
                HStack(spacing: 6) {
                    Image(systemName: "square.and.arrow.up")
                        .font(.system(size: 14, weight: .semibold))
                    Text("Share My Stats")
                        .font(.headline)
                }
                .foregroundColor(.white)
                .padding(.horizontal, 24)
                .padding(.vertical, 12)
                .background(LinearGradient.solGradient)
                .clipShape(Capsule())
            }
        }
        .padding(24)
        .glassCard()
    }

    private func statCard(_ title: String, _ value: String) -> some View {
        VStack(spacing: 6) {
            Text(value)
                .font(.system(size: 18, weight: .bold, design: .rounded))
                .foregroundColor(.white)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
            Text(title)
                .font(.system(size: 10, weight: .medium))
                .foregroundColor(.white.opacity(0.5))
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 12)
        .background(Color.white.opacity(0.06))
        .clipShape(RoundedRectangle(cornerRadius: 12))
    }

    private func formatTime(_ minutes: Int) -> String {
        if minutes < 60 {
            return "\(minutes)m"
        } else {
            let h = minutes / 60
            let m = minutes % 60
            return m > 0 ? "\(h)h \(m)m" : "\(h)h"
        }
    }

    private func shareStats() {
        let text = """
        🎧 My Soluna Week
        ⏱ \(formatTime(totalMinutes)) listening
        📻 Top: \(topChannel.capitalized)
        🔥 \(streakDays) day streak
        \(currentBadge) \(fanRank.currentRank.name)

        solun.art
        """
        let av = UIActivityViewController(activityItems: [text], applicationActivities: nil)
        if let windowScene = UIApplication.shared.connectedScenes.first as? UIWindowScene,
           let root = windowScene.windows.first?.rootViewController {
            root.present(av, animated: true)
        }
    }
}
