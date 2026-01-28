#include <soluna/pipeline/ring_buffer.h>
#include <soluna/pipeline/pipeline.h>
#include <soluna/transport/ostp.h>
#include <soluna/pal/net.h>
#include <soluna/pal/time.h>
#include <gtest/gtest.h>

#include <thread>
#include <atomic>
#include <vector>
#include <cmath>

using namespace soluna;
using namespace soluna::pipeline;
using namespace soluna::transport;
using namespace soluna::pal;

/**
 * Loopback test: TX → UDP loopback → RX on same machine.
 * Verifies that audio data survives the RTP/OSTP encode/decode cycle.
 */
TEST(Loopback, SinglePacketRoundtrip) {
    // Generate test audio: 48 samples of 440Hz sine at 48kHz, mono
    constexpr uint32_t kFrames = 48;
    constexpr uint32_t kChannels = 1;
    constexpr size_t kFrameSize = sizeof(int32_t) * kChannels;

    std::vector<float> tx_float(kFrames);
    for (uint32_t i = 0; i < kFrames; i++) {
        tx_float[i] = std::sin(2.0 * M_PI * 440.0 * i / 48000.0);
    }

    // Convert to S24
    std::vector<int32_t> tx_s24(kFrames);
    float_to_s24(tx_float.data(), tx_s24.data(), kFrames);

    // Build OSTP packet
    std::vector<uint8_t> packet(kMaxPacketSize);
    size_t pkt_size = ostp_build_packet(
        packet.data(), packet.size(),
        0xDEADBEEF, 0, 0, kPayloadTypePCM24,
        1, 0, 0,
        tx_s24.data(), kFrames * kFrameSize
    );
    ASSERT_GT(pkt_size, 0u);

    // Parse it back
    RtpHeader rtp{};
    OstpHeader ostp{};
    const uint8_t* payload = nullptr;
    size_t payload_size = 0;

    bool ok = ostp_parse_packet(packet.data(), pkt_size, rtp, ostp, payload, payload_size);
    ASSERT_TRUE(ok);
    ASSERT_EQ(payload_size, kFrames * kFrameSize);

    // Convert back to float
    const auto* rx_s24 = reinterpret_cast<const int32_t*>(payload);
    std::vector<float> rx_float(kFrames);
    s24_to_float(rx_s24, rx_float.data(), kFrames);

    // Verify roundtrip accuracy
    for (uint32_t i = 0; i < kFrames; i++) {
        EXPECT_NEAR(rx_float[i], tx_float[i], 1.0f / 8388607.0f)
            << "Sample mismatch at frame " << i;
    }
}

/**
 * UDP loopback: send and receive on localhost.
 * Tests the full network path (excluding audio hardware).
 */
