/**
 * FEC Decoder Implementation
 * SPDX-License-Identifier: MIT
 */

#include <soluna/wifi/fec.h>
#include <algorithm>
#include <cstring>
#include <map>

namespace soluna::wifi {

FecDecoder::FecDecoder(const FecConfig& config)
    : config_(config)
{
}

FecDecoder::~FecDecoder() = default;

FecDecoder::Group& FecDecoder::get_or_create_group(uint32_t group_id) {
    auto it = groups_.find(group_id);
    if (it != groups_.end()) return it->second;

    Group g;
    g.data_packets.resize(config_.group_size);
    g.data_received.resize(config_.group_size, false);
    g.parity_packets.resize(config_.parity_count);
    g.parity_received.resize(config_.parity_count, false);
    groups_[group_id] = std::move(g);
    return groups_[group_id];
}

void FecDecoder::feed(uint32_t group_id, uint8_t index, bool is_parity,
                      const void* data, size_t size) {
    auto& group = get_or_create_group(group_id);

    std::vector<uint8_t> buf(size);
    std::memcpy(buf.data(), data, size);

    if (size > group.max_packet_size) {
        group.max_packet_size = size;
    }

    if (is_parity) {
        uint8_t parity_idx = index - config_.group_size;
        if (parity_idx < config_.parity_count) {
            group.parity_packets[parity_idx] = std::move(buf);
            group.parity_received[parity_idx] = true;
        }
    } else {
        if (index < config_.group_size) {
            group.data_packets[index] = std::move(buf);
            group.data_received[index] = true;
        }
    }
}

bool FecDecoder::is_complete(uint32_t group_id) const {
    auto it = groups_.find(group_id);
    if (it == groups_.end()) return false;

    for (bool received : it->second.data_received) {
        if (!received) return false;
    }
    return true;
}

bool FecDecoder::can_recover(uint32_t group_id) const {
    auto it = groups_.find(group_id);
    if (it == groups_.end()) return false;

    const auto& group = it->second;

    // Count missing data packets
    size_t missing = 0;
    for (bool received : group.data_received) {
        if (!received) missing++;
    }

    if (missing == 0) return true; // already complete

    switch (config_.mode) {
        case FecMode::XorParity:
            // XOR can recover exactly 1 missing packet
            return missing == 1 && group.parity_received.size() > 0
                   && group.parity_received[0];

        case FecMode::ReedSolomon: {
            // Our simplified RS can recover if we have enough parity
            size_t parity_available = 0;
            for (bool received : group.parity_received) {
                if (received) parity_available++;
            }
            return missing <= parity_available;
        }

        case FecMode::None:
            return missing == 0;
    }

    return false;
}

std::vector<FecPacket> FecDecoder::recover(uint32_t group_id) {
    auto it = groups_.find(group_id);
    if (it == groups_.end()) return {};

    auto& group = it->second;

    // Count missing
    size_t missing = 0;
    for (bool received : group.data_received) {
        if (!received) missing++;
    }

    if (missing == 0) return {}; // nothing to recover

    switch (config_.mode) {
        case FecMode::XorParity:
            return recover_xor(group, group_id);
        case FecMode::ReedSolomon:
            return recover_rs(group, group_id);
        case FecMode::None:
            return {};
    }

    return {};
}

std::vector<FecPacket> FecDecoder::recover_xor(Group& group, uint32_t group_id) {
    // Find the single missing packet
    int missing_idx = -1;
    for (size_t i = 0; i < group.data_received.size(); i++) {
        if (!group.data_received[i]) {
            if (missing_idx >= 0) return {}; // more than 1 missing
            missing_idx = static_cast<int>(i);
        }
    }

    if (missing_idx < 0) return {};
    if (group.parity_packets.empty() || !group.parity_received[0]) return {};

    // Recover: missing = parity XOR all_other_data
    size_t max_len = group.max_packet_size;
    std::vector<uint8_t> recovered(max_len, 0);

    // Start with parity
    for (size_t j = 0; j < group.parity_packets[0].size() && j < max_len; j++) {
        recovered[j] = group.parity_packets[0][j];
    }

    // XOR with all received data packets
    for (size_t i = 0; i < group.data_packets.size(); i++) {
        if (static_cast<int>(i) == missing_idx) continue;
        for (size_t j = 0; j < group.data_packets[i].size() && j < max_len; j++) {
            recovered[j] ^= group.data_packets[i][j];
        }
    }

    FecPacket pkt;
    pkt.fec_group_id = group_id;
    pkt.index = static_cast<uint8_t>(missing_idx);
    pkt.is_parity = false;
    pkt.data = std::move(recovered);

    group.data_packets[missing_idx] = pkt.data;
    group.data_received[missing_idx] = true;

    return {pkt};
}

std::vector<FecPacket> FecDecoder::recover_rs(Group& group, uint32_t group_id) {
    // Simplified RS recovery using the same bit-pattern XOR scheme
    // Recover missing packets iteratively using available parity

    std::vector<FecPacket> recovered;

    // Iteratively try to recover missing packets
    bool progress = true;
    while (progress) {
        progress = false;

        for (size_t p = 0; p < group.parity_received.size(); p++) {
            if (!group.parity_received[p]) continue;

            // Find which data packets this parity covers
            std::vector<size_t> covered;
            for (size_t i = 0; i < static_cast<size_t>(config_.group_size); i++) {
                if ((i + 1) & (1u << p)) {
                    covered.push_back(i);
                }
            }

            // Count missing in this subset
            int missing_idx = -1;
            int missing_count = 0;
            for (size_t idx : covered) {
                if (!group.data_received[idx]) {
                    missing_idx = static_cast<int>(idx);
                    missing_count++;
                }
            }

            if (missing_count != 1 || missing_idx < 0) continue;

            // Recover the single missing packet from this parity
            size_t max_len = group.max_packet_size;
            std::vector<uint8_t> rec(max_len, 0);

            // Start with parity
            for (size_t j = 0; j < group.parity_packets[p].size() && j < max_len; j++) {
                rec[j] = group.parity_packets[p][j];
            }

            // XOR with received packets in the subset
            for (size_t idx : covered) {
                if (static_cast<int>(idx) == missing_idx) continue;
                for (size_t j = 0; j < group.data_packets[idx].size() && j < max_len; j++) {
                    rec[j] ^= group.data_packets[idx][j];
                }
            }

            FecPacket pkt;
            pkt.fec_group_id = group_id;
            pkt.index = static_cast<uint8_t>(missing_idx);
            pkt.is_parity = false;
            pkt.data = std::move(rec);

            group.data_packets[missing_idx] = pkt.data;
            group.data_received[missing_idx] = true;
            recovered.push_back(pkt);
            progress = true;
        }
    }

    return recovered;
}

void FecDecoder::prune(uint32_t keep_groups) {
    while (groups_.size() > keep_groups) {
        groups_.erase(groups_.begin());
    }
}

void FecDecoder::reset() {
    groups_.clear();
}

} // namespace soluna::wifi
