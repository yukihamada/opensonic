//
//  SolunaTheme.swift
//  SolunaReceiverMac
//
//  Modern design system — 2025 glass morphism + gradient accents (macOS)
//

import SwiftUI

// MARK: - Brand Colors

extension Color {
    static let solunaGradientStart = Color(red: 0.42, green: 0.22, blue: 0.88)
    static let solunaGradientMid   = Color(red: 0.28, green: 0.40, blue: 0.95)
    static let solunaGradientEnd   = Color(red: 0.15, green: 0.75, blue: 0.85)
    static let solunaLive          = Color(red: 0.20, green: 0.88, blue: 0.55)
    static let solunaSurface       = Color(red: 0.08, green: 0.08, blue: 0.12)
    static let solunaSurface2      = Color(red: 0.12, green: 0.12, blue: 0.17)
    static let solunaMic           = Color(red: 0.95, green: 0.30, blue: 0.35)
}

// MARK: - Gradient Presets

extension LinearGradient {
    static let solunaAccent = LinearGradient(
        colors: [.solunaGradientStart, .solunaGradientMid, .solunaGradientEnd],
        startPoint: .topLeading, endPoint: .bottomTrailing
    )
    static let solunaSubtle = LinearGradient(
        colors: [
            Color.solunaGradientStart.opacity(0.15),
            Color.solunaGradientEnd.opacity(0.08)
        ],
        startPoint: .topLeading, endPoint: .bottomTrailing
    )
    static let solunaBg = LinearGradient(
        colors: [
            Color.solunaSurface,
            Color(red: 0.06, green: 0.06, blue: 0.10)
        ],
        startPoint: .top, endPoint: .bottom
    )
}

// MARK: - Glass Card Modifier

struct GlassCard: ViewModifier {
    var cornerRadius: CGFloat = 16
    var opacity: Double = 0.08

    func body(content: Content) -> some View {
        content
            .background(
                RoundedRectangle(cornerRadius: cornerRadius)
                    .fill(.ultraThinMaterial)
                    .overlay(
                        RoundedRectangle(cornerRadius: cornerRadius)
                            .stroke(Color.white.opacity(0.08), lineWidth: 0.5)
                    )
            )
    }
}

extension View {
    func glassCard(cornerRadius: CGFloat = 16) -> some View {
        modifier(GlassCard(cornerRadius: cornerRadius))
    }
}

// MARK: - Animated Gradient Ring

struct GradientRing: View {
    let isActive: Bool
    @State private var rotation: Double = 0

    var body: some View {
        Circle()
            .stroke(
                AngularGradient(
                    colors: isActive
                        ? [.solunaGradientStart, .solunaGradientMid, .solunaGradientEnd, .solunaGradientStart]
                        : [Color.gray.opacity(0.3), Color.gray.opacity(0.15), Color.gray.opacity(0.3)],
                    center: .center,
                    startAngle: .degrees(rotation),
                    endAngle: .degrees(rotation + 360)
                ),
                lineWidth: 2.5
            )
            .onAppear {
                guard isActive else { return }
                withAnimation(.linear(duration: 3).repeatForever(autoreverses: false)) {
                    rotation = 360
                }
            }
            .onChange(of: isActive) { newValue in
                let active = newValue
                if active {
                    rotation = 0
                    withAnimation(.linear(duration: 3).repeatForever(autoreverses: false)) {
                        rotation = 360
                    }
                }
            }
    }
}

// MARK: - Waveform Visualizer (modern)

struct WaveformVisualizer: View {
    let level: Float
    let barCount: Int = 32

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 30.0)) { timeline in
            Canvas { context, size in
                let w = size.width / CGFloat(barCount)
                let gap: CGFloat = 2
                let time = timeline.date.timeIntervalSince1970

                for i in 0..<barCount {
                    let fi = Double(i)
                    let wave = sin(fi * 0.5 + time * 4.0) * 0.3 + 0.5
                    let h = max(3, CGFloat(level) * size.height * CGFloat(wave))
                    let x = CGFloat(i) * w + gap / 2
                    let y = (size.height - h) / 2

                    let rect = CGRect(x: x, y: y, width: w - gap, height: h)
                    let path = Path(roundedRect: rect, cornerRadius: (w - gap) / 2)

                    let progress = CGFloat(i) / CGFloat(barCount)
                    let color = Color(
                        red: 0.42 * (1 - progress) + 0.15 * progress,
                        green: 0.22 * (1 - progress) + 0.75 * progress,
                        blue: 0.88 * (1 - progress) + 0.85 * progress
                    )
                    context.fill(path, with: .color(color.opacity(0.8)))
                }
            }
        }
    }
}

// MARK: - Pill Button Style

struct PillButtonStyle: ButtonStyle {
    let color: Color
    let isActive: Bool

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 12, weight: .semibold))
            .foregroundColor(isActive ? .white : color)
            .padding(.horizontal, 12)
            .padding(.vertical, 6)
            .background(color.opacity(isActive ? 1.0 : 0.12))
            .clipShape(Capsule())
            .scaleEffect(configuration.isPressed ? 0.95 : 1.0)
            .animation(.spring(response: 0.2), value: configuration.isPressed)
    }
}
