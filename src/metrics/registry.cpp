/**
 * Soluna — Metrics Registry Implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/metrics/registry.h>
#include <sstream>
#include <cmath>
#include <chrono>
#include <limits>

namespace soluna {
namespace metrics {

// Counter implementation
std::string Counter::format() const {
    std::ostringstream out;
    out << "# HELP " << name_ << " " << help_ << "\n";
    out << "# TYPE " << name_ << " counter\n";
    out << name_ << " " << value_.load(std::memory_order_relaxed) << "\n";
    return out.str();
}

// LabeledCounter implementation
void LabeledCounter::add(const Labels& labels, uint64_t delta) {
    std::string key = labels_to_key(labels);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = values_.find(key);
    if (it == values_.end()) {
        auto& entry = values_[key];
        entry.first = labels;
        entry.second.store(delta, std::memory_order_relaxed);
    } else {
        it->second.second.fetch_add(delta, std::memory_order_relaxed);
    }
}

std::string LabeledCounter::labels_to_key(const Labels& labels) const {
    std::string key;
    for (const auto& label : labels) {
        key += label.name + "=" + label.value + ",";
    }
    return key;
}

std::string LabeledCounter::format() const {
    std::ostringstream out;
    out << "# HELP " << name_ << " " << help_ << "\n";
    out << "# TYPE " << name_ << " counter\n";

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [key, entry] : values_) {
        out << name_ << "{";
        bool first = true;
        for (const auto& label : entry.first) {
            if (!first) out << ",";
            out << label.name << "=\"" << label.value << "\"";
            first = false;
        }
        out << "} " << entry.second.load(std::memory_order_relaxed) << "\n";
    }
    return out.str();
}

// Gauge implementation
void Gauge::add(double delta) {
    double current = value_.load(std::memory_order_relaxed);
    while (!value_.compare_exchange_weak(current, current + delta,
                                          std::memory_order_relaxed));
}

void Gauge::set_to_current_time() {
    auto now = std::chrono::system_clock::now();
    auto epoch = now.time_since_epoch();
    double seconds = std::chrono::duration<double>(epoch).count();
    set(seconds);
}

std::string Gauge::format() const {
    std::ostringstream out;
    out << "# HELP " << name_ << " " << help_ << "\n";
    out << "# TYPE " << name_ << " gauge\n";
    out << name_ << " " << value_.load(std::memory_order_relaxed) << "\n";
    return out.str();
}

// LabeledGauge implementation
void LabeledGauge::set(const Labels& labels, double value) {
    std::string key = labels_to_key(labels);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = values_.find(key);
    if (it == values_.end()) {
        auto& entry = values_[key];
        entry.first = labels;
        entry.second.store(value, std::memory_order_relaxed);
    } else {
        it->second.second.store(value, std::memory_order_relaxed);
    }
}

void LabeledGauge::add(const Labels& labels, double delta) {
    std::string key = labels_to_key(labels);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = values_.find(key);
    if (it == values_.end()) {
        auto& entry = values_[key];
        entry.first = labels;
        entry.second.store(delta, std::memory_order_relaxed);
    } else {
        double current = it->second.second.load(std::memory_order_relaxed);
        while (!it->second.second.compare_exchange_weak(current, current + delta,
                                                         std::memory_order_relaxed));
    }
}

std::string LabeledGauge::labels_to_key(const Labels& labels) const {
    std::string key;
    for (const auto& label : labels) {
        key += label.name + "=" + label.value + ",";
    }
    return key;
}

std::string LabeledGauge::format() const {
    std::ostringstream out;
    out << "# HELP " << name_ << " " << help_ << "\n";
    out << "# TYPE " << name_ << " gauge\n";

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [key, entry] : values_) {
        out << name_ << "{";
        bool first = true;
        for (const auto& label : entry.first) {
            if (!first) out << ",";
            out << label.name << "=\"" << label.value << "\"";
            first = false;
        }
        out << "} " << entry.second.load(std::memory_order_relaxed) << "\n";
    }
    return out.str();
}

// Histogram implementation
std::vector<double> Histogram::default_buckets() {
    return {0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0};
}

std::vector<double> Histogram::linear_buckets(double start, double width, int count) {
    std::vector<double> buckets;
    for (int i = 0; i < count; i++) {
        buckets.push_back(start + width * i);
    }
    return buckets;
}

std::vector<double> Histogram::exponential_buckets(double start, double factor, int count) {
    std::vector<double> buckets;
    double value = start;
    for (int i = 0; i < count; i++) {
        buckets.push_back(value);
        value *= factor;
    }
    return buckets;
}

Histogram::Histogram(std::string name, std::string help)
    : Histogram(std::move(name), std::move(help), default_buckets()) {}

Histogram::Histogram(std::string name, std::string help, std::vector<double> bucket_bounds)
    : Metric(std::move(name), std::move(help), MetricType::Histogram) {
    // Add +Inf bucket
    bucket_bounds.push_back(std::numeric_limits<double>::infinity());

    for (double bound : bucket_bounds) {
        auto bucket = std::make_unique<HistogramBucket>();
        bucket->upper_bound = bound;
        buckets_.push_back(std::move(bucket));
    }
}

void Histogram::observe(double value) {
    // Update all buckets where value <= upper_bound
    for (auto& bucket : buckets_) {
        if (value <= bucket->upper_bound) {
            bucket->count.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Update count and sum
    count_.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(sum_mutex_);
        double current = sum_.load(std::memory_order_relaxed);
        while (!sum_.compare_exchange_weak(current, current + value,
                                            std::memory_order_relaxed));
    }
}

double Histogram::sum() const {
    return sum_.load(std::memory_order_relaxed);
}

std::string Histogram::format() const {
    std::ostringstream out;
    out << "# HELP " << name_ << " " << help_ << "\n";
    out << "# TYPE " << name_ << " histogram\n";

    for (const auto& bucket : buckets_) {
        out << name_ << "_bucket{le=\"";
        if (std::isinf(bucket->upper_bound)) {
            out << "+Inf";
        } else {
            out << bucket->upper_bound;
        }
        out << "\"} " << bucket->count.load(std::memory_order_relaxed) << "\n";
    }

    out << name_ << "_sum " << sum_.load(std::memory_order_relaxed) << "\n";
    out << name_ << "_count " << count_.load(std::memory_order_relaxed) << "\n";

    return out.str();
}

// Registry implementation
Registry& Registry::instance() {
    static Registry instance;
    return instance;
}

Counter& Registry::counter(const std::string& name, const std::string& help) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = metrics_.find(name);
    if (it != metrics_.end()) {
        return dynamic_cast<Counter&>(*it->second);
    }
    auto counter = std::make_unique<Counter>(name, help);
    Counter& ref = *counter;
    metrics_[name] = std::move(counter);
    return ref;
}

LabeledCounter& Registry::labeled_counter(const std::string& name, const std::string& help,
                                           const std::vector<std::string>& label_names) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = metrics_.find(name);
    if (it != metrics_.end()) {
        return dynamic_cast<LabeledCounter&>(*it->second);
    }
    auto counter = std::make_unique<LabeledCounter>(name, help, label_names);
    LabeledCounter& ref = *counter;
    metrics_[name] = std::move(counter);
    return ref;
}

Gauge& Registry::gauge(const std::string& name, const std::string& help) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = metrics_.find(name);
    if (it != metrics_.end()) {
        return dynamic_cast<Gauge&>(*it->second);
    }
    auto gauge = std::make_unique<Gauge>(name, help);
    Gauge& ref = *gauge;
    metrics_[name] = std::move(gauge);
    return ref;
}

LabeledGauge& Registry::labeled_gauge(const std::string& name, const std::string& help,
                                       const std::vector<std::string>& label_names) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = metrics_.find(name);
    if (it != metrics_.end()) {
        return dynamic_cast<LabeledGauge&>(*it->second);
    }
    auto gauge = std::make_unique<LabeledGauge>(name, help, label_names);
    LabeledGauge& ref = *gauge;
    metrics_[name] = std::move(gauge);
    return ref;
}

Histogram& Registry::histogram(const std::string& name, const std::string& help) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = metrics_.find(name);
    if (it != metrics_.end()) {
        return dynamic_cast<Histogram&>(*it->second);
    }
    auto histogram = std::make_unique<Histogram>(name, help);
    Histogram& ref = *histogram;
    metrics_[name] = std::move(histogram);
    return ref;
}

Histogram& Registry::histogram(const std::string& name, const std::string& help,
                                const std::vector<double>& buckets) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = metrics_.find(name);
    if (it != metrics_.end()) {
        return dynamic_cast<Histogram&>(*it->second);
    }
    auto histogram = std::make_unique<Histogram>(name, help, buckets);
    Histogram& ref = *histogram;
    metrics_[name] = std::move(histogram);
    return ref;
}

Metric* Registry::get(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = metrics_.find(name);
    return it != metrics_.end() ? it->second.get() : nullptr;
}

std::string Registry::format_all() const {
    std::ostringstream out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [name, metric] : metrics_) {
        out << metric->format() << "\n";
    }
    return out.str();
}

void Registry::for_each(const std::function<void(const Metric&)>& visitor) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [name, metric] : metrics_) {
        visitor(*metric);
    }
}

void Registry::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_.clear();
}

// Standard metrics
namespace standard {

Counter& audio_frames_processed_total() {
    return Registry::instance().counter(
        "soluna_audio_frames_processed_total",
        "Total number of audio frames processed");
}

Counter& audio_buffer_underruns_total() {
    return Registry::instance().counter(
        "soluna_audio_buffer_underruns_total",
        "Total number of audio buffer underruns");
}

Counter& audio_buffer_overruns_total() {
    return Registry::instance().counter(
        "soluna_audio_buffer_overruns_total",
        "Total number of audio buffer overruns");
}

Counter& rtp_packets_sent_total() {
    return Registry::instance().counter(
        "soluna_rtp_packets_sent_total",
        "Total number of RTP packets sent");
}

Counter& rtp_packets_received_total() {
    return Registry::instance().counter(
        "soluna_rtp_packets_received_total",
        "Total number of RTP packets received");
}

Counter& rtp_packets_lost_total() {
    return Registry::instance().counter(
        "soluna_rtp_packets_lost_total",
        "Total number of RTP packets lost");
}

Counter& rtp_bytes_sent_total() {
    return Registry::instance().counter(
        "soluna_rtp_bytes_sent_total",
        "Total bytes sent via RTP");
}

Counter& rtp_bytes_received_total() {
    return Registry::instance().counter(
        "soluna_rtp_bytes_received_total",
        "Total bytes received via RTP");
}

Gauge& ptp_offset_ns() {
    return Registry::instance().gauge(
        "soluna_ptp_offset_ns",
        "Current PTP clock offset in nanoseconds");
}

Gauge& ptp_synced() {
    return Registry::instance().gauge(
        "soluna_ptp_synced",
        "PTP synchronization status (1=synced, 0=not synced)");
}

Gauge& ptp_delay_ns() {
    return Registry::instance().gauge(
        "soluna_ptp_delay_ns",
        "Current PTP path delay in nanoseconds");
}

Gauge& active_streams() {
    return Registry::instance().gauge(
        "soluna_active_streams",
        "Number of active audio streams");
}

Gauge& active_connections() {
    return Registry::instance().gauge(
        "soluna_active_connections",
        "Number of active client connections");
}

Gauge& uptime_seconds() {
    return Registry::instance().gauge(
        "soluna_uptime_seconds",
        "Daemon uptime in seconds");
}

Gauge& cpu_usage_percent() {
    return Registry::instance().gauge(
        "soluna_cpu_usage_percent",
        "CPU usage percentage");
}

Gauge& memory_usage_bytes() {
    return Registry::instance().gauge(
        "soluna_memory_usage_bytes",
        "Memory usage in bytes");
}

Histogram& audio_latency_seconds() {
    return Registry::instance().histogram(
        "soluna_audio_latency_seconds",
        "Audio end-to-end latency in seconds",
        {0.0001, 0.0005, 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1});
}

Histogram& network_latency_seconds() {
    return Registry::instance().histogram(
        "soluna_network_latency_seconds",
        "Network round-trip latency in seconds");
}

Histogram& processing_latency_seconds() {
    return Registry::instance().histogram(
        "soluna_processing_latency_seconds",
        "Audio processing latency in seconds",
        {0.00001, 0.00005, 0.0001, 0.0005, 0.001, 0.005, 0.01});
}

} // namespace standard

} // namespace metrics
} // namespace soluna
