// The carried passenger's seated idle pose, the pure part (native/mod/ride_pose_math.hpp): quaternions and joint rows in
// both conventions, the frame alignment, the two-bone arm IK (the hand lands on the target, the bone lengths and the
// elbow's hinge axis are kept, the elbow goes to the pole's side, the reach clamp, a straight arm, the forearm twist). No
// game.
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "ride_pose_math.hpp"

using namespace st::ridepose;

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

bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }
bool NearV(V a, V b, double eps = 1e-6) { return Len(a - b) <= eps; }

// a fixed pseudo-random sequence (no <random>: the same numbers on every compiler)
unsigned g_seed = 12345;
double Rnd() {
    g_seed = g_seed * 1103515245u + 12345u;
    return static_cast<double>((g_seed >> 8) & 0xFFFF) / 65535.0 * 2.0 - 1.0;
}
void RndQuat(double (&q)[4]) {
    for (double& v : q) v = Rnd();
    const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (double& v : q) v /= n;
}
M3 RndRows() {
    double q[4];
    RndQuat(q);
    return RotFromQuat(q);  // orthonormal rows
}
bool SameQuat(const double (&a)[4], const double (&b)[4], double eps = 1e-9) {
    const double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    return Near(std::fabs(d), 1.0, eps);
}
// the components of v along the rows (a vector in the frame's own coordinates)
V InFrame(const M3& rows, V v) { return Apply(rows, v); }
V FromFrame(const M3& rows, V c) { return Row(rows, 0) * c.x + Row(rows, 1) * c.y + Row(rows, 2) * c.z; }

void TestQuat() {
    for (int i = 0; i < 200; ++i) {
        double q[4], back[4];
        RndQuat(q);
        QuatFromRot(RotFromQuat(q), back);
        CHECK(SameQuat(q, back));
        const M3 r = RotFromQuat(q);
        CHECK(MaxDiff(Mul(r, T(r)), M3{}) < 1e-12);
    }
    // the four branches of Shepperd's method: rotations of ~180 deg about each axis
    for (int k = 0; k < 3; ++k) {
        V axis;
        (k == 0 ? axis.x : k == 1 ? axis.y : axis.z) = 1;
        double q[4], back[4];
        QuatFromRot(AxisAngle(axis, 3.1), q);
        QuatFromRot(RotFromQuat(q), back);
        CHECK(SameQuat(q, back));
        const V r = Apply(RotFromQuat(q), V{0.3, 0.4, 0.5});
        const V want = Apply(AxisAngle(axis, 3.1), V{0.3, 0.4, 0.5});
        CHECK(NearV(r, want, 1e-9));
    }
    // AxisAngle: 90 deg about z turns x onto y
    CHECK(NearV(Apply(AxisAngle({0, 0, 1}, 1.5707963267948966), {1, 0, 0}), {0, 1, 0}, 1e-12));
}

void TestConventions() {
    for (int conv = 0; conv < 2; ++conv) {
        for (int i = 0; i < 50; ++i) {
            double q[4], back[4];
            RndQuat(q);
            const M3 parent = RndRows();
            const M3 child = Mul(LocalRows(q, conv), parent);
            QuatFromLocalRows(Mul(child, T(parent)), conv, back);
            CHECK(SameQuat(q, back));
        }
    }
    // the two conventions differ (a gate can tell them apart)
    double q[4] = {0.2, 0.3, 0.1, 0.9};
    CHECK(MaxDiff(LocalRows(q, 0), LocalRows(q, 1)) > 0.1);
}

void TestAlign() {
    M3 a;
    CHECK(Align({1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 0}, &a));
    CHECK(NearV(Apply(a, {1, 0, 0}), {0, 0, 1}, 1e-12));
    CHECK(NearV(Apply(a, {0, 1, 0}), {1, 0, 0}, 1e-12));
    CHECK(MaxDiff(Mul(a, T(a)), M3{}) < 1e-12);
    // a normal not perpendicular to u counts by its perpendicular part
    CHECK(Align({2, 0, 0}, {1, 1, 0}, {0, 3, 0}, {0, 1, 1}, &a));
    CHECK(NearV(Apply(a, {1, 0, 0}), {0, 1, 0}, 1e-12));
    CHECK(NearV(Apply(a, {0, 1, 0}), {0, 0, 1}, 1e-12));
    // degenerate: n along u
    CHECK(!Align({1, 0, 0}, {2, 0, 0}, {0, 1, 0}, {0, 0, 1}, &a));
    CHECK(!Align({0, 0, 0}, {0, 1, 0}, {0, 1, 0}, {0, 0, 1}, &a));
}

