/**
 * SolunaVstReceiver — Implementation
 *
 * Self-contained network audio receiver for VST3/AU plugins.
 * Receives OSTP or AES67 RTP packets, converts to float, writes to ring buffer.
 *
 * SPDX-License-Identifier: MIT
 */

#include "soluna_plugin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

// POSIX sockets (macOS, Linux)
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using ssize_t = int;
#endif

namespace soluna::vst {

// ── RTP Header (RFC 3550) — 12 bytes ────────────────────────────────────────

#pragma pack(push, 1)
struct RtpHdr {
    uint8_t  cc : 4;
    uint8_t  extension : 1;
    uint8_t  padding : 1;
    uint8_t  version : 2;
    uint8_t  pt : 7;
    uint8_t  marker : 1;
    uint16_t sequence;
    uint32_t timestamp;
    uint32_t ssrc;
};
#pragma pack(pop)

static_assert(sizeof(RtpHdr) == 12, "RTP header must be 12 bytes");

// OSTP extension header — 8 bytes
#pragma pack(push, 1)
struct OstpExt {
    uint16_t profile;    // 0x4F53 = "OS"
    uint16_t length;     // 2 (32-bit words)
    uint16_t stream_id;
    uint16_t seq_ext;
    uint32_t media_ts;
};
#pragma pack(pop)

static_assert(sizeof(OstpExt) == 12, "OSTP ext must be 12 bytes (4 RTP ext hdr + 8 OSTP)");

// Payload type constants
constexpr uint8_t kPT_PCM24 = 96;  // OSTP dynamic: S24_LE in 32-bit container
constexpr uint8_t kPT_F32   = 97;  // OSTP dynamic: IEEE float
constexpr uint8_t kPT_L24   = 10;  // AES67: L24 packed big-endian
constexpr uint8_t kPT_L16   = 11;  // AES67: L16 big-endian
constexpr uint16_t kOSTP_PROFILE = 0x4F53; // "OS"

// ═══════════════════════════════════════════════════════════════════════════
// FloatRing
// ═══════════════════════════════════════════════════════════════════════════

FloatRing::FloatRing(size_t capacity_samples)
    : capacity_(next_pow2(capacity_samples))
    , mask_(capacity_ - 1)
    , buf_(std::make_unique<float[]>(capacity_))
{
    std::memset(buf_.get(), 0, capacity_ * sizeof(float));
}

size_t FloatRing::next_pow2(size_t v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    v++;
    return (v < 64) ? 64 : v;
}

size_t FloatRing::write(const float* data, size_t count) {
    size_t wp = write_pos_.load(std::memory_order_relaxed);
    size_t rp = read_pos_.load(std::memory_order_acquire);
    size_t space = capacity_ - (wp - rp);
    size_t to_write = std::min(count, space);

    for (size_t i = 0; i < to_write; ++i) {
        buf_[(wp + i) & mask_] = data[i];
    }

    write_pos_.store(wp + to_write, std::memory_order_release);
    return to_write;
}

size_t FloatRing::read(float* data, size_t count) {
    size_t wp = write_pos_.load(std::memory_order_acquire);
    size_t rp = read_pos_.load(std::memory_order_relaxed);
    size_t avail = wp - rp;
    size_t to_read = std::min(count, avail);

    for (size_t i = 0; i < to_read; ++i) {
        data[i] = buf_[(rp + i) & mask_];
    }

    read_pos_.store(rp + to_read, std::memory_order_release);
    return to_read;
}

size_t FloatRing::available() const {
    size_t wp = write_pos_.load(std::memory_order_acquire);
    size_t rp = read_pos_.load(std::memory_order_relaxed);
    return wp - rp;
}

void FloatRing::reset() {
    write_pos_.store(0, std::memory_order_relaxed);
    read_pos_.store(0, std::memory_order_relaxed);
}

// ═══════════════════════════════════════════════════════════════════════════
// SolunaVstReceiver
// ═══════════════════════════════════════════════════════════════════════════

SolunaVstReceiver::SolunaVstReceiver() = default;

SolunaVstReceiver::~SolunaVstReceiver() {
    stop();
}

// ── Conversion helpers ──────────────────────────────────────────────────────

float SolunaVstReceiver::s24_to_float(int32_t s24) {
    // S24_LE stored in 32-bit container: sign-extend from 24 bits
    if (s24 & 0x00800000) s24 |= 0xFF000000;
    else                  s24 &= 0x00FFFFFF;
    return static_cast<float>(s24) / 8388608.0f;  // 2^23
}

float SolunaVstReceiver::l24_to_float(const uint8_t* p) {
    // L24: packed 3-byte big-endian → sign-extended int
    int32_t v = (static_cast<int32_t>(p[0]) << 16)
              | (static_cast<int32_t>(p[1]) << 8)
              |  static_cast<int32_t>(p[2]);
    if (v & 0x800000) v |= 0xFF000000;  // sign-extend
    return static_cast<float>(v) / 8388608.0f;
}

float SolunaVstReceiver::l16_to_float(const uint8_t* p) {
    // L16: 2-byte big-endian signed
    int16_t v = static_cast<int16_t>((p[0] << 8) | p[1]);
    return static_cast<float>(v) / 32768.0f;  // 2^15
}

// ── Parameter access ────────────────────────────────────────────────────────

float SolunaVstReceiver::get_param(ParamId id) const {
    switch (id) {
        case kParamVolume:   return volume_.load(std::memory_order_relaxed);
        case kParamMute:     return mute_.load(std::memory_order_relaxed);
        case kParamBufferMs: return buffer_ms_.load(std::memory_order_relaxed);
        default:             return 0.0f;
    }
}

void SolunaVstReceiver::set_param(ParamId id, float value) {
    switch (id) {
        case kParamVolume:
            volume_.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
        case kParamMute:
            mute_.store(value >= 0.5f ? 1.0f : 0.0f, std::memory_order_relaxed);
            break;
        case kParamBufferMs:
            buffer_ms_.store(std::clamp(value, 10.0f, 500.0f), std::memory_order_relaxed);
            break;
        default:
            break;
    }
}

float SolunaVstReceiver::fill_level() const {
    if (!ring_) return 0.0f;
    return static_cast<float>(ring_->available()) /
           static_cast<float>(ring_->capacity());
}

// ── Start / Stop ────────────────────────────────────────────────────────────

bool SolunaVstReceiver::start(const Config& cfg) {
    if (running_.load()) stop();

    config_ = cfg;

    // Allocate ring buffer: enough for ~1 second of audio at given sample rate
    // The user-facing "buffer_ms" parameter controls prefill, not ring size.
    size_t ring_samples = cfg.sample_rate * cfg.channels * 2;  // 2 seconds
    ring_ = std::make_unique<FloatRing>(ring_samples);

    // Scratch buffers: one for the receive thread, one for the audio thread.
    // Max DAW buffer size is typically 8192 frames; RTP packets are much smaller.
    rx_scratch_ = std::make_unique<float[]>(8192 * cfg.channels);
    process_scratch_ = std::make_unique<float[]>(8192 * cfg.channels);

    // Create UDP socket
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    socket_fd_ = static_cast<int>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
#else
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#endif
    if (socket_fd_ < 0) {
        fprintf(stderr, "[SolunaVST] socket() failed\n");
        return false;
    }

    // Allow address reuse (multiple receivers on same port)
    int reuse = 1;
    setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEPORT,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#endif

    // Bind
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg.port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(socket_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        fprintf(stderr, "[SolunaVST] bind(%d) failed: %s\n",
                cfg.port, strerror(errno));
#ifdef _WIN32
        closesocket(socket_fd_);
#else
        close(socket_fd_);
#endif
        socket_fd_ = -1;
        return false;
    }

    // Join multicast group (unless unicast/P2P mode)
    if (!cfg.unicast) {
        struct ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = inet_addr(cfg.multicast_group.c_str());
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);

        if (setsockopt(socket_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       reinterpret_cast<const char*>(&mreq), sizeof(mreq)) < 0) {
            fprintf(stderr, "[SolunaVST] IP_ADD_MEMBERSHIP(%s) failed: %s\n",
                    cfg.multicast_group.c_str(), strerror(errno));
            // Continue anyway — might work for unicast fallback
        }
    }

