/**
 * AirPlay 2 Receiver — RTSP server + mDNS advertisement + RTP audio reception
 *
 * Implements a minimal AirPlay receiver compatible with Apple devices.
 * Supports unencrypted ALAC audio streaming (FairPlay deferred).
 *
 * SPDX-License-Identifier: OpenSonic-Community-1.0
 */

#ifdef SOLUNA_HAS_AIRPLAY

#include <soluna/transport/airplay.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <net/if.h>
#include <ifaddrs.h>

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <map>

// Platform-specific mDNS
#ifdef __APPLE__
#include <dns_sd.h>
#include <sys/select.h>
#include <net/if_dl.h>
#else
// Linux: use Avahi D-Bus client (or fall back to manual mDNS)
#ifdef SOLUNA_HAS_AVAHI
#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/error.h>
#else
// Stub: manual mDNS advertisement not implemented yet
#endif
#endif

namespace soluna::transport {

// ── mDNS State ───────────────────────────────────────────────────────────────

struct AirPlayReceiver::MdnsState {
#ifdef __APPLE__
    DNSServiceRef raop_ref  = nullptr;
    DNSServiceRef airplay_ref = nullptr;
    std::thread   pump_thread;
    std::atomic<bool> pump_running{false};
#elif defined(SOLUNA_HAS_AVAHI)
    AvahiSimplePoll*  poll     = nullptr;
    AvahiClient*      client   = nullptr;
    AvahiEntryGroup*  group    = nullptr;
    std::thread       poll_thread;
    std::atomic<bool> poll_running{false};
#endif
};

// ── Constructor / Destructor ─────────────────────────────────────────────────

AirPlayReceiver::AirPlayReceiver()
    : mdns_(std::make_unique<MdnsState>()) {}

AirPlayReceiver::~AirPlayReceiver() {
    stop();
}

// ── Public API ───────────────────────────────────────────────────────────────

void AirPlayReceiver::set_device_name(const std::string& name) {
    device_name_ = name;
}

void AirPlayReceiver::set_audio_callback(AirPlayAudioCallback cb) {
    audio_callback_ = std::move(cb);
}

bool AirPlayReceiver::start(uint16_t rtsp_port) {
    if (running_.load()) return true;

    rtsp_port_ = rtsp_port;

    // Create RTSP listening socket
    rtsp_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (rtsp_fd_ < 0) {
        fprintf(stderr, "[airplay] Failed to create RTSP socket: %s\n", strerror(errno));
        return false;
    }

    int optval = 1;
    setsockopt(rtsp_fd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(rtsp_port);

    if (bind(rtsp_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        fprintf(stderr, "[airplay] Failed to bind RTSP port %u: %s\n", rtsp_port, strerror(errno));
        close(rtsp_fd_);
        rtsp_fd_ = -1;
        return false;
    }

    if (listen(rtsp_fd_, 4) < 0) {
        fprintf(stderr, "[airplay] Failed to listen on RTSP socket: %s\n", strerror(errno));
        close(rtsp_fd_);
        rtsp_fd_ = -1;
        return false;
    }

    running_.store(true);

    // Start mDNS advertisement
    if (!start_mdns(rtsp_port)) {
        fprintf(stderr, "[airplay] mDNS advertisement failed (continuing without)\n");
    }

    // Start RTSP accept thread
    rtsp_thread_ = std::thread([this]() { rtsp_accept_loop(); });

    fprintf(stderr, "[airplay] AirPlay receiver started on port %u (name: %s)\n",
            rtsp_port, device_name_.c_str());
    return true;
}

void AirPlayReceiver::stop() {
    if (!running_.load()) return;
    running_.store(false);

    // Close RTSP socket to unblock accept()
    if (rtsp_fd_ >= 0) {
        shutdown(rtsp_fd_, SHUT_RDWR);
        close(rtsp_fd_);
        rtsp_fd_ = -1;
    }

    // Close active session
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (session_.client_fd >= 0) {
            close(session_.client_fd);
            session_.client_fd = -1;
        }
        session_.active = false;
    }

    stop_mdns();

    if (rtsp_thread_.joinable()) rtsp_thread_.join();
    if (audio_thread_.joinable()) audio_thread_.join();

    fprintf(stderr, "[airplay] AirPlay receiver stopped\n");
}

std::string AirPlayReceiver::client_ip() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(session_mutex_));
    return session_.active ? session_.client_ip : "";
}

