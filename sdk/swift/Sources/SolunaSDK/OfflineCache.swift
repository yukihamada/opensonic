import Foundation
import AVFoundation

/// Metadata for a cached audio track.
public struct CachedTrack: Identifiable, Sendable {
    /// Unique identifier for this cached track.
    public let id: String
    /// Channel this track was recorded from.
    public let channel: String
    /// Track title (from META: packet or generated).
    public let title: String
    /// Duration in seconds.
    public let duration: TimeInterval
    /// File URL for the cached .caf file.
    public let fileURL: URL
    /// When this track was cached.
    public let cachedAt: Date
}

/// Automatic caching of received audio to disk as .caf files.
///
/// Organizes cached audio by channel in the app's caches directory:
/// `caches/soluna/<channel>/<timestamp>.caf`
///
/// Implements LRU eviction when `maxCacheSize` is exceeded.
///
/// Usage:
/// ```swift
/// let cache = OfflineCache()
/// cache.isEnabled = true
/// // Feed audio buffers from the player...
/// cache.writeBuffer(buffer, channel: "jazz", title: "Late Night Jazz")
///
/// // Later, play offline
/// let tracks = cache.cachedTracks(for: "jazz")
/// if let track = tracks.first {
///     cache.playOffline(track: track, using: audioPlayer)
/// }
/// ```
public final class OfflineCache {

    // MARK: - Public Properties

    /// Whether caching is enabled. When `false`, no new data is written.
    public var isEnabled: Bool = false

    /// Current total cache size in bytes.
    public private(set) var cacheSize: Int64 = 0

    /// Maximum cache size in bytes (default 500 MB).
    public var maxCacheSize: Int64 = 500 * 1024 * 1024

    // MARK: - Private

    private let fileManager = FileManager.default
    private let cacheRoot: URL
    private let queue = DispatchQueue(label: "com.soluna.offlinecache", qos: .utility)

    /// Currently open file handle for writing, keyed by channel.
    private var activeWriters: [String: CacheWriter] = [:]

    // MARK: - Init

    public init() {
        let caches = fileManager.urls(for: .cachesDirectory, in: .userDomainMask).first!
        cacheRoot = caches.appendingPathComponent("soluna", isDirectory: true)
        ensureDirectory(cacheRoot)
        recalculateCacheSize()
    }

    // MARK: - Public API

    /// Write a PCM buffer to the cache for the given channel.
    ///
    /// - Parameters:
    ///   - buffer: The decoded PCM audio buffer.
    ///   - channel: Channel name.
    ///   - title: Optional track title.
    public func writeBuffer(_ buffer: AVAudioPCMBuffer, channel: String, title: String? = nil) {
        guard isEnabled else { return }

        queue.async { [weak self] in
            guard let self else { return }

            let writer = self.writerForChannel(channel, title: title)
            writer.appendBuffer(buffer)

            // Periodic size check
            if writer.bytesWritten % (1024 * 1024) < UInt64(buffer.frameLength * 4) {
                self.recalculateCacheSize()
                self.evictIfNeeded()
            }
        }
    }

    /// Finalize the current recording segment for a channel.
    ///
    /// - Parameter channel: Channel name.
    public func finishSegment(for channel: String) {
        queue.async { [weak self] in
            self?.activeWriters[channel]?.close()
            self?.activeWriters.removeValue(forKey: channel)
            self?.recalculateCacheSize()
        }
    }

    /// Get all cached tracks for a given channel, sorted newest first.
    ///
    /// - Parameter channel: Channel name.
    /// - Returns: Array of `CachedTrack` metadata.
    public func cachedTracks(for channel: String) -> [CachedTrack] {
        let channelDir = cacheRoot.appendingPathComponent(channel, isDirectory: true)
        guard let files = try? fileManager.contentsOfDirectory(
            at: channelDir,
            includingPropertiesForKeys: [.fileSizeKey, .creationDateKey],
            options: [.skipsHiddenFiles]
        ) else {
            return []
        }

        return files
            .filter { $0.pathExtension == "caf" }
            .compactMap { url -> CachedTrack? in
                guard let attrs = try? fileManager.attributesOfItem(atPath: url.path),
                      let createdAt = attrs[.creationDate] as? Date,
                      let fileSize = attrs[.size] as? Int64 else { return nil }

                // Estimate duration: stereo float32 @ 48kHz = 384000 bytes/sec
                let duration = TimeInterval(fileSize) / 384000.0

                let name = url.deletingPathExtension().lastPathComponent
                return CachedTrack(
                    id: url.lastPathComponent,
                    channel: channel,
                    title: name,
                    duration: duration,
                    fileURL: url,
                    cachedAt: createdAt
                )
            }
            .sorted { $0.cachedAt > $1.cachedAt }
    }

