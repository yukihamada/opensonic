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
 *   --max-members N   Maximum members per group (default: 1000000)
 *   --stats-interval  Stats output interval in seconds (default: 30)
 *   --origin          Run as origin tier (accept downstream region relays)
 *   --region HOST:PORT  Run as region relay (connect upstream to origin)
 *   --edge HOST:PORT    Run as edge relay (connect upstream to region)
 *   --cascade-secret S  Shared auth secret between tiers
 *   --workers N        Number of worker threads for sendto() (default: 4)
 *   --help            Show this help
 *
 * UDP Protocol:
 *   JOIN:<group>:<password>\n   — Join a group (password optional)
 *   HELLO\n                     — Heartbeat (keep-alive every 5s)
 *   CHECK:<name>\n              — Check if channel name is available
 *   CLAIM:<name>:<device>:<txn>\n — Claim ownership of a channel name (annual)
 *   RELEASE:<name>:<device>\n   — Release ownership of a channel name
 *   GRANT:<role>:<device>\n     — Grant role to member (owner only, role=dj|listener)
 *   MEMBERS\n                   — List all members and their roles in current group
 *   META:<json>\n               — Broadcast metadata JSON (DJ/owner only on owned channels)
 *   FILE:<filename>\n           — File-sync: announce file (DJ/owner only)
 *   SYNC:<action>:<pos>:<ts>\n  — File-sync: play/pause/seek (DJ/owner only)
 *   REPLAY:<offset_sec>\n       — Request replay of last N seconds of audio
 *   RECORD:<group>\n            — Start server-side recording for a group
 *   RECORD_STOP:<group>\n       — Stop server-side recording for a group
 *   TEXT:<json>\n                — Text channel: lyrics/chat/info (lyric=DJ only)
 *   MIX:on|off\n                — Toggle DJ+mic simultaneous mode
 *   PING:<target_addr>:<ts>\n   — Latency probe (forwarded to target peer)
 *   ROUTE:<peer_addr>:<mode>\n  — Inform relay of preferred path (p2p|relay)
 *   CASCADE_JOIN:<group>:<peer_id>:<secret>\n  — downstream relay subscribes to a group
 *   CASCADE_HELLO:<peer_id>\n                  — downstream keepalive
 *   CASCADE_LEAVE:<group>:<peer_id>\n          — downstream unsubscribes
 *   CASCADE_STATS:<peer_id>:<json>\n           — downstream reports stats
 *   SWARM_READY\n                              — Client can relay audio to peers
 *   SWARM_UNABLE\n                             — Client can't relay (symmetric NAT)
 *   SWARM_ACK:<parent_ip>:<parent_port>\n      — Client confirms parent assignment
 *   SWARM_LOST:<parent_ip>:<parent_port>\n     — Client lost connection to parent
 *   FILE_OFFER:<name>:<size>:<sha256>:<chunks>\n — DJ offers file for P2P distribution
 *   FILE_URL:<url>:<sha256>:<size>\n           — DJ provides URL for HTTP download
 *   FILE_CHUNK:<sha256>:<idx>:<base64>\n       — DJ sends a chunk (relay ack only)
 *   FILE_HAVE:<sha256>:<bitfield_hex>\n        — Client reports owned chunks
 *   FILE_COMPLETE:<sha256>\n                   — Client has all chunks (full seeder)
 *   CHUNK_REQ:<sha256>:<chunk_index>\n         — Client asks for peers with chunk N
 *   MODE:<private|public>\n                  — Set channel mode (owner/DJ)
 *   MODE:<private|public>\n                  — Set channel mode (owner/DJ)
 *   COPYRIGHT_ACK\n                          — DJ acknowledges copyright detection
 *   COPYRIGHT_SKIP\n                         — DJ will skip to avoid charges
 *   LICENSED_PLAY:<json>\n                   — DJ declares licensed content (no detection)
 *   WALLET\n                                 — Query own wallet balance
 *   CHARGE:<amount>:<ts>:<hmac>\n            — Add funds (HMAC-SHA256 verified)
 *   WITHDRAW:<amount>[:<session>]\n          — Withdraw funds (requires payout account)
 *   SUPPORT:<amount>[:<session>]\n           — Listener funds DJ's royalty costs
 *   TIP:<amount>[:<session>]\n               — Listener tips DJ
 *   PAYOUT_SETUP:<method>:<id>\n             — Setup payout (stripe|paypal|bank)
 *   TIP:<amount>\n                           — Tip the DJ of current group
 *   TRANSACTIONS:<count>\n                   — Get last N transactions
 *   RIGHTS_BALANCE[:<holder>]\n              — Query rights holder balance(s)
 *   <RTP/OSTP packet>           — Audio data (DJ/owner only on owned channels)
 *
 * HTTP Endpoints:
 *   GET  /metrics                             — Prometheus metrics
 *   GET  /ws/audio?channel=<name>            — WebSocket audio stream (binary frames)
 *   GET  /api/channel/check?name=<name>      — Check channel availability
 *   GET  /api/admin/channels?key=<key>       — Admin: list all channels
 *
 * Cascade Architecture (planet-scale):
 *   DJ → Origin (1) → Region Relays (~20) → Edge Relays (~10K) → Listeners (3B)
 *   Each tier fans out to the next. Edges handle ~300K listeners via sendmmsg().
 *   If none of --origin/--region/--edge are set, relay works as standalone (unchanged).
 *
 * P2P Swarm Distribution (auto-activates at >50 listeners):
 *   Every listener re-broadcasts audio to 2-4 other listeners, forming a tree.
 *   Relay becomes coordinator (assigns tree positions, handles signaling).
 *   Audio flows peer-to-peer. Fan-out 4: 4^16 = 4.2B. Latency: 16*20ms = 320ms.
 *   DJ -> Relay (coordinator) -> Root nodes (depth 0) -> P2P tree -> Listeners
 *
 * P2P File Distribution (BitTorrent-style):
 *   DJ offers a file (FILE_OFFER or FILE_URL). Relay coordinates who has what.
 *   Chunks are 64KB. Relay assigns first-wave peers different chunk ranges.
 *   Peers trade chunks P2P (CHUNK_REQ/CHUNK_DATA), report progress (FILE_HAVE).
 *   Once listeners have the file, SYNC:play triggers local playback — zero bandwidth.
 *   FILE_URL is fastest: clients HTTP GET from CDN/cloud, no P2P overhead needed.
 *
 * Channel modes:
 *   - Public (default): relay forwarding, listed in discovery, copyright detection ON
 *   - Private: P2P preferred, not listed, copyright detection OFF
 *   - Owner/DJ can toggle via MODE:private or MODE:public
 *   - LAN multicast & P2P direct always bypass copyright (no relay involved)
 *
 * Channel ownership model:
 *   - Random channels (6 hex chars) → free, everyone can DJ
 *   - Built-in names (soluna, default, test) → free, everyone can DJ
 *   - Custom names → yearly purchase via CLAIM, owner controls DJ permissions
 *   - Roles: Owner (full control) > DJ (can broadcast) > Listener (receive only)
 *   - On JOIN: owner gets Owner role, others get Listener on owned channels
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
#include <deque>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <sys/stat.h>
#include <dirent.h>
#include <thread>
#include <random>
#include <atomic>
#include <queue>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <climits>
#include <cmath>

#include <sstream>
#include <iomanip>

#ifdef __linux__
// sendmmsg for batch UDP sending on Linux
#include <sys/socket.h>
#endif

// ── Embedded SHA-256 + HMAC-SHA256 (no OpenSSL dependency) ───────────────────
namespace {

static const uint32_t sha256_k[] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

// Returns raw 32-byte SHA-256 digest
std::string sha256_raw(const std::string& input) {
    uint32_t h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                       0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    size_t orig_len = input.size();
    size_t padded_len = ((orig_len + 9 + 63) / 64) * 64;
    std::vector<uint8_t> msg(padded_len, 0);
    memcpy(msg.data(), input.data(), orig_len);
    msg[orig_len] = 0x80;
    uint64_t bit_len = orig_len * 8;
    for (int i = 0; i < 8; i++) msg[padded_len - 1 - i] = (uint8_t)(bit_len >> (i * 8));

    for (size_t i = 0; i < padded_len; i += 64) {
        uint32_t w[64];
        for (int j = 0; j < 16; j++)
            w[j] = (msg[i+j*4]<<24)|(msg[i+j*4+1]<<16)|(msg[i+j*4+2]<<8)|msg[i+j*4+3];
        for (int j = 16; j < 64; j++) {
            uint32_t s0 = rotr(w[j-15],7)^rotr(w[j-15],18)^(w[j-15]>>3);
            uint32_t s1 = rotr(w[j-2],17)^rotr(w[j-2],19)^(w[j-2]>>10);
            w[j] = w[j-16]+s0+w[j-7]+s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int j = 0; j < 64; j++) {
            uint32_t S1=rotr(e,6)^rotr(e,11)^rotr(e,25);
            uint32_t ch=(e&f)^(~e&g);
            uint32_t t1=hh+S1+ch+sha256_k[j]+w[j];
            uint32_t S0=rotr(a,2)^rotr(a,13)^rotr(a,22);
            uint32_t mj=(a&b)^(a&c)^(b&c);
            uint32_t t2=S0+mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    std::string result(32, 0);
    for (int i = 0; i < 8; i++) {
        result[i*4]=(char)(h[i]>>24); result[i*4+1]=(char)(h[i]>>16);
        result[i*4+2]=(char)(h[i]>>8); result[i*4+3]=(char)h[i];
    }
    return result;
}

std::string sha256_hex(const std::string& input) {
    std::string raw = sha256_raw(input);
    std::ostringstream ss;
    for (unsigned char c : raw) ss << std::hex << std::setfill('0') << std::setw(2) << (int)(unsigned char)c;
    return ss.str();
}

// HMAC-SHA256: returns raw 32-byte digest
std::string hmac_sha256(const std::string& key, const std::string& message) {
    const size_t block_size = 64;
    std::string k = key;
    if (k.size() > block_size) k = sha256_raw(k);
    if (k.size() < block_size) k.resize(block_size, 0);
    std::string ipad(block_size, 0x36), opad(block_size, 0x5c);
    for (size_t i = 0; i < block_size; i++) { ipad[i] ^= k[i]; opad[i] ^= k[i]; }
    return sha256_raw(opad + sha256_raw(ipad + message));
}

std::string hmac_sha256_hex(const std::string& key, const std::string& message) {
    std::string raw = hmac_sha256(key, message);
    std::ostringstream ss;
    for (unsigned char c : raw) ss << std::hex << std::setfill('0') << std::setw(2) << (int)(unsigned char)c;
    return ss.str();
}

} // anonymous namespace

// ── Configuration ─────────────────────────────────────────────────────────────

static constexpr uint16_t kDefaultPort       = 5100;
static constexpr uint16_t kHttpPort          = 5102;
static constexpr size_t   kMaxPktSize        = 65536;
static constexpr int      kStaleTimeoutSec   = 15;   // member timeout
static constexpr int      kCleanupIntervalSec = 10;  // cleanup cycle
static constexpr size_t   kDefaultMaxGroups  = 100;
static constexpr size_t   kDefaultMaxMembers = 1000000;
static constexpr int64_t  kChannelExpiryDays = 365;
static const char*        kChannelDbPath     = "/data/channels.json";
// Loaded from env vars at startup (no hardcoded secrets)
static std::string g_webhook_path;   // RELAY_WEBHOOK_PATH env
static std::string g_admin_key;      // RELAY_ADMIN_KEY env
static std::string g_charge_secret;  // RELAY_CHARGE_SECRET env (HMAC key for CHARGE verification)

// Replay ring buffer sizing (overridable via --max-replay)
static constexpr size_t kDefaultMaxReplay    = 5000;  // ~50 seconds at 100 pps
static size_t kMaxReplayPackets              = kDefaultMaxReplay;

// ── Data structures ───────────────────────────────────────────────────────────

// Channel visibility mode: controls connection path and copyright behavior
enum class ChannelMode : uint8_t {
    Private = 0,   // P2P preferred, not listed in discovery, no copyright detection
    Public  = 1,   // relay forwarding, listed in discovery, copyright detection active
};

// Member roles: what a member is allowed to do in the channel
enum class MemberRole : uint8_t {
    Listener = 0,  // can only receive audio
    DJ       = 1,  // can send audio (DJ/mic) + FILE/META/SYNC commands
    Owner    = 2,  // full control: grant/revoke roles, manage channel
};

struct Member {
    sockaddr_in addr;
    std::chrono::steady_clock::time_point last_seen;
    std::string device_name;  // from JOIN message (optional)
    MemberRole role = MemberRole::Listener;
    bool mixing = false;  // DJ + mic simultaneous mode
    std::string session_token;  // 16-char hex, issued on JOIN, required for wallet ops
};

struct ReplayPacket {
    std::vector<uint8_t> data;
    std::chrono::steady_clock::time_point timestamp;
};

// ── P2P File manifest ─────────────────────────────────────────────────────────

struct FileManifest {
    std::string filename;
    std::string sha256;           // hex
    uint64_t size_bytes = 0;
    uint32_t num_chunks = 0;
    std::string url;              // optional HTTP URL for direct download
    std::chrono::steady_clock::time_point offered;

    // Track which members have which chunks
    // Key: member addr_key, Value: bitfield (1 bit per chunk)
    std::unordered_map<uint64_t, std::vector<bool>> peer_chunks;

    // Members who have the complete file
    std::unordered_set<uint64_t> complete_peers;
};

// ── Copyright detection & royalty tracking ──────────────────────────────────

static constexpr size_t kFingerprintIntervalSec = 5;    // extract fingerprint every 5s
static constexpr size_t kFingerprintSampleRate = 48000;
static constexpr size_t kFingerprintRingSize = kFingerprintSampleRate * 10;  // 10s ring buffer
static constexpr size_t kFingerprintSamplesNeeded = kFingerprintSampleRate * 5;  // 5s of audio

struct CopyrightMatch {
    std::string track_title;
    std::string artist;
    std::string album;
    std::string isrc;              // International Standard Recording Code
    std::string rights_holder;
    float confidence = 0.0f;       // 0.0-1.0
    std::chrono::steady_clock::time_point detected_at;
};

struct RoyaltyEntry {
    std::string group_name;         // channel where it was played
    std::string dj_device;          // who played it
    std::string isrc;               // track identifier
    std::string track_title;
    std::string artist;
    std::string rights_holder;
    uint64_t play_duration_sec;     // how long was it played
    uint32_t listener_count;        // how many heard it
    int64_t timestamp;              // unix timestamp
    double royalty_amount;          // calculated royalty (e.g., $0.003 per listener per play)
    bool settled = false;           // paid out to rights holder
};

// ── Wallet & billing ────────────────────────────────────────────────────────

struct Wallet {
    std::string device_id;          // unique device identifier
    double balance = 0.0;           // current balance in USD
    double total_charged = 0.0;     // lifetime charges
    double total_earned = 0.0;      // lifetime earnings (tips, cashback)
    double total_royalties_paid = 0.0;
    std::chrono::steady_clock::time_point last_activity;
};

struct PayoutAccount {
    std::string device_id;
    std::string payout_method;      // "stripe", "paypal", "bank"
    std::string payout_id;          // Stripe account ID, PayPal email, etc.
    double pending_payout = 0.0;    // accumulated but not yet withdrawn
    double total_withdrawn = 0.0;
};

struct Transaction {
    int64_t timestamp;
    std::string tx_id;              // unique transaction ID
    std::string from_device;        // who paid
    std::string to_device;          // who received (or "platform", "rights:<isrc>")
    double amount;
    std::string type;               // "charge", "royalty", "tip", "cashback", "withdraw"
    std::string description;
};

struct FingerprintBuffer {
    std::mutex mtx;
    std::vector<int16_t> ring;       // mono 16-bit samples (downmixed from stereo S24)
    size_t write_pos = 0;
    size_t total_written = 0;
    bool active = false;
    std::string group_name;

    FingerprintBuffer() : ring(kFingerprintRingSize, 0) {}
};

// Per-group fingerprint state
struct GroupCopyrightState {
    FingerprintBuffer fp_buf;
    CopyrightMatch current_match;
    bool is_copyrighted = false;
    std::chrono::steady_clock::time_point match_start;  // when current song was first detected
    uint32_t warnings_sent = 0;
    bool dj_acknowledged = false;   // DJ accepted the charge
};

struct Group {
    std::string name;
    std::string password;     // empty = no auth
    ChannelMode mode = ChannelMode::Public;  // default public; owner can change via MODE:
    std::vector<Member> members;
    std::chrono::steady_clock::time_point created;
    uint64_t packets_forwarded = 0;
    uint64_t bytes_forwarded   = 0;

    // META: last metadata JSON for new joiners
    std::string last_meta;

    // FILE sync: current file being played and last sync command
    std::string current_file;   // filename for file-sync mode
    std::string last_sync;      // last SYNC: command (play/pause/seek)

    // TEXT: last lyric text for new joiners
    std::string last_text;

    // REPLAY: ring buffer of recent audio packets
    std::vector<ReplayPacket> replay_buffer;
    size_t replay_write_pos = 0;

    // RECORD: server-side recording
    FILE* record_file = nullptr;
    std::string record_path;

    // P2P Swarm tree
    bool swarm_active = false;
    std::vector<struct SwarmNode> swarm_tree;   // parallel to members, same indices

    // P2P File distribution
    std::unordered_map<std::string, FileManifest> file_manifests;  // sha256 → manifest
    std::string current_file_sha;  // currently playing file's sha256

    // Copyright detection
    std::unique_ptr<GroupCopyrightState> copyright;  // lazily initialized on first audio
};

// ── P2P Swarm distribution ──────────────────────────────────────────────────
// Every listener also re-broadcasts audio to 2-4 other listeners, forming a
// tree/mesh that scales to billions with near-zero server bandwidth cost.
// Fan-out 4: 4^16 = 4.2 billion. Latency: 16 hops x 20ms = 320ms.

static constexpr size_t kSwarmFanOut = 4;        // each node relays to up to 4 children
static constexpr size_t kSwarmThreshold = 50;    // activate swarm when group > 50 members
[[maybe_unused]] static constexpr int kSwarmOrphanTimeoutMs = 2000;  // reconnect orphans within 2s

// ── P2P File distribution ──────────────────────────────────────────────────
static constexpr size_t kFileChunkSize = 65536;  // 64KB chunks
static constexpr size_t kFileFirstWavePeers = 4; // initial seed peers for chunk distribution

struct SwarmNode {
    sockaddr_in addr;
    std::string device_name;
    int depth = 0;                     // 0 = root (gets audio from DJ/relay)
    sockaddr_in parent_addr{};         // who sends audio to this node
    std::vector<sockaddr_in> children; // who this node sends audio to (max kSwarmFanOut)
    bool is_relay_capable = true;      // false if NAT/firewall prevents forwarding
    uint64_t bytes_relayed = 0;
    std::chrono::steady_clock::time_point joined;
};

// ── Channel ownership ─────────────────────────────────────────────────────────

struct ChannelRecord {
    std::string device;
    std::string txn;
    int64_t expires;  // unix timestamp
    std::string stripe_customer;  // Stripe customer ID (web purchases)
    std::string stripe_sub;       // Stripe subscription ID
};

// Built-in free names — joinable by anyone, everyone gets DJ role (like random channels)
static const std::vector<std::string> kFreeNames = {
    "soluna", "default", "test", "admin", "system", "relay", "server",
    "opensonic", "api", "help", "support", "status", "debug",
    "music", "audio", "home", "live", "studio", "party", "zen",
    "bass", "beat", "jazz", "rock", "pop", "mix", "dj",
    "room", "cafe", "bar", "club", "lounge", "lobby",
    "office", "work", "lab", "gym", "spa", "pool",
    "kitchen", "bedroom", "garden", "garage", "patio",
    "upstairs", "downstairs", "main", "master", "guest",
    "sakura", "fuji", "tokyo", "kyoto", "osaka", "nara",
};

// Reserved names — cannot be CLAIMED (purchased) by anyone, but joinable as free channels
static const std::vector<std::string> kReservedNames = {
    // Brand / system
    "soluna", "default", "test", "admin", "system", "relay", "server",
    "opensonic", "api", "help", "support", "status", "debug",
    // Premium tier — short, high-value names (free to use, cannot be purchased)
    "music", "audio", "home", "live", "studio", "party", "zen",
    "bass", "beat", "jazz", "rock", "pop", "mix", "dj",
    "room", "cafe", "bar", "club", "lounge", "lobby",
    "office", "work", "lab", "gym", "spa", "pool",
    "kitchen", "bedroom", "garden", "garage", "patio",
    // Single-word rooms
    "upstairs", "downstairs", "main", "master", "guest",
    // Japanese-popular
    "sakura", "fuji", "tokyo", "kyoto", "osaka", "nara",
};

static std::map<std::string, ChannelRecord> g_channels;  // guarded by g_mutex

// Forward-declared globals (needed by cascade code before full Global state section)
static int                  g_udp_sock = -1;

// ── Cascade relay tree ──────────────────────────────────────────────────────

enum class RelayTier : uint8_t {
    Standalone = 0,  // default: single relay (unchanged behavior)
    Origin     = 1,  // top tier: DJ connects here
    Region     = 2,  // mid tier: fans to edges
    Edge       = 3,  // leaf tier: fans to listeners
};

struct DownstreamPeer {
    sockaddr_in addr;
    std::string peer_id;           // unique ID of this downstream relay
    std::chrono::steady_clock::time_point last_seen;
    uint64_t listener_count = 0;   // reported by downstream
    std::unordered_set<std::string> groups;  // which groups this peer subscribes to
};

// State for cascade mode
static RelayTier g_tier = RelayTier::Standalone;
static std::string g_cascade_secret;
static std::string g_my_peer_id;  // random UUID generated at startup

// Upstream connection (for region/edge modes)
static sockaddr_in g_upstream_addr{};
static bool g_upstream_connected = false;

// Downstream peers (for origin/region modes)
static std::mutex g_downstream_mutex;
static std::vector<DownstreamPeer> g_downstream_peers;

// Reverse lookup: addr → group name (for quick lookup in forward_audio)
static std::unordered_map<uint64_t, std::string> g_addr_to_group;
// key = (uint64_t(ip) << 16) | port

static uint64_t addr_key(const sockaddr_in& a) {
    return ((uint64_t)a.sin_addr.s_addr << 16) | (uint64_t)ntohs(a.sin_port);
}

// Worker thread pool for batch sending
struct ForwardTask {
    std::vector<uint8_t> data;
    std::vector<sockaddr_in> destinations;
};

static size_t g_num_workers = 4;
static std::mutex g_work_mutex;
static std::condition_variable g_work_cv;
static std::queue<ForwardTask> g_work_queue;
static bool g_workers_running = true;
static std::vector<std::thread> g_worker_threads;
static std::atomic<uint64_t> g_worker_tasks_done{0};

// Generate random hex peer ID
static std::string generate_hex(int length) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    const char hex[] = "0123456789abcdef";
    std::string id;
    id.reserve(length);
    for (int i = 0; i < length; i++) id += hex[dis(gen)];
    return id;
}

static std::string generate_peer_id() { return generate_hex(32); }

// validate_session_token: defined later (after g_groups declaration)
static bool validate_session_token(const sockaddr_in& from, const std::string& token);

// ── batch_sendto — use sendmmsg on Linux, fallback loop elsewhere ────────

#ifdef __linux__
static void batch_sendto(int sock, const uint8_t* data, size_t len,
                         const std::vector<sockaddr_in>& dests) {
    if (dests.empty()) return;
    constexpr size_t kChunkSize = 1024;
    // iovec pointing to the same data for every message
    struct iovec iov;
    iov.iov_base = const_cast<uint8_t*>(data);
    iov.iov_len = len;

    std::vector<struct mmsghdr> msgs(std::min(dests.size(), kChunkSize));
    for (size_t offset = 0; offset < dests.size(); offset += kChunkSize) {
        size_t count = std::min(kChunkSize, dests.size() - offset);
        for (size_t i = 0; i < count; i++) {
            memset(&msgs[i], 0, sizeof(struct mmsghdr));
            msgs[i].msg_hdr.msg_name = const_cast<sockaddr_in*>(&dests[offset + i]);
            msgs[i].msg_hdr.msg_namelen = sizeof(sockaddr_in);
            msgs[i].msg_hdr.msg_iov = &iov;
            msgs[i].msg_hdr.msg_iovlen = 1;
        }
        sendmmsg(sock, msgs.data(), (unsigned int)count, 0);
    }
}
#else
static void batch_sendto(int sock, const uint8_t* data, size_t len,
                         const std::vector<sockaddr_in>& dests) {
    for (const auto& dest : dests) {
        sendto(sock, data, len, 0, (const sockaddr*)&dest, sizeof(dest));
    }
}
#endif

// Worker thread function
static void worker_thread_func() {
    while (true) {
        ForwardTask task;
        {
            std::unique_lock<std::mutex> lock(g_work_mutex);
            g_work_cv.wait(lock, [] { return !g_work_queue.empty() || !g_workers_running; });
            if (!g_workers_running && g_work_queue.empty()) return;
            task = std::move(g_work_queue.front());
            g_work_queue.pop();
        }
        batch_sendto(g_udp_sock, task.data.data(), task.data.size(), task.destinations);
        g_worker_tasks_done.fetch_add(1, std::memory_order_relaxed);
    }
}

static void enqueue_forward(const uint8_t* data, size_t len,
                             std::vector<sockaddr_in>&& dests) {
    if (dests.empty()) return;
    // For small destination lists, send inline to avoid queue overhead
    if (dests.size() <= 4) {
        batch_sendto(g_udp_sock, data, len, dests);
        return;
    }
    ForwardTask task;
    task.data.assign(data, data + len);
    task.destinations = std::move(dests);
    {
        std::lock_guard<std::mutex> lock(g_work_mutex);
        g_work_queue.push(std::move(task));
    }
    g_work_cv.notify_one();
}

static void start_workers() {
    for (size_t i = 0; i < g_num_workers; i++) {
        g_worker_threads.emplace_back(worker_thread_func);
    }
}

static void stop_workers() {
    {
        std::lock_guard<std::mutex> lock(g_work_mutex);
        g_workers_running = false;
    }
    g_work_cv.notify_all();
    for (auto& t : g_worker_threads) {
        if (t.joinable()) t.join();
    }
}

// Simple JSON helpers (no external libs) ──────────────────────────────────────

static std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            default:   out += c;
        }
    }
    return out;
}

// Minimal JSON string value extractor: find "key":"value" after pos
static std::string json_get_string(const std::string& json, const std::string& key, size_t start = 0) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle, start);
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + needle.size());  // skip to colon area
    if (pos == std::string::npos) return "";
    pos++;  // opening quote of value
    std::string val;
    for (size_t i = pos; i < json.size(); i++) {
        if (json[i] == '\\' && i + 1 < json.size()) { val += json[++i]; continue; }
        if (json[i] == '"') break;
        val += json[i];
    }
    return val;
}

// Minimal JSON integer extractor: find "key":12345
static int64_t json_get_int(const std::string& json, const std::string& key, size_t start = 0) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle, start);
    if (pos == std::string::npos) return 0;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return 0;
    pos++;
    while (pos < json.size() && json[pos] == ' ') pos++;
    return std::atoll(json.c_str() + pos);
}

// Minimal JSON double extractor: find "key":12.34
static double json_get_double(const std::string& json, const std::string& key, size_t start = 0) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle, start);
    if (pos == std::string::npos) return 0.0;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return 0.0;
    pos++;
    while (pos < json.size() && json[pos] == ' ') pos++;
    return std::atof(json.c_str() + pos);
}

