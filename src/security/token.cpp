/**
 * Soluna — Token Utilities Implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/security/token.h>
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>

// Use OpenSSL if available, otherwise fallback to simple implementation
#ifdef SOLUNA_HAS_DTLS
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#define HAS_OPENSSL 1
#else
#define HAS_OPENSSL 0
#endif

namespace soluna {
namespace security {

// Simple SHA-256 implementation (fallback when OpenSSL not available)
#if !HAS_OPENSSL

// Minimal SHA-256 implementation
namespace {

static const uint32_t k[] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
inline uint32_t sig0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
inline uint32_t sig1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
inline uint32_t ep0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
inline uint32_t ep1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

std::string sha256_internal(const std::string& input) {
    uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    // Padding
    size_t orig_len = input.size();
    size_t padded_len = ((orig_len + 9 + 63) / 64) * 64;
    std::vector<uint8_t> msg(padded_len, 0);
    memcpy(msg.data(), input.data(), orig_len);
    msg[orig_len] = 0x80;

    uint64_t bit_len = orig_len * 8;
    for (int i = 0; i < 8; i++) {
        msg[padded_len - 1 - i] = static_cast<uint8_t>(bit_len >> (i * 8));
    }

    // Process blocks
    for (size_t i = 0; i < padded_len; i += 64) {
        uint32_t w[64];
        for (int j = 0; j < 16; j++) {
            w[j] = (msg[i + j * 4] << 24) | (msg[i + j * 4 + 1] << 16) |
                   (msg[i + j * 4 + 2] << 8) | msg[i + j * 4 + 3];
        }
        for (int j = 16; j < 64; j++) {
            w[j] = ep1(w[j - 2]) + w[j - 7] + ep0(w[j - 15]) + w[j - 16];
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

        for (int j = 0; j < 64; j++) {
            uint32_t t1 = hh + sig1(e) + ch(e, f, g) + k[j] + w[j];
            uint32_t t2 = sig0(a) + maj(a, b, c);
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    std::string result(32, 0);
    for (int i = 0; i < 8; i++) {
        result[i * 4] = static_cast<char>(h[i] >> 24);
        result[i * 4 + 1] = static_cast<char>(h[i] >> 16);
        result[i * 4 + 2] = static_cast<char>(h[i] >> 8);
        result[i * 4 + 3] = static_cast<char>(h[i]);
    }
    return result;
}

} // anonymous namespace

#endif

std::string Token::generate(size_t length) {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

    std::string result;
    result.reserve(length);

#if HAS_OPENSSL
    std::vector<uint8_t> bytes(length);
    RAND_bytes(bytes.data(), static_cast<int>(length));
    for (size_t i = 0; i < length; i++) {
        result += charset[bytes[i] % (sizeof(charset) - 1)];
    }
#else
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);
    for (size_t i = 0; i < length; i++) {
        result += charset[dist(gen)];
    }
#endif

    return result;
}

std::string Token::generate_base64(size_t bytes) {
    std::string raw;
    raw.resize(bytes);

#if HAS_OPENSSL
    RAND_bytes(reinterpret_cast<unsigned char*>(&raw[0]), static_cast<int>(bytes));
#else
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, 255);
    for (size_t i = 0; i < bytes; i++) {
        raw[i] = static_cast<char>(dist(gen));
    }
#endif

    return base64url_encode(raw);
}

std::string Token::sha256(const std::string& input) {
#if HAS_OPENSSL
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()),
           input.size(), hash);
    return std::string(reinterpret_cast<char*>(hash), SHA256_DIGEST_LENGTH);
#else
    return sha256_internal(input);
#endif
}

std::string Token::sha256_hex(const std::string& input) {
    std::string hash = sha256(input);
    std::ostringstream ss;
    for (unsigned char c : hash) {
        ss << std::hex << std::setfill('0') << std::setw(2)
           << static_cast<int>(static_cast<unsigned char>(c));
    }
    return ss.str();
}

std::string Token::hmac_sha256(const std::string& key, const std::string& message) {
#if HAS_OPENSSL
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int result_len;
    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(message.data()),
         message.size(), result, &result_len);
    return std::string(reinterpret_cast<char*>(result), result_len);
#else
    // Simplified HMAC-SHA256 (not constant-time)
    const size_t block_size = 64;
    std::string k = key;

    if (k.size() > block_size) {
        k = sha256(k);
    }
    if (k.size() < block_size) {
        k.resize(block_size, 0);
    }

    std::string ipad(block_size, 0x36);
    std::string opad(block_size, 0x5c);

    for (size_t i = 0; i < block_size; i++) {
        ipad[i] ^= k[i];
        opad[i] ^= k[i];
    }

    return sha256(opad + sha256(ipad + message));
#endif
}

static const char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const char base64url_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string Token::base64_encode(const std::string& input) {
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);

    for (size_t i = 0; i < input.size(); i += 3) {
        uint32_t n = (static_cast<uint8_t>(input[i]) << 16);
        if (i + 1 < input.size()) n |= (static_cast<uint8_t>(input[i + 1]) << 8);
        if (i + 2 < input.size()) n |= static_cast<uint8_t>(input[i + 2]);

        output += base64_chars[(n >> 18) & 0x3f];
        output += base64_chars[(n >> 12) & 0x3f];
        output += (i + 1 < input.size()) ? base64_chars[(n >> 6) & 0x3f] : '=';
        output += (i + 2 < input.size()) ? base64_chars[n & 0x3f] : '=';
    }

    return output;
}

std::string Token::base64_decode(const std::string& input) {
    std::string output;
    output.reserve((input.size() / 4) * 3);

    auto decode_char = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    for (size_t i = 0; i < input.size(); i += 4) {
        int a = decode_char(input[i]);
        int b = decode_char(input[i + 1]);
        int c = (i + 2 < input.size() && input[i + 2] != '=') ? decode_char(input[i + 2]) : 0;
        int d = (i + 3 < input.size() && input[i + 3] != '=') ? decode_char(input[i + 3]) : 0;

        output += static_cast<char>((a << 2) | (b >> 4));
        if (i + 2 < input.size() && input[i + 2] != '=') {
            output += static_cast<char>(((b & 0x0f) << 4) | (c >> 2));
        }
        if (i + 3 < input.size() && input[i + 3] != '=') {
            output += static_cast<char>(((c & 0x03) << 6) | d);
        }
    }

    return output;
}

std::string Token::base64url_encode(const std::string& input) {
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);

    for (size_t i = 0; i < input.size(); i += 3) {
        uint32_t n = (static_cast<uint8_t>(input[i]) << 16);
        if (i + 1 < input.size()) n |= (static_cast<uint8_t>(input[i + 1]) << 8);
        if (i + 2 < input.size()) n |= static_cast<uint8_t>(input[i + 2]);

        output += base64url_chars[(n >> 18) & 0x3f];
        output += base64url_chars[(n >> 12) & 0x3f];
        if (i + 1 < input.size()) output += base64url_chars[(n >> 6) & 0x3f];
        if (i + 2 < input.size()) output += base64url_chars[n & 0x3f];
    }

    return output;
}

std::string Token::base64url_decode(const std::string& input) {
    std::string output;
    output.reserve((input.size() / 4) * 3 + 3);

    auto decode_char = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };

    size_t len = input.size();
    size_t i = 0;

    while (i + 1 < len) {
        int a = decode_char(input[i]);
        int b = decode_char(input[i + 1]);
        int c = (i + 2 < len) ? decode_char(input[i + 2]) : -1;
        int d = (i + 3 < len) ? decode_char(input[i + 3]) : -1;

        if (a < 0 || b < 0) break;

        output += static_cast<char>((a << 2) | (b >> 4));

        if (c >= 0) {
            output += static_cast<char>(((b & 0x0f) << 4) | (c >> 2));
            if (d >= 0) {
                output += static_cast<char>(((c & 0x03) << 6) | d);
            }
        }

        i += 4;
    }

    return output;
}

bool Token::secure_compare(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }

    volatile uint8_t result = 0;
    for (size_t i = 0; i < a.size(); i++) {
        result |= static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i]);
    }
    return result == 0;
}

std::string Token::generate_nonce() {
    return generate_base64(24);
}

} // namespace security
} // namespace soluna
