/**
 * Soluna ESP32 — Peer-to-Peer Mode Implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include "sol_p2p.h"

#ifdef ESP_PLATFORM

#include <esp_log.h>
#include <esp_timer.h>
#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

static const char* TAG = "sol_p2p";

#define SOL_P2P_MAX_PEERS 8
#define SOL_P2P_MAGIC 0x534F4C50  // "SOLP"
#define SOL_P2P_VERSION 1
#define SOL_P2P_TIMEOUT_MS 15000

/**
 * P2P message header.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint16_t length;
} sol_p2p_header_t;

/**
 * P2P announce message.
 */
typedef struct __attribute__((packed)) {
    sol_p2p_header_t header;
    sol_p2p_device_t device;
} sol_p2p_announce_t;

static struct {
    sol_p2p_config_t config;
    sol_peer_t peers[SOL_P2P_MAX_PEERS];
    int peer_count;
    int socket_fd;
    TaskHandle_t task_handle;
    bool running;
    bool is_ptp_leader;
    uint32_t last_announce_ms;
} s_p2p = {0};

static uint32_t get_time_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void send_announce(void) {
    sol_p2p_announce_t msg = {0};
    msg.header.magic = SOL_P2P_MAGIC;
    msg.header.version = SOL_P2P_VERSION;
    msg.header.type = SOL_P2P_MSG_ANNOUNCE;
    msg.header.length = sizeof(sol_p2p_device_t);
    memcpy(&msg.device, &s_p2p.config.local_device, sizeof(sol_p2p_device_t));
    msg.device.is_ptp_leader = s_p2p.is_ptp_leader ? 1 : 0;

    struct sockaddr_in broadcast_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(s_p2p.config.discovery_port),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };

    sendto(s_p2p.socket_fd, &msg, sizeof(msg), 0,
           (struct sockaddr*)&broadcast_addr, sizeof(broadcast_addr));
}

static void handle_announce(const sol_p2p_announce_t* msg, struct sockaddr_in* from_addr) {
    const sol_p2p_device_t* device = &msg->device;

    // Ignore our own announcements
    if (device->device_id == s_p2p.config.local_device.device_id) {
        return;
    }

    // Find existing peer or add new
    int idx = -1;
    for (int i = 0; i < s_p2p.peer_count; i++) {
        if (s_p2p.peers[i].device.device_id == device->device_id) {
            idx = i;
            break;
        }
    }

    bool is_new = (idx < 0);
    if (is_new) {
        if (s_p2p.peer_count >= SOL_P2P_MAX_PEERS) {
            ESP_LOGW(TAG, "Max peers reached, ignoring new peer");
            return;
        }
        idx = s_p2p.peer_count++;
    }

    // Update peer info
    sol_peer_t* peer = &s_p2p.peers[idx];
    memcpy(&peer->device, device, sizeof(sol_p2p_device_t));
    peer->device.ip_addr = from_addr->sin_addr.s_addr;
    peer->last_seen_ms = get_time_ms();

    if (is_new) {
        peer->state = SOL_PEER_DISCONNECTED;
        peer->packets_sent = 0;
        peer->packets_received = 0;
        ESP_LOGI(TAG, "Discovered new peer: %s (ID=%08x)", device->name, device->device_id);
    }

    // Notify callback
    if (s_p2p.config.peer_cb) {
        s_p2p.config.peer_cb(device, is_new);
    }
}

