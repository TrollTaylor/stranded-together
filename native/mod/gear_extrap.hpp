// Gear props: the tick's placement extrapolated one frame (docs/real-partner-body.md section 26.7), the pure part. No game
// memory, no Windows: unit-tested in tests/gear_extrap_tests.cpp. The game side is real_body.cpp (PlaceGear).
//
// A prop draws the placement it has when its own update runs. The body's update places every prop on the pose it has just
// written, and in 90-99 % of frames the props update after it. In the rest (1-10 %, more while driving) a prop's update
// starts on another worker thread while the body is still in its update, before that placement: the prop then draws the
// tick's placement, which has this frame's world (the tick moved the body) but the previous frame's pose (2 cm mean, up
// to 10.6 cm, single frames). So each body-update placement is kept in the body's frame, and the tick places the prop at
// the newest one plus its last step: where the body's update of this frame will put it.
#pragma once
#include <cmath>
#include <cstdint>

#include "self_predict.hpp"

namespace st::gearx {

constexpr double kMaxStepM = 0.25;        // a step over 25 cm in one frame (a seat snap, a layout change): the plain placement
constexpr double kMaxTurnDeg = 30.0;      // ... or a turn over 30 degrees
constexpr double kMaxAgeFrames = 2.5;     // the newest record over 2.5 frames old (the body not updated): plain
constexpr double kMinSpanFrames = 0.5;    // two records less than half a frame apart (a forced update in the same frame): plain
// The self body's own world (v0.21, section 26.7): the tick runs before the self body's update moves it (follow 4), so a
// prop whose update races that update drew the tick's placement one frame of motion behind (30-41 cm at 11 m/s, run
// 20261120-204717). The tick extrapolates the body's world from its last two updates too; a body moves far more per frame
// than a prop in its frame, so its step limit is 1.5 m (45 m/s at 30 fps), the turn limit the same.
constexpr double kMaxBodyStepM = 1.5;

// A prop's transform in the body's frame, at one of the body's own updates (QPC ms).
struct Rec {
    double p[3] = {};
    float r[3][3] = {};  // rows, as the entity world transform
    double t_ms = 0;
};

// The last two records of one prop, keyed by the prop entity, its GUID and the published list's generation (any change
// starts the history again).
struct Hist {
    Rec cur, prev;
    std::uint32_t n = 0;  // records kept: 0, 1 or 2
    std::uint32_t gen = 0;
    std::uintptr_t e = 0;
    std::uint64_t g0 = 0, g1 = 0;
};

enum class Why : std::uint8_t { ok, few, stale, span, step, turn, nonfinite };
inline const char* WhyName(Why w) {
    switch (w) {
        case Why::ok: return "ok";
        case Why::few: return "few";
        case Why::stale: return "stale";
        case Why::span: return "span";
        case Why::step: return "step";
        case Why::turn: return "turn";
        case Why::nonfinite: return "not finite";
    }
    return "?";
}

// World -> the body's frame. Rows: w.p = W.p + l.p W.r, w.r = l.r W.r, so l.p = W.r (w.p - W.p), l.r = w.r W.r^T.
inline void ToLocal(const double (&Wp)[3], const float (&Wr)[3][3], const double (&wp)[3], const float (&wr)[3][3], double (&lp)[3],
                    float (&lr)[3][3]) {
    const double d[3] = {wp[0] - Wp[0], wp[1] - Wp[1], wp[2] - Wp[2]};
    for (int a = 0; a < 3; ++a) {
        lp[a] = d[0] * Wr[a][0] + d[1] * Wr[a][1] + d[2] * Wr[a][2];
        for (int c = 0; c < 3; ++c) lr[a][c] = wr[a][0] * Wr[c][0] + wr[a][1] * Wr[c][1] + wr[a][2] * Wr[c][2];
    }
}

// Adds a record; a different key (prop entity, GUID, generation) drops what was kept.
inline void Push(Hist& h, std::uint32_t gen, std::uintptr_t e, std::uint64_t g0, std::uint64_t g1, const Rec& r) {
    if (h.gen != gen || h.e != e || h.g0 != g0 || h.g1 != g1) {
        h.n = 0;
        h.gen = gen, h.e = e, h.g0 = g0, h.g1 = g1;
    }
    h.prev = h.cur;
    h.cur = r;
    h.n = h.n < 2 ? h.n + 1 : 2;
}

inline bool Matches(const Hist& h, std::uint32_t gen, std::uintptr_t e, std::uint64_t g0, std::uint64_t g1) {
    return h.gen == gen && h.e == e && h.g0 == g0 && h.g1 == g1;
}

// The prop in the body's frame one body update after the newest record: p = 2 cur - prev, r = cur (prev^T cur). `now_ms`:
// the tick's QPC ms; `frame_ms`: the tick's frame time. Anything but Why::ok leaves p / r untouched (the caller places
// the plain way).
// `max_step_m`: kMaxStepM for a prop in the body's frame, kMaxBodyStepM for the body's own world.
inline Why Extrapolate(const Hist& h, double now_ms, double frame_ms, double (&p)[3], float (&r)[3][3], double max_step_m = kMaxStepM) {
    if (h.n < 2 || !(frame_ms > 0)) return Why::few;
    const double age = now_ms - h.cur.t_ms, span = h.cur.t_ms - h.prev.t_ms;
    if (!(age >= -1.0) || age > kMaxAgeFrames * frame_ms) return Why::stale;
    if (!(span >= kMinSpanFrames * frame_ms) || span > kMaxAgeFrames * frame_ms) return Why::span;
    double q[3];
    double d2 = 0;
    for (int a = 0; a < 3; ++a) {
        const double d = h.cur.p[a] - h.prev.p[a];
        q[a] = h.cur.p[a] + d;
        d2 += d * d;
    }
    if (!std::isfinite(d2)) return Why::nonfinite;
    if (std::sqrt(d2) > max_step_m) return Why::step;
    float m[3][3], o[3][3];
    selfpred::RotStep(h.prev.r, h.cur.r, m);
    double c = (static_cast<double>(m[0][0]) + m[1][1] + m[2][2] - 1.0) / 2.0;
    if (!std::isfinite(c)) return Why::nonfinite;
    c = c > 1 ? 1 : c < -1 ? -1 : c;
    if (std::acos(c) * 57.29577951308232 > kMaxTurnDeg) return Why::turn;
    selfpred::RotMul(h.cur.r, m, o);
    for (int a = 0; a < 3; ++a) {
        if (!std::isfinite(q[a])) return Why::nonfinite;
        for (int b = 0; b < 3; ++b)
            if (!std::isfinite(o[a][b])) return Why::nonfinite;
    }
    for (int a = 0; a < 3; ++a) {
        p[a] = q[a];
        for (int b = 0; b < 3; ++b) r[a][b] = o[a][b];
    }
    return Why::ok;
}

}  // namespace st::gearx
