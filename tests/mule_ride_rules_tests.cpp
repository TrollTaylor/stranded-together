// Round 30 stage 0 (native/mod/mule_ride_rules.hpp): the helper keys (CRC-32C & 0x7FFFFFFF), the seat / hand helpers, the side
// anchors, the recorder's trigger and the clip validation.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "mule_ride_rules.hpp"

using namespace st;
using namespace st::muleride;

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

// An independent table CRC-32C (the slicing the game's 8 / 4 / 2 / 1 steps amount to, byte by byte).
std::uint32_t TableCrc(const char* s, std::size_t n, std::uint32_t crc) {
    static std::uint32_t t[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
            t[i] = c;
        }
        init = true;
    }
    for (std::size_t i = 0; i < n; ++i) crc = t[(crc ^ static_cast<std::uint8_t>(s[i])) & 0xFF] ^ (crc >> 8);
    return crc;
}

void TestKeys() {
    // the standard check value (init and xorout 0xFFFFFFFF): the polynomial and the bit order are CRC-32C's
    CHECK((Crc32c("123456789", 9, 0xFFFFFFFFu) ^ 0xFFFFFFFFu) == 0xE3069283u);
    CHECK(Crc32c("", 0) == 0);
    for (const char* n : kHelperNames) {
        const std::uint32_t k = HelperKey(n);
        CHECK(k == (TableCrc(n, std::strlen(n), 0) & 0x7FFFFFFFu));
        CHECK((k & 0x80000000u) == 0);
        CHECK(k != 0x7FFFFFFFu);  // the game's "not computed yet" (-1 before the mask)
    }
    // all different
    for (std::size_t a = 0; a < kHelperCount; ++a)
        for (std::size_t b = a + 1; b < kHelperCount; ++b) CHECK(HelperKey(kHelperNames[a]) != HelperKey(kHelperNames[b]));
    static_assert(HelperKey("HLP_Driver") == (Crc32c("HLP_Driver", 10) & 0x7FFFFFFFu), "constexpr key");
}

void TestSeats() {
    CHECK(std::strcmp(SeatHelper(0), "HLP_Driver") == 0);
    CHECK(std::strcmp(SeatHelper(1), "HLP_Enemy_3") == 0);
    CHECK(std::strcmp(SeatHelper(2), "HLP_Enemy") == 0);
    CHECK(std::strcmp(SeatHelper(3), "HLP_Enemy_4") == 0);
    CHECK(std::strcmp(SeatHelper(4), "HLP_Enemy_2") == 0);
    CHECK(std::strcmp(SeatHelper(9), "HLP_Driver") == 0);
    CHECK(std::strcmp(HandHelper(0, true), "HLP_HandleHandL") == 0);
    CHECK(std::strcmp(HandHelper(0, false), "HLP_HandleHandR") == 0);
    CHECK(std::strcmp(HandHelper(1, true), "HLP_MuleHandRF") == 0 && !HandHelper(1, false));
    CHECK(std::strcmp(HandHelper(3, true), "HLP_MuleHandRB") == 0 && !HandHelper(3, false));
    CHECK(std::strcmp(HandHelper(2, false), "HLP_MuleHandLF") == 0 && !HandHelper(2, true));
    CHECK(std::strcmp(HandHelper(4, false), "HLP_MuleHandLB") == 0 && !HandHelper(4, true));
    CHECK(!HandHelper(5, true) && !HandHelper(5, false));
    CHECK(std::strcmp(InboardHelper(1), "HLP_MuleHandRF") == 0 && std::strcmp(InboardHelper(2), "HLP_MuleHandLF") == 0);
    CHECK(std::strcmp(InboardHelper(3), "HLP_MuleHandRB") == 0 && std::strcmp(InboardHelper(4), "HLP_MuleHandLB") == 0);
    CHECK(!InboardHelper(0) && InboardHandJoint(0) == -1);
    CHECK(InboardHandJoint(1) == 38 && InboardHandJoint(3) == 38 && InboardHandJoint(2) == 9 && InboardHandJoint(4) == 9);
    // every helper the mapping names is in the list
    for (int s = 0; s <= 4; ++s) {
        bool found = false;
        for (const char* n : kHelperNames) found |= std::strcmp(n, SeatHelper(s)) == 0;
        CHECK(found);
    }
    // the anchors agree with truck_v3's seat table (within 2 mm)
    for (int s = 1; s <= 4; ++s) {
        double a[3], t[3];
        CHECK(SideAnchor(s, a) && truckv3::SeatTableLocal(static_cast<std::uint8_t>(s), t));
        CHECK(Dist(a, t) < 0.002);
    }
    double a[3];
    CHECK(!SideAnchor(0, a) && !SideAnchor(5, a));
    CHECK(!IsFinger(9) && IsFinger(10) && IsFinger(28) && !IsFinger(29) && !IsFinger(38) && IsFinger(39) && IsFinger(57) && !IsFinger(58) && !IsFinger(63));
}

