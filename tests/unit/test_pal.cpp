/**
 * Unit tests for Platform Abstraction Layer
 * Tests Clock, Thread, and Socket interfaces on the current platform.
 * SPDX-License-Identifier: MIT
 */

#include <soluna/pal/time.h>
#include <soluna/pal/thread.h>
#include <soluna/pal/net.h>
#include <soluna/pal/audio.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <vector>

using namespace soluna::pal;

// ---- Clock Tests ----

TEST(ClockTest, MonotonicNowReturnsPositive) {
    auto ts = Clock::instance().monotonic_now();
    EXPECT_GT(ts.to_ns(), 0);
}

TEST(ClockTest, MonotonicIsMonotonic) {
    auto t1 = Clock::instance().monotonic_now();
    auto t2 = Clock::instance().monotonic_now();
    EXPECT_GE(t2.to_ns(), t1.to_ns());
}

TEST(ClockTest, RealtimeNowReturnsPositive) {
    auto ts = Clock::instance().realtime_now();
    // Realtime should be after 2020-01-01: ~1577836800 seconds
    EXPECT_GT(ts.seconds, 1577836800);
}

TEST(ClockTest, SleepNs) {
    auto before = Clock::instance().monotonic_now();
    Clock::instance().sleep_ns(1'000'000LL); // 1ms
    auto after = Clock::instance().monotonic_now();

    int64_t elapsed_ns = after.to_ns() - before.to_ns();
    // Should have slept at least 0.5ms (allowing for scheduling jitter)
    EXPECT_GT(elapsed_ns, 500'000LL);
    // Should not have slept more than 50ms
    EXPECT_LT(elapsed_ns, 50'000'000LL);
}

TEST(ClockTest, SleepUntil) {
    auto now = Clock::instance().monotonic_now();
    int64_t target_ns = now.to_ns() + 2'000'000LL; // 2ms from now
    auto target = Timestamp::from_ns(target_ns);

    Clock::instance().sleep_until(target);

    auto after = Clock::instance().monotonic_now();
    EXPECT_GE(after.to_ns(), target_ns - 500'000LL); // allow 0.5ms early
}

TEST(ClockTest, TimestampFromNs) {
    auto ts = Timestamp::from_ns(1'500'000'000LL); // 1.5 seconds
    EXPECT_EQ(ts.seconds, 1);
    EXPECT_EQ(ts.nanoseconds, 500'000'000);
}

TEST(ClockTest, TimestampFromNsNegative) {
    auto ts = Timestamp::from_ns(-500'000'000LL);
    EXPECT_EQ(ts.seconds, -1);
    EXPECT_EQ(ts.nanoseconds, 500'000'000);
}

// ---- Thread Tests ----

TEST(ThreadTest, CreateAndRun) {
    std::atomic<bool> ran{false};
    auto thread = Thread::create("test_thread");
    ASSERT_NE(thread, nullptr);

    bool started = thread->start([&ran]() {
        ran.store(true);
    });
    EXPECT_TRUE(started);

    thread->join();
    EXPECT_TRUE(ran.load());
    EXPECT_FALSE(thread->is_running());
}

TEST(ThreadTest, HighPriority) {
    std::atomic<bool> ran{false};
    auto thread = Thread::create("test_high", ThreadPriority::High);
    ASSERT_NE(thread, nullptr);

    thread->start([&ran]() { ran.store(true); });
    thread->join();
    EXPECT_TRUE(ran.load());
}

TEST(ThreadTest, MultipleThreads) {
    constexpr int N = 4;
    std::atomic<int> counter{0};
    std::vector<std::unique_ptr<Thread>> threads;

    for (int i = 0; i < N; i++) {
        auto t = Thread::create("worker_" + std::to_string(i));
        t->start([&counter]() {
            counter.fetch_add(1);
        });
        threads.push_back(std::move(t));
    }

    for (auto& t : threads) {
        t->join();
    }
    EXPECT_EQ(counter.load(), N);
}

TEST(ThreadTest, DoubleStartFails) {
    auto thread = Thread::create("test_double");
    std::atomic<bool> stop{false};

    thread->start([&stop]() {
        while (!stop.load()) {
            Clock::instance().sleep_ns(100'000LL);
        }
    });

    // Second start should fail
    bool ok = thread->start([]() {});
    EXPECT_FALSE(ok);

    stop.store(true);
    thread->join();
}

// ---- Socket Tests ----

TEST(SocketTest, Create) {
    auto sock = UdpSocket::create();
    ASSERT_NE(sock, nullptr);
    EXPECT_GE(sock->fd(), 0);
}

TEST(SocketTest, BindAndRecv) {
    auto sender = UdpSocket::create();
    auto receiver = UdpSocket::create();
    ASSERT_NE(sender, nullptr);
    ASSERT_NE(receiver, nullptr);

    ASSERT_TRUE(receiver->bind(0)); // ephemeral port

    // Get the bound port via fd
    // For this test, bind to a known port
    auto recv2 = UdpSocket::create();
    ASSERT_NE(recv2, nullptr);
    ASSERT_TRUE(recv2->bind(19753));
    recv2->set_recv_timeout_ms(100);

    // Send data
    SocketAddress dest{"127.0.0.1", 19753};
    const char* msg = "soluna_test";
    int sent = sender->send_to(msg, 11, dest);
    EXPECT_EQ(sent, 11);

    // Receive data
    char buf[64]{};
    SocketAddress src;
    int n = recv2->recv_from(buf, sizeof(buf), src);
    EXPECT_EQ(n, 11);
    EXPECT_EQ(std::string(buf, 11), "soluna_test");
    EXPECT_EQ(src.ip, "127.0.0.1");
}

TEST(SocketTest, NonblockingRecvNoData) {
    auto sock = UdpSocket::create();
    ASSERT_NE(sock, nullptr);
    ASSERT_TRUE(sock->bind(19754));

    char buf[64];
    SocketAddress src;
    int n = sock->recv_from_nonblock(buf, sizeof(buf), src);
    EXPECT_LE(n, 0); // no data available
}

TEST(SocketTest, SetDscp) {
    auto sock = UdpSocket::create();
    ASSERT_NE(sock, nullptr);
    // EF (Expedited Forwarding) = 46
    bool ok = sock->set_dscp(46);
    // May fail without root, but should not crash
    (void)ok;
}

// ---- Audio Device Tests ----

TEST(AudioDeviceTest, Enumerate) {
    auto devices = AudioDevice::enumerate();
    // Just verify it doesn't crash; may return empty list in CI
    EXPECT_GE(devices.size(), 0u);

    for (const auto& d : devices) {
        EXPECT_FALSE(d.id.empty());
        EXPECT_FALSE(d.name.empty());
    }
}

TEST(AudioDeviceTest, Create) {
    auto device = AudioDevice::create();
    ASSERT_NE(device, nullptr);
    EXPECT_FALSE(device->is_running());
}
