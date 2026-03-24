import Foundation
import Combine

/// Export format for metrics.
public enum ExportFormat: String, Sendable {
    case prometheus
    case json
    case datadog
}

/// Exports SDK metrics in Prometheus, JSON, or Datadog formats.
///
/// Collects and exports runtime metrics such as connection duration,
/// packet counts, latency, and listener counts for monitoring dashboards.
///
/// Usage:
/// ```swift
/// let exporter = MetricsExporter()
/// exporter.format = .prometheus
/// let output = exporter.export()
/// // or push to a metrics endpoint
/// try await exporter.pushTo(url: "https://metrics.example.com/push")
/// ```
public final class MetricsExporter: ObservableObject {

    // MARK: - Configuration

    /// Output format for exported metrics.
    public var format: ExportFormat = .prometheus

    // MARK: - Metrics Storage

    /// Connection duration in seconds.
    public var connectionDurationSeconds: Double = 0

    /// Total packets received.
    public var packetsReceivedTotal: UInt64 = 0

    /// Packet loss ratio (0.0 - 1.0).
    public var packetLossRatio: Double = 0

    /// Audio latency in milliseconds.
    public var audioLatencyMs: Double = 0

    /// Total codec switches.
    public var codecSwitchesTotal: UInt64 = 0

    /// Total reconnections.
    public var reconnectsTotal: UInt64 = 0

    /// Current number of active listeners.
    public var activeListeners: Int = 0

    /// Current channel name (used as label).
    public var channel: String = ""

    // MARK: - Private

    private let session: URLSession
    private var autoExportTask: Task<Void, Never>?

    // MARK: - Init

    public init() {
        let config = URLSessionConfiguration.default
        config.timeoutIntervalForRequest = 10
        session = URLSession(configuration: config)
    }

    deinit {
        autoExportTask?.cancel()
    }

    // MARK: - Public API

    /// Export all metrics as a formatted string.
    ///
    /// - Returns: Metrics string in the configured format.
    public func export() -> String {
        switch format {
        case .prometheus:
            return exportPrometheus()
        case .json:
            return exportJSON()
        case .datadog:
            return exportDatadog()
        }
    }

    /// Push metrics to a remote endpoint via HTTP POST.
    ///
    /// - Parameter url: The metrics endpoint URL.
    public func pushTo(url: String) async throws {
        guard let endpoint = URL(string: url) else {
            throw MetricsExporterError.invalidURL
        }

        var request = URLRequest(url: endpoint)
        request.httpMethod = "POST"
        request.setValue("SolunaSDK/1.0", forHTTPHeaderField: "User-Agent")

        let body = export()
        request.httpBody = body.data(using: .utf8)

        switch format {
        case .prometheus:
            request.setValue("text/plain; version=0.0.4", forHTTPHeaderField: "Content-Type")
        case .json, .datadog:
            request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        }

        let (_, response) = try await session.data(for: request)
        if let http = response as? HTTPURLResponse, http.statusCode >= 400 {
            throw MetricsExporterError.pushFailed(statusCode: http.statusCode)
        }
    }

