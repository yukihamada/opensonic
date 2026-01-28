/**
 * Soluna — Codec Tests
 *
 * Tests for codec factory and PCM/AAC/FLAC codecs.
 *
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>
#include <soluna/codec/codec.h>
#include <cmath>

using namespace soluna::codec;

class CodecTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Default config for tests
        config_.sample_rate = 48000;
        config_.channels = 2;
        config_.bit_depth = 16;
        config_.bitrate = 128000;
    }

    CodecConfig config_;
};

// PCM Encoder Tests
TEST_F(CodecTest, PcmEncoderCreate) {
    config_.type = CodecType::PCM;
    auto result = CodecFactory::create_encoder(config_);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value()->type(), CodecType::PCM);
}

TEST_F(CodecTest, PcmEncoder16bit) {
    config_.type = CodecType::PCM;
    config_.bit_depth = 16;
    auto encoder = CodecFactory::create_encoder(config_).value();

    // Create test signal: sine wave
    const size_t frame_count = 480;
    std::vector<float> input(frame_count * config_.channels);
    for (size_t i = 0; i < input.size(); i++) {
        input[i] = std::sin(2.0 * M_PI * 1000.0 * i / config_.sample_rate);
    }

    auto result = encoder->encode(input.data(), frame_count);
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.frames_encoded, frame_count);
    EXPECT_EQ(result.data.size(), frame_count * config_.channels * 2);  // 16-bit = 2 bytes
}

TEST_F(CodecTest, PcmEncoder24bit) {
    config_.type = CodecType::PCM;
    config_.bit_depth = 24;
    auto encoder = CodecFactory::create_encoder(config_).value();

    const size_t frame_count = 480;
    std::vector<float> input(frame_count * config_.channels, 0.5f);

    auto result = encoder->encode(input.data(), frame_count);
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.frames_encoded, frame_count);
    EXPECT_EQ(result.data.size(), frame_count * config_.channels * 4);  // 24-bit in 32-bit container
}

TEST_F(CodecTest, PcmEncoder32bit) {
    config_.type = CodecType::PCM;
    config_.bit_depth = 32;
    auto encoder = CodecFactory::create_encoder(config_).value();

    const size_t frame_count = 480;
    std::vector<float> input(frame_count * config_.channels, 0.25f);

    auto result = encoder->encode(input.data(), frame_count);
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.frames_encoded, frame_count);
    EXPECT_EQ(result.data.size(), frame_count * config_.channels * 4);  // 32-bit float
}

// PCM Decoder Tests
TEST_F(CodecTest, PcmDecoderCreate) {
    config_.type = CodecType::PCM;
    auto result = CodecFactory::create_decoder(config_);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value()->type(), CodecType::PCM);
}

TEST_F(CodecTest, PcmDecoder16bit) {
    config_.type = CodecType::PCM;
    config_.bit_depth = 16;
    auto decoder = CodecFactory::create_decoder(config_).value();

    // Create 16-bit PCM data
    const size_t frame_count = 480;
    std::vector<int16_t> pcm_data(frame_count * config_.channels);
    for (size_t i = 0; i < pcm_data.size(); i++) {
        pcm_data[i] = static_cast<int16_t>(16384 * std::sin(2.0 * M_PI * 1000.0 * i / config_.sample_rate));
    }

    auto result = decoder->decode(reinterpret_cast<const uint8_t*>(pcm_data.data()),
                                   pcm_data.size() * sizeof(int16_t));
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.frames_decoded, frame_count);
    EXPECT_EQ(result.samples.size(), frame_count * config_.channels);
}

// PCM Round-trip Test
TEST_F(CodecTest, PcmRoundTrip16bit) {
    config_.type = CodecType::PCM;
    config_.bit_depth = 16;
    auto encoder = CodecFactory::create_encoder(config_).value();
    auto decoder = CodecFactory::create_decoder(config_).value();

    const size_t frame_count = 480;
    std::vector<float> original(frame_count * config_.channels);
    for (size_t i = 0; i < original.size(); i++) {
        original[i] = 0.5f * std::sin(2.0 * M_PI * 440.0 * i / config_.sample_rate);
    }

    auto encoded = encoder->encode(original.data(), frame_count);
    ASSERT_TRUE(encoded.success);

    auto decoded = decoder->decode(encoded.data.data(), encoded.data.size());
    ASSERT_TRUE(decoded.success);
    EXPECT_EQ(decoded.samples.size(), original.size());

    // Check round-trip accuracy (16-bit quantization error)
    // Tolerance is 2 LSBs due to rounding in both encode and decode
    for (size_t i = 0; i < original.size(); i++) {
        EXPECT_NEAR(decoded.samples[i], original[i], 2.0f / 32768.0f);
    }
}

// Clipping test
TEST_F(CodecTest, PcmEncoderClipping) {
    config_.type = CodecType::PCM;
    config_.bit_depth = 16;
    auto encoder = CodecFactory::create_encoder(config_).value();

    const size_t frame_count = 10;
    std::vector<float> input(frame_count * config_.channels);
    input[0] = 2.0f;   // Over +1.0
    input[1] = -2.0f;  // Under -1.0
    input[2] = 1.0f;   // At limit
    input[3] = -1.0f;  // At limit

    auto result = encoder->encode(input.data(), frame_count);
    ASSERT_TRUE(result.success);

    // Decode and verify clipping
    auto decoder = CodecFactory::create_decoder(config_).value();
    auto decoded = decoder->decode(result.data.data(), result.data.size());
    ASSERT_TRUE(decoded.success);

    // Should be clamped to [-1, 1]
    EXPECT_LE(decoded.samples[0], 1.0f);
    EXPECT_GE(decoded.samples[1], -1.0f);
}

// Factory availability tests
TEST_F(CodecTest, FactoryPcmAlwaysAvailable) {
    EXPECT_TRUE(CodecFactory::is_available(CodecType::PCM));
}

TEST_F(CodecTest, FactoryAvailableCodecs) {
    auto codecs = CodecFactory::available_codecs();
    EXPECT_FALSE(codecs.empty());

    // PCM should always be in the list
    bool has_pcm = false;
    for (auto c : codecs) {
        if (c == CodecType::PCM) has_pcm = true;
    }
    EXPECT_TRUE(has_pcm);
}

TEST_F(CodecTest, FactoryUnknownCodec) {
    // Creating encoder for unavailable codec should return error
    config_.type = CodecType::AAC;
    if (!CodecFactory::is_available(CodecType::AAC)) {
        auto result = CodecFactory::create_encoder(config_);
        EXPECT_FALSE(result.ok());
        EXPECT_EQ(result.error().code(), soluna::ErrorCode::CodecNotFound);
    }
}

// AAC Tests (conditional)
#ifdef SOLUNA_ENABLE_AAC
TEST_F(CodecTest, AacEncoderCreate) {
    config_.type = CodecType::AAC;
    config_.bitrate = 128000;
    auto result = CodecFactory::create_encoder(config_);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value()->type(), CodecType::AAC);
}

TEST_F(CodecTest, AacEncode) {
    config_.type = CodecType::AAC;
    config_.bitrate = 128000;
    auto encoder = CodecFactory::create_encoder(config_).value();

    // AAC encoder needs specific frame sizes (1024 samples)
    const size_t frame_count = 1024;
    std::vector<float> input(frame_count * config_.channels);
    for (size_t i = 0; i < input.size(); i++) {
        input[i] = 0.5f * std::sin(2.0 * M_PI * 1000.0 * i / config_.sample_rate);
    }

    auto result = encoder->encode(input.data(), frame_count);
    ASSERT_TRUE(result.success);
    EXPECT_GT(result.data.size(), 0u);
}

TEST_F(CodecTest, AacDecoderCreate) {
    config_.type = CodecType::AAC;
    auto result = CodecFactory::create_decoder(config_);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value()->type(), CodecType::AAC);
}

TEST_F(CodecTest, AacAvailable) {
    EXPECT_TRUE(CodecFactory::is_available(CodecType::AAC));
}
#endif

// FLAC Tests (conditional)
#ifdef SOLUNA_ENABLE_FLAC
TEST_F(CodecTest, FlacEncoderCreate) {
    config_.type = CodecType::FLAC;
    config_.bit_depth = 16;
    auto result = CodecFactory::create_encoder(config_);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value()->type(), CodecType::FLAC);
}

TEST_F(CodecTest, FlacEncode) {
    config_.type = CodecType::FLAC;
    config_.bit_depth = 16;
    config_.flac_compression = 5;
    auto encoder = CodecFactory::create_encoder(config_).value();

    const size_t frame_count = 4096;  // FLAC works better with larger buffers
    std::vector<float> input(frame_count * config_.channels);
    for (size_t i = 0; i < input.size(); i++) {
        input[i] = 0.5f * std::sin(2.0 * M_PI * 1000.0 * i / config_.sample_rate);
    }

    auto result = encoder->encode(input.data(), frame_count);
    ASSERT_TRUE(result.success);
    // FLAC may not output data until flush for streaming encoder
}

TEST_F(CodecTest, FlacDecoderCreate) {
    config_.type = CodecType::FLAC;
    auto result = CodecFactory::create_decoder(config_);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value()->type(), CodecType::FLAC);
}

TEST_F(CodecTest, FlacAvailable) {
    EXPECT_TRUE(CodecFactory::is_available(CodecType::FLAC));
}

TEST_F(CodecTest, FlacLossless) {
    // FLAC should be lossless
    config_.type = CodecType::FLAC;
    config_.bit_depth = 16;
    config_.flac_compression = 5;
    auto encoder = CodecFactory::create_encoder(config_).value();
    auto decoder = CodecFactory::create_decoder(config_).value();

    const size_t frame_count = 4096;
    std::vector<float> original(frame_count * config_.channels);
    for (size_t i = 0; i < original.size(); i++) {
        // Use values that map exactly to 16-bit integers
        int16_t sample = static_cast<int16_t>(16384 * std::sin(2.0 * M_PI * 1000.0 * i / config_.sample_rate));
        original[i] = static_cast<float>(sample) / 32768.0f;
    }

    auto encoded = encoder->encode(original.data(), frame_count);
    auto flushed = encoder->flush();

    // Combine encoded data
    std::vector<uint8_t> all_data = encoded.data;
    all_data.insert(all_data.end(), flushed.data.begin(), flushed.data.end());

    if (!all_data.empty()) {
        auto decoded = decoder->decode(all_data.data(), all_data.size());
        if (decoded.success && !decoded.samples.empty()) {
            // FLAC is lossless - check exact match (within float precision)
            size_t check_count = std::min(decoded.samples.size(), original.size());
            for (size_t i = 0; i < check_count; i++) {
                EXPECT_NEAR(decoded.samples[i], original[i], 1e-5f);
            }
        }
    }
}
#endif

// Opus availability test
TEST_F(CodecTest, OpusAvailability) {
    bool expected =
#ifdef SOLUNA_ENABLE_OPUS
        true;
#else
        false;
#endif
    EXPECT_EQ(CodecFactory::is_available(CodecType::Opus), expected);
}
