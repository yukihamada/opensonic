import XCTest
@testable import SolunaSDK

final class AuthManagerTests: XCTestCase {

    // Helper to build a JWT with given payload claims
    private func makeJWT(
        channelId: String = "jazz",
        userId: String = "user123",
        role: String = "dj",
        exp: TimeInterval? = nil,
        iat: TimeInterval? = nil
    ) -> String {
        let expValue = exp ?? (Date().timeIntervalSince1970 + 3600)
        let iatValue = iat ?? Date().timeIntervalSince1970

        let header = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}"
        let payload = """
        {"channelId":"\(channelId)","userId":"\(userId)","role":"\(role)","exp":\(expValue),"iat":\(iatValue)}
        """
        let signature = "fake-signature"

        let headerB64 = Data(header.utf8).base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
        let payloadB64 = Data(payload.utf8).base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
        let sigB64 = Data(signature.utf8).base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")

        return "\(headerB64).\(payloadB64).\(sigB64)"
    }

    func testValidJWTParsing() {
        let jwt = makeJWT(channelId: "jazz", userId: "user42", role: "dj")
        let claims = AuthManager.decodeJWT(jwt)
        XCTAssertNotNil(claims)
        XCTAssertEqual(claims?.channelId, "jazz")
        XCTAssertEqual(claims?.userId, "user42")
        XCTAssertEqual(claims?.role, .dj)
    }

    func testExpiredTokenDetection() {
        let auth = AuthManager()
        let jwt = makeJWT(exp: Date().timeIntervalSince1970 - 100) // expired 100s ago
        auth.setToken(jwt)
        XCTAssertFalse(auth.isAuthenticated)
        XCTAssertEqual(auth.currentRole, .listener) // reset to listener when expired
    }

    func testValidTokenAuthentication() {
        let auth = AuthManager()
        let jwt = makeJWT(role: "admin", exp: Date().timeIntervalSince1970 + 3600)
        auth.setToken(jwt)
        XCTAssertTrue(auth.isAuthenticated)
        XCTAssertEqual(auth.currentRole, .admin)
    }

    func testRoleExtraction() {
        for roleName in ["listener", "dj", "admin", "owner"] {
            let jwt = makeJWT(role: roleName)
            let claims = AuthManager.decodeJWT(jwt)
            XCTAssertNotNil(claims)
            XCTAssertEqual(claims?.role.rawValue, roleName)
        }
    }

    func testInvalidJWTBadBase64() {
        let claims = AuthManager.decodeJWT("not.a.valid-jwt!!!")
        XCTAssertNil(claims)
    }

    func testInvalidJWTMissingParts() {
        XCTAssertNil(AuthManager.decodeJWT("only-one-part"))
        XCTAssertNil(AuthManager.decodeJWT("two.parts"))
    }

    func testInvalidJWTBadJSON() {
        let badPayload = Data("not-json".utf8).base64EncodedString()
        let jwt = "header.\(badPayload).signature"
        XCTAssertNil(AuthManager.decodeJWT(jwt))
    }

    func testChannelIdExtraction() {
        let jwt = makeJWT(channelId: "lofi")
        let claims = AuthManager.decodeJWT(jwt)
        XCTAssertEqual(claims?.channelId, "lofi")
    }

    func testAuthHeaderFormat() {
        let auth = AuthManager()
        let jwt = makeJWT()
        auth.setToken(jwt)
        let header = auth.authHeader()
        XCTAssertTrue(header.hasPrefix("AUTH:"))
        XCTAssertTrue(header.hasSuffix("\n"))
        XCTAssertTrue(header.contains(jwt))
    }

    func testAuthHeaderEmptyWhenNoToken() {
        let auth = AuthManager()
        XCTAssertEqual(auth.authHeader(), "")
    }

    func testClearToken() {
        let auth = AuthManager()
        auth.setToken(makeJWT(role: "owner"))
        XCTAssertTrue(auth.isAuthenticated)
        auth.clearToken()
        XCTAssertFalse(auth.isAuthenticated)
        XCTAssertEqual(auth.currentRole, .listener)
        XCTAssertNil(auth.claims)
    }

    func testHasPermission() {
        let auth = AuthManager()
        auth.setToken(makeJWT(role: "admin"))
        XCTAssertTrue(auth.hasPermission(for: .listener))
        XCTAssertTrue(auth.hasPermission(for: .dj))
        XCTAssertTrue(auth.hasPermission(for: .admin))
        XCTAssertFalse(auth.hasPermission(for: .owner))
    }

    func testHasPermissionWhenNotAuthenticated() {
        let auth = AuthManager()
        XCTAssertFalse(auth.hasPermission(for: .listener))
    }

    func testValidateTokenAfterExpiry() {
        let auth = AuthManager()
        let jwt = makeJWT(exp: Date().timeIntervalSince1970 - 1)
        auth.setToken(jwt)
        XCTAssertFalse(auth.validateToken())
    }

    func testChannelRoleComparable() {
        XCTAssertTrue(ChannelRole.listener < ChannelRole.dj)
        XCTAssertTrue(ChannelRole.dj < ChannelRole.admin)
        XCTAssertTrue(ChannelRole.admin < ChannelRole.owner)
        XCTAssertFalse(ChannelRole.owner < ChannelRole.listener)
    }
}
