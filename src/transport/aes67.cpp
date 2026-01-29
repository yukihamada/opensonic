/**
 * AES67 Compatibility — SAP/SDP announcer and standard RTP builder
 * SPDX-License-Identifier: MIT
 */

#include <soluna/transport/aes67.h>
#include <soluna/pal/net.h>
#include <soluna/pal/thread.h>
#include <cstring>
#include <sstream>
#include <atomic>
#include <map>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace soluna::transport {

std::string aes67_generate_sdp(const Aes67Session& session) {
    std::ostringstream sdp;

    // SDP v0 (RFC 4566)
    sdp << "v=0\r\n";

    // o=<username> <sess-id> <sess-version> IN IP4 <origin-addr>
    sdp << "o=- " << session.session_id
        << " " << session.session_version
        << " IN IP4 " << session.origin_address << "\r\n";

    // s=<session name>
    sdp << "s=" << session.session_name << "\r\n";

    // c=IN IP4 <multicast-group>/32
    sdp << "c=IN IP4 " << session.multicast_group << "/32\r\n";

    // t=0 0 (permanent session)
    sdp << "t=0 0\r\n";

    // m=audio <port> RTP/AVP <payload-type>
    sdp << "m=audio " << session.rtp_port
        << " RTP/AVP " << static_cast<int>(session.payload_type) << "\r\n";

    // a=rtpmap:<pt> L24/<rate>/<channels> or L16/<rate>/<channels>
    const char* encoding = (session.bit_depth == 16) ? "L16" : "L24";
    sdp << "a=rtpmap:" << static_cast<int>(session.payload_type)
        << " " << encoding << "/" << session.sample_rate;
    if (session.channels > 1) {
        sdp << "/" << session.channels;
    }
    sdp << "\r\n";

    // a=ptime:<packet time in ms>
    double ptime_ms = session.packet_time_us / 1000.0;
    sdp << "a=ptime:" << ptime_ms << "\r\n";

    // a=ts-refclk:ptp=IEEE1588-2008 (AES67 requirement)
    sdp << "a=ts-refclk:ptp=IEEE1588-2008\r\n";

    // a=mediaclk:direct=0
    sdp << "a=mediaclk:direct=0\r\n";

    return sdp.str();
}

uint16_t aes67_sap_hash(const std::string& sdp) {
    // FNV-1a hash for better distribution (RFC 2974 suggests CRC or similar)
    // Using 32-bit FNV-1a then folding to 16-bit
    constexpr uint32_t kFnvPrime = 0x01000193;
    constexpr uint32_t kFnvOffsetBasis = 0x811c9dc5;

    uint32_t hash = kFnvOffsetBasis;
    for (char c : sdp) {
        hash ^= static_cast<uint8_t>(c);
        hash *= kFnvPrime;
    }

    // Fold 32-bit to 16-bit using XOR
    return static_cast<uint16_t>((hash >> 16) ^ (hash & 0xFFFF));
}

size_t aes67_build_sap_packet(const Aes67Session& session,
                              uint8_t* out_buf, size_t buf_size) {
    std::string sdp = aes67_generate_sdp(session);

    // Payload type string "application/sdp\0"
    const char* payload_type = "application/sdp";
    size_t pt_len = std::strlen(payload_type) + 1; // include null

    size_t total = sizeof(SapHeader) + pt_len + sdp.size();
    if (total > buf_size) return 0;

    // SAP header
    SapHeader hdr{};
    hdr.version_flags = 0x20;  // V=1, A=0, R=0, T=0, E=0, C=0
    hdr.auth_length = 0;
    hdr.msg_id_hash = htons(aes67_sap_hash(sdp));

    // Convert origin IP to uint32
    struct in_addr addr{};
    if (inet_pton(AF_INET, session.origin_address.c_str(), &addr) == 1) {
        hdr.originating_source = addr.s_addr; // already network byte order
    }

    std::memcpy(out_buf, &hdr, sizeof(SapHeader));
    std::memcpy(out_buf + sizeof(SapHeader), payload_type, pt_len);
    std::memcpy(out_buf + sizeof(SapHeader) + pt_len, sdp.c_str(), sdp.size());

    return total;
}

