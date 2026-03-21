//
//  ClockSync.swift
//  SolunaReceiver
//
//  Manages NTP-based playback synchronization.
//  Uses the system clock (which is NTP-synced on iOS/macOS) and RTP timestamps
//  to align playback across devices within ±10ms.
//

import Foundation

/// Thread-safe NTP clock synchronization for multi-device audio playback.
/// Maps RTP timestamps to wall-clock time so all receivers play the same
/// audio sample at the same moment.
final class ClockSync: @unchecked Sendable {
    static let shared = ClockSync()

    /// Target end-to-end latency in milliseconds.
    /// All devices buffer this much before playing, ensuring sync.
    var targetLatencyMs: Int = 200

    /// The relay's reference: maps RTP timestamp to wall-clock nanoseconds
    private var refRtpTimestamp: UInt32 = 0
    private var refWallClockNs: UInt64 = 0
    private(set) var hasReference = false
    private var lock = os_unfair_lock()

    /// Current wall-clock time in nanoseconds (NTP-synced via system)
    var wallClockNs: UInt64 {
        var ts = timespec()
        clock_gettime(CLOCK_REALTIME, &ts)
        return UInt64(ts.tv_sec) * 1_000_000_000 + UInt64(ts.tv_nsec)
    }

    /// Set the reference point from relay (received via SYNC message or first packet).
    /// Thread-safe.
    func setReference(rtpTimestamp: UInt32, wallClockNs: UInt64) {
        os_unfair_lock_lock(&lock)
        refRtpTimestamp = rtpTimestamp
        refWallClockNs = wallClockNs
        hasReference = true
        os_unfair_lock_unlock(&lock)
    }

    /// Calculate how many samples to skip or add to align with wall-clock.
    /// Returns: positive = skip samples (we're behind), negative = add silence (we're ahead).
    /// Thread-safe.
    func calculateOffset(currentRtpTimestamp: UInt32, sampleRate: Int = 48000) -> Int {
        os_unfair_lock_lock(&lock)
        guard hasReference else {
            os_unfair_lock_unlock(&lock)
            return 0
        }
        let refRtp = refRtpTimestamp
        let refWall = refWallClockNs
        os_unfair_lock_unlock(&lock)

        // Expected wall-clock time for this RTP timestamp
        let rtpDelta = Int64(currentRtpTimestamp) - Int64(refRtp)
        let expectedNs = Int64(refWall) + (rtpDelta * 1_000_000_000 / Int64(sampleRate))

        // Add target latency
        let targetPlayNs = expectedNs + Int64(targetLatencyMs) * 1_000_000

        // Current wall-clock
        let nowNs = Int64(wallClockNs)

        // Difference in samples
        let diffNs = nowNs - targetPlayNs
        let diffSamples = Int(diffNs * Int64(sampleRate) / 1_000_000_000)

        return diffSamples  // positive = late (skip), negative = early (silence)
    }

    /// Current sync offset in milliseconds (for UI display).
    /// Positive = playback is late, negative = early.
    func offsetMs(currentRtpTimestamp: UInt32, sampleRate: Int = 48000) -> Double {
        let samples = calculateOffset(currentRtpTimestamp: currentRtpTimestamp, sampleRate: sampleRate)
        return Double(samples) * 1000.0 / Double(sampleRate)
    }

    func reset() {
        os_unfair_lock_lock(&lock)
        hasReference = false
        refRtpTimestamp = 0
        refWallClockNs = 0
        os_unfair_lock_unlock(&lock)
    }
}
