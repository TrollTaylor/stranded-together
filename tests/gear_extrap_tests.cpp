// Gear props' one-frame extrapolation, the pure part (native/mod/gear_extrap.hpp): the body frame, the history and its key,
// constant velocity and turn, the gates (too few, stale, span, step, turn, not finite). No game.
#include <cmath>
#include <cstdio>

#include "gear_extrap.hpp"

using namespace st::gearx;

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

void Ident(float (&r)[3][3]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i][j] = i == j ? 1.0f : 0.0f;
}
// Yaw about the up axis (z), rows.
void Yaw(double deg, float (&r)[3][3]) {
    const double a = deg / 57.29577951308232, c = std::cos(a), s = std::sin(a);
    Ident(r);
    r[0][0] = static_cast<float>(c), r[0][1] = static_cast<float>(s);
    r[1][0] = static_cast<float>(-s), r[1][1] = static_cast<float>(c);
}
// child (in parent's frame) -> world, as real_body.cpp's Compose
void Compose(const double (&Pp)[3], const float (&Pr)[3][3], const double (&p)[3], const float (&r)[3][3], double (&op)[3], float (&orr)[3][3]) {
    for (int k = 0; k < 3; ++k) {
        op[k] = Pp[k] + p[0] * Pr[0][k] + p[1] * Pr[1][k] + p[2] * Pr[2][k];
        for (int c = 0; c < 3; ++c) orr[k][c] = r[k][0] * Pr[0][c] + r[k][1] * Pr[1][c] + r[k][2] * Pr[2][c];
    }
}

Rec At(double x, double y, double z, double yaw_deg, double t) {
    Rec r;
    r.p[0] = x, r.p[1] = y, r.p[2] = z;
    Yaw(yaw_deg, r.r);
    r.t_ms = t;
    return r;
}

void TestToLocalRoundTrip() {
    double Wp[3] = {100.5, -20.25, 3.0};
    float Wr[3][3];
    Yaw(37, Wr);
    const double lp0[3] = {0.1, 0.25, 1.3};
    float lr0[3][3];
    Yaw(-12, lr0);
    double wp[3];
    float wr[3][3];
    Compose(Wp, Wr, lp0, lr0, wp, wr);
    double lp[3];
    float lr[3][3];
    ToLocal(Wp, Wr, wp, wr, lp, lr);
    for (int a = 0; a < 3; ++a) {
        CHECK(Near(lp[a], lp0[a], 1e-5));
        for (int b = 0; b < 3; ++b) CHECK(Near(lr[a][b], lr0[a][b], 1e-5));
    }
}

void TestHistoryKey() {
    Hist h;
    Push(h, 1, 0x1000, 7, 8, At(0, 0, 0, 0, 0));
    CHECK(h.n == 1);
    Push(h, 1, 0x1000, 7, 8, At(0.01, 0, 0, 0, 33));
    CHECK(h.n == 2);
    Push(h, 1, 0x1000, 7, 8, At(0.02, 0, 0, 0, 66));
    CHECK(h.n == 2 && Near(h.prev.p[0], 0.01) && Near(h.cur.p[0], 0.02));
    CHECK(Matches(h, 1, 0x1000, 7, 8));
    CHECK(!Matches(h, 2, 0x1000, 7, 8));
    // another generation (the list republished), another entity, another GUID: the history starts again
    Push(h, 2, 0x1000, 7, 8, At(0, 0, 0, 0, 99));
    CHECK(h.n == 1);
    Push(h, 2, 0x2000, 7, 8, At(0, 0, 0, 0, 132));
    CHECK(h.n == 1);
    Push(h, 2, 0x2000, 7, 9, At(0, 0, 0, 0, 165));
    CHECK(h.n == 1);
    Push(h, 2, 0x2000, 7, 9, At(0, 0, 0, 0, 198));
    CHECK(h.n == 2);
}

