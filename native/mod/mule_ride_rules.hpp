// Round 30 stage 0 (side riding; docs/real-partner-body.md section 24.19): the pure part of the MULE side-ride probes. Header-only,
// unit-tested (tests/mule_ride_rules_tests.cpp). Nothing here reads game memory.
//
//   HelperKey     the game's key for a skeleton helper name (FUN_14190D040: SSE4.2 crc32 = CRC-32C, reflected 0x82F63B78, init 0,
//                 no final xor, over the name's bytes, & 0x7FFFFFFF); the helper lookup 0x142F6B290 (the truck's vt slot +0x130)
//                 takes a pointer to it (`ride_test 41`).
//   SeatHelper    the MULE seat -> its helper (FUN_142DC8520: 0 HLP_Driver, 1 HLP_Enemy_3, 2 HLP_Enemy, 3 HLP_Enemy_4,
//                 4 HLP_Enemy_2; anything else HLP_Driver as the game's default).
//   HandHelper    the seat's hand helper (FUN_142DC78B0(seat, side)): side != 0: 0 HLP_HandleHandL, 1 HLP_MuleHandRF,
//                 3 HLP_MuleHandRB; side == 0: 0 HLP_HandleHandR, 2 HLP_MuleHandLF, 4 HLP_MuleHandLB; else none.
//   InboardHand   the hand towards the truck on a side seat: the right side (1, 3) the left hand (joint 38, side != 0), the left
//                 side (2, 4) the right hand (joint 9, side == 0).
//   SideAnchor    the side seats' places in the truck's frame (the MULE's entity, truck_v3 8 / kSeatTable): (+-1.299, 0.127 / -0.692,
//                 0.778).
//   RecWatch      the recorder's trigger (`mule_rec 1`): the rising edge of actor +0xE44 bit 2 starts a board (bit 0 newly set or
//                 the MULE in a truck by +0xE20) or an alight (bit 0 already set); a board ends 0.3 s after bit 2 clears, an
//                 alight 0.5 s after bit 0 clears, either at 6 s.
//   ClipCheck     a recorded clip's validation: unit quaternions, joint speed <= 2500 deg/s (fingers left out), entity step <=
//                 0.5 m per frame, frame gaps < 150 ms, the board's end (the alight's start) within 3 cm of the seat's anchor.
//                 Round 30 release A relaxed the speed (1440) and the gap (50 ms): the P2 clips' worst were finger curls and
//                 single long frames at a load.
//
// Round 30 release A (side seats; docs/real-partner-body.md section 24.20):
//   HangLift      the procedural hang's root lift: the lower foot joint of our standing pose at kHangFeetZ above the anchor.
//   Glide         the get-on / get-off timeline: a 0.5 s eased glide onto the anchor blending into the hang; off: a 0.3 s settle
//                 back to standing on the anchor, then a 0.4 s glide out to the exit spot.
//   LoopPick      a seated MULE's frames (one seat) -> the window that loops best (its ends' largest joint angle, fingers left
//                 out), 1.5 - 4 s long, no gap over 400 ms; LoopSample plays it.
//   Lib*          the in-session library's optional cache file (%LOCALAPPDATA%\StrandedTogether\mule_ride_lib.bin, never in the
//                 repo, a release zip or a log bundle): a header, version, the skeleton's layout hash, the game exe's
//                 fingerprint and a CRC-32 over everything.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "truck_v3_rules.hpp"

namespace st::muleride {

inline constexpr unsigned kJoints = 77;          // Sam's / the MULE's skeleton (layout BE39C83F)
inline constexpr unsigned kHandR = 9, kHandL = 38;
inline constexpr unsigned kFootR = 63, kFootL = 68;
inline constexpr unsigned kRecorderSlot = 8;     // realbody's enemy pose slot for the recorder (never the SEP1 slots 0..7)
inline constexpr unsigned kProbeSlot = 7;        // the P1-0 probe's slot (fixed)

// ---------------------------------------------------------------- the helper names and their keys
inline constexpr const char* kHelperNames[] = {
    "HLP_Driver",      "HLP_Enemy",       "HLP_Enemy_2",    "HLP_Enemy_3",    "HLP_Enemy_4",    "HLP_HandleHandL",
    "HLP_HandleHandR", "HLP_MuleHandRF",  "HLP_MuleHandLF", "HLP_MuleHandRB", "HLP_MuleHandLB", "HLP_Passenger",
};
inline constexpr std::size_t kHelperCount = sizeof(kHelperNames) / sizeof(kHelperNames[0]);

// CRC-32C (Castagnoli, reflected 0x82F63B78) as the SSE4.2 crc32 instruction computes it: no pre / post inversion of its own.
inline constexpr std::uint32_t Crc32c(const char* s, std::size_t n, std::uint32_t crc = 0) {
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= static_cast<std::uint8_t>(s[i]);
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
    }
    return crc;
}
inline constexpr std::size_t CLen(const char* s) {
    std::size_t n = 0;
    while (s && s[n]) ++n;
    return n;
}
// The game's key (FUN_14190D040; cached at the name string - 0xC).
inline constexpr std::uint32_t HelperKey(const char* name) { return Crc32c(name, CLen(name)) & 0x7FFFFFFFu; }

