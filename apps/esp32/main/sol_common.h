/**
 * Soluna ESP32 — Common definitions
 * C-only header (no exceptions, no dynamic allocation in audio path)
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#define SOL_VERSION_MAJOR 0
#define SOL_VERSION_MINOR 1
#define SOL_VERSION_PATCH 0

/* Audio parameters */
#define SOL_SAMPLE_RATE      48000
#define SOL_BIT_DEPTH        24
#define SOL_MAX_CHANNELS     8
#define SOL_FRAME_SIZE_SAMPLES 96   /* 2ms at 48kHz */
#define SOL_FRAME_SIZE_BYTES (SOL_FRAME_SIZE_SAMPLES * sizeof(int32_t))

/* Network */
#define SOL_MCAST_AUDIO     "239.69.0.1"
#define SOL_MCAST_PTP       "224.0.1.129"
#define SOL_MCAST_MDNS      "224.0.0.251"
#define SOL_PORT_RTP         5004
#define SOL_PORT_PTP_EVENT   319
#define SOL_PORT_PTP_GENERAL 320
#define SOL_PORT_CONTROL     8401
#define SOL_PORT_MDNS        5353

/* RTP */
#define SOL_RTP_HEADER_SIZE  12
#define SOL_OSTP_EXT_SIZE    8
#define SOL_MAX_PACKET_SIZE  1500
#define SOL_RTP_PT_PCM24     96
#define SOL_SSRC_DEFAULT     0x534F4C41  /* "SOLA" */

/* FEC */
#define SOL_FEC_GROUP_SIZE   5

/* Ring buffer — unified capacity is 192000 frames (4s @ 48kHz).
 * ESP32 internal SRAM cannot hold that; use 19200 (400ms) as a
 * practical compromise.  Enable CONFIG_SPIRAM=y and set
 * SOL_RING_FRAMES=192000 when PSRAM is available.                       */
#define SOL_RING_CAPACITY_UNIFIED 192000          /* other platforms     */
#define SOL_RING_FRAMES           4800           /* ESP32 SRAM budget   */
#define SOL_RING_CHANNELS         2               /* static-alloc width  */

/* Drift-correction / jitter-buffer parameters (unified across platforms) */
#define SOL_TARGET_FILL_FRAMES    2880            /* 60 ms @ 48 kHz      */
#define SOL_DRIFT_TRIGGER_MULT    3               /* discard when > target*3 */
#define SOL_DRIFT_DIVISOR         80              /* frame_count/80+1 per cb */
#define SOL_DRIFT_XFADE_LEN       48              /* crossfade after discard */

/* Fade / sample-scale constants (float, matching iOS/Mac/Linux/Win)     */
#define SOL_FADE_IN               0.002f
#define SOL_FADE_OUT              0.004f
#define SOL_SAMPLE_SCALE          8388608.0f      /* 2^23                */

/* Task pinning: WiFi/Net on core 0, Audio on core 1 */
#define SOL_CORE_NET   0
#define SOL_CORE_AUDIO 1

/* Memory budget */
#define SOL_STACK_AUDIO   8192  /* increased for drift-correction float math */
#define SOL_STACK_NET     4096
#define SOL_STACK_PTP     3072
#define SOL_STACK_CTRL    3072

/* Error codes */
typedef enum {
    SOL_OK = 0,
    SOL_ERR_INIT = -1,
    SOL_ERR_IO = -2,
    SOL_ERR_TIMEOUT = -3,
    SOL_ERR_PARAM = -4,
    SOL_ERR_FULL = -5,
    SOL_ERR_EMPTY = -6,
} sol_err_t;

/* Device mode */
typedef enum {
    SOL_MODE_TX = 0,    /* Capture → network */
    SOL_MODE_RX = 1,    /* Network → playback */
    SOL_MODE_TXRX = 2,  /* Both directions */
} sol_mode_t;

/* Device configuration (stored in NVS) */
typedef struct {
    sol_mode_t mode;
    uint8_t channels;
    uint16_t rtp_port;
    uint32_t ssrc;
    char device_name[32];
    char wifi_ssid[33];
    char wifi_pass[65];
    uint8_t fec_enabled;
    float target_latency_ms;
} sol_config_t;

/* Runtime statistics */
typedef struct {
    uint64_t packets_tx;
    uint64_t packets_rx;
    uint64_t packets_lost;
    uint64_t fec_recovered;
    uint32_t underruns;
    uint32_t overruns;
    int64_t ptp_offset_ns;
    float jitter_ms;
    uint32_t free_heap;
    uint32_t uptime_sec;
} sol_stats_t;

/* Simple lock-free SPSC ring buffer (fixed-size frames) */
typedef struct {
    int32_t* buffer;
    uint32_t capacity;    /* in frames */
    uint32_t frame_size;  /* samples per frame (= channels) */
    volatile uint32_t read_pos;
    volatile uint32_t write_pos;
} sol_ring_t;

/* Playback state for drift correction & fade (used by I2S TX task) */
typedef struct {
    bool     prefilled;          /* initial prefill completed?           */
    float    ramp;               /* current fade envelope [0..vol]       */
    uint32_t drift_xfade;        /* crossfade countdown after discard    */
    float    held_sample[SOL_MAX_CHANNELS]; /* last output sample per ch */
    uint32_t target_fill_frames; /* target ring-buffer fill level        */
} sol_playback_state_t;

static inline void sol_ring_init(sol_ring_t* r, int32_t* buf,
                                  uint32_t capacity, uint32_t frame_size) {
    r->buffer = buf;
    r->capacity = capacity;
    r->frame_size = frame_size;
    r->read_pos = 0;
    r->write_pos = 0;
}

static inline uint32_t sol_ring_available(const sol_ring_t* r) {
    uint32_t w = r->write_pos;
    uint32_t rd = r->read_pos;
    return (w >= rd) ? (w - rd) : (r->capacity - rd + w);
}

static inline uint32_t sol_ring_free(const sol_ring_t* r) {
    return r->capacity - 1 - sol_ring_available(r);
}

static inline bool sol_ring_write(sol_ring_t* r, const int32_t* data, uint32_t frames) {
    if (sol_ring_free(r) < frames) return false;
    uint32_t samples = frames * r->frame_size;
    uint32_t wp = r->write_pos;
    uint32_t wp_samples = wp * r->frame_size;
    uint32_t cap_samples = r->capacity * r->frame_size;

    for (uint32_t i = 0; i < samples; i++) {
        r->buffer[(wp_samples + i) % cap_samples] = data[i];
    }
    __sync_synchronize();
    r->write_pos = (wp + frames) % r->capacity;
    return true;
}

static inline void sol_ring_discard(sol_ring_t* r, uint32_t frames) {
    uint32_t avail = sol_ring_available(r);
    if (frames > avail) frames = avail;
    __sync_synchronize();
    r->read_pos = (r->read_pos + frames) % r->capacity;
}

static inline bool sol_ring_read(sol_ring_t* r, int32_t* data, uint32_t frames) {
    if (sol_ring_available(r) < frames) return false;
    uint32_t samples = frames * r->frame_size;
    uint32_t rp = r->read_pos;
    uint32_t rp_samples = rp * r->frame_size;
    uint32_t cap_samples = r->capacity * r->frame_size;

    for (uint32_t i = 0; i < samples; i++) {
        data[i] = r->buffer[(rp_samples + i) % cap_samples];
    }
    __sync_synchronize();
    r->read_pos = (rp + frames) % r->capacity;
    return true;
}
