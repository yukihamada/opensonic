/**
 * WebSocket Server — HTTP + WebSocket implementation
 * SPDX-License-Identifier: MIT
 */

#include <soluna/control/websocket_server.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define CLOSE_SOCKET closesocket
#define SOCKET_ERROR_VAL SOCKET_ERROR
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
using socket_t = int;
#define CLOSE_SOCKET close
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR_VAL (-1)
#endif

#ifdef SOLUNA_HAS_TLS
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

// SHA-1 for WebSocket handshake (minimal implementation)
namespace {

struct WsSHA1 {
    uint32_t state[5];
    uint64_t total_len;
    size_t buf_len;
    uint8_t buffer[64];

    WsSHA1() { reset(); }

    void reset() {
        state[0] = 0x67452301;
        state[1] = 0xEFCDAB89;
        state[2] = 0x98BADCFE;
        state[3] = 0x10325476;
        state[4] = 0xC3D2E1F0;
        total_len = 0;
        buf_len = 0;
        std::memset(buffer, 0, sizeof(buffer));
    }

    static uint32_t rol(uint32_t v, int bits) {
        return (v << bits) | (v >> (32 - bits));
    }

    void transform(const uint8_t block[64]) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = (uint32_t(block[i*4]) << 24) | (uint32_t(block[i*4+1]) << 16) |
                   (uint32_t(block[i*4+2]) << 8) | uint32_t(block[i*4+3]);
        }
        for (int i = 16; i < 80; i++) {
            w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        }

        uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];

        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else              { f = b ^ c ^ d; k = 0xCA62C1D6; }

            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }

        state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
    }

    void update(const void* data, size_t len) {
        auto p = static_cast<const uint8_t*>(data);
        total_len += len;

        // Fill buffer
        while (len > 0) {
            size_t copy = 64 - buf_len;
            if (copy > len) copy = len;
            std::memcpy(buffer + buf_len, p, copy);
            buf_len += copy;
            p += copy;
            len -= copy;
            if (buf_len == 64) {
                transform(buffer);
                buf_len = 0;
            }
        }
    }

    void final(uint8_t digest[20]) {
        // Save original message bit length before padding modifies total_len
        uint64_t msg_bits = total_len * 8;

        // Pad: append 0x80, then zeros until we have room for 8-byte length
        buffer[buf_len++] = 0x80;
        if (buf_len > 56) {
            // Not enough room in current block for length — fill and process
            std::memset(buffer + buf_len, 0, 64 - buf_len);
            transform(buffer);
            buf_len = 0;
        }
        // Zero-fill up to byte 56
        std::memset(buffer + buf_len, 0, 56 - buf_len);

        // Append original message bit length (big-endian 64-bit) at offset 56
        buffer[56] = uint8_t(msg_bits >> 56);
        buffer[57] = uint8_t(msg_bits >> 48);
        buffer[58] = uint8_t(msg_bits >> 40);
        buffer[59] = uint8_t(msg_bits >> 32);
        buffer[60] = uint8_t(msg_bits >> 24);
        buffer[61] = uint8_t(msg_bits >> 16);
        buffer[62] = uint8_t(msg_bits >> 8);
        buffer[63] = uint8_t(msg_bits);
        transform(buffer);

        for (int i = 0; i < 5; i++) {
            digest[i*4]   = uint8_t(state[i] >> 24);
            digest[i*4+1] = uint8_t(state[i] >> 16);
            digest[i*4+2] = uint8_t(state[i] >> 8);
            digest[i*4+3] = uint8_t(state[i]);
        }
    }
};

std::string base64_encode(const uint8_t* data, size_t len) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);

    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = uint32_t(data[i]) << 16;
        if (i + 1 < len) n |= uint32_t(data[i+1]) << 8;
        if (i + 2 < len) n |= uint32_t(data[i+2]);

        out += table[(n >> 18) & 0x3F];
        out += table[(n >> 12) & 0x3F];
        out += (i + 1 < len) ? table[(n >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? table[n & 0x3F] : '=';
    }
    return out;
}

} // anonymous namespace