static void channels_load() {
    FILE* f = fopen(kChannelDbPath, "r");
    if (!f) {
        fprintf(stderr, "[relay] No channel DB at %s — starting fresh\n", kChannelDbPath);
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string data(sz, '\0');
    size_t rd = fread(&data[0], 1, sz, f);
    fclose(f);
    data.resize(rd);

    // Parse: find each channel object block inside "channels":{...}
    size_t channels_pos = data.find("\"channels\"");
    if (channels_pos == std::string::npos) return;

    // Walk through "name":{...} pairs
    size_t pos = data.find('{', channels_pos + 10);  // opening { of channels object
    if (pos == std::string::npos) return;
    pos++;

    while (pos < data.size()) {
        // Find next key (channel name)
        size_t q1 = data.find('"', pos);
        if (q1 == std::string::npos) break;
        size_t q2 = data.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string name = data.substr(q1 + 1, q2 - q1 - 1);

        // Find the object for this channel
        size_t obj_start = data.find('{', q2);
        if (obj_start == std::string::npos) break;
        size_t obj_end = data.find('}', obj_start);
        if (obj_end == std::string::npos) break;

        std::string obj = data.substr(obj_start, obj_end - obj_start + 1);
        ChannelRecord rec;
        rec.device          = json_get_string(obj, "device");
        rec.txn             = json_get_string(obj, "txn");
        rec.expires         = json_get_int(obj, "expires");
        rec.stripe_customer = json_get_string(obj, "stripe_customer");
        rec.stripe_sub      = json_get_string(obj, "stripe_sub");

        if (!name.empty() && !rec.device.empty()) {
            g_channels[name] = rec;
        }
        pos = obj_end + 1;
        // Skip comma
        while (pos < data.size() && (data[pos] == ',' || data[pos] == ' ' || data[pos] == '\n')) pos++;
        if (pos < data.size() && data[pos] == '}') break;  // end of channels object
    }

    fprintf(stderr, "[relay] Loaded %zu channels from %s\n", g_channels.size(), kChannelDbPath);
}

static void channels_save() {
    FILE* f = fopen(kChannelDbPath, "w");
    if (!f) {
        fprintf(stderr, "[relay] WARNING: cannot write %s: %s\n", kChannelDbPath, strerror(errno));
        return;
    }
    fprintf(f, "{\"channels\":{");
    bool first = true;
    for (const auto& [name, rec] : g_channels) {
        if (!first) fprintf(f, ",");
        first = false;
        fprintf(f, "\n  \"%s\":{\"device\":\"%s\",\"txn\":\"%s\",\"expires\":%lld,\"stripe_customer\":\"%s\",\"stripe_sub\":\"%s\"}",
                json_escape(name).c_str(),
                json_escape(rec.device).c_str(),
                json_escape(rec.txn).c_str(),
                (long long)rec.expires,
                json_escape(rec.stripe_customer).c_str(),
                json_escape(rec.stripe_sub).c_str());
    }
    fprintf(f, "\n}}\n");
    fclose(f);
}

// ── Channel name validation ──────────────────────────────────────────────────

static bool is_hex_string(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

static bool is_random_channel(const std::string& name) {
    // Random channels are exactly 6 hex chars like "a3f9b2"
    return name.size() == 6 && is_hex_string(name);
}

static bool is_free_name(const std::string& name) {
    for (const auto& n : kFreeNames) {
        if (name == n) return true;
    }
    return false;
}

static bool is_reserved_name(const std::string& name) {
    // Convert to lowercase for comparison
    std::string lower = name;
    for (auto& c : lower) c = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    for (const auto& n : kReservedNames) {
        if (lower == n) return true;
    }
    return false;
}

static bool is_valid_channel_name(const std::string& name) {
    if (name.size() < 3 || name.size() > 20) return false;
    for (char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-'))
            return false;
    }
    // Reject all-hex names (reserved for random generation)
    if (is_hex_string(name)) return false;
    return true;
}

static int64_t now_unix() {
    return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// ── Global state ──────────────────────────────────────────────────────────────

static std::shared_mutex    g_mutex;  // shared_lock for reads, unique_lock for writes
static std::map<std::string, Group> g_groups;
// g_udp_sock declared earlier (before cascade section)
static std::atomic<bool>    g_running{true};

// Limits
static size_t g_max_groups  = kDefaultMaxGroups;
static size_t g_max_members = kDefaultMaxMembers;

// Stats counters
static uint64_t g_total_packets_rx  = 0;
static uint64_t g_total_packets_fwd = 0;
static uint64_t g_total_bytes_rx    = 0;
static uint64_t g_total_bytes_fwd   = 0;
static uint64_t g_total_joins       = 0;

// P2P Swarm stats
static std::atomic<uint64_t> g_swarm_bytes_saved{0};   // bandwidth saved by P2P forwarding
static std::atomic<uint64_t> g_swarm_packets_saved{0};  // packets not sent by relay

// P2P File distribution stats
static std::atomic<uint64_t> g_file_offers{0};          // total FILE_OFFER commands
static std::atomic<uint64_t> g_file_completions{0};     // total FILE_COMPLETE from clients
static std::atomic<uint64_t> g_file_url_offers{0};      // total FILE_URL commands

// Copyright detection & royalty tracking
static std::mutex g_royalty_mutex;
static std::vector<RoyaltyEntry> g_royalty_ledger;
static std::string g_royalty_log_path = "/data/royalties.jsonl";  // JSONL append log
static std::atomic<uint64_t> g_copyright_detections{0};
static std::atomic<uint64_t> g_total_royalty_cents{0};  // running total in cents
static bool g_copyright_enabled = true;   // --no-copyright to disable
static std::string g_fingerprint_api_url;  // external fingerprint API endpoint
static std::string g_fingerprint_api_key;  // API key for fingerprint service

// ── Wallet & billing state ────────────────────────────────────────────────────
static std::mutex g_wallet_mutex;
static std::unordered_map<std::string, Wallet> g_wallets;           // device_id → wallet
static std::unordered_map<std::string, PayoutAccount> g_payouts;    // device_id → payout info
static constexpr size_t kMaxTransactions = 100000;                     // cap in-memory log
static std::deque<Transaction> g_transactions;                         // in-memory transaction log (capped)
static std::string g_transactions_log_path = "/data/transactions.jsonl";
static std::string g_wallets_db_path = "/data/wallets.json";

// Rights holder balances (by ISRC prefix or rights_holder name)
static std::unordered_map<std::string, double> g_rights_holder_balances;

// Billing stats (atomics for lock-free reads)
static std::atomic<uint64_t> g_total_tips_cents{0};
static std::atomic<uint64_t> g_total_charges_cents{0};
static std::atomic<uint64_t> g_total_withdrawals_cents{0};

// Grace period tracking for insufficient balance DJs
// device_id → timestamp when grace period started
static std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_grace_periods;
static constexpr int kGracePeriodSec = 300;  // 5 minutes

// ── Royalty rate calculation ─────────────────────────────────────────────────
// Tiered pricing: per-listener rate decreases with audience size.
// Base rate aligned with SoundExchange/Spotify industry standards.
//
// | Listeners |  Rate/person/min | Equivalent per-stream |
// |-----------|------------------|-----------------------|
// |  1-100    | $0.001           | ~$0.004 (Spotify-level) |
// |  101-1K   | $0.0005          | ~$0.002               |
// |  1K-10K   | $0.0002          | ~$0.0008              |
// |  10K-100K | $0.0001          | ~$0.0004              |
// |  100K+    | $0.00005         | ~$0.0002              |
//
// Examples:
//   50 listeners × 4 hours = $12.00  (¥1,800)   — bar DJ, close to JASRAC monthly
//   1,000 × 2 hours = $60.00 (¥9,000)            — online event
//   100K × 1 hour = $300.00 (¥45,000)             — festival stream
//   1M × 1 hour = $1,500.00 (¥225,000)            — major live event

static double calculate_royalty_per_min(size_t listener_count) {
    if (listener_count == 0) return 0.0;

    // Tiered calculation: each tier applies to its range only
    double total = 0.0;
    size_t remaining = listener_count;

    // Tier 1: first 100 listeners at $0.001/person/min
    size_t t1 = std::min(remaining, (size_t)100);
    total += t1 * 0.001;
    remaining -= t1;

    // Tier 2: 101-1,000 at $0.0005/person/min
    size_t t2 = std::min(remaining, (size_t)900);
    total += t2 * 0.0005;
    remaining -= t2;

    // Tier 3: 1,001-10,000 at $0.0002/person/min
    size_t t3 = std::min(remaining, (size_t)9000);
    total += t3 * 0.0002;
    remaining -= t3;

    // Tier 4: 10,001-100,000 at $0.0001/person/min
    size_t t4 = std::min(remaining, (size_t)90000);
    total += t4 * 0.0001;
    remaining -= t4;

    // Tier 5: 100,001+ at $0.00005/person/min
    total += remaining * 0.00005;

    return total;
}

// Fingerprint thread
static std::thread g_fingerprint_thread;
static std::atomic<bool> g_fingerprint_running{true};

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

// Validate session token: find member by address, check token matches.
// Caller must hold g_mutex.
static bool validate_session_token(const sockaddr_in& from, const std::string& token) {
    if (token.empty()) return false;
    for (auto& [name, group] : g_groups) {
        for (auto& m : group.members) {
            if (addr_equal(m.addr, from)) {
                return m.session_token == token;
            }
        }
    }
    return false;
}

// ── Wallet persistence ──────────────────────────────────────────────────────

static void wallets_load() {
    FILE* f = fopen(g_wallets_db_path.c_str(), "r");
    if (!f) {
        fprintf(stderr, "[wallet] No wallet DB at %s — starting fresh\n", g_wallets_db_path.c_str());
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string data(sz, '\0');
    size_t rd = fread(&data[0], 1, sz, f);
    fclose(f);
    data.resize(rd);

    // Parse wallets: {"wallets":{...}, "payouts":{...}, "rights_holder_balances":{...}}
    size_t wallets_pos = data.find("\"wallets\"");
    if (wallets_pos != std::string::npos) {
        size_t pos = data.find('{', wallets_pos + 9);
        if (pos != std::string::npos) {
            pos++;
            while (pos < data.size()) {
                size_t q1 = data.find('"', pos);
                if (q1 == std::string::npos) break;
                size_t q2 = data.find('"', q1 + 1);
                if (q2 == std::string::npos) break;
                std::string device_id = data.substr(q1 + 1, q2 - q1 - 1);

                size_t obj_start = data.find('{', q2);
                if (obj_start == std::string::npos) break;
                size_t obj_end = data.find('}', obj_start);
                if (obj_end == std::string::npos) break;
                std::string obj = data.substr(obj_start, obj_end - obj_start + 1);

                Wallet w;
                w.device_id = device_id;
                w.balance = json_get_double(obj, "balance");
                w.total_charged = json_get_double(obj, "total_charged");
                w.total_earned = json_get_double(obj, "total_earned");
                w.total_royalties_paid = json_get_double(obj, "total_royalties_paid");
                w.last_activity = std::chrono::steady_clock::now();

                if (!device_id.empty()) {
                    g_wallets[device_id] = w;
                }
                pos = obj_end + 1;
                while (pos < data.size() && (data[pos] == ',' || data[pos] == ' ' || data[pos] == '\n')) pos++;
                if (pos < data.size() && data[pos] == '}') break;
            }
        }
    }

    // Parse payout accounts
    size_t payouts_pos = data.find("\"payouts\"");
    if (payouts_pos != std::string::npos) {
        size_t pos = data.find('{', payouts_pos + 9);
        if (pos != std::string::npos) {
            pos++;
            while (pos < data.size()) {
                size_t q1 = data.find('"', pos);
                if (q1 == std::string::npos) break;
                size_t q2 = data.find('"', q1 + 1);
                if (q2 == std::string::npos) break;
                std::string device_id = data.substr(q1 + 1, q2 - q1 - 1);

                size_t obj_start = data.find('{', q2);
                if (obj_start == std::string::npos) break;
                size_t obj_end = data.find('}', obj_start);
                if (obj_end == std::string::npos) break;
                std::string obj = data.substr(obj_start, obj_end - obj_start + 1);

                PayoutAccount pa;
                pa.device_id = device_id;
                pa.payout_method = json_get_string(obj, "payout_method");
                pa.payout_id = json_get_string(obj, "payout_id");
                pa.pending_payout = json_get_double(obj, "pending_payout");
                pa.total_withdrawn = json_get_double(obj, "total_withdrawn");

                if (!device_id.empty()) {
                    g_payouts[device_id] = pa;
                }
                pos = obj_end + 1;
                while (pos < data.size() && (data[pos] == ',' || data[pos] == ' ' || data[pos] == '\n')) pos++;
                if (pos < data.size() && data[pos] == '}') break;
            }
        }
    }

    // Parse rights holder balances
    size_t rh_pos = data.find("\"rights_holder_balances\"");
    if (rh_pos != std::string::npos) {
        size_t pos = data.find('{', rh_pos + 23);
        if (pos != std::string::npos) {
            pos++;
            while (pos < data.size()) {
                size_t q1 = data.find('"', pos);
                if (q1 == std::string::npos) break;
                size_t q2 = data.find('"', q1 + 1);
                if (q2 == std::string::npos) break;
                std::string holder = data.substr(q1 + 1, q2 - q1 - 1);

                size_t colon = data.find(':', q2);
                if (colon == std::string::npos) break;
                size_t val_start = colon + 1;
                while (val_start < data.size() && data[val_start] == ' ') val_start++;
                double bal = std::atof(data.c_str() + val_start);

                if (!holder.empty()) {
                    g_rights_holder_balances[holder] = bal;
                }

                // Skip to next entry or end
                size_t next_comma = data.find(',', val_start);
                size_t next_brace = data.find('}', val_start);
                if (next_brace != std::string::npos && (next_comma == std::string::npos || next_brace < next_comma)) break;
                pos = (next_comma != std::string::npos) ? next_comma + 1 : data.size();
            }
        }
    }

    fprintf(stderr, "[wallet] Loaded %zu wallets, %zu payout accounts, %zu rights holders from %s\n",
            g_wallets.size(), g_payouts.size(), g_rights_holder_balances.size(),
            g_wallets_db_path.c_str());
}

// ── Seed famous DJ wallets (first-run only) ─────────────────────────────────
// Pre-creates accounts for well-known DJs with up to ¥100,000 (~$670) balance.
// Only seeds accounts that don't already exist (won't overwrite on restart).

static void wallets_seed() {
    struct DJSeed {
        const char* device_id;
        double balance;     // USD
        const char* note;   // for logging
    };

    // Demo seed wallets — generic test accounts (no real names)
    // ¥100,000 ≈ $670 USD
    static const DJSeed seeds[] = {
        // ── Platform ──
        {"DJ-Admin",         670.00, "Platform admin account"},

        // ── Genre demo channels ──
        {"DJ-House",         100.00, "House music demo"},
        {"DJ-Techno",        100.00, "Techno demo"},
        {"DJ-HipHop",        100.00, "Hip-hop demo"},
        {"DJ-Jazz",          100.00, "Jazz demo"},
        {"DJ-Classical",     100.00, "Classical demo"},
        {"DJ-Ambient",       100.00, "Ambient demo"},
        {"DJ-Bass",          100.00, "Bass music demo"},
        {"DJ-Trance",        100.00, "Trance demo"},
        {"DJ-DnB",           100.00, "Drum & Bass demo"},
        {"DJ-Lofi",          100.00, "Lo-fi demo"},

        // ── Test accounts ──
        {"DJ-Test-01",        50.00, "Test account 01"},
        {"DJ-Test-02",        50.00, "Test account 02"},
        {"DJ-Test-03",        50.00, "Test account 03"},
        {"DJ-Test-04",        50.00, "Test account 04"},
        {"DJ-Test-05",        50.00, "Test account 05"},
    };

    std::lock_guard<std::mutex> lock(g_wallet_mutex);
    int seeded = 0;

    for (const auto& seed : seeds) {
        if (g_wallets.find(seed.device_id) != g_wallets.end()) continue;  // already exists

        Wallet w;
        w.device_id = seed.device_id;
        w.balance = seed.balance;
        w.total_charged = seed.balance;  // recorded as initial charge
        w.last_activity = std::chrono::steady_clock::now();
        g_wallets[seed.device_id] = w;
        seeded++;
    }

    if (seeded > 0) {
        fprintf(stderr, "[wallet] Seeded %d demo DJ wallets\n", seeded);
    }
}

static void wallets_save() {
    std::lock_guard<std::mutex> lock(g_wallet_mutex);
    FILE* f = fopen(g_wallets_db_path.c_str(), "w");
    if (!f) {
        fprintf(stderr, "[wallet] WARNING: cannot write %s: %s\n", g_wallets_db_path.c_str(), strerror(errno));
        return;
    }

    // Wallets
    fprintf(f, "{\"wallets\":{");
    bool first = true;
    for (const auto& [id, w] : g_wallets) {
        if (!first) fprintf(f, ",");
        first = false;
        fprintf(f, "\n  \"%s\":{\"balance\":%.4f,\"total_charged\":%.4f,\"total_earned\":%.4f,\"total_royalties_paid\":%.4f}",
                json_escape(id).c_str(), w.balance, w.total_charged, w.total_earned, w.total_royalties_paid);
    }
    fprintf(f, "\n},\"payouts\":{");

    // Payout accounts
    first = true;
    for (const auto& [id, pa] : g_payouts) {
        if (!first) fprintf(f, ",");
        first = false;
        fprintf(f, "\n  \"%s\":{\"payout_method\":\"%s\",\"payout_id\":\"%s\",\"pending_payout\":%.4f,\"total_withdrawn\":%.4f}",
                json_escape(id).c_str(),
                json_escape(pa.payout_method).c_str(),
                json_escape(pa.payout_id).c_str(),
                pa.pending_payout, pa.total_withdrawn);
    }
    fprintf(f, "\n},\"rights_holder_balances\":{");

    // Rights holder balances
    first = true;
    for (const auto& [holder, bal] : g_rights_holder_balances) {
        if (!first) fprintf(f, ",");
        first = false;
        fprintf(f, "\n  \"%s\":%.4f", json_escape(holder).c_str(), bal);
    }
    fprintf(f, "\n}}\n");
    fclose(f);
}

// Generate unique transaction ID
static std::string generate_tx_id() {
    static std::atomic<uint64_t> counter{0};
    char buf[64];
    snprintf(buf, sizeof(buf), "tx_%lld_%llu",
             (long long)now_unix(), (unsigned long long)counter.fetch_add(1, std::memory_order_relaxed));
    return buf;
}

// Log a transaction to in-memory list and append to JSONL file
// Caller must hold g_wallet_mutex.
static void log_transaction(const std::string& from_device, const std::string& to_device,
                             double amount, const std::string& type,
                             const std::string& description) {
    Transaction tx;
    tx.timestamp = now_unix();
    tx.tx_id = generate_tx_id();
    tx.from_device = from_device;
    tx.to_device = to_device;
    tx.amount = amount;
    tx.type = type;
    tx.description = description;
    g_transactions.push_back(tx);
    while (g_transactions.size() > kMaxTransactions) {
        g_transactions.pop_front();  // evict oldest
    }

    // Append to JSONL log (full history preserved on disk)
    FILE* f = fopen(g_transactions_log_path.c_str(), "a");
    if (f) {
        fprintf(f, "{\"ts\":%lld,\"tx_id\":\"%s\",\"from\":\"%s\",\"to\":\"%s\","
                   "\"amount\":%.4f,\"type\":\"%s\",\"desc\":\"%s\"}\n",
                (long long)tx.timestamp,
                json_escape(tx.tx_id).c_str(),
                json_escape(tx.from_device).c_str(),
                json_escape(tx.to_device).c_str(),
                tx.amount,
                json_escape(tx.type).c_str(),
                json_escape(tx.description).c_str());
        fclose(f);
    }
}

// ── P2P Swarm tree management ─────────────────────────────────────────────────

// Build a SWARM_ASSIGN message for a given swarm node.
// Format: SWARM_ASSIGN:<parent_ip>:<parent_port>[:<child_ip>:<child_port>]*\n
static std::string build_swarm_assign(const SwarmNode& node) {
    char parent_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &node.parent_addr.sin_addr, parent_ip, sizeof(parent_ip));
    std::string msg = "SWARM_ASSIGN:" + std::string(parent_ip) + ":"
                      + std::to_string(ntohs(node.parent_addr.sin_port));
    for (const auto& child : node.children) {
        char child_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &child.sin_addr, child_ip, sizeof(child_ip));
        msg += ":" + std::string(child_ip) + ":" + std::to_string(ntohs(child.sin_port));
    }
    msg += "\n";
    return msg;
}

// Send a SWARM_ASSIGN to a specific member.
static void send_swarm_assign(const SwarmNode& node) {
    std::string msg = build_swarm_assign(node);
    sendto(g_udp_sock, msg.c_str(), msg.size(), 0,
           (const sockaddr*)&node.addr, sizeof(node.addr));
}

// Full rebuild of the swarm tree — called on initial activation or catastrophic failure.
// Caller must hold g_mutex.
static void rebuild_swarm_tree(Group& group) {
    // Only activate if group has > kSwarmThreshold members
    if (group.members.size() <= kSwarmThreshold) {
        if (group.swarm_active) {
            // Deactivate swarm, go back to relay forwarding
            group.swarm_active = false;
            group.swarm_tree.clear();
            // Notify all members: parent = 0.0.0.0:0 means relay mode
            for (const auto& m : group.members) {
                SwarmNode dummy;
                dummy.addr = m.addr;
                memset(&dummy.parent_addr, 0, sizeof(dummy.parent_addr));
                send_swarm_assign(dummy);
            }
            fprintf(stderr, "[swarm] Deactivated swarm for group '%s' (%zu members < %zu threshold)\n",
                    group.name.c_str(), group.members.size(), kSwarmThreshold);
        }
        return;
    }

    group.swarm_active = true;
    group.swarm_tree.clear();
    group.swarm_tree.resize(group.members.size());

    // BFS queue of node indices with room for children
    std::queue<size_t> available;

    // Find DJ/Owner as root
    size_t root_idx = 0;
    for (size_t i = 0; i < group.members.size(); i++) {
        if (group.members[i].role >= MemberRole::DJ) {
            root_idx = i;
            break;
        }
    }

    // Root node: depth 0, receives from relay directly (parent = 0.0.0.0:0)
    auto now = std::chrono::steady_clock::now();
    group.swarm_tree[root_idx].addr = group.members[root_idx].addr;
    group.swarm_tree[root_idx].device_name = group.members[root_idx].device_name;
    group.swarm_tree[root_idx].depth = 0;
    group.swarm_tree[root_idx].is_relay_capable = true;
    group.swarm_tree[root_idx].joined = now;
    memset(&group.swarm_tree[root_idx].parent_addr, 0, sizeof(sockaddr_in));
    available.push(root_idx);

    // Assign all other members as children in BFS order
    for (size_t i = 0; i < group.members.size(); i++) {
        if (i == root_idx) continue;

        group.swarm_tree[i].addr = group.members[i].addr;
        group.swarm_tree[i].device_name = group.members[i].device_name;
        group.swarm_tree[i].joined = now;

        // Find a parent with room
        while (!available.empty()) {
            size_t parent_idx = available.front();
            if (group.swarm_tree[parent_idx].children.size() < kSwarmFanOut) {
                // Assign as child
                group.swarm_tree[i].parent_addr = group.swarm_tree[parent_idx].addr;
                group.swarm_tree[i].depth = group.swarm_tree[parent_idx].depth + 1;
                group.swarm_tree[parent_idx].children.push_back(group.members[i].addr);

                // This new node can also accept children (if relay-capable)
                if (group.swarm_tree[i].is_relay_capable) {
                    available.push(i);
                }
                break;
            } else {
                available.pop();  // this parent is full
            }
        }
    }

    // Send SWARM_ASSIGN to every member
    for (size_t i = 0; i < group.swarm_tree.size(); i++) {
        send_swarm_assign(group.swarm_tree[i]);
    }

    // Compute max depth for logging
    int max_depth = 0;
    for (const auto& sn : group.swarm_tree) {
        if (sn.depth > max_depth) max_depth = sn.depth;
    }
    fprintf(stderr, "[swarm] Built tree for group '%s': %zu nodes, max depth %d, fan-out %zu\n",
            group.name.c_str(), group.swarm_tree.size(), max_depth, kSwarmFanOut);
}

// Incrementally add a new member to the swarm tree — O(1).
// Caller must hold g_mutex.
static void swarm_add_member(Group& group, size_t member_idx) {
    if (!group.swarm_active) {
        // Check if we should activate swarm
        if (group.members.size() > kSwarmThreshold) {
            rebuild_swarm_tree(group);
        }
        return;
    }

    // Grow swarm_tree to match members
    while (group.swarm_tree.size() <= member_idx) {
        group.swarm_tree.emplace_back();
    }

    auto& node = group.swarm_tree[member_idx];
    node.addr = group.members[member_idx].addr;
    node.device_name = group.members[member_idx].device_name;
    node.joined = std::chrono::steady_clock::now();
    node.is_relay_capable = true;  // default; updated by SWARM_UNABLE

    // Find the shallowest node with room for more children
    int best_depth = INT_MAX;
    size_t best_parent = SIZE_MAX;
    for (size_t i = 0; i < group.swarm_tree.size(); i++) {
        if (i == member_idx) continue;
        if (!group.swarm_tree[i].is_relay_capable) continue;
        if (group.swarm_tree[i].children.size() < kSwarmFanOut &&
            group.swarm_tree[i].depth < best_depth) {
            best_depth = group.swarm_tree[i].depth;
            best_parent = i;
        }
    }

    if (best_parent == SIZE_MAX) {
        // No parent with room — fallback: receive from relay (depth 0)
        node.depth = 0;
        memset(&node.parent_addr, 0, sizeof(sockaddr_in));
    } else {
        node.parent_addr = group.swarm_tree[best_parent].addr;
        node.depth = group.swarm_tree[best_parent].depth + 1;
        group.swarm_tree[best_parent].children.push_back(node.addr);

        // Tell the parent about the new child
        std::string add_msg = "SWARM_ADD_CHILD:" + addr_str(node.addr) + "\n";
        sendto(g_udp_sock, add_msg.c_str(), add_msg.size(), 0,
               (const sockaddr*)&group.swarm_tree[best_parent].addr,
               sizeof(group.swarm_tree[best_parent].addr));
    }

    // Tell the new member its assignment
    send_swarm_assign(node);

    fprintf(stderr, "[swarm] Added member %s to group '%s' at depth %d (parent=%s)\n",
            addr_str(node.addr).c_str(), group.name.c_str(), node.depth,
            best_parent == SIZE_MAX ? "relay" : addr_str(group.swarm_tree[best_parent].addr).c_str());
}

// Remove a member from the swarm tree, reassigning orphaned children.
// Caller must hold g_mutex.
static void swarm_remove_member(Group& group, const sockaddr_in& removed_addr) {
    if (!group.swarm_active || group.swarm_tree.empty()) return;

    // Find the removed node in the swarm tree
    size_t removed_idx = SIZE_MAX;
    for (size_t i = 0; i < group.swarm_tree.size(); i++) {
        if (addr_equal(group.swarm_tree[i].addr, removed_addr)) {
            removed_idx = i;
            break;
        }
    }
    if (removed_idx == SIZE_MAX) return;

    SwarmNode& removed = group.swarm_tree[removed_idx];

    // Collect orphaned children (the children who were receiving from this node)
    std::vector<sockaddr_in> orphans = removed.children;

    // Remove this node as child from its parent
    for (auto& sn : group.swarm_tree) {
        auto it = std::remove_if(sn.children.begin(), sn.children.end(),
            [&](const sockaddr_in& c) { return addr_equal(c, removed_addr); });
        if (it != sn.children.end()) {
            sn.children.erase(it, sn.children.end());
            // Notify the parent to stop forwarding to this child
            std::string rm_msg = "SWARM_REMOVE_CHILD:" + addr_str(removed_addr) + "\n";
            sendto(g_udp_sock, rm_msg.c_str(), rm_msg.size(), 0,
                   (const sockaddr*)&sn.addr, sizeof(sn.addr));
            break;
        }
    }

    // Erase the removed node from swarm_tree
    group.swarm_tree.erase(group.swarm_tree.begin() + removed_idx);

    // Reassign each orphan to the shallowest available parent
    for (const auto& orphan_addr : orphans) {
        // Find the orphan in swarm_tree
        size_t orphan_idx = SIZE_MAX;
        for (size_t i = 0; i < group.swarm_tree.size(); i++) {
            if (addr_equal(group.swarm_tree[i].addr, orphan_addr)) {
                orphan_idx = i;
                break;
            }
        }
        if (orphan_idx == SIZE_MAX) continue;

        // Find best new parent
        int best_depth = INT_MAX;
        size_t best_parent = SIZE_MAX;
        for (size_t i = 0; i < group.swarm_tree.size(); i++) {
            if (i == orphan_idx) continue;
            if (!group.swarm_tree[i].is_relay_capable) continue;
            if (group.swarm_tree[i].children.size() < kSwarmFanOut &&
                group.swarm_tree[i].depth < best_depth) {
                best_depth = group.swarm_tree[i].depth;
                best_parent = i;
            }
        }

        if (best_parent != SIZE_MAX) {
            group.swarm_tree[orphan_idx].parent_addr = group.swarm_tree[best_parent].addr;
            group.swarm_tree[orphan_idx].depth = group.swarm_tree[best_parent].depth + 1;
            group.swarm_tree[best_parent].children.push_back(orphan_addr);

            // Notify the orphan about new parent
            std::string promote_msg = "SWARM_PROMOTE:" + addr_str(group.swarm_tree[best_parent].addr) + "\n";
            sendto(g_udp_sock, promote_msg.c_str(), promote_msg.size(), 0,
                   (const sockaddr*)&orphan_addr, sizeof(orphan_addr));

            // Notify the new parent about the new child
            std::string add_msg = "SWARM_ADD_CHILD:" + addr_str(orphan_addr) + "\n";
            sendto(g_udp_sock, add_msg.c_str(), add_msg.size(), 0,
                   (const sockaddr*)&group.swarm_tree[best_parent].addr,
                   sizeof(group.swarm_tree[best_parent].addr));
        } else {
            // No available parent — orphan receives from relay (depth 0)
            group.swarm_tree[orphan_idx].depth = 0;
            memset(&group.swarm_tree[orphan_idx].parent_addr, 0, sizeof(sockaddr_in));
            std::string promote_msg = "SWARM_PROMOTE:0.0.0.0:0\n";
            sendto(g_udp_sock, promote_msg.c_str(), promote_msg.size(), 0,
                   (const sockaddr*)&orphan_addr, sizeof(orphan_addr));
        }
    }

    // Check if we should deactivate swarm
    if (group.members.size() <= kSwarmThreshold) {
        rebuild_swarm_tree(group);  // will deactivate
    }
}

// Handle SWARM_LOST: client reports parent is unreachable.
// Find the client, temporarily make it a root node, then find a new parent.
// Caller must hold g_mutex.
static void handle_swarm_lost(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 11, len - 11);  // skip "SWARM_LOST:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    // Parse lost parent address: <ip>:<port>
    size_t colon = payload.find(':');
    if (colon == std::string::npos) return;
    std::string lost_ip = payload.substr(0, colon);
    uint16_t lost_port = (uint16_t)std::atoi(payload.substr(colon + 1).c_str());

    std::lock_guard<std::shared_mutex> lock(g_mutex);

    // Find sender's group
    std::string gname = find_member_group(from);
    if (gname.empty()) return;

    auto git = g_groups.find(gname);
    if (git == g_groups.end()) return;
    Group& group = git->second;

    if (!group.swarm_active || group.swarm_tree.empty()) return;

    // Find the sender in the swarm tree
    size_t sender_idx = SIZE_MAX;
    for (size_t i = 0; i < group.swarm_tree.size(); i++) {
        if (addr_equal(group.swarm_tree[i].addr, from)) {
            sender_idx = i;
            break;
        }
    }
    if (sender_idx == SIZE_MAX) return;

    // Temporarily promote to root (receive from relay directly)
    group.swarm_tree[sender_idx].depth = 0;
    memset(&group.swarm_tree[sender_idx].parent_addr, 0, sizeof(sockaddr_in));

    // Find a new parent for this node
    int best_depth = INT_MAX;
    size_t best_parent = SIZE_MAX;
    for (size_t i = 0; i < group.swarm_tree.size(); i++) {
        if (i == sender_idx) continue;
        if (!group.swarm_tree[i].is_relay_capable) continue;
        if (group.swarm_tree[i].children.size() < kSwarmFanOut &&
            group.swarm_tree[i].depth < best_depth) {
            // Don't reassign to the lost parent
            sockaddr_in check{};
            check.sin_family = AF_INET;
            inet_pton(AF_INET, lost_ip.c_str(), &check.sin_addr);
            check.sin_port = htons(lost_port);
            if (addr_equal(group.swarm_tree[i].addr, check)) continue;

            best_depth = group.swarm_tree[i].depth;
            best_parent = i;
        }
    }

    if (best_parent != SIZE_MAX) {
        group.swarm_tree[sender_idx].parent_addr = group.swarm_tree[best_parent].addr;
        group.swarm_tree[sender_idx].depth = group.swarm_tree[best_parent].depth + 1;
        group.swarm_tree[best_parent].children.push_back(from);

        std::string promote_msg = "SWARM_PROMOTE:" + addr_str(group.swarm_tree[best_parent].addr) + "\n";
        sendto(g_udp_sock, promote_msg.c_str(), promote_msg.size(), 0,
               (const sockaddr*)&from, sizeof(from));

        std::string add_msg = "SWARM_ADD_CHILD:" + addr_str(from) + "\n";
        sendto(g_udp_sock, add_msg.c_str(), add_msg.size(), 0,
               (const sockaddr*)&group.swarm_tree[best_parent].addr,
               sizeof(group.swarm_tree[best_parent].addr));

        fprintf(stderr, "[swarm] Reassigned %s in group '%s' from lost parent %s:%u to %s\n",
                addr_str(from).c_str(), gname.c_str(), lost_ip.c_str(), lost_port,
                addr_str(group.swarm_tree[best_parent].addr).c_str());
    } else {
        // Stay as root — relay will forward directly
        std::string promote_msg = "SWARM_PROMOTE:0.0.0.0:0\n";
        sendto(g_udp_sock, promote_msg.c_str(), promote_msg.size(), 0,
               (const sockaddr*)&from, sizeof(from));
        fprintf(stderr, "[swarm] %s in group '%s' promoted to root (no available parent)\n",
                addr_str(from).c_str(), gname.c_str());
    }
}

// Handle SWARM_READY: client tells relay it can relay to others.
// Caller acquires g_mutex inside.
static void handle_swarm_ready(const sockaddr_in& from) {
    std::lock_guard<std::shared_mutex> lock(g_mutex);
    std::string gname = find_member_group(from);
    if (gname.empty()) return;
    auto git = g_groups.find(gname);
    if (git == g_groups.end()) return;
    Group& group = git->second;
    if (!group.swarm_active) return;
    for (auto& sn : group.swarm_tree) {
        if (addr_equal(sn.addr, from)) {
            sn.is_relay_capable = true;
            break;
        }
    }
}

// Handle SWARM_UNABLE: client can't relay (symmetric NAT, mobile data saver).
// Caller acquires g_mutex inside.
static void handle_swarm_unable(const sockaddr_in& from) {
    std::lock_guard<std::shared_mutex> lock(g_mutex);
    std::string gname = find_member_group(from);
    if (gname.empty()) return;
    auto git = g_groups.find(gname);
    if (git == g_groups.end()) return;
    Group& group = git->second;
    if (!group.swarm_active) return;

    for (size_t i = 0; i < group.swarm_tree.size(); i++) {
        if (addr_equal(group.swarm_tree[i].addr, from)) {
            group.swarm_tree[i].is_relay_capable = false;
            // If this node has children, they need to be reassigned
            if (!group.swarm_tree[i].children.empty()) {
                // Collect and reassign children
                std::vector<sockaddr_in> orphans = group.swarm_tree[i].children;
                group.swarm_tree[i].children.clear();

                for (const auto& orphan_addr : orphans) {
                    size_t orphan_idx = SIZE_MAX;
                    for (size_t j = 0; j < group.swarm_tree.size(); j++) {
                        if (addr_equal(group.swarm_tree[j].addr, orphan_addr)) {
                            orphan_idx = j;
                            break;
                        }
                    }
                    if (orphan_idx == SIZE_MAX) continue;

                    // Find new parent
                    int best_depth = INT_MAX;
                    size_t best_parent = SIZE_MAX;
                    for (size_t j = 0; j < group.swarm_tree.size(); j++) {
                        if (j == orphan_idx || j == i) continue;
                        if (!group.swarm_tree[j].is_relay_capable) continue;
                        if (group.swarm_tree[j].children.size() < kSwarmFanOut &&
                            group.swarm_tree[j].depth < best_depth) {
                            best_depth = group.swarm_tree[j].depth;
                            best_parent = j;
                        }
                    }

                    if (best_parent != SIZE_MAX) {
                        group.swarm_tree[orphan_idx].parent_addr = group.swarm_tree[best_parent].addr;
                        group.swarm_tree[orphan_idx].depth = group.swarm_tree[best_parent].depth + 1;
                        group.swarm_tree[best_parent].children.push_back(orphan_addr);

                        std::string promote_msg = "SWARM_PROMOTE:" + addr_str(group.swarm_tree[best_parent].addr) + "\n";
                        sendto(g_udp_sock, promote_msg.c_str(), promote_msg.size(), 0,
                               (const sockaddr*)&orphan_addr, sizeof(orphan_addr));
                        std::string add_msg = "SWARM_ADD_CHILD:" + addr_str(orphan_addr) + "\n";
                        sendto(g_udp_sock, add_msg.c_str(), add_msg.size(), 0,
                               (const sockaddr*)&group.swarm_tree[best_parent].addr,
                               sizeof(group.swarm_tree[best_parent].addr));
                    } else {
                        group.swarm_tree[orphan_idx].depth = 0;
                        memset(&group.swarm_tree[orphan_idx].parent_addr, 0, sizeof(sockaddr_in));
                        std::string promote_msg = "SWARM_PROMOTE:0.0.0.0:0\n";
                        sendto(g_udp_sock, promote_msg.c_str(), promote_msg.size(), 0,
                               (const sockaddr*)&orphan_addr, sizeof(orphan_addr));
                    }
                }
            }
            break;
        }
    }
}

// Handle SWARM_ACK: client confirms it's receiving from assigned parent.
// Just an informational ack — no state change needed.
static void handle_swarm_ack(const char* /*msg*/, size_t /*len*/, const sockaddr_in& /*from*/) {
    // Informational — could be used for latency tracking in the future.
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

    std::lock_guard<std::shared_mutex> lock(g_mutex);

    // ── Channel ownership enforcement ────────────────────────────────────
    // Random channels (6 hex chars) and built-in free names bypass ownership.
    // Custom names: if claimed by another device, reject.
    if (!is_random_channel(group_name) && !is_free_name(group_name)) {
        auto ch_it = g_channels.find(group_name);
        if (ch_it != g_channels.end() && ch_it->second.expires > now_unix()) {
            // Channel is claimed — check if this device owns it
            if (ch_it->second.device != device_name) {
                const char* err = "ERR:channel_reserved\n";
                sendto(g_udp_sock, err, strlen(err), 0,
                       (const sockaddr*)&from, sizeof(from));
                return;
            }
        }
        // Not claimed or owned by this device → allowed
    }

    // Remove member from any existing group first
    std::string old_group = find_member_group(from);
    if (!old_group.empty() && old_group != group_name) {
        auto it = g_groups.find(old_group);
        if (it != g_groups.end()) {
            // Remove from swarm tree before erasing from members
            swarm_remove_member(it->second, from);
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

    // Notify existing members about the new peer, and new peer about existing members
    std::string new_peer_msg = "PEER:" + addr_str(from) + "\n";
    for (const auto& m : group.members) {
        // Tell existing member about the new joiner
        sendto(g_udp_sock, new_peer_msg.c_str(), new_peer_msg.size(), 0,
               (const sockaddr*)&m.addr, sizeof(m.addr));
        // Tell new joiner about existing member
        std::string existing_msg = "PEER:" + addr_str(m.addr) + "\n";
        sendto(g_udp_sock, existing_msg.c_str(), existing_msg.size(), 0,
               (const sockaddr*)&from, sizeof(from));
    }

    Member member;
    member.addr = from;
    member.last_seen = now;
    member.device_name = device_name;
    member.session_token = generate_hex(16);

    // Assign role based on channel ownership:
    // - Random/free channels: everyone is DJ (open broadcast)
    // - Owned channels: owner gets Owner role, first member gets DJ, others are Listeners
    if (is_random_channel(group_name) || is_free_name(group_name)) {
        member.role = MemberRole::DJ;
    } else {
        auto ch_it = g_channels.find(group_name);
        if (ch_it != g_channels.end() && ch_it->second.device == device_name) {
            member.role = MemberRole::Owner;
        } else if (group.members.empty()) {
            // First joiner of unclaimed channel gets DJ
            member.role = MemberRole::DJ;
        } else {
            member.role = MemberRole::Listener;
        }
    }

    group.members.push_back(member);
    size_t new_member_idx = group.members.size() - 1;
    g_total_joins++;

    // Update reverse lookup map
    g_addr_to_group[addr_key(from)] = group_name;

    // P2P Swarm: add new member to swarm tree (or activate swarm if threshold crossed)
    swarm_add_member(group, new_member_idx);

    // If running as region/edge, send CASCADE_JOIN upstream for new groups
    if ((g_tier == RelayTier::Region || g_tier == RelayTier::Edge) && g_upstream_connected) {
        // Check if this is the first local member in this group — if so, subscribe upstream
        if (group.members.size() == 1) {
            std::string cascade_msg = "CASCADE_JOIN:" + group_name + ":" + g_my_peer_id + ":" + g_cascade_secret + "\n";
            sendto(g_udp_sock, cascade_msg.c_str(), cascade_msg.size(), 0,
                   (const sockaddr*)&g_upstream_addr, sizeof(g_upstream_addr));
        }
    }

    const char* role_name = member.role == MemberRole::Owner ? "owner" :
                            member.role == MemberRole::DJ ? "dj" : "listener";
    fprintf(stderr, "[relay] %s joined group '%s' (%zu members, role=%s)%s\n",
            addr_str(from).c_str(), group_name.c_str(), group.members.size(),
            role_name,
            device_name.empty() ? "" : (" [" + device_name + "]").c_str());

    // Tell the joiner their role
    char role_msg[64];
    snprintf(role_msg, sizeof(role_msg), "ROLE:%s\n", role_name);

    const char* ok = "OK:joined\n";
    sendto(g_udp_sock, ok, strlen(ok), 0,
           (const sockaddr*)&from, sizeof(from));
    sendto(g_udp_sock, role_msg, strlen(role_msg), 0,
           (const sockaddr*)&from, sizeof(from));

    // Send session token (required for wallet/billing operations)
    std::string token_msg = "SESSION:" + member.session_token + "\n";
    sendto(g_udp_sock, token_msg.c_str(), token_msg.size(), 0,
           (const sockaddr*)&from, sizeof(from));

    // Tell the joiner the channel mode
    const char* mode_str = group.mode == ChannelMode::Private ? "private" : "public";
    char mode_msg[64];
    snprintf(mode_msg, sizeof(mode_msg), "MODE:%s\n", mode_str);
    sendto(g_udp_sock, mode_msg, strlen(mode_msg), 0,
           (const sockaddr*)&from, sizeof(from));

    // For private channels, send PEER addresses so client can attempt P2P
    if (group.mode == ChannelMode::Private) {
        for (const auto& m : group.members) {
            if (addr_equal(m.addr, from)) continue;
            std::string peer_msg = "PEER:" + addr_str(m.addr) + "\n";
            sendto(g_udp_sock, peer_msg.c_str(), peer_msg.size(), 0,
                   (const sockaddr*)&from, sizeof(from));
        }
    }

    // Send last metadata to new joiner if available
    if (!group.last_meta.empty()) {
        std::string meta_msg = "META:" + group.last_meta + "\n";
        sendto(g_udp_sock, meta_msg.c_str(), meta_msg.size(), 0,
               (const sockaddr*)&from, sizeof(from));
    }

    // Send current file and sync state to new joiner (file-sync mode)
    if (!group.current_file.empty()) {
        std::string file_msg = "FILE:" + group.current_file + "\n";
        sendto(g_udp_sock, file_msg.c_str(), file_msg.size(), 0,
               (const sockaddr*)&from, sizeof(from));
    }
    if (!group.last_sync.empty()) {
        std::string sync_msg = "SYNC:" + group.last_sync + "\n";
        sendto(g_udp_sock, sync_msg.c_str(), sync_msg.size(), 0,
               (const sockaddr*)&from, sizeof(from));
    }

    // Send cached last lyric text to new joiner
    if (!group.last_text.empty()) {
        std::string text_msg = "TEXT:" + group.last_text + "\n";
        sendto(g_udp_sock, text_msg.c_str(), text_msg.size(), 0,
               (const sockaddr*)&from, sizeof(from));
    }

    // Send active P2P file manifests to new joiner
    for (const auto& [sha, fm] : group.file_manifests) {
        std::string manifest_msg = "FILE_MANIFEST:" + fm.filename + ":" +
                                   std::to_string(fm.size_bytes) + ":" + fm.sha256 + ":" +
                                   std::to_string(fm.num_chunks) + "\n";
        sendto(g_udp_sock, manifest_msg.c_str(), manifest_msg.size(), 0,
               (const sockaddr*)&from, sizeof(from));
        // Also send URL if available
        if (!fm.url.empty()) {
            std::string url_msg = "FILE_URL:" + fm.url + ":" + fm.sha256 + ":" +
                                  std::to_string(fm.size_bytes) + "\n";
            sendto(g_udp_sock, url_msg.c_str(), url_msg.size(), 0,
                   (const sockaddr*)&from, sizeof(from));
        }
    }
}

// ── CHECK handler ─────────────────────────────────────────────────────────────
// Format: "CHECK:<name>\n"

static void handle_check(const char* msg, size_t len, const sockaddr_in& from) {
    std::string name(msg + 6, len - 6);
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r'))
        name.pop_back();

    std::lock_guard<std::shared_mutex> lock(g_mutex);
    auto it = g_channels.find(name);
    if (it != g_channels.end() && it->second.expires > now_unix()) {
        const char* reply = "OK:taken\n";
        sendto(g_udp_sock, reply, strlen(reply), 0,
               (const sockaddr*)&from, sizeof(from));
    } else {
        const char* reply = "OK:available\n";
        sendto(g_udp_sock, reply, strlen(reply), 0,
               (const sockaddr*)&from, sizeof(from));
    }
}

// ── CLAIM handler ─────────────────────────────────────────────────────────────
// Format: "CLAIM:<name>:<device_id>:<transaction_id>\n"

static void handle_claim(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 6, len - 6);
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    // Split into name:device:txn
    std::string name, device, txn;
    size_t p1 = payload.find(':');
    if (p1 == std::string::npos) {
        const char* err = "ERR:invalid_format\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }
    name = payload.substr(0, p1);
    size_t p2 = payload.find(':', p1 + 1);
    if (p2 == std::string::npos) {
        const char* err = "ERR:invalid_format\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }
    device = payload.substr(p1 + 1, p2 - p1 - 1);
    txn = payload.substr(p2 + 1);

    if (!is_valid_channel_name(name)) {
        const char* err = "ERR:invalid_name\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    if (is_free_name(name)) {
        const char* err = "ERR:reserved_name\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    std::lock_guard<std::shared_mutex> lock(g_mutex);

    int64_t expiry = now_unix() + kChannelExpiryDays * 86400;
    auto it = g_channels.find(name);

    if (it == g_channels.end() || it->second.expires <= now_unix()) {
        // Available (not claimed or expired) → claim it
        g_channels[name] = {device, txn, expiry, "", ""};
        channels_save();
        fprintf(stderr, "[relay] Channel '%s' claimed by device %s (txn %s)\n",
                name.c_str(), device.c_str(), txn.c_str());
        const char* reply = "OK:claimed\n";
        sendto(g_udp_sock, reply, strlen(reply), 0, (const sockaddr*)&from, sizeof(from));
    } else if (it->second.device == device) {
        // Same device → renew
        it->second.txn = txn;
        it->second.expires = expiry;
        channels_save();
        fprintf(stderr, "[relay] Channel '%s' renewed by device %s\n",
                name.c_str(), device.c_str());
        const char* reply = "OK:renewed\n";
        sendto(g_udp_sock, reply, strlen(reply), 0, (const sockaddr*)&from, sizeof(from));
    } else {
        // Taken by another device
        const char* reply = "ERR:taken\n";
        sendto(g_udp_sock, reply, strlen(reply), 0, (const sockaddr*)&from, sizeof(from));
    }
}

// ── RELEASE handler ───────────────────────────────────────────────────────────
// Format: "RELEASE:<name>:<device_id>\n"

static void handle_release(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 8, len - 8);
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    std::string name, device;
    size_t p1 = payload.find(':');
    if (p1 == std::string::npos) {
        const char* err = "ERR:invalid_format\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }
    name = payload.substr(0, p1);
    device = payload.substr(p1 + 1);

    std::lock_guard<std::shared_mutex> lock(g_mutex);

    auto it = g_channels.find(name);
    if (it != g_channels.end() && it->second.device == device) {
        g_channels.erase(it);
        channels_save();
        fprintf(stderr, "[relay] Channel '%s' released by device %s\n",
                name.c_str(), device.c_str());
    }
    // Always reply OK (idempotent)
    const char* reply = "OK:released\n";
    sendto(g_udp_sock, reply, strlen(reply), 0, (const sockaddr*)&from, sizeof(from));
}

// ── Permission helpers ─────────────────────────────────────────────────────────

static Member* find_member(Group& group, const sockaddr_in& addr) {
    for (auto& m : group.members) {
        if (addr_equal(m.addr, addr)) return &m;
    }
    return nullptr;
}

// ── MODE handler ──────────────────────────────────────────────────────────────
// Format: "MODE:<private|public>\n"
// Only channel owner (or DJ on free channels) can change mode.
// Private: P2P preferred, no copyright detection, not listed in discovery.
// Public:  relay forwarding, copyright detection, listed in discovery.

static void handle_mode(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 5, len - 5);  // skip "MODE:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    ChannelMode new_mode;
    if (payload == "private") {
        new_mode = ChannelMode::Private;
    } else if (payload == "public") {
        new_mode = ChannelMode::Public;
    } else {
        const char* err = "ERR:invalid_mode\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    std::lock_guard<std::shared_mutex> lock(g_mutex);

    std::string group_name = find_member_group(from);
    if (group_name.empty()) {
        const char* err = "ERR:not_in_group\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    auto& group = g_groups[group_name];
    Member* sender = find_member(group, from);
    if (!sender) return;

    // Only Owner can change mode on owned channels; DJ+ on free/random channels
    if (is_random_channel(group_name) || is_free_name(group_name)) {
        if (sender->role < MemberRole::DJ) {
            const char* err = "ERR:permission_denied\n";
            sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
            return;
        }
    } else {
        if (sender->role < MemberRole::Owner) {
            const char* err = "ERR:owner_only\n";
            sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
            return;
        }
    }

    group.mode = new_mode;
    const char* mode_str = new_mode == ChannelMode::Private ? "private" : "public";

    // Notify all members of mode change
    char notify[64];
    snprintf(notify, sizeof(notify), "MODE:%s\n", mode_str);
    for (const auto& m : group.members) {
        sendto(g_udp_sock, notify, strlen(notify), 0,
               (const sockaddr*)&m.addr, sizeof(m.addr));
    }

    // Private mode: send PEER addresses to all members for P2P hole-punching
    if (new_mode == ChannelMode::Private) {
        for (const auto& m : group.members) {
            for (const auto& other : group.members) {
                if (addr_equal(m.addr, other.addr)) continue;
                std::string peer_msg = "PEER:" + addr_str(other.addr) + "\n";
                sendto(g_udp_sock, peer_msg.c_str(), peer_msg.size(), 0,
                       (const sockaddr*)&m.addr, sizeof(m.addr));
            }
        }
    }

    fprintf(stderr, "[relay] Group '%s' mode changed to %s by %s\n",
            group_name.c_str(), mode_str, addr_str(from).c_str());
}

// ── GRANT handler ─────────────────────────────────────────────────────────────
// Format: "GRANT:<role>:<device_name>\n"  (role = "dj" or "listener")
// Only channel owner can grant roles. Applies to the sender's current group.

static void handle_grant(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 6, len - 6);
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    size_t p1 = payload.find(':');
    if (p1 == std::string::npos) {
        const char* err = "ERR:invalid_format\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    std::string role_str = payload.substr(0, p1);
    std::string target_device = payload.substr(p1 + 1);

    MemberRole new_role;
    if (role_str == "dj")          new_role = MemberRole::DJ;
    else if (role_str == "listener") new_role = MemberRole::Listener;
    else {
        const char* err = "ERR:invalid_role\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    std::lock_guard<std::shared_mutex> lock(g_mutex);

    // Find sender's group
    std::string gname = find_member_group(from);
    if (gname.empty()) {
        const char* err = "ERR:not_in_group\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    auto& group = g_groups[gname];
    Member* sender = find_member(group, from);
    if (!sender || sender->role < MemberRole::Owner) {
        const char* err = "ERR:not_owner\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    // Find target member by device name
    bool found = false;
    for (auto& m : group.members) {
        if (m.device_name == target_device) {
            m.role = new_role;
            found = true;
            // Notify the target member of their new role
            char notify[128];
            snprintf(notify, sizeof(notify), "ROLE:%s\n", role_str.c_str());
            sendto(g_udp_sock, notify, strlen(notify), 0,
                   (const sockaddr*)&m.addr, sizeof(m.addr));
            break;
        }
    }

    if (found) {
        fprintf(stderr, "[relay] %s granted '%s' role to '%s' in '%s'\n",
                addr_str(from).c_str(), role_str.c_str(), target_device.c_str(), gname.c_str());
        const char* reply = "OK:granted\n";
        sendto(g_udp_sock, reply, strlen(reply), 0, (const sockaddr*)&from, sizeof(from));
    } else {
        const char* err = "ERR:member_not_found\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
    }
}

// ── MEMBERS handler ───────────────────────────────────────────────────────────
// Format: "MEMBERS\n" — list all members and their roles in the sender's group

static void handle_members(const sockaddr_in& from) {
    std::lock_guard<std::shared_mutex> lock(g_mutex);

    std::string gname = find_member_group(from);
    if (gname.empty()) {
        const char* err = "ERR:not_in_group\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    auto& group = g_groups[gname];
    const char* mode_str = group.mode == ChannelMode::Private ? "private" : "public";
    std::string response = "MEMBERS:{\"group\":\"" + json_escape(gname) +
                           "\",\"mode\":\"" + mode_str + "\",\"members\":[";
    bool first = true;
    for (const auto& m : group.members) {
        if (!first) response += ",";
        first = false;
        const char* role_name = m.role == MemberRole::Owner ? "owner" :
                                m.role == MemberRole::DJ ? "dj" : "listener";
        response += "{\"device\":\"" + json_escape(m.device_name) + "\","
                    "\"role\":\"" + role_name + "\","
                    "\"mixing\":" + (m.mixing ? std::string("true") : std::string("false")) + ","
                    "\"addr\":\"" + addr_str(m.addr) + "\"}";
    }
    response += "]}\n";
    sendto(g_udp_sock, response.c_str(), response.size(), 0,
           (const sockaddr*)&from, sizeof(from));
}

// ── PING handler (P2P latency measurement) ──────────────────────────────────
// Format: "PING:<target_ip>:<target_port>:<timestamp_ms>\n"
// Relay forwards to the target member as: "PING:<sender_ip>:<sender_port>:<timestamp_ms>\n"
// The target responds directly to the sender with PONG (client-side logic).

static void handle_ping(const char* msg, size_t len, const sockaddr_in& from) {
    // Parse: skip "PING:" prefix
    std::string payload(msg + 5, len - 5);
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    // payload = "<target_ip>:<target_port>:<timestamp_ms>"
    // Find the target address (first two colon-separated fields)
    size_t colon1 = payload.find(':');
    if (colon1 == std::string::npos) return;
    size_t colon2 = payload.find(':', colon1 + 1);
    if (colon2 == std::string::npos) return;

    std::string target_ip   = payload.substr(0, colon1);
    std::string target_port = payload.substr(colon1 + 1, colon2 - colon1 - 1);
    std::string timestamp   = payload.substr(colon2 + 1);

    // Build target sockaddr_in
    sockaddr_in target_addr{};
    target_addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, target_ip.c_str(), &target_addr.sin_addr) != 1) return;
    target_addr.sin_port = htons((uint16_t)std::atoi(target_port.c_str()));

    std::lock_guard<std::shared_mutex> lock(g_mutex);

    // Verify both sender and target are in the same group
    std::string sender_group = find_member_group(from);
    if (sender_group.empty()) return;

    auto it = g_groups.find(sender_group);
    if (it == g_groups.end()) return;

    bool target_found = false;
    for (const auto& m : it->second.members) {
        if (addr_equal(m.addr, target_addr)) { target_found = true; break; }
    }
    if (!target_found) return;

    // Forward PING to target with sender's address
    std::string fwd = "PING:" + addr_str(from) + ":" + timestamp + "\n";
    sendto(g_udp_sock, fwd.c_str(), fwd.size(), 0,
           (const sockaddr*)&target_addr, sizeof(target_addr));
}

// ── ROUTE handler (informational path preference) ───────────────────────────
// Format: "ROUTE:<peer_ip>:<peer_port>:<p2p|relay>\n"
// Client informs relay which path it chose for a given peer.
// Informational only — relay always forwards audio regardless.

static void handle_route(const char* /* msg */, size_t /* len */, const sockaddr_in& /* from */) {
    // No-op: informational message logged for future analytics.
    // Relay continues forwarding audio to all members regardless of client preference.
    // Client-side deduplication handles packets arriving on both paths.
}

// ── HELLO handler ─────────────────────────────────────────────────────────────

static void handle_hello(const sockaddr_in& from) {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::shared_mutex> lock(g_mutex);

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

// ── Copyright detection & royalty tracking ──────────────────────────────────

// Simplified spectral fingerprint — computes energy in frequency bands
// In production, replace with Chromaprint library integration
static uint64_t compute_audio_fingerprint(const int16_t* samples, size_t count) {
    // Compute energy in 8 frequency bands over 32 windows
    // Each bit = whether energy increased or decreased vs previous window
    uint64_t hash = 0;
    size_t window_size = count / 32;
    if (window_size == 0) return 0;

    double prev_energies[8] = {};

    for (int w = 0; w < 32; w++) {
        double energies[8] = {};
        const int16_t* win = samples + w * window_size;

        // Simple DFT energy in 8 bands
        for (int band = 0; band < 8; band++) {
            double freq = 200.0 * (1 << band);  // 200, 400, 800, ..., 25600 Hz
            double real = 0, imag = 0;
            for (size_t i = 0; i < window_size; i++) {
                double angle = 2.0 * M_PI * freq * (double)i / (double)kFingerprintSampleRate;
                real += win[i] * cos(angle);
                imag += win[i] * sin(angle);
            }
            energies[band] = real * real + imag * imag;
        }

        // Compare with previous window
        if (w > 0) {
            for (int band = 0; band < 2; band++) {  // use 2 bits per window
                if (energies[band] > prev_energies[band]) {
                    hash |= (1ULL << (w * 2 + band));
                }
            }
        }

        memcpy(prev_energies, energies, sizeof(energies));
    }

    return hash;
}

// Match fingerprint against known track database
static bool match_fingerprint(uint64_t fingerprint, const std::string& group_name,
                               CopyrightMatch& out_match) {
    // Method 1: Local database lookup (hash -> track info)
    // In production, this would be a large database or Bloom filter
    // For now: stub that checks against a known-tracks file

    // Method 2: External API call (async, non-blocking)
    // If g_fingerprint_api_url is set, POST the fingerprint for matching
    // Response: { "match": true, "track": {...}, "confidence": 0.95 }

    // Stub implementation — always returns false (no local DB)
    // Real implementation hooks into ACRCloud, AudibleMagic, or self-hosted DB
    (void)fingerprint;
    (void)group_name;
    (void)out_match;
    return false;
}

static void append_royalty_log(const RoyaltyEntry& e) {
    FILE* f = fopen(g_royalty_log_path.c_str(), "a");
    if (!f) return;
    fprintf(f, "{\"ts\":%lld,\"group\":\"%s\",\"dj\":\"%s\",\"isrc\":\"%s\","
               "\"track\":\"%s\",\"artist\":\"%s\",\"rights_holder\":\"%s\","
               "\"duration_sec\":%llu,\"listeners\":%u,\"royalty_usd\":%.4f}\n",
            (long long)e.timestamp, e.group_name.c_str(), e.dj_device.c_str(),
            e.isrc.c_str(), e.track_title.c_str(), e.artist.c_str(),
            e.rights_holder.c_str(), (unsigned long long)e.play_duration_sec,
            e.listener_count, e.royalty_amount);
    fclose(f);
}

static void notify_copyright_detected(const std::string& group_name, Group& group,
                                       const CopyrightMatch& match) {
    // Build COPYRIGHT_DETECT JSON — send to DJ only
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "COPYRIGHT_DETECT:{\"track\":\"%s\",\"artist\":\"%s\","
        "\"isrc\":\"%s\",\"rights_holder\":\"%s\","
        "\"confidence\":%.2f,\"action\":\"warn\"}\n",
        match.track_title.c_str(), match.artist.c_str(),
        match.isrc.c_str(), match.rights_holder.c_str(),
        match.confidence);

    for (auto& m : group.members) {
        if (m.role >= MemberRole::DJ) {
            sendto(g_udp_sock, buf, strlen(buf), 0,
                   (const sockaddr*)&m.addr, sizeof(m.addr));
        }
    }

    // Send COPYRIGHT_INFO to all listeners (informational, not a warning)
    char info[1024];
    snprintf(info, sizeof(info),
        "COPYRIGHT_INFO:{\"track\":\"%s\",\"artist\":\"%s\","
        "\"album\":\"%s\",\"rights_holder\":\"%s\",\"now_playing\":true}\n",
        match.track_title.c_str(), match.artist.c_str(),
        match.album.c_str(), match.rights_holder.c_str());

    for (auto& m : group.members) {
        sendto(g_udp_sock, info, strlen(info), 0,
               (const sockaddr*)&m.addr, sizeof(m.addr));
    }

    fprintf(stderr, "[copyright] Detected '%s' by %s in group '%s' (confidence=%.2f)\n",
            match.track_title.c_str(), match.artist.c_str(),
            group_name.c_str(), match.confidence);
}

// Helper: send a UDP message to a specific DJ member in a group. Caller must hold g_mutex.
static void send_to_dj(const Group& group, const char* msg, size_t msg_len) {
    for (const auto& m : group.members) {
        if (m.role >= MemberRole::DJ) {
            sendto(g_udp_sock, msg, msg_len, 0,
                   (const sockaddr*)&m.addr, sizeof(m.addr));
        }
    }
}

// Helper: deduct royalty from DJ wallet and distribute 70/20/10 split.
// Called by both finalize_royalty (end of song) and royalty_tick (periodic).
// Caller must NOT hold g_wallet_mutex (this function acquires it).
static void deduct_royalty_from_wallet(const std::string& dj_device,
                                        const std::string& rights_holder,
                                        const std::string& isrc,
                                        double royalty_amount,
                                        const std::string& track_title,
                                        const std::string& artist,
                                        const Group& group,
                                        const std::string& group_name) {
    std::lock_guard<std::mutex> wlock(g_wallet_mutex);

    auto& dj_wallet = g_wallets[dj_device];
    if (dj_wallet.device_id.empty()) {
        dj_wallet.device_id = dj_device;
        dj_wallet.last_activity = std::chrono::steady_clock::now();
    }

    if (dj_wallet.balance >= royalty_amount) {
        // Deduct from DJ
        dj_wallet.balance -= royalty_amount;
        dj_wallet.total_royalties_paid += royalty_amount;

        // Distribute: 70% rights holder, 10% platform, 20% DJ cashback
        double rights_share = royalty_amount * 0.70;
        double platform_share = royalty_amount * 0.10;
        double dj_cashback = royalty_amount * 0.20;

        // Credit rights holder
        g_rights_holder_balances[rights_holder] += rights_share;

        // DJ cashback
        dj_wallet.balance += dj_cashback;
        dj_wallet.total_earned += dj_cashback;
        dj_wallet.last_activity = std::chrono::steady_clock::now();

        // Update payout pending if rights holder has payout account
        auto pa_it = g_payouts.find(rights_holder);
        if (pa_it != g_payouts.end()) {
            pa_it->second.pending_payout += rights_share;
        }

        // Log transactions
        char desc[256];
        snprintf(desc, sizeof(desc), "%s by %s", track_title.c_str(), artist.c_str());
        log_transaction(dj_device, "rights:" + isrc, rights_share, "royalty", desc);
        log_transaction(dj_device, "platform", platform_share, "platform_fee", desc);
        log_transaction("platform", dj_device, dj_cashback, "cashback", desc);

        // Remove grace period if DJ has funds
        g_grace_periods.erase(dj_device);

        // Notify DJ of charge
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "COPYRIGHT_CHARGE:{\"amount\":%.4f,\"balance\":%.2f,"
                 "\"track\":\"%s\",\"artist\":\"%s\",\"cashback\":%.4f}\n",
                 royalty_amount, dj_wallet.balance,
                 json_escape(track_title).c_str(),
                 json_escape(artist).c_str(),
                 dj_cashback);
        send_to_dj(group, msg, strlen(msg));
    } else {
        // Insufficient balance — warn DJ, start grace period
        auto grace_it = g_grace_periods.find(dj_device);
        auto now_tp = std::chrono::steady_clock::now();

        if (grace_it == g_grace_periods.end()) {
            g_grace_periods[dj_device] = now_tp;
        } else {
            auto grace_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now_tp - grace_it->second).count();
            if (grace_elapsed >= kGracePeriodSec) {
                // Grace period expired — auto-switch to private mode
                // (Mode change must happen outside wallet lock, so we just notify)
                char warn[512];
                snprintf(warn, sizeof(warn),
                         "COPYRIGHT_WARN:{\"message\":\"Grace period expired. Switching to private mode.\","
                         "\"balance\":%.2f,\"action\":\"auto_private\"}\n",
                         dj_wallet.balance);
                send_to_dj(group, warn, strlen(warn));

                // Log accumulated debt as a transaction
                log_transaction(dj_device, "platform", royalty_amount, "royalty_debt",
                                "Insufficient balance, accumulated debt");

                fprintf(stderr, "[wallet] DJ '%s' grace period expired in '%s', switching to private\n",
                        dj_device.c_str(), group_name.c_str());
                return;  // caller should switch mode
            }
        }

        double estimated_per_min = calculate_royalty_per_min(group.members.size());
        char warn[512];
        snprintf(warn, sizeof(warn),
                 "COPYRIGHT_WARN:{\"message\":\"Low balance. Public mode requires credits "
                 "for copyrighted music. Top up or switch to private.\","
                 "\"balance\":%.2f,\"estimated_cost_per_min\":%.4f}\n",
                 dj_wallet.balance, estimated_per_min);
        send_to_dj(group, warn, strlen(warn));
    }
}

static void finalize_royalty(const std::string& group_name, const Group& group,
                              const GroupCopyrightState& cs) {
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        now - cs.match_start).count();

    if (duration < 10) return;  // ignore very short plays (< 10s, likely skip)

    // NOTE: Actual royalty deduction happens in royalty_tick() every 60 seconds,
    // using the real-time listener count at each tick. This avoids the problem of
    // charging the final listener count for the entire duration.
    //
    // finalize_royalty() only handles:
    // 1. Logging the completed play to the royalty ledger (for reporting)
    // 2. Charging the REMAINING partial minute (< 60s since last tick)
    //
    // This means: if a song played for 3m45s with varying listeners:
    //   tick@1min: calculate_royalty_per_min(50)  = $0.05  (already charged)
    //   tick@2min: calculate_royalty_per_min(80)  = $0.08  (already charged)
    //   tick@3min: calculate_royalty_per_min(120) = $0.11  (already charged)
    //   finalize:  calculate_royalty_per_min(100) * 45/60 = $0.075 (partial minute)
    //   Total: $0.315 — fair, proportional to actual listeners at each moment

    // Find DJ device name
    std::string dj_device;
    for (const auto& m : group.members) {
        if (m.role >= MemberRole::DJ) {
            dj_device = m.device_name;
            break;
        }
    }

    // Calculate partial-minute remainder
    // royalty_tick charges full minutes; we charge the leftover seconds here
    int64_t remainder_sec = duration % 60;
    double partial_minutes = (double)remainder_sec / 60.0;
    uint32_t current_listeners = (uint32_t)group.members.size();
    double partial_royalty = calculate_royalty_per_min(current_listeners) * partial_minutes;

    // Log the complete play entry (total duration for reporting, but only partial charge)
    RoyaltyEntry entry;
    entry.group_name = group_name;
    entry.isrc = cs.current_match.isrc;
    entry.track_title = cs.current_match.track_title;
    entry.artist = cs.current_match.artist;
    entry.rights_holder = cs.current_match.rights_holder;
    entry.play_duration_sec = (uint64_t)duration;
    entry.listener_count = current_listeners;
    entry.timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    entry.royalty_amount = partial_royalty;  // only the partial minute remainder
    entry.dj_device = dj_device;

    {
        std::lock_guard<std::mutex> lock(g_royalty_mutex);
        g_royalty_ledger.push_back(entry);
        g_total_royalty_cents += (uint64_t)(partial_royalty * 100);
    }

    // Append to JSONL log file
    append_royalty_log(entry);

    // Deduct only the partial-minute remainder from DJ wallet
    if (!dj_device.empty() && partial_royalty > 0.0001) {
        deduct_royalty_from_wallet(dj_device, entry.rights_holder, entry.isrc,
                                   partial_royalty, entry.track_title, entry.artist,
                                   group, group_name);
    }

    fprintf(stderr, "[copyright] Play ended: '%s' in '%s', %llus, %u listeners, partial=$%.4f\n",
            entry.track_title.c_str(), group_name.c_str(),
            (unsigned long long)entry.play_duration_sec,
            entry.listener_count, partial_royalty);
}

// ── Periodic royalty tick ────────────────────────────────────────────────────
// Called every 60 seconds by fingerprint thread.
// For each group with active copyrighted content, calculates 1 minute of royalty
// and deducts from DJ wallet in real-time.
static void royalty_tick() {
    // Caller should hold g_mutex for group iteration
    for (auto& [name, group] : g_groups) {
        if (!group.copyright || !group.copyright->is_copyrighted) continue;
        if (group.mode != ChannelMode::Public) continue;

        auto& cs = *group.copyright;

        // Calculate 1 minute of royalty
        double royalty_per_min = calculate_royalty_per_min(group.members.size());
        if (royalty_per_min < 0.0001) continue;  // negligible

        // Find DJ device name
        std::string dj_device;
        for (const auto& m : group.members) {
            if (m.role >= MemberRole::DJ) {
                dj_device = m.device_name;
                break;
            }
        }
        if (dj_device.empty()) continue;

        // Check grace period — if expired, auto-switch to private
        {
            std::lock_guard<std::mutex> wlock(g_wallet_mutex);
            auto grace_it = g_grace_periods.find(dj_device);
            if (grace_it != g_grace_periods.end()) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - grace_it->second).count();
                if (elapsed >= kGracePeriodSec) {
                    // Auto-switch to private mode
                    group.mode = ChannelMode::Private;
                    g_grace_periods.erase(grace_it);

                    char notify[64];
                    snprintf(notify, sizeof(notify), "MODE:private\n");
                    for (const auto& m : group.members) {
                        sendto(g_udp_sock, notify, strlen(notify), 0,
                               (const sockaddr*)&m.addr, sizeof(m.addr));
                    }
                    fprintf(stderr, "[wallet] Auto-switched '%s' to private (DJ '%s' insufficient balance)\n",
                            name.c_str(), dj_device.c_str());
                    continue;
                }
            }
        }

        // Deduct 1 minute of royalty (do not hold g_mutex when acquiring g_wallet_mutex
        // but we're already iterating under g_mutex — deduct_royalty_from_wallet acquires
        // g_wallet_mutex separately, which is safe since lock order is g_mutex → g_wallet_mutex)
        deduct_royalty_from_wallet(dj_device, cs.current_match.rights_holder,
                                   cs.current_match.isrc, royalty_per_min,
                                   cs.current_match.track_title, cs.current_match.artist,
                                   group, name);

        // Broadcast DJ balance to all members in real-time
        {
            std::lock_guard<std::mutex> wlock(g_wallet_mutex);
            auto wit = g_wallets.find(dj_device);
            double balance = (wit != g_wallets.end()) ? wit->second.balance : 0.0;
            double rate_per_min = royalty_per_min;

            char bal_msg[256];
            snprintf(bal_msg, sizeof(bal_msg),
                "BALANCE_UPDATE:{\"dj\":\"%s\",\"balance\":%.2f,\"rate_per_min\":%.4f,"
                "\"listeners\":%zu,\"track\":\"%s\",\"artist\":\"%s\"}\n",
                dj_device.c_str(), balance, rate_per_min,
                group.members.size(),
                cs.current_match.track_title.c_str(),
                cs.current_match.artist.c_str());
            for (const auto& m : group.members) {
                sendto(g_udp_sock, bal_msg, strlen(bal_msg), 0,
                       (const sockaddr*)&m.addr, sizeof(m.addr));
            }
        }
    }
}

static void fingerprint_thread_func() {
    uint64_t tick_counter = 0;
    auto last_wallet_save = std::chrono::steady_clock::now();

    while (g_fingerprint_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(kFingerprintIntervalSec));
        if (!g_fingerprint_running.load(std::memory_order_relaxed)) break;
        tick_counter++;

        // Iterate all groups, extract fingerprints
        std::lock_guard<std::shared_mutex> glock(g_mutex);

        // Royalty tick every ~60 seconds (60 / kFingerprintIntervalSec ticks)
        if (tick_counter % (60 / kFingerprintIntervalSec) == 0) {
            royalty_tick();
        }

        // Wallet persistence every ~30 seconds
        auto now_tp = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now_tp - last_wallet_save).count() >= 30) {
            // Release g_mutex before saving (wallets_save acquires g_wallet_mutex only)
            // Actually we save in a fire-and-forget manner within the thread
            wallets_save();
            last_wallet_save = now_tp;
        }

        for (auto& [name, group] : g_groups) {
            if (!group.copyright || !group.copyright->fp_buf.active) continue;

            auto& cs = *group.copyright;
            auto& fp = cs.fp_buf;

            // Lock fingerprint buffer
            std::lock_guard<std::mutex> flock(fp.mtx);

            if (fp.total_written < kFingerprintSamplesNeeded) continue;

            // Extract the last 5 seconds of audio
            std::vector<int16_t> samples(kFingerprintSamplesNeeded);
            size_t read_start = (fp.write_pos + kFingerprintRingSize - kFingerprintSamplesNeeded)
                                % kFingerprintRingSize;
            for (size_t i = 0; i < kFingerprintSamplesNeeded; i++) {
                samples[i] = fp.ring[(read_start + i) % kFingerprintRingSize];
            }

            // Compute fingerprint hash
            uint64_t fingerprint = compute_audio_fingerprint(samples.data(), samples.size());

            // Match against known tracks
            CopyrightMatch match;
            bool found = match_fingerprint(fingerprint, name, match);

            if (found && match.confidence > 0.7f) {
                if (!cs.is_copyrighted || cs.current_match.isrc != match.isrc) {
                    // New copyrighted track detected
                    cs.is_copyrighted = true;
                    cs.current_match = match;
                    cs.match_start = std::chrono::steady_clock::now();
                    cs.warnings_sent = 0;
                    cs.dj_acknowledged = false;
                    g_copyright_detections++;

                    // Notify DJ
                    notify_copyright_detected(name, group, match);
                }
            } else if (cs.is_copyrighted && !found) {
                // Song ended — finalize royalty entry
                finalize_royalty(name, group, cs);
                cs.is_copyrighted = false;
            }
        }
    }
}

