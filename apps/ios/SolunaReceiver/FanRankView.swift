import SwiftUI

/// Fan Rank dashboard — shows rank, progress, stats, and perks
struct FanRankView: View {
    @ObservedObject private var rankManager = FanRankManager.shared
    @State private var animateProgress = false
    @State private var selectedRank: FanRankManager.Rank?

    var body: some View {
        ScrollView(.vertical, showsIndicators: false) {
            VStack(spacing: 24) {
                // MARK: - Current Rank Hero
                currentRankCard

                // MARK: - Progress to Next Rank
                progressCard

                // MARK: - All Ranks (horizontal scroll)
                rankTierScroll

                // MARK: - Stats
                statsCard

                // MARK: - Top Channels
                if !rankManager.channelListens.isEmpty {
                    topChannelsCard
                }
            }
            .padding(.horizontal, 16)
            .padding(.top, 8)
            .padding(.bottom, 32)
        }
        .background(LinearGradient.solunaBg.ignoresSafeArea())
        .navigationTitle("Fan Rank")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear { withAnimation(.easeOut(duration: 0.8)) { animateProgress = true } }
    }

    // MARK: - Current Rank Card

    private var currentRankCard: some View {
        VStack(spacing: 12) {
            Text(rankManager.currentRank.icon)
                .font(.system(size: 64))
                .shadow(color: rankColor(rankManager.currentRank).opacity(0.6), radius: 20)

            Text(rankManager.currentRank.name)
                .font(.system(size: 28, weight: .bold, design: .rounded))
                .foregroundStyle(rankGradient(rankManager.currentRank))

            Text(rankManager.currentRank.nameJa)
                .font(.system(size: 14, weight: .medium))
                .foregroundColor(.white.opacity(0.5))

            // Perk badge
            HStack(spacing: 6) {
                Image(systemName: "star.fill")
                    .font(.system(size: 10))
                Text(rankManager.currentRank.perk)
                    .font(.system(size: 12, weight: .medium))
            }
            .foregroundColor(rankColor(rankManager.currentRank))
            .padding(.horizontal, 12)
            .padding(.vertical, 6)
            .background(rankColor(rankManager.currentRank).opacity(0.15))
            .clipShape(Capsule())
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 28)
        .glassCard()
    }

    // MARK: - Progress Card

    private var progressCard: some View {
        VStack(spacing: 14) {
            if let next = rankManager.nextRank {
                HStack {
                    Text(rankManager.currentRank.icon)
                        .font(.system(size: 20))
                    Spacer()
                    Text("\(rankManager.listensToNextRank) listens to go")
                        .font(.system(size: 12, weight: .medium))
                        .foregroundColor(.white.opacity(0.5))
                    Spacer()
                    Text(next.icon)
                        .font(.system(size: 20))
                }

                // Progress bar with Sol+Luna gradient
                GeometryReader { geo in
                    ZStack(alignment: .leading) {
                        RoundedRectangle(cornerRadius: 6)
                            .fill(Color.white.opacity(0.08))
                            .frame(height: 12)

                        RoundedRectangle(cornerRadius: 6)
                            .fill(LinearGradient.solLunaGradient)
                            .frame(
                                width: animateProgress
                                    ? geo.size.width * CGFloat(rankManager.progressToNextRank)
                                    : 0,
                                height: 12
                            )
                            .animation(.easeOut(duration: 1.0), value: animateProgress)
                    }
                }
                .frame(height: 12)

                HStack {
                    Text(rankManager.currentRank.name)
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundColor(rankColor(rankManager.currentRank))
                    Spacer()
                    Text("\(Int(rankManager.progressToNextRank * 100))%")
                        .font(.system(size: 11, weight: .bold, design: .monospaced))
                        .foregroundColor(.white.opacity(0.7))
                    Spacer()
                    Text(next.name)
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundColor(rankColor(next))
                }
            } else {
                // Diamond — max rank
                HStack(spacing: 8) {
                    Image(systemName: "crown.fill")
                        .foregroundColor(.rankDiamond)
                    Text("Maximum rank achieved")
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundColor(.white.opacity(0.8))
                }
            }
        }
        .padding(16)
        .glassCard()
    }

    // MARK: - Rank Tier Scroll

