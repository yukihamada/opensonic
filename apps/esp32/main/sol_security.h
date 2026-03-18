/**
 * Soluna ESP32 — SipHash Auth + Replay Prevention (§20-21)
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* SipHash-2-4: returns 64-bit hash */
uint64_t sol_siphash_2_4(const uint8_t key[16],
                          const uint8_t *msg, size_t len);

/* Verify packet: last 4 bytes = SipHash tag */
bool sol_packet_verify(const uint8_t key[16],
                       const uint8_t *pkt, size_t pkt_len);

/* Replay detection state */
typedef struct {
    uint16_t last_seq;
    uint32_t window;
    bool initialized;
} sol_replay_state_t;

void sol_replay_init(sol_replay_state_t *state);
bool sol_replay_check(sol_replay_state_t *state, uint16_t seq);
