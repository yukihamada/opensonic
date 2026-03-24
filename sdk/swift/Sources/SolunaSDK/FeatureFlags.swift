import Foundation

// MARK: - Feature

/// SDK features that can be toggled at runtime.
public enum Feature: String, CaseIterable, Sendable {
    /// End-to-end encryption for audio streams.
    case e2eEncryption     = "e2e_encryption"
    /// P2P peer discovery on the local network.
    case p2pDiscovery      = "p2p_discovery"
    /// DJ dual-deck mode with crossfade.
    case djMode            = "dj_mode"
    /// Microphone transmit to relay.
    case micTransmit       = "mic_transmit"
    /// Offline caching of audio streams.
    case offlineCache      = "offline_cache"
    /// Real-time noise cancellation on mic input.
    case noiseCancellation = "noise_cancellation"
    /// Inaudible audio watermarking for rights tracking.
    case audioWatermark    = "audio_watermark"
    /// Usage analytics collection.
    case analytics         = "analytics"
    /// In-app billing and subscription management.
    case billing           = "billing"
    /// Voice command recognition.
    case voiceCommands     = "voice_commands"
    /// Multi-region relay routing.
    case multiRegion       = "multi_region"
    /// Automatic quality-of-service adjustment.
    case autoQoS           = "auto_qos"
}

// MARK: - FeatureFlags

/// Runtime feature flag manager for enabling and disabling SDK capabilities.
///
/// Flags persist in `UserDefaults` so they survive app restarts. Defaults
/// are set so that most features are enabled out of the box, with opt-in
/// features (encryption, watermark, billing) disabled by default.
///
/// Usage:
/// ```swift
/// if FeatureFlags.shared.isEnabled(.djMode) {
///     showDJControls()
/// }
///
/// FeatureFlags.shared.disable(.analytics)
/// ```
public final class FeatureFlags: @unchecked Sendable {

    // MARK: - Singleton

    /// Shared feature flags instance.
    public static let shared = FeatureFlags()

    // MARK: - Private

    private let queue = DispatchQueue(label: "com.soluna.featureflags", qos: .utility)
    private var flags: [Feature: Bool]
    private let storageKey = "com.soluna.featureflags"

    /// Features that are disabled by default (opt-in).
    private static let optInFeatures: Set<Feature> = [
        .e2eEncryption,
        .audioWatermark,
        .billing,
    ]

    // MARK: - Init

    private init() {
        // Start with defaults
        var defaults = [Feature: Bool]()
        for feature in Feature.allCases {
            defaults[feature] = !Self.optInFeatures.contains(feature)
        }

        // Override with persisted values
        if let stored = UserDefaults.standard.dictionary(forKey: storageKey) as? [String: Bool] {
            for (key, value) in stored {
                if let feature = Feature(rawValue: key) {
                    defaults[feature] = value
                }
            }
        }

        flags = defaults
    }

    // MARK: - Public API

    /// Check whether a feature is enabled.
    ///
    /// - Parameter flag: The feature to check.
    /// - Returns: `true` if the feature is enabled.
    public func isEnabled(_ flag: Feature) -> Bool {
        queue.sync { flags[flag] ?? false }
    }

    /// Enable a feature.
    ///
    /// - Parameter flag: The feature to enable.
    public func enable(_ flag: Feature) {
        queue.sync {
            flags[flag] = true
            persist()
        }
    }

    /// Disable a feature.
    ///
    /// - Parameter flag: The feature to disable.
    public func disable(_ flag: Feature) {
        queue.sync {
            flags[flag] = false
            persist()
        }
    }

    /// Bulk-set feature flags.
    ///
    /// - Parameter newFlags: A dictionary of features and their enabled state.
    public func setAll(_ newFlags: [Feature: Bool]) {
        queue.sync {
            for (feature, enabled) in newFlags {
                flags[feature] = enabled
            }
            persist()
        }
    }

    /// Load feature flags from a `RemoteConfig` instance.
    ///
    /// Reads keys matching `Feature.rawValue` and interprets their values
    /// as booleans (`"true"/"1"/"yes"` = enabled).
    ///
    /// - Parameter config: The remote config to read from.
    public func loadFromRemoteConfig(_ config: RemoteConfig) {
        queue.sync {
            for feature in Feature.allCases {
                if let value = config.getBool(feature.rawValue) {
                    flags[feature] = value
                }
            }
            persist()
        }
    }

    // MARK: - Private

    private func persist() {
        var dict = [String: Bool]()
        for (feature, enabled) in flags {
            dict[feature.rawValue] = enabled
        }
        UserDefaults.standard.set(dict, forKey: storageKey)
    }
}
