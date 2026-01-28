/**
 * Soluna ESP32 — I2S + DMA Audio Driver
 * Uses ESP-IDF I2S driver (STD mode for ESP32-S3)
 * SPDX-License-Identifier: MIT
 */

#include "sol_i2s.h"

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include <string.h>

static const char* TAG = "sol_i2s";

static i2s_chan_handle_t s_tx_chan = NULL;
static i2s_chan_handle_t s_rx_chan = NULL;
static sol_ring_t* s_tx_ring = NULL;
static sol_ring_t* s_rx_ring = NULL;
static TaskHandle_t s_tx_task = NULL;
static TaskHandle_t s_rx_task = NULL;
static volatile bool s_running = false;
static uint32_t s_underruns = 0;
static uint32_t s_overruns = 0;
static uint8_t s_channels = 2;

/* TX task: ring buffer → I2S DMA */
static void i2s_tx_task(void* arg) {
    int32_t buf[SOL_FRAME_SIZE_SAMPLES * SOL_MAX_CHANNELS];
    size_t bytes_written;
    const uint32_t frame_bytes = SOL_FRAME_SIZE_SAMPLES * s_channels * sizeof(int32_t);

    while (s_running) {
        if (sol_ring_read(s_tx_ring, buf, SOL_FRAME_SIZE_SAMPLES)) {
            i2s_channel_write(s_tx_chan, buf, frame_bytes, &bytes_written, portMAX_DELAY);
        } else {
            /* Underrun: write silence */
            memset(buf, 0, frame_bytes);
            i2s_channel_write(s_tx_chan, buf, frame_bytes, &bytes_written, portMAX_DELAY);
            s_underruns++;
        }
    }

    vTaskDelete(NULL);
}

/* RX task: I2S DMA → ring buffer */
static void i2s_rx_task(void* arg) {
    int32_t buf[SOL_FRAME_SIZE_SAMPLES * SOL_MAX_CHANNELS];
    size_t bytes_read;
    const uint32_t frame_bytes = SOL_FRAME_SIZE_SAMPLES * s_channels * sizeof(int32_t);

    while (s_running) {
        esp_err_t err = i2s_channel_read(s_rx_chan, buf, frame_bytes,
                                          &bytes_read, portMAX_DELAY);
        if (err == ESP_OK && bytes_read == frame_bytes) {
            if (!sol_ring_write(s_rx_ring, buf, SOL_FRAME_SIZE_SAMPLES)) {
                s_overruns++;
            }
        }
    }

    vTaskDelete(NULL);
}

sol_err_t sol_i2s_init(const sol_i2s_config_t* cfg) {
    s_channels = cfg->channels;

    /* Create I2S channel */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = cfg->dma_buf_count ? cfg->dma_buf_count : 4;
    chan_cfg.dma_frame_num = cfg->dma_buf_len ? cfg->dma_buf_len : SOL_FRAME_SIZE_SAMPLES;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(err));
        return SOL_ERR_INIT;
    }

    /* Configure STD mode (I2S Philips) */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(cfg->sample_rate ? cfg->sample_rate : SOL_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                        cfg->channels == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)cfg->bck_pin,
            .ws = (gpio_num_t)cfg->ws_pin,
            .dout = (gpio_num_t)cfg->data_out_pin,
            .din = (gpio_num_t)cfg->data_in_pin,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    if (s_tx_chan) {
        err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "TX init failed: %s", esp_err_to_name(err));
            return SOL_ERR_INIT;
        }
    }
    if (s_rx_chan) {
        err = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "RX init failed: %s", esp_err_to_name(err));
            return SOL_ERR_INIT;
        }
    }

    ESP_LOGI(TAG, "I2S initialized: %uHz, %uch, BCK=%u WS=%u DOUT=%u DIN=%u",
        cfg->sample_rate ? cfg->sample_rate : SOL_SAMPLE_RATE,
        cfg->channels, cfg->bck_pin, cfg->ws_pin,
        cfg->data_out_pin, cfg->data_in_pin);

    return SOL_OK;
}

sol_err_t sol_i2s_start_tx(sol_ring_t* ring) {
    if (!s_tx_chan || s_running) return SOL_ERR_INIT;

    s_tx_ring = ring;
    s_running = true;
    i2s_channel_enable(s_tx_chan);

    xTaskCreatePinnedToCore(i2s_tx_task, "sol_i2s_tx",
        SOL_STACK_AUDIO, NULL, configMAX_PRIORITIES - 1,
        &s_tx_task, SOL_CORE_AUDIO);

    ESP_LOGI(TAG, "I2S TX started on core %d", SOL_CORE_AUDIO);
    return SOL_OK;
}

sol_err_t sol_i2s_start_rx(sol_ring_t* ring) {
    if (!s_rx_chan || s_running) return SOL_ERR_INIT;

    s_rx_ring = ring;
    s_running = true;
    i2s_channel_enable(s_rx_chan);

    xTaskCreatePinnedToCore(i2s_rx_task, "sol_i2s_rx",
        SOL_STACK_AUDIO, NULL, configMAX_PRIORITIES - 1,
        &s_rx_task, SOL_CORE_AUDIO);

    ESP_LOGI(TAG, "I2S RX started on core %d", SOL_CORE_AUDIO);
    return SOL_OK;
}

void sol_i2s_stop(void) {
    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(10));

    if (s_tx_chan) i2s_channel_disable(s_tx_chan);
    if (s_rx_chan) i2s_channel_disable(s_rx_chan);

    if (s_tx_chan) { i2s_del_channel(s_tx_chan); s_tx_chan = NULL; }
    if (s_rx_chan) { i2s_del_channel(s_rx_chan); s_rx_chan = NULL; }

    ESP_LOGI(TAG, "I2S stopped. Underruns: %lu, Overruns: %lu",
        (unsigned long)s_underruns, (unsigned long)s_overruns);
}

uint32_t sol_i2s_underruns(void) { return s_underruns; }
uint32_t sol_i2s_overruns(void) { return s_overruns; }
