// Self view follow mode 4's pure part (native/mod/self_predict.hpp): the step / copy decision and its reasons, the seated
// step (v0.20) against v0.19's rule, the consistency gate on a seat snap, the dt scale, the rotation power; round 12: the far
// hips rule (native/mod/pelvis_scale.hpp). No game.
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "pelvis_scale.hpp"
#include "self_predict.hpp"

using namespace st::selfpred;

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

bool Near(double a, double b, double eps = 1e-5) { return std::fabs(a - b) <= eps; }

// A steady frame at 30 fps: three records, Sam moving `step` m along x per frame.
In Steady(double step, double prev_step) {
    In in;
    in.records = 3;
    in.frame_ms = 33.3;
    in.age_ms = 25;
    in.span_ms = 33.3;
    in.span2_ms = 33.3;
    in.moved_m = 0;
    in.dp[0] = step;
    in.dp_prev[0] = prev_step;
    in.dt_body = 1.0 / 30;
    in.dt_sam = 1.0 / 30;
    return in;
}

void TestReasons() {
    In in = Steady(0.1, 0.1);
    CHECK(Decide(in).why == Why::step && Near(Decide(in).k, 1.0));
    In c = in;
    c.carried = true;
    CHECK(Decide(c).why == Why::ride && Decide(c, true).why == Why::ride);
    In h = in;
    h.records = 1;
    CHECK(Decide(h).why == Why::no_history);
    h = in, h.frame_ms = 0;
    CHECK(Decide(h).why == Why::no_history);
    In a = in;
    a.age_ms = 10;  // Sam already updated in this frame
    CHECK(Decide(a).why == Why::after);
    In s = in;
    s.age_ms = 90;
    CHECK(Decide(s).why == Why::stale);
    s = in, s.span_ms = 100;
    CHECK(Decide(s).why == Why::stale);
    In m = in;
    m.moved_m = 0.006;
    CHECK(Decide(m).why == Why::moved);
    m.moved_m = NAN;
    CHECK(Decide(m).why == Why::moved);
    In j = in;
    j.dp[0] = 3.5, j.dp_prev[0] = 3.5;
    CHECK(Decide(j).why == Why::jump);
}

void TestSeated() {
    // driving at 11 m/s, 30 fps: a 37 cm step every frame. v0.19 copied (the body a frame behind); now the step.
    In in = Steady(0.367, 0.36);
    in.seated = true;
    CHECK(Decide(in).why == Why::step);
    CHECK(Decide(in, true).why == Why::ride);
    // the carry still copies
    in.carried = true;
    CHECK(Decide(in).why == Why::ride);
}

void TestGate() {
    // getting on: Sam walks up (2 cm a frame), then snaps 85 cm into the seat: no step on the next frame
    In on = Steady(0.85, 0.02);
    on.seated = true;
    CHECK(Decide(on).why == Why::gate);
    CHECK(Decide(on, true).why == Why::ride);  // v0.19 copied while seated anyway
    // the frame after: seated and still (the vehicle not moving yet), his step 0 after the 85 cm one
    In after = Steady(0.0, 0.85);
    after.seated = true;
    CHECK(Decide(after).why == Why::gate);
    // getting off at a stop: out of the seat by 1.2 m (no parent any more): no step; v0.19 stepped it
    In off = Steady(1.2, 0.0);
    CHECK(Decide(off).why == Why::gate);
    CHECK(Decide(off, true).why == Why::step);
    // accelerating at 5 m/s^2 from 10 m/s: the step grows by about 0.6 cm a frame: stepped
    In acc = Steady(0.339, 0.333);
    acc.seated = true;
    CHECK(Decide(acc).why == Why::step);
    // on foot: a run start (0 -> 4 cm), a sharp turn at 4 m/s (13 cm, 60 degrees off), a reversal at a jog: stepped
    CHECK(Decide(Steady(0.04, 0.0)).why == Why::step);
    In turn = Steady(0.0, 0.133);
    turn.dp[0] = 0.133 * 0.5, turn.dp[1] = 0.133 * 0.866;
    CHECK(Decide(turn).why == Why::step);
    CHECK(Decide(Steady(-0.08, 0.08)).why == Why::step);
    // 20 m/s on the trike (67 cm a frame): steady, stepped; the gate scales with the step
    In fast = Steady(0.67, 0.66);
    fast.seated = true;
    CHECK(Decide(fast).why == Why::step);
    // the frame before that too old (a hitch): no gate decision on it
    In hitch = Steady(0.85, 0.0);
    hitch.span2_ms = 120;
    CHECK(Decide(hitch).why == Why::step);
    // only two records: no gate yet
    In two = Steady(0.85, 0.0);
    two.records = 2;
    CHECK(Decide(two).why == Why::step);
    // StepConsistent itself
    const double a[3] = {0.30, 0, 0}, b[3] = {0.10, 0, 0}, c[3] = {0.0, 0.0, 0.0};
    CHECK(StepConsistent(a, b));   // 20 cm off: under the floor (25 cm)
    const double d[3] = {0.36, 0, 0};
    CHECK(!StepConsistent(d, b));  // 26 cm off
    CHECK(StepConsistent(c, c));
    const double e[3] = {1.1, 0, 0}, f[3] = {0.6, 0, 0};
    CHECK(!StepConsistent(e, f));  // 50 cm off a 60 cm step: over three quarters of it (45 cm)
    const double g[3] = {0.85, 0, 0};
    CHECK(StepConsistent(g, f));   // 25 cm off a 60 cm step: under three quarters of it
}

