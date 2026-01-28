/**
 * AES67 Compatibility Tests
 * SPDX-License-Identifier: MIT
 */

#include <soluna/transport/aes67.h>
#include <gtest/gtest.h>
#include <cstring>
#include <string>

using namespace soluna::transport;

// --- SDP Generation ---

TEST(Aes67Sdp, BasicL24Mono) {
    Aes67Session sess;
    sess.session_name = "Soluna-TestStream";
    sess.origin_address = "192.168.1.100";
    sess.session_id = 12345;
    sess.session_version = 1;
    sess.multicast_group = "239.69.0.1";
    sess.rtp_port = 5004;
    sess.sample_rate = 48000;
    sess.channels = 1;
    sess.bit_depth = 24;
    sess.payload_type = kPayloadTypeL24;
    sess.packet_time_us = 1000;

    std::string sdp = aes67_generate_sdp(sess);

    EXPECT_NE(sdp.find("v=0"), std::string::npos);
    EXPECT_NE(sdp.find("o=- 12345 1 IN IP4 192.168.1.100"), std::string::npos);
    EXPECT_NE(sdp.find("s=Soluna-TestStream"), std::string::npos);
    EXPECT_NE(sdp.find("c=IN IP4 239.69.0.1/32"), std::string::npos);
    EXPECT_NE(sdp.find("t=0 0"), std::string::npos);
    EXPECT_NE(sdp.find("m=audio 5004 RTP/AVP 10"), std::string::npos);
    EXPECT_NE(sdp.find("a=rtpmap:10 L24/48000"), std::string::npos);
    EXPECT_NE(sdp.find("a=ptime:1"), std::string::npos);
    EXPECT_NE(sdp.find("a=ts-refclk:ptp=IEEE1588-2008"), std::string::npos);
    EXPECT_NE(sdp.find("a=mediaclk:direct=0"), std::string::npos);
}

TEST(Aes67Sdp, L16Stereo) {
    Aes67Session sess;
    sess.session_name = "Stereo16";
    sess.origin_address = "10.0.0.5";
    sess.session_id = 99;
    sess.session_version = 2;
    sess.multicast_group = "239.69.0.2";
    sess.rtp_port = 5010;
    sess.sample_rate = 48000;
    sess.channels = 2;
    sess.bit_depth = 16;
    sess.payload_type = kPayloadTypeL16;
    sess.packet_time_us = 1000;

    std::string sdp = aes67_generate_sdp(sess);

    EXPECT_NE(sdp.find("m=audio 5010 RTP/AVP 11"), std::string::npos);
    EXPECT_NE(sdp.find("a=rtpmap:11 L16/48000/2"), std::string::npos);
}

TEST(Aes67Sdp, MonoOmitsChannelCount) {
    Aes67Session sess;
    sess.session_name = "Mono";
    sess.origin_address = "10.0.0.1";
    sess.multicast_group = "239.69.0.1";
    sess.channels = 1;
    sess.bit_depth = 24;
    sess.payload_type = kPayloadTypeL24;

    std::string sdp = aes67_generate_sdp(sess);

    // Mono should not have /channels suffix
    EXPECT_NE(sdp.find("a=rtpmap:10 L24/48000\r\n"), std::string::npos);
}

// --- SAP Packet ---

TEST(Aes67Sap, PacketFormat) {
    Aes67Session sess;
    sess.session_name = "TestSAP";
    sess.origin_address = "192.168.1.10";
    sess.session_id = 1;
    sess.session_version = 1;
    sess.multicast_group = "239.69.0.1";
    sess.rtp_port = 5004;

    uint8_t buf[2048];
    size_t len = aes67_build_sap_packet(sess, buf, sizeof(buf));

    ASSERT_GT(len, sizeof(SapHeader));

    // Check SAP header
    SapHeader* hdr = reinterpret_cast<SapHeader*>(buf);
    EXPECT_EQ(hdr->version_flags, 0x20);  // V=1
    EXPECT_EQ(hdr->auth_length, 0);

    // Check payload type string
    const char* pt = reinterpret_cast<const char*>(buf + sizeof(SapHeader));
    EXPECT_STREQ(pt, "application/sdp");

    // Check SDP is present after payload type string
    size_t pt_len = std::strlen("application/sdp") + 1;
    std::string sdp(reinterpret_cast<const char*>(buf + sizeof(SapHeader) + pt_len),
                    len - sizeof(SapHeader) - pt_len);
    EXPECT_NE(sdp.find("v=0"), std::string::npos);
    EXPECT_NE(sdp.find("m=audio"), std::string::npos);
}

