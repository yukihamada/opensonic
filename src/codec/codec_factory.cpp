/**
 * Soluna — Codec Factory Implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/codec/codec.h>
#include <soluna/codec/opus_wrapper.h>
#include <soluna/codec/aac_wrapper.h>
#include <soluna/codec/flac_wrapper.h>
#include <cstring>
#include <cmath>

namespace soluna {
namespace codec {

// PCM Encoder implementation
PcmEncoder::PcmEncoder(const CodecConfig& config) : config_(config) {}

EncodeResult PcmEncoder::encode(const float* input, size_t frame_count) {
    EncodeResult result;

    size_t samples = frame_count * config_.channels;

    switch (config_.bit_depth) {
        case 16: {
            result.data.resize(samples * 2);
            int16_t* out = reinterpret_cast<int16_t*>(result.data.data());
            for (size_t i = 0; i < samples; i++) {
                float clamped = std::max(-1.0f, std::min(1.0f, input[i]));
                out[i] = static_cast<int16_t>(clamped * 32767.0f);
            }
            break;
        }
        case 24: {
            // Pack into 32-bit container (S24_LE in 32-bit)
            result.data.resize(samples * 4);
            int32_t* out = reinterpret_cast<int32_t*>(result.data.data());
            for (size_t i = 0; i < samples; i++) {
                float clamped = std::max(-1.0f, std::min(1.0f, input[i]));
                out[i] = static_cast<int32_t>(clamped * 8388607.0f);  // 2^23 - 1
            }
            break;
        }
        case 32: {
            result.data.resize(samples * 4);
            memcpy(result.data.data(), input, samples * 4);
            break;
        }
        default:
            return result;
    }

    result.frames_encoded = frame_count;
    result.success = true;
    return result;
}

// PCM Decoder implementation
PcmDecoder::PcmDecoder(const CodecConfig& config) : config_(config) {}

DecodeResult PcmDecoder::decode(const uint8_t* input, size_t size) {
    DecodeResult result;

    size_t bytes_per_sample = config_.bit_depth / 8;
    if (config_.bit_depth == 24) bytes_per_sample = 4;  // Stored in 32-bit

    size_t samples = size / bytes_per_sample;
    size_t frames = samples / config_.channels;

    result.samples.resize(samples);

    switch (config_.bit_depth) {
        case 16: {
            const int16_t* in = reinterpret_cast<const int16_t*>(input);
            for (size_t i = 0; i < samples; i++) {
                result.samples[i] = static_cast<float>(in[i]) / 32768.0f;
            }
            break;
        }
        case 24: {
            const int32_t* in = reinterpret_cast<const int32_t*>(input);
            for (size_t i = 0; i < samples; i++) {
                result.samples[i] = static_cast<float>(in[i]) / 8388608.0f;
            }
            break;
        }
        case 32: {
            memcpy(result.samples.data(), input, size);
            break;
        }
        default:
            return result;
    }

    result.frames_decoded = frames;
    result.success = true;
    return result;
}

// Codec Factory
Result<std::unique_ptr<Encoder>> CodecFactory::create_encoder(const CodecConfig& config) {
    switch (config.type) {
        case CodecType::PCM:
            return std::unique_ptr<Encoder>(new PcmEncoder(config));

        case CodecType::Opus:
#ifdef SOLUNA_ENABLE_OPUS
            return std::unique_ptr<Encoder>(new OpusEncoderWrapper(config));
#else
            return Error(ErrorCode::CodecNotFound, "Opus codec not compiled in");
#endif

        case CodecType::AAC:
#ifdef SOLUNA_ENABLE_AAC
            return std::unique_ptr<Encoder>(new AacEncoder(config));
#else
            return Error(ErrorCode::CodecNotFound, "AAC codec not compiled in");
#endif

        case CodecType::FLAC:
#ifdef SOLUNA_ENABLE_FLAC
            return std::unique_ptr<Encoder>(new FlacEncoder(config));
#else
            return Error(ErrorCode::CodecNotFound, "FLAC codec not compiled in");
#endif
    }

    return Error(ErrorCode::CodecNotFound, "Unknown codec type");
}

Result<std::unique_ptr<Decoder>> CodecFactory::create_decoder(const CodecConfig& config) {
    switch (config.type) {
        case CodecType::PCM:
            return std::unique_ptr<Decoder>(new PcmDecoder(config));

        case CodecType::Opus:
#ifdef SOLUNA_ENABLE_OPUS
            return std::unique_ptr<Decoder>(new OpusDecoderWrapper(config));
#else
            return Error(ErrorCode::CodecNotFound, "Opus codec not compiled in");
#endif

        case CodecType::AAC:
#ifdef SOLUNA_ENABLE_AAC
            return std::unique_ptr<Decoder>(new AacDecoder(config));
#else
            return Error(ErrorCode::CodecNotFound, "AAC codec not compiled in");
#endif

        case CodecType::FLAC:
#ifdef SOLUNA_ENABLE_FLAC
            return std::unique_ptr<Decoder>(new FlacDecoder(config));
#else
            return Error(ErrorCode::CodecNotFound, "FLAC codec not compiled in");
#endif
    }

    return Error(ErrorCode::CodecNotFound, "Unknown codec type");
}

bool CodecFactory::is_available(CodecType type) {
    switch (type) {
        case CodecType::PCM:
            return true;
        case CodecType::Opus:
#ifdef SOLUNA_ENABLE_OPUS
            return true;
#else
            return false;
#endif
        case CodecType::AAC:
#ifdef SOLUNA_ENABLE_AAC
            return true;
#else
            return false;
#endif
        case CodecType::FLAC:
#ifdef SOLUNA_ENABLE_FLAC
            return true;
#else
            return false;
#endif
    }
    return false;
}

std::vector<CodecType> CodecFactory::available_codecs() {
    std::vector<CodecType> codecs;
    codecs.push_back(CodecType::PCM);

#ifdef SOLUNA_ENABLE_OPUS
    codecs.push_back(CodecType::Opus);
#endif

#ifdef SOLUNA_ENABLE_AAC
    codecs.push_back(CodecType::AAC);
#endif

#ifdef SOLUNA_ENABLE_FLAC
    codecs.push_back(CodecType::FLAC);
#endif

    return codecs;
}

} // namespace codec
} // namespace soluna
