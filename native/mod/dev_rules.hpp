// Dev tools (docs/debug-actions.md "Dev tools"), the pure part: which tool may run in which session, the vehicle kinds and
// variants the game knows, the weather presets, the place list's labels. No game memory, no Windows: unit-tested in
// tests/dev_rules_tests.cpp. The game side is dev_tools.cpp (and vehicle_share.cpp for the vehicles).
//
// The real-save rule: a tool that changes what a save keeps (a vehicle, an enemy, the weather, the inventory) runs only in a
// game whose saves are sandboxed (the launcher's Local test with a test save, a hidden run: automation::SavesRedirected()).
// Never in a session that plays on a real save. Player-state tools (refill, god mode, infinite ammo, teleports) run in any
// session once the dev tools are on, like the debug menu's existing Heal and Teleport.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace st::devrules {

// What a tool touches.
enum class Tool : std::uint8_t {
    player,         // our Sam only (refill, god mode, ammo, teleport): any session with the dev tools on
    world,          // this game's own world or inventory (weather, MULEs, BTs, boots, chiral crystals)
    vehicle_spawn,  // a new vehicle (in a host-held save session it lives in the host's world: the host's game decides)
    vehicle_edit,   // an existing vehicle (remove, repair)
    structure,      // a structure (dev_build ...; docs/world-sync.md): in a host-held save session it lives in the host's world
};

struct Session {
    bool enabled = false;           // dev_tools=1 in session.ini (the launcher's Debug page), or a hidden run
    bool saves_redirected = false;  // this game's saves are sandboxed (Local test / test save / hidden run / co-op guest)
    int role = 0;                   // 0 solo / echo / none, 1 host, 2 guest
    bool host_save = false;         // the host-held save session (docs/coop-session.md): the host's world is the kept one
};

enum class Route : std::uint8_t {
    refused,     // `why` says why
    local,       // runs in this game's world
    local_only,  // runs in this game's world only: the partner's world does not see it (the guest's own copy)
    ask_host,    // the guest asks the host's game, which decides with its own sandbox state (a refusal comes back)
};
struct Verdict {
    Route route = Route::refused;
    const char* why = "";
    bool ok() const noexcept { return route != Route::refused; }
};

inline constexpr const char* kOffReason = "dev tools are off (launcher Debug page: Dev tools, or dev_tools=1 in session.ini)";
inline constexpr const char* kRealSaveReason =
    "refused: this game plays on its real save and this changes what the save keeps (use the launcher's Local test with a test save)";
inline constexpr const char* kGuestEditReason =
    "refused on the guest: shared vehicles live in the host's world (use Dev tools on the host's game)";
inline constexpr const char* kHostRealReason = "refused by the host: its game plays on its real save";

inline Verdict Decide(Tool t, const Session& s) noexcept {
    Verdict v;
    if (!s.enabled) return v.why = kOffReason, v;
    if (t == Tool::player) return v.route = Route::local, v;
    const bool coop_guest = s.role == 2 && s.host_save;
    if (coop_guest) {
        // the guest's game always runs on its sandbox; its world is a copy of the host's save and is never kept
        if (t == Tool::vehicle_spawn || t == Tool::structure) return v.route = Route::ask_host, v;
        if (t == Tool::vehicle_edit) return v.why = kGuestEditReason, v;
        if (!s.saves_redirected) return v.why = kRealSaveReason, v;  // DllMain refuses this session anyway
        return v.route = Route::local_only, v;
    }
    if (!s.saves_redirected) return v.why = kRealSaveReason, v;
    // a host-held save session's host: a vehicle goes to the guest too (the M2 shared vehicles); the rest is the host's only
    if (s.role == 1 && s.host_save && t == Tool::world) return v.route = Route::local_only, v;
    // two separate worlds (host_save=0, or no partner)
    if (s.role == 1 || s.role == 2) return v.route = s.host_save ? Route::local : Route::local_only, v;
    return v.route = Route::local, v;
}

