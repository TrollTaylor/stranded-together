// Round 27 M3 (native/mod/enemy_target_rules.hpp): the target pick, the bucket plan, the forced pick, the range-check redirect's
// compare and the key clamp. No game.
#include <cstdio>
#include <cstdint>
#include <limits>
#include <string_view>

#include "config_values.hpp"
#include "enemy_target_rules.hpp"

using namespace st;
using namespace st::erules;

namespace {
int g_checks = 0, g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

constexpr std::uint64_t t0 = 10000;
const PickCfg kCfg{};

PickIn In(std::uint64_t now, bool p, bool l, float dp = 10.f, float dl = 10.f) {
    PickIn x;
    x.now = now, x.in_p = p, x.in_l = l, x.dp = dp, x.dl = dl;
    return x;
}
PickState Cur(Who w, std::uint64_t since, std::uint64_t seen_p, std::uint64_t seen_l) {
    PickState s;
    s.cur = w, s.rule = rOnly, s.since = since, s.seen_p = seen_p, s.seen_l = seen_l;
    return s;
}

void TestPick() {
    // T1 only her
    PickOut o = TargetPick(PickState{}, In(t0, true, false), kCfg);
    CHECK(o.st.cur == kPartner && o.st.rule == rOnly && o.act == aFrontPartner && o.changed && o.st.since == t0);
    // T2 sticky: her since t0, both present at t0+100
    o = TargetPick(Cur(kPartner, t0, t0, 0), In(t0 + 100, true, true), kCfg);
    CHECK(o.st.cur == kPartner && o.st.rule == rSticky && o.act == aFrontPartner && !o.changed);
    const std::uint8_t lp[2] = {kLocal, kPartner};
    Plan p = PlanBucket(lp, 2, o.act);
    CHECK(p.swap == 1 && !p.count0);
    // T3 fresh but absent: only Sam in the bucket at t0+500 -> kept, suppress
    o = TargetPick(Cur(kPartner, t0, t0, 0), In(t0 + 500, false, true), kCfg);
    CHECK(o.st.cur == kPartner && o.st.rule == rSticky && o.act == aSuppress && !o.changed);
    const std::uint8_t l1[1] = {kLocal};
    p = PlanBucket(l1, 1, o.act);
    CHECK(p.count0 && p.swap == -1);
    // T4 the switch after 1.5 s of absence
    o = TargetPick(Cur(kPartner, t0, t0, 0), In(t0 + 1600, false, true), kCfg);
    CHECK(o.st.cur == kLocal && o.st.rule == rOnly && o.act == aFrontLocal && o.changed && o.st.since == t0 + 1600);
    // T5 flicker: her every 1000 ms, Sam every tick, 10 s at 25 ms -> stays her, never changed
    {
        PickState s = Cur(kPartner, t0, t0, t0);
        bool ok = true;
        for (std::uint64_t t = t0 + 25; t <= t0 + 10000; t += 25) {
            const PickOut q = TargetPick(s, In(t, (t - t0) % 1000 == 0, true), kCfg);
            if (q.st.cur != kPartner || q.changed) ok = false;
            s = q.st;
        }
        CHECK(ok);
    }
    // T6 both on the first tick: nearer; a recent hit wins
    o = TargetPick(PickState{}, In(t0, true, true, 5.f, 12.f), kCfg);
    CHECK(o.st.cur == kPartner && o.st.rule == rNearer && o.act == aFrontPartner);
    o = TargetPick(PickState{}, In(t0, true, true, 12.f, 5.f), kCfg);
    CHECK(o.st.cur == kLocal && o.st.rule == rNearer && o.act == aFrontLocal);
    {
        PickIn x = In(t0, true, true, 5.f, 12.f);
        x.hit_l = t0 - 100;
        o = TargetPick(PickState{}, x, kCfg);
        CHECK(o.st.cur == kLocal && o.st.rule == rHitBy);
    }
    // T7 the hit switch's minimum hold: Sam since t0, both fresh, her hit at t0+500 -> held; at t0+1000 (hit t0+900) -> her
    {
        PickIn x = In(t0 + 500, true, true);
        x.hit_p = t0 + 500;
        o = TargetPick(Cur(kLocal, t0, t0, t0), x, kCfg);
        CHECK(o.st.cur == kLocal && o.st.rule == rSticky);
        x = In(t0 + 1000, true, true);
        x.hit_p = t0 + 900;
        o = TargetPick(Cur(kLocal, t0, t0, t0), x, kCfg);
        CHECK(o.st.cur == kPartner && o.st.rule == rHitBy && o.changed && o.act == aFrontPartner);
    }
    // T8 neither fresh, a recent hit by her -> her (hit by), nothing to do on an empty bucket
    {
        PickIn x = In(t0 + 5000, false, false);
        x.hit_p = t0 + 4800;
        o = TargetPick(Cur(kLocal, t0, t0, t0), x, kCfg);
        CHECK(o.st.cur == kPartner && o.st.rule == rHitBy && o.act == aNone);
    }
    // T9 her gone: Sam present -> Sam (only); nobody -> none (gone)
    {
        PickIn x = In(t0 + 100, true, true);
        x.partner_ok = false;
        o = TargetPick(Cur(kPartner, t0, t0, t0), x, kCfg);
        CHECK(o.st.cur == kLocal && o.st.rule == rOnly && o.act == aFrontLocal);
        x = In(t0 + 100, true, false);
        x.partner_ok = false;
        o = TargetPick(Cur(kPartner, t0, t0, 0), x, kCfg);
        CHECK(o.st.cur == kNone && o.st.rule == rGone && o.act == aNone && o.changed);
    }
    // T10 never seen
    o = TargetPick(PickState{}, In(t0, false, false), kCfg);
    CHECK(o.st.cur == kNone && o.act == aNone && !o.changed);
    // T11 a tie: Sam (default)
    o = TargetPick(PickState{}, In(t0, true, true, 8.f, 8.f), kCfg);
    CHECK(o.st.cur == kLocal && o.st.rule == rDefault);
    // T12 0 means never, also early in the clock
    {
        PickState s;
        s.cur = kPartner, s.since = 1, s.seen_p = 0;
        o = TargetPick(s, In(500, false, true), kCfg);
        CHECK(o.st.cur == kLocal && o.st.rule == rOnly);
    }
    // T13 a hit older than the target's since does not switch
    {
        PickIn x = In(t0 + 2000, true, true);
        x.hit_p = t0 - 100;
        o = TargetPick(Cur(kLocal, t0, t0 + 1900, t0 + 1900), x, kCfg);
        CHECK(o.st.cur == kLocal && o.st.rule == rSticky);
    }
    // T14 a hit outside the window
    {
        PickIn x = In(t0 + 5000, true, true);
        x.hit_p = t0 + 5000 - 3001;
        o = TargetPick(Cur(kLocal, t0, t0 + 4900, t0 + 4900), x, kCfg);
        CHECK(o.st.cur == kLocal && o.st.rule == rSticky);
        x = In(t0 + 9000, false, false);
        x.hit_p = t0 + 9000 - 3001;
        o = TargetPick(Cur(kLocal, t0, 0, t0), x, kCfg);
        CHECK(o.st.cur == kLocal && o.st.rule == rKept);
    }
    // the fresh window follows the config
    {
        PickCfg c;
        c.fresh_ms = FreshMs(3000);
        o = TargetPick(Cur(kPartner, t0, t0, 0), In(t0 + 2500, false, true), c);
        CHECK(o.st.cur == kPartner && o.act == aSuppress);
    }
}

void TestPlan() {
    const std::uint8_t lp[2] = {kLocal, kPartner}, pl[2] = {kPartner, kLocal}, l1[1] = {kLocal}, llp[3] = {kLocal, kLocal, kPartner};
    Plan p = PlanBucket(lp, 2, aFrontPartner);
    CHECK(p.swap == 1 && !p.count0);
    p = PlanBucket(pl, 2, aFrontPartner);
    CHECK(p.swap == -1 && !p.count0);
    p = PlanBucket(l1, 1, aSuppress);
    CHECK(p.count0 && p.swap == -1);
    p = PlanBucket(llp, 3, aFrontPartner);
    CHECK(p.swap == 2);
    p = PlanBucket(pl, 2, aFrontLocal);
    CHECK(p.swap == 1);
    p = PlanBucket(lp, 0, aSuppress);
    CHECK(!p.count0 && p.swap == -1);
    p = PlanBucket(nullptr, 0, aFrontPartner);
    CHECK(p.swap == -1);
    // arm D reproduced (the pick off: no action): record[0] stays Sam's
    p = PlanBucket(lp, 2, aNone);
    CHECK(p.swap == -1 && !p.count0);
    // her record missing: nothing to bring forward
    p = PlanBucket(l1, 1, aFrontPartner);
    CHECK(p.swap == -1 && !p.count0);
}

void TestForcedAndHelpers() {
    CHECK(ForcedAct(kPartner, true, true) == aFrontPartner);
    CHECK(ForcedAct(kPartner, false, true) == aSuppress);
    CHECK(ForcedAct(kPartner, false, false) == aNone);
    CHECK(ForcedAct(kLocal, true, true) == aFrontLocal);
    CHECK(ForcedAct(kLocal, true, false) == aSuppress);
    CHECK(ForcedAct(kNone, true, true) == aNone);
    // the hash stays in 0..511 and spreads neighbouring K
    bool in = true;
    unsigned same = 0;
    for (std::uint64_t k = 0x1F0000000ull; k < 0x1F0000000ull + 0x4000ull * 64; k += 0x4000ull) {
        if (TgtHash(k) >= 512) in = false;
        if (TgtHash(k) == TgtHash(k + 0x4000ull)) ++same;
    }
    CHECK(in && same < 4);
    // the range check, as 0x142f06270 does it
    const double her[3] = {10, 0, 0}, own[3] = {0, 0, 0};
    CHECK(RangeCheck(0, her, own, 20.f) == 2 && RangeCheck(0, her, own, 5.f) == 1);
    CHECK(RangeCheck(1, her, own, 20.f) == 1 && RangeCheck(1, her, own, 5.f) == 2);
    CHECK(RangeCheck(0, her, own, 10.f) == 2 && RangeCheck(1, her, own, 10.f) == 2);  // equal: both "ae"
    // the key clamp
    CHECK(FreshMs(0) == 1500 && FreshMs(-5) == 1500 && FreshMs(100) == 500 && FreshMs(1500) == 1500 && FreshMs(9000) == 3000);
    // H2's flag test
    CHECK(!FlagsReject(0xF0) && FlagsReject(0x8F0) && !FlagsReject(0x800008F0u) && !FlagsReject(0x80000000u));
    CHECK(std::string_view(RuleText(rSticky)) == "sticky" && std::string_view(WhoText(kPartner)) == "her");
}

// the round-27 keys (cfgval::SetTargetKey): the "not given" placeholders, ranges, bad values kept
void TestKeys() {
    cfgval::TargetKeys k;
    CHECK(k.stim == 0 && k.vis && k.radius == 300.0 && k.pick == 0 && k.redirect == 0 && k.fresh_ms == 1500);
    CHECK(!k.stim_set && !k.pick_set && !k.red_set);
    CHECK(cfgval::SetTargetKey(k, "enemy_partner_stim", "1") && k.stim == 1);
    CHECK(cfgval::SetTargetKey(k, "enemy_partner_stim", "2") && k.stim == 1);
    CHECK(cfgval::SetTargetKey(k, "enemy_partner_stim", "0") && k.stim == 0);
    CHECK(cfgval::SetTargetKey(k, "enemy_partner_vis", "0") && !k.vis);
    CHECK(cfgval::SetTargetKey(k, "enemy_partner_stim_radius", "500") && k.radius == 500.0);
    CHECK(cfgval::SetTargetKey(k, "enemy_partner_stim_radius", "5") && k.radius == 500.0);
    CHECK(cfgval::SetTargetKey(k, "enemy_partner_stim_radius", "x") && k.radius == 500.0);
    CHECK(cfgval::SetTargetKey(k, "enemy_target_pick", "7") && k.pick == 7);
    CHECK(cfgval::SetTargetKey(k, "enemy_target_pick", "128") && k.pick == 7);  // (M4) the top is 127
    CHECK(cfgval::SetTargetKey(k, "enemy_target_pick", "127") && k.pick == 127);
    CHECK(cfgval::SetTargetKey(k, "enemy_target_pick", "71") && k.pick == 71);  // 1 2 4 64 (the default)
    CHECK(cfgval::SetTargetKey(k, "enemy_target_pick", "63") && k.pick == 63);
    CHECK(cfgval::SetTargetKey(k, "enemy_target_pick", "55") && k.pick == 55);  // 1 2 4 16 32 (the scenario's)
    CHECK(cfgval::SetTargetKey(k, "enemy_target_pick", "7") && k.pick == 7);
    CHECK(cfgval::SetTargetKey(k, "enemy_target_pick", "1.5") && k.pick == 7);
    CHECK(cfgval::SetTargetKey(k, "enemy_target_redirect", "39") && k.redirect == 39);  // 1 2 4 and 32 (the shoot actor)
    CHECK(cfgval::SetTargetKey(k, "enemy_target_redirect", "7") && k.redirect == 7);
    CHECK(cfgval::SetTargetKey(k, "enemy_target_redirect", "256") && k.redirect == 7);
    CHECK(cfgval::SetTargetKey(k, "enemy_target_redirect", "-1") && k.redirect == 7);
    CHECK(cfgval::SetTargetKey(k, "enemy_target_fresh_ms", "100") && k.fresh_ms == 500);
    CHECK(cfgval::SetTargetKey(k, "enemy_target_fresh_ms", "9000") && k.fresh_ms == 3000);
    CHECK(cfgval::SetTargetKey(k, "enemy_target_fresh_ms", "2000") && k.fresh_ms == 2000);
    CHECK(!cfgval::SetTargetKey(k, "enemy_partner_aural", "1"));
    CHECK(!cfgval::SetTargetKey(k, "enemy_owner", "1"));
    CHECK(k.stim_set && k.pick_set && k.red_set);
}
// (round 27 M4) the defaults: the keys not given follow enemy_hits (1 / 71 / 35 on, 0 / 0 / 0 off); a given key is kept (0 too)
void TestTargetDefaults() {
    cfgval::TargetKeys k;
    cfgval::TargetKeys d = cfgval::TargetDefaults(true, k, false, false, false);
    CHECK(d.stim == 1 && d.pick == 71 && d.redirect == 35 && d.vis && d.radius == 300.0 && d.fresh_ms == 1500);
    static_assert(cfgval::kTargetStimDefault == 1 && cfgval::kTargetPickDefault == 71 && cfgval::kTargetRedirectDefault == 35);
    d = cfgval::TargetDefaults(false, k, false, false, false);
    CHECK(d.stim == 0 && d.pick == 0 && d.redirect == 0);
    // an explicit 0 with enemy_hits on stays 0
    cfgval::TargetKeys z;
    CHECK(cfgval::SetTargetKey(z, "enemy_partner_stim", "0") && z.stim_set);
    d = cfgval::TargetDefaults(true, z, z.stim_set, z.pick_set, z.red_set);
    CHECK(d.stim == 0 && d.pick == 71 && d.redirect == 35);
    // explicit keys with enemy_hits off are kept
    cfgval::TargetKeys e;
    CHECK(cfgval::SetTargetKey(e, "enemy_target_pick", "55") && cfgval::SetTargetKey(e, "enemy_target_redirect", "39"));
    d = cfgval::TargetDefaults(false, e, e.stim_set, e.pick_set, e.red_set);
    CHECK(d.stim == 0 && d.pick == 55 && d.redirect == 39);
    d = cfgval::TargetDefaults(true, e, e.stim_set, e.pick_set, e.red_set);
    CHECK(d.stim == 1 && d.pick == 55 && d.redirect == 39);
    // a bad value is not "given": the default applies
    cfgval::TargetKeys b;
    CHECK(cfgval::SetTargetKey(b, "enemy_target_pick", "128") && !b.pick_set);
    CHECK(cfgval::SetTargetKey(b, "enemy_partner_stim", "2") && !b.stim_set);
    d = cfgval::TargetDefaults(true, b, b.stim_set, b.pick_set, b.red_set);
    CHECK(d.stim == 1 && d.pick == 71 && d.redirect == 35);
}
// (round 27 M4) the destroy guard's body flags and the raycast self-test's verdict
void TestGuardAndSelfTest() {
    CHECK(BodyFlagsLive(0x32844443ull));                       // a live partner body (bit 29 set, 30 / 31 clear)
    CHECK(!BodyFlagsLive(0x32844443ull | (1ull << 30)));       // queued for removal
    CHECK(!BodyFlagsLive(0x32844443ull | (1ull << 31)));       // being removed now
    CHECK(!BodyFlagsLive(0x32844443ull & ~(1ull << 29)));      // not in the world
    CHECK(!BodyFlagsLive(0) && BodyFlagsLive(1ull << 29));
    // (review) the gone mark on her current body: stale (cleared after kGoneStaleMs) only when nothing backs a removal
    const std::uint64_t live = 0x32844443ull;
    CHECK(GoneMarkStale(false, false, false, true, live, false));                 // a trip on a live body: the agent changed
    CHECK(!GoneMarkStale(true, false, false, true, live, false));                 // op 12's simulate: "12 0" ends it
    CHECK(!GoneMarkStale(false, true, false, true, live, false));                 // the stimulus not removed yet
    CHECK(!GoneMarkStale(false, false, true, true, live, false));                 // the RemoveEntityNow hook saw the body
    CHECK(!GoneMarkStale(false, false, false, false, live, false));               // its flags unread
    CHECK(!GoneMarkStale(false, false, false, true, live | (1ull << 31), false)); // being removed now
    CHECK(!GoneMarkStale(false, false, false, true, live | (1ull << 30), false)); // queued for removal
    CHECK(!GoneMarkStale(false, false, false, true, live, true));                 // the trip saw its vtable change (freed)
    static_assert(kGoneStaleMs >= 500.0 && kGoneStaleMs <= 2000.0, "the stale hold: about 1 s");
    const double root = 352.49;
    // L32 at the root: ok; L47 only: ok (static only); neither: retry; a fault on either: fault
    CHECK(LosSelfTest(1, 352.40, 1, 352.40, 0, root, false) == SelfTest::ok);
    CHECK(LosSelfTest(0, 0.0, 1, 352.10, 0, root, false) == SelfTest::ok_static);
    CHECK(LosSelfTest(0, 0.0, 0, 0.0, 0, root, false) == SelfTest::retry);
    CHECK(LosSelfTest(-1, 0.0, 1, 352.40, 0, root, false) == SelfTest::fault);
    CHECK(LosSelfTest(1, 352.40, -1, 0.0, 0, root, false) == SelfTest::fault);
    // the tolerance: 0.75 m on the vertical ray, 1.5 m beside
    CHECK(LosSelfTest(1, root - 1.2, 0, 0.0, 0, root, false) == SelfTest::retry);
    CHECK(LosSelfTest(1, root - 1.2, 0, 0.0, 0, root, true) == SelfTest::ok);
    CHECK(LosSelfTest(1, root - 1.6, 0, 0.0, 0, root, true) == SelfTest::retry);
    CHECK(LosSelfTest(1, root + 0.7, 0, 0.0, 0, root, false) == SelfTest::ok);
    // a NaN height is a miss
    CHECK(LosSelfTest(1, std::numeric_limits<double>::quiet_NaN(), 0, 0.0, 0, root, false) == SelfTest::retry);
    // L52 (the control) is not gated: its hit passes nothing, its miss fails nothing
    CHECK(LosSelfTest(0, 0.0, 0, 0.0, 1, root, false) == SelfTest::retry);
    CHECK(LosSelfTest(1, 352.40, 0, 0.0, 1, root, false) == SelfTest::ok);
    // the 10-attempt fail is the caller's (retry, then no attempt left): the verdict itself stays retry
    for (int i = 0; i < 10; ++i) CHECK(LosSelfTest(0, 0.0, 0, 0.0, 0, root, i % 2 == 1) == SelfTest::retry);
    CHECK(std::string_view(SelfTestText(SelfTest::ok)) == "ok" && std::string_view(SelfTestText(SelfTest::retry)) == "trying again");
    CHECK(std::string_view(SelfTestText(SelfTest::ok_static)).substr(0, 18) == "ok (static only: t");
}
// redirect bit 32 (the shoot actor), the OnHit attacker verdict and the fresh goal check
void TestShootAndGuards() {
    // the job's call, the carrier, id 0, registered: seen; with bit 32 and her picked and fresh: redirect
    CHECK(ShootPlan(true, true, 0, true, false, false) == ShootAct::seen);
    CHECK(ShootPlan(true, true, 0, true, true, false) == ShootAct::seen);
    CHECK(ShootPlan(true, true, 0, true, false, true) == ShootAct::seen);
    CHECK(ShootPlan(true, true, 0, true, true, true) == ShootAct::redirect);
    CHECK(ShootPlan(true, true, 0xFFE00000u, true, true, true) == ShootAct::redirect);  // the id's top bits are not the actor
    // any other caller, carrier, actor or an unregistered MULE: the original, nothing counted
    CHECK(ShootPlan(false, true, 0, true, true, true) == ShootAct::pass);
    CHECK(ShootPlan(true, false, 0, true, true, true) == ShootAct::pass);
    CHECK(ShootPlan(true, true, 0x2029, true, true, true) == ShootAct::pass);
    CHECK(ShootPlan(true, true, 0x1FFFFF, true, true, true) == ShootAct::pass);
    CHECK(ShootPlan(true, true, 0, false, true, true) == ShootAct::pass);
    // the attacker chain
    AttackerView a;
    CHECK(AttackerVerdict(a) == HitBy::bad);  // nothing readable: not one of the game's
    a.is_sam = true;
    CHECK(AttackerVerdict(a) == HitBy::sam);
    a = AttackerView{};
    a.vt_ok = true;
    CHECK(AttackerVerdict(a) == HitBy::other);
    a.owner_sam = true;
    CHECK(AttackerVerdict(a) == HitBy::sam);
    a.owner_sam = false, a.has_inst = true;
    CHECK(AttackerVerdict(a) == HitBy::odd);  // an instance whose vtable is not the game's: never read (info only)
    a.inst_vt_ok = true;
    CHECK(AttackerVerdict(a) == HitBy::other);
    a.inst_owner_sam = true;
    CHECK(AttackerVerdict(a) == HitBy::sam);
    a.vt_ok = false;
    CHECK(AttackerVerdict(a) == HitBy::bad);  // the attacker itself first
    // the goal check
    CHECK(CheckGoal(0, false, 0, 0x1000) == GoalCheck::none);
    CHECK(CheckGoal(0x5000, true, 0x1000, 0x1000) == GoalCheck::ok);
    CHECK(CheckGoal(0x5000, true, 0x1008, 0x1000) == GoalCheck::bad);
    CHECK(CheckGoal(0x5000, false, 0x1000, 0x1000) == GoalCheck::bad);
    CHECK(CheckGoal(0x5000, true, 0x1000, 0) == GoalCheck::bad);  // no known vtable (its byte checks failed)
    // run 3: the carrier classes bit 32 counts (the MULE's DSMuleWeaponComponent + 0x50 and the plain NPC's)
    constexpr std::uintptr_t mule = 0x143DBA800, npc = 0x143DC7B78;
    CHECK(CarrierVtOk(mule, mule, npc));
    CHECK(CarrierVtOk(npc, mule, npc));
    CHECK(!CarrierVtOk(0x143E2BD80, mule, npc));  // Higgs' (not analysed)
    CHECK(!CarrierVtOk(0, mule, npc));
    CHECK(!CarrierVtOk(0, 0, 0));
}

// run 3: a switch by rule 3 (only visible) reads as "sticky" one tick later: the transition's rule must be captured at the change
// (enemy_target.cpp's chg_rule), not read from the state at 4 Hz
void TestTransitionRule() {
    // on her, seen at t0; Sam seen too; she drops out of the bucket
    PickState s = Cur(kPartner, t0, t0, t0);
    s.rule = rSticky;
    // t0 + 2000: her last sighting 2 s old (> fresh 1500), Sam in the bucket: rule 3 to Sam
    PickOut o = TargetPick(s, In(t0 + 2000, false, true), kCfg);
    CHECK(o.changed && o.st.cur == kLocal && o.st.rule == rOnly);
    const Rule at_change = o.st.rule;
    // the next tick: Sam still in the bucket: the current target fresh, the rule now sticky (what a 4 Hz reader would see)
    PickOut o2 = TargetPick(o.st, In(t0 + 2050, false, true), kCfg);
    CHECK(!o2.changed && o2.st.cur == kLocal && o2.st.rule == rSticky);
    CHECK(at_change == rOnly && o2.st.rule != at_change);
}

// Round 27 M3f, pick bits 16 (the line-of-sight keep) and 32 (her record from ours), and the sight verdict. The run-175849 repro:
// on her since t0, both in the bucket; the sensor stops appending her record (Sam stays): rule 3 used to switch at +1.5 s.
PickCfg EstCfg(bool synth) {
    PickCfg c;
    c.est_on = true, c.synth_on = synth;
    return c;
}
PickIn EstIn(std::uint64_t now, bool p, bool l, bool est) {
    PickIn x = In(now, p, l);
    x.est_p = est;
    return x;
}
void TestSightKeep() {
    PickState on_her = Cur(kPartner, t0, t0, t0);
    on_her.rule = rSticky;
    // 1 bits off: the estimate is ignored, rule 3 switches to Sam (today's behaviour)
    PickOut o = TargetPick(on_her, EstIn(t0 + 2000, false, true, true), kCfg);
    CHECK(o.changed && o.st.cur == kLocal && o.st.rule == rOnly);
    // 2 bit 16: kept on her by the estimate, the bucket suppressed (no synth without bit 32)
    o = TargetPick(on_her, EstIn(t0 + 2000, false, true, true), EstCfg(false));
    CHECK(!o.changed && o.st.cur == kPartner && o.st.rule == rSeenEst && o.act == aSuppress);
    // 3 bit 32: her record from ours
    o = TargetPick(on_her, EstIn(t0 + 2000, false, true, true), EstCfg(true));
    CHECK(!o.changed && o.st.cur == kPartner && o.st.rule == rSeenEst && o.act == aSynthPartner);
    // 4 the estimate not clear: rule 3 switches as before
    o = TargetPick(on_her, EstIn(t0 + 2000, false, true, false), EstCfg(true));
    CHECK(o.changed && o.st.cur == kLocal && o.st.rule == rOnly && o.act == aFrontLocal);
    // 5 her record fresh: plain sticky, the bucket's own record to the front (no synth)
    o = TargetPick(on_her, EstIn(t0 + 500, true, true, true), EstCfg(true));
    CHECK(o.st.cur == kPartner && o.st.rule == rSticky && o.act == aFrontPartner);
    // 6 keep-only: on Sam, her estimate clear but no bucket record: stays Sam
    o = TargetPick(Cur(kLocal, t0, t0, t0), EstIn(t0 + 2000, false, true, true), EstCfg(true));
    CHECK(!o.changed && o.st.cur == kLocal && o.act == aFrontLocal);
    // 7 keep-only: no target yet, only the estimate: no target
    o = TargetPick(PickState{}, EstIn(t0, false, false, true), EstCfg(true));
    CHECK(o.st.cur == kNone && o.act == aNone);
    // 8 her bucket record older than est_max_ms: the estimate stops counting
    o = TargetPick(on_her, EstIn(t0 + 30001, false, true, true), EstCfg(true));
    CHECK(o.changed && o.st.cur == kLocal && o.st.rule == rOnly);
    o = TargetPick(on_her, EstIn(t0 + 30000, false, true, true), EstCfg(true));
    CHECK(!o.changed && o.st.cur == kPartner && o.st.rule == rSeenEst);
    // 9 her stimulus gone: gone, whatever the estimate
    {
        PickIn x = EstIn(t0 + 2000, false, true, true);
        x.partner_ok = false;
        o = TargetPick(on_her, x, EstCfg(true));
        CHECK(o.st.cur != kPartner);
    }
    // 10 hit by Sam while kept by the estimate: rule 1 still switches (Sam fresh, his hit after the hold)
    {
        PickIn x = EstIn(t0 + 2000, false, true, true);
        x.hit_l = t0 + 1900;
        o = TargetPick(on_her, x, EstCfg(true));
        CHECK(o.changed && o.st.cur == kLocal && o.st.rule == rHitBy);
    }
    // 11 neither in the bucket, kept by the estimate: no record to rewrite, nothing done
    o = TargetPick(on_her, EstIn(t0 + 2000, false, false, true), EstCfg(true));
    CHECK(o.st.cur == kPartner && o.st.rule == rSeenEst && o.act == aNone);
    // 12 the 10 s repro: her record gone from t0+100, the estimate clear throughout, Sam every tick: never leaves her, and every
    // tick without her record is a synth (inside fresh_ms too: better than suppressing the bucket)
    {
        PickState s = on_her;
        bool stay = true;
        unsigned synth = 0;
        for (std::uint64_t t = t0 + 25; t <= t0 + 10000; t += 25) {
            const PickOut q = TargetPick(s, EstIn(t, t <= t0 + 100, true, true), EstCfg(true));
            if (q.st.cur != kPartner || q.changed) stay = false;
            synth += q.act == aSynthPartner;
            s = q.st;
        }
        CHECK(stay && synth == (10000 - 125) / 25 + 1);
    }
    // 13 the plan: synth only over a local record[0] with no record of hers
    const std::uint8_t l1[1] = {kLocal}, lp[2] = {kLocal, kPartner}, pl[2] = {kPartner, kLocal};
    Plan p = PlanBucket(l1, 1, aSynthPartner);
    CHECK(p.synth && p.swap == -1 && !p.count0);
    CHECK(!PlanBucket(lp, 2, aSynthPartner).synth && !PlanBucket(pl, 2, aSynthPartner).synth);
    CHECK(!PlanBucket(l1, 0, aSynthPartner).synth && !PlanBucket(nullptr, 1, aSynthPartner).synth);
    CHECK(!PlanBucket(l1, 1, aSuppress).synth && !PlanBucket(l1, 1, aFrontLocal).synth);
    // (M3f review) a refused synth falls back to the count-0 write (as bit 16 alone suppresses); never with a record of hers
    CHECK(p.fallback0);
    CHECK(!PlanBucket(lp, 2, aSynthPartner).fallback0 && !PlanBucket(lp, 2, aSynthPartner).count0);
    CHECK(!PlanBucket(l1, 0, aSynthPartner).fallback0 && !PlanBucket(nullptr, 1, aSynthPartner).fallback0);
    CHECK(!PlanBucket(l1, 1, aSuppress).fallback0 && !PlanBucket(l1, 1, aFrontPartner).fallback0);
    CHECK(std::string_view(RuleText(rSeenEst)) == "sticky (line of sight)");
}
void TestSightVerdict() {
    const SightCfg c{};
    SightIn s;
    CHECK(SightVerdict(s, c) == Sight::no_data);
    s.have = true, s.horiz = 20, s.bearing_deg = 10, s.pitch_deg = 5, s.vis = 1;
    CHECK(SightVerdict(s, c) == Sight::none);  // the cheap tests passed: cast the ray
    s.cast = true, s.ray = 0;
    CHECK(SightVerdict(s, c) == Sight::clear);
    s.ray = 2;
    CHECK(SightVerdict(s, c) == Sight::blocked);
    s.ray = -2;
    CHECK(SightVerdict(s, c) == Sight::ray_error);
    s.ray = 0, s.horiz = 61;
    CHECK(SightVerdict(s, c) == Sight::out_range);
    s.horiz = 20, s.bearing_deg = -61;
    CHECK(SightVerdict(s, c) == Sight::out_cone);
    s.bearing_deg = 60, s.pitch_deg = 41;
    CHECK(SightVerdict(s, c) == Sight::out_pitch);
    s.pitch_deg = -40, s.vis = 0.04f;
    CHECK(SightVerdict(s, c) == Sight::low_vis);
    s.vis = 0.05f;
    CHECK(SightVerdict(s, c) == Sight::clear);
    s.horiz = std::numeric_limits<double>::quiet_NaN();
    CHECK(SightVerdict(s, c) == Sight::no_data);
    CHECK(std::string_view(SightText(Sight::blocked)) == "blocked" && std::string_view(SightText(Sight::none)) == "-");
}
}  // namespace

int main() {
    TestKeys();
    TestTargetDefaults();
    TestGuardAndSelfTest();
    TestPick();
    TestPlan();
    TestForcedAndHelpers();
    TestShootAndGuards();
    TestTransitionRule();
    TestSightKeep();
    TestSightVerdict();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
