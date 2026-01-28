/**
 * Soluna ESP32 — Audio Engine
 * Integrates I2S, RTP, FEC, and ring buffers into TX/RX pipelines
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "sol_common.h"

/** Initialize audio engine with given config */
sol_err_t sol_engine_init(const sol_config_t* config, sol_stats_t* stats);

/** Start audio processing (launches tasks) */
sol_err_t sol_engine_start(void);

/** Stop audio processing */
void sol_engine_stop(void);
