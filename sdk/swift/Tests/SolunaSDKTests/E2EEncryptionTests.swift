import XCTest
@testable import SolunaSDK

final class E2EEncryptionTests: XCTestCase {

    func testKeyPairGeneration() {
        let enc = E2EEncryption()
        let pubKey = enc.generateKeyPair()
        // P256 public key raw representation = 64 bytes (x + y coordinates)
        XCTAssertEqual(pubKey.count, 64)
        XCTAssertNotNil(enc.publicKeyBase64)
    }

    func testPublicKeyBase64NilBeforeGeneration() {
        let enc = E2EEncryption()
        XCTAssertNil(enc.publicKeyBase64)
    }

    func testECDHKeyExchangeBetweenTwoInstances() {
        let alice = E2EEncryption()
        let bob = E2EEncryption()

        let alicePub = alice.generateKeyPair()
        let bobPub = bob.generateKeyPair()

        XCTAssertTrue(alice.deriveSessionKey(peerPublicKey: bobPub))
        XCTAssertTrue(bob.deriveSessionKey(peerPublicKey: alicePub))
    }

    func testEncryptThenDecryptReturnsOriginal() throws {
        let alice = E2EEncryption()
        let bob = E2EEncryption()

        let alicePub = alice.generateKeyPair()
        let bobPub = bob.generateKeyPair()

        alice.enable(peerPublicKey: bobPub)
        bob.enable(peerPublicKey: alicePub)

        XCTAssertTrue(alice.isEnabled)
        XCTAssertTrue(bob.isEnabled)

        let original = Data("Hello, Soluna audio stream!".utf8)
        let encrypted = try alice.encrypt(payload: original)

        // Encrypted data should be different from original
        XCTAssertNotEqual(encrypted, original)

        let decrypted = bob.decrypt(payload: encrypted)
        XCTAssertNotNil(decrypted)
        XCTAssertEqual(decrypted, original)
    }

    func testDecryptWithWrongKeyFails() throws {
        let alice = E2EEncryption()
        let bob = E2EEncryption()
        let eve = E2EEncryption()

        let alicePub = alice.generateKeyPair()
        let bobPub = bob.generateKeyPair()
        let evePub = eve.generateKeyPair()

        alice.enable(peerPublicKey: bobPub)
        eve.enable(peerPublicKey: alicePub)  // Eve derives key with Alice, not Bob

        let original = Data("Secret audio data".utf8)
        let encrypted = try alice.encrypt(payload: original)

        // Bob doesn't have the right session key set up
        // Eve has a different session key
        let decrypted = eve.decrypt(payload: encrypted)
        // Decryption should either fail (nil) or produce wrong data
        if let decrypted = decrypted {
            XCTAssertNotEqual(decrypted, original)
        }
        // This is acceptable -- AES-GCM should reject with wrong key
    }

    func testEncryptWithoutSessionKeyThrows() {
        let enc = E2EEncryption()
        enc.generateKeyPair()
        // No session key derived yet
        XCTAssertThrowsError(try enc.encrypt(payload: Data("test".utf8))) { error in
            XCTAssertTrue(error is E2EError)
        }
    }

    func testDecryptWithoutSessionKeyReturnsNil() {
        let enc = E2EEncryption()
        let result = enc.decrypt(payload: Data([0x00, 0x01, 0x02]))
        XCTAssertNil(result)
    }

    func testMultipleEncryptionsProduceDifferentCiphertexts() throws {
        let alice = E2EEncryption()
        let bob = E2EEncryption()

        let alicePub = alice.generateKeyPair()
        let bobPub = bob.generateKeyPair()

        alice.enable(peerPublicKey: bobPub)

        let payload = Data("same data".utf8)
        let encrypted1 = try alice.encrypt(payload: payload)
        let encrypted2 = try alice.encrypt(payload: payload)

        // Different nonces should produce different ciphertexts
        XCTAssertNotEqual(encrypted1, encrypted2)
    }

    func testDisableClearsState() throws {
        let enc = E2EEncryption()
        let other = E2EEncryption()
        let otherPub = other.generateKeyPair()
        enc.generateKeyPair()
        enc.enable(peerPublicKey: otherPub)
        XCTAssertTrue(enc.isEnabled)

        enc.disable()
        XCTAssertFalse(enc.isEnabled)
        XCTAssertNil(enc.publicKeyBase64)
    }

    func testEmptyPayloadEncryptDecrypt() throws {
        let alice = E2EEncryption()
        let bob = E2EEncryption()

        let alicePub = alice.generateKeyPair()
        let bobPub = bob.generateKeyPair()

        alice.enable(peerPublicKey: bobPub)
        bob.enable(peerPublicKey: alicePub)

        let empty = Data()
        let encrypted = try alice.encrypt(payload: empty)
        let decrypted = bob.decrypt(payload: encrypted)
        XCTAssertNotNil(decrypted)
        XCTAssertEqual(decrypted, empty)
    }

    func testKeyOfferMessage() {
        let enc = E2EEncryption()
        XCTAssertNil(enc.keyOfferMessage())
        enc.generateKeyPair()
        let msg = enc.keyOfferMessage()
        XCTAssertNotNil(msg)
        XCTAssertTrue(msg!.hasPrefix("KEY_OFFER:"))
        XCTAssertTrue(msg!.hasSuffix("\n"))
    }

    func testKeyAcceptMessage() {
        let enc = E2EEncryption()
        enc.generateKeyPair()
        let msg = enc.keyAcceptMessage()
        XCTAssertNotNil(msg)
        XCTAssertTrue(msg!.hasPrefix("KEY_ACCEPT:"))
    }

    func testParseKeyExchange() {
        let enc = E2EEncryption()
        let pubKey = enc.generateKeyPair()
        let base64 = pubKey.base64EncodedString()

        let offerMsg = "KEY_OFFER:\(base64)\n"
        let parsed = E2EEncryption.parseKeyExchange(message: offerMsg)
        XCTAssertNotNil(parsed)
        XCTAssertEqual(parsed, pubKey)

        let acceptMsg = "KEY_ACCEPT:\(base64)"
        let parsed2 = E2EEncryption.parseKeyExchange(message: acceptMsg)
        XCTAssertNotNil(parsed2)
        XCTAssertEqual(parsed2, pubKey)

        // Non-key message
        XCTAssertNil(E2EEncryption.parseKeyExchange(message: "HELLO\n"))
    }

    func testDeriveSessionKeyWithInvalidKeyFails() {
        let enc = E2EEncryption()
        enc.generateKeyPair()
        let invalidKey = Data([0x00, 0x01, 0x02])
        XCTAssertFalse(enc.deriveSessionKey(peerPublicKey: invalidKey))
    }

    func testDeriveSessionKeyWithoutKeyPairFails() {
        let enc = E2EEncryption()
        let other = E2EEncryption()
        let otherPub = other.generateKeyPair()
        XCTAssertFalse(enc.deriveSessionKey(peerPublicKey: otherPub))
    }
}
