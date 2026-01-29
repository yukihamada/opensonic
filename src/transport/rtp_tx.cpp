#include <soluna/transport/ostp.h>
#include <soluna/transport/aes67.h>
#include <soluna/transport/transport_manager.h>
#include <soluna/pal/net.h>
#include <soluna/pal/time.h>
#include <soluna/pipeline/ring_buffer.h>
#include <cstdio>
#include <atomic>
#include <vector>
#include <functional>

namespace soluna::transport {

/**
 * Transport mode for RTP transmission.
 */
enum class TransportMode {
    OSTP,   // Soluna Transport Protocol (with extension headers)
    AES67,  // AES67-compatible standard RTP (no extension headers)
};

/**
 * PTP timestamp provider callback.
 * Returns current PTP-synchronized timestamp in nanoseconds.
 */
using PtpTimestampCallback = std::function<int64_t()>;

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
        TransportMode mode = TransportMode::OSTP;
        uint8_t aes67_bit_depth = 24;  // 24 or 16 for AES67 mode
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

    /**
     * Initialize with TransportManager for optional DTLS encryption.
     */
    bool init(TransportManager& transport_mgr) {
        transport_socket_ = transport_mgr.create_tx_socket();
        if (!transport_socket_) return false;
        transport_socket_->set_dscp(46); // EF for audio

        // For DTLS, need to establish secure channel
        if (transport_mgr.is_dtls_enabled()) {
            auto* secure = dynamic_cast<SecureTransportSocket*>(transport_socket_.get());
            if (secure && !secure->handshake(config_.dest)) {
                return false;
            }
        }

        return true;
    }

    /**
     * Set PTP timestamp provider for AES67 mode.
     * When set, media timestamps will use PTP-synchronized time.
     */
    void set_ptp_timestamp_callback(PtpTimestampCallback cb) {
        ptp_timestamp_cb_ = std::move(cb);
    }

    /**
     * Set transport mode (OSTP or AES67).
     */
    void set_mode(TransportMode mode) {
        config_.mode = mode;
    }

    /**
     * Get current transport mode.
     */
    TransportMode mode() const { return config_.mode; }

    // Send one packet's worth of audio from the ring buffer.
    // Returns true if packet was sent.
    bool send_packet(pipeline::RingBuffer& ring) {
        const size_t frames_needed = samples_per_packet_;

        if (ring.available_read() < frames_needed) {
            return false;
        }

        size_t pkt_size = 0;

        if (config_.mode == TransportMode::AES67) {
            pkt_size = build_aes67_packet(ring, frames_needed);
        } else {
            pkt_size = build_ostp_packet(ring, frames_needed);
        }

        if (pkt_size == 0) return false;

        int sent = send_packet_data(packet_buf_.data(), pkt_size);
        if (sent <= 0) return false;

        sequence_++;
        rtp_timestamp_ += static_cast<uint32_t>(frames_needed);

        if (config_.mode == TransportMode::AES67 && ptp_timestamp_cb_) {
            // In AES67 mode with PTP, media timestamp is derived from PTP time
            // Already handled in build_aes67_packet
        } else {
            media_timestamp_ += static_cast<uint32_t>(
                (frames_needed * 1'000'000'000ULL) / config_.sample_rate);
        }

        return true;
    }

    uint64_t packets_sent() const { return sequence_; }

private:
    size_t build_ostp_packet(pipeline::RingBuffer& ring, size_t frames_needed) {
        // Read audio data directly after the header
        uint8_t* payload_ptr = packet_buf_.data() + kTotalHeaderSize;
        size_t payload_size = frames_needed * frame_size_;

        ring.read(payload_ptr, frames_needed);

        // Build the packet
        uint16_t seq_lo = static_cast<uint16_t>(sequence_ & 0xFFFF);
        uint16_t seq_hi = static_cast<uint16_t>((sequence_ >> 16) & 0xFFFF);

        return ostp_build_packet(
            packet_buf_.data(), packet_buf_.size(),
            config_.ssrc, seq_lo, rtp_timestamp_,
            kPayloadTypePCM24,
            config_.stream_id, seq_hi,
            media_timestamp_,
            payload_ptr, payload_size
        );
    }

    size_t build_aes67_packet(pipeline::RingBuffer& ring, size_t frames_needed) {
        // Read audio data to temp buffer
        std::vector<int32_t> audio_data(frames_needed * config_.channels);
        ring.read(audio_data.data(), frames_needed);

        // Determine payload type based on bit depth
        uint8_t payload_type = (config_.aes67_bit_depth == 16)
            ? kPayloadTypeL16
            : kPayloadTypeL24;

        // For AES67, use PTP-synchronized timestamp if available
        uint32_t rtp_ts = rtp_timestamp_;
        if (ptp_timestamp_cb_) {
            // Convert PTP nanoseconds to RTP timestamp units
            int64_t ptp_ns = ptp_timestamp_cb_();
            rtp_ts = static_cast<uint32_t>((ptp_ns * config_.sample_rate) / 1'000'000'000LL);
        }

        // Convert audio to network format
        size_t bytes_per_sample = (config_.aes67_bit_depth == 16) ? 2 : 3;
        size_t payload_size = frames_needed * config_.channels * bytes_per_sample;
        std::vector<uint8_t> payload(payload_size);

        if (config_.aes67_bit_depth == 16) {
            // Convert S24 to S16 big-endian (AES67 uses network byte order)
            auto* out = reinterpret_cast<int16_t*>(payload.data());
            for (size_t i = 0; i < frames_needed * config_.channels; i++) {
                int16_t sample = static_cast<int16_t>(audio_data[i] >> 8);
                out[i] = static_cast<int16_t>((sample >> 8) | (sample << 8)); // swap bytes
            }
        } else {
            // Convert S24 to 24-bit big-endian packed
            uint8_t* out = payload.data();
            for (size_t i = 0; i < frames_needed * config_.channels; i++) {
                int32_t sample = audio_data[i];
                // Pack as big-endian 24-bit
                *out++ = static_cast<uint8_t>((sample >> 16) & 0xFF);
                *out++ = static_cast<uint8_t>((sample >> 8) & 0xFF);
                *out++ = static_cast<uint8_t>(sample & 0xFF);
            }
        }

        return aes67_build_rtp_packet(
            packet_buf_.data(), packet_buf_.size(),
            config_.ssrc, static_cast<uint16_t>(sequence_ & 0xFFFF),
            rtp_ts, payload_type,
            payload.data(), payload_size
        );
    }

    int send_packet_data(const void* data, size_t len) {
        if (transport_socket_) {
            return transport_socket_->send_to(data, len, config_.dest);
        } else if (socket_) {
            return socket_->send_to(data, len, config_.dest);
        }
        return -1;
    }

    Config config_;
    uint32_t samples_per_packet_;
    size_t frame_size_;
    std::unique_ptr<pal::UdpSocket> socket_;
    std::unique_ptr<TransportSocket> transport_socket_;
    std::vector<uint8_t> packet_buf_;

    uint64_t sequence_ = 0;
    uint32_t rtp_timestamp_ = 0;
    uint32_t media_timestamp_ = 0;

    PtpTimestampCallback ptp_timestamp_cb_;
};

} // namespace soluna::transport
