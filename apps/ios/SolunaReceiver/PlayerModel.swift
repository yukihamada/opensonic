//
//  PlayerModel.swift
//  SolunaReceiver
//
//  Manages music player state: file upload, WebSocket file transfer,
//  AVPlayer local playback, animated spectrum visualizer, queue system,
//  Now Playing info, and remote command center integration.
//

import Foundation
import AVFoundation
import MediaPlayer
import SwiftUI

#if os(iOS)
typealias PlatformImage = UIImage
#elseif os(macOS)
typealias PlatformImage = NSImage
#endif

// MARK: - QueueItem

struct QueueItem: Identifiable {
    let id = UUID()
    let name: String
    let title: String       // from ID3 or filename without extension
    let artist: String      // from ID3 or ""
    var data: Data
    var artwork: PlatformImage?
}

@MainActor
final class PlayerModel: ObservableObject {

    // MARK: - Phase

    enum Phase: Equatable {
        case idle
        case streaming      // PCM over WS; local file not yet received
        case transferring   // receiving file chunks from daemon
        case switching      // file complete, awaiting player.switch timing
        case localPlaying   // AVPlayer active
        case paused

        var label: String {
            switch self {
            case .idle:          return "No Track"
            case .streaming:     return "Streaming"
            case .transferring:  return "Receiving…"
            case .switching:     return "Syncing…"
            case .localPlaying:  return "Playing"
            case .paused:        return "Paused"
            }
        }

        var icon: String {
            switch self {
            case .idle:          return "music.note"
            case .streaming:     return "dot.radiowaves.left.and.right"
            case .transferring:  return "arrow.down.circle.fill"
            case .switching:     return "arrow.triangle.2.circlepath"
            case .localPlaying:  return "play.circle.fill"
            case .paused:        return "pause.circle.fill"
            }
        }

        var isPlaying: Bool { self == .localPlaying || self == .streaming }
    }

    // MARK: - Constants

    static let maxFileSizeMB = 200

    // MARK: - Published

    @Published private(set) var phase:        Phase  = .idle
    @Published private(set) var trackName:    String = ""
    @Published private(set) var trackFmt:     String = ""
    @Published private(set) var durationMs:   UInt64 = 0
    @Published private(set) var positionMs:   UInt64 = 0
    @Published private(set) var transferPct:  Double = 0   // 0..1
    @Published              var spectrum:     [Float] = Array(repeating: 0, count: 20)
    @Published              var uploadProgress: Double = 0  // 0..1
    @Published              var queue: [QueueItem] = []
    @Published private(set) var currentIndex: Int = 0
    @Published              var errorMessage: String? = nil

    // MARK: - Private

    private weak var _daemon: DaemonClient?
    private var player:    AVPlayer?
    private var timeObs:   Any?
    private var endObs:    NSObjectProtocol?
    private var specTimer: Timer?
    private var specPhase: Float = 0

    /// Index in queue that has been pre-fetched to the daemon (next-file buffer).
    private var _prefetchedIndex: Int? = nil
    /// Whether pre-fetch upload is currently in flight.
    private var _prefetching = false

    /// Reference to SpeakersController for multi-speaker upload
    weak var speakersController: SpeakersController?

    // MARK: - Computed

    var currentItem: QueueItem? {
        guard currentIndex < queue.count else { return nil }
        return queue[currentIndex]
    }

    // MARK: - Init

    init() {
        setupRemoteCommands()
        #if os(iOS)
        configureAudioSession()
        #endif
    }

    // MARK: - Daemon binding

    var daemon: DaemonClient? {
        get { _daemon }
        set {
            _daemon = newValue
            guard let d = newValue else { return }
            d.onFileReceived = { [weak self] data, name in
                Task { @MainActor [weak self] in self?.handleFile(data, name: name) }
            }
            d.onPlayerSwitch = { [weak self] delay, pos in
                Task { @MainActor [weak self] in self?.handleSwitch(delayMs: delay, posMs: pos) }
            }
            d.onNextFileReceived = { [weak self] data, name in
                Task { @MainActor [weak self] in self?.handleNextFile(data, name: name) }
            }
            // Sync initial state
            if d.playerActive { phase = .streaming }
            trackName  = d.playerName
            trackFmt   = d.playerFmt
            durationMs = d.playerDurMs
        }
    }

    func rebindIfNeeded(_ daemon: DaemonClient?) {
        guard _daemon !== daemon else { return }
        self.daemon = daemon
    }

    // MARK: - Queue management

