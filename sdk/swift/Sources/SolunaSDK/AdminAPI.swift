import Foundation

/// A channel as returned by the admin API.
public struct AdminChannel: Codable, Sendable {
    public let name: String
    public let listenerCount: Int
    public let createdAt: String
    public let isActive: Bool

    public init(name: String, listenerCount: Int, createdAt: String, isActive: Bool) {
        self.name = name
        self.listenerCount = listenerCount
        self.createdAt = createdAt
        self.isActive = isActive
    }
}

/// A channel member as returned by the admin API.
public struct AdminMember: Codable, Sendable {
    public let deviceId: String
    public let deviceName: String
    public let joinedAt: String
    public let ipAddress: String

    public init(deviceId: String, deviceName: String, joinedAt: String, ipAddress: String) {
        self.deviceId = deviceId
        self.deviceName = deviceName
        self.joinedAt = joinedAt
        self.ipAddress = ipAddress
    }
}

/// Relay server statistics.
public struct RelayStats: Codable, Sendable {
    public let totalChannels: Int
    public let totalListeners: Int
    public let uptimeSeconds: Int
    public let packetsRelayed: UInt64
    public let bytesRelayed: UInt64

    public init(totalChannels: Int, totalListeners: Int, uptimeSeconds: Int, packetsRelayed: UInt64, bytesRelayed: UInt64) {
        self.totalChannels = totalChannels
        self.totalListeners = totalListeners
        self.uptimeSeconds = uptimeSeconds
        self.packetsRelayed = packetsRelayed
        self.bytesRelayed = bytesRelayed
    }
}

/// Configuration for creating a channel.
public struct ChannelConfig: Codable, Sendable {
    public var maxListeners: Int
    public var isPublic: Bool
    public var codec: String
    public var bitrate: Int

    public init(maxListeners: Int = 100, isPublic: Bool = true, codec: String = "opus", bitrate: Int = 128000) {
        self.maxListeners = maxListeners
        self.isPublic = isPublic
        self.codec = codec
        self.bitrate = bitrate
    }
}

/// REST API client for relay server administration.
///
/// Provides management operations such as listing/creating/deleting channels,
/// managing members, and retrieving server statistics.
///
/// Usage:
/// ```swift
/// let admin = AdminAPI(baseURL: "https://relay.solun.art", apiKey: "my-admin-key")
/// let channels = try await admin.listChannels()
/// let stats = try await admin.getStats()
/// ```
public final class AdminAPI {

    // MARK: - Configuration

    /// Base URL of the relay admin API.
    public let baseURL: String

    /// Admin API key for authentication.
    public let apiKey: String

    // MARK: - Private

    private let session: URLSession
    private let decoder = JSONDecoder()
    private let encoder = JSONEncoder()

    // MARK: - Init

    public init(baseURL: String, apiKey: String) {
        self.baseURL = baseURL.hasSuffix("/") ? String(baseURL.dropLast()) : baseURL
        self.apiKey = apiKey

        let config = URLSessionConfiguration.default
        config.timeoutIntervalForRequest = 15
        session = URLSession(configuration: config)

        decoder.keyDecodingStrategy = .convertFromSnakeCase
        encoder.keyEncodingStrategy = .convertToSnakeCase
    }

    // MARK: - Public API

    /// List all channels on the relay server.
    public func listChannels() async throws -> [AdminChannel] {
        return try await get("/api/admin/channels")
    }

    /// Create a new channel.
    ///
    /// - Parameters:
    ///   - name: Channel name.
    ///   - config: Channel configuration.
    public func createChannel(name: String, config: ChannelConfig) async throws {
        struct CreateRequest: Codable {
            let name: String
            let config: ChannelConfig
        }
        let body = CreateRequest(name: name, config: config)
        try await post("/api/admin/channels", body: body)
    }

    /// Delete a channel.
    ///
    /// - Parameter name: Channel name to delete.
    public func deleteChannel(name: String) async throws {
        try await delete("/api/admin/channels/\(name)")
    }

    /// List members of a channel.
    ///
    /// - Parameter channel: Channel name.
    public func listMembers(channel: String) async throws -> [AdminMember] {
        return try await get("/api/admin/channels/\(channel)/members")
    }

    /// Kick a member from a channel.
    ///
    /// - Parameters:
    ///   - channel: Channel name.
    ///   - deviceId: Device ID of the member to kick.
    public func kickMember(channel: String, deviceId: String) async throws {
        try await delete("/api/admin/channels/\(channel)/members/\(deviceId)")
    }

    /// Get relay server statistics.
    public func getStats() async throws -> RelayStats {
        return try await get("/api/admin/stats")
    }

    /// Set a remote configuration value.
    ///
    /// - Parameters:
    ///   - key: Configuration key.
    ///   - value: Configuration value.
    public func setRemoteConfig(key: String, value: String) async throws {
        struct ConfigRequest: Codable {
            let key: String
            let value: String
        }
        let body = ConfigRequest(key: key, value: value)
        try await post("/api/admin/config", body: body)
    }

    // MARK: - Private HTTP Helpers

    private func get<T: Decodable>(_ path: String) async throws -> T {
        let request = buildRequest(path: path, method: "GET")
        let (data, response) = try await session.data(for: request)
        try validateResponse(response)
        return try decoder.decode(T.self, from: data)
    }

    private func post<T: Encodable>(_ path: String, body: T) async throws {
        var request = buildRequest(path: path, method: "POST")
        request.httpBody = try encoder.encode(body)
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        let (_, response) = try await session.data(for: request)
        try validateResponse(response)
    }

    private func delete(_ path: String) async throws {
        let request = buildRequest(path: path, method: "DELETE")
        let (_, response) = try await session.data(for: request)
        try validateResponse(response)
    }

    private func buildRequest(path: String, method: String) -> URLRequest {
        let url = URL(string: "\(baseURL)\(path)")!
        var request = URLRequest(url: url)
        request.httpMethod = method
        request.setValue(apiKey, forHTTPHeaderField: "X-Admin-Key")
        request.setValue("SolunaSDK/1.0", forHTTPHeaderField: "User-Agent")
        return request
    }

    private func validateResponse(_ response: URLResponse) throws {
        guard let http = response as? HTTPURLResponse else {
            throw AdminAPIError.invalidResponse
        }
        guard (200..<300).contains(http.statusCode) else {
            throw AdminAPIError.httpError(statusCode: http.statusCode)
        }
    }
}

/// Errors from the Admin API.
public enum AdminAPIError: Error, LocalizedError {
    case invalidResponse
    case httpError(statusCode: Int)

    public var errorDescription: String? {
        switch self {
        case .invalidResponse:
            return "Invalid response from admin API"
        case .httpError(let code):
            return "Admin API returned HTTP \(code)"
        }
    }
}
