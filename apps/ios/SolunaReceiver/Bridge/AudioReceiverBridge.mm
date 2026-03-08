//
//  AudioReceiverBridge.mm
//  SolunaReceiver
//
//  Objective-C++ bridge for C++ RTP receiver and CoreAudio output
//

#import "AudioReceiverBridge.h"
#import <AVFoundation/AVFoundation.h>

#include "web_embedded.h"
#include <soluna/soluna.h>
#include <soluna/pal/audio.h>
#include <soluna/pal/net.h>
#include <soluna/transport/rtp.h>
#include <soluna/transport/ostp.h>
#include <soluna/pipeline/ring_buffer.h>
#include <soluna/pipeline/pipeline.h>
#include <soluna/transport/packet_scheduler.h>
#include <soluna/control/websocket_server.h>

#include <atomic>
#include <thread>
#include <memory>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <random>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#endif

using namespace soluna;

// ============================================================================
// Internal C++ Implementation
// ============================================================================

namespace {

/// Receive mode for RTP reception
enum class ReceiveMode {
    OSTP,
    AES67,
    Auto
};

/// RTP payload types (AES67 standard)
constexpr uint8_t kPayloadTypeL24_AES67 = 10;  // AES67 24-bit
constexpr uint8_t kPayloadTypeL16_AES67 = 11;  // AES67 16-bit
constexpr uint8_t kPayloadTypeL24 = 98;  // OSTP default
constexpr uint8_t kPayloadTypeL16 = 11;

/// Check if RTP packet is AES67 standard (no OSTP extension)
inline bool aes67_is_standard_packet(const transport::RtpHeader& hdr) {
    // AES67 uses PT 10 (L24) or PT 11 (L16) and no extension header
    return (hdr.pt == kPayloadTypeL24_AES67 || hdr.pt == kPayloadTypeL16_AES67) && hdr.extension == 0;
}

/// Simple RTP receiver (embedded to avoid header dependency issues)
class SimpleRtpReceiver {
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
        uint64_t packets_concealed = 0;
        uint64_t sequence_errors = 0;
        uint64_t aes67_packets = 0;
        uint64_t ostp_packets = 0;
        int32_t last_sequence = -1;
    };

    explicit SimpleRtpReceiver(const Config& config)
        : config_(config)
        , frame_size_(sample_size(config.format) * config.channels)
        , recv_buf_(transport::kMaxPacketSize)
        , audio_buf_(transport::kMaxPayloadSize / sizeof(int32_t))
    {}

    Stats stats_snapshot() const { return stats_; }

    bool init() {
        socket_ = pal::UdpSocket::create();
        if (!socket_) return false;
        if (!socket_->bind(config_.listen_port)) return false;
        if (!socket_->join_multicast(config_.multicast_group)) return false;
        socket_->set_recv_timeout_ms(5);  // 5ms — batches WiFi bursts better than 1ms
        return true;
    }

    // Relay callback: invoked with raw bytes for every received packet
    std::function<void(const uint8_t*, size_t)> relay_callback;

