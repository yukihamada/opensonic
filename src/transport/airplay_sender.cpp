/**
 * AirPlay Sender (TX) — mDNS discovery + RTSP client + RTP/ALAC streaming
 *
 * Discovers AirPlay speakers on the local network via Bonjour/Avahi,
 * establishes RTSP sessions, encodes PCM audio to ALAC, and streams
 * RTP packets to all connected speakers.
 *
 * Protocol flow per speaker:
 *   1. mDNS browse for _raop._tcp → resolve host:port
 *   2. RTSP ANNOUNCE (SDP with ALAC fmtp)
 *   3. RTSP SETUP (negotiate UDP ports for audio/control/timing)
 *   4. RTSP RECORD (start streaming)
 *   5. RTP packets with ALAC-encoded audio on UDP
 *   6. RTSP TEARDOWN on stop
 *
 * Supports unencrypted AirPlay 1 speakers. FairPlay/HomeKit auth deferred.
 *
 * SPDX-License-Identifier: OpenSonic-Community-1.0
 */

#ifdef SOLUNA_HAS_AIRPLAY

#include <soluna/transport/airplay.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <sstream>

#ifdef __APPLE__
#include <dns_sd.h>
#include <sys/select.h>
#else
#ifdef SOLUNA_HAS_AVAHI
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/error.h>
#endif
#endif