namespace soluna::control {

// --- HTTP/WebSocket protocol helpers ---

std::string http_response(int status_code, const std::string& content_type,
                          size_t content_length, const std::string& extra_headers) {
    std::string status_text;
    switch (status_code) {
        case 200: status_text = "OK"; break;
        case 101: status_text = "Switching Protocols"; break;
        case 404: status_text = "Not Found"; break;
        default:  status_text = "Error"; break;
    }

    std::ostringstream ss;
    ss << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";
    if (!content_type.empty())
        ss << "Content-Type: " << content_type << "\r\n";
    ss << "Content-Length: " << content_length << "\r\n";
    ss << "Connection: keep-alive\r\n";
    if (!extra_headers.empty())
        ss << extra_headers;
    ss << "\r\n";
    return ss.str();
}

bool parse_http_request(const std::string& raw, HttpRequest& req) {
    // Parse first line: METHOD /path HTTP/1.1
    auto line_end = raw.find("\r\n");
    if (line_end == std::string::npos) return false;

    std::string first_line = raw.substr(0, line_end);
    auto sp1 = first_line.find(' ');
    auto sp2 = first_line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;

    req.method = first_line.substr(0, sp1);
    req.path = first_line.substr(sp1 + 1, sp2 - sp1 - 1);

    // Check headers
    std::string lower_raw = raw;
    std::transform(lower_raw.begin(), lower_raw.end(), lower_raw.begin(), ::tolower);

    req.is_websocket_upgrade =
        lower_raw.find("upgrade: websocket") != std::string::npos &&
        lower_raw.find("connection: upgrade") != std::string::npos;

    // Extract Sec-WebSocket-Key
    std::string key_header = "sec-websocket-key: ";
    auto pos = lower_raw.find(key_header);
    if (pos != std::string::npos) {
        auto start = pos + key_header.size();
        auto end = raw.find("\r\n", start);
        if (end != std::string::npos) {
            req.ws_key = raw.substr(start, end - start);
            // Trim whitespace
            while (!req.ws_key.empty() && req.ws_key.back() == ' ')
                req.ws_key.pop_back();
        }
    }

    return true;
}

std::string ws_accept_key(const std::string& client_key) {
    // RFC 6455: SHA-1(key + GUID)
    static const char* guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string concat = client_key + guid;

    WsSHA1 sha;
    sha.update(concat.data(), concat.size());
    uint8_t digest[20];
    sha.final(digest);

    return base64_encode(digest, 20);
}

std::vector<uint8_t> ws_build_text_frame(const std::string& text) {
    std::vector<uint8_t> frame;
    frame.push_back(0x81); // FIN + text opcode

    size_t len = text.size();
    if (len < 126) {
        frame.push_back(static_cast<uint8_t>(len));
    } else if (len <= 65535) {
        frame.push_back(126);
        frame.push_back(static_cast<uint8_t>(len >> 8));
        frame.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; i--) {
            frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
        }
    }

    frame.insert(frame.end(), text.begin(), text.end());
    return frame;
}

WsFrame ws_parse_frame(const uint8_t* data, size_t len, size_t& consumed) {
    WsFrame frame;
    consumed = 0;

    if (len < 2) return frame;

    frame.opcode = data[0] & 0x0F;
    bool masked = (data[1] & 0x80) != 0;
    uint64_t payload_len = data[1] & 0x7F;
    size_t offset = 2;

    if (payload_len == 126) {
        if (len < 4) return frame;
        payload_len = (uint64_t(data[2]) << 8) | data[3];
        offset = 4;
    } else if (payload_len == 127) {
        if (len < 10) return frame;
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | data[2 + i];
        }
        offset = 10;
    }

    uint8_t mask_key[4] = {};
    if (masked) {
        if (len < offset + 4) return frame;
        std::memcpy(mask_key, data + offset, 4);
        offset += 4;
    }

    if (len < offset + payload_len) return frame;

    frame.payload.resize(payload_len);
    for (size_t i = 0; i < payload_len; i++) {
        frame.payload[i] = static_cast<char>(
            data[offset + i] ^ (masked ? mask_key[i % 4] : 0));
    }

    consumed = offset + payload_len;
    frame.valid = true;
    return frame;
}

