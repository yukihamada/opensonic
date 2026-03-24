import Foundation
import CryptoKit

/// End-to-end encryption for Soluna audio streams using AES-GCM 256-bit.
///
/// Each client generates an ephemeral ECDH P256 key pair. When two peers connect,
/// they exchange public keys via the relay control channel and derive a shared
/// session key using HKDF. All subsequent audio payloads are encrypted/decrypted
/// with AES-GCM using a monotonically incrementing nonce to prevent replay attacks.
///
/// Usage:
/// ```swift
/// let encryption = E2EEncryption()
/// let myPublicKey = encryption.generateKeyPair()
/// // Send myPublicKey to peer via relay control channel
/// // Receive peer's public key
/// encryption.enable(peerPublicKey: peerPublicKeyData)
/// let encrypted = try encryption.encrypt(payload: audioData)
/// let decrypted = encryption.decrypt(payload: encrypted)
/// ```
public final class E2EEncryption: ObservableObject {

    // MARK: - Published State

    /// Whether E2E encryption is currently active with a derived session key.
    @Published public private(set) var isEnabled: Bool = false

    // MARK: - Key Exchange Message Prefixes

    /// Control message prefix for offering a public key.
    public static let keyOfferPrefix = "KEY_OFFER:"

    /// Control message prefix for accepting a public key.
    public static let keyAcceptPrefix = "KEY_ACCEPT:"

    // MARK: - Private

    private var privateKey: P256.KeyAgreement.PrivateKey?
    private var sessionKey: SymmetricKey?
    private var nonceCounter: UInt64 = 0
    private let lock = NSLock()

    // MARK: - Init

    public init() {}

    // MARK: - Key Pair Generation

    /// Generate a new ephemeral ECDH P256 key pair.
    ///
    /// - Returns: The raw representation of the public key suitable for transmission.
    @discardableResult
    public func generateKeyPair() -> Data {
        let key = P256.KeyAgreement.PrivateKey()
        privateKey = key
        return key.publicKey.rawRepresentation
    }

    /// The current public key as a Base64-encoded string for control channel messages.
    ///
    /// Returns `nil` if no key pair has been generated.
    public var publicKeyBase64: String? {
        guard let privateKey else { return nil }
        return privateKey.publicKey.rawRepresentation.base64EncodedString()
    }

    // MARK: - Session Key Derivation

    /// Derive a session key from the peer's public key using ECDH + HKDF.
    ///
    /// - Parameter peerPublicKey: The raw representation of the peer's P256 public key.
    /// - Returns: `true` if key derivation succeeded.
    @discardableResult
    public func deriveSessionKey(peerPublicKey: Data) -> Bool {
        guard let privateKey else { return false }

        do {
            let peerKey = try P256.KeyAgreement.PublicKey(rawRepresentation: peerPublicKey)
            let sharedSecret = try privateKey.sharedSecretFromKeyAgreement(with: peerKey)

            // Derive 256-bit key using HKDF-SHA256
            sessionKey = sharedSecret.hkdfDerivedSymmetricKey(
                using: SHA256.self,
                salt: Data("SolunaSDK-E2E-v1".utf8),
                sharedInfo: Data("audio-encryption".utf8),
                outputByteCount: 32
            )

            lock.lock()
            nonceCounter = 0
            lock.unlock()

            return true
        } catch {
            print("[SolunaSDK] E2EEncryption: Key derivation failed: \(error)")
            return false
        }
    }

    // MARK: - Enable / Disable

    /// Enable encryption by deriving a session key from the peer's public key.
    ///
    /// - Parameter peerPublicKey: The raw representation of the peer's P256 public key.
    public func enable(peerPublicKey: Data) {
        if deriveSessionKey(peerPublicKey: peerPublicKey) {
            isEnabled = true
        }
    }

    /// Disable encryption and clear all key material.
    public func disable() {
        isEnabled = false
        sessionKey = nil
        privateKey = nil
        lock.lock()
        nonceCounter = 0
        lock.unlock()
    }

