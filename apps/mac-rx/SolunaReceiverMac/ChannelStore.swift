//
//  ChannelStore.swift
//  SolunaReceiver
//
//  StoreKit 2 manager for custom channel name purchases
//

import StoreKit
import Network
import Foundation

// MARK: - Subscription Plans

enum SolunaPlan: String, CaseIterable {
    case free = "Free"
    case pro = "Pro"
    case studio = "Studio"

    var isSubscribed: Bool { self != .free }
    var canCreateChannel: Bool { self != .free }
    var hasHighQuality: Bool { self != .free }
    var hasAnalytics: Bool { self == .studio }
    var hasBroadcast: Bool { self == .studio }

    var price: String {
        switch self {
        case .free: return "\u{00A5}0"
        case .pro: return "\u{00A5}500/\u{6708}"
        case .studio: return "\u{00A5}2,000/\u{6708}"
        }
    }

    var channelSlots: Int {
        switch self {
        case .free: return 0
        case .pro: return 1
        case .studio: return 3
        }
    }

    var features: [(String, String)] {
        switch self {
        case .free: return [
            ("\u{30C1}\u{30E3}\u{30F3}\u{30CD}\u{30EB}", "\u{30E9}\u{30F3}\u{30C0}\u{30E0}\u{306E}\u{307F}"),
            ("\u{97F3}\u{8CEA}", "\u{6A19}\u{6E96}"),
            ("\u{5E83}\u{544A}", "\u{3042}\u{308A}"),
            ("\u{914D}\u{4FE1}", "\u{2014}"),
        ]
        case .pro: return [
            ("\u{30C1}\u{30E3}\u{30F3}\u{30CD}\u{30EB}", "\u{30AB}\u{30B9}\u{30BF}\u{30E0}1\u{500B}"),
            ("\u{97F3}\u{8CEA}", "\u{9AD8}\u{97F3}\u{8CEA}"),
            ("\u{5E83}\u{544A}", "\u{306A}\u{3057}"),
            ("\u{914D}\u{4FE1}", "\u{5BFE}\u{5FDC}"),
        ]
        case .studio: return [
            ("\u{30C1}\u{30E3}\u{30F3}\u{30CD}\u{30EB}", "\u{30AB}\u{30B9}\u{30BF}\u{30E0}3\u{500B}"),
            ("\u{97F3}\u{8CEA}", "\u{6700}\u{9AD8}\u{97F3}\u{8CEA}"),
            ("\u{5E83}\u{544A}", "\u{306A}\u{3057}"),
            ("\u{914D}\u{4FE1}", "Pro Dashboard"),
        ]
        }
    }
}

@MainActor
final class ChannelStore: ObservableObject {
    static let productID = "com.soluna.channel.year"  // ¥1,000/year
    static let proMonthlyID = "com.soluna.pro.monthly"  // ¥500/month
    static let studioMonthlyID = "com.soluna.studio.monthly"  // ¥2,000/month
    static let relayHost = "relay.solun.art"
    static let relayPort: UInt16 = 5100

    @Published var product: Product?
    @Published var proProduct: Product?
    @Published var studioProduct: Product?
    @Published var isPurchased: Bool = false
    @Published var purchasedChannelName: String?
    @Published var expiryDate: Date?
    @Published var currentPlan: SolunaPlan = .free
    @Published var isLoading: Bool = false
    @Published var errorMessage: String?

    private var transactionListener: Task<Void, Error>?

    init() {
        transactionListener = listenForTransactions()

        // Restore persisted state
        let defaults = UserDefaults.standard
        if let name = defaults.string(forKey: "purchasedChannel"), !name.isEmpty {
            purchasedChannelName = name
            isPurchased = true
        }
        if let interval = defaults.object(forKey: "channelExpiry") as? Double, interval > 0 {
            expiryDate = Date(timeIntervalSince1970: interval)
        }

        Task { await loadProducts() }
        Task { await checkEntitlements() }
    }

    deinit {
        transactionListener?.cancel()
    }

    // MARK: - StoreKit

