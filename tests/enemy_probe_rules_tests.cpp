// Round 23 E (native/mod/enemy_probe_rules.hpp): the M2 probes' freshness, angles, push and phase trackers, ray trim, key.
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "config.hpp"
#include "config_values.hpp"
#include "enemy_probe_rules.hpp"

using namespace st;
using namespace st::eprobe;

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

bool Near(double a, double b, double e = 1e-6) { return a - b < e && b - a < e; }

void TestNames() {
    CHECK(std::strcmp(KnowledgeTypeName(1), "PlayerVisual") == 0);
    CHECK(std::strcmp(KnowledgeTypeName(51), "PlayerNoise") == 0);
    CHECK(std::strcmp(KnowledgeTypeName(81), "Damaged") == 0);
    CHECK(std::strcmp(KnowledgeTypeName(85), "PlayerLastKnown") == 0);
    CHECK(std::strcmp(KnowledgeTypeName(7), "other") == 0);
    CHECK(LeverName(0)[0] == 'a' && LeverName(1)[0] == 'b' && LeverName(2)[0] == 'c' && LeverName(3)[0] == 'd');
    CHECK(std::strcmp(LeverName(9), "?") == 0);
}

void TestFresh() {
    FreshTrack t;
    CHECK(t.Verdict() == Fresh::none);
    CHECK(t.Feed(false, 0, 0) == Fresh::none);
    CHECK(t.Verdict() == Fresh::none);
    // first sight: stale until it changes
    CHECK(t.Feed(true, 10, 500) == Fresh::stale);
    CHECK(t.Verdict() == Fresh::stale);
    CHECK(t.Feed(true, 10, 1000) == Fresh::stale);
    // a change: fresh for the window
    CHECK(t.Feed(true, 11, 1500) == Fresh::fresh);
    CHECK(t.Feed(true, 11, 2500) == Fresh::fresh);
    CHECK(t.Feed(true, 11, 3000) == Fresh::fresh);  // 1500 ms: still within
    CHECK(t.Feed(true, 11, 3001) == Fresh::stale);
    CHECK(t.Verdict() == Fresh::fresh);
    // gone, then back the same: first sight again (stale)
    CHECK(t.Feed(false, 0, 3500) == Fresh::none);
    CHECK(t.Feed(true, 11, 4000) == Fresh::stale);
    CHECK(t.n_fresh == 3 && t.n_stale == 4 && t.n_none == 2);
    CHECK(std::strcmp(FreshText(Fresh::fresh), "fresh") == 0 && std::strcmp(FreshText(Fresh::stale), "stale") == 0 &&
          std::strcmp(FreshText(Fresh::none), "none") == 0);
    // never written after the first sight: the verdict stays stale
    FreshTrack s;
    for (int i = 0; i < 10; ++i) s.Feed(true, 5, 500.0 * i);
    CHECK(s.Verdict() == Fresh::stale && s.n_fresh == 0);
}

void TestAngles() {
    CHECK(Near(Wrap180(190), -170) && Near(Wrap180(-190), 170) && Near(Wrap180(720), 0) && Near(Wrap180(180), 180));
    CHECK(Near(Wrap180(0.0 / 1.0), 0));
    // facing +x (yaw 0): a point straight ahead, at 90 deg, behind
    CHECK(Near(OffDeg(0, 0, 0, 10, 0), 0));
    CHECK(Near(OffDeg(0, 0, 0, 0, 10), 90));
    CHECK(Near(OffDeg(0, 0, 0, -10, 0), 180));
    CHECK(Near(OffDeg(3.14159265358979323846 / 2, 0, 0, 0, 10), 0, 1e-9));
    CHECK(OffDeg(0, 1, 1, 1.05, 1) < 0);  // too close
    CHECK(WithinCone(0) && WithinCone(30) && !WithinCone(30.5) && !WithinCone(-1));
    // across the wrap: facing 170 deg, the point at -170 deg is 20 deg off
    constexpr double kRad = 3.14159265358979323846 / 180.0;
    CHECK(Near(OffDeg(170 * kRad, 0, 0, std::cos(-170 * kRad), std::sin(-170 * kRad)), 20, 1e-6));
}

