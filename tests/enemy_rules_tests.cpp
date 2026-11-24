// Enemy sync P1a (native/mod/enemy_rules.hpp): the schedule, the pose slots, the interpolation, the band, entry, the lease (and
// the cutscene hold), the lock-free entity set, the enemy shots' gate, the damage policy and the fingerprint gate. No game.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "config_values.hpp"
#include "enemy_rules.hpp"

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

void TestClasses() {
    CHECK(ClassOf(10, 250) == Class::near10);
    CHECK(ClassOf(250, 250) == Class::near10);
    CHECK(ClassOf(251, 250) == Class::mid2);
    CHECK(ClassOf(1000, 250) == Class::mid2);
    CHECK(ClassOf(1001, 250) == Class::far02);
    CHECK(ClassOf(-1, 250) == Class::far02);
    CHECK(PeriodOf(Class::near10) == 100 && PeriodOf(Class::mid2) == 500 && PeriodOf(Class::far02) == 5000);
    CHECK(FreshMs(Class::near10) == 300 && FreshMs(Class::mid2) == 1500);
}

void TestSchedule() {
    // 150 enemies: 10 near, 40 mid, 100 far
    std::vector<Cand> c;
    for (unsigned i = 0; i < 150; ++i) c.push_back(Cand{0x2000u | (i + 1), i < 10 ? 50.0 + i : i < 50 ? 400.0 + i : 2000.0 + i});
    Schedule s;
    std::vector<double> first(150, -1), lastsent(150, -1), maxgap(150, 0);
    unsigned out[64];
    unsigned max_tick = 0;
    for (double t = 0; t <= 20000; t += 33) {
        const unsigned n = s.Pick(c.data(), 150, t, 250, out, 64);
        if (n > max_tick) max_tick = n;
        for (unsigned k = 0; k < n; ++k) {
            const unsigned i = out[k];
            if (first[i] < 0) first[i] = t;
            if (lastsent[i] >= 0 && t - lastsent[i] > maxgap[i]) maxgap[i] = t - lastsent[i];
            lastsent[i] = t;
        }
    }
    CHECK(max_tick <= 64);
    bool all = true;
    for (unsigned i = 0; i < 150; ++i) all = all && first[i] >= 0 && first[i] <= 5500;
    CHECK(all);  // the whole set within 5.5 s
    // the near class about every 100 ms (a 33 ms tick: within 133 ms), the mid about every 500, the far about every 5 s
    double near_gap = 0, mid_gap = 0, far_gap = 0;
    for (unsigned i = 0; i < 10; ++i) near_gap = std::fmax(near_gap, maxgap[i]);
    for (unsigned i = 10; i < 50; ++i) mid_gap = std::fmax(mid_gap, maxgap[i]);
    for (unsigned i = 50; i < 150; ++i) far_gap = std::fmax(far_gap, maxgap[i]);
    CHECK(near_gap <= 133.0 + 1e-9);
    CHECK(mid_gap <= 533.0 + 1e-9);
    CHECK(far_gap <= 5500.0);
    // the cap: everything due at once (a new schedule) gives 64, the near class first
    Schedule s2;
    const unsigned n = s2.Pick(c.data(), 150, 0, 250, out, 64);
    CHECK(n == 64);
    bool near_first = true;
    for (unsigned k = 0; k < 10; ++k) near_first = near_first && out[k] < 10;
    CHECK(near_first);
    // a smaller cap
    Schedule s3;
    CHECK(s3.Pick(c.data(), 150, 0, 250, out, 32) == 32);
}

