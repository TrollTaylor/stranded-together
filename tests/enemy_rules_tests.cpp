// Enemy sync P1a (native/mod/enemy_rules.hpp): the schedule, the pose slots, the interpolation, the band, entry, the lease (and
// the cutscene hold), the lock-free entity set, the enemy shots' gate, the damage policy and the fingerprint gate. No game.
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
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
    // M0 "no statues" (round 20): the host's copy unloaded or hidden there, or its place far from ours at entry
    x = e, x.host_realized = false;
    CHECK(Eligible(x) == Refusal::host_unrealized);
    CHECK(std::strstr(RefusalText(Refusal::host_unrealized), "host") != nullptr);
    x = e, x.host_hidden = true;
    CHECK(Eligible(x) == Refusal::host_hidden);
    x = e, x.entry_err_max_m = 30, x.entry_err_m = 30.0;
    CHECK(Eligible(x) == Refusal::ok);
    x.entry_err_m = 30.1;
    CHECK(Eligible(x) == Refusal::entry_far);
    x.entry_err_m = 198.58;  // the user's 02039 at 140.67
    CHECK(Eligible(x) == Refusal::entry_far);
    x.entry_err_max_m = 0;  // off
    CHECK(Eligible(x) == Refusal::ok);
    x = e, x.entry_err_max_m = 30, x.entry_err_m = -1;  // unknown: not refused
    CHECK(Eligible(x) == Refusal::ok);
    // the local refusals keep their order before the host's
    x = e, x.host_realized = false, x.hidden_or_asleep = true;
    CHECK(Eligible(x) == Refusal::hidden_or_asleep);
    x = e, x.host_realized = false, x.host_hidden = true;
    CHECK(Eligible(x) == Refusal::host_unrealized);
    // M0's quick path (enemy_host_far): the host's Sam beyond the distance (2035 m after the statues gate's teleport)
    x = e, x.host_far_m = 1500, x.host_dist_m = 2035;
    CHECK(Eligible(x) == Refusal::host_far);
    CHECK(std::strstr(RefusalText(Refusal::host_far), "enemy_host_far") != nullptr);
    x.host_dist_m = 1500;
    CHECK(Eligible(x) == Refusal::ok);
    x.host_dist_m = -1;  // unknown (no partner body here): not refused
    CHECK(Eligible(x) == Refusal::ok);
    x = e, x.host_far_m = 0, x.host_dist_m = 5000;  // off
    CHECK(Eligible(x) == Refusal::ok);
    CHECK(HostFar(1500.1, 1500) && !HostFar(1500, 1500) && !HostFar(-1, 1500) && !HostFar(9999, 0));
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
    CHECK(cfgval::EntryErrMax("30", 30.0) == 30.0 && cfgval::EntryErrMax("0", 30.0) == 0.0 && cfgval::EntryErrMax("5", 30.0) == 5.0 &&
          cfgval::EntryErrMax("1000", 30.0) == 1000.0);
    CHECK(cfgval::EntryErrMax("4", 30.0) == 30.0 && cfgval::EntryErrMax("1001", 30.0) == 30.0 && cfgval::EntryErrMax("", 30.0) == 30.0 &&
          cfgval::EntryErrMax("30m", 30.0) == 30.0 && cfgval::EntryErrMax("-5", 30.0) == 30.0);
    CHECK(cfgval::HostFar("1500", 1500.0) == 1500.0 && cfgval::HostFar("0", 1500.0) == 0.0 && cfgval::HostFar("300", 1500.0) == 300.0 &&
          cfgval::HostFar("10000", 1500.0) == 10000.0 && cfgval::HostFar("299", 1500.0) == 1500.0 && cfgval::HostFar("10001", 1500.0) == 1500.0 &&
          cfgval::HostFar("", 1500.0) == 1500.0 && cfgval::HostFar("1km", 1500.0) == 1500.0);
    CHECK(cfgval::PoseCount("0", 6) == 0 && cfgval::PoseCount("8", 6) == 8 && cfgval::PoseCount("9", 6) == 6 && cfgval::PoseCount("-1", 6) == 6 &&
          cfgval::PoseCount("", 6) == 6 && cfgval::PoseCount("4x", 6) == 6);
    // P2
    CHECK(cfgval::HitOwner("credit", 1) == 0 && cfgval::HitOwner("body", 0) == 1 && cfgval::HitOwner("none", 0) == 2 && cfgval::HitOwner("sam", 0) == 3 &&
          cfgval::HitOwner("player", 0) == 0 && cfgval::HitOwner("", 1) == 1);
    CHECK(std::strcmp(cfgval::HitOwnerName(0), "credit") == 0 && std::strcmp(cfgval::HitOwnerName(3), "sam") == 0);
    CHECK(cfgval::HitPath("direct", 1) == 0 && cfgval::HitPath("query", 0) == 1 && cfgval::HitPath("sphere", 0) == 0);
    CHECK(cfgval::HitRate("30", 10) == 30 && cfgval::HitRate("1", 10) == 1 && cfgval::HitRate("120", 10) == 120 && cfgval::HitRate("0", 10) == 10 &&
          cfgval::HitRate("121", 10) == 10 && cfgval::HitRate("3x", 10) == 10);
    // the defaults once every line is read: enemy_sync follows host_save, enemy_hits follows enemy_sync, enemy_owner off unless given
    for (int hs = 0; hs < 2; ++hs)
        for (int ss = 0; ss < 2; ++ss)
            for (int sv = 0; sv < 2; ++sv)
                for (int hset = 0; hset < 2; ++hset)
                    for (int hv = 0; hv < 2; ++hv) {
                        const cfgval::EnemyKeys k = cfgval::EnemyDefaults(hs != 0, ss != 0, sv != 0, hset != 0, hv != 0, false, true);
                        const bool sync = ss ? sv != 0 : hs != 0;
                        CHECK(k.sync == sync);
                        CHECK(k.hits == (hset ? hv != 0 : sync));
                        CHECK(k.owner == (cfgval::kOwnerDefaultOn && k.hits));  // unset: the default (whatever the field held)
                    }
    CHECK(cfgval::EnemyDefaults(true, false, false, false, false, true, true).owner && !cfgval::EnemyDefaults(true, false, false, false, false, true, false).owner);
    CHECK(cfgval::EnemyDefaults(true, false, false, false, false, false, false).hits);   // host_save alone: sync and hits on
    CHECK(!cfgval::EnemyDefaults(true, true, false, false, true, false, false).hits);    // enemy_sync=0: hits follow it off
    CHECK(cfgval::EnemyDefaults(false, false, false, true, true, false, false).hits);    // enemy_hits=1 given: kept
    // the P1c keys: each on by default, each set alone, an unknown key untouched (review rv-correctness N9)
    {
        const cfgval::OwnerKeys d;
        CHECK(d.outcomes && d.follow_guard && d.attack_registry && d.hits);
        const char* const keys[4] = {"enemy_owner_outcomes", "enemy_follow_guard", "enemy_attack_registry", "enemy_owner_hits"};
        for (int i = 0; i < 4; ++i) {
            cfgval::OwnerKeys k;
            CHECK(cfgval::SetOwnerKey(k, keys[i], false));
            CHECK(k.outcomes == (i != 0) && k.follow_guard == (i != 1) && k.attack_registry == (i != 2) && k.hits == (i != 3));
            CHECK(cfgval::SetOwnerKey(k, keys[i], true) && k.outcomes && k.follow_guard && k.attack_registry && k.hits);
        }
        cfgval::OwnerKeys u;
        CHECK(!cfgval::SetOwnerKey(u, "enemy_owner", false) && !cfgval::SetOwnerKey(u, "enemy_owner_outcome", false) && u.outcomes && u.hits);
    }
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
    // M0 "no statues": the host unloaded or hid its copy (the newest frame, within the silence window)
    x = l, x.host_realized = false;
    CHECK(Lease(x) == LeaseVerdict::keep);  // the guard off: the bits are not read
    x.host_guard = true;
    CHECK(Lease(x) == LeaseVerdict::release_host_unrealized);
    CHECK(Releases(Lease(x)) && !HideOnRelease(Lease(x), true, true));  // given back alive: her own game runs it
    CHECK(std::strcmp(LeaseText(Lease(x)), "host unrealized") == 0);
    x = l, x.host_guard = true, x.host_hidden = true;
    CHECK(Lease(x) == LeaseVerdict::release_host_hidden);
    CHECK(Releases(Lease(x)) && HideOnRelease(Lease(x), true, true));  // hidden here while hidden there (enemy_host_hide 1)
    CHECK(!HideOnRelease(Lease(x), false, true));  // enemy_host_hide 0 (the default): given back alive
    CHECK(!HideOnRelease(Lease(x), true, false));  // held by a cutscene (still frozen): never hidden (review r20 enemy 3)
    x = l, x.host_guard = true, x.host_realized = false, x.host_hidden = true;
    CHECK(Lease(x) == LeaseVerdict::release_host_unrealized);
    x = l, x.host_guard = true, x.host_realized = false, x.frame_age_ms = 1600;
    CHECK(Lease(x) == LeaseVerdict::release_silence);  // silence first (the same give-back)
    x = l, x.host_guard = true, x.host_realized = false, x.bit14_now = false;
    CHECK(Lease(x) == LeaseVerdict::forget_taken);
    x = l, x.host_guard = true, x.host_hidden = true, x.hold = true;
    CHECK(Lease(x) == LeaseVerdict::hold);
    x = l, x.host_guard = true;
    CHECK(Lease(x) == LeaseVerdict::keep);
    // M0's quick path: the host's Sam far (his frames stopped while his world loads; the silence window would take 3 s)
    x = l, x.host_guard = true, x.host_far_m = 1500, x.host_dist_m = 2064.6;
    CHECK(Lease(x) == LeaseVerdict::release_host_far);
    CHECK(Releases(Lease(x)) && !HideOnRelease(Lease(x), true, true));  // given back alive
    CHECK(std::strcmp(LeaseText(Lease(x)), "host far") == 0);
    x.host_guard = false;  // the guard off: not read
    CHECK(Lease(x) == LeaseVerdict::keep);
    x = l, x.host_guard = true, x.host_far_m = 1500, x.host_dist_m = 900;
    CHECK(Lease(x) == LeaseVerdict::keep);
    x = l, x.host_guard = true, x.host_far_m = 0, x.host_dist_m = 5000;  // enemy_host_far=0
    CHECK(Lease(x) == LeaseVerdict::keep);
    x = l, x.host_guard = true, x.host_far_m = 1500, x.host_dist_m = 2064.6, x.host_realized = false;
    CHECK(Lease(x) == LeaseVerdict::release_host_unrealized);  // the host's own bits first
    CHECK(!HideOnRelease(LeaseVerdict::release_silence, true, true) && !HideOnRelease(LeaseVerdict::release_range, true, true));
    // a copy hidden for the host: kept hidden only while the host's fresh frame says realized and hidden
    CHECK(KeepHostHidden(true, 100, true, true));
    CHECK(!KeepHostHidden(true, 100, true, false));   // shown there again
    CHECK(!KeepHostHidden(true, 100, false, true));   // unloaded there: hers is given back
    CHECK(!KeepHostHidden(true, 3001, true, true));   // the frames stopped
    CHECK(KeepHostHidden(true, 3000, true, true));
    CHECK(!KeepHostHidden(false, 0, true, true));     // no track
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


