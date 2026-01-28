/**
 * Soluna ESP32 — PTP Follower Implementation
 * Receives Sync/Follow_Up, sends Delay_Req, processes Delay_Resp
 * Uses PI controller for clock servo
 * SPDX-License-Identifier: MIT
 */

#include "sol_ptp_follower.h"
#include "sol_net.h"
#include "sol_clock.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include <string.h>

static const char* TAG = "sol_ptp";

/* PTP message types */
#define PTP_MSG_SYNC        0x00
#define PTP_MSG_DELAY_REQ   0x01
#define PTP_MSG_FOLLOW_UP   0x08
#define PTP_MSG_DELAY_RESP  0x09

/* PTP header size */
#define PTP_HEADER_SIZE     34

/* PI controller gains */
#define PTP_KP  0.5
#define PTP_KI  0.05

/* State */
static volatile sol_ptp_state_t s_state = {0};
static volatile bool s_running = false;
static TaskHandle_t s_task = NULL;
static sol_socket_t s_event_sock;
static sol_socket_t s_general_sock;

/* Servo state */
static double s_integral = 0.0;
static int64_t s_corrected_offset = 0;

/* Timestamps for delay measurement */
static int64_t s_t1 = 0;  /* Sync origin timestamp (from master) */
static int64_t s_t2 = 0;  /* Sync receive timestamp (local) */
static int64_t s_t3 = 0;  /* Delay_Req transmit timestamp (local) */
static int64_t s_t4 = 0;  /* Delay_Req receive timestamp (from master) */
static uint16_t s_seq = 0;

static int64_t ptp_timestamp_to_ns(const uint8_t* data) {
    /* PTP timestamp: 6 bytes seconds + 4 bytes nanoseconds */
    int64_t sec = 0;
    for (int i = 0; i < 6; i++) {
        sec = (sec << 8) | data[i];
    }
    uint32_t nsec = ((uint32_t)data[6] << 24) | ((uint32_t)data[7] << 16) |
                    ((uint32_t)data[8] << 8) | data[9];
    return sec * 1000000000LL + nsec;
}

static void build_delay_req(uint8_t* buf, uint16_t seq) {
    memset(buf, 0, PTP_HEADER_SIZE);
    buf[0] = PTP_MSG_DELAY_REQ | 0x20;  /* Version 2 */
    buf[1] = 0x02;                        /* PTP version */
    buf[2] = 0x00;                        /* Message length high */
    buf[3] = PTP_HEADER_SIZE + 10;        /* Message length low */
    buf[30] = (uint8_t)(seq >> 8);
    buf[31] = (uint8_t)(seq & 0xFF);
}

static void update_servo(int64_t offset_ns) {
    double err = (double)offset_ns;
    s_integral += err * PTP_KI;

    /* Clamp integral */
    if (s_integral > 1e9) s_integral = 1e9;
    if (s_integral < -1e9) s_integral = -1e9;

    double correction = err * PTP_KP + s_integral;
    s_corrected_offset = (int64_t)correction;

    s_state.offset_ns = offset_ns;
}

static void ptp_task(void* arg) {
    uint8_t buf[128];
    char src_ip[16];
    uint16_t src_port;
    bool have_sync = false;
    uint16_t delay_req_seq = 0;

    while (s_running) {
        /* Listen on event port for Sync */
        int n = sol_socket_recvfrom(&s_event_sock, buf, sizeof(buf),
                                     src_ip, sizeof(src_ip), &src_port, 500);
        if (n > PTP_HEADER_SIZE) {
            uint8_t msg_type = buf[0] & 0x0F;

            if (msg_type == PTP_MSG_SYNC) {
                s_t2 = sol_clock_now_ns();
                have_sync = true;
                s_state.sync_count++;
            }
        }

        /* Listen on general port for Follow_Up and Delay_Resp */
        n = sol_socket_recvfrom(&s_general_sock, buf, sizeof(buf),
                                 src_ip, sizeof(src_ip), &src_port, 100);
        if (n > PTP_HEADER_SIZE) {
            uint8_t msg_type = buf[0] & 0x0F;

            if (msg_type == PTP_MSG_FOLLOW_UP && have_sync) {
                /* Extract precise origin timestamp */
                s_t1 = ptp_timestamp_to_ns(buf + PTP_HEADER_SIZE);

                /* Send Delay_Req */
                uint8_t dreq[PTP_HEADER_SIZE + 10];
                build_delay_req(dreq, delay_req_seq++);
                s_t3 = sol_clock_now_ns();
                sol_socket_sendto(&s_event_sock, dreq, sizeof(dreq),
                                  src_ip, SOL_PORT_PTP_EVENT);

                have_sync = false;
            } else if (msg_type == PTP_MSG_DELAY_RESP) {
                s_t4 = ptp_timestamp_to_ns(buf + PTP_HEADER_SIZE);

                /* Compute offset and delay */
                /* offset = ((t2 - t1) - (t4 - t3)) / 2 */
                /* delay  = ((t2 - t1) + (t4 - t3)) / 2 */
                int64_t d1 = s_t2 - s_t1;
                int64_t d2 = s_t4 - s_t3;
                int64_t offset = (d1 - d2) / 2;
                int64_t delay = (d1 + d2) / 2;

                s_state.path_delay_ns = delay;
                s_state.synced = true;

                update_servo(offset);

                if (s_state.sync_count % 10 == 0) {
                    ESP_LOGI(TAG, "PTP offset=%lldns delay=%lldns syncs=%lu",
                        (long long)offset, (long long)delay,
                        (unsigned long)s_state.sync_count);
                }
            }
        }
    }

    vTaskDelete(NULL);
}

sol_err_t sol_ptp_init(void) {
    sol_err_t err;

    err = sol_socket_create(&s_event_sock);
    if (err != SOL_OK) return err;
    err = sol_socket_bind(&s_event_sock, SOL_PORT_PTP_EVENT);
    if (err != SOL_OK) return err;
    err = sol_socket_join_mcast(&s_event_sock, SOL_MCAST_PTP);
    if (err != SOL_OK) return err;

    err = sol_socket_create(&s_general_sock);
    if (err != SOL_OK) return err;
    err = sol_socket_bind(&s_general_sock, SOL_PORT_PTP_GENERAL);
    if (err != SOL_OK) return err;
    err = sol_socket_join_mcast(&s_general_sock, SOL_MCAST_PTP);
    if (err != SOL_OK) return err;

    memset((void*)&s_state, 0, sizeof(s_state));
    s_integral = 0.0;
    s_corrected_offset = 0;
    s_running = true;

    xTaskCreatePinnedToCore(ptp_task, "sol_ptp", SOL_STACK_PTP,
        NULL, 5, &s_task, SOL_CORE_NET);

    ESP_LOGI(TAG, "PTP follower started on core %d", SOL_CORE_NET);
    return SOL_OK;
}

void sol_ptp_stop(void) {
    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(600));
    sol_socket_close(&s_event_sock);
    sol_socket_close(&s_general_sock);
    ESP_LOGI(TAG, "PTP follower stopped. Syncs: %lu",
        (unsigned long)s_state.sync_count);
}

sol_ptp_state_t sol_ptp_get_state(void) {
    sol_ptp_state_t copy;
    memcpy(&copy, (void*)&s_state, sizeof(copy));
    return copy;
}

int64_t sol_ptp_now_ns(void) {
    return sol_clock_now_ns() - s_corrected_offset;
}
