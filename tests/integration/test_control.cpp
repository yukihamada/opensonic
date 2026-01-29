/**
 * Integration tests for Control Plane
 * Tests handle_command() with real Discovery/SessionManager/RoutingMatrix
 * SPDX-License-Identifier: MIT
 */

#include <soluna/control/discovery.h>
#include <soluna/control/session.h>
#include <soluna/control/routing.h>
#include <soluna/control/protocol.h>
#include <gtest/gtest.h>

namespace soluna::sync { class PtpEngine; }
namespace soluna::control {
ControlResponse handle_command(
    const ControlRequest& req,
    Discovery& discovery,
    SessionManager& sessions,
    RoutingMatrix& routing,
    sync::PtpEngine* ptp = nullptr);
}

using namespace soluna::control;

class ControlIntegrationTest : public ::testing::Test {
protected:
    Discovery discovery;
    SessionManager sessions;
    RoutingMatrix routing;

    ControlResponse exec(CommandType cmd,
                         std::map<std::string, std::string> params = {}) {
        ControlRequest req;
        req.id = 1;
        req.command = cmd;
        req.params = std::move(params);
        return handle_command(req, discovery, sessions, routing);
    }
};

TEST_F(ControlIntegrationTest, Version) {
    auto resp = exec(CommandType::Version);
    EXPECT_TRUE(resp.success);
    EXPECT_NE(resp.data.find("version"), std::string::npos);
}

TEST_F(ControlIntegrationTest, Status) {
    auto resp = exec(CommandType::Status);
    EXPECT_TRUE(resp.success);
    EXPECT_NE(resp.data.find("devices"), std::string::npos);
    EXPECT_NE(resp.data.find("streams"), std::string::npos);
    EXPECT_NE(resp.data.find("routes"), std::string::npos);
}

TEST_F(ControlIntegrationTest, DeviceList) {
    DeviceInfo d;
    d.id = "dev1"; d.name = "Test"; d.host = "192.168.1.1";
    d.input_channels = 2; d.output_channels = 2;
    discovery.add_device(d);

    auto resp = exec(CommandType::DeviceList);
    EXPECT_TRUE(resp.success);
    EXPECT_NE(resp.data.find("dev1"), std::string::npos);
    EXPECT_NE(resp.data.find("Test"), std::string::npos);
}

TEST_F(ControlIntegrationTest, StreamCreateAndList) {
    auto resp = exec(CommandType::StreamCreate, {
        {"source", "devA"}, {"sink", "devB"}, {"channels", "2"}
    });
    EXPECT_TRUE(resp.success);
    EXPECT_NE(resp.data.find("stream_id"), std::string::npos);

    auto list_resp = exec(CommandType::StreamList);
    EXPECT_TRUE(list_resp.success);
    EXPECT_NE(list_resp.data.find("devA"), std::string::npos);
}

TEST_F(ControlIntegrationTest, StreamCreateMissingParams) {
    auto resp = exec(CommandType::StreamCreate, {{"source", "devA"}});
    EXPECT_FALSE(resp.success);
    EXPECT_NE(resp.error.find("required"), std::string::npos);
}

TEST_F(ControlIntegrationTest, StreamDestroy) {
    auto create = exec(CommandType::StreamCreate, {
        {"source", "devA"}, {"sink", "devB"}
    });
    EXPECT_TRUE(create.success);

    // Extract stream_id from response data
    auto resp = exec(CommandType::StreamDestroy, {{"stream_id", "1"}});
    EXPECT_TRUE(resp.success);
}

TEST_F(ControlIntegrationTest, StreamDestroyNotFound) {
    auto resp = exec(CommandType::StreamDestroy, {{"stream_id", "999"}});
    EXPECT_FALSE(resp.success);
}

TEST_F(ControlIntegrationTest, RouteAddAndList) {
    auto resp = exec(CommandType::RouteAdd, {
        {"source", "devA:1"}, {"sink", "devB:1"}, {"gain_db", "-3"}
    });
    EXPECT_TRUE(resp.success);

    auto list_resp = exec(CommandType::RouteList);
    EXPECT_TRUE(list_resp.success);
    EXPECT_NE(list_resp.data.find("devA:1"), std::string::npos);
}

TEST_F(ControlIntegrationTest, RouteAddDuplicate) {
    exec(CommandType::RouteAdd, {
        {"source", "devA:1"}, {"sink", "devB:1"}
    });
    auto resp = exec(CommandType::RouteAdd, {
        {"source", "devA:1"}, {"sink", "devB:1"}
    });
    EXPECT_FALSE(resp.success);
}

TEST_F(ControlIntegrationTest, RouteRemove) {
    exec(CommandType::RouteAdd, {
        {"source", "devA:1"}, {"sink", "devB:1"}
    });
    auto resp = exec(CommandType::RouteRemove, {
        {"source", "devA:1"}, {"sink", "devB:1"}
    });
    EXPECT_TRUE(resp.success);
}

TEST_F(ControlIntegrationTest, RouteSetGain) {
    exec(CommandType::RouteAdd, {
        {"source", "devA:1"}, {"sink", "devB:1"}
    });
    auto resp = exec(CommandType::RouteSetGain, {
        {"source", "devA:1"}, {"sink", "devB:1"}, {"gain_db", "-6"}
    });
    EXPECT_TRUE(resp.success);
}

TEST_F(ControlIntegrationTest, RouteSetMute) {
    exec(CommandType::RouteAdd, {
        {"source", "devA:1"}, {"sink", "devB:1"}
    });
    auto resp = exec(CommandType::RouteSetMute, {
        {"source", "devA:1"}, {"sink", "devB:1"}, {"muted", "true"}
    });
    EXPECT_TRUE(resp.success);
}

TEST_F(ControlIntegrationTest, MeterGet) {
    auto resp = exec(CommandType::MeterGet, {{"channel", "devA:1"}});
    EXPECT_TRUE(resp.success);
    EXPECT_NE(resp.data.find("peak_db"), std::string::npos);
}

TEST_F(ControlIntegrationTest, MeterGetMissingChannel) {
    auto resp = exec(CommandType::MeterGet);
    EXPECT_FALSE(resp.success);
}

TEST_F(ControlIntegrationTest, UnknownCommand) {
    auto resp = exec(CommandType::Unknown);
    EXPECT_FALSE(resp.success);
}

TEST_F(ControlIntegrationTest, FullWorkflow) {
    // 1. Add device
    DeviceInfo d;
    d.id = "mixer1"; d.name = "Mixer"; d.host = "10.0.0.1";
    d.input_channels = 8; d.output_channels = 8;
    discovery.add_device(d);

    // 2. Create stream
    auto stream_resp = exec(CommandType::StreamCreate, {
        {"source", "mixer1"}, {"sink", "speaker1"}, {"channels", "2"}
    });
    EXPECT_TRUE(stream_resp.success);

    // 3. Add routes
    exec(CommandType::RouteAdd, {{"source", "mixer1:1"}, {"sink", "speaker1:1"}});
    exec(CommandType::RouteAdd, {{"source", "mixer1:2"}, {"sink", "speaker1:2"}});

    // 4. Set gain
    exec(CommandType::RouteSetGain, {
        {"source", "mixer1:1"}, {"sink", "speaker1:1"}, {"gain_db", "-3"}
    });

    // 5. Check status
    auto status = exec(CommandType::Status);
    EXPECT_TRUE(status.success);
    EXPECT_NE(status.data.find("\"streams\":1"), std::string::npos);
    EXPECT_NE(status.data.find("\"routes\":2"), std::string::npos);
}