namespace soluna::transport {

// ── ALAC Encoder ─────────────────────────────────────────────────────────────
// Minimal ALAC encoder that outputs uncompressed ALAC frames.
// AirPlay speakers accept "uncompressed" ALAC (is_not_compressed=1),
// which is essentially raw PCM with a small ALAC frame header.
// This avoids the complexity of full ALAC compression while remaining
// fully compatible with all AirPlay receivers.

AlacEncoder::AlacEncoder() = default;
AlacEncoder::~AlacEncoder() = default;
AlacEncoder::AlacEncoder(AlacEncoder&&) noexcept = default;
AlacEncoder& AlacEncoder::operator=(AlacEncoder&&) noexcept = default;

bool AlacEncoder::configure(uint32_t frame_length, uint32_t sample_rate,
                            uint8_t bit_depth, uint8_t num_channels) {
    if (frame_length == 0 || frame_length > 4096) return false;
    if (num_channels == 0 || num_channels > 8) return false;
    if (bit_depth != 16 && bit_depth != 24) return false;

    frame_length_ = frame_length;
    sample_rate_ = sample_rate;
    bit_depth_ = bit_depth;
    num_channels_ = num_channels;
    configured_ = true;

    fprintf(stderr, "[airplay-tx] ALAC encoder configured: %u frames, %uHz, %u-bit, %uch\n",
            frame_length, sample_rate, bit_depth, num_channels);
    return true;
}

bool AlacEncoder::encode(const int16_t* input, uint32_t num_frames,
                         uint8_t* output, size_t output_capacity, size_t& out_len) {
    if (!configured_ || !input || !output) return false;

    // ALAC uncompressed frame format:
    //   3 bits: channels - 1
    //  16 bits: 0 (unused)
    //   1 bit:  has_size (1 if num_frames != frame_length_)
    //   2 bits: 0 (uncompressed indicator)
    //   1 bit:  1 (is_not_compressed)
    //   if has_size: 32 bits frame count
    //   then: raw PCM samples (big-endian)

    // Compute sizes
    bool has_size = (num_frames != frame_length_);
    size_t header_bits = 3 + 16 + 1 + 2 + 1;
    if (has_size) header_bits += 32;
    size_t header_bytes = (header_bits + 7) / 8;
    size_t pcm_bytes = num_frames * num_channels_ * (bit_depth_ / 8);
    size_t total = header_bytes + pcm_bytes;

    if (output_capacity < total) return false;

    memset(output, 0, total);

    // Write header using bit operations
    // We build the header manually since it's only a few bytes
    uint64_t hdr = 0;
    int bits_used = 0;

    auto write_bits = [&](uint32_t val, int nbits) {
        hdr = (hdr << nbits) | (val & ((1u << nbits) - 1));
        bits_used += nbits;
    };

    write_bits(num_channels_ - 1, 3);   // channels - 1
    write_bits(0, 16);                    // unused
    write_bits(has_size ? 1 : 0, 1);     // has_size
    write_bits(0, 2);                     // uncompressed flags
    write_bits(1, 1);                     // is_not_compressed

    if (has_size) {
        write_bits(num_frames, 32);
    }

    // Write header bytes (big-endian)
    int hdr_byte_count = (bits_used + 7) / 8;
    int pad = hdr_byte_count * 8 - bits_used;
    hdr <<= pad; // align to byte boundary
    for (int i = hdr_byte_count - 1; i >= 0; i--) {
        output[i] = static_cast<uint8_t>(hdr & 0xFF);
        hdr >>= 8;
    }

    // Write PCM samples in big-endian (network byte order)
    uint8_t* pcm_out = output + hdr_byte_count;
    if (bit_depth_ == 16) {
        for (uint32_t i = 0; i < num_frames * num_channels_; i++) {
            int16_t s = input[i];
            pcm_out[i * 2 + 0] = static_cast<uint8_t>((s >> 8) & 0xFF);
            pcm_out[i * 2 + 1] = static_cast<uint8_t>(s & 0xFF);
        }
    } else {
        // 24-bit: pad int16 to 24-bit
        for (uint32_t i = 0; i < num_frames * num_channels_; i++) {
            int32_t s = static_cast<int32_t>(input[i]) << 8;
            pcm_out[i * 3 + 0] = static_cast<uint8_t>((s >> 16) & 0xFF);
            pcm_out[i * 3 + 1] = static_cast<uint8_t>((s >> 8) & 0xFF);
            pcm_out[i * 3 + 2] = static_cast<uint8_t>(s & 0xFF);
        }
    }

    out_len = hdr_byte_count + pcm_bytes;
    return true;
}

std::string AlacEncoder::fmtp_line() const {
    // Format: "96 <frameLength> 0 <bitDepth> 40 10 14 <channels> 255 0 0 <sampleRate>"
    std::ostringstream oss;
    oss << "96 " << frame_length_ << " 0 " << (int)bit_depth_
        << " 40 10 14 " << (int)num_channels_
        << " 255 0 0 " << sample_rate_;
    return oss.str();
}

// ── mDNS Browse State ────────────────────────────────────────────────────────

struct AirPlaySender::BrowseState {
#ifdef __APPLE__
    DNSServiceRef browse_ref = nullptr;
    std::thread   pump_thread;
    std::atomic<bool> pump_running{false};
#elif defined(SOLUNA_HAS_AVAHI)
    AvahiSimplePoll*    poll       = nullptr;
    AvahiClient*        client     = nullptr;
    AvahiServiceBrowser* browser   = nullptr;
    std::thread          poll_thread;
    std::atomic<bool>    poll_running{false};
#endif
    AirPlaySender* sender = nullptr; // back-pointer for callbacks
};

// ── Constructor / Destructor ─────────────────────────────────────────────────

AirPlaySender::AirPlaySender()
    : browse_(std::make_unique<BrowseState>()) {
    browse_->sender = this;
}

AirPlaySender::~AirPlaySender() {
    stop();
}

// ── Public API ───────────────────────────────────────────────────────────────

bool AirPlaySender::start() {
    if (running_.load()) return true;

    // Configure encoder: 352 frames @ 44100Hz, 16-bit stereo (AirPlay standard)
    if (!encoder_.configure(352, 44100, 16, 2)) {
        fprintf(stderr, "[airplay-tx] Failed to configure ALAC encoder\n");
        return false;
    }

    running_.store(true);

    if (!start_mdns_browse()) {
        fprintf(stderr, "[airplay-tx] mDNS browse failed (continuing — add speakers manually)\n");
    }

    // Start timing sync thread
    timing_thread_ = std::thread([this]() { timing_thread_func(); });

    fprintf(stderr, "[airplay-tx] AirPlay sender started, discovering speakers...\n");
    return true;
}

void AirPlaySender::stop() {
    if (!running_.load()) return;
    running_.store(false);

    stop_mdns_browse();

    // Teardown all speaker sessions
    {
        std::lock_guard<std::mutex> lock(speakers_mutex_);
        for (auto& spk : speakers_) {
            if (spk.active) {
                rtsp_teardown(spk);
            }
        }
        speakers_.clear();
    }

    if (timing_thread_.joinable()) timing_thread_.join();

    fprintf(stderr, "[airplay-tx] AirPlay sender stopped\n");
}

void AirPlaySender::send_audio(const int16_t* pcm, uint32_t frames,
                                uint8_t channels, uint32_t sample_rate) {
    if (!running_.load()) return;

    // Resample/remix to 44100Hz stereo if needed
    // For now, assume input matches encoder config or handle simple cases
    std::vector<int16_t> resampled;
    const int16_t* send_pcm = pcm;
    uint32_t send_frames = frames;

    // Channel conversion: mono -> stereo
    if (channels == 1 && encoder_.channels() == 2) {
        resampled.resize(frames * 2);
        for (uint32_t i = 0; i < frames; i++) {
            resampled[i * 2 + 0] = pcm[i];
            resampled[i * 2 + 1] = pcm[i];
        }
        send_pcm = resampled.data();
    } else if (channels == 2 && encoder_.channels() == 1) {
        resampled.resize(frames);
        for (uint32_t i = 0; i < frames; i++) {
            resampled[i] = static_cast<int16_t>(
                (static_cast<int32_t>(pcm[i * 2]) + pcm[i * 2 + 1]) / 2);
        }
        send_pcm = resampled.data();
    }

    // Simple sample rate conversion: 48000 -> 44100 (drop samples approximation)
    // A proper resampler would be better, but this gets audio flowing.
    if (sample_rate != encoder_.sample_rate() && sample_rate > 0) {
        double ratio = static_cast<double>(encoder_.sample_rate()) / sample_rate;
        uint32_t out_frames = static_cast<uint32_t>(frames * ratio);
        uint8_t ch = encoder_.channels();
        std::vector<int16_t> rs(out_frames * ch);
        for (uint32_t i = 0; i < out_frames; i++) {
            uint32_t src_i = static_cast<uint32_t>(i / ratio);
            if (src_i >= send_frames) src_i = send_frames - 1;
            for (uint8_t c = 0; c < ch; c++) {
                rs[i * ch + c] = send_pcm[src_i * ch + c];
            }
        }
        resampled = std::move(rs);
        send_pcm = resampled.data();
        send_frames = out_frames;
    }

    // Encode in frame_length-sized chunks and send
    uint32_t fl = encoder_.frame_length();
    uint8_t ch = encoder_.channels();

    // Encode buffer: generous size for ALAC
    std::vector<uint8_t> alac_buf(fl * ch * 4 + 64);

    for (uint32_t offset = 0; offset + fl <= send_frames; offset += fl) {
        size_t alac_len = 0;
        if (!encoder_.encode(send_pcm + offset * ch, fl,
                             alac_buf.data(), alac_buf.size(), alac_len)) {
            continue;
        }

        // Send to all active speakers
        std::lock_guard<std::mutex> lock(speakers_mutex_);
        for (auto& spk : speakers_) {
            if (spk.active) {
                send_rtp_audio(spk, alac_buf.data(), alac_len);
            }
        }
    }
}

std::vector<std::string> AirPlaySender::discovered_speakers() const {
    std::lock_guard<std::mutex> lock(speakers_mutex_);
    std::vector<std::string> names;
    names.reserve(speakers_.size());
    for (const auto& spk : speakers_) {
        names.push_back(spk.name);
    }
    return names;
}

size_t AirPlaySender::active_speaker_count() const {
    std::lock_guard<std::mutex> lock(speakers_mutex_);
    size_t count = 0;
    for (const auto& spk : speakers_) {
        if (spk.active) count++;
    }
    return count;
}

// ── mDNS Discovery ───────────────────────────────────────────────────────────

#ifdef __APPLE__

// Bonjour resolve callback
static void DNSSD_API resolve_callback(
    DNSServiceRef /*ref*/, DNSServiceFlags /*flags*/, uint32_t /*ifIndex*/,
    DNSServiceErrorType errorCode,
    const char* /*fullname*/, const char* hosttarget, uint16_t port,
    uint16_t /*txtLen*/, const unsigned char* /*txtRecord*/,
    void* context) {

    if (errorCode != kDNSServiceErr_NoError) return;

    auto* info = static_cast<std::pair<AirPlaySender*, std::string>*>(context);
    AirPlaySender* sender = info->first;
    std::string name = info->second;

    // Resolve hostname to IP
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    std::string host_str = hosttarget;
    // Remove trailing dot if present
    if (!host_str.empty() && host_str.back() == '.') {
        host_str.pop_back();
    }

    if (getaddrinfo(host_str.c_str(), nullptr, &hints, &res) == 0 && res) {
        char ip[INET_ADDRSTRLEN];
        auto* sa = reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
        inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
        freeaddrinfo(res);

        sender->on_speaker_found(name, ip, ntohs(port));
    } else {
        // Try using hostname directly
        sender->on_speaker_found(name, host_str, ntohs(port));
    }

    delete info;
}

// Bonjour browse callback
static void DNSSD_API browse_callback(
    DNSServiceRef /*ref*/, DNSServiceFlags flags, uint32_t ifIndex,
    DNSServiceErrorType errorCode,
    const char* serviceName, const char* regtype, const char* replyDomain,
    void* context) {

    if (errorCode != kDNSServiceErr_NoError) return;

    auto* state = static_cast<AirPlaySender::BrowseState*>(context);
    AirPlaySender* sender = state->sender;

    if (flags & kDNSServiceFlagsAdd) {
        fprintf(stderr, "[airplay-tx] Found AirPlay speaker: %s\n", serviceName);

        // Resolve to get host:port
        DNSServiceRef resolve_ref = nullptr;
        auto* info = new std::pair<AirPlaySender*, std::string>(sender, serviceName);

        DNSServiceErrorType err = DNSServiceResolve(
            &resolve_ref, 0, ifIndex,
            serviceName, regtype, replyDomain,
            resolve_callback, info);

        if (err == kDNSServiceErr_NoError && resolve_ref) {
            // Process the resolve synchronously (with timeout)
            int fd = DNSServiceRefSockFD(resolve_ref);
            struct timeval tv{5, 0}; // 5 second timeout
            fd_set r;
            FD_ZERO(&r);
            FD_SET(fd, &r);
            if (select(fd + 1, &r, nullptr, nullptr, &tv) > 0) {
                DNSServiceProcessResult(resolve_ref);
            } else {
                delete info; // timeout — callback won't fire
            }
            DNSServiceRefDeallocate(resolve_ref);
        } else {
            delete info;
        }
    } else {
        // Speaker removed
        sender->on_speaker_removed(serviceName);
    }
}

bool AirPlaySender::start_mdns_browse() {
    DNSServiceErrorType err = DNSServiceBrowse(
        &browse_->browse_ref, 0, 0,
        "_raop._tcp", nullptr,
        browse_callback, browse_.get());

    if (err != kDNSServiceErr_NoError) {
        fprintf(stderr, "[airplay-tx] Failed to start mDNS browse: %d\n", err);
        return false;
    }

    // Pump mDNS in background
    browse_->pump_running.store(true);
    browse_->pump_thread = std::thread([this]() {
        while (browse_->pump_running.load() && running_.load()) {
            int fd = DNSServiceRefSockFD(browse_->browse_ref);
            if (fd < 0) break;
            fd_set r;
            FD_ZERO(&r);
            FD_SET(fd, &r);
            struct timeval tv{1, 0};
            if (select(fd + 1, &r, nullptr, nullptr, &tv) > 0) {
                DNSServiceProcessResult(browse_->browse_ref);
            }
        }
    });

    fprintf(stderr, "[airplay-tx] mDNS: browsing for _raop._tcp speakers\n");
    return true;
}

void AirPlaySender::stop_mdns_browse() {
    browse_->pump_running.store(false);
    if (browse_->pump_thread.joinable()) browse_->pump_thread.join();
    if (browse_->browse_ref) {
        DNSServiceRefDeallocate(browse_->browse_ref);
        browse_->browse_ref = nullptr;
    }
}

#elif defined(SOLUNA_HAS_AVAHI)

// Avahi resolve callback
static void avahi_resolve_callback(
    AvahiServiceResolver* r, AvahiIfIndex /*iface*/, AvahiProtocol /*proto*/,
    AvahiResolverEvent event, const char* name, const char* /*type*/,
    const char* /*domain*/, const char* /*host_name*/,
    const AvahiAddress* address, uint16_t port,
    AvahiStringList* /*txt*/, AvahiLookupResultFlags /*flags*/,
    void* userdata) {

    auto* state = static_cast<AirPlaySender::BrowseState*>(userdata);

    if (event == AVAHI_RESOLVER_FOUND && address) {
        char ip[AVAHI_ADDRESS_STR_MAX];
        avahi_address_snprint(ip, sizeof(ip), address);
        state->sender->on_speaker_found(name ? name : "", ip, port);
    }

    avahi_service_resolver_free(r);
}

// Avahi browse callback
static void avahi_browse_callback(
    AvahiServiceBrowser* /*b*/, AvahiIfIndex iface, AvahiProtocol proto,
    AvahiBrowserEvent event, const char* name, const char* type,
    const char* domain, AvahiLookupResultFlags /*flags*/,
    void* userdata) {

    auto* state = static_cast<AirPlaySender::BrowseState*>(userdata);

    if (event == AVAHI_BROWSER_NEW) {
        fprintf(stderr, "[airplay-tx] Found AirPlay speaker: %s\n", name);
        avahi_service_resolver_new(
            state->client, iface, proto, name, type, domain,
            AVAHI_PROTO_UNSPEC, (AvahiLookupFlags)0,
            avahi_resolve_callback, userdata);
    } else if (event == AVAHI_BROWSER_REMOVE) {
        state->sender->on_speaker_removed(name ? name : "");
    }
}

bool AirPlaySender::start_mdns_browse() {
    browse_->poll = avahi_simple_poll_new();
    if (!browse_->poll) return false;

    int err = 0;
    browse_->client = avahi_client_new(
        avahi_simple_poll_get(browse_->poll), (AvahiClientFlags)0,
        nullptr, nullptr, &err);
    if (!browse_->client) {
        fprintf(stderr, "[airplay-tx] Failed to create Avahi client: %s\n",
                avahi_strerror(err));
        avahi_simple_poll_free(browse_->poll);
        browse_->poll = nullptr;
        return false;
    }

    browse_->browser = avahi_service_browser_new(
        browse_->client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
        "_raop._tcp", nullptr, (AvahiLookupFlags)0,
        avahi_browse_callback, browse_.get());
    if (!browse_->browser) {
        fprintf(stderr, "[airplay-tx] Failed to create Avahi browser\n");
        return false;
    }

    browse_->poll_running.store(true);
    browse_->poll_thread = std::thread([this]() {
        while (browse_->poll_running.load() && running_.load()) {
            avahi_simple_poll_iterate(browse_->poll, 1000);
        }
    });

    fprintf(stderr, "[airplay-tx] mDNS: browsing for _raop._tcp via Avahi\n");
    return true;
}

void AirPlaySender::stop_mdns_browse() {
    browse_->poll_running.store(false);
    if (browse_->poll) avahi_simple_poll_quit(browse_->poll);
    if (browse_->poll_thread.joinable()) browse_->poll_thread.join();
    if (browse_->browser) avahi_service_browser_free(browse_->browser);
    if (browse_->client) avahi_client_free(browse_->client);
    if (browse_->poll) avahi_simple_poll_free(browse_->poll);
    browse_->browser = nullptr;
    browse_->client = nullptr;
    browse_->poll = nullptr;
}

#else

// No mDNS backend — stubs
bool AirPlaySender::start_mdns_browse() {
    fprintf(stderr, "[airplay-tx] mDNS: no backend available (need Bonjour or Avahi)\n");
    return false;
}
void AirPlaySender::stop_mdns_browse() {}

#endif

// ── Speaker Found/Removed ────────────────────────────────────────────────────

void AirPlaySender::on_speaker_found(const std::string& name,
                                      const std::string& host, uint16_t port) {
    std::lock_guard<std::mutex> lock(speakers_mutex_);

    // Check for duplicate
    for (const auto& spk : speakers_) {
        if (spk.host == host && spk.port == port) {
            fprintf(stderr, "[airplay-tx] Speaker '%s' already known (%s:%u)\n",
                    name.c_str(), host.c_str(), port);
            return;
        }
    }

    // Filter out ourselves (Soluna receiver)
    // The _raop._tcp name format is "AABBCCDDEEFF@DeviceName"
    // Skip speakers named "*@Soluna" to avoid feedback loops
    if (name.find("@Soluna") != std::string::npos) {
        fprintf(stderr, "[airplay-tx] Skipping our own receiver: %s\n", name.c_str());
        return;
    }

    fprintf(stderr, "[airplay-tx] Connecting to speaker '%s' at %s:%u\n",
            name.c_str(), host.c_str(), port);

    AirPlaySpeaker spk;
    spk.name = name;
    spk.host = host;
    spk.port = port;
    spk.ssrc = generate_ssrc();
    spk.rtp_seq = 0;
    spk.rtp_timestamp = 0;

    // Connect and set up RTSP session
    if (rtsp_connect(spk) && rtsp_announce(spk) && rtsp_setup(spk) && rtsp_record(spk)) {
        spk.active = true;
        speakers_.push_back(std::move(spk));
        fprintf(stderr, "[airplay-tx] Speaker '%s' connected and streaming\n", name.c_str());
    } else {
        fprintf(stderr, "[airplay-tx] Failed to connect to speaker '%s'\n", name.c_str());
        if (spk.rtsp_fd >= 0) close(spk.rtsp_fd);
        if (spk.audio_fd >= 0) close(spk.audio_fd);
    }
}

void AirPlaySender::on_speaker_removed(const std::string& name) {
    std::lock_guard<std::mutex> lock(speakers_mutex_);
    auto it = std::remove_if(speakers_.begin(), speakers_.end(),
        [&](AirPlaySpeaker& spk) {
            // Match by name suffix (raop name is "MAC@Name")
            if (spk.name == name || name.find(spk.name) != std::string::npos) {
                fprintf(stderr, "[airplay-tx] Speaker '%s' removed\n", spk.name.c_str());
                rtsp_teardown(spk);
                return true;
            }
            return false;
        });
    speakers_.erase(it, speakers_.end());
}

// ── RTSP Client ──────────────────────────────────────────────────────────────

std::string AirPlaySender::rtsp_send_receive(int fd, const std::string& request) {
    // Send request
    ssize_t sent = send(fd, request.c_str(), request.size(), 0);
    if (sent < 0) {
        fprintf(stderr, "[airplay-tx] RTSP send error: %s\n", strerror(errno));
        return "";
    }

    // Receive response (wait up to 5 seconds)
    struct pollfd pfd{fd, POLLIN, 0};
    if (poll(&pfd, 1, 5000) <= 0) {
        fprintf(stderr, "[airplay-tx] RTSP response timeout\n");
        return "";
    }

    char buf[4096];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return "";
    buf[n] = '\0';
    return std::string(buf, n);
}

bool AirPlaySender::rtsp_connect(AirPlaySpeaker& speaker) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    // Set TCP_NODELAY for low-latency RTSP
    int optval = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));

    // Connect with timeout
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(speaker.port);

    if (inet_pton(AF_INET, speaker.host.c_str(), &addr.sin_addr) != 1) {
        // Try DNS resolution
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(speaker.host.c_str(), nullptr, &hints, &res) != 0 || !res) {
            close(fd);
            return false;
        }
        addr.sin_addr = reinterpret_cast<struct sockaddr_in*>(res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }

    // Non-blocking connect with timeout
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int ret = connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (ret < 0 && errno == EINPROGRESS) {
        struct pollfd pfd{fd, POLLOUT, 0};
        if (poll(&pfd, 1, 5000) <= 0) {
            close(fd);
            return false;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
            close(fd);
            return false;
        }
    } else if (ret < 0) {
        close(fd);
        return false;
    }

    // Back to blocking mode
    fcntl(fd, F_SETFL, flags);

    speaker.rtsp_fd = fd;
    return true;
}

