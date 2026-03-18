/**
 * Soluna — License Key Validation
 * SPDX-License-Identifier: OpenSonic-Community-1.0
 */

#include <soluna/security/license.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#ifdef SOLUNA_HAS_TLS
#include <openssl/hmac.h>
#include <openssl/evp.h>
#endif

namespace soluna::security {

// --- Base64 decode (minimal) ---
static const uint8_t b64_table[] = {
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,62,64,62,64,63,
    52,53,54,55,56,57,58,59,60,61,64,64,64,0,64,64,
    64, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,63,
    64,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,64,64,64,64,64
};

static std::string base64_decode(const std::string& in) {
    std::string out;
    uint32_t val = 0;
    int bits = -8;
    for (unsigned char c : in) {
        if (c == '=' || c >= 128) continue;
        uint8_t v = b64_table[c];
        if (v >= 64) continue;
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

// --- Simple JSON parser (just what we need) ---
static std::string json_get_string(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    auto end = json.find('"', pos + 1);
    if (end == std::string::npos) return "";
    return json.substr(pos + 1, end - pos - 1);
}

static uint32_t json_get_uint(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return 0;
    pos++;
    while (pos < json.size() && json[pos] == ' ') pos++;
    return static_cast<uint32_t>(std::strtoul(json.c_str() + pos, nullptr, 10));
}

static bool json_get_bool(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return false;
    return json.find("true", pos) == pos + 1 || json.find("true", pos) < pos + 6;
}

// Embedded public verification key (HMAC secret).
// In production, this would be derived differently, but for the
// OpenSonic license system we use a known key for validation.
static const char* LICENSE_HMAC_KEY = "opensonic-license-v1-hmac-secret";

static std::string hex_encode(const uint8_t* data, size_t len) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        out.push_back(hex[data[i] >> 4]);
        out.push_back(hex[data[i] & 0xF]);
    }
    return out;
}

LicenseInfo validate_license_key(const std::string& key) {
    LicenseInfo info;

    if (key.empty()) return info;

    // Format: base64_payload.hex_signature
    auto dot = key.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot == key.size() - 1) {
        return info;
    }

    std::string payload_b64 = key.substr(0, dot);
    std::string sig_hex = key.substr(dot + 1);

    // Decode payload
    std::string payload = base64_decode(payload_b64);
    if (payload.empty()) return info;

#ifdef SOLUNA_HAS_TLS
    // Verify HMAC-SHA256 signature
    uint8_t hmac_result[32];
    unsigned int hmac_len = 0;
    HMAC(EVP_sha256(),
         LICENSE_HMAC_KEY, static_cast<int>(std::strlen(LICENSE_HMAC_KEY)),
         reinterpret_cast<const uint8_t*>(payload_b64.c_str()),
         payload_b64.size(),
         hmac_result, &hmac_len);

    std::string expected_sig = hex_encode(hmac_result, hmac_len);
    if (sig_hex.size() != expected_sig.size()) return info;

    // Constant-time comparison
    uint8_t diff = 0;
    for (size_t i = 0; i < sig_hex.size(); i++) {
        diff |= static_cast<uint8_t>(sig_hex[i]) ^ static_cast<uint8_t>(expected_sig[i]);
    }
    if (diff != 0) return info;
#else
    // Without TLS/OpenSSL, reject all non-Free licenses (signature cannot be verified)
    fprintf(stderr, "License: signature verification unavailable (no OpenSSL) — rejecting key\n");
    return info;
#endif

    // Parse JSON payload
    std::string tier = json_get_string(payload, "tier");
    info.max_participants = json_get_uint(payload, "max_participants");
    info.licensee = json_get_string(payload, "licensee");
    info.expires = json_get_string(payload, "expires");
    info.annual = json_get_bool(payload, "annual");

    if (tier == "S" || tier == "small") {
        info.tier = LicenseTier::Small;
        if (info.max_participants == 0) info.max_participants = 5000;
    } else if (tier == "M" || tier == "medium") {
        info.tier = LicenseTier::Medium;
        if (info.max_participants == 0) info.max_participants = 10000;
    } else if (tier == "L" || tier == "large") {
        info.tier = LicenseTier::Large;
        if (info.max_participants == 0) info.max_participants = 100000;
    } else {
        return info; // Invalid tier
    }

    info.valid = true;
    return info;
}

bool is_license_active(const LicenseInfo& info) {
    if (!info.valid) return false;
    if (info.expires.empty()) return true;

    // Parse YYYY-MM-DD
    if (info.expires.size() < 10) return false;
    int year = std::atoi(info.expires.c_str());
    int month = std::atoi(info.expires.c_str() + 5);
    int day = std::atoi(info.expires.c_str() + 8);

    time_t now = time(nullptr);
    struct tm* tm = localtime(&now);
    int cur_year = tm->tm_year + 1900;
    int cur_month = tm->tm_mon + 1;
    int cur_day = tm->tm_mday;

    if (cur_year < year) return true;
    if (cur_year > year) return false;
    if (cur_month < month) return true;
    if (cur_month > month) return false;
    return cur_day <= day;
}

const char* tier_name(LicenseTier tier) {
    switch (tier) {
        case LicenseTier::Free:   return "Free";
        case LicenseTier::Small:  return "S (1,001-5,000)";
        case LicenseTier::Medium: return "M (5,001-10,000)";
        case LicenseTier::Large:  return "L (10,001+)";
    }
    return "Unknown";
}

std::string load_license_key() {
    // 1. Environment variable
    const char* env = std::getenv("SOLUNA_LICENSE_KEY");
    if (env && *env) return env;

    // 2. ~/.config/soluna/license.key
    const char* home = std::getenv("HOME");
    if (home) {
        std::string path = std::string(home) + "/.config/soluna/license.key";
        std::ifstream f(path);
        if (f.good()) {
            std::string key;
            std::getline(f, key);
            key.erase(std::remove(key.begin(), key.end(), '\n'), key.end());
            key.erase(std::remove(key.begin(), key.end(), '\r'), key.end());
            if (!key.empty()) return key;
        }
    }

    // 3. /etc/soluna/license.key
    {
        std::ifstream f("/etc/soluna/license.key");
        if (f.good()) {
            std::string key;
            std::getline(f, key);
            key.erase(std::remove(key.begin(), key.end(), '\n'), key.end());
            key.erase(std::remove(key.begin(), key.end(), '\r'), key.end());
            if (!key.empty()) return key;
        }
    }

    return "";
}

} // namespace soluna::security
