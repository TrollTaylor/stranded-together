// Round 27 M1 (native/mod/enemy_l1_rules.hpp): the L1 probe's attribution, trackers and decision.
#include <cstdio>
#include <cstring>

#include "enemy_l1_rules.hpp"

using namespace st::l1rules;

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

void TestGeometry() {
    const double root[3] = {0, 0, 0};
    const double mid[3] = {1, 0, 1};
    CHECK(Near(SegDist(mid, root), 1.0));
    const double above[3] = {0, 0, 2.8};
    CHECK(Near(SegDist(above, root), 1.0));
    const double below[3] = {0, 0, -0.5};
    CHECK(Near(SegDist(below, root), 0.5));
    const double her[3] = {10, 0, 0}, sam[3] = {-10, 0, 0};
    const double at_her[3] = {10.5, 0, 1.0}, at_sam[3] = {-9.2, 0, 1.0}, mid2[3] = {0, 0, 1};
    CHECK(Nearest(at_her, true, her, true, sam) == kHer);
    CHECK(Nearest(at_sam, true, her, true, sam) == kSam);
    CHECK(Nearest(mid2, true, her, true, sam) == kNone);
    CHECK(Nearest(at_her, false, her, true, sam) == kNone);  // her unknown: not attributed to her
    // both within the cap: the nearer
    const double h2[3] = {0, 0, 0}, s2[3] = {1, 0, 0}, p[3] = {0.3, 0, 1};
    CHECK(Nearest(p, true, h2, true, s2) == kHer);
    // the ray
    const double o[3] = {0, 0, 0}, d[3] = {1, 0, 0}, q[3] = {5, 2, 0}, back[3] = {-3, 4, 0};
    CHECK(Near(RayDist(o, d, q), 2.0));
    CHECK(Near(RayDist(o, d, back), 5.0));  // behind the origin: to the origin
    const double zero[3] = {0, 0, 0};
    CHECK(Near(RayDist(o, zero, q), std::sqrt(29.0)));
    // facing
    CHECK(Near(FacingOff(0.0, o, q), std::atan2(2.0, 5.0) * 180.0 / kPi));
    const double west[3] = {-5, 0, 0};
    CHECK(Near(FacingOff(0.0, o, west), 180.0));
    CHECK(Near(FacingOff(kPi - 0.1, o, west), 0.1 * 180.0 / kPi, 1e-9));
    CHECK(Near(FacingOff(-kPi + 0.1, o, west), 0.1 * 180.0 / kPi, 1e-9));
    CHECK(std::strcmp(WhoText(kHer), "her") == 0 && std::strcmp(WhoText(kSam), "Sam") == 0 && std::strcmp(WhoText(7), "none") == 0);
}

void TestRoundLine() {
    const double mule[3] = {0, 0, 0}, her[3] = {25, 0, 0}, sam[3] = {0, 150, 0};
    const double aim[3] = {25, 0, 1.3};
    RoundAt r = RoundLine(mule, 0.0, true, aim, true, her, true, sam);
    CHECK(r.who == kHer && r.her < 0.2 && r.sam > 100);
    // no aim point: along the facing (level from the chest: 0.1 m above her chest)
    r = RoundLine(mule, 0.0, false, aim, true, her, true, sam);
    CHECK(r.who == kHer && Near(r.her, 0.1, 1e-9));
    // facing Sam
    r = RoundLine(mule, kPi / 2, false, aim, true, her, true, sam);
    CHECK(r.who == kSam);
    // facing away from both
    r = RoundLine(mule, kPi, false, aim, true, her, true, sam);
    CHECK(r.who == kNone);
    // her unknown
    r = RoundLine(mule, 0.0, true, aim, false, her, true, sam);
    CHECK(r.who == kNone && r.her > 1e299);
}

Sample Base(double t) {
    Sample s;
    s.t = t;
    s.realized = true;
    s.her_ok = s.sam_ok = true;
    s.her[0] = 25;  // her 25 m in front (facing +x)
    s.sam[0] = -150;  // Sam 150 m behind
    s.yaw = 0;
    return s;
}