inline constexpr bool SideSeat(int seat) { return seat >= 1 && seat <= 4; }
inline constexpr bool RightSide(int seat) { return seat == 1 || seat == 3; }  // +x (the truck's right)

inline constexpr const char* SeatHelper(int seat) {
    switch (seat) {
        case 1: return "HLP_Enemy_3";
        case 2: return "HLP_Enemy";
        case 3: return "HLP_Enemy_4";
        case 4: return "HLP_Enemy_2";
        default: return "HLP_Driver";
    }
}
// nullptr: the game's empty name (no helper for that seat / side).
inline constexpr const char* HandHelper(int seat, bool side) {
    if (side) return seat == 0 ? "HLP_HandleHandL" : seat == 1 ? "HLP_MuleHandRF" : seat == 3 ? "HLP_MuleHandRB" : nullptr;
    return seat == 0 ? "HLP_HandleHandR" : seat == 2 ? "HLP_MuleHandLF" : seat == 4 ? "HLP_MuleHandLB" : nullptr;
}
inline constexpr const char* InboardHelper(int seat) { return SideSeat(seat) ? HandHelper(seat, RightSide(seat)) : nullptr; }
// Sam's joint of the inboard hand (-1: not a side seat).
inline constexpr int InboardHandJoint(int seat) { return !SideSeat(seat) ? -1 : RightSide(seat) ? static_cast<int>(kHandL) : static_cast<int>(kHandR); }

// The side anchors (the MULE's entity in the truck's frame, seated).
inline bool SideAnchor(int seat, double (&p)[3]) {
    if (!SideSeat(seat)) return false;
    p[0] = RightSide(seat) ? 1.299 : -1.299;
    p[1] = seat <= 2 ? 0.127 : -0.692;
    p[2] = 0.778;
    return true;
}

// Fingers (the ghost's error leaves them out): the hands' children, 10..28 and 39..57.
inline constexpr bool IsFinger(unsigned j) { return (j > kHandR && j <= 28) || (j > kHandL && j <= 57); }

// ---------------------------------------------------------------- frames
// A point of an entity's model space (the model matrices' row 3) in the world, and a world point in a vehicle's frame.
inline void PointWorld(const truckv3::Xf& e, const float (&p)[3], double (&w)[3]) {
    for (int i = 0; i < 3; ++i) w[i] = e.pos[i] + p[0] * e.rot[0][i] + p[1] * e.rot[1][i] + p[2] * e.rot[2][i];
}
inline void PointLocal(const truckv3::Xf& v, const double (&w)[3], double (&l)[3]) {
    const double d[3] = {w[0] - v.pos[0], w[1] - v.pos[1], w[2] - v.pos[2]};
    for (int j = 0; j < 3; ++j) l[j] = d[0] * v.rot[j][0] + d[1] * v.rot[j][1] + d[2] * v.rot[j][2];
}
inline double Dist(const double (&a)[3], const double (&b)[3]) {
    const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

// One recorded animate (the MULE's local joints after its own update) and one game-tick sample (its entity in the truck's frame).
struct PoseFrame {
    double ms = 0;  // QPC ms
    float q[kJoints][4] = {};
    float root_t[3] = {}, pelvis_t[3] = {};
};
struct LocalSample {
    double ms = 0;  // QPC ms
    double pos[3] = {};
};

// The sample nearest `t` (sorted by time) within `max_gap` ms, else -1.
inline long PairIndex(const LocalSample* l, std::size_t n, double t, double max_gap) {
    if (!l || !n) return -1;
    std::size_t lo = 0, hi = n;
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (l[mid].ms < t) lo = mid + 1;
        else hi = mid;
    }
    long best = -1;
    double bd = max_gap;
    for (std::size_t k = lo > 0 ? lo - 1 : 0; k <= lo && k < n; ++k) {
        const double d = std::fabs(l[k].ms - t);
        if (d <= bd) bd = d, best = static_cast<long>(k);
    }
    return best;
}

// ---------------------------------------------------------------- the recorder's trigger
enum class ClipKind : std::uint8_t { none, board, alight };
inline const char* ClipKindName(ClipKind k) { return k == ClipKind::board ? "board" : k == ClipKind::alight ? "alight" : "none"; }
enum class RecEvent : std::uint8_t { none, start, stop, capped };

inline constexpr std::uint32_t kBitRiding = 0x1, kBitAction = 0x4;
inline constexpr double kBoardTailMs = 300, kAlightTailMs = 500, kClipCapMs = 6000;

