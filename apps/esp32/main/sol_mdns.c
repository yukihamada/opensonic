/**
 * Soluna ESP32 — mDNS Implementation
 * Advertises _soluna._udp on the local network
 * SPDX-License-Identifier: MIT
 */

#include "sol_mdns.h"

#include "mdns.h"
#include "esp_log.h"

#include <stdio.h>

static const char* TAG = "sol_mdns";

sol_err_t sol_mdns_init(const char* hostname, const char* device_name,
                         uint8_t channels, sol_mode_t mode) {
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return SOL_ERR_INIT;
    }

    mdns_hostname_set(hostname);
    mdns_instance_name_set(device_name);

    /* Announce _soluna._udp service */
    char ch_str[4];
    snprintf(ch_str, sizeof(ch_str), "%u", channels);

    const char* mode_str = (mode == SOL_MODE_TX) ? "tx" :
                            (mode == SOL_MODE_RX) ? "rx" : "txrx";

    mdns_txt_item_t txt[] = {
        {"version", "0.1.0"},
        {"channels", ch_str},
        {"mode", mode_str},
        {"rate", "48000"},
        {"platform", "esp32"},
    };

    err = mdns_service_add(device_name, "_soluna", "_udp",
                            SOL_PORT_CONTROL, txt, sizeof(txt) / sizeof(txt[0]));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_service_add failed: %s", esp_err_to_name(err));
        return SOL_ERR_INIT;
    }

    ESP_LOGI(TAG, "mDNS: %s._soluna._udp (%s, %uch)", hostname, mode_str, channels);
    return SOL_OK;
}

sol_err_t sol_mdns_update(const char* device_name, uint8_t channels,
                           sol_mode_t mode) {
    char ch_str[4];
    snprintf(ch_str, sizeof(ch_str), "%u", channels);

    const char* mode_str = (mode == SOL_MODE_TX) ? "tx" :
                            (mode == SOL_MODE_RX) ? "rx" : "txrx";

    mdns_txt_item_t txt[] = {
        {"version", "0.1.0"},
        {"channels", ch_str},
        {"mode", mode_str},
        {"rate", "48000"},
        {"platform", "esp32"},
    };

    mdns_service_txt_set("_soluna", "_udp", txt, sizeof(txt) / sizeof(txt[0]));
    return SOL_OK;
}

void sol_mdns_stop(void) {
    mdns_service_remove_all();
    mdns_free();
    ESP_LOGI(TAG, "mDNS stopped");
}