bool AirPlaySender::rtsp_announce(AirPlaySpeaker& speaker) {
    // Build SDP body
    std::string fmtp = encoder_.fmtp_line();
    std::ostringstream sdp;
    sdp << "v=0\r\n"
        << "o=iTunes " << speaker.ssrc << " O IN IP4 0.0.0.0\r\n"
        << "s=iTunes\r\n"
        << "c=IN IP4 " << speaker.host << "\r\n"
        << "t=0 0\r\n"
        << "m=audio 0 RTP/AVP 96\r\n"
        << "a=rtpmap:96 AppleLossless\r\n"
        << "a=fmtp:" << fmtp << "\r\n";
    std::string sdp_str = sdp.str();

    int cseq = ++cseq_;
    std::ostringstream req;
    req << "ANNOUNCE rtsp://" << speaker.host << ":" << speaker.port << "/" << speaker.ssrc << " RTSP/1.0\r\n"
        << "CSeq: " << cseq << "\r\n"
        << "Content-Type: application/sdp\r\n"
        << "Content-Length: " << sdp_str.size() << "\r\n"
        << "User-Agent: Soluna/1.0\r\n"
        << "\r\n"
        << sdp_str;

    std::string resp = rtsp_send_receive(speaker.rtsp_fd, req.str());
    if (resp.empty() || resp.find("200") == std::string::npos) {
        fprintf(stderr, "[airplay-tx] ANNOUNCE failed: %s\n",
                resp.empty() ? "(no response)" : resp.substr(0, 60).c_str());
        return false;
    }
    return true;
}

