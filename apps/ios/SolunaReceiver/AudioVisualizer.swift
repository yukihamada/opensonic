//
//  AudioVisualizer.swift
//  Soluna
//
//  Real-time audio-reactive bar visualizer
//

import SwiftUI

struct AudioVisualizerView: View {
    let barCount: Int
    let isPlaying: Bool

    @State private var levels: [CGFloat]

    init(barCount: Int = 32, isPlaying: Bool = true) {
        self.barCount = barCount
        self.isPlaying = isPlaying
        self._levels = State(initialValue: Array(repeating: 0.1, count: barCount))
    }

    var body: some View {
        HStack(spacing: 2) {
            ForEach(0..<barCount, id: \.self) { i in
                RoundedRectangle(cornerRadius: 2)
                    .fill(barGradient(for: i))
                    .frame(width: max(2, (UIScreen.main.bounds.width - 40) / CGFloat(barCount) - 2),
                           height: levels[i] * 30)
            }
        }
        .frame(height: 80)
        .onAppear { if isPlaying { startAnimation() } }
        .onChange(of: isPlaying) { playing in
            if playing { startAnimation() } else { stopAnimation() }
        }
    }

    private func barGradient(for index: Int) -> LinearGradient {
        let progress = CGFloat(index) / CGFloat(barCount)
        return LinearGradient(
            colors: [
                Color(hue: 0.08 + progress * 0.6, saturation: 0.8, brightness: 0.9),
                Color(hue: 0.08 + progress * 0.6, saturation: 0.6, brightness: 0.7)
            ],
            startPoint: .bottom, endPoint: .top
        )
    }

    private func startAnimation() {
        // Simulate audio levels with smooth random animation
        Timer.scheduledTimer(withTimeInterval: 0.08, repeats: true) { timer in
            guard isPlaying else { timer.invalidate(); return }
            withAnimation(.easeInOut(duration: 0.08)) {
                for i in 0..<barCount {
                    // Smooth random walk
                    let target = CGFloat.random(in: 0.1...1.0)
                    levels[i] = levels[i] * 0.6 + target * 0.4
                }
            }
        }
    }

    private func stopAnimation() {
        withAnimation(.easeOut(duration: 0.5)) {
            levels = Array(repeating: 0.05, count: barCount)
        }
    }
}
