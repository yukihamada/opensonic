import Foundation
#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#endif

// MARK: - RelayRegion

/// A relay server region with connectivity metadata.
public struct RelayRegion: Identifiable, Sendable, Hashable {
    /// Unique region identifier (e.g. "nrt", "lax", "ams").
    public let id: String

    /// Relay server hostname.
    public let host: String

    /// Relay server port.
    public let port: UInt16

    /// Measured round-trip latency in milliseconds (nil if not yet probed).
    public var latencyMs: Double?

    /// Human-readable region name.
    public let name: String

    /// Flag emoji for the region.
    public let flag: String

    public init(id: String, host: String, port: UInt16, latencyMs: Double? = nil, name: String, flag: String) {
        self.id = id
        self.host = host
        self.port = port
        self.latencyMs = latencyMs
        self.name = name
        self.flag = flag
    }

    public func hash(into hasher: inout Hasher) {
        hasher.combine(id)
    }

    public static func == (lhs: RelayRegion, rhs: RelayRegion) -> Bool {
        lhs.id == rhs.id
    }
}

// MARK: - Default Regions

/// Pre-configured relay regions.
public enum RelayRegions {
    public static let tokyo = RelayRegion(
        id: "nrt", host: "relay.solun.art", port: OSTConstants.defaultPort,
        name: "Tokyo", flag: "🇯🇵"
    )
    public static let losAngeles = RelayRegion(
        id: "lax", host: "lax.relay.solun.art", port: OSTConstants.defaultPort,
        name: "Los Angeles", flag: "🇺🇸"
    )
    public static let amsterdam = RelayRegion(
        id: "ams", host: "ams.relay.solun.art", port: OSTConstants.defaultPort,
        name: "Amsterdam", flag: "🇳🇱"
    )

    public static let all: [RelayRegion] = [tokyo, losAngeles, amsterdam]
}

// MARK: - MultiRegionRouter

/// Automatic multi-region relay selection with latency probing and failover.
///
/// Sends UDP HELLO probes to each region and selects the lowest-latency relay.
/// If the current relay fails, automatically switches to the next best region.
///
/// Usage:
/// ```swift
/// let router = MultiRegionRouter()
/// let best = await router.selectBestRegion()
/// print("Using \(best.name) (\(best.latencyMs ?? -1)ms)")
/// ```
@MainActor
public final class MultiRegionRouter: ObservableObject {

    // MARK: - Published State

    /// All known relay regions with their latest latency measurements.
    @Published public var availableRegions: [RelayRegion]

    /// The currently selected region (nil if none selected).
    @Published public var currentRegion: RelayRegion?

    // MARK: - Configuration

    /// Number of probe packets to send per region.
    public var probeCount: Int = 3

    /// Timeout for each probe in seconds.
    public var probeTimeoutSeconds: TimeInterval = 2.0

    /// User-preferred region override (nil = auto-select).
    private var preferredRegionId: String?

    // MARK: - Init

    public init(regions: [RelayRegion] = RelayRegions.all) {
        self.availableRegions = regions
    }

    // MARK: - Public API

    /// Probe all regions and return them sorted by latency (lowest first).
    ///
    /// Sends UDP HELLO packets to each region and measures round-trip time.
    /// Regions that fail to respond within the timeout get `latencyMs = nil`.
    public func probeAllRegions() async -> [RelayRegion] {
        var results: [RelayRegion] = []

        for region in availableRegions {
            var probed = region
            probed.latencyMs = await probeLatency(host: region.host, port: region.port)
            results.append(probed)
        }

        // Sort: regions with latency first (ascending), then unreachable
        results.sort { a, b in
            switch (a.latencyMs, b.latencyMs) {
            case let (aMs?, bMs?): return aMs < bMs
            case (_?, nil): return true
            case (nil, _?): return false
            case (nil, nil): return false
            }
        }

        availableRegions = results
        return results
    }

