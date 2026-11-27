// The partner's Odradek, the pure part (native/mod/gear_odra.hpp): the part a STC1 odradek item names, the sender's
// presence rule and its hold, the anchor joint, the HLP_Odradek comparison and the joint deltas. No game.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "gear_odra.hpp"

using namespace st::gearodra;

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

bool Near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) <= eps; }

In Ready() {
    In in;
    in.on = in.partner_cap = in.arm = in.body = in.arm_vis = in.body_vis = in.on_sam = true;
    in.body_flags = 0xC00006;  // round 28 p4 run, idle with the Odradek out
    in.body_state = 0x01;
    return in;
}

void TestPart() {
    CHECK(PartOf(kPartBody) == kPartBody);
    CHECK(PartOf(kPartArm) == kPartArm);
    CHECK(PartOf(0xFF) == kPartBody);  // the kind's old meaning
    CHECK(PartOf(2) == -1);
    CHECK(PartOf(0x7F) == -1);
}

void TestBodyUp() {
    CHECK(BodyUp(0xC00006, 0x01));
    CHECK(BodyUp(0x4, 0x14));   // open (a scan): still up, unlike the scan predicate
    CHECK(BodyUp(0x4, 0x21));
    CHECK(!BodyUp(0x4, 0x22));  // unknown state
    CHECK(!BodyUp(0x4, 0x00));  // not set up
    CHECK(!BodyUp(0xC00002, 0x01));  // bit 2 clear
}

void TestDecide() {
    CHECK(Decide(Ready()) == Why::shown);
    In in = Ready();
    in.on = false;
    CHECK(Decide(in) == Why::off);
    in = Ready();
    in.partner_cap = false;
    CHECK(Decide(in) == Why::no_cap);
    in = Ready();
    in.cutscene = true;
    in.riding = true;
    CHECK(Decide(in) == Why::cutscene);  // the first reason that applies
    in.cutscene = false;
    CHECK(Decide(in) == Why::riding);
    in = Ready();
    in.arm = false;
    CHECK(Decide(in) == Why::no_odradek);
    in = Ready();
    in.body = false;
    CHECK(Decide(in) == Why::no_odradek);
    in = Ready();
    in.arm_vis = false;
    CHECK(Decide(in) == Why::hidden);
    in = Ready();
    in.on_sam = false;
    CHECK(Decide(in) == Why::off_sam);
    in = Ready();
    in.body_state = 0;
    CHECK(Decide(in) == Why::not_up);
    // the one-shot request flag +0x4C7F is not an input at all: nothing in In names it
    for (unsigned w = 0; w < kWhyCount; ++w) CHECK(std::strlen(WhyName(static_cast<Why>(w))) > 2);
    CHECK(std::strcmp(WhyName(static_cast<Why>(200)), "?") == 0);
}

void TestHold() {
    Hold h;
    bool ch = false;
    // shown after kShowAfterMs of shown reads (100 ms reads: the third)
    CHECK(!Step(h, Why::shown, 1000, &ch) && !ch);
    CHECK(!Step(h, Why::shown, 1100, &ch) && !ch);
    CHECK(Step(h, Why::shown, 1200, &ch) && ch);
    CHECK(Step(h, Why::shown, 1300, &ch) && !ch);
    // a hard hide at once
    CHECK(!Step(h, Why::riding, 1400, &ch) && ch);
    CHECK(!Step(h, Why::shown, 1500, &ch));
    CHECK(!Step(h, Why::shown, 1600, &ch));
    CHECK(Step(h, Why::shown, 1700, &ch) && ch);
    CHECK(!Step(h, Why::cutscene, 1800, &ch) && ch);
    CHECK(Step(h, Why::shown, 1900) == false);
    CHECK(Step(h, Why::shown, 2000) == false);
    CHECK(Step(h, Why::shown, 2100) == true);
    CHECK(!Step(h, Why::off, 2200, &ch) && ch);
    CHECK(Step(h, Why::shown, 2300) == false);
    CHECK(Step(h, Why::shown, 2450) == true);
    CHECK(!Step(h, Why::no_cap, 2500, &ch) && ch);
    // the game hiding it: after kHideAfterMs; one hidden read between shown ones does nothing
    h = Hold{};
    Step(h, Why::shown, 0);
    Step(h, Why::shown, 200);
    CHECK(h.shown);
    CHECK(Step(h, Why::hidden, 300));
    CHECK(Step(h, Why::shown, 400));
    CHECK(h.pending_ms < 0);
    CHECK(Step(h, Why::hidden, 500));
    CHECK(Step(h, Why::no_odradek, 600));  // the reason may change: the first disagreeing read counts
    CHECK(!Step(h, Why::hidden, 750, &ch) && ch);
    // not set up: held for kNotUpHideMs (a scan's short states)
    h = Hold{};
    Step(h, Why::shown, 0);
    Step(h, Why::shown, 200);
    CHECK(Step(h, Why::not_up, 300));
    CHECK(Step(h, Why::not_up, 2200));
    CHECK(!Step(h, Why::not_up, 2300, &ch) && ch);
    // a clock that goes back restarts the wait
    h = Hold{};
    Step(h, Why::shown, 5000);
    CHECK(!Step(h, Why::shown, 100));
    CHECK(!Step(h, Why::shown, 200));
    CHECK(Step(h, Why::shown, 250));
    CHECK(h.last == Why::shown);
}

