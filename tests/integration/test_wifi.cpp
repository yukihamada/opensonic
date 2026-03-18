/**
 * Integration tests for WiFi audio path
 * Tests jitter buffer + FEC + adaptation + codec together
 * SPDX-License-Identifier: MIT
 */

#include <soluna/wifi/jitter_buffer.h>
#include <soluna/wifi/fec.h>
#include <soluna/wifi/adaptation.h>
#include <soluna/codec/opus_wrapper.h>
#include <gtest/gtest.h>
#include <cmath>
#include <random>
#include <vector>

using namespace soluna::wifi;
using namespace soluna::codec;

// Simulate WiFi audio path: encoder → FEC → jitter buffer → decoder
TEST(WiFiIntegration, FullPathNoLoss) {
    // Setup
    OpusEncoderConfig enc_cfg;
    enc_cfg.channels = 1;
    enc_cfg.frame_size_samples = 480;  // 10ms — valid Opus frame size
    OpusEncoder encoder(enc_cfg);

    OpusDecoderConfig dec_cfg;
    dec_cfg.channels = 1;
    OpusDecoder decoder(dec_cfg);

    FecConfig fec_cfg;
    fec_cfg.mode = FecMode::XorParity;
    fec_cfg.group_size = 5;
    FecEncoder fec_enc(fec_cfg);

    JitterBufferConfig jb_cfg;
    jb_cfg.initial_depth_ms = 20.0;
    JitterBuffer jitter(jb_cfg);

    // Generate and send 20 packets
    for (uint16_t i = 0; i < 20; i++) {
        // Generate audio (480 samples = 10ms at 48kHz)
        std::vector<float> audio(480);
        for (size_t j = 0; j < 480; j++) {
            audio[j] = 0.5f * std::sin(2.0f * M_PI * 1000.0f *
                (i * 480 + j) / 48000.0f);
        }

        // Encode
        auto encoded = encoder.encode(audio.data(), 480);
        ASSERT_TRUE(encoded.success);

        // FEC encode
        fec_enc.feed(encoded.data.data(), encoded.data.size());

        // Push to jitter buffer
        int64_t ts = i * 10000000LL; // 10ms intervals
        jitter.push(i, ts, encoded.data.data(), encoded.data.size());
    }

    // Receive from jitter buffer
    size_t decoded_count = 0;
    std::vector<uint8_t> recv_buf(480 * 4 * 2); // large enough

    while (jitter.ready()) {
        size_t got = jitter.pop(recv_buf.data(), recv_buf.size());
        if (got == 0) break;

        auto decoded = decoder.decode(recv_buf.data(), got, 480);
        EXPECT_TRUE(decoded.success);
        decoded_count++;
    }

    EXPECT_GT(decoded_count, 0u);
}

// Simulate packet loss with FEC recovery
TEST(WiFiIntegration, FecRecoveryUnderLoss) {
    FecConfig cfg;
    cfg.mode = FecMode::XorParity;
    cfg.group_size = 5;
    cfg.parity_count = 1;

    FecEncoder encoder(cfg);
    FecDecoder decoder(cfg);

    // Create 5 data packets
    std::vector<std::vector<uint8_t>> originals;
    for (int i = 0; i < 5; i++) {
        std::vector<uint8_t> data(384); // 96 samples * 4 bytes
        for (size_t j = 0; j < data.size(); j++) {
            data[j] = static_cast<uint8_t>((i * 100 + j) & 0xFF);
        }
        originals.push_back(data);
        encoder.feed(data.data(), data.size());
    }

    uint32_t group_id = encoder.current_group_id() - 1;
    auto& parity = encoder.get_parity();

    // Simulate: lose packet index 3
    int lost_idx = 3;
    for (int i = 0; i < 5; i++) {
        if (i == lost_idx) continue;
        decoder.feed(group_id, static_cast<uint8_t>(i), false,
                     originals[i].data(), originals[i].size());
    }
    decoder.feed(group_id, parity[0].index, true,
                 parity[0].data.data(), parity[0].data.size());

    auto recovered = decoder.recover(group_id);
    ASSERT_EQ(recovered.size(), 1u);
    EXPECT_EQ(recovered[0].index, lost_idx);
    EXPECT_EQ(recovered[0].data.size(), originals[lost_idx].size());
    EXPECT_EQ(std::memcmp(recovered[0].data.data(), originals[lost_idx].data(),
                          originals[lost_idx].size()), 0);
}

