// Round 28 P4 / O3: the Odradek scan's pure rules (no game access; tests/odra_rules_tests.cpp; docs/enemy-sync.md section 19).
//
// The scan state machine FUN_14282B530 keeps its state in inv +0x4850:
//   0 idle, 1 arm (waits for the arm entity's +0x348 == 0x14, 2 s timeout), 2 body, 3 charge (+0x4934 counts down),
//   4 expanding (the wave grows; wave +0x2BD set moves on to 5), 5 marks shown (while a +0x48D8 mark entry is live), then 0.
// The pulse (the survey component's FUN_1433123C0 call and the DSPlayerSystem origin) happens on the 3 -> 4 step.
// The survey component (DSOdradekSurveyEffectComponent): +0x244 active, +0x245 finished (idle: 0 / 1; a scan: 1 / 0).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace st::odrarules {

inline constexpr int kIdle = 0, kArm = 1, kBody = 2, kCharge = 3, kExpand = 4, kMarks = 5;
inline constexpr double kMaxRangeM = 300.0;       // a partner scan from farther away is refused (O3)
inline constexpr double kMinIntervalMs = 1500.0;  // at most one accepted remote / replayed scan per 1.5 s
inline constexpr std::uint32_t kBodyReadyMask = 0x6408D3C2u;  // FUN_14295BA70: the body states the scan may start from

// The pulse edge: the local state machine stepped from charge to expanding (the native survey call happened).
inline bool ScanEdge(int prev, int cur) noexcept { return prev == kCharge && cur == kExpand; }

// A survey runs (FUN_1433123C0 wrote +0x244 = 1, +0x245 = 0) and has not finished yet.
inline bool SurveyActive(std::uint8_t b244, std::uint8_t b245) noexcept { return b244 != 0 && b245 == 0; }

// FUN_14295BA70 (the body entity): +0x3E4 bit 2 set, and the body state +0x348 <= 0x1E with its bit in kBodyReadyMask.
inline bool BodyReady(std::uint32_t flags3e4, std::uint8_t state348) noexcept {
    if (!(flags3e4 & 4u)) return false;
    if (state348 > 0x1E) return false;
    return ((kBodyReadyMask >> state348) & 1u) != 0;
}

// ---- the local scan request (odra scan): the native trigger 0x142805F80 only makes sense where the machine consumes it.
enum class TriggerVerdict : std::uint8_t { accept, busy, no_odradek, not_ready, no_wave };

struct TriggerIn {
    int state = 0;             // inv +0x4850
    bool arm_ref = false;      // inv +0x4C88 non-null
    bool body_ref = false;     // inv +0x4CA0 non-null
    // inv +0x4C7F bit 0: NOT a gate (round 28 p4 run: 0 at idle with the Odradek out). It is the one-shot request that the export
    // ActivateOdradek (0x1426E64C0) sets and the inventory update clears again (0x1427DBF3A, and 0x1427DBC80); neither the native
    // trigger 0x142805CE0 nor the state machine FUN_14282B530 reads it. Kept for the log only.
    bool activated = false;
    bool body_ready = false;   // BodyReady(...) of the body entity
    bool wave = false;         // the wave pointer (inv +0x4EF8, or +0x4F00 by +0x4C7C bit 0x40) non-null
};

// The request bit +0x4C79 & 0x20 is consumed only in state 0 or 5, and cleared on every call.
inline TriggerVerdict ScanRequest(const TriggerIn& in) noexcept {
    if (in.state != kIdle && in.state != kMarks) return TriggerVerdict::busy;
    if (!in.arm_ref || !in.body_ref) return TriggerVerdict::no_odradek;
    if (!in.wave) return TriggerVerdict::no_wave;  // state 4 reads wave +0x2AC with no null check
    if (!in.body_ready) return TriggerVerdict::not_ready;
    return TriggerVerdict::accept;
}

inline const char* TriggerName(TriggerVerdict v) noexcept {
    switch (v) {
        case TriggerVerdict::accept: return "accept";
        case TriggerVerdict::busy: return "busy (the local scan runs)";
        case TriggerVerdict::no_odradek: return "no odradek (arm / body ref missing)";
        case TriggerVerdict::not_ready: return "not ready (the body state)";
        case TriggerVerdict::no_wave: return "no wave";
    }
    return "?";
}

