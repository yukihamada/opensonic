/**
 * Soluna ESP32 — PTP Follower (Slave-only)
 * Simplified PTPv2: Sync + Follow_Up + Delay_Req/Resp
 * No master capability (ESP32 always follows)
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "sol_common.h"

typedef struct {
    int64_t offset_ns;        /* Clock offset from master */
    int64_t path_delay_ns;    /* One-way path delay */
    bool synced;              /* Have we received at least one sync? */
    uint32_t sync_count;      /* Total syncs received */
    uint32_t lost_count;      /* Missed syncs */
} sol_ptp_state_t;

/** Initialize PTP follower task (runs on SOL_CORE_NET) */
sol_err_t sol_ptp_init(void);

/** Stop PTP follower */
void sol_ptp_stop(void);

/** Get current PTP state (thread-safe copy) */
sol_ptp_state_t sol_ptp_get_state(void);

/** Get PTP-corrected monotonic time (local + offset) */
int64_t sol_ptp_now_ns(void);