void TestScale() {
    CHECK(Near(StepScale(1.0 / 30, 1.0 / 30), 1.0));
    CHECK(Near(StepScale(0.025, 0.0125), 2.0));
    CHECK(Near(StepScale(0.040, 0.010), 2.0));    // clamped
    CHECK(Near(StepScale(0.008, 0.033), 0.5));    // clamped
    CHECK(Near(StepScale(0.020, 0.025), 0.8));
    CHECK(Near(StepScale(0, 0.03), 1.0));
    CHECK(Near(StepScale(0.03, 0), 1.0));
    CHECK(Near(StepScale(NAN, 0.03), 1.0));
    In in = Steady(0.1, 0.1);
    in.dt_body = 0.02, in.dt_sam = 0.025;
    CHECK(Near(Decide(in).k, 0.8) && Near(Decide(in, true).k, 1.0));
}

void Yaw(double deg, float (&r)[3][3]) {
    const double a = deg * 3.141592653589793 / 180.0, c = std::cos(a), s = std::sin(a);
    const float m[3][3] = {{static_cast<float>(c), static_cast<float>(s), 0}, {static_cast<float>(-s), static_cast<float>(c), 0}, {0, 0, 1}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i][j] = m[i][j];
}

void TestRotation() {
    float prev[3][3], cur[3][3], m[3][3], half[3][3], twice[3][3], ten[3][3], id[3][3];
    Yaw(10, prev);
    Yaw(16, cur);
    RotStep(prev, cur, m);
    float m6[3][3];
    Yaw(6, m6);
    CHECK(RotAngleDeg(m, m6) < 0.05);
    RotPow(m, 0.5, half);
    float y3[3][3];
    Yaw(3, y3);
    CHECK(RotAngleDeg(half, y3) < 0.05);
    RotPow(m, 2.0, twice);
    float y12[3][3];
    Yaw(12, y12);
    CHECK(RotAngleDeg(twice, y12) < 0.05);
    Yaw(0, id);
    RotPow(id, 1.7, ten);
    CHECK(RotAngleDeg(ten, id) < 0.05);
    // Apply: now (yaw 16, at x 5) + 0.5 step (6 degrees, 10 cm) = yaw 19 at x 5.05
    const double now_p[3] = {5, 0, 1}, dp[3] = {0.1, 0, 0};
    double p[3];
    float r[3][3];
    CHECK(Apply(now_p, cur, dp, m, 0.5, p, r));
    float y19[3][3];
    Yaw(19, y19);
    CHECK(Near(p[0], 5.05) && Near(p[1], 0) && Near(p[2], 1) && RotAngleDeg(r, y19) < 0.05);
    CHECK(Apply(now_p, cur, dp, m, 1.0, p, r));
    float y22[3][3];
    Yaw(22, y22);
    CHECK(Near(p[0], 5.1) && RotAngleDeg(r, y22) < 0.05);
    const double bad[3] = {NAN, 0, 0};
    CHECK(!Apply(now_p, cur, bad, m, 1.0, p, r));
    CHECK(Near(p[0], 5) && RotAngleDeg(r, cur) < 0.05);
}