void TestConstantVelocityAndTurn() {
    Hist h;
    Push(h, 1, 0x1000, 1, 2, At(0.10, 0.20, 1.00, 10, 1000));
    Push(h, 1, 0x1000, 1, 2, At(0.12, 0.19, 1.01, 14, 1033.3));
    double p[3] = {};
    float r[3][3] = {};
    CHECK(Extrapolate(h, 1050, 33.3, p, r) == Why::ok);
    CHECK(Near(p[0], 0.14) && Near(p[1], 0.18) && Near(p[2], 1.02));
    float want[3][3];
    Yaw(18, want);
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b) CHECK(Near(r[a][b], want[a][b], 1e-5));
    // standing still: the newest record as is
    Hist s;
    Push(s, 1, 0x1000, 1, 2, At(0.3, 0.1, 1.2, 5, 1000));
    Push(s, 1, 0x1000, 1, 2, At(0.3, 0.1, 1.2, 5, 1033.3));
    CHECK(Extrapolate(s, 1040, 33.3, p, r) == Why::ok);
    CHECK(Near(p[0], 0.3) && Near(p[1], 0.1) && Near(p[2], 1.2));
    Yaw(5, want);
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b) CHECK(Near(r[a][b], want[a][b], 1e-5));
}

void TestGates() {
    double p[3] = {9, 9, 9};
    float r[3][3];
    Ident(r);
    Hist h;
    CHECK(Extrapolate(h, 0, 33.3, p, r) == Why::few);
    Push(h, 1, 0x1000, 1, 2, At(0, 0, 0, 0, 1000));
    CHECK(Extrapolate(h, 1010, 33.3, p, r) == Why::few);
    Push(h, 1, 0x1000, 1, 2, At(0.01, 0, 0, 0, 1033.3));
    CHECK(Extrapolate(h, 1050, 0, p, r) == Why::few);                 // no frame time
    CHECK(Extrapolate(h, 1033.3 + 84, 33.3, p, r) == Why::stale);     // the body not updated for 2.5 frames
    CHECK(Extrapolate(h, 1033.3 + 80, 33.3, p, r) == Why::ok);
    // the gates leave p / r untouched
    double q[3] = {9, 9, 9};
    float rr[3][3];
    Ident(rr);
    CHECK(Extrapolate(h, 2000, 33.3, q, rr) == Why::stale && q[0] == 9 && q[1] == 9 && q[2] == 9 && rr[0][0] == 1.0f);
    // two records from one frame (a forced update): span under half a frame
    Hist f;
    Push(f, 1, 0x1000, 1, 2, At(0, 0, 0, 0, 1000));
    Push(f, 1, 0x1000, 1, 2, At(0.05, 0, 0, 0, 1005));
    CHECK(Extrapolate(f, 1010, 33.3, p, r) == Why::span);
    // a gap of 3 frames between the records
    Hist g;
    Push(g, 1, 0x1000, 1, 2, At(0, 0, 0, 0, 1000));
    Push(g, 1, 0x1000, 1, 2, At(0.05, 0, 0, 0, 1100));
    CHECK(Extrapolate(g, 1110, 33.3, p, r) == Why::span);
    // a seat snap: 30 cm in one frame
    Hist s;
    Push(s, 1, 0x1000, 1, 2, At(0, 0, 0, 0, 1000));
    Push(s, 1, 0x1000, 1, 2, At(0.30, 0, 0, 0, 1033.3));
    CHECK(Extrapolate(s, 1040, 33.3, p, r) == Why::step);
    Hist s2;
    Push(s2, 1, 0x1000, 1, 2, At(0, 0, 0, 0, 1000));
    Push(s2, 1, 0x1000, 1, 2, At(0.24, 0, 0, 0, 1033.3));
    CHECK(Extrapolate(s2, 1040, 33.3, p, r) == Why::ok && Near(p[0], 0.48));
    // the body's own world (v0.21): 37 cm a frame at 11 m/s is a step for a prop, not for the body (limit 1.5 m)
    Hist b;
    Push(b, 1, 1, 0, 0, At(10.0, 5.0, 0, 30, 1000));
    Push(b, 1, 1, 0, 0, At(10.37, 5.0, 0, 30, 1033.3));
    CHECK(Extrapolate(b, 1040, 33.3, p, r) == Why::step);
    CHECK(Extrapolate(b, 1040, 33.3, p, r, kMaxBodyStepM) == Why::ok && Near(p[0], 10.74) && Near(p[1], 5.0));
    Hist b2;
    Push(b2, 1, 1, 0, 0, At(0, 0, 0, 0, 1000));
    Push(b2, 1, 1, 0, 0, At(1.6, 0, 0, 0, 1033.3));  // a teleport of the body: plain
    CHECK(Extrapolate(b2, 1040, 33.3, p, r, kMaxBodyStepM) == Why::step);
    // a turn of 40 degrees in one frame
    Hist t;
    Push(t, 1, 0x1000, 1, 2, At(0, 0, 0, 0, 1000));
    Push(t, 1, 0x1000, 1, 2, At(0, 0, 0, 40, 1033.3));
    CHECK(Extrapolate(t, 1040, 33.3, p, r) == Why::turn);
    // not finite
    Hist n;
    Push(n, 1, 0x1000, 1, 2, At(0, 0, 0, 0, 1000));
    Rec bad = At(0, 0, 0, 0, 1033.3);
    bad.p[1] = std::nan("");
    Push(n, 1, 0x1000, 1, 2, bad);
    CHECK(Extrapolate(n, 1040, 33.3, p, r) == Why::nonfinite);
    Hist n2;
    Push(n2, 1, 0x1000, 1, 2, At(0, 0, 0, 0, 1000));
    Rec bad2 = At(0, 0, 0, 0, 1033.3);
    bad2.r[0][0] = std::nanf("");
    Push(n2, 1, 0x1000, 1, 2, bad2);
    CHECK(Extrapolate(n2, 1040, 33.3, p, r) == Why::nonfinite);
    CHECK(WhyName(Why::turn)[0] == 't');
}

