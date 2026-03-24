import Foundation

/// Severity level for audit log events.
public enum AuditLevel: String, Codable, Sendable, Comparable {
    case debug
    case info
    case warning
    case error
    case security

    private var order: Int {
        switch self {
        case .debug:    return 0
        case .info:     return 1
        case .warning:  return 2
        case .error:    return 3
        case .security: return 4
        }
    }

    public static func < (lhs: AuditLevel, rhs: AuditLevel) -> Bool {
        lhs.order < rhs.order
    }
}

/// Category for audit log events.
public enum AuditCategory: String, Codable, Sendable {
    case connection
    case auth
    case channel
    case error
    case security
}

/// A single audit log event.
public struct AuditEvent: Codable, Sendable {
    /// When the event occurred.
    public let timestamp: Date
    /// Severity level.
    public let level: AuditLevel
    /// Event category.
    public let category: AuditCategory
    /// Human-readable description of the event.
    public let message: String
    /// Additional key-value metadata.
    public let metadata: [String: String]

    public init(
        timestamp: Date = Date(),
        level: AuditLevel,
        category: AuditCategory,
        message: String,
        metadata: [String: String] = [:]
    ) {
        self.timestamp = timestamp
        self.level = level
        self.category = category
        self.message = message
        self.metadata = metadata
    }
}

/// Comprehensive audit logging for compliance and security monitoring.
///
/// Logs all connection events, authentication, channel access, and errors
/// to rotating log files stored in the app's documents directory. Supports
/// JSON and CSV export for compliance reporting.
///
/// Thread-safe: all writes are serialized on a dedicated dispatch queue.
///
/// Usage:
/// ```swift
/// AuditLogger.shared.log(AuditEvent(
///     level: .info,
///     category: .connection,
///     message: "Connected to relay",
///     metadata: ["host": "relay.solun.art", "channel": "jazz"]
/// ))
///
/// // Export logs for compliance
/// let jsonData = AuditLogger.shared.exportLogs(since: oneWeekAgo)
/// let csvString = AuditLogger.shared.exportCSV(since: oneWeekAgo)
/// ```
public final class AuditLogger: @unchecked Sendable {

    // MARK: - Singleton

    /// Shared audit logger instance.
    public static let shared = AuditLogger()

    // MARK: - Configuration

    /// Maximum total log size in megabytes before rotation (default 50 MB).
    public var maxLogSizeMB: Int = 50

    /// Number of days to retain log files (default 90).
    public var retentionDays: Int = 90

    /// Minimum level to log. Events below this level are discarded.
    public var minimumLevel: AuditLevel = .debug

    // MARK: - External Handler

    private var externalHandler: ((AuditEvent) -> Void)?

    /// Set an external handler for forwarding events to services like Sentry or Datadog.
    ///
    /// - Parameter handler: A closure called for each logged event, or `nil` to remove.
    public func setExternalHandler(_ handler: ((AuditEvent) -> Void)?) {
        queue.async { [weak self] in
            self?.externalHandler = handler
        }
    }

    // MARK: - Private

    private let queue = DispatchQueue(label: "com.soluna.auditlogger", qos: .utility)
    private let fileManager = FileManager.default
    private let logDirectory: URL
    private var currentLogFile: URL?
    private var currentFileHandle: FileHandle?
    private var currentFileSize: Int64 = 0

    private let encoder: JSONEncoder = {
        let enc = JSONEncoder()
        enc.dateEncodingStrategy = .iso8601
        enc.outputFormatting = [.sortedKeys]
        return enc
    }()

    private let decoder: JSONDecoder = {
        let dec = JSONDecoder()
        dec.dateDecodingStrategy = .iso8601
        return dec
    }()

    // MARK: - Init

    private init() {
        let documents = fileManager.urls(for: .documentDirectory, in: .userDomainMask).first!
        logDirectory = documents.appendingPathComponent("soluna-audit-logs", isDirectory: true)

        if !fileManager.fileExists(atPath: logDirectory.path) {
            try? fileManager.createDirectory(at: logDirectory, withIntermediateDirectories: true)
        }

        openCurrentLogFile()
    }

    deinit {
        currentFileHandle?.closeFile()
    }

    // MARK: - Public API

    /// Log an audit event.
    ///
    /// The event is written to the current log file as a JSON line and forwarded
    /// to any external handler.
    ///
    /// - Parameter event: The event to log.
    public func log(_ event: AuditEvent) {
        guard event.level >= minimumLevel else { return }

        queue.async { [weak self] in
            guard let self else { return }
            self.writeEvent(event)
            self.externalHandler?(event)
        }
    }

    /// Convenience method to log with inline parameters.
    ///
    /// - Parameters:
    ///   - level: Severity level.
    ///   - category: Event category.
    ///   - message: Description of the event.
    ///   - metadata: Additional key-value pairs.
    public func log(level: AuditLevel, category: AuditCategory, message: String, metadata: [String: String] = [:]) {
        log(AuditEvent(level: level, category: category, message: message, metadata: metadata))
    }

