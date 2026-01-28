/**
 * Soluna ESP32 — Binary Control Protocol Implementation
 * SPDX-License-Identifier: MIT
 */

#include "sol_control.h"
#include "sol_net.h"
#include "sol_clock.h"
#include "sol_ptp_follower.h"
#include "sol_ota.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"

#include <string.h>

static const char* TAG = "sol_ctrl";

static sol_socket_t s_sock;
static volatile bool s_running = false;
static TaskHandle_t s_task = NULL;
static sol_config_t* s_config = NULL;
static sol_stats_t* s_stats = NULL;

/* Pack uint16 big-endian */
static void pack_u16(uint8_t* dst, uint16_t val) {
    dst[0] = (uint8_t)(val >> 8);
    dst[1] = (uint8_t)(val & 0xFF);
}

/* Pack uint32 big-endian */
static void pack_u32(uint8_t* dst, uint32_t val) {
    dst[0] = (uint8_t)(val >> 24);
    dst[1] = (uint8_t)(val >> 16);
    dst[2] = (uint8_t)(val >> 8);
    dst[3] = (uint8_t)(val & 0xFF);
}

/* Pack uint64 big-endian */
static void pack_u64(uint8_t* dst, uint64_t val) {
    pack_u32(dst, (uint32_t)(val >> 32));
    pack_u32(dst + 4, (uint32_t)(val & 0xFFFFFFFF));
}

static void send_response(const char* ip, uint16_t port,
                            uint8_t cmd, const uint8_t* payload, uint16_t len) {
    uint8_t buf[512];
    if (SOL_CTRL_HDR_SIZE + len > sizeof(buf)) return;

    buf[0] = SOL_CTRL_MAGIC;
    buf[1] = cmd;
    pack_u16(buf + 2, len);
    if (len > 0 && payload) {
        memcpy(buf + SOL_CTRL_HDR_SIZE, payload, len);
    }

    sol_socket_sendto(&s_sock, buf, SOL_CTRL_HDR_SIZE + len, ip, port);
}

static void handle_ping(const char* ip, uint16_t port) {
    send_response(ip, port, SOL_CMD_PONG, NULL, 0);
}

static void handle_status(const char* ip, uint16_t port) {
    /* Update dynamic stats */
    s_stats->free_heap = esp_get_free_heap_size();
    s_stats->uptime_sec = (uint32_t)(sol_clock_now_us() / 1000000ULL);

    sol_ptp_state_t ptp = sol_ptp_get_state();
    s_stats->ptp_offset_ns = ptp.offset_ns;

    /* Pack status response (64 bytes) */
    uint8_t resp[64];
    memset(resp, 0, sizeof(resp));

    pack_u64(resp + 0,  s_stats->packets_tx);
    pack_u64(resp + 8,  s_stats->packets_rx);
    pack_u64(resp + 16, s_stats->packets_lost);
    pack_u64(resp + 24, s_stats->fec_recovered);
    pack_u32(resp + 32, s_stats->underruns);
    pack_u32(resp + 36, s_stats->overruns);
    /* PTP offset (int64) */
    pack_u64(resp + 40, (uint64_t)s_stats->ptp_offset_ns);
    pack_u32(resp + 48, s_stats->free_heap);
    pack_u32(resp + 52, s_stats->uptime_sec);
    /* Jitter as fixed-point (ms * 100) */
    uint16_t jitter_fp = (uint16_t)(s_stats->jitter_ms * 100.0f);
    pack_u16(resp + 56, jitter_fp);
    resp[58] = ptp.synced ? 1 : 0;
    pack_u32(resp + 59, ptp.sync_count);

    send_response(ip, port, SOL_CMD_STATUS_RESP, resp, 64);
}

static void handle_config_get(const char* ip, uint16_t port) {
    /* Pack config */
    uint8_t resp[128];
    memset(resp, 0, sizeof(resp));

    resp[0] = (uint8_t)s_config->mode;
    resp[1] = s_config->channels;
    pack_u16(resp + 2, s_config->rtp_port);
    pack_u32(resp + 4, s_config->ssrc);
    resp[8] = s_config->fec_enabled;
    /* target latency as fixed-point ms * 10 */
    pack_u16(resp + 9, (uint16_t)(s_config->target_latency_ms * 10.0f));
    /* device name (32 bytes) */
    memcpy(resp + 11, s_config->device_name, 32);

    send_response(ip, port, SOL_CMD_CONFIG_RESP, resp, 43);
}

