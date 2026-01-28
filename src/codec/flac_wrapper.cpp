/**
 * Soluna — FLAC Codec Wrapper Implementation
 *
 * Uses libFLAC when available.
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/codec/flac_wrapper.h>

#ifdef SOLUNA_ENABLE_FLAC

#include <FLAC/stream_encoder.h>
#include <FLAC/stream_decoder.h>
#include <cstring>

namespace soluna {
namespace codec {

// Encoder
struct FlacEncoder::Impl {
    FLAC__StreamEncoder* encoder = nullptr;
    std::vector<FLAC__int32> input_buffer;
    std::vector<uint8_t> output_buffer;
    size_t output_pos = 0;

    static FLAC__StreamEncoderWriteStatus write_callback(
        const FLAC__StreamEncoder* encoder,
        const FLAC__byte buffer[],
        size_t bytes,
        uint32_t samples,
        uint32_t current_frame,
        void* client_data) {

        (void)encoder;
        (void)samples;
        (void)current_frame;

        Impl* impl = static_cast<Impl*>(client_data);
        if (impl->output_pos + bytes > impl->output_buffer.size()) {
            impl->output_buffer.resize(impl->output_pos + bytes + 4096);
        }
        memcpy(impl->output_buffer.data() + impl->output_pos, buffer, bytes);
        impl->output_pos += bytes;
        return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
    }
};

FlacEncoder::FlacEncoder(const CodecConfig& config) : config_(config) {
    impl_ = std::make_unique<Impl>();

    impl_->encoder = FLAC__stream_encoder_new();
    if (!impl_->encoder) {
        return;
    }

    FLAC__stream_encoder_set_channels(impl_->encoder, config.channels);
    FLAC__stream_encoder_set_bits_per_sample(impl_->encoder, config.bit_depth);
    FLAC__stream_encoder_set_sample_rate(impl_->encoder, config.sample_rate);
    FLAC__stream_encoder_set_compression_level(impl_->encoder, config.flac_compression);

    // Use stream encoding (to memory)
    impl_->output_buffer.resize(65536);

    FLAC__StreamEncoderInitStatus status = FLAC__stream_encoder_init_stream(
        impl_->encoder,
        &Impl::write_callback,
        nullptr,  // seek
        nullptr,  // tell
        nullptr,  // metadata
        impl_.get());

    if (status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        FLAC__stream_encoder_delete(impl_->encoder);
        impl_->encoder = nullptr;
    }
}

FlacEncoder::~FlacEncoder() {
    if (impl_ && impl_->encoder) {
        FLAC__stream_encoder_finish(impl_->encoder);
        FLAC__stream_encoder_delete(impl_->encoder);
    }
}

EncodeResult FlacEncoder::encode(const float* input, size_t frame_count) {
    EncodeResult result;

    if (!impl_ || !impl_->encoder) {
        return result;
    }

    // Convert float to int32
    size_t samples = frame_count * config_.channels;
    impl_->input_buffer.resize(samples);

    float scale = static_cast<float>((1 << (config_.bit_depth - 1)) - 1);
    for (size_t i = 0; i < samples; i++) {
        float clamped = std::max(-1.0f, std::min(1.0f, input[i]));
        impl_->input_buffer[i] = static_cast<FLAC__int32>(clamped * scale);
    }

    // Reset output position
    impl_->output_pos = 0;

    // Encode
    FLAC__bool ok = FLAC__stream_encoder_process_interleaved(
        impl_->encoder,
        impl_->input_buffer.data(),
        static_cast<uint32_t>(frame_count));

    if (!ok) {
        return result;
    }

    // Copy output
    if (impl_->output_pos > 0) {
        result.data.assign(impl_->output_buffer.begin(),
                          impl_->output_buffer.begin() + impl_->output_pos);
    }

    result.frames_encoded = frame_count;
    result.success = true;
    return result;
}

EncodeResult FlacEncoder::flush() {
    EncodeResult result;

    if (!impl_ || !impl_->encoder) {
        return result;
    }

    impl_->output_pos = 0;
    FLAC__stream_encoder_finish(impl_->encoder);

    if (impl_->output_pos > 0) {
        result.data.assign(impl_->output_buffer.begin(),
                          impl_->output_buffer.begin() + impl_->output_pos);
        result.success = true;
    }

    return result;
}

void FlacEncoder::reset() {
    // FLAC encoder doesn't support reset, would need recreation
}

// Decoder
struct FlacDecoder::Impl {
    FLAC__StreamDecoder* decoder = nullptr;
    std::vector<float> output_samples;
    uint32_t channels = 2;
    const uint8_t* input_data = nullptr;
    size_t input_size = 0;
    size_t input_pos = 0;

    static FLAC__StreamDecoderReadStatus read_callback(
        const FLAC__StreamDecoder* decoder,
        FLAC__byte buffer[],
        size_t* bytes,
        void* client_data) {

        (void)decoder;
        Impl* impl = static_cast<Impl*>(client_data);

        size_t remaining = impl->input_size - impl->input_pos;
        if (remaining == 0) {
            *bytes = 0;
            return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
        }

        size_t to_read = std::min(*bytes, remaining);
        memcpy(buffer, impl->input_data + impl->input_pos, to_read);
        impl->input_pos += to_read;
        *bytes = to_read;

        return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }

    static FLAC__StreamDecoderWriteStatus write_callback(
        const FLAC__StreamDecoder* decoder,
        const FLAC__Frame* frame,
        const FLAC__int32* const buffer[],
        void* client_data) {

        (void)decoder;
        Impl* impl = static_cast<Impl*>(client_data);

        size_t samples = frame->header.blocksize * frame->header.channels;
        size_t start = impl->output_samples.size();
        impl->output_samples.resize(start + samples);

        float scale = 1.0f / static_cast<float>(1 << (frame->header.bits_per_sample - 1));

        // Interleave channels
        for (uint32_t i = 0; i < frame->header.blocksize; i++) {
            for (uint32_t ch = 0; ch < frame->header.channels; ch++) {
                impl->output_samples[start + i * frame->header.channels + ch] =
                    static_cast<float>(buffer[ch][i]) * scale;
            }
        }

        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }

    static void error_callback(
        const FLAC__StreamDecoder* decoder,
        FLAC__StreamDecoderErrorStatus status,
        void* client_data) {
        (void)decoder;
        (void)status;
        (void)client_data;
    }
};

FlacDecoder::FlacDecoder(const CodecConfig& config) : config_(config) {
    impl_ = std::make_unique<Impl>();
    impl_->channels = config.channels;

    impl_->decoder = FLAC__stream_decoder_new();
    if (!impl_->decoder) {
        return;
    }

    FLAC__StreamDecoderInitStatus status = FLAC__stream_decoder_init_stream(
        impl_->decoder,
        &Impl::read_callback,
        nullptr,  // seek
        nullptr,  // tell
        nullptr,  // length
        nullptr,  // eof
        &Impl::write_callback,
        nullptr,  // metadata
        &Impl::error_callback,
        impl_.get());

    if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        FLAC__stream_decoder_delete(impl_->decoder);
        impl_->decoder = nullptr;
    }
}

FlacDecoder::~FlacDecoder() {
    if (impl_ && impl_->decoder) {
        FLAC__stream_decoder_finish(impl_->decoder);
        FLAC__stream_decoder_delete(impl_->decoder);
    }
}

DecodeResult FlacDecoder::decode(const uint8_t* input, size_t size) {
    DecodeResult result;

    if (!impl_ || !impl_->decoder) {
        return result;
    }

    // Setup input
    impl_->input_data = input;
    impl_->input_size = size;
    impl_->input_pos = 0;
    impl_->output_samples.clear();

    // Decode
    FLAC__bool ok = FLAC__stream_decoder_process_single(impl_->decoder);
    if (!ok) {
        return result;
    }

    result.samples = std::move(impl_->output_samples);
    result.frames_decoded = result.samples.size() / config_.channels;
    result.success = true;
    return result;
}

void FlacDecoder::reset() {
    if (impl_ && impl_->decoder) {
        FLAC__stream_decoder_reset(impl_->decoder);
    }
}

} // namespace codec
} // namespace soluna

#endif // SOLUNA_ENABLE_FLAC
