/**
 * Soluna ESP32 — XOR Parity FEC (lightweight)
 * Single-loss recovery within a group of SOL_FEC_GROUP_SIZE packets
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "sol_common.h"

/* FEC encoder: accumulates XOR parity over a group */
typedef struct {
    uint8_t parity[SOL_FRAME_SIZE_SAMPLES * SOL_MAX_CHANNELS * 4];
    size_t parity_size;
    uint8_t count;       /* Packets accumulated in current group */
    uint32_t group_id;   /* Current group number */
} sol_fec_enc_t;

/* FEC decoder: recovers lost packets */
typedef struct {
    uint8_t packets[SOL_FEC_GROUP_SIZE][SOL_FRAME_SIZE_SAMPLES * SOL_MAX_CHANNELS * 4];
    size_t packet_sizes[SOL_FEC_GROUP_SIZE];
    bool received[SOL_FEC_GROUP_SIZE];
    uint8_t parity[SOL_FRAME_SIZE_SAMPLES * SOL_MAX_CHANNELS * 4];
    size_t parity_size;
    bool has_parity;
    uint8_t received_count;
} sol_fec_dec_t;

/** Initialize FEC encoder */
void sol_fec_enc_init(sol_fec_enc_t* enc);

/**
 * Feed a data packet to encoder.
 * When group is complete, parity is available via sol_fec_enc_get_parity().
 * Returns true when a parity packet is ready.
 */
bool sol_fec_enc_feed(sol_fec_enc_t* enc, const uint8_t* data, size_t len);

/** Get parity data (valid after sol_fec_enc_feed returns true) */
const uint8_t* sol_fec_enc_get_parity(const sol_fec_enc_t* enc, size_t* len);

/** Get current group ID */
uint32_t sol_fec_enc_group_id(const sol_fec_enc_t* enc);

/** Initialize FEC decoder for one group */
void sol_fec_dec_init(sol_fec_dec_t* dec);

/** Feed a data packet at index within group */
void sol_fec_dec_feed_data(sol_fec_dec_t* dec, uint8_t index,
                            const uint8_t* data, size_t len);

/** Feed parity packet */
void sol_fec_dec_feed_parity(sol_fec_dec_t* dec,
                              const uint8_t* data, size_t len);

/**
 * Try to recover a lost packet.
 * Returns true if exactly one packet was lost and recovered.
 * recovered_index: which index was recovered
 * recovered_data: pointer to recovered data (internal buffer)
 * recovered_len: size of recovered data
 */
bool sol_fec_dec_recover(sol_fec_dec_t* dec, uint8_t* recovered_index,
                          const uint8_t** recovered_data, size_t* recovered_len);