// ── mDNS Advertisement ──────────────────────────────────────────────────────

std::string AirPlayReceiver::get_mac_address() const {
    // Try to get a real MAC address from network interfaces
    struct ifaddrs* ifas = nullptr;
    if (getifaddrs(&ifas) != 0) return "AA:BB:CC:DD:EE:FF";

    std::string result = "AA:BB:CC:DD:EE:FF";

#ifdef __APPLE__
    // On macOS, use AF_LINK to get MAC
    for (auto* ifa = ifas; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_LINK) continue;
        std::string name = ifa->ifa_name;
        if (name == "lo0" || name.find("utun") == 0) continue;
        // Get MAC from sockaddr_dl
        auto* sdl = reinterpret_cast<struct sockaddr_dl*>(ifa->ifa_addr);
        if (sdl->sdl_alen == 6) {
            auto* mac = reinterpret_cast<unsigned char*>(LLADDR(sdl));
            char buf[18];
            snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            result = buf;
            break;
        }
    }
#else
    // On Linux, read from /sys/class/net/*/address
    for (auto* ifa = ifas; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        std::string name = ifa->ifa_name;
        if (name == "lo") continue;
        std::string path = "/sys/class/net/" + name + "/address";
        FILE* f = fopen(path.c_str(), "r");
        if (f) {
            char buf[32] = {};
            if (fgets(buf, sizeof(buf), f)) {
                // Remove trailing newline
                size_t len = strlen(buf);
                if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
                result = buf;
                // Convert to uppercase
                for (auto& c : result) c = toupper(c);
            }
            fclose(f);
            break;
        }
    }
#endif

    freeifaddrs(ifas);
    return result;
}

