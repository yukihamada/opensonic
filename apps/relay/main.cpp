/**
 * soluna-relay — WAN Relay Server for Soluna Network Audio
 *
 * A standalone relay server that enables internet-based audio connections
 * with group names. Clients connect over UDP, join a named group, and
 * audio packets are forwarded to all other group members.
 *
 * Usage:
 *   soluna-relay [options]
 *
 * Options:
 *   --port PORT       UDP listen port (default: 5100)
 *   --max-groups N    Maximum concurrent groups (default: 100)
 *   --max-members N   Maximum members per group (default: 16)
 *   --stats-interval  Stats output interval in seconds (default: 30)
 *   --help            Show this help
 *
 * UDP Protocol:
 *   JOIN:<group>:<password>\n   — Join a group (password optional)
 *   HELLO\n                     — Heartbeat (keep-alive every 5s)
 *   <RTP/OSTP packet>           — Audio data (0x80 prefix), forwarded to group
 *
 * SPDX-License-Identifier: OpenSonic-Community-1.0
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>

// ── Configuration ─────────────────────────────────────────────────────────────

static constexpr uint16_t kDefaultPort       = 5100;
static constexpr size_t   kMaxPktSize        = 65536;
static constexpr int      kStaleTimeoutSec   = 15;   // member timeout
static constexpr int      kCleanupIntervalSec = 10;  // cleanup cycle
static constexpr size_t   kDefaultMaxGroups  = 100;
static constexpr size_t   kDefaultMaxMembers = 16;

// ── Data structures ───────────────────────────────────────────────────────────

struct Member {
    sockaddr_in addr;
    std::chrono::steady_clock::time_point last_seen;
    std::string device_name;  // from JOIN message (optional)
};

struct Group {
    std::string name;
    std::string password;     // empty = no auth
    std::vector<Member> members;
    std::chrono::steady_clock::time_point created;
    uint64_t packets_forwarded = 0;
    uint64_t bytes_forwarded   = 0;
};

// ── Global state ──────────────────────────────────────────────────────────────

static std::mutex           g_mutex;
static std::map<std::string, Group> g_groups;
static int                  g_udp_sock = -1;
static volatile bool        g_running  = true;

// Limits
static size_t g_max_groups  = kDefaultMaxGroups;
static size_t g_max_members = kDefaultMaxMembers;

// Stats counters
static uint64_t g_total_packets_rx  = 0;
static uint64_t g_total_packets_fwd = 0;
static uint64_t g_total_bytes_rx    = 0;
static uint64_t g_total_bytes_fwd   = 0;
static uint64_t g_total_joins       = 0;

// ── Helpers ───────────────────────────────────────────────────────────────────

static void handle_signal(int) { g_running = false; }

static bool addr_equal(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr &&
           a.sin_port == b.sin_port;
}

static std::string addr_str(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    char buf[64];
    snprintf(buf, sizeof(buf), "%s:%u", ip, ntohs(addr.sin_port));
    return buf;
}

// Find which group a member belongs to (by address).
// Returns group name or empty string. Caller must hold g_mutex.
static std::string find_member_group(const sockaddr_in& addr) {
    for (const auto& [name, group] : g_groups) {
        for (const auto& m : group.members) {
            if (addr_equal(m.addr, addr)) {
                return name;
            }
        }
    }
    return "";
}

// ── JOIN handler ──────────────────────────────────────────────────────────────
// Format: "JOIN:<group_name>\n" or "JOIN:<group_name>:<password>\n"
// Optionally: "JOIN:<group_name>:<password>:<device_name>\n"

static void handle_join(const char* msg, size_t len, const sockaddr_in& from) {
    // Parse: skip "JOIN:" prefix
    std::string payload(msg + 5, len - 5);
    // Strip trailing newline/whitespace
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    // Split by ':'
    std::string group_name, password, device_name;
    size_t pos1 = payload.find(':');
    if (pos1 == std::string::npos) {
        group_name = payload;
    } else {
        group_name = payload.substr(0, pos1);
        size_t pos2 = payload.find(':', pos1 + 1);
        if (pos2 == std::string::npos) {
            password = payload.substr(pos1 + 1);
        } else {
            password = payload.substr(pos1 + 1, pos2 - pos1 - 1);
            device_name = payload.substr(pos2 + 1);
        }
    }

    if (group_name.empty()) {
        const char* err = "ERR:empty_group\n";
        sendto(g_udp_sock, err, strlen(err), 0,
               (const sockaddr*)&from, sizeof(from));
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);

    // Remove member from any existing group first
    std::string old_group = find_member_group(from);
    if (!old_group.empty() && old_group != group_name) {
        auto it = g_groups.find(old_group);
        if (it != g_groups.end()) {
            auto& members = it->second.members;
            members.erase(
                std::remove_if(members.begin(), members.end(),
                    [&](const Member& m) { return addr_equal(m.addr, from); }),
                members.end());
            if (members.empty()) {
                fprintf(stderr, "[relay] Group '%s' dissolved (empty)\n", old_group.c_str());
                g_groups.erase(it);
            }
        }
    }

    auto now = std::chrono::steady_clock::now();

    // Find or create group
    auto it = g_groups.find(group_name);
    if (it == g_groups.end()) {
        // New group
        if (g_groups.size() >= g_max_groups) {
            const char* err = "ERR:max_groups\n";
            sendto(g_udp_sock, err, strlen(err), 0,
                   (const sockaddr*)&from, sizeof(from));
            return;
        }
        Group g;
        g.name = group_name;
        g.password = password;
        g.created = now;
        it = g_groups.emplace(group_name, std::move(g)).first;
        fprintf(stderr, "[relay] Group '%s' created%s\n",
                group_name.c_str(), password.empty() ? "" : " (password-protected)");
    } else {
        // Existing group — check password
        if (!it->second.password.empty() && it->second.password != password) {
            const char* err = "ERR:wrong_password\n";
            sendto(g_udp_sock, err, strlen(err), 0,
                   (const sockaddr*)&from, sizeof(from));
            return;
        }
    }

    auto& group = it->second;

    // Check if already a member (update last_seen)
    for (auto& m : group.members) {
        if (addr_equal(m.addr, from)) {
            m.last_seen = now;
            if (!device_name.empty()) m.device_name = device_name;
            // Send OK (re-join)
            const char* ok = "OK:joined\n";
            sendto(g_udp_sock, ok, strlen(ok), 0,
                   (const sockaddr*)&from, sizeof(from));
            return;
        }
    }

    // New member
    if (group.members.size() >= g_max_members) {
        const char* err = "ERR:group_full\n";
        sendto(g_udp_sock, err, strlen(err), 0,
               (const sockaddr*)&from, sizeof(from));
        return;
    }

    Member member;
    member.addr = from;
    member.last_seen = now;
    member.device_name = device_name;
    group.members.push_back(member);
    g_total_joins++;

    fprintf(stderr, "[relay] %s joined group '%s' (%zu members)%s\n",
            addr_str(from).c_str(), group_name.c_str(), group.members.size(),
            device_name.empty() ? "" : (" [" + device_name + "]").c_str());

    const char* ok = "OK:joined\n";
    sendto(g_udp_sock, ok, strlen(ok), 0,
           (const sockaddr*)&from, sizeof(from));
}

// ── HELLO handler ─────────────────────────────────────────────────────────────

static void handle_hello(const sockaddr_in& from) {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_mutex);

    for (auto& [name, group] : g_groups) {
        for (auto& m : group.members) {
            if (addr_equal(m.addr, from)) {
                m.last_seen = now;
                return;
            }
        }
    }
    // Unknown sender — ignore (they haven't JOINed)
}

// ── Audio forwarding ──────────────────────────────────────────────────────────
// Forward an audio packet to all group members except the sender.

static void forward_audio(const uint8_t* data, size_t len, const sockaddr_in& from) {
    std::lock_guard<std::mutex> lock(g_mutex);

    for (auto& [name, group] : g_groups) {
        bool sender_in_group = false;
        for (const auto& m : group.members) {
            if (addr_equal(m.addr, from)) {
                sender_in_group = true;
                break;
            }
        }
        if (!sender_in_group) continue;

        // Forward to all other members
        for (const auto& m : group.members) {
            if (addr_equal(m.addr, from)) continue;  // skip sender
            sendto(g_udp_sock, data, len, 0,
                   (const sockaddr*)&m.addr, sizeof(m.addr));
            g_total_packets_fwd++;
            g_total_bytes_fwd += len;
        }
        group.packets_forwarded++;
        group.bytes_forwarded += len * (group.members.size() > 1 ? group.members.size() - 1 : 0);
        return;  // member can only be in one group
    }
}

// ── Stale member cleanup ──────────────────────────────────────────────────────

static void cleanup_stale() {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_mutex);

    for (auto it = g_groups.begin(); it != g_groups.end(); ) {
        auto& members = it->second.members;
        size_t before = members.size();

        members.erase(
            std::remove_if(members.begin(), members.end(),
                [&](const Member& m) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        now - m.last_seen).count();
                    if (elapsed > kStaleTimeoutSec) {
                        fprintf(stderr, "[relay] %s timed out from group '%s'\n",
                                addr_str(m.addr).c_str(), it->first.c_str());
                        return true;
                    }
                    return false;
                }),
            members.end());

        if (members.empty() && before > 0) {
            fprintf(stderr, "[relay] Group '%s' dissolved (all members timed out)\n",
                    it->first.c_str());
            it = g_groups.erase(it);
        } else {
            ++it;
        }
    }
}

// ── Stats output ──────────────────────────────────────────────────────────────

static void print_stats() {
    std::lock_guard<std::mutex> lock(g_mutex);

    size_t total_members = 0;
    for (const auto& [name, group] : g_groups) {
        total_members += group.members.size();
    }

    fprintf(stderr,
        "\n[relay] ─── Stats ───────────────────────────────────\n"
        "[relay] Groups: %zu  Members: %zu  Total joins: %llu\n"
        "[relay] Packets RX: %llu  Forwarded: %llu\n"
        "[relay] Bytes RX: %llu  Forwarded: %llu\n",
        g_groups.size(), total_members,
        (unsigned long long)g_total_joins,
        (unsigned long long)g_total_packets_rx,
        (unsigned long long)g_total_packets_fwd,
        (unsigned long long)g_total_bytes_rx,
        (unsigned long long)g_total_bytes_fwd);

    for (const auto& [name, group] : g_groups) {
        fprintf(stderr, "[relay]   Group '%s': %zu members, %llu pkts fwd\n",
                name.c_str(), group.members.size(),
                (unsigned long long)group.packets_forwarded);
        for (const auto& m : group.members) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - m.last_seen).count();
            fprintf(stderr, "[relay]     %s  last_seen=%llds ago%s\n",
                    addr_str(m.addr).c_str(), (long long)age,
                    m.device_name.empty() ? "" : (" [" + m.device_name + "]").c_str());
        }
    }

    fprintf(stderr, "[relay] ─────────────────────────────────────────────\n\n");
}

// ── Main ──────────────────────────────────────────────────────────────────────

static void print_usage(const char* prog) {
    fprintf(stderr,
        "soluna-relay — WAN Relay Server for Soluna Network Audio\n\n"
        "Usage: %s [options]\n\n"
        "Options:\n"
        "  --port PORT          UDP listen port (default: %u)\n"
        "  --max-groups N       Maximum concurrent groups (default: %zu)\n"
        "  --max-members N      Maximum members per group (default: %zu)\n"
        "  --stats-interval S   Stats output interval in seconds (default: 30)\n"
        "  --help               Show this help\n\n"
        "UDP Protocol:\n"
        "  JOIN:<group>:<password>\\n   Join/create group\n"
        "  HELLO\\n                     Heartbeat (send every 5s)\n"
        "  <RTP/OSTP audio packet>     Forwarded to all group members\n\n",
        prog, kDefaultPort, kDefaultMaxGroups, kDefaultMaxMembers);
}

int main(int argc, char** argv) {
    uint16_t port = kDefaultPort;
    uint32_t stats_interval = 30;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if      (arg == "--port")           port = (uint16_t)atoi(next());
        else if (arg == "--max-groups")     g_max_groups = (size_t)atoi(next());
        else if (arg == "--max-members")    g_max_members = (size_t)atoi(next());
        else if (arg == "--stats-interval") stats_interval = (uint32_t)atoi(next());
        else if (arg == "--help") { print_usage(argv[0]); return 0; }
        else {
            fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            print_usage(argv[0]);
            return 1;
        }
    }

    // Create UDP socket
    g_udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_sock < 0) {
        perror("socket");
        return 1;
    }

    int reuse = 1;
    setsockopt(g_udp_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Set receive buffer to 2MB for high throughput
    int rcvbuf = 2 * 1024 * 1024;
    setsockopt(g_udp_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(port);

    if (bind(g_udp_sock, (const sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("bind");
        close(g_udp_sock);
        return 1;
    }

    // Set recv timeout for periodic cleanup
    timeval tv{1, 0};  // 1 second timeout
    setsockopt(g_udp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    char timeout_str[32];
    snprintf(timeout_str, sizeof(timeout_str), "%ds", kStaleTimeoutSec);
    fprintf(stderr,
        "╔═══════════════════════════════════════════════════╗\n"
        "║       soluna-relay — WAN Audio Relay Server       ║\n"
        "╠═══════════════════════════════════════════════════╣\n"
        "║  UDP port:      %-33u ║\n"
        "║  Max groups:    %-33zu ║\n"
        "║  Max members:   %-33zu ║\n"
        "║  Stale timeout: %-33s ║\n"
        "╚═══════════════════════════════════════════════════╝\n",
        port, g_max_groups, g_max_members, timeout_str);

    auto last_cleanup = std::chrono::steady_clock::now();
    auto last_stats   = std::chrono::steady_clock::now();

    static uint8_t pkt[kMaxPktSize];

    while (g_running) {
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(g_udp_sock, pkt, sizeof(pkt), 0,
                             (sockaddr*)&from, &from_len);

        auto now = std::chrono::steady_clock::now();

        if (n > 0) {
            g_total_packets_rx++;
            g_total_bytes_rx += (uint64_t)n;

            // Classify packet
            if (n >= 5 && memcmp(pkt, "JOIN:", 5) == 0) {
                // JOIN message
                handle_join((const char*)pkt, (size_t)n, from);
            } else if (n >= 5 && memcmp(pkt, "HELLO", 5) == 0) {
                // Heartbeat
                handle_hello(from);
            } else if (n >= 12 && (pkt[0] & 0xC0) == 0x80) {
                // RTP/OSTP audio packet (version bits = 2, i.e. 0x80)
                forward_audio(pkt, (size_t)n, from);
            }
            // else: unknown packet, silently drop
        }

        // Periodic cleanup
        auto cleanup_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_cleanup).count();
        if (cleanup_elapsed >= kCleanupIntervalSec) {
            cleanup_stale();
            last_cleanup = now;
        }

        // Periodic stats
        auto stats_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_stats).count();
        if (stats_interval > 0 && stats_elapsed >= (long long)stats_interval) {
            print_stats();
            last_stats = now;
        }
    }

    fprintf(stderr, "\n[relay] Shutting down...\n");
    print_stats();
    close(g_udp_sock);
    return 0;
}
