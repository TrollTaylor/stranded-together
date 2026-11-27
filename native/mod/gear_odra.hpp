// Round 28 O1 (docs/real-partner-body.md section 12.9): the partner's Odradek on the partner body, the pure part. No game
// memory, no Windows: unit-tested in tests/gear_odra_tests.cpp. The game side is partner_gear.cpp (ReadOdradek, OdraTick).
//
// The sender reads its own Odradek every gear read (100 ms): the arm (inv +0x4C88, DSOdradekArmEntity) and the body
// (inv +0x4CA0, DSOdradekBodyEntity), both inventory references (entity = value - 0x20). Both go out as STC1 items of
// GearKind::odradek, `model` naming the part (kPartBody / kPartArm), each with its world pose relative to one joint of
// Sam's skeleton. The receiver draws them as the cinematic props demo_odra_main_def_new / demo_odra_arms_def_new (the
// player's own art parts on the same skeletons; no DSOdradek*Entity, which crashes a copy at 0x14296F3AE). Nothing is
// sent or drawn unless both games have odradek_gear=1: each announces it with the STC1 flag kGearOdraCap.
//
// Not a signal: inv +0x4C7F bit 0 (the one-shot request ActivateOdradek sets and the inventory update clears; round 28
// p4 run: 0 at idle with the Odradek out). The presence is the refs, their classes, both entities' visible bits and the
// body's +0x3E4 bit 2 (the bit the game's own predicate FUN_14295BA70 tests first) with a known state +0x348.
#pragma once
#include <cmath>
#include <cstdint>

