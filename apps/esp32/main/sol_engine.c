/**
 * Soluna ESP32 — Audio Engine Implementation
 * TX path: I2S RX → ring → RTP TX (+FEC)
 * RX path: RTP RX (+FEC) → ring → I2S TX
 * SPDX-License-Identifier: MIT
 */

#include "sol_engine.h"
#include "sol_i2s.h"
#include "sol_rtp.h"
#include "sol_fec.h"
#include "sol_clock.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include <string.h>

static const char* TAG = "sol_engine";

/* Static buffers (no malloc in audio path).
 * Use SOL_RING_CHANNELS (2) for allocation to fit in ESP32 SRAM.
 * With PSRAM, increase SOL_RING_FRAMES toward SOL_RING_CAPACITY_UNIFIED. */
static int32_t s_tx_ring_buf[SOL_RING_FRAMES * SOL_RING_CHANNELS];
static int32_t s_rx_ring_buf[SOL_RING_FRAMES * SOL_RING_CHANNELS];
static sol_ring_t s_tx_ring;
static sol_ring_t s_rx_ring;

static sol_rtp_tx_t s_rtp_tx;
static sol_rtp_rx_t s_rtp_rx;
static sol_fec_enc_t s_fec_enc;
static sol_fec_dec_t s_fec_dec;

static const sol_config_t* s_config = NULL;
static sol_stats_t* s_stats = NULL;
static volatile bool s_running = false;
static TaskHandle_t s_net_tx_task = NULL;
static TaskHandle_t s_net_rx_task = NULL;

/* Network TX task: reads from ring buffer → RTP TX */
static void net_tx_task(void* arg) {
    int32_t audio[SOL_FRAME_SIZE_SAMPLES * SOL_MAX_CHANNELS];
    int64_t next_ns = sol_clock_now_ns();
    const int64_t interval_ns = (int64_t)SOL_FRAME_SIZE_SAMPLES * 1000000000LL / SOL_SAMPLE_RATE;

    ESP_LOGI(TAG, "Net TX task started (interval=%lldus)", (long long)(interval_ns / 1000));

    while (s_running) {
        next_ns += interval_ns;

        if (sol_ring_read(&s_tx_ring, audio, SOL_FRAME_SIZE_SAMPLES)) {
            sol_err_t err = sol_rtp_tx_send(&s_rtp_tx, audio);
            if (err == SOL_OK) {
                s_stats->packets_tx++;

                /* FEC: feed to encoder */
                if (s_config->fec_enabled) {
                    bool parity_ready = sol_fec_enc_feed(&s_fec_enc,
                        (const uint8_t*)audio,
                        SOL_FRAME_SIZE_SAMPLES * s_config->channels * sizeof(int32_t));
                    if (parity_ready) {
                        /* Send parity as an extra RTP packet with special stream_id */
                        size_t plen;
                        const uint8_t* parity = sol_fec_enc_get_parity(&s_fec_enc, &plen);
                        /* Reuse RTP TX with different payload (parity data) */
                        sol_rtp_tx_send(&s_rtp_tx, (const int32_t*)parity);
                    }
                }
            }
        } else {
            s_stats->underruns++;
        }

        sol_clock_sleep_until_ns(next_ns);
    }

    vTaskDelete(NULL);
}

/* Network RX task: RTP RX → ring buffer */
static void net_rx_task(void* arg) {
    int32_t audio[SOL_FRAME_SIZE_SAMPLES * SOL_MAX_CHANNELS];
    uint8_t fec_group_idx = 0;

    ESP_LOGI(TAG, "Net RX task started");

    while (s_running) {
        sol_err_t err = sol_rtp_rx_recv(&s_rtp_rx, audio, 10);

        if (err == SOL_OK) {
            s_stats->packets_rx++;

            /* FEC decode */
            if (s_config->fec_enabled) {
                sol_fec_dec_feed_data(&s_fec_dec, fec_group_idx,
                    (const uint8_t*)audio,
                    SOL_FRAME_SIZE_SAMPLES * s_config->channels * sizeof(int32_t));
                fec_group_idx++;

                if (fec_group_idx >= SOL_FEC_GROUP_SIZE) {
                    /* Try recovery */
                    uint8_t recovered_idx;
                    const uint8_t* recovered_data;
                    size_t recovered_len;
                    if (sol_fec_dec_recover(&s_fec_dec, &recovered_idx,
                                             &recovered_data, &recovered_len)) {
                        /* Write recovered packet to ring */
                        sol_ring_write(&s_rx_ring, (const int32_t*)recovered_data,
                                       SOL_FRAME_SIZE_SAMPLES);
                        s_stats->fec_recovered++;
                    }

                    sol_fec_dec_init(&s_fec_dec);
                    fec_group_idx = 0;
                }
            }

            /* Write to ring buffer */
            if (!sol_ring_write(&s_rx_ring, audio, SOL_FRAME_SIZE_SAMPLES)) {
                s_stats->overruns++;
            }
        } else if (err == SOL_ERR_TIMEOUT) {
            /* Check for packet loss */
            s_stats->packets_lost = s_rtp_rx.packets_lost;
        }
    }

    vTaskDelete(NULL);
}

