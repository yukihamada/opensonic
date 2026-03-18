#include <inttypes.h>
/**
 * Soluna ESP32 — VAD Deep Sleep + Modem Control (§19)
 * Battery optimization: Wi-Fi modem sleep during silence.
 * SPDX-License-Identifier: MIT
 */

#include "sol_vad_sleep.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sol_vad";

static uint32_t s_last_packet_ms = 0;
static bool s_modem_sleeping = false;
static const uint32_t SILENCE_THRESHOLD_MS = 100;

void sol_vad_init(void) {
    s_last_packet_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    s_modem_sleeping = false;
    ESP_LOGI(TAG, "VAD sleep controller initialized (threshold=%" PRIu32 "ms)",
             SILENCE_THRESHOLD_MS);
}

void sol_vad_on_packet(void) {
    s_last_packet_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    /* Wake modem if sleeping */
    if (s_modem_sleeping) {
        esp_wifi_set_ps(WIFI_PS_NONE);  /* Full power */
        s_modem_sleeping = false;
        ESP_LOGI(TAG, "Modem WAKE (packet received)");
    }
}

void sol_vad_tick(void) {
    if (s_modem_sleeping) return;

    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t elapsed = now_ms - s_last_packet_ms;

    if (elapsed >= SILENCE_THRESHOLD_MS) {
        /* No packets for 100ms → enter modem sleep (~20mA vs ~100mA) */
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        s_modem_sleeping = true;
        ESP_LOGI(TAG, "Modem SLEEP (silence %" PRIu32 "ms)", elapsed);
    }
}

bool sol_vad_is_sleeping(void) {
    return s_modem_sleeping;
}
