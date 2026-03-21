//
//  ReactionOverlay.swift
//  SolunaReceiver
//
//  Emoji reaction bombs — tap to burst, broadcast via relay TEXT:react protocol.
//

import SwiftUI

struct FloatingReaction: Identifiable {
    let id = UUID()
    let emoji: String
    let x: CGFloat       // 0.0 - 1.0 normalized
    let scale: CGFloat    // random size variation
    let drift: CGFloat    // horizontal wobble
}

@MainActor
class ReactionManager: ObservableObject {
    static let shared = ReactionManager()

    @Published var reactions: [FloatingReaction] = []

    func addLocal(emoji: String, x: CGFloat) {
        // Burst: add 3-5 of the same emoji with slight variation
        let count = Int.random(in: 3...5)
        for _ in 0..<count {
            let r = FloatingReaction(
                emoji: emoji,
                x: x + CGFloat.random(in: -0.08...0.08),
                scale: CGFloat.random(in: 0.7...1.4),
                drift: CGFloat.random(in: -30...30)
            )
            reactions.append(r)
            let rid = r.id
            DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) { [weak self] in
                self?.reactions.removeAll { $0.id == rid }
            }
        }
    }

    func handleRelay(_ text: String) {
        guard text.hasPrefix("TEXT:react ") else { return }
        let emoji = String(text.dropFirst("TEXT:react ".count)).trimmingCharacters(in: .whitespacesAndNewlines)
        guard !emoji.isEmpty else { return }
        addLocal(emoji: emoji, x: CGFloat.random(in: 0.15...0.85))
    }
}

struct ReactionOverlayView: View {
    @ObservedObject private var manager = ReactionManager.shared

    var body: some View {
        GeometryReader { geo in
            ForEach(manager.reactions) { r in
                ReactionBubble(emoji: r.emoji, scale: r.scale, drift: r.drift)
                    .position(x: r.x * geo.size.width, y: geo.size.height - 80)
            }
        }
        .allowsHitTesting(false)
        .ignoresSafeArea()
    }
}

// Each emoji: starts at bottom, floats up with wobble, scales up then fades
struct ReactionBubble: View {
    let emoji: String
    let scale: CGFloat
    let drift: CGFloat

    @State private var yOffset: CGFloat = 0
    @State private var xOffset: CGFloat = 0
    @State private var opacity: Double = 1
    @State private var currentScale: CGFloat = 0.3

    var body: some View {
        Text(emoji)
            .font(.system(size: 32 * scale))
            .scaleEffect(currentScale)
            .offset(x: xOffset, y: yOffset)
            .opacity(opacity)
            .onAppear {
                // Pop in
                withAnimation(.spring(response: 0.3, dampingFraction: 0.5)) {
                    currentScale = 1.0
                }
                // Float up with wobble
                withAnimation(.easeOut(duration: 2.2)) {
                    yOffset = CGFloat.random(in: -400 ... -250)
                    xOffset = drift
                }
                // Fade out in last second
                withAnimation(.easeIn(duration: 0.8).delay(1.4)) {
                    opacity = 0
                    currentScale = 0.5
                }
            }
    }
}

/// Horizontal bar of reaction emoji buttons
struct ReactionBar: View {
    let emojis = ["🔥", "❤️", "🎵", "✨", "🙌"]
    let sendUDP: (String) -> Void

    @State private var tappedEmoji: String?

    var body: some View {
        HStack(spacing: 14) {
            ForEach(emojis, id: \.self) { emoji in
                Button {
                    tappedEmoji = emoji
                    let x = CGFloat.random(in: 0.2...0.8)
                    ReactionManager.shared.addLocal(emoji: emoji, x: x)
                    sendUDP("TEXT:react \(emoji)\n")
                    // Haptic
                    UIImpactFeedbackGenerator(style: .medium).impactOccurred()
                    // Reset tap animation
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { tappedEmoji = nil }
                } label: {
                    Text(emoji)
                        .font(.system(size: 24))
                        .scaleEffect(tappedEmoji == emoji ? 1.5 : 1.0)
                        .animation(.spring(response: 0.2, dampingFraction: 0.4), value: tappedEmoji)
                }
                .buttonStyle(.plain)
            }
        }
    }
}
