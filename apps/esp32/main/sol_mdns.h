/**
 * Soluna ESP32 — mDNS Service Announcement
 * Announces _soluna._udp service via esp_mdns
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "sol_common.h"

/** Initialize mDNS and announce service */
sol_err_t sol_mdns_init(const char* hostname, const char* device_name,
                         uint8_t channels, sol_mode_t mode);

/** Update service properties (e.g., after config change) */
sol_err_t sol_mdns_update(const char* device_name, uint8_t channels,
                           sol_mode_t mode);

/** Stop mDNS */
void sol_mdns_stop(void);