static void handle_connect(const sol_p2p_header_t* header, struct sockaddr_in* from_addr) {
    // Find the requesting peer
    uint32_t from_ip = from_addr->sin_addr.s_addr;
    int idx = -1;
    for (int i = 0; i < s_p2p.peer_count; i++) {
        if (s_p2p.peers[i].device.ip_addr == from_ip) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        ESP_LOGW(TAG, "Connect request from unknown peer");
        return;
    }

    sol_peer_t* peer = &s_p2p.peers[idx];
    sol_peer_state_t old_state = peer->state;
    peer->state = SOL_PEER_CONNECTED;

    ESP_LOGI(TAG, "Peer connected: %s", peer->device.name);

    // Send accept
    sol_p2p_header_t accept = {
        .magic = SOL_P2P_MAGIC,
        .version = SOL_P2P_VERSION,
        .type = SOL_P2P_MSG_ACCEPT,
        .length = 0,
    };
    sendto(s_p2p.socket_fd, &accept, sizeof(accept), 0,
           (struct sockaddr*)from_addr, sizeof(*from_addr));

    // Notify callback
    if (s_p2p.config.state_cb) {
        s_p2p.config.state_cb(peer, old_state);
    }
}

static void handle_accept(struct sockaddr_in* from_addr) {
    uint32_t from_ip = from_addr->sin_addr.s_addr;
    for (int i = 0; i < s_p2p.peer_count; i++) {
        if (s_p2p.peers[i].device.ip_addr == from_ip &&
            s_p2p.peers[i].state == SOL_PEER_CONNECTING) {
            sol_peer_t* peer = &s_p2p.peers[i];
            sol_peer_state_t old_state = peer->state;
            peer->state = SOL_PEER_CONNECTED;

            ESP_LOGI(TAG, "Connection accepted by: %s", peer->device.name);

            if (s_p2p.config.state_cb) {
                s_p2p.config.state_cb(peer, old_state);
            }
            break;
        }
    }
}

static void handle_disconnect(struct sockaddr_in* from_addr) {
    uint32_t from_ip = from_addr->sin_addr.s_addr;
    for (int i = 0; i < s_p2p.peer_count; i++) {
        if (s_p2p.peers[i].device.ip_addr == from_ip) {
            sol_peer_t* peer = &s_p2p.peers[i];
            sol_peer_state_t old_state = peer->state;
            peer->state = SOL_PEER_DISCONNECTED;

            ESP_LOGI(TAG, "Peer disconnected: %s", peer->device.name);

            if (s_p2p.config.state_cb) {
                s_p2p.config.state_cb(peer, old_state);
            }
            break;
        }
    }
}

static void p2p_task(void* arg) {
    (void)arg;
    uint8_t buf[256];
    struct sockaddr_in from_addr;
    socklen_t from_len;

    while (s_p2p.running) {
        // Check for timeout on peers
        uint32_t now = get_time_ms();
        for (int i = 0; i < s_p2p.peer_count; i++) {
            if (now - s_p2p.peers[i].last_seen_ms > SOL_P2P_TIMEOUT_MS) {
                if (s_p2p.peers[i].state != SOL_PEER_DISCONNECTED) {
                    sol_peer_state_t old_state = s_p2p.peers[i].state;
                    s_p2p.peers[i].state = SOL_PEER_DISCONNECTED;
                    ESP_LOGW(TAG, "Peer timeout: %s", s_p2p.peers[i].device.name);

                    if (s_p2p.config.state_cb) {
                        s_p2p.config.state_cb(&s_p2p.peers[i], old_state);
                    }
                }
            }
        }

        // Send periodic announce
        if (now - s_p2p.last_announce_ms >= s_p2p.config.announce_interval_ms) {
            send_announce();
            s_p2p.last_announce_ms = now;
        }

        // Receive messages
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };  // 100ms timeout
        setsockopt(s_p2p.socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        from_len = sizeof(from_addr);
        int len = recvfrom(s_p2p.socket_fd, buf, sizeof(buf), 0,
                           (struct sockaddr*)&from_addr, &from_len);
        if (len < (int)sizeof(sol_p2p_header_t)) {
            continue;
        }

        sol_p2p_header_t* header = (sol_p2p_header_t*)buf;
        if (header->magic != SOL_P2P_MAGIC || header->version != SOL_P2P_VERSION) {
            continue;
        }

        switch (header->type) {
            case SOL_P2P_MSG_ANNOUNCE:
                if (len >= (int)sizeof(sol_p2p_announce_t)) {
                    handle_announce((sol_p2p_announce_t*)buf, &from_addr);
                }
                break;
            case SOL_P2P_MSG_CONNECT:
                handle_connect(header, &from_addr);
                break;
            case SOL_P2P_MSG_ACCEPT:
                handle_accept(&from_addr);
                break;
            case SOL_P2P_MSG_DISCONNECT:
                handle_disconnect(&from_addr);
                break;
            default:
                ESP_LOGD(TAG, "Unknown message type: %d", header->type);
                break;
        }
    }

    close(s_p2p.socket_fd);
    s_p2p.socket_fd = -1;
    vTaskDelete(NULL);
}