    func enqueue(_ data: Data, name: String) {
        let sizeMB = data.count / (1024 * 1024)
        if sizeMB > Self.maxFileSizeMB {
            errorMessage = "File \"\(name)\" is \(sizeMB) MB — limit is \(Self.maxFileSizeMB) MB."
            return
        }
        let item = extractMetadata(data: data, name: name)
        queue.append(item)
        if phase == .idle {
            currentIndex = queue.count - 1
            uploadAndPlay(item: item)
        }
    }

    func nextTrack() {
        guard currentIndex + 1 < queue.count else { return }
        let nextIdx = currentIndex + 1
        currentIndex = nextIdx
        // If this track was pre-buffered, just signal the daemon to switch
        if _prefetchedIndex == nextIdx {
            _prefetchedIndex = nil
            _prefetching = false
            trackName  = queue[nextIdx].name
            trackFmt   = formatLabel(for: queue[nextIdx].name)
            phase      = .switching
            _daemon?.playerNextFileReady()
        } else {
            _prefetchedIndex = nil
            uploadAndPlay(item: queue[currentIndex])
        }
    }

    func previousTrack() {
        guard currentIndex > 0 else { return }
        currentIndex -= 1
        uploadAndPlay(item: queue[currentIndex])
    }

    func jumpTo(index: Int) {
        guard index >= 0 && index < queue.count else { return }
        currentIndex = index
        uploadAndPlay(item: queue[currentIndex])
    }

    private func uploadAndPlay(item: QueueItem) {
        stopCurrentPlayer()
        trackName     = item.name
        trackFmt      = formatLabel(for: item.name)
        phase         = .streaming
        transferPct   = 0
        uploadProgress = 0

        Task { [weak self] in
            guard let self else { return }
            let data = item.data
            let name = item.name
            if let sc = self.speakersController {
                await sc.uploadToAll(data, name: name) { [weak self] pct in
                    Task { @MainActor [weak self] in self?.uploadProgress = pct }
                }
            } else if let d = self._daemon, d.isConnected {
                await d.uploadFile(data, name: name) { [weak self] pct in
                    Task { @MainActor [weak self] in self?.uploadProgress = pct }
                }
            }
        }
        updateNowPlaying()
    }

    // MARK: - Upload (legacy single-file entry point)

    func upload(_ data: Data, name: String) {
        enqueue(data, name: name)
    }

    // MARK: - Metadata extraction

    private func extractMetadata(data: Data, name: String) -> QueueItem {
        let tmp = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("soluna_meta_\(UUID().uuidString)_\(name)")
        var titleResult  = String(name.prefix(while: { $0 != "." }))
        var artistResult = ""
        var artResult:   PlatformImage? = nil

        do {
            try data.write(to: tmp)
            let asset = AVAsset(url: tmp)
            let meta  = asset.commonMetadata
            for item in meta {
                switch item.commonKey {
                case AVMetadataKey.commonKeyTitle:
                    if let v = item.stringValue, !v.isEmpty { titleResult = v }
                case AVMetadataKey.commonKeyArtist:
                    if let v = item.stringValue, !v.isEmpty { artistResult = v }
                case AVMetadataKey.commonKeyArtwork:
                    if let d = item.dataValue {
                        #if os(iOS)
                        artResult = UIImage(data: d)
                        #elseif os(macOS)
                        artResult = NSImage(data: d)
                        #endif
                    }
                default:
                    break
                }
            }
            try? FileManager.default.removeItem(at: tmp)
        } catch {
            try? FileManager.default.removeItem(at: tmp)
        }

        return QueueItem(name: name,
                         title: titleResult,
                         artist: artistResult,
                         data: data,
                         artwork: artResult)
    }

    // MARK: - Transport controls

    func play() {
        if phase == .paused {
            player?.play()
            phase = .localPlaying
            startSpectrum()
            updateNowPlaying()
        } else {
            _daemon?.playerPlay()
        }
    }

    func pause() {
        if phase == .localPlaying {
            player?.pause()
            phase = .paused
            stopSpectrum()
            updateNowPlaying()
        } else {
            _daemon?.playerPause()
        }
    }

    func stop() {
        _daemon?.playerStop()
        stopCurrentPlayer()
        phase      = .idle
        trackName  = ""
        trackFmt   = ""
        durationMs = 0
        positionMs = 0
        uploadProgress = 0
        cleanTempFiles()
        updateNowPlaying()
    }

