//
//  SubscriptionView.swift
//  Soluna
//
//  Subscription upsell — 3-plan comparison (Free / Pro / Studio)
//

import SwiftUI

struct SubscriptionView: View {
    @ObservedObject var store: ChannelStore
    @Environment(\.dismiss) private var dismiss
    @State private var selectedPlan: SolunaPlan = .pro

    var body: some View {
        NavigationView {
            ZStack {
                LinearGradient.solunaBg.ignoresSafeArea()

                ScrollView(showsIndicators: false) {
                    VStack(spacing: 20) {
                        // Hero
                        VStack(spacing: 8) {
                            Text("Soluna")
                                .font(.system(size: 32, weight: .bold, design: .rounded))
                                .foregroundStyle(LinearGradient(
                                    colors: [.solunaGradientStart, .solunaGradientEnd],
                                    startPoint: .leading, endPoint: .trailing
                                ))
                            Text("\u{3042}\u{306A}\u{305F}\u{306B}\u{5408}\u{3063}\u{305F}\u{30D7}\u{30E9}\u{30F3}\u{3092}\u{9078}\u{3076}")
                                .font(.system(size: 15))
                                .foregroundColor(.white.opacity(0.6))
                        }
                        .padding(.top, 20)

                        // Current plan badge
                        if store.currentPlan != .free {
                            HStack(spacing: 8) {
                                Image(systemName: "checkmark.seal.fill")
                                    .foregroundColor(.solunaLive)
                                Text("\u{73FE}\u{5728}\u{306E}\u{30D7}\u{30E9}\u{30F3}: \(store.currentPlan.rawValue)")
                                    .font(.system(size: 14, weight: .semibold))
                                    .foregroundColor(.white)
                            }
                            .padding(12)
                            .frame(maxWidth: .infinity)
                            .background(Color.solunaLive.opacity(0.12))
                            .clipShape(RoundedRectangle(cornerRadius: 12))
                        }

                        // Plan selector tabs
                        HStack(spacing: 0) {
                            ForEach(SolunaPlan.allCases, id: \.self) { plan in
                                Button {
                                    withAnimation(.easeInOut(duration: 0.2)) { selectedPlan = plan }
                                } label: {
                                    VStack(spacing: 4) {
                                        if plan == .pro {
                                            Text("POPULAR")
                                                .font(.system(size: 8, weight: .heavy))
                                                .foregroundColor(.white)
                                                .padding(.horizontal, 6)
                                                .padding(.vertical, 2)
                                                .background(LinearGradient(
                                                    colors: [.solunaGradientStart, .solunaGradientEnd],
                                                    startPoint: .leading, endPoint: .trailing
                                                ))
                                                .clipShape(Capsule())
                                        } else {
                                            Spacer().frame(height: 14)
                                        }
                                        Text(plan.rawValue)
                                            .font(.system(size: 14, weight: .bold))
                                            .foregroundColor(selectedPlan == plan ? .white : .white.opacity(0.5))
                                        Text(plan.price)
                                            .font(.system(size: 11, weight: .medium, design: .monospaced))
                                            .foregroundColor(selectedPlan == plan ? .solunaLive : .white.opacity(0.4))
                                    }
                                    .frame(maxWidth: .infinity)
                                    .padding(.vertical, 10)
                                    .background(selectedPlan == plan ? Color.white.opacity(0.1) : Color.clear)
                                    .clipShape(RoundedRectangle(cornerRadius: 10))
                                    .overlay(
                                        RoundedRectangle(cornerRadius: 10)
                                            .stroke(selectedPlan == plan ? Color.solunaLive.opacity(0.4) : Color.clear, lineWidth: 1)
                                    )
                                }
                                .buttonStyle(.plain)
                            }
                        }
                        .padding(4)
                        .background(Color.white.opacity(0.04))
                        .clipShape(RoundedRectangle(cornerRadius: 12))

                        // Feature comparison
                        featureTable

                        // Channel slots
                        HStack(spacing: 12) {
                            Image(systemName: "antenna.radiowaves.left.and.right")
                                .font(.system(size: 16))
                                .foregroundColor(.solunaLive)
                            VStack(alignment: .leading, spacing: 2) {
                                Text("\u{30C1}\u{30E3}\u{30F3}\u{30CD}\u{30EB}\u{30B9}\u{30ED}\u{30C3}\u{30C8}")
                                    .font(.system(size: 13, weight: .semibold))
                                    .foregroundColor(.white)
                                Text(selectedPlan == .free
                                     ? "\u{30E9}\u{30F3}\u{30C0}\u{30E0}\u{30C1}\u{30E3}\u{30F3}\u{30CD}\u{30EB}\u{306E}\u{307F}"
                                     : "\u{30AB}\u{30B9}\u{30BF}\u{30E0}\u{30C1}\u{30E3}\u{30F3}\u{30CD}\u{30EB} \(selectedPlan.channelSlots)\u{500B}")
                                    .font(.system(size: 11))
                                    .foregroundColor(.white.opacity(0.5))
                            }
                            Spacer()
                            HStack(spacing: 4) {
                                ForEach(0..<3, id: \.self) { i in
                                    Circle()
                                        .fill(i < selectedPlan.channelSlots ? Color.solunaLive : Color.white.opacity(0.1))
                                        .frame(width: 10, height: 10)
                                }
                            }
                        }
                        .padding(14)
                        .background(Color.white.opacity(0.06))
                        .clipShape(RoundedRectangle(cornerRadius: 12))

                        // CTA button
                        if selectedPlan == .free {
                            if store.currentPlan == .free {
                                Text("\u{73FE}\u{5728}\u{306E}\u{30D7}\u{30E9}\u{30F3}")
                                    .font(.system(size: 14, weight: .semibold))
                                    .foregroundColor(.white.opacity(0.5))
                                    .frame(maxWidth: .infinity)
                                    .padding(.vertical, 14)
                                    .background(Color.white.opacity(0.06))
                                    .clipShape(Capsule())
                            }
                        } else if store.currentPlan == selectedPlan {
                            Text("\u{73FE}\u{5728}\u{306E}\u{30D7}\u{30E9}\u{30F3}")
                                .font(.system(size: 14, weight: .semibold))
                                .foregroundColor(.solunaLive)
                                .frame(maxWidth: .infinity)
                                .padding(.vertical, 14)
                                .background(Color.solunaLive.opacity(0.12))
                                .clipShape(Capsule())
                        } else {
                            Button {
                                Task {
                                    if selectedPlan == .pro {
                                        try? await store.purchasePro()
                                    } else {
                                        try? await store.purchaseStudio()
                                    }
                                }
                            } label: {
                                VStack(spacing: 4) {
                                    Text("\(selectedPlan.rawValue) \u{306B}\u{30A2}\u{30C3}\u{30D7}\u{30B0}\u{30EC}\u{30FC}\u{30C9}")
                                        .font(.system(size: 15, weight: .bold))
                                        .foregroundColor(.white)
                                    Text("7\u{65E5}\u{9593}\u{7121}\u{6599}\u{30C8}\u{30E9}\u{30A4}\u{30A2}\u{30EB}\u{4ED8}\u{304D}")
                                        .font(.system(size: 11))
                                        .foregroundColor(.white.opacity(0.7))
                                }
                                .frame(maxWidth: .infinity)
                                .padding(.vertical, 14)
                                .background(LinearGradient(
                                    colors: [.solunaGradientStart, .solunaGradientEnd],
                                    startPoint: .leading, endPoint: .trailing
                                ))
                                .clipShape(Capsule())
                            }
                        }

                        // Restore
                        Button("\u{8CFC}\u{5165}\u{3092}\u{5FA9}\u{5143}") {
                            Task { await store.checkEntitlements() }
                        }
                        .font(.system(size: 13))
                        .foregroundColor(.white.opacity(0.4))
                        .padding(.top, 4)
                    }
                    .padding(.horizontal, 16)
                    .padding(.bottom, 40)
                }
            }
            .navigationTitle("Subscription")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button("Done") { dismiss() }
                        .foregroundColor(.white)
                }
            }
        }
    }

    // MARK: - Feature Comparison Table

    private var featureTable: some View {
        VStack(spacing: 0) {
            ForEach(Array(selectedPlan.features.enumerated()), id: \.offset) { index, pair in
                HStack {
                    Text(pair.0)
                        .font(.system(size: 12, weight: .medium))
                        .foregroundColor(.white.opacity(0.5))
                        .frame(width: 80, alignment: .leading)
                    Spacer()
                    Text(pair.1)
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundColor(.white)
                }
                .padding(.horizontal, 14)
                .padding(.vertical, 10)
                if index < selectedPlan.features.count - 1 {
                    Divider().background(Color.white.opacity(0.06))
                }
            }
        }
        .background(Color.white.opacity(0.06))
        .clipShape(RoundedRectangle(cornerRadius: 12))
    }
}
