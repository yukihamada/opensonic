/**
 * Soluna — Configuration Loader
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/config/config.h>
#include <soluna/soluna.h>
#include "yaml_parser.h"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <regex>

#ifdef _WIN32
#include <userenv.h>
#pragma comment(lib, "userenv.lib")
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace soluna {
namespace config {

using yaml::YamlValue;

// Helper to get home directory
static std::string get_home_dir() {
#ifdef _WIN32
    char* home = std::getenv("USERPROFILE");
    if (home) return home;
    return "C:\\";
#else
    const char* home = std::getenv("HOME");
    if (home) return home;
    struct passwd* pw = getpwuid(getuid());
    if (pw) return pw->pw_dir;
    return "/";
#endif
}

// Environment variable expansion
std::string ConfigLoader::expand_env(const std::string& input) {
    std::string result = input;
    std::regex env_var(R"(\$\{([^}:]+)(?::-([^}]*))?\})");
    std::smatch match;

    while (std::regex_search(result, match, env_var)) {
        std::string var_name = match[1].str();
        std::string default_val = match[2].str();

        const char* env_val = std::getenv(var_name.c_str());
        std::string replacement = env_val ? env_val : default_val;

        result = match.prefix().str() + replacement + match.suffix().str();
    }

    // Also handle simple $VAR syntax
    std::regex simple_var(R"(\$([A-Za-z_][A-Za-z0-9_]*))");
    while (std::regex_search(result, match, simple_var)) {
        std::string var_name = match[1].str();
        const char* env_val = std::getenv(var_name.c_str());
        std::string replacement = env_val ? env_val : "";
        result = match.prefix().str() + replacement + match.suffix().str();
    }

    return result;
}

// Default search paths
std::vector<std::string> ConfigLoader::default_paths() {
    std::string home = get_home_dir();
    return {
        "./soluna.yaml",
        "./config/soluna.yaml",
        home + "/.config/soluna/config.yaml",
        home + "/.soluna.yaml",
#ifdef _WIN32
        "C:\\ProgramData\\Soluna\\config.yaml",
#else
        "/etc/soluna/config.yaml",
        "/usr/local/etc/soluna/config.yaml",
#endif
    };
}

// Read file contents
static Result<std::string> read_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return Error(ErrorCode::ConfigFileNotFound,
                     "Cannot open config file",
                     path);
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// Write file contents
static Result<void> write_file(const std::string& path, const std::string& content) {
    std::ofstream file(path);
    if (!file) {
        return Error(ErrorCode::ConfigWriteError,
                     "Cannot write config file",
                     path);
    }
    file << content;
    if (!file) {
        return Error(ErrorCode::ConfigWriteError,
                     "Write failed",
                     path);
    }
    return Result<void>::success();
}

// Convert YamlValue to Config
static void load_device_config(DeviceConfig& cfg, const YamlValue& yaml) {
    if (!yaml.is_map()) return;
    if (yaml.has("name")) cfg.name = yaml["name"].as_string(cfg.name);
    if (yaml.has("audio")) cfg.audio_device = yaml["audio"].as_string(cfg.audio_device);
    if (yaml.has("audio_device")) cfg.audio_device = yaml["audio_device"].as_string(cfg.audio_device);
    if (yaml.has("interface")) cfg.interface = yaml["interface"].as_string();
}

static void load_network_config(NetworkConfig& cfg, const YamlValue& yaml) {
    if (!yaml.is_map()) return;
    if (yaml.has("control_port")) cfg.control_port = static_cast<uint16_t>(yaml["control_port"].as_int(cfg.control_port));
    if (yaml.has("rtp_base")) cfg.rtp_base_port = static_cast<uint16_t>(yaml["rtp_base"].as_int(cfg.rtp_base_port));
    if (yaml.has("rtp_base_port")) cfg.rtp_base_port = static_cast<uint16_t>(yaml["rtp_base_port"].as_int(cfg.rtp_base_port));
    if (yaml.has("multicast_audio")) cfg.multicast_audio = yaml["multicast_audio"].as_string(cfg.multicast_audio);
    if (yaml.has("multicast_ptp")) cfg.multicast_ptp = yaml["multicast_ptp"].as_string(cfg.multicast_ptp);
    if (yaml.has("dscp")) cfg.dscp = static_cast<int>(yaml["dscp"].as_int(cfg.dscp));
}

static void load_audio_config(AudioConfig& cfg, const YamlValue& yaml) {
    if (!yaml.is_map()) return;
    if (yaml.has("sample_rate")) cfg.sample_rate = static_cast<uint32_t>(yaml["sample_rate"].as_int(cfg.sample_rate));
    if (yaml.has("channels")) cfg.channels = static_cast<uint32_t>(yaml["channels"].as_int(cfg.channels));
    if (yaml.has("bit_depth")) cfg.bit_depth = static_cast<uint32_t>(yaml["bit_depth"].as_int(cfg.bit_depth));
    if (yaml.has("frames_per_packet")) cfg.frames_per_packet = static_cast<uint32_t>(yaml["frames_per_packet"].as_int(cfg.frames_per_packet));
    if (yaml.has("buffer_packets")) cfg.buffer_packets = static_cast<uint32_t>(yaml["buffer_packets"].as_int(cfg.buffer_packets));
}

static void load_security_config(SecurityConfig& cfg, const YamlValue& yaml) {
    if (!yaml.is_map()) return;
    if (yaml.has("dtls")) cfg.dtls_enabled = yaml["dtls"].as_bool(cfg.dtls_enabled);
    if (yaml.has("dtls_enabled")) cfg.dtls_enabled = yaml["dtls_enabled"].as_bool(cfg.dtls_enabled);
    if (yaml.has("auth_enabled")) cfg.auth_enabled = yaml["auth_enabled"].as_bool(cfg.auth_enabled);
    if (yaml.has("certificate_path")) cfg.certificate_path = yaml["certificate_path"].as_string();
    if (yaml.has("private_key_path")) cfg.private_key_path = yaml["private_key_path"].as_string();

    if (yaml.has("devices")) {
        const auto& devices = yaml["devices"].as_list();
        for (const auto& dev : devices) {
            SecurityConfig::DeviceCredential cred;
            cred.id = dev["id"].as_string();
            cred.psk_hash = dev["psk"].as_string();
            if (dev.has("roles")) {
                for (const auto& role : dev["roles"].as_list()) {
                    cred.roles.push_back(role.as_string());
                }
            }
            cfg.devices.push_back(std::move(cred));
        }
    }

    if (yaml.has("roles")) {
        const auto& roles = yaml["roles"];
        if (roles.is_map()) {
            for (const auto& [role_name, perms] : roles.as_map()) {
                SecurityConfig::RolePermissions rp;
                rp.role = role_name;
                for (const auto& perm : perms.as_list()) {
                    rp.permissions.push_back(perm.as_string());
                }
                cfg.roles.push_back(std::move(rp));
            }
        }
    }
}

static void load_metrics_config(MetricsConfig& cfg, const YamlValue& yaml) {
    if (!yaml.is_map()) return;
    if (yaml.has("enabled")) cfg.enabled = yaml["enabled"].as_bool(cfg.enabled);
    if (yaml.has("port")) cfg.port = static_cast<uint16_t>(yaml["port"].as_int(cfg.port));
    if (yaml.has("path")) cfg.path = yaml["path"].as_string(cfg.path);
    if (yaml.has("scrape_interval_ms")) cfg.scrape_interval_ms = static_cast<uint32_t>(yaml["scrape_interval_ms"].as_int(cfg.scrape_interval_ms));
}

static void load_logging_config(LoggingConfig& cfg, const YamlValue& yaml) {
    if (!yaml.is_map()) return;
    if (yaml.has("level")) cfg.level = yaml["level"].as_string(cfg.level);
    if (yaml.has("file")) cfg.file = yaml["file"].as_string();
    if (yaml.has("json_format")) cfg.json_format = yaml["json_format"].as_bool(cfg.json_format);
    if (yaml.has("include_timestamp")) cfg.include_timestamp = yaml["include_timestamp"].as_bool(cfg.include_timestamp);
}

static void load_audit_config(AuditConfig& cfg, const YamlValue& yaml) {
    if (!yaml.is_map()) return;
    if (yaml.has("enabled")) cfg.enabled = yaml["enabled"].as_bool(cfg.enabled);
    if (yaml.has("file")) cfg.file = yaml["file"].as_string(cfg.file);
    if (yaml.has("events")) {
        cfg.events.clear();
        for (const auto& ev : yaml["events"].as_list()) {
            cfg.events.push_back(ev.as_string());
        }
    }
}

static void load_routing_config(RoutingConfig& cfg, const YamlValue& yaml) {
    if (!yaml.is_map()) return;
    if (yaml.has("auto_rules")) {
        for (const auto& rule_yaml : yaml["auto_rules"].as_list()) {
            AutoRouteRule rule;
            rule.name = rule_yaml["name"].as_string();

            if (rule_yaml.has("trigger")) {
                const auto& trigger = rule_yaml["trigger"];
                rule.trigger.type = trigger["type"].as_string();
                rule.trigger.pattern = trigger["pattern"].as_string();
            }

            if (rule_yaml.has("actions")) {
                for (const auto& action_yaml : rule_yaml["actions"].as_list()) {
                    AutoRouteRule::Action action;
                    action.type = action_yaml["type"].as_string();
                    action.source = action_yaml["source"].as_string();
                    action.sink = action_yaml["sink"].as_string();
                    action.gain_db = static_cast<float>(action_yaml["gain_db"].as_float(0.0));
                    rule.actions.push_back(std::move(action));
                }
            }

            cfg.auto_rules.push_back(std::move(rule));
        }
    }
}

static void load_plugins(std::vector<PluginConfig>& plugins, const YamlValue& yaml) {
    if (!yaml.is_list()) return;
    for (const auto& plugin_yaml : yaml.as_list()) {
        PluginConfig plugin;
        if (plugin_yaml.is_string()) {
            plugin.path = plugin_yaml.as_string();
        } else if (plugin_yaml.is_map()) {
            plugin.path = plugin_yaml["path"].as_string();
            if (plugin_yaml.has("params")) {
                for (const auto& [k, v] : plugin_yaml["params"].as_map()) {
                    plugin.params[k] = v.as_string();
                }
            }
        }
        plugins.push_back(std::move(plugin));
    }
}

// Config implementation

Config Config::defaults() {
    return Config{};
}

Result<Config> Config::parse(const std::string& yaml_content) {
    auto yaml_result = yaml::parse(yaml_content);
    if (!yaml_result.ok()) {
        return yaml_result.error();
    }

    const auto& yaml = yaml_result.value();
    // Empty or null document returns defaults
    if (yaml.is_null()) {
        return Config::defaults();
    }
    if (!yaml.is_map()) {
        return Error(ErrorCode::ConfigParseError, "Config root must be a map");
    }

    Config cfg = Config::defaults();

    if (yaml.has("device")) load_device_config(cfg.device, yaml["device"]);
    if (yaml.has("network")) load_network_config(cfg.network, yaml["network"]);
    if (yaml.has("audio")) load_audio_config(cfg.audio, yaml["audio"]);
    if (yaml.has("security")) load_security_config(cfg.security, yaml["security"]);
    if (yaml.has("metrics")) load_metrics_config(cfg.metrics, yaml["metrics"]);
    if (yaml.has("logging")) load_logging_config(cfg.logging, yaml["logging"]);
    if (yaml.has("audit")) load_audit_config(cfg.audit, yaml["audit"]);
    if (yaml.has("routing")) load_routing_config(cfg.routing, yaml["routing"]);
    if (yaml.has("plugins")) load_plugins(cfg.plugins, yaml["plugins"]);

    auto validate_result = cfg.validate();
    if (!validate_result.ok()) {
        return validate_result.error();
    }

    return cfg;
}

Result<Config> Config::load(const std::string& path) {
    auto content_result = read_file(path);
    if (!content_result.ok()) {
        return content_result.error();
    }

    // Expand environment variables
    std::string expanded = ConfigLoader::expand_env(content_result.value());

    return Config::parse(expanded);
}

Result<void> Config::save(const std::string& path) const {
    return write_file(path, to_yaml());
}

std::string Config::to_yaml() const {
    std::ostringstream out;

    out << "# Soluna Configuration\n";
    out << "# Generated by Soluna v" << SOLUNA_VERSION_MAJOR << "."
        << SOLUNA_VERSION_MINOR << "." << SOLUNA_VERSION_PATCH << "\n\n";

    // Device
    out << "device:\n";
    out << "  name: \"" << device.name << "\"\n";
    out << "  audio: \"" << device.audio_device << "\"\n";
    if (!device.interface.empty()) {
        out << "  interface: \"" << device.interface << "\"\n";
    }
    out << "\n";

    // Network
    out << "network:\n";
    out << "  control_port: " << network.control_port << "\n";
    out << "  rtp_base: " << network.rtp_base_port << "\n";
    out << "  multicast_audio: \"" << network.multicast_audio << "\"\n";
    out << "  multicast_ptp: \"" << network.multicast_ptp << "\"\n";
    out << "  dscp: " << network.dscp << "\n";
    out << "\n";

    // Audio
    out << "audio:\n";
    out << "  sample_rate: " << audio.sample_rate << "\n";
    out << "  channels: " << audio.channels << "\n";
    out << "  bit_depth: " << audio.bit_depth << "\n";
    out << "  frames_per_packet: " << audio.frames_per_packet << "\n";
    out << "  buffer_packets: " << audio.buffer_packets << "\n";
    out << "\n";

    // Security
    out << "security:\n";
    out << "  dtls: " << (security.dtls_enabled ? "true" : "false") << "\n";
    out << "  auth_enabled: " << (security.auth_enabled ? "true" : "false") << "\n";
    if (!security.certificate_path.empty()) {
        out << "  certificate_path: \"" << security.certificate_path << "\"\n";
    }
    if (!security.private_key_path.empty()) {
        out << "  private_key_path: \"" << security.private_key_path << "\"\n";
    }
    out << "\n";

    // Metrics
    out << "metrics:\n";
    out << "  enabled: " << (metrics.enabled ? "true" : "false") << "\n";
    out << "  port: " << metrics.port << "\n";
    out << "  path: \"" << metrics.path << "\"\n";
    out << "\n";

    // Logging
    out << "logging:\n";
    out << "  level: \"" << logging.level << "\"\n";
    if (!logging.file.empty()) {
        out << "  file: \"" << logging.file << "\"\n";
    }
    out << "  json_format: " << (logging.json_format ? "true" : "false") << "\n";
    out << "\n";

    // Audit
    out << "audit:\n";
    out << "  enabled: " << (audit.enabled ? "true" : "false") << "\n";
    out << "  file: \"" << audit.file << "\"\n";
    out << "\n";

    // Plugins
    if (!plugins.empty()) {
        out << "plugins:\n";
        for (const auto& p : plugins) {
            out << "  - " << p.path << "\n";
        }
        out << "\n";
    }

    return out.str();
}

Result<void> Config::validate() const {
    // Validate sample rate
    if (audio.sample_rate != 44100 && audio.sample_rate != 48000 &&
        audio.sample_rate != 88200 && audio.sample_rate != 96000 &&
        audio.sample_rate != 176400 && audio.sample_rate != 192000) {
        return Error(ErrorCode::ConfigValidationError,
                     "Invalid sample rate",
                     std::to_string(audio.sample_rate));
    }

    // Validate channels
    if (audio.channels == 0 || audio.channels > 64) {
        return Error(ErrorCode::ConfigValidationError,
                     "Channels must be 1-64",
                     std::to_string(audio.channels));
    }

    // Validate bit depth
    if (audio.bit_depth != 16 && audio.bit_depth != 24 && audio.bit_depth != 32) {
        return Error(ErrorCode::ConfigValidationError,
                     "Bit depth must be 16, 24, or 32",
                     std::to_string(audio.bit_depth));
    }

    // Validate ports
    if (network.control_port == 0) {
        return Error(ErrorCode::ConfigValidationError,
                     "Control port cannot be 0");
    }

    // Validate DSCP
    if (network.dscp < 0 || network.dscp > 63) {
        return Error(ErrorCode::ConfigValidationError,
                     "DSCP must be 0-63",
                     std::to_string(network.dscp));
    }

    // Validate log level
    if (logging.level != "debug" && logging.level != "info" &&
        logging.level != "warn" && logging.level != "error") {
        return Error(ErrorCode::ConfigValidationError,
                     "Log level must be debug, info, warn, or error",
                     logging.level);
    }

    return Result<void>::success();
}

void Config::merge(const Config& other) {
    // Only merge non-default values (simple heuristic: non-empty strings, non-zero numbers)
    if (!other.device.name.empty() && other.device.name != "soluna-device") {
        device.name = other.device.name;
    }
    if (!other.device.audio_device.empty() && other.device.audio_device != "default") {
        device.audio_device = other.device.audio_device;
    }
    if (!other.device.interface.empty()) {
        device.interface = other.device.interface;
    }

    // Network
    if (other.network.control_port != 8400) {
        network.control_port = other.network.control_port;
    }
    if (other.network.rtp_base_port != 5004) {
        network.rtp_base_port = other.network.rtp_base_port;
    }

    // Audio
    if (other.audio.sample_rate != 48000) {
        audio.sample_rate = other.audio.sample_rate;
    }
    if (other.audio.channels != 2) {
        audio.channels = other.audio.channels;
    }

    // Security (always merge if enabled)
    if (other.security.dtls_enabled) {
        security.dtls_enabled = true;
    }
    if (other.security.auth_enabled) {
        security.auth_enabled = true;
    }

    // Metrics
    if (other.metrics.enabled) {
        metrics = other.metrics;
    }

    // Plugins (append)
    for (const auto& p : other.plugins) {
        plugins.push_back(p);
    }
}

// ConfigLoader implementation

Result<Config> ConfigLoader::load(const std::string& path) {
    return Config::load(path);
}

Result<Config> ConfigLoader::load_with_fallbacks(const std::vector<std::string>& paths) {
    for (const auto& path : paths) {
        auto result = Config::load(path);
        if (result.ok()) {
            return result;
        }
        // Only continue if file not found
        if (result.error().code() != ErrorCode::ConfigFileNotFound) {
            return result;
        }
    }

    // No config found, return defaults
    return Config::defaults();
}

} // namespace config
} // namespace soluna