// A driver's right arm (model space: x right, y forward, z up): reaching forward to a wheel, the elbow a little bent.
struct Chain {
    ArmIn in;
    double l1 = 0, l2 = 0;
    V t_elbow, t_wrist;  // the bone offsets in the parent joint's frame (what the game's translations are)
};
Chain DriverArm(int conv, double side) {
    Chain c;
    c.in.conv = conv;
    c.in.parent = RndRows();
    c.in.upper = RndRows();
    c.in.fore = RndRows();
    c.in.s = {0.20 * side, 0.02, 1.42};
    c.in.e = c.in.s + Norm(V{0.05 * side, 0.20, -0.22}) * 0.29;
    c.in.w = c.in.e + Norm(V{-0.06 * side, 0.26, 0.05}) * 0.26;
    c.l1 = Len(c.in.e - c.in.s), c.l2 = Len(c.in.w - c.in.e);
    c.t_elbow = InFrame(c.in.upper, c.in.e - c.in.s);
    c.t_wrist = InFrame(c.in.fore, c.in.w - c.in.e);
    c.in.target = {0.11 * side, 0.30, 0.98};  // on the thigh
    c.in.pole = PoleDir({side, 0, 0}, {0, 1, 0}, {0, 0, 1}, HandRest{});
    c.in.fallback_n = {side, 0, 0};
    return c;
}

void TestSolve() {
    for (int conv = 0; conv < 2; ++conv) {
        for (double side : {1.0, -1.0}) {
            for (int rep = 0; rep < 20; ++rep) {
                Chain c = DriverArm(conv, side);
                ArmOut o;
                CHECK(SolveArm(c.in, &o));
                CHECK(!o.clamped);
                // the hand lands on the target (1 mm), the bones keep their lengths
                CHECK(Len(o.wrist - c.in.target) < 1e-3);
                CHECK(Near(Len(o.elbow - c.in.s), c.l1, 1e-9));
                CHECK(Near(Len(o.wrist - o.elbow), c.l2, 1e-9));
                // forward kinematics with the new rows and the old bone offsets reach the same elbow and wrist
                CHECK(NearV(c.in.s + FromFrame(o.upper, c.t_elbow), o.elbow, 1e-9));
                CHECK(NearV(o.elbow + FromFrame(o.fore, c.t_wrist), o.wrist, 1e-9));
                // ... and the quaternions give those rows
                CHECK(MaxDiff(Mul(LocalRows(o.q_upper, conv), c.in.parent), o.upper) < 1e-9);
                CHECK(MaxDiff(Mul(LocalRows(o.q_fore, conv), o.upper), o.fore) < 1e-9);
                // the elbow goes to the pole's side of the shoulder-hand line
                const V u = Norm(c.in.target - c.in.s);
                const V mid = c.in.s + u * Dot(o.elbow - c.in.s, u);
                const V pole_perp = c.in.pole - u * Dot(c.in.pole, u);
                CHECK(Dot(o.elbow - mid, pole_perp) > 0);
                // the elbow's hinge axis is the same axis of the upper arm before and after
                const V n_old = Norm(Cross(c.in.e - c.in.s, c.in.w - c.in.e));
                const V n_new = Norm(Cross(o.elbow - c.in.s, o.wrist - o.elbow));
                CHECK(NearV(InFrame(c.in.upper, n_old), InFrame(o.upper, n_new), 1e-9));
                CHECK(NearV(InFrame(c.in.fore, n_old), InFrame(o.fore, n_new), 1e-9));
                // the rows stay orthonormal
                CHECK(MaxDiff(Mul(o.upper, T(o.upper)), M3{}) < 1e-9);
                CHECK(MaxDiff(Mul(o.fore, T(o.fore)), M3{}) < 1e-9);
            }
        }
    }
}

