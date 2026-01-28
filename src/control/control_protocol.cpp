/**
 * Control Protocol — JSON serialization (minimal, no external JSON lib in Phase 3)
 *
 * Uses a simple hand-rolled JSON serializer/parser sufficient for the
 * control protocol. nlohmann/json will be integrated in Phase 7.
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/control/protocol.h>
#include <algorithm>
#include <sstream>

namespace soluna::control {

static const std::pair<CommandType, const char*> kCommandNames[] = {
    {CommandType::DeviceList,       "device.list"},
    {CommandType::DeviceInfo,       "device.info"},
    {CommandType::StreamList,       "stream.list"},
    {CommandType::StreamCreate,     "stream.create"},
    {CommandType::StreamDestroy,    "stream.destroy"},
    {CommandType::RouteList,        "route.list"},
    {CommandType::RouteAdd,         "route.add"},
    {CommandType::RouteRemove,      "route.remove"},
    {CommandType::RouteSetGain,     "route.set_gain"},
    {CommandType::RouteSetMute,     "route.set_mute"},
    {CommandType::MeterGet,         "meter.get"},
    {CommandType::MeterSubscribe,   "meter.subscribe"},
    {CommandType::MeterUnsubscribe, "meter.unsubscribe"},
    {CommandType::Status,           "status"},
    {CommandType::Version,          "version"},
};

const char* command_to_string(CommandType cmd) {
    for (const auto& [type, name] : kCommandNames) {
        if (type == cmd) return name;
    }
    return "unknown";
}

CommandType string_to_command(const std::string& s) {
    for (const auto& [type, name] : kCommandNames) {
        if (s == name) return type;
    }
    return CommandType::Unknown;
}

// Simple JSON escape
static std::string json_escape(const std::string& s) {
    std::string result;
    result.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:   result += c; break;
        }
    }
    return result;
}

std::string serialize_request(const ControlRequest& req) {
    std::ostringstream ss;
    ss << "{\"id\":" << req.id
       << ",\"command\":\"" << json_escape(command_to_string(req.command)) << "\"";

    if (!req.params.empty()) {
        ss << ",\"params\":{";
        bool first = true;
        for (const auto& [key, val] : req.params) {
            if (!first) ss << ",";
            ss << "\"" << json_escape(key) << "\":\"" << json_escape(val) << "\"";
            first = false;
        }
        ss << "}";
    }

    ss << "}";
    return ss.str();
}

// Minimal JSON parser — extract string values by key
static std::string extract_string(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

static uint32_t extract_uint(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos += search.size();
    // Skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    std::string num;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        num += json[pos++];
    }
    return num.empty() ? 0 : static_cast<uint32_t>(std::stoul(num));
}

static bool extract_bool(const std::string& json, const std::string& key) {
    std::string search_t = "\"" + key + "\":true";
    return json.find(search_t) != std::string::npos;
}

bool parse_request(const std::string& json, ControlRequest& req) {
    req.id = extract_uint(json, "id");
    std::string cmd = extract_string(json, "command");
    if (cmd.empty()) return false;
    req.command = string_to_command(cmd);

    // Extract params object
    auto params_pos = json.find("\"params\":{");
    if (params_pos != std::string::npos) {
        auto start = params_pos + 10;
        auto end = json.find('}', start);
        if (end != std::string::npos) {
            std::string params_str = json.substr(start, end - start);
            // Parse key:value pairs
            size_t pos = 0;
            while (pos < params_str.size()) {
                auto key_start = params_str.find('"', pos);
                if (key_start == std::string::npos) break;
                auto key_end = params_str.find('"', key_start + 1);
                if (key_end == std::string::npos) break;
                std::string key = params_str.substr(key_start + 1, key_end - key_start - 1);

                auto val_start = params_str.find('"', key_end + 2);
                if (val_start == std::string::npos) break;
                auto val_end = params_str.find('"', val_start + 1);
                if (val_end == std::string::npos) break;
                std::string val = params_str.substr(val_start + 1, val_end - val_start - 1);

                req.params[key] = val;
                pos = val_end + 1;
            }
        }
    }

    return true;
}

std::string serialize_response(const ControlResponse& resp) {
    std::ostringstream ss;
    ss << "{\"id\":" << resp.id
       << ",\"success\":" << (resp.success ? "true" : "false");

    if (!resp.error.empty()) {
        ss << ",\"error\":\"" << json_escape(resp.error) << "\"";
    }

    if (!resp.data.empty()) {
        ss << ",\"data\":" << resp.data; // data is already JSON
    }

    ss << "}";
    return ss.str();
}

bool parse_response(const std::string& json, ControlResponse& resp) {
    resp.id = extract_uint(json, "id");
    resp.success = extract_bool(json, "success");
    resp.error = extract_string(json, "error");

    // Extract data (raw JSON value)
    auto data_pos = json.find("\"data\":");
    if (data_pos != std::string::npos) {
        auto start = data_pos + 7;
        // Simplified: take everything until the last }
        auto end = json.rfind('}');
        if (end != std::string::npos && end > start) {
            resp.data = json.substr(start, end - start);
        }
    }

    return true;
}

} // namespace soluna::control