// ---------------------------------------------------------------- P2 (round 17): hits and outcomes
void TestHitVerdict() {
    CHECK(AttackOk(1) && AttackOk(0x47) && AttackOk(0x18C) && !AttackOk(0x18D) && !AttackOk(0) && !AttackOk(0x180) && !AttackOk(0x18E));
    CHECK(HitVerdict(false, true, true) == HitV::pass);    // not a mirror: the game's
    CHECK(HitVerdict(false, false, false) == HitV::pass);
    CHECK(HitVerdict(true, true, true) == HitV::capture);  // hers on a mirror, hits on: dropped and sent
    CHECK(HitVerdict(true, true, false) == HitV::drop);    // hits off: dropped (the host decides)
    CHECK(HitVerdict(true, false, true) == HitV::drop);    // another's hit on a mirror: dropped, never sent
    CHECK(HitVerdict(true, false, false) == HitV::drop);
    CHECK(LethalRefused(true, false) && !LethalRefused(true, true) && !LethalRefused(false, false));
}
void TestHitDedupe() {
    HitDedupe d;
    CHECK(!d.Seen(0x2001, 77, 0x47, 1000));
    CHECK(d.Seen(0x2001, 77, 0x47, 1200));    // within 300 ms
    CHECK(!d.Seen(0x2001, 77, 0x48, 1200));   // another attack
    CHECK(!d.Seen(0x2002, 77, 0x47, 1200));   // another enemy
    CHECK(!d.Seen(0x2001, 77, 0x47, 1301));   // past the window: new (remembered again)
    CHECK(d.Seen(0x2001, 77, 0x47, 1500));
    CHECK(!d.Seen(0x2001, 0, 0x47, 2000) && !d.Seen(0x2001, 0, 0x47, 2000));  // serial 0 never dedupes
    d.Reset();
    CHECK(!d.Seen(0x2001, 77, 0x47, 1550));
}
void TestFrames() {
    const double root[3] = {1500.0, 990.0, 170.0};
    const double p[3] = {1501.0, 990.5, 171.2};
    for (double yaw : {0.0, 1.0, -2.5, 3.14159, -3.14159, 7.0}) {
        double l[3], w[3];
        ToLocal(root, yaw, p, l);
        ToWorld(root, yaw, l, w);
        CHECK(std::fabs(w[0] - p[0]) < 1e-9 && std::fabs(w[1] - p[1]) < 1e-9 && std::fabs(w[2] - p[2]) < 1e-9);
        CHECK(std::fabs(l[2] - 1.2) < 1e-9 && std::fabs(std::hypot(l[0], l[1]) - std::hypot(1.0, 0.5)) < 1e-9);
    }
    // the same local point on a copy turned by yaw + 2 pi (the wrap) lands at the same place
    double l[3], a[3], b[3];
    ToLocal(root, 0.3, p, l);
    ToWorld(root, 0.3, l, a);
    ToWorld(root, 0.3 + 2 * kPi, l, b);
    CHECK(std::fabs(a[0] - b[0]) < 1e-9 && std::fabs(a[1] - b[1]) < 1e-9);
    // a point 1 m ahead (its forward row = (cos yaw, sin yaw)) is local +x
    const double yaw = 1.2;
    const double ahead[3] = {root[0] + std::cos(yaw), root[1] + std::sin(yaw), root[2]};
    ToLocal(root, yaw, ahead, l);
    CHECK(std::fabs(l[0] - 1) < 1e-9 && std::fabs(l[1]) < 1e-9);
    // the host's copy turned 90 deg: the hit follows the body (a body shot stays a body shot)
    const double host_root[3] = {1500.5, 990.2, 170.0};
    double w[3];
    ToWorld(host_root, yaw + kPi / 2, l, w);
    CHECK(std::fabs(w[0] - (host_root[0] + std::cos(yaw + kPi / 2))) < 1e-9 && std::fabs(w[1] - (host_root[1] + std::sin(yaw + kPi / 2))) < 1e-9);
}
void TestHostSerial() {
    CHECK(HostSerial(0x1234, 9) == 0x5C001234u);
    CHECK(HostSerial(0x1234, 10) == HostSerial(0x1234, 9));  // the same guest serial, the same host serial
    CHECK(HostSerial(0xAB123456u, 1) == 0x5C123456u);
    CHECK(HostSerial(0, 5) == 0x5D000005u && HostSerial(0, 6) != HostSerial(0, 5));
    CHECK(HostSerial(0, 0) != 0 && HostSerial(0, 0x1000000) != 0);
}
void TestRateLimit() {
    RateLimit r;
    int ok = 0;
    for (int i = 0; i < 40; ++i) ok += r.Allow(0x2001, 1000.0 + i, 30, 60) ? 1 : 0;
    CHECK(ok == 30);                          // 30 a second per enemy
    for (int i = 0; i < 40; ++i) ok += r.Allow(0x2002, 1100.0 + i, 30, 60) ? 1 : 0;
    CHECK(ok == 60);                          // 60 in all
    CHECK(!r.Allow(0x2003, 1500, 30, 60));
    CHECK(r.Allow(0x2001, 2100, 30, 60));     // a second later: room again
}
void TestApplyGate() {
    ApplyIn a;
    CHECK(ApplyGate(a) == HitResult::applied);
    ApplyIn b = a;
    b.host = false;
    CHECK(ApplyGate(b) == HitResult::off);
    b = a, b.mirroring = false;
    CHECK(ApplyGate(b) == HitResult::off);
    b = a, b.hits_on = false;
    CHECK(ApplyGate(b) == HitResult::off);
    b = a, b.fp_equal = false;
    CHECK(ApplyGate(b) == HitResult::world);
    b = a, b.attack_ok = false;
    CHECK(ApplyGate(b) == HitResult::attack);
    b = a, b.kind_ok = false;  // a test HIT outside a hidden run
    CHECK(ApplyGate(b) == HitResult::attack);
    b = a, b.values_ok = false;
    CHECK(ApplyGate(b) == HitResult::values);
    // the host's own lethal setting decides
    b = a, b.lethal = true, b.lethal_allowed = false;
    CHECK(ApplyGate(b) == HitResult::lethal);
    b = a, b.lethal = true, b.lethal_allowed = true;
    CHECK(ApplyGate(b) == HitResult::applied);
    b = a, b.lethal = false, b.lethal_allowed = false;
    CHECK(ApplyGate(b) == HitResult::applied);
    CHECK(std::strcmp(HitResultText(static_cast<std::uint8_t>(HitResult::lethal)), "lethal") == 0 &&
          std::strcmp(HitResultText(static_cast<std::uint8_t>(HitResult::values)), "values") == 0 && std::strcmp(HitResultText(kHitResults), "?") == 0);
    b = a, b.found = false;
    CHECK(ApplyGate(b) == HitResult::unrealized);
    b = a, b.realized = false;
    CHECK(ApplyGate(b) == HitResult::unrealized);
    b = a, b.state = 3;
    CHECK(ApplyGate(b) == HitResult::dead);
    b = a, b.state = 5;
    CHECK(ApplyGate(b) == HitResult::dead);
    b = a, b.state = 4;  // down: still hit (a fainted MULE takes hits; the game decides)
    CHECK(ApplyGate(b) == HitResult::applied);
    b = a, b.hidden_or_asleep = true;
    CHECK(ApplyGate(b) == HitResult::hidden);
    b = a, b.held = true;
    CHECK(ApplyGate(b) == HitResult::hidden);
    b = a, b.rate_ok = false;
    CHECK(ApplyGate(b) == HitResult::rate);
    b = a, b.point_m = 3.01;
    CHECK(ApplyGate(b) == HitResult::too_far);
    b = a, b.point_m = std::nan("");
    CHECK(ApplyGate(b) == HitResult::too_far);
    CHECK(std::strcmp(HitResultText(0), "applied") == 0 && std::strcmp(HitResultText(10), "hidden") == 0 && std::strcmp(HitResultText(14), "?") == 0);
}
void TestOutcomes() {
    for (std::uint32_t st : {1u, 2u}) CHECK(OutcomeOf(st, kDownFaint | kDownSleep) == Defeat::none);
    for (std::uint32_t st : {3u, 5u}) {
        CHECK(OutcomeOf(st, 0) == Defeat::dead);
        CHECK(OutcomeOf(st, kDownFaint) == Defeat::dead);
    }
    CHECK(OutcomeOf(4, kDownFaint) == Defeat::faint);
    CHECK(OutcomeOf(4, kDownSleep) == Defeat::sleep);
    CHECK(OutcomeOf(4, kDownRestrained) == Defeat::restrained);
    CHECK(OutcomeOf(4, kDownOther) == Defeat::other);
    CHECK(OutcomeOf(4, 0) == Defeat::other);
    CHECK(OutcomeOf(4, kDownFaint | kDownRestrained) == Defeat::faint);
    CHECK(DownBits(kDownFaint) == 1 && DownBits(kDownSleep) == 2 && DownBits(kDownRestrained) == 4 && DownBits(kDownOther) == 0x20 && DownBits(0x400) == 0);
    // the guest's merge
    OutcomeRec r;
    CHECK(MergeOutcome(r, 1, 1, 1) && r.defeat == 1 && r.ver == 1);   // none -> faint
    CHECK(!MergeOutcome(r, 0, 1, 0));                                   // the same version: ignored
    CHECK(!MergeOutcome(r, 0, 0, 0));                                   // older: ignored
    CHECK(MergeOutcome(r, 0, 2, 0) && r.defeat == 0);                   // faint -> none (it got up)
    CHECK(MergeOutcome(r, 4, 3, 1) && r.defeat == 4);                   // dead
    CHECK(!MergeOutcome(r, 0, 4, 0) && r.defeat == 4 && r.ver == 3);    // dead is sticky
    CHECK(!MergeOutcome(r, 1, 5, 2) && r.defeat == 4);                  // a resync cannot lift it either
    CHECK(MergeOutcome(r, 0, 6, 3) && r.defeat == 0);                   // only a revive
    OutcomeRec w;
    w.have = true, w.defeat = 1, w.ver = 65535;
    CHECK(MergeOutcome(w, 0, 0, 0));                                    // the version wraps
    CHECK(!MergeOutcome(w, 9, 1, 0));                                   // a defeat beyond 5
    CHECK(std::strcmp(DefeatText(1), "faint") == 0 && std::strcmp(DefeatText(4), "dead") == 0 && std::strcmp(CauseText(1), "guest") == 0);
}
void TestDeadRelease() {
    CHECK(DeadRelease(static_cast<std::uint8_t>(Defeat::dead)) == ReleaseAs::hide_sleep);
    for (int d : {0, 1, 2, 3, 5}) CHECK(DeadRelease(static_cast<std::uint8_t>(d)) == ReleaseAs::alive);
}
void TestPoseDead() {
    // a dead or down enemy is still posed (realized), in the 5 Hz half whatever its rank, and it does not take a live one's fast rank
    PoseSlots s;
    PoseCand c[5] = {PoseCand{0x2001, 10, true, true}, PoseCand{0x2002, 20, true}, PoseCand{0x2003, 30, true}, PoseCand{0x2004, 40, true},
                     PoseCand{0x2005, 50, true}};
    s.Update(c, 5, 6);
    CHECK(s.Used() == 5 && s.SlotOf(0x2001) >= 0);
    CHECK(s.Period(static_cast<unsigned>(s.SlotOf(0x2001))) == kPoseSlowMs);
    CHECK(s.Period(static_cast<unsigned>(s.SlotOf(0x2002))) == kPoseFastMs && s.Period(static_cast<unsigned>(s.SlotOf(0x2003))) == kPoseFastMs);
    CHECK(s.Period(static_cast<unsigned>(s.SlotOf(0x2004))) == kPoseFastMs);  // the third live one
    CHECK(s.Period(static_cast<unsigned>(s.SlotOf(0x2005))) == kPoseSlowMs);
    // two down near her: the three nearest live ones still fast
    PoseCand d[5] = {PoseCand{0x3001, 5, true, true}, PoseCand{0x3002, 6, true, true}, PoseCand{0x3003, 30, true}, PoseCand{0x3004, 40, true},
                     PoseCand{0x3005, 50, true}};
    PoseSlots t;
    t.Update(d, 5, 6);
    for (std::uint32_t id : {0x3003u, 0x3004u, 0x3005u}) CHECK(t.Period(static_cast<unsigned>(t.SlotOf(id))) == kPoseFastMs);
    for (std::uint32_t id : {0x3001u, 0x3002u}) CHECK(t.Period(static_cast<unsigned>(t.SlotOf(id))) == kPoseSlowMs);
}
// Round 23: the tie-up relay's decisions (docs/enemy-sync.md 14).
void TestRestrain() {
    // what a captured HIT starts
    CHECK(RestrainTrigger(0x15C, 3) == RestrainTrig::strand);
    CHECK(RestrainTrigger(0x47, 41) == RestrainTrig::strand);  // the strand's weapon, whatever its attack
    CHECK(RestrainTrigger(0x102, 45) == RestrainTrig::bola);
    CHECK(RestrainTrigger(0x47, 45) == RestrainTrig::none);
    // the fallback's evidence: attack 0x15C AND weapon 41 at contact; a test waives weapon and distance
    CHECK(RestrainFallback(0x15C, 41, 0.0, false));
    CHECK(RestrainFallback(0x15C, 41, 2.5, false));
    CHECK(!RestrainFallback(0x15C, 41, 2.6, false));
    CHECK(!RestrainFallback(0x15C, 45, 0.0, false));
    CHECK(!RestrainFallback(0x102, 41, 0.0, false));
    CHECK(!RestrainFallback(0x15C, 41, -1.0, false));
    CHECK(RestrainFallback(0x15C, 0, 50.0, true));
    CHECK(!RestrainFallback(0x102, 0, 0.0, true));
    // when to send
    const std::uint8_t none = 0, faint = 1, sleep = 2, restrained = 3, dead = 4;
    WatchIn w;
    w.held_hit = true;
    w.age_ms = 100;
    CHECK(RestrainWatch(w) == WatchDo::wait);
    w.copy = restrained;
    CHECK(RestrainWatch(w) == WatchDo::send_restrain);  // the native read: bit 21
    w.copy_was = restrained;
    CHECK(RestrainWatch(w) == WatchDo::wait);  // no edge: it was restrained before
    w.copy_was = none, w.copy = faint;
    CHECK(RestrainWatch(w) == WatchDo::send_faint);
    w.copy = sleep;
    CHECK(RestrainWatch(w) == WatchDo::wait);  // sleep is not ours to send
    w.copy = restrained, w.owner = faint;
    CHECK(RestrainWatch(w) == WatchDo::drop);  // the owner has it down already
    w.owner = none, w.frozen = false;
    CHECK(RestrainWatch(w) == WatchDo::drop);  // no longer a copy here
    w.frozen = true, w.copy = none, w.age_ms = 1499;
    CHECK(RestrainWatch(w) == WatchDo::wait);
    w.age_ms = 1500;
    CHECK(RestrainWatch(w) == WatchDo::release);  // nothing seen, no fallback: the held HIT goes as before
    w.fallback = true;
    CHECK(RestrainWatch(w) == WatchDo::fallback);
    w.held_hit = false, w.fallback = false;
    CHECK(RestrainWatch(w) == WatchDo::drop);  // a scan's watch with nothing seen
    w.copy = dead;
    CHECK(RestrainWatch(w) == WatchDo::drop);  // dead is never sent as a restrain
    // the owner's gate (the validator: kind 5 only from a peer with the part)
    RestrainIn r;
    CHECK(RestrainGate(r) == HitResult::applied);
    r.peer_part = false;
    CHECK(RestrainGate(r) == HitResult::attack);
    r = RestrainIn{}, r.on = false;
    CHECK(RestrainGate(r) == HitResult::off);
    r = RestrainIn{}, r.k_ok = false;
    CHECK(RestrainGate(r) == HitResult::attack);
    r = RestrainIn{}, r.fp_equal = false;
    CHECK(RestrainGate(r) == HitResult::world);
    r = RestrainIn{}, r.owned = false;
    CHECK(RestrainGate(r) == HitResult::receiver);  // not that game's (the other owns it)
    r = RestrainIn{}, r.found = false;
    CHECK(RestrainGate(r) == HitResult::unrealized);  // not that MULE
    r = RestrainIn{}, r.realized = false;
    CHECK(RestrainGate(r) == HitResult::unrealized);
    r = RestrainIn{}, r.state = 3;
    CHECK(RestrainGate(r) == HitResult::dead);
    r = RestrainIn{}, r.state = 5;
    CHECK(RestrainGate(r) == HitResult::dead);
    r = RestrainIn{}, r.hidden_or_asleep = true;
    CHECK(RestrainGate(r) == HitResult::hidden);
    r = RestrainIn{}, r.defeat_now = faint;
    CHECK(RestrainGate(r) == HitResult::down);  // already down
    r.defeat_now = restrained;
    CHECK(RestrainGate(r) == HitResult::down);
    CHECK(RestrainSetResult(1) == HitResult::applied && RestrainSetResult(0) == HitResult::applied && RestrainSetResult(-2) == HitResult::down &&
          RestrainSetResult(-1) == HitResult::fault);
    CHECK(kHitResults == 14 && std::strcmp(HitResultText(13), "down already") == 0 && std::strcmp(HitResultText(14), "?") == 0);
    // the hold
    CHECK(HoldStep(100, false, false) == HoldEnd::none);
    CHECK(HoldStep(100, true, false) == HoldEnd::outcome);
    CHECK(HoldStep(100, true, true) == HoldEnd::refused);
    CHECK(HoldStep(2999, false, false) == HoldEnd::none);
    CHECK(HoldStep(3000, false, false) == HoldEnd::timeout);
    CHECK(BlendBack(-5) == 0.0f && BlendBack(125) == 0.5f && BlendBack(250) == 1.0f && BlendBack(1000) == 1.0f);
    // the replay flush: only on the edge into a down outcome
    CHECK(FlushOnDefeat(none, faint) && FlushOnDefeat(none, sleep) && FlushOnDefeat(none, restrained) && FlushOnDefeat(none, dead) && FlushOnDefeat(none, 5));
    CHECK(!FlushOnDefeat(none, none) && !FlushOnDefeat(restrained, dead) && !FlushOnDefeat(faint, none) && !FlushOnDefeat(none, 6));
    // the ini keys
    cfgval::RestrainKeys k;
    CHECK(k.relay && k.flush);
    CHECK(cfgval::SetRestrainKey(k, "enemy_restrain_relay", false) && !k.relay && k.flush);
    CHECK(cfgval::SetRestrainKey(k, "enemy_replay_flush", false) && !k.flush);
    CHECK(!cfgval::SetRestrainKey(k, "enemy_restrain", true));
}