void TestPoseSlots() {
    PoseSlots p;
    PoseCand c[10];
    for (unsigned i = 0; i < 10; ++i) c[i] = PoseCand{0x2000u | (i + 1), 20.0 + 10.0 * i, true};  // 20, 30, ... 110 m
    p.Update(c, 10, 6);
    CHECK(p.Used() == 6);
    for (unsigned i = 0; i < 6; ++i) CHECK(p.SlotOf(c[i].id) >= 0);
    CHECK(p.SlotOf(c[6].id) < 0);
    // the nearest 3 at 10 Hz, the next 3 at 5 Hz
    for (unsigned i = 0; i < 6; ++i) CHECK(p.Period(static_cast<unsigned>(p.SlotOf(c[i].id))) == (i < 3 ? 100.0 : 200.0));
    const int slot5 = p.SlotOf(c[5].id);
    // hysteresis: #6 (80 m) moves to 75 m: #5 (70 m) stays (a member ranks 10 m nearer)
    c[6].dist = 75;
    p.Update(c, 10, 6);
    CHECK(p.SlotOf(c[5].id) == slot5 && p.SlotOf(c[6].id) < 0);
    // ... at 55 m it takes the slot of the farthest member
    c[6].dist = 55;
    p.Update(c, 10, 6);
    CHECK(p.SlotOf(c[6].id) == slot5 && p.SlotOf(c[5].id) < 0);
    // not realized on the host: no slot; beyond 120 m (+10 for a member): none
    c[0].realized = false;
    p.Update(c, 10, 6);
    CHECK(p.SlotOf(c[0].id) < 0);
    PoseSlots q;
    PoseCand far[2] = {PoseCand{0x2001, 121, true}, PoseCand{0x2002, 119, true}};
    q.Update(far, 2, 6);
    CHECK(q.Used() == 1 && q.SlotOf(0x2002) >= 0);
    far[1].dist = 129;  // a member: kept to 130 m
    q.Update(far, 2, 6);
    CHECK(q.SlotOf(0x2002) >= 0);
    far[1].dist = 131;
    q.Update(far, 2, 6);
    CHECK(q.Used() == 0);
    // count 0: none
    PoseSlots z;
    z.Update(c, 10, 0);
    CHECK(z.Used() == 0);
    // count above 8: 8
    PoseCand many[12];
    for (unsigned i = 0; i < 12; ++i) many[i] = PoseCand{0x2100u + i, 5.0 + i, true};
    z.Update(many, 12, 12);
    CHECK(z.Used() == 8);
}

void TestClock() {
    SenderClock k;
    CHECK(!k.have);
    const double t0 = k.Unwrap(0xFFFFFF00u);
    k.Note(t0, 1000);
    const double t1 = k.Unwrap(0x00000064u);  // wrapped: 0x164 ms later
    CHECK(std::fabs(t1 - t0 - 0x164) < 1e-6);
    k.Note(t1, 1000 + 0x164 + 30);  // a slower path: the offset stays the fastest
    CHECK(std::fabs(k.offset - (1000 - t0)) < 1e-6);
    CHECK(std::fabs(k.Render(2000) - (2000 - k.offset - 100)) < 1e-9);
}

void TestInterp() {
    Track tr;
    Sample a;
    a.t = 1000, a.pos[0] = 0, a.yaw = static_cast<float>(kPi - 0.1);
    a.vel[0] = 2.0f;
    Sample b = a;
    b.t = 1100, b.pos[0] = 0.2, b.yaw = static_cast<float>(-kPi + 0.1);
    CHECK(tr.Push(a, 0) && tr.Push(b, 0));
    CHECK(!tr.Push(a, 0));  // older: dropped
    double p[3];
    float y = 0;
    bool ex = false;
    CHECK(tr.At(1050, p, &y, &ex) && std::fabs(p[0] - 0.1) < 1e-9 && !ex);
    CHECK(YawDiffDeg(y, static_cast<float>(kPi)) < 0.01);  // across +-180 the short way
    // extrapolation, capped at 300 ms
    CHECK(tr.At(1200, p, &y, &ex) && ex && std::fabs(p[0] - (0.2 + 2.0 * 0.1)) < 1e-6);
    CHECK(tr.At(5000, p, &y, &ex) && std::fabs(p[0] - (0.2 + 2.0 * 0.3)) < 1e-6);
    // before the oldest: the oldest
    CHECK(tr.At(0, p, &y) && p[0] == 0);
    // a jump of more than 5 m: a snap (the earlier place until the later's time)
    Sample c = b;
    c.t = 1200, c.pos[0] = 10.2;
    tr.Push(c, 0);
    CHECK(tr.At(1150, p, &y) && std::fabs(p[0] - 0.2) < 1e-9);
    CHECK(tr.At(1200, p, &y) && std::fabs(p[0] - 10.2) < 1e-9);
    // the ring keeps the newest 8
    Track r;
    for (int i = 0; i < 12; ++i) {
        Sample s;
        s.t = i * 100.0;
        r.Push(s, 0);
    }
    CHECK(r.n == Track::kN && r.s[0].t == 400);
    CHECK(YawDiffDeg(static_cast<float>(kPi - 0.01), static_cast<float>(-kPi + 0.01)) < 1.2);
}

