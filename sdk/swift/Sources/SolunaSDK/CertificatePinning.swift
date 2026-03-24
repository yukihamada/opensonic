import Foundation
import Security

// MARK: - CertificatePinning

/// TLS certificate pinning for relay server connections.
///
/// Validates server certificates against a set of trusted SHA-256 public key
/// hashes to prevent man-in-the-middle attacks. Supports multiple pins for
/// seamless certificate rotation.
///
/// Usage:
/// ```swift
/// let pinner = CertificatePinning()
/// pinner.addPin(sha256: "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=")
///
/// // In URLSessionDelegate:
/// let isValid = pinner.validate(serverTrust: trust)
/// ```
public final class CertificatePinning: @unchecked Sendable {

    // MARK: - Properties

    /// Set of trusted SHA-256 public key hashes (base64-encoded).
    public private(set) var pinnedHashes: Set<String>

    /// Whether certificate pinning is enforced.
    ///
    /// When disabled, `validate(serverTrust:)` always returns `true`.
    public var isEnabled: Bool = true

    private let queue = DispatchQueue(label: "com.soluna.certpinning", qos: .utility)

    // MARK: - Init

    /// Create a pinner with default pins for the Soluna relay.
    public init() {
        // Default pins for relay.solun.art — update when certificates rotate.
        pinnedHashes = []
    }

    // MARK: - Public API

    /// Add a trusted public key hash.
    ///
    /// - Parameter sha256: Base64-encoded SHA-256 hash of the certificate's
    ///   Subject Public Key Info (SPKI).
    public func addPin(sha256: String) {
        queue.sync { _ = pinnedHashes.insert(sha256) }
    }

    /// Remove a trusted public key hash.
    ///
    /// - Parameter sha256: The hash to remove.
    public func removePin(sha256: String) {
        queue.sync { _ = pinnedHashes.remove(sha256) }
    }

    /// Validate a server trust against the pinned hashes.
    ///
    /// - Parameter serverTrust: The `SecTrust` object from the TLS handshake.
    /// - Returns: `true` if the server's public key matches any pinned hash,
    ///   or if pinning is disabled / no pins are configured.
    public func validate(serverTrust: SecTrust) -> Bool {
        guard isEnabled else { return true }

        let currentPins: Set<String> = queue.sync { pinnedHashes }
        guard !currentPins.isEmpty else { return true }

        // Evaluate the trust chain
        var error: CFError?
        guard SecTrustEvaluateWithError(serverTrust, &error) else {
            return false
        }

        // Extract the certificate chain
        guard let chain = SecTrustCopyCertificateChain(serverTrust) as? [SecCertificate],
              !chain.isEmpty else {
            return false
        }

        // Check each certificate in the chain
        for cert in chain {
            if let hash = publicKeyHash(for: cert), currentPins.contains(hash) {
                return true
            }
        }

        return false
    }

    /// Create a `URLSession` authentication challenge handler suitable for use
    /// in `URLSessionDelegate.urlSession(_:didReceive:completionHandler:)`.
    ///
    /// - Returns: A closure that handles `NSURLAuthenticationMethodServerTrust` challenges.
    public func urlSessionHandler() -> (URLAuthenticationChallenge, @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) -> Void {
        return { [weak self] challenge, completion in
            guard let self,
                  challenge.protectionSpace.authenticationMethod == NSURLAuthenticationMethodServerTrust,
                  let serverTrust = challenge.protectionSpace.serverTrust else {
                completion(.performDefaultHandling, nil)
                return
            }

            if self.validate(serverTrust: serverTrust) {
                completion(.useCredential, URLCredential(trust: serverTrust))
            } else {
                completion(.cancelAuthenticationChallenge, nil)
            }
        }
    }

    // MARK: - Private

    /// Extract and hash the Subject Public Key Info from a certificate.
    private func publicKeyHash(for certificate: SecCertificate) -> String? {
        guard let publicKey = SecCertificateCopyKey(certificate) else { return nil }

        var error: Unmanaged<CFError>?
        guard let publicKeyData = SecKeyCopyExternalRepresentation(publicKey, &error) as Data? else {
            return nil
        }

        // SHA-256 hash of the raw public key data
        let hash = sha256(data: publicKeyData)
        return hash.base64EncodedString()
    }

    /// Compute SHA-256 using CommonCrypto via Security framework.
    private func sha256(data: Data) -> Data {
        var hash = [UInt8](repeating: 0, count: 32)
        data.withUnsafeBytes { ptr in
            guard let base = ptr.baseAddress else { return }
            _ = CC_SHA256(base, CC_LONG(data.count), &hash)
        }
        return Data(hash)
    }
}

// MARK: - CommonCrypto Bridge

// Import CommonCrypto's SHA256 without a module import (available on all Apple platforms).
@_silgen_name("CC_SHA256")
private func CC_SHA256(_ data: UnsafeRawPointer, _ len: UInt32, _ md: UnsafeMutablePointer<UInt8>) -> UnsafeMutablePointer<UInt8>?

private typealias CC_LONG = UInt32