void TestFrames() {
    truckv3::Xf e;
    e.pos[0] = 10, e.pos[1] = 20, e.pos[2] = 30;
    // yaw 90: x -> y, y -> -x
    e.rot[0][0] = 0, e.rot[0][1] = 1, e.rot[0][2] = 0;
    e.rot[1][0] = -1, e.rot[1][1] = 0, e.rot[1][2] = 0;
    e.rot[2][0] = 0, e.rot[2][1] = 0, e.rot[2][2] = 1;
    const float p[3] = {1, 2, 3};
    double w[3], l[3];
    PointWorld(e, p, w);
    CHECK(std::fabs(w[0] - 8) < 1e-9 && std::fabs(w[1] - 21) < 1e-9 && std::fabs(w[2] - 33) < 1e-9);
    PointLocal(e, w, l);
    CHECK(std::fabs(l[0] - 1) < 1e-6 && std::fabs(l[1] - 2) < 1e-6 && std::fabs(l[2] - 3) < 1e-6);
}

void TestPair() {
    LocalSample s[4];
    s[0].ms = 0, s[1].ms = 16, s[2].ms = 33, s[3].ms = 100;
    CHECK(PairIndex(s, 4, 15, 40) == 1);
    CHECK(PairIndex(s, 4, 25, 40) == 2);
    CHECK(PairIndex(s, 4, -10, 40) == 0);
    CHECK(PairIndex(s, 4, 70, 20) == -1);
    CHECK(PairIndex(s, 4, 200, 40) == -1);
    CHECK(PairIndex(s, 4, 140, 40) == 3);
    CHECK(PairIndex(nullptr, 0, 1, 40) == -1);
}

void TestWatch() {
    // a board: bit 2 rises with the vehicle id set, bit 0 later, bit 2 clears, 0.3 s on
    RecWatch w;
    CHECK(w.Step(0, false, 0) == RecEvent::none);
    CHECK(w.Step(0x4, true, 16) == RecEvent::start && w.kind == ClipKind::board);
    CHECK(w.Step(0x5, true, 1000) == RecEvent::none);
    CHECK(w.Step(0x1, true, 2000) == RecEvent::none);
    CHECK(w.Step(0x1, true, 2299) == RecEvent::none);
    CHECK(w.Step(0x1, true, 2300) == RecEvent::stop);
    w.Done();
    // an alight: riding, bit 2 rises; ends 0.5 s after bit 0 clears
    CHECK(w.Step(0x5, true, 3000) == RecEvent::start && w.kind == ClipKind::alight);
    CHECK(w.Step(0x4, false, 4000) == RecEvent::none);
    CHECK(w.Step(0x1, true, 4100) == RecEvent::none);  // back on: the clear restarts
    CHECK(w.Step(0x0, false, 4200) == RecEvent::none);
    CHECK(w.Step(0x0, false, 4700) == RecEvent::stop);
    w.Done();
    // bit 2 rises on foot with no truck: pending until the truck shows
    CHECK(w.Step(0x4, false, 5000) == RecEvent::none && w.pending);
    CHECK(w.Step(0x4, true, 5100) == RecEvent::start && w.kind == ClipKind::board && !w.pending);
    // the cap
    CHECK(w.Step(0x4, true, 11099) == RecEvent::none);
    CHECK(w.Step(0x4, true, 11100) == RecEvent::capped);
    w.Done();
    // pending dropped when bit 2 clears first
    RecWatch p;
    p.Step(0, false, 0);
    p.Step(0x4, false, 10);
    CHECK(p.pending);
    CHECK(p.Step(0, false, 20) == RecEvent::none && !p.pending);
    CHECK(p.Step(0, true, 30) == RecEvent::none && p.kind == ClipKind::none);
    // first sight with bit 2 already set: no edge
    RecWatch f;
    CHECK(f.Step(0x4, true, 0) == RecEvent::none && f.kind == ClipKind::none);
}

