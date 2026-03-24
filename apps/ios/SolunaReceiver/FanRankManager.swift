import SwiftUI
import Combine

/// Fan Rank system — tracks listening time and awards ranks
class FanRankManager: ObservableObject {
    static let shared = FanRankManager()

    enum Rank: Int, CaseIterable, Comparable {
        case beginner = 0
        case bronze = 1
        case silver = 2
        case gold = 3
        case platinum = 4
        case diamond = 5

        static func < (lhs: Rank, rhs: Rank) -> Bool { lhs.rawValue < rhs.rawValue }

        var name: String {
            switch self {
            case .beginner: return "Beginner"
            case .bronze: return "Bronze"
            case .silver: return "Silver"
            case .gold: return "Gold"
            case .platinum: return "Platinum"
            case .diamond: return "Diamond"
            }
        }

        var nameJa: String {
            switch self {
            case .beginner: return "ビギナー"
            case .bronze: return "ブロンズ"
            case .silver: return "シルバー"
            case .gold: return "ゴールド"
            case .platinum: return "プラチナ"
            case .diamond: return "ダイヤモンド"
            }
        }

        var icon: String {
            switch self {
            case .beginner: return "🎵"
            case .bronze: return "🥉"
            case .silver: return "🥈"
            case .gold: return "🥇"
            case .platinum: return "💎"
            case .diamond: return "👑"
            }
        }

        var requiredListens: Int {
            switch self {
            case .beginner: return 0
            case .bronze: return 10
            case .silver: return 100
            case .gold: return 1000
            case .platinum: return 10000
            case .diamond: return 100000
            }
        }

        var perk: String {
            switch self {
            case .beginner: return "基本視聴"
            case .bronze: return "限定コンテンツ"
            case .silver: return "先行試聴"
            case .gold: return "バックステージ映像"
            case .platinum: return "クレジット掲載"
            case .diamond: return "アーティスト直接交流"
            }
        }

        var perkEn: String {
            switch self {
            case .beginner: return "Basic listening"
            case .bronze: return "Exclusive content"
            case .silver: return "Early access"
            case .gold: return "Backstage footage"
            case .platinum: return "Credits listing"
            case .diamond: return "Direct artist interaction"
            }
        }

        static func fromListenCount(_ count: Int) -> Rank {
            for rank in Rank.allCases.reversed() {
                if count >= rank.requiredListens { return rank }
            }
            return .beginner
        }
    }

    @Published var totalListens: Int = 0
    @Published var currentRank: Rank = .beginner
    @Published var enaiPoints: Int = 0
    @Published var channelListens: [String: Int] = [:]  // channel -> listen count

    private let listenKey = "soluna_total_listens"
    private let enaiKey = "soluna_enai_points"
    private let channelKey = "soluna_channel_listens"

    private init() {
        loadFromDefaults()
    }

    /// Record a listen event (call when a packet is received, throttle to once per song/30s)
    func recordListen(channel: String) {
        totalListens += 1
        channelListens[channel, default: 0] += 1

        // Award ENAI points: 1 point per 10 listens
        if totalListens % 10 == 0 {
            enaiPoints += 1
        }

        let newRank = Rank.fromListenCount(totalListens)
        if newRank > currentRank {
            currentRank = newRank
            // Could trigger a notification/animation here
        }

        // Save periodically (every 10 listens)
        if totalListens % 10 == 0 {
            saveToDefaults()
        }
    }

    /// Progress to next rank (0.0 to 1.0)
    var progressToNextRank: Double {
        let current = currentRank.requiredListens
        let nextRank = Rank.allCases.first(where: { $0.rawValue == currentRank.rawValue + 1 })
        guard let next = nextRank else { return 1.0 } // Already diamond
        let needed = next.requiredListens - current
        let progress = totalListens - current
        return min(1.0, Double(progress) / Double(needed))
    }

    /// Next rank (nil if diamond)
    var nextRank: Rank? {
        Rank.allCases.first(where: { $0.rawValue == currentRank.rawValue + 1 })
    }

    /// Listens needed for next rank
    var listensToNextRank: Int {
        guard let next = nextRank else { return 0 }
        return max(0, next.requiredListens - totalListens)
    }

    private func loadFromDefaults() {
        let defaults = UserDefaults.standard
        totalListens = defaults.integer(forKey: listenKey)
        enaiPoints = defaults.integer(forKey: enaiKey)
        if let data = defaults.data(forKey: channelKey),
           let dict = try? JSONDecoder().decode([String: Int].self, from: data) {
            channelListens = dict
        }
        currentRank = Rank.fromListenCount(totalListens)
    }

    func saveToDefaults() {
        let defaults = UserDefaults.standard
        defaults.set(totalListens, forKey: listenKey)
        defaults.set(enaiPoints, forKey: enaiKey)
        if let data = try? JSONEncoder().encode(channelListens) {
            defaults.set(data, forKey: channelKey)
        }
    }
}