void TestAnchor() {
    CHECK(AnchorJoint(false, -1) == kJointShoulderL);
    CHECK(AnchorJoint(true, -1) == kJointSpine5);
    CHECK(AnchorJoint(true, 35) == 35);
    CHECK(AnchorJoint(false, 5) == 5);
    CHECK(AnchorJoint(false, 77) == kJointShoulderL);  // out of the layout: auto
    CHECK(AnchorJoint(true, 200) == kJointSpine5);
}

void TestHelper() {
    // exactly the helper, axes as rows
    float p[3] = {kHelperT[0], kHelperT[1], kHelperT[2]};
    float r[3][3];
    for (int k = 0; k < 3; ++k)
        for (int c = 0; c < 3; ++c) r[k][c] = kHelperAxes[k][c];
    HelperDelta d = CompareHelper(p, r);
    CHECK(Near(d.cm, 0.0) && Near(d.deg_rows, 0.0, 0.05));
    CHECK(d.deg_cols > 10.0);  // the decoded axes are not symmetric: the two readings differ
    // as columns
    for (int k = 0; k < 3; ++k)
        for (int c = 0; c < 3; ++c) r[k][c] = kHelperAxes[c][k];
    d = CompareHelper(p, r);
    CHECK(Near(d.deg_cols, 0.0, 0.05) && d.deg_rows > 10.0);
    // 1 cm off
    p[0] += 0.01f;
    d = CompareHelper(p, r);
    CHECK(Near(d.cm, 1.0, 0.01));
    p[1] = std::numeric_limits<float>::quiet_NaN();
    CHECK(CompareHelper(p, r).cm > 1e8);
    // the helper's axes are a rotation (orthonormal, det +1)
    const float(&a)[3][3] = kHelperAxes;
    for (int k = 0; k < 3; ++k) CHECK(Near(a[k][0] * a[k][0] + a[k][1] * a[k][1] + a[k][2] * a[k][2], 1.0, 1e-4));
    const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                       a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    CHECK(Near(det, 1.0, 1e-4));
    // its distance from the shoulder joint: about 28 cm
    CHECK(Near(std::sqrt(kHelperT[0] * kHelperT[0] + kHelperT[1] * kHelperT[1] + kHelperT[2] * kHelperT[2]), 0.2766, 0.001));
}

void TestAngleAndJoint() {
    const float id[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const float z90[3][3] = {{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}};
    CHECK(Near(AngleDeg(id, id), 0.0, 0.05));
    CHECK(Near(AngleDeg(id, z90), 90.0, 0.05));
    float nan3[3][3] = {{std::numeric_limits<float>::quiet_NaN(), 0, 0}, {0, 1, 0}, {0, 0, 1}};
    CHECK(AngleDeg(id, nan3) == 180.0);
    float a[4][4] = {{2, 0, 0, 0}, {0, 2, 0, 0}, {0, 0, 2, 0}, {0.1f, 0.2f, 0.3f, 1}};  // scaled axes: normalised first
    float b[4][4] = {{0, 1, 0, 0}, {-1, 0, 0, 0}, {0, 0, 1, 0}, {0.1f, 0.2f, 0.33f, 1}};
    double cm = -1, deg = -1;
    CHECK(JointDelta(a, b, &cm, &deg) && Near(cm, 3.0, 0.01) && Near(deg, 90.0, 0.05));
    CHECK(JointDelta(a, a, &cm, &deg) && Near(cm, 0.0) && Near(deg, 0.0, 0.05));
    float z[4][4] = {};
    CHECK(!JointDelta(a, z, &cm, &deg));
    b[3][0] = std::numeric_limits<float>::infinity();
    CHECK(!JointDelta(a, b, &cm, &deg));
    static_assert(kArmJoints == 14 && kBodyJoints == 44, "the props' skeletons (static-Q5Q6)");
}
}  // namespace

int main() {
    TestPart();
    TestBodyUp();
    TestDecide();
    TestHold();
    TestAnchor();
    TestHelper();
    TestAngleAndJoint();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
