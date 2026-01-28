/**
 * Throughput Benchmarks for Soluna
 *
 * Measures:
 * - Audio pipeline throughput (samples/sec)
 * - FEC encode/decode throughput
 * - Packet processing throughput
 * - Format conversion throughput
 *
 * SPDX-License-Identifier: MIT
 */

#include <benchmark/benchmark.h>
#include <soluna/pipeline/ring_buffer.h>
#include <soluna/pipeline/pipeline.h>
#include <soluna/wifi/fec.h>

#include <cstring>
#include <random>
#include <vector>

using namespace soluna;

// -----------------------------------------------------------------------------
// Audio Pipeline Throughput
// -----------------------------------------------------------------------------

static void BM_RingBuffer_Throughput(benchmark::State& state) {
    const size_t channels = static_cast<size_t>(state.range(0));
    const size_t frame_size = channels * sizeof(float);
    const size_t frames_per_iteration = 480;  // 10ms at 48kHz

    pipeline::RingBuffer rb(4096, frame_size);
    std::vector<uint8_t> data(frame_size * frames_per_iteration);

    // Simulate audio data
    std::mt19937 rng(42);
    for (auto& b : data) {
        b = static_cast<uint8_t>(rng());
    }

    for (auto _ : state) {
        size_t written = rb.write(data.data(), frames_per_iteration);
        size_t read = rb.read(data.data(), frames_per_iteration);
        benchmark::DoNotOptimize(written);
        benchmark::DoNotOptimize(read);
    }

    // Calculate throughput in samples/second
    double samples_per_iter = static_cast<double>(frames_per_iteration * channels);
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(samples_per_iter));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(frame_size * frames_per_iteration));
}
BENCHMARK(BM_RingBuffer_Throughput)
    ->Arg(1)      // mono
    ->Arg(2)      // stereo
    ->Arg(8)      // 7.1 surround
    ->Arg(32)     // 32-channel
    ->Arg(64)     // 64-channel
    ->Unit(benchmark::kMicrosecond);

// -----------------------------------------------------------------------------
// Format Conversion Throughput
// -----------------------------------------------------------------------------

static void BM_FloatToS24_Throughput(benchmark::State& state) {
    const size_t sample_count = static_cast<size_t>(state.range(0));

    std::vector<float> src(sample_count);
    std::vector<int32_t> dst(sample_count);

    // Generate random audio data in [-1.0, 1.0]
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (auto& s : src) {
        s = dist(rng);
    }

    for (auto _ : state) {
        pipeline::float_to_s24(src.data(), dst.data(), sample_count);
        benchmark::DoNotOptimize(dst[0]);
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(sample_count));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(sample_count * sizeof(float)));
}
BENCHMARK(BM_FloatToS24_Throughput)
    ->Arg(48)       // 1ms mono @ 48kHz
    ->Arg(480)      // 10ms mono
    ->Arg(4800)     // 100ms mono
    ->Arg(96)       // 1ms stereo
    ->Arg(960)      // 10ms stereo
    ->Unit(benchmark::kMicrosecond);

static void BM_S24ToFloat_Throughput(benchmark::State& state) {
    const size_t sample_count = static_cast<size_t>(state.range(0));

    std::vector<int32_t> src(sample_count);
    std::vector<float> dst(sample_count);

    // Generate random 24-bit audio data
    std::mt19937 rng(42);
    std::uniform_int_distribution<int32_t> dist(-8388608, 8388607);
    for (auto& s : src) {
        s = dist(rng);
    }

    for (auto _ : state) {
        pipeline::s24_to_float(src.data(), dst.data(), sample_count);
        benchmark::DoNotOptimize(dst[0]);
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(sample_count));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(sample_count * sizeof(int32_t)));
}
BENCHMARK(BM_S24ToFloat_Throughput)
    ->Arg(48)
    ->Arg(480)
    ->Arg(4800)
    ->Arg(96)
    ->Arg(960)
    ->Unit(benchmark::kMicrosecond);

// -----------------------------------------------------------------------------
// FEC Encode/Decode Throughput
// -----------------------------------------------------------------------------

static void BM_FEC_XorEncode(benchmark::State& state) {
    const size_t packet_size = static_cast<size_t>(state.range(0));

    wifi::FecConfig config;
    config.mode = wifi::FecMode::XorParity;
    config.group_size = 5;
    config.max_packet_size = packet_size;

    wifi::FecEncoder encoder(config);

    std::vector<uint8_t> packet(packet_size);
    std::mt19937 rng(42);
    for (auto& b : packet) {
        b = static_cast<uint8_t>(rng());
    }

    for (auto _ : state) {
        // Feed group_size packets to generate parity
        for (uint8_t i = 0; i < config.group_size; ++i) {
            encoder.feed(packet.data(), packet_size);
        }
        benchmark::DoNotOptimize(encoder.get_parity().size());
    }

    // Packets processed per iteration
    state.SetItemsProcessed(state.iterations() * config.group_size);
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(packet_size * config.group_size));
}
BENCHMARK(BM_FEC_XorEncode)
    ->Arg(256)
    ->Arg(512)
    ->Arg(1024)
    ->Arg(1400)   // typical MTU payload
    ->Unit(benchmark::kMicrosecond);

