/**
 * Soluna ESP32 — lwIP Network Implementation
 * SPDX-License-Identifier: MIT
 */

#include "sol_net.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/igmp.h"
#include "esp_log.h"

static const char* TAG = "sol_net";

sol_err_t sol_socket_create(sol_socket_t* sock) {
    sock->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock->fd < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return SOL_ERR_IO;
    }

    int reuse = 1;
    setsockopt(sock->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    return SOL_OK;
}

sol_err_t sol_socket_bind(sol_socket_t* sock, uint16_t port) {
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind(%u) failed: errno %d", port, errno);
        return SOL_ERR_IO;
    }
    return SOL_OK;
}

sol_err_t sol_socket_join_mcast(sol_socket_t* sock, const char* group) {
    struct ip_mreq mreq = {0};
    mreq.imr_multiaddr.s_addr = inet_addr(group);
    mreq.imr_interface.s_addr = INADDR_ANY;

    if (setsockopt(sock->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   &mreq, sizeof(mreq)) < 0) {
        ESP_LOGE(TAG, "join_mcast(%s) failed: errno %d", group, errno);
        return SOL_ERR_IO;
    }

    ESP_LOGI(TAG, "Joined multicast %s", group);
    return SOL_OK;
}

int sol_socket_sendto(sol_socket_t* sock, const void* data, size_t len,
                      const char* ip, uint16_t port) {
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip);

    return sendto(sock->fd, data, len, 0,
                  (struct sockaddr*)&addr, sizeof(addr));
}

int sol_socket_recvfrom(sol_socket_t* sock, void* data, size_t len,
                        char* src_ip, size_t ip_len, uint16_t* src_port,
                        uint32_t timeout_ms) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in src = {0};
    socklen_t addrlen = sizeof(src);
    int n = recvfrom(sock->fd, data, len, 0,
                     (struct sockaddr*)&src, &addrlen);

    if (n > 0 && src_ip) {
        inet_ntoa_r(src.sin_addr, src_ip, ip_len);
    }
    if (n > 0 && src_port) {
        *src_port = ntohs(src.sin_port);
    }
    return n;
}

void sol_socket_close(sol_socket_t* sock) {
    if (sock->fd >= 0) {
        close(sock->fd);
        sock->fd = -1;
    }
}
