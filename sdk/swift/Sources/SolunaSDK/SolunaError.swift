import Foundation

// MARK: - Unified Error Type

/// Unified error type for all SolunaSDK operations.
///
/// Each case carries context to aid debugging. Conforms to `LocalizedError`
/// so that `.localizedDescription` returns a human-readable message suitable
/// for display or logging.
public enum SolunaError: Error, LocalizedError, Sendable {
    /// The relay connection could not be established.
    case connectionFailed(String)

    /// Authentication with the relay server failed.
    case authenticationFailed(String)

    /// The requested channel does not exist.
    case channelNotFound(String)

    /// The audio codec is not supported on this platform.
    case codecNotSupported(String)

    /// An encryption or decryption operation failed.
    case encryptionError(String)

    /// No network connectivity is available.
    case networkUnavailable

    /// An operation exceeded the allowed time.
    case timeout(TimeInterval)

    /// The relay server returned an error.
    case serverError(Int, String)

    /// The user lacks permission for the requested action.
    case permissionDenied(String)

    /// Usage quota has been exceeded.
    case quotaExceeded

    /// SDK configuration is invalid.
    case invalidConfiguration(String)

    /// An unexpected internal error occurred.
    case internalError(String)

    // MARK: - LocalizedError

    public var errorDescription: String? {
        switch self {
        case .connectionFailed(let reason):
            return "Connection failed: \(reason)"
        case .authenticationFailed(let reason):
            return "Authentication failed: \(reason)"
        case .channelNotFound(let channel):
            return "Channel not found: \(channel)"
        case .codecNotSupported(let codec):
            return "Codec not supported: \(codec)"
        case .encryptionError(let detail):
            return "Encryption error: \(detail)"
        case .networkUnavailable:
            return "Network unavailable"
        case .timeout(let interval):
            return "Operation timed out after \(String(format: "%.1f", interval))s"
        case .serverError(let code, let message):
            return "Server error \(code): \(message)"
        case .permissionDenied(let reason):
            return "Permission denied: \(reason)"
        case .quotaExceeded:
            return "Usage quota exceeded"
        case .invalidConfiguration(let detail):
            return "Invalid configuration: \(detail)"
        case .internalError(let detail):
            return "Internal error: \(detail)"
        }
    }
}

// MARK: - Retry Policy

/// Retry strategy for recoverable errors.
public enum RetryPolicy: Sendable {
    /// Do not retry.
    case none

    /// Retry at a fixed interval.
    ///
    /// - Parameters:
    ///   - interval: Seconds between retries.
    ///   - maxRetries: Maximum number of retry attempts.
    case fixed(interval: TimeInterval, maxRetries: Int)

    /// Retry with exponential backoff.
    ///
    /// - Parameters:
    ///   - base: Base interval in seconds (doubled each attempt).
    ///   - max: Maximum interval cap in seconds.
    ///   - maxRetries: Maximum number of retry attempts.
    case exponentialBackoff(base: TimeInterval, max: TimeInterval, maxRetries: Int)

    /// Compute the delay for a given attempt index (0-based).
    ///
    /// Returns `nil` if the attempt exceeds `maxRetries`.
    public func delay(for attempt: Int) -> TimeInterval? {
        switch self {
        case .none:
            return nil
        case .fixed(let interval, let maxRetries):
            return attempt < maxRetries ? interval : nil
        case .exponentialBackoff(let base, let max, let maxRetries):
            guard attempt < maxRetries else { return nil }
            let delay = min(base * pow(2.0, Double(attempt)), max)
            return delay
        }
    }
}

// MARK: - Error Handler

/// Centralized error handler with retry support.
///
/// Attach an `onError` closure to observe all SDK errors. The handler
/// evaluates `retryPolicy` and `shouldRetry(_:)` to decide whether
/// a failed operation should be retried automatically.
///
/// Usage:
/// ```swift
/// let handler = SolunaErrorHandler()
/// handler.retryPolicy = .exponentialBackoff(base: 1, max: 30, maxRetries: 5)
/// handler.onError = { error in
///     print("SDK error: \(error.localizedDescription)")
/// }
/// ```
public final class SolunaErrorHandler: @unchecked Sendable {

    /// Called whenever an error is reported.
    public var onError: ((SolunaError) -> Void)?

    /// The retry policy applied to retryable errors.
    public var retryPolicy: RetryPolicy = .none

    public init() {}

    /// Determine whether the given error is eligible for retry.
    ///
    /// Network-related and transient server errors return `true`.
    /// Permanent failures (auth, permission, config) return `false`.
    public func shouldRetry(_ error: SolunaError) -> Bool {
        switch error {
        case .connectionFailed,
             .networkUnavailable,
             .timeout:
            return true
        case .serverError(let code, _):
            // 5xx errors are retryable; 4xx are not
            return code >= 500
        case .authenticationFailed,
             .channelNotFound,
             .codecNotSupported,
             .encryptionError,
             .permissionDenied,
             .quotaExceeded,
             .invalidConfiguration,
             .internalError:
            return false
        }
    }

    /// Report an error. Invokes `onError` on the calling thread.
    ///
    /// - Parameter error: The error to report.
    public func handle(_ error: SolunaError) {
        onError?(error)
    }
}
