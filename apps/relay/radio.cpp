// soluna-radio — multi-channel music streamer to relay as OSTP RTP packets
//
// Single-channel mode (backward compatible):
//   soluna-radio --dir /data/music --relay HOST:PORT --channel NAME
//
// Multi-channel mode (new):
//   soluna-radio --config /etc/soluna-radio.json --relay HOST:PORT
//
// Decodes audio via ffmpeg -> 48kHz mono 24-bit PCM -> OSTP/RTP -> UDP to relay.
// In multi-channel mode, one thread per channel shares a single UDP socket.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>
#include <csignal>
#include <dirent.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>

// ── Minimal JSON config parser (no external dependencies) ──

struct ChannelConfig {
    std::string name;
    std::string dir;
};

static std::string json_string_value(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    auto end = json.find('"', pos + 1);
    if (end == std::string::npos) return "";
    return json.substr(pos + 1, end - pos - 1);
}

static std::vector<ChannelConfig> parse_config(const std::string& path) {
    std::vector<ChannelConfig> channels;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        fprintf(stderr, "[radio] Cannot open config: %s\n", path.c_str());
        return channels;
    }
    // Read entire file
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string content(sz, '\0');
    fread(&content[0], 1, sz, f);
    fclose(f);

    // Find each object in "channels" array
    auto arr_pos = content.find("\"channels\"");
    if (arr_pos == std::string::npos) return channels;
    auto brace = content.find('[', arr_pos);
    if (brace == std::string::npos) return channels;

    size_t pos = brace + 1;
    while (true) {
        auto obj_start = content.find('{', pos);
        if (obj_start == std::string::npos) break;
        auto obj_end = content.find('}', obj_start);
        if (obj_end == std::string::npos) break;
        // Check we haven't passed the closing ']'
        auto arr_end = content.find(']', brace);
        if (arr_end != std::string::npos && obj_start > arr_end) break;

        std::string obj = content.substr(obj_start, obj_end - obj_start + 1);
        ChannelConfig ch;
        ch.name = json_string_value(obj, "name");
        ch.dir = json_string_value(obj, "dir");
        if (!ch.name.empty() && !ch.dir.empty()) {
            channels.push_back(ch);
        }
        pos = obj_end + 1;
    }
    return channels;
}

// ── Constants ──

static const uint32_t SAMPLE_RATE = 48000;
static const uint32_t CHANNELS    = 1;
static const uint32_t FRAMES_PER_PKT = 96;  // 2ms at 48kHz mono (96*1*4=384 bytes/pkt, ~500pps)
static const uint8_t  PT_L24     = 96;      // OSTP 24-bit linear PCM
static const uint8_t  PT_ADPCM   = 116;     // IMA-ADPCM mono (OSTP S4.9)
static const uint32_t SSRC       = 0x524144;  // "RAD"
static const uint16_t OSTP_PROFILE = 0x4F53;  // "OS"
static const uint32_t RAW_FIRST_PKTS = 5;  // S4.9: 5 raw PCM packets then switch to ADPCM

// ── IMA-ADPCM inline encoder (from soluna/codec/adpcm.h) ──

static const int16_t kStepTable[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,
    50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,
    253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,
    1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,
    3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,
    10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,
    27086,29794,32767
};
static const int8_t kIndexTable[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};

struct AdpcmState { int32_t valprev = 0; int32_t index = 0; };

static uint8_t adpcm_encode_one(int16_t sample, AdpcmState& s) {
    int step = kStepTable[s.index];
    int diff = sample - s.valprev;
    uint8_t nib = 0;
    if (diff < 0) { nib = 8; diff = -diff; }
    if (diff >= step) { nib |= 4; diff -= step; }
    if (diff >= (step >> 1)) { nib |= 2; diff -= (step >> 1); }
    if (diff >= (step >> 2)) { nib |= 1; }
    int dq = kStepTable[s.index] >> 3;
    if (nib & 4) dq += kStepTable[s.index];
    if (nib & 2) dq += (kStepTable[s.index] >> 1);
    if (nib & 1) dq += (kStepTable[s.index] >> 2);
    s.valprev += (nib & 8) ? -dq : dq;
    if (s.valprev > 32767) s.valprev = 32767;
    if (s.valprev < -32768) s.valprev = -32768;
    s.index += kIndexTable[nib];
    if (s.index < 0) s.index = 0;
    if (s.index > 88) s.index = 88;
    return nib;
}

