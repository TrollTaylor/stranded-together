// The partner's Odradek animates, the pure part (native/mod/odra_pose_rules.hpp, round 29 O2): the two skeletons, the sender's
// gate and schedule, the receiver's routing, playback bracket and stale rule, the arm's forward kinematics in the game's joint
// convention, the body on the arm's tip and the sender's FK check. No game.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "odra_pose_rules.hpp"

using namespace st::odrapose;
namespace rp = st::ridepose;
namespace go = st::gearodra;

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

bool Near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }
bool NearV(rp::V a, double x, double y, double z, double eps = 1e-4) { return Near(a.x, x, eps) && Near(a.y, y, eps) && Near(a.z, z, eps); }

void QuatAxis(double ax, double ay, double az, double deg, float (&q)[4]) {
    const double n = std::sqrt(ax * ax + ay * ay + az * az), h = deg * 3.14159265358979 / 360.0;
    q[0] = static_cast<float>(ax / n * std::sin(h)), q[1] = static_cast<float>(ay / n * std::sin(h)), q[2] = static_cast<float>(az / n * std::sin(h));
    q[3] = static_cast<float>(std::cos(h));
}

void TestSkeletons() {
    CHECK(SkeletonOk(go::kPartArm, kArmLayout, 14));
    CHECK(SkeletonOk(go::kPartBody, kBodyLayout, 44));
    CHECK(!SkeletonOk(go::kPartArm, kArmLayout, 13));
    CHECK(!SkeletonOk(go::kPartArm, kBodyLayout, 44));
    CHECK(!SkeletonOk(go::kPartBody, kArmLayout, 14));
    CHECK(!SkeletonOk(5, kArmLayout, 14));
    CHECK(PartOfSkeleton(kArmLayout, 14) == go::kPartArm);
    CHECK(PartOfSkeleton(kBodyLayout, 44) == go::kPartBody);
    CHECK(PartOfSkeleton(kBodyLayout, 14) == -1);
    CHECK(PartOfSkeleton(0x12345678u, 44) == -1);
    static_assert(kArmJoints == go::kArmJoints && kBodyJoints == go::kBodyJoints, "the same skeletons as O1 (static-Q5Q6)");
    static_assert(kArmTip == kArmJoints - 1, "HLP_Oderadek is on the chain's last joint");
}

void TestSendGate() {
    SendIn in;
    in.gear_on = in.pose_on = in.parts_sent = in.partner_cap = in.item = true;
    CHECK(ShouldSendOdraPose(in) == SendWhy::send);
    SendIn a = in;
    a.item = false;
    CHECK(ShouldSendOdraPose(a) == SendWhy::no_item);
    a.partner_cap = false;  // the partner's frames lack kGearOdraPoseCap (v0.40 or odradek_pose=0): never sent
    CHECK(ShouldSendOdraPose(a) == SendWhy::no_cap);
    a.parts_sent = false;
    CHECK(ShouldSendOdraPose(a) == SendWhy::not_sent);
    a.pose_on = false;
    CHECK(ShouldSendOdraPose(a) == SendWhy::pose_off);
    a.gear_on = false;
    CHECK(ShouldSendOdraPose(a) == SendWhy::gear_off);
    SendIn b = in;
    b.gear_on = false;  // odradek_pose alone does nothing
    CHECK(ShouldSendOdraPose(b) == SendWhy::gear_off);
    for (unsigned w = 0; w < kSendWhyCount; ++w) CHECK(std::strcmp(SendWhyName(static_cast<SendWhy>(w)), "?") != 0);
    // the schedule: the first at once, a changed pose at most every 66 ms, an unchanged one every second
    CHECK(Due(false, false, 0));
    CHECK(!Due(true, true, 30));
    CHECK(Due(true, true, 66));
    CHECK(!Due(true, false, 999));
    CHECK(Due(true, false, 1000));
    static_assert(kReadMs <= 1000.0 / 15 && kMinSendMs <= 1000.0 / 15 + 0.5, "at least 15 Hz");
}

