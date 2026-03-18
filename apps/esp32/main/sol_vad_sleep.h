/**
 * Soluna ESP32 — VAD Deep Sleep (§19)
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>

void sol_vad_init(void);
void sol_vad_on_packet(void);   /* Call on every received audio packet */
void sol_vad_tick(void);        /* Call from main loop (~10Hz) */
bool sol_vad_is_sleeping(void);