// A clip of n frames at `fps`, the entity moving from `from` to `to` (truck frame) and joint 5 turning at `dps`.
struct Clip {
    std::vector<PoseFrame> f;
    std::vector<LocalSample> l;
};
Clip Make(unsigned n, double fps, const double (&from)[3], const double (&to)[3], double dps) {
    Clip c;
    c.f.resize(n), c.l.resize(n);
    for (unsigned i = 0; i < n; ++i) {
        const double t = i * 1000.0 / fps;
        c.f[i].ms = 1000 + t;
        for (unsigned j = 0; j < kJoints; ++j) c.f[i].q[j][3] = 1;
        const double ang = dps * t / 1000.0 * 3.14159265358979 / 180.0;
        c.f[i].q[5][2] = static_cast<float>(std::sin(ang / 2)), c.f[i].q[5][3] = static_cast<float>(std::cos(ang / 2));
        c.l[i].ms = 1000 + t + 3;  // the game tick a little later
        const double u = n > 1 ? static_cast<double>(i) / (n - 1) : 1;
        for (int k = 0; k < 3; ++k) c.l[i].pos[k] = from[k] + (to[k] - from[k]) * u;
    }
    return c;
}

void TestClip() {
    double anchor[3];
    SideAnchor(1, anchor);
    const double far[3] = {anchor[0] + 2.0, anchor[1] - 0.5, anchor[2] - 0.8};
    const double vfar[3] = {anchor[0] + 6.0, anchor[1], anchor[2]};
    Clip c = Make(90, 30, far, anchor, 200);
    ClipVerdict v = ClipCheck(ClipKind::board, c.f.data(), c.f.size(), c.l.data(), c.l.size(), anchor);
    CHECK(v.ok && v.why[0] == 0);
    CHECK(v.frames == 90 && v.first == 0 && v.paired == 90);
    CHECK(std::fabs(v.fps - 30) < 0.1 && std::fabs(v.dur_s - 89.0 / 30) < 0.01);
    CHECK(v.end_d < 1e-9 && std::fabs(v.max_dps - 200) < 1 && v.spin_joint == 5);
    // the start trimmed to within 2.5 m
    c = Make(120, 30, vfar, anchor, 0);
    v = ClipCheck(ClipKind::board, c.f.data(), c.f.size(), c.l.data(), c.l.size(), anchor);
    CHECK(v.ok && v.first > 0 && v.start_d <= kBoardStartM);
    // a spin (2000 deg/s passes since release A; 3000 does not)
    c = Make(90, 30, far, anchor, 2000);
    v = ClipCheck(ClipKind::board, c.f.data(), c.f.size(), c.l.data(), c.l.size(), anchor);
    CHECK(v.ok);
    c = Make(90, 30, far, anchor, 3000);
    v = ClipCheck(ClipKind::board, c.f.data(), c.f.size(), c.l.data(), c.l.size(), anchor);
    CHECK(!v.ok && std::strcmp(v.why, "joint spin") == 0);
    // a frame gap
    c = Make(90, 30, far, anchor, 0);
    for (unsigned i = 50; i < 90; ++i) c.f[i].ms += 100, c.l[i].ms += 100;  // 133 ms: passes
    v = ClipCheck(ClipKind::board, c.f.data(), c.f.size(), c.l.data(), c.l.size(), anchor);
    CHECK(v.ok && v.max_gap_ms > 130);
    for (unsigned i = 50; i < 90; ++i) c.f[i].ms += 60, c.l[i].ms += 60;    // 193 ms
    v = ClipCheck(ClipKind::board, c.f.data(), c.f.size(), c.l.data(), c.l.size(), anchor);
    CHECK(!v.ok && std::strcmp(v.why, "frame gap") == 0);
    // a finger curling fast is left out of the speed check
    c = Make(90, 30, far, anchor, 0);
    for (unsigned i = 0; i < 90; ++i) {
        const double ang = 4000.0 * i / 30.0 * 3.14159265358979 / 180.0;
        c.f[i].q[12][0] = static_cast<float>(std::sin(ang / 2)), c.f[i].q[12][3] = static_cast<float>(std::cos(ang / 2));
    }
    v = ClipCheck(ClipKind::board, c.f.data(), c.f.size(), c.l.data(), c.l.size(), anchor);
    CHECK(v.ok && v.spin_joint != 12);
    // non-unit
    c = Make(90, 30, far, anchor, 0);
    c.f[10].q[3][3] = 0.9f;
    v = ClipCheck(ClipKind::board, c.f.data(), c.f.size(), c.l.data(), c.l.size(), anchor);
    CHECK(!v.ok && std::strcmp(v.why, "non-unit quaternions") == 0 && v.bad_quats == 1);
    // a jump of the entity
    c = Make(90, 30, far, anchor, 0);
    c.l[40].pos[0] += 1.0;
    v = ClipCheck(ClipKind::board, c.f.data(), c.f.size(), c.l.data(), c.l.size(), anchor);
    CHECK(!v.ok && std::strcmp(v.why, "entity jump") == 0);
    // the end off the anchor
    const double off[3] = {anchor[0] + 0.1, anchor[1], anchor[2]};
    c = Make(90, 30, far, off, 0);
    v = ClipCheck(ClipKind::board, c.f.data(), c.f.size(), c.l.data(), c.l.size(), anchor);
    CHECK(!v.ok && std::strcmp(v.why, "end off the anchor") == 0);
    // too few
    c = Make(5, 30, far, anchor, 0);
    v = ClipCheck(ClipKind::board, c.f.data(), c.f.size(), c.l.data(), c.l.size(), anchor);
    CHECK(!v.ok && std::strcmp(v.why, "too few frames") == 0);
    // no truck frame
    c = Make(90, 30, far, anchor, 0);
    v = ClipCheck(ClipKind::board, c.f.data(), c.f.size(), nullptr, 0, anchor);
    CHECK(!v.ok && std::strcmp(v.why, "no truck frame") == 0);
    // the alight: from the anchor, away
    c = Make(90, 30, anchor, far, 0);
    v = ClipCheck(ClipKind::alight, c.f.data(), c.f.size(), c.l.data(), c.l.size(), anchor);
    CHECK(v.ok && v.start_d < 1e-9 && v.end_d > 2);
    c = Make(90, 30, off, far, 0);
    v = ClipCheck(ClipKind::alight, c.f.data(), c.f.size(), c.l.data(), c.l.size(), anchor);
    CHECK(!v.ok && std::strcmp(v.why, "start off the anchor") == 0);
    c = Make(90, 30, anchor, anchor, 0);
    v = ClipCheck(ClipKind::alight, c.f.data(), c.f.size(), c.l.data(), c.l.size(), anchor);
    CHECK(!v.ok && std::strcmp(v.why, "never left the seat") == 0);
    // empty
    v = ClipCheck(ClipKind::board, nullptr, 0, nullptr, 0, anchor);
    CHECK(!v.ok && std::strcmp(v.why, "no frames") == 0);
    CHECK(std::strcmp(ClipKindName(ClipKind::board), "board") == 0 && std::strcmp(ClipKindName(ClipKind::alight), "alight") == 0);
}

