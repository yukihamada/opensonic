/**
 * Session Manager Implementation
 * SPDX-License-Identifier: MIT
 */

#include <soluna/control/session.h>
#include <soluna/pal/time.h>
#include <algorithm>
#include <cstdlib>

namespace soluna::control {

SessionManager::SessionManager() = default;
SessionManager::~SessionManager() = default;

uint16_t SessionManager::create_stream(const std::string& source_device,
                                        const std::string& sink_device,
                                        uint32_t channels,
                                        uint32_t sample_rate) {
    std::lock_guard<std::mutex> lock(mutex_);

    StreamInfo stream;
    stream.stream_id = next_id_++;
    stream.source_device = source_device;
    stream.sink_device = sink_device;
    stream.channels = channels;
    stream.sample_rate = sample_rate;
    stream.format = SampleFormat::S24_LE;
    stream.tier = PacketTier::Standard;
    stream.rtp_port = next_port_++;
    if (next_port_ > kPortRTPMax) {
        next_port_ = kPortRTPBase;
    }
    stream.ssrc = static_cast<uint32_t>(std::rand());
    stream.multicast_group = kMulticastAudio;
    stream.state = StreamState::Active;
    stream.created_ns = pal::Clock::instance().monotonic_now().to_ns();

    streams_[stream.stream_id] = stream;
    return stream.stream_id;
}

bool SessionManager::destroy_stream(uint16_t stream_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return false;
    streams_.erase(it);
    return true;
}

std::unique_ptr<StreamInfo> SessionManager::get_stream(uint16_t stream_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return nullptr;
    return std::make_unique<StreamInfo>(it->second);
}

std::vector<StreamInfo> SessionManager::list_streams() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<StreamInfo> result;
    result.reserve(streams_.size());
    for (const auto& [id, info] : streams_) {
        result.push_back(info);
    }
    return result;
}

void SessionManager::set_stream_state(uint16_t stream_id, StreamState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(stream_id);
    if (it != streams_.end()) {
        it->second.state = state;
    }
}

uint16_t SessionManager::next_stream_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_id_;
}

uint16_t SessionManager::next_rtp_port() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_port_;
}

} // namespace soluna::control
