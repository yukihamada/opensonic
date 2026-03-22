//
//  ArtistDashboard.swift
//  Soluna — Artist Revenue Dashboard (Live)
//

import SwiftUI

@MainActor
class ArtistRevenueManager: ObservableObject {
    static let shared = ArtistRevenueManager()

    @Published var totalRevenue: Int = 0  // in credits
    @Published var todayRevenue: Int = 0
    @Published var revenueHistory: [(time: Date, amount: Int)] = []
    @Published var topSupporters: [(name: String, amount: Int)] = []

    func handleTip(from: String, amount: Int) {
        totalRevenue += amount
        todayRevenue += amount
        revenueHistory.append((Date(), amount))

        // Update top supporters
        if let idx = topSupporters.firstIndex(where: { $0.name == from }) {
            topSupporters[idx].amount += amount
        } else {
            topSupporters.append((from, amount))
        }
        topSupporters.sort { $0.amount > $1.amount }
        if topSupporters.count > 10 { topSupporters = Array(topSupporters.prefix(10)) }
    }
}

struct ArtistDashboardView: View {
    @ObservedObject var revenue = ArtistRevenueManager.shared
    @ObservedObject var fanRank = FanRankManager.shared
    @Environment(\.dismiss) var dismiss

    var body: some View {
        NavigationStack {
            ZStack {
                LinearGradient.solunaBg.ignoresSafeArea()
                ScrollView {
                    VStack(spacing: 16) {
                        // Big revenue number with animation
                        VStack(spacing: 4) {
                            Text("\(revenue.todayRevenue)")
                                .font(.system(size: 56, weight: .black, design: .rounded))
                                .foregroundStyle(LinearGradient.solLunaGradient)
                            Text("credits today")
                                .font(.caption)
                                .foregroundColor(.white.opacity(0.4))
                        }
                        .padding(.vertical, 20)

                        // Stats row
                        HStack(spacing: 20) {
                            statCard(title: "All Time", value: "\(revenue.totalRevenue)")
                            statCard(title: "Listeners", value: "--")
                            statCard(title: "Tips", value: "\(revenue.revenueHistory.count)")
                        }

                        // Top supporters
                        if !revenue.topSupporters.isEmpty {
                            VStack(alignment: .leading, spacing: 8) {
                                Text("Top Supporters")
                                    .font(.system(size: 13, weight: .bold))
                                    .foregroundColor(.white.opacity(0.6))
                                ForEach(Array(revenue.topSupporters.enumerated()), id: \.offset) { idx, supporter in
                                    HStack {
                                        Text("\(idx + 1)")
                                            .font(.system(size: 11, weight: .bold, design: .rounded))
                                            .foregroundColor(.white.opacity(0.3))
                                            .frame(width: 20)
                                        Text(supporter.name)
                                            .font(.system(size: 13, weight: .medium))
                                            .foregroundColor(.white)
                                        Spacer()
                                        Text("\(supporter.amount)")
                                            .font(.system(size: 13, weight: .bold, design: .monospaced))
                                            .foregroundColor(.solunaSol)
                                    }
                                    .padding(.vertical, 4)
                                }
                            }
                            .padding(12)
                            .glassCard()
                        }

                        // Recent tips feed
                        if !revenue.revenueHistory.isEmpty {
                            VStack(alignment: .leading, spacing: 6) {
                                Text("Recent Tips")
                                    .font(.system(size: 13, weight: .bold))
                                    .foregroundColor(.white.opacity(0.6))
                                ForEach(revenue.revenueHistory.suffix(5).reversed(), id: \.time) { entry in
                                    HStack {
                                        Text("+\(entry.amount) credits")
                                            .font(.system(size: 12, design: .monospaced))
                                            .foregroundColor(.green)
                                        Spacer()
                                        Text(entry.time, style: .time)
                                            .font(.system(size: 10))
                                            .foregroundColor(.white.opacity(0.3))
                                    }
                                }
                            }
                            .padding(12)
                            .glassCard()
                        }
                    }
                    .padding(16)
                }
            }
            .navigationTitle("Artist Dashboard")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }

    private func statCard(title: String, value: String) -> some View {
        VStack(spacing: 4) {
            Text(value)
                .font(.system(size: 20, weight: .bold, design: .rounded))
                .foregroundColor(.white)
            Text(title)
                .font(.system(size: 10, weight: .medium))
                .foregroundColor(.white.opacity(0.4))
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 12)
        .glassCard()
    }
}
