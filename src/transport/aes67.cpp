/**
 * AES67 Compatibility — SAP/SDP announcer and standard RTP builder
 * SPDX-License-Identifier: MIT
 */

#include <soluna/transport/aes67.h>
#include <cstring>
#include <sstream>

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
    // Simple hash of SDP content (RFC 2974 suggests CRC or similar)
    uint16_t hash = 0;
    for (size_t i = 0; i < sdp.size(); i++) {
        hash ^= static_cast<uint16_t>(static_cast<uint8_t>(sdp[i])) << ((i & 1) ? 8 : 0);
    }
    return hash;
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

} // namespace soluna::transport
