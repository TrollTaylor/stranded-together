// Round 23 E (native/mod/enemy_probe_rules.hpp): the M2 probes' freshness, angles, push and phase trackers, ray trim, key.
#include <cstdio>
#include <cstring>

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
}  // namespace

int main() {
    TestNames();
    TestFresh();
    TestAngles();
    TestPush();
    TestPhase();
    TestShorten();
    TestSigPtr();
    TestKey();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