void TestRoute() {
    RouteIn in;
    in.layout_match = in.item_in_frame = in.kind_odradek = true;
    in.model = go::kPartArm;
    in.skeleton = kArmLayout, in.n = 14;
    CHECK(Route(in) == go::kPartArm);
    in.skeleton = kBodyLayout, in.n = 44;  // the arm item with the body's pose: dropped
    CHECK(Route(in) == -1);
    in.model = go::kPartBody;
    CHECK(Route(in) == go::kPartBody);
    in.model = go::kPartLegacy;  // the kind's old meaning: the body
    CHECK(Route(in) == go::kPartBody);
    in.model = 2;
    CHECK(Route(in) == -1);
    in.model = go::kPartBody;
    in.kind_odradek = false;  // the item is the backpack (or anything else): never routed
    CHECK(Route(in) == -1);
    // ahead of its gear frame (another layout): the skeleton names the part
    RouteIn ahead;
    ahead.skeleton = kArmLayout, ahead.n = 14;
    CHECK(Route(ahead) == go::kPartArm);
    ahead.skeleton = kBodyLayout, ahead.n = 44;
    CHECK(Route(ahead) == go::kPartBody);
    ahead.skeleton = 0x9B68A8ADu, ahead.n = 10;  // the boots' skeleton
    CHECK(Route(ahead) == -1);
    ahead.layout_match = true;  // the layout matches but the item index is beyond the frame
    ahead.skeleton = kArmLayout, ahead.n = 14;
    CHECK(Route(ahead) == go::kPartArm);
}

void TestPlayback() {
    const double k1[] = {0, 66, 132, 198};
    Bracket b = Pick(k1, 4, 100);
    CHECK(b.a == 1 && b.b == 2 && b.c == 3 && Near(b.key_a, 66) && Near(b.key_b, 132) && Near(b.key_c, 198));
    b = Pick(k1, 4, 250);  // past the newest: held there
    CHECK(b.a == 3 && b.b == -1 && b.c == -1);
    b = Pick(k1, 4, -10);  // before the oldest: the oldest
    CHECK(b.a == 0 && b.b == -1);
    b = Pick(k1, 4, 66);
    CHECK(b.a == 1 && b.b == 2);
    b = Pick(k1, 4, 150);
    CHECK(b.a == 2 && b.b == 3 && b.c == -1);
    const double k2[] = {0, 1000};  // after a pause (the keepalive): the segment starts 100 ms before its end
    b = Pick(k2, 2, 950);
    CHECK(b.a == 0 && b.b == 1 && Near(b.key_a, 900) && Near(b.key_b, 1000));
    CHECK(Near(SegU(950, b.key_a, b.key_b), 0.5));
    CHECK(Near(SegU(800, b.key_a, b.key_b), 0.0));
    const double k3[] = {0, 66, 500};  // the frame after b far behind it: not passed
    b = Pick(k3, 3, 30);
    CHECK(b.a == 0 && b.b == 1 && b.c == -1);
    const double k4[] = {10, 10};  // equal keys: no segment
    b = Pick(k4, 2, 20);
    CHECK(b.a == 1 && b.b == -1);
    CHECK(Pick(nullptr, 0, 5).a == -1);
    CHECK(Near(SegU(5, 0, 10), 0.5) && Near(SegU(-5, 0, 10), 0) && Near(SegU(50, 0, 10), 1));
    CHECK(Near(SegU(5, 10, 10), 0) && Near(SegU(15, 10, 10), 1));
    // the blend: the shorter way, normalised (the props' update does the same)
    const float id[4] = {0, 0, 0, 1}, neg[4] = {0, 0, 0, -1};
    float o[4];
    Nlerp(id, neg, 0.5f, o);
    CHECK(Near(o[3], 1) || Near(o[3], -1));
    float z90[4], z45[4];
    QuatAxis(0, 0, 1, 90, z90);
    QuatAxis(0, 0, 1, 45, z45);
    Nlerp(id, z90, 0.5f, o);
    CHECK(Near(o[2], z45[2], 1e-5) && Near(o[3], z45[3], 1e-5));
    Nlerp(id, z90, 0.0f, o);
    CHECK(Near(o[3], 1));
    // the stale rule: no frame for 1.5 s holds the last pose
    CHECK(!Stale(1000, 0));
    CHECK(!Stale(2400, 1000));
    CHECK(Stale(2600, 1000));
    static_assert(kPlayDelayMs >= kMinSendMs, "the props play one send interval behind: a frame to interpolate towards");
}

// The chain the independent way (column vectors): W_j = W_{j-1} R(q_j), p_j = p_{j-1} + W_{j-1} t_j.
void ColumnFk(const float (*q)[4], const float (*t)[3], unsigned tip, rp::M3* w, rp::V* p) {
    rp::M3 W;
    rp::V P;
    for (unsigned j = 0; j <= tip; ++j) {
        const double qd[4] = {q[j][0], q[j][1], q[j][2], q[j][3]};
        P = P + rp::Apply(W, rp::V{t[j][0], t[j][1], t[j][2]});
        W = rp::Mul(W, rp::RotFromQuat(qd));
    }
    *w = W, *p = P;
}

