// BT sync B0 (round 28 P3): the BT field census; see bt_sync.hpp. The pure part (hashes, prints, the archive table) is
// bt_rules.hpp. Read only: every game read goes through partner::mem::Read (ReadProcessMemory on our own process), so a stale
// pointer reads as "unreadable" and never raises; no game function is called, nothing is written, nothing is hooked.
// B1 (round 28 lane B1, bt_sync=1|2; the second half of this file): the same census once a second feeds the BTS1 relay (FIELD /
// RESET / NOTICED / VOID on channel 6), one pass-through detour on the voidout FUN_14299B1B0 (start-up, byte-checked), and with
// bt_sync=2 the host's RESET applied on the guest through ResetAllGazers (byte-checked, the game thread, SEH-guarded).
// Round 30 BT S0 (the end of the anonymous namespace): the VOID verdict (a toast only when the partner's GameOver is seen within 5 s),
// HunterAreaUp for the ride (F6), and the world probes bt_probe 9..14: hidden runs only, each lazily hooked or called on its op.
// Round 30 BT S1 / S2 (docs/enemy-sync.md section 24; the very end of the anonymous namespace): TROUBLE and GZST (the partner's
// trouble as markers, bt_markers.cpp; nothing received changes the game) and KILL (a gazer a player kills ends in both worlds,
// through bt_world.cpp's one gazer update hook).
#include "bt_sync.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#include "MinHook.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "automation.hpp"
#include "bt_markers.hpp"
#include "bt_rules.hpp"
#include "bt_world.hpp"
#include "config.hpp"
#include "coop_session.hpp"
#include "enemy_sync.hpp"
#include "game_actions.hpp"
#include "game_api.hpp"
#include "link.hpp"
#include "log.hpp"
#include "partner_source.hpp"
#include "player_sync.hpp"
#include "ride_together.hpp"
#include "shot_sync.hpp"