void TestPush() {
    Push p;
    CHECK(!p.Done(1000, 15000));
    p.Sample(500, 10, 1);  // before the hand-over: ignored
    CHECK(p.turn_ms < 0 && p.shot_ms < 0 && p.samples == 0);
    p.Owned(1000, 120);
    CHECK(p.start_off == 120 && p.turn_ms < 0 && p.samples == 1);
    p.Sample(1500, 60, 0);
    p.Sample(2200, 25, 0);
    CHECK(p.turn_ms == 1200 && p.shot_ms < 0 && !p.Done(2200, 15000));
    p.Sample(3000, 40, 2);
    CHECK(p.shot_ms == 2000 && p.turn_ms == 1200 && p.Done(3000, 15000));
    CHECK(Near(p.min_off, 25));
    char t[64];
    p.Text(t, sizeof t);
    CHECK(std::strcmp(t, "turned 1200 ms, shot 2000 ms") == 0);
    // already facing at the hand-over: 0 ms; timeout without a shot
    Push q;
    q.Owned(0, 5);
    CHECK(q.turn_ms == 0);
    CHECK(!q.Done(14999, 15000) && q.Done(15000, 15000));
    q.Text(t, sizeof t);
    CHECK(std::strcmp(t, "turned 0 ms, shot never") == 0);
    Push r;
    r.Owned(0, 150);
    r.Sample(1000, 140, 0);
    r.Text(t, sizeof t);
    CHECK(std::strcmp(t, "turned never, shot never") == 0);
    char at[160];
    r.AttackLine(at, sizeof at);
    CHECK(std::strcmp(at, "first attack never, hits on our Sam 0 (shot 0, melee 0; attacks 0; shots 0)") == 0);
    CHECK(r.FirstAttack() == Attack::none && r.FirstAttackMs() < 0);
}

// Round 24: the hits on our Sam by kind (a shot within 2 s before: its rounds; else melee), the bursts, the first attack, the
// whole window
void TestPushHits() {
    // a melee MULE: no shot, a burst of 8 hit messages over two samples, then another swing 1 s later
    Push m;
    m.Owned(10000, 20);
    m.Sample(10100, 15, 0, 0);
    m.Sample(14000, 5, 0, 5);
    CHECK(m.melee_ms == 4000 && m.melee_hits == 5 && m.shot_hits == 0 && m.attacks == 1 && m.hits == 5);
    m.Sample(14100, 5, 0, 8);
    CHECK(m.melee_hits == 8 && m.attacks == 1);  // within 300 ms: the same swing
    m.Sample(15100, 5, 0, 12);
    CHECK(m.melee_hits == 12 && m.attacks == 2 && m.melee_ms == 4000);
    CHECK(m.FirstAttack() == Attack::melee && m.FirstAttackMs() == 4000 && m.shot_ms < 0);
    char at[160];
    m.AttackLine(at, sizeof at);
    CHECK(std::strcmp(at, "first attack after 4000 ms (melee), hits on our Sam 12 (shot 0, melee 12; attacks 2; shots 0)") == 0);
    // the whole window: turned and "shot" does not end it early
    CHECK(!m.Done(29999, 20000, true) && m.Done(30000, 20000, true));
    // a gun MULE: shots, then its rounds hit within 2 s (shot hits); a hit 2.5 s after the last shot rise counts as melee
    Push g;
    g.Owned(0, 120);
    g.Sample(1500, 25, 0, 0);
    g.Sample(2000, 10, 3, 0);
    CHECK(g.shot_ms == 2000 && g.shots == 3 && g.turn_ms == 1500);
    g.Sample(2600, 10, 3, 2);
    CHECK(g.shot_hits == 2 && g.melee_hits == 0 && g.melee_ms < 0 && g.attacks == 1);
    g.Sample(4500, 10, 3, 3);
    CHECK(g.shot_hits == 2 && g.melee_hits == 1 && g.melee_ms == 4500 && g.attacks == 2);
    CHECK(g.FirstAttack() == Attack::shot && g.FirstAttackMs() == 2000);
    CHECK(g.Done(3000, 20000) && !g.Done(3000, 20000, true));  // the old end: turned and shot
    g.AttackLine(at, sizeof at);
    CHECK(std::strcmp(at, "first attack after 2000 ms (shot), hits on our Sam 3 (shot 2, melee 1; attacks 2; shots 3)") == 0);
    // a new hand-over resets every count
    g.Owned(9000, 50);
    CHECK(g.hits == 0 && g.shots == 0 && g.attacks == 0 && g.melee_ms < 0 && g.shot_ms < 0 && g.FirstAttack() == Attack::none);
    // a shot and a melee hit in the same sample: the shot's (and the first attack a shot)
    Push s;
    s.Owned(0, 0);
    s.Sample(700, 0, 1, 1);
    CHECK(s.shot_hits == 1 && s.melee_hits == 0 && s.FirstAttack() == Attack::shot);
    CHECK(std::strcmp(AttackText(Attack::melee), "melee") == 0 && std::strcmp(AttackText(Attack::shot), "shot") == 0 &&
          std::strcmp(AttackText(Attack::none), "none") == 0);
}

