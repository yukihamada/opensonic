/**
 * Soluna ESP32 — NVS Configuration Storage
 *
 * Persistent configuration using Non-Volatile Storage.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "sol_common.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Configuration keys stored in NVS.
 */
#define SOL_NVS_NAMESPACE       "soluna"
#define SOL_NVS_KEY_DEVICE_NAME "dev_name"
#define SOL_NVS_KEY_MODE        "mode"
#define SOL_NVS_KEY_WIFI_SSID   "wifi_ssid"
#define SOL_NVS_KEY_WIFI_PASS   "wifi_pass"
#define SOL_NVS_KEY_CHANNELS    "channels"
#define SOL_NVS_KEY_RTP_PORT    "rtp_port"
#define SOL_NVS_KEY_SSRC        "ssrc"
#define SOL_NVS_KEY_FEC_ENABLED "fec_on"
#define SOL_NVS_KEY_TARGET_LAT  "target_lat"

/* Note: sol_config_t is defined in sol_common.h */

/**
 * Initialize NVS storage.
 *
 * @return 0 on success
 */
int sol_nvs_init(void);

/**
 * Load configuration from NVS.
 *
 * @param config Output configuration structure
 * @return 0 on success
 */
int sol_nvs_load(sol_config_t* config);

/**
 * Save configuration to NVS.
 *
 * @param config Configuration to save
 * @return 0 on success
 */
int sol_nvs_save(const sol_config_t* config);

/**
 * Reset configuration to factory defaults.
 *
 * @return 0 on success
 */
int sol_nvs_factory_reset(void);

/**
 * Get default configuration values.
 *
 * @param config Output configuration structure
 */
void sol_nvs_get_defaults(sol_config_t* config);

#ifdef __cplusplus
}
#endif