static void handle_config_set(const uint8_t* payload, uint16_t len,
                                const char* ip, uint16_t port) {
    if (len < 11) return;

    s_config->mode = (sol_mode_t)payload[0];
    s_config->channels = payload[1];
    s_config->rtp_port = ((uint16_t)payload[2] << 8) | payload[3];
    s_config->ssrc = ((uint32_t)payload[4] << 24) | ((uint32_t)payload[5] << 16) |
                     ((uint32_t)payload[6] << 8) | payload[7];
    s_config->fec_enabled = payload[8];
    uint16_t lat_fp = ((uint16_t)payload[9] << 8) | payload[10];
    s_config->target_latency_ms = lat_fp / 10.0f;

    if (len >= 43) {
        memcpy(s_config->device_name, payload + 11, 32);
    }

    ESP_LOGI(TAG, "Config updated: mode=%d ch=%u port=%u fec=%u",
        s_config->mode, s_config->channels, s_config->rtp_port,
        s_config->fec_enabled);

    /* Send back current config as ACK */
    handle_config_get(ip, port);
}

static void control_task(void* arg) {
    uint8_t buf[512];
    char src_ip[16];
    uint16_t src_port;

    while (s_running) {
        int n = sol_socket_recvfrom(&s_sock, buf, sizeof(buf),
                                     src_ip, sizeof(src_ip), &src_port, 500);
        if (n < SOL_CTRL_HDR_SIZE) continue;
        if (buf[0] != SOL_CTRL_MAGIC) continue;

        uint8_t cmd = buf[1];
        uint16_t payload_len = ((uint16_t)buf[2] << 8) | buf[3];
        const uint8_t* payload = buf + SOL_CTRL_HDR_SIZE;

        if (SOL_CTRL_HDR_SIZE + payload_len > (uint16_t)n) continue;

        switch (cmd) {
            case SOL_CMD_PING:
                handle_ping(src_ip, src_port);
                break;
            case SOL_CMD_STATUS:
                handle_status(src_ip, src_port);
                break;
            case SOL_CMD_CONFIG_GET:
                handle_config_get(src_ip, src_port);
                break;
            case SOL_CMD_CONFIG_SET:
                handle_config_set(payload, payload_len, src_ip, src_port);
                break;
            case SOL_CMD_REBOOT:
                ESP_LOGW(TAG, "Reboot requested");
                esp_restart();
                break;
            case SOL_CMD_OTA_BEGIN: {
                if (payload_len < 4) break;
                uint32_t fw_size = ((uint32_t)payload[0] << 24) |
                                   ((uint32_t)payload[1] << 16) |
                                   ((uint32_t)payload[2] << 8) |
                                   payload[3];
                ESP_LOGI(TAG, "OTA begin: firmware_size=%lu", (unsigned long)fw_size);
                sol_err_t err = sol_ota_begin(fw_size);
                if (err == SOL_OK) {
                    send_response(src_ip, src_port, SOL_CMD_OTA_READY, NULL, 0);
                } else {
                    uint8_t err_code = (uint8_t)err;
                    send_response(src_ip, src_port, SOL_CMD_OTA_ERROR, &err_code, 1);
                }
                break;
            }
            case SOL_CMD_OTA_CHUNK: {
                if (payload_len < 5) break;
                uint32_t seq = ((uint32_t)payload[0] << 24) |
                               ((uint32_t)payload[1] << 16) |
                               ((uint32_t)payload[2] << 8) |
                               payload[3];
                const uint8_t* chunk_data = payload + 4;
                uint16_t chunk_len = payload_len - 4;
                sol_err_t err = sol_ota_write(seq, chunk_data, chunk_len);
                if (err == SOL_OK) {
                    /* ACK with sequence number */
                    uint8_t ack[4];
                    pack_u32(ack, seq);
                    send_response(src_ip, src_port, SOL_CMD_OTA_ACK, ack, 4);
                } else {
                    uint8_t err_code = (uint8_t)err;
                    send_response(src_ip, src_port, SOL_CMD_OTA_ERROR, &err_code, 1);
                }
                break;
            }
            case SOL_CMD_OTA_END: {
                ESP_LOGI(TAG, "OTA finish requested");
                sol_err_t err = sol_ota_finish();
                /* If finish succeeds, device reboots and we won't reach here */
                if (err != SOL_OK) {
                    uint8_t err_code = (uint8_t)err;
                    send_response(src_ip, src_port, SOL_CMD_OTA_ERROR, &err_code, 1);
                }
                break;
            }
            default:
                ESP_LOGW(TAG, "Unknown cmd: 0x%02x", cmd);
                break;
        }
    }

    vTaskDelete(NULL);
}

sol_err_t sol_control_init(sol_config_t* config, sol_stats_t* stats) {
    s_config = config;
    s_stats = stats;

    sol_err_t err = sol_socket_create(&s_sock);
    if (err != SOL_OK) return err;
    err = sol_socket_bind(&s_sock, SOL_PORT_CONTROL);
    if (err != SOL_OK) return err;

    s_running = true;
    xTaskCreatePinnedToCore(control_task, "sol_ctrl", SOL_STACK_CTRL,
        NULL, 3, &s_task, SOL_CORE_NET);

    ESP_LOGI(TAG, "Control protocol listening on UDP %u", SOL_PORT_CONTROL);
    return SOL_OK;
}

void sol_control_stop(void) {
    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(600));
    sol_socket_close(&s_sock);
    ESP_LOGI(TAG, "Control stopped");
}
