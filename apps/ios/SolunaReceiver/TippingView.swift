//
//  TippingView.swift
//  SolunaReceiver
//
//  Tipping/support sheet for the current DJ.
//  Glass card style matching the Soluna design system.
//

import SwiftUI

struct TippingView: View {
    let djName: String
    let djDeviceId: String
    @ObservedObject var tipManager: TipManager
    @Environment(\.dismiss) private var dismiss

    private let quickTips: [(amount: Int, label: String, emoji: String)] = [
        (100, "100", "\u{2764}\u{FE0F}"),
        (500, "500", "\u{1F525}"),
        (1000, "1K", "\u{1F48E}"),
    ]

    var body: some View {
        ZStack {
            LinearGradient.solunaBg.ignoresSafeArea()

            // Floating tip animations
            ForEach(tipManager.tipAnimations) { anim in
                FloatingTipView(emoji: anim.emoji, amount: anim.amount)
            }

            ScrollView(showsIndicators: false) {
                VStack(spacing: 24) {
                    // Header
                    VStack(spacing: 8) {
                        Text("Support the DJ").font(.system(size: 24, weight: .bold)).foregroundColor(.white)
                        HStack(spacing: 6) {
                            Image(systemName: "music.mic").font(.system(size: 14)).foregroundColor(.solunaSol)
                            Text(djName).font(.system(size: 17, weight: .semibold)).foregroundColor(.white)
                        }
                    }
                    .padding(.top, 32)

                    // DJ Avatar area
                    ZStack {
                        Circle()
                            .fill(RadialGradient(
                                colors: [.solunaSol.opacity(0.4), .solunaLuna.opacity(0.2), .clear],
                                center: .center, startRadius: 10, endRadius: 50
                            ))
                            .frame(width: 100, height: 100)
                        Image(systemName: "headphones")
                            .font(.system(size: 40, weight: .medium))
                            .foregroundColor(.white.opacity(0.7))
                    }

                    // Session total
                    VStack(spacing: 4) {
                        Text("This session").font(.system(size: 13)).foregroundColor(.white.opacity(0.4))
                        HStack(spacing: 4) {
                            Image(systemName: "heart.fill").font(.system(size: 14)).foregroundColor(.solunaSolEnd)
                            Text("\(tipManager.totalTipped)")
                                .font(.system(size: 28, weight: .bold, design: .rounded))
                                .foregroundColor(.white)
                            Text("credits").font(.system(size: 14)).foregroundColor(.white.opacity(0.5))
                        }
                    }
                    .padding(16)
                    .frame(maxWidth: .infinity)
                    .glassCard()

                    // Quick tip buttons
                    VStack(spacing: 12) {
                        Text("Send a tip").font(.system(size: 15, weight: .semibold)).foregroundColor(.white.opacity(0.6))
                        HStack(spacing: 16) {
                            ForEach(quickTips, id: \.amount) { tip in
                                Button {
                                    tipManager.sendTip(amount: tip.amount, djDeviceId: djDeviceId)
                                } label: {
                                    VStack(spacing: 6) {
                                        Text(tip.emoji).font(.system(size: 28))
                                        Text(tip.label)
                                            .font(.system(size: 14, weight: .bold, design: .rounded))
                                            .foregroundColor(.white)
                                    }
                                    .frame(width: 80, height: 80)
                                    .background(
                                        RoundedRectangle(cornerRadius: 16)
                                            .fill(tipGradient(for: tip.amount))
                                    )
                                    .overlay(
                                        RoundedRectangle(cornerRadius: 16)
                                            .strokeBorder(Color.white.opacity(0.15), lineWidth: 0.5)
                                    )
                                    .shadow(color: tipShadow(for: tip.amount), radius: 8, y: 4)
                                }
                                .buttonStyle(.plain)
                            }
                        }
                    }
                    .padding(20)
                    .frame(maxWidth: .infinity)
                    .glassCard()

                    // Support DJ button (royalty contribution)
                    Button {
                        tipManager.sendSupport(amount: 500, djDeviceId: djDeviceId)
                    } label: {
                        HStack(spacing: 10) {
                            Image(systemName: "star.fill").font(.system(size: 16))
                            VStack(alignment: .leading, spacing: 2) {
                                Text("Support DJ").font(.system(size: 15, weight: .bold))
                                Text("Contribute to royalty costs (500 credits)")
                                    .font(.system(size: 11))
                                    .foregroundColor(.white.opacity(0.6))
                            }
                            Spacer()
                            Image(systemName: "chevron.right").font(.system(size: 12, weight: .semibold))
                                .foregroundColor(.white.opacity(0.4))
                        }
                        .foregroundColor(.white)
                        .padding(16)
                        .background(
                            RoundedRectangle(cornerRadius: 16)
                                .fill(LinearGradient(
                                    colors: [.solunaLuna.opacity(0.3), .solunaLunaEnd.opacity(0.2)],
                                    startPoint: .leading, endPoint: .trailing
                                ))
                        )
                        .overlay(
                            RoundedRectangle(cornerRadius: 16)
                                .strokeBorder(Color.solunaLuna.opacity(0.3), lineWidth: 0.5)
                        )
                    }
                    .buttonStyle(.plain)
                    .padding(.horizontal, 4)

                    // Close
                    Button { dismiss() } label: {
                        Text("Done").font(.system(size: 15, weight: .semibold)).foregroundColor(.white.opacity(0.5))
                            .padding(.vertical, 12).frame(maxWidth: .infinity)
                    }

                    Spacer().frame(height: 20)
                }
                .padding(.horizontal, 20)
            }
        }
        .preferredColorScheme(.dark)
    }

