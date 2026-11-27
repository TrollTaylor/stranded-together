// Round 28 P1 (docs/enemy-sync.md 17): the pure parts of the enemy attack probe (enemy_phit.cpp). No game memory, no Windows:
// the body capsule tests, the caller-site labels, the wrapper frames, the attack names, the per-attacker tallies and the ring
// codec's packing. Unit-tested in tests/enemy_phit_rules_tests.cpp.
//
// The game's attack queries (ds.exe, static RE round 28 Q1):
//   the sphere 0x1429934E0 (ctx, const double centre[3], float radius, float amount, filter*, cb, user, ...): MULE melee (kick /
//     push, 0x142B53C20 -> 0x142993C70), the counter-push (0x142AF5F30 -> 0x142993440), DSWeaponEntity shape-0 strikes
//     (0x142916DE0 -> 0x1429933B0), and every other user (player states, catchers, the vehicle tackle, the mod's own calls);
//   the capsule 0x14298A3F0 (ctx, const double a[3], const double b[3], float radius, float amount, filter*, ...): DSWeaponEntity
//     shape-1 strikes (0x142917C22 / 0x142917F81), which bypass the sphere;
//   MeleeAttackEventInstance Start 0x1424BAA70 (this): the paired grab; 0 = engaged, this+0x60 / +0x68 the attacker / target actors.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace st::phrules {

// ---------------------------------------------------------------- the body capsule (a player standing: feet = the entity's root)
inline constexpr double kBodyRadius = 0.35;   // m (her Humanoid blocker capsule is 0.40; the standing body is thinner)
inline constexpr double kBodyHeight = 1.80;   // m, feet to the top of the head
inline constexpr double kNearMargin = 1.0;    // m: a gap within this counts as "near"
inline constexpr double kJumpStepM = 3.0;     // m in one tick: a teleport (60 Hz: 180 m/s)

