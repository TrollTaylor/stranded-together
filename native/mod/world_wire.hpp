// Shared structures (world sync, docs/world-sync.md): the pure part. The "SWS1" messages on the link's reliable ordered
// channel 1 ("world"), the structure Record (the allow-listed plain-data ranges of the game's 0x6C8-byte construction
// record), the census digest entries and bucket hashes the join check compares. No game memory, no link, no Windows:
// unit-tested in tests/world_wire_tests.cpp.
//
// Identity: a structure is the host's construction point. Its key is the host's CP id (`hid`, Player ids 10000+n); the
// guard is its type and position. The host's world decides everything (design-final D2); the guest's world is a mirror
// inside its sandbox (the host-held save).
//
// Every decoder is strict: exact sizes, the magic, version 1, known types, zero reserved flag bits, finite numbers in
// range. A message that fails is dropped whole (and counted).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace st::wwire {

// ---------------------------------------------------------------- constants
inline constexpr std::uint8_t kChannel = 1;               // the link's reliable ordered channel "world"
inline constexpr std::size_t kMaxMessage = 1024;          // link_protocol.hpp kMaxReliableMessage
inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kHeaderSize = 16;            // "SWS1" | u8 type | u8 version | u16 flags | u32 host_world | u32 seq
inline constexpr std::size_t kInfoSize = 0x6C8;           // the game's construction record (ctor 0x14277FDE0)
inline constexpr int kBuckets = 64;                       // join-check buckets: hid & 63
inline constexpr std::size_t kDigestMax = 24;             // entries per DIGEST
inline constexpr std::size_t kNeedMax = 64;               // hids per NEED
inline constexpr double kMaxCoord = 20000.0;              // |x|, |y| (m)
inline constexpr double kMaxZ = 5000.0;

// header flags
inline constexpr std::uint16_t kFlagAnim = 1u;    // play the build-up / printer (ADD), the dismantle animation (REM)
inline constexpr std::uint16_t kFlagDev = 2u;     // made by a dev action (sandboxed saves only)
inline constexpr std::uint16_t kFlagRetry = 4u;   // re-sent after a new link session
inline constexpr std::uint16_t kFlagAll = 7u;

enum class Msg : std::uint8_t {
    hello = 1,       // both: protocol, parts, ready
    snap_req = 2,    // G->H: the guest's world counter; the host answers SUM
    sum = 3,         // H->G: count + 64 bucket hashes
    bucket_req = 4,  // G->H: the buckets that differ
    digest = 5,      // H->G: the host's entries of those buckets (chunked)
    need = 6,        // G->H: mirror these (the host answers ADD, anim 0)
    add = 7,         // H->G: a structure of the host's world: mirror it
    gbuild = 8,      // G->H: the guest built this (her commit was held): create it in the host's world
    rej = 9,         // H->G: a GBUILD refused
    rem = 10,        // H->G: a structure is gone from the host's world
    remreq = 11,     // G->H: the guest dismantled / picked up a bound structure
    mats = 12,       // G->H: the guest contributed / repaired at a terminal (W2): apply it to the host's structure
    state = 13,      // H->G: a structure's level, materials, level-up and durability (absolute, W2)
    dur = 14,        // H->G: durabilities, batched (W2)
    check = 15,      // H->G: every 2 s, one hash over the 64 bucket hashes
    lost = 16,       // G->H: a bound structure vanished from the guest's world without a REM
};
const char* MsgName(std::uint8_t type) noexcept;

struct Header {
    std::uint8_t type = 0;
    std::uint16_t flags = 0;
    std::uint32_t host_world = 0;  // the host's world counter (coop::WorldReady) the message belongs to
    std::uint32_t seq = 0;         // per sender, from 1
};
std::size_t WriteHeader(const Header& h, std::uint8_t* out, std::size_t cap) noexcept;
bool ReadHeader(const std::uint8_t* in, std::size_t size, Header* h) noexcept;