void TestTracker() {
    Arm a;
    Sample s = Base(0);
    Add(a, s);
    CHECK(a.noticed < 0 && a.turned_her < 0 && a.start_her == 25 && a.turned_sam < 0);
    s = Base(250);
    s.pv_her = true;
    Add(a, s);
    CHECK(a.noticed == 250 && a.turned_her == 250);  // already facing her: turned at the notice
    // she is approached by 2 m
    s = Base(500);
    s.mule[0] = 2.5;
    s.aim_ok = true;
    s.aim[0] = 25, s.aim[2] = 1.3;
    s.shots = 3;  // the first sample's count was 0: 3 rounds, on the aim line at her
    RoundAt last;
    CHECK(Add(a, s, &last) == 3);
    CHECK(a.approached_her && !a.approached_sam && a.fired == 3 && a.at_her == 3 && a.at_sam == 0 && last.who == kHer);
    CHECK(a.aim_n == 1 && a.aim_her == 1 && a.target == kHer);
    // the same count again: no new rounds
    s.t = 750;
    CHECK(Add(a, s) == 0 && a.fired == 3);
    // not realized: counted as a sample only
    Sample nr;
    nr.t = 1000;
    Add(a, nr);
    CHECK(a.samples == 5 && a.realized == 4);
    // aim at Sam's place
    Sample t = Base(1250);
    t.aim_ok = true;
    t.aim[0] = -150, t.aim[2] = 1;
    t.daemon = kSam;
    t.yaw = kPi;  // turned to Sam
    t.shots = 3;
    Add(a, t);
    CHECK(a.aim_n == 3 && a.aim_sam == 1 && a.turned_sam == 1250 && a.daemon_sam == 1 && a.target == kSam);
    CHECK(SamPull(a));
    CHECK(Near(a.first_off_sam, 180.0, 1e-9) && Near(a.last_off_sam, 0.0, 1e-9) && TurnedDuring(a) == kSam);
}

void TestTurnedDuring() {
    Arm g;
    Sample s = Base(0);
    s.yaw = kPi / 2;  // facing neither
    s.lk_who = kHer;
    s.cp_who = kSam;
    Add(g, s);
    CHECK(TurnedDuring(g) == kNone && g.lk_her == 1 && g.cp_sam == 1);
    s.t = 3000, s.yaw = 0.1;  // now facing her
    Add(g, s);
    CHECK(TurnedDuring(g) == kHer && g.turned_her < 0);  // no PV at her: a turn, not a notice
    Arm h;
    s.t = 0;
    Add(h, s);  // facing her from the start: no turn
    CHECK(TurnedDuring(h) == kNone);
}

void TestRaysAndValidity() {
    // two rounds with real lines: one at Sam although the aim point is at her (the joint-aim case), one at her; a third on the
    // sample's line (no ray)
    Arm a;
    Sample s = Base(0);
    s.stim = true;
    Add(a, s);
    s.t = 250, s.pv_her = true, s.aim_ok = true, s.aim[0] = 25, s.aim[2] = 1.3, s.shots = 3;
    s.ray_n = 2;
    s.ray_o[0][2] = 1.4, s.ray_d[0][0] = -1;  // backwards along -x: Sam's chest at (-150, 0, 1.3)
    s.ray_o[1][2] = 1.4, s.ray_d[1][0] = 1;   // at her
    RoundAt last;
    CHECK(Add(a, s, &last) == 3);
    CHECK(a.fired == 3 && a.at_sam == 1 && a.at_her == 2 && a.rays == 2 && last.who == kHer);
    CHECK(!a.pv_at_start && a.stim_n == 2 && !a.stim_lost);
    // her stimulus gone mid-arm
    s.t = 500, s.stim = false, s.ray_n = 0;
    Add(a, s);
    CHECK(a.stim_lost && Invalid(a));
    // a PlayerVisual entry at her in the first realized sample: contaminated
    Arm b;
    Sample t = Base(0);
    t.pv_her = true;
    Add(b, t);
    CHECK(b.pv_at_start && Invalid(b));
    // no stimulus at all (A, G): never "lost"
    Arm c;
    Add(c, Base(0));
    Add(c, Base(250));
    CHECK(!c.stim_lost && c.stim_n == 0);
}

