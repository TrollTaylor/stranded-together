// The carried passenger's seated idle pose (docs/real-partner-body.md section 24.14), the pure part: quaternions and
// frames in the game's joint convention, and the two-bone arm IK that rests the hands on the thighs. No game memory, no
// Windows: unit-tested in tests/ride_pose_tests.cpp. The game side is real_body.cpp (RideOverride, source 3).
//
// Frames are the game's model matrix rows, normalised: row k is the joint's k-th axis in model space. A joint's local
// rotation as rows is L = C * P^T (C the child's rows, P the parent's). Which way a local quaternion maps to those rows is
// measured on Sam's own pose at run time (the convention gate): 0 = the rows are R(q)^T (the column-vector matrix's
// columns are the child's axes in the parent's frame), 1 = the rows are R(q).
#pragma once
#include <cmath>

namespace st::ridepose {

struct V {
    double x = 0, y = 0, z = 0;
};
inline V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V operator*(V a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double Dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V Cross(V a, V b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double Len(V a) { return std::sqrt(Dot(a, a)); }
inline bool Finite(V a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
// a / |a|, or `fb` when a is too short
inline V Norm(V a, V fb = {}) {
    const double n = Len(a);
    return n > 1e-9 && std::isfinite(n) ? a * (1.0 / n) : fb;
}

struct M3 {
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};
inline M3 Mul(const M3& a, const M3& b) {
    M3 o;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) o.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return o;
}
inline M3 T(const M3& a) {
    M3 o;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) o.m[i][j] = a.m[j][i];
    return o;
}
inline V Apply(const M3& a, V v) {
    return {a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z, a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
            a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z};
}
inline V Row(const M3& a, int r) { return {a.m[r][0], a.m[r][1], a.m[r][2]}; }
inline void SetRow(M3& a, int r, V v) { a.m[r][0] = v.x, a.m[r][1] = v.y, a.m[r][2] = v.z; }
inline M3 Cols(V a, V b, V c) {
    M3 o;
    o.m[0][0] = a.x, o.m[1][0] = a.y, o.m[2][0] = a.z;
    o.m[0][1] = b.x, o.m[1][1] = b.y, o.m[2][1] = b.z;
    o.m[0][2] = c.x, o.m[1][2] = c.y, o.m[2][2] = c.z;
    return o;
}
// the largest element difference
inline double MaxDiff(const M3& a, const M3& b) {
    double e = 0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) e = std::fmax(e, std::fabs(a.m[i][j] - b.m[i][j]));
    return e;
}

// q = (x, y, z, w) -> the column-vector rotation matrix (R * v rotates v). q need not be normalised.
inline M3 RotFromQuat(const double (&q)[4]) {
    double x = q[0], y = q[1], z = q[2], w = q[3];
    const double n = std::sqrt(x * x + y * y + z * z + w * w);
    M3 r;
    if (!(n > 1e-12)) return r;
    x /= n, y /= n, z /= n, w /= n;
    r.m[0][0] = 1 - 2 * (y * y + z * z), r.m[0][1] = 2 * (x * y - z * w), r.m[0][2] = 2 * (x * z + y * w);
    r.m[1][0] = 2 * (x * y + z * w), r.m[1][1] = 1 - 2 * (x * x + z * z), r.m[1][2] = 2 * (y * z - x * w);
    r.m[2][0] = 2 * (x * z - y * w), r.m[2][1] = 2 * (y * z + x * w), r.m[2][2] = 1 - 2 * (x * x + y * y);
    return r;
}
// The inverse of RotFromQuat (Shepperd's method), normalised.
inline void QuatFromRot(const M3& r, double (&q)[4]) {
    const double tr = r.m[0][0] + r.m[1][1] + r.m[2][2];
    if (tr > 0) {
        const double s = std::sqrt(tr + 1.0) * 2;
        q[3] = 0.25 * s, q[0] = (r.m[2][1] - r.m[1][2]) / s, q[1] = (r.m[0][2] - r.m[2][0]) / s, q[2] = (r.m[1][0] - r.m[0][1]) / s;
    } else if (r.m[0][0] > r.m[1][1] && r.m[0][0] > r.m[2][2]) {
        const double s = std::sqrt(1.0 + r.m[0][0] - r.m[1][1] - r.m[2][2]) * 2;
        q[3] = (r.m[2][1] - r.m[1][2]) / s, q[0] = 0.25 * s, q[1] = (r.m[0][1] + r.m[1][0]) / s, q[2] = (r.m[0][2] + r.m[2][0]) / s;
    } else if (r.m[1][1] > r.m[2][2]) {
        const double s = std::sqrt(1.0 + r.m[1][1] - r.m[0][0] - r.m[2][2]) * 2;
        q[3] = (r.m[0][2] - r.m[2][0]) / s, q[0] = (r.m[0][1] + r.m[1][0]) / s, q[1] = 0.25 * s, q[2] = (r.m[1][2] + r.m[2][1]) / s;
    } else {
        const double s = std::sqrt(1.0 + r.m[2][2] - r.m[0][0] - r.m[1][1]) * 2;
        q[3] = (r.m[1][0] - r.m[0][1]) / s, q[0] = (r.m[0][2] + r.m[2][0]) / s, q[1] = (r.m[1][2] + r.m[2][1]) / s, q[2] = 0.25 * s;
    }
    const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (double& v : q) v /= n;
}
// A local quaternion as rows (L = C * P^T), and back, in convention `conv` (see the top).
inline M3 LocalRows(const double (&q)[4], int conv) { return conv == 0 ? T(RotFromQuat(q)) : RotFromQuat(q); }
inline void QuatFromLocalRows(const M3& l, int conv, double (&q)[4]) { QuatFromRot(conv == 0 ? T(l) : l, q); }
// The rotation by `angle` (rad) about the unit `axis` (column-vector form).
inline M3 AxisAngle(V axis, double angle) {
    const V a = Norm(axis, {0, 0, 1});
    const double h = 0.5 * angle, s = std::sin(h);
    const double q[4] = {a.x * s, a.y * s, a.z * s, std::cos(h)};
    return RotFromQuat(q);
}

// The rotation A (column-vector form) that turns the frame (u_from, n_from) onto (u_to, n_to): A * u_from points along
// u_to, and n_from's part perpendicular to u_from along n_to's part perpendicular to u_to. False when degenerate.
inline bool Align(V u_from, V n_from, V u_to, V n_to, M3* a) {
    const V uf = Norm(u_from), ut = Norm(u_to);
    if (Len(uf) < 0.5 || Len(ut) < 0.5) return false;
    const V nf = Norm(n_from - uf * Dot(n_from, uf)), nt = Norm(n_to - ut * Dot(n_to, ut));
    if (Len(nf) < 0.5 || Len(nt) < 0.5) return false;
    const M3 ff = Cols(uf, nf, Cross(uf, nf)), ft = Cols(ut, nt, Cross(ut, nt));
    *a = Mul(ft, T(ff));
    return true;
}

// Where the hand goes and where the elbow points (docs 24.14): on top of the thigh, `along` of the way from the hip joint
// to the knee, `up` along the body's up and `in` toward the other thigh.
struct HandRest {
    double along = 0.60, up = 0.09, in = 0.03;
    double pole_out = 0.7, pole_back = 0.5, pole_down = 0.2;
    double twist = 0;  // the forearm's twist about its own axis (rad; mirrored on the left)
};

// One arm: shoulder S (the upper arm joint), elbow, wrist (model positions); the clavicle's, the upper arm's and the
// forearm's rows. The solution: the two joints' new local quaternions (convention `conv`), the elbow and wrist.
struct ArmIn {
    V s, e, w;
    M3 parent, upper, fore;
    V target, pole;  // pole: a direction from the shoulder (the side the elbow goes)
    V fallback_n;    // the bend normal when the arm is (nearly) straight
    double twist = 0;
    int conv = 0;
};
struct ArmOut {
    double q_upper[4] = {0, 0, 0, 1}, q_fore[4] = {0, 0, 0, 1};
    M3 upper, fore;  // the new rows
    V elbow, wrist;
    bool clamped = false;
};
inline bool SolveArm(const ArmIn& in, ArmOut* out) {
    const double l1 = Len(in.e - in.s), l2 = Len(in.w - in.e);
    if (!(l1 > 0.05 && l1 < 1.0 && l2 > 0.05 && l2 < 1.0)) return false;
    if (!Finite(in.s) || !Finite(in.e) || !Finite(in.w) || !Finite(in.target) || !Finite(in.pole)) return false;
    V tgt = in.target;
    V d = tgt - in.s;
    double dl = Len(d);
    const V u = Norm(d, Norm(in.w - in.s));
    const double reach = 0.98 * (l1 + l2), least = std::fabs(l1 - l2) + 0.02;
    out->clamped = false;
    if (dl > reach) dl = reach, out->clamped = true;
    if (dl < least) dl = least, out->clamped = true;
    tgt = in.s + u * dl;
    const double a = (l1 * l1 - l2 * l2 + dl * dl) / (2 * dl);
    const double h = std::sqrt(std::fmax(0.0, l1 * l1 - a * a));
    V side = in.pole - u * Dot(in.pole, u);
    if (Len(side) < 1e-6) side = in.fallback_n - u * Dot(in.fallback_n, u);
    side = Norm(side);
    if (Len(side) < 0.5) return false;
    const V elbow = in.s + u * a + side * h;
    // the bend normals: the old one from the arm as it is, the new one from the solution (straight: the pole's plane)
    V n_old = Cross(in.e - in.s, in.w - in.e);
    if (Len(n_old) < 1e-4 * l1 * l2) n_old = in.fallback_n;
    V n_new = Cross(elbow - in.s, tgt - elbow);
    if (Len(n_new) < 1e-4 * l1 * l2) n_new = Cross(u, side);
    M3 a_up, a_fo;
    if (!Align(in.e - in.s, n_old, elbow - in.s, n_new, &a_up) || !Align(in.w - in.e, n_old, tgt - elbow, n_new, &a_fo)) return false;
    if (in.twist != 0) a_fo = Mul(AxisAngle(tgt - elbow, in.twist), a_fo);
    // rows: each axis r -> A r, so the rows become R * A^T
    out->upper = Mul(in.upper, T(a_up));
    out->fore = Mul(in.fore, T(a_fo));
    QuatFromLocalRows(Mul(out->upper, T(in.parent)), in.conv, out->q_upper);
    QuatFromLocalRows(Mul(out->fore, T(out->upper)), in.conv, out->q_fore);
    out->elbow = elbow;
    out->wrist = elbow + Apply(a_fo, in.w - in.e);
    return Finite(out->elbow) && Finite(out->wrist);
}

// The hand's resting place on a thigh (hip joint `hip`, knee `knee`), the body's up, toward the other thigh.
inline V RestTarget(V hip, V knee, V up, V medial, const HandRest& r) { return hip + (knee - hip) * r.along + up * r.up + medial * r.in; }
// The elbow's side: out, back and a little down.
inline V PoleDir(V lateral, V fwd, V up, const HandRest& r) { return Norm(lateral * r.pole_out - fwd * r.pole_back - up * r.pole_down, lateral); }

}  // namespace st::ridepose
