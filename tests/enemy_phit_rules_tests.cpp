// Round 28 P1 (native/mod/enemy_phit_rules.hpp): the attack probe's body gaps, caller sites, wrappers, tallies and packing.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "config_values.hpp"
#include "enemy_phit_rules.hpp"

using namespace st::phrules;

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

bool Close(double a, double b, double e = 1e-6) { return a - b < e && b - a < e; }

void TestSegments() {
    // point to segment: beside, beyond each end, a degenerate segment
    CHECK(Close(PointSegDist({1, 0, 1}, {0, 0, 0}, {0, 0, 2}), 1.0));
    CHECK(Close(PointSegDist({0, 0, 3}, {0, 0, 0}, {0, 0, 2}), 1.0));
    CHECK(Close(PointSegDist({0, 0, -0.5}, {0, 0, 0}, {0, 0, 2}), 0.5));
    CHECK(Close(PointSegDist({3, 4, 0}, {0, 0, 0}, {0, 0, 0}), 5.0));
    // segment to segment: crossing, parallel, skew, end to end, degenerate
    CHECK(Close(SegSegDist({-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}), 0.0));
    CHECK(Close(SegSegDist({0, 0, 0}, {2, 0, 0}, {0, 1, 0}, {2, 1, 0}), 1.0));
    CHECK(Close(SegSegDist({-1, 0, 0}, {1, 0, 0}, {0, -1, 2}, {0, 1, 2}), 2.0));
    CHECK(Close(SegSegDist({0, 0, 0}, {1, 0, 0}, {3, 0, 0}, {4, 0, 0}), 2.0));
    CHECK(Close(SegSegDist({0, 0, 0}, {0, 0, 0}, {0, 0, 1}, {0, 0, 1}), 1.0));
    CHECK(Close(SegSegDist({0, 0, 0}, {0, 0, 0}, {1, -1, 0}, {1, 1, 0}), 1.0));
    CHECK(Close(SegSegDist({1, -1, 0}, {1, 1, 0}, {0, 0, 0}, {0, 0, 0}), 1.0));
    // symmetric
    CHECK(Close(SegSegDist({0, 0, 0}, {1, 2, 3}, {4, 0, -1}, {2, 2, 2}), SegSegDist({4, 0, -1}, {2, 2, 2}, {0, 0, 0}, {1, 2, 3})));
}

void TestGaps() {
    const V3 feet{10, 20, 5};
    // a MULE kick at knee height, 1 m in front of her axis: gap 1 - 0.35 - r
    CHECK(Close(SphereGap({11, 20, 5.5}, 0.5, feet), 1.0 - kBodyRadius - 0.5));
    CHECK(!Overlaps(SphereGap({11, 20, 5.5}, 0.5, feet)));
    CHECK(Near(SphereGap({11, 20, 5.5}, 0.5, feet)));
    CHECK(Overlaps(SphereGap({10.6, 20, 6.0}, 0.5, feet)));
    // above the head: from the top of the axis (feet + 1.45)
    CHECK(Close(SphereGap({10, 20, 5 + 1.45 + 2.0}, 0.25, feet), 2.0 - kBodyRadius - 0.25));
    // far away: neither overlap nor near
    CHECK(!Overlaps(SphereGap({30, 20, 5}, 1.0, feet)));
    CHECK(!Near(SphereGap({30, 20, 5}, 1.0, feet)));
    // a capsule strike across her chest
    CHECK(Overlaps(CapsuleGap({9, 21, 6.2}, {11, 19, 6.2}, 0.1, feet)));
    CHECK(Close(CapsuleGap({12, 19, 6.2}, {12, 21, 6.2}, 0.1, feet), 2.0 - kBodyRadius - 0.1));
    // bad input: NaN, never an overlap
    CHECK(std::isnan(SphereGap({NAN, 0, 0}, 1.0, feet)));
    CHECK(std::isnan(SphereGap({0, 0, 0}, -1.0, feet)));
    CHECK(std::isnan(CapsuleGap({0, 0, 0}, {0, 0, INFINITY}, 1.0, feet)));
    CHECK(!Overlaps(std::nan("")));
    CHECK(!Near(std::nan("")));
    CHECK(Overlaps(0.0) && Near(1.0) && !Near(1.01));
}

