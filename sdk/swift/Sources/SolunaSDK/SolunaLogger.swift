import Foundation
import os

// MARK: - Log Level

/// Severity levels for SDK logging.
public enum LogLevel: Int, Comparable, Sendable {
    case verbose = 0
    case debug   = 1
    case info    = 2
    case warning = 3
    case error   = 4
    case none    = 5

    public static func < (lhs: LogLevel, rhs: LogLevel) -> Bool {
        lhs.rawValue < rhs.rawValue
    }

    /// Human-readable label.
    var label: String {
        switch self {
        case .verbose: return "VERBOSE"
        case .debug:   return "DEBUG"
        case .info:    return "INFO"
        case .warning: return "WARNING"
        case .error:   return "ERROR"
        case .none:    return "NONE"
        }
    }

    /// Corresponding `OSLogType`.
    var osLogType: OSLogType {
        switch self {
        case .verbose: return .debug
        case .debug:   return .debug
        case .info:    return .info
        case .warning: return .default
        case .error:   return .error
        case .none:    return .info
        }
    }
}

// MARK: - Log Category

/// Subsystem categories for filtering SDK log output.
public enum LogCategory: String, Sendable, CaseIterable {
    case connection  = "connection"
    case audio       = "audio"
    case p2p         = "p2p"
    case encryption  = "encryption"
    case sync        = "sync"
    case cache       = "cache"
    case billing     = "billing"
    case general     = "general"
}

// MARK: - Log Entry

/// A single structured log entry.
public struct LogEntry: Sendable {
    /// When the log was recorded.
    public let timestamp: Date
    /// Severity level.
    public let level: LogLevel
    /// Subsystem category.
    public let category: LogCategory
    /// Log message.
    public let message: String
    /// Source file that produced the log.
    public let file: String
    /// Source line number.
    public let line: Int
}

// MARK: - SolunaLogger

/// Unified logging system for the Soluna SDK.
///
/// Built on Apple's `os_log` subsystem. Supports console, file, and custom
/// external sinks. Verbose logging is compiled out in release builds.
///
/// Usage:
/// ```swift
/// SolunaLogger.shared.logLevel = .debug
/// SolunaLogger.shared.log(.info, .audio, "Playback started")
///
/// // Convenience shorthand:
/// SolunaLogger.debug(.connection, "Heartbeat sent")
/// SolunaLogger.error(.audio, "Buffer underrun")
/// ```
public final class SolunaLogger: @unchecked Sendable {

    // MARK: - Singleton

    /// Shared logger instance.
    public static let shared = SolunaLogger()

    // MARK: - Configuration

    /// Minimum level to emit. Messages below this level are discarded.
    #if DEBUG
    public var logLevel: LogLevel = .debug
    #else
    public var logLevel: LogLevel = .info
    #endif

    /// Whether console output via `os_log` is enabled.
    public var isConsoleEnabled: Bool = true

    // MARK: - Private

    private let queue = DispatchQueue(label: "com.soluna.logger", qos: .utility)
    private let subsystem = "com.soluna.sdk"
    private var osLoggers: [LogCategory: OSLog] = [:]
    private var fileHandle: FileHandle?
    private var fileURL: URL?
    private var externalSink: ((LogEntry) -> Void)?

    private let dateFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd HH:mm:ss.SSS"
        f.locale = Locale(identifier: "en_US_POSIX")
        return f
    }()

    // MARK: - Init

    private init() {
        for category in LogCategory.allCases {
            osLoggers[category] = OSLog(subsystem: subsystem, category: category.rawValue)
        }
    }

    deinit {
        fileHandle?.closeFile()
    }

    // MARK: - Public API

    /// Log a message at the given level and category.
    ///
    /// - Parameters:
    ///   - level: Severity level.
    ///   - category: Subsystem category.
    ///   - message: The log message.
    ///   - file: Source file (auto-filled).
    ///   - line: Source line (auto-filled).
    public func log(
        _ level: LogLevel,
        _ category: LogCategory,
        _ message: String,
        file: String = #fileID,
        line: Int = #line
    ) {
        guard level >= logLevel else { return }

        #if !DEBUG
        // Strip verbose logs entirely in release builds
        if level == .verbose { return }
        #endif

        let entry = LogEntry(
            timestamp: Date(),
            level: level,
            category: category,
            message: message,
            file: file,
            line: line
        )

        // os_log on current thread for immediacy
        if isConsoleEnabled, let logger = osLoggers[category] {
            os_log(
                "%{public}@",
                log: logger,
                type: level.osLogType,
                "[\(level.label)] \(message)"
            )
        }

        // File and external sink on background queue
        queue.async { [weak self] in
            self?.writeToFile(entry)
            self?.externalSink?(entry)
        }
    }

    /// Enable logging to a file in the specified directory.
    ///
    /// Creates a file named `soluna-sdk.log` in the given directory.
    /// Subsequent log entries will be appended to this file.
    ///
    /// - Parameter directory: The directory URL where the log file is created.
    public func enableFileLogging(directory: URL) {
        queue.async { [weak self] in
            guard let self else { return }
            self.fileHandle?.closeFile()

            let url = directory.appendingPathComponent("soluna-sdk.log")
            let fm = FileManager.default

            if !fm.fileExists(atPath: directory.path) {
                try? fm.createDirectory(at: directory, withIntermediateDirectories: true)
            }
            if !fm.fileExists(atPath: url.path) {
                fm.createFile(atPath: url.path, contents: nil)
            }

            self.fileURL = url
            self.fileHandle = FileHandle(forWritingAtPath: url.path)
            self.fileHandle?.seekToEndOfFile()
        }
    }

    /// Enable or disable console logging via `os_log`.
    ///
    /// - Parameter enabled: Whether console output should be active.
    public func enableConsoleLogging(_ enabled: Bool) {
        isConsoleEnabled = enabled
    }

    /// Set an external sink for forwarding log entries to a custom destination.
    ///
    /// - Parameter sink: A closure that receives each `LogEntry`, or `nil` to remove.
    public func setExternalSink(_ sink: ((LogEntry) -> Void)?) {
        queue.async { [weak self] in
            self?.externalSink = sink
        }
    }

    // MARK: - Convenience Class Methods

    /// Log a verbose message.
    public static func verbose(_ category: LogCategory, _ message: String, file: String = #fileID, line: Int = #line) {
        shared.log(.verbose, category, message, file: file, line: line)
    }

    /// Log a debug message.
    public static func debug(_ category: LogCategory, _ message: String, file: String = #fileID, line: Int = #line) {
        shared.log(.debug, category, message, file: file, line: line)
    }

    /// Log an info message.
    public static func info(_ category: LogCategory, _ message: String, file: String = #fileID, line: Int = #line) {
        shared.log(.info, category, message, file: file, line: line)
    }

    /// Log a warning message.
    public static func warning(_ category: LogCategory, _ message: String, file: String = #fileID, line: Int = #line) {
        shared.log(.warning, category, message, file: file, line: line)
    }

    /// Log an error message.
    public static func error(_ category: LogCategory, _ message: String, file: String = #fileID, line: Int = #line) {
        shared.log(.error, category, message, file: file, line: line)
    }

    // MARK: - Private — File Writing

    private func writeToFile(_ entry: LogEntry) {
        guard let handle = fileHandle else { return }
        let ts = dateFormatter.string(from: entry.timestamp)
        let line = "\(ts) [\(entry.level.label)] [\(entry.category.rawValue)] \(entry.message) (\(entry.file):\(entry.line))\n"
        if let data = line.data(using: .utf8) {
            handle.write(data)
        }
    }
}
