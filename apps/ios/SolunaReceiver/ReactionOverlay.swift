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
    let x: CGFloat
    let startTime = Date()
}

@MainActor
class ReactionManager: ObservableObject {
    static let shared = ReactionManager()

    @Published var reactions: [FloatingReaction] = []

    func addLocal(emoji: String, x: CGFloat) {
        let r = FloatingReaction(emoji: emoji, x: x)
        reactions.append(r)
        // Auto-remove after 2 seconds
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
            self?.reactions.removeAll { $0.id == r.id }
        }
    }

    /// Parse incoming TEXT:react messages from the relay.
    /// Format: "TEXT:react <emoji>\n"
    func handleRelay(_ text: String) {
        guard text.hasPrefix("TEXT:react ") else { return }
        let emoji = String(text.dropFirst("TEXT:react ".count)).trimmingCharacters(in: .whitespacesAndNewlines)
        guard !emoji.isEmpty else { return }
        let x = CGFloat.random(in: 0.1...0.9)
        addLocal(emoji: emoji, x: x)
    }
}

struct ReactionOverlayView: View {
    @ObservedObject var manager = ReactionManager.shared
    let screenWidth: CGFloat

    var body: some View {
        ZStack {
            ForEach(manager.reactions) { r in
                Text(r.emoji)
                    .font(.system(size: 36))
                    .position(x: r.x * screenWidth, y: 0)
                    .transition(.asymmetric(
                        insertion: .scale(scale: 0.5).combined(with: .opacity),
                        removal: .opacity
                    ))
                    .modifier(FloatUp(duration: 2.0))
            }
        }
        .allowsHitTesting(false)
    }
}

// Animated floating modifier
struct FloatUp: ViewModifier {
    let duration: Double
    @State private var offset: CGFloat = 0
    @State private var opacity: Double = 1

    func body(content: Content) -> some View {
        content
            .offset(y: offset)
            .opacity(opacity)
            .onAppear {
                withAnimation(.easeOut(duration: duration)) {
                    offset = -300
                    opacity = 0
                }
            }
    }
}

/// Horizontal bar of reaction emoji buttons
struct ReactionBar: View {
    let emojis = ["🔥", "❤️", "🎵", "✨", "🙌"]
    let sendUDP: (String) -> Void

    var body: some View {
        HStack(spacing: 12) {
            ForEach(emojis, id: \.self) { emoji in
                Button {
                    // Show locally + broadcast
                    let x = CGFloat.random(in: 0.2...0.8)
                    ReactionManager.shared.addLocal(emoji: emoji, x: x)
                    sendUDP("TEXT:react \(emoji)\n")
                } label: {
                    Text(emoji)
                        .font(.system(size: 22))
                        .frame(width: 36, height: 36)
                        .background(Color.white.opacity(0.08))
                        .clipShape(Circle())
                }
            }
        }
    }
}