int sol_p2p_init(const sol_p2p_config_t* config) {
    memcpy(&s_p2p.config, config, sizeof(sol_p2p_config_t));

    if (s_p2p.config.discovery_port == 0) {
        s_p2p.config.discovery_port = 8402;
    }
    if (s_p2p.config.announce_interval_ms == 0) {
        s_p2p.config.announce_interval_ms = 5000;
    }

    s_p2p.peer_count = 0;
    s_p2p.is_ptp_leader = (config->local_device.mode == SOL_MODE_TX);
    s_p2p.running = false;

    ESP_LOGI(TAG, "P2P initialized: %s (ID=%08x, mode=%d)",
             config->local_device.name,
             config->local_device.device_id,
             config->local_device.mode);

    return 0;
}

void sol_p2p_shutdown(void) {
    sol_p2p_stop_discovery();
    memset(&s_p2p, 0, sizeof(s_p2p));
}

int sol_p2p_start_discovery(void) {
    if (s_p2p.running) {
        return 0;
    }

    // Create UDP socket
    s_p2p.socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_p2p.socket_fd < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        return -1;
    }

    // Enable broadcast
    int broadcast = 1;
    setsockopt(s_p2p.socket_fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    // Bind to discovery port
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(s_p2p.config.discovery_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(s_p2p.socket_fd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind socket");
        close(s_p2p.socket_fd);
        return -1;
    }

    s_p2p.running = true;
    s_p2p.last_announce_ms = 0;

    // Start discovery task
    xTaskCreatePinnedToCore(p2p_task, "sol_p2p", 4096, NULL, 5, &s_p2p.task_handle, SOL_CORE_NET);

    ESP_LOGI(TAG, "P2P discovery started on port %d", s_p2p.config.discovery_port);
    return 0;
}

void sol_p2p_stop_discovery(void) {
    if (!s_p2p.running) {
        return;
    }

    s_p2p.running = false;

    // Wait for task to finish
    if (s_p2p.task_handle) {
        vTaskDelay(pdMS_TO_TICKS(200));
        s_p2p.task_handle = NULL;
    }

    ESP_LOGI(TAG, "P2P discovery stopped");
}

int sol_p2p_connect(uint32_t device_id) {
    // Find peer
    int idx = -1;
    for (int i = 0; i < s_p2p.peer_count; i++) {
        if (s_p2p.peers[i].device.device_id == device_id) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        ESP_LOGE(TAG, "Unknown device ID: %08x", device_id);
        return -1;
    }

    sol_peer_t* peer = &s_p2p.peers[idx];

    if (peer->state != SOL_PEER_DISCONNECTED) {
        ESP_LOGW(TAG, "Peer already connected or connecting");
        return -1;
    }

    sol_peer_state_t old_state = peer->state;
    peer->state = SOL_PEER_CONNECTING;

    // Send connect request
    sol_p2p_header_t connect = {
        .magic = SOL_P2P_MAGIC,
        .version = SOL_P2P_VERSION,
        .type = SOL_P2P_MSG_CONNECT,
        .length = 0,
    };

    struct sockaddr_in peer_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(s_p2p.config.discovery_port),
        .sin_addr.s_addr = peer->device.ip_addr,
    };

    sendto(s_p2p.socket_fd, &connect, sizeof(connect), 0,
           (struct sockaddr*)&peer_addr, sizeof(peer_addr));

    ESP_LOGI(TAG, "Connecting to: %s", peer->device.name);

    if (s_p2p.config.state_cb) {
        s_p2p.config.state_cb(peer, old_state);
    }

    return 0;
}

