/**
 * Soluna ESP32 — SipHash-2-4 Authentication + Replay Prevention (§20-21)
 * SPDX-License-Identifier: MIT
 */

#include "sol_security.h"
#include <string.h>

/* ── SipHash-2-4 (RFC 7693) ── */

#define ROTL64(x, b) (((x) << (b)) | ((x) >> (64 - (b))))

#define SIPROUND \
    v0 += v1; v1 = ROTL64(v1, 13); v1 ^= v0; v0 = ROTL64(v0, 32); \
    v2 += v3; v3 = ROTL64(v3, 16); v3 ^= v2; \
    v0 += v3; v3 = ROTL64(v3, 21); v3 ^= v0; \
    v2 += v1; v1 = ROTL64(v1, 17); v1 ^= v2; v2 = ROTL64(v2, 32);

uint64_t sol_siphash_2_4(const uint8_t key[16],
                          const uint8_t *msg, size_t len) {
    uint64_t k0, k1;
    memcpy(&k0, key, 8);
    memcpy(&k1, key + 8, 8);

    uint64_t v0 = k0 ^ 0x736f6d6570736575ULL;
    uint64_t v1 = k1 ^ 0x646f72616e646f6dULL;
    uint64_t v2 = k0 ^ 0x6c7967656e657261ULL;
    uint64_t v3 = k1 ^ 0x7465646279746573ULL;

    const uint8_t *end = msg + len - (len % 8);
    for (; msg < end; msg += 8) {
        uint64_t m;
        memcpy(&m, msg, 8);
        v3 ^= m;
        SIPROUND; SIPROUND;
        v0 ^= m;
    }

    uint64_t b = ((uint64_t)len) << 56;
    switch (len & 7) {
        case 7: b |= ((uint64_t)msg[6]) << 48; /* fall through */
        case 6: b |= ((uint64_t)msg[5]) << 40; /* fall through */
        case 5: b |= ((uint64_t)msg[4]) << 32; /* fall through */
        case 4: b |= ((uint64_t)msg[3]) << 24; /* fall through */
        case 3: b |= ((uint64_t)msg[2]) << 16; /* fall through */
        case 2: b |= ((uint64_t)msg[1]) << 8;  /* fall through */
        case 1: b |= ((uint64_t)msg[0]);        break;
    }
    v3 ^= b;
    SIPROUND; SIPROUND;
    v0 ^= b;
    v2 ^= 0xFF;
    SIPROUND; SIPROUND; SIPROUND; SIPROUND;

    return v0 ^ v1 ^ v2 ^ v3;
}

/* ── Packet Authentication ── */

bool sol_packet_verify(const uint8_t key[16],
                       const uint8_t *pkt, size_t pkt_len) {
    if (pkt_len < 4) return false;

    /* Last 4 bytes are SipHash tag */
    uint64_t hash = sol_siphash_2_4(key, pkt, pkt_len - 4);
    uint32_t expected;
    memcpy(&expected, pkt + pkt_len - 4, 4);
    return (uint32_t)hash == expected;
}

/* ── Replay Prevention (Sliding Window) ── */

void sol_replay_init(sol_replay_state_t *state) {
    state->last_seq = 0;
    state->window = 0;
    state->initialized = false;
}

bool sol_replay_check(sol_replay_state_t *state, uint16_t seq) {
    if (!state->initialized) {
        state->last_seq = seq;
        state->window = 1;
        state->initialized = true;
        return true;  /* accept first packet */
    }

    int32_t diff = (int32_t)(uint16_t)(seq - state->last_seq);
    /* Handle 16-bit wraparound */
    if (diff > 32767) diff -= 65536;

    if (diff > 0) {
        /* New packet — advance window */
        if (diff >= 32) state->window = 0;
        else            state->window <<= diff;
        state->window |= 1;
        state->last_seq = seq;
        return true;
    } else if (diff >= -31) {
        /* Late but within window */
        uint32_t bit = 1u << (uint32_t)(-diff);
        if (state->window & bit) return false;  /* duplicate/replay */
        state->window |= bit;
        return true;
    }
    return false;  /* too old — replay attack */
}