void TestBand() {
    CHECK(Band(0.01, 1, 0, -1e9) == Drive::none);
    CHECK(Band(0.01, 5, 0, -1e9) == Drive::set);
    CHECK(Band(1.5, 0, 0, -1e9) == Drive::set);
    CHECK(Band(2.5, 0, 1000, 0) == Drive::teleport);
    CHECK(Band(2.5, 0, 1200, 1000) == Drive::set);  // 2 a second at most
    // a second's worth of big errors at 30 ticks a second: 2 teleports
    double last = -1e9;
    int tps = 0;
    for (int i = 0; i < 30; ++i) {
        const double now = i * 33.4;
        if (Band(10, 0, now, last) == Drive::teleport) ++tps, last = now;
    }
    CHECK(tps == 2);
}

void TestEligible() {
    EligibleIn e;
    CHECK(Eligible(e) == Refusal::ok);
    EligibleIn x = e;
    x.fresh = false;
    CHECK(Eligible(x) == Refusal::stale);
    x = e, x.realized = false;
    CHECK(Eligible(x) == Refusal::not_realized);
    x = e, x.dead = true;
    CHECK(Eligible(x) == Refusal::dead);
    x = e, x.hidden_or_asleep = true;
    CHECK(Eligible(x) == Refusal::hidden_or_asleep);
    x = e, x.sequence_only = true;
    CHECK(Eligible(x) == Refusal::sequence_only);
    x = e, x.held_here = true;
    CHECK(Eligible(x) == Refusal::held_here);
    CHECK(std::strcmp(RefusalText(Refusal::held_here), "a real sequence holds it") == 0);
    x = e, x.held_on_host = true;
    CHECK(Eligible(x) == Refusal::held_on_host);
    x = e, x.cutscene = true;
    CHECK(Eligible(x) == Refusal::cutscene);
    // a MULE seated in a vehicle (untested: refused, review rv-safety 13)
    x = e, x.in_vehicle = true;
    CHECK(Eligible(x) == Refusal::in_vehicle);
    CHECK(std::strcmp(RefusalText(Refusal::in_vehicle), "in a vehicle") == 0);
    x.cutscene = true;
    CHECK(Eligible(x) == Refusal::cutscene);
}

// The session.ini values with ranges (config_values.hpp; review rv-tests 13).
void TestConfigValues() {
    CHECK(cfgval::ShotsSound("auto") == -1 && cfgval::ShotsSound("1") == 1 && cfgval::ShotsSound("on") == 1 && cfgval::ShotsSound("0") == 0 &&
          cfgval::ShotsSound("x") == 0);
    CHECK(cfgval::EnemyFreeze("occupied") == 1 && cfgval::EnemyFreeze("1") == 1 && cfgval::EnemyFreeze("0") == 0 && cfgval::EnemyFreeze("hold") == 0 &&
          cfgval::EnemyFreeze("") == 0);
    CHECK(cfgval::MirrorRadius("300", 250.0) == 300.0 && cfgval::MirrorRadius("20", 250.0) == 20.0 && cfgval::MirrorRadius("1000", 250.0) == 1000.0);
    CHECK(cfgval::MirrorRadius("19.9", 250.0) == 250.0 && cfgval::MirrorRadius("1001", 250.0) == 250.0 && cfgval::MirrorRadius("", 250.0) == 250.0 &&
          cfgval::MirrorRadius("300m", 250.0) == 250.0 && cfgval::MirrorRadius("abc", 250.0) == 250.0);
    CHECK(cfgval::PoseCount("0", 6) == 0 && cfgval::PoseCount("8", 6) == 8 && cfgval::PoseCount("9", 6) == 6 && cfgval::PoseCount("-1", 6) == 6 &&
          cfgval::PoseCount("", 6) == 6 && cfgval::PoseCount("4x", 6) == 6);
}