// ── OSTP packet builder (inlined from transport library) ──

static uint32_t ostp_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}

// RTP(12) + ExtHdr(4) + OSTP(8) = 24 bytes header
static const size_t HEADER_SIZE = 24;
static const size_t CRC_SIZE = 4;

static size_t build_ostp_packet(
    uint8_t* buf, size_t buf_size,
    uint32_t ssrc, uint16_t seq, uint32_t rtp_ts,
    uint8_t pt, uint16_t stream_id, uint16_t seq_ext,
    uint32_t media_ts,
    const uint8_t* payload, size_t payload_size)
{
    size_t total = HEADER_SIZE + payload_size + CRC_SIZE;
    if (buf_size < total) return 0;

    memset(buf, 0, HEADER_SIZE);

    // RTP header (12 bytes) — bitfield layout is compiler-dependent,
    // so write bytes directly for portability.
    // Byte 0: V=2, P=0, X=1, CC=0 -> 0b10_0_1_0000 = 0x90
    buf[0] = 0x90;
    // Byte 1: M=0, PT
    buf[1] = pt & 0x7F;
    // Bytes 2-3: sequence (network order)
    buf[2] = (uint8_t)(seq >> 8);
    buf[3] = (uint8_t)(seq);
    // Bytes 4-7: timestamp
    buf[4] = (uint8_t)(rtp_ts >> 24);
    buf[5] = (uint8_t)(rtp_ts >> 16);
    buf[6] = (uint8_t)(rtp_ts >> 8);
    buf[7] = (uint8_t)(rtp_ts);
    // Bytes 8-11: SSRC
    buf[8]  = (uint8_t)(ssrc >> 24);
    buf[9]  = (uint8_t)(ssrc >> 16);
    buf[10] = (uint8_t)(ssrc >> 8);
    buf[11] = (uint8_t)(ssrc);

    // RTP extension header (4 bytes)
    buf[12] = (uint8_t)(OSTP_PROFILE >> 8);  // "OS"
    buf[13] = (uint8_t)(OSTP_PROFILE);
    buf[14] = 0x00; buf[15] = 0x02;  // length = 2 words (8 bytes)

    // OSTP header (8 bytes)
    buf[16] = (uint8_t)(stream_id >> 8);
    buf[17] = (uint8_t)(stream_id);
    buf[18] = (uint8_t)(seq_ext >> 8);
    buf[19] = (uint8_t)(seq_ext);
    buf[20] = (uint8_t)(media_ts >> 24);
    buf[21] = (uint8_t)(media_ts >> 16);
    buf[22] = (uint8_t)(media_ts >> 8);
    buf[23] = (uint8_t)(media_ts);

    // Payload
    memcpy(buf + HEADER_SIZE, payload, payload_size);

    // CRC-32 trailer
    uint32_t crc = ostp_crc32(buf + HEADER_SIZE, payload_size);
    buf[HEADER_SIZE + payload_size + 0] = (uint8_t)(crc >> 24);
    buf[HEADER_SIZE + payload_size + 1] = (uint8_t)(crc >> 16);
    buf[HEADER_SIZE + payload_size + 2] = (uint8_t)(crc >> 8);
    buf[HEADER_SIZE + payload_size + 3] = (uint8_t)(crc);

    return total;
}

// ── File scanning ──

