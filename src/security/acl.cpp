/**
 * Soluna — Access Control List Implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/security/acl.h>
#include <algorithm>

namespace soluna {
namespace security {

ACL::ACL() {
    // Register all standard permissions
    all_permissions_.insert({
        permissions::STREAM_CREATE,
        permissions::STREAM_DELETE,
        permissions::STREAM_MODIFY,
        permissions::STREAM_VIEW,
        permissions::ROUTE_CREATE,
        permissions::ROUTE_DELETE,
        permissions::ROUTE_MODIFY,
        permissions::ROUTE_VIEW,
        permissions::CONFIG_READ,
        permissions::CONFIG_WRITE,
        permissions::DEVICE_MANAGE,
        permissions::METRICS_VIEW,
        permissions::ADMIN,
    });
}

Result<void> ACL::init(const config::SecurityConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Clear existing roles
    roles_.clear();

    // Load default roles first
    load_defaults();

    // Override with config roles
    for (const auto& role_cfg : config.roles) {
        Role role;
        role.name = role_cfg.role;
        for (const auto& perm : role_cfg.permissions) {
            role.permissions.insert(perm);
            all_permissions_.insert(perm);
        }
        roles_[role.name] = std::move(role);
    }

    return Result<void>::success();
}

void ACL::load_defaults() {
    // Admin role - all permissions
    {
        Role role;
        role.name = roles::ADMIN;
        role.description = "Full administrative access";
        role.permissions = all_permissions_;
        roles_[role.name] = std::move(role);
    }

    // Operator role - stream and route management
    {
        Role role;
        role.name = roles::OPERATOR;
        role.description = "Stream and route management";
        role.permissions = {
            permissions::STREAM_CREATE,
            permissions::STREAM_DELETE,
            permissions::STREAM_MODIFY,
            permissions::STREAM_VIEW,
            permissions::ROUTE_CREATE,
            permissions::ROUTE_DELETE,
            permissions::ROUTE_MODIFY,
            permissions::ROUTE_VIEW,
            permissions::CONFIG_READ,
            permissions::METRICS_VIEW,
        };
        roles_[role.name] = std::move(role);
    }

    // Viewer role - read-only access
    {
        Role role;
        role.name = roles::VIEWER;
        role.description = "Read-only access";
        role.permissions = {
            permissions::STREAM_VIEW,
            permissions::ROUTE_VIEW,
            permissions::CONFIG_READ,
            permissions::METRICS_VIEW,
        };
        roles_[role.name] = std::move(role);
    }

    // Stream role - basic streaming
    {
        Role role;
        role.name = roles::STREAM;
        role.description = "Basic streaming operations";
        role.permissions = {
            permissions::STREAM_CREATE,
            permissions::STREAM_VIEW,
            permissions::ROUTE_VIEW,
        };
        roles_[role.name] = std::move(role);
    }
}

Result<void> ACL::define_role(const std::string& role_name,
                               const std::vector<std::string>& permissions,
                               const std::string& description) {
    if (role_name.empty()) {
        return Error(ErrorCode::InvalidArgument, "Role name cannot be empty");
    }

    std::lock_guard<std::mutex> lock(mutex_);

    Role role;
    role.name = role_name;
    role.description = description;
    for (const auto& perm : permissions) {
        role.permissions.insert(perm);
        all_permissions_.insert(perm);
    }

    roles_[role_name] = std::move(role);
    return Result<void>::success();
}

Result<void> ACL::remove_role(const std::string& role_name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = roles_.find(role_name);
    if (it == roles_.end()) {
        return Error(ErrorCode::NotFound, "Role not found", role_name);
    }

    roles_.erase(it);
    return Result<void>::success();
}

const Role* ACL::get_role(const std::string& role_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = roles_.find(role_name);
    return it != roles_.end() ? &it->second : nullptr;
}

std::vector<std::string> ACL::list_roles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    result.reserve(roles_.size());
    for (const auto& [name, _] : roles_) {
        result.push_back(name);
    }
    return result;
}

bool ACL::role_has_permission(const std::string& role_name, const std::string& permission) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = roles_.find(role_name);
    if (it == roles_.end()) {
        return false;
    }

    // Admin permission grants everything
    if (it->second.permissions.count(permissions::ADMIN) > 0) {
        return true;
    }

    return it->second.permissions.count(permission) > 0;
}

bool ACL::check_permission(const AuthContext& ctx, const std::string& permission) const {
    if (!ctx.authenticated) {
        return false;
    }

    for (const auto& role : ctx.roles) {
        if (role_has_permission(role, permission)) {
            return true;
        }
    }

    return false;
}

bool ACL::check_all_permissions(const AuthContext& ctx,
                                 const std::vector<std::string>& permissions) const {
    for (const auto& perm : permissions) {
        if (!check_permission(ctx, perm)) {
            return false;
        }
    }
    return true;
}

bool ACL::check_any_permission(const AuthContext& ctx,
                                const std::vector<std::string>& permissions) const {
    for (const auto& perm : permissions) {
        if (check_permission(ctx, perm)) {
            return true;
        }
    }
    return false;
}

Result<void> ACL::require_permission(const AuthContext& ctx, const std::string& permission) const {
    if (!ctx.authenticated) {
        return Error(ErrorCode::AuthenticationRequired, "Authentication required");
    }

    if (!check_permission(ctx, permission)) {
        return Error(ErrorCode::AccessDenied,
                     "Permission denied",
                     "Required: " + permission);
    }

    return Result<void>::success();
}

std::set<std::string> ACL::get_permissions(const std::vector<std::string>& role_names) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::set<std::string> result;

    for (const auto& role_name : role_names) {
        auto it = roles_.find(role_name);
        if (it != roles_.end()) {
            // Admin role has all permissions
            if (it->second.permissions.count(permissions::ADMIN) > 0) {
                return all_permissions_;
            }
            result.insert(it->second.permissions.begin(), it->second.permissions.end());
        }
    }

    return result;
}

std::vector<std::string> ACL::list_permissions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {all_permissions_.begin(), all_permissions_.end()};
}

// Authorizer implementation

Authorizer::Authorizer(const ACL& acl, bool auth_enabled)
    : acl_(acl), auth_enabled_(auth_enabled) {}

Result<void> Authorizer::authorize(const AuthContext& ctx, const std::string& permission) const {
    // If auth is disabled, allow all
    if (!auth_enabled_) {
        return Result<void>::success();
    }

    return acl_.require_permission(ctx, permission);
}

} // namespace security
} // namespace soluna