// The whole round: a body moving and turning, a prop riding a joint that swings; the body-frame extrapolation from two
// updates lands on the third update's placement to within the swing's second difference.
void TestBodyRoundTrip() {
    Hist h;
    double expect_p[3] = {};
    for (int k = 0; k < 3; ++k) {
        const double t = 1000 + 33.3 * k;
        double Wp[3] = {10.0 + 0.3 * k, 5.0 - 0.1 * k, 2.0};
        float Wr[3][3];
        Yaw(20 + 3 * k, Wr);
        const double lp[3] = {0.05 * k, 0.2, 1.1 + 0.01 * k};
        float lr[3][3];
        Yaw(2.0 * k, lr);
        double wp[3];
        float wr[3][3];
        Compose(Wp, Wr, lp, lr, wp, wr);
        if (k < 2) {
            Rec r;
            ToLocal(Wp, Wr, wp, wr, r.p, r.r);
            r.t_ms = t;
            Push(h, 3, 0x5000, 11, 12, r);
        } else {
            double p[3];
            float rr[3][3];
            CHECK(Extrapolate(h, t - 20, 33.3, p, rr) == Why::ok);
            double xp[3];
            float xr[3][3];
            Compose(Wp, Wr, p, rr, xp, xr);
            for (int a = 0; a < 3; ++a) {
                expect_p[a] = wp[a];
                CHECK(Near(xp[a], expect_p[a], 1e-4));
                for (int b = 0; b < 3; ++b) CHECK(Near(xr[a][b], wr[a][b], 1e-4));
            }
        }
    }
}
}  // namespace

int main() {
    TestToLocalRoundTrip();
    TestHistoryKey();
    TestConstantVelocityAndTurn();
    TestGates();
    TestBodyRoundTrip();
    std::printf("gear_extrap: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