// Handle COPYRIGHT_ACK from DJ
static void handle_copyright_ack(const sockaddr_in& from) {
    std::lock_guard<std::shared_mutex> lock(g_mutex);
    std::string gname = find_member_group(from);
    if (gname.empty()) return;
    auto it = g_groups.find(gname);
    if (it == g_groups.end() || !it->second.copyright) return;
    it->second.copyright->dj_acknowledged = true;
    fprintf(stderr, "[copyright] DJ acknowledged copyright in group '%s'\n", gname.c_str());
}

// Handle COPYRIGHT_SKIP from DJ
static void handle_copyright_skip(const sockaddr_in& from) {
    std::lock_guard<std::shared_mutex> lock(g_mutex);
    std::string gname = find_member_group(from);
    if (gname.empty()) return;
    auto it = g_groups.find(gname);
    if (it == g_groups.end() || !it->second.copyright) return;
    auto& cs = *it->second.copyright;
    if (cs.is_copyrighted) {
        fprintf(stderr, "[copyright] DJ skipping copyrighted track '%s' in group '%s'\n",
                cs.current_match.track_title.c_str(), gname.c_str());
        // Finalize with short duration (DJ chose to skip)
        finalize_royalty(gname, it->second, cs);
        cs.is_copyrighted = false;
    }
}