namespace st::bt {
namespace {

namespace mem = partner::mem;
namespace R = btrules;

constexpr std::uintptr_t kImage = 0x140000000ull;  // ds.exe VAs at its preferred base
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

// ---------------------------------------------------------------- game addresses (ds.exe Director's Cut; round 28 static answer)
constexpr std::uintptr_t kGazerGlobal = 0x147BEBEC0;    // DSGazerManager*  (EnableRespawnGazers 0x1429A76D0: mov rsi, [rip])
constexpr std::uintptr_t kRegGlobal = 0x147BBB8C0;      // the game actor registry
constexpr std::uintptr_t kHunterGlobal = 0x147BECBD0;   // the hunter manager
constexpr std::uintptr_t kCatcherGlobal = 0x147BEE060;  // the catcher manager
// the gazer manager
constexpr std::uintptr_t kMgrResource = 0x68, kMgrCount = 0x78, kMgrIds = 0x80;
constexpr std::uintptr_t kMgrConc = 0x230, kMgrConcTimer = 0x234, kMgrNoPerceive = 0x23B;
constexpr std::uintptr_t kMgrMap = 0x1750, kMgrMapCap = 0x175C;  // HashMap<u32 field id, state*>: data, capacity (map + 0xC)
constexpr std::size_t kMapSlot = 0x18;                            // +0 hash | 0x80000000 (occupied), +8 key, +0x10 value
// a field state (FUN_14299F650 registers it, FUN_1429A31A0 kills into it)
constexpr std::uintptr_t kStKilled = 0x00, kStKilledIds = 0x08, kStTimers = 0x10, kStTotal = 0x20;
// a gazer actor (DSGazerGameActor; GameActor base: +0x08 id, +0x20 in use, +0x24 state, +0x28 transform, +0x68 entity ref)
constexpr std::uintptr_t kActId = 0x08, kActC = 0x0C, kActLoc = 0x18, kActInUse = 0x20, kActState = 0x24, kActXf = 0x28;
constexpr std::uintptr_t kActEntRef = 0x68, kActFlags = 0x1B4;
// +0x204: the gazer's own distance to the LOCAL player (0x1429953EB, from FUN_14283EC70 +0x270); +0x388: its parameters, +0x98
// the near trigger radius (0x142995C3D) [V code, round 28 P3]
constexpr std::uintptr_t kActPlayerDist = 0x204, kActParams = 0x388, kParTrigger = 0x98;
// round 30 S1 (GZST): +0x1A8 the gazer's mode byte (read at 0x142995822 / 0x14299694A), +0xB4 its approach (written at
// 0x142995CFB) [V code; I meaning]
constexpr std::uintptr_t kActMode = 0x1A8, kActApproach = 0xB4;
constexpr std::size_t kActHead = 0x390;  // one read covers every field above
// the locator (DSGazerGameActorLocator): +0x80 its scene, +0x88 its GUID; the scene: +0x98 its GUID
constexpr std::uintptr_t kLocScene = 0x80, kLocGuid = 0x88, kSceneGuid = 0x98;
// the actor registry: +0x440 group count, +0x448 groups; a group: +0x30 first slot, +0x40 count, +0x48 actors
constexpr std::uintptr_t kRegGroupCount = 0x440, kRegGroups = 0x448;
constexpr std::uintptr_t kGrpFirst = 0x30, kGrpCount = 0x40, kGrpActors = 0x48;
// an entity: +0xC8 position (doubles)
constexpr std::uintptr_t kEntPos = 0xC8;
// the hunter manager
constexpr std::uintptr_t kHunResource = 0x138, kHunArea = 0x140, kHunSpawns = 0x240, kHunGold = 0x244;
// the catcher manager: records of 0x110
constexpr std::uintptr_t kCatCount = 0x88, kCatData = 0x90;
constexpr std::size_t kCatRec = 0x110;
constexpr std::uintptr_t kCatEnt = 0x00, kCatActor = 0x20, kCatInactive = 0xF0, kCatHp = 0xF4;

// ---------------------------------------------------------------- the byte checks
enum Group : unsigned {
    kMgr = 1u,       // the gazer manager's id list: without it no census
    kField = 2u,     // the field map and its states
    kActor = 4u,     // the gazer actor's fields (via the registry)
    kReg = 8u,       // the actor registry's groups
    kConc = 16u,     // the concentration meter and the not-perceivable byte
    kHunter = 32u,   // the hunter manager
    kCatcher = 64u,  // the catcher manager
};
constexpr unsigned kAllGroups = kMgr | kField | kActor | kReg | kConc | kHunter | kCatcher;
struct Site {
    std::uintptr_t va;
    const char* sig;
    const char* name;
    unsigned group;
};
// clang-format off
constexpr Site kSites[] = {
    // EnableRespawnGazers 0x1429A76D0: the manager global, its SRW lock +0x98, the ids +0x80 / count +0x78; then each id to the
    // registry; and the broadcast to actor type 8 (message 0x58305569)
    {0x1429A775F, "48 8B 35 5A 47 24 05 48 8D BE 98 00 00 00 48 8B CF FF 15 9A 2D 19 01 84 C0 75 09 48 8B CF FF 15 9D 2D 19 01 48 8B 9E "
                  "80 00 00 00 48 63 46 78 48 8D 34 83", "gazer manager id list (+0x78 / +0x80)", kMgr},
    {0x1429A7792, "48 3B DE 74 20 66 0F 1F 84 00 00 00 00 00 8B 13 48 8B 0D 17 41 21 05 E8 D2 31 95 FF", "gazer ids to the registry", kMgr | kReg},
    {0x1429A7716, "BA 08 00 00 00 48 C7 44 24 30 69 55 30 58", "gazers are actor type 8", kMgr},
    // the registry lookup (copied from enemy_census.cpp): groups +0x440 / +0x448, group +0x30 / +0x40 / +0x48, in use +0x20
    {0x1422FA980, "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 20 3B 15 B8 67 7F 02 8B DA 0F 84 86 00 00 00 8B C2 C1 E8 0D 44 0F B6 F0 "
                  "44 3B B1 B0 04 00 00 73 74 48 8B B9 48 04 00 00 48 63 81 40 04 00 00 48 8D 2C C7 48 3B FD 74 5D 0F 1F 80 00 00 00 00 48 8B "
                  "37 48 8B CE 48 8B 06 FF", "actor registry lookup (groups)", kReg},
    {0x1422FA9DA, "50 38 41 3B C6 75 1B 44 8B 46 30 8B D3 81 E2 FF 1F 00 00 41 3B D0 72 0A 8B 4E 40 41 03 C8 3B D1 72 0B 48 83 C7 08 48 3B FD "
                  "75 CB EB 1F 48 8B 46 48 81 E3 FF 1F 00 00 41 2B D8 48 63 CB 48 8B 04 C8 48 85 C0 74 06 80 78 20 00 75 02 33 C0 48 8B 5C 24 "
                  "48 48 8B 6C 24 50", "actor registry lookup (slots, in use)", kReg},
    {0x1422D6268, "48 8B 0D 51 56 8E 05 E8 0C 47 02 00 48 85 C0 74 1D 48 8B 48 68", "registry global, actor +0x68", kReg},
    // the gazer actor: GameActor transform +0x28; the kill site's +0x1B4 bit 22, +0x18 locator, +0x0C; the flag writes
    {0x141A87230, "48 8D 41 28 C3", "GameActor transform getter (+0x28)", kActor},
    {0x142995F6A, "41 8B 86 B4 01 00 00 C1 E8 16 A8 01 75 33 49 8B 46 18 48 8D 94 24 80 00 00 00 48 8B 0D 35 5F 25 05 48 89 84 24 88 00 00 00 "
                  "41 8B 46 0C 89 84 24 90 00 00 00 4C 89 A4 24 80 00 00 00", "gazer kill site (+0x1B4, +0x18, +0x0C)", kActor},
    {0x14299F3CF, "81 89 B4 01 00 00 00 00 00 04", "gazer +0x1B4 |= respawn", kActor},
    {0x14299F2D0, "81 89 B4 01 00 00 00 00 00 02", "gazer +0x1B4 |= caught", kActor},
    // the gazer update FUN_142995210 (round 28 P3): +0x204 = |gazer - local player| (FUN_14283EC70 +0x270); the near bit 0x1000 set
    // (and 0x80000 cleared) when +0x204 <= [+0x388] +0x98; flag 1 while state 2
    {0x142995390, "E8 DB 98 EA FF C4 C1 7B 10 57 08", "gazer update: the local player (FUN_14283EC70)", kActor},
    {0x1429953E0, "C4 E3 59 40 C4 7F E8 2C FC 18 01 C4 C1 7A 11 86 04 02 00 00", "gazer +0x204 (distance to the local player)", kActor},
    {0x142995C36, "49 8B 8E 88 03 00 00 C5 FA 10 81 98 00 00 00 C4 C1 78 2F 86 04 02 00 00 72 29 49 83 BE B0 03 00 00 00 74 1F 41 8B 8E "
                  "B4 01 00 00 8B C1 C1 E8 16 A8 01 75 0F 0F BA F1 13 0F BA E9 0C 41 89 8E B4 01 00 00", "gazer near bit (+0x388 +0x98 vs +0x204)", kActor},
    {0x1429956A8, "41 83 7E 24 02 75 0A 83 C9 01 41 89 8E B4 01 00 00", "gazer +0x1B4 bit 0 while state 2", kActor},
    // the field: the kill (locator +0x80 scene, crc32c of scene +0x98, the map +0x1750, its capacity, the state +0 / +8 / +0x10 /
    // +0x20, message 0x324D8FD4), the map find, the registration (total +0x20)
    {0x1429A3212, "48 8B 6E 08 48 85 ED 74 09 48 8B 8D 80 00 00 00 EB 03 48 8B CB 48 85 C9 0F 84 1D 02 00 00 4C 8B EB 48 8D 54 24 20 F2 4C 0F "
                  "38 F1 A9 98 00 00 00 45 8B ED F2 4C 0F 38 F1 A9 A0 00 00 00 48 8D 8F 50 17 00 00", "gazer kill: scene GUID crc, the field map", kField},
    {0x1429A3256, "44 89 6C 24 20 E8 30 25 FD FE 83 F8 FF 75 0A 8B 87 5C 17 00 00 8B C8 EB 06 8B 8F 5C 17 00 00 3B C1 0F 83 2D 02 00 00 48 8B "
                  "BF 50 17 00 00 48 98 48 8D 0C 40 39 1C CF 0F 84 17 02 00 00 48 8B 7C CF 10", "gazer kill: map slot +0x10", kField},
    {0x1429A3298, "48 85 ED 74 1B 48 8B F3 F2 48 0F 38 F1 B5 88 00 00 00 8B F6 F2 48 0F 38 F1 B5 90 00 00 00 EB 02 8B F3 48 63 2F 8B D3 85 ED "
                  "7E 2A 48 8B 47 08", "gazer kill: locator GUID crc, state +0 / +8", kField},
    {0x1429A33DA, "8B 4F 10 8D 41 01 89 47 10 8B 47 20 39 07 0F 85 BC 00 00 00 48 8D 4C 24 28 E8 68 83 A6 FF 48 8B 0D D9 7B 26 05 48 8D 05 EA "
                  "29 40 01 45 33 C9 48 89 44 24 28 45 33 C0 C7 44 24 44 FF FF 1F 00 48 8D 54 24 28 48 89 5C 24 50 89 5C 24 58 C7 44 24 40 08 "
                  "00 00 00 C7 44 24 48 D4 8F 4D 32", "gazer kill: timers +0x10, total +0x20, field cleared", kField},
    {0x141975790, "48 89 5C 24 08 48 89 7C 24 10 4C 8B 01 4D 85 C0 0F 84 B6 00 00 00 44 8B 51 0C 33 DB 8B 3A 41 FF CA F2 0F 38 F1 DF",
                  "u32 hash map find (data, capacity +0xC)", kField},
    {0x141975824, "42 8B 14 02 8B CA 41 23 CA 41 3B C9 7F 2A 3B D3 75 06 41 39 7B 08", "u32 hash map find (key +8)", kField},
    {0x14299F6C3, "48 8B 56 10 33 C0 48 8B 74 24 38 48 2B 74 24 40 48 C1 FE 03 F2 48 0F 38 F1 82 98 00 00 00 8B C8 F2 48 0F 38 F1 8A A0 00 00 "
                  "00 89 4C 24 30 48 8D 54 24 30 48 8D 8F 50 17 00 00 E8 8E 60 FD FE 83 F8 FF 75 06 8B 87 5C 17 00 00 48 98 48 8D 14 40 48 8B "
                  "87 50 17 00 00 48 8B 54 D0 10 89 72 20", "field registration: total +0x20", kField},
    // the manager update: the not-perceivable byte +0x23B, the concentration meter +0x230
    {0x1429A478C, "88 87 3B 02 00 00", "gazer manager +0x23B (not perceivable)", kConc},
    {0x1429A47E7, "C5 F2 58 97 30 02 00 00 C5 FA 11 97 30 02 00 00", "gazer manager +0x230 (concentration)", kConc},
    // the hunter manager: +0x138 resource, +0x140 the hunter area, +0x240 spawns / +0x244 gold; its global
    {0x1429B8D7E, "48 83 B9 38 01 00 00 00", "hunter manager +0x138", kHunter},
    {0x1429B8DC7, "48 8D BB 40 01 00 00", "hunter manager +0x140", kHunter},
    {0x1429B8DD3, "48 8B 0F 45 33 FF 48 85 C9", "hunter manager +0x140 test", kHunter},
    {0x1429B8F22, "FF 83 40 02 00 00 44 88 A3 44 02 00 00", "hunter manager +0x240 / +0x244", kHunter},
    {0x1429C4ED8, "48 8B 0D F1 7C 22 05", "hunter manager global", kHunter},
    // the catcher manager: its lock +0xE8, records +0x90 / +0x88 (0x110 each, +0xF0 inactive, +0xF4 health, +0 entity ref)
    {0x142A1505A, "48 8D 99 E8 00 00 00", "catcher manager +0xE8", kCatcher},
    {0x142A1507A, "48 8B 87 90 00 00 00 48 63 8F 88 00 00 00 48 69 D1 10 01 00 00 48 03 D0 48 3B C2 74 14 80 B8 F0 00 00 00 00",
                  "catcher records (+0x88 / +0x90, 0x110, +0xF0)", kCatcher},
    {0x142A915A9, "48 8B 35 B0 CA 15 05 48 85 F6 75 0D C5 F8", "catcher manager global", kCatcher},
    {0x142A91886, "48 69 C8 10 01 00 00 49 03 C8 74 A4 C5 FA 10 B1 F4 00 00 00", "catcher record +0xF4 (health)", kCatcher},
    {0x142A91690, "48 8B 37 48 85 F6 48 8D 46 E0", "catcher record +0 (entity ref)", kCatcher},
};
// clang-format on
struct Rip {
    std::uintptr_t insn, want;
    unsigned group;
    const char* what;
};
constexpr Rip kRips[] = {
    {0x1429A775F, kGazerGlobal, kMgr, "the gazer manager global"},
    {0x1429A77A2, kRegGlobal, kReg, "the actor registry global (gazer ids)"},
    {0x1422D6268, kRegGlobal, kReg, "the actor registry global"},
    {0x1429C4ED8, kHunterGlobal, kHunter, "the hunter manager global"},
    {0x142A915A9, kCatcherGlobal, kCatcher, "the catcher manager global"},
};

// ---------------------------------------------------------------- state (game thread only)
int g_ini = 0;            // bt_probe in session.ini
bool g_on = false;        // the census runs
bool g_autodump = false;  // ini 2: one dump when gazers first appear
double g_interval_ms = 1000.0;
double g_next_ms = 0;
bool g_checked = false;
unsigned g_groups = 0;
char g_off[160] = "";
bool g_refused_logged = false;
double g_now_ms = 0;  // the last tick's clock (the action's dump uses it)

int g_n = 0;  // censuses since the probe went on
int g_max_gazers = 0, g_max_fields = 0, g_noticed = 0, g_change_lines = 0, g_dropped_total = 0;
int g_near_events = 0, g_max_near = 0;
float g_conc_max = 0;
bool g_perceived = false;
R::LineBudget g_budget;

struct Grp {
    std::uint32_t first = 0;
    std::int32_t count = 0;
    std::uintptr_t actors = 0;
};
std::vector<Grp> g_grps;
double g_grps_ms = -1e18;

struct GazerPrev {
    std::uint32_t flags = 0;
    std::int32_t state = 0;
};
std::map<std::uint32_t, GazerPrev> g_prev_gazers;
std::map<std::uint32_t, std::int32_t> g_prev_killed;
bool g_have_prev = false;
bool g_prev_mgr = false, g_prev_hunter = false;
std::uint8_t g_prev_noperc = 0;
struct CatcherPrev {
    std::uint32_t actor = 0;
    bool active = false;
    float hp = 0;
};
std::vector<CatcherPrev> g_prev_catchers;

void Put(char* msg, std::size_t cap, const char* fmt, ...) {
    if (!msg || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, cap, fmt, ap);
    va_end(ap);
}

unsigned Check() {
    if (g_checked) return g_groups;
    g_checked = true;
    if (!mem::Base()) {
        std::snprintf(g_off, sizeof(g_off), "ds.exe base unknown");
        Log("[bt] byte checks: ds.exe base unknown (the probe refuses)");
        return g_groups = 0;
    }
    unsigned groups = kAllGroups;
    int ok = 0, total = 0;
    for (const Site& s : kSites) {
        ++total;
        if (mem::CodeMatches(A(s.va), s.sig)) {
            ++ok;
            continue;
        }
        Log("[bt] byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
        if ((s.group & kMgr) && !g_off[0]) std::snprintf(g_off, sizeof(g_off), "%s differs", s.name);
        groups &= ~s.group;
    }
    for (const Rip& r : kRips) {
        ++total;
        if (mem::RipTarget(A(r.insn), 3, 7) == A(r.want)) {
            ++ok;
            continue;
        }
        Log("[bt] byte check FAILED: %s", r.what);
        if ((r.group & kMgr) && !g_off[0]) std::snprintf(g_off, sizeof(g_off), "%s is not where expected", r.what);
        groups &= ~r.group;
    }
    if (!(groups & kMgr)) groups = 0;
    if (!(groups & kReg)) groups &= ~kActor;  // the actors are found through the registry
    g_groups = groups;
    Log("[bt] byte checks: %d/%d ok; %s%s%s%s%s%s%s", ok, total, (groups & kMgr) ? "gazer list" : "OFF (the probe refuses)",
        (groups & kReg) ? ", registry" : "", (groups & kActor) ? ", gazer fields" : "", (groups & kField) ? ", fields" : "",
        (groups & kConc) ? ", concentration" : "", (groups & kHunter) ? ", hunter" : "", (groups & kCatcher) ? ", catchers" : "");
    return g_groups;
}

// ---------------------------------------------------------------- the registry (its groups read directly, cached 5 s)
void RefreshGroups(double now_ms, bool force) {
    if (!force && !g_grps.empty() && now_ms - g_grps_ms < 5000.0) return;
    g_grps_ms = now_ms;
    g_grps.clear();
    const std::uintptr_t reg = mem::Q(A(kRegGlobal));
    std::int32_t n = 0;
    if (!reg || !mem::Rd(reg + kRegGroupCount, n) || n <= 0 || n > 512) return;
    const std::uintptr_t arr = mem::Q(reg + kRegGroups);
    if (!arr) return;
    std::vector<std::uintptr_t> ptrs(static_cast<std::size_t>(n));
    if (!mem::Read(arr, ptrs.data(), ptrs.size() * sizeof(std::uintptr_t))) return;
    for (std::uintptr_t g : ptrs) {
        if (!g) continue;
        std::uint8_t h[0x50];
        if (!mem::Read(g, h, sizeof(h))) continue;
        Grp x;
        std::memcpy(&x.first, h + kGrpFirst, 4);
        std::memcpy(&x.count, h + kGrpCount, 4);
        std::memcpy(&x.actors, h + kGrpActors, 8);
        if (x.count <= 0 || x.count > 0x2000 || x.first > 0x1FFF || !x.actors) continue;
        g_grps.push_back(x);
    }
}
std::uintptr_t Resolve(std::uint32_t id) {
    const std::uint32_t slot = R::ActorSlot(id);
    for (const Grp& g : g_grps) {
        if (slot < g.first || slot >= g.first + static_cast<std::uint32_t>(g.count)) continue;
        const std::uintptr_t a = mem::Q(g.actors + static_cast<std::uintptr_t>(slot - g.first) * 8);
        if (!a) continue;
        std::uint32_t aid = 0;
        std::uint8_t used = 0;
        if (mem::Rd(a + kActId, aid) && aid == id && mem::Rd(a + kActInUse, used) && used) return a;
    }
    return 0;
}

// ---------------------------------------------------------------- one census
struct Gazer {
    std::uint32_t id = 0, c = 0, flags = 0, loc_hash = 0, field = 0;
    std::int32_t state = -1;
    std::uint8_t mode = 0;  // round 30 S1 (GZST): +0x1A8
    float approach = 0;     // +0xB4
    float pdist = -1, trig = -1;  // +0x204 (the game's distance to the local player), [+0x388] +0x98 (the near radius)
    std::uintptr_t actor = 0;
    bool located = false, pos_ok = false;
    double pos[3] = {0, 0, 0};
};
struct Field {
    std::uint32_t id = 0;
    std::int32_t killed = 0, timers = 0, total = 0;
    std::vector<std::uint32_t> kills;
};
struct Catcher {
    std::uint32_t actor = 0;
    bool entity = false, active = false, pos_ok = false;
    float hp = 0;
    double pos[3] = {0, 0, 0};  // round 30 S1: its entity's position (record +0 entity ref - 0x20, +0xC8)
};
struct Census {
    bool mgr = false;
    std::int32_t list = 0;
    int resolved = 0, active = 0, dead = 0, near_n = 0, located = 0;
    std::vector<Gazer> g;
    std::vector<Field> f;
    bool conc_ok = false;
    float conc = 0, conc_timer = 0;
    std::uint8_t noperc = 0;
    bool hunter_ok = false, hunter_res = false, hunter_area = false;
    std::int32_t spawns = 0;
    std::uint8_t gold = 0;
    bool catchers_ok = false;
    std::vector<Catcher> c;
    bool me_ok = false, partner_ok = false;
    double me[3] = {0, 0, 0}, partner[3] = {0, 0, 0};
};

bool LocalPos(double (&out)[3]) {
    game::Entity* e = nullptr;
    game::WorldTransform xf{};
    if (!game::TryGetLocalPlayer(&e) || !e || !game::TryGetTransform(e, &xf)) return false;
    out[0] = xf.pos.x, out[1] = xf.pos.y, out[2] = xf.pos.z;
    return R::PosSane(out);
}

void ReadGazer(Gazer& z) {
    z.actor = Resolve(z.id);
    if (!z.actor) return;
    std::uint8_t h[kActHead];
    if (!mem::Read(z.actor, h, sizeof(h))) {
        z.actor = 0;
        return;
    }
    std::memcpy(&z.c, h + kActC, 4);
    std::memcpy(&z.state, h + kActState, 4);
    std::memcpy(&z.flags, h + kActFlags, 4);
    std::memcpy(&z.pdist, h + kActPlayerDist, 4);
    if (!std::isfinite(z.pdist)) z.pdist = -1;
    z.mode = h[kActMode];
    std::memcpy(&z.approach, h + kActApproach, 4);
    if (!std::isfinite(z.approach)) z.approach = 0;
    std::uintptr_t params = 0;
    std::memcpy(&params, h + kActParams, 8);
    if (params && !(mem::Rd(params + kParTrigger, z.trig) && std::isfinite(z.trig))) z.trig = -1;
    std::memcpy(z.pos, h + kActXf, sizeof(z.pos));
    z.pos_ok = R::PosSane(z.pos);
    if (!z.pos_ok) {  // the entity's own position
        std::uintptr_t ref = 0;
        std::memcpy(&ref, h + kActEntRef, 8);
        if (ref > 0x20 && mem::Read(ref - 0x20 + kEntPos, z.pos, sizeof(z.pos))) z.pos_ok = R::PosSane(z.pos);
    }
    std::uintptr_t loc = 0;
    std::memcpy(&loc, h + kActLoc, 8);
    std::uint8_t guid[16];
    if (!loc || !mem::Read(loc + kLocGuid, guid, sizeof(guid)) || R::GuidZero(guid)) return;
    z.loc_hash = R::GuidHash(guid);
    const std::uintptr_t scene = mem::Q(loc + kLocScene);
    if (scene && mem::Read(scene + kSceneGuid, guid, sizeof(guid)) && !R::GuidZero(guid)) z.field = R::GuidHash(guid);
    z.located = true;
}

void ReadFields(std::uintptr_t mgr, Census& c) {
    const std::uintptr_t data = mem::Q(mgr + kMgrMap);
    std::uint32_t cap = 0;
    if (!data || !mem::Rd(mgr + kMgrMapCap, cap) || cap == 0 || cap > 4096) return;
    std::vector<std::uint8_t> slots(static_cast<std::size_t>(cap) * kMapSlot);
    if (!mem::Read(data, slots.data(), slots.size())) return;
    for (std::uint32_t i = 0; i < cap; ++i) {
        const std::uint8_t* s = slots.data() + static_cast<std::size_t>(i) * kMapSlot;
        std::uint32_t hash = 0, key = 0;
        std::uintptr_t state = 0;
        std::memcpy(&hash, s, 4);
        std::memcpy(&key, s + 8, 4);
        std::memcpy(&state, s + 0x10, 8);
        if (!(hash & 0x80000000u) || !state) continue;
        std::uint8_t b[0x24];
        if (!mem::Read(state, b, sizeof(b))) continue;
        Field f;
        f.id = key;
        std::memcpy(&f.killed, b + kStKilled, 4);
        std::memcpy(&f.timers, b + kStTimers, 4);
        std::memcpy(&f.total, b + kStTotal, 4);
        if (f.killed > 0 && f.killed <= 512) {
            const std::uintptr_t ids = mem::Q(state + kStKilledIds);
            f.kills.resize(static_cast<std::size_t>(f.killed));
            if (!ids || !mem::Read(ids, f.kills.data(), f.kills.size() * 4)) f.kills.clear();
        }
        c.f.push_back(std::move(f));
    }
}

void TakeCensus(double now_ms, Census& c) {
    const unsigned groups = g_groups;
    const std::uintptr_t mgr = mem::Q(A(kGazerGlobal));
    std::uintptr_t res = 0;
    c.mgr = mgr && mem::Rd(mgr + kMgrResource, res);
    if (c.mgr) {
        std::vector<std::uint32_t> ids;
        if (mem::Rd(mgr + kMgrCount, c.list) && c.list > 0 && c.list <= 4096) {
            const std::uintptr_t p = mem::Q(mgr + kMgrIds);
            ids.resize(static_cast<std::size_t>(c.list));
            if (!p || !mem::Read(p, ids.data(), ids.size() * 4)) ids.clear();
        }
        if (groups & kActor) {
            RefreshGroups(now_ms, false);
            bool miss = false;
            for (std::uint32_t id : ids) miss |= !Resolve(id);
            if (miss) RefreshGroups(now_ms, true);  // a group added since the cache: once, then as found
        }
        for (std::uint32_t id : ids) {
            Gazer z;
            z.id = id;
            if (groups & kActor) ReadGazer(z);
            if (z.actor) ++c.resolved;
            if (z.actor && z.state == 0) ++c.active;
            if (z.actor && z.state == 2) ++c.dead;
            if (z.actor && (z.flags & R::kFlagNear)) ++c.near_n;
            if (z.located) ++c.located;
            c.g.push_back(z);
        }
        if (groups & kField) ReadFields(mgr, c);
        if (groups & kConc) {
            c.conc_ok = mem::Rd(mgr + kMgrConc, c.conc) && mem::Rd(mgr + kMgrConcTimer, c.conc_timer) && mem::Rd(mgr + kMgrNoPerceive, c.noperc);
            if (c.conc_ok && !std::isfinite(c.conc)) c.conc = 0;
        }
    }
    if (groups & kHunter) {
        const std::uintptr_t hm = mem::Q(A(kHunterGlobal));
        if (hm) {
            std::uintptr_t r = 0, area = 0;
            c.hunter_ok = mem::Rd(hm + kHunResource, r) && mem::Rd(hm + kHunArea, area) && mem::Rd(hm + kHunSpawns, c.spawns) &&
                          mem::Rd(hm + kHunGold, c.gold);
            c.hunter_res = r != 0;
            c.hunter_area = area != 0;
        }
    }
    if (groups & kCatcher) {
        const std::uintptr_t cm = mem::Q(A(kCatcherGlobal));
        std::int32_t n = 0;
        if (cm && mem::Rd(cm + kCatCount, n) && n >= 0 && n <= 64) {
            c.catchers_ok = true;
            const std::uintptr_t data = mem::Q(cm + kCatData);
            if (n > 0 && data) {
                std::vector<std::uint8_t> recs(static_cast<std::size_t>(n) * kCatRec);
                if (mem::Read(data, recs.data(), recs.size())) {
                    for (std::int32_t i = 0; i < n; ++i) {
                        const std::uint8_t* r = recs.data() + static_cast<std::size_t>(i) * kCatRec;
                        Catcher k;
                        std::uintptr_t ent = 0;
                        std::memcpy(&ent, r + kCatEnt, 8);
                        std::memcpy(&k.actor, r + kCatActor, 4);
                        std::memcpy(&k.hp, r + kCatHp, 4);
                        k.entity = ent != 0;
                        k.active = r[kCatInactive] == 0;
                        if (ent > 0x20 && mem::Read(ent - 0x20 + kEntPos, k.pos, sizeof(k.pos))) k.pos_ok = R::PosSane(k.pos);
                        if (!std::isfinite(k.hp)) k.hp = 0;
                        c.c.push_back(k);
                    }
                }
            }
        }
    }
    c.me_ok = LocalPos(c.me);
    c.partner_ok = sync::DebugPartnerPosition(c.partner) && R::PosSane(c.partner);
}

// ---------------------------------------------------------------- the lines
struct Prints {
    std::uint32_t loc = 0, fields = 0, kills = 0;
};
Prints MakePrints(const Census& c) {
    std::vector<std::uint32_t> locs;
    for (const Gazer& z : c.g)
        if (z.located) locs.push_back(z.loc_hash);
    std::vector<std::pair<std::uint32_t, std::int32_t>> fs;
    std::vector<R::FieldKills> ks;
    for (const Field& f : c.f) {
        fs.emplace_back(f.id, f.total);
        ks.push_back({f.id, f.kills});
    }
    return {R::LocatorPrint(std::move(locs)), R::FieldsPrint(std::move(fs)), R::KillsPrint(std::move(ks))};
}
std::vector<R::FieldSeen> FieldsSeen(const Census& c) {
    std::map<std::uint32_t, std::vector<std::uint32_t>> live;
    for (const Gazer& z : c.g)
        if (z.located) live[z.field].push_back(z.loc_hash);
    std::vector<R::FieldSeen> out;
    for (const auto& kv : live) {
        R::FieldSeen s;
        s.id = kv.first;
        s.live = static_cast<int>(kv.second.size());
        s.live_print = R::LocatorPrint(kv.second);
        for (const Field& f : c.f)
            if (f.id == kv.first) s.killed = f.killed, s.timers = f.timers, s.total = f.total;
        out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const R::FieldSeen& a, const R::FieldSeen& b) { return a.live > b.live; });
    return out;
}
const Gazer* Nearest(const Census& c, double* dist) {
    const Gazer* best = nullptr;
    *dist = -1;
    if (!c.me_ok) return nullptr;
    for (const Gazer& z : c.g) {
        if (!z.pos_ok) continue;
        const double d = R::Dist3(z.pos, c.me);
        if (!best || d < *dist) best = &z, *dist = d;
    }
    return best;
}
double NearestTo(const Census& c, const double* p) {
    double best = -1;
    for (const Gazer& z : c.g) {
        if (!z.pos_ok) continue;
        const double d = R::Dist3(z.pos, p);
        if (best < 0 || d < best) best = d;
    }
    return best;
}

bool Change(double now_ms, const char* fmt, ...) {
    if (!g_budget.Take(now_ms)) return false;
    ++g_change_lines;
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    Log("[bt] %s", line);
    return true;
}

void LogChanges(double now_ms, const Census& c) {
    if (!g_have_prev) return;
    if (c.mgr != g_prev_mgr) Change(now_ms, "gazer manager %s", c.mgr ? "back" : "gone (no field loaded / at the title)");
    // the near bit (the local player within the gazer's trigger radius): one line per gazer, rise and fall
    for (const Gazer& z : c.g) {
        const auto it = g_prev_gazers.find(z.id);
        if (it == g_prev_gazers.end() || !z.actor) continue;
        const int e = R::FlagEdge(it->second.flags, z.flags, R::kFlagNear);
        if (e > 0) ++g_near_events;
        if (e != 0)
            Change(now_ms, "near%s: gazer loc %08X (%05X, %08X %s) me %.1f m, game %.1f m, trigger %.1f m, partner %.1f m", e > 0 ? "" : " gone",
                   z.loc_hash, z.id, z.field, R::FieldName(z.field), (z.pos_ok && c.me_ok) ? R::Dist3(z.pos, c.me) : -1.0, z.pdist, z.trig,
                   (z.pos_ok && c.partner_ok) ? R::Dist3(z.pos, c.partner) : -1.0);
    }
    // every flag / state change, grouped by (was, now): one line per gazer when a group has at most 3, else one line for the group
    struct Key {
        std::uint32_t f0, f1;
        std::int32_t s0, s1;
        bool operator<(const Key& o) const { return f0 != o.f0 ? f0 < o.f0 : f1 != o.f1 ? f1 < o.f1 : s0 != o.s0 ? s0 < o.s0 : s1 < o.s1; }
    };
    std::map<Key, std::vector<const Gazer*>> groups;
    for (const Gazer& z : c.g) {
        const auto it = g_prev_gazers.find(z.id);
        if (it == g_prev_gazers.end() || !z.actor) continue;
        if (it->second.flags == z.flags && it->second.state == z.state) continue;
        groups[{it->second.flags, z.flags, it->second.state, z.state}].push_back(&z);
    }
    for (const auto& kv : groups) {
        char was[96], now[96];
        R::FlagsText(kv.first.f0, was, sizeof(was));
        R::FlagsText(kv.first.f1, now, sizeof(now));
        if (kv.second.size() > 3) {
            const Gazer& z = *kv.second.front();
            Change(now_ms, "%zu gazers (%08X %s) flags %08X ->%08X (%s ->%s) state %d -> %d", kv.second.size(), z.field, R::FieldName(z.field),
                   kv.first.f0, kv.first.f1, was, now, kv.first.s0, kv.first.s1);
            continue;
        }
        for (const Gazer* z : kv.second)
            Change(now_ms, "gazer loc %08X (%05X, %08X %s) flags %08X ->%08X (%s ->%s) state %d -> %d, me %.1f m, game %.1f m", z->loc_hash, z->id,
                   z->field, R::FieldName(z->field), kv.first.f0, kv.first.f1, was, now, kv.first.s0, kv.first.s1,
                   (z->pos_ok && c.me_ok) ? R::Dist3(z->pos, c.me) : -1.0, z->pdist);
    }
    for (const Field& f : c.f) {
        const auto it = g_prev_killed.find(f.id);
        const std::int32_t was = it == g_prev_killed.end() ? 0 : it->second;
        if (it != g_prev_killed.end() && was == f.killed) continue;
        if (it == g_prev_killed.end() && f.killed == 0) continue;
        Change(now_ms, "field %08X (%s) killed %d -> %d / %d%s", f.id, R::FieldName(f.id), was, f.killed, f.total,
               (R::TotalKnown(f.total) && f.killed == f.total) ? " (CLEARED)" : "");
    }
    if (c.hunter_ok && c.hunter_area != g_prev_hunter)
        Change(now_ms, "hunter area %s (spawns %d, gold %d)", c.hunter_area ? "ACTIVE (this game's player was noticed)" : "gone", c.spawns, c.gold);
    if (c.conc_ok && c.noperc != g_prev_noperc) Change(now_ms, "player not perceivable %d -> %d", g_prev_noperc, c.noperc);
    for (std::size_t i = 0; i < c.c.size(); ++i) {
        const Catcher& k = c.c[i];
        const CatcherPrev* p = i < g_prev_catchers.size() ? &g_prev_catchers[i] : nullptr;
        if (p && p->actor == k.actor && p->active == k.active && std::fabs(p->hp - k.hp) < 1.0f) continue;
        Change(now_ms, "catcher %zu actor %05X %s hp %.1f%s", i, k.actor, k.active ? "ACTIVE" : "inactive", k.hp, p ? "" : " (new)");
    }
}
void Remember(const Census& c) {
    g_have_prev = true;
    g_prev_mgr = c.mgr;
    g_prev_gazers.clear();
    for (const Gazer& z : c.g)
        if (z.actor) g_prev_gazers[z.id] = {z.flags, z.state};
    g_prev_killed.clear();
    for (const Field& f : c.f) g_prev_killed[f.id] = f.killed;
    if (c.hunter_ok) g_prev_hunter = c.hunter_area;
    if (c.conc_ok) g_prev_noperc = c.noperc;
    g_prev_catchers.clear();
    for (const Catcher& k : c.c) g_prev_catchers.push_back({k.actor, k.active, k.hp});
}

void LogCensus(const Census& c) {
    ++g_n;
    int added = 0, gone = 0;
    if (g_have_prev) {
        for (const Gazer& z : c.g)
            if (z.actor && !g_prev_gazers.count(z.id)) ++added;
        for (const auto& kv : g_prev_gazers) {
            bool still = false;
            for (const Gazer& z : c.g) still |= z.id == kv.first && z.actor != 0;
            gone += still ? 0 : 1;
        }
    }
    int cleared = 0;
    int unset = 0;
    for (const Field& f : c.f) {
        cleared += (R::TotalKnown(f.total) && f.killed == f.total) ? 1 : 0;
        unset += f.total == R::kTotalUnset ? 1 : 0;
    }
    const Prints p = MakePrints(c);
    double dn = -1;
    const Gazer* n = Nearest(c, &dn);
    const double dp = c.partner_ok ? NearestTo(c, c.partner) : -1;
    const int dropped = g_budget.TakeDropped();
    g_dropped_total += dropped;
    char conc[64] = "still n/a";  // +0x230: the player stillness meter (1.000 at rest), not a BT meter
    if (c.conc_ok) std::snprintf(conc, sizeof(conc), "still %.3f%s", c.conc, c.noperc ? " (not perceivable)" : "");
    int active_catchers = 0;
    for (const Catcher& k : c.c) active_catchers += k.active ? 1 : 0;
    Log("[bt] #%d gazers %d (list %d, resolved %d, active %d, dead %d, located %d; +%d -%d) near %d fields %d (cleared %d, unset %d) "
        "locfp=%08X fieldfp=%08X killfp=%08X nearest me %.1f m (loc %08X, game %.1f m, trigger %.1f m) partner %.1f m %s hunter %s spawns %d "
        "catchers %d/%d%s dropped %d",
        g_n, static_cast<int>(c.g.size()), c.list, c.resolved, c.active, c.dead, c.located, added, gone, c.near_n, static_cast<int>(c.f.size()),
        cleared, unset, p.loc, p.fields, p.kills, dn, n ? n->loc_hash : 0u, n ? n->pdist : -1.0f, n ? n->trig : -1.0f, dp, conc,
        !c.hunter_ok ? "n/a" : c.hunter_area ? "ACTIVE" : "none", c.spawns, active_catchers, static_cast<int>(c.c.size()),
        c.mgr ? "" : " (no gazer manager)", dropped);
    const std::vector<R::FieldSeen> seen = FieldsSeen(c);
    if (!seen.empty()) {
        char line[900];
        int at = std::snprintf(line, sizeof(line), "[bt] #%d live fields:", g_n);
        for (std::size_t i = 0; i < seen.size() && i < 6 && at > 0 && at < static_cast<int>(sizeof(line)) - 1; ++i) {
            const R::FieldSeen& s = seen[i];
            at += std::snprintf(line + at, sizeof(line) - static_cast<std::size_t>(at), " %s %08X live %d/%d killed %d locp=%08X (%s);",
                                R::FieldName(s.id), s.id, s.live, s.total, s.killed, s.live_print, R::MatchName(R::MatchStatic(s)));
        }
        Log("%s", line);
    }
    g_max_gazers = std::max(g_max_gazers, static_cast<int>(c.g.size()));
    g_max_fields = std::max(g_max_fields, static_cast<int>(c.f.size()));
    if (c.hunter_area && !(g_have_prev && g_prev_hunter)) ++g_noticed;
    if (c.conc_ok) g_conc_max = std::max(g_conc_max, c.conc);
    g_max_near = std::max(g_max_near, c.near_n);
    R::Perceive pv;
    pv.hunter = c.hunter_area;
    pv.near_n = c.near_n;
    g_perceived |= pv.any();
}

int CountType8() {
    int n = 0;
    for (const Grp& g : g_grps) {
        std::vector<std::uintptr_t> actors(static_cast<std::size_t>(g.count));
        if (!mem::Read(g.actors, actors.data(), actors.size() * 8)) continue;
        for (std::uintptr_t a : actors) {
            std::uint32_t id = 0;
            std::uint8_t used = 0;
            if (a && mem::Rd(a + kActId, id) && R::ActorType(id) == R::kGazerActorType && mem::Rd(a + kActInUse, used) && used) ++n;
        }
    }
    return n;
}

void Dump(double now_ms, const Census& c) {
    Log("[bt] dump: gazer manager %s, list %d, fields %d, catchers %d, me %s (%.1f %.1f %.1f), partner %s (%.1f %.1f %.1f)",
        c.mgr ? "ok" : "none", c.list, static_cast<int>(c.f.size()), static_cast<int>(c.c.size()), c.me_ok ? "ok" : "n/a", c.me[0], c.me[1],
        c.me[2], c.partner_ok ? "ok" : "n/a", c.partner[0], c.partner[1], c.partner[2]);
    int lines = 0;
    for (std::size_t i = 0; i < c.g.size() && lines < 200; ++i, ++lines) {
        const Gazer& z = c.g[i];
        char fl[96];
        R::FlagsText(z.flags, fl, sizeof(fl));
        const char* type = z.actor ? mem::RttiName(mem::RttiOf(z.actor)) : nullptr;
        Log("[bt] dump gazer %zu/%zu id=%05X (type %u) %s state %d loc %08X field %08X (%s) pos %.1f %.1f %.1f me %.1f m (game %.1f m, "
            "trigger %.1f m) partner %.1f m flags %08X (%s) c=%08X",
            i + 1, c.g.size(), z.id, R::ActorType(z.id), type ? type : (z.actor ? "?" : "UNRESOLVED"), z.state, z.loc_hash, z.field,
            R::FieldName(z.field), z.pos[0], z.pos[1], z.pos[2], (z.pos_ok && c.me_ok) ? R::Dist3(z.pos, c.me) : -1.0, z.pdist, z.trig,
            (z.pos_ok && c.partner_ok) ? R::Dist3(z.pos, c.partner) : -1.0, z.flags, fl, z.c);
    }
    for (const Field& f : c.f) {
        const R::KnownField* k = R::FindKnownField(f.id);
        char ks[16 * 9 + 8] = "";
        int at = 0;
        for (std::size_t i = 0; i < f.kills.size() && i < 16; ++i)
            at += std::snprintf(ks + at, sizeof(ks) - static_cast<std::size_t>(at), " %08X", f.kills[i]);
        Log("[bt] dump field %08X (%s) killed %d timers %d total %d [archive %d]%s kills:%s%s", f.id, k ? k->name : "?", f.killed, f.timers,
            f.total, k ? k->locators : -1, (R::TotalKnown(f.total) && f.killed == f.total) ? " CLEARED" : "", ks[0] ? ks : " none",
            f.kills.size() > 16 ? " ..." : "");
    }
    if (c.hunter_ok) Log("[bt] dump hunter: resource %s, hunter area %s, spawns %d, gold %d", c.hunter_res ? "loaded" : "none",
                         c.hunter_area ? "ACTIVE" : "none", c.spawns, c.gold);
    if (c.conc_ok) Log("[bt] dump stillness meter (+0x230) %.4f timer %.3f not-perceivable %d", c.conc, c.conc_timer, c.noperc);
    for (std::size_t i = 0; i < c.c.size(); ++i)
        Log("[bt] dump catcher %zu: actor %05X %s entity %s hp %.1f", i, c.c[i].actor, c.c[i].active ? "ACTIVE" : "inactive",
            c.c[i].entity ? "yes" : "none", c.c[i].hp);
    if (g_groups & kReg) {
        RefreshGroups(now_ms, true);
        Log("[bt] dump registry: type-8 actors in use %d vs the gazer list %d (%d groups)", CountType8(), c.list, static_cast<int>(g_grps.size()));
    }
    Log("[bt] dump done: %d gazer lines, %d fields, %d catchers", lines, static_cast<int>(c.f.size()), static_cast<int>(c.c.size()));
}

const char* Refusal() {
    if (g_ini == 0 && !automation::Enabled()) return "bt_probe=0 in session.ini and not a hidden run";
    return nullptr;
}

void StopLine(const char* why) {
    Log("[bt] probe stop (%s): %d censuses, max gazers %d, max fields %d, noticed %d times, near %d times (max %d at once), still max %.3f, "
        "perceived %s, change lines %d (dropped %d)",
        why, g_n, g_max_gazers, g_max_fields, g_noticed, g_near_events, g_max_near, g_conc_max, g_perceived ? "yes" : "no", g_change_lines,
        g_dropped_total);
}

// ================================================================ B1: the BT field sync (bt_sync=1|2)
// ---------------------------------------------------------------- game addresses (ds.exe DC; round 28 lane B1 static scan)
// The voidout FUN_14299B1B0(rcx gazer manager, rdx const double pos[3], r8b gazer caused, r9b with baggage): it reads only these
// four (no stack arguments; every caller builds pos at [rsp+0x20]), checks [mgr+0x68], then the far distance [[mgr+0x68]+0x54].
constexpr std::uintptr_t kVoidFn = 0x14299B1B0;
constexpr const char* kVoidSig =
    "48 8B C4 48 89 78 18 41 54 41 56 41 57 48 81 EC 80 00 00 00 48 83 79 68 00 45 0F B6 F9 45 0F B6 E0 4C 8B F2 48 8B F9 0F 84 AE 01 00 00";
constexpr std::uintptr_t kVoidRes = 0x68, kResFarDist = 0x54;  // 0x14299B1F6 / 0x14299B22D
// Its four E8 call sites (the return addresses name the caller; the only references to it in the image). Round 30 read each caller
// [V code] and renamed the sources (the numbers stay; bt_rules.hpp VoidSource):
//   0x14299B44B  FUN_14299B3B0, whose only caller is the timer list FUN_142B21CA0: a scripted / timed voidout;
//   0x14299F360  FUN_14299EFB0, the gazer catch (message 0x682DFB3B);
//   0x1429A5509  the gazer manager update FUN_1429A3F50: a human corpse on the list +0x1760 (FUN_1429A1230 sets +0x1780 bit 0),
//                3 s later (corpse necrosis; ResetAllGazers clears +0x1780, so a RESET cancels a pending one);
//   0x142A9766A  FUN_142A97650, queued by the MULE actor FUN_142AA2230: a MULE corpse's necrosis (not the BT catcher).
struct VoidCaller {
    std::uintptr_t ret;
    std::uint8_t source;
};
constexpr VoidCaller kVoidCallers[] = {{0x14299B44B, R::kVoidSrcScripted},
                                       {0x14299F360, R::kVoidSrcCatch},
                                       {0x1429A5509, R::kVoidSrcCorpseNecrosis},
                                       {0x142A9766A, R::kVoidSrcMuleCorpse}};
constexpr unsigned kVoidCallerCount = sizeof(kVoidCallers) / sizeof(kVoidCallers[0]);
// ResetAllGazers (the thunk dev_tools.cpp calls for dev_enemies 3: mov rcx, [the gazer manager]; jmp the body)
constexpr std::uintptr_t kResetThunk = 0x1429A7220, kResetBody = 0x1429A17A0;
constexpr const char* kResetThunkSig = "48 8B 0D 99 4C 24 05 E9 74 A5 FF FF";
constexpr const char* kResetBodySig = "4C 8B DC 41 54 48 81 EC 80 00 00 00";

// ---------------------------------------------------------------- the voidout detour (any thread the game calls it on)
struct VoidRec {
    double pos[3] = {0, 0, 0};
    bool pos_ok = false;
    std::uint8_t caused = 0, baggage = 0, source = R::kVoidSrcUnknown;
    float far_dist = -1;
    std::uintptr_t ret = 0;
};
constexpr unsigned kVoidRing = 8;
SRWLOCK g_void_lock = SRWLOCK_INIT;
VoidRec g_void_q[kVoidRing];
unsigned g_void_n = 0;
std::atomic<unsigned> g_void_calls{0}, g_void_lost{0};
std::atomic<bool> g_void_pass{false};  // EmergencyStop: straight through
thread_local bool t_crater = false;  // round 30 P3: inside the probe's guarded crater (no VOID relay, the flow call dropped)
std::uintptr_t g_void_ret[kVoidCallerCount] = {};
using VoidFn = std::uint64_t (*)(void*, const double*, std::uint8_t, std::uint8_t);
VoidFn g_void_orig = nullptr;
bool g_void_hook = false;
char g_void_text[96] = "not installed";

void VoidRecord(void* mgr, const double* pos, std::uint8_t caused, std::uint8_t baggage, std::uintptr_t ret) noexcept {
    g_void_calls.fetch_add(1, std::memory_order_relaxed);
    VoidRec r;
    r.caused = caused, r.baggage = baggage, r.ret = ret;
    r.pos_ok = pos && mem::Read(reinterpret_cast<std::uintptr_t>(pos), r.pos, sizeof(r.pos)) && R::PosSane(r.pos);
    std::uintptr_t res = 0;
    float fd = 0;
    if (mgr && mem::Rd(reinterpret_cast<std::uintptr_t>(mgr) + kVoidRes, res) && res && mem::Rd(res + kResFarDist, fd) && std::isfinite(fd)) r.far_dist = fd;
    for (unsigned i = 0; i < kVoidCallerCount; ++i)
        if (ret == g_void_ret[i]) r.source = kVoidCallers[i].source;
    AcquireSRWLockExclusive(&g_void_lock);
    if (g_void_n < kVoidRing) g_void_q[g_void_n++] = r;
    else g_void_lost.fetch_add(1, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_void_lock);
}
std::uint64_t VoidDetour(void* mgr, const double* pos, std::uint8_t caused, std::uint8_t baggage) {
    if (!g_void_pass.load(std::memory_order_relaxed) && !t_crater) VoidRecord(mgr, pos, caused, baggage, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    return g_void_orig(mgr, pos, caused, baggage);
}
unsigned TakeVoids(VoidRec* out, unsigned cap) noexcept {
    AcquireSRWLockExclusive(&g_void_lock);
    const unsigned n = g_void_n < cap ? g_void_n : cap;
    for (unsigned i = 0; i < n; ++i) out[i] = g_void_q[i];
    for (unsigned i = n; i < g_void_n; ++i) g_void_q[i - n] = g_void_q[i];
    g_void_n -= n;
    ReleaseSRWLockExclusive(&g_void_lock);
    return n;
}

// Start-up (bootstrap thread, before any world): the prologue checked, then the detour in; the callers' E8 targets checked for the
// log only (a caller we cannot name is relayed as "unknown caller").
void InstallVoidHook() {
    if (!mem::Base()) {
        std::snprintf(g_void_text, sizeof(g_void_text), "off: ds.exe base unknown");
        Log("[bt] sync: voidout hook off: ds.exe base unknown (no VOID from this game)");
        return;
    }
    int callers = 0;
    for (unsigned i = 0; i < kVoidCallerCount; ++i) {
        g_void_ret[i] = A(kVoidCallers[i].ret);
        if (mem::RipTarget(A(kVoidCallers[i].ret - 5), 1, 5) == A(kVoidFn)) ++callers;
    }
    if (!mem::CodeMatches(A(kVoidFn), kVoidSig)) {
        std::snprintf(g_void_text, sizeof(g_void_text), "off: byte check failed");
        Log("[bt] byte check FAILED: the voidout FUN_14299B1B0 prologue (the hook stays out; no VOID from this game)");
        return;
    }
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        std::snprintf(g_void_text, sizeof(g_void_text), "off: MinHook %s", MH_StatusToString(init));
        Log("[bt] sync: voidout hook off: MinHook %s", MH_StatusToString(init));
        return;
    }
    void* target = reinterpret_cast<void*>(A(kVoidFn));
    MH_STATUS s = MH_CreateHook(target, reinterpret_cast<void*>(&VoidDetour), reinterpret_cast<void**>(&g_void_orig));
    if (s == MH_OK) s = MH_EnableHook(target);
    g_void_hook = s == MH_OK;
    std::snprintf(g_void_text, sizeof(g_void_text), "%s", g_void_hook ? "in" : MH_StatusToString(s));
    Log("[bt] sync: voidout hook %s (FUN_14299B1B0, pass-through; %d/%u callers checked)", g_void_hook ? "in" : MH_StatusToString(s), callers,
        kVoidCallerCount);
}

// ResetAllGazers: byte-checked on first use, called on the game thread (as dev_enemies 3), SEH-guarded.
int g_reset_check = -1;  // -1 not yet, 0 failed, 1 ok
bool ResetUsable() {
    if (g_reset_check >= 0) return g_reset_check == 1;
    bool ok = mem::Base() != 0;
    if (ok && !mem::CodeMatches(A(kResetThunk), kResetThunkSig)) ok = false, Log("[bt] byte check FAILED: the ResetAllGazers thunk 0x1429A7220");
    if (ok && !mem::CodeMatches(A(kResetBody), kResetBodySig)) ok = false, Log("[bt] byte check FAILED: the ResetAllGazers body 0x1429A17A0");
    if (ok && mem::RipTarget(A(kResetThunk), 3, 7) != A(kGazerGlobal)) ok = false, Log("[bt] byte check FAILED: ResetAllGazers' gazer manager global");
    if (ok && mem::RipTarget(A(kResetThunk + 7), 1, 5) != A(kResetBody)) ok = false, Log("[bt] byte check FAILED: ResetAllGazers' jump to its body");
    g_reset_check = ok ? 1 : 0;
    return ok;
}
bool CallReset(std::uintptr_t fn) noexcept {
    __try {
        reinterpret_cast<void (*)()>(fn)();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------- the sync's state (game thread, but the atomics)
int g_sync = 0;                     // bt_sync
int g_role = 0;                     // 1 host, 2 guest
std::atomic<bool> g_sync_dead{false};  // EmergencyStop / a faulted apply path: nothing more is sent or applied
double g_s_next_ms = 0;
std::uint32_t g_s_gen = 0;
bool g_s_beacon = false, g_s_hello_sent = false, g_s_peer = false, g_s_wait_logged = false, g_s_hello_wait_logged = false;
bool g_s_off_logged = false;
double g_s_hello_ms = 0;
int g_s_peer_level = 0, g_s_peer_role = 0;
std::uint16_t g_s_peer_caps = 0;
std::uint32_t g_s_seq = 0;            // our BTS1 count this link session
std::uint16_t g_epoch = 0;            // our resets seen (not the applied ones)
std::uint16_t g_applied_epoch = 0;    // guest: the host's last RESET epoch applied this link session
double g_s_applied_ms = -1e18;
R::RecSummary g_s_rec;
bool g_s_have_rec = false;
std::map<std::uint64_t, std::uint32_t> g_s_prev_flags;  // locator (or 1 << 32 | id) -> flags, the last census
std::set<std::uint64_t> g_s_quiet;  // gazers logged once: inside the trigger by our distance, near bit clear
std::set<std::uint64_t> g_s_skip_logged;  // gazers logged once: a near rise held back (state != 0 or a position unreadable)
std::set<std::uint64_t> g_s_close_logged;  // gazers logged once: within kCloseLogM (the game's or our distance), whatever the bit
std::set<std::uint64_t> g_s_first_near;  // gazers logged once: the first census that saw them already had the near bit
int g_s_unres_max = 0;  // the most unresolved gazers (no actor) logged in one census so far
constexpr double kCloseLogM = 25.0;
bool g_s_prev_hunter = false;
R::NoticeGate g_s_gate;
R::FieldMsg g_s_here, g_s_there, g_s_last_sent;
bool g_s_have_here = false, g_s_have_there = false, g_s_sent_any = false;
double g_s_field_ms = -1e18;
std::uint32_t g_s_cmp_key = 0;
double g_s_cmp_ms = -1e18;
double g_s_me[3] = {0, 0, 0};
bool g_s_me_ok = false;
struct LocalGazer {
    std::uint32_t id = 0, field = 0;
    double pos[3] = {0, 0, 0};
    bool pos_ok = false;
};
std::map<std::uint32_t, LocalGazer> g_s_locs;  // locator -> this game's live gazer, the last census
double g_s_toast_ms = -1e18;
R::LineBudget g_s_budget;
struct Counts {
    unsigned tx_hello = 0, tx_field = 0, tx_noticed = 0, held_noticed = 0, tx_void = 0, tx_reset = 0;
    unsigned rx_hello = 0, rx_field = 0, rx_noticed = 0, rx_void = 0, rx_reset = 0, applied = 0, refused = 0;
    unsigned compares = 0, bad = 0, stale = 0, unsent = 0, resets_here = 0;
} g_s;

const char* RoleText(int r) { return r == 1 ? "host" : r == 2 ? "guest" : "?"; }

// The guest applies the host's RESET only on a sandboxed save (a host-save guest, a test save, a hidden run): with host_save=0 she
// plays on her own real save, which a partner never changes (dev_tools' kRealSaveReason rule; review round 28 defaults). The
// hello's reset-apply cap says so.
bool ResetApplyHere() { return g_sync >= 2 && g_role == 2 && automation::SavesRedirected(); }

// Round 30 S1 / S2 (the code is at the end of the anonymous namespace).
bool g_trouble_on = false;  // bt_trouble (with bt_sync)
int g_kill_level = 0;       // bt_kill, at most bt_sync (bt_sync=1 applies nothing)
double g_reset_ms = -1e18;  // the last reset here, seen or applied: no kill is sent within 5 s of it
struct Counts2 {
    unsigned tx_trouble = 0, tx_gzst = 0, tx_kill = 0, unsent_kill = 0;
    unsigned rx_trouble = 0, rx_gzst = 0, rx_kill = 0, kill_applied = 0, kill_refused = 0, events_lost = 0;
} g_s2;
// KILL goes from here: the gazer update hook can go in (its byte checks) and bt_kill >= 1.
bool KillSendable() {
    const unsigned w = btworld::Check();
    return g_kill_level >= 1 && (w & btworld::kBitHook) && (w & btworld::kBitKill);
}
// A partner's KILL may be applied here: bt_kill 2; the guest only on a sandboxed save (Must-NOT 5).
bool KillApplyHere() { return g_kill_level >= 2 && KillSendable() && (g_role == 1 || automation::SavesRedirected()); }
void TroubleCensus(double now_ms, const Census& c);
void GzstCensus(double now_ms, const Census& c);
void OnTrouble(double now_ms, const std::uint8_t* b, std::size_t n);
void OnGzst(double now_ms, const std::uint8_t* b, std::size_t n);
void OnKill(double now_ms, const std::uint8_t* b, std::size_t n);
void InstallKillHook(const char* why);
void S12Session();
void S12Tick(double now_ms);

void Toast(double now_ms, const char* text) {
    if (now_ms < g_s_toast_ms) return;
    g_s_toast_ms = now_ms + 8000.0;
    coop::SetVehicleNotice(text, true, now_ms + 5000.0);
}

// Round 30: a partner VOID waits up to R::kVoidDownWindowMs for the partner's GameOver (their SEH1 beacon's player_down == 1, game
// state bit 13) before its verdict: "Your partner was voided out" only then; otherwise logged only (a corpse or a MULE corpse
// voiding out near them, a scripted crater). g_s_down_ms: when the newest beacon that said "down (GameOver)" arrived (-1 none).
struct PendVoid {
    double void_ms = 0;
    bool test = false;
    std::uint8_t source = R::kVoidSrcUnknown;
};
constexpr unsigned kPendVoids = 8;
PendVoid g_s_pend[kPendVoids];
unsigned g_s_pend_n = 0;
double g_s_down_ms = -1;

void SampleDown(double now_ms) {
    shotw::Beacon b;
    double age = 0;
    if (!shots::PeerBeacon(&b, &age) || b.player_down != R::kBeaconDownDead) return;
    const double at = now_ms - (std::isfinite(age) && age > 0 ? age : 0);
    if (at > g_s_down_ms) g_s_down_ms = at;
}

void ResolveVoids(double now_ms) {
    unsigned keep = 0;
    // round 30 review: without the down channel no GameOver can come (VoidVerdict decides by source at once)
    const bool channel = g_s_pend_n && shots::DownChannel();
    for (unsigned i = 0; i < g_s_pend_n; ++i) {
        const PendVoid& pv = g_s_pend[i];
        const R::VoidCall c = R::VoidVerdict(pv.test, pv.void_ms, g_s_down_ms, now_ms, channel, pv.source);
        switch (c) {
            case R::VoidCall::wait: g_s_pend[keep++] = pv; break;
            case R::VoidCall::test:
                Log("[bt] partner voidout verdict: a test: toast");
                Toast(now_ms, "Voidout test from your partner");
                break;
            case R::VoidCall::partner:
                Log("[bt] partner voidout verdict: the partner went down (GameOver) within 5 s: toast; source %s", R::VoidSourceName(pv.source));
                Toast(now_ms, "Your partner was voided out");
                break;
            case R::VoidCall::log_only:
                Log("[bt] partner voidout verdict: no GameOver from the partner within 5 s: log only; source %s", R::VoidSourceName(pv.source));
                break;
            case R::VoidCall::no_channel_toast:
                Log("[bt] partner voidout verdict: no down channel (restrain part off here or there): toast by source; source %s",
                    R::VoidSourceName(pv.source));
                Toast(now_ms, "Your partner was voided out");
                break;
            case R::VoidCall::no_channel_log:
                Log("[bt] partner voidout verdict: no down channel (restrain part off here or there): log only; source %s", R::VoidSourceName(pv.source));
                break;
        }
    }
    g_s_pend_n = keep;
}

bool Send(const std::uint8_t* b, std::size_t n) {
    if (!n || g_sync_dead.load()) return false;
    if (!link::SendCombat(b, n)) {
        ++g_s.unsent;
        return false;
    }
    ++g_s_seq;
    return true;
}

void SendHello(double now_ms) {
    R::BtsHello h;
    h.level = static_cast<std::uint8_t>(g_sync);
    h.role = static_cast<std::uint8_t>(g_role);
    h.caps = static_cast<std::uint16_t>(R::kCapField | R::kCapNoticed | (g_void_hook ? R::kCapVoid : 0) | (ResetApplyHere() ? R::kCapResetApply : 0) |
                                        (g_trouble_on ? (R::kCapTrouble | R::kCapGzst) : 0) | (KillSendable() ? R::kCapKill : 0) |
                                        (KillApplyHere() ? R::kCapKillApply : 0));
    std::uint8_t b[R::kBtsHelloSize];
    if (!Send(b, R::EncodeHello(g_s_seq, h, b, sizeof(b)))) return;
    g_s_hello_sent = true;
    g_s_hello_ms = now_ms;
    ++g_s.tx_hello;
    Log("[bt] sync: hello sent (bt_sync %d, role %s, caps 0x%X)", g_sync, RoleText(g_role), h.caps);
}

// The census in the FIELD form, and the records' summary.
R::FieldMsg BuildField(const Census& c, R::RecSummary* rec) {
    R::FieldMsg m;
    m.epoch = g_epoch;
    m.applied = g_applied_epoch;
    std::vector<std::uint32_t> ids, locs;
    std::vector<R::FieldKills> ks;
    int unset = 0, cleared = 0, killed = 0;
    for (const Field& f : c.f) {
        ids.push_back(f.id);
        unset += f.total == R::kTotalUnset ? 1 : 0;
        cleared += (R::TotalKnown(f.total) && f.killed == f.total) ? 1 : 0;
        killed += std::clamp(f.killed, 0, 0xFFFF);
        ks.push_back({f.id, f.kills});
    }
    for (const Gazer& z : c.g)
        if (z.located) locs.push_back(z.loc_hash);
    m.fields = static_cast<std::uint16_t>(std::min<std::size_t>(c.f.size(), 0xFFFF));
    m.unset = static_cast<std::uint16_t>(unset);
    m.cleared = static_cast<std::uint16_t>(cleared);
    m.live = static_cast<std::uint16_t>(std::min<std::size_t>(locs.size(), 0xFFFF));
    m.ids = R::LocatorPrint(ids);
    m.kills = R::KillsPrint(std::move(ks));
    m.locs = R::LocatorPrint(std::move(locs));
    const std::vector<R::FieldSeen> seen = FieldsSeen(c);
    for (const R::FieldSeen& s : seen) {
        if (m.n >= R::kBtsFieldMax) break;
        R::FieldEntry& e = m.e[m.n++];
        e.id = s.id;
        e.live = static_cast<std::uint16_t>(std::clamp(s.live, 0, 0xFFFF));
        e.killed = static_cast<std::uint16_t>(std::clamp(s.killed, 0, 0xFFFF));
        e.total = s.total;
        e.print = s.live_print;
    }
    rec->fields = static_cast<int>(c.f.size());
    rec->unset = unset;
    rec->killed_total = killed;
    rec->ids = m.ids;
    return m;
}

// The compare line, when its result changed (or every 30 s while it holds).
void CompareNow(double now_ms) {
    if (!g_s_have_here || !g_s_have_there) return;
    const R::FieldCompare c = R::CompareFields(g_s_here, g_s_there);
    const std::uint32_t key = c.Key(g_s_here, g_s_there);
    if (key == g_s_cmp_key && now_ms - g_s_cmp_ms < 30000.0) return;
    g_s_cmp_key = key;
    g_s_cmp_ms = now_ms;
    ++g_s.compares;
    char live[400] = "";
    int at = 0;
    for (unsigned i = 0; i < g_s_there.n && at >= 0 && at < static_cast<int>(sizeof(live)) - 1; ++i) {
        const R::FieldEntry& t = g_s_there.e[i];
        const R::FieldEntry* h = R::FindEntry(g_s_here, t.id);
        at += std::snprintf(live + at, sizeof(live) - static_cast<std::size_t>(at), "%s%s %u/%u %s", i ? "; " : "", R::FieldName(t.id), h ? h->live : 0u,
                            t.live, R::EntryCompareName(g_s_here, t));
    }
    Log("[bt] field compare #%u: records %s (%u/%u, ids %08X/%08X), kills %s, unset %u/%u, epoch %u/%u (applied %u/%u), live %u/%u; %s", g_s.compares,
        c.records ? "equal" : "DIFFER", g_s_here.fields, g_s_there.fields, g_s_here.ids, g_s_there.ids, c.kills ? "equal" : "DIFFER", g_s_here.unset,
        g_s_there.unset, g_s_here.epoch, g_s_there.epoch, g_s_here.applied, g_s_there.applied, g_s_here.live, g_s_there.live,
        live[0] ? live : "no live field there");
}

void SendVoid(double now_ms, const VoidRec& r, bool test) {
    (void)now_ms;
    double me[3];
    const bool me_ok = LocalPos(me);
    R::BtsVoid v;
    v.pos[0] = static_cast<float>(r.pos[0]), v.pos[1] = static_cast<float>(r.pos[1]), v.pos[2] = static_cast<float>(r.pos[2]);
    v.player_dist = (me_ok && r.pos_ok) ? static_cast<float>(R::Dist3(me, r.pos)) : -1.0f;
    v.far_dist = r.far_dist;
    const bool far_crater = v.player_dist >= 0 && r.far_dist > 0 && v.player_dist >= r.far_dist;
    v.reason = test ? R::kVoidReasonTest : R::VoidReason(r.caused != 0, r.baggage != 0, far_crater);
    v.source = test ? R::kVoidSrcTest : r.source;
    v.flags = static_cast<std::uint8_t>((r.caused ? R::kVoidGazer : 0) | (r.baggage ? R::kVoidBaggage : 0) | (far_crater ? R::kVoidFar : 0) | (test ? R::kVoidTest : 0));
    v.sender_ms = static_cast<std::uint32_t>(link::NowMs());
    const std::uint32_t seq = g_s_seq;
    std::uint8_t b[R::kBtsVoidSize];
    const bool sent = g_s_peer && Send(b, R::EncodeVoid(seq, v, b, sizeof(b)));
    if (sent) ++g_s.tx_void;
    Log("[bt] voidout here%s: at (%.1f %.1f %.1f)%s, reason %u %s, source %s (caller 0x%llX), %.0f m from me (far %.0f m); %s", test ? " (test)" : "",
        r.pos[0], r.pos[1], r.pos[2], r.pos_ok ? "" : " (pos unreadable)", v.reason, R::VoidReasonName(v.reason), R::VoidSourceName(v.source),
        static_cast<unsigned long long>(r.ret ? r.ret - mem::Base() + kImage : 0), v.player_dist, v.far_dist,
        sent ? "VOID sent" : g_s_peer ? "VOID NOT sent (the link did not take it)" : "not sent (no partner hello)");
    if (sent) Log("[bt] VOID sent%s (seq %u)", test ? " (test)" : "", seq);
}

bool SendNoticed(const R::BtsNoticed& n, const char* what, double trig) {
    if (!g_s_peer) return false;
    const std::uint32_t seq = g_s_seq;
    std::uint8_t b[R::kBtsNoticedSize];
    if (!Send(b, R::EncodeNoticed(seq, n, b, sizeof(b)))) return false;
    ++g_s.tx_noticed;
    const double gz[3] = {n.gazer[0], n.gazer[1], n.gazer[2]}, me[3] = {n.player[0], n.player[1], n.player[2]};
    Log("[bt] NOTICED sent (%s): gazer loc %08X (field %08X %s) me %.1f m from it (game %.1f m, trigger %.1f m), %u near now (seq %u)", what, n.loc,
        n.field, R::FieldName(n.field), R::Dist3(gz, me), n.game_dist, trig, n.near_n, seq);
    return true;
}

// One census: the reset poll, the near rises (NOTICED), the hunter area (NOTICED found), the FIELD, the compare.
void SyncCensus(double now_ms) {
    Census c;
    TakeCensus(now_ms, c);
    g_s_me_ok = c.me_ok;
    if (c.me_ok) std::memcpy(g_s_me, c.me, sizeof(g_s_me));
    R::RecSummary rec;
    const R::FieldMsg m = BuildField(c, &rec);
    // the reset (polled: the records all unset at once, nothing killed)
    if (g_s_have_rec && R::LooksLikeReset(g_s_rec, rec)) {
        ++g_s.resets_here;
        g_reset_ms = now_ms;  // round 30 S2: no kill is sent within 5 s of a reset
        g_s_quiet.clear();  // the "near not set" diagnostic logs again after a reset (before / after it are told apart)
        g_s_skip_logged.clear();
        g_s_close_logged.clear();
        g_s_first_near.clear();
        if (R::ResetEcho(now_ms, g_s_applied_ms)) {
            Log("[bt] reset seen here: the applied host epoch %u (%d records unset; not sent back)", g_applied_epoch, rec.unset);
        } else {
            if (++g_epoch == 0) g_epoch = 1;
            R::BtsReset r;
            r.epoch = g_epoch;
            r.fields = static_cast<std::uint16_t>(std::min(g_s_rec.fields, 0xFFFF));
            r.ids = g_s_rec.ids;
            const std::uint32_t seq = g_s_seq;
            std::uint8_t b[R::kBtsResetSize];
            const bool sent = g_s_peer && Send(b, R::EncodeReset(seq, r, b, sizeof(b)));
            if (sent) ++g_s.tx_reset;
            Log("[bt] reset seen here: epoch %u (%d records, ids %08X, all unset)%s", g_epoch, rec.fields, rec.ids, sent ? "" : " (not sent: no partner hello)");
            if (sent) Log("[bt] RESET epoch %u sent (seq %u)", g_epoch, seq);
        }
    }
    g_s_rec = rec;
    g_s_have_rec = true;
    // the near rises: this game's own gazers sensing this game's player
    std::map<std::uint64_t, std::uint32_t> flags;
    g_s_locs.clear();
    int unresolved = 0;
    for (const Gazer& z : c.g) {
        if (!z.actor) {
            ++unresolved;  // counted below: this skip was silent before
            continue;
        }
        const std::uint64_t key = z.located ? z.loc_hash : ((1ull << 32) | z.id);
        flags[key] = z.flags;
        if (z.located) {
            LocalGazer& l = g_s_locs[z.loc_hash];
            l.id = z.id;
            l.field = z.field;
            l.pos_ok = z.pos_ok;
            std::memcpy(l.pos, z.pos, sizeof(l.pos));
        }
        const auto it = g_s_prev_flags.find(key);
        const std::uint32_t was = it == g_s_prev_flags.end() ? 0u : it->second;
        // diagnostics (w2b, read only, once per gazer; cleared on a reset): a close gazer by the game's distance (+0x204, -1 when
        // unreadable) or ours, whatever its bit and whether its position reads; and a gazer first seen with the near bit already set
        // (no rise can follow). Together with the probe's change lines they tell "the bit never rose" from "rose, held back".
        {
            const double own = z.pos_ok && c.me_ok ? R::Dist3(z.pos, c.me) : -1.0;
            const bool close = (z.pdist >= 0 && z.pdist <= kCloseLogM) || (own >= 0 && own <= kCloseLogM);
            if (close && g_s_close_logged.insert(key).second)
                Log("[bt] near check: gazer loc %08X (field %08X %s) state %d, flags %08X (near %d), trigger %.1f m, game %.1f m, me %.1f m, "
                    "pos_ok %d me_ok %d",
                    z.loc_hash, z.field, R::FieldName(z.field), z.state, z.flags, (z.flags & R::kFlagNear) ? 1 : 0, z.trig, z.pdist, own,
                    z.pos_ok ? 1 : 0, c.me_ok ? 1 : 0);
            if (it == g_s_prev_flags.end() && (z.flags & R::kFlagNear) && g_s_first_near.insert(key).second)
                Log("[bt] near already set when first seen: gazer loc %08X (field %08X %s) state %d, flags %08X, game %.1f m, me %.1f m",
                    z.loc_hash, z.field, R::FieldName(z.field), z.state, z.flags, z.pdist, own);
        }
        // diagnostic (w2): inside the trigger by OUR distance but the game's near bit clear (once per gazer); the game's own +0x204
        // distance beside it shows a stale gazer update
        if (z.pos_ok && c.me_ok && z.trig > 0 && !(z.flags & R::kFlagNear)) {
            const double own = R::Dist3(z.pos, c.me);
            if (own <= z.trig && g_s_quiet.insert(key).second)
                Log("[bt] near not set: gazer loc %08X (field %08X %s) me %.1f m, game %.1f m, trigger %.1f m, state %d, flags %08X", z.loc_hash,
                    z.field, R::FieldName(z.field), own, z.pdist, z.trig, z.state, z.flags);
        }
        if (R::FlagEdge(was, z.flags, R::kFlagNear) <= 0) continue;
        if (z.state != 0 || !z.pos_ok || !c.me_ok) {
            // (w2b) a rise held back is kept as a rise: the bit is stored clear, so a later census (state 0, position read) sends it.
            // Behaviour change: before, this rise was consumed (stored set) and lost for good. The gate-held path below still consumes it.
            flags[key] = z.flags & ~R::kFlagNear;
            if (g_s_skip_logged.insert(key).second)
                Log("[bt] near here, not sent yet: gazer loc %08X (field %08X %s) state %d pos_ok %d me_ok %d flags %08X game %.1f m", z.loc_hash,
                    z.field, R::FieldName(z.field), z.state, z.pos_ok ? 1 : 0, c.me_ok ? 1 : 0, z.flags, z.pdist);
            continue;
        }
        if (!g_s_gate.Take(z.field, z.located ? z.loc_hash : z.id, now_ms)) {
            ++g_s.held_noticed;
            continue;
        }
        R::BtsNoticed n;
        n.kind = R::kNoticeNear;
        n.near_n = static_cast<std::uint8_t>(std::min(c.near_n, 255));
        n.field = z.field;
        n.loc = z.loc_hash;
        for (int i = 0; i < 3; ++i) n.gazer[i] = static_cast<float>(z.pos[i]), n.player[i] = static_cast<float>(c.me[i]);
        n.game_dist = z.pdist;
        if (!SendNoticed(n, "near", z.trig) && g_s_budget.Take(now_ms))
            Log("[bt] near here: gazer loc %08X (field %08X %s), %.1f m; not sent (%s)", z.loc_hash, z.field, R::FieldName(z.field), z.pdist,
                g_s_peer ? "the link did not take it" : "no partner hello");
    }
    g_s_prev_flags.swap(flags);
    if (unresolved > g_s_unres_max) {
        g_s_unres_max = unresolved;
        Log("[bt] census: %d gazer(s) without an actor skipped (of %d; the most so far)", unresolved, static_cast<int>(c.g.size()));
    }
    // the hunter area: this game's player found (never seen in the probe runs; kept as the stronger kind)
    if (c.hunter_ok && c.hunter_area && !g_s_prev_hunter && c.me_ok && g_s_gate.Take(0xFFFFFFFFu, 0xFFFFFFFEu, now_ms)) {
        double dn = -1;
        const Gazer* z = Nearest(c, &dn);
        R::BtsNoticed n;
        n.kind = R::kNoticeFound;
        n.near_n = static_cast<std::uint8_t>(std::min(c.near_n, 255));
        n.field = z ? z->field : 0;
        n.loc = z ? z->loc_hash : 0;
        for (int i = 0; i < 3; ++i) n.gazer[i] = static_cast<float>(z ? z->pos[i] : c.me[i]), n.player[i] = static_cast<float>(c.me[i]);
        n.game_dist = z ? z->pdist : -1.0f;
        SendNoticed(n, "found: hunter area", z ? z->trig : -1.0);
    }
    if (c.hunter_ok) g_s_prev_hunter = c.hunter_area;
    // round 30 S1: this game's player's trouble (TROUBLE) and the gazers sensing them (GZST)
    TroubleCensus(now_ms, c);
    GzstCensus(now_ms, c);
    // FIELD: on a change (at most 1 Hz), else every 10 s
    g_s_here = m;
    g_s_have_here = true;
    if (g_s_peer && ((!g_s_sent_any || !R::SameContent(m, g_s_last_sent)) || now_ms - g_s_field_ms >= 10000.0)) {
        std::uint8_t b[R::kBtsMaxSize];
        const std::uint32_t seq = g_s_seq;
        if (Send(b, R::EncodeField(seq, m, b, sizeof(b)))) {
            ++g_s.tx_field;
            if (!g_s_sent_any)
                Log("[bt] FIELD sent #1: %u records (unset %u, ids %08X), kills %08X, %u live gazers (locp %08X), %u live fields, epoch %u (seq %u)",
                    m.fields, m.unset, m.ids, m.kills, m.live, m.locs, m.n, m.epoch, seq);
            g_s_sent_any = true;
            g_s_last_sent = m;
            g_s_field_ms = now_ms;
        }
    }
    CompareNow(now_ms);
}

// ---------------------------------------------------------------- received BTS1 (game thread)
void OnHello(const std::uint8_t* b, std::size_t n) {
    R::BtsHello h;
    if (!R::DecodeHello(b, n, h)) {
        ++g_s.bad;
        return;
    }
    ++g_s.rx_hello;
    g_s_peer_level = h.level, g_s_peer_role = h.role, g_s_peer_caps = h.caps;
    if (h.role == g_role) {
        Log("[bt] sync: partner hello says role %s as we do: relay off this link session", RoleText(h.role));
        g_s_peer = false;
        return;
    }
    g_s_peer = true;
    Log("[bt] sync: partner hello: bt_sync %u, role %s, caps 0x%X; relay on (FIELD, NOTICED%s%s)", h.level, RoleText(h.role), h.caps,
        (h.caps & R::kCapVoid) ? ", VOID" : ", VOID (its hook is out)", (h.caps & R::kCapResetApply) ? ", RESET applied there" : "");
    // round 30 S1 / S2: each new type only to a partner whose hello names it (an older build never gets one)
    const bool tr = g_trouble_on && (h.caps & R::kCapTrouble), gz = g_trouble_on && (h.caps & R::kCapGzst);
    const bool kill = KillSendable() && (h.caps & R::kCapKill);
    Log("[bt] sync: TROUBLE %s, GZST %s, KILL %s (partner kills applied here: %s; mine applied there: %s)", tr ? "on" : "off", gz ? "on" : "off",
        kill ? "on" : "off", KillApplyHere() ? "yes" : "no", (h.caps & R::kCapKillApply) ? "yes" : "no");
    if (kill) InstallKillHook("the kill relay");
}

void OnField(double now_ms, const std::uint8_t* b, std::size_t n) {
    R::FieldMsg m;
    if (!R::DecodeField(b, n, m)) {
        ++g_s.bad;
        return;
    }
    ++g_s.rx_field;
    if (!g_s_have_there) Log("[bt] FIELD received #1: %u records (unset %u, ids %08X), kills %08X, %u live gazers, %u live fields, epoch %u", m.fields, m.unset,
                             m.ids, m.kills, m.live, m.n, m.epoch);
    g_s_there = m;
    g_s_have_there = true;
    CompareNow(now_ms);
}

// A load in progress, or no live local player now (the census state may be from before it: SyncTick clears it when the player
// goes, but a RESET drained in the same tick must see the live state).
bool LoadingNow() {
    std::uint64_t flags = 0;
    std::uint32_t mask = 0;
    if (game::ReadStopGates(&flags, &mask) && mask != 0) return true;
    double me[3];
    return !LocalPos(me);
}

// The world is being played now (dev_tools' Playing): Sam awake, no menu / cutscene / scripted scene.
const char* PlayingWhy() {
    game::Entity* e = nullptr;
    if (!game::TryGetLocalPlayer(&e) || !e) return "no local player";
    std::uint64_t flags = 0;
    if (!mem::Rd(reinterpret_cast<std::uintptr_t>(e) + 0x88, flags) || (flags & 0x100)) return "Sam is asleep (loading, cutscene or menu)";
    std::uint64_t state = 0;
    std::uint32_t load = 0;
    if (!game::ReadStopGates(&state, &load)) return "the game state is unreadable";
    if (state & game::kStoppingGameEventMask) return "a menu, cutscene or scripted scene is running";
    return nullptr;
}

void OnReset(double now_ms, const std::uint8_t* b, std::size_t n) {
    R::BtsReset r;
    if (!R::DecodeReset(b, n, r)) {
        ++g_s.bad;
        return;
    }
    ++g_s.rx_reset;
    const char* why = nullptr;
    if (g_role != 2) why = "the host owns the field state (a guest's reset is not applied here)";
    else if (g_sync < 2) why = "bt_sync=1 here (2 applies it)";
    else if (!automation::SavesRedirected()) why = "this game plays on its own real save (host_save=0): a partner's reset never changes it";
    else if (g_sync_dead.load()) why = "the sync is stopped";
    else if (g_applied_epoch && !R::EpochNewer(r.epoch, g_applied_epoch)) why = "not newer than the last applied";
    else if (!g_s_me_ok || !g_s_have_rec) why = "no census here yet (not in a world)";
    else if (LoadingNow()) why = "loading (no live player, or a load in progress)";
    else if (const char* p = PlayingWhy()) why = p;
    else if (g_s_prev_hunter) why = "in a BT encounter here (the hunter area)";
    else if (g_s_rec.fields != r.fields || g_s_rec.ids != r.ids) why = "a different record set here (another area or not loaded)";
    else if (!mem::Q(A(kGazerGlobal))) why = "no gazer manager here";
    else if (!ResetUsable()) why = "the ResetAllGazers byte checks failed";
    if (why) {
        ++g_s.refused;
        Log("[bt] RESET epoch %u from the %s: not applied: %s", r.epoch, RoleText(g_s_peer_role), why);
        return;
    }
    const int before = g_s_rec.unset;
    if (!CallReset(A(kResetThunk))) {
        ++g_s.refused;
        g_reset_check = 0;
        Log("[bt] RESET epoch %u: ResetAllGazers FAULTED; resets are not applied again this run", r.epoch);
        return;
    }
    ++g_s.applied;
    g_applied_epoch = r.epoch;
    g_s_applied_ms = now_ms;
    g_reset_ms = now_ms;  // round 30 S2
    Log("[bt] RESET epoch %u applied (ResetAllGazers; %d records, %d unset before)", r.epoch, g_s_rec.fields, before);
}

void OnNoticed(double now_ms, const std::uint8_t* b, std::size_t n) {
    R::BtsNoticed m;
    if (!R::DecodeNoticed(b, n, m)) {
        ++g_s.bad;
        return;
    }
    ++g_s.rx_noticed;
    const double gz[3] = {m.gazer[0], m.gazer[1], m.gazer[2]}, pp[3] = {m.player[0], m.player[1], m.player[2]};
    char here[96] = "not streamed here";
    const auto it = g_s_locs.find(m.loc);
    if (m.kind == R::kNoticeTest) std::snprintf(here, sizeof(here), "a test");
    else if (it != g_s_locs.end())
        std::snprintf(here, sizeof(here), "live (id %05X, you %.1f m from it)", it->second.id,
                      (it->second.pos_ok && g_s_me_ok) ? R::Dist3(it->second.pos, g_s_me) : -1.0);
    const char* kind = m.kind == R::kNoticeNear ? "near" : m.kind == R::kNoticeFound ? "found" : "test";
    Log("[bt] partner sensed (%s): gazer loc %08X (field %08X %s) at (%.1f %.1f %.1f), the partner %.1f m from it (game %.1f m), you %.1f m; here: %s; "
        "%u near there",
        kind, m.loc, m.field, R::FieldName(m.field), gz[0], gz[1], gz[2], R::Dist3(gz, pp), m.game_dist, g_s_me_ok ? R::Dist3(gz, g_s_me) : -1.0, here,
        m.near_n);
    // round 30 S1: with the markers on and a partner sending GZST / TROUBLE, the in-world marks say this (no toast)
    if (m.kind != R::kNoticeTest && btmarks::On() && (g_s_peer_caps & R::kCapGzst)) {
        if (g_s_budget.Take(now_ms)) Log("[bt] partner sensed: no toast (the markers show it)");
        return;
    }
    Toast(now_ms, m.kind == R::kNoticeFound ? "The BTs found your partner" : "A BT senses your partner");
}

void OnVoid(double now_ms, const std::uint8_t* b, std::size_t n) {
    R::BtsVoid v;
    if (!R::DecodeVoid(b, n, v)) {
        ++g_s.bad;
        return;
    }
    ++g_s.rx_void;
    const bool test = (v.flags & R::kVoidTest) != 0;
    const double p[3] = {v.pos[0], v.pos[1], v.pos[2]};
    Log("[bt] partner voidout%s: at (%.1f %.1f %.1f), reason %u %s, source %s, the partner %.0f m from it, you %.0f m (their far-crater distance %.0f m); "
        "notice only (no game effect)",
        test ? " (test)" : "", p[0], p[1], p[2], v.reason, R::VoidReasonName(v.reason), R::VoidSourceName(v.source), v.player_dist,
        g_s_me_ok ? R::Dist3(p, g_s_me) : -1.0, v.far_dist);
    // round 30: the toast only when the partner's own game went to GameOver within 5 s (VoidVerdict); resolved in SyncTick
    PendVoid pv;
    pv.void_ms = now_ms, pv.test = test, pv.source = v.source;
    if (g_s_pend_n < kPendVoids) g_s_pend[g_s_pend_n++] = pv;
    else Log("[bt] partner voidout verdict: %u already pending: log only; source %s", kPendVoids, R::VoidSourceName(v.source));
    ResolveVoids(now_ms);
}

void NewSession(std::uint32_t gen);

void SyncDrain(double now_ms) {
    static std::uint8_t buf[1100];
    std::size_t n = 0;
    std::uint32_t gen = 0;
    for (int k = 0; k < 32 && link::PopBtMessage(buf, sizeof(buf), n, gen); ++k) {
        // the message's generation is the authority: an older one is stale; a newer one (queued before Generation() moved on)
        // starts that session here first, so its once-only HELLO is kept
        if (static_cast<std::int32_t>(gen - g_s_gen) < 0) {
            ++g_s.stale;
            continue;
        }
        if (gen != g_s_gen) NewSession(gen);
        R::BtsHeader h;
        if (!R::DecodeBtsHeader(buf, n, h)) {
            ++g_s.bad;
            if (g_s_budget.Take(now_ms)) Log("[bt] sync: a bad BTS1 message (%zu B, type %u) dropped", n, n > 4 ? buf[4] : 0u);
            continue;
        }
        if (h.type == R::kBtsHello) {
            OnHello(buf, n);
            continue;
        }
        if (!g_s_peer) {  // nothing before the partner's hello is taken (it is sent first, in order)
            ++g_s.stale;
            continue;
        }
        switch (h.type) {
            case R::kBtsField: OnField(now_ms, buf, n); break;
            case R::kBtsReset: OnReset(now_ms, buf, n); break;
            case R::kBtsNoticed: OnNoticed(now_ms, buf, n); break;
            case R::kBtsVoid: OnVoid(now_ms, buf, n); break;
            case R::kBtsTrouble: OnTrouble(now_ms, buf, n); break;
            case R::kBtsGzst: OnGzst(now_ms, buf, n); break;
            case R::kBtsKill: OnKill(now_ms, buf, n); break;
            default: ++g_s.bad; break;
        }
    }
}

void NewSession(std::uint32_t gen) {
    g_s_gen = gen;
    g_s_down_ms = -1;  // round 30: a GameOver from the last link session is not this one's
    if (g_s_pend_n)    // round 30 review: nor is a VOID still pending from it (it could meet this session's GameOver)
        Log("[bt] partner voidout verdict: %u pending dropped (a new link session): log only", g_s_pend_n);
    g_s_pend_n = 0;
    g_s_beacon = g_s_hello_sent = g_s_peer = g_s_wait_logged = g_s_hello_wait_logged = false;
    g_s_seq = 0;
    g_applied_epoch = 0;
    g_s_applied_ms = -1e18;
    g_s_have_there = g_s_sent_any = false;
    g_s_field_ms = -1e18;
    g_s_cmp_key = 0;
    g_s_peer_level = g_s_peer_role = 0;
    g_s_peer_caps = 0;
    S12Session();
}

unsigned PendingKills();
void SyncStatus(const char* why) {
    Log("[bt] sync status (%s): bt_sync %d role %s, partner %s (bt_sync %d), voidout hook %s; sent hello %u field %u noticed %u (held %u) void %u reset %u; "
        "got hello %u field %u noticed %u void %u reset %u (applied %u, refused %u); compares %u, resets here %u, voidout calls %u (lost %u), bad %u, "
        "stale %u, unsent %u; trouble sent %u got %u, gzst sent %u got %u, kill sent %u (not sent %u) got %u (applied %u, refused %u, pending %u), "
        "world events lost %u",
        why, g_sync, RoleText(g_role), g_s_peer ? "on" : "none", g_s_peer_level, g_void_text, g_s.tx_hello, g_s.tx_field, g_s.tx_noticed, g_s.held_noticed,
        g_s.tx_void, g_s.tx_reset, g_s.rx_hello, g_s.rx_field, g_s.rx_noticed, g_s.rx_void, g_s.rx_reset, g_s.applied, g_s.refused, g_s.compares,
        g_s.resets_here, g_void_calls.load(), g_void_lost.load(), g_s.bad, g_s.stale, g_s.unsent, g_s2.tx_trouble, g_s2.rx_trouble, g_s2.tx_gzst,
        g_s2.rx_gzst, g_s2.tx_kill, g_s2.unsent_kill, g_s2.rx_kill, g_s2.kill_applied, g_s2.kill_refused, PendingKills(), g_s2.events_lost);
}

void SyncTick(double now_ms) {
    if (g_sync_dead.load()) return;
    const std::uint32_t gen = link::Generation();
    if (static_cast<std::int32_t>(gen - g_s_gen) > 0) NewSession(gen);  // only a newer one (a message may have moved g_s_gen on)
    SyncDrain(now_ms);
    // this game's voidouts (the detour's records): every one logged, relayed once the partner said hello
    VoidRec vr[kVoidRing];
    const unsigned nv = TakeVoids(vr, kVoidRing);
    for (unsigned i = 0; i < nv; ++i) SendVoid(now_ms, vr[i], false);
    // round 30: the partner's GameOver (beacon) and the pending VOID verdicts
    if (g_s_peer) SampleDown(now_ms);
    if (g_s_pend_n) ResolveVoids(now_ms);
    S12Tick(now_ms);  // round 30 S1 / S2: the trouble view's expiry, the test sequence, the partner kills (4 Hz)
    if (now_ms < g_s_next_ms) return;
    g_s_next_ms = now_ms + 1000.0;
    if (link::GetInfo().connected) {
        if (!g_s_beacon) {
            shotw::Beacon b;
            double age = 0;
            g_s_beacon = shots::PeerBeacon(&b, &age);
            if (!g_s_beacon && !g_s_wait_logged) {
                g_s_wait_logged = true;
                Log("[bt] sync: waiting for the partner's SEH1 beacon (it proves channel 6) before the hello");
            }
        }
        if (g_s_beacon && !g_s_hello_sent) SendHello(now_ms);
        if (g_s_hello_sent && !g_s_peer && !g_s_hello_wait_logged && now_ms - g_s_hello_ms > 15000.0) {
            g_s_hello_wait_logged = true;
            Log("[bt] sync: no partner hello after 15 s (its bt_sync is 0, or an older build): relay off");
        }
    }
    double me[3];
    if (!LocalPos(me)) {  // the title, a load: the census state is from before it (a RESET must not be checked against it)
        g_s_me_ok = g_s_have_rec = false;
        return;
    }
    if (!(Check() & kMgr)) {
        g_s_me_ok = g_s_have_rec = false;
        if (!g_s_off_logged) Log("[bt] sync off: %s", g_off[0] ? g_off : "byte checks failed");
        g_s_off_logged = true;
        return;
    }
    SyncCensus(now_ms);
}

// ================================================================ round 30 BT S0: the world probes (bt_probe 9..14; hidden runs only)
// Nothing here is installed or called outside a hidden run (automation::Enabled()) and the bt_probe action. The game-changing ones
// (10 kill, 11 crater, 14 hunter area) also need a sandboxed save (automation::SavesRedirected()). Every address is byte-checked
// on the first probe op (ProbeCheck), every game call and write is SEH-guarded.
//   P1 (op 9)  a lazy pass-through detour on the gazer update FUN_142995210 counts its calls on the Tick thread and on others;
//   P2 (op 10) a kill armed for one gazer, applied inside that detour before the original (the update's own rain-gone kill);
//   P3 (op 11) the guarded crater: the voidout called with its GameOver flow call FUN_14341F840 dropped (a thread-local gate);
//   P4 (op 12) the weather rows, the rain at Sam, the gazer manager's +0x238 / +0x239;
//   P7 (op 14) the hunter area FUN_1429B8D60 placed ahead of Sam, then watched for 30 s.
// The gazer update hook (P1 / P2) is bt_world.cpp's (round 30 S2): one detour on FUN_142995210 for the probes and the kill relay.
// The gazer's health +0x180 and the last update's health +0x2B0 (copied from +0x180 by FUN_14299A3D0 at the end of each update).
constexpr std::uintptr_t kGzHealth = 0x180, kGzPrevHealth = 0x2B0;
// the voidout's insides: the crater slot +0x2B0 (an entity ref, entity = ref - 0x20), the early-return query FUN_143409690 on
// [0x147C0D6C0] kind 4, and the GameOver flow FUN_14341F840 (8 arguments: 4 integer registers and 4 stack slots, passed verbatim)
constexpr std::uintptr_t kMgrCraterSlot = 0x2B0, kFlowGlobal = 0x147C0D6C0, kFlowQuery = 0x143409690, kFlowFn = 0x14341F840;
// the rain at a point: float FUN_142E0C3F0(obj, const double pos[3]), obj = [[0x144C73850]] (the gazer update's own read, 0x1429957A4)
constexpr std::uintptr_t kRainGlobal = 0x144C73850, kRainFn = 0x142E0C3F0;
// the weather table (dev_tools' global): row = wg + ((i32 [wg+0x64A0] * 0x40 + 0x18 + region) * 0x5C), +0 the forecast type
constexpr std::uintptr_t kWeatherGlobal = 0x147BF83B8, kWeatherSlot = 0x64A0;
constexpr std::uintptr_t kMgrTimefall = 0x238, kMgrInArea = 0x239;  // FUN_1429A3F50's writes (0x1429A4958 / 0x1429A4A41)
// the hunter area FUN_1429B8D60(hunter manager, const double pos[3], char gold, u8): returns the area entity (or 0 without a
// resource); an area already up is moved
constexpr std::uintptr_t kHunterAreaFn = 0x1429B8D60;

enum PBit : unsigned { kPbGz = 1u, kPbKill = 2u, kPbCrater = 4u, kPbWeather = 8u, kPbHunter = 16u };
struct PSite {
    std::uintptr_t va;
    const char* sig;
    const char* name;
    unsigned bits;
};
// clang-format off
constexpr PSite kProbeSites[] = {
    // (round 30 S2: the gazer update's sites, P1 / P2's, are bt_world.cpp's checks now: one hook, one set of checks)
    {0x14299B1B0, "48 8B C4 48 89 78 18 41 54 41 56 41 57 48 81 EC 80 00 00 00 48 83 79 68 00 45 0F B6 F9 45 0F B6 E0 4C 8B F2 48 8B F9 "
                  "0F 84 AE 01 00 00", "the voidout FUN_14299B1B0 (+0x68)", kPbCrater},
    {0x14299B22D, "C5 FA 10 42 54", "the voidout's far distance [+0x68] +0x54", kPbCrater},
    {0x14299B24F, "48 81 C7 B0 02 00 00", "the voidout's crater slot +0x2B0", kPbCrater},
    {0x14299B30B, "48 8B 0D AE 23 27 05 B2 04 E8 77 E3 A6 00 48 8B B4 24 A8 00 00 00 84 C0 75 5E", "the voidout's early return (kind 4)", kPbCrater},
    {kFlowQuery, "48 8B 41 10 48 63 49 08 4C 8D 04 C8 49 3B C0 74 1C 48 8B 08 48 85 C9 74 0B 38 51 11 75 06 F6 41 14 01 75 06",
     "the query FUN_143409690 (+0x10 / +0x08, entry +0x11 / +0x14)", kPbCrater},
    {kFlowFn, "40 53 48 83 EC 70 48 89 6C 24 68 48 8D 99 88 05 00 00", "the GameOver flow FUN_14341F840", kPbCrater},
    {0x1429957A4, "48 8B 05 ?? ?? ?? ?? 49 8B D7 48 8B 08 E8", "the update's rain read", kPbWeather},
    {kRainFn, "48 83 EC 48 C5 FB 10 42 10 C5 FB 5C 49 18 C5 FB 10 52 08 C5 EB 5C 41 10", "the rain at a point FUN_142E0C3F0", kPbWeather},
    {0x142E1FE79, "66 3B F0 75 29 48 63 8B A0 64 00 00 48 C1 E1 06 48 83 C1 18 40 0F B6 C5 48 03 C8 40 0F B6 C7 48 6B D1 5C 89 04 1A",
     "the weather rows", kPbWeather},
    {0x1429A4958, "48 8D 9F A0 00 00 00 88 87 38 02 00 00", "the gazer manager +0x238", kPbWeather},
    {0x1429A4A41, "88 87 39 02 00 00 48 8B 05", "the gazer manager +0x239", kPbWeather},
    {kHunterAreaFn, "40 55 53 41 54 41 55 41 56 48 8B EC 48 83 EC 70 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 E8 48 83 B9 38 01 00 00 00",
     "the hunter area FUN_1429B8D60", kPbHunter},
};
// clang-format on
struct PRip {
    std::uintptr_t insn;
    unsigned off, len;
    std::uintptr_t want;
    const char* what;
    unsigned bits;
};
constexpr PRip kProbeRips[] = {
    {0x14299B30B, 3, 7, kFlowGlobal, "the voidout's query object 0x147C0D6C0", kPbCrater},
    {0x14299B314, 1, 5, kFlowQuery, "the voidout's call to FUN_143409690", kPbCrater},
    {0x14299B37E, 1, 5, kFlowFn, "the voidout's call to FUN_14341F840", kPbCrater},
    {0x1429957A4, 3, 7, kRainGlobal, "the rain object 0x144C73850", kPbWeather},
    {0x1429957B1, 1, 5, kRainFn, "the update's call to FUN_142E0C3F0", kPbWeather},
    {0x142E1FE34, 3, 7, kWeatherGlobal, "the weather global 0x147BF83B8", kPbWeather},
    {0x1429C4EFA, 1, 5, kHunterAreaFn, "the call to FUN_1429B8D60 (FUN_1429C49D0)", kPbHunter},
};
bool g_p_checked = false;
unsigned g_p_bits = 0;

unsigned ProbeCheck() {
    if (g_p_checked) return g_p_bits;
    g_p_checked = true;
    if (!mem::Base()) {
        Log("[bt] probe byte checks: ds.exe base unknown (the world probes refuse)");
        return g_p_bits = 0;
    }
    unsigned bits = kPbGz | kPbKill | kPbCrater | kPbWeather | kPbHunter;
    int ok = 0, total = 0;
    for (const PSite& s : kProbeSites) {
        ++total;
        if (mem::CodeMatches(A(s.va), s.sig)) {
            ++ok;
            continue;
        }
        Log("[bt] probe byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
        bits &= ~s.bits;
    }
    for (const PRip& r : kProbeRips) {
        ++total;
        if (mem::RipTarget(A(r.insn), r.off, r.len) == A(r.want)) {
            ++ok;
            continue;
        }
        Log("[bt] probe byte check FAILED: %s", r.what);
        bits &= ~r.bits;
    }
    const unsigned w = btworld::Check();  // the gazer update's checks (bt_world.cpp, logged there)
    if (!(w & btworld::kBitHook)) bits &= ~kPbGz;
    if (!(w & btworld::kBitKill)) bits &= ~kPbKill;
    if (!(bits & kPbGz)) bits &= ~kPbKill;
    if (!(Check() & kHunter)) bits &= ~kPbHunter;
    if (!(Check() & kMgr)) bits &= ~(kPbKill | kPbCrater | kPbWeather);
    g_p_bits = bits;
    Log("[bt] probe byte checks: %d/%d ok; threads %s, kill %s, crater %s, weather %s, hunter %s", ok, total, (bits & kPbGz) ? "ok" : "OFF",
        (bits & kPbKill) ? "ok" : "OFF", (bits & kPbCrater) ? "ok" : "OFF", (bits & kPbWeather) ? "ok" : "OFF", (bits & kPbHunter) ? "ok" : "OFF");
    return bits;
}

bool MhInit(char* msg, std::size_t cap) {
    const MH_STATUS init = MH_Initialize();
    if (init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED) return true;
    Put(msg, cap, "bt probe: MinHook %s", MH_StatusToString(init));
    return false;
}

// ---------------------------------------------------------------- P1 / P2: the gazer update hook (bt_world.cpp since round 30 S2)
bool InstallGz(char* msg, std::size_t cap) {
    if (btworld::HookIn()) return true;
    if (!(ProbeCheck() & kPbGz)) return Put(msg, cap, "bt probe: the gazer update byte checks failed (log)"), false;
    return btworld::InstallHook("the world probes", msg, cap);
}
void ThreadsLine() {
    unsigned tick = 0, other = 0, k = 0;
    bool over = false;
    btworld::Threads(&tick, &other, &k, &over);
    Log("[bt] gz update threads: tick %u, other %u (threads %u%s)", tick, other, k, over ? "+" : "");
}

// P2's watch (game thread)
// P2's watch (the outcome comes from bt_world's event ring: OnProbeEvent)
constexpr int kKillFault = btworld::kApplyFault;
// An armed kill's refusal in the update (review crash 3: kApplyStale, another gazer at that address).
const char* ApplyText(int r) {
    return r == btworld::kApplyFault ? "FAULTED" : r == btworld::kApplyStale ? "another gazer at that address (its id changed)" : R::KillGuardText(r);
}
struct KillWatch {
    bool on = false, done = false;
    std::uint32_t loc = 0, field = 0;
    std::uintptr_t actor = 0;
    double arm_ms = 0, done_ms = 0;
    std::int32_t list0 = 0, killed0 = -1;
    int result = btworld::kNotArmed;
    std::uint32_t prev_after = 0, flags_after = 0;
} g_kw;
void OnProbeEvent(const btworld::Event& e) {
    if (!g_kw.on || g_kw.done || e.actor != g_kw.actor || e.applied == btworld::kNotArmed) return;
    g_kw.done = true;
    g_kw.result = e.applied;
    if (e.read_ok) std::memcpy(&g_kw.prev_after, &e.prev_after, 4), g_kw.flags_after = e.flags;
    else g_kw.prev_after = g_kw.flags_after = 0xFFFFFFFFu;
}

// ---------------------------------------------------------------- P3: the guarded crater
using FlowFn = void (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
FlowFn g_flow_orig = nullptr;
bool g_flow_hook = false;
bool g_crater_dead = false;  // FATAL-GUARD (or EmergencyStop): no more P3 this run
std::atomic<unsigned> g_flow_drops{0};
void FlowDetour(void* a, std::uint64_t b, std::uint64_t c, std::uint64_t d, std::uint64_t e, std::uint64_t f, std::uint64_t g, std::uint64_t h) {
    if (t_crater) {
        g_flow_drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_flow_orig(a, b, c, d, e, f, g, h);
}
// FUN_143409690(obj, 4) replicated with reads: an entry of [obj+0x10] (count i32 [obj+8]) with +0x11 == 4 and +0x14 bit 0.
bool FlowEarlyNow() {
    const std::uintptr_t obj = mem::Q(A(kFlowGlobal));
    std::int32_t n = 0;
    if (!obj || !mem::Rd(obj + 8, n) || n <= 0 || n > 4096) return false;
    const std::uintptr_t list = mem::Q(obj + 0x10);
    if (!list) return false;
    for (std::int32_t i = 0; i < n; ++i) {
        const std::uintptr_t e = mem::Q(list + static_cast<std::uintptr_t>(i) * 8);
        std::uint8_t kind = 0, fl = 0;
        if (e && mem::Rd(e + 0x11, kind) && kind == 4 && mem::Rd(e + 0x14, fl) && (fl & 1)) return true;
    }
    return false;
}
bool CallVoidGuarded(std::uintptr_t fn, void* mgr, const double* pos) noexcept {
    bool ok = false;
    __try {
        __try {
            t_crater = true;
            reinterpret_cast<void (*)(void*, const double*, std::uint8_t, std::uint8_t)>(fn)(mgr, pos, 0, 0);
            ok = true;
        } __finally {
            t_crater = false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}
struct CraterWatch {
    bool on = false;
    double start_ms = 0, next_ms = 0;
    int n = 0;
} g_cw;

// ---------------------------------------------------------------- P4: the weather
float RainAt(std::uintptr_t obj, const double* pos, bool* ok) noexcept {
    __try {
        const float r = reinterpret_cast<float (*)(void*, const double*)>(A(kRainFn))(reinterpret_cast<void*>(obj), pos);
        *ok = true;
        return r;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ok = false;
        return 0;
    }
}

// ---------------------------------------------------------------- P7: the hunter area
std::uintptr_t CallHunterArea(std::uintptr_t hm, const double* pos, bool* ok) noexcept {
    __try {
        const std::uintptr_t e = reinterpret_cast<std::uintptr_t (*)(void*, const double*, char, std::uint8_t)>(A(kHunterAreaFn))(
            reinterpret_cast<void*>(hm), pos, 0, 0);
        *ok = true;
        return e;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ok = false;
        return 0;
    }
}
struct HunterWatch {
    bool on = false;
    double start_ms = 0, next_ms = 0;
    int n = 0;
} g_hw;

bool SamFacing(double (&me)[3], double* fx, double* fy) {
    game::Entity* e = nullptr;
    game::WorldTransform wt{};
    if (!game::TryGetLocalPlayer(&e) || !e || !game::TryGetTransform(e, &wt)) return false;
    me[0] = wt.pos.x, me[1] = wt.pos.y, me[2] = wt.pos.z;
    *fx = wt.rot.m[1][0], *fy = wt.rot.m[1][1];
    return R::PosSane(me);
}

const Field* FindField(const Census& c, std::uint32_t id) {
    for (const Field& f : c.f)
        if (f.id == id) return &f;
    return nullptr;
}

// ---------------------------------------------------------------- the ops (game thread, the action)
bool OpKill(const double (&v)[3], char* msg, std::size_t cap) {
    if (!automation::SavesRedirected()) return Put(msg, cap, "bt probe kill: only on a sandboxed save (a hidden run)"), false;
    std::uint32_t loc = 0;
    if (!R::LocArg(v[1], v[2], &loc)) return Put(msg, cap, "bt probe kill: 10 <locator hi16> <lo16> (0 0 = the nearest live gazer)"), false;
    if (!(ProbeCheck() & kPbKill) || !(Check() & kActor)) return Put(msg, cap, "bt probe kill: off (byte checks; log)"), false;
    if (g_kw.on) return Put(msg, cap, "bt probe kill: one is armed already"), false;
    double me[3];
    if (!LocalPos(me)) return Put(msg, cap, "bt probe kill: no local player (not in a world)"), false;
    Census c;
    TakeCensus(g_now_ms, c);
    const Gazer* z = nullptr;
    double best = -1;
    for (const Gazer& g : c.g) {
        if (!g.actor) continue;
        if (loc) {
            if (g.located && g.loc_hash == loc) z = &g;
            continue;
        }
        if (g.state != 0 || !g.pos_ok) continue;
        const double d = R::Dist3(g.pos, me);
        if (!z || d < best) z = &g, best = d;
    }
    if (!z) return Put(msg, cap, "bt probe kill: no live gazer %s", loc ? "with that locator" : "here"), false;
    float h = 0, prev = 0;
    std::uint32_t flags = 0;
    std::int32_t state = -1;
    if (!mem::Rd(z->actor + kActState, state) || !mem::Rd(z->actor + kGzHealth, h) || !mem::Rd(z->actor + kGzPrevHealth, prev) ||
        !mem::Rd(z->actor + kActFlags, flags))
        return Put(msg, cap, "bt probe kill: the gazer is unreadable"), false;
    const int g = R::KillGuard(state, h, prev, flags);
    if (g != R::kKillOk) {
        Log("[bt] probe kill loc %08X: refused: %s (state %d, health %.2f, last %.2f, flags 0x%X)", z->loc_hash, R::KillGuardText(g), state, h, prev, flags);
        return Put(msg, cap, "bt probe kill: refused: %s", R::KillGuardText(g)), false;
    }
    if (!InstallGz(msg, cap)) return false;
    const Field* f = FindField(c, z->field);
    g_kw = KillWatch{};
    g_kw.on = true, g_kw.loc = z->loc_hash, g_kw.field = z->field, g_kw.actor = z->actor, g_kw.arm_ms = g_now_ms;
    g_kw.list0 = c.list, g_kw.killed0 = f ? f->killed : -1;
    if (!btworld::ArmKill(z->actor, z->id, R::kTagProbe)) {
        g_kw.on = false;
        return Put(msg, cap, "bt probe kill: not armed (the kill slots are full or that gazer is armed already)"), false;
    }
    Log("[bt] probe kill loc %08X (id %05X, field %08X %s): armed (health %.2f, last %.2f, flags 0x%X; reason 0x%08X at its next update)", z->loc_hash,
        z->id, z->field, R::FieldName(z->field), h, prev, flags, R::kKillRainGone);
    return Put(msg, cap, "bt probe kill: armed for loc %08X (log)", z->loc_hash), true;
}

bool OpCrater(const double (&v)[3], char* msg, std::size_t cap) {
    if (!automation::SavesRedirected()) return Put(msg, cap, "bt probe crater: only on a sandboxed save (a hidden run)"), false;
    if (g_crater_dead) return Put(msg, cap, "bt probe crater: refused for the rest of the run (FATAL-GUARD or emergency stop)"), false;
    if (!(ProbeCheck() & kPbCrater)) return Put(msg, cap, "bt probe crater: off (byte checks; log)"), false;
    if (g_cw.on) return Put(msg, cap, "bt probe crater: the last one is still watched"), false;
    const double m = v[1] > 0 && std::isfinite(v[1]) ? v[1] : 200.0;
    double me[3], fx = 0, fy = 0, pos[3];
    if (!SamFacing(me, &fx, &fy) || !R::AheadOf(me, fx, fy, m, pos)) return Put(msg, cap, "bt probe crater: no local player transform"), false;
    const std::uintptr_t mgr = mem::Q(A(kGazerGlobal));
    const std::uintptr_t res = mgr ? mem::Q(mgr + kMgrResource) : 0;
    const std::uintptr_t slot = mgr ? mem::Q(mgr + kMgrCraterSlot) : 1;
    float far_m = -1;
    if (res && !(mem::Rd(res + kResFarDist, far_m) && std::isfinite(far_m))) far_m = -1;
    const double dist = R::Dist3(me, pos);
    if (const char* why = R::CraterGate(res != 0, slot == 0 || slot == 0x20, dist, far_m)) {
        Log("[bt] probe crater at (%.1f %.1f %.1f) m %.0f: refused: %s (far %.0f m, slot 0x%llX)", pos[0], pos[1], pos[2], m, why, far_m,
            static_cast<unsigned long long>(slot));
        return Put(msg, cap, "bt probe crater: refused: %s", why), false;
    }
    if (!g_flow_hook) {
        if (!MhInit(msg, cap)) return false;
        void* target = reinterpret_cast<void*>(A(kFlowFn));
        MH_STATUS s = MH_CreateHook(target, reinterpret_cast<void*>(&FlowDetour), reinterpret_cast<void**>(&g_flow_orig));
        if (s == MH_OK) s = MH_EnableHook(target);
        g_flow_hook = s == MH_OK;
        Log("[bt] probe: GameOver flow gate %s (FUN_14341F840, drops a call only inside the probe's crater)", g_flow_hook ? "in" : MH_StatusToString(s));
        if (!g_flow_hook) return Put(msg, cap, "bt probe crater: the flow gate failed: %s", MH_StatusToString(s)), false;
    }
    std::uint8_t first = 0;
    if (!mem::Rd(A(kFlowFn), first) || first != 0xE9) {
        g_crater_dead = true;
        Log("[bt] FATAL-GUARD: FUN_14341F840 does not start with our jmp (0x%02X); bt_probe 11 refused for the rest of the run", first);
        return Put(msg, cap, "bt probe crater: FATAL-GUARD (the flow gate is not in)"), false;
    }
    const unsigned drops0 = g_flow_drops.load();
    const bool early0 = FlowEarlyNow();
    const bool called = CallVoidGuarded(A(kVoidFn), reinterpret_cast<void*>(mgr), pos);
    const unsigned drops = g_flow_drops.load() - drops0;
    const bool early = early0 || FlowEarlyNow();
    if (!called) {
        g_crater_dead = true;
        Log("[bt] probe crater at (%.1f %.1f %.1f) m %.0f: the voidout FAULTED (flow dropped %u); bt_probe 11 refused for the rest of the run", pos[0],
            pos[1], pos[2], m, drops);
        return Put(msg, cap, "bt probe crater: the voidout faulted"), false;
    }
    if (drops != 1 && !early) {
        g_crater_dead = true;
        Log("[bt] FATAL-GUARD: the crater's GameOver flow was neither dropped (%u) nor skipped by the early return; bt_probe 11 refused for the "
            "rest of the run",
            drops);
    }
    const std::uintptr_t after = mem::Q(mgr + kMgrCraterSlot);
    Log("[bt] probe crater at (%.1f %.1f %.1f) m %.0f: flow dropped %u%s, crater entity 0x%llX (+0x2B0)", pos[0], pos[1], pos[2], m, drops,
        early ? " (the early return: kind 4 set)" : "", static_cast<unsigned long long>(after > 0x20 ? after - 0x20 : 0));
    g_cw = CraterWatch{};
    g_cw.on = true, g_cw.start_ms = g_now_ms, g_cw.next_ms = g_now_ms + 1000.0;
    return Put(msg, cap, "bt probe crater: %.0f m ahead, flow dropped %u (log)", m, drops), true;
}

bool OpWeather(char* msg, std::size_t cap) {
    if (!(ProbeCheck() & kPbWeather)) return Put(msg, cap, "bt probe weather: off (byte checks; log)"), false;
    const std::uintptr_t wg = mem::Q(A(kWeatherGlobal));
    std::int32_t slot = -1;
    if (!wg || !mem::Rd(wg + kWeatherSlot, slot) || slot < 0 || slot > 255) return Put(msg, cap, "bt probe weather: no weather manager (no world?)"), false;
    int n = 0, rain = 0, heavy = 0, sunny = 0, mix = 0, other = 0;
    char rows[512] = "";
    std::size_t at = 0;
    for (int r = 0; r < 64; ++r) {
        const std::uintptr_t row = wg + (static_cast<std::uintptr_t>(slot) * 0x40 + 0x18 + static_cast<std::uintptr_t>(r)) * 0x5C;
        std::uint32_t t[3] = {0, 0, 0};
        if (!mem::Read(row, t, sizeof(t))) continue;
        if (t[0] == 0) continue;
        ++n;
        (t[0] == 3 ? rain : t[0] == 5 ? heavy : t[0] == 6 ? sunny : t[0] == 2 ? mix : other)++;
        if (at + 24 < sizeof(rows)) at += static_cast<std::size_t>(std::snprintf(rows + at, sizeof(rows) - at, " %d:%u/%u/%u", r, t[0], t[1], t[2]));
    }
    double me[3];
    bool rain_ok = false;
    float here = 0;
    const std::uintptr_t ro = mem::Q(A(kRainGlobal));
    const std::uintptr_t obj = ro ? mem::Q(ro) : 0;
    if (obj && LocalPos(me)) here = RainAt(obj, me, &rain_ok);
    const std::uintptr_t mgr = mem::Q(A(kGazerGlobal));
    std::uint8_t tf = 0xFF, in_area = 0xFF;
    if (mgr) mem::Rd(mgr + kMgrTimefall, tf), mem::Rd(mgr + kMgrInArea, in_area);
    char rain_txt[24] = "unreadable";
    if (rain_ok) std::snprintf(rain_txt, sizeof(rain_txt), "%.2f", here);
    Log("[bt] weather: area %d, regions %d (rain only %d, heavy %d, sunny %d, mix %d, other %d), rain here %s, mgr timefall %u area %u", slot, n, rain,
        heavy, sunny, mix, other, rain_txt, tf, in_area);
    Log("[bt] weather rows (region:type/+4/+8):%s", n ? rows : " none set");
    return Put(msg, cap, "bt probe weather: area %d, %d regions, rain here %s, timefall %u (log)", slot, n, rain_txt, tf), true;
}

bool OpHunter(const double (&v)[3], char* msg, std::size_t cap) {
    if (!automation::SavesRedirected()) return Put(msg, cap, "bt probe hunter: only on a sandboxed save (a hidden run)"), false;
    if (ride::Carried()) return Put(msg, cap, "bt probe hunter: refused during a carry"), false;
    if (!(ProbeCheck() & kPbHunter)) return Put(msg, cap, "bt probe hunter: off (byte checks; log)"), false;
    const double m = v[1] > 0 && std::isfinite(v[1]) ? v[1] : 30.0;
    double me[3], fx = 0, fy = 0, pos[3];
    if (!SamFacing(me, &fx, &fy) || !R::AheadOf(me, fx, fy, m, pos)) return Put(msg, cap, "bt probe hunter: no local player transform"), false;
    const std::uintptr_t hm = mem::Q(A(kHunterGlobal));
    std::uintptr_t res = 0, area0 = 0;
    if (!hm || !mem::Rd(hm + kHunResource, res) || !mem::Rd(hm + kHunArea, area0)) return Put(msg, cap, "bt probe hunter: no hunter manager"), false;
    if (!res) return Put(msg, cap, "bt probe hunter: the hunter manager has no resource (+0x138)"), false;
    bool ok = false;
    const std::uintptr_t ent = CallHunterArea(hm, pos, &ok);
    std::uintptr_t area1 = 0;
    mem::Rd(hm + kHunArea, area1);
    Log("[bt] probe hunter area at (%.1f %.1f %.1f) m %.0f: FUN_1429B8D60 %s 0x%llX, +0x140 0x%llX -> 0x%llX", pos[0], pos[1], pos[2], m,
        ok ? "returned" : "FAULTED", static_cast<unsigned long long>(ent), static_cast<unsigned long long>(area0), static_cast<unsigned long long>(area1));
    if (!ok) return Put(msg, cap, "bt probe hunter: the call faulted"), false;
    g_hw = HunterWatch{};
    g_hw.on = true, g_hw.start_ms = g_now_ms, g_hw.next_ms = g_now_ms + 1000.0;
    return Put(msg, cap, "bt probe hunter: area %.0f m ahead (%s; log)", m, area1 ? "up" : "not up"), true;
}

bool ProbeOps(int op, const double (&v)[3], char* msg, std::size_t cap) {
    if (!automation::Enabled()) return Put(msg, cap, "bt probe %d: hidden runs only", op), false;
    switch (op) {
        case R::kOpThreads:
            if (!btworld::HookIn()) {
                if (!InstallGz(msg, cap)) return false;
                return Put(msg, cap, "bt probe: the gazer update counter is in (bt_probe 9 again for the counts)"), true;
            }
            ThreadsLine();
            return Put(msg, cap, "bt probe: gz update counts (log)"), true;
        case R::kOpKill: return OpKill(v, msg, cap);
        case R::kOpCrater: return OpCrater(v, msg, cap);
        case R::kOpWeather: return OpWeather(msg, cap);
        case R::kOpHunter: return OpHunter(v, msg, cap);
    }
    return Put(msg, cap, "bt probe %d: not built (9 threads, 10 kill, 11 crater, 12 weather, 14 hunter area)", op), false;
}

// The watchers (game thread, every tick while one runs).
void ProbeTick(double now_ms) {
    btworld::NoteTickThread();
    if (g_kw.on) {
        if (g_kw.done) {
            if (g_kw.done_ms == 0) g_kw.done_ms = now_ms;
            if (now_ms - g_kw.done_ms >= 2000.0) {  // the kill list and the field record after the death processing
                g_kw.on = false;
                const int r = g_kw.result;
                if (r != R::kKillOk) {
                    Log("[bt] probe kill loc %08X: refused in the update: %s", g_kw.loc, ApplyText(r));
                } else {
                    // round 30 review: +0x2B0 after the update only says whether the update ran to its end (0x142996A45's copy
                    // FUN_14299A3D0 writes +0x180, 0 after the kill, into +0x2B0 whether or not the death branch ran). The death
                    // branch's own mark is its last call FUN_1429A31A0 (0x142995FA6, skipped with +0x1B4 bit 0x400000): it kills
                    // into the field record, so the field's kill count rising is the death edge.
                    const std::uint32_t pb = g_kw.prev_after, fl = g_kw.flags_after;
                    float prev = 0;
                    std::memcpy(&prev, &pb, 4);
                    Census c;
                    TakeCensus(now_ms, c);
                    const Field* f = FindField(c, g_kw.field);
                    const char* upd = pb == 0xFFFFFFFFu ? "unread" : prev > 0 ? "returned early" : "ran to its end";
                    char edge[64];
                    if (fl != 0xFFFFFFFFu && (fl & 0x400000u))
                        std::snprintf(edge, sizeof(edge), "n/a (+0x1B4 0x400000: no kill list)");
                    else if (!f || g_kw.killed0 < 0)
                        std::snprintf(edge, sizeof(edge), "n/a (no field record)");
                    else
                        std::snprintf(edge, sizeof(edge), "%s (field killed %+d)", f->killed > g_kw.killed0 ? "seen" : "not seen", f->killed - g_kw.killed0);
                    Log("[bt] probe kill loc %08X: applied, the update %s (+0x2B0 %.3g), death edge %s, list %d -> %d, field killed %d -> %d", g_kw.loc,
                        upd, pb == 0xFFFFFFFFu ? 0.0 : static_cast<double>(prev), edge, g_kw.list0, c.list, g_kw.killed0, f ? f->killed : -1);
                }
            }
        } else if (now_ms - g_kw.arm_ms > 10000.0) {
            if (btworld::Disarm(g_kw.actor)) {  // the detour never took it
                g_kw.on = false;
                Log("[bt] probe kill loc %08X: not applied (the gazer did not update in 10 s)", g_kw.loc);
            }
        }
    }
    if (g_cw.on && now_ms >= g_cw.next_ms) {
        g_cw.next_ms = now_ms + 1000.0;
        ++g_cw.n;
        const std::uintptr_t mgr = mem::Q(A(kGazerGlobal));
        Log("[bt] probe crater +%d s: +0x2B0 0x%llX", g_cw.n, static_cast<unsigned long long>(mgr ? mem::Q(mgr + kMgrCraterSlot) : 0));
        if (g_cw.n >= 20) g_cw.on = false;
    }
    if (g_hw.on && now_ms >= g_hw.next_ms) {
        g_hw.next_ms = now_ms + 1000.0;
        ++g_hw.n;
        const std::uintptr_t hm = mem::Q(A(kHunterGlobal));
        std::uintptr_t area = 0;
        std::int32_t spawns = 0;
        std::uint8_t gold = 0;
        double ap[3] = {0, 0, 0};
        bool ap_ok = false;
        if (hm && mem::Rd(hm + kHunArea, area) && area > 0x20) ap_ok = mem::Read(area - 0x20 + kEntPos, ap, sizeof(ap)) && R::PosSane(ap);
        if (hm) mem::Rd(hm + kHunSpawns, spawns), mem::Rd(hm + kHunGold, gold);
        std::uint64_t state = 0;
        std::uint32_t load = 0;
        const bool st_ok = game::ReadStopGates(&state, &load);
        game::Diagnostics d;
        game::ReadDiagnostics(&d);
        double me[3];
        const bool me_ok = LocalPos(me);
        Log("[bt] probe hunter +%d s: +0x140 0x%llX at (%.1f %.1f %.1f)%s, spawns %d gold %u; game state 0x%llX (GameOver %s, stopping %s), Sam health "
            "%.0f, Sam %.1f m from it",
            g_hw.n, static_cast<unsigned long long>(area), ap[0], ap[1], ap[2], ap_ok ? "" : " (no position)", spawns, gold,
            static_cast<unsigned long long>(state), st_ok ? ((state & (1ull << 13)) ? "yes" : "no") : "?",
            st_ok ? ((state & game::kStoppingGameEventMask) ? "yes" : "no") : "?", d.health_valid ? d.health : -1.0f,
            ap_ok && me_ok ? R::Dist3(ap, me) : -1.0);
        if (g_hw.n >= 30) g_hw.on = false;
    }
}

// ================================================================ round 30 BT S1 / S2 (docs/enemy-sync.md section 24)
// S1: TROUBLE (this game's player hunted, caught or fighting a catcher) and GZST (this game's gazers sensing this game's player) go
// to the partner, whose game logs them, draws markers (bt_markers.cpp) and shows at most one toast per episode (caught, or a
// catcher fight starting). Nothing received changes the game. PULLED is never sent (no pull reading exists yet).
// S2: a gazer a player kills here (bt_world's death edge; btrules::KillRelay) goes as KILL. The partner's game applies it to its own
// copy of that gazer (btrules::KillApplyGate, then bt_world's armed kill: the update's own rain-gone kill, tagged "mirror" so its
// death edge is never sent back), or holds it (up to 60 s) or refuses it, with the reason logged.

R::TroubleTracker g_tr;
R::TroubleView g_tv;
std::uint8_t g_tr_logged = 0;  // the kind last logged as sent
struct TroubleTest {
    bool on = false;
    double t0 = 0;
    int step = -1;
    std::uint16_t episode = 0;
    double pos[3] = {0, 0, 0};
    bool pos_ok = false;
} g_tt;
std::map<std::uint32_t, std::uint8_t> g_gz_sent;    // locator -> bits, the interesting gazers in the last GZST
double g_gz_ms = -1e18;
std::map<std::uint32_t, std::uint8_t> g_gz_logged;  // receiver: locator -> bits last logged
R::EchoRing g_echo;
struct PendKill {
    R::BtsKill k;
    double rx_ms = 0, arm_ms = 0, done_ms = 0, dist = -1;
    const char* last_why = nullptr;
    bool armed = false, done = false;
    int result = btworld::kNotArmed;
    std::uintptr_t actor = 0, scene = 0;
    std::uint32_t id = 0, field_here = 0;
    std::int32_t list0 = 0, killed0 = -1;
};
constexpr unsigned kPendKills = 16;
std::vector<PendKill> g_pk;
double g_pk_next_ms = 0;
struct TestKill {
    bool on = false, done = false;
    std::uint32_t loc = 0, field = 0, id = 0;
    std::uintptr_t actor = 0;
    double arm_ms = 0, done_ms = 0;
    int result = btworld::kNotArmed;
    std::int32_t list0 = 0, killed0 = -1;
} g_tk;

unsigned PendingKills() { return static_cast<unsigned>(g_pk.size()); }

void InstallKillHook(const char* why) {
    if (btworld::HookIn()) return;
    char msg[160] = "";
    if (!btworld::InstallHook(why, msg, sizeof(msg))) Log("[bt] kill relay off: %s", msg[0] ? msg : "the gazer update hook did not go in");
}

// "death edge seen (field killed +1)": the death branch's last call kills into the field record, so its count rising is the edge
// (the +0x2B0 copy alone does not tell it; see ProbeTick).
void EdgeText(const Census& c, std::uint32_t field, std::int32_t killed0, char* out, std::size_t cap) {
    const Field* f = FindField(c, field);
    if (!f || killed0 < 0) std::snprintf(out, cap, "n/a (no field record)");
    else std::snprintf(out, cap, "%s (field killed %+d)", f->killed > killed0 ? "seen" : "not seen", f->killed - killed0);
}

// ---------------------------------------------------------------- S1 sender
void TroubleSend(std::uint8_t kind, std::uint8_t flags, std::uint16_t episode, const double* pos, float hp, std::uint32_t field, std::uint32_t loc,
                 const char* where) {
    R::BtsTrouble t;
    t.kind = kind, t.flags = flags, t.episode = episode, t.hp = hp, t.field = field, t.loc = loc;
    for (int i = 0; i < 3; ++i) t.pos[i] = pos ? static_cast<float>(pos[i]) : 0.0f;
    const std::uint32_t seq = g_s_seq;
    std::uint8_t b[R::kBtsTroubleSize];
    const bool sent = Send(b, R::EncodeTrouble(seq, t, b, sizeof(b)));
    if (sent) ++g_s2.tx_trouble;
    const bool test = (flags & R::kTrfTest) != 0;
    if (kind == g_tr_logged && !test && sent) return;  // a keepalive or a health step: the kind was logged
    g_tr_logged = kind;
    char hp_text[24] = "";
    if (kind == R::kTrCatcher) std::snprintf(hp_text, sizeof(hp_text), " hp %.0f%%", static_cast<double>(hp));
    char gz[48] = "";
    if (loc) std::snprintf(gz, sizeof(gz), ", gazer loc %08X (field %08X)", loc, field);
    Log("[bt] TROUBLE %s%s: %s%s (episode %u%s%s), at (%.1f %.1f %.1f) (%s)%s (seq %u)", sent ? "sent" : "NOT sent", test ? " (test)" : "",
        R::TroubleName(kind), hp_text, episode, (flags & R::kTrfGold) ? ", gold" : "", (flags & R::kTrfCarried) ? ", carried" : "", t.pos[0], t.pos[1],
        t.pos[2], where, gz, seq);
}

void TroubleCensus(double now_ms, const Census& c) {
    if (!g_trouble_on || !g_s_peer || !(g_s_peer_caps & R::kCapTrouble) || g_tt.on) return;
    R::TroubleIn in;
    in.hunted = c.hunter_ok && c.hunter_area;
    const Gazer* caught = nullptr;  // a gazer within 10 m holding this game's player (+0x1B4 0x2000000)
    if (c.me_ok)
        for (const Gazer& z : c.g)
            if (z.actor && z.state == 0 && z.pos_ok && (z.flags & R::kFlagCaught) && R::Dist3(z.pos, c.me) <= 10.0) {
                caught = &z;
                break;
            }
    in.caught = caught != nullptr;
    const Catcher* cat = nullptr;  // an active catcher record (+0xF0 clear) with an entity and health
    for (const Catcher& k : c.c)
        if (k.entity && k.active && k.hp > 0 && (!cat || (k.pos_ok && !cat->pos_ok))) cat = &k;
    in.catcher = cat != nullptr;
    in.catcher_hp = cat ? cat->hp : 0.0f;
    float pct = -1;
    const std::uint8_t kind = g_tr.Step(in, now_ms, &pct);
    if (!kind) return;
    const std::uint8_t flags = static_cast<std::uint8_t>((c.gold ? R::kTrfGold : 0) | (ride::Carried() ? R::kTrfCarried : 0));
    const double* pos = c.me_ok ? c.me : nullptr;
    const char* where = c.me_ok ? "my position" : "no position";
    std::uint32_t field = 0, loc = 0;
    if (kind == R::kTrCatcher && cat && cat->pos_ok) pos = cat->pos, where = "the catcher";
    if (kind == R::kTrCaught && caught) field = caught->field, loc = caught->loc_hash;
    TroubleSend(kind, flags, g_tr.episode, pos, kind == R::kTrCatcher ? pct : -1.0f, field, loc, where);
}

void FillGz(R::GzEntry& e, const Gazer& z, std::uint8_t bits) {
    e.field = z.field, e.loc = z.loc_hash;
    e.state = static_cast<std::uint8_t>(std::clamp(z.state, 0, 255));
    e.mode = z.mode, e.bits = bits, e.approach = z.approach;
    for (int i = 0; i < 3; ++i) e.pos[i] = static_cast<float>(z.pos[i]);
}

// The gazers sensing, reacting to or holding this game's player (nearest first, up to 8), on a change and every second while any;
// a gazer that stopped goes once more with its bits now (the partner drops its marker).
void GzstCensus(double now_ms, const Census& c) {
    if (!g_trouble_on || !g_s_peer || !(g_s_peer_caps & R::kCapGzst)) return;
    struct Cand {
        const Gazer* z;
        double d;
        std::uint8_t bits;
    };
    std::vector<Cand> v;
    for (const Gazer& z : c.g) {
        if (!z.actor || !z.located || !z.pos_ok) continue;
        const std::uint8_t bits = R::GzBits(z.flags, z.state);
        if (R::GzInteresting(bits)) v.push_back({&z, c.me_ok ? R::Dist3(z.pos, c.me) : 1e9, bits});
    }
    std::sort(v.begin(), v.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
    R::BtsGzst m;
    std::map<std::uint32_t, std::uint8_t> now_set;
    for (const Cand& k : v) {
        if (m.n >= R::kBtsGzstMax) break;
        FillGz(m.e[m.n++], *k.z, k.bits);
        now_set[k.z->loc_hash] = k.bits;
    }
    unsigned ended = 0;
    for (const auto& kv : g_gz_sent) {
        if (m.n >= R::kBtsGzstMax) break;  // the rest expire on the partner (3 s)
        if (now_set.count(kv.first)) continue;
        R::GzEntry& e = m.e[m.n++];
        e = R::GzEntry{};
        e.loc = kv.first;
        for (const Gazer& z : c.g)
            if (z.located && z.loc_hash == kv.first) {
                FillGz(e, z, static_cast<std::uint8_t>(R::GzBits(z.flags, z.state) & ~(R::kGzNear | R::kGzReact | R::kGzCaught)));
                if (!z.pos_ok) e.pos[0] = e.pos[1] = e.pos[2] = 0;
                break;
            }
        ++ended;
    }
    const bool changed = now_set != g_gz_sent;
    if (!changed && (now_set.empty() || now_ms - g_gz_ms < 1000.0)) return;
    if (!m.n) {
        g_gz_sent.clear();
        return;
    }
    m.sender_ms = static_cast<std::uint32_t>(link::NowMs());
    const std::uint32_t seq = g_s_seq;
    std::uint8_t b[R::kBtsMaxSize];
    if (!Send(b, R::EncodeGzst(seq, m, b, sizeof(b)))) return;
    ++g_s2.tx_gzst;
    g_gz_ms = now_ms;
    if (changed && g_s_budget.Take(now_ms)) {
        char near_text[96] = "";
        if (!now_set.empty()) {
            char bits[64];
            R::GzBitsText(m.e[0].bits, bits, sizeof(bits));
            std::snprintf(near_text, sizeof(near_text), "; nearest loc %08X (field %08X %s) bits%s", m.e[0].loc, m.e[0].field, R::FieldName(m.e[0].field),
                          bits);
        }
        Log("[bt] GZST sent: %u sensing, %u ended%s (seq %u)", static_cast<unsigned>(now_set.size()), ended, near_text, seq);
    }
    g_gz_sent.swap(now_set);
}

// ---------------------------------------------------------------- S1 receiver
void OnTrouble(double now_ms, const std::uint8_t* b, std::size_t n) {
    R::BtsTrouble m;
    if (!R::DecodeTrouble(b, n, m)) {
        ++g_s.bad;
        return;
    }
    ++g_s2.rx_trouble;
    const bool test = (m.flags & R::kTrfTest) != 0, gold = (m.flags & R::kTrfGold) != 0;
    const R::TroubleView::Out o = g_tv.Take(m, now_ms);
    const double p[3] = {m.pos[0], m.pos[1], m.pos[2]};
    switch (m.kind) {
        case R::kTrHunted:
        case R::kTrPulled:
        case R::kTrCaught: btmarks::Tar(p, gold, test, now_ms); break;
        case R::kTrCatcher: btmarks::Catcher(p, m.hp, now_ms); break;
        case R::kTrClear: btmarks::ClearTrouble(); break;
    }
    if (o.log) {
        char what[32];
        if (m.kind == R::kTrCatcher) std::snprintf(what, sizeof(what), "CATCHER hp %.0f%%", static_cast<double>(m.hp));
        else std::snprintf(what, sizeof(what), "%s", R::TroubleName(m.kind));
        char gz[48] = "";
        if (m.loc) std::snprintf(gz, sizeof(gz), ", gazer loc %08X (%s)", m.loc, g_s_locs.count(m.loc) ? "live here" : "not streamed here");
        Log("[bt] partner trouble: %s (episode %u%s%s%s) at (%.1f %.1f %.1f), you %.0f m%s; %s", what, m.episode, test ? ", test" : "", gold ? ", gold" : "",
            (m.flags & R::kTrfCarried) ? ", carried" : "", p[0], p[1], p[2], g_s_me_ok ? R::Dist3(p, g_s_me) : -1.0, gz,
            m.kind == R::kTrClear ? "markers gone" : btmarks::On() ? "marker kept 3 s" : "no marker (bt_markers=0)");
    }
    if (o.toast) {
        const char* t = m.kind == R::kTrCaught ? "Your partner was caught by a BT" : "Your partner is fighting a catcher";
        // review (ux 3): with the in-world markers on, they show it (the tar, the catcher's ring); no text on the screen
        if (btmarks::On()) {
            Log("[bt] partner trouble toast (once this episode %u): %s: not shown (the markers show it)", m.episode, t);
        } else {
            Log("[bt] partner trouble toast (once this episode %u): %s", m.episode, t);
            Toast(now_ms, t);
        }
    }
}

void OnGzst(double now_ms, const std::uint8_t* b, std::size_t n) {
    R::BtsGzst m;
    if (!R::DecodeGzst(b, n, m)) {
        ++g_s.bad;
        return;
    }
    ++g_s2.rx_gzst;
    if (g_gz_logged.size() > 64) g_gz_logged.clear();
    for (unsigned i = 0; i < m.n; ++i) {
        const R::GzEntry& e = m.e[i];
        const bool test = (e.bits & R::kGzTest) != 0;
        const auto it = g_s_locs.find(e.loc);
        const bool live = it != g_s_locs.end();
        double at[3] = {e.pos[0], e.pos[1], e.pos[2]};
        if (live && it->second.pos_ok) std::memcpy(at, it->second.pos, sizeof(at));  // the marker on this game's own copy
        if (R::GzInteresting(e.bits)) btmarks::Gazer(e.loc, at, test, now_ms);
        else btmarks::DropGazer(e.loc);
        const auto lg = g_gz_logged.find(e.loc);
        if (lg != g_gz_logged.end() && lg->second == e.bits) continue;
        g_gz_logged[e.loc] = e.bits;
        if (!test && !g_s_budget.Take(now_ms)) continue;
        char bits[64];
        R::GzBitsText(e.bits, bits, sizeof(bits));
        char here_text[96] = "not streamed here";
        if (live)
            std::snprintf(here_text, sizeof(here_text), "live (id %05X, you %.1f m from it)", it->second.id,
                          (it->second.pos_ok && g_s_me_ok) ? R::Dist3(it->second.pos, g_s_me) : -1.0);
        Log("[bt] partner gazer state: loc %08X (field %08X %s) bits%s, state %u mode %u approach %.2f at (%.1f %.1f %.1f); here: %s", e.loc, e.field,
            R::FieldName(e.field), bits, e.state, e.mode, static_cast<double>(e.approach), at[0], at[1], at[2], here_text);
    }
}

// ---------------------------------------------------------------- S2: this game's kills (bt_world's events, game thread)
void KillEvent(double now_ms, const btworld::Event& e) {
    if (e.applied != btworld::kNotArmed) {  // an armed kill's outcome
        if (e.tag == R::kTagMirror)
            for (PendKill& p : g_pk)
                if (p.armed && !p.done && p.actor == e.actor) p.done = true, p.done_ms = now_ms, p.result = e.applied;
        if (e.tag == R::kTagTest && g_tk.on && !g_tk.done && e.actor == g_tk.actor) g_tk.done = true, g_tk.done_ms = now_ms, g_tk.result = e.applied;
    }
    if (!e.edge || !g_sync || g_sync_dead.load()) return;
    std::uint32_t loc = 0, field = 0;
    for (const auto& kv : g_s_locs)
        if (kv.second.id == e.id) {
            loc = kv.first, field = kv.second.field;
            break;
        }
    if (!loc) {  // streamed in since the last census
        Census c;
        TakeCensus(now_ms, c);
        for (const Gazer& z : c.g)
            if (z.located && (z.actor == e.actor || z.id == e.id)) {
                loc = z.loc_hash, field = z.field;
                break;
            }
    }
    std::uint32_t pid = 0;
    const bool pid_ok = btworld::PlayerId(&pid);
    std::uint8_t cause = 0;
    const double since_reset = g_reset_ms > -1e17 ? now_ms - g_reset_ms : -1.0;
    const char* why = R::KillRelay(e.reason, e.killer, pid_ok, pid, e.tag, g_echo.Has(loc, now_ms), since_reset, &cause);
    if (!why && !loc) why = "its locator is unknown (not in the census)";
    if (!why && g_kill_level < 1) why = "bt_kill=0 here";
    if (!why && !g_s_peer) why = "no partner hello";
    if (!why && !(g_s_peer_caps & R::kCapKill)) why = "the partner's build takes no KILL";
    // review (logic 7): a partner that would refuse every kill (its bt_kill below 2, or the guest on its real save) gets none
    if (!why && !(g_s_peer_caps & R::kCapKillApply)) why = "the partner does not apply kills (its bt_kill, or its real save)";
    if (why) {
        ++g_s2.unsent_kill;
        if (e.tag != R::kTagNone || g_s_budget.Take(now_ms))
            Log("[bt] kill here: loc %08X (field %08X %s, id %05X, reason %08X, killer %05X, me %05X%s): not sent: %s", loc, field, R::FieldName(field), e.id,
                e.reason, e.killer, pid, pid_ok ? "" : " unknown", why);
        return;
    }
    R::BtsKill k;
    k.field = field, k.loc = loc, k.reason = e.reason, k.world = esync::Gate().guids, k.cause = cause;
    k.flags = static_cast<std::uint8_t>((automation::SavesRedirected() ? R::kKfSandboxed : 0) | (e.tag == R::kTagTest ? R::kKfTest : 0));
    for (int i = 0; i < 3; ++i) k.pos[i] = e.pos_ok ? static_cast<float>(e.pos[i]) : 0.0f;
    k.sender_ms = static_cast<std::uint32_t>(link::NowMs());
    const std::uint32_t seq = g_s_seq;
    std::uint8_t b[R::kBtsKillSize];
    if (!Send(b, R::EncodeKill(seq, k, b, sizeof(b)))) {
        ++g_s2.unsent_kill;
        Log("[bt] kill here: loc %08X: not sent: the link did not take it", loc);
        return;
    }
    ++g_s2.tx_kill;
    Log("[bt] KILL sent%s: loc %08X (field %08X %s, reason %08X, cause %s, world %08X%s) (seq %u)", e.tag == R::kTagTest ? " (test)" : "", loc, field,
        R::FieldName(field), e.reason, R::CauseName(cause), k.world, (k.flags & R::kKfSandboxed) ? ", sandboxed" : ", real save", seq);
}

void WorldEvents(double now_ms) {
    btworld::Event ev[32];
    unsigned lost = 0;
    const unsigned n = btworld::TakeEvents(ev, 32, &lost);
    if (lost) {
        g_s2.events_lost += lost;
        if (g_s_budget.Take(now_ms)) Log("[bt] world events: %u lost (the ring of 32 was full)", lost);
    }
    for (unsigned i = 0; i < n; ++i) {
        OnProbeEvent(ev[i]);
        KillEvent(now_ms, ev[i]);
    }
}

// ---------------------------------------------------------------- S2: the partner's kills
void OnKill(double now_ms, const std::uint8_t* b, std::size_t n) {
    R::BtsKill k;
    if (!R::DecodeKill(b, n, k)) {
        ++g_s.bad;
        return;
    }
    ++g_s2.rx_kill;
    g_echo.Add(k.loc, now_ms);  // this gazer's death edge here within 10 s is not sent back
    const std::uint32_t world = esync::Gate().guids;
    Log("[bt] KILL received: loc %08X (field %08X %s, reason %08X, cause %s%s%s), world %08X / here %08X, from the %s", k.loc, k.field,
        R::FieldName(k.field), k.reason, R::CauseName(k.cause), (k.flags & R::kKfTest) ? ", test" : "",
        (k.flags & R::kKfSandboxed) ? ", sandboxed" : ", real save", k.world, world, RoleText(g_s_peer_role));
    for (const PendKill& q : g_pk)
        if (q.k.loc == k.loc) {
            Log("[bt] partner kill loc %08X: not applied: one for it is pending already", k.loc);
            return;
        }
    if (g_pk.size() >= kPendKills) {
        ++g_s2.kill_refused;
        Log("[bt] partner kill loc %08X: not applied: %u pending already", k.loc, kPendKills);
        return;
    }
    PendKill p;
    p.k = k;
    p.rx_ms = now_ms;
    g_pk.push_back(p);
    g_pk_next_ms = 0;  // the next tick looks at it
}

// An armed partner kill: true when it is finished (logged).
bool ArmedStep(double now_ms, PendKill& p) {
    if (p.done) {
        if (now_ms - p.done_ms < 2000.0) return false;  // the kill list and the field record after the death processing
        if (p.result != R::kKillOk) {
            ++g_s2.kill_refused;
            Log("[bt] partner kill loc %08X: not applied: refused in the update: %s", p.k.loc, ApplyText(p.result));
            return true;
        }
        Census c;
        TakeCensus(now_ms, c);
        char edge[64];
        EdgeText(c, p.field_here, p.killed0, edge, sizeof(edge));
        ++g_s2.kill_applied;
        Log("[bt] partner kill applied loc %08X (id %05X, field %08X %s; from the %s, reason %08X, cause %s%s; %.0f m from you, +0x3A8 0x%llX): death edge "
            "%s, list %d -> %d",
            p.k.loc, p.id, p.field_here, R::FieldName(p.field_here), RoleText(g_s_peer_role), p.k.reason, R::CauseName(p.k.cause),
            (p.k.flags & R::kKfTest) ? ", test" : "", p.dist, static_cast<unsigned long long>(p.scene), edge, p.list0, c.list);
        return true;
    }
    if (now_ms - p.arm_ms > 10000.0 && btworld::Disarm(p.actor)) {
        ++g_s2.kill_refused;
        Log("[bt] partner kill loc %08X: not applied: the gazer did not update in 10 s", p.k.loc);
        return true;
    }
    // review (crash 4): the update took the kill but its outcome never came (the event ring was full): finished, not waited on
    if (now_ms - p.arm_ms > 12000.0 && !btworld::Armed(p.actor)) {
        ++g_s2.kill_refused;
        Log("[bt] partner kill loc %08X: not applied: its outcome was lost (the event ring was full)", p.k.loc);
        return true;
    }
    return false;
}

void ProcessKills(double now_ms) {
    if (g_pk.empty()) return;
    const char* not_playing = LoadingNow() ? "loading (no live player, or a load in progress)" : PlayingWhy();
    const bool carried = ride::Carried();
    const int allowed = not_playing ? -1 : btworld::BtAllowed();
    const std::uint32_t world = esync::Gate().guids;
    const bool records = g_s_have_here && g_s_have_there && R::CompareFields(g_s_here, g_s_there).records;
    std::vector<PendKill> keep;
    const bool loading = LoadingNow();
    for (PendKill& p : g_pk) {
        if (p.armed) {
            // review (crash 3): a load frees the gazers; an armed kill is taken back, never applied to what comes next there
            if (loading && !p.done && btworld::Disarm(p.actor)) {
                ++g_s2.kill_refused;
                Log("[bt] partner kill loc %08X: not applied: a load (disarmed)", p.k.loc);
                continue;
            }
            if (!ArmedStep(now_ms, p)) keep.push_back(p);
            continue;
        }
        R::KillApplyIn in;
        in.bt_kill = g_kill_level;
        in.role = g_role;
        in.sandboxed = automation::SavesRedirected();
        in.sender_sandboxed = (p.k.flags & R::kKfSandboxed) != 0;
        in.world_equal = world && p.k.world && world == p.k.world;
        in.records_equal = records;
        in.age_ms = now_ms - p.rx_ms;
        in.not_playing = not_playing;
        in.carried = carried;
        in.bt_allowed = allowed;
        std::uintptr_t actor = 0, scene = 0;
        std::uint32_t id = 0, field_here = 0;
        double dist = -1;
        const auto it = g_s_locs.find(p.k.loc);
        if (it != g_s_locs.end()) {
            id = it->second.id, field_here = it->second.field;
            actor = Resolve(id);
            if (it->second.pos_ok && g_s_me_ok) dist = R::Dist3(it->second.pos, g_s_me);
        }
        in.streamed = actor != 0;
        // round 30: +0x3A8 is the scene of the gazer's own locator (FUN_142994B40), set on every placed gazer. It is read for the
        // log lines only; it never refuses a kill (ArmKill's actor id check covers a re-used actor).
        std::uintptr_t loc_scene = 0;
        if (actor) {
            if (!btworld::SceneOwner(actor, &scene)) scene = 0;
            in.scene_owner = scene != 0;
            const std::uintptr_t loc = mem::Q(actor + kActLoc);
            loc_scene = loc ? mem::Q(loc + kLocScene) : 0;
            std::int32_t state = -1;
            float h = 0, prev = 0;
            std::uint32_t flags = 0;
            if (mem::Rd(actor + kActState, state) && mem::Rd(actor + kGzHealth, h) && mem::Rd(actor + kGzPrevHealth, prev) && mem::Rd(actor + kActFlags, flags))
                in.guard = R::KillGuard(state, h, prev, flags);
            else
                in.streamed = false;  // unreadable now: held as not streamed
        }
        in.dist_m = dist;
        R::KillVerdict v = R::KillApplyGate(in);
        if (v.call == R::KillCall::apply && !KillApplyHere()) v = {R::KillCall::refuse, "the kill byte checks failed here"};
        if (v.call == R::KillCall::apply) {
            InstallKillHook("the kill relay");
            Census c;
            TakeCensus(now_ms, c);
            const Field* f = FindField(c, field_here);
            p.list0 = c.list, p.killed0 = f ? f->killed : -1;
            if (btworld::ArmKill(actor, id, R::kTagMirror)) {
                p.armed = true, p.actor = actor, p.id = id, p.arm_ms = now_ms, p.scene = scene, p.dist = dist, p.field_here = field_here;
                Log("[bt] partner kill loc %08X: armed (id %05X, field %08X %s; %.0f m from you, +0x3A8 0x%llX %s, after %.1f s)", p.k.loc, id, field_here,
                    R::FieldName(field_here), dist, static_cast<unsigned long long>(scene),
                    !scene ? "(unread or 0)" : scene == loc_scene ? "(the locator's scene)" : "(NOT the locator's scene: logged only)", in.age_ms / 1000.0);
                keep.push_back(p);
                continue;
            }
            v = {R::KillCall::hold, "the kill slots are busy"};
        }
        if (v.call == R::KillCall::hold) {
            if (p.last_why != v.why) {
                p.last_why = v.why;
                Log("[bt] partner kill loc %08X: held: %s", p.k.loc, v.why);
            }
            keep.push_back(p);
            continue;
        }
        ++g_s2.kill_refused;
        Log("[bt] partner kill loc %08X: not applied: %s", p.k.loc, v.why);
    }
    g_pk.swap(keep);
}

// ---------------------------------------------------------------- the tests (hidden runs) and the tick
void TestTroubleTick(double now_ms) {
    if (!g_tt.on) return;
    const int k = R::TestStepAt(now_ms - g_tt.t0);
    while (g_tt.step < k) {
        const R::TestStep& st = R::kTestSteps[++g_tt.step];
        TroubleSend(st.kind, R::kTrfTest, g_tt.episode, g_tt.pos_ok ? g_tt.pos : nullptr, st.hp, 0, 0, "a test step at my position");
    }
    if (g_tt.step >= static_cast<int>(R::kTestStepCount) - 1) {
        g_tt.on = false;
        g_tr_logged = 0;
    }
}

void TestKillTick(double now_ms) {
    if (!g_tk.on) return;
    if (g_tk.done) {
        if (now_ms - g_tk.done_ms < 2000.0) return;
        g_tk.on = false;
        if (g_tk.result != R::kKillOk) {
            Log("[bt] test kill loc %08X: refused in the update: %s", g_tk.loc, ApplyText(g_tk.result));
            return;
        }
        Census c;
        TakeCensus(now_ms, c);
        char edge[64];
        EdgeText(c, g_tk.field, g_tk.killed0, edge, sizeof(edge));
        Log("[bt] test kill loc %08X: applied here, death edge %s, list %d -> %d", g_tk.loc, edge, g_tk.list0, c.list);
    } else if (now_ms - g_tk.arm_ms > 10000.0 && btworld::Disarm(g_tk.actor)) {
        g_tk.on = false;
        Log("[bt] test kill loc %08X: not applied (the gazer did not update in 10 s)", g_tk.loc);
    } else if (LoadingNow() && btworld::Disarm(g_tk.actor)) {
        g_tk.on = false;
        Log("[bt] test kill loc %08X: not applied (a load: disarmed)", g_tk.loc);
    } else if (now_ms - g_tk.arm_ms > 12000.0 && !btworld::Armed(g_tk.actor)) {
        g_tk.on = false;
        Log("[bt] test kill loc %08X: not applied (its outcome was lost: the event ring was full)", g_tk.loc);
    }
}

void S12Tick(double now_ms) {
    if (g_tv.Expire(now_ms)) {
        btmarks::ClearTrouble();
        Log("[bt] partner trouble: no TROUBLE for 3 s (no CLEAR came): its markers are gone");
    }
    TestTroubleTick(now_ms);
    if ((g_pk.empty() && !g_tk.on) || now_ms < g_pk_next_ms) return;
    g_pk_next_ms = now_ms + 250.0;
    ProcessKills(now_ms);
    TestKillTick(now_ms);
}

void S12Session() {
    const std::uint16_t ep = g_tr.episode;
    g_tr = R::TroubleTracker{};
    g_tr.episode = ep;  // episodes keep counting (the partner tells them apart)
    g_tr_logged = 0;
    g_tt.on = false;
    g_tv = R::TroubleView{};
    g_gz_sent.clear();
    g_gz_ms = -1e18;
    g_gz_logged.clear();
    g_echo = R::EchoRing{};
    if (!g_pk.empty()) {
        for (const PendKill& p : g_pk)
            if (p.armed && !p.done) btworld::Disarm(p.actor);
        Log("[bt] partner kills: %u pending dropped (a new link session)", static_cast<unsigned>(g_pk.size()));
    }
    g_pk.clear();
    btmarks::ClearTrouble();
}

// The nearest live gazer (loc 0) or the one with that locator.
const Gazer* PickGazer(const Census& c, std::uint32_t loc, const double* me, bool killable = false) {
    const Gazer* z = nullptr;
    double best = -1;
    for (const Gazer& g : c.g) {
        if (!g.actor) continue;
        if (loc) {
            if (g.located && g.loc_hash == loc) z = &g;
            continue;
        }
        if (g.state != 0 || !g.pos_ok || !g.located) continue;
        // review (logic 6): for the test kill, the nearest one the kill can take (KillGuard: dying, or flags 0x9, is refused in the update)
        if (killable) {
            float h = 0, prev = 0;
            if (!mem::Rd(g.actor + kGzHealth, h) || !mem::Rd(g.actor + kGzPrevHealth, prev) || R::KillGuard(g.state, h, prev, g.flags) != R::kKillOk) continue;
        }
        const double d = R::Dist3(g.pos, me);
        if (!z || d < best) z = &g, best = d;
    }
    return z;
}

// bt_sync 5: the test TROUBLE sequence (HUNTED, CATCHER 100 / 60 / 20 %, CLEAR over 6 s), each flagged a test.
bool OpTestTrouble(char* msg, std::size_t cap) {
    if (!automation::Enabled()) return Put(msg, cap, "bt sync: the test messages are for hidden runs only"), false;
    if (!g_trouble_on) return Put(msg, cap, "bt sync: bt_trouble=0 here"), false;
    if (!g_s_peer) return Put(msg, cap, "bt sync: no partner hello yet"), false;
    if (!(g_s_peer_caps & R::kCapTrouble)) return Put(msg, cap, "bt sync: the partner's build takes no TROUBLE"), false;
    if (g_tt.on) return Put(msg, cap, "bt sync: a test sequence runs already"), false;
    if (g_tr.kind) return Put(msg, cap, "bt sync: this game's player is in trouble now (%s)", R::TroubleName(g_tr.kind)), false;
    g_tt = TroubleTest{};
    g_tt.on = true;
    g_tt.t0 = g_now_ms;
    if (++g_tr.episode == 0) g_tr.episode = 1;
    g_tt.episode = g_tr.episode;
    g_tt.pos_ok = LocalPos(g_tt.pos);
    Log("[bt] test TROUBLE sequence: episode %u (HUNTED, CATCHER 100/60/20 %%, CLEAR over 6 s)", g_tt.episode);
    return Put(msg, cap, "bt sync: test TROUBLE sequence, episode %u (log)", g_tt.episode), true;
}

// bt_sync 6: a test GZST of the nearest live gazer here.
bool OpTestGzst(char* msg, std::size_t cap) {
    if (!automation::Enabled()) return Put(msg, cap, "bt sync: the test messages are for hidden runs only"), false;
    if (!g_trouble_on) return Put(msg, cap, "bt sync: bt_trouble=0 here"), false;
    if (!g_s_peer) return Put(msg, cap, "bt sync: no partner hello yet"), false;
    if (!(g_s_peer_caps & R::kCapGzst)) return Put(msg, cap, "bt sync: the partner's build takes no GZST"), false;
    double me[3];
    if (!LocalPos(me)) return Put(msg, cap, "bt sync: no local player (not in a world)"), false;
    Census c;
    TakeCensus(g_now_ms, c);
    const Gazer* z = PickGazer(c, 0, me);
    if (!z) return Put(msg, cap, "bt sync: no live located gazer here"), false;
    R::BtsGzst m;
    m.n = 1;
    FillGz(m.e[0], *z, static_cast<std::uint8_t>((R::GzBits(z->flags, z->state) & ~R::kGzDead) | R::kGzTest));
    m.sender_ms = static_cast<std::uint32_t>(link::NowMs());
    const std::uint32_t seq = g_s_seq;
    std::uint8_t b[R::kBtsMaxSize];
    if (!Send(b, R::EncodeGzst(seq, m, b, sizeof(b)))) return Put(msg, cap, "bt sync: the link did not take the test GZST"), false;
    ++g_s2.tx_gzst;
    char bits[64];
    R::GzBitsText(m.e[0].bits, bits, sizeof(bits));
    Log("[bt] GZST sent (test): loc %08X (field %08X %s) bits%s, %.1f m from me (seq %u)", z->loc_hash, z->field, R::FieldName(z->field), bits,
        R::Dist3(z->pos, me), seq);
    return Put(msg, cap, "bt sync: a test GZST for loc %08X (log)", z->loc_hash), true;
}

// bt_sync 7 <hi16> <lo16>: a real kill of one gazer here (0 0 = the nearest live one), as bt_probe 10 but tagged a test: its death
// edge goes to the partner as KILL (cause "a test"), where the receiver's gate decides as for any kill.
bool OpTestKill(const double (&v)[3], char* msg, std::size_t cap) {
    if (!automation::Enabled()) return Put(msg, cap, "bt sync: the test kill is for hidden runs only"), false;
    if (!automation::SavesRedirected()) return Put(msg, cap, "bt sync: the test kill only on a sandboxed save"), false;
    if (g_kill_level < 1) return Put(msg, cap, "bt sync: bt_kill=0 here"), false;
    std::uint32_t loc = 0;
    if (!R::LocArg(v[1], v[2], &loc)) return Put(msg, cap, "bt sync: 7 <locator hi16> <lo16> (0 0 = the nearest live gazer)"), false;
    if (!(Check() & kActor) || !KillSendable()) return Put(msg, cap, "bt sync: the kill is off (byte checks; log)"), false;
    if (g_tk.on) return Put(msg, cap, "bt sync: a test kill is armed already"), false;
    double me[3];
    if (!LocalPos(me)) return Put(msg, cap, "bt sync: no local player (not in a world)"), false;
    Census c;
    TakeCensus(g_now_ms, c);
    const Gazer* z = PickGazer(c, loc, me, true);
    if (!z) return Put(msg, cap, "bt sync: no live gazer %s", loc ? "with that locator" : "here"), false;
    float h = 0, prev = 0;
    std::uint32_t flags = 0;
    std::int32_t state = -1;
    if (!mem::Rd(z->actor + kActState, state) || !mem::Rd(z->actor + kGzHealth, h) || !mem::Rd(z->actor + kGzPrevHealth, prev) ||
        !mem::Rd(z->actor + kActFlags, flags))
        return Put(msg, cap, "bt sync: the gazer is unreadable"), false;
    const int g = R::KillGuard(state, h, prev, flags);
    if (g != R::kKillOk) {
        Log("[bt] test kill loc %08X: refused: %s (state %d, health %.2f, last %.2f, flags 0x%X)", z->loc_hash, R::KillGuardText(g), state, h, prev, flags);
        return Put(msg, cap, "bt sync: test kill refused: %s", R::KillGuardText(g)), false;
    }
    if (!btworld::HookIn() && !btworld::InstallHook("the kill test", msg, cap)) return false;
    const Field* f = FindField(c, z->field);
    g_tk = TestKill{};
    g_tk.on = true, g_tk.loc = z->loc_hash, g_tk.field = z->field, g_tk.id = z->id, g_tk.actor = z->actor, g_tk.arm_ms = g_now_ms;
    g_tk.list0 = c.list, g_tk.killed0 = f ? f->killed : -1;
    if (!btworld::ArmKill(z->actor, z->id, R::kTagTest)) {
        g_tk.on = false;
        return Put(msg, cap, "bt sync: not armed (the kill slots are full or that gazer is armed already)"), false;
    }
    Log("[bt] test kill loc %08X (id %05X, field %08X %s): armed, %.1f m from me (the update's own kill at its next update; its death edge goes as "
        "KILL)",
        z->loc_hash, z->id, z->field, R::FieldName(z->field), z->pos_ok ? R::Dist3(z->pos, me) : -1.0);
    return Put(msg, cap, "bt sync: test kill armed for loc %08X (log)", z->loc_hash), true;
}

}  // namespace

void Install(const Config& config) noexcept {
    // B1: bt_sync=1|2 (config.cpp clamps it; the integration adds the key), only in a two-game session
    g_sync = R::SyncLevel(static_cast<double>(config.bt_sync));
    if (g_sync) {
        g_role = config.role == Role::host ? 1 : config.role == Role::guest ? 2 : 0;
        if (!g_role) {
            Log("[bt] sync off: needs a two-game session (role %s; bt_sync=%d ignored)", RoleName(config.role), g_sync);
            g_sync = 0;
        } else {
            Log("[bt] sync: bt_sync=%d as the %s: FIELD compare, NOTICED, VOID notices%s; the relay waits for the partner's hello", g_sync,
                RoleText(g_role),
                ResetApplyHere()               ? ", the host's RESET applied here"
                : g_sync >= 2 && g_role == 2 ? ", the host's RESET not applied here (this game plays on its own real save)"
                                               : "");
            InstallVoidHook();
            // round 30 S1 / S2
            g_trouble_on = config.bt_trouble;
            g_kill_level = std::min(R::SyncLevel(static_cast<double>(config.bt_kill)), g_sync);
            btmarks::Init(config);
            Log("[bt] sync: TROUBLE / GZST %s; KILL %s%s", g_trouble_on ? "on (the partner's trouble as markers here)" : "off (bt_trouble=0)",
                g_kill_level >= 2 ? "on (partner kills applied here)" : g_kill_level == 1 ? "relayed, not applied (bt_kill=1 or bt_sync=1)" : "off (bt_kill=0)",
                g_kill_level >= 2 && g_role == 2 && !automation::SavesRedirected() ? "; but never on this real save (host_save=0)" : "");
        }
    }
    g_ini = R::ProbeMode(static_cast<double>(config.bt_probe));
    if (g_ini == 0) return;
    g_on = true;
    g_autodump = g_ini == 2;
    g_interval_ms = 1000.0;
    Log("[bt] bt_probe=%d: the census once a second from the first tick in a world%s (read only)", g_ini,
        g_autodump ? ", one dump when gazers first appear" : "");
}

void Tick(double now_ms) noexcept {
    g_now_ms = now_ms;
    if (g_sync) SyncTick(now_ms);
    if (btworld::HookIn()) WorldEvents(now_ms);  // round 30 S2: the gazer update's events (the kill relay and the probe's watch)
    if (btworld::HookIn() || g_kw.on || g_cw.on || g_hw.on) ProbeTick(now_ms);  // round 30: the world probes (hidden runs only)
    if (!g_on || now_ms < g_next_ms) return;
    g_next_ms = now_ms + g_interval_ms;
    double me[3];
    if (!LocalPos(me)) return;  // the title, a load: nothing to count
    if (!(Check() & kMgr)) {
        if (!g_refused_logged) Log("[bt] probe off: %s", g_off[0] ? g_off : "byte checks failed");
        g_refused_logged = true;
        g_on = false;
        return;
    }
    Census c;
    TakeCensus(now_ms, c);
    LogChanges(now_ms, c);
    LogCensus(c);
    if (g_autodump && !c.g.empty()) {
        g_autodump = false;
        Dump(now_ms, c);
    }
    Remember(c);
}

bool Command(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (const char* why = Refusal()) return Put(msg, cap, "bt probe: %s", why), false;
    const int op = R::ProbeOp(v[0]);
    if (op >= R::kOpThreads) return ProbeOps(op, v, msg, cap);  // round 30: the world probes (hidden runs only)
    if (op == 0) {
        if (!g_on) return Put(msg, cap, "bt probe: already off (%d censuses)", g_n), true;
        g_on = false;
        StopLine("action");
        return Put(msg, cap, "bt probe stopped after %d censuses (perceived %s)", g_n, g_perceived ? "yes" : "no"), true;
    }
    if (!(Check() & kMgr)) return Put(msg, cap, "bt probe: off: %s", g_off[0] ? g_off : "byte checks failed"), false;
    if (op == 1) {
        g_interval_ms = R::ProbeIntervalMs(v[1]);
        if (!g_on) g_n = 0, g_have_prev = false, g_near_events = 0, g_max_near = 0, g_perceived = false, g_noticed = 0, g_conc_max = 0;
        g_on = true;
        g_next_ms = 0;
        Log("[bt] probe on: every %.2f s (read only)", g_interval_ms / 1000.0);
        return Put(msg, cap, "bt probe on: a census every %.2f s", g_interval_ms / 1000.0), true;
    }
    double me[3];
    if (!LocalPos(me)) return Put(msg, cap, "bt probe dump: no local player (not in a world)"), false;
    Census c;
    const double now_ms = g_now_ms;
    TakeCensus(now_ms, c);
    Dump(now_ms, c);
    return Put(msg, cap, "bt probe dump: %d gazers (%d located), %d fields, %d catchers, hunter %s (log)", static_cast<int>(c.g.size()),
               c.located, static_cast<int>(c.f.size()), static_cast<int>(c.c.size()), c.hunter_area ? "ACTIVE" : "none"),
           true;
}

bool SyncCommand(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!g_sync) return Put(msg, cap, "bt sync: off (bt_sync=0 in session.ini, or not a two-game session)"), false;
    const int op = R::SyncOp(v[0]);
    if (op == 0) {
        SyncStatus("action");
        return Put(msg, cap, "bt sync: partner %s; sent %u field / %u noticed / %u void / %u reset; got %u / %u / %u / %u (applied %u) (log)",
                   g_s_peer ? "on" : "none", g_s.tx_field, g_s.tx_noticed, g_s.tx_void, g_s.tx_reset, g_s.rx_field, g_s.rx_noticed, g_s.rx_void,
                   g_s.rx_reset, g_s.applied),
               true;
    }
    if (g_sync_dead.load()) return Put(msg, cap, "bt sync: stopped (emergency stop)"), false;
    if (op == 1) {
        g_s_field_ms = -1e18;  // the next census sends FIELD
        g_s_next_ms = 0;
        g_s_cmp_ms = -1e18;
        return Put(msg, cap, "bt sync: FIELD at the next census%s", g_s_peer ? "" : " (no partner hello yet: nothing is sent)"), true;
    }
    if (op == 3 || op == 4) {
        if (!automation::Enabled()) return Put(msg, cap, "bt sync: the test messages are for hidden runs only"), false;
        if (!g_s_peer) return Put(msg, cap, "bt sync: no partner hello yet"), false;
        double me[3];
        const bool me_ok = LocalPos(me);
        if (op == 3) {
            VoidRec r;
            if (me_ok) std::memcpy(r.pos, me, sizeof(r.pos)), r.pos_ok = true;
            SendVoid(g_now_ms, r, true);
            return Put(msg, cap, "bt sync: a test VOID (log)"), true;
        }
        R::BtsNoticed n;
        n.kind = R::kNoticeTest;
        for (int i = 0; i < 3; ++i) n.gazer[i] = n.player[i] = me_ok ? static_cast<float>(me[i]) : 0.0f;
        n.game_dist = 0;
        if (!SendNoticed(n, "test", -1.0)) return Put(msg, cap, "bt sync: the link did not take the test NOTICED"), false;
        return Put(msg, cap, "bt sync: a test NOTICED (log)"), true;
    }
    if (op == 5) return OpTestTrouble(msg, cap);
    if (op == 6) return OpTestGzst(msg, cap);
    if (op == 7) return OpTestKill(v, msg, cap);
    return Put(msg, cap, "bt sync: v[0] 0 status, 1 FIELD now, 3 test VOID, 4 test NOTICED, 5 test TROUBLE, 6 test GZST, 7 <hi16> <lo16> a kill here"),
           false;
}

int HunterAreaUp() noexcept {
    if (!(Check() & kHunter)) return -1;
    const std::uintptr_t hm = mem::Q(A(kHunterGlobal));
    std::uintptr_t area = 0;
    if (!hm || !mem::Rd(hm + kHunArea, area)) return -1;
    return area ? 1 : 0;
}

void EmergencyStop() noexcept {
    g_void_pass.store(true);
    g_sync_dead.store(true);
    btworld::EmergencyStop();    // round 30: the gazer update hook straight through, armed kills dropped
    g_crater_dead = true;        // no more guarded craters (the flow gate passes everything while t_crater is clear)
}

}  // namespace st::bt
