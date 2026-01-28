/**
 * Soluna — Metrics Tests
 *
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>
#include <soluna/metrics/metrics.h>
#include <soluna/metrics/registry.h>
#include <thread>
#include <vector>

using namespace soluna::metrics;

class MetricsTest : public ::testing::Test {
protected:
    void SetUp() override {
        Registry::instance().clear();
    }
};

TEST_F(MetricsTest, CounterBasic) {
    Counter counter("test_counter", "A test counter");

    EXPECT_EQ(counter.value(), 0u);

    counter.inc();
    EXPECT_EQ(counter.value(), 1u);

    counter.add(5);
    EXPECT_EQ(counter.value(), 6u);
}

TEST_F(MetricsTest, CounterFormat) {
    Counter counter("test_requests_total", "Total requests");
    counter.add(42);

    std::string output = counter.format();
    EXPECT_NE(output.find("# HELP test_requests_total Total requests"), std::string::npos);
    EXPECT_NE(output.find("# TYPE test_requests_total counter"), std::string::npos);
    EXPECT_NE(output.find("test_requests_total 42"), std::string::npos);
}

TEST_F(MetricsTest, LabeledCounter) {
    LabeledCounter counter("http_requests_total", "Total HTTP requests",
                           {"method", "status"});

    counter.inc({{"method", "GET"}, {"status", "200"}});
    counter.inc({{"method", "GET"}, {"status", "200"}});
    counter.inc({{"method", "POST"}, {"status", "201"}});

    std::string output = counter.format();
    EXPECT_NE(output.find("method=\"GET\""), std::string::npos);
    EXPECT_NE(output.find("status=\"200\""), std::string::npos);
}

TEST_F(MetricsTest, GaugeBasic) {
    Gauge gauge("temperature", "Current temperature");

    gauge.set(25.5);
    EXPECT_DOUBLE_EQ(gauge.value(), 25.5);

    gauge.inc();
    EXPECT_DOUBLE_EQ(gauge.value(), 26.5);

    gauge.dec();
    EXPECT_DOUBLE_EQ(gauge.value(), 25.5);

    gauge.add(-5.0);
    EXPECT_DOUBLE_EQ(gauge.value(), 20.5);
}

TEST_F(MetricsTest, GaugeFormat) {
    Gauge gauge("queue_length", "Current queue length");
    gauge.set(10);

    std::string output = gauge.format();
    EXPECT_NE(output.find("# TYPE queue_length gauge"), std::string::npos);
    EXPECT_NE(output.find("queue_length 10"), std::string::npos);
}

TEST_F(MetricsTest, LabeledGauge) {
    LabeledGauge gauge("active_connections", "Active connections",
                       {"service"});

    gauge.set({{"service", "api"}}, 10);
    gauge.set({{"service", "web"}}, 20);
    gauge.inc({{"service", "api"}});

    std::string output = gauge.format();
    EXPECT_NE(output.find("service=\"api\""), std::string::npos);
    EXPECT_NE(output.find("service=\"web\""), std::string::npos);
}

TEST_F(MetricsTest, HistogramDefaultBuckets) {
    auto buckets = Histogram::default_buckets();
    EXPECT_EQ(buckets.size(), 12u);
    EXPECT_DOUBLE_EQ(buckets[0], 0.001);
    EXPECT_DOUBLE_EQ(buckets[11], 10.0);
}

TEST_F(MetricsTest, HistogramLinearBuckets) {
    auto buckets = Histogram::linear_buckets(0.0, 0.5, 5);
    EXPECT_EQ(buckets.size(), 5u);
    EXPECT_DOUBLE_EQ(buckets[0], 0.0);
    EXPECT_DOUBLE_EQ(buckets[1], 0.5);
    EXPECT_DOUBLE_EQ(buckets[4], 2.0);
}

TEST_F(MetricsTest, HistogramExponentialBuckets) {
    auto buckets = Histogram::exponential_buckets(1.0, 2.0, 4);
    EXPECT_EQ(buckets.size(), 4u);
    EXPECT_DOUBLE_EQ(buckets[0], 1.0);
    EXPECT_DOUBLE_EQ(buckets[1], 2.0);
    EXPECT_DOUBLE_EQ(buckets[2], 4.0);
    EXPECT_DOUBLE_EQ(buckets[3], 8.0);
}

TEST_F(MetricsTest, HistogramObserve) {
    Histogram histogram("request_duration", "Request duration",
                        {0.1, 0.5, 1.0});

    histogram.observe(0.05);
    histogram.observe(0.3);
    histogram.observe(0.8);
    histogram.observe(2.0);

    EXPECT_EQ(histogram.count(), 4u);
    EXPECT_DOUBLE_EQ(histogram.sum(), 3.15);
}

TEST_F(MetricsTest, HistogramFormat) {
    Histogram histogram("latency_seconds", "Latency in seconds",
                        {0.1, 0.5, 1.0});

    histogram.observe(0.05);
    histogram.observe(0.3);

    std::string output = histogram.format();
    EXPECT_NE(output.find("# TYPE latency_seconds histogram"), std::string::npos);
    EXPECT_NE(output.find("latency_seconds_bucket{le=\"0.1\"}"), std::string::npos);
    EXPECT_NE(output.find("latency_seconds_bucket{le=\"+Inf\"}"), std::string::npos);
    EXPECT_NE(output.find("latency_seconds_sum"), std::string::npos);
    EXPECT_NE(output.find("latency_seconds_count 2"), std::string::npos);
}

TEST_F(MetricsTest, Timer) {
    Histogram histogram("operation_duration", "Operation duration");

    {
        Timer timer(histogram);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(histogram.count(), 1u);
    EXPECT_GT(histogram.sum(), 0.009);  // At least 9ms
}

TEST_F(MetricsTest, RegistryCounter) {
    auto& counter1 = Registry::instance().counter("my_counter", "Help text");
    auto& counter2 = Registry::instance().counter("my_counter", "Different help");

    // Should return same metric
    EXPECT_EQ(&counter1, &counter2);

    counter1.inc();
    EXPECT_EQ(counter2.value(), 1u);
}

TEST_F(MetricsTest, RegistryGauge) {
    auto& gauge = Registry::instance().gauge("my_gauge", "Help text");
    gauge.set(42);

    auto* retrieved = Registry::instance().get("my_gauge");
    EXPECT_NE(retrieved, nullptr);
    EXPECT_EQ(dynamic_cast<Gauge*>(retrieved)->value(), 42);
}

TEST_F(MetricsTest, RegistryHistogram) {
    auto& hist = Registry::instance().histogram("my_histogram", "Help",
                                                 {0.1, 1.0, 10.0});
    hist.observe(0.5);
    hist.observe(5.0);

    EXPECT_EQ(hist.count(), 2u);
}

TEST_F(MetricsTest, RegistryFormatAll) {
    Registry::instance().counter("counter_a", "Counter A").add(10);
    Registry::instance().gauge("gauge_b", "Gauge B").set(20);

    std::string output = Registry::instance().format_all();

    EXPECT_NE(output.find("counter_a 10"), std::string::npos);
    EXPECT_NE(output.find("gauge_b 20"), std::string::npos);
}

TEST_F(MetricsTest, RegistryForEach) {
    Registry::instance().counter("c1", "Counter 1");
    Registry::instance().gauge("g1", "Gauge 1");

    int count = 0;
    Registry::instance().for_each([&count](const Metric&) {
        count++;
    });

    EXPECT_EQ(count, 2);
}

TEST_F(MetricsTest, StandardMetrics) {
    // Verify standard metrics are accessible
    standard::audio_frames_processed_total().inc();
    standard::rtp_packets_sent_total().add(100);
    standard::ptp_offset_ns().set(1000);
    standard::active_streams().set(2);
    standard::uptime_seconds().set(3600);
    standard::audio_latency_seconds().observe(0.001);

    // Verify they appear in output
    std::string output = Registry::instance().format_all();
    EXPECT_NE(output.find("soluna_audio_frames_processed_total"), std::string::npos);
    EXPECT_NE(output.find("soluna_rtp_packets_sent_total 100"), std::string::npos);
    EXPECT_NE(output.find("soluna_ptp_offset_ns 1000"), std::string::npos);
}

TEST_F(MetricsTest, ConcurrentCounterUpdates) {
    Counter counter("concurrent_counter", "Test concurrent updates");

    const int num_threads = 4;
    const int increments_per_thread = 10000;

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&counter, increments_per_thread]() {
            for (int j = 0; j < increments_per_thread; j++) {
                counter.inc();
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(counter.value(), static_cast<uint64_t>(num_threads * increments_per_thread));
}

TEST_F(MetricsTest, ConcurrentGaugeUpdates) {
    Gauge gauge("concurrent_gauge", "Test concurrent updates");

    const int num_threads = 4;
    const int updates_per_thread = 1000;

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&gauge, updates_per_thread]() {
            for (int j = 0; j < updates_per_thread; j++) {
                gauge.inc();
                gauge.dec();
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // After equal inc/dec, should be back to 0
    EXPECT_DOUBLE_EQ(gauge.value(), 0.0);
}

TEST_F(MetricsTest, ConcurrentHistogramObserve) {
    Histogram histogram("concurrent_histogram", "Test concurrent observe");

    const int num_threads = 4;
    const int observations_per_thread = 1000;

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&histogram, observations_per_thread]() {
            for (int j = 0; j < observations_per_thread; j++) {
                histogram.observe(1.0);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(histogram.count(), static_cast<uint64_t>(num_threads * observations_per_thread));
    EXPECT_DOUBLE_EQ(histogram.sum(), static_cast<double>(num_threads * observations_per_thread));
}
