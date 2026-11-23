// Enemy census (enemy sync milestone P0, docs/enemy-sync.md): a read-only walk of the game's actor registry and of the NPC
// manager's GUID maps. It answers "do both worlds have the same enemies, with the same GUIDs, in the same places?" before
// any enemy is mirrored. Nothing is written to the game. The walk itself calls no game function: every read of the
// registry, the NPC maps and the actors goes through ReadProcessMemory (partner::mem), and every layout it relies on is
// byte-checked first (one log line and off on a mismatch). Only our own Sam's entity and place come from the mod's usual
// SEH-guarded getters (game::TryGetLocalPlayer, game::TryGetTransform), as everywhere else in the mod.
//
//   action enemy_census <tag> [radius_m] [flags]
//     tag       a number the lines carry (compare the same tag on both games)
//     radius_m  detail lines only for enemies within this distance of our Sam (0 = all, the default)
//     flags     bit 1: also one line per actor of every other type (gazers, catchers, ...); bit 2: the MULE manager's
//               killed records, raw
//
// The pure part below (actor ids, the game's GUID hash-map entries, the cross-world comparison) is unit-tested in
// tests/enemy_census_tests.cpp.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace st::enemy {

// ---------------------------------------------------------------- actor ids (0x1422FA980: type = id >> 13 & 0xFF, slot = id & 0x1FFF)
constexpr std::uint32_t kActorNone = 0x1FFFFF;  // [0x144AF1150]
constexpr std::uint32_t ActorType(std::uint32_t id) { return (id >> 13) & 0xFFu; }
constexpr std::uint32_t ActorSlot(std::uint32_t id) { return id & 0x1FFFu; }
// MULEs, Demens and the warriors (SetSequenceOnlyMule 0x142B2FA63: id & 0x1FE000 == 0x2000)
constexpr bool IsHumanEnemyId(std::uint32_t id) { return id != kActorNone && (id & 0x1FE000u) == 0x2000u; }

// ---------------------------------------------------------------- the game's GUID hash maps (0x141B037E0 / 0x141B038F0)
// Open addressing, capacity a power of two at map+0xC, data at map+0; a slot is used when its u32 hash (crc32c | 0x80000000)
// is non-zero. The NPC manager's main map (+0x78) has 0x18-byte slots {hash, GUID at +4, kind at +0x14}; the per-kind maps
// (sub +0x18, kind 6 at +0x48) have 0x20-byte slots {hash, GUID at +8, value at +0x18 = the NPC record}.
struct Guid16 {
    std::uint8_t b[16] = {};
    bool operator==(const Guid16& o) const { return std::memcmp(b, o.b, 16) == 0; }
    bool operator!=(const Guid16& o) const { return !(*this == o); }
    bool operator<(const Guid16& o) const { return std::memcmp(b, o.b, 16) < 0; }
    bool Zero() const {
        for (std::uint8_t c : b)
            if (c) return false;
        return true;
    }
};
constexpr std::size_t kKindSlot = 0x18, kValueSlot = 0x20;
constexpr std::uint32_t kMaxMapCapacity = 1u << 16;
constexpr std::uint32_t kKinds = 7;  // 0..5 in per-kind managers at npc+8+kind*8, 6 at npc+0x48

inline bool MapCapacityOk(std::uint32_t cap) { return cap > 0 && cap <= kMaxMapCapacity && (cap & (cap - 1)) == 0; }

inline std::uint32_t Le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 |
           static_cast<std::uint32_t>(p[3]) << 24;
}
inline std::uint64_t Le64(const std::uint8_t* p) { return static_cast<std::uint64_t>(Le32(p)) | static_cast<std::uint64_t>(Le32(p + 4)) << 32; }

// A used main-map slot: its GUID and kind. False for an empty slot (or a hash without the game's high bit).
inline bool DecodeKindSlot(const std::uint8_t* slot, Guid16* guid, std::uint32_t* kind) {
    const std::uint32_t h = Le32(slot);
    if (!h || !(h & 0x80000000u)) return false;
    std::memcpy(guid->b, slot + 4, 16);
    *kind = Le32(slot + 0x14);
    return true;
}
// A used per-kind slot: its GUID and value (the NPC record's address).
inline bool DecodeValueSlot(const std::uint8_t* slot, Guid16* guid, std::uint64_t* value) {
    const std::uint32_t h = Le32(slot);
    if (!h || !(h & 0x80000000u)) return false;
    std::memcpy(guid->b, slot + 8, 16);
    *value = Le64(slot + 0x18);
    return true;
}

