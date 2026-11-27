// Round 27 M3 (docs/enemy-sync.md 16): the pure rules of the target pick. No game, no link: unit-tested in
// tests/enemy_target_rules_tests.cpp (st_enemy_target_rules_tests).
//
// TargetPick runs on an AI worker inside the ProcessSensorResults detour, once per call for a registered MULE's knowledge object
// K: from whether this tick's PlayerVisual bucket holds her (the partner) and / or our Sam, and the hits each of them landed on
// it, it keeps or changes the MULE's target and says what to do with the bucket (bring her record or Sam's to record[0], or
// suppress the bucket for this tick so the game does not flip to the other player while the target is only missing for a frame).
// PlanBucket turns that action into a swap index or "count 0". All times are mod milliseconds; 0 means never.
#pragma once

#include <cstdint>
#include <limits>

namespace st::erules {

enum Who : std::uint8_t { kNone = 0, kLocal = 1, kPartner = 2 };
enum Rule : std::uint8_t { rNone = 0, rOnly, rSticky, rHitBy, rNearer, rDefault, rKept, rGone, rForced, rPlace, rSeenEst };
// aSynthPartner (pick bit 32): the bucket holds our Sam but not her, the MULE's target is her and our line-of-sight estimate says
// she is visible: record[0] (Sam's) is rewritten into hers, so the game keeps shooting her.
enum Act : std::uint8_t { aNone = 0, aFrontLocal, aFrontPartner, aSuppress, aSynthPartner };

struct PickCfg {
    std::uint64_t fresh_ms = 1500;  // seen in the bucket this recently: still "visible" (H1)
    std::uint64_t hit_ms = 3000;    // a hit this recent counts
    std::uint64_t min_hold = 1000;  // a hit-by switch only after the target was held this long
    std::uint64_t est_ms = 500;         // pick bit 16: a "clear" line-of-sight estimate this recent counts (PickIn::est_p)
    std::uint64_t est_max_ms = 30000;   // ... but only while her bucket record was seen within this long (no estimate-only target)
    bool est_on = false;                // pick bit 16
    bool synth_on = false;              // pick bit 32 (needs est_on)
};
struct PickState {
    Who cur = kNone;
    Rule rule = rNone;
    std::uint64_t since = 0, seen_p = 0, seen_l = 0;
    float last_dp = std::numeric_limits<float>::infinity(), last_dl = std::numeric_limits<float>::infinity();
};
struct PickIn {
    std::uint64_t now = 0;
    bool in_p = false, in_l = false;  // her / our Sam's record is in this tick's PlayerVisual bucket
    float dp = 0, dl = 0;             // their records' +0x20 distance (the first matching record)
    bool partner_ok = true;           // her stimulus is ours and live (else she counts as gone)
    std::uint64_t hit_p = 0, hit_l = 0;
    bool est_p = false;  // pick bit 16: our own line-of-sight estimate says she is visible (clear, at most est_ms old)
};
struct PickOut {
    PickState st;
    Act act = aNone;
    bool changed = false;
};

inline const char* RuleText(Rule r) noexcept {
    switch (r) {
        case rOnly: return "only visible";
        case rSticky: return "sticky";
        case rHitBy: return "hit by";
        case rNearer: return "nearer";
        case rDefault: return "default Sam";
        case rKept: return "kept";
        case rGone: return "gone";
        case rForced: return "forced";
        case rPlace: return "by place";
        case rSeenEst: return "sticky (line of sight)";
        default: return "none";
    }
}
inline const char* WhoText(Who w) noexcept { return w == kPartner ? "her" : w == kLocal ? "Sam" : "none"; }

inline PickOut TargetPick(PickState s, const PickIn& x, const PickCfg& c) noexcept {
    if (x.in_p) s.seen_p = x.now, s.last_dp = x.dp;
    if (x.in_l) s.seen_l = x.now, s.last_dl = x.dl;
    auto fresh = [&](std::uint64_t t) { return t != 0 && x.now >= t && x.now - t <= c.fresh_ms; };
    auto recent = [&](std::uint64_t t) { return t != 0 && x.now >= t && x.now - t <= c.hit_ms; };
    const bool fp = x.partner_ok && fresh(s.seen_p), fl = fresh(s.seen_l);
    // Pick bit 16: keep-only. The estimate never makes her the target (rules 1, 3, 4, 5 see only the bucket); it only keeps her
    // when she already is (rule 2), and only while her bucket record was seen within est_max_ms.
    const bool ep = c.est_on && x.partner_ok && x.est_p && s.seen_p != 0 && x.now >= s.seen_p && x.now - s.seen_p <= c.est_max_ms;
    Who next = s.cur;
    Rule r = s.rule;
    if (!x.partner_ok && s.cur == kPartner) next = kNone, r = rGone;
    const Who other = next == kPartner ? kLocal : kPartner;
    const bool fcur = (next == kPartner && (fp || ep)) || (next == kLocal && fl);
    const bool fother = other == kPartner ? fp : fl;
    const std::uint64_t hother = other == kPartner ? (x.partner_ok ? x.hit_p : 0) : x.hit_l;
    if (next != kNone && fother && recent(hother) && hother > s.since && x.now - s.since >= c.min_hold) {
        next = other, r = rHitBy;  // 1: hit by the other one, held long enough
    } else if (fcur) {
        r = (next == kPartner && !fp) ? rSeenEst : rSticky;  // 2: the current target is still visible (bucket, or bit 16)
    } else if (fp != fl) {
        next = fp ? kPartner : kLocal, r = rOnly;  // 3: only one visible
    } else if (fp && fl) {  // 4: both visible, no usable current target
        const bool hp = x.partner_ok && recent(x.hit_p), hl = recent(x.hit_l);
        if (hp || hl) next = (hp && (!hl || x.hit_p >= x.hit_l)) ? kPartner : kLocal, r = rHitBy;
        else if (s.last_dp < s.last_dl) next = kPartner, r = rNearer;
        else if (s.last_dl < s.last_dp) next = kLocal, r = rNearer;
        else next = kLocal, r = rDefault;
    } else {  // 5: neither visible
        const std::uint64_t hp = x.partner_ok && recent(x.hit_p) ? x.hit_p : 0, hl = recent(x.hit_l) ? x.hit_l : 0;
        const Who h = (hp || hl) ? ((hp && hp >= hl) ? kPartner : kLocal) : kNone;
        if (h != kNone && h != next) next = h, r = rHitBy;
        else if (next != kNone && r != rGone) r = rKept;
    }
    PickOut o;
    o.changed = next != s.cur;
    if (o.changed) s.since = x.now;
    s.cur = next, s.rule = r;
    const bool tin = (next == kPartner && x.in_p) || (next == kLocal && x.in_l);
    const bool tfresh = (next == kPartner && (fp || ep)) || (next == kLocal && fl);
    const bool oin = (next == kPartner && x.in_l) || (next == kLocal && x.in_p);
    const bool synth = next == kPartner && !x.in_p && x.in_l && ep && c.synth_on;
    o.act = next == kNone ? aNone
            : tin         ? (next == kPartner ? aFrontPartner : aFrontLocal)
            : synth       ? aSynthPartner
            : (tfresh && oin) ? aSuppress
                              : aNone;
    o.st = s;
    return o;
}

// Pick bit 16, the mod's own line-of-sight estimate (SightTick, game thread, 10 Hz): from the MULE's eye to her chest. The cheap
// tests first (no data, range, view cone, pitch, her visibility value), then one world ray (enemy_combat WorldRay: hits, 0 none,
// < 0 an error). Only `clear` counts as seen (PickIn::est_p).
enum class Sight : std::uint8_t { none, clear, blocked, out_range, out_cone, out_pitch, low_vis, no_data, ray_error };
struct SightCfg {
    double max_m = 60;       // horizontal range
    double cone_deg = 60;    // |bearing| from the MULE's facing
    double pitch_deg = 40;   // |pitch| eye -> chest
    float min_vis = 0.05f;   // her visibility value (VisNow) below this: hidden (cloaked, in a cave, ...)
};
struct SightIn {
    bool have = false;  // both positions, the MULE's facing and her visibility were read
    double horiz = 0, bearing_deg = 0, pitch_deg = 0;
    float vis = 1;
    bool cast = false;  // the ray was cast; `ray` is its WorldRay result
    int ray = 0;
};
// none: the cheap tests passed and no ray was cast yet (cast one, then ask again with `cast`)
inline Sight SightVerdict(const SightIn& s, const SightCfg& c) noexcept {
    if (!s.have || !(s.horiz == s.horiz) || !(s.bearing_deg == s.bearing_deg) || !(s.pitch_deg == s.pitch_deg)) return Sight::no_data;
    if (s.horiz > c.max_m) return Sight::out_range;
    if (s.bearing_deg > c.cone_deg || s.bearing_deg < -c.cone_deg) return Sight::out_cone;
    if (s.pitch_deg > c.pitch_deg || s.pitch_deg < -c.pitch_deg) return Sight::out_pitch;
    if (!(s.vis >= c.min_vis)) return Sight::low_vis;
    if (!s.cast) return Sight::none;
    return s.ray < 0 ? Sight::ray_error : s.ray > 0 ? Sight::blocked : Sight::clear;
}
inline const char* SightText(Sight v) noexcept {
    switch (v) {
        case Sight::clear: return "clear";
        case Sight::blocked: return "blocked";
        case Sight::out_range: return "out_range";
        case Sight::out_cone: return "out_cone";
        case Sight::out_pitch: return "out_pitch";
        case Sight::low_vis: return "low_vis";
        case Sight::no_data: return "no_data";
        case Sight::ray_error: return "ray_error";
        default: return "-";
    }
}

struct Plan {
    int swap = -1;       // swap record[0] with record[swap]
    bool count0 = false;
    bool synth = false;  // rewrite record[0] (a local record: our Sam's) into her stimulus (aSynthPartner)
    // (M3f review) aSynthPartner: suppress the bucket (count 0) when the synth is refused or cannot run, as bit 16 alone would
    // (aSuppress); a refused synth never hands the original our Sam's record
    bool fallback0 = false;
};
// who[i]: the owner of the bucket's record i (kPartner her, kLocal anything else: our Sam, a vehicle, ...)
inline Plan PlanBucket(const std::uint8_t* who, int n, Act a) noexcept {
    Plan p;
    if (a == aSuppress) {
        p.count0 = n > 0;
        return p;
    }
    if (a == aSynthPartner) {  // only over a bucket with no record of hers and a local record[0]
        bool any_p = false;
        for (int i = 0; who && i < n; ++i) any_p = any_p || who[i] == kPartner;
        if (n > 0 && who && !any_p) {  // (never suppressed with a record of hers in it)
            p.synth = who[0] == kLocal;
            p.fallback0 = true;  // the count-0 write when the synth did not write (PsrOne), or at once with no synth
            p.count0 = !p.synth;
        }
        return p;
    }
    const Who w = a == aFrontPartner ? kPartner : a == aFrontLocal ? kLocal : kNone;
    if (w == kNone || n <= 0 || !who || who[0] == w) return p;
    for (int i = 1; i < n; ++i)
        if (who[i] == w) {
            p.swap = i;
            break;
        }
    return p;
}

// A forced pick (hidden runs: enemy_tgt 5): the forced player whenever its record is in the bucket, else suppress when the other
// one is there.
inline Act ForcedAct(Who f, bool in_p, bool in_l) noexcept {
    if (f == kPartner) return in_p ? aFrontPartner : in_l ? aSuppress : aNone;
    if (f == kLocal) return in_l ? aFrontLocal : in_p ? aSuppress : aNone;
    return aNone;
}

// The tgt table's slot for K (open addressing over 512 slots).
inline unsigned TgtHash(std::uint64_t k) noexcept { return static_cast<unsigned>(((k >> 4) * 0x9E3779B97F4A7C15ull) >> 55); }

// Redirect bit 2b: the range-check task's compare, as the game does it (0x142f06270: float differences, the squared range;
// 1 success, 2 fail). sym 0 = the [0x147BFB918] symbol (2 when range^2 >= d^2), 1 = the [0x147BFB920] one (2 when d^2 >= range^2).
inline int RangeCheck(int sym, const double (&target)[3], const double (&own)[3], float range) noexcept {
    const float dx = static_cast<float>(target[0] - own[0]), dy = static_cast<float>(target[1] - own[1]), dz = static_cast<float>(target[2] - own[2]);
    const float d2 = dx * dx + dy * dy + dz * dz, r2 = range * range;
    if (sym == 0) return r2 >= d2 ? 2 : 1;
    return d2 >= r2 ? 2 : 1;
}

// enemy_target_fresh_ms: clamped 500..3000 (0 or junk: the default 1500)
inline std::uint64_t FreshMs(long long v) noexcept { return v <= 0 ? 1500u : v < 500 ? 500u : v > 3000 ? 3000u : static_cast<std::uint64_t>(v); }

// H2 (pick bit 4): the classifier's type-0 test on a player's +0x2B0 flags (bit 11 set and bit 31 clear: rejected)
inline bool FlagsReject(std::uint32_t f) noexcept { return (f & 0x800u) != 0 && (f & 0x80000000u) == 0; }

// Redirect bit 32, the shoot actor (round 27 run 2): the body shoot job (0x142EFF4B0) hands the shoot goal's actor id to its weapon
// carrier's SetTargetActor (vt+0xC0, 0x142BC0880), and the carrier's GetTargetPosition turns id 0 into our Sam's live body: the
// muzzle. The detour on SetTargetActor acts only for the job's own call (its return address), the MULE carrier's vtable, actor id
// 0 (the local player) and a registered MULE: `seen` counts it; `redirect` (bit 32 live and the MULE's pick her, her record
// fresh) calls the carrier's SetTargetPos with her chest instead. Anything else: `pass` (the original, nothing counted).
enum class ShootAct : std::uint8_t { pass, seen, redirect };
inline ShootAct ShootPlan(bool from_job, bool carrier, std::uint32_t id, bool registered, bool live, bool want_her) noexcept {
    if (!from_job || !carrier || (id & 0x1FFFFFu) != 0 || !registered) return ShootAct::pass;
    return live && want_her ? ShootAct::redirect : ShootAct::seen;
}
// Round 27 run 3: the shoot job's weapon carrier is the MULE's DSMuleWeaponComponent + 0x50 (vtable 0x143DBA800, whose +0xB8 /
// +0xC0 / +0xD0 wrappers test the component's type and then call the base functions with the same `this`) or a plain
// DSNpcWeaponComponent + 0x50 (vtable 0x143DC7B78). Run 2's filter accepted only the second, so every MULE call was dropped. Any
// other vtable (0: unread) is a carrier class nobody analysed: not counted.
inline bool CarrierVtOk(std::uintptr_t vt, std::uintptr_t mule_vt, std::uintptr_t npc_vt) noexcept {
    return vt != 0 && (vt == mule_vt || vt == npc_vt);
}

// H3, our Sam's hit on a MULE (the OnHit detour): the attacker chain read with each object's vtable checked (in ds.exe) before its
// fields. `bad`: an object of the chain was not one of the game's (a stale reference; counted as onhit_bad, never read further).
// `odd`: the attacker is one of the game's objects but its +0x338 instance is not (a class whose +0x338 is not an instance
// pointer; counted as onhit_odd, information only).
struct AttackerView {
    bool is_sam = false;          // the attacker is our Sam himself
    bool vt_ok = false;           // the attacker's vtable is in the image
    bool owner_sam = false;       // its +0x300 holder is our Sam
    bool has_inst = false;        // its +0x338 instance is set
    bool inst_vt_ok = false;      // that instance's vtable is in the image
    bool inst_owner_sam = false;  // that instance's +0x300 holder is our Sam
};
enum class HitBy : std::uint8_t { other, sam, bad, odd };
inline HitBy AttackerVerdict(const AttackerView& a) noexcept {
    if (a.is_sam) return HitBy::sam;
    if (!a.vt_ok) return HitBy::bad;
    if (a.owner_sam) return HitBy::sam;
    if (!a.has_inst) return HitBy::other;
    if (!a.inst_vt_ok) return HitBy::odd;
    return a.inst_owner_sam ? HitBy::sam : HitBy::other;
}

// The goal-backed redirects (2a / 4 / 16) look the goal up fresh in each call (no cache) and write to it only when its vtable is
// its type's (a byte-checked constant: `want`, 0 when those checks failed). none: no such goal (nothing written); bad: a goal of
// another vtable, or no known vtable (counted as turn_goal_bad, nothing written).
enum class GoalCheck : std::uint8_t { none, ok, bad };
inline GoalCheck CheckGoal(std::uintptr_t goal, bool vt_read, std::uintptr_t vt, std::uintptr_t want) noexcept {
    if (!goal) return GoalCheck::none;
    return vt_read && want && vt == want ? GoalCheck::ok : GoalCheck::bad;
}

}  // namespace st::erules