    bool receive_packet(pipeline::RingBuffer& ring) {
        pal::SocketAddress src;
        int received = socket_->recv_from_nonblock(recv_buf_.data(), recv_buf_.size(), src);
        if (received <= 0) return false;

        // Forward raw bytes to nearby peers when in relay mode
        if (relay_callback) {
            relay_callback(recv_buf_.data(), static_cast<size_t>(received));
        }

        // Check if this is an AES67 packet or OSTP packet
        bool is_aes67 = false;
        if (config_.mode == ReceiveMode::Auto &&
            static_cast<size_t>(received) >= sizeof(transport::RtpHeader)) {
            const auto* rtp = reinterpret_cast<const transport::RtpHeader*>(recv_buf_.data());
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

    /// Inject a raw packet from a relay peer (bypasses UDP socket)
    bool inject_raw_packet(const uint8_t* data, size_t len, pipeline::RingBuffer& ring) {
        bool is_aes67 = false;
        if (config_.mode == ReceiveMode::Auto && len >= sizeof(transport::RtpHeader)) {
            const auto* rtp = reinterpret_cast<const transport::RtpHeader*>(data);
            is_aes67 = aes67_is_standard_packet(*rtp);
        } else if (config_.mode == ReceiveMode::AES67) {
            is_aes67 = true;
        }
        if (is_aes67) return receive_aes67_packet(data, len, ring);
        else          return receive_ostp_packet(data, len, ring);
    }

private:
    bool receive_ostp_packet(const uint8_t* data, size_t len, pipeline::RingBuffer& ring) {
        transport::RtpHeader rtp;
        transport::OstpHeader ostp;
        const uint8_t* payload = nullptr;
        size_t payload_size = 0;

        if (transport::ostp_parse_packet(data, len, rtp, ostp, payload, payload_size) != 0) {
            return false;
        }

        // Sequence check — returns gap count (positive = missing packets)
        uint32_t full_seq = (static_cast<uint32_t>(ostp.sequence_ext) << 16) | rtp.sequence;
        int32_t gap = check_sequence(full_seq);

        stats_.packets_received++;
        stats_.ostp_packets++;

        // Discard duplicate packets (gap <= 0 means same or older sequence)
        if (gap < 0) return true;  // duplicate — already received

        // ── NTP-based sync: use media_timestamp to align with other receivers ──
        // media_timestamp = lower 32 bits of sender's CLOCK_REALTIME in nanoseconds.
        // By comparing with our CLOCK_REALTIME, we compute the playout offset and
        // adjust target_fill_frames_ so all devices play in sync.
        if (ostp.media_timestamp != 0 && sync_target_frames_ != nullptr) {
            struct timespec now_ts;
            clock_gettime(CLOCK_REALTIME, &now_ts);
            uint32_t now_ns32 = static_cast<uint32_t>(
                (static_cast<uint64_t>(now_ts.tv_sec) * 1'000'000'000ULL + now_ts.tv_nsec)
                & 0xFFFFFFFF);
            // age_ms = how long ago this packet was created (sender → receiver)
            int32_t age_ns = static_cast<int32_t>(now_ns32 - ostp.media_timestamp);
            double age_ms = age_ns / 1'000'000.0;
            // Clamp to reasonable range (0..500ms)
            if (age_ms >= 0.0 && age_ms < 500.0) {
                // We want all receivers to play at exactly sync_playout_delay_ms_
                // after the packet was created. So the buffer should hold:
                //   target = (playout_delay - age) in frames
                // If age < playout_delay, we need more buffer (packet arrived early).
                // If age > playout_delay, we need less buffer (packet arrived late).
                double needed_ms = sync_playout_delay_ms_ - age_ms;
                if (needed_ms < 10.0) needed_ms = 10.0;   // minimum 10ms
                if (needed_ms > 200.0) needed_ms = 200.0;  // maximum 200ms
                uint32_t needed_frames = static_cast<uint32_t>(needed_ms * 48.0);
                // EMA smoothing to avoid jitter in the target
                uint32_t cur = sync_target_frames_->load(std::memory_order_relaxed);
                uint32_t smoothed = cur == 0 ? needed_frames
                    : static_cast<uint32_t>(cur * 0.95 + needed_frames * 0.05);
                sync_target_frames_->store(smoothed, std::memory_order_relaxed);
            }
        }

        // OSTP payload is int32_t (4 bytes/sample, native byte order) — not S24_LE 3-byte
        size_t frames = payload_size / (sizeof(int32_t) * config_.channels);

        // PLC: conceal gaps of ≤2 packets by repeating the last known frame
        if (gap > 0 && gap <= 2 && frames > 0 && !last_frame_.empty()) {
            for (int32_t i = 0; i < gap; i++) {
                ring.write(last_frame_.data(), frames);
            }
            stats_.packets_concealed += static_cast<uint64_t>(gap);
        }

        ring.write(payload, frames);

        // Save last frame for PLC
        if (frames > 0) {
            last_frame_.assign(
                reinterpret_cast<const int32_t*>(payload),
                reinterpret_cast<const int32_t*>(payload) + frames * config_.channels);
        }

        return true;
    }

    bool receive_aes67_packet(const uint8_t* data, size_t len, pipeline::RingBuffer& ring) {
        if (len < sizeof(transport::RtpHeader)) {
            return false;
        }

        transport::RtpHeader rtp;
        std::memcpy(&rtp, data, sizeof(transport::RtpHeader));

        uint16_t sequence = ntohs(rtp.sequence);

        // Sequence check (16-bit only for AES67)
        if (stats_.last_sequence >= 0) {
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
        const uint8_t* payload = data + sizeof(transport::RtpHeader);
        size_t payload_size = len - sizeof(transport::RtpHeader);

        // Convert payload based on payload type
        size_t samples = 0;
        if (rtp.pt == kPayloadTypeL24) {
            // L24: 24-bit packed big-endian -> S24_LE
            samples = payload_size / 3;
            for (size_t i = 0; i < samples && i < audio_buf_.size(); i++) {
                int32_t sample = (static_cast<int32_t>(payload[i * 3]) << 16)
                               | (static_cast<int32_t>(payload[i * 3 + 1]) << 8)
                               | static_cast<int32_t>(payload[i * 3 + 2]);
                if (sample & 0x800000) {
                    sample |= 0xFF000000;
                }
                audio_buf_[i] = sample;
            }
        } else if (rtp.pt == kPayloadTypeL16) {
            // L16: 16-bit big-endian -> S24_LE
            samples = payload_size / 2;
            const int16_t* src = reinterpret_cast<const int16_t*>(payload);
            for (size_t i = 0; i < samples && i < audio_buf_.size(); i++) {
                int16_t be_sample = src[i];
                int16_t sample_le = static_cast<int16_t>((be_sample >> 8) | (be_sample << 8));
                audio_buf_[i] = static_cast<int32_t>(sample_le) << 8;
            }
        } else {
            return false;
        }

        size_t frames = samples / config_.channels;
        ring.write(audio_buf_.data(), frames);

        return true;
    }

    // Returns the gap (missing packet count > 0) or 0 if in sequence
    int32_t check_sequence(uint32_t full_seq) {
        int32_t gap = 0;
        if (stats_.last_sequence >= 0) {
            int32_t expected = stats_.last_sequence + 1;
            if (static_cast<int32_t>(full_seq) != expected) {
                stats_.sequence_errors++;
                gap = static_cast<int32_t>(full_seq) - expected;
                if (gap > 0) {
                    stats_.packets_dropped += static_cast<uint64_t>(gap);
                }
            }
        }
        stats_.last_sequence = static_cast<int32_t>(full_seq);
        return gap;
    }

    Config config_;
    size_t frame_size_;
    std::unique_ptr<pal::UdpSocket> socket_;
    std::vector<uint8_t> recv_buf_;
    std::vector<int32_t> audio_buf_;
    std::vector<int32_t> last_frame_;  // PLC: last received frame for concealment
    Stats stats_;

public:
    // NTP-based sync: pointer to ReceiverImpl's target_fill_frames_ (set externally)
    std::atomic<uint32_t>* sync_target_frames_ = nullptr;
    double sync_playout_delay_ms_ = 80.0;  // default playout delay matching RPi
};

/// WAN relay client — connects to soluna-relay server via UDP
class WanRelayClient {
public:
    enum class State { Disconnected, Connecting, Connected, Error };
    using RxCallback = std::function<void(const uint8_t*, size_t)>;

    bool connect(const std::string& host, uint16_t port,
                 const std::string& group, const std::string& password,
                 const std::string& device_name) {
        if (state_.load() == State::Connected) disconnect();
        state_.store(State::Connecting);
        group_ = group;
        error_.clear();

        // DNS resolve
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%u", port);
        if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || !res) {
            error_ = "DNS resolution failed: " + host;
            state_.store(State::Error);
            return false;
        }
        std::memcpy(&relay_addr_, res->ai_addr, sizeof(relay_addr_));
        freeaddrinfo(res);

        udp_sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_sock_ < 0) {
            error_ = "socket() failed";
            state_.store(State::Error);
            return false;
        }

        // Send JOIN
        std::string join_msg = "JOIN:" + group + ":" + password + ":" + device_name + "\n";
        ::sendto(udp_sock_, join_msg.c_str(), join_msg.size(), 0,
                 (struct sockaddr*)&relay_addr_, sizeof(relay_addr_));

        // Wait for OK:joined (3s timeout), also collect PEER messages
        struct timeval tv{3, 0};
        setsockopt(udp_sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        bool joined = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            char buf[256];
            ssize_t n = ::recvfrom(udp_sock_, buf, sizeof(buf)-1, 0, nullptr, nullptr);
            if (n <= 0) break;
            std::string msg(buf, n);
            if (msg.find("OK:joined") != std::string::npos) { joined = true; break; }
            // Collect PEER messages that arrive before OK
            if (n >= 6 && memcmp(buf, "PEER:", 5) == 0) {
                while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
                    msg.pop_back();
                auto colon = msg.rfind(':');
                if (colon != std::string::npos && colon > 5) {
                    std::string ip = msg.substr(5, colon - 5);
                    uint16_t pport = (uint16_t)atoi(msg.substr(colon + 1).c_str());
                    sockaddr_in peer{};
                    peer.sin_family = AF_INET;
                    peer.sin_port = htons(pport);
                    inet_pton(AF_INET, ip.c_str(), &peer.sin_addr);
                    add_peer(peer);
                    fprintf(stderr, "[wan-p2p] Peer discovered (during join): %s:%u\n", ip.c_str(), pport);
                }
            }
        }
        if (!joined) {
            error_ = "Failed to join relay group";
            ::close(udp_sock_); udp_sock_ = -1;
            state_.store(State::Error);
            return false;
        }

        running_.store(true);
        state_.store(State::Connected);
        recv_thread_ = std::thread([this]() { recv_loop(); });
        fprintf(stderr, "[wan-relay] Connected to relay, group='%s'\n", group_.c_str());
        return true;
    }

    void disconnect() {
        running_.store(false);
        if (recv_thread_.joinable()) recv_thread_.join();
        if (udp_sock_ >= 0) { ::close(udp_sock_); udp_sock_ = -1; }
        {
            std::lock_guard<std::mutex> lk(peers_mutex_);
            peers_.clear();
        }
        state_.store(State::Disconnected);
        group_.clear();
        error_.clear();
    }

    void send_audio(const uint8_t* data, size_t len) {
        if (udp_sock_ < 0 || state_.load() != State::Connected) return;
        // Send to all direct peers (P2P)
        {
            std::lock_guard<std::mutex> lk(peers_mutex_);
            for (const auto& peer : peers_) {
                ::sendto(udp_sock_, data, len, 0,
                         (const struct sockaddr*)&peer, sizeof(peer));
            }
        }
        // Also send via relay as fallback
        ::sendto(udp_sock_, data, len, 0,
                 (struct sockaddr*)&relay_addr_, sizeof(relay_addr_));
    }

    void set_rx_callback(RxCallback cb) {
        std::lock_guard<std::mutex> lk(cb_mutex_);
        rx_callback_ = std::move(cb);
    }

    State state() const { return state_.load(); }
    const std::string& group() const { return group_; }
    const std::string& error() const { return error_; }

private:
    void recv_loop() {
        struct timeval tv{1, 0};
        setsockopt(udp_sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        uint8_t buf[2048];
        auto last_hello = std::chrono::steady_clock::now();

        while (running_.load()) {
            sockaddr_in sender{};
            socklen_t sender_len = sizeof(sender);
            ssize_t n = ::recvfrom(udp_sock_, buf, sizeof(buf), 0,
                                   (struct sockaddr*)&sender, &sender_len);
            if (n > 0) {
                // PEER message: "PEER:ip:port\n"
                if (n >= 6 && memcmp(buf, "PEER:", 5) == 0) {
                    std::string msg((const char*)buf, n);
                    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
                        msg.pop_back();
                    auto colon = msg.rfind(':');
                    if (colon != std::string::npos && colon > 5) {
                        std::string ip = msg.substr(5, colon - 5);
                        uint16_t pport = (uint16_t)atoi(msg.substr(colon + 1).c_str());
                        sockaddr_in peer{};
                        peer.sin_family = AF_INET;
                        peer.sin_port = htons(pport);
                        inet_pton(AF_INET, ip.c_str(), &peer.sin_addr);
                        add_peer(peer);
                        fprintf(stderr, "[wan-p2p] Peer discovered: %s:%u\n", ip.c_str(), pport);
                        // Send a punch packet to open NAT
                        const char* punch = "PUNCH\n";
                        ::sendto(udp_sock_, punch, 6, 0,
                                 (const struct sockaddr*)&peer, sizeof(peer));
                    }
                }
                // RTP packet: from relay or peer
                else if (n >= 12 && (buf[0] & 0xC0) == 0x80) {
                    std::lock_guard<std::mutex> lk(cb_mutex_);
                    if (rx_callback_) rx_callback_(buf, static_cast<size_t>(n));
                }
                // OK:joined or other control — ignore
            }
            // HELLO heartbeat every 5s
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_hello).count() >= 5) {
                const char* hello = "HELLO\n";
                ::sendto(udp_sock_, hello, 6, 0,
                         (struct sockaddr*)&relay_addr_, sizeof(relay_addr_));
                last_hello = now;
            }
        }
    }

    void add_peer(const sockaddr_in& peer) {
        std::lock_guard<std::mutex> lk(peers_mutex_);
        for (const auto& p : peers_) {
            if (p.sin_addr.s_addr == peer.sin_addr.s_addr && p.sin_port == peer.sin_port)
                return; // already known
        }
        peers_.push_back(peer);
    }

    std::atomic<State> state_{State::Disconnected};
    std::atomic<bool> running_{false};
    int udp_sock_ = -1;
    sockaddr_in relay_addr_{};
    std::string group_, error_;
    std::thread recv_thread_;
    std::mutex cb_mutex_;
    RxCallback rx_callback_;
    std::mutex peers_mutex_;
    std::vector<sockaddr_in> peers_;
};

/// Internal microphone transmitter implementation
class TransmitterImpl {
public:
    TransmitterImpl(const std::string& dest_ip, uint16_t dest_port, uint32_t channels)
        : dest_ip_(dest_ip)
        , dest_port_(dest_port)
        , channels_(channels)
        , running_(false)
        , packets_sent_(0)
        // Ring buffer: 1 second capacity, stereo int32_t
        , ring_buffer_(kDefaultSampleRate, channels * sizeof(int32_t))
        , ssrc_(arc4random())
    {}