struct RecWatch {
    ClipKind kind = ClipKind::none;
    bool have_prev = false, pending = false;
    std::uint32_t prev = 0;
    double t0 = 0, t_clear = -1;

    // One game tick: the MULE's ride bits (+0xE44) and whether +0xE20 names a truck.
    RecEvent Step(std::uint32_t bits, bool in_truck, double now_ms) {
        RecEvent ev = RecEvent::none;
        if (kind == ClipKind::none) {
            const bool rising = have_prev && (bits & kBitAction) && !(prev & kBitAction);
            if (rising) {
                if (prev & kBitRiding) kind = ClipKind::alight;
                else if ((bits & kBitRiding) || in_truck) kind = ClipKind::board;
                else pending = true;
            } else if (pending) {
                if (!(bits & kBitAction)) pending = false;
                else if ((bits & kBitRiding) || in_truck) kind = ClipKind::board;
            }
            if (kind != ClipKind::none) pending = false, t0 = now_ms, t_clear = -1, ev = RecEvent::start;
        } else {
            const bool cleared = kind == ClipKind::board ? !(bits & kBitAction) : !(bits & kBitRiding);
            if (cleared && t_clear < 0) t_clear = now_ms;
            if (!cleared) t_clear = -1;
            const double tail = kind == ClipKind::board ? kBoardTailMs : kAlightTailMs;
            if (t_clear >= 0 && now_ms - t_clear >= tail) ev = RecEvent::stop;
            else if (now_ms - t0 >= kClipCapMs) ev = RecEvent::capped;
        }
        prev = bits, have_prev = true;
        return ev;
    }
    void Done() { kind = ClipKind::none, t_clear = -1; }
};

// ---------------------------------------------------------------- the clip's validation
inline constexpr double kQuatTol = 0.01;      // | |q| - 1 |
inline constexpr double kMaxDegPerS = 2500.0; // any joint but the fingers
inline constexpr double kMaxStepM = 0.5;      // the entity per frame (truck frame)
inline constexpr double kMaxGapMs = 150.0;    // between frames (strictly less)
inline constexpr double kPairMs = 40.0;       // a frame and its game-tick sample
inline constexpr double kBoardStartM = 2.5;   // the board starts within this of the anchor (earlier frames trimmed)
inline constexpr double kAnchorM = 0.03;      // the board's end / the alight's start
inline constexpr double kAlightAwayM = 0.3;   // the alight ends at least this far from the anchor
inline constexpr unsigned kMinFrames = 10;

struct ClipVerdict {
    bool ok = false;
    const char* why = "no frames";
    unsigned first = 0, frames = 0, paired = 0, bad_quats = 0;
    int spin_joint = -1;
    double dur_s = 0, fps = 0, max_gap_ms = 0, max_dps = 0, max_step_m = 0;
    double start_d = -1, end_d = -1;
    double start[3] = {}, end[3] = {};
};

inline double QuatAngleDeg(const float (&a)[4], const float (&b)[4]) {
    const double na = std::sqrt(static_cast<double>(a[0]) * a[0] + static_cast<double>(a[1]) * a[1] + static_cast<double>(a[2]) * a[2] + static_cast<double>(a[3]) * a[3]);
    const double nb = std::sqrt(static_cast<double>(b[0]) * b[0] + static_cast<double>(b[1]) * b[1] + static_cast<double>(b[2]) * b[2] + static_cast<double>(b[3]) * b[3]);
    if (na < 1e-9 || nb < 1e-9) return 0;
    double d = (static_cast<double>(a[0]) * b[0] + static_cast<double>(a[1]) * b[1] + static_cast<double>(a[2]) * b[2] + static_cast<double>(a[3]) * b[3]) / (na * nb);
    d = std::fabs(d);
    if (d > 1) d = 1;
    return 2.0 * std::acos(d) * 57.29577951308232;
}

