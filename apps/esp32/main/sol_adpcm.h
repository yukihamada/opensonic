/**
 * Soluna ESP32 — IMA-ADPCM Decoder (§4.9)
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int32_t valprev;
    int32_t index;
} sol_adpcm_state_t;

void sol_adpcm_init(sol_adpcm_state_t *state);
void sol_adpcm_seed(sol_adpcm_state_t *state, int16_t last_raw_sample);
size_t sol_adpcm_decode(sol_adpcm_state_t *state,
                        const uint8_t *adpcm_data, size_t adpcm_size,
                        int16_t *pcm_out, size_t max_samples);
