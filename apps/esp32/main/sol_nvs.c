/**
 * Soluna ESP32 — NVS Configuration Storage Implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include "sol_nvs.h"

#ifdef ESP_PLATFORM
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_log.h>
#endif

#include <string.h>

static const char* TAG = "sol_nvs";

void sol_nvs_get_defaults(sol_config_t* config) {
    memset(config, 0, sizeof(*config));
    strncpy(config->device_name, "soluna-esp32", sizeof(config->device_name) - 1);
    config->mode = SOL_MODE_RX;
    strncpy(config->wifi_ssid, "", sizeof(config->wifi_ssid) - 1);
    strncpy(config->wifi_pass, "", sizeof(config->wifi_pass) - 1);
    config->channels = 2;
    config->rtp_port = SOL_PORT_RTP;
    config->ssrc = SOL_SSRC_DEFAULT;
    config->fec_enabled = 1;
    config->target_latency_ms = 20.0f;
}

#ifdef ESP_PLATFORM

int sol_nvs_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition corrupt, erasing...");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    return (err == ESP_OK) ? 0 : -1;
}

int sol_nvs_load(sol_config_t* config) {
    nvs_handle_t handle;
    esp_err_t err;

    // Start with defaults
    sol_nvs_get_defaults(config);

    err = nvs_open(SOL_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved config, using defaults");
        return 0;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return -1;
    }

    size_t len;

    // Load device name
    len = sizeof(config->device_name);
    nvs_get_str(handle, SOL_NVS_KEY_DEVICE_NAME, config->device_name, &len);

    // Load mode
    uint8_t mode = 0;
    if (nvs_get_u8(handle, SOL_NVS_KEY_MODE, &mode) == ESP_OK) {
        config->mode = (sol_mode_t)mode;
    }

    // Load WiFi credentials
    len = sizeof(config->wifi_ssid);
    nvs_get_str(handle, SOL_NVS_KEY_WIFI_SSID, config->wifi_ssid, &len);

    len = sizeof(config->wifi_pass);
    nvs_get_str(handle, SOL_NVS_KEY_WIFI_PASS, config->wifi_pass, &len);

    // Load audio settings
    nvs_get_u8(handle, SOL_NVS_KEY_CHANNELS, &config->channels);
    nvs_get_u16(handle, SOL_NVS_KEY_RTP_PORT, &config->rtp_port);
    nvs_get_u32(handle, SOL_NVS_KEY_SSRC, &config->ssrc);
    nvs_get_u8(handle, SOL_NVS_KEY_FEC_ENABLED, &config->fec_enabled);

    // Load target latency (stored as integer ms * 10)
    uint16_t lat_x10 = 0;
    if (nvs_get_u16(handle, SOL_NVS_KEY_TARGET_LAT, &lat_x10) == ESP_OK) {
        config->target_latency_ms = (float)lat_x10 / 10.0f;
    }

    nvs_close(handle);

    ESP_LOGI(TAG, "Config loaded: name=%s mode=%d", config->device_name, config->mode);
    return 0;
}

int sol_nvs_save(const sol_config_t* config) {
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(SOL_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for write: %s", esp_err_to_name(err));
        return -1;
    }

    nvs_set_str(handle, SOL_NVS_KEY_DEVICE_NAME, config->device_name);
    nvs_set_u8(handle, SOL_NVS_KEY_MODE, (uint8_t)config->mode);
    nvs_set_str(handle, SOL_NVS_KEY_WIFI_SSID, config->wifi_ssid);
    nvs_set_str(handle, SOL_NVS_KEY_WIFI_PASS, config->wifi_pass);
    nvs_set_u8(handle, SOL_NVS_KEY_CHANNELS, config->channels);
    nvs_set_u16(handle, SOL_NVS_KEY_RTP_PORT, config->rtp_port);
    nvs_set_u32(handle, SOL_NVS_KEY_SSRC, config->ssrc);
    nvs_set_u8(handle, SOL_NVS_KEY_FEC_ENABLED, config->fec_enabled);
    nvs_set_u16(handle, SOL_NVS_KEY_TARGET_LAT, (uint16_t)(config->target_latency_ms * 10.0f));

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
        return -1;
    }

    ESP_LOGI(TAG, "Config saved");
    return 0;
}

int sol_nvs_factory_reset(void) {
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(SOL_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return -1;
    }

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    ESP_LOGI(TAG, "Factory reset complete");
    return (err == ESP_OK) ? 0 : -1;
}

#else
// Stub implementations for non-ESP32 builds

int sol_nvs_init(void) { return 0; }
int sol_nvs_load(sol_config_t* config) { sol_nvs_get_defaults(config); return 0; }
int sol_nvs_save(const sol_config_t* config) { (void)config; return 0; }
int sol_nvs_factory_reset(void) { return 0; }

#endif