void TestIdsAndAttacks() {
    CHECK(IsMuleId(0x2029) && IsMuleId(0x202B) && IsMuleId(0x2000));
    CHECK(!IsMuleId(kNoActor) && !IsMuleId(0x0001) && !IsMuleId(0x4029));
    CHECK(std::strcmp(AttackName(0x16F), "MuleKick") == 0);
    CHECK(std::strcmp(AttackName(0x157), "VehicleTackle") == 0);
    CHECK(std::strcmp(AttackName(0x1234), "?") == 0);
    CHECK(IsMuleMelee(0x15E) && IsMuleMelee(0x16D) && IsMuleMelee(0x171));
    CHECK(!IsMuleMelee(0x172) && !IsMuleMelee(0x157) && !IsMuleMelee(0x16C));
    CHECK(std::strcmp(QueryName(Query::sphere), "sphere") == 0 && std::strcmp(QueryName(Query::grab), "grab") == 0);
}

void TestSites() {
    CHECK(WrapperOfReturn(0x142993D08ull + 5) == 2);
    CHECK(WrapperOfReturn(0x14294B0BAull + 5) == 3);
    CHECK(kWrappers[3].frame == 0xB8 && kWrappers[0].frame == 0x98);
    CHECK(WrapperOfReturn(0x142993D08ull) == -1);
    CHECK(WrapperOfReturn(0) == -1);
    CHECK(std::strcmp(SiteName(0x142B53F99ull), "mule-melee") == 0);
    CHECK(std::strcmp(SiteName(0x142917C22ull), "weapon-capsule") == 0);
    CHECK(std::strcmp(SiteName(0x142F8AEF5ull), "vehicle-tackle") == 0);
    CHECK(std::strcmp(SiteName(0x140001000ull), "other") == 0);
    CHECK(IsMuleSite(0x142B53F99ull) && IsMuleSite(0x142AF6090ull) && IsMuleSite(0x14291803Aull) && IsMuleSite(0x142917F81ull));
    CHECK(!IsMuleSite(0x142F8AEF5ull) && !IsMuleSite(0x1427FED76ull) && !IsMuleSite(0));
    // every label site is distinct
    for (unsigned i = 0; i < sizeof(kSites) / sizeof(kSites[0]); ++i)
        for (unsigned j = i + 1; j < sizeof(kSites) / sizeof(kSites[0]); ++j) CHECK(kSites[i].call_va != kSites[j].call_va);
}

void TestTally() {
    Tally<3, unsigned> t;
    t.Add(5), t.Add(5), t.Add(7), t.Add(9), t.Add(11), t.Add(11), t.Add(7);
    CHECK(t.used == 3 && t.Of(5) == 2 && t.Of(7) == 2 && t.Of(9) == 1 && t.Of(11) == 0 && t.other == 2);
    Stat s;
    s.Add(Query::sphere, 0x16F, 0x142B53F99ull, 0x1D, 0x2029, 0.5f, -0.2, 40.0, 1);
    s.Add(Query::capsule, 0x16D, 0x142917C22ull, 0x1D, 0x2029, 0.3f, 0.6, std::nan(""), 0);
    s.Add(Query::sphere, 0x16F, 0x142B53F99ull, 0x1D, 0x2029, 0.5f, 3.0, 0.5, 0x10000);  // a bad count is not summed
    CHECK(s.calls == 3 && s.sphere == 2 && s.capsule == 1);
    CHECK(s.pbody == 1 && s.pbody_near == 2 && s.sam == 0 && s.sam_near == 1);
    CHECK(Close(s.min_pbody, -0.2) && Close(s.min_sam, 0.5));
    CHECK(Close(s.r_min, 0.3, 1e-6) && Close(s.r_max, 0.5, 1e-6));
    CHECK(s.hits == 1);
    CHECK(s.attacks.Of(0x16F) == 2 && s.attacks.Of(0x16D) == 1 && s.sites.used == 2 && s.layers.used == 1 && s.attackers.Of(0x2029) == 3);
    Stat e;
    CHECK(e.min_pbody > 1e8 && e.r_max < 0);
}

