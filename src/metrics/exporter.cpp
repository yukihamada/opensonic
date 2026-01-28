/**
 * Soluna — Metrics Exporter Implementation
 *
 * Simple HTTP server for Prometheus scraping.
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/metrics/exporter.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
#define CLOSE_SOCKET closesocket
#define SOCKET_ERROR_CODE WSAGetLastError()
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
using socket_t = int;
#define CLOSE_SOCKET close
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define SOCKET_ERROR_CODE errno
#endif

#include <sstream>
#include <chrono>
#include <cstring>

namespace soluna {
namespace metrics {

Exporter::Exporter(const ExporterConfig& config)
    : config_(config)
    , registry_(&Registry::instance()) {}

Exporter::~Exporter() {
    stop();
}

Result<void> Exporter::start() {
    if (!config_.enabled) {
        return Result<void>::success();
    }

    if (running_.load()) {
        return Error(ErrorCode::AlreadyExists, "Exporter already running");
    }

#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        return Error(ErrorCode::SocketError, "WSAStartup failed");
    }
#endif

    // Create socket
    server_socket_ = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (server_socket_ == INVALID_SOCKET) {
        return Error(ErrorCode::SocketError, "Failed to create socket");
    }

    // Allow address reuse
    int opt = 1;
    setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    // Bind
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);

    if (config_.bind_address == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, config_.bind_address.c_str(), &addr.sin_addr);
    }

    if (bind(server_socket_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        CLOSE_SOCKET(server_socket_);
        return Error(ErrorCode::SocketBindFailed,
                     "Failed to bind to port",
                     std::to_string(config_.port));
    }

    // Listen
    if (listen(server_socket_, 5) == SOCKET_ERROR) {
        CLOSE_SOCKET(server_socket_);
        return Error(ErrorCode::SocketError, "Failed to listen");
    }

    running_.store(true);
    server_thread_ = std::thread(&Exporter::server_thread, this);

    return Result<void>::success();
}

void Exporter::stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);

    // Close socket to interrupt accept()
    if (server_socket_ != -1) {
        CLOSE_SOCKET(server_socket_);
        server_socket_ = -1;
    }

    if (server_thread_.joinable()) {
        server_thread_.join();
    }

#ifdef _WIN32
    WSACleanup();
#endif
}

void Exporter::server_thread() {
    while (running_.load()) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_socket = static_cast<int>(
            accept(server_socket_, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len));

        if (client_socket == INVALID_SOCKET) {
            if (!running_.load()) break;
            continue;
        }

        // Set receive timeout
        struct timeval timeout;
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));

        // Read request
        char buffer[4096];
        int n = recv(client_socket, buffer, sizeof(buffer) - 1, 0);

        if (n > 0) {
            buffer[n] = '\0';
            std::string request(buffer);
            std::string response = handle_request(request);

            // Send response
            send(client_socket, response.c_str(), static_cast<int>(response.size()), 0);
        }

        CLOSE_SOCKET(client_socket);
    }
}

std::string Exporter::handle_request(const std::string& request) {
    // Parse HTTP request line
    std::istringstream ss(request);
    std::string method, path, version;
    ss >> method >> path >> version;

    // Only GET is allowed
    if (method != "GET") {
        return method_not_allowed_response();
    }

    // Check path
    if (path == config_.path || path == config_.path + "/") {
        return metrics_response();
    }

    // Health check endpoint
    if (path == "/health" || path == "/healthz") {
        return "HTTP/1.1 200 OK\r\n"
               "Content-Type: text/plain\r\n"
               "Content-Length: 2\r\n"
               "\r\n"
               "OK";
    }

    return not_found_response();
}

std::string Exporter::metrics_response() {
    // Update scrape metrics
    auto now = std::chrono::system_clock::now();
    auto epoch = now.time_since_epoch();
    uint64_t seconds = std::chrono::duration_cast<std::chrono::seconds>(epoch).count();
    last_scrape_.store(seconds);
    scrape_count_.fetch_add(1);

    // Get metrics
    std::string body = registry_->format_all();

    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n";
    response << "Content-Length: " << body.size() << "\r\n";
    response << "\r\n";
    response << body;

    return response.str();
}

std::string Exporter::not_found_response() {
    const char* body = "404 Not Found\n";
    std::ostringstream response;
    response << "HTTP/1.1 404 Not Found\r\n";
    response << "Content-Type: text/plain\r\n";
    response << "Content-Length: " << strlen(body) << "\r\n";
    response << "\r\n";
    response << body;
    return response.str();
}

std::string Exporter::method_not_allowed_response() {
    const char* body = "405 Method Not Allowed\n";
    std::ostringstream response;
    response << "HTTP/1.1 405 Method Not Allowed\r\n";
    response << "Allow: GET\r\n";
    response << "Content-Type: text/plain\r\n";
    response << "Content-Length: " << strlen(body) << "\r\n";
    response << "\r\n";
    response << body;
    return response.str();
}

} // namespace metrics
} // namespace soluna