inline ClipVerdict ClipCheck(ClipKind kind, const PoseFrame* f, std::size_t n, const LocalSample* l, std::size_t nl, const double (&anchor)[3]) {
    ClipVerdict v;
    if (!f || n == 0) return v;
    // the pairs; a board starts at its first paired frame within kBoardStartM of the anchor
    std::size_t first = n;
    for (std::size_t i = 0; i < n; ++i) {
        const long k = PairIndex(l, nl, f[i].ms, kPairMs);
        if (k < 0) continue;
        if (kind != ClipKind::board || Dist(l[k].pos, anchor) <= kBoardStartM) {
            first = i;
            break;
        }
    }
    if (first == n) {
        v.first = 0, v.frames = static_cast<unsigned>(n);
        v.why = nl ? (kind == ClipKind::board ? "never near the anchor" : "no truck frame") : "no truck frame";
        return v;
    }
    v.first = static_cast<unsigned>(first);
    v.frames = static_cast<unsigned>(n - first);
    v.dur_s = (f[n - 1].ms - f[first].ms) / 1000.0;
    v.fps = v.dur_s > 0 ? (v.frames - 1) / v.dur_s : 0;
    long kprev = -1, kfirst = -1, klast = -1;
    for (std::size_t i = first; i < n; ++i) {
        for (unsigned j = 0; j < kJoints; ++j) {
            const double len = std::sqrt(static_cast<double>(f[i].q[j][0]) * f[i].q[j][0] + static_cast<double>(f[i].q[j][1]) * f[i].q[j][1] +
                                         static_cast<double>(f[i].q[j][2]) * f[i].q[j][2] + static_cast<double>(f[i].q[j][3]) * f[i].q[j][3]);
            if (!(std::fabs(len - 1.0) <= kQuatTol)) ++v.bad_quats;
        }
        if (i > first) {
            const double dt = f[i].ms - f[i - 1].ms;
            if (dt > v.max_gap_ms) v.max_gap_ms = dt;
            if (dt > 0)
                for (unsigned j = 0; j < kJoints; ++j) {
                    if (IsFinger(j)) continue;
                    const double dps = QuatAngleDeg(f[i - 1].q[j], f[i].q[j]) / (dt / 1000.0);
                    if (dps > v.max_dps) v.max_dps = dps, v.spin_joint = static_cast<int>(j);
                }
        }
        const long k = PairIndex(l, nl, f[i].ms, kPairMs);
        if (k < 0) continue;
        ++v.paired;
        if (kfirst < 0) kfirst = k;
        klast = k;
        if (kprev >= 0) {
            const double step = Dist(l[kprev].pos, l[k].pos);
            if (step > v.max_step_m) v.max_step_m = step;
        }
        kprev = k;
    }
    for (int i = 0; i < 3; ++i) v.start[i] = l[kfirst].pos[i], v.end[i] = l[klast].pos[i];
    v.start_d = Dist(l[kfirst].pos, anchor), v.end_d = Dist(l[klast].pos, anchor);
    if (v.frames < kMinFrames) v.why = "too few frames";
    else if (v.bad_quats) v.why = "non-unit quaternions";
    else if (!(v.max_gap_ms < kMaxGapMs)) v.why = "frame gap";
    else if (v.max_dps > kMaxDegPerS) v.why = "joint spin";
    else if (v.max_step_m > kMaxStepM) v.why = "entity jump";
    else if (kind == ClipKind::board && v.end_d > kAnchorM) v.why = "end off the anchor";
    else if (kind == ClipKind::alight && v.start_d > kAnchorM) v.why = "start off the anchor";
    else if (kind == ClipKind::alight && v.end_d < kAlightAwayM) v.why = "never left the seat";
    else v.ok = true, v.why = "";
    return v;
}

// ---------------------------------------------------------------- release A: the procedural hang (our own parameters)
inline constexpr double kHangFeetZ = 0.12;                    // the foot joints above the anchor (the step)
inline constexpr double kHangLiftMin = -0.10, kHangLiftMax = 0.40;
inline constexpr double kHangPelvisOut = 0.10;                // the pelvis this far outward of the anchor (leaning out)
// The root's lift (model z) that puts the lower of the two foot joints of the standing pose at kHangFeetZ; clamped. A
// non-finite foot: 0.
inline double HangLift(double foot_r_z, double foot_l_z) {
    if (!std::isfinite(foot_r_z) || !std::isfinite(foot_l_z)) return 0;
    double lift = kHangFeetZ - (foot_r_z < foot_l_z ? foot_r_z : foot_l_z);
    if (lift < kHangLiftMin) lift = kHangLiftMin;
    if (lift > kHangLiftMax) lift = kHangLiftMax;
    return lift;
}
// The outboard arm's relaxed target (model frame, x the truck's right): out and a little back, at the hip.
inline void OutboardTarget(int side, double (&t)[3]) { t[0] = side * 0.42, t[1] = -0.06, t[2] = 0.98; }

