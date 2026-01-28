/**
 * Memory Benchmarks for Soluna
 *
 * Measures:
 * - Ring buffer memory allocation and usage
 * - Jitter buffer memory footprint
 * - FEC encoder/decoder memory usage
 * - Pipeline memory overhead
 *
 * SPDX-License-Identifier: MIT
 */

#include <benchmark/benchmark.h>
#include <soluna/pipeline/ring_buffer.h>
#include <soluna/wifi/fec.h>
#include <soluna/wifi/jitter_buffer.h>

#include <cstring>
#include <memory>
#include <random>
#include <vector>

using namespace soluna;

// -----------------------------------------------------------------------------
// Ring Buffer Memory
// -----------------------------------------------------------------------------

static void BM_RingBuffer_Allocation(benchmark::State& state) {
    const size_t capacity = static_cast<size_t>(state.range(0));
    const size_t frame_size = 8;  // stereo float32

    for (auto _ : state) {
        auto rb = std::make_unique<pipeline::RingBuffer>(capacity, frame_size);
        benchmark::DoNotOptimize(rb->capacity());
    }

    // Report memory usage
    state.SetLabel(std::to_string(capacity * frame_size / 1024) + "KB");
}
BENCHMARK(BM_RingBuffer_Allocation)
    ->Arg(256)
    ->Arg(512)
    ->Arg(1024)
    ->Arg(2048)
    ->Arg(4096)
    ->Arg(8192)
    ->Unit(benchmark::kMicrosecond);

static void BM_RingBuffer_PowerOfTwo_Rounding(benchmark::State& state) {
    // Test that odd sizes are rounded correctly
    const size_t requested = static_cast<size_t>(state.range(0));
    const size_t frame_size = 8;

    for (auto _ : state) {
        auto rb = std::make_unique<pipeline::RingBuffer>(requested, frame_size);
        benchmark::DoNotOptimize(rb->capacity());
    }

    // Verify power-of-two behavior
    pipeline::RingBuffer rb(requested, frame_size);
    state.SetLabel("req=" + std::to_string(requested) + " actual=" + std::to_string(rb.capacity()));
}
BENCHMARK(BM_RingBuffer_PowerOfTwo_Rounding)
    ->Arg(100)
    ->Arg(200)
    ->Arg(500)
    ->Arg(1000)
    ->Arg(1500)
    ->Unit(benchmark::kMicrosecond);

// -----------------------------------------------------------------------------
// Jitter Buffer Memory
// -----------------------------------------------------------------------------

static void BM_JitterBuffer_Allocation(benchmark::State& state) {
    wifi::JitterBufferConfig config;
    config.sample_rate = 48000;
    config.channels = static_cast<uint32_t>(state.range(0));
    config.initial_depth_ms = 4.0;
    config.max_depth_ms = 20.0;

    for (auto _ : state) {
        auto jb = std::make_unique<wifi::JitterBuffer>(config);
        benchmark::DoNotOptimize(jb->stats().current_depth_ms);
    }

    state.SetLabel(std::to_string(config.channels) + "ch");
}
BENCHMARK(BM_JitterBuffer_Allocation)
    ->Arg(1)
    ->Arg(2)
    ->Arg(8)
    ->Arg(32)
    ->Arg(64)
    ->Unit(benchmark::kMicrosecond);

static void BM_JitterBuffer_FillMemory(benchmark::State& state) {
    const size_t num_packets = static_cast<size_t>(state.range(0));
    const size_t packet_size = 384;  // 1ms stereo @ 48kHz

    wifi::JitterBufferConfig config;
    config.sample_rate = 48000;
    config.channels = 2;
    config.max_depth_ms = 100.0;  // Allow many packets

    wifi::JitterBuffer jb(config);
    std::vector<uint8_t> packet(packet_size);

    uint16_t seq = 0;
    int64_t ts_ns = 0;

    for (auto _ : state) {
        state.PauseTiming();
        jb.reset();
        seq = 0;
        ts_ns = 0;
        state.ResumeTiming();

        // Fill the buffer
        for (size_t i = 0; i < num_packets; ++i) {
            jb.push(seq++, ts_ns, packet.data(), packet_size);
            ts_ns += 1000000;  // 1ms
        }
        benchmark::DoNotOptimize(jb.stats().buffer_occupancy);
    }

    state.SetLabel(std::to_string(num_packets * packet_size / 1024) + "KB");
}
BENCHMARK(BM_JitterBuffer_FillMemory)
    ->Arg(10)
    ->Arg(50)
    ->Arg(100)
    ->Arg(200)
    ->Unit(benchmark::kMicrosecond);