// ── Wallet & billing handlers ────────────────────────────────────────────────

// Helper: find device_name for an address (requires g_mutex held)
static std::string find_device_name(const sockaddr_in& addr) {
    for (const auto& [name, group] : g_groups) {
        for (const auto& m : group.members) {
            if (addr_equal(m.addr, addr)) {
                return m.device_name;
            }
        }
    }
    return "";
}

// WALLET\n — Query own wallet balance
static void handle_wallet(const sockaddr_in& from) {
    std::string device_id;
    {
        std::lock_guard<std::shared_mutex> lock(g_mutex);
        device_id = find_device_name(from);
    }
    if (device_id.empty()) {
        const char* err = "ERR:not_in_group\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    std::lock_guard<std::mutex> wlock(g_wallet_mutex);
    auto& w = g_wallets[device_id];
    if (w.device_id.empty()) {
        w.device_id = device_id;
        w.last_activity = std::chrono::steady_clock::now();
    }

    char reply[512];
    snprintf(reply, sizeof(reply),
             "WALLET:{\"balance\":%.2f,\"total_charged\":%.2f,"
             "\"total_earned\":%.2f,\"total_royalties\":%.2f}\n",
             w.balance, w.total_charged, w.total_earned, w.total_royalties_paid);
    sendto(g_udp_sock, reply, strlen(reply), 0, (const sockaddr*)&from, sizeof(from));
}

// CHARGE:<amount_usd>:<payment_token>\n — Add funds to wallet
static void handle_charge(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 7, len - 7);  // skip "CHARGE:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    // Parse amount and token
    size_t colon = payload.find(':');
    if (colon == std::string::npos) {
        const char* err = "ERR:invalid_format\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }
    double amount = std::atof(payload.substr(0, colon).c_str());
    std::string token = payload.substr(colon + 1);

    if (amount <= 0.0 || amount > 10000.0) {
        const char* err = "ERR:invalid_amount\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    if (token.empty()) {
        const char* err = "ERR:payment_failed\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    std::string device_id;
    {
        std::lock_guard<std::shared_mutex> lock(g_mutex);
        device_id = find_device_name(from);
    }
    if (device_id.empty()) {
        const char* err = "ERR:not_in_group\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    // HMAC-SHA256 verification for CHARGE command.
    // Token format: <timestamp>:<hmac_hex>
    // HMAC = HMAC-SHA256(g_charge_secret, "CHARGE:<amount>:<device_id>:<timestamp>")
    // Replay protection: timestamp must be within 300 seconds of server time.
    if (g_charge_secret.empty()) {
        const char* err = "ERR:charge_disabled\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        fprintf(stderr, "[wallet] CHARGE rejected: RELAY_CHARGE_SECRET not configured\n");
        return;
    }
    {
        size_t sep = token.find(':');
        if (sep == std::string::npos) {
            const char* err = "ERR:invalid_token\n";
            sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
            return;
        }
        std::string ts_str = token.substr(0, sep);
        std::string client_hmac = token.substr(sep + 1);
        int64_t ts = std::atoll(ts_str.c_str());
        int64_t now = now_unix();
        if (std::abs(now - ts) > 300) {
            const char* err = "ERR:token_expired\n";
            sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
            fprintf(stderr, "[wallet] CHARGE rejected: token expired (drift=%llds) from %s\n",
                    (long long)(now - ts), addr_str(from).c_str());
            return;
        }
        // Compute expected HMAC
        std::string msg = "CHARGE:" + std::to_string(amount) + ":" + device_id + ":" + ts_str;
        std::string expected = hmac_sha256_hex(g_charge_secret, msg);
        if (client_hmac != expected) {
            const char* err = "ERR:invalid_token\n";
            sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
            fprintf(stderr, "[wallet] CHARGE rejected: HMAC mismatch from %s\n",
                    addr_str(from).c_str());
            return;
        }
    }
    {
        std::lock_guard<std::mutex> wlock(g_wallet_mutex);
        auto& w = g_wallets[device_id];
        if (w.device_id.empty()) w.device_id = device_id;
        w.balance += amount;
        w.total_charged += amount;
        w.last_activity = std::chrono::steady_clock::now();
        g_total_charges_cents += (uint64_t)(amount * 100);

        log_transaction("external", device_id, amount, "charge",
                         "Payment token: " + token.substr(0, 8) + "...");

        char reply[256];
        snprintf(reply, sizeof(reply),
                 "OK:charged:{\"balance\":%.2f,\"amount\":%.2f}\n",
                 w.balance, amount);
        sendto(g_udp_sock, reply, strlen(reply), 0, (const sockaddr*)&from, sizeof(from));
    }

    fprintf(stderr, "[wallet] Charged $%.2f to '%s'\n", amount, device_id.c_str());
}

// WITHDRAW:<amount_usd>[:<session_token>]\n — Withdraw funds
static void handle_withdraw(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 9, len - 9);  // skip "WITHDRAW:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    // Parse amount and optional session token
    std::string amount_str = payload, session_tok;
    size_t tok_colon = payload.find(':');
    if (tok_colon != std::string::npos) {
        amount_str = payload.substr(0, tok_colon);
        session_tok = payload.substr(tok_colon + 1);
    }

    // Validate session token if provided
    if (!session_tok.empty()) {
        std::lock_guard<std::shared_mutex> lock(g_mutex);
        if (!validate_session_token(from, session_tok)) {
            const char* err = "ERR:invalid_session\n";
            sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
            return;
        }
    }

    double amount = std::atof(amount_str.c_str());
    if (amount <= 0.0) {
        const char* err = "ERR:invalid_amount\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    std::string device_id;
    {
        std::lock_guard<std::shared_mutex> lock(g_mutex);
        device_id = find_device_name(from);
    }
    if (device_id.empty()) {
        const char* err = "ERR:not_in_group\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    std::lock_guard<std::mutex> wlock(g_wallet_mutex);

    // Check payout account exists
    auto pa_it = g_payouts.find(device_id);
    if (pa_it == g_payouts.end()) {
        const char* err = "ERR:no_payout_account\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    auto& w = g_wallets[device_id];
    if (w.balance < amount) {
        const char* err = "ERR:insufficient_balance\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    w.balance -= amount;
    w.last_activity = std::chrono::steady_clock::now();
    pa_it->second.total_withdrawn += amount;
    g_total_withdrawals_cents += (uint64_t)(amount * 100);

    log_transaction(device_id, "external:" + pa_it->second.payout_method, amount,
                     "withdraw", "Withdrawal to " + pa_it->second.payout_method);

    char reply[256];
    snprintf(reply, sizeof(reply),
             "OK:withdrawn:{\"amount\":%.2f,\"remaining\":%.2f}\n",
             amount, w.balance);
    sendto(g_udp_sock, reply, strlen(reply), 0, (const sockaddr*)&from, sizeof(from));

    fprintf(stderr, "[wallet] Withdrew $%.2f from '%s' via %s\n",
            amount, device_id.c_str(), pa_it->second.payout_method.c_str());
}

// PAYOUT_SETUP:<method>:<id>\n — Setup payout account
static void handle_payout_setup(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 13, len - 13);  // skip "PAYOUT_SETUP:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    size_t colon = payload.find(':');
    if (colon == std::string::npos) {
        const char* err = "ERR:invalid_format\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    std::string method = payload.substr(0, colon);
    std::string payout_id = payload.substr(colon + 1);

    if (method != "stripe" && method != "paypal" && method != "bank") {
        const char* err = "ERR:invalid_payout_method\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }
    if (payout_id.empty()) {
        const char* err = "ERR:invalid_payout_id\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    std::string device_id;
    {
        std::lock_guard<std::shared_mutex> lock(g_mutex);
        device_id = find_device_name(from);
    }
    if (device_id.empty()) {
        const char* err = "ERR:not_in_group\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    {
        std::lock_guard<std::mutex> wlock(g_wallet_mutex);
        PayoutAccount& pa = g_payouts[device_id];
        pa.device_id = device_id;
        pa.payout_method = method;
        pa.payout_id = payout_id;
    }

    const char* reply = "OK:payout_setup\n";
    sendto(g_udp_sock, reply, strlen(reply), 0, (const sockaddr*)&from, sizeof(from));

    fprintf(stderr, "[wallet] Payout setup for '%s': %s (%s)\n",
            device_id.c_str(), method.c_str(), payout_id.c_str());
}

// TIP:<amount_usd>[:<session_token>]\n — Listener tips the DJ of current group
static void handle_tip(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 4, len - 4);  // skip "TIP:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    // Parse amount and optional session token
    std::string amount_str = payload, session_tok;
    size_t tok_colon = payload.find(':');
    if (tok_colon != std::string::npos) {
        amount_str = payload.substr(0, tok_colon);
        session_tok = payload.substr(tok_colon + 1);
    }
    if (!session_tok.empty()) {
        std::lock_guard<std::shared_mutex> lock(g_mutex);
        if (!validate_session_token(from, session_tok)) {
            const char* err = "ERR:invalid_session\n";
            sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
            return;
        }
    }

    double amount = std::atof(amount_str.c_str());
    if (amount <= 0.0 || amount > 1000.0) {
        const char* err = "ERR:invalid_amount\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    std::string tipper_device;
    std::string dj_device;
    sockaddr_in dj_addr{};
    bool found_dj = false;

    {
        std::lock_guard<std::shared_mutex> lock(g_mutex);
        std::string gname = find_member_group(from);
        if (gname.empty()) {
            const char* err = "ERR:not_in_group\n";
            sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
            return;
        }
        auto& group = g_groups[gname];
        for (const auto& m : group.members) {
            if (addr_equal(m.addr, from)) {
                tipper_device = m.device_name;
            }
            if (m.role >= MemberRole::DJ && !addr_equal(m.addr, from)) {
                dj_device = m.device_name;
                dj_addr = m.addr;
                found_dj = true;
            }
        }
    }

    if (tipper_device.empty()) {
        const char* err = "ERR:not_in_group\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }
    if (!found_dj || dj_device.empty()) {
        const char* err = "ERR:no_dj_in_group\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    {
        std::lock_guard<std::mutex> wlock(g_wallet_mutex);
        auto& tipper_wallet = g_wallets[tipper_device];
        if (tipper_wallet.device_id.empty()) {
            tipper_wallet.device_id = tipper_device;
            tipper_wallet.last_activity = std::chrono::steady_clock::now();
        }

        if (tipper_wallet.balance < amount) {
            const char* err = "ERR:insufficient_balance\n";
            sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
            return;
        }

        // Deduct from tipper, credit to DJ (100% to DJ, no platform cut)
        tipper_wallet.balance -= amount;
        tipper_wallet.last_activity = std::chrono::steady_clock::now();

        auto& dj_wallet = g_wallets[dj_device];
        if (dj_wallet.device_id.empty()) dj_wallet.device_id = dj_device;
        dj_wallet.balance += amount;
        dj_wallet.total_earned += amount;
        dj_wallet.last_activity = std::chrono::steady_clock::now();

        g_total_tips_cents += (uint64_t)(amount * 100);

        log_transaction(tipper_device, dj_device, amount, "tip", "Listener tip");

        // Notify tipper
        char reply[256];
        snprintf(reply, sizeof(reply),
                 "OK:tipped:{\"amount\":%.2f,\"to\":\"%s\"}\n",
                 amount, json_escape(dj_device).c_str());
        sendto(g_udp_sock, reply, strlen(reply), 0, (const sockaddr*)&from, sizeof(from));

        // Notify DJ
        char dj_notify[256];
        snprintf(dj_notify, sizeof(dj_notify),
                 "TIP_RECEIVED:{\"amount\":%.2f,\"from\":\"%s\"}\n",
                 amount, json_escape(tipper_device).c_str());
        sendto(g_udp_sock, dj_notify, strlen(dj_notify), 0,
               (const sockaddr*)&dj_addr, sizeof(dj_addr));
    }

    fprintf(stderr, "[wallet] Tip $%.2f from '%s' to '%s'\n",
            amount, tipper_device.c_str(), dj_device.c_str());
}

// ── SUPPORT handler ──────────────────────────────────────────────────────────
// Format: "SUPPORT:<amount_usd>\n"
// Listener adds money to the DJ's wallet to help cover royalty costs.
// Different from TIP: SUPPORT is specifically for royalty support (shown differently in UI),
// and is broadcast to all members so everyone sees the support.

static void handle_support(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 8, len - 8);  // skip "SUPPORT:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    // Parse amount and optional session token
    std::string amount_str = payload, session_tok;
    size_t tok_colon = payload.find(':');
    if (tok_colon != std::string::npos) {
        amount_str = payload.substr(0, tok_colon);
        session_tok = payload.substr(tok_colon + 1);
    }
    if (!session_tok.empty()) {
        std::lock_guard<std::shared_mutex> lock(g_mutex);
        if (!validate_session_token(from, session_tok)) {
            const char* err = "ERR:invalid_session\n";
            sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
            return;
        }
    }

    double amount = std::atof(amount_str.c_str());
    if (amount <= 0.0 || amount > 10000.0) {
        const char* err = "ERR:invalid_amount\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    std::string supporter_device;
    std::string dj_device;
    std::string group_name;
    std::vector<sockaddr_in> all_addrs;

    {
        std::lock_guard<std::shared_mutex> lock(g_mutex);
        group_name = find_member_group(from);
        if (group_name.empty()) {
            const char* err = "ERR:not_in_group\n";
            sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
            return;
        }
        auto& group = g_groups[group_name];
        for (const auto& m : group.members) {
            all_addrs.push_back(m.addr);
            if (addr_equal(m.addr, from)) supporter_device = m.device_name;
            if (m.role >= MemberRole::DJ && !addr_equal(m.addr, from)) dj_device = m.device_name;
        }
    }

    if (supporter_device.empty() || dj_device.empty()) {
        const char* err = "ERR:no_dj_in_group\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    {
        std::lock_guard<std::mutex> wlock(g_wallet_mutex);
        auto& sup_wallet = g_wallets[supporter_device];
        if (sup_wallet.device_id.empty()) {
            sup_wallet.device_id = supporter_device;
            sup_wallet.last_activity = std::chrono::steady_clock::now();
        }
        if (sup_wallet.balance < amount) {
            const char* err = "ERR:insufficient_balance\n";
            sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
            return;
        }

        // Transfer from supporter to DJ wallet
        sup_wallet.balance -= amount;
        sup_wallet.last_activity = std::chrono::steady_clock::now();

        auto& dj_wallet = g_wallets[dj_device];
        if (dj_wallet.device_id.empty()) dj_wallet.device_id = dj_device;
        dj_wallet.balance += amount;
        dj_wallet.total_earned += amount;
        dj_wallet.last_activity = std::chrono::steady_clock::now();

        log_transaction(supporter_device, dj_device, amount, "support",
                        "Royalty support for " + group_name);

        // Confirm to supporter
        char reply[256];
        snprintf(reply, sizeof(reply),
                 "OK:supported:{\"amount\":%.2f,\"to\":\"%s\",\"balance\":%.2f}\n",
                 amount, json_escape(dj_device).c_str(), sup_wallet.balance);
        sendto(g_udp_sock, reply, strlen(reply), 0, (const sockaddr*)&from, sizeof(from));

        // Broadcast SUPPORT_RECEIVED to ALL members (everyone sees the support)
        char notify[512];
        snprintf(notify, sizeof(notify),
                 "SUPPORT_RECEIVED:{\"amount\":%.2f,\"from\":\"%s\",\"dj\":\"%s\","
                 "\"dj_balance\":%.2f}\n",
                 amount, json_escape(supporter_device).c_str(),
                 json_escape(dj_device).c_str(), dj_wallet.balance);
        for (const auto& addr : all_addrs) {
            sendto(g_udp_sock, notify, strlen(notify), 0,
                   (const sockaddr*)&addr, sizeof(addr));
        }
    }

    fprintf(stderr, "[wallet] Support $%.2f from '%s' to DJ '%s' in '%s'\n",
            amount, supporter_device.c_str(), dj_device.c_str(), group_name.c_str());
}

// ── LICENSED_PLAY handler ────────────────────────────────────────────────────
// Format: "LICENSED_PLAY:<json>\n"
// DJ declares they have a license for the track. Skips copyright detection.
// JSON: {"track":"...","artist":"...","isrc":"...","license":"jasrac|cc|original|blanket"}
// The relay trusts the declaration (audit trail logged), but doesn't charge royalties.
// This enables one-click play of properly licensed content without detection overhead.

static void handle_licensed_play(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 14, len - 14);  // skip "LICENSED_PLAY:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    std::lock_guard<std::shared_mutex> lock(g_mutex);

    std::string group_name = find_member_group(from);
    if (group_name.empty()) {
        const char* err = "ERR:not_in_group\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    auto& group = g_groups[group_name];
    Member* sender = find_member(group, from);
    if (!sender || sender->role < MemberRole::DJ) {
        const char* err = "ERR:permission_denied\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    // Mark this group as having a licensed track — suppress copyright detection
    if (group.copyright) {
        group.copyright->is_copyrighted = false;  // clear any active detection
        group.copyright->dj_acknowledged = true;   // suppress further detection
    }

    // Broadcast COPYRIGHT_INFO to all members (proper attribution without warning)
    std::string info_msg = "COPYRIGHT_INFO:" + payload + "\n";
    for (const auto& m : group.members) {
        sendto(g_udp_sock, info_msg.c_str(), info_msg.size(), 0,
               (const sockaddr*)&m.addr, sizeof(m.addr));
    }

    // Log for audit trail (even licensed plays are tracked for reporting)
    fprintf(stderr, "[licensed] DJ '%s' declared licensed play in '%s': %s\n",
            sender->device_name.c_str(), group_name.c_str(), payload.c_str());

    // Append to royalty log as a licensed play (royalty_amount = 0)
    FILE* f = fopen(g_royalty_log_path.c_str(), "a");
    if (f) {
        auto ts = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        fprintf(f, "{\"ts\":%lld,\"group\":\"%s\",\"dj\":\"%s\",\"type\":\"licensed_play\","
                   "\"data\":%s,\"royalty_usd\":0}\n",
                (long long)ts, group_name.c_str(), sender->device_name.c_str(),
                payload.c_str());
        fclose(f);
    }

    const char* ok = "OK:licensed_play\n";
    sendto(g_udp_sock, ok, strlen(ok), 0, (const sockaddr*)&from, sizeof(from));
}

// RIGHTS_BALANCE[:<rights_holder_name>]\n — Query rights holder balance(s)
// If name specified, returns balance for that rights holder.
// If no name, returns all rights holder balances (admin/DJ use).
static void handle_rights_balance(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload;
    if (len > 15 && msg[14] == ':') {
        payload.assign(msg + 15, len - 15);  // skip "RIGHTS_BALANCE:"
        while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
            payload.pop_back();
    }

    std::lock_guard<std::mutex> wlock(g_wallet_mutex);

    if (!payload.empty()) {
        // Specific rights holder query
        auto it = g_rights_holder_balances.find(payload);
        double bal = (it != g_rights_holder_balances.end()) ? it->second : 0.0;
        char reply[256];
        snprintf(reply, sizeof(reply),
                 "RIGHTS_BALANCE:{\"holder\":\"%s\",\"balance\":%.4f}\n",
                 payload.c_str(), bal);
        sendto(g_udp_sock, reply, strlen(reply), 0, (const sockaddr*)&from, sizeof(from));
    } else {
        // All rights holders — build JSON array
        std::string json = "RIGHTS_BALANCE:{\"holders\":[";
        bool first = true;
        for (const auto& [holder, bal] : g_rights_holder_balances) {
            if (!first) json += ",";
            first = false;
            json += "{\"name\":\"" + holder + "\",\"balance\":" +
                    std::to_string(bal) + "}";
        }
        json += "],\"total\":" + std::to_string(g_rights_holder_balances.size()) + "}\n";
        sendto(g_udp_sock, json.c_str(), json.size(), 0, (const sockaddr*)&from, sizeof(from));
    }
}

// TRANSACTIONS:<count>\n — Get last N transactions for the requesting device
static void handle_transactions(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 13, len - 13);  // skip "TRANSACTIONS:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    int count = std::atoi(payload.c_str());
    if (count <= 0) count = 10;
    if (count > 100) count = 100;

    std::string device_id;
    {
        std::lock_guard<std::shared_mutex> lock(g_mutex);
        device_id = find_device_name(from);
    }
    if (device_id.empty()) {
        const char* err = "ERR:not_in_group\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    // Collect transactions involving this device
    std::string result = "TRANSACTIONS:[";
    bool first = true;
    int found = 0;

    {
        std::lock_guard<std::mutex> wlock(g_wallet_mutex);
        // Iterate backwards for most recent first
        for (int i = (int)g_transactions.size() - 1; i >= 0 && found < count; i--) {
            const auto& tx = g_transactions[i];
            if (tx.from_device == device_id || tx.to_device == device_id) {
                if (!first) result += ",";
                first = false;

                char entry[512];
                snprintf(entry, sizeof(entry),
                         "{\"ts\":%lld,\"tx_id\":\"%s\",\"from\":\"%s\",\"to\":\"%s\","
                         "\"amount\":%.4f,\"type\":\"%s\",\"desc\":\"%s\"}",
                         (long long)tx.timestamp,
                         json_escape(tx.tx_id).c_str(),
                         json_escape(tx.from_device).c_str(),
                         json_escape(tx.to_device).c_str(),
                         tx.amount,
                         json_escape(tx.type).c_str(),
                         json_escape(tx.description).c_str());
                result += entry;
                found++;
            }
        }
    }

    result += "]\n";
    sendto(g_udp_sock, result.c_str(), result.size(), 0,
           (const sockaddr*)&from, sizeof(from));
}

// ── Audio forwarding ──────────────────────────────────────────────────────────
// Forward an audio packet to all group members except the sender.

// Forward declaration (defined later, near HTTP server)
static void ws_broadcast_audio(const std::string& channel, const uint8_t* data, size_t len);

static void forward_audio(const uint8_t* data, size_t len, const sockaddr_in& from) {
    std::string group_name;
    std::vector<sockaddr_in> local_dests;
    size_t swarm_saved = 0;
    bool is_swarm = false;

    // Hold g_mutex for the lookup + destination collection phase.
    // Destinations are copied out so sendto() happens outside the lock.
    {
        std::lock_guard<std::shared_mutex> lock(g_mutex);

        // Use reverse lookup to find group quickly
        Group* group_ptr = nullptr;
        auto rk = g_addr_to_group.find(addr_key(from));
        if (rk != g_addr_to_group.end()) {
            auto it = g_groups.find(rk->second);
            if (it != g_groups.end()) {
                group_name = rk->second;
                group_ptr = &it->second;
            }
        }
        // Fallback: linear scan if reverse lookup misses
        if (!group_ptr) {
            for (auto& [name, group] : g_groups) {
                if (find_member(group, from)) {
                    group_name = name;
                    group_ptr = &group;
                    break;
                }
            }
        }
        if (!group_ptr) return;

        auto& group = *group_ptr;
        Member* sender = find_member(group, from);
        if (!sender) return;

        // Permission check: only DJ/Owner can send audio on owned channels
        if (!is_random_channel(group_name) && !is_free_name(group_name) &&
            sender->role < MemberRole::DJ) {
            return;  // silently drop
        }

        // Build destination list under lock
        local_dests.reserve(group.members.size());

        if (group.swarm_active && !group.swarm_tree.empty()) {
            is_swarm = true;
            for (const auto& sn : group.swarm_tree) {
                if (addr_equal(sn.addr, from)) continue;
                if (sn.depth == 0) local_dests.push_back(sn.addr);
            }
            size_t full_count = group.members.size() - 1;
            swarm_saved = (full_count > local_dests.size()) ? full_count - local_dests.size() : 0;
        } else {
            for (const auto& m : group.members) {
                if (addr_equal(m.addr, from)) continue;
                local_dests.push_back(m.addr);
            }
        }

        size_t fwd_count = local_dests.size();
        g_total_packets_fwd += fwd_count;
        g_total_bytes_fwd += len * fwd_count;
        group.packets_forwarded++;
        group.bytes_forwarded += len * fwd_count;

        // Store in replay buffer
        ReplayPacket rp;
        rp.data.assign(data, data + len);
        rp.timestamp = std::chrono::steady_clock::now();
        if (group.replay_buffer.size() < kMaxReplayPackets) {
            group.replay_buffer.push_back(std::move(rp));
        } else {
            group.replay_buffer[group.replay_write_pos] = std::move(rp);
        }
        group.replay_write_pos = (group.replay_write_pos + 1) % kMaxReplayPackets;

        // Record if active
        if (group.record_file) {
            uint32_t pkt_len = (uint32_t)len;
            fwrite(&pkt_len, 4, 1, group.record_file);
            fwrite(data, 1, len, group.record_file);
        }

        // Copy samples to fingerprint buffer (non-blocking, fire-and-forget)
        if (g_copyright_enabled && len > 28 && group.mode == ChannelMode::Public) {
            if (!group.copyright) {
                group.copyright = std::make_unique<GroupCopyrightState>();
            }
            auto& fp = group.copyright->fp_buf;
            if (fp.mtx.try_lock()) {
                const uint8_t* payload = data + 24;
                size_t payload_len = len - 24 - 4;
                size_t num_frames = payload_len / 8;
                for (size_t i = 0; i < num_frames && i < 480; i++) {
                    int32_t left, right;
                    memcpy(&left, payload + i * 8, 4);
                    memcpy(&right, payload + i * 8 + 4, 4);
                    int32_t mono = (left + right) / 2;
                    int16_t sample = (int16_t)(mono >> 8);
                    fp.ring[fp.write_pos] = sample;
                    fp.write_pos = (fp.write_pos + 1) % kFingerprintRingSize;
                    fp.total_written++;
                }
                fp.active = true;
                fp.group_name = group_name;
                fp.mtx.unlock();
            }
        }
    }  // g_mutex released here

    // Track swarm savings (atomic, outside lock)
    if (is_swarm) {
        g_swarm_packets_saved.fetch_add(swarm_saved, std::memory_order_relaxed);
        g_swarm_bytes_saved.fetch_add(swarm_saved * len, std::memory_order_relaxed);
    }

    // Cascade fan-out: also forward to downstream relay peers
    if (g_tier == RelayTier::Origin || g_tier == RelayTier::Region) {
        std::lock_guard<std::mutex> dlock(g_downstream_mutex);
        for (const auto& dp : g_downstream_peers) {
            if (dp.groups.count(group_name)) {
                local_dests.push_back(dp.addr);
            }
        }
    }

    // Enqueue to worker threads — sendto() happens outside g_mutex
    enqueue_forward(data, len, std::move(local_dests));

    // Push audio to WebSocket browser listeners (non-blocking)
    ws_broadcast_audio(group_name, data, len);
}

// ── Stale member cleanup ──────────────────────────────────────────────────────

static void cleanup_stale() {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::shared_mutex> lock(g_mutex);

    for (auto it = g_groups.begin(); it != g_groups.end(); ) {
        auto& members = it->second.members;
        size_t before = members.size();

        // Collect stale members first (for swarm removal)
        std::vector<sockaddr_in> stale_addrs;
        for (const auto& m : members) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - m.last_seen).count();
            if (elapsed > kStaleTimeoutSec) {
                stale_addrs.push_back(m.addr);
            }
        }

        // Remove each stale member from swarm tree before erasing from members
        for (const auto& sa : stale_addrs) {
            fprintf(stderr, "[relay] %s timed out from group '%s'\n",
                    addr_str(sa).c_str(), it->first.c_str());
            g_addr_to_group.erase(addr_key(sa));
            swarm_remove_member(it->second, sa);
        }

        members.erase(
            std::remove_if(members.begin(), members.end(),
                [&](const Member& m) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        now - m.last_seen).count();
                    return elapsed > kStaleTimeoutSec;
                }),
            members.end());

        if (members.empty() && before > 0) {
            // If region/edge, send CASCADE_LEAVE upstream
            if ((g_tier == RelayTier::Region || g_tier == RelayTier::Edge) && g_upstream_connected) {
                std::string cascade_msg = "CASCADE_LEAVE:" + it->first + ":" + g_my_peer_id + "\n";
                sendto(g_udp_sock, cascade_msg.c_str(), cascade_msg.size(), 0,
                       (const sockaddr*)&g_upstream_addr, sizeof(g_upstream_addr));
            }
            // Close any open recording file
            if (it->second.record_file) {
                fclose(it->second.record_file);
                it->second.record_file = nullptr;
                fprintf(stderr, "[relay] Recording closed for dissolved group '%s' → %s\n",
                        it->first.c_str(), it->second.record_path.c_str());
            }
            fprintf(stderr, "[relay] Group '%s' dissolved (all members timed out)\n",
                    it->first.c_str());
            it = g_groups.erase(it);
        } else {
            ++it;
        }
    }

    // Clean up expired channel registrations
    int64_t ts = now_unix();
    bool channels_changed = false;
    for (auto it = g_channels.begin(); it != g_channels.end(); ) {
        if (it->second.expires <= ts) {
            fprintf(stderr, "[relay] Channel '%s' expired, removing\n", it->first.c_str());
            it = g_channels.erase(it);
            channels_changed = true;
        } else {
            ++it;
        }
    }
    if (channels_changed) channels_save();
}

// ── Stats output ──────────────────────────────────────────────────────────────

static void print_stats() {
    std::lock_guard<std::shared_mutex> lock(g_mutex);

    size_t total_members = 0;
    for (const auto& [name, group] : g_groups) {
        total_members += group.members.size();
    }

    fprintf(stderr,
        "\n[relay] ─── Stats ───────────────────────────────────\n"
        "[relay] Groups: %zu  Members: %zu  Channels: %zu  Total joins: %llu\n"
        "[relay] Packets RX: %llu  Forwarded: %llu\n"
        "[relay] Bytes RX: %llu  Forwarded: %llu\n",
        g_groups.size(), total_members, g_channels.size(),
        (unsigned long long)g_total_joins,
        (unsigned long long)g_total_packets_rx,
        (unsigned long long)g_total_packets_fwd,
        (unsigned long long)g_total_bytes_rx,
        (unsigned long long)g_total_bytes_fwd);

    // P2P Swarm stats
    size_t swarm_active_groups = 0;
    int global_max_depth = 0;
    for (const auto& [name, group] : g_groups) {
        int group_max_depth = 0;
        fprintf(stderr, "[relay]   Group '%s': %zu members, %llu pkts fwd",
                name.c_str(), group.members.size(),
                (unsigned long long)group.packets_forwarded);
        if (group.swarm_active) {
            swarm_active_groups++;
            for (const auto& sn : group.swarm_tree) {
                if (sn.depth > group_max_depth) group_max_depth = sn.depth;
            }
            if (group_max_depth > global_max_depth) global_max_depth = group_max_depth;
            size_t root_count = 0;
            for (const auto& sn : group.swarm_tree) {
                if (sn.depth == 0) root_count++;
            }
            fprintf(stderr, " [SWARM: depth=%d, roots=%zu]", group_max_depth, root_count);
        }
        fprintf(stderr, "\n");
        for (const auto& m : group.members) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - m.last_seen).count();
            fprintf(stderr, "[relay]     %s  last_seen=%llds ago%s\n",
                    addr_str(m.addr).c_str(), (long long)age,
                    m.device_name.empty() ? "" : (" [" + m.device_name + "]").c_str());
        }
    }

    if (swarm_active_groups > 0 || g_swarm_bytes_saved.load(std::memory_order_relaxed) > 0) {
        fprintf(stderr,
            "[relay] Swarm: %zu active groups, max depth %d\n"
            "[relay] Swarm P2P savings: %llu packets, %llu bytes not sent by relay\n",
            swarm_active_groups, global_max_depth,
            (unsigned long long)g_swarm_packets_saved.load(std::memory_order_relaxed),
            (unsigned long long)g_swarm_bytes_saved.load(std::memory_order_relaxed));
    }

    // P2P File distribution stats
    {
        size_t total_files = 0;
        uint64_t total_file_size = 0;
        size_t total_complete = 0;
        size_t total_peers = 0;
        for (const auto& [name, group] : g_groups) {
            total_files += group.file_manifests.size();
            for (const auto& [sha, fm] : group.file_manifests) {
                total_file_size += fm.size_bytes;
                total_complete += fm.complete_peers.size();
                total_peers += group.members.size();
            }
        }
        if (total_files > 0 || g_file_offers.load(std::memory_order_relaxed) > 0) {
            // Estimate bandwidth saved: complete_peers * file_size = data that would have been streamed
            uint64_t bandwidth_saved = 0;
            for (const auto& [name, group] : g_groups) {
                for (const auto& [sha, fm] : group.file_manifests) {
                    // Each complete peer would have needed size_bytes streamed
                    // Instead, they got it P2P (or via URL)
                    if (fm.complete_peers.size() > 1) {
                        bandwidth_saved += fm.size_bytes * (fm.complete_peers.size() - 1);
                    }
                }
            }
            fprintf(stderr,
                "[relay] P2P Files: %zu active, %llu bytes total, %zu/%zu seeders\n"
                "[relay] P2P File offers: %llu (chunk) + %llu (URL), completions: %llu\n"
                "[relay] P2P bandwidth saved: %llu bytes\n",
                total_files, (unsigned long long)total_file_size,
                total_complete, total_peers,
                (unsigned long long)g_file_offers.load(std::memory_order_relaxed),
                (unsigned long long)g_file_url_offers.load(std::memory_order_relaxed),
                (unsigned long long)g_file_completions.load(std::memory_order_relaxed),
                (unsigned long long)bandwidth_saved);
        }
    }

    // Cascade stats
    if (g_tier != RelayTier::Standalone) {
        const char* tier_str = g_tier == RelayTier::Origin ? "ORIGIN" :
                               g_tier == RelayTier::Region ? "REGION" :
                               g_tier == RelayTier::Edge   ? "EDGE" : "STANDALONE";
        fprintf(stderr, "[relay] Tier: %s  Peer: %s\n", tier_str, g_my_peer_id.c_str());

        if (g_tier == RelayTier::Origin || g_tier == RelayTier::Region) {
            std::lock_guard<std::mutex> dlock(g_downstream_mutex);
            uint64_t total_downstream_listeners = 0;
            fprintf(stderr, "[relay] Downstream relays: %zu\n", g_downstream_peers.size());
            for (const auto& dp : g_downstream_peers) {
                total_downstream_listeners += dp.listener_count;
                fprintf(stderr, "[relay]   Peer %s (%s): %llu listeners, %zu groups\n",
                        dp.peer_id.c_str(), addr_str(dp.addr).c_str(),
                        (unsigned long long)dp.listener_count, dp.groups.size());
            }
            fprintf(stderr, "[relay] Total propagated listeners: %llu\n",
                    (unsigned long long)(total_members + total_downstream_listeners));
        }
        if (g_tier == RelayTier::Region || g_tier == RelayTier::Edge) {
            fprintf(stderr, "[relay] Upstream: %s (connected=%s)\n",
                    addr_str(g_upstream_addr).c_str(),
                    g_upstream_connected ? "yes" : "no");
        }
        fprintf(stderr, "[relay] Worker tasks completed: %llu\n",
                (unsigned long long)g_worker_tasks_done.load(std::memory_order_relaxed));
    }

    // Copyright detection & royalty stats
    if (g_copyright_enabled) {
        size_t active_copyrighted = 0;
        for (const auto& [name, group] : g_groups) {
            if (group.copyright && group.copyright->is_copyrighted) {
                active_copyrighted++;
            }
        }
        size_t ledger_size;
        {
            std::lock_guard<std::mutex> rlock(g_royalty_mutex);
            ledger_size = g_royalty_ledger.size();
        }
        fprintf(stderr,
            "[relay] Copyright: %llu detections, %zu active streams\n"
            "[relay] Royalties: %zu entries, $%.2f total logged\n",
            (unsigned long long)g_copyright_detections.load(std::memory_order_relaxed),
            active_copyrighted,
            ledger_size,
            (double)g_total_royalty_cents.load(std::memory_order_relaxed) / 100.0);
    }

    // Wallet & billing stats
    {
        std::lock_guard<std::mutex> wlock(g_wallet_mutex);
        double total_balance = 0.0;
        for (const auto& [id, w] : g_wallets) {
            total_balance += w.balance;
        }
        double total_rh_pending = 0.0;
        for (const auto& [holder, bal] : g_rights_holder_balances) {
            total_rh_pending += bal;
        }
        fprintf(stderr,
            "[relay] Wallets: %zu total, $%.2f total balance\n"
            "[relay] Charges: $%.2f  Tips: $%.2f  Withdrawals: $%.2f\n"
            "[relay] Rights holders: %zu, $%.2f pending payouts\n"
            "[relay] Transactions: %zu logged\n",
            g_wallets.size(), total_balance,
            (double)g_total_charges_cents.load(std::memory_order_relaxed) / 100.0,
            (double)g_total_tips_cents.load(std::memory_order_relaxed) / 100.0,
            (double)g_total_withdrawals_cents.load(std::memory_order_relaxed) / 100.0,
            g_rights_holder_balances.size(), total_rh_pending,
            g_transactions.size());
    }

    fprintf(stderr, "[relay] ─────────────────────────────────────────────\n\n");
}

// ── META handler ──────────────────────────────────────────────────────────────
// Format: "META:<json>\n"

static void handle_meta(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 5, len - 5);  // skip "META:"
    // Strip trailing newline
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    std::lock_guard<std::shared_mutex> lock(g_mutex);
    for (auto& [name, group] : g_groups) {
        Member* sender = find_member(group, from);
        if (!sender) continue;
        if (!is_random_channel(name) && !is_free_name(name) &&
            sender->role < MemberRole::DJ) {
            const char* err = "ERR:no_permission\n";
            sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
            return;
        }

        group.last_meta = payload;

        // Forward META: to all other members
        std::string fwd = "META:" + payload + "\n";
        for (const auto& m : group.members) {
            if (addr_equal(m.addr, from)) continue;
            sendto(g_udp_sock, fwd.c_str(), fwd.size(), 0,
                   (const sockaddr*)&m.addr, sizeof(m.addr));
        }
        return;
    }
}

// ── FILE handler ──────────────────────────────────────────────────────────────
// Format: "FILE:<filename>\n" — broadcast to group, store as current_file
static void handle_file(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 5, len - 5);
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    std::lock_guard<std::shared_mutex> lock(g_mutex);
    for (auto& [name, group] : g_groups) {
        Member* sender = find_member(group, from);
        if (!sender) continue;
        if (!is_random_channel(name) && !is_free_name(name) &&
            sender->role < MemberRole::DJ) {
            const char* err = "ERR:no_permission\n";
            sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
            return;
        }

        group.current_file = payload;
        group.last_sync.clear();  // reset sync state for new file

        // Forward FILE: to all other members
        std::string fwd = "FILE:" + payload + "\n";
        for (const auto& m : group.members) {
            if (addr_equal(m.addr, from)) continue;
            sendto(g_udp_sock, fwd.c_str(), fwd.size(), 0,
                   (const sockaddr*)&m.addr, sizeof(m.addr));
        }
        fprintf(stderr, "[relay] FILE: %s in group '%s'\n", payload.c_str(), name.c_str());
        return;
    }
}

// ── SYNC handler ──────────────────────────────────────────────────────────────
// Format: "SYNC:<action>:<pos_ms>:<wall_ms>\n" — broadcast to group
static void handle_sync(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 5, len - 5);
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    std::lock_guard<std::shared_mutex> lock(g_mutex);
    for (auto& [name, group] : g_groups) {
        Member* sender = find_member(group, from);
        if (!sender) continue;
        if (!is_random_channel(name) && !is_free_name(name) &&
            sender->role < MemberRole::DJ) {
            const char* err = "ERR:no_permission\n";
            sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
            return;
        }

        group.last_sync = payload;

        // Forward SYNC: to all other members
        std::string fwd = "SYNC:" + payload + "\n";
        for (const auto& m : group.members) {
            if (addr_equal(m.addr, from)) continue;
            sendto(g_udp_sock, fwd.c_str(), fwd.size(), 0,
                   (const sockaddr*)&m.addr, sizeof(m.addr));
        }
        fprintf(stderr, "[relay] SYNC: %s in group '%s'\n", payload.c_str(), name.c_str());
        return;
    }
}

// ── READY handler ─────────────────────────────────────────────────────────────
// Format: "READY:<filename>\n" — receiver→relay, forward to all (including radio)
static void handle_ready(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 6, len - 6);
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    std::lock_guard<std::shared_mutex> lock(g_mutex);
    for (auto& [name, group] : g_groups) {
        bool sender_in_group = false;
        for (const auto& m : group.members) {
            if (addr_equal(m.addr, from)) { sender_in_group = true; break; }
        }
        if (!sender_in_group) continue;

        // Forward READY: to all members (radio needs to count ready clients)
        std::string fwd = "READY:" + payload + "\n";
        for (const auto& m : group.members) {
            if (addr_equal(m.addr, from)) continue;
            sendto(g_udp_sock, fwd.c_str(), fwd.size(), 0,
                   (const sockaddr*)&m.addr, sizeof(m.addr));
        }
        return;
    }
}

