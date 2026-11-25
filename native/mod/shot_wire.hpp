// Shots (docs/enemy-sync.md P3, "ship A"): the players' own gunfire shown on the other screen through the game's own
// CreateBullet (muzzle flash, trail, impacts). The pure part: the SFX1 shot frame and the SEH1 combat beacon (their codecs
// and strict decoders), the record's flag byte, the replay set (the attack references of our replays, so their damage can
// be told apart), the shot-id dedupe window, the hand anchor, the play gate, the pull grouping and the rate cap.
// No game, no link: unit-tested in tests/shot_wire_tests.cpp (st_shot_wire_tests).
//
// SFX1 (unreliable, the shooter's game -> the other; every shot goes out in two consecutive frames):
//   header 20 B: 'SFX1' | u8 version 1 | u8 n (1..14) | u16 flags (bit0: shooter keys are enemy actor ids) | u32 seq |
//                u32 sender_ms | u32 world
//   per shot 68 B (little-endian):
//     0 u32 shot_id (the sender's running count)        4 u32 shooter (0xFFFFFFFE the sender's Sam; else an enemy actor id)
//     8 u16 attack_id     10 u8 weapon_id (0 = no muzzle)    11 u8 magazine_id (0 = none)
//    12 i32 origin[3] mm (absolute)   24 i16 dir[3] q15 (unit)   30 u16 speed dm/s (> 0)
//    32 u32 target_actor (0x1FFFFF none)   36 i16 target_off[3] cm
//    42 u8 rec04  43 u8 rec05  44 u8 rec06  45 u8 rec58  46 u8 rec5A  47 u8 recD0  48 u8 recD1
//    49 u8 flags: bit0 muzzle (first pellet of a pull), bit1 hand_rel valid, bit2 resent copy, bit3 lethal
//    50 u16 age_ms (capture -> send)   52 u32 rec84   56 f32 rec94
//    60 i16 hand_rel[3] mm (the origin from the shooter's right hand joint, in the shooter's yaw frame: x forward, y left, z up)
//    66 u16 yaw (the shooter's heading, 2^16 a turn, atan2 of its forward axis)
//
// SEH1 (unreliable, both ways; 1 Hz for the first 10 s, then every 5 s and on every change), 24 B:
//   'SEH1' | u8 version 1 | u8 parts | u16 enemy_count | u32 ids_fp | u32 guids_fp | u32 world | u8 freeze_mode | u8 role | u16 0
//   parts: bit0 mirror (enemy_sync), bit1 hits (enemy_hits), bit2 fire (shots_sync), bit3 own (P1b, v0.33.0), bit4 enemy fire,
//          bit5 camp (enemy_owner, round 21), bit6 wide attack ids (round 21)
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace st::shotw {

// ---------------------------------------------------------------- SFX1
inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kHeaderSize = 20;
inline constexpr std::size_t kShotSize = 68;
inline constexpr unsigned kMaxShots = 14;  // 20 + 14 * 68 = 972 B: within one FRAME message of a datagram
inline constexpr std::size_t kMaxFrameSize = kHeaderSize + kMaxShots * kShotSize;
inline constexpr std::uint32_t kShooterSam = 0xFFFFFFFEu;
inline constexpr std::uint32_t kNoActor = 0x1FFFFFu;
// The attack ids (EDSAttackId, the RTTI enum 0x144BD7D58: 264 entries up to 0x194). kMaxAttackId: the narrow range every build
// decodes (up to v0.33.0); kAttackIdWireMax: the wide one (round 21: 0x18E..0x194, the DC's fixed guns; 0x191 EnemyRubberFixedGun
// is weapon 92, the MULE fixed gun), sent only to a partner whose beacon carries kPartAttackWide. 0x18D is the enum's placeholder
// "_#2": never. The runtime authority is the game's own attack registry (AttackAllowed: the game's lookup reads list[-1] for an
// id it does not hold).
inline constexpr std::uint16_t kMaxAttackId = 0x18C;
inline constexpr std::uint16_t kAttackIdWireMax = 0x194;
inline constexpr std::uint16_t kAttackPlaceholder = 0x18D;
// CreateBullet (0x14294D340) sends these two attack ids to a projectile-entity path (0x14294C870, magazine 0x71 / 0x72), not to
// the Enqueue: never encoded, decoded or replayed (round 15 review).
inline constexpr std::uint16_t kAttackEntityA = 0x5F, kAttackEntityB = 0x60;
// The wire's bound: `wide` the partner (or this decoder) knows 0x18E..0x194.
inline constexpr bool AttackReplayable(std::uint16_t a, bool wide = false) noexcept {
    return a != 0 && a <= (wide ? kAttackIdWireMax : kMaxAttackId) && a != kAttackPlaceholder && a != kAttackEntityA && a != kAttackEntityB;
}
// The replay's rule: the narrow range as before (every build replays it; review rv-correctness R8: a registry read must never
// narrow it), and an id above 0x18C only within the wire bound and when the game's registry lists it (`listed`: a 0x200-bit set
// of the ids the registry holds, read once a world; nullptr: not read, then only the narrow range, the old behaviour).
inline constexpr bool AttackAllowed(std::uint16_t a, bool wide, const std::uint8_t* listed) noexcept {
    if (AttackReplayable(a, false)) return true;
    if (!listed) return false;
    return AttackReplayable(a, wide) && a < 0x200 && ((listed[a >> 3] >> (a & 7)) & 1u) != 0;
}

enum ShotFlag : std::uint8_t { kMuzzle = 1, kHandRel = 2, kResent = 4, kLethal = 8 };
enum FrameFlag : std::uint16_t { kEnemyShooters = 1 };

struct Shot {
    std::uint32_t shot_id = 0;
    std::uint32_t shooter = kShooterSam;
    std::uint16_t attack_id = 0;
    std::uint8_t weapon_id = 0, magazine_id = 0;
    double origin[3] = {};                  // m, world
    float dir[3] = {1, 0, 0};               // unit
    float speed = 0;                        // m/s
    std::uint32_t target_actor = kNoActor;
    float target_off[3] = {};               // m
    std::uint8_t rec04 = 0, rec05 = 0, rec06 = 0, rec58 = 0, rec5A = 0, recD0 = 0, recD1 = 0;
    std::uint8_t flags = 0;
    std::uint16_t age_ms = 0;
    std::uint32_t rec84 = 0;
    float rec94 = 0;
    float hand_rel[3] = {};                 // m, the shooter's yaw frame (flags & kHandRel)
    float yaw = 0;                          // rad
};

struct Frame {
    std::uint16_t flags = 0;
    std::uint32_t seq = 0, sender_ms = 0, world = 0;
    std::uint8_t n = 0;
    Shot shots[kMaxShots];
};

// 0 when it does not encode (n out of range, a shot that would not decode: speed 0, a zero or non-finite direction, an
// origin beyond +-2000 km, an attack id out of range).
std::size_t EncodeShots(const Frame& f, std::uint8_t* out, std::size_t cap) noexcept;
// Strict: the magic, the version, n in 1..kMaxShots, the exact size, every shot's direction (finite, |q15 vector| 0.9..1.1),
// speed > 0 and attack id in 1..0x194 but 0x5F / 0x60 / 0x18D (AttackReplayable wide; an old build decodes only up to 0x18C and
// rejects the whole frame, so the sender drops a wide shot for a partner without kPartAttackWide, never the frame).
bool DecodeShots(const std::uint8_t* d, std::size_t n, Frame& out) noexcept;
inline bool LooksLikeShotFrame(const std::uint8_t* d, std::size_t n) noexcept {
    return d && n >= kHeaderSize + kShotSize && n <= kMaxFrameSize && d[0] == 'S' && d[1] == 'F' && d[2] == 'X' && d[3] == '1';
}

// ---------------------------------------------------------------- SEH1
inline constexpr std::size_t kBeaconSize = 24;
// kPartOwn (8): the ownership protocol (P1b's OWN / OWNACK / OWNRESET, v0.33.0). kPartCamp (32): P1c, the host hands over per
// camp with that protocol (round 21; no new SCB1 type): ownership runs only when both beacons carry 32. This build never sends
// 8 (a v0.33 host would run P1b's per-enemy rules against it: review rv-correctness R1), so 8 is read only to name an old partner. kPartAttackWide
// (64): this build decodes the wide attack ids (up to 0x194), whatever its ownership.
enum Part : std::uint8_t { kPartMirror = 1, kPartHits = 2, kPartFire = 4, kPartOwn = 8, kPartEnemyFire = 16, kPartCamp = 32, kPartAttackWide = 64 };
struct Beacon {
    std::uint8_t parts = 0;
    std::uint16_t enemy_count = 0;
    std::uint32_t ids_fp = 0, guids_fp = 0, world = 0;
    std::uint8_t freeze_mode = 0;  // 0 none, 1 occupied, 2 occupied+required, 3 sleep, 4 puppet
    std::uint8_t role = 0;         // 1 host, 2 guest, 0 solo
};
std::size_t EncodeBeacon(const Beacon& b, std::uint8_t* out, std::size_t cap) noexcept;
bool DecodeBeacon(const std::uint8_t* d, std::size_t n, Beacon& out) noexcept;
inline bool LooksLikeBeacon(const std::uint8_t* d, std::size_t n) noexcept {
    return d && n == kBeaconSize && d[0] == 'S' && d[1] == 'E' && d[2] == 'H' && d[3] == '1';
}
// The other enemy frames the link queues raw for the enemy rounds (SEN1, SEP1): the magic only.
inline bool HasMagic(const std::uint8_t* d, std::size_t n, const char (&m)[5]) noexcept {
    return d && n >= 4 && d[0] == static_cast<std::uint8_t>(m[0]) && d[1] == static_cast<std::uint8_t>(m[1]) && d[2] == static_cast<std::uint8_t>(m[2]) &&
           d[3] == static_cast<std::uint8_t>(m[3]);
}
// "fire on" between two games: both beacons carry the bit.
inline bool BothHave(std::uint8_t ours, std::uint8_t theirs, std::uint8_t part) noexcept { return (ours & part) && (theirs & part); }

// ---------------------------------------------------------------- the bullet record's flag byte +0xD0 (CreateBullet 0x14294D340)
// CreateBullet(..., p8, p9, p10, p11, p12, ...) sets bit0 = p8, bit2 = p9, bit5 = p12, bit6 = p11, bit7 = p10 and keeps bits 1, 3
// and 4 (mask 0x1A) as they were: those cannot be set through the call (the replay writes the whole byte into its record).
struct D0Args {
    std::uint8_t p8 = 0, p9 = 0, p10 = 0, p11 = 0, p12 = 0;
};
inline D0Args UnpackD0(std::uint8_t d0) noexcept {
    D0Args a;
    a.p8 = d0 & 1u;
    a.p9 = (d0 >> 2) & 1u;
    a.p12 = (d0 >> 5) & 1u;
    a.p11 = (d0 >> 6) & 1u;
    a.p10 = (d0 >> 7) & 1u;
    return a;
}
// The byte CreateBullet builds from the arguments over a record byte `old` (the decompile's expression, verbatim).
inline std::uint8_t PackD0(const D0Args& a, std::uint8_t old = 0) noexcept {
    return static_cast<std::uint8_t>((((((a.p10 & 1) * 2 | (a.p11 & 1)) * 2 | (a.p12 & 1)) << 3 | (a.p9 & 1)) << 2) | (old & 0x1A) | (a.p8 & 1));
}
inline constexpr std::uint8_t kD0NotSettable = 0x1A;

// ---------------------------------------------------------------- the replay set
// The attack references our replays' bullets carry (read from the bullet system's slot after the enqueue: the context
// handle, and the words beside it), each kept while its bullet can still fly: ReplayTtlMs (150 m / speed + 1 s, at least
// 4 s, at most 10 s). The damage filter drops a hit whose DamageInfo reference (or context) is one of them, give or take the
// 0x20 an entity reference and its object differ by. A replay that would push out a live entry is refused (HasRoom): a
// bullet whose references are gone could hurt the watcher. 2048 entries hold 512 replays a second at 4 s, above the
// sender's cap (320 a second).
inline constexpr std::uint8_t kReplayKindPartner = 1, kReplayKindEnemy = 2, kReplayKindTest = 3, kReplayKindAimed = 4;
inline constexpr unsigned kReplaySetSize = 2048;
inline constexpr double kReplayTtlMs = 4000;     // the least
inline constexpr double kReplayTtlMaxMs = 10000;  // the most
inline constexpr double kReplayRangeM = 150;     // the replay's target: 150 m along the shot
inline constexpr float kMinReplaySpeed = 20.0f;  // m/s: a slower shot is refused (150 m would take over 7.5 s)
inline constexpr unsigned kReplayRefs = 4;
inline double ReplayTtlMs(float speed) noexcept {
    if (!(speed > 0.0f)) return kReplayTtlMaxMs;
    const double t = kReplayRangeM / static_cast<double>(speed) * 1000.0 + 1000.0;
    return t < kReplayTtlMs ? kReplayTtlMs : t > kReplayTtlMaxMs ? kReplayTtlMaxMs : t;
}
struct ReplaySet {
    struct Entry {
        std::uint64_t ref[kReplayRefs] = {};
        double t = -1e18;
        double ttl = kReplayTtlMs;
        std::uint8_t kind = 0;  // 1 the partner's shot, 2 an enemy's (kReplayKindEnemy), 3 a test, 4 the aimed test
        bool Live(double now) const noexcept { return !(now - t > ttl || now < t - 1000); }
    };
    Entry e[kReplaySetSize];
    unsigned head = 0;
    // False: the next Add would push out an entry that is still live.
    bool HasRoom(double now) const noexcept { return !e[head].Live(now); }
    // Every entry gone (a load: the old world's references can come back on the new world's bullets).
    void Clear() noexcept {
        for (Entry& x : e) x = Entry{};
        head = 0;
    }
    void Add(const std::uint64_t* refs, unsigned n, double t, std::uint8_t kind, double ttl_ms = kReplayTtlMs) noexcept {
        Entry& x = e[head];
        head = (head + 1) % kReplaySetSize;
        x = Entry{};
        for (unsigned i = 0; i < n && i < kReplayRefs; ++i) x.ref[i] = refs[i];
        x.t = t;
        x.ttl = ttl_ms;
        x.kind = kind;
    }
    // `how` (optional): 0 exact, 1 the value is the entry + 0x20, 2 the entry is the value + 0x20; `slot` which word matched.
    bool Find(std::uint64_t v, double now, std::uint8_t* kind = nullptr, int* how = nullptr, int* slot = nullptr) const noexcept {
        if (!v) return false;
        for (const Entry& x : e) {
            if (!x.Live(now)) continue;
            for (unsigned i = 0; i < kReplayRefs; ++i) {
                const std::uint64_t r = x.ref[i];
                if (!r) continue;
                const int h = v == r ? 0 : v == r + 0x20 ? 1 : r == v + 0x20 ? 2 : -1;
                if (h < 0) continue;
                if (kind) *kind = x.kind;
                if (how) *how = h;
                if (slot) *slot = static_cast<int>(i);
                return true;
            }
        }
        return false;
    }
    double Newest() const noexcept {
        double t = -1e18;
        for (const Entry& x : e)
            if (x.t > t) t = x.t;
        return t;
    }
};

// ---------------------------------------------------------------- the bullet category (the Enqueue 0x142916860)
// With the record's +0x5A (category) non-zero, the Enqueue looks the category up in the registry (+0x30 count, +0x38 list,
// key u8 +0x20) while it holds the bullet system's lock, and reads list[-1] when it is missing; with +0xD1 bit 1 it also
// builds a context through 0x14298AEC0, which looks the category up the same way. A fault there leaves the lock held: the
// game hangs. So the receiver masks +0xD1 to the bits a replay needs (bit 0: the category's second attack, +0x3A; Sam's
// records carry 0x00, runs 20261123-204612 / -204732) and refuses a category the registry does not list.
inline constexpr std::uint8_t kD1Mask = 0x01;
inline constexpr std::uint8_t MaskD1(std::uint8_t d1) noexcept { return static_cast<std::uint8_t>(d1 & kD1Mask); }
// `listed`: the registry lists category rec5A (looked up by the caller; ignored for category 0).
inline constexpr bool CategoryOk(std::uint8_t rec5A, std::uint8_t recD1, bool listed) noexcept {
    return listed || (rec5A == 0 && !(recD1 & 2));
}
// The Enqueue's own step for a record with attack 0 and a category: the category entry's attack u16 at +0x38, or at +0x3A when
// the record's +0xD1 bit 0 is set (run 20261123-204612: Sam's rifle records carry attack 0, category 71, D1 0x00 -> 0x47).
inline constexpr std::uint16_t CategoryAttackOf(std::uint16_t at38, std::uint16_t at3A, std::uint8_t recD1) noexcept {
    return (recD1 & 1) ? at3A : at38;
}
// The attack a captured record stands for: its own +0x5C when set, else its category's (found: the registry lists it).
inline constexpr std::uint16_t RecordAttack(std::uint16_t rec5C, std::uint8_t rec5A, bool found, std::uint16_t at38, std::uint16_t at3A,
                                            std::uint8_t recD1) noexcept {
    return rec5C ? rec5C : rec5A && found ? CategoryAttackOf(at38, at3A, recD1) : 0;
}

// ---------------------------------------------------------------- the shot-id dedupe (a 256-id window behind the newest)
struct Dedupe {
    std::uint32_t newest = 0;
    std::uint64_t seen[4] = {};  // bit k: newest - k was taken
    bool have = false;
    void Reset() noexcept { *this = Dedupe{}; }
    bool Bit(unsigned k) const noexcept { return (seen[k >> 6] >> (k & 63)) & 1u; }
    void Set(unsigned k) noexcept { seen[k >> 6] |= 1ull << (k & 63); }
    void Shift(unsigned by) noexcept {
        if (by >= 256) {
            seen[0] = seen[1] = seen[2] = seen[3] = 0;
            return;
        }
        while (by >= 64) {
            seen[3] = seen[2], seen[2] = seen[1], seen[1] = seen[0], seen[0] = 0;
            by -= 64;
        }
        if (!by) return;
        for (int i = 3; i > 0; --i) seen[i] = (seen[i] << by) | (seen[i - 1] >> (64 - by));
        seen[0] <<= by;
    }
    // True: a new shot id (take it). False: a duplicate, or older than the window.
    bool Accept(std::uint32_t id) noexcept {
        if (!have) {
            have = true;
            newest = id;
            seen[0] = 1, seen[1] = seen[2] = seen[3] = 0;
            return true;
        }
        const std::int32_t diff = static_cast<std::int32_t>(id - newest);
        if (diff > 0) {
            Shift(static_cast<unsigned>(diff));
            newest = id;
            Set(0);
            return true;
        }
        const unsigned back = static_cast<unsigned>(-diff);
        if (back >= 256 || Bit(back)) return false;
        Set(back);
        return true;
    }
};

// ---------------------------------------------------------------- the hand anchor
// World <-> the shooter's yaw frame (x forward, y left, z up; z is the world's up axis). yaw = atan2(forward.y, forward.x).
inline float YawOf(const float (&forward)[3]) noexcept { return static_cast<float>(std::atan2(forward[1], forward[0])); }
inline void ToYawFrame(const double (&world_delta)[3], float yaw, float (&out)[3]) noexcept {
    const double c = std::cos(yaw), s = std::sin(yaw);
    out[0] = static_cast<float>(world_delta[0] * c + world_delta[1] * s);
    out[1] = static_cast<float>(-world_delta[0] * s + world_delta[1] * c);
    out[2] = static_cast<float>(world_delta[2]);
}
inline void FromYawFrame(const float (&rel)[3], float yaw, const double (&base)[3], double (&out)[3]) noexcept {
    const double c = std::cos(yaw), s = std::sin(yaw);
    out[0] = base[0] + rel[0] * c - rel[1] * s;
    out[1] = base[1] + rel[0] * s + rel[1] * c;
    out[2] = base[2] + rel[2];
}
// The replay's origin: from the drawn hand when the shot carries one and the result stays within `max_m` of the absolute
// origin (the drawn body lags and is another character: its gun is where the flash belongs, but never far from the shot).
inline bool AnchoredOrigin(const Shot& s, bool have_hand, const double (&hand)[3], float body_yaw, double max_m, double (&out)[3]) noexcept {
    out[0] = s.origin[0], out[1] = s.origin[1], out[2] = s.origin[2];
    if (!(s.flags & kHandRel) || !have_hand) return false;
    double a[3];
    FromYawFrame(s.hand_rel, body_yaw, hand, a);
    const double dx = a[0] - s.origin[0], dy = a[1] - s.origin[1], dz = a[2] - s.origin[2];
    if (!(dx * dx + dy * dy + dz * dz <= max_m * max_m)) return false;
    out[0] = a[0], out[1] = a[1], out[2] = a[2];
    return true;
}

// ---------------------------------------------------------------- the play gate
// Hold a shot until the partner body plays the moment it was fired (the pose play time, in the sender's unwrapped ms), so
// the flash meets the drawn arm. No body: at once. Due more than 500 ms ago: at once, counted late. More than 2 s ahead (a
// clock jump): at once.
enum class Gate : std::uint8_t { hold, fire, late, now_no_body, now_ahead };
inline constexpr double kLateMs = 500, kAheadMs = 2000;
inline Gate PlayGate(bool have_body, double play_ms, double due_ms) noexcept {
    if (!have_body) return Gate::now_no_body;
    if (due_ms - play_ms > kAheadMs) return Gate::now_ahead;
    if (play_ms < due_ms) return Gate::hold;
    return play_ms - due_ms > kLateMs ? Gate::late : Gate::fire;
}
// A u32 sender time unwrapped next to a known unwrapped one (wrap-safe: within +-24 days).
inline double UnwrapNear(std::uint32_t t, double near_ms) noexcept {
    const std::uint32_t n = static_cast<std::uint32_t>(static_cast<std::uint64_t>(near_ms < 0 ? 0 : near_ms) & 0xFFFFFFFFull);
    return near_ms + static_cast<double>(static_cast<std::int32_t>(t - n));
}

// ---------------------------------------------------------------- pulls (the pellets of one trigger pull share the muzzle)
inline constexpr double kPullMs = 16;
struct PullGroup {
    double last_ms = -1e18;
    std::uint8_t last_weapon = 0;
    // True: the first record of a new pull (it carries the muzzle flash and the magazine's effect).
    bool Next(double now_ms, std::uint8_t weapon) noexcept {
        const bool first = !(now_ms - last_ms <= kPullMs && now_ms >= last_ms && weapon == last_weapon);
        last_ms = now_ms;
        last_weapon = weapon;
        return first;
    }
};

// Per shooter (enemy shots, round 16: the pellets of one MULE's pull share the muzzle, another MULE's shot starts its own pull):
// a small LRU of PullGroups keyed by the shooter id.
struct PullGroups {
    static constexpr unsigned kN = 16;
    struct E {
        std::uint32_t shooter = 0;
        PullGroup g;
        double used = -1e18;
        bool on = false;
    };
    E e[kN];
    bool Next(std::uint32_t shooter, double now_ms, std::uint8_t weapon) noexcept {
        unsigned at = kN;
        for (unsigned i = 0; i < kN && at == kN; ++i)
            if (e[i].on && e[i].shooter == shooter) at = i;
        if (at == kN) {
            at = 0;  // a free entry, else the least recently used
            for (unsigned i = 0; i < kN; ++i) {
                if (!e[i].on) {
                    at = i;
                    break;
                }
                if (e[i].used < e[at].used) at = i;
            }
            e[at] = E{};
            e[at].on = true;
            e[at].shooter = shooter;
        }
        e[at].used = now_ms;
        return e[at].g.Next(now_ms, weapon);
    }
};

// ---------------------------------------------------------------- the rate cap (N shots per 100 ms window)
inline constexpr unsigned kCapShots = 32;       // the players' shots
inline constexpr unsigned kEnemyCapShots = 24;  // the enemy shots (round 16; their own cap, beside the players')
inline constexpr double kCapWindowMs = 100;
template <unsigned N>
struct RateCapN {
    double t[N] = {};
    unsigned head = 0, count = 0;
    bool Take(double now) noexcept {
        while (count && now - t[head] >= kCapWindowMs) head = (head + 1) % N, --count;
        if (count >= N) return false;
        t[(head + count) % N] = now;
        ++count;
        return true;
    }
};
using RateCap = RateCapN<kCapShots>;
using EnemyRateCap = RateCapN<kEnemyCapShots>;

// The shooter a frame may carry: an enemy frame (flags kEnemyShooters) only enemy actor ids (type 1: id & 0x1FE000 == 0x2000), any
// other frame only the sender's Sam. A frame that mixes them is refused (round 16).
inline constexpr bool ShooterOk(std::uint16_t frame_flags, std::uint32_t shooter) noexcept {
    return (frame_flags & kEnemyShooters) ? (shooter != kNoActor && (shooter & 0x1FE000u) == 0x2000u) : shooter == kShooterSam;
}

// ---------------------------------------------------------------- the ids a replay passes (the registry, snapshot)
// The weapon / magazine registry ([0x147BEB0F8]) as the replay found it: CreateBullet dereferences a missing weapon's +0x80
// and reads a missing magazine's list[-1], so an id is passed only when it is listed (a weapon also needs its +0x80 child
// list, 0..64 entries). A refused id becomes 0: no muzzle / no magazine effect, the bullet still flies.
struct IdCheck {
    bool listed = false;
    bool child_ok = true;  // weapons: +0x80 non-null, its +0x90 count in 0..64
};
inline std::uint8_t ValidId(std::uint8_t id, const IdCheck& c) noexcept { return id && c.listed && c.child_ok ? id : 0; }

// ---------------------------------------------------------------- the drop at the bullet (round 16)
// Every bullet hands its damage to what it hit through one of two functions (BulletDamageA 0x142912120 / B 0x142912380:
// `receiver->vt[+0x10](receiver, DamageInfo)`), whatever the receiver is: vehicles, cargo, structures, BTs, NPCs, props. A
// replay is visual only (the shooter's own game decides the real damage), so a replay's hit is dropped there, before any
// receiver sees it, except on our own Sam: his hits go on to the shared Sam damage detour, whose "shots" filter keeps its
// policy (shots_friendly_fire). `replay`: the bullet's attack reference is in the replay set; `owner_dummy`: the bullet's
// owner is the bullet system's dummy (every CreateBullet bullet's: a reused reference on one of the game's own bullets is
// never dropped); `our_sam`: the receiver is our Sam's (entity + 0x38); `keep_test`: a hidden-run control (the replay's
// damage let through on purpose).
// A damage message (on our Sam, on an NPC) is one of our replays only when its reference is in the replay set AND its attacker
// is the bullet system's dummy, known (non-zero), give or take the 0x20 an entity and its reference differ by: every replay's
// attacker is (runs 20261124-093441 / -104304); a game round whose reference reuses a replay's is never one (review 16).
inline constexpr bool ReplayHit(bool ref_match, std::uint64_t attacker, std::uint64_t dummy) noexcept {
    return ref_match && dummy != 0 && attacker != 0 && (attacker == dummy || attacker == dummy + 0x20 || attacker + 0x20 == dummy);
}
enum class BulletVerdict : std::uint8_t { pass, drop };
inline constexpr BulletVerdict BulletRule(bool replay, bool owner_dummy, bool our_sam, bool keep_test) noexcept {
    return replay && owner_dummy && !our_sam && !keep_test ? BulletVerdict::drop : BulletVerdict::pass;
}
// Enemy sync P2 (docs/enemy-sync.md 9): a bullet's hit on a frozen mirror (or a part of one). Every such hit is dropped here,
// except her own round (`ours`, not a replay) while the hits are captured (`hit_capture`: enemy_hits on, Entity OnHit hooked,
// both beacons' hits part, mirroring on) and the receiver is an entity receiver whose vt+0x10 is Entity OnHit (`entity_onhit`):
// that one passes to the game (the DamageInfo is built, her impacts and MsgBulletHit stay) and the OnHit detour drops it and
// sends the HIT. Her round on a part without an entity receiver is dropped with no HIT (`drop_no_entity`, counted).
enum class MirrorBullet : std::uint8_t { drop, pass_capture, drop_no_entity };
inline constexpr MirrorBullet MirrorBulletRule(bool replay, bool ours, bool hit_capture, bool entity_onhit) noexcept {
    return replay || !ours || !hit_capture ? MirrorBullet::drop : entity_onhit ? MirrorBullet::pass_capture : MirrorBullet::drop_no_entity;
}

// Enemy sync P1b (docs/enemy-sync.md 10): the enemy shots' mask (shots::SetEnemyShots). bit0: capture the rounds of the enemies
// this game owns and sends (P1a's host = 1), bit1: replay the other game's enemy rounds from our frozen mirrors (P1a's guest = 2);
// P1b sets 3 on both.
inline constexpr int kEnemyCapture = 1, kEnemyReplay = 2;
inline constexpr bool EnemyCaptureOn(int mask) noexcept { return (mask & kEnemyCapture) != 0; }
inline constexpr bool EnemyReplayOn(int mask) noexcept { return (mask & kEnemyReplay) != 0; }

// What a dropped hit would have hit, by the receiving entity's class name (the game's own type name), for the counters.
enum class HitClass : std::uint8_t { vehicle, cargo, structure, bt, npc, player, other };
inline constexpr unsigned kHitClasses = 7;
inline const char* HitClassName(HitClass c) noexcept {
    static const char* const kNames[kHitClasses] = {"vehicle", "cargo", "structure", "bt", "npc", "player", "other"};
    const unsigned i = static_cast<unsigned>(c);
    return i < kHitClasses ? kNames[i] : "other";
}
namespace detail {
inline bool Has(const char* s, const char* sub) noexcept {
    if (!s || !sub || !*sub) return false;
    for (; *s; ++s) {
        const char* a = s;
        const char* b = sub;
        while (*a && *b && *a == *b) ++a, ++b;
        if (!*b) return true;
    }
    return false;
}
}  // namespace detail
inline HitClass ClassifyHit(const char* name) noexcept {
    using detail::Has;
    if (!name || !*name) return HitClass::other;
    // the order matters: "DSVehicleHitchCarrierMover" is a vehicle, "DSBaggage2Entity" cargo, a BT before an NPC
    if (Has(name, "Baggage") || Has(name, "Cargo")) return HitClass::cargo;
    if (Has(name, "Vehicle") || Has(name, "Motorbike") || Has(name, "Speedcar") || Has(name, "Truck") || Has(name, "Trike") || Has(name, "HoveringCart"))
        return HitClass::vehicle;
    if (Has(name, "Gazer") || Has(name, "Catcher") || Has(name, "Hunter") || Has(name, "Whale") || Has(name, "SupportBt") || Has(name, "BT"))
        return HitClass::bt;
    if (Has(name, "Player")) return HitClass::player;
    if (Has(name, "Humanoid") || Has(name, "Mule") || Has(name, "Npc") || Has(name, "Warrior") || Has(name, "Higgs") || Has(name, "Demens") ||
        Has(name, "DeliveryBot") || Has(name, "Animal") || Has(name, "ControlledEntity"))  // a MULE's entity: DSControlledEntity
        return HitClass::npc;
    if (Has(name, "Placable") || Has(name, "Construction") || Has(name, "Ladder") || Has(name, "Rope") || Has(name, "Signboard") || Has(name, "Destructib") ||
        Has(name, "ArmorPlate") || Has(name, "FixedGun") || Has(name, "Zipline") || Has(name, "Bridge") || Has(name, "Shelter") || Has(name, "Postbox") ||
        Has(name, "Generator") || Has(name, "Tower"))
        return HitClass::structure;
    return HitClass::other;
}

// The aimed test (hidden runs, `shots 5`): a value read before and after the replays (a vehicle's life, a structure's
// durability, a cargo's): unchanged within eps (a structure decays in timefall between the reads: eps absorbs it).
inline bool ValueUnchanged(float before, float after, float eps) noexcept {
    return std::isfinite(before) && std::isfinite(after) && std::fabs(after - before) <= eps;
}
// The aimed replay's origin: `m` from the target point toward `from` (our Sam), at the target's height + up (straight at it).
inline void AimedOrigin(const double (&target)[3], const double (&from)[3], double m, double up, double (&out)[3]) noexcept {
    double dx = from[0] - target[0], dy = from[1] - target[1];
    const double l = std::sqrt(dx * dx + dy * dy);
    if (l < 1e-3) dx = 1, dy = 0;
    else dx /= l, dy /= l;
    out[0] = target[0] + dx * m, out[1] = target[1] + dy * m, out[2] = target[2] + up;
}

// ---------------------------------------------------------------- quantisation helpers (shared by the codec and the tests)
inline std::int16_t Q15(float v) noexcept {
    const float c = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
    return static_cast<std::int16_t>(std::lround(c * 32767.0f));
}
inline float FromQ15(std::int16_t q) noexcept { return static_cast<float>(q) / 32767.0f; }

}  // namespace st::shotw
