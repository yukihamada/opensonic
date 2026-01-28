/**
 * mDNS/DNS-SD Discovery Implementation
 *
 * Phase 3: In-process device registry.
 * Network-based mDNS (via Avahi/Bonjour) will be added when
 * platform integration is complete.
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/control/discovery.h>
#include <soluna/pal/time.h>
#include <algorithm>

namespace soluna::control {

Discovery::Discovery() = default;
Discovery::~Discovery() { stop(); }

void Discovery::set_local_device(const DeviceInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    local_device_ = info;
    local_device_.is_local = true;
    devices_[info.id] = local_device_;
}

bool Discovery::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return false;
    running_ = true;

    // Add local device to registry
    if (!local_device_.id.empty()) {
        local_device_.last_seen_ns = pal::Clock::instance().monotonic_now().to_ns();
        devices_[local_device_.id] = local_device_;
        notify(DiscoveryEvent::DeviceAdded, local_device_);
    }

    return true;
}

void Discovery::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
}

void Discovery::set_callback(DiscoveryCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(cb);
}

std::vector<DeviceInfo> Discovery::devices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DeviceInfo> result;
    result.reserve(devices_.size());
    for (const auto& [id, info] : devices_) {
        result.push_back(info);
    }
    return result;
}

std::unique_ptr<DeviceInfo> Discovery::find_device(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(id);
    if (it == devices_.end()) return nullptr;
    return std::make_unique<DeviceInfo>(it->second);
}

void Discovery::add_device(const DeviceInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool exists = devices_.count(info.id) > 0;
    DeviceInfo updated = info;
    updated.last_seen_ns = pal::Clock::instance().monotonic_now().to_ns();
    devices_[info.id] = updated;
    notify(exists ? DiscoveryEvent::DeviceUpdated : DiscoveryEvent::DeviceAdded, updated);
}

void Discovery::remove_device(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(id);
    if (it != devices_.end()) {
        DeviceInfo removed = it->second;
        devices_.erase(it);
        notify(DiscoveryEvent::DeviceRemoved, removed);
    }
}

void Discovery::prune_stale(int64_t timeout_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = pal::Clock::instance().monotonic_now().to_ns();
    std::vector<std::string> stale;

    for (const auto& [id, info] : devices_) {
        if (info.is_local) continue; // never prune local device
        if (now - info.last_seen_ns > timeout_ns) {
            stale.push_back(id);
        }
    }

    for (const auto& id : stale) {
        auto it = devices_.find(id);
        if (it != devices_.end()) {
            DeviceInfo removed = it->second;
            devices_.erase(it);
            notify(DiscoveryEvent::DeviceRemoved, removed);
        }
    }
}

void Discovery::notify(DiscoveryEvent event, const DeviceInfo& device) {
    if (callback_) {
        callback_(event, device);
    }
}

} // namespace soluna::control
