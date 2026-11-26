// Round 27 M1 (docs/enemy-sync.md 15): the pure rules of the L1 probe (enemy_l1_probe.cpp): which player a place, an aim or a
// round is "at", the per-arm trackers (notice, turn, approach, aim, rounds, the switch test) and the decision; run 2 added the
// per-arm geometry rule (enemy_target.cpp's geometry line) and the re-calm's ready rule (enemy_l1 6). No game access:
// unit-tested in tests/enemy_l1_rules_tests.cpp.
#pragma once

#include <cmath>
#include <cstdint>

namespace st::l1rules {

inline constexpr double kBodyHeight = 1.8;    // a player's body: the vertical segment from its root up this far
inline constexpr double kChest = 1.3;         // a player's chest above its root
inline constexpr double kMuleChest = 1.4;     // a MULE's chest (where its rounds start) above its root
inline constexpr double kAttribute = 1.5;     // a place within this of a body is "at" that player
inline constexpr double kTurnDeg = 30.0;      // facing within this of a player is "turned to" it
inline constexpr double kApproach = 2.0;      // the distance to a player dropping by this much is "approached"
inline constexpr double kPi = 3.14159265358979323846;

enum Who : int { kNone = 0, kHer = 1, kSam = 2 };
inline const char* WhoText(int w) noexcept { return w == kHer ? "her" : w == kSam ? "Sam" : "none"; }

// Distance from p to the vertical segment root .. root + h (z up).
inline double SegDist(const double (&p)[3], const double (&root)[3], double h = kBodyHeight) noexcept {
    const double z = p[2] < root[2] ? root[2] : (p[2] > root[2] + h ? root[2] + h : p[2]);
    const double dx = p[0] - root[0], dy = p[1] - root[1], dz = p[2] - z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
inline double Dist(const double (&a)[3], const double (&b)[3]) noexcept {
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
inline double Dist2d(const double (&a)[3], const double (&b)[3]) noexcept {
    const double dx = a[0] - b[0], dy = a[1] - b[1];
    return std::sqrt(dx * dx + dy * dy);
}

// The player whose body a place is at: the nearer one within `cap` (kNone: neither, or a player not known).
inline int Nearest(const double (&p)[3], bool her_ok, const double (&her)[3], bool sam_ok, const double (&sam)[3], double cap = kAttribute) noexcept {
    const double dh = her_ok ? SegDist(p, her) : 1e300, ds = sam_ok ? SegDist(p, sam) : 1e300;
    if (dh > cap && ds > cap) return kNone;
    return dh <= ds ? kHer : kSam;
}

// Closest approach of the ray from `o` along `d` (any length; t >= 0) to the point p. A zero direction: the distance to o.
inline double RayDist(const double (&o)[3], const double (&d)[3], const double (&p)[3]) noexcept {
    const double dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    const double vx = p[0] - o[0], vy = p[1] - o[1], vz = p[2] - o[2];
    double t = dd > 1e-12 ? (vx * d[0] + vy * d[1] + vz * d[2]) / dd : 0.0;
    if (t < 0) t = 0;
    const double ex = vx - t * d[0], ey = vy - t * d[1], ez = vz - t * d[2];
    return std::sqrt(ex * ex + ey * ey + ez * ez);
}

// Angle in degrees between a facing yaw (radians, atan2(y, x)) and the horizontal direction from `from` to `to`.
inline double FacingOff(double yaw, const double (&from)[3], const double (&to)[3]) noexcept {
    const double dx = to[0] - from[0], dy = to[1] - from[1];
    if (dx * dx + dy * dy < 1e-6) return 0.0;
    double d = std::atan2(dy, dx) - yaw;
    while (d > kPi) d -= 2 * kPi;
    while (d < -kPi) d += 2 * kPi;
    return std::fabs(d) * 180.0 / kPi;
}

// A round's line: from the MULE's chest toward the aim point when it has one, else along its facing (level). Its closest approach
// to each chest (1e300: that player not known).
struct RoundAt {
    double her = 1e300, sam = 1e300;
    int who = kNone;  // the player it passed within kAttribute of (the nearer)
};
inline RoundAt RoundLine(const double (&mule)[3], double yaw, bool aim_ok, const double (&aim)[3], bool her_ok, const double (&her)[3], bool sam_ok,
                         const double (&sam)[3]) noexcept {
    const double o[3] = {mule[0], mule[1], mule[2] + kMuleChest};
    double d[3] = {std::cos(yaw), std::sin(yaw), 0.0};
    if (aim_ok && Dist(o, aim) > 0.5) d[0] = aim[0] - o[0], d[1] = aim[1] - o[1], d[2] = aim[2] - o[2];
    RoundAt r;
    if (her_ok) {
        const double c[3] = {her[0], her[1], her[2] + kChest};
        r.her = RayDist(o, d, c);
    }
    if (sam_ok) {
        const double c[3] = {sam[0], sam[1], sam[2] + kChest};
        r.sam = RayDist(o, d, c);
    }
    if (r.her <= kAttribute || r.sam <= kAttribute) r.who = r.her <= r.sam ? kHer : kSam;
    return r;
}
// RoundLine's line itself: its origin (the MULE's chest) and direction (toward the aim point, else along the facing, level).
inline void RoundLineDir(const double (&mule)[3], double yaw, bool aim_ok, const double (&aim)[3], double (&o)[3], double (&d)[3]) noexcept {
    o[0] = mule[0], o[1] = mule[1], o[2] = mule[2] + kMuleChest;
    d[0] = std::cos(yaw), d[1] = std::sin(yaw), d[2] = 0.0;
    if (aim_ok && Dist(o, aim) > 0.5) d[0] = aim[0] - o[0], d[1] = aim[1] - o[1], d[2] = aim[2] - o[2];
}
// Run 2 (round 27): the angle in degrees between a round's direction d (from o) and the line from o to a player's chest (root +
// kChest); -1 when either is degenerate.
inline double AngleToChest(const double (&o)[3], const double (&d)[3], const double (&root)[3]) noexcept {
    const double v[3] = {root[0] - o[0], root[1] - o[1], root[2] + kChest - o[2]};
    const double dd = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), vv = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (dd < 1e-9 || vv < 1e-9) return -1.0;
    double c = (d[0] * v[0] + d[1] * v[1] + d[2] * v[2]) / (dd * vv);
    c = c > 1 ? 1 : (c < -1 ? -1 : c);
    return std::acos(c) * 180.0 / kPi;
}
// The horizontal angle in degrees between a round's direction and a facing yaw (radians, atan2(y, x)); -1 for a vertical round.
inline double AngleToYaw(const double (&d)[3], double yaw) noexcept {
    if (d[0] * d[0] + d[1] * d[1] < 1e-12) return -1.0;
    double a = std::atan2(d[1], d[0]) - yaw;
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return std::fabs(a) * 180.0 / kPi;
}
// A round's real line (review round 27 M1: the bullet the shot hook saw, its origin and direction): its closest approach to each chest.
inline RoundAt RoundRay(const double (&o)[3], const double (&d)[3], bool her_ok, const double (&her)[3], bool sam_ok, const double (&sam)[3]) noexcept {
    RoundAt r;
    if (her_ok) {
        const double c[3] = {her[0], her[1], her[2] + kChest};
        r.her = RayDist(o, d, c);
    }
    if (sam_ok) {
        const double c[3] = {sam[0], sam[1], sam[2] + kChest};
        r.sam = RayDist(o, d, c);
    }
    if (r.her <= kAttribute || r.sam <= kAttribute) r.who = r.her <= r.sam ? kHer : kSam;
    return r;
}

// One 4 Hz sample of the watched MULE (the probe fills it from the game; times in ms since the arm started).
struct Sample {
    double t = 0;
    bool realized = false;
    double mule[3] = {};
    double yaw = 0;
    bool her_ok = false, sam_ok = false;
    double her[3] = {}, sam[3] = {};
    bool pv_her = false, pv_sam = false;  // a PlayerVisual entry whose place is at her / at Sam
    // run 2 (round 27): the age (s) of the youngest PlayerVisual entry at her (meaningful with pv_her); the youngest entry's age of
    // any (-1 none); the seconds since her stimulus was made (-1 unknown or none: then a PV at her at the start is contamination)
    double pv_her_age = 0, pv_age_min = -1, stim_age = -1;
    bool lk = false;                      // any PlayerLastKnown entry
    bool aim_ok = false;                  // the aim goal's point
    double aim[3] = {};
    int daemon = kNone;                   // the PlayerInfo daemon's centre: at whom
    int lk_who = kNone;                   // the PlayerLastKnown entry's place: at whom
    int cp_who = kNone;                   // the CP's alert place: at whom
    unsigned shots = 0;                   // the MULE's rounds so far (cumulative, from the shot counts)
    int phase = 0;                        // the switch test's phase (0 her, then both seen, 1 she hides, 2 Sam far and she is back)
    bool stim = false;                    // her stimulus of ours was live (not released) at this sample
    static constexpr unsigned kRays = 8;
    unsigned ray_n = 0;                   // the real lines of the rounds new in this sample (oldest first; the rest: the sample's line)
    double ray_o[kRays][3] = {}, ray_d[kRays][3] = {};
};

// One arm's summary (times: ms since the arm started, -1 never).
struct Arm {
    int samples = 0, realized = 0;
    double noticed = -1;                       // the first fresh PV at her (age < kNoticeAge; run 2: a stale entry is no notice)
    double turned_her = -1, turned_sam = -1;   // the first facing within kTurnDeg of her (at or after the notice) / of Sam (any time)
    double start_her = -1, start_sam = -1;     // the distances at the first realized sample
    double min_her = 1e300, min_sam = 1e300;
    bool approached_her = false, approached_sam = false;
    int aim_n = 0, aim_her = 0, aim_sam = 0;   // aim samples after the notice, and at whom
    int pv_her = 0, pv_sam = 0, lk = 0;        // samples with a PV at her / at Sam / any LK
    int daemon_her = 0, daemon_sam = 0;        // samples after the notice with the daemon's centre at her / at Sam
    int lk_her = 0, lk_sam = 0, cp_her = 0, cp_sam = 0;  // samples with the LK entry / the CP alert place at her / at Sam
    double first_off_her = -1, first_off_sam = -1, last_off_her = -1, last_off_sam = -1;  // the facing's angle to each (deg)
    unsigned shots0 = 0, fired = 0, at_her = 0, at_sam = 0;
    unsigned fired_ph[3] = {}, at_her_ph[3] = {}, at_sam_ph[3] = {};  // run 2: the same per switch phase (the phase at the sample)
    double pv_age0 = -1, stim_age0 = -1;       // the youngest PV entry's age / her stimulus' age at the first realized sample (s; -1 none)
    bool have_shots0 = false;
    int target = kNone;                        // the newest sample's target (aim, else the unique PV)
    // the switch test: ms from phase 1 to a target at Sam / from phase 2 to her; -1 never; kNa (-2) not a switch: the phase before
    // did not end on the other player (review round 27 M1: a MULE already on Sam when she hides has not switched)
    double switch_sam = -1, switch_her = -1;
    double phase_at[3] = {0, -1, -1};
    int phase = 0;
    int last_target[3] = {kNone, kNone, kNone};  // the newest target seen in each phase (phase 0's: who wins with both seen)
    // validity (review round 27 M1): a PlayerVisual entry at her in the first realized sample older than her stimulus (run 2:
    // contamination from the arm before; one the arm's own fresh stimulus made is not);
    // the re-calm before the arm timed out (set by the probe); her stimulus lost during the arm; the goal reads off (set by the probe)
    bool pv_at_start = false, calm_timeout = false, stim_lost = false, no_goal_reads = false;
    int stim_n = 0;                            // samples with her stimulus live
    unsigned rays = 0;                         // rounds attributed on their real line (the rest on the sample's line)
};
inline constexpr double kNa = -2;
inline constexpr double kNoticeAge = 0.5;  // s: a PlayerVisual entry at her this young is a notice (run 2)

// The newest sample's target: the aim's player, else whichever PV is unique.
inline int SampleTarget(const Sample& s) noexcept {
    if (s.aim_ok) {
        const int w = Nearest(s.aim, s.her_ok, s.her, s.sam_ok, s.sam);
        if (w != kNone) return w;
    }
    if (s.pv_her != s.pv_sam) return s.pv_her ? kHer : kSam;
    return kNone;
}

// Adds one sample; rounds (a rise of s.shots) are attributed on the sample's line. Returns the rounds new in this sample.
inline unsigned Add(Arm& a, const Sample& s, RoundAt* last_round = nullptr) noexcept {
    ++a.samples;
    if (s.stim) ++a.stim_n;
    else if (a.stim_n > 0) a.stim_lost = true;
    if (s.phase != a.phase && s.phase >= 0 && s.phase < 3) {
        a.phase = s.phase;
        if (a.phase_at[s.phase] < 0) {
            a.phase_at[s.phase] = s.t;
            // a switch only from the other player: phase 0 must have ended on her, phase 1 on Sam
            if (s.phase == 1 && a.last_target[0] != kHer) a.switch_sam = kNa;
            if (s.phase == 2 && a.last_target[1] != kSam) a.switch_her = kNa;
        }
    }
    if (!a.have_shots0) a.shots0 = s.shots, a.have_shots0 = true;
    if (!s.realized) return 0;
    if (++a.realized == 1) {
        a.pv_age0 = s.pv_age_min, a.stim_age0 = s.stim_age;
        // run 2: a PV at her older than her stimulus (any age when the stimulus' age is unknown) came from before the arm
        if (s.pv_her && s.pv_her_age > s.stim_age) a.pv_at_start = true;
    }
    if (s.pv_her) ++a.pv_her;
    if (s.pv_sam) ++a.pv_sam;
    if (s.lk) ++a.lk;
    if (s.lk_who == kHer) ++a.lk_her;
    if (s.lk_who == kSam) ++a.lk_sam;
    if (s.cp_who == kHer) ++a.cp_her;
    if (s.cp_who == kSam) ++a.cp_sam;
    if (s.pv_her && s.pv_her_age < kNoticeAge && a.noticed < 0) a.noticed = s.t;
    if (s.her_ok) {
        const double d = Dist2d(s.mule, s.her);
        if (a.start_her < 0) a.start_her = d;
        if (d < a.min_her) a.min_her = d;
        if (a.start_her - d >= kApproach) a.approached_her = true;
        const double off = FacingOff(s.yaw, s.mule, s.her);
        if (a.first_off_her < 0) a.first_off_her = off;
        a.last_off_her = off;
        if (a.turned_her < 0 && a.noticed >= 0 && off <= kTurnDeg) a.turned_her = s.t;
    }
    if (s.sam_ok) {
        const double d = Dist2d(s.mule, s.sam);
        if (a.start_sam < 0) a.start_sam = d;
        if (d < a.min_sam) a.min_sam = d;
        if (a.start_sam - d >= kApproach) a.approached_sam = true;
        const double off = FacingOff(s.yaw, s.mule, s.sam);
        if (a.first_off_sam < 0) a.first_off_sam = off;
        a.last_off_sam = off;
        if (a.turned_sam < 0 && off <= kTurnDeg) a.turned_sam = s.t;
    }
    if (a.noticed >= 0) {
        if (s.aim_ok) {
            ++a.aim_n;
            const int w = Nearest(s.aim, s.her_ok, s.her, s.sam_ok, s.sam);
            if (w == kHer) ++a.aim_her;
            if (w == kSam) ++a.aim_sam;
        }
        if (s.daemon == kHer) ++a.daemon_her;
        if (s.daemon == kSam) ++a.daemon_sam;
    }
    a.target = SampleTarget(s);
    if (a.target != kNone && a.phase >= 0 && a.phase < 3) a.last_target[a.phase] = a.target;
    if (a.phase == 1 && a.switch_sam == -1 && a.target == kSam && a.phase_at[1] >= 0) a.switch_sam = s.t - a.phase_at[1];
    if (a.phase == 2 && a.switch_her == -1 && a.target == kHer && a.phase_at[2] >= 0) a.switch_her = s.t - a.phase_at[2];
    unsigned fresh = 0;
    const unsigned seen = a.shots0 + a.fired;
    if (s.shots > seen) {
        fresh = s.shots - seen;
        const RoundAt line = RoundLine(s.mule, s.yaw, s.aim_ok, s.aim, s.her_ok, s.her, s.sam_ok, s.sam);
        for (unsigned i = 0; i < fresh; ++i) {
            const bool ray = i < s.ray_n && i < Sample::kRays;
            const RoundAt r = ray ? RoundRay(s.ray_o[i], s.ray_d[i], s.her_ok, s.her, s.sam_ok, s.sam) : line;
            if (ray) ++a.rays;
            if (r.who == kHer) ++a.at_her;
            if (r.who == kSam) ++a.at_sam;
            if (a.phase >= 0 && a.phase < 3) {
                if (r.who == kHer) ++a.at_her_ph[a.phase];
                if (r.who == kSam) ++a.at_sam_ph[a.phase];
            }
            if (last_round) *last_round = r;
        }
        a.fired += fresh;
        if (a.phase >= 0 && a.phase < 3) a.fired_ph[a.phase] += fresh;
    }
    return fresh;
}

// Whom the MULE turned to during the arm (G, her hit's credit): the player it faced within kTurnDeg at the end but not at the start
// (both: the nearer angle); kNone: no such turn.
inline int TurnedDuring(const Arm& a) noexcept {
    const bool her = a.first_off_her > kTurnDeg && a.last_off_her >= 0 && a.last_off_her <= kTurnDeg;
    const bool sam = a.first_off_sam > kTurnDeg && a.last_off_sam >= 0 && a.last_off_sam <= kTurnDeg;
    if (her && sam) return a.last_off_her <= a.last_off_sam ? kHer : kSam;
    return her ? kHer : sam ? kSam : kNone;
}

// The decision (docs/enemy-sync.md 15). B: L1 on the gun MULE (her 25 m in front, Sam far); C: L1 on the melee MULE (her 6 m in
// front, Sam far); D: the switch test on the gun MULE.
enum Decision : int { kFail = 0, kPNotice = 1, kPNoFire = 2, kPAim = 3, kPFull = 4, kUndecided = 5 };
inline const char* DecisionText(int d) noexcept {
    switch (d) {
        case kUndecided: return "undecided";
        case kPFull: return "P-full";
        case kPAim: return "P-aim";
        case kPNoFire: return "P-nofire";
        case kPNotice: return "P-notice";
        default: return "FAIL";
    }
}
inline bool AimsAtHer(const Arm& b) noexcept { return b.aim_n > 0 && b.aim_her * 10 >= b.aim_n * 7; }  // >= 70 %
inline bool SamPull(const Arm& a) noexcept {
    return a.turned_sam >= 0 || a.approached_sam || (a.daemon_sam > a.daemon_her && a.daemon_sam > 0);
}
inline bool CFull(const Arm& c) noexcept {
    return c.noticed >= 0 && c.noticed <= 4000 && c.turned_her >= 0 && c.turned_her - c.noticed <= 4000 && c.approached_her;
}
inline bool BFull(const Arm& b) noexcept { return b.fired >= 1 && b.at_her * 10 >= b.fired * 6; }  // >= 60 % of rounds at her
inline bool DFull(const Arm& d) noexcept {
    return d.switch_sam >= 0 && d.switch_sam <= 3000 && d.switch_her >= 0 && d.switch_her <= 3000;
}
// An arm whose numbers do not mean what they say: it started with a PlayerVisual entry at her, its re-calm timed out, or her
// stimulus went away during it.
inline bool Invalid(const Arm& a) noexcept { return a.pv_at_start || a.calm_timeout || a.stim_lost; }
inline int Decide(const Arm& b, const Arm& c, const Arm& d) noexcept {
    if (Invalid(b) || Invalid(c)) return kUndecided;
    if (b.noticed < 0 && c.noticed < 0) return kFail;  // PV never at her place in B and C
    // no aim data: the goal reads off, or the gun MULE never noticed her (its aim after a notice is what P-notice judges)
    if (b.no_goal_reads || b.noticed < 0) return kUndecided;
    if (!AimsAtHer(b)) return kPNotice;  // (noticed, the goal reads on, and no aim at her: aim_n 0 is "never aimed")
    if (CFull(c) && BFull(b) && DFull(d) && !Invalid(d) && !d.no_goal_reads && !SamPull(b) && !SamPull(c)) return kPFull;
    if (b.fired == 0) return kPNoFire;
    return kPAim;
}

// ---- run 2 (round 27): an arm's geometry, per arm (enemy_target.cpp's Geometry; the target gate's arm codes 1..9)
inline constexpr double kEye = 1.6;  // a MULE's eye above its root (the cone's origin)
// The cone's pitch (deg) from the MULE's eye to a point `aim_h` above a player whose root is `dz` above the MULE's; 90 when the
// player stands on the MULE (horiz ~0).
inline double GeomPitch(double dz, double aim_h, double horiz) noexcept {
    return horiz > 0.01 ? std::atan2(std::fabs(dz + aim_h - kEye), horiz) * 180.0 / kPi : 90.0;
}
// Melee arms: CH 7, CS 8, CL 9 (on the melee MULE); the rest (and 0, no arm) are gun arms. CS judges our Sam, the others her.
inline bool MeleeArm(int code) noexcept { return code >= 7; }
inline bool SamArm(int code) noexcept { return code == 8; }
// melee: |dz| <= 1, 2..8 m, bearing <= 45 deg; gun: 10..30 m, bearing <= 30 deg, pitch <= 15 deg. A bearing < 0 (unknown) or a
// subject not read is never valid.
inline bool GeomValid(int code, bool ok, double dz, double horiz, double bearing, double pitch) noexcept {
    if (!ok || bearing < 0) return false;
    if (MeleeArm(code)) return std::fabs(dz) <= 1.0 && horiz >= 2.0 && horiz <= 8.0 && bearing <= 45.0;
    return horiz >= 10.0 && horiz <= 30.0 && bearing <= 30.0 && pitch <= 15.0;
}
// Run 3: the horizontal angle (deg, 0..180) at the MULE between her and our Sam; -1 when either is unread or stands on the MULE.
inline double SepDeg(const double (&m)[3], bool h_ok, const double (&h)[3], bool s_ok, const double (&s)[3]) noexcept {
    if (!h_ok || !s_ok) return -1.0;
    const double ax = h[0] - m[0], ay = h[1] - m[1], bx = s[0] - m[0], by = s[1] - m[1];
    const double la = std::sqrt(ax * ax + ay * ay), lb = std::sqrt(bx * bx + by * by);
    if (la < 1e-3 || lb < 1e-3) return -1.0;
    double c = (ax * bx + ay * by) / (la * lb);
    c = c > 1 ? 1 : (c < -1 ? -1 : c);
    return std::acos(c) * 180.0 / kPi;
}

// ---- run 3 (round 27): op 11, "arm at geometry". The arm starts once her geometry is valid for its rule and her horizontal
// distance is in the arm's window around its nominal (the scenario's placement), or after kArmGeoLimitMs anyway.
inline constexpr double kArmGeoLimitMs = 2500;
// The nominal horizontal distance (m) of her placement per arm code: TA 25, TF0 / TF1 22, TE / TX 16, CH 5, CL 6; TB (2) and CS (8)
// have no window (-1): TB is mid-engagement, CS judges our Sam.
inline double ArmNominal(int code) noexcept {
    switch (code) {
        case 1: return 25;
        case 3:
        case 4: return 22;
        case 5:
        case 6: return 16;
        case 7: return 5;
        case 9: return 6;
        default: return -1;
    }
}
// Her geometry valid (l1rules::GeomValid by the arm's rule) and within nominal -4 .. +2 m (no window: the geometry alone).
inline bool ArmHerOk(int code, bool geom_valid, double horiz) noexcept {
    const double n = ArmNominal(code);
    return geom_valid && (n < 0 || (horiz >= n - 4.0 && horiz <= n + 2.0));
}
enum class ArmGo : std::uint8_t { wait, go_valid, go_timeout };
inline ArmGo ArmStep(double elapsed_ms, bool her_ok) noexcept {
    if (her_ok) return ArmGo::go_valid;
    return elapsed_ms >= kArmGeoLimitMs ? ArmGo::go_timeout : ArmGo::wait;
}

// ---- run 2 (round 27): the re-calm (enemy_l1 6). One 500 ms reading of the target MULE, its camp and its knowledge.
inline constexpr double kCalmHoldMs = 1500;   // quiet this long: ready
inline constexpr double kCalmPvAge = 3.0;     // s: a PlayerVisual entry at least this old ...
inline constexpr double kCalmPvDist = 3.0;    // m: ... and farther than this from both players is stale, not a sighting
inline constexpr double kCalmApplyMs = 500;   // the re-apply period while something is alerted
struct CalmIn {
    int phase = -1;              // the target's alert phase (-1 unread)
    unsigned members = 0;        // the other camp members read
    unsigned members_hot = 0;    // of those, at phase 3..5
    unsigned members_up = 0;     // of those, above 2 (3..5 and anything unknown above)
    int cp_phase = -1;           // the CP's phase; -1 no CP known (a loose MULE), -2 a CP whose phase did not read
    int pv_n = 0;                // PlayerVisual entries
    double pv_min_age = 1e300;   // the youngest's age (s)
    double pv_min_dist = 1e300;  // the nearest entry's distance to either player (m; 1e300 neither known)
};
// Run 3: the camp's CP rests at PreCaution (2) with nothing alerted (run 20261126-154530: cp 2 half a second into every calm, and
// every calm timed out on "cp > 0"); erules::CampAlerted is >= 3. The CP at 0..2 is not alerted.
inline constexpr int kCpBaseMax = 2;
// Something to re-apply the calm to: the target or a member at phase >= 3, or the CP alerted (3..) or unread.
inline bool CalmHot(const CalmIn& c) noexcept { return c.phase >= 3 || c.members_up > 0 || c.cp_phase > kCpBaseMax || c.cp_phase == -2; }
// Every PlayerVisual entry stale: old and away from both players (none: true).
inline bool CalmPvStale(const CalmIn& c) noexcept { return c.pv_n <= 0 || (c.pv_min_age >= kCalmPvAge && c.pv_min_dist > kCalmPvDist); }
// Quiet: the target 0..2, no member above 2, the CP at 0..2 (or no CP known), every PV entry stale.
inline bool CalmQuiet(const CalmIn& c) noexcept {
    return c.phase >= 0 && c.phase <= 2 && c.members_up == 0 && (c.cp_phase == -1 || (c.cp_phase >= 0 && c.cp_phase <= kCpBaseMax)) && CalmPvStale(c);
}
// The calm forces the CP only when it reads alerted (3..5): fewer game writes (run 3).
inline bool CalmCpForce(int cp_phase) noexcept { return cp_phase >= 3 && cp_phase <= 5; }
// Ready once quiet for kCalmHoldMs without a break (now: ms).
struct CalmHold {
    double since = -1;
};
inline bool CalmStep(CalmHold& h, bool quiet, double now) noexcept {
    if (!quiet) return h.since = -1, false;
    if (h.since < 0) h.since = now;
    return now - h.since >= kCalmHoldMs;
}
// The re-apply cap for a window of `secs`: about 20 per 12 s, at least 3, at most 40.
inline int CalmMaxApplies(double secs) noexcept {
    const int n = static_cast<int>(std::ceil(secs * 20.0 / 12.0));
    return n < 3 ? 3 : n > 40 ? 40 : n;
}

}  // namespace st::l1rules