void TestPhase() {
    PhaseTrack t;
    CHECK(!t.Feed(100, 0));
    CHECK(t.start == 0 && t.last == 0 && t.changes == 0);
    CHECK(!t.Feed(350, 0));
    CHECK(t.Feed(600, 2));
    CHECK(t.first_raise_ms == 500 && t.changes == 1 && t.max == 2);
    CHECK(t.Feed(850, 1) && t.changes == 2 && t.last == 1 && t.max == 2);
    CHECK(!t.Feed(1100, 1));
    CHECK(t.first_raise_ms == 500);
    // a start above zero: a drop is a change, not a raise
    PhaseTrack d;
    d.Feed(0, 3);
    CHECK(d.Feed(250, 1) && d.first_raise_ms < 0 && d.max == 3);
}

void TestShorten() {
    const double a[3] = {0, 0, 1.6}, b[3] = {10, 0, 1.6};
    double ray[6] = {}, len = 0;
    CHECK(Shorten(a, b, kRayTrimM, ray, &len));
    CHECK(Near(len, 10) && Near(ray[0], 0.6) && Near(ray[3], 9.4) && Near(ray[2], 1.6) && Near(ray[5], 1.6));
    const double c[3] = {1.2, 0, 1.6};
    CHECK(!Shorten(a, c, kRayTrimM, ray, &len) && Near(len, 1.2));
    const double e[3] = {0, 0, 1.6};
    CHECK(!Shorten(a, e, kRayTrimM, ray, nullptr));
}

void TestSigPtr() {
    const std::uint8_t x[4] = {1, 2, 3, 4}, y[4] = {1, 2, 3, 5};
    CHECK(EntrySig(x, 4) == EntrySig(x, 4) && EntrySig(x, 4) != EntrySig(y, 4));
    CHECK(EntrySig(x, 0) == 1469598103934665603ull);
    const std::uint64_t lo = 0x140000000ull, hi = 0x148000000ull;
    CHECK(PlausiblePtr(0x2A0001000ull, lo, hi));
    CHECK(!PlausiblePtr(0x141000000ull, lo, hi));  // in the image
    CHECK(!PlausiblePtr(0x2A0001004ull, lo, hi));  // unaligned
    CHECK(!PlausiblePtr(0x8000ull, lo, hi) && !PlausiblePtr(0x800000000000ull, lo, hi) && !PlausiblePtr(0, lo, hi));
    CHECK(SameActor(0x2029, 0x2029) == 1 && SameActor(0xE0002029u, 0x2029) == 2 && SameActor(0x2028, 0x2029) == 0 && SameActor(5, 0) == 0);
}