size_t aes67_build_rtp_packet(uint8_t* out_buf, size_t buf_size,
                              uint32_t ssrc, uint16_t sequence,
                              uint32_t timestamp, uint8_t payload_type,
                              const void* payload, size_t payload_size) {
    size_t total = sizeof(RtpHeader) + payload_size;
    if (total > buf_size) return 0;

    // Standard RTP header (no extension)
    RtpHeader rtp{};
    rtp.version = 2;
    rtp.padding = 0;
    rtp.extension = 0;  // No OSTP extension for AES67
    rtp.cc = 0;
    rtp.marker = 0;
    rtp.pt = payload_type;
    rtp.sequence = htons(sequence);
    rtp.timestamp = htonl(timestamp);
    rtp.ssrc = htonl(ssrc);

    std::memcpy(out_buf, &rtp, sizeof(RtpHeader));
    std::memcpy(out_buf + sizeof(RtpHeader), payload, payload_size);

    return total;
}

bool aes67_is_standard_packet(const RtpHeader& hdr) {
    return (hdr.pt == kPayloadTypeL24 || hdr.pt == kPayloadTypeL16);
}

// ============================================================================
// SDP Parser
// ============================================================================

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> result;
    std::istringstream iss(s);
    std::string token;
    while (std::getline(iss, token, delim)) {
        result.push_back(token);
    }
    return result;
}

bool aes67_parse_sdp(const char* sdp, Aes67RemoteSession& out) {
    if (!sdp) return false;
    return aes67_parse_sdp(std::string(sdp), out);
}

