/**
 * Soluna — Minimal YAML Parser
 *
 * A lightweight YAML parser that supports the subset needed for Soluna config:
 * - Scalars (strings, numbers, booleans)
 * - Lists (sequences)
 * - Maps (dictionaries)
 * - Comments (#)
 * - Multi-line strings (|, >)
 *
 * Does NOT support:
 * - Anchors/aliases
 * - Tags
 * - Complex keys
 * - Multiple documents
 *
 * SPDX-License-Identifier: MIT
 */

#include "yaml_parser.h"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace soluna {
namespace config {
namespace yaml {

// Trim whitespace from both ends
static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Count leading spaces
static size_t leading_spaces(const std::string& line) {
    size_t count = 0;
    for (char c : line) {
        if (c == ' ') count++;
        else if (c == '\t') count += 2;  // Treat tabs as 2 spaces
        else break;
    }
    return count;
}

// Check if string is a number
static bool is_number(const std::string& s) {
    if (s.empty()) return false;
    size_t i = 0;
    if (s[i] == '-' || s[i] == '+') i++;
    bool has_dot = false;
    bool has_digit = false;
    for (; i < s.size(); i++) {
        if (std::isdigit(s[i])) {
            has_digit = true;
        } else if (s[i] == '.' && !has_dot) {
            has_dot = true;
        } else {
            return false;
        }
    }
    return has_digit;
}

// Parse a scalar value
YamlValue parse_scalar(const std::string& s) {
    std::string val = trim(s);

    // Empty
    if (val.empty() || val == "~" || val == "null" || val == "Null" || val == "NULL") {
        return YamlValue();  // null
    }

    // Quoted string - handle first to preserve content
    if (val.size() >= 2 &&
        ((val.front() == '"' && val.back() == '"') ||
         (val.front() == '\'' && val.back() == '\''))) {
        return YamlValue(val.substr(1, val.size() - 2));
    }

    // Remove inline comments (# not inside quotes) for unquoted values
    size_t comment_pos = val.find('#');
    if (comment_pos != std::string::npos) {
        val = trim(val.substr(0, comment_pos));
        if (val.empty()) return YamlValue();
        // Re-check for quotes after trimming comment
        if (val.size() >= 2 &&
            ((val.front() == '"' && val.back() == '"') ||
             (val.front() == '\'' && val.back() == '\''))) {
            return YamlValue(val.substr(1, val.size() - 2));
        }
    }

    // Boolean
    if (val == "true" || val == "True" || val == "TRUE" ||
        val == "yes" || val == "Yes" || val == "YES" ||
        val == "on" || val == "On" || val == "ON") {
        return YamlValue(true);
    }
    if (val == "false" || val == "False" || val == "FALSE" ||
        val == "no" || val == "No" || val == "NO" ||
        val == "off" || val == "Off" || val == "OFF") {
        return YamlValue(false);
    }

    // Number
    if (is_number(val)) {
        if (val.find('.') != std::string::npos) {
            return YamlValue(std::stod(val));
        }
        return YamlValue(static_cast<int64_t>(std::stoll(val)));
    }

    // Plain string
    return YamlValue(val);
}

// Parser state
struct ParserState {
    std::vector<std::string> lines;
    size_t current = 0;
    std::string error;

    bool has_more() const { return current < lines.size(); }

    const std::string& peek() const {
        static const std::string empty;
        return current < lines.size() ? lines[current] : empty;
    }

    std::string consume() {
        return current < lines.size() ? lines[current++] : "";
    }

