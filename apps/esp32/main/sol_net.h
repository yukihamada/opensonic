/**
 * Soluna ESP32 — lwIP Network Abstraction
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "sol_common.h"

typedef struct {
    int fd;
} sol_socket_t;

/** Create UDP socket */
sol_err_t sol_socket_create(sol_socket_t* sock);

/** Bind to port */
sol_err_t sol_socket_bind(sol_socket_t* sock, uint16_t port);

/** Join multicast group */
sol_err_t sol_socket_join_mcast(sol_socket_t* sock, const char* group);

/** Send to address */
int sol_socket_sendto(sol_socket_t* sock, const void* data, size_t len,
                      const char* ip, uint16_t port);

/** Receive with timeout (ms). Returns bytes received, 0 on timeout, <0 on error */
int sol_socket_recvfrom(sol_socket_t* sock, void* data, size_t len,
                        char* src_ip, size_t ip_len, uint16_t* src_port,
                        uint32_t timeout_ms);

/** Close socket */
void sol_socket_close(sol_socket_t* sock);
