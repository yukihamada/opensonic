/**
 * Transport Manager — Implementation
 * SPDX-License-Identifier: MIT
 */

#include <soluna/transport/transport_manager.h>

namespace soluna::transport {

// ============================================================================
// PlainTransportSocket
// ============================================================================

PlainTransportSocket::PlainTransportSocket(std::unique_ptr<pal::UdpSocket> socket)
    : socket_(std::move(socket))
{
}

bool PlainTransportSocket::bind(uint16_t port) {
    return socket_->bind(port);
}

bool PlainTransportSocket::join_multicast(const std::string& group, const std::string& iface) {
    return socket_->join_multicast(group, iface);
}

bool PlainTransportSocket::leave_multicast(const std::string& group) {
    return socket_->leave_multicast(group);
}

int PlainTransportSocket::send_to(const void* data, size_t len, const pal::SocketAddress& dest) {
    return socket_->send_to(data, len, dest);
}

int PlainTransportSocket::recv_from(void* data, size_t len, pal::SocketAddress& src) {
    return socket_->recv_from(data, len, src);
}

int PlainTransportSocket::recv_from_nonblock(void* data, size_t len, pal::SocketAddress& src) {
    return socket_->recv_from_nonblock(data, len, src);
}

bool PlainTransportSocket::set_dscp(uint8_t dscp) {
    return socket_->set_dscp(dscp);
}

bool PlainTransportSocket::set_recv_timeout_ms(uint32_t ms) {
    return socket_->set_recv_timeout_ms(ms);
}

int PlainTransportSocket::fd() const {
    return socket_->fd();
}

// ============================================================================
// SecureTransportSocket
// ============================================================================

SecureTransportSocket::SecureTransportSocket(std::unique_ptr<DtlsSocket> socket,
                                             std::unique_ptr<pal::UdpSocket> fallback)
    : dtls_socket_(std::move(socket))
    , fallback_socket_(std::move(fallback))
{
}

bool SecureTransportSocket::bind(uint16_t port) {
    bound_port_ = port;
    bool dtls_ok = dtls_socket_ ? dtls_socket_->bind(port) : false;
    // Fallback socket binds to next port for multicast (DTLS can't do multicast)
    if (fallback_socket_ && port > 0) {
        fallback_socket_->bind(port + 1);
    }
    return dtls_ok;
}

bool SecureTransportSocket::join_multicast(const std::string& group, const std::string& iface) {
    // DTLS doesn't support multicast, use fallback socket
    if (fallback_socket_) {
        return fallback_socket_->join_multicast(group, iface);
    }
    return false;
}

bool SecureTransportSocket::leave_multicast(const std::string& group) {
    if (fallback_socket_) {
        return fallback_socket_->leave_multicast(group);
    }
    return false;
}

int SecureTransportSocket::send_to(const void* data, size_t len, const pal::SocketAddress& dest) {
    if (!dtls_socket_ || !dtls_socket_->is_connected()) {
        return -1;
    }
    return dtls_socket_->send_to(data, len, dest);
}

int SecureTransportSocket::recv_from(void* data, size_t len, pal::SocketAddress& src) {
    if (!dtls_socket_ || !dtls_socket_->is_connected()) {
        return -1;
    }
    return dtls_socket_->recv_from(data, len, src);
}

int SecureTransportSocket::recv_from_nonblock(void* data, size_t len, pal::SocketAddress& src) {
    // For DTLS, we use recv_from with a short timeout
    // The recv_timeout should have been set appropriately
    if (!dtls_socket_ || !dtls_socket_->is_connected()) {
        return -1;
    }
    return dtls_socket_->recv_from(data, len, src);
}

bool SecureTransportSocket::set_dscp(uint8_t dscp) {
    // DTLS socket uses the underlying UDP socket's DSCP
    // This is set during creation, not exposed directly
    (void)dscp;
    return true;
}

bool SecureTransportSocket::set_recv_timeout_ms(uint32_t ms) {
    // DTLS socket timeout is set during handshake
    (void)ms;
    return true;
}

int SecureTransportSocket::fd() const {
    return dtls_socket_ ? dtls_socket_->fd() : -1;
}

bool SecureTransportSocket::handshake(const pal::SocketAddress& peer) {
    if (!dtls_socket_) return false;
    return dtls_socket_->handshake(peer);
}

bool SecureTransportSocket::is_connected() const {
    return dtls_socket_ && dtls_socket_->is_connected();
}

void SecureTransportSocket::shutdown() {
    if (dtls_socket_) {
        dtls_socket_->shutdown();
    }
}

// ============================================================================
// TransportManager
// ============================================================================

TransportManager::TransportManager(const config::SecurityConfig& config)
    : config_(config)
{
}

std::unique_ptr<TransportSocket> TransportManager::create_socket(DtlsRole role) {
    if (config_.dtls_enabled) {
        DtlsConfig dtls_config;
        dtls_config.role = role;
        dtls_config.cert_file = config_.certificate_path;
        dtls_config.key_file = config_.private_key_path;

        auto dtls = DtlsSocket::create(dtls_config);
        if (!dtls) {
            // DTLS not available (OpenSSL not linked), fall back to plain
            auto udp = pal::UdpSocket::create();
            if (!udp) return nullptr;
            return std::make_unique<PlainTransportSocket>(std::move(udp));
        }

        // Create fallback socket for multicast operations
        auto fallback = pal::UdpSocket::create();
        return std::make_unique<SecureTransportSocket>(std::move(dtls), std::move(fallback));
    }

    // Plain UDP socket
    auto udp = pal::UdpSocket::create();
    if (!udp) return nullptr;
    return std::make_unique<PlainTransportSocket>(std::move(udp));
}

std::unique_ptr<TransportSocket> TransportManager::create_tx_socket() {
    return create_socket(DtlsRole::Client);
}

std::unique_ptr<TransportSocket> TransportManager::create_rx_socket() {
    return create_socket(DtlsRole::Server);
}

std::unique_ptr<TransportSocket> TransportManager::establish_secure_channel(
    const pal::SocketAddress& peer_addr,
    uint16_t local_port)
{
    if (!config_.dtls_enabled) {
        // Non-secure: just create a plain socket
        auto socket = create_socket(DtlsRole::Client);
        if (socket && local_port > 0) {
            socket->bind(local_port);
        }
        return socket;
    }

    // Create DTLS client socket
    auto socket = create_socket(DtlsRole::Client);
    if (!socket) return nullptr;

    if (local_port > 0) {
        if (!socket->bind(local_port)) {
            return nullptr;
        }
    }

    // Perform handshake
    auto* secure = dynamic_cast<SecureTransportSocket*>(socket.get());
    if (secure) {
        if (!secure->handshake(peer_addr)) {
            return nullptr;
        }
    }

    return socket;
}

} // namespace soluna::transport
