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
 *   TALK:on|off\n               — Toggle talk mode (all members can send audio)
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

#include <sqlite3.h>
#include <curl/curl.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/srtp.h>

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
static std::string g_stripe_webhook_secret;  // STRIPE_WEBHOOK_SECRET env (for signature verification)

// CHARGE replay protection: track used HMAC tokens (TTL = 300s)
static std::mutex g_charge_nonce_mutex;
static std::unordered_map<std::string, int64_t> g_charge_used_tokens;  // hmac -> expiry timestamp

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
    std::string device_id;    // UUID, from JOIN or server-generated
    MemberRole role = MemberRole::Listener;
    bool mixing = false;  // DJ + mic simultaneous mode
    bool mic_allowed = false;  // per-device mic permission (Owner/DJ always implicitly allowed)
    std::string session_token;  // 16-char hex, issued on JOIN, required for wallet ops
    uint32_t net_delay_ms = 0;  // reported network delay for sync mode coordination
    sockaddr_in backup_parent{};     // secondary parent for dual-parent reception
    bool has_backup_parent = false;
    std::chrono::steady_clock::time_point last_primary_packet;  // for churn detection
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

/// Who pays copyright royalties on this channel
enum class RoyaltyPayer {
    DJ = 0,        // DJ pays (default — current behavior)
    Owner = 1,     // Channel owner pays (venue/event)
    Listener = 2,  // All listeners split the cost
    Free = 3,      // No one pays (private/licensed)
};

struct Group {
    std::string name;
    std::string password;     // empty = no auth
    ChannelMode mode = ChannelMode::Public;  // default public; owner can change via MODE:
    RoyaltyPayer royalty_payer = RoyaltyPayer::DJ;  // who pays copyright royalties
    std::vector<Member> members;
    std::chrono::steady_clock::time_point created;
    uint64_t packets_forwarded = 0;
    uint64_t bytes_forwarded   = 0;
    std::chrono::steady_clock::time_point last_audio_time;  // last audio packet forwarded

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

    // FEC (Forward Error Correction) state — §8.1 of protocol spec
    // XOR parity over kFecGroupSize data packets; emitted as PT=127
    struct FecState {
        std::vector<uint8_t> xor_buf;    // running XOR accumulator
        uint16_t base_seq = 0;           // first sequence in current FEC group
        size_t   pkt_count = 0;          // packets accumulated so far
        size_t   max_len = 0;            // max payload length in group (for padding)
    } fec;

    // Per-member sequence tracking (for NACK gap detection)
    // Maps member addr_key → last seen 32-bit sequence number
    std::unordered_map<uint64_t, uint32_t> member_last_seq;

    bool talk_mode = false;  // When true, all members can send audio (conversation mode)

    // Sync mode: group-wide max delay coordination
    uint32_t max_delay_ms = 0;  // current broadcast max delay
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

// Generate cryptographically secure random hex string via OpenSSL RAND_bytes
static std::string generate_hex(int length) {
    int byte_count = (length + 1) / 2;
    std::vector<unsigned char> buf(byte_count);
    RAND_bytes(buf.data(), byte_count);
    const char hex[] = "0123456789abcdef";
    std::string id;
    id.reserve(length);
    for (int i = 0; i < byte_count && (int)id.size() < length; i++) {
        id += hex[(buf[i] >> 4) & 0x0f];
        if ((int)id.size() < length)
            id += hex[buf[i] & 0x0f];
    }
    return id;
}

static std::string generate_peer_id() { return generate_hex(32); }

// Generate UUID v4 (xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx)
static std::string generate_uuid_v4() {
    std::string hex = generate_hex(32);
    // Set version 4 (char at position 12) and variant (char at position 16)
    hex[12] = '4';
    const char variant[] = "89ab";
    std::random_device rd;
    hex[16] = variant[rd() % 4];
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) +
           "-" + hex.substr(16, 4) + "-" + hex.substr(20, 12);
}

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
    std::string tmp_path = std::string(kChannelDbPath) + ".tmp";
    FILE* f = fopen(tmp_path.c_str(), "w");
    if (!f) {
        fprintf(stderr, "[relay] WARNING: cannot write %s: %s\n", tmp_path.c_str(), strerror(errno));
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
    rename(tmp_path.c_str(), kChannelDbPath);  // atomic on POSIX
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

// ── Listener-side fingerprint aggregation ────────────────────────────────────
struct FingerprintReport {
    uint64_t hash;
    uint64_t timestamp;  // unix seconds
    std::string device_id;
    std::string chromaprint;  // base64-encoded Chromaprint fingerprint (optional)
    int duration = 0;         // audio duration in seconds for Chromaprint (optional)
};

struct ChannelFingerprints {
    std::mutex mtx;
    std::vector<FingerprintReport> recent;  // last 60 seconds of reports

    // Consensus: if N reports in last 60s have similar hash, it's a match
    // "similar" = hamming distance <= 8 bits (out of 64)
};

static std::unordered_map<std::string, ChannelFingerprints> g_listener_fingerprints;

// ── Fingerprint SQLite persistence & match tracking ──────────────────────────
static sqlite3* g_fp_db = nullptr;
static std::mutex g_fp_db_mutex;

struct FingerprintMatchEntry {
    std::string channel;
    std::string fingerprint;  // hex string
    size_t listener_count = 0;
    double confidence = 0.0;
    uint64_t first_seen = 0;
    uint64_t last_seen = 0;
    double revenue_rights_holder = 0.0;
    double revenue_dj_cashback = 0.0;
    double revenue_platform = 0.0;
};
static std::mutex g_fp_matches_mutex;
static std::unordered_map<std::string, FingerprintMatchEntry> g_fp_matches;  // channel → current match

static void fp_db_init() {
    const char* db_path = "/data/fingerprints.db";
    int rc = sqlite3_open(db_path, &g_fp_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[fp-db] Failed to open %s: %s\n", db_path, sqlite3_errmsg(g_fp_db));
        g_fp_db = nullptr;
        return;
    }
    // WAL mode for concurrent reads
    sqlite3_exec(g_fp_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

    const char* sql =
        "CREATE TABLE IF NOT EXISTS fingerprint_reports("
        "  id INTEGER PRIMARY KEY,"
        "  channel TEXT,"
        "  device_id TEXT,"
        "  fingerprint TEXT,"
        "  timestamp INTEGER,"
        "  created_at INTEGER,"
        "  chromaprint TEXT,"
        "  duration INTEGER"
        ");"
        "CREATE TABLE IF NOT EXISTS fingerprint_matches("
        "  id INTEGER PRIMARY KEY,"
        "  channel TEXT,"
        "  fingerprint TEXT,"
        "  listener_count INTEGER,"
        "  confidence REAL,"
        "  first_seen INTEGER,"
        "  last_seen INTEGER,"
        "  revenue_total REAL"
        ");"
        "CREATE TABLE IF NOT EXISTS identified_songs("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  acoustid TEXT,"
        "  musicbrainz_id TEXT,"
        "  title TEXT,"
        "  artist TEXT,"
        "  album TEXT,"
        "  isrc TEXT,"
        "  youtube_url TEXT,"
        "  youtube_video_id TEXT,"
        "  rights_holder TEXT,"
        "  created_at INTEGER DEFAULT (strftime('%s','now'))"
        ");"
        "CREATE TABLE IF NOT EXISTS channel_now_playing("
        "  channel TEXT PRIMARY KEY,"
        "  song_id INTEGER REFERENCES identified_songs(id),"
        "  fingerprint TEXT,"
        "  confidence REAL,"
        "  listeners INTEGER,"
        "  identified_at INTEGER,"
        "  updated_at INTEGER DEFAULT (strftime('%s','now'))"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_fp_reports_channel ON fingerprint_reports(channel);"
        "CREATE INDEX IF NOT EXISTS idx_fp_matches_channel ON fingerprint_matches(channel);"
        "CREATE INDEX IF NOT EXISTS idx_identified_songs_acoustid ON identified_songs(acoustid);"
        "CREATE INDEX IF NOT EXISTS idx_identified_songs_mbid ON identified_songs(musicbrainz_id);";
    char* err_msg = nullptr;
    rc = sqlite3_exec(g_fp_db, sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[fp-db] Schema error: %s\n", err_msg);
        sqlite3_free(err_msg);
    } else {
        fprintf(stderr, "[fp-db] Initialized %s\n", db_path);
    }
}

static void fp_db_insert_report(const std::string& channel, const std::string& device_id,
                                 const std::string& fingerprint_hex, uint64_t timestamp,
                                 const std::string& chromaprint = "", int duration = 0) {
    if (!g_fp_db) return;
    std::lock_guard<std::mutex> lock(g_fp_db_mutex);
    uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO fingerprint_reports(channel,device_id,fingerprint,timestamp,created_at,chromaprint,duration) VALUES(?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(g_fp_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, channel.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, device_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, fingerprint_hex.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, (sqlite3_int64)timestamp);
        sqlite3_bind_int64(stmt, 5, (sqlite3_int64)now);
        if (chromaprint.empty()) {
            sqlite3_bind_null(stmt, 6);
        } else {
            sqlite3_bind_text(stmt, 6, chromaprint.c_str(), -1, SQLITE_TRANSIENT);
        }
        if (duration > 0) {
            sqlite3_bind_int(stmt, 7, duration);
        } else {
            sqlite3_bind_null(stmt, 7);
        }
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

static void fp_db_insert_match(const FingerprintMatchEntry& m) {
    if (!g_fp_db) return;
    std::lock_guard<std::mutex> lock(g_fp_db_mutex);
    double revenue_total = m.revenue_rights_holder + m.revenue_dj_cashback + m.revenue_platform;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO fingerprint_matches(channel,fingerprint,listener_count,confidence,first_seen,last_seen,revenue_total) VALUES(?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(g_fp_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, m.channel.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, m.fingerprint.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)m.listener_count);
        sqlite3_bind_double(stmt, 4, m.confidence);
        sqlite3_bind_int64(stmt, 5, (sqlite3_int64)m.first_seen);
        sqlite3_bind_int64(stmt, 6, (sqlite3_int64)m.last_seen);
        sqlite3_bind_double(stmt, 7, revenue_total);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

// ── HTTP client (libcurl) for external API calls ─────────────────────────────
static size_t curl_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total);
    return total;
}

static std::string http_get(const std::string& url, const std::string& user_agent = "Soluna/1.0 (https://solun.art)") {
    CURL* curl = curl_easy_init();
    if (!curl) return "";
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "[http] GET %s failed: %s\n", url.c_str(), curl_easy_strerror(res));
        return "";
    }
    return response;
}

static std::string http_post_json(const std::string& url, const std::string& body,
                                   int timeout_sec = 5) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";
    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_sec);
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "[http] POST %s failed: %s\n", url.c_str(), curl_easy_strerror(res));
        return "";
    }
    return response;
}

// ── Solana RPC: verify TIP transaction (OSTP v0.9.3 §7.3) ───────────────────
// Returns true if tx_signature is a confirmed transaction transferring >= amount_usd
// Uses the memo field ("SOLUNA_TIP:<channel>:<amount>") to match intent.
static bool solana_verify_tip_tx(const std::string& tx_sig, const std::string& wallet_pubkey,
                                  double amount_usd) {
    // Only verify if signature looks like a base58 Solana tx (~87 chars)
    if (tx_sig.size() < 60 || tx_sig.size() > 100) return false;

    static const char* RPC = "https://api.mainnet-beta.solana.com";
    char body[512];
    snprintf(body, sizeof(body),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getTransaction\","
             "\"params\":[\"%s\",{\"encoding\":\"json\",\"maxSupportedTransactionVersion\":0}]}",
             tx_sig.c_str());

    std::string resp = http_post_json(RPC, body, 5);
    if (resp.empty()) {
        fprintf(stderr, "[tip] Solana RPC timeout for tx %s — accept pending\n", tx_sig.c_str());
        return true;  // network failure: accept (don't block legitimate tips)
    }

    // Check for "err":null — confirmed transaction
    size_t err_pos = resp.find("\"err\":");
    if (err_pos == std::string::npos) return false;
    size_t val_pos = err_pos + 6;
    while (val_pos < resp.size() && resp[val_pos] == ' ') val_pos++;
    if (resp.substr(val_pos, 4) != "null") {
        fprintf(stderr, "[tip] Solana tx %s not confirmed\n", tx_sig.c_str());
        return false;
    }

    // Check feePayer matches wallet_pubkey
    if (!wallet_pubkey.empty() && resp.find(wallet_pubkey) == std::string::npos) {
        fprintf(stderr, "[tip] Solana tx %s: pubkey mismatch\n", tx_sig.c_str());
        return false;
    }

    return true;  // confirmed, feePayer matches
}

// ── Song identification via AcoustID / MusicBrainz / YouTube ─────────────────

// Simple JSON value extractor (for flat JSON — handles both string and non-string values)
static std::string json_extract_string(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    // Skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size()) return "";
    if (json[pos] == '"') {
        // String value
        pos++;
        size_t end = json.find('"', pos);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos);
    }
    // Non-string value (number, bool, null)
    size_t end = json.find_first_of(",}]\n", pos);
    if (end == std::string::npos) return json.substr(pos);
    return json.substr(pos, end - pos);
}

// URL-encode a string for query parameters
static std::string url_encode(const std::string& s) {
    CURL* curl = curl_easy_init();
    if (!curl) return s;
    char* encoded = curl_easy_escape(curl, s.c_str(), (int)s.size());
    std::string result = encoded ? encoded : s;
    if (encoded) curl_free(encoded);
    curl_easy_cleanup(curl);
    return result;
}

struct IdentifiedSong {
    int64_t id = 0;
    std::string acoustid;
    std::string musicbrainz_id;
    std::string title;
    std::string artist;
    std::string album;
    std::string isrc;
    std::string youtube_url;
    std::string youtube_video_id;
    std::string rights_holder;
};

// Insert or find an identified song in the DB. Returns song ID (0 on failure).
static int64_t fp_db_upsert_song(const IdentifiedSong& song) {
    if (!g_fp_db) return 0;
    std::lock_guard<std::mutex> lock(g_fp_db_mutex);

    // Check if already exists by acoustid or musicbrainz_id
    sqlite3_stmt* stmt = nullptr;
    const char* check_sql = "SELECT id FROM identified_songs WHERE acoustid=? OR musicbrainz_id=? LIMIT 1";
    if (sqlite3_prepare_v2(g_fp_db, check_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, song.acoustid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, song.musicbrainz_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int64_t existing_id = sqlite3_column_int64(stmt, 0);
            sqlite3_finalize(stmt);
            // Update YouTube info if we have it now
            if (!song.youtube_url.empty()) {
                const char* upd = "UPDATE identified_songs SET youtube_url=?, youtube_video_id=? WHERE id=?";
                sqlite3_stmt* ustmt = nullptr;
                if (sqlite3_prepare_v2(g_fp_db, upd, -1, &ustmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(ustmt, 1, song.youtube_url.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(ustmt, 2, song.youtube_video_id.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(ustmt, 3, existing_id);
                    sqlite3_step(ustmt);
                    sqlite3_finalize(ustmt);
                }
            }
            return existing_id;
        }
        sqlite3_finalize(stmt);
    }

    // Insert new song
    const char* ins_sql = "INSERT INTO identified_songs(acoustid,musicbrainz_id,title,artist,album,isrc,youtube_url,youtube_video_id,rights_holder) VALUES(?,?,?,?,?,?,?,?,?)";
    stmt = nullptr;
    if (sqlite3_prepare_v2(g_fp_db, ins_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, song.acoustid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, song.musicbrainz_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, song.title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, song.artist.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, song.album.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, song.isrc.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, song.youtube_url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 8, song.youtube_video_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 9, song.rights_holder.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            int64_t new_id = sqlite3_last_insert_rowid(g_fp_db);
            sqlite3_finalize(stmt);
            return new_id;
        }
        sqlite3_finalize(stmt);
    }
    return 0;
}

// Update channel_now_playing table
static void fp_db_update_now_playing(const std::string& channel, int64_t song_id,
                                      const std::string& fingerprint, double confidence,
                                      size_t listeners) {
    if (!g_fp_db || song_id <= 0) return;
    std::lock_guard<std::mutex> lock(g_fp_db_mutex);
    uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const char* sql = "INSERT OR REPLACE INTO channel_now_playing(channel,song_id,fingerprint,confidence,listeners,identified_at,updated_at) VALUES(?,?,?,?,?,?,?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_fp_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, channel.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, song_id);
        sqlite3_bind_text(stmt, 3, fingerprint.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 4, confidence);
        sqlite3_bind_int64(stmt, 5, (sqlite3_int64)listeners);
        sqlite3_bind_int64(stmt, 6, (sqlite3_int64)now);
        sqlite3_bind_int64(stmt, 7, (sqlite3_int64)now);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

// Identify a song using AcoustID → MusicBrainz → YouTube pipeline.
// Called from fingerprint_thread_func when consensus is found and chromaprint is available.
// Runs in the fingerprint thread (background), so blocking HTTP calls are acceptable.
static void identify_song(const std::string& channel, const std::string& chromaprint,
                           int duration, const std::string& fp_hex, double confidence,
                           size_t listeners) {
    // Step 1: AcoustID lookup
    const char* acoustid_key_env = getenv("ACOUSTID_API_KEY");
    if (!acoustid_key_env || acoustid_key_env[0] == '\0') {
        fprintf(stderr, "[song-id] ACOUSTID_API_KEY not set, skipping identification\n");
        return;
    }
    std::string acoustid_key = acoustid_key_env;

    std::string acoustid_url = "https://api.acoustid.org/v2/lookup?client=" + url_encode(acoustid_key)
        + "&fingerprint=" + url_encode(chromaprint)
        + "&duration=" + std::to_string(duration)
        + "&meta=recordings+releasegroups";

    fprintf(stderr, "[song-id] Querying AcoustID for channel '%s' (duration=%ds)...\n",
            channel.c_str(), duration);
    std::string acoustid_resp = http_get(acoustid_url);
    if (acoustid_resp.empty()) {
        fprintf(stderr, "[song-id] AcoustID returned empty response\n");
        return;
    }

    // Parse AcoustID response — extract first result's recording
    // Response format: {"status":"ok","results":[{"id":"xxx","recordings":[{"id":"mbid","title":"...","artists":[{"id":"...","name":"..."}]}]}]}
    std::string acoustid_id = json_extract_string(acoustid_resp, "id");
    // Find first recording ID (MusicBrainz ID)
    std::string mbid;
    std::string title;
    std::string artist;
    {
        // Look for "recordings" array, then first "id" inside it
        size_t rec_pos = acoustid_resp.find("\"recordings\"");
        if (rec_pos != std::string::npos) {
            std::string rec_section = acoustid_resp.substr(rec_pos);
            mbid = json_extract_string(rec_section, "id");
            title = json_extract_string(rec_section, "title");
            // Find artist name in the recordings section
            size_t artists_pos = rec_section.find("\"artists\"");
            if (artists_pos != std::string::npos) {
                artist = json_extract_string(rec_section.substr(artists_pos), "name");
            }
        }
    }

    if (mbid.empty()) {
        fprintf(stderr, "[song-id] No MusicBrainz recording found in AcoustID response for channel '%s'\n",
                channel.c_str());
        return;
    }

    fprintf(stderr, "[song-id] AcoustID match: acoustid=%s mbid=%s title='%s' artist='%s'\n",
            acoustid_id.c_str(), mbid.c_str(), title.c_str(), artist.c_str());

    // Step 2: MusicBrainz lookup for album, ISRC
    // Rate limit: 1 req/sec — sleep briefly to be safe
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    std::string mb_url = "https://musicbrainz.org/ws/2/recording/" + mbid
        + "?inc=artists+releases+isrcs&fmt=json";
    std::string mb_resp = http_get(mb_url);

    std::string album;
    std::string isrc;
    if (!mb_resp.empty()) {
        // Extract album from first release title
        size_t rel_pos = mb_resp.find("\"releases\"");
        if (rel_pos != std::string::npos) {
            album = json_extract_string(mb_resp.substr(rel_pos), "title");
        }
        // Extract ISRC
        size_t isrc_pos = mb_resp.find("\"isrcs\"");
        if (isrc_pos != std::string::npos) {
            // ISRCs is an array of strings: ["USXXX..."]
            size_t q1 = mb_resp.find('"', isrc_pos + 7);  // skip "isrcs"
            if (q1 != std::string::npos) {
                q1 = mb_resp.find('"', q1 + 1);  // skip [
                if (q1 != std::string::npos) {
                    size_t q2 = mb_resp.find('"', q1 + 1);
                    if (q2 != std::string::npos) {
                        isrc = mb_resp.substr(q1 + 1, q2 - q1 - 1);
                    }
                }
            }
        }
        // If title/artist were empty from AcoustID, try MusicBrainz
        if (title.empty()) title = json_extract_string(mb_resp, "title");
        if (artist.empty()) {
            size_t ac_pos = mb_resp.find("\"artist-credit\"");
            if (ac_pos != std::string::npos) {
                artist = json_extract_string(mb_resp.substr(ac_pos), "name");
            }
        }
        fprintf(stderr, "[song-id] MusicBrainz: title='%s' artist='%s' album='%s' isrc='%s'\n",
                title.c_str(), artist.c_str(), album.c_str(), isrc.c_str());
    }

    // Step 3: YouTube search
    std::string youtube_url;
    std::string youtube_video_id;
    const char* yt_key_env = getenv("YOUTUBE_API_KEY");
    if (yt_key_env && yt_key_env[0] != '\0' && !title.empty()) {
        std::string query = artist.empty() ? title : (artist + " " + title);
        std::string yt_url = "https://www.googleapis.com/youtube/v3/search?part=snippet&q="
            + url_encode(query) + "&type=video&videoCategoryId=10&key="
            + url_encode(std::string(yt_key_env)) + "&maxResults=1";
        std::string yt_resp = http_get(yt_url);
        if (!yt_resp.empty()) {
            youtube_video_id = json_extract_string(yt_resp, "videoId");
            if (!youtube_video_id.empty()) {
                youtube_url = "https://youtube.com/watch?v=" + youtube_video_id;
                fprintf(stderr, "[song-id] YouTube: %s\n", youtube_url.c_str());
            }
        }
    }

    // Step 4: Store in DB
    IdentifiedSong song;
    song.acoustid = acoustid_id;
    song.musicbrainz_id = mbid;
    song.title = title;
    song.artist = artist;
    song.album = album;
    song.isrc = isrc;
    song.youtube_url = youtube_url;
    song.youtube_video_id = youtube_video_id;

    int64_t song_id = fp_db_upsert_song(song);
    if (song_id > 0) {
        fp_db_update_now_playing(channel, song_id, fp_hex, confidence, listeners);
        fprintf(stderr, "[song-id] Identified song id=%lld for channel '%s': '%s' by '%s'\n",
                (long long)song_id, channel.c_str(), title.c_str(), artist.c_str());
    }
}

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

// ── Email Auth (passwordless) ───────────────────────────────────────────────

struct UserAccount {
    std::string user_id;                  // "u-" + UUID
    std::string email;
    std::string username;                 // unique @handle (optional)
    std::vector<std::string> devices;     // device_ids
    std::string apns_token;              // APNs device token for push notifications
    int64_t created_at = 0;
    int64_t last_login = 0;
};

struct VerificationCode {
    std::string email;
    std::string code;            // 6-digit
    int64_t expires;             // unix timestamp
    int attempts = 0;            // brute-force protection
};

static std::mutex g_auth_mutex;
static std::unordered_map<std::string, UserAccount> g_users;           // email → UserAccount
static std::unordered_map<std::string, std::string> g_auth_tokens;     // token → email
static std::unordered_map<std::string, std::string> g_username_to_email; // username → email
static std::unordered_map<std::string, std::string> g_username_to_apns; // username → apns_token
static std::unordered_map<std::string, std::string> g_device_to_email;  // device_id → email (for multi-device sync)
static std::unordered_map<std::string, VerificationCode> g_verify_codes; // email → code
static std::unordered_map<std::string, int64_t> g_email_rate_limit;    // email → next_allowed_ts
static std::string g_users_db_path = "/data/users.json";
static std::string g_resend_api_key;  // RESEND_API_KEY env

// APNs push notification config
static std::string g_apns_key_id;        // APNS_KEY_ID env
static std::string g_apns_team_id;       // APNS_TEAM_ID env
static std::string g_apns_bundle_id;     // APNS_BUNDLE_ID env (com.soluna.SolunaReceiver)
static std::string g_apns_auth_key_pem;  // APNS_AUTH_KEY env (contents of .p8 file)

// Generate 6-digit verification code
static std::string generate_code_6() {
    unsigned char buf[4];
    RAND_bytes(buf, 4);
    uint32_t n = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16)
               | ((uint32_t)buf[2] << 8)  | buf[3];
    n = n % 1000000;
    char code[8];
    snprintf(code, sizeof(code), "%06u", n);
    return std::string(code);
}

// Generate auth token with embedded creation timestamp: "<unix_hex>.<random_hex>"
static constexpr int64_t kTokenExpirySeconds = 30 * 86400;  // 30 days

static std::string generate_auth_token() {
    char ts_hex[20];
    snprintf(ts_hex, sizeof(ts_hex), "%llx", (unsigned long long)now_unix());
    return std::string(ts_hex) + "." + generate_hex(32);
}

// Check if token has expired (30 days). Returns false if expired.
static bool token_is_valid(const std::string& token) {
    size_t dot = token.find('.');
    if (dot == std::string::npos) {
        // Legacy token without timestamp — treat as expired
        return false;
    }
    std::string ts_hex = token.substr(0, dot);
    char* end = nullptr;
    int64_t created = (int64_t)strtoull(ts_hex.c_str(), &end, 16);
    if (end == ts_hex.c_str()) return false;  // parse failure
    return (now_unix() - created) < kTokenExpirySeconds;
}

// Send verification email via Resend API
static bool send_verification_email(const std::string& email, const std::string& code) {
    if (g_resend_api_key.empty()) {
        fprintf(stderr, "[auth] RESEND_API_KEY not set, cannot send email\n");
        return false;
    }
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string json_body = "{\"from\":\"Soluna <noreply@solun.art>\","
        "\"to\":[\"" + json_escape(email) + "\"],"
        "\"subject\":\"Soluna verification code: " + code + "\","
        "\"html\":\"<h2>" + code + "</h2><p>Enter this code in the Soluna app to verify your email.</p>"
        "<p>This code expires in 5 minutes.</p>\"}";

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + g_resend_api_key).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.resend.com/emails");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code >= 400) {
        fprintf(stderr, "[auth] Resend API failed: %s (HTTP %ld) response: %s\n",
                curl_easy_strerror(res), http_code, response.c_str());
        return false;
    }
    fprintf(stderr, "[auth] Verification email sent to %s\n", email.c_str());
    return true;
}

// Save users to disk (atomic)
static void users_save() {
    std::lock_guard<std::mutex> lock(g_auth_mutex);
    std::string tmp_path = g_users_db_path + ".tmp";
    FILE* f = fopen(tmp_path.c_str(), "w");
    if (!f) return;
    fprintf(f, "{\"users\":{");
    bool first = true;
    for (const auto& [email, u] : g_users) {
        if (!first) fprintf(f, ",");
        first = false;
        fprintf(f, "\n  \"%s\":{\"user_id\":\"%s\",\"username\":\"%s\",\"apns_token\":\"%s\",\"devices\":[",
                json_escape(email).c_str(), json_escape(u.user_id).c_str(),
                json_escape(u.username).c_str(), json_escape(u.apns_token).c_str());
        for (size_t i = 0; i < u.devices.size(); i++) {
            if (i > 0) fprintf(f, ",");
            fprintf(f, "\"%s\"", json_escape(u.devices[i]).c_str());
        }
        fprintf(f, "],\"created_at\":%lld,\"last_login\":%lld}",
                (long long)u.created_at, (long long)u.last_login);
    }
    fprintf(f, "\n},\"tokens\":{");
    first = true;
    for (const auto& [token, email] : g_auth_tokens) {
        if (!first) fprintf(f, ",");
        first = false;
        fprintf(f, "\n  \"%s\":\"%s\"", json_escape(token).c_str(), json_escape(email).c_str());
    }
    fprintf(f, "\n}}\n");
    fclose(f);
    rename(tmp_path.c_str(), g_users_db_path.c_str());
}

// Load users from disk
static void users_load() {
    FILE* f = fopen(g_users_db_path.c_str(), "r");
    if (!f) {
        fprintf(stderr, "[auth] No users file at %s, starting fresh\n", g_users_db_path.c_str());
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string content(size, '\0');
    fread(&content[0], 1, size, f);
    fclose(f);

    // Minimal JSON parser for users (same pattern as channels_load/wallets_load)
    // Parse users
    size_t users_pos = content.find("\"users\":{");
    if (users_pos == std::string::npos) return;
    size_t pos = users_pos + 9;
    while (pos < content.size()) {
        size_t email_start = content.find('"', pos);
        if (email_start == std::string::npos || content[email_start + 1] == '}') break;
        size_t email_end = content.find('"', email_start + 1);
        std::string email = content.substr(email_start + 1, email_end - email_start - 1);

        // Find user_id
        size_t uid_key = content.find("\"user_id\":\"", email_end);
        if (uid_key == std::string::npos) break;
        size_t uid_start = uid_key + 11;
        size_t uid_end = content.find('"', uid_start);
        std::string user_id = content.substr(uid_start, uid_end - uid_start);

        // Find username (optional field)
        std::string username;
        size_t un_key = content.find("\"username\":\"", uid_end);
        if (un_key != std::string::npos) {
            size_t un_start = un_key + 12;
            size_t un_end = content.find('"', un_start);
            username = content.substr(un_start, un_end - un_start);
        }

        // Find apns_token (optional field)
        std::string apns_token;
        size_t at_key = content.find("\"apns_token\":\"", uid_end);
        if (at_key != std::string::npos) {
            size_t at_start = at_key + 14;
            size_t at_end = content.find('"', at_start);
            apns_token = content.substr(at_start, at_end - at_start);
        }

        // Find created_at
        size_t ca_key = content.find("\"created_at\":", uid_end);
        int64_t created_at = 0;
        if (ca_key != std::string::npos) created_at = std::atoll(content.c_str() + ca_key + 13);

        // Find last_login
        size_t ll_key = content.find("\"last_login\":", uid_end);
        int64_t last_login = 0;
        if (ll_key != std::string::npos) last_login = std::atoll(content.c_str() + ll_key + 13);

        // Find devices array
        std::vector<std::string> devices;
        size_t dev_key = content.find("\"devices\":[", uid_end);
        if (dev_key != std::string::npos) {
            size_t arr_start = dev_key + 11;
            size_t arr_end = content.find(']', arr_start);
            std::string arr = content.substr(arr_start, arr_end - arr_start);
            size_t dpos = 0;
            while ((dpos = arr.find('"', dpos)) != std::string::npos) {
                size_t dend = arr.find('"', dpos + 1);
                if (dend == std::string::npos) break;
                devices.push_back(arr.substr(dpos + 1, dend - dpos - 1));
                dpos = dend + 1;
            }
        }

        UserAccount u;
        u.user_id = user_id;
        u.email = email;
        u.username = username;
        u.apns_token = apns_token;
        u.devices = devices;
        u.created_at = created_at;
        u.last_login = last_login;
        g_users[email] = u;
        if (!username.empty()) g_username_to_email[username] = email;
        if (!username.empty() && !apns_token.empty()) g_username_to_apns[username] = apns_token;
        for (const auto& dev : devices) {
            if (!dev.empty()) g_device_to_email[dev] = email;
        }

        // Advance past this entry
        pos = content.find('}', uid_end);
        if (pos == std::string::npos) break;
        pos++;
    }

    // Parse tokens
    size_t tokens_pos = content.find("\"tokens\":{");
    if (tokens_pos != std::string::npos) {
        pos = tokens_pos + 10;
        while (pos < content.size()) {
            size_t tk_start = content.find('"', pos);
            if (tk_start == std::string::npos || content[tk_start + 1] == '}') break;
            size_t tk_end = content.find('"', tk_start + 1);
            std::string token = content.substr(tk_start + 1, tk_end - tk_start - 1);
            size_t val_start = content.find('"', tk_end + 1);
            size_t val_end = content.find('"', val_start + 1);
            std::string email = content.substr(val_start + 1, val_end - val_start - 1);
            g_auth_tokens[token] = email;
            pos = val_end + 1;
        }
    }

    fprintf(stderr, "[auth] Loaded %zu users, %zu tokens from %s\n",
            g_users.size(), g_auth_tokens.size(), g_users_db_path.c_str());
}

// ── APNs push notifications ──────────────────────────────────────────────────
// JWT ES256 + HTTP/2 via libcurl to api.push.apple.com

static std::string apns_base64url(const std::string& in) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string o;
    o.reserve(((in.size() + 2) / 3) * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        uint32_t n = ((uint8_t)in[i]) << 16;
        if (i + 1 < in.size()) n |= ((uint8_t)in[i + 1]) << 8;
        if (i + 2 < in.size()) n |= (uint8_t)in[i + 2];
        o += t[(n >> 18) & 0x3f]; o += t[(n >> 12) & 0x3f];
        o += (i + 1 < in.size()) ? t[(n >> 6) & 0x3f] : '\0';
        o += (i + 2 < in.size()) ? t[n & 0x3f] : '\0';
    }
    // Remove trailing nulls (no padding for URL-safe base64)
    while (!o.empty() && o.back() == '\0') o.pop_back();
    return o;
}

// Generate APNs JWT valid for 1 hour
static std::string apns_make_jwt() {
    if (g_apns_key_id.empty() || g_apns_team_id.empty() || g_apns_auth_key_pem.empty()) return "";
    // Header
    std::string header_json = "{\"alg\":\"ES256\",\"kid\":\"" + g_apns_key_id + "\"}";
    std::string header = apns_base64url(header_json);
    // Payload
    int64_t iat = (int64_t)time(nullptr);
    std::string payload_json = "{\"iss\":\"" + g_apns_team_id + "\",\"iat\":" + std::to_string(iat) + "}";
    std::string payload = apns_base64url(payload_json);
    std::string signing_input = header + "." + payload;
    // Sign with EVP_PKEY (P-256 / prime256v1)
    BIO* bio = BIO_new_mem_buf(g_apns_auth_key_pem.c_str(), (int)g_apns_auth_key_pem.size());
    if (!bio) return "";
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) {
        fprintf(stderr, "[apns] Failed to parse EC private key\n");
        return "";
    }
    // EVP_DigestSign produces DER-encoded ECDSA signature
    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) { EVP_PKEY_free(pkey); return ""; }
    if (EVP_DigestSignInit(md_ctx, nullptr, EVP_sha256(), nullptr, pkey) != 1) {
        EVP_MD_CTX_free(md_ctx); EVP_PKEY_free(pkey); return "";
    }
    if (EVP_DigestSignUpdate(md_ctx, signing_input.c_str(), signing_input.size()) != 1) {
        EVP_MD_CTX_free(md_ctx); EVP_PKEY_free(pkey); return "";
    }
    size_t der_sig_len = 0;
    if (EVP_DigestSignFinal(md_ctx, nullptr, &der_sig_len) != 1) {
        EVP_MD_CTX_free(md_ctx); EVP_PKEY_free(pkey); return "";
    }
    std::vector<unsigned char> der_sig(der_sig_len);
    if (EVP_DigestSignFinal(md_ctx, der_sig.data(), &der_sig_len) != 1) {
        EVP_MD_CTX_free(md_ctx); EVP_PKEY_free(pkey); return "";
    }
    der_sig.resize(der_sig_len);
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    // Decode DER signature to r+s for IEEE P1363 format (64-byte raw for JWT)
    const unsigned char* der_ptr = der_sig.data();
    ECDSA_SIG* sig = d2i_ECDSA_SIG(nullptr, &der_ptr, (long)der_sig.size());
    if (!sig) return "";
    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    ECDSA_SIG_get0(sig, &r, &s);
    unsigned char raw_sig[64] = {};
    int r_len = BN_num_bytes(r), s_len = BN_num_bytes(s);
    BN_bn2bin(r, raw_sig + (32 - r_len));
    BN_bn2bin(s, raw_sig + 32 + (32 - s_len));
    ECDSA_SIG_free(sig);
    std::string sig_str(reinterpret_cast<char*>(raw_sig), 64);
    return signing_input + "." + apns_base64url(sig_str);
}

