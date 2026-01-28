/**
 * Unit tests for Discovery
 * SPDX-License-Identifier: MIT
 */

#include <soluna/control/discovery.h>
#include <gtest/gtest.h>

using namespace soluna::control;

class DiscoveryTest : public ::testing::Test {
protected:
    Discovery discovery;

    DeviceInfo make_device(const std::string& id, const std::string& name,
                           uint32_t inputs = 2, uint32_t outputs = 2) {
        DeviceInfo d;
        d.id = id;
        d.name = name;
        d.host = "192.168.1.100";
        d.input_channels = inputs;
        d.output_channels = outputs;
        d.sample_rate = 48000;
        d.last_seen_ns = 1000000000;
        return d;
    }
};

TEST_F(DiscoveryTest, AddDevice) {
    discovery.add_device(make_device("dev1", "Device 1"));
    auto devices = discovery.devices();
    ASSERT_EQ(devices.size(), 1u);
    EXPECT_EQ(devices[0].id, "dev1");
    EXPECT_EQ(devices[0].name, "Device 1");
}

TEST_F(DiscoveryTest, AddMultipleDevices) {
    discovery.add_device(make_device("dev1", "Device 1"));
    discovery.add_device(make_device("dev2", "Device 2"));
    discovery.add_device(make_device("dev3", "Device 3"));
    auto devices = discovery.devices();
    EXPECT_EQ(devices.size(), 3u);
}

TEST_F(DiscoveryTest, FindDevice) {
    discovery.add_device(make_device("dev1", "Device 1"));
    auto d = discovery.find_device("dev1");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->name, "Device 1");
}

TEST_F(DiscoveryTest, FindNonexistent) {
    auto d = discovery.find_device("nonexistent");
    EXPECT_EQ(d, nullptr);
}

TEST_F(DiscoveryTest, RemoveDevice) {
    discovery.add_device(make_device("dev1", "Device 1"));
    discovery.remove_device("dev1");
    EXPECT_EQ(discovery.devices().size(), 0u);
}

TEST_F(DiscoveryTest, UpdateDevice) {
    auto d1 = make_device("dev1", "Old Name");
    discovery.add_device(d1);
    auto d2 = make_device("dev1", "New Name");
    discovery.add_device(d2);
    auto devices = discovery.devices();
    ASSERT_EQ(devices.size(), 1u);
    EXPECT_EQ(devices[0].name, "New Name");
}

TEST_F(DiscoveryTest, Callback) {
    int added = 0, removed = 0, updated = 0;
    discovery.set_callback([&](DiscoveryEvent event, const DeviceInfo&) {
        switch (event) {
            case DiscoveryEvent::DeviceAdded: added++; break;
            case DiscoveryEvent::DeviceRemoved: removed++; break;
            case DiscoveryEvent::DeviceUpdated: updated++; break;
        }
    });

    discovery.add_device(make_device("dev1", "Device 1"));
    EXPECT_EQ(added, 1);

    discovery.add_device(make_device("dev1", "Updated"));
    EXPECT_EQ(updated, 1);

    discovery.remove_device("dev1");
    EXPECT_EQ(removed, 1);
}

TEST_F(DiscoveryTest, PruneStale) {
    // add_device sets last_seen_ns to current monotonic time.
    // Prune with a very large timeout => device should NOT be pruned.
    discovery.add_device(make_device("dev1", "Device 1"));
    discovery.prune_stale(999999999999LL);  // ~1000 seconds
    EXPECT_EQ(discovery.devices().size(), 1u);

    // Prune with timeout of 0 => everything older than "now" is stale
    // Since add happened in the past (even nanoseconds ago), it will be pruned.
    discovery.prune_stale(0);
    EXPECT_EQ(discovery.devices().size(), 0u);
}

TEST_F(DiscoveryTest, SetLocalDevice) {
    auto local = make_device("local1", "Local Device");
    local.is_local = true;
    discovery.set_local_device(local);
    // Local device should be accessible
    auto d = discovery.find_device("local1");
    ASSERT_NE(d, nullptr);
    EXPECT_TRUE(d->is_local);
}