// ── REPLAY handler ────────────────────────────────────────────────────────────
// Format: "REPLAY:<offset_sec>\n"

static void handle_replay(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 7, len - 7);
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    int offset_sec = std::atoi(payload.c_str());
    if (offset_sec <= 0 || offset_sec > (int)(kMaxReplayPackets / 100)) {
        const char* err = "ERR:invalid_offset\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    std::lock_guard<std::shared_mutex> lock(g_mutex);
    auto now = std::chrono::steady_clock::now();

    for (auto& [name, group] : g_groups) {
        bool sender_in_group = false;
        for (const auto& m : group.members) {
            if (addr_equal(m.addr, from)) { sender_in_group = true; break; }
        }
        if (!sender_in_group) continue;

        auto cutoff = now - std::chrono::seconds(offset_sec);

        // Send all buffered packets from cutoff onwards to the requester
        size_t sent = 0;
        for (const auto& rp : group.replay_buffer) {
            if (rp.data.empty()) continue;
            if (rp.timestamp >= cutoff) {
                sendto(g_udp_sock, rp.data.data(), rp.data.size(), 0,
                       (const sockaddr*)&from, sizeof(from));
                sent++;
            }
        }

        char reply[64];
        snprintf(reply, sizeof(reply), "OK:replay:%zu\n", sent);
        sendto(g_udp_sock, reply, strlen(reply), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }
}

// ── RECORD handler ────────────────────────────────────────────────────────────
// Format: "RECORD:<group>\n"

static void handle_record(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 7, len - 7);
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    std::lock_guard<std::shared_mutex> lock(g_mutex);
    auto it = g_groups.find(payload);
    if (it == g_groups.end()) {
        const char* err = "ERR:no_group\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    // Verify sender is a member of the group
    bool is_member = false;
    for (const auto& m : it->second.members) {
        if (addr_equal(m.addr, from)) { is_member = true; break; }
    }
    if (!is_member) {
        const char* err = "ERR:not_member\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    if (it->second.record_file) {
        const char* err = "ERR:already_recording\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    // Create recordings directory
    mkdir("/data/recordings", 0755);

    char path[256];
    snprintf(path, sizeof(path), "/data/recordings/%s_%lld.raw",
             payload.c_str(), (long long)now_unix());
    it->second.record_file = fopen(path, "wb");
    if (!it->second.record_file) {
        const char* err = "ERR:cannot_open_file\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }
    it->second.record_path = path;
    fprintf(stderr, "[relay] Recording started for group '%s' → %s\n", payload.c_str(), path);

    const char* reply = "OK:recording\n";
    sendto(g_udp_sock, reply, strlen(reply), 0, (const sockaddr*)&from, sizeof(from));
}

// ── RECORD_STOP handler ───────────────────────────────────────────────────────
// Format: "RECORD_STOP:<group>\n"

static void handle_record_stop(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 12, len - 12);  // skip "RECORD_STOP:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    std::lock_guard<std::shared_mutex> lock(g_mutex);
    auto it = g_groups.find(payload);
    if (it != g_groups.end() && it->second.record_file) {
        fclose(it->second.record_file);
        it->second.record_file = nullptr;
        fprintf(stderr, "[relay] Recording stopped for group '%s' → %s\n",
                payload.c_str(), it->second.record_path.c_str());
        it->second.record_path.clear();
    }
    const char* reply = "OK:record_stopped\n";
    sendto(g_udp_sock, reply, strlen(reply), 0, (const sockaddr*)&from, sizeof(from));
}

// ── TEXT handler ───────────────────────────────────────────────────────────────
// Format: "TEXT:<json>\n"
// json: {"text":"...","type":"lyric|chat|info","ts":123456,"duration":3000}

static void handle_text(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 5, len - 5);  // skip "TEXT:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    // Extract "type" from JSON payload to check permissions
    std::string text_type = json_get_string(payload, "type");

    std::lock_guard<std::shared_mutex> lock(g_mutex);
    for (auto& [name, group] : g_groups) {
        Member* sender = find_member(group, from);
        if (!sender) continue;

        // "lyric" type requires DJ/Owner role (same as META/FILE/SYNC)
        if (text_type == "lyric") {
            if (!is_random_channel(name) && !is_free_name(name) &&
                sender->role < MemberRole::DJ) {
                const char* err = "ERR:no_permission\n";
                sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
                return;
            }
            // Store lyric text for new joiners
            group.last_text = payload;
        }
        // "chat" and "info" types: allow all roles (no permission check)

        // Forward TEXT: to all other group members
        std::string fwd = "TEXT:" + payload + "\n";
        for (const auto& m : group.members) {
            if (addr_equal(m.addr, from)) continue;
            sendto(g_udp_sock, fwd.c_str(), fwd.size(), 0,
                   (const sockaddr*)&m.addr, sizeof(m.addr));
        }
        return;
    }
}

// ── MIX handler ───────────────────────────────────────────────────────────────
// Format: "MIX:on\n" or "MIX:off\n"

static void handle_mix(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 4, len - 4);
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    std::lock_guard<std::shared_mutex> lock(g_mutex);
    for (auto& [name, group] : g_groups) {
        Member* sender = find_member(group, from);
        if (!sender) continue;
        sender->mixing = (payload == "on");
        // Notify group
        std::string notify = "MIX:" + std::string(sender->device_name.empty() ? addr_str(from) : sender->device_name) + ":" + payload + "\n";
        for (const auto& m : group.members) {
            if (addr_equal(m.addr, from)) continue;
            sendto(g_udp_sock, notify.c_str(), notify.size(), 0,
                   (const sockaddr*)&m.addr, sizeof(m.addr));
        }
        return;
    }
}

