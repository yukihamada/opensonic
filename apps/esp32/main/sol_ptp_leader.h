/**
 * Soluna ESP32 — PTP Leader Mode
 *
 * Provides PTP grandmaster clock for standalone P2P networks.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "sol_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * PTP leader statistics.
 */
typedef struct {
    uint32_t announce_sent;
    uint32_t sync_sent;
    uint32_t delay_req_received;
    uint32_t followers_count;
    int64_t clock_offset_ns;
} sol_ptp_leader_stats_t;

/**
 * PTP leader configuration.
 */
typedef struct {
    uint8_t priority1;          // PTP priority1 (default 128)
    uint8_t priority2;          // PTP priority2 (default 128)
    uint8_t domain;             // PTP domain (default 0)
    uint32_t announce_interval; // Announce interval in ms (default 1000)
    uint32_t sync_interval;     // Sync interval in ms (default 125)
} sol_ptp_leader_config_t;

/**
 * Initialize PTP leader mode.
 *
 * @param config PTP leader configuration (NULL for defaults)
 * @return 0 on success
 */
int sol_ptp_leader_init(const sol_ptp_leader_config_t* config);

/**
 * Shutdown PTP leader mode.
 */
void sol_ptp_leader_shutdown(void);

/**
 * Start PTP leader (grandmaster clock).
 *
 * @return 0 on success
 */
int sol_ptp_leader_start(void);

/**
 * Stop PTP leader.
 */
void sol_ptp_leader_stop(void);

/**
 * Check if PTP leader is running.
 */
bool sol_ptp_leader_is_running(void);

/**
 * Get PTP leader statistics.
 *
 * @param stats Output statistics structure
 */
void sol_ptp_leader_get_stats(sol_ptp_leader_stats_t* stats);

/**
 * Get current PTP time.
 *
 * @return Nanoseconds since epoch
 */
int64_t sol_ptp_leader_get_time(void);

#ifdef __cplusplus
}
#endif
