import Foundation
import Combine
#if os(iOS)
import UIKit
#endif
#if os(macOS)
import IOKit.ps
#endif

// MARK: - Power State

/// Device power state derived from battery level and system low-power mode.
public enum PowerState: Int, Sendable, Comparable {
    /// Battery > 50% and not in low-power mode. Full features enabled.
    case optimal  = 0
    /// Battery 20-50% or low-power mode active. Reduce non-essential work.
    case saving   = 1
    /// Battery < 20%. Minimum viable playback only.
    case critical = 2

    public static func < (lhs: PowerState, rhs: PowerState) -> Bool {
        lhs.rawValue < rhs.rawValue
    }
}

// MARK: - BatteryOptimizer

/// Monitors battery state and adjusts SDK behavior to conserve power.
///
/// When the device enters `.saving` or `.critical` power state, the optimizer
/// can disable expensive features like FFT analysis, reduce heartbeat
/// frequency, and lower buffer quality. Integrators can observe
/// `powerState` or register an `onPowerStateChanged` callback.
///
/// Usage:
/// ```swift
/// let optimizer = BatteryOptimizer()
/// optimizer.onPowerStateChanged = { state in
///     switch state {
///     case .optimal:  enableAllFeatures()
///     case .saving:   disableAnalyzer()
///     case .critical: minimumViableMode()
///     }
/// }
/// optimizer.startMonitoring()
/// ```
public final class BatteryOptimizer: ObservableObject {

    // MARK: - Published State

    /// Whether the optimizer is active.
    @Published public var isEnabled: Bool = true

    /// Current battery level (0.0 - 1.0). Returns -1.0 if unavailable.
    @Published public private(set) var batteryLevel: Float = 1.0

    /// Whether the system low-power mode is active.
    @Published public private(set) var isLowPowerMode: Bool = false

    /// Current power state derived from battery level and low-power mode.
    @Published public private(set) var powerState: PowerState = .optimal

    // MARK: - Callbacks

    /// Called when the power state changes.
    public var onPowerStateChanged: ((PowerState) -> Void)?

    // MARK: - Private

    private var monitorTimer: Timer?
    #if os(iOS)
    private var lowPowerObserver: NSObjectProtocol?
    private var batteryLevelObserver: NSObjectProtocol?
    #endif

    // MARK: - Init

    public init() {}

    deinit {
        stopMonitoring()
    }

    // MARK: - Public API

    /// Start monitoring battery level and power mode.
    public func startMonitoring() {
        #if os(iOS)
        UIDevice.current.isBatteryMonitoringEnabled = true
        readBatteryiOS()

        lowPowerObserver = NotificationCenter.default.addObserver(
            forName: .NSProcessInfoPowerStateDidChange,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            self?.updatePowerState()
        }

        batteryLevelObserver = NotificationCenter.default.addObserver(
            forName: UIDevice.batteryLevelDidChangeNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            self?.readBatteryiOS()
        }
        #endif

        // Periodic polling as a fallback (every 30 seconds)
        monitorTimer = Timer.scheduledTimer(withTimeInterval: 30.0, repeats: true) { [weak self] _ in
            self?.pollBattery()
        }

        updatePowerState()
    }

    /// Stop monitoring and release resources.
    public func stopMonitoring() {
        monitorTimer?.invalidate()
        monitorTimer = nil

        #if os(iOS)
        if let obs = lowPowerObserver {
            NotificationCenter.default.removeObserver(obs)
            lowPowerObserver = nil
        }
        if let obs = batteryLevelObserver {
            NotificationCenter.default.removeObserver(obs)
            batteryLevelObserver = nil
        }
        UIDevice.current.isBatteryMonitoringEnabled = false
        #endif
    }

    // MARK: - Private — Platform-Specific Battery Reading

    #if os(iOS)
    private func readBatteryiOS() {
        let level = UIDevice.current.batteryLevel
        batteryLevel = level >= 0 ? level : 1.0
        isLowPowerMode = ProcessInfo.processInfo.isLowPowerModeEnabled
        updatePowerState()
    }
    #endif

    #if os(macOS)
    private func readBatteryMacOS() {
        guard let snapshot = IOPSCopyPowerSourcesInfo()?.takeRetainedValue(),
              let sources = IOPSCopyPowerSourcesList(snapshot)?.takeRetainedValue() as? [CFTypeRef],
              let firstSource = sources.first,
              let desc = IOPSGetPowerSourceDescription(snapshot, firstSource)?.takeUnretainedValue() as? [String: Any]
        else {
            batteryLevel = 1.0
            return
        }

        if let capacity = desc[kIOPSCurrentCapacityKey] as? Int,
           let maxCapacity = desc[kIOPSMaxCapacityKey] as? Int, maxCapacity > 0 {
            batteryLevel = Float(capacity) / Float(maxCapacity)
        }

        isLowPowerMode = ProcessInfo.processInfo.isLowPowerModeEnabled
        updatePowerState()
    }
    #endif

    private func pollBattery() {
        #if os(iOS)
        readBatteryiOS()
        #elseif os(macOS)
        readBatteryMacOS()
        #endif
    }

    // MARK: - Private — State Evaluation

    private func updatePowerState() {
        guard isEnabled else { return }

        let newState: PowerState
        if batteryLevel < 0.20 || (isLowPowerMode && batteryLevel < 0.30) {
            newState = .critical
        } else if batteryLevel < 0.50 || isLowPowerMode {
            newState = .saving
        } else {
            newState = .optimal
        }

        if newState != powerState {
            powerState = newState
            onPowerStateChanged?(newState)
        }
    }
}
