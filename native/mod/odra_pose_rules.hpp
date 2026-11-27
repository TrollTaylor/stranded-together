// Round 29 O2 (docs/real-partner-body.md section 12.10): the partner's Odradek animates on the other screen (it opens, flaps,
// points and folds), the pure part. No game memory, no Windows: unit-tested in tests/odra_pose_tests.cpp. The game side is
// partner_gear.cpp (OdraPoseRead, TakeSendPose, SetPartnerPose, OdraApplyPoses, PublishNow).
//
// The sender reads the local skinned pose of its Odradek's arm (odra_arms_def, 14 joints, one chain: the parent of j is j - 1)
// and body (odra_main_def, 44 joints) while the two parts go out as STC1 odradek items (round 28 O1), and sends each as an STK1
// gear pose with the flag wire::kGearPoseOdra, only to a partner whose STC1 frames carry wire::kGearOdraPoseCap. The receiver
// writes them onto its cinematic props through two pose targets (the props share the player parts' Skeleton objects: the
// joints copy 1:1, static-Q5Q6), interpolated between the two newest frames in the props' own model updates, and places the
// body prop on the arm prop's tip: arm * FK(arm pose)[13] * HLP_Oderadek (the game parents the body to the arm's joint 13 through
// that helper: 0x142977420 -> 0x14235E990(body, arm, 0)).
//
// The joint convention (the same as the game's model matrices, measured in runs):
// - A model matrix is 4 rows: the joint's x, y, z axes in model space, then its position (real_body.cpp JointFrame; the head
//   lift's check at real_body.cpp:1155-1157: the child sits at parent + t . the parent's rows).
// - A local joint is {q x, y, z, w | t x, y, z | scale} (real_body.cpp:124 struct Joint, 0x30 bytes); look::ReadPose keeps the
//   first 7 floats (gear_look.cpp ReadPose: q = p[0..3], t = p[4..6]).
// - The child's rows are L * P with L = R(q)^T, R the column-vector matrix of q (convention 0 of ride_pose_math.hpp:5-8 and
//   :98; measured once per session by the seated-arms gate, real_body.cpp:795-806: error 1e-7, the other 1.7;
//   docs/real-partner-body.md "the convention gate").
// The joints' scales are taken as 1 (ReadPose does not carry them); the sender's FK check (below) proves it on the real arm.
#pragma once
#include <cmath>
#include <cstdint>

#include "gear_odra.hpp"
#include "ride_pose_math.hpp"