// 32 hex digits, the bytes in memory order (the same text on both games for the same GUID).
inline void GuidText(const Guid16& g, char (&out)[33]) {
    static const char kHex[] = "0123456789ABCDEF";
    for (int i = 0; i < 16; ++i) {
        out[2 * i] = kHex[g.b[i] >> 4];
        out[2 * i + 1] = kHex[g.b[i] & 15];
    }
    out[32] = 0;
}

inline std::uint32_t Fnv1a(std::uint32_t h, const void* data, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}
constexpr std::uint32_t kFnvSeed = 2166136261u;

// ---------------------------------------------------------------- one enemy as a census sees it, and two censuses compared
struct Seen {
    Guid16 guid;            // zero: no NPC record names this actor
    std::uint32_t id = kActorNone;
    bool has_pos = false;
    double x = 0, y = 0, z = 0;
    bool realized = false;  // its entity is in the world
    bool dead = false;
};

// Fingerprint of a census: the sorted GUIDs (order-free, so both games give the same number for the same set).
inline std::uint32_t GuidSetPrint(std::vector<Guid16> g) {
    std::sort(g.begin(), g.end());
    std::uint32_t h = kFnvSeed;
    for (const Guid16& x : g) h = Fnv1a(h, x.b, 16);
    return h;
}

struct Comparison {
    int both = 0, only_a = 0, only_b = 0;  // by GUID (actors without one are not compared)
    int same_id = 0;                       // matched and the same actor id in both worlds
    int both_pos = 0, within = 0, beyond = 0;  // matched with a position on both sides; within / beyond the guard
    int both_realized = 0;
    int dead_differs = 0;
    double max_m = 0, p50_m = 0, p95_m = 0;  // over both_pos
    int dup_a = 0, dup_b = 0;                // a GUID seen twice in one census
};

inline double Percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const double r = p * static_cast<double>(v.size() - 1);
    const std::size_t i = static_cast<std::size_t>(r);
    const double f = r - static_cast<double>(i);
    return i + 1 < v.size() ? v[i] + (v[i + 1] - v[i]) * f : v[i];
}

// Match two censuses by GUID; a matched pair is "close" when both have a position within guard_m of each other (the P1
// identity rule: never mirror a GUID match whose places differ).
inline Comparison Compare(const std::vector<Seen>& a, const std::vector<Seen>& b, double guard_m) {
    Comparison c;
    std::vector<const Seen*> sa, sb;
    for (const Seen& s : a)
        if (!s.guid.Zero()) sa.push_back(&s);
    for (const Seen& s : b)
        if (!s.guid.Zero()) sb.push_back(&s);
    auto less = [](const Seen* x, const Seen* y) { return x->guid < y->guid; };
    std::sort(sa.begin(), sa.end(), less);
    std::sort(sb.begin(), sb.end(), less);
    for (std::size_t i = 1; i < sa.size(); ++i)
        if (sa[i]->guid == sa[i - 1]->guid) ++c.dup_a;
    for (std::size_t i = 1; i < sb.size(); ++i)
        if (sb[i]->guid == sb[i - 1]->guid) ++c.dup_b;
    std::vector<double> d;
    std::size_t i = 0, j = 0;
    while (i < sa.size() || j < sb.size()) {
        if (j >= sb.size() || (i < sa.size() && sa[i]->guid < sb[j]->guid)) {
            ++c.only_a, ++i;
            continue;
        }
        if (i >= sa.size() || sb[j]->guid < sa[i]->guid) {
            ++c.only_b, ++j;
            continue;
        }
        const Seen& x = *sa[i];
        const Seen& y = *sb[j];
        ++c.both;
        if (x.id == y.id) ++c.same_id;
        if (x.realized && y.realized) ++c.both_realized;
        if (x.dead != y.dead) ++c.dead_differs;
        if (x.has_pos && y.has_pos) {
            const double m = std::sqrt((x.x - y.x) * (x.x - y.x) + (x.y - y.y) * (x.y - y.y) + (x.z - y.z) * (x.z - y.z));
            ++c.both_pos;
            d.push_back(m);
            if (m <= guard_m) ++c.within;
            else ++c.beyond;
            if (m > c.max_m) c.max_m = m;
        }
        ++i, ++j;
    }
    c.p50_m = Percentile(d, 0.5);
    c.p95_m = Percentile(d, 0.95);
    return c;
}

// ---------------------------------------------------------------- the game side (enemy_census.cpp)
// Game thread (a debug action). False with a reason in msg when the census is off (a byte check failed) or no world.
bool Census(int tag, double radius_m, int flags, char* msg, std::size_t cap) noexcept;

}  // namespace st::enemy