// --- Server implementation ---

struct ClientConn {
    socket_t fd = INVALID_SOCKET;
    bool is_websocket = false;
    std::vector<uint8_t> recv_buf;
#ifdef SOLUNA_HAS_TLS
    SSL* ssl = nullptr;
#endif
};

struct WebSocketServer::Impl {
    std::atomic<bool> running{false};
    std::thread server_thread;
    socket_t listen_fd = INVALID_SOCKET;

    const WebFile* web_files = nullptr;
    size_t web_file_count = 0;

    WsMessageCallback message_cb;

    mutable std::mutex clients_mutex;
    std::vector<ClientConn> clients;

#ifdef SOLUNA_HAS_TLS
    SSL_CTX* ssl_ctx = nullptr;
    bool tls_enabled = false;
#endif

    void serve_loop(uint16_t port);
    void handle_http(ClientConn& client);
    void handle_websocket(ClientConn& client);
    void serve_file(ClientConn& client, const std::string& path);
    void send_all(ClientConn& client, const void* data, size_t len);
    int recv_data(ClientConn& client, void* buf, size_t len);
    void close_client(ClientConn& client);
};

WebSocketServer::WebSocketServer() : impl_(std::make_unique<Impl>()) {}
WebSocketServer::~WebSocketServer() {
    stop();
#ifdef SOLUNA_HAS_TLS
    if (impl_->ssl_ctx) {
        SSL_CTX_free(impl_->ssl_ctx);
        impl_->ssl_ctx = nullptr;
    }
#endif
}

bool WebSocketServer::enable_tls(const std::string& cert_path, const std::string& key_path) {
#ifdef SOLUNA_HAS_TLS
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        fprintf(stderr, "TLS: SSL_CTX_new failed\n");
        return false;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (SSL_CTX_use_certificate_file(ctx, cert_path.c_str(), SSL_FILETYPE_PEM) != 1) {
        fprintf(stderr, "TLS: failed to load cert: %s\n", cert_path.c_str());
        SSL_CTX_free(ctx);
        return false;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
        fprintf(stderr, "TLS: failed to load key: %s\n", key_path.c_str());
        SSL_CTX_free(ctx);
        return false;
    }
    if (!SSL_CTX_check_private_key(ctx)) {
        fprintf(stderr, "TLS: cert/key mismatch\n");
        SSL_CTX_free(ctx);
        return false;
    }
    impl_->ssl_ctx = ctx;
    impl_->tls_enabled = true;
    fprintf(stderr, "TLS: enabled (cert=%s)\n", cert_path.c_str());
    return true;
#else
    (void)cert_path; (void)key_path;
    fprintf(stderr, "TLS: not compiled (build with -DSOLUNA_ENABLE_TLS=ON)\n");
    return false;
#endif
}

void WebSocketServer::set_web_files(const WebFile* files, size_t count) {
    impl_->web_files = files;
    impl_->web_file_count = count;
}

void WebSocketServer::set_message_callback(WsMessageCallback cb) {
    impl_->message_cb = std::move(cb);
}

bool WebSocketServer::start(uint16_t port) {
    if (impl_->running) return false;

    impl_->running = true;
    impl_->server_thread = std::thread([this, port]() {
        impl_->serve_loop(port);
    });

    return true;
}

void WebSocketServer::stop() {
    if (!impl_->running) return;
    impl_->running = false;

    if (impl_->listen_fd != INVALID_SOCKET) {
        CLOSE_SOCKET(impl_->listen_fd);
        impl_->listen_fd = INVALID_SOCKET;
    }

    if (impl_->server_thread.joinable()) {
        impl_->server_thread.join();
    }

    std::lock_guard<std::mutex> lock(impl_->clients_mutex);
    for (auto& c : impl_->clients) {
        impl_->close_client(c);
    }
    impl_->clients.clear();
}

bool WebSocketServer::is_running() const {
    return impl_->running;
}

void WebSocketServer::broadcast(const std::string& message) {
    auto frame = ws_build_text_frame(message);
    std::lock_guard<std::mutex> lock(impl_->clients_mutex);
    for (auto& c : impl_->clients) {
        if (c.is_websocket && c.fd != INVALID_SOCKET) {
            impl_->send_all(c, frame.data(), frame.size());
        }
    }
}