bool AirPlayReceiver::start_mdns(uint16_t rtsp_port) {
    std::string mac = get_mac_address();
    // Remove colons for service name
    std::string mac_nodash = mac;
    mac_nodash.erase(std::remove(mac_nodash.begin(), mac_nodash.end(), ':'), mac_nodash.end());

    // RAOP service name: "<MAC>@<DeviceName>"
    std::string raop_name = mac_nodash + "@" + device_name_;

    // TXT records for _raop._tcp
    // Key AirPlay features for basic unencrypted streaming
    std::vector<std::pair<std::string, std::string>> raop_txt = {
        {"tp",  "UDP"},
        {"sm",  "false"},
        {"ek",  "1"},
        {"et",  "0,1"},       // encryption types: 0=none, 1=RSA (we only support 0)
        {"cn",  "0,1"},       // codecs: 0=PCM, 1=ALAC
        {"ch",  "2"},         // channels
        {"ss",  "16"},        // sample size
        {"sr",  "44100"},     // sample rate
        {"pw",  "false"},     // no password
        {"vn",  "65537"},     // version
        {"da",  "true"},
        {"vs",  "366.0"},     // server version
        {"md",  "0,1,2"},     // metadata types
        {"ft",  "0x5A7FFFF7,0x1E"},  // features bitmask
    };

    // TXT records for _airplay._tcp
    std::vector<std::pair<std::string, std::string>> airplay_txt = {
        {"deviceid", mac},
        {"features", "0x5A7FFFF7,0x1E"},
        {"model",    "Soluna1,1"},
        {"srcvers",  "366.0"},
    };

#ifdef __APPLE__
    // Build TXT record data for Bonjour
    auto build_txt = [](const std::vector<std::pair<std::string, std::string>>& records) -> std::vector<uint8_t> {
        std::vector<uint8_t> txt;
        for (const auto& kv : records) {
            std::string entry = kv.first + "=" + kv.second;
            if (entry.size() > 255) continue;
            txt.push_back(static_cast<uint8_t>(entry.size()));
            txt.insert(txt.end(), entry.begin(), entry.end());
        }
        return txt;
    };

    auto raop_txt_data = build_txt(raop_txt);
    auto airplay_txt_data = build_txt(airplay_txt);

    uint16_t netport = htons(rtsp_port);

    // Register _raop._tcp
    DNSServiceErrorType err = DNSServiceRegister(
        &mdns_->raop_ref, 0, 0,
        raop_name.c_str(),
        "_raop._tcp",
        nullptr, nullptr,
        netport,
        static_cast<uint16_t>(raop_txt_data.size()),
        raop_txt_data.data(),
        nullptr, nullptr);
    if (err != kDNSServiceErr_NoError) {
        fprintf(stderr, "[airplay] Failed to register _raop._tcp: %d\n", err);
        return false;
    }

    // Register _airplay._tcp
    err = DNSServiceRegister(
        &mdns_->airplay_ref, 0, 0,
        device_name_.c_str(),
        "_airplay._tcp",
        nullptr, nullptr,
        netport,
        static_cast<uint16_t>(airplay_txt_data.size()),
        airplay_txt_data.data(),
        nullptr, nullptr);
    if (err != kDNSServiceErr_NoError) {
        fprintf(stderr, "[airplay] Failed to register _airplay._tcp: %d\n", err);
    }

    // Pump mDNS sockets in background
    mdns_->pump_running.store(true);
    mdns_->pump_thread = std::thread([this]() {
        while (mdns_->pump_running.load()) {
            fd_set r;
            FD_ZERO(&r);
            int maxfd = -1;
            if (mdns_->raop_ref) {
                int fd = DNSServiceRefSockFD(mdns_->raop_ref);
                FD_SET(fd, &r);
                maxfd = std::max(maxfd, fd);
            }
            if (mdns_->airplay_ref) {
                int fd = DNSServiceRefSockFD(mdns_->airplay_ref);
                FD_SET(fd, &r);
                maxfd = std::max(maxfd, fd);
            }
            if (maxfd < 0) break;

            struct timeval tv{1, 0};
            if (select(maxfd + 1, &r, nullptr, nullptr, &tv) > 0) {
                if (mdns_->raop_ref) {
                    int fd = DNSServiceRefSockFD(mdns_->raop_ref);
                    if (FD_ISSET(fd, &r))
                        DNSServiceProcessResult(mdns_->raop_ref);
                }
                if (mdns_->airplay_ref) {
                    int fd = DNSServiceRefSockFD(mdns_->airplay_ref);
                    if (FD_ISSET(fd, &r))
                        DNSServiceProcessResult(mdns_->airplay_ref);
                }
            }
        }
    });

    fprintf(stderr, "[airplay] mDNS: advertising '%s' via _raop._tcp + _airplay._tcp\n",
            raop_name.c_str());
    return true;

#elif defined(SOLUNA_HAS_AVAHI)
    // Avahi implementation for Linux
    mdns_->poll = avahi_simple_poll_new();
    if (!mdns_->poll) {
        fprintf(stderr, "[airplay] Failed to create Avahi simple poll\n");
        return false;
    }

    int avahi_err = 0;
    mdns_->client = avahi_client_new(
        avahi_simple_poll_get(mdns_->poll), (AvahiClientFlags)0,
        nullptr, nullptr, &avahi_err);
    if (!mdns_->client) {
        fprintf(stderr, "[airplay] Failed to create Avahi client: %s\n",
                avahi_strerror(avahi_err));
        avahi_simple_poll_free(mdns_->poll);
        mdns_->poll = nullptr;
        return false;
    }

    mdns_->group = avahi_entry_group_new(mdns_->client, nullptr, nullptr);
    if (!mdns_->group) {
        fprintf(stderr, "[airplay] Failed to create Avahi entry group\n");
        return false;
    }

    // Build Avahi TXT string list
    auto build_avahi_txt = [](const std::vector<std::pair<std::string, std::string>>& records) -> AvahiStringList* {
        AvahiStringList* list = nullptr;
        for (const auto& kv : records) {
            std::string entry = kv.first + "=" + kv.second;
            list = avahi_string_list_add(list, entry.c_str());
        }
        return list;
    };

    AvahiStringList* raop_sl = build_avahi_txt(raop_txt);
    avahi_entry_group_add_service_strlst(
        mdns_->group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, (AvahiPublishFlags)0,
        raop_name.c_str(), "_raop._tcp", nullptr, nullptr,
        rtsp_port, raop_sl);
    avahi_string_list_free(raop_sl);

    AvahiStringList* ap_sl = build_avahi_txt(airplay_txt);
    avahi_entry_group_add_service_strlst(
        mdns_->group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, (AvahiPublishFlags)0,
        device_name_.c_str(), "_airplay._tcp", nullptr, nullptr,
        rtsp_port, ap_sl);
    avahi_string_list_free(ap_sl);

    avahi_entry_group_commit(mdns_->group);

    // Run poll loop in background
    mdns_->poll_running.store(true);
    mdns_->poll_thread = std::thread([this]() {
        while (mdns_->poll_running.load()) {
            avahi_simple_poll_iterate(mdns_->poll, 1000);
        }
    });

    fprintf(stderr, "[airplay] mDNS: advertising '%s' via Avahi\n", raop_name.c_str());
    return true;
#else
    fprintf(stderr, "[airplay] mDNS: no mDNS backend available (need Bonjour or Avahi)\n");
    return false;
#endif
}