    /// Start automatic periodic metrics export.
    ///
    /// - Parameters:
    ///   - url: The metrics endpoint URL.
    ///   - intervalSeconds: Seconds between exports (minimum 5).
    public func startAutoExport(url: String, intervalSeconds: Int) {
        stopAutoExport()
        let interval = max(intervalSeconds, 5)

        autoExportTask = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: UInt64(interval) * 1_000_000_000)
                guard !Task.isCancelled, let self else { return }
                try? await self.pushTo(url: url)
            }
        }
    }

    /// Stop automatic metrics export.
    public func stopAutoExport() {
        autoExportTask?.cancel()
        autoExportTask = nil
    }

    // MARK: - Private Formatters

    private func exportPrometheus() -> String {
        let labels = channel.isEmpty ? "" : "{channel=\"\(channel)\"}"
        var lines: [String] = []

        lines.append("# HELP soluna_connection_duration_seconds Total connection duration")
        lines.append("# TYPE soluna_connection_duration_seconds counter")
        lines.append("soluna_connection_duration_seconds\(labels) \(connectionDurationSeconds)")

        lines.append("# HELP soluna_packets_received_total Total packets received")
        lines.append("# TYPE soluna_packets_received_total counter")
        lines.append("soluna_packets_received_total\(labels) \(packetsReceivedTotal)")

        lines.append("# HELP soluna_packet_loss_ratio Packet loss ratio")
        lines.append("# TYPE soluna_packet_loss_ratio gauge")
        lines.append("soluna_packet_loss_ratio\(labels) \(packetLossRatio)")

        lines.append("# HELP soluna_audio_latency_ms Audio latency in milliseconds")
        lines.append("# TYPE soluna_audio_latency_ms gauge")
        lines.append("soluna_audio_latency_ms\(labels) \(audioLatencyMs)")

        lines.append("# HELP soluna_codec_switches_total Total codec switches")
        lines.append("# TYPE soluna_codec_switches_total counter")
        lines.append("soluna_codec_switches_total\(labels) \(codecSwitchesTotal)")

        lines.append("# HELP soluna_reconnects_total Total reconnections")
        lines.append("# TYPE soluna_reconnects_total counter")
        lines.append("soluna_reconnects_total\(labels) \(reconnectsTotal)")

        lines.append("# HELP soluna_active_listeners Current active listeners")
        lines.append("# TYPE soluna_active_listeners gauge")
        lines.append("soluna_active_listeners\(labels) \(activeListeners)")

        return lines.joined(separator: "\n") + "\n"
    }

    private func exportJSON() -> String {
        let metrics: [String: Any] = [
            "timestamp": ISO8601DateFormatter().string(from: Date()),
            "channel": channel,
            "connection_duration_seconds": connectionDurationSeconds,
            "packets_received_total": packetsReceivedTotal,
            "packet_loss_ratio": packetLossRatio,
            "audio_latency_ms": audioLatencyMs,
            "codec_switches_total": codecSwitchesTotal,
            "reconnects_total": reconnectsTotal,
            "active_listeners": activeListeners
        ]

        guard let data = try? JSONSerialization.data(withJSONObject: metrics, options: [.prettyPrinted, .sortedKeys]),
              let str = String(data: data, encoding: .utf8) else {
            return "{}"
        }
        return str
    }

    private func exportDatadog() -> String {
        let timestamp = Int(Date().timeIntervalSince1970)
        let tags = channel.isEmpty ? "[]" : "[\"channel:\(channel)\"]"

        let series: [[String: Any]] = [
            ["metric": "soluna.connection_duration", "points": [[timestamp, connectionDurationSeconds]], "type": "count", "tags": tags],
            ["metric": "soluna.packets_received", "points": [[timestamp, packetsReceivedTotal]], "type": "count", "tags": tags],
            ["metric": "soluna.packet_loss_ratio", "points": [[timestamp, packetLossRatio]], "type": "gauge", "tags": tags],
            ["metric": "soluna.audio_latency_ms", "points": [[timestamp, audioLatencyMs]], "type": "gauge", "tags": tags],
            ["metric": "soluna.active_listeners", "points": [[timestamp, activeListeners]], "type": "gauge", "tags": tags],
        ]

        let payload: [String: Any] = ["series": series]

        guard let data = try? JSONSerialization.data(withJSONObject: payload, options: []),
              let str = String(data: data, encoding: .utf8) else {
            return "{}"
        }
        return str
    }
}

/// Errors from the metrics exporter.
public enum MetricsExporterError: Error, LocalizedError {
    case invalidURL
    case pushFailed(statusCode: Int)

    public var errorDescription: String? {
        switch self {
        case .invalidURL:
            return "Invalid metrics endpoint URL"
        case .pushFailed(let code):
            return "Metrics push failed with HTTP \(code)"
        }
    }
}