// Round 30 release A: the hang, the glide, the loop and the cache file.
void TestHang() {
    CHECK(std::fabs(HangLift(0.08, 0.10) - 0.04) < 1e-12);
    CHECK(std::fabs(HangLift(0.10, 0.07) - 0.05) < 1e-12);
    CHECK(HangLift(-5, 0) == kHangLiftMax && HangLift(3, 3) == kHangLiftMin);
    CHECK(HangLift(std::nan(""), 0) == 0);
    double t[3];
    OutboardTarget(1, t);
    CHECK(t[0] > 0.3 && t[2] > 0.8);
    OutboardTarget(-1, t);
    CHECK(t[0] < -0.3);
}

void TestGlide() {
    CHECK(Ease(-1) == 0 && Ease(0) == 0 && Ease(1) == 1 && Ease(2) == 1 && std::fabs(Ease(0.5) - 0.5) < 1e-12 && Ease(0.25) < 0.25);
    const double a[3] = {0, 0, 0}, b[3] = {1, 2, 0.8};
    double p[3];
    GlidePos(a, b, 0, p);
    CHECK(p[0] == 0 && p[1] == 0 && p[2] == 0);
    GlidePos(a, b, 1, p);
    CHECK(std::fabs(p[0] - 1) < 1e-12 && std::fabs(p[2] - 0.8) < 1e-12);
    GlidePos(a, b, 0.5, p);
    CHECK(std::fabs(p[2] - (0.4 + kGlideArcM)) < 1e-12);
    Glide g;
    CHECK(g.Step(0).phase == GlidePhase::none);
    g.StartOn(1000);
    GlideOut o = g.Step(1000);
    CHECK(o.phase == GlidePhase::on && o.pose_w == 0 && o.place_t == 0 && !o.place_on_anchor);
    o = g.Step(1250);
    CHECK(o.phase == GlidePhase::on && std::fabs(o.pose_w - 0.5) < 1e-12 && std::fabs(o.place_t - 0.5) < 1e-12);
    o = g.Step(1000 + kBoardGlideMs);
    CHECK(o.phase == GlidePhase::seated && o.pose_w == 1 && o.place_on_anchor);
    g.StartOff(5000);
    o = g.Step(5000);
    CHECK(o.phase == GlidePhase::settle && o.pose_w == 1 && o.place_on_anchor);
    o = g.Step(5150);
    CHECK(o.phase == GlidePhase::settle && std::fabs(o.pose_w - 0.5) < 1e-12);
    o = g.Step(5000 + kAlightSettleMs + 200);
    CHECK(o.phase == GlidePhase::off && o.pose_w == 0 && std::fabs(o.place_t - 0.5) < 1e-12 && !o.place_on_anchor);
    o = g.Step(5000 + kAlightSettleMs + kAlightGlideMs);
    CHECK(o.phase == GlidePhase::done && o.place_t == 1);
    // a late tick skips straight through
    g.StartOff(0);
    CHECK(g.Step(10000).phase == GlidePhase::done);
    g.Seated();
    CHECK(g.Step(0).phase == GlidePhase::seated);
    g.Reset();
    CHECK(g.phase == GlidePhase::none && std::strcmp(GlidePhaseName(GlidePhase::off), "getting off") == 0);
}

