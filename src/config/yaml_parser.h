#pragma once

/**
 * Soluna — Minimal YAML Parser (Internal Header)
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/core/error.h>
#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace soluna {
namespace config {
namespace yaml {

/**
 * YAML value type - can hold any YAML scalar, list, or map.
 */
class YamlValue {
public:
    using List = std::vector<YamlValue>;
    using Map = std::map<std::string, YamlValue>;

    YamlValue() : data_(std::monostate{}) {}
    YamlValue(bool v) : data_(v) {}
    YamlValue(int64_t v) : data_(v) {}
    YamlValue(int v) : data_(static_cast<int64_t>(v)) {}
    YamlValue(double v) : data_(v) {}
    YamlValue(std::string v) : data_(std::move(v)) {}
    YamlValue(const char* v) : data_(std::string(v)) {}
    YamlValue(List v) : data_(std::move(v)) {}
    YamlValue(Map v) : data_(std::move(v)) {}

    bool is_null() const;
    bool is_bool() const;
    bool is_int() const;
    bool is_float() const;
    bool is_string() const;
    bool is_list() const;
    bool is_map() const;

    bool as_bool(bool default_val = false) const;
    int64_t as_int(int64_t default_val = 0) const;
    double as_float(double default_val = 0.0) const;
    std::string as_string(const std::string& default_val = "") const;
    const List& as_list() const;
    const Map& as_map() const;

    // Map access
    const YamlValue& operator[](const std::string& key) const;
    bool has(const std::string& key) const;

    // List access
    const YamlValue& operator[](size_t index) const;
    size_t size() const;

    // Serialize back to YAML
    std::string to_yaml() const;

private:
    std::variant<
        std::monostate,  // null
        bool,
        int64_t,
        double,
        std::string,
        List,
        Map
    > data_;
};

/**
 * Parse YAML content into a YamlValue tree.
 */
Result<YamlValue> parse(const std::string& content);

} // namespace yaml
} // namespace config
} // namespace soluna