void TestHitValues() {
    const std::uint32_t p1 = 0x3F800000u, p125 = 0x41480000u;  // 1.0, 12.5
    CHECK(HitValuesOk(0, 1, 1, p125, 5) && HitValuesOk(0, 0.11f, 0.4f, p125, 0xFFFFFFFFu) && HitValuesOk(0, 1, 1, p1, 1));
    CHECK(HitValuesOk(kHitAmountMax, kHitMultMax, kHitA20Max, 0x42C80000u, kHitPartMax - 1));  // the limits themselves (power 100)
    CHECK(!HitValuesOk(-1, 1, 1, p1, 1) && !HitValuesOk(kHitAmountMax + 1, 1, 1, p1, 1));
    CHECK(!HitValuesOk(0, 1e6f, 1, p1, 1) && !HitValuesOk(0, -0.1f, 1, p1, 1));
    CHECK(!HitValuesOk(0, 1, 2.5f, p1, 1) && !HitValuesOk(0, 1, -1, p1, 1));
    CHECK(!HitValuesOk(0, 1, 1, 0x42CA0000u, 1) && !HitValuesOk(0, 1, 1, 0xBF800000u, 1) && !HitValuesOk(0, 1, 1, 0x7FC00000u, 1));  // 101, -1, NaN
    CHECK(!HitValuesOk(0, 1, 1, p1, kHitPartMax) && !HitValuesOk(0, 1, 1, p1, 0x80000000u));
    CHECK(!HitValuesOk(std::nanf(""), 1, 1, p1, 1) && !HitValuesOk(0, std::nanf(""), 1, p1, 1) && !HitValuesOk(0, 1, std::nanf(""), p1, 1));
}

