/**
 * Soluna ESP32 — Audio Engine Implementation
 * TX path: I2S RX → ring → RTP TX (+FEC)
 * RX path: RTP RX (+FEC) → ring → I2S TX
 * NACK: 16-slot cache on TX; gap-detect + NACK request on RX
 * RTCP: 5-second text report via UDP
 * SPDX-License-Identifier: MIT
 */

#include "sol_engine.h"
#include "sol_i2s.h"
#include "sol_rtp.h"
#include "sol_fec.h"
#include "sol_clock.h"
#include <fcntl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include <string.h>
#include <inttypes.h>
#include <stdio.h>
#include <arpa/inet.h>

static const char* TAG = "sol_engine";

/* -------------------------------------------------------------------------
 * NACK cache — 16 slots, circular, stores last 16 sent RTP packets.
 * RAM budget: 16 * (1500 + 4 + 2) ≈ 24 KB  (acceptable on ESP32)
 * ---------------------------------------------------------------------- */
#define SOL_NACK_CACHE_SIZE  16
#define SOL_NACK_PORT_OFFSET 1   /* NACK socket = rtp_port + 1 */
#define SOL_NACK_MAGIC_0     0x4E
#define SOL_NACK_MAGIC_1     0x41

typedef struct {
    uint8_t  data[SOL_MAX_PACKET_SIZE];
    uint16_t size;
    uint32_t seq;  /* RTP sequence (16-bit seq stored as 32-bit for easy lookup) */
} sol_nack_slot_t;

typedef struct {
    sol_nack_slot_t slots[SOL_NACK_CACHE_SIZE];
    uint8_t         head;  /* index of next slot to overwrite */
} sol_nack_cache_t;

static sol_nack_cache_t s_nack_cache;

/** Store a copy of the just-sent packet in the NACK cache. */
static void nack_cache_store(const uint8_t* pkt, uint16_t len, uint32_t seq)
{
    sol_nack_slot_t* slot = &s_nack_cache.slots[s_nack_cache.head];
    uint16_t copy_len = (len > SOL_MAX_PACKET_SIZE) ? SOL_MAX_PACKET_SIZE : len;
    memcpy(slot->data, pkt, copy_len);
    slot->size = copy_len;
    slot->seq  = seq;
    s_nack_cache.head = (s_nack_cache.head + 1) % SOL_NACK_CACHE_SIZE;
}