bool AirPlaySender::rtsp_setup(AirPlaySpeaker& speaker) {
    // Allocate local UDP ports
    speaker.local_audio_port = alloc_udp_port();
    speaker.local_control_port = alloc_udp_port();
    speaker.local_timing_port = alloc_udp_port();

    int cseq = ++cseq_;
    std::ostringstream req;
    req << "SETUP rtsp://" << speaker.host << ":" << speaker.port << "/" << speaker.ssrc << " RTSP/1.0\r\n"
        << "CSeq: " << cseq << "\r\n"
        << "Transport: RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;"
        << "control_port=" << speaker.local_control_port << ";"
        << "timing_port=" << speaker.local_timing_port << "\r\n"
        << "User-Agent: Soluna/1.0\r\n"
        << "\r\n";

    std::string resp = rtsp_send_receive(speaker.rtsp_fd, req.str());
    if (resp.empty() || resp.find("200") == std::string::npos) {
        fprintf(stderr, "[airplay-tx] SETUP failed: %s\n",
                resp.empty() ? "(no response)" : resp.substr(0, 80).c_str());
        return false;
    }

    // Parse server's transport response for port assignments
    auto parse_port = [&](const std::string& key) -> uint16_t {
        auto pos = resp.find(key + "=");
        if (pos == std::string::npos) return 0;
        return static_cast<uint16_t>(std::stoi(resp.substr(pos + key.size() + 1)));
    };

    speaker.server_audio_port = parse_port("server_port");
    speaker.server_control_port = parse_port("control_port");
    speaker.server_timing_port = parse_port("timing_port");

    if (speaker.server_audio_port == 0) {
        fprintf(stderr, "[airplay-tx] SETUP: no server_port in response\n");
        return false;
    }

    // Create UDP socket for audio transmission
    speaker.audio_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (speaker.audio_fd < 0) {
        fprintf(stderr, "[airplay-tx] Failed to create audio UDP socket\n");
        return false;
    }

    // Bind to local audio port
    struct sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(speaker.local_audio_port);
    if (::bind(speaker.audio_fd, reinterpret_cast<struct sockaddr*>(&local_addr),
               sizeof(local_addr)) < 0) {
        fprintf(stderr, "[airplay-tx] Failed to bind audio port %u: %s\n",
                speaker.local_audio_port, strerror(errno));
        // Try with any port
        local_addr.sin_port = 0;
        ::bind(speaker.audio_fd, reinterpret_cast<struct sockaddr*>(&local_addr),
               sizeof(local_addr));
    }

    fprintf(stderr, "[airplay-tx] SETUP complete: server audio=%u control=%u timing=%u\n",
            speaker.server_audio_port, speaker.server_control_port,
            speaker.server_timing_port);
    return true;
}