    func loadProducts() async {
        do {
            let products = try await Product.products(for: [
                Self.productID,
                Self.proMonthlyID,
                Self.studioMonthlyID
            ])
            for p in products {
                switch p.id {
                case Self.productID:
                    product = p
                case Self.proMonthlyID:
                    proProduct = p
                case Self.studioMonthlyID:
                    studioProduct = p
                default:
                    break
                }
            }
        } catch {
            errorMessage = "Failed to load products: \(error.localizedDescription)"
        }
    }

    @discardableResult
    func purchasePro() async throws -> Transaction? {
        guard let proProduct else {
            errorMessage = "Pro product not available"
            return nil
        }
        return try await purchaseProduct(proProduct)
    }

    @discardableResult
    func purchaseStudio() async throws -> Transaction? {
        guard let studioProduct else {
            errorMessage = "Studio product not available"
            return nil
        }
        return try await purchaseProduct(studioProduct)
    }

    private func purchaseProduct(_ product: Product) async throws -> Transaction? {
        isLoading = true
        defer { isLoading = false }

        let result = try await product.purchase()

        switch result {
        case .success(let verification):
            let transaction = try checkVerified(verification)
            await transaction.finish()
            if transaction.productID == Self.proMonthlyID ||
               transaction.productID == Self.studioMonthlyID {
                await checkEntitlements()
            } else {
                await updatePurchaseState(transaction)
            }
            return transaction

        case .userCancelled:
            return nil

        case .pending:
            errorMessage = "Purchase is pending approval"
            return nil

        @unknown default:
            errorMessage = "Unknown purchase result"
            return nil
        }
    }

    @discardableResult
    func purchase() async throws -> Transaction? {
        guard let product else {
            errorMessage = "Product not available"
            return nil
        }
        return try await purchaseProduct(product)
    }

    func checkEntitlements() async {
        var foundChannel = false
        var detectedPlan: SolunaPlan = .free

        for await result in Transaction.currentEntitlements {
            if case .verified(let transaction) = result {
                switch transaction.productID {
                case Self.productID:
                    foundChannel = true
                    await updateChannelState(transaction)
                case Self.studioMonthlyID:
                    detectedPlan = .studio
                case Self.proMonthlyID:
                    if detectedPlan != .studio {
                        detectedPlan = .pro
                    }
                default:
                    break
                }
            }
        }

        currentPlan = detectedPlan

        if !foundChannel {
            isPurchased = false
            purchasedChannelName = nil
            expiryDate = nil
            persistState()
        }
    }

    private func listenForTransactions() -> Task<Void, Error> {
        Task.detached { [weak self] in
            for await result in Transaction.updates {
                if case .verified(let transaction) = result {
                    await transaction.finish()
                    switch transaction.productID {
                    case ChannelStore.productID:
                        await self?.updatePurchaseState(transaction)
                    case ChannelStore.proMonthlyID, ChannelStore.studioMonthlyID:
                        await self?.checkEntitlements()
                    default:
                        break
                    }
                }
            }
        }
    }

    private func updatePurchaseState(_ transaction: Transaction) async {
        await updateChannelState(transaction)
    }

    private func updateChannelState(_ transaction: Transaction) async {
        if transaction.revocationDate != nil {
            isPurchased = false
            purchasedChannelName = nil
            expiryDate = nil
        } else {
            isPurchased = true
            expiryDate = transaction.expirationDate
        }
        persistState()
    }

    private func checkVerified<T>(_ result: VerificationResult<T>) throws -> T {
        switch result {
        case .unverified(_, let error):
            throw error
        case .verified(let safe):
            return safe
        }
    }

    private func persistState() {
        let defaults = UserDefaults.standard
        defaults.set(purchasedChannelName ?? "", forKey: "purchasedChannel")
        defaults.set(expiryDate?.timeIntervalSince1970 ?? 0, forKey: "channelExpiry")
    }

