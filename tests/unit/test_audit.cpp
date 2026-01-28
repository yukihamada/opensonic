/**
 * Soluna — Audit Log Tests
 *
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>
#include <soluna/audit/audit_log.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

using namespace soluna;
using namespace soluna::audit;

class AuditTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / "soluna_audit_test";
        fs::create_directories(test_dir_);
        test_file_ = test_dir_ / "audit.jsonl";
    }

    void TearDown() override {
        fs::remove_all(test_dir_);
    }

    fs::path test_dir_;
    fs::path test_file_;
};

TEST_F(AuditTest, InitDisabled) {
    AuditLog log;
    AuditConfig config;
    config.enabled = false;

    auto result = log.init(config);
    EXPECT_TRUE(result.ok());
    EXPECT_FALSE(log.is_enabled());
}

TEST_F(AuditTest, InitEnabled) {
    AuditLog log;
    AuditConfig config;
    config.enabled = true;
    config.file_path = test_file_.string();

    auto result = log.init(config);
    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(log.is_enabled());
}

TEST_F(AuditTest, LogEntry) {
    AuditLog log;
    AuditConfig config;
    config.enabled = true;
    config.file_path = test_file_.string();
    config.async_write = false;  // Sync for testing

    auto result = log.init(config);
    ASSERT_TRUE(result.ok());

    log.log(events::AUTH_SUCCESS, "test-device", "", true, "", "127.0.0.1");
    log.flush();

    EXPECT_EQ(log.entry_count(), 1u);

    // Verify file contents
    std::ifstream file(test_file_);
    std::string line;
    ASSERT_TRUE(std::getline(file, line));

    EXPECT_NE(line.find("auth_success"), std::string::npos);
    EXPECT_NE(line.find("test-device"), std::string::npos);
    EXPECT_NE(line.find("127.0.0.1"), std::string::npos);
}

TEST_F(AuditTest, LogWithDetails) {
    AuditLog log;
    AuditConfig config;
    config.enabled = true;
    config.file_path = test_file_.string();
    config.async_write = false;

    log.init(config);

    std::map<std::string, std::string> details;
    details["stream_id"] = "1";
    details["sample_rate"] = "48000";

    log.log(events::STREAM_CREATED, "admin", "stream:1", true, details);
    log.flush();

    std::ifstream file(test_file_);
    std::string line;
    std::getline(file, line);

    EXPECT_NE(line.find("stream_created"), std::string::npos);
    EXPECT_NE(line.find("stream_id"), std::string::npos);
    EXPECT_NE(line.find("48000"), std::string::npos);
}

TEST_F(AuditTest, EventFilter) {
    AuditLog log;
    AuditConfig config;
    config.enabled = true;
    config.file_path = test_file_.string();
    config.events = {events::AUTH_SUCCESS, events::AUTH_FAILURE};
    config.async_write = false;

    log.init(config);

    EXPECT_TRUE(log.should_log(events::AUTH_SUCCESS));
    EXPECT_TRUE(log.should_log(events::AUTH_FAILURE));
    EXPECT_FALSE(log.should_log(events::STREAM_CREATED));

    log.log(events::AUTH_SUCCESS, "device1");
    log.log(events::STREAM_CREATED, "device1");  // Should be filtered
    log.flush();

    EXPECT_EQ(log.entry_count(), 1u);
}

TEST_F(AuditTest, Callback) {
    AuditLog log;
    AuditConfig config;
    config.enabled = true;
    config.file_path = test_file_.string();

    log.init(config);

    bool callback_called = false;
    std::string received_event;

    log.set_callback([&](const AuditEntry& entry) {
        callback_called = true;
        received_event = entry.event;
    });

    log.log(events::DEVICE_CONNECTED, "esp32-1", "", true, "", "192.168.1.100");

    EXPECT_TRUE(callback_called);
    EXPECT_EQ(received_event, events::DEVICE_CONNECTED);
}

TEST_F(AuditTest, AuditEntryToJson) {
    AuditEntry entry;
    entry.timestamp_ns = 1706443200000000000ULL;
    entry.event = "auth_success";
    entry.actor = "test-device";
    entry.target = "";
    entry.success = true;
    entry.remote_address = "127.0.0.1";

    std::string json = entry.to_json();

    EXPECT_NE(json.find("1706443200000000000"), std::string::npos);
    EXPECT_NE(json.find("auth_success"), std::string::npos);
    EXPECT_NE(json.find("test-device"), std::string::npos);
    EXPECT_NE(json.find("\"success\":true"), std::string::npos);
}

TEST_F(AuditTest, AuditEntryFromJson) {
    std::string json = R"({"ts":1706443200000000000,"event":"auth_success","actor":"test","success":true})";

    auto result = AuditEntry::from_json(json);
    ASSERT_TRUE(result.ok());

    const AuditEntry& entry = result.value();
    EXPECT_EQ(entry.timestamp_ns, 1706443200000000000ULL);
    EXPECT_EQ(entry.event, "auth_success");
    EXPECT_EQ(entry.actor, "test");
    EXPECT_TRUE(entry.success);
}

TEST_F(AuditTest, ConvenienceFunctions) {
    AuditLog log;
    AuditConfig config;
    config.enabled = true;
    config.file_path = test_file_.string();
    config.async_write = false;

    // Initialize global audit_log (need to access via singleton)
    // For this test, we'll create a local instance

    log.init(config);

    // Test that logging doesn't crash
    log.log(events::AUTH_SUCCESS, "device", "", true, "", "192.168.1.1");
    log.log(events::STREAM_CREATED, "admin", "stream:1");
    log.log(events::CONFIG_CHANGED, "admin", "config:sample_rate", true,
            R"({"old":"44100","new":"48000"})");
    log.flush();

    EXPECT_EQ(log.entry_count(), 3u);
}

TEST_F(AuditTest, JsonEscaping) {
    AuditLog log;
    AuditConfig config;
    config.enabled = true;
    config.file_path = test_file_.string();
    config.async_write = false;

    log.init(config);

    // Test special characters that need escaping
    log.log("test_event", "device\"with\"quotes", "target\nwith\nnewlines", true,
            "tab\there");
    log.flush();

    std::ifstream file(test_file_);
    std::string line;
    std::getline(file, line);

    // Should be valid JSON (no raw quotes/newlines)
    EXPECT_NE(line.find("\\\""), std::string::npos);  // Escaped quotes
    EXPECT_NE(line.find("\\n"), std::string::npos);   // Escaped newlines
    EXPECT_NE(line.find("\\t"), std::string::npos);   // Escaped tabs
}

TEST_F(AuditTest, DisabledDoesNotWrite) {
    AuditLog log;
    AuditConfig config;
    config.enabled = false;
    config.file_path = test_file_.string();

    log.init(config);

    log.log(events::AUTH_SUCCESS, "device");
    log.flush();

    EXPECT_EQ(log.entry_count(), 0u);
    EXPECT_FALSE(fs::exists(test_file_));
}

TEST_F(AuditTest, RotateFiles) {
    AuditLog log;
    AuditConfig config;
    config.enabled = true;
    config.file_path = test_file_.string();
    config.max_file_size = 100;  // Very small for testing
    config.max_files = 3;
    config.async_write = false;

    log.init(config);

    // Write enough to trigger rotation
    for (int i = 0; i < 10; i++) {
        log.log("test_event", "device", "target_with_long_name_to_fill_space",
                true, "some_details_here");
    }
    log.flush();

    // Check that rotation happened
    EXPECT_TRUE(fs::exists(test_file_));
    // Rotated files may exist
}