struct V3 {
    double x = 0, y = 0, z = 0;
};
inline V3 Sub(const V3& a, const V3& b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline double Dot(const V3& a, const V3& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double Len(const V3& a) noexcept { return std::sqrt(Dot(a, a)); }
inline V3 At(const V3& a, const V3& d, double t) noexcept { return {a.x + d.x * t, a.y + d.y * t, a.z + d.z * t}; }
inline bool Finite(const V3& a) noexcept { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

// The body's axis: from feet + r to feet + h - r (z up).
inline void BodyAxis(const V3& feet, V3* a, V3* b) noexcept {
    *a = {feet.x, feet.y, feet.z + kBodyRadius};
    *b = {feet.x, feet.y, feet.z + kBodyHeight - kBodyRadius};
}
inline double Clamp01(double t) noexcept { return t < 0 ? 0 : t > 1 ? 1 : t; }
// Distance from point p to segment ab.
inline double PointSegDist(const V3& p, const V3& a, const V3& b) noexcept {
    const V3 d = Sub(b, a);
    const double dd = Dot(d, d);
    const double t = dd > 1e-12 ? Clamp01(Dot(Sub(p, a), d) / dd) : 0.0;
    return Len(Sub(p, At(a, d, t)));
}
// Distance between segments p1q1 and p2q2 (Ericson, Real-Time Collision Detection 5.1.9).
inline double SegSegDist(const V3& p1, const V3& q1, const V3& p2, const V3& q2) noexcept {
    const V3 d1 = Sub(q1, p1), d2 = Sub(q2, p2), r = Sub(p1, p2);
    const double a = Dot(d1, d1), e = Dot(d2, d2), f = Dot(d2, r);
    double s = 0, t = 0;
    if (a <= 1e-12 && e <= 1e-12) return Len(r);
    if (a <= 1e-12) {
        t = Clamp01(f / e);
    } else {
        const double c = Dot(d1, r);
        if (e <= 1e-12) {
            s = Clamp01(-c / a);
        } else {
            const double b = Dot(d1, d2), den = a * e - b * b;
            s = den > 1e-12 ? Clamp01((b * f - c * e) / den) : 0.0;
            t = (b * s + f) / e;
            if (t < 0) t = 0, s = Clamp01(-c / a);
            else if (t > 1) t = 1, s = Clamp01((b - c) / a);
        }
    }
    return Len(Sub(At(p1, d1, s), At(p2, d2, t)));
}
// The gap (m) between a query sphere and a standing body at `feet`: <= 0 overlaps. NaN for a bad input.
inline double SphereGap(const V3& centre, double radius, const V3& feet) noexcept {
    if (!Finite(centre) || !Finite(feet) || !std::isfinite(radius) || radius < 0) return std::nan("");
    V3 a, b;
    BodyAxis(feet, &a, &b);
    return PointSegDist(centre, a, b) - kBodyRadius - radius;
}
// The same for a query capsule (axis a..b, radius).
inline double CapsuleGap(const V3& qa, const V3& qb, double radius, const V3& feet) noexcept {
    if (!Finite(qa) || !Finite(qb) || !Finite(feet) || !std::isfinite(radius) || radius < 0) return std::nan("");
    V3 a, b;
    BodyAxis(feet, &a, &b);
    return SegSegDist(qa, qb, a, b) - kBodyRadius - radius;
}
inline bool Overlaps(double gap) noexcept { return std::isfinite(gap) && gap <= 0.0; }
inline bool Near(double gap) noexcept { return std::isfinite(gap) && gap <= kNearMargin; }

// ---------------------------------------------------------------- the attacker's pose at a call (round 28 P1b)
// Run 20261126-235739: 02029's only kick (5 calls) came 2.3 m from her while it faced our Sam 155 m away (164 deg off her), so
// a gap alone cannot tell "aimed at her, out of reach" from "aimed elsewhere". The attacker's root (+0xC8) and facing (rot row
// 1, +0xEC / +0xF0) at each call give both.
inline constexpr double kFacedDeg = 45.0;  // within this of the line to a body: "faced" it
inline constexpr double kPiD = 3.14159265358979323846;
// Horizontal distance (NaN for a bad input)
inline double HorizDist(const V3& a, const V3& b) noexcept {
    if (!Finite(a) || !Finite(b)) return std::nan("");
    const double dx = b.x - a.x, dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
}
// Degrees (0..180) between a facing yaw (radians, atan2(fy, fx)) and the horizontal line from `from` to `to`; 0 when they are
// within 1 mm horizontally, NaN for a bad input.
inline double FacingOffDeg(double yaw, const V3& from, const V3& to) noexcept {
    if (!std::isfinite(yaw) || !Finite(from) || !Finite(to)) return std::nan("");
    const double dx = to.x - from.x, dy = to.y - from.y;
    if (dx * dx + dy * dy < 1e-6) return 0.0;
    double d = std::atan2(dy, dx) - yaw;
    while (d > kPiD) d -= 2 * kPiD;
    while (d < -kPiD) d += 2 * kPiD;
    return std::fabs(d) * 180.0 / kPiD;
}
// The yaw of a facing vector (fx, fy); NaN when it is not finite or (near) zero.
inline double YawOfFacing(float fx, float fy) noexcept {
    if (!std::isfinite(fx) || !std::isfinite(fy) || static_cast<double>(fx) * fx + static_cast<double>(fy) * fy < 1e-6) return std::nan("");
    return std::atan2(static_cast<double>(fy), static_cast<double>(fx));
}
inline bool Faced(double off_deg) noexcept { return std::isfinite(off_deg) && off_deg <= kFacedDeg; }
// The query shape's centre: the sphere's centre, or the capsule's midpoint
inline V3 Mid(const V3& a, const V3& b) noexcept { return {(a.x + b.x) * 0.5, (a.y + b.y) * 0.5, (a.z + b.z) * 0.5}; }

// ---------------------------------------------------------------- actors and attacks
inline constexpr std::uint32_t kNoActor = 0x1FFFFF;
// A human enemy's game actor id (enemy::IsHumanEnemyId: MULEs, Demens, warriors)
inline constexpr bool IsMuleId(std::uint32_t id) noexcept { return id != kNoActor && (id & 0x1FE000u) == 0x2000u; }

inline const char* AttackName(std::uint16_t a) noexcept {
    switch (a) {
        case 0x10B: return "PlayerTackle";
        case 0x11D: return "PlayerSliding";
        case 0x11E: return "PlayerLowKick";
        case 0x157: return "VehicleTackle";
        case 0x158: return "VehicleTacklePlayer";
        case 0x15E: return "Push";
        case 0x15F: return "BlastWave";
        case 0x16D: return "MultiRodSwing";
        case 0x16E: return "MultiRodThrust";
        case 0x16F: return "MuleKick";
        case 0x170: return "MulePush";
        case 0x171: return "MuleTackle";
        case 0x172: return "CliffPush";
        case 0x173: return "CliffKick";
        case 0x177: return "HeavyPhysics";
        case 0x178: return "TarWave";
        case 0x17A: return "ToxicGasZone";
        case 0x17B: return "ElectricZone";  // run 20261126-235739: 356 calls (r 1.30, layer 0x1D, main thread) at site 0x142E44DBF
        case 0x17C: return "TranquilizerGasZone";
        case 0x17F: return "AmeliePush";
        case 0x180: return "AimTarget";
        default: return "?";
    }
}
// The MULE melee kinds the E2 relay is about (kick, push, rod, tackle)
inline constexpr bool IsMuleMelee(std::uint16_t a) noexcept { return a == 0x15E || (a >= 0x16D && a <= 0x171); }

// ---------------------------------------------------------------- caller sites (ds.exe VAs of the E8 call instructions)
enum class Query : std::uint8_t { sphere = 0, capsule = 1, grab = 2 };
inline const char* QueryName(Query q) noexcept { return q == Query::sphere ? "sphere" : q == Query::capsule ? "capsule" : "grab"; }

// The four wrappers of the sphere: their return address inside them (the call + 5) and their frame below the outer return
// address (sub rsp, n; no pushes): the outer caller's return address is at [inner return slot + 8 + frame].
struct Wrapper {
    std::uint64_t entry, call_va, frame;
    const char* name;
};
inline constexpr Wrapper kWrappers[4] = {
    {0x1429933B0ull, 0x14299342Aull, 0x98, "0x1429933B0"},
    {0x142993440ull, 0x1429934C9ull, 0x98, "0x142993440"},
    {0x142993C70ull, 0x142993D08ull, 0x98, "0x142993C70"},
    {0x14294B000ull, 0x14294B0BAull, 0xB8, "0x14294B000"},
};
// The wrapper index whose call returns to `ret_va` (a ds.exe VA), -1 none
inline int WrapperOfReturn(std::uint64_t ret_va) noexcept {
    for (int i = 0; i < 4; ++i)
        if (kWrappers[i].call_va + 5 == ret_va) return i;
    return -1;
}

struct SiteLabel {
    std::uint64_t call_va;
    const char* label;
};
// clang-format off
inline constexpr SiteLabel kSites[] = {
    {0x142B53F99ull, "mule-melee"},          // DSMuleWeaponComponent melee state Update 0x142B53C20 (kick 0x16F / push 0x170) via 0x142993C70
    {0x142AF6090ull, "mule-counter-push"},   // DSMuleDamageComponent 0x142AF5F30 (push 0x170 while hit) via 0x142993440
    {0x142917C7Cull, "weapon-sphere"},       // DSWeaponEntity 0x142916DE0, mode 5 shape 0 (rod 0x16D / 0x16E, tackle 0x171) via 0x1429933B0
    {0x14291803Aull, "weapon-sphere"},
    {0x142917C22ull, "weapon-capsule"},      // the same, shape 1 (the capsule query)
    {0x142917F81ull, "weapon-capsule"},
    {0x142ACB40Eull, "bt-tar"},              // DSTarMeshOnTerrainComponent
    {0x142ACBCFCull, "bt-whale-bomb"},       // DSWhaleBombComponent
    {0x142F8AEF5ull, "vehicle-tackle"},      // VehicleTackleEmit via 0x142993440
    {0x142F8AFFAull, "vehicle-capsule"},
    {0x14300B8C2ull, "amelie-push"},
    {0x1427FED76ull, "player-state"},        // DSPlayerState strikes
    {0x142868A42ull, "player-state"},
    {0x142873B79ull, "player-state"},
    {0x142865336ull, "player-state"},
    {0x1428669E2ull, "player-state"},
    {0x142868087ull, "player-state"},
    {0x1429BD7D8ull, "catcher-jellyfish"},
    {0x142A1FD55ull, "catcher-quad"},
    {0x142A5AEECull, "catcher-quad"},
    {0x142A8AA26ull, "catcher-whale"},
    {0x142E0FC23ull, "warrior-mech"},
    {0x142FBA01Dull, "cliff"},
    {0x142FBA10Bull, "cliff"},
    {0x142FBA2EFull, "cliff"},
    {0x142C04791ull, "shared-substance"},
    {0x142C352AEull, "baggage2"},
    {0x142E44DBFull, "zone"},                // 0x142E44B00 via 0x142993440 (class unknown): the ElectricZone 0x17B ticks (run 20261126-235739)
};
// clang-format on
inline const char* SiteName(std::uint64_t call_va) noexcept {
    for (const SiteLabel& s : kSites)
        if (s.call_va == call_va) return s.label;
    return "other";
}
// The sites the E2 relay is about (the MULE's own melee paths)
inline bool IsMuleSite(std::uint64_t call_va) noexcept {
    const char* n = SiteName(call_va);
    return std::strcmp(n, "mule-melee") == 0 || std::strcmp(n, "mule-counter-push") == 0 || std::strcmp(n, "weapon-sphere") == 0 ||
           std::strcmp(n, "weapon-capsule") == 0;
}

// ---------------------------------------------------------------- small tallies (game thread)
// Up to N distinct keys with counts; a full table counts the rest in `other`.
template <unsigned N, class K>
struct Tally {
    K key[N] = {};
    std::uint32_t n[N] = {};
    unsigned used = 0;
    std::uint32_t other = 0;
    void Add(K k) noexcept {
        for (unsigned i = 0; i < used; ++i)
            if (key[i] == k) return void(++n[i]);
        if (used < N) {
            key[used] = k, n[used] = 1, ++used;
            return;
        }
        ++other;
    }
    std::uint32_t Of(K k) const noexcept {
        for (unsigned i = 0; i < used; ++i)
            if (key[i] == k) return n[i];
        return 0;
    }
};

// One attacker's (or one site's) numbers.
struct Stat {
    std::uint32_t calls = 0, sphere = 0, capsule = 0;
    std::uint32_t pbody = 0, pbody_near = 0, sam = 0, sam_near = 0;
    std::uint32_t hits = 0;  // the query's return summed (receivers)
    double min_pbody = 1e9, min_sam = 1e9;
    float r_min = 1e9f, r_max = -1.0f;
    Tally<6, std::uint16_t> attacks;
    Tally<4, std::uint64_t> sites;
    Tally<3, std::uint32_t> layers;
    Tally<4, std::uint32_t> attackers;
    void Add(Query q, std::uint16_t attack, std::uint64_t site, std::uint32_t layer, std::uint32_t attacker, float radius, double gap_pbody,
             double gap_sam, std::uint32_t ret) noexcept {
        ++calls;
        if (q == Query::capsule) ++capsule;
        else ++sphere;
        if (Overlaps(gap_pbody)) ++pbody;
        if (Near(gap_pbody)) ++pbody_near;
        if (Overlaps(gap_sam)) ++sam;
        if (Near(gap_sam)) ++sam_near;
        if (std::isfinite(gap_pbody) && gap_pbody < min_pbody) min_pbody = gap_pbody;
        if (std::isfinite(gap_sam) && gap_sam < min_sam) min_sam = gap_sam;
        if (std::isfinite(radius)) {
            if (radius < r_min) r_min = radius;
            if (radius > r_max) r_max = radius;
        }
        hits += ret > 0xFFFF ? 0 : ret;
        attacks.Add(attack);
        sites.Add(site);
        layers.Add(layer);
        attackers.Add(attacker);
    }
    // The attacker's pose at a call (P1b): its horizontal distance and facing offset to the partner body and to our Sam, and its
    // reach (the horizontal distance from its root to the shape's centre). NaN parts are skipped.
    std::uint32_t posed = 0, faced_pbody = 0, faced_sam = 0;
    double min_att_pbody = 1e9, max_reach = -1.0;
    void AddPose(double d_pbody, double off_pbody, double off_sam, double reach) noexcept {
        ++posed;
        if (std::isfinite(d_pbody) && d_pbody < min_att_pbody) min_att_pbody = d_pbody;
        if (Faced(off_pbody)) ++faced_pbody;
        if (Faced(off_sam)) ++faced_sam;
        if (std::isfinite(reach) && reach > max_reach) max_reach = reach;
    }
};

// ---------------------------------------------------------------- the ring record's packing (two 32-bit halves of a word)
inline constexpr std::uint64_t Pack(std::uint32_t lo, std::uint32_t hi) noexcept { return static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32); }
inline constexpr std::uint32_t Lo(std::uint64_t w) noexcept { return static_cast<std::uint32_t>(w); }
inline constexpr std::uint32_t Hi(std::uint64_t w) noexcept { return static_cast<std::uint32_t>(w >> 32); }
inline std::uint32_t FBits(float f) noexcept {
    std::uint32_t u = 0;
    std::memcpy(&u, &f, 4);
    return u;
}
inline float BitsF(std::uint32_t u) noexcept {
    float f = 0;
    std::memcpy(&f, &u, 4);
    return f;
}
inline std::uint64_t DBits(double d) noexcept {
    std::uint64_t u = 0;
    std::memcpy(&u, &d, 8);
    return u;
}
inline double BitsD(std::uint64_t u) noexcept {
    double d = 0;
    std::memcpy(&d, &u, 8);
    return d;
}

// The amount the query uses (decompile 0x1429934E0 lines 137-141): a negative amount takes the attack resource's default.
inline float EffectiveAmount(float amount, bool have_res, float res_default) noexcept { return amount < 0 && have_res ? res_default : amount; }

// The watch's length (s): default 60 (absent, below 1 or NaN), at most 300 (a longer request is capped).
inline double WatchSecs(double v) noexcept { return !(v >= 1) ? 60.0 : v > 300 ? 300.0 : v; }


// ---------------------------------------------------------------- round 28 E2 (docs/enemy-sync.md 20): the PHIT relay's pure rules
// The host sees its MULE's melee query reach her partner body (the gap <= 0) and sends PHIT; her game applies that strike to her
// real Sam (enemy_phit.cpp). The attacks relayed: the MULE melee strikes, rod swing 0x16D, rod thrust 0x16E, kick 0x16F, punch
// 0x170, tackle 0x171. Not her push 0x15E (the counter-push answer to her own push), not ElectricZone 0x17B (an area shock on the
// main thread: a stated limit), not the paired grab (none seen in the P1 runs: logged only).
inline constexpr bool PhitAttackOk(std::uint16_t a) noexcept { return a >= 0x16D && a <= 0x171; }
inline constexpr double kPhitDedupeMs = 300.0;   // one strike is several queries (a kick ~5 calls): one PHIT per MULE + attack
                                                 // (a sliding window: a repeat within it moves it on)
inline constexpr unsigned kPhitPerMuleSec = 3;   // PHITs per MULE in any 1 s
inline constexpr unsigned kPhitAllSec = 8;       // all PHITs in any 1 s
inline constexpr double kPhitDodgeM = 4.0;       // her real Sam moved more than this from where the host saw her: dodged
inline constexpr double kPhitPointMaxM = 8.0;    // the contact point within this of her feet at detection (the wire's bound too)
inline constexpr double kMeleeFaceM = 6.0;       // redirect bit 64: her feet within this of the MULE (horizontal)
inline constexpr double kMeleeFaceDzM = 3.0;     // and within this in height
inline constexpr double kChestDz = 1.0;          // her Sam's apply centre: root + this
inline constexpr float kApplyRadius = 0.5f;      // the apply query's radius (the MULE melee sphere's)

// The contact point of a query that reached a body: the sphere's centre; on a capsule the axis point nearest the body's mid axis.
inline V3 ClosestOnSeg(const V3& p, const V3& a, const V3& b) noexcept {
    const V3 d = Sub(b, a);
    const double dd = Dot(d, d);
    return At(a, d, dd > 1e-12 ? Clamp01(Dot(Sub(p, a), d) / dd) : 0.0);
}
inline V3 ContactPoint(bool capsule, const V3& qa, const V3& qb, const V3& feet) noexcept {
    if (!capsule) return qa;
    return ClosestOnSeg({feet.x, feet.y, feet.z + kBodyHeight * 0.5}, qa, qb);
}
// The strike's horizontal direction (unit, from the attacker's root to the contact point); {0,0} when it is degenerate.
inline void StrikeDir(const V3& attacker, const V3& contact, float* dx, float* dy) noexcept {
    const double x = contact.x - attacker.x, y = contact.y - attacker.y, l = std::sqrt(x * x + y * y);
    if (!(l > 1e-6) || !std::isfinite(l)) {
        *dx = 0, *dy = 0;
        return;
    }
    *dx = static_cast<float>(x / l), *dy = static_cast<float>(y / l);
}

// The host's detection test (a worker thread, per query): a MULE's melee attack, not the mod's own call, reaching her body.
inline bool PhitCandidate(bool mod_call, std::uint32_t attacker, std::uint16_t attack, double gap_pbody) noexcept {
    return !mod_call && IsMuleId(attacker) && PhitAttackOk(attack) && Overlaps(gap_pbody);
}

// One PHIT per MULE + attack until kPhitDedupeMs pass with no query of it (the game thread; a fixed table, an empty or the oldest
// entry reused). The window slides: every repeat moves it on, so one long strike (a kick's queries span 266-281 ms) is one PHIT
// even when its last queries are drained more than 300 ms after its first.
struct PhitDedupe {
    static constexpr unsigned kN = 32;
    std::uint64_t key[kN] = {};
    double at[kN] = {};
    static constexpr std::uint64_t Key(std::uint32_t id, std::uint16_t attack) noexcept {
        return (static_cast<std::uint64_t>(id) << 16) | attack | (1ull << 63);
    }
    // true: send it (and remember it); false: a repeat of the same strike
    bool Take(std::uint32_t id, std::uint16_t attack, double now_ms) noexcept {
        const std::uint64_t k = Key(id, attack);
        int empty = -1;
        unsigned oldest = 0;
        for (unsigned i = 0; i < kN; ++i) {
            if (key[i] == k) {
                const bool repeat = now_ms >= at[i] && now_ms - at[i] < kPhitDedupeMs;
                at[i] = now_ms;  // a repeat moves the window on; a new strike starts it
                return !repeat;
            }
            if (key[i] == 0) {
                if (empty < 0) empty = static_cast<int>(i);
            } else if (at[i] < at[oldest] || key[oldest] == 0) {
                oldest = i;
            }
        }
        const unsigned slot = empty >= 0 ? static_cast<unsigned>(empty) : oldest;
        key[slot] = k, at[slot] = now_ms;
        return true;
    }
    void Reset() noexcept { *this = PhitDedupe{}; }
};

// The rate cap (the game thread): at most kPhitPerMuleSec per MULE and kPhitAllSec in all within any 1000 ms.
struct PhitRate {
    static constexpr unsigned kN = kPhitAllSec;
    std::uint32_t id[kN] = {};
    double at[kN] = {};
    unsigned used = 0, next = 0;
    bool Take(std::uint32_t mule, double now_ms) noexcept {
        unsigned all = 0, mine = 0;
        for (unsigned i = 0; i < used; ++i) {
            if (now_ms >= at[i] && now_ms - at[i] < 1000.0) {
                ++all;
                if (id[i] == mule) ++mine;
            }
        }
        if (all >= kPhitAllSec || mine >= kPhitPerMuleSec) return false;
        id[next] = mule, at[next] = now_ms;
        next = (next + 1) % kN;
        if (used < kN) ++used;
        return true;
    }
    void Reset() noexcept { *this = PhitRate{}; }
};

// The guest's freshness test of a relayed strike: how far it arrives behind the best seen. off = (her ms) - (the host's sender_ms),
// modulo 2^32; the base is the smallest off seen (the link's one-way delay plus the clocks' offset); it may rise to the last
// window's smallest so a drift or a reconnect does not hold an old base for ever. Observe returns the lag behind the base (ms).
inline constexpr double kPhitStaleLagMs = 1000.0;      // a strike this far behind the best seen is a backlog: refused (stale)
inline constexpr double kPhitLagWindowMs = 20000.0;
inline constexpr double kPhitHelloRepeatMs = 30000.0;  // the guest's hello again (once per session, on a change, then this slow)
struct PhitLag {
    bool have = false;
    std::uint32_t base = 0, cand = 0;
    double cand_at = 0;
    static constexpr std::int32_t D(std::uint32_t a, std::uint32_t b) noexcept { return static_cast<std::int32_t>(a - b); }
    std::int32_t Observe(std::uint32_t off, double now_ms) noexcept {
        if (!have) {
            have = true, base = cand = off, cand_at = now_ms;
            return 0;
        }
        if (D(off, cand) < 0) cand = off;
        if (now_ms - cand_at >= kPhitLagWindowMs || now_ms < cand_at) base = cand, cand = off, cand_at = now_ms;  // the last window's best
        if (D(off, base) < 0) base = off;
        return D(off, base);
    }
    void Reset() noexcept { *this = PhitLag{}; }
};

// The guest's checks on a received PHIT, in order (the first that fails names the refusal).
enum class PhitVerdict : std::uint8_t { apply = 0, off, no_peer, not_valid, no_sam, not_on_foot, down, cutscene, dodged, no_owner };
inline const char* PhitVerdictText(PhitVerdict v) noexcept {
    switch (v) {
        case PhitVerdict::apply: return "apply";
        case PhitVerdict::off: return "off (enemy_partner_melee=0)";
        case PhitVerdict::no_peer: return "another world (the guids fingerprint)";
        case PhitVerdict::not_valid: return "not valid";
        case PhitVerdict::no_sam: return "no Sam";
        case PhitVerdict::not_on_foot: return "not on foot";
        case PhitVerdict::down: return "she is down";
        case PhitVerdict::cutscene: return "a cutscene or a load";
        case PhitVerdict::dodged: return "dodged";
        case PhitVerdict::no_owner: return "no mirror of the MULE (mode 1)";
    }
    return "?";
}
struct PhitIn {
    bool key = false, world = false, valid = false, sam = false, on_foot = false, down = false, cutscene = false;
    double moved_m = 0;                       // her real Sam's root now to her body's root when the host saw the strike
    bool need_owner = false, owner = false;   // mode 1 wants the MULE's mirror as the attacker
};
inline PhitVerdict PhitDecide(const PhitIn& x) noexcept {
    if (!x.key) return PhitVerdict::off;
    if (!x.world) return PhitVerdict::no_peer;
    if (!x.valid) return PhitVerdict::not_valid;
    if (!x.sam) return PhitVerdict::no_sam;
    if (!x.on_foot) return PhitVerdict::not_on_foot;
    if (x.down) return PhitVerdict::down;
    if (x.cutscene) return PhitVerdict::cutscene;
    if (!std::isfinite(x.moved_m) || x.moved_m > kPhitDodgeM) return PhitVerdict::dodged;
    if (x.need_owner && !x.owner) return PhitVerdict::no_owner;
    return PhitVerdict::apply;
}
// A PHIT's own fields: a MULE, a relayed attack, a finite contact point within kPhitPointMaxM of her feet at detection.
inline bool PhitValid(std::uint32_t id, std::uint16_t attack, const V3& point, const V3& her_at) noexcept {
    return IsMuleId(id) && PhitAttackOk(attack) && Finite(point) && Finite(her_at) && Len(Sub(point, her_at)) <= kPhitPointMaxM;
}
// Mode 2's explicit amount: the attack entry's +0x28 (10 for the MULE melee ids in the registry dump), read as f32 when that is
// a sane amount, else as i32, else 10; capped at 100 (never an instant kill).
inline float PhitAmount(std::uint32_t bits28) noexcept {
    float f = 0;
    std::memcpy(&f, &bits28, 4);
    if (std::isfinite(f) && f >= 1.0f && f <= 1000.0f) return f > 100.0f ? 100.0f : f;  // >= 1: an i32 (a denormal as f32) falls through
    const auto i = static_cast<std::int32_t>(bits28);
    if (i > 0 && i <= 1000) return i > 100 ? 100.0f : static_cast<float>(i);
    return 10.0f;
}
// Redirect bit 64 (enemy_target.cpp MeleeFix): her feet near enough the MULE for its strike to be aimed at her.
inline bool MeleeFaceWants(const V3& mule, const V3& her) noexcept {
    return Finite(mule) && Finite(her) && HorizDist(mule, her) <= kMeleeFaceM && std::fabs(mule.z - her.z) <= kMeleeFaceDzM;
}
// session.ini enemy_partner_melee: 0 off, 1 (the default) the MULE's mirror as the attacker with the native amount, 2 no attacker
// and the attack entry's amount (meant as the fallback; it applies with no damage in our runs: use 1); anything else 0.
inline constexpr int PartnerMeleeMode(int v) noexcept { return v == 1 || v == 2 ? v : 0; }

}  // namespace st::phrules