    // Set non-blocking (we use select with timeout in the thread)
#ifndef _WIN32
    int flags = fcntl(socket_fd_, F_GETFL, 0);
    fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);
#else
    u_long mode = 1;
    ioctlsocket(socket_fd_, FIONBIO, &mode);
#endif

    // Start receive thread
    stop_flag_.store(false, std::memory_order_relaxed);
    prefilled_.store(false, std::memory_order_relaxed);
    packets_received_.store(0, std::memory_order_relaxed);
    underruns_.store(0, std::memory_order_relaxed);
    status_.store(ConnStatus::Listening, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);

    thread_ = std::make_unique<std::thread>(&SolunaVstReceiver::receive_thread, this);

    fprintf(stderr, "[SolunaVST] started: %s:%u ch=%u rate=%u %s\n",
            cfg.multicast_group.c_str(), cfg.port,
            cfg.channels, cfg.sample_rate,
            cfg.unicast ? "(unicast)" : "(multicast)");

    return true;
}

void SolunaVstReceiver::stop() {
    if (!running_.load()) return;

    stop_flag_.store(true, std::memory_order_relaxed);

    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
    thread_.reset();

    if (socket_fd_ >= 0) {
        // Leave multicast
        if (!config_.unicast) {
            struct ip_mreq mreq{};
            mreq.imr_multiaddr.s_addr = inet_addr(config_.multicast_group.c_str());
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            setsockopt(socket_fd_, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                       reinterpret_cast<const char*>(&mreq), sizeof(mreq));
        }
#ifdef _WIN32
        closesocket(socket_fd_);
#else
        close(socket_fd_);
#endif
        socket_fd_ = -1;
    }

    running_.store(false, std::memory_order_relaxed);
    status_.store(ConnStatus::Disconnected, std::memory_order_relaxed);

    fprintf(stderr, "[SolunaVST] stopped (pkts=%llu underruns=%llu)\n",
            static_cast<unsigned long long>(packets_received_.load()),
            static_cast<unsigned long long>(underruns_.load()));
}