    ~TransmitterImpl() { stop(); }

    bool start() {
        if (running_.load()) return false;

        // Open mic input
        audio_device_ = pal::AudioDevice::create();
        if (!audio_device_) return false;

        pal::AudioStreamConfig audio_cfg;
        audio_cfg.sample_rate = kDefaultSampleRate;
        audio_cfg.channels = 1;  // Mono mic
        audio_cfg.frames_per_buffer = 240;
        audio_cfg.format = SampleFormat::S24_LE;

        if (!audio_device_->open_input("", audio_cfg)) {
            fprintf(stderr, "[SolunaTx] Failed to open mic input\n");
            audio_device_.reset();
            return false;
        }

        // Create UDP socket for multicast TX
        socket_ = pal::UdpSocket::create();
        if (!socket_) {
            audio_device_.reset();
            return false;
        }

        running_.store(true);
        ring_buffer_.reset();
        packets_sent_.store(0);

        // Conversion buffer: mono float → stereo int32_t (S24)
        // Oversized to handle iOS delivering more frames than requested
        // (IOBufferDuration is just a preference; actual may be 480+ frames)
        conv_buf_.resize(4096 * channels_);

        // Start audio capture callback
        audio_device_->start([this](float* buffer, uint32_t frame_count) {
            mic_callback(buffer, frame_count);
        });

        // Start TX packet thread
        tx_thread_ = std::thread([this]() { tx_loop(); });

        return true;
    }