    /// Probe all regions and select the one with the lowest latency.
    ///
    /// If a preferred region is set and reachable, it will be selected instead.
    @discardableResult
    public func selectBestRegion() async -> RelayRegion {
        let probed = await probeAllRegions()

        // Honor preferred region if set and reachable
        if let preferredId = preferredRegionId,
           let preferred = probed.first(where: { $0.id == preferredId && $0.latencyMs != nil }) {
            currentRegion = preferred
            return preferred
        }

        // Select lowest latency
        if let best = probed.first(where: { $0.latencyMs != nil }) {
            currentRegion = best
            return best
        }

        // Fallback to first region even if unreachable
        let fallback = probed.first ?? RelayRegions.tokyo
        currentRegion = fallback
        return fallback
    }

    /// Set a preferred region. The router will favor this region if it is reachable.
    ///
    /// Pass nil to clear the preference and return to automatic selection.
    public func setPreferredRegion(_ region: RelayRegion?) {
        preferredRegionId = region?.id
        if let region {
            currentRegion = region
        }
    }

    /// Trigger failover: mark the current region as failed and switch to the next best.
    @discardableResult
    public func failover() async -> RelayRegion {
        // Remove current region from candidates temporarily
        let failedId = currentRegion?.id
        let candidates = availableRegions.filter { $0.id != failedId && $0.latencyMs != nil }

        if let next = candidates.first {
            currentRegion = next
            return next
        }

        // Re-probe everything
        return await selectBestRegion()
    }

    // MARK: - Latency Probing (BSD Socket UDP)

    /// Send UDP HELLO probes to measure round-trip latency.
    ///
    /// Returns the median RTT in milliseconds, or nil if the host is unreachable.
    private func probeLatency(host: String, port: UInt16) async -> Double? {
        await withCheckedContinuation { continuation in
            DispatchQueue.global(qos: .utility).async {
                var rtts: [Double] = []

                // DNS resolve
                var hints = addrinfo()
                hints.ai_family = AF_INET
                hints.ai_socktype = SOCK_DGRAM
                var res: UnsafeMutablePointer<addrinfo>?

                guard getaddrinfo(host, "\(port)", &hints, &res) == 0, let addrInfo = res else {
                    continuation.resume(returning: nil)
                    return
                }

                var addr = sockaddr_in()
                memcpy(&addr, addrInfo.pointee.ai_addr, Int(addrInfo.pointee.ai_addrlen))
                freeaddrinfo(res)

                // Create socket
                let sock = socket(AF_INET, SOCK_DGRAM, 0)
                guard sock >= 0 else {
                    continuation.resume(returning: nil)
                    return
                }

                // Set receive timeout
                let timeoutUs = Int32(self.probeTimeoutSeconds * 1_000_000)
                var tv = timeval(tv_sec: Int(timeoutUs / 1_000_000), tv_usec: Int32(timeoutUs % 1_000_000))
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

                let hello = "HELLO\n"

                for _ in 0..<self.probeCount {
                    let start = CFAbsoluteTimeGetCurrent()

                    // Send HELLO
                    hello.withCString { ptr in
                        withUnsafePointer(to: addr) { addrPtr in
                            let sockaddrPtr = UnsafeRawPointer(addrPtr).assumingMemoryBound(to: sockaddr.self)
                            _ = sendto(sock, ptr, strlen(ptr), 0, sockaddrPtr, socklen_t(MemoryLayout<sockaddr_in>.size))
                        }
                    }

                    // Wait for any response
                    var buf = [UInt8](repeating: 0, count: 1024)
                    var senderAddr = sockaddr_in()
                    var senderLen = socklen_t(MemoryLayout<sockaddr_in>.size)

                    let n = withUnsafeMutablePointer(to: &senderAddr) { senderPtr -> Int in
                        senderPtr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sockaddrPtr in
                            recvfrom(sock, &buf, buf.count, 0, sockaddrPtr, &senderLen)
                        }
                    }

                    if n > 0 {
                        let elapsed = (CFAbsoluteTimeGetCurrent() - start) * 1000.0
                        rtts.append(elapsed)
                    }

                    // Brief pause between probes
                    usleep(50_000)
                }

                Darwin.close(sock)

                if rtts.isEmpty {
                    continuation.resume(returning: nil)
                } else {
                    // Return median RTT
                    rtts.sort()
                    let median = rtts[rtts.count / 2]
                    continuation.resume(returning: median)
                }
            }
        }
    }
}
