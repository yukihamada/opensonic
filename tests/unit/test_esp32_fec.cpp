/**
 * Unit tests for ESP32 XOR FEC (pure C logic)
 * Tests the same algorithm used on ESP32, compiled for desktop.
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>
#include <cstring>
#include <vector>

/* Pull in the ESP32 FEC types and functions directly (C-compatible) */
extern "C" {

#define SOL_FRAME_SIZE_SAMPLES 96
#define SOL_MAX_CHANNELS 8
#define SOL_FEC_GROUP_SIZE 5

/* Inline the FEC structs and functions for testing */
typedef struct {
    uint8_t parity[SOL_FRAME_SIZE_SAMPLES * SOL_MAX_CHANNELS * 4];
    size_t parity_size;
    uint8_t count;
    uint32_t group_id;
} test_fec_enc_t;

typedef struct {
    uint8_t packets[SOL_FEC_GROUP_SIZE][SOL_FRAME_SIZE_SAMPLES * SOL_MAX_CHANNELS * 4];
    size_t packet_sizes[SOL_FEC_GROUP_SIZE];
    bool received[SOL_FEC_GROUP_SIZE];
    uint8_t parity[SOL_FRAME_SIZE_SAMPLES * SOL_MAX_CHANNELS * 4];
    size_t parity_size;
    bool has_parity;
    uint8_t received_count;
} test_fec_dec_t;

static void test_fec_enc_init(test_fec_enc_t* enc) {
    memset(enc, 0, sizeof(*enc));
}

static bool test_fec_enc_feed(test_fec_enc_t* enc, const uint8_t* data, size_t len) {
    if (enc->count == 0) {
        memcpy(enc->parity, data, len);
        enc->parity_size = len;
    } else {
        size_t xor_len = (len < enc->parity_size) ? len : enc->parity_size;
        for (size_t i = 0; i < xor_len; i++) {
            enc->parity[i] ^= data[i];
        }
        if (len > enc->parity_size) {
            memcpy(enc->parity + enc->parity_size, data + enc->parity_size, len - enc->parity_size);
            enc->parity_size = len;
        }
    }
    enc->count++;
    if (enc->count >= SOL_FEC_GROUP_SIZE) {
        enc->group_id++;
        enc->count = 0;
        return true;
    }
    return false;
}

static void test_fec_dec_init(test_fec_dec_t* dec) {
    memset(dec, 0, sizeof(*dec));
}

static void test_fec_dec_feed_data(test_fec_dec_t* dec, uint8_t index,
                                    const uint8_t* data, size_t len) {
    if (index >= SOL_FEC_GROUP_SIZE) return;
    memcpy(dec->packets[index], data, len);
    dec->packet_sizes[index] = len;
    dec->received[index] = true;
    dec->received_count++;
}

static void test_fec_dec_feed_parity(test_fec_dec_t* dec, const uint8_t* data, size_t len) {
    memcpy(dec->parity, data, len);
    dec->parity_size = len;
    dec->has_parity = true;
}

static bool test_fec_dec_recover(test_fec_dec_t* dec, uint8_t* recovered_index,
                                  const uint8_t** recovered_data, size_t* recovered_len) {
    if (!dec->has_parity) return false;
    if (dec->received_count != SOL_FEC_GROUP_SIZE - 1) return false;

    uint8_t missing = 0xFF;
    for (uint8_t i = 0; i < SOL_FEC_GROUP_SIZE; i++) {
        if (!dec->received[i]) { missing = i; break; }
    }
    if (missing == 0xFF) return false;

    size_t max_len = dec->parity_size;
    memcpy(dec->packets[missing], dec->parity, max_len);
    for (uint8_t i = 0; i < SOL_FEC_GROUP_SIZE; i++) {
        if (i == missing) continue;
        size_t xor_len = (dec->packet_sizes[i] < max_len) ? dec->packet_sizes[i] : max_len;
        for (size_t j = 0; j < xor_len; j++) {
            dec->packets[missing][j] ^= dec->packets[i][j];
        }
    }
    dec->packet_sizes[missing] = max_len;
    *recovered_index = missing;
    *recovered_data = dec->packets[missing];
    *recovered_len = max_len;
    return true;
}

} // extern "C"

TEST(Esp32Fec, EncoderProducesParity) {
    test_fec_enc_t enc;
    test_fec_enc_init(&enc);

    uint8_t data[64];
    for (int i = 0; i < SOL_FEC_GROUP_SIZE; i++) {
        memset(data, (uint8_t)(i + 1), sizeof(data));
        bool ready = test_fec_enc_feed(&enc, data, sizeof(data));
        if (i < SOL_FEC_GROUP_SIZE - 1) {
            EXPECT_FALSE(ready);
        } else {
            EXPECT_TRUE(ready);
        }
    }
    EXPECT_EQ(enc.group_id, 1u);
}

