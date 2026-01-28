#include <soluna/sync/ptp.h>
#include <gtest/gtest.h>
#include <cstring>

using namespace soluna::sync;

// ---- Timestamp ----

TEST(PtpTimestamp, FromNsRoundtrip) {
    int64_t ns = 1'234'567'890'123'456'789LL;
    auto ts = PtpTimestamp::from_ns(ns);
    EXPECT_EQ(ts.to_ns(), ns);
}

TEST(PtpTimestamp, FromPalRoundtrip) {
    soluna::pal::Timestamp pal_ts;
    pal_ts.seconds = 1234567;
    pal_ts.nanoseconds = 890123456;

    auto ptp_ts = PtpTimestamp::from_pal(pal_ts);
    auto back = ptp_ts.to_pal();
    EXPECT_EQ(back.seconds, pal_ts.seconds);
    EXPECT_EQ(back.nanoseconds, pal_ts.nanoseconds);
}

// ---- Sync message ----

TEST(PtpMessages, SyncSerializeDeserialize) {
    PtpHeader hdr;
    hdr.version = 2;
    hdr.domain_number = 0;
    hdr.sequence_id = 42;
    hdr.source_port_id.clock_id = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    hdr.source_port_id.port_number = 1;
    hdr.log_message_interval = -3;

    PtpTimestamp origin = PtpTimestamp::from_ns(1'500'000'000'000LL);

    uint8_t buf[kPtpSyncSize];
    size_t len = ptp_serialize_sync(buf, sizeof(buf), hdr, origin);
    ASSERT_EQ(len, kPtpSyncSize);

    PtpHeader parsed_hdr;
    ASSERT_TRUE(ptp_parse_header(buf, len, parsed_hdr));
    EXPECT_EQ(parsed_hdr.message_type, PtpMessageType::Sync);
    EXPECT_EQ(parsed_hdr.version, 2);
    EXPECT_EQ(parsed_hdr.sequence_id, 42u);
    EXPECT_EQ(parsed_hdr.source_port_id.clock_id[0], 0x01);
    EXPECT_EQ(parsed_hdr.source_port_id.port_number, 1u);
    EXPECT_EQ(parsed_hdr.log_message_interval, -3);

    PtpTimestamp parsed_ts;
    ASSERT_TRUE(ptp_parse_timestamp_body(buf, len, parsed_ts));
    EXPECT_EQ(parsed_ts.to_ns(), 1'500'000'000'000LL);
}

// ---- Follow_Up message ----

