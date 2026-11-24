// Enemy sync P1a (docs/enemy-sync.md 8): the pure rules the host sender and the guest's mirrors run on. No game, no link:
// unit-tested in tests/enemy_rules_tests.cpp (st_enemy_rules_tests).
//
//   Schedule       the SEN1 classes by distance from the guest (10 Hz within the mirror radius, 2 Hz within 1000 m, 0.2 Hz the
//                  rest), oldest first, at most 32 entries a frame and 2 frames a tick
//   PoseSlots      the nearest n (0..8) enemies realized on the host within 120 m of the guest (10 m hysteresis), each with a
//                  stable slot; 10 Hz for the nearest 3, 5 Hz for the rest
//   SenderClock    the host's tick clock seen from the guest (unwrapped; offset = min(receive - sent))
//   Track          one enemy's SEN1 samples: interpolated 100 ms behind, extrapolated by its velocity for at most 300 ms, a
//                  jump of more than 5 m between two samples a snap, the yaw along the shortest arc
//   Band           what the guest's mirror driver does with an error: nothing, the per-tick set, or a teleport (<= 2 a second)
//   Eligible       whether the guest may freeze its copy (every refusal named)
//   Lease          when a frozen mirror is given back (silence, range, unrealized, off), held (a cutscene) or forgotten (a game
//                  sequence took it)
//   CutsceneHold   hold everything through a cutscene, release everything 2 s after it ends
//   KeySet         a lock-free set of entity -> actor id (the frozen mirrors; the host's shooters), read on any thread
//   EnemyShotGate  when an enemy shot is replayed against the mirror's render time
//   SamPolicy      keep / drop for a hit on our Sam: replays by kind, and hits from frozen mirrors
//   FingerprintGate whether both games mirror this world (both on, equal fingerprints, a fresh peer)
//
// P2 (round 17, docs/enemy-sync.md 9): her hits on the host's enemies and their outcomes
//   AttackOk       the attacks a HIT may carry (1..0x18D, never 0x180 AimTarget)
//   HitVerdict     a hit on an entity at Entity OnHit: pass (not a mirror), drop, or drop and capture (hers, hits on)
//   HitDedupe      the guest's 300 ms dedupe of one hit seen twice (the same key, serial and attack; serial 0 never dedupes)
//   ToLocal/ToWorld a point (or a direction) in an enemy's yaw frame and back
//   HostSerial     the serial the host stamps for a guest serial (the same guest serial -> the same host serial)
//   RateLimit      the host's HITs a second per enemy and in all
//   ApplyGate      whether the host applies a HIT (every refusal named, the HITRES result)
//   OutcomeOf      an enemy's defeat from the MULE state (+0xEA0) and the down bits (+0xEA4)
//   MergeOutcome   the guest's table: newer versions win, dead is sticky (only a revive from the host lifts it)
//   DeadRelease    a mirror given back: alive, or (the host's is dead) hidden and asleep here
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "shot_wire.hpp"