bool aes67_parse_sdp(const std::string& sdp, Aes67RemoteSession& out) {
    out = Aes67RemoteSession{};

    std::istringstream stream(sdp);
    std::string line;

    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty() || line.size() < 2 || line[1] != '=') continue;

        char type = line[0];
        std::string value = line.substr(2);

        switch (type) {
            case 'o': {
                // o=<username> <sess-id> <sess-version> IN IP4 <origin-addr>
                auto parts = split(value, ' ');
                if (parts.size() >= 6) {
                    out.session_id = static_cast<uint32_t>(std::stoul(parts[1]));
                    out.session_version = static_cast<uint32_t>(std::stoul(parts[2]));
                    out.origin_address = parts[5];
                }
                break;
            }
            case 's':
                out.session_name = value;
                break;
            case 'c': {
                // c=IN IP4 <multicast-group>/TTL
                auto parts = split(value, ' ');
                if (parts.size() >= 3) {
                    std::string addr = parts[2];
                    auto slash = addr.find('/');
                    if (slash != std::string::npos) {
                        addr = addr.substr(0, slash);
                    }
                    out.multicast_ip = addr;
                }
                break;
            }
            case 'm': {
                // m=audio <port> RTP/AVP <payload-type>
                auto parts = split(value, ' ');
                if (parts.size() >= 4 && parts[0] == "audio") {
                    out.port = static_cast<uint16_t>(std::stoi(parts[1]));
                    out.payload_type = static_cast<uint8_t>(std::stoi(parts[3]));
                }
                break;
            }
            case 'a': {
                // Handle attributes
                auto eq_pos = value.find(':');
                std::string attr_name = (eq_pos != std::string::npos)
                    ? value.substr(0, eq_pos)
                    : value;
                std::string attr_value = (eq_pos != std::string::npos)
                    ? value.substr(eq_pos + 1)
                    : "";

                if (attr_name == "rtpmap") {
                    // rtpmap:<pt> L24/<rate>/<channels> or L16/<rate>/<channels>
                    auto space_pos = attr_value.find(' ');
                    if (space_pos != std::string::npos) {
                        std::string encoding = attr_value.substr(space_pos + 1);
                        auto parts = split(encoding, '/');
                        if (!parts.empty()) {
                            if (parts[0] == "L24") {
                                out.bit_depth = 24;
                            } else if (parts[0] == "L16") {
                                out.bit_depth = 16;
                            }
                            if (parts.size() >= 2) {
                                out.sample_rate = static_cast<uint32_t>(std::stoul(parts[1]));
                            }
                            if (parts.size() >= 3) {
                                out.channels = static_cast<uint8_t>(std::stoi(parts[2]));
                            } else {
                                out.channels = 1; // Mono if not specified
                            }
                        }
                    }
                } else if (attr_name == "ptime") {
                    double ptime_ms = std::stod(attr_value);
                    out.packet_time_us = static_cast<uint32_t>(ptime_ms * 1000);
                } else if (attr_name == "ts-refclk") {
                    if (attr_value.find("ptp=IEEE1588") != std::string::npos) {
                        out.has_ptp_refclk = true;
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    // Validate minimum required fields
    return !out.multicast_ip.empty() && out.port > 0;
}

// ============================================================================
// SAP Listener
// ============================================================================

struct SapListener::Impl {
    std::unique_ptr<pal::UdpSocket> socket;
    std::unique_ptr<pal::Thread> thread;
    SessionCallback callback;
    std::atomic<bool> running{false};

    // Track known sessions by hash
    std::mutex sessions_mutex;
    std::map<uint16_t, Aes67RemoteSession> sessions;

    void receive_loop() {
        uint8_t buf[2048];
        pal::SocketAddress src;

        while (running.load()) {
            int n = socket->recv_from(buf, sizeof(buf), src);
            if (n <= 0) continue;
            if (static_cast<size_t>(n) < sizeof(SapHeader) + 1) continue;

            // Parse SAP header
            const SapHeader* hdr = reinterpret_cast<const SapHeader*>(buf);

            // Check version (1) and address type (0 = IPv4)
            uint8_t version = (hdr->version_flags >> 5) & 0x07;
            uint8_t addr_type = (hdr->version_flags >> 4) & 0x01;
            uint8_t msg_type = (hdr->version_flags >> 2) & 0x01; // 0 = announce, 1 = delete

            if (version != 1) continue;
            if (addr_type != 0) continue; // Only IPv4 supported for now

            // Find payload type string and SDP
            const char* payload_start = reinterpret_cast<const char*>(buf + sizeof(SapHeader));
            size_t remaining = static_cast<size_t>(n) - sizeof(SapHeader);

            // Skip payload type string (null-terminated)
            const char* sdp_start = static_cast<const char*>(
                std::memchr(payload_start, '\0', remaining));
            if (!sdp_start) continue;
            sdp_start++; // Skip null terminator

            size_t sdp_len = remaining - (sdp_start - payload_start);
            if (sdp_len == 0) continue;

            std::string sdp(sdp_start, sdp_len);

            Aes67RemoteSession session;
            if (aes67_parse_sdp(sdp, session)) {
                bool is_deletion = (msg_type == 1);
                uint16_t hash = ntohs(hdr->msg_id_hash);

                {
                    std::lock_guard<std::mutex> lock(sessions_mutex);
                    if (is_deletion) {
                        sessions.erase(hash);
                    } else {
                        sessions[hash] = session;
                    }
                }

                if (callback) {
                    callback(session, is_deletion);
                }
            }
        }
    }
};

SapListener::SapListener() : impl_(std::make_unique<Impl>()) {}

SapListener::~SapListener() {
    stop();
}

bool SapListener::start(SessionCallback on_session) {
    if (impl_->running.load()) return false;

    impl_->callback = std::move(on_session);

    impl_->socket = pal::UdpSocket::create();
    if (!impl_->socket) return false;

    if (!impl_->socket->bind(kSapPort)) return false;
    if (!impl_->socket->join_multicast(kSapMulticastAddr)) return false;
    impl_->socket->set_recv_timeout_ms(100);

    impl_->running.store(true);

    impl_->thread = pal::Thread::create("sap-listener", pal::ThreadPriority::Normal);
    impl_->thread->start([this]() { impl_->receive_loop(); });

    return true;
}

void SapListener::stop() {
    impl_->running.store(false);
    if (impl_->thread) {
        impl_->thread->join();
        impl_->thread.reset();
    }
    if (impl_->socket) {
        impl_->socket->leave_multicast(kSapMulticastAddr);
        impl_->socket.reset();
    }
}

bool SapListener::is_running() const {
    return impl_->running.load();
}

size_t SapListener::session_count() const {
    std::lock_guard<std::mutex> lock(impl_->sessions_mutex);
    return impl_->sessions.size();
}

} // namespace soluna::transport