// ── Receive thread ──────────────────────────────────────────────────────────

void SolunaVstReceiver::receive_thread() {
    uint8_t pkt[kMaxUdpPacket];

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        // select() with 10ms timeout to allow clean shutdown
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(socket_fd_, &fds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10000;  // 10ms

        int ready = select(socket_fd_ + 1, &fds, nullptr, nullptr, &tv);
        if (ready <= 0) continue;

        struct sockaddr_in src_addr{};
        socklen_t addr_len = sizeof(src_addr);

        ssize_t received = recvfrom(socket_fd_, reinterpret_cast<char*>(pkt), sizeof(pkt), 0,
                                     reinterpret_cast<struct sockaddr*>(&src_addr), &addr_len);

        if (received <= 0) continue;

        process_rtp_packet(pkt, static_cast<size_t>(received));
    }
}

void SolunaVstReceiver::process_rtp_packet(const uint8_t* data, size_t len) {
    if (len < sizeof(RtpHdr)) return;

    const RtpHdr* rtp = reinterpret_cast<const RtpHdr*>(data);
    if (rtp->version != 2) return;

    // Determine if OSTP or AES67 based on extension bit and payload type
    const uint8_t* payload = nullptr;
    size_t payload_size = 0;
    uint8_t pt = rtp->pt;
    bool is_ostp = false;

    if (rtp->extension) {
        // Check for OSTP extension header
        if (len >= sizeof(RtpHdr) + sizeof(OstpExt)) {
            const OstpExt* ext = reinterpret_cast<const OstpExt*>(data + sizeof(RtpHdr));
            uint16_t profile = ntohs(ext->profile);
            if (profile == kOSTP_PROFILE) {
                is_ostp = true;
                size_t hdr_size = sizeof(RtpHdr) + sizeof(OstpExt);
                payload = data + hdr_size;
                // OSTP packets have a 4-byte CRC trailer
                if (len > hdr_size + 4) {
                    payload_size = len - hdr_size - 4;
                } else if (len > hdr_size) {
                    payload_size = len - hdr_size;
                }
            }
        }
    }

    if (!is_ostp) {
        // Standard RTP (AES67 compatible)
        payload = data + sizeof(RtpHdr);
        payload_size = len - sizeof(RtpHdr);
    }

    if (!payload || payload_size == 0) return;

    // Convert payload to float based on payload type
    // rx_scratch_ is only accessed from this thread — no contention.
    const uint32_t ch = config_.channels;

    size_t out_samples = 0;
    float* out = rx_scratch_.get();
    const size_t max_out = 8192 * ch;

    if (is_ostp && (pt == kPT_PCM24 || pt >= 96)) {
        // OSTP S24_LE in 32-bit containers
        size_t sample_count = payload_size / 4;
        if (sample_count > max_out) sample_count = max_out;
        const int32_t* src = reinterpret_cast<const int32_t*>(payload);
        for (size_t i = 0; i < sample_count; ++i) {
            out[i] = s24_to_float(src[i]);
        }
        out_samples = sample_count;
    } else if (pt == kPT_F32) {
        // IEEE float 32-bit
        size_t sample_count = payload_size / 4;
        if (sample_count > max_out) sample_count = max_out;
        std::memcpy(out, payload, sample_count * sizeof(float));
        out_samples = sample_count;
    } else if (pt == kPT_L24) {
        // AES67 L24: packed 3-byte big-endian
        size_t sample_count = payload_size / 3;
        if (sample_count > max_out) sample_count = max_out;
        for (size_t i = 0; i < sample_count; ++i) {
            out[i] = l24_to_float(payload + i * 3);
        }
        out_samples = sample_count;
    } else if (pt == kPT_L16) {
        // AES67 L16: 2-byte big-endian
        size_t sample_count = payload_size / 2;
        if (sample_count > max_out) sample_count = max_out;
        for (size_t i = 0; i < sample_count; ++i) {
            out[i] = l16_to_float(payload + i * 2);
        }
        out_samples = sample_count;
    } else {
        // Unknown payload type — skip
        return;
    }

    // Write interleaved samples to ring buffer
    if (out_samples > 0 && ring_) {
        ring_->write(out, out_samples);
    }

    // Update status
    uint64_t prev = packets_received_.fetch_add(1, std::memory_order_relaxed);
    if (prev == 0) {
        status_.store(ConnStatus::Receiving, std::memory_order_relaxed);
    }
}