void TestChain() {
    float q[kArmJoints][4], t[kArmJoints][3];
    for (unsigned j = 0; j < kArmJoints; ++j) q[j][0] = q[j][1] = q[j][2] = 0, q[j][3] = 1, t[j][0] = 0, t[j][1] = 0.1f, t[j][2] = 0;
    Frame f;
    CHECK(ChainFrame(q, t, kArmJoints, kArmTip, kConv, &f));
    CHECK(NearV(f.p, 0, 1.4, 0) && NearV(rp::Row(f.r, 0), 1, 0, 0) && NearV(rp::Row(f.r, 2), 0, 0, 1));
    CHECK(ChainFrame(q, t, kArmJoints, 0, kConv, &f) && NearV(f.p, 0, 0.1, 0));
    CHECK(!ChainFrame(q, t, kArmJoints, kArmJoints, kConv, &f));  // tip beyond the chain
    // joint 0 turned 90 deg about z, joint 1 one metre along its x: convention 0 (the rows are R(q)^T, measured) puts the child
    // on model +y, convention 1 on -y
    float q2[2][4], t2[2][3] = {{0, 0, 0}, {1, 0, 0}};
    QuatAxis(0, 0, 1, 90, q2[0]);
    q2[1][0] = q2[1][1] = q2[1][2] = 0, q2[1][3] = 1;
    CHECK(ChainFrame(q2, t2, 2, 1, 0, &f) && NearV(f.p, 0, 1, 0) && NearV(rp::Row(f.r, 0), 0, 1, 0) && NearV(rp::Row(f.r, 1), -1, 0, 0));
    CHECK(ChainFrame(q2, t2, 2, 1, 1, &f) && NearV(f.p, 0, -1, 0));
    // two quarter turns: a half turn
    QuatAxis(0, 0, 1, 90, q2[1]);
    CHECK(ChainFrame(q2, t2, 2, 1, 0, &f) && NearV(rp::Row(f.r, 0), -1, 0, 0) && NearV(rp::Row(f.r, 1), 0, -1, 0));
    // a bent chain against the column-vector FK: convention 0 is it (rows = W^T)
    for (unsigned j = 0; j < kArmJoints; ++j) {
        QuatAxis(0.3 + 0.1 * j, -0.5 + 0.07 * j, 0.8, 7.0 + 3.0 * j, q[j]);
        t[j][0] = 0.01f * static_cast<float>(j % 3), t[j][1] = 0.04f, t[j][2] = -0.005f * static_cast<float>(j % 2);
    }
    rp::M3 W;
    rp::V P;
    ColumnFk(q, t, kArmTip, &W, &P);
    CHECK(ChainFrame(q, t, kArmJoints, kArmTip, 0, &f));
    CHECK(NearV(f.p, P.x, P.y, P.z, 1e-5) && rp::MaxDiff(f.r, rp::T(W)) < 1e-5);
    Frame g;
    CHECK(ChainFrame(q, t, kArmJoints, kArmTip, 1, &g));
    double cm = 0, deg = 0;
    FrameDelta(f, g, &cm, &deg);
    CHECK(cm > 1.0 || deg > 5.0);  // the other convention is told apart on a bent chain
    // bad joints
    q[3][0] = q[3][1] = q[3][2] = q[3][3] = 0;
    CHECK(!ChainFrame(q, t, kArmJoints, kArmTip, 0, &f));
    q[3][3] = 1;
    t[5][1] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!ChainFrame(q, t, kArmJoints, kArmTip, 0, &f));
    CHECK(!ChainFrame(nullptr, t, kArmJoints, kArmTip, 0, &f));
}