// ---------------------------------------------------------------- the Record
// The construction record's plain-data ranges the game's save writer (WriteRecord 0x142CA7970) persists, plus the placement
// check's snapped transform (+0x680). Never: +0x48 id, +0x50 owner, +0x54 UUID, +0x64 slot, +0x68 server id string,
// +0xA8 / +0x2F0 contributor lists (strings), +0x644.. runtime flags, +0x650 ref, +0x670 stored cargo.
struct Range {
    std::uint16_t off, len;
};
inline constexpr Range kRanges[] = {
    {0x000, 0x004},  // type, pad, area
    {0x008, 0x03C},  // transform: pos 3 doubles, rot 3x3 floats
    {0x04C, 0x004},  // level
    {0x070, 0x038},  // state, +0x74, +0x78, durability +0x7C, +0x80..+0x8C, own materials +0x90..+0xA4
    {0x2D8, 0x018},  // an int[6] block
    {0x520, 0x0C4},  // others' stamp, others' sum, +0x540 block, repair, +0x570..+0x5A4 subclass fields, 2nd transform, +0x5C0..
    {0x5E8, 0x01C},  // +0x5E8..+0x603
    {0x608, 0x018},  // timestamps
    {0x620, 0x024},  // flag bytes +0x620..+0x643 (the receiver sets +0x62E, +0x644, +0x645 itself)
    {0x680, 0x040},  // the placement check's snapped transform
};
constexpr std::size_t RecordSizeOf() noexcept {
    std::size_t n = 0;
    for (const Range& r : kRanges) n += r.len;
    return n;
}
inline constexpr std::size_t kRecordSize = RecordSizeOf();
static_assert(kRecordSize == 496, "the Record is 496 bytes");
// The Record offset of an info offset inside the ranges, or -1.
int RecordOffset(std::size_t info_off) noexcept;

struct Record {
    std::uint8_t b[kRecordSize] = {};
    std::uint8_t Type() const noexcept { return b[0]; }
    void Pos(double (&out)[3]) const noexcept;
    void SetPos(const double (&p)[3]) noexcept;
    void Rot(float (&out)[9]) const noexcept;
    std::uint32_t Level() const noexcept;
    std::uint8_t State() const noexcept;
    float Durability() const noexcept;
    std::uint8_t Flag(std::size_t info_off) const noexcept;  // a flag byte +0x620..+0x643
    bool operator==(const Record& o) const noexcept { return std::memcmp(b, o.b, sizeof(b)) == 0; }
};
// info (kInfoSize bytes) -> Record (the ranges only)
void RecordFromInfo(const std::uint8_t* info, Record* out) noexcept;
// Record -> info: writes the ranges only (everything else of `info` stays as the game's ctor left it).
void RecordToInfo(const Record& r, std::uint8_t* info) noexcept;
// type 0..45, a finite transform in range with rotation rows, level 0..5, state 0..3, finite durability, SubclassValid.
bool RecordValid(const Record& r) noexcept;
// W3: a ladder's (types 24..26) or rope's (27..29) subclass fields are finite and in range; true for every other type.
bool SubclassValid(const Record& r) noexcept;

// ---------------------------------------------------------------- transforms
struct Xf {
    double pos[3] = {};
    float rot[9] = {};
};
bool XfValid(const Xf& x) noexcept;

// ---------------------------------------------------------------- census entries and the join check
inline constexpr std::uint8_t kEntOffline = 1u;   // cp+0xAB bit 7
inline constexpr std::uint8_t kEntUploaded = 2u;  // a server id (cp+0xB8 non-empty)
inline constexpr std::uint8_t kEntAllBits = 3u;
struct Entry {
    std::uint32_t hid = 0;
    std::uint8_t type = 0;
    std::uint8_t state = 0;
    std::uint8_t level = 0;
    std::uint8_t flags = 0;
    double pos[3] = {};
};
inline constexpr std::size_t kEntrySize = 4 + 4 + 24;
int BucketOf(std::uint32_t hid) noexcept;
// FNV-1a over (hid, type, position in cm; ladders / ropes: hid and type only). Not state or level (state follows each player's streaming distance; levels
// are W2's STATE).
std::uint64_t EntryHash(const Entry& e) noexcept;
// Each bucket: FNV-1a over its entries' hashes sorted by hid. `buckets` gets kBuckets values.
void BucketHashes(const std::vector<Entry>& entries, std::uint64_t (&buckets)[kBuckets]) noexcept;
std::uint64_t CheckHash(const std::uint64_t (&buckets)[kBuckets]) noexcept;