// ---------------------------------------------------------------- P1b: ownership
OwnIn TakeIn() {
    OwnIn in;
    in.owner = Owner::host;
    in.d_g = 12, in.d_h = 48, in.phase = 0, in.cp_phase = 0;
    return in;
}
OwnIn GuestIn() {
    OwnIn in;
    in.owner = Owner::guest;
    in.d_g = 12, in.d_h = 48, in.phase = 0;
    in.silence_ms = 100, in.range_m = 270;
    return in;
}
void TestHostDecideTake() {
    CHECK(HostDecide(TakeIn()).act == OwnAct::take && HostDecide(TakeIn()).reason == OwnReason::proximity);
    // every condition alone
    auto refused = [](void (*f)(OwnIn&)) {
        OwnIn in = TakeIn();
        f(in);
        return HostDecide(in).act == OwnAct::none;
    };
    CHECK(refused([](OwnIn& i) { i.d_g = 40.1; }));
    CHECK(!refused([](OwnIn& i) { i.d_g = 40.0, i.d_h = 60; }));
    CHECK(refused([](OwnIn& i) { i.d_g = -1; }));             // unknown
    CHECK(refused([](OwnIn& i) { i.d_h = -1; }));             // unknown
    CHECK(refused([](OwnIn& i) { i.d_h = 26.9; }));            // the tie: 12 + 15 = 27
    CHECK(!refused([](OwnIn& i) { i.d_h = 27.0; }));           // exactly the margin takes (the host wins only below it)
    CHECK(refused([](OwnIn& i) { i.d_g = 3, i.d_h = 25.0; })); // the host within 25 m: never (the overlap, plan 8.2)
    CHECK(!refused([](OwnIn& i) { i.d_g = 3, i.d_h = 25.1; }));
    CHECK(refused([](OwnIn& i) { i.d_g = 3, i.d_h = 19; }));   // host 19 m, guest 3 m: no take (back would win)
    CHECK(!refused([](OwnIn& i) { i.phase = 1; }));            // Return: calm
    CHECK(!refused([](OwnIn& i) { i.phase = 2; }));            // PreCaution: calm
    CHECK(refused([](OwnIn& i) { i.phase = 3; }));
    CHECK(!refused([](OwnIn& i) { i.cp_phase = 1; }));
    CHECK(Calm(0) && Calm(1) && Calm(2) && !Calm(3) && !Calm(kPhaseUnknown));
    {
        OwnIn w = TakeIn();
        CHECK(TakeBlock(w) == nullptr);
        w.d_h = 20;
        CHECK(TakeBlock(w) && std::strcmp(TakeBlock(w), "the host not 15 m farther") == 0);
        w = TakeIn();
        w.damaged = true;
        CHECK(std::strcmp(TakeBlock(w), "damaged") == 0);
    }
    CHECK(refused([](OwnIn& i) { i.phase = kPhaseUnknown; }));
    CHECK(!refused([](OwnIn& i) { i.cp_phase = 2; }));           // a camp at PreCaution with calm members: taken
    CHECK(refused([](OwnIn& i) { i.cp_phase = 3; }));
    CHECK(CampAlerted(3) && CampAlerted(5) && !CampAlerted(2) && !CampAlerted(kPhaseUnknown));
    CHECK(!refused([](OwnIn& i) { i.cp_phase = kPhaseUnknown; }));  // unknown camp: ignored
    CHECK(refused([](OwnIn& i) { i.damaged = true; }));
    CHECK(refused([](OwnIn& i) { i.terminal = true; }));
    CHECK(refused([](OwnIn& i) { i.guest_ok = false; }));
    CHECK(refused([](OwnIn& i) { i.host_ok = false; }));
    CHECK(refused([](OwnIn& i) { i.hold_over = false; }));
    CHECK(refused([](OwnIn& i) { i.guest_owned = kMaxGuestOwned; }));
    CHECK(!refused([](OwnIn& i) { i.guest_owned = kMaxGuestOwned - 1; }));
    CHECK(refused([](OwnIn& i) { i.changes_this_sec = kMaxChangesPerSec; }));
    CHECK(refused([](OwnIn& i) { i.parts_ok = false; }));
    CHECK(refused([](OwnIn& i) { i.pending = true; }));
    // a host-owned enemy never gets a back or a forced back
    OwnIn in = TakeIn();
    in.forced = in.refused = true, in.silence_ms = 1e9, in.d_g = 1000;
    CHECK(HostDecide(in).act == OwnAct::none);
}
void TestHostDecideBack() {
    CHECK(HostDecide(GuestIn()).act == OwnAct::none);
    OwnIn in = GuestIn();
    in.d_g = 60.0;
    CHECK(HostDecide(in).act == OwnAct::none);
    in.d_g = 60.1;
    CHECK(HostDecide(in).act == OwnAct::back && HostDecide(in).reason == OwnReason::far_off);
    in = GuestIn();
    in.d_h = 20.1;
    CHECK(HostDecide(in).act == OwnAct::none);
    in.d_h = 20.0;
    CHECK(HostDecide(in).act == OwnAct::back && HostDecide(in).reason == OwnReason::host_near);
    in.d_h = -1;  // unknown: no host-near
    CHECK(HostDecide(in).act == OwnAct::none);
    // the overlap: host 19 m, guest 3 m -> back (host near)
    in = GuestIn();
    in.d_g = 3, in.d_h = 19;
    CHECK(HostDecide(in).reason == OwnReason::host_near);
    // alerted: never back by proximity, wherever she goes
    in = GuestIn();
    in.d_g = 200, in.d_h = 5, in.phase = 5;
    CHECK(HostDecide(in).act == OwnAct::none);
    in.phase = 3;
    CHECK(HostDecide(in).act == OwnAct::none);
    in.phase = 2;  // PreCaution: calm
    CHECK(HostDecide(in).act == OwnAct::back);
    in.phase = kPhaseUnknown;
    CHECK(HostDecide(in).act == OwnAct::none);
    // calm again after a hit: the proximity rules apply
    in.phase = 0, in.damaged = true;
    CHECK(HostDecide(in).act == OwnAct::back);
    // the hold
    in.hold_over = false;
    CHECK(HostDecide(in).act == OwnAct::none);
    // the forced rules ignore phase, damage and the hold
    auto forced = [](void (*f)(OwnIn&), OwnReason r) {
        OwnIn x = GuestIn();
        x.phase = 5, x.damaged = true, x.hold_over = false;
        f(x);
        const OwnVerdict v = HostDecide(x);
        return v.act == OwnAct::forced_back && v.reason == r;
    };
    CHECK(forced([](OwnIn& x) { x.silence_ms = 3000.1; }, OwnReason::silence));
    CHECK(!forced([](OwnIn& x) { x.silence_ms = 3000.0; }, OwnReason::silence));
    CHECK(forced([](OwnIn& x) { x.unrealized_guest = true; }, OwnReason::unrealized));
    CHECK(forced([](OwnIn& x) { x.d_g = 270.1; }, OwnReason::range));
    CHECK(!forced([](OwnIn& x) { x.d_g = 270.0; }, OwnReason::range));
    CHECK(forced([](OwnIn& x) { x.d_g = 450, x.unrealized_guest = true; }, OwnReason::range));  // range first
    CHECK(forced([](OwnIn& x) { x.refused = true; }, OwnReason::refused));
    CHECK(forced([](OwnIn& x) { x.forced = true; }, OwnReason::forced));
    // dead or down on her side: hers (never back by proximity: the host's copy would get up alive); the forced rules still end it
    in = GuestIn();
    in.terminal = true, in.d_g = 200, in.d_h = 5;
    CHECK(HostDecide(in).act == OwnAct::none);
    CHECK(forced([](OwnIn& x) { x.terminal = true, x.d_g = 270.1; }, OwnReason::range));
    CHECK(forced([](OwnIn& x) { x.terminal = true, x.silence_ms = 3000.1; }, OwnReason::silence));
    CHECK(forced([](OwnIn& x) { x.pending = true, x.ack_wait_ms = 3000.1; }, OwnReason::ack_timeout));
    // pending within the ack time: nothing (not even silence)
    in = GuestIn();
    in.pending = true, in.ack_wait_ms = 2999, in.silence_ms = 1e9, in.d_g = 1000;
    CHECK(HostDecide(in).act == OwnAct::none);
    // the place a return gives the host's copy: within 10 m of its follower, at most 3 m below
    CHECK(BackPlaceOk(0, 0) && BackPlaceOk(10.0, -3.0) && BackPlaceOk(2, 8));
    CHECK(!BackPlaceOk(10.01, 0) && !BackPlaceOk(5, -3.01) && !BackPlaceOk(-1, 0) && !BackPlaceOk(std::nan(""), 0) && !BackPlaceOk(1, std::nan("")));
    // the phase a forced return carries
    CHECK(ReturnPhase(0) == 0 && ReturnPhase(2) == 2 && ReturnPhase(3) == 3 && ReturnPhase(4) == 3 && ReturnPhase(5) == 3 && ReturnPhase(kPhaseUnknown) == kPhaseUnknown);
    CHECK(std::strcmp(PhaseText(5), "alert") == 0 && std::strcmp(OwnReasonText(OwnReason::far_off), "far") == 0);
}
void TestHostOwnMachine() {
    HostOwn e;
    CHECK(!HostTake(e, 1000, false) && e.owner == Owner::host && e.hold_until == 1000 + kRefuseHoldMs && e.epoch == 0);
    e = HostOwn{};
    CHECK(HostTake(e, 1000, true) && e.owner == Owner::guest && e.pending && e.frozen && e.epoch == 1 && e.hold_until == 1000 + kHoldMs);
    CHECK(!HostTake(e, 1100, true));                                  // already hers
    CHECK(HostOnAck(e, 0, 0, 1100) == AckAct::ignore);                // another epoch
    CHECK(HostOnAck(e, 1, 0, 1100) == AckAct::taken && !e.pending && e.frozen);
    CHECK(HostOnAck(e, 1, 0, 1200) == AckAct::ignore);                // a repeat
    CHECK(HostBack(e, 20000) && e.owner == Owner::host && e.pending && e.frozen && e.epoch == 2);
    CHECK(!HostBackTimedOut(e, 23000) && HostBackTimedOut(e, 23001) && !e.pending && !e.frozen);
    e = HostOwn{};
    HostTake(e, 0, true);
    CHECK(HostOnAck(e, 1, 2, 300) == AckAct::refused_back && e.owner == Owner::host && !e.pending && !e.frozen && e.epoch == 2 && e.hold_until == 300 + kRefuseHoldMs);
    e = HostOwn{};
    HostTake(e, 0, true);
    HostOnAck(e, 1, 0, 100);
    HostBack(e, 15000);
    CHECK(HostOnAck(e, 2, 1, 15200) == AckAct::release && !e.frozen && !e.pending && e.owner == Owner::host);
    // a reset
    e = HostOwn{};
    HostTake(e, 0, true);
    HostResetEntry(e, 50);
    CHECK(e.owner == Owner::host && !e.pending && !e.frozen && e.epoch == 2);
    // the epoch wraps
    e = HostOwn{};
    e.epoch = 65535;
    CHECK(HostTake(e, 0, true) && e.epoch == 0);
}
void TestGuestOnOwn() {
    GuestOwn g;
    CHECK(GuestOnOwn(g, 1, Owner::guest) == GuestAct::take && g.known && g.epoch == 1);
    g.owned = true;  // (the caller, after the release)
    CHECK(GuestOnOwn(g, 1, Owner::guest) == GuestAct::ack_taken);   // equal: idempotent
    CHECK(GuestOnOwn(g, 0, Owner::host) == GuestAct::ignore);       // older
    CHECK(GuestOnOwn(g, 2, Owner::host) == GuestAct::release && g.epoch == 2);
    g.owned = false;
    CHECK(GuestOnOwn(g, 2, Owner::host) == GuestAct::ack_released);
    CHECK(GuestOnOwn(g, 3, Owner::host) == GuestAct::ack_released);  // a back for one she never took
    // the u16 wrap: 65535 -> 0 is newer
    GuestOwn w;
    CHECK(GuestOnOwn(w, 65535, Owner::guest) == GuestAct::take);
    w.owned = true;
    CHECK(GuestOnOwn(w, 0, Owner::host) == GuestAct::release);
    CHECK(GuestOnOwn(w, 65534, Owner::guest) == GuestAct::ignore);
    CHECK(EpochNewer(0, 65535) && !EpochNewer(65535, 0) && !EpochNewer(5, 5));
    CHECK(GenNewer(1, 0xFFFFFFFFu) && GenNewer(7, 6) && !GenNewer(6, 7));
}
// 10 000 random sequences of one enemy's hand-overs: the host's decisions through HostDecide (random distances, phases, damage,
// outcomes, holds), both directions' reliable in-order messages delayed 0..400 ms and one in 20 by 2.5..6 s (a stalled link),
// the guest refusing one take in 10, resets (OWNRESET). Two thinking copies only inside a bounded window: a reset's announce
// window (until she has the OWNRESET) or a back whose OWNACK timed out while her OWN(host) is still in flight (until she has
// it); both at most the longest delay. Zero thinking only while a hand-over is in flight (two ack timeouts and a 2 Hz decision at most); both
// tables equal at the end.
void TestOwnProperty() {
    std::uint32_t rng = 12345;
    auto rnd = [&](unsigned n) {
        rng = rng * 1664525u + 1013904223u;
        return (rng >> 8) % n;
    };
    constexpr double kLongMin = 2500, kLongMax = 6000;
    auto delay = [&]() { return rnd(20) == 0 ? kLongMin + rnd(static_cast<unsigned>(kLongMax - kLongMin) + 1) : static_cast<double>(rnd(401)); };
    struct Msg {
        int type;  // 0 OWN, 1 ACK, 2 RESET
        std::uint16_t epoch;
        Owner owner;
        std::uint8_t result;
        std::uint32_t gen;
        double at;
    };
    unsigned two_thinking = 0, bad_end = 0, takes = 0, backs = 0, refusals = 0, timeouts = 0, timeout_two = 0, reset_two = 0;
    double worst_zero = 0, worst_reset_window = 0, worst_timeout_window = 0;
    for (int run = 0; run < 10000; ++run) {
        HostOwn e;
        GuestOwn g;
        std::uint32_t hgen = 1, ggen = 1;
        std::vector<Msg> to_g, to_h;
        double now = 0, zero_since = -1, reset_since = -1, timeout_since = -1;
        std::uint16_t timeout_epoch = 0;
        const int steps = 200 + static_cast<int>(rnd(400));
        auto ack = [&](std::uint16_t ep, Owner o, std::uint8_t res) { to_h.push_back(Msg{1, ep, o, res, ggen, now + delay()}); };
        auto send = [&](Owner o) { to_g.push_back(Msg{0, e.epoch, o, 0, hgen, now + delay()}); };
        for (int st = 0; st <= steps + 400; ++st, now += 50) {
            const bool live = st < steps;  // then only drain
            // deliveries (in order: a later message never overtakes)
            for (auto* q : {&to_g, &to_h}) {
                for (std::size_t i = 1; i < q->size(); ++i) (*q)[i].at = (std::max)((*q)[i].at, (*q)[i - 1].at);
            }
            while (!to_g.empty() && to_g.front().at <= now) {
                const Msg m = to_g.front();
                to_g.erase(to_g.begin());
                if (m.type == 2) {
                    if (GenNewer(m.gen, ggen)) ggen = m.gen, g = GuestOwn{}, timeout_since = -1;  // frozen again
                    if (m.gen == hgen) reset_since = -1;  // the newest reset is in (an older one's window runs on)
                    continue;
                }
                if (m.gen != ggen) continue;
                switch (GuestOnOwn(g, m.epoch, m.owner)) {
                    case GuestAct::take:
                        if (rnd(10) == 0) ++refusals, ack(m.epoch, Owner::host, 2);  // refused: hers never unfroze
                        else g.owned = true, ack(m.epoch, Owner::guest, 0);
                        break;
                    case GuestAct::release: g.owned = false, ack(m.epoch, Owner::host, 1); break;
                    case GuestAct::ack_taken: ack(m.epoch, Owner::guest, 0); break;
                    case GuestAct::ack_released: ack(m.epoch, Owner::host, 1); break;
                    case GuestAct::ignore: break;
                }
                // she has the timed-out back (or a newer OWN): that window is over
                if (timeout_since >= 0 && (m.epoch == timeout_epoch || EpochNewer(m.epoch, timeout_epoch))) timeout_since = -1;
            }
            while (!to_h.empty() && to_h.front().at <= now) {
                const Msg m = to_h.front();
                to_h.erase(to_h.begin());
                if (m.gen == hgen) HostOnAck(e, m.epoch, m.result, now);
            }
            // the host's decisions (2 Hz, as OwnHostTick), through HostDecide
            if (live && st % 10 == 0) {
                OwnIn oi;
                oi.owner = e.owner, oi.pending = e.pending;
                oi.d_g = rnd(30) == 0 ? -1.0 : static_cast<double>(rnd(800)) / 10.0;
                oi.d_h = rnd(30) == 0 ? -1.0 : oi.d_g + static_cast<double>(rnd(1200)) / 10.0 - 30.0;
                const unsigned ph = rnd(8) < 6 ? rnd(3) : rnd(7);  // mostly calm; 6: unknown
                oi.phase = ph == 6 ? kPhaseUnknown : static_cast<std::uint8_t>(ph);
                oi.cp_phase = static_cast<std::uint8_t>(rnd(4) == 0 ? rnd(6) : 0);
                oi.damaged = rnd(8) == 0;
                oi.terminal = rnd(15) == 0;
                oi.guest_ok = rnd(20) != 0, oi.host_ok = rnd(20) != 0;
                oi.hold_over = now >= e.hold_until;
                oi.silence_ms = rnd(40) == 0 ? 3500.0 : 100.0;
                oi.unrealized_guest = rnd(60) == 0;
                oi.range_m = 270, oi.ack_wait_ms = now - e.t_sent;
                oi.forced = rnd(80) == 0;
                const OwnVerdict v = HostDecide(oi);
                if (e.owner == Owner::host && !e.pending && v.act == OwnAct::take) {
                    if (HostTake(e, now, rnd(20) != 0)) ++takes, send(Owner::guest);
                } else if (e.owner == Owner::guest && (v.act == OwnAct::back || v.act == OwnAct::forced_back)) {
                    if (HostBack(e, now)) ++backs, send(Owner::host);
                }
                if (rnd(400) == 0) {
                    HostResetEntry(e, now);
                    ++hgen;
                    to_g.push_back(Msg{2, 0, Owner::host, 0, hgen, now + delay()});
                    reset_since = now;  // until she has the OWNRESET (what was in flight before it still lands first)
                    timeout_since = -1;
                }
            }
            if (HostBackTimedOut(e, now)) ++timeouts, timeout_since = now, timeout_epoch = e.epoch;
            const bool host_thinks = !e.frozen, guest_thinks = g.owned;
            if (host_thinks && guest_thinks) {
                if (reset_since >= 0) ++reset_two, worst_reset_window = (std::max)(worst_reset_window, now - reset_since);
                else if (timeout_since >= 0) ++timeout_two, worst_timeout_window = (std::max)(worst_timeout_window, now - timeout_since);
                else ++two_thinking;
            }
            if (!host_thinks && !guest_thinks) {
                if (zero_since < 0) zero_since = now;
                worst_zero = (std::max)(worst_zero, now - zero_since);
            } else {
                zero_since = -1;
            }
        }
        // the end: nothing in flight, both tables agree
        if (!to_g.empty() || !to_h.empty() || e.pending || ((e.owner == Owner::guest) != g.owned)) ++bad_end;
    }
    CHECK(two_thinking == 0);
    CHECK(bad_end == 0);
    // a take whose OWN or OWNACK is stalled: forced back at the ack timeout (at the next 2 Hz decision), released at the back's
    // timeout: two ack timeouts and one decision period at most
    CHECK(worst_zero <= 2 * kAckTimeoutMs + 500 + 100);
    CHECK(worst_reset_window <= kLongMax + 50);    // a reset's announce window: the delay of its OWNRESET
    CHECK(worst_timeout_window <= kLongMax + 50);  // a timed-out back: the delay of her OWN(host)
    // the paths are reached: takes, backs, refusals, timeouts, and both windows
    CHECK(takes > 1000 && backs > 100 && refusals > 50 && timeouts > 20 && timeout_two > 0 && reset_two > 0);
    std::printf("own property: two-thinking outside a window %u (in a timeout window %u ticks, max %.0f ms; in a reset window %u ticks, max %.0f ms), bad end %u, "
                "worst zero %.0f ms (takes %u backs %u refusals %u timeouts %u)\n",
                two_thinking, timeout_two, worst_timeout_window, reset_two, worst_reset_window, bad_end, worst_zero, takes, backs, refusals, timeouts);
}
void TestPoseAlloc() {
    PoseAlloc a;
    // capture takes up to its limit
    for (int i = 0; i < 6; ++i) CHECK(a.Take(PoseAlloc::kCapture, 6) == i);
    CHECK(a.Take(PoseAlloc::kCapture, 6) == -1);
    // apply first: the 2 free, then a capture's (stolen, from the top)
    bool stolen = true;
    CHECK(a.Take(PoseAlloc::kApply, 6, &stolen) == 6 && !stolen);
    CHECK(a.Take(PoseAlloc::kApply, 6, &stolen) == 7 && !stolen);
    CHECK(a.Take(PoseAlloc::kApply, 6, &stolen) == 5 && stolen);
    CHECK(a.Count(PoseAlloc::kApply) == 3 && a.Count(PoseAlloc::kCapture) == 5);
    // the apply limit
    for (int i = 0; i < 3; ++i) a.Take(PoseAlloc::kApply, 6);
    CHECK(a.Count(PoseAlloc::kApply) == 6 && a.Take(PoseAlloc::kApply, 6) == -1);
    CHECK(a.Count(PoseAlloc::kApply) + a.Count(PoseAlloc::kCapture) <= kPoseSlots);
    a.Free(5);
    CHECK(a.At(5) == PoseAlloc::kFree && a.Count(PoseAlloc::kApply) == 5);
    // the probe's slot 7
    PoseAlloc b;
    b.SetProbing(true);
    for (int i = 0; i < 7; ++i) CHECK(b.Take(PoseAlloc::kApply, 8) == i);
    CHECK(b.Take(PoseAlloc::kApply, 8) == -1 && b.At(7) == PoseAlloc::kFree);
    b.SetProbing(false);
    CHECK(b.Take(PoseAlloc::kApply, 8) == 7);
    CHECK(b.Take(PoseAlloc::kFree, 8) == -1);
}
// Round 23 B (design 7.2): the mirror diagnostics' gates, the not-holding rule, the in-vehicle test and the keys.
void TestMirrorsB() {
    // diagnostics: the worst line while p95 is above 5 m; one full line per id the first time it is above 20 m
    CHECK(!MirrorWorstDue(5.0) && MirrorWorstDue(5.01) && !MirrorWorstDue(0));
    CHECK(MirrorFirstFull(20.5, false) && !MirrorFirstFull(20.5, true) && !MirrorFirstFull(20.0, false));
    // the teleport ring: the newest 3 within the window
    TeleportRing r;
    CHECK(r.Within(0, 2000) == 0);
    r.Note(1000);
    r.Note(1500);
    CHECK(r.Within(1500, 2000) == 2);
    CHECK(!NotHolding(true, 50.0, r.Within(1500, 2000)));  // 2 teleports: not yet
    r.Note(2000);
    CHECK(r.Within(2000, 2000) == 3);
    CHECK(NotHolding(true, 10.5, r.Within(2000, 2000)));
    CHECK(!NotHolding(true, 10.0, r.Within(2000, 2000)));  // 10 m: holding (above 10 only)
    CHECK(!NotHolding(false, 200.0, r.Within(2000, 2000)));  // enemy_mirror_hold_check=0
    CHECK(r.Within(3000, 2000) == 3);                         // 1000 is exactly 2 s old: in
    CHECK(r.Within(3001, 2000) == 2);                         // ... and out a millisecond later
    CHECK(!NotHolding(true, 100.0, r.Within(3001, 2000)));
    r.Note(3001);  // the ring keeps the newest 3: 1500 2000 3001
    CHECK(r.Within(3001, 2000) == 3 && r.Within(3600, 2000) == 2);
    // the 500 ms teleport gap allows at most 5 in 2 s; 3 is reached after 1 s of not holding
    TeleportRing g;
    for (int i = 0; i < 5; ++i) g.Note(10000 + 500.0 * i);
    CHECK(g.Within(12000, 2000) == 3);
    CHECK(g.Within(9000, 2000) == 0);  // none at or before 9000
    g.Reset();
    CHECK(g.Within(12000, 2000) == 0 && g.head == 0);
    // retried every 5 s
    CHECK(!NotHoldRetry(4999) && NotHoldRetry(5000) && NotHoldRetry(60000));
    // the in-vehicle test: the parent chain first, else the ride fields (the byte checks passed)
    CHECK(!SeatedByRide(kVehicleIdNone, 0));        // the constructor's none
    CHECK(!SeatedByRide(0xFFFFFFFFu, 0));           // none in the low 21 bits
    CHECK(!SeatedByRide(0, 0));                     // zero (never a vehicle id seen)
    CHECK(!SeatedByRide(0x1234, -1));               // no seat
    CHECK(!SeatedByRide(0x1234, 16));               // out of range
    CHECK(SeatedByRide(0x1234, 0) && SeatedByRide(0xFFE01234u, 15));
    CHECK(VehicleTest(true, false, kVehicleIdNone, -1) == VehTest::parent);
    CHECK(VehicleTest(true, true, 0x1234, 0) == VehTest::parent);  // the parent wins
    CHECK(VehicleTest(false, true, 0x1234, 1) == VehTest::ride);
    CHECK(VehicleTest(false, false, 0x1234, 1) == VehTest::none);  // ride fields off (a byte check failed, or a read fault)
    CHECK(VehicleTest(false, true, kVehicleIdNone, -1) == VehTest::none);
    // round 23 D: the truck rules name it a driver (last: the parent chain and the ride fields first)
    CHECK(VehicleTest(false, false, kVehicleIdNone, -1, true) == VehTest::driver);
    CHECK(VehicleTest(false, true, 0x1234, 1, true) == VehTest::ride);
    CHECK(VehicleTest(true, true, 0x1234, 1, true) == VehTest::parent);
    CHECK(VehicleTest(false, true, kVehicleIdNone, -1, false) == VehTest::none);
    CHECK(std::strcmp(VehTestText(VehTest::driver), "a truck's driver seat (unit +0x158)") == 0);
    CHECK(std::strcmp(VehTestText(VehTest::ride), "the actor's +0xE20 / +0x3C0") == 0 && std::strcmp(VehTestText(VehTest::none), "none") == 0);
    CHECK(std::strcmp(VehicleSideText(true, true), "both copies") == 0 && std::strcmp(VehicleSideText(false, true), "the owner's copy") == 0 &&
          std::strcmp(VehicleSideText(true, false), "our copy") == 0 && std::strcmp(VehicleSideText(false, false), "neither") == 0);
    // the lease: released to its own game when either copy is in a vehicle (only with the check on)
    LeaseIn l;
    l.frozen_by_us = true;
    l.in_vehicle_host = true;
    CHECK(Lease(l) == LeaseVerdict::keep);  // vehicle_check off: the old behaviour
    l.vehicle_check = true;
    CHECK(Lease(l) == LeaseVerdict::release_vehicle && Releases(LeaseVerdict::release_vehicle));
    l.in_vehicle_host = false, l.in_vehicle_here = true;
    CHECK(Lease(l) == LeaseVerdict::release_vehicle);
    l.in_vehicle_here = false;
    CHECK(Lease(l) == LeaseVerdict::keep);
    l.in_vehicle_here = true, l.frame_age_ms = 1600;
    CHECK(Lease(l) == LeaseVerdict::release_silence);  // silence first
    l.frame_age_ms = 0, l.off = true;
    CHECK(Lease(l) == LeaseVerdict::release_off);
    CHECK(std::strcmp(LeaseText(LeaseVerdict::release_vehicle), "in a vehicle") == 0);
    // entry: refused while the owner's entry says in a vehicle
    EligibleIn e;
    e.host_in_vehicle = true;
    CHECK(Eligible(e) == Refusal::host_in_vehicle);
    CHECK(std::strstr(RefusalText(Refusal::host_in_vehicle), "in a vehicle on the host") != nullptr);
    e.in_vehicle = true;
    CHECK(Eligible(e) == Refusal::in_vehicle);
    // the keys
    cfgval::MirrorKeys k;
    CHECK(k.hold_check && k.vehicle);
    CHECK(cfgval::SetMirrorKey(k, "enemy_mirror_hold_check", false) && !k.hold_check && k.vehicle);
    CHECK(cfgval::SetMirrorKey(k, "enemy_mirror_vehicle", false) && !k.vehicle);
    CHECK(!cfgval::SetMirrorKey(k, "enemy_mirror", true) && !k.hold_check);
}

}  // namespace