namespace st::odrapose {

// ---- the two skeletons (static-Q5Q6: the player's parts and the props share them)
inline constexpr unsigned kArmJoints = 14, kBodyJoints = 44;
inline constexpr std::uint32_t kArmLayout = 0xA7074286u;   // odra_arms_def
inline constexpr std::uint32_t kBodyLayout = 0x2A97D3D4u;  // odra_main_def
inline constexpr unsigned kArmTip = 13;                     // JNT_C_B_013_joint, where HLP_Oderadek is
// HLP_Oderadek (the arm's skeleton helpers, index 13; decoded offline): identity rotation, this offset in joint 13's frame (m).
inline constexpr double kHelperT[3] = {0.0, 0.011729, 0.011248};

// The skeleton a part's pose must be on (layout hash and joint count).
inline bool SkeletonOk(int part, std::uint32_t layout, unsigned n) noexcept {
    if (part == gearodra::kPartArm) return layout == kArmLayout && n == kArmJoints;
    if (part == gearodra::kPartBody) return layout == kBodyLayout && n == kBodyJoints;
    return false;
}
// The part a skeleton names (-1 neither).
inline int PartOfSkeleton(std::uint32_t layout, unsigned n) noexcept {
    return SkeletonOk(gearodra::kPartArm, layout, n) ? gearodra::kPartArm : SkeletonOk(gearodra::kPartBody, layout, n) ? gearodra::kPartBody : -1;
}

// ---- the sender
inline constexpr double kReadMs = 30;        // the local poses read every 30 ms on the game thread while sent (33 Hz, >= 15 Hz)
inline constexpr double kMinSendMs = 66;     // a changed pose goes out at most every 66 ms per part (15 Hz)
inline constexpr double kKeepaliveMs = 1000; // and every second unchanged
inline constexpr float kChangeM = 0.001f, kChangeDeg = 0.5f;  // "changed": a joint moved 1 mm or turned 0.5 deg

struct SendIn {
    bool gear_on = false;      // odradek_gear (and the O1 byte checks)
    bool pose_on = false;      // odradek_pose
    bool parts_sent = false;   // ReadOdradek's hold says the two parts go out as items now
    bool partner_cap = false;  // the partner's newest STC1 frame carries kGearOdraPoseCap (or our own gear on the debug body)
    bool item = false;         // this part is an item of our current frame (its entity known)
};
enum class SendWhy : std::uint8_t { send, gear_off, pose_off, not_sent, no_cap, no_item };
inline constexpr unsigned kSendWhyCount = 6;
inline SendWhy ShouldSendOdraPose(const SendIn& in) noexcept {
    if (!in.gear_on) return SendWhy::gear_off;
    if (!in.pose_on) return SendWhy::pose_off;
    if (!in.parts_sent) return SendWhy::not_sent;
    if (!in.partner_cap) return SendWhy::no_cap;
    if (!in.item) return SendWhy::no_item;
    return SendWhy::send;
}
inline const char* SendWhyName(SendWhy w) noexcept {
    switch (w) {
        case SendWhy::send: return "sent";
        case SendWhy::gear_off: return "odradek_gear off";
        case SendWhy::pose_off: return "odradek_pose off";
        case SendWhy::not_sent: return "our Odradek is not sent";
        case SendWhy::no_cap: return "the partner's frames lack the pose flag (odradek_pose=0 there or an older build)";
        case SendWhy::no_item: return "the part is not in our current frame";
    }
    return "?";
}
// A stream's next send: never sent, the keepalive, or changed and the minimum interval passed.
inline bool Due(bool have_sent, bool changed, double since_ms, double min_ms = kMinSendMs) noexcept {
    return !have_sent || since_ms >= kKeepaliveMs || (changed && since_ms >= min_ms);
}

// ---- the receiver's routing of an STK1 pose with kGearPoseOdra
struct RouteIn {
    bool layout_match = false;  // the pose's layout is the partner's newest gear frame's
    bool item_in_frame = false; // and its item index is one of that frame's items
    bool kind_odradek = false;  // that item is a GearKind::odradek
    std::uint8_t model = 0xFF;  // its model (the part)
    std::uint32_t skeleton = 0;
    unsigned n = 0;
};
// The part (gearodra::kPartBody / kPartArm), or -1 (dropped, counted). With the frame: the item's part, whose skeleton the
// pose must be on. A pose ahead of its layout's gear frame (STC1 goes once per change): its skeleton names the part.
inline int Route(const RouteIn& in) noexcept {
    if (in.layout_match && in.item_in_frame) {
        if (!in.kind_odradek) return -1;
        const int part = gearodra::PartOf(in.model);
        return part >= 0 && SkeletonOk(part, in.skeleton, in.n) ? part : -1;
    }
    return PartOfSkeleton(in.skeleton, in.n);
}

// ---- the receiver's playback (keys: the receive times, ms)
inline constexpr double kPlayDelayMs = 100;   // the props play the frames this far behind their arrival
inline constexpr double kMaxSegmentMs = 100;  // a segment after a pause (the 1 s keepalive) starts this long before its end
inline constexpr double kMaxNextMs = 250;     // the frame after b is passed only this close after it (one motion)
inline constexpr double kStaleMs = 1500;      // no frame for this long: the last pose held (the prop stays posed; logged)

// A part's newest frames, oldest first: the bracket for the play time. a = the newest at or before it (else the oldest, held),
// b = the next one (interpolated a -> b; key_a moved up to kMaxSegmentMs before key_b after a pause), c = the one after b
// when it follows within kMaxNextMs (the update may run past key_b).
struct Bracket {
    int a = -1, b = -1, c = -1;
    double key_a = 0, key_b = 0, key_c = 0;
};
inline Bracket Pick(const double* keys, unsigned n, double play) noexcept {
    Bracket br;
    if (!keys || n == 0) return br;
    int i = -1;
    for (unsigned k = 0; k < n; ++k)
        if (keys[k] <= play) i = static_cast<int>(k);
    if (i < 0) {
        br.a = 0;
        br.key_a = keys[0];
        return br;
    }
    br.a = i;
    br.key_a = keys[i];
    if (static_cast<unsigned>(i) + 1 < n && keys[i + 1] > keys[i]) {
        br.b = i + 1;
        br.key_b = keys[i + 1];
        if (br.key_b - br.key_a > kMaxSegmentMs) br.key_a = br.key_b - kMaxSegmentMs;
        if (static_cast<unsigned>(i) + 2 < n && keys[i + 2] > keys[i + 1] && keys[i + 2] - keys[i + 1] <= kMaxNextMs) {
            br.c = i + 2;
            br.key_c = keys[i + 2];
        }
    }
    return br;
}
// The share of the way from key_a to key_b at `play` (0..1).
inline float SegU(double play, double key_a, double key_b) noexcept {
    if (!(key_b > key_a)) return play >= key_b ? 1.0f : 0.0f;
    const double u = (play - key_a) / (key_b - key_a);
    return u <= 0 ? 0.0f : u >= 1 ? 1.0f : static_cast<float>(u);
}
// The same blend as the props' own update (real_body.cpp ApplyGearPose): the shorter way, linear, normalised; t linear.
inline void Nlerp(const float (&a)[4], const float (&b)[4], float u, float (&out)[4]) noexcept {
    float bb[4] = {b[0], b[1], b[2], b[3]};
    if (a[0] * bb[0] + a[1] * bb[1] + a[2] * bb[2] + a[3] * bb[3] < 0)
        for (float& c : bb) c = -c;
    for (int c = 0; c < 4; ++c) out[c] = a[c] + u * (bb[c] - a[c]);
    const float n = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
    if (n > 1e-6f) {
        for (float& c : out) c /= n;
    } else {
        for (int c = 0; c < 4; ++c) out[c] = a[c];
    }
}
inline bool Stale(double now_ms, double last_rx_ms) noexcept { return last_rx_ms > 0 && now_ms - last_rx_ms > kStaleMs; }

// ---- forward kinematics (the convention above)
struct Frame {
    ridepose::V p;   // position (model space, m)
    ridepose::M3 r;  // rows: the joint's axes
};
inline constexpr int kConv = 0;  // the rows are R(q)^T (measured)
// The model-space frame of joint `tip` of a single chain (the parent of j is j - 1; joint 0's parent the model origin) from its
// local joints: q[j] = {x, y, z, w}, t[j] in m. False on bad input (tip >= n, a zero quaternion, not finite).
inline bool ChainFrame(const float (*q)[4], const float (*t)[3], unsigned n, unsigned tip, int conv, Frame* out) noexcept {
    if (!q || !t || !out || tip >= n) return false;
    Frame f;  // the model origin: identity
    for (unsigned j = 0; j <= tip; ++j) {
        const double qd[4] = {q[j][0], q[j][1], q[j][2], q[j][3]};
        const double qn = qd[0] * qd[0] + qd[1] * qd[1] + qd[2] * qd[2] + qd[3] * qd[3];
        if (!(qn > 1e-12) || !std::isfinite(qn)) return false;
        const ridepose::V tj{t[j][0], t[j][1], t[j][2]};
        if (!ridepose::Finite(tj)) return false;
        Frame c;
        c.p = f.p + ridepose::Row(f.r, 0) * tj.x + ridepose::Row(f.r, 1) * tj.y + ridepose::Row(f.r, 2) * tj.z;
        c.r = ridepose::Mul(ridepose::LocalRows(qd, conv), f.r);
        f = c;
    }
    *out = f;
    return ridepose::Finite(f.p);
}
// The body's place in the arm entity's frame: joint 13's frame (rows normalised) times HLP_Oderadek (identity rotation, kHelperT).
// p in m, r rows. False when the frame is degenerate.
inline bool BodyInArm(const Frame& tip, double (&p)[3], float (&r)[3][3]) noexcept {
    ridepose::V ax[3];
    for (int k = 0; k < 3; ++k) {
        ax[k] = ridepose::Row(tip.r, k);
        const double l = ridepose::Len(ax[k]);
        if (!(l > 0.2 && l < 5.0)) return false;
        ax[k] = ax[k] * (1.0 / l);
    }
    const ridepose::V bp = tip.p + ax[0] * kHelperT[0] + ax[1] * kHelperT[1] + ax[2] * kHelperT[2];
    if (!ridepose::Finite(bp)) return false;
    p[0] = bp.x, p[1] = bp.y, p[2] = bp.z;
    for (int k = 0; k < 3; ++k) r[k][0] = static_cast<float>(ax[k].x), r[k][1] = static_cast<float>(ax[k].y), r[k][2] = static_cast<float>(ax[k].z);
    return true;
}
// Rows (float) <-> Frame.
inline Frame FromModelMatrix(const float (&m)[4][4]) noexcept {
    Frame f;
    for (int k = 0; k < 3; ++k) ridepose::SetRow(f.r, k, ridepose::V{m[k][0], m[k][1], m[k][2]});
    f.p = ridepose::V{m[3][0], m[3][1], m[3][2]};
    return f;
}
// Two frames apart: distance (cm) and the angle between their normalised rows (deg; 180 when degenerate).
inline void FrameDelta(const Frame& a, const Frame& b, double* cm, double* deg) noexcept {
    const ridepose::V d = a.p - b.p;
    if (cm) *cm = ridepose::Len(d) * 100.0;
    float ra[3][3], rb[3][3];
    bool ok = true;
    for (int k = 0; k < 3; ++k) {
        const ridepose::V x = ridepose::Row(a.r, k), y = ridepose::Row(b.r, k);
        const double lx = ridepose::Len(x), ly = ridepose::Len(y);
        if (!(lx > 1e-6) || !(ly > 1e-6)) ok = false;
        for (int c = 0; c < 3 && ok; ++c) {
            const double vx = c == 0 ? x.x : c == 1 ? x.y : x.z, vy = c == 0 ? y.x : c == 1 ? y.y : y.z;
            ra[k][c] = static_cast<float>(vx / lx), rb[k][c] = static_cast<float>(vy / ly);
        }
    }
    if (deg) *deg = ok ? gearodra::AngleDeg(ra, rb) : 180.0;
}

// ---- the sender's FK check: does the body sit where arm * FK(pose)[13] * HLP_Oderadek says? It decides the receiver's body
// placement (the pose frames carry wire::kGearPoseOdraFk while it holds). The bit goes on with the first pass. A failed check
// is tried again after kFkRetryMs; one failure alone does not take the bit away (a check taken while the arm swings may see the
// body a frame behind it), kFkFailsOff failures in a row turn it off for the session (a wrong convention or another attach:
// the receiver keeps the STC1 item's own body transform, O1). A pass needs the position and the turn: HLP_Oderadek's offset is
// only 1.6 cm, so a wrong attach rotation moves the predicted body less than kFkOkCm up to ~135 deg and shows only in the turn.
inline constexpr double kFkOkCm = 3.0;
inline constexpr double kFkOkDeg = 10.0;
inline constexpr double kFkEveryMs = 30000, kFkRetryMs = 2000;
inline constexpr int kFkFailsOff = 3;
struct FkState {
    bool checked = false, ok = false, off = false;
    int fails = 0;
    unsigned checks = 0, passed = 0;
};
inline bool FkPass(double body_cm, double body_deg) noexcept {
    return std::isfinite(body_cm) && std::isfinite(body_deg) && body_cm >= 0 && body_deg >= 0 && body_cm <= kFkOkCm && body_deg <= kFkOkDeg;
}
// One check's result; returns the delay to the next (ms).
inline double FkStep(FkState& s, double body_cm, double body_deg) noexcept {
    s.checked = true;
    ++s.checks;
    const bool pass = FkPass(body_cm, body_deg);
    if (pass) {
        ++s.passed;
        s.fails = 0;
        s.ok = !s.off;
        return kFkEveryMs;
    }
    if (++s.fails >= kFkFailsOff) s.off = true;
    if (s.off) s.ok = false;
    return s.off ? kFkEveryMs : kFkRetryMs;
}

}  // namespace st::odrapose