// ---------------------------------------------------------------- release A: get on / get off
inline constexpr double kBoardGlideMs = 500, kAlightSettleMs = 300, kAlightGlideMs = 400;
inline constexpr double kGlideArcM = 0.15;  // the glide's lift at its middle (over the step)
inline double Ease(double t) {
    if (!(t > 0)) return 0;
    if (t >= 1) return 1;
    return t * t * (3 - 2 * t);
}
// From `a` to `b` at eased t, lifted by an arc.
inline void GlidePos(const double (&a)[3], const double (&b)[3], double t, double (&out)[3]) {
    const double e = Ease(t);
    for (int i = 0; i < 3; ++i) out[i] = a[i] + (b[i] - a[i]) * e;
    const double u = t < 0 ? 0 : t > 1 ? 1 : t;
    out[2] += kGlideArcM * 4 * u * (1 - u);
}
enum class GlidePhase : std::uint8_t { none, on, seated, settle, off, done };
inline const char* GlidePhaseName(GlidePhase p) {
    switch (p) {
        case GlidePhase::on: return "getting on";
        case GlidePhase::seated: return "on the seat";
        case GlidePhase::settle: return "settling";
        case GlidePhase::off: return "getting off";
        case GlidePhase::done: return "off";
        default: return "none";
    }
}
// pose_w: 0 our standing pose .. 1 the hang; place_t: 0 at the glide's start .. 1 at its end (on: Sam's spot -> the anchor;
// off: the anchor -> the exit spot); `place_on_anchor`: the entity on the anchor (seated, settling).
struct GlideOut {
    GlidePhase phase = GlidePhase::none;
    double pose_w = 0, place_t = 0;
    bool place_on_anchor = false;
};
struct Glide {
    GlidePhase phase = GlidePhase::none;
    double t0 = 0;
    void StartOn(double now) { phase = GlidePhase::on, t0 = now; }
    void Seated() { phase = GlidePhase::seated; }  // a resume / a snap: no glide
    void StartOff(double now) { phase = GlidePhase::settle, t0 = now; }
    void Reset() { phase = GlidePhase::none, t0 = 0; }
    GlideOut Step(double now) {
        GlideOut o;
        const double e = now - t0;
        if (phase == GlidePhase::on && e >= kBoardGlideMs) phase = GlidePhase::seated;
        if (phase == GlidePhase::settle && e >= kAlightSettleMs) phase = GlidePhase::off, t0 += kAlightSettleMs;
        if (phase == GlidePhase::off && now - t0 >= kAlightGlideMs) phase = GlidePhase::done;
        const double e2 = now - t0;
        o.phase = phase;
        switch (phase) {
            case GlidePhase::on: o.place_t = e2 / kBoardGlideMs, o.pose_w = Ease(e2 / kBoardGlideMs); break;
            case GlidePhase::seated: o.pose_w = 1, o.place_t = 1, o.place_on_anchor = true; break;
            case GlidePhase::settle: o.pose_w = 1 - Ease(e2 / kAlightSettleMs), o.place_on_anchor = true; break;
            case GlidePhase::off: o.place_t = e2 / kAlightGlideMs; break;
            case GlidePhase::done: o.place_t = 1; break;
            default: break;
        }
        if (o.place_t < 0) o.place_t = 0;
        if (o.place_t > 1) o.place_t = 1;
        return o;
    }
};

// ---------------------------------------------------------------- release A: the in-session library (seated MULEs' idles)
inline constexpr unsigned kLibMaxFrames = 120;   // one seat's loop
inline constexpr unsigned kLibLearnFrames = 240; // the learning window (frames of one seat, one episode)
inline constexpr double kLibMinMs = 1500, kLibMaxMs = 4000, kLibMaxGapMs = 400, kLibSeamMaxDeg = 14.0;
inline constexpr double kLibFeetTolM = 0.10;    // played: the lower foot joint within this of kHangFeetZ ...
inline constexpr unsigned kLibFeetBadRun = 30;   // ... or after this many updates in a row the library is off (procedural)

// Review (logic 1 / ux 5): which frames of a MULE on a side seat the library learns from. Only a MULE sitting on its anchor
// (its entity's place in the truck's frame, measured: not the seat table's, within kLearnAnchorM; a MULE climbing on or off,
// parented but still in its animation, is elsewhere) and not alerted (its phase known and >= kLearnPhaseAlert: chasing,
// aiming, shooting from the seat; unknown (0xFF) passes). nullptr: learn it; else why not.
inline constexpr double kLearnAnchorM = 0.10;
inline constexpr std::uint8_t kLearnPhaseAlert = 4, kLearnPhaseUnknown = 0xFF;
inline const char* LearnRefusal(int seat, bool lpos_measured, const double (&lpos)[3], std::uint8_t phase) {
    double a[3];
    if (!SideAnchor(seat, a)) return "not a side seat";
    if (!lpos_measured) return "its place is not measured";
    for (double v : lpos)
        if (!std::isfinite(v)) return "its place is not measured";
    const double dx = lpos[0] - a[0], dy = lpos[1] - a[1], dz = lpos[2] - a[2];
    if (!(dx * dx + dy * dy + dz * dz <= kLearnAnchorM * kLearnAnchorM)) return "off its anchor (getting on or off)";
    if (phase != kLearnPhaseUnknown && phase >= kLearnPhaseAlert) return "alerted";
    return nullptr;
}

