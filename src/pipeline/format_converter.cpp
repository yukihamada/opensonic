#include <soluna/pipeline/pipeline.h>
#include <algorithm>
#include <cmath>

namespace soluna::pipeline {

void float_to_s24(const float* src, int32_t* dst, size_t sample_count) {
    constexpr float scale = 8388607.0f; // 2^23 - 1
    for (size_t i = 0; i < sample_count; i++) {
        float clamped = std::max(-1.0f, std::min(1.0f, src[i]));
        dst[i] = static_cast<int32_t>(clamped * scale);
    }
}

void s24_to_float(const int32_t* src, float* dst, size_t sample_count) {
    constexpr float inv_scale = 1.0f / 8388607.0f;
    for (size_t i = 0; i < sample_count; i++) {
        // Sign-extend from 24 bits
        int32_t val = src[i];
        if (val & 0x00800000) {
            val |= static_cast<int32_t>(0xFF000000u);
        } else {
            val &= 0x00FFFFFF;
        }
        dst[i] = static_cast<float>(val) * inv_scale;
    }
}

} // namespace soluna::pipeline
