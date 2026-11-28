// Round 31 stage 5 (bt_mirror): this feature's own pure logic (no game, no Windows), tested by tests/bt_mirror_tests.cpp. The
// shared codecs, caps and the stage-1 gates stay in bt_rules.hpp (included); new pure helpers of this feature go here, so the
// seven builders never edit one file at once.
//
// GM, the gazer mirror (the round 31 build spec, Stage 5, CHANGED): the receiver's REALIZED copy (+0x24 == 2: an entity and a
// DSGazerComponent, this game's player within about 50 m, kept to 60 m, in rain) of a gazer that senses the partner reacts the
// way the partner's own gazer does. The window is trigger + 5 m (about 25 m) .. 60 m; nearer, this game's own gazer AI decides.
// Gate supersedes bt_rules.hpp's round-31 stage-1 GzMirrorGate (state 0, no real_save / turn_only): bt_mirror uses Gate only.
#pragma once

#include <cmath>
#include <cstdint>

#include "bt_rules.hpp"

namespace st::btrules::mirror {

constexpr int kStage = 5;

// The window and the timers.
constexpr double kOnMarginM = 5.0;     // on: this game's player beyond trigger + 5 m
constexpr double kOffMarginM = 2.0;    // released below trigger + 2 m (between the two a mirror keeps its state)
constexpr double kFarM = 60.0;         // the game releases a realized gazer at 60 m (PF b:1403); the mirror never goes beyond
constexpr double kQuietMs = 2000.0;    // released 2 s after the last GZST2 entry for it
constexpr double kGateMs = 250.0;      // the gate, the census and the GZST2 send run at 4 Hz
constexpr double kYawMinMs = 33.0;     // the yaw is written at most 30 times a second per mirror
constexpr double kYawMaxDtS = 0.25;    // one yaw step never covers more than 0.25 s (a stalled tick does not jump)
constexpr double kYawEpsRad = 0.0087;  // 0.5 deg: closer than that, no write
constexpr unsigned kMax = 6;           // at most 6 mirrored (the back checks included)
constexpr float kAgitOn = 0.5f;        // the agitation pair is written while the partner's agitation is above 0.5
constexpr unsigned kAgitOnByte = 128;  // 0.5 * 255 rounded up: a byte above 127
constexpr std::uint8_t kAgitOwn80 = 1;      // comp+0x80 while agitated (PF 7)
constexpr float kAgitOwn7C = 10.0f;         // comp+0x7C while agitated (PF 7)
constexpr int kBackTries = 5;               // the write-back is read back once a second, at most 5 times
// BB stress (build spec, Stage 5 gate): if "Lou's stress level is increasing" shows in the mirror leg's shots and not in the
// control leg's, this goes false (the agitation write only on a sandboxed guest, never on the host) before the default flips.
constexpr bool kAgitOnHost = true;

// The sender's agitation byte (GZST2 u8 agit): the graph var (comp's [[+0x198]+0x58] + idx*0x30 + 0x20) when the gazer has a
// component (state 2) and it reads; otherwise (a gazer sensing its own player is state 0: no component, PF fact 1) 1.0 while it
// senses, reacts to or holds its player, else 0.
inline float SenderAgit(std::uint8_t bits, bool graph_ok, float graph) {
    if (graph_ok && std::isfinite(graph) && graph >= 0) return graph > 1 ? 1.0f : graph;
    return (bits & kGzDead) == 0 && (bits & (kGzNear | kGzReact | kGzCaught)) != 0 ? 1.0f : 0.0f;
}
// A gazer the sender reports in GZST2: it senses, reacts to or holds the sender's player, and is not dead.
inline bool SenderWants(std::uint8_t bits) { return (bits & kGzDead) == 0 && (bits & (kGzNear | kGzReact | kGzCaught)) != 0; }
// The sender's reach (gate fix, runs 20261128-012446 / -012712): +0x1B4 NEAR 0x1000 is NOT cleared when the sender's player
// leaves. The host went from 9.3 m to 113 m from 40AB1A93 and its census never saw the bit drop (no "GZST sent: .. ended" in
// 20 s; the round 30 bt-field runs show the same at 65 m), so GZST2 kept the guest's mirror on until the partner was gone. A
// sensing bit counts only while the sender's player is within the gazer's trigger radius + 10 m (30 m when the radius is
// unknown), by our own distance (gazer position to Sam); the game's +0x204 only when ours is unknown (+0x204 may be as stale
// as the bit). Unknown both ways: not in reach.
constexpr double kReachSlackM = 10.0, kReachNoTriggerM = 30.0;
inline double SenderReachM(double trigger_m) {
    return (std::isfinite(trigger_m) && trigger_m > 0) ? trigger_m + kReachSlackM : kReachNoTriggerM;
}
inline bool SenderInReach(double own_dist_m, double game_dist_m, double trigger_m) {
    const double d = (std::isfinite(own_dist_m) && own_dist_m >= 0) ? own_dist_m : game_dist_m;
    return std::isfinite(d) && d >= 0 && d <= SenderReachM(trigger_m);
}
// The bits the sender acts on: the sensing bits (NEAR, react, caught) cleared while out of reach (so a return is a new rise).
inline std::uint8_t SenderBits(std::uint8_t bits, bool in_reach) {
    return in_reach ? bits : static_cast<std::uint8_t>(bits & ~(kGzNear | kGzReact | kGzCaught));
}
// The GZST2 edges from the bits' rises (react -> FOUND, caught -> CAUGHT).
inline std::uint8_t SenderEdges(std::uint8_t prev, std::uint8_t now) {
    std::uint8_t e = 0;
    if ((now & kGzReact) && !(prev & kGzReact)) e |= kGeFound;
    if ((now & kGzCaught) && !(prev & kGzCaught)) e |= kGeCaught;
    return e;
}
// The partner's agitation byte asks for the pair.
inline bool AgitWanted(std::uint8_t agit) { return agit >= kAgitOnByte; }

// Dead here: flags 0x8 (PF fact 2) or the health +0x180 at or below 0 (NaN counts as unknown, not dead).
inline bool DeadHere(std::uint32_t flags, float health) { return (flags & kFlagDead) != 0 || (std::isfinite(health) && health <= 0); }

// The per-gazer gate (4 Hz, game thread). mirror: start or keep mirroring it; release: stop now (write the agitation pair back);
// none: not mirrored, nothing to do. held: a "not realized here" refusal of a gazer the partner's GZST2 shows (logged as
// `[bt] mirror: held: loc L not realized here`). turn_only: the yaw only (a real-save guest, or the drop hooks are not in).
struct GateIn {
    int key = 0;                     // bt_gazer_mirror here
    int sync_level = 0;              // bt_sync here (2 applies)
    bool emergency = false;
    bool mirrored = false;           // this gazer is mirrored now
    unsigned mirrored_n = 0;         // records in use now (mirrors and back checks; this one included when mirrored)
    std::uint8_t partner_bits = 0;   // its last GZST2 entry's bits
    double since_ms = 1e18;          // since that entry
    bool resolved = false;           // the same field and locator, live here (its actor resolved)
    bool loading = false;
    const char* not_playing = nullptr;
    int state = -1;                  // +0x24 here
    bool dead = false;               // DeadHere
    bool caught_here = false;        // +0x1B4 0x2000000 here (it holds this game's player)
    bool comp_ok = false;            // its entity has a DSGazerComponent here (only in state 2)
    double local_dist_m = -1;        // +0x204 here (this game's player)
    double trigger_m = -1;           // [+0x388] +0x98 here
    bool real_save = false;          // this game is a guest on its own real save (host_save=0)
    bool drops_ok = false;           // the two drop detours are in (FUN_1429C49D0, FUN_14299EFB0)
};
struct GateOut {
    GzCall call = GzCall::none;
    const char* why = nullptr;
    bool held = false;
    bool turn_only = false;
};
inline GateOut Gate(const GateIn& in) {
    GateOut o;
    o.turn_only = in.real_save || !in.drops_ok;
    auto stop = [&](const char* w, bool held = false) {
        o.call = in.mirrored ? GzCall::release : GzCall::none;
        o.why = w;
        o.held = held && !in.mirrored;
        return o;
    };
    if (in.emergency) return stop("EmergencyStop");
    if (in.key < 1) return stop("bt_gazer_mirror is 0 here");
    if (in.sync_level < 2) return stop("bt_sync is below 2 here (relayed, not applied)");
    if (in.loading) return stop("a load");
    if (!(in.since_ms <= kQuietMs)) return stop("no GZST2 for 2 s");
    if (!SenderWants(in.partner_bits)) return stop("the partner's gazer no longer senses her");
    if (!in.resolved) return stop("not live and streamed here");
    if (in.not_playing) return stop(in.not_playing);
    if (in.state != 2) return stop("not realized here (no entity: this game's player is beyond about 50 m, or no rain)", true);
    if (in.dead) return stop("dead here");
    if (in.caught_here) return stop("it holds this game's player");
    if (!in.comp_ok) return stop("not realized here (no DSGazerComponent)", true);
    if (!(in.local_dist_m >= 0) || !(in.trigger_m >= 0)) return stop("its distance or trigger radius is unknown here");
    if (in.local_dist_m > kFarM) return stop("not realized here (this game's player is beyond 60 m)", true);
    if (in.mirrored) {
        if (in.local_dist_m < in.trigger_m + kOffMarginM) return stop("this game's player came within its trigger radius + 2 m");
        o.call = GzCall::mirror;
        return o;
    }
    if (!(in.local_dist_m > in.trigger_m + kOnMarginM)) return stop("this game's player is within its trigger radius + 5 m");
    if (in.mirrored_n >= kMax) return stop("6 gazers are mirrored already");
    o.call = GzCall::mirror;
    return o;
}

// What a mirror writes this frame (AfterUpdate): kind on the host and a sandboxed guest; the agitation pair also while the
// partner's agitation is above 0.5 (and, while kAgitOnHost is false, never on the host). role: 1 host, 2 guest.
struct Writes {
    bool kind = false, agit = false;
};
inline Writes WritesFor(bool turn_only, int role, std::uint8_t partner_agit, bool agit_on_host = kAgitOnHost) {
    Writes w;
    if (turn_only) return w;
    w.kind = true;
    w.agit = AgitWanted(partner_agit) && (role == 2 || agit_on_host);
    return w;
}

// The yaw (radians, in the entity's forward row convention: atan2(row1.y, row1.x), PF 9) from `from` toward `to` in the ground
// plane (x, y; z up). False when the two are closer than 0.5 m or not finite.
inline bool YawToward(const double* from, const double* to, double* out) {
    const double dx = to[0] - from[0], dy = to[1] - from[1];
    if (!std::isfinite(dx) || !std::isfinite(dy) || dx * dx + dy * dy < 0.25) return false;
    *out = std::atan2(dy, dx);
    return true;
}
// The signed shortest turn from cur to want (radians, -pi..pi).
inline double YawDiff(double cur, double want) {
    const double pi = 3.141592653589793;
    double d = std::fmod(want - cur, 2 * pi);
    if (d > pi) d -= 2 * pi;
    if (d < -pi) d += 2 * pi;
    return d;
}
// The step dt for one yaw write: the time since the last write, at most kYawMaxDtS (0 when no write happened yet: the first
// step uses one tick of kYawMinMs).
inline double YawDt(double now_ms, double last_ms) {
    if (!(last_ms > 0) || !(now_ms > last_ms)) return kYawMinMs / 1000.0;
    const double dt = (now_ms - last_ms) / 1000.0;
    return dt > kYawMaxDtS ? kYawMaxDtS : dt;
}
// The 2D rotation of every row by `delta` (radians), as bt_probe 23 does (x' = x c - y s, y' = x s + y c): the forward row's yaw
// grows by delta.
inline void RotateRows(float (&m)[3][3], double delta) {
    const double c = std::cos(delta), s = std::sin(delta);
    for (int r = 0; r < 3; ++r) {
        const double x = m[r][0], y = m[r][1];
        m[r][0] = static_cast<float>(x * c - y * s), m[r][1] = static_cast<float>(x * s + y * c);
    }
}
inline double RowYaw(const float (&m)[3][3]) { return std::atan2(static_cast<double>(m[1][1]), static_cast<double>(m[1][0])); }
constexpr double kDeg = 57.29577951308232;

// The per-mirror status line: once a second for the first 10 s, then every 10 s.
inline double NextLineMs(double start_ms, double now_ms) { return now_ms + (now_ms - start_ms < 10000.0 ? 1000.0 : 10000.0); }

// The agitation write-back read back: comp+0x80 and +0x7C equal the gazer's own values.
inline bool BackOk(unsigned own80, float own7C, unsigned b80, float b7C) {
    return b80 == own80 && std::isfinite(b7C) && std::fabs(b7C - own7C) < 0.01f;
}

// The stale reasons (the per-write re-check; 0 = still good).
enum Stale : int { kFresh = 0, kStaleId = 1, kStaleState = 2, kStaleEntity = 3, kStaleBack = 4 };
inline const char* StaleText(int w) {
    switch (w) {
        case kStaleId: return "the gazer is gone: another id at its actor's address";
        case kStaleState: return "state != 2: its entity and component were released";
        case kStaleEntity: return "its entity changed (+0x68)";
        case kStaleBack: return "the component no longer points back at the gazer";
        default: return "stale";
    }
}
// The re-check itself, on values read now: the id read, the id mirrored, the state, the entity now and at the start, and the
// component's back pointer [comp+0x110] (the actor is that - 0xE8).
inline int StaleCheck(bool id_ok, std::uint32_t id, std::uint32_t want_id, int state, std::uintptr_t ent_now, std::uintptr_t ent0,
                      std::uintptr_t comp_back, std::uintptr_t actor) {
    if (!actor || !id_ok || id != want_id) return kStaleId;
    if (state != 2) return kStaleState;
    if (!ent0 || ent_now != ent0) return kStaleEntity;
    if (comp_back <= 0xE8 || comp_back - 0xE8 != actor) return kStaleBack;
    return kFresh;
}

// The drop detours' verdicts: FUN_1429C49D0 (DSGazerComponent MsgDsAiAction) drops FOUND and catch for a mirrored component
// (0x68A02F9F, the idle action, and everything else pass); FUN_14299EFB0 (the actor message) drops caught for a mirrored actor.
inline bool DropAction(std::uint32_t hash) { return hash == kGzActionFound || hash == kGzActionCatch; }
inline bool DropMessage(std::uint32_t hash) { return hash == kGzMsgCaught; }

}  // namespace st::btrules::mirror