// Review (ux 4): Sam standing still, the procedural hang's base. An update is still when every joint (fingers aside) turned
// under kStillDeg since the last one (min_dot: the smallest |q . q_prev|); kStillRun still updates in a row keep the pose.
inline constexpr double kStillDeg = 1.0;
inline constexpr unsigned kStillRun = 12;
inline unsigned StillStep(double min_dot, unsigned run) {
    const double lim = std::cos(kStillDeg * 0.5 * 3.14159265358979323846 / 180.0);
    if (!(min_dot >= lim)) return 0;
    return run < 1000000u ? run + 1 : run;
}
// The procedural hang's crouch: the pelvis this much lower than the standing pose with the feet on the step (the MULEs' pelvis
// sits 0.81 - 1.0 m over the anchor, Sam's standing one ~1.1 m), the knees bent by a two-bone leg IK that keeps the feet where
// the lift put them. kLegFailRun failed leg solves in a row: no crouch for the rest of that carry.
inline constexpr double kHangCrouchM = 0.15;
inline constexpr unsigned kLegFailRun = 30;

// Review (ux 7 / 8): the side glide's facing and path.
// The yaw (radians, relative to the truck's) at glide fraction t: from `from_rel` to `to_rel` the short way, eased.
inline double WrapPiD(double a) {
    const double tau = 2.0 * 3.14159265358979323846;
    if (!std::isfinite(a)) return 0;
    a = std::fmod(a + 3.14159265358979323846, tau);
    if (a < 0) a += tau;
    return a - 3.14159265358979323846;
}
inline double GlideYaw(double from_rel, double to_rel, double t) { return WrapPiD(from_rel + WrapPiD(to_rel - from_rel) * Ease(t)); }
// Getting on from farther than kGlideViaM past the seat's approach point (1 m out from the anchor, at the anchor's height):
// first there along the ground, then up onto the anchor with the arc (as the MULEs climb from beside the slot), split by the
// two legs' lengths; else GlidePos straight. `via`: the approach point in the truck's frame (its z: where Sam stood).
inline constexpr double kGlideViaM = 0.3;
inline void GlidePath(const double (&a)[3], const double (&via)[3], const double (&b)[3], double t, double (&out)[3]) {
    const double d1 = std::sqrt((via[0] - a[0]) * (via[0] - a[0]) + (via[1] - a[1]) * (via[1] - a[1]));
    const double d2 = std::sqrt((b[0] - via[0]) * (b[0] - via[0]) + (b[1] - via[1]) * (b[1] - via[1]));
    if (!(d1 > kGlideViaM) || !(d2 > 0.05) || !std::isfinite(d1 + d2)) return GlidePos(a, b, t, out);
    const double e = Ease(t), s = d1 / (d1 + d2);
    if (e < s) {
        const double u = e / s;
        for (int i = 0; i < 3; ++i) out[i] = a[i] + (via[i] - a[i]) * u;
    } else {
        const double u = s < 1 ? (e - s) / (1 - s) : 1;
        for (int i = 0; i < 3; ++i) out[i] = via[i] + (b[i] - via[i]) * u;
        out[2] += kGlideArcM * 4 * u * (1 - u);
    }
}

