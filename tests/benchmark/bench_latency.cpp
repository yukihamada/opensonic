/**
 * Latency Benchmarks for Soluna
 *
 * Measures:
 * - Ring buffer write/read latency
 * - PTP timestamp operations
 * - Clock servo computation latency
 * - End-to-end pipeline latency simulation
 *
 * SPDX-License-Identifier: MIT
 */

#include <benchmark/benchmark.h>
#include <soluna/pipeline/ring_buffer.h>
#include <soluna/sync/ptp.h>
#include <soluna/pal/time.h>

#include <cstring>
#include <random>
#include <vector>

using namespace soluna;

// -----------------------------------------------------------------------------
// Ring Buffer Latency
// -----------------------------------------------------------------------------

static void BM_RingBuffer_WriteSingle(benchmark::State& state) {
    const size_t frame_size = static_cast<size_t>(state.range(0));
    pipeline::RingBuffer rb(1024, frame_size);
    std::vector<uint8_t> data(frame_size);

    for (auto _ : state) {
        rb.write(data.data(), 1);
        benchmark::DoNotOptimize(rb.available_read());
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(frame_size));
}
BENCHMARK(BM_RingBuffer_WriteSingle)
    ->Arg(4)      // mono float32
    ->Arg(8)      // stereo float32
    ->Arg(12)     // stereo 24-bit
    ->Arg(192)    // 48 channels float32
    ->Unit(benchmark::kNanosecond);

static void BM_RingBuffer_ReadSingle(benchmark::State& state) {
    const size_t frame_size = static_cast<size_t>(state.range(0));
    pipeline::RingBuffer rb(1024, frame_size);
    std::vector<uint8_t> data(frame_size);

    // Pre-fill the buffer
    for (size_t i = 0; i < 512; ++i) {
        rb.write(data.data(), 1);
    }

    for (auto _ : state) {
        rb.read(data.data(), 1);
        // Refill to maintain steady state
        rb.write(data.data(), 1);
        benchmark::DoNotOptimize(data[0]);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(frame_size));
}
BENCHMARK(BM_RingBuffer_ReadSingle)
    ->Arg(4)
    ->Arg(8)
    ->Arg(12)
    ->Arg(192)
    ->Unit(benchmark::kNanosecond);

static void BM_RingBuffer_WriteBatch(benchmark::State& state) {
    const size_t frame_size = 8;  // stereo float32
    const size_t batch_size = static_cast<size_t>(state.range(0));
    pipeline::RingBuffer rb(4096, frame_size);
    std::vector<uint8_t> data(frame_size * batch_size);

    for (auto _ : state) {
        size_t written = rb.write(data.data(), batch_size);
        // Read back to prevent overflow
        rb.read(data.data(), written);
        benchmark::DoNotOptimize(written);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(batch_size));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(batch_size * frame_size));
}
BENCHMARK(BM_RingBuffer_WriteBatch)
    ->Arg(48)     // 1ms at 48kHz
    ->Arg(96)     // 2ms
    ->Arg(240)    // 5ms
    ->Arg(480)    // 10ms
    ->Unit(benchmark::kNanosecond);

// -----------------------------------------------------------------------------
// PTP Timestamp Operations
// -----------------------------------------------------------------------------