    private static func machineDeviceID() -> String {
        #if canImport(UIKit)
        return UIDevice.current.identifierForVendor?.uuidString ?? UUID().uuidString
        #else
        // macOS: use hardware UUID
        let service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IOPlatformExpertDevice"))
        defer { IOObjectRelease(service) }
        if let uuid = IORegistryEntryCreateCFProperty(service, "IOPlatformUUID" as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue() as? String {
            return uuid
        }
        return UUID().uuidString
        #endif
    }

    // MARK: - Channel name persistence

    func savePurchasedChannel(_ name: String) {
        purchasedChannelName = name
        persistState()
    }

    // MARK: - UDP Relay Communication

    func checkChannel(name: String) async -> ChannelAvailability {
        let message = "CHECK:\(name)\n"
        do {
            let response = try await sendUDP(message: message)
            if response.contains("OK:available") {
                return .available
            } else if response.contains("OK:taken") {
                return .taken
            } else {
                return .error("Unexpected response: \(response)")
            }
        } catch {
            return .error(error.localizedDescription)
        }
    }

    func claimChannel(name: String, transactionID: UInt64) async -> ClaimResult {
        let deviceID = Self.machineDeviceID()
        let message = "CLAIM:\(name):\(deviceID):\(transactionID)\n"
        do {
            let response = try await sendUDP(message: message)
            if response.contains("OK:claimed") || response.contains("OK:renewed") {
                savePurchasedChannel(name)
                return .success
            } else if response.contains("ERR:taken") {
                return .taken
            } else if response.contains("ERR:invalid_name") {
                return .error("Invalid channel name (3-20 chars, letters/numbers/hyphens)")
            } else if response.contains("ERR:reserved_name") {
                return .error("This name is reserved")
            } else {
                return .error("Unexpected response: \(response)")
            }
        } catch {
            return .error(error.localizedDescription)
        }
    }

    func releaseChannel() async -> Bool {
        guard let name = purchasedChannelName, !name.isEmpty else { return false }
        let deviceID = Self.machineDeviceID()
        let message = "RELEASE:\(name):\(deviceID)\n"
        do {
            let response = try await sendUDP(message: message)
            if response.contains("OK:released") {
                purchasedChannelName = nil
                isPurchased = false
                expiryDate = nil
                persistState()
                return true
            }
        } catch {
            errorMessage = "Release failed: \(error.localizedDescription)"
        }
        return false
    }

    private func sendUDP(message: String) async throws -> String {
        try await withCheckedThrowingContinuation { continuation in
            let host = NWEndpoint.Host(Self.relayHost)
            let port = NWEndpoint.Port(rawValue: Self.relayPort)!
            let connection = NWConnection(host: host, port: port, using: .udp)

            var didResume = false

            let timeout = DispatchWorkItem {
                if !didResume {
                    didResume = true
                    connection.cancel()
                    continuation.resume(throwing: ChannelStoreError.timeout)
                }
            }
            DispatchQueue.global().asyncAfter(deadline: .now() + 3, execute: timeout)

            connection.stateUpdateHandler = { state in
                switch state {
                case .ready:
                    let data = message.data(using: .utf8)!
                    connection.send(content: data, completion: .contentProcessed { sendError in
                        if let sendError {
                            timeout.cancel()
                            if !didResume {
                                didResume = true
                                connection.cancel()
                                continuation.resume(throwing: sendError)
                            }
                            return
                        }
                        // Receive response
                        connection.receiveMessage { data, _, _, recvError in
                            timeout.cancel()
                            guard !didResume else { return }
                            didResume = true
                            connection.cancel()

                            if let recvError {
                                continuation.resume(throwing: recvError)
                            } else if let data, let response = String(data: data, encoding: .utf8) {
                                continuation.resume(returning: response)
                            } else {
                                continuation.resume(throwing: ChannelStoreError.noData)
                            }
                        }
                    })

                case .failed(let error):
                    timeout.cancel()
                    if !didResume {
                        didResume = true
                        connection.cancel()
                        continuation.resume(throwing: error)
                    }

                default:
                    break
                }
            }

            connection.start(queue: .global())
        }
    }
}

// MARK: - Types

enum ChannelAvailability: Equatable {
    case available
    case taken
    case error(String)
    case unknown
}

enum ClaimResult: Equatable {
    case success
    case taken
    case error(String)
}

enum ChannelStoreError: LocalizedError {
    case timeout
    case noData

    var errorDescription: String? {
        switch self {
        case .timeout: return "Connection timed out"
        case .noData: return "No data received"
        }
    }
}