bool AirPlaySender::rtsp_record(AirPlaySpeaker& speaker) {
    int cseq = ++cseq_;
    std::ostringstream req;
    req << "RECORD rtsp://" << speaker.host << ":" << speaker.port << "/" << speaker.ssrc << " RTSP/1.0\r\n"
        << "CSeq: " << cseq << "\r\n"
        << "Range: npt=0-\r\n"
        << "Session: 1\r\n"
        << "RTP-Info: seq=" << speaker.rtp_seq << ";rtptime=" << speaker.rtp_timestamp << "\r\n"
        << "User-Agent: Soluna/1.0\r\n"
        << "\r\n";

    std::string resp = rtsp_send_receive(speaker.rtsp_fd, req.str());
    if (resp.empty() || resp.find("200") == std::string::npos) {
        fprintf(stderr, "[airplay-tx] RECORD failed: %s\n",
                resp.empty() ? "(no response)" : resp.substr(0, 60).c_str());
        return false;
    }
    return true;
}

void AirPlaySender::rtsp_teardown(AirPlaySpeaker& speaker) {
    if (speaker.rtsp_fd >= 0) {
        int cseq = ++cseq_;
        std::ostringstream req;
        req << "TEARDOWN rtsp://" << speaker.host << ":" << speaker.port << "/" << speaker.ssrc << " RTSP/1.0\r\n"
            << "CSeq: " << cseq << "\r\n"
            << "Session: 1\r\n"
            << "User-Agent: Soluna/1.0\r\n"
            << "\r\n";

        // Best-effort send, don't wait for response
        send(speaker.rtsp_fd, req.str().c_str(), req.str().size(), 0);
        close(speaker.rtsp_fd);
        speaker.rtsp_fd = -1;
    }
    if (speaker.audio_fd >= 0) {
        close(speaker.audio_fd);
        speaker.audio_fd = -1;
    }
    speaker.active = false;
}