void AirPlayReceiver::stop_mdns() {
#ifdef __APPLE__
    mdns_->pump_running.store(false);
    if (mdns_->pump_thread.joinable()) mdns_->pump_thread.join();
    if (mdns_->raop_ref) {
        DNSServiceRefDeallocate(mdns_->raop_ref);
        mdns_->raop_ref = nullptr;
    }
    if (mdns_->airplay_ref) {
        DNSServiceRefDeallocate(mdns_->airplay_ref);
        mdns_->airplay_ref = nullptr;
    }
#elif defined(SOLUNA_HAS_AVAHI)
    mdns_->poll_running.store(false);
    if (mdns_->poll) avahi_simple_poll_quit(mdns_->poll);
    if (mdns_->poll_thread.joinable()) mdns_->poll_thread.join();
    if (mdns_->group) avahi_entry_group_free(mdns_->group);
    if (mdns_->client) avahi_client_free(mdns_->client);
    if (mdns_->poll) avahi_simple_poll_free(mdns_->poll);
    mdns_->group = nullptr;
    mdns_->client = nullptr;
    mdns_->poll = nullptr;
#endif
}

// ── RTSP Server ──────────────────────────────────────────────────────────────

void AirPlayReceiver::rtsp_accept_loop() {
    while (running_.load()) {
        struct sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(rtsp_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
        if (client_fd < 0) {
            if (running_.load()) {
                fprintf(stderr, "[airplay] accept() failed: %s\n", strerror(errno));
            }
            continue;
        }

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        std::string client_ip = ip_str;

        fprintf(stderr, "[airplay] RTSP client connected from %s\n", client_ip.c_str());

        // Handle client in current thread (single session model)
        // Close previous session if any
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            if (session_.client_fd >= 0) {
                close(session_.client_fd);
                session_.active = false;
            }
            if (audio_thread_.joinable()) {
                // Signal audio thread to stop by closing socket
                audio_thread_.detach();
            }
        }

        rtsp_handle_client(client_fd, client_ip);
    }
}