void TestTrackerBaseline() {
    // shots before the arm (the first sample's count is the baseline)
    Arm a;
    Sample s = Base(0);
    s.shots = 10;
    Add(a, s);
    CHECK(a.fired == 0 && a.shots0 == 10);
    s.t = 250;
    s.shots = 12;
    CHECK(Add(a, s) == 2 && a.fired == 2);
    // no PV: no aim counted before the notice
    s.t = 500;
    s.aim_ok = true;
    s.aim[0] = 25;
    Add(a, s);
    CHECK(a.aim_n == 0 && a.noticed < 0);
}

void TestSwitch() {
    Arm d;
    Sample s = Base(0);
    Add(d, s);  // her alone, not yet seen
    s.t = 3000, s.pv_her = true;  // seen: the target is her
    Add(d, s);
    CHECK(d.target == kHer && d.last_target[0] == kHer);
    s.t = 6000, s.sam[0] = 15, s.sam[1] = 9, s.pv_sam = true;  // Sam in front too, off her line: both seen (no unique target)
    Add(d, s);
    CHECK(d.target == kNone && d.last_target[0] == kHer);
    s.t = 10000, s.phase = 1, s.pv_her = false;  // she hides
    Add(d, s);
    CHECK(d.phase_at[1] == 10000 && d.switch_sam == 0 && d.last_target[1] == kSam);
    s.t = 20000, s.phase = 2, s.pv_sam = false, s.pv_her = false;  // Sam far, she is back
    s.sam[0] = -150, s.sam[1] = 0;
    Add(d, s);
    CHECK(d.phase_at[2] == 20000 && d.switch_her == -1);
    s.t = 22500, s.pv_her = true;
    Add(d, s);
    CHECK(d.switch_her == 2500 && DFull(d));
    // phase 0 ended on Sam (the tie went to him): hiding her is not a switch (n/a), and the arm is not full
    Arm e;
    Sample u = Base(0);
    u.sam[0] = 15, u.sam[1] = 9, u.pv_sam = true;
    Add(e, u);
    CHECK(e.last_target[0] == kSam);
    u.t = 8000, u.phase = 1;
    Add(e, u);
    CHECK(e.switch_sam == kNa && !DFull(e));
    // phase 1 never on Sam: the return to her is n/a too
    Arm f;
    Sample w = Base(0);
    w.pv_her = true;
    Add(f, w);
    w.t = 8000, w.phase = 1, w.pv_her = false;
    Add(f, w);  // nobody seen in phase 1
    w.t = 16000, w.phase = 2, w.pv_her = true;
    Add(f, w);
    CHECK(f.switch_sam == -1 && f.switch_her == kNa);
    // the aim wins over the PVs
    Sample a = Base(0);
    a.pv_sam = true;
    a.aim_ok = true;
    a.aim[0] = 25, a.aim[2] = 1;
    CHECK(SampleTarget(a) == kHer);
    a.aim[0] = 0;  // an aim at no one: the unique PV
    CHECK(SampleTarget(a) == kSam);
}

Arm Full(bool gun) {
    Arm a;
    a.noticed = 1000, a.turned_her = 1500, a.approached_her = true;
    if (gun) a.aim_n = 10, a.aim_her = 9, a.fired = 5, a.at_her = 4;
    return a;
}

