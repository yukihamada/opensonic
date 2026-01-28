/**
 * Unit tests for Opus Wrapper (stub mode — no libopus required)
 * SPDX-License-Identifier: MIT
 */

#include <soluna/codec/opus_wrapper.h>
#include <gtest/gtest.h>
#include <cmath>
#include <vector>

using namespace soluna::codec;

TEST(OpusEncoderTest, CreateAndInitialize) {
    OpusEncoderConfig config;
    config.sample_rate = 48000;
    config.channels = 1;
    config.bitrate = 96000;
    OpusEncoder encoder(config);
    EXPECT_TRUE(encoder.is_initialized());
}

TEST(OpusEncoderTest, EncodeSineWave) {
    OpusEncoderConfig config;
    config.sample_rate = 48000;
    config.channels = 1;
    config.frame_size_samples = 96;
    OpusEncoder encoder(config);

    // Generate 96 samples of 1kHz sine
    std::vector<float> input(96);
    for (size_t i = 0; i < input.size(); i++) {
        input[i] = 0.5f * std::sin(2.0f * M_PI * 1000.0f * i / 48000.0f);
    }

    auto result = encoder.encode(input.data(), 96);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.frames_encoded, 96u);
    EXPECT_GT(result.data.size(), 0u);
}

TEST(OpusEncoderTest, EncodeStereo) {
    OpusEncoderConfig config;
    config.sample_rate = 48000;
    config.channels = 2;
    config.bitrate = 128000;
    OpusEncoder encoder(config);

    std::vector<float> input(96 * 2, 0.3f); // interleaved stereo
    auto result = encoder.encode(input.data(), 96);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.frames_encoded, 96u);
}

TEST(OpusEncoderTest, EncodeEmpty) {
    OpusEncoder encoder;
    auto result = encoder.encode(nullptr, 0);
    EXPECT_FALSE(result.success);
}

TEST(OpusEncoderTest, SetBitrate) {
    OpusEncoder encoder;
    encoder.set_bitrate(64000);
    EXPECT_EQ(encoder.config().bitrate, 64000u);
}

TEST(OpusEncoderTest, SetPacketLoss) {
    OpusEncoder encoder;
    encoder.set_packet_loss_pct(5);
    EXPECT_EQ(encoder.config().packet_loss_pct, 5);
}

TEST(OpusDecoderTest, CreateAndInitialize) {
    OpusDecoderConfig config;
    config.sample_rate = 48000;
    config.channels = 1;
    OpusDecoder decoder(config);
    EXPECT_TRUE(decoder.is_initialized());
}

TEST(OpusDecoderTest, DecodeStubData) {
    OpusEncoderConfig enc_config;
    enc_config.sample_rate = 48000;
    enc_config.channels = 1;
    OpusEncoder encoder(enc_config);

    OpusDecoderConfig dec_config;
    dec_config.sample_rate = 48000;
    dec_config.channels = 1;
    OpusDecoder decoder(dec_config);

    // Encode
    std::vector<float> input(96, 0.5f);
    auto encoded = encoder.encode(input.data(), 96);
    ASSERT_TRUE(encoded.success);

    // Decode
    auto decoded = decoder.decode(encoded.data.data(), encoded.data.size(), 96);
    EXPECT_TRUE(decoded.success);
    EXPECT_EQ(decoded.frames_decoded, 96u);
    EXPECT_EQ(decoded.samples.size(), 96u);
}

TEST(OpusDecoderTest, RoundtripPreservesData) {
    // In stub mode, encode/decode should perfectly preserve data
    OpusEncoderConfig enc_config;
    enc_config.channels = 1;
    OpusEncoder encoder(enc_config);

    OpusDecoderConfig dec_config;
    dec_config.channels = 1;
    OpusDecoder decoder(dec_config);

    std::vector<float> input(96);
    for (size_t i = 0; i < 96; i++) {
        input[i] = 0.5f * std::sin(2.0f * M_PI * i / 96.0f);
    }

    auto encoded = encoder.encode(input.data(), 96);
    ASSERT_TRUE(encoded.success);

    auto decoded = decoder.decode(encoded.data.data(), encoded.data.size(), 96);
    ASSERT_TRUE(decoded.success);

#ifndef SOLUNA_HAS_OPUS
    // Stub mode: exact match
    for (size_t i = 0; i < 96; i++) {
        EXPECT_FLOAT_EQ(decoded.samples[i], input[i]);
    }
#endif
}

TEST(OpusDecoderTest, PacketLossConcealment) {
    OpusDecoderConfig config;
    config.sample_rate = 48000;
    config.channels = 1;
    OpusDecoder decoder(config);

    auto result = decoder.decode_plc(96);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.plc_used);
    EXPECT_EQ(result.frames_decoded, 96u);
    EXPECT_EQ(result.samples.size(), 96u);
}

TEST(OpusDecoderTest, StereoDecoder) {
    OpusDecoderConfig config;
    config.sample_rate = 48000;
    config.channels = 2;
    OpusDecoder decoder(config);

    auto result = decoder.decode_plc(96);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.samples.size(), 96u * 2);
}