void TestNames() {
    for (Why w : {Why::step, Why::after, Why::ride, Why::no_history, Why::stale, Why::moved, Why::jump, Why::gate}) CHECK(WhyName(w)[0] != '?');
}

// The far hips rule (pelvis_scale.hpp): bit-identical to the ratio up to 1.15 m, the constant rest offset from 1.45 m, smooth
// in between; the private room's anchored offsets of 2.0-2.5 m keep her within a centimetre of Sam's hips less the rest offset.
void TestPelvis() {
    using namespace st::pelvis;
    const float r = 0.957f;
    const float her_rest[3] = {0.0f, 0.02f, 0.957f};
    float off[3];
    CHECK(RestOffset(her_rest, r, 1.0f, off));
    const double ol = std::sqrt(static_cast<double>(off[0]) * off[0] + static_cast<double>(off[1]) * off[1] + static_cast<double>(off[2]) * off[2]);
    CHECK(Near(ol, 0.043, 1e-4) && off[2] > 0.0429f && std::fabs(off[0]) < 1e-6f);
    float bad[3];
    CHECK(!RestOffset(her_rest, 1.0f, 1.0f, bad) && bad[0] == 0 && bad[2] == 0);
    const float tiny[3] = {0, 0, 0.1f};
    CHECK(!RestOffset(tiny, r, 1.0f, bad));
    CHECK(!RestOffset(nullptr, r, 1.0f, bad));
    // standing, seated, walking: exactly the ratio
    const float pts[][3] = {{0, 0, 1.0f}, {0.1f, -0.2f, 0.78f}, {0.3f, 0.4f, 0.9f}, {0, 0, 1.15f}};
    for (const auto& t : pts) {
        float a[3], b[3];
        Scaled(t, r, off, a);
        Scaled(t, r, nullptr, b);
        CHECK(a[0] == t[0] * r && a[1] == t[1] * r && a[2] == t[2] * r && a[0] == b[0] && a[1] == b[1] && a[2] == b[2]);
    }
    // the room's anchored hips 2.47 m away (mostly level): Sam's minus the rest offset, not 10.6 cm toward the root
    const float far[3] = {2.3f, -0.6f, 0.7f};
    float a[3], b[3];
    Scaled(far, r, off, a);
    Scaled(far, r, nullptr, b);
    double ea = 0, eb = 0;
    for (int k = 0; k < 3; ++k) {
        const double ideal = far[k] - off[k];
        ea += (a[k] - ideal) * (a[k] - ideal);
        eb += (b[k] - ideal) * (b[k] - ideal);
    }
    CHECK(std::sqrt(ea) < 0.01 && std::sqrt(eb) > 0.09);
    // smooth through the blend: along a line from 1.0 to 2.0 m, no step between samples 1 mm apart over 2 mm
    float last[3];
    bool have = false;
    double worst = 0;
    for (int i = 0; i <= 1000; ++i) {
        const float d = 1.0f + 0.001f * i;
        const float t[3] = {d * 0.6f, 0.0f, d * 0.8f};
        float o[3];
        Scaled(t, r, off, o);
        if (have) {
            const double dd = std::sqrt((o[0] - last[0]) * (o[0] - last[0]) + (o[1] - last[1]) * (o[1] - last[1]) + (o[2] - last[2]) * (o[2] - last[2]));
            worst = std::fmax(worst, dd);
        }
        last[0] = o[0], last[1] = o[1], last[2] = o[2], have = true;
    }
    CHECK(worst < 0.002);
    // not finite in: the ratio's result (no NaN from the blend)
    const float nan3[3] = {NAN, 0, 1};
    float o[3];
    Scaled(nan3, r, off, o);
    CHECK(std::isnan(o[0]) && o[2] == r);
}
}  // namespace

int main() {
    TestReasons();
    TestSeated();
    TestGate();
    TestScale();
    TestRotation();
    TestNames();
    TestPelvis();
    std::printf("self_predict_tests: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