    void stop() {
        if (!running_.load()) return;
        running_.store(false);

        if (audio_device_) {
            audio_device_->stop();
            audio_device_->close();
            audio_device_.reset();
        }

        if (tx_thread_.joinable()) {
            tx_thread_.join();
        }

        socket_.reset();
        ring_buffer_.reset();
    }

    bool is_running() const { return running_.load(); }
    uint64_t packets_sent() const { return packets_sent_.load(); }
    float peak_level() const { return peak_level_.load(std::memory_order_relaxed); }

    /// Callback to forward TX packets to WAN relay
    std::function<void(const uint8_t*, size_t)> tx_relay_callback;

private:
    void mic_callback(float* buffer, uint32_t frame_count) {
        // Track peak level for UI meter
        float peak = 0.0f;
        for (uint32_t i = 0; i < frame_count; i++) {
            float abs_val = std::fabs(buffer[i]);
            if (abs_val > peak) peak = abs_val;
        }
        // Exponential decay: fast attack, slow release
        float prev = peak_level_.load(std::memory_order_relaxed);
        if (peak > prev) {
            peak_level_.store(peak, std::memory_order_relaxed);
        } else {
            peak_level_.store(prev * 0.85f, std::memory_order_relaxed);
        }

        // Convert mono float → stereo S24 int32_t
        size_t out_idx = 0;
        for (uint32_t i = 0; i < frame_count; i++) {
            int32_t sample = static_cast<int32_t>(buffer[i] * 8388607.0f);
            // Duplicate mono to stereo
            for (uint32_t ch = 0; ch < channels_; ch++) {
                conv_buf_[out_idx++] = sample;
            }
        }
        ring_buffer_.write(conv_buf_.data(), frame_count);
    }

    void tx_loop() {
        constexpr uint32_t kFramesPerPacket = 240;  // LAN tier: 5ms @ 48kHz
        const size_t frame_size = channels_ * sizeof(int32_t);

        transport::PacketScheduler scheduler(PacketTier::LAN, kDefaultSampleRate);
        scheduler.reset();

        std::vector<int32_t> audio_buf(kFramesPerPacket * channels_);
        std::vector<uint8_t> packet_buf(transport::kMaxPacketSize);

        pal::SocketAddress dest;
        dest.ip = dest_ip_;
        dest.port = dest_port_;

        uint32_t sequence = 0;
        uint32_t rtp_timestamp = 0;

        while (running_.load()) {
            scheduler.wait_next();

            if (ring_buffer_.available_read() < kFramesPerPacket) {
                // Underrun: send silence
                std::memset(audio_buf.data(), 0, kFramesPerPacket * channels_ * sizeof(int32_t));
            } else {
                ring_buffer_.read(audio_buf.data(), kFramesPerPacket);
            }

            uint16_t seq_lo = static_cast<uint16_t>(sequence & 0xFFFF);
            uint16_t seq_hi = static_cast<uint16_t>((sequence >> 16) & 0xFFFF);

            size_t pkt_size = transport::ostp_build_packet(
                packet_buf.data(), packet_buf.size(),
                ssrc_, seq_lo, rtp_timestamp,
                96,  // kPayloadTypePCM24
                1,   // stream_id
                seq_hi,
                0,   // media_timestamp
                audio_buf.data(),
                kFramesPerPacket * frame_size
            );

            if (pkt_size > 0) {
                socket_->send_to(packet_buf.data(), pkt_size, dest);
                if (tx_relay_callback) {
                    tx_relay_callback(packet_buf.data(), pkt_size);
                }
                packets_sent_.fetch_add(1);
            }

            sequence++;
            rtp_timestamp += kFramesPerPacket;
        }
    }

    std::string dest_ip_;
    uint16_t dest_port_;
    uint32_t channels_;
    std::atomic<bool> running_;
    std::atomic<uint64_t> packets_sent_;
    std::atomic<float> peak_level_{0.0f};

    pipeline::RingBuffer ring_buffer_;
    uint32_t ssrc_;

    std::unique_ptr<pal::AudioDevice> audio_device_;
    std::unique_ptr<pal::UdpSocket> socket_;
    std::vector<int32_t> conv_buf_;
    std::thread tx_thread_;
};

/// Internal receiver implementation
class ReceiverImpl {
public:
    ReceiverImpl(const std::string& multicast_group, uint16_t port, uint32_t channels)
        : multicast_group_(multicast_group)
        , port_(port)
        , channels_(channels)
        , volume_(1.0f)
        , muted_(false)
        , running_(false)
        , target_fill_frames_(14400) // 300ms — iPhone WiFi needs larger buffer for burst loss
        , ring_buffer_(192000, channels * sizeof(int32_t)) // 4s capacity for WiFi jitter
        , read_buffer_(4096 * channels)
        , drain_buf_(4096 * channels)
        , held_sample_(channels, 0)
        , ramp_(0.0f)
        , dither_rng_(std::random_device{}())
    {}

    ~ReceiverImpl() {
        stop();
    }

    bool start() {
        if (running_.load()) return false;

        // Create RTP receiver
        SimpleRtpReceiver::Config rx_config;
        rx_config.listen_port = port_;
        rx_config.multicast_group = multicast_group_;
        rx_config.channels = channels_;
        rx_config.format = SampleFormat::S24_LE;
        rx_config.mode = ReceiveMode::Auto;

        rtp_receiver_ = std::make_unique<SimpleRtpReceiver>(rx_config);
        if (!rtp_receiver_->init()) {
            return false;
        }

        // Wire up NTP sync: receiver adjusts target_fill_frames_ based on media_timestamp
        rtp_receiver_->sync_target_frames_ = &target_fill_frames_;
        rtp_receiver_->sync_playout_delay_ms_ = 80.0;

        // Create audio output device
        audio_device_ = pal::AudioDevice::create();
        if (!audio_device_) {
            return false;
        }

        pal::AudioStreamConfig audio_config;
        audio_config.sample_rate = kDefaultSampleRate;
        audio_config.channels = channels_;
        audio_config.frames_per_buffer = 512;  // ~10ms — more headroom for WiFi jitter
        audio_config.format = SampleFormat::S24_LE;

        if (!audio_device_->open_output("default", audio_config)) {
            return false;
        }

        running_.store(true);

        // Start audio playback FIRST so the render callback is active before data arrives
        auto callback = [this](float* buffer, uint32_t frame_count) {
            audio_callback(buffer, frame_count);
        };

        if (!audio_device_->start(callback)) {
            fprintf(stderr, "[SolunaRx] Failed to start audio device\n");
            running_.store(false);
            return false;
        }

        // Flush ALL stale packets from the UDP socket buffer.
        if (rtp_receiver_) {
            pipeline::RingBuffer discard_buf(65536, channels_ * sizeof(int32_t));
            int flushed = 0;
            for (int i = 0; i < 100000; i++) {
                if (!rtp_receiver_->receive_packet(discard_buf)) break;
                flushed++;
                if (discard_buf.available_write() < 1024) discard_buf.reset();
            }
            if (flushed > 0) {
                fprintf(stderr, "[SolunaRx] Flushed %d stale packets from socket\n", flushed);
            }
        }

        // Reset ring buffer and state for a clean start
        ring_buffer_.reset();
        prefilled_ = false;
        ramp_ = 0.0f;

        // NOW start the receive thread with a clean slate
        receive_thread_ = std::thread([this]() {
            receive_loop();
        });

        // Start WebSocket control server on port 8400
        ws_server_.set_web_files(
            reinterpret_cast<const soluna::control::WebFile*>(embedded_web_files),
            embedded_web_file_count);
        ws_server_.set_message_callback([this](const std::string& msg) -> std::string {
            return handle_ws_command(msg);
        });
        ws_server_.start(8400);

        return true;
    }