// -----------------------------------------------------------------------------
// FEC Memory Usage
// -----------------------------------------------------------------------------

static void BM_FEC_EncoderAllocation(benchmark::State& state) {
    wifi::FecConfig config;
    config.mode = wifi::FecMode::XorParity;
    config.group_size = static_cast<uint8_t>(state.range(0));
    config.max_packet_size = 1400;

    for (auto _ : state) {
        auto encoder = std::make_unique<wifi::FecEncoder>(config);
        auto gs = encoder->config().group_size;
        benchmark::DoNotOptimize(gs);
    }

    state.SetLabel("group=" + std::to_string(config.group_size));
}
BENCHMARK(BM_FEC_EncoderAllocation)
    ->Arg(3)
    ->Arg(5)
    ->Arg(8)
    ->Arg(10)
    ->Arg(16)
    ->Unit(benchmark::kMicrosecond);

static void BM_FEC_DecoderAllocation(benchmark::State& state) {
    wifi::FecConfig config;
    config.mode = wifi::FecMode::XorParity;
    config.group_size = static_cast<uint8_t>(state.range(0));
    config.max_packet_size = 1400;

    for (auto _ : state) {
        auto decoder = std::make_unique<wifi::FecDecoder>(config);
        auto gs = decoder->config().group_size;
        benchmark::DoNotOptimize(gs);
    }

    state.SetLabel("group=" + std::to_string(config.group_size));
}
BENCHMARK(BM_FEC_DecoderAllocation)
    ->Arg(3)
    ->Arg(5)
    ->Arg(8)
    ->Arg(10)
    ->Arg(16)
    ->Unit(benchmark::kMicrosecond);

static void BM_FEC_DecoderGroupMemory(benchmark::State& state) {
    const size_t num_groups = static_cast<size_t>(state.range(0));
    const size_t packet_size = 1400;

    wifi::FecConfig config;
    config.mode = wifi::FecMode::XorParity;
    config.group_size = 5;
    config.max_packet_size = packet_size;

    wifi::FecDecoder decoder(config);
    std::vector<uint8_t> packet(packet_size);

    for (auto _ : state) {
        state.PauseTiming();
        decoder.reset();
        state.ResumeTiming();

        // Accumulate groups (simulating high latency situation)
        for (uint32_t g = 0; g < num_groups; ++g) {
            for (uint8_t i = 0; i < config.group_size; ++i) {
                decoder.feed(g, i, false, packet.data(), packet_size);
            }
        }
        benchmark::DoNotOptimize(decoder.is_complete(0));
    }

    size_t mem_kb = num_groups * config.group_size * packet_size / 1024;
    state.SetLabel(std::to_string(num_groups) + " groups ~" + std::to_string(mem_kb) + "KB");
}
BENCHMARK(BM_FEC_DecoderGroupMemory)
    ->Arg(4)
    ->Arg(8)
    ->Arg(16)
    ->Arg(32)
    ->Arg(64)
    ->Unit(benchmark::kMicrosecond);

// -----------------------------------------------------------------------------
// Multi-Buffer Memory Patterns
// -----------------------------------------------------------------------------

