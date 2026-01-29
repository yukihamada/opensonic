/**
 * Preset Manager — Routing configuration persistence
 *
 * Saves and loads routing presets as JSON files.
 * Uses hand-rolled JSON since Phase 3 doesn't have nlohmann/json yet.
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/control/preset_manager.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <sys/stat.h>
#include <dirent.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif

namespace soluna::control {

namespace {

std::string get_home_directory() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    return home ? home : "";
}

std::string get_default_preset_directory() {
    std::string home = get_home_directory();
    if (home.empty()) return "";
#ifdef _WIN32
    return home + "\\.soluna\\presets";
#else
    return home + "/.soluna/presets";
#endif
}

bool directory_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool create_directory_recursive(const std::string& path) {
    if (directory_exists(path)) return true;

    // Find parent directory
    size_t pos = path.find_last_of("/\\");
    if (pos != std::string::npos && pos > 0) {
        std::string parent = path.substr(0, pos);
        if (!create_directory_recursive(parent)) return false;
    }

#ifdef _WIN32
    return _mkdir(path.c_str()) == 0;
#else
    return mkdir(path.c_str(), 0755) == 0;
#endif
}

int64_t get_file_mtime(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return static_cast<int64_t>(st.st_mtime);
    }
    return 0;
}

// Simple JSON escape
std::string json_escape(const std::string& s) {
    std::string result;
    result.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:   result += c; break;
        }
    }
    return result;
}

// Simple JSON string extraction
std::string extract_json_string(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

// Extract number from JSON
double extract_json_number(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return 0.0;
    pos += search.size();
    // Skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    std::string num;
    while (pos < json.size() && (std::isdigit(json[pos]) || json[pos] == '.' ||
           json[pos] == '-' || json[pos] == '+' || json[pos] == 'e' || json[pos] == 'E')) {
        num += json[pos++];
    }
    return num.empty() ? 0.0 : std::stod(num);
}

// Extract boolean from JSON
bool extract_json_bool(const std::string& json, const std::string& key) {
    std::string search_true = "\"" + key + "\":true";
    return json.find(search_true) != std::string::npos;
}

} // anonymous namespace

PresetManager::PresetManager(const std::string& directory)
    : directory_(directory.empty() ? get_default_preset_directory() : directory)
{
}

bool PresetManager::ensure_directory() const {
    return create_directory_recursive(directory_);
}

std::string PresetManager::sanitize_name(const std::string& name) const {
    std::string result;
    result.reserve(name.size());
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
            result += c;
        } else if (c == ' ') {
            result += '_';
        }
        // Skip other characters
    }
    if (result.empty()) result = "preset";
    return result;
}

std::string PresetManager::preset_path(const std::string& name) const {
    return directory_ + "/" + sanitize_name(name) + ".json";
}

std::vector<PresetInfo> PresetManager::list() const {
    std::vector<PresetInfo> presets;

    if (!directory_exists(directory_)) return presets;

    DIR* dir = opendir(directory_.c_str());
    if (!dir) return presets;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string filename = entry->d_name;
        if (filename.size() > 5 && filename.substr(filename.size() - 5) == ".json") {
            std::string name = filename.substr(0, filename.size() - 5);
            std::string path = directory_ + "/" + filename;

            PresetInfo info;
            info.name = name;
            info.filename = filename;
            info.modified_time = get_file_mtime(path);

            // Count routes by reading the file
            std::ifstream file(path);
            if (file) {
                std::string content((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
                // Count occurrences of "source" key as proxy for route count
                size_t count = 0;
                size_t pos = 0;
                while ((pos = content.find("\"source\":", pos)) != std::string::npos) {
                    count++;
                    pos++;
                }
                info.route_count = count;
            }

            presets.push_back(info);
        }
    }

    closedir(dir);

    // Sort by modification time (newest first)
    std::sort(presets.begin(), presets.end(), [](const PresetInfo& a, const PresetInfo& b) {
        return a.modified_time > b.modified_time;
    });

    return presets;
}

bool PresetManager::save(const std::string& name, const RoutingMatrix& routing) {
    if (!ensure_directory()) return false;

    std::string path = preset_path(name);
    std::ofstream file(path);
    if (!file) return false;

    auto routes = routing.list_routes();

    // Build JSON
    std::ostringstream json;
    json << "{\n";
    json << "  \"name\": \"" << json_escape(name) << "\",\n";
    json << "  \"version\": 1,\n";
    json << "  \"routes\": [\n";

    for (size_t i = 0; i < routes.size(); i++) {
        const auto& r = routes[i];
        json << "    {\n";
        json << "      \"source\": \"" << json_escape(r.source.to_string()) << "\",\n";
        json << "      \"sink\": \"" << json_escape(r.sink.to_string()) << "\",\n";
        json << "      \"gain_db\": " << r.gain_db << ",\n";
        json << "      \"muted\": " << (r.muted ? "true" : "false") << "\n";
        json << "    }";
        if (i + 1 < routes.size()) json << ",";
        json << "\n";
    }

    json << "  ]\n";
    json << "}\n";

    file << json.str();
    return file.good();
}

bool PresetManager::load(const std::string& name, RoutingMatrix& routing) {
    std::string path = preset_path(name);
    std::ifstream file(path);
    if (!file) return false;

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    // Clear existing routes
    auto existing = routing.list_routes();
    for (const auto& r : existing) {
        routing.remove_route(r.source, r.sink);
    }

    // Parse routes array
    // Find "routes": [ ... ]
    size_t routes_start = content.find("\"routes\":");
    if (routes_start == std::string::npos) return false;

    size_t array_start = content.find('[', routes_start);
    if (array_start == std::string::npos) return false;

    size_t array_end = content.rfind(']');
    if (array_end == std::string::npos || array_end <= array_start) return false;

    std::string routes_json = content.substr(array_start + 1, array_end - array_start - 1);

    // Parse each route object
    size_t pos = 0;
    while (pos < routes_json.size()) {
        size_t obj_start = routes_json.find('{', pos);
        if (obj_start == std::string::npos) break;

        size_t obj_end = routes_json.find('}', obj_start);
        if (obj_end == std::string::npos) break;

        std::string route_json = routes_json.substr(obj_start, obj_end - obj_start + 1);

        std::string source = extract_json_string(route_json, "source");
        std::string sink = extract_json_string(route_json, "sink");
        float gain = static_cast<float>(extract_json_number(route_json, "gain_db"));
        bool muted = extract_json_bool(route_json, "muted");

        if (!source.empty() && !sink.empty()) {
            routing.add_route(ChannelId::parse(source), ChannelId::parse(sink), gain);
            if (muted) {
                routing.set_mute(ChannelId::parse(source), ChannelId::parse(sink), true);
            }
        }

        pos = obj_end + 1;
    }

    return true;
}

bool PresetManager::remove(const std::string& name) {
    std::string path = preset_path(name);
    return std::remove(path.c_str()) == 0;
}

bool PresetManager::exists(const std::string& name) const {
    std::string path = preset_path(name);
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

} // namespace soluna::control
