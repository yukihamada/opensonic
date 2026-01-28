/**
 * Soluna ESP32 — Built-in Web UI
 *
 * Lightweight HTTP server for device configuration.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "sol_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * WebUI status callback - provides real-time status to UI.
 */
typedef struct {
    bool ptp_synced;
    int64_t ptp_offset_ns;
    uint32_t packets_received;
    uint32_t packets_lost;
    uint32_t buffer_level_ms;
    int8_t rssi;
    uint32_t uptime_sec;
    const char* ip_address;
} sol_webui_status_t;

typedef void (*sol_webui_status_callback_t)(sol_webui_status_t* status);

/**
 * WebUI action callback - called when user performs actions.
 */
typedef enum {
    SOL_WEBUI_ACTION_REBOOT,
    SOL_WEBUI_ACTION_FACTORY_RESET,
    SOL_WEBUI_ACTION_SAVE_CONFIG,
    SOL_WEBUI_ACTION_START_OTA,
} sol_webui_action_t;

typedef int (*sol_webui_action_callback_t)(sol_webui_action_t action, void* data);

/**
 * WebUI configuration.
 */
typedef struct {
    uint16_t port;                          // HTTP port (default 80)
    sol_webui_status_callback_t status_cb;  // Status callback
    sol_webui_action_callback_t action_cb;  // Action callback
    sol_config_t* config;                   // Pointer to live config
} sol_webui_config_t;

/**
 * Start the WebUI HTTP server.
 *
 * @param webui_config WebUI configuration
 * @return 0 on success
 */
int sol_webui_start(const sol_webui_config_t* webui_config);

/**
 * Stop the WebUI HTTP server.
 */
void sol_webui_stop(void);

/**
 * Check if WebUI is running.
 */
bool sol_webui_is_running(void);

#ifdef __cplusplus
}
#endif