    void stop() {
        if (!running_.load()) return;

        wan_relay_disconnect();
        running_.store(false);

        if (audio_device_) {
            audio_device_->stop();
            audio_device_->close();
        }

        if (receive_thread_.joinable()) {
            receive_thread_.join();
        }

        rtp_receiver_.reset();
        audio_device_.reset();
        ring_buffer_.reset();
    }

    bool is_running() const {
        return running_.load();
    }

    void set_volume(float volume) {
        volume_.store(std::max(0.0f, std::min(1.0f, volume)));
    }

    float volume() const { return volume_.load(); }

    void set_muted(bool muted) { muted_.store(muted); }
    bool is_muted() const { return muted_.load(); }

    void set_buffer_ms(uint32_t ms) {
        ms = std::max(5u, std::min(2000u, ms));
        target_fill_frames_.store(ms * 48u);
    }
    uint32_t buffer_ms() const { return target_fill_frames_.load() / 48u; }

    SimpleRtpReceiver::Stats stats() const {
        if (rtp_receiver_) return rtp_receiver_->stats_snapshot();
        return {};
    }

    int device_health() const {
        return health_.load(std::memory_order_relaxed);
    }

    void set_network_disabled(bool d) { network_disabled_.store(d); }
    bool is_network_disabled() const { return network_disabled_.load(); }

    // ── Relay support ──────────────────────────────────────────────────────

    void set_relay_callback(std::function<void(const uint8_t*, size_t)> cb) {
        relay_callback_ = std::move(cb);
        if (rtp_receiver_) rtp_receiver_->relay_callback = relay_callback_;
    }

    void inject_raw_packet(const uint8_t* data, size_t len) {
        if (rtp_receiver_) rtp_receiver_->inject_raw_packet(data, len, ring_buffer_);
    }

    // ── WAN Relay ─────────────────────────────────────────────────────────

    bool wan_relay_connect(const std::string& host, uint16_t port,
                           const std::string& group, const std::string& password) {
        if (!wan_relay_) wan_relay_ = std::make_unique<WanRelayClient>();
        wan_relay_->set_rx_callback([this](const uint8_t* data, size_t len) {
            inject_raw_packet(data, len);
        });
        return wan_relay_->connect(host, port, group, password, "iPhone");
    }

    void wan_relay_disconnect() {
        if (wan_relay_) wan_relay_->disconnect();
    }

    WanRelayClient::State wan_relay_state() const {
        return wan_relay_ ? wan_relay_->state() : WanRelayClient::State::Disconnected;
    }

    std::string wan_relay_group() const {
        return wan_relay_ ? wan_relay_->group() : "";
    }

    std::string wan_relay_error() const {
        return wan_relay_ ? wan_relay_->error() : "";
    }

    void wan_relay_send_audio(const uint8_t* data, size_t len) {
        if (wan_relay_) wan_relay_->send_audio(data, len);
    }

    std::unique_ptr<WanRelayClient> wan_relay_;

private:
    // ── Health tracking helpers (audio-callback thread only) ──────────────

    static uint64_t now_ms_() {
        using namespace std::chrono;
        return (uint64_t)duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()).count();
    }

    void record_underrun_now() {
        uint64_t now = now_ms_();
        if (health_window_start_ms_ == 0 || now - health_window_start_ms_ >= 30000) {
            health_window_start_ms_ = now;
            health_underruns_in_window_ = 0;
        }
        health_underruns_in_window_++;
        last_underrun_ms_ = now;

        int cur = health_.load(std::memory_order_relaxed);
        if (health_underruns_in_window_ >= 50 && cur < 2) {
            // Extreme underruns: silence device to prevent noise
            health_.store(2, std::memory_order_relaxed);
            health_silenced_.store(true, std::memory_order_relaxed);
        } else if (health_underruns_in_window_ >= 10 && cur < 1) {
            // Moderate underruns: stressed — auto-increase buffer 50%, cap at 2000ms
            health_.store(1, std::memory_order_relaxed);
            uint32_t cur_frames = target_fill_frames_.load(std::memory_order_relaxed);
            uint32_t new_frames = std::min(cur_frames + cur_frames / 2, 96000u);
            target_fill_frames_.store(new_frames, std::memory_order_relaxed);
        }
    }

    void maybe_check_recovery() {
        if (health_.load(std::memory_order_relaxed) == 0) return;
        if (++recovery_check_counter_ < 200) return;  // ~1 s at 5ms/callback
        recovery_check_counter_ = 0;
        if (last_underrun_ms_ == 0) return;
        if (now_ms_() - last_underrun_ms_ >= 10000) {
            // 10 seconds clean: restore normal operation
            health_.store(0, std::memory_order_relaxed);
            health_silenced_.store(false, std::memory_order_relaxed);
            health_window_start_ms_ = 0;
            health_underruns_in_window_ = 0;
            last_underrun_ms_ = 0;
        }
    }