void TestPacking() {
    CHECK(Lo(Pack(0x12345678u, 0x9ABCDEF0u)) == 0x12345678u && Hi(Pack(0x12345678u, 0x9ABCDEF0u)) == 0x9ABCDEF0u);
    CHECK(BitsF(FBits(-1.5f)) == -1.5f && BitsF(FBits(0.0f)) == 0.0f);
    CHECK(std::isnan(BitsF(FBits(std::nanf("")))));
    CHECK(BitsD(DBits(1234.5678)) == 1234.5678);
    CHECK(Lo(Pack(FBits(0.5f), FBits(-1.0f))) == FBits(0.5f));
    // the amount: a negative one takes the resource's default when there is one
    CHECK(EffectiveAmount(-1.0f, true, 25.0f) == 25.0f);
    CHECK(EffectiveAmount(-1.0f, false, 25.0f) == -1.0f);
    CHECK(EffectiveAmount(100.0f, true, 25.0f) == 100.0f);
    // the watch's length
    CHECK(WatchSecs(0) == 60.0 && WatchSecs(40) == 40.0 && WatchSecs(301) == 300.0 && WatchSecs(1) == 1.0 && WatchSecs(-5) == 60.0);
    CHECK(WatchSecs(1000) == 300.0 && WatchSecs(300) == 300.0 && WatchSecs(std::nan("")) == 60.0);
}

// P1b: the attacker's pose at a call (facing offsets, distances, the per-attacker pose numbers)
void TestPose() {
    const V3 mule{100, 200, 10};
    // facing +x (yaw 0): a body 2 m ahead is 0 deg off, one behind 180, one to the side 90
    CHECK(Close(FacingOffDeg(0.0, mule, {102, 200, 10}), 0.0));
    CHECK(Close(FacingOffDeg(0.0, mule, {98, 200, 10}), 180.0));
    CHECK(Close(FacingOffDeg(0.0, mule, {100, 203, 10}), 90.0));
    CHECK(Close(FacingOffDeg(0.0, mule, {100, 197, 10}), 90.0));
    // wrap: facing just under +180 deg, a target just over -180 deg is ~2 deg off
    CHECK(Close(FacingOffDeg(kPiD - 0.01745329, mule, {90, 199.825, 10}), 2.0, 0.05));
    // on top of it: 0; bad input: NaN, never "faced"
    CHECK(Close(FacingOffDeg(1.0, mule, {100, 200, 50}), 0.0));
    CHECK(std::isnan(FacingOffDeg(std::nan(""), mule, {1, 2, 3})));
    CHECK(std::isnan(FacingOffDeg(0.0, mule, {NAN, 2, 3})));
    CHECK(!Faced(std::nan("")) && Faced(45.0) && !Faced(45.5) && Faced(0.0));
    // yaw from the facing row; a zero or NaN row is no yaw
    CHECK(Close(YawOfFacing(0.0f, 1.0f), kPiD / 2));
    CHECK(Close(YawOfFacing(-1.0f, 0.0f), kPiD));
    CHECK(std::isnan(YawOfFacing(0.0f, 0.0f)) && std::isnan(YawOfFacing(NAN, 1.0f)));
    // horizontal distance ignores z
    CHECK(Close(HorizDist(mule, {103, 204, -50}), 5.0));
    CHECK(std::isnan(HorizDist(mule, {INFINITY, 0, 0})));
    CHECK(Close(Mid({0, 0, 0}, {2, 4, 6}).y, 2.0));
    // the run's kick: her 2.3 m behind it (164 deg), Sam 155 m ahead (2 deg): faced Sam, not her
    Stat s;
    s.AddPose(2.3, 164.0, 2.0, 1.0);
    s.AddPose(2.4, 170.0, 3.0, 1.2);
    s.AddPose(std::nan(""), std::nan(""), 50.0, std::nan(""));
    CHECK(s.posed == 3 && s.faced_pbody == 0 && s.faced_sam == 2);
    CHECK(Close(s.min_att_pbody, 2.3) && Close(s.max_reach, 1.2));
    Stat e;
    CHECK(e.posed == 0 && e.min_att_pbody > 1e8 && e.max_reach < 0);
    // the new names
    CHECK(std::strcmp(AttackName(0x17B), "ElectricZone") == 0 && std::strcmp(SiteName(0x142E44DBFull), "zone") == 0);
    CHECK(!IsMuleSite(0x142E44DBFull) && !IsMuleMelee(0x17B));
}

