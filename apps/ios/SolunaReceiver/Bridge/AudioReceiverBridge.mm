//
//  AudioReceiverBridge.mm
//  SolunaReceiver
//
//  Objective-C++ bridge for C++ RTP receiver and CoreAudio output
//

#import "AudioReceiverBridge.h"
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreMotion/CMHeadphoneMotionManager.h>

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
#include <mutex>
#include <functional>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>

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

// ============================================================================
// Biquad Filter Primitives (for EQ)
// ============================================================================

struct BiquadCoeffs {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

struct BiquadState {
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
};

/// Compute peaking EQ filter coefficients
inline BiquadCoeffs peaking_eq(float freq_hz, float gain_db, float q, float sample_rate = 48000.0f) {
    if (std::fabs(gain_db) < 0.01f) return {}; // bypass
    float A = std::pow(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * M_PI * freq_hz / sample_rate;
    float alpha = std::sin(w0) / (2.0f * q);
    float a0 = 1.0f + alpha / A;
    BiquadCoeffs c;
    c.b0 = (1.0f + alpha * A) / a0;
    c.b1 = (-2.0f * std::cos(w0)) / a0;
    c.b2 = (1.0f - alpha * A) / a0;
    c.a1 = c.b1;  // same as -2*cos(w0)/a0
    c.a2 = (1.0f - alpha / A) / a0;
    return c;
}

/// Apply biquad filter to a single sample
inline float biquad_process(const BiquadCoeffs& c, BiquadState& s, float x) {
    float y = c.b0 * x + c.b1 * s.x1 + c.b2 * s.x2 - c.a1 * s.y1 - c.a2 * s.y2;
    s.x2 = s.x1; s.x1 = x;
    s.y2 = s.y1; s.y1 = y;
    return y;
}

// ============================================================================
// 3-Band Parametric EQ (200Hz / 1kHz / 5kHz)
// ============================================================================

struct EQ3Band {
    static constexpr int kBands = 3;
    static constexpr float kDefaultFreqs[3] = {200.0f, 1000.0f, 5000.0f};
    static constexpr float kDefaultQ = 1.0f;

    std::atomic<float> gains[3] = {{0}, {0}, {0}};  // dB (-12..+12)
    BiquadCoeffs coeffs[3] = {};
    // Per-channel state (max 2 channels)
    BiquadState state[3][2] = {};

    void update_coeffs() {
        for (int i = 0; i < kBands; i++) {
            coeffs[i] = peaking_eq(kDefaultFreqs[i], gains[i].load(std::memory_order_relaxed), kDefaultQ);
        }
    }

    float process(float sample, int channel) {
        float s = sample;
        for (int i = 0; i < kBands; i++) {
            if (std::fabs(gains[i].load(std::memory_order_relaxed)) > 0.01f) {
                s = biquad_process(coeffs[i], state[i][channel & 1], s);
            }
        }
        return s;
    }
};

constexpr float EQ3Band::kDefaultFreqs[3];

// ============================================================================
// Compressor / Limiter
// ============================================================================

struct Compressor {
    std::atomic<float> threshold{-20.0f};  // dB (-60..0)
    std::atomic<float> ratio{4.0f};        // 1:1 .. 20:1
    std::atomic<float> attack_ms{10.0f};   // ms (0.1-100)
    std::atomic<float> release_ms{100.0f}; // ms (10-1000)
    std::atomic<bool>  enabled{false};

    // Envelope follower state (audio-callback-only)
    float envelope_db = -96.0f;

    /// Process a single sample. Returns compressed sample.
    float process(float sample, float sample_rate = 48000.0f) {
        if (!enabled.load(std::memory_order_relaxed)) return sample;

        float thresh = threshold.load(std::memory_order_relaxed);
        float rat    = ratio.load(std::memory_order_relaxed);
        float att_ms = attack_ms.load(std::memory_order_relaxed);
        float rel_ms = release_ms.load(std::memory_order_relaxed);

        // Input level in dB
        float abs_s = std::fabs(sample);
        float input_db = (abs_s > 1e-10f) ? 20.0f * std::log10(abs_s) : -96.0f;

        // Envelope follower (peak detector with attack/release)
        float att_coeff = std::exp(-1.0f / (att_ms * sample_rate / 1000.0f));
        float rel_coeff = std::exp(-1.0f / (rel_ms * sample_rate / 1000.0f));

        if (input_db > envelope_db)
            envelope_db = att_coeff * envelope_db + (1.0f - att_coeff) * input_db;
        else
            envelope_db = rel_coeff * envelope_db + (1.0f - rel_coeff) * input_db;

        // Gain computation
        float gain_db = 0.0f;
        if (envelope_db > thresh) {
            float over = envelope_db - thresh;
            gain_db = over * (1.0f / rat - 1.0f);
        }

        float gain = std::pow(10.0f, gain_db / 20.0f);
        return sample * gain;
    }
};

// ============================================================================

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

        // Store media_timestamp for sync mode (wall-clock NS, 32-bit truncated)
        if (ostp.media_timestamp != 0) {
            last_media_timestamp.store(ostp.media_timestamp, std::memory_order_relaxed);
        }

        // OSTP payload is int32_t (4 bytes/sample, native byte order) — not S24_LE 3-byte
        size_t frames = payload_size / (sizeof(int32_t) * config_.channels);

        // PLC: conceal gaps of ≤2 packets by repeating the last known frame
        if (gap > 0 && gap <= 2 && frames > 0 && !last_frame_.empty()) {
            size_t plc_frames = last_frame_.size() / config_.channels;
            for (int32_t i = 0; i < gap; i++) {
                ring.write(last_frame_.data(), plc_frames);
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
        if (rtp.pt == kPayloadTypeL24 || rtp.pt == kPayloadTypeL24_AES67) {
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
        } else if (rtp.pt == kPayloadTypeL16 || rtp.pt == kPayloadTypeL16_AES67) {
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

    // Last received OSTP media timestamp (wall-clock NS, 32-bit truncated)
    std::atomic<uint32_t> last_media_timestamp{0};
};

/// WAN relay client — connects to soluna-relay server via UDP
class WanRelayClient {
public:
    enum class State { Disconnected, Connecting, Connected, Error };
    using RxCallback = std::function<void(const uint8_t*, size_t)>;
    using MetaCallback = std::function<void(const std::string&)>;
    using FileCallback = std::function<void(const std::string&)>;
    using SyncCallback = std::function<void(const std::string&)>;

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
            // Handle FILE/SYNC/META that arrive during join handshake
            else if (n >= 5 && memcmp(buf, "FILE:", 5) == 0) {
                std::string fname(buf + 5, n - 5);
                while (!fname.empty() && (fname.back() == '\n' || fname.back() == '\r')) fname.pop_back();
                std::lock_guard<std::mutex> lk(cb_mutex_);
                if (file_callback_) file_callback_(fname);
            }
            else if (n >= 5 && memcmp(buf, "SYNC:", 5) == 0) {
                std::string sync(buf + 5, n - 5);
                while (!sync.empty() && (sync.back() == '\n' || sync.back() == '\r')) sync.pop_back();
                std::lock_guard<std::mutex> lk(cb_mutex_);
                if (sync_callback_) sync_callback_(sync);
            }
            else if (n >= 5 && memcmp(buf, "META:", 5) == 0) {
                std::string meta(buf + 5, n - 5);
                while (!meta.empty() && (meta.back() == '\n' || meta.back() == '\r')) meta.pop_back();
                std::lock_guard<std::mutex> lk(cb_mutex_);
                if (meta_callback_) meta_callback_(meta);
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

    void set_meta_callback(MetaCallback cb) {
        std::lock_guard<std::mutex> lk(cb_mutex_);
        meta_callback_ = std::move(cb);
    }

    void set_file_callback(FileCallback cb) {
        std::lock_guard<std::mutex> lk(cb_mutex_);
        file_callback_ = std::move(cb);
    }

    void set_sync_callback(SyncCallback cb) {
        std::lock_guard<std::mutex> lk(cb_mutex_);
        sync_callback_ = std::move(cb);
    }

    /// Send READY notification to relay
    void send_ready(const std::string& filename) {
        if (udp_sock_ < 0 || state_.load() != State::Connected) return;
        std::string msg = "READY:" + filename + "\n";
        ::sendto(udp_sock_, msg.c_str(), msg.size(), 0,
                 (struct sockaddr*)&relay_addr_, sizeof(relay_addr_));
    }

    State state() const { return state_.load(); }
    const std::string& group() const { return group_; }
    const std::string& error() const { return error_; }

private:
    void recv_loop() {
        struct timeval tv{1, 0};
        setsockopt(udp_sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        uint8_t buf[16384];  // Must fit OSTP packets (up to ~4KB for 480-frame stereo)
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
                // META message: metadata broadcast from relay
                else if (n >= 5 && memcmp(buf, "META:", 5) == 0) {
                    std::string meta((const char*)buf + 5, n - 5);
                    while (!meta.empty() && (meta.back() == '\n' || meta.back() == '\r'))
                        meta.pop_back();
                    std::lock_guard<std::mutex> lk(cb_mutex_);
                    if (meta_callback_) meta_callback_(meta);
                }
                // FILE: message — file sync mode, download and prepare
                else if (n >= 5 && memcmp(buf, "FILE:", 5) == 0) {
                    std::string filename((const char*)buf + 5, n - 5);
                    while (!filename.empty() && (filename.back() == '\n' || filename.back() == '\r'))
                        filename.pop_back();
                    std::lock_guard<std::mutex> lk(cb_mutex_);
                    if (file_callback_) file_callback_(filename);
                }
                // SYNC: message — file sync mode, play/pause/seek
                else if (n >= 5 && memcmp(buf, "SYNC:", 5) == 0) {
                    std::string sync((const char*)buf + 5, n - 5);
                    while (!sync.empty() && (sync.back() == '\n' || sync.back() == '\r'))
                        sync.pop_back();
                    std::lock_guard<std::mutex> lk(cb_mutex_);
                    if (sync_callback_) sync_callback_(sync);
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
    MetaCallback meta_callback_;
    FileCallback file_callback_;
    SyncCallback sync_callback_;
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

// ============================================================================
// DJ Broadcaster — decode audio file and stream via OSTP to relay
// ============================================================================

class DJBroadcaster {
public:
    explicit DJBroadcaster(WanRelayClient* relay)
        : relay_(relay)
        , running_(false)
        , skip_flag_(false)
        , progress_(0.0f)
        , ssrc_(arc4random())
    {}

    ~DJBroadcaster() { stop_mic_capture(); stop(); }

    // ── Mic mixing ──────────────────────────────────────────────────────
    std::atomic<bool> mic_mix_enabled_{false};
    std::atomic<float> mic_gain_{1.0f};
    std::atomic<float> music_gain_{0.7f};

    void set_mic_mix(bool enabled) {
        if (enabled && !mic_queue_) {
            start_mic_capture();
        } else if (!enabled && mic_queue_) {
            stop_mic_capture();
        }
        mic_mix_enabled_ = enabled;
    }

    bool start(const std::string& filepath) {
        if (running_.load()) return false;

        filepath_ = filepath;
        skip_flag_.store(false);
        progress_.store(0.0f);

        // Extract filename from path
        auto slash = filepath_.rfind('/');
        track_name_ = (slash != std::string::npos) ? filepath_.substr(slash + 1) : filepath_;

        running_.store(true);
        broadcast_thread_ = std::thread([this]() { broadcast_loop(); });

        fprintf(stderr, "[SolunaDJ] Started broadcast: %s\n", track_name_.c_str());
        return true;
    }

    void stop() {
        if (!running_.load()) return;
        running_.store(false);
        if (broadcast_thread_.joinable()) broadcast_thread_.join();
        track_name_.clear();
        progress_.store(0.0f);
        fprintf(stderr, "[SolunaDJ] Stopped\n");
    }

    void skip() {
        skip_flag_.store(true);
    }

    bool is_running() const { return running_.load(); }
    float progress() const { return progress_.load(std::memory_order_relaxed); }
    std::string track_name() const { return track_name_; }

private:
    // ── Mic capture internals ───────────────────────────────────────────
    AudioQueueRef mic_queue_ = nullptr;
    std::vector<float> mic_ring_;  // stereo ring buffer
    std::atomic<size_t> mic_write_pos_{0};
    size_t mic_read_pos_ = 0;
    static constexpr size_t kMicRingSize = 48000;  // 1 second @ 48kHz

    void start_mic_capture() {
        @autoreleasepool {
            // Configure AVAudioSession for play+record (mic needs .playAndRecord)
            AVAudioSession* session = [AVAudioSession sharedInstance];
            NSError* err = nil;
            [session setCategory:AVAudioSessionCategoryPlayAndRecord
                     withOptions:AVAudioSessionCategoryOptionDefaultToSpeaker |
                                 AVAudioSessionCategoryOptionAllowBluetooth |
                                 AVAudioSessionCategoryOptionMixWithOthers
                           error:&err];
            if (err) {
                fprintf(stderr, "[DJ-Mic] AVAudioSession setCategory failed: %s\n",
                        err.localizedDescription.UTF8String);
            }
            [session setActive:YES error:&err];
        }

        mic_ring_.assign(kMicRingSize * 2, 0.0f);  // stereo
        mic_write_pos_.store(0, std::memory_order_relaxed);
        mic_read_pos_ = 0;

        AudioStreamBasicDescription fmt{};
        fmt.mSampleRate = 48000;
        fmt.mFormatID = kAudioFormatLinearPCM;
        fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        fmt.mBytesPerPacket = 4;
        fmt.mFramesPerPacket = 1;
        fmt.mBytesPerFrame = 4;
        fmt.mChannelsPerFrame = 1;  // mono mic
        fmt.mBitsPerChannel = 32;

        OSStatus st = AudioQueueNewInput(&fmt, mic_aq_callback, this,
                                         nullptr, nullptr, 0, &mic_queue_);
        if (st != noErr) {
            fprintf(stderr, "[DJ-Mic] AudioQueueNewInput failed: %d\n", (int)st);
            mic_queue_ = nullptr;
            return;
        }

        for (int i = 0; i < 3; i++) {
            AudioQueueBufferRef buf;
            AudioQueueAllocateBuffer(mic_queue_, 480 * 4, &buf);
            AudioQueueEnqueueBuffer(mic_queue_, buf, 0, nullptr);
        }
        st = AudioQueueStart(mic_queue_, nullptr);
        if (st != noErr) {
            fprintf(stderr, "[DJ-Mic] AudioQueueStart failed: %d\n", (int)st);
            AudioQueueDispose(mic_queue_, true);
            mic_queue_ = nullptr;
            return;
        }
        fprintf(stderr, "[DJ-Mic] Mic capture started (iOS)\n");
    }

    void stop_mic_capture() {
        if (mic_queue_) {
            AudioQueueStop(mic_queue_, true);
            AudioQueueDispose(mic_queue_, true);
            mic_queue_ = nullptr;
            fprintf(stderr, "[DJ-Mic] Mic capture stopped\n");
        }
    }

    static void mic_aq_callback(void* ctx, AudioQueueRef queue,
                                AudioQueueBufferRef buf,
                                const AudioTimeStamp*, UInt32,
                                const AudioStreamPacketDescription*) {
        auto* self = static_cast<DJBroadcaster*>(ctx);
        const float* data = (const float*)buf->mAudioData;
        size_t frames = buf->mAudioDataByteSize / sizeof(float);
        size_t wr = self->mic_write_pos_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < frames; i++) {
            size_t idx = (wr + i) % self->kMicRingSize;
            self->mic_ring_[idx * 2]     = data[i];  // L = mono
            self->mic_ring_[idx * 2 + 1] = data[i];  // R = mono
        }
        self->mic_write_pos_.store(wr + frames, std::memory_order_release);
        AudioQueueEnqueueBuffer(queue, buf, 0, nullptr);
    }

    /// Mix mic samples into interleaved S24 payload buffer (in-place)
    void mix_mic_into_payload(int32_t* payload, uint32_t frames) {
        if (!mic_mix_enabled_.load(std::memory_order_relaxed)) return;

        float mg  = mic_gain_.load(std::memory_order_relaxed);
        float mug = music_gain_.load(std::memory_order_relaxed);
        size_t mic_avail = mic_write_pos_.load(std::memory_order_acquire) - mic_read_pos_;
        size_t mic_frames = std::min(mic_avail, (size_t)frames);

        for (uint32_t i = 0; i < frames; i++) {
            for (int ch = 0; ch < 2; ch++) {
                float music = (float)payload[i * 2 + ch] / 8388607.0f * mug;
                float mic = 0.0f;
                if (i < mic_frames) {
                    size_t idx = (mic_read_pos_ + i) % kMicRingSize;
                    mic = mic_ring_[idx * 2 + ch] * mg;
                }
                float mixed = music + mic;
                if (mixed > 1.0f) mixed = 1.0f;
                if (mixed < -1.0f) mixed = -1.0f;
                payload[i * 2 + ch] = (int32_t)(mixed * 8388607.0f);
            }
        }
        mic_read_pos_ += mic_frames;
    }

    void send_relay_command(const std::string& cmd) {
        if (!relay_) return;
        // Use the relay's send_audio path which forwards to relay server + peers.
        // But relay commands are text, not audio — send directly via relay's internal socket.
        // We reuse send_audio which sends to relay_addr_ and all peers.
        relay_->send_audio(reinterpret_cast<const uint8_t*>(cmd.c_str()), cmd.size());
    }

    void broadcast_loop() {
        @autoreleasepool {
            NSURL* url = [NSURL fileURLWithPath:@(filepath_.c_str())];
            NSError* err = nil;
            AVAudioFile* file = [[AVAudioFile alloc] initForReading:url error:&err];
            if (!file) {
                fprintf(stderr, "[SolunaDJ] Failed to open file: %s\n",
                        err ? err.localizedDescription.UTF8String : "unknown");
                running_.store(false);
                return;
            }

            AVAudioFormat* procFmt = [[AVAudioFormat alloc]
                initWithCommonFormat:AVAudioPCMFormatFloat32
                sampleRate:48000 channels:2 interleaved:NO];

            AVAudioFrameCount totalFrames = (AVAudioFrameCount)file.length;

            // Send FILE: and META: commands to relay
            std::string file_cmd = "FILE:" + track_name_ + "\n";
            send_relay_command(file_cmd);

            std::string meta_cmd = "META:{\"track\":\"" + track_name_ + "\",\"source\":\"dj\"}\n";
            send_relay_command(meta_cmd);

            constexpr uint32_t kFramesPerPacket = 480; // 10ms @ 48kHz
            AVAudioPCMBuffer* buf = [[AVAudioPCMBuffer alloc]
                initWithPCMFormat:procFmt frameCapacity:kFramesPerPacket];

            const size_t frame_size = 2 * sizeof(int32_t); // stereo S24
            std::vector<uint8_t> packet_buf(transport::kMaxPacketSize);
            int32_t payload[kFramesPerPacket * 2]; // stereo interleaved S24

            uint32_t sequence = 0;
            uint32_t rtp_timestamp = 0;
            AVAudioFramePosition framesRead = 0;
            bool sent_sync = false;

            auto wall_start = std::chrono::steady_clock::now();

            while (running_.load() && !skip_flag_.load()) {
                buf.frameLength = 0;
                [file readIntoBuffer:buf frameCount:kFramesPerPacket error:&err];
                if (buf.frameLength == 0) break; // EOF

                // Convert float32 to int32 S24
                const float* left = buf.floatChannelData[0];
                const float* right = buf.floatChannelData[1];
                for (uint32_t i = 0; i < buf.frameLength; i++) {
                    payload[i * 2]     = (int32_t)(left[i] * 8388607.0f);
                    payload[i * 2 + 1] = (int32_t)(right[i] * 8388607.0f);
                }

                // Mix mic audio into music if enabled
                mix_mic_into_payload(payload, buf.frameLength);

                // Build OSTP packet (same pattern as TransmitterImpl)
                uint16_t seq_lo = static_cast<uint16_t>(sequence & 0xFFFF);
                uint16_t seq_hi = static_cast<uint16_t>((sequence >> 16) & 0xFFFF);

                size_t pkt_size = transport::ostp_build_packet(
                    packet_buf.data(), packet_buf.size(),
                    ssrc_, seq_lo, rtp_timestamp,
                    96,   // kPayloadTypePCM24
                    1,    // stream_id
                    seq_hi,
                    0,    // media_timestamp
                    payload,
                    buf.frameLength * frame_size
                );

                if (pkt_size > 0 && relay_) {
                    relay_->send_audio(packet_buf.data(), pkt_size);
                }

                framesRead += buf.frameLength;
                sequence++;
                rtp_timestamp += buf.frameLength;

                // Update progress
                if (totalFrames > 0) {
                    progress_.store((float)framesRead / (float)totalFrames,
                                    std::memory_order_relaxed);
                }

                // Send SYNC: command after 3 seconds of streaming
                if (!sent_sync && framesRead >= 48000 * 3) {
                    uint64_t wall_ms = (uint64_t)std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                    uint64_t pos_ms = (uint64_t)framesRead * 1000 / 48000;
                    std::string sync_cmd = "SYNC:play:" + std::to_string(pos_ms)
                                         + ":" + std::to_string(wall_ms) + "\n";
                    send_relay_command(sync_cmd);
                    sent_sync = true;
                }

                // Pace to real-time: sleep until next packet time
                auto next_time = wall_start + std::chrono::microseconds(
                    (uint64_t)framesRead * 1000000 / 48000);
                std::this_thread::sleep_until(next_time);
            }

            progress_.store(1.0f, std::memory_order_relaxed);
            running_.store(false);
            fprintf(stderr, "[SolunaDJ] Broadcast finished: %s (frames=%lld)\n",
                    track_name_.c_str(), (long long)framesRead);
        }
    }

    WanRelayClient* relay_;  // borrowed, owned by ReceiverImpl
    std::atomic<bool> running_;
    std::atomic<bool> skip_flag_;
    std::atomic<float> progress_;
    uint32_t ssrc_;
    std::string filepath_;
    std::string track_name_;
    std::thread broadcast_thread_;
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
        , target_fill_frames_(2880)  // 60ms initial — adaptive algorithm grows as needed
        , ring_buffer_(192000, channels * sizeof(int32_t)) // 4s capacity for WiFi jitter
        , read_buffer_(4096 * channels)
        , drain_buf_(4096 * channels)
        , held_sample_(channels, 0)
        , ramp_(0.0f)
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

    // ── Sync mode ──────────────────────────────────────────────────────────
    void set_sync_mode(bool enabled) {
        sync_mode_.store(enabled);
        if (enabled) {
            target_fill_frames_.store(sync_delay_ms_.load() * 48u);
        }
    }
    bool is_sync_mode() const { return sync_mode_.load(); }

    void set_sync_delay_ms(uint32_t ms) {
        ms = std::max(50u, std::min(1000u, ms));
        sync_delay_ms_.store(ms);
        if (sync_mode_.load()) {
            target_fill_frames_.store(ms * 48u);
        }
    }
    uint32_t get_sync_delay_ms() const { return sync_delay_ms_.load(); }

    // ── Output latency compensation (Bluetooth/AirPlay) ─────────────────
    void set_output_latency_ms(float ms) { output_latency_ms_.store(std::max(0.0f, ms)); }
    float get_output_latency_ms() const { return output_latency_ms_.load(); }

    // ── Loudness normalization (EBU R128) ─────────────────────────────────
    void set_loudness_norm(bool enabled) { loudness_norm_enabled_.store(enabled); }
    bool is_loudness_norm() const { return loudness_norm_enabled_.load(); }

    // ── 3-Band EQ ─────────────────────────────────────────────────────────
    void set_eq(int band, float gain_db) {
        if (band >= 0 && band < 3) {
            eq_.gains[band].store(std::clamp(gain_db, -12.0f, 12.0f));
            eq_.update_coeffs();
        }
    }

    // ── Compressor ────────────────────────────────────────────────────────
    void set_compressor(float thresh, float rat, float att, float rel, bool en) {
        compressor_.threshold.store(std::clamp(thresh, -60.0f, 0.0f));
        compressor_.ratio.store(std::clamp(rat, 1.0f, 20.0f));
        compressor_.attack_ms.store(std::clamp(att, 0.1f, 100.0f));
        compressor_.release_ms.store(std::clamp(rel, 10.0f, 1000.0f));
        compressor_.enabled.store(en);
    }

    // ── Recording ─────────────────────────────────────────────────────────
    void set_record_callback(std::function<void(const float*, uint32_t)> cb) {
        std::lock_guard<std::mutex> lock(record_mutex_);
        record_callback_ = std::move(cb);
    }

    SimpleRtpReceiver::Stats stats() const {
        if (rtp_receiver_) return rtp_receiver_->stats_snapshot();
        return {};
    }

    int device_health() const {
        return health_.load(std::memory_order_relaxed);
    }

    void set_filesync_network_disabled(bool d) { filesync_network_disabled_.store(d); }
    bool is_filesync_network_disabled() const { return filesync_network_disabled_.load(); }
    bool is_network_disabled() const { return relay_network_disabled_.load() || filesync_network_disabled_.load(); }

    // ── Relay support ──────────────────────────────────────────────────────

    void set_relay_callback(std::function<void(const uint8_t*, size_t)> cb) {
        relay_callback_ = std::move(cb);
        if (rtp_receiver_) rtp_receiver_->relay_callback = relay_callback_;
    }

    void inject_raw_packet(const uint8_t* data, size_t len) {
        if (rtp_receiver_) rtp_receiver_->inject_raw_packet(data, len, ring_buffer_);
    }

    void inject_pcm_samples(const int32_t* samples, size_t frame_count) {
        ring_buffer_.write(samples, frame_count);
    }

    void flush_ring_buffer() {
        flush_requested_.store(true, std::memory_order_release);
    }

    // ── WAN Relay ─────────────────────────────────────────────────────────

    bool wan_relay_connect(const std::string& host, uint16_t port,
                           const std::string& group, const std::string& password) {
        if (!wan_relay_) wan_relay_ = std::make_unique<WanRelayClient>();
        wan_relay_->set_rx_callback([this](const uint8_t* data, size_t len) {
            inject_raw_packet(data, len);
        });
        // Apply stored callbacks BEFORE connect so they're active when
        // the relay sends FILE:/SYNC: to new joiners immediately after JOIN
        if (stored_meta_cb_) wan_relay_->set_meta_callback(stored_meta_cb_);
        if (stored_file_cb_) wan_relay_->set_file_callback(stored_file_cb_);
        if (stored_sync_cb_) wan_relay_->set_sync_callback(stored_sync_cb_);
        bool ok = wan_relay_->connect(host, port, group, password, "iPhone");
        if (ok) {
            // Switch to relay-only mode: multicast is LAN-only, relay carries WAN audio
            relay_network_disabled_.store(true);
        }
        return ok;
    }

    void wan_relay_disconnect() {
        if (wan_relay_) wan_relay_->disconnect();
        relay_network_disabled_.store(false);  // restore multicast
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

    void wan_relay_set_meta_callback(WanRelayClient::MetaCallback cb) {
        stored_meta_cb_ = cb;
        if (wan_relay_) wan_relay_->set_meta_callback(std::move(cb));
    }

    void wan_relay_set_file_callback(WanRelayClient::FileCallback cb) {
        stored_file_cb_ = cb;
        if (wan_relay_) wan_relay_->set_file_callback(std::move(cb));
    }

    void wan_relay_set_sync_callback(WanRelayClient::SyncCallback cb) {
        stored_sync_cb_ = cb;
        if (wan_relay_) wan_relay_->set_sync_callback(std::move(cb));
    }

    void wan_relay_send_ready(const std::string& filename) {
        if (wan_relay_) wan_relay_->send_ready(filename);
    }

    void wan_relay_send_audio(const uint8_t* data, size_t len) {
        if (wan_relay_) wan_relay_->send_audio(data, len);
    }

    std::unique_ptr<WanRelayClient> wan_relay_;
    WanRelayClient::MetaCallback stored_meta_cb_;
    WanRelayClient::FileCallback stored_file_cb_;
    WanRelayClient::SyncCallback stored_sync_cb_;

    // ── Public accessors for ObjC bridge ──────────────────────────────────
    float output_peak() const { return output_peak_.load(std::memory_order_relaxed); }
    size_t ring_buffer_available_read() const { return ring_buffer_.available_read(); }
    uint32_t get_target_fill_frames() const { return target_fill_frames_.load(); }
    SimpleRtpReceiver::Stats get_rtp_stats() const {
        return rtp_receiver_ ? rtp_receiver_->stats_snapshot() : SimpleRtpReceiver::Stats{};
    }
    bool has_rtp_receiver() const { return rtp_receiver_ != nullptr; }

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

        // ── Adaptive buffer: increase on underrun ──────────────────────────
        // Bump target by 20ms (960 frames @ 48kHz) each underrun, up to 2000ms max.
        uint32_t cur_target = target_fill_frames_.load(std::memory_order_relaxed);
        constexpr uint32_t kBumpFrames = 960;    // 20ms
        constexpr uint32_t kMaxTarget  = 96000;  // 2000ms
        if (cur_target + kBumpFrames <= kMaxTarget) {
            target_fill_frames_.store(cur_target + kBumpFrames, std::memory_order_relaxed);
        }

        int cur = health_.load(std::memory_order_relaxed);
        if (health_underruns_in_window_ >= 200 && cur < 2) {
            // Extreme underruns: silence device to prevent noise
            health_.store(2, std::memory_order_relaxed);
            health_silenced_.store(true, std::memory_order_relaxed);
        } else if (health_underruns_in_window_ >= 30 && cur < 1) {
            // Moderate underruns: mark stressed (UI indicator only)
            health_.store(1, std::memory_order_relaxed);
        }
    }

    void maybe_check_recovery() {
        if (++recovery_check_counter_ < 200) return;  // ~1 s at 5ms/callback
        recovery_check_counter_ = 0;

        // ── Adaptive buffer: shrink when stable ────────────────────────────
        // If no underrun for 5 seconds, reduce target by 5ms (240 frames).
        // This slowly converges to the minimum viable buffer for the network.
        constexpr uint32_t kShrinkFrames = 240;   // 5ms
        constexpr uint32_t kMinTarget    = 1440;   // 30ms floor
        uint64_t now = now_ms_();
        if (last_underrun_ms_ != 0 && now - last_underrun_ms_ >= 5000) {
            uint32_t cur_target = target_fill_frames_.load(std::memory_order_relaxed);
            if (cur_target > kMinTarget + kShrinkFrames) {
                target_fill_frames_.store(cur_target - kShrinkFrames, std::memory_order_relaxed);
            }
        }

        // Health recovery: 10 seconds clean → restore good
        if (health_.load(std::memory_order_relaxed) == 0) return;
        if (last_underrun_ms_ == 0) return;
        if (now - last_underrun_ms_ >= 10000) {
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
            // When network disabled (relay or file-sync), audio arrives via inject_raw_packet instead
            if (relay_network_disabled_.load() || filesync_network_disabled_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (rtp_receiver_) {
                for (int i = 0; i < 10 && running_.load(); i++) {
                    if (!rtp_receiver_->receive_packet(ring_buffer_)) break;
                }
            }

            // ── Sync mode: adjust buffer target from OSTP wall-clock timestamps ──
            if (sync_mode_.load(std::memory_order_relaxed) && rtp_receiver_) {
                uint32_t media_ts = rtp_receiver_->last_media_timestamp.load(std::memory_order_relaxed);
                if (media_ts != 0) {
                    struct timespec now_ts;
                    clock_gettime(CLOCK_REALTIME, &now_ts);
                    uint32_t now_ns32 = static_cast<uint32_t>(
                        (static_cast<uint64_t>(now_ts.tv_sec) * 1'000'000'000ULL + now_ts.tv_nsec)
                        & 0xFFFFFFFF);
                    int32_t net_delay_ns = static_cast<int32_t>(now_ns32 - media_ts);
                    if (net_delay_ns >= 0 && net_delay_ns < 2'000'000'000) {
                        // Add hardware output latency (Bluetooth/AirPlay) to sync delay
                        float hw_latency_ms = output_latency_ms_.load(std::memory_order_relaxed);
                        uint32_t total_delay_ms = sync_delay_ms_.load() + static_cast<uint32_t>(hw_latency_ms);
                        uint32_t sync_delay_ns = total_delay_ms * 1'000'000u;
                        int32_t buffer_ns = static_cast<int32_t>(sync_delay_ns) - net_delay_ns;
                        if (buffer_ns < 5'000'000) buffer_ns = 5'000'000;
                        uint32_t target = static_cast<uint32_t>(
                            (static_cast<int64_t>(buffer_ns) * 48000) / 1'000'000'000LL);
                        uint32_t prev = target_fill_frames_.load();
                        target_fill_frames_.store(static_cast<uint32_t>(prev * 0.98 + target * 0.02));
                    }
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
        if (flush_requested_.load(std::memory_order_acquire)) {
            flush_requested_.store(false, std::memory_order_relaxed);
            ring_buffer_.reset();
            prefilled_ = false;
            ramp_ = 0.0f;
        }

        const float vol = (muted_.load() || health_silenced_.load(std::memory_order_relaxed))
                          ? 0.0f : volume_.load();
        const uint32_t total_samples = frame_count * channels_;

        uint32_t target = target_fill_frames_.load();
        // Ensure target is at least a few callbacks worth
        const uint32_t min_target = frame_count * 3;
        if (target < min_target) target = min_target;

        // ── Gradual drift correction with crossfade ────────────────────────
        // Only discard when buffer is significantly overfilled (3x target)
        // to avoid WiFi burst → discard → underrun cycle.
        {
            size_t avail_now = ring_buffer_.available_read();
            if (prefilled_ && avail_now > static_cast<size_t>(target) * 3) {
                size_t excess = avail_now - static_cast<size_t>(target) * 2;
                // Discard at most 1 sample per callback — very gradual
                size_t drift = std::min(excess, static_cast<size_t>(frame_count / 80 + 1));
                ring_buffer_.discard(drift);
                drift_xfade_ = 48;
            }
        }

        const size_t avail = ring_buffer_.available_read();

        constexpr float kFadeIn  = 0.002f;  // faster fade-in for snappier recovery
        constexpr float kFadeOut = 0.004f;

        // ── Initial prefill (only at startup, NOT reset on underrun) ───────
        if (!prefilled_) {
            if (avail < min_target) {
                std::memset(buffer, 0, total_samples * sizeof(float));
                return;
            }
            prefilled_ = true;
            ramp_ = 0.0f;  // ensure clean fade-in
        }

        // ── Underrun: play what we have, then fade out remainder ──────────
        // This avoids hard cuts on short WiFi hiccups.
        if (avail < frame_count) {
            record_underrun_now();
            maybe_check_recovery();
            // Play available samples first
            const size_t have = avail;
            if (have > 0) {
                ring_buffer_.read(read_buffer_.data(), have);
                const int32_t* src = read_buffer_.data();
                for (uint32_t i = 0; i < have; i++) {
                    ramp_ += kFadeIn * (vol - ramp_);
                    for (uint32_t ch = 0; ch < channels_; ch++) {
                        const uint32_t idx = i * channels_ + ch;
                        float s = static_cast<float>(src[idx]) / 8388608.0f;
                        if (s > 1.0f) s = 1.0f;
                        else if (s < -1.0f) s = -1.0f;
                        float out = s * ramp_;
                        buffer[idx] = out;
                        held_sample_[ch] = out;
                    }
                }
            }
            // Fade out the remainder using last held sample
            for (uint32_t i = static_cast<uint32_t>(have); i < frame_count; i++) {
                ramp_ *= (1.0f - kFadeOut);
                for (uint32_t ch = 0; ch < channels_; ch++) {
                    buffer[i * channels_ + ch] = held_sample_[ch] * ramp_;
                }
            }
            return;
        }

        maybe_check_recovery();

        // ── Normal playback ────────────────────────────────────────────────
        ring_buffer_.read(read_buffer_.data(), frame_count);
        const int32_t* src = read_buffer_.data();
        for (uint32_t i = 0; i < frame_count; i++) {
            ramp_ += kFadeIn * (vol - ramp_);
            for (uint32_t ch = 0; ch < channels_; ch++) {
                const uint32_t idx = i * channels_ + ch;
                float s = static_cast<float>(src[idx]) / 8388608.0f;
                if (s > 1.0f) s = 1.0f;
                else if (s < -1.0f) s = -1.0f;
                // 3-band EQ
                s = eq_.process(s, ch);
                // Compressor
                s = compressor_.process(s);
                float out = s * ramp_;
                if (drift_xfade_ > 0) {
                    float alpha = 1.0f - static_cast<float>(drift_xfade_) / 49.0f;
                    out = out * alpha + held_sample_[ch] * (1.0f - alpha);
                }
                buffer[idx] = out;
                held_sample_[ch] = out;
            }
            if (drift_xfade_ > 0) drift_xfade_--;
        }

        // ── EBU R128 loudness normalization ──────────────────────────────
        if (loudness_norm_enabled_.load(std::memory_order_relaxed)) {
            // Simplified K-weighted loudness measurement — mean square power
            float sum_sq = 0;
            for (uint32_t i = 0; i < total_samples; i++) {
                float s = buffer[i];
                sum_sq += s * s;
            }
            float mean_sq = sum_sq / total_samples;

            // Store power (mean square) in sliding window (~400ms at ~120 callbacks/sec)
            // Power-domain averaging is correct for EBU R128 (not dB averaging)
            loudness_window_[loudness_window_pos_] = mean_sq;
            loudness_window_pos_ = (loudness_window_pos_ + 1) % 48;
            if (loudness_window_pos_ == 0) loudness_window_full_ = true;

            // Calculate integrated loudness — average power then convert to dB
            int count = loudness_window_full_ ? 48 : loudness_window_pos_;
            if (count > 0) {
                float power_sum = 0;
                for (int i = 0; i < count; i++) power_sum += loudness_window_[i];
                float avg_power = power_sum / count;
                float avg_db = (avg_power > 1e-20f) ? 10.0f * log10f(avg_power) : -100.0f;

                // Target -23 LUFS, calculate needed gain
                float target_db = -23.0f;
                float gain_db = target_db - avg_db;
                // Clamp gain to reasonable range (-12 to +12 dB)
                if (gain_db < -12.0f) gain_db = -12.0f;
                if (gain_db > 12.0f) gain_db = 12.0f;

                float target_gain = powf(10.0f, gain_db / 20.0f);
                // Smooth gain changes (EMA)
                loudness_current_gain_ = loudness_current_gain_ * 0.95f + target_gain * 0.05f;
            }

            // Apply gain
            for (uint32_t i = 0; i < total_samples; i++) {
                buffer[i] *= loudness_current_gain_;
            }
        }

        // ── Output level metering (for visualization) ───────────────────
        {
            float peak = 0.0f;
            for (uint32_t i = 0; i < total_samples; i++) {
                float a = buffer[i] < 0 ? -buffer[i] : buffer[i];
                if (a > peak) peak = a;
            }
            float prev = output_peak_.load(std::memory_order_relaxed);
            if (peak > prev) {
                output_peak_.store(peak, std::memory_order_relaxed);
            } else {
                output_peak_.store(prev * 0.92f, std::memory_order_relaxed);
            }
        }

        // ── Recording: write rendered samples ────────────────────────────
        {
            std::lock_guard<std::mutex> lock(record_mutex_);
            if (record_callback_) {
                record_callback_(buffer, total_samples);
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
    std::atomic<bool>     relay_network_disabled_{false};
    std::atomic<bool>     filesync_network_disabled_{false};
    std::atomic<bool>     sync_mode_{true};
    std::atomic<uint32_t> sync_delay_ms_{200};
    // Output latency compensation (Bluetooth/AirPlay):
    std::atomic<float>    output_latency_ms_{0.0f};
    // Loudness normalization (EBU R128):
    std::atomic<bool>     loudness_norm_enabled_{false};
    // Output level metering (written audio-cb, read ObjC for visualization):
    std::atomic<float>    output_peak_{0.0f};
    // 3-band EQ and compressor (audio-callback-only processing state):
    EQ3Band               eq_;
    Compressor            compressor_;
    // Recording callback (called from audio callback with float samples):
    std::function<void(const float*, uint32_t)> record_callback_;
    std::mutex record_mutex_;
    // Health tracking atomics (written audio-cb, read ObjC):
    std::atomic<int>      health_{0};           ///< 0=good 1=stressed 2=silenced
    std::atomic<bool>     health_silenced_{false};
    std::atomic<bool>     flush_requested_{false};
    // audio_callback-only state (no atomics needed):
    bool                  prefilled_ = false;
    float                 ramp_      = 0.0f;
    std::vector<float>    held_sample_;
    // Drift correction crossfade counter (audio-callback-only):
    uint32_t drift_xfade_ = 0;
    // Loudness normalization state (audio-callback-only):
    float loudness_current_gain_ = 1.0f;
    float loudness_window_[48]{};           // 400ms window at ~120 callbacks/sec
    int   loudness_window_pos_ = 0;
    bool  loudness_window_full_ = false;
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
    std::unique_ptr<DJBroadcaster> _djImpl;
    NSTimer *_statsTimer;
    uint32_t _bufferTargetMs;
    CMHeadphoneMotionManager *_headphoneMotion;
    ExtAudioFileRef _recordFile;
    BOOL _isRecording;
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
        _bufferTargetMs = 80;
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
    if (_impl) _impl->set_filesync_network_disabled((bool)disabled);
}

- (BOOL)filesyncNetworkDisabled {
    return _impl ? (BOOL)_impl->is_filesync_network_disabled() : NO;
}

- (void)setFilesyncNetworkDisabled:(BOOL)disabled {
    if (_impl) _impl->set_filesync_network_disabled((bool)disabled);
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

- (void)setMetaCallback:(nullable void(^)(NSString *))callback {
    if (_impl) {
        if (callback) {
            _impl->wan_relay_set_meta_callback([callback](const std::string& json) {
                NSString *str = [NSString stringWithUTF8String:json.c_str()];
                dispatch_async(dispatch_get_main_queue(), ^{
                    callback(str);
                });
            });
        } else {
            _impl->wan_relay_set_meta_callback(nullptr);
        }
    }
}

- (void)setFileCallback:(nullable void(^)(NSString *))callback {
    if (_impl) {
        if (callback) {
            _impl->wan_relay_set_file_callback([callback](const std::string& filename) {
                NSString *str = [NSString stringWithUTF8String:filename.c_str()];
                dispatch_async(dispatch_get_main_queue(), ^{
                    callback(str);
                });
            });
        } else {
            _impl->wan_relay_set_file_callback(nullptr);
        }
    }
}

- (void)setSyncCallback:(nullable void(^)(NSString *))callback {
    if (_impl) {
        if (callback) {
            _impl->wan_relay_set_sync_callback([callback](const std::string& sync) {
                NSString *str = [NSString stringWithUTF8String:sync.c_str()];
                dispatch_async(dispatch_get_main_queue(), ^{
                    callback(str);
                });
            });
        } else {
            _impl->wan_relay_set_sync_callback(nullptr);
        }
    }
}

- (void)sendReady:(NSString *)filename {
    if (_impl) {
        _impl->wan_relay_send_ready(std::string(filename.UTF8String));
    }
}

- (void)injectRawPacket:(NSData *)data {
    if (_impl && data.length > 0) {
        _impl->inject_raw_packet(static_cast<const uint8_t*>(data.bytes), data.length);
    }
}

- (void)injectPcmSamples:(NSData *)data frameCount:(NSUInteger)frameCount {
    if (_impl && data.length > 0) {
        _impl->inject_pcm_samples(static_cast<const int32_t*>(data.bytes), frameCount);
    }
}

- (void)flushRingBuffer {
    if (_impl) {
        _impl->flush_ring_buffer();
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

- (void)sendAudioViaWanRelay:(NSData *)data {
    if (_impl) _impl->wan_relay_send_audio(
        static_cast<const uint8_t*>(data.bytes), data.length);
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

// ── Sync Mode ───────────────────────────────────────────────────────────────

- (BOOL)syncMode {
    return _impl ? _impl->is_sync_mode() : NO;
}

- (void)setSyncMode:(BOOL)syncMode {
    if (_impl) _impl->set_sync_mode(syncMode);
}

- (uint32_t)syncDelayMs {
    return _impl ? _impl->get_sync_delay_ms() : 200;
}

- (void)setSyncDelayMs:(uint32_t)syncDelayMs {
    if (_impl) _impl->set_sync_delay_ms(syncDelayMs);
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

- (float)outputPeakLevel {
    return _impl ? _impl->output_peak() : 0.0f;
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

// ── Network Quality Stats ───────────────────────────────────────────────────

- (float)networkLatencyMs {
    if (!_impl) return 0;
    // Approximate latency from ring buffer fill depth (at 48kHz)
    size_t fill = _impl->ring_buffer_available_read();
    return (float)fill / 48.0f;
}

- (float)jitterMs {
    if (!_impl) return 0;
    // Approximate jitter from difference between actual and target buffer fill
    size_t fill = _impl->ring_buffer_available_read();
    uint32_t target = _impl->get_target_fill_frames();
    int32_t diff = (int32_t)fill - (int32_t)target;
    return fabsf((float)diff / 48.0f);
}

- (float)packetLossPercent {
    if (!_impl || !_impl->has_rtp_receiver()) return 0;
    auto st = _impl->get_rtp_stats();
    if (st.packets_received == 0) return 0;
    uint64_t total = st.packets_received + st.packets_dropped;
    return (float)st.packets_dropped / (float)total * 100.0f;
}

// ── Loudness Normalization ──────────────────────────────────────────────────

- (void)setLoudnessNormEnabled:(BOOL)enabled {
    if (_impl) _impl->set_loudness_norm(enabled);
}

- (BOOL)loudnessNormEnabled {
    return _impl ? _impl->is_loudness_norm() : NO;
}

// ── Output Latency Compensation ─────────────────────────────────────────────

- (void)setOutputLatencyMs:(float)ms {
    if (_impl) _impl->set_output_latency_ms(ms);
}

- (float)outputLatencyMs {
    return _impl ? _impl->get_output_latency_ms() : 0.0f;
}

// ── Spatial Audio (AirPods Head Tracking) ───────────────────────────────────

- (void)setSpatialAudioEnabled:(BOOL)enabled {
    if (enabled && !_headphoneMotion) {
        _headphoneMotion = [[CMHeadphoneMotionManager alloc] init];
        if (_headphoneMotion.isDeviceMotionAvailable) {
            [_headphoneMotion startDeviceMotionUpdates];
            fprintf(stderr, "[SolunaRx] Spatial audio: head tracking started\n");
        } else {
            fprintf(stderr, "[SolunaRx] Spatial audio: head tracking not available\n");
            _headphoneMotion = nil;
        }
    } else if (!enabled && _headphoneMotion) {
        [_headphoneMotion stopDeviceMotionUpdates];
        _headphoneMotion = nil;
        fprintf(stderr, "[SolunaRx] Spatial audio: head tracking stopped\n");
    }
}

- (BOOL)spatialAudioEnabled {
    return _headphoneMotion != nil;
}

// ── 3-Band EQ ───────────────────────────────────────────────────────────────

- (void)setEQBand:(int)band gain:(float)gainDb {
    if (_impl) _impl->set_eq(band, gainDb);
}

// ── Compressor ──────────────────────────────────────────────────────────────

- (void)setCompressorThreshold:(float)thresh ratio:(float)ratio attack:(float)attackMs release:(float)releaseMs enabled:(BOOL)enabled {
    if (_impl) _impl->set_compressor(thresh, ratio, attackMs, releaseMs, enabled);
}

// ── Recording ───────────────────────────────────────────────────────────────

- (BOOL)startRecordingToFile:(NSString *)path {
    if (_isRecording) return NO;

    NSURL *url = [NSURL fileURLWithPath:path];
    uint32_t ch = _channels;

    // Client (input) format: 32-bit float interleaved
    AudioStreamBasicDescription clientFormat = {};
    clientFormat.mSampleRate = 48000;
    clientFormat.mFormatID = kAudioFormatLinearPCM;
    clientFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    clientFormat.mBitsPerChannel = 32;
    clientFormat.mChannelsPerFrame = ch;
    clientFormat.mFramesPerPacket = 1;
    clientFormat.mBytesPerFrame = ch * 4;
    clientFormat.mBytesPerPacket = ch * 4;

    // File format: 16-bit WAV
    AudioStreamBasicDescription fileFormat = {};
    fileFormat.mSampleRate = 48000;
    fileFormat.mFormatID = kAudioFormatLinearPCM;
    fileFormat.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fileFormat.mBitsPerChannel = 16;
    fileFormat.mChannelsPerFrame = ch;
    fileFormat.mFramesPerPacket = 1;
    fileFormat.mBytesPerFrame = ch * 2;
    fileFormat.mBytesPerPacket = ch * 2;

    OSStatus status = ExtAudioFileCreateWithURL(
        (__bridge CFURLRef)url,
        kAudioFileWAVEType,
        &fileFormat,
        nullptr,
        kAudioFileFlags_EraseFile,
        &_recordFile
    );
    if (status != noErr) return NO;

    status = ExtAudioFileSetProperty(
        _recordFile,
        kExtAudioFileProperty_ClientDataFormat,
        sizeof(clientFormat),
        &clientFormat
    );
    if (status != noErr) {
        ExtAudioFileDispose(_recordFile);
        _recordFile = nullptr;
        return NO;
    }

    _isRecording = YES;

    // Set record callback on impl
    ExtAudioFileRef file = _recordFile;
    _impl->set_record_callback([file, ch](const float* samples, uint32_t count) {
        AudioBufferList abl;
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = ch;
        abl.mBuffers[0].mDataByteSize = count * sizeof(float);
        abl.mBuffers[0].mData = (void*)samples;
        UInt32 frames = count / ch;
        ExtAudioFileWriteAsync(file, frames, &abl);
    });

    return YES;
}

- (void)stopRecording {
    if (!_isRecording) return;
    _isRecording = NO;

    if (_impl) {
        _impl->set_record_callback(nullptr);
    }

    if (_recordFile) {
        ExtAudioFileDispose(_recordFile);
        _recordFile = nullptr;
    }
}

- (BOOL)isRecording {
    return _isRecording;
}

// ── DJ Mode ─────────────────────────────────────────────────────────────────

- (BOOL)startDJBroadcast:(NSString *)filePath {
    if (_djImpl && _djImpl->is_running()) return NO;
    if (!_impl) return NO;

    // Ensure WAN relay is connected — DJ mode requires relay
    WanRelayClient* relay = _impl->wan_relay_.get();
    if (!relay || relay->state() != WanRelayClient::State::Connected) {
        fprintf(stderr, "[SolunaDJ] Cannot start: relay not connected\n");
        return NO;
    }

    _djImpl = std::make_unique<DJBroadcaster>(relay);
    if (!_djImpl->start(std::string(filePath.UTF8String))) {
        _djImpl.reset();
        return NO;
    }
    return YES;
}

- (void)stopDJ {
    if (_djImpl) {
        _djImpl->stop();
        _djImpl.reset();
    }
}

- (void)skipDJTrack {
    if (_djImpl) _djImpl->skip();
}

- (BOOL)isDJActive {
    return _djImpl && _djImpl->is_running();
}

- (NSString *)djCurrentTrack {
    if (!_djImpl) return nil;
    auto name = _djImpl->track_name();
    return name.empty() ? nil : @(name.c_str());
}

- (float)djProgress {
    return _djImpl ? _djImpl->progress() : 0.0f;
}

// ── DJ Mic Mix ──────────────────────────────────────────────────────────────

- (void)setDjMicMixEnabled:(BOOL)enabled {
    if (_djImpl) _djImpl->set_mic_mix(enabled);
}

- (BOOL)djMicMixEnabled {
    return _djImpl ? _djImpl->mic_mix_enabled_.load() : NO;
}

- (void)setDjMicGain:(float)gain {
    if (_djImpl) _djImpl->mic_gain_.store(gain, std::memory_order_relaxed);
}

- (float)djMicGain {
    return _djImpl ? _djImpl->mic_gain_.load(std::memory_order_relaxed) : 1.0f;
}

- (void)setDjMusicGain:(float)gain {
    if (_djImpl) _djImpl->music_gain_.store(gain, std::memory_order_relaxed);
}

- (float)djMusicGain {
    return _djImpl ? _djImpl->music_gain_.load(std::memory_order_relaxed) : 0.7f;
}

@end