sol_err_t sol_engine_init(const sol_config_t* config, sol_stats_t* stats) {
    s_config = config;
    s_stats = stats;
    memset(stats, 0, sizeof(*stats));

    /* Initialize ring buffers */
    sol_ring_init(&s_tx_ring, s_tx_ring_buf, SOL_RING_FRAMES, config->channels);
    sol_ring_init(&s_rx_ring, s_rx_ring_buf, SOL_RING_FRAMES, config->channels);

    /* Initialize I2S */
    sol_i2s_config_t i2s_cfg = {
        .bck_pin = 26,
        .ws_pin = 25,
        .data_out_pin = 22,
        .data_in_pin = 23,
        .channels = config->channels,
        .sample_rate = SOL_SAMPLE_RATE,
        .dma_buf_count = 4,
        .dma_buf_len = SOL_FRAME_SIZE_SAMPLES,
    };

    sol_err_t err = sol_i2s_init(&i2s_cfg);
    if (err != SOL_OK) {
        ESP_LOGE(TAG, "I2S init failed");
        return err;
    }

    /* Initialize RTP */
    if (config->mode == SOL_MODE_TX || config->mode == SOL_MODE_TXRX) {
        err = sol_rtp_tx_init(&s_rtp_tx, SOL_MCAST_AUDIO,
                               config->rtp_port, config->ssrc, config->channels);
        if (err != SOL_OK) return err;
    }

    if (config->mode == SOL_MODE_RX || config->mode == SOL_MODE_TXRX) {
        err = sol_rtp_rx_init(&s_rtp_rx, config->rtp_port, config->channels);
        if (err != SOL_OK) return err;
    }

    /* Initialize FEC */
    if (config->fec_enabled) {
        sol_fec_enc_init(&s_fec_enc);
        sol_fec_dec_init(&s_fec_dec);
    }

    ESP_LOGI(TAG, "Engine initialized: mode=%d ch=%u fec=%u",
        config->mode, config->channels, config->fec_enabled);

    return SOL_OK;
}

sol_err_t sol_engine_start(void) {
    s_running = true;

    /* Start I2S depending on mode */
    if (s_config->mode == SOL_MODE_TX || s_config->mode == SOL_MODE_TXRX) {
        sol_i2s_start_rx(&s_tx_ring);  /* I2S RX = audio capture → tx_ring */
        xTaskCreatePinnedToCore(net_tx_task, "sol_net_tx",
            SOL_STACK_NET, NULL, configMAX_PRIORITIES - 2,
            &s_net_tx_task, SOL_CORE_NET);
    }

    if (s_config->mode == SOL_MODE_RX || s_config->mode == SOL_MODE_TXRX) {
        sol_i2s_start_tx(&s_rx_ring);  /* I2S TX = rx_ring → audio playback */
        xTaskCreatePinnedToCore(net_rx_task, "sol_net_rx",
            SOL_STACK_NET, NULL, configMAX_PRIORITIES - 2,
            &s_net_rx_task, SOL_CORE_NET);
    }

    ESP_LOGI(TAG, "Engine started");
    return SOL_OK;
}

void sol_engine_stop(void) {
    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(50));

    sol_i2s_stop();

    if (s_config->mode == SOL_MODE_TX || s_config->mode == SOL_MODE_TXRX) {
        sol_rtp_tx_close(&s_rtp_tx);
    }
    if (s_config->mode == SOL_MODE_RX || s_config->mode == SOL_MODE_TXRX) {
        sol_rtp_rx_close(&s_rtp_rx);
    }

    ESP_LOGI(TAG, "Engine stopped. TX:%llu RX:%llu Lost:%llu FEC:%llu Under:%lu Over:%lu",
        (unsigned long long)s_stats->packets_tx,
        (unsigned long long)s_stats->packets_rx,
        (unsigned long long)s_stats->packets_lost,
        (unsigned long long)s_stats->fec_recovered,
        (unsigned long)s_stats->underruns,
        (unsigned long)s_stats->overruns);
}