void TestDecide() {
    Arm b = Full(true), c = Full(false), d;
    d.switch_sam = 1500, d.switch_her = 2000;
    CHECK(Decide(b, c, d) == kPFull);
    CHECK(std::strcmp(DecisionText(kPFull), "P-full") == 0 && std::strcmp(DecisionText(-3), "FAIL") == 0);
    // no notice in both B and C
    Arm nb, nc;
    CHECK(Decide(nb, nc, d) == kFail);
    // noticed in C only: B has no aim data -> undecided (review round 27 M1)
    CHECK(Decide(nb, c, d) == kUndecided);
    CHECK(std::strcmp(DecisionText(kUndecided), "undecided") == 0);
    // the goal reads off: undecided, never P-notice
    Arm bg = b;
    bg.no_goal_reads = true;
    CHECK(Decide(bg, c, d) == kUndecided);
    // noticed with the goal reads on and no aim at all: it never aimed -> P-notice
    Arm bn = b;
    bn.aim_n = 0, bn.aim_her = 0;
    CHECK(Decide(bn, c, d) == kPNotice);
    // an invalid B or C (calm timeout, PV at her at the start, her stimulus lost): undecided
    Arm bi = b;
    bi.calm_timeout = true;
    CHECK(Invalid(bi) && Decide(bi, c, d) == kUndecided);
    Arm ci = c;
    ci.stim_lost = true;
    CHECK(Decide(b, ci, d) == kUndecided);
    ci = c;
    ci.pv_at_start = true;
    CHECK(Decide(b, ci, d) == kUndecided);
    // an invalid D: not full
    Arm di = d;
    di.calm_timeout = true;
    CHECK(Decide(b, c, di) == kPAim);
    // aim below 70 %
    Arm b2 = b;
    b2.aim_her = 6;
    CHECK(!AimsAtHer(b2) && Decide(b2, c, d) == kPNotice);
    b2.aim_her = 7;
    CHECK(AimsAtHer(b2) && Decide(b2, c, d) == kPFull);
    // no rounds
    Arm b3 = b;
    b3.fired = 0, b3.at_her = 0;
    CHECK(Decide(b3, c, d) == kPNoFire);
    // rounds mostly not at her
    Arm b4 = b;
    b4.at_her = 2;
    CHECK(!BFull(b4) && Decide(b4, c, d) == kPAim);
    b4.at_her = 3;  // 60 %
    CHECK(BFull(b4));
    // C too slow to notice / to turn / no approach
    Arm c2 = c;
    c2.noticed = 4500, c2.turned_her = 5000;
    CHECK(!CFull(c2) && Decide(b, c2, d) == kPAim);
    c2 = c;
    c2.turned_her = 5100;
    CHECK(!CFull(c2));
    c2 = c;
    c2.approached_her = false;
    CHECK(!CFull(c2));
    // the switch too slow or one way only
    Arm d2 = d;
    d2.switch_her = 3500;
    CHECK(!DFull(d2) && Decide(b, c, d2) == kPAim);
    d2 = d;
    d2.switch_sam = -1;
    CHECK(!DFull(d2));
    // Sam pulls B or C: never full
    Arm c3 = c;
    c3.approached_sam = true;
    CHECK(Decide(b, c3, d) == kPAim);
    Arm b5 = b;
    b5.daemon_sam = 4, b5.daemon_her = 1;
    CHECK(Decide(b5, c, d) == kPAim);
    b5.daemon_her = 4;  // a tie: not a pull
    CHECK(Decide(b5, c, d) == kPFull);
    // a pull with no rounds: P-nofire
    Arm b6 = b3;
    b6.turned_sam = 2000;
    CHECK(Decide(b6, c, d) == kPNoFire);
}

// run 2 (round 27): contamination only when the PV at her is older than her stimulus; the notice is a fresh PV at her
void TestRun2Notice() {
    // the arm's own stimulus made 0.27 s before the first sample, which reads a fresh PV at her: not contamination, a notice
    Arm a;
    Sample s = Base(0);
    s.pv_her = true, s.pv_her_age = 0.0, s.stim_age = 0.27, s.pv_age_min = 0.0;
    Add(a, s);
    CHECK(!a.pv_at_start && !Invalid(a) && a.noticed == 0 && Near(a.pv_age0, 0.0) && Near(a.stim_age0, 0.27));
    // a PV at her 39.5 s old, the stimulus 0.3 s old: contamination
    Arm b;
    s.pv_her_age = 39.5, s.stim_age = 0.3, s.pv_age_min = 39.5;
    Add(b, s);
    CHECK(b.pv_at_start && Invalid(b) && b.noticed < 0 && Near(b.pv_age0, 39.5));
    // the stimulus' age unknown (-1): any PV at her at the start is contamination (the M1 rule)
    Arm c;
    s.pv_her_age = 0.0, s.stim_age = -1;
    Add(c, s);
    CHECK(c.pv_at_start);
    // a stale PV at her is no notice; the first fresh one is
    Arm d;
    Sample t = Base(0);
    t.stim_age = 0.2;
    Add(d, t);
    t.t = 250, t.pv_her = true, t.pv_her_age = 1.2;
    Add(d, t);
    CHECK(d.noticed < 0 && d.pv_her == 1);
    t.t = 500, t.pv_her_age = 0.49;
    Add(d, t);
    CHECK(d.noticed == 500 && !d.pv_at_start);
    // no PV at the start: pv_age0 -1
    CHECK(Near(d.pv_age0, -1.0));
}