// ---------------------------------------------------------------- P1c (round 21): ownership per camp
void TestCampGeometry() {
    // a loose MULE (a camp of one, r 10 m) gets P1b's distances exactly
    CHECK(CampR(0) == kCampRMinM && CampR(500) == kCampRMaxM && CampR(55) == 55);
    CHECK(CampEngage(CampR(0)) == kTakeM);
    CHECK(CampMargin(CampR(0)) == kTakeMarginM);
    CHECK(CampLeft(CampR(0)) == kBackFarM);
    // a big camp: wider, capped
    CHECK(CampEngage(60) == 90 && CampMargin(60) == 28 && CampLeft(60) == 110);
    CHECK(CampEngage(120) == kCampEngageMaxM && CampMargin(120) == kCampMarginMaxM);
    // monotonic in r
    for (double r = 10; r < 120; r += 5) {
        CHECK(CampEngage(r) <= CampEngage(r + 5));
        CHECK(CampMargin(r) <= CampMargin(r + 5));
        CHECK(CampLeft(r) > CampEngage(r));  // hysteresis: she leaves farther out than she engages
    }
}

CampIn CalmCamp() {
    CampIn c;
    c.owner = Owner::host, c.r = 10, c.d_g = 20, c.d_h = 80, c.phase = 0, c.cp_phase = 0, c.members = 4, c.takeable = 4;
    return c;
}

bool TakeBlocked(const CampIn& in, const char* want) {
    const char* b = CampTakeBlock(in);
    return b && std::strcmp(b, want) == 0 && CampDecide(in).act == OwnAct::none;
}

// (round 27 M4) take first: a calm host camp she is closing on, our Sam far, holds her stimulus off until the take (20 s at most)
void TestCampTakeFirst() {
    const CampIn c = CalmCamp();
    CHECK(CampTakeFirst(c, 0));
    CHECK(CampTakeFirst(c, kTakeFirstCloseMaxMs - 1));
    CHECK(!CampTakeFirst(c, kTakeFirstCloseMaxMs));  // a blocked take never hides her for good
    { CampIn x = c; x.d_g = 200, x.d_h = 400; CHECK(CampTakeFirst(x, 0)); }  // her approach: no engage distance in the rule
    { CampIn x = c; x.cp_phase = kPhaseCaution; CHECK(!CampTakeFirst(x, 0)); }  // alerted
    { CampIn x = c; x.cp_phase = kPhaseUnknown; CHECK(CampTakeFirst(x, 0)); }   // a loose camp: no CP phase
    { CampIn x = c; x.phase = kPhaseCaution; CHECK(!CampTakeFirst(x, 0)); }
    { CampIn x = c; x.phase = kPhaseUnknown; CHECK(!CampTakeFirst(x, 0)); }
    { CampIn x = c; x.d_g = 5, x.d_h = 24; CHECK(!CampTakeFirst(x, 0)); }  // Sam within 25 m
    { CampIn x = c; x.d_h = 34.9; CHECK(!CampTakeFirst(x, 0)); }           // the margin short
    { CampIn x = c; x.d_h = 35; CHECK(CampTakeFirst(x, 0)); }              // exactly the margin
    { CampIn x = c; x.owner = Owner::guest; CHECK(!CampTakeFirst(x, 0)); }
    { CampIn x = c; x.pending = true; CHECK(!CampTakeFirst(x, 0)); }
    { CampIn x = c; x.parts_ok = false; CHECK(!CampTakeFirst(x, 0)); }
    { CampIn x = c; x.members = kCampMaxMembers + 1; CHECK(!CampTakeFirst(x, 0)); }
    { CampIn x = c; x.takeable = 0; CHECK(!CampTakeFirst(x, 0)); }
    { CampIn x = c; x.host_loaded = false; CHECK(!CampTakeFirst(x, 0)); }
    { CampIn x = c; x.d_g = -1; CHECK(!CampTakeFirst(x, 0)); }
    { CampIn x = c; x.d_h = -1; CHECK(!CampTakeFirst(x, 0)); }
    { CampIn x = c; x.guest_ok = false; CHECK(!CampTakeFirst(x, 0)); }
    { CampIn x = c; x.host_ok = false; CHECK(!CampTakeFirst(x, 0)); }
    // the hold, her cap and one-at-a-time do not end it (the take waits; close_ms bounds that)
    { CampIn x = c; x.hold_over = false; CHECK(CampTakeFirst(x, 0)); }
}

void TestCampTake() {
    const CampIn c = CalmCamp();
    CHECK(CampTakeBlock(c) == nullptr);
    CHECK(CampDecide(c).act == OwnAct::take && CampDecide(c).reason == OwnReason::proximity);
    // the log order: each block shows while every earlier one is clear
    { CampIn x = c; x.owner = Owner::guest; CHECK(CampTakeBlock(x) && std::strcmp(CampTakeBlock(x), "not the host's") == 0); }
    { CampIn x = c; x.pending = true; CHECK(TakeBlocked(x, "pending")); }
    { CampIn x = c; x.parts_ok = false; CHECK(TakeBlocked(x, "parts")); }
    { CampIn x = c; x.members = kCampMaxMembers + 1; CHECK(TakeBlocked(x, "too big")); }
    { CampIn x = c; x.takeable = 0; CHECK(TakeBlocked(x, "nothing to take (down, held or unread on the host)")); }
    { CampIn x = c; x.d_g = 41; CHECK(TakeBlocked(x, "the guest beyond its engage distance")); }
    { CampIn x = c; x.d_g = -1; CHECK(TakeBlocked(x, "the guest beyond its engage distance")); }
    { CampIn x = c; x.d_h = 34.9; CHECK(TakeBlocked(x, "the host not its margin farther")); }
    { CampIn x = c; x.d_h = 35; CHECK(CampTakeBlock(x) == nullptr); }  // exactly the margin: taken
    { CampIn x = c; x.d_g = 5, x.d_h = 24; CHECK(TakeBlocked(x, "the host within 25 m")); }
    { CampIn x = c; x.phase = kPhaseCaution; CHECK(TakeBlocked(x, "a member's phase")); }
    { CampIn x = c; x.phase = kPhaseUnknown; CHECK(TakeBlocked(x, "a member's phase")); }
    { CampIn x = c; x.cp_phase = kPhaseCaution; CHECK(TakeBlocked(x, "the camp's phase")); }
    { CampIn x = c; x.cp_phase = kPhaseUnknown; CHECK(CampTakeBlock(x) == nullptr); }  // a loose camp: no CP phase
    { CampIn x = c; x.guest_ok = false; CHECK(TakeBlocked(x, "the guest (a cutscene, refusals)")); }
    { CampIn x = c; x.host_ok = false; CHECK(TakeBlocked(x, "the host (a cutscene, a save, the guards)")); }
    { CampIn x = c; x.hold_over = false; CHECK(TakeBlocked(x, "the hold")); }
    { CampIn x = c; x.guest_owned = kMaxGuestOwnedMembers - 3; CHECK(TakeBlocked(x, "her cap")); }
    { CampIn x = c; x.guest_owned = kMaxGuestOwnedMembers - 4; CHECK(CampTakeBlock(x) == nullptr); }
    { CampIn x = c; x.since_change_ms = 500; CHECK(TakeBlocked(x, "one change at a time")); }
    { CampIn x = c; x.other_pending = true; CHECK(TakeBlocked(x, "one change at a time")); }
    // a big camp: her 80 m out is within its engage distance (r 60: 90 m), the host 28 m farther
    {
        CampIn x = c;
        x.r = 60, x.d_g = 80, x.d_h = 108;
        CHECK(CampTakeBlock(x) == nullptr);
        x.d_h = 107;
        CHECK(CampTakeBlock(x) != nullptr);
    }
    // a camp the host has not loaded: her distance alone, reason "unloaded"
    {
        CampIn x = c;
        x.host_loaded = false, x.d_h = -1;
        CHECK(CampDecide(x).act == OwnAct::take && CampDecide(x).reason == OwnReason::unloaded);
        x.d_h = 10;
        CHECK(CampDecide(x).act == OwnAct::take);
        // its phases are stale on the host (the AI does not run there): not a block
        x.d_g = 20, x.phase = 4, x.cp_phase = 4;
        CHECK(CampDecide(x).act == OwnAct::take && CampDecide(x).reason == OwnReason::unloaded);
        x.host_loaded = true, x.d_h = 80;  // loaded again: the phases count
        CHECK(TakeBlocked(x, "a member's phase"));
        x.host_loaded = false;
        x.d_g = 41;
        CHECK(CampDecide(x).act == OwnAct::none);
    }
}