// ── RTP Audio Streaming ──────────────────────────────────────────────────────

void AirPlaySender::send_rtp_audio(AirPlaySpeaker& speaker,
                                    const uint8_t* alac_data, size_t alac_len) {
    // Build RTP packet:
    //   12 bytes RTP header + ALAC payload
    constexpr size_t kRtpHeaderSize = 12;
    std::vector<uint8_t> pkt(kRtpHeaderSize + alac_len);

    // RTP header
    pkt[0] = 0x80; // V=2, P=0, X=0, CC=0
    pkt[1] = 96;   // M=0, PT=96 (ALAC)

    // Sequence number (network byte order)
    pkt[2] = static_cast<uint8_t>((speaker.rtp_seq >> 8) & 0xFF);
    pkt[3] = static_cast<uint8_t>(speaker.rtp_seq & 0xFF);
    speaker.rtp_seq++;

    // Timestamp (network byte order)
    pkt[4] = static_cast<uint8_t>((speaker.rtp_timestamp >> 24) & 0xFF);
    pkt[5] = static_cast<uint8_t>((speaker.rtp_timestamp >> 16) & 0xFF);
    pkt[6] = static_cast<uint8_t>((speaker.rtp_timestamp >> 8) & 0xFF);
    pkt[7] = static_cast<uint8_t>(speaker.rtp_timestamp & 0xFF);
    speaker.rtp_timestamp += encoder_.frame_length();

    // SSRC (network byte order)
    pkt[8]  = static_cast<uint8_t>((speaker.ssrc >> 24) & 0xFF);
    pkt[9]  = static_cast<uint8_t>((speaker.ssrc >> 16) & 0xFF);
    pkt[10] = static_cast<uint8_t>((speaker.ssrc >> 8) & 0xFF);
    pkt[11] = static_cast<uint8_t>(speaker.ssrc & 0xFF);

    // ALAC payload
    memcpy(pkt.data() + kRtpHeaderSize, alac_data, alac_len);

    // Send to speaker's audio port
    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(speaker.server_audio_port);
    inet_pton(AF_INET, speaker.host.c_str(), &dest.sin_addr);

    ssize_t sent = sendto(speaker.audio_fd, pkt.data(), pkt.size(), 0,
                          reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        // Connection lost — mark speaker as inactive
        fprintf(stderr, "[airplay-tx] Send error to '%s': %s\n",
                speaker.name.c_str(), strerror(errno));
        speaker.active = false;
    }
}

