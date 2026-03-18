/**
 * Soluna ESP32 — ESP-NOW Hybrid Mesh Bridge (§16)
 * Self-healing P2P audio relay for Wi-Fi dead zones.
 * SPDX-License-Identifier: MIT
 */

#include "sol_espnow.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "sol_espnow";
static const uint8_t BROADCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static sol_espnow_rx_cb_t s_rx_callback = NULL;
static bool s_initialized = false;

/* ESP-NOW receive callback */
static void espnow_recv_cb(const esp_now_recv_info_t *info,
                           const uint8_t *data, int len) {
    if (s_rx_callback && len > 0) {
        s_rx_callback(data, (size_t)len);
    }
}

esp_err_t sol_espnow_init(sol_espnow_rx_cb_t rx_cb) {
    if (s_initialized) return ESP_OK;

    esp_err_t ret = esp_now_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register receive callback */
    s_rx_callback = rx_cb;
    esp_now_register_recv_cb(espnow_recv_cb);

    /* Add broadcast peer */
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, BROADCAST_MAC, 6);
    peer.channel = 0;  /* Use current channel */
    peer.encrypt = false;
    ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add_peer failed: %s", esp_err_to_name(ret));
    }

    s_initialized = true;
    ESP_LOGI(TAG, "ESP-NOW mesh bridge initialized");
    return ESP_OK;
}

void sol_espnow_deinit(void) {
    if (!s_initialized) return;
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    s_initialized = false;
}

esp_err_t sol_espnow_broadcast(const uint8_t *data, size_t len) {
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (len > 250) return ESP_ERR_INVALID_SIZE;  /* ESP-NOW max payload */

    return esp_now_send(BROADCAST_MAC, data, (size_t)len);
}
