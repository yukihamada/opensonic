/**
 * Stress Test: Long-running soak test
 *
 * Default: 60 seconds
 * Set SOLUNA_SOAK_DURATION_SEC environment variable for longer runs
 * (e.g., 28800 for 8 hours).
 *
 * Tests sustained packet scheduling, ring buffer throughput, and
 * routing operations over extended periods.
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/soluna.h>
#include <soluna/pipeline/ring_buffer.h>
#include <soluna/control/routing.h>
#include <soluna/transport/packet_scheduler.h>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace soluna;
using namespace soluna::pipeline;
using namespace soluna::control;
using namespace soluna::transport;

static int get_soak_duration_sec() {
    const char* env = std::getenv("SOLUNA_SOAK_DURATION_SEC");
    if (env) {
        int val = std::atoi(env);
        if (val > 0) return val;
    }
    return 60; // default 60s
}

TEST(LongevitySoak, RingBufferThroughput) {
    constexpr size_t kFramesPerPacket = 48;
    constexpr size_t kChannels = 2;
    constexpr size_t kFrameSize = kChannels * sizeof(float);

    RingBuffer ring(kFramesPerPacket * 8, kFrameSize);

    std::vector<float> write_buf(kFramesPerPacket * kChannels, 0.5f);
    std::vector<float> read_buf(kFramesPerPacket * kChannels);

    int duration_sec = get_soak_duration_sec();
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(duration_sec);

    uint64_t cycles = 0;
    uint64_t underruns = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        // Simulate 1ms packet cadence batch
        for (int pkt = 0; pkt < 100; pkt++) {
            size_t w = ring.write(write_buf.data(), kFramesPerPacket);
            if (w == 0) {
                // Buffer full — drain
                ring.read(read_buf.data(), kFramesPerPacket);
                ring.write(write_buf.data(), kFramesPerPacket);
            }

            size_t r = ring.read(read_buf.data(), kFramesPerPacket);
            if (r == 0) underruns++;
            cycles++;
        }
    }

    printf("Soak: %llu cycles, %llu underruns over %d seconds\n",
           (unsigned long long)cycles, (unsigned long long)underruns,
           duration_sec);

    // Underrun rate should be very low
    double underrun_rate = (double)underruns / cycles;
    EXPECT_LT(underrun_rate, 0.01) << "Too many underruns";
}

TEST(LongevitySoak, RoutingStability) {
    RoutingMatrix routing;

    constexpr uint32_t kChannels = 8;
    constexpr size_t kFrames = 48;

    // Set up 8x8 routing
    for (uint32_t ch = 1; ch <= kChannels; ch++) {
        routing.add_route(ChannelId{"src", ch}, ChannelId{"dst", ch}, 0.0f);
    }

    std::vector<std::vector<float>> src_bufs(kChannels, std::vector<float>(kFrames, 0.25f));
    std::vector<std::vector<float>> dst_bufs(kChannels, std::vector<float>(kFrames, 0.0f));

    std::map<ChannelId, const float*> sources;
    std::map<ChannelId, float*> sinks;
    for (uint32_t ch = 1; ch <= kChannels; ch++) {
        sources[ChannelId{"src", ch}] = src_bufs[ch - 1].data();
        sinks[ChannelId{"dst", ch}] = dst_bufs[ch - 1].data();
    }

    int duration_sec = get_soak_duration_sec();
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(duration_sec);

    uint64_t iterations = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        for (int batch = 0; batch < 1000; batch++) {
            // Zero sinks
            for (auto& buf : dst_bufs) {
                std::fill(buf.begin(), buf.end(), 0.0f);
            }
            routing.apply(sources, sinks, kFrames);
            iterations++;
        }

        // Periodically verify correctness
        if (iterations % 100000 == 0) {
            for (uint32_t ch = 0; ch < kChannels; ch++) {
                ASSERT_NEAR(dst_bufs[ch][0], 0.25f, 1e-6f)
                    << "Routing corruption at iteration " << iterations;
            }
        }
    }

    printf("Routing soak: %llu iterations over %d seconds\n",
           (unsigned long long)iterations, duration_sec);
    EXPECT_GT(iterations, 0u);
}

TEST(LongevitySoak, ProducerConsumerPipeline) {
    constexpr size_t kFramesPerPacket = 48;
    constexpr size_t kFrameSize = sizeof(float);

    RingBuffer ring(kFramesPerPacket * 16, kFrameSize);

    std::atomic<bool> running{true};
    std::atomic<uint64_t> produced{0};
    std::atomic<uint64_t> consumed{0};
    std::atomic<uint64_t> underruns{0};

    int duration_sec = get_soak_duration_sec();

    // Producer thread
    std::thread producer([&]() {
        std::vector<float> buf(kFramesPerPacket, 1.0f);
        while (running.load(std::memory_order_relaxed)) {
            if (ring.available_write() >= kFramesPerPacket) {
                ring.write(buf.data(), kFramesPerPacket);
                produced.fetch_add(1, std::memory_order_relaxed);
            }
            // Simulate ~1ms packet cadence
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    // Consumer thread
    std::thread consumer([&]() {
        std::vector<float> buf(kFramesPerPacket);
        while (running.load(std::memory_order_relaxed)) {
            if (ring.available_read() >= kFramesPerPacket) {
                ring.read(buf.data(), kFramesPerPacket);
                consumed.fetch_add(1, std::memory_order_relaxed);
            } else {
                underruns.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
    running.store(false, std::memory_order_relaxed);

    producer.join();
    consumer.join();

    uint64_t p = produced.load();
    uint64_t c = consumed.load();
    uint64_t u = underruns.load();

    printf("Producer-Consumer soak (%d sec): produced=%llu consumed=%llu underruns=%llu\n",
           duration_sec, (unsigned long long)p, (unsigned long long)c, (unsigned long long)u);

    EXPECT_GT(p, 0u);
    EXPECT_GT(c, 0u);
    // Consumed should be close to produced
    EXPECT_GE(c, p / 2) << "Consumer fell too far behind";
}