// ── P2P File distribution handlers ────────────────────────────────────────────

// Helper: parse hex string to nibble bitfield
static std::vector<bool> hex_to_bitfield(const std::string& hex, uint32_t num_chunks) {
    std::vector<bool> bits(num_chunks, false);
    for (size_t i = 0; i < hex.size(); i++) {
        char c = hex[i];
        uint8_t nibble = 0;
        if (c >= '0' && c <= '9') nibble = c - '0';
        else if (c >= 'a' && c <= 'f') nibble = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') nibble = 10 + (c - 'A');
        for (int b = 3; b >= 0; b--) {
            size_t bit_idx = i * 4 + (3 - b);
            if (bit_idx < num_chunks) {
                bits[bit_idx] = (nibble >> b) & 1;
            }
        }
    }
    return bits;
}

// Helper: broadcast a message to all group members except sender
static void broadcast_to_group(Group& group, const std::string& msg, const sockaddr_in* exclude = nullptr) {
    for (const auto& m : group.members) {
        if (exclude && addr_equal(m.addr, *exclude)) continue;
        sendto(g_udp_sock, msg.c_str(), msg.size(), 0,
               (const sockaddr*)&m.addr, sizeof(m.addr));
    }
}

// FILE_OFFER:<filename>:<size_bytes>:<sha256>:<num_chunks>\n
// DJ announces a file. Relay stores manifest, broadcasts to all members.
static void handle_file_offer(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 11, len - 11);  // skip "FILE_OFFER:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    // Parse: filename:size_bytes:sha256:num_chunks
    size_t p1 = payload.find(':');
    if (p1 == std::string::npos) return;
    size_t p2 = payload.find(':', p1 + 1);
    if (p2 == std::string::npos) return;
    size_t p3 = payload.find(':', p2 + 1);
    if (p3 == std::string::npos) return;

    std::string filename = payload.substr(0, p1);
    uint64_t size_bytes = (uint64_t)std::strtoull(payload.substr(p1 + 1, p2 - p1 - 1).c_str(), nullptr, 10);
    std::string sha256 = payload.substr(p2 + 1, p3 - p2 - 1);
    uint32_t num_chunks = (uint32_t)std::atoi(payload.substr(p3 + 1).c_str());

    if (filename.empty() || sha256.empty() || num_chunks == 0) return;

    std::lock_guard<std::shared_mutex> lock(g_mutex);
    std::string gname = find_member_group(from);
    if (gname.empty()) return;

    auto git = g_groups.find(gname);
    if (git == g_groups.end()) return;
    Group& group = git->second;

    // Permission check: DJ/Owner only
    Member* sender = find_member(group, from);
    if (!sender) return;
    if (!is_random_channel(gname) && !is_free_name(gname) &&
        sender->role < MemberRole::DJ) {
        const char* err = "ERR:no_permission\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    // Create manifest
    FileManifest manifest;
    manifest.filename = filename;
    manifest.sha256 = sha256;
    manifest.size_bytes = size_bytes;
    manifest.num_chunks = num_chunks;
    manifest.offered = std::chrono::steady_clock::now();

    // Mark DJ as complete seeder (has all chunks)
    uint64_t dj_key = addr_key(from);
    std::vector<bool> all_chunks(num_chunks, true);
    manifest.peer_chunks[dj_key] = all_chunks;
    manifest.complete_peers.insert(dj_key);

    group.file_manifests[sha256] = std::move(manifest);
    group.current_file_sha = sha256;

    g_file_offers.fetch_add(1, std::memory_order_relaxed);

    // Broadcast FILE_MANIFEST to all members
    std::string broadcast = "FILE_MANIFEST:" + filename + ":" +
                            std::to_string(size_bytes) + ":" + sha256 + ":" +
                            std::to_string(num_chunks) + "\n";
    broadcast_to_group(group, broadcast, &from);

    // Coordinate initial distribution: assign chunk ranges to first-wave peers
    // Pick up to kFileFirstWavePeers listeners as initial seed targets
    std::vector<size_t> first_wave;
    for (size_t i = 0; i < group.members.size() && first_wave.size() < kFileFirstWavePeers; i++) {
        if (!addr_equal(group.members[i].addr, from)) {
            first_wave.push_back(i);
        }
    }

    if (!first_wave.empty()) {
        // Send SEED_PLAN to each first-wave peer: which chunks to request from DJ
        for (size_t w = 0; w < first_wave.size(); w++) {
            // Assign chunks in round-robin: peer w gets chunks w, w+fanout, w+2*fanout...
            std::string chunks_list;
            for (uint32_t c = (uint32_t)w; c < num_chunks; c += (uint32_t)first_wave.size()) {
                if (!chunks_list.empty()) chunks_list += ",";
                chunks_list += std::to_string(c);
            }
            std::string plan = "SEED_PLAN:" + sha256 + ":" + addr_str(from) + ":" + chunks_list + "\n";
            sendto(g_udp_sock, plan.c_str(), plan.size(), 0,
                   (const sockaddr*)&group.members[first_wave[w]].addr,
                   sizeof(group.members[first_wave[w]].addr));
        }
    }

    fprintf(stderr, "[p2p-file] FILE_OFFER: %s (%llu bytes, %u chunks, sha=%s) in group '%s'\n",
            filename.c_str(), (unsigned long long)size_bytes, num_chunks,
            sha256.substr(0, 8).c_str(), gname.c_str());
}

// FILE_URL:<url>:<sha256>:<size_bytes>\n
// DJ provides a URL for direct HTTP download. Forward to all members.
static void handle_file_url(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 9, len - 9);  // skip "FILE_URL:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    // Parse: url:sha256:size_bytes — but URL may contain colons, so parse from the end
    size_t last_colon = payload.rfind(':');
    if (last_colon == std::string::npos) return;
    size_t second_last_colon = payload.rfind(':', last_colon - 1);
    if (second_last_colon == std::string::npos) return;

    std::string url = payload.substr(0, second_last_colon);
    std::string sha256 = payload.substr(second_last_colon + 1, last_colon - second_last_colon - 1);
    uint64_t size_bytes = (uint64_t)std::strtoull(payload.substr(last_colon + 1).c_str(), nullptr, 10);

    if (url.empty() || sha256.empty()) return;

    std::lock_guard<std::shared_mutex> lock(g_mutex);
    std::string gname = find_member_group(from);
    if (gname.empty()) return;

    auto git = g_groups.find(gname);
    if (git == g_groups.end()) return;
    Group& group = git->second;

    // Permission check
    Member* sender = find_member(group, from);
    if (!sender) return;
    if (!is_random_channel(gname) && !is_free_name(gname) &&
        sender->role < MemberRole::DJ) {
        const char* err = "ERR:no_permission\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    // Store URL in manifest (create if doesn't exist)
    auto mit = group.file_manifests.find(sha256);
    if (mit != group.file_manifests.end()) {
        mit->second.url = url;
    } else {
        // Create a minimal manifest for URL-only distribution
        FileManifest manifest;
        manifest.sha256 = sha256;
        manifest.size_bytes = size_bytes;
        manifest.url = url;
        manifest.offered = std::chrono::steady_clock::now();
        manifest.num_chunks = (uint32_t)((size_bytes + kFileChunkSize - 1) / kFileChunkSize);
        // DJ is a complete seeder
        uint64_t dj_key = addr_key(from);
        manifest.complete_peers.insert(dj_key);
        group.file_manifests[sha256] = std::move(manifest);
    }

    g_file_url_offers.fetch_add(1, std::memory_order_relaxed);

    // Forward FILE_URL to all members
    std::string fwd = "FILE_URL:" + url + ":" + sha256 + ":" + std::to_string(size_bytes) + "\n";
    broadcast_to_group(group, fwd, &from);

    fprintf(stderr, "[p2p-file] FILE_URL: %s (sha=%s) in group '%s'\n",
            url.c_str(), sha256.substr(0, 8).c_str(), gname.c_str());
}

// FILE_HAVE:<sha256>:<bitfield_hex>\n
// Client reports which chunks it has. Update manifest peer_chunks.
static void handle_file_have(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 10, len - 10);  // skip "FILE_HAVE:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    size_t colon = payload.find(':');
    if (colon == std::string::npos) return;

    std::string sha256 = payload.substr(0, colon);
    std::string bitfield_hex = payload.substr(colon + 1);

    if (sha256.empty() || bitfield_hex.empty()) return;

    std::lock_guard<std::shared_mutex> lock(g_mutex);
    std::string gname = find_member_group(from);
    if (gname.empty()) return;

    auto git = g_groups.find(gname);
    if (git == g_groups.end()) return;
    Group& group = git->second;

    auto mit = group.file_manifests.find(sha256);
    if (mit == group.file_manifests.end()) return;

    FileManifest& manifest = mit->second;
    uint64_t peer_key = addr_key(from);

    std::vector<bool> bits = hex_to_bitfield(bitfield_hex, manifest.num_chunks);
    manifest.peer_chunks[peer_key] = bits;

    // Check if this peer now has all chunks
    bool all_complete = true;
    for (uint32_t i = 0; i < manifest.num_chunks; i++) {
        if (i < bits.size() && !bits[i]) { all_complete = false; break; }
    }
    if (all_complete) {
        manifest.complete_peers.insert(peer_key);
    }
}

// FILE_COMPLETE:<sha256>\n
// Client has all chunks. Mark as complete seeder.
static void handle_file_complete(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 14, len - 14);  // skip "FILE_COMPLETE:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    std::string sha256 = payload;
    if (sha256.empty()) return;

    std::lock_guard<std::shared_mutex> lock(g_mutex);
    std::string gname = find_member_group(from);
    if (gname.empty()) return;

    auto git = g_groups.find(gname);
    if (git == g_groups.end()) return;
    Group& group = git->second;

    auto mit = group.file_manifests.find(sha256);
    if (mit == group.file_manifests.end()) return;

    FileManifest& manifest = mit->second;
    uint64_t peer_key = addr_key(from);

    // Mark all chunks as owned
    std::vector<bool> all_chunks(manifest.num_chunks, true);
    manifest.peer_chunks[peer_key] = all_chunks;
    manifest.complete_peers.insert(peer_key);

    g_file_completions.fetch_add(1, std::memory_order_relaxed);

    // Notify DJ (and other interested parties) that this peer is ready
    std::string ready_msg = "FILE_READY:" + sha256 + "\n";
    // Send to all DJ/Owner members so they know a listener is ready
    for (const auto& m : group.members) {
        if (m.role >= MemberRole::DJ) {
            sendto(g_udp_sock, ready_msg.c_str(), ready_msg.size(), 0,
                   (const sockaddr*)&m.addr, sizeof(m.addr));
        }
    }

    fprintf(stderr, "[p2p-file] FILE_COMPLETE: %s from %s in group '%s' (%zu/%zu seeders)\n",
            sha256.substr(0, 8).c_str(), addr_str(from).c_str(), gname.c_str(),
            manifest.complete_peers.size(), group.members.size());
}

// CHUNK_REQ:<sha256>:<chunk_index>\n
// Client asks relay "who has chunk N?" Relay responds with FILE_PEERS.
static void handle_chunk_request(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 10, len - 10);  // skip "CHUNK_REQ:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    size_t colon = payload.find(':');
    if (colon == std::string::npos) return;

    std::string sha256 = payload.substr(0, colon);
    uint32_t chunk_index = (uint32_t)std::atoi(payload.substr(colon + 1).c_str());

    std::lock_guard<std::shared_mutex> lock(g_mutex);
    std::string gname = find_member_group(from);
    if (gname.empty()) return;

    auto git = g_groups.find(gname);
    if (git == g_groups.end()) return;
    Group& group = git->second;

    auto mit = group.file_manifests.find(sha256);
    if (mit == group.file_manifests.end()) return;

    FileManifest& manifest = mit->second;
    if (chunk_index >= manifest.num_chunks) return;

    // Build list of peers that have this chunk
    // Prefer: 1) complete seeders, 2) peers with the chunk, 3) same /24 subnet first
    uint32_t requester_subnet = from.sin_addr.s_addr & htonl(0xFFFFFF00);
    std::string peers_msg = "FILE_PEERS:" + sha256 + ":" + std::to_string(chunk_index);

    struct PeerCandidate {
        sockaddr_in addr;
        bool same_subnet;
        bool complete;
    };
    std::vector<PeerCandidate> candidates;

    for (const auto& [peer_key, chunks] : manifest.peer_chunks) {
        // Check if this peer has the requested chunk
        bool has_chunk = false;
        if (manifest.complete_peers.count(peer_key)) {
            has_chunk = true;
        } else if (chunk_index < chunks.size() && chunks[chunk_index]) {
            has_chunk = true;
        }
        if (!has_chunk) continue;

        // Find the member's address from their addr_key
        for (const auto& m : group.members) {
            if (addr_key(m.addr) == peer_key && !addr_equal(m.addr, from)) {
                bool same = (m.addr.sin_addr.s_addr & htonl(0xFFFFFF00)) == requester_subnet;
                candidates.push_back({m.addr, same, manifest.complete_peers.count(peer_key) > 0});
                break;
            }
        }
    }

    // Sort: same-subnet complete seeders first, then same-subnet partial, then others
    std::sort(candidates.begin(), candidates.end(),
        [](const PeerCandidate& a, const PeerCandidate& b) {
            if (a.same_subnet != b.same_subnet) return a.same_subnet;
            return a.complete && !b.complete;
        });

    // Include up to 6 peers in the response
    size_t count = std::min(candidates.size(), (size_t)6);
    for (size_t i = 0; i < count; i++) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &candidates[i].addr.sin_addr, ip, sizeof(ip));
        peers_msg += ":" + std::string(ip) + ":" + std::to_string(ntohs(candidates[i].addr.sin_port));
    }
    peers_msg += "\n";

    sendto(g_udp_sock, peers_msg.c_str(), peers_msg.size(), 0,
           (const sockaddr*)&from, sizeof(from));
}

// FILE_CHUNK:<sha256>:<chunk_index>:<data_base64>\n
// DJ sends a chunk. Relay does NOT store the data, but notes the DJ has it.
// In practice, DJ sends chunks directly to first-wave peers; this handler
// is for relay-mediated fallback only.
static void handle_file_chunk(const char* msg, size_t len, const sockaddr_in& from) {
    // We only need to parse sha256 and chunk_index for bookkeeping
    // The relay does NOT store chunk data — it's a coordinator only
    std::string payload(msg + 11, len - 11);  // skip "FILE_CHUNK:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    size_t p1 = payload.find(':');
    if (p1 == std::string::npos) return;
    size_t p2 = payload.find(':', p1 + 1);
    if (p2 == std::string::npos) return;

    std::string sha256 = payload.substr(0, p1);
    // chunk_index and data_base64 are present but relay doesn't store the data
    // The relay's role is coordination — chunk data flows peer-to-peer

    // Just confirm receipt
    const char* ok = "OK:chunk_received\n";
    sendto(g_udp_sock, ok, strlen(ok), 0, (const sockaddr*)&from, sizeof(from));
    (void)sha256;  // suppress unused warning
}

// ── Cascade protocol handlers ────────────────────────────────────────────────

// CASCADE_JOIN:<group>:<peer_id>:<secret>\n
static void handle_cascade_join(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 13, len - 13);  // skip "CASCADE_JOIN:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    // Parse group:peer_id:secret
    size_t p1 = payload.find(':');
    if (p1 == std::string::npos) return;
    size_t p2 = payload.find(':', p1 + 1);
    if (p2 == std::string::npos) return;

    std::string group = payload.substr(0, p1);
    std::string peer_id = payload.substr(p1 + 1, p2 - p1 - 1);
    std::string secret = payload.substr(p2 + 1);

    // Verify cascade secret
    if (!g_cascade_secret.empty() && secret != g_cascade_secret) {
        const char* err = "ERR:cascade_auth_failed\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        fprintf(stderr, "[cascade] Rejected CASCADE_JOIN from %s: bad secret\n", addr_str(from).c_str());
        return;
    }

    std::lock_guard<std::mutex> lock(g_downstream_mutex);
    // Find or create downstream peer
    DownstreamPeer* peer = nullptr;
    for (auto& dp : g_downstream_peers) {
        if (dp.peer_id == peer_id) {
            peer = &dp;
            break;
        }
    }
    if (!peer) {
        g_downstream_peers.push_back({});
        peer = &g_downstream_peers.back();
        peer->peer_id = peer_id;
        peer->addr = from;
        peer->last_seen = std::chrono::steady_clock::now();
        fprintf(stderr, "[cascade] New downstream peer %s (%s)\n", peer_id.c_str(), addr_str(from).c_str());
    }
    peer->addr = from;  // update addr (NAT rebind)
    peer->last_seen = std::chrono::steady_clock::now();
    peer->groups.insert(group);

    fprintf(stderr, "[cascade] Peer %s subscribed to group '%s'\n", peer_id.c_str(), group.c_str());
    const char* ok = "OK:cascade_joined\n";
    sendto(g_udp_sock, ok, strlen(ok), 0, (const sockaddr*)&from, sizeof(from));
}

// CASCADE_HELLO:<peer_id>\n
static void handle_cascade_hello(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 14, len - 14);  // skip "CASCADE_HELLO:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    std::string peer_id = payload;

    std::lock_guard<std::mutex> lock(g_downstream_mutex);
    for (auto& dp : g_downstream_peers) {
        if (dp.peer_id == peer_id) {
            dp.last_seen = std::chrono::steady_clock::now();
            dp.addr = from;  // update addr (NAT rebind)
            return;
        }
    }
    // Unknown peer — ignore (they haven't sent CASCADE_JOIN)
}

// CASCADE_LEAVE:<group>:<peer_id>\n
static void handle_cascade_leave(const char* msg, size_t len, const sockaddr_in& /*from*/) {
    std::string payload(msg + 14, len - 14);  // skip "CASCADE_LEAVE:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    size_t p1 = payload.find(':');
    if (p1 == std::string::npos) return;
    std::string group = payload.substr(0, p1);
    std::string peer_id = payload.substr(p1 + 1);

    std::lock_guard<std::mutex> lock(g_downstream_mutex);
    for (auto& dp : g_downstream_peers) {
        if (dp.peer_id == peer_id) {
            dp.groups.erase(group);
            fprintf(stderr, "[cascade] Peer %s unsubscribed from group '%s'\n", peer_id.c_str(), group.c_str());
            return;
        }
    }
}

// CASCADE_STATS:<peer_id>:<json>\n
static void handle_cascade_stats(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 14, len - 14);  // skip "CASCADE_STATS:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    size_t p1 = payload.find(':');
    if (p1 == std::string::npos) return;
    std::string peer_id = payload.substr(0, p1);
    std::string json = payload.substr(p1 + 1);

    uint64_t listeners = (uint64_t)json_get_int(json, "listeners");

    std::lock_guard<std::mutex> lock(g_downstream_mutex);
    for (auto& dp : g_downstream_peers) {
        if (dp.peer_id == peer_id) {
            dp.listener_count = listeners;
            dp.last_seen = std::chrono::steady_clock::now();
            dp.addr = from;
            return;
        }
    }
}

// Clean up stale downstream peers
static void cleanup_downstream_peers() {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_downstream_mutex);
    g_downstream_peers.erase(
        std::remove_if(g_downstream_peers.begin(), g_downstream_peers.end(),
            [&](const DownstreamPeer& dp) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    now - dp.last_seen).count();
                if (elapsed > 30) {  // 30s timeout for downstream relays
                    fprintf(stderr, "[cascade] Downstream peer %s timed out\n", dp.peer_id.c_str());
                    return true;
                }
                return false;
            }),
        g_downstream_peers.end());
}

