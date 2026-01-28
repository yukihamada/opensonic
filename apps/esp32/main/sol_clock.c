/**
 * Soluna ESP32 — High-Resolution Clock
 * Uses esp_timer (64-bit microsecond counter)
 * SPDX-License-Identifier: MIT
 */

#include "sol_clock.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

int64_t sol_clock_now_ns(void) {
    return esp_timer_get_time() * 1000LL;
}

int64_t sol_clock_now_us(void) {
    return esp_timer_get_time();
}

void sol_clock_sleep_ns(int64_t ns) {
    if (ns <= 0) return;

    if (ns < 1000000LL) {
        /* < 1ms: spin-wait for precision */
        int64_t target = sol_clock_now_ns() + ns;
        while (sol_clock_now_ns() < target) {
            /* Spin */
        }
    } else {
        /* >= 1ms: use FreeRTOS delay */
        uint32_t ms = (uint32_t)(ns / 1000000LL);
        if (ms < 1) ms = 1;
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

void sol_clock_sleep_until_ns(int64_t target_ns) {
    int64_t now = sol_clock_now_ns();
    int64_t diff = target_ns - now;
    if (diff > 0) {
        sol_clock_sleep_ns(diff);
    }
}
