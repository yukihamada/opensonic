/**
 * DSP Plugin Host — Dynamic plugin loading
 * SPDX-License-Identifier: MIT
 */

#include <soluna/pipeline/dsp_plugin.h>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace soluna::pipeline {

PluginHost::PluginHost() = default;

PluginHost::~PluginHost() {
    unload_all();
}

bool PluginHost::load(const std::string& path) {
    LoadedPlugin lp;
    lp.path = path;

#ifdef _WIN32
    lp.handle = LoadLibraryA(path.c_str());
    if (!lp.handle) {
        fprintf(stderr, "Plugin load failed: %s (error %lu)\n",
                path.c_str(), GetLastError());
        return false;
    }

    auto create_fn = reinterpret_cast<PluginCreateFunc>(
        GetProcAddress(static_cast<HMODULE>(lp.handle), "soluna_plugin_create"));
    lp.destroy_fn = reinterpret_cast<PluginDestroyFunc>(
        GetProcAddress(static_cast<HMODULE>(lp.handle), "soluna_plugin_destroy"));
#else
    lp.handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lp.handle) {
        fprintf(stderr, "Plugin load failed: %s (%s)\n", path.c_str(), dlerror());
        return false;
    }

    auto create_fn = reinterpret_cast<PluginCreateFunc>(
        dlsym(lp.handle, "soluna_plugin_create"));
    lp.destroy_fn = reinterpret_cast<PluginDestroyFunc>(
        dlsym(lp.handle, "soluna_plugin_destroy"));
#endif

    if (!create_fn || !lp.destroy_fn) {
        fprintf(stderr, "Plugin missing entry points: %s\n", path.c_str());
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(lp.handle));
#else
        dlclose(lp.handle);
#endif
        return false;
    }

    lp.instance = create_fn();
    if (!lp.instance) {
        fprintf(stderr, "Plugin create returned null: %s\n", path.c_str());
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(lp.handle));
#else
        dlclose(lp.handle);
#endif
        return false;
    }

    plugins_.push_back(std::move(lp));
    return true;
}

void PluginHost::unload_all() {
    for (auto& lp : plugins_) {
        if (lp.instance && lp.destroy_fn) {
            lp.destroy_fn(lp.instance);
        }
        if (lp.handle) {
#ifdef _WIN32
            FreeLibrary(static_cast<HMODULE>(lp.handle));
#else
            dlclose(lp.handle);
#endif
        }
    }
    plugins_.clear();
}

bool PluginHost::init_all(uint32_t sample_rate, uint32_t channels) {
    for (auto& lp : plugins_) {
        if (lp.instance && !lp.instance->init(sample_rate, channels)) {
            fprintf(stderr, "Plugin init failed: %s\n", lp.instance->name());
            return false;
        }
    }
    return true;
}

void PluginHost::process_all(float* buffer, size_t frames, uint32_t channels) {
    for (auto& lp : plugins_) {
        if (lp.instance) {
            lp.instance->process(buffer, frames, channels);
        }
    }
}

size_t PluginHost::plugin_count() const {
    return plugins_.size();
}

DspPlugin* PluginHost::plugin(size_t index) const {
    if (index >= plugins_.size()) return nullptr;
    return plugins_[index].instance;
}

} // namespace soluna::pipeline