static std::vector<std::string> scan_music(const std::string& dir) {
    std::vector<std::string> files;
    DIR* d = opendir(dir.c_str());
    if (!d) return files;
    struct dirent* ent;
    while ((ent = readdir(d))) {
        std::string name = ent->d_name;
        if (name.size() < 4) continue;
        std::string ext = name.substr(name.size() - 4);
        for (auto& c : ext) c = tolower(c);
        if (ext == ".mp3" || ext == ".wav" || ext == "flac" || ext == ".ogg" || ext == ".m4a")
            files.push_back(dir + "/" + name);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    return files;
}

// ── Relay protocol messages ──
// Socket sends are serialized with a mutex (UDP sendto is not thread-safe on all platforms)

static std::mutex g_sock_mutex;

static void send_join(int sock, const struct sockaddr_in& relay, const std::string& group) {
    std::string msg = "JOIN:" + group + "\n";
    std::lock_guard<std::mutex> lock(g_sock_mutex);
    sendto(sock, msg.c_str(), msg.size(), 0,
           (struct sockaddr*)&relay, sizeof(relay));
}

static void send_meta(int sock, const struct sockaddr_in& relay,
                      const std::string& group, const std::string& title) {
    std::string msg = "META:" + group + " {\"title\":\"" + title + "\"}\n";
    std::lock_guard<std::mutex> lock(g_sock_mutex);
    sendto(sock, msg.c_str(), msg.size(), 0,
           (struct sockaddr*)&relay, sizeof(relay));
}

static void send_packet(int sock, const struct sockaddr_in& relay,
                        const uint8_t* pkt, size_t pkt_size) {
    std::lock_guard<std::mutex> lock(g_sock_mutex);
    sendto(sock, pkt, pkt_size, 0,
           (struct sockaddr*)&relay, sizeof(relay));
}

// ── Graceful shutdown ──

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running.store(false);
}

// ── RadioChannel: encapsulates one channel's streaming state and loop ──

struct RadioChannel {
    std::string name;
    std::string dir;
    std::vector<std::string> files;

    // Shared resources (not owned)
    int sock;
    struct sockaddr_in relay_addr;

    // Per-channel RTP state
    uint16_t seq = 0;
    uint16_t seq_ext = 0;
    uint32_t rtp_ts = 0;
    time_t last_join = 0;
    uint16_t stream_id = (CHANNELS << 10) | 0;

    RadioChannel(const std::string& ch_name, const std::string& ch_dir,
                 int udp_sock, const struct sockaddr_in& relay)
        : name(ch_name), dir(ch_dir), sock(udp_sock), relay_addr(relay) {}

    bool init() {
        files = scan_music(dir);
        if (files.empty()) {
            fprintf(stderr, "[radio:%s] No audio files found in %s\n",
                    name.c_str(), dir.c_str());
            return false;
        }
        fprintf(stderr, "[radio:%s] Found %zu files\n", name.c_str(), files.size());
        return true;
    }

