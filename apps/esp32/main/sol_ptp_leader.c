#include "esp_mac.h"
/**
 * Soluna ESP32 — PTP Leader Mode Implementation
 *
 * Simplified PTPv2 grandmaster for standalone ESP32 networks.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sol_ptp_leader.h"

#ifdef ESP_PLATFORM

#include <esp_log.h>
#include <esp_timer.h>
#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

static const char* TAG = "sol_ptp_leader";

/**
 * PTP message types.
 */
#define PTP_MSG_SYNC         0x00
#define PTP_MSG_DELAY_REQ    0x01
#define PTP_MSG_FOLLOW_UP    0x08
#define PTP_MSG_DELAY_RESP   0x09
#define PTP_MSG_ANNOUNCE     0x0B

/**
 * PTP header (34 bytes).
 */
typedef struct __attribute__((packed)) {
    uint8_t  msg_type;          // [0]
    uint8_t  version;           // [1]
    uint16_t msg_length;        // [2-3]
    uint8_t  domain;            // [4]
    uint8_t  reserved1;         // [5]
    uint16_t flags;             // [6-7]
    int64_t  correction;        // [8-15]
    uint32_t reserved2;         // [16-19]
    uint8_t  source_port_id[10];// [20-29]
    uint16_t sequence_id;       // [30-31]
    uint8_t  control;           // [32]
    int8_t   log_msg_interval;  // [33]
} ptp_header_t;

/**
 * PTP timestamp (10 bytes).
 */
typedef struct __attribute__((packed)) {
    uint16_t seconds_hi;
    uint32_t seconds_lo;
    uint32_t nanoseconds;
} ptp_timestamp_t;

/**
 * PTP Sync message (34 + 10 = 44 bytes).
 */
typedef struct __attribute__((packed)) {
    ptp_header_t header;
    ptp_timestamp_t origin_timestamp;
} ptp_sync_t;

/**
 * PTP Announce message (34 + 30 = 64 bytes).
 */
typedef struct __attribute__((packed)) {
    ptp_header_t header;
    ptp_timestamp_t origin_timestamp;
    int16_t  current_utc_offset;
    uint8_t  reserved;
    uint8_t  grandmaster_priority1;
    uint32_t grandmaster_clock_quality;
    uint8_t  grandmaster_priority2;
    uint8_t  grandmaster_identity[8];
    uint16_t steps_removed;
    uint8_t  time_source;
} ptp_announce_t;

/**
 * PTP Delay_Resp message.
 */
typedef struct __attribute__((packed)) {
    ptp_header_t header;
    ptp_timestamp_t receive_timestamp;
    uint8_t  requesting_port_id[10];
} ptp_delay_resp_t;

static struct {
    sol_ptp_leader_config_t config;
    sol_ptp_leader_stats_t stats;
    int event_socket;
    int general_socket;
    TaskHandle_t task_handle;
    bool running;
    uint16_t sequence_id;
    uint8_t clock_id[8];
} s_leader = {0};

static int64_t get_ptp_time_ns(void) {
    // Use ESP32's high-resolution timer
    // In production, this would use a hardware timestamping unit
    return esp_timer_get_time() * 1000LL;  // microseconds to nanoseconds
}

static void fill_timestamp(ptp_timestamp_t* ts, int64_t time_ns) {
    int64_t sec = time_ns / 1000000000LL;
    uint32_t nsec = (uint32_t)(time_ns % 1000000000LL);
    ts->seconds_hi = htons((uint16_t)(sec >> 32));
    ts->seconds_lo = htonl((uint32_t)sec);
    ts->nanoseconds = htonl(nsec);
}

static void fill_header(ptp_header_t* h, uint8_t msg_type, uint16_t length) {
    memset(h, 0, sizeof(*h));
    h->msg_type = msg_type;
    h->version = 2;  // PTPv2
    h->msg_length = htons(length);
    h->domain = s_leader.config.domain;
    h->flags = htons(0x0200);  // Two-step flag
    memcpy(h->source_port_id, s_leader.clock_id, 8);
    h->source_port_id[8] = 0;
    h->source_port_id[9] = 1;  // Port number
    h->sequence_id = htons(s_leader.sequence_id++);
}

