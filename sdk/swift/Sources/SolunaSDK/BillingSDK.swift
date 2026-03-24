import Foundation
import StoreKit
import Combine

/// Subscription plan tiers.
public enum SubscriptionPlan: String, Sendable, CaseIterable {
    case free = "free"
    case starter = "starter"
    case pro = "pro"
    case enterprise = "enterprise"
}

/// A product available for purchase.
public struct SolunaProduct: Identifiable, Sendable {
    public let id: String
    public let name: String
    public let price: String
    public let period: String

    public init(id: String, name: String, price: String, period: String) {
        self.id = id
        self.name = name
        self.price = price
        self.period = period
    }
}

/// Result of a purchase attempt.
public enum PurchaseResult: Sendable {
    case success(transactionId: UInt64)
    case pending
    case cancelled
    case failed(String)
}

/// StoreKit 2 billing integration for Soluna subscriptions.
///
/// Manages in-app purchases and subscriptions using StoreKit 2,
/// with support for loading products, purchasing, and restoring.
///
/// Usage:
/// ```swift
/// let billing = BillingManager()
/// await billing.loadProducts()
/// let result = try await billing.purchase(productId: "com.soluna.pro.monthly")
/// ```
public final class BillingManager: ObservableObject {

    // MARK: - Published State

    /// Available products for purchase.
    @Published public private(set) var products: [SolunaProduct] = []

    /// Current subscription plan.
    @Published public private(set) var currentPlan: SubscriptionPlan = .free

    /// Whether the user has an active subscription.
    public var isSubscribed: Bool {
        currentPlan != .free
    }

    // MARK: - Private

    /// StoreKit product IDs to load.
    private let productIds: Set<String> = [
        "com.soluna.starter.monthly",
        "com.soluna.pro.monthly",
        "com.soluna.enterprise.monthly",
        "com.soluna.starter.yearly",
        "com.soluna.pro.yearly",
        "com.soluna.enterprise.yearly"
    ]

    /// Cached StoreKit products.
    private var storeProducts: [Product] = []

    /// Transaction listener task.
    private var transactionListener: Task<Void, Never>?

    // MARK: - Init

    public init() {
        startTransactionListener()
    }

    deinit {
        transactionListener?.cancel()
    }

    // MARK: - Public API

    /// Load available products from the App Store.
    public func loadProducts() async {
        do {
            storeProducts = try await Product.products(for: productIds)

            let mapped = storeProducts.map { product in
                SolunaProduct(
                    id: product.id,
                    name: product.displayName,
                    price: product.displayPrice,
                    period: product.subscription?.subscriptionPeriod.debugDescription ?? ""
                )
            }

            await MainActor.run {
                self.products = mapped
            }
        } catch {
            print("[SolunaSDK] Failed to load products: \(error)")
        }
    }

    /// Purchase a product by its ID.
    ///
    /// - Parameter productId: The product identifier to purchase.
    /// - Returns: The result of the purchase attempt.
    public func purchase(productId: String) async throws -> PurchaseResult {
        guard let product = storeProducts.first(where: { $0.id == productId }) else {
            return .failed("Product not found: \(productId)")
        }

        let result = try await product.purchase()

        switch result {
        case .success(let verification):
            let transaction = try checkVerified(verification)
            await transaction.finish()
            await updateCurrentPlan()
            return .success(transactionId: transaction.id)

        case .userCancelled:
            return .cancelled

        case .pending:
            return .pending

        @unknown default:
            return .failed("Unknown purchase result")
        }
    }

    /// Restore previously purchased subscriptions.
    public func restorePurchases() async {
        try? await AppStore.sync()
        await updateCurrentPlan()
    }

    // MARK: - Private

    private func startTransactionListener() {
        transactionListener = Task.detached { [weak self] in
            for await result in Transaction.updates {
                guard let self else { return }
                if let transaction = try? self.checkVerified(result) {
                    await transaction.finish()
                    await self.updateCurrentPlan()
                }
            }
        }
    }

    private func checkVerified<T>(_ result: VerificationResult<T>) throws -> T {
        switch result {
        case .unverified(_, let error):
            throw error
        case .verified(let safe):
            return safe
        }
    }

    @MainActor
    private func updateCurrentPlan() async {
        var highestPlan: SubscriptionPlan = .free

        for await result in Transaction.currentEntitlements {
            guard let transaction = try? checkVerified(result) else { continue }

            if transaction.productID.contains("enterprise") {
                highestPlan = .enterprise
                break
            } else if transaction.productID.contains("pro") && highestPlan != .enterprise {
                highestPlan = .pro
            } else if transaction.productID.contains("starter") && highestPlan == .free {
                highestPlan = .starter
            }
        }

        currentPlan = highestPlan
    }
}
