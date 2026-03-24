import Foundation
import Combine

// MARK: - Negotiation Result

/// Result of protocol version negotiation with the relay server.
public enum NegotiationResult: Sendable {
    /// Both client and server are fully compatible.
    case compatible

    /// Compatible, but the server offers a newer version. Upgrade recommended.
    case upgradeRecommended(serverVersion: String)

    /// The server version is too old or too new; communication is not possible.
    case incompatible(reason: String)
}

// MARK: - ProtocolVersion

/// Manages OSTP protocol version negotiation and feature detection.
///
/// On connect the client sends `VERSION:<client_version>` and expects
/// `VERSION:<server_version>` in response. Features are gated by the
/// negotiated version.
///
/// Usage:
/// ```swift
/// let pv = ProtocolVersion()
/// let result = pv.negotiate(serverVersion: "0.9.1")
/// if pv.isCompatible {
///     // proceed
/// }
/// ```
public final class ProtocolVersion: ObservableObject {

    // MARK: - Constants

    /// Current OSTP protocol version implemented by this SDK.
    public let currentVersion: String = "0.9.3"

    /// Minimum server version this SDK can communicate with.
    public let minimumVersion: String = "0.8.0"

    // MARK: - Published State

    /// The server's protocol version after negotiation. Nil before negotiation.
    @Published public private(set) var serverVersion: String?

    // MARK: - Computed

    /// Whether the negotiated versions are compatible.
    ///
    /// Returns `true` if the server version is at least `minimumVersion`
    /// and the server can understand our `currentVersion`.
    public var isCompatible: Bool {
        guard let sv = serverVersion else { return false }
        return Self.compare(sv, isAtLeast: minimumVersion)
    }

    // MARK: - Init

    public init() {}

    // MARK: - Public API

    /// Negotiate with the server's reported version.
    ///
    /// - Parameter serverVersion: The version string received from the relay.
    /// - Returns: The negotiation result.
    @discardableResult
    public func negotiate(serverVersion: String) -> NegotiationResult {
        self.serverVersion = serverVersion

        // Server too old
        if !Self.compare(serverVersion, isAtLeast: minimumVersion) {
            return .incompatible(reason: "Server \(serverVersion) is below minimum \(minimumVersion)")
        }

        // Client is behind the server
        if Self.compare(serverVersion, isGreaterThan: currentVersion) {
            return .upgradeRecommended(serverVersion: serverVersion)
        }

        return .compatible
    }

    /// Build the client version handshake message to send on connect.
    ///
    /// - Returns: The formatted version string, e.g. `"VERSION:0.9.3\n"`.
    public func versionMessage() -> String {
        "VERSION:\(currentVersion)\n"
    }

    /// Parse a version response from the relay server.
    ///
    /// Expects the format `"VERSION:<semver>"`. Returns `nil` if the
    /// message does not match.
    ///
    /// - Parameter message: The raw control message from the relay.
    /// - Returns: The extracted version string, or `nil`.
    public func parseVersionResponse(_ message: String) -> String? {
        let trimmed = message.trimmingCharacters(in: .whitespacesAndNewlines)
        guard trimmed.hasPrefix("VERSION:") else { return nil }
        let version = String(trimmed.dropFirst("VERSION:".count))
        return version.isEmpty ? nil : version
    }

    // MARK: - Feature Flags by Version

    /// Whether Forward Error Correction is supported (>= 0.9.0).
    public var supportsFEC: Bool {
        guard let sv = serverVersion else { return false }
        return Self.compare(sv, isAtLeast: "0.9.0")
    }

    /// Whether end-to-end encryption is supported (>= 0.9.3).
    public var supportsE2E: Bool {
        guard let sv = serverVersion else { return false }
        return Self.compare(sv, isAtLeast: "0.9.3")
    }

    /// Whether LC3 codec is supported (>= 0.9.1).
    public var supportsLC3: Bool {
        guard let sv = serverVersion else { return false }
        return Self.compare(sv, isAtLeast: "0.9.1")
    }

    /// Whether multi-region routing is supported (>= 0.9.2).
    public var supportsMultiRegion: Bool {
        guard let sv = serverVersion else { return false }
        return Self.compare(sv, isAtLeast: "0.9.2")
    }

    // MARK: - Private — Semver Comparison

    /// Parse a semver string into (major, minor, patch) integers.
    private static func parseSemver(_ version: String) -> (Int, Int, Int)? {
        let parts = version.split(separator: ".").compactMap { Int($0) }
        guard parts.count >= 2 else { return nil }
        let major = parts[0]
        let minor = parts[1]
        let patch = parts.count >= 3 ? parts[2] : 0
        return (major, minor, patch)
    }

    /// Returns `true` if `version >= target`.
    private static func compare(_ version: String, isAtLeast target: String) -> Bool {
        guard let (vMaj, vMin, vPat) = parseSemver(version),
              let (tMaj, tMin, tPat) = parseSemver(target) else { return false }

        if vMaj != tMaj { return vMaj > tMaj }
        if vMin != tMin { return vMin > tMin }
        return vPat >= tPat
    }

    /// Returns `true` if `version > target`.
    private static func compare(_ version: String, isGreaterThan target: String) -> Bool {
        guard let (vMaj, vMin, vPat) = parseSemver(version),
              let (tMaj, tMin, tPat) = parseSemver(target) else { return false }

        if vMaj != tMaj { return vMaj > tMaj }
        if vMin != tMin { return vMin > tMin }
        return vPat > tPat
    }
}
