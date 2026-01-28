/**
 * Unit tests for Control Protocol (JSON serialization)
 * SPDX-License-Identifier: MIT
 */

#include <soluna/control/protocol.h>
#include <gtest/gtest.h>

using namespace soluna::control;

TEST(ProtocolTest, CommandToString) {
    EXPECT_STREQ(command_to_string(CommandType::DeviceList), "device.list");
    EXPECT_STREQ(command_to_string(CommandType::RouteAdd), "route.add");
    EXPECT_STREQ(command_to_string(CommandType::Status), "status");
    EXPECT_STREQ(command_to_string(CommandType::Version), "version");
    EXPECT_STREQ(command_to_string(CommandType::Unknown), "unknown");
}

TEST(ProtocolTest, StringToCommand) {
    EXPECT_EQ(string_to_command("device.list"), CommandType::DeviceList);
    EXPECT_EQ(string_to_command("route.add"), CommandType::RouteAdd);
    EXPECT_EQ(string_to_command("status"), CommandType::Status);
    EXPECT_EQ(string_to_command("nonsense"), CommandType::Unknown);
}

TEST(ProtocolTest, SerializeRequest) {
    ControlRequest req;
    req.id = 42;
    req.command = CommandType::RouteAdd;
    req.params["source"] = "devA:1";
    req.params["sink"] = "devB:1";

    std::string json = serialize_request(req);
    EXPECT_NE(json.find("\"id\":42"), std::string::npos);
    EXPECT_NE(json.find("\"command\":\"route.add\""), std::string::npos);
    EXPECT_NE(json.find("\"params\":{"), std::string::npos);
    EXPECT_NE(json.find("devA:1"), std::string::npos);
}

TEST(ProtocolTest, SerializeRequestNoParams) {
    ControlRequest req;
    req.id = 1;
    req.command = CommandType::Status;
    std::string json = serialize_request(req);
    EXPECT_EQ(json.find("\"params\""), std::string::npos);
}

TEST(ProtocolTest, ParseRequest) {
    std::string json = R"({"id":42,"command":"route.add","params":{"source":"devA:1","sink":"devB:1"}})";
    ControlRequest req;
    EXPECT_TRUE(parse_request(json, req));
    EXPECT_EQ(req.id, 42u);
    EXPECT_EQ(req.command, CommandType::RouteAdd);
    EXPECT_EQ(req.get_param("source"), "devA:1");
    EXPECT_EQ(req.get_param("sink"), "devB:1");
}

TEST(ProtocolTest, ParseRequestNoParams) {
    std::string json = R"({"id":1,"command":"status"})";
    ControlRequest req;
    EXPECT_TRUE(parse_request(json, req));
    EXPECT_EQ(req.command, CommandType::Status);
    EXPECT_TRUE(req.params.empty());
}

TEST(ProtocolTest, ParseRequestInvalidCommand) {
    std::string json = R"({"id":1,"command":"invalid"})";
    ControlRequest req;
    EXPECT_TRUE(parse_request(json, req));
    EXPECT_EQ(req.command, CommandType::Unknown);
}

TEST(ProtocolTest, ParseRequestNoCommand) {
    std::string json = R"({"id":1})";
    ControlRequest req;
    EXPECT_FALSE(parse_request(json, req));
}

TEST(ProtocolTest, SerializeResponse) {
    ControlResponse resp;
    resp.id = 42;
    resp.success = true;
    resp.data = "{\"count\":3}";
    std::string json = serialize_response(resp);
    EXPECT_NE(json.find("\"id\":42"), std::string::npos);
    EXPECT_NE(json.find("\"success\":true"), std::string::npos);
    EXPECT_NE(json.find("\"data\":{\"count\":3}"), std::string::npos);
}

TEST(ProtocolTest, SerializeResponseError) {
    ControlResponse resp;
    resp.id = 1;
    resp.success = false;
    resp.error = "route not found";
    std::string json = serialize_response(resp);
    EXPECT_NE(json.find("\"success\":false"), std::string::npos);
    EXPECT_NE(json.find("\"error\":\"route not found\""), std::string::npos);
}

TEST(ProtocolTest, ParseResponse) {
    std::string json = R"({"id":42,"success":true,"data":{"count":3}})";
    ControlResponse resp;
    EXPECT_TRUE(parse_response(json, resp));
    EXPECT_EQ(resp.id, 42u);
    EXPECT_TRUE(resp.success);
    EXPECT_FALSE(resp.data.empty());
}

TEST(ProtocolTest, RequestRoundtrip) {
    ControlRequest req;
    req.id = 100;
    req.command = CommandType::StreamCreate;
    req.params["source"] = "mic1";
    req.params["sink"] = "speaker1";

    std::string json = serialize_request(req);
    ControlRequest parsed;
    EXPECT_TRUE(parse_request(json, parsed));
    EXPECT_EQ(parsed.id, req.id);
    EXPECT_EQ(parsed.command, req.command);
    EXPECT_EQ(parsed.get_param("source"), "mic1");
    EXPECT_EQ(parsed.get_param("sink"), "speaker1");
}

TEST(ProtocolTest, GetParamDefault) {
    ControlRequest req;
    EXPECT_EQ(req.get_param("missing"), "");
    EXPECT_EQ(req.get_param("missing", "default"), "default");
}