void TestLease() {
    LeaseIn l;
    l.frozen_by_us = true;
    CHECK(Lease(l) == LeaseVerdict::keep);
    LeaseIn x = l;
    x.frozen_by_us = false;
    x.off = true;
    CHECK(Lease(x) == LeaseVerdict::none);  // never one we did not freeze
    x = l, x.frame_age_ms = 1600;
    CHECK(Lease(x) == LeaseVerdict::release_silence);
    x.cls = Class::mid2;
    CHECK(Lease(x) == LeaseVerdict::keep);
    x.frame_age_ms = 3100;
    CHECK(Lease(x) == LeaseVerdict::release_silence);
    x = l, x.dist_m = 269;
    CHECK(Lease(x) == LeaseVerdict::keep);
    x.dist_m = 271;
    CHECK(Lease(x) == LeaseVerdict::release_range);
    x = l, x.realized_here = false;
    CHECK(Lease(x) == LeaseVerdict::release_unrealized);
    x = l, x.off = true;
    CHECK(Lease(x) == LeaseVerdict::release_off);
    x = l, x.bit14_now = false;
    CHECK(Lease(x) == LeaseVerdict::forget_taken);  // forgotten, nothing posted
    CHECK(!Releases(LeaseVerdict::forget_taken) && Releases(LeaseVerdict::release_off));
    x = l, x.hold = true, x.off = true, x.frame_age_ms = 99999;
    CHECK(Lease(x) == LeaseVerdict::hold);  // a cutscene: nothing given back
    // the cutscene hold: held through it and 2 s after, then one release-all
    CutsceneHold h;
    CHECK(h.Step(false, 0) == 0);
    CHECK(h.Step(true, 100) == 1);
    CHECK(h.Step(true, 5000) == 1);
    CHECK(h.Step(false, 6000) == 1);
    CHECK(h.Step(false, 7999) == 1);
    CHECK(h.Step(false, 8000) == 2);
    CHECK(h.Step(false, 8100) == 0);
    // idempotence: a released mirror is not ours any more, the next verdict is none
    x = l, x.off = true;
    CHECK(Releases(Lease(x)));
    x.frozen_by_us = false;
    CHECK(Lease(x) == LeaseVerdict::none);
}

void TestKeySet() {
    KeySet s;
    CHECK(!s.Find(0x1000) && s.Size() == 0);
    std::uint64_t k[3] = {0x10000, 0x20000, 0x30000};
    std::uint32_t v[3] = {0x2001, 0x2002, 0x2003};
    s.Publish(k, v, 3);
    std::uint32_t got = 0;
    CHECK(s.Find(0x20000, &got) && got == 0x2002);
    CHECK(s.Find(0x20020, &got) && got == 0x2002);   // + 0x20
    CHECK(s.Find(0x30000 - 0x20, &got) && got == 0x2003);  // the key is the value + 0x20
    CHECK(!s.Find(0x20020, &got, false));            // exact only
    CHECK(!s.Find(0x20010));
    // removal
    s.Publish(k, v, 1);
    CHECK(s.Find(0x10000) && !s.Find(0x20000) && s.Size() == 1);
    // capacity 64
    std::uint64_t many[80];
    std::uint32_t mv[80];
    for (unsigned i = 0; i < 80; ++i) many[i] = 0x100000 + 0x1000ull * i, mv[i] = 0x2000 + i;
    s.Publish(many, mv, 80);
    CHECK(s.Size() == 64 && s.Find(many[63]) && !s.Find(many[64]));
    s.Clear();
    CHECK(s.Size() == 0 && !s.Find(many[0]));
}

void TestShotGate() {
    CHECK(EnemyShotGate(false, 0, 1000) == shotw::Gate::now_no_body);
    CHECK(EnemyShotGate(true, 900, 1000) == shotw::Gate::hold);
    CHECK(EnemyShotGate(true, 1100, 1000) == shotw::Gate::fire);
    CHECK(EnemyShotGate(true, 1600, 1000) == shotw::Gate::late);
    CHECK(EnemyShotGate(true, 1000, 3500) == shotw::Gate::now_ahead);
}

