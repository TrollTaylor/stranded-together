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
//   AttackOk       the attacks a HIT may carry (1..0x18C, never 0x180 AimTarget)
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
    std::uint8_t phase = 0xFF, flags = 0;  // round 23 B: the entry's phase and flags (the mirror diagnostics, kInVehicle)
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
    // M0 "no statues" (round 20, docs/enemy-sync.md 8.10; enemy_host_guard): the newest SEN1 sample's host bits. A copy the host has
    // unloaded (2 km away after a faint) or hidden is never mirrored: it would stand frozen, unposed, at a stale place.
    bool host_realized = true;     // SEN1 state kRealized
    bool host_hidden = false;      // SEN1 state kHidden (hidden or asleep there)
    double entry_err_m = -1;       // the host's place vs ours at entry (-1 unknown)
    double entry_err_max_m = 0;    // enemy_entry_err_max (0 off)
    // M0's quick path (enemy_host_far): the host's own Sam this far from it (his player sync, seen here at once; the host's frames
    // for it stop while his world loads, so the bits above come only after the silence window)
    double host_dist_m = -1;       // the partner body (the host's Sam) from it here (-1 unknown)
    double host_far_m = 0;         // enemy_host_far (0 off)
    // round 23 B (enemy_mirror_vehicle): the owner's SEN1 entry says its copy sits in a vehicle (EntryFlag kInVehicle)
    bool host_in_vehicle = false;
    // round 23 C (enemy_down_hold): the owner's player is down (death / KnotSpace): no new freeze until it is back (critique R7d: the
    // owner's game may have hidden its copy for the Seam)
    bool owner_down = false;
};
enum class Refusal : std::uint8_t {
    ok, stale, not_realized, dead, hidden_or_asleep, sequence_only, held_here, held_on_host, cutscene, in_vehicle, host_unrealized, host_hidden, entry_far,
    host_far, host_in_vehicle, owner_down
};
// M0: the host's Sam beyond enemy_host_far from it (the distance known, the switch on)
inline constexpr bool HostFar(double host_dist_m, double host_far_m) noexcept { return host_far_m > 0 && host_dist_m > host_far_m; }
inline Refusal Eligible(const EligibleIn& e) noexcept {
    if (e.cutscene) return Refusal::cutscene;
    if (e.owner_down) return Refusal::owner_down;
    if (!e.fresh) return Refusal::stale;
    if (!e.realized) return Refusal::not_realized;
    if (e.dead) return Refusal::dead;
    if (e.hidden_or_asleep) return Refusal::hidden_or_asleep;
    if (e.sequence_only) return Refusal::sequence_only;
    if (e.held_here) return Refusal::held_here;
    if (e.held_on_host) return Refusal::held_on_host;
    if (e.in_vehicle) return Refusal::in_vehicle;
    if (e.host_in_vehicle) return Refusal::host_in_vehicle;
    if (!e.host_realized) return Refusal::host_unrealized;
    if (e.host_hidden) return Refusal::host_hidden;
    if (e.entry_err_max_m > 0 && e.entry_err_m > e.entry_err_max_m) return Refusal::entry_far;
    if (HostFar(e.host_dist_m, e.host_far_m)) return Refusal::host_far;
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
        case Refusal::host_unrealized: return "not realized on the host (unloaded there)";
        case Refusal::host_hidden: return "hidden or asleep on the host";
        case Refusal::entry_far: return "the host's place is too far from ours (enemy_entry_err_max)";
        case Refusal::host_far: return "the host's Sam is too far from it (enemy_host_far)";
        case Refusal::host_in_vehicle: return "in a vehicle on the host (its own game runs it until vehicles are synced)";
        case Refusal::owner_down: return "the owner's player is down (death or KnotSpace): no new mirror until it is back";
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
    bool host_guard = false;    // enemy_host_guard: the two host bits below are read (M0)
    bool host_realized = true;  // the newest SEN1 sample: kRealized
    bool host_hidden = false;   // the newest SEN1 sample: kHidden
    double host_dist_m = -1;    // M0 quick path: the host's Sam from it here (-1 unknown)
    double host_far_m = 0;      // enemy_host_far (0 off)
    double frame_age_ms = 0;
    Class cls = Class::near10;  // its class by the last frame's distance
    double dist_m = 0;          // from our Sam
    double radius_m = 250.0;
    // round 23 B (enemy_mirror_vehicle, checked every 1 s): our copy or the owner's copy sits in a vehicle
    bool vehicle_check = false;
    bool in_vehicle_here = false, in_vehicle_host = false;
    // round 25 V1 (enemy_single_hold_vehicle): a MULE of its camp is owned here as a single, so the in-vehicle release waits (the
    // mirror stays frozen at the host's frames, as before round 23) until the single ends
    bool hold_vehicle_single = false;
};
enum class LeaseVerdict : std::uint8_t {
    none, keep, hold, release_off, release_unrealized, release_silence, release_range, forget_taken, release_host_unrealized, release_host_hidden,
    release_host_far, release_vehicle
};
inline LeaseVerdict Lease(const LeaseIn& l) noexcept {
    if (!l.frozen_by_us) return LeaseVerdict::none;  // never touch what we did not freeze
    if (l.hold) return LeaseVerdict::hold;
    if (!l.bit14_now) return LeaseVerdict::forget_taken;
    if (l.off) return LeaseVerdict::release_off;
    if (!l.realized_here) return LeaseVerdict::release_unrealized;
    if (l.frame_age_ms > (l.cls == Class::near10 ? kSilenceNearMs : kSilenceFarMs)) return LeaseVerdict::release_silence;
    // round 23 B: seated in a vehicle (here or on the owner's side): given back to its own game (M0's release, not hidden) until
    // vehicles are synced (design 8, V3)
    if (l.vehicle_check && (l.in_vehicle_here || l.in_vehicle_host) && !l.hold_vehicle_single) return LeaseVerdict::release_vehicle;
    // M0: the host unloaded or hid its copy (the newest frame says so, within the silence window): never a statue
    if (l.host_guard && !l.host_realized) return LeaseVerdict::release_host_unrealized;
    if (l.host_guard && l.host_hidden) return LeaseVerdict::release_host_hidden;
    // M0's quick path: the host's Sam went far (a teleport, a faint's relocation): his copy unloads there, his frames stop
    if (l.host_guard && HostFar(l.host_dist_m, l.host_far_m)) return LeaseVerdict::release_host_far;
    if (l.dist_m > l.radius_m + kRangeSlackM) return LeaseVerdict::release_range;
    return LeaseVerdict::keep;
}
inline bool Releases(LeaseVerdict v) noexcept {
    return v == LeaseVerdict::release_off || v == LeaseVerdict::release_unrealized || v == LeaseVerdict::release_silence || v == LeaseVerdict::release_range ||
           v == LeaseVerdict::release_host_unrealized || v == LeaseVerdict::release_host_hidden || v == LeaseVerdict::release_host_far ||
           v == LeaseVerdict::release_vehicle;
}
// M0: how a mirror is given back. The host hid its copy: with enemy_host_hide on (default off: no run has hidden a mirrored MULE on
// the host yet), hers is hidden here too (never a live MULE beside her that the host's world has put away), and only when the
// release really posted now (`posted`: not held by a cutscene, its weapon flag restored first; review r20 enemy 3). Every other
// release: given back alive (her own game runs it).
inline constexpr bool HideOnRelease(LeaseVerdict v, bool hide_on, bool posted) noexcept {
    return v == LeaseVerdict::release_host_hidden && hide_on && posted;
}
// M0: a copy hidden here because the host's is hidden is shown again once the host's newest frame is fresh (3 s) and says realized
// and not hidden, or once the frames stop (the host no longer sends it: her own game takes it), or on off / reset.
inline constexpr double kHostHiddenStaleMs = 3000.0;
inline constexpr bool KeepHostHidden(bool have_frame, double frame_age_ms, bool host_realized, bool host_hidden) noexcept {
    return have_frame && frame_age_ms <= kHostHiddenStaleMs && host_realized && host_hidden;
}
inline const char* LeaseText(LeaseVerdict v) noexcept {
    switch (v) {
        case LeaseVerdict::release_off: return "off";
        case LeaseVerdict::release_unrealized: return "unrealized";
        case LeaseVerdict::release_silence: return "silence";
        case LeaseVerdict::release_range: return "range";
        case LeaseVerdict::release_host_unrealized: return "host unrealized";
        case LeaseVerdict::release_host_hidden: return "host hidden";
        case LeaseVerdict::release_host_far: return "host far";
        case LeaseVerdict::release_vehicle: return "in a vehicle";
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
// (0x18D is the narrow table's placeholder row, never a real attack: review rv-correctness N5)
inline constexpr std::uint16_t kAttackAim = 0x180, kAttackMax = 0x18C;
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
    applied = 0, off = 1, dead = 2, unrealized = 3, world = 4, rate = 5, too_far = 6, fault = 7, receiver = 8, attack = 9, hidden = 10, lethal = 11, values = 12,
    down = 13  // round 23: a restrain (HIT kind 5) refused: the owner's copy is down already (only ever sent to a partner with kPartRestrain)
};
inline constexpr unsigned kHitResults = 14;
inline const char* HitResultText(std::uint8_t r) noexcept {
    static const char* const k[kHitResults] = {"applied", "off", "dead", "unrealized", "world", "rate", "far", "fault", "receiver", "attack", "hidden", "lethal", "values",
                                               "down already"};
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
    outcome = 11, reset = 12, unloaded = 13, host_nearer = 14  // (13, 14: P1c, round 21)
};
inline constexpr std::uint8_t kOwnReasonMax = 14;
inline const char* OwnReasonText(OwnReason r) noexcept {
    static const char* const k[15] = {"none", "proximity", "far", "host near", "silence", "refused", "ack", "forced", "unrealized on the guest", "range",
                                      "ack timeout", "outcome", "reset", "not loaded on the host", "the host nearer"};
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

// One enemy's inputs at the host's 2 Hz walk. P1b's per-enemy decision (OwnIn, TakeBlock, HostDecide): since round 21 the host
// decides per camp (CampDecide); these stay as the model TestOwnProperty proves the OWN / OWNACK protocol on (test-only).
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
// Take: the host's copy is frozen FIRST (the caller did it; `frozen_ok` false: nothing changes, hold 10 s). (P1c passes its
// camp holds, 15 s.)
inline bool HostTake(HostOwn& e, double now, bool frozen_ok, double hold_ms = kHoldMs, double refuse_hold_ms = kRefuseHoldMs) noexcept {
    if (e.owner != Owner::host || e.pending) return false;
    if (!frozen_ok) {
        e.hold_until = now + refuse_hold_ms;
        return false;
    }
    e.epoch = static_cast<std::uint16_t>(e.epoch + 1);
    e.owner = Owner::guest, e.pending = true, e.frozen = true;
    e.t_sent = e.t_change = now, e.hold_until = now + hold_ms;
    return true;
}
// Back (by proximity or forced): OWN(host); the host's copy stays frozen until her OWNACK(released) or the ack timeout.
inline bool HostBack(HostOwn& e, double now, double hold_ms = kHoldMs) noexcept {
    if (e.owner != Owner::guest) return false;
    e.epoch = static_cast<std::uint16_t>(e.epoch + 1);
    e.owner = Owner::host, e.pending = true;
    e.t_sent = e.t_change = now, e.hold_until = now + hold_ms;
    return true;
}
// Her OWNACK. result 0 taken, 1 released, 2 refused. Returns what the host does now.
enum class AckAct : std::uint8_t { ignore, taken, refused_back, release };
inline AckAct HostOnAck(HostOwn& e, std::uint16_t epoch, std::uint8_t result, double now, double refuse_hold_ms = kRefuseHoldMs) noexcept {
    if (epoch != e.epoch || !e.pending) return AckAct::ignore;
    if (e.owner == Owner::guest) {
        if (result == 0) {
            e.pending = false;
            return AckAct::taken;
        }
        // refused (or an odd answer): hers never unfroze; the host's again at once, hold 10 s
        e.epoch = static_cast<std::uint16_t>(e.epoch + 1);
        e.owner = Owner::host, e.pending = false, e.frozen = false;
        e.t_change = now, e.hold_until = now + refuse_hold_ms;
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

// ==================================================================================================================== P1c
// Ownership per camp (round 21, docs/enemy-sync.md 13). A camp (a DSMuleCPGameActor's members; a MULE in no camp list is a camp of
// one) changes owner as a whole: the host decides per camp at 2 Hz (CampDecide) and hands each member over with P1b's OWN / OWNACK
// (the protocol TestOwnProperty proves). The guest needs no camp knowledge. A loose camp (r 10 m) gets P1b's distances exactly.
inline constexpr double kCampRMinM = 10.0, kCampRMaxM = 120.0;
inline constexpr double kCampEngageMinM = 40.0, kCampEngageAddM = 30.0, kCampEngageMaxM = 150.0;
inline constexpr double kCampMarginMinM = 15.0, kCampMarginK = 0.3, kCampMarginAddM = 10.0, kCampMarginMaxM = 40.0;
inline constexpr double kCampLeftAddM = 20.0;
inline constexpr double kCampHoldMs = 15000.0, kCampRefuseHoldMs = 15000.0, kCampChangeGapMs = 1000.0;
inline constexpr unsigned kCampMaxMembers = 30, kMaxGuestOwnedMembers = 32;
inline constexpr double kMemberGuardMs = 1000.0, kNoEntryHideMs = 3000.0;
inline constexpr double Clamp(double v, double lo, double hi) noexcept { return v < lo ? lo : v > hi ? hi : v; }
// r: the members' spread around their centre (the farthest member), 10..120 m.
inline constexpr double CampR(double spread_m) noexcept { return Clamp(spread_m, kCampRMinM, kCampRMaxM); }
// E: she engages (takes) within this of the nearest standing member: 40..150 m (r + 30).
inline constexpr double CampEngage(double r) noexcept { return Clamp(r + kCampEngageAddM, kCampEngageMinM, kCampEngageMaxM); }
// H: the hysteresis margin (take: the host H farther than she is; back: she H farther than the host): 15..40 m.
inline constexpr double CampMargin(double r) noexcept { return Clamp(kCampMarginK * r + kCampMarginAddM, kCampMarginMinM, kCampMarginMaxM); }
// She left it: beyond E + 20.
inline constexpr double CampLeft(double r) noexcept { return CampEngage(r) + kCampLeftAddM; }

// One camp's inputs at the host's 2 Hz walk.
struct CampIn {
    Owner owner = Owner::host;      // guest: a member is hers (or its take is in flight)
    bool pending = false;           // a member's OWN is out, its OWNACK not in
    double r = kCampRMinM;
    double d_g = -1, d_h = -1;      // the nearest standing member (its owner's place) to her body / to the host's Sam; < 0 unknown
    std::uint8_t phase = 0;         // the highest member phase on the owner's side (kPhaseUnknown: one unknown)
    std::uint8_t cp_phase = kPhaseUnknown;  // the host's CP phase (+0x210); unknown for a loose camp
    bool host_loaded = true;        // a member realized on the host
    unsigned members = 1;           // the camp's members on the host
    unsigned takeable = 1;          // members a take would hand over (standing, readable, not held on the host)
    bool guest_ok = true;           // her frames: no cutscene there, refusals under the cap
    bool host_ok = true;            // the host: no cutscene, no save busy, the guards in, no vehicle
    bool hold_over = true;          // the camp's 15 s hold (since its last change or a refusal) is over
    bool parts_ok = true;           // both beacons carry kPartCamp
    unsigned guest_owned = 0;       // members she owns now (every camp)
    double since_change_ms = 1e9;   // since the newest camp take
    bool other_pending = false;     // another camp's hand-over in flight
    // guest-owned
    double silence_ms = 0;          // since her newest frame for its quietest member (or the OWNACK)
    bool unrealized_guest = false;  // a member not realized there or held by a sequence there (her newest frame)
    double range_m = 270.0;         // enemy_mirror_radius + 20
    double ack_wait_ms = 0;         // since the oldest pending OWN
    bool refused = false;           // a member's OWNACK refused it (a hard refusal: CampRefuseSplits false)
    bool forced = false;            // enemy_own / enemy_camp forced it back, or a game sequence took a host follower
    bool down_guest = false;        // a member down or dead on her side
    bool outcomes_ok = true;        // the outcome setter can carry her outcomes (enemy_owner_outcomes and its checks)
};
// Why a host-owned camp is not taken now (nullptr: take it). The order is the log's.
inline const char* CampTakeBlock(const CampIn& in) noexcept {
    if (in.owner != Owner::host) return "not the host's";
    if (in.pending) return "pending";
    if (!in.parts_ok) return "parts";
    if (in.members > kCampMaxMembers) return "too big";
    if (in.takeable == 0) return "nothing to take (down, held or unread on the host)";
    const double e = CampEngage(in.r), h = CampMargin(in.r);
    if (!(in.d_g >= 0) || in.d_g > e) return "the guest beyond its engage distance";
    if (in.host_loaded) {
        if (!(in.d_h >= 0) || in.d_h < in.d_g + h) return "the host not its margin farther";  // the host wins ties
        if (in.d_h <= kTakeHostMinM) return "the host within 25 m";
        // the phases count only on a loaded camp: an unloaded one's AI does not run on the host (its phase is stale, run
        // 20261125-082959: caution kept 40 s at 2 km after a range back) while her game runs it already (M0 released her mirrors)
        if (!Calm(in.phase)) return "a member's phase";
        if (CampAlerted(in.cp_phase)) return "the camp's phase";
    }
    if (!in.guest_ok) return "the guest (a cutscene, refusals)";
    if (!in.host_ok) return "the host (a cutscene, a save, the guards)";
    if (!in.hold_over) return "the hold";
    if (in.guest_owned + in.takeable > kMaxGuestOwnedMembers) return "her cap";
    if (in.since_change_ms < kCampChangeGapMs || in.other_pending) return "one change at a time";
    return nullptr;
}
// Round 27 M4, take first (enemy_target_pick bit 64; docs/enemy-sync.md 16.7): with the partner stimulus on, the host's MULEs
// would notice her on her approach and alert their camp on the host, which then refuses her take for "the camp's phase" (the
// fight stays on the host's game). While a calm, loaded host camp would go to her on her approach (the host its margin farther,
// beyond 25 m), her stimulus is refused for its members so the camp stays calm until the take. `close_ms`: how long she has been
// within its engage distance without a take (the caller's clock); after kTakeFirstCloseMaxMs a blocked take never hides her for
// good. Her engage distance is deliberately left out: the rule covers her approach.
inline constexpr double kTakeFirstCloseMaxMs = 20000.0;
inline bool CampTakeFirst(const CampIn& in, double close_ms) noexcept {
    if (in.owner != Owner::host || in.pending || !in.parts_ok) return false;
    if (in.members > kCampMaxMembers || in.takeable == 0) return false;
    if (!in.host_loaded || !(in.d_g >= 0) || !(in.d_h >= 0)) return false;
    if (in.d_h < in.d_g + CampMargin(in.r) || in.d_h <= kTakeHostMinM) return false;
    if (!Calm(in.phase) || CampAlerted(in.cp_phase)) return false;
    if (!in.guest_ok || !in.host_ok) return false;
    return close_ms < kTakeFirstCloseMaxMs;
}
inline OwnVerdict CampDecide(const CampIn& in) noexcept {
    if (in.owner == Owner::guest) {
        // forced: ignore phase, the hold and her downs (each member's outcome is carried or left as P1b left it)
        if (in.refused) return {OwnAct::forced_back, OwnReason::refused};
        if (in.forced) return {OwnAct::forced_back, OwnReason::forced};
        if (in.pending) return in.ack_wait_ms > kAckTimeoutMs ? OwnVerdict{OwnAct::forced_back, OwnReason::ack_timeout} : OwnVerdict{};
        if (in.silence_ms > kOwnSilenceMs) return {OwnAct::forced_back, OwnReason::silence};
        if (in.d_g > in.range_m) return {OwnAct::forced_back, OwnReason::range};
        if (in.unrealized_guest && in.host_loaded) return {OwnAct::forced_back, OwnReason::unrealized};
        // a member down on her side and no way to carry it: hers (the host's copy would get up at her place)
        if (in.down_guest && !in.outcomes_ok) return {};
        // by proximity: calm (a camp fighting her stays hers: the host walking in does not take it), after the hold
        if (!in.hold_over || !Calm(in.phase)) return {};
        const double h = CampMargin(in.r);
        if (in.d_g > CampLeft(in.r)) return {OwnAct::back, OwnReason::far_off};
        // the host's distances count only on a camp loaded on the host (as the take's: the unloaded take's hysteresis; review
        // rv-correctness R9: an unloaded camp taken with the host 30 m away would come back at once)
        if (in.host_loaded && in.d_h >= 0 && in.d_h <= kBackHostNearM) return {OwnAct::back, OwnReason::host_near};
        if (in.host_loaded && in.d_h >= 0 && in.d_h <= CampEngage(in.r) && in.d_h + h <= in.d_g) return {OwnAct::back, OwnReason::host_nearer};
        return {};
    }
    if (CampTakeBlock(in)) return {};
    return {OwnAct::take, in.host_loaded ? OwnReason::proximity : OwnReason::unloaded};
}
// Why a guest-owned camp stays hers now (the 10 s log line).
inline const char* CampStaysWhy(const CampIn& in) noexcept {
    if (in.pending) return "pending";
    if (in.down_guest && !in.outcomes_ok) return "a member down on her side (no outcome carry)";
    if (!in.hold_over) return "the hold";
    if (!Calm(in.phase)) return "alerted on her side";
    return "she is nearer";
}
// Her OWNACK refusal for one member splits (that member stays the host's, the camp hers): not mirrored, not realized, dead, held
// or hidden there (one member's state; the host leaves it out of the camp's takes: CampNoTakeMs). A cutscene, her cap, an alerted
// camp or a fault send the whole camp back. (Review rv-gameplay 1: a member she killed came back standing on the host, her
// refusal `dead` sent the camp back every 15 s.)
inline constexpr bool CampRefuseSplits(std::uint8_t refusal) noexcept {
    return refusal == static_cast<std::uint8_t>(OwnRefuse::not_mirror) || refusal == static_cast<std::uint8_t>(OwnRefuse::not_realized) ||
           refusal == static_cast<std::uint8_t>(OwnRefuse::dead) || refusal == static_cast<std::uint8_t>(OwnRefuse::held) ||
           refusal == static_cast<std::uint8_t>(OwnRefuse::hidden);
}
// How long a split member stays out of its camp's takes (ms): dead there, the rest of this world (< 0); any other split 15 s.
inline constexpr double kCampNoTakeWorld = -1.0;
inline constexpr double CampNoTakeMs(std::uint8_t refusal) noexcept {
    return refusal == static_cast<std::uint8_t>(OwnRefuse::dead) ? kCampNoTakeWorld : kCampRefuseHoldMs;
}
// A refusal that counts toward the "refusals under the cap" of CampIn::guest_ok (3 a minute): the hard ones only.
inline constexpr bool CampRefuseCounts(std::uint8_t refusal) noexcept { return !CampRefuseSplits(refusal); }

// The whole camp she owns unrealized (or held by a sequence) there for this long sends it back (spec 8.2). One member alone
// never does: the follower guard hides that one's host copy (review rv-gameplay 3).
inline constexpr double kCampUnrealizedMs = 2000.0;
// Her body missing from the host's view (a vehicle, a respawn, a hiccup): her last place held this long (review rv-gameplay 14).
inline constexpr double kGuestPlaceHoldMs = 3000.0;
// A camp key kept for a member that a camp walk did not list (a partial walk while loading): this long (review rv-gameplay 12).
inline constexpr double kCampKeyKeepMs = 5000.0;

// One member of one camp at the host's 2 Hz walk, as CampAggregate reads it.
struct CampMember {
    bool hers = false;              // its owner is the guest (its take acked or in flight)
    bool pending = false;           // its OWN is out
    bool forced = false;            // enemy_own forced it, or a game sequence took the host's copy
    bool standing = true;           // hers: her newest frame shows no defeat; the host's: not dead or down here
    bool takeable = false;          // the host's: a take would hand it over (standing, read, free here, not on the no-take list)
    bool realized_host = false;     // realized in the host's world
    bool bad_guest = false;         // hers: her fresh frame says not realized there or held by a sequence there
    double bad_for_ms = 0;          // since bad_guest
    double silence_ms = 0;          // hers, not pending: since her newest frame (or the OWNACK)
    double ack_wait_ms = 0;         // pending: since its OWN
    std::uint8_t phase = kPhaseUnknown;  // its owner's side
    double pos[3] = {};             // its owner's place
};
struct CampAgg {
    unsigned hers = 0, pend = 0;
};
inline double Dist3m(const double* a, const double* b) noexcept {
    const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}
// The per-camp part of CampIn from its members (review rv-gameplay 15): owner, pending, r, the distances (the nearest standing
// member; none standing: every member), the phase (her standing members' if she owns one, else the host's standing ones'), loaded,
// takeable, silence, ack wait, unrealized (every settled member of hers bad for kCampUnrealizedMs), down there, forced. `guest` /
// `host` nullptr: unknown (her camp with her place unknown: d_g 1e9, a range back). The rest of CampIn is the caller's.
inline CampAgg CampAggregate(const CampMember* m, std::size_t n, const double* guest, const double* host, CampIn& in) noexcept {
    CampAgg g;
    in.members = static_cast<unsigned>(n);
    in.takeable = 0, in.host_loaded = false, in.silence_ms = 0, in.ack_wait_ms = 0, in.down_guest = false;
    bool forced = false, ph_g_unknown = false, ph_h_unknown = false;
    std::uint8_t ph_g = 0, ph_h = 0;
    unsigned settled = 0, bad = 0;
    double dg = 1e18, dh = 1e18, dg_all = 1e18, dh_all = 1e18, c[3] = {0, 0, 0};
    for (std::size_t i = 0; i < n; ++i) {
        const CampMember& e = m[i];
        in.host_loaded = in.host_loaded || e.realized_host;
        if (e.pending) ++g.pend, in.ack_wait_ms = (std::max)(in.ack_wait_ms, e.ack_wait_ms);
        if (e.hers) {
            ++g.hers;
            in.down_guest = in.down_guest || !e.standing;
            forced = forced || e.forced;
            if (!e.pending) {
                ++settled;
                in.silence_ms = (std::max)(in.silence_ms, e.silence_ms);
                if (e.bad_guest && e.bad_for_ms >= kCampUnrealizedMs) ++bad;
            }
            if (e.standing) {
                if (e.phase == kPhaseUnknown) ph_g_unknown = true;
                else ph_g = (std::max)(ph_g, e.phase);
            }
        } else {
            if (e.takeable) ++in.takeable;
            if (e.standing) {
                if (e.phase == kPhaseUnknown) ph_h_unknown = true;
                else ph_h = (std::max)(ph_h, e.phase);
            }
        }
        const double a_g = guest ? Dist3m(e.pos, guest) : 1e9, a_h = host ? Dist3m(e.pos, host) : 1e18;
        dg_all = (std::min)(dg_all, a_g), dh_all = (std::min)(dh_all, a_h);
        if (e.standing) dg = (std::min)(dg, a_g), dh = (std::min)(dh, a_h);
        c[0] += e.pos[0], c[1] += e.pos[1], c[2] += e.pos[2];
    }
    if (dg > 1e17) dg = dg_all, dh = dh_all;  // none standing: every member counts
    double spread = 0;
    if (n) {
        const double k = 1.0 / static_cast<double>(n);
        c[0] *= k, c[1] *= k, c[2] *= k;
        for (std::size_t i = 0; i < n; ++i) spread = (std::max)(spread, Dist3m(m[i].pos, c));
    }
    in.r = CampR(spread);
    in.owner = g.hers ? Owner::guest : Owner::host;
    in.pending = g.pend > 0;
    in.d_g = !guest ? (g.hers ? 1e9 : -1) : dg;
    in.d_h = host ? dh : -1;
    in.phase = g.hers ? (ph_g_unknown ? kPhaseUnknown : ph_g) : (ph_h_unknown ? kPhaseUnknown : ph_h);
    in.unrealized_guest = settled > 0 && bad == settled;
    in.forced = in.forced || forced;
    return g;
}
// A straggler of a camp she owns (a member that was down, held or in a vehicle on the host at the take): taken on its own when the
// camp is settled and calm, one a tick, under her cap and the change gap (review rv-gameplay 5).
inline bool CampStragglerOk(const CampIn& in) noexcept {
    return in.owner == Owner::guest && !in.pending && in.takeable > 0 && Calm(in.phase) && in.parts_ok && in.guest_ok && in.host_ok &&
           in.guest_owned + 1 <= kMaxGuestOwnedMembers && in.since_change_ms >= kCampChangeGapMs && !in.other_pending;
}
// A carry that waits (a queued hit, the host's game saving, a cutscene holding the copy's return) gives up after this.
inline constexpr double kCarryWaitMs = 15000.0;
// Her outcome carried onto the host's copy at its return: the outcome setter's k (0 faint, 1 sleep, 2 restrained), -1 none.
// Dead is carried as faint this round (a kill on the host is not the setter's k 3 path: untested); an unknown down as faint.
inline constexpr int CarryOutcome(Defeat d, bool setter_ok) noexcept {
    if (!setter_ok) return -1;
    switch (d) {
        case Defeat::none: return -1;
        case Defeat::faint: return 0;
        case Defeat::sleep: return 1;
        case Defeat::restrained: return 2;
        case Defeat::dead: return 0;
        case Defeat::other: return 0;
    }
    return -1;
}
// The host's follower guard (M0 the other way): a frozen host copy of a member of hers is hidden while her newest frame says it is
// not realized there or held by a sequence there for a second, or while no frame of hers came for 3 s; shown again when her frames
// say realized and free.
enum class Guard : std::uint8_t { show, hide };
inline constexpr Guard FollowerGuard(double frame_age_ms, double bad_for_ms) noexcept {
    return frame_age_ms > kNoEntryHideMs || bad_for_ms >= kMemberGuardMs ? Guard::hide : Guard::show;
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

// ==================================================================================================================== round 23
// The tie-up relay (docs/enemy-sync.md 14). The ACTING game (its Sam strands or bolas its frozen copy of an enemy the OTHER game
// owns) reads its own copy: when the game tied it up (+0xEA4 bit 21) or knocked it out (bit 19) it sends a HIT of kind 5 (k 2 / k 0,
// no damage) and holds the copy's own animation; the OWNER applies the outcome setter (0x142AA8000) on its real copy instead of the
// old stun. The fallback (no native read within 1.5 s): the strand's weapon 41 with attack 0x15C at contact.
inline constexpr std::uint16_t kAttackStrand = 0x15C, kAttackBola = 0x102;
inline constexpr std::uint8_t kWeaponStrand = 41;
inline constexpr double kRestrainWatchMs = 1500.0, kRestrainHoldMs = 3000.0, kRestrainBlendMs = 250.0, kRestrainContactM = 2.5;
inline constexpr std::uint8_t kRestrainK = 2, kFaintK = 0;  // the outcome setter's k (HIT b180)
// What one of our captured HITs on a copy starts: strand / bola (held back while the copy is watched) or none (sent as before).
enum class RestrainTrig : std::uint8_t { none = 0, strand = 1, bola = 2 };
inline constexpr RestrainTrig RestrainTrigger(std::uint16_t attack, std::uint8_t weapon) noexcept {
    return attack == kAttackStrand || weapon == kWeaponStrand ? RestrainTrig::strand : attack == kAttackBola ? RestrainTrig::bola : RestrainTrig::none;
}
inline const char* RestrainTrigText(RestrainTrig t) noexcept { return t == RestrainTrig::strand ? "strand" : t == RestrainTrig::bola ? "bola" : "hit"; }
// The fallback's evidence: the strand's attack AND its weapon, the hit at contact (the acting Sam within 2.5 m). A hidden-run test
// (the debug action's synthetic strand) waives the weapon and the distance; it is logged as a test, never as native.
inline constexpr bool RestrainFallback(std::uint16_t attack, std::uint8_t weapon, double her_dist_m, bool test) noexcept {
    return attack == kAttackStrand && (test || (weapon == kWeaponStrand && her_dist_m >= 0.0 && her_dist_m <= kRestrainContactM));
}
inline constexpr bool DownDefeat(std::uint8_t d) noexcept { return d != static_cast<std::uint8_t>(Defeat::none) && d < kDefeats; }
// One watch (the acting side, each tick while open).
struct WatchIn {
    double age_ms = 0;          // since the watch opened
    bool frozen = true;         // still a frozen copy here (the other game owns it)
    std::uint8_t copy = 0;      // Defeat of our copy now (OutcomeOf +0xEA0 / +0xEA4)
    std::uint8_t copy_was = 0;  // ... when the watch opened (only an edge counts)
    std::uint8_t owner = 0;     // the owner's outcome as known here (the guest: its OUTCOME table; the host: her SEN1 defeat bits)
    bool held_hit = false;      // a strand / bola HIT held back
    bool fallback = false;      // RestrainFallback for that HIT
};
enum class WatchDo : std::uint8_t { wait = 0, send_restrain = 1, send_faint = 2, fallback = 3, release = 4, drop = 5 };
inline const char* WatchDoText(WatchDo d) noexcept {
    static const char* const k[6] = {"wait", "restrained (native)", "knocked out (native)", "restrained (fallback)", "no tie-up: the held HIT sent", "closed"};
    return k[static_cast<unsigned>(d) < 6 ? static_cast<unsigned>(d) : 0];
}
inline WatchDo RestrainWatch(const WatchIn& w) noexcept {
    if (!w.frozen) return WatchDo::drop;            // no longer a copy (ours now, or released): nothing for the other game
    if (DownDefeat(w.owner)) return WatchDo::drop;  // down there already: nothing to send (the held stun is dropped too)
    const std::uint8_t r = static_cast<std::uint8_t>(Defeat::restrained), f = static_cast<std::uint8_t>(Defeat::faint);
    if (w.copy == r && w.copy_was != r) return WatchDo::send_restrain;
    if (w.copy == f && w.copy_was != f) return WatchDo::send_faint;
    if (w.age_ms < kRestrainWatchMs) return WatchDo::wait;
    if (w.fallback) return WatchDo::fallback;
    return w.held_hit ? WatchDo::release : WatchDo::drop;
}
// The owner's gate for one HIT of kind 5 (the host for her restrains; the guest for the host's, relayed P1c).
struct RestrainIn {
    bool on = true;               // enemy_restrain_relay here, hits on
    bool peer_part = true;        // the sender's beacon carries kPartRestrain (else it never may send kind 5)
    bool k_ok = true;             // b180 0 / 2
    bool fp_equal = true;         // the HIT's guids fingerprint is ours
    bool owned = true;            // this game owns it (the host: not hers; the guest: hers now)
    bool found = true;            // the key names an in-use MULE actor here (the same actor id)
    bool realized = true;
    std::uint32_t state = 1;      // +0xEA0
    bool hidden_or_asleep = false;
    std::uint8_t defeat_now = 0;  // OutcomeOf now
};
inline HitResult RestrainGate(const RestrainIn& r) noexcept {
    if (!r.on) return HitResult::off;
    if (!r.peer_part || !r.k_ok) return HitResult::attack;
    if (!r.fp_equal) return HitResult::world;
    if (!r.owned) return HitResult::receiver;
    if (!r.found || !r.realized) return HitResult::unrealized;
    if (r.state == 3 || r.state == 5) return HitResult::dead;
    if (r.hidden_or_asleep) return HitResult::hidden;
    if (DownDefeat(r.defeat_now)) return HitResult::down;
    return HitResult::applied;
}
// ecombat::SetOutcome's answer as a HITRES: 1 done / 0 deferred (read back later) applied, -2 down already, else a fault.
inline constexpr HitResult RestrainSetResult(int rc) noexcept { return rc == 1 || rc == 0 ? HitResult::applied : rc == -2 ? HitResult::down : HitResult::fault; }
// The acting side's pose hold on its copy: ends on the owner's down outcome, a refusal, or after 3 s (then the owner's frames are
// blended back in over 250 ms).
enum class HoldEnd : std::uint8_t { none = 0, outcome = 1, refused = 2, timeout = 3 };
inline constexpr HoldEnd HoldStep(double held_ms, bool owner_down, bool refused) noexcept {
    return refused ? HoldEnd::refused : owner_down ? HoldEnd::outcome : held_ms >= kRestrainHoldMs ? HoldEnd::timeout : HoldEnd::none;
}
inline constexpr float BlendBack(double since_ms) noexcept {
    return since_ms <= 0 ? 0.0f : since_ms >= kRestrainBlendMs ? 1.0f : static_cast<float>(since_ms / kRestrainBlendMs);
}
// The replay flush: a shooter's queued shot replays are dropped when its outcome turns down (faint, sleep, restrained, dead, other).
inline constexpr bool FlushOnDefeat(std::uint8_t was, std::uint8_t now) noexcept { return DownDefeat(now) && !DownDefeat(was); }

// ---------------------------------------------------------------- round 23 B: the mirrors (design 7.2; docs/enemy-sync.md 14.8)
// Diagnostics (log only): the 5 s "mirror worst" line while the window's error p95 is above 5 m, one full line the first time an id's
// error is above 20 m, and the 30 s "mirror error" summary.
inline constexpr double kMirrorWorstP95M = 5.0, kMirrorFirstFullM = 20.0, kMirrorSummaryMs = 30000.0;
inline constexpr bool MirrorWorstDue(double p95_m) noexcept { return p95_m > kMirrorWorstP95M; }
inline constexpr bool MirrorFirstFull(double err_m, bool logged_before) noexcept { return !logged_before && err_m > kMirrorFirstFullM; }
// The not-holding rule (enemy_mirror_hold_check): a mirror whose error is still above 10 m after 3 teleports within 2 s does not
// hold its owner's place. It is given back and hidden here (M0's hide: never a statue), retried every 5 s; its shots are still
// replayed from the owner's place.
inline constexpr double kNotHoldErrM = 10.0, kNotHoldWindowMs = 2000.0, kNotHoldRetryMs = 5000.0;
inline constexpr unsigned kNotHoldTeleports = 3;
// One mirror's newest teleports (a ring of 3, the driver's own; the entry's teleport is not counted).
struct TeleportRing {
    double t[kNotHoldTeleports] = {-1e18, -1e18, -1e18};
    unsigned head = 0;
    void Note(double now) noexcept {
        t[head] = now;
        head = (head + 1) % kNotHoldTeleports;
    }
    // teleports at or before `now` and at most `window_ms` old
    unsigned Within(double now, double window_ms) const noexcept {
        unsigned n = 0;
        for (const double x : t)
            if (x > -1e17 && x <= now && now - x <= window_ms) ++n;
        return n;
    }
    void Reset() noexcept {
        for (double& x : t) x = -1e18;
        head = 0;
    }
};
// `err_m`: the error measured this tick before the drive writes (so it is the place after the earlier teleports).
inline constexpr bool NotHolding(bool on, double err_m, unsigned teleports_2s) noexcept {
    return on && err_m > kNotHoldErrM && teleports_2s >= kNotHoldTeleports;
}
// A copy hidden as not holding is shown again (and may be mirrored again) once it has been hidden this long.
inline constexpr bool NotHoldRetry(double hidden_ms) noexcept { return hidden_ms >= kNotHoldRetryMs; }
// The in-vehicle test (enemy_mirror_vehicle), every 1 s on both sides for every lease (not only at entry). Two tests, the first
// that fires is logged: the entity's parent chain (+0x70, up to 4 levels, a vehicle class), else the MULE actor's ride fields:
// +0xE20 the vehicle actor id and +0x3C0 the seat, written by the ride action FUN_142B51E70 (0x142B52075 / 0x142B51F69) and set to
// none by the actor's constructor (0x1FFFFF from 0x144AF1150 / -1). [U] whether getting out clears them (probe P6).
inline constexpr double kVehicleCheckMs = 1000.0;
inline constexpr std::uint32_t kVehicleIdNone = 0x1FFFFF;
// Round 23 D (docs/enemy-sync.md 14.10): a third test, `driver`: the shared vehicles' truck rules (vehicle_truck_rules.hpp) name the
// MULE as a vehicle's driver (unit +0x158, entity +0x8B0) in this game (vehicle_mule_hold).
enum class VehTest : std::uint8_t { none = 0, parent = 1, ride = 2, driver = 3 };
inline const char* VehTestText(VehTest t) noexcept {
    return t == VehTest::parent ? "the parent chain +0x70" : t == VehTest::ride ? "the actor's +0xE20 / +0x3C0" : t == VehTest::driver ? "a truck's driver seat (unit +0x158)" : "none";
}
inline constexpr bool SeatedByRide(std::uint32_t e20, std::int32_t seat) noexcept {
    return (e20 & kVehicleIdNone) != kVehicleIdNone && (e20 & kVehicleIdNone) != 0 && seat >= 0 && seat < 16;
}
inline constexpr VehTest VehicleTest(bool parent_chain, bool ride_ok, std::uint32_t e20, std::int32_t seat, bool truck_driver = false) noexcept {
    return parent_chain ? VehTest::parent : ride_ok && SeatedByRide(e20, seat) ? VehTest::ride : truck_driver ? VehTest::driver : VehTest::none;
}
// Which side's copy is in a vehicle (the release line).
inline const char* VehicleSideText(bool here, bool owner) noexcept {
    return here && owner ? "both copies" : here ? "our copy" : owner ? "the owner's copy" : "neither";
}

// ---------------------------------------------------------------- round 23 C: the other player down (design 7.3 with critique R7,
// B2, B3; docs/enemy-sync.md 14.9; session.ini enemy_down_hold, enemy_down_hold_radius)
// "Down" is death or the Seam only: the game state's GameOver (bit 13) or KnotSpace (bit 9: death, repatriation, limbo). Never a
// faint (bit 14) or a stagger: MULEs knock Sam down to steal his cargo, and that must not change anything. Each game sends its own
// player's code in its SEH1 beacon (byte 22; only to a partner whose parts carry kPartRestrain, a round-23 build: an older decoder
// refuses a non-zero byte 22 and the whole beacon with it). 3 / 4 are the hidden-run fakes (enemy_sync 9).
enum class PlayerDown : std::uint8_t { none = 0, dead = 1, knot = 2, test_down = 3, test_limbo = 4 };
inline constexpr std::uint8_t kPlayerDownMax = 4;
inline constexpr std::uint64_t kGsKnotSpace = 1ull << 9, kGsGameOver = 1ull << 13, kGsFaint = 1ull << 14;
inline constexpr std::uint8_t DownOfState(std::uint64_t flags) noexcept {
    return (flags & kGsGameOver) ? static_cast<std::uint8_t>(PlayerDown::dead) : (flags & kGsKnotSpace) ? static_cast<std::uint8_t>(PlayerDown::knot) : std::uint8_t{0};
}
inline const char* PlayerDownText(std::uint8_t d) noexcept {
    switch (d) {
        case 0: return "up";
        case 1: return "death: game over";
        case 2: return "KnotSpace: death, repatriation or limbo";
        case 3: return "test: down";
        case 4: return "test: limbo";
    }
    return "?";
}
// The sender's latch: down at once (a change of reason too); back only after 1 s of not down (the Seam's own cutscene and load steps
// may flicker the flags).
inline constexpr double kDownClearMs = 1000.0;
struct DownLatch {
    std::uint8_t code = 0;
    double clear_since = -1;
    std::uint8_t Step(std::uint8_t raw, double now) noexcept {
        if (raw) {
            code = raw, clear_since = -1;
        } else if (code) {
            if (clear_since < 0) clear_since = now;
            if (now - clear_since >= kDownClearMs) code = 0, clear_since = -1;
        }
        return code;
    }
    void Reset() noexcept { code = 0, clear_since = -1; }
};
// The receiver's change of the partner's code: 0 none, 1 went down, 2 back up, 3 still down, another reason.
enum class DownChange : std::uint8_t { none = 0, down = 1, back = 2, reason = 3 };
inline constexpr DownChange DownStep(std::uint8_t was, std::uint8_t now) noexcept {
    return was == now ? DownChange::none : !was ? DownChange::down : !now ? DownChange::back : DownChange::reason;
}
// The partner's code this tick (round 23 review): the enemy_sync 9 fake when set (-1 none); else its beacon's byte; with no beacon
// (none yet, or none for 20 s) the code in force is kept: a missing beacon is "unknown", never "back" (the hold ends on a world or
// link change, the role, the switch or a fault instead).
inline constexpr std::uint8_t PeerDownNow(int fake, bool have_peer, std::uint8_t beacon_down, std::uint8_t was) noexcept {
    return fake >= 0 ? static_cast<std::uint8_t>(fake) : have_peer ? beacon_down : was;
}
// The guest's mirrors when sync is off or her Sam is not there while the host's Sam is down (round 23 review): every frozen copy is
// kept frozen as it is (held, or hidden by the hold), never given back alive, instead of the usual "every mirror released". The
// usual release runs once he is back (the hold resumed first), or at a world / link change, the switch or a fault.
inline constexpr bool DownKeepsOff(bool hold_on, std::uint8_t peer_down, bool guest) noexcept { return hold_on && peer_down != 0 && guest; }
// The guest's "not holding" copies (14.8: given back hidden and asleep) are shown and woken again by the 5 s retry, or by sync off,
// only while the host's Sam is not down (review): shown while he is down, the entry would refuse it (owner_down) and leave a live,
// unmirrored MULE beside her. The final unhides (the switch off, a fault, a link change, no guest role) always run.
inline constexpr bool NotHoldUnhideNow(bool final_unhide, bool down_on) noexcept { return final_unhide || !down_on; }
// While either player is down, no ownership changes between the two games (no take, no back, no forced back): the copies of the
// down player's fights stay frozen, held or hidden by DownHold (critique R7c: no OWN hand-over for this case, the reload after a
// death throws it away). Off (enemy_down_hold=0): the old rules.
inline constexpr bool DownFreezesOwnership(bool on, bool ours_down, bool partner_down) noexcept { return on && (ours_down || partner_down); }
// One frozen copy (a guest's mirror of a host-owned enemy, or the host's follower of a guest-owned one) while the player whose game
// owns it is down:
//   near our player (within enemy_down_hold_radius; an unknown distance counts as near):
//     shown with the owner's pose before (posed), or hidden by our own game already (the Seam hid it here: never touched) -> hold:
//       frozen in its last pose, nothing written (no place, no pose, no hide), until that player is back;
//     never posed -> hide (kept frozen, hidden here; shown again when that player is back);
//   far: down (faint, sleep, restrained, dead) -> hide (never given back alive: it would stand up); standing -> usual (M0's
//     release, as before: far from us).
struct DownIn {
    bool posed = false;          // it has had the owner's pose here (a slot now, or one before)
    bool hidden_here = false;    // our own game hides or sleeps it (actor +0x88 & 3, not by us)
    std::uint8_t defeat = 0;     // its last known outcome (erules::Defeat)
    double dist_m = -1;          // our copy's place to our player (-1 unknown)
    double radius_m = 120.0;     // enemy_down_hold_radius
};
enum class DownAct : std::uint8_t { usual = 0, hold = 1, hide = 2 };
inline constexpr DownAct DownHold(const DownIn& d) noexcept {
    const bool is_near = d.dist_m < 0 || d.dist_m <= d.radius_m;  // (not `near`: a Windows macro)
    if (is_near) return d.posed || d.hidden_here ? DownAct::hold : DownAct::hide;
    return DownDefeat(d.defeat) ? DownAct::hide : DownAct::usual;
}
// The lease verdicts the hold overrides (the guest's mirrors): keeping and every release the owner's side or the distance causes. Not
// overridden: our own copy taken by a game sequence or unloaded here, the switch / the link / the world (a reset), a vehicle, our
// own cutscene's hold.
inline constexpr bool DownOverrides(LeaseVerdict v) noexcept {
    return v == LeaseVerdict::keep || v == LeaseVerdict::release_silence || v == LeaseVerdict::release_host_unrealized ||
           v == LeaseVerdict::release_host_hidden || v == LeaseVerdict::release_host_far || v == LeaseVerdict::release_range;
}
inline constexpr DownAct DownLease(bool on, bool partner_down, LeaseVerdict v, const DownIn& d) noexcept {
    return on && partner_down && DownOverrides(v) ? DownHold(d) : DownAct::usual;
}
inline const char* DownActText(DownAct a) noexcept { return a == DownAct::hold ? "held" : a == DownAct::hide ? "hidden" : "usual"; }

// ---------------------------------------------------------------- round 25 R1: the single-take rule (enemy_single_rule, host)
// docs/enemy-sync.md 14.18 (design section 5). Asked when the host would hand ONE MULE to the guest:
//   RULE-W (walk): a single is allowed only when her CP phase is <= 3 (unknown: not counted) AND no other member of that camp was
//     hers earlier in this world (a frozen copy now); otherwise the WHOLE camp switches, or nothing when a camp take is not allowed.
//   RULE-B (behind): it switches only while it already faces her in the old game: her Sam within 60 deg of its facing, <= 25 m, and
//     the host copy alerted; else it stays with its owner (re-checked at the 2 Hz walk).
// Off (rule_on false): always single (the round-24 behaviour).
enum class SingleVerdict : std::uint8_t { single, camp, hold_walk, hold_behind };
inline constexpr double kSingleFaceDeg = 60.0, kSingleFaceM = 25.0;
struct SingleIn {
    bool rule_on = false;
    int cp_phase = -1;          // her CP's phase (-1 unknown)
    int earlier_hers = 0;       // other members of the camp that were hers earlier in this world
    bool camp_allowed = false;  // a whole-camp take is possible now
    double off_deg = -1;        // her Sam off the host copy's facing (-1 unknown)
    double dist_m = -1;         // her Sam from the host copy (-1 unknown)
    bool alerted = false;       // the host copy's phase >= 4
};
inline SingleVerdict SingleRule(const SingleIn& s) noexcept {
    if (!s.rule_on) return SingleVerdict::single;
    if (s.cp_phase > 3 || s.earlier_hers > 0) return s.camp_allowed ? SingleVerdict::camp : SingleVerdict::hold_walk;
    if (s.off_deg < 0 || s.off_deg > kSingleFaceDeg || s.dist_m < 0 || s.dist_m > kSingleFaceM || !s.alerted) return SingleVerdict::hold_behind;
    return SingleVerdict::single;
}
inline const char* SingleVerdictText(SingleVerdict v) noexcept {
    switch (v) {
        case SingleVerdict::single: return "single";
        case SingleVerdict::camp: return "camp";
        case SingleVerdict::hold_walk: return "RULE-W";
        case SingleVerdict::hold_behind: return "RULE-B";
    }
    return "?";
}

}  // namespace st::erules