void AirPlayReceiver::rtsp_handle_client(int client_fd, const std::string& client_ip) {
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        // Reset session fields (can't copy-assign due to unique_ptr in AlacDecoder)
        session_.client_fd = client_fd;
        session_.client_ip = client_ip;
        session_.audio_port = 0;
        session_.control_port = 0;
        session_.timing_port = 0;
        session_.remote_audio = 0;
        session_.remote_control = 0;
        session_.remote_timing = 0;
        session_.volume = -30.0f;
        session_.active = false;
        session_.decoder = AlacDecoder(); // move-assign new decoder
    }

    // Read RTSP requests line by line
    std::string buffer;
    char read_buf[4096];

    while (running_.load()) {
        struct pollfd pfd{client_fd, POLLIN, 0};
        int ret = poll(&pfd, 1, 1000); // 1s timeout
        if (ret < 0) break;
        if (ret == 0) continue;
        if (pfd.revents & (POLLERR | POLLHUP)) break;

        ssize_t n = recv(client_fd, read_buf, sizeof(read_buf), 0);
        if (n <= 0) break;
        buffer.append(read_buf, n);

        // Parse complete RTSP messages
        while (true) {
            // Find end of headers
            size_t header_end = buffer.find("\r\n\r\n");
            if (header_end == std::string::npos) break;

            std::string headers = buffer.substr(0, header_end);

            // Check for Content-Length
            size_t content_length = 0;
            auto cl_pos = headers.find("Content-Length:");
            if (cl_pos == std::string::npos)
                cl_pos = headers.find("content-length:");
            if (cl_pos != std::string::npos) {
                content_length = std::stoul(headers.substr(cl_pos + 15));
            }

            size_t total_len = header_end + 4 + content_length;
            if (buffer.size() < total_len) break; // need more data

            std::string body;
            if (content_length > 0) {
                body = buffer.substr(header_end + 4, content_length);
            }
            buffer.erase(0, total_len);

            // Parse request line
            auto first_line_end = headers.find("\r\n");
            std::string request_line = headers.substr(0, first_line_end);
            std::string remaining_headers = (first_line_end != std::string::npos)
                ? headers.substr(first_line_end + 2) : "";

            // Parse method and URI
            std::istringstream rss(request_line);
            std::string method, uri, version;
            rss >> method >> uri >> version;

            // Parse CSeq
            int cseq = 0;
            auto cseq_pos = remaining_headers.find("CSeq:");
            if (cseq_pos == std::string::npos)
                cseq_pos = remaining_headers.find("cseq:");
            if (cseq_pos != std::string::npos) {
                cseq = std::stoi(remaining_headers.substr(cseq_pos + 5));
            }

            fprintf(stderr, "[airplay] RTSP %s %s (CSeq: %d)\n",
                    method.c_str(), uri.c_str(), cseq);

            // Handle request
            std::string response = handle_rtsp_request(method, uri, remaining_headers, body, cseq);

            // Send response
            if (!response.empty()) {
                send(client_fd, response.c_str(), response.size(), 0);
            }
        }
    }

    // Clean up session
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (session_.client_fd == client_fd) {
            close(client_fd);
            session_.client_fd = -1;
            session_.active = false;
        }
    }

    fprintf(stderr, "[airplay] RTSP client disconnected (%s)\n", client_ip.c_str());
}

std::string AirPlayReceiver::handle_rtsp_request(
    const std::string& method, const std::string& uri,
    const std::string& headers, const std::string& body,
    int cseq) {

    if (method == "OPTIONS")       return handle_options(cseq);
    if (method == "ANNOUNCE")      return handle_announce(body, cseq);
    if (method == "SETUP")         return handle_setup(headers, cseq);
    if (method == "RECORD")        return handle_record(cseq);
    if (method == "SET_PARAMETER") return handle_set_parameter(headers, body, cseq);
    if (method == "FLUSH")         return handle_flush(cseq);
    if (method == "TEARDOWN")      return handle_teardown(cseq);
    if (method == "GET" && uri == "/info") return handle_get_info(cseq);

    // Unknown method: 501 Not Implemented
    std::ostringstream oss;
    oss << "RTSP/1.0 501 Not Implemented\r\n"
        << "CSeq: " << cseq << "\r\n"
        << "\r\n";
    return oss.str();
}

// ── RTSP Method Handlers ─────────────────────────────────────────────────────