TEST(Loopback, UdpRoundtrip) {
    constexpr uint16_t kTestPort = 15004; // use a high port to avoid permission issues
    constexpr uint32_t kFrames = 48;
    constexpr uint32_t kChannels = 1;
    constexpr size_t kFrameSize = sizeof(int32_t) * kChannels;

    // Create RX socket
    auto rx_sock = UdpSocket::create();
    ASSERT_NE(rx_sock, nullptr);
    ASSERT_TRUE(rx_sock->bind(kTestPort));
    rx_sock->set_recv_timeout_ms(1000);

    // Create TX socket
    auto tx_sock = UdpSocket::create();
    ASSERT_NE(tx_sock, nullptr);

    // Generate test data
    std::vector<int32_t> tx_data(kFrames);
    for (uint32_t i = 0; i < kFrames; i++) {
        tx_data[i] = static_cast<int32_t>(i * 1000);
    }

    // Build packet
    std::vector<uint8_t> packet(kMaxPacketSize);
    size_t pkt_size = ostp_build_packet(
        packet.data(), packet.size(),
        0x42, 1, 0, kPayloadTypePCM24,
        1, 0, 0,
        tx_data.data(), kFrames * kFrameSize
    );
    ASSERT_GT(pkt_size, 0u);

    // Send to localhost
    SocketAddress dest{"127.0.0.1", kTestPort};
    int sent = tx_sock->send_to(packet.data(), pkt_size, dest);
    EXPECT_EQ(static_cast<size_t>(sent), pkt_size);

    // Receive
    std::vector<uint8_t> recv_buf(kMaxPacketSize);
    SocketAddress src;
    int received = rx_sock->recv_from(recv_buf.data(), recv_buf.size(), src);
    ASSERT_GT(received, 0);

    // Parse
    RtpHeader rtp{};
    OstpHeader ostp{};
    const uint8_t* payload = nullptr;
    size_t payload_size = 0;

    bool ok = ostp_parse_packet(recv_buf.data(), static_cast<size_t>(received),
                                 rtp, ostp, payload, payload_size);
    ASSERT_TRUE(ok);
    EXPECT_EQ(rtp.ssrc, 0x42u);
    EXPECT_EQ(rtp.sequence, 1u);
    EXPECT_EQ(payload_size, kFrames * kFrameSize);

    // Verify data
    const auto* rx_data = reinterpret_cast<const int32_t*>(payload);
    for (uint32_t i = 0; i < kFrames; i++) {
        EXPECT_EQ(rx_data[i], tx_data[i]) << "Mismatch at frame " << i;
    }
}

/**
 * Pipeline integration: ring buffer → OSTP → ring buffer.
 * Simulates the full audio pipeline without hardware.
 */
TEST(Loopback, PipelineRoundtrip) {
    constexpr uint32_t kFrames = 48;
    constexpr uint32_t kChannels = 1;
    constexpr size_t kFrameSize = sizeof(int32_t) * kChannels;
    constexpr uint32_t kPacketCount = 100;

    RingBuffer tx_ring(kFrames * kPacketCount, kFrameSize);
    RingBuffer rx_ring(kFrames * kPacketCount, kFrameSize);

    // Fill TX ring with known data
    for (uint32_t p = 0; p < kPacketCount; p++) {
        std::vector<int32_t> data(kFrames);
        for (uint32_t i = 0; i < kFrames; i++) {
            data[i] = static_cast<int32_t>(p * kFrames + i);
        }
        tx_ring.write(data.data(), kFrames);
    }

    // Process: TX ring → OSTP packets → RX ring
    std::vector<uint8_t> packet(kMaxPacketSize);
    std::vector<int32_t> audio_buf(kFrames);

    for (uint32_t p = 0; p < kPacketCount; p++) {
        // Read from TX ring
        size_t read = tx_ring.read(audio_buf.data(), kFrames);
        ASSERT_EQ(read, kFrames);

        // Build packet
        size_t pkt_size = ostp_build_packet(
            packet.data(), packet.size(),
            0, static_cast<uint16_t>(p), 0, kPayloadTypePCM24,
            1, 0, 0,
            audio_buf.data(), kFrames * kFrameSize
        );
        ASSERT_GT(pkt_size, 0u);

        // Parse packet
        RtpHeader rtp{};
        OstpHeader ostp{};
        const uint8_t* payload = nullptr;
        size_t payload_size = 0;

        bool ok = ostp_parse_packet(packet.data(), pkt_size, rtp, ostp, payload, payload_size);
        ASSERT_TRUE(ok);

        // Write to RX ring
        size_t frames = payload_size / kFrameSize;
        rx_ring.write(payload, frames);
    }

    // Verify all data
    for (uint32_t p = 0; p < kPacketCount; p++) {
        std::vector<int32_t> data(kFrames);
        size_t read = rx_ring.read(data.data(), kFrames);
        ASSERT_EQ(read, kFrames);

        for (uint32_t i = 0; i < kFrames; i++) {
            EXPECT_EQ(data[i], static_cast<int32_t>(p * kFrames + i))
                << "Mismatch at packet " << p << " frame " << i;
        }
    }
}
