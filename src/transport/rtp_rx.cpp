#include <soluna/transport/ostp.h>
#include <soluna/transport/aes67.h>
#include <soluna/transport/transport_manager.h>
#include <soluna/pal/net.h>
#include <soluna/pipeline/ring_buffer.h>
#include <cstdio>
#include <atomic>
#include <vector>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace soluna::transport {

/**
 * Receive mode for RTP reception.
 */
enum class ReceiveMode {
    OSTP,      // Expect Soluna Transport Protocol packets
    AES67,     // Expect AES67-compatible standard RTP packets
    Auto,      // Auto-detect based on payload type
};

class RtpReceiver {
public:
    struct Config {
        uint16_t listen_port = kPortRTPBase;
        std::string multicast_group = kMulticastAudio;
        uint32_t channels = 1;
        SampleFormat format = SampleFormat::S24_LE;
        ReceiveMode mode = ReceiveMode::Auto;
    };

    struct Stats {
        uint64_t packets_received = 0;
        uint64_t packets_dropped = 0;
        uint64_t sequence_errors = 0;
        uint64_t aes67_packets = 0;
        uint64_t ostp_packets = 0;
        int32_t last_sequence = -1;
    };

    explicit RtpReceiver(const Config& config)
        : config_(config)
        , frame_size_(sample_size(config.format) * config.channels)
        , recv_buf_(kMaxPacketSize)
        , audio_buf_(kMaxPayloadSize / sizeof(int32_t))
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

    /**
     * Initialize with TransportManager for optional DTLS encryption.
     */
    bool init(TransportManager& transport_mgr) {
        transport_socket_ = transport_mgr.create_rx_socket();
        if (!transport_socket_) return false;
        if (!transport_socket_->bind(config_.listen_port)) return false;
        if (!transport_socket_->join_multicast(config_.multicast_group)) return false;
        transport_socket_->set_recv_timeout_ms(1);
        return true;
    }

    /**
     * Set receive mode.
     */
    void set_mode(ReceiveMode mode) {
        config_.mode = mode;
    }

    /**
     * Get current receive mode.
     */
    ReceiveMode mode() const { return config_.mode; }

    // Try to receive one packet and write audio to ring buffer.
    // Returns true if a packet was received and processed.
    bool receive_packet(pipeline::RingBuffer& ring) {
        pal::SocketAddress src;
        int received = recv_packet_data(recv_buf_.data(), recv_buf_.size(), src);
        if (received <= 0) return false;

        // Check if this is an AES67 packet or OSTP packet
        bool is_aes67 = false;
        if (config_.mode == ReceiveMode::Auto && static_cast<size_t>(received) >= sizeof(RtpHeader)) {
            const RtpHeader* rtp = reinterpret_cast<const RtpHeader*>(recv_buf_.data());
            is_aes67 = aes67_is_standard_packet(*rtp);
        } else if (config_.mode == ReceiveMode::AES67) {
            is_aes67 = true;
        }

        if (is_aes67) {
            return receive_aes67_packet(recv_buf_.data(), static_cast<size_t>(received), ring);
        } else {
            return receive_ostp_packet(recv_buf_.data(), static_cast<size_t>(received), ring);
        }
    }

    const Stats& stats() const { return stats_; }

private:
    bool receive_ostp_packet(const uint8_t* data, size_t len, pipeline::RingBuffer& ring) {
        RtpHeader rtp;
        OstpHeader ostp;
        const uint8_t* payload = nullptr;
        size_t payload_size = 0;

        if (ostp_parse_packet(data, len, rtp, ostp, payload, payload_size) != 0) {
            return false;
        }

        // Sequence check
        uint32_t full_seq = (static_cast<uint32_t>(ostp.sequence_ext) << 16) | rtp.sequence;
        check_sequence(full_seq);

        stats_.packets_received++;
        stats_.ostp_packets++;

        // Write payload to ring buffer
        size_t frames = payload_size / frame_size_;
        ring.write(payload, frames);

        return true;
    }

    bool receive_aes67_packet(const uint8_t* data, size_t len, pipeline::RingBuffer& ring) {
        if (len < sizeof(RtpHeader)) {
            return false;
        }

        // Parse RTP header
        RtpHeader rtp;
        std::memcpy(&rtp, data, sizeof(RtpHeader));

        // Convert from network byte order
        uint16_t sequence = ntohs(rtp.sequence);
        // uint32_t timestamp = ntohl(rtp.timestamp);  // Not used currently

        // Sequence check (16-bit only for AES67)
        uint32_t full_seq = sequence;
        if (stats_.last_sequence >= 0) {
            // Handle 16-bit wraparound
            int32_t last16 = stats_.last_sequence & 0xFFFF;
            int32_t diff = static_cast<int32_t>(sequence) - last16;
            if (diff < -32768) diff += 65536;
            if (diff > 32768) diff -= 65536;
            if (diff != 1) {
                stats_.sequence_errors++;
                if (diff > 0) {
                    stats_.packets_dropped += static_cast<uint64_t>(diff - 1);
                }
            }
        }
        stats_.last_sequence = sequence;
        stats_.packets_received++;
        stats_.aes67_packets++;

        // Extract payload
        const uint8_t* payload = data + sizeof(RtpHeader);
        size_t payload_size = len - sizeof(RtpHeader);

        // Convert payload based on payload type
        size_t samples = 0;
        if (rtp.pt == kPayloadTypeL24) {
            // L24: 24-bit packed big-endian → S24_LE (32-bit container)
            samples = payload_size / 3;
            for (size_t i = 0; i < samples && i < audio_buf_.size(); i++) {
                int32_t sample = (static_cast<int32_t>(payload[i * 3]) << 16)
                               | (static_cast<int32_t>(payload[i * 3 + 1]) << 8)
                               | static_cast<int32_t>(payload[i * 3 + 2]);
                // Sign extend from 24-bit
                if (sample & 0x800000) {
                    sample |= 0xFF000000;
                }
                audio_buf_[i] = sample;
            }
        } else if (rtp.pt == kPayloadTypeL16) {
            // L16: 16-bit big-endian → S24_LE (shifted to 24-bit range)
            samples = payload_size / 2;
            const int16_t* src = reinterpret_cast<const int16_t*>(payload);
            for (size_t i = 0; i < samples && i < audio_buf_.size(); i++) {
                // Swap bytes (big-endian to little-endian)
                int16_t be_sample = src[i];
                int16_t sample = static_cast<int16_t>((be_sample >> 8) | (be_sample << 8));
                // Shift to 24-bit range
                audio_buf_[i] = static_cast<int32_t>(sample) << 8;
            }
        } else {
            // Unknown payload type
            return false;
        }

        // Write to ring buffer
        size_t frames = samples / config_.channels;
        ring.write(audio_buf_.data(), frames);

        return true;
    }

    void check_sequence(uint32_t full_seq) {
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
    }

    int recv_packet_data(void* data, size_t len, pal::SocketAddress& src) {
        if (transport_socket_) {
            return transport_socket_->recv_from_nonblock(data, len, src);
        } else if (socket_) {
            return socket_->recv_from_nonblock(data, len, src);
        }
        return -1;
    }

    Config config_;
    size_t frame_size_;
    std::unique_ptr<pal::UdpSocket> socket_;
    std::unique_ptr<TransportSocket> transport_socket_;
    std::vector<uint8_t> recv_buf_;
    std::vector<int32_t> audio_buf_;
    Stats stats_;
};

} // namespace soluna::transport