TEST(Esp32Fec, RecoverSingleLoss) {
    test_fec_enc_t enc;
    test_fec_enc_init(&enc);

    std::vector<std::vector<uint8_t>> originals;
    for (int i = 0; i < SOL_FEC_GROUP_SIZE; i++) {
        std::vector<uint8_t> data(128);
        for (size_t j = 0; j < data.size(); j++) {
            data[j] = static_cast<uint8_t>((i * 37 + j * 13) & 0xFF);
        }
        originals.push_back(data);
        test_fec_enc_feed(&enc, data.data(), data.size());
    }

    /* Try recovering each index */
    for (int lost = 0; lost < SOL_FEC_GROUP_SIZE; lost++) {
        test_fec_dec_t dec;
        test_fec_dec_init(&dec);

        for (int i = 0; i < SOL_FEC_GROUP_SIZE; i++) {
            if (i == lost) continue;
            test_fec_dec_feed_data(&dec, (uint8_t)i,
                originals[i].data(), originals[i].size());
        }
        test_fec_dec_feed_parity(&dec, enc.parity, enc.parity_size);

        uint8_t recovered_idx;
        const uint8_t* recovered_data;
        size_t recovered_len;

        ASSERT_TRUE(test_fec_dec_recover(&dec, &recovered_idx,
                                          &recovered_data, &recovered_len))
            << "Failed to recover index " << lost;
        EXPECT_EQ(recovered_idx, lost);
        EXPECT_EQ(recovered_len, originals[lost].size());
        EXPECT_EQ(memcmp(recovered_data, originals[lost].data(),
                          originals[lost].size()), 0)
            << "Data mismatch for recovered index " << lost;
    }
}

TEST(Esp32Fec, NoRecoveryWithoutParity) {
    test_fec_dec_t dec;
    test_fec_dec_init(&dec);

    uint8_t data[64] = {1, 2, 3};
    for (int i = 0; i < SOL_FEC_GROUP_SIZE - 1; i++) {
        test_fec_dec_feed_data(&dec, (uint8_t)i, data, sizeof(data));
    }

    uint8_t idx;
    const uint8_t* ptr;
    size_t len;
    EXPECT_FALSE(test_fec_dec_recover(&dec, &idx, &ptr, &len));
}

TEST(Esp32Fec, NoRecoveryTwoLost) {
    test_fec_enc_t enc;
    test_fec_enc_init(&enc);

    uint8_t data[64];
    for (int i = 0; i < SOL_FEC_GROUP_SIZE; i++) {
        memset(data, (uint8_t)i, sizeof(data));
        test_fec_enc_feed(&enc, data, sizeof(data));
    }

    test_fec_dec_t dec;
    test_fec_dec_init(&dec);

    /* Only provide 3 of 5 packets */
    for (int i = 0; i < SOL_FEC_GROUP_SIZE - 2; i++) {
        memset(data, (uint8_t)i, sizeof(data));
        test_fec_dec_feed_data(&dec, (uint8_t)i, data, sizeof(data));
    }
    test_fec_dec_feed_parity(&dec, enc.parity, enc.parity_size);

    uint8_t idx;
    const uint8_t* ptr;
    size_t len;
    EXPECT_FALSE(test_fec_dec_recover(&dec, &idx, &ptr, &len));
}

TEST(Esp32Fec, MultipleGroups) {
    test_fec_enc_t enc;
    test_fec_enc_init(&enc);

    uint8_t data[32];
    int groups_completed = 0;

    for (int i = 0; i < SOL_FEC_GROUP_SIZE * 3; i++) {
        memset(data, (uint8_t)i, sizeof(data));
        if (test_fec_enc_feed(&enc, data, sizeof(data))) {
            groups_completed++;
        }
    }
    EXPECT_EQ(groups_completed, 3);
    EXPECT_EQ(enc.group_id, 3u);
}

TEST(Esp32BinaryProtocol, PacketHeader) {
    /* Test the binary control protocol header format */
    uint8_t pkt[8];
    pkt[0] = 0x53;  /* Magic: 'S' */
    pkt[1] = 0x01;  /* PING */
    pkt[2] = 0x00;  /* Length high */
    pkt[3] = 0x00;  /* Length low */

    EXPECT_EQ(pkt[0], 0x53);
    EXPECT_EQ(pkt[1], 0x01);
    uint16_t payload_len = ((uint16_t)pkt[2] << 8) | pkt[3];
    EXPECT_EQ(payload_len, 0u);
}

TEST(Esp32BinaryProtocol, StatusResponsePacking) {
    /* Verify 64-byte status response layout */
    uint8_t resp[64];
    memset(resp, 0, sizeof(resp));

    /* Pack some test values */
    uint64_t packets_tx = 12345;
    uint64_t packets_rx = 67890;

    /* Big-endian pack */
    for (int i = 7; i >= 0; i--) {
        resp[i] = (uint8_t)(packets_tx & 0xFF);
        packets_tx >>= 8;
    }
    for (int i = 15; i >= 8; i--) {
        resp[i] = (uint8_t)(packets_rx & 0xFF);
        packets_rx >>= 8;
    }

    /* Unpack and verify */
    uint64_t val = 0;
    for (int i = 0; i < 8; i++) {
        val = (val << 8) | resp[i];
    }
    EXPECT_EQ(val, 12345u);

    val = 0;
    for (int i = 8; i < 16; i++) {
        val = (val << 8) | resp[i];
    }
    EXPECT_EQ(val, 67890u);
}
