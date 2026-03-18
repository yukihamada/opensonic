/**
 * Ravenna Compatibility — AES67 + mDNS discovery + SDP extensions
 * SPDX-License-Identifier: MIT
 */

#ifdef SOLUNA_HAS_RAVENNA

#include <soluna/transport/ravenna.h>
#include <cstring>
#include <sstream>
#include <cstdio>
#include <thread>

#ifdef __APPLE__
#include <dns_sd.h>
#endif

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace soluna::transport {

std::string ravenna_generate_sdp(const RavennaSession& session) {
    // Start with standard AES67 SDP
    std::string sdp = aes67_generate_sdp(session);

    // Append Ravenna-specific attributes
    std::ostringstream extra;
    extra << "a=x-ravenna-session:" << session.ravenna_session_version << "\r\n";
    if (!session.device_name.empty()) {
        extra << "a=x-ravenna-device:" << session.device_name << "\r\n";
    }

    return sdp + extra.str();
}

size_t ravenna_build_sap_packet(const RavennaSession& session,
                                 uint8_t* out_buf, size_t buf_size) {
    std::string sdp = ravenna_generate_sdp(session);

    const char* payload_type = "application/sdp";
    size_t pt_len = std::strlen(payload_type) + 1; // include null

    size_t total = sizeof(SapHeader) + pt_len + sdp.size();
    if (total > buf_size) return 0;

    // SAP header
    SapHeader hdr{};
    hdr.version_flags = 0x20;  // V=1, A=0, R=0, T=0, E=0, C=0
    hdr.auth_length = 0;
    hdr.msg_id_hash = htons(aes67_sap_hash(sdp));

    struct in_addr addr{};
    if (inet_pton(AF_INET, session.origin_address.c_str(), &addr) == 1) {
        hdr.originating_source = addr.s_addr;
    }

    std::memcpy(out_buf, &hdr, sizeof(SapHeader));
    std::memcpy(out_buf + sizeof(SapHeader), payload_type, pt_len);
    std::memcpy(out_buf + sizeof(SapHeader) + pt_len, sdp.c_str(), sdp.size());

    return total;
}

// ============================================================================
// mDNS via dns_sd.h (Apple Bonjour)
// ============================================================================

#ifdef __APPLE__

static DNSServiceRef g_ravenna_mdns = nullptr;

bool ravenna_start_mdns(const RavennaSession& session) {
    if (g_ravenna_mdns) {
        // Already running
        return true;
    }

    // Build TXT record: txtvers=1, rate=<sample_rate>, ch=<channels>, name=<device_name>
    TXTRecordRef txt;
    TXTRecordCreate(&txt, 0, nullptr);

    TXTRecordSetValue(&txt, "txtvers", 1, "1");

    std::string rate_str = std::to_string(session.sample_rate);
    TXTRecordSetValue(&txt, "rate",
                      static_cast<uint8_t>(rate_str.size()),
                      rate_str.c_str());

    std::string ch_str = std::to_string(session.channels);
    TXTRecordSetValue(&txt, "ch",
                      static_cast<uint8_t>(ch_str.size()),
                      ch_str.c_str());

    if (!session.device_name.empty()) {
        TXTRecordSetValue(&txt, "name",
                          static_cast<uint8_t>(session.device_name.size()),
                          session.device_name.c_str());
    }

    uint16_t netport = htons(session.rtp_port);

    // Use device_name as instance name, fall back to "Soluna"
    const char* instance_name = session.device_name.empty()
        ? "Soluna" : session.device_name.c_str();

    DNSServiceErrorType err = DNSServiceRegister(
        &g_ravenna_mdns, 0, 0,
        instance_name,
        "_ravenna._tcp",   // Ravenna service type
        nullptr,           // domain (default = .local)
        nullptr,           // host (default = this machine)
        netport,
        TXTRecordGetLength(&txt),
        TXTRecordGetBytesPtr(&txt),
        nullptr, nullptr); // callback (fire-and-forget)

    TXTRecordDeallocate(&txt);

    if (err != kDNSServiceErr_NoError) {
        fprintf(stderr, "[ravenna] DNSServiceRegister failed: %d\n", err);
        g_ravenna_mdns = nullptr;
        return false;
    }

    // Pump the mDNS socket in a background thread (same pattern as _soluna._tcp)
    std::thread([] {
        int fd = DNSServiceRefSockFD(g_ravenna_mdns);
        while (g_ravenna_mdns && fd >= 0) {
            fd_set r;
            FD_ZERO(&r);
            FD_SET(fd, &r);
            struct timeval tv{1, 0};
            if (select(fd + 1, &r, nullptr, nullptr, &tv) > 0) {
                DNSServiceProcessResult(g_ravenna_mdns);
            }
        }
    }).detach();

    printf("[ravenna] Advertising _ravenna._tcp on port %u\n", session.rtp_port);
    return true;
}

void ravenna_stop_mdns() {
    if (g_ravenna_mdns) {
        DNSServiceRefDeallocate(g_ravenna_mdns);
        g_ravenna_mdns = nullptr;
        printf("[ravenna] Stopped _ravenna._tcp advertisement\n");
    }
}

#else
// Non-Apple platforms: stubs (Avahi or other mDNS implementation TBD)

bool ravenna_start_mdns(const RavennaSession& /*session*/) {
    fprintf(stderr, "[ravenna] mDNS not implemented on this platform\n");
    return false;
}

void ravenna_stop_mdns() {
    // no-op
}

#endif // __APPLE__

} // namespace soluna::transport

#endif // SOLUNA_HAS_RAVENNA
