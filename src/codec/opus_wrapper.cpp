/**
 * Opus Codec Wrapper Implementation
 *
 * When SOLUNA_HAS_OPUS is defined (libopus linked), uses real Opus.
 * Otherwise, provides a passthrough stub: encode copies raw PCM data,
 * decode copies it back. This allows the build to succeed without libopus.
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/codec/opus_wrapper.h>
#include <algorithm>
#include <cstring>
#include <cmath>

#ifdef SOLUNA_HAS_OPUS
#include <opus/opus.h>
#endif

namespace soluna::codec {

// ─────────────────────────── Encoder ───────────────────────────

struct OpusEncoder::Impl {
#ifdef SOLUNA_HAS_OPUS
    ::OpusEncoder* encoder = nullptr;
#endif
    std::vector<uint8_t> encode_buf;
};

OpusEncoder::OpusEncoder(const OpusEncoderConfig& config)
    : config_(config)
    , impl_(std::make_unique<Impl>())
{
#ifdef SOLUNA_HAS_OPUS
    int opus_app;
    switch (config.application) {
        case OpusApplication::VoIP:
            opus_app = OPUS_APPLICATION_VOIP; break;
        case OpusApplication::LowDelay:
            opus_app = OPUS_APPLICATION_RESTRICTED_LOWDELAY; break;
        default:
            opus_app = OPUS_APPLICATION_AUDIO; break;
    }

    int err;
    impl_->encoder = opus_encoder_create(
        config.sample_rate, config.channels, opus_app, &err);

    if (err == OPUS_OK && impl_->encoder) {
        opus_encoder_ctl(impl_->encoder, OPUS_SET_BITRATE(config.bitrate));
        opus_encoder_ctl(impl_->encoder, OPUS_SET_COMPLEXITY(config.complexity));
        if (config.use_fec) {
            opus_encoder_ctl(impl_->encoder, OPUS_SET_INBAND_FEC(1));
            opus_encoder_ctl(impl_->encoder, OPUS_SET_PACKET_LOSS_PERC(config.packet_loss_pct));
        }
        initialized_ = true;
    }
#else
    // Stub mode — always initialized
    initialized_ = true;
#endif

    // Pre-allocate encode buffer (max Opus output is ~4000 bytes for typical frames)
    impl_->encode_buf.resize(4096);
}

OpusEncoder::~OpusEncoder() {
#ifdef SOLUNA_HAS_OPUS
    if (impl_ && impl_->encoder) {
        opus_encoder_destroy(impl_->encoder);
    }
#endif
}

OpusEncodeResult OpusEncoder::encode(const float* input, size_t frame_count) {
    OpusEncodeResult result;

    if (!initialized_ || !input || frame_count == 0) {
        return result;
    }

#ifdef SOLUNA_HAS_OPUS
    int max_bytes = static_cast<int>(impl_->encode_buf.size());
    int encoded = opus_encode_float(
        impl_->encoder, input,
        static_cast<int>(frame_count),
        impl_->encode_buf.data(), max_bytes);

    if (encoded > 0) {
        result.data.assign(impl_->encode_buf.begin(),
                          impl_->encode_buf.begin() + encoded);
        result.frames_encoded = frame_count;
        result.success = true;
    }
#else
    // Stub: copy raw float data as "encoded" output
    size_t total_samples = frame_count * config_.channels;
    size_t data_size = total_samples * sizeof(float);
    result.data.resize(data_size);
    std::memcpy(result.data.data(), input, data_size);
    result.frames_encoded = frame_count;
    result.success = true;
#endif

    return result;
}

void OpusEncoder::set_bitrate(uint32_t bitrate) {
    config_.bitrate = bitrate;
#ifdef SOLUNA_HAS_OPUS
    if (impl_ && impl_->encoder) {
        opus_encoder_ctl(impl_->encoder, OPUS_SET_BITRATE(bitrate));
    }
#endif
}

void OpusEncoder::set_packet_loss_pct(int pct) {
    config_.packet_loss_pct = pct;
#ifdef SOLUNA_HAS_OPUS
    if (impl_ && impl_->encoder) {
        opus_encoder_ctl(impl_->encoder, OPUS_SET_PACKET_LOSS_PERC(pct));
    }
#endif
}

// ─────────────────────────── Decoder ───────────────────────────

struct OpusDecoder::Impl {
#ifdef SOLUNA_HAS_OPUS
    ::OpusDecoder* decoder = nullptr;
#endif
};

OpusDecoder::OpusDecoder(const OpusDecoderConfig& config)
    : config_(config)
    , impl_(std::make_unique<Impl>())
{
#ifdef SOLUNA_HAS_OPUS
    int err;
    impl_->decoder = opus_decoder_create(config.sample_rate, config.channels, &err);
    initialized_ = (err == OPUS_OK && impl_->decoder != nullptr);
#else
    initialized_ = true;
#endif
}

OpusDecoder::~OpusDecoder() {
#ifdef SOLUNA_HAS_OPUS
    if (impl_ && impl_->decoder) {
        opus_decoder_destroy(impl_->decoder);
    }
#endif
}

OpusDecodeResult OpusDecoder::decode(const void* input, size_t size,
                                      size_t frame_count) {
    OpusDecodeResult result;

    if (!initialized_ || !input || size == 0 || frame_count == 0) {
        return result;
    }

#ifdef SOLUNA_HAS_OPUS
    result.samples.resize(frame_count * config_.channels);
    int decoded = opus_decode_float(
        impl_->decoder,
        static_cast<const unsigned char*>(input),
        static_cast<opus_int32>(size),
        result.samples.data(),
        static_cast<int>(frame_count),
        0 /* no FEC decode */);

    if (decoded > 0) {
        result.frames_decoded = static_cast<size_t>(decoded);
        result.samples.resize(result.frames_decoded * config_.channels);
        result.success = true;
    }
