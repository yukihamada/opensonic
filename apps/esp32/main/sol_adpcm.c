/**
 * Soluna ESP32 — IMA-ADPCM Decoder + Raw First Strategy (§4.9)
 * Zero-latency start with raw PCM, then 4:1 ADPCM compression.
 * SPDX-License-Identifier: MIT
 */

#include "sol_adpcm.h"
#include <string.h>

static const int16_t step_table[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,
    50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,
    253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,
    1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,
    3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,
    10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,
    27086,29794,32767
};

static const int8_t index_table[16] = {
    -1,-1,-1,-1, 2, 4, 6, 8,
    -1,-1,-1,-1, 2, 4, 6, 8
};

void sol_adpcm_init(sol_adpcm_state_t *state) {
    state->valprev = 0;
    state->index = 40;  /* Mid-table for fast convergence */
}

void sol_adpcm_seed(sol_adpcm_state_t *state, int16_t last_raw_sample) {
    state->valprev = last_raw_sample;
    state->index = 40;
}

static inline int16_t decode_nibble(uint8_t nibble, sol_adpcm_state_t *s) {
    int step = step_table[s->index];
    int diff = step >> 3;
    if (nibble & 4) diff += step;
    if (nibble & 2) diff += (step >> 1);
    if (nibble & 1) diff += (step >> 2);
    if (nibble & 8) s->valprev -= diff;
    else            s->valprev += diff;
    if (s->valprev > 32767)  s->valprev = 32767;
    if (s->valprev < -32768) s->valprev = -32768;
    s->index += index_table[nibble & 0x0F];
    if (s->index < 0)  s->index = 0;
    if (s->index > 88) s->index = 88;
    return (int16_t)s->valprev;
}

size_t sol_adpcm_decode(sol_adpcm_state_t *state,
                        const uint8_t *adpcm_data, size_t adpcm_size,
                        int16_t *pcm_out, size_t max_samples) {
    if (adpcm_size < 4) return 0;

    /* Header: valprev(16) + index(8) + reserved(8) */
    if (state->valprev == 0 && state->index == 40) {
        state->valprev = (int16_t)(adpcm_data[0] | (adpcm_data[1] << 8));
        state->index = adpcm_data[2];
        if (state->index > 88) state->index = 88;
    }

    size_t num_samples = (adpcm_size - 4) * 2;
    if (num_samples > max_samples) num_samples = max_samples;

    for (size_t i = 0; i < num_samples; i++) {
        uint8_t nibble;
        if (i & 1) nibble = (adpcm_data[4 + i/2] >> 4) & 0x0F;
        else       nibble = adpcm_data[4 + i/2] & 0x0F;
        pcm_out[i] = decode_nibble(nibble, state);
    }
    return num_samples;
}
