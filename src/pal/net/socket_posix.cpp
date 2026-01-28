#include <soluna/pal/net.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>

namespace soluna::pal {

class UdpSocketPosix : public UdpSocket {
public:
    UdpSocketPosix() {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ >= 0) {
            int reuse = 1;
            ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
            ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif
        }
    }

    ~UdpSocketPosix() override {
        if (fd_ >= 0) ::close(fd_);
    }

    bool bind(uint16_t port) override {
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;
        return ::bind(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0;
    }

    bool join_multicast(const std::string& group, const std::string& iface) override {
        struct ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = inet_addr(group.c_str());
        if (iface.empty()) {
            mreq.imr_interface.s_addr = INADDR_ANY;
        } else {
            mreq.imr_interface.s_addr = inet_addr(iface.c_str());
        }
        return ::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == 0;
    }

    bool leave_multicast(const std::string& group) override {
        struct ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = inet_addr(group.c_str());
        mreq.imr_interface.s_addr = INADDR_ANY;
        return ::setsockopt(fd_, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq)) == 0;
    }

    int send_to(const void* data, size_t len, const SocketAddress& dest) override {
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(dest.port);
        inet_pton(AF_INET, dest.ip.c_str(), &addr.sin_addr);
        return static_cast<int>(::sendto(fd_, data, len, 0,
            reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)));
    }

    int recv_from(void* data, size_t len, SocketAddress& src) override {
        struct sockaddr_in addr{};
        socklen_t addrlen = sizeof(addr);
        int n = static_cast<int>(::recvfrom(fd_, data, len, 0,
            reinterpret_cast<struct sockaddr*>(&addr), &addrlen));
        if (n > 0) {
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str));
            src.ip = ip_str;
            src.port = ntohs(addr.sin_port);
        }
        return n;
    }

    int recv_from_nonblock(void* data, size_t len, SocketAddress& src) override {
        struct sockaddr_in addr{};
        socklen_t addrlen = sizeof(addr);
        int n = static_cast<int>(::recvfrom(fd_, data, len, MSG_DONTWAIT,
            reinterpret_cast<struct sockaddr*>(&addr), &addrlen));
        if (n > 0) {
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str));
            src.ip = ip_str;
            src.port = ntohs(addr.sin_port);
        }
        return n;
    }

    bool set_dscp(uint8_t dscp) override {
        int tos = dscp << 2;
        return ::setsockopt(fd_, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) == 0;
    }

    bool set_recv_timeout_ms(uint32_t ms) override {
        struct timeval tv{};
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        return ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
    }

    int fd() const override { return fd_; }

private:
    int fd_ = -1;
};

std::unique_ptr<UdpSocket> UdpSocket::create() {
    auto sock = std::make_unique<UdpSocketPosix>();
    if (sock->fd() < 0) return nullptr;
    return sock;
}

} // namespace soluna::pal
