import Foundation

// MARK: - Protocols for Testability

/// Protocol for audio playback operations.
public protocol AudioPlayable: AnyObject {
    /// Start the audio engine.
    func startPlayback()
    /// Stop the audio engine.
    func stopPlayback()
}

/// Protocol for relay server connections.
public protocol RelayConnectable: AnyObject {
    /// Connect to the relay server.
    /// - Returns: `true` if the connection was established.
    func connect() -> Bool
    /// Disconnect from the relay server.
    func disconnect()
    /// Send raw binary data to the relay.
    func sendRawData(_ data: Data)
}

/// Protocol for P2P peer discovery.
public protocol PeerDiscoverable: AnyObject {
    /// Start scanning for peers.
    func startScan()
    /// Stop scanning for peers.
    func stopScan()
}

// MARK: - DependencyContainer

/// Lightweight dependency injection container for the Soluna SDK.
///
/// Allows production classes to be swapped with mocks in tests.
/// Supports both factory-based registrations (new instance per resolve)
/// and singleton registrations (shared instance).
///
/// Usage:
/// ```swift
/// // Production setup (automatic via default registrations)
/// let container = DependencyContainer.shared
///
/// // Test setup
/// DependencyContainer.shared.reset()
/// DependencyContainer.shared.registerSingleton(AudioPlayable.self, instance: MockAudioPlayer())
///
/// // Resolve
/// let player: AudioPlayable? = DependencyContainer.shared.resolve(AudioPlayable.self)
/// ```
public final class DependencyContainer: @unchecked Sendable {

    // MARK: - Singleton

    /// Shared dependency container.
    public static let shared = DependencyContainer()

    // MARK: - Private

    private let queue = DispatchQueue(label: "com.soluna.di", qos: .utility)
    private var factories: [String: () -> Any] = [:]
    private var singletons: [String: Any] = [:]

    // MARK: - Init

    private init() {}

    // MARK: - Public API

    /// Register a factory that creates a new instance each time `resolve` is called.
    ///
    /// - Parameters:
    ///   - type: The protocol or class type to register.
    ///   - factory: A closure that produces an instance.
    public func register<T>(_ type: T.Type, factory: @escaping () -> T) {
        let key = String(describing: type)
        queue.sync {
            factories[key] = factory
            // Remove any existing singleton so the factory takes effect
            singletons.removeValue(forKey: key)
        }
    }

    /// Register a pre-created singleton instance.
    ///
    /// - Parameters:
    ///   - type: The protocol or class type to register.
    ///   - instance: The singleton instance to return on every resolve.
    public func registerSingleton<T>(_ type: T.Type, instance: T) {
        let key = String(describing: type)
        queue.sync {
            singletons[key] = instance
            // Remove any factory so the singleton takes precedence
            factories.removeValue(forKey: key)
        }
    }

    /// Resolve a dependency.
    ///
    /// Returns a singleton if one is registered, otherwise invokes the
    /// factory. Returns `nil` if the type has not been registered.
    ///
    /// - Parameter type: The protocol or class type to resolve.
    /// - Returns: An instance of the requested type, or `nil`.
    public func resolve<T>(_ type: T.Type) -> T? {
        let key = String(describing: type)
        return queue.sync {
            if let singleton = singletons[key] as? T {
                return singleton
            }
            if let factory = factories[key] {
                return factory() as? T
            }
            return nil
        }
    }

    /// Clear all registrations. Intended for use in test teardown.
    public func reset() {
        queue.sync {
            factories.removeAll()
            singletons.removeAll()
        }
    }
}
