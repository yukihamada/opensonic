/**
 * Winsock2 UDP Socket Implementation (Windows)
 * SPDX-License-Identifier: MIT
 */

#include <soluna/pal/net.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#include <cstring>

namespace soluna::pal {

// RAII Winsock initializer
struct WinsockInit {
    WinsockInit() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WinsockInit() {
        WSACleanup();
    }
};

static WinsockInit g_winsock_init;

class UdpSocketWin32 : public UdpSocket {
public:
    UdpSocketWin32() {
        sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ != INVALID_SOCKET) {
            int reuse = 1;
            ::setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR,
                reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        }
    }

    ~UdpSocketWin32() override {
        if (sock_ != INVALID_SOCKET) {
            ::closesocket(sock_);
        }
    }

    bool bind(uint16_t port) override {
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;
        return ::bind(sock_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0;
    }

    bool join_multicast(const std::string& group, const std::string& iface) override {
        struct ip_mreq mreq{};
        inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr);
        if (iface.empty()) {
            mreq.imr_interface.s_addr = INADDR_ANY;
        } else {
            inet_pton(AF_INET, iface.c_str(), &mreq.imr_interface);
        }
        return ::setsockopt(sock_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
            reinterpret_cast<const char*>(&mreq), sizeof(mreq)) == 0;
    }

    bool leave_multicast(const std::string& group) override {
        struct ip_mreq mreq{};
        inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = INADDR_ANY;
        return ::setsockopt(sock_, IPPROTO_IP, IP_DROP_MEMBERSHIP,
            reinterpret_cast<const char*>(&mreq), sizeof(mreq)) == 0;
    }

    int send_to(const void* data, size_t len, const SocketAddress& dest) override {
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(dest.port);
        inet_pton(AF_INET, dest.ip.c_str(), &addr.sin_addr);
        return ::sendto(sock_, static_cast<const char*>(data), static_cast<int>(len), 0,
            reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    }

    int recv_from(void* data, size_t len, SocketAddress& src) override {
        struct sockaddr_in addr{};
        int addrlen = sizeof(addr);
        int n = ::recvfrom(sock_, static_cast<char*>(data), static_cast<int>(len), 0,
            reinterpret_cast<struct sockaddr*>(&addr), &addrlen);
        if (n > 0) {
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str));
            src.ip = ip_str;
            src.port = ntohs(addr.sin_port);
        }
        return n;
    }

    int recv_from_nonblock(void* data, size_t len, SocketAddress& src) override {
        // Use select with 0 timeout for non-blocking check
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(sock_, &read_fds);
        struct timeval tv{0, 0};

        int ret = ::select(0, &read_fds, nullptr, nullptr, &tv);
        if (ret <= 0) return ret;

        return recv_from(data, len, src);
    }

    bool set_dscp(uint8_t dscp) override {
        int tos = dscp << 2;
        return ::setsockopt(sock_, IPPROTO_IP, IP_TOS,
            reinterpret_cast<const char*>(&tos), sizeof(tos)) == 0;
    }

    bool set_recv_timeout_ms(uint32_t ms) override {
        DWORD timeout = ms;
        return ::setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO,
            reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0;
    }

    int fd() const override {
        return static_cast<int>(sock_);
    }

private:
    SOCKET sock_ = INVALID_SOCKET;
};

std::unique_ptr<UdpSocket> UdpSocket::create() {
    auto sock = std::make_unique<UdpSocketWin32>();
    if (sock->fd() < 0) return nullptr;
    return sock;
}

} // namespace soluna::pal

#endif // _WIN32