    private var rankTierScroll: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("ALL RANKS")
                .font(.system(size: 11, weight: .bold))
                .foregroundColor(.white.opacity(0.4))
                .tracking(1.5)
                .padding(.leading, 4)

            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 12) {
                    ForEach(FanRankManager.Rank.allCases, id: \.rawValue) { rank in
                        rankTile(rank)
                            .onTapGesture { withAnimation(.spring(response: 0.3)) {
                                selectedRank = selectedRank == rank ? nil : rank
                            }}
                    }
                }
                .padding(.horizontal, 4)
                .padding(.vertical, 4)
            }

            // Expanded detail for selected rank
            if let rank = selectedRank {
                rankDetailCard(rank)
                    .transition(.asymmetric(
                        insertion: .move(edge: .top).combined(with: .opacity),
                        removal: .opacity
                    ))
            }
        }
    }

    private func rankTile(_ rank: FanRankManager.Rank) -> some View {
        let isCurrent = rank == rankManager.currentRank
        let isUnlocked = rank <= rankManager.currentRank

        return VStack(spacing: 6) {
            Text(rank.icon)
                .font(.system(size: isCurrent ? 32 : 24))
                .opacity(isUnlocked ? 1.0 : 0.3)

            Text(rank.name)
                .font(.system(size: 10, weight: isCurrent ? .bold : .medium))
                .foregroundColor(isUnlocked ? .white : .white.opacity(0.3))

            Text("\(rank.requiredListens)")
                .font(.system(size: 9, weight: .medium, design: .monospaced))
                .foregroundColor(.white.opacity(0.3))
        }
        .frame(width: 72, height: 90)
        .background(
            RoundedRectangle(cornerRadius: 14)
                .fill(isCurrent
                    ? AnyShapeStyle(rankGradient(rank).opacity(0.25))
                    : AnyShapeStyle(Color.white.opacity(0.04))
                )
                .overlay(
                    RoundedRectangle(cornerRadius: 14)
                        .stroke(
                            isCurrent
                                ? rankColor(rank).opacity(0.6)
                                : Color.white.opacity(0.06),
                            lineWidth: isCurrent ? 1.5 : 0.5
                        )
                )
        )
        .scaleEffect(selectedRank == rank ? 1.08 : 1.0)
    }

    private func rankDetailCard(_ rank: FanRankManager.Rank) -> some View {
        HStack(spacing: 14) {
            Text(rank.icon)
                .font(.system(size: 36))

            VStack(alignment: .leading, spacing: 4) {
                Text(rank.name)
                    .font(.system(size: 16, weight: .bold))
                    .foregroundStyle(rankGradient(rank))

                Text(rank.perkEn)
                    .font(.system(size: 12, weight: .medium))
                    .foregroundColor(.white.opacity(0.6))

                Text("Requires \(rank.requiredListens) listens")
                    .font(.system(size: 11, weight: .medium, design: .monospaced))
                    .foregroundColor(.white.opacity(0.35))
            }

            Spacer()

            if rank <= rankManager.currentRank {
                Image(systemName: "checkmark.seal.fill")
                    .font(.system(size: 20))
                    .foregroundColor(.solunaLive)
            } else {
                Image(systemName: "lock.fill")
                    .font(.system(size: 16))
                    .foregroundColor(.white.opacity(0.2))
            }
        }
        .padding(14)
        .glassCard(cornerRadius: 14)
    }

    // MARK: - Stats Card

    private var statsCard: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("STATS")
                .font(.system(size: 11, weight: .bold))
                .foregroundColor(.white.opacity(0.4))
                .tracking(1.5)

            HStack(spacing: 0) {
                statItem(
                    icon: "headphones",
                    value: formatNumber(rankManager.totalListens),
                    label: "Listens",
                    gradient: .solGradient
                )

                divider

                statItem(
                    icon: "star.circle.fill",
                    value: formatNumber(rankManager.enaiPoints),
                    label: "ENAI",
                    gradient: .lunaGradient
                )

                divider

                statItem(
                    icon: "antenna.radiowaves.left.and.right",
                    value: "\(rankManager.channelListens.count)",
                    label: "Channels",
                    gradient: .solunaAccent
                )
            }
        }
        .padding(16)
        .glassCard()
    }

    private var divider: some View {
        Rectangle()
            .fill(Color.white.opacity(0.06))
            .frame(width: 1, height: 40)
    }

    private func statItem(icon: String, value: String, label: String, gradient: LinearGradient) -> some View {
        VStack(spacing: 6) {
            Image(systemName: icon)
                .font(.system(size: 16))
                .foregroundStyle(gradient)

            Text(value)
                .font(.system(size: 20, weight: .bold, design: .rounded))
                .foregroundColor(.white)

            Text(label)
                .font(.system(size: 10, weight: .medium))
                .foregroundColor(.white.opacity(0.4))
        }
        .frame(maxWidth: .infinity)
    }

    // MARK: - Top Channels Card

    private var topChannelsCard: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("TOP CHANNELS")
                .font(.system(size: 11, weight: .bold))
                .foregroundColor(.white.opacity(0.4))
                .tracking(1.5)

            let sorted = rankManager.channelListens
                .sorted { $0.value > $1.value }
                .prefix(5)
            let maxCount = sorted.first?.value ?? 1

            ForEach(Array(sorted.enumerated()), id: \.element.key) { index, entry in
                HStack(spacing: 12) {
                    Text("\(index + 1)")
                        .font(.system(size: 12, weight: .bold, design: .monospaced))
                        .foregroundColor(.white.opacity(0.3))
                        .frame(width: 20)

                    VStack(alignment: .leading, spacing: 4) {
                        Text(entry.key)
                            .font(.system(size: 14, weight: .semibold))
                            .foregroundColor(.white)

                        GeometryReader { geo in
                            RoundedRectangle(cornerRadius: 3)
                                .fill(channelBarGradient(index: index))
                                .frame(
                                    width: geo.size.width * CGFloat(entry.value) / CGFloat(maxCount),
                                    height: 6
                                )
                        }
                        .frame(height: 6)
                    }

                    Text(formatNumber(entry.value))
                        .font(.system(size: 12, weight: .medium, design: .monospaced))
                        .foregroundColor(.white.opacity(0.5))
                }
            }
        }
        .padding(16)
        .glassCard()
    }

    // MARK: - Helpers

    private func rankColor(_ rank: FanRankManager.Rank) -> Color {
        switch rank {
        case .beginner: return .solunaGradientEnd
        case .bronze: return .rankBronze
        case .silver: return .rankSilver
        case .gold: return .rankGold
        case .platinum: return .rankPlatinum
        case .diamond: return .rankDiamond
        }
    }

    private func rankGradient(_ rank: FanRankManager.Rank) -> LinearGradient {
        switch rank {
        case .beginner:
            return LinearGradient(colors: [.solunaGradientMid, .solunaGradientEnd], startPoint: .leading, endPoint: .trailing)
        case .bronze:
            return LinearGradient(colors: [.rankBronze, .solunaSol], startPoint: .leading, endPoint: .trailing)
        case .silver:
            return LinearGradient(colors: [.rankSilver, .white], startPoint: .leading, endPoint: .trailing)
        case .gold:
            return LinearGradient(colors: [.solunaSol, .rankGold], startPoint: .leading, endPoint: .trailing)
        case .platinum:
            return LinearGradient(colors: [.rankPlatinum, .solunaLuna], startPoint: .leading, endPoint: .trailing)
        case .diamond:
            return LinearGradient(colors: [.rankDiamond, .solunaLunaEnd], startPoint: .leading, endPoint: .trailing)
        }
    }

    private func channelBarGradient(index: Int) -> LinearGradient {
        let gradients: [LinearGradient] = [.solGradient, .lunaGradient, .solunaAccent, .solLunaGradient, .solunaSubtle]
        return gradients[index % gradients.count]
    }

    private func formatNumber(_ n: Int) -> String {
        if n >= 100_000 { return String(format: "%.0fK", Double(n) / 1000) }
        if n >= 10_000 { return String(format: "%.1fK", Double(n) / 1000) }
        if n >= 1_000 { return String(format: "%.1fK", Double(n) / 1000) }
        return "\(n)"
    }
}

// MARK: - Preview

#Preview {
    NavigationStack {
        FanRankView()
    }
    .preferredColorScheme(.dark)
}