// ---- a scan shown from another origin (the probe's replay now, the partner's scan in O3)
enum class ScanVerdict : std::uint8_t {
    accept,
    busy_local_scan,  // the local machine is not idle (states 1..5, or an unknown state)
    busy_survey,      // the survey component still shows a scan
    too_far,          // farther than kMaxRangeM from the local player
    rate_limited,     // less than kMinIntervalMs after the last accepted one
    duplicate,        // the same seq as the last accepted one
    stale,            // an older seq (u16 wraparound)
    bad_origin,       // not finite
    no_wave,          // the local wave pointer is null (the decal step at 0x14282C78E reads wave +0x20 unchecked)
    no_comp,          // no survey component
};

struct ScanIn {
    int state = 0;                 // local inv +0x4850
    std::uint8_t b244 = 0, b245 = 1;  // local survey comp
    bool wave = false;
    bool comp = false;
    double origin[3] = {0, 0, 0};
    double dist_m = 0;             // from the local player to origin
    double since_last_ms = 1e9;    // since the last accepted scan (a large value when none)
    bool have_last_seq = false;
    std::uint16_t seq = 0, last_seq = 0;
};

// The newer test of two u16 sequence numbers: >0 newer, 0 the same, <0 older (half the range each way).
inline int SeqCompare(std::uint16_t seq, std::uint16_t last) noexcept {
    return static_cast<int>(static_cast<std::int16_t>(static_cast<std::uint16_t>(seq - last)));
}

inline ScanVerdict AcceptScan(const ScanIn& in) noexcept {
    for (double c : in.origin)
        if (!std::isfinite(c)) return ScanVerdict::bad_origin;
    if (!std::isfinite(in.dist_m)) return ScanVerdict::bad_origin;
    if (in.have_last_seq) {
        const int d = SeqCompare(in.seq, in.last_seq);
        if (d == 0) return ScanVerdict::duplicate;
        if (d < 0) return ScanVerdict::stale;
    }
    if (!in.comp) return ScanVerdict::no_comp;
    if (in.state != kIdle) return ScanVerdict::busy_local_scan;
    if (SurveyActive(in.b244, in.b245)) return ScanVerdict::busy_survey;
    if (!in.wave) return ScanVerdict::no_wave;
    if (in.dist_m > kMaxRangeM) return ScanVerdict::too_far;
    if (in.since_last_ms < kMinIntervalMs) return ScanVerdict::rate_limited;
    return ScanVerdict::accept;
}

inline const char* ScanName(ScanVerdict v) noexcept {
    switch (v) {
        case ScanVerdict::accept: return "accept";
        case ScanVerdict::busy_local_scan: return "busy (local scan)";
        case ScanVerdict::busy_survey: return "busy (survey shown)";
        case ScanVerdict::too_far: return "too far";
        case ScanVerdict::rate_limited: return "rate limited";
        case ScanVerdict::duplicate: return "duplicate seq";
        case ScanVerdict::stale: return "stale seq";
        case ScanVerdict::bad_origin: return "bad origin";
        case ScanVerdict::no_wave: return "no wave";
        case ScanVerdict::no_comp: return "no survey component";
    }
    return "?";
}

// The survey's direction (comp +0xF0, float4): inv +0x4880 / +0x4884 normalized, z and w 0; (0, 1) when degenerate.
inline void SurveyDir(float dx, float dy, float (&out)[4]) noexcept {
    const float l2 = dx * dx + dy * dy;
    if (!(l2 > 1e-8f) || !std::isfinite(l2)) {
        out[0] = 0.0f, out[1] = 1.0f;
    } else {
        const float l = std::sqrt(l2);
        out[0] = dx / l, out[1] = dy / l;
    }
    out[2] = 0.0f, out[3] = 0.0f;
}


// ================================================================ O3: the partner's scan shown here (odradek_scan_sync=1)
//
// RE (round 28 O3; ds.exe DC): what draws the visible scan is not the survey component (its +0x244 lasts 0.2 s) but the wave
// object (DSOdradekEffectInstance, inv +0x4EF8 / +0x4F00) and the machine's per-frame tail 0x14282C542, which runs on EVERY call
// in any state: while the selected wave is non-null and wave +0x2BD == 0 it writes the shader globals (OdradekSensorPosition =
// wave +0x20, EmitDirection = inv +0x4880, Distance = wave +0x2AC), the sensor globals [0x147BFDFF0] / [0x147BFD940], the
// DecalManager ring and DSPlayerSystem +0x205F0. The wave's own update FUN_143318CB0 grows +0x2AC from 0 to Size over TimeMax,
// then sets +0x2BD. So restarting the wave at another origin (the pulse's wave steps 0x14282BEC6..0x14282BF5E) while the machine
// sits in state 0 makes the game draw a full scan there, without the Odradek's arm / body animation or the body sound (the wave's
// own Start / Edge sounds play at the wave position, set by wave +0x2C1), and without the AI stimulus (the +0x4868 timer).
//
// The steps, by bit (StartMask; the probe's odra 5 takes a mask to bisect them):
inline constexpr std::uint32_t kStartWave = 1;      // FUN_141E0B020(wave, xf), +0x28C = 135 deg, +0x250 = dir, +0x2C0 = 1,
                                                    // FUN_1432FC7A0(wave) (the reset: +0x290 / +0x2AC / +0x2BD = 0), +0x2C1 = 1
