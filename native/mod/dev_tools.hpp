// Dev tools: spawning, world control and player cheats for play-testing and hidden test runs (docs/debug-actions.md
// "Dev tools"). The rules are dev_rules.hpp (which tool runs in which session); the vehicles are vehicle_share.cpp's
// DevSpawn / DevRemove / DevRepair; this file is the rest (refill, god mode, infinite ammo, weather, MULEs, BTs, boots,
// chiral crystals, the place list) and the one entry the debug actions call.
//
// Threads: Command and Tick on the game thread (st::debug::OnTick); GetView on any thread (the overlay's Dev tools page).
// Every game address is byte-checked on the first tick; a mismatch turns that tool off with one log line and the reason on
// the page. Game calls are SEH-wrapped as a last resort only: every object is validated (vtable / RTTI / non-null) first.
#pragma once
#include <cstddef>
#include <cstdint>

#include "dev_rules.hpp"

namespace st {
struct Config;
}

namespace st::dev {

// Bootstrap thread, once: dev_tools=1 in session.ini (or a hidden run, automation=1) turns the tools on.
void Install(const Config& config) noexcept;
bool Enabled() noexcept;
// Host (vehicle_share's HostServeRequest): whether a guest's dev vehicle request may spawn in this world: the dev tools on
// here too and this game's saves sandboxed (the hidden-run real-save override counts). nullptr = yes, else why not.
const char* HostRefusesGuestDev() noexcept;
// Any thread: nullptr when this game's saves are sandboxed (automation::SavesRedirected(), the hidden-run real-save override
// counts as a real save), else devrules::kRealSaveReason. For the actions outside the dev tools that change world state a save
// keeps (the Player tab's time of day and day/night cycle): they run with the dev tools off too, but never on a real save.
const char* RealSaveRefusal() noexcept;
// Any thread: the dev rules' verdict for a tool class in this session now (world_share.cpp's structure tools).
devrules::Verdict DecideNow(devrules::Tool t) noexcept;

// Game thread, every tick (debug::OnTick): the byte checks (once), the place list and the view (2 Hz), the vehicle check.
void Tick(double now_ms) noexcept;

// Operations (the debug actions' handlers, docs/debug-actions.md "Dev tools"). v = the request's numbers.
enum class Op : std::uint8_t {
    status,          // v0 = 0 log; 1 v1 = 1 test: act as if this game played on its real save (hidden runs only), 0 back;
                     // 2 the open menu shows the Dev tools page (hidden runs); 3 v1 Sam v1 m up (a fall test, hidden runs)
    vehicle,         // v0 kind, v1 variant, v2 metres ahead (0 = 8)
    vehicle_remove,  // v0 max metres (0 = 15); v1 = 1: the newest dev vehicle instead of the nearest
    vehicle_fix,     // v0 max metres (0 = 10)
    refill,          // health, stamina, battery, blood (DSPlayerSystem RecoverFull)
    god,             // v0 = 0 off, 1 on, 2 invulnerable
    ammo,            // v0 = 0 / 1
    weather,         // v0 = 0 clear, 1 timefall, 2 heavy timefall, 3 the game's mix
    enemies,         // v0 = 1 resurrect every MULE / terrorist, 2 remove them from the world, 3 reset the BTs
    places,          // log the current area's places
    place,           // v0 = index in the place list: teleport there
    items,           // v0 = 1 new boots, 2 chiral crystals +v1 (0 = 100)
    go_to,           // v0 x, v1 y, v2 z (0 = the topmost ground under x, y): Sam there, then waits for the ground (sandboxed saves
                     // only: a place anywhere in the world); "[dev] goto #n: settled ..." / "NOT settled" (GetGotoState)
    boots,           // v0 = 0 log the boots' life; 1 v1 = percent: set it (the game's SetShoesLifeByRate; sandboxed saves only);
                     // 2 v1 = 0/1: a 1 s "[dev] boots meter" line (hidden tests: the passenger's boots)
};
bool Command(Op op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

// What the Dev tools page shows (a copy; published by Tick).
struct Place {
    char label[48] = {};
    double pos[3] = {};
    float dist_m = 0;
};
struct View {
    std::uint64_t serial = 0;
    bool enabled = false;
    bool test_real = false;         // the hidden-run override of status 1 is on
    char session[96] = {};          // "sandboxed save (Local test / hidden run)" / "real save"
    // per tool class (dev_rules.hpp Tool): ok and the reason when not
    bool ok[4] = {};
    char why[4][128] = {};
    char note[4][64] = {};          // the route's note when ok ("your world only", "the host decides", ...)
    // tools off by a byte check, with the first reason (e.g. "god mode: DebugSettings RTTI"; the vehicle tools' too)
    char off[160] = {};
    // the vehicle tools' byte checks (vshare::DevCheckResult): the pools (remove, repair), the spawn path, Repair
    bool veh_pools = true, veh_spawn = true, veh_repair = true;
    int god = -1;                   // -1 unknown
    int ammo = -1;
    int area = -1;
    int place_count = 0;
    static constexpr int kMaxPlaces = 64;
    Place places[kMaxPlaces];
};
View GetView() noexcept;

// The newest dev_goto (Op::go_to), any thread (the automation thread's "waitworld <s> goto").
struct GotoState {
    enum Phase : std::uint8_t { none, moving, settled, failed };
    Phase phase = none;
    std::uint32_t serial = 0;  // 1, 2, ... per game; 0 = none asked
    char text[160] = {};       // the settle / failure line
};
GotoState GetGotoState() noexcept;
std::uint32_t GotoSerial() noexcept;

}  // namespace st::dev