    func seek(fraction: Double) {
        guard durationMs > 0 else { return }
        let ms = UInt64(fraction * Double(durationMs))
        positionMs = ms
        if phase == .localPlaying || phase == .paused {
            player?.seek(to: CMTime(value: CMTimeValue(ms), timescale: 1000))
        } else {
            _daemon?.playerSeek(ms: ms)
        }
        updateNowPlaying()
    }

    // MARK: - Pre-buffer (next track)

    /// Triggered at 75% playback to pre-upload the next queue item.
    private func prefetchNextIfNeeded() {
        let nextIdx = currentIndex + 1
        guard nextIdx < queue.count,
              _prefetchedIndex != nextIdx,
              !_prefetching else { return }
        _prefetching = true
        let item = queue[nextIdx]
        Task { [weak self] in
            guard let self, let d = self._daemon, d.isConnected else {
                await MainActor.run { self?._prefetching = false }
                return
            }
            await d.prefetchFile(item.data, name: item.name)
            await MainActor.run {
                self._prefetchedIndex = nextIdx
                self._prefetching = false
            }
        }
    }

    /// Called when daemon has fully distributed the next-file buffer.
    private func handleNextFile(_ data: Data, name: String) {
        // Write to temp file so AVPlayer can open it on switch
        let tmp = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("soluna_next_\(name)")
        try? data.write(to: tmp)
        // Signal daemon to schedule the switch (50ms delay)
        _daemon?.playerNextFileReady()
    }

    // MARK: - File complete → player.file_ready

    private func handleFile(_ data: Data, name: String) {
        let tmp = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("soluna_\(name)")
        guard (try? data.write(to: tmp)) != nil else { return }
        transferPct = 1.0
        phase = .switching
        _daemon?.playerFileReady()
    }

    // MARK: - player.switch → start AVPlayer

    private func handleSwitch(delayMs: UInt32, posMs: UInt64) {
        let tmpDir  = URL(fileURLWithPath: NSTemporaryDirectory())
        var tmp     = tmpDir.appendingPathComponent("soluna_\(trackName)")
        // Check if file arrived via pre-buffer path (soluna_next_<name>)
        if !FileManager.default.fileExists(atPath: tmp.path) {
            let nextTmp = tmpDir.appendingPathComponent("soluna_next_\(trackName)")
            if FileManager.default.fileExists(atPath: nextTmp.path) {
                try? FileManager.default.moveItem(at: nextTmp, to: tmp)
            }
        }
        guard FileManager.default.fileExists(atPath: tmp.path) else { return }

        let item = AVPlayerItem(url: tmp)
        let p    = AVPlayer(playerItem: item)
        player = p

        // Observe duration from the asset
        let asset = item.asset
        Task { [weak self] in
            let dur = try? await asset.load(.duration)
            guard let self, let d = dur, d.isNumeric else { return }
            self.durationMs = UInt64(d.seconds * 1000)
            self.updateNowPlaying()
        }

        p.seek(to: CMTime(value: CMTimeValue(posMs), timescale: 1000)) { [weak self, weak p] _ in
            guard let self, let p else { return }
            DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(Int(delayMs))) {
                p.play()
                self.phase = .localPlaying
                self.startSpectrum()
                self.updateNowPlaying()
            }
        }