/** Look up a sequence number in the cache.  Returns slot ptr or NULL. */
static const sol_nack_slot_t* nack_cache_find(uint32_t seq)
{
    for (int i = 0; i < SOL_NACK_CACHE_SIZE; i++) {
        if (s_nack_cache.slots[i].size > 0 && s_nack_cache.slots[i].seq == seq) {
            return &s_nack_cache.slots[i];
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Static state
 * ---------------------------------------------------------------------- */
static int32_t s_tx_ring_buf[SOL_RING_FRAMES * SOL_RING_CHANNELS];
static int32_t s_rx_ring_buf[SOL_RING_FRAMES * SOL_RING_CHANNELS];
static sol_ring_t s_tx_ring;
static sol_ring_t s_rx_ring;

static sol_rtp_tx_t s_rtp_tx;
static sol_rtp_rx_t s_rtp_rx;
static sol_fec_enc_t s_fec_enc;
static sol_fec_dec_t s_fec_dec;

static const sol_config_t* s_config = NULL;
static sol_stats_t*        s_stats  = NULL;
static volatile bool       s_running = false;
static TaskHandle_t s_net_tx_task   = NULL;
static TaskHandle_t s_net_rx_task   = NULL;
static TaskHandle_t s_nack_task     = NULL;
static TaskHandle_t s_rtcp_task_h   = NULL;

/* NACK UDP socket (bound to rtp_port+1) */
static int s_nack_sock = -1;

/* Sender IP/port (filled when first RTP packet arrives) */
static struct sockaddr_in s_sender_addr;
static bool               s_sender_known = false;

/* -------------------------------------------------------------------------
 * Internal: build and send a FEC parity RTP packet
 * Header:  RTP (12) + OSTP ext (8) + parity payload + CRC32
 * PT=127, stream_id upper nibble = 0xF (FEC marker)
 * ---------------------------------------------------------------------- */
static uint32_t s_crc32_table[256];
static bool     s_crc32_ready = false;

static void crc32_init(void)
{
    if (s_crc32_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        s_crc32_table[i] = c;
    }
    s_crc32_ready = true;
}

static uint32_t crc32_compute(const uint8_t* data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) crc = s_crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void send_fec_packet(sol_rtp_tx_t* tx, const uint8_t* parity, size_t plen)
{
    /* Total: 12 (RTP) + 4 (ext hdr) + 8 (OSTP ext) + plen + 4 (CRC) */
    size_t total = 12 + 4 + 8 + plen + 4;
    if (total > SOL_MAX_PACKET_SIZE) return;

    uint8_t buf[SOL_MAX_PACKET_SIZE];
    memset(buf, 0, total);

    /* RTP header */
    buf[0]  = 0x90;            /* V=2, P=0, X=1, CC=0 */
    buf[1]  = 127;             /* M=0, PT=127 (FEC) */
    buf[2]  = (tx->seq >> 8) & 0xFF;
    buf[3]  = tx->seq & 0xFF;
    tx->seq++;
    uint32_t ts = tx->timestamp;
    buf[4]  = (ts >> 24) & 0xFF;
    buf[5]  = (ts >> 16) & 0xFF;
    buf[6]  = (ts >>  8) & 0xFF;
    buf[7]  =  ts        & 0xFF;
    uint32_t ssrc = tx->ssrc;
    buf[8]  = (ssrc >> 24) & 0xFF;
    buf[9]  = (ssrc >> 16) & 0xFF;
    buf[10] = (ssrc >>  8) & 0xFF;
    buf[11] =  ssrc        & 0xFF;

    /* RTP extension header: profile 'OS', length=2 (2 x 32-bit words = 8 bytes) */
    buf[12] = 0x4F; buf[13] = 0x53; /* 'OS' */
    buf[14] = 0x00; buf[15] = 0x02; /* length = 2 words */

    /* OSTP extension: stream_id=0xFF00 (FEC channel), seq_ext=group_id, media_ts */
    uint32_t gid = sol_fec_enc_group_id(&s_fec_enc);
    buf[16] = 0xFF; buf[17] = 0x00; /* stream_id: deck=0xF, ch=0 = FEC */
    buf[18] = (gid >> 8) & 0xFF;
    buf[19] =  gid       & 0xFF;
    buf[20] = (ts >> 24) & 0xFF;
    buf[21] = (ts >> 16) & 0xFF;
    buf[22] = (ts >>  8) & 0xFF;
    buf[23] =  ts        & 0xFF;

    /* Parity payload */
    memcpy(buf + 24, parity, plen);

    /* CRC32 over header + payload */
    uint32_t crc = crc32_compute(buf, 24 + plen);
    buf[24 + plen + 0] = (crc >> 24) & 0xFF;
    buf[24 + plen + 1] = (crc >> 16) & 0xFF;
    buf[24 + plen + 2] = (crc >>  8) & 0xFF;
    buf[24 + plen + 3] =  crc        & 0xFF;

    /* Transmit via existing TX socket */
    struct sockaddr_in dest = {0};
    dest.sin_family      = AF_INET;
    dest.sin_port        = htons(tx->dest_port);
    inet_aton(tx->dest_ip, &dest.sin_addr);

    sendto(tx->sock.fd, buf, (int)total, 0,
           (struct sockaddr*)&dest, sizeof(dest));
}

/* -------------------------------------------------------------------------
 * Network TX task: I2S ring → RTP TX (+FEC parity) + NACK cache store
 * ---------------------------------------------------------------------- */
static void net_tx_task(void* arg)
{
    int32_t audio[SOL_FRAME_SIZE_SAMPLES * SOL_MAX_CHANNELS];
    int64_t next_ns = sol_clock_now_ns();
    const int64_t interval_ns =
        (int64_t)SOL_FRAME_SIZE_SAMPLES * 1000000000LL / SOL_SAMPLE_RATE;

    ESP_LOGI(TAG, "Net TX task started (interval=%lldus)",
             (long long)(interval_ns / 1000));

    crc32_init();

    while (s_running) {
        next_ns += interval_ns;

        if (sol_ring_read(&s_tx_ring, audio, SOL_FRAME_SIZE_SAMPLES)) {
            sol_err_t err = sol_rtp_tx_send(&s_rtp_tx, audio);
            if (err == SOL_OK) {
                s_stats->packets_tx++;

                /* Store last-sent packet in NACK cache.
                 * seq was already incremented by sol_rtp_tx_send; roll back by 1
                 * to get the sequence of the packet we just sent. */
                uint16_t sent_seq = (uint16_t)(s_rtp_tx.seq - 1);

                /* Reconstruct the on-wire bytes from audio buffer.
                 * We approximate the packet by reading tx context fields.
                 * For simplicity we store the raw audio as payload — the NACK
                 * retransmit path calls sol_rtp_tx_send again with those samples,
                 * so here we just cache audio + seq for lookup. */
                nack_cache_store((const uint8_t*)audio,
                    (uint16_t)(SOL_FRAME_SIZE_SAMPLES *
                               s_config->channels * sizeof(int32_t)),
                    sent_seq);

                /* FEC: feed to encoder */
                if (s_config->fec_enabled) {
                    bool parity_ready = sol_fec_enc_feed(
                        &s_fec_enc,
                        (const uint8_t*)audio,
                        SOL_FRAME_SIZE_SAMPLES * s_config->channels * sizeof(int32_t));

                    if (parity_ready) {
                        size_t plen;
                        const uint8_t* parity = sol_fec_enc_get_parity(&s_fec_enc, &plen);
                        /* Send a properly formatted FEC RTP packet (PT=127) */
                        send_fec_packet(&s_rtp_tx, parity, plen);
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

/* -------------------------------------------------------------------------
 * Network RX task: RTP RX (+FEC) → ring buffer + gap-detect NACK
 * ---------------------------------------------------------------------- */
static void net_rx_task(void* arg)
{
    int32_t audio[SOL_FRAME_SIZE_SAMPLES * SOL_MAX_CHANNELS];
    uint8_t fec_group_idx = 0;

    ESP_LOGI(TAG, "Net RX task started");

    while (s_running) {
        sol_err_t err = sol_rtp_rx_recv(&s_rtp_rx, audio, 10);

        if (err == SOL_OK) {
            s_stats->packets_rx++;

            /* Gap detection → send NACK to sender */
            if (s_rtp_rx.got_first && s_sender_known) {
                uint16_t expected = (uint16_t)(s_rtp_rx.last_seq);
                /* sol_rtp_rx_recv already updated last_seq; the previous last_seq
                 * was the sequence before the current packet.  We detect a gap
                 * if the loss counter increased. */
                if (s_rtp_rx.packets_lost > s_stats->packets_lost) {
                    /* Build NACK request: magic 0x4E 0x41 + one uint32 seq */
                    uint8_t nack_buf[6];
                    nack_buf[0] = SOL_NACK_MAGIC_0;
                    nack_buf[1] = SOL_NACK_MAGIC_1;
                    uint32_t lost_seq = (uint32_t)((expected - 1) & 0xFFFF);
                    nack_buf[2] = (lost_seq >> 24) & 0xFF;
                    nack_buf[3] = (lost_seq >> 16) & 0xFF;
                    nack_buf[4] = (lost_seq >>  8) & 0xFF;
                    nack_buf[5] =  lost_seq        & 0xFF;

                    struct sockaddr_in nack_dest = s_sender_addr;
                    nack_dest.sin_port = htons(
                        ntohs(s_sender_addr.sin_port) + SOL_NACK_PORT_OFFSET);
                    sendto(s_rtp_rx.sock.fd, nack_buf, sizeof(nack_buf), 0,
                           (struct sockaddr*)&nack_dest, sizeof(nack_dest));
                }
            }

            /* Update stats loss tracking */
            s_stats->packets_lost = s_rtp_rx.packets_lost;

            /* FEC decode */
            if (s_config->fec_enabled) {
                sol_fec_dec_feed_data(&s_fec_dec, fec_group_idx,
                    (const uint8_t*)audio,
                    SOL_FRAME_SIZE_SAMPLES * s_config->channels * sizeof(int32_t));
                fec_group_idx++;

                if (fec_group_idx >= SOL_FEC_GROUP_SIZE) {
                    uint8_t recovered_idx;
                    const uint8_t* recovered_data;
                    size_t recovered_len;
                    if (sol_fec_dec_recover(&s_fec_dec, &recovered_idx,
                                             &recovered_data, &recovered_len)) {
                        sol_ring_write(&s_rx_ring,
                                       (const int32_t*)recovered_data,
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
            s_stats->packets_lost = s_rtp_rx.packets_lost;
        }
    }

    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * NACK service task: listens on rtp_port+1, retransmits from cache
 * Packet format: 0x4E 0x41 [uint32 seq] [uint32 seq] ...
 * ---------------------------------------------------------------------- */
static void nack_service_task(void* arg)
{
    uint8_t buf[256];
    struct sockaddr_in from;
    socklen_t from_len;

    ESP_LOGI(TAG, "NACK service task started (port=%u)",
             s_config->rtp_port + SOL_NACK_PORT_OFFSET);

    while (s_running) {
        from_len = sizeof(from);
        int n = recvfrom(s_nack_sock, buf, sizeof(buf), 0,
                         (struct sockaddr*)&from, &from_len);
        if (n < 2) { vTaskDelay(pdMS_TO_TICKS(1)); continue; }

        if (buf[0] != SOL_NACK_MAGIC_0 || buf[1] != SOL_NACK_MAGIC_1) continue;

        int offset = 2;
        while (offset + 4 <= n) {
            uint32_t seq = ((uint32_t)buf[offset]   << 24) |
                           ((uint32_t)buf[offset+1] << 16) |
                           ((uint32_t)buf[offset+2] <<  8) |
                            (uint32_t)buf[offset+3];
            offset += 4;

            const sol_nack_slot_t* slot = nack_cache_find(seq & 0xFFFF);
            if (slot && slot->size > 0) {
                /* Retransmit cached payload as a new RTP send */
                sol_rtp_tx_send(&s_rtp_tx, (const int32_t*)slot->data);
                ESP_LOGD(TAG, "NACK retransmit seq=%u", (unsigned)seq);
            }
        }
    }

    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * RTCP report task: sends text report every 5 seconds
 * Format: REPORT:<ssrc>:<rx>:<lost>:<loss_pct>:<jitter_ms>:<last_seq>\n
 * ---------------------------------------------------------------------- */
static void sol_rtcp_task(void* arg)
{
    char report[128];
    struct sockaddr_in relay_addr = {0};
    relay_addr.sin_family = AF_INET;
    relay_addr.sin_port   = htons(s_config->rtp_port);
    inet_aton(SOL_MCAST_AUDIO, &relay_addr.sin_addr);

    ESP_LOGI(TAG, "RTCP task started");

    while (s_running) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        uint64_t rx   = s_stats->packets_rx;
        uint64_t lost = s_stats->packets_lost;
        uint64_t total = rx + lost;
        uint32_t loss_pct = (total > 0) ? (uint32_t)((lost * 100) / total) : 0;

        int len = snprintf(report, sizeof(report),
            "REPORT:%08lX:%llu:%llu:%u:%u:%u\n",
            (unsigned long)s_config->ssrc,
            (unsigned long long)rx,
            (unsigned long long)lost,
            loss_pct,
            (unsigned)(s_stats->jitter_ms * 1000.0f + 0.5f),
            (unsigned)(s_rtp_rx.last_seq));

        if (len > 0) {
            sendto(s_rtp_tx.sock.fd, report, len, 0,
                   (struct sockaddr*)&relay_addr, sizeof(relay_addr));
            ESP_LOGD(TAG, "RTCP: %s", report);
        }
    }

    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

sol_err_t sol_engine_init(const sol_config_t* config, sol_stats_t* stats)
{
    s_config = config;
    s_stats  = stats;
    memset(stats, 0, sizeof(*stats));
    memset(&s_nack_cache, 0, sizeof(s_nack_cache));
    memset(&s_sender_addr, 0, sizeof(s_sender_addr));
    s_sender_known = false;

    /* Initialize ring buffers */
    sol_ring_init(&s_tx_ring, s_tx_ring_buf, SOL_RING_FRAMES, config->channels);
    sol_ring_init(&s_rx_ring, s_rx_ring_buf, SOL_RING_FRAMES, config->channels);

    /* Initialize I2S */
    sol_i2s_config_t i2s_cfg = {
        .bck_pin      = 26,
        .ws_pin       = 25,
        .data_out_pin = 22,
        .data_in_pin  = 23,
        .channels     = config->channels,
        .sample_rate  = SOL_SAMPLE_RATE,
        .dma_buf_count = 4,
        .dma_buf_len  = SOL_FRAME_SIZE_SAMPLES,
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

    /* NACK service socket bound to rtp_port+1 */
    if (config->mode == SOL_MODE_TX || config->mode == SOL_MODE_TXRX) {
        s_nack_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_nack_sock >= 0) {
            struct sockaddr_in nack_bind = {0};
            nack_bind.sin_family      = AF_INET;
            nack_bind.sin_port        = htons(config->rtp_port + SOL_NACK_PORT_OFFSET);
            nack_bind.sin_addr.s_addr = INADDR_ANY;

            /* Non-blocking so the NACK task can check s_running */
            int flags = fcntl(s_nack_sock, F_GETFL, 0);
            fcntl(s_nack_sock, F_SETFL, flags | O_NONBLOCK);

            if (bind(s_nack_sock, (struct sockaddr*)&nack_bind,
                     sizeof(nack_bind)) < 0) {
                ESP_LOGW(TAG, "NACK socket bind failed");
                close(s_nack_sock);
                s_nack_sock = -1;
            }
        }
    }

    /* Initialize FEC */
    if (config->fec_enabled) {
        sol_fec_enc_init(&s_fec_enc);
        sol_fec_dec_init(&s_fec_dec);
    }

    crc32_init();

    ESP_LOGI(TAG, "Engine initialized: mode=%d ch=%" PRIu32 " fec=%" PRIu32 "",
             config->mode, config->channels, config->fec_enabled);

    return SOL_OK;
}

sol_err_t sol_engine_start(void)
{
    s_running = true;

    if (s_config->mode == SOL_MODE_TX || s_config->mode == SOL_MODE_TXRX) {
        sol_i2s_start_rx(&s_tx_ring);
        xTaskCreatePinnedToCore(net_tx_task, "sol_net_tx",
            SOL_STACK_NET, NULL, configMAX_PRIORITIES - 2,
            &s_net_tx_task, SOL_CORE_NET);

        /* NACK service task */
        if (s_nack_sock >= 0) {
            xTaskCreatePinnedToCore(nack_service_task, "sol_nack",
                SOL_STACK_NET, NULL, configMAX_PRIORITIES - 3,
                &s_nack_task, SOL_CORE_NET);
        }
    }

    if (s_config->mode == SOL_MODE_RX || s_config->mode == SOL_MODE_TXRX) {
        sol_i2s_start_tx(&s_rx_ring);
        xTaskCreatePinnedToCore(net_rx_task, "sol_net_rx",
            SOL_STACK_NET, NULL, configMAX_PRIORITIES - 2,
            &s_net_rx_task, SOL_CORE_NET);
    }

    /* RTCP task (both TX and RX modes benefit from reporting) */
    xTaskCreatePinnedToCore(sol_rtcp_task, "sol_rtcp",
        SOL_STACK_CTRL, NULL, configMAX_PRIORITIES - 4,
        &s_rtcp_task_h, SOL_CORE_NET);

    ESP_LOGI(TAG, "Engine started");
    return SOL_OK;
}

void sol_engine_stop(void)
{
    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(50));

    sol_i2s_stop();

    if (s_config->mode == SOL_MODE_TX || s_config->mode == SOL_MODE_TXRX) {
        sol_rtp_tx_close(&s_rtp_tx);
        if (s_nack_sock >= 0) {
            close(s_nack_sock);
            s_nack_sock = -1;
        }
    }
    if (s_config->mode == SOL_MODE_RX || s_config->mode == SOL_MODE_TXRX) {
        sol_rtp_rx_close(&s_rtp_rx);
    }

    ESP_LOGI(TAG,
        "Engine stopped. TX:%llu RX:%llu Lost:%llu FEC:%llu Under:%lu Over:%lu",
        (unsigned long long)s_stats->packets_tx,
        (unsigned long long)s_stats->packets_rx,
        (unsigned long long)s_stats->packets_lost,
        (unsigned long long)s_stats->fec_recovered,
        (unsigned long)s_stats->underruns,
        (unsigned long)s_stats->overruns);
}
