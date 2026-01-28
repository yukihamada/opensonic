/**
 * Soluna — Configuration System Tests
 *
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>
#include <soluna/config/config.h>
#include <cstdlib>

using namespace soluna;
using namespace soluna::config;

TEST(ConfigTest, DefaultConfig) {
    Config cfg = Config::defaults();

    EXPECT_EQ(cfg.device.name, "soluna-device");
    EXPECT_EQ(cfg.device.audio_device, "default");
    EXPECT_EQ(cfg.network.control_port, 8400);
    EXPECT_EQ(cfg.network.rtp_base_port, 5004);
    EXPECT_EQ(cfg.audio.sample_rate, 48000u);
    EXPECT_EQ(cfg.audio.channels, 2u);
    EXPECT_EQ(cfg.audio.bit_depth, 24u);
    EXPECT_FALSE(cfg.security.dtls_enabled);
    EXPECT_FALSE(cfg.metrics.enabled);
}

TEST(ConfigTest, ParseSimpleYaml) {
    const char* yaml = R"(
device:
  name: "studio-main"
  audio: "hw:0"
network:
  control_port: 8500
audio:
  sample_rate: 96000
  channels: 8
)";

    auto result = Config::parse(yaml);
    ASSERT_TRUE(result.ok()) << result.error().to_string();

    const Config& cfg = result.value();
    EXPECT_EQ(cfg.device.name, "studio-main");
    EXPECT_EQ(cfg.device.audio_device, "hw:0");
    EXPECT_EQ(cfg.network.control_port, 8500);
    EXPECT_EQ(cfg.audio.sample_rate, 96000u);
    EXPECT_EQ(cfg.audio.channels, 8u);
}

TEST(ConfigTest, ParseSecurityConfig) {
    const char* yaml = R"(
security:
  auth_enabled: true
  dtls: true
  devices:
    - id: "studio-console"
      psk: "sha256:abc123"
      roles: [admin]
    - id: "speaker-1"
      psk: "sha256:def456"
      roles: [stream]
  roles:
    admin: [stream_create, route_modify, config_write]
    stream: [stream_create]
)";

    auto result = Config::parse(yaml);
    ASSERT_TRUE(result.ok()) << result.error().to_string();

    const Config& cfg = result.value();
    EXPECT_TRUE(cfg.security.auth_enabled);
    EXPECT_TRUE(cfg.security.dtls_enabled);
    EXPECT_EQ(cfg.security.devices.size(), 2u);
    EXPECT_EQ(cfg.security.devices[0].id, "studio-console");
    EXPECT_EQ(cfg.security.devices[0].psk_hash, "sha256:abc123");
    EXPECT_EQ(cfg.security.devices[0].roles.size(), 1u);
    EXPECT_EQ(cfg.security.devices[0].roles[0], "admin");
}

TEST(ConfigTest, ParseMetricsConfig) {
    const char* yaml = R"(
metrics:
  enabled: true
  port: 9200
  path: "/soluna/metrics"
  scrape_interval_ms: 10000
)";

    auto result = Config::parse(yaml);
    ASSERT_TRUE(result.ok()) << result.error().to_string();

    const Config& cfg = result.value();
    EXPECT_TRUE(cfg.metrics.enabled);
    EXPECT_EQ(cfg.metrics.port, 9200);
    EXPECT_EQ(cfg.metrics.path, "/soluna/metrics");
    EXPECT_EQ(cfg.metrics.scrape_interval_ms, 10000u);
}

TEST(ConfigTest, ParseLoggingConfig) {
    const char* yaml = R"(
logging:
  level: debug
  file: /var/log/soluna/daemon.log
  json_format: true
  include_timestamp: true
)";

    auto result = Config::parse(yaml);
    ASSERT_TRUE(result.ok()) << result.error().to_string();

    const Config& cfg = result.value();
    EXPECT_EQ(cfg.logging.level, "debug");
    EXPECT_EQ(cfg.logging.file, "/var/log/soluna/daemon.log");
    EXPECT_TRUE(cfg.logging.json_format);
    EXPECT_TRUE(cfg.logging.include_timestamp);
}

TEST(ConfigTest, ParseAuditConfig) {
    const char* yaml = R"(
audit:
  enabled: true
  file: /var/log/soluna/audit.jsonl
  events: [auth_success, auth_failure, stream_created]
)";

    auto result = Config::parse(yaml);
    ASSERT_TRUE(result.ok()) << result.error().to_string();

    const Config& cfg = result.value();
    EXPECT_TRUE(cfg.audit.enabled);
    EXPECT_EQ(cfg.audit.file, "/var/log/soluna/audit.jsonl");
    EXPECT_EQ(cfg.audit.events.size(), 3u);
    EXPECT_EQ(cfg.audit.events[0], "auth_success");
}

TEST(ConfigTest, ParseRoutingConfig) {
    const char* yaml = R"(
routing:
  auto_rules:
    - name: "connect-speakers"
      trigger:
        type: device_connected
        pattern: "esp32-speaker-*"
      actions:
        - type: add_route
          source: "main-out:0"
          sink: "$device:0"
          gain_db: -6.0
)";

    auto result = Config::parse(yaml);
    ASSERT_TRUE(result.ok()) << result.error().to_string();

    const Config& cfg = result.value();
    EXPECT_EQ(cfg.routing.auto_rules.size(), 1u);

    const auto& rule = cfg.routing.auto_rules[0];
    EXPECT_EQ(rule.name, "connect-speakers");
    EXPECT_EQ(rule.trigger.type, "device_connected");
    EXPECT_EQ(rule.trigger.pattern, "esp32-speaker-*");
    EXPECT_EQ(rule.actions.size(), 1u);
    EXPECT_EQ(rule.actions[0].type, "add_route");
    EXPECT_EQ(rule.actions[0].source, "main-out:0");
    EXPECT_EQ(rule.actions[0].sink, "$device:0");
    EXPECT_FLOAT_EQ(rule.actions[0].gain_db, -6.0f);
}

TEST(ConfigTest, ParsePlugins) {
    const char* yaml = R"(
plugins:
  - /usr/lib/soluna/plugins/limiter.so
  - /usr/lib/soluna/plugins/eq.so
)";

    auto result = Config::parse(yaml);
    ASSERT_TRUE(result.ok()) << result.error().to_string();

    const Config& cfg = result.value();
    EXPECT_EQ(cfg.plugins.size(), 2u);
    EXPECT_EQ(cfg.plugins[0].path, "/usr/lib/soluna/plugins/limiter.so");
    EXPECT_EQ(cfg.plugins[1].path, "/usr/lib/soluna/plugins/eq.so");
}

TEST(ConfigTest, ValidationInvalidSampleRate) {
    const char* yaml = R"(
audio:
  sample_rate: 12345
)";

    auto result = Config::parse(yaml);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), ErrorCode::ConfigValidationError);
}

TEST(ConfigTest, ValidationInvalidChannels) {
    const char* yaml = R"(
audio:
  channels: 128
)";

    auto result = Config::parse(yaml);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), ErrorCode::ConfigValidationError);
}

TEST(ConfigTest, ValidationInvalidLogLevel) {
    const char* yaml = R"(
logging:
  level: verbose
)";

    auto result = Config::parse(yaml);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), ErrorCode::ConfigValidationError);
}

TEST(ConfigTest, ToYaml) {
    Config cfg = Config::defaults();
    cfg.device.name = "test-device";
    cfg.audio.sample_rate = 96000;

    std::string yaml = cfg.to_yaml();

    EXPECT_NE(yaml.find("test-device"), std::string::npos);
    EXPECT_NE(yaml.find("96000"), std::string::npos);
}

TEST(ConfigTest, Merge) {
    Config base = Config::defaults();
    base.device.name = "base-device";

    Config overlay;
    overlay.device.name = "overlay-device";
    overlay.audio.sample_rate = 96000;

    base.merge(overlay);

    EXPECT_EQ(base.device.name, "overlay-device");
    EXPECT_EQ(base.audio.sample_rate, 96000u);
}

TEST(ConfigLoaderTest, ExpandEnvSimple) {
    setenv("SOLUNA_TEST_VAR", "test_value", 1);

    std::string input = "prefix_${SOLUNA_TEST_VAR}_suffix";
    std::string result = ConfigLoader::expand_env(input);

    EXPECT_EQ(result, "prefix_test_value_suffix");

    unsetenv("SOLUNA_TEST_VAR");
}

TEST(ConfigLoaderTest, ExpandEnvWithDefault) {
    unsetenv("SOLUNA_NONEXISTENT_VAR");

    std::string input = "value_${SOLUNA_NONEXISTENT_VAR:-default}_end";
    std::string result = ConfigLoader::expand_env(input);

    EXPECT_EQ(result, "value_default_end");
}

TEST(ConfigLoaderTest, ExpandEnvMultiple) {
    setenv("SOLUNA_VAR1", "one", 1);
    setenv("SOLUNA_VAR2", "two", 1);

    std::string input = "${SOLUNA_VAR1} and ${SOLUNA_VAR2}";
    std::string result = ConfigLoader::expand_env(input);

    EXPECT_EQ(result, "one and two");

    unsetenv("SOLUNA_VAR1");
    unsetenv("SOLUNA_VAR2");
}

TEST(ConfigLoaderTest, DefaultPaths) {
    auto paths = ConfigLoader::default_paths();
    EXPECT_FALSE(paths.empty());

    // Should include common paths
    bool has_etc = false;
    bool has_local = false;
    for (const auto& path : paths) {
        if (path.find("soluna.yaml") != std::string::npos ||
            path.find("config.yaml") != std::string::npos) {
            has_local = true;
        }
#ifndef _WIN32
        if (path.find("/etc/") != std::string::npos) {
            has_etc = true;
        }
#endif
    }
    EXPECT_TRUE(has_local);
#ifndef _WIN32
    EXPECT_TRUE(has_etc);
#endif
}

TEST(ConfigLoaderTest, LoadNonexistent) {
    auto result = ConfigLoader::load("/nonexistent/path/config.yaml");
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), ErrorCode::ConfigFileNotFound);
}

TEST(ConfigLoaderTest, LoadWithFallbacksNoneExist) {
    auto result = ConfigLoader::load_with_fallbacks({
        "/nonexistent1.yaml",
        "/nonexistent2.yaml"
    });

    // Should return defaults when no config found
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.value().device.name, "soluna-device");
}

// YAML Parser specific tests
TEST(YamlParserTest, EmptyDocument) {
    auto result = Config::parse("");
    EXPECT_TRUE(result.ok());
}

TEST(YamlParserTest, Comments) {
    const char* yaml = R"(
# This is a comment
device:
  # Another comment
  name: "test"  # Inline comment (may not be fully supported)
)";

    auto result = Config::parse(yaml);
    ASSERT_TRUE(result.ok()) << result.error().to_string();
    EXPECT_EQ(result.value().device.name, "test");
}

TEST(YamlParserTest, QuotedStrings) {
    const char* yaml = R"(
device:
  name: "quoted string"
  audio: 'single quoted'
)";

    auto result = Config::parse(yaml);
    ASSERT_TRUE(result.ok()) << result.error().to_string();
    EXPECT_EQ(result.value().device.name, "quoted string");
    EXPECT_EQ(result.value().device.audio_device, "single quoted");
}

TEST(YamlParserTest, BooleanValues) {
    const char* yaml = R"(
security:
  dtls: true
  auth_enabled: yes
metrics:
  enabled: false
)";

    auto result = Config::parse(yaml);
    ASSERT_TRUE(result.ok()) << result.error().to_string();
    EXPECT_TRUE(result.value().security.dtls_enabled);
    EXPECT_TRUE(result.value().security.auth_enabled);
    EXPECT_FALSE(result.value().metrics.enabled);
}

TEST(YamlParserTest, NumericValues) {
    const char* yaml = R"(
audio:
  sample_rate: 48000
  channels: 2
network:
  dscp: 46
)";

    auto result = Config::parse(yaml);
    ASSERT_TRUE(result.ok()) << result.error().to_string();
    EXPECT_EQ(result.value().audio.sample_rate, 48000u);
    EXPECT_EQ(result.value().audio.channels, 2u);
    EXPECT_EQ(result.value().network.dscp, 46);
}

TEST(YamlParserTest, InlineList) {
    const char* yaml = R"(
audit:
  events: [auth_success, auth_failure, config_changed]
)";

    auto result = Config::parse(yaml);
    ASSERT_TRUE(result.ok()) << result.error().to_string();
    EXPECT_EQ(result.value().audit.events.size(), 3u);
}

TEST(YamlParserTest, NestedMaps) {
    const char* yaml = R"(
routing:
  auto_rules:
    - name: rule1
      trigger:
        type: device_connected
        pattern: "*"
)";

    auto result = Config::parse(yaml);
    ASSERT_TRUE(result.ok()) << result.error().to_string();
    EXPECT_EQ(result.value().routing.auto_rules.size(), 1u);
    EXPECT_EQ(result.value().routing.auto_rules[0].trigger.type, "device_connected");
}
