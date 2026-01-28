/**
 * Soluna ESP32 — XOR FEC Implementation
 * Zero-allocation: all buffers are statically sized
 * SPDX-License-Identifier: MIT
 */

#include "sol_fec.h"
#include <string.h>

void sol_fec_enc_init(sol_fec_enc_t* enc) {
    memset(enc, 0, sizeof(*enc));
}

bool sol_fec_enc_feed(sol_fec_enc_t* enc, const uint8_t* data, size_t len) {
    if (enc->count == 0) {
        /* First packet in group: copy as initial parity */
        memcpy(enc->parity, data, len);
        enc->parity_size = len;
    } else {
        /* XOR with accumulated parity */
        size_t xor_len = (len < enc->parity_size) ? len : enc->parity_size;
        for (size_t i = 0; i < xor_len; i++) {
            enc->parity[i] ^= data[i];
        }
        /* If this packet is longer, extend parity */
        if (len > enc->parity_size) {
            memcpy(enc->parity + enc->parity_size, data + enc->parity_size,
                   len - enc->parity_size);
            enc->parity_size = len;
        }
    }

    enc->count++;

    if (enc->count >= SOL_FEC_GROUP_SIZE) {
        enc->group_id++;
        enc->count = 0;
        return true;  /* Parity ready */
    }
    return false;
}

const uint8_t* sol_fec_enc_get_parity(const sol_fec_enc_t* enc, size_t* len) {
    *len = enc->parity_size;
    return enc->parity;
}

uint32_t sol_fec_enc_group_id(const sol_fec_enc_t* enc) {
    return enc->group_id;
}

void sol_fec_dec_init(sol_fec_dec_t* dec) {
    memset(dec, 0, sizeof(*dec));
}

void sol_fec_dec_feed_data(sol_fec_dec_t* dec, uint8_t index,
                            const uint8_t* data, size_t len) {
    if (index >= SOL_FEC_GROUP_SIZE) return;
    if (dec->received[index]) return;

    memcpy(dec->packets[index], data, len);
    dec->packet_sizes[index] = len;
    dec->received[index] = true;
    dec->received_count++;
}

void sol_fec_dec_feed_parity(sol_fec_dec_t* dec,
                              const uint8_t* data, size_t len) {
    memcpy(dec->parity, data, len);
    dec->parity_size = len;
    dec->has_parity = true;
}

bool sol_fec_dec_recover(sol_fec_dec_t* dec, uint8_t* recovered_index,
                          const uint8_t** recovered_data, size_t* recovered_len) {
    /* Need exactly one missing + parity */
    if (!dec->has_parity) return false;
    if (dec->received_count != SOL_FEC_GROUP_SIZE - 1) return false;

    /* Find missing index */
    uint8_t missing = 0xFF;
    for (uint8_t i = 0; i < SOL_FEC_GROUP_SIZE; i++) {
        if (!dec->received[i]) {
            missing = i;
            break;
        }
    }
    if (missing == 0xFF) return false;

    /* Recover: XOR parity with all received packets */
    size_t max_len = dec->parity_size;
    memcpy(dec->packets[missing], dec->parity, max_len);

    for (uint8_t i = 0; i < SOL_FEC_GROUP_SIZE; i++) {
        if (i == missing) continue;
        size_t xor_len = (dec->packet_sizes[i] < max_len) ?
                          dec->packet_sizes[i] : max_len;
        for (size_t j = 0; j < xor_len; j++) {
            dec->packets[missing][j] ^= dec->packets[i][j];
        }
    }

    dec->packet_sizes[missing] = max_len;
    dec->received[missing] = true;
    dec->received_count++;

    *recovered_index = missing;
    *recovered_data = dec->packets[missing];
    *recovered_len = max_len;
    return true;
}