// run 2: rounds per switch phase
void TestPhaseRounds() {
    Arm a;
    Sample s = Base(0);
    Add(a, s);
    s.t = 250, s.pv_her = true, s.aim_ok = true, s.aim[0] = 25, s.aim[2] = 1.3, s.shots = 2;  // phase 0: 2 at her
    Add(a, s);
    s.t = 500, s.phase = 1, s.aim[0] = -150, s.aim[2] = 1.3, s.shots = 5;  // phase 1: 3 at Sam
    Add(a, s);
    s.t = 750, s.phase = 2, s.aim[0] = 0, s.aim[1] = 80, s.shots = 6;  // phase 2: 1 at neither
    Add(a, s);
    CHECK(a.fired == 6 && a.at_her == 2 && a.at_sam == 3);
    CHECK(a.fired_ph[0] == 2 && a.fired_ph[1] == 3 && a.fired_ph[2] == 1);
    CHECK(a.at_her_ph[0] == 2 && a.at_her_ph[1] == 0 && a.at_her_ph[2] == 0);
    CHECK(a.at_sam_ph[0] == 0 && a.at_sam_ph[1] == 3 && a.at_sam_ph[2] == 0);
}

// run 2: a round's angles
void TestAngles() {
    const double o[3] = {0, 0, 1.3}, her[3] = {10, 0, 0}, sam[3] = {0, 10, 0};
    const double d[3] = {1, 0, 0};
    CHECK(Near(AngleToChest(o, d, her), 0.0, 1e-6));
    CHECK(Near(AngleToChest(o, d, sam), 90.0, 1e-6));
    const double zero[3] = {0, 0, 0};
    CHECK(AngleToChest(o, zero, her) < 0);
    CHECK(Near(AngleToYaw(d, 0.0), 0.0, 1e-9) && Near(AngleToYaw(d, kPi / 2), 90.0, 1e-9) && Near(AngleToYaw(d, -kPi + 0.1), 180.0 - 0.1 * 180.0 / kPi, 1e-9));
    const double up[3] = {0, 0, 1};
    CHECK(AngleToYaw(up, 0.0) < 0);
    // RoundLineDir: the aim line from the chest, else the level facing
    const double mule[3] = {0, 0, 0}, aim[3] = {25, 0, 1.4};
    double lo[3], ld[3];
    RoundLineDir(mule, kPi / 2, true, aim, lo, ld);
    CHECK(Near(lo[2], kMuleChest) && Near(ld[0], 25.0) && Near(ld[1], 0.0) && Near(ld[2], 0.0));
    RoundLineDir(mule, kPi / 2, false, aim, lo, ld);
    CHECK(Near(ld[0], 0.0, 1e-9) && Near(ld[1], 1.0, 1e-9) && Near(ld[2], 0.0));
}