// ── Audio callback (real-time safe) ─────────────────────────────────────────

void SolunaVstReceiver::process(float* out_left, float* out_right, uint32_t frames) {
    const uint32_t ch = config_.channels;

    // Check prefill: wait until ring has buffer_ms worth of audio
    if (!prefilled_.load(std::memory_order_relaxed)) {
        float target_ms = buffer_ms_.load(std::memory_order_relaxed);
        size_t target_samples = static_cast<size_t>(
            target_ms * 0.001f * config_.sample_rate * ch);

        if (ring_ && ring_->available() >= target_samples) {
            prefilled_.store(true, std::memory_order_relaxed);
        } else {
            // Output silence while prefilling
            std::memset(out_left, 0, frames * sizeof(float));
            if (out_right) std::memset(out_right, 0, frames * sizeof(float));
            return;
        }
    }

    // Read interleaved samples from ring
    // process_scratch_ is only accessed from the audio thread — no contention.
    size_t need = static_cast<size_t>(frames) * ch;
    float* interleaved = process_scratch_.get();

    size_t got = 0;
    if (ring_) {
        got = ring_->read(interleaved, need);
    }

    // Underrun: output silence and reset prefill
    if (got < need) {
        // Zero-fill the remainder
        std::memset(interleaved + got, 0, (need - got) * sizeof(float));
        if (got == 0) {
            underruns_.fetch_add(1, std::memory_order_relaxed);
            // Reset prefill so we wait for buffer to fill again
            prefilled_.store(false, std::memory_order_relaxed);
        }
    }

    // Apply volume and mute
    float gain = muted() ? 0.0f : volume();

    // De-interleave into output channel buffers
    if (ch >= 2 && out_right) {
        for (uint32_t i = 0; i < frames; ++i) {
            out_left[i]  = interleaved[i * ch]     * gain;
            out_right[i] = interleaved[i * ch + 1] * gain;
        }
    } else {
        // Mono or single channel
        for (uint32_t i = 0; i < frames; ++i) {
            out_left[i] = interleaved[i * ch] * gain;
        }
        if (out_right) {
            // Duplicate mono to right channel
            std::memcpy(out_right, out_left, frames * sizeof(float));
        }
    }
}

} // namespace soluna::vst