// The host's answer to a guest's dev vehicle request (vehicle_wire ReqReason::dev).
inline bool HostAllowsDevRequest(bool host_saves_redirected) noexcept { return host_saves_redirected; }

// ---------------------------------------------------------------- vehicles
// The 14 kind / variant pairs of the game's vehicle catalogue table (0x144D7F880: {u32 item id, i32 kind, i32 variant},
// read by KindVariantToItem 0x14329EA80 up to 0x144D7F928).
struct VehiclePair {
    int kind;
    int variant;
};
inline constexpr VehiclePair kVehiclePairs[] = {
    {1, 0}, {1, 1},                                          // roadster
    {2, 0}, {2, 1}, {2, 2}, {2, 4}, {2, 5}, {2, 6},          // truck
    {3, 0}, {3, 1}, {3, 2}, {3, 3}, {3, 7}, {3, 8},          // trike
};
inline constexpr std::size_t kVehiclePairCount = sizeof(kVehiclePairs) / sizeof(kVehiclePairs[0]);

inline bool ValidVehicle(int kind, int variant) noexcept {
    for (const VehiclePair& p : kVehiclePairs)
        if (p.kind == kind && p.variant == variant) return true;
    return false;
}
inline const char* KindName(int kind) noexcept {
    return kind == 1 ? "roadster" : kind == 2 ? "truck" : kind == 3 ? "trike" : "?";
}
// The spawn record's +0 (0x142F830A0): the pool in bits 0..3, the variant in bits 6..11.
inline std::uint16_t VehicleRec0(int kind, int variant) noexcept {
    return static_cast<std::uint16_t>((kind & 0xF) | ((variant & 0x3F) << 6));
}
// A place `dist` metres ahead along the facing (fx, fy) (Sam's rotation row 1).
inline void Ahead(const double (&pos)[3], double fx, double fy, double dist, double (&out)[3]) noexcept {
    out[0] = pos[0] + fx * dist;
    out[1] = pos[1] + fy * dist;
    out[2] = pos[2] + 0.5;
}

// ---------------------------------------------------------------- weather (DS forecast types, EDSWeatherForecastType)
enum class Weather : std::uint8_t { rain = 3, heavy = 5, clear = 6, normal = 2 };  // RainyOnly, HeavyRainOnly, SunnyOnly, Average
inline constexpr int kWeatherRegions = 64;  // EDSWeatherRegionIndex: OutOfRegion, Region01..63
inline bool WeatherFromCode(int code, Weather* out) noexcept {
    switch (code) {
        case 0: *out = Weather::clear; return true;
        case 1: *out = Weather::rain; return true;
        case 2: *out = Weather::heavy; return true;
        case 3: *out = Weather::normal; return true;
        default: return false;
    }
}
inline const char* WeatherName(Weather w) noexcept {
    switch (w) {
        case Weather::rain: return "timefall (rain only)";
        case Weather::heavy: return "heavy timefall";
        case Weather::clear: return "clear (sunny only)";
        case Weather::normal: return "the game's mix (average)";
    }
    return "?";
}

// ---------------------------------------------------------------- god mode (the retail DebugSettings EGodMode)
inline bool ValidGodMode(int m) noexcept { return m >= 0 && m <= 2; }
inline const char* GodModeName(int m) noexcept { return m == 0 ? "off" : m == 1 ? "on" : m == 2 ? "invulnerable" : "?"; }