CampIn HerCamp() {
    CampIn c = CalmCamp();
    c.owner = Owner::guest;
    return c;
}

bool Says(const char* got, const char* want) { return got && std::strcmp(got, want) == 0; }

void TestCampBack() {
    const CampIn c = HerCamp();
    CHECK(CampDecide(c).act == OwnAct::none);
    CHECK(Says(CampStaysWhy(c), "she is nearer"));
    // forced, in this order, whatever the phase and the hold
    {
        CampIn x = c;
        x.refused = true, x.forced = true, x.phase = 4, x.hold_over = false;
        CHECK(CampDecide(x).act == OwnAct::forced_back && CampDecide(x).reason == OwnReason::refused);
    }
    { CampIn x = c; x.forced = true, x.phase = 4; CHECK(CampDecide(x).act == OwnAct::forced_back && CampDecide(x).reason == OwnReason::forced); }
    { CampIn x = c; x.pending = true, x.ack_wait_ms = kAckTimeoutMs + 1; CHECK(CampDecide(x).reason == OwnReason::ack_timeout); }
    { CampIn x = c; x.pending = true, x.ack_wait_ms = 100, x.d_g = 500; CHECK(CampDecide(x).act == OwnAct::none); }  // pending: wait
    { CampIn x = c; x.silence_ms = kOwnSilenceMs + 1, x.phase = 4; CHECK(CampDecide(x).reason == OwnReason::silence); }
    { CampIn x = c; x.d_g = 271, x.phase = 4; CHECK(CampDecide(x).reason == OwnReason::range); }
    { CampIn x = c; x.unrealized_guest = true, x.phase = 4; CHECK(CampDecide(x).reason == OwnReason::unrealized); }
    { CampIn x = c; x.unrealized_guest = true, x.host_loaded = false; CHECK(CampDecide(x).act == OwnAct::none); }  // nowhere better to run
    // a member down on her side and no carry: hers
    {
        CampIn x = c;
        x.down_guest = true, x.outcomes_ok = false, x.d_g = 200;
        CHECK(CampDecide(x).act == OwnAct::none);
        CHECK(Says(CampStaysWhy(x), "a member down on her side (no outcome carry)"));
    }
    { CampIn x = c; x.down_guest = true, x.outcomes_ok = true, x.d_g = 61; CHECK(CampDecide(x).act == OwnAct::back); }
    // by proximity only calm and after the hold: the host walking into her fight does not take it
    {
        CampIn x = c;
        x.phase = kPhaseCaution, x.d_h = 5;
        CHECK(CampDecide(x).act == OwnAct::none);
        CHECK(Says(CampStaysWhy(x), "alerted on her side"));
    }
    {
        CampIn x = c;
        x.hold_over = false, x.d_g = 100;
        CHECK(CampDecide(x).act == OwnAct::none);
        CHECK(Says(CampStaysWhy(x), "the hold"));
    }
    { CampIn x = c; x.d_g = 60; CHECK(CampDecide(x).act == OwnAct::none); x.d_g = 60.1; CHECK(CampDecide(x).reason == OwnReason::far_off); }
    { CampIn x = c; x.d_h = 20; CHECK(CampDecide(x).reason == OwnReason::host_near); }
    // the host nearer by the margin, within engage
    { CampIn x = c; x.d_g = 38, x.d_h = 23; CHECK(CampDecide(x).act == OwnAct::back && CampDecide(x).reason == OwnReason::host_nearer); }
    { CampIn x = c; x.d_g = 38, x.d_h = 24; CHECK(CampDecide(x).act == OwnAct::none); }
    {
        CampIn x = c;
        x.r = 60, x.d_g = 100, x.d_h = 72;
        CHECK(CampDecide(x).reason == OwnReason::host_nearer);
        x.d_h = 73;
        CHECK(CampDecide(x).act == OwnAct::none);
    }
    // an unloaded camp: the host's distances do not count for a back either (review rv-correctness R9)
    { CampIn x = c; x.host_loaded = false, x.d_h = 20; CHECK(CampDecide(x).act == OwnAct::none); }
    { CampIn x = c; x.host_loaded = false, x.d_g = 38, x.d_h = 23; CHECK(CampDecide(x).act == OwnAct::none); }
    { CampIn x = c; x.host_loaded = false, x.d_g = 60.1; CHECK(CampDecide(x).reason == OwnReason::far_off); }
    // hysteresis: no place is both a take and a back by proximity, loaded or not
    int both = 0;
    for (bool loaded : {true, false})
        for (double r : {10.0, 40.0, 120.0})
            for (double dg = 0; dg <= 200; dg += 2.5)
                for (double dh = 0; dh <= 200; dh += 2.5) {
                    CampIn t = CalmCamp();
                    t.r = r, t.d_g = dg, t.d_h = dh, t.host_loaded = loaded;
                    CampIn b = HerCamp();
                    b.r = r, b.d_g = dg, b.d_h = dh, b.host_loaded = loaded;
                    if (CampDecide(t).act == OwnAct::take && CampDecide(b).act == OwnAct::back) {
                        if (both++ < 3) std::printf("  take and back at once: loaded %d r %.0f d_g %.1f d_h %.1f\n", loaded ? 1 : 0, r, dg, dh);
                    }
                }
    CHECK(both == 0);
}

void TestCampSplits() {
    // one member's state there splits (review rv-correctness B2, rv-gameplay 1); a cutscene, her cap, an alerted camp, a fault do not
    for (OwnRefuse r : {OwnRefuse::not_mirror, OwnRefuse::not_realized, OwnRefuse::dead, OwnRefuse::held, OwnRefuse::hidden}) {
        CHECK(CampRefuseSplits(static_cast<std::uint8_t>(r)));
        CHECK(!CampRefuseCounts(static_cast<std::uint8_t>(r)));
    }
    for (OwnRefuse r : {OwnRefuse::cutscene, OwnRefuse::cap, OwnRefuse::camp_alerted, OwnRefuse::fault}) {
        CHECK(!CampRefuseSplits(static_cast<std::uint8_t>(r)));
        CHECK(CampRefuseCounts(static_cast<std::uint8_t>(r)));
    }
    // a member dead there stays out of its camp's takes for the world; the other splits 15 s
    CHECK(CampNoTakeMs(static_cast<std::uint8_t>(OwnRefuse::dead)) < 0);
    CHECK(CampNoTakeMs(static_cast<std::uint8_t>(OwnRefuse::held)) == kCampRefuseHoldMs);
    CHECK(CampNoTakeMs(static_cast<std::uint8_t>(OwnRefuse::not_realized)) == kCampRefuseHoldMs);
    CHECK(Says(OwnReasonText(OwnReason::unloaded), "not loaded on the host"));
    CHECK(Says(OwnReasonText(OwnReason::host_nearer), "the host nearer"));
    CHECK(Says(OwnReasonText(static_cast<OwnReason>(15)), "?"));
}

// CampAggregate (review rv-gameplay 15): the per-camp glue, from member states.
CampMember Mem(bool hers, double x, double y = 0) {
    CampMember m;
    m.hers = hers, m.realized_host = true, m.phase = 0, m.takeable = !hers;
    m.pos[0] = x, m.pos[1] = y, m.pos[2] = 0;
    return m;
}
void TestCampAggregate() {
    const double guest[3] = {0, 0, 0}, host[3] = {200, 0, 0};
    {
        // four of the host's, standing: the nearest member's distances, r from the spread, everything takeable
        CampMember m[4] = {Mem(false, 30), Mem(false, 35), Mem(false, 38), Mem(false, 35, 3)};
        CampIn c;
        const CampAgg g = CampAggregate(m, 4, guest, host, c);
        CHECK(g.hers == 0 && g.pend == 0 && c.owner == Owner::host && !c.pending);
        CHECK(c.members == 4 && c.takeable == 4 && c.host_loaded);
        CHECK(std::fabs(c.d_g - 30) < 1e-9 && std::fabs(c.d_h - 162) < 1e-9);
        CHECK(c.r == kCampRMinM && c.phase == 0);
        // unknown places: d_g -1 for a host camp, d_h -1
        CampIn u;
        CampAggregate(m, 4, nullptr, nullptr, u);
        CHECK(u.d_g == -1 && u.d_h == -1);
    }
    {
        // a member she killed (rv-gameplay 1): down there, not standing; the camp's distances from the standing ones; her phase
        // from the standing ones; nothing forced
        CampMember m[3] = {Mem(true, 30), Mem(true, 80), Mem(true, 90)};
        m[0].standing = false, m[0].phase = 4;
        m[1].phase = 1, m[2].phase = 0;
        CampIn c;
        const CampAgg g = CampAggregate(m, 3, guest, host, c);
        CHECK(g.hers == 3 && c.owner == Owner::guest && c.down_guest && !c.forced);
        CHECK(std::fabs(c.d_g - 80) < 1e-9 && c.phase == 1);
        CHECK(!c.unrealized_guest);
        // her place unknown: a range back's 1e9
        CampIn u;
        CampAggregate(m, 3, nullptr, host, u);
        CHECK(u.d_g == 1e9 && CampDecide(u).reason == OwnReason::range);
        // every member down: every member counts for the distances
        for (CampMember& x : m) x.standing = false;
        CampIn d;
        CampAggregate(m, 3, guest, host, d);
        CHECK(std::fabs(d.d_g - 30) < 1e-9 && d.phase == 0);
    }
    {
        // one member seq-held there (a cargo theft, a truck: rv-gameplay 3): never the whole camp back; every settled member for
        // 2 s: back
        CampMember m[3] = {Mem(true, 20), Mem(true, 25), Mem(true, 30)};
        m[0].bad_guest = true, m[0].bad_for_ms = 60000;
        CampIn c;
        CampAggregate(m, 3, guest, host, c);
        CHECK(!c.unrealized_guest && CampDecide(c).act == OwnAct::none);
        for (CampMember& x : m) x.bad_guest = true, x.bad_for_ms = kCampUnrealizedMs - 1;
        CampAggregate(m, 3, guest, host, c);
        CHECK(!c.unrealized_guest);
        for (CampMember& x : m) x.bad_for_ms = kCampUnrealizedMs;
        CampAggregate(m, 3, guest, host, c);
        CHECK(c.unrealized_guest && CampDecide(c).reason == OwnReason::unrealized);
        // a pending member does not count (it sent no frame yet); a camp with none settled is never "unrealized"
        m[0].bad_guest = false, m[0].pending = true;
        CampAggregate(m, 3, guest, host, c);
        CHECK(c.unrealized_guest && c.pending);
        for (CampMember& x : m) x.pending = true;
        CampAggregate(m, 3, guest, host, c);
        CHECK(!c.unrealized_guest);
    }
    {
        // a split camp (rv-gameplay 5): hers plus a straggler of the host's that stands again: taken alone when calm and settled
        CampMember m[3] = {Mem(true, 20), Mem(true, 25), Mem(false, 30)};
        CampIn c;
        CampAggregate(m, 3, guest, host, c);
        CHECK(c.owner == Owner::guest && c.takeable == 1);
        CHECK(CampStragglerOk(c));
        { CampIn x = c; x.pending = true; CHECK(!CampStragglerOk(x)); }
        { CampIn x = c; x.phase = kPhaseCaution; CHECK(!CampStragglerOk(x)); }
        { CampIn x = c; x.since_change_ms = 500; CHECK(!CampStragglerOk(x)); }
        { CampIn x = c; x.guest_owned = kMaxGuestOwnedMembers; CHECK(!CampStragglerOk(x)); }
        { CampIn x = c; x.host_ok = false; CHECK(!CampStragglerOk(x)); }
        m[2].takeable = false;  // down, held or on the no-take list
        CampAggregate(m, 3, guest, host, c);
        CHECK(c.takeable == 0 && !CampStragglerOk(c));
        CampIn h = CalmCamp();
        CHECK(!CampStragglerOk(h));  // the host's camp: a take, not a straggler
    }
    {
        // an orphan (rv-gameplay 11): one of hers in no camp of the host's view, unloaded there: a loose camp of one, her frames
        CampMember o = Mem(true, 300);
        o.realized_host = false;
        CampIn c;
        c.range_m = 270;
        CampAggregate(&o, 1, guest, host, c);
        CHECK(!c.host_loaded && c.members == 1 && c.r == kCampRMinM);
        CHECK(CampDecide(c).reason == OwnReason::range);
        o.pos[0] = 100;
        CampAggregate(&o, 1, guest, host, c);
        CHECK(CampDecide(c).reason == OwnReason::far_off);
        o.pos[0] = 30, o.silence_ms = kOwnSilenceMs + 1;
        CampAggregate(&o, 1, guest, host, c);
        CHECK(CampDecide(c).reason == OwnReason::silence);
        // the host's distance does not count unloaded (rv-correctness R9)
        const double near_host[3] = {35, 0, 0};
        o.silence_ms = 0;
        CampAggregate(&o, 1, guest, near_host, c);
        CHECK(c.d_h < kBackHostNearM && CampDecide(c).act == OwnAct::none);
    }
    {
        // a forced member forces the camp
        CampMember m[2] = {Mem(true, 20), Mem(true, 25)};
        m[1].forced = true;
        CampIn c;
        CampAggregate(m, 2, guest, host, c);
        CHECK(c.forced && CampDecide(c).reason == OwnReason::forced);
    }
}