inline bool UnitFrame(const PoseFrame& f) {
    for (unsigned j = 0; j < kJoints; ++j) {
        const double n = std::sqrt(static_cast<double>(f.q[j][0]) * f.q[j][0] + static_cast<double>(f.q[j][1]) * f.q[j][1] +
                                   static_cast<double>(f.q[j][2]) * f.q[j][2] + static_cast<double>(f.q[j][3]) * f.q[j][3]);
        if (!(std::fabs(n - 1.0) <= kQuatTol)) return false;
    }
    for (int k = 0; k < 3; ++k)
        if (!std::isfinite(f.root_t[k]) || !std::isfinite(f.pelvis_t[k]) || std::fabs(f.root_t[k]) > 3 || std::fabs(f.pelvis_t[k]) > 3) return false;
    return std::isfinite(f.ms);
}
// The largest joint angle between two frames (fingers left out), and the translations' largest difference (m) as degrees per cm.
inline double FrameDiffDeg(const PoseFrame& a, const PoseFrame& b) {
    double worst = 0;
    for (unsigned j = 0; j < kJoints; ++j) {
        if (IsFinger(j)) continue;
        const double d = QuatAngleDeg(a.q[j], b.q[j]);
        if (d > worst) worst = d;
    }
    for (int k = 0; k < 3; ++k) {
        const double d = std::fabs(static_cast<double>(a.pelvis_t[k]) - b.pelvis_t[k]) * 100.0;  // 1 cm = 1 degree
        if (d > worst) worst = d;
    }
    return worst;
}
struct LoopInfo {
    bool ok = false;
    const char* why = "no frames";
    unsigned start = 0, n = 0;  // frames start .. start + n - 1; the last one is the seam (it plays as the first)
    double len_ms = 0, seam_deg = 0;
};
inline LoopInfo LoopPick(const PoseFrame* f, std::size_t n) {
    LoopInfo li;
    if (!f || n < 2) return li;
    // the longest run of good frames (unit, increasing time, no long gap) ending at the newest
    std::size_t first = n - 1;
    if (!UnitFrame(f[n - 1])) return li.why = "non-unit frames", li;
    while (first > 0 && UnitFrame(f[first - 1]) && f[first].ms > f[first - 1].ms && f[first].ms - f[first - 1].ms <= kLibMaxGapMs) --first;
    if (f[n - 1].ms - f[first].ms < kLibMinMs) return li.why = "too short", li;
    double best = 1e9;
    std::size_t bi = 0, bj = 0;
    for (std::size_t i = first; i < n; ++i)
        for (std::size_t j = i + 1; j < n; ++j) {
            const double len = f[j].ms - f[i].ms;
            if (len < kLibMinMs) continue;
            if (len > kLibMaxMs || j - i + 1 > kLibMaxFrames) break;
            const double d = FrameDiffDeg(f[i], f[j]);
            if (d < best) best = d, bi = i, bj = j;
        }
    if (best > 1e8) return li.why = "no window", li;
    li.start = static_cast<unsigned>(bi), li.n = static_cast<unsigned>(bj - bi + 1);
    li.len_ms = f[bj].ms - f[bi].ms, li.seam_deg = best;
    if (best > kLibSeamMaxDeg) return li.why = "no seam", li;
    li.ok = true, li.why = "";
    return li;
}
// nlerp with the hemisphere fix
inline void QuatBlend(const float (&a)[4], const float (&b)[4], double t, float (&o)[4]) {
    const double d = static_cast<double>(a[0]) * b[0] + static_cast<double>(a[1]) * b[1] + static_cast<double>(a[2]) * b[2] + static_cast<double>(a[3]) * b[3];
    const double s = d < 0 ? -1.0 : 1.0;
    double r[4], n2 = 0;
    for (int k = 0; k < 4; ++k) r[k] = a[k] * (1 - t) + s * b[k] * t, n2 += r[k] * r[k];
    const double n = std::sqrt(n2);
    if (!(n > 1e-6)) {
        std::memcpy(o, a, sizeof(o));
        return;
    }
    for (int k = 0; k < 4; ++k) o[k] = static_cast<float>(r[k] / n);
}
inline void FrameBlend(const PoseFrame& a, const PoseFrame& b, double t, PoseFrame& o) {
    for (unsigned j = 0; j < kJoints; ++j) QuatBlend(a.q[j], b.q[j], t, o.q[j]);
    for (int k = 0; k < 3; ++k) {
        o.root_t[k] = static_cast<float>(a.root_t[k] + (b.root_t[k] - a.root_t[k]) * t);
        o.pelvis_t[k] = static_cast<float>(a.pelvis_t[k] + (b.pelvis_t[k] - a.pelvis_t[k]) * t);
    }
    o.ms = a.ms + (b.ms - a.ms) * t;
}
// A loop's frames f[0 .. n-1] (f[n-1] the seam: it plays as f[0]) at `t_ms` since the start, wrapped.
inline bool LoopSample(const PoseFrame* f, std::size_t n, double t_ms, PoseFrame& out) {
    if (!f || n < 2 || !std::isfinite(t_ms)) return false;
    const double len = f[n - 1].ms - f[0].ms;
    if (!(len > 0)) return false;
    double t = std::fmod(t_ms, len);
    if (t < 0) t += len;
    const double at = f[0].ms + t;
    std::size_t i = 0;
    while (i + 2 < n && f[i + 1].ms <= at) ++i;
    const double span = f[i + 1].ms - f[i].ms;
    const double u = span > 0 ? (at - f[i].ms) / span : 0;
    // the seam's frame is the first one again
    FrameBlend(f[i], i + 1 == n - 1 ? f[0] : f[i + 1], u < 0 ? 0 : u > 1 ? 1 : u, out);
    out.ms = t;
    return true;
}

