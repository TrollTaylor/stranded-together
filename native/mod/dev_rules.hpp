// Dev tools (docs/debug-actions.md "Dev tools"), the pure part: which tool may run in which session, the vehicle kinds and
// variants the game knows, the weather presets, the place list's labels. No game memory, no Windows: unit-tested in
// tests/dev_rules_tests.cpp. The game side is dev_tools.cpp (and vehicle_share.cpp for the vehicles).
//
// The real-save rule: a tool that changes what a save keeps (a vehicle, an enemy, the weather, the inventory) runs only in a
// game whose saves are sandboxed (the launcher's Local test with a test save, a hidden run: automation::SavesRedirected()).
// Never in a session that plays on a real save. Player-state tools (refill, god mode, infinite ammo, teleports) run in any
// session once the dev tools are on, like the debug menu's existing Heal and Teleport.
#pragma once
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
        if (t == Tool::vehicle_spawn) return v.route = Route::ask_host, v;
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

}  // namespace st::devrules
