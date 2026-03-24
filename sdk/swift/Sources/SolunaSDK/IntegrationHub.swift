import Foundation
import Combine

/// Events that can trigger integrations.
public enum IntegrationEvent: String, Sendable, Hashable, CaseIterable {
    case trackChanged
    case listenerJoined
    case listenerLeft
    case channelCreated
    case error
}

/// Type of external integration.
public enum IntegrationType: String, Sendable {
    case slack
    case webhook
    case mqtt
}

/// An external service integration configuration.
public struct Integration: Identifiable, Sendable {
    public let id: String
    public let type: IntegrationType
    public let config: [String: String]
    public let enabledEvents: Set<IntegrationEvent>

    public init(id: String = UUID().uuidString, type: IntegrationType, config: [String: String], enabledEvents: Set<IntegrationEvent>) {
        self.id = id
        self.type = type
        self.config = config
        self.enabledEvents = enabledEvents
    }
}

/// External service integrations hub for Slack, webhooks, and MQTT.
///
/// Dispatches Soluna events to configured external services via HTTP POST
/// (Slack/webhooks) or MQTT publish.
///
/// Usage:
/// ```swift
/// let hub = IntegrationHub()
/// hub.addSlackWebhook(url: "https://hooks.slack.com/...", events: [.trackChanged, .error])
/// hub.dispatch(event: .trackChanged, payload: ["track": "Jazz Standard #5"])
/// ```
public final class IntegrationHub: ObservableObject {

    // MARK: - Published State

    /// All configured integrations.
    @Published public private(set) var integrations: [Integration] = []

    // MARK: - Private

    private let session: URLSession
    private let queue = DispatchQueue(label: "com.soluna.integrations", qos: .utility)

    // MARK: - Init

    public init() {
        let config = URLSessionConfiguration.default
        config.timeoutIntervalForRequest = 10
        session = URLSession(configuration: config)
    }

    // MARK: - Public API

    /// Add a Slack incoming webhook integration.
    ///
    /// - Parameters:
    ///   - url: The Slack webhook URL.
    ///   - events: Events that should trigger this webhook.
    /// - Returns: The integration ID.
    @discardableResult
    public func addSlackWebhook(url: String, events: Set<IntegrationEvent>) -> String {
        let integration = Integration(
            type: .slack,
            config: ["url": url],
            enabledEvents: events
        )
        integrations.append(integration)
        return integration.id
    }

    /// Add a generic HTTP webhook integration.
    ///
    /// - Parameters:
    ///   - url: The webhook URL.
    ///   - events: Events that should trigger this webhook.
    ///   - headers: Additional HTTP headers to include.
    /// - Returns: The integration ID.
    @discardableResult
    public func addGenericWebhook(url: String, events: Set<IntegrationEvent>, headers: [String: String] = [:]) -> String {
        var config = headers
        config["url"] = url
        let integration = Integration(
            type: .webhook,
            config: config,
            enabledEvents: events
        )
        integrations.append(integration)
        return integration.id
    }

    /// Add an MQTT broker integration (stub — sends via HTTP POST to bridge).
    ///
    /// - Parameters:
    ///   - host: MQTT broker hostname.
    ///   - port: MQTT broker port.
    ///   - topic: MQTT topic to publish to.
    /// - Returns: The integration ID.
    @discardableResult
    public func addMQTTBroker(host: String, port: UInt16, topic: String) -> String {
        let integration = Integration(
            type: .mqtt,
            config: [
                "host": host,
                "port": String(port),
                "topic": topic
            ],
            enabledEvents: Set(IntegrationEvent.allCases)
        )
        integrations.append(integration)
        return integration.id
    }

    /// Remove an integration by its ID.
    ///
    /// - Parameter id: The integration ID to remove.
    public func removeIntegration(id: String) {
        integrations.removeAll { $0.id == id }
    }

    /// Dispatch an event to all matching integrations.
    ///
    /// - Parameters:
    ///   - event: The event type.
    ///   - payload: Key-value payload data to include.
    public func dispatch(event: IntegrationEvent, payload: [String: String] = [:]) {
        let matching = integrations.filter { $0.enabledEvents.contains(event) }

        for integration in matching {
            queue.async { [weak self] in
                self?.send(event: event, payload: payload, to: integration)
            }
        }
    }

    // MARK: - Private

    private func send(event: IntegrationEvent, payload: [String: String], to integration: Integration) {
        switch integration.type {
        case .slack:
            sendSlack(event: event, payload: payload, config: integration.config)
        case .webhook:
            sendWebhook(event: event, payload: payload, config: integration.config)
        case .mqtt:
            sendMQTTStub(event: event, payload: payload, config: integration.config)
        }
    }

    private func sendSlack(event: IntegrationEvent, payload: [String: String], config: [String: String]) {
        guard let urlString = config["url"], let url = URL(string: urlString) else { return }

        var text = "[Soluna] \(event.rawValue)"
        if !payload.isEmpty {
            let details = payload.map { "\($0.key): \($0.value)" }.joined(separator: ", ")
            text += " — \(details)"
        }

        let body: [String: Any] = ["text": text]
        postJSON(url: url, body: body, headers: [:])
    }

    private func sendWebhook(event: IntegrationEvent, payload: [String: String], config: [String: String]) {
        guard let urlString = config["url"], let url = URL(string: urlString) else { return }

        var body: [String: Any] = [
            "event": event.rawValue,
            "timestamp": ISO8601DateFormatter().string(from: Date()),
            "source": "SolunaSDK"
        ]
        body["data"] = payload

        var headers: [String: String] = [:]
        for (key, value) in config where key != "url" {
            headers[key] = value
        }

        postJSON(url: url, body: body, headers: headers)
    }

    private func sendMQTTStub(event: IntegrationEvent, payload: [String: String], config: [String: String]) {
        // MQTT is a stub — log the event that would be published
        let topic = config["topic"] ?? "soluna/events"
        print("[SolunaSDK] MQTT stub: topic=\(topic) event=\(event.rawValue) payload=\(payload)")
    }

    private func postJSON(url: URL, body: [String: Any], headers: [String: String]) {
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.setValue("SolunaSDK/1.0", forHTTPHeaderField: "User-Agent")

        for (key, value) in headers {
            request.setValue(value, forHTTPHeaderField: key)
        }

        do {
            request.httpBody = try JSONSerialization.data(withJSONObject: body)
        } catch {
            print("[SolunaSDK] Integration JSON encode error: \(error)")
            return
        }

        let task = session.dataTask(with: request) { _, response, error in
            if let error {
                print("[SolunaSDK] Integration send error: \(error.localizedDescription)")
            }
            if let http = response as? HTTPURLResponse, http.statusCode >= 400 {
                print("[SolunaSDK] Integration HTTP \(http.statusCode)")
            }
        }
        task.resume()
    }
}