// ---------------------------------------------------------------- release A: the library's cache file
// "STML" | u32 version | u32 layout hash | u64 exe fingerprint | u32 seats (4) | per seat: u32 n, f32 seam_deg, n frames
// (f32 ms since the loop's start, 77 x 4 f32 rotations, 3 f32 root t, 3 f32 pelvis t) | u32 CRC-32 of all before it. Little endian.
// 2 (review logic 1 / ux 5): a version-1 file may hold a loop learned from an alerted or climbing MULE: refused, relearned.
inline constexpr std::uint32_t kLibVersion = 2;
inline constexpr std::uint32_t kLayoutHash = 0xBE39C83Fu;  // Sam's / the MULE's skeleton
inline constexpr std::size_t kLibFrameBytes = 4 + kJoints * 16 + 12 + 12;
inline constexpr std::size_t kLibMaxBytes = 24 + 4 * (8 + kLibMaxFrames * kLibFrameBytes) + 4;
struct SeatLoop {
    unsigned n = 0;
    float seam_deg = 0;
    PoseFrame f[kLibMaxFrames];
};
// CRC-32 (IEEE, reflected 0xEDB88320, init and final xor 0xFFFFFFFF).
inline std::uint32_t Crc32(const std::uint8_t* p, std::size_t n, std::uint32_t crc = 0) {
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
namespace libio {
inline void U32(std::vector<std::uint8_t>& o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
inline void F32(std::vector<std::uint8_t>& o, float v) {
    std::uint32_t u;
    std::memcpy(&u, &v, 4);
    U32(o, u);
}
inline std::uint32_t GetU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
inline float GetF32(const std::uint8_t* p) {
    const std::uint32_t u = GetU32(p);
    float v;
    std::memcpy(&v, &u, 4);
    return v;
}
}  // namespace libio
// `loops`: 4 seats (MULE seats 1..4 at 0..3).
inline void LibEncode(const SeatLoop* loops, std::uint64_t exe_fp, std::vector<std::uint8_t>& out) {
    out.clear();
    out.reserve(4096);
    out.push_back('S'), out.push_back('T'), out.push_back('M'), out.push_back('L');
    libio::U32(out, kLibVersion);
    libio::U32(out, kLayoutHash);
    libio::U32(out, static_cast<std::uint32_t>(exe_fp));
    libio::U32(out, static_cast<std::uint32_t>(exe_fp >> 32));
    libio::U32(out, 4);
    for (unsigned s = 0; s < 4; ++s) {
        const SeatLoop& l = loops[s];
        const unsigned n = l.n <= kLibMaxFrames ? l.n : 0;
        libio::U32(out, n);
        libio::F32(out, l.seam_deg);
        for (unsigned i = 0; i < n; ++i) {
            const PoseFrame& f = l.f[i];
            libio::F32(out, static_cast<float>(f.ms - l.f[0].ms));
            for (unsigned j = 0; j < kJoints; ++j)
                for (int k = 0; k < 4; ++k) libio::F32(out, f.q[j][k]);
            for (int k = 0; k < 3; ++k) libio::F32(out, f.root_t[k]);
            for (int k = 0; k < 3; ++k) libio::F32(out, f.pelvis_t[k]);
        }
    }
    libio::U32(out, Crc32(out.data(), out.size()));
}
// Strict: the magic, the version, the layout hash, this exe's fingerprint, 4 seats, every count <= kLibMaxFrames, the size, the
// CRC, every frame unit with increasing times; a seat with 0 or 2+ frames. False with `why`; `loops` then all empty.
inline bool LibDecode(const std::uint8_t* p, std::size_t size, std::uint64_t exe_fp, SeatLoop* loops, const char** why) {
    const char* w = "";
    for (unsigned s = 0; s < 4; ++s) loops[s].n = 0;
    const auto fail = [&](const char* r) {
        for (unsigned s = 0; s < 4; ++s) loops[s].n = 0;
        if (why) *why = r;
        return false;
    };
    if (!p || size < 28 || size > kLibMaxBytes) return fail("size");
    if (std::memcmp(p, "STML", 4) != 0) return fail("magic");
    if (Crc32(p, size - 4) != libio::GetU32(p + size - 4)) return fail("crc");
    if (libio::GetU32(p + 4) != kLibVersion) return fail("version");
    if (libio::GetU32(p + 8) != kLayoutHash) return fail("layout");
    const std::uint64_t fp = libio::GetU32(p + 12) | static_cast<std::uint64_t>(libio::GetU32(p + 16)) << 32;
    if (fp != exe_fp) return fail("another game build");
    if (libio::GetU32(p + 20) != 4) return fail("seats");
    std::size_t at = 24;
    for (unsigned s = 0; s < 4; ++s) {
        if (at + 8 > size - 4) return fail("size");
        const std::uint32_t n = libio::GetU32(p + at);
        if (n > kLibMaxFrames || n == 1) return fail("count");
        loops[s].seam_deg = libio::GetF32(p + at + 4);
        at += 8;
        if (at + static_cast<std::size_t>(n) * kLibFrameBytes > size - 4) return fail("size");
        for (unsigned i = 0; i < n; ++i) {
            PoseFrame& f = loops[s].f[i];
            f.ms = libio::GetF32(p + at), at += 4;
            for (unsigned j = 0; j < kJoints; ++j)
                for (int k = 0; k < 4; ++k) f.q[j][k] = libio::GetF32(p + at), at += 4;
            for (int k = 0; k < 3; ++k) f.root_t[k] = libio::GetF32(p + at), at += 4;
            for (int k = 0; k < 3; ++k) f.pelvis_t[k] = libio::GetF32(p + at), at += 4;
            if (!UnitFrame(f) || (i > 0 && !(f.ms > loops[s].f[i - 1].ms))) return fail("a bad frame");
        }
        loops[s].n = n;
    }
    if (at != size - 4) return fail("size");
    if (why) *why = w;
    return true;
}

}  // namespace st::muleride