// Upstream connection loop (for region/edge modes)
static void upstream_thread_func() {
    fprintf(stderr, "[cascade] Upstream thread started, connecting to %s\n",
            addr_str(g_upstream_addr).c_str());

    g_upstream_connected = true;

    auto last_hello = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    auto last_stats = std::chrono::steady_clock::now() - std::chrono::seconds(60);
    std::unordered_set<std::string> subscribed_groups;

    while (g_running) {
        auto now = std::chrono::steady_clock::now();

        // Send CASCADE_HELLO every 5 seconds
        auto hello_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_hello).count();
        if (hello_elapsed >= 5) {
            std::string hello_msg = "CASCADE_HELLO:" + g_my_peer_id + "\n";
            sendto(g_udp_sock, hello_msg.c_str(), hello_msg.size(), 0,
                   (const sockaddr*)&g_upstream_addr, sizeof(g_upstream_addr));
            last_hello = now;
        }

        // Send CASCADE_STATS every 30 seconds
        auto stats_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_stats).count();
        if (stats_elapsed >= 30) {
            // Count local listeners
            uint64_t total_listeners = 0;
            std::vector<std::string> group_list;
            {
                std::lock_guard<std::shared_mutex> lock(g_mutex);
                for (const auto& [name, group] : g_groups) {
                    total_listeners += group.members.size();
                    group_list.push_back(name);
                }
            }
            // Also count downstream relay listeners
            {
                std::lock_guard<std::mutex> lock(g_downstream_mutex);
                for (const auto& dp : g_downstream_peers) {
                    total_listeners += dp.listener_count;
                }
            }
            std::string groups_json = "[";
            bool first = true;
            for (const auto& g : group_list) {
                if (!first) groups_json += ",";
                first = false;
                groups_json += "\"" + json_escape(g) + "\"";
            }
            groups_json += "]";
            std::string stats_msg = "CASCADE_STATS:" + g_my_peer_id + ":{\"listeners\":"
                + std::to_string(total_listeners) + ",\"groups\":" + groups_json + "}\n";
            sendto(g_udp_sock, stats_msg.c_str(), stats_msg.size(), 0,
                   (const sockaddr*)&g_upstream_addr, sizeof(g_upstream_addr));
            last_stats = now;
        }

        // Check for new local groups that need CASCADE_JOIN upstream
        {
            std::lock_guard<std::shared_mutex> lock(g_mutex);
            for (const auto& [name, group] : g_groups) {
                if (!group.members.empty() && subscribed_groups.find(name) == subscribed_groups.end()) {
                    std::string cascade_msg = "CASCADE_JOIN:" + name + ":" + g_my_peer_id + ":" + g_cascade_secret + "\n";
                    sendto(g_udp_sock, cascade_msg.c_str(), cascade_msg.size(), 0,
                           (const sockaddr*)&g_upstream_addr, sizeof(g_upstream_addr));
                    subscribed_groups.insert(name);
                }
            }
            // Remove subscriptions for dissolved groups
            for (auto sit = subscribed_groups.begin(); sit != subscribed_groups.end(); ) {
                if (g_groups.find(*sit) == g_groups.end()) {
                    std::string cascade_msg = "CASCADE_LEAVE:" + *sit + ":" + g_my_peer_id + "\n";
                    sendto(g_udp_sock, cascade_msg.c_str(), cascade_msg.size(), 0,
                           (const sockaddr*)&g_upstream_addr, sizeof(g_upstream_addr));
                    sit = subscribed_groups.erase(sit);
                } else {
                    ++sit;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    g_upstream_connected = false;
}

// Handle audio from upstream (region/edge receives audio, forwards to local members)
static void forward_upstream_audio(const uint8_t* data, size_t len) {
    // This is called when we receive an audio packet from our upstream relay.
    // Forward to all local members in all groups (upstream only sends packets
    // for groups we're subscribed to).
    std::lock_guard<std::shared_mutex> lock(g_mutex);
    for (auto& [name, group] : g_groups) {
        std::vector<sockaddr_in> dests;
        dests.reserve(group.members.size());
        for (const auto& m : group.members) {
            dests.push_back(m.addr);
        }
        if (!dests.empty()) {
            size_t count = dests.size();
            g_total_packets_fwd += count;
            g_total_bytes_fwd += len * count;
            group.packets_forwarded++;
            group.bytes_forwarded += len * count;

            // Also forward to downstream relays (if we're a region)
            if (g_tier == RelayTier::Region) {
                std::lock_guard<std::mutex> dlock(g_downstream_mutex);
                for (const auto& dp : g_downstream_peers) {
                    if (dp.groups.count(name)) {
                        dests.push_back(dp.addr);
                    }
                }
            }

            enqueue_forward(data, len, std::move(dests));
        }
    }
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
        "  --origin             Run as origin tier (accept downstream relays)\n"
        "  --region HOST:PORT   Run as region relay (connect to origin)\n"
        "  --edge HOST:PORT     Run as edge relay (connect to region)\n"
        "  --cascade-secret S   Shared auth secret between tiers\n"
        "  --workers N          Worker threads for sendto() (default: 4)\n"
        "  --no-copyright       Disable copyright detection\n"
        "  --fingerprint-api U  External fingerprint API endpoint\n"
        "  --fingerprint-api-key K  API key for fingerprint service\n"
        "  --royalty-log PATH   Royalty JSONL log path (default: /data/royalties.jsonl)\n"
        "  --wallets-db PATH    Wallet database file (default: /data/wallets.json)\n"
        "  --transactions-log P Transaction log file (default: /data/transactions.jsonl)\n"
        "  --help               Show this help\n\n"
        "Cascade Architecture (planet-scale):\n"
        "  DJ -> Origin (1) -> Region (~20) -> Edge (~10K) -> Listeners (3B)\n"
        "  If none of --origin/--region/--edge, runs as standalone (default).\n\n"
        "UDP Protocol:\n"
        "  JOIN:<group>:<password>\\n        Join/create group\n"
        "  HELLO\\n                          Heartbeat (send every 5s)\n"
        "  CHECK:<name>\\n                   Check channel name availability\n"
        "  CLAIM:<name>:<device>:<txn>\\n    Claim channel ownership\n"
        "  RELEASE:<name>:<device>\\n        Release channel ownership\n"
        "  META:<json>\\n                    Broadcast metadata to group\n"
        "  REPLAY:<offset_sec>\\n            Replay last N seconds of audio\n"
        "  RECORD:<group>\\n                 Start server-side recording\n"
        "  RECORD_STOP:<group>\\n            Stop server-side recording\n"
        "  PING:<addr>:<ts>\\n               Latency probe (forwarded to target)\n"
        "  ROUTE:<addr>:<p2p|relay>\\n       Path preference (informational)\n"
        "  CASCADE_JOIN:<group>:<id>:<secret>\\n  Downstream relay subscribes\n"
        "  CASCADE_HELLO:<id>\\n             Downstream relay keepalive\n"
        "  CASCADE_LEAVE:<group>:<id>\\n     Downstream relay unsubscribes\n"
        "  CASCADE_STATS:<id>:<json>\\n      Downstream reports stats\n"
        "  SWARM_READY\\n                    Client can relay audio to peers\n"
        "  SWARM_UNABLE\\n                   Client can't relay (NAT/firewall)\n"
        "  SWARM_ACK:<parent>\\n             Client confirms parent assignment\n"
        "  SWARM_LOST:<parent>\\n            Client lost connection to parent\n"
        "  FILE_OFFER:<n>:<sz>:<sha>:<c>\\n  P2P file offer (DJ only)\n"
        "  FILE_URL:<url>:<sha>:<sz>\\n      HTTP URL for file download\n"
        "  FILE_HAVE:<sha>:<bitfield>\\n     Report owned chunks\n"
        "  FILE_COMPLETE:<sha>\\n            All chunks received\n"
        "  CHUNK_REQ:<sha>:<idx>\\n          Ask relay for peers with chunk\n"
        "  WALLET\\n                        Query wallet balance\n"
        "  CHARGE:<amount>:<token>\\n       Add funds to wallet\n"
        "  WITHDRAW:<amount>\\n             Withdraw funds\n"
        "  PAYOUT_SETUP:<method>:<id>\\n    Setup payout account\n"
        "  TIP:<amount>\\n                  Tip the DJ\n"
        "  TRANSACTIONS:<count>\\n          Get last N transactions\n"
        "  <RTP/OSTP audio packet>          Forwarded to all group members\n\n",
        prog, kDefaultPort, kDefaultMaxGroups, kDefaultMaxMembers);
}

// ── HTTP API for Web channel purchases ──────────────────────────────────────

static void http_send(int fd, int status, const char* status_text,
                      const char* content_type, const std::string& body) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n\r\n",
        status, status_text, content_type, body.size());
    write(fd, header, hlen);
    write(fd, body.data(), body.size());
}

// ── Minimal SHA-1 for WebSocket handshake (RFC 6455) ─────────────────────────
namespace {
std::string sha1_raw(const std::string& input) {
    uint32_t h0=0x67452301,h1=0xEFCDAB89,h2=0x98BADCFE,h3=0x10325476,h4=0xC3D2E1F0;
    size_t orig=input.size(), padded=((orig+9+63)/64)*64;
    std::vector<uint8_t> msg(padded,0);
    memcpy(msg.data(),input.data(),orig);
    msg[orig]=0x80;
    uint64_t bits=orig*8;
    for(int i=0;i<8;i++) msg[padded-1-i]=(uint8_t)(bits>>(i*8));
    auto rotl=[](uint32_t x,int n)->uint32_t{return(x<<n)|(x>>(32-n));};
    for(size_t blk=0;blk<padded;blk+=64){
        uint32_t w[80];
        for(int i=0;i<16;i++) w[i]=(msg[blk+i*4]<<24)|(msg[blk+i*4+1]<<16)|(msg[blk+i*4+2]<<8)|msg[blk+i*4+3];
        for(int i=16;i<80;i++) w[i]=rotl(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
        uint32_t a=h0,b=h1,c=h2,d=h3,e=h4;
        for(int i=0;i<80;i++){
            uint32_t f,k;
            if(i<20){f=(b&c)|((~b)&d);k=0x5A827999;}
            else if(i<40){f=b^c^d;k=0x6ED9EBA1;}
            else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}
            else{f=b^c^d;k=0xCA62C1D6;}
            uint32_t tmp=rotl(a,5)+f+e+k+w[i];
            e=d;d=c;c=rotl(b,30);b=a;a=tmp;
        }
        h0+=a;h1+=b;h2+=c;h3+=d;h4+=e;
    }
    std::string r(20,0);
    auto put=[&](int i,uint32_t v){r[i]=(char)(v>>24);r[i+1]=(char)(v>>16);r[i+2]=(char)(v>>8);r[i+3]=(char)v;};
    put(0,h0);put(4,h1);put(8,h2);put(12,h3);put(16,h4);
    return r;
}
std::string base64_encode(const std::string& in) {
    static const char t[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; o.reserve(((in.size()+2)/3)*4);
    for(size_t i=0;i<in.size();i+=3){
        uint32_t n=((uint8_t)in[i])<<16;
        if(i+1<in.size())n|=((uint8_t)in[i+1])<<8;
        if(i+2<in.size())n|=(uint8_t)in[i+2];
        o+=t[(n>>18)&0x3f]; o+=t[(n>>12)&0x3f];
        o+=(i+1<in.size())?t[(n>>6)&0x3f]:'=';
        o+=(i+2<in.size())?t[n&0x3f]:'=';
    }
    return o;
}
} // namespace

// ── WebSocket audio bridge ───────────────────────────────────────────────────
// Upgrades HTTP to WebSocket, JOINs a channel, and streams audio as binary frames.
// URL: /ws/audio?channel=<name>[&device=<name>]

// Active WebSocket listeners registered as virtual members
struct WsListener {
    int fd;
    std::string channel;
    std::string device_name;
    std::atomic<bool> active{true};
};
static std::mutex g_ws_mutex;
static std::vector<std::shared_ptr<WsListener>> g_ws_listeners;

// Send a WebSocket binary frame
static bool ws_send_binary(int fd, const uint8_t* data, size_t len) {
    uint8_t header[10];
    size_t hlen = 2;
    header[0] = 0x82;  // FIN + binary opcode
    if (len < 126) {
        header[1] = (uint8_t)len;
    } else if (len < 65536) {
        header[1] = 126;
        header[2] = (uint8_t)(len >> 8);
        header[3] = (uint8_t)(len & 0xFF);
        hlen = 4;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; i++) header[2+i] = (uint8_t)(len >> (56 - i*8));
        hlen = 10;
    }
    if (write(fd, header, hlen) != (ssize_t)hlen) return false;
    if (write(fd, data, len) != (ssize_t)len) return false;
    return true;
}

// Send a WebSocket text frame
static bool ws_send_text(int fd, const std::string& msg) {
    uint8_t header[4];
    size_t hlen = 2;
    header[0] = 0x81;  // FIN + text opcode
    if (msg.size() < 126) {
        header[1] = (uint8_t)msg.size();
    } else {
        header[1] = 126;
        header[2] = (uint8_t)(msg.size() >> 8);
        header[3] = (uint8_t)(msg.size() & 0xFF);
        hlen = 4;
    }
    if (write(fd, header, hlen) != (ssize_t)hlen) return false;
    if (write(fd, msg.data(), msg.size()) != (ssize_t)msg.size()) return false;
    return true;
}

// WebSocket client thread: reads ping/pong/close, keeps connection alive
static void ws_client_thread(std::shared_ptr<WsListener> ws) {
    // Register this WS listener to receive audio from the channel
    {
        std::lock_guard<std::mutex> lock(g_ws_mutex);
        g_ws_listeners.push_back(ws);
    }

    fprintf(stderr, "[ws] Browser connected to channel '%s' as '%s'\n",
            ws->channel.c_str(), ws->device_name.c_str());

    // Send initial status
    ws_send_text(ws->fd, "{\"type\":\"joined\",\"channel\":\"" + ws->channel + "\"}");

    // Read loop: handle ping/pong/close frames
    while (ws->active.load()) {
        uint8_t hdr[2];
        ssize_t r = read(ws->fd, hdr, 2);
        if (r <= 0) break;

        uint8_t opcode = hdr[0] & 0x0F;
        bool masked = hdr[1] & 0x80;
        uint64_t payload_len = hdr[1] & 0x7F;

        if (payload_len == 126) {
            uint8_t ext[2]; read(ws->fd, ext, 2);
            payload_len = (ext[0] << 8) | ext[1];
        } else if (payload_len == 127) {
            uint8_t ext[8]; read(ws->fd, ext, 8);
            payload_len = 0;
            for (int i = 0; i < 8; i++) payload_len = (payload_len << 8) | ext[i];
        }

        uint8_t mask[4] = {};
        if (masked) read(ws->fd, mask, 4);

        // Read and unmask payload
        std::vector<uint8_t> payload(payload_len);
        size_t total_read = 0;
        while (total_read < payload_len) {
            r = read(ws->fd, payload.data() + total_read, payload_len - total_read);
            if (r <= 0) goto done;
            total_read += r;
        }
        if (masked) for (size_t i = 0; i < payload_len; i++) payload[i] ^= mask[i % 4];

        if (opcode == 0x8) break;  // Close
        if (opcode == 0x9) {       // Ping → Pong
            uint8_t pong[2] = {0x8A, 0x00};
            write(ws->fd, pong, 2);
        }
        // Text frames from browser are ignored (audio is server→browser only)
    }
done:
    ws->active.store(false);
    close(ws->fd);

    // Unregister
    {
        std::lock_guard<std::mutex> lock(g_ws_mutex);
        g_ws_listeners.erase(
            std::remove_if(g_ws_listeners.begin(), g_ws_listeners.end(),
                [&](const auto& l) { return l.get() == ws.get(); }),
            g_ws_listeners.end());
    }

    fprintf(stderr, "[ws] Browser disconnected from channel '%s'\n", ws->channel.c_str());
}

// Called from forward_audio to push audio to WebSocket listeners
static void ws_broadcast_audio(const std::string& channel, const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(g_ws_mutex);
    for (auto& ws : g_ws_listeners) {
        if (ws->active.load() && ws->channel == channel) {
            if (!ws_send_binary(ws->fd, data, len)) {
                ws->active.store(false);  // connection broken
            }
        }
    }
}