TEST(Aes67Sap, BufferTooSmall) {
    Aes67Session sess;
    sess.session_name = "Test";
    sess.origin_address = "192.168.1.10";
    sess.multicast_group = "239.69.0.1";

    uint8_t buf[8]; // way too small
    size_t len = aes67_build_sap_packet(sess, buf, sizeof(buf));
    EXPECT_EQ(len, 0u);
}

TEST(Aes67Sap, HashDeterministic) {
    std::string sdp1 = "v=0\r\no=- 1 1 IN IP4 10.0.0.1\r\n";
    std::string sdp2 = "v=0\r\no=- 1 1 IN IP4 10.0.0.1\r\n";

    EXPECT_EQ(aes67_sap_hash(sdp1), aes67_sap_hash(sdp2));
}

TEST(Aes67Sap, HashDiffers) {
    std::string sdp1 = "v=0\r\no=- 1 1 IN IP4 10.0.0.1\r\n";
    std::string sdp2 = "v=0\r\no=- 2 1 IN IP4 10.0.0.2\r\n";

    EXPECT_NE(aes67_sap_hash(sdp1), aes67_sap_hash(sdp2));
}

// --- AES67 RTP Packet ---

TEST(Aes67Rtp, BuildPacket) {
    uint8_t buf[1500];
    int32_t audio[48]; // 1ms of mono
    std::memset(audio, 0xAB, sizeof(audio));

    size_t len = aes67_build_rtp_packet(buf, sizeof(buf),
        0x12345678, 100, 4800, kPayloadTypeL24,
        audio, sizeof(audio));

    ASSERT_EQ(len, sizeof(RtpHeader) + sizeof(audio));

    // Parse header
    RtpHeader* rtp = reinterpret_cast<RtpHeader*>(buf);
    EXPECT_EQ(rtp->version, 2);
    EXPECT_EQ(rtp->extension, 0);  // No OSTP extension
    EXPECT_EQ(rtp->pt, kPayloadTypeL24);
}

TEST(Aes67Rtp, NoExtensionHeader) {
    uint8_t buf[256];
    uint8_t payload[48];
    std::memset(payload, 0, sizeof(payload));

    size_t len = aes67_build_rtp_packet(buf, sizeof(buf),
        1, 0, 0, kPayloadTypeL24, payload, sizeof(payload));

    RtpHeader* rtp = reinterpret_cast<RtpHeader*>(buf);
    EXPECT_EQ(rtp->extension, 0);
    EXPECT_EQ(len, sizeof(RtpHeader) + sizeof(payload));
}

TEST(Aes67Rtp, BufferTooSmall) {
    uint8_t buf[8];
    uint8_t payload[48];

    size_t len = aes67_build_rtp_packet(buf, sizeof(buf),
        1, 0, 0, kPayloadTypeL24, payload, sizeof(payload));
    EXPECT_EQ(len, 0u);
}

// --- AES67 Detection ---

TEST(Aes67Detect, StandardPayloadType) {
    RtpHeader hdr{};
    hdr.pt = kPayloadTypeL24;
    EXPECT_TRUE(aes67_is_standard_packet(hdr));

    hdr.pt = kPayloadTypeL16;
    EXPECT_TRUE(aes67_is_standard_packet(hdr));
}

TEST(Aes67Detect, NonStandardPayloadType) {
    RtpHeader hdr{};
    hdr.pt = kPayloadTypePCM24;  // Dynamic type 96
    EXPECT_FALSE(aes67_is_standard_packet(hdr));

    hdr.pt = kPayloadTypeF32;
    EXPECT_FALSE(aes67_is_standard_packet(hdr));
}