static void send_announce(void) {
    ptp_announce_t msg = {0};
    fill_header(&msg.header, PTP_MSG_ANNOUNCE, sizeof(msg));
    msg.header.control = 5;
    msg.header.log_msg_interval = 1;  // 2 seconds

    fill_timestamp(&msg.origin_timestamp, get_ptp_time_ns());
    msg.grandmaster_priority1 = s_leader.config.priority1;
    msg.grandmaster_priority2 = s_leader.config.priority2;
    msg.grandmaster_clock_quality = htonl(0xFE01FFFE);  // Default quality
    memcpy(msg.grandmaster_identity, s_leader.clock_id, 8);
    msg.time_source = 0xA0;  // Internal oscillator

    struct sockaddr_in mcast_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(SOL_PORT_PTP_GENERAL),
    };
    inet_pton(AF_INET, SOL_MCAST_PTP, &mcast_addr.sin_addr);

    sendto(s_leader.general_socket, &msg, sizeof(msg), 0,
           (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));

    s_leader.stats.announce_sent++;
}

static void send_sync(void) {
    int64_t t1 = get_ptp_time_ns();

    // Send Sync
    ptp_sync_t sync = {0};
    fill_header(&sync.header, PTP_MSG_SYNC, sizeof(sync));
    sync.header.control = 0;
    sync.header.log_msg_interval = -3;  // 125ms
    fill_timestamp(&sync.origin_timestamp, 0);  // Will be in Follow_Up

    struct sockaddr_in mcast_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(SOL_PORT_PTP_EVENT),
    };
    inet_pton(AF_INET, SOL_MCAST_PTP, &mcast_addr.sin_addr);

    sendto(s_leader.event_socket, &sync, sizeof(sync), 0,
           (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));

    // Send Follow_Up with actual timestamp
    ptp_sync_t follow_up = {0};
    fill_header(&follow_up.header, PTP_MSG_FOLLOW_UP, sizeof(follow_up));
    follow_up.header.control = 2;
    follow_up.header.log_msg_interval = -3;
    follow_up.header.sequence_id = sync.header.sequence_id;  // Same sequence
    fill_timestamp(&follow_up.origin_timestamp, t1);

    mcast_addr.sin_port = htons(SOL_PORT_PTP_GENERAL);
    sendto(s_leader.general_socket, &follow_up, sizeof(follow_up), 0,
           (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));

    s_leader.stats.sync_sent++;
}

static void handle_delay_req(const ptp_header_t* req, struct sockaddr_in* from_addr) {
    int64_t t4 = get_ptp_time_ns();

    ptp_delay_resp_t resp = {0};
    fill_header(&resp.header, PTP_MSG_DELAY_RESP, sizeof(resp));
    resp.header.control = 3;
    resp.header.sequence_id = req->sequence_id;  // Echo sequence
    fill_timestamp(&resp.receive_timestamp, t4);
    memcpy(resp.requesting_port_id, req->source_port_id, 10);

    // Send unicast response
    from_addr->sin_port = htons(SOL_PORT_PTP_GENERAL);
    sendto(s_leader.general_socket, &resp, sizeof(resp), 0,
           (struct sockaddr*)from_addr, sizeof(*from_addr));

    s_leader.stats.delay_req_received++;
}

static void ptp_leader_task(void* arg) {
    (void)arg;
    uint8_t buf[128];
    struct sockaddr_in from_addr;
    socklen_t from_len;

    uint32_t last_announce_ms = 0;
    uint32_t last_sync_ms = 0;

    while (s_leader.running) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        // Send periodic Announce
        if (now_ms - last_announce_ms >= s_leader.config.announce_interval) {
            send_announce();
            last_announce_ms = now_ms;
        }

        // Send periodic Sync
        if (now_ms - last_sync_ms >= s_leader.config.sync_interval) {
            send_sync();
            last_sync_ms = now_ms;
        }

        // Receive Delay_Req messages
        struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };  // 50ms timeout
        setsockopt(s_leader.event_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        from_len = sizeof(from_addr);
        int len = recvfrom(s_leader.event_socket, buf, sizeof(buf), 0,
                           (struct sockaddr*)&from_addr, &from_len);
        if (len >= (int)sizeof(ptp_header_t)) {
            ptp_header_t* header = (ptp_header_t*)buf;
            if ((header->msg_type & 0x0F) == PTP_MSG_DELAY_REQ) {
                handle_delay_req(header, &from_addr);
            }
        }
    }

    close(s_leader.event_socket);
    close(s_leader.general_socket);
    s_leader.event_socket = -1;
    s_leader.general_socket = -1;
    vTaskDelete(NULL);
}