    /// Play a cached track through an AVAudioEngine player node.
    ///
    /// - Parameters:
    ///   - track: The `CachedTrack` to play.
    ///   - engine: An AVAudioEngine (must already be started).
    ///   - playerNode: An AVAudioPlayerNode attached to the engine.
    public func playOffline(track: CachedTrack, engine: AVAudioEngine, playerNode: AVAudioPlayerNode) {
        guard let audioFile = try? AVAudioFile(forReading: track.fileURL) else {
            print("[SolunaSDK] OfflineCache: Failed to open \(track.fileURL.lastPathComponent)")
            return
        }

        playerNode.stop()
        playerNode.scheduleFile(audioFile, at: nil)
        playerNode.play()
    }

    /// Remove all cached files.
    public func clearCache() {
        queue.async { [weak self] in
            guard let self else { return }
            // Close all active writers
            for (_, writer) in self.activeWriters {
                writer.close()
            }
            self.activeWriters.removeAll()

            try? self.fileManager.removeItem(at: self.cacheRoot)
            self.ensureDirectory(self.cacheRoot)
            self.cacheSize = 0
        }
    }

    // MARK: - Private Helpers

    private func writerForChannel(_ channel: String, title: String?) -> CacheWriter {
        if let existing = activeWriters[channel] { return existing }

        let channelDir = cacheRoot.appendingPathComponent(channel, isDirectory: true)
        ensureDirectory(channelDir)

        let timestamp = Int(Date().timeIntervalSince1970)
        let filename = title?.replacingOccurrences(of: "/", with: "-") ?? "\(timestamp)"
        let fileURL = channelDir.appendingPathComponent("\(filename).caf")

        let writer = CacheWriter(url: fileURL)
        activeWriters[channel] = writer
        return writer
    }

    private func ensureDirectory(_ url: URL) {
        if !fileManager.fileExists(atPath: url.path) {
            try? fileManager.createDirectory(at: url, withIntermediateDirectories: true)
        }
    }

    private func recalculateCacheSize() {
        var total: Int64 = 0
        if let enumerator = fileManager.enumerator(
            at: cacheRoot,
            includingPropertiesForKeys: [.fileSizeKey],
            options: [.skipsHiddenFiles]
        ) {
            for case let fileURL as URL in enumerator {
                if let size = try? fileURL.resourceValues(forKeys: [.fileSizeKey]).fileSize {
                    total += Int64(size)
                }
            }
        }
        cacheSize = total
    }

    private func evictIfNeeded() {
        guard cacheSize > maxCacheSize else { return }

        // Collect all cached files with creation dates
        var files: [(url: URL, date: Date, size: Int64)] = []
        if let enumerator = fileManager.enumerator(
            at: cacheRoot,
            includingPropertiesForKeys: [.fileSizeKey, .creationDateKey],
            options: [.skipsHiddenFiles]
        ) {
            for case let fileURL as URL in enumerator {
                guard fileURL.pathExtension == "caf",
                      let values = try? fileURL.resourceValues(forKeys: [.fileSizeKey, .creationDateKey]),
                      let size = values.fileSize,
                      let date = values.creationDate else { continue }
                files.append((url: fileURL, date: date, size: Int64(size)))
            }
        }

        // LRU: remove oldest files first
        files.sort { $0.date < $1.date }

        for file in files {
            guard cacheSize > maxCacheSize else { break }
            try? fileManager.removeItem(at: file.url)
            cacheSize -= file.size
        }
    }
}

// MARK: - CacheWriter

/// Internal helper that writes PCM buffers to a .caf file.
private final class CacheWriter {
    private var audioFile: AVAudioFile?
    private(set) var bytesWritten: UInt64 = 0

    init(url: URL) {
        let format = AVAudioFormat(
            commonFormat: .pcmFormatFloat32,
            sampleRate: 48000,
            channels: 2,
            interleaved: false
        )!
        audioFile = try? AVAudioFile(forWriting: url, settings: format.settings)
    }

    func appendBuffer(_ buffer: AVAudioPCMBuffer) {
        guard let file = audioFile else { return }
        do {
            try file.write(from: buffer)
            bytesWritten += UInt64(buffer.frameLength) * UInt64(buffer.format.channelCount) * 4
        } catch {
            print("[SolunaSDK] OfflineCache write error: \(error)")
        }
    }

    func close() {
        audioFile = nil
    }
}