// ---------------------------------------------------------------- places
// The delivery point types (EDSConstructionPointType, read from the game's RTTI at runtime): a short word for the list.
inline const char* PlaceKind(int type) noexcept {
    switch (type) {
        case 0: return "delivery base";
        case 1: return "prepper shelter";
        case 2: case 3: case 4: return "safe house";
        case 5: case 6: case 7: return "post box";
        case 20: return "MULE post";
        default: return "place";
    }
}
// "<name>" or, without a readable name, "<kind> #<order>". Returns the length written.
inline std::size_t PlaceLabel(const char* name, int type, std::uint32_t order, char* out, std::size_t cap) noexcept {
    if (!out || !cap) return 0;
    int n = 0;
    if (name && name[0]) n = std::snprintf(out, cap, "%s", name);
    else n = std::snprintf(out, cap, "%s #%u", PlaceKind(type), order);
    return n < 0 ? 0 : static_cast<std::size_t>(n) < cap ? static_cast<std::size_t>(n) : cap - 1;
}
// A name read from game memory is shown only when it is printable ASCII (1..63 chars).
inline bool PrintableName(const char* s, std::size_t max) noexcept {
    if (!s || !s[0]) return false;
    std::size_t i = 0;
    for (; i < max && s[i]; ++i)
        if (static_cast<unsigned char>(s[i]) < 0x20 || static_cast<unsigned char>(s[i]) > 0x7E) return false;
    return i < max && i < 64;
}

// ---------------------------------------------------------------- dev_goto (a place anywhere in this area: sandboxed saves only)
// The settle rule the game thread runs every tick after the teleport (dev_tools.cpp): Sam within kGotoNearM of x, y (25 m: a place's
// own point may sit in a terminal or a wall, and the game pushes Sam out of it: 3.5 m at South Knot City, run
// 20261121-082806-goto-places; 8.3 m at the Weather Station, 20261121-084340; a teleport that did not take is kilometres off) (and, with a
// known z, within kGotoHighM of it), playable (the caller's gates), and still (z and x, y within kGotoStillM) for kGotoStillMs.
// Below z by more than kGotoFallM (the ground was not streamed in yet: he fell through) he is placed again, at most
// kGotoMaxPlacements times, kGotoRetryMs apart. kGotoTimeoutMs without that: failed.
// snap: the game's topmost-ground snap (PlaceOn mode 2) at a known reference height z (dev_place's last candidate): the
// topmost surface may sit well above or below that height, so "at" and "fell" use kGotoSnapSlackM instead.
inline constexpr double kGotoNearM = 25.0, kGotoHighM = 5.0, kGotoFallM = 5.0, kGotoStillM = 0.3, kGotoSnapSlackM = 30.0;
inline constexpr double kGotoStillMs = 1500, kGotoRetryMs = 1000, kGotoTimeoutMs = 25000;
inline constexpr int kGotoMaxPlacements = 4;
// z == 0: "the ground under x, y" (the game's snap, PlaceOn mode 2).
inline bool GotoTargetOk(double x, double y, double z) noexcept {
    const auto finite = [](double v) { return v == v && v - v == 0; };
    return finite(x) && finite(y) && finite(z) && x > -50000 && x < 50000 && y > -50000 && y < 50000 && z > -10000 && z < 10000;
}
struct GotoTrack {
    double x = 0, y = 0, z = 0;  // the target (z 0: the ground)
    double t0 = 0;               // the first placement
    double still_since = -1;     // the still window's start (-1: not still)
    double ref[3] = {};          // the place the window started at
    double last_place = 0;       // the newest placement
    int placements = 1;
    bool snap = false;           // mode 2 at the reference height z (not a z 0 goto)
};
// Sam is below a known target height by more than the fall margin: he fell through ground that was not streamed in yet, or was
// placed inside a hill. Never for z 0 (no height known).
inline bool GotoFell(const GotoTrack& t, const double (&pos)[3]) noexcept {
    return t.z != 0 && pos[2] < t.z - (t.snap ? kGotoSnapSlackM : kGotoFallM);
}
enum class GotoStep : std::uint8_t { wait, settled, place_again, timeout };
inline GotoStep GotoAdvance(GotoTrack& t, bool playable, const double (&pos)[3], double now_ms) noexcept {
    const double dx = pos[0] - t.x, dy = pos[1] - t.y;
    const bool close_xy = dx * dx + dy * dy <= kGotoNearM * kGotoNearM;
    const bool ground = t.z == 0;
    const bool fell = GotoFell(t, pos);
    if (close_xy && fell && t.placements < kGotoMaxPlacements && now_ms - t.last_place >= kGotoRetryMs) {
        ++t.placements;
        t.last_place = now_ms;
        t.still_since = -1;
        return GotoStep::place_again;
    }
    const double high = t.snap ? kGotoSnapSlackM : kGotoHighM;
    const bool at = close_xy && (ground || (pos[2] - t.z <= high && t.z - pos[2] <= high));
    const auto moved = [&]() {
        const double ax = pos[0] - t.ref[0], ay = pos[1] - t.ref[1], az = pos[2] - t.ref[2];
        return ax * ax + ay * ay + az * az > kGotoStillM * kGotoStillM;
    };
    if (!playable || !at) {
        t.still_since = -1;
    } else if (t.still_since < 0 || moved()) {
        t.still_since = now_ms;
        t.ref[0] = pos[0], t.ref[1] = pos[1], t.ref[2] = pos[2];
    } else if (now_ms - t.still_since >= kGotoStillMs) {
        return GotoStep::settled;
    }
    return now_ms - t.t0 >= kGotoTimeoutMs ? GotoStep::timeout : GotoStep::wait;
}

