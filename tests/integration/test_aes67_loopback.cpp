/**
 * AES67 Loopback Integration Test
 *
 * Tests AES67-compatible RTP transmission and reception,
 * SDP parsing, and SAP discovery.
 *
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>
#include <soluna/transport/aes67.h>
#include <soluna/transport/rtp.h>
#include <soluna/pal/net.h>
#include <soluna/sync/ptp_engine.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <cstring>

using namespace soluna;
using namespace soluna::transport;

class Aes67Test : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a test session
        session_.session_name = "TestStream";
        session_.origin_address = "192.168.1.100";
        session_.session_id = 12345;
        session_.session_version = 1;
        session_.multicast_group = "239.69.0.1";
        session_.rtp_port = 5004;
        session_.sample_rate = 48000;
        session_.channels = 2;
        session_.bit_depth = 24;
        session_.payload_type = kPayloadTypeL24;
        session_.packet_time_us = 1000;
    }

    Aes67Session session_;
};

// ============================================================================
// SDP Generation Tests
// ============================================================================

TEST_F(Aes67Test, SdpGeneration) {
    std::string sdp = aes67_generate_sdp(session_);

    EXPECT_NE(sdp.find("v=0"), std::string::npos);
    EXPECT_NE(sdp.find("s=TestStream"), std::string::npos);
    EXPECT_NE(sdp.find("c=IN IP4 239.69.0.1/32"), std::string::npos);
    EXPECT_NE(sdp.find("m=audio 5004"), std::string::npos);
    EXPECT_NE(sdp.find("L24/48000/2"), std::string::npos);
    EXPECT_NE(sdp.find("ptp=IEEE1588-2008"), std::string::npos);
}

TEST_F(Aes67Test, SdpGenerationL16Mono) {
    session_.bit_depth = 16;
    session_.channels = 1;
    session_.payload_type = kPayloadTypeL16;

    std::string sdp = aes67_generate_sdp(session_);

    EXPECT_NE(sdp.find("L16/48000"), std::string::npos);
    // Mono should not have channel count
    EXPECT_EQ(sdp.find("/48000/1"), std::string::npos);
}

// ============================================================================
// SDP Parsing Tests
// ============================================================================

TEST_F(Aes67Test, SdpParseBasic) {
    std::string sdp = aes67_generate_sdp(session_);

    Aes67RemoteSession parsed;
    ASSERT_TRUE(aes67_parse_sdp(sdp, parsed));

    EXPECT_EQ(parsed.session_name, "TestStream");
    EXPECT_EQ(parsed.multicast_ip, "239.69.0.1");
    EXPECT_EQ(parsed.port, 5004);
    EXPECT_EQ(parsed.sample_rate, 48000u);
    EXPECT_EQ(parsed.channels, 2);
    EXPECT_EQ(parsed.bit_depth, 24);
    EXPECT_TRUE(parsed.has_ptp_refclk);
}

TEST_F(Aes67Test, SdpParseL16Mono) {
    session_.bit_depth = 16;
    session_.channels = 1;
    session_.payload_type = kPayloadTypeL16;

    std::string sdp = aes67_generate_sdp(session_);

    Aes67RemoteSession parsed;
    ASSERT_TRUE(aes67_parse_sdp(sdp, parsed));

    EXPECT_EQ(parsed.bit_depth, 16);
    EXPECT_EQ(parsed.channels, 1);
}

TEST_F(Aes67Test, SdpParseInvalid) {
    Aes67RemoteSession parsed;

    // Empty SDP
    EXPECT_FALSE(aes67_parse_sdp("", parsed));

    // Missing required fields
    EXPECT_FALSE(aes67_parse_sdp("v=0\r\ns=Test\r\n", parsed));
}

TEST_F(Aes67Test, SdpParseExternalFormat) {
    // Test parsing of an externally-generated SDP (e.g., from Dante)
    const char* external_sdp =
        "v=0\r\n"
        "o=- 1234567890 1 IN IP4 10.0.0.50\r\n"
        "s=Dante-Device\r\n"
        "c=IN IP4 239.255.0.1/32\r\n"
        "t=0 0\r\n"
        "m=audio 5004 RTP/AVP 10\r\n"
        "a=rtpmap:10 L24/48000/8\r\n"
        "a=ptime:1\r\n"
        "a=ts-refclk:ptp=IEEE1588-2008\r\n"
        "a=mediaclk:direct=0\r\n";

    Aes67RemoteSession parsed;
    ASSERT_TRUE(aes67_parse_sdp(external_sdp, parsed));

    EXPECT_EQ(parsed.session_name, "Dante-Device");
    EXPECT_EQ(parsed.origin_address, "10.0.0.50");
    EXPECT_EQ(parsed.multicast_ip, "239.255.0.1");
    EXPECT_EQ(parsed.port, 5004);
    EXPECT_EQ(parsed.sample_rate, 48000u);
    EXPECT_EQ(parsed.channels, 8);
    EXPECT_EQ(parsed.bit_depth, 24);
    EXPECT_EQ(parsed.packet_time_us, 1000u);
    EXPECT_TRUE(parsed.has_ptp_refclk);
}

// ============================================================================
// SAP Packet Tests
// ============================================================================

TEST_F(Aes67Test, SapPacketBuild) {
    uint8_t buf[2048];
    size_t len = aes67_build_sap_packet(session_, buf, sizeof(buf));

    EXPECT_GT(len, sizeof(SapHeader));

    // Check SAP header
    const SapHeader* hdr = reinterpret_cast<const SapHeader*>(buf);
    EXPECT_EQ((hdr->version_flags >> 5) & 0x07, 1); // Version 1
    EXPECT_EQ(hdr->auth_length, 0); // No auth

    // Check payload type string
    const char* payload_type = reinterpret_cast<const char*>(buf + sizeof(SapHeader));
    EXPECT_STREQ(payload_type, "application/sdp");
}

TEST_F(Aes67Test, SapHashConsistency) {
    std::string sdp = aes67_generate_sdp(session_);

    uint16_t hash1 = aes67_sap_hash(sdp);
    uint16_t hash2 = aes67_sap_hash(sdp);

    EXPECT_EQ(hash1, hash2);

    // Different SDP should have different hash
    session_.session_name = "DifferentStream";
    std::string sdp2 = aes67_generate_sdp(session_);
    uint16_t hash3 = aes67_sap_hash(sdp2);

    EXPECT_NE(hash1, hash3);
}

// ============================================================================
// RTP Packet Tests
// ============================================================================

TEST_F(Aes67Test, RtpPacketBuildL24) {
    // Create test audio data (48 samples × 2 channels × 3 bytes)
    constexpr size_t frames = 48;
    constexpr size_t channels = 2;
    constexpr size_t bytes_per_sample = 3;
    std::vector<uint8_t> payload(frames * channels * bytes_per_sample);

    // Fill with test pattern
    for (size_t i = 0; i < payload.size(); i++) {
        payload[i] = static_cast<uint8_t>(i & 0xFF);
    }

    uint8_t buf[1500];
    size_t len = aes67_build_rtp_packet(
        buf, sizeof(buf),
        0x12345678, 100, 4800,
        kPayloadTypeL24,
        payload.data(), payload.size()
    );

    EXPECT_EQ(len, sizeof(RtpHeader) + payload.size());

    // Check RTP header
    const RtpHeader* rtp = reinterpret_cast<const RtpHeader*>(buf);
    EXPECT_EQ(rtp->version, 2);
    EXPECT_EQ(rtp->extension, 0); // No extension for AES67
    EXPECT_EQ(rtp->pt, kPayloadTypeL24);
}

TEST_F(Aes67Test, RtpPacketIsAes67) {
    RtpHeader hdr_l24{};
    hdr_l24.pt = kPayloadTypeL24;
    EXPECT_TRUE(aes67_is_standard_packet(hdr_l24));

    RtpHeader hdr_l16{};
    hdr_l16.pt = kPayloadTypeL16;
    EXPECT_TRUE(aes67_is_standard_packet(hdr_l16));

    RtpHeader hdr_ostp{};
    hdr_ostp.pt = kPayloadTypePCM24; // 96
    EXPECT_FALSE(aes67_is_standard_packet(hdr_ostp));
}

// ============================================================================
// PTP Media Clock Tests
// ============================================================================

TEST_F(Aes67Test, MediaClockToRtpTimestamp) {
    // Test conversion at 48kHz
    int64_t media_clock_ns = 1'000'000'000LL; // 1 second
    uint32_t rtp_ts = sync::PtpEngine::media_clock_to_rtp_timestamp(media_clock_ns, 48000);
    EXPECT_EQ(rtp_ts, 48000u);

    // Test at 96kHz
    rtp_ts = sync::PtpEngine::media_clock_to_rtp_timestamp(media_clock_ns, 96000);
    EXPECT_EQ(rtp_ts, 96000u);

    // Test sub-second
    media_clock_ns = 500'000'000LL; // 0.5 seconds
    rtp_ts = sync::PtpEngine::media_clock_to_rtp_timestamp(media_clock_ns, 48000);
    EXPECT_EQ(rtp_ts, 24000u);
}

// ============================================================================
// Loopback Tests (TX → RX)
// ============================================================================

TEST_F(Aes67Test, LoopbackL24) {
    // Create TX socket
    auto tx_socket = pal::UdpSocket::create();
    ASSERT_NE(tx_socket, nullptr);
    tx_socket->set_dscp(46);

    // Create RX socket
    auto rx_socket = pal::UdpSocket::create();
    ASSERT_NE(rx_socket, nullptr);
    ASSERT_TRUE(rx_socket->bind(15100));
    rx_socket->set_recv_timeout_ms(100);

    pal::SocketAddress dest{"127.0.0.1", 15100};

    // Create and send AES67 packet
    constexpr size_t frames = 48;
    constexpr size_t channels = 2;
    constexpr size_t bytes_per_sample = 3;
    std::vector<uint8_t> tx_payload(frames * channels * bytes_per_sample);

    // Fill with recognizable pattern
    for (size_t i = 0; i < tx_payload.size(); i++) {
        tx_payload[i] = static_cast<uint8_t>((i * 7) & 0xFF);
    }

    uint8_t tx_buf[1500];
    size_t tx_len = aes67_build_rtp_packet(
        tx_buf, sizeof(tx_buf),
        0xABCDEF01, 1, 0,
        kPayloadTypeL24,
        tx_payload.data(), tx_payload.size()
    );

    int sent = tx_socket->send_to(tx_buf, tx_len, dest);
    EXPECT_EQ(sent, static_cast<int>(tx_len));

    // Receive and verify
    uint8_t rx_buf[1500];
    pal::SocketAddress src;
    int received = rx_socket->recv_from(rx_buf, sizeof(rx_buf), src);

    ASSERT_EQ(received, static_cast<int>(tx_len));

    // Verify RTP header
    const RtpHeader* rtp = reinterpret_cast<const RtpHeader*>(rx_buf);
    EXPECT_EQ(rtp->version, 2);
    EXPECT_EQ(rtp->pt, kPayloadTypeL24);
    EXPECT_TRUE(aes67_is_standard_packet(*rtp));

    // Verify payload
    const uint8_t* rx_payload = rx_buf + sizeof(RtpHeader);
    EXPECT_EQ(std::memcmp(rx_payload, tx_payload.data(), tx_payload.size()), 0);
}

TEST_F(Aes67Test, LoopbackL16) {
    auto tx_socket = pal::UdpSocket::create();
    auto rx_socket = pal::UdpSocket::create();
    ASSERT_NE(tx_socket, nullptr);
    ASSERT_NE(rx_socket, nullptr);

    ASSERT_TRUE(rx_socket->bind(15101));
    rx_socket->set_recv_timeout_ms(100);

    pal::SocketAddress dest{"127.0.0.1", 15101};

    // Create L16 payload
    constexpr size_t frames = 48;
    constexpr size_t channels = 2;
    std::vector<uint8_t> tx_payload(frames * channels * 2); // 16-bit

    for (size_t i = 0; i < tx_payload.size(); i++) {
        tx_payload[i] = static_cast<uint8_t>((i * 11) & 0xFF);
    }

    uint8_t tx_buf[1500];
    size_t tx_len = aes67_build_rtp_packet(
        tx_buf, sizeof(tx_buf),
        0x12345678, 42, 1000,
        kPayloadTypeL16,
        tx_payload.data(), tx_payload.size()
    );

    int sent = tx_socket->send_to(tx_buf, tx_len, dest);
    EXPECT_EQ(sent, static_cast<int>(tx_len));

    uint8_t rx_buf[1500];
    pal::SocketAddress src;
    int received = rx_socket->recv_from(rx_buf, sizeof(rx_buf), src);

    ASSERT_EQ(received, static_cast<int>(tx_len));

    const RtpHeader* rtp = reinterpret_cast<const RtpHeader*>(rx_buf);
    EXPECT_EQ(rtp->pt, kPayloadTypeL16);
}

// ============================================================================
// SAP Listener Tests (requires network, may be skipped in CI)
// ============================================================================

TEST_F(Aes67Test, SapListenerStartStop) {
    SapListener listener;

    EXPECT_FALSE(listener.is_running());
    EXPECT_EQ(listener.session_count(), 0u);

    bool callback_called = false;
    ASSERT_TRUE(listener.start([&](const Aes67RemoteSession& session, bool is_deletion) {
        callback_called = true;
        (void)session;
        (void)is_deletion;
    }));

    EXPECT_TRUE(listener.is_running());

    listener.stop();

    EXPECT_FALSE(listener.is_running());
}

// Note: Full SAP discovery test would require sending SAP packets,
// which needs proper multicast networking setup
