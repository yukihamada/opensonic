import Foundation
import Combine

/// Server-driven SDK configuration fetched from the relay.
///
/// Fetches and caches configuration values from the relay server,
/// allowing runtime tuning without app updates.
///
/// Usage:
/// ```swift
/// let config = RemoteConfig()
/// await config.fetch()
/// let bufferMs = config.getInt("bufferMs") ?? 200
/// ```
public final class RemoteConfig: ObservableObject {

    // MARK: - Published State

    /// When the configuration was last successfully fetched.
    @Published public private(set) var lastFetchedAt: Date?

    // MARK: - Callbacks

    /// Called when configuration values change after a fetch.
    public var onConfigChanged: (([String: String]) -> Void)?

    // MARK: - Configuration

    /// The URL to fetch configuration from.
    public var configURL: String = "https://relay.solun.art/api/config"

    // MARK: - Private

    private var values: [String: String] = [:]
    private let session: URLSession
    private let cacheKey = "com.soluna.remoteconfig.values"
    private let queue = DispatchQueue(label: "com.soluna.remoteconfig", qos: .utility)

    // MARK: - Init

    public init() {
        let config = URLSessionConfiguration.default
        config.timeoutIntervalForRequest = 10
        session = URLSession(configuration: config)

        // Load cached values from UserDefaults
        if let cached = UserDefaults.standard.dictionary(forKey: cacheKey) as? [String: String] {
            values = cached
        }
    }

    // MARK: - Public API

    /// Fetch configuration from the relay server.
    public func fetch() async {
        guard let url = URL(string: configURL) else {
            print("[SolunaSDK] Invalid remote config URL: \(configURL)")
            return
        }

        var request = URLRequest(url: url)
        request.setValue("SolunaSDK/1.0", forHTTPHeaderField: "User-Agent")
        request.setValue("application/json", forHTTPHeaderField: "Accept")

        do {
            let (data, response) = try await session.data(for: request)

            guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode) else {
                print("[SolunaSDK] Remote config fetch failed")
                return
            }

            guard let newValues = try? JSONDecoder().decode([String: String].self, from: data) else {
                print("[SolunaSDK] Remote config parse failed")
                return
            }

            let changed = newValues != values

            queue.sync {
                values = newValues
            }

            // Cache to UserDefaults
            UserDefaults.standard.set(newValues, forKey: cacheKey)

            await MainActor.run {
                lastFetchedAt = Date()
                if changed {
                    onConfigChanged?(newValues)
                }
            }
        } catch {
            print("[SolunaSDK] Remote config fetch error: \(error.localizedDescription)")
        }
    }

    /// Get a string configuration value.
    ///
    /// - Parameter key: The configuration key.
    /// - Returns: The value, or nil if not set.
    public func get(_ key: String) -> String? {
        queue.sync { values[key] }
    }

    /// Get an integer configuration value.
    ///
    /// - Parameter key: The configuration key.
    /// - Returns: The integer value, or nil if not set or not parseable.
    public func getInt(_ key: String) -> Int? {
        guard let str = get(key) else { return nil }
        return Int(str)
    }

    /// Get a boolean configuration value.
    ///
    /// - Parameter key: The configuration key.
    /// - Returns: The boolean value, or nil if not set.
    public func getBool(_ key: String) -> Bool? {
        guard let str = get(key) else { return nil }
        switch str.lowercased() {
        case "true", "1", "yes": return true
        case "false", "0", "no": return false
        default: return nil
        }
    }

    /// Get a float configuration value.
    ///
    /// - Parameter key: The configuration key.
    /// - Returns: The float value, or nil if not set or not parseable.
    public func getFloat(_ key: String) -> Float? {
        guard let str = get(key) else { return nil }
        return Float(str)
    }
}