        var prefetchTriggered = false
        let iv = CMTime(seconds: 0.25, preferredTimescale: CMTimeScale(NSEC_PER_SEC))
        timeObs = p.addPeriodicTimeObserver(forInterval: iv, queue: .main) { [weak self] t in
            guard let self else { return }
            let ms = UInt64(max(0, t.seconds) * 1000)
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.positionMs = ms
                self.updateNowPlaying()
                // Pre-fetch next track at 75% of current track
                if !prefetchTriggered,
                   self.durationMs > 0,
                   ms >= self.durationMs * 3 / 4 {
                    prefetchTriggered = true
                    self.prefetchNextIfNeeded()
                }
            }
        }

        // End of file → auto-advance queue (remove old observer first)
        if let obs = endObs { NotificationCenter.default.removeObserver(obs) }
        endObs = NotificationCenter.default.addObserver(forName: .AVPlayerItemDidPlayToEndTime,
                                               object: item, queue: .main) { [weak self] _ in
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.stopSpectrum()
                if self.currentIndex + 1 < self.queue.count {
                    self.nextTrack()
                } else {
                    self.phase = .idle
                    self.updateNowPlaying()
                }
            }
        }
    }

    // MARK: - Now Playing + Remote Commands

    private func setupRemoteCommands() {
        let center = MPRemoteCommandCenter.shared()
        center.playCommand.addTarget { [weak self] _ in
            self?.play()
            return .success
        }
        center.pauseCommand.addTarget { [weak self] _ in
            self?.pause()
            return .success
        }
        center.nextTrackCommand.addTarget { [weak self] _ in
            self?.nextTrack()
            return .success
        }
        center.previousTrackCommand.addTarget { [weak self] _ in
            self?.previousTrack()
            return .success
        }
        center.changePlaybackPositionCommand.addTarget { [weak self] event in
            guard let e = event as? MPChangePlaybackPositionCommandEvent,
                  let self else { return .commandFailed }
            let duration = Double(self.durationMs) / 1000
            guard duration > 0 else { return .commandFailed }
            self.seek(fraction: e.positionTime / duration)
            return .success
        }
        center.togglePlayPauseCommand.addTarget { [weak self] _ in
            guard let self else { return .commandFailed }
            if self.phase.isPlaying { self.pause() } else { self.play() }
            return .success
        }
    }

    private func updateNowPlaying() {
        var info = [String: Any]()
        if let item = currentItem {
            info[MPMediaItemPropertyTitle]  = item.title.isEmpty ? item.name : item.title
            info[MPMediaItemPropertyArtist] = item.artist
            if let art = item.artwork {
                info[MPMediaItemPropertyArtwork] = MPMediaItemArtwork(
                    boundsSize: CGSize(width: 300, height: 300)) { _ in art }
            }
        } else if !trackName.isEmpty {
            info[MPMediaItemPropertyTitle] = trackName
        }
        info[MPMediaItemPropertyPlaybackDuration] = Double(durationMs) / 1000
        info[MPNowPlayingInfoPropertyElapsedPlaybackTime] = Double(positionMs) / 1000
        info[MPNowPlayingInfoPropertyPlaybackRate] = phase.isPlaying ? 1.0 : 0.0
        MPNowPlayingInfoCenter.default().nowPlayingInfo = info
    }

    // MARK: - iOS AVAudioSession

    #if os(iOS)
    private func configureAudioSession() {
        try? AVAudioSession.sharedInstance().setCategory(.playback, mode: .default)
        try? AVAudioSession.sharedInstance().setActive(true)
    }
    #endif

    // MARK: - Helpers

    private func stopCurrentPlayer() {
        player?.pause()
        if let obs = timeObs { player?.removeTimeObserver(obs) }
        timeObs = nil
        if let obs = endObs { NotificationCenter.default.removeObserver(obs) }
        endObs = nil
        player  = nil
        stopSpectrum()
    }

    private func cleanTempFiles() {
        let tmp   = URL(fileURLWithPath: NSTemporaryDirectory())
        let files = (try? FileManager.default.contentsOfDirectory(
            at: tmp, includingPropertiesForKeys: nil)) ?? []
        files.filter { $0.lastPathComponent.hasPrefix("soluna_") }
             .forEach { try? FileManager.default.removeItem(at: $0) }
    }

    private func formatLabel(for name: String) -> String {
        let lower = name.lowercased()
        if lower.hasSuffix(".mp3")  { return "MP3" }
        if lower.hasSuffix(".flac") { return "FLAC" }
        if lower.hasSuffix(".aiff") || lower.hasSuffix(".aif") { return "AIFF" }
        if lower.hasSuffix(".alac") || lower.hasSuffix(".m4a") { return "ALAC" }
        return "WAV"
    }

    // MARK: - Spectrum animation

    func startSpectrum() {
        stopSpectrum()
        specTimer = Timer.scheduledTimer(withTimeInterval: 0.08, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in self?.tickSpectrum() }
        }
    }

    private func stopSpectrum() {
        specTimer?.invalidate()
        specTimer = nil
        withAnimation(Animation.easeOut(duration: 0.5)) {
            spectrum = Array(repeating: 0, count: spectrum.count)
        }
    }

    private func tickSpectrum() {
        specPhase += 0.12
        let n = spectrum.count
        spectrum = (0..<n).map { i in
            let f   = Float(i)
            let low = 0.38 * sin(specPhase * 0.8 + f * 0.45)
            let mid = 0.30 * sin(specPhase * 1.6 + f * 0.75)
            let hi  = 0.14 * sin(specPhase * 2.5 + f * 1.20)
            return max(0.04, min(1.0, (low + mid + hi + 0.82) * 0.52))
        }
    }

    deinit {
        specTimer?.invalidate()
        // cleanTempFiles() called from nonisolated deinit cannot call @MainActor method
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("soluna_player")
        try? FileManager.default.removeItem(at: dir)
    }
}
