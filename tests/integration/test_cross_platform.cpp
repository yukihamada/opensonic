/**
 * Integration tests for Cross-Platform PAL
 * Tests that Clock + Thread + Socket work together correctly.
 * SPDX-License-Identifier: MIT
 */

#include <soluna/pal/time.h>
#include <soluna/pal/thread.h>
#include <soluna/pal/net.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <vector>

using namespace soluna::pal;

// Thread + Clock integration: RT thread timing accuracy
TEST(CrossPlatformIntegration, ThreadTimingAccuracy) {
    constexpr int kIterations = 100;
    constexpr int64_t kIntervalNs = 1'000'000LL; // 1ms

    std::vector<int64_t> timestamps;
    timestamps.reserve(kIterations);
    std::atomic<bool> done{false};

    auto thread = Thread::create("timing_test", ThreadPriority::High);
    thread->start([&]() {
        auto& clock = Clock::instance();
        auto next = clock.monotonic_now();

        for (int i = 0; i < kIterations; i++) {
            timestamps.push_back(clock.monotonic_now().to_ns());
            next = Timestamp::from_ns(next.to_ns() + kIntervalNs);
            clock.sleep_until(next);
        }
        done.store(true);
    });

    thread->join();
    ASSERT_TRUE(done.load());
    ASSERT_EQ(timestamps.size(), static_cast<size_t>(kIterations));

    // Compute jitter statistics
    double total_jitter = 0.0;
    double max_jitter = 0.0;
    int late_count = 0;

    for (size_t i = 1; i < timestamps.size(); i++) {
        int64_t diff = timestamps[i] - timestamps[i - 1];
        double jitter_ms = std::abs(diff - kIntervalNs) / 1e6;
        total_jitter += jitter_ms;
        if (jitter_ms > max_jitter) max_jitter = jitter_ms;
        if (diff > kIntervalNs * 3) late_count++;
    }

    double avg_jitter_ms = total_jitter / (kIterations - 1);

    // Average jitter should be < 5ms (allows for scheduler contention under test load)
    EXPECT_LT(avg_jitter_ms, 5.0);
    // No more than 20% severely late packets (>3x interval) under load
    EXPECT_LT(late_count, kIterations / 5);
}

// Socket + Thread: concurrent send/receive
TEST(CrossPlatformIntegration, ConcurrentSocketIO) {
    constexpr int kPackets = 50;
    constexpr uint16_t kPort = 19760;

    std::atomic<int> received{0};
    std::atomic<bool> rx_ready{false};

    auto rx_thread = Thread::create("rx_thread");
    rx_thread->start([&]() {
        auto sock = UdpSocket::create();
        sock->bind(kPort);
        sock->set_recv_timeout_ms(500);
        rx_ready.store(true);

        char buf[64];
        SocketAddress src;
        for (int i = 0; i < kPackets; i++) {
            int n = sock->recv_from(buf, sizeof(buf), src);
            if (n > 0) received.fetch_add(1);
        }
    });

    // Wait for receiver to be ready
    while (!rx_ready.load()) {
        Clock::instance().sleep_ns(100'000LL);
    }
    Clock::instance().sleep_ns(1'000'000LL); // extra 1ms settle

    auto tx_thread = Thread::create("tx_thread");
    tx_thread->start([&]() {
        auto sock = UdpSocket::create();
        SocketAddress dest{"127.0.0.1", kPort};

        for (int i = 0; i < kPackets; i++) {
            char msg[16];
            snprintf(msg, sizeof(msg), "pkt%04d", i);
            sock->send_to(msg, 7, dest);
            Clock::instance().sleep_ns(100'000LL); // 0.1ms between packets
        }
    });

    tx_thread->join();
    rx_thread->join();

    // Should receive most packets (allow some loss due to timing)
    EXPECT_GT(received.load(), kPackets / 2);
}

// Clock precision test: measure monotonic resolution
TEST(CrossPlatformIntegration, ClockResolution) {
    auto& clock = Clock::instance();

    int64_t min_diff = INT64_MAX;
    for (int i = 0; i < 1000; i++) {
        auto t1 = clock.monotonic_now();
        auto t2 = clock.monotonic_now();
        int64_t diff = t2.to_ns() - t1.to_ns();
        if (diff > 0 && diff < min_diff) {
            min_diff = diff;
        }
    }

    // Resolution should be better than 1ms (most platforms < 1us)
    EXPECT_LT(min_diff, 1'000'000LL);
}

// Multi-thread socket broadcast
TEST(CrossPlatformIntegration, MultiThreadBroadcast) {
    constexpr int kThreads = 3;
    constexpr int kPacketsPerThread = 10;
    constexpr uint16_t kPort = 19761;

    std::atomic<int> total_received{0};
    std::atomic<bool> rx_ready{false};

    auto rx_thread = Thread::create("rx_bcast");
    rx_thread->start([&]() {
        auto sock = UdpSocket::create();
        sock->bind(kPort);
        sock->set_recv_timeout_ms(2000);
        rx_ready.store(true);

        char buf[64];
        SocketAddress src;
        int expected = kThreads * kPacketsPerThread;
        for (int i = 0; i < expected; i++) {
            int n = sock->recv_from(buf, sizeof(buf), src);
            if (n > 0) total_received.fetch_add(1);
        }
    });

    while (!rx_ready.load()) {
        Clock::instance().sleep_ns(100'000LL);
    }
    Clock::instance().sleep_ns(1'000'000LL);

    std::vector<std::unique_ptr<Thread>> senders;
    for (int t = 0; t < kThreads; t++) {
        auto thread = Thread::create("tx_" + std::to_string(t));
        thread->start([t]() {
            auto sock = UdpSocket::create();
            SocketAddress dest{"127.0.0.1", kPort};
            for (int i = 0; i < kPacketsPerThread; i++) {
                char msg[32];
                snprintf(msg, sizeof(msg), "t%d_p%d", t, i);
                sock->send_to(msg, static_cast<int>(strlen(msg)), dest);
                Clock::instance().sleep_ns(200'000LL);
            }
        });
        senders.push_back(std::move(thread));
    }

    for (auto& t : senders) t->join();
    rx_thread->join();

    EXPECT_GT(total_received.load(), (kThreads * kPacketsPerThread) / 2);
}
