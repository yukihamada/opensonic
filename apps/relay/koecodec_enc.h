// koecodec_enc.h — KoeCodec PT=117 encoder for soluna-radio
// MDCT (320-sample frame, 160-sample hop) + Residual Product VQ @ 16kHz
// Trained on music. ~13.6 kbps for 2-frame (20ms) packets.
//
// Usage:
//   KoeEncoder enc;
//   enc.init();
//   float mono16k[160];
//   uint8_t out[256];
//   size_t n = enc.encode_frame(mono16k, out, sizeof(out));  // 1 frame → packet
//
#pragma once
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdlib>

// Forward declare — included AFTER this header
#include "codebooks_music.h"

static constexpr int KOE_HOP_SIZE  = 160;
static constexpr int KOE_FRAME_SIZE= 320;
static constexpr int KOE_STAGES    = KOE_N_STAGES;  // 3
static constexpr int KOE_SUBS      = KOE_N_SUB;     // 4
static constexpr int KOE_CBSIZE    = KOE_CB_SIZE;    // 256
static constexpr int KOE_SDIM      = KOE_SUB_DIM;   // 40
static constexpr int KOE_DIM       = KOE_SUBS * KOE_SDIM; // 160
static constexpr int KOE_GBITS     = 6;   // gain quantization bits
static constexpr int KOE_GLEVELS   = (1 << KOE_GBITS); // 64
static constexpr float KOE_GLOG_MIN= -20.0f;
static constexpr float KOE_GLOG_MAX=  10.0f;

struct KoeEncoder {
    float*  basis;     // KOE_FRAME_SIZE * KOE_HOP_SIZE floats (200KB)
    float   window[KOE_FRAME_SIZE];
    float   prev_hop[KOE_HOP_SIZE]; // overlap from last frame
    // Float codebooks: [stage][sub][entry * sub_dim]
    float** cbs;       // cbs[s*KOE_SUBS+m] → float[KOE_CBSIZE * KOE_SDIM]

    // Returns false on allocation failure
    bool init() {
        basis = (float*)malloc(KOE_FRAME_SIZE * KOE_HOP_SIZE * sizeof(float));
        if (!basis) return false;

        // Sine window
        for (int n = 0; n < KOE_FRAME_SIZE; n++)
            window[n] = sinf((float)M_PI * (n + 0.5f) / KOE_FRAME_SIZE);

        // MDCT basis: cos(pi/N * (n+0.5+N/2) * (k+0.5)), N=HOP
        float invN = (float)M_PI / KOE_HOP_SIZE;
        float scale = 2.0f / KOE_HOP_SIZE;
        for (int n = 0; n < KOE_FRAME_SIZE; n++) {
            float base = invN * (n + 0.5f + KOE_HOP_SIZE * 0.5f);
            for (int k = 0; k < KOE_HOP_SIZE; k++)
                basis[n * KOE_HOP_SIZE + k] = scale * cosf(base * (k + 0.5f));
        }

        memset(prev_hop, 0, sizeof(prev_hop));

        // Dequantize codebooks from Q15 int16 to float
        cbs = (float**)malloc(KOE_STAGES * KOE_SUBS * sizeof(float*));
        if (!cbs) return false;

        // Scale factors per stage/sub (from codebooks_music.h)
        static const float koe_scales[KOE_STAGES][KOE_SUBS] = {
            {koe_scale_0_0, koe_scale_0_1, koe_scale_0_2, koe_scale_0_3},
            {koe_scale_1_0, koe_scale_1_1, koe_scale_1_2, koe_scale_1_3},
            {koe_scale_2_0, koe_scale_2_1, koe_scale_2_2, koe_scale_2_3},
        };
        // Codebook int16 pointers (from codebooks_music.h)
        static const int16_t* koe_cbs_i16[KOE_STAGES][KOE_SUBS] = {
            {&koe_cb_0_0[0][0], &koe_cb_0_1[0][0], &koe_cb_0_2[0][0], &koe_cb_0_3[0][0]},
            {&koe_cb_1_0[0][0], &koe_cb_1_1[0][0], &koe_cb_1_2[0][0], &koe_cb_1_3[0][0]},
            {&koe_cb_2_0[0][0], &koe_cb_2_1[0][0], &koe_cb_2_2[0][0], &koe_cb_2_3[0][0]},
        };

        for (int s = 0; s < KOE_STAGES; s++) {
            for (int m = 0; m < KOE_SUBS; m++) {
                int idx = s * KOE_SUBS + m;
                cbs[idx] = (float*)malloc(KOE_CBSIZE * KOE_SDIM * sizeof(float));
                if (!cbs[idx]) return false;
                float sc = koe_scales[s][m] / 32767.0f;
                const int16_t* src = koe_cbs_i16[s][m];
                for (int i = 0; i < KOE_CBSIZE * KOE_SDIM; i++)
                    cbs[idx][i] = src[i] * sc;
            }
        }
        return true;
    }