void TestDamagePolicy() {
    DamageIn d;
    // kind 1 (the partner's): dropped unless friendly fire
    d.replay = true, d.kind = 1;
    CHECK(SamPolicy(d) == Dmg::drop);
    d.friendly_fire = true;
    CHECK(SamPolicy(d) == Dmg::keep);
    // kind 4 (the aimed test) as kind 1
    d.kind = 4, d.friendly_fire = false;
    CHECK(SamPolicy(d) == Dmg::drop);
    // kind 2 (an enemy's): kept when enemy_shots_hurt, dropped without it or under a nohurt test
    d.kind = 2, d.enemy_hurt = true;
    CHECK(SamPolicy(d) == Dmg::keep);
    d.nohurt_once = true;
    CHECK(SamPolicy(d) == Dmg::drop);
    d.nohurt_once = false, d.enemy_hurt = false;
    CHECK(SamPolicy(d) == Dmg::drop);
    d.friendly_fire = true;  // friendly fire never keeps an enemy's
    CHECK(SamPolicy(d) == Dmg::drop);
    // a kind-2 replay is never dropped by the mirror rule
    d.enemy_hurt = true, d.mirror_attacker = true, d.guard = true;
    CHECK(SamPolicy(d) == Dmg::keep);
    // kind 3 (a test): dropped unless the test keeps it; friendly fire does not keep it
    d = DamageIn{};
    d.replay = true, d.kind = 3, d.friendly_fire = true;
    CHECK(SamPolicy(d) == Dmg::drop);
    d.test_nofilter = true;
    CHECK(SamPolicy(d) == Dmg::keep);
    // not a replay: a frozen mirror's hit dropped (guard on), counted only with the guard off; any other hit kept
    d = DamageIn{};
    d.mirror_attacker = true;
    CHECK(SamPolicy(d) == Dmg::drop);
    d.guard = false;
    CHECK(SamPolicy(d) == Dmg::keep);
    d.mirror_attacker = false, d.guard = true;
    CHECK(SamPolicy(d) == Dmg::keep);
}

void TestFingerprintGate() {
    Print a{shotw::kPartMirror | shotw::kPartFire, 150, 0x40624894u, 0x55AD7620u};
    Print b = a;
    CHECK(FingerprintGate(a, b, true, 1000) == FpGate::on);
    CHECK(FingerprintGate(a, b, false, 0) == FpGate::no_peer);
    CHECK(FingerprintGate(a, b, true, 25000) == FpGate::stale);
    Print c = b;
    c.parts = shotw::kPartFire;
    CHECK(FingerprintGate(a, c, true, 0) == FpGate::not_theirs);
    CHECK(FingerprintGate(c, b, true, 0) == FpGate::not_ours);
    c = b, c.ids ^= 1;
    CHECK(FingerprintGate(a, c, true, 0) == FpGate::differ);
    c = b, c.guids ^= 1;
    CHECK(FingerprintGate(a, c, true, 0) == FpGate::differ);
    c = b, c.count = 149;
    CHECK(FingerprintGate(a, c, true, 0) == FpGate::differ);
    c = b, c.guids = 0;
    CHECK(FingerprintGate(a, c, true, 0) == FpGate::unready);
    Print z = a;
    z.count = 0, z.ids = 0, z.guids = 0;
    CHECK(FingerprintGate(z, z, true, 0) == FpGate::unready);  // equal zeros never turn it on
}

void TestPulls() {
    shotw::PullGroups g;
    CHECK(g.Next(0x2001, 0, 12));      // the first of 0x2001's pull
    CHECK(g.Next(0x2002, 5, 12));      // another MULE: its own pull
    CHECK(!g.Next(0x2001, 10, 12));    // 0x2001's pellet within 16 ms
    CHECK(!g.Next(0x2002, 15, 12));
    CHECK(g.Next(0x2001, 100, 12));    // a new pull
    for (unsigned i = 0; i < 20; ++i) g.Next(0x3000u + i, 200.0 + i, 1);  // 20 more shooters: the LRU keeps 16
    CHECK(g.Next(0x2001, 230, 12));    // pushed out: a new pull
    shotw::EnemyRateCap cap;
    int took = 0;
    for (int i = 0; i < 40; ++i) took += cap.Take(i * 2.0) ? 1 : 0;  // 40 within 80 ms
    CHECK(took == 24);
    CHECK(cap.Take(150.0));
}

}  // namespace

int main() {
    TestClasses();
    TestSchedule();
    TestPoseSlots();
    TestClock();
    TestInterp();
    TestBand();
    TestEligible();
    TestConfigValues();
    TestLease();
    TestKeySet();
    TestShotGate();
    TestDamagePolicy();
    TestFingerprintGate();
    TestPulls();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