void TestKey() {
    bool on = false;
    CHECK(cfgval::SetProbeKey(on, "enemy_m2_probes", true) && on);
    CHECK(cfgval::SetProbeKey(on, "enemy_m2_probes", false) && !on);
    CHECK(!cfgval::SetProbeKey(on, "enemy_m2_probe", true) && !on);
}
void TestRound25() {
    CHECK(std::strcmp(KnowledgeTypeName(0x54), "CPOrder") == 0 && std::strcmp(KnowledgeTypeName(0x5D), "InCombat") == 0);
    CHECK(std::strcmp(KnowledgeTypeName(0x52), "DamagedAtPosition") == 0 && std::strcmp(KnowledgeTypeName(-1), "none") == 0);
    CHECK(LeverName(4)[0] == 'e' && LeverName(5)[0] == 'f' && LeverName(6)[0] == 'g' && std::strcmp(LeverName(7), "?") == 0);
    int l = -1, s = -1;
    CHECK(P2Window(0, &l, &s) && l == 0 && s == 20);
    CHECK(P2Window(3000, &l, &s) && l == 0 && s == 30);
    CHECK(P2Window(2004, &l, &s) && l == 4 && s == 20);
    CHECK(P2Window(9906, &l, &s) && l == 6 && s == 40);
    CHECK(P2Window(3, &l, &s) && l == 3 && s == 20);
    CHECK(!P2Window(7, &l, &s) && !P2Window(-1, &l, &s) && !P2Window(2.5, &l, &s) && !P2Window(3099, &l, &s));
    CHECK(P2StepMs(0) == 250.0 && P2StepMs(2999) == 250.0 && P2StepMs(3000) == 1000.0);
    CHECK(P2Due(0, -1) && !P2Due(100, 0) && P2Due(250, 0) && !P2Due(3500, 3000) && P2Due(4000, 3000));
    PathTrack p;
    char buf[256];
    p.Text(buf, sizeof buf);
    CHECK(std::strcmp(buf, "no samples") == 0 && p.StoppedAt() < 0);
    p.Add(0.0, 0, 0, 4, 90);
    p.Add(1.0, 2, 0, 6, 80);
    p.Add(2.0, 4, 0, 8, 40);
    p.Add(12.0, 24, 0, 28, 20);
    p.Add(13.0, 24.2, 0, 28, 20);
    p.Add(14.0, 24.2, 0, 27, 10);
    CHECK(Near(p.max_d, 28) && Near(p.max_t, 12) && Near(p.max_d10, 8) && Near(p.max_walk, 24.2));
    CHECK(Near(p.StoppedAt(), 12) && Near(p.face_t, 12) && Near(p.ex, 24.2));
    p.Text(buf, sizeof buf);
    CHECK(std::strstr(buf, "stopped at t 12.0") && std::strstr(buf, "facing her at t 12.0"));
    PathTrack q;
    q.Add(0, 0, 0, 5, 50);
    q.Add(1, 3, 0, 5, 50);
    CHECK(q.StoppedAt() < 0 && q.face_t < 0);
    q.Text(buf, sizeof buf);
    CHECK(std::strstr(buf, "moving at the end") && std::strstr(buf, "never facing her"));
    const double pos[3] = {1, 2, 3};
    KnowEntryText(buf, sizeof buf, -1, pos, 0, 0, 0, 0, 0);
    CHECK(std::strcmp(buf, "none") == 0);
    KnowEntryText(buf, sizeof buf, 0x54, pos, 0.5f, -1.0f, 0x2029, 3, 0xAB);
    CHECK(std::strncmp(buf, "CPOrder(0x54) at (1.0,2.0,3.0)", 30) == 0 && std::strstr(buf, "actor 002029"));
    CHECK(KnowListed(1) && KnowListed(0x54) && KnowListed(0x55) && KnowListed(0x5D) && KnowListed(0x51) && KnowListed(0x52) &&
          !KnowListed(0x33));
    // K1
    CHECK(KnowPush(0, 0, false, -1, -1, false, 0, 3) == KnowStep::done);
    CHECK(KnowPush(0, 0, false, -1, -1, false, 1, 3) == KnowStep::write);
    CHECK(KnowPush(10, 0, true, -1, -1, false, 1, 3) == KnowStep::wait);
    CHECK(KnowPush(600, 0, false, 0, -1, false, 1, 3) == KnowStep::done);
    CHECK(KnowPush(300, 0, false, 0, -1, false, 2, 3) == KnowStep::wait);
    CHECK(KnowPush(600, 0, false, 0, -1, false, 2, 3) == KnowStep::write);
    CHECK(KnowPush(600, 0, true, 0, -1, false, 2, 3) == KnowStep::wait);
    CHECK(KnowPush(600, 0, false, 0, -1, true, 2, 3) == KnowStep::done);
    CHECK(KnowPush(1600, 0, false, 0, 500, false, 2, 3) == KnowStep::done);
    CHECK(KnowPush(1400, 0, false, 0, 500, false, 2, 3) == KnowStep::write);
    CHECK(KnowPush(3100, 0, false, 2500, -1, false, 2, 3) == KnowStep::done);
    CHECK(KnowPush(3000, 0, false, 2400, -1, false, 2, 3) == KnowStep::write);
    CHECK(std::strcmp(KnowStepText(KnowStep::wait), "wait") == 0);
    // C1
    CHECK(!CampRefreshWanted(0, 5) && !CampRefreshWanted(1, 3) && CampRefreshWanted(1, 4) && CampRefreshWanted(1, 5));
    CHECK(!CampRefreshWanted(1, -1) && CampRefreshWanted(2, 0) && CampRefreshWanted(2, -1));
    // T1
    const std::uint64_t h[3] = {0x10, 0x20, 0x30};
    CHECK(TicketsToReturn(h, 3, 0x20) == 1 && TicketsToReturn(h, 3, 0x40) == 0 && TicketsToReturn(h, 1, 0x20) == 0);
    CHECK(TicketsToReturn(h, 0, 0x10) == 0 && TicketsToReturn(h, 65, 0x10) == 0 && TicketsToReturn(nullptr, 3, 0x10) == 0);
    CHECK(TicketsToReturn(h, 3, 0) == 0);
    // keys
    cfgval::TakeKeys k;
    // round 26 (docs/enemy-sync.md 14.19): on by default, the tested arms K1 2 and C1 1; config.hpp's defaults the same
    CHECK(k.know == 2 && k.know_secs == 3.0 && k.camp == 1 && k.tickets && k.hold_vehicle && k.rule && k.role && k.face);
    {
        const Config c;
        CHECK(c.enemy_m2_probes && c.enemy_take_know == k.know && c.enemy_take_know_secs == k.know_secs && c.enemy_take_camp == k.camp);
        CHECK(c.enemy_ticket_return == k.tickets && c.enemy_single_hold_vehicle == k.hold_vehicle && c.enemy_single_rule == k.rule);
        CHECK(c.enemy_take_role == k.role && c.enemy_take_face == k.face);
    }
    // each turns off with 0
    for (const char* key : {"enemy_take_know", "enemy_take_camp"}) {
        cfgval::TakeKeys z;
        CHECK(cfgval::SetTakeKey(z, key, "0") && (std::strcmp(key, "enemy_take_know") == 0 ? z.know == 0 : z.camp == 0));
    }
    {
        cfgval::TakeKeys z;
        for (const char* key : {"enemy_ticket_return", "enemy_single_hold_vehicle", "enemy_single_rule", "enemy_take_role", "enemy_take_face"})
            CHECK(cfgval::SetTakeKey(z, key, "0"));
        CHECK(!z.tickets && !z.hold_vehicle && !z.rule && !z.role && !z.face);
    }
    k.know = 0;
    CHECK(cfgval::SetTakeKey(k, "enemy_take_know", "2") && k.know == 2);
    CHECK(cfgval::SetTakeKey(k, "enemy_take_know", "7") && k.know == 2);
    CHECK(cfgval::SetTakeKey(k, "enemy_take_know_secs", "5") && k.know_secs == 5.0);
    CHECK(cfgval::SetTakeKey(k, "enemy_take_know_secs", "99") && k.know_secs == 5.0);
    CHECK(cfgval::SetTakeKey(k, "enemy_take_camp", "1") && k.camp == 1);
    CHECK(cfgval::SetTakeKey(k, "enemy_ticket_return", "1") && k.tickets);
    CHECK(cfgval::SetTakeKey(k, "enemy_single_hold_vehicle", "true") && k.hold_vehicle);
    CHECK(cfgval::SetTakeKey(k, "enemy_single_rule", "1") && k.rule);
    CHECK(cfgval::SetTakeKey(k, "enemy_single_rule", "0") && !k.rule);
    CHECK(!cfgval::SetTakeKey(k, "enemy_take", "1"));
}
}  // namespace

