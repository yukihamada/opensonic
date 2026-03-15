//
//  DJDeckView.swift
//  SolunaReceiver
//
//  Dual-deck DJ UI: two decks with progress rings, crossfader, and load buttons.
//

import SwiftUI
import UniformTypeIdentifiers

struct DJDeckView: View {
    @ObservedObject var receiver: AudioReceiver
    @Environment(\.dismiss) private var dismiss

    @State private var showPickerA = false
    @State private var showPickerB = false
    @State private var crossfader: Float = 0.5

    var body: some View {
        NavigationView {
            ZStack {
                Color.black.ignoresSafeArea()

                VStack(spacing: 20) {
                    // Title
                    Text("DJ Mode")
                        .font(.title2.bold())
                        .foregroundColor(.white)

                    // Decks
                    HStack(spacing: 12) {
                        DeckPanel(
                            label: "DECK A",
                            color: .blue,
                            trackName: receiver.deckATrack,
                            progress: receiver.deckAProgress,
                            isPlaying: receiver.deckAPlaying,
                            onLoad: { showPickerA = true },
                            onToggle: { receiver.toggleDeckA() }
                        )

                        DeckPanel(
                            label: "DECK B",
                            color: .purple,
                            trackName: receiver.deckBTrack,
                            progress: receiver.deckBProgress,
                            isPlaying: receiver.deckBPlaying,
                            onLoad: { showPickerB = true },
                            onToggle: { receiver.toggleDeckB() }
                        )
                    }

                    // Crossfader
                    VStack(spacing: 6) {
                        HStack {
                            Text("A").font(.caption.bold()).foregroundColor(.blue)
                            Spacer()
                            Text("CROSSFADER").font(.caption2).foregroundColor(.gray)
                            Spacer()
                            Text("B").font(.caption.bold()).foregroundColor(.purple)
                        }
                        Slider(value: Binding(
                            get: { Double(crossfader) },
                            set: { v in
                                crossfader = Float(v)
                                receiver.setCrossfader(crossfader)
                            }
                        ), in: 0...1)
                        .tint(.white)
                    }
                    .padding(.horizontal)

                    // Stop button
                    Button("Stop DJ") {
                        receiver.stopDualDeck()
                        dismiss()
                    }
                    .foregroundColor(.red)
                    .padding(.top, 4)
                }
                .padding()
            }
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") { dismiss() }
                        .foregroundColor(.white)
                }
            }
        }
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

// MARK: - DeckPanel

struct DeckPanel: View {
    let label: String
    let color: Color
    let trackName: String?
    let progress: Float
    let isPlaying: Bool
    let onLoad: () -> Void
    let onToggle: () -> Void

    var body: some View {
        VStack(spacing: 10) {
            Text(label)
                .font(.caption.bold())
                .foregroundColor(color)

            // Progress ring with play/pause icon
            ZStack {
                Circle()
                    .stroke(color.opacity(0.2), lineWidth: 6)
                Circle()
                    .trim(from: 0, to: CGFloat(progress))
                    .stroke(color, style: StrokeStyle(lineWidth: 6, lineCap: .round))
                    .rotationEffect(.degrees(-90))
                    .animation(.linear(duration: 0.4), value: progress)

                Image(systemName: isPlaying ? "pause.fill" : "play.fill")
                    .font(.title2)
                    .foregroundColor(isPlaying ? color : .gray)
                    .onTapGesture { onToggle() }
            }
            .frame(width: 100, height: 100)

            Text(trackName.map {
                URL(fileURLWithPath: $0).deletingPathExtension().lastPathComponent
            } ?? "No Track")
                .font(.caption2)
                .foregroundColor(.white.opacity(0.7))
                .lineLimit(2)
                .multilineTextAlignment(.center)
                .frame(maxWidth: .infinity)

            Button(action: onLoad) {
                Label("Load", systemImage: "folder.badge.plus")
                    .font(.caption)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 6)
                    .background(color.opacity(0.2))
                    .cornerRadius(8)
                    .foregroundColor(color)
            }
        }
        .padding(12)
        .background(Color.white.opacity(0.05))
        .cornerRadius(16)
    }
}