    /// Export all log events since the given date as JSON data.
    ///
    /// - Parameter since: The earliest date to include.
    /// - Returns: JSON-encoded array of `AuditEvent` objects.
    public func exportLogs(since: Date) -> Data {
        var events = [AuditEvent]()

        queue.sync {
            events = self.readEvents(since: since)
        }

        return (try? encoder.encode(events)) ?? Data("[]".utf8)
    }

    /// Export all log events since the given date as a CSV string.
    ///
    /// - Parameter since: The earliest date to include.
    /// - Returns: CSV-formatted string with header row.
    public func exportCSV(since: Date) -> String {
        var events = [AuditEvent]()

        queue.sync {
            events = self.readEvents(since: since)
        }

        let formatter = ISO8601DateFormatter()
        var csv = "timestamp,level,category,message,metadata\n"

        for event in events {
            let ts = formatter.string(from: event.timestamp)
            let metaString = event.metadata.map { "\($0.key)=\($0.value)" }.joined(separator: ";")
            let escapedMessage = event.message.replacingOccurrences(of: "\"", with: "\"\"")
            let escapedMeta = metaString.replacingOccurrences(of: "\"", with: "\"\"")
            csv += "\"\(ts)\",\"\(event.level.rawValue)\",\"\(event.category.rawValue)\",\"\(escapedMessage)\",\"\(escapedMeta)\"\n"
        }

        return csv
    }

    /// Delete all log files.
    public func clearLogs() {
        queue.async { [weak self] in
            guard let self else { return }
            self.currentFileHandle?.closeFile()
            self.currentFileHandle = nil
            self.currentLogFile = nil

            try? self.fileManager.removeItem(at: self.logDirectory)
            try? self.fileManager.createDirectory(at: self.logDirectory, withIntermediateDirectories: true)

            self.currentFileSize = 0
            self.openCurrentLogFile()
        }
    }

    // MARK: - Private — File Management

    private func openCurrentLogFile() {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyy-MM-dd"
        let dateString = formatter.string(from: Date())
        let fileURL = logDirectory.appendingPathComponent("audit-\(dateString).jsonl")

        if !fileManager.fileExists(atPath: fileURL.path) {
            fileManager.createFile(atPath: fileURL.path, contents: nil)
        }

        currentLogFile = fileURL
        currentFileHandle = FileHandle(forWritingAtPath: fileURL.path)
        currentFileHandle?.seekToEndOfFile()
        currentFileSize = Int64(currentFileHandle?.offsetInFile ?? 0)
    }

    private func writeEvent(_ event: AuditEvent) {
        guard let data = try? encoder.encode(event) else { return }

        var line = data
        line.append(contentsOf: [0x0A]) // newline

        // Rotate if needed
        let maxBytes = Int64(maxLogSizeMB) * 1024 * 1024
        if currentFileSize + Int64(line.count) > maxBytes {
            rotateLogFile()
        }

        // Ensure we have the correct day's file
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyy-MM-dd"
        let today = formatter.string(from: Date())
        let expectedFile = "audit-\(today).jsonl"
        if currentLogFile?.lastPathComponent != expectedFile {
            currentFileHandle?.closeFile()
            openCurrentLogFile()
        }

        currentFileHandle?.write(line)
        currentFileSize += Int64(line.count)

        // Evict old files
        evictOldLogs()
    }

    private func rotateLogFile() {
        currentFileHandle?.closeFile()

        // Rename current file with timestamp suffix
        if let current = currentLogFile, fileManager.fileExists(atPath: current.path) {
            let rotated = current.deletingPathExtension()
                .appendingPathExtension("\(Int(Date().timeIntervalSince1970)).jsonl")
            try? fileManager.moveItem(at: current, to: rotated)
        }

        openCurrentLogFile()
    }

    private func evictOldLogs() {
        let cutoff = Date().addingTimeInterval(-Double(retentionDays) * 86400)

        guard let files = try? fileManager.contentsOfDirectory(
            at: logDirectory,
            includingPropertiesForKeys: [.creationDateKey],
            options: [.skipsHiddenFiles]
        ) else { return }

        for file in files {
            guard let attrs = try? file.resourceValues(forKeys: [.creationDateKey]),
                  let created = attrs.creationDate,
                  created < cutoff else { continue }
            try? fileManager.removeItem(at: file)
        }
    }

    // MARK: - Private — Reading

    private func readEvents(since: Date) -> [AuditEvent] {
        guard let files = try? fileManager.contentsOfDirectory(
            at: logDirectory,
            includingPropertiesForKeys: [.creationDateKey],
            options: [.skipsHiddenFiles]
        ) else {
            return []
        }

        var events = [AuditEvent]()

        for file in files.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
            guard file.pathExtension == "jsonl",
                  let content = try? String(contentsOf: file, encoding: .utf8) else { continue }

            let lines = content.split(separator: "\n")
            for line in lines {
                guard let data = line.data(using: .utf8),
                      let event = try? decoder.decode(AuditEvent.self, from: data),
                      event.timestamp >= since else { continue }
                events.append(event)
            }
        }

        return events.sorted { $0.timestamp < $1.timestamp }
    }
}