void TestRound25e() {
    CHECK(RoleWhileHers(1, 1) == 0 && RoleWhileHers(1, 0) == -1 && RoleWhileHers(1, 2) == -1);
    CHECK(RoleWhileHers(0, 1) == -1 && RoleWhileHers(2, 1) == -1 && RoleWhileHers(1, -1) == -1 && RoleWhileHers(1, 8) == -1);
    double y = 99;
    // facing +x (yaw 0), Sam 10 m behind (-x): turned to pi
    CHECK(FaceYaw(1, 0.0, 0, 0, -10, 0, &y) && std::fabs(std::fabs(y) - 3.14159265) < 1e-6);
    // Sam 10 m to the left (+y), 90 deg off: turned to pi/2
    CHECK(FaceYaw(1, 0.0, 0, 0, 0, 10, &y) && std::fabs(y - 1.5707963) < 1e-6);
    y = 99;
    CHECK(!FaceYaw(1, 0.0, 0, 0, 10, 5, &y) && y == 99);   // 27 deg off: no turn
    CHECK(!FaceYaw(1, 0.0, 0, 0, -31, 0, &y));             // over 30 m
    CHECK(!FaceYaw(1, 0.0, 0, 0, -0.3, 0, &y));            // under 0.5 m
    CHECK(!FaceYaw(0, 0.0, 0, 0, -10, 0, &y) && !FaceYaw(2, 0.0, 0, 0, -10, 0, &y) && !FaceYaw(1, 0.0, 0, 0, -10, 0, nullptr));
    CHECK(!FaceYaw(1, std::nan(""), 0, 0, -10, 0, &y));
}
// round 26 review: a single take is a forced take, alone in its camp here and alone in its OWN batch
void TestRound26() {
    CHECK(TakeAlone(true, true, 0, 1));    // enemy_own <id> 3
    CHECK(TakeAlone(true, true, 0, 0));    // (a batch count of 0 only outside OwnDrain: never more than one)
    CHECK(!TakeAlone(false, true, 0, 1));  // an ordinary (proximity) take
    CHECK(!TakeAlone(true, false, 0, 1));  // its camp not readable here
    CHECK(!TakeAlone(true, true, 1, 1));   // another member of its camp hers already
    CHECK(!TakeAlone(true, true, 0, 3));   // the first member of a forced whole-camp take (enemy_own .. 2, R1's camp verdict)
}

int main() {
    TestNames();
    TestFresh();
    TestAngles();
    TestPush();
    TestPushHits();
    TestPhase();
    TestShorten();
    TestSigPtr();
    TestKey();
    TestRound25();
    TestRound25e();
    TestRound26();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