    void receive_loop() {
        // ONLY writes to ring_buffer_ — never reads (RingBuffer is SPSC).
        // Drain happens exclusively in audio_callback to avoid data race.
        // Propagate relay callback now that rtp_receiver_ is initialized.
        if (relay_callback_ && rtp_receiver_) {
            rtp_receiver_->relay_callback = relay_callback_;
        }
        uint64_t log_counter = 0;
        while (running_.load()) {
            // When network_disabled_, audio arrives via inject_raw_packet instead
            if (!network_disabled_.load() && rtp_receiver_) {
                for (int i = 0; i < 10 && running_.load(); i++) {
                    if (!rtp_receiver_->receive_packet(ring_buffer_)) break;
                }
            }
            // Periodic debug log every ~2 seconds
            if (++log_counter % 20000 == 0) {
                auto st = rtp_receiver_ ? rtp_receiver_->stats_snapshot() : SimpleRtpReceiver::Stats{};
                fprintf(stderr, "[SolunaRx] pkts=%llu seq_err=%llu dropped=%llu fill=%zu prefilled=%d target=%u underruns=%u\n",
                        (unsigned long long)st.packets_received,
                        (unsigned long long)st.sequence_errors,
                        (unsigned long long)st.packets_dropped,
                        ring_buffer_.available_read(),
                        (int)prefilled_,
                        target_fill_frames_.load() / 48u,
                        health_underruns_in_window_);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    std::string handle_ws_command(const std::string& msg) {
        int id = 0;
        auto p = msg.find("\"id\":");
        if (p != std::string::npos) try { id = std::stoi(msg.substr(p + 5)); } catch (...) {}

        std::string cmd;
        p = msg.find("\"command\":\"");
        if (p != std::string::npos) {
            auto s = p + 11, e = msg.find('"', s);
            if (e != std::string::npos) cmd = msg.substr(s, e - s);
        }

        char buf[512];
        if (cmd == "rx.stats" || cmd == "system.stats") {
            auto st = stats();
            size_t fill = ring_buffer_.available_read();
            uint32_t target_ms = target_fill_frames_.load() / 48u;
            snprintf(buf, sizeof(buf),
                "{\"id\":%d,\"success\":true,\"data\":"
                "\"{\\\"packets\\\":%llu,\\\"errors\\\":%llu,"
                "\\\"buf_fill\\\":%zu,\\\"buf_cap\\\":4096,"
                "\\\"volume\\\":%.3f,\\\"muted\\\":%s,"
                "\\\"buf_target_ms\\\":%u}\"}",
                id,
                (unsigned long long)st.packets_received,
                (unsigned long long)st.sequence_errors,
                fill,
                (double)volume_.load(),
                muted_.load() ? "true" : "false",
                target_ms);
        } else if (cmd == "rx.set_buffer") {
            p = msg.find("\"ms\":");
            if (p != std::string::npos) {
                try {
                    uint32_t ms = static_cast<uint32_t>(std::stoul(msg.substr(p + 5)));
                    ms = std::max(5u, std::min(200u, ms));
                    target_fill_frames_.store(ms * 48u);
                } catch (...) {}
            }
            snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
        } else if (cmd == "rx.set_volume") {
            p = msg.find("\"volume\":");
            if (p != std::string::npos) {
                try { set_volume(std::stof(msg.substr(p + 9))); } catch (...) {}
            }
            snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
        } else if (cmd == "rx.set_mute") {
            p = msg.find("\"muted\":");
            if (p != std::string::npos)
                set_muted(msg.substr(p + 8, 4) == "true");
            snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);

        // ── monitor.* aliases (Mac DaemonClient compatibility) ──────────
        } else if (cmd == "monitor.stats") {
            auto st = stats();
            uint32_t bms = target_fill_frames_.load() / 48u;
            snprintf(buf, sizeof(buf),
                "{\"id\":%d,\"success\":true,\"data\":"
                "\"{\\\"supported\\\":true,\\\"running\\\":true,"
                "\\\"volume\\\":%.3f,\\\"muted\\\":%s,"
                "\\\"packets\\\":%llu,\\\"buf_ms\\\":%u}\"}",
                id,
                (double)volume_.load(),
                muted_.load() ? "true" : "false",
                (unsigned long long)st.packets_received,
                bms);
        } else if (cmd == "monitor.set_volume") {
            p = msg.find("\"volume\":");
            if (p != std::string::npos) {
                try { set_volume(std::stof(msg.substr(p + 9))); } catch (...) {}
            }
            snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
        } else if (cmd == "monitor.set_mute") {
            p = msg.find("\"muted\":");
            if (p != std::string::npos)
                set_muted(msg.substr(p + 8, 4) == "true");
            snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
        } else if (cmd == "monitor.set_buffer") {
            p = msg.find("\"ms\":");
            if (p != std::string::npos) {
                try {
                    uint32_t ms = static_cast<uint32_t>(std::stoul(msg.substr(p + 5)));
                    set_buffer_ms(ms);
                } catch (...) {}
            }
            snprintf(buf, sizeof(buf), "{\"id\":%d,\"success\":true,\"data\":\"\"}", id);
        } else if (cmd == "monitor.list_devices") {
            snprintf(buf, sizeof(buf),
                "{\"id\":%d,\"success\":true,\"data\":\"[]\"}",
                id);
        } else if (cmd == "system.info") {
            snprintf(buf, sizeof(buf),
                "{\"id\":%d,\"success\":true,\"data\":"
                "\"{\\\"tunnel_url\\\":\\\"\\\"}\"}",
                id);
        } else {
            snprintf(buf, sizeof(buf),
                "{\"id\":%d,\"success\":false,\"data\":\"unknown command\"}", id);
        }
        return buf;
    }

    void audio_callback(float* buffer, uint32_t frame_count) {
        const float vol = (muted_.load() || health_silenced_.load(std::memory_order_relaxed))
                          ? 0.0f : volume_.load();
        const uint32_t total_samples = frame_count * channels_;

        // Adaptive target: always >= frame_count*4 to prevent immediate underrun
        uint32_t target = target_fill_frames_.load();
        const uint32_t min_target = frame_count * 4;
        if (target < min_target) {
            target = min_target;
            target_fill_frames_.store(target);
        }

        // ── Gradual drift correction ─────────────────────────────────────────
        // When buffer exceeds target, discard up to 10% of frame_count per callback.
        // This creates an imperceptible speed-up instead of audible gaps.
        {
            size_t avail_now = ring_buffer_.available_read();
            if (prefilled_ && avail_now > static_cast<size_t>(target) + frame_count * 2) {
                size_t drift = std::min(avail_now - static_cast<size_t>(target),
                                        static_cast<size_t>(frame_count / 10 + 1));
                ring_buffer_.discard(drift);
            }
        }

        const size_t avail = ring_buffer_.available_read();
        const bool has_data = (avail >= frame_count);

        constexpr float kFadeIn  = 0.001f;
        constexpr float kFadeOut = 0.005f;

        if (!prefilled_) {
            if (avail < target) {
                ramp_ *= (1.0f - kFadeOut);
                std::memset(buffer, 0, total_samples * sizeof(float));
                return;
            }
            prefilled_ = true;
        }

        if (!has_data) {
            prefilled_ = false;
            record_underrun_now();
            maybe_check_recovery(); // Also check recovery during underruns
            for (uint32_t i = 0; i < frame_count; i++) {
                ramp_ *= (1.0f - kFadeOut);
                for (uint32_t ch = 0; ch < channels_; ch++) {
                    buffer[i * channels_ + ch] = held_sample_[ch] * ramp_;
                }
            }
            return;
        }

        // Periodically check if health can be recovered
        maybe_check_recovery();

        // Normal playback with soft limiter to prevent clipping
        ring_buffer_.read(read_buffer_.data(), frame_count);
        const int32_t* src = read_buffer_.data();
        for (uint32_t i = 0; i < frame_count; i++) {
            ramp_ += kFadeIn * (vol - ramp_);
            for (uint32_t ch = 0; ch < channels_; ch++) {
                const uint32_t idx = i * channels_ + ch;
                // TPDF dithering: two uniform random values → triangular distribution
                // Reduces quantization noise at low signal levels
                const float d = (dither_dist_(dither_rng_) + dither_dist_(dither_rng_)) * 0.5f;
                float s = (static_cast<float>(src[idx]) + d) / 8388608.0f;
                // Soft limiter: tanh-style knee at ±0.9 to prevent hard clipping
                if (s > 0.9f)       s = 0.9f + 0.1f * std::tanh((s - 0.9f) * 5.0f);
                else if (s < -0.9f) s = -0.9f + 0.1f * std::tanh((s + 0.9f) * 5.0f);
                const float out = s * ramp_;
                buffer[idx] = out;
                held_sample_[ch] = out;
            }
        }
    }

    std::function<void(const uint8_t*, size_t)> relay_callback_;

    std::string multicast_group_;
    uint16_t port_;
    uint32_t channels_;
    std::atomic<float>    volume_;
    std::atomic<bool>     muted_;
    std::atomic<bool>     running_;
    std::atomic<uint32_t> target_fill_frames_;
    std::atomic<bool>     network_disabled_{false};
    // Health tracking atomics (written audio-cb, read ObjC):
    std::atomic<int>      health_{0};           ///< 0=good 1=stressed 2=silenced
    std::atomic<bool>     health_silenced_{false};
    // audio_callback-only state (no atomics needed):
    bool                  prefilled_ = false;
    float                 ramp_      = 0.0f;
    std::vector<float>    held_sample_;
    // TPDF dithering for int32→float conversion (reduces quantization noise)
    std::minstd_rand      dither_rng_;
    std::uniform_real_distribution<float> dither_dist_{-1.0f, 1.0f};
    // Health tracking — audio-callback-only (no atomic needed):
    uint64_t health_window_start_ms_    = 0;
    uint32_t health_underruns_in_window_ = 0;
    uint64_t last_underrun_ms_          = 0;
    uint32_t recovery_check_counter_    = 0;

    std::unique_ptr<SimpleRtpReceiver> rtp_receiver_;
    std::unique_ptr<pal::AudioDevice>  audio_device_;
    pipeline::RingBuffer  ring_buffer_;
    std::vector<int32_t>  read_buffer_;
    std::vector<int32_t>  drain_buf_;

    std::thread receive_thread_;
    soluna::control::WebSocketServer ws_server_;
};

} // anonymous namespace


// ============================================================================
// Objective-C Implementation
// ============================================================================

@implementation SolunaReceiverStats {
    uint64_t _packetsReceived;
    uint64_t _packetsDropped;
    uint64_t _packetsConcealed;
    uint64_t _sequenceErrors;
    uint64_t _aes67Packets;
    uint64_t _ostpPackets;
}

- (instancetype)initWithStats:(const SimpleRtpReceiver::Stats&)stats {
    self = [super init];
    if (self) {
        _packetsReceived  = stats.packets_received;
        _packetsDropped   = stats.packets_dropped;
        _packetsConcealed = stats.packets_concealed;
        _sequenceErrors   = stats.sequence_errors;
        _aes67Packets     = stats.aes67_packets;
        _ostpPackets      = stats.ostp_packets;
    }
    return self;
}

- (uint64_t)packetsReceived  { return _packetsReceived; }
- (uint64_t)packetsDropped   { return _packetsDropped; }
- (uint64_t)packetsConcealed { return _packetsConcealed; }
- (uint64_t)sequenceErrors   { return _sequenceErrors; }
- (uint64_t)aes67Packets     { return _aes67Packets; }
- (uint64_t)ostpPackets      { return _ostpPackets; }

@end


@interface SolunaAudioReceiver () {
    std::unique_ptr<ReceiverImpl> _impl;
    std::unique_ptr<TransmitterImpl> _txImpl;
    NSTimer *_statsTimer;
    uint32_t _bufferTargetMs;
}
@end

@implementation SolunaAudioReceiver

+ (instancetype)sharedInstance {
    static SolunaAudioReceiver *shared = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        shared = [[SolunaAudioReceiver alloc] init];
    });
    return shared;
}

