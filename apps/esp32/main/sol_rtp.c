/**
 * Soluna ESP32 — RTP TX/RX Implementation
 * RTP + 8-byte OSTP extension header
 * SPDX-License-Identifier: MIT
 */

#include "sol_rtp.h"
#include "sol_clock.h"

#include "esp_log.h"
#include "lwip/sockets.h"
#include <string.h>

static const char* TAG = "sol_rtp";

/* Build RTP+OSTP packet into buffer. Returns total packet size. */
static size_t build_packet(uint8_t* pkt, size_t pkt_size,
                            uint32_t ssrc, uint16_t seq, uint32_t ts,
                            uint16_t stream_id, uint16_t seq_ext,
                            uint32_t media_ts,
                            const void* payload, size_t payload_size) {
    size_t total = SOL_RTP_HEADER_SIZE + SOL_OSTP_EXT_SIZE + payload_size;
    if (total > pkt_size) return 0;

    /* RTP header */
    pkt[0] = 0x90;  /* V=2, P=0, X=1, CC=0 */
    pkt[1] = SOL_RTP_PT_PCM24 & 0x7F;
    pkt[2] = (uint8_t)(seq >> 8);
    pkt[3] = (uint8_t)(seq & 0xFF);
    pkt[4] = (uint8_t)(ts >> 24);
    pkt[5] = (uint8_t)(ts >> 16);
    pkt[6] = (uint8_t)(ts >> 8);
    pkt[7] = (uint8_t)(ts & 0xFF);
    pkt[8]  = (uint8_t)(ssrc >> 24);
    pkt[9]  = (uint8_t)(ssrc >> 16);
    pkt[10] = (uint8_t)(ssrc >> 8);
    pkt[11] = (uint8_t)(ssrc & 0xFF);

    /* OSTP extension header (8 bytes) */
    uint8_t* ext = pkt + SOL_RTP_HEADER_SIZE;
    ext[0] = (uint8_t)(stream_id >> 8);
    ext[1] = (uint8_t)(stream_id & 0xFF);
    ext[2] = (uint8_t)(seq_ext >> 8);
    ext[3] = (uint8_t)(seq_ext & 0xFF);
    ext[4] = (uint8_t)(media_ts >> 24);
    ext[5] = (uint8_t)(media_ts >> 16);
    ext[6] = (uint8_t)(media_ts >> 8);
    ext[7] = (uint8_t)(media_ts & 0xFF);

    /* Payload */
    memcpy(pkt + SOL_RTP_HEADER_SIZE + SOL_OSTP_EXT_SIZE, payload, payload_size);

    return total;
}

/* Parse RTP+OSTP packet. Returns payload pointer and size. */
static bool parse_packet(const uint8_t* pkt, size_t pkt_size,
                          uint32_t* ssrc, uint16_t* seq,
                          const uint8_t** payload, size_t* payload_size) {
    if (pkt_size < SOL_RTP_HEADER_SIZE + SOL_OSTP_EXT_SIZE) return false;

    uint8_t version = (pkt[0] >> 6) & 0x03;
    if (version != 2) return false;

    *seq = ((uint16_t)pkt[2] << 8) | pkt[3];
    *ssrc = ((uint32_t)pkt[8] << 24) | ((uint32_t)pkt[9] << 16) |
            ((uint32_t)pkt[10] << 8) | pkt[11];

    *payload = pkt + SOL_RTP_HEADER_SIZE + SOL_OSTP_EXT_SIZE;
    *payload_size = pkt_size - SOL_RTP_HEADER_SIZE - SOL_OSTP_EXT_SIZE;
    return true;
}

