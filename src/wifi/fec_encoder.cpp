/**
 * FEC Encoder Implementation
 * SPDX-License-Identifier: MIT
 */

#include <soluna/wifi/fec.h>
#include <algorithm>
#include <cstring>

namespace soluna::wifi {

FecEncoder::FecEncoder(const FecConfig& config)
    : config_(config)
{
}

FecEncoder::~FecEncoder() = default;

bool FecEncoder::feed(const void* data, size_t size) {
    // Store the data packet
    std::vector<uint8_t> buf(size);
    std::memcpy(buf.data(), data, size);
    group_buf_.push_back(std::move(buf));

    if (size > max_len_in_group_) {
        max_len_in_group_ = size;
    }

    // Check if group is complete
    if (group_buf_.size() >= config_.group_size) {
        parity_packets_.clear();

        switch (config_.mode) {
            case FecMode::XorParity:
                generate_xor_parity();
                break;
            case FecMode::ReedSolomon:
                generate_rs_parity();
                break;
            case FecMode::None:
                break;
        }

        group_buf_.clear();
        max_len_in_group_ = 0;
        group_id_++;
        return !parity_packets_.empty();
    }

    return false;
}

void FecEncoder::generate_xor_parity() {
    // XOR all packets in the group to produce one parity packet
    size_t max_len = max_len_in_group_;

    FecPacket parity;
    parity.fec_group_id = group_id_;
    parity.index = config_.group_size;
    parity.is_parity = true;
    parity.data.resize(max_len, 0);

    for (const auto& pkt : group_buf_) {
        for (size_t i = 0; i < pkt.size(); i++) {
            parity.data[i] ^= pkt[i];
        }
        // Shorter packets contribute 0 for remaining bytes (already 0-initialized)
    }

    parity_packets_.push_back(std::move(parity));
}

void FecEncoder::generate_rs_parity() {
    // Simplified Reed-Solomon: use systematic RS-like encoding
    // For Phase 4, we implement a practical XOR-based multi-parity scheme:
    // Parity[j] = XOR of data packets where bit j is set in packet index
    //
    // This allows recovery of up to parity_count lost packets
    // when the loss pattern is favorable.

    size_t max_len = max_len_in_group_;
    size_t n_parity = std::min(static_cast<size_t>(config_.parity_count),
                                static_cast<size_t>(4));

    for (size_t p = 0; p < n_parity; p++) {
        FecPacket parity;
        parity.fec_group_id = group_id_;
        parity.index = static_cast<uint8_t>(config_.group_size + p);
        parity.is_parity = true;
        parity.data.resize(max_len, 0);

        // Each parity packet XORs a different subset of data packets
        // Parity p covers packets where bit p is set in (index+1)
        for (size_t i = 0; i < group_buf_.size(); i++) {
            if ((i + 1) & (1u << p)) {
                for (size_t j = 0; j < group_buf_[i].size(); j++) {
                    parity.data[j] ^= group_buf_[i][j];
                }
            }
        }

        parity_packets_.push_back(std::move(parity));
    }
}

void FecEncoder::reset() {
    group_buf_.clear();
    parity_packets_.clear();
    group_id_ = 0;
    max_len_in_group_ = 0;
}

} // namespace soluna::wifi
