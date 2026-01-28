/**
 * Soluna ESP32 — High-Resolution Clock (esp_timer)
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "sol_common.h"

/** Get monotonic time in nanoseconds */
int64_t sol_clock_now_ns(void);

/** Get monotonic time in microseconds */
int64_t sol_clock_now_us(void);

/** Sleep for specified nanoseconds (busy-wait for < 1ms, vTaskDelay otherwise) */
void sol_clock_sleep_ns(int64_t ns);

/** Sleep until absolute monotonic time (nanoseconds) */
void sol_clock_sleep_until_ns(int64_t target_ns);