    // Run the streaming loop (blocking — intended to be called from a thread).
    // Returns when g_running becomes false.
    void run() {
        send_join(sock, relay_addr, name);
        last_join = time(nullptr);
        fprintf(stderr, "[radio:%s] Started streaming\n", name.c_str());

        uint8_t pkt[2048];

        for (size_t idx = 0; g_running.load(); idx = (idx + 1) % files.size()) {
            const auto& filepath = files[idx];
            auto slash = filepath.rfind('/');
            std::string filename = (slash != std::string::npos)
                ? filepath.substr(slash + 1) : filepath;

            fprintf(stderr, "[radio:%s] Now playing: %s (%zu/%zu)\n",
                    name.c_str(), filename.c_str(), idx + 1, files.size());
            send_meta(sock, relay_addr, name, filename);

            // ffmpeg: decode to 48kHz mono s32le
            char cmd[2048];
            snprintf(cmd, sizeof(cmd),
                "ffmpeg -v error -i '%s' -f s32le -acodec pcm_s32le -ar %u -ac %u - 2>/dev/null",
                filepath.c_str(), SAMPLE_RATE, CHANNELS);

            FILE* pipe = popen(cmd, "r");
            if (!pipe) {
                fprintf(stderr, "[radio:%s] Failed to open: %s\n",
                        name.c_str(), filepath.c_str());
                continue;
            }

            int32_t pcm_buf[FRAMES_PER_PKT * CHANNELS];
            AdpcmState adpcm_state;
            uint32_t pkt_count = 0;

            // Timing
            struct timespec next_send;
            clock_gettime(CLOCK_MONOTONIC, &next_send);
            long pkt_ns = (long)(FRAMES_PER_PKT * 1000000000ULL / SAMPLE_RATE);

            while (g_running.load()) {
                size_t nread = fread(pcm_buf, sizeof(int32_t),
                                     FRAMES_PER_PKT * CHANNELS, pipe);
                if (nread == 0) break;

                size_t frames = nread / CHANNELS;

                // OSTP convention: 24-bit PCM stored in int32_t (range +/-8388607).
                // ffmpeg s32le uses full 32-bit range -> shift right 8 bits.
                for (size_t i = 0; i < nread; i++)
                    pcm_buf[i] >>= 8;

                // media_timestamp: wall-clock milliseconds (32-bit, 49-day rollover)
                struct timespec now_ts;
                clock_gettime(CLOCK_REALTIME, &now_ts);
                uint32_t media_ts = (uint32_t)(
                    (now_ts.tv_sec * 1000ULL + now_ts.tv_nsec / 1000000ULL) & 0xFFFFFFFF);

                const uint8_t* payload;
                size_t payload_size;
                uint8_t pt;
                uint8_t adpcm_buf[4 + FRAMES_PER_PKT];

                if (pkt_count < RAW_FIRST_PKTS) {
                    // S4.9 Raw First: send raw PCM for instant DMA playback
                    payload = reinterpret_cast<const uint8_t*>(pcm_buf);
                    payload_size = nread * sizeof(int32_t);
                    pt = PT_L24;

                    // Seed ADPCM state from last sample
                    int16_t last_sample = (int16_t)(pcm_buf[nread - 1] >> 8);
                    adpcm_state.valprev = last_sample;
                    adpcm_state.index = 40;
                } else {
                    // ADPCM mode: 4:1 bandwidth reduction
                    int16_t pcm16[FRAMES_PER_PKT * CHANNELS];
                    for (size_t i = 0; i < nread; i++)
                        pcm16[i] = (int16_t)(pcm_buf[i] >> 8);

                    adpcm_buf[0] = (uint8_t)(adpcm_state.valprev & 0xFF);
                    adpcm_buf[1] = (uint8_t)((adpcm_state.valprev >> 8) & 0xFF);
                    adpcm_buf[2] = (uint8_t)adpcm_state.index;
                    adpcm_buf[3] = 0;
                    for (size_t i = 0; i < nread; i++) {
                        uint8_t nib = adpcm_encode_one(pcm16[i], adpcm_state);
                        if (i & 1) adpcm_buf[4 + i/2] |= (nib << 4);
                        else       adpcm_buf[4 + i/2] = nib;
                    }

                    payload = adpcm_buf;
                    payload_size = 4 + (nread + 1) / 2;
                    pt = PT_ADPCM;
                }

                size_t pkt_size = build_ostp_packet(
                    pkt, sizeof(pkt),
                    SSRC, seq, rtp_ts,
                    pt, stream_id, seq_ext, media_ts,
                    payload, payload_size);

                if (pkt_size > 0) {
                    send_packet(sock, relay_addr, pkt, pkt_size);
                }

                seq++;
                if (seq == 0) seq_ext++;
                rtp_ts += (uint32_t)frames;
                pkt_count++;

                // Keep-alive
                time_t now = time(nullptr);
                if (now - last_join >= 5) {
                    send_join(sock, relay_addr, name);
                    last_join = now;
                }

                // Pace to real-time
                next_send.tv_nsec += pkt_ns;
                if (next_send.tv_nsec >= 1000000000L) {
                    next_send.tv_sec++;
                    next_send.tv_nsec -= 1000000000L;
                }
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_send, NULL);
            }

            pclose(pipe);
            fprintf(stderr, "[radio:%s] Track finished: %s\n",
                    name.c_str(), filename.c_str());
            send_join(sock, relay_addr, name);
        }