std::vector<uint8_t> ws_build_binary_frame(const uint8_t* data, size_t len) {
    std::vector<uint8_t> frame;
    frame.push_back(0x82); // FIN + binary opcode
    if (len < 126) {
        frame.push_back(static_cast<uint8_t>(len));
    } else if (len <= 65535) {
        frame.push_back(126);
        frame.push_back(static_cast<uint8_t>(len >> 8));
        frame.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; i--) {
            frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
        }
    }
    frame.insert(frame.end(), data, data + len);
    return frame;
}

void WebSocketServer::broadcast_binary(const uint8_t* data, size_t len) {
    auto frame = ws_build_binary_frame(data, len);
    std::lock_guard<std::mutex> lock(impl_->clients_mutex);
    for (auto& c : impl_->clients) {
        if (c.is_websocket && c.fd != INVALID_SOCKET) {
            impl_->send_all(c, frame.data(), frame.size());
        }
    }
}

size_t WebSocketServer::client_count() const {
    std::lock_guard<std::mutex> lock(impl_->clients_mutex);
    size_t count = 0;
    for (const auto& c : impl_->clients) {
        if (c.is_websocket) count++;
    }
    return count;
}

void WebSocketServer::Impl::send_all(ClientConn& client, const void* data, size_t len) {
    auto p = static_cast<const char*>(data);
    size_t sent = 0;
    while (sent < len) {
#ifdef SOLUNA_HAS_TLS
        if (client.ssl) {
            int n = SSL_write(client.ssl, p + sent, static_cast<int>(len - sent));
            if (n <= 0) break;
            sent += n;
            continue;
        }
#endif
        int n = ::send(client.fd, p + sent, static_cast<int>(len - sent), 0);
        if (n <= 0) break;
        sent += n;
    }
}

int WebSocketServer::Impl::recv_data(ClientConn& client, void* buf, size_t len) {
#ifdef SOLUNA_HAS_TLS
    if (client.ssl) {
        return SSL_read(client.ssl, buf, static_cast<int>(len));
    }
#endif
    return recv(client.fd, reinterpret_cast<char*>(buf), static_cast<int>(len), 0);
}

void WebSocketServer::Impl::close_client(ClientConn& client) {
#ifdef SOLUNA_HAS_TLS
    if (client.ssl) {
        SSL_shutdown(client.ssl);
        SSL_free(client.ssl);
        client.ssl = nullptr;
    }
#endif
    if (client.fd != INVALID_SOCKET) {
        CLOSE_SOCKET(client.fd);
        client.fd = INVALID_SOCKET;
    }
}

void WebSocketServer::Impl::serve_loop(uint16_t port) {
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == INVALID_SOCKET) { running = false; return; }

    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        CLOSE_SOCKET(listen_fd);
        listen_fd = INVALID_SOCKET;
        running = false;
        return;
    }

    if (::listen(listen_fd, 8) < 0) {
        CLOSE_SOCKET(listen_fd);
        listen_fd = INVALID_SOCKET;
        running = false;
        return;
    }

#ifndef _WIN32
    // Set non-blocking for poll
    fcntl(listen_fd, F_SETFL, O_NONBLOCK);
#endif

    while (running) {
        // Build poll set
        std::vector<struct pollfd> fds;
        {
            struct pollfd pfd{};
            pfd.fd = listen_fd;
            pfd.events = POLLIN;
            fds.push_back(pfd);
        }

        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            for (auto& c : clients) {
                if (c.fd != INVALID_SOCKET) {
                    struct pollfd pfd{};
                    pfd.fd = c.fd;
                    pfd.events = POLLIN;
                    fds.push_back(pfd);
                }
            }
        }

#ifdef _WIN32
        int ret = WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), 100);
#else
        int ret = poll(fds.data(), static_cast<nfds_t>(fds.size()), 100);