static void BM_MultiBuffer_Allocation(benchmark::State& state) {
    const size_t num_buffers = static_cast<size_t>(state.range(0));
    const size_t buffer_capacity = 1024;
    const size_t frame_size = 8;  // stereo float

    for (auto _ : state) {
        std::vector<std::unique_ptr<pipeline::RingBuffer>> buffers;
        buffers.reserve(num_buffers);
        for (size_t i = 0; i < num_buffers; ++i) {
            buffers.push_back(std::make_unique<pipeline::RingBuffer>(buffer_capacity, frame_size));
        }
        benchmark::DoNotOptimize(buffers.size());
    }

    size_t total_kb = num_buffers * buffer_capacity * frame_size / 1024;
    state.SetLabel(std::to_string(num_buffers) + " bufs = " + std::to_string(total_kb) + "KB");
}
BENCHMARK(BM_MultiBuffer_Allocation)
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->Arg(16)
    ->Arg(32)
    ->Arg(64)
    ->Unit(benchmark::kMicrosecond);

// -----------------------------------------------------------------------------
// Memory Access Patterns
// -----------------------------------------------------------------------------

static void BM_SequentialWrite(benchmark::State& state) {
    const size_t buffer_size = static_cast<size_t>(state.range(0));
    std::vector<uint8_t> buffer(buffer_size);
    std::vector<uint8_t> data(64);

    for (auto _ : state) {
        for (size_t i = 0; i < buffer_size; i += 64) {
            std::memcpy(buffer.data() + i, data.data(), 64);
        }
        benchmark::DoNotOptimize(buffer[0]);
    }

    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(buffer_size));
}
BENCHMARK(BM_SequentialWrite)
    ->Arg(4096)
    ->Arg(16384)
    ->Arg(65536)
    ->Arg(262144)
    ->Arg(1048576)
    ->Unit(benchmark::kMicrosecond);

static void BM_RandomWrite(benchmark::State& state) {
    const size_t buffer_size = static_cast<size_t>(state.range(0));
    const size_t num_writes = buffer_size / 64;

    std::vector<uint8_t> buffer(buffer_size);
    std::vector<uint8_t> data(64);

    // Pre-compute random indices
    std::vector<size_t> indices(num_writes);
    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> dist(0, buffer_size - 64);
    for (auto& idx : indices) {
        idx = dist(rng);
    }

    for (auto _ : state) {
        for (size_t idx : indices) {
            std::memcpy(buffer.data() + idx, data.data(), 64);
        }
        benchmark::DoNotOptimize(buffer[0]);
    }

    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(num_writes * 64));
}
BENCHMARK(BM_RandomWrite)
    ->Arg(4096)
    ->Arg(16384)
    ->Arg(65536)
    ->Arg(262144)
    ->Arg(1048576)
    ->Unit(benchmark::kMicrosecond);

// -----------------------------------------------------------------------------
// Cache Line Efficiency
// -----------------------------------------------------------------------------

static void BM_CacheLineAccess_Aligned(benchmark::State& state) {
    const size_t num_elements = static_cast<size_t>(state.range(0));

    // Aligned to cache lines (64 bytes)
    struct alignas(64) CacheAligned {
        std::atomic<size_t> value{0};
        char padding[56];
    };

    std::vector<CacheAligned> elements(num_elements);

    for (auto _ : state) {
        for (auto& elem : elements) {
            elem.value.store(42, std::memory_order_relaxed);
        }
        benchmark::DoNotOptimize(elements[0].value.load());
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(num_elements));
}
BENCHMARK(BM_CacheLineAccess_Aligned)
    ->Arg(16)
    ->Arg(64)
    ->Arg(256)
    ->Arg(1024)
    ->Unit(benchmark::kMicrosecond);

static void BM_CacheLineAccess_Packed(benchmark::State& state) {
    const size_t num_elements = static_cast<size_t>(state.range(0));

    // Packed (potential false sharing)
    std::vector<std::atomic<size_t>> elements(num_elements);

    for (auto _ : state) {
        for (auto& elem : elements) {
            elem.store(42, std::memory_order_relaxed);
        }
        benchmark::DoNotOptimize(elements[0].load());
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(num_elements));
}
BENCHMARK(BM_CacheLineAccess_Packed)
    ->Arg(16)
    ->Arg(64)
    ->Arg(256)
    ->Arg(1024)
    ->Unit(benchmark::kMicrosecond);