sol_err_t sol_rtp_tx_init(sol_rtp_tx_t* tx, const char* dest_ip,
                           uint16_t port, uint32_t ssrc, uint8_t channels) {
    memset(tx, 0, sizeof(*tx));
    strncpy(tx->dest_ip, dest_ip, sizeof(tx->dest_ip) - 1);
    tx->dest_port = port;
    tx->ssrc = ssrc;
    tx->channels = channels;
    tx->stream_id = 1;

    sol_err_t err = sol_socket_create(&tx->sock);
    if (err != SOL_OK) return err;

    /* Set DSCP for audio traffic (EF = 46) */
    int tos = 46 << 2;
    setsockopt(tx->sock.fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

    ESP_LOGI(TAG, "RTP TX init: %s:%u ssrc=0x%08lx ch=%u",
        dest_ip, port, (unsigned long)ssrc, channels);
    return SOL_OK;
}

sol_err_t sol_rtp_tx_send(sol_rtp_tx_t* tx, const int32_t* audio) {
    uint8_t pkt[SOL_MAX_PACKET_SIZE];
    size_t payload_size = SOL_FRAME_SIZE_SAMPLES * tx->channels * sizeof(int32_t);

    uint32_t full_seq = ((uint32_t)tx->seq_ext << 16) | tx->seq;
    size_t pkt_size = build_packet(pkt, sizeof(pkt),
        tx->ssrc, tx->seq, tx->timestamp,
        tx->stream_id, tx->seq_ext, tx->media_ts,
        audio, payload_size);

    if (pkt_size == 0) return SOL_ERR_PARAM;

    int sent = sol_socket_sendto(&tx->sock, pkt, pkt_size,
                                  tx->dest_ip, tx->dest_port);
    if (sent < 0) return SOL_ERR_IO;

    /* Advance sequence */
    tx->seq++;
    if (tx->seq == 0) tx->seq_ext++;
    tx->timestamp += SOL_FRAME_SIZE_SAMPLES;
    tx->media_ts += (uint32_t)((uint64_t)SOL_FRAME_SIZE_SAMPLES * 1000000000ULL / SOL_SAMPLE_RATE);
    tx->packets_sent++;

    return SOL_OK;
}

sol_err_t sol_rtp_rx_init(sol_rtp_rx_t* rx, uint16_t port, uint8_t channels) {
    memset(rx, 0, sizeof(*rx));
    rx->channels = channels;

    sol_err_t err = sol_socket_create(&rx->sock);
    if (err != SOL_OK) return err;

    err = sol_socket_bind(&rx->sock, port);
    if (err != SOL_OK) return err;

    err = sol_socket_join_mcast(&rx->sock, SOL_MCAST_AUDIO);
    if (err != SOL_OK) return err;

    ESP_LOGI(TAG, "RTP RX init: port=%u ch=%u", port, channels);
    return SOL_OK;
}

sol_err_t sol_rtp_rx_recv(sol_rtp_rx_t* rx, int32_t* audio, uint32_t timeout_ms) {
    uint8_t pkt[SOL_MAX_PACKET_SIZE];

    int n = sol_socket_recvfrom(&rx->sock, pkt, sizeof(pkt),
                                 NULL, 0, NULL, timeout_ms);
    if (n <= 0) return SOL_ERR_TIMEOUT;

    uint32_t ssrc;
    uint16_t seq;
    const uint8_t* payload;
    size_t payload_size;

    if (!parse_packet(pkt, (size_t)n, &ssrc, &seq, &payload, &payload_size)) {
        return SOL_ERR_IO;
    }

    /* Sequence gap detection */
    if (rx->got_first) {
        uint16_t expected = rx->last_seq + 1;
        if (seq != expected) {
            uint16_t gap = seq - expected;
            rx->packets_lost += gap;
        }
    }
    rx->last_seq = seq;
    rx->got_first = true;
    rx->packets_received++;

    /* Copy audio payload */
    size_t expected_size = SOL_FRAME_SIZE_SAMPLES * rx->channels * sizeof(int32_t);
    if (payload_size >= expected_size) {
        memcpy(audio, payload, expected_size);
    } else {
        memcpy(audio, payload, payload_size);
        memset((uint8_t*)audio + payload_size, 0, expected_size - payload_size);
    }

    return SOL_OK;
}

void sol_rtp_tx_close(sol_rtp_tx_t* tx) {
    sol_socket_close(&tx->sock);
    ESP_LOGI(TAG, "RTP TX closed. Packets: %llu", (unsigned long long)tx->packets_sent);
}

void sol_rtp_rx_close(sol_rtp_rx_t* rx) {
    sol_socket_close(&rx->sock);
    ESP_LOGI(TAG, "RTP RX closed. Packets: %llu Lost: %llu",
        (unsigned long long)rx->packets_received,
        (unsigned long long)rx->packets_lost);
}