    void destroy() {
        free(basis); basis = nullptr;
        if (cbs) {
            for (int i = 0; i < KOE_STAGES * KOE_SUBS; i++) free(cbs[i]);
            free(cbs); cbs = nullptr;
        }
    }

    // MDCT of one 320-sample frame → 160 coefficients
    void mdct(const float* frame, float* coeffs) const {
        for (int k = 0; k < KOE_HOP_SIZE; k++) {
            float sum = 0.0f;
            for (int n = 0; n < KOE_FRAME_SIZE; n++)
                sum += frame[n] * window[n] * basis[n * KOE_HOP_SIZE + k];
            coeffs[k] = sum;
        }
    }

    // Encode a single 160-sample 16kHz mono frame into the packet bitstream.
    // Call repeatedly; frame[0..159] = new samples.
    // out_bits: [stage][sub] shape indices written to running bitstream.
    // Returns bytes produced (only flushes when enough bits).
    //
    // Simpler API: encode N complete frames at once.
    // samples: float[n_frames * KOE_HOP_SIZE] (16kHz mono)
    // out: output buffer
    // Returns bytes written.
    size_t encode(const float* samples, int n_frames, uint8_t* out, size_t out_max) {
        if (n_frames <= 0) return 0;

        // Bitstream state
        size_t bits = 0;
        auto put = [&](uint32_t val, int nbits) {
            for (int i = nbits - 1; i >= 0; i--) {
                size_t bi = bits / 8;
                int    bo = (int)(bits % 8);
                if (bi >= out_max) return;
                if (bo == 0) out[bi] = 0;
                if ((val >> i) & 1) out[bi] |= (uint8_t)(0x80u >> bo);
                bits++;
            }
        };

        // Header
        put((uint32_t)n_frames,  16);
        put((uint32_t)KOE_STAGES, 4);
        put((uint32_t)KOE_SUBS,   4);
        put((uint32_t)KOE_BITS_PER_IDX, 4); // 8
        put((uint32_t)KOE_GBITS,  4);        // 6

        for (int f = 0; f < n_frames; f++) {
            // Build 320-sample frame: prev_hop + new hop
            float frame[KOE_FRAME_SIZE];
            memcpy(frame,               prev_hop,              KOE_HOP_SIZE * sizeof(float));
            memcpy(frame + KOE_HOP_SIZE, samples + f * KOE_HOP_SIZE, KOE_HOP_SIZE * sizeof(float));

            // MDCT → 160 coefficients
            float coeffs[KOE_HOP_SIZE];
            mdct(frame, coeffs);

            // Gain-shape decomposition
            float gains[KOE_SUBS], sub[KOE_SUBS][KOE_SDIM];
            for (int m = 0; m < KOE_SUBS; m++) {
                const float* c = coeffs + m * KOE_SDIM;
                memcpy(sub[m], c, KOE_SDIM * sizeof(float));
                float ss = 1e-20f;
                for (int d = 0; d < KOE_SDIM; d++) ss += c[d] * c[d];
                gains[m] = sqrtf(ss / KOE_SDIM);
            }

            // Quantize gains
            uint32_t gcodes[KOE_SUBS];
            float qgains[KOE_SUBS];
            for (int m = 0; m < KOE_SUBS; m++) {
                float lg = log2f(gains[m] + 1e-10f);
                int code = (int)((lg - KOE_GLOG_MIN) / (KOE_GLOG_MAX - KOE_GLOG_MIN)
                                 * (KOE_GLEVELS - 1) + 0.5f);
                if (code < 0) code = 0;
                if (code >= KOE_GLEVELS) code = KOE_GLEVELS - 1;
                gcodes[m] = (uint32_t)code;
                qgains[m] = powf(2.0f, (float)code / (KOE_GLEVELS - 1)
                                 * (KOE_GLOG_MAX - KOE_GLOG_MIN) + KOE_GLOG_MIN);
            }

            // Normalize by dequantized gain
            float norm[KOE_SUBS][KOE_SDIM];
            for (int m = 0; m < KOE_SUBS; m++) {
                float g = qgains[m];
                for (int d = 0; d < KOE_SDIM; d++) norm[m][d] = sub[m][d] / g;
            }

            // Residual VQ across stages
            float residual[KOE_SUBS][KOE_SDIM];
            for (int m = 0; m < KOE_SUBS; m++) memcpy(residual[m], norm[m], KOE_SDIM * sizeof(float));

            float cum[KOE_SUBS][KOE_SDIM] = {};
            uint32_t sidx[KOE_STAGES][KOE_SUBS];

            for (int s = 0; s < KOE_STAGES; s++) {
                for (int m = 0; m < KOE_SUBS; m++) {
                    const float* cb = cbs[s * KOE_SUBS + m];
                    float best_d = 1e30f;
                    uint32_t best_i = 0;
                    for (int i = 0; i < KOE_CBSIZE; i++) {
                        const float* e = cb + i * KOE_SDIM;
                        float d = 0.0f;
                        for (int k2 = 0; k2 < KOE_SDIM; k2++) {
                            float diff = residual[m][k2] - e[k2];
                            d += diff * diff;
                        }
                        if (d < best_d) { best_d = d; best_i = (uint32_t)i; }
                    }
                    sidx[s][m] = best_i;
                    const float* best = cb + best_i * KOE_SDIM;
                    for (int k2 = 0; k2 < KOE_SDIM; k2++) {
                        cum[m][k2] += best[k2];
                        residual[m][k2] = norm[m][k2] - cum[m][k2];
                    }
                }
            }

            // Write gains then shape indices
            for (int m = 0; m < KOE_SUBS; m++) put(gcodes[m], KOE_GBITS);
            for (int s = 0; s < KOE_STAGES; s++)
                for (int m = 0; m < KOE_SUBS; m++)
                    put(sidx[s][m], KOE_BITS_PER_IDX);

            // Slide overlap
            memcpy(prev_hop, samples + f * KOE_HOP_SIZE, KOE_HOP_SIZE * sizeof(float));
        }

        return (bits + 7) / 8;
    }
};

// ── Downsampler: 48kHz stereo int32 (24-bit in 32) → 16kHz mono float ──
// Simple: average 3 stereo pairs → 1 mono sample
static inline void downsample_48k_to_16k(
    const int32_t* pcm48_stereo, size_t n_stereo_frames, // frames (L+R pairs)
    float* out_mono16k, size_t& out_count)
{
    out_count = 0;
    // Each 16kHz sample = average of 3 consecutive 48kHz stereo pairs (downmixed to mono)
    static float accum = 0.0f;
    static int   accum_count = 0;
    const float scale = 1.0f / (8388608.0f * 2.0f * 3.0f); // /2 for stereo, /3 for downsample

    for (size_t i = 0; i < n_stereo_frames; i++) {
        // Stereo → mono, then accumulate for 3x decimation
        accum += (float)(pcm48_stereo[i*2] + pcm48_stereo[i*2+1]);
        accum_count++;
        if (accum_count == 3) {
            out_mono16k[out_count++] = accum * scale;
            accum = 0.0f;
            accum_count = 0;
        }
    }
}