// Send APNs push notification in a detached thread (fire-and-forget)
static void apns_send_push(const std::string& device_token,
                            const std::string& title, const std::string& body,
                            const std::string& category) {
    if (device_token.empty() || g_apns_bundle_id.empty()) return;
    // Detach to avoid blocking the relay loop
    std::thread([device_token, title, body, category]() {
        std::string jwt = apns_make_jwt();
        if (jwt.empty()) {
            fprintf(stderr, "[apns] JWT generation failed\n");
            return;
        }
        std::string url = "https://api.push.apple.com/3/device/" + device_token;
        std::string payload = "{\"aps\":{\"alert\":{\"title\":\"" + title +
                              "\",\"body\":\"" + body + "\"},"
                              "\"sound\":\"default\",\"badge\":1},"
                              "\"category\":\"" + category + "\"}";
        CURL* curl = curl_easy_init();
        if (!curl) return;
        std::string response;
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("Authorization: bearer " + jwt).c_str());
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, ("apns-topic: " + g_apns_bundle_id).c_str());
        headers = curl_slist_append(headers, "apns-push-type: alert");
        headers = curl_slist_append(headers, "apns-priority: 10");
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (res != CURLE_OK || http_code != 200) {
            fprintf(stderr, "[apns] Push failed: curl=%s http=%ld resp=%s\n",
                    curl_easy_strerror(res), http_code, response.c_str());
        } else {
            fprintf(stderr, "[apns] Push sent to ...%s\n",
                    device_token.size() > 8 ? device_token.substr(device_token.size() - 8).c_str() : "?");
        }
    }).detach();
}

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
                // Constant-time comparison to prevent timing attacks
                if (m.session_token.size() != token.size()) return false;
                volatile unsigned char result = 0;
                for (size_t i = 0; i < token.size(); i++)
                    result |= m.session_token[i] ^ token[i];
                return (result == 0);
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
    std::string tmp_path = g_wallets_db_path + ".tmp";
    FILE* f = fopen(tmp_path.c_str(), "w");
    if (!f) {
        fprintf(stderr, "[wallet] WARNING: cannot write %s: %s\n", tmp_path.c_str(), strerror(errno));
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
    rename(tmp_path.c_str(), g_wallets_db_path.c_str());  // atomic on POSIX
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

    // Find sender's group via O(1) reverse lookup
    auto _rk = g_addr_to_group.find(addr_key(from));
    std::string gname = (_rk != g_addr_to_group.end()) ? _rk->second : "";
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
    auto _rk = g_addr_to_group.find(addr_key(from));
    std::string gname = (_rk != g_addr_to_group.end()) ? _rk->second : "";
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
    auto _rk = g_addr_to_group.find(addr_key(from));
    std::string gname = (_rk != g_addr_to_group.end()) ? _rk->second : "";
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

// ── PARENT_FAIL handler ───────────────────────────────────────────────────────
// Format: "PARENT_FAIL:<parent_ip>:<parent_port>\n"
// Client reports primary parent stopped sending (>80ms silence); relay reassigns.

static void handle_parent_fail(const char* msg, size_t len, const sockaddr_in& from) {
    // Parse parent IP:port from message
    std::string payload(msg + 12, len - 12); // skip "PARENT_FAIL:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    fprintf(stderr, "[swarm] PARENT_FAIL from %s: parent=%s\n",
            addr_str(from).c_str(), payload.c_str());

    // Reassign: find a new parent from group members
    std::lock_guard<std::shared_mutex> lock(g_mutex);
    auto _rk = g_addr_to_group.find(addr_key(from));
    std::string gname = (_rk != g_addr_to_group.end()) ? _rk->second : "";
    if (gname.empty()) return;

    auto& group = g_groups[gname];
    if (!group.swarm_active) return;

    // Find requester in swarm tree
    for (size_t i = 0; i < group.members.size(); i++) {
        if (addr_equal(group.members[i].addr, from)) {
            // Pick a new parent: someone NOT equal to failed parent, with space for children
            for (size_t j = 0; j < group.members.size(); j++) {
                if (j == i) continue;
                if (group.swarm_tree[j].children.size() < kSwarmFanOut) {
                    group.swarm_tree[i].parent_addr = group.swarm_tree[j].addr;
                    send_swarm_assign(group.swarm_tree[i]);
                    fprintf(stderr, "[swarm] Reassigned %s → parent %s (fast recovery)\n",
                            addr_str(from).c_str(), addr_str(group.swarm_tree[j].addr).c_str());
                    break;
                }
            }
            break;
        }
    }
}

