#pragma once

/**
 * SolunaVstReceiver — Format-agnostic Soluna network audio receiver
 *
 * Receives RTP audio (OSTP or AES67) from multicast or unicast UDP,
 * converts S24/L24/L16 payloads to float, and stores in a lock-free
 * ring buffer for DAW audio callback consumption.
 *
 * This class is intentionally self-contained (no soluna_core dependency)
 * so the VST3/AU plugin can be distributed as a single binary.
 *
 * SPDX-License-Identifier: MIT
 */

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace soluna::vst {

// ── Constants ───────────────────────────────────────────────────────────────

constexpr const char* kDefaultMulticast = "239.69.0.1";
constexpr uint16_t    kDefaultPort      = 5004;
constexpr uint32_t    kDefaultSampleRate = 48000;
constexpr uint32_t    kDefaultChannels  = 2;
constexpr size_t      kMaxUdpPacket     = 16384;

// ── Parameter IDs ───────────────────────────────────────────────────────────

enum ParamId : uint32_t {
    kParamVolume     = 0,  // 0.0 .. 1.0 (linear gain)
    kParamMute       = 1,  // 0.0 = off, 1.0 = muted
    kParamBufferMs   = 2,  // 10 .. 500 ms (ring buffer target fill)
    kParamCount      = 3,
};

// ── Connection status ───────────────────────────────────────────────────────

enum class ConnStatus : uint32_t {
    Disconnected = 0,
    Listening    = 1,
    Receiving    = 2,
};

// ── Lock-free SPSC float ring buffer ────────────────────────────────────────

class FloatRing {
public:
    explicit FloatRing(size_t capacity_samples);
    ~FloatRing() = default;

    FloatRing(const FloatRing&) = delete;
    FloatRing& operator=(const FloatRing&) = delete;

    /** Write interleaved float samples. Returns count actually written. */
    size_t write(const float* data, size_t count);

    /** Read interleaved float samples. Returns count actually read. */
    size_t read(float* data, size_t count);

    /** Samples available for reading. */
    size_t available() const;

    /** Reset pointers (only safe with no concurrent access). */
    void reset();

    size_t capacity() const { return capacity_; }

private:
    size_t next_pow2(size_t v);

    const size_t capacity_;
    const size_t mask_;
    std::unique_ptr<float[]> buf_;

    alignas(64) std::atomic<size_t> write_pos_{0};
    alignas(64) std::atomic<size_t> read_pos_{0};
};

// ── SolunaVstReceiver ───────────────────────────────────────────────────────

class SolunaVstReceiver {
public:
    struct Config {
        std::string multicast_group = kDefaultMulticast;
        uint16_t    port            = kDefaultPort;
        uint32_t    channels        = kDefaultChannels;
        uint32_t    sample_rate     = kDefaultSampleRate;
        bool        unicast         = false;   // true = P2P mode (no multicast join)
    };

    SolunaVstReceiver();
    ~SolunaVstReceiver();

    SolunaVstReceiver(const SolunaVstReceiver&) = delete;
    SolunaVstReceiver& operator=(const SolunaVstReceiver&) = delete;

    // ── Lifecycle ───────────────────────────────────────────────────────

    /** Start the receive thread. Returns true on success. */
    bool start(const Config& cfg);

    /** Stop the receive thread and release the socket. */
    void stop();

    /** True if the receive thread is running. */
    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    // ── Audio callback interface ────────────────────────────────────────

    /**
     * Pull audio from the ring buffer into DAW output buffers.
     * Called from the DAW's real-time audio thread.
     *
     * @param out_left   Left channel output (or mono)
     * @param out_right  Right channel output (nullptr for mono)
     * @param frames     Number of frames requested
     */
    void process(float* out_left, float* out_right, uint32_t frames);

    // ── Parameters ──────────────────────────────────────────────────────

    float    get_param(ParamId id) const;
    void     set_param(ParamId id, float value);

    float    volume() const    { return volume_.load(std::memory_order_relaxed); }
    bool     muted() const     { return mute_.load(std::memory_order_relaxed) > 0.5f; }
    float    buffer_ms() const { return buffer_ms_.load(std::memory_order_relaxed); }

    // ── Status ──────────────────────────────────────────────────────────

    ConnStatus status() const { return status_.load(std::memory_order_relaxed); }

    uint64_t packets_received() const { return packets_received_.load(std::memory_order_relaxed); }
    uint64_t underruns() const        { return underruns_.load(std::memory_order_relaxed); }

    /** Ring buffer fill level, 0.0 .. 1.0. */
    float    fill_level() const;

private:
    void receive_thread();
    void process_rtp_packet(const uint8_t* data, size_t len);

    // Convert S24 (32-bit container, little-endian) to float [-1, +1].
    static float s24_to_float(int32_t s24);

    // Convert L24 (packed 3-byte big-endian) to float.
    static float l24_to_float(const uint8_t* p);

    // Convert L16 (big-endian) to float.
    static float l16_to_float(const uint8_t* p);

    Config config_;
    int    socket_fd_ = -1;

    std::unique_ptr<FloatRing> ring_;
    std::unique_ptr<std::thread> thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_flag_{false};

    // Parameters (atomic for lock-free access from audio thread)
    std::atomic<float> volume_{1.0f};
    std::atomic<float> mute_{0.0f};
    std::atomic<float> buffer_ms_{50.0f};

    // Status
    std::atomic<ConnStatus> status_{ConnStatus::Disconnected};
    std::atomic<uint64_t>   packets_received_{0};
    std::atomic<uint64_t>   underruns_{0};

    // Prefill tracking
    std::atomic<bool> prefilled_{false};

    // Scratch buffers (allocated once in start())
    // rx_scratch_: used by receive thread for payload-to-float conversion
    // process_scratch_: used by audio thread for ring read + deinterleave
    std::unique_ptr<float[]> rx_scratch_;
    std::unique_ptr<float[]> process_scratch_;
};

} // namespace soluna::vst