- (instancetype)init {
    return [self initWithMulticastGroup:@"239.69.0.1" port:5004 channels:2];
}

- (instancetype)initWithMulticastGroup:(NSString *)group
                                  port:(uint16_t)port
                              channels:(uint32_t)channels {
    self = [super init];
    if (self) {
        _multicastGroup = [group copy];
        _port = port;
        _channels = channels;
        _volume = 1.0f;
        _bufferTargetMs = 40;
        _state = SolunaReceiverStateStopped;
    }
    return self;
}

- (void)dealloc {
    [self stop];
}

- (BOOL)start {
    if (_state != SolunaReceiverStateStopped) {
        return NO;
    }

    [self willChangeValueForKey:@"state"];
    _state = SolunaReceiverStateConnecting;
    [self didChangeValueForKey:@"state"];

    if ([_delegate respondsToSelector:@selector(receiverDidChange:)]) {
        [_delegate receiverDidChange:_state];
    }

    // Create implementation
    _impl = std::make_unique<ReceiverImpl>(
        std::string([_multicastGroup UTF8String]),
        _port,
        _channels
    );
    _impl->set_volume(_volume);
    _impl->set_buffer_ms(_bufferTargetMs);

    if (!_impl->start()) {
        _impl.reset();

        [self willChangeValueForKey:@"state"];
        _state = SolunaReceiverStateError;
        [self didChangeValueForKey:@"state"];

        if ([_delegate respondsToSelector:@selector(receiverDidChange:)]) {
            [_delegate receiverDidChange:_state];
        }

        NSError *error = [NSError errorWithDomain:@"SolunaReceiver"
                                             code:-1
                                         userInfo:@{NSLocalizedDescriptionKey: @"Failed to start receiver"}];
        if ([_delegate respondsToSelector:@selector(receiverDidEncounter:)]) {
            [_delegate receiverDidEncounter:error];
        }

        return NO;
    }

    [self willChangeValueForKey:@"state"];
    _state = SolunaReceiverStateReceiving;
    [self didChangeValueForKey:@"state"];

    if ([_delegate respondsToSelector:@selector(receiverDidChange:)]) {
        [_delegate receiverDidChange:_state];
    }

    // Start stats timer
    _statsTimer = [NSTimer scheduledTimerWithTimeInterval:0.5
                                                   target:self
                                                 selector:@selector(updateStats)
                                                 userInfo:nil
                                                  repeats:YES];

    return YES;
}