std::string AirPlayReceiver::handle_options(int cseq) {
    std::ostringstream oss;
    oss << "RTSP/1.0 200 OK\r\n"
        << "CSeq: " << cseq << "\r\n"
        << "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, "
           "OPTIONS, SET_PARAMETER, GET_PARAMETER\r\n"
        << "\r\n";
    return oss.str();
}

std::string AirPlayReceiver::handle_announce(const std::string& body, int cseq) {
    // Parse SDP to extract ALAC configuration
    // Look for a=fmtp: line which contains ALAC parameters
    std::istringstream sdp(body);
    std::string line;
    while (std::getline(sdp, line)) {
        // Remove \r if present
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.find("a=fmtp:") == 0) {
            // Format: a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100
            std::string fmtp = line.substr(7); // skip "a=fmtp:"
            std::lock_guard<std::mutex> lock(session_mutex_);
            if (!session_.decoder.configure(fmtp)) {
                fprintf(stderr, "[airplay] Failed to parse ALAC fmtp: %s\n", fmtp.c_str());
            } else {
                fprintf(stderr, "[airplay] ALAC configured: %u frames, %uHz, %u-bit, %uch\n",
                        session_.decoder.frame_length(),
                        session_.decoder.sample_rate(),
                        session_.decoder.bit_depth(),
                        session_.decoder.channels());
            }
        }
    }

    std::ostringstream oss;
    oss << "RTSP/1.0 200 OK\r\n"
        << "CSeq: " << cseq << "\r\n"
        << "\r\n";
    return oss.str();
}

std::string AirPlayReceiver::handle_setup(const std::string& headers, int cseq) {
    // Parse Transport header for client ports
    // Format: Transport: RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;
    //         control_port=6001;timing_port=6002
    uint16_t client_control = 0, client_timing = 0;

    auto transport_pos = headers.find("Transport:");
    if (transport_pos == std::string::npos)
        transport_pos = headers.find("transport:");
    if (transport_pos != std::string::npos) {
        auto line_end = headers.find("\r\n", transport_pos);
        std::string transport_line = headers.substr(transport_pos + 10,
            line_end != std::string::npos ? line_end - transport_pos - 10 : std::string::npos);

        // Parse control_port and timing_port
        auto parse_port = [&](const std::string& key) -> uint16_t {
            auto pos = transport_line.find(key + "=");
            if (pos == std::string::npos) return 0;
            return static_cast<uint16_t>(std::stoi(transport_line.substr(pos + key.size() + 1)));
        };
        client_control = parse_port("control_port");
        client_timing = parse_port("timing_port");
    }

    // Allocate local UDP ports
    uint16_t local_audio   = alloc_udp_port();
    uint16_t local_control = alloc_udp_port();
    uint16_t local_timing  = alloc_udp_port();

    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_.audio_port = local_audio;
        session_.control_port = local_control;
        session_.timing_port = local_timing;
        session_.remote_control = client_control;
        session_.remote_timing = client_timing;
    }

    fprintf(stderr, "[airplay] SETUP: audio=%u, control=%u, timing=%u "
            "(client control=%u, timing=%u)\n",
            local_audio, local_control, local_timing,
            client_control, client_timing);

    std::ostringstream oss;
    oss << "RTSP/1.0 200 OK\r\n"
        << "CSeq: " << cseq << "\r\n"
        << "Transport: RTP/AVP/UDP;unicast;mode=record;"
        << "server_port=" << local_audio << ";"
        << "control_port=" << local_control << ";"
        << "timing_port=" << local_timing << "\r\n"
        << "Session: 1\r\n"
        << "\r\n";
    return oss.str();
}

std::string AirPlayReceiver::handle_record(int cseq) {
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_.active = true;
    }

    // Start audio reception thread
    if (audio_thread_.joinable()) audio_thread_.detach();
    audio_thread_ = std::thread([this]() { audio_receive_loop(); });

    std::ostringstream oss;
    oss << "RTSP/1.0 200 OK\r\n"
        << "CSeq: " << cseq << "\r\n"
        << "Audio-Latency: 2205\r\n"
        << "\r\n";
    return oss.str();
}

