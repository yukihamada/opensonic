/**
 * Soluna ESP32 — Main Entry Point
 *
 * Boot sequence:
 *   1. NVS init (load config)
 *   2. WiFi connect
 *   3. mDNS announce
 *   4. PTP follower start
 *   5. Audio engine start
 *   6. Control protocol start
 *   7. Status LED blink loop
 *
 * SPDX-License-Identifier: MIT
 */

#include "sol_common.h"
#include "sol_i2s.h"
#include "sol_net.h"
#include "sol_clock.h"
#include "sol_ptp_follower.h"
#include "sol_rtp.h"
#include "sol_fec.h"
#include "sol_mdns.h"
#include "sol_control.h"
#include "sol_engine.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include <string.h>

static const char* TAG = "soluna";

/* WiFi connection event group */
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_count = 0;
#define MAX_RETRY 10

/* Device config and stats */
static sol_config_t s_config;
static sol_stats_t s_stats;

/* ---- NVS Config ---- */

static void load_default_config(sol_config_t* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode = SOL_MODE_RX;
    cfg->channels = 2;
    cfg->rtp_port = SOL_PORT_RTP;
    cfg->ssrc = SOL_SSRC_DEFAULT;
    strncpy(cfg->device_name, "soluna-esp32", sizeof(cfg->device_name) - 1);
    strncpy(cfg->wifi_ssid, "SolunaAudio", sizeof(cfg->wifi_ssid) - 1);
    cfg->wifi_pass[0] = '\0';  /* Open network by default */
    cfg->fec_enabled = 1;
    cfg->target_latency_ms = 8.0f;
}

static void load_config_from_nvs(sol_config_t* cfg) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("soluna", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS not found, using defaults");
        load_default_config(cfg);
        return;
    }

    load_default_config(cfg);

    uint8_t u8val;
    uint16_t u16val;
    uint32_t u32val;
    size_t str_len;

    if (nvs_get_u8(nvs, "mode", &u8val) == ESP_OK) cfg->mode = (sol_mode_t)u8val;
    if (nvs_get_u8(nvs, "channels", &u8val) == ESP_OK) cfg->channels = u8val;
    if (nvs_get_u16(nvs, "rtp_port", &u16val) == ESP_OK) cfg->rtp_port = u16val;
    if (nvs_get_u32(nvs, "ssrc", &u32val) == ESP_OK) cfg->ssrc = u32val;
    if (nvs_get_u8(nvs, "fec", &u8val) == ESP_OK) cfg->fec_enabled = u8val;

    str_len = sizeof(cfg->device_name);
    nvs_get_str(nvs, "dev_name", cfg->device_name, &str_len);

    str_len = sizeof(cfg->wifi_ssid);
    nvs_get_str(nvs, "wifi_ssid", cfg->wifi_ssid, &str_len);

    str_len = sizeof(cfg->wifi_pass);
    nvs_get_str(nvs, "wifi_pass", cfg->wifi_pass, &str_len);

    nvs_close(nvs);

    ESP_LOGI(TAG, "Config loaded: mode=%d ch=%u port=%u ssid='%s' fec=%u",
        cfg->mode, cfg->channels, cfg->rtp_port, cfg->wifi_ssid, cfg->fec_enabled);
}

/* ---- WiFi ---- */

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGI(TAG, "WiFi retry %d/%d", s_retry_count, MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "WiFi connected: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static sol_err_t wifi_init(const sol_config_t* cfg) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    esp_event_handler_instance_t inst_any, inst_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &inst_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &inst_got_ip));

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, cfg->wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, cfg->wifi_pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = cfg->wifi_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi '%s'...", cfg->wifi_ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        return SOL_OK;
    }

    ESP_LOGE(TAG, "WiFi connection failed");
    return SOL_ERR_IO;
}

/* ---- Main ---- */

void app_main(void) {
    ESP_LOGI(TAG, "Soluna ESP32 v%d.%d.%d starting...",
        SOL_VERSION_MAJOR, SOL_VERSION_MINOR, SOL_VERSION_PATCH);
    ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());

    /* 1. NVS init */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    load_config_from_nvs(&s_config);

    /* 2. WiFi connect */
    sol_err_t err = wifi_init(&s_config);
    if (err != SOL_OK) {
        ESP_LOGE(TAG, "Cannot start without WiFi. Rebooting in 5s...");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    /* 3. mDNS announce */
    sol_mdns_init(s_config.device_name, s_config.device_name,
                   s_config.channels, s_config.mode);

    /* 4. PTP follower */
    err = sol_ptp_init();
    if (err != SOL_OK) {
        ESP_LOGW(TAG, "PTP init failed (continuing without sync)");
    }

    /* 5. Audio engine */
    err = sol_engine_init(&s_config, &s_stats);
    if (err != SOL_OK) {
        ESP_LOGE(TAG, "Engine init failed: %d", err);
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    err = sol_engine_start();
    if (err != SOL_OK) {
        ESP_LOGE(TAG, "Engine start failed: %d", err);
    }

    /* 6. Control protocol */
    sol_control_init(&s_config, &s_stats);

    /* 7. Status monitoring loop */
    ESP_LOGI(TAG, "Soluna running. Free heap: %lu bytes",
        (unsigned long)esp_get_free_heap_size());

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));

        sol_ptp_state_t ptp = sol_ptp_get_state();
        ESP_LOGI(TAG,
            "TX:%llu RX:%llu Lost:%llu FEC:%llu Under:%lu Over:%lu "
            "PTP:%s(%lldns) Heap:%lu",
            (unsigned long long)s_stats.packets_tx,
            (unsigned long long)s_stats.packets_rx,
            (unsigned long long)s_stats.packets_lost,
            (unsigned long long)s_stats.fec_recovered,
            (unsigned long)s_stats.underruns,
            (unsigned long)s_stats.overruns,
            ptp.synced ? "sync" : "free",
            (long long)ptp.offset_ns,
            (unsigned long)esp_get_free_heap_size());
    }
}
