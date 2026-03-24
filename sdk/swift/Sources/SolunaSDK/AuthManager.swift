import Foundation

/// Role within a Soluna channel, determining access permissions.
public enum ChannelRole: String, Codable, Sendable, CaseIterable, Comparable {
    case listener
    case dj
    case admin
    case owner

    /// Numeric privilege level for comparison.
    private var level: Int {
        switch self {
        case .listener: return 0
        case .dj:       return 1
        case .admin:    return 2
        case .owner:    return 3
        }
    }

    public static func < (lhs: ChannelRole, rhs: ChannelRole) -> Bool {
        lhs.level < rhs.level
    }
}

/// Decoded JWT token claims for Soluna channel authentication.
public struct TokenClaims: Sendable {
    /// The channel this token grants access to.
    public let channelId: String
    /// The authenticated user's identifier.
    public let userId: String
    /// The user's role within the channel.
    public let role: ChannelRole
    /// Token expiration timestamp.
    public let exp: Date
    /// Token issued-at timestamp.
    public let iat: Date
}

/// JWT token-based channel authentication manager.
///
/// Validates JWT tokens for channel access control. Supports HS256 and RS256
/// algorithms (signature verification is delegated to the relay server; this
/// client only decodes and checks expiry).
///
/// Usage:
/// ```swift
/// let auth = AuthManager()
/// auth.setToken(jwtString)
/// if auth.isAuthenticated {
///     print("Role: \(auth.currentRole)")
///     let header = auth.authHeader()
///     // Send AUTH:<token> to relay
/// }
/// ```
public final class AuthManager: ObservableObject {

    // MARK: - Published State

    /// Whether the user is currently authenticated with a valid (non-expired) token.
    @Published public private(set) var isAuthenticated: Bool = false

    /// The current user's role in the channel.
    @Published public private(set) var currentRole: ChannelRole = .listener

    /// The decoded claims from the current token.
    @Published public private(set) var claims: TokenClaims?

    // MARK: - Callbacks

    /// Called when the token is approaching expiration.
    /// The `TimeInterval` parameter is the number of seconds remaining until expiry.
    public var onTokenExpiring: ((TimeInterval) -> Void)?

    // MARK: - Private

    private var rawToken: String?
    private var expiryTimer: Timer?

    /// Control message prefix for relay authentication.
    public static let authPrefix = "AUTH:"

    // MARK: - Init

    public init() {}

    deinit {
        expiryTimer?.invalidate()
    }

    // MARK: - Public API

    /// Set and validate a JWT token for channel authentication.
    ///
    /// Decodes the token payload, checks expiration, and starts an expiry
    /// warning timer that fires 60 seconds before the token expires.
    ///
    /// - Parameter jwt: The raw JWT string (header.payload.signature).
    public func setToken(_ jwt: String) {
        rawToken = jwt

        guard let decoded = Self.decodeJWT(jwt) else {
            isAuthenticated = false
            currentRole = .listener
            claims = nil
            return
        }

        claims = decoded

        if decoded.exp > Date() {
            isAuthenticated = true
            currentRole = decoded.role
            scheduleExpiryWarning(expiration: decoded.exp)
        } else {
            isAuthenticated = false
            currentRole = .listener
        }
    }

    /// Validate whether the current token is still valid (not expired).
    ///
    /// - Returns: `true` if the token exists and has not expired.
    public func validateToken() -> Bool {
        guard let claims else {
            isAuthenticated = false
            return false
        }

        let valid = claims.exp > Date()
        isAuthenticated = valid
        if !valid {
            currentRole = .listener
        }
        return valid
    }

    /// Generate the AUTH control message for relay authentication.
    ///
    /// - Returns: The formatted `AUTH:<token>` string, or an empty string if no token is set.
    public func authHeader() -> String {
        guard let rawToken else { return "" }
        return "\(Self.authPrefix)\(rawToken)\n"
    }

    /// Clear the current token and reset authentication state.
    public func clearToken() {
        rawToken = nil
        claims = nil
        isAuthenticated = false
        currentRole = .listener
        expiryTimer?.invalidate()
        expiryTimer = nil
    }

    /// Check if the current role has at least the specified privilege level.
    ///
    /// - Parameter requiredRole: The minimum role required.
    /// - Returns: `true` if the current role meets or exceeds the requirement.
    public func hasPermission(for requiredRole: ChannelRole) -> Bool {
        guard isAuthenticated else { return false }
        return currentRole >= requiredRole
    }

    // MARK: - JWT Decoding

    /// Decode a JWT token string into its claims.
    ///
    /// This performs minimal JWT parsing: splits on ".", Base64URL-decodes the
    /// payload, and extracts the expected fields. Signature verification is
    /// delegated to the relay server.
    ///
    /// - Parameter jwt: The raw JWT string.
    /// - Returns: Decoded `TokenClaims`, or `nil` if parsing fails.
    public static func decodeJWT(_ jwt: String) -> TokenClaims? {
        let parts = jwt.split(separator: ".")
        guard parts.count == 3 else { return nil }

        // Decode payload (second segment)
        guard let payloadData = base64URLDecode(String(parts[1])) else { return nil }

        guard let json = try? JSONSerialization.jsonObject(with: payloadData) as? [String: Any] else {
            return nil
        }

        // Extract required fields
        guard let channelId = json["channelId"] as? String ?? json["channel_id"] as? String,
              let userId = json["userId"] as? String ?? json["user_id"] as? String ?? json["sub"] as? String,
              let roleString = json["role"] as? String,
              let role = ChannelRole(rawValue: roleString),
              let expTimestamp = json["exp"] as? TimeInterval,
              let iatTimestamp = json["iat"] as? TimeInterval else {
            return nil
        }

        return TokenClaims(
            channelId: channelId,
            userId: userId,
            role: role,
            exp: Date(timeIntervalSince1970: expTimestamp),
            iat: Date(timeIntervalSince1970: iatTimestamp)
        )
    }

    // MARK: - Private Helpers

    /// Decode a Base64URL-encoded string to Data.
    private static func base64URLDecode(_ string: String) -> Data? {
        var base64 = string
            .replacingOccurrences(of: "-", with: "+")
            .replacingOccurrences(of: "_", with: "/")

        // Pad to multiple of 4
        let remainder = base64.count % 4
        if remainder != 0 {
            base64 += String(repeating: "=", count: 4 - remainder)
        }

        return Data(base64Encoded: base64)
    }

    /// Schedule a timer that fires 60 seconds before token expiration.
    private func scheduleExpiryWarning(expiration: Date) {
        expiryTimer?.invalidate()

        let warningTime = expiration.timeIntervalSinceNow - 60
        guard warningTime > 0 else {
            // Already within 60 seconds of expiry
            onTokenExpiring?(expiration.timeIntervalSinceNow)
            return
        }

        expiryTimer = Timer.scheduledTimer(withTimeInterval: warningTime, repeats: false) { [weak self] _ in
            guard let self else { return }
            let remaining = expiration.timeIntervalSinceNow
            self.onTokenExpiring?(remaining)
        }
    }
}