#endif
        if (ret <= 0) continue;

        // Accept new connections
        if (fds[0].revents & POLLIN) {
            struct sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            socket_t client_fd = accept(listen_fd,
                reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
            if (client_fd != INVALID_SOCKET) {
                ClientConn conn;
                conn.fd = client_fd;
#ifdef SOLUNA_HAS_TLS
                if (tls_enabled && ssl_ctx) {
                    conn.ssl = SSL_new(ssl_ctx);
                    SSL_set_fd(conn.ssl, client_fd);
                    if (SSL_accept(conn.ssl) <= 0) {
                        SSL_free(conn.ssl);
                        CLOSE_SOCKET(client_fd);
                        continue;
                    }
                }
#endif
                std::lock_guard<std::mutex> lock(clients_mutex);
                clients.push_back(std::move(conn));
            }
        }

        // Process client data
        for (size_t i = 1; i < fds.size(); i++) {
            if (!(fds[i].revents & POLLIN)) continue;

            std::lock_guard<std::mutex> lock(clients_mutex);
            // Find matching client
            for (auto& c : clients) {
                if (c.fd == fds[i].fd) {
                    uint8_t buf[4096];
                    int n = recv_data(c, buf, sizeof(buf));
                    if (n <= 0) {
                        close_client(c);
                        break;
                    }

                    c.recv_buf.insert(c.recv_buf.end(), buf, buf + n);

                    if (c.is_websocket) {
                        handle_websocket(c);
                    } else {
                        handle_http(c);
                    }
                    break;
                }
            }

            // Clean up disconnected clients
            clients.erase(
                std::remove_if(clients.begin(), clients.end(),
                    [](const ClientConn& c) { return c.fd == INVALID_SOCKET; }),
                clients.end());
        }
    }
}

void WebSocketServer::Impl::handle_http(ClientConn& client) {
    // Check if we have a complete HTTP request
    std::string data(client.recv_buf.begin(), client.recv_buf.end());
    auto header_end = data.find("\r\n\r\n");
    if (header_end == std::string::npos) return; // incomplete

    HttpRequest req;
    if (!parse_http_request(data, req)) {
        close_client(client);
        return;
    }

    client.recv_buf.clear();

    if (req.is_websocket_upgrade && req.path == "/ws") {
        // WebSocket upgrade
        std::string accept = ws_accept_key(req.ws_key);
        std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";

        send_all(client, response.data(), response.size());
        client.is_websocket = true;
        return;
    }

    // Serve static files
    serve_file(client, req.path);
}

void WebSocketServer::Impl::serve_file(ClientConn& client, const std::string& path) {
    std::string resolved = path;
    if (resolved == "/") resolved = "/index.html";

    for (size_t i = 0; i < web_file_count; i++) {
        if (resolved == web_files[i].path) {
            std::string hdr = http_response(200, web_files[i].mime_type,
                                             web_files[i].size);
            send_all(client, hdr.data(), hdr.size());
            send_all(client, web_files[i].data, web_files[i].size);
            return;
        }
    }

    // 404
    std::string body = "Not Found";
    std::string hdr = http_response(404, "text/plain", body.size());
    send_all(client, hdr.data(), hdr.size());
    send_all(client, body.data(), body.size());
}

void WebSocketServer::Impl::handle_websocket(ClientConn& client) {
    while (client.recv_buf.size() >= 2) {
        size_t consumed = 0;
        auto frame = ws_parse_frame(client.recv_buf.data(),
                                     client.recv_buf.size(), consumed);

        if (!frame.valid) break;

        client.recv_buf.erase(client.recv_buf.begin(),
                              client.recv_buf.begin() + consumed);

        if (frame.opcode == 0x08) {
            // Close frame
            close_client(client);
            return;
        }

        if (frame.opcode == 0x09) {
            // Ping → Pong
            auto pong = ws_build_text_frame(frame.payload);
            pong[0] = 0x8A; // Pong opcode
            send_all(client, pong.data(), pong.size());
            continue;
        }

        if (frame.opcode == 0x01 && message_cb) {
            // Text frame
            std::string response = message_cb(frame.payload);
            auto resp_frame = ws_build_text_frame(response);
            send_all(client, resp_frame.data(), resp_frame.size());
        }
    }
}

} // namespace soluna::control