// Adaptation controller responds to quality changes
TEST(WiFiIntegration, AdaptationDrivesConfig) {
    AdaptationController ctrl;

    // Start with good conditions
    WiFiQualityMetrics m;
    m.jitter_ms = 0.5;
    m.packet_loss_rate = 0.0;
    ctrl.update(m);
    auto fec1 = ctrl.recommended_fec();
    auto depth1 = ctrl.recommended_jitter_depth_ms();

    // Degrade to poor conditions
    m.jitter_ms = 8.0;
    m.packet_loss_rate = 0.04;
    ctrl.update(m);
    auto fec2 = ctrl.recommended_fec();
    auto depth2 = ctrl.recommended_jitter_depth_ms();

    // FEC should be stronger
    EXPECT_GT(static_cast<int>(fec2.mode), static_cast<int>(fec1.mode));
    // Jitter buffer should be deeper
    EXPECT_GT(depth2, depth1);
}

// Opus PLC (packet loss concealment)
TEST(WiFiIntegration, OpusPLC) {
    OpusDecoderConfig cfg;
    cfg.channels = 1;
    OpusDecoder decoder(cfg);

    // First decode a real packet (480 samples = 10ms)
    OpusEncoder encoder;
    std::vector<float> audio(480, 0.3f);
    auto encoded = encoder.encode(audio.data(), 480);
    ASSERT_TRUE(encoded.success);

    auto decoded = decoder.decode(encoded.data.data(), encoded.data.size(), 480);
    ASSERT_TRUE(decoded.success);

    // Then simulate packet loss — use PLC
    auto plc = decoder.decode_plc(480);
    EXPECT_TRUE(plc.success);
    EXPECT_TRUE(plc.plc_used);
    EXPECT_EQ(plc.frames_decoded, 480u);
}

// Jitter buffer handles reordering + varying jitter
TEST(WiFiIntegration, JitterBufferWithRealisticJitter) {
    JitterBufferConfig cfg;
    cfg.initial_depth_ms = 6.0;
    cfg.max_depth_ms = 20.0;
    JitterBuffer jb(cfg);

    std::mt19937 rng(42);
    std::normal_distribution<double> jitter_dist(0.0, 1.5); // 1.5ms std dev

    // Send 100 packets with simulated jitter
    std::vector<float> audio(96, 0.5f);
    std::vector<std::pair<uint16_t, int64_t>> send_order;

    for (uint16_t i = 0; i < 100; i++) {
        double jitter_ms = jitter_dist(rng);
        int64_t arrival_ns = static_cast<int64_t>(
            (i * 2.0 + jitter_ms) * 1e6);
        if (arrival_ns < 0) arrival_ns = 0;
        send_order.push_back({i, arrival_ns});
    }

    // Simulate slight reordering by sorting by arrival time
    std::sort(send_order.begin(), send_order.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    for (const auto& [seq, ts] : send_order) {
        jb.push(seq, ts, audio.data(), audio.size() * sizeof(float));
    }

    auto stats = jb.stats();
    EXPECT_EQ(stats.packets_received, 100u);
    EXPECT_GT(stats.jitter_ms, 0.0);

    // Pop all available
    std::vector<float> out(96);
    size_t popped = 0;
    while (jb.ready()) {
        size_t got = jb.pop(out.data(), out.size() * sizeof(float));
        if (got == 0) break;
        popped++;
    }

    EXPECT_GT(popped, 10u); // should recover many packets despite jitter
}