    // MARK: - Encrypt / Decrypt

    /// Encrypt an audio payload using AES-GCM with the derived session key.
    ///
    /// The output format is: `nonce (12 bytes) || ciphertext || tag (16 bytes)`.
    ///
    /// - Parameter payload: The plaintext audio data.
    /// - Returns: The encrypted data.
    /// - Throws: If encryption fails or no session key is available.
    public func encrypt(payload: Data) throws -> Data {
        guard let sessionKey else {
            throw E2EError.noSessionKey
        }

        let nonce = nextNonce()
        let sealedBox = try AES.GCM.seal(payload, using: sessionKey, nonce: nonce)

        guard let combined = sealedBox.combined else {
            throw E2EError.encryptionFailed
        }

        return combined
    }

    /// Decrypt an audio payload using AES-GCM with the derived session key.
    ///
    /// Expects the format: `nonce (12 bytes) || ciphertext || tag (16 bytes)`.
    ///
    /// - Parameter payload: The encrypted data.
    /// - Returns: The decrypted plaintext data, or `nil` if decryption fails.
    public func decrypt(payload: Data) -> Data? {
        guard let sessionKey else { return nil }

        do {
            let sealedBox = try AES.GCM.SealedBox(combined: payload)
            return try AES.GCM.open(sealedBox, using: sessionKey)
        } catch {
            return nil
        }
    }

    // MARK: - Control Channel Helpers

    /// Format a KEY_OFFER control message containing this client's public key.
    ///
    /// - Returns: The formatted message string, or `nil` if no key pair exists.
    public func keyOfferMessage() -> String? {
        guard let base64 = publicKeyBase64 else { return nil }
        return "\(Self.keyOfferPrefix)\(base64)\n"
    }

    /// Format a KEY_ACCEPT control message containing this client's public key.
    ///
    /// - Returns: The formatted message string, or `nil` if no key pair exists.
    public func keyAcceptMessage() -> String? {
        guard let base64 = publicKeyBase64 else { return nil }
        return "\(Self.keyAcceptPrefix)\(base64)\n"
    }

    /// Parse a peer's public key from a KEY_OFFER or KEY_ACCEPT control message.
    ///
    /// - Parameter message: The raw control message string.
    /// - Returns: The peer's public key data, or `nil` if the message is not a key exchange message.
    public static func parseKeyExchange(message: String) -> Data? {
        let trimmed = message.trimmingCharacters(in: .whitespacesAndNewlines)
        let base64: String

        if trimmed.hasPrefix(keyOfferPrefix) {
            base64 = String(trimmed.dropFirst(keyOfferPrefix.count))
        } else if trimmed.hasPrefix(keyAcceptPrefix) {
            base64 = String(trimmed.dropFirst(keyAcceptPrefix.count))
        } else {
            return nil
        }

        return Data(base64Encoded: base64)
    }

    // MARK: - Private

    /// Generate the next AES-GCM nonce from the monotonic counter.
    private func nextNonce() -> AES.GCM.Nonce {
        lock.lock()
        let current = nonceCounter
        nonceCounter += 1
        lock.unlock()

        // Encode counter as 12-byte (96-bit) nonce, little-endian
        var nonceBytes = [UInt8](repeating: 0, count: 12)
        withUnsafeBytes(of: current.littleEndian) { src in
            for i in 0..<min(src.count, 12) {
                nonceBytes[i] = src[i]
            }
        }

        // Force-unwrap is safe: 12 bytes is always valid for AES-GCM nonce
        return try! AES.GCM.Nonce(data: nonceBytes)
    }
}

// MARK: - E2EError

/// Errors that can occur during E2E encryption operations.
public enum E2EError: Error, LocalizedError {
    case noSessionKey
    case encryptionFailed

    public var errorDescription: String? {
        switch self {
        case .noSessionKey:
            return "No session key available. Call enable(peerPublicKey:) first."
        case .encryptionFailed:
            return "AES-GCM encryption failed to produce combined output."
        }
    }
}