void TestBodyInArm() {
    Frame tip;
    tip.p = rp::V{1, 2, 3};
    double p[3];
    float r[3][3];
    CHECK(BodyInArm(tip, p, r));
    CHECK(Near(p[0], 1) && Near(p[1], 2 + kHelperT[1]) && Near(p[2], 3 + kHelperT[2]));
    CHECK(Near(r[0][0], 1) && Near(r[1][1], 1) && Near(r[2][2], 1) && Near(r[0][1], 0));
    // the tip turned 90 deg about z (rows: x -> +y, y -> -x): the helper's offset turns with it; scaled rows are normalised
    rp::SetRow(tip.r, 0, rp::V{0, 1.1, 0});
    rp::SetRow(tip.r, 1, rp::V{-1.1, 0, 0});
    rp::SetRow(tip.r, 2, rp::V{0, 0, 1.1});
    CHECK(BodyInArm(tip, p, r));
    CHECK(Near(p[0], 1 - kHelperT[1]) && Near(p[1], 2) && Near(p[2], 3 + kHelperT[2]));
    CHECK(Near(r[0][1], 1) && Near(r[1][0], -1) && Near(r[2][2], 1));
    rp::SetRow(tip.r, 1, rp::V{0, 0, 0});  // degenerate
    CHECK(!BodyInArm(tip, p, r));
    // the helper (decoded offline): 1.6 cm off joint 13
    CHECK(Near(std::sqrt(kHelperT[1] * kHelperT[1] + kHelperT[2] * kHelperT[2]), 0.01625, 1e-4));
}

void TestDeltaAndModel() {
    float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0.5f, -0.25f, 2, 1}};
    const Frame a = FromModelMatrix(m);
    CHECK(NearV(a.p, 0.5, -0.25, 2) && NearV(rp::Row(a.r, 1), 0, 1, 0));
    double cm = -1, deg = -1;
    FrameDelta(a, a, &cm, &deg);
    CHECK(Near(cm, 0) && Near(deg, 0, 1e-3));
    Frame b = a;
    b.p = b.p + rp::V{0, 0.02, 0};
    float q[1][4], t[1][3] = {{0, 0, 0}};
    QuatAxis(1, 0, 0, 10, q[0]);
    Frame turned;
    CHECK(ChainFrame(q, t, 1, 0, kConv, &turned));
    b.r = turned.r;
    FrameDelta(a, b, &cm, &deg);
    CHECK(Near(cm, 2.0, 1e-3) && Near(deg, 10.0, 0.05));
    Frame z;
    rp::SetRow(z.r, 0, rp::V{0, 0, 0});
    FrameDelta(a, z, &cm, &deg);
    CHECK(Near(deg, 180));
}

void TestFkCheck() {
    FkState s;
    CHECK(!s.ok && !s.checked);
    CHECK(Near(FkStep(s, 5.0, 0.5), kFkRetryMs) && !s.ok && s.fails == 1);  // never passed: off until it does
    CHECK(Near(FkStep(s, 0.4, 0.5), kFkEveryMs) && s.ok && s.fails == 0 && s.passed == 1);
    CHECK(Near(FkStep(s, 4.0, 0.5), kFkRetryMs) && s.ok && s.fails == 1);  // one failure alone keeps the bit (a swing seen a frame late)
    CHECK(Near(FkStep(s, 2.9, 9.9), kFkEveryMs) && s.ok && s.fails == 0);
    FkStep(s, 9.0, 0.5);
    FkStep(s, 9.0, 0.5);
    CHECK(s.ok && !s.off);
    CHECK(Near(FkStep(s, 9.0, 0.5), kFkEveryMs) && !s.ok && s.off);  // three in a row: off for the session
    FkStep(s, 0.1, 0.1);
    CHECK(!s.ok && s.off && s.checks == 8);
    FkState n;
    FkStep(n, std::numeric_limits<double>::quiet_NaN(), 0.0);
    CHECK(!n.ok && n.fails == 1);
    FkStep(n, 0.5, std::numeric_limits<double>::quiet_NaN());
    CHECK(!n.ok && n.fails == 2);
    CHECK(!FkPass(-1, -1));  // not measured
    // the right place with the wrong turn (another attach rotation): HLP_Oderadek's 1.6 cm offset turned 90 deg about x moves
    // the predicted body ~2.3 cm, under kFkOkCm; the turn fails it
    {
        const double h = std::sqrt(0.011729 * 0.011729 + 0.011248 * 0.011248) * 100.0;
        const double moved = h * std::sqrt(2.0);
        CHECK(moved < kFkOkCm);
        CHECK(!FkPass(moved, 90.0));
        FkState w;
        CHECK(Near(FkStep(w, moved, 90.0), kFkRetryMs) && !w.ok && w.fails == 1);
        CHECK(FkPass(moved, 2.0));
    }
    CHECK(Near(kFkOkCm, 3.0) && Near(kFkOkDeg, 10.0));
}
}  // namespace

int main() {
    TestSkeletons();
    TestSendGate();
    TestRoute();
    TestPlayback();
    TestChain();
    TestBodyInArm();
    TestDeltaAndModel();
    TestFkCheck();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
