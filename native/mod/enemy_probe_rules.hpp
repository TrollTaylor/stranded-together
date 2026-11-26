// Round 23 E (docs/enemy-sync.md 14.11): the M2 probes' pure part (design section 10, critique R2 / R3). No game memory, no link,
// no Windows. Unit-tested in tests/enemy_probe_rules_tests.cpp (st_enemy_probe_rules_tests).
//
// The probes themselves (enemy_combat.cpp, the debug action enemy_m2, hidden runs with enemy_m2_probes=1) only read the game,
// except P2's forced hand-over and its levers. This header holds what they decide:
//   KnowledgeTypeName   the knowledge entry types the probes name (PlayerVisual 1, PlayerNoise 51, Damaged 81, PlayerLastKnown 85)
//   Fresh / FreshTrack  P1: whether a PlayerVisual entry for our Sam is being written (its bytes changed within a window), was
//                       written once and left (stale), or is not there (none); the verdict over the whole probe
//   Wrap180 / OffDeg    angles: a MULE's facing against a point, within a cone
//   Push                P2: the time from the hand-over to "within 30 deg of our Sam", to its first shot and (round 24) to its
//                       first attack (a shot or a melee hit on our Sam), and its hits on our Sam by kind
//   PhaseTrack          P5: the phase's changes over a probe (start, end, changes, the first raise)
//   Shorten             P4: a ray shortened at both ends (its own and the target's bodies left out)
//   EntrySig            P1 / P5: a 64-bit FNV-1a of an entry's bytes (its change test)
//   PlausiblePtr        the knowledge search's candidate pointers
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace st::eprobe {

inline constexpr int kPlayerVisual = 1, kPlayerNoise = 51, kDamaged = 81, kPlayerLastKnown = 85;
inline constexpr double kFreshWindowMs = 1500.0;  // P1: an entry whose bytes changed within this is "fresh"
inline constexpr double kConeDeg = 30.0;          // P2: "turned to our Sam"
inline constexpr double kRayTrimM = 0.6;          // P4: off each end of the ray

inline const char* KnowledgeTypeName(int t) noexcept {
    switch (t) {
        case kPlayerVisual: return "PlayerVisual";
        case kPlayerNoise: return "PlayerNoise";
        case kDamaged: return "Damaged";
        case kPlayerLastKnown: return "PlayerLastKnown";
        default: return "other";
    }
}

// ---- P1: fresh / stale / none
enum class Fresh : std::uint8_t { none, fresh, stale };
inline const char* FreshText(Fresh f) noexcept { return f == Fresh::fresh ? "fresh" : f == Fresh::stale ? "stale" : "none"; }
struct FreshTrack {
    bool have = false;
    std::uint64_t sig = 0;
    double changed_ms = 0;
    unsigned n_none = 0, n_fresh = 0, n_stale = 0;
    // One sample: present (an entry for our Sam found) and its bytes' signature. The first sight of an entry is "stale" (it may be
    // an old one); a change within the window is "fresh"; a missing entry is "none" (and forgets the last).
    Fresh Feed(bool present, std::uint64_t s, double now, double window = kFreshWindowMs) noexcept {
        Fresh f = Fresh::none;
        if (!present) {
            have = false;
        } else if (!have) {
            have = true, sig = s, changed_ms = now - window - 1.0;
            f = Fresh::stale;
        } else {
            if (s != sig) sig = s, changed_ms = now;
            f = now - changed_ms <= window ? Fresh::fresh : Fresh::stale;
        }
        (f == Fresh::fresh ? n_fresh : f == Fresh::stale ? n_stale : n_none) += 1;
        return f;
    }
    // the probe's verdict: fresh if any sample was, else stale if any was, else none
    Fresh Verdict() const noexcept { return n_fresh ? Fresh::fresh : n_stale ? Fresh::stale : Fresh::none; }
};

// ---- angles
inline double Wrap180(double d) noexcept {
    if (!std::isfinite(d)) return 0.0;
    d = std::fmod(d, 360.0);
    if (d > 180.0) d -= 360.0;
    if (d < -180.0) d += 360.0;
    return d;
}
// degrees between a facing (yaw, radians, atan2 of the forward's y / x) and the direction from (fx, fy) to (tx, ty); -1 when the two
// places are within 0.1 m
inline double OffDeg(double yaw_rad, double fx, double fy, double tx, double ty) noexcept {
    const double dx = tx - fx, dy = ty - fy;
    if (dx * dx + dy * dy < 0.01) return -1.0;
    constexpr double kDeg = 57.29577951308232;
    return std::fabs(Wrap180(std::atan2(dy, dx) * kDeg - yaw_rad * kDeg));
}
inline bool WithinCone(double off_deg, double cone = kConeDeg) noexcept { return off_deg >= 0.0 && off_deg <= cone; }

// ---- P2: the push measurement
inline const char* LeverName(int lever) noexcept {
    switch (lever) {
        case 0: return "a (baseline: nothing)";
        case 1: return "b (a test hit, owner our Sam)";
        case 2: return "c (AlertForEntityWithPerfectThreatInfo -> our Sam)";
        case 3: return "d' (our Sam's own visual stimulus +0xA4 x3 for 1 s)";
        default: return "?";
    }
}
// Round 24: a hit on our Sam counts as a shot's when the MULE's shot count rose in that sample or within this before it, else as
// melee (a strike: run 20261125-154710's 02029 hit her Sam with no shot of its own). Hit messages closer than kAttackGapMs are one
// attack (a melee swing sends a burst of them ~15 ms apart).
inline constexpr double kShotHitWindowMs = 2000.0;
inline constexpr double kAttackGapMs = 300.0;
enum class Attack : std::uint8_t { none, shot, melee };
inline const char* AttackText(Attack a) noexcept { return a == Attack::shot ? "shot" : a == Attack::melee ? "melee" : "none"; }
struct Push {
    double t0 = -1;           // the hand-over seen here (ms), -1 not yet
    double turn_ms = -1;      // first within the cone, from t0
    double shot_ms = -1;      // first shot, from t0
    double melee_ms = -1;     // first melee hit on our Sam (a hit with no shot of its own before it), from t0
    double start_off = -1;    // its angle to our Sam at t0
    double min_off = 1e9;     // the smallest seen
    unsigned samples = 0;
    unsigned shots = 0;       // its shots since t0
    unsigned hits = 0, shot_hits = 0, melee_hits = 0;  // hit messages on our Sam since t0 (all; by kind)
    unsigned attacks = 0;     // hit bursts (kAttackGapMs apart)
    double last_shot_rise = -1e18, last_hit = -1e18;
    void Owned(double now, double off) noexcept {
        t0 = now, start_off = off, samples = 0, min_off = 1e9, turn_ms = shot_ms = melee_ms = -1;
        shots = hits = shot_hits = melee_hits = attacks = 0;
        last_shot_rise = last_hit = -1e18;
        Sample(now, off, 0, 0);
    }
    // shots_since / hits_since: its counts since t0 (monotonic)
    void Sample(double now, double off, unsigned shots_since, unsigned hits_since = 0) noexcept {
        if (t0 < 0) return;
        ++samples;
        if (off >= 0 && off < min_off) min_off = off;
        if (turn_ms < 0 && WithinCone(off)) turn_ms = now - t0;
        if (shots_since > shots) shots = shots_since, last_shot_rise = now;
        if (shot_ms < 0 && shots_since > 0) shot_ms = now - t0;
        if (hits_since > hits) {
            const unsigned d = hits_since - hits;
            hits = hits_since;
            const bool by_shot = now - last_shot_rise <= kShotHitWindowMs;
            (by_shot ? shot_hits : melee_hits) += d;
            if (!by_shot && melee_ms < 0) melee_ms = now - t0;
            if (now - last_hit > kAttackGapMs) ++attacks;
            last_hit = now;
        }
    }
    // the first attack: a shot or a melee hit, whichever came first (a shot wins a tie)
    Attack FirstAttack() const noexcept {
        if (shot_ms >= 0 && (melee_ms < 0 || shot_ms <= melee_ms)) return Attack::shot;
        return melee_ms >= 0 ? Attack::melee : Attack::none;
    }
    double FirstAttackMs() const noexcept {
        const Attack a = FirstAttack();
        return a == Attack::shot ? shot_ms : a == Attack::melee ? melee_ms : -1;
    }
    // full: run the whole window (count the hits); else done once turned and shot
    bool Done(double now, double timeout_ms, bool full = false) const noexcept {
        return t0 >= 0 && ((!full && turn_ms >= 0 && shot_ms >= 0) || now - t0 >= timeout_ms);
    }
    // "turned 1200 ms, shot 2400 ms" / "turned never, shot never"
    int Text(char* out, std::size_t cap) const noexcept {
        char a[32], b[32];
        if (turn_ms >= 0) std::snprintf(a, sizeof a, "%.0f ms", turn_ms);
        else std::snprintf(a, sizeof a, "never");
        if (shot_ms >= 0) std::snprintf(b, sizeof b, "%.0f ms", shot_ms);
        else std::snprintf(b, sizeof b, "never");
        return std::snprintf(out, cap, "turned %s, shot %s", a, b);
    }
    // "first attack after 2400 ms (shot), hits on our Sam 3 (shot 3, melee 0; attacks 1; shots 4)" / "first attack never, hits on
    // our Sam 0 (...)"
    int AttackLine(char* out, std::size_t cap) const noexcept {
        char a[48];
        const Attack k = FirstAttack();
        if (k != Attack::none) std::snprintf(a, sizeof a, "after %.0f ms (%s)", FirstAttackMs(), AttackText(k));
        else std::snprintf(a, sizeof a, "never");
        return std::snprintf(out, cap, "first attack %s, hits on our Sam %u (shot %u, melee %u; attacks %u; shots %u)", a, hits, shot_hits, melee_hits, attacks, shots);
    }
};

// ---- P5: the phase over a probe
struct PhaseTrack {
    int start = -999, last = -999, max = -999;
    unsigned changes = 0;
    double t0 = 0, first_raise_ms = -1;
    // true when the phase changed (the first sample sets the start)
    bool Feed(double now, int phase) noexcept {
        if (start == -999) {
            start = last = max = phase, t0 = now;
            return false;
        }
        if (phase > max) max = phase;
        if (phase > start && first_raise_ms < 0) first_raise_ms = now - t0;
        if (phase == last) return false;
        last = phase, ++changes;
        return true;
    }
};

// ---- P4: a ray shortened by m at both ends; false when it is shorter than 2 m + 0.1 m
inline bool Shorten(const double (&from)[3], const double (&to)[3], double m, double (&ray)[6], double* len) noexcept {
    const double d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    const double l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len) *len = l;
    if (!(l > 2.0 * m + 0.1)) return false;
    for (int i = 0; i < 3; ++i) {
        ray[i] = from[i] + d[i] / l * m;
        ray[3 + i] = to[i] - d[i] / l * m;
    }
    return true;
}

// ---- an entry's change test
inline std::uint64_t EntrySig(const std::uint8_t* p, std::size_t n) noexcept {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// a user-mode heap pointer (8-aligned, above 64 KiB, below the 47-bit limit) and not in the game image
inline bool PlausiblePtr(std::uint64_t v, std::uint64_t image_lo, std::uint64_t image_hi) noexcept {
    if (v < 0x10000ull || v >= 0x800000000000ull || (v & 7u)) return false;
    return !(v >= image_lo && v < image_hi);
}

// an entry's actor id against ours: 1 the same id, 2 the same 21-bit id (the game's id mask 0x1FFFFF), 0 no
inline int SameActor(std::uint32_t entry, std::uint32_t ours) noexcept {
    if (!ours) return 0;
    if (entry == ours) return 1;
    return (entry & 0x1FFFFFu) == (ours & 0x1FFFFFu) ? 2 : 0;
}

}  // namespace st::eprobe
