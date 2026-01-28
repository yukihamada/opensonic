/**
 * Soluna — AAC Codec Wrapper Implementation
 *
 * Uses FDK-AAC library when available.
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/codec/aac_wrapper.h>

#ifdef SOLUNA_ENABLE_AAC

#include <fdk-aac/aacenc_lib.h>
#include <fdk-aac/aacdecoder_lib.h>
#include <cstring>

namespace soluna {
namespace codec {

struct AacEncoder::Impl {
    HANDLE_AACENCODER encoder = nullptr;
    std::vector<int16_t> input_buffer;
    std::vector<uint8_t> output_buffer;
    AACENC_InfoStruct info{};
};

AacEncoder::AacEncoder(const CodecConfig& config) : config_(config) {
    impl_ = std::make_unique<Impl>();

    // Open encoder
    if (aacEncOpen(&impl_->encoder, 0, config.channels) != AACENC_OK) {
        return;
    }

    // Configure
    aacEncoder_SetParam(impl_->encoder, AACENC_AOT,
                        config.aac_sbr ? AOT_SBR : AOT_AAC_LC);
    aacEncoder_SetParam(impl_->encoder, AACENC_SAMPLERATE, config.sample_rate);
    aacEncoder_SetParam(impl_->encoder, AACENC_CHANNELMODE,
                        config.channels == 1 ? MODE_1 : MODE_2);
    aacEncoder_SetParam(impl_->encoder, AACENC_BITRATE, config.bitrate);
    aacEncoder_SetParam(impl_->encoder, AACENC_TRANSMUX, TT_MP4_RAW);

    if (aacEncEncode(impl_->encoder, nullptr, nullptr, nullptr, nullptr) != AACENC_OK) {
        aacEncClose(&impl_->encoder);
        impl_->encoder = nullptr;
        return;
    }

    aacEncInfo(impl_->encoder, &impl_->info);

    impl_->input_buffer.resize(impl_->info.frameLength * config.channels);
    impl_->output_buffer.resize(impl_->info.maxOutBufBytes);
}

AacEncoder::~AacEncoder() {
    if (impl_ && impl_->encoder) {
        aacEncClose(&impl_->encoder);
    }
}

EncodeResult AacEncoder::encode(const float* input, size_t frame_count) {
    EncodeResult result;

    if (!impl_ || !impl_->encoder) {
        return result;
    }

    // Convert float to int16
    size_t samples = frame_count * config_.channels;
    impl_->input_buffer.resize(samples);
    for (size_t i = 0; i < samples; i++) {
        float clamped = std::max(-1.0f, std::min(1.0f, input[i]));
        impl_->input_buffer[i] = static_cast<int16_t>(clamped * 32767.0f);
    }

    // Setup buffers
    AACENC_BufDesc in_buf{};
    AACENC_BufDesc out_buf{};
    AACENC_InArgs in_args{};
    AACENC_OutArgs out_args{};

    void* in_ptr = impl_->input_buffer.data();
    int in_size = static_cast<int>(samples * sizeof(int16_t));
    int in_elem_size = sizeof(int16_t);
    int in_id = IN_AUDIO_DATA;

    in_buf.numBufs = 1;
    in_buf.bufs = &in_ptr;
    in_buf.bufferIdentifiers = &in_id;
    in_buf.bufSizes = &in_size;
    in_buf.bufElSizes = &in_elem_size;
    in_args.numInSamples = static_cast<int>(samples);

    void* out_ptr = impl_->output_buffer.data();
    int out_size = static_cast<int>(impl_->output_buffer.size());
    int out_elem_size = 1;
    int out_id = OUT_BITSTREAM_DATA;

    out_buf.numBufs = 1;
    out_buf.bufs = &out_ptr;
    out_buf.bufferIdentifiers = &out_id;
    out_buf.bufSizes = &out_size;
    out_buf.bufElSizes = &out_elem_size;

    // Encode
    AACENC_ERROR err = aacEncEncode(impl_->encoder, &in_buf, &out_buf, &in_args, &out_args);
    if (err != AACENC_OK && err != AACENC_ENCODE_EOF) {
        return result;
    }

    if (out_args.numOutBytes > 0) {
        result.data.assign(impl_->output_buffer.begin(),
                          impl_->output_buffer.begin() + out_args.numOutBytes);
    }

    result.frames_encoded = frame_count;
    result.success = true;
    return result;
}

EncodeResult AacEncoder::flush() {
    EncodeResult result;

    if (!impl_ || !impl_->encoder) {
        return result;
    }

    AACENC_BufDesc out_buf{};
    AACENC_InArgs in_args{};
    AACENC_OutArgs out_args{};

    void* out_ptr = impl_->output_buffer.data();
    int out_size = static_cast<int>(impl_->output_buffer.size());
    int out_elem_size = 1;
    int out_id = OUT_BITSTREAM_DATA;

    out_buf.numBufs = 1;
    out_buf.bufs = &out_ptr;
    out_buf.bufferIdentifiers = &out_id;
    out_buf.bufSizes = &out_size;
    out_buf.bufElSizes = &out_elem_size;
    in_args.numInSamples = -1;  // Signal EOF

    AACENC_ERROR err = aacEncEncode(impl_->encoder, nullptr, &out_buf, &in_args, &out_args);
    if (err == AACENC_ENCODE_EOF && out_args.numOutBytes > 0) {
        result.data.assign(impl_->output_buffer.begin(),
                          impl_->output_buffer.begin() + out_args.numOutBytes);
        result.success = true;
    }

    return result;
}

void AacEncoder::reset() {
    // FDK-AAC doesn't have a reset function, recreate encoder
    if (impl_ && impl_->encoder) {
        aacEncClose(&impl_->encoder);
        impl_->encoder = nullptr;
    }
    // Re-initialize would go here
}

size_t AacEncoder::get_delay() const {
    if (impl_) {
        return impl_->info.nDelay;
    }
    return 0;
}

// Decoder implementation
struct AacDecoder::Impl {
    HANDLE_AACDECODER decoder = nullptr;
    std::vector<int16_t> output_buffer;
};

AacDecoder::AacDecoder(const CodecConfig& config) : config_(config) {
    impl_ = std::make_unique<Impl>();

    impl_->decoder = aacDecoder_Open(TT_MP4_RAW, 1);
    if (!impl_->decoder) {
        return;
    }

    // Configure
    aacDecoder_SetParam(impl_->decoder, AAC_PCM_MAX_OUTPUT_CHANNELS, config.channels);
    aacDecoder_SetParam(impl_->decoder, AAC_PCM_MIN_OUTPUT_CHANNELS, config.channels);

    // Allocate output buffer (max frame size)
    impl_->output_buffer.resize(4096 * config.channels);
}

AacDecoder::~AacDecoder() {
    if (impl_ && impl_->decoder) {
        aacDecoder_Close(impl_->decoder);
    }
}

DecodeResult AacDecoder::decode(const uint8_t* input, size_t size) {
    DecodeResult result;

    if (!impl_ || !impl_->decoder) {
        return result;
    }

    // Feed data
    UINT bytes_valid = static_cast<UINT>(size);
    UCHAR* buf_ptr = const_cast<UCHAR*>(input);
    AAC_DECODER_ERROR err = aacDecoder_Fill(impl_->decoder, &buf_ptr, &size, &bytes_valid);
    if (err != AAC_DEC_OK) {
        return result;
    }

    // Decode
    err = aacDecoder_DecodeFrame(impl_->decoder,
                                  impl_->output_buffer.data(),
                                  static_cast<int>(impl_->output_buffer.size()),
                                  0);
    if (err != AAC_DEC_OK) {
        return result;
    }

    // Get stream info
    CStreamInfo* info = aacDecoder_GetStreamInfo(impl_->decoder);
    if (!info) {
        return result;
    }

    // Convert to float
    size_t samples = info->frameSize * info->numChannels;
    result.samples.resize(samples);
    for (size_t i = 0; i < samples; i++) {
        result.samples[i] = static_cast<float>(impl_->output_buffer[i]) / 32768.0f;
    }

    result.frames_decoded = info->frameSize;
    result.success = true;
    return result;
}

void AacDecoder::reset() {
    // Recreate decoder would go here
}

} // namespace codec
} // namespace soluna

#endif // SOLUNA_ENABLE_AAC
