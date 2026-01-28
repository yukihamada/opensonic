#include <soluna/transport/ostp.h>
#include <soluna/pal/net.h>
#include <soluna/pipeline/ring_buffer.h>
#include <cstdio>
#include <atomic>
#include <vector>

namespace soluna::transport {

class RtpReceiver {
public:
    struct Config {
        uint16_t listen_port = kPortRTPBase;
        std::string multicast_group = kMulticastAudio;
        uint32_t channels = 1;
        SampleFormat format = SampleFormat::S24_LE;
    };

    struct Stats {
        uint64_t packets_received = 0;
        uint64_t packets_dropped = 0;
        uint64_t sequence_errors = 0;
        int32_t last_sequence = -1;
    };

    explicit RtpReceiver(const Config& config)
        : config_(config)
        , frame_size_(sample_size(config.format) * config.channels)
        , recv_buf_(kMaxPacketSize)
    {
    }

    bool init() {
        socket_ = pal::UdpSocket::create();
        if (!socket_) return false;
        if (!socket_->bind(config_.listen_port)) return false;
        if (!socket_->join_multicast(config_.multicast_group)) return false;
        socket_->set_recv_timeout_ms(1);
        return true;
    }

    // Try to receive one packet and write audio to ring buffer.
    // Returns true if a packet was received and processed.
    bool receive_packet(pipeline::RingBuffer& ring) {
        pal::SocketAddress src;
        int received = socket_->recv_from_nonblock(recv_buf_.data(), recv_buf_.size(), src);
        if (received <= 0) return false;

        RtpHeader rtp;
        OstpHeader ostp;
        const uint8_t* payload = nullptr;
        size_t payload_size = 0;

        if (!ostp_parse_packet(recv_buf_.data(), static_cast<size_t>(received),
                               rtp, ostp, payload, payload_size)) {
            return false;
        }

        // Sequence check
        uint32_t full_seq = (static_cast<uint32_t>(ostp.sequence_ext) << 16) | rtp.sequence;
        if (stats_.last_sequence >= 0) {
            int32_t expected = stats_.last_sequence + 1;
            if (static_cast<int32_t>(full_seq) != expected) {
                stats_.sequence_errors++;
                int32_t gap = static_cast<int32_t>(full_seq) - expected;
                if (gap > 0) {
                    stats_.packets_dropped += static_cast<uint64_t>(gap);
                }
            }
        }
        stats_.last_sequence = static_cast<int32_t>(full_seq);
        stats_.packets_received++;

        // Write payload to ring buffer
        size_t frames = payload_size / frame_size_;
        size_t written = ring.write(payload, frames);
        if (written < frames) {
            // Ring buffer overflow — drop remaining
        }

        return true;
    }

    const Stats& stats() const { return stats_; }

private:
    Config config_;
    size_t frame_size_;
    std::unique_ptr<pal::UdpSocket> socket_;
    std::vector<uint8_t> recv_buf_;
    Stats stats_;
};

} // namespace soluna::transport
