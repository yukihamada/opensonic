//
//  PlayerView.swift
//  Soluna
//
//  Music player UI: drag-and-drop / file picker → upload to daemon
//  → auto-switch from PCM stream to local AVPlayer playback.
//  Includes queue list, upload progress, AirPlay button, prev/next controls.
//

import SwiftUI
import UniformTypeIdentifiers
import AVKit

struct PlayerView: View {
    @ObservedObject var model: PlayerModel
    @State private var showPicker = false
    @State private var isDragging = false

    var body: some View {
        ScrollView {
            VStack(spacing: 24) {
                artAndSpectrum
                trackInfo
                phaseBar
                uploadProgressBar
                seekBar
                transportControls
                queueList
                dropZone
            }
            .padding(.horizontal, 20)
            .padding(.top, 12)
            .padding(.bottom, 40)
        }
        .background(Color(.systemGroupedBackground).ignoresSafeArea())
        .fileImporter(
            isPresented: $showPicker,
            allowedContentTypes: [.mp3, .wav, .flac, .aiff, .audio],
            allowsMultipleSelection: true
        ) { result in
            guard let urls = try? result.get() else { return }
            for url in urls { loadFile(url: url) }
        }
        .alert("File Too Large", isPresented: .init(
            get: { model.errorMessage != nil },
            set: { if !$0 { model.errorMessage = nil } }
        )) {
            Button("OK") { model.errorMessage = nil }
        } message: {
            Text(model.errorMessage ?? "")
        }
    }

    // MARK: - Art + Spectrum

