//
//  PlayerView.swift
//  SolunaReceiverMac
//
//  Music player UI: drag-and-drop / file picker → upload to daemon
//  → auto-switch from PCM stream to local AVPlayer playback.
//  Includes queue list, upload progress, and prev/next controls.
//

import SwiftUI
import UniformTypeIdentifiers
import AppKit

struct PlayerView: View {
    @ObservedObject var model: PlayerModel
    @State private var isDragging = false

    var body: some View {
        ScrollView {
            VStack(spacing: 20) {
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
            .padding(.bottom, 32)
        }
        .background(Color(nsColor: .windowBackgroundColor).ignoresSafeArea())
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
            RoundedRectangle(cornerRadius: 16)
                .fill(Color(nsColor: .controlBackgroundColor))
                .frame(height: 180)
                .shadow(color: .black.opacity(0.08), radius: 10, y: 3)

            VStack(spacing: 0) {
                // Artwork overlay (if available)
                if let artwork = model.currentItem?.artwork {
                    ZStack {
                        Image(nsImage: artwork)
                            .resizable()
                            .aspectRatio(contentMode: .fill)
                            .frame(height: 180)
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

                // Format badge
                if !model.trackFmt.isEmpty {
                    HStack {
                        Spacer()
                        Text(model.trackFmt)
                            .font(.caption2.weight(.semibold))
                            .foregroundColor(.white)
                            .padding(.horizontal, 8)
                            .padding(.vertical, 3)
                            .background(badgeColor(for: model.trackFmt))
                            .clipShape(Capsule())
                            .padding(.trailing, 14)
                            .padding(.bottom, 10)
                    }
                }
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
                    .frame(height: max(4, CGFloat(v) * 120))
                    .animation(.easeOut(duration: 0.08), value: v)
            }
        }
        .frame(height: 120)
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
        HStack(spacing: 20) {
            // Previous track (shown when queue has multiple items)
            if model.queue.count > 1 {
                Button {
                    model.previousTrack()
                } label: {
                    Image(systemName: "backward.fill")
                        .font(.title3)
                        .foregroundColor(model.currentIndex > 0 ? .primary : .secondary)
                }
                .buttonStyle(.plain)
                .disabled(model.currentIndex == 0)
            }

            Button {
                model.seek(fraction: 0)
            } label: {
                Image(systemName: "backward.end.fill")
                    .font(.title3)
                    .foregroundColor(.primary)
            }
            .buttonStyle(.plain)
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
                        .frame(width: 52, height: 52)
                    Image(systemName: model.phase == .localPlaying || model.phase == .streaming
                          ? "pause.fill" : "play.fill")
                        .font(.title3)
                        .foregroundColor(Color(nsColor: .windowBackgroundColor))
                        .offset(x: model.phase == .idle ? 2 : 0)
                }
            }
            .buttonStyle(.plain)
            .disabled(model.phase == .idle || model.phase == .transferring || model.phase == .switching)

            Button { model.stop() } label: {
                Image(systemName: "stop.fill")
                    .font(.title3)
                    .foregroundColor(.primary)
            }
            .buttonStyle(.plain)
            .disabled(model.phase == .idle)

            // Next track (shown when queue has multiple items)
            if model.queue.count > 1 {
                Button {
                    model.nextTrack()
                } label: {
                    Image(systemName: "forward.fill")
                        .font(.title3)
                        .foregroundColor(model.currentIndex + 1 < model.queue.count ? .primary : .secondary)
                }
                .buttonStyle(.plain)
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
                        queueRow(idx: idx, item: item)
                    }
                }
            }
        }
    }

    private func queueRow(idx: Int, item: QueueItem) -> some View {
        let isCurrent: Bool = idx == model.currentIndex
        let bgColor: Color = isCurrent ? Color.blue.opacity(0.08) : Color(nsColor: .controlBackgroundColor)
        let titleText: String = item.title.isEmpty ? item.name : item.title
        return Button {
            model.jumpTo(index: idx)
        } label: {
            HStack(spacing: 12) {
                Text(isCurrent && model.phase.isPlaying ? "♪" : "\(idx + 1)")
                    .font(.caption.monospacedDigit())
                    .foregroundColor(isCurrent ? .blue : .secondary)
                    .frame(width: 20)

                Text(titleText)
                    .font(.subheadline)
                    .fontWeight(isCurrent ? .semibold : .regular)
                    .foregroundColor(.primary)
                    .lineLimit(1)

                Spacer()

                Button {
                    model.queue.remove(at: idx)
                } label: {
                    Image(systemName: "xmark.circle.fill")
                        .font(.body)
                        .foregroundColor(.secondary.opacity(0.6))
                }
                .buttonStyle(.plain)
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 7)
            .background(RoundedRectangle(cornerRadius: 8).fill(bgColor))
        }
        .buttonStyle(.plain)
    }

    // MARK: - Drop zone / file picker

    private var dropZone: some View {
        Button {
            openFilePicker()
        } label: {
            VStack(spacing: 10) {
                Image(systemName: isDragging ? "arrow.down.circle.fill" : "plus.circle")
                    .font(.system(size: 32))
                    .foregroundColor(.blue)
                Text(isDragging ? "Drop to Upload" : "Add Track")
                    .font(.subheadline.weight(.medium))
                    .foregroundColor(.blue)
                Text("MP3 · WAV · FLAC · AIFF")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 24)
            .background(
                RoundedRectangle(cornerRadius: 12)
                    .strokeBorder(
                        isDragging ? Color.blue : Color(nsColor: .separatorColor),
                        style: StrokeStyle(lineWidth: 2, dash: isDragging ? [] : [6, 4])
                    )
                    .background(
                        RoundedRectangle(cornerRadius: 12)
                            .fill(isDragging ? Color.blue.opacity(0.06) : Color.clear)
                    )
            )
        }
        .buttonStyle(.plain)
        .dropDestination(for: URL.self) { urls, _ in
            for url in urls { loadFile(url: url) }
            return !urls.isEmpty
        } isTargeted: { isDragging = $0 }
    }

    // MARK: - Helpers

    private func openFilePicker() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.mp3, .wav, .flac, .aiff, .audio]
        panel.allowsMultipleSelection = true
        panel.canChooseDirectories    = false
        if panel.runModal() == .OK {
            for url in panel.urls { loadFile(url: url) }
        }
    }

    private func loadFile(url: URL) {
        guard url.startAccessingSecurityScopedResource() else {
            // Local file — no sandbox restriction
            if let data = try? Data(contentsOf: url) {
                model.enqueue(data, name: url.lastPathComponent)
            }
            return
        }
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

// MARK: - UTType convenience

private extension UTType {
    static let mp3  = UTType("public.mp3") ?? .audio
    static let wav  = UTType("com.microsoft.waveform-audio") ?? .audio
    static let flac = UTType("org.xiph.flac") ?? .audio
    static let aiff = UTType("public.aiff-audio") ?? .audio
}