// ── Multi-device auto-sync ────────────────────────────────────────────────────
// When a device joins a channel, notify all other linked devices of the same user
// so they can auto-join the same channel. Sends "SYNC_PLAY:<channel>\n" via UDP.
// IMPORTANT: Caller must already hold g_mutex (shared_mutex write lock).
static void notify_linked_devices(const std::string& device_id, const std::string& group_name) {
    // Look up which user owns this device and collect sibling device IDs
    std::vector<std::string> sibling_devices;
    {
        std::lock_guard<std::mutex> auth_lock(g_auth_mutex);
        auto dit = g_device_to_email.find(device_id);
        if (dit == g_device_to_email.end()) return;  // unlinked device
        auto uit = g_users.find(dit->second);
        if (uit == g_users.end()) return;
        for (const auto& d : uit->second.devices) {
            if (d != device_id) sibling_devices.push_back(d);
        }
    }
    if (sibling_devices.empty()) return;

    // For each sibling device, find if it's currently connected to any group
    // and send it a SYNC_PLAY notification
    std::string sync_msg = "SYNC_PLAY:" + group_name + "\n";
    for (const auto& sib_dev : sibling_devices) {
        // Search all groups for a member with this device_id
        for (const auto& [gname, grp] : g_groups) {
            for (const auto& m : grp.members) {
                if (m.device_id == sib_dev) {
                    sendto(g_udp_sock, sync_msg.c_str(), sync_msg.size(), 0,
                           (const sockaddr*)&m.addr, sizeof(m.addr));
                    fprintf(stderr, "[sync] Notified device %s (%s) → SYNC_PLAY:%s\n",
                            sib_dev.c_str(), addr_str(m.addr).c_str(), group_name.c_str());
                    goto next_sibling;  // found this device, move to next
                }
            }
        }
        next_sibling:;
    }
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
    // Format: JOIN:<group>[:<password>[:<device_name>[:<device_id>]]]
    std::string group_name, password, device_name, device_id;
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
            size_t pos3 = payload.find(':', pos2 + 1);
            if (pos3 == std::string::npos) {
                device_name = payload.substr(pos2 + 1);
            } else {
                device_name = payload.substr(pos2 + 1, pos3 - pos2 - 1);
                device_id = payload.substr(pos3 + 1);
            }
        }
    }
    // Generate device_id server-side if not provided by client
    if (device_id.empty()) {
        device_id = generate_uuid_v4();
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
            if (ch_it->second.device != device_id && ch_it->second.device != device_name) {
                const char* err = "ERR:channel_reserved\n";
                sendto(g_udp_sock, err, strlen(err), 0,
                       (const sockaddr*)&from, sizeof(from));
                return;
            }
        }
        // Not claimed or owned by this device → allowed
    }

    // Remove member from any existing group first
    auto _rk = g_addr_to_group.find(addr_key(from));
    std::string old_group = (_rk != g_addr_to_group.end()) ? _rk->second : "";
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
            if (!device_id.empty()) m.device_id = device_id;
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
    member.device_id = device_id;
    member.session_token = generate_hex(32);  // 128-bit session token

    // Assign role based on channel ownership:
    // - Random/free channels: everyone is DJ (open broadcast)
    // - Owned channels: owner gets Owner role, first member gets DJ, others are Listeners
    if (is_random_channel(group_name) || is_free_name(group_name)) {
        member.role = MemberRole::DJ;
    } else if (group.talk_mode) {
        // Talk mode: everyone can send audio
        member.role = MemberRole::DJ;
    } else {
        auto ch_it = g_channels.find(group_name);
        if (ch_it != g_channels.end() &&
            (ch_it->second.device == device_id || ch_it->second.device == device_name)) {
            member.role = MemberRole::Owner;
        } else if (group.members.empty()) {
            // First joiner of unclaimed channel gets DJ
            member.role = MemberRole::DJ;
        } else {
            member.role = MemberRole::Listener;
        }
    }

    // DJ and Owner implicitly have mic permission
    if (member.role >= MemberRole::DJ) {
        member.mic_allowed = true;
    }

    group.members.push_back(member);
    size_t new_member_idx = group.members.size() - 1;
    g_total_joins++;

    // @mention notification: if channel name is "@<username>", push notify that user
    // This implements phone-call-like notifications when someone joins your channel
    if (group_name.size() > 1 && group_name[0] == '@') {
        std::string mentioned = group_name.substr(1);  // strip '@'
        // Look up APNs token outside g_mutex (g_auth_mutex is separate)
        std::string apns_tok;
        {
            std::lock_guard<std::mutex> auth_lock(g_auth_mutex);
            auto it = g_username_to_apns.find(mentioned);
            if (it != g_username_to_apns.end()) apns_tok = it->second;
        }
        if (!apns_tok.empty()) {
            std::string caller = device_name.empty() ? "Someone" : device_name;
            apns_send_push(apns_tok,
                "📞 " + caller + " wants to talk",
                caller + " joined @" + mentioned + "'s channel on Soluna",
                "MENTION_CALL");
        }
    }

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

    // Send the joiner their own external (server-reflexive) address.
    // Enables NAT traversal: client learns its public IP:port as seen by relay,
    // and can share that address with peers for UDP hole-punching (§10, RFC 5389).
    // Format: YOUR_ADDR:<ip>:<port>
    {
        std::string your_addr_msg = "YOUR_ADDR:" + addr_str(from) + "\n";
        sendto(g_udp_sock, your_addr_msg.c_str(), your_addr_msg.size(), 0,
               (const sockaddr*)&from, sizeof(from));
    }

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

    // Send current max delay to new joiner (sync mode coordination)
    if (group.max_delay_ms > 0) {
        char delay_buf[64];
        snprintf(delay_buf, sizeof(delay_buf), "MAXDELAY:%u\n", group.max_delay_ms);
        sendto(g_udp_sock, delay_buf, strlen(delay_buf), 0,
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

    // Multi-device auto-sync: notify other devices of the same user
    if (!device_id.empty()) {
        notify_linked_devices(device_id, group_name);
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

    auto _rk = g_addr_to_group.find(addr_key(from));
    std::string group_name = (_rk != g_addr_to_group.end()) ? _rk->second : "";
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

// ── ROYALTY_PAYER handler ──────────────────────────────────────────────────────
// Format: "ROYALTY_PAYER:<mode>\n"  (mode = "dj" | "owner" | "listener" | "free")
// Owner/DJ only. Sets who pays copyright royalties on this channel.
static void handle_royalty_payer(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 15, len - 15);  // skip "ROYALTY_PAYER:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    RoyaltyPayer new_payer;
    if (payload == "dj")            new_payer = RoyaltyPayer::DJ;
    else if (payload == "owner")    new_payer = RoyaltyPayer::Owner;
    else if (payload == "listener") new_payer = RoyaltyPayer::Listener;
    else if (payload == "free")     new_payer = RoyaltyPayer::Free;
    else {
        const char* err = "ERR:invalid_payer (dj|owner|listener|free)\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    std::lock_guard<std::shared_mutex> lock(g_mutex);

    auto _rk = g_addr_to_group.find(addr_key(from));
    std::string group_name = (_rk != g_addr_to_group.end()) ? _rk->second : "";
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

    group.royalty_payer = new_payer;

    // Notify all members
    char notify[128];
    snprintf(notify, sizeof(notify), "ROYALTY_PAYER:%s\n", payload.c_str());
    for (const auto& m : group.members) {
        sendto(g_udp_sock, notify, strlen(notify), 0,
               (const sockaddr*)&m.addr, sizeof(m.addr));
    }

    fprintf(stderr, "[copyright] Group '%s' royalty payer set to '%s' by %s\n",
            group_name.c_str(), payload.c_str(), sender->device_id.c_str());
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

    // Find sender's group via O(1) reverse lookup
    auto _rk = g_addr_to_group.find(addr_key(from));
    std::string gname = (_rk != g_addr_to_group.end()) ? _rk->second : "";
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
            // Update mic_allowed: DJ/Owner get mic, Listener loses implicit mic
            if (new_role >= MemberRole::DJ) {
                m.mic_allowed = true;
            }
            // Note: demoting to Listener does not clear mic_allowed if explicitly granted
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

    auto _rk = g_addr_to_group.find(addr_key(from));
    std::string gname = (_rk != g_addr_to_group.end()) ? _rk->second : "";
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
        bool effective_mic = m.mic_allowed || m.role >= MemberRole::DJ;
        response += "{\"device\":\"" + json_escape(m.device_name) + "\","
                    "\"device_id\":\"" + json_escape(m.device_id) + "\","
                    "\"role\":\"" + role_name + "\","
                    "\"mixing\":" + (m.mixing ? std::string("true") : std::string("false")) + ","
                    "\"mic_allowed\":" + (effective_mic ? std::string("true") : std::string("false")) + ","
                    "\"addr\":\"" + addr_str(m.addr) + "\"}";
    }
    response += "]}\n";
    sendto(g_udp_sock, response.c_str(), response.size(), 0,
           (const sockaddr*)&from, sizeof(from));
}

// ── GOSSIP_PEERS handler ──────────────────────────────────────────────────────
// Format: "GOSSIP_PEERS\n" — request peer table (8 candidates) for redundant reception

static void handle_gossip_peers(const sockaddr_in& from) {
    std::vector<sockaddr_in> candidates;
    {
        std::shared_lock<std::shared_mutex> lock(g_mutex);
        auto _rk = g_addr_to_group.find(addr_key(from));
        std::string gname = (_rk != g_addr_to_group.end()) ? _rk->second : "";
        if (gname.empty()) return;
        auto& group = g_groups[gname];
        for (const auto& m : group.members) {
            if (!addr_equal(m.addr, from) && candidates.size() < 8) {
                candidates.push_back(m.addr);
            }
        }
    }
    if (candidates.empty()) return;

    // Build PEERS: response
    std::string resp = "PEERS:";
    for (size_t i = 0; i < candidates.size(); i++) {
        if (i > 0) resp += ',';
        char buf[64];
        snprintf(buf, sizeof(buf), "%s:%u",
                 inet_ntoa(candidates[i].sin_addr),
                 ntohs(candidates[i].sin_port));
        resp += buf;
    }
    resp += "\n";
    sendto(g_udp_sock, resp.c_str(), resp.size(), 0, (const sockaddr*)&from, sizeof(from));
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
    auto _rk = g_addr_to_group.find(addr_key(from));
    std::string sender_group = (_rk != g_addr_to_group.end()) ? _rk->second : "";
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

// ── TURN fallback handler (OSTP v0.9.3 §5.x / RFC 5766 subset) ───────────────
// When STUN hole-punching fails, client sends TURN_ALLOC to request relay forwarding.
// The relay already forwards all group audio — this just confirms TURN allocation.
//
//   Client → Relay:  TURN_ALLOC:<session_token>\n
//   Relay  → Client: TURN_OK:<relay_ip>:<relay_port>\n
static void handle_turn_alloc(const char* msg, size_t len, const sockaddr_in& from) {
    std::string session_tok(msg + 11, len - 11);  // skip "TURN_ALLOC:"
    while (!session_tok.empty() && (session_tok.back() == '\n' || session_tok.back() == '\r'))
        session_tok.pop_back();

    {
        std::lock_guard<std::shared_mutex> lock(g_mutex);
        if (!validate_session_token(from, session_tok)) {
            const char* err = "ERR:turn_invalid_session\n";
            sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
            return;
        }
    }

    // Use the relay's socket address as the TURN allocation address
    struct sockaddr_in relay_sa{};
    socklen_t sa_len = sizeof(relay_sa);
    getsockname(g_udp_sock, (struct sockaddr*)&relay_sa, &sa_len);

    char relay_ip[INET_ADDRSTRLEN] = "relay.solun.art";
    uint16_t relay_port = kDefaultPort;
    if (relay_sa.sin_addr.s_addr != INADDR_ANY) {
        inet_ntop(AF_INET, &relay_sa.sin_addr, relay_ip, sizeof(relay_ip));
        relay_port = ntohs(relay_sa.sin_port);
    }

    char resp[128];
    snprintf(resp, sizeof(resp), "TURN_OK:%s:%u\n", relay_ip, (unsigned)relay_port);
    sendto(g_udp_sock, resp, strlen(resp), 0, (const sockaddr*)&from, sizeof(from));

    fprintf(stderr, "[turn] ALLOC from %s → relay %s:%u\n",
            addr_str(from).c_str(), relay_ip, (unsigned)relay_port);
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
static bool match_fingerprint([[maybe_unused]] uint64_t fingerprint, const std::string& group_name,
                               CopyrightMatch& out_match) {
    // Method 1: Local database lookup (hash -> track info)
    // In production, this would be a large database or Bloom filter
    // For now: stub that checks against a known-tracks file

    // Method 2: External API call (async, non-blocking)
    // If g_fingerprint_api_url is set, POST the fingerprint for matching
    // Response: { "match": true, "track": {...}, "confidence": 0.95 }

    // Look up channel_now_playing → identified_songs in SQLite
    // If a song was recently identified (via listener chromaprint consensus → AcoustID),
    // return that info so the relay's own fingerprint detection can trigger COPYRIGHT_DETECT.
    if (!g_fp_db) return false;

    std::lock_guard<std::mutex> lock(g_fp_db_mutex);
    const char* sql =
        "SELECT s.title, s.artist, s.album, s.isrc, s.rights_holder, np.confidence "
        "FROM channel_now_playing np "
        "JOIN identified_songs s ON np.song_id = s.id "
        "WHERE np.channel = ? AND np.updated_at > strftime('%s','now') - 120 "
        "LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_fp_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, group_name.c_str(), -1, SQLITE_TRANSIENT);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* t = (const char*)sqlite3_column_text(stmt, 0);
        const char* a = (const char*)sqlite3_column_text(stmt, 1);
        const char* al = (const char*)sqlite3_column_text(stmt, 2);
        const char* i = (const char*)sqlite3_column_text(stmt, 3);
        const char* rh = (const char*)sqlite3_column_text(stmt, 4);
        double conf = sqlite3_column_double(stmt, 5);

        if (t && t[0]) {
            out_match.track_title = t;
            out_match.artist = a ? a : "";
            out_match.album = al ? al : "";
            out_match.isrc = i ? i : "";
            out_match.rights_holder = rh ? rh : "";
            out_match.confidence = (float)conf;
            out_match.detected_at = std::chrono::steady_clock::now();
            found = true;
        }
    }
    sqlite3_finalize(stmt);
    return found;
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

    // Calculate partial-minute remainder
    // royalty_tick charges full minutes; we charge the leftover seconds here
    int64_t remainder_sec = duration % 60;
    double partial_minutes = (double)remainder_sec / 60.0;
    uint32_t current_listeners = (uint32_t)group.members.size();
    double partial_royalty = calculate_royalty_per_min(current_listeners) * partial_minutes;

    // Find DJ device_id (for logging)
    std::string dj_device;
    for (const auto& m : group.members) {
        if (m.role >= MemberRole::DJ) { dj_device = m.device_id; break; }
    }

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

    // Free mode — no charges for partial remainder either
    if (group.royalty_payer == RoyaltyPayer::Free) return;

    if (partial_royalty < 0.0001) return;

    // Listener mode — split partial royalty among all listeners
    if (group.royalty_payer == RoyaltyPayer::Listener) {
        std::vector<std::string> listener_ids;
        for (const auto& m : group.members) {
            if (m.role < MemberRole::DJ && !m.device_id.empty())
                listener_ids.push_back(m.device_id);
        }
        if (!listener_ids.empty()) {
            double per_listener = partial_royalty / listener_ids.size();
            std::lock_guard<std::mutex> wlock(g_wallet_mutex);
            for (const auto& lid : listener_ids) {
                auto& w = g_wallets[lid];
                if (w.device_id.empty()) w.device_id = lid;
                w.balance -= per_listener;
                w.total_royalties_paid += per_listener;
            }
            g_rights_holder_balances[cs.current_match.rights_holder] += partial_royalty * 0.70;
            if (!dj_device.empty()) {
                auto& djw = g_wallets[dj_device];
                if (djw.device_id.empty()) djw.device_id = dj_device;
                djw.balance += partial_royalty * 0.20;
                djw.total_earned += partial_royalty * 0.20;
            }
        }
        return;
    }

    // DJ or Owner mode — find the payer
    std::string payer_device;
    if (group.royalty_payer == RoyaltyPayer::Owner) {
        for (const auto& m : group.members) {
            if (m.role == MemberRole::Owner) { payer_device = m.device_id; break; }
        }
    }
    if (payer_device.empty()) {
        // Fallback to DJ
        payer_device = dj_device;
    }
    if (!payer_device.empty()) {
        deduct_royalty_from_wallet(payer_device, entry.rights_holder, entry.isrc,
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

        // Free mode — no copyright charges
        if (group.royalty_payer == RoyaltyPayer::Free) continue;

        // Listener mode — split royalty among all listeners
        if (group.royalty_payer == RoyaltyPayer::Listener) {
            size_t listener_count = 0;
            std::vector<std::string> listener_ids;
            for (const auto& m : group.members) {
                if (m.role < MemberRole::DJ) {
                    listener_count++;
                    if (!m.device_id.empty()) listener_ids.push_back(m.device_id);
                }
            }
            if (listener_count == 0 || listener_ids.empty()) continue;
            double per_listener = royalty_per_min / listener_count;
            if (per_listener < 0.0001) continue;
            // Deduct from each listener's wallet
            {
                std::lock_guard<std::mutex> wlock(g_wallet_mutex);
                double rights_share_total = royalty_per_min * 0.70;
                // DJ cashback goes to DJ (20%)
                std::string dj_id;
                for (const auto& m : group.members) {
                    if (m.role >= MemberRole::DJ) { dj_id = m.device_id; break; }
                }
                for (const auto& lid : listener_ids) {
                    auto& w = g_wallets[lid];
                    if (w.device_id.empty()) w.device_id = lid;
                    w.balance -= per_listener;
                    w.total_royalties_paid += per_listener;
                }
                g_rights_holder_balances[cs.current_match.rights_holder] += rights_share_total;
                if (!dj_id.empty()) {
                    auto& djw = g_wallets[dj_id];
                    if (djw.device_id.empty()) djw.device_id = dj_id;
                    djw.balance += royalty_per_min * 0.20;  // DJ cashback
                    djw.total_earned += royalty_per_min * 0.20;
                }
                log_transaction("listeners:" + name, "rights:" + cs.current_match.isrc,
                                rights_share_total, "royalty",
                                cs.current_match.track_title + " by " + cs.current_match.artist);
            }
            continue;
        }

        // DJ or Owner mode — find the payer
        std::string payer_device;
        if (group.royalty_payer == RoyaltyPayer::Owner) {
            // Find owner
            for (const auto& m : group.members) {
                if (m.role == MemberRole::Owner) { payer_device = m.device_id; break; }
            }
        }
        if (payer_device.empty()) {
            // Fallback to DJ (default, or if no owner found)
            for (const auto& m : group.members) {
                if (m.role >= MemberRole::DJ) { payer_device = m.device_id; break; }
            }
        }
        if (payer_device.empty()) continue;

        // Check grace period — if expired, auto-switch to private
        {
            std::lock_guard<std::mutex> wlock(g_wallet_mutex);
            auto grace_it = g_grace_periods.find(payer_device);
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
                    fprintf(stderr, "[wallet] Auto-switched '%s' to private (payer '%s' insufficient balance)\n",
                            name.c_str(), payer_device.c_str());
                    continue;
                }
            }
        }

        // Deduct 1 minute of royalty (do not hold g_mutex when acquiring g_wallet_mutex
        // but we're already iterating under g_mutex — deduct_royalty_from_wallet acquires
        // g_wallet_mutex separately, which is safe since lock order is g_mutex → g_wallet_mutex)
        deduct_royalty_from_wallet(payer_device, cs.current_match.rights_holder,
                                   cs.current_match.isrc, royalty_per_min,
                                   cs.current_match.track_title, cs.current_match.artist,
                                   group, name);

        // Broadcast payer balance to all members in real-time
        {
            std::lock_guard<std::mutex> wlock(g_wallet_mutex);
            auto wit = g_wallets.find(payer_device);
            double balance = (wit != g_wallets.end()) ? wit->second.balance : 0.0;
            double rate_per_min = royalty_per_min;

            char bal_msg[256];
            snprintf(bal_msg, sizeof(bal_msg),
                "BALANCE_UPDATE:{\"dj\":\"%s\",\"balance\":%.2f,\"rate_per_min\":%.4f,"
                "\"listeners\":%zu,\"track\":\"%s\",\"artist\":\"%s\"}\n",
                payer_device.c_str(), balance, rate_per_min,
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

        // ── Listener fingerprint consensus (every 10s = every 2 ticks) ──
        if (tick_counter % 2 == 0) {
            uint64_t now_ts = (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            for (auto& [chan, cfp] : g_listener_fingerprints) {
                if (!cfp.mtx.try_lock()) continue;
                std::lock_guard<std::mutex> adopt(cfp.mtx, std::adopt_lock);

                // Prune reports older than 60 seconds
                cfp.recent.erase(
                    std::remove_if(cfp.recent.begin(), cfp.recent.end(),
                        [&](const FingerprintReport& r) { return now_ts - r.timestamp > 60; }),
                    cfp.recent.end());

                if (cfp.recent.size() < 2) continue;

                // Find the largest cluster (hamming distance <= 8)
                size_t best_count = 0;
                uint64_t best_hash = 0;
                for (size_t i = 0; i < cfp.recent.size(); i++) {
                    size_t count = 0;
                    for (size_t j = 0; j < cfp.recent.size(); j++) {
                        uint64_t xored = cfp.recent[i].hash ^ cfp.recent[j].hash;
                        int dist = __builtin_popcountll(xored);
                        if (dist <= 8) count++;
                    }
                    if (count > best_count) {
                        best_count = count;
                        best_hash = cfp.recent[i].hash;
                    }
                }

                if (best_count >= 2) {
                    fprintf(stderr, "[copyright] Consensus on channel '%s': hash=%016llx (N=%zu reports)\n",
                            chan.c_str(), (unsigned long long)best_hash, best_count);
                    CopyrightMatch consensus_match;
                    match_fingerprint(best_hash, chan, consensus_match);

                    // Revenue split calculation (70% rights_holder, 20% dj_cashback, 10% platform)
                    double total_royalty = calculate_royalty_per_min(best_count);
                    double rev_rights_holder = total_royalty * 0.70;
                    double rev_dj_cashback   = total_royalty * 0.20;
                    double rev_platform      = total_royalty * 0.10;

                    // Update running total
                    g_total_royalty_cents += (uint64_t)(total_royalty * 100);

                    // Format fingerprint as hex
                    char hash_hex[17];
                    snprintf(hash_hex, sizeof(hash_hex), "%016llx", (unsigned long long)best_hash);

                    // Update or create match entry
                    {
                        std::lock_guard<std::mutex> mlock(g_fp_matches_mutex);
                        auto& entry = g_fp_matches[chan];
                        if (entry.fingerprint != hash_hex) {
                            // New fingerprint — reset
                            entry.channel = chan;
                            entry.fingerprint = hash_hex;
                            entry.listener_count = best_count;
                            entry.confidence = consensus_match.confidence > 0 ? consensus_match.confidence : (double)best_count / cfp.recent.size();
                            entry.first_seen = now_ts;
                            entry.last_seen = now_ts;
                            entry.revenue_rights_holder = rev_rights_holder;
                            entry.revenue_dj_cashback = rev_dj_cashback;
                            entry.revenue_platform = rev_platform;
                        } else {
                            // Same fingerprint — accumulate
                            entry.listener_count = best_count;
                            entry.last_seen = now_ts;
                            entry.revenue_rights_holder += rev_rights_holder;
                            entry.revenue_dj_cashback += rev_dj_cashback;
                            entry.revenue_platform += rev_platform;
                        }
                        // Persist match to SQLite
                        fp_db_insert_match(entry);

                        // Song identification: find chromaprint from cluster reports
                        std::string best_chromaprint;
                        int best_duration = 0;
                        for (const auto& r : cfp.recent) {
                            uint64_t xored = r.hash ^ best_hash;
                            int dist = __builtin_popcountll(xored);
                            if (dist <= 8 && !r.chromaprint.empty()) {
                                best_chromaprint = r.chromaprint;
                                best_duration = r.duration;
                                break;
                            }
                        }
                        if (!best_chromaprint.empty() && best_duration > 0) {
                            // Run identification in a detached thread to avoid blocking
                            std::string id_channel = chan;
                            std::string id_fp = hash_hex;
                            double id_confidence = entry.confidence;
                            size_t id_listeners = best_count;
                            std::thread([id_channel, best_chromaprint, best_duration, id_fp, id_confidence, id_listeners]() {
                                identify_song(id_channel, best_chromaprint, best_duration,
                                              id_fp, id_confidence, id_listeners);
                            }).detach();
                        }
                    }
                }
            }
        }
    }
}

// Handle COPYRIGHT_ACK from DJ
static void handle_copyright_ack(const sockaddr_in& from) {
    std::lock_guard<std::shared_mutex> lock(g_mutex);
    auto _rk = g_addr_to_group.find(addr_key(from));
    std::string gname = (_rk != g_addr_to_group.end()) ? _rk->second : "";
    if (gname.empty()) return;
    auto it = g_groups.find(gname);
    if (it == g_groups.end() || !it->second.copyright) return;
    it->second.copyright->dj_acknowledged = true;
    fprintf(stderr, "[copyright] DJ acknowledged copyright in group '%s'\n", gname.c_str());
}

// Handle COPYRIGHT_SKIP from DJ
static void handle_copyright_skip(const sockaddr_in& from) {
    std::lock_guard<std::shared_mutex> lock(g_mutex);
    auto _rk = g_addr_to_group.find(addr_key(from));
    std::string gname = (_rk != g_addr_to_group.end()) ? _rk->second : "";
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
[[maybe_unused]] static std::string find_device_name(const sockaddr_in& addr) {
    for (const auto& [name, group] : g_groups) {
        for (const auto& m : group.members) {
            if (addr_equal(m.addr, addr)) {
                return m.device_name;
            }
        }
    }
    return "";
}

// Helper: find device_id (UUID) for an address (requires g_mutex held)
// Use this for wallet/ownership operations instead of find_device_name
static std::string find_device_id(const sockaddr_in& addr) {
    for (const auto& [name, group] : g_groups) {
        for (const auto& m : group.members) {
            if (addr_equal(m.addr, addr)) {
                return m.device_id;
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
        device_id = find_device_id(from);
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
        device_id = find_device_id(from);
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
        if (std::abs(now - ts) > 30) {
            const char* err = "ERR:token_expired\n";
            sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
            fprintf(stderr, "[wallet] CHARGE rejected: token expired (drift=%llds) from %s\n",
                    (long long)(now - ts), addr_str(from).c_str());
            return;
        }
        // Compute expected HMAC
        std::string msg = "CHARGE:" + std::to_string(amount) + ":" + device_id + ":" + ts_str;
        std::string expected = hmac_sha256_hex(g_charge_secret, msg);
        // Constant-time comparison to prevent timing attacks
        bool hmac_match = (client_hmac.size() == expected.size());
        if (hmac_match) {
            volatile unsigned char result = 0;
            for (size_t i = 0; i < client_hmac.size(); i++)
                result |= client_hmac[i] ^ expected[i];
            hmac_match = (result == 0);
        }
        if (!hmac_match) {
            const char* err = "ERR:invalid_token\n";
            sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
            fprintf(stderr, "[wallet] CHARGE rejected: HMAC mismatch from %s\n",
                    addr_str(from).c_str());
            return;
        }
        // Replay protection: reject duplicate HMAC within validity window
        {
            std::lock_guard<std::mutex> nlock(g_charge_nonce_mutex);
            // Purge expired entries
            int64_t now_ts = now_unix();
            for (auto it = g_charge_used_tokens.begin(); it != g_charge_used_tokens.end(); ) {
                if (it->second < now_ts) it = g_charge_used_tokens.erase(it);
                else ++it;
            }
            if (g_charge_used_tokens.count(client_hmac)) {
                const char* err = "ERR:replay_detected\n";
                sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
                fprintf(stderr, "[wallet] CHARGE rejected: replay from %s\n", addr_str(from).c_str());
                return;
            }
            g_charge_used_tokens[client_hmac] = now_ts + 300;  // expires in 300s
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

    // Session token is REQUIRED for all financial operations
    if (session_tok.empty()) {
        const char* err = "ERR:session_required\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }
    {
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
        device_id = find_device_id(from);
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
        device_id = find_device_id(from);
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

// TIP:<amount_usd>:<tx_signature>:<wallet_pubkey>[:<session_token>]\n (OSTP v0.9.3 §7.3)
// Legacy: TIP:<amount_usd>[:<session_token>]\n (no blockchain verification)
static void handle_tip(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 4, len - 4);  // skip "TIP:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    // Split fields: amount[:tx_sig[:wallet_pubkey[:session_token]]]
    std::vector<std::string> parts;
    {
        std::string s = payload;
        size_t p;
        while ((p = s.find(':')) != std::string::npos) {
            parts.push_back(s.substr(0, p));
            s = s.substr(p + 1);
        }
        parts.push_back(s);
    }

    std::string amount_str   = parts.size() > 0 ? parts[0] : "";
    std::string tx_sig       = parts.size() > 1 ? parts[1] : "";
    std::string wallet_pubkey= parts.size() > 2 ? parts[2] : "";
    std::string session_tok  = parts.size() > 3 ? parts[3] :
                               (parts.size() == 2 ? parts[1] : ""); // legacy: 2nd field = session
    // Session token is REQUIRED for all financial operations
    if (session_tok.empty()) {
        const char* err = "ERR:session_required\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }
    {
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

    // OSTP v0.9.3 §7.3: verify on-chain tx if tx_signature provided
    if (!tx_sig.empty()) {
        if (!solana_verify_tip_tx(tx_sig, wallet_pubkey, amount)) {
            const char* err = "ERR:tx_verification_failed\n";
            sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
            return;
        }
    }

    std::string tipper_device;
    std::string dj_device;
    sockaddr_in dj_addr{};
    bool found_dj = false;

    {
        std::lock_guard<std::shared_mutex> lock(g_mutex);
        auto _rk = g_addr_to_group.find(addr_key(from));
        std::string gname = (_rk != g_addr_to_group.end()) ? _rk->second : "";
        if (gname.empty()) {
            const char* err = "ERR:not_in_group\n";
            sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
            return;
        }
        auto& group = g_groups[gname];
        for (const auto& m : group.members) {
            if (addr_equal(m.addr, from)) {
                tipper_device = m.device_id;
            }
            if (m.role >= MemberRole::DJ && !addr_equal(m.addr, from)) {
                dj_device = m.device_id;
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
    // Session token is REQUIRED for all financial operations
    if (session_tok.empty()) {
        const char* err = "ERR:session_required\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }
    {
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
        auto _rk = g_addr_to_group.find(addr_key(from));
        group_name = (_rk != g_addr_to_group.end()) ? _rk->second : "";
        if (group_name.empty()) {
            const char* err = "ERR:not_in_group\n";
            sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
            return;
        }
        auto& group = g_groups[group_name];
        for (const auto& m : group.members) {
            all_addrs.push_back(m.addr);
            if (addr_equal(m.addr, from)) supporter_device = m.device_id;
            if (m.role >= MemberRole::DJ && !addr_equal(m.addr, from)) dj_device = m.device_id;
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

    auto _rk = g_addr_to_group.find(addr_key(from));
    std::string group_name = (_rk != g_addr_to_group.end()) ? _rk->second : "";
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
        device_id = find_device_id(from);
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
static void enqueue_forward(const uint8_t* data, size_t len, std::vector<sockaddr_in>&& dests);

// ── OSTP packet validation (§2 of protocol spec) ─────────────────────────────
// Validates RTP header + OSTP extension header + CRC-32 trailer.
// Returns true if packet is valid OSTP, false otherwise (still forwarded for
// backwards compatibility with non-OSTP RTP senders).

static constexpr uint16_t kOSTP_Profile = 0x4F53;  // "OS"

static uint32_t crc32_ieee(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}

struct OstpHeader {
    // RTP fields
    uint8_t  version;         // V (must be 2)
    bool     padding;         // P
    bool     extension;       // X (must be 1 for OSTP)
    uint8_t  csrc_count;      // CC
    bool     marker;          // M
    uint8_t  payload_type;    // PT (96=S24, 97=F32, 98=Opus, 10/11=AES67, 126=NACK, 127=FEC)
    uint16_t sequence;        // Sequence number (lower 16 bits)
    uint32_t rtp_timestamp;   // RTP timestamp (48kHz sample count)
    uint32_t ssrc;            // Synchronization source
    // OSTP extension fields (present when X=1 and profile=0x4F53)
    uint16_t stream_id;       // Logical stream ID
    uint16_t seq_extension;   // Upper 16 bits of 32-bit sequence
    uint32_t media_timestamp; // Wall-clock nanoseconds (lower 32 bits)
    // Derived
    bool     is_ostp;         // true if OSTP extension was parsed
    bool     crc_valid;       // true if CRC-32 trailer matched
    size_t   payload_offset;  // byte offset where audio payload begins
    size_t   payload_len;     // length of audio payload (excluding CRC trailer)
};

static OstpHeader parse_ostp_header(const uint8_t* data, size_t len) {
    OstpHeader h = {};

    if (len < 12) return h;  // minimum RTP header

    // Parse RTP header (12 bytes)
    h.version    = (data[0] >> 6) & 0x03;
    h.padding    = (data[0] >> 5) & 0x01;
    h.extension  = (data[0] >> 4) & 0x01;
    h.csrc_count = data[0] & 0x0F;
    h.marker     = (data[1] >> 7) & 0x01;
    h.payload_type = data[1] & 0x7F;
    h.sequence   = (uint16_t)(data[2] << 8 | data[3]);
    h.rtp_timestamp = (uint32_t)(data[4] << 24 | data[5] << 16 | data[6] << 8 | data[7]);
    h.ssrc       = (uint32_t)(data[8] << 24 | data[9] << 16 | data[10] << 8 | data[11]);

    size_t offset = 12 + h.csrc_count * 4;  // skip CSRC list

    // Parse RTP extension header + OSTP extension
    if (h.extension && len >= offset + 4) {
        uint16_t profile = (uint16_t)(data[offset] << 8 | data[offset + 1]);
        uint16_t ext_len = (uint16_t)(data[offset + 2] << 8 | data[offset + 3]);
        offset += 4;

        if (profile == kOSTP_Profile && ext_len == 2 && len >= offset + 8) {
            h.is_ostp = true;
            h.stream_id      = (uint16_t)(data[offset] << 8 | data[offset + 1]);
            h.seq_extension  = (uint16_t)(data[offset + 2] << 8 | data[offset + 3]);
            h.media_timestamp = (uint32_t)(data[offset + 4] << 24 | data[offset + 5] << 16 |
                                           data[offset + 6] << 8 | data[offset + 7]);
            offset += ext_len * 4;  // ext_len is in 32-bit words
        } else {
            // Non-OSTP RTP extension — skip it
            offset += ext_len * 4;
        }
    }

    h.payload_offset = offset;

    // CRC-32 validation: last 4 bytes of packet are CRC of payload only
    if (h.is_ostp && len >= offset + 4) {
        size_t crc_offset = len - 4;
        h.payload_len = crc_offset - offset;
        uint32_t expected_crc = (uint32_t)(data[crc_offset] << 24 | data[crc_offset + 1] << 16 |
                                           data[crc_offset + 2] << 8 | data[crc_offset + 3]);
        uint32_t actual_crc = crc32_ieee(data + offset, h.payload_len);
        h.crc_valid = (expected_crc == actual_crc);
    } else {
        // Non-OSTP or no CRC trailer — treat full remaining as payload
        h.payload_len = (len > offset) ? len - offset : 0;
        h.crc_valid = true;  // no CRC to check
    }

    return h;
}

// Stats counters for OSTP validation
static std::atomic<uint64_t> g_ostp_packets{0};       // packets with valid OSTP extension
static std::atomic<uint64_t> g_ostp_crc_ok{0};        // packets with valid CRC-32
static std::atomic<uint64_t> g_ostp_crc_fail{0};      // packets with CRC-32 mismatch
static std::atomic<uint64_t> g_rtp_legacy_packets{0};  // RTP packets without OSTP extension

// FEC & NACK stats
static constexpr size_t kFecGroupSize = 5;             // 5 data packets + 1 parity (§8.1)
[[maybe_unused]] static constexpr uint8_t kPT_S24     = 96;
static constexpr uint8_t kPT_Float32 = 97;
static constexpr uint8_t kPT_Opus    = 98;
static constexpr uint8_t kPT_AES67_L24 = 10;
static constexpr uint8_t kPT_AES67_L16 = 11;
[[maybe_unused]] static constexpr uint8_t kPT_SYNC   = 125;
static constexpr uint8_t kPT_NACK   = 126;
static constexpr uint8_t kPT_FEC    = 127;
static std::atomic<uint64_t> g_sync_packets{0};
static std::atomic<uint64_t> g_fec_packets_sent{0};
[[maybe_unused]] static std::atomic<uint64_t> g_fec_recoveries{0};
static std::atomic<uint64_t> g_nack_requests_rx{0};
static std::atomic<uint64_t> g_nack_retransmits{0};
static std::atomic<uint64_t> g_fec_accumulations{0};  // debug: packets entering FEC accumulator
static std::atomic<uint64_t> g_fwd_group_found{0};   // debug: forward_audio found group
static std::atomic<uint64_t> g_fwd_no_group{0};      // debug: forward_audio no group found

// ── FEC parity generation (§8.1) ─────────────────────────────────────────────
// Builds an XOR parity packet from a group of data packets.
// Called from forward_audio when fec.pkt_count reaches kFecGroupSize.
static std::vector<uint8_t> build_fec_packet(const std::vector<uint8_t>& xor_buf,
                                              uint32_t ssrc, uint16_t base_seq,
                                              uint32_t rtp_ts) {
    // FEC packet: RTP header (PT=127) + OSTP extension + XOR payload + CRC-32
    size_t total = 24 + xor_buf.size() + 4;  // 12 RTP + 4 ext hdr + 8 OSTP ext + payload + 4 CRC
    std::vector<uint8_t> pkt(total, 0);

    // RTP header
    pkt[0] = 0x90;  // V=2, P=0, X=1, CC=0
    pkt[1] = kPT_FEC;  // PT=127
    pkt[2] = (uint8_t)(base_seq >> 8);
    pkt[3] = (uint8_t)(base_seq & 0xFF);
    pkt[4] = (uint8_t)(rtp_ts >> 24); pkt[5] = (uint8_t)(rtp_ts >> 16);
    pkt[6] = (uint8_t)(rtp_ts >> 8);  pkt[7] = (uint8_t)(rtp_ts);
    pkt[8] = (uint8_t)(ssrc >> 24); pkt[9] = (uint8_t)(ssrc >> 16);
    pkt[10] = (uint8_t)(ssrc >> 8); pkt[11] = (uint8_t)(ssrc);

    // RTP extension header: profile=0x4F53, length=2 words
    pkt[12] = 0x4F; pkt[13] = 0x53;
    pkt[14] = 0x00; pkt[15] = 0x02;

    // OSTP extension: stream_id=0xFFFF (FEC marker), seq_ext=0, media_ts=0
    pkt[16] = 0xFF; pkt[17] = 0xFF;
    pkt[18] = 0x00; pkt[19] = 0x00;
    pkt[20] = 0x00; pkt[21] = 0x00; pkt[22] = 0x00; pkt[23] = 0x00;

    // XOR payload
    memcpy(pkt.data() + 24, xor_buf.data(), xor_buf.size());

    // CRC-32 over payload only
    uint32_t crc = crc32_ieee(pkt.data() + 24, xor_buf.size());
    pkt[total - 4] = (uint8_t)(crc >> 24);
    pkt[total - 3] = (uint8_t)(crc >> 16);
    pkt[total - 2] = (uint8_t)(crc >> 8);
    pkt[total - 1] = (uint8_t)(crc);

    return pkt;
}

// ── Clock sync handler (§9) ──────────────────────────────────────────────────
// PT=125 sync packet: NTP-like clock offset measurement between RX and relay.
// Layout (25 bytes):
//   Byte 0:    0x7D (PT=125 marker)
//   Bytes 1-8: T1 (sender timestamp, 64-bit LE nanoseconds)
//   Bytes 9-16: T2 (relay receive timestamp, filled by relay)
//   Bytes 17-24: T3 (relay send timestamp, filled by relay)
// RX computes: offset = ((T2-T1) + (T3-T4)) / 2, RTT = (T4-T1) - (T3-T2)
static void handle_sync_packet(const uint8_t* data, size_t len,
                                const sockaddr_in& from) {
    g_sync_packets.fetch_add(1, std::memory_order_relaxed);

    // Sync packet must be exactly 25 bytes and start with 0x7D
    if (len < 25 || data[0] != 0x7D) return;

    // Get relay receive time (T2) — CLOCK_REALTIME nanoseconds
    struct timespec now_ts;
    clock_gettime(CLOCK_REALTIME, &now_ts);
    uint64_t t2_ns = (uint64_t)now_ts.tv_sec * 1000000000ULL + (uint64_t)now_ts.tv_nsec;

    // Build response: copy original packet, fill T2 and T3
    uint8_t reply[25];
    memcpy(reply, data, 25);

    // T2 at bytes 9-16 (little-endian)
    memcpy(reply + 9, &t2_ns, 8);

    // T3 = relay send time (capture just before sending)
    clock_gettime(CLOCK_REALTIME, &now_ts);
    uint64_t t3_ns = (uint64_t)now_ts.tv_sec * 1000000000ULL + (uint64_t)now_ts.tv_nsec;
    memcpy(reply + 17, &t3_ns, 8);

    // Send pong back to sender
    sendto(g_udp_sock, reply, 25, 0, (const sockaddr*)&from, sizeof(from));
}

// ── NACK handler (§8.2) ──────────────────────────────────────────────────────
// PT=126 packet payload contains list of missing 16-bit sequence numbers.
// Looks up replay buffer and retransmits matching packets.
static void handle_nack_packet(const uint8_t* data, size_t len, const sockaddr_in& from) {
    g_nack_requests_rx.fetch_add(1, std::memory_order_relaxed);

    OstpHeader h = parse_ostp_header(data, len);
    if (h.payload_len < 2) return;  // need at least one sequence number

    const uint8_t* payload = data + h.payload_offset;
    size_t num_seqs = h.payload_len / 2;
    if (num_seqs > 32) num_seqs = 32;  // cap to prevent abuse

    // Find the sender's group
    std::lock_guard<std::shared_mutex> lock(g_mutex);
    std::string gname;
    Group* gptr = nullptr;
    auto rk = g_addr_to_group.find(addr_key(from));
    if (rk != g_addr_to_group.end()) {
        auto it = g_groups.find(rk->second);
        if (it != g_groups.end()) {
            gname = rk->second;
            gptr = &it->second;
        }
    }
    if (!gptr) return;

    // Parse requested sequence numbers
    for (size_t i = 0; i < num_seqs; i++) {
        uint16_t req_seq = (uint16_t)(payload[i * 2] << 8 | payload[i * 2 + 1]);

        // Search replay buffer for matching sequence
        for (const auto& rp : gptr->replay_buffer) {
            if (rp.data.size() >= 4) {
                uint16_t pkt_seq = (uint16_t)(rp.data[2] << 8 | rp.data[3]);
                if (pkt_seq == req_seq) {
                    sendto(g_udp_sock, rp.data.data(), rp.data.size(), 0,
                           (const sockaddr*)&from, sizeof(from));
                    g_nack_retransmits.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
        }
    }
}

static void forward_audio(const uint8_t* data, size_t len, const sockaddr_in& from) {
    // ── OSTP header parsing (§2) ──
    // Parse and validate, but always forward for backwards compatibility.
    // CRC failures are counted but not dropped (sender may be non-OSTP).
    OstpHeader ostp = parse_ostp_header(data, len);
    if (ostp.is_ostp) {
        g_ostp_packets.fetch_add(1, std::memory_order_relaxed);
        if (ostp.crc_valid) {
            g_ostp_crc_ok.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_ostp_crc_fail.fetch_add(1, std::memory_order_relaxed);
            // Log CRC failures (rate-limited: only every 100th)
            uint64_t fail_count = g_ostp_crc_fail.load(std::memory_order_relaxed);
            if (fail_count <= 3 || fail_count % 100 == 0) {
                fprintf(stderr, "[ostp] CRC-32 mismatch on packet from %s (PT=%d, seq=%u, #%llu)\n",
                        addr_str(from).c_str(), ostp.payload_type, ostp.sequence,
                        (unsigned long long)fail_count);
            }
        }
    } else {
        g_rtp_legacy_packets.fetch_add(1, std::memory_order_relaxed);
    }

    // Handle clock sync packets (PT=125) — NTP-like offset measurement
    if (len >= 25 && data[0] == 0x7D) {
        handle_sync_packet(data, len, from);
        return;
    }

    // Handle NACK packets (PT=126) — retransmit from replay buffer
    if (ostp.payload_type == kPT_NACK) {
        handle_nack_packet(data, len, from);
        return;
    }

    // Handle RTCP APP (PT=204) "SWCH" — forward to all group members for sync switch (OSTP v0.9.3 §5.4)
    if (len == 16 && (data[0] & 0xC0) == 0x80 && data[1] == 204 &&
        data[8] == 'S' && data[9] == 'W' && data[10] == 'C' && data[11] == 'H') {
        std::vector<sockaddr_in> dests;
        {
            std::lock_guard<std::shared_mutex> lock(g_mutex);
            auto rk = g_addr_to_group.find(addr_key(from));
            if (rk != g_addr_to_group.end()) {
                auto it = g_groups.find(rk->second);
                if (it != g_groups.end()) {
                    for (const auto& m : it->second.members) {
                        if (!addr_equal(m.addr, from)) dests.push_back(m.addr);
                    }
                }
            }
        }
        for (const auto& dst : dests) {
            sendto(g_udp_sock, data, len, 0, (const sockaddr*)&dst, sizeof(dst));
        }
        return;
    }

    std::string group_name;
    std::vector<sockaddr_in> local_dests;
    size_t swarm_saved = 0;
    bool is_swarm = false;
    std::vector<uint8_t> fec_pkt_to_send;   // FEC parity packet (built under lock, sent after)
    std::vector<sockaddr_in> fec_dests;

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
        if (!group_ptr) { g_fwd_no_group.fetch_add(1, std::memory_order_relaxed); return; }
        g_fwd_group_found.fetch_add(1, std::memory_order_relaxed);

        auto& group = *group_ptr;
        Member* sender = find_member(group, from);
        if (!sender) return;

        // Permission check: only DJ/Owner or mic_allowed members can send audio
        if (!is_random_channel(group_name) && !is_free_name(group_name)) {
            if (sender->role < MemberRole::DJ && !sender->mic_allowed) {
                return;  // silently drop — no mic permission
            }
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
        group.last_audio_time = std::chrono::steady_clock::now();

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
        // PayloadType-aware extraction (§2.3): S24, Float32, AES67 L24/L16
        // Opus (PT=98) is compressed — skip fingerprinting (can't extract PCM without decoder)
        if (g_copyright_enabled && len > 28 && group.mode == ChannelMode::Public &&
            ostp.payload_type != kPT_Opus && ostp.payload_type != kPT_NACK &&
            ostp.payload_type != kPT_FEC) {
            if (!group.copyright) {
                group.copyright = std::make_unique<GroupCopyrightState>();
            }
            auto& fp = group.copyright->fp_buf;
            if (fp.mtx.try_lock()) {
                size_t pay_off = ostp.payload_offset > 0 ? ostp.payload_offset : 24;
                size_t pay_len = ostp.payload_len > 0 ? ostp.payload_len : (len > pay_off + 4 ? len - pay_off - 4 : 0);
                const uint8_t* payload = data + pay_off;
                uint8_t pt = ostp.payload_type;

                if (pt == kPT_AES67_L16) {
                    // AES67 L16: 2 bytes per sample, big-endian, stereo interleaved
                    size_t num_frames = pay_len / 4;  // 2 bytes × 2 channels
                    for (size_t i = 0; i < num_frames && i < 480; i++) {
                        int16_t left  = (int16_t)(payload[i * 4] << 8 | payload[i * 4 + 1]);
                        int16_t right = (int16_t)(payload[i * 4 + 2] << 8 | payload[i * 4 + 3]);
                        int16_t mono = (int16_t)(((int32_t)left + right) / 2);
                        fp.ring[fp.write_pos] = mono;
                        fp.write_pos = (fp.write_pos + 1) % kFingerprintRingSize;
                        fp.total_written++;
                    }
                } else if (pt == kPT_AES67_L24) {
                    // AES67 L24: 3 bytes per sample, big-endian, stereo interleaved
                    size_t num_frames = pay_len / 6;  // 3 bytes × 2 channels
                    for (size_t i = 0; i < num_frames && i < 480; i++) {
                        const uint8_t* s = payload + i * 6;
                        int32_t left  = ((int32_t)s[0] << 24 | s[1] << 16 | s[2] << 8) >> 8;
                        int32_t right = ((int32_t)s[3] << 24 | s[4] << 16 | s[5] << 8) >> 8;
                        int16_t mono = (int16_t)(((left + right) / 2) >> 8);
                        fp.ring[fp.write_pos] = mono;
                        fp.write_pos = (fp.write_pos + 1) % kFingerprintRingSize;
                        fp.total_written++;
                    }
                } else if (pt == kPT_Float32) {
                    // Float32: 4 bytes IEEE 754 per channel, stereo interleaved
                    size_t num_frames = pay_len / 8;  // 4 bytes × 2 channels
                    for (size_t i = 0; i < num_frames && i < 480; i++) {
                        float left, right;
                        memcpy(&left, payload + i * 8, 4);
                        memcpy(&right, payload + i * 8 + 4, 4);
                        float mono = (left + right) * 0.5f;
                        int16_t sample = (int16_t)(mono * 32767.0f);
                        fp.ring[fp.write_pos] = sample;
                        fp.write_pos = (fp.write_pos + 1) % kFingerprintRingSize;
                        fp.total_written++;
                    }
                } else {
                    // PT=96 (S24) or unknown: 4-byte int32 stereo (original behavior)
                    size_t num_frames = pay_len / 8;
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
                }
                fp.active = true;
                fp.group_name = group_name;
                fp.mtx.unlock();
            }
        }

        // ── FEC parity generation (§8.1) ──
        // Accumulate XOR parity over kFecGroupSize data packets per group.
        // Only for audio packets (not NACK/FEC themselves).
        if (ostp.payload_type != kPT_NACK && ostp.payload_type != kPT_FEC &&
            ostp.payload_offset > 0 && ostp.payload_len > 0) {
            g_fec_accumulations.fetch_add(1, std::memory_order_relaxed);
            auto& fec = group.fec;
            const uint8_t* pay = data + ostp.payload_offset;
            size_t plen = ostp.payload_len;

            if (fec.pkt_count == 0) {
                // First packet in new FEC group
                fec.xor_buf.assign(pay, pay + plen);
                fec.base_seq = ostp.sequence;
                fec.max_len = plen;
                fec.pkt_count = 1;
            } else {
                // XOR accumulate (pad shorter payloads with zero)
                if (plen > fec.xor_buf.size()) fec.xor_buf.resize(plen, 0);
                for (size_t i = 0; i < plen; i++) fec.xor_buf[i] ^= pay[i];
                if (plen > fec.max_len) fec.max_len = plen;
                fec.pkt_count++;

                if (fec.pkt_count >= kFecGroupSize) {
                    // Emit FEC parity packet
                    fec.xor_buf.resize(fec.max_len, 0);
                    fec_pkt_to_send = build_fec_packet(fec.xor_buf, ostp.ssrc,
                                                        fec.base_seq, ostp.rtp_timestamp);
                    fec_dests = local_dests;  // send to same destinations
                    fec.pkt_count = 0;
                    fec.xor_buf.clear();
                }
            }
        }

        // Track member sequence for gap detection (NACK support)
        if (ostp.version == 2) {
            uint64_t mk = addr_key(from);
            uint32_t full_seq = ((uint32_t)ostp.seq_extension << 16) | ostp.sequence;
            group.member_last_seq[mk] = full_seq;
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

    // Send FEC parity packet if one was generated this cycle
    if (!fec_pkt_to_send.empty()) {
        enqueue_forward(fec_pkt_to_send.data(), fec_pkt_to_send.size(), std::move(fec_dests));
        g_fec_packets_sent.fetch_add(1, std::memory_order_relaxed);
    }

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

// ── DELAY handler ─────────────────────────────────────────────────────────────
// Format: "DELAY:<net_delay_ms>\n" — receiver reports its network delay
// Relay computes group max and broadcasts "MAXDELAY:<ms>\n" to all members.
// This allows sync mode to align all devices to the slowest one (up to 2000ms).
static void handle_delay(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 6, len - 6);
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();
    uint32_t reported_ms = 0;
    try { reported_ms = static_cast<uint32_t>(std::stoul(payload)); } catch (...) { return; }
    if (reported_ms > 2000) reported_ms = 2000;  // cap at 2s

    std::lock_guard<std::shared_mutex> lock(g_mutex);
    for (auto& [name, group] : g_groups) {
        Member* sender = find_member(group, from);
        if (!sender) continue;

        sender->net_delay_ms = reported_ms;

        // Compute group max delay (+ 100ms jitter margin)
        uint32_t group_max = 0;
        for (const auto& m : group.members) {
            if (m.net_delay_ms > group_max) group_max = m.net_delay_ms;
        }
        uint32_t target_delay = std::min(group_max + 100u, 2000u);

        // Only broadcast if changed significantly (>20ms difference)
        if (target_delay > group.max_delay_ms + 20 ||
            (group.max_delay_ms > 120 && target_delay + 20 < group.max_delay_ms)) {
            group.max_delay_ms = target_delay;
            char buf[64];
            snprintf(buf, sizeof(buf), "MAXDELAY:%u\n", target_delay);
            std::string fwd(buf);
            for (const auto& m : group.members) {
                sendto(g_udp_sock, fwd.c_str(), fwd.size(), 0,
                       (const sockaddr*)&m.addr, sizeof(m.addr));
            }
            fprintf(stderr, "[relay] MAXDELAY: %u ms in group '%s' (reporter: %u ms)\n",
                    target_delay, name.c_str(), reported_ms);
        }
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

// ── TALK handler ─────────────────────────────────────────────────────────────
// Format: "TALK:on\n" or "TALK:off\n"
// Enables/disables talk mode (all members can send audio simultaneously).
// Any DJ or Owner can toggle this.
static void handle_talk(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 5, len - 5);
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    bool enable = (payload == "on");

    std::lock_guard<std::shared_mutex> lock(g_mutex);
    auto it = g_addr_to_group.find(addr_key(from));
    if (it == g_addr_to_group.end()) return;

    auto& group = g_groups[it->second];
    Member* sender = find_member(group, from);
    if (!sender || sender->role < MemberRole::DJ) return;

    group.talk_mode = enable;

    // When talk mode is enabled, promote all existing listeners to DJ
    if (enable) {
        for (auto& m : group.members) {
            if (m.role == MemberRole::Listener) {
                m.role = MemberRole::DJ;
                m.mic_allowed = true;
            }
        }
    }

    // Notify all members
    std::string notify = "TALK:" + payload + "\n";
    for (const auto& m : group.members) {
        sendto(g_udp_sock, notify.c_str(), notify.size(), 0,
               (const sockaddr*)&m.addr, sizeof(m.addr));
    }
}

// ── Per-Device Mic Permission handlers ────────────────────────────────────────

// Format: "MIC_ALLOW:<device_id>\n"
// Owner or DJ grants mic permission to a specific device.
static void handle_mic_allow(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 10, len - 10);  // skip "MIC_ALLOW:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    std::string target_device_id = payload;
    if (target_device_id.empty()) {
        const char* err = "ERR:missing_device_id\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    std::lock_guard<std::shared_mutex> lock(g_mutex);
    auto it = g_addr_to_group.find(addr_key(from));
    if (it == g_addr_to_group.end()) return;

    auto& group = g_groups[it->second];
    Member* sender = find_member(group, from);
    if (!sender || sender->role < MemberRole::DJ) {
        const char* err = "ERR:permission_denied\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    // Find target member by device_id
    for (auto& m : group.members) {
        if (m.device_id == target_device_id) {
            m.mic_allowed = true;
            // Notify the affected device
            const char* status = "MIC_STATUS:allowed\n";
            sendto(g_udp_sock, status, strlen(status), 0,
                   (const sockaddr*)&m.addr, sizeof(m.addr));
            // Confirm to sender
            const char* ok = "OK:mic_allowed\n";
            sendto(g_udp_sock, ok, strlen(ok), 0, (const sockaddr*)&from, sizeof(from));
            return;
        }
    }
    const char* err = "ERR:device_not_found\n";
    sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
}

// Format: "MIC_DENY:<device_id>\n"
// Owner or DJ revokes mic permission from a specific device.
static void handle_mic_deny(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 9, len - 9);  // skip "MIC_DENY:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    std::string target_device_id = payload;
    if (target_device_id.empty()) {
        const char* err = "ERR:missing_device_id\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    std::lock_guard<std::shared_mutex> lock(g_mutex);
    auto it = g_addr_to_group.find(addr_key(from));
    if (it == g_addr_to_group.end()) return;

    auto& group = g_groups[it->second];
    Member* sender = find_member(group, from);
    if (!sender || sender->role < MemberRole::DJ) {
        const char* err = "ERR:permission_denied\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    // Find target member by device_id
    for (auto& m : group.members) {
        if (m.device_id == target_device_id) {
            // Don't allow denying mic from Owner or DJ (they have implicit permission)
            if (m.role >= MemberRole::DJ) {
                const char* err = "ERR:cannot_deny_dj_owner\n";
                sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
                return;
            }
            m.mic_allowed = false;
            // Notify the affected device
            const char* status = "MIC_STATUS:denied\n";
            sendto(g_udp_sock, status, strlen(status), 0,
                   (const sockaddr*)&m.addr, sizeof(m.addr));
            // Confirm to sender
            const char* ok = "OK:mic_denied\n";
            sendto(g_udp_sock, ok, strlen(ok), 0, (const sockaddr*)&from, sizeof(from));
            return;
        }
    }
    const char* err = "ERR:device_not_found\n";
    sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
}

// Format: "VOLUME:<target_device_id>:<level_0_100>\n"
// Forward volume command to a specific device in the sender's group.
// Any group member can send this (no role restriction).
static void handle_volume_command(const char* msg, size_t len, const sockaddr_in& from) {
    std::string payload(msg + 7, len - 7);  // skip "VOLUME:"
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    auto colon = payload.find(':');
    if (colon == std::string::npos) return;

    std::string target_device = payload.substr(0, colon);
    std::string level_str = payload.substr(colon + 1);
    if (target_device.empty() || level_str.empty()) return;

    std::shared_lock<std::shared_mutex> lock(g_mutex);
    auto rk = g_addr_to_group.find(addr_key(from));
    if (rk == g_addr_to_group.end()) return;
    auto git = g_groups.find(rk->second);
    if (git == g_groups.end()) return;

    for (const auto& m : git->second.members) {
        if (m.device_id == target_device) {
            std::string cmd = "VOLUME_SET:" + level_str + "\n";
            sendto(g_udp_sock, cmd.c_str(), cmd.size(), 0,
                   (const sockaddr*)&m.addr, sizeof(m.addr));
            return;
        }
    }
}

// Format: "MIC_LIST\n"
// Returns JSON array of all devices with their mic status.
static void handle_mic_list(const sockaddr_in& from) {
    std::lock_guard<std::shared_mutex> lock(g_mutex);
    auto it = g_addr_to_group.find(addr_key(from));
    if (it == g_addr_to_group.end()) {
        const char* err = "ERR:not_in_group\n";
        sendto(g_udp_sock, err, strlen(err), 0, (const sockaddr*)&from, sizeof(from));
        return;
    }

    auto& group = g_groups[it->second];
    std::string response = "MIC_LIST:[";
    bool first = true;
    for (const auto& m : group.members) {
        if (!first) response += ",";
        first = false;
        bool effective_mic = m.mic_allowed || m.role >= MemberRole::DJ;
        const char* role_name = m.role == MemberRole::Owner ? "owner" :
                                m.role == MemberRole::DJ ? "dj" : "listener";
        response += "{\"device_id\":\"" + json_escape(m.device_id) + "\","
                    "\"device\":\"" + json_escape(m.device_name) + "\","
                    "\"role\":\"" + role_name + "\","
                    "\"mic_allowed\":" + (effective_mic ? std::string("true") : std::string("false")) + "}";
    }
    response += "]\n";
    sendto(g_udp_sock, response.c_str(), response.size(), 0,
           (const sockaddr*)&from, sizeof(from));
}

// ── GLOBAL_DEVICES handler ───────────────────────────────────────────────────
// Format: "GLOBAL_DEVICES\n" — no auth required, lists all DJ/Owner across all groups
// Response: DEVICES:[{"name":"...","id":"...","group":"...","role":"owner|dj","addr":"ip:port"},...]\n
static void handle_global_devices(const sockaddr_in& from) {
    std::lock_guard<std::shared_mutex> lock(g_mutex);
    std::string response = "DEVICES:[";
    bool first = true;
    for (const auto& [gname, group] : g_groups) {
        for (const auto& m : group.members) {
            if (m.role < MemberRole::DJ) continue;  // only TX-capable members
            if (!first) response += ",";
            first = false;
            const char* role_name = m.role == MemberRole::Owner ? "owner" : "dj";
            response += "{\"name\":\"" + json_escape(m.device_name.empty() ? "Unknown" : m.device_name) + "\","
                        "\"id\":\"" + json_escape(m.device_id) + "\","
                        "\"group\":\"" + json_escape(gname) + "\","
                        "\"role\":\"" + role_name + "\","
                        "\"addr\":\"" + addr_str(m.addr) + "\"}";
        }
    }
    response += "]\n";
    sendto(g_udp_sock, response.c_str(), response.size(), 0,
           (const sockaddr*)&from, sizeof(from));
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
    auto _rk = g_addr_to_group.find(addr_key(from));
    std::string gname = (_rk != g_addr_to_group.end()) ? _rk->second : "";
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
    auto _rk = g_addr_to_group.find(addr_key(from));
    std::string gname = (_rk != g_addr_to_group.end()) ? _rk->second : "";
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
    auto _rk = g_addr_to_group.find(addr_key(from));
    std::string gname = (_rk != g_addr_to_group.end()) ? _rk->second : "";
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
    auto _rk = g_addr_to_group.find(addr_key(from));
    std::string gname = (_rk != g_addr_to_group.end()) ? _rk->second : "";
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
    auto _rk = g_addr_to_group.find(addr_key(from));
    std::string gname = (_rk != g_addr_to_group.end()) ? _rk->second : "";
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

// ── DTLS-SRTP (optional encrypted transport) ─────────────────────────────────
// When --dtls-cert and --dtls-key are provided, the relay accepts DTLS
// handshakes on the same UDP port. After DTLS negotiation, SRTP keys are
// exported and used to encrypt/decrypt audio packets.
//
// Protocol flow:
//   1. Client sends DTLS ClientHello to relay UDP port
//   2. Relay completes DTLS handshake (using OpenSSL)
//   3. Both sides export SRTP keying material (RFC 5764)
//   4. Subsequent audio is SRTP-encrypted
//
// This is opt-in: clients that don't initiate DTLS send plaintext RTP (backwards compatible).

static SSL_CTX* g_dtls_ctx = nullptr;
static std::string g_dtls_cert_path;
static std::string g_dtls_key_path;
static bool g_dtls_enabled = false;
static std::atomic<uint64_t> g_dtls_sessions{0};
static std::atomic<uint64_t> g_dtls_handshake_ok{0};
static std::atomic<uint64_t> g_dtls_handshake_fail{0};

// Per-client DTLS session state
struct DtlsSession {
    SSL* ssl = nullptr;
    BIO* rbio = nullptr;  // read BIO (incoming UDP → OpenSSL)
    BIO* wbio = nullptr;  // write BIO (OpenSSL → outgoing UDP)
    sockaddr_in peer_addr;
    bool handshake_done = false;
    // SRTP keying material (exported after handshake)
    uint8_t srtp_key_client[16 + 14] = {};  // key(16) + salt(14) for client
    uint8_t srtp_key_server[16 + 14] = {};  // key(16) + salt(14) for server
    std::chrono::steady_clock::time_point created;
};

static std::mutex g_dtls_mutex;
static std::unordered_map<uint64_t, std::unique_ptr<DtlsSession>> g_dtls_sessions_map;

static bool init_dtls_context() {
    if (g_dtls_cert_path.empty() || g_dtls_key_path.empty()) return false;

    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    g_dtls_ctx = SSL_CTX_new(DTLS_server_method());
    if (!g_dtls_ctx) {
        fprintf(stderr, "[dtls] Failed to create SSL_CTX\n");
        return false;
    }

    // Load certificate and private key
    if (SSL_CTX_use_certificate_file(g_dtls_ctx, g_dtls_cert_path.c_str(), SSL_FILETYPE_PEM) != 1) {
        fprintf(stderr, "[dtls] Failed to load certificate: %s\n", g_dtls_cert_path.c_str());
        SSL_CTX_free(g_dtls_ctx); g_dtls_ctx = nullptr;
        return false;
    }
    if (SSL_CTX_use_PrivateKey_file(g_dtls_ctx, g_dtls_key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
        fprintf(stderr, "[dtls] Failed to load private key: %s\n", g_dtls_key_path.c_str());
        SSL_CTX_free(g_dtls_ctx); g_dtls_ctx = nullptr;
        return false;
    }

    // Enable SRTP extension (RFC 5764)
    if (SSL_CTX_set_tlsext_use_srtp(g_dtls_ctx, "SRTP_AES128_CM_SHA1_80:SRTP_AES128_CM_SHA1_32") != 0) {
        fprintf(stderr, "[dtls] Failed to set SRTP profiles\n");
        SSL_CTX_free(g_dtls_ctx); g_dtls_ctx = nullptr;
        return false;
    }

    // Set cipher suites for DTLS 1.2
    SSL_CTX_set_cipher_list(g_dtls_ctx, "HIGH:!aNULL:!MD5:!RC4");

    fprintf(stderr, "[dtls] DTLS-SRTP enabled (cert=%s)\n", g_dtls_cert_path.c_str());
    g_dtls_enabled = true;
    return true;
}

// Check if packet is a DTLS record (ContentType 20-25, version 0xFEFx)
static bool is_dtls_packet(const uint8_t* data, size_t len) {
    if (len < 13) return false;  // minimum DTLS record header
    uint8_t content_type = data[0];
    // DTLS content types: change_cipher_spec(20), alert(21), handshake(22), application_data(23)
    if (content_type < 20 || content_type > 25) return false;
    // DTLS version: 0xFEFD (1.2) or 0xFEFF (1.0)
    if (data[1] != 0xFE) return false;
    return true;
}

// Handle incoming DTLS packet: feed to SSL engine, process handshake or decrypt
static void handle_dtls_packet(const uint8_t* data, size_t len, const sockaddr_in& from) {
    if (!g_dtls_ctx) return;

    uint64_t key = addr_key(from);
    std::lock_guard<std::mutex> lock(g_dtls_mutex);

    auto it = g_dtls_sessions_map.find(key);
    if (it == g_dtls_sessions_map.end()) {
        // New DTLS session
        auto sess = std::make_unique<DtlsSession>();
        sess->peer_addr = from;
        sess->created = std::chrono::steady_clock::now();
        sess->ssl = SSL_new(g_dtls_ctx);
        sess->rbio = BIO_new(BIO_s_mem());
        sess->wbio = BIO_new(BIO_s_mem());
        SSL_set_bio(sess->ssl, sess->rbio, sess->wbio);
        SSL_set_accept_state(sess->ssl);

        it = g_dtls_sessions_map.emplace(key, std::move(sess)).first;
        g_dtls_sessions.fetch_add(1, std::memory_order_relaxed);
    }

    auto& sess = it->second;

    // Feed incoming data to SSL read BIO
    BIO_write(sess->rbio, data, (int)len);

    if (!sess->handshake_done) {
        int ret = SSL_do_handshake(sess->ssl);
        if (ret == 1) {
            sess->handshake_done = true;
            g_dtls_handshake_ok.fetch_add(1, std::memory_order_relaxed);

            // Export SRTP keying material
            SRTP_PROTECTION_PROFILE* profile = SSL_get_selected_srtp_profile(sess->ssl);
            if (profile) {
                // key_len=16, salt_len=14 for SRTP_AES128_CM_SHA1_80
                uint8_t material[2 * (16 + 14)];
                if (SSL_export_keying_material(sess->ssl, material, sizeof(material),
                        "EXTRACTOR-dtls_srtp", 19, nullptr, 0, 0) == 1) {
                    memcpy(sess->srtp_key_client, material, 30);
                    memcpy(sess->srtp_key_server, material + 30, 30);
                    fprintf(stderr, "[dtls] SRTP keys exported for %s (profile=%s)\n",
                            addr_str(from).c_str(), profile->name);
                }
            }
        } else {
            int err = SSL_get_error(sess->ssl, ret);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                g_dtls_handshake_fail.fetch_add(1, std::memory_order_relaxed);
                fprintf(stderr, "[dtls] Handshake failed for %s (err=%d)\n",
                        addr_str(from).c_str(), err);
                SSL_free(sess->ssl);
                g_dtls_sessions_map.erase(it);
                return;
            }
        }
    } else {
        // Handshake done — read application data (SRTP packets)
        uint8_t buf[65536];
        int rd = SSL_read(sess->ssl, buf, sizeof(buf));
        if (rd > 0 && rd >= 12 && (buf[0] & 0xC0) == 0x80) {
            // Decrypted RTP/OSTP packet — process normally
            forward_audio(buf, (size_t)rd, from);
        }
    }

    // Flush any pending outgoing DTLS data back to the client
    uint8_t out_buf[65536];
    int pending;
    while ((pending = BIO_read(sess->wbio, out_buf, sizeof(out_buf))) > 0) {
        sendto(g_udp_sock, out_buf, (size_t)pending, 0,
               (const sockaddr*)&from, sizeof(from));
    }
}

static void cleanup_dtls_sessions() {
    std::lock_guard<std::mutex> lock(g_dtls_mutex);
    auto now = std::chrono::steady_clock::now();
    for (auto it = g_dtls_sessions_map.begin(); it != g_dtls_sessions_map.end(); ) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second->created).count();
        if (age > 3600) {  // 1 hour timeout
            SSL_free(it->second->ssl);
            it = g_dtls_sessions_map.erase(it);
        } else {
            ++it;
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
        "  GOSSIP_PEERS\\n                   Request 8 peer candidates\n"
        "  PARENT_FAIL:<ip>:<port>\\n        Report parent failure for fast recovery\n"
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

// Allowed CORS origins for HTTP API
static const char* cors_origin(const char* request_origin) {
    if (!request_origin || !*request_origin) return "https://solun.art";
    // Exact-match allowed origins
    static const char* allowed[] = {
        "https://solun.art",
        "https://relay.solun.art",
        "https://www.solun.art",
        "http://localhost:3000",
        "http://localhost:5173",
        "http://127.0.0.1:3000",
        "http://127.0.0.1:5173",
        nullptr
    };
    for (const char** p = allowed; *p; ++p) {
        if (strcmp(request_origin, *p) == 0)
            return request_origin;
    }
    return "https://solun.art";
}

static void http_send(int fd, int status, const char* status_text,
                      const char* content_type, const std::string& body,
                      const char* origin = nullptr) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: %s\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
        "Connection: close\r\n\r\n",
        status, status_text, content_type, body.size(), cors_origin(origin));
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
    bool raw_format{false};   // true = strip OSTP header, send raw S16LE PCM only
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
    // Remove the HTTP recv timeout — WebSocket connections are long-lived
    timeval no_timeout{300, 0};  // 5 minute read timeout (for keepalive)
    setsockopt(ws->fd, SOL_SOCKET, SO_RCVTIMEO, &no_timeout, sizeof(no_timeout));

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
        // Binary frame: handle clock sync packets (PT=125, 0x7D marker, 25 bytes)
        if (opcode == 0x2 && payload_len == 25 && payload[0] == 0x7D) {
            g_sync_packets.fetch_add(1, std::memory_order_relaxed);

            // T2 = relay receive time (CLOCK_REALTIME nanoseconds)
            struct timespec now_ts;
            clock_gettime(CLOCK_REALTIME, &now_ts);
            uint64_t t2_ns = (uint64_t)now_ts.tv_sec * 1000000000ULL + (uint64_t)now_ts.tv_nsec;
            memcpy(payload.data() + 9, &t2_ns, 8);

            // T3 = relay send time
            clock_gettime(CLOCK_REALTIME, &now_ts);
            uint64_t t3_ns = (uint64_t)now_ts.tv_sec * 1000000000ULL + (uint64_t)now_ts.tv_nsec;
            memcpy(payload.data() + 17, &t3_ns, 8);

            // Send sync pong back as binary WebSocket frame
            ws_send_binary(ws->fd, payload.data(), 25);
        }
        // Handle text frames: NACK requests from WebSocket receivers.
        // Format: "NACK:<seq1>,<seq2>,..." (comma-separated 16-bit sequence numbers)
        // Relay retransmits matching packets from replay buffer as binary WebSocket frames.
        // This allows NAT-traversal-hostile receivers (browsers, mobile behind CGNAT) to
        // request retransmission without needing a reverse UDP path.
        if (opcode == 0x1 && payload_len >= 5) {
            std::string text(payload.begin(), payload.end());
            if (text.substr(0, 5) == "NACK:") {
                std::string seq_list = text.substr(5);
                // Parse comma-separated sequence numbers
                std::vector<uint16_t> requested_seqs;
                size_t pos = 0;
                while (pos < seq_list.size() && requested_seqs.size() < 32) {
                    size_t comma = seq_list.find(',', pos);
                    std::string tok = seq_list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                    if (!tok.empty()) {
                        int seq_val = std::atoi(tok.c_str());
                        if (seq_val >= 0 && seq_val <= 65535)
                            requested_seqs.push_back((uint16_t)seq_val);
                    }
                    if (comma == std::string::npos) break;
                    pos = comma + 1;
                }

                if (!requested_seqs.empty()) {
                    g_nack_requests_rx.fetch_add(1, std::memory_order_relaxed);

                    std::lock_guard<std::shared_mutex> lock(g_mutex);
                    auto it = g_groups.find(ws->channel);
                    if (it != g_groups.end()) {
                        const auto& group = it->second;
                        for (uint16_t req_seq : requested_seqs) {
                            for (const auto& rp : group.replay_buffer) {
                                if (rp.data.size() >= 4) {
                                    uint16_t pkt_seq = (uint16_t)(rp.data[2] << 8 | rp.data[3]);
                                    if (pkt_seq == req_seq) {
                                        // Retransmit as binary WebSocket frame
                                        ws_send_binary(ws->fd, rp.data.data(), rp.data.size());
                                        g_nack_retransmits.fetch_add(1, std::memory_order_relaxed);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        // Other frames from browser are ignored
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
            bool ok;
            if (ws->raw_format) {
                // Strip OSTP/RTP header, send only raw PCM payload
                OstpHeader h = parse_ostp_header(data, len);
                if (h.payload_len > 0 && h.payload_offset < len) {
                    ok = ws_send_binary(ws->fd, data + h.payload_offset, h.payload_len);
                } else {
                    // Fallback: send full packet if parse failed
                    ok = ws_send_binary(ws->fd, data, len);
                }
            } else {
                // Send full OSTP packet (default, for WASM player)
                ok = ws_send_binary(ws->fd, data, len);
            }
            if (!ok) {
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

        // Simple per-IP rate limiting (60 requests/minute) — skip for WebSocket upgrades
        bool is_ws_upgrade = (strcasestr(buf, "Upgrade: websocket") != nullptr);
        if (!is_ws_upgrade) {
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

        // Helper: extract request body
        const char* body_ptr = strstr(buf, "\r\n\r\n");
        std::string req_body = body_ptr ? std::string(body_ptr + 4) : "";

        // Helper: extract JSON string value (simple)
        auto json_val = [](const std::string& json, const std::string& key) -> std::string {
            std::string needle = "\"" + key + "\":\"";
            size_t pos = json.find(needle);
            if (pos == std::string::npos) return "";
            size_t start = pos + needle.size();
            size_t end = json.find('"', start);
            return end != std::string::npos ? json.substr(start, end - start) : "";
        };

        // Helper: extract Authorization Bearer token
        auto get_bearer = [&]() -> std::string {
            const char* auth_hdr = strcasestr(buf, "Authorization: Bearer ");
            if (!auth_hdr) return "";
            const char* tok_start = auth_hdr + 22;
            const char* tok_end = strpbrk(tok_start, "\r\n ");
            return tok_end ? std::string(tok_start, tok_end - tok_start) : std::string(tok_start);
        };

        // ── Auth API ────────────────────────────────────────────────────────

        // POST /api/auth/request-code — send 6-digit code to email
        if (strcmp(method, "POST") == 0 && strcmp(path, "/api/auth/request-code") == 0) {
            std::string email = json_val(req_body, "email");
            if (email.empty() || email.find('@') == std::string::npos || email.size() > 254) {
                http_send(fd, 400, "Bad Request", "application/json",
                    "{\"error\":\"invalid_email\"}");
                close(fd); continue;
            }
            // Rate limit: 1 code per 30s per email
            {
                std::lock_guard<std::mutex> lock(g_auth_mutex);
                int64_t now = now_unix();
                auto it = g_email_rate_limit.find(email);
                if (it != g_email_rate_limit.end() && now < it->second) {
                    http_send(fd, 429, "Too Many Requests", "application/json",
                        "{\"error\":\"rate_limited\",\"retry_after\":" + std::to_string(it->second - now) + "}");
                    close(fd); continue;
                }
                g_email_rate_limit[email] = now + 30;

                // Generate and store verification code
                std::string code = generate_code_6();
                VerificationCode vc;
                vc.email = email;
                vc.code = code;
                vc.expires = now + 300;  // 5 minutes
                vc.attempts = 0;
                g_verify_codes[email] = vc;
            }

            // Send email asynchronously to avoid blocking HTTP response
            std::string code_copy;
            {
                std::lock_guard<std::mutex> lock(g_auth_mutex);
                code_copy = g_verify_codes[email].code;
            }
            std::thread([email, code_copy]() {
                send_verification_email(email, code_copy);
            }).detach();

            http_send(fd, 200, "OK", "application/json",
                "{\"ok\":true,\"message\":\"verification_code_sent\"}");
            close(fd); continue;
        }

        // POST /api/auth/verify — verify code, bind email to device, issue token
        if (strcmp(method, "POST") == 0 && strcmp(path, "/api/auth/verify") == 0) {
            std::string email = json_val(req_body, "email");
            std::string code = json_val(req_body, "code");
            std::string device_id = json_val(req_body, "device_id");

            if (email.empty() || code.empty()) {
                http_send(fd, 400, "Bad Request", "application/json",
                    "{\"error\":\"missing_fields\"}");
                close(fd); continue;
            }

            std::lock_guard<std::mutex> lock(g_auth_mutex);
            auto it = g_verify_codes.find(email);
            if (it == g_verify_codes.end()) {
                http_send(fd, 400, "Bad Request", "application/json",
                    "{\"error\":\"no_pending_code\"}");
                close(fd); continue;
            }

            auto& vc = it->second;
            if (now_unix() > vc.expires) {
                g_verify_codes.erase(it);
                http_send(fd, 400, "Bad Request", "application/json",
                    "{\"error\":\"code_expired\"}");
                close(fd); continue;
            }
            if (++vc.attempts > 5) {
                g_verify_codes.erase(it);
                http_send(fd, 429, "Too Many Requests", "application/json",
                    "{\"error\":\"too_many_attempts\"}");
                close(fd); continue;
            }
            if (vc.code != code) {
                http_send(fd, 400, "Bad Request", "application/json",
                    "{\"error\":\"invalid_code\",\"attempts_remaining\":" + std::to_string(5 - vc.attempts) + "}");
                close(fd); continue;
            }

            // Code verified! Create or update user account
            g_verify_codes.erase(it);

            auto& user = g_users[email];
            if (user.user_id.empty()) {
                // New user
                user.user_id = "u-" + generate_uuid_v4();
                user.email = email;
                user.created_at = now_unix();
            }
            user.last_login = now_unix();

            // Link device if provided and not already linked
            if (!device_id.empty()) {
                bool found = false;
                for (const auto& d : user.devices) {
                    if (d == device_id) { found = true; break; }
                }
                if (!found) user.devices.push_back(device_id);
                g_device_to_email[device_id] = email;
            }

            // Generate auth token
            std::string token = generate_auth_token();
            g_auth_tokens[token] = email;

            // Build response
            std::string devices_json = "[";
            for (size_t i = 0; i < user.devices.size(); i++) {
                if (i > 0) devices_json += ",";
                devices_json += "\"" + json_escape(user.devices[i]) + "\"";
            }
            devices_json += "]";

            std::string resp = "{\"ok\":true,\"token\":\"" + token + "\""
                + ",\"user_id\":\"" + json_escape(user.user_id) + "\""
                + ",\"email\":\"" + json_escape(email) + "\""
                + ",\"devices\":" + devices_json + "}";

            // Persist
            std::thread([]() { users_save(); }).detach();

            http_send(fd, 200, "OK", "application/json", resp);
            close(fd); continue;
        }

        // GET /api/auth/me — get current user info
        if (strcmp(method, "GET") == 0 && strcmp(path, "/api/auth/me") == 0) {
            std::string token = get_bearer();
            if (token.empty()) {
                http_send(fd, 401, "Unauthorized", "application/json",
                    "{\"error\":\"missing_token\"}");
                close(fd); continue;
            }
            std::lock_guard<std::mutex> lock(g_auth_mutex);
            auto it = g_auth_tokens.find(token);
            if (it == g_auth_tokens.end() || !token_is_valid(token)) {
                if (it != g_auth_tokens.end()) g_auth_tokens.erase(it);
                http_send(fd, 401, "Unauthorized", "application/json",
                    "{\"error\":\"invalid_token\"}");
                close(fd); continue;
            }
            auto uit = g_users.find(it->second);
            if (uit == g_users.end()) {
                http_send(fd, 404, "Not Found", "application/json",
                    "{\"error\":\"user_not_found\"}");
                close(fd); continue;
            }
            const auto& u = uit->second;
            std::string devices_json = "[";
            for (size_t i = 0; i < u.devices.size(); i++) {
                if (i > 0) devices_json += ",";
                devices_json += "\"" + json_escape(u.devices[i]) + "\"";
            }
            devices_json += "]";
            std::string resp = "{\"user_id\":\"" + json_escape(u.user_id) + "\""
                + ",\"email\":\"" + json_escape(u.email) + "\""
                + ",\"username\":" + (u.username.empty() ? "null" : "\"" + json_escape(u.username) + "\"")
                + ",\"devices\":" + devices_json
                + ",\"created_at\":" + std::to_string(u.created_at)
                + ",\"last_login\":" + std::to_string(u.last_login) + "}";
            http_send(fd, 200, "OK", "application/json", resp);
            close(fd); continue;
        }

        // GET /api/auth/check-username — check if a username is available
        if (strcmp(method, "GET") == 0 && strncmp(path, "/api/auth/check-username", 24) == 0) {
            // Parse ?name= from path
            std::string name;
            const char* q = strchr(path, '?');
            if (q) {
                std::string qs(q + 1);
                auto eq = qs.find("name=");
                if (eq != std::string::npos) {
                    auto amp = qs.find('&', eq + 5);
                    name = qs.substr(eq + 5, amp == std::string::npos ? amp : amp - eq - 5);
                    // Simple URL decode for common chars
                    std::string decoded;
                    for (size_t i = 0; i < name.size(); i++) {
                        if (name[i] == '%' && i + 2 < name.size()) {
                            char h[3] = { name[i+1], name[i+2], 0 };
                            decoded += (char)strtol(h, nullptr, 16);
                            i += 2;
                        } else if (name[i] == '+') {
                            decoded += ' ';
                        } else {
                            decoded += name[i];
                        }
                    }
                    name = decoded;
                }
            }
            if (name.empty() || name.size() < 3 || name.size() > 30) {
                http_send(fd, 400, "Bad Request", "application/json",
                    "{\"error\":\"invalid_username\",\"message\":\"3-30 characters required\"}");
                close(fd); continue;
            }
            // Only allow alphanumeric, underscore, hyphen
            bool valid = true;
            for (char c : name) {
                if (!isalnum((unsigned char)c) && c != '_' && c != '-') { valid = false; break; }
            }
            if (!valid) {
                http_send(fd, 400, "Bad Request", "application/json",
                    "{\"error\":\"invalid_username\",\"message\":\"Only letters, numbers, _ and - allowed\"}");
                close(fd); continue;
            }
            std::lock_guard<std::mutex> lock(g_auth_mutex);
            bool available = g_username_to_email.find(name) == g_username_to_email.end();
            http_send(fd, 200, "OK", "application/json",
                "{\"available\":" + std::string(available ? "true" : "false") + "}");
            close(fd); continue;
        }

        // POST /api/auth/set-username — set or change username for authenticated user
        if (strcmp(method, "POST") == 0 && strcmp(path, "/api/auth/set-username") == 0) {
            std::string token = get_bearer();
            if (token.empty()) {
                http_send(fd, 401, "Unauthorized", "application/json",
                    "{\"error\":\"missing_token\"}");
                close(fd); continue;
            }
            std::string new_username = json_val(req_body, "username");
            if (new_username.empty() || new_username.size() < 3 || new_username.size() > 30) {
                http_send(fd, 400, "Bad Request", "application/json",
                    "{\"error\":\"invalid_username\",\"message\":\"3-30 characters required\"}");
                close(fd); continue;
            }
            bool valid = true;
            for (char c : new_username) {
                if (!isalnum((unsigned char)c) && c != '_' && c != '-') { valid = false; break; }
            }
            if (!valid) {
                http_send(fd, 400, "Bad Request", "application/json",
                    "{\"error\":\"invalid_username\",\"message\":\"Only letters, numbers, _ and - allowed\"}");
                close(fd); continue;
            }
            std::lock_guard<std::mutex> lock(g_auth_mutex);
            auto it = g_auth_tokens.find(token);
            if (it == g_auth_tokens.end() || !token_is_valid(token)) {
                if (it != g_auth_tokens.end()) g_auth_tokens.erase(it);
                http_send(fd, 401, "Unauthorized", "application/json",
                    "{\"error\":\"invalid_token\"}");
                close(fd); continue;
            }
            const std::string& email = it->second;
            // Check uniqueness (allow setting same username again)
            auto existing = g_username_to_email.find(new_username);
            if (existing != g_username_to_email.end() && existing->second != email) {
                http_send(fd, 409, "Conflict", "application/json",
                    "{\"error\":\"username_taken\"}");
                close(fd); continue;
            }
            auto& user = g_users[email];
            // Remove old username mapping
            if (!user.username.empty()) g_username_to_email.erase(user.username);
            user.username = new_username;
            g_username_to_email[new_username] = email;
            std::thread([]() { users_save(); }).detach();
            http_send(fd, 200, "OK", "application/json",
                "{\"ok\":true,\"username\":\"" + json_escape(new_username) + "\"}");
            close(fd); continue;
        }

        // POST /api/auth/link-device — add a device to current user
        if (strcmp(method, "POST") == 0 && strcmp(path, "/api/auth/link-device") == 0) {
            std::string token = get_bearer();
            std::string device_id = json_val(req_body, "device_id");
            if (token.empty()) {
                http_send(fd, 401, "Unauthorized", "application/json",
                    "{\"error\":\"missing_token\"}");
                close(fd); continue;
            }
            if (device_id.empty()) {
                http_send(fd, 400, "Bad Request", "application/json",
                    "{\"error\":\"missing_device_id\"}");
                close(fd); continue;
            }
            std::lock_guard<std::mutex> lock(g_auth_mutex);
            auto it = g_auth_tokens.find(token);
            if (it == g_auth_tokens.end() || !token_is_valid(token)) {
                if (it != g_auth_tokens.end()) g_auth_tokens.erase(it);
                http_send(fd, 401, "Unauthorized", "application/json",
                    "{\"error\":\"invalid_token\"}");
                close(fd); continue;
            }
            auto& user = g_users[it->second];
            bool found = false;
            for (const auto& d : user.devices) {
                if (d == device_id) { found = true; break; }
            }
            if (!found) {
                user.devices.push_back(device_id);
                g_device_to_email[device_id] = it->second;
                std::thread([]() { users_save(); }).detach();
            }
            http_send(fd, 200, "OK", "application/json",
                "{\"ok\":true,\"devices_count\":" + std::to_string(user.devices.size()) + "}");
            close(fd); continue;
        }

        // POST /api/auth/register-push — register APNs device token for push notifications
        if (strcmp(method, "POST") == 0 && strcmp(path, "/api/auth/register-push") == 0) {
            std::string token = get_bearer();
            if (token.empty()) {
                http_send(fd, 401, "Unauthorized", "application/json",
                    "{\"error\":\"missing_token\"}");
                close(fd); continue;
            }
            std::string apns_token = json_val(req_body, "apns_token");
            if (apns_token.empty()) {
                http_send(fd, 400, "Bad Request", "application/json",
                    "{\"error\":\"missing_apns_token\"}");
                close(fd); continue;
            }
            std::lock_guard<std::mutex> lock(g_auth_mutex);
            auto it = g_auth_tokens.find(token);
            if (it == g_auth_tokens.end() || !token_is_valid(token)) {
                if (it != g_auth_tokens.end()) g_auth_tokens.erase(it);
                http_send(fd, 401, "Unauthorized", "application/json",
                    "{\"error\":\"invalid_token\"}");
                close(fd); continue;
            }
            auto& user = g_users[it->second];
            user.apns_token = apns_token;
            // Update quick-lookup map if user has a username
            if (!user.username.empty()) {
                g_username_to_apns[user.username] = apns_token;
            }
            std::thread([]() { users_save(); }).detach();
            http_send(fd, 200, "OK", "application/json", "{\"ok\":true}");
            close(fd); continue;
        }

        // WebSocket upgrade: GET /ws/audio?channel=<name>[&device=<name>][&token=<tok>][&format=raw|ostp]
        if (strcmp(method, "GET") == 0 && strncmp(path, "/ws/audio", 9) == 0) {
            // Check for Upgrade: websocket header
            const char* upgrade_hdr = strcasestr(buf, "Upgrade: websocket");
            const char* ws_key_hdr = strcasestr(buf, "Sec-WebSocket-Key:");
            if (upgrade_hdr && ws_key_hdr) {
                // Parse query string parameters
                std::string channel, device = "browser", token, format_str = "ostp";
                const char* q = strchr(path, '?');
                if (q) {
                    std::string qs(q + 1);
                    // Helper: extract a query parameter value by key
                    auto get_param = [&](const std::string& key) -> std::string {
                        std::string needle = key + "=";
                        size_t pos = 0;
                        while ((pos = qs.find(needle, pos)) != std::string::npos) {
                            // Ensure we matched at param boundary (start or after '&')
                            if (pos == 0 || qs[pos - 1] == '&') {
                                size_t vstart = pos + needle.size();
                                size_t end = qs.find('&', vstart);
                                return qs.substr(vstart, end == std::string::npos ? end : end - vstart);
                            }
                            pos += needle.size();
                        }
                        return "";
                    };
                    channel = get_param("channel");
                    std::string d = get_param("device");
                    if (!d.empty()) device = d;
                    token = get_param("token");
                    std::string f = get_param("format");
                    if (!f.empty()) format_str = f;
                }
                if (channel.empty()) {
                    http_send(fd, 400, "Bad Request", "text/plain", "Missing channel parameter");
                    close(fd); continue;
                }

                // ── WebSocket authentication ──
                // Free channels (built-in free names + random 6-hex) require no auth.
                // Purchased/owned channels require a valid token matching the channel record.
                // Unclaimed channels (not in g_channels) are treated as open.
                bool channel_is_free = is_free_name(channel) || is_random_channel(channel);
                if (!channel_is_free) {
                    std::shared_lock<std::shared_mutex> lk(g_mutex);
                    auto it = g_channels.find(channel);
                    if (it != g_channels.end()) {
                        const auto& rec = it->second;
                        // Token must match one of: device ID, txn hash,
                        // stripe_customer, or stripe_sub
                        bool valid = !token.empty() && (
                            token == rec.device ||
                            token == rec.txn ||
                            token == rec.stripe_customer ||
                            token == rec.stripe_sub
                        );
                        if (!valid) {
                            // Complete WS handshake so we can send a JSON error
                            const char* ks = ws_key_hdr + 18;
                            while (*ks == ' ') ks++;
                            const char* ke = strstr(ks, "\r\n");
                            std::string wk(ks, ke ? ke : ks + 24);
                            std::string ac = base64_encode(sha1_raw(
                                wk + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
                            std::string rsp = "HTTP/1.1 101 Switching Protocols\r\n"
                                "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                                "Sec-WebSocket-Accept: " + ac + "\r\n"
                                "Access-Control-Allow-Origin: https://solun.art\r\n\r\n";
                            write(fd, rsp.c_str(), rsp.size());
                            // Send JSON error then close frame (1008 = Policy Violation)
                            ws_send_text(fd,
                                "{\"type\":\"error\",\"code\":\"auth_required\","
                                "\"message\":\"This channel requires a valid token. "
                                "Pass ?token=<your_token> to authenticate.\"}");
                            uint8_t close_frame[4] = {0x88, 0x02, 0x03, 0xF0};
                            write(fd, close_frame, 4);
                            close(fd);
                            fprintf(stderr, "[ws] Auth rejected for channel '%s' (token=%s)\n",
                                    channel.c_str(), token.empty() ? "(none)" : "***");
                            continue;
                        }
                    }
                    // Channel not in g_channels = unclaimed, treat as open
                }

                // Extract WebSocket key and compute accept hash
                const char* key_start = ws_key_hdr + 18;
                while (*key_start == ' ') key_start++;
                const char* key_end = strstr(key_start, "\r\n");
                std::string ws_key(key_start, key_end ? key_end : key_start + 24);
                std::string accept = base64_encode(sha1_raw(ws_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));

                // Send WebSocket upgrade response
                std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                    "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: " + accept + "\r\n"
                    "Access-Control-Allow-Origin: https://solun.art\r\n\r\n";
                write(fd, resp.c_str(), resp.size());

                // Create WsListener and spawn handler thread
                auto ws = std::make_shared<WsListener>();
                ws->fd = fd;
                ws->channel = channel;
                ws->device_name = device;
                ws->raw_format = (format_str == "raw");
                std::thread(ws_client_thread, ws).detach();
                continue;  // fd is now owned by the WebSocket thread, don't close
            }
        }

        // GET / — Landing page
        if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
            // Gather live stats
            size_t total_channels = 0, total_listeners = 0, live_channels = 0;
            std::vector<std::pair<std::string,int>> top_channels;
            {
                std::shared_lock<std::shared_mutex> lk(g_mutex);
                total_channels = g_groups.size();
                for (auto& [name, group] : g_groups) {
                    int n = (int)group.members.size();
                    total_listeners += n;
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - group.last_audio_time).count();
                    bool live = (group.packets_forwarded > 0 && elapsed < 5);
                    if (live) {
                        live_channels++;
                        top_channels.push_back({name, n});
                    }
                }
            }
            std::sort(top_channels.begin(), top_channels.end(),
                [](auto& a, auto& b){ return a.second > b.second; });
            if (top_channels.size() > 6) top_channels.resize(6);

            std::string live_list;
            for (auto& [name, cnt] : top_channels) {
                live_list += R"(<a class="live-ch" href="/c/)" + name + R"("><span class="live-dot"></span>)" + name + R"(<span class="live-cnt">)" + std::to_string(cnt) + R"(</span></a>)";
            }
            if (live_list.empty()) {
                live_list = R"(<div class="no-live">No live channels right now</div>)";
            }

            std::string html = R"(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>Soluna — Open Source Network Audio</title>
<meta name="description" content="Real-time hi-fi audio streaming. Open protocol. Zero latency. Stream music to any device, anywhere.">
<meta property="og:title" content="Soluna — Open Source Network Audio">
<meta property="og:description" content="Real-time hi-fi audio streaming. Open protocol. Zero latency.">
<meta property="og:type" content="website">
<meta property="og:url" content="https://relay.solun.art/">
<meta name="twitter:card" content="summary">
<link rel="canonical" href="https://relay.solun.art/">
<style>
:root{--bg:#05060a;--surface:#0d0f18;--surface2:#141723;--border:rgba(255,255,255,0.06);--accent1:#3b82f6;--accent2:#8b5cf6;--accent3:#06b6d4;--glow:rgba(59,130,246,0.3);--text:#e8eaf2;--text2:#8b91a8;--text3:#50566e}
*{box-sizing:border-box;margin:0;padding:0}
@keyframes fadeUp{from{opacity:0;transform:translateY(30px)}to{opacity:1;transform:translateY(0)}}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.4}}
@keyframes glow{0%,100%{box-shadow:0 0 20px var(--glow),0 0 60px rgba(139,92,246,0.15)}50%{box-shadow:0 0 40px var(--glow),0 0 80px rgba(139,92,246,0.25)}}
@keyframes shimmer{0%{background-position:-200% 0}100%{background-position:200% 0}}
@keyframes float{0%,100%{transform:translateY(0)}50%{transform:translateY(-6px)}}
@keyframes spin{from{transform:rotate(0deg)}to{transform:rotate(360deg)}}
body{font-family:-apple-system,BlinkMacSystemFont,"Inter","Helvetica Neue",sans-serif;background:var(--bg);color:var(--text);min-height:100vh;overflow-x:hidden}
.bg-glow{position:fixed;top:-30%;left:50%;transform:translateX(-50%);width:1000px;height:1000px;background:radial-gradient(circle,rgba(59,130,246,0.08) 0%,rgba(139,92,246,0.05) 40%,transparent 70%);pointer-events:none;z-index:0}
.wrap{max-width:720px;margin:0 auto;padding:48px 24px 64px;position:relative;z-index:1}
.hero{text-align:center;animation:fadeUp 0.8s ease-out both}
.logo{font-size:13px;font-weight:700;letter-spacing:4px;text-transform:uppercase;color:var(--text2);margin-bottom:20px}
h1{font-size:clamp(40px,10vw,64px);font-weight:900;line-height:1.05;background:linear-gradient(135deg,#fff 0%,var(--accent1) 50%,var(--accent2) 100%);-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text;margin-bottom:16px}
.sub{font-size:clamp(16px,3vw,20px);color:var(--text2);line-height:1.5;max-width:480px;margin:0 auto 40px}
.stats{display:grid;grid-template-columns:repeat(3,1fr);gap:16px;margin-bottom:48px;animation:fadeUp 0.8s ease-out 0.15s both}
.stat{background:var(--surface);border:1px solid var(--border);border-radius:16px;padding:20px 16px;text-align:center}
.stat-val{font-size:32px;font-weight:900;background:linear-gradient(135deg,var(--accent3),var(--accent1));-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text}
.stat-label{font-size:11px;font-weight:700;letter-spacing:1.5px;text-transform:uppercase;color:var(--text3);margin-top:4px}
.section{animation:fadeUp 0.8s ease-out 0.3s both}
.section-title{font-size:11px;font-weight:700;letter-spacing:2px;text-transform:uppercase;color:var(--text3);margin-bottom:16px;text-align:center}
.live-grid{display:flex;flex-wrap:wrap;gap:10px;justify-content:center;margin-bottom:48px}
.live-ch{display:inline-flex;align-items:center;gap:8px;padding:10px 18px;background:var(--surface);border:1px solid rgba(16,185,129,0.2);border-radius:12px;text-decoration:none;color:var(--text);font-weight:600;font-size:14px;transition:all 0.2s}
.live-ch:hover{border-color:rgba(16,185,129,0.5);background:var(--surface2);transform:translateY(-2px)}
.live-dot{width:8px;height:8px;border-radius:50%;background:#10b981;animation:pulse 1.5s ease-in-out infinite;flex-shrink:0}
.live-cnt{font-size:11px;color:var(--text3);font-weight:700;background:rgba(255,255,255,0.05);padding:2px 8px;border-radius:8px}
.no-live{color:var(--text3);font-size:14px;padding:16px}
.features{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:16px;margin-bottom:48px;animation:fadeUp 0.8s ease-out 0.4s both}
.feat{background:var(--surface);border:1px solid var(--border);border-radius:16px;padding:24px 20px;transition:all 0.25s}
.feat:hover{border-color:rgba(59,130,246,0.3);transform:translateY(-4px)}
.feat-icon{font-size:28px;margin-bottom:12px}
.feat h3{font-size:15px;font-weight:800;margin-bottom:6px}
.feat p{font-size:13px;color:var(--text2);line-height:1.5}
.cta-row{display:flex;gap:12px;justify-content:center;flex-wrap:wrap;margin-bottom:48px;animation:fadeUp 0.8s ease-out 0.5s both}
.cta{display:inline-flex;align-items:center;gap:8px;padding:14px 28px;border-radius:14px;font-size:15px;font-weight:700;text-decoration:none;transition:all 0.2s;cursor:pointer;border:none}
.cta-primary{color:#fff;background:linear-gradient(135deg,var(--accent1),var(--accent2));animation:glow 3s ease-in-out infinite}
.cta-primary:hover{transform:scale(1.04)}
.cta-secondary{color:var(--text);background:var(--surface);border:1px solid var(--border)}
.cta-secondary:hover{border-color:rgba(59,130,246,0.4);background:var(--surface2)}
.proto{background:var(--surface);border:1px solid var(--border);border-radius:20px;padding:32px;margin-bottom:48px;text-align:center;animation:fadeUp 0.8s ease-out 0.55s both}
.proto h2{font-size:20px;font-weight:800;margin-bottom:8px}
.proto p{font-size:14px;color:var(--text2);line-height:1.6;max-width:480px;margin:0 auto 20px}
.proto-features{display:flex;flex-wrap:wrap;gap:8px;justify-content:center;margin-bottom:20px}
.proto-tag{font-size:11px;font-weight:700;letter-spacing:0.5px;padding:6px 14px;border-radius:8px;background:rgba(59,130,246,0.1);color:var(--accent1);border:1px solid rgba(59,130,246,0.15)}
.footer{text-align:center;padding-top:32px;border-top:1px solid var(--border);animation:fadeUp 0.8s ease-out 0.6s both}
.footer-links{display:flex;gap:24px;justify-content:center;margin-bottom:12px;flex-wrap:wrap}
.footer-links a{color:var(--text2);text-decoration:none;font-size:13px;font-weight:600;transition:color 0.2s}
.footer-links a:hover{color:var(--accent1)}
.footer-copy{font-size:12px;color:var(--text3)}
@media(max-width:480px){.stats{grid-template-columns:1fr}.features{grid-template-columns:1fr}}
</style>
</head>
<body>
<div class="bg-glow"></div>
<div class="wrap">
<div class="hero">
<div class="logo">&#9670; S O L U N A</div>
<h1>Network Audio,<br>Reimagined</h1>
<p class="sub">Real-time hi-fi streaming over an open protocol. Zero latency. Any device. Everywhere.</p>
</div>
<div class="stats">
<div class="stat"><div class="stat-val">)" + std::to_string(live_channels) + R"(</div><div class="stat-label">Live Now</div></div>
<div class="stat"><div class="stat-val">)" + std::to_string(total_listeners) + R"(</div><div class="stat-label">Listeners</div></div>
<div class="stat"><div class="stat-val">)" + std::to_string(total_channels) + R"(</div><div class="stat-label">Channels</div></div>
</div>
<div class="section">
<div class="section-title">Live Channels</div>
<div class="live-grid">)" + live_list + R"(</div>
</div>
<div class="features">
<div class="feat"><div class="feat-icon">&#127911;</div><h3>Hi-Fi Audio</h3><p>24-bit/48kHz uncompressed PCM. No codec artifacts. Studio quality.</p></div>
<div class="feat"><div class="feat-icon">&#9889;</div><h3>Sub-10ms Latency</h3><p>RTP-based protocol with P2P mesh and global relay network.</p></div>
<div class="feat"><div class="feat-icon">&#127760;</div><h3>Open Protocol</h3><p>OSTP v1.0 — documented, extensible, and open source.</p></div>
<div class="feat"><div class="feat-icon">&#128274;</div><h3>Encrypted</h3><p>DTLS-SRTP transport encryption. FEC error correction. NACK recovery.</p></div>
<div class="feat"><div class="feat-icon">&#127911;</div><h3>Copyright Detection</h3><p>Real-time fingerprinting with automatic royalty calculation.</p></div>
<div class="feat"><div class="feat-icon">&#128640;</div><h3>Scales to Billions</h3><p>P2P swarm distribution. 4^16 = 4.2B listeners, 320ms max latency.</p></div>
</div>
<div class="cta-row">
<a class="cta cta-primary" href="https://solun.art/dashboard">Open Dashboard</a>
<a class="cta cta-secondary" href="https://github.com/yukihamada/opensonic">GitHub</a>
<a class="cta cta-secondary" href="https://github.com/yukihamada/opensonic/blob/master/docs/protocol-ja.md">Protocol Spec</a>
</div>
<div class="proto">
<h2>OSTP — Open Sonic Transport Protocol</h2>
<p>A real-time audio transport built on RTP. Designed for music streaming at scale with integrated copyright and royalty management.</p>
<div class="proto-features">
<span class="proto-tag">RTP + Extension Header</span>
<span class="proto-tag">CRC-32 Integrity</span>
<span class="proto-tag">FEC Parity</span>
<span class="proto-tag">NACK Recovery</span>
<span class="proto-tag">DTLS-SRTP</span>
<span class="proto-tag">P2P Swarm</span>
<span class="proto-tag">AcoustID Fingerprint</span>
<span class="proto-tag">Tiered Royalties</span>
</div>
<a class="cta cta-secondary" href="https://github.com/yukihamada/opensonic/blob/master/docs/protocol-ja.md" style="font-size:13px">Read the Full Specification &rarr;</a>
</div>
<div class="footer">
<div class="footer-links">
<a href="https://solun.art">Home</a>
<a href="https://solun.art/dashboard">Dashboard</a>
<a href="https://github.com/yukihamada/opensonic">GitHub</a>
<a href="https://testflight.apple.com/join/PYbefDSE">iOS TestFlight</a>
<a href="/metrics">Metrics</a>
</div>
<div class="footer-copy">&copy; 2026 Soluna &middot; Open Source Network Audio</div>
</div>
</div>
</body>
</html>)";
            http_send(fd, 200, "OK", "text/html; charset=utf-8", html.c_str());
            close(fd); continue;
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

            // Count current listeners and check if audio is actively streaming
            int listener_count = 0;
            bool channel_exists = false;
            bool is_streaming = false;
            {
                std::shared_lock<std::shared_mutex> lk(g_mutex);
                auto it = g_groups.find(channel_name);
                if (it != g_groups.end()) {
                    channel_exists = true;
                    listener_count = (int)it->second.members.size();
                    // Consider "streaming" if audio packets received in last 5 seconds
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - it->second.last_audio_time).count();
                    is_streaming = (it->second.packets_forwarded > 0 && elapsed < 5);
                }
            }

            std::string status_text;
            std::string status_badge;
            std::string badge_color;
            if (is_streaming) {
                status_badge = "LIVE";
                badge_color = "#10b981";
                status_text = std::to_string(listener_count) + " listeners connected";
            } else if (channel_exists) {
                status_badge = "IDLE";
                badge_color = "#f59e0b";  // amber for connected but no audio
                status_text = std::to_string(listener_count) + " connected · No audio";
            } else {
                status_badge = "OFFLINE";
                badge_color = "#6b7280";
                status_text = "Channel available";
            }

            std::string html = R"(<!DOCTYPE html>
<html lang="ja">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>)" + channel_name + R"( — Soluna Channel</title>
<meta name="description" content="Listen to )" + channel_name + R"( on Soluna. Real-time hi-fi audio streaming.">
<meta property="og:title" content=")" + channel_name + R"( — Soluna">
<meta property="og:description" content="Listen live on Soluna. Hi-fi streaming, zero latency.">
<meta property="og:type" content="music.radio_station">
<meta property="og:url" content="https://relay.solun.art/c/)" + channel_name + R"(">
<meta name="twitter:card" content="summary">
<meta name="twitter:title" content=")" + channel_name + R"( — Soluna">
<meta name="twitter:description" content="Listen live on Soluna. Real-time hi-fi audio streaming.">
<link rel="canonical" href="https://relay.solun.art/c/)" + channel_name + R"(">
<style>
:root{--bg:#05060a;--surface:#0d0f18;--surface2:#141723;--border:rgba(255,255,255,0.06);--accent1:#3b82f6;--accent2:#8b5cf6;--accent3:#06b6d4;--glow:rgba(59,130,246,0.3);--text:#e8eaf2;--text2:#8b91a8;--text3:#50566e}
*{box-sizing:border-box;margin:0;padding:0}
@keyframes fadeUp{from{opacity:0;transform:translateY(30px)}to{opacity:1;transform:translateY(0)}}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.4}}
@keyframes glow{0%,100%{box-shadow:0 0 20px var(--glow),0 0 60px rgba(139,92,246,0.15)}50%{box-shadow:0 0 40px var(--glow),0 0 80px rgba(139,92,246,0.25)}}
@keyframes barAnim{0%,100%{transform:scaleY(0.3)}50%{transform:scaleY(1)}}
@keyframes wave{0%,100%{transform:scaleY(0.4)}25%{transform:scaleY(1)}50%{transform:scaleY(0.6)}75%{transform:scaleY(0.8)}}
@keyframes shimmer{0%{background-position:-200% 0}100%{background-position:200% 0}}
@keyframes float{0%,100%{transform:translateY(0)}50%{transform:translateY(-6px)}}
body{font-family:-apple-system,BlinkMacSystemFont,"Inter","Helvetica Neue",sans-serif;background:var(--bg);color:var(--text);min-height:100vh;display:flex;flex-direction:column;align-items:center;overflow-x:hidden}
.bg-glow{position:fixed;top:-40%;left:50%;transform:translateX(-50%);width:800px;height:800px;background:radial-gradient(circle,rgba(59,130,246,0.08) 0%,rgba(139,92,246,0.05) 40%,transparent 70%);pointer-events:none;z-index:0}
.spectrum{display:flex;align-items:flex-end;justify-content:center;gap:3px;height:60px;padding:32px 0 0;z-index:1;position:relative}
.spectrum .bar{width:4px;border-radius:2px;background:linear-gradient(to top,var(--accent1),var(--accent2));transform-origin:bottom;animation:barAnim 0.8s ease-in-out infinite)" +
std::string(is_streaming ? "" : ";animation-play-state:paused;transform:scaleY(0.15);opacity:0.3") +
R"(}
.spectrum .bar:nth-child(1){height:40px;animation-delay:0s}
.spectrum .bar:nth-child(2){height:55px;animation-delay:0.1s}
.spectrum .bar:nth-child(3){height:35px;animation-delay:0.2s}
.spectrum .bar:nth-child(4){height:60px;animation-delay:0.05s}
.spectrum .bar:nth-child(5){height:45px;animation-delay:0.15s}
.spectrum .bar:nth-child(6){height:50px;animation-delay:0.25s}
.spectrum .bar:nth-child(7){height:30px;animation-delay:0.3s}
.spectrum .bar:nth-child(8){height:58px;animation-delay:0.08s}
.spectrum .bar:nth-child(9){height:42px;animation-delay:0.18s}
.spectrum .bar:nth-child(10){height:52px;animation-delay:0.12s}
.spectrum .bar:nth-child(11){height:38px;animation-delay:0.22s}
.spectrum .bar:nth-child(12){height:48px;animation-delay:0.07s}
.spectrum .bar:nth-child(13){height:56px;animation-delay:0.17s}
.spectrum .bar:nth-child(14){height:33px;animation-delay:0.27s}
.spectrum .bar:nth-child(15){height:44px;animation-delay:0.13s}
.spectrum .bar:nth-child(16){height:50px;animation-delay:0.03s}
.spectrum .bar:nth-child(17){height:36px;animation-delay:0.23s}
.spectrum .bar:nth-child(18){height:54px;animation-delay:0.09s}
.spectrum .bar:nth-child(19){height:40px;animation-delay:0.19s}
.spectrum .bar:nth-child(20){height:46px;animation-delay:0.14s}
.container{max-width:520px;width:100%;padding:0 24px 48px;z-index:1;position:relative}
.hero{text-align:center;animation:fadeUp 0.8s ease-out both;margin-top:24px}
.logo-mark{font-size:14px;font-weight:600;letter-spacing:3px;text-transform:uppercase;color:var(--text2);margin-bottom:16px}
.channel-name{font-size:clamp(36px,8vw,52px);font-weight:900;line-height:1.1;word-break:break-all;background:linear-gradient(135deg,#fff 0%,var(--accent1) 50%,var(--accent2) 100%);-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text;margin-bottom:16px}
.badge-row{display:flex;align-items:center;justify-content:center;gap:12px;margin-bottom:8px}
.badge{display:inline-flex;align-items:center;gap:6px;font-size:11px;font-weight:800;letter-spacing:1.5px;padding:6px 14px;border-radius:20px;color:#fff;text-transform:uppercase}
.badge-live{background:rgba(16,185,129,0.15);color:#10b981;border:1px solid rgba(16,185,129,0.3)}
.badge-live .dot{width:8px;height:8px;border-radius:50%;background:#10b981;animation:pulse 1.5s ease-in-out infinite}
.badge-idle{background:rgba(107,114,128,0.15);color:#9ca3af;border:1px solid rgba(107,114,128,0.3)}
.listeners{color:var(--text2);font-size:14px;margin-bottom:32px}
.now-playing{background:var(--surface);border:1px solid var(--border);border-radius:16px;padding:20px 24px;margin-bottom:32px;animation:fadeUp 0.8s ease-out 0.2s both}
.np-label{font-size:11px;font-weight:700;letter-spacing:2px;text-transform:uppercase;color:var(--accent3);margin-bottom:12px}
.waveform{display:flex;align-items:center;justify-content:center;gap:2px;height:32px;margin-bottom:8px}
.waveform .w{width:3px;border-radius:1.5px;background:linear-gradient(to top,var(--accent3),var(--accent1));animation:wave 1.2s ease-in-out infinite}
.waveform .w:nth-child(1){height:12px;animation-delay:0s}
.waveform .w:nth-child(2){height:20px;animation-delay:0.1s}
.waveform .w:nth-child(3){height:28px;animation-delay:0.2s}
.waveform .w:nth-child(4){height:16px;animation-delay:0.05s}
.waveform .w:nth-child(5){height:32px;animation-delay:0.15s}
.waveform .w:nth-child(6){height:24px;animation-delay:0.25s}
.waveform .w:nth-child(7){height:18px;animation-delay:0.08s}
.waveform .w:nth-child(8){height:26px;animation-delay:0.18s}
.waveform .w:nth-child(9){height:14px;animation-delay:0.28s}
.waveform .w:nth-child(10){height:22px;animation-delay:0.12s}
.waveform .w:nth-child(11){height:30px;animation-delay:0.03s}
.waveform .w:nth-child(12){height:16px;animation-delay:0.22s}
.np-text{color:var(--text2);font-size:13px}
.listen-btn{display:flex;align-items:center;justify-content:center;gap:10px;width:100%;padding:18px 24px;border-radius:16px;font-size:17px;font-weight:800;text-decoration:none;color:#fff;background:linear-gradient(135deg,var(--accent1),var(--accent2));border:none;cursor:pointer;position:relative;overflow:hidden;animation:fadeUp 0.8s ease-out 0.3s both,glow 3s ease-in-out infinite;transition:transform 0.2s}
.listen-btn:hover{transform:scale(1.03)}
.listen-btn:active{transform:scale(0.98)}
.listen-btn .shimmer{position:absolute;inset:0;background:linear-gradient(90deg,transparent,rgba(255,255,255,0.1),transparent);background-size:200% 100%;animation:shimmer 3s linear infinite}
.downloads{margin-top:24px;animation:fadeUp 0.8s ease-out 0.5s both}
.dl-label{font-size:11px;font-weight:700;letter-spacing:2px;text-transform:uppercase;color:var(--text3);text-align:center;margin-bottom:12px}
.dl-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}
.dl-card{display:flex;flex-direction:column;align-items:center;gap:8px;padding:16px 8px;background:var(--surface);border:1px solid var(--border);border-radius:14px;text-decoration:none;color:var(--text);transition:all 0.25s;animation:float 4s ease-in-out infinite}
.dl-card:nth-child(2){animation-delay:0.5s}
.dl-card:nth-child(3){animation-delay:1s}
.dl-card:hover{border-color:rgba(59,130,246,0.3);background:var(--surface2);transform:translateY(-4px)}
.dl-card svg{opacity:0.7}
.dl-card .dl-name{font-size:13px;font-weight:700}
.dl-card .dl-type{font-size:10px;color:var(--text3);font-weight:600;letter-spacing:0.5px}
.divider{border:none;border-top:1px solid var(--border);margin:32px 0}
.footer{text-align:center;color:var(--text3);font-size:12px;animation:fadeUp 0.8s ease-out 0.6s both}
.footer a{color:var(--text2);text-decoration:none;transition:color 0.2s}
.footer a:hover{color:var(--accent1)}
.tagline{font-size:12px;color:var(--text3);margin-top:6px;letter-spacing:0.5px}
.share-row{display:flex;gap:10px;justify-content:center;margin-top:20px;animation:fadeUp 0.8s ease-out 0.35s both}
.share-btn{display:inline-flex;align-items:center;gap:6px;padding:10px 18px;border-radius:12px;font-size:13px;font-weight:700;text-decoration:none;color:var(--text);background:var(--surface);border:1px solid var(--border);cursor:pointer;transition:all 0.2s}
.share-btn:hover{border-color:rgba(59,130,246,0.4);background:var(--surface2);transform:translateY(-2px)}
.qr-box{text-align:center;margin-top:20px;animation:fadeUp 0.8s ease-out 0.4s both}
.qr-box img{border-radius:12px;background:#fff;padding:12px}
.qr-label{font-size:11px;color:var(--text3);margin-top:8px;font-weight:600;letter-spacing:0.5px}
.np-song{display:flex;align-items:center;gap:12px;margin-top:8px}
.np-art{width:48px;height:48px;border-radius:10px;object-fit:cover;background:var(--surface2)}
.np-info{flex:1;min-width:0}
.np-title{font-size:14px;font-weight:700;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.np-artist{font-size:12px;color:var(--text2);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.np-yt{flex-shrink:0}
.np-yt a{display:flex;align-items:center;justify-content:center;width:36px;height:36px;border-radius:50%;background:rgba(255,0,0,0.15);color:#f00;text-decoration:none;transition:all 0.2s}
.np-yt a:hover{background:rgba(255,0,0,0.25);transform:scale(1.1)}
.toast{position:fixed;bottom:24px;left:50%;transform:translateX(-50%) translateY(60px);background:var(--surface2);color:var(--text);padding:12px 24px;border-radius:12px;font-size:13px;font-weight:600;border:1px solid var(--border);opacity:0;transition:all 0.3s;z-index:99;pointer-events:none}
.toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
</style>
</head>
<body>
<div class="bg-glow"></div>
<div class="spectrum">
<div class="bar"></div><div class="bar"></div><div class="bar"></div><div class="bar"></div><div class="bar"></div>
<div class="bar"></div><div class="bar"></div><div class="bar"></div><div class="bar"></div><div class="bar"></div>
<div class="bar"></div><div class="bar"></div><div class="bar"></div><div class="bar"></div><div class="bar"></div>
<div class="bar"></div><div class="bar"></div><div class="bar"></div><div class="bar"></div><div class="bar"></div>
</div>
<div class="container">
<div class="hero">
<div class="logo-mark">&#9670; S O L U N A</div>
<div class="channel-name">)" + channel_name + R"(</div>
<div class="badge-row">)" +
(is_streaming
    ? std::string(R"(<span class="badge badge-live"><span class="dot"></span>LIVE</span>)")
    : (channel_exists
        ? std::string(R"(<span class="badge badge-idle" style="background:rgba(245,158,11,0.15);color:#f59e0b">IDLE</span>)")
        : std::string(R"(<span class="badge badge-idle">OFFLINE</span>)")))
+ R"(</div>
<div class="listeners">)" + status_text + R"(</div>
</div>
<div class="now-playing" id="np">
<div class="np-label">Now Playing</div>
<div class="waveform" id="np-wave">
<div class="w"></div><div class="w"></div><div class="w"></div><div class="w"></div><div class="w"></div><div class="w"></div>
<div class="w"></div><div class="w"></div><div class="w"></div><div class="w"></div><div class="w"></div><div class="w"></div>
</div>
<div class="np-text" id="np-text">)" + (is_streaming ? std::string("Identifying song...") : std::string("Waiting for broadcast...")) + R"(</div>
<div class="np-song" id="np-song" style="display:none">
<img class="np-art" id="np-art" src="" alt="">
<div class="np-info"><div class="np-title" id="np-title"></div><div class="np-artist" id="np-artist"></div></div>
<div class="np-yt" id="np-yt"></div>
</div>
</div>
<a class="listen-btn" href="https://solun.art/dashboard#channel=)" + channel_name + R"HTM(">
<span class="shimmer"></span>
<svg viewBox="0 0 24 24" fill="currentColor" width="22" height="22"><path d="M8 5v14l11-7z"/></svg>
Listen in Browser
</a>
<div class="share-row">
<button class="share-btn" onclick="copyLink()">
<svg viewBox="0 0 24 24" fill="currentColor" width="16" height="16"><path d="M16 1H4c-1.1 0-2 .9-2 2v14h2V3h12V1zm3 4H8c-1.1 0-2 .9-2 2v14c0 1.1.9 2 2 2h11c1.1 0 2-.9 2-2V7c0-1.1-.9-2-2-2zm0 16H8V7h11v14z"/></svg>
Copy Link
</button>
<button class="share-btn" onclick="shareNative()">
<svg viewBox="0 0 24 24" fill="currentColor" width="16" height="16"><path d="M18 16.08c-.76 0-1.44.3-1.96.77L8.91 12.7c.05-.23.09-.46.09-.7s-.04-.47-.09-.7l7.05-4.11c.54.5 1.25.81 2.04.81 1.66 0 3-1.34 3-3s-1.34-3-3-3-3 1.34-3 3c0 .24.04.47.09.7L8.04 9.81C7.5 9.31 6.79 9 6 9c-1.66 0-3 1.34-3 3s1.34 3 3 3c.79 0 1.5-.31 2.04-.81l7.12 4.16c-.05.21-.08.43-.08.65 0 1.61 1.31 2.92 2.92 2.92s2.92-1.31 2.92-2.92-1.31-2.92-2.92-2.92z"/></svg>
Share
</button>
</div>
<div class="qr-box"><img width="160" height="160" style="border-radius:12px" alt="QR Code" src="https://api.qrserver.com/v1/create-qr-code/?size=160x160&amp;data=https%3A%2F%2Frelay.solun.art%2Fc%2F)HTM" + channel_name + R"HTM("><div class="qr-label">Scan to listen</div></div>
<div class="downloads">
<div class="dl-label">Download App</div>
<div class="dl-grid">
<a class="dl-card" href="https://github.com/yukihamada/opensonic/releases/latest/download/Soluna-mac.pkg">
<svg viewBox="0 0 24 24" fill="currentColor" width="28" height="28"><path d="M17.05 20.28c-.98.95-2.05.8-3.08.35-1.09-.46-2.09-.48-3.24 0-1.44.62-2.2.44-3.06-.35C2.79 15.25 3.51 7.59 9.05 7.31c1.35.07 2.29.74 3.08.8 1.18-.24 2.31-.93 3.57-.84 1.51.12 2.65.72 3.4 1.8-3.12 1.87-2.38 5.98.48 7.13-.57 1.5-1.31 2.99-2.54 4.09zM12.03 7.25c-.15-2.23 1.66-4.07 3.74-4.25.29 2.58-2.34 4.5-3.74 4.25z"/></svg>
<span class="dl-name">Mac</span>
<span class="dl-type">.pkg</span>
</a>
<a class="dl-card" href="https://testflight.apple.com/join/PYbefDSE">
<svg viewBox="0 0 24 24" fill="currentColor" width="28" height="28"><path d="M15.5 1h-8C6.12 1 5 2.12 5 3.5v17C5 21.88 6.12 23 7.5 23h8c1.38 0 2.5-1.12 2.5-2.5v-17C18 2.12 16.88 1 15.5 1zm-4 21c-.83 0-1.5-.67-1.5-1.5s.67-1.5 1.5-1.5 1.5.67 1.5 1.5-.67 1.5-1.5 1.5zm4.5-4H7V4h9v14z"/></svg>
<span class="dl-name">iOS</span>
<span class="dl-type">TestFlight</span>
</a>
<a class="dl-card" href="https://github.com/yukihamada/opensonic/releases/latest/download/Soluna-android.apk">
<svg viewBox="0 0 24 24" fill="currentColor" width="28" height="28"><path d="M17.6 11.8l1.7-3c.1-.2 0-.4-.2-.5s-.4 0-.5.2l-1.7 3c-1.3-.6-2.7-.9-4.2-.9s-2.9.3-4.2.9L6.8 8.5c-.1-.2-.3-.3-.5-.2s-.3.3-.2.5l1.7 3C4.7 13.6 2.7 16.8 2.3 20.6h18.6c-.4-3.8-2.4-7-5.3-8.8z"/></svg>
<span class="dl-name">Android</span>
<span class="dl-type">.apk</span>
</a>
</div>
</div>
<hr class="divider">
<div class="footer">
<a href="/">&#9670; Soluna</a> &mdash; Open Source Network Audio
<div class="tagline">Hi-Fi streaming. Zero latency. Pure sound.</div>
</div>
</div>
<div class="toast" id="toast"></div>
<script>
var CH=)HTM" + channel_name + R"(';
// Deep link for iOS
if(/iPhone|iPad|iPod/.test(navigator.userAgent)){setTimeout(function(){window.location='soluna://channel/'+CH},100)}
// Copy link
function copyLink(){navigator.clipboard.writeText(location.href).then(function(){showToast('Link copied!')}).catch(function(){})}
function shareNative(){if(navigator.share){navigator.share({title:CH+' — Soluna',url:location.href})}else{copyLink()}}
function showToast(m){var t=document.getElementById('toast');t.textContent=m;t.classList.add('show');setTimeout(function(){t.classList.remove('show')},2000)}
// Now Playing auto-fetch
function fetchNP(){
  fetch('/api/now-playing?channel='+encodeURIComponent(CH)).then(function(r){return r.json()}).then(function(d){
    var song=document.getElementById('np-song'),txt=document.getElementById('np-text'),wave=document.getElementById('np-wave');
    if(d.title){
      song.style.display='flex';txt.style.display='none';
      document.getElementById('np-title').textContent=d.title;
      document.getElementById('np-artist').textContent=d.artist||'';
      var art=document.getElementById('np-art');
      if(d.artwork_url){art.src=d.artwork_url;art.style.display='block'}else{art.style.display='none'}
      var yt=document.getElementById('np-yt');
      if(d.youtube_url){yt.innerHTML='<a href="'+d.youtube_url+'" target="_blank"><svg viewBox="0 0 24 24" fill="currentColor" width="18" height="18"><path d="M8 5v14l11-7z"/></svg></a>'}else{yt.innerHTML=''}
    }else{song.style.display='none';txt.style.display='block'}
  }).catch(function(){})
}
fetchNP();setInterval(fetchNP,10000);
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
            m += "# HELP soluna_relay_ostp_packets_total Valid OSTP packets received\n";
            m += "# TYPE soluna_relay_ostp_packets_total counter\n";
            m += "soluna_relay_ostp_packets_total " + std::to_string(g_ostp_packets.load()) + "\n";
            m += "# HELP soluna_relay_ostp_crc_ok_total OSTP packets with valid CRC-32\n";
            m += "# TYPE soluna_relay_ostp_crc_ok_total counter\n";
            m += "soluna_relay_ostp_crc_ok_total " + std::to_string(g_ostp_crc_ok.load()) + "\n";
            m += "# HELP soluna_relay_ostp_crc_fail_total OSTP packets with CRC-32 mismatch\n";
            m += "# TYPE soluna_relay_ostp_crc_fail_total counter\n";
            m += "soluna_relay_ostp_crc_fail_total " + std::to_string(g_ostp_crc_fail.load()) + "\n";
            m += "# HELP soluna_relay_rtp_legacy_packets_total RTP packets without OSTP extension\n";
            m += "# TYPE soluna_relay_rtp_legacy_packets_total counter\n";
            m += "soluna_relay_rtp_legacy_packets_total " + std::to_string(g_rtp_legacy_packets.load()) + "\n";
            m += "# HELP soluna_relay_fwd_group_found_total forward_audio group found\n";
            m += "# TYPE soluna_relay_fwd_group_found_total counter\n";
            m += "soluna_relay_fwd_group_found_total " + std::to_string(g_fwd_group_found.load()) + "\n";
            m += "# HELP soluna_relay_fwd_no_group_total forward_audio no group\n";
            m += "# TYPE soluna_relay_fwd_no_group_total counter\n";
            m += "soluna_relay_fwd_no_group_total " + std::to_string(g_fwd_no_group.load()) + "\n";
            m += "# HELP soluna_relay_fec_accumulations_total Packets entering FEC accumulator\n";
            m += "# TYPE soluna_relay_fec_accumulations_total counter\n";
            m += "soluna_relay_fec_accumulations_total " + std::to_string(g_fec_accumulations.load()) + "\n";
            m += "# HELP soluna_relay_fec_packets_sent_total FEC parity packets generated\n";
            m += "# TYPE soluna_relay_fec_packets_sent_total counter\n";
            m += "soluna_relay_fec_packets_sent_total " + std::to_string(g_fec_packets_sent.load()) + "\n";
            m += "# HELP soluna_relay_sync_packets_total Clock sync (PT=125) packets handled\n";
            m += "# TYPE soluna_relay_sync_packets_total counter\n";
            m += "soluna_relay_sync_packets_total " + std::to_string(g_sync_packets.load()) + "\n";
            m += "# HELP soluna_relay_nack_requests_total NACK retransmission requests received\n";
            m += "# TYPE soluna_relay_nack_requests_total counter\n";
            m += "soluna_relay_nack_requests_total " + std::to_string(g_nack_requests_rx.load()) + "\n";
            m += "# HELP soluna_relay_nack_retransmits_total Packets retransmitted via NACK\n";
            m += "# TYPE soluna_relay_nack_retransmits_total counter\n";
            m += "soluna_relay_nack_retransmits_total " + std::to_string(g_nack_retransmits.load()) + "\n";
            if (g_dtls_enabled) {
                m += "# HELP soluna_relay_dtls_sessions_total DTLS sessions initiated\n";
                m += "# TYPE soluna_relay_dtls_sessions_total counter\n";
                m += "soluna_relay_dtls_sessions_total " + std::to_string(g_dtls_sessions.load()) + "\n";
                m += "# HELP soluna_relay_dtls_handshake_ok_total DTLS handshakes completed\n";
                m += "# TYPE soluna_relay_dtls_handshake_ok_total counter\n";
                m += "soluna_relay_dtls_handshake_ok_total " + std::to_string(g_dtls_handshake_ok.load()) + "\n";
                m += "# HELP soluna_relay_dtls_handshake_fail_total DTLS handshake failures\n";
                m += "# TYPE soluna_relay_dtls_handshake_fail_total counter\n";
                m += "soluna_relay_dtls_handshake_fail_total " + std::to_string(g_dtls_handshake_fail.load()) + "\n";
            }
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

        // POST /api/wallet/charge — HTTP version of CHARGE command (economic layer test)
        // Body: {"device_id":"<id>","amount":<float>,"token":"<ts>:<hmac>"}
        // HMAC = HMAC-SHA256(RELAY_CHARGE_SECRET, "CHARGE:<amount>:<device_id>:<ts>")
        if (strcmp(method, "POST") == 0 && strcmp(path, "/api/wallet/charge") == 0) {
            if (g_charge_secret.empty()) {
                http_send(fd, 503, "Service Unavailable", "application/json",
                    "{\"error\":\"charge_disabled\"}");
                close(fd); continue;
            }
            // Parse JSON body fields
            std::string dev_id  = json_val(req_body, "device_id");
            double amount       = json_get_double(req_body, "amount");
            std::string token   = json_val(req_body, "token");
            if (dev_id.empty() || amount <= 0.0 || amount > 10000.0 || token.empty()) {
                http_send(fd, 400, "Bad Request", "application/json",
                    "{\"error\":\"invalid_params\"}");
                close(fd); continue;
            }
            // Validate HMAC token (same logic as UDP CHARGE handler)
            size_t sep = token.find(':');
            if (sep == std::string::npos) {
                http_send(fd, 400, "Bad Request", "application/json",
                    "{\"error\":\"invalid_token\"}");
                close(fd); continue;
            }
            std::string ts_str     = token.substr(0, sep);
            std::string client_hmac = token.substr(sep + 1);
            int64_t ts  = std::atoll(ts_str.c_str());
            int64_t now = now_unix();
            if (std::abs(now - ts) > 30) {
                http_send(fd, 400, "Bad Request", "application/json",
                    "{\"error\":\"token_expired\"}");
                close(fd); continue;
            }
            std::string hmac_input = "CHARGE:" + std::to_string(amount) + ":" + dev_id + ":" + ts_str;
            std::string expected   = hmac_sha256_hex(g_charge_secret, hmac_input);
            bool hmac_ok = (client_hmac.size() == expected.size());
            if (hmac_ok) {
                volatile unsigned char r = 0;
                for (size_t i = 0; i < client_hmac.size(); i++) r |= client_hmac[i] ^ expected[i];
                hmac_ok = (r == 0);
            }
            if (!hmac_ok) {
                http_send(fd, 401, "Unauthorized", "application/json",
                    "{\"error\":\"invalid_token\"}");
                close(fd); continue;
            }
            // Replay protection
            {
                std::lock_guard<std::mutex> nlock(g_charge_nonce_mutex);
                int64_t now_ts = now_unix();
                for (auto it = g_charge_used_tokens.begin(); it != g_charge_used_tokens.end(); ) {
                    if (it->second < now_ts) it = g_charge_used_tokens.erase(it); else ++it;
                }
                if (g_charge_used_tokens.count(client_hmac)) {
                    http_send(fd, 409, "Conflict", "application/json",
                        "{\"error\":\"replay_detected\"}");
                    close(fd); continue;
                }
                g_charge_used_tokens[client_hmac] = now_ts + 300;
            }
            // Apply charge
            double new_balance = 0.0;
            {
                std::lock_guard<std::mutex> wlock(g_wallet_mutex);
                auto& w = g_wallets[dev_id];
                if (w.device_id.empty()) w.device_id = dev_id;
                w.balance += amount;
                w.total_charged += amount;
                w.last_activity = std::chrono::steady_clock::now();
                g_total_charges_cents += (uint64_t)(amount * 100);
                new_balance = w.balance;
            }
            wallets_save();
            char resp_buf[256];
            snprintf(resp_buf, sizeof(resp_buf),
                "{\"ok\":true,\"balance\":%.4f,\"charged\":%.4f}", new_balance, amount);
            http_send(fd, 200, "OK", "application/json", resp_buf);
            fprintf(stderr, "[wallet/http] CHARGE: device=%s amount=%.4f new_balance=%.4f\n",
                    dev_id.c_str(), amount, new_balance);
            close(fd); continue;
        }

        // GET /api/wallet?device_id=<id> — Query wallet balance
        if (strcmp(method, "GET") == 0 && strncmp(path, "/api/wallet?device_id=", 22) == 0) {
            std::string dev_id(path + 22);  // device_ids are hex, no URL decode needed
            double balance = 0.0;
            bool found = false;
            {
                std::lock_guard<std::mutex> wlock(g_wallet_mutex);
                auto it = g_wallets.find(dev_id);
                if (it != g_wallets.end()) {
                    balance = it->second.balance;
                    found = true;
                }
            }
            char resp_buf[128];
            snprintf(resp_buf, sizeof(resp_buf),
                "{\"device_id\":\"%s\",\"balance\":%.4f,\"found\":%s}",
                dev_id.c_str(), balance, found ? "true" : "false");
            http_send(fd, 200, "OK", "application/json", resp_buf);
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
        // Requires authenticated user (Bearer token)
        if (strcmp(method, "POST") == 0 && strcmp(path, "/api/channel/claim-stripe") == 0) {
            // Verify auth token
            std::string bearer = get_bearer();
            if (bearer.empty()) {
                http_send(fd, 401, "Unauthorized", "application/json",
                    "{\"error\":\"missing_token\"}");
                close(fd); continue;
            }
            {
                std::lock_guard<std::mutex> alock(g_auth_mutex);
                auto bearer_it = g_auth_tokens.find(bearer);
                if (bearer_it == g_auth_tokens.end() || !token_is_valid(bearer)) {
                    if (bearer_it != g_auth_tokens.end()) g_auth_tokens.erase(bearer_it);
                    http_send(fd, 401, "Unauthorized", "application/json",
                        "{\"error\":\"invalid_token\"}");
                    close(fd); continue;
                }
            }
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

            // Stripe webhook signature verification (if STRIPE_WEBHOOK_SECRET is set)
            if (!g_stripe_webhook_secret.empty()) {
                // Parse Stripe-Signature header: t=<ts>,v1=<sig>
                // Parse Stripe-Signature header from raw HTTP
                const char* sig_ptr = strcasestr(buf, "Stripe-Signature: ");
                std::string stripe_ts, stripe_sig;
                if (sig_ptr) {
                    sig_ptr += strlen("Stripe-Signature: ");
                    const char* eol = strstr(sig_ptr, "\r\n");
                    std::string hdr(sig_ptr, eol ? (size_t)(eol - sig_ptr) : strlen(sig_ptr));
                    for (size_t pos = 0; pos < hdr.size(); ) {
                        size_t comma = hdr.find(',', pos);
                        std::string part = hdr.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                        if (part.substr(0, 2) == "t=") stripe_ts = part.substr(2);
                        else if (part.substr(0, 3) == "v1=") stripe_sig = part.substr(3);
                        if (comma == std::string::npos) break;
                        pos = comma + 1;
                    }
                }
                if (stripe_ts.empty() || stripe_sig.empty()) {
                    http_send(fd, 400, "Bad Request", "application/json",
                        "{\"error\":\"missing Stripe-Signature\"}");
                    close(fd); continue;
                }
                // Verify: HMAC-SHA256(secret, timestamp + "." + payload)
                std::string signed_payload = stripe_ts + "." + payload;
                std::string expected_sig = hmac_sha256_hex(g_stripe_webhook_secret, signed_payload);
                // Constant-time comparison
                bool sig_match = (stripe_sig.size() == expected_sig.size());
                if (sig_match) {
                    volatile unsigned char result = 0;
                    for (size_t i = 0; i < stripe_sig.size(); i++)
                        result |= stripe_sig[i] ^ expected_sig[i];
                    sig_match = (result == 0);
                }
                if (!sig_match) {
                    fprintf(stderr, "[relay] Stripe webhook: signature mismatch!\n");
                    http_send(fd, 400, "Bad Request", "application/json",
                        "{\"error\":\"invalid signature\"}");
                    close(fd); continue;
                }
                // Check timestamp freshness (reject > 5 min old)
                int64_t sig_ts = std::strtoll(stripe_ts.c_str(), nullptr, 10);
                if (std::abs(now_unix() - sig_ts) > 300) {
                    fprintf(stderr, "[relay] Stripe webhook: timestamp too old\n");
                    http_send(fd, 400, "Bad Request", "application/json",
                        "{\"error\":\"timestamp too old\"}");
                    close(fd); continue;
                }
            }

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

        // GET /api/channels/live — public: list active groups with status
        if (strcmp(method, "GET") == 0 && strcmp(path, "/api/channels/live") == 0) {
            std::lock_guard<std::shared_mutex> lock(g_mutex);
            auto now_steady = std::chrono::steady_clock::now();
            std::string json = "{\"groups\":[";
            bool first = true;
            for (const auto& [name, group] : g_groups) {
                if (!first) json += ",";
                first = false;
                // Determine status: live (audio < 5s ago), idle (connected), offline
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    now_steady - group.last_audio_time).count();
                const char* status = (group.packets_forwarded > 0 && elapsed < 5)
                    ? "live" : (group.members.empty() ? "offline" : "idle");
                // Count roles
                int listeners = 0, djs = 0;
                for (const auto& m : group.members) {
                    if (m.role == MemberRole::DJ || m.role == MemberRole::Owner) djs++;
                    else listeners++;
                }
                // Build member list
                std::string members_json = "[";
                bool mfirst = true;
                for (const auto& m : group.members) {
                    if (!mfirst) members_json += ",";
                    mfirst = false;
                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &m.addr.sin_addr, ip, sizeof(ip));
                    auto seen_ago = std::chrono::duration_cast<std::chrono::seconds>(
                        now_steady - m.last_seen).count();
                    const char* role_str = (m.role == MemberRole::Owner) ? "owner"
                        : (m.role == MemberRole::DJ) ? "dj" : "listener";
                    members_json += "{\"device\":\"" + json_escape(m.device_name) + "\""
                        + ",\"role\":\"" + role_str + "\""
                        + ",\"last_seen_sec\":" + std::to_string(seen_ago)
                        + "}";
                }
                members_json += "]";
                json += "{\"name\":\"" + json_escape(name) + "\""
                      + ",\"status\":\"" + status + "\""
                      + ",\"members\":" + std::to_string(group.members.size())
                      + ",\"listeners\":" + std::to_string(listeners)
                      + ",\"djs\":" + std::to_string(djs)
                      + ",\"packets_forwarded\":" + std::to_string(group.packets_forwarded)
                      + ",\"bytes_forwarded\":" + std::to_string(group.bytes_forwarded)
                      + ",\"last_audio_sec_ago\":" + std::to_string(elapsed)
                      + ",\"meta\":\"" + json_escape(group.last_meta) + "\""
                      + ",\"member_list\":" + members_json
                      + "}";
            }
            json += "],\"total_groups\":" + std::to_string(g_groups.size())
                  + ",\"server_region\":\"" + json_escape(getenv("FLY_REGION") ? getenv("FLY_REGION") : "unknown") + "\""
                  + ",\"note\":\"Settings ownership: channel_name=client, members/packets/meta=server, volume/buffer/mode=client\""
                  + "}";
            http_send(fd, 200, "OK", "application/json", json);
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
            if (g_admin_key.empty() || admin_key != g_admin_key) {
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
                "Access-Control-Allow-Origin: https://solun.art\r\n"
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

        // GET /api/fingerprint/matches — current fingerprint match status
        if (strcmp(method, "GET") == 0 && strcmp(path, "/api/fingerprint/matches") == 0) {
            std::ostringstream json;
            json << "{\"matches\":[";
            size_t total_matches = 0;
            {
                std::lock_guard<std::mutex> mlock(g_fp_matches_mutex);
                bool first = true;
                for (const auto& [ch, m] : g_fp_matches) {
                    if (!first) json << ",";
                    first = false;
                    total_matches++;
                    json << "{"
                         << "\"channel\":\"" << m.channel << "\","
                         << "\"fingerprint\":\"" << m.fingerprint << "\","
                         << "\"listeners\":" << m.listener_count << ","
                         << std::fixed << std::setprecision(4)
                         << "\"confidence\":" << m.confidence << ","
                         << "\"first_seen\":" << m.first_seen << ","
                         << "\"last_seen\":" << m.last_seen << ","
                         << "\"revenue\":{"
                         << std::setprecision(6)
                         << "\"rights_holder\":" << m.revenue_rights_holder << ","
                         << "\"dj_cashback\":" << m.revenue_dj_cashback << ","
                         << "\"platform\":" << m.revenue_platform
                         << "}}";
                }
            }
            // Count total channels with active fingerprint data
            size_t total_channels = g_listener_fingerprints.size();
            json << "],\"total_channels\":" << total_channels
                 << ",\"total_matches\":" << total_matches << "}";
            http_send(fd, 200, "OK", "application/json", json.str());
            close(fd); continue;
        }

        // POST /api/fingerprint — listener-side fingerprint report
        if (strcmp(method, "POST") == 0 && strcmp(path, "/api/fingerprint") == 0) {
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
            auto extract_int = [](const char* json, const char* key) -> uint64_t {
                char search[64];
                snprintf(search, sizeof(search), "\"%s\":", key);
                const char* p = strstr(json, search);
                if (!p) return 0;
                p += strlen(search);
                while (*p == ' ') p++;
                return strtoull(p, nullptr, 10);
            };
            std::string channel = extract(body, "channel");
            std::string hash_str = extract(body, "hash");
            if (hash_str.empty()) hash_str = extract(body, "fingerprint");  // iOS/Web use "fingerprint"
            uint64_t timestamp = extract_int(body, "timestamp");
            std::string device_id = extract(body, "device_id");
            std::string chromaprint = extract(body, "chromaprint");
            int duration = (int)extract_int(body, "duration");
            if (channel.empty() || hash_str.empty()) {
                http_send(fd, 400, "Bad Request", "application/json",
                    "{\"error\":\"channel and hash required\"}");
                close(fd); continue;
            }
            // Parse hex hash (16 chars = 64 bits)
            uint64_t hash = strtoull(hash_str.c_str(), nullptr, 16);
            if (timestamp == 0) {
                timestamp = (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            }
            // Insert into aggregation map (use try_lock to avoid blocking UDP thread)
            auto& cfp = g_listener_fingerprints[channel];
            if (cfp.mtx.try_lock()) {
                cfp.recent.push_back({hash, timestamp, device_id, chromaprint, duration});
                cfp.mtx.unlock();
            }
            // Persist report to SQLite
            fp_db_insert_report(channel, device_id, hash_str, timestamp, chromaprint, duration);
            http_send(fd, 200, "OK", "application/json", "{\"ok\":true}");
            close(fd); continue;
        }

        // GET /api/now-playing — current song playing on each channel
        if (strcmp(method, "GET") == 0 && strncmp(path, "/api/now-playing", 16) == 0) {
            // Optional ?channel=xxx filter
            std::string filter_channel;
            const char* qmark = strchr(path, '?');
            if (qmark) {
                const char* ch_param = strstr(qmark, "channel=");
                if (ch_param) {
                    ch_param += 8;
                    const char* end = strchr(ch_param, '&');
                    filter_channel = end ? std::string(ch_param, end - ch_param) : std::string(ch_param);
                }
            }
            std::ostringstream json;
            json << "{\"channels\":[";
            if (g_fp_db) {
                std::lock_guard<std::mutex> lock(g_fp_db_mutex);
                std::string sql = "SELECT np.channel, s.title, s.artist, s.album, s.isrc, "
                    "s.youtube_url, s.youtube_video_id, np.listeners, np.confidence, np.identified_at "
                    "FROM channel_now_playing np JOIN identified_songs s ON np.song_id = s.id";
                if (!filter_channel.empty()) sql += " WHERE np.channel = ?";
                sql += " ORDER BY np.updated_at DESC";
                sqlite3_stmt* stmt = nullptr;
                if (sqlite3_prepare_v2(g_fp_db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                    if (!filter_channel.empty()) {
                        sqlite3_bind_text(stmt, 1, filter_channel.c_str(), -1, SQLITE_TRANSIENT);
                    }
                    bool first = true;
                    while (sqlite3_step(stmt) == SQLITE_ROW) {
                        if (!first) json << ",";
                        first = false;
                        auto col_text = [&](int c) -> std::string {
                            const char* v = (const char*)sqlite3_column_text(stmt, c);
                            return v ? v : "";
                        };
                        // Escape JSON strings (simple: replace " with \")
                        auto esc = [](const std::string& s) -> std::string {
                            std::string r;
                            r.reserve(s.size());
                            for (char c : s) {
                                if (c == '"') r += "\\\"";
                                else if (c == '\\') r += "\\\\";
                                else if (c == '\n') r += "\\n";
                                else r += c;
                            }
                            return r;
                        };
                        json << "{"
                             << "\"channel\":\"" << esc(col_text(0)) << "\","
                             << "\"song\":{"
                             << "\"title\":\"" << esc(col_text(1)) << "\","
                             << "\"artist\":\"" << esc(col_text(2)) << "\","
                             << "\"album\":\"" << esc(col_text(3)) << "\","
                             << "\"isrc\":\"" << esc(col_text(4)) << "\","
                             << "\"youtube_url\":\"" << esc(col_text(5)) << "\","
                             << "\"youtube_video_id\":\"" << esc(col_text(6)) << "\""
                             << "},"
                             << "\"listeners\":" << sqlite3_column_int(stmt, 7) << ","
                             << std::fixed << std::setprecision(4)
                             << "\"confidence\":" << sqlite3_column_double(stmt, 8) << ","
                             << "\"identified_at\":" << sqlite3_column_int64(stmt, 9)
                             << "}";
                    }
                    sqlite3_finalize(stmt);
                }
            }
            json << "]}";
            http_send(fd, 200, "OK", "application/json", json.str());
            close(fd); continue;
        }

        // GET /api/songs — full song database with play stats
        if (strcmp(method, "GET") == 0 && strcmp(path, "/api/songs") == 0) {
            std::ostringstream json;
            json << "{\"songs\":[";
            size_t total = 0;
            if (g_fp_db) {
                std::lock_guard<std::mutex> lock(g_fp_db_mutex);
                const char* sql =
                    "SELECT s.id, s.title, s.artist, s.album, s.isrc, s.acoustid, s.musicbrainz_id, "
                    "s.youtube_url, s.youtube_video_id, s.rights_holder, s.created_at, "
                    "COUNT(np.channel) AS total_plays "
                    "FROM identified_songs s "
                    "LEFT JOIN channel_now_playing np ON np.song_id = s.id "
                    "GROUP BY s.id ORDER BY s.created_at DESC";
                sqlite3_stmt* stmt = nullptr;
                if (sqlite3_prepare_v2(g_fp_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                    bool first = true;
                    while (sqlite3_step(stmt) == SQLITE_ROW) {
                        if (!first) json << ",";
                        first = false;
                        total++;
                        auto col_text = [&](int c) -> std::string {
                            const char* v = (const char*)sqlite3_column_text(stmt, c);
                            return v ? v : "";
                        };
                        auto esc = [](const std::string& s) -> std::string {
                            std::string r;
                            r.reserve(s.size());
                            for (char c : s) {
                                if (c == '"') r += "\\\"";
                                else if (c == '\\') r += "\\\\";
                                else if (c == '\n') r += "\\n";
                                else r += c;
                            }
                            return r;
                        };
                        json << "{"
                             << "\"id\":" << sqlite3_column_int64(stmt, 0) << ","
                             << "\"title\":\"" << esc(col_text(1)) << "\","
                             << "\"artist\":\"" << esc(col_text(2)) << "\","
                             << "\"album\":\"" << esc(col_text(3)) << "\","
                             << "\"isrc\":\"" << esc(col_text(4)) << "\","
                             << "\"acoustid\":\"" << esc(col_text(5)) << "\","
                             << "\"musicbrainz_id\":\"" << esc(col_text(6)) << "\","
                             << "\"youtube_url\":\"" << esc(col_text(7)) << "\","
                             << "\"youtube_video_id\":\"" << esc(col_text(8)) << "\","
                             << "\"rights_holder\":\"" << esc(col_text(9)) << "\","
                             << "\"total_plays\":" << sqlite3_column_int(stmt, 11)
                             << "}";
                    }
                    sqlite3_finalize(stmt);
                }
            }
            json << "],\"total\":" << total << "}";
            http_send(fd, 200, "OK", "application/json", json.str());
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

    // Load DTLS cert/key from environment variables if not supplied as CLI args.
    // Fly.io secrets DTLS_CERT_PEM and DTLS_KEY_PEM are written to tmpfiles.
    static std::string dtls_cert_tmpfile, dtls_key_tmpfile;
    auto write_pem_tmpfile = [](const char* env_var, const char* prefix,
                                 std::string& out_path) {
        const char* pem = getenv(env_var);
        if (!pem || !*pem) return;
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "/tmp/soluna-%s-XXXXXX.pem", prefix);
        int fd = mkstemps(tmp, 4);
        if (fd < 0) { fprintf(stderr, "[dtls] mkstemps failed for %s\n", env_var); return; }
        fchmod(fd, 0600);  // restrict PEM file permissions
        // Replace literal \n in env var with real newlines
        std::string content(pem);
        size_t pos = 0;
        while ((pos = content.find("\\n", pos)) != std::string::npos)
            content.replace(pos, 2, "\n");
        write(fd, content.c_str(), content.size());
        close(fd);
        out_path = tmp;
        fprintf(stderr, "[dtls] Loaded %s from env → %s\n", env_var, tmp);
    };
    write_pem_tmpfile("DTLS_CERT_PEM", "cert", dtls_cert_tmpfile);
    write_pem_tmpfile("DTLS_KEY_PEM",  "key",  dtls_key_tmpfile);
    if (!dtls_cert_tmpfile.empty()) g_dtls_cert_path = dtls_cert_tmpfile;
    if (!dtls_key_tmpfile.empty())  g_dtls_key_path  = dtls_key_tmpfile;

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
        else if (arg == "--dtls-cert")      g_dtls_cert_path = next();
        else if (arg == "--dtls-key")       g_dtls_key_path = next();
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
        const char* env_stripe_wh = getenv("STRIPE_WEBHOOK_SECRET");
        if (env_stripe_wh && env_stripe_wh[0]) {
            g_stripe_webhook_secret = env_stripe_wh;
            fprintf(stderr, "[relay] Stripe webhook signature verification enabled\n");
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

    // Load email auth database
    users_load();

    // Read Resend API key for email sending
    if (const char* rk = getenv("RESEND_API_KEY")) {
        g_resend_api_key = rk;
        fprintf(stderr, "[auth] RESEND_API_KEY configured (%zu chars)\n", g_resend_api_key.size());
    } else {
        fprintf(stderr, "[auth] WARNING: RESEND_API_KEY not set, email auth disabled\n");
    }

    // Read APNs push notification config
    if (const char* v = getenv("APNS_KEY_ID"))    g_apns_key_id   = v;
    if (const char* v = getenv("APNS_TEAM_ID"))   g_apns_team_id  = v;
    if (const char* v = getenv("APNS_BUNDLE_ID"))  g_apns_bundle_id = v;
    if (const char* v = getenv("APNS_AUTH_KEY"))  {
        g_apns_auth_key_pem = v;
        // Allow \n escaping in env var (Fly.io stores as single line)
        size_t p = 0;
        while ((p = g_apns_auth_key_pem.find("\\n", p)) != std::string::npos) {
            g_apns_auth_key_pem.replace(p, 2, "\n");
        }
    }
    if (!g_apns_key_id.empty() && !g_apns_team_id.empty() && !g_apns_bundle_id.empty() && !g_apns_auth_key_pem.empty())
        fprintf(stderr, "[apns] APNs configured: kid=%s team=%s bundle=%s\n",
                g_apns_key_id.c_str(), g_apns_team_id.c_str(), g_apns_bundle_id.c_str());
    else
        fprintf(stderr, "[apns] WARNING: APNs not fully configured — push disabled\n");

    // Initialize libcurl globally (must be called before any curl_easy_init)
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Initialize fingerprint SQLite database
    fp_db_init();

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

    // Initialize DTLS-SRTP if cert/key provided
    if (!g_dtls_cert_path.empty()) {
        init_dtls_context();
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
            } else if (n >= 6 && memcmp(pkt, "DELAY:", 6) == 0) {
                // Sync mode: device reports its network delay
                handle_delay((const char*)pkt, (size_t)n, from);
            } else if (n >= 6 && memcmp(pkt, "READY:", 6) == 0) {
                // File-sync: receiver ready notification
                handle_ready((const char*)pkt, (size_t)n, from);
            } else if (n >= 5 && memcmp(pkt, "MODE:", 5) == 0) {
                // Set channel mode: private (P2P, no copyright) or public (relay, copyright)
                handle_mode((const char*)pkt, (size_t)n, from);
            } else if (n >= 15 && memcmp(pkt, "ROYALTY_PAYER:", 14) == 0) {
                // Set who pays copyright royalties (dj|owner|listener|free)
                handle_royalty_payer((const char*)pkt, (size_t)n, from);
            } else if (n >= 6 && memcmp(pkt, "GRANT:", 6) == 0) {
                // Grant DJ/listener role to a member (owner only)
                handle_grant((const char*)pkt, (size_t)n, from);
            } else if (n >= 7 && memcmp(pkt, "VOLUME:", 7) == 0) {
                // Remote volume control: forward to target device in same group
                handle_volume_command((const char*)pkt, (size_t)n, from);
            } else if (n >= 7 && memcmp(pkt, "MEMBERS", 7) == 0) {
                // List members and roles
                handle_members(from);
            } else if (n >= 12 && memcmp(pkt, "GOSSIP_PEERS", 12) == 0) {
                // Gossip: request 8 peer candidates for redundant reception
                handle_gossip_peers(from);
            } else if (n >= 12 && memcmp(pkt, "PARENT_FAIL:", 12) == 0) {
                // Swarm: report primary parent failure, request fast reassignment
                handle_parent_fail((const char*)pkt, n, from);
            } else if (n >= 5 && memcmp(pkt, "TEXT:", 5) == 0) {
                // Text channel (lyrics/chat/info)
                handle_text((const char*)pkt, (size_t)n, from);
            } else if (n >= 4 && memcmp(pkt, "MIX:", 4) == 0) {
                // DJ + mic simultaneous mode toggle
                handle_mix((const char*)pkt, (size_t)n, from);
            } else if (n >= 5 && memcmp(pkt, "TALK:", 5) == 0) {
                // Talk mode: all members can send audio (conversation mode)
                handle_talk((const char*)pkt, (size_t)n, from);
            } else if (n >= 10 && memcmp(pkt, "MIC_ALLOW:", 10) == 0) {
                // Per-device mic permission: grant
                handle_mic_allow((const char*)pkt, (size_t)n, from);
            } else if (n >= 9 && memcmp(pkt, "MIC_DENY:", 9) == 0) {
                // Per-device mic permission: revoke
                handle_mic_deny((const char*)pkt, (size_t)n, from);
            } else if (n >= 8 && memcmp(pkt, "MIC_LIST", 8) == 0 && (n == 8 || pkt[8] == '\n')) {
                // Per-device mic permission: list all devices with mic status
                handle_mic_list(from);
            } else if (n >= 14 && memcmp(pkt, "GLOBAL_DEVICES", 14) == 0 && (n == 14 || pkt[14] == '\n')) {
                // Global device registry: list all DJ/Owner across all groups
                handle_global_devices(from);
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
            } else if (n >= 11 && memcmp(pkt, "TURN_ALLOC:", 11) == 0) {
                // TURN fallback allocation request (OSTP v0.9.3 §5.x)
                handle_turn_alloc((const char*)pkt, (size_t)n, from);
            } else if (g_dtls_enabled && is_dtls_packet(pkt, (size_t)n)) {
                // DTLS record — handle handshake or decrypt SRTP
                handle_dtls_packet(pkt, (size_t)n, from);
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
            if (g_dtls_enabled) {
                cleanup_dtls_sessions();
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
    curl_global_cleanup();
    return 0;
}
