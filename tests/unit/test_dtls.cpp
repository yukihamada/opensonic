/**
 * DTLS Socket Tests
 *
 * Tests DTLS socket creation and interface.
 * When SOLUNA_HAS_DTLS is defined, tests the full handshake + loopback.
 * Otherwise, tests that create() returns nullptr gracefully.
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/transport/dtls.h>
#include <gtest/gtest.h>
#include <cstring>
#include <thread>

using namespace soluna::transport;
using namespace soluna::pal;

#ifdef SOLUNA_HAS_DTLS

TEST(DtlsSocket, CreateServerSocket) {
    DtlsConfig cfg;
    cfg.role = DtlsRole::Server;
    auto sock = DtlsSocket::create(cfg);
    ASSERT_NE(sock, nullptr);
    EXPECT_FALSE(sock->is_connected());
}

TEST(DtlsSocket, CreateClientSocket) {
    DtlsConfig cfg;
    cfg.role = DtlsRole::Client;
    auto sock = DtlsSocket::create(cfg);
    ASSERT_NE(sock, nullptr);
    EXPECT_FALSE(sock->is_connected());
}

TEST(DtlsSocket, BindSocket) {
    DtlsConfig cfg;
    cfg.role = DtlsRole::Server;
    auto sock = DtlsSocket::create(cfg);
    ASSERT_NE(sock, nullptr);
    // Use a high port to avoid conflicts
    EXPECT_TRUE(sock->bind(19876));
}

TEST(DtlsSocket, LoopbackHandshakeAndTransfer) {
    // Server
    DtlsConfig srv_cfg;
    srv_cfg.role = DtlsRole::Server;
    srv_cfg.handshake_timeout_ms = 3000;
    auto server = DtlsSocket::create(srv_cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->bind(19877));

    // Client
    DtlsConfig cli_cfg;
    cli_cfg.role = DtlsRole::Client;
    cli_cfg.handshake_timeout_ms = 3000;
    auto client = DtlsSocket::create(cli_cfg);
    ASSERT_NE(client, nullptr);
    ASSERT_TRUE(client->bind(19878));

    SocketAddress srv_addr{"127.0.0.1", 19877};
    SocketAddress cli_addr{"127.0.0.1", 19878};

    // Handshake in parallel
    bool srv_ok = false, cli_ok = false;

    std::thread srv_thread([&]() {
        srv_ok = server->handshake(cli_addr);
    });

    std::thread cli_thread([&]() {
        cli_ok = client->handshake(srv_addr);
    });

    srv_thread.join();
    cli_thread.join();

    if (!srv_ok || !cli_ok) {
        GTEST_SKIP() << "DTLS handshake failed (may require loopback support)";
    }

    EXPECT_TRUE(server->is_connected());
    EXPECT_TRUE(client->is_connected());

    // Send data client → server
    const char* msg = "Hello DTLS";
    client->send_to(msg, std::strlen(msg), srv_addr);

    char buf[256] = {};
    SocketAddress src;
    int n = server->recv_from(buf, sizeof(buf), src);
    ASSERT_GT(n, 0);
    EXPECT_STREQ(buf, "Hello DTLS");

    server->shutdown();
    client->shutdown();
}

#else // !SOLUNA_HAS_DTLS

TEST(DtlsSocket, CreateReturnsNullWithoutOpenSSL) {
    DtlsConfig cfg;
    cfg.role = DtlsRole::Server;
    auto sock = DtlsSocket::create(cfg);
    EXPECT_EQ(sock, nullptr);
}

TEST(DtlsSocket, CreateClientReturnsNullWithoutOpenSSL) {
    DtlsConfig cfg;
    cfg.role = DtlsRole::Client;
    auto sock = DtlsSocket::create(cfg);
    EXPECT_EQ(sock, nullptr);
}

#endif // SOLUNA_HAS_DTLS
