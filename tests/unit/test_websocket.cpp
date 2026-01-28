/**
 * WebSocket Protocol Tests
 *
 * Tests HTTP parsing, WebSocket frame building/parsing,
 * and accept key computation (RFC 6455).
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/control/websocket_server.h>
#include <gtest/gtest.h>
#include <cstring>

using namespace soluna::control;

// --- HTTP Request Parsing ---

TEST(HttpParse, GetRoot) {
    std::string raw = "GET / HTTP/1.1\r\nHost: localhost:8400\r\n\r\n";
    HttpRequest req;
    ASSERT_TRUE(parse_http_request(raw, req));
    EXPECT_EQ(req.method, "GET");
    EXPECT_EQ(req.path, "/");
    EXPECT_FALSE(req.is_websocket_upgrade);
}

TEST(HttpParse, GetAppJs) {
    std::string raw = "GET /app.js HTTP/1.1\r\nHost: localhost\r\n\r\n";
    HttpRequest req;
    ASSERT_TRUE(parse_http_request(raw, req));
    EXPECT_EQ(req.path, "/app.js");
}

TEST(HttpParse, WebSocketUpgrade) {
    std::string raw =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost:8400\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";

    HttpRequest req;
    ASSERT_TRUE(parse_http_request(raw, req));
    EXPECT_EQ(req.path, "/ws");
    EXPECT_TRUE(req.is_websocket_upgrade);
    EXPECT_EQ(req.ws_key, "dGhlIHNhbXBsZSBub25jZQ==");
}

TEST(HttpParse, Incomplete) {
    // No \r\n\r\n terminator — should fail
    std::string raw = "GET / HTT";
    HttpRequest req;
    EXPECT_FALSE(parse_http_request(raw, req));
}

// --- WebSocket Accept Key (RFC 6455 Section 4.2.2) ---

TEST(WsAcceptKey, RFC6455Example) {
    // RFC 6455 example: key "dGhlIHNhbXBsZSBub25jZQ=="
    //   → SHA-1("dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11")
    //   → Base64 = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
    std::string accept = ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==");
    EXPECT_EQ(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

// --- WebSocket Frame Building ---

TEST(WsFrame, BuildSmallTextFrame) {
    auto frame = ws_build_text_frame("Hello");
    ASSERT_GE(frame.size(), 7u);
    EXPECT_EQ(frame[0], 0x81); // FIN + text
    EXPECT_EQ(frame[1], 5);    // length
    EXPECT_EQ(frame[2], 'H');
    EXPECT_EQ(frame[6], 'o');
}

TEST(WsFrame, BuildMediumTextFrame) {
    std::string text(200, 'A');
    auto frame = ws_build_text_frame(text);
    EXPECT_EQ(frame[0], 0x81);
    EXPECT_EQ(frame[1], 126);  // extended length
    uint16_t len = (uint16_t(frame[2]) << 8) | frame[3];
    EXPECT_EQ(len, 200u);
    EXPECT_EQ(frame.size(), 4u + 200u);
}

TEST(WsFrame, BuildEmptyFrame) {
    auto frame = ws_build_text_frame("");
    EXPECT_EQ(frame.size(), 2u);
    EXPECT_EQ(frame[0], 0x81);
    EXPECT_EQ(frame[1], 0);
}

// --- WebSocket Frame Parsing ---

TEST(WsFrame, ParseUnmaskedTextFrame) {
    // Build then parse
    auto frame = ws_build_text_frame("Test123");
    size_t consumed = 0;
    auto parsed = ws_parse_frame(frame.data(), frame.size(), consumed);

    ASSERT_TRUE(parsed.valid);
    EXPECT_EQ(parsed.opcode, 0x01); // text
    EXPECT_EQ(parsed.payload, "Test123");
    EXPECT_EQ(consumed, frame.size());
}

TEST(WsFrame, ParseMaskedTextFrame) {
    // Client sends masked frames (RFC 6455)
    std::string text = "Hi";
    uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};

    std::vector<uint8_t> frame;
    frame.push_back(0x81); // FIN + text
    frame.push_back(0x80 | 2); // MASK + len=2
    frame.insert(frame.end(), mask, mask + 4);
    // Masked payload
    frame.push_back('H' ^ mask[0]);
    frame.push_back('i' ^ mask[1]);

    size_t consumed = 0;
    auto parsed = ws_parse_frame(frame.data(), frame.size(), consumed);

    ASSERT_TRUE(parsed.valid);
    EXPECT_EQ(parsed.opcode, 0x01);
    EXPECT_EQ(parsed.payload, "Hi");
}

TEST(WsFrame, ParseIncomplete) {
    uint8_t data[1] = {0x81};
    size_t consumed = 0;
    auto parsed = ws_parse_frame(data, 1, consumed);
    EXPECT_FALSE(parsed.valid);
}

TEST(WsFrame, ParseCloseFrame) {
    uint8_t data[] = {0x88, 0x00}; // Close frame, no payload
    size_t consumed = 0;
    auto parsed = ws_parse_frame(data, 2, consumed);
    ASSERT_TRUE(parsed.valid);
    EXPECT_EQ(parsed.opcode, 0x08);
}

// --- HTTP Response ---

TEST(HttpResponse, Status200) {
    std::string resp = http_response(200, "text/html", 42);
    EXPECT_NE(resp.find("200 OK"), std::string::npos);
    EXPECT_NE(resp.find("Content-Type: text/html"), std::string::npos);
    EXPECT_NE(resp.find("Content-Length: 42"), std::string::npos);
}

TEST(HttpResponse, Status404) {
    std::string resp = http_response(404, "text/plain", 9);
    EXPECT_NE(resp.find("404 Not Found"), std::string::npos);
}
