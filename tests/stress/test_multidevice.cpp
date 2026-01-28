/**
 * Stress Test: 10+ device session management
 *
 * Verifies that session management, discovery, and routing can handle
 * many concurrent devices and streams.
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/control/session.h>
#include <soluna/control/discovery.h>
#include <soluna/control/routing.h>
#include <gtest/gtest.h>
#include <vector>
#include <string>

using namespace soluna::control;

class MultideviceStressTest : public ::testing::Test {
protected:
    SessionManager sessions_;
    Discovery discovery_;
    RoutingMatrix routing_;
};

TEST_F(MultideviceStressTest, TenDeviceRegistration) {
    for (int i = 0; i < 10; i++) {
        DeviceInfo dev;
        dev.id = "device_" + std::to_string(i);
        dev.name = "Device " + std::to_string(i);
        dev.host = "192.168.1." + std::to_string(100 + i);
        dev.control_port = 8400;
        dev.input_channels = 8;
        dev.output_channels = 8;
        dev.sample_rate = 48000;
        dev.is_local = (i == 0);

        discovery_.add_device(dev);
    }

    auto devices = discovery_.devices();
    EXPECT_EQ(devices.size(), 10u);
}

TEST_F(MultideviceStressTest, TenDeviceStreams) {
    // Create streams between 10 device pairs
    std::vector<uint16_t> stream_ids;
    for (int i = 0; i < 10; i++) {
        std::string src = "src_device_" + std::to_string(i);
        std::string dst = "dst_device_" + std::to_string(i);
        uint16_t id = sessions_.create_stream(src, dst, 8, 48000);
        ASSERT_NE(id, 0u) << "Failed to create stream " << i;
        stream_ids.push_back(id);
    }

    auto streams = sessions_.list_streams();
    EXPECT_EQ(streams.size(), 10u);

    // Destroy all
    for (auto id : stream_ids) {
        EXPECT_TRUE(sessions_.destroy_stream(id));
    }

    streams = sessions_.list_streams();
    EXPECT_EQ(streams.size(), 0u);
}

TEST_F(MultideviceStressTest, RapidStreamCreateDestroy) {
    // Create/destroy 1000 streams to test resource management
    for (int i = 0; i < 1000; i++) {
        std::string src = "src_" + std::to_string(i % 10);
        std::string dst = "dst_" + std::to_string(i % 10);
        uint16_t id = sessions_.create_stream(src, dst, 2);
        ASSERT_NE(id, 0u);
        EXPECT_TRUE(sessions_.destroy_stream(id));
    }

    EXPECT_EQ(sessions_.list_streams().size(), 0u);
}

TEST_F(MultideviceStressTest, ConcurrentRoutes10Devices) {
    // 10 devices, 8 channels each = 80 channels
    // Create full mesh of routes between device 0 outputs → all others inputs
    for (int dst_dev = 1; dst_dev < 10; dst_dev++) {
        for (uint32_t ch = 1; ch <= 8; ch++) {
            ChannelId src{"device_0", ch};
            ChannelId dst{"device_" + std::to_string(dst_dev), ch};
            ASSERT_TRUE(routing_.add_route(src, dst, 0.0f));
        }
    }

    // 9 devices × 8 channels = 72 routes
    EXPECT_EQ(routing_.route_count(), 72u);

    // Remove routes for device 5
    routing_.remove_device_routes("device_5");
    EXPECT_EQ(routing_.route_count(), 64u);
}

TEST_F(MultideviceStressTest, DeviceAddRemoveCycle) {
    // Simulate devices joining and leaving
    for (int cycle = 0; cycle < 100; cycle++) {
        // Add 10 devices
        for (int i = 0; i < 10; i++) {
            DeviceInfo dev;
            dev.id = "dev_" + std::to_string(i);
            dev.name = "Device " + std::to_string(i);
            dev.host = "192.168.1." + std::to_string(100 + i);
            dev.input_channels = 4;
            dev.output_channels = 4;
            dev.is_local = false;
            discovery_.add_device(dev);
        }

        EXPECT_EQ(discovery_.devices().size(), 10u);

        // Remove all
        for (int i = 0; i < 10; i++) {
            discovery_.remove_device("dev_" + std::to_string(i));
        }

        EXPECT_EQ(discovery_.devices().size(), 0u);
    }
}

TEST_F(MultideviceStressTest, StreamStateTransitions) {
    // Create streams and cycle through states
    for (int i = 0; i < 10; i++) {
        uint16_t id = sessions_.create_stream(
            "src_" + std::to_string(i),
            "dst_" + std::to_string(i), 2);
        ASSERT_NE(id, 0u);

        sessions_.set_stream_state(id, StreamState::Negotiating);
        auto info = sessions_.get_stream(id);
        ASSERT_NE(info, nullptr);
        EXPECT_EQ(info->state, StreamState::Negotiating);

        sessions_.set_stream_state(id, StreamState::Active);
        info = sessions_.get_stream(id);
        EXPECT_EQ(info->state, StreamState::Active);
    }

    // All active
    auto streams = sessions_.list_streams();
    for (const auto& s : streams) {
        EXPECT_EQ(s.state, StreamState::Active);
    }
}
