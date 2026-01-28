/**
 * Soluna ESP32 — OTA Firmware Update Implementation
 * SPDX-License-Identifier: MIT
 */

#include "sol_ota.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

#include <string.h>

static const char* TAG = "sol_ota";

static sol_ota_status_t s_status = {0};
static esp_ota_handle_t s_ota_handle = 0;
static const esp_partition_t* s_update_partition = NULL;

sol_err_t sol_ota_init(void) {
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = SOL_OTA_IDLE;
    s_status.chunk_size = SOL_OTA_CHUNK_SIZE;

    ESP_LOGI(TAG, "OTA subsystem initialized");
    return SOL_OK;
}

sol_err_t sol_ota_begin(uint32_t firmware_size) {
    if (s_status.state == SOL_OTA_IN_PROGRESS) {
        ESP_LOGW(TAG, "OTA already in progress");
        return SOL_ERR_PARAM;
    }

    /* Find the next OTA partition */
    s_update_partition = esp_ota_get_next_update_partition(NULL);
    if (!s_update_partition) {
        ESP_LOGE(TAG, "No OTA partition found");
        s_status.state = SOL_OTA_ERROR;
        return SOL_ERR_INIT;
    }

    ESP_LOGI(TAG, "OTA begin: partition=%s size=%lu",
             s_update_partition->label, (unsigned long)firmware_size);

    /* Check partition size */
    if (firmware_size > s_update_partition->size) {
        ESP_LOGE(TAG, "Firmware too large: %lu > %lu",
                 (unsigned long)firmware_size,
                 (unsigned long)s_update_partition->size);
        s_status.state = SOL_OTA_ERROR;
        return SOL_ERR_PARAM;
    }

    esp_err_t err = esp_ota_begin(s_update_partition, firmware_size, &s_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        s_status.state = SOL_OTA_ERROR;
        return SOL_ERR_IO;
    }

    s_status.state = SOL_OTA_IN_PROGRESS;
    s_status.firmware_size = firmware_size;
    s_status.bytes_received = 0;
    s_status.next_sequence = 0;

    return SOL_OK;
}

sol_err_t sol_ota_write(uint32_t sequence, const uint8_t* data, size_t len) {
    if (s_status.state != SOL_OTA_IN_PROGRESS) {
        ESP_LOGW(TAG, "OTA not in progress");
        return SOL_ERR_PARAM;
    }

    if (sequence != s_status.next_sequence) {
        ESP_LOGW(TAG, "OTA sequence mismatch: expected %lu got %lu",
                 (unsigned long)s_status.next_sequence,
                 (unsigned long)sequence);
        return SOL_ERR_PARAM;
    }

    if (len == 0 || len > SOL_OTA_CHUNK_SIZE) {
        return SOL_ERR_PARAM;
    }

    /* Don't exceed firmware size */
    if (s_status.bytes_received + len > s_status.firmware_size) {
        len = s_status.firmware_size - s_status.bytes_received;
    }

    esp_err_t err = esp_ota_write(s_ota_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        s_status.state = SOL_OTA_ERROR;
        return SOL_ERR_IO;
    }

    s_status.bytes_received += (uint32_t)len;
    s_status.next_sequence++;

    if (s_status.next_sequence % 100 == 0) {
        ESP_LOGI(TAG, "OTA progress: %lu / %lu bytes (%lu%%)",
                 (unsigned long)s_status.bytes_received,
                 (unsigned long)s_status.firmware_size,
                 (unsigned long)(s_status.bytes_received * 100 / s_status.firmware_size));
    }

    return SOL_OK;
}

sol_err_t sol_ota_finish(void) {
    if (s_status.state != SOL_OTA_IN_PROGRESS) {
        ESP_LOGW(TAG, "OTA not in progress");
        return SOL_ERR_PARAM;
    }

    if (s_status.bytes_received < s_status.firmware_size) {
        ESP_LOGW(TAG, "OTA incomplete: %lu / %lu bytes",
                 (unsigned long)s_status.bytes_received,
                 (unsigned long)s_status.firmware_size);
        return SOL_ERR_PARAM;
    }

    esp_err_t err = esp_ota_end(s_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        s_status.state = SOL_OTA_ERROR;
        return SOL_ERR_IO;
    }

    err = esp_ota_set_boot_partition(s_update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        s_status.state = SOL_OTA_ERROR;
        return SOL_ERR_IO;
    }

    s_status.state = SOL_OTA_COMPLETE;
    ESP_LOGI(TAG, "OTA complete: %lu bytes written. Rebooting...",
             (unsigned long)s_status.bytes_received);

    /* Reboot into new firmware */
    esp_restart();

    /* Should not reach here */
    return SOL_OK;
}

sol_ota_status_t sol_ota_get_status(void) {
    return s_status;
}

void sol_ota_confirm_boot(void) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "Confirming OTA boot — marking as valid");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
}