std::string AirPlayReceiver::handle_set_parameter(
    const std::string& headers, const std::string& body, int cseq) {

    // Check Content-Type for volume vs metadata
    auto ct_pos = headers.find("Content-Type:");
    if (ct_pos == std::string::npos)
        ct_pos = headers.find("content-type:");
    if (ct_pos != std::string::npos) {
        auto ct_end = headers.find("\r\n", ct_pos);
        std::string content_type = headers.substr(ct_pos + 13,
            ct_end != std::string::npos ? ct_end - ct_pos - 13 : std::string::npos);

        // Trim whitespace
        while (!content_type.empty() && content_type.front() == ' ')
            content_type.erase(content_type.begin());

        if (content_type.find("text/parameters") != std::string::npos) {
            // Parse text parameters (volume, progress)
            std::istringstream pss(body);
            std::string line;
            while (std::getline(pss, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.find("volume:") == 0) {
                    float vol = std::stof(line.substr(7));
                    std::lock_guard<std::mutex> lock(session_mutex_);
                    session_.volume = vol;
                    fprintf(stderr, "[airplay] Volume: %.1f dB\n", vol);
                }
            }
        }
        // DAAP metadata (application/x-dmap-tagged) — log but don't parse yet
        else if (content_type.find("application/x-dmap-tagged") != std::string::npos) {
            fprintf(stderr, "[airplay] Received DAAP metadata (%zu bytes)\n", body.size());
        }
    }

    std::ostringstream oss;
    oss << "RTSP/1.0 200 OK\r\n"
        << "CSeq: " << cseq << "\r\n"
        << "\r\n";
    return oss.str();
}

std::string AirPlayReceiver::handle_flush(int cseq) {
    fprintf(stderr, "[airplay] FLUSH\n");
    // Audio buffer flushing would happen here
    std::ostringstream oss;
    oss << "RTSP/1.0 200 OK\r\n"
        << "CSeq: " << cseq << "\r\n"
        << "\r\n";
    return oss.str();
}

std::string AirPlayReceiver::handle_teardown(int cseq) {
    fprintf(stderr, "[airplay] TEARDOWN\n");
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_.active = false;
    }

    std::ostringstream oss;
    oss << "RTSP/1.0 200 OK\r\n"
        << "CSeq: " << cseq << "\r\n"
        << "\r\n";
    return oss.str();
}

std::string AirPlayReceiver::handle_get_info(int cseq) {
    // Return basic device info (simplified plist as text)
    std::string mac = get_mac_address();
    std::ostringstream body;
    body << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
            "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
         << "<plist version=\"1.0\">\n<dict>\n"
         << "  <key>deviceid</key><string>" << mac << "</string>\n"
         << "  <key>features</key><integer>0x5A7FFFF7</integer>\n"
         << "  <key>model</key><string>Soluna1,1</string>\n"
         << "  <key>name</key><string>" << device_name_ << "</string>\n"
         << "  <key>statusFlags</key><integer>4</integer>\n"
         << "</dict>\n</plist>\n";

    std::string body_str = body.str();
    std::ostringstream oss;
    oss << "RTSP/1.0 200 OK\r\n"
        << "CSeq: " << cseq << "\r\n"
        << "Content-Type: text/x-apple-plist+xml\r\n"
        << "Content-Length: " << body_str.size() << "\r\n"
        << "\r\n"
        << body_str;
    return oss.str();
}

// ── Audio Reception ──────────────────────────────────────────────────────────

