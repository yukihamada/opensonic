import Foundation
import Combine
#if os(iOS)
import UIKit
#endif

// MARK: - Memory Pressure Level

/// Memory pressure level indicating how aggressively caches should be trimmed.
public enum MemoryPressure: Int, Sendable, Comparable {
    /// Memory usage is within normal bounds.
    case normal   = 0
    /// Memory usage is elevated; non-essential caches should be trimmed.
    case warning  = 1
    /// Memory is critically low; stop non-essential work immediately.
    case critical = 2

    public static func < (lhs: MemoryPressure, rhs: MemoryPressure) -> Bool {
        lhs.rawValue < rhs.rawValue
    }
}

// MARK: - MemoryManager

/// Monitors process memory usage and coordinates progressive cleanup
/// when the system signals memory pressure.
///
/// Cleanup handlers are invoked in ascending priority order, allowing
/// the most expendable caches to be freed first.
///
/// Usage:
/// ```swift
/// let mm = MemoryManager()
/// mm.registerCleanupHandler(priority: 10) {
///     // trim audio cache
/// }
/// mm.startMonitoring()
/// ```
public final class MemoryManager: ObservableObject {

    // MARK: - Published State

    /// Current resident memory in megabytes.
    @Published public private(set) var currentUsageMB: Double = 0

    /// Current memory pressure level.
    @Published public private(set) var memoryPressureLevel: MemoryPressure = .normal

    // MARK: - Callbacks

    /// Called when the system issues a memory warning (iOS) or pressure rises.
    public var onMemoryWarning: (() -> Void)?

    // MARK: - Private

    private var monitorTimer: Timer?
    private var cleanupHandlers: [(priority: Int, handler: () -> Void)] = []
    private let queue = DispatchQueue(label: "com.soluna.memorymanager", qos: .utility)

    #if os(iOS)
    private var notificationObserver: NSObjectProtocol?
    #endif

    private var dispatchSource: DispatchSourceMemoryPressure?

    // MARK: - Thresholds (MB)

    /// Memory usage threshold that triggers the `.warning` level.
    private let warningThresholdMB: Double = 200

    /// Memory usage threshold that triggers the `.critical` level.
    private let criticalThresholdMB: Double = 400

    // MARK: - Init

    public init() {}

    deinit {
        stopMonitoring()
    }

    // MARK: - Public API

    /// Register a cleanup handler invoked when memory pressure rises.
    ///
    /// Handlers with lower priority values are called first (most expendable).
    ///
    /// - Parameters:
    ///   - priority: Execution order (lower = called earlier).
    ///   - handler: The cleanup closure.
    public func registerCleanupHandler(priority: Int, handler: @escaping () -> Void) {
        queue.sync {
            cleanupHandlers.append((priority: priority, handler: handler))
            cleanupHandlers.sort { $0.priority < $1.priority }
        }
    }

    /// Start periodic memory monitoring.
    ///
    /// On iOS, also observes `didReceiveMemoryWarningNotification`.
    /// On all platforms, uses a GCD memory pressure dispatch source.
    public func startMonitoring() {
        // Periodic sampling (every 5 seconds)
        monitorTimer = Timer.scheduledTimer(withTimeInterval: 5.0, repeats: true) { [weak self] _ in
            self?.sample()
        }
        // Take an initial reading
        sample()

        #if os(iOS)
        notificationObserver = NotificationCenter.default.addObserver(
            forName: UIApplication.didReceiveMemoryWarningNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            self?.handleSystemWarning()
        }
        #endif

        // GCD memory pressure source
        let source = DispatchSource.makeMemoryPressureSource(eventMask: [.warning, .critical], queue: queue)
        source.setEventHandler { [weak self] in
            guard let self else { return }
            let event = source.data
            if event.contains(.critical) {
                DispatchQueue.main.async { self.escalate(to: .critical) }
            } else if event.contains(.warning) {
                DispatchQueue.main.async { self.escalate(to: .warning) }
            }
        }
        source.resume()
        dispatchSource = source
    }

    /// Stop monitoring and release resources.
    public func stopMonitoring() {
        monitorTimer?.invalidate()
        monitorTimer = nil

        #if os(iOS)
        if let observer = notificationObserver {
            NotificationCenter.default.removeObserver(observer)
            notificationObserver = nil
        }
        #endif

        dispatchSource?.cancel()
        dispatchSource = nil
    }

    // MARK: - Private — Sampling

    private func sample() {
        let usage = Self.residentMemoryMB()
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.currentUsageMB = usage

            let level: MemoryPressure
            if usage >= self.criticalThresholdMB {
                level = .critical
            } else if usage >= self.warningThresholdMB {
                level = .warning
            } else {
                level = .normal
            }

            if level > self.memoryPressureLevel {
                self.escalate(to: level)
            } else {
                self.memoryPressureLevel = level
            }
        }
    }

    private func handleSystemWarning() {
        escalate(to: .warning)
    }

    /// Escalate to the given pressure level and run appropriate cleanup.
    private func escalate(to level: MemoryPressure) {
        memoryPressureLevel = level
        onMemoryWarning?()

        queue.async { [weak self] in
            guard let self else { return }
            // Run cleanup handlers appropriate for the level
            let handlers = self.cleanupHandlers
            for entry in handlers {
                entry.handler()
                // For warning level, only run low-priority handlers
                if level == .warning && entry.priority > 50 { break }
            }
        }
    }

    // MARK: - Private — Memory Measurement

    /// Read the process resident memory size using `mach_task_basic_info`.
    static func residentMemoryMB() -> Double {
        var info = mach_task_basic_info()
        var count = mach_msg_type_number_t(MemoryLayout<mach_task_basic_info>.size / MemoryLayout<natural_t>.size)
        let result = withUnsafeMutablePointer(to: &info) { infoPtr in
            infoPtr.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { rawPtr in
                task_info(mach_task_self_, task_flavor_t(MACH_TASK_BASIC_INFO), rawPtr, &count)
            }
        }
        guard result == KERN_SUCCESS else { return 0 }
        return Double(info.resident_size) / (1024.0 * 1024.0)
    }
}