#else
    // Stub: copy raw float data back
    size_t total_samples = frame_count * config_.channels;
    size_t expected_size = total_samples * sizeof(float);

    result.samples.resize(total_samples);
    size_t copy_size = std::min(size, expected_size);
    std::memcpy(result.samples.data(), input, copy_size);
    result.frames_decoded = frame_count;
    result.success = true;
#endif

    return result;
}

OpusDecodeResult OpusDecoder::decode_fec(const void* input, size_t size,
                                          size_t frame_count) {
    OpusDecodeResult result;

    if (!initialized_ || !input || size == 0 || frame_count == 0) {
        return result;
    }

#ifdef SOLUNA_HAS_OPUS
    result.samples.resize(frame_count * config_.channels);
    int decoded = opus_decode_float(
        impl_->decoder,
        static_cast<const unsigned char*>(input),
        static_cast<opus_int32>(size),
        result.samples.data(),
        static_cast<int>(frame_count),
        1 /* FEC decode: recover previous frame from this packet's FEC data */);

    if (decoded > 0) {
        result.frames_decoded = static_cast<size_t>(decoded);
        result.samples.resize(result.frames_decoded * config_.channels);
        result.success = true;
    }
#else
    // Stub: copy raw float data back (no FEC in stub mode)
    size_t total_samples = frame_count * config_.channels;
    size_t expected_size = total_samples * sizeof(float);
    result.samples.resize(total_samples);
    size_t copy_size = std::min(size, expected_size);
    std::memcpy(result.samples.data(), input, copy_size);
    result.frames_decoded = frame_count;
    result.success = true;
#endif

    return result;
}

OpusDecodeResult OpusDecoder::decode_plc(size_t frame_count) {
    OpusDecodeResult result;

    if (!initialized_ || frame_count == 0) {
        return result;
    }

#ifdef SOLUNA_HAS_OPUS
    result.samples.resize(frame_count * config_.channels);
    int decoded = opus_decode_float(
        impl_->decoder,
        nullptr, 0, // NULL input = PLC
        result.samples.data(),
        static_cast<int>(frame_count),
        0);

    if (decoded > 0) {
        result.frames_decoded = static_cast<size_t>(decoded);
        result.samples.resize(result.frames_decoded * config_.channels);
        result.success = true;
        result.plc_used = true;
    }
#else
    // Stub: generate silence for PLC
    size_t total_samples = frame_count * config_.channels;
    result.samples.resize(total_samples, 0.0f);
    result.frames_decoded = frame_count;
    result.success = true;
    result.plc_used = true;
#endif

    return result;
}

} // namespace soluna::codec