// ---------------------------------------------------------------- bodies
struct Hello {
    std::uint16_t proto = 1;
    std::uint8_t on = 0;        // world_sync on in this game
    std::uint8_t parts = 0;     // bit0 build, bit1 contrib, bit2 ladders, bit3 ziplines (W4)
    std::uint8_t anim_mode = 0; // 0 "645", 1 bits, 2 loaded, 3 off
    std::uint8_t ready = 0;     // this game's world is in the session and censused
    std::uint32_t world = 0;    // the sender's world counter
    std::uint32_t sender = 0;   // round 5: the sending game process's random nonce (a restarted partner -> a fresh dedup window)
};
struct SnapReq {
    std::uint32_t guest_world = 0;
    std::uint8_t reason = 0;    // 0 load, 1 heal, 2 reconnect
};
struct Sum {
    std::uint32_t count = 0;
    std::uint64_t buckets[kBuckets] = {};
};
struct BucketReq {
    std::uint64_t mask = 0;
};
struct Digest {
    std::uint16_t serial = 0;   // one answer to one BUCKET_REQ
    std::uint8_t part = 0, parts = 1;
    std::uint64_t mask = 0;     // the buckets this answer covers
    std::vector<Entry> entries; // <= kDigestMax
};
struct Need {
    std::vector<std::uint32_t> hids;  // 1..kNeedMax
};
struct ReqId {
    std::uint32_t nonce = 0;  // the guest game's session nonce
    std::uint16_t n = 0;      // 1, 2, ...
    bool operator==(const ReqId& o) const noexcept { return nonce == o.nonce && n == o.n; }
    bool Empty() const noexcept { return nonce == 0 && n == 0; }
};
enum class Origin : std::uint8_t { save = 0, host = 1, guest = 2 };
struct Add {
    std::uint32_t hid = 0;
    Origin origin = Origin::save;
    ReqId req;                  // origin guest: the request it answers
    bool has_pcc = false;
    Xf pcc;                     // the printer's place (the builder's PCC)
    Record rec;
};
struct GBuild {
    ReqId req;
    bool has_pcc = false;
    Xf pcc;
    double body[3] = {};        // where the guest's body was
    Record rec;
};
enum class RejReason : std::uint8_t { invalid = 1, type = 2, disabled = 3, too_far = 4, not_ready = 5, host_real = 6 };
const char* RejName(std::uint8_t r) noexcept;
struct Rej {
    ReqId req;
    RejReason reason = RejReason::invalid;
};
enum class RemReason : std::uint8_t { manual = 1, evicted = 2, decay = 3, area = 4, pickup = 5, guest = 6 };
const char* RemName(std::uint8_t r) noexcept;
struct Rem {  // REM and REMREQ
    std::uint32_t hid = 0;
    std::uint8_t type = 0;      // guard
    double pos[3] = {};         // guard
    RemReason reason = RemReason::area;
};
struct Check {
    std::uint32_t count = 0;
    std::uint64_t hash = 0;
};
struct Lost {
    std::uint32_t hid = 0;
};

// ---- W2: contributions, upgrades, repair, durability
inline constexpr std::int32_t kMaxMat = 100000;      // one contribution, per material
inline constexpr std::int32_t kMaxTotal = 10000000;  // a structure's totals, per material
inline constexpr std::size_t kDurMax = 100;          // entries per DUR
inline constexpr std::uint8_t kMaxLevelWire = 5;
enum class MatsKind : std::uint8_t { build = 0, repair = 1 };  // build = toward the next level (and the first: "being built")
struct Mats {
    ReqId req;                  // idempotent across link sessions
    std::uint32_t hid = 0;
    std::uint8_t type = 0;      // guard
    double pos[3] = {};         // guard
    MatsKind kind = MatsKind::build;
    std::int32_t m[6] = {};     // the amounts (0..kMaxMat each, at least one > 0)
};
enum class LevelUp : std::uint8_t { none = 0, queued = 1, running = 2 };
enum class StateWhy : std::uint8_t { change = 0, join = 1, applied = 2, retry = 3 };
struct State {
    std::uint32_t hid = 0;
    std::uint8_t type = 0;      // guard
    double pos[3] = {};         // guard
    std::uint8_t level = 0;
    std::uint8_t max_level = 0; // the category config's (+0x98)
    LevelUp levelup = LevelUp::none;
    StateWhy why = StateWhy::change;
    std::int32_t own[6] = {};       // cp+0x210 (the host's own contributions)
    std::int32_t invested[6] = {};  // Invested 0x142C58A50: own + contributor list + others' sum
    std::int32_t repair[6] = {};    // cp+0x260
    float dur = 0;                  // cp+0xD8 (timefall seconds)
    ReqId req;                      // the guest contribution this answers (why applied / retry), else empty
    std::int32_t excess[6] = {};    // of that contribution: not needed (the structure was at its last level)
};
struct DurEnt {
    std::uint32_t hid = 0;
    float dur = 0;
};
struct Dur {
    std::vector<DurEnt> e;      // 1..kDurMax
};
bool MatsValid(const std::int32_t (&m)[6], std::int32_t max, bool need_positive) noexcept;
bool DurValid(float d) noexcept;