// run 2: the per-arm geometry (the run's own arm-start lines)
void TestGeomRule() {
    // gun: TA (dz +6.07 at 23.7 m, bearing 1; her aim 1.3 m): pitch about 13.7, valid
    const double p_ta = GeomPitch(6.07, 1.3, 23.7);
    CHECK(p_ta > 13.0 && p_ta < 14.5 && GeomValid(1, true, 6.07, 23.7, 1, p_ta));
    CHECK(!GeomValid(2, true, -1.33, 14.6, 167, GeomPitch(-1.33, 1.3, 14.6)));  // TB: behind it
    CHECK(!GeomValid(5, true, -1.19, 34.2, 177, GeomPitch(-1.19, 1.3, 34.2)));  // TE: too far, behind
    CHECK(GeomValid(6, true, -0.54, 23.0, 21, GeomPitch(-0.54, 1.3, 23.0)));    // TX
    CHECK(!GeomValid(1, true, 0, 8.0, 0, GeomPitch(0, 1.3, 8.0)));              // too near for a gun arm
    CHECK(!GeomValid(1, true, 12.0, 25.0, 0, GeomPitch(12.0, 1.3, 25.0)));      // pitch over 15
    CHECK(!GeomValid(1, true, 0, 20.0, 31, GeomPitch(0, 1.3, 20.0)));           // bearing over 30
    CHECK(!GeomValid(1, false, 0, 20.0, 0, 0) && !GeomValid(1, true, 0, 20.0, -1, 0));  // unread / bearing unknown
    CHECK(GeomValid(0, true, 0, 20.0, 0, GeomPitch(0, 1.3, 20.0)));             // no arm: the gun rule
    // melee: CL (dz 0.00 at 5.8 m, bearing 19) and CH (dz 0.13 at 4.0 m, bearing 1) valid; her on the structure, too far, off
    CHECK(GeomValid(9, true, 0.0, 5.8, 19, 0) && GeomValid(7, true, 0.13, 4.0, 1, 0));
    CHECK(!GeomValid(7, true, 3.4, 4.0, 1, 0) && !GeomValid(9, true, 0, 8.5, 0, 0) && !GeomValid(9, true, 0, 1.5, 0, 0) && !GeomValid(9, true, 0, 5, 46, 0));
    // CS judges our Sam (30.4 m, +3.1 m, bearing 114 in run 2: invalid)
    CHECK(SamArm(8) && !SamArm(7) && MeleeArm(7) && MeleeArm(8) && MeleeArm(9) && !MeleeArm(6));
    CHECK(!GeomValid(8, true, 3.12, 30.4, 114, 0));
    CHECK(Near(GeomPitch(0, kEye, 10), 0.0) && Near(GeomPitch(1, 1.6, 0), 90.0));
}

// run 2: the re-calm's ready rule
void TestCalm() {
    CalmIn c;
    c.phase = 0, c.cp_phase = 0, c.pv_n = 0;
    CHECK(CalmQuiet(c) && !CalmHot(c));
    c.phase = -1;  // the target unread
    CHECK(!CalmQuiet(c) && !CalmHot(c));
    c.phase = 2;
    CHECK(CalmQuiet(c));
    c.phase = 5;
    CHECK(!CalmQuiet(c) && CalmHot(c));
    c.phase = 0, c.members = 4, c.members_up = 1, c.members_hot = 1;  // a member alerted
    CHECK(!CalmQuiet(c) && CalmHot(c));
    c.members_up = 0, c.members_hot = 0, c.cp_phase = 5;  // the CP alerted
    CHECK(!CalmQuiet(c) && CalmHot(c));
    c.cp_phase = -2;  // a CP whose phase did not read
    CHECK(!CalmQuiet(c) && CalmHot(c));
    c.cp_phase = -1;  // no CP known (a loose MULE)
    CHECK(CalmQuiet(c));
    // the stale PV entry (run 2: it never expires) no longer blocks: old and away from both players
    c.cp_phase = 0, c.pv_n = 1, c.pv_min_age = 39.5, c.pv_min_dist = 20.0;
    CHECK(CalmPvStale(c) && CalmQuiet(c));
    c.pv_min_age = 2.0;  // too young
    CHECK(!CalmQuiet(c));
    c.pv_min_age = 5.0, c.pv_min_dist = 2.0;  // at a player
    CHECK(!CalmQuiet(c));
    c.pv_min_dist = 1e300;  // neither player known: far
    CHECK(CalmQuiet(c));
    // held 1.5 s without a break
    CalmHold h;
    CHECK(!CalmStep(h, true, 0) && !CalmStep(h, true, 500) && !CalmStep(h, true, 1000) && CalmStep(h, true, 1500));
    CHECK(!CalmStep(h, false, 2000) && h.since < 0);
    CHECK(!CalmStep(h, true, 2500) && !CalmStep(h, true, 3500) && CalmStep(h, true, 4000));
    CHECK(CalmMaxApplies(12) == 20 && CalmMaxApplies(16) == 27 && CalmMaxApplies(1) == 3 && CalmMaxApplies(100) == 40);
    // run 3: the CP rests at PreCaution (2): 0..2 quiet and not hot; 3 and 5 hot and not quiet; -2 (unread) hot; -1 unchanged
    CalmIn q;
    q.phase = 0, q.pv_n = 0;
    for (int cp = 0; cp <= kCpBaseMax; ++cp) {
        q.cp_phase = cp;
        CHECK(CalmQuiet(q) && !CalmHot(q));
    }
    q.cp_phase = 3;
    CHECK(!CalmQuiet(q) && CalmHot(q));
    q.cp_phase = 5;
    CHECK(!CalmQuiet(q) && CalmHot(q));
    q.cp_phase = -2;
    CHECK(!CalmQuiet(q) && CalmHot(q));
    q.cp_phase = -1;
    CHECK(CalmQuiet(q) && !CalmHot(q));
    // the CP forced only when alerted
    CHECK(!CalmCpForce(-1) && !CalmCpForce(0) && !CalmCpForce(2) && CalmCpForce(3) && CalmCpForce(5) && !CalmCpForce(6));
}

