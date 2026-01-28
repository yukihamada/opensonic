/**
 * Soluna ESP32 — OTA (Over-The-Air) Firmware Update
 *
 * Receives firmware chunks via the binary control protocol and
 * writes them to the OTA partition using ESP-IDF OTA API.
 *
 * Flow:
 *   1. Host sends OTA_BEGIN with firmware size
 *   2. Device responds OTA_READY
 *   3. Host sends OTA_WRITE chunks (1024 bytes, sequenced)
 *   4. Host sends OTA_FINISH
 *   5. Device validates, sets boot partition, reboots
 *
 * Rollback: If the new firmware fails to boot, the bootloader
 * automatically reverts to the previous partition.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "sol_common.h"

/** OTA state */
typedef enum {
    SOL_OTA_IDLE = 0,
    SOL_OTA_IN_PROGRESS,
    SOL_OTA_COMPLETE,
    SOL_OTA_ERROR,
} sol_ota_state_t;

/** OTA status info */
typedef struct {
    sol_ota_state_t state;
    uint32_t firmware_size;      /* Expected total size */
    uint32_t bytes_received;     /* Bytes written so far */
    uint32_t next_sequence;      /* Expected next chunk sequence */
    uint32_t chunk_size;         /* Size per chunk (default 1024) */
} sol_ota_status_t;

/**
 * OTA_BEGIN payload (received from host):
 *   [0..3]  firmware_size (big-endian)
 *
 * OTA_WRITE payload (received from host):
 *   [0..3]  sequence number (big-endian)
 *   [4..N]  firmware data chunk (up to 1024 bytes)
 *
 * OTA_FINISH payload: empty
 */

#define SOL_OTA_CHUNK_SIZE 1024

/** Response command IDs (sent back to host) */
#define SOL_CMD_OTA_READY   0xF4
#define SOL_CMD_OTA_ACK     0xF5
#define SOL_CMD_OTA_ERROR   0xF6

/**
 * Initialize OTA subsystem.
 * Must be called before handling OTA commands.
 */
sol_err_t sol_ota_init(void);

/**
 * Handle OTA_BEGIN command.
 * Prepares the OTA partition for writing.
 *
 * @param firmware_size  Total firmware size in bytes
 * @return SOL_OK on success
 */
sol_err_t sol_ota_begin(uint32_t firmware_size);

/**
 * Handle OTA_WRITE command.
 * Writes a chunk of firmware data.
 *
 * @param sequence  Chunk sequence number (0-based)
 * @param data      Firmware data chunk
 * @param len       Chunk length (up to SOL_OTA_CHUNK_SIZE)
 * @return SOL_OK on success
 */
sol_err_t sol_ota_write(uint32_t sequence, const uint8_t* data, size_t len);

/**
 * Handle OTA_FINISH command.
 * Validates firmware, sets boot partition.
 * On success, the device will reboot.
 *
 * @return SOL_OK on success (device reboots), error code otherwise
 */
sol_err_t sol_ota_finish(void);

/**
 * Get current OTA status.
 */
sol_ota_status_t sol_ota_get_status(void);

/**
 * Confirm that the current firmware is working.
 * Call this early in main() after a successful OTA boot
 * to prevent automatic rollback.
 */
void sol_ota_confirm_boot(void);