// An idle of `n` frames at `fps`: joint 3 swings with period `period_ms`, the pelvis bobs with it.
std::vector<PoseFrame> Idle(unsigned n, double fps, double period_ms) {
    std::vector<PoseFrame> v(n);
    for (unsigned i = 0; i < n; ++i) {
        const double t = i * 1000.0 / fps;
        v[i].ms = 5000 + t;
        for (unsigned j = 0; j < kJoints; ++j) v[i].q[j][3] = 1;
        const double ang = 0.3 * std::sin(2 * 3.14159265358979 * t / period_ms);
        v[i].q[3][0] = static_cast<float>(std::sin(ang / 2)), v[i].q[3][3] = static_cast<float>(std::cos(ang / 2));
        v[i].pelvis_t[2] = static_cast<float>(0.95 + 0.01 * std::sin(2 * 3.14159265358979 * t / period_ms));
    }
    return v;
}

void TestLoop() {
    std::vector<PoseFrame> v = Idle(150, 30, 2000);
    LoopInfo li = LoopPick(v.data(), v.size());
    CHECK(li.ok && li.why[0] == 0 && li.seam_deg < 1.0);
    CHECK(std::fabs(li.len_ms - 2000) < 40 && li.n >= 59 && li.n <= 62);
    // the frames play back: at 0 the first, at the length the first again, half way the middle one
    PoseFrame o;
    CHECK(LoopSample(&v[li.start], li.n, 0, o) && std::fabs(o.q[3][0] - v[li.start].q[3][0]) < 1e-5);
    CHECK(LoopSample(&v[li.start], li.n, li.len_ms, o) && std::fabs(o.q[3][0] - v[li.start].q[3][0]) < 1e-5);
    CHECK(LoopSample(&v[li.start], li.n, -li.len_ms / 2, o) && std::fabs(o.q[3][0] - v[li.start + 30].q[3][0]) < 0.02);
    CHECK(LoopSample(&v[li.start], li.n, 7 * li.len_ms + 500, o) && std::fabs(o.ms - 500) < 1e-6);
    CHECK(!LoopSample(&v[0], 1, 0, o) && !LoopSample(nullptr, 5, 0, o));
    // too short; a gap cuts the run; non-unit; no seam (a steady drift)
    std::vector<PoseFrame> s = Idle(30, 30, 2000);
    CHECK(!LoopPick(s.data(), s.size()).ok && std::strcmp(LoopPick(s.data(), s.size()).why, "too short") == 0);
    v = Idle(150, 30, 2000);
    for (unsigned i = 120; i < 150; ++i) v[i].ms += 1000;
    CHECK(std::strcmp(LoopPick(v.data(), v.size()).why, "too short") == 0);
    v = Idle(150, 30, 2000);
    v[149].q[0][3] = 0.5f;
    CHECK(std::strcmp(LoopPick(v.data(), v.size()).why, "non-unit frames") == 0);
    v = Idle(150, 30, 2000);
    for (unsigned i = 0; i < 150; ++i) {
        const double ang = 0.02 * i;
        v[i].q[3][0] = static_cast<float>(std::sin(ang / 2)), v[i].q[3][3] = static_cast<float>(std::cos(ang / 2));
    }
    li = LoopPick(v.data(), v.size());
    CHECK(!li.ok && std::strcmp(li.why, "no seam") == 0 && li.seam_deg > kLibSeamMaxDeg);
    // a finger's difference does not count
    v = Idle(150, 30, 2000);
    v[149].q[20][0] = 0.6f, v[149].q[20][3] = 0.8f;
    CHECK(LoopPick(v.data(), v.size()).ok);
    // the blend keeps the hemisphere
    const float a[4] = {0, 0, 0, 1}, b[4] = {0, 0, 0, -1};
    float q[4];
    QuatBlend(a, b, 0.5, q);
    CHECK(std::fabs(q[3] - 1) < 1e-6);
}

