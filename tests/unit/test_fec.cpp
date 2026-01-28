/**
 * Unit tests for FEC Encoder/Decoder
 * SPDX-License-Identifier: MIT
 */

#include <soluna/wifi/fec.h>
#include <gtest/gtest.h>
#include <cstring>
#include <numeric>

using namespace soluna::wifi;

class FecXorTest : public ::testing::Test {
protected:
    FecConfig config;

    void SetUp() override {
        config.mode = FecMode::XorParity;
        config.group_size = 5;
        config.parity_count = 1;
        config.max_packet_size = 1500;
    }

    std::vector<uint8_t> make_data(size_t size, uint8_t fill) {
        std::vector<uint8_t> data(size, fill);
        return data;
    }
};

TEST_F(FecXorTest, EncoderProducesParity) {
    FecEncoder encoder(config);

    // Feed 4 packets — no parity yet
    for (int i = 0; i < 4; i++) {
        auto data = make_data(100, static_cast<uint8_t>(i + 1));
        EXPECT_FALSE(encoder.feed(data.data(), data.size()));
    }

    // 5th packet completes the group
    auto data = make_data(100, 5);
    EXPECT_TRUE(encoder.feed(data.data(), data.size()));

    auto& parity = encoder.get_parity();
    ASSERT_EQ(parity.size(), 1u);
    EXPECT_TRUE(parity[0].is_parity);
    EXPECT_EQ(parity[0].data.size(), 100u);
}

TEST_F(FecXorTest, RecoverSingleLoss) {
    FecEncoder encoder(config);
    FecDecoder decoder(config);

    // Create 5 data packets
    std::vector<std::vector<uint8_t>> data_pkts;
    for (int i = 0; i < 5; i++) {
        auto data = make_data(100, static_cast<uint8_t>(i + 10));
        data_pkts.push_back(data);
        encoder.feed(data.data(), data.size());
    }

    uint32_t group_id = encoder.current_group_id() - 1;
    auto& parity = encoder.get_parity();

    // Feed all data packets EXCEPT index 2
    for (int i = 0; i < 5; i++) {
        if (i == 2) continue; // simulate loss
        decoder.feed(group_id, static_cast<uint8_t>(i), false,
                     data_pkts[i].data(), data_pkts[i].size());
    }
    // Feed parity
    decoder.feed(group_id, parity[0].index, true,
                 parity[0].data.data(), parity[0].data.size());

    EXPECT_FALSE(decoder.is_complete(group_id));
    EXPECT_TRUE(decoder.can_recover(group_id));

    auto recovered = decoder.recover(group_id);
    ASSERT_EQ(recovered.size(), 1u);
    EXPECT_EQ(recovered[0].index, 2u);

    // Verify recovered data matches original
    ASSERT_EQ(recovered[0].data.size(), data_pkts[2].size());
    EXPECT_EQ(std::memcmp(recovered[0].data.data(), data_pkts[2].data(),
                          data_pkts[2].size()), 0);
}

TEST_F(FecXorTest, CannotRecoverTwoLosses) {
    FecEncoder encoder(config);
    FecDecoder decoder(config);

    for (int i = 0; i < 5; i++) {
        auto data = make_data(100, static_cast<uint8_t>(i));
        encoder.feed(data.data(), data.size());
    }

    uint32_t group_id = encoder.current_group_id() - 1;
    auto& parity = encoder.get_parity();

    // Feed only 3 of 5 data packets (2 lost)
    for (int i = 0; i < 3; i++) {
        auto data = make_data(100, static_cast<uint8_t>(i));
        decoder.feed(group_id, static_cast<uint8_t>(i), false,
                     data.data(), data.size());
    }
    decoder.feed(group_id, parity[0].index, true,
                 parity[0].data.data(), parity[0].data.size());

    EXPECT_FALSE(decoder.can_recover(group_id));
}

TEST_F(FecXorTest, CompleteGroupNeedsNoRecovery) {
    FecDecoder decoder(config);

    for (int i = 0; i < 5; i++) {
        auto data = make_data(100, static_cast<uint8_t>(i));
        decoder.feed(0, static_cast<uint8_t>(i), false,
                     data.data(), data.size());
    }

    EXPECT_TRUE(decoder.is_complete(0));
    EXPECT_TRUE(decoder.can_recover(0));
    auto recovered = decoder.recover(0);
    EXPECT_TRUE(recovered.empty());
}