namespace st::erules {

inline constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- classes
enum class Class : std::uint8_t { near10 = 0, mid2 = 1, far02 = 2 };
inline constexpr double kMidRadiusM = 1000.0;
inline constexpr double kPeriodMs[3] = {100.0, 500.0, 5000.0};
inline Class ClassOf(double dist_m, double radius_m) noexcept {
    if (!(dist_m >= 0)) return Class::far02;
    return dist_m <= radius_m ? Class::near10 : dist_m <= kMidRadiusM ? Class::mid2 : Class::far02;
}
inline double PeriodOf(Class c) noexcept { return kPeriodMs[static_cast<unsigned>(c) < 3 ? static_cast<unsigned>(c) : 2]; }
// How old a frame may be and still count as fresh for its class (3 periods: 0.3 s / 1.5 s / 15 s).
inline double FreshMs(Class c) noexcept { return 3.0 * PeriodOf(c); }

// ---------------------------------------------------------------- the SEN1 schedule
struct Cand {
    std::uint32_t id = 0;
    double dist = -1;  // from the guest's place (m); < 0 unknown (the far class)
};
class Schedule {
public:
    static constexpr unsigned kMaxIds = 256, kPerFrame = 32, kFramesPerTick = 2, kPerTick = kPerFrame * kFramesPerTick;
    static constexpr double kSlackMs = 10.0;  // a tick's jitter: due a little early rather than a whole tick late
    void Reset() noexcept { n_ = 0; }
    // Picks this tick's entries: the indices into `in`, the 10 Hz class first, then the 2 Hz, then the rest, each oldest first.
    // At most min(cap, kPerTick). Their send time becomes `now`.
    unsigned Pick(const Cand* in, unsigned n, double now, double radius_m, unsigned* out, unsigned cap) noexcept {
        if (cap > kPerTick) cap = kPerTick;
        struct Due {
            unsigned i;
            unsigned cls;
            double last;
        };
        Due due[kMaxIds];
        unsigned nd = 0;
        for (unsigned i = 0; i < n && nd < kMaxIds; ++i) {
            const Class c = ClassOf(in[i].dist, radius_m);
            const double last = LastOf(in[i].id);
            if (now - last >= PeriodOf(c) - kSlackMs) due[nd++] = Due{i, static_cast<unsigned>(c), last};
        }
        std::sort(due, due + nd, [](const Due& a, const Due& b) { return a.cls != b.cls ? a.cls < b.cls : a.last != b.last ? a.last < b.last : a.i < b.i; });
        unsigned k = 0;
        for (unsigned j = 0; j < nd && k < cap; ++j) {
            out[k++] = due[j].i;
            SetLast(in[due[j].i].id, now);
        }
        return k;
    }
    double LastOf(std::uint32_t id) const noexcept {
        for (unsigned i = 0; i < n_; ++i)
            if (e_[i].id == id) return e_[i].last;
        return -1e18;
    }

private:
    struct E {
        std::uint32_t id;
        double last;
    };
    void SetLast(std::uint32_t id, double t) noexcept {
        for (unsigned i = 0; i < n_; ++i)
            if (e_[i].id == id) {
                e_[i].last = t;
                return;
            }
        if (n_ < kMaxIds) {
            e_[n_++] = E{id, t};
            return;
        }
        unsigned o = 0;  // full: the oldest goes
        for (unsigned i = 1; i < n_; ++i)
            if (e_[i].last < e_[o].last) o = i;
        e_[o] = E{id, t};
    }
    E e_[kMaxIds] = {};
    unsigned n_ = 0;
};

// ---------------------------------------------------------------- the pose slots (SEP1, host)
inline constexpr unsigned kPoseSlots = 8;
inline constexpr double kPoseRadiusM = 120.0, kPoseHystM = 10.0;
inline constexpr double kPoseFastMs = 100.0, kPoseSlowMs = 200.0;
struct PoseCand {
    std::uint32_t id = 0;
    double dist = -1;
    bool realized = false;  // realized on the host (its pose can be captured); P2: a dead one too (its corpse keeps being posed)
    bool slow = false;      // P2: a dead or down one (it lies still) is posed in the 5 Hz half whatever its rank, and is not counted
                            // in the live ones' fast ranks
};
class PoseSlots {
public:
    void Reset() noexcept {
        for (unsigned k = 0; k < kPoseSlots; ++k) id_[k] = 0, period_[k] = 0;
    }
    // The nearest `count` (0..8) realized candidates within radius (members keep their slot to radius + hyst and rank hyst
    // nearer), each in a stable slot; the nearest 3 every 100 ms, the others every 200 ms.
    void Update(const PoseCand* c, unsigned n, unsigned count, double radius_m = kPoseRadiusM, double hyst_m = kPoseHystM) noexcept {
        if (count > kPoseSlots) count = kPoseSlots;
        struct R {
            unsigned i;
            double score;
        };
        R r[256];
        unsigned nr = 0;
        for (unsigned i = 0; i < n && nr < 256; ++i) {
            if (!c[i].realized || !c[i].id || !(c[i].dist >= 0)) continue;
            const bool member = SlotOf(c[i].id) >= 0;
            if (c[i].dist > radius_m + (member ? hyst_m : 0.0)) continue;
            r[nr++] = R{i, c[i].dist - (member ? hyst_m : 0.0)};
        }
        std::sort(r, r + nr, [&](const R& a, const R& b) { return a.score != b.score ? a.score < b.score : c[a.i].id < c[b.i].id; });
        if (nr > count) nr = count;
        // free the slots of ids no longer chosen
        for (unsigned k = 0; k < kPoseSlots; ++k) {
            if (!id_[k]) continue;
            bool keep = false;
            for (unsigned j = 0; j < nr && !keep; ++j) keep = c[r[j].i].id == id_[k];
            if (!keep) id_[k] = 0, period_[k] = 0;
        }
        for (unsigned j = 0; j < nr; ++j) {
            const std::uint32_t id = c[r[j].i].id;
            if (SlotOf(id) >= 0) continue;
            for (unsigned k = 0; k < kPoseSlots; ++k)
                if (!id_[k]) {
                    id_[k] = id;
                    break;
                }
        }
        // periods by the real distance among the chosen live ones: the nearest 3 fast (a slow one, dead or down, never fast and
        // never ranked: it does not push a live attacker into the 5 Hz half)
        for (unsigned j = 0; j < nr; ++j) {
            const int k = SlotOf(c[r[j].i].id);
            if (k < 0) continue;
            unsigned rank = 0;
            for (unsigned m = 0; m < nr; ++m)
                if (!c[r[m].i].slow && (c[r[m].i].dist < c[r[j].i].dist || (c[r[m].i].dist == c[r[j].i].dist && m < j))) ++rank;
            period_[k] = rank < 3 && !c[r[j].i].slow ? kPoseFastMs : kPoseSlowMs;
        }
    }
    std::uint32_t Id(unsigned k) const noexcept { return k < kPoseSlots ? id_[k] : 0; }
    double Period(unsigned k) const noexcept { return k < kPoseSlots ? period_[k] : 0; }
    int SlotOf(std::uint32_t id) const noexcept {
        if (!id) return -1;
        for (unsigned k = 0; k < kPoseSlots; ++k)
            if (id_[k] == id) return static_cast<int>(k);
        return -1;
    }
    unsigned Used() const noexcept {
        unsigned u = 0;
        for (unsigned k = 0; k < kPoseSlots; ++k) u += id_[k] != 0;
        return u;
    }

private:
    std::uint32_t id_[kPoseSlots] = {};
    double period_[kPoseSlots] = {};
};

// ---------------------------------------------------------------- the sender's clock (guest)
struct SenderClock {
    bool have = false;
    double offset = 0;         // min(receive - sent): our clock minus the sender's, plus the fastest path
    double newest_sent = -1e18;
    void Reset() noexcept { *this = SenderClock{}; }
    double Unwrap(std::uint32_t sent) const noexcept {
        return newest_sent > -1e17 ? shotw::UnwrapNear(sent, newest_sent) : static_cast<double>(sent);
    }
    void Note(double sent, double rx) noexcept {
        const double o = rx - sent;
        if (!have || o < offset) offset = o;
        have = true;
        if (sent > newest_sent) newest_sent = sent;
    }
    // The sender time a mirror shows now: delay_ms behind the newest path.
    double Render(double now, double delay_ms = 100.0) const noexcept { return now - offset - delay_ms; }
};

// ---------------------------------------------------------------- one enemy's samples (guest)
inline constexpr double kInterpDelayMs = 100.0, kMaxExtrapMs = 300.0, kSnapM = 5.0;
struct Sample {
    double t = 0;  // the sender's unwrapped ms
    double pos[3] = {};
    float yaw = 0;
    float vel[3] = {};
    std::uint8_t state = 0;
};
inline float WrapPi(float a) noexcept {
    double x = std::fmod(static_cast<double>(a) + kPi, 2.0 * kPi);
    if (x < 0) x += 2.0 * kPi;
    return static_cast<float>(x - kPi);
}
inline float YawLerp(float a, float b, double t) noexcept { return WrapPi(static_cast<float>(a + static_cast<double>(WrapPi(b - a)) * t)); }
inline double YawDiffDeg(float a, float b) noexcept { return std::fabs(static_cast<double>(WrapPi(a - b))) * 180.0 / kPi; }
struct Track {
    static constexpr unsigned kN = 8;
    Sample s[kN];
    unsigned n = 0;  // oldest first
    double rx_ms = -1e18;  // our clock at the newest sample
    void Reset() noexcept { n = 0, rx_ms = -1e18; }
    // Keeps the newest kN by sender time; an older-or-equal one is dropped (a duplicate or reordered frame).
    bool Push(const Sample& x, double rx) noexcept {
        if (n && x.t <= s[n - 1].t) return false;
        if (n == kN) {
            for (unsigned i = 1; i < kN; ++i) s[i - 1] = s[i];
            --n;
        }
        s[n++] = x;
        rx_ms = rx;
        return true;
    }
    const Sample* Newest() const noexcept { return n ? &s[n - 1] : nullptr; }
    // The place at sender time t. extrapolated: t was past the newest sample.
    bool At(double t, double (&pos)[3], float* yaw, bool* extrapolated = nullptr) const noexcept {
        if (extrapolated) *extrapolated = false;
        if (!n) return false;
        const Sample& z = s[n - 1];
        if (t >= z.t) {
            double dt = t - z.t;
            if (dt > kMaxExtrapMs) dt = kMaxExtrapMs;
            for (int k = 0; k < 3; ++k) pos[k] = z.pos[k] + static_cast<double>(z.vel[k]) * dt / 1000.0;
            *yaw = z.yaw;
            if (extrapolated) *extrapolated = t > z.t;
            return true;
        }
        if (t <= s[0].t) {
            for (int k = 0; k < 3; ++k) pos[k] = s[0].pos[k];
            *yaw = s[0].yaw;
            return true;
        }
        unsigned i = 0;
        while (i + 1 < n && s[i + 1].t <= t) ++i;
        const Sample& a = s[i];
        const Sample& b = s[i + 1];
        const double dx = b.pos[0] - a.pos[0], dy = b.pos[1] - a.pos[1], dz = b.pos[2] - a.pos[2];
        if (dx * dx + dy * dy + dz * dz > kSnapM * kSnapM) {  // a snap: a's place until b's time
            for (int k = 0; k < 3; ++k) pos[k] = a.pos[k];
            *yaw = a.yaw;
            return true;
        }
        const double f = (t - a.t) / (b.t - a.t);
        for (int k = 0; k < 3; ++k) pos[k] = a.pos[k] + (b.pos[k] - a.pos[k]) * f;
        *yaw = YawLerp(a.yaw, b.yaw, f);
        return true;
    }
};

// ---------------------------------------------------------------- the driver's band
enum class Drive : std::uint8_t { none, set, teleport };
inline constexpr double kBandStillM = 0.05, kBandStillDeg = 3.0, kBandSetM = 2.0, kTeleportGapMs = 500.0;
inline Drive Band(double err_m, double yaw_err_deg, double now, double last_teleport_ms, double teleport_gap_ms = kTeleportGapMs) noexcept {
    if (err_m < kBandStillM && yaw_err_deg < kBandStillDeg) return Drive::none;
    if (err_m <= kBandSetM) return Drive::set;
    return now - last_teleport_ms >= teleport_gap_ms ? Drive::teleport : Drive::set;
}

// ---------------------------------------------------------------- entry (guest)
struct EligibleIn {
    bool fresh = true;             // a fresh frame for it (its class)
    bool realized = true;          // state 2 here, the entity the actor names
    bool dead = false;             // entity +0x88 bit 7
    bool hidden_or_asleep = false; // actor +0x88 & 3
    bool sequence_only = false;    // +0x124 bit 12
    bool held_here = false;        // +0xEA4 bit 14 (a real sequence holds it here)
    bool held_on_host = false;     // SEN1 state bit 7
    bool cutscene = false;
    bool in_vehicle = false;       // its entity's parent chain (+0x70) names a vehicle: seated in a truck (untested: refused)
};
enum class Refusal : std::uint8_t { ok, stale, not_realized, dead, hidden_or_asleep, sequence_only, held_here, held_on_host, cutscene, in_vehicle };
inline Refusal Eligible(const EligibleIn& e) noexcept {
    if (e.cutscene) return Refusal::cutscene;
    if (!e.fresh) return Refusal::stale;
    if (!e.realized) return Refusal::not_realized;
    if (e.dead) return Refusal::dead;
    if (e.hidden_or_asleep) return Refusal::hidden_or_asleep;
    if (e.sequence_only) return Refusal::sequence_only;
    if (e.held_here) return Refusal::held_here;
    if (e.held_on_host) return Refusal::held_on_host;
    if (e.in_vehicle) return Refusal::in_vehicle;
    return Refusal::ok;
}
inline const char* RefusalText(Refusal r) noexcept {
    switch (r) {
        case Refusal::ok: return "ok";
        case Refusal::stale: return "no fresh frame";
        case Refusal::not_realized: return "not realized here";
        case Refusal::dead: return "dead";
        case Refusal::hidden_or_asleep: return "hidden or asleep";
        case Refusal::sequence_only: return "sequence-only";
        case Refusal::held_here: return "a real sequence holds it";
        case Refusal::held_on_host: return "a real sequence holds it on the host";
        case Refusal::cutscene: return "cutscene";
        case Refusal::in_vehicle: return "in a vehicle";
    }
    return "?";
}

// ---------------------------------------------------------------- the lease (guest)
inline constexpr double kSilenceNearMs = 1500.0, kSilenceFarMs = 3000.0, kRangeSlackM = 20.0;
struct LeaseIn {
    bool frozen_by_us = false;
    bool hold = false;          // a cutscene (CutsceneHold says hold): nothing is posted
    bool off = false;           // the world, the link or the switch: give everything back
    bool bit14_now = true;      // +0xEA4 bit 14 still set (cleared: the game took it)
    bool realized_here = true;
    double frame_age_ms = 0;
    Class cls = Class::near10;  // its class by the last frame's distance
    double dist_m = 0;          // from our Sam
    double radius_m = 250.0;
};
enum class LeaseVerdict : std::uint8_t { none, keep, hold, release_off, release_unrealized, release_silence, release_range, forget_taken };
inline LeaseVerdict Lease(const LeaseIn& l) noexcept {
    if (!l.frozen_by_us) return LeaseVerdict::none;  // never touch what we did not freeze
    if (l.hold) return LeaseVerdict::hold;
    if (!l.bit14_now) return LeaseVerdict::forget_taken;
    if (l.off) return LeaseVerdict::release_off;
    if (!l.realized_here) return LeaseVerdict::release_unrealized;
    if (l.frame_age_ms > (l.cls == Class::near10 ? kSilenceNearMs : kSilenceFarMs)) return LeaseVerdict::release_silence;
    if (l.dist_m > l.radius_m + kRangeSlackM) return LeaseVerdict::release_range;
    return LeaseVerdict::keep;
}
inline bool Releases(LeaseVerdict v) noexcept {
    return v == LeaseVerdict::release_off || v == LeaseVerdict::release_unrealized || v == LeaseVerdict::release_silence || v == LeaseVerdict::release_range;
}
inline const char* LeaseText(LeaseVerdict v) noexcept {
    switch (v) {
        case LeaseVerdict::release_off: return "off";
        case LeaseVerdict::release_unrealized: return "unrealized";
        case LeaseVerdict::release_silence: return "silence";
        case LeaseVerdict::release_range: return "range";
        case LeaseVerdict::forget_taken: return "taken";
        case LeaseVerdict::hold: return "hold";
        case LeaseVerdict::keep: return "keep";
        case LeaseVerdict::none: return "none";
    }
    return "?";
}

// A cutscene: hold every mirror (nothing posted, nothing driven) while it plays and for 2 s after; then release everything once.
struct CutsceneHold {
    static constexpr double kAfterMs = 2000.0;
    bool in = false, pending = false;
    double end_ms = 0;
    // 0 normal, 1 hold, 2 release everything now (once, 2 s after the end)
    int Step(bool cutscene, double now) noexcept {
        if (cutscene) {
            in = true, pending = false;
            return 1;
        }
        if (in) in = false, pending = true, end_ms = now;
        if (pending) {
            if (now - end_ms >= kAfterMs) {
                pending = false;
                return 2;
            }
            return 1;
        }
        return 0;
    }
};

// ---------------------------------------------------------------- the lock-free entity set (any thread reads)
// An entity matches a key exactly, or one is the other + 0x20 (an entity and its reference differ by 0x20).
inline bool NearKey(std::uint64_t v, std::uint64_t k) noexcept { return v && k && (v == k || v == k + 0x20 || v + 0x20 == k); }
class KeySet {
public:
    static constexpr unsigned kCap = 64;
    // One writer (the game thread).
    void Publish(const std::uint64_t* keys, const std::uint32_t* vals, unsigned n) noexcept {
        if (n > kCap) n = kCap;
        const std::uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (unsigned i = 0; i < kCap; ++i) {
            key_[i].store(i < n ? keys[i] : 0, std::memory_order_relaxed);
            val_[i].store(i < n && vals ? vals[i] : 0, std::memory_order_relaxed);
        }
        n_.store(n, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);
    }
    void Clear() noexcept { Publish(nullptr, nullptr, 0); }
    // `near20`: the +-0x20 match (NearKey). A read torn by a publish is retried; after 4 tries the last scan answers.
    bool Find(std::uint64_t v, std::uint32_t* val = nullptr, bool near20 = true) const noexcept {
        if (!v) return false;
        bool found = false;
        std::uint32_t got = 0;
        for (int tries = 0; tries < 4; ++tries) {
            const std::uint32_t a = seq_.load(std::memory_order_acquire);
            found = false;
            if (!(a & 1)) {
                for (unsigned i = 0; i < kCap && !found; ++i) {
                    const std::uint64_t k = key_[i].load(std::memory_order_relaxed);
                    if (k && (near20 ? NearKey(v, k) : v == k)) found = true, got = val_[i].load(std::memory_order_relaxed);
                }
                std::atomic_thread_fence(std::memory_order_acquire);
                if (seq_.load(std::memory_order_relaxed) == a) break;
            }
        }
        if (found && val) *val = got;
        return found;
    }
    unsigned Size() const noexcept { return n_.load(std::memory_order_relaxed); }

private:
    std::atomic<std::uint32_t> seq_{0};
    std::atomic<std::uint64_t> key_[kCap] = {};
    std::atomic<std::uint32_t> val_[kCap] = {};
    std::atomic<unsigned> n_{0};
};

// ---------------------------------------------------------------- the enemy shots' play gate (guest)
// due: the shot's fire time on the host's clock; render: the mirror's render time on the same clock (SEN1's). No clock: at
// once. Past due: fire (late after 500 ms). Over 2 s ahead (a clock jump): at once.
inline shotw::Gate EnemyShotGate(bool have_clock, double render_ms, double due_ms) noexcept { return shotw::PlayGate(have_clock, render_ms, due_ms); }

// ---------------------------------------------------------------- the damage policy on our Sam
// Replays (the "shots" filter decides; kinds: 1 the partner's, 2 an enemy's, 3 a test, 4 the aimed test): the partner's are
// dropped unless shots_friendly_fire; an enemy's are kept when enemy_shots_hurt (the victim decides; a one-shot nohurt test
// drops it); a test's are dropped unless the test asked to keep it. A hit that is not a replay is dropped when its attacker is a
// frozen mirror and the guard is on. A replay is never decided by the mirror rule (its attacker is the bullet system's dummy).
enum class Dmg : std::uint8_t { keep, drop };
struct DamageIn {
    bool replay = false;
    std::uint8_t kind = 0;
    bool friendly_fire = false;    // shots_friendly_fire
    bool enemy_hurt = true;        // enemy_shots_hurt
    bool test_nofilter = false;    // shots_test flag 1
    bool nohurt_once = false;      // enemy_shot_test nohurt
    bool mirror_attacker = false;  // the attacker is a frozen mirror
    bool guard = true;             // enemy_mirror_guard
};
inline Dmg ReplayPolicy(const DamageIn& d) noexcept {
    if (d.kind == 3) return d.test_nofilter ? Dmg::keep : Dmg::drop;
    if (d.kind == 2) return d.enemy_hurt && !d.nohurt_once ? Dmg::keep : Dmg::drop;
    return d.friendly_fire ? Dmg::keep : Dmg::drop;
}
inline Dmg SamPolicy(const DamageIn& d) noexcept {
    if (d.replay) return ReplayPolicy(d);
    return d.mirror_attacker && d.guard ? Dmg::drop : Dmg::keep;
}

// ---------------------------------------------------------------- the fingerprint gate
struct Print {
    std::uint8_t parts = 0;
    std::uint16_t count = 0;
    std::uint32_t ids = 0, guids = 0;
};
enum class FpGate : std::uint8_t { on, not_ours, no_peer, stale, not_theirs, unready, differ };
inline constexpr double kPeerFreshMs = 20000.0;
inline FpGate FingerprintGate(const Print& ours, const Print& theirs, bool have_peer, double peer_age_ms) noexcept {
    if (!(ours.parts & shotw::kPartMirror)) return FpGate::not_ours;
    if (!have_peer) return FpGate::no_peer;
    if (!(peer_age_ms <= kPeerFreshMs)) return FpGate::stale;
    if (!(theirs.parts & shotw::kPartMirror)) return FpGate::not_theirs;
    if (!ours.count || !ours.ids || !ours.guids || !theirs.count || !theirs.ids || !theirs.guids) return FpGate::unready;
    if (ours.count != theirs.count || ours.ids != theirs.ids || ours.guids != theirs.guids) return FpGate::differ;
    return FpGate::on;
}
inline const char* FpGateText(FpGate g) noexcept {
    switch (g) {
        case FpGate::on: return "on";
        case FpGate::not_ours: return "off here";
        case FpGate::no_peer: return "no beacon from the partner";
        case FpGate::stale: return "the partner's beacon is stale";
        case FpGate::not_theirs: return "off on the partner";
        case FpGate::unready: return "fingerprints not ready";
        case FpGate::differ: return "fingerprints differ";
    }
    return "?";
}

// ==================================================================================================================== P2
// ---------------------------------------------------------------- attacks
inline constexpr std::uint16_t kAttackAim = 0x180, kAttackMax = 0x18D;
inline constexpr bool AttackOk(std::uint16_t a) noexcept { return a >= 1 && a <= kAttackMax && a != kAttackAim; }

// ---------------------------------------------------------------- a hit at Entity OnHit (any thread)
// mirror: the receiver's entity (or a parent within 4 levels) is a frozen mirror here; ours: her own (our Sam, his weapon or a
// part of him, our player id in the context, the vehicle she drives); hits_on: both beacons carry the hits part and mirroring is
// on. A mirror's hit is always dropped (the host decides: no damage, stats or telemetry in her world); hers is also captured.
enum class HitV : std::uint8_t { pass, drop, capture };
inline constexpr HitV HitVerdict(bool mirror, bool ours, bool hits_on) noexcept {
    return !mirror ? HitV::pass : ours && hits_on ? HitV::capture : HitV::drop;
}
// The lethal policy (enemy_hits_lethal=0, the default until a lethal run passes): a round whose attack resource does damage
// (+0x24 > 0) is refused at the guest (her own setting) and again at the host (the host's setting decides: ApplyGate `lethal`).
inline constexpr bool LethalRefused(bool lethal, bool lethal_allowed) noexcept { return lethal && !lethal_allowed; }

// ---------------------------------------------------------------- a HIT's values (both sides; review rv-safety 3)
// The ranges the P2-0 probe and the gate saw, with room: the DamageInfo amount 0 (every round), the multiplier 0.11..1 and a20
// 0.4..1 (rifle rounds and her Sam's 0x15E), the context's power (P+0x64 as f32) 12.5..13 (her rifle) and 1 (the test HITs),
// parts 1..10 (0xFFFFFFFF none). Anything outside is refused (`values`): a wrong layout or a bad peer never reaches the game's
// damage path with an instant faint or kill (a huge multiplier) or a part index the MULE does not have.
inline constexpr float kHitAmountMax = 500.0f, kHitMultMax = 4.0f, kHitA20Max = 2.0f, kHitPowerMax = 100.0f;
inline constexpr std::uint32_t kHitPartMax = 32;  // parts below 32 (a MULE's seen 1..10), or 0xFFFFFFFF
inline bool HitValuesOk(float amount, float mult, float a20, std::uint32_t power_bits, std::uint32_t part) noexcept {
    float power = 0;
    static_assert(sizeof(power) == sizeof(power_bits), "f32");
    std::memcpy(&power, &power_bits, sizeof(power));
    if (!(amount >= 0.0f && amount <= kHitAmountMax)) return false;
    if (!(mult >= 0.0f && mult <= kHitMultMax)) return false;
    if (!(a20 >= 0.0f && a20 <= kHitA20Max)) return false;
    if (!(power >= 0.0f && power <= kHitPowerMax)) return false;
    return part == 0xFFFFFFFFu || part < kHitPartMax;
}

// ---------------------------------------------------------------- the guest's dedupe (game thread)
// One hit can reach the capture twice (the job path and the MULE handler, a part and its owner): the same key, serial and
// attack within 300 ms is dropped. A serial of 0 never dedupes (nothing names it).
class HitDedupe {
public:
    static constexpr unsigned kN = 64;
    static constexpr double kWindowMs = 300.0;
    void Reset() noexcept { n_ = 0, head_ = 0; }
    // True: seen within the window (drop it). False: new (remembered).
    bool Seen(std::uint32_t key, std::uint32_t serial, std::uint16_t attack, double now) noexcept {
        if (!serial) return false;
        for (unsigned i = 0; i < n_; ++i) {
            const E& e = e_[i];
            if (e.key == key && e.serial == serial && e.attack == attack && now - e.t <= kWindowMs && now >= e.t) return true;
        }
        e_[head_] = E{key, serial, attack, now};
        head_ = (head_ + 1) % kN;
        if (n_ < kN) ++n_;
        return false;
    }

private:
    struct E {
        std::uint32_t key, serial;
        std::uint16_t attack;
        double t;
    };
    E e_[kN] = {};
    unsigned n_ = 0, head_ = 0;
};

// ---------------------------------------------------------------- an enemy's yaw frame (the yaw: atan2 of the forward row)
// local = Rz(-yaw)(p - root); world = root + Rz(yaw) local. A direction the same without the root.
inline void DirToLocal(double yaw, const double (&d)[3], double (&out)[3]) noexcept {
    const double c = std::cos(yaw), s = std::sin(yaw);
    const double x = d[0], y = d[1];
    out[0] = c * x + s * y, out[1] = -s * x + c * y, out[2] = d[2];
}
inline void DirToWorld(double yaw, const double (&d)[3], double (&out)[3]) noexcept {
    const double c = std::cos(yaw), s = std::sin(yaw);
    const double x = d[0], y = d[1];
    out[0] = c * x - s * y, out[1] = s * x + c * y, out[2] = d[2];
}
inline void ToLocal(const double (&root)[3], double yaw, const double (&p)[3], double (&out)[3]) noexcept {
    const double d[3] = {p[0] - root[0], p[1] - root[1], p[2] - root[2]};
    DirToLocal(yaw, d, out);
}
inline void ToWorld(const double (&root)[3], double yaw, const double (&local)[3], double (&out)[3]) noexcept {
    DirToWorld(yaw, local, out);
    for (int k = 0; k < 3; ++k) out[k] += root[k];
}
inline constexpr double kHitLocalMaxM = 8.0;   // the guest: a point further from the mirror's root is refused (`far`)
inline constexpr double kApplyMaxM = 3.0;      // the host: the point turned into its world within 3 m of its copy's root

// ---------------------------------------------------------------- the host's serial
// The MULE's own dedupe ring ignores a serial seen within 4 s: the same guest serial (one bullet hitting twice) maps to the same
// host serial; a guest serial of 0 maps by the HIT's sequence. Never 0, never a game serial's range (they count up from 1).
inline constexpr std::uint32_t HostSerial(std::uint32_t guest_serial, std::uint32_t hit_seq) noexcept {
    return guest_serial ? 0x5C000000u | (guest_serial & 0xFFFFFFu) : 0x5D000000u | (hit_seq & 0xFFFFFFu);
}

// ---------------------------------------------------------------- the host's rate (game thread)
class RateLimit {
public:
    static constexpr unsigned kN = 256;
    static constexpr double kWindowMs = 1000.0;
    void Reset() noexcept { n_ = 0; }
    // True (and counted): within per_key a second for this key and total a second in all.
    bool Allow(std::uint32_t key, double now, unsigned per_key, unsigned total) noexcept {
        unsigned k = 0;
        for (unsigned i = 0; i < n_; ++i)
            if (now - e_[i].t < kWindowMs && now >= e_[i].t) e_[k++] = e_[i];
        n_ = k;
        unsigned mine = 0;
        for (unsigned i = 0; i < n_; ++i) mine += e_[i].key == key ? 1u : 0u;
        if (mine >= per_key || n_ >= total || n_ >= kN) return false;
        e_[n_++] = E{key, now};
        return true;
    }

private:
    struct E {
        std::uint32_t key;
        double t;
    };
    E e_[kN] = {};
    unsigned n_ = 0;
};

// ---------------------------------------------------------------- the host's gate
// The HITRES results (the wire's numbers).
// lethal: a damaging round while the host's enemy_hits_lethal=0; values: a value outside HitValuesOk's ranges.
enum class HitResult : std::uint8_t {
    applied = 0, off = 1, dead = 2, unrealized = 3, world = 4, rate = 5, too_far = 6, fault = 7, receiver = 8, attack = 9, hidden = 10, lethal = 11, values = 12
};
inline constexpr unsigned kHitResults = 13;
inline const char* HitResultText(std::uint8_t r) noexcept {
    static const char* const k[kHitResults] = {"applied", "off", "dead", "unrealized", "world", "rate", "far", "fault", "receiver", "attack", "hidden", "lethal", "values"};
    return r < kHitResults ? k[r] : "?";
}
struct ApplyIn {
    bool host = true, mirroring = true, hits_on = true;
    bool fp_equal = true;          // the HIT's guids fingerprint is ours
    bool attack_ok = true;         // AttackOk
    bool kind_ok = true;           // not a test HIT (kind 4) outside a hidden run
    bool values_ok = true;         // HitValuesOk
    bool lethal = false;           // the host's own registry entry for the attack does damage (+0x24 > 0) or the HIT says lethal
    bool lethal_allowed = true;    // the host's enemy_hits_lethal
    bool found = true;             // the key names an in-use MULE actor here (the same actor id)
    bool realized = true;          // state 2 with an entity
    std::uint32_t state = 1;       // +0xEA0: 3 / 5 dead
    bool hidden_or_asleep = false; // a88 & 3
    bool held = false;             // +0xEA4 bit 14 (a sequence holds it)
    bool rate_ok = true;           // RateLimit
    double point_m = 0;            // the point in the host's world from its copy's root
};
inline HitResult ApplyGate(const ApplyIn& a) noexcept {
    if (!a.host || !a.mirroring || !a.hits_on) return HitResult::off;
    if (!a.fp_equal) return HitResult::world;
    if (!a.attack_ok || !a.kind_ok) return HitResult::attack;
    if (!a.values_ok) return HitResult::values;
    if (LethalRefused(a.lethal, a.lethal_allowed)) return HitResult::lethal;
    if (!a.found || !a.realized) return HitResult::unrealized;
    if (a.state == 3 || a.state == 5) return HitResult::dead;
    if (a.hidden_or_asleep || a.held) return HitResult::hidden;
    if (!a.rate_ok) return HitResult::rate;
    if (!(a.point_m <= kApplyMaxM)) return HitResult::too_far;
    return HitResult::applied;
}

// ---------------------------------------------------------------- outcomes
// The wire's defeat (OUTCOME, SEN1 state bits 2-4): 0 none, 1 faint, 2 sleep, 3 restrained, 4 dead, 5 another down kind.
enum class Defeat : std::uint8_t { none = 0, faint = 1, sleep = 2, restrained = 3, dead = 4, other = 5 };
inline constexpr unsigned kDefeats = 6;
inline const char* DefeatText(std::uint8_t d) noexcept {
    static const char* const k[kDefeats] = {"none", "faint", "sleep", "restrained", "dead", "down"};
    return d < kDefeats ? k[d] : "?";
}
inline constexpr std::uint32_t kDownFaint = 0x80000, kDownSleep = 0x100000, kDownRestrained = 0x200000, kDownOther = 0x1000000;
// state 3 / 5 dead; state 4 down: by bit 19 / 20 / 21, else another down kind; anything else none. Down is not terminal (a
// fainted MULE gets up: the host reads it back to none).
inline constexpr Defeat OutcomeOf(std::uint32_t state, std::uint32_t ea4) noexcept {
    return state == 3 || state == 5 ? Defeat::dead
           : state == 4             ? ((ea4 & kDownFaint) ? Defeat::faint : (ea4 & kDownSleep) ? Defeat::sleep : (ea4 & kDownRestrained) ? Defeat::restrained : Defeat::other)
                                    : Defeat::none;
}
inline constexpr std::uint8_t DownBits(std::uint32_t ea4) noexcept { return static_cast<std::uint8_t>((ea4 >> 19) & 0x3Fu); }
// The OUTCOME causes: 0 the host's world, 1 a guest HIT (applied to it in the last 3 s), 2 a resync, 3 a revive (dead -> alive:
// the game brought it back, the only way out of dead).
enum class Cause : std::uint8_t { world = 0, guest = 1, resync = 2, revive = 3 };
inline const char* CauseText(std::uint8_t c) noexcept {
    static const char* const k[4] = {"host", "guest", "resync", "revive"};
    return c < 4 ? k[c] : "?";
}
inline constexpr bool VerNewer(std::uint16_t a, std::uint16_t b) noexcept { return static_cast<std::int16_t>(static_cast<std::uint16_t>(a - b)) > 0; }
struct OutcomeRec {
    bool have = false;
    std::uint8_t defeat = 0;
    std::uint16_t ver = 0;
};
// The guest's merge: a newer version wins; dead stays dead unless the host says it was revived. True: the record changed.
inline bool MergeOutcome(OutcomeRec& cur, std::uint8_t defeat, std::uint16_t ver, std::uint8_t cause) noexcept {
    if (defeat >= kDefeats) return false;
    if (cur.have && !VerNewer(ver, cur.ver)) return false;
    if (cur.have && cur.defeat == static_cast<std::uint8_t>(Defeat::dead) && defeat != cur.defeat && cause != static_cast<std::uint8_t>(Cause::revive)) return false;
    const bool changed = !cur.have || cur.defeat != defeat;
    cur.have = true, cur.defeat = defeat, cur.ver = ver;
    return changed;
}

// ---------------------------------------------------------------- the dead release (guest)
// A mirror whose host copy is dead is never given back alive (it would stand up in her world): after Occupied{0} / Required{0}
// it is hidden and put to sleep (Eligible then refuses it: hidden or asleep). Down or alive: given back as before.
enum class ReleaseAs : std::uint8_t { alive, hide_sleep };
inline constexpr ReleaseAs DeadRelease(std::uint8_t defeat) noexcept {
    return defeat == static_cast<std::uint8_t>(Defeat::dead) ? ReleaseAs::hide_sleep : ReleaseAs::alive;
}

// ==================================================================================================================== P1b
// Ownership by proximity (round 17, docs/enemy-sync.md 10). The host decides everything (HostDecide at 2 Hz); the guest only
// answers its OWN messages (GuestOnOwn) and reports through SEN1. At most one THINKING copy (unfrozen and realized) per enemy
// across the two worlds: the old owner freezes first, the new one unfreezes after.
inline constexpr double kTakeM = 40.0, kTakeMarginM = 15.0, kTakeHostMinM = 25.0, kBackFarM = 60.0, kBackHostNearM = 20.0;
inline constexpr double kHoldMs = 10000.0, kOwnSilenceMs = 3000.0, kAckTimeoutMs = 3000.0, kRefuseHoldMs = 10000.0;
inline constexpr unsigned kMaxGuestOwned = 16, kMaxChangesPerSec = 4;
inline constexpr double kOwnRangeSlackM = 20.0;  // forced back beyond enemy_mirror_radius + 20
inline constexpr std::uint8_t kPhaseUnknown = 0xFF, kPhaseCaution = 3, kPhaseMax = 5;
// EDSMulePhase (the RTTI enum 0x144C0E818): 0 Normal, 1 Return, 2 PreCaution, 3 Caution, 4 Evasion, 5 Alert.
inline const char* PhaseText(std::uint8_t p) noexcept {
    static const char* const k[6] = {"normal", "return", "precaution", "caution", "evasion", "alert"};
    return p <= kPhaseMax ? k[p] : "unknown";
}
enum class Owner : std::uint8_t { host = 1, guest = 2 };
// The wire's reason byte (SCB1 OWN).
enum class OwnReason : std::uint8_t {
    none = 0, proximity = 1, far_off = 2, host_near = 3, silence = 4, refused = 5, ack = 6, forced = 7, unrealized = 8, range = 9, ack_timeout = 10,
    outcome = 11, reset = 12
};
inline constexpr std::uint8_t kOwnReasonMax = 12;
inline const char* OwnReasonText(OwnReason r) noexcept {
    static const char* const k[13] = {"none", "proximity", "far", "host near", "silence", "refused", "ack", "forced", "unrealized on the guest", "range",
                                      "ack timeout", "outcome", "reset"};
    const unsigned i = static_cast<unsigned>(r);
    return i <= kOwnReasonMax ? k[i] : "?";
}
// The guest's refusal codes (OWNACK).
enum class OwnRefuse : std::uint8_t { none = 0, not_realized = 1, held = 2, hidden = 3, dead = 4, cutscene = 5, not_mirror = 6, cap = 7, camp_alerted = 8, fault = 9 };
inline const char* OwnRefuseText(std::uint8_t r) noexcept {
    static const char* const k[10] = {"none", "not realized here", "held by a sequence here", "hidden or asleep here", "dead here", "cutscene",
                                      "not a live mirror here", "owned cap", "her camp is alerted", "the freeze or release faulted"};
    return r < 10 ? k[r] : "?";
}

// One enemy's inputs at the host's 2 Hz walk.
struct OwnIn {
    Owner owner = Owner::host;
    bool pending = false;          // an OWN sent, its OWNACK not in
    double d_g = -1, d_h = -1;     // the enemy (its owner's place) to the guest (the partner body) / to the host's Sam; < 0 unknown
    std::uint8_t phase = 0;        // the owner's copy's phase (+0x110; 0xFF unknown)
    std::uint8_t cp_phase = kPhaseUnknown;  // the host's camp phase (+0x210) for a host-owned enemy (0xFF unknown: ignored)
    bool damaged = false;          // a hit on the owner's copy since the last owner change
    bool terminal = false;         // dead or defeated on the owner (host-owned: no take; guest-owned: no back by proximity)
    bool guest_ok = true;          // her newest frames: no cutscene there, refusals under the cap
    bool host_ok = true;           // the host's copy: not sequence-only, not held, not hidden / asleep, not in a vehicle, no host cutscene
    bool hold_over = true;         // the 10 s hold since the last change (or a refusal) is over
    unsigned guest_owned = 0;      // how many she owns now (pending takes included)
    unsigned changes_this_sec = 0; // owner changes started in the last second
    bool parts_ok = true;          // both beacons carry kPartOwn
    // the forced rules (guest-owned)
    double silence_ms = 0;         // since her newest accepted frame for it (or the OWNACK)
    bool unrealized_guest = false; // her frame: not realized there, or held by a sequence there
    double range_m = 270.0;        // enemy_mirror_radius + 20
    double ack_wait_ms = 0;        // since the OWN while pending
    bool refused = false;          // an OWNACK refused it
    bool forced = false;           // enemy_own <id> host (hidden runs)
};
enum class OwnAct : std::uint8_t { none, take, back, forced_back };
struct OwnVerdict {
    OwnAct act = OwnAct::none;
    OwnReason reason = OwnReason::none;
};
// Calm: Normal (0), Return (1, walking back to its post after a search) or PreCaution (2, suspicious: the camp's push never
// escalates its members with it). Caution and up: alerted (it never changes owner by proximity). Unknown: not calm.
inline constexpr std::uint8_t kPhaseCalmMax = 2;
inline constexpr bool Calm(std::uint8_t phase) noexcept { return phase <= kPhaseCalmMax; }
// A camp (CP +0x210) is alerted from Caution (3) up: the phases its push (0x142B37EB0) escalates its members with. A camp at
// PreCaution (2) with calm members is common (run 20261124-151238: the camp SW of Lake Knot City at 2, its MULEs at 0) [I].
inline constexpr bool CampAlerted(std::uint8_t cp_phase) noexcept { return cp_phase != kPhaseUnknown && cp_phase >= kPhaseCaution; }
// Why a host-owned enemy is not taken now (nullptr: take it). The order is the log's.
inline const char* TakeBlock(const OwnIn& in) noexcept {
    if (in.owner != Owner::host) return "not the host's";
    if (in.pending) return "pending";
    if (!in.parts_ok) return "parts";
    if (!(in.d_g >= 0) || in.d_g > kTakeM) return "the guest beyond 40 m";
    if (!(in.d_h >= 0) || in.d_h < in.d_g + kTakeMarginM) return "the host not 15 m farther";  // the host wins ties
    if (in.d_h <= kTakeHostMinM) return "the host within 25 m";
    if (!Calm(in.phase)) return "its phase";
    if (in.cp_phase != kPhaseUnknown && CampAlerted(in.cp_phase)) return "its camp's phase";
    if (in.damaged) return "damaged";
    if (in.terminal) return "dead or down";
    if (!in.guest_ok) return "the guest (a cutscene, refusals)";
    if (!in.host_ok) return "the host's copy (held, hidden, sequence-only, a vehicle, a cutscene, a save, the guards)";
    if (!in.hold_over) return "the hold";
    if (in.guest_owned >= kMaxGuestOwned) return "her cap";
    if (in.changes_this_sec >= kMaxChangesPerSec) return "the changes a second";
    return nullptr;
}
inline OwnVerdict HostDecide(const OwnIn& in) noexcept {
    if (in.owner == Owner::guest) {
        // forced: ignore phase, damage and the hold
        if (in.refused) return {OwnAct::forced_back, OwnReason::refused};
        if (in.forced) return {OwnAct::forced_back, OwnReason::forced};
        if (in.pending) return in.ack_wait_ms > kAckTimeoutMs ? OwnVerdict{OwnAct::forced_back, OwnReason::ack_timeout} : OwnVerdict{};
        if (in.silence_ms > kOwnSilenceMs) return {OwnAct::forced_back, OwnReason::silence};
        if (in.d_g > in.range_m) return {OwnAct::forced_back, OwnReason::range};
        if (in.unrealized_guest) return {OwnAct::forced_back, OwnReason::unrealized};
        // dead or down on her side (her SEN1 state: kDead or its defeat bits): hers, never handed back by proximity. Back here the
        // host's copy would get up alive at her place while hers lies (review rv-safety 6); only the forced rules above end it.
        if (in.terminal) return {};
        // by proximity: calm (an alerted one stays hers until a forced rule), after the hold
        if (!in.hold_over || !Calm(in.phase)) return {};
        if (in.d_g > kBackFarM) return {OwnAct::back, OwnReason::far_off};
        if (in.d_h >= 0 && in.d_h <= kBackHostNearM) return {OwnAct::back, OwnReason::host_near};
        return {};
    }
    return TakeBlock(in) ? OwnVerdict{} : OwnVerdict{OwnAct::take, OwnReason::proximity};
}
// The place a return gives the host's copy (her OWNACK or her newest frame): only within 10 m of where the host's follower stands
// and at most 3 m below it (review rv-safety 5: a copy of hers that fell through the ground, a stale or bad frame never moves
// the host's enemy, a move its save keeps); else it is released where it stands.
inline constexpr double kBackPlaceMaxM = 10.0, kBackPlaceMaxDropM = 3.0;
inline constexpr bool BackPlaceOk(double err_m, double dz_m) noexcept { return err_m >= 0 && err_m <= kBackPlaceMaxM && dz_m >= -kBackPlaceMaxDropM; }
// The phase a forced return gives the host's copy (RisePhase: escalation only): hers capped at Caution (the Alert path takes the
// LOCAL player as the camp's target); unknown: nothing.
inline constexpr std::uint8_t ReturnPhase(std::uint8_t guest_phase) noexcept {
    return guest_phase > kPhaseMax ? kPhaseUnknown : guest_phase > kPhaseCaution ? kPhaseCaution : guest_phase;
}

// u16 epochs and u32 generations, newer by the serial-number rule (65535 -> 0 is newer).
inline constexpr bool EpochNewer(std::uint16_t a, std::uint16_t b) noexcept { return static_cast<std::int16_t>(static_cast<std::uint16_t>(a - b)) > 0; }
inline constexpr bool GenNewer(std::uint32_t a, std::uint32_t b) noexcept { return static_cast<std::int32_t>(a - b) > 0; }

// ---------------------------------------------------------------- the host's table entry (pure state machine)
struct HostOwn {
    Owner owner = Owner::host;
    std::uint16_t epoch = 0;
    bool pending = false;          // an OWN out, no OWNACK yet
    bool frozen = false;           // the host's copy is frozen by us (guest-owned, or a back waiting for her OWNACK)
    double t_sent = -1e18, t_change = -1e18, hold_until = -1e18;
};
// Take: the host's copy is frozen FIRST (the caller did it; `frozen_ok` false: nothing changes, hold 10 s).
inline bool HostTake(HostOwn& e, double now, bool frozen_ok) noexcept {
    if (e.owner != Owner::host || e.pending) return false;
    if (!frozen_ok) {
        e.hold_until = now + kRefuseHoldMs;
        return false;
    }
    e.epoch = static_cast<std::uint16_t>(e.epoch + 1);
    e.owner = Owner::guest, e.pending = true, e.frozen = true;
    e.t_sent = e.t_change = now, e.hold_until = now + kHoldMs;
    return true;
}
// Back (by proximity or forced): OWN(host); the host's copy stays frozen until her OWNACK(released) or the ack timeout.
inline bool HostBack(HostOwn& e, double now) noexcept {
    if (e.owner != Owner::guest) return false;
    e.epoch = static_cast<std::uint16_t>(e.epoch + 1);
    e.owner = Owner::host, e.pending = true;
    e.t_sent = e.t_change = now, e.hold_until = now + kHoldMs;
    return true;
}
// Her OWNACK. result 0 taken, 1 released, 2 refused. Returns what the host does now.
enum class AckAct : std::uint8_t { ignore, taken, refused_back, release };
inline AckAct HostOnAck(HostOwn& e, std::uint16_t epoch, std::uint8_t result, double now) noexcept {
    if (epoch != e.epoch || !e.pending) return AckAct::ignore;
    if (e.owner == Owner::guest) {
        if (result == 0) {
            e.pending = false;
            return AckAct::taken;
        }
        // refused (or an odd answer): hers never unfroze; the host's again at once, hold 10 s
        e.epoch = static_cast<std::uint16_t>(e.epoch + 1);
        e.owner = Owner::host, e.pending = false, e.frozen = false;
        e.t_change = now, e.hold_until = now + kRefuseHoldMs;
        return AckAct::refused_back;
    }
    // a back: released (or refused: she has nothing; the host's either way)
    e.pending = false, e.frozen = false;
    return AckAct::release;
}
// A back whose OWNACK did not come within the timeout: the host's copy released anyway (from her newest frame).
inline bool HostBackTimedOut(HostOwn& e, double now) noexcept {
    if (e.owner != Owner::host || !e.pending || now - e.t_sent <= kAckTimeoutMs) return false;
    e.pending = false, e.frozen = false;
    return true;
}
// A reset (gate, link, a world, the switch, a fault, a save): everything the host's at once, nothing pending.
inline void HostResetEntry(HostOwn& e, double now) noexcept {
    if (e.owner == Owner::guest || e.pending) e.epoch = static_cast<std::uint16_t>(e.epoch + 1), e.t_change = now;
    e.owner = Owner::host, e.pending = false, e.frozen = false;
}

// ---------------------------------------------------------------- the guest's side of one enemy
struct GuestOwn {
    bool owned = false;            // her copy runs its own AI here
    bool known = false;            // an OWN seen for it (this generation)
    std::uint16_t epoch = 0;
};
enum class GuestAct : std::uint8_t { ignore, take, release, ack_taken, ack_released };
// One OWN for this enemy, this generation (a newer generation resets the whole table first: the caller). take / release: do it
// and answer; ack_*: a repeat (the same epoch), answer the state again; ignore: an older epoch.
inline GuestAct GuestOnOwn(GuestOwn& g, std::uint16_t epoch, Owner owner) noexcept {
    if (g.known && epoch != g.epoch && !EpochNewer(epoch, g.epoch)) return GuestAct::ignore;
    if (g.known && epoch == g.epoch) return g.owned ? GuestAct::ack_taken : GuestAct::ack_released;
    g.known = true, g.epoch = epoch;
    if (owner == Owner::guest) return g.owned ? GuestAct::ack_taken : GuestAct::take;
    return g.owned ? GuestAct::release : GuestAct::ack_released;
}

// ---------------------------------------------------------------- the 8 enemy pose slots, shared by capture and apply
// Apply (mirrors) first: an apply may take a capture's slot when none is free (the caller turns that capture off). Slot 7 is
// the probe's while it runs.
class PoseAlloc {
public:
    enum Use : std::uint8_t { kFree = 0, kApply = 1, kCapture = 2 };
    void Reset() noexcept {
        for (unsigned k = 0; k < kPoseSlots; ++k) use_[k] = kFree;
    }
    void SetProbing(bool on) noexcept { probing_ = on; }
    // A free slot for `u` while fewer than `limit` of that use are taken (-1 none). An apply may take a capture's (`*stolen`).
    int Take(Use u, unsigned limit, bool* stolen = nullptr) noexcept {
        if (stolen) *stolen = false;
        if (u == kFree || Count(u) >= limit) return -1;
        const unsigned n = probing_ ? kPoseSlots - 1 : kPoseSlots;
        for (unsigned k = 0; k < n; ++k)
            if (use_[k] == kFree) return use_[k] = u, static_cast<int>(k);
        if (u == kApply)
            for (unsigned k = n; k-- > 0;)
                if (use_[k] == kCapture) {
                    if (stolen) *stolen = true;
                    return use_[k] = kApply, static_cast<int>(k);
                }
        return -1;
    }
    void Free(int k) noexcept {
        if (k >= 0 && k < static_cast<int>(kPoseSlots)) use_[k] = kFree;
    }
    Use At(int k) const noexcept { return k >= 0 && k < static_cast<int>(kPoseSlots) ? static_cast<Use>(use_[k]) : kFree; }
    unsigned Count(Use u) const noexcept {
        unsigned c = 0;
        for (unsigned k = 0; k < kPoseSlots; ++k) c += use_[k] == u ? 1u : 0u;
        return c;
    }

private:
    std::uint8_t use_[kPoseSlots] = {};
    bool probing_ = false;
};

}  // namespace st::erules
