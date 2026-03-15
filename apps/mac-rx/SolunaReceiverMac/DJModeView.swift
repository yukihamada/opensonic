//
//  DJModeView.swift
//  SolunaReceiverMac
//
//  Dual-deck DJ mode: Deck A | Crossfader | Deck B with equal-power mixing.
//

import SwiftUI
import UniformTypeIdentifiers

struct DJModeView: View {
    @ObservedObject var receiver: AudioReceiver

    @State private var showPickerA = false
    @State private var showPickerB = false
    @State private var crossfader: Float = 0.5

    var body: some View {
        HStack(alignment: .top, spacing: 24) {
            // Deck A
            MacDeckPanel(
                label: "DECK A",
                color: .blue,
                trackName: receiver.deckATrack,
                progress: receiver.deckAProgress,
                isPlaying: receiver.deckAPlaying,
                onLoad: { showPickerA = true },
                onToggle: { receiver.toggleDeckA() }
            )

            // Crossfader (vertical, center)
            VStack(spacing: 12) {
                Spacer()
                Text("XFADER")
                    .font(.caption2.bold())
                    .foregroundColor(.secondary)

                Slider(value: Binding(
                    get: { Double(crossfader) },
                    set: { v in
                        crossfader = Float(v)
                        receiver.setCrossfader(crossfader)
                    }
                ), in: 0...1)
                .rotationEffect(.degrees(-90))
                .frame(width: 120, height: 40)

                HStack {
                    Text("A").font(.caption.bold()).foregroundColor(.blue)
                    Spacer()
                    Text("B").font(.caption.bold()).foregroundColor(.purple)
                }
                .frame(width: 120)

                Spacer()

                Button(role: .destructive) {
                    receiver.stopDualDeck()
                } label: {
                    Label("Stop", systemImage: "stop.fill")
                        .font(.caption)
                }
                .buttonStyle(.borderedProminent)
                .tint(.red.opacity(0.8))
            }
            .frame(width: 130)

            // Deck B
            MacDeckPanel(
                label: "DECK B",
                color: .purple,
                trackName: receiver.deckBTrack,
                progress: receiver.deckBProgress,
                isPlaying: receiver.deckBPlaying,
                onLoad: { showPickerB = true },
                onToggle: { receiver.toggleDeckB() }
            )
        }
        .padding()
        .fileImporter(isPresented: $showPickerA,
                      allowedContentTypes: [.audio],
                      allowsMultipleSelection: false) { result in
            if let url = try? result.get().first {
                receiver.startDeckA(url: url)
            }
        }
        .fileImporter(isPresented: $showPickerB,
                      allowedContentTypes: [.audio],
                      allowsMultipleSelection: false) { result in
            if let url = try? result.get().first {
                receiver.startDeckB(url: url)
            }
        }
    }
}

// MARK: - MacDeckPanel

struct MacDeckPanel: View {
    let label: String
    let color: Color
    let trackName: String?
    let progress: Float
    let isPlaying: Bool
    let onLoad: () -> Void
    let onToggle: () -> Void

    var body: some View {
        VStack(spacing: 12) {
            Text(label)
                .font(.headline.bold())
                .foregroundColor(color)

            // Progress ring
            ZStack {
                Circle()
                    .stroke(color.opacity(0.15), lineWidth: 8)
                Circle()
                    .trim(from: 0, to: CGFloat(progress))
                    .stroke(color, style: StrokeStyle(lineWidth: 8, lineCap: .round))
                    .rotationEffect(.degrees(-90))
                    .animation(.linear(duration: 0.4), value: progress)

                Button(action: onToggle) {
                    Image(systemName: isPlaying ? "pause.fill" : "play.fill")
                        .font(.largeTitle)
                        .foregroundColor(isPlaying ? color : .secondary)
                }
                .buttonStyle(.plain)
            }
            .frame(width: 140, height: 140)

            // Track name
            Text(trackName.map {
                URL(fileURLWithPath: $0).deletingPathExtension().lastPathComponent
            } ?? "No Track Loaded")
                .font(.caption)
                .foregroundColor(.secondary)
                .lineLimit(2)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 180)

            // Progress percentage
            Text("\(Int(progress * 100))%")
                .font(.caption.monospacedDigit())
                .foregroundColor(color.opacity(0.8))

            ProgressView(value: Double(progress))
                .tint(color)
                .frame(maxWidth: 180)
                .animation(.linear(duration: 0.4), value: progress)

            Button(action: onLoad) {
                Label("Load Track", systemImage: "folder.badge.plus")
                    .font(.caption)
            }
            .buttonStyle(.bordered)
            .tint(color)
        }
        .padding(16)
        .background(color.opacity(0.05))
        .cornerRadius(16)
        .overlay(
            RoundedRectangle(cornerRadius: 16)
                .stroke(color.opacity(0.2), lineWidth: 1)
        )
    }
}
