/**
 * Soluna — Authentication Manager Implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/security/auth.h>
#include <soluna/security/token.h>
#include <algorithm>
#include <random>
#include <sstream>
#include <iomanip>

namespace soluna {
namespace security {

AuthManager::AuthManager() = default;
AuthManager::~AuthManager() = default;

Result<void> AuthManager::init(const config::SecurityConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    enabled_ = config.auth_enabled;

    // Load devices from config
    for (const auto& dev : config.devices) {
        DeviceCredential cred;
        cred.id = dev.id;
        cred.psk_hash = dev.psk_hash;
        cred.roles = dev.roles;
        cred.enabled = true;
        devices_[cred.id] = std::move(cred);
    }

    return Result<void>::success();
}

Result<void> AuthManager::add_device(const DeviceCredential& credential) {
    if (credential.id.empty()) {
        return Error(ErrorCode::InvalidArgument, "Device ID cannot be empty");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    devices_[credential.id] = credential;
    return Result<void>::success();
}

Result<void> AuthManager::remove_device(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = devices_.find(device_id);
    if (it == devices_.end()) {
        return Error(ErrorCode::NotFound, "Device not found", device_id);
    }

    devices_.erase(it);

    // Also invalidate any sessions for this device
    for (auto sit = sessions_.begin(); sit != sessions_.end(); ) {
        if (sit->second.device_id == device_id) {
            sit = sessions_.erase(sit);
        } else {
            ++sit;
        }
    }

    return Result<void>::success();
}

const DeviceCredential* AuthManager::get_device(const std::string& device_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(device_id);
    return it != devices_.end() ? &it->second : nullptr;
}

std::vector<std::string> AuthManager::list_devices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    result.reserve(devices_.size());
    for (const auto& [id, _] : devices_) {
        result.push_back(id);
    }
    return result;
}

std::string AuthManager::generate_challenge(const std::string& device_id) {
    std::string challenge = Token::generate_nonce();

    std::lock_guard<std::mutex> lock(mutex_);
    pending_challenges_[device_id] = challenge;

    return challenge;
}

AuthResult AuthManager::authenticate(const std::string& device_id,
                                      const std::string& challenge,
                                      const std::string& response,
                                      const std::string& remote_address) {
    AuthResult result;
    result.device_id = device_id;

    emit_event(AuthEvent::LoginAttempt, device_id, remote_address);

    std::lock_guard<std::mutex> lock(mutex_);

    // Check if device exists
    auto dev_it = devices_.find(device_id);
    if (dev_it == devices_.end()) {
        result.error_message = "Unknown device";
        emit_event(AuthEvent::LoginFailed, device_id, "Unknown device");
        return result;
    }

    if (!dev_it->second.enabled) {
        result.error_message = "Device disabled";
        emit_event(AuthEvent::LoginFailed, device_id, "Device disabled");
        return result;
    }

    // Verify challenge
    auto challenge_it = pending_challenges_.find(device_id);
    if (challenge_it == pending_challenges_.end() || challenge_it->second != challenge) {
        result.error_message = "Invalid challenge";
        emit_event(AuthEvent::LoginFailed, device_id, "Invalid challenge");
        return result;
    }

    // Verify response: expected = SHA256(challenge + psk)
    // Note: psk_hash is SHA256(psk), we need to verify against SHA256(challenge + psk)
    // For simplicity, we store the PSK hash and verify: SHA256(challenge + PSK) = response
    // This requires client to know PSK, not just hash

    // Expected: client computes SHA256(challenge + psk), we verify
    // We have psk_hash = SHA256(psk)
    // Simple approach: response should be SHA256(challenge + psk_hash)
    std::string expected = Token::sha256_hex(challenge + dev_it->second.psk_hash);

    if (!Token::secure_compare(response, expected)) {
        result.error_message = "Authentication failed";
        emit_event(AuthEvent::LoginFailed, device_id, "Invalid response");
        pending_challenges_.erase(challenge_it);
        return result;
    }

    // Success - create session
    pending_challenges_.erase(challenge_it);

    Session session;
    session.token = Token::generate_base64();
    session.device_id = device_id;
    session.roles = dev_it->second.roles;
    session.created_at = std::chrono::steady_clock::now();
    session.last_activity = session.created_at;
    session.ttl = default_ttl_;
    session.remote_address = remote_address;

    sessions_[session.token] = session;

    result.success = true;
    result.session_token = session.token;
    result.roles = session.roles;

    emit_event(AuthEvent::LoginSuccess, device_id, "");
    emit_event(AuthEvent::SessionCreated, device_id, session.token);

    return result;
}

AuthResult AuthManager::authenticate_psk(const std::string& device_id,
                                          const std::string& psk,
                                          const std::string& remote_address) {
    AuthResult result;
    result.device_id = device_id;

    emit_event(AuthEvent::LoginAttempt, device_id, remote_address);

    std::lock_guard<std::mutex> lock(mutex_);

    auto dev_it = devices_.find(device_id);
    if (dev_it == devices_.end()) {
        result.error_message = "Unknown device";
        emit_event(AuthEvent::LoginFailed, device_id, "Unknown device");
        return result;
    }

    if (!dev_it->second.enabled) {
        result.error_message = "Device disabled";
        emit_event(AuthEvent::LoginFailed, device_id, "Device disabled");
        return result;
    }

    // Verify PSK
    if (!verify_psk(psk, dev_it->second.psk_hash)) {
        result.error_message = "Invalid PSK";
        emit_event(AuthEvent::LoginFailed, device_id, "Invalid PSK");
        return result;
    }

    // Create session
    Session session;
    session.token = Token::generate_base64();
    session.device_id = device_id;
    session.roles = dev_it->second.roles;
    session.created_at = std::chrono::steady_clock::now();
    session.last_activity = session.created_at;
    session.ttl = default_ttl_;
    session.remote_address = remote_address;

    sessions_[session.token] = session;

    result.success = true;
    result.session_token = session.token;
    result.roles = session.roles;

    emit_event(AuthEvent::LoginSuccess, device_id, "");
    emit_event(AuthEvent::SessionCreated, device_id, session.token);

    return result;
}

Result<Session> AuthManager::validate_token(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sessions_.find(token);
    if (it == sessions_.end()) {
        return Error(ErrorCode::TokenInvalid, "Session not found");
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.last_activity);

    if (elapsed > it->second.ttl) {
        std::string device_id = it->second.device_id;
        sessions_.erase(it);
        emit_event(AuthEvent::TokenExpired, device_id, token);
        return Error(ErrorCode::TokenExpired, "Session expired");
    }

    return it->second;
}

Result<void> AuthManager::touch_session(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sessions_.find(token);
    if (it == sessions_.end()) {
        return Error(ErrorCode::TokenInvalid, "Session not found");
    }

    it->second.last_activity = std::chrono::steady_clock::now();
    return Result<void>::success();
}

Result<void> AuthManager::logout(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sessions_.find(token);
    if (it == sessions_.end()) {
        return Error(ErrorCode::TokenInvalid, "Session not found");
    }

    std::string device_id = it->second.device_id;
    sessions_.erase(it);

    emit_event(AuthEvent::Logout, device_id, token);
    emit_event(AuthEvent::SessionDestroyed, device_id, token);

    return Result<void>::success();
}

Result<void> AuthManager::logout_device(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        if (it->second.device_id == device_id) {
            emit_event(AuthEvent::SessionDestroyed, device_id, it->first);
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }

    return Result<void>::success();
}

size_t AuthManager::active_session_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

void AuthManager::cleanup_expired_sessions() {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::steady_clock::now();

    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - it->second.last_activity);

        if (elapsed > it->second.ttl) {
            emit_event(AuthEvent::TokenExpired, it->second.device_id, it->first);
            emit_event(AuthEvent::SessionDestroyed, it->second.device_id, it->first);
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

std::string AuthManager::hash_psk(const std::string& psk) {
    return "sha256:" + Token::sha256_hex(psk);
}

bool AuthManager::verify_psk(const std::string& psk, const std::string& hash) {
    // Extract hash type prefix
    size_t colon = hash.find(':');
    if (colon == std::string::npos) {
        // No prefix, assume SHA-256
        return Token::secure_compare(Token::sha256_hex(psk), hash);
    }

    std::string type = hash.substr(0, colon);
    std::string stored_hash = hash.substr(colon + 1);

    if (type == "sha256") {
        return Token::secure_compare(Token::sha256_hex(psk), stored_hash);
    }

    return false;
}

std::string AuthManager::generate_token(size_t length) {
    return Token::generate_base64(length);
}

void AuthManager::emit_event(AuthEvent event, const std::string& device_id, const std::string& details) {
    if (event_callback_) {
        event_callback_(event, device_id, details);
    }
}

bool AuthContext::has_role(const std::string& role) const {
    return std::find(roles.begin(), roles.end(), role) != roles.end();
}

bool AuthContext::has_any_role(const std::vector<std::string>& check_roles) const {
    for (const auto& role : check_roles) {
        if (has_role(role)) {
            return true;
        }
    }
    return false;
}

} // namespace security
} // namespace soluna