static void BM_PtpTimestamp_ToNs(benchmark::State& state) {
    sync::PtpTimestamp ts;
    ts.seconds_msb = 0;
    ts.seconds_lsb = 1234567890;
    ts.nanoseconds = 123456789;

    for (auto _ : state) {
        auto ns = ts.to_ns();
        benchmark::DoNotOptimize(ns);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PtpTimestamp_ToNs)->Unit(benchmark::kNanosecond);

static void BM_PtpTimestamp_FromNs(benchmark::State& state) {
    int64_t ns = 1234567890123456789LL;

    for (auto _ : state) {
        auto ts = sync::PtpTimestamp::from_ns(ns);
        benchmark::DoNotOptimize(ts.seconds_lsb);
        ns++;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PtpTimestamp_FromNs)->Unit(benchmark::kNanosecond);

static void BM_PtpSerialize_Sync(benchmark::State& state) {
    uint8_t buf[128];
    sync::PtpHeader hdr;
    hdr.message_type = sync::PtpMessageType::Sync;
    hdr.sequence_id = 1;
    sync::PtpTimestamp ts = sync::PtpTimestamp::from_ns(1234567890000000000LL);

    for (auto _ : state) {
        auto len = sync::ptp_serialize_sync(buf, sizeof(buf), hdr, ts);
        benchmark::DoNotOptimize(len);
        hdr.sequence_id++;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PtpSerialize_Sync)->Unit(benchmark::kNanosecond);

static void BM_PtpParse_Header(benchmark::State& state) {
    uint8_t buf[128];
    sync::PtpHeader hdr_out;
    sync::PtpHeader hdr_in;
    hdr_in.message_type = sync::PtpMessageType::Sync;
    hdr_in.sequence_id = 42;
    sync::PtpTimestamp ts = sync::PtpTimestamp::from_ns(1234567890000000000LL);

    // Pre-serialize
    auto len = sync::ptp_serialize_sync(buf, sizeof(buf), hdr_in, ts);

    for (auto _ : state) {
        bool ok = sync::ptp_parse_header(buf, len, hdr_out);
        benchmark::DoNotOptimize(ok);
        benchmark::DoNotOptimize(hdr_out.sequence_id);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PtpParse_Header)->Unit(benchmark::kNanosecond);

// -----------------------------------------------------------------------------
// Clock Operations
// -----------------------------------------------------------------------------

static void BM_Clock_MonotonicNow(benchmark::State& state) {
    auto& clock = pal::Clock::instance();

    for (auto _ : state) {
        auto ts = clock.monotonic_now();
        benchmark::DoNotOptimize(ts.nanoseconds);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Clock_MonotonicNow)->Unit(benchmark::kNanosecond);

static void BM_Clock_RealtimeNow(benchmark::State& state) {
    auto& clock = pal::Clock::instance();

    for (auto _ : state) {
        auto ts = clock.realtime_now();
        benchmark::DoNotOptimize(ts.nanoseconds);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Clock_RealtimeNow)->Unit(benchmark::kNanosecond);

// -----------------------------------------------------------------------------
// End-to-End Pipeline Simulation
// -----------------------------------------------------------------------------

static void BM_Pipeline_Roundtrip(benchmark::State& state) {
    const size_t frame_size = 8;      // stereo float32
    const size_t frames_per_packet = 48;  // 1ms at 48kHz
    const size_t packet_size = frame_size * frames_per_packet;

    pipeline::RingBuffer tx_buffer(1024, frame_size);
    pipeline::RingBuffer rx_buffer(1024, frame_size);

    std::vector<uint8_t> audio_in(packet_size);
    std::vector<uint8_t> audio_out(packet_size);
    std::vector<uint8_t> network_packet(packet_size + 16);  // + header overhead

    // Simulate random audio data
    std::mt19937 rng(42);
    for (auto& b : audio_in) {
        b = static_cast<uint8_t>(rng());
    }

    for (auto _ : state) {
        // TX path: audio -> ring buffer -> network packet
        tx_buffer.write(audio_in.data(), frames_per_packet);
        tx_buffer.read(network_packet.data() + 16, frames_per_packet);

        // RX path: network packet -> ring buffer -> audio
        rx_buffer.write(network_packet.data() + 16, frames_per_packet);
        rx_buffer.read(audio_out.data(), frames_per_packet);

        benchmark::DoNotOptimize(audio_out[0]);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(packet_size * 2));
}
BENCHMARK(BM_Pipeline_Roundtrip)->Unit(benchmark::kMicrosecond);

// Measure theoretical minimum latency (memory copies only)
static void BM_Pipeline_MemcpyBaseline(benchmark::State& state) {
    const size_t packet_size = state.range(0);

    std::vector<uint8_t> src(packet_size);
    std::vector<uint8_t> dst(packet_size);

    for (auto _ : state) {
        std::memcpy(dst.data(), src.data(), packet_size);
        benchmark::DoNotOptimize(dst[0]);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(packet_size));
}
BENCHMARK(BM_Pipeline_MemcpyBaseline)
    ->Arg(384)    // 1ms stereo @ 48kHz
    ->Arg(768)    // 2ms
    ->Arg(1920)   // 5ms
    ->Unit(benchmark::kNanosecond);