        fprintf(stderr, "[radio:%s] Stopped\n", name.c_str());
    }
};

// ── Main ──

int main(int argc, char* argv[]) {
    std::string dir, relay_host = "127.0.0.1", channel = "soluna", config_path;
    uint16_t relay_port = 5100;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--relay") && i + 1 < argc) {
            std::string r = argv[++i];
            auto colon = r.find(':');
            if (colon != std::string::npos) {
                relay_host = r.substr(0, colon);
                relay_port = (uint16_t)atoi(r.substr(colon + 1).c_str());
            } else {
                relay_host = r;
            }
        }
        else if (!strcmp(argv[i], "--channel") && i + 1 < argc) channel = argv[++i];
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) config_path = argv[++i];
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) ++i; // compat
    }

    // Build channel list from either --config or --dir/--channel
    std::vector<ChannelConfig> channel_configs;

    if (!config_path.empty()) {
        // Multi-channel mode
        channel_configs = parse_config(config_path);
        if (channel_configs.empty()) {
            fprintf(stderr, "[radio] No channels found in config: %s\n", config_path.c_str());
            return 1;
        }
        fprintf(stderr, "[radio] Loaded %zu channels from %s\n",
                channel_configs.size(), config_path.c_str());
    } else if (!dir.empty()) {
        // Single-channel mode (backward compatible)
        ChannelConfig cc;
        cc.name = channel;
        cc.dir = dir;
        channel_configs.push_back(cc);
    } else {
        fprintf(stderr,
            "Usage:\n"
            "  soluna-radio --dir DIR --relay HOST:PORT --channel NAME\n"
            "  soluna-radio --config CONFIG.json --relay HOST:PORT\n");
        return 1;
    }

    // Resolve relay address
    struct sockaddr_in relay_addr{};
    relay_addr.sin_family = AF_INET;
    relay_addr.sin_port = htons(relay_port);
    struct hostent* he = gethostbyname(relay_host.c_str());
    if (he) memcpy(&relay_addr.sin_addr, he->h_addr, he->h_length);
    else inet_pton(AF_INET, relay_host.c_str(), &relay_addr.sin_addr);

    // Single shared UDP socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    // Install signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize all channels
    std::vector<RadioChannel*> channels;
    for (const auto& cc : channel_configs) {
        auto* ch = new RadioChannel(cc.name, cc.dir, sock, relay_addr);
        if (ch->init()) {
            channels.push_back(ch);
        } else {
            fprintf(stderr, "[radio] Skipping channel '%s' (no files)\n", cc.name.c_str());
            delete ch;
        }
    }

    if (channels.empty()) {
        fprintf(stderr, "[radio] No channels with audio files. Exiting.\n");
        close(sock);
        return 1;
    }

    fprintf(stderr, "[radio] Starting %zu channel(s) on %s:%u\n",
            channels.size(), relay_host.c_str(), relay_port);

    // Launch one thread per channel
    std::vector<std::thread> threads;
    for (auto* ch : channels) {
        threads.emplace_back([ch]() { ch->run(); });
    }

    // Wait for all threads to finish (on SIGINT/SIGTERM, g_running becomes false)
    for (auto& t : threads) {
        t.join();
    }

    fprintf(stderr, "[radio] All channels stopped. Cleaning up.\n");
    for (auto* ch : channels) delete ch;
    close(sock);
    return 0;
}
