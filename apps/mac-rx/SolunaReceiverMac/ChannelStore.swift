//
//  ChannelStore.swift
//  SolunaReceiver
//
//  StoreKit 2 manager for custom channel name purchases
//

import StoreKit
import Network
import Foundation

@MainActor
final class ChannelStore: ObservableObject {
    static let productID = "com.soluna.channel.year"  // ¥1,000/year
    static let relayHost = "46.225.77.119"
    static let relayPort: UInt16 = 5100

    @Published var product: Product?
    @Published var isPurchased: Bool = false
    @Published var purchasedChannelName: String?
    @Published var expiryDate: Date?
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
            let products = try await Product.products(for: [Self.productID])
            product = products.first
        } catch {
            errorMessage = "Failed to load products: \(error.localizedDescription)"
        }
    }

    @discardableResult
    func purchase() async throws -> Transaction? {
        guard let product else {
            errorMessage = "Product not available"
            return nil
        }

        isLoading = true
        defer { isLoading = false }

        let result = try await product.purchase()

        switch result {
        case .success(let verification):
            let transaction = try checkVerified(verification)
            await transaction.finish()
            await updatePurchaseState(transaction)
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

    func checkEntitlements() async {
        for await result in Transaction.currentEntitlements {
            if case .verified(let transaction) = result,
               transaction.productID == Self.productID {
                await updatePurchaseState(transaction)
                return
            }
        }
        // No active entitlement found
        isPurchased = false
        purchasedChannelName = nil
        expiryDate = nil
        persistState()
    }

    private func listenForTransactions() -> Task<Void, Error> {
        Task.detached { [weak self] in
            for await result in Transaction.updates {
                if case .verified(let transaction) = result,
                   transaction.productID == ChannelStore.productID {
                    await transaction.finish()
                    await self?.updatePurchaseState(transaction)
                }
            }
        }
    }

    private func updatePurchaseState(_ transaction: Transaction) async {
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
