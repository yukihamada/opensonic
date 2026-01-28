/**
 * Soluna — DSP Processing Chain Implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/pipeline/dsp_chain.h>
#include <algorithm>

namespace soluna {
namespace pipeline {

struct DspChain::Impl {
    std::vector<std::unique_ptr<DspNode>> nodes;
    uint32_t sample_rate = 48000;
    uint32_t channels = 2;
    uint32_t block_size = 256;
    bool chain_bypassed = false;
    mutable std::mutex mutex;
};

DspChain::DspChain() : impl_(std::make_unique<Impl>()) {}
DspChain::~DspChain() = default;

void DspChain::init(uint32_t sample_rate, uint32_t channels, uint32_t block_size) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->sample_rate = sample_rate;
    impl_->channels = channels;
    impl_->block_size = block_size;

    // Initialize all existing plugins
    for (auto& node : impl_->nodes) {
        if (node && node->plugin) {
            node->plugin->init(sample_rate, channels);
        }
    }
}

int DspChain::add_plugin(const std::string& name, std::unique_ptr<DspPlugin> plugin) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    // Check for duplicate name
    for (const auto& node : impl_->nodes) {
        if (node && node->name == name) {
            return -1;
        }
    }

    auto node = std::make_unique<DspNode>();
    node->name = name;
    node->plugin = std::move(plugin);
    node->bypassed = false;
    node->input_gain = 1.0f;
    node->output_gain = 1.0f;

    // Initialize the plugin
    if (node->plugin) {
        node->plugin->init(impl_->sample_rate, impl_->channels);
    }

    impl_->nodes.push_back(std::move(node));
    return static_cast<int>(impl_->nodes.size() - 1);
}

int DspChain::insert_plugin(size_t index, const std::string& name,
                            std::unique_ptr<DspPlugin> plugin) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    if (index > impl_->nodes.size()) {
        return -1;
    }

    // Check for duplicate name
    for (const auto& node : impl_->nodes) {
        if (node && node->name == name) {
            return -1;
        }
    }

    auto node = std::make_unique<DspNode>();
    node->name = name;
    node->plugin = std::move(plugin);
    node->bypassed = false;
    node->input_gain = 1.0f;
    node->output_gain = 1.0f;

    // Initialize the plugin
    if (node->plugin) {
        node->plugin->init(impl_->sample_rate, impl_->channels);
    }

    impl_->nodes.insert(impl_->nodes.begin() + index, std::move(node));
    return static_cast<int>(index);
}

bool DspChain::remove_plugin(const std::string& name) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    for (auto it = impl_->nodes.begin(); it != impl_->nodes.end(); ++it) {
        if (*it && (*it)->name == name) {
            impl_->nodes.erase(it);
            return true;
        }
    }
    return false;
}

bool DspChain::remove_plugin(size_t index) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    if (index >= impl_->nodes.size()) {
        return false;
    }

    impl_->nodes.erase(impl_->nodes.begin() + index);
    return true;
}

bool DspChain::move_plugin(size_t from_index, size_t to_index) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    if (from_index >= impl_->nodes.size() || to_index >= impl_->nodes.size()) {
        return false;
    }

    if (from_index == to_index) {
        return true;
    }

    auto node = std::move(impl_->nodes[from_index]);
    impl_->nodes.erase(impl_->nodes.begin() + from_index);

    if (to_index > from_index) {
        to_index--;  // Adjust for the removal
    }

    impl_->nodes.insert(impl_->nodes.begin() + to_index, std::move(node));
    return true;
}

size_t DspChain::size() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->nodes.size();
}

DspPlugin* DspChain::get_plugin(size_t index) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    if (index >= impl_->nodes.size()) {
        return nullptr;
    }

    return impl_->nodes[index] ? impl_->nodes[index]->plugin.get() : nullptr;
}

DspPlugin* DspChain::get_plugin(const std::string& name) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    for (const auto& node : impl_->nodes) {
        if (node && node->name == name) {
            return node->plugin.get();
        }
    }
    return nullptr;
}

int DspChain::find_plugin(const std::string& name) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    for (size_t i = 0; i < impl_->nodes.size(); i++) {
        if (impl_->nodes[i] && impl_->nodes[i]->name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const DspNode* DspChain::get_node(size_t index) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    if (index >= impl_->nodes.size()) {
        return nullptr;
    }

    return impl_->nodes[index].get();
}

bool DspChain::set_bypass(const std::string& name, bool bypassed) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    for (auto& node : impl_->nodes) {
        if (node && node->name == name) {
            node->bypassed = bypassed;
            return true;
        }
    }
    return false;
}

bool DspChain::set_bypass(size_t index, bool bypassed) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    if (index >= impl_->nodes.size() || !impl_->nodes[index]) {
        return false;
    }

    impl_->nodes[index]->bypassed = bypassed;
    return true;
}

bool DspChain::is_bypassed(const std::string& name) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    for (const auto& node : impl_->nodes) {
        if (node && node->name == name) {
            return node->bypassed;
        }
    }
    return true;  // Not found = bypassed
}

bool DspChain::is_bypassed(size_t index) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    if (index >= impl_->nodes.size() || !impl_->nodes[index]) {
        return true;
    }

    return impl_->nodes[index]->bypassed;
}

bool DspChain::set_input_gain(const std::string& name, float gain) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    for (auto& node : impl_->nodes) {
        if (node && node->name == name) {
            node->input_gain = gain;
            return true;
        }
    }
    return false;
}

bool DspChain::set_input_gain(size_t index, float gain) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    if (index >= impl_->nodes.size() || !impl_->nodes[index]) {
        return false;
    }

    impl_->nodes[index]->input_gain = gain;
    return true;
}

bool DspChain::set_output_gain(const std::string& name, float gain) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    for (auto& node : impl_->nodes) {
        if (node && node->name == name) {
            node->output_gain = gain;
            return true;
        }
    }
    return false;
}

bool DspChain::set_output_gain(size_t index, float gain) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    if (index >= impl_->nodes.size() || !impl_->nodes[index]) {
        return false;
    }

    impl_->nodes[index]->output_gain = gain;
    return true;
}

void DspChain::set_chain_bypass(bool bypassed) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->chain_bypassed = bypassed;
}

bool DspChain::is_chain_bypassed() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->chain_bypassed;
}

void DspChain::process(float* buffer, size_t frame_count) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    if (impl_->chain_bypassed) {
        return;  // Pass through unchanged
    }

    size_t sample_count = frame_count * impl_->channels;

    for (auto& node : impl_->nodes) {
        if (!node || !node->plugin || node->bypassed) {
            continue;
        }

        // Apply input gain
        if (node->input_gain != 1.0f) {
            for (size_t i = 0; i < sample_count; i++) {
                buffer[i] *= node->input_gain;
            }
        }

        // Process through plugin
        node->plugin->process(buffer, frame_count, impl_->channels);

        // Apply output gain
        if (node->output_gain != 1.0f) {
            for (size_t i = 0; i < sample_count; i++) {
                buffer[i] *= node->output_gain;
            }
        }
    }
}

void DspChain::reset() {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    for (auto& node : impl_->nodes) {
        if (node && node->plugin) {
            node->plugin->reset();
        }
    }
}

size_t DspChain::get_latency() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    size_t total = 0;
    for (const auto& node : impl_->nodes) {
        if (node && !node->bypassed) {
            // Use node's latency field (set by caller if known)
            total += node->latency;
        }
    }
    return total;
}

std::vector<std::string> DspChain::get_plugin_names() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    std::vector<std::string> names;
    names.reserve(impl_->nodes.size());

    for (const auto& node : impl_->nodes) {
        if (node) {
            names.push_back(node->name);
        }
    }

    return names;
}

void DspChain::clear() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->nodes.clear();
}

} // namespace pipeline
} // namespace soluna