// run 3: the separation angle and op 11's arm-at-geometry rules
void TestRun3() {
    const double m[3] = {0, 0, 0};
    const double her[3] = {20, 0, 0};
    const double same[3] = {150, 0, 3};
    CHECK(Near(SepDeg(m, true, her, true, same), 0.0));
    const double c40 = std::cos(40.0 * kPi / 180.0), s40 = std::sin(40.0 * kPi / 180.0);
    const double at40[3] = {170 * c40, 170 * s40, -2};
    CHECK(std::fabs(SepDeg(m, true, her, true, at40) - 40.0) < 0.01);
    const double behind[3] = {-8, 0, 0};
    CHECK(Near(SepDeg(m, true, her, true, behind), 180.0));
    CHECK(SepDeg(m, false, her, true, same) == -1.0 && SepDeg(m, true, her, false, same) == -1.0);
    CHECK(SepDeg(m, true, m, true, same) == -1.0);  // on the MULE
    // the nominal per code
    CHECK(ArmNominal(1) == 25 && ArmNominal(3) == 22 && ArmNominal(4) == 22 && ArmNominal(5) == 16 && ArmNominal(6) == 16);
    CHECK(ArmNominal(7) == 5 && ArmNominal(9) == 6 && ArmNominal(2) < 0 && ArmNominal(8) < 0 && ArmNominal(0) < 0);
    // her ok: geometry valid and within nominal -4 .. +2 (no window: geometry alone)
    CHECK(ArmHerOk(1, true, 25) && ArmHerOk(1, true, 21) && ArmHerOk(1, true, 27) && !ArmHerOk(1, true, 20.9) && !ArmHerOk(1, true, 27.1));
    CHECK(!ArmHerOk(1, false, 25));
    CHECK(ArmHerOk(2, true, 40) && !ArmHerOk(2, false, 22));
    CHECK(ArmHerOk(5, true, 16) && !ArmHerOk(5, true, 19) && ArmHerOk(7, true, 1.0) && !ArmHerOk(7, true, 7.5));
    // the step: valid at once; else wait until 2500 ms, then go anyway
    CHECK(ArmStep(0, true) == ArmGo::go_valid && ArmStep(3000, true) == ArmGo::go_valid);
    CHECK(ArmStep(0, false) == ArmGo::wait && ArmStep(2499, false) == ArmGo::wait && ArmStep(2500, false) == ArmGo::go_timeout);
}

}  // namespace

int main() {
    TestGeometry();
    TestRoundLine();
    TestTracker();
    TestTrackerBaseline();
    TestRaysAndValidity();
    TestTurnedDuring();
    TestSwitch();
    TestDecide();
    TestRun2Notice();
    TestPhaseRounds();
    TestAngles();
    TestGeomRule();
    TestCalm();
    TestRun3();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
