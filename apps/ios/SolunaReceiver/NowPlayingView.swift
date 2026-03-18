//
//  NowPlayingView.swift
//  SolunaReceiver
//
//  Fetches song identification data from the relay server and displays
//  "Now Playing" info including title, artist, YouTube link, and listener count.
//

import SwiftUI

// MARK: - Data Model

struct NowPlayingInfo: Codable {
    let title: String?
    let artist: String?
    let youtube_url: String?
    let artwork_url: String?
    let confidence: Double?
    let listeners: Int?
}

// MARK: - ViewModel

@MainActor
final class NowPlayingViewModel: ObservableObject {
    @Published var info: NowPlayingInfo?
    @Published var isIdentifying = true

    private var timer: Timer?
    private var channel: String = ""
    private let relayHost = "relay.solun.art"
    private static let cacheKey = "soluna_last_now_playing"

    func start(channel: String) {
        self.channel = channel
        isIdentifying = true
        // Restore cached metadata while waiting for fresh data
        if info == nil {
            info = Self.loadCachedInfo()
        }
        fetch()

        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 10, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in
                self?.fetch()
            }
        }
    }

    func stop() {
        timer?.invalidate()
        timer = nil
        info = nil
        isIdentifying = true
    }

    private func fetch() {
        guard !channel.isEmpty,
              let encoded = channel.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed),
              let url = URL(string: "https://\(relayHost)/api/now-playing?channel=\(encoded)")
        else { return }

        var request = URLRequest(url: url)
        request.timeoutInterval = 8

        URLSession.shared.dataTask(with: request) { [weak self] data, response, error in
            Task { @MainActor [weak self] in
                guard let self else { return }
                guard let data,
                      let status = (response as? HTTPURLResponse)?.statusCode,
                      status == 200 else {
                    self.isIdentifying = true
                    return
                }
                if let decoded = try? JSONDecoder().decode(NowPlayingInfo.self, from: data) {
                    self.info = decoded
                    self.isIdentifying = (decoded.title == nil)
                    if decoded.title != nil {
                        Self.cacheInfo(decoded)
                    }
                } else {
                    self.isIdentifying = true
                }
            }
        }.resume()
    }

    private static func cacheInfo(_ info: NowPlayingInfo) {
        if let data = try? JSONEncoder().encode(info) {
            UserDefaults.standard.set(data, forKey: cacheKey)
        }
    }

    private static func loadCachedInfo() -> NowPlayingInfo? {
        guard let data = UserDefaults.standard.data(forKey: cacheKey) else { return nil }
        return try? JSONDecoder().decode(NowPlayingInfo.self, from: data)
    }
}

// MARK: - View

struct NowPlayingView: View {
    @StateObject private var viewModel = NowPlayingViewModel()
    let channel: String
    let isReceiving: Bool

    var body: some View {
        Group {
            if isReceiving {
                cardContent
            }
        }
        .onChange(of: channel) { newChannel in
            if isReceiving {
                viewModel.start(channel: newChannel)
            }
        }
        .onChange(of: isReceiving) { receiving in
            if receiving {
                viewModel.start(channel: channel)
            } else {
                viewModel.stop()
            }
        }
        .onAppear {
            if isReceiving {
                viewModel.start(channel: channel)
            }
        }
        .onDisappear {
            viewModel.stop()
        }
    }

    @ViewBuilder
    private var cardContent: some View {
        if let info = viewModel.info, info.title != nil {
            identifiedCard(info)
        } else if viewModel.isIdentifying {
            identifyingCard
        }
    }

    // MARK: - Identified Song Card

    private func identifiedCard(_ info: NowPlayingInfo) -> some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                // Artwork or animated equalizer
                ZStack {
                    if let artworkStr = info.artwork_url, let artworkURL = URL(string: artworkStr) {
                        AsyncImage(url: artworkURL) { image in
                            image.resizable().scaledToFill()
                        } placeholder: {
                            equalizerIcon
                        }
                        .frame(width: 56, height: 56)
                        .clipShape(RoundedRectangle(cornerRadius: 10))
                    } else {
                        equalizerIcon
                    }
                }

