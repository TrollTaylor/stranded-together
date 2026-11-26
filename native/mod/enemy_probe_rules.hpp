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
// Round 25 (docs/enemy-sync.md 14.18, walk-away / behind):
//   P2Window / P2Due    the P2 window argument (lever + 100 x secs) and its sampling schedule (0.25 s for 3 s, then 1 s)
//   PathTrack           D1: the taken MULE's path in her world (max distance from her Sam, stop time, facing-her time)
//   KnowEntryText       D2: one knowledge entry as text (type name, or "none")
//   KnowPush            K1: when to write the forced player visual (write / wait / done)
//   CampRefreshWanted   C1: whether the take forces her camp to 5 first
//   TicketsToReturn     T1: whether a frozen copy's individual really holds a ticket entry (the return is then called once)
//   RoleWhileHers       R2: the camp role her taken MULE gets while it is hers (role 1 -> 0)
//   FaceYaw             F1: the yaw her taken MULE is turned to at the take (her Sam more than 60 deg off, 0.5..30 m)
//   TakeAlone           round 26: whether her take is a single take (the levers act on those only)
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

inline constexpr int kDamagedAtPosition = 0x52, kCPOrder = 0x54, kInCombat = 0x5D;  // round 25 (D2)
inline const char* KnowledgeTypeName(int t) noexcept {
    switch (t) {
        case kPlayerVisual: return "PlayerVisual";
        case kPlayerNoise: return "PlayerNoise";
        case kDamaged: return "Damaged";
        case kDamagedAtPosition: return "DamagedAtPosition";
        case kCPOrder: return "CPOrder";
        case kPlayerLastKnown: return "PlayerLastKnown";
        case kInCombat: return "InCombat";
        default: return t < 0 ? "none" : "other";
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
        case 4: return "e (K1: forced player visual, the game's own flag K+0x736)";
        case 5: return "f (C1: her camp forced to 5 at the take)";
        case 6: return "g (T1: tickets held by frozen copies returned)";
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

// ---- Round 25: the P2 window argument. Actions carry three numbers, so "enemy_m2 2 <id> <v>" packs v = lever + 100 x secs
// (3000: lever a for 30 s; 4: lever e for the default). secs 0 = the default 20, above 40 = 40. Levers 0..6; else false.
inline constexpr int kP2DefaultSecs = 20, kP2MaxSecs = 40, kP2Levers = 7;
inline bool P2Window(double v, int* lever, int* secs) noexcept {
    if (!(v >= 0.0) || v > 1.0e6 || v != std::floor(v)) return false;
    const long n = static_cast<long>(v);
    const int l = static_cast<int>(n % 100);
    long s = n / 100;
    if (l >= kP2Levers) return false;
    if (s == 0) s = kP2DefaultSecs;
    if (s > kP2MaxSecs) s = kP2MaxSecs;
    if (lever) *lever = l;
    if (secs) *secs = static_cast<int>(s);
    return true;
}
// the sampling schedule: every 0.25 s for the first 3 s after the hand-over (the decision splits at 1-2 s), then every 1 s.
// t_ms since the hand-over; last_ms the last sample's t (< 0: none yet).
inline double P2StepMs(double t_ms) noexcept { return t_ms < 3000.0 ? 250.0 : 1000.0; }
inline bool P2Due(double t_ms, double last_ms) noexcept { return last_ms < 0.0 || t_ms - last_ms >= P2StepMs(t_ms) - 1.0; }

// ---- D1: the path summary. Add each sample (t in s, the MULE's x / y in her world, its distance to her Sam, the angle of her
// Sam off its facing, deg). Stopped: the start of the last stretch under kStillMps to the end (-1 when it moved at the end);
// facing: the first sample within the 30 deg cone (-1 never).
inline constexpr double kStillMps = 0.5, kEarlyS = 10.0;
struct PathTrack {
    int n = 0;
    double sx = 0, sy = 0;          // start
    double ex = 0, ey = 0, et = 0;  // last sample
    double max_d = 0, max_t = 0;    // farthest from her Sam
    double max_d10 = 0;             // farthest in the first 10 s
    double max_walk = 0;            // farthest from the start
    double still_since = -1;        // the current still stretch's start (s)
    double face_t = -1;
    void Add(double t, double x, double y, double d_sam, double off_deg) noexcept {
        if (n == 0) {
            sx = x, sy = y;
            still_since = t;
        } else {
            const double dt = t - et;
            const double v = dt > 1e-3 ? std::sqrt((x - ex) * (x - ex) + (y - ey) * (y - ey)) / dt : 0.0;
            if (v >= kStillMps) still_since = -1;
            else if (still_since < 0) still_since = et;
        }
        if (d_sam > max_d || n == 0) max_d = d_sam, max_t = t;
        if (t <= kEarlyS && d_sam > max_d10) max_d10 = d_sam;
        const double w = std::sqrt((x - sx) * (x - sx) + (y - sy) * (y - sy));
        if (w > max_walk) max_walk = w;
        if (face_t < 0 && WithinCone(off_deg)) face_t = t;
        ex = x, ey = y, et = t;
        ++n;
    }
    double StoppedAt() const noexcept { return n ? still_since : -1; }
    void Text(char* buf, std::size_t cap) const noexcept {
        if (!n) {
            std::snprintf(buf, cap, "no samples");
            return;
        }
        char st[32], fc[32];
        if (StoppedAt() < 0) std::snprintf(st, sizeof st, "moving at the end");
        else std::snprintf(st, sizeof st, "stopped at t %.1f", StoppedAt());
        if (face_t < 0) std::snprintf(fc, sizeof fc, "never facing her");
        else std::snprintf(fc, sizeof fc, "facing her at t %.1f", face_t);
        std::snprintf(buf, cap, "max distance %.1f m at t %.1f, first 10 s %.1f m, walked %.1f m, end (x,y) (%.1f,%.1f), %s, %s", max_d,
                      max_t, max_d10, max_walk, ex, ey, st, fc);
    }
};

// ---- D2: one knowledge entry as text; type < 0 = "none" (no entry of the kind asked for).
inline void KnowEntryText(char* buf, std::size_t cap, int type, const double (&pos)[3], float age, float memory, std::uint32_t actor,
                          std::uint32_t flags, std::uint64_t key) noexcept {
    if (type < 0) {
        std::snprintf(buf, cap, "none");
        return;
    }
    std::snprintf(buf, cap, "%s(0x%X) at (%.1f,%.1f,%.1f) age %.2f mem %.2f actor %06X flags %X key %llX", KnowledgeTypeName(type),
                  static_cast<unsigned>(type), pos[0], pos[1], pos[2], static_cast<double>(age), static_cast<double>(memory), actor, flags,
                  static_cast<unsigned long long>(key));
}
// the types D2 lists one by one (the others are counted)
inline bool KnowListed(int t) noexcept {
    return t == kPlayerVisual || t == kCPOrder || t == kPlayerLastKnown || t == kInCombat || t == kDamaged || t == kDamagedAtPosition;
}

// ---- K1: the forced player visual (enemy_take_know: 0 off, 1 once at the take, 2 at the take then 2 Hz for `secs`).
// now / taken / last_write / faced_since in ms (last_write < 0: never written; faced_since < 0: not within 30 deg now).
// Write only while the game's flag is 0 (its reader clears it), at most 2 Hz, inside the window; done at the first attack, once
// she has been within 30 deg of its facing for 1 s, or when the window ends (mode 1: after its one write).
enum class KnowStep : std::uint8_t { write, wait, done };
inline const char* KnowStepText(KnowStep s) noexcept { return s == KnowStep::write ? "write" : s == KnowStep::wait ? "wait" : "done"; }
inline constexpr double kKnowGapMs = 500.0, kKnowFacedMs = 1000.0;
inline KnowStep KnowPush(double now, double taken, bool flag_set, double last_write, double faced_since, bool attacked, int mode,
                         double secs) noexcept {
    if (mode <= 0) return KnowStep::done;
    if (attacked) return KnowStep::done;
    if (faced_since >= 0.0 && now - faced_since >= kKnowFacedMs) return KnowStep::done;
    if (mode == 1 && last_write >= 0.0) return KnowStep::done;
    if (now - taken > secs * 1000.0) return KnowStep::done;
    if (flag_set) return KnowStep::wait;
    if (last_write >= 0.0 && now - last_write < kKnowGapMs) return KnowStep::wait;
    return KnowStep::write;
}

// ---- C1: whether the take forces her camp (CP) to phase 5 first. key enemy_take_camp: 0 off, 1 only when her CP's phase before
// the take is >= 4 (the walk condition), 2 always. cp_before < 0: unknown (only key 2 forces).
inline bool CampRefreshWanted(int key, int cp_before) noexcept {
    if (key >= 2) return true;
    if (key == 1) return cp_before >= 4;
    return false;
}

// ---- T1: the game's ticket return (FUN_142e60b70) increments `available` even for a non-holder [O], so it is called only when
// the individual is really among the entry's holders, and once per entry. holders: the entry's holder array (n of them; a count
// outside 1..64 is not trusted). 1 call it, 0 not.
inline constexpr int kMaxHolders = 64;
inline int TicketsToReturn(const std::uint64_t* holders, int n, std::uint64_t indiv) noexcept {
    if (!holders || !indiv || n < 1 || n > kMaxHolders) return 0;
    for (int i = 0; i < n; ++i)
        if (holders[i] == indiv) return 1;
    return 0;
}

// ---- Round 25 e (the experiment stage): R2 and F1, two more single-take levers on her game (hidden runs only).
// R2 (enemy_take_role=1): the taken MULE's camp role (DSMuleGameActor+0x368, read only by the HTN term DsMuleGetMuleRole0 through
// FUN_142afabc0 [O]) is 1 on the MULEs that go to a fixed point first (02029: 40-60 m, then shoots from there; 02030) and 0 / 2 on
// the ones that engage where they stand [O, runs 195150 / 202127]. While she owns it, role 1 reads 0; the old value comes back at
// the give-back. RoleWhileHers: the role to write (-1: none). A value outside 0..7 is not trusted.
inline constexpr int kRolePost = 1, kRoleEngage = 0, kRoleMax = 7;
inline int RoleWhileHers(int key, int role) noexcept {
    if (key != 1 || role < 0 || role > kRoleMax) return -1;
    return role == kRolePost ? kRoleEngage : -1;
}
// F1 (enemy_take_face=1): at the take (the copy still frozen) her MULE is turned in place to face her Sam when she is more than
// 60 deg off its facing and 0.5..30 m away. FaceYaw: true and the yaw (radians, atan2 of the forward's y / x) to turn to.
inline constexpr double kFaceMinOffDeg = 60.0, kFaceMinM = 0.5, kFaceMaxM = 30.0;
inline bool FaceYaw(int key, double yaw, double mx, double my, double sx, double sy, double* out) noexcept {
    if (key != 1 || !out || !std::isfinite(yaw)) return false;
    const double dx = sx - mx, dy = sy - my, d = std::sqrt(dx * dx + dy * dy);
    if (!(d >= kFaceMinM && d <= kFaceMaxM)) return false;
    if (OffDeg(yaw, mx, my, sx, sy) <= kFaceMinOffDeg) return false;
    *out = std::atan2(dy, dx);
    return true;
}
// Round 26 (docs/enemy-sync.md 14.19): her game's test that a take is a single take (the round-25 levers act on those only, on by
// default in real games). TakeAlone: a forced take (OWN reason 7, the host's enemy_own), its camp readable here, no other member of
// that camp hers already, and no other forced take in the same batch of OWN messages (a forced whole-camp take, enemy_own .. 2 or
// R1's "the whole camp is taken instead", arrives as one OWN per member in one batch: none of them is alone).
inline bool TakeAlone(bool forced, bool camp_read, unsigned owned_others, unsigned forced_in_batch) noexcept {
    return forced && camp_read && owned_others == 0 && forced_in_batch <= 1;
}

}  // namespace st::eprobe
