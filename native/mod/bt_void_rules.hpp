// Round 31 stage 6 (bt_void): this feature's own pure logic (no game, no Windows), tested by tests/bt_void_tests.cpp. The
// shared codecs, caps and the stage-1 gates stay in bt_rules.hpp (included: VOID2, BlastGate, FlashFade, CraterApplyGate,
// CraterRadius, CraterFlags); the helpers here are the module's glue around them, so the seven builders never edit one file at
// once.
//   VB: the partner's voidout blast at its place, with the voidout flash faded by distance (FadeStart bt_void_flash_m, FadeRange 450
//       written into the effector resource while a 9 s window is open; PF 14). Cosmetic, on every save; a real-save guest only from
//       300 m (BlastGate) until the gate's near leg passes.
//   CR: the crater record a catcher voidout (FUN_142A003D0) adds on the sender, added here through the catcher manager's pending
//       list (PF 15) on the host or a sandboxed guest (CraterApplyGate). It persists in the save it lands on (PF 16). A gazer voidout
//       makes none (PF 17): its VOID2 never asks for one, and the gate's gazer row is refused for good here (CraterGate pins it).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "bt_rules.hpp"

namespace st::btrules::voidout {

constexpr int kStage = 6;

// ---------------------------------------------------------------- the wire's source byte
// VOID2's source is VoidSource (bt_rules.hpp) for the voidouts FUN_14299B1B0 makes (blast only). The crater a catcher's
// annihilation adds comes from its own path, FUN_142A003D0 (the sender's pass-through), so its VOID2 names source 5 (VOID2 only:
// VOID v1 never carries it). When the same catch also runs FUN_14299B1B0, the receiver plays one blast (the second VOID2 is a
// repeat) and still gates the crater.
constexpr std::uint8_t kSrcCatcher = 5;
inline const char* SourceName(std::uint8_t s) { return s == kSrcCatcher ? "catcher annihilation" : VoidSourceName(s); }

// ---------------------------------------------------------------- CR dropped (the round 31 gate)
// The first gate batch (run 20261128-012033-two-games-bt-void-crater): the sandboxed guest's crater record went live (+8.1 s) and
// 1.2 s later her game sent a game-server write, urn:otgc_set_mission_progress (its only one in 213 hidden runs; the probe's
// craters in the open sent none). A crater is world state the game reacts to with its own server requests, so CR stays off: every
// crater is refused here (after the gate's own rows, so a real-save guest still logs its own reason). One constant turns it back
// on once a run shows a crater with 0 game-server writes.
constexpr bool kCraterDropped = true;
constexpr const char* kCraterDroppedWhy =
    "CR dropped this round (a crater record added here was followed by a game-server write, urn:otgc_set_mission_progress)";
inline bool CraterDropped() { return kCraterDropped; }  // (a call: no constant-condition warning where it is tested)

// ---------------------------------------------------------------- caps and the send / apply levels
// kCapVoid2: this game understands VOID2 (bt_void >= 1 with the relay on). kCapWorldApply: craters may be applied here (bt_void 2,
// bt_sync 2, the host or a sandboxed guest: never a real-save guest). bt_extra may OR kCapWorldApply in too (RESPAWN).
inline std::uint16_t Caps(int bt_void, int sync_level, int role, bool real_save) {
    if (bt_void < 1 || sync_level < 1 || (role != 1 && role != 2)) return 0;
    std::uint16_t c = kCapVoid2;
    if (!CraterDropped() && bt_void >= 2 && sync_level >= 2 && (role == 1 || !real_save)) c = static_cast<std::uint16_t>(c | kCapWorldApply);
    return c;
}
// VOID2 goes to a partner whose hello offers it (bt_sync 1 sends too: "sends but applies nothing").
inline bool SendOk(int bt_void, int sync_level, int role, bool peer, std::uint16_t partner_caps) {
    return bt_void >= 1 && sync_level >= 1 && (role == 1 || role == 2) && peer && (partner_caps & kCapVoid2) != 0;
}

// BlastGate with this game's bt_sync: bt_sync 1 logs a partner VOID2 and applies nothing (design 2.0).
inline BlastVerdict BlastFor(int sync_level, const BlastIn& in) {
    if (in.emergency) return BlastVerdict{BlastCall::drop, "EmergencyStop", false};
    if (sync_level < 2) return BlastVerdict{BlastCall::drop, "bt_sync is 1 here (VOID2 logged, never applied)", false};
    return BlastGate(in);
}

// CraterApplyGate as stage 6 ships it: the gazer row refused for good (PF 17: the base game's gazer voidout adds no record; only
// FUN_142A003D0 and the save reader grow the list), bt_sync 2 only, and a test crater (bt_sync 9, kV2Test) only on a sandboxed
// save (a hidden run's redirected saves): a crater is permanent on the save it lands on (PF 16), so a test never reaches a real one.
inline KillVerdict CraterGateRows(int sync_level, CraterApplyIn in) {
    in.gazer_crater_ok = false;
    if (sync_level < 2) return KillVerdict{KillCall::refuse, "bt_sync is 1 here (VOID2 logged, never applied)"};
    const KillVerdict v = CraterApplyGate(in);
    if (v.call != KillCall::refuse && (in.flags & kV2Test) && !in.sandboxed)
        return KillVerdict{KillCall::refuse, "a test crater lands only on a sandboxed save (a hidden run's)"};
    return v;
}
// What the module runs: the rows, then (while CR is dropped) a refusal for whatever they would apply or hold.
inline KillVerdict CraterGate(int sync_level, const CraterApplyIn& in) {
    const KillVerdict v = CraterGateRows(sync_level, in);
    if (CraterDropped() && v.call != KillCall::refuse) return KillVerdict{KillCall::refuse, kCraterDroppedWhy};
    return v;
}

// ---------------------------------------------------------------- the sender: VOID2 from this game's voidouts
// A FUN_14299B1B0 voidout (bt_sync's detour, drained on the game thread): the blast only (crater 0; PF 17). False: no position.
struct LocalIn {
    double pos[3] = {0, 0, 0};
    bool pos_ok = false;
    bool caused = false, baggage = false;
    std::uint8_t source = 0;
    float far_dist = -1;  // [[mgr+0x68]+0x54]
    double me[3] = {0, 0, 0};
    bool me_ok = false;
    std::uint32_t sender_ms = 0, void_id = 0, world = 0;
};
inline bool Void2FromLocal(const LocalIn& in, BtsVoid2& m) {
    if (!in.pos_ok || !PosSane(in.pos)) return false;
    m = BtsVoid2{};
    for (int i = 0; i < 3; ++i) m.pos[i] = in.pos[i];
    const double d = in.me_ok ? Dist3(in.me, in.pos) : -1.0;
    m.player_dist = std::isfinite(d) ? static_cast<float>(d) : -1.0f;
    const bool is_far = m.player_dist >= 0 && in.far_dist > 0 && m.player_dist >= in.far_dist;  // (not `far`: a windows.h macro)
    m.source = in.source;
    m.reason = VoidReason(in.caused, in.baggage, is_far);
    m.flags = static_cast<std::uint8_t>((in.caused ? kV2Gazer : 0) | (in.baggage ? kV2Baggage : 0) | (is_far ? kV2Far : 0));
    m.crater_r = 0;
    m.sender_ms = in.sender_ms, m.void_id = in.void_id, m.world = in.world;
    return Void2Valid(m);
}
// A catcher voidout: what FUN_142A003D0 added to the pending list on the sender (its record's position and radius; else the
// catcher record's position and the default radius 107), the catcher's class (EDSCatcherType + 1, 0 unknown). Crater wanted.
struct CatcherIn {
    double pos[3] = {0, 0, 0};
    bool pos_ok = false;
    float radius = 0;  // the record's (0 / bad: kCraterRadiusDefault)
    std::uint8_t class1 = 0;
    double me[3] = {0, 0, 0};
    bool me_ok = false;
    std::uint32_t sender_ms = 0, void_id = 0, world = 0;
};
inline bool Void2FromCatcher(const CatcherIn& in, BtsVoid2& m) {
    if (!in.pos_ok || !PosSane(in.pos)) return false;
    m = BtsVoid2{};
    for (int i = 0; i < 3; ++i) m.pos[i] = in.pos[i];
    const double d = in.me_ok ? Dist3(in.me, in.pos) : -1.0;
    m.player_dist = std::isfinite(d) ? static_cast<float>(d) : -1.0f;
    m.source = kSrcCatcher;
    m.reason = VoidReason(false, false, false);  // 3 Annihilation (the reason byte is a label only)
    m.flags = static_cast<std::uint8_t>(kV2Catcher | kV2CraterWanted);
    m.class1 = in.class1 <= kCatcherClassMax1 ? in.class1 : 0;
    m.crater_r = CraterRadius(in.radius);
    m.sender_ms = in.sender_ms, m.void_id = in.void_id, m.world = in.world;
    return Void2Valid(m);
}

// ---------------------------------------------------------------- the test op: bt_sync 9 <m> <mode> (hidden runs only)
// mode bits: 1 the spot m from the partner's body (on the line from it towards this game's Sam; 0 m = at the partner), else m ahead
// of this game's Sam; 2 a catcher-flagged test that wants a crater (radius kTestCraterR). mode 4 alone: this game acts as a
// real-save guest for bt_void's own gates (m 1 on, 0 off; the receiver's stand-in for the host_save=0 leg, which a launcherini
// scenario cannot run): it only makes the gates stricter.
constexpr float kTestCraterR = 40.0f;
constexpr double kTestMaxM = 3000.0;
struct TestArgs {
    double m = 0;
    bool from_partner = false, crater = false, standin = false, standin_on = false;
};
inline bool ParseTest(double v1, double v2, TestArgs& a) {
    a = TestArgs{};
    const double mode = std::isfinite(v2) ? v2 : 0;
    if (!(mode >= 0 && mode <= 4 && mode == std::floor(mode))) return false;
    if (!std::isfinite(v1) || v1 < 0 || v1 > kTestMaxM) return false;
    const int bits = static_cast<int>(mode);
    if (bits == 4) {
        if (v1 != 0 && v1 != 1) return false;
        a.standin = true, a.standin_on = v1 == 1;
        return true;
    }
    a.m = v1, a.from_partner = (bits & 1) != 0, a.crater = (bits & 2) != 0;
    return true;
}
// The spot: from the partner (towards me, at the partner's height; no direction: my facing) or m ahead of me (AheadOf).
inline bool TestSpot(const double* me, double fx, double fy, const double* partner, bool partner_ok, double m, bool from_partner, double* out) {
    if (!me || !out || !PosSane(me)) return false;
    if (!from_partner) return AheadOf(me, fx, fy, m, out);
    if (!partner_ok || !partner || !PosSane(partner)) return false;
    double dx = me[0] - partner[0], dy = me[1] - partner[1];
    double l = std::sqrt(dx * dx + dy * dy);
    if (!(l > 0.5)) {  // on top of each other: my facing
        dx = fx, dy = fy;
        l = std::sqrt(dx * dx + dy * dy);
        if (!(l > 1e-6)) return false;
    }
    out[0] = partner[0] + dx / l * m;
    out[1] = partner[1] + dy / l * m;
    out[2] = partner[2];
    return PosSane(out);
}
inline bool Void2Test(const double* pos, double sender_dist, bool crater, std::uint32_t sender_ms, std::uint32_t void_id, std::uint32_t world,
                      BtsVoid2& m) {
    if (!pos || !PosSane(pos)) return false;
    m = BtsVoid2{};
    for (int i = 0; i < 3; ++i) m.pos[i] = pos[i];
    m.player_dist = sender_dist >= 0 && std::isfinite(sender_dist) ? static_cast<float>(sender_dist) : -1.0f;
    m.source = kVoidSrcTest;
    m.reason = kVoidReasonTest;
    m.flags = static_cast<std::uint8_t>(kV2Test | (crater ? (kV2Catcher | kV2CraterWanted) : 0));
    m.crater_r = crater ? kTestCraterR : 0;
    m.sender_ms = sender_ms, m.void_id = void_id, m.world = world;
    return Void2Valid(m);
}

// ---------------------------------------------------------------- repeats and the queues
// The same voidout twice: a VOID2 within 5 s and 100 m of one already queued, playing, or this game's own voidout there (a corpse
// that voided out in both worlds). The later one is dropped as a repeat (its crater, if it wants one, still goes to the crater gate).
constexpr double kRepeatMs = 5000.0, kRepeatM = 100.0;
inline bool Repeat(const double* a, double a_ms, const double* b, double b_ms) {
    if (!a || !b || !PosSane(a) || !PosSane(b)) return false;
    const double dt = b_ms - a_ms;
    return std::fabs(dt) <= kRepeatMs && Dist3(a, b) <= kRepeatM;
}
// A small FIFO (game thread): the pending blasts and craters. Push refuses when full (counted by the caller).
template <class T, unsigned N>
struct Fifo {
    T e[N] = {};
    unsigned n = 0;
    bool Push(const T& v) {
        if (n >= N) return false;
        e[n++] = v;
        return true;
    }
    void PopFront() {
        if (!n) return;
        for (unsigned i = 1; i < n; ++i) e[i - 1] = e[i];
        --n;
    }
    void Clear() { n = 0; }
};
struct PendingVoid {
    BtsVoid2 m;
    double rx_ms = 0;
    bool held_logged = false;
};
constexpr unsigned kBlastQueueCap = 4, kCraterQueueCap = 4;

// ---------------------------------------------------------------- the fade (the effector resource res+0x28 / +0x2C)
struct Fade {
    float start = kFlashOrigStart, range = kFlashOrigRange;
};
inline Fade FadeFor(int flash_m) {
    Fade f;
    f.start = static_cast<float>(FlashStartM(static_cast<double>(flash_m)));
    f.range = kFlashRangeM;
    return f;
}
inline bool SameFade(const Fade& a, const Fade& b) { return std::fabs(a.start - b.start) < 0.01f && std::fabs(a.range - b.range) < 0.01f; }
// What to save before our write: what is there, unless it is already ours (a restore that failed before): then the shipped
// 11500 / 11800 (PF 14 read them every time).
inline Fade FadeToSave(const Fade& cur, const Fade& ours) {
    if (SameFade(cur, ours) || !std::isfinite(cur.start) || !std::isfinite(cur.range) || !(cur.start > 0) || !(cur.range > 0))
        return Fade{};
    return cur;
}
// The restore: write the saved values back only while ours are still there; already the saved ones: nothing to do; anything else
// (the resource freed / reused by a load): leave it.
enum class RestoreCall { write, done, changed };
inline RestoreCall RestoreFor(const Fade& cur, const Fade& ours, const Fade& saved) {
    if (SameFade(cur, ours)) return RestoreCall::write;
    if (SameFade(cur, saved)) return RestoreCall::done;
    return RestoreCall::changed;
}

// ---------------------------------------------------------------- the near check (the gate's leg 4: before and 12 s after a blast)
struct NearRead {
    float health = -1;          // Sam's (-1 unread)
    unsigned flow_calls = 0;    // the GameOver flow gate's counter (bt_sync's FlowCalls; flow_ok false: not hooked)
    bool flow_ok = false;
    std::uint64_t hm140 = 0;    // the hunter manager's area pointer (hm+0x140)
    bool hm_ok = false;
    bool game_over = false;     // DSGameState bit 13
};
// True when nothing changed (health within 0.5, no flow call, hm+0x140 the same, no GameOver); out: "unchanged" or what changed.
inline bool NearCheck(const NearRead& a, const NearRead& b, char* out, std::size_t cap) {
    char buf[160] = {};
    std::size_t at = 0;
    auto add = [&](const char* fmt, double x, double y) {
        if (at + 48 < sizeof(buf)) at += static_cast<std::size_t>(std::snprintf(buf + at, sizeof(buf) - at, fmt, at ? ", " : "", x, y));
    };
    if (a.health >= 0 && b.health >= 0 && std::fabs(a.health - b.health) > 0.5f) add("%shealth %.1f -> %.1f", a.health, b.health);
    if (a.health >= 0 && b.health < 0) add("%shealth %.1f -> unread (%.0f)", a.health, -1);
    if (a.flow_ok && b.flow_ok && b.flow_calls != a.flow_calls) add("%sflow calls %.0f -> %.0f", a.flow_calls, b.flow_calls);
    if (a.hm_ok && b.hm_ok && a.hm140 != b.hm140) add("%shm+0x140 %.0f -> %.0f", a.hm140 ? 1.0 : 0.0, b.hm140 ? 1.0 : 0.0);
    if (!a.game_over && b.game_over) add("%sGameOver %.0f -> %.0f", 0, 1);
    const bool same = at == 0;
    if (out && cap) std::snprintf(out, cap, "%s%s", same ? "unchanged" : "CHANGED: ", same ? "" : buf);
    return same;
}

// ---------------------------------------------------------------- the crater kept / gone after a load
// A record we added, found again by its place and radius (the gen may be kept by the save, PF 16, but is not relied on).
constexpr double kSameCraterM = 1.0;
inline bool SameCrater(const double* a, float ra, const double* b, float rb) {
    return a && b && PosSane(a) && PosSane(b) && Dist3(a, b) <= kSameCraterM && std::fabs(ra - rb) <= 0.5f;
}
constexpr double kAfterLoadFirstMs = 8000.0, kAfterLoadLastMs = 30000.0, kCraterActiveMs = 9000.0;

// ---------------------------------------------------------------- log rate (game thread)
// A token bucket: up to kLogBurst lines at once, one more each kLogRefillMs; a line beyond that is dropped and counted.
constexpr double kLogBurst = 24.0, kLogRefillMs = 1000.0;
struct LogBucket {
    double tokens = kLogBurst, last_ms = -1;
    unsigned dropped = 0;
    bool Take(double now_ms) {
        if (last_ms >= 0 && now_ms > last_ms) {
            tokens += (now_ms - last_ms) / kLogRefillMs;
            if (tokens > kLogBurst) tokens = kLogBurst;
        }
        last_ms = now_ms;
        if (tokens >= 1.0) {
            tokens -= 1.0;
            return true;
        }
        ++dropped;
        return false;
    }
};

}  // namespace st::btrules::voidout
