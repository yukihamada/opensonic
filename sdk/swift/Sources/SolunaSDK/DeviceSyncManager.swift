import Foundation
import Combine

/// Manages multi-device synchronized playback using OSTP wall-clock sync.
///
/// Uses NTP-like clock offset calculation between devices and adjusts
/// AudioPlayer buffer scheduling to synchronize playout across devices.
/// Sends `DELAY:<ms>` messages to the relay for group-wide max delay
/// coordination and receives `MAXDELAY:<ms>` to adjust accordingly.
///
/// Usage:
/// ```swift
/// let sync = DeviceSyncManager()
/// sync.attach(connection: relayConnection)
/// sync.enableSync()
/// // Read sync.syncOffset for current offset
/// sync.disableSync()
/// ```
public final class DeviceSyncManager: ObservableObject {

    // MARK: - Published State

    /// The calculated clock offset in seconds relative to the relay reference clock.
    @Published public private(set) var syncOffset: TimeInterval = 0

    /// Whether sync is currently enabled.
    @Published public private(set) var isSyncEnabled = false

    /// The group-wide maximum delay in milliseconds, as reported by the relay.
    @Published public private(set) var maxDelayMs: Int = 0

    // MARK: - Private

    private weak var connection: RelayConnection?
    private var syncTimer: Timer?

    /// Rolling buffer of measured round-trip offsets for averaging.
    private var offsetSamples: [TimeInterval] = []
    private let maxSamples = 10

    /// Timestamp of the last DELAY probe sent.
    private var probeSentAt: TimeInterval = 0

    // MARK: - Init

    public init() {}

    deinit {
        disableSync()
    }

    // MARK: - Public API

    /// Attach a relay connection for sending/receiving sync messages.
    ///
    /// - Parameter connection: The active `RelayConnection` instance.
    public func attach(connection: RelayConnection) {
        self.connection = connection
    }

    /// Enable synchronized playback. Begins periodic clock offset probes.
    public func enableSync() {
        guard !isSyncEnabled else { return }
        isSyncEnabled = true
        offsetSamples.removeAll()

        // Send a delay probe every 2 seconds
        syncTimer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
            self?.sendDelayProbe()
        }
        // Send first probe immediately
        sendDelayProbe()
    }

    /// Disable synchronized playback and reset offset.
    public func disableSync() {
        isSyncEnabled = false
        syncTimer?.invalidate()
        syncTimer = nil
        syncOffset = 0
        maxDelayMs = 0
        offsetSamples.removeAll()
    }

    /// Handle a control message from the relay. Call this from the relay's
    /// `onControlMessage` handler.
    ///
    /// Recognized messages:
    /// - `PONG:<server_time_ms>` — response to our DELAY probe
    /// - `MAXDELAY:<ms>` — group-wide maximum delay from relay
    ///
    /// - Parameter message: The raw control message string.
    public func handleControlMessage(_ message: String) {
        let trimmed = message.trimmingCharacters(in: .whitespacesAndNewlines)

        if trimmed.hasPrefix("PONG:") {
            handlePong(trimmed)
        } else if trimmed.hasPrefix("MAXDELAY:") {
            handleMaxDelay(trimmed)
        }
    }

    // MARK: - Private

    private func sendDelayProbe() {
        guard let connection, isSyncEnabled else { return }
        probeSentAt = currentTimeMs()
        let localMs = Int(probeSentAt)
        connection.sendControlMessage("DELAY:\(localMs)\n")
    }

    private func handlePong(_ message: String) {
        let now = currentTimeMs()
        guard let serverMsStr = message.split(separator: ":").last,
              let serverMs = Double(serverMsStr) else { return }

        // NTP-like offset: offset = serverTime - (sentTime + rtt/2)
        let rtt = now - probeSentAt
        let offset = serverMs - (probeSentAt + rtt / 2.0)

        offsetSamples.append(offset)
        if offsetSamples.count > maxSamples {
            offsetSamples.removeFirst()
        }

        // Use median for robustness against outliers
        let sorted = offsetSamples.sorted()
        let median = sorted[sorted.count / 2]
        syncOffset = median / 1000.0 // Convert ms to seconds
    }

    private func handleMaxDelay(_ message: String) {
        guard let msStr = message.split(separator: ":").last,
              let ms = Int(msStr) else { return }
        maxDelayMs = ms
    }

    private func currentTimeMs() -> TimeInterval {
        return Date().timeIntervalSince1970 * 1000.0
    }
}