// session.ini enemy_phit_probe (config_values.hpp SetPhitKey)
void TestKey() {
    bool on = false;
    CHECK(st::cfgval::SetPhitKey(on, "enemy_phit_probe", true) && on);
    CHECK(st::cfgval::SetPhitKey(on, "enemy_phit_probe", false) && !on);
    CHECK(!st::cfgval::SetPhitKey(on, "enemy_phit_probes", true) && !on);
    // Round 28 E2: enemy_partner_melee (0 / 1 / 2, anything else 0) and enemy_melee_face (SetPartnerMeleeKey)
    int m = 9;
    bool f = false;
    CHECK(st::cfgval::SetPartnerMeleeKey(m, f, "enemy_partner_melee", "1", true) && m == 1);
    CHECK(st::cfgval::SetPartnerMeleeKey(m, f, "enemy_partner_melee", "2", false) && m == 2);
    CHECK(st::cfgval::SetPartnerMeleeKey(m, f, "enemy_partner_melee", "3", true) && m == 0);
    CHECK(st::cfgval::SetPartnerMeleeKey(m, f, "enemy_partner_melee", "x", false) && m == 0);
    CHECK(st::cfgval::SetPartnerMeleeKey(m, f, "enemy_melee_face", "1", true) && f);
    CHECK(!st::cfgval::SetPartnerMeleeKey(m, f, "enemy_partner_melees", "1", true));
}

