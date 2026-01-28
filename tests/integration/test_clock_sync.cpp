/**
 * Integration test: Clock synchronization pipeline
 *
 * Tests the full offset/delay calculation path without network I/O:
 * - Simulate master sending Sync + Follow_Up
 * - Simulate slave receiving and computing offset
 * - Verify convergence of clock servo
 */

#include <soluna/sync/ptp.h>
#include <soluna/sync/clock_servo.h>
#include <soluna/sync/adaptive_pll.h>
#include <soluna/pipeline/playout_buffer.h>
#include <soluna/pipeline/pipeline.h>
#include <gtest/gtest.h>
#include <cmath>

using namespace soluna::sync;
using namespace soluna::pipeline;

TEST(ClockSync, OffsetCalculation) {
    // Simulate PTP offset measurement:
    // t1 = master send time
    // t2 = slave receive time
    // t3 = slave delay_req send time
    // t4 = master delay_req receive time
    //
    // offset = ((t2 - t1) - (t4 - t3)) / 2
    // delay  = ((t2 - t1) + (t4 - t3)) / 2

    // Master is 1000ns ahead of slave
    int64_t true_offset = 1000;  // slave is behind by 1us
    int64_t true_delay  = 50000; // 50us one-way

    // Simulate timestamps
    int64_t t1 = 1'000'000'000LL;                         // master send
    int64_t t2 = t1 + true_delay + true_offset;           // slave recv (local clock)
    int64_t t3 = t2 + 1'000'000;                          // slave delay_req send
    int64_t t4 = t3 + true_delay - true_offset;           // master recv

    double measured_offset = (static_cast<double>(t2 - t1) -
                              static_cast<double>(t4 - t3)) / 2.0;
    double measured_delay  = (static_cast<double>(t2 - t1) +
                              static_cast<double>(t4 - t3)) / 2.0;

    EXPECT_NEAR(measured_offset, static_cast<double>(true_offset), 1.0);
    EXPECT_NEAR(measured_delay, static_cast<double>(true_delay), 1.0);
}

TEST(ClockSync, ServoConvergence) {
    // Simulate a slave syncing to master over multiple iterations
    ClockServoConfig cfg;
    cfg.kp = 0.7;
    cfg.ki = 0.3;
    cfg.filter_weight = 0.5;
    cfg.converged_threshold_ns = 100.0;
    ClockServo servo(cfg);

    double true_offset = 5000.0; // 5us initial offset
    double true_delay = 50000.0;
    double interval_s = 0.125; // 125ms sync interval

    servo.feed_delay(true_delay);

    // Simulate sync loop with realistic feedback
    for (int i = 0; i < 500; i++) {
        double adj = servo.feed_offset(true_offset);
        // Apply ppb correction over sync interval
        true_offset += adj * interval_s;
    }

    // Should converge to near zero
    EXPECT_LT(std::abs(servo.state().offset_ns), 100.0);
    EXPECT_TRUE(servo.state().converged);
}

TEST(ClockSync, PtpMessageRoundtrip) {
    // Full message encode → decode cycle for all PTP message types
    uint8_t buf[256];

    PtpHeader hdr;
    hdr.version = 2;
    hdr.domain_number = 0;
    hdr.source_port_id.clock_id = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
    hdr.source_port_id.port_number = 1;

    // Sync
    {
        hdr.sequence_id = 1;
        PtpTimestamp ts = PtpTimestamp::from_ns(1234567890123LL);
        size_t len = ptp_serialize_sync(buf, sizeof(buf), hdr, ts);
        ASSERT_GT(len, 0u);

        PtpHeader parsed;
        ASSERT_TRUE(ptp_parse_header(buf, len, parsed));
        EXPECT_EQ(parsed.sequence_id, 1u);
        PtpTimestamp parsed_ts;
        ASSERT_TRUE(ptp_parse_timestamp_body(buf, len, parsed_ts));
        EXPECT_EQ(parsed_ts.to_ns(), ts.to_ns());
    }

    // Announce
    {
        hdr.sequence_id = 2;
        PtpAnnounceBody body;
        body.grandmaster_priority1 = 100;
        body.grandmaster_priority2 = 200;
        body.grandmaster_clock_quality.clock_class = PtpClockClass::AppSpecific;
        body.grandmaster_clock_quality.clock_accuracy = PtpClockAccuracy::Within1us;
        body.grandmaster_identity = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

        size_t len = ptp_serialize_announce(buf, sizeof(buf), hdr, body);
        ASSERT_GT(len, 0u);

        PtpAnnounceBody parsed;
        ASSERT_TRUE(ptp_parse_announce(buf, len, parsed));
        EXPECT_EQ(parsed.grandmaster_priority1, 100u);
        EXPECT_EQ(static_cast<uint8_t>(parsed.grandmaster_clock_quality.clock_class),
                  static_cast<uint8_t>(PtpClockClass::AppSpecific));
    }
}

static PlayoutPacket make_cs_packet(uint16_t seq, uint32_t media_ts,
                                  size_t frames, uint32_t channels) {
    PlayoutPacket pkt;
    pkt.sequence = seq;
    pkt.media_timestamp = media_ts;
    pkt.rtp_timestamp = seq * static_cast<uint32_t>(frames);
    pkt.audio_data.resize(frames * channels * sizeof(int32_t), 0);
    pkt.valid = true;
    return pkt;
}

TEST(ClockSync, PlayoutBufferWithSyncedTime) {
    // Simulate PTP-synced playout
    PlayoutBufferConfig cfg;
    cfg.capacity_packets = 32;
    cfg.playout_delay_ns = 2'000'000; // 2ms playout delay
    cfg.channels = 1;
    PlayoutBuffer buf(cfg);

    // Insert packets with media timestamps at 1ms intervals
    for (int i = 0; i < 10; i++) {
        PlayoutPacket pkt;
        pkt.sequence = static_cast<uint16_t>(i);
        pkt.media_timestamp = static_cast<uint32_t>(i * 1'000'000); // every 1ms
        pkt.rtp_timestamp = static_cast<uint32_t>(i * 48);
        pkt.audio_data.resize(48 * sizeof(int32_t), 0);
        pkt.valid = true;
        buf.insert(pkt);
    }

    // At time = playout_delay (2ms), first packet should be ready
    PlayoutPacket out;
    EXPECT_TRUE(buf.read_at(2'000'001, out));
    EXPECT_EQ(out.sequence, 0u);

    // At time = 3ms, second packet should be ready
    EXPECT_TRUE(buf.read_at(3'000'001, out));
    EXPECT_EQ(out.sequence, 1u);

    // At time = 1ms (too early), should underrun
    PlayoutBuffer buf2(cfg);
    buf2.insert(make_cs_packet(0, 5'000'000, 48, 1));
    EXPECT_FALSE(buf2.read_at(5'000'000, out)); // exactly at media_ts, but delay not elapsed
}

TEST(ClockSync, AdaptivePllTracksFreqOffset) {
    AdaptivePllConfig cfg;
    cfg.initial_bandwidth_hz = 5.0;
    cfg.sample_rate = 48000;
    AdaptivePll pll(cfg);

    int64_t local_t = 0;
    int64_t media_t = 0;
    int64_t interval = 1'000'000; // 1ms

    pll.feed(local_t, media_t);

    // Simulate 50ppm frequency offset over 1000 packets
    for (int i = 1; i <= 1000; i++) {
        local_t += interval;
        media_t += interval + 50; // 50ns/ms = 50ppm
        pll.feed(local_t, media_t);
    }

    // Should detect positive offset (media faster)
    EXPECT_GT(pll.state().freq_offset_ppb, 0.0);
}