int sol_ptp_leader_init(const sol_ptp_leader_config_t* config) {
    if (config) {
        memcpy(&s_leader.config, config, sizeof(sol_ptp_leader_config_t));
    } else {
        s_leader.config.priority1 = 128;
        s_leader.config.priority2 = 128;
        s_leader.config.domain = 0;
        s_leader.config.announce_interval = 1000;
        s_leader.config.sync_interval = 125;
    }

    // Generate clock ID from MAC address
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    s_leader.clock_id[0] = mac[0];
    s_leader.clock_id[1] = mac[1];
    s_leader.clock_id[2] = mac[2];
    s_leader.clock_id[3] = 0xFF;
    s_leader.clock_id[4] = 0xFE;
    s_leader.clock_id[5] = mac[3];
    s_leader.clock_id[6] = mac[4];
    s_leader.clock_id[7] = mac[5];

    memset(&s_leader.stats, 0, sizeof(s_leader.stats));
    s_leader.running = false;

    ESP_LOGI(TAG, "PTP leader initialized (priority1=%d, priority2=%d)",
             s_leader.config.priority1, s_leader.config.priority2);

    return 0;
}

void sol_ptp_leader_shutdown(void) {
    sol_ptp_leader_stop();
    memset(&s_leader, 0, sizeof(s_leader));
}

int sol_ptp_leader_start(void) {
    if (s_leader.running) {
        return 0;
    }

    // Create event socket (port 319)
    s_leader.event_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_leader.event_socket < 0) {
        ESP_LOGE(TAG, "Failed to create event socket");
        return -1;
    }

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(SOL_PORT_PTP_EVENT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(s_leader.event_socket, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind event socket");
        close(s_leader.event_socket);
        return -1;
    }

    // Join PTP multicast group
    struct ip_mreq mreq = {0};
    inet_pton(AF_INET, SOL_MCAST_PTP, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(s_leader.event_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    // Create general socket (port 320)
    s_leader.general_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_leader.general_socket < 0) {
        ESP_LOGE(TAG, "Failed to create general socket");
        close(s_leader.event_socket);
        return -1;
    }

    bind_addr.sin_port = htons(SOL_PORT_PTP_GENERAL);
    if (bind(s_leader.general_socket, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind general socket");
        close(s_leader.event_socket);
        close(s_leader.general_socket);
        return -1;
    }

    setsockopt(s_leader.general_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    s_leader.running = true;
    s_leader.sequence_id = 0;

    xTaskCreatePinnedToCore(ptp_leader_task, "ptp_leader", 4096, NULL, 6, &s_leader.task_handle, SOL_CORE_NET);

    ESP_LOGI(TAG, "PTP leader started");
    return 0;
}

void sol_ptp_leader_stop(void) {
    if (!s_leader.running) {
        return;
    }

    s_leader.running = false;

    if (s_leader.task_handle) {
        vTaskDelay(pdMS_TO_TICKS(200));
        s_leader.task_handle = NULL;
    }

    ESP_LOGI(TAG, "PTP leader stopped");
}

bool sol_ptp_leader_is_running(void) {
    return s_leader.running;
}

void sol_ptp_leader_get_stats(sol_ptp_leader_stats_t* stats) {
    memcpy(stats, &s_leader.stats, sizeof(sol_ptp_leader_stats_t));
}

int64_t sol_ptp_leader_get_time(void) {
    return get_ptp_time_ns();
}

#else
// Stub implementations for non-ESP32 builds

int sol_ptp_leader_init(const sol_ptp_leader_config_t* config) { (void)config; return 0; }
void sol_ptp_leader_shutdown(void) {}
int sol_ptp_leader_start(void) { return 0; }
void sol_ptp_leader_stop(void) {}
bool sol_ptp_leader_is_running(void) { return false; }
void sol_ptp_leader_get_stats(sol_ptp_leader_stats_t* stats) { (void)stats; }
int64_t sol_ptp_leader_get_time(void) { return 0; }

#endif
