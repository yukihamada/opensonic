/**
 * Soluna — Authentication Tests
 *
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>
#include <soluna/security/auth.h>
#include <soluna/security/acl.h>
#include <soluna/security/token.h>

using namespace soluna;
using namespace soluna::security;

class AuthTest : public ::testing::Test {
protected:
    void SetUp() override {
        config::SecurityConfig config;
        config.auth_enabled = true;

        config::SecurityConfig::DeviceCredential dev;
        dev.id = "test-device";
        dev.psk_hash = AuthManager::hash_psk("secret123");
        dev.roles = {"stream"};
        config.devices.push_back(dev);

        auth_.init(config);
    }

    AuthManager auth_;
};

TEST_F(AuthTest, IsEnabled) {
    EXPECT_TRUE(auth_.is_enabled());
}

TEST_F(AuthTest, AddDevice) {
    DeviceCredential cred;
    cred.id = "new-device";
    cred.psk_hash = AuthManager::hash_psk("password");
    cred.roles = {"viewer"};

    auto result = auth_.add_device(cred);
    EXPECT_TRUE(result.ok());

    auto devices = auth_.list_devices();
    EXPECT_EQ(devices.size(), 2u);
}

TEST_F(AuthTest, RemoveDevice) {
    auto result = auth_.remove_device("test-device");
    EXPECT_TRUE(result.ok());

    auto devices = auth_.list_devices();
    EXPECT_EQ(devices.size(), 0u);
}

TEST_F(AuthTest, RemoveNonexistentDevice) {
    auto result = auth_.remove_device("nonexistent");
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
}

TEST_F(AuthTest, HashPSK) {
    std::string hash = AuthManager::hash_psk("secret123");
    EXPECT_TRUE(hash.find("sha256:") == 0);
    EXPECT_GT(hash.size(), 10u);
}

TEST_F(AuthTest, VerifyPSK) {
    std::string hash = AuthManager::hash_psk("secret123");
    EXPECT_TRUE(AuthManager::verify_psk("secret123", hash));
    EXPECT_FALSE(AuthManager::verify_psk("wrong", hash));
}

TEST_F(AuthTest, AuthenticatePSK) {
    auto result = auth_.authenticate_psk("test-device", "secret123", "127.0.0.1");

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.device_id, "test-device");
    EXPECT_FALSE(result.session_token.empty());
    EXPECT_EQ(result.roles.size(), 1u);
    EXPECT_EQ(result.roles[0], "stream");
}

TEST_F(AuthTest, AuthenticatePSKWrongPassword) {
    auto result = auth_.authenticate_psk("test-device", "wrong", "127.0.0.1");

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error_message.empty());
}

TEST_F(AuthTest, AuthenticatePSKUnknownDevice) {
    auto result = auth_.authenticate_psk("unknown", "secret123", "127.0.0.1");

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_message, "Unknown device");
}

TEST_F(AuthTest, ChallengeResponse) {
    std::string challenge = auth_.generate_challenge("test-device");
    EXPECT_FALSE(challenge.empty());

    // Client computes: SHA256(challenge + psk_hash)
    // The auth manager uses the full psk_hash including "sha256:" prefix
    std::string psk_hash = AuthManager::hash_psk("secret123");
    std::string response = Token::sha256_hex(challenge + psk_hash);

    auto result = auth_.authenticate("test-device", challenge, response, "127.0.0.1");
    EXPECT_TRUE(result.success) << "Error: " << result.error_message;
}

TEST_F(AuthTest, SessionValidation) {
    auto auth_result = auth_.authenticate_psk("test-device", "secret123");
    ASSERT_TRUE(auth_result.success);

    auto session_result = auth_.validate_token(auth_result.session_token);
    ASSERT_TRUE(session_result.ok());

    const Session& session = session_result.value();
    EXPECT_EQ(session.device_id, "test-device");
    EXPECT_FALSE(session.roles.empty());
}

TEST_F(AuthTest, SessionInvalidToken) {
    auto result = auth_.validate_token("invalid-token");
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), ErrorCode::TokenInvalid);
}

TEST_F(AuthTest, Logout) {
    auto auth_result = auth_.authenticate_psk("test-device", "secret123");
    ASSERT_TRUE(auth_result.success);

    auto logout_result = auth_.logout(auth_result.session_token);
    EXPECT_TRUE(logout_result.ok());

    // Token should now be invalid
    auto validate_result = auth_.validate_token(auth_result.session_token);
    EXPECT_FALSE(validate_result.ok());
}

TEST_F(AuthTest, LogoutDevice) {
    // Create multiple sessions
    auto result1 = auth_.authenticate_psk("test-device", "secret123");
    auto result2 = auth_.authenticate_psk("test-device", "secret123");
    ASSERT_TRUE(result1.success);
    ASSERT_TRUE(result2.success);

    EXPECT_EQ(auth_.active_session_count(), 2u);

    auth_.logout_device("test-device");

    EXPECT_EQ(auth_.active_session_count(), 0u);
}

TEST_F(AuthTest, TouchSession) {
    auto auth_result = auth_.authenticate_psk("test-device", "secret123");
    ASSERT_TRUE(auth_result.success);

    auto touch_result = auth_.touch_session(auth_result.session_token);
    EXPECT_TRUE(touch_result.ok());
}

TEST_F(AuthTest, EventCallback) {
    bool callback_called = false;
    AuthEvent received_event;
    std::string received_device;

    auth_.set_event_callback([&](AuthEvent event, const std::string& device, const std::string&) {
        callback_called = true;
        received_event = event;
        received_device = device;
    });

    auth_.authenticate_psk("test-device", "secret123");

    EXPECT_TRUE(callback_called);
    // Last event should be SessionCreated
    EXPECT_EQ(received_event, AuthEvent::SessionCreated);
    EXPECT_EQ(received_device, "test-device");
}

// ACL Tests
class ACLTest : public ::testing::Test {
protected:
    void SetUp() override {
        acl_.load_defaults();
    }

    ACL acl_;
};

TEST_F(ACLTest, DefaultRoles) {
    auto roles = acl_.list_roles();
    EXPECT_GE(roles.size(), 4u);  // admin, operator, viewer, stream

    EXPECT_NE(acl_.get_role(roles::ADMIN), nullptr);
    EXPECT_NE(acl_.get_role(roles::OPERATOR), nullptr);
    EXPECT_NE(acl_.get_role(roles::VIEWER), nullptr);
    EXPECT_NE(acl_.get_role(roles::STREAM), nullptr);
}

TEST_F(ACLTest, AdminHasAllPermissions) {
    EXPECT_TRUE(acl_.role_has_permission(roles::ADMIN, permissions::STREAM_CREATE));
    EXPECT_TRUE(acl_.role_has_permission(roles::ADMIN, permissions::CONFIG_WRITE));
    EXPECT_TRUE(acl_.role_has_permission(roles::ADMIN, permissions::DEVICE_MANAGE));
}

TEST_F(ACLTest, ViewerHasLimitedPermissions) {
    EXPECT_TRUE(acl_.role_has_permission(roles::VIEWER, permissions::STREAM_VIEW));
    EXPECT_TRUE(acl_.role_has_permission(roles::VIEWER, permissions::CONFIG_READ));
    EXPECT_FALSE(acl_.role_has_permission(roles::VIEWER, permissions::CONFIG_WRITE));
    EXPECT_FALSE(acl_.role_has_permission(roles::VIEWER, permissions::STREAM_CREATE));
}

TEST_F(ACLTest, DefineCustomRole) {
    auto result = acl_.define_role("custom", {permissions::STREAM_VIEW, permissions::ROUTE_VIEW}, "Custom role");
    EXPECT_TRUE(result.ok());

    EXPECT_TRUE(acl_.role_has_permission("custom", permissions::STREAM_VIEW));
    EXPECT_FALSE(acl_.role_has_permission("custom", permissions::STREAM_CREATE));
}

TEST_F(ACLTest, CheckPermission) {
    AuthContext ctx;
    ctx.authenticated = true;
    ctx.roles = {roles::OPERATOR};

    EXPECT_TRUE(acl_.check_permission(ctx, permissions::STREAM_CREATE));
    EXPECT_TRUE(acl_.check_permission(ctx, permissions::ROUTE_MODIFY));
    EXPECT_FALSE(acl_.check_permission(ctx, permissions::DEVICE_MANAGE));
}

TEST_F(ACLTest, CheckPermissionUnauthenticated) {
    AuthContext ctx;
    ctx.authenticated = false;

    EXPECT_FALSE(acl_.check_permission(ctx, permissions::STREAM_VIEW));
}

TEST_F(ACLTest, CheckAllPermissions) {
    AuthContext ctx;
    ctx.authenticated = true;
    ctx.roles = {roles::OPERATOR};

    EXPECT_TRUE(acl_.check_all_permissions(ctx, {permissions::STREAM_CREATE, permissions::ROUTE_CREATE}));
    EXPECT_FALSE(acl_.check_all_permissions(ctx, {permissions::STREAM_CREATE, permissions::DEVICE_MANAGE}));
}

TEST_F(ACLTest, CheckAnyPermission) {
    AuthContext ctx;
    ctx.authenticated = true;
    ctx.roles = {roles::VIEWER};

    EXPECT_TRUE(acl_.check_any_permission(ctx, {permissions::STREAM_VIEW, permissions::DEVICE_MANAGE}));
    EXPECT_FALSE(acl_.check_any_permission(ctx, {permissions::STREAM_CREATE, permissions::DEVICE_MANAGE}));
}

TEST_F(ACLTest, RequirePermission) {
    AuthContext ctx;
    ctx.authenticated = true;
    ctx.roles = {roles::VIEWER};

    auto result = acl_.require_permission(ctx, permissions::STREAM_VIEW);
    EXPECT_TRUE(result.ok());

    auto denied = acl_.require_permission(ctx, permissions::STREAM_CREATE);
    EXPECT_FALSE(denied.ok());
    EXPECT_EQ(denied.error().code(), ErrorCode::AccessDenied);
}

TEST_F(ACLTest, RequirePermissionUnauthenticated) {
    AuthContext ctx;
    ctx.authenticated = false;

    auto result = acl_.require_permission(ctx, permissions::STREAM_VIEW);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), ErrorCode::AuthenticationRequired);
}

// Token Tests
TEST(TokenTest, Generate) {
    std::string token1 = Token::generate(32);
    std::string token2 = Token::generate(32);

    EXPECT_EQ(token1.size(), 32u);
    EXPECT_NE(token1, token2);
}

TEST(TokenTest, GenerateBase64) {
    std::string token = Token::generate_base64(24);
    EXPECT_FALSE(token.empty());

    // URL-safe base64 should not contain + or /
    EXPECT_EQ(token.find('+'), std::string::npos);
    EXPECT_EQ(token.find('/'), std::string::npos);
}

TEST(TokenTest, SHA256) {
    std::string hash = Token::sha256_hex("hello");
    EXPECT_EQ(hash.size(), 64u);  // 256 bits = 64 hex chars
    EXPECT_EQ(hash, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
}

TEST(TokenTest, Base64Roundtrip) {
    std::string original = "Hello, World!";
    std::string encoded = Token::base64_encode(original);
    std::string decoded = Token::base64_decode(encoded);

    EXPECT_EQ(decoded, original);
}

TEST(TokenTest, Base64URLRoundtrip) {
    std::string original = "Hello World Test";
    std::string encoded = Token::base64url_encode(original);
    std::string decoded = Token::base64url_decode(encoded);

    EXPECT_EQ(decoded, original);
}

TEST(TokenTest, SecureCompare) {
    EXPECT_TRUE(Token::secure_compare("abc", "abc"));
    EXPECT_FALSE(Token::secure_compare("abc", "abd"));
    EXPECT_FALSE(Token::secure_compare("abc", "ab"));
}

// AuthContext Tests
TEST(AuthContextTest, HasRole) {
    AuthContext ctx;
    ctx.roles = {"admin", "operator"};

    EXPECT_TRUE(ctx.has_role("admin"));
    EXPECT_TRUE(ctx.has_role("operator"));
    EXPECT_FALSE(ctx.has_role("viewer"));
}

TEST(AuthContextTest, HasAnyRole) {
    AuthContext ctx;
    ctx.roles = {"viewer"};

    EXPECT_TRUE(ctx.has_any_role({"admin", "viewer"}));
    EXPECT_FALSE(ctx.has_any_role({"admin", "operator"}));
}

// Authorizer Tests
TEST(AuthorizerTest, AuthDisabled) {
    ACL acl;
    acl.load_defaults();
    Authorizer auth(acl, false);  // Auth disabled

    AuthContext ctx;
    ctx.authenticated = false;  // Not authenticated

    // Should still allow because auth is disabled
    auto result = auth.authorize(ctx, permissions::ADMIN);
    EXPECT_TRUE(result.ok());
}

TEST(AuthorizerTest, AuthEnabled) {
    ACL acl;
    acl.load_defaults();
    Authorizer auth(acl, true);  // Auth enabled

    AuthContext ctx;
    ctx.authenticated = true;
    ctx.roles = {roles::VIEWER};

    auto allowed = auth.authorize(ctx, permissions::STREAM_VIEW);
    EXPECT_TRUE(allowed.ok());

    auto denied = auth.authorize(ctx, permissions::ADMIN);
    EXPECT_FALSE(denied.ok());
}