// The camp machine end to end (review rv-correctness R11; the model of TestOwnProperty): a camp of 1..6 members, each with
// P1b's HostOwn / GuestOwn, delayed in-order messages (one channel each way), her split and hard refusals, a member that dies on
// her side, random places and load, the host's 2 Hz decision through CampAggregate / CampDecide / CampStragglerOk as
// CampHostTick runs them. Invariants: no two thinking copies outside the protocol's windows; zero thinking bounded; no
// proximity take / back sooner than the hold after the camp's last change; a split refusal never sends the camp back; a member
// dead on her side never offered again.
void TestCampOwnProperty() {
    std::uint32_t rng = 777;
    auto rnd = [&](unsigned n) {
        rng = rng * 1664525u + 1013904223u;
        return (rng >> 8) % n;
    };
    constexpr double kLongMin = 2500, kLongMax = 6000;
    auto delay = [&]() { return rnd(25) == 0 ? kLongMin + rnd(static_cast<unsigned>(kLongMax - kLongMin) + 1) : static_cast<double>(rnd(401)); };
    struct Msg {
        int m;  // the member
        bool own;  // OWN (else OWNACK)
        std::uint16_t epoch;
        Owner owner;
        std::uint8_t result, refusal;
        double at;
    };
    unsigned two = 0, timeout_two = 0, bad_end = 0, takes = 0, backs = 0, splits = 0, hards = 0, stragglers = 0, split_backs = 0, quick = 0,
             dead_offers = 0, unreal_backs = 0, prox_backs = 0, timeouts = 0;
    double worst_zero = 0;
    for (int run = 0; run < 3000; ++run) {
        const int n = 1 + static_cast<int>(rnd(6));
        std::vector<HostOwn> e(n);
        std::vector<GuestOwn> g(n);
        std::vector<double> no_take(n, -1e18), zero_since(n, -1), timeout_since(n, -1), bad_since(n, -1);
        std::vector<std::uint16_t> timeout_epoch(n, 0);
        std::vector<bool> dead_there(n, false), dead_known(n, false), down_host(n, false);
        std::vector<std::array<double, 3>> pos(n);
        const double spread = 5 + rnd(60);
        for (int i = 0; i < n; ++i) pos[i] = {static_cast<double>(rnd(static_cast<unsigned>(spread))), static_cast<double>(rnd(static_cast<unsigned>(spread))), 0};
        double gp[3] = {static_cast<double>(rnd(200)), 0, 0}, hp[3] = {static_cast<double>(rnd(300)), 50, 0};
        double hold_until = -1e18, last_change = -1e18, last_take = -1e18, last_back = -1e18;
        bool refused = false, hard_seen = false, loaded = rnd(4) != 0;
        double unreal_from = -1;
        std::vector<Msg> to_g, to_h;
        double now = 0;
        const int steps = 400 + static_cast<int>(rnd(600));
        const int dies = rnd(3) == 0 ? static_cast<int>(rnd(static_cast<unsigned>(n))) : -1;
        auto send = [&](int i, Owner o) { to_g.push_back(Msg{i, true, e[i].epoch, o, 0, 0, now + delay()}); };
        auto ack = [&](int i, std::uint16_t ep, Owner o, std::uint8_t res, std::uint8_t refusal) { to_h.push_back(Msg{i, false, ep, o, res, refusal, now + delay()}); };
        for (int st = 0; st <= steps + 400; ++st, now += 50) {
            const bool live = st < steps;
            for (auto* q : {&to_g, &to_h})
                for (std::size_t k = 1; k < q->size(); ++k) (*q)[k].at = (std::max)((*q)[k].at, (*q)[k - 1].at);
            // the world moves: her walk, the host's walk, the load, a death on her side, a whole-camp unrealized spell there
            if (live) {
                gp[0] = (std::max)(0.0, (std::min)(300.0, gp[0] + static_cast<double>(rnd(9)) - 4.0));
                hp[0] = (std::max)(0.0, (std::min)(300.0, hp[0] + static_cast<double>(rnd(9)) - 4.0));
                if (rnd(400) == 0) loaded = !loaded;
                if (dies >= 0 && g[dies].owned && rnd(100) == 0) dead_there[dies] = true;
                if (unreal_from < 0 && rnd(1500) == 0) unreal_from = now;
                else if (unreal_from >= 0 && rnd(80) == 0) unreal_from = -1;
                for (int i = 0; i < n; ++i)
                    if (rnd(3000) == 0) down_host[i] = !down_host[i];
            }
            while (!to_g.empty() && to_g.front().at <= now) {
                const Msg m = to_g.front();
                to_g.erase(to_g.begin());
                const int i = m.m;
                switch (GuestOnOwn(g[i], m.epoch, m.owner)) {
                    case GuestAct::take: {
                        if (dead_there[i] && dead_known[i]) ++dead_offers;
                        std::uint8_t refusal = 0;
                        if (dead_there[i]) refusal = static_cast<std::uint8_t>(OwnRefuse::dead);
                        else if (rnd(12) == 0) refusal = static_cast<std::uint8_t>(OwnRefuse::not_realized);
                        else if (rnd(30) == 0) refusal = static_cast<std::uint8_t>(OwnRefuse::held);
                        else if (rnd(60) == 0) refusal = static_cast<std::uint8_t>(OwnRefuse::cutscene);
                        if (refusal) ack(i, m.epoch, Owner::host, 2, refusal);
                        else g[i].owned = true, ack(i, m.epoch, Owner::guest, 0, 0);
                        break;
                    }
                    case GuestAct::release: g[i].owned = false, ack(i, m.epoch, Owner::host, 1, 0); break;
                    case GuestAct::ack_taken: ack(i, m.epoch, Owner::guest, 0, 0); break;
                    case GuestAct::ack_released: ack(i, m.epoch, Owner::host, 1, 0); break;
                    case GuestAct::ignore: break;
                }
                if (timeout_since[i] >= 0 && (m.epoch == timeout_epoch[i] || EpochNewer(m.epoch, timeout_epoch[i]))) timeout_since[i] = -1;
            }
            while (!to_h.empty() && to_h.front().at <= now) {
                const Msg m = to_h.front();
                to_h.erase(to_h.begin());
                const int i = m.m;
                const bool was_back = e[i].owner == Owner::host && e[i].pending;
                const AckAct a = HostOnAck(e[i], m.epoch, m.result, now, kCampRefuseHoldMs);
                if (a == AckAct::refused_back) {
                    if (CampRefuseSplits(m.refusal)) {
                        ++splits;
                        const double ms = CampNoTakeMs(m.refusal);
                        no_take[i] = ms < 0 ? 1e18 : now + ms;
                        if (m.refusal == static_cast<std::uint8_t>(OwnRefuse::dead)) dead_known[i] = true;
                    } else {
                        ++hards, hard_seen = true, refused = true;
                    }
                } else if (a == AckAct::release && was_back && dead_there[i]) {
                    // a back of one dead there: her outcome carried, never taken again this world (CarryOutcomeTo)
                    no_take[i] = 1e18, dead_known[i] = true, down_host[i] = true;
                }
            }
            // the host's decision (2 Hz)
            if (live && st % 10 == 0) {
                std::vector<CampMember> mm(n);
                for (int i = 0; i < n; ++i) {
                    CampMember& c = mm[i];
                    c.hers = e[i].owner == Owner::guest;
                    c.pending = e[i].pending;
                    c.ack_wait_ms = now - e[i].t_sent;
                    c.realized_host = loaded;
                    c.pos[0] = pos[i][0], c.pos[1] = pos[i][1], c.pos[2] = 0;
                    c.phase = 0;
                    if (c.hers) {
                        c.standing = !dead_there[i];
                        c.silence_ms = rnd(200) == 0 ? kOwnSilenceMs + 500 : 100;
                        const bool bad = unreal_from >= 0;
                        if (bad && bad_since[i] < 0) bad_since[i] = now;
                        if (!bad) bad_since[i] = -1;
                        c.bad_guest = bad, c.bad_for_ms = bad ? now - bad_since[i] : 0;
                        c.phase = rnd(10) == 0 ? kPhaseCaution : 0;
                    } else {
                        c.standing = !down_host[i];
                        c.takeable = c.standing && now >= no_take[i];
                    }
                }
                CampIn ci;
                const CampAgg ag = CampAggregate(mm.data(), mm.size(), gp, hp, ci);
                ci.guest_ok = rnd(30) != 0, ci.host_ok = rnd(30) != 0;
                ci.hold_over = now >= hold_until;
                ci.since_change_ms = now - last_change;
                ci.range_m = 270;
                ci.refused = refused;
                ci.outcomes_ok = true;
                ci.cp_phase = 0;
                const OwnVerdict v = CampDecide(ci);
                if (v.act == OwnAct::take) {
                    if (now - last_back < kCampHoldMs) ++quick;
                    unsigned took = 0, failed = 0;
                    for (int i = 0; i < n; ++i) {
                        if (e[i].owner != Owner::host || e[i].pending || !mm[i].takeable) continue;
                        if (HostTake(e[i], now, rnd(40) != 0, kCampHoldMs, kCampRefuseHoldMs)) ++took, ++takes, send(i, Owner::guest);
                        else ++failed;
                    }
                    if (failed && took)
                        for (int i = 0; i < n; ++i)
                            if (e[i].owner == Owner::guest && HostBack(e[i], now, kCampHoldMs)) ++backs, send(i, Owner::host);
                    hold_until = now + kCampHoldMs, last_change = last_take = now, refused = false, hard_seen = false;
                } else if (v.act == OwnAct::back || v.act == OwnAct::forced_back) {
                    if (v.reason == OwnReason::refused && !hard_seen) ++split_backs;
                    if (v.reason == OwnReason::unrealized) ++unreal_backs;
                    if (v.reason == OwnReason::far_off || v.reason == OwnReason::host_near || v.reason == OwnReason::host_nearer) {
                        ++prox_backs;
                        if (now - last_take < kCampHoldMs) ++quick;
                    }
                    for (int i = 0; i < n; ++i)
                        if (e[i].owner == Owner::guest && HostBack(e[i], now, kCampHoldMs)) ++backs, send(i, Owner::host);
                    hold_until = now + kCampHoldMs, last_change = last_back = now, refused = false, hard_seen = false;
                } else if (ag.hers && CampStragglerOk(ci)) {
                    for (int i = 0; i < n; ++i) {
                        if (e[i].owner != Owner::host || e[i].pending || !mm[i].takeable) continue;
                        if (HostTake(e[i], now, true, kCampHoldMs, kCampRefuseHoldMs)) ++stragglers, ++takes, send(i, Owner::guest);
                        last_change = now;
                        break;
                    }
                }
            }
            for (int i = 0; i < n; ++i) {
                if (HostBackTimedOut(e[i], now)) ++timeouts, timeout_since[i] = now, timeout_epoch[i] = e[i].epoch;
                const bool host_thinks = !e[i].frozen, guest_thinks = g[i].owned;
                if (host_thinks && guest_thinks) {
                    if (timeout_since[i] >= 0) ++timeout_two;
                    else ++two;
                }
                if (!host_thinks && !guest_thinks) {
                    if (zero_since[i] < 0) zero_since[i] = now;
                    worst_zero = (std::max)(worst_zero, now - zero_since[i]);
                } else {
                    zero_since[i] = -1;
                }
            }
        }
        if (!to_g.empty() || !to_h.empty()) ++bad_end;
        for (int i = 0; i < n; ++i)
            if (e[i].pending || ((e[i].owner == Owner::guest) != g[i].owned)) ++bad_end;
    }
    CHECK(two == 0);
    CHECK(bad_end == 0);
    CHECK(worst_zero <= 2 * kAckTimeoutMs + 500 + 100);
    CHECK(quick == 0);         // no proximity take / back within the hold of the camp's last change
    CHECK(split_backs == 0);   // a split refusal never sends the camp back
    CHECK(dead_offers == 0);   // a member known dead on her side is never offered again
    CHECK(takes > 1000 && backs > 300 && splits > 100 && hards > 20 && stragglers > 20 && unreal_backs > 5 && prox_backs > 100 && timeouts > 5);
    std::printf("camp own property: two-thinking %u (in a timeout window %u ticks), bad end %u, worst zero %.0f ms, quick cycles %u, split backs %u, dead offers %u "
                "(takes %u backs %u splits %u hard %u stragglers %u unrealized backs %u proximity backs %u timeouts %u)\n",
                two, timeout_two, bad_end, worst_zero, quick, split_backs, dead_offers, takes, backs, splits, hards, stragglers, unreal_backs, prox_backs, timeouts);
}