void TestLib() {
    CHECK(Crc32(reinterpret_cast<const std::uint8_t*>("123456789"), 9) == 0xCBF43926u);
    std::vector<SeatLoop> loops(4), back(4);
    std::vector<PoseFrame> v = Idle(150, 30, 2000);
    const LoopInfo li = LoopPick(v.data(), v.size());
    CHECK(li.ok);
    for (unsigned i = 0; i < li.n; ++i) loops[0].f[i] = v[li.start + i];
    loops[0].n = li.n, loops[0].seam_deg = static_cast<float>(li.seam_deg);
    for (unsigned i = 0; i < 40; ++i) loops[2].f[i] = v[i];
    loops[2].n = 40;
    std::vector<std::uint8_t> bytes;
    const std::uint64_t fp = 0x1122334455667788ull;
    LibEncode(loops.data(), fp, bytes);
    CHECK(bytes.size() == 24 + 4 * 8 + (li.n + 40) * kLibFrameBytes + 4 && bytes.size() <= kLibMaxBytes);
    const char* why = "x";
    CHECK(LibDecode(bytes.data(), bytes.size(), fp, back.data(), &why) && why[0] == 0);
    CHECK(back[0].n == li.n && back[1].n == 0 && back[2].n == 40 && back[3].n == 0);
    CHECK(back[0].f[0].ms == 0 && std::fabs(back[0].f[li.n - 1].ms - li.len_ms) < 0.01);
    CHECK(std::memcmp(back[0].f[7].q, v[li.start + 7].q, sizeof(back[0].f[7].q)) == 0 && back[0].f[7].pelvis_t[2] == v[li.start + 7].pelvis_t[2]);
    // another game build, a flipped byte, a cut, another layout
    CHECK(!LibDecode(bytes.data(), bytes.size(), fp + 1, back.data(), &why) && std::strcmp(why, "another game build") == 0 && back[0].n == 0);
    std::vector<std::uint8_t> bad = bytes;
    bad[200] ^= 1;
    CHECK(!LibDecode(bad.data(), bad.size(), fp, back.data(), &why) && std::strcmp(why, "crc") == 0);
    CHECK(!LibDecode(bytes.data(), bytes.size() - 1, fp, back.data(), &why));
    bad = bytes;
    bad[8] ^= 1;
    const std::uint32_t c = Crc32(bad.data(), bad.size() - 4);
    for (int i = 0; i < 4; ++i) bad[bad.size() - 4 + i] = static_cast<std::uint8_t>(c >> (8 * i));
    CHECK(!LibDecode(bad.data(), bad.size(), fp, back.data(), &why) && std::strcmp(why, "layout") == 0);
    bad = bytes;
    bad[0] = 'X';
    CHECK(!LibDecode(bad.data(), bad.size(), fp, back.data(), &why) && std::strcmp(why, "magic") == 0);
    CHECK(!LibDecode(nullptr, 0, fp, back.data(), &why));
    // an empty library round-trips
    std::vector<SeatLoop> none(4);
    LibEncode(none.data(), fp, bytes);
    CHECK(bytes.size() == 24 + 32 + 4 && LibDecode(bytes.data(), bytes.size(), fp, back.data(), &why));
    // review (ux 5): version 2: a version-1 cache (learned before the alert and anchor gates) is refused and relearned
    static_assert(kLibVersion == 2, "the side library's cache version");
    LibEncode(loops.data(), fp, bytes);
    bad = bytes;
    bad[4] = 1, bad[5] = bad[6] = bad[7] = 0;
    {
        const std::uint32_t c1 = Crc32(bad.data(), bad.size() - 4);
        for (int i = 0; i < 4; ++i) bad[bad.size() - 4 + i] = static_cast<std::uint8_t>(c1 >> (8 * i));
    }
    CHECK(!LibDecode(bad.data(), bad.size(), fp, back.data(), &why) && std::strcmp(why, "version") == 0);
}