// ---------------------------------------------------------------- place landing (dev_place: never inside a post box / MULE post)
// A delivery point's own transform is the ORIGIN of its structure (the construction point): a post box's or MULE post's
// collision encloses it and the game does not push Sam out (round 18). The stand point is the game's own "where the player
// stands to use the terminal": GetConstructionPointFrontAccessTransform (0x142CD80E0) = the construction point's transform
// (cp+0x468 position, cp+0x480 rotation rows: right, forward, up) moved by its config's TerminalFrontAccessOffset
// (config+0x2D0, local x / y / z along those rows). Without it (no construction point found, a zero offset, an odd result):
// points around the place at a radius from its config's collision box, forward first.
// The kind group of a place type (the Dev tab's "kind" filter): post boxes 5..7 are one group, safe houses 2..4 another.
inline int PlaceGroup(int type) noexcept { return type >= 5 && type <= 7 ? 5 : type >= 2 && type <= 4 ? 2 : type; }
// dev_place's filter: 0 any place, -1 delivery bases (type 0), else the group of that type (5 any post box, 20 MULE post).
inline bool PlaceFilterMatches(int filter, int type) noexcept {
    if (filter == 0) return true;
    return PlaceGroup(filter < 0 ? 0 : filter) == PlaceGroup(type);
}
// The small structures whose collision traps Sam at their origin.
inline bool SmallPost(int type) noexcept { return PlaceGroup(type) == 5 || type == 20; }
inline bool Finite(double v) noexcept { return v == v && v - v == 0; }
// out = p + o.x * row0 + o.y * row1 + o.z * row2 (r: 9 floats, rows right, forward, up) - the game's math (0x142CD80E0).
inline void FrontAccess(const double (&p)[3], const float (&r)[9], const float (&o)[3], double (&out)[3]) noexcept {
    for (int i = 0; i < 3; ++i) out[i] = p[i] + static_cast<double>(o[0]) * r[i] + static_cast<double>(o[1]) * r[3 + i] + static_cast<double>(o[2]) * r[6 + i];
}
// A stand point is used when it is finite, kStandMinM..kStandMaxM from the place (flat) and within kStandDzM of its height.
inline constexpr double kStandMinM = 0.8, kStandMaxM = 12.0, kStandDzM = 4.0;
inline bool StandPointOk(const double (&place)[3], const double (&s)[3]) noexcept {
    if (!Finite(s[0]) || !Finite(s[1]) || !Finite(s[2])) return false;
    const double dx = s[0] - place[0], dy = s[1] - place[1], dz = s[2] - place[2];
    const double d = dx * dx + dy * dy;
    return d >= kStandMinM * kStandMinM && d <= kStandMaxM * kStandMaxM && dz <= kStandDzM && -dz <= kStandDzM;
}
// The config's collision box (local min / max) is usable when finite, ordered and 5 cm .. 50 m on every axis.
inline bool BoxUsable(const float (&mn)[3], const float (&mx)[3]) noexcept {
    for (int i = 0; i < 3; ++i) {
        if (!Finite(mn[i]) || !Finite(mx[i])) return false;
        const double e = static_cast<double>(mx[i]) - mn[i];
        if (!(e >= 0.05 && e <= 50.0)) return false;
    }
    return true;
}
// The fallback ring: the box's flat half-size (its farthest x / y side from the origin, under 15 m) + kFallbackClearM, else
// 1.5 m (post box / MULE post) or 4 m (any other place) + kFallbackClearM.
inline constexpr double kFallbackClearM = 1.2;
inline double FallbackRadius(int type, bool box_ok, const float (&mn)[3], const float (&mx)[3]) noexcept {
    double half = 0;
    if (box_ok) {
        const double v[4] = {mn[0] < 0 ? -mn[0] : mn[0], mx[0] < 0 ? -mx[0] : mx[0], mn[1] < 0 ? -mn[1] : mn[1], mx[1] < 0 ? -mx[1] : mx[1]};
        for (double x : v) half = x > half ? x : half;
        if (!(half > 0.05 && half < 15.0)) half = 0;
    }
    if (half == 0) half = SmallPost(type) ? 1.5 : 4.0;
    return half + kFallbackClearM;
}
// The landing candidates, best first: the stand point (when ok), then the ring: +forward, -forward, +right, -right, then
// +forward at twice the radius on the topmost ground (snap). `centre`: the structure's own position (its construction point's
// when found, else the place's); rows: its rotation (9 floats: right, forward, up). The ring keeps the centre's height (z),
// probed by PlaceOn mode 1 (z + 2.5 .. z - 1.5): on a slope steeper than that, or over a drop, Sam falls (GotoFell) and the
// next candidate is tried (LandAfterFall); the last one is the game's topmost-ground snap, which needs no height.
inline constexpr int kMaxLandings = 7;  // 6 + one spare: dev_place's hidden-run test candidate (the structure's own point) goes first
struct Landing {
    double pos[3] = {};
    const char* rule = "";
    bool snap = false;  // PlaceOn mode 2 (the topmost ground at x, y); pos[2] is only the reference height
};
struct LandPlan {
    int n = 0;
    Landing c[kMaxLandings];
};
inline LandPlan PlanLanding(const double (&centre)[3], const float (&r)[9], bool stand_ok, const double (&stand)[3], double radius) noexcept {
    LandPlan p;
    if (stand_ok) {
        for (int i = 0; i < 3; ++i) p.c[p.n].pos[i] = stand[i];
        p.c[p.n++].rule = "front access";
    }
    // flat unit forward / right (the rows may tilt with the slope the structure stands on)
    double fx = r[3], fy = r[4], rx = r[0], ry = r[1];
    double fn = fx * fx + fy * fy, rn = rx * rx + ry * ry;
    if (!(fn > 1e-6) || !Finite(fn)) fx = 0, fy = 1, fn = 1;
    if (!(rn > 1e-6) || !Finite(rn)) rx = fy, ry = -fx, rn = fx * fx + fy * fy;
    const double fl = 1.0 / std::sqrt(fn), rl = 1.0 / std::sqrt(rn);
    const double dirs[4][2] = {{fx * fl, fy * fl}, {-fx * fl, -fy * fl}, {rx * rl, ry * rl}, {-rx * rl, -ry * rl}};
    const char* rules[4] = {"fallback +forward", "fallback -forward", "fallback +right", "fallback -right"};
    for (int k = 0; k < 4 && p.n < kMaxLandings; ++k) {
        p.c[p.n].pos[0] = centre[0] + dirs[k][0] * radius;
        p.c[p.n].pos[1] = centre[1] + dirs[k][1] * radius;
        p.c[p.n].pos[2] = centre[2];
        p.c[p.n++].rule = rules[k];
    }
    if (p.n < kMaxLandings) {
        p.c[p.n].pos[0] = centre[0] + dirs[0][0] * radius * 2;
        p.c[p.n].pos[1] = centre[1] + dirs[0][1] * radius * 2;
        p.c[p.n].pos[2] = centre[2];
        p.c[p.n].snap = true;
        p.c[p.n++].rule = "fallback far +forward, topmost ground";
    }
    return p;
}
// The candidate walk (dev_tools.cpp GotoTick / PlaceAfterSettle). `next`: the index of the candidate after the one placed now;
// `n`: the plan's size. stay: keep this candidate (settled outside every box / placed again at the same point).
enum class LandStep : std::uint8_t { stay, next, give_up };
// After the goto settled: inside a structure's box (its own or a small neighbour's) -> the next candidate, none left: give up.
inline LandStep LandAfterSettle(bool inside, int next, int n) noexcept {
    return !inside ? LandStep::stay : next < n ? LandStep::next : LandStep::give_up;
}
// After Sam fell at a candidate (placements: the tracker's count after the fall, 2 = its first re-placement) or the goto timed
// out with him fallen (timed_out): the first fall is placed again at the same point (a far place: the ground streams in), a
// second one or a fallen timeout moves on (the point is inside a hill or over a drop). None left: the same point again while
// the tracker allows it; give up on a timeout.
inline LandStep LandAfterFall(int placements, bool timed_out, int next, int n) noexcept {
    if (!timed_out && placements <= 2) return LandStep::stay;
    if (next < n) return LandStep::next;
    return timed_out ? LandStep::give_up : LandStep::stay;
}
// Whether Sam (his feet at `sam`; the test point is kBodyUpM above them, his middle) is inside the structure's collision box
// (local min / max, shrunk by kBoxShrinkM) given its transform (position p, rotation rows r). `local` gets his middle's local
// coordinates.
inline constexpr double kBodyUpM = 0.9, kBoxShrinkM = 0.1;
inline bool InsideBox(const double (&sam)[3], const double (&p)[3], const float (&r)[9], const float (&mn)[3], const float (&mx)[3],
                      double (&local)[3]) noexcept {
    const double d[3] = {sam[0] - p[0], sam[1] - p[1], sam[2] + kBodyUpM - p[2]};
    for (int i = 0; i < 3; ++i) local[i] = d[0] * r[3 * i] + d[1] * r[3 * i + 1] + d[2] * r[3 * i + 2];
    for (int i = 0; i < 3; ++i)
        if (!(local[i] > mn[i] + kBoxShrinkM && local[i] < mx[i] - kBoxShrinkM)) return false;
    return true;
}

