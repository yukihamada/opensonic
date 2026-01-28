#include <soluna/pipeline/playout_buffer.h>
#include <soluna/pipeline/pipeline.h>
#include <gtest/gtest.h>
#include <cstring>

using namespace soluna::pipeline;

static PlayoutPacket make_packet(uint16_t seq, uint32_t media_ts, size_t frames = 48,
                                  uint32_t channels = 1) {
    PlayoutPacket pkt;
    pkt.sequence = seq;
    pkt.media_timestamp = media_ts;
    pkt.rtp_timestamp = seq * 48;
    pkt.audio_data.resize(frames * channels * sizeof(int32_t), 0);
    // Fill with recognizable pattern
    auto* data = reinterpret_cast<int32_t*>(pkt.audio_data.data());
    for (size_t i = 0; i < frames * channels; i++) {
        data[i] = static_cast<int32_t>(seq * 1000 + i);
    }
    pkt.valid = true;
    return pkt;
}

TEST(PlayoutBuffer, InsertAndRead) {
    PlayoutBufferConfig cfg;
    cfg.capacity_packets = 16;
    cfg.playout_delay_ns = 0; // no delay for testing
    PlayoutBuffer buf(cfg);

    // Insert packet at media_ts = 1000000 (1ms)
    auto pkt = make_packet(0, 1'000'000);
    EXPECT_TRUE(buf.insert(pkt));

    auto stats = buf.stats();
    EXPECT_EQ(stats.packets_received, 1u);
    EXPECT_EQ(stats.current_depth, 1);

    // Read at playout time
    PlayoutPacket out;
    bool ok = buf.read_at(1'000'001, out); // slightly after playout time
    EXPECT_TRUE(ok);
    EXPECT_EQ(out.sequence, 0u);

    stats = buf.stats();
    EXPECT_EQ(stats.packets_played, 1u);
    EXPECT_EQ(stats.current_depth, 0);
}

TEST(PlayoutBuffer, ReadTooEarly) {
    PlayoutBufferConfig cfg;
    cfg.capacity_packets = 16;
    cfg.playout_delay_ns = 1'000'000; // 1ms delay
    PlayoutBuffer buf(cfg);

    auto pkt = make_packet(0, 1'000'000);
    buf.insert(pkt);

    // Read too early (before playout delay)
    PlayoutPacket out;
    bool ok = buf.read_at(1'500'000, out); // only 0.5ms into playout delay
    EXPECT_FALSE(ok);

    // Read at correct time (media_ts + delay)
    ok = buf.read_at(2'000'001, out);
    EXPECT_TRUE(ok);
}

TEST(PlayoutBuffer, OrderedPlayback) {
    PlayoutBufferConfig cfg;
    cfg.capacity_packets = 16;
    cfg.playout_delay_ns = 0;
    PlayoutBuffer buf(cfg);

    // Insert out of order
    buf.insert(make_packet(2, 3'000'000));
    buf.insert(make_packet(0, 1'000'000));
    buf.insert(make_packet(1, 2'000'000));

    // Read should return lowest sequence first
    PlayoutPacket out;
    EXPECT_TRUE(buf.read_at(10'000'000, out));
    EXPECT_EQ(out.sequence, 0u);

    EXPECT_TRUE(buf.read_at(10'000'000, out));
    EXPECT_EQ(out.sequence, 1u);

    EXPECT_TRUE(buf.read_at(10'000'000, out));
    EXPECT_EQ(out.sequence, 2u);
}

TEST(PlayoutBuffer, Overflow) {
    PlayoutBufferConfig cfg;
    cfg.capacity_packets = 4;
    cfg.playout_delay_ns = 0;
    PlayoutBuffer buf(cfg);

    for (int i = 0; i < 4; i++) {
        EXPECT_TRUE(buf.insert(make_packet(i, i * 1'000'000)));
    }

    // 5th packet should be dropped
    EXPECT_FALSE(buf.insert(make_packet(4, 4'000'000)));

    auto stats = buf.stats();
    EXPECT_EQ(stats.packets_dropped_overflow, 1u);
}

TEST(PlayoutBuffer, Underrun) {
    PlayoutBufferConfig cfg;
    cfg.capacity_packets = 16;
    cfg.playout_delay_ns = 0;
    PlayoutBuffer buf(cfg);

    // Don't insert anything
    PlayoutPacket out;
    bool ok = buf.read_at(1'000'000, out);
    EXPECT_FALSE(ok);
}

TEST(PlayoutBuffer, MultiChannelFrameRead) {
    PlayoutBufferConfig cfg;
    cfg.capacity_packets = 16;
    cfg.playout_delay_ns = 0;
    cfg.channels = 2;
    PlayoutBuffer buf(cfg);

    // Insert a stereo packet (48 frames x 2 channels)
    auto pkt = make_packet(0, 1'000'000, 48, 2);
    buf.insert(pkt);

    // Read as float
    float output[48 * 2];
    size_t frames = buf.read_frames(2'000'000, output, 48, 2);
    EXPECT_EQ(frames, 48u);
}

TEST(PlayoutBuffer, Reset) {
    PlayoutBufferConfig cfg;
    cfg.capacity_packets = 16;
    cfg.playout_delay_ns = 0;
    PlayoutBuffer buf(cfg);

    buf.insert(make_packet(0, 1'000'000));
    buf.insert(make_packet(1, 2'000'000));

    buf.reset();
    auto stats = buf.stats();
    EXPECT_EQ(stats.current_depth, 0);
    EXPECT_EQ(stats.packets_received, 0u);
}