static void http_thread_func() {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("http socket"); return; }
    int reuse = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(kHttpPort);
    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("http bind"); close(srv); return; }
    if (listen(srv, 16) < 0) { perror("http listen"); close(srv); return; }
    fprintf(stderr, "[relay] HTTP API listening on port %u\n", kHttpPort);

    while (g_running) {
        sockaddr_in client{};
        socklen_t clen = sizeof(client);
        int fd = accept(srv, (sockaddr*)&client, &clen);
        if (fd < 0) continue;

        // Set recv timeout to prevent slow-loris attacks
        timeval rcv_tv{5, 0};  // 5 second timeout
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));

        // Cap request size at 16KB to prevent memory abuse
        static constexpr size_t kMaxHttpRequest = 16384;
        char buf[kMaxHttpRequest]{};
        ssize_t total = 0;
        // Read until we have full headers + body (Content-Length aware)
        while (total < (ssize_t)sizeof(buf) - 1) {
            ssize_t n = read(fd, buf + total, sizeof(buf) - 1 - total);
            if (n <= 0) break;
            total += n;
            buf[total] = '\0';
            // Check if we have the full body
            const char* hdr_end = strstr(buf, "\r\n\r\n");
            if (hdr_end) {
                // Parse Content-Length
                const char* cl = strcasestr(buf, "Content-Length:");
                if (cl) {
                    int clen = atoi(cl + 15);
                    int body_start = (hdr_end + 4) - buf;
                    if (total - body_start >= clen) break;  // got full body
                } else {
                    break;  // no content-length, assume we have everything
                }
            }
        }
        if (total <= 0) { close(fd); continue; }

        // Simple per-IP rate limiting (60 requests/minute)
        {
            static std::mutex rl_mutex;
            static std::unordered_map<uint32_t, std::pair<int64_t, int>> rl_map;  // ip → (window_start, count)
            uint32_t ip = client.sin_addr.s_addr;
            int64_t now = now_unix();
            std::lock_guard<std::mutex> rl(rl_mutex);
            auto& entry = rl_map[ip];
            if (now - entry.first >= 60) {
                entry = {now, 1};
            } else if (++entry.second > 60) {
                http_send(fd, 429, "Too Many Requests", "application/json",
                    "{\"error\":\"rate_limited\"}");
                close(fd); continue;
            }
            // Evict stale entries periodically (every 1000 requests)
            if (rl_map.size() > 10000) {
                for (auto it = rl_map.begin(); it != rl_map.end(); ) {
                    if (now - it->second.first > 120) it = rl_map.erase(it);
                    else ++it;
                }
            }
        }

        // Parse method and path
        char method[8]{}, path[256]{};
        sscanf(buf, "%7s %255s", method, path);

        // CORS preflight
        if (strcmp(method, "OPTIONS") == 0) {
            http_send(fd, 204, "No Content", "text/plain", "");
            close(fd); continue;
        }

        // WebSocket upgrade: GET /ws/audio?channel=<name>[&device=<name>]
        if (strcmp(method, "GET") == 0 && strncmp(path, "/ws/audio", 9) == 0) {
            // Check for Upgrade: websocket header
            const char* upgrade_hdr = strcasestr(buf, "Upgrade: websocket");
            const char* ws_key_hdr = strcasestr(buf, "Sec-WebSocket-Key:");
            if (upgrade_hdr && ws_key_hdr) {
                // Parse channel from query string
                std::string channel, device = "browser";
                const char* q = strchr(path, '?');
                if (q) {
                    std::string qs(q + 1);
                    size_t cp = qs.find("channel=");
                    if (cp != std::string::npos) {
                        size_t end = qs.find('&', cp + 8);
                        channel = qs.substr(cp + 8, end == std::string::npos ? end : end - cp - 8);
                    }
                    size_t dp = qs.find("device=");
                    if (dp != std::string::npos) {
                        size_t end = qs.find('&', dp + 7);
                        device = qs.substr(dp + 7, end == std::string::npos ? end : end - dp - 7);
                    }
                }
                if (channel.empty()) {
                    http_send(fd, 400, "Bad Request", "text/plain", "Missing channel parameter");
                    close(fd); continue;
                }

                // Extract WebSocket key and compute accept hash
                const char* key_start = ws_key_hdr + 18;
                while (*key_start == ' ') key_start++;
                const char* key_end = strstr(key_start, "\r\n");
                std::string ws_key(key_start, key_end ? key_end : key_start + 24);
                std::string accept = base64_encode(sha1_raw(ws_key + "258EAFA5-E914-47DA-95CA-5AB5DC11D455"));

                // Send WebSocket upgrade response
                std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                    "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: " + accept + "\r\n"
                    "Access-Control-Allow-Origin: *\r\n\r\n";
                write(fd, resp.c_str(), resp.size());

                // Create WsListener and spawn handler thread
                auto ws = std::make_shared<WsListener>();
                ws->fd = fd;
                ws->channel = channel;
                ws->device_name = device;
                std::thread(ws_client_thread, ws).detach();
                continue;  // fd is now owned by the WebSocket thread, don't close
            }
        }

        // GET /.well-known/apple-app-site-association — Universal Links
        if (strcmp(method, "GET") == 0 && strcmp(path, "/.well-known/apple-app-site-association") == 0) {
            std::string aasa = R"({
  "applinks": {
    "apps": [],
    "details": [
      {
        "appID": "5BV85JW8US.com.soluna.SolunaReceiver",
        "paths": ["/c/*", "/channel/*"]
      }
    ]
  },
  "webcredentials": {
    "apps": ["5BV85JW8US.com.soluna.SolunaReceiver"]
  }
})";
            http_send(fd, 200, "OK", "application/json", aasa.c_str());
            close(fd); continue;
        }

        // GET /c/<channel> — Channel landing page (Universal Link + download page)
        if (strcmp(method, "GET") == 0 && strncmp(path, "/c/", 3) == 0 && strlen(path) > 3) {
            std::string channel_name(path + 3);
            // URL-decode basic %XX
            std::string decoded;
            for (size_t i = 0; i < channel_name.size(); i++) {
                if (channel_name[i] == '%' && i + 2 < channel_name.size()) {
                    int hex = 0;
                    sscanf(channel_name.c_str() + i + 1, "%2x", &hex);
                    decoded += (char)hex;
                    i += 2;
                } else if (channel_name[i] == '+') {
                    decoded += ' ';
                } else {
                    decoded += channel_name[i];
                }
            }
            channel_name = decoded;

            // Count current listeners in this channel
            int listener_count = 0;
            bool channel_exists = false;
            {
                std::shared_lock<std::shared_mutex> lk(g_mutex);
                auto it = g_groups.find(channel_name);
                if (it != g_groups.end()) {
                    channel_exists = true;
                    listener_count = (int)it->second.members.size();
                }
            }

            std::string status_text = channel_exists
                ? (std::to_string(listener_count) + " listeners connected")
                : "Channel available";
            std::string status_badge = channel_exists ? "LIVE" : "IDLE";
            std::string badge_color = channel_exists ? "#10b981" : "#6b7280";

            std::string html = R"(<!DOCTYPE html>
<html lang="ja">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>)" + channel_name + R"( — Soluna Channel</title>
<meta property="og:title" content=")" + channel_name + R"( — Soluna Channel">
<meta property="og:description" content="Join this audio channel on Soluna. Real-time streaming.">
<meta property="og:type" content="website">
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,"Inter",sans-serif;background:#05060a;color:#e8eaf2;display:flex;justify-content:center;align-items:center;min-height:100vh;padding:20px}
.card{background:#12151f;border:1px solid rgba(255,255,255,0.08);border-radius:24px;padding:48px 40px;max-width:420px;width:100%;text-align:center}
.logo{font-size:28px;font-weight:800;margin-bottom:8px;background:linear-gradient(135deg,#3b82f6,#8b5cf6);-webkit-background-clip:text;-webkit-text-fill-color:transparent}
.channel-name{font-size:32px;font-weight:800;margin:24px 0 8px;word-break:break-all}
.badge{display:inline-block;font-size:11px;font-weight:700;padding:4px 12px;border-radius:20px;color:#fff;margin-bottom:8px}
.listeners{color:#8b91a8;font-size:14px;margin-bottom:32px}
.btn{display:flex;align-items:center;justify-content:center;gap:8px;width:100%;padding:14px;border-radius:14px;font-size:15px;font-weight:700;text-decoration:none;color:#fff;margin-bottom:12px;transition:opacity 0.2s}
.btn:hover{opacity:0.85}
.btn-ios{background:linear-gradient(135deg,#007AFF,#5856D6)}
.btn-mac{background:linear-gradient(135deg,#3b82f6,#8b5cf6)}
.btn-android{background:linear-gradient(135deg,#34A853,#0F9D58)}
.btn-browser{background:linear-gradient(135deg,#06b6d4,#3b82f6)}
.divider{border:none;border-top:1px solid rgba(255,255,255,0.06);margin:24px 0}
.footer{color:#50566e;font-size:12px}
.footer a{color:#8b91a8;text-decoration:none}
</style>
</head>
<body>
<div class="card">
<div class="logo">&#9670; Soluna</div>
<div class="channel-name">)" + channel_name + R"(</div>
<span class="badge" style="background:)" + badge_color + R"(">)" + status_badge + R"(</span>
<div class="listeners">)" + status_text + R"(</div>
<a class="btn btn-ios" href="https://testflight.apple.com/join/PYbefDSE">
<svg viewBox="0 0 24 24" fill="currentColor" width="18" height="18"><path d="M17.05 20.28c-.98.95-2.05.8-3.08.35-1.09-.46-2.09-.48-3.24 0-1.44.62-2.2.44-3.06-.35C2.79 15.25 3.51 7.59 9.05 7.31c1.35.07 2.29.74 3.08.8 1.18-.24 2.31-.93 3.57-.84 1.51.12 2.65.72 3.4 1.8-3.12 1.87-2.38 5.98.48 7.13-.57 1.5-1.31 2.99-2.54 4.09zM12.03 7.25c-.15-2.23 1.66-4.07 3.74-4.25.29 2.58-2.34 4.5-3.74 4.25z"/></svg>
iOS TestFlight
</a>
<a class="btn btn-mac" href="https://github.com/yukihamada/opensonic/releases/latest/download/Soluna-mac.pkg">
<svg viewBox="0 0 24 24" fill="currentColor" width="18" height="18"><path d="M19 9h-4V3H9v6H5l7 7 7-7zM5 18v2h14v-2H5z"/></svg>
Mac .pkg Download
</a>
<a class="btn btn-android" href="https://github.com/yukihamada/opensonic/releases/latest/download/Soluna-android.apk">
<svg viewBox="0 0 24 24" fill="currentColor" width="18" height="18"><path d="M17.6 11.8l1.7-3c.1-.2 0-.4-.2-.5s-.4 0-.5.2l-1.7 3c-1.3-.6-2.7-.9-4.2-.9s-2.9.3-4.2.9L6.8 8.5c-.1-.2-.3-.3-.5-.2s-.3.3-.2.5l1.7 3C4.7 13.6 2.7 16.8 2.3 20.6h18.6c-.4-3.8-2.4-7-5.3-8.8z"/></svg>
Android APK Download
</a>
<a class="btn btn-browser" href="/dashboard#channel=)" + channel_name + R"(">
<svg viewBox="0 0 24 24" fill="currentColor" width="18" height="18"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm-1 17.93c-3.95-.49-7-3.85-7-7.93 0-.62.08-1.21.21-1.79L9 15v1c0 1.1.9 2 2 2v1.93zm6.9-2.54c-.26-.81-1-1.39-1.9-1.39h-1v-3c0-.55-.45-1-1-1H8v-2h2c.55 0 1-.45 1-1V7h2c1.1 0 2-.9 2-2v-.41c2.93 1.19 5 4.06 5 7.41 0 2.08-.8 3.97-2.1 5.39z"/></svg>
Browser Dashboard
</a>
<hr class="divider">
<div class="footer"><a href="/">Soluna</a> — Open Source Network Audio</div>
</div>
<script>
// Try to open app via Universal Link (iOS/Android deep link)
if (/iPhone|iPad|iPod/.test(navigator.userAgent)) {
  setTimeout(function(){ window.location='soluna://channel/)" + channel_name + R"('; }, 100);
}
</script>
</body>
</html>)";
            http_send(fd, 200, "OK", "text/html; charset=utf-8", html.c_str());
            close(fd); continue;
        }

        // GET /metrics — Prometheus metrics endpoint
        if (strcmp(method, "GET") == 0 && strcmp(path, "/metrics") == 0) {
            std::string m;
            m.reserve(4096);
            // Counters
            m += "# HELP soluna_relay_packets_rx_total Total UDP packets received\n";
            m += "# TYPE soluna_relay_packets_rx_total counter\n";
            m += "soluna_relay_packets_rx_total " + std::to_string(g_total_packets_rx) + "\n";
            m += "# HELP soluna_relay_packets_fwd_total Total packets forwarded\n";
            m += "# TYPE soluna_relay_packets_fwd_total counter\n";
            m += "soluna_relay_packets_fwd_total " + std::to_string(g_total_packets_fwd) + "\n";
            m += "# HELP soluna_relay_bytes_rx_total Total bytes received\n";
            m += "# TYPE soluna_relay_bytes_rx_total counter\n";
            m += "soluna_relay_bytes_rx_total " + std::to_string(g_total_bytes_rx) + "\n";
            m += "# HELP soluna_relay_bytes_fwd_total Total bytes forwarded\n";
            m += "# TYPE soluna_relay_bytes_fwd_total counter\n";
            m += "soluna_relay_bytes_fwd_total " + std::to_string(g_total_bytes_fwd) + "\n";
            m += "# HELP soluna_relay_joins_total Total JOIN events\n";
            m += "# TYPE soluna_relay_joins_total counter\n";
            m += "soluna_relay_joins_total " + std::to_string(g_total_joins) + "\n";
            m += "# HELP soluna_relay_charges_cents_total Total charges in cents\n";
            m += "# TYPE soluna_relay_charges_cents_total counter\n";
            m += "soluna_relay_charges_cents_total " + std::to_string(g_total_charges_cents.load()) + "\n";
            m += "# HELP soluna_relay_tips_cents_total Total tips in cents\n";
            m += "# TYPE soluna_relay_tips_cents_total counter\n";
            m += "soluna_relay_tips_cents_total " + std::to_string(g_total_tips_cents.load()) + "\n";
            m += "# HELP soluna_relay_withdrawals_cents_total Total withdrawals in cents\n";
            m += "# TYPE soluna_relay_withdrawals_cents_total counter\n";
            m += "soluna_relay_withdrawals_cents_total " + std::to_string(g_total_withdrawals_cents.load()) + "\n";
            m += "# HELP soluna_relay_swarm_packets_saved_total Packets saved by P2P swarm\n";
            m += "# TYPE soluna_relay_swarm_packets_saved_total counter\n";
            m += "soluna_relay_swarm_packets_saved_total " + std::to_string(g_swarm_packets_saved.load()) + "\n";
            // Gauges
            {
                std::lock_guard<std::shared_mutex> lock(g_mutex);
                m += "# HELP soluna_relay_groups_active Current active groups\n";
                m += "# TYPE soluna_relay_groups_active gauge\n";
                m += "soluna_relay_groups_active " + std::to_string(g_groups.size()) + "\n";
                size_t total_members = 0;
                for (const auto& [name, group] : g_groups) total_members += group.members.size();
                m += "# HELP soluna_relay_members_active Current connected members\n";
                m += "# TYPE soluna_relay_members_active gauge\n";
                m += "soluna_relay_members_active " + std::to_string(total_members) + "\n";
            }
            {
                std::lock_guard<std::mutex> wl(g_wallet_mutex);
                m += "# HELP soluna_relay_wallets_total Total wallet accounts\n";
                m += "# TYPE soluna_relay_wallets_total gauge\n";
                m += "soluna_relay_wallets_total " + std::to_string(g_wallets.size()) + "\n";
                m += "# HELP soluna_relay_rights_holders_total Total rights holders\n";
                m += "# TYPE soluna_relay_rights_holders_total gauge\n";
                m += "soluna_relay_rights_holders_total " + std::to_string(g_rights_holder_balances.size()) + "\n";
            }
            m += "# HELP soluna_relay_transactions_total In-memory transactions\n";
            m += "# TYPE soluna_relay_transactions_total gauge\n";
            m += "soluna_relay_transactions_total " + std::to_string(g_transactions.size()) + "\n";
            http_send(fd, 200, "OK", "text/plain; version=0.0.4; charset=utf-8", m);
            close(fd); continue;
        }

        // GET /api/channel/check?name=xxx
        if (strcmp(method, "GET") == 0 && strncmp(path, "/api/channel/check?name=", 24) == 0) {
            std::string name(path + 24);
            if (is_reserved_name(name)) {
                http_send(fd, 200, "OK", "application/json",
                    "{\"available\":false,\"taken\":true,\"reserved\":true}");
                close(fd); continue;
            }
            std::lock_guard<std::shared_mutex> lock(g_mutex);
            auto it = g_channels.find(name);
            bool taken = (it != g_channels.end() && it->second.expires > now_unix());
            std::string json = taken
                ? "{\"available\":false,\"taken\":true}"
                : "{\"available\":true,\"taken\":false}";
            http_send(fd, 200, "OK", "application/json", json);
            close(fd); continue;
        }

        // POST /api/channel/claim  body: {"name":"xxx","device":"yyy","session_id":"cs_xxx"}
        // Requires Stripe checkout session_id OR existing ownership (same device)
        if (strcmp(method, "POST") == 0 && strcmp(path, "/api/channel/claim") == 0) {
            const char* body = strstr(buf, "\r\n\r\n");
            if (body) body += 4; else body = "";
            auto extract = [](const char* json, const char* key) -> std::string {
                char search[64];
                snprintf(search, sizeof(search), "\"%s\":\"", key);
                const char* p = strstr(json, search);
                if (!p) return "";
                p += strlen(search);
                const char* end = strchr(p, '"');
                return end ? std::string(p, end - p) : "";
            };
            std::string name = extract(body, "name");
            std::string device = extract(body, "device");
            std::string session_id = extract(body, "session_id");
            if (name.empty() || device.empty()) {
                http_send(fd, 400, "Bad Request", "application/json",
                    "{\"error\":\"name and device required\"}");
                close(fd); continue;
            }
            if (name.size() < 3 || name.size() > 20) {
                http_send(fd, 400, "Bad Request", "application/json",
                    "{\"error\":\"invalid_name\"}");
                close(fd); continue;
            }
            if (is_reserved_name(name)) {
                http_send(fd, 403, "Forbidden", "application/json",
                    "{\"error\":\"reserved_name\"}");
                close(fd); continue;
            }
            // Check authorization: need session_id (from Stripe) or be the existing owner
            std::lock_guard<std::shared_mutex> lock(g_mutex);
            auto it = g_channels.find(name);
            bool is_owner = (it != g_channels.end() && it->second.device == device && it->second.expires > now_unix());
            if (!is_owner && (session_id.empty() || session_id.substr(0, 3) != "cs_")) {
                http_send(fd, 402, "Payment Required", "application/json",
                    "{\"error\":\"payment_required\",\"message\":\"Stripe checkout session_id required\"}");
                close(fd); continue;
            }
            int64_t expiry = now_unix() + kChannelExpiryDays * 86400;
            if (it == g_channels.end() || it->second.expires <= now_unix()) {
                g_channels[name] = {device, session_id, expiry, "", ""};
                channels_save();
                http_send(fd, 200, "OK", "application/json",
                    "{\"status\":\"claimed\",\"name\":\"" + name + "\"}");
            } else if (it->second.device == device) {
                it->second.expires = expiry;
                channels_save();
                http_send(fd, 200, "OK", "application/json",
                    "{\"status\":\"renewed\",\"name\":\"" + name + "\"}");
            } else {
                http_send(fd, 409, "Conflict", "application/json",
                    "{\"error\":\"taken\"}");
            }
            close(fd); continue;
        }

        // POST /api/channel/claim-stripe  body: {"name":"xxx","device":"yyy","customer":"cus_xxx","subscription":"sub_xxx"}
        if (strcmp(method, "POST") == 0 && strcmp(path, "/api/channel/claim-stripe") == 0) {
            const char* body = strstr(buf, "\r\n\r\n");
            if (body) body += 4; else body = "";
            auto extract = [](const char* json, const char* key) -> std::string {
                char search[64];
                snprintf(search, sizeof(search), "\"%s\":\"", key);
                const char* p = strstr(json, search);
                if (!p) return "";
                p += strlen(search);
                const char* end = strchr(p, '"');
                return end ? std::string(p, end - p) : "";
            };
            std::string name = extract(body, "name");
            std::string device = extract(body, "device");
            std::string customer = extract(body, "customer");
            std::string sub = extract(body, "subscription");
            if (name.empty() || device.empty()) {
                http_send(fd, 400, "Bad Request", "application/json",
                    "{\"error\":\"name and device required\"}");
                close(fd); continue;
            }
            int64_t expiry = now_unix() + kChannelExpiryDays * 86400;
            std::lock_guard<std::shared_mutex> lock(g_mutex);
            auto it = g_channels.find(name);
            if (it == g_channels.end() || it->second.expires <= now_unix()) {
                g_channels[name] = {device, "", expiry, customer, sub};
                channels_save();
                http_send(fd, 200, "OK", "application/json",
                    "{\"status\":\"claimed\",\"name\":\"" + name + "\"}");
            } else if (it->second.device == device) {
                it->second.expires = expiry;
                it->second.stripe_customer = customer;
                it->second.stripe_sub = sub;
                channels_save();
                http_send(fd, 200, "OK", "application/json",
                    "{\"status\":\"renewed\",\"name\":\"" + name + "\"}");
            } else {
                http_send(fd, 409, "Conflict", "application/json",
                    "{\"error\":\"taken\"}");
            }
            close(fd); continue;
        }

        // POST /api/stripe/wh-<secret> — Stripe sends subscription events here
        if (strcmp(method, "POST") == 0 && !g_webhook_path.empty() && g_webhook_path == path) {
            const char* body = strstr(buf, "\r\n\r\n");
            if (body) body += 4; else body = "";
            std::string payload(body);
            // Extract event type and subscription/customer IDs
            std::string event_type = json_get_string(payload, "type");
            // For subscription events, data is nested: data.object.id, data.object.customer
            std::string sub_id = json_get_string(payload, "id", payload.find("\"object\""));
            std::string cust_id = json_get_string(payload, "customer");

            fprintf(stderr, "[relay] Stripe webhook: type=%s sub=%s cust=%s\n",
                    event_type.c_str(), sub_id.c_str(), cust_id.c_str());

            if (event_type == "customer.subscription.deleted" ||
                event_type == "customer.subscription.paused" ||
                event_type == "invoice.payment_failed") {
                // Find and expire the channel owned by this subscription or customer
                std::lock_guard<std::shared_mutex> lock(g_mutex);
                for (auto& [name, rec] : g_channels) {
                    if ((!sub_id.empty() && rec.stripe_sub == sub_id) ||
                        (!cust_id.empty() && rec.stripe_customer == cust_id)) {
                        fprintf(stderr, "[relay] Expiring channel '%s' (sub=%s)\n",
                                name.c_str(), sub_id.c_str());
                        rec.expires = now_unix();  // expire immediately
                        channels_save();
                        break;
                    }
                }
            }
            // Always return 200 to Stripe (they retry on non-2xx)
            http_send(fd, 200, "OK", "application/json", "{\"received\":true}");
            close(fd); continue;
        }

        // GET /api/admin/channels — list all channels (simple admin view)
        if (strcmp(method, "GET") == 0 && strncmp(path, "/api/admin/channels", 19) == 0) {
            // Check for admin key in query string: ?key=<RELAY_ADMIN_KEY>
            const char* key_param = strstr(path, "key=");
            if (!key_param || g_admin_key.empty() || g_admin_key != std::string(key_param + 4, g_admin_key.size())) {
                http_send(fd, 403, "Forbidden", "application/json",
                    "{\"error\":\"invalid admin key\"}");
                close(fd); continue;
            }
            std::lock_guard<std::shared_mutex> lock(g_mutex);
            int64_t now = now_unix();
            std::string json = "{\"channels\":[";
            bool first = true;
            int active = 0, expired = 0;
            for (const auto& [name, rec] : g_channels) {
                if (!first) json += ",";
                first = false;
                bool is_active = rec.expires > now;
                if (is_active) active++; else expired++;
                int64_t days_left = is_active ? (rec.expires - now) / 86400 : 0;
                json += "{\"name\":\"" + json_escape(name) + "\""
                      + ",\"device\":\"" + json_escape(rec.device) + "\""
                      + ",\"active\":" + (is_active ? "true" : "false")
                      + ",\"days_left\":" + std::to_string(days_left)
                      + ",\"stripe_customer\":\"" + json_escape(rec.stripe_customer) + "\""
                      + ",\"stripe_sub\":\"" + json_escape(rec.stripe_sub) + "\""
                      + "}";
            }
            json += "],\"total\":" + std::to_string(g_channels.size())
                  + ",\"active\":" + std::to_string(active)
                  + ",\"expired\":" + std::to_string(expired) + "}";
            http_send(fd, 200, "OK", "application/json", json);
            close(fd); continue;
        }

        // POST /api/admin/release — manually release a channel
        if (strcmp(method, "POST") == 0 && strcmp(path, "/api/admin/release") == 0) {
            const char* body = strstr(buf, "\r\n\r\n");
            if (body) body += 4; else body = "";
            auto extract = [](const char* json, const char* key) -> std::string {
                char search[64];
                snprintf(search, sizeof(search), "\"%s\":\"", key);
                const char* p = strstr(json, search);
                if (!p) return "";
                p += strlen(search);
                const char* end = strchr(p, '"');
                return end ? std::string(p, end - p) : "";
            };
            std::string admin_key = extract(body, "key");
            std::string name = extract(body, "name");
            if (admin_key != "soluna-admin-2026") {
                http_send(fd, 403, "Forbidden", "application/json",
                    "{\"error\":\"invalid admin key\"}");
                close(fd); continue;
            }
            std::lock_guard<std::shared_mutex> lock(g_mutex);
            auto it = g_channels.find(name);
            if (it != g_channels.end()) {
                g_channels.erase(it);
                channels_save();
                http_send(fd, 200, "OK", "application/json",
                    "{\"released\":\"" + json_escape(name) + "\"}");
            } else {
                http_send(fd, 404, "Not Found", "application/json",
                    "{\"error\":\"channel not found\"}");
            }
            close(fd); continue;
        }

        // GET /api/music/ — list available music files
        if (strcmp(method, "GET") == 0 && strcmp(path, "/api/music/") == 0) {
            DIR* d = opendir("/data/music");
            std::string json = "{\"files\":[";
            bool first_file = true;
            if (d) {
                struct dirent* entry;
                while ((entry = readdir(d))) {
                    std::string name = entry->d_name;
                    if (name.size() < 4) continue;
                    std::string ext = name.substr(name.size() - 4);
                    for (auto& c : ext) c = tolower(c);
                    if (ext != ".mp3" && ext != ".wav" && ext != ".m4a" && ext != ".aac") continue;
                    if (!first_file) json += ",";
                    first_file = false;
                    json += "\"" + json_escape(name) + "\"";
                }
                closedir(d);
            }
            json += "]}";
            http_send(fd, 200, "OK", "application/json", json);
            close(fd); continue;
        }

        // GET /api/music/<filename> — serve music file
        if (strcmp(method, "GET") == 0 && strncmp(path, "/api/music/", 11) == 0 && strlen(path) > 11) {
            // URL-decode the filename (%20 → space, %XX → char)
            std::string raw(path + 11);
            std::string filename;
            for (size_t i = 0; i < raw.size(); i++) {
                if (raw[i] == '%' && i + 2 < raw.size()) {
                    int hi = 0, lo = 0;
                    if (sscanf(raw.c_str() + i + 1, "%1x%1x", &hi, &lo) == 2) {
                        filename += (char)((hi << 4) | lo);
                        i += 2;
                        continue;
                    }
                }
                filename += raw[i];
            }
            // Security: reject path traversal
            if (filename.find("..") != std::string::npos || filename.find('/') != std::string::npos) {
                http_send(fd, 400, "Bad Request", "application/json", "{\"error\":\"invalid_filename\"}");
                close(fd); continue;
            }
            std::string filepath = "/data/music/" + filename;
            FILE* fp = fopen(filepath.c_str(), "rb");
            if (!fp) {
                http_send(fd, 404, "Not Found", "application/json", "{\"error\":\"file_not_found\"}");
                close(fd); continue;
            }
            fseek(fp, 0, SEEK_END);
            long file_size = ftell(fp);
            fseek(fp, 0, SEEK_SET);

            // Determine content type
            std::string ext = filename.substr(filename.rfind('.') + 1);
            for (auto& c : ext) c = tolower(c);
            const char* content_type = "application/octet-stream";
            if (ext == "mp3") content_type = "audio/mpeg";
            else if (ext == "m4a" || ext == "aac") content_type = "audio/mp4";
            else if (ext == "wav") content_type = "audio/wav";

            // Send HTTP response with file
            char header[512];
            int hlen = snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: %s\r\n"
                "Content-Length: %ld\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Content-Disposition: inline; filename=\"%s\"\r\n"
                "\r\n", content_type, file_size, filename.c_str());
            write(fd, header, hlen);

            // Stream file in 64KB chunks
            char fbuf[65536];
            size_t total_sent = 0;
            while (total_sent < (size_t)file_size) {
                size_t chunk = fread(fbuf, 1, sizeof(fbuf), fp);
                if (chunk == 0) break;
                write(fd, fbuf, chunk);
                total_sent += chunk;
            }
            fclose(fp);
            fprintf(stderr, "[relay] Served music file: %s (%ld bytes)\n", filename.c_str(), file_size);
            close(fd); continue;
        }

        http_send(fd, 404, "Not Found", "application/json", "{\"error\":\"not_found\"}");
        close(fd);
    }
    close(srv);
}

int main(int argc, char** argv) {
    uint16_t port = kDefaultPort;
    uint32_t stats_interval = 30;

    std::string upstream_host_port;  // for --region / --edge

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if      (arg == "--port")           port = (uint16_t)atoi(next());
        else if (arg == "--max-groups")     g_max_groups = (size_t)atoi(next());
        else if (arg == "--max-members")    g_max_members = (size_t)atoi(next());
        else if (arg == "--stats-interval") stats_interval = (uint32_t)atoi(next());
        else if (arg == "--origin")         g_tier = RelayTier::Origin;
        else if (arg == "--region") {       g_tier = RelayTier::Region; upstream_host_port = next(); }
        else if (arg == "--edge") {         g_tier = RelayTier::Edge;   upstream_host_port = next(); }
        else if (arg == "--cascade-secret") g_cascade_secret = next();
        else if (arg == "--workers")        g_num_workers = (size_t)atoi(next());
        else if (arg == "--no-copyright")   g_copyright_enabled = false;
        else if (arg == "--fingerprint-api")     g_fingerprint_api_url = next();
        else if (arg == "--fingerprint-api-key") g_fingerprint_api_key = next();
        else if (arg == "--royalty-log")     g_royalty_log_path = next();
        else if (arg == "--wallets-db")      g_wallets_db_path = next();
        else if (arg == "--transactions-log") g_transactions_log_path = next();
        else if (arg == "--max-replay")     kMaxReplayPackets = (size_t)atoi(next());
        else if (arg == "--help") { print_usage(argv[0]); return 0; }
        else {
            fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            print_usage(argv[0]);
            return 1;
        }
    }

    // Load secrets from environment variables
    {
        const char* env_admin = getenv("RELAY_ADMIN_KEY");
        if (env_admin && env_admin[0]) {
            g_admin_key = env_admin;
        } else {
            // Generate random admin key if not set (printed to stderr for operator)
            g_admin_key = generate_peer_id();  // reuse hex generator
            fprintf(stderr, "[relay] WARNING: RELAY_ADMIN_KEY not set, generated: %s\n",
                    g_admin_key.c_str());
        }
        const char* env_wh = getenv("RELAY_WEBHOOK_PATH");
        if (env_wh && env_wh[0]) {
            g_webhook_path = std::string("/api/stripe/wh-") + env_wh;
        }
        // If not set, webhook endpoint is disabled (g_webhook_path stays empty)
        const char* env_charge = getenv("RELAY_CHARGE_SECRET");
        if (env_charge && env_charge[0]) {
            g_charge_secret = env_charge;
        }
    }

    // Generate unique peer ID
    g_my_peer_id = generate_peer_id();

    // Parse upstream address for region/edge modes
    if (g_tier == RelayTier::Region || g_tier == RelayTier::Edge) {
        if (upstream_host_port.empty()) {
            fprintf(stderr, "Error: --region/--edge requires HOST:PORT argument\n");
            return 1;
        }
        size_t colon = upstream_host_port.rfind(':');
        if (colon == std::string::npos) {
            fprintf(stderr, "Error: Invalid upstream address '%s', expected HOST:PORT\n",
                    upstream_host_port.c_str());
            return 1;
        }
        std::string host = upstream_host_port.substr(0, colon);
        uint16_t uport = (uint16_t)atoi(upstream_host_port.substr(colon + 1).c_str());
        memset(&g_upstream_addr, 0, sizeof(g_upstream_addr));
        g_upstream_addr.sin_family = AF_INET;
        g_upstream_addr.sin_port = htons(uport);
        if (inet_pton(AF_INET, host.c_str(), &g_upstream_addr.sin_addr) != 1) {
            fprintf(stderr, "Error: Cannot resolve upstream host '%s'\n", host.c_str());
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

    // Load channel ownership DB
    channels_load();

    // Load wallet database
    wallets_load();
    wallets_seed();  // Pre-create famous DJ wallets on first run

    // Start HTTP API thread
    std::thread http_thread(http_thread_func);
    http_thread.detach();

    // Start worker threads
    start_workers();

    // Start fingerprint thread for copyright detection
    if (g_copyright_enabled) {
        g_fingerprint_thread = std::thread(fingerprint_thread_func);
        fprintf(stderr, "[copyright] Fingerprint thread started (interval=%zus)\n",
                kFingerprintIntervalSec);
    }

    char timeout_str[32];
    snprintf(timeout_str, sizeof(timeout_str), "%ds", kStaleTimeoutSec);
    const char* tier_name = g_tier == RelayTier::Origin ? "ORIGIN" :
                            g_tier == RelayTier::Region ? "REGION" :
                            g_tier == RelayTier::Edge   ? "EDGE" : "STANDALONE";
    fprintf(stderr,
        "╔═══════════════════════════════════════════════════╗\n"
        "║       soluna-relay — WAN Audio Relay Server       ║\n"
        "╠═══════════════════════════════════════════════════╣\n"
        "║  UDP port:      %-33u ║\n"
        "║  Max groups:    %-33zu ║\n"
        "║  Max members:   %-33zu ║\n"
        "║  Stale timeout: %-33s ║\n"
        "║  Tier:          %-33s ║\n"
        "║  Peer ID:       %-33s ║\n"
        "║  Workers:       %-33zu ║\n"
        "║  Copyright:     %-33s ║\n"
        "╚═══════════════════════════════════════════════════╝\n",
        port, g_max_groups, g_max_members, timeout_str,
        tier_name, g_my_peer_id.c_str(), g_num_workers,
        g_copyright_enabled ? "enabled" : "disabled");

    if (g_tier == RelayTier::Region || g_tier == RelayTier::Edge) {
        fprintf(stderr, "[cascade] Upstream: %s\n", addr_str(g_upstream_addr).c_str());
    }

    // Start upstream connection thread for region/edge modes
    std::thread upstream_thread;
    if (g_tier == RelayTier::Region || g_tier == RelayTier::Edge) {
        upstream_thread = std::thread(upstream_thread_func);
    }

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
            } else if (n >= 6 && memcmp(pkt, "CHECK:", 6) == 0) {
                // Channel name availability check
                handle_check((const char*)pkt, (size_t)n, from);
            } else if (n >= 6 && memcmp(pkt, "CLAIM:", 6) == 0) {
                // Channel name claim
                handle_claim((const char*)pkt, (size_t)n, from);
            } else if (n >= 8 && memcmp(pkt, "RELEASE:", 8) == 0) {
                // Channel name release
                handle_release((const char*)pkt, (size_t)n, from);
            } else if (n >= 5 && memcmp(pkt, "META:", 5) == 0) {
                // Metadata broadcast
                handle_meta((const char*)pkt, (size_t)n, from);
            } else if (n >= 7 && memcmp(pkt, "REPLAY:", 7) == 0) {
                // Timeshift replay request
                handle_replay((const char*)pkt, (size_t)n, from);
            } else if (n >= 12 && memcmp(pkt, "RECORD_STOP:", 12) == 0) {
                // Stop recording (must check before RECORD:)
                handle_record_stop((const char*)pkt, (size_t)n, from);
            } else if (n >= 7 && memcmp(pkt, "RECORD:", 7) == 0) {
                // Start recording
                handle_record((const char*)pkt, (size_t)n, from);
            } else if (n >= 14 && memcmp(pkt, "FILE_COMPLETE:", 14) == 0) {
                // P2P File: client has all chunks (must check before FILE_*)
                handle_file_complete((const char*)pkt, (size_t)n, from);
            } else if (n >= 11 && memcmp(pkt, "FILE_OFFER:", 11) == 0) {
                // P2P File: DJ announces a file for distribution
                handle_file_offer((const char*)pkt, (size_t)n, from);
            } else if (n >= 11 && memcmp(pkt, "FILE_CHUNK:", 11) == 0) {
                // P2P File: DJ sends a chunk (relay only acknowledges)
                handle_file_chunk((const char*)pkt, (size_t)n, from);
            } else if (n >= 10 && memcmp(pkt, "FILE_HAVE:", 10) == 0) {
                // P2P File: client reports chunk ownership bitfield
                handle_file_have((const char*)pkt, (size_t)n, from);
            } else if (n >= 9 && memcmp(pkt, "FILE_URL:", 9) == 0) {
                // P2P File: DJ provides URL for HTTP download
                handle_file_url((const char*)pkt, (size_t)n, from);
            } else if (n >= 5 && memcmp(pkt, "FILE:", 5) == 0) {
                // File-sync: announce current file
                handle_file((const char*)pkt, (size_t)n, from);
            } else if (n >= 5 && memcmp(pkt, "SYNC:", 5) == 0) {
                // File-sync: play/pause/seek command
                handle_sync((const char*)pkt, (size_t)n, from);
            } else if (n >= 6 && memcmp(pkt, "READY:", 6) == 0) {
                // File-sync: receiver ready notification
                handle_ready((const char*)pkt, (size_t)n, from);
            } else if (n >= 5 && memcmp(pkt, "MODE:", 5) == 0) {
                // Set channel mode: private (P2P, no copyright) or public (relay, copyright)
                handle_mode((const char*)pkt, (size_t)n, from);
            } else if (n >= 6 && memcmp(pkt, "GRANT:", 6) == 0) {
                // Grant DJ/listener role to a member (owner only)
                handle_grant((const char*)pkt, (size_t)n, from);
            } else if (n >= 7 && memcmp(pkt, "MEMBERS", 7) == 0) {
                // List members and roles
                handle_members(from);
            } else if (n >= 5 && memcmp(pkt, "TEXT:", 5) == 0) {
                // Text channel (lyrics/chat/info)
                handle_text((const char*)pkt, (size_t)n, from);
            } else if (n >= 4 && memcmp(pkt, "MIX:", 4) == 0) {
                // DJ + mic simultaneous mode toggle
                handle_mix((const char*)pkt, (size_t)n, from);
            } else if (n >= 13 && memcmp(pkt, "CASCADE_JOIN:", 13) == 0) {
                // Cascade: downstream relay subscribes to a group
                handle_cascade_join((const char*)pkt, (size_t)n, from);
            } else if (n >= 14 && memcmp(pkt, "CASCADE_HELLO:", 14) == 0) {
                // Cascade: downstream relay keepalive
                handle_cascade_hello((const char*)pkt, (size_t)n, from);
            } else if (n >= 14 && memcmp(pkt, "CASCADE_LEAVE:", 14) == 0) {
                // Cascade: downstream relay unsubscribes
                handle_cascade_leave((const char*)pkt, (size_t)n, from);
            } else if (n >= 14 && memcmp(pkt, "CASCADE_STATS:", 14) == 0) {
                // Cascade: downstream relay reports stats
                handle_cascade_stats((const char*)pkt, (size_t)n, from);
            } else if (n >= 11 && memcmp(pkt, "SWARM_LOST:", 11) == 0) {
                // P2P Swarm: client lost connection to parent
                handle_swarm_lost((const char*)pkt, (size_t)n, from);
            } else if (n >= 11 && memcmp(pkt, "SWARM_READY", 11) == 0) {
                // P2P Swarm: client can relay to others
                handle_swarm_ready(from);
            } else if (n >= 13 && memcmp(pkt, "SWARM_UNABLE", 12) == 0) {
                // P2P Swarm: client can't relay (symmetric NAT, data saver)
                handle_swarm_unable(from);
            } else if (n >= 10 && memcmp(pkt, "SWARM_ACK:", 10) == 0) {
                // P2P Swarm: client confirms receiving from assigned parent
                handle_swarm_ack((const char*)pkt, (size_t)n, from);
            } else if (n >= 10 && memcmp(pkt, "CHUNK_REQ:", 10) == 0) {
                // P2P File: client asks which peers have a chunk
                handle_chunk_request((const char*)pkt, (size_t)n, from);
            } else if (n >= 13 && memcmp(pkt, "COPYRIGHT_ACK", 13) == 0) {
                // Copyright: DJ acknowledges notice
                handle_copyright_ack(from);
            } else if (n >= 14 && memcmp(pkt, "COPYRIGHT_SKIP", 14) == 0) {
                // Copyright: DJ will skip track
                handle_copyright_skip(from);
            } else if (n >= 6 && memcmp(pkt, "WALLET", 6) == 0 && (n == 6 || pkt[6] == '\n')) {
                // Wallet balance query
                handle_wallet(from);
            } else if (n >= 7 && memcmp(pkt, "CHARGE:", 7) == 0) {
                // Add funds to wallet
                handle_charge((const char*)pkt, (size_t)n, from);
            } else if (n >= 9 && memcmp(pkt, "WITHDRAW:", 9) == 0) {
                // Withdraw funds from wallet
                handle_withdraw((const char*)pkt, (size_t)n, from);
            } else if (n >= 13 && memcmp(pkt, "PAYOUT_SETUP:", 13) == 0) {
                // Setup payout account
                handle_payout_setup((const char*)pkt, (size_t)n, from);
            } else if (n >= 14 && memcmp(pkt, "LICENSED_PLAY:", 14) == 0) {
                // DJ declares licensed content (skip copyright detection)
                handle_licensed_play((const char*)pkt, (size_t)n, from);
            } else if (n >= 8 && memcmp(pkt, "SUPPORT:", 8) == 0) {
                // Listener supports DJ's royalty costs
                handle_support((const char*)pkt, (size_t)n, from);
            } else if (n >= 4 && memcmp(pkt, "TIP:", 4) == 0) {
                // Listener tips DJ
                handle_tip((const char*)pkt, (size_t)n, from);
            } else if (n >= 14 && memcmp(pkt, "RIGHTS_BALANCE", 14) == 0) {
                // Rights holder balance query
                handle_rights_balance((const char*)pkt, (size_t)n, from);
            } else if (n >= 13 && memcmp(pkt, "TRANSACTIONS:", 13) == 0) {
                // Get transaction history
                handle_transactions((const char*)pkt, (size_t)n, from);
            } else if (n >= 5 && memcmp(pkt, "HELLO", 5) == 0) {
                // Heartbeat
                handle_hello(from);
            } else if (n >= 5 && memcmp(pkt, "PING:", 5) == 0) {
                // P2P latency probe — forward to target peer
                handle_ping((const char*)pkt, (size_t)n, from);
            } else if (n >= 6 && memcmp(pkt, "ROUTE:", 6) == 0) {
                // Path preference notification (informational)
                handle_route((const char*)pkt, (size_t)n, from);
            } else if (n >= 12 && (pkt[0] & 0xC0) == 0x80) {
                // RTP/OSTP audio packet (version bits = 2, i.e. 0x80)
                // If from upstream relay, forward to all local members
                if ((g_tier == RelayTier::Region || g_tier == RelayTier::Edge) &&
                    g_upstream_connected && addr_equal(from, g_upstream_addr)) {
                    forward_upstream_audio(pkt, (size_t)n);
                } else {
                    forward_audio(pkt, (size_t)n, from);
                }
            }
            // else: unknown packet, silently drop
        }

        // Periodic cleanup
        auto cleanup_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_cleanup).count();
        if (cleanup_elapsed >= kCleanupIntervalSec) {
            cleanup_stale();
            if (g_tier == RelayTier::Origin || g_tier == RelayTier::Region) {
                cleanup_downstream_peers();
            }
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

    // Save wallets on shutdown
    wallets_save();
    fprintf(stderr, "[relay] Wallets saved to %s\n", g_wallets_db_path.c_str());

    // Stop fingerprint thread
    g_fingerprint_running.store(false, std::memory_order_relaxed);
    if (g_fingerprint_thread.joinable()) {
        g_fingerprint_thread.join();
    }

    // Stop worker threads
    stop_workers();

    // Join upstream thread if running
    if (upstream_thread.joinable()) {
        upstream_thread.join();
    }

    close(g_udp_sock);
    return 0;
}