// Round 28 E2: the PHIT relay's rules
void TestPhit() {
    // the relayed attacks: the MULE melee strikes only (not her push 0x15E, not ElectricZone 0x17B)
    CHECK(PhitAttackOk(0x16D) && PhitAttackOk(0x16E) && PhitAttackOk(0x16F) && PhitAttackOk(0x170) && PhitAttackOk(0x171));
    CHECK(!PhitAttackOk(0x15E) && !PhitAttackOk(0x17B) && !PhitAttackOk(0x16C) && !PhitAttackOk(0x172) && !PhitAttackOk(0));
    // the host's test: a MULE, a melee attack, not the mod's own call, the shape reaching her (gap <= 0)
    CHECK(PhitCandidate(false, 0x2029, 0x16E, -0.5));
    CHECK(PhitCandidate(false, 0x2029, 0x16F, 0.0));
    CHECK(!PhitCandidate(true, 0x2029, 0x16E, -0.5));
    CHECK(!PhitCandidate(false, 0x2029, 0x16E, 0.01));
    CHECK(!PhitCandidate(false, 0x2029, 0x15E, -0.5));
    CHECK(!PhitCandidate(false, 0x4001, 0x16E, -0.5));
    CHECK(!PhitCandidate(false, kNoActor, 0x16E, -0.5));
    CHECK(!PhitCandidate(false, 0x2029, 0x16E, std::nan("")));
    // the run's baton (20261127-002430): a 0.5 m capsule through her chest reaches her; its contact is on the axis
    const V3 feet{10, 20, 0};
    const V3 qa{9.2, 20.0, 1.2}, qb{10.6, 20.0, 1.2};
    CHECK(Overlaps(CapsuleGap(qa, qb, 0.5, feet)));
    const V3 c = ContactPoint(true, qa, qb, feet);
    CHECK(Close(c.x, 10.0) && Close(c.y, 20.0) && Close(c.z, 1.2));
    // a sphere's contact is its centre; a capsule's beyond its ends clamps to the end
    const V3 s = ContactPoint(false, {1, 2, 3}, {9, 9, 9}, feet);
    CHECK(Close(s.x, 1) && Close(s.y, 2) && Close(s.z, 3));
    const V3 e = ContactPoint(true, {0, 0, 1}, {1, 0, 1}, {5, 0, 0});
    CHECK(Close(e.x, 1) && Close(e.z, 1));
    // the strike's direction: unit, horizontal; degenerate -> 0
    float dx = 9, dy = 9;
    StrikeDir({0, 0, 0}, {3, 4, 7}, &dx, &dy);
    CHECK(Close(dx, 0.6) && Close(dy, 0.8));
    StrikeDir({1, 1, 0}, {1, 1, 5}, &dx, &dy);
    CHECK(dx == 0 && dy == 0);
    // dedupe: the same MULE + attack within 300 ms of its last query is one strike (the window slides); another attack or MULE is
    // not; 300 ms with no query, again
    PhitDedupe d;
    CHECK(d.Take(0x2029, 0x16E, 1000));
    CHECK(!d.Take(0x2029, 0x16E, 1100));
    CHECK(!d.Take(0x2029, 0x16E, 1299));
    CHECK(d.Take(0x2029, 0x16D, 1100));
    CHECK(d.Take(0x2032, 0x16E, 1100));
    CHECK(!d.Take(0x2029, 0x16E, 1310));         // one long strike: 310 ms after its first, 11 ms after its last
    CHECK(!d.Take(0x2029, 0x16E, 1500));
    CHECK(d.Take(0x2029, 0x16E, 1800));          // 300 ms quiet: the next strike
    CHECK(!d.Take(0x2029, 0x16E, 1900));
    // the guest's lag: the best (local - sender) is the base; a backlog arrives far behind it; modulo 2^32
    PhitLag lag;
    CHECK(lag.Observe(5000u, 10000) == 0);
    CHECK(lag.Observe(5040u, 10100) == 40);
    CHECK(lag.Observe(4990u, 10200) == 0);       // a better one: the new base
    CHECK(lag.Observe(6990u, 10300) == 2000);    // 2 s behind: stale
    CHECK(lag.Observe(4995u, 31000) == 5);       // the window rolls to its best (4990), kept
    PhitLag w;
    CHECK(w.Observe(0xFFFFFFF0u, 0) == 0);
    CHECK(w.Observe(0x00000010u, 100) == 32);    // across the wrap
    // a full table reuses the oldest entry
    PhitDedupe f;
    for (unsigned i = 0; i < PhitDedupe::kN; ++i) CHECK(f.Take(0x2000 + i + 1, 0x16F, 100.0 + i));
    CHECK(f.Take(0x2100, 0x16F, 200));          // the oldest (0x2001) goes
    CHECK(f.Take(0x2001, 0x16F, 201));          // so it is new again
    CHECK(!f.Take(0x2100, 0x16F, 250));
    // the rate cap: 3 per MULE, 8 in all, per 1000 ms
    PhitRate r;
    CHECK(r.Take(0x2029, 0) && r.Take(0x2029, 100) && r.Take(0x2029, 200));
    CHECK(!r.Take(0x2029, 300));
    CHECK(r.Take(0x2029, 1001));                 // the first one left the window
    for (unsigned i = 0; i < 5; ++i) CHECK(r.Take(0x2030 + i, 1002 + i));
    CHECK(!r.Take(0x2040, 1010));                // 8 in the window (100, 200, 1001, five more)
    CHECK(r.Take(0x2040, 1101));                 // 100 left it
    // validity: a MULE, a relayed attack, a finite point within 8 m of her
    CHECK(PhitValid(0x2029, 0x16F, {11, 20, 1}, feet));
    CHECK(!PhitValid(0x2029, 0x16F, {19, 20, 1}, feet));
    CHECK(!PhitValid(0x2029, 0x17B, {11, 20, 1}, feet));
    CHECK(!PhitValid(0x4001, 0x16F, {11, 20, 1}, feet));
    CHECK(!PhitValid(0x2029, 0x16F, {NAN, 20, 1}, feet));
    // the guest's checks, in order; the 4 m dodge
    PhitIn in;
    in.key = in.world = in.valid = in.sam = in.on_foot = true;
    in.moved_m = 1.0;
    CHECK(PhitDecide(in) == PhitVerdict::apply);
    in.moved_m = 4.0;
    CHECK(PhitDecide(in) == PhitVerdict::apply);
    in.moved_m = 4.01;
    CHECK(PhitDecide(in) == PhitVerdict::dodged);
    in.moved_m = std::nan("");
    CHECK(PhitDecide(in) == PhitVerdict::dodged);
    in.moved_m = 0;
    in.need_owner = true;
    CHECK(PhitDecide(in) == PhitVerdict::no_owner);
    in.owner = true;
    CHECK(PhitDecide(in) == PhitVerdict::apply);
    in.cutscene = true;
    CHECK(PhitDecide(in) == PhitVerdict::cutscene);
    in.down = true;
    CHECK(PhitDecide(in) == PhitVerdict::down);
    in.on_foot = false;
    CHECK(PhitDecide(in) == PhitVerdict::not_on_foot);
    in.sam = false;
    CHECK(PhitDecide(in) == PhitVerdict::no_sam);
    in.valid = false;
    CHECK(PhitDecide(in) == PhitVerdict::not_valid);
    in.world = false;
    CHECK(PhitDecide(in) == PhitVerdict::no_peer);
    in.key = false;
    CHECK(PhitDecide(in) == PhitVerdict::off);
    CHECK(std::strcmp(PhitVerdictText(PhitVerdict::dodged), "dodged") == 0);
    // mode 2's amount: f32 10, i32 10, junk -> 10, capped at 100
    float ten = 10.0f, big = 5000.0f;
    std::uint32_t tb = 0, bb = 0;
    std::memcpy(&tb, &ten, 4);
    std::memcpy(&bb, &big, 4);
    CHECK(PhitAmount(tb) == 10.0f && PhitAmount(10u) == 10.0f && PhitAmount(0u) == 10.0f && PhitAmount(150u) == 100.0f);
    CHECK(PhitAmount(bb) == 10.0f);
    // redirect bit 64: her within 6 m horizontally and 3 m in height
    CHECK(MeleeFaceWants({0, 0, 0}, {4, 4, 1}));
    CHECK(!MeleeFaceWants({0, 0, 0}, {5, 5, 0}));
    CHECK(!MeleeFaceWants({0, 0, 0}, {1, 1, 3.5}));
    CHECK(!MeleeFaceWants({0, 0, NAN}, {1, 1, 0}));
    // the key's values
    CHECK(PartnerMeleeMode(0) == 0 && PartnerMeleeMode(1) == 1 && PartnerMeleeMode(2) == 2 && PartnerMeleeMode(3) == 0 && PartnerMeleeMode(-1) == 0);
}
}  // namespace

int main() {
    TestSegments();
    TestGaps();
    TestIdsAndAttacks();
    TestSites();
    TestTally();
    TestPacking();
    TestPose();
    TestKey();
    TestPhit();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
