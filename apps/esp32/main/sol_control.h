/**
 * Soluna ESP32 — Binary Control Protocol (UDP)
 * Lightweight binary protocol for ESP32 ←→ Desktop communication
 * Port: SOL_PORT_CONTROL (8401)
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "sol_common.h"

/* Binary command IDs */
typedef enum {
    SOL_CMD_PING        = 0x01,
    SOL_CMD_PONG        = 0x02,
    SOL_CMD_STATUS      = 0x10,
    SOL_CMD_STATUS_RESP = 0x11,
    SOL_CMD_CONFIG_GET  = 0x20,
    SOL_CMD_CONFIG_SET  = 0x21,
    SOL_CMD_CONFIG_RESP = 0x22,
    SOL_CMD_REBOOT      = 0xF0,
    SOL_CMD_OTA_BEGIN   = 0xF1,
    SOL_CMD_OTA_CHUNK   = 0xF2,
    SOL_CMD_OTA_END     = 0xF3,
    SOL_CMD_OTA_READY   = 0xF4,
    SOL_CMD_OTA_ACK     = 0xF5,
    SOL_CMD_OTA_ERROR   = 0xF6,
} sol_cmd_id_t;

/*
 * Packet format:
 *   [0]     Magic: 0x53 ('S')
 *   [1]     Command ID
 *   [2..3]  Payload length (big-endian)
 *   [4..N]  Payload
 */
#define SOL_CTRL_MAGIC  0x53
#define SOL_CTRL_HDR_SIZE 4

/** Initialize control protocol (starts listener task on SOL_CORE_NET) */
sol_err_t sol_control_init(sol_config_t* config, sol_stats_t* stats);

/** Stop control listener */
void sol_control_stop(void);