// ---------------------------------------------------------------- map markers (Teleport to map marker)
// The player's own map markers (the map screen's route pins: routes A..E, numbers 0..9; the game's user-marker pool,
// dev_tools.cpp). The newest is the active one with the largest id (ids count up in placement order, from 0). A marker's z
// is the terrain height once the game's heightmap query (+0x42) is done; before that it is not trusted as a height: the
// topmost ground under x, y (the same topmost-hit rule as the game's own marker raycast). Run 20261124-183256-marker-teleport:
// a new marker 175 m away had its z moved to the terrain height within 2 s but kept the raycast byte (+0x41) set, as did 5 of
// the save's 7 markers 1-1.8 km away (the game's update casts that ray only under conditions not mapped yet): those use their
// terrain z (MarkerTargetZ).
struct Marker {
    int slot = -1;            // pool index (letter * 10 + number)
    int letter = 0, number = 0;
    double pos[3] = {};
    std::uint64_t id = 0;
    bool snapped = false;     // the game's ground snap is done: z is the ground
    int pending = 0;          // 1: the game's raycast (+0x41) pending, 2: its heightmap query (+0x42) pending
    int area = -1;            // the area the mod first saw it in (-1 unknown)
};
inline bool MarkerSlotOk(int letter, int number) noexcept { return letter >= 0 && letter < 5 && number >= 0 && number < 10; }
// "C0": the route letter and the number, as the map screen shows them.
inline void MarkerName(int letter, int number, char (&out)[4]) noexcept {
    out[0] = MarkerSlotOk(letter, number) ? static_cast<char>('A' + letter) : '?';
    out[1] = MarkerSlotOk(letter, number) ? static_cast<char>('0' + number) : '?';
    out[2] = 0, out[3] = 0;
}
// A marker the mod saw in another area is not on this map. Unknown (-1, either side): allowed.
inline bool MarkerAreaOk(int marker_area, int current_area) noexcept { return marker_area < 0 || current_area < 0 || marker_area == current_area; }
// The game's GetCurrentArea as an area: 0xFFFF (none) and a fault (-1) are unknown (-1).
inline int KnownArea(int raw) noexcept { return raw >= 0 && raw != 0xFFFF ? raw : -1; }
// An area for the player: the map's region where known (100 the east, 200 the central region: Mountain / Lake Knot City), with
// the game's own name (game::AreaName, passed in: "Area02"), else "area <n>".
inline void AreaText(int area, const char* internal, char* out, std::size_t cap) noexcept {
    if (!out || !cap) return;
    const char* region = area == 100 ? "the Eastern Region" : area == 200 ? "the Central Region" : nullptr;
    if (area < 0) std::snprintf(out, cap, "an unknown area");
    else if (region) std::snprintf(out, cap, "%s (%s)", region, internal ? internal : "?");
    else if (internal) std::snprintf(out, cap, "%s", internal);
    else std::snprintf(out, cap, "area %d", area);
}
// want_id >= 0: the marker with that id (ids start at 0); < 0: the newest (largest id) of those on this map (MarkerAreaOk with
// current_area), else the newest of all (its refusal then names the other area). -1 when there is none.
inline int PickMarker(const Marker* m, int n, std::int64_t want_id, int current_area = -1) noexcept {
    if (want_id >= 0) {
        for (int i = 0; i < n; ++i)
            if (m[i].id == static_cast<std::uint64_t>(want_id)) return i;
        return -1;
    }
    int best = -1, best_here = -1;
    for (int i = 0; i < n; ++i) {
        if (best < 0 || m[i].id > m[best].id) best = i;
        if (MarkerAreaOk(m[i].area, current_area) && (best_here < 0 || m[i].id > m[best_here].id)) best_here = i;
    }
    return best_here >= 0 ? best_here : best;
}
// The action's v0 (Teleport to map marker): 0 the newest, else the marker's id + 1 (ids start at 0).
inline std::int64_t MarkerWant(double v0) noexcept { return v0 >= 1 && v0 < 9.0e15 ? static_cast<std::int64_t>(v0) - 1 : -1; }
// The goto's z: the marker's own once the game's heightmap query is done (+0x42 back to 0: z is the terrain height; run
// 20261124-183504 matched the ground within 0.8 m at 175 m and 1 km) and it is a sane height: PlaceOn mode 1 there, placed
// again if Sam falls through ground not streamed in yet. While that query is pending: 0 (the game's topmost-ground snap).
inline double MarkerTargetZ(const Marker& m) noexcept {
    return !(m.pending & 2) && Finite(m.pos[2]) && m.pos[2] > -10000 && m.pos[2] < 10000 && m.pos[2] != 0 ? m.pos[2] : 0.0;
}
// The mod's area tag of a pool slot belongs to the marker seen there now: the same id at the same place (flat, within
// kMarkerSameM: the game snaps only z). Ids restart per save, so another save's marker in that slot is told apart by place.
inline constexpr double kMarkerSameM = 2.0;
inline bool SameMarker(std::uint64_t id0, double x0, double y0, std::uint64_t id1, double x1, double y1) noexcept {
    const double dx = x1 - x0, dy = y1 - y0;
    return id0 == id1 && dx * dx + dy * dy <= kMarkerSameM * kMarkerSameM;
}

// ---------------------------------------------------------------- boots (the game's SetShoesLifeByRate: rate x (max + spare))
// dev_boots 1 <percent>: 0..100 -> the rate 0..1 (0 = worn through).
inline bool BootsRate(double percent, float* rate) noexcept {
    if (!(percent >= 0 && percent <= 100)) return false;
    *rate = static_cast<float>(percent / 100.0);
    return true;
}

}  // namespace st::devrules
