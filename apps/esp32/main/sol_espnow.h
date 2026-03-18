/**
 * Soluna ESP32 — ESP-NOW Hybrid Mesh (§16)
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

typedef void (*sol_espnow_rx_cb_t)(const uint8_t *data, size_t len);

esp_err_t sol_espnow_init(sol_espnow_rx_cb_t rx_cb);
void sol_espnow_deinit(void);
esp_err_t sol_espnow_broadcast(const uint8_t *data, size_t len);