TEST(PtpMessages, FollowUpSerializeDeserialize) {
    PtpHeader hdr;
    hdr.version = 2;
    hdr.domain_number = 0;
    hdr.sequence_id = 100;
    hdr.source_port_id.clock_id = {0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33, 0x44};
    hdr.source_port_id.port_number = 2;

    PtpTimestamp precise_ts = PtpTimestamp::from_ns(999'999'999'999LL);

    uint8_t buf[kPtpFollowUpSize];
    size_t len = ptp_serialize_follow_up(buf, sizeof(buf), hdr, precise_ts);
    ASSERT_EQ(len, kPtpFollowUpSize);

    PtpHeader parsed_hdr;
    ASSERT_TRUE(ptp_parse_header(buf, len, parsed_hdr));
    EXPECT_EQ(parsed_hdr.message_type, PtpMessageType::FollowUp);
    EXPECT_EQ(parsed_hdr.sequence_id, 100u);

    PtpTimestamp parsed_ts;
    ASSERT_TRUE(ptp_parse_timestamp_body(buf, len, parsed_ts));
    EXPECT_EQ(parsed_ts.to_ns(), 999'999'999'999LL);
}

// ---- Delay_Req message ----

TEST(PtpMessages, DelayReqSerializeDeserialize) {
    PtpHeader hdr;
    hdr.version = 2;
    hdr.domain_number = 0;
    hdr.sequence_id = 7;
    hdr.source_port_id.clock_id.fill(0x55);
    hdr.source_port_id.port_number = 1;

    PtpTimestamp ts = PtpTimestamp::from_ns(12345678901234LL);

    uint8_t buf[kPtpDelayReqSize];
    size_t len = ptp_serialize_delay_req(buf, sizeof(buf), hdr, ts);
    ASSERT_EQ(len, kPtpDelayReqSize);

    PtpHeader parsed_hdr;
    ASSERT_TRUE(ptp_parse_header(buf, len, parsed_hdr));
    EXPECT_EQ(parsed_hdr.message_type, PtpMessageType::DelayReq);
}

// ---- Delay_Resp message ----

TEST(PtpMessages, DelayRespSerializeDeserialize) {
    PtpHeader hdr;
    hdr.version = 2;
    hdr.domain_number = 0;
    hdr.sequence_id = 7;
    hdr.source_port_id.clock_id.fill(0xAA);
    hdr.source_port_id.port_number = 1;

    PtpDelayRespBody body;
    body.receive_timestamp = PtpTimestamp::from_ns(5000000000LL);
    body.requesting_port_id.clock_id = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    body.requesting_port_id.port_number = 3;

    uint8_t buf[kPtpDelayRespSize];
    size_t len = ptp_serialize_delay_resp(buf, sizeof(buf), hdr, body);
    ASSERT_EQ(len, kPtpDelayRespSize);

    PtpHeader parsed_hdr;
    ASSERT_TRUE(ptp_parse_header(buf, len, parsed_hdr));
    EXPECT_EQ(parsed_hdr.message_type, PtpMessageType::DelayResp);

    PtpDelayRespBody parsed_body;
    ASSERT_TRUE(ptp_parse_delay_resp(buf, len, parsed_body));
    EXPECT_EQ(parsed_body.receive_timestamp.to_ns(), 5000000000LL);
    EXPECT_EQ(parsed_body.requesting_port_id.clock_id[0], 0x01);
    EXPECT_EQ(parsed_body.requesting_port_id.port_number, 3u);
}

// ---- Announce message ----

TEST(PtpMessages, AnnounceSerializeDeserialize) {
    PtpHeader hdr;
    hdr.version = 2;
    hdr.domain_number = 0;
    hdr.sequence_id = 99;
    hdr.source_port_id.clock_id.fill(0xBB);
    hdr.source_port_id.port_number = 1;

    PtpAnnounceBody body;
    body.grandmaster_priority1 = 100;
    body.grandmaster_priority2 = 200;
    body.grandmaster_clock_quality.clock_class = PtpClockClass::Default;
    body.grandmaster_clock_quality.clock_accuracy = PtpClockAccuracy::Within1us;
    body.grandmaster_clock_quality.offset_scaled_log_variance = 0x4E5D;
    body.grandmaster_identity = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    body.steps_removed = 0;
    body.time_source = 0xA0;

    uint8_t buf[kPtpAnnounceSize];
    size_t len = ptp_serialize_announce(buf, sizeof(buf), hdr, body);
    ASSERT_EQ(len, kPtpAnnounceSize);

    PtpHeader parsed_hdr;
    ASSERT_TRUE(ptp_parse_header(buf, len, parsed_hdr));
    EXPECT_EQ(parsed_hdr.message_type, PtpMessageType::Announce);

    PtpAnnounceBody parsed_body;
    ASSERT_TRUE(ptp_parse_announce(buf, len, parsed_body));
    EXPECT_EQ(parsed_body.grandmaster_priority1, 100u);
    EXPECT_EQ(parsed_body.grandmaster_priority2, 200u);
    EXPECT_EQ(static_cast<uint8_t>(parsed_body.grandmaster_clock_quality.clock_class),
              static_cast<uint8_t>(PtpClockClass::Default));
    EXPECT_EQ(static_cast<uint8_t>(parsed_body.grandmaster_clock_quality.clock_accuracy),
              static_cast<uint8_t>(PtpClockAccuracy::Within1us));
    EXPECT_EQ(parsed_body.grandmaster_clock_quality.offset_scaled_log_variance, 0x4E5Du);
    EXPECT_EQ(parsed_body.grandmaster_identity[0], 0x11u);
    EXPECT_EQ(parsed_body.steps_removed, 0u);
}

// ---- Buffer too small ----

TEST(PtpMessages, BufferTooSmall) {
    uint8_t buf[10];
    PtpHeader hdr{};
    hdr.version = 2;

    EXPECT_EQ(ptp_serialize_sync(buf, sizeof(buf), hdr, {}), 0u);
    EXPECT_EQ(ptp_serialize_follow_up(buf, sizeof(buf), hdr, {}), 0u);
    EXPECT_EQ(ptp_serialize_delay_req(buf, sizeof(buf), hdr, {}), 0u);
    EXPECT_EQ(ptp_serialize_delay_resp(buf, sizeof(buf), hdr, {}), 0u);
    EXPECT_EQ(ptp_serialize_announce(buf, sizeof(buf), hdr, {}), 0u);
}

TEST(PtpMessages, ParseHeaderTooShort) {
    uint8_t buf[10] = {};
    PtpHeader hdr;
    EXPECT_FALSE(ptp_parse_header(buf, sizeof(buf), hdr));
}

TEST(PtpMessages, ParseHeaderBadVersion) {
    uint8_t buf[kPtpSyncSize] = {};
    PtpHeader hdr{};
    hdr.version = 2;
    ptp_serialize_sync(buf, sizeof(buf), hdr, {});

    // Corrupt version
    buf[1] = 0x03;
    PtpHeader parsed;
    EXPECT_FALSE(ptp_parse_header(buf, kPtpSyncSize, parsed));
}