    void skip_empty_and_comments() {
        while (has_more()) {
            std::string trimmed = trim(peek());
            if (trimmed.empty() || trimmed[0] == '#') {
                current++;
            } else {
                break;
            }
        }
    }
};

// Forward declarations
static YamlValue parse_value(ParserState& state, size_t indent);
static YamlValue parse_map(ParserState& state, size_t indent);
static YamlValue parse_list(ParserState& state, size_t indent);

// Parse a map
static YamlValue parse_map(ParserState& state, size_t indent) {
    YamlValue::Map map;

    while (state.has_more()) {
        state.skip_empty_and_comments();
        if (!state.has_more()) break;

        const std::string& line = state.peek();
        size_t line_indent = leading_spaces(line);

        if (line_indent < indent) {
            break;  // Dedented, end of this map
        }

        if (line_indent > indent) {
            state.error = "Unexpected indentation";
            break;
        }

        std::string trimmed = trim(line);

        // Check for list item at this level
        if (trimmed[0] == '-') {
            break;  // Not a map entry
        }

        // Find key: value
        size_t colon = trimmed.find(':');
        if (colon == std::string::npos) {
            state.error = "Expected ':' in map entry";
            break;
        }

        std::string key = trim(trimmed.substr(0, colon));
        std::string rest = trim(trimmed.substr(colon + 1));

        state.consume();

        if (rest.empty()) {
            // Value is on next lines (nested structure)
            state.skip_empty_and_comments();
            if (state.has_more()) {
                size_t next_indent = leading_spaces(state.peek());
                if (next_indent > indent) {
                    std::string next_trimmed = trim(state.peek());
                    if (next_trimmed[0] == '-') {
                        map[key] = parse_list(state, next_indent);
                    } else {
                        map[key] = parse_map(state, next_indent);
                    }
                } else {
                    map[key] = YamlValue();  // null
                }
            }
        } else if (rest[0] == '|' || rest[0] == '>') {
            // Multi-line string
            bool literal = (rest[0] == '|');
            std::string block;
            state.skip_empty_and_comments();

            size_t block_indent = 0;
            bool first = true;

            while (state.has_more()) {
                const std::string& block_line = state.peek();
                size_t bl_indent = leading_spaces(block_line);

                if (trim(block_line).empty()) {
                    if (!first) block += "\n";
                    state.consume();
                    continue;
                }

                if (first) {
                    block_indent = bl_indent;
                    first = false;
                }

                if (bl_indent < block_indent) {
                    break;
                }

                if (!block.empty()) {
                    block += literal ? "\n" : " ";
                }
                block += block_line.substr(block_indent);
                state.consume();
            }

            map[key] = YamlValue(block);
        } else if (rest[0] == '[') {
            // Inline list
            YamlValue::List list;
            std::string items = rest.substr(1);
            size_t end = items.rfind(']');
            if (end != std::string::npos) {
                items = items.substr(0, end);
            }
            std::istringstream ss(items);
            std::string item;
            while (std::getline(ss, item, ',')) {
                list.push_back(parse_scalar(item));
            }
            map[key] = YamlValue(std::move(list));
        } else if (rest[0] == '{') {
            // Inline map
            YamlValue::Map inline_map;
            std::string items = rest.substr(1);
            size_t end = items.rfind('}');
            if (end != std::string::npos) {
                items = items.substr(0, end);
            }
            std::istringstream ss(items);
            std::string item;
            while (std::getline(ss, item, ',')) {
                size_t c = item.find(':');
                if (c != std::string::npos) {
                    std::string k = trim(item.substr(0, c));
                    std::string v = trim(item.substr(c + 1));
                    inline_map[k] = parse_scalar(v);
                }
            }
            map[key] = YamlValue(std::move(inline_map));
        } else {
            // Simple value
            map[key] = parse_scalar(rest);
        }
    }

    return YamlValue(std::move(map));
}

// Parse a list
static YamlValue parse_list(ParserState& state, size_t indent) {
    YamlValue::List list;

    while (state.has_more()) {
        state.skip_empty_and_comments();
        if (!state.has_more()) break;

        const std::string& line = state.peek();
        size_t line_indent = leading_spaces(line);

        if (line_indent < indent) {
            break;  // Dedented
        }

        std::string trimmed = trim(line);
        if (trimmed[0] != '-') {
            break;  // Not a list item
        }

        state.consume();

        std::string rest = trim(trimmed.substr(1));

        if (rest.empty()) {
            // Nested structure
            state.skip_empty_and_comments();
            if (state.has_more()) {
                size_t next_indent = leading_spaces(state.peek());
                if (next_indent > indent) {
                    std::string next_trimmed = trim(state.peek());
                    if (next_trimmed[0] == '-') {
                        list.push_back(parse_list(state, next_indent));
                    } else {
                        list.push_back(parse_map(state, next_indent));
                    }
                } else {
                    list.push_back(YamlValue());
                }
            }
        } else if (rest.find(':') != std::string::npos && rest[0] != '"' && rest[0] != '\'') {
            // Inline map as list item: "- key: value"
            YamlValue::Map item_map;
            size_t colon = rest.find(':');
            std::string key = trim(rest.substr(0, colon));
            std::string val = trim(rest.substr(colon + 1));
            item_map[key] = parse_scalar(val);

            // Check for more keys at higher indent
            state.skip_empty_and_comments();
            if (state.has_more()) {
                size_t next_indent = leading_spaces(state.peek());
                if (next_indent > indent) {
                    auto nested = parse_map(state, next_indent);
                    if (nested.is_map()) {
                        for (const auto& [k, v] : nested.as_map()) {
                            item_map[k] = v;
                        }
                    }
                }
            }
            list.push_back(YamlValue(std::move(item_map)));
        } else {
            // Simple value
            list.push_back(parse_scalar(rest));
        }
    }

    return YamlValue(std::move(list));
}

// Parse YAML content
Result<YamlValue> parse(const std::string& content) {
    ParserState state;

    // Split into lines
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        // Remove BOM if present
        if (!state.lines.empty() || line.substr(0, 3) != "\xEF\xBB\xBF") {
            state.lines.push_back(line);
        } else {
            state.lines.push_back(line.substr(3));
        }
    }