// Each Encode writes header + body; returns the size (0 when it does not fit or does not validate).
std::size_t EncodeHello(const Header& h, const Hello& b, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t EncodeSnapReq(const Header& h, const SnapReq& b, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t EncodeSum(const Header& h, const Sum& b, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t EncodeBucketReq(const Header& h, const BucketReq& b, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t EncodeDigest(const Header& h, const Digest& b, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t EncodeNeed(const Header& h, const Need& b, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t EncodeAdd(const Header& h, const Add& b, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t EncodeGBuild(const Header& h, const GBuild& b, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t EncodeRej(const Header& h, const Rej& b, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t EncodeRem(const Header& h, const Rem& b, std::uint8_t* out, std::size_t cap) noexcept;  // h.type rem or remreq
std::size_t EncodeCheck(const Header& h, const Check& b, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t EncodeLost(const Header& h, const Lost& b, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t EncodeMats(const Header& h, const Mats& b, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t EncodeState(const Header& h, const State& b, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t EncodeDur(const Header& h, const Dur& b, std::uint8_t* out, std::size_t cap) noexcept;

// Each Decode checks the header's type and the exact body.
bool DecodeHello(const std::uint8_t* in, std::size_t n, Header* h, Hello* b) noexcept;
bool DecodeSnapReq(const std::uint8_t* in, std::size_t n, Header* h, SnapReq* b) noexcept;
bool DecodeSum(const std::uint8_t* in, std::size_t n, Header* h, Sum* b) noexcept;
bool DecodeBucketReq(const std::uint8_t* in, std::size_t n, Header* h, BucketReq* b) noexcept;
bool DecodeDigest(const std::uint8_t* in, std::size_t n, Header* h, Digest* b) noexcept;
bool DecodeNeed(const std::uint8_t* in, std::size_t n, Header* h, Need* b) noexcept;
bool DecodeAdd(const std::uint8_t* in, std::size_t n, Header* h, Add* b) noexcept;
bool DecodeGBuild(const std::uint8_t* in, std::size_t n, Header* h, GBuild* b) noexcept;
bool DecodeRej(const std::uint8_t* in, std::size_t n, Header* h, Rej* b) noexcept;
bool DecodeRem(const std::uint8_t* in, std::size_t n, Header* h, Rem* b) noexcept;
bool DecodeCheck(const std::uint8_t* in, std::size_t n, Header* h, Check* b) noexcept;
bool DecodeLost(const std::uint8_t* in, std::size_t n, Header* h, Lost* b) noexcept;
bool DecodeMats(const std::uint8_t* in, std::size_t n, Header* h, Mats* b) noexcept;
bool DecodeState(const std::uint8_t* in, std::size_t n, Header* h, State* b) noexcept;
bool DecodeDur(const std::uint8_t* in, std::size_t n, Header* h, Dur* b) noexcept;

// The digest of a bucket set, in chunks of kDigestMax (at least one, possibly empty, so the guest always hears back).
std::vector<Digest> ChunkDigest(const std::vector<Entry>& entries, std::uint64_t mask, std::uint16_t serial);

// Reassembly of one DIGEST answer (by serial): complete when every part arrived.
class DigestAssembler {
public:
    void Reset() noexcept;
    // false: a part of another serial than the one being assembled (it starts over with it), or a repeat.
    bool Add(const Digest& d) noexcept;
    bool Complete() const noexcept;
    std::uint16_t Serial() const noexcept { return serial_; }
    std::uint64_t Mask() const noexcept { return mask_; }
    const std::vector<Entry>& Entries() const noexcept { return entries_; }

private:
    std::uint16_t serial_ = 0;
    std::uint8_t parts_ = 0;
    std::uint64_t mask_ = 0;
    std::vector<bool> got_;
    std::vector<Entry> entries_;
    bool active_ = false;
};

}  // namespace st::wwire
