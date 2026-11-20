// Self view follow mode 4 (docs/real-partner-body.md section 26), the pure part: whether the self body's own update puts it
// where Sam will be after his update of this frame (his world now plus his last per-frame step, scaled to this frame), or
// copies his world as is, and why. No game memory, no Windows: unit-tested in tests/self_predict_tests.cpp. The game side
// is real_body.cpp (TrackSam, PredictSelfWorld).
//
// v0.19 skipped the step whenever Sam had a parent entity (driving in the game): "the vehicle moves him outside his update".
// Measured in the v0.19 play test 2 research (section 26.6): it does not. Sam's world at the body's update equals his newest
// record (0.00 cm in every seated second), so in a vehicle the body showed his world of the previous frame, 20-25 cm mean
// and up to 40 cm behind his gear at 11 m/s and 30 fps. Seated, the step predicts him to 0.5-1.6 cm. What a vehicle does
// add is one huge step when he snaps into or out of the seat (85-137 cm in one frame); the consistency gate below keeps the
// prediction from repeating it.
#pragma once
#include <cmath>
#include <cstdint>

namespace st::selfpred {

constexpr double kMovedM = 0.005;      // Sam's world now against his newest record: something outside his update moved him
constexpr double kJumpM = 3.0;         // a step over 3 m in one frame: a teleport, a load
constexpr double kGateFloorM = 0.25;   // the consistency gate: his newest step may differ from the one before by this much
constexpr double kGateRatio = 0.75;    // ... or by this share of the one before, whichever is larger
constexpr double kScaleMin = 0.5;      // the step's dt scale, clamped
constexpr double kScaleMax = 2.0;

// Why the body got the world it got.
enum class Why : std::uint8_t {
    step,        // his world now plus his last per-frame step (scaled)
    after,       // Sam already updated in this frame: his world now is this frame's
    ride,        // carried as a passenger (the carry teleports him from the game tick; v0.19: also while seated)
    no_history,  // no previous frame yet
    stale,       // his records are over 2.5 frames old (not updated: asleep, a cutscene)
    moved,       // his world now is more than 5 mm from his newest record: that move is already in it
    jump,        // a step over 3 m
    gate,        // his newest step differs from the one before by more than the gate (a seat snap, a teleport-like move)
};
inline const char* WhyName(Why w) {
    switch (w) {
        case Why::step: return "step";
        case Why::after: return "after Sam";
        case Why::ride: return "ride";
        case Why::no_history: return "no history";
        case Why::stale: return "stale";
        case Why::moved: return "moved";
        case Why::jump: return "jump";
        case Why::gate: return "gate";
    }
    return "?";
}

struct In {
    bool carried = false;      // ride::Carried(): the carry moves Sam from the game tick
    bool seated = false;       // Sam has a parent entity (+0x70): he drives / sits in a vehicle in the game
    int records = 0;           // Sam's frame records kept: 1 newest only, 2 + the frame before, 3 + the one before that
    double frame_ms = 0;       // the tick's frame time (EMA)
    double age_ms = 0;         // now - his newest record
    double span_ms = 0;        // his newest record - the frame before's
    double span2_ms = 0;       // the frame before's - the one before that (records >= 3)
    double moved_m = 0;        // his world now against his newest record
    double dp[3] = {};         // his newest per-frame step (newest - the frame before)
    double dp_prev[3] = {};    // the step before it (records >= 3)
    double dt_body = 0;        // the game's dt of this body update, s (0: unknown)
    double dt_sam = 0;         // the sum of the game's dts of Sam's updates in his newest frame, s (0: unknown)
};

struct Out {
    Why why = Why::no_history;
    double k = 1.0;  // the step's scale (Why::step only)
};

inline double Len(const double (&v)[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

// This frame against his last: the step is his velocity over his last frame times this frame's time. 1 when either dt is
// unknown; clamped to 0.5-2. At an even pace (the 30 fps cap of two games) it is 1.00.
inline double StepScale(double dt_body, double dt_sam) {
    if (!(dt_body > 0) || !(dt_sam > 0) || !std::isfinite(dt_body) || !std::isfinite(dt_sam)) return 1.0;
    const double k = dt_body / dt_sam;
    return k < kScaleMin ? kScaleMin : k > kScaleMax ? kScaleMax : k;
}

// His newest step against the one before: a vehicle accelerating at 5 m/s^2 changes its step by about 0.6 cm a frame at
// 30 fps, running starts and turns by a few cm; a seat snap by 85 cm and more.
inline bool StepConsistent(const double (&dp)[3], const double (&dp_prev)[3]) {
    const double d[3] = {dp[0] - dp_prev[0], dp[1] - dp_prev[1], dp[2] - dp_prev[2]};
    const double rel = kGateRatio * Len(dp_prev);
    const double lim = rel > kGateFloorM ? rel : kGateFloorM;
    const double dd = Len(d);
    return std::isfinite(dd) && dd <= lim;
}

// The decision. `v019`: v0.19's rule (no step while seated, no gate, no scale), for the meter's comparison and `self_view 13 0`.
inline Out Decide(const In& in, bool v019 = false) {
    Out o;
    if (in.carried || (v019 && in.seated)) return o.why = Why::ride, o;
    if (in.records < 2 || !(in.frame_ms > 0)) return o.why = Why::no_history, o;
    if (in.age_ms < 0.5 * in.frame_ms) return o.why = Why::after, o;
    if (in.age_ms > 2.5 * in.frame_ms || in.span_ms > 2.5 * in.frame_ms) return o.why = Why::stale, o;
    if (!std::isfinite(in.moved_m) || in.moved_m > kMovedM) return o.why = Why::moved, o;
    const double len = Len(in.dp);
    if (!std::isfinite(len) || len > kJumpM) return o.why = Why::jump, o;
    if (!v019 && in.records >= 3 && in.span2_ms <= 2.5 * in.frame_ms && !StepConsistent(in.dp, in.dp_prev)) return o.why = Why::gate, o;
    o.why = Why::step;
    o.k = v019 ? 1.0 : StepScale(in.dt_body, in.dt_sam);
    return o;
}

// Rotations as rows (r_cur = r_prev M): M = r_prev^T r_cur.
inline void RotStep(const float (&prev)[3][3], const float (&cur)[3][3], float (&m)[3][3]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[i][j] = prev[0][i] * cur[0][j] + prev[1][i] * cur[1][j] + prev[2][i] * cur[2][j];
}
inline void RotMul(const float (&a)[3][3], const float (&b)[3][3], float (&o)[3][3]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) o[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
}
// M^k for a rotation M (axis-angle, the angle times k).
inline void RotPow(const float (&m)[3][3], double k, float (&o)[3][3]) {
    double c = (static_cast<double>(m[0][0]) + m[1][1] + m[2][2] - 1.0) / 2.0;
    c = c > 1 ? 1 : c < -1 ? -1 : c;
    const double th = std::acos(c);
    double ax = static_cast<double>(m[2][1]) - m[1][2], ay = static_cast<double>(m[0][2]) - m[2][0], az = static_cast<double>(m[1][0]) - m[0][1];
    const double s = std::sqrt(ax * ax + ay * ay + az * az);
    if (th < 1e-6 || s < 1e-9) {  // (almost) no rotation: the linear part scaled
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) o[i][j] = static_cast<float>((i == j ? 1.0 : 0.0) + k * (m[i][j] - (i == j ? 1.0 : 0.0)));
        return;
    }
    ax /= s, ay /= s, az /= s;
    const double t = th * k, ct = std::cos(t), st = std::sin(t), v = 1 - ct;
    const double r[3][3] = {{ct + ax * ax * v, ax * ay * v - az * st, ax * az * v + ay * st},
                            {ay * ax * v + az * st, ct + ay * ay * v, ay * az * v - ax * st},
                            {az * ax * v - ay * st, az * ay * v + ax * st, ct + az * az * v}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) o[i][j] = static_cast<float>(r[i][j]);
}
// The angle between two rotations, degrees.
inline double RotAngleDeg(const float (&a)[3][3], const float (&b)[3][3]) {
    float m[3][3];
    RotStep(a, b, m);
    double c = (static_cast<double>(m[0][0]) + m[1][1] + m[2][2] - 1.0) / 2.0;
    c = c > 1 ? 1 : c < -1 ? -1 : c;
    return std::acos(c) * 57.29577951308232;
}

// The predicted world: now + k dp, rotation now M^k. False (and `p` / `r` = now) if the result is not finite.
inline bool Apply(const double (&now_p)[3], const float (&now_r)[3][3], const double (&dp)[3], const float (&m)[3][3], double k, double (&p)[3],
                  float (&r)[3][3]) {
    float mk[3][3];
    if (std::fabs(k - 1.0) < 1e-3) {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) mk[i][j] = m[i][j];
    } else {
        RotPow(m, k, mk);
    }
    RotMul(now_r, mk, r);
    bool ok = true;
    for (int i = 0; i < 3; ++i) {
        p[i] = now_p[i] + k * dp[i];
        ok = ok && std::isfinite(p[i]);
        for (int j = 0; j < 3; ++j) ok = ok && std::isfinite(r[i][j]);
    }
    if (!ok) {
        for (int i = 0; i < 3; ++i) {
            p[i] = now_p[i];
            for (int j = 0; j < 3; ++j) r[i][j] = now_r[i][j];
        }
    }
    return ok;
}

}  // namespace st::selfpred
