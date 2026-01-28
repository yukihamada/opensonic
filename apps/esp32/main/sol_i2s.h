/**
 * Soluna ESP32 — I2S + DMA Audio Driver
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "sol_common.h"

typedef struct {
    uint8_t bck_pin;
    uint8_t ws_pin;
    uint8_t data_out_pin;
    uint8_t data_in_pin;
    uint8_t channels;
    uint32_t sample_rate;
    uint32_t dma_buf_count;
    uint32_t dma_buf_len;  /* frames per DMA buffer */
} sol_i2s_config_t;

/**
 * Initialize I2S peripheral with DMA.
 * Audio task runs on SOL_CORE_AUDIO.
 */
sol_err_t sol_i2s_init(const sol_i2s_config_t* cfg);

/** Start I2S TX (reads from ring buffer) */
sol_err_t sol_i2s_start_tx(sol_ring_t* ring);

/** Start I2S RX (writes to ring buffer) */
sol_err_t sol_i2s_start_rx(sol_ring_t* ring);

/** Stop I2S */
void sol_i2s_stop(void);

/** Get DMA underrun/overrun counts */
uint32_t sol_i2s_underruns(void);
uint32_t sol_i2s_overruns(void);