void AirPlayReceiver::audio_receive_loop() {
    uint16_t audio_port;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        audio_port = session_.audio_port;
    }

    // Create UDP socket for audio reception
    int audio_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (audio_fd < 0) {
        fprintf(stderr, "[airplay] Failed to create audio UDP socket\n");
        return;
    }

    int optval = 1;
    setsockopt(audio_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(audio_port);

    if (::bind(audio_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        fprintf(stderr, "[airplay] Failed to bind audio port %u: %s\n", audio_port, strerror(errno));
        close(audio_fd);
        return;
    }

    fprintf(stderr, "[airplay] Audio receiver listening on UDP port %u\n", audio_port);

    // Receive buffer
    uint8_t pkt_buf[2048];
    // Decode buffer (352 frames * 2 channels * 2 bytes = 1408 bytes typical)
    std::vector<int16_t> pcm_buf(4096);

    uint16_t last_seq = 0;
    bool first_packet = true;
    uint64_t total_packets = 0;
    uint64_t lost_packets = 0;

    while (running_.load()) {
        bool active;
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            active = session_.active;
        }
        if (!active) break;

        struct pollfd pfd{audio_fd, POLLIN, 0};
        int ret = poll(&pfd, 1, 500);
        if (ret <= 0) continue;

        struct sockaddr_in src_addr{};
        socklen_t src_len = sizeof(src_addr);
        ssize_t n = recvfrom(audio_fd, pkt_buf, sizeof(pkt_buf), 0,
                             reinterpret_cast<struct sockaddr*>(&src_addr), &src_len);
        if (n < 4) continue; // too small for RTP header

        // Parse RTP header (minimal)
        // Byte 0: V(2) P(1) X(1) CC(4)
        // Byte 1: M(1) PT(7)
        // Bytes 2-3: sequence number
        // Bytes 4-7: timestamp
        // Bytes 8-11: SSRC
        uint8_t version = (pkt_buf[0] >> 6) & 0x3;
        if (version != 2) continue; // not RTP

        uint8_t  pt  = pkt_buf[1] & 0x7F;
        uint16_t seq = (static_cast<uint16_t>(pkt_buf[2]) << 8) | pkt_buf[3];

        // AirPlay uses PT 96 for ALAC audio
        // PT 84 (0x54) is also used for timing requests
        // PT 85 (0x55) for timing responses
        // PT 86 (0x56) for sync packets
        if (pt != 96) continue; // skip non-audio packets

        // Check sequence numbers for loss detection
        if (!first_packet) {
            uint16_t expected = last_seq + 1;
            if (seq != expected) {
                uint16_t gap = seq - expected;
                lost_packets += gap;
                if (gap < 100) { // avoid spam for large gaps (seek, etc.)
                    fprintf(stderr, "[airplay] Packet loss: expected seq %u, got %u (gap %u)\n",
                            expected, seq, gap);
                }
            }
        }
        last_seq = seq;
        first_packet = false;
        total_packets++;

        // RTP header is 12 bytes; ALAC data follows
        size_t header_size = 12;
        // Check for extension header (X bit)
        if (pkt_buf[0] & 0x10) {
            if (n < 16) continue;
            uint16_t ext_len = (static_cast<uint16_t>(pkt_buf[14]) << 8) | pkt_buf[15];
            header_size += 4 + ext_len * 4;
        }

        if (static_cast<size_t>(n) <= header_size) continue;

        const uint8_t* alac_data = pkt_buf + header_size;
        size_t alac_len = n - header_size;

        // Decode ALAC
        uint32_t decoded_frames = 0;
        bool ok;
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            ok = session_.decoder.decode(alac_data, alac_len, pcm_buf.data(), decoded_frames);
        }

        if (ok && decoded_frames > 0 && audio_callback_) {
            uint8_t channels;
            uint32_t sample_rate;
            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                channels = session_.decoder.channels();
                sample_rate = session_.decoder.sample_rate();
            }
            audio_callback_(pcm_buf.data(), decoded_frames, channels, sample_rate);
        }
    }

    close(audio_fd);

    if (total_packets > 0) {
        fprintf(stderr, "[airplay] Audio stats: %lu packets received, %lu lost (%.2f%%)\n",
                (unsigned long)total_packets, (unsigned long)lost_packets,
                total_packets > 0 ? (double)lost_packets / total_packets * 100.0 : 0.0);
    }
}

// ── Helpers ──────────────────────────────────────────────────────────────────

uint16_t AirPlayReceiver::alloc_udp_port() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0; // let OS assign

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return 0;
    }

    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len);
    uint16_t port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

} // namespace soluna::transport

#endif // SOLUNA_HAS_AIRPLAY