namespace st::gearodra {

// STC1 GearKind::odradek: `model` names the part.
inline constexpr std::uint8_t kPartBody = 0;     // the body (odra_main_def, 44 joints) -> demo_odra_main_def_new
inline constexpr std::uint8_t kPartArm = 1;      // the arm (odra_arms_def, 14 joints) -> demo_odra_arms_def_new
inline constexpr std::uint8_t kPartLegacy = 0xFF;  // wire::kGearNone: the kind's old meaning (one prop, the body); never sent

// The receiver's part for an item's model: kPartBody, kPartArm, or -1 (any other value is refused: not drawn).
inline int PartOf(std::uint8_t model) noexcept {
    if (model == kPartBody || model == kPartLegacy) return kPartBody;
    if (model == kPartArm) return kPartArm;
    return -1;
}

// The body's state byte +0x348 values the game uses (1 .. 0x21: r-odradek.md 2.1); 0 is none / not set up.
inline constexpr std::uint8_t kStateMin = 0x01, kStateMax = 0x21;

// The body is set up and drawn by the game: +0x3E4 bit 2 (FUN_14295BA70's first test) and a known state. Not the scan
// predicate itself (kBodyReadyMask in odra_rules.hpp): its mask leaves out 0x14 (open), so it reads "not ready" all
// through a scan, and the Odradek on the shoulder must not blink off while its owner scans.
inline bool BodyUp(std::uint32_t flags3e4, std::uint8_t state348) noexcept {
    return (flags3e4 & 4u) != 0 && state348 >= kStateMin && state348 <= kStateMax;
}

// The sender's own Odradek as read this pass (fault-free reads).
struct In {
    bool on = false;           // odradek_gear=1 (or `gear 40 1`) and the Odradek byte checks passed
    bool partner_cap = false;  // the partner's newest STC1 frame carries kGearOdraCap (its build draws both parts, its key is on)
    bool arm = false;          // inv +0x4C88 names a DSOdradekArmEntity
    bool body = false;         // inv +0x4CA0 names a DSOdradekBodyEntity
    bool arm_vis = false;      // the arm's entity +0x88 bit 1
    bool body_vis = false;     // the body's entity +0x88 bit 1
    std::uint32_t body_flags = 0;  // body +0x3E4
    std::uint8_t body_state = 0;   // body +0x348
    bool on_sam = false;       // both world transforms read and within 5 m of Sam
    bool riding = false;       // Sam is seated in a vehicle or carried as a passenger
    bool cutscene = false;     // a load or set-up, a cutscene, an in-game cutscene, a movie (or the `gear 40 3 1` test hold)
};

enum class Why : std::uint8_t { shown, off, no_cap, cutscene, riding, no_odradek, hidden, off_sam, not_up };
inline constexpr unsigned kWhyCount = 9;

// What this read says (the first reason that applies, in this order).
inline Why Decide(const In& in) noexcept {
    if (!in.on) return Why::off;
    if (!in.partner_cap) return Why::no_cap;
    if (in.cutscene) return Why::cutscene;
    if (in.riding) return Why::riding;
    if (!in.arm || !in.body) return Why::no_odradek;
    if (!in.arm_vis || !in.body_vis) return Why::hidden;
    if (!in.on_sam) return Why::off_sam;
    if (!BodyUp(in.body_flags, in.body_state)) return Why::not_up;
    return Why::shown;
}

inline const char* WhyName(Why w) noexcept {
    switch (w) {
        case Why::shown: return "shown";
        case Why::off: return "off (odradek_gear=0)";
        case Why::no_cap: return "the partner does not draw it (its frames lack the Odradek flag: odradek_gear=0 there or an older build)";
        case Why::cutscene: return "a load / cutscene / movie";
        case Why::riding: return "riding a vehicle";
        case Why::no_odradek: return "no Odradek (an arm / body ref missing or not an Odradek entity)";
        case Why::hidden: return "the game hides it (an entity's visible bit clear)";
        case Why::off_sam: return "not on Sam (unreadable or over 5 m away)";
        case Why::not_up: return "not set up (body +0x3E4 bit 2 clear or an unknown state)";
    }
    return "?";
}

// A hard hide (the key off, no capability, a cutscene, a ride) applies at once; the game hiding it or a lost ref after
// kHideAfterMs; a body that is not set up after kNotUpHideMs (a short state the scan or an animation passes through must
// not respawn the props). Shown again after kShowAfterMs of reads that all say shown.
inline constexpr double kShowAfterMs = 150, kHideAfterMs = 250, kNotUpHideMs = 2000;
inline bool HardHide(Why w) noexcept { return w == Why::off || w == Why::no_cap || w == Why::cutscene || w == Why::riding; }

struct Hold {
    bool shown = false;
    double pending_ms = -1;  // since when the reads disagree with `shown` (-1: they agree)
    Why last = Why::off;     // the newest read's reason
};

// One read: returns whether the parts go out now; *changed when that flipped on this read.
inline bool Step(Hold& h, Why w, double now_ms, bool* changed = nullptr) noexcept {
    if (changed) *changed = false;
    h.last = w;
    const bool want = w == Why::shown;
    if (want == h.shown) {
        h.pending_ms = -1;
        return h.shown;
    }
    if (h.pending_ms < 0 || now_ms < h.pending_ms) h.pending_ms = now_ms;
    const double need = want ? kShowAfterMs : HardHide(w) ? 0.0 : w == Why::not_up ? kNotUpHideMs : kHideAfterMs;
    if (now_ms - h.pending_ms >= need) {
        h.shown = want;
        h.pending_ms = -1;
        if (changed) *changed = true;
    }
    return h.shown;
}

// ---- the anchor joint (Sam's 77-joint layout) both parts' poses are measured against
inline constexpr std::uint8_t kJointShoulderL = 35;  // JNT_L_B_000_Shoulder: where the game hangs the arm (HLP_Odradek)
inline constexpr std::uint8_t kJointSpine5 = 5;      // the chest: the torso rule (section 12; the shoulder frames differ on Fragile)
// auto: the left shoulder when the other screen draws us as Sam (the game's own attach, exact on a Sam body), the chest when it
// draws us as Fragile (run 20261117-132631: a prop anchored on the shoulder turned on her skeleton). Overridden by `gear 40 4 <j>`.
inline std::uint8_t AnchorJoint(bool drawn_as_fragile, int force) noexcept {
    if (force >= 0 && force < 77) return static_cast<std::uint8_t>(force);
    return drawn_as_fragile ? kJointSpine5 : kJointShoulderL;
}

// Sam's HLP_Odradek (sam_body skeleton helpers, index 35 = JNT_L_B_000_Shoulder; decoded offline, static-Q5Q6): the arm's
// place in joint 35's frame. The axes as decoded (three vectors); which of rows / columns the game means is what the run's
// sender line tells (both are compared). The game attaches the arm there with an identity mover (AttachToHelperMover).
inline constexpr float kHelperT[3] = {0.054615f, -0.246383f, 0.113942f};
inline constexpr float kHelperAxes[3][3] = {{0.0f, -0.93265f, -0.36078f}, {0.0f, 0.36078f, -0.93265f}, {1.0f, 0.0f, 0.0f}};

// The angle (deg) between two rotations given as 3 rows each (normalised; 0..180, not finite input: 180). From the Frobenius
// distance (|A - B|^2 = 8 sin^2(angle / 2) for rotations), not acos of the trace: exact at 0 even when the rows are only
// nearly unit (the decoded helper axes are 5 digits: acos would read 0.3 deg for the helper against itself).
inline double AngleDeg(const float (&a)[3][3], const float (&b)[3][3]) noexcept {
    double s = 0;
    for (int k = 0; k < 3; ++k)
        for (int c = 0; c < 3; ++c) s += (static_cast<double>(a[k][c]) - b[k][c]) * (static_cast<double>(a[k][c]) - b[k][c]);
    if (!std::isfinite(s)) return 180.0;
    double h = std::sqrt(s) / (2.0 * 1.4142135623730951);
    h = h > 1.0 ? 1.0 : h;
    return 2.0 * std::asin(h) * 57.29577951308232;
}

// The arm's measured pose on joint 35 against HLP_Odradek: the distance (cm), and the angle with the decoded axes taken as
// rows and as columns (deg).
struct HelperDelta {
    double cm = 0, deg_rows = 180, deg_cols = 180;
};
inline HelperDelta CompareHelper(const float (&p)[3], const float (&r)[3][3]) noexcept {
    HelperDelta d;
    double s = 0;
    for (int k = 0; k < 3; ++k) s += (static_cast<double>(p[k]) - kHelperT[k]) * (static_cast<double>(p[k]) - kHelperT[k]);
    d.cm = std::isfinite(s) ? std::sqrt(s) * 100.0 : 1e9;
    float rows[3][3], cols[3][3];
    for (int k = 0; k < 3; ++k)
        for (int c = 0; c < 3; ++c) rows[k][c] = kHelperAxes[k][c], cols[k][c] = kHelperAxes[c][k];
    d.deg_rows = AngleDeg(r, rows);
    d.deg_cols = AngleDeg(r, cols);
    return d;
}

// Two model-space joint matrices (4 rows: the axes, then the position; a skinned model's pose): the distance (cm) and the
// angle (deg) between them, the axes normalised first. False when either is not finite or degenerate.
inline bool JointDelta(const float (&a)[4][4], const float (&b)[4][4], double* cm, double* deg) noexcept {
    float ra[3][3], rb[3][3];
    for (int k = 0; k < 3; ++k) {
        const double la = std::sqrt(static_cast<double>(a[k][0]) * a[k][0] + static_cast<double>(a[k][1]) * a[k][1] + static_cast<double>(a[k][2]) * a[k][2]);
        const double lb = std::sqrt(static_cast<double>(b[k][0]) * b[k][0] + static_cast<double>(b[k][1]) * b[k][1] + static_cast<double>(b[k][2]) * b[k][2]);
        if (!(la > 1e-6) || !(lb > 1e-6) || !std::isfinite(la) || !std::isfinite(lb)) return false;
        for (int c = 0; c < 3; ++c) ra[k][c] = static_cast<float>(a[k][c] / la), rb[k][c] = static_cast<float>(b[k][c] / lb);
    }
    double s = 0;
    for (int c = 0; c < 3; ++c) s += (static_cast<double>(a[3][c]) - b[3][c]) * (static_cast<double>(a[3][c]) - b[3][c]);
    if (!std::isfinite(s)) return false;
    if (cm) *cm = std::sqrt(s) * 100.0;
    if (deg) *deg = AngleDeg(ra, rb);
    return true;
}

// The skeletons the props share with the player's parts (static-Q5Q6: the same Skeleton objects).
inline constexpr unsigned kArmJoints = 14, kBodyJoints = 44;

// A placement check on the receiver: placed (where the prop is) against wanted (the anchor joint times the item's pose).
inline constexpr double kAttachOkCm = 2.0;  // the gate's "attached" threshold (the detour places it from the same pose)

}  // namespace st::gearodra
