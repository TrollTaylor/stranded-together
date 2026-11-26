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
enum Rule : std::uint8_t { rNone = 0, rOnly, rSticky, rHitBy, rNearer, rDefault, rKept, rGone, rForced, rPlace };
enum Act : std::uint8_t { aNone = 0, aFrontLocal, aFrontPartner, aSuppress };

struct PickCfg {
    std::uint64_t fresh_ms = 1500;  // seen in the bucket this recently: still "visible" (H1)
    std::uint64_t hit_ms = 3000;    // a hit this recent counts
    std::uint64_t min_hold = 1000;  // a hit-by switch only after the target was held this long
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
    Who next = s.cur;
    Rule r = s.rule;
    if (!x.partner_ok && s.cur == kPartner) next = kNone, r = rGone;
    const Who other = next == kPartner ? kLocal : kPartner;
    const bool fcur = (next == kPartner && fp) || (next == kLocal && fl);
    const bool fother = other == kPartner ? fp : fl;
    const std::uint64_t hother = other == kPartner ? (x.partner_ok ? x.hit_p : 0) : x.hit_l;
    if (next != kNone && fother && recent(hother) && hother > s.since && x.now - s.since >= c.min_hold) {
        next = other, r = rHitBy;  // 1: hit by the other one, held long enough
    } else if (fcur) {
        r = rSticky;  // 2: the current target is still visible
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
    const bool tfresh = (next == kPartner && fp) || (next == kLocal && fl);
    const bool oin = (next == kPartner && x.in_l) || (next == kLocal && x.in_p);
    o.act = next == kNone ? aNone : tin ? (next == kPartner ? aFrontPartner : aFrontLocal) : (tfresh && oin) ? aSuppress : aNone;
    o.st = s;
    return o;
}

struct Plan {
    int swap = -1;  // swap record[0] with record[swap]
    bool count0 = false;
};
// who[i]: the owner of the bucket's record i (kPartner her, kLocal anything else: our Sam, a vehicle, ...)
inline Plan PlanBucket(const std::uint8_t* who, int n, Act a) noexcept {
    Plan p;
    if (a == aSuppress) {
        p.count0 = n > 0;
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
struct AttackerView {
    bool is_sam = false;          // the attacker is our Sam himself
    bool vt_ok = false;           // the attacker's vtable is in the image
    bool owner_sam = false;       // its +0x300 holder is our Sam
    bool has_inst = false;        // its +0x338 instance is set
    bool inst_vt_ok = false;      // that instance's vtable is in the image
    bool inst_owner_sam = false;  // that instance's +0x300 holder is our Sam
};
enum class HitBy : std::uint8_t { other, sam, bad };
inline HitBy AttackerVerdict(const AttackerView& a) noexcept {
    if (a.is_sam) return HitBy::sam;
    if (!a.vt_ok) return HitBy::bad;
    if (a.owner_sam) return HitBy::sam;
    if (!a.has_inst) return HitBy::other;
    if (!a.inst_vt_ok) return HitBy::bad;
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
