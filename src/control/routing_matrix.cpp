/**
 * Routing Matrix Implementation
 * SPDX-License-Identifier: MIT
 */

#include <soluna/control/routing.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace soluna::control {

RoutingMatrix::RoutingMatrix() = default;
RoutingMatrix::~RoutingMatrix() = default;

bool RoutingMatrix::add_route(const ChannelId& source, const ChannelId& sink,
                               float gain_db) {
    std::lock_guard<std::mutex> lock(mutex_);
    RouteKey key{source, sink};

    if (routes_.count(key) > 0) return false; // already exists

    Route route;
    route.source = source;
    route.sink = sink;
    route.gain_db = gain_db;
    route.muted = false;

    routes_[key] = route;
    notify(route, true);
    return true;
}

bool RoutingMatrix::remove_route(const ChannelId& source, const ChannelId& sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    RouteKey key{source, sink};

    auto it = routes_.find(key);
    if (it == routes_.end()) return false;

    Route route = it->second;
    routes_.erase(it);
    notify(route, false);
    return true;
}

void RoutingMatrix::remove_device_routes(const std::string& device) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RouteKey> to_remove;

    for (const auto& [key, route] : routes_) {
        if (route.source.device == device || route.sink.device == device) {
            to_remove.push_back(key);
        }
    }

    for (const auto& key : to_remove) {
        auto it = routes_.find(key);
        if (it != routes_.end()) {
            Route route = it->second;
            routes_.erase(it);
            notify(route, false);
        }
    }
}

bool RoutingMatrix::set_gain(const ChannelId& source, const ChannelId& sink,
                              float gain_db) {
    std::lock_guard<std::mutex> lock(mutex_);
    RouteKey key{source, sink};
    auto it = routes_.find(key);
    if (it == routes_.end()) return false;
    it->second.gain_db = gain_db;
    return true;
}

bool RoutingMatrix::set_mute(const ChannelId& source, const ChannelId& sink,
                              bool muted) {
    std::lock_guard<std::mutex> lock(mutex_);
    RouteKey key{source, sink};
    auto it = routes_.find(key);
    if (it == routes_.end()) return false;
    it->second.muted = muted;
    return true;
}

std::vector<Route> RoutingMatrix::list_routes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Route> result;
    result.reserve(routes_.size());
    for (const auto& [key, route] : routes_) {
        result.push_back(route);
    }
    return result;
}

std::vector<Route> RoutingMatrix::get_source_routes(const ChannelId& source) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Route> result;
    for (const auto& [key, route] : routes_) {
        if (route.source == source) {
            result.push_back(route);
        }
    }
    return result;
}

std::vector<Route> RoutingMatrix::get_sink_routes(const ChannelId& sink) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Route> result;
    for (const auto& [key, route] : routes_) {
        if (route.sink == sink) {
            result.push_back(route);
        }
    }
    return result;
}

bool RoutingMatrix::has_route(const ChannelId& source, const ChannelId& sink) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return routes_.count({source, sink}) > 0;
}

size_t RoutingMatrix::route_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return routes_.size();
}

void RoutingMatrix::apply(const std::map<ChannelId, const float*>& sources,
                           std::map<ChannelId, float*>& sinks,
                           size_t frame_count) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Zero all sink buffers first
    for (auto& [id, buf] : sinks) {
        std::memset(buf, 0, frame_count * sizeof(float));
    }

    // Sum all routed sources into sinks with gain
    for (const auto& [key, route] : routes_) {
        auto src_it = sources.find(route.source);
        auto dst_it = sinks.find(route.sink);
        if (src_it == sources.end() || dst_it == sinks.end()) continue;

        const float* src = src_it->second;
        float* dst = dst_it->second;
        float gain = route.gain_linear();

        for (size_t i = 0; i < frame_count; i++) {
            dst[i] += src[i] * gain;
        }
    }
}

MeterValues RoutingMatrix::get_meter(const ChannelId& channel) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = meters_.find(channel);
    if (it == meters_.end()) {
        return MeterValues{};
    }
    return it->second;
}

void RoutingMatrix::update_meters(const ChannelId& channel, const float* data,
                                   size_t frame_count) {
    if (frame_count == 0 || !data) return;

    float peak = 0.0f;
    float sum_sq = 0.0f;
    uint64_t clips = 0;

    for (size_t i = 0; i < frame_count; i++) {
        float abs_val = std::abs(data[i]);
        if (abs_val > peak) peak = abs_val;
        sum_sq += data[i] * data[i];
        if (abs_val >= 1.0f) clips++;
    }

    float rms = std::sqrt(sum_sq / static_cast<float>(frame_count));

    // Convert to dBFS
    constexpr float kMinDb = -144.0f;
    float peak_db = (peak > 0.0f) ? 20.0f * std::log10(peak) : kMinDb;
    float rms_db = (rms > 0.0f) ? 20.0f * std::log10(rms) : kMinDb;

    std::lock_guard<std::mutex> lock(mutex_);
    auto& meter = meters_[channel];

    // Peak hold with fast attack / slow decay
    if (peak_db > meter.peak_db) {
        meter.peak_db = peak_db;
    } else {
        meter.peak_db = meter.peak_db * 0.95f + peak_db * 0.05f;
    }

    meter.rms_db = meter.rms_db * 0.9f + rms_db * 0.1f;
    meter.clip_count += clips;
}

void RoutingMatrix::reset_meters() {
    std::lock_guard<std::mutex> lock(mutex_);
    meters_.clear();
}

void RoutingMatrix::set_change_callback(RouteChangeCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    change_callback_ = std::move(cb);
}

void RoutingMatrix::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [key, route] : routes_) {
        notify(route, false);
    }
    routes_.clear();
}

void RoutingMatrix::notify(const Route& route, bool added) {
    if (change_callback_) {
        change_callback_(route, added);
    }
}

// Crossfade helper
void crossfade(const float* old_buf, const float* new_buf, float* output,
               size_t frame_count) {
    for (size_t i = 0; i < frame_count; i++) {
        float t = static_cast<float>(i) / static_cast<float>(frame_count);
        output[i] = old_buf[i] * (1.0f - t) + new_buf[i] * t;
    }
}

} // namespace soluna::control