void TestCarryOutcome() {
    CHECK(CarryOutcome(Defeat::none, true) == -1);
    CHECK(CarryOutcome(Defeat::faint, true) == 0);
    CHECK(CarryOutcome(Defeat::sleep, true) == 1);
    CHECK(CarryOutcome(Defeat::restrained, true) == 2);
    CHECK(CarryOutcome(Defeat::dead, true) == 0);  // never a standing MULE back: a kill carried as a faint
    CHECK(CarryOutcome(Defeat::other, true) == 0);
    for (std::uint8_t d = 0; d < kDefeats; ++d) {
        const Defeat dd = static_cast<Defeat>(d);
        CHECK(CarryOutcome(dd, false) == -1);
        CHECK(CarryOutcome(dd, true) <= 2);                  // never k 3 (dead) this round
        CHECK((CarryOutcome(dd, true) >= 0) == (d != 0));  // every down is carried
    }
}

void TestFollowerGuard() {
    CHECK(FollowerGuard(0, 0) == Guard::show);
    CHECK(FollowerGuard(kNoEntryHideMs, 0) == Guard::show);
    CHECK(FollowerGuard(kNoEntryHideMs + 1, 0) == Guard::hide);
    CHECK(FollowerGuard(100, kMemberGuardMs - 1) == Guard::show);
    CHECK(FollowerGuard(100, kMemberGuardMs) == Guard::hide);
}

// Round 23 C (docs/enemy-sync.md 14.9): a player down (death or KnotSpace): hold, hide or the usual rules.
void TestSamDown() {
    // down from the game state: GameOver (death) or KnotSpace; a faint, a stagger or nothing never
    CHECK(DownOfState(0) == 0);
    CHECK(DownOfState(kGsFaint) == 0);
    CHECK(DownOfState(kGsKnotSpace) == static_cast<std::uint8_t>(PlayerDown::knot));
    CHECK(DownOfState(kGsGameOver) == static_cast<std::uint8_t>(PlayerDown::dead));
    CHECK(DownOfState(kGsGameOver | kGsKnotSpace | kGsFaint) == static_cast<std::uint8_t>(PlayerDown::dead));
    CHECK(DownOfState(kGsKnotSpace | kGsFaint) == static_cast<std::uint8_t>(PlayerDown::knot));
    CHECK(DownOfState(1ull << 8) == 0);  // Subspace alone: not down
    for (std::uint8_t c = 0; c <= kPlayerDownMax; ++c) CHECK(std::strcmp(PlayerDownText(c), "?") != 0);
    CHECK(std::strcmp(PlayerDownText(kPlayerDownMax + 1), "?") == 0);
    // the sender's latch: down at once, back after 1 s of not down; a flicker under 1 s keeps it down
    DownLatch l;
    CHECK(l.Step(0, 0) == 0);
    CHECK(l.Step(2, 100) == 2);
    CHECK(l.Step(0, 200) == 2);
    CHECK(l.Step(0, 1100) == 2);
    CHECK(l.Step(2, 1150) == 2);  // flicker: the clock starts again
    CHECK(l.Step(0, 1200) == 2);
    CHECK(l.Step(0, 2199) == 2);
    CHECK(l.Step(0, 2200) == 0);
    CHECK(l.Step(1, 3000) == 1);
    CHECK(l.Step(2, 3010) == 2);  // a new reason at once
    l.Reset();
    CHECK(l.code == 0 && l.clear_since < 0);
    // the receiver's changes
    CHECK(DownStep(0, 0) == DownChange::none && DownStep(2, 2) == DownChange::none);
    CHECK(DownStep(0, 2) == DownChange::down && DownStep(2, 0) == DownChange::back && DownStep(2, 1) == DownChange::reason);
    // review: no beacon keeps the code in force ("unknown", never "back"); the fake wins; a beacon's 0 is "back"
    CHECK(PeerDownNow(-1, false, 0, 1) == 1 && PeerDownNow(-1, false, 0, 0) == 0 && PeerDownNow(-1, false, 3, 2) == 2);
    CHECK(DownStep(1, PeerDownNow(-1, false, 0, 1)) == DownChange::none);
    CHECK(PeerDownNow(-1, true, 0, 1) == 0 && DownStep(1, PeerDownNow(-1, true, 0, 1)) == DownChange::back);
    CHECK(PeerDownNow(-1, true, 2, 0) == 2 && PeerDownNow(3, true, 0, 0) == 3 && PeerDownNow(0, false, 0, 4) == 0);
    // review: sync off / no local Sam while the host's Sam is down keeps the guest's mirrors (never the host, never with the key off)
    CHECK(DownKeepsOff(true, 1, true) && DownKeepsOff(true, 2, true) && DownKeepsOff(true, 3, true));
    CHECK(!DownKeepsOff(true, 0, true) && !DownKeepsOff(false, 1, true) && !DownKeepsOff(true, 1, false));
    // review: the not-holding retry waits while he is down; the final unhides never wait
    CHECK(NotHoldUnhideNow(false, false) && !NotHoldUnhideNow(false, true) && NotHoldUnhideNow(true, true) && NotHoldUnhideNow(true, false));
    // ownership: frozen while either player is down, only with the key on
    CHECK(!DownFreezesOwnership(true, false, false));
    CHECK(DownFreezesOwnership(true, true, false) && DownFreezesOwnership(true, false, true) && DownFreezesOwnership(true, true, true));
    CHECK(!DownFreezesOwnership(false, true, true));
    // the decision for one frozen copy
    DownIn d;
    d.radius_m = 120.0;
    // near, posed: held in its last pose
    d.dist_m = 30.0, d.posed = true;
    CHECK(DownHold(d) == DownAct::hold);
    // near, never posed: hidden (kept frozen)
    d.posed = false;
    CHECK(DownHold(d) == DownAct::hide);
    // near, never posed but our own game hides it already: held (never touched)
    d.hidden_here = true;
    CHECK(DownHold(d) == DownAct::hold);
    d.hidden_here = false;
    // an unknown distance counts as near
    d.dist_m = -1;
    CHECK(DownHold(d) == DownAct::hide);
    d.posed = true;
    CHECK(DownHold(d) == DownAct::hold);
    // the radius edge: within, inclusive
    d.dist_m = 120.0;
    CHECK(DownHold(d) == DownAct::hold);
    // far and standing: the usual rules (M0's release, as before)
    d.dist_m = 120.5;
    CHECK(DownHold(d) == DownAct::usual);
    d.posed = false;
    CHECK(DownHold(d) == DownAct::usual);
    // far and down (faint, sleep, restrained, dead): hidden, never given back alive (it would stand up)
    for (std::uint8_t df : {static_cast<std::uint8_t>(Defeat::faint), static_cast<std::uint8_t>(Defeat::sleep), static_cast<std::uint8_t>(Defeat::restrained),
                            static_cast<std::uint8_t>(Defeat::dead)}) {
        d.defeat = df;
        CHECK(DownHold(d) == DownAct::hide);
        d.dist_m = 10.0, d.posed = true;
        CHECK(DownHold(d) == DownAct::hold);  // near and down: held lying (it stays down)
        d.dist_m = 120.5, d.posed = false;
    }
    d.defeat = static_cast<std::uint8_t>(Defeat::none);
    // the lease verdicts the hold overrides
    for (LeaseVerdict v : {LeaseVerdict::keep, LeaseVerdict::release_silence, LeaseVerdict::release_host_unrealized, LeaseVerdict::release_host_hidden,
                           LeaseVerdict::release_host_far, LeaseVerdict::release_range})
        CHECK(DownOverrides(v));
    for (LeaseVerdict v : {LeaseVerdict::none, LeaseVerdict::hold, LeaseVerdict::release_off, LeaseVerdict::release_unrealized, LeaseVerdict::forget_taken,
                           LeaseVerdict::release_vehicle})
        CHECK(!DownOverrides(v));
    DownIn n;
    n.dist_m = 5.0, n.posed = true;
    CHECK(DownLease(true, true, LeaseVerdict::release_host_hidden, n) == DownAct::hold);   // the KnotSpace hide: held, not released alive
    CHECK(DownLease(true, true, LeaseVerdict::release_silence, n) == DownAct::hold);       // his frames stopped: held
    CHECK(DownLease(true, true, LeaseVerdict::keep, n) == DownAct::hold);                  // frames still come: held all the same
    CHECK(DownLease(true, false, LeaseVerdict::release_host_hidden, n) == DownAct::usual); // he is up: the old rules
    CHECK(DownLease(false, true, LeaseVerdict::release_host_hidden, n) == DownAct::usual); // enemy_down_hold=0: the old rules
    CHECK(DownLease(true, true, LeaseVerdict::forget_taken, n) == DownAct::usual);         // a game sequence took ours: forgotten as before
    CHECK(DownLease(true, true, LeaseVerdict::release_vehicle, n) == DownAct::usual);
    CHECK(DownLease(true, true, LeaseVerdict::release_unrealized, n) == DownAct::usual);
    n.posed = false;
    CHECK(DownLease(true, true, LeaseVerdict::release_host_hidden, n) == DownAct::hide);
    CHECK(std::strcmp(DownActText(DownAct::hold), "held") == 0 && std::strcmp(DownActText(DownAct::hide), "hidden") == 0 &&
          std::strcmp(DownActText(DownAct::usual), "usual") == 0);
    // no new mirror while the owner's player is down (after the cutscene refusal)
    EligibleIn e;
    e.owner_down = true;
    CHECK(Eligible(e) == Refusal::owner_down);
    CHECK(std::strcmp(RefusalText(Refusal::owner_down), "?") != 0);
    e.cutscene = true;
    CHECK(Eligible(e) == Refusal::cutscene);
    // the keys
    bool hold = true;
    CHECK(cfgval::SetDownKey(hold, "enemy_down_hold", false) && !hold);
    CHECK(cfgval::SetDownKey(hold, "enemy_down_hold", true) && hold);
    CHECK(!cfgval::SetDownKey(hold, "enemy_down", false) && hold);
    static_assert(cfgval::kDownHoldRadiusDefault == 120.0, "the default radius");
    CHECK(cfgval::DownRadius("200", 120.0) == 200.0 && cfgval::DownRadius("19", 120.0) == 120.0 && cfgval::DownRadius("x", 120.0) == 120.0);
}

void TestSingleRule() {
    erules::SingleIn s;
    CHECK(erules::SingleRule(s) == erules::SingleVerdict::single);  // off
    s.rule_on = true;
    s.cp_phase = 2, s.off_deg = 20, s.dist_m = 10, s.alerted = true;
    CHECK(erules::SingleRule(s) == erules::SingleVerdict::single);
    s.cp_phase = 5;
    CHECK(erules::SingleRule(s) == erules::SingleVerdict::hold_walk);
    s.camp_allowed = true;
    CHECK(erules::SingleRule(s) == erules::SingleVerdict::camp);
    s.cp_phase = -1, s.earlier_hers = 1;
    CHECK(erules::SingleRule(s) == erules::SingleVerdict::camp);
    s.earlier_hers = 0;
    CHECK(erules::SingleRule(s) == erules::SingleVerdict::single);
    s.off_deg = 61;
    CHECK(erules::SingleRule(s) == erules::SingleVerdict::hold_behind);
    s.off_deg = 60, s.dist_m = 26;
    CHECK(erules::SingleRule(s) == erules::SingleVerdict::hold_behind);
    s.dist_m = 25, s.alerted = false;
    CHECK(erules::SingleRule(s) == erules::SingleVerdict::hold_behind);
    s.alerted = true;
    CHECK(erules::SingleRule(s) == erules::SingleVerdict::single);
    s.off_deg = -1;
    CHECK(erules::SingleRule(s) == erules::SingleVerdict::hold_behind);
    CHECK(std::strcmp(erules::SingleVerdictText(erules::SingleVerdict::hold_walk), "RULE-W") == 0);
    // V1: the lease's vehicle release waits while a single of its camp is ours
    erules::LeaseIn l;
    l.frozen_by_us = true, l.vehicle_check = true, l.in_vehicle_host = true;
    CHECK(erules::Lease(l) == erules::LeaseVerdict::release_vehicle);
    l.hold_vehicle_single = true;
    CHECK(erules::Lease(l) == erules::LeaseVerdict::keep);
    l.frame_age_ms = 5000;
    CHECK(erules::Lease(l) == erules::LeaseVerdict::release_silence);
}

int main() {
    TestCampGeometry();
    TestCampTake();
    TestCampTakeFirst();
    TestCampBack();
    TestCampSplits();
    TestCampAggregate();
    TestCampOwnProperty();
    TestCarryOutcome();
    TestFollowerGuard();
    TestHostDecideTake();
    TestHostDecideBack();
    TestHostOwnMachine();
    TestGuestOnOwn();
    TestOwnProperty();
    TestPoseAlloc();
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
    TestHitVerdict();
    TestHitDedupe();
    TestFrames();
    TestHostSerial();
    TestRateLimit();
    TestApplyGate();
    TestOutcomes();
    TestDeadRelease();
    TestPoseDead();
    TestHitValues();
    TestRestrain();
    TestMirrorsB();
    TestSamDown();
    TestSingleRule();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