// Review (logic 1 / ux 5): which seated MULE frames the library learns from.
void TestLearn() {
    double a[3];
    for (int s = 1; s <= 4; ++s) {
        CHECK(SideAnchor(s, a));
        CHECK(LearnRefusal(s, true, a, kLearnPhaseUnknown) == nullptr);  // on its anchor, phase unknown: learned
        CHECK(LearnRefusal(s, true, a, 0) == nullptr && LearnRefusal(s, true, a, 3) == nullptr);  // normal .. caution
        CHECK(std::strcmp(LearnRefusal(s, true, a, 4), "alerted") == 0 && std::strcmp(LearnRefusal(s, true, a, 5), "alerted") == 0);
        CHECK(std::strcmp(LearnRefusal(s, false, a, 0), "its place is not measured") == 0);
        double off[3] = {a[0], a[1], a[2] + 0.12};  // climbing on
        CHECK(std::strcmp(LearnRefusal(s, true, off, 0), "off its anchor (getting on or off)") == 0);
        double near_[3] = {a[0] + 0.05, a[1] - 0.05, a[2] + 0.05};
        CHECK(LearnRefusal(s, true, near_, 0) == nullptr);
        double nan_[3] = {std::nan(""), a[1], a[2]};
        CHECK(std::strcmp(LearnRefusal(s, true, nan_, 0), "its place is not measured") == 0);
    }
    CHECK(std::strcmp(LearnRefusal(0, true, a, 0), "not a side seat") == 0 && std::strcmp(LearnRefusal(5, true, a, 0), "not a side seat") == 0);
}

// Review (ux 4): standing still, the crouch's constants.
void TestStill() {
    const double half = std::cos(kStillDeg * 0.5 * 3.14159265358979323846 / 180.0);
    unsigned run = 0;
    for (unsigned i = 0; i < kStillRun; ++i) run = StillStep(1.0, run);
    CHECK(run == kStillRun);
    CHECK(StillStep(half + 1e-9, run) == kStillRun + 1);
    CHECK(StillStep(half - 1e-6, run) == 0);  // a joint turned over 1 deg: the run starts again
    CHECK(StillStep(std::nan(""), run) == 0 && StillStep(-1.0, run) == 0);
    CHECK(StillStep(1.0, 1000000u) == 1000000u);  // no wrap
    static_assert(kHangCrouchM > 0.05 && kHangCrouchM < 0.3 && kLegFailRun >= 10, "the crouch");
}

