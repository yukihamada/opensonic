/**
 * Soluna ESP32 — RTP TX/RX
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "sol_common.h"
#include "sol_net.h"

/* RTP TX context */
typedef struct {
    sol_socket_t sock;
    char dest_ip[16];
    uint16_t dest_port;
    uint32_t ssrc;
    uint16_t seq;
    uint32_t timestamp;
    uint16_t stream_id;
    uint16_t seq_ext;
    uint32_t media_ts;
    uint8_t channels;
    uint64_t packets_sent;
} sol_rtp_tx_t;

/* RTP RX context */
typedef struct {
    sol_socket_t sock;
    uint32_t expected_ssrc;
    uint16_t last_seq;
    bool got_first;
    uint8_t channels;
    uint64_t packets_received;
    uint64_t packets_lost;
} sol_rtp_rx_t;

/** Initialize RTP transmitter */
sol_err_t sol_rtp_tx_init(sol_rtp_tx_t* tx, const char* dest_ip,
                           uint16_t port, uint32_t ssrc, uint8_t channels);

/** Send one audio frame (SOL_FRAME_SIZE_SAMPLES of int32_t per channel) */
sol_err_t sol_rtp_tx_send(sol_rtp_tx_t* tx, const int32_t* audio);

/** Initialize RTP receiver */
sol_err_t sol_rtp_rx_init(sol_rtp_rx_t* rx, uint16_t port, uint8_t channels);

/**
 * Receive one audio frame.
 * Returns SOL_OK with audio data, SOL_ERR_TIMEOUT if no packet, SOL_ERR_IO on error.
 */
sol_err_t sol_rtp_rx_recv(sol_rtp_rx_t* rx, int32_t* audio, uint32_t timeout_ms);

/** Close TX/RX */
void sol_rtp_tx_close(sol_rtp_tx_t* tx);
void sol_rtp_rx_close(sol_rtp_rx_t* rx);