    // MARK: - Helpers

    private func tipGradient(for amount: Int) -> LinearGradient {
        switch amount {
        case 1000...:
            return LinearGradient(colors: [.solunaLuna, .solunaLunaEnd], startPoint: .topLeading, endPoint: .bottomTrailing)
        case 500...:
            return LinearGradient(colors: [.solunaSol, .solunaSolEnd], startPoint: .topLeading, endPoint: .bottomTrailing)
        default:
            return LinearGradient(colors: [.solunaSolEnd.opacity(0.6), .solunaMic.opacity(0.4)], startPoint: .topLeading, endPoint: .bottomTrailing)
        }
    }

    private func tipShadow(for amount: Int) -> Color {
        switch amount {
        case 1000...: return .solunaLuna.opacity(0.4)
        case 500...: return .solunaSol.opacity(0.4)
        default: return .solunaSolEnd.opacity(0.3)
        }
    }
}

// MARK: - Floating Tip Animation

struct FloatingTipView: View {
    let emoji: String
    let amount: Int
    @State private var offset: CGFloat = 0
    @State private var opacity: Double = 1.0
    @State private var scale: CGFloat = 0.5

    var body: some View {
        VStack(spacing: 2) {
            Text(emoji).font(.system(size: 36))
            Text("+\(amount)")
                .font(.system(size: 14, weight: .bold, design: .rounded))
                .foregroundColor(.white)
        }
        .scaleEffect(scale)
        .opacity(opacity)
        .offset(y: offset)
        .onAppear {
            withAnimation(.spring(response: 0.3, dampingFraction: 0.6)) {
                scale = 1.2
            }
            withAnimation(.easeOut(duration: 1.8)) {
                offset = -200
            }
            withAnimation(.easeIn(duration: 0.8).delay(1.0)) {
                opacity = 0
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) {
                withAnimation(.easeInOut(duration: 0.3)) {
                    scale = 1.0
                }
            }
        }
    }
}

#Preview {
    TippingView(
        djName: "DJ Soluna",
        djDeviceId: "test-device",
        tipManager: TipManager.shared
    )
}