inline constexpr std::uint32_t kStartEmitDir = 2;   // inv +0x4880 / +0x4884 = dir (the tail's shader / decal direction)
inline constexpr std::uint32_t kStartSlot = 4;      // inv +0x4874 += 1 (the pulse counter: its parity picks the shader slot)
inline constexpr std::uint32_t kStartSensor = 8;    // the trigger's sensor writes: FUN_142F4B5A0 / FUN_141DD08C0 / FUN_142F4CF60
                                                    // on [0x147BFDFF0], and [0x147BFD940] +0x5FB0 / +0x5FC0 / +0x5FD0 / +0x5FD4 / +0x5FD8
inline constexpr std::uint32_t kStartFront = 16;    // [0x147BF09D8] +0x1B8..+0x1F3 = the 60-byte transform (the wave front)
inline constexpr std::uint32_t kStartSystem = 32;   // DSPlayerSystem +0x205D8 origin, +0x205F8 angle, +0x205F4 yaw (never +0x205D4)
inline constexpr std::uint32_t kStartSurvey = 64;   // survey comp +0x246 = 1, +0xF0 = dir, FUN_1433123C0(comp, origin)
inline constexpr std::uint32_t kStartAll = 127;
// state 5 (the local marks are shown, up to DisplayMarkTime): the scan is shown without the DSPlayerSystem writes, so the
// local marks' sector / in-wave tests keep Sam's own last scan (the machine's state 5 never reads the wave: 0x14282B6A0)
inline constexpr std::uint32_t kStartVisual = kStartAll & ~kStartSystem;
inline constexpr float kScanAngleRad = 2.35619449f;  // 0x4016CBE4: the pulse's wave +0x28C and DSPlayerSystem +0x205F8
inline constexpr double kRemoteMaxRangeM = 500.0;    // farther: dropped (the wave reaches 200 m; 300 + 200 m is out of sight)
inline constexpr double kPendingTtlMs = 10000.0;     // a queued partner scan is dropped (stale) after 10 s

// ---- the SOD1 message (reliable combat channel 6; docs/enemy-sync.md section 19), little-endian:
//   HELLO  'SOD1' | u8 version 1 | u8 type 1 | u8 caps | u8 0                                          8 B
//   SCAN   'SOD1' | u8 version 1 | u8 type 2 | u16 seq | u8 flags | u8 0 | f64 x y z | f32 dx dy | u32 world   46 B
//          (world: the sender's world fingerprint, esync::Gate().guids, 0 when it has none; a receiver with its own refuses a
//          scan from another world)
// The HELLO is the capability announcement: a game shows the partner's scans, and sends its own, only after the partner's
// HELLO carried kCapOdradekScan (its odradek_scan_sync=1 and its detour installed). Sent once per link generation and on a
// change. The SCAN is the sender's own pulse (its 3 -> 4 edge): the DSPlayerSystem origin and inv +0x4880.
inline constexpr std::uint8_t kSodVersion = 1;
inline constexpr std::uint8_t kSodHello = 1, kSodScan = 2;
inline constexpr std::size_t kSodHelloSize = 8, kSodScanSize = 46;
// The capability bit (the HELLO's caps byte; the round's capability table, docs/enemy-sync.md section 19). Only this feature
// reads the byte, so the value cannot collide with the SEH1 parts (full since kPartRestrain).
inline constexpr std::uint8_t kCapOdradekScan = 1;
inline constexpr std::uint8_t kSodCapsKnown = kCapOdradekScan;
inline constexpr std::uint8_t kSodFlagTest = 1;     // a hidden-run scan (odra 8), not the sender's own pulse
inline constexpr std::uint8_t kSodFlagsKnown = kSodFlagTest;