// ── Timing Sync Thread ───────────────────────────────────────────────────────
// AirPlay uses NTP-like timing sync on UDP ports 7010/7011.
// The receiver expects periodic timing requests to maintain sync.
// We send timing request packets every ~3 seconds to each speaker.

void AirPlaySender::timing_thread_func() {
    while (running_.load()) {
        // Sleep 3 seconds between timing sync rounds
        for (int i = 0; i < 30 && running_.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!running_.load()) break;

        // Get current NTP-like timestamp (seconds since 1900-01-01)
        auto now = std::chrono::system_clock::now();
        auto epoch = now.time_since_epoch();
        // NTP epoch offset: seconds from 1900-01-01 to 1970-01-01
        constexpr uint64_t kNtpEpochOffset = 2208988800ULL;
        uint64_t ntp_secs = std::chrono::duration_cast<std::chrono::seconds>(epoch).count()
                            + kNtpEpochOffset;
        uint64_t ntp_frac = std::chrono::duration_cast<std::chrono::nanoseconds>(epoch).count()
                            % 1000000000ULL;
        // Convert fraction to NTP fractional seconds (2^32 / 10^9)
        uint32_t ntp_frac32 = static_cast<uint32_t>(
            (ntp_frac * 4294967296ULL) / 1000000000ULL);

        // Build timing request packet (32 bytes)
        // AirPlay timing packet format:
        //   Byte 0: 0x80 (RTP v2)
        //   Byte 1: 0xD3 (PT=0x53 | M=1) — timing request
        //   Bytes 2-3: sequence (0)
        //   Bytes 4-7: 0
        //   Bytes 8-15: origin timestamp (NTP, our send time)
        //   Bytes 16-23: receive timestamp (0 for request)
        //   Bytes 24-31: transmit timestamp (NTP, our send time)
        uint8_t timing_pkt[32] = {};
        timing_pkt[0] = 0x80;
        timing_pkt[1] = 0xD3; // M=1, PT=0x53

        auto write_ntp = [](uint8_t* dst, uint32_t secs, uint32_t frac) {
            dst[0] = (secs >> 24) & 0xFF;
            dst[1] = (secs >> 16) & 0xFF;
            dst[2] = (secs >> 8) & 0xFF;
            dst[3] = secs & 0xFF;
            dst[4] = (frac >> 24) & 0xFF;
            dst[5] = (frac >> 16) & 0xFF;
            dst[6] = (frac >> 8) & 0xFF;
            dst[7] = frac & 0xFF;
        };

        uint32_t secs32 = static_cast<uint32_t>(ntp_secs);
        write_ntp(timing_pkt + 24, secs32, ntp_frac32); // transmit timestamp
        // origin timestamp = same as transmit for initial request
        write_ntp(timing_pkt + 8, secs32, ntp_frac32);

        std::lock_guard<std::mutex> lock(speakers_mutex_);
        for (auto& spk : speakers_) {
            if (!spk.active || spk.server_timing_port == 0) continue;

            // Create a UDP socket for timing if we don't have one
            // (reuse audio_fd with different dest port for simplicity)
            int timing_fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (timing_fd < 0) continue;

            struct sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_port = htons(spk.server_timing_port);
            inet_pton(AF_INET, spk.host.c_str(), &dest.sin_addr);

            sendto(timing_fd, timing_pkt, sizeof(timing_pkt), 0,
                   reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
            close(timing_fd);
        }
    }
}

// ── Helpers ──────────────────────────────────────────────────────────────────

uint16_t AirPlaySender::alloc_udp_port() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;

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

uint32_t AirPlaySender::generate_ssrc() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    return dist(gen);
}

} // namespace soluna::transport

#endif // SOLUNA_HAS_AIRPLAY
