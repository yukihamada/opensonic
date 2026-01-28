#include <soluna/pipeline/ring_buffer.h>
#include <gtest/gtest.h>
#include <vector>
#include <thread>
#include <numeric>

using soluna::pipeline::RingBuffer;

TEST(RingBuffer, CreateAndCapacity) {
    RingBuffer rb(100, 4);
    // Rounds up to 128
    EXPECT_EQ(rb.capacity(), 128u);
    EXPECT_EQ(rb.frame_size(), 4u);
}

TEST(RingBuffer, PowerOfTwoRounding) {
    RingBuffer rb1(1, 1);
    EXPECT_EQ(rb1.capacity(), 2u); // minimum 2

    RingBuffer rb2(64, 1);
    EXPECT_EQ(rb2.capacity(), 64u);

    RingBuffer rb3(65, 1);
    EXPECT_EQ(rb3.capacity(), 128u);
}

TEST(RingBuffer, EmptyState) {
    RingBuffer rb(64, sizeof(float));
    EXPECT_EQ(rb.available_read(), 0u);
    EXPECT_EQ(rb.available_write(), 64u);
}

TEST(RingBuffer, WriteAndRead) {
    RingBuffer rb(64, sizeof(int32_t));
    std::vector<int32_t> write_data = {1, 2, 3, 4, 5};
    std::vector<int32_t> read_data(5);

    size_t written = rb.write(write_data.data(), 5);
    EXPECT_EQ(written, 5u);
    EXPECT_EQ(rb.available_read(), 5u);

    size_t read = rb.read(read_data.data(), 5);
    EXPECT_EQ(read, 5u);
    EXPECT_EQ(rb.available_read(), 0u);
    EXPECT_EQ(read_data, write_data);
}

TEST(RingBuffer, PartialRead) {
    RingBuffer rb(64, sizeof(int32_t));
    std::vector<int32_t> data = {10, 20, 30, 40, 50};
    rb.write(data.data(), 5);

    std::vector<int32_t> out(3);
    size_t read = rb.read(out.data(), 3);
    EXPECT_EQ(read, 3u);
    EXPECT_EQ(out[0], 10);
    EXPECT_EQ(out[1], 20);
    EXPECT_EQ(out[2], 30);
    EXPECT_EQ(rb.available_read(), 2u);
}

TEST(RingBuffer, Overflow) {
    RingBuffer rb(4, sizeof(int32_t)); // capacity = 4
    std::vector<int32_t> data = {1, 2, 3, 4, 5, 6};

    size_t written = rb.write(data.data(), 6);
    EXPECT_EQ(written, 4u); // only 4 fit
    EXPECT_EQ(rb.available_read(), 4u);
    EXPECT_EQ(rb.available_write(), 0u);
}

TEST(RingBuffer, Wraparound) {
    RingBuffer rb(4, sizeof(int32_t));

    // Fill half, read half, fill again to force wrap
    int32_t d1[] = {1, 2};
    rb.write(d1, 2);
    int32_t out[2];
    rb.read(out, 2);

    int32_t d2[] = {3, 4, 5, 6};
    size_t written = rb.write(d2, 4);
    EXPECT_EQ(written, 4u);

    int32_t result[4];
    size_t read = rb.read(result, 4);
    EXPECT_EQ(read, 4u);
    EXPECT_EQ(result[0], 3);
    EXPECT_EQ(result[1], 4);
    EXPECT_EQ(result[2], 5);
    EXPECT_EQ(result[3], 6);
}

TEST(RingBuffer, Peek) {
    RingBuffer rb(64, sizeof(int32_t));
    std::vector<int32_t> data = {100, 200, 300};
    rb.write(data.data(), 3);

    std::vector<int32_t> peek_data(3);
    size_t peeked = rb.peek(peek_data.data(), 3);
    EXPECT_EQ(peeked, 3u);
    EXPECT_EQ(peek_data[0], 100);
    EXPECT_EQ(rb.available_read(), 3u); // still available
}

TEST(RingBuffer, Reset) {
    RingBuffer rb(64, sizeof(int32_t));
    int32_t data[] = {1, 2, 3};
    rb.write(data, 3);
    EXPECT_EQ(rb.available_read(), 3u);

    rb.reset();
    EXPECT_EQ(rb.available_read(), 0u);
    EXPECT_EQ(rb.available_write(), 64u);
}

TEST(RingBuffer, ConcurrentSPSC) {
    constexpr size_t kFrameCount = 100000;
    constexpr size_t kFrameSize = sizeof(int32_t);
    RingBuffer rb(1024, kFrameSize);

    std::atomic<bool> done{false};

    // Producer
    std::thread producer([&]() {
        for (size_t i = 0; i < kFrameCount; ) {
            int32_t val = static_cast<int32_t>(i);
            size_t written = rb.write(&val, 1);
            if (written > 0) i++;
        }
        done.store(true);
    });

    // Consumer
    std::vector<int32_t> received;
    received.reserve(kFrameCount);

    std::thread consumer([&]() {
        while (!done.load() || rb.available_read() > 0) {
            int32_t val;
            if (rb.read(&val, 1) > 0) {
                received.push_back(val);
            }
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), kFrameCount);
    for (size_t i = 0; i < kFrameCount; i++) {
        EXPECT_EQ(received[i], static_cast<int32_t>(i)) << "Mismatch at index " << i;
    }
}