void TestClampAndStraight() {
    Chain c = DriverArm(0, 1.0);
    // out of reach: the hand stops at 98 % of the arm's length on the line to the target
    c.in.target = c.in.s + V{0, 3, -1};
    ArmOut o;
    CHECK(SolveArm(c.in, &o));
    CHECK(o.clamped);
    CHECK(Near(Len(o.wrist - c.in.s), 0.98 * (c.l1 + c.l2), 1e-6));
    CHECK(Len(Cross(Norm(o.wrist - c.in.s), Norm(V{0, 3, -1}))) < 1e-6);
    // too close: pushed out to just over |l1 - l2|
    c.in.target = c.in.s + V{0, 0.005, 0};
    CHECK(SolveArm(c.in, &o));
    CHECK(o.clamped);
    CHECK(Len(o.wrist - c.in.s) > std::fabs(c.l1 - c.l2));
    // an arm that is straight now: the fallback normal; still solves onto the target
    Chain s = DriverArm(1, 1.0);
    s.in.w = s.in.e + Norm(s.in.e - s.in.s) * 0.26;
    s.l2 = 0.26;
    s.t_wrist = InFrame(s.in.fore, s.in.w - s.in.e);
    CHECK(SolveArm(s.in, &o));
    CHECK(Len(o.wrist - s.in.target) < 1e-3);
    CHECK(NearV(o.elbow + FromFrame(o.fore, s.t_wrist), o.wrist, 1e-9));
    // a pole along the shoulder-hand line: the fallback normal gives the side
    Chain p = DriverArm(0, 1.0);
    p.in.pole = p.in.target - p.in.s;
    CHECK(SolveArm(p.in, &o));
    CHECK(Len(o.wrist - p.in.target) < 1e-3);
    // nonsense input is refused
    Chain b = DriverArm(0, 1.0);
    b.in.e = b.in.s;
    CHECK(!SolveArm(b.in, &o));
    b = DriverArm(0, 1.0);
    b.in.target.x = std::nan("");
    CHECK(!SolveArm(b.in, &o));
}

void TestTwist() {
    Chain c = DriverArm(0, 1.0);
    ArmOut a, b;
    CHECK(SolveArm(c.in, &a));
    c.in.twist = 0.6;
    CHECK(SolveArm(c.in, &b));
    // the same elbow and wrist; the forearm turned 0.6 rad about its own axis
    CHECK(NearV(a.elbow, b.elbow, 1e-9));
    CHECK(NearV(a.wrist, b.wrist, 1e-9));
    const V axis = Norm(a.wrist - a.elbow);
    const M3 rel = Mul(T(b.fore), a.fore);  // column form: b's axes from a's
    (void)rel;
    double ang = 0;
    for (int k = 0; k < 3; ++k) {
        const V ra = Row(a.fore, k), rb = Row(b.fore, k);
        const V pa = ra - axis * Dot(ra, axis), pb = rb - axis * Dot(rb, axis);
        if (Len(pa) > 0.3 && Len(pb) > 0.3) ang = std::fmax(ang, std::acos(std::fmin(1.0, Dot(Norm(pa), Norm(pb)))));
        CHECK(Near(Dot(ra, axis), Dot(rb, axis), 1e-9));  // the part along the axis is unchanged
    }
    CHECK(Near(ang, 0.6, 1e-6));
}

void TestRest() {
    const HandRest r;
    const V t = RestTarget({0.1, 0, 1.0}, {0.1, 0.45, 1.0}, {0, 0, 1}, {-1, 0, 0}, r);
    CHECK(NearV(t, {0.1 - 0.03, 0.27, 1.09}, 1e-12));
    const V p = PoleDir({1, 0, 0}, {0, 1, 0}, {0, 0, 1}, r);
    CHECK(Near(Len(p), 1.0, 1e-12));
    CHECK(p.x > 0 && p.y < 0 && p.z < 0);
}
}  // namespace

int main() {
    TestQuat();
    TestConventions();
    TestAlign();
    TestSolve();
    TestClampAndStraight();
    TestTwist();
    TestRest();
    std::printf("ride_pose_tests: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
