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
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

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
    bool realized = false;  // realized on the host (its pose can be captured)
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
        // periods by the real distance among the chosen: the nearest 3 fast
        for (unsigned j = 0; j < nr; ++j) {
            const int k = SlotOf(c[r[j].i].id);
            if (k < 0) continue;
            unsigned rank = 0;
            for (unsigned m = 0; m < nr; ++m)
                if (c[r[m].i].dist < c[r[j].i].dist || (c[r[m].i].dist == c[r[j].i].dist && m < j)) ++rank;
            period_[k] = rank < 3 ? kPoseFastMs : kPoseSlowMs;
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

}  // namespace st::erules
