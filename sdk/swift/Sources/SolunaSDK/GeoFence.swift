import Foundation

/// Result of a geographic access check.
public enum GeoFenceResult: Sendable {
    /// Access is allowed from this region.
    case allowed(region: String)
    /// Access is blocked from this region.
    case blocked(region: String)
    /// Region could not be determined.
    case unknown
}

/// IP/region-based access control for Soluna relay connections.
///
/// Restricts relay connections based on geographic location using the device
/// locale and an optional IP geolocation API lookup. Configure allowed or
/// blocked region sets using ISO 3166-1 alpha-2 country codes.
///
/// Usage:
/// ```swift
/// let geoFence = GeoFence()
/// geoFence.allowedRegions = ["JP", "US", "GB", "DE"]
///
/// let result = await geoFence.checkAccess()
/// switch result {
/// case .allowed(let region):
///     print("Access granted from \(region)")
/// case .blocked(let region):
///     print("Access blocked from \(region)")
/// case .unknown:
///     print("Could not determine region")
/// }
/// ```
public final class GeoFence: ObservableObject {

    // MARK: - Published State

    /// The currently detected region (ISO 3166-1 alpha-2 code), or `nil` if unknown.
    @Published public private(set) var currentRegion: String?

    // MARK: - Configuration

    /// Set of allowed country codes (ISO 3166-1 alpha-2).
    /// If non-empty, only these regions are permitted. Takes precedence over `blockedRegions`.
    public var allowedRegions: Set<String> = []

    /// Set of blocked country codes (ISO 3166-1 alpha-2).
    /// Only checked if `allowedRegions` is empty.
    public var blockedRegions: Set<String> = []

    /// Whether to use the IP geolocation API for region detection.
    /// When `false`, only the device locale is used.
    public var useIPGeolocation: Bool = true

    /// URL for the IP geolocation API.
    public var geolocationURL: URL = URL(string: "https://ipapi.co/json/")!

    /// Timeout for the IP geolocation API request in seconds.
    public var geolocationTimeout: TimeInterval = 5.0

    // MARK: - Computed

    /// Whether access is currently allowed based on the last known region.
    ///
    /// Returns `true` if no region restrictions are configured or if the current
    /// region passes the allow/block check.
    public var isAllowed: Bool {
        guard !allowedRegions.isEmpty || !blockedRegions.isEmpty else { return true }
        guard let region = currentRegion else { return true }
        return evaluateAccess(region: region)
    }

    // MARK: - Init

    public init() {
        // Initialize with device locale
        currentRegion = deviceLocaleRegion()
    }

    // MARK: - Public API

    /// Check geographic access by detecting the current region.
    ///
    /// First checks the device locale. If `useIPGeolocation` is enabled, also
    /// queries the IP geolocation API for a more accurate result.
    ///
    /// - Returns: The access result indicating whether the region is allowed, blocked, or unknown.
    public func checkAccess() async -> GeoFenceResult {
        // Start with device locale
        var region = deviceLocaleRegion()

        // Try IP geolocation for more accurate result
        if useIPGeolocation {
            if let ipRegion = await fetchIPRegion() {
                region = ipRegion
            }
        }

        if let region {
            currentRegion = region

            if evaluateAccess(region: region) {
                return .allowed(region: region)
            } else {
                return .blocked(region: region)
            }
        }

        return .unknown
    }

    /// Manually set the current region (for testing or when region is known).
    ///
    /// - Parameter region: ISO 3166-1 alpha-2 country code.
    public func setRegion(_ region: String) {
        currentRegion = region.uppercased()
    }

    // MARK: - Private

    /// Get the region from the device's current locale.
    private func deviceLocaleRegion() -> String? {
        if #available(iOS 16, macOS 13, *) {
            return Locale.current.region?.identifier
        } else {
            return Locale.current.regionCode
        }
    }

    /// Evaluate whether a region passes the allow/block rules.
    private func evaluateAccess(region: String) -> Bool {
        let upperRegion = region.uppercased()

        if !allowedRegions.isEmpty {
            // Allowlist mode: only listed regions are permitted
            return allowedRegions.contains(upperRegion)
        }

        if !blockedRegions.isEmpty {
            // Blocklist mode: listed regions are denied
            return !blockedRegions.contains(upperRegion)
        }

        // No restrictions configured
        return true
    }

    /// Fetch the country code from an IP geolocation API.
    private func fetchIPRegion() async -> String? {
        var request = URLRequest(url: geolocationURL)
        request.timeoutInterval = geolocationTimeout
        request.setValue("application/json", forHTTPHeaderField: "Accept")

        do {
            let (data, response) = try await URLSession.shared.data(for: request)

            guard let httpResponse = response as? HTTPURLResponse,
                  httpResponse.statusCode == 200 else {
                return nil
            }

            guard let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let countryCode = json["country_code"] as? String ?? json["country"] as? String else {
                return nil
            }

            return countryCode.uppercased()
        } catch {
            return nil
        }
    }
}
