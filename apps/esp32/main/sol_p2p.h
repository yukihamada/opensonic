/**
 * Soluna ESP32 — Peer-to-Peer Mode
 *
 * Direct device-to-device streaming without desktop daemon.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "sol_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * P2P discovery message types.
 */
typedef enum {
    SOL_P2P_MSG_ANNOUNCE = 1,   // Device announcement
    SOL_P2P_MSG_QUERY = 2,      // Query for devices
    SOL_P2P_MSG_RESPONSE = 3,   // Response to query
    SOL_P2P_MSG_CONNECT = 4,    // Connection request
    SOL_P2P_MSG_ACCEPT = 5,     // Connection accepted
    SOL_P2P_MSG_REJECT = 6,     // Connection rejected
    SOL_P2P_MSG_DISCONNECT = 7, // Disconnect notification
} sol_p2p_msg_type_t;

/**
 * P2P device info (sent in discovery).
 */
typedef struct {
    uint32_t device_id;         // Unique device ID (from MAC)
    char name[32];              // Human-readable name
    sol_mode_t mode;            // TX, RX, or TXRX
    uint8_t channels;           // Number of channels
    uint32_t sample_rate;       // Sample rate
    uint16_t rtp_port;          // RTP port
    uint8_t is_ptp_leader;      // 1 if this device is PTP leader
    uint32_t ip_addr;           // IP address (network byte order)
} sol_p2p_device_t;

/**
 * Peer connection state.
 */
typedef enum {
    SOL_PEER_DISCONNECTED = 0,
    SOL_PEER_CONNECTING,
    SOL_PEER_CONNECTED,
    SOL_PEER_STREAMING,
} sol_peer_state_t;

/**
 * Peer connection info.
 */
typedef struct {
    sol_p2p_device_t device;
    sol_peer_state_t state;
    uint32_t last_seen_ms;
    uint32_t packets_sent;
    uint32_t packets_received;
} sol_peer_t;

/**
 * Callback when a new peer is discovered.
 */
typedef void (*sol_p2p_peer_callback_t)(const sol_p2p_device_t* device, bool is_new);

/**
 * Callback when peer connection state changes.
 */
typedef void (*sol_p2p_state_callback_t)(const sol_peer_t* peer, sol_peer_state_t old_state);

/**
 * P2P configuration.
 */
typedef struct {
    sol_p2p_device_t local_device;      // This device's info
    uint16_t discovery_port;            // UDP port for discovery (default 8402)
    uint32_t announce_interval_ms;      // How often to announce (default 5000)
    sol_p2p_peer_callback_t peer_cb;    // Peer discovery callback
    sol_p2p_state_callback_t state_cb;  // State change callback
} sol_p2p_config_t;

/**
 * Initialize P2P subsystem.
 *
 * @param config P2P configuration
 * @return 0 on success
 */
int sol_p2p_init(const sol_p2p_config_t* config);

/**
 * Shutdown P2P subsystem.
 */
void sol_p2p_shutdown(void);

/**
 * Start discovery and announcements.
 *
 * @return 0 on success
 */
int sol_p2p_start_discovery(void);

/**
 * Stop discovery.
 */
void sol_p2p_stop_discovery(void);

/**
 * Connect to a peer.
 *
 * @param device_id Device ID to connect to
 * @return 0 on success
 */
int sol_p2p_connect(uint32_t device_id);

/**
 * Disconnect from a peer.
 *
 * @param device_id Device ID to disconnect from
 */
void sol_p2p_disconnect(uint32_t device_id);

/**
 * Disconnect from all peers.
 */
void sol_p2p_disconnect_all(void);

/**
 * Get list of discovered peers.
 *
 * @param peers Output array
 * @param max_peers Maximum number of peers to return
 * @return Number of peers found
 */
int sol_p2p_get_peers(sol_peer_t* peers, int max_peers);

/**
 * Get number of connected peers.
 */
int sol_p2p_connected_count(void);

/**
 * Check if we are the PTP leader.
 */
bool sol_p2p_is_ptp_leader(void);

/**
 * Set PTP leader mode.
 *
 * @param is_leader true to become PTP leader
 */
void sol_p2p_set_ptp_leader(bool is_leader);

#ifdef __cplusplus
}
#endif
