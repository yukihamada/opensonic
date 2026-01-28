/**
 * Soluna — Audit Log Implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/audit/audit_log.h>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <chrono>

namespace fs = std::filesystem;

namespace soluna {
namespace audit {

// Simple JSON escaping
static std::string json_escape(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    std::ostringstream ss;
                    ss << "\\u" << std::hex << std::setfill('0') << std::setw(4)
                       << static_cast<int>(static_cast<unsigned char>(c));
                    result += ss.str();
                } else {
                    result += c;
                }
        }
    }
    return result;
}

std::string AuditEntry::to_json() const {
    std::ostringstream json;
    json << "{\"ts\":" << timestamp_ns;
    json << ",\"event\":\"" << json_escape(event) << "\"";
    json << ",\"actor\":\"" << json_escape(actor) << "\"";

    if (!target.empty()) {
        json << ",\"target\":\"" << json_escape(target) << "\"";
    }

    json << ",\"success\":" << (success ? "true" : "false");

    if (!details.empty()) {
        // Details is already JSON or a simple string
        if (details[0] == '{' || details[0] == '[') {
            json << ",\"details\":" << details;
        } else {
            json << ",\"details\":\"" << json_escape(details) << "\"";
        }
    }

    if (!remote_address.empty()) {
        json << ",\"remote\":\"" << json_escape(remote_address) << "\"";
    }

    json << "}";
    return json.str();
}

Result<AuditEntry> AuditEntry::from_json(const std::string& json) {
    // Minimal JSON parsing for audit entries
    AuditEntry entry;

    // Find timestamp
    auto ts_pos = json.find("\"ts\":");
    if (ts_pos != std::string::npos) {
        size_t start = ts_pos + 5;
        size_t end = json.find_first_of(",}", start);
        entry.timestamp_ns = std::stoull(json.substr(start, end - start));
    }

    // Find event
    auto ev_pos = json.find("\"event\":\"");
    if (ev_pos != std::string::npos) {
        size_t start = ev_pos + 9;
        size_t end = json.find('"', start);
        entry.event = json.substr(start, end - start);
    }

    // Find actor
    auto actor_pos = json.find("\"actor\":\"");
    if (actor_pos != std::string::npos) {
        size_t start = actor_pos + 9;
        size_t end = json.find('"', start);
        entry.actor = json.substr(start, end - start);
    }

    // Find target
    auto target_pos = json.find("\"target\":\"");
    if (target_pos != std::string::npos) {
        size_t start = target_pos + 10;
        size_t end = json.find('"', start);
        entry.target = json.substr(start, end - start);
    }

    // Find success
    entry.success = json.find("\"success\":true") != std::string::npos;

    return entry;
}

AuditLog::AuditLog() = default;

AuditLog::~AuditLog() {
    if (file_.is_open()) {
        flush();
        file_.close();
    }
}

Result<void> AuditLog::init(const config::AuditConfig& config) {
    AuditConfig audit_config;
    audit_config.enabled = config.enabled;
    audit_config.file_path = config.file;
    audit_config.events = std::set<std::string>(config.events.begin(), config.events.end());
    return init(audit_config);
}

Result<void> AuditLog::init(const AuditConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    config_ = config;
    enabled_ = config.enabled;

    if (!enabled_) {
        return Result<void>::success();
    }

    // Create directory if needed
    fs::path log_path(config_.file_path);
    fs::path dir = log_path.parent_path();
    if (!dir.empty() && !fs::exists(dir)) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            return Error(ErrorCode::ConfigWriteError,
                         "Cannot create audit log directory",
                         dir.string());
        }
    }

    // Open file
    file_.open(config_.file_path, std::ios::app);
    if (!file_.is_open()) {
        return Error(ErrorCode::ConfigWriteError,
                     "Cannot open audit log file",
                     config_.file_path);
    }

    // Get current file size
    file_.seekp(0, std::ios::end);
    current_file_size_ = static_cast<size_t>(file_.tellp());

    return Result<void>::success();
}

bool AuditLog::should_log(const std::string& event) const {
    if (!enabled_) return false;
    if (config_.events.empty()) return true;  // Log all
    return config_.events.count(event) > 0;
}

void AuditLog::log(const std::string& event,
                   const std::string& actor,
                   const std::string& target,
                   bool success,
                   const std::string& details,
                   const std::string& remote_address) {
    if (!should_log(event)) return;

    AuditEntry entry;
    entry.timestamp_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    entry.event = event;
    entry.actor = actor;
    entry.target = target;
    entry.success = success;
    entry.details = details;
    entry.remote_address = remote_address;

    log(entry);
}

void AuditLog::log(const std::string& event,
                   const std::string& actor,
                   const std::string& target,
                   bool success,
                   const std::map<std::string, std::string>& details,
                   const std::string& remote_address) {
    // Convert map to JSON object
    std::ostringstream json;
    json << "{";
    bool first = true;
    for (const auto& [k, v] : details) {
        if (!first) json << ",";
        json << "\"" << json_escape(k) << "\":\"" << json_escape(v) << "\"";
        first = false;
    }
    json << "}";

    log(event, actor, target, success, json.str(), remote_address);
}

void AuditLog::log(const AuditEntry& entry) {
    if (!enabled_) return;

    // Call callback if registered
    if (callback_) {
        callback_(entry);
    }

    write_entry(entry);
}

void AuditLog::write_entry(const AuditEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!file_.is_open()) return;

    std::string json = entry.to_json() + "\n";

    file_ << json;
    current_file_size_ += json.size();
    entry_count_.fetch_add(1);

    if (!config_.async_write) {
        file_.flush();
    }

    rotate_if_needed();
}

void AuditLog::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
    }
}

void AuditLog::rotate_if_needed() {
    // Assumes mutex is held
    if (current_file_size_ < config_.max_file_size) {
        return;
    }

    rotate();
}

void AuditLog::rotate() {
    // Assumes mutex is held
    file_.close();

    // Rotate files: .4 -> .5, .3 -> .4, etc.
    for (size_t i = config_.max_files - 1; i > 0; i--) {
        std::string old_name = config_.file_path + "." + std::to_string(i);
        std::string new_name = config_.file_path + "." + std::to_string(i + 1);

        std::error_code ec;
        if (fs::exists(old_name, ec)) {
            fs::rename(old_name, new_name, ec);
        }
    }

    // Current -> .1
    std::error_code ec;
    fs::rename(config_.file_path, config_.file_path + ".1", ec);

    // Open new file
    file_.open(config_.file_path, std::ios::out);
    current_file_size_ = 0;
}

// Global instance
AuditLog& audit_log() {
    static AuditLog instance;
    return instance;
}

// Convenience functions
void audit_auth_success(const std::string& device_id, const std::string& remote_address) {
    audit_log().log(events::AUTH_SUCCESS, device_id, "", true, "", remote_address);
}

void audit_auth_failure(const std::string& device_id, const std::string& reason,
                        const std::string& remote_address) {
    audit_log().log(events::AUTH_FAILURE, device_id, "", false, reason, remote_address);
}

void audit_logout(const std::string& device_id) {
    audit_log().log(events::LOGOUT, device_id);
}

void audit_stream_created(const std::string& actor, const std::string& stream_id,
                          const std::map<std::string, std::string>& details) {
    audit_log().log(events::STREAM_CREATED, actor, "stream:" + stream_id, true, details);
}

void audit_stream_deleted(const std::string& actor, const std::string& stream_id) {
    audit_log().log(events::STREAM_DELETED, actor, "stream:" + stream_id);
}

void audit_route_created(const std::string& actor, const std::string& source,
                         const std::string& sink) {
    audit_log().log(events::ROUTE_CREATED, actor, source + "->" + sink);
}

void audit_route_deleted(const std::string& actor, const std::string& source,
                         const std::string& sink) {
    audit_log().log(events::ROUTE_DELETED, actor, source + "->" + sink);
}

void audit_config_changed(const std::string& actor, const std::string& key,
                          const std::string& old_value, const std::string& new_value) {
    std::map<std::string, std::string> details;
    details["key"] = key;
    details["old"] = old_value;
    details["new"] = new_value;
    audit_log().log(events::CONFIG_CHANGED, actor, "config:" + key, true, details);
}

void audit_device_connected(const std::string& device_id, const std::string& address) {
    audit_log().log(events::DEVICE_CONNECTED, device_id, "", true, "", address);
}

void audit_device_disconnected(const std::string& device_id, const std::string& reason) {
    audit_log().log(events::DEVICE_DISCONNECTED, device_id, "", true, reason);
}

void audit_startup() {
    audit_log().log(events::STARTUP, "system");
}

void audit_shutdown() {
    audit_log().log(events::SHUTDOWN, "system");
}

void audit_error(const std::string& error_code, const std::string& message) {
    std::map<std::string, std::string> details;
    details["code"] = error_code;
    details["message"] = message;
    audit_log().log(events::ERROR, "system", "", false, details);
}

} // namespace audit
} // namespace soluna