    state.skip_empty_and_comments();

    if (!state.has_more()) {
        return YamlValue();  // Empty document
    }

    // Detect root type
    std::string first = trim(state.peek());
    if (first[0] == '-') {
        return parse_list(state, leading_spaces(state.peek()));
    }

    return parse_map(state, 0);
}

// YamlValue implementation

bool YamlValue::is_null() const {
    return std::holds_alternative<std::monostate>(data_);
}

bool YamlValue::is_bool() const {
    return std::holds_alternative<bool>(data_);
}

bool YamlValue::is_int() const {
    return std::holds_alternative<int64_t>(data_);
}

bool YamlValue::is_float() const {
    return std::holds_alternative<double>(data_);
}

bool YamlValue::is_string() const {
    return std::holds_alternative<std::string>(data_);
}

bool YamlValue::is_list() const {
    return std::holds_alternative<List>(data_);
}

bool YamlValue::is_map() const {
    return std::holds_alternative<Map>(data_);
}

bool YamlValue::as_bool(bool default_val) const {
    if (is_bool()) return std::get<bool>(data_);
    if (is_int()) return std::get<int64_t>(data_) != 0;
    if (is_string()) {
        const auto& s = std::get<std::string>(data_);
        if (s == "true" || s == "yes" || s == "on") return true;
        if (s == "false" || s == "no" || s == "off") return false;
    }
    return default_val;
}

int64_t YamlValue::as_int(int64_t default_val) const {
    if (is_int()) return std::get<int64_t>(data_);
    if (is_float()) return static_cast<int64_t>(std::get<double>(data_));
    if (is_string()) {
        try {
            return std::stoll(std::get<std::string>(data_));
        } catch (...) {}
    }
    return default_val;
}

double YamlValue::as_float(double default_val) const {
    if (is_float()) return std::get<double>(data_);
    if (is_int()) return static_cast<double>(std::get<int64_t>(data_));
    if (is_string()) {
        try {
            return std::stod(std::get<std::string>(data_));
        } catch (...) {}
    }
    return default_val;
}

std::string YamlValue::as_string(const std::string& default_val) const {
    if (is_string()) return std::get<std::string>(data_);
    if (is_int()) return std::to_string(std::get<int64_t>(data_));
    if (is_float()) return std::to_string(std::get<double>(data_));
    if (is_bool()) return std::get<bool>(data_) ? "true" : "false";
    return default_val;
}

const YamlValue::List& YamlValue::as_list() const {
    static const List empty;
    return is_list() ? std::get<List>(data_) : empty;
}

const YamlValue::Map& YamlValue::as_map() const {
    static const Map empty;
    return is_map() ? std::get<Map>(data_) : empty;
}

const YamlValue& YamlValue::operator[](const std::string& key) const {
    static const YamlValue null_value;
    if (!is_map()) return null_value;
    const auto& map = std::get<Map>(data_);
    auto it = map.find(key);
    return it != map.end() ? it->second : null_value;
}

const YamlValue& YamlValue::operator[](size_t index) const {
    static const YamlValue null_value;
    if (!is_list()) return null_value;
    const auto& list = std::get<List>(data_);
    return index < list.size() ? list[index] : null_value;
}

bool YamlValue::has(const std::string& key) const {
    if (!is_map()) return false;
    return std::get<Map>(data_).count(key) > 0;
}

size_t YamlValue::size() const {
    if (is_list()) return std::get<List>(data_).size();
    if (is_map()) return std::get<Map>(data_).size();
    return 0;
}

// Serialize to YAML
static void serialize_value(std::ostream& out, const YamlValue& val, int indent);

static void serialize_map(std::ostream& out, const YamlValue::Map& map, int indent) {
    std::string prefix(indent, ' ');
    for (const auto& [key, val] : map) {
        out << prefix << key << ":";
        if (val.is_map() || val.is_list()) {
            out << "\n";
            serialize_value(out, val, indent + 2);
        } else {
            out << " ";
            serialize_value(out, val, 0);
            out << "\n";
        }
    }
}

static void serialize_list(std::ostream& out, const YamlValue::List& list, int indent) {
    std::string prefix(indent, ' ');
    for (const auto& val : list) {
        out << prefix << "-";
        if (val.is_map()) {
            out << "\n";
            serialize_value(out, val, indent + 2);
        } else if (val.is_list()) {
            out << "\n";
            serialize_value(out, val, indent + 2);
        } else {
            out << " ";
            serialize_value(out, val, 0);
            out << "\n";
        }
    }
}

static void serialize_value(std::ostream& out, const YamlValue& val, int indent) {
    if (val.is_null()) {
        out << "null";
    } else if (val.is_bool()) {
        out << (val.as_bool() ? "true" : "false");
    } else if (val.is_int()) {
        out << val.as_int();
    } else if (val.is_float()) {
        out << val.as_float();
    } else if (val.is_string()) {
        const auto& s = val.as_string();
        // Quote if needed
        bool needs_quote = s.empty() || s.find(':') != std::string::npos ||
                           s.find('#') != std::string::npos ||
                           s.find('\n') != std::string::npos ||
                           s[0] == '"' || s[0] == '\'' ||
                           s[0] == '[' || s[0] == '{' ||
                           s[0] == '-' || s[0] == '>' || s[0] == '|';
        if (needs_quote) {
            out << '"';
            for (char c : s) {
                if (c == '"') out << "\\\"";
                else if (c == '\\') out << "\\\\";
                else if (c == '\n') out << "\\n";
                else out << c;
            }
            out << '"';
        } else {
            out << s;
        }
    } else if (val.is_map()) {
        serialize_map(out, val.as_map(), indent);
    } else if (val.is_list()) {
        serialize_list(out, val.as_list(), indent);
    }
}

std::string YamlValue::to_yaml() const {
    std::ostringstream out;
    if (is_map() || is_list()) {
        serialize_value(out, *this, 0);
    } else {
        serialize_value(out, *this, 0);
        out << "\n";
    }
    return out.str();
}

} // namespace yaml
} // namespace config
} // namespace soluna