struct SodScan {
    std::uint16_t seq = 0;
    std::uint8_t flags = 0;
    double origin[3] = {0, 0, 0};
    float dir[2] = {0, 1};
    std::uint32_t world = 0;  // the sender's world fingerprint (0: none)
};

namespace detail {
inline void PutU16(std::uint8_t* p, std::uint16_t v) noexcept { p[0] = static_cast<std::uint8_t>(v), p[1] = static_cast<std::uint8_t>(v >> 8); }
inline std::uint16_t GetU16(const std::uint8_t* p) noexcept { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
template <class T>
inline void PutRaw(std::uint8_t* p, T v) noexcept { std::memcpy(p, &v, sizeof(T)); }  // x64: little-endian
template <class T>
inline T GetRaw(const std::uint8_t* p) noexcept {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}
inline bool Magic(const std::uint8_t* d, std::size_t n) noexcept { return d && n >= 6 && d[0] == 'S' && d[1] == 'O' && d[2] == 'D' && d[3] == '1'; }
}  // namespace detail

// Any SOD1 message (the link's routing test: the magic only).
inline bool LooksLikeSod(const std::uint8_t* d, std::size_t n) noexcept { return detail::Magic(d, n); }

inline std::size_t EncodeSodHello(std::uint8_t caps, std::uint8_t* out, std::size_t cap) noexcept {
    if (!out || cap < kSodHelloSize) return 0;
    std::memcpy(out, "SOD1", 4);
    out[4] = kSodVersion, out[5] = kSodHello, out[6] = static_cast<std::uint8_t>(caps & kSodCapsKnown), out[7] = 0;
    return kSodHelloSize;
}

// A newer build may add caps bits: unknown bits are ignored (masked), not refused.
inline bool DecodeSodHello(const std::uint8_t* d, std::size_t n, std::uint8_t* caps) noexcept {
    if (!detail::Magic(d, n) || n != kSodHelloSize || d[4] != kSodVersion || d[5] != kSodHello || d[7] != 0) return false;
    if (caps) *caps = static_cast<std::uint8_t>(d[6] & kSodCapsKnown);
    return true;
}

// The direction normalized in the plane; false when degenerate or not finite.
inline bool NormDir2(float dx, float dy, float (&out)[2]) noexcept {
    const float l2 = dx * dx + dy * dy;
    if (!std::isfinite(l2) || !(l2 > 1e-8f)) return false;
    const float l = std::sqrt(l2);
    out[0] = dx / l, out[1] = dy / l;
    return true;
}

inline bool OriginOk(const double (&o)[3]) noexcept {
    for (double c : o)
        if (!std::isfinite(c) || c > 1e7 || c < -1e7) return false;
    return !(o[0] == 0.0 && o[1] == 0.0 && o[2] == 0.0);  // an unset DSPlayerSystem origin
}

inline std::size_t EncodeSodScan(const SodScan& s, std::uint8_t* out, std::size_t cap) noexcept {
    if (!out || cap < kSodScanSize) return 0;
    float d[2];
    if (!OriginOk(s.origin) || !NormDir2(s.dir[0], s.dir[1], d) || (s.flags & ~kSodFlagsKnown)) return 0;
    std::memcpy(out, "SOD1", 4);
    out[4] = kSodVersion, out[5] = kSodScan;
    detail::PutU16(out + 6, s.seq);
    out[8] = s.flags, out[9] = 0;
    for (int i = 0; i < 3; ++i) detail::PutRaw(out + 10 + 8 * i, s.origin[i]);
    detail::PutRaw(out + 34, d[0]);
    detail::PutRaw(out + 38, d[1]);
    detail::PutRaw(out + 42, s.world);
    return kSodScanSize;
}

// Strict: the exact size, version 1, the reserved byte 0, known flags only, a finite non-zero origin, a unit direction
// (0.9..1.1 before it is normalized again).
inline bool DecodeSodScan(const std::uint8_t* d, std::size_t n, SodScan& out) noexcept {
    if (!detail::Magic(d, n) || n != kSodScanSize || d[4] != kSodVersion || d[5] != kSodScan || d[9] != 0) return false;
    if (d[8] & ~kSodFlagsKnown) return false;
    SodScan s;
    s.seq = detail::GetU16(d + 6);
    s.flags = d[8];
    for (int i = 0; i < 3; ++i) s.origin[i] = detail::GetRaw<double>(d + 10 + 8 * i);
    const float dx = detail::GetRaw<float>(d + 34), dy = detail::GetRaw<float>(d + 38);
    s.world = detail::GetRaw<std::uint32_t>(d + 42);
    if (!OriginOk(s.origin) || !std::isfinite(dx) || !std::isfinite(dy)) return false;
    const float l2 = dx * dx + dy * dy;
    if (l2 < 0.81f || l2 > 1.21f) return false;
    if (!NormDir2(dx, dy, s.dir)) return false;
    out = s;
    return true;
}

// ---- the game thread's admission of a received SCAN (before it is queued for the machine)
enum class AdmitVerdict : std::uint8_t {
    queue,       // handed to the detour (shown at once when the machine is idle, else when it is)
    off,         // odradek_scan_sync=0 here, or the detour is off / stopped
    no_hello,    // the partner never announced kCapOdradekScan on this link generation
    no_player,   // no local player transform
    too_far,     // farther than kRemoteMaxRangeM
    duplicate,   // the same seq as the newest taken on this link generation
    stale,       // an older seq
    other_world, // both games have a world fingerprint and they differ (another map or area)
};

struct AdmitIn {
    bool on = false;            // the key, the detour installed, not stopped
    bool peer_cap = false;      // the partner's HELLO carried kCapOdradekScan (this link generation)
    bool test = false;          // odra 5 (hidden runs): no partner, no seq history
    bool have_player = false;
    double dist_m = 0;          // the local player to the origin
    bool have_last = false;     // a seq was taken on this link generation
    std::uint16_t seq = 0, last_seq = 0;
    std::uint32_t world = 0, our_world = 0;  // the scan's world fingerprint and ours (0: unknown, not compared)
};

inline AdmitVerdict AdmitRemote(const AdmitIn& in) noexcept {
    if (!in.on) return AdmitVerdict::off;
    if (!in.test && !in.peer_cap) return AdmitVerdict::no_hello;
    if (!in.test && in.have_last) {
        const int d = SeqCompare(in.seq, in.last_seq);
        if (d == 0) return AdmitVerdict::duplicate;
        if (d < 0) return AdmitVerdict::stale;
    }
    if (in.world && in.our_world && in.world != in.our_world) return AdmitVerdict::other_world;
    if (!in.have_player) return AdmitVerdict::no_player;
    if (!std::isfinite(in.dist_m) || in.dist_m > kRemoteMaxRangeM) return AdmitVerdict::too_far;
    return AdmitVerdict::queue;
}

// A partner scan already admitted, re-checked every tick until the detour starts it: dropped on a load, a new world fingerprint
// or the local player out of range (a load or fast travel within its 10 s wait must not start it at the old origin).
enum class KeepVerdict : std::uint8_t { keep, loading, world_changed, no_player, too_far };
struct KeepIn {
    bool loading = false;            // the game's load gates (game::ReadStopGates' mask != 0)
    std::uint32_t world_at = 0, world_now = 0;  // our world fingerprint when it was admitted and now
    bool have_player = false;
    double dist_m = 0;
};
inline KeepVerdict KeepPending(const KeepIn& in) noexcept {
    if (in.loading) return KeepVerdict::loading;
    if (in.world_at != in.world_now) return KeepVerdict::world_changed;
    if (!in.have_player) return KeepVerdict::no_player;
    if (!std::isfinite(in.dist_m) || in.dist_m > kRemoteMaxRangeM) return KeepVerdict::too_far;
    return KeepVerdict::keep;
}
inline const char* KeepName(KeepVerdict v) noexcept {
    switch (v) {
        case KeepVerdict::keep: return "keep";
        case KeepVerdict::loading: return "a load";
        case KeepVerdict::world_changed: return "another world here";
        case KeepVerdict::no_player: return "no local player";
        case KeepVerdict::too_far: return "too_far (the local player moved)";
    }
    return "?";
}

inline const char* AdmitName(AdmitVerdict v) noexcept {
    switch (v) {
        case AdmitVerdict::queue: return "queue";
        case AdmitVerdict::off: return "off";
        case AdmitVerdict::no_hello: return "no_hello";
        case AdmitVerdict::no_player: return "no_player";
        case AdmitVerdict::too_far: return "too_far";
        case AdmitVerdict::duplicate: return "duplicate";
        case AdmitVerdict::stale: return "stale_seq";
        case AdmitVerdict::other_world: return "other_world";
    }
    return "?";
}

// ---- the detour's decision for the queued scan (inside FUN_14282B530, after the machine ran)
enum class StartVerdict : std::uint8_t {
    start_full,       // state 0: every step of the mask
    start_visual,     // state 5: the mask without kStartSystem
    wait_local_scan,  // states 1..4: the local scan runs (its pulse would take the wave over)
    wait_wave,        // the wave still expands (a scan shown before: local or the partner's)
    wait_survey,      // the survey component still shows a scan
    wait_no_wave,     // the wave pointer is null (not created yet)
    bad_state,        // an unknown state: dropped
};

struct StartIn {
    bool ok = false;          // the inventory reads succeeded
    int state = 0;
    bool wave = false;
    bool wave_done = false;   // wave +0x2BD
    bool survey_active = false;
};

inline StartVerdict RemoteStart(const StartIn& in) noexcept {
    if (!in.ok) return StartVerdict::wait_no_wave;
    if (in.state >= kArm && in.state <= kExpand) return StartVerdict::wait_local_scan;
    if (in.state != kIdle && in.state != kMarks) return StartVerdict::bad_state;
    if (!in.wave) return StartVerdict::wait_no_wave;
    if (!in.wave_done) return StartVerdict::wait_wave;
    if (in.survey_active) return StartVerdict::wait_survey;
    return in.state == kIdle ? StartVerdict::start_full : StartVerdict::start_visual;
}

inline bool Started(StartVerdict v) noexcept { return v == StartVerdict::start_full || v == StartVerdict::start_visual; }
inline bool Waits(StartVerdict v) noexcept {
    return v == StartVerdict::wait_local_scan || v == StartVerdict::wait_wave || v == StartVerdict::wait_survey || v == StartVerdict::wait_no_wave;
}
// The steps the detour runs: the requested mask, less kStartSystem in state 5.
inline std::uint32_t StartSteps(StartVerdict v, std::uint32_t mask) noexcept {
    if (v == StartVerdict::start_full) return mask & kStartAll;
    if (v == StartVerdict::start_visual) return mask & kStartVisual;
    return 0;
}

inline const char* StartName(StartVerdict v) noexcept {
    switch (v) {
        case StartVerdict::start_full: return "full";
        case StartVerdict::start_visual: return "visual";
        case StartVerdict::wait_local_scan: return "busy_local_scan";
        case StartVerdict::wait_wave: return "wave_running";
        case StartVerdict::wait_survey: return "busy_survey";
        case StartVerdict::wait_no_wave: return "no_wave";
        case StartVerdict::bad_state: return "bad_state";
    }
    return "?";
}

// ---- the one-slot queue (latest wins): a newer scan replaces a pending one; a pending one expires after kPendingTtlMs.
inline bool Supersedes(bool test_new, bool test_old, std::uint16_t new_seq, std::uint16_t old_seq) noexcept {
    if (test_new || test_old) return true;  // a hidden-run scan always replaces (and is replaced)
    return SeqCompare(new_seq, old_seq) > 0;
}
inline bool PendingExpired(double age_ms) noexcept { return !(age_ms <= kPendingTtlMs); }

// The wave's transform from the scan origin and direction (rows right, forward, up, as the player's +0xE0): forward = (nx, ny,
// 0), right = forward x up = (ny, -nx, 0), up = (0, 0, 1).
inline void RotFromDir(float nx, float ny, float (&m)[3][3]) noexcept {
    m[0][0] = ny, m[0][1] = -nx, m[0][2] = 0.0f;
    m[1][0] = nx, m[1][1] = ny, m[1][2] = 0.0f;
    m[2][0] = 0.0f, m[2][1] = 0.0f, m[2][2] = 1.0f;
}

// The sender: a pulse goes out only to a partner that announced the capability, with the key on here, on a live link whose
// channel 6 the partner is known to take (its SEH1 beacon seen: an older lab-mode peer drops channel 6 unacknowledged).
struct SendIn {
    bool on = false;          // the key, the detour installed, not stopped
    bool connected = false;
    bool channel_ok = false;  // the partner's SEH1 beacon seen (shots::PeerBeacon)
    bool peer_cap = false;    // the partner's HELLO carried kCapOdradekScan
    bool announced = false;   // a HELLO with the bit went out on this link generation (a later caps 0 after a fault must follow)
};
// With the key off nothing is ever sent (0 SOD1 traffic); a caps change after a HELLO (a fault stopped the detour) is announced.
inline bool MaySendHello(const SendIn& in) noexcept { return (in.on || in.announced) && in.connected && in.channel_ok; }
inline bool MaySendScan(const SendIn& in) noexcept { return in.on && in.connected && in.channel_ok && in.peer_cap; }

}  // namespace st::odrarules
