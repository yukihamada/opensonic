/**
 * Soluna — Automatic Routing Engine Implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/control/auto_router.h>
#include <algorithm>
#include <sstream>

namespace soluna {
namespace control {

struct AutoRouter::Impl {
    std::vector<RouteRule> rules;
    ActionCallback action_callback;
    mutable std::mutex mutex;
};

AutoRouter::AutoRouter() : impl_(std::make_unique<Impl>()) {}
AutoRouter::~AutoRouter() = default;

void AutoRouter::add_rule(const RouteRule& rule) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    // Insert in priority order (higher priority first)
    auto it = std::find_if(impl_->rules.begin(), impl_->rules.end(),
        [&](const RouteRule& r) { return r.priority < rule.priority; });

    impl_->rules.insert(it, rule);
}

bool AutoRouter::remove_rule(const std::string& name) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    auto it = std::find_if(impl_->rules.begin(), impl_->rules.end(),
        [&](const RouteRule& r) { return r.name == name; });

    if (it != impl_->rules.end()) {
        impl_->rules.erase(it);
        return true;
    }
    return false;
}

void AutoRouter::clear_rules() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->rules.clear();
}

std::vector<RouteRule> AutoRouter::get_rules() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->rules;
}

bool AutoRouter::set_rule_enabled(const std::string& name, bool enabled) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    for (auto& rule : impl_->rules) {
        if (rule.name == name) {
            rule.enabled = enabled;
            return true;
        }
    }
    return false;
}

int AutoRouter::process_event(const RouteEvent& event) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    int matched = 0;

    for (const auto& rule : impl_->rules) {
        if (!rule.enabled) continue;
        if (rule.trigger != event.type) continue;

        // Match pattern against device name
        bool pattern_match = false;
        if (rule.pattern.empty()) {
            pattern_match = true;  // Empty pattern matches all
        } else {
            try {
                std::regex re(rule.pattern);
                pattern_match = std::regex_search(event.device_name, re);
            } catch (const std::regex_error&) {
                // Invalid regex - try simple glob-like matching
                // Convert simple glob to regex: * -> .*, ? -> .
                std::string pattern = rule.pattern;
                std::string regex_pattern;
                for (char c : pattern) {
                    if (c == '*') regex_pattern += ".*";
                    else if (c == '?') regex_pattern += ".";
                    else if (c == '.' || c == '+' || c == '(' || c == ')' ||
                             c == '[' || c == ']' || c == '{' || c == '}' ||
                             c == '^' || c == '$' || c == '|' || c == '\\') {
                        regex_pattern += '\\';
                        regex_pattern += c;
                    }
                    else regex_pattern += c;
                }
                try {
                    std::regex re(regex_pattern);
                    pattern_match = std::regex_search(event.device_name, re);
                } catch (...) {
                    pattern_match = false;
                }
            }
        }

        if (!pattern_match) continue;

        // Rule matched - execute actions
        matched++;

        if (impl_->action_callback) {
            for (const auto& action : rule.actions) {
                impl_->action_callback(action, event);
            }
        }

        if (rule.stop_on_match) break;
    }

    return matched;
}

void AutoRouter::set_action_callback(ActionCallback callback) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->action_callback = std::move(callback);
}

std::string AutoRouter::substitute_variables(const std::string& pattern,
                                              const RouteEvent& event) {
    std::string result = pattern;

    // Built-in variables
    size_t pos;

    // $device -> device_id
    while ((pos = result.find("$device")) != std::string::npos) {
        result.replace(pos, 7, event.device_id);
    }

    // $name -> device_name
    while ((pos = result.find("$name")) != std::string::npos) {
        result.replace(pos, 5, event.device_name);
    }

    // $stream -> stream_id
    while ((pos = result.find("$stream")) != std::string::npos) {
        result.replace(pos, 7, event.stream_id);
    }

    // Custom variables from event context
    for (const auto& var : event.variables) {
        std::string var_pattern = "$" + var.first;
        while ((pos = result.find(var_pattern)) != std::string::npos) {
            result.replace(pos, var_pattern.length(), var.second);
        }
    }

    return result;
}

Result<void> AutoRouter::load_rules(const std::string& yaml_config) {
    // Simple YAML-like parser for routing rules
    // Expected format:
    // auto_rules:
    //   - name: "rule-name"
    //     trigger:
    //       type: device_connected
    //       pattern: "esp32-*"
    //     actions:
    //       - type: add_route
    //         source: "main-out:0"
    //         sink: "$device:0"
    //         gain_db: -6.0

    std::istringstream stream(yaml_config);
    std::string line;
    RouteRule current_rule;
    RouteAction current_action;
    bool in_rules = false;
    bool in_rule = false;
    bool in_trigger = false;
    bool in_actions = false;
    bool in_action = false;

    auto save_action = [&]() {
        if (in_action) {
            current_rule.actions.push_back(current_action);
            current_action = RouteAction{};
            in_action = false;
        }
    };

    auto save_rule = [&]() {
        save_action();
        if (in_rule && !current_rule.name.empty()) {
            add_rule(current_rule);
            current_rule = RouteRule{};
            in_rule = false;
            in_trigger = false;
            in_actions = false;
        }
    };

    while (std::getline(stream, line)) {
        // Skip empty lines and comments
        size_t first_non_space = line.find_first_not_of(" \t");
        if (first_non_space == std::string::npos) continue;
        if (line[first_non_space] == '#') continue;

        // Count indentation
        size_t indent = first_non_space;
        std::string trimmed = line.substr(first_non_space);

        // Remove trailing whitespace
        size_t last = trimmed.find_last_not_of(" \t\r\n");
        if (last != std::string::npos) {
            trimmed = trimmed.substr(0, last + 1);
        }

        // Parse key: value or list item
        if (trimmed.substr(0, 11) == "auto_rules:") {
            in_rules = true;
            continue;
        }

        if (!in_rules) continue;

        if (trimmed[0] == '-' && trimmed.length() > 1) {
            // List item
            std::string item = trimmed.substr(1);
            while (!item.empty() && (item[0] == ' ' || item[0] == '\t')) {
                item = item.substr(1);
            }

            if (indent <= 2) {
                // New rule
                save_rule();
                in_rule = true;
                in_trigger = false;
                in_actions = false;

                // Check if name is on same line
                if (item.substr(0, 5) == "name:") {
                    std::string value = item.substr(5);
                    while (!value.empty() && value[0] == ' ') value = value.substr(1);
                    if (!value.empty() && value[0] == '"') {
                        value = value.substr(1);
                        size_t end = value.find('"');
                        if (end != std::string::npos) value = value.substr(0, end);
                    }
                    current_rule.name = value;
                }
            } else if (in_actions) {
                // New action
                save_action();
                in_action = true;

                // Check if type is on same line
                if (item.substr(0, 5) == "type:") {
                    std::string value = item.substr(5);
                    while (!value.empty() && value[0] == ' ') value = value.substr(1);
                    current_action.type = parse_action_type(value);
                }
            }
            continue;
        }

        // Key: value pair
        size_t colon = trimmed.find(':');
        if (colon == std::string::npos) continue;

        std::string key = trimmed.substr(0, colon);
        std::string value = trimmed.substr(colon + 1);
        while (!value.empty() && value[0] == ' ') value = value.substr(1);

        // Remove quotes
        if (!value.empty() && value[0] == '"') {
            value = value.substr(1);
            size_t end = value.find('"');
            if (end != std::string::npos) value = value.substr(0, end);
        }

        if (key == "name" && in_rule) {
            current_rule.name = value;
        } else if (key == "enabled" && in_rule) {
            current_rule.enabled = (value == "true" || value == "yes" || value == "1");
        } else if (key == "priority" && in_rule) {
            current_rule.priority = std::stoi(value);
        } else if (key == "stop_on_match" && in_rule) {
            current_rule.stop_on_match = (value == "true" || value == "yes" || value == "1");
        } else if (key == "trigger" && in_rule) {
            in_trigger = true;
            in_actions = false;
        } else if (key == "type" && in_trigger) {
            current_rule.trigger = parse_trigger_type(value);
        } else if (key == "pattern" && in_trigger) {
            current_rule.pattern = value;
        } else if (key == "actions" && in_rule) {
            in_trigger = false;
            in_actions = true;
        } else if (in_action) {
            if (key == "type") {
                current_action.type = parse_action_type(value);
            } else if (key == "source") {
                current_action.source = value;
            } else if (key == "sink") {
                current_action.sink = value;
            } else if (key == "gain_db") {
                current_action.gain_db = std::stof(value);
            } else if (key == "script") {
                current_action.script_path = value;
            }
        }
    }

    save_rule();
    return {};
}

} // namespace control
} // namespace soluna
