#include <soluna/transport/ostp.h>
#include <gtest/gtest.h>
#include <vector>
#include <cstring>

using namespace soluna::transport;

TEST(OstpPacket, BuildAndParse) {
    uint8_t packet[kMaxPacketSize];

    // Create test payload
    int32_t audio_data[] = {1000, 2000, -3000, 4000};
    size_t payload_size = sizeof(audio_data);

    size_t pkt_size = ostp_build_packet(
        packet, sizeof(packet),
        0x12345678,   // ssrc
        42,           // sequence
        9600,         // rtp_timestamp
        kPayloadTypePCM24,
        7,            // stream_id
        1,            // sequence_ext
        20000000,     // media_timestamp
        audio_data, payload_size
    );

    ASSERT_GT(pkt_size, 0u);
    EXPECT_EQ(pkt_size, kTotalHeaderSize + payload_size + kCrcTrailerSize);

    // Parse
    RtpHeader rtp{};
    OstpHeader ostp{};
    const uint8_t* parsed_payload = nullptr;
    size_t parsed_payload_size = 0;

    int rc = ostp_parse_packet(packet, pkt_size, rtp, ostp, parsed_payload, parsed_payload_size);
    ASSERT_EQ(rc, 0);

    EXPECT_EQ(rtp.version, 2);
    EXPECT_EQ(rtp.extension, 1);
    EXPECT_EQ(rtp.pt, kPayloadTypePCM24);
    EXPECT_EQ(rtp.sequence, 42u);
    EXPECT_EQ(rtp.timestamp, 9600u);
    EXPECT_EQ(rtp.ssrc, 0x12345678u);

    EXPECT_EQ(ostp.stream_id, 7u);
    EXPECT_EQ(ostp.sequence_ext, 1u);
    EXPECT_EQ(ostp.media_timestamp, 20000000u);

    EXPECT_EQ(parsed_payload_size, payload_size);
    EXPECT_EQ(std::memcmp(parsed_payload, audio_data, payload_size), 0);
}

TEST(OstpPacket, BufferTooSmall) {
    uint8_t packet[10]; // too small
    int32_t data = 42;

    size_t pkt_size = ostp_build_packet(
        packet, sizeof(packet),
        0, 0, 0, kPayloadTypePCM24,
        0, 0, 0,
        &data, sizeof(data)
    );

    EXPECT_EQ(pkt_size, 0u);
}

TEST(OstpPacket, ParseTooShort) {
    uint8_t packet[4] = {};
    RtpHeader rtp{};
    OstpHeader ostp{};
    const uint8_t* payload = nullptr;
    size_t payload_size = 0;

    int rc = ostp_parse_packet(packet, sizeof(packet), rtp, ostp, payload, payload_size);
    EXPECT_NE(rc, 0);
}

TEST(OstpPacket, ParseBadVersion) {
    uint8_t packet[kMaxPacketSize];
    int32_t data = 0;
    size_t pkt_size = ostp_build_packet(
        packet, sizeof(packet),
        0, 0, 0, kPayloadTypePCM24,
        0, 0, 0, &data, sizeof(data)
    );
    ASSERT_GT(pkt_size, 0u);

    // Corrupt version
    packet[0] = 0; // version = 0
    RtpHeader rtp{};
    OstpHeader ostp{};
    const uint8_t* payload = nullptr;
    size_t payload_size = 0;

    int rc = ostp_parse_packet(packet, pkt_size, rtp, ostp, payload, payload_size);
    EXPECT_NE(rc, 0);
}

TEST(OstpPacket, EmptyPayload) {
    uint8_t packet[kMaxPacketSize];

    size_t pkt_size = ostp_build_packet(
        packet, sizeof(packet),
        0, 0, 0, kPayloadTypePCM24,
        0, 0, 0,
        nullptr, 0
    );

    ASSERT_EQ(pkt_size, kTotalHeaderSize + kCrcTrailerSize);

    RtpHeader rtp{};
    OstpHeader ostp{};
    const uint8_t* payload = nullptr;
    size_t payload_size = 0;

    int rc = ostp_parse_packet(packet, pkt_size, rtp, ostp, payload, payload_size);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(payload_size, 0u);
}

TEST(OstpPacket, SequenceExtension) {
    uint8_t packet[kMaxPacketSize];
    int32_t data = 0;

    // Test large sequence number using extension
    uint16_t seq_lo = 0xFFFF;
    uint16_t seq_hi = 0x0001;

    size_t pkt_size = ostp_build_packet(
        packet, sizeof(packet),
        0, seq_lo, 0, kPayloadTypePCM24,
        1, seq_hi, 0,
        &data, sizeof(data)
    );

    RtpHeader rtp{};
    OstpHeader ostp{};
    const uint8_t* payload = nullptr;
    size_t payload_size = 0;

    int rc = ostp_parse_packet(packet, pkt_size, rtp, ostp, payload, payload_size);
    ASSERT_EQ(rc, 0);

    uint32_t full_seq = (static_cast<uint32_t>(ostp.sequence_ext) << 16) | rtp.sequence;
    EXPECT_EQ(full_seq, 0x0001FFFFu);
}