TEST_F(FecXorTest, VariableSizePackets) {
    FecEncoder encoder(config);
    FecDecoder decoder(config);

    std::vector<std::vector<uint8_t>> data_pkts;
    for (int i = 0; i < 5; i++) {
        auto data = make_data(80 + i * 10, static_cast<uint8_t>(i + 1));
        data_pkts.push_back(data);
        encoder.feed(data.data(), data.size());
    }

    uint32_t group_id = encoder.current_group_id() - 1;
    auto& parity = encoder.get_parity();

    // Lose packet 4 (largest: 120 bytes)
    for (int i = 0; i < 4; i++) {
        decoder.feed(group_id, static_cast<uint8_t>(i), false,
                     data_pkts[i].data(), data_pkts[i].size());
    }
    decoder.feed(group_id, parity[0].index, true,
                 parity[0].data.data(), parity[0].data.size());

    EXPECT_TRUE(decoder.can_recover(group_id));
    auto recovered = decoder.recover(group_id);
    ASSERT_EQ(recovered.size(), 1u);
    // First 120 bytes should match (padded XOR)
    for (size_t j = 0; j < data_pkts[4].size(); j++) {
        EXPECT_EQ(recovered[0].data[j], data_pkts[4][j])
            << "mismatch at byte " << j;
    }
}

TEST_F(FecXorTest, MultipleGroups) {
    FecEncoder encoder(config);

    EXPECT_EQ(encoder.current_group_id(), 0u);

    for (int g = 0; g < 3; g++) {
        for (int i = 0; i < 5; i++) {
            auto data = make_data(100, static_cast<uint8_t>(g * 10 + i));
            encoder.feed(data.data(), data.size());
        }
    }

    EXPECT_EQ(encoder.current_group_id(), 3u);
}

TEST_F(FecXorTest, EncoderReset) {
    FecEncoder encoder(config);

    for (int i = 0; i < 3; i++) {
        auto data = make_data(100, static_cast<uint8_t>(i));
        encoder.feed(data.data(), data.size());
    }

    encoder.reset();
    EXPECT_EQ(encoder.current_group_id(), 0u);
}

TEST_F(FecXorTest, DecoderPrune) {
    FecDecoder decoder(config);

    for (uint32_t g = 0; g < 10; g++) {
        for (int i = 0; i < 5; i++) {
            auto data = make_data(100, static_cast<uint8_t>(i));
            decoder.feed(g, static_cast<uint8_t>(i), false,
                         data.data(), data.size());
        }
    }

    decoder.prune(3);
    // Only last 3 groups should remain
    EXPECT_FALSE(decoder.is_complete(0));
    EXPECT_TRUE(decoder.is_complete(9));
}

// Reed-Solomon tests
class FecRsTest : public ::testing::Test {
protected:
    FecConfig config;

    void SetUp() override {
        config.mode = FecMode::ReedSolomon;
        config.group_size = 5;
        config.parity_count = 2;
    }
};

TEST_F(FecRsTest, ProducesMultipleParity) {
    FecEncoder encoder(config);

    for (int i = 0; i < 5; i++) {
        auto data = std::vector<uint8_t>(100, static_cast<uint8_t>(i + 1));
        encoder.feed(data.data(), data.size());
    }

    auto& parity = encoder.get_parity();
    ASSERT_EQ(parity.size(), 2u);
    EXPECT_TRUE(parity[0].is_parity);
    EXPECT_TRUE(parity[1].is_parity);
}

TEST_F(FecRsTest, RecoverSingleLoss) {
    FecEncoder encoder(config);
    FecDecoder decoder(config);

    std::vector<std::vector<uint8_t>> data_pkts;
    for (int i = 0; i < 5; i++) {
        auto data = std::vector<uint8_t>(100, static_cast<uint8_t>(i + 1));
        data_pkts.push_back(data);
        encoder.feed(data.data(), data.size());
    }

    uint32_t group_id = encoder.current_group_id() - 1;
    auto& parity = encoder.get_parity();

    // Lose packet 1
    for (int i = 0; i < 5; i++) {
        if (i == 1) continue;
        decoder.feed(group_id, static_cast<uint8_t>(i), false,
                     data_pkts[i].data(), data_pkts[i].size());
    }
    for (const auto& p : parity) {
        decoder.feed(group_id, p.index, true, p.data.data(), p.data.size());
    }

    EXPECT_TRUE(decoder.can_recover(group_id));
    auto recovered = decoder.recover(group_id);
    EXPECT_GE(recovered.size(), 1u);
}
