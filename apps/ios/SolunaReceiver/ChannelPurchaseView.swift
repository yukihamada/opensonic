//
//  ChannelPurchaseView.swift
//  SolunaReceiver
//
//  UI for purchasing a custom channel name via StoreKit 2
//

import SwiftUI

struct ChannelPurchaseView: View {
    @ObservedObject var store: ChannelStore
    @Binding var activeChannel: String
    @Environment(\.dismiss) private var dismiss

    @State private var desiredName: String = ""
    @State private var availability: ChannelAvailability = .unknown
    @State private var isChecking: Bool = false
    @State private var checkTask: Task<Void, Never>?
    @State private var claimState: ClaimState = .idle
    @State private var showSuccess: Bool = false

    private enum ClaimState: Equatable {
        case idle
        case purchasing
        case claiming
        case success(String)
        case error(String)
    }

    private static let reservedNames = ["soluna", "default", "test"]

    private var nameIsValid: Bool {
        let trimmed = desiredName.trimmingCharacters(in: .whitespaces).lowercased()
        guard trimmed.count >= 3, trimmed.count <= 20 else { return false }
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "-"))
        guard trimmed.unicodeScalars.allSatisfy({ allowed.contains($0) }) else { return false }
        // Reject all-hex names (reserved for random channel generation)
        let hexChars = CharacterSet(charactersIn: "0123456789abcdefABCDEF")
        if trimmed.unicodeScalars.allSatisfy({ hexChars.contains($0) }) { return false }
        // Reject built-in reserved names
        if Self.reservedNames.contains(trimmed) { return false }
        return true
    }

    var body: some View {
        NavigationView {
            Form {
                // Already purchased
                if store.isPurchased, let name = store.purchasedChannelName {
                    Section {
                        HStack(spacing: 12) {
                            Image(systemName: "checkmark.seal.fill")
                                .font(.title2)
                                .foregroundColor(.blue)
                            VStack(alignment: .leading, spacing: 4) {
                                Text(name)
                                    .font(.headline)
                                if let expiry = store.expiryDate {
                                    Text("Expires \(expiry, style: .date)")
                                        .font(.caption)
                                        .foregroundColor(.secondary)
                                }
                            }
                            Spacer()
                            Text("Active")
                                .font(.caption.weight(.semibold))
                                .foregroundColor(.white)
                                .padding(.horizontal, 10)
                                .padding(.vertical, 4)
                                .background(Color.blue)
                                .clipShape(Capsule())
                        }
                        .padding(.vertical, 4)
                    } header: {
                        Text("Current Custom Channel")
                    } footer: {
                        Text("Your custom channel name is reserved and renewed yearly.")
                    }

                    Section {
                        Button {
                            activeChannel = name
                            dismiss()
                        } label: {
                            Label("Use This Channel", systemImage: "checkmark.circle")
                        }
                    }
                }

                // Claim a new custom channel
                Section {
                    HStack {
                        Image(systemName: "number")
                            .foregroundColor(.purple)
                            .frame(width: 28)
                        TextField("Channel name", text: $desiredName)
                            .autocapitalization(.none)
                            .autocorrectionDisabled()
                            .onChange(of: desiredName) { _ in
                                debounceCheck()
                            }
                        availabilityIndicator
                    }

                    // Validation rules
                    VStack(alignment: .leading, spacing: 4) {
                        ruleRow("3-20 characters", met: desiredName.count >= 3 && desiredName.count <= 20)
                        ruleRow("Letters, numbers, hyphens only", met: {
                            guard !desiredName.isEmpty else { return false }
                            let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "-"))
                            return desiredName.unicodeScalars.allSatisfy { allowed.contains($0) }
                        }())
                        ruleRow("Not all hex digits (reserved for random)", met: {
                            guard !desiredName.isEmpty else { return false }
                            let hex = CharacterSet(charactersIn: "0123456789abcdefABCDEF")
                            return !desiredName.unicodeScalars.allSatisfy { hex.contains($0) }
                        }())
                    }
                    .padding(.vertical, 4)
                } header: {
                    Text(store.isPurchased ? "Change Channel Name" : "Custom Channel Name")
                } footer: {
                    Text("Choose a unique name for your channel. Other devices use this name to find your stream.")
                }

                // Price & Purchase
                Section {
                    if let product = store.product {
                        HStack {
                            VStack(alignment: .leading, spacing: 2) {
                                Text("Custom Channel")
                                    .font(.subheadline.weight(.semibold))
                                Text("Yearly subscription")
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                            }
                            Spacer()
                            Text(product.displayPrice)
                                .font(.subheadline.weight(.bold))
                                .foregroundColor(.blue)
                        }
                    }

                    Button {
                        Task { await purchaseAndClaim() }
                    } label: {
                        HStack {
                            Spacer()
                            if claimState == .purchasing || claimState == .claiming {
                                ProgressView()
                                    .padding(.trailing, 8)
                            }
                            Text(purchaseButtonTitle)
                                .fontWeight(.semibold)
                            Spacer()
                        }
                    }
                    .disabled(!canPurchase)

                    if case .error(let msg) = claimState {
                        HStack(spacing: 8) {
                            Image(systemName: "exclamationmark.triangle.fill")
                                .foregroundColor(.red)
                            Text(msg)
                                .font(.caption)
                                .foregroundColor(.red)
                        }
                    }
                } header: {
                    Text("Purchase")
                }

                // Free option
                Section {
                    Button {
                        let hex = randomHexChannel()
                        activeChannel = hex
                        dismiss()
                    } label: {
                        HStack {
                            Image(systemName: "shuffle")
                                .foregroundColor(.secondary)
                                .frame(width: 28)
                            VStack(alignment: .leading, spacing: 2) {
                                Text("Use Random Channel")
                                    .foregroundColor(.primary)
                                Text("Free - generates a 6-character code")
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                            }
                        }
                    }
                } header: {
                    Text("Free Option")
                }
            }
            .navigationTitle("Channel")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") { dismiss() }
                }
            }
            .alert("Channel Claimed", isPresented: $showSuccess) {
                Button("OK") {
                    if case .success(let name) = claimState {
                        activeChannel = name
                    }
                    dismiss()
                }
            } message: {
                if case .success(let name) = claimState {
                    Text("Your channel \"\(name)\" is now reserved. It has been set as your active channel.")
                }
            }
        }
    }

    // MARK: - Subviews

    @ViewBuilder
    private var availabilityIndicator: some View {
        if isChecking {
            ProgressView()
                .scaleEffect(0.8)
        } else if desiredName.isEmpty || !nameIsValid {
            EmptyView()
        } else {
            switch availability {
            case .available:
                Image(systemName: "checkmark.circle.fill")
                    .foregroundColor(.green)
            case .taken:
                Image(systemName: "xmark.circle.fill")
                    .foregroundColor(.red)
            case .error:
                Image(systemName: "exclamationmark.triangle.fill")
                    .foregroundColor(.orange)
            case .unknown:
                EmptyView()
            }
        }
    }

    private func ruleRow(_ text: String, met: Bool) -> some View {
        HStack(spacing: 6) {
            Image(systemName: met ? "checkmark.circle.fill" : "circle")
                .font(.caption)
                .foregroundColor(met ? .green : .secondary)
            Text(text)
                .font(.caption)
                .foregroundColor(met ? .primary : .secondary)
        }
    }

    // MARK: - Logic

    private var canPurchase: Bool {
        guard nameIsValid else { return false }
        guard availability == .available else { return false }
        guard store.product != nil else { return false }
        if case .purchasing = claimState { return false }
        if case .claiming = claimState { return false }
        return true
    }

    private var purchaseButtonTitle: String {
        switch claimState {
        case .purchasing: return "Purchasing..."
        case .claiming: return "Claiming channel..."
        default:
            if store.isPurchased {
                return "Claim This Name"
            }
            return "Purchase & Claim"
        }
    }

    private func debounceCheck() {
        checkTask?.cancel()
        availability = .unknown

        let name = desiredName.trimmingCharacters(in: .whitespaces).lowercased()
        guard name.count >= 3, nameIsValid else { return }

        checkTask = Task {
            try? await Task.sleep(nanoseconds: 500_000_000) // 0.5s debounce
            guard !Task.isCancelled else { return }

            isChecking = true
            let result = await store.checkChannel(name: name)
            guard !Task.isCancelled else { return }
            isChecking = false
            availability = result
        }
    }

    private func purchaseAndClaim() async {
        let name = desiredName.trimmingCharacters(in: .whitespaces).lowercased()

        // If already purchased (changing name), skip purchase step
        if store.isPurchased {
            claimState = .claiming
            let result = await store.claimChannel(name: name, transactionID: 0)
            switch result {
            case .success:
                claimState = .success(name)
                showSuccess = true
            case .taken:
                claimState = .error("Channel name was just taken. Try another.")
                availability = .taken
            case .error(let msg):
                claimState = .error(msg)
            }
            return
        }

        // Full purchase flow
        claimState = .purchasing
        do {
            guard let transaction = try await store.purchase() else {
                claimState = .idle
                return
            }
            claimState = .claiming
            let result = await store.claimChannel(name: name, transactionID: transaction.id)
            switch result {
            case .success:
                claimState = .success(name)
                showSuccess = true
            case .taken:
                claimState = .error("Channel name was just taken. Try another name -- your subscription is active.")
            case .error(let msg):
                claimState = .error(msg)
            }
        } catch {
            claimState = .error(error.localizedDescription)
        }
    }

    private func randomHexChannel() -> String {
        let bytes = (0..<3).map { _ in UInt8.random(in: 0...255) }
        return bytes.map { String(format: "%02x", $0) }.joined()
    }
}

#Preview {
    ChannelPurchaseView(
        store: ChannelStore(),
        activeChannel: .constant("soluna")
    )
}