static void BM_FEC_XorDecode_NoLoss(benchmark::State& state) {
    const size_t packet_size = static_cast<size_t>(state.range(0));

    wifi::FecConfig config;
    config.mode = wifi::FecMode::XorParity;
    config.group_size = 5;
    config.max_packet_size = packet_size;

    wifi::FecDecoder decoder(config);

    std::vector<uint8_t> packet(packet_size);
    std::mt19937 rng(42);
    for (auto& b : packet) {
        b = static_cast<uint8_t>(rng());
    }

    uint32_t group_id = 0;
    for (auto _ : state) {
        // Feed all data packets (no parity needed)
        for (uint8_t i = 0; i < config.group_size; ++i) {
            decoder.feed(group_id, i, false, packet.data(), packet_size);
        }
        bool complete = decoder.is_complete(group_id);
        benchmark::DoNotOptimize(complete);
        decoder.prune(1);
        group_id++;
    }

    state.SetItemsProcessed(state.iterations() * config.group_size);
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(packet_size * config.group_size));
}
BENCHMARK(BM_FEC_XorDecode_NoLoss)
    ->Arg(256)
    ->Arg(512)
    ->Arg(1024)
    ->Arg(1400)
    ->Unit(benchmark::kMicrosecond);

static void BM_FEC_XorDecode_WithRecovery(benchmark::State& state) {
    const size_t packet_size = static_cast<size_t>(state.range(0));

    wifi::FecConfig config;
    config.mode = wifi::FecMode::XorParity;
    config.group_size = 5;
    config.max_packet_size = packet_size;

    wifi::FecEncoder encoder(config);
    wifi::FecDecoder decoder(config);

    // Create test packets
    std::vector<std::vector<uint8_t>> packets(config.group_size);
    std::mt19937 rng(42);
    for (auto& pkt : packets) {
        pkt.resize(packet_size);
        for (auto& b : pkt) {
            b = static_cast<uint8_t>(rng());
        }
    }

    uint32_t group_id = 0;
    for (auto _ : state) {
        // Generate parity
        for (const auto& pkt : packets) {
            encoder.feed(pkt.data(), pkt.size());
        }
        const auto& parity = encoder.get_parity();

        // Simulate loss of packet 0, receive others + parity
        for (uint8_t i = 1; i < config.group_size; ++i) {
            decoder.feed(group_id, i, false, packets[i].data(), packets[i].size());
        }
        if (!parity.empty()) {
            decoder.feed(group_id, config.group_size, true,
                        parity[0].data.data(), parity[0].data.size());
        }

        auto recovered = decoder.recover(group_id);
        benchmark::DoNotOptimize(recovered.size());
        decoder.prune(1);
        group_id++;
    }

    state.SetItemsProcessed(state.iterations() * config.group_size);
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(packet_size * config.group_size));
}
BENCHMARK(BM_FEC_XorDecode_WithRecovery)
    ->Arg(256)
    ->Arg(512)
    ->Arg(1024)
    ->Arg(1400)
    ->Unit(benchmark::kMicrosecond);

// -----------------------------------------------------------------------------
// Multi-Channel Throughput Test
// -----------------------------------------------------------------------------

static void BM_MultiChannel_Pipeline(benchmark::State& state) {
    const size_t num_channels = static_cast<size_t>(state.range(0));
    const size_t frames_per_packet = 48;  // 1ms at 48kHz

    // Create ring buffers for each channel pair (stereo streams)
    const size_t num_streams = (num_channels + 1) / 2;
    std::vector<std::unique_ptr<pipeline::RingBuffer>> buffers;
    for (size_t i = 0; i < num_streams; ++i) {
        buffers.push_back(std::make_unique<pipeline::RingBuffer>(1024, 8));  // stereo float
    }

    std::vector<uint8_t> packet(8 * frames_per_packet);
    std::mt19937 rng(42);
    for (auto& b : packet) {
        b = static_cast<uint8_t>(rng());
    }

    for (auto _ : state) {
        for (auto& buf : buffers) {
            buf->write(packet.data(), frames_per_packet);
            buf->read(packet.data(), frames_per_packet);
        }
        benchmark::DoNotOptimize(packet[0]);
    }

    size_t total_samples = frames_per_packet * num_channels;
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(total_samples));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(8 * frames_per_packet * num_streams));
}
BENCHMARK(BM_MultiChannel_Pipeline)
    ->Arg(2)      // stereo
    ->Arg(8)      // 7.1
    ->Arg(16)
    ->Arg(32)
    ->Arg(64)
    ->Arg(128)    // large channel count
    ->Unit(benchmark::kMicrosecond);

// -----------------------------------------------------------------------------
// Packet Scheduling Throughput
// -----------------------------------------------------------------------------

static void BM_PacketBurst_Throughput(benchmark::State& state) {
    const size_t burst_size = static_cast<size_t>(state.range(0));
    const size_t packet_size = 384;  // 1ms stereo @ 48kHz

    pipeline::RingBuffer rb(4096, 8);  // stereo float
    std::vector<uint8_t> packet(packet_size);

    std::mt19937 rng(42);
    for (auto& b : packet) {
        b = static_cast<uint8_t>(rng());
    }

    for (auto _ : state) {
        // Simulate burst of packets
        for (size_t i = 0; i < burst_size; ++i) {
            rb.write(packet.data(), 48);
        }
        for (size_t i = 0; i < burst_size; ++i) {
            rb.read(packet.data(), 48);
        }
        benchmark::DoNotOptimize(packet[0]);
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(burst_size));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(packet_size * burst_size));
}
BENCHMARK(BM_PacketBurst_Throughput)
    ->Arg(1)
    ->Arg(4)
    ->Arg(8)
    ->Arg(16)
    ->Arg(32)
    ->Unit(benchmark::kMicrosecond);