                VStack(alignment: .leading, spacing: 4) {
                    Text(info.title ?? "")
                        .font(.subheadline.weight(.semibold))
                        .lineLimit(1)

                    if let artist = info.artist, !artist.isEmpty {
                        Text(artist)
                            .font(.caption)
                            .foregroundColor(.secondary)
                            .lineLimit(1)
                    }

                    HStack(spacing: 8) {
                        if let confidence = info.confidence {
                            HStack(spacing: 3) {
                                Image(systemName: "waveform")
                                    .font(.system(size: 9))
                                Text("\(Int(confidence * 100))%")
                                    .font(.system(size: 10, weight: .medium, design: .monospaced))
                            }
                            .foregroundColor(.green)
                        }

                        if let listeners = info.listeners, listeners > 0 {
                            HStack(spacing: 3) {
                                Image(systemName: "person.2.fill")
                                    .font(.system(size: 9))
                                Text("\(listeners)")
                                    .font(.system(size: 10, weight: .medium, design: .monospaced))
                            }
                            .foregroundColor(.secondary)
                        }
                    }
                }

                Spacer()

                if let ytStr = info.youtube_url, let ytURL = URL(string: ytStr) {
                    Link(destination: ytURL) {
                        Image(systemName: "play.circle.fill")
                            .font(.system(size: 28))
                            .foregroundColor(.red)
                    }
                }
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 12)
        }
        .background(Color(.secondarySystemGroupedBackground))
        .clipShape(RoundedRectangle(cornerRadius: 16))
    }

    // MARK: - Identifying State

    private var identifyingCard: some View {
        HStack(spacing: 10) {
            EqualizerBars(barCount: 4, isAnimating: true)
                .frame(width: 24, height: 20)

            Text("Identifying...")
                .font(.footnote.weight(.medium))
                .foregroundColor(.secondary)

            Spacer()
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(Color(UIColor.secondarySystemGroupedBackground))
        .clipShape(RoundedRectangle(cornerRadius: 12))
    }

    // MARK: - Equalizer Icon (static for artwork placeholder)

    private var equalizerIcon: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 10)
                .fill(Color(.tertiarySystemFill))
                .frame(width: 56, height: 56)
            EqualizerBars(barCount: 5, isAnimating: true)
                .frame(width: 28, height: 24)
        }
    }
}

// MARK: - Animated Equalizer Bars

struct EqualizerBars: View {
    let barCount: Int
    let isAnimating: Bool

    @State private var phases: [CGFloat] = []

    var body: some View {
        HStack(alignment: .bottom, spacing: 2) {
            ForEach(0..<barCount, id: \.self) { i in
                let height = isAnimating
                    ? (i < phases.count ? phases[i] : 0.3)
                    : 0.15
                RoundedRectangle(cornerRadius: 1.5)
                    .fill(
                        LinearGradient(
                            colors: [.blue, .cyan],
                            startPoint: .bottom,
                            endPoint: .top
                        )
                    )
                    .frame(maxWidth: .infinity)
                    .scaleEffect(y: height, anchor: .bottom)
                    .animation(
                        .easeInOut(duration: Double.random(in: 0.3...0.6))
                        .repeatForever(autoreverses: true)
                        .delay(Double(i) * 0.1),
                        value: phases
                    )
            }
        }
        .onAppear {
            phases = (0..<barCount).map { _ in CGFloat.random(in: 0.3...1.0) }
            // Trigger animation by changing phases
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
                phases = (0..<barCount).map { _ in CGFloat.random(in: 0.3...1.0) }
            }
        }
    }
}

// MARK: - Preview

#Preview {
    VStack(spacing: 16) {
        NowPlayingView(channel: "test", isReceiving: true)
    }
    .padding()
    .background(Color(UIColor.systemGroupedBackground))
}