// Review (ux 7 / 8): the glide's facing and its path.
void TestGlideTurn() {
    const double pi = 3.14159265358979323846;
    CHECK(std::fabs(WrapPiD(3 * pi / 2) + pi / 2) < 1e-12 && std::fabs(WrapPiD(-3 * pi / 2) - pi / 2) < 1e-12 && WrapPiD(std::nan("")) == 0);
    CHECK(std::fabs(GlideYaw(1.0, 0.0, 0) - 1.0) < 1e-12 && std::fabs(GlideYaw(1.0, 0.0, 1)) < 1e-12);
    CHECK(std::fabs(GlideYaw(1.0, 0.0, 0.5) - 0.5) < 1e-12);  // Ease(0.5) == 0.5
    // the short way across +-pi: from 170 deg to -170 deg passes 180, not 0
    const double f = 170 * pi / 180, t = -170 * pi / 180, m = GlideYaw(f, t, 0.5);
    CHECK(std::fabs(std::fabs(m) - pi) < 1e-9);
    // the path: from 2.5 m out beside the right front seat, first to the approach point (1 m out), then up onto the anchor
    double an[3];
    CHECK(SideAnchor(1, an));
    const double from[3] = {an[0] + 2.5, an[1] + 0.4, 0.05}, via[3] = {an[0] + 1.0, an[1], 0.05};
    double p[3];
    GlidePath(from, via, an, 0, p);
    CHECK(std::fabs(p[0] - from[0]) < 1e-12 && std::fabs(p[1] - from[1]) < 1e-12 && std::fabs(p[2] - from[2]) < 1e-12);
    GlidePath(from, via, an, 1, p);
    CHECK(std::fabs(p[0] - an[0]) < 1e-12 && std::fabs(p[1] - an[1]) < 1e-12 && std::fabs(p[2] - an[2]) < 1e-12);
    // the split: the first leg's share of the horizontal length; at it Sam is at the approach point, on the ground
    const double d1 = std::sqrt(1.5 * 1.5 + 0.4 * 0.4), d2 = 1.0, s = d1 / (d1 + d2);
    double lo = 0, hi = 1;  // Ease^-1(s) by bisection
    for (int i = 0; i < 60; ++i) (Ease((lo + hi) / 2) < s ? lo : hi) = (lo + hi) / 2;
    GlidePath(from, via, an, lo, p);
    CHECK(std::fabs(p[0] - via[0]) < 1e-6 && std::fabs(p[1] - via[1]) < 1e-6 && std::fabs(p[2] - via[2]) < 1e-6);
    // the first leg stays on the ground (no arc); the second has it
    GlidePath(from, via, an, lo * 0.5, p);
    CHECK(std::fabs(p[2] - 0.05) < 1e-12 && p[0] > via[0]);
    double q[3];
    const double tm = lo + (1 - lo) * 0.5;
    GlidePath(from, via, an, tm, q);
    CHECK(q[0] < via[0] && q[0] > an[0]);
    // close to the seat (within kGlideViaM of the approach point): the straight GlidePos
    const double close_[3] = {via[0] + 0.2, via[1], 0.05};
    double r1[3], r2[3];
    GlidePath(close_, via, an, 0.3, r1);
    GlidePos(close_, an, 0.3, r2);
    CHECK(std::fabs(r1[0] - r2[0]) < 1e-12 && std::fabs(r1[1] - r2[1]) < 1e-12 && std::fabs(r1[2] - r2[2]) < 1e-12);
}
}  // namespace

int main() {
    TestKeys();
    TestSeats();
    TestFrames();
    TestPair();
    TestWatch();
    TestClip();
    TestHang();
    TestGlide();
    TestLoop();
    TestLib();
    TestLearn();
    TestStill();
    TestGlideTurn();
    std::printf("mule_ride_rules: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
