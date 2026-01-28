#include <soluna/transport/ostp.h>
#include <soluna/pal/net.h>
#include <soluna/pal/time.h>
#include <soluna/pipeline/ring_buffer.h>
#include <cstdio>
#include <atomic>
#include <vector>

namespace soluna::transport {

class RtpTransmitter {
public:
    struct Config {
        uint32_t ssrc = 0;
        uint16_t stream_id = 1;
        uint32_t sample_rate = kDefaultSampleRate;
        uint32_t channels = 1;
        SampleFormat format = SampleFormat::S24_LE;
        PacketTier tier = PacketTier::Standard;
        pal::SocketAddress dest;
    };

    explicit RtpTransmitter(const Config& config)
        : config_(config)
        , samples_per_packet_(samples_per_packet(config.tier))
        , frame_size_(sample_size(config.format) * config.channels)
        , packet_buf_(kMaxPacketSize)
    {
    }

    bool init() {
        socket_ = pal::UdpSocket::create();
        if (!socket_) return false;
        socket_->set_dscp(46); // EF for audio
        return true;
    }

    // Send one packet's worth of audio from the ring buffer.
    // Returns true if packet was sent.
    bool send_packet(pipeline::RingBuffer& ring) {
        const size_t frames_needed = samples_per_packet_;

        if (ring.available_read() < frames_needed) {
            return false;
        }

        // Read audio data directly after the header
        uint8_t* payload_ptr = packet_buf_.data() + kTotalHeaderSize;
        size_t payload_size = frames_needed * frame_size_;

        ring.read(payload_ptr, frames_needed);

        // Build the packet
        uint16_t seq_lo = static_cast<uint16_t>(sequence_ & 0xFFFF);
        uint16_t seq_hi = static_cast<uint16_t>((sequence_ >> 16) & 0xFFFF);

        size_t pkt_size = ostp_build_packet(
            packet_buf_.data(), packet_buf_.size(),
            config_.ssrc, seq_lo, rtp_timestamp_,
            kPayloadTypePCM24,
            config_.stream_id, seq_hi,
            media_timestamp_,
            payload_ptr, payload_size
        );

        if (pkt_size == 0) return false;

        int sent = socket_->send_to(packet_buf_.data(), pkt_size, config_.dest);
        if (sent <= 0) return false;

        sequence_++;
        rtp_timestamp_ += static_cast<uint32_t>(frames_needed);
        media_timestamp_ += static_cast<uint32_t>(
            (frames_needed * 1'000'000'000ULL) / config_.sample_rate);

        return true;
    }

    uint64_t packets_sent() const { return sequence_; }

private:
    Config config_;
    uint32_t samples_per_packet_;
    size_t frame_size_;
    std::unique_ptr<pal::UdpSocket> socket_;
    std::vector<uint8_t> packet_buf_;

    uint64_t sequence_ = 0;
    uint32_t rtp_timestamp_ = 0;
    uint32_t media_timestamp_ = 0;
};

} // namespace soluna::transport