    private var artAndSpectrum: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 20)
                .fill(Color(.secondarySystemGroupedBackground))
                .frame(height: 200)
                .shadow(color: .black.opacity(0.08), radius: 12, y: 4)

            VStack(spacing: 0) {
                // Artwork overlay (if available)
                if let artwork = model.currentItem?.artwork {
                    ZStack {
                        Image(uiImage: artwork)
                            .resizable()
                            .aspectRatio(contentMode: .fill)
                            .frame(height: 200)
                            .clipped()
                            .opacity(0.25)
                        spectrumView
                            .padding(.horizontal, 16)
                            .padding(.vertical, 12)
                    }
                } else {
                    spectrumView
                        .padding(.horizontal, 16)
                        .padding(.vertical, 12)
                }

                // Format badge + AirPlay
                HStack {
                    if !model.trackFmt.isEmpty {
                        Text(model.trackFmt)
                            .font(.caption2.weight(.semibold))
                            .foregroundColor(.white)
                            .padding(.horizontal, 8)
                            .padding(.vertical, 3)
                            .background(badgeColor(for: model.trackFmt))
                            .clipShape(Capsule())
                            .padding(.leading, 14)
                    }
                    Spacer()
                    AirPlayButton()
                        .frame(width: 32, height: 32)
                        .padding(.trailing, 14)
                }
                .padding(.bottom, 10)
            }
        }
    }

    private func badgeColor(for fmt: String) -> Color {
        switch fmt {
        case "MP3":  return .blue
        case "FLAC": return .purple
        case "AIFF": return .teal
        case "ALAC": return .indigo
        default:     return .green
        }
    }

    private var spectrumView: some View {
        HStack(alignment: .bottom, spacing: 3) {
            ForEach(Array(model.spectrum.enumerated()), id: \.offset) { i, v in
                let t = Double(i) / Double(max(1, model.spectrum.count - 1))
                RoundedRectangle(cornerRadius: 3)
                    .fill(barColor(t: t))
                    .frame(maxWidth: .infinity)
                    .frame(height: max(4, CGFloat(v) * 140))
                    .animation(.easeOut(duration: 0.08), value: v)
            }
        }
        .frame(height: 140)
    }

    private func barColor(t: Double) -> Color {
        Color(hue: 0.55 + t * 0.25, saturation: 0.75, brightness: 0.85)
    }

    // MARK: - Track info

    private var trackInfo: some View {
        VStack(spacing: 4) {
            let title  = model.currentItem?.title ?? model.trackName
            let artist = model.currentItem?.artist ?? ""

            Text(title.isEmpty ? "No Track Selected" : title)
                .font(.headline)
                .lineLimit(1)
                .truncationMode(.middle)
                .foregroundColor(title.isEmpty ? .secondary : .primary)

            if !artist.isEmpty {
                Text(artist)
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .lineLimit(1)
            }

            HStack(spacing: 6) {
                Image(systemName: model.phase.icon)
                    .font(.caption)
                    .foregroundColor(.secondary)
                Text(model.phase.label)
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
            .animation(.easeInOut(duration: 0.3), value: model.phase)
        }
    }

    // MARK: - Phase / transfer progress bar

    @ViewBuilder
    private var phaseBar: some View {
        if model.phase == .transferring {
            VStack(spacing: 6) {
                HStack {
                    Text("Receiving file")
                        .font(.caption)
                        .foregroundColor(.secondary)
                    Spacer()
                    Text("\(Int(model.transferPct * 100))%")
                        .font(.caption.monospacedDigit())
                        .foregroundColor(.secondary)
                }
                ProgressView(value: model.transferPct)
                    .tint(.blue)
                    .animation(.linear(duration: 0.1), value: model.transferPct)
            }
            .padding(.horizontal, 4)
        }
    }

    // MARK: - Upload progress bar

    @ViewBuilder
    private var uploadProgressBar: some View {
        if model.uploadProgress > 0 && model.uploadProgress < 1 {
            VStack(spacing: 6) {
                HStack {
                    Text("Uploading")
                        .font(.caption)
                        .foregroundColor(.secondary)
                    Spacer()
                    Text("\(Int(model.uploadProgress * 100))%")
                        .font(.caption.monospacedDigit())
                        .foregroundColor(.secondary)
                }
                ProgressView(value: model.uploadProgress)
                    .tint(.orange)
                    .animation(.linear(duration: 0.1), value: model.uploadProgress)
            }
            .padding(.horizontal, 4)
        }
    }

    // MARK: - Seek bar

    private var seekBar: some View {
        VStack(spacing: 6) {
            Slider(
                value: Binding(
                    get: { model.durationMs > 0 ? Double(model.positionMs) / Double(model.durationMs) : 0 },
                    set: { model.seek(fraction: $0) }
                )
            )
            .accentColor(.primary)
            .disabled(model.durationMs == 0)

            HStack {
                Text(formatMs(model.positionMs))
                Spacer()
                Text(formatMs(model.durationMs))
            }
            .font(.caption.monospacedDigit())
            .foregroundColor(.secondary)
        }
    }

    // MARK: - Transport controls

    private var transportControls: some View {
        HStack(spacing: 28) {
            // Previous track (shown when queue has multiple items)
            if model.queue.count > 1 {
                Button {
                    model.previousTrack()
                } label: {
                    Image(systemName: "backward.fill")
                        .font(.title2)
                        .foregroundColor(model.currentIndex > 0 ? .primary : .secondary)
                }
                .disabled(model.currentIndex == 0)
            }

            Button {
                model.seek(fraction: 0)
            } label: {
                Image(systemName: "backward.end.fill")
                    .font(.title2)
                    .foregroundColor(.primary)
            }
            .disabled(model.phase == .idle)

            // Play / Pause
            Button {
                if model.phase == .localPlaying || model.phase == .streaming {
                    model.pause()
                } else {
                    model.play()
                }
            } label: {
                ZStack {
                    Circle()
                        .fill(Color.primary)
                        .frame(width: 64, height: 64)
                    Image(systemName: model.phase == .localPlaying || model.phase == .streaming
                          ? "pause.fill" : "play.fill")
                        .font(.title2)
                        .foregroundColor(Color(.systemBackground))
                        .offset(x: model.phase == .idle ? 2 : 0)
                }
            }
            .disabled(model.phase == .idle || model.phase == .transferring || model.phase == .switching)

            Button { model.stop() } label: {
                Image(systemName: "stop.fill")
                    .font(.title2)
                    .foregroundColor(.primary)
            }
            .disabled(model.phase == .idle)

            // Next track (shown when queue has multiple items)
            if model.queue.count > 1 {
                Button {
                    model.nextTrack()
                } label: {
                    Image(systemName: "forward.fill")
                        .font(.title2)
                        .foregroundColor(model.currentIndex + 1 < model.queue.count ? .primary : .secondary)
                }
                .disabled(model.currentIndex + 1 >= model.queue.count)
            }
        }
        .padding(.vertical, 4)
    }

    // MARK: - Queue list

    @ViewBuilder
    private var queueList: some View {
        if !model.queue.isEmpty {
            VStack(spacing: 0) {
                HStack {
                    Text("Queue")
                        .font(.subheadline.weight(.semibold))
                        .foregroundColor(.secondary)
                    Spacer()
                    Text("\(model.queue.count) track\(model.queue.count == 1 ? "" : "s")")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                .padding(.horizontal, 4)
                .padding(.bottom, 8)

                VStack(spacing: 2) {
                    ForEach(Array(model.queue.enumerated()), id: \.element.id) { idx, item in
                        let isCurrent = idx == model.currentIndex
                        Button {
                            model.jumpTo(index: idx)
                        } label: {
                            HStack(spacing: 12) {
                                // Playing indicator or index
                                ZStack {
                                    if isCurrent && model.phase.isPlaying {
                                        Image(systemName: "waveform")
                                            .font(.caption.weight(.semibold))
                                            .foregroundColor(.blue)
                                    } else {
                                        Text("\(idx + 1)")
                                            .font(.caption.monospacedDigit())
                                            .foregroundColor(isCurrent ? .blue : .secondary)
                                    }
                                }
                                .frame(width: 20)

                                // Artwork thumbnail
                                if let art = item.artwork {
                                    Image(uiImage: art)
                                        .resizable()
                                        .aspectRatio(contentMode: .fill)
                                        .frame(width: 36, height: 36)
                                        .clipShape(RoundedRectangle(cornerRadius: 6))
                                } else {
                                    RoundedRectangle(cornerRadius: 6)
                                        .fill(Color(.tertiarySystemFill))
                                        .frame(width: 36, height: 36)
                                        .overlay(
                                            Image(systemName: "music.note")
                                                .font(.caption)
                                                .foregroundColor(.secondary)
                                        )
                                }

                                // Title + artist
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(item.title.isEmpty ? item.name : item.title)
                                        .font(.subheadline)
                                        .fontWeight(isCurrent ? .semibold : .regular)
                                        .foregroundColor(isCurrent ? .primary : .primary)
                                        .lineLimit(1)
                                    if !item.artist.isEmpty {
                                        Text(item.artist)
                                            .font(.caption)
                                            .foregroundColor(.secondary)
                                            .lineLimit(1)
                                    }
                                }

                                Spacer()

                                // Delete button
                                Button {
                                    withAnimation {
                                        model.queue.remove(at: idx)
                                        if model.currentIndex >= model.queue.count && !model.queue.isEmpty {
                                            model.queue.indices.last.map { _ in () }
                                        }
                                    }
                                } label: {
                                    Image(systemName: "xmark.circle.fill")
                                        .font(.body)
                                        .foregroundColor(.secondary.opacity(0.6))
                                }
                                .buttonStyle(.plain)
                            }
                            .padding(.horizontal, 12)
                            .padding(.vertical, 8)
                            .background(
                                RoundedRectangle(cornerRadius: 10)
                                    .fill(isCurrent ? Color.blue.opacity(0.08) : Color(.secondarySystemGroupedBackground))
                            )
                        }
                        .buttonStyle(.plain)
                    }
                }
            }
        }
    }

    // MARK: - Drop zone / file picker

    private var dropZone: some View {
        Button {
            showPicker = true
        } label: {
            VStack(spacing: 10) {
                Image(systemName: isDragging ? "arrow.down.circle.fill" : "plus.circle")
                    .font(.system(size: 36))
                    .foregroundColor(.blue)
                Text(isDragging ? "Drop to Upload" : "Add Track")
                    .font(.subheadline.weight(.medium))
                    .foregroundColor(.blue)
                Text("MP3 · WAV · FLAC · AIFF")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 28)
            .background(
                RoundedRectangle(cornerRadius: 16)
                    .strokeBorder(
                        isDragging ? Color.blue : Color(.separator),
                        style: StrokeStyle(lineWidth: 2, dash: isDragging ? [] : [6, 4])
                    )
                    .background(
                        RoundedRectangle(cornerRadius: 16)
                            .fill(isDragging ? Color.blue.opacity(0.06) : Color.clear)
                    )
            )
        }
        .buttonStyle(.plain)
        .modifier(DropDestinationModifier { urls in
            for url in urls { loadFile(url: url) }
        })
    }

    // MARK: - Helpers

    private func loadFile(url: URL) {
        guard url.startAccessingSecurityScopedResource() else { return }
        defer { url.stopAccessingSecurityScopedResource() }
        guard let data = try? Data(contentsOf: url) else { return }
        model.enqueue(data, name: url.lastPathComponent)
    }

    private func formatMs(_ ms: UInt64) -> String {
        let s  = Int(ms / 1000)
        let m  = s / 60
        let ss = s % 60
        return String(format: "%d:%02d", m, ss)
    }
}

// MARK: - AirPlay button (iOS)

struct AirPlayButton: UIViewRepresentable {
    func makeUIView(context: Context) -> AVRoutePickerView {
        let view = AVRoutePickerView()
        view.tintColor = UIColor.secondaryLabel
        view.activeTintColor = UIColor.systemBlue
        return view
    }
    func updateUIView(_ uiView: AVRoutePickerView, context: Context) {}
}

// MARK: - UTType convenience

private extension UTType {
    static let mp3  = UTType("public.mp3") ?? .audio
    static let wav  = UTType("com.microsoft.waveform-audio") ?? .audio
    static let flac = UTType("org.xiph.flac") ?? .audio
    static let aiff = UTType("public.aiff-audio") ?? .audio
}

// MARK: - Drop Destination (iOS 16+)

private struct DropDestinationModifier: ViewModifier {
    let handler: ([URL]) -> Void
    func body(content: Content) -> some View {
        if #available(iOS 16.0, *) {
            content.dropDestination(for: URL.self) { urls, _ in
                handler(urls)
                return !urls.isEmpty
            }
        } else {
            content
        }
    }
}