- (void)stop {
    // Stop mic transmit if active
    [self stopMicTransmit];

    [_statsTimer invalidate];
    _statsTimer = nil;

    if (_impl) {
        _impl->stop();
        _impl.reset();
    }

    [self willChangeValueForKey:@"state"];
    _state = SolunaReceiverStateStopped;
    [self didChangeValueForKey:@"state"];

    if ([_delegate respondsToSelector:@selector(receiverDidChange:)]) {
        [_delegate receiverDidChange:_state];
    }
}

- (void)setVolume:(float)volume {
    _volume = std::max(0.0f, std::min(1.0f, volume));
    if (_impl) _impl->set_volume(_volume);
}

- (BOOL)muted {
    return _impl ? (BOOL)_impl->is_muted() : NO;
}

- (void)setMuted:(BOOL)muted {
    if (_impl) _impl->set_muted((bool)muted);
}

- (uint32_t)bufferTargetMs {
    return _impl ? _impl->buffer_ms() : _bufferTargetMs;
}

- (void)setBufferTargetMs:(uint32_t)ms {
    _bufferTargetMs = std::max(5u, std::min(2000u, ms));
    if (_impl) _impl->set_buffer_ms(_bufferTargetMs);
}

- (SolunaDeviceHealth)deviceHealth {
    return _impl ? (SolunaDeviceHealth)_impl->device_health() : SolunaDeviceHealthGood;
}

- (BOOL)networkDisabled {
    return _impl ? (BOOL)_impl->is_network_disabled() : NO;
}

- (void)setNetworkDisabled:(BOOL)disabled {
    if (_impl) _impl->set_network_disabled((bool)disabled);
}

- (SolunaReceiverStats *)currentStats {
    if (_impl) {
        auto stats = _impl->stats();
        return [[SolunaReceiverStats alloc] initWithStats:stats];
    }
    return [[SolunaReceiverStats alloc] init];
}

- (void)updateStats {
    if ([_delegate respondsToSelector:@selector(receiverDidUpdate:)]) {
        [_delegate receiverDidUpdate:[self currentStats]];
    }
}

// ── P2P Relay ───────────────────────────────────────────────────────────────

- (void)setRelayCallback:(nullable void(^)(NSData *))callback {
    if (_impl) {
        if (callback) {
            _impl->set_relay_callback([callback](const uint8_t* data, size_t len) {
                NSData *packet = [NSData dataWithBytes:data length:len];
                callback(packet);
            });
        } else {
            _impl->set_relay_callback(nullptr);
        }
    }
}

- (void)injectRawPacket:(NSData *)data {
    if (_impl && data.length > 0) {
        _impl->inject_raw_packet(static_cast<const uint8_t*>(data.bytes), data.length);
    }
}

// ── WAN Relay ───────────────────────────────────────────────────────────────

- (BOOL)connectToRelay:(NSString *)host port:(uint16_t)port
                 group:(NSString *)group password:(NSString *)password {
    if (!_impl) return NO;
    auto* impl = _impl.get();
    bool ok = impl->wan_relay_connect(
        std::string([host UTF8String]), port,
        std::string([group UTF8String]),
        std::string([password UTF8String]));
    if (ok && _txImpl) {
        _txImpl->tx_relay_callback = [impl](const uint8_t* data, size_t len) {
            impl->wan_relay_send_audio(data, len);
        };
    }
    return ok ? YES : NO;
}

- (void)disconnectRelay {
    if (_impl) _impl->wan_relay_disconnect();
    if (_txImpl) _txImpl->tx_relay_callback = nullptr;
}

- (SolunaRelayState)relayState {
    if (!_impl) return SolunaRelayStateDisconnected;
    switch (_impl->wan_relay_state()) {
        case WanRelayClient::State::Connecting:   return SolunaRelayStateConnecting;
        case WanRelayClient::State::Connected:     return SolunaRelayStateConnected;
        case WanRelayClient::State::Error:         return SolunaRelayStateError;
        default:                                   return SolunaRelayStateDisconnected;
    }
}

- (NSString *)relayGroup {
    if (!_impl) return nil;
    auto g = _impl->wan_relay_group();
    return g.empty() ? nil : [NSString stringWithUTF8String:g.c_str()];
}

- (NSString *)relayError {
    if (!_impl) return nil;
    auto e = _impl->wan_relay_error();
    return e.empty() ? nil : [NSString stringWithUTF8String:e.c_str()];
}

// ── Mic Transmit (TX) ───────────────────────────────────────────────────────

- (BOOL)isMicTransmitting {
    return _txImpl && _txImpl->is_running();
}

- (uint64_t)txPacketsSent {
    return _txImpl ? _txImpl->packets_sent() : 0;
}

- (float)micInputLevel {
    return _txImpl ? _txImpl->peak_level() : 0.0f;
}

- (BOOL)startMicTransmit {
    if (_txImpl && _txImpl->is_running()) return YES;

    // Note: AVAudioSession category is managed by Swift (AudioReceiver.toggleMic)
    // to avoid triggering interruption notifications.

    _txImpl = std::make_unique<TransmitterImpl>(
        std::string([_multicastGroup UTF8String]),
        _port,
        _channels
    );

    if (!_txImpl->start()) {
        fprintf(stderr, "[SolunaTx] Failed to start transmitter\n");
        _txImpl.reset();
        return NO;
    }

    fprintf(stderr, "[SolunaTx] Mic transmit started\n");
    return YES;
}

- (void)stopMicTransmit {
    if (_txImpl) {
        _txImpl->stop();
        _txImpl.reset();
    }

    // Note: AVAudioSession restore is managed by Swift (AudioReceiver.toggleMic)
    fprintf(stderr, "[SolunaTx] Mic transmit stopped\n");
}

@end
