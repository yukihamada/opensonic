//
//  ListenerMap.swift
//  Soluna — Listener Map visualization
//

import SwiftUI

struct ListenerMapView: View {
    let listenerCount: Int
    @State private var animating = false

    // Major city positions on a simplified world map (normalized 0-1)
    private static let cities: [(name: String, x: CGFloat, y: CGFloat)] = [
        ("Tokyo", 0.85, 0.38),
        ("Osaka", 0.83, 0.40),
        ("LA", 0.15, 0.38),
        ("NYC", 0.28, 0.35),
        ("London", 0.47, 0.28),
        ("Berlin", 0.52, 0.28),
        ("Sydney", 0.88, 0.70),
        ("Sao Paulo", 0.32, 0.62),
        ("Seoul", 0.82, 0.36),
        ("Bangkok", 0.75, 0.48),
    ]

    var body: some View {
        VStack(spacing: 6) {
            HStack {
                Image(systemName: "globe.americas.fill")
                    .font(.system(size: 11))
                    .foregroundColor(.solunaLuna)
                Text("Listeners Worldwide")
                    .font(.system(size: 11, weight: .medium))
                    .foregroundColor(.white.opacity(0.5))
                Spacer()
            }

            // Mini world map
            GeometryReader { geo in
                ZStack {
                    // Dark map background
                    RoundedRectangle(cornerRadius: 8)
                        .fill(Color.white.opacity(0.03))

                    // City dots with pulse
                    ForEach(0..<min(listenerCount + 1, Self.cities.count), id: \.self) { i in
                        let city = Self.cities[i]
                        Circle()
                            .fill(Color.solunaSol)
                            .frame(width: 6, height: 6)
                            .shadow(color: .solunaSol.opacity(0.8), radius: 4)
                            .position(x: city.x * geo.size.width, y: city.y * geo.size.height)
                            .opacity(animating ? 1.0 : 0.4)
                            .animation(
                                .easeInOut(duration: 1.5)
                                .repeatForever()
                                .delay(Double(i) * 0.2),
                                value: animating
                            )
                    }
                }
            }
            .frame(height: 60)
            .onAppear { animating = true }
        }
        .padding(10)
        .glassCard()
    }
}