void sol_p2p_disconnect(uint32_t device_id) {
    for (int i = 0; i < s_p2p.peer_count; i++) {
        if (s_p2p.peers[i].device.device_id == device_id) {
            sol_peer_t* peer = &s_p2p.peers[i];

            if (peer->state == SOL_PEER_DISCONNECTED) {
                return;
            }

            sol_peer_state_t old_state = peer->state;
            peer->state = SOL_PEER_DISCONNECTED;

            // Send disconnect
            sol_p2p_header_t disconnect = {
                .magic = SOL_P2P_MAGIC,
                .version = SOL_P2P_VERSION,
                .type = SOL_P2P_MSG_DISCONNECT,
                .length = 0,
            };

            struct sockaddr_in peer_addr = {
                .sin_family = AF_INET,
                .sin_port = htons(s_p2p.config.discovery_port),
                .sin_addr.s_addr = peer->device.ip_addr,
            };

            sendto(s_p2p.socket_fd, &disconnect, sizeof(disconnect), 0,
                   (struct sockaddr*)&peer_addr, sizeof(peer_addr));

            ESP_LOGI(TAG, "Disconnected from: %s", peer->device.name);

            if (s_p2p.config.state_cb) {
                s_p2p.config.state_cb(peer, old_state);
            }
            break;
        }
    }
}

void sol_p2p_disconnect_all(void) {
    for (int i = 0; i < s_p2p.peer_count; i++) {
        if (s_p2p.peers[i].state != SOL_PEER_DISCONNECTED) {
            sol_p2p_disconnect(s_p2p.peers[i].device.device_id);
        }
    }
}

int sol_p2p_get_peers(sol_peer_t* peers, int max_peers) {
    int count = (s_p2p.peer_count < max_peers) ? s_p2p.peer_count : max_peers;
    memcpy(peers, s_p2p.peers, count * sizeof(sol_peer_t));
    return count;
}

int sol_p2p_connected_count(void) {
    int count = 0;
    for (int i = 0; i < s_p2p.peer_count; i++) {
        if (s_p2p.peers[i].state == SOL_PEER_CONNECTED ||
            s_p2p.peers[i].state == SOL_PEER_STREAMING) {
            count++;
        }
    }
    return count;
}

bool sol_p2p_is_ptp_leader(void) {
    return s_p2p.is_ptp_leader;
}

void sol_p2p_set_ptp_leader(bool is_leader) {
    s_p2p.is_ptp_leader = is_leader;
    s_p2p.config.local_device.is_ptp_leader = is_leader ? 1 : 0;
    ESP_LOGI(TAG, "PTP leader mode: %s", is_leader ? "enabled" : "disabled");
}

#else
// Stub implementations for non-ESP32 builds

int sol_p2p_init(const sol_p2p_config_t* config) { (void)config; return 0; }
void sol_p2p_shutdown(void) {}
int sol_p2p_start_discovery(void) { return 0; }
void sol_p2p_stop_discovery(void) {}
int sol_p2p_connect(uint32_t device_id) { (void)device_id; return 0; }
void sol_p2p_disconnect(uint32_t device_id) { (void)device_id; }
void sol_p2p_disconnect_all(void) {}
int sol_p2p_get_peers(sol_peer_t* peers, int max_peers) { (void)peers; (void)max_peers; return 0; }
int sol_p2p_connected_count(void) { return 0; }
bool sol_p2p_is_ptp_leader(void) { return false; }
void sol_p2p_set_ptp_leader(bool is_leader) { (void)is_leader; }

#endif
