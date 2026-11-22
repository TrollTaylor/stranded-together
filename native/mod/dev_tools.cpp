// Dev tools, game side. See dev_tools.hpp and docs/debug-actions.md "Dev tools" for the evidence behind every address.
#include "dev_tools.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "automation.hpp"
#include "config.hpp"
#include "game_actions.hpp"
#include "game_api.hpp"
#include "log.hpp"
#include "overlay/overlay_menu.hpp"
#include "partner_source.hpp"
#include "quickstart.hpp"
#include "story_state.hpp"
#include "vehicle_share.hpp"

namespace st::dev {
namespace {
namespace mem = partner::mem;
using devrules::Tool;

constexpr std::uintptr_t kImage = 0x140000000ull;  // ds.exe VAs at its preferred base
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

// ---------------------------------------------------------------- game addresses (ds.exe DC, PE timestamp 1706443442)
// Player
constexpr std::uintptr_t kRecoverFull = 0x142738940;      // DSPlayerSystem RecoverFull body (Sam, flags): the export 0x1426E6F50 passes flags 0
constexpr std::uintptr_t kRecoverShoes = 0x142744B40;     // RecoverOrSupplyShoes body (status [Sam+0x54E0], 0, 1)
constexpr std::uintptr_t kAddChiral = 0x1426E6530;        // DSPlayerSystem_sExportedAddChiralCrystalCount(int)
constexpr std::uintptr_t kPlayerSystemGlobal = 0x147BE7980;
constexpr std::uintptr_t kSamStatus = 0x54E0, kSamEquip = 0x54C8, kSamParam = 0x54C0;
// DebugSettings (retail leftover): +0x24 EGodMode (0 off, 1 on, 2 invulnerable), +0x28 infinite ammo (byte)
constexpr std::uintptr_t kDebugSettingsGlobal = 0x147BE6A50;
constexpr std::uintptr_t kDebugSettingsRtti = 0x144F10E90;
constexpr std::uintptr_t kDsGod = 0x24, kDsAmmo = 0x28;
// Weather (DS forecast), the current area
constexpr std::uintptr_t kForecastDirectEx = 0x142E1FE20;  // (u16 area, u8 region, u8 type): only for the current area
constexpr std::uintptr_t kWeatherGlobal = 0x147BF83B8;
constexpr std::uintptr_t kForecastReset = 0x142E1FC80;    // ResetForecastTime(u8): the forecast timers restart (the sky follows the new type)
constexpr std::uintptr_t kCurArea = 0x14262A860;
// Enemies
constexpr std::uintptr_t kMuleResurrect = 0x142B64420;     // ResetAndResurrectAllMulesByMissionChange (thunk: [mgr], -1)
constexpr std::uintptr_t kMuleRemoveAll = 0x142B645F0;     // RemoveAllMulesFromWorld(u8) (thunk: [mgr], flag)
constexpr std::uintptr_t kMuleGlobal = 0x147BF09D8;
constexpr std::uintptr_t kGazerReset = 0x1429A7220;        // ResetAllGazers (thunk: [mgr])
constexpr std::uintptr_t kGazerGlobal = 0x147BEBEC0;
// Places: DSMissionSystem +0x78820 Array<Ref<DSDeliveryPointInfoResource>> (u32 count, u32 capacity, T* data at +8)
constexpr std::uintptr_t kMissionSystemGlobal = 0x147C0AFF8;
constexpr std::uintptr_t kDpArray = 0x78820;
constexpr std::uintptr_t kDpRtti = 0x144DD5E50;
constexpr std::uintptr_t kDpText = 0x20, kDpArea = 0x68, kDpType = 0x6A, kDpOrder = 0x78, kDpXf = 0x80;
constexpr std::uintptr_t kPointTypeEnum = 0x144C33550;  // EDSConstructionPointType: +8 u16 count, +0x10 name, +0x18 values (0x28 each)
// Boots' life (dev_boots; capstone over ds.exe, round 4): the export DSPlayerSystem_sExportedSetShoesLifeByRate 0x1426E7460(float)
// takes [player +0x54E0] (Sam's status) and jumps to 0x142741860(status, rate, 0): when [status +0x368] is set (worn boots) it
// writes (status +0x334 + status +0x330) x rate through 0x142741720(status +0x320, durability): the first +0x334 points at +0x1C
// of that block (status +0x33C), the rest at +0xC (status +0x32C, at most +0x330). SetShoesForcibly 0x1426E7430 names the boots' type
// byte status +0x3B8. We call the body with the status we read and checked ourselves (the export does not null-check it).
constexpr std::uintptr_t kShoesByRate = 0x142741860;
constexpr std::uintptr_t kShoes = 0x320, kShoesLife = 0x33C, kShoesMax = 0x334, kShoesSpare = 0x330, kShoesOver = 0x32C, kShoesFlags = 0x3B0,
                         kShoesItem = 0x368, kShoesType = 0x3B8, kShoesWornType = 0x328;
constexpr std::uintptr_t kSetShoesForcibly = 0x1426E7430;

enum Feature : unsigned {
    kFRefill = 1u, kFGod = 2u, kFWeather = 4u, kFMules = 8u, kFBts = 16u, kFPlaces = 32u, kFBoots = 64u, kFChiral = 128u, kFBootsLife = 256u,
};
constexpr unsigned kFLast = kFBootsLife;
const char* FeatureName(unsigned f) {
    switch (f) {
        case kFRefill: return "refill";
        case kFGod: return "god mode / ammo";
        case kFWeather: return "weather";
        case kFMules: return "MULEs";
        case kFBts: return "BTs";
        case kFPlaces: return "places";
        case kFBoots: return "boots";
        case kFChiral: return "chiral crystals";
        case kFBootsLife: return "boots' life";
        default: return "?";
    }
}
struct Site {
    std::uintptr_t va;
    const char* sig;
    const char* name;
    unsigned features;
};
// clang-format off
constexpr Site kSites[] = {
    // RecoverFull body: status +0x54E0, equipment +0x54C8, params +0x54C0 (none of them null-checked: we check first)
    {kRecoverFull, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 40 48 8B 99 E0 54 00 00 45 33 C9 48 8B A9 C8 54 00 00 45 33 C0 48 8B B9 C0 54 00 00",
     "RecoverFull body", kFRefill},
    {0x1426E6F50, "40 53 48 83 EC 20 0F B6 D9 E8 D2 7C 15 00 8B D3 48 8B C8 03 D2 48 83 C4 20 5B E9 D1 19 05 00", "RecoverFull export -> body (flags 0)", kFRefill},
    {0x1427E1010, "8B 81 14 48 00 00 89 81 10 48 00 00 C7 81 F8 47 00 00 00 00 00 00 C3", "battery refill body", kFRefill},
    // DebugSettings global, +0x24 >= 1 (god), == 2 (invulnerable), +0x28 infinite ammo; the setters
    {0x1434F6EFE, "48 8B 05 4B FB 6E 04 83 78 24 01", "god mode reader (>= 1)", kFGod},
    {0x1434F6F43, "48 8B 05 06 FB 6E 04 83 78 24 02", "invulnerable reader (== 2)", kFGod},
    {0x1435BD53E, "48 8B 05 0B 95 62 04 80 78 28 00", "infinite ammo reader", kFGod},
    {0x1438282B0, "40 53 48 83 EC 20 49 8B 01 48 8B D9 33 D2 49 8B C9 FF 50 10 85 C0 79 0D C7 43 24 00 00 00 00", "DebugSettings::SetGodMode (+0x24)", kFGod},
    {0x143828300, "40 53 48 83 EC 20 49 8B 01 48 8B D9 33 D2 49 8B C9 FF 50 08 88 43 28", "DebugSettings::EnableInfiniteAmmo (+0x28)", kFGod},
    // SetForecastTypeOfRegionDirectEx: the weather global, type 14 = no change, the current-area test, the table row
    {kForecastDirectEx, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 1D 7D 85 DD 04 41 0F B6 F8 0F B6 EA 0F B7 F1 48 85 DB 74 5D 41 80 F8 0E 74 57",
     "SetForecastTypeOfRegionDirectEx", kFWeather},
    {0x142E1FE79, "66 3B F0 75 29 48 63 8B A0 64 00 00 48 C1 E1 06 48 83 C1 18 40 0F B6 C5 48 03 C8 40 0F B6 C7 48 6B D1 5C 89 04 1A", "forecast table row", kFWeather},
    {kForecastReset, "48 8B 05 31 87 DD 04 48 85 C0 74 41 C5 FA 10 80 A8 64 00 00", "ResetForecastTime", kFWeather},
    {kCurArea, "48 83 EC 28 48 8B 05 ?? ?? ?? ?? 48 85 C0 74 12 80 B8 A8 00 00 00 00 74 09 0F B7 40 48", "current area", kFWeather | kFPlaces},
    // MULE manager: the two exports' thunks (the global and the bodies)
    {kMuleResurrect, "48 8B 0D B1 C5 08 05 BA FF FF FF FF E9 BF 24 FB FF", "ResetAndResurrectAllMules thunk", kFMules},
    {0x142B168F0, "48 89 6C 24 18 48 89 7C 24 20 41 56 48 83 EC 40 48 8B F9 8B EA", "MULE resurrect body", kFMules},
    {0x142B169BA, "E8 B1 82 D2 FF", "MULE resurrect: the player object (0x14283EC70)", kFMules},
    {kMuleRemoveAll, "0F B6 D1 48 8B 0D DE C3 08 05 E9 C1 FA FA FF", "RemoveAllMulesFromWorld thunk", kFMules},
    {0x142B140C0, "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 50", "MULE remove-all body", kFMules},
    // Gazers (BTs)
    {kGazerReset, "48 8B 0D 99 4C 24 05 E9 74 A5 FF FF", "ResetAllGazers thunk", kFBts},
    {0x1429A17A0, "4C 8B DC 41 54 48 81 EC 80 00 00 00", "ResetAllGazers body", kFBts},
    // GetDeliveryPointInfoResources: DSMissionSystem global +0x78820
    {0x1433CA80A, "48 8B 15 E7 07 84 04 48 8B D9 33 FF 48 8D 4C 24 20 48 81 C2 20 88 07 00", "delivery point list (DSMissionSystem +0x78820)", kFPlaces},
    // RecoverOrSupplyShoes export: status +0x54E0, (0, 1) -> body
    {0x1426E6F80, "48 83 EC 28 E8 A7 7C 15 00 41 B0 01 33 D2 48 8B 88 E0 54 00 00 48 83 C4 28 E9 A2 DB 05 00", "RecoverOrSupplyShoes export", kFBoots},
    // AddChiralCrystalCount: DSPlayerSystem global (not null-checked: we check), local player null-checked
    {kAddChiral, "48 89 5C 24 10 57 48 83 EC 20 48 8B 1D 3F 14 50 05 8B F9 E8 E8 86 15 00 4C 8B C0 48 85 C0 74 2D 8B 93 7C 1B 00 00", "AddChiralCrystalCount", kFChiral},
    // Boots' life: the export (status +0x54E0, r8 0, jmp 0x142741860), the body (+0x368 test, the r8 test, +0x320 / +0x334 / +0x330,
    // the setter call), the setter (life +0x1C when worn, +0x8C when not), SetShoesForcibly (the type byte +0x3B8)
    {0x1426E7460, "48 83 EC 38 C5 F8 29 74 24 20 C5 F8 28 F0 E8 BD 77 15 00 45 33 C0 C5 F8 28 CE 48 8B 88 E0 54 00 00 C5 F8 28 74 24 20 48 83 C4 38 E9 D0 A3 05 00",
     "SetShoesLifeByRate export -> body", kFBootsLife},
    {kShoesByRate, "48 89 6C 24 18 56 48 83 EC 40 48 83 B9 68 03 00 00 00 48 8B F1 C5 F8 29 74 24 30 C5 F8 28 F1 75 6A 45 84 C0 0F 84 84 00 00 00",
     "SetShoesLifeByRate body", kFBootsLife},
    {0x1427418EB, "48 8D A9 20 03 00 00 C5 FA 10 86 34 03 00 00 C5 FA 58 8E 30 03 00 00 C5 F2 59 CE 48 8B CD E8 12 FE FF FF",
     "SetShoesLifeByRate: (max + spare) x rate", kFBootsLife},
    {0x142741720, "40 53 48 83 EC 30 0F B6 81 90 00 00 00 48 8B D9 A8 01 75 0E C5 FA 11 89 8C 00 00 00 48 83 C4 30 5B C3 C5 FA 10 41 14 C5 F8 2F C1",
     "shoes life setter", kFBootsLife},
    {0x142741757, "C5 FA 11 49 1C C5 E8 57 D2 EB 16 C5 FA 11 41 1C C5 F2 5C C0 C5 FA 5D 51 10", "shoes life setter: life +0x1C, spare +0xC", kFBootsLife},
    {0x1426E7430, "40 53 48 83 EC 20 0F B6 D9 E8 F2 77 15 00 48 85 C0 74 0D 48 8B 80 E0 54 00 00 88 98 B8 03 00 00", "SetShoesForcibly (type +0x3B8)",
     kFBootsLife},
};
// clang-format on

// ---------------------------------------------------------------- state
bool g_enabled = false;
int g_role = 0;  // 0 solo / echo, 1 host, 2 guest
bool g_host_save = false;
bool g_checked = false;
unsigned g_features = 0;
char g_off[160] = {};
bool g_test_real = false;
int g_god_set = -1;  // what we wrote (restore on off)
SRWLOCK g_view_lock = SRWLOCK_INIT;
View g_view;
double g_next_view = 0;
double g_now_ms = 0;  // the newest tick's time (game thread)
char g_enum_names[64][28] = {};
int g_enum_count = 0;

void Put(char* out, std::size_t cap, const char* fmt, ...) {
    if (!out || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(out, cap, fmt, ap);
    va_end(ap);
}

bool WriteBytes(std::uintptr_t a, const void* in, std::size_t n) {
    SIZE_T put = 0;
    return a >= 0x10000 && WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), in, n, &put) && put == n;
}

bool RttiNamed(std::uintptr_t rtti, const char* name) {
    const std::uintptr_t p = mem::Q(rtti + 0x40);
    char buf[64] = {};
    const std::size_t n = std::strlen(name) + 1;
    return p && n <= sizeof(buf) && mem::Read(p, buf, n) && std::memcmp(buf, name, n) == 0;
}

void Check() {
    if (g_checked) return;
    g_checked = true;
    unsigned features = kFRefill | kFGod | kFWeather | kFMules | kFBts | kFPlaces | kFBoots | kFChiral | kFBootsLife;
    int ok = 0, total = 0;
    auto fail = [&](unsigned f, const char* why) {
        if (!g_off[0]) {
            for (unsigned b = 1; b <= kFLast; b <<= 1)
                if (f & b) {
                    Put(g_off, sizeof(g_off), "%s: %s", FeatureName(b), why);
                    break;
                }
        }
        features &= ~f;
    };
    for (const Site& s : kSites) {
        ++total;
        if (mem::CodeMatches(A(s.va), s.sig)) {
            ++ok;
            continue;
        }
        Log("[dev] byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
        fail(s.features, s.name);
    }
    // the RIP targets the code above reads through
    auto rip = [&](std::uintptr_t insn, std::uintptr_t want, unsigned f, const char* what) {
        ++total;
        if (mem::RipTarget(A(insn), 3, 7) == A(want)) ++ok;
        else {
            Log("[dev] byte check FAILED: %s", what);
            fail(f, what);
        }
    };
    rip(0x1434F6EFE, kDebugSettingsGlobal, kFGod, "the DebugSettings global");
    rip(0x142E1FE34, kWeatherGlobal, kFWeather, "the weather global");
    rip(kMuleResurrect, kMuleGlobal, kFMules, "the MULE manager global");
    rip(kGazerReset, kGazerGlobal, kFBts, "the gazer manager global");
    rip(0x1433CA80A, kMissionSystemGlobal, kFPlaces, "the DSMissionSystem global");
    rip(kAddChiral + 10, kPlayerSystemGlobal, kFChiral, "the DSPlayerSystem global");
    ++total;
    if (RttiNamed(A(kDebugSettingsRtti), "DebugSettings")) ++ok;
    else fail(kFGod, "DebugSettings RTTI");
    ++total;
    if (RttiNamed(A(kDpRtti), "DSDeliveryPointInfoResource")) ++ok;
    else fail(kFPlaces, "DSDeliveryPointInfoResource RTTI");
    // the delivery point types' names (read from the game's RTTI enum, nothing shipped)
    ++total;
    char ename[32] = {};
    const std::uintptr_t np = mem::Q(A(kPointTypeEnum) + 0x10);
    std::uint16_t n = 0;
    if (np && mem::Read(np, ename, sizeof(ename) - 1) && std::strcmp(ename, "EDSConstructionPointType") == 0 && mem::Rd(A(kPointTypeEnum) + 8, n) &&
        n > 0 && n <= 64) {
        ++ok;
        const std::uintptr_t vals = mem::Q(A(kPointTypeEnum) + 0x18);
        for (std::uint16_t i = 0; i < n && g_enum_count < 64; ++i) {
            std::uint32_t value = 0;
            const std::uintptr_t sp = mem::Q(vals + 0x28u * i + 8);
            char s[28] = {};
            if (!mem::Rd(vals + 0x28u * i, value) || !sp || !mem::Read(sp, s, sizeof(s) - 1) || value >= 64) continue;
            std::memcpy(g_enum_names[value], s, sizeof(s));
            if (static_cast<int>(value) + 1 > g_enum_count) g_enum_count = static_cast<int>(value) + 1;
        }
    }  // without it the list shows the type numbers
    g_features = features;
    char on[200] = {};
    std::size_t k = 0;
    for (unsigned b = 1; b <= kFLast; b <<= 1)
        if (features & b) k += static_cast<std::size_t>(std::snprintf(on + k, sizeof(on) - k, "%s%s", k ? ", " : "", FeatureName(b)));
    Log("[dev] dev tools: %d/%d byte checks ok; on: %s%s%s", ok, total, k ? on : "none", g_off[0] ? "; OFF: " : "", g_off);
    // the vehicle tools' own byte checks (vehicle_share.cpp kDevSites, and the pools / spawn path when the shared vehicles
    // did not check them) on this first tick too, not on the first click: their "[dev] vehicles:" line now
    (void)vshare::DevCheckResult();
}

bool Has(unsigned f, char* msg, std::size_t cap) {
    Check();
    if (g_features & f) return true;
    Put(msg, cap, "%s is off: this game build differs (%s)", FeatureName(f), g_off[0] ? g_off : "a byte check");
    return false;
}

devrules::Session SessionNow() {
    devrules::Session s;
    s.enabled = g_enabled;
    s.saves_redirected = automation::SavesRedirected() && !g_test_real;
    s.role = g_role;
    s.host_save = g_host_save;
    return s;
}

}  // namespace

devrules::Verdict DecideNow(devrules::Tool t) noexcept { return devrules::Decide(t, SessionNow()); }

namespace {

// Every tool runs through this: the dev tools on, the real-save rule, a note for the result.
bool Gate(Tool t, char* msg, std::size_t cap, devrules::Verdict* out = nullptr) {
    const devrules::Verdict v = devrules::Decide(t, SessionNow());
    if (out) *out = v;
    if (v.ok()) return true;
    Put(msg, cap, "%s", v.why);
    Log("[dev] refused: %s", v.why);
    return false;
}

struct Sam {
    std::uintptr_t e = 0;
    game::WorldTransform xf{};
};
bool ReadSam(Sam* s) {
    game::Entity* e = nullptr;
    if (!game::TryGetLocalPlayer(&e) || !e) return false;
    s->e = reinterpret_cast<std::uintptr_t>(e);
    return mem::Read(s->e + game::entity_off::kPosition, &s->xf.pos, sizeof(s->xf.pos)) &&
           mem::Read(s->e + game::entity_off::kRotation, &s->xf.rot, sizeof(s->xf.rot));
}

// The world is being played: a local Sam, awake, nothing loading, no menu / cutscene / knot space.
const char* Playing(Sam* s) {
    if (!ReadSam(s)) return "no local player (title screen or loading)";
    std::uint64_t flags = 0;
    if (!mem::Rd(s->e + 0x88, flags) || (flags & 0x100)) return "Sam is asleep (loading, cutscene or menu)";
    std::uint64_t state = 0;
    std::uint32_t load = 0;
    if (!game::ReadStopGates(&state, &load)) return "the game state is unreadable";
    if (load) return "the game is loading";
    if (state & game::kStoppingGameEventMask) return "a menu, cutscene or scripted scene is running";
    return nullptr;
}

// ---------------------------------------------------------------- guarded calls
bool CallRecoverFull(std::uintptr_t sam) noexcept {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, int)>(A(kRecoverFull))(sam, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallShoes(std::uintptr_t status) noexcept {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, int, bool)>(A(kRecoverShoes))(status, 0, true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallChiral(int n) noexcept {
    __try {
        reinterpret_cast<void (*)(int)>(A(kAddChiral))(n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallForecast(std::uint16_t area, std::uint8_t region, std::uint8_t type) noexcept {
    __try {
        reinterpret_cast<void (*)(std::uint16_t, std::uint8_t, std::uint8_t)>(A(kForecastDirectEx))(area, region, type);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
int CallArea() noexcept {
    __try {
        return reinterpret_cast<std::uint16_t (*)()>(A(kCurArea))();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
bool CallVoid0(std::uintptr_t fn) noexcept {
    __try {
        reinterpret_cast<void (*)()>(fn)();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallVoidU8(std::uintptr_t fn, std::uint8_t a) noexcept {
    __try {
        reinterpret_cast<void (*)(std::uint8_t)>(fn)(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------- player
float StatusField(std::uintptr_t s, std::uintptr_t off) {
    float v = -1;
    mem::Rd(s + off, v);
    return v;
}

bool Refill(char* msg, std::size_t cap) {
    if (!Gate(Tool::player, msg, cap) || !Has(kFRefill, msg, cap)) return false;
    Sam s;
    if (const char* why = Playing(&s)) return Put(msg, cap, "refill refused: %s", why), false;
    const std::uintptr_t st = mem::Q(s.e + kSamStatus), eq = mem::Q(s.e + kSamEquip), pa = mem::Q(s.e + kSamParam);
    if (!st || !eq || !pa) return Put(msg, cap, "refill refused: Sam's status / equipment / parameters are not there"), false;
    float bat0 = 0, batmax = 0, bat1 = 0;  // floats (run 20261120-035314: 1700.0)
    mem::Rd(eq + 0x4810, bat0);
    mem::Rd(eq + 0x4814, batmax);
    const float h0 = StatusField(st, 0xC0), hm = StatusField(st, 0xC4), sta0 = StatusField(st, 0x108), stam = StatusField(st, 0x104);
    if (!CallRecoverFull(s.e)) {
        g_features &= ~kFRefill;
        return Put(msg, cap, "RecoverFull faulted; refill off for this run"), false;
    }
    mem::Rd(eq + 0x4810, bat1);
    const float h1 = StatusField(st, 0xC0), sta1 = StatusField(st, 0x108);
    Log("[dev] refill: RecoverFull(Sam, 0): status +0xC0 %.1f -> %.1f / %.1f, stamina +0x108 %.1f -> %.1f / %.1f, battery %.0f -> %.0f / %.0f", h0, h1, hm,
        sta0, sta1, stam, bat0, bat1, batmax);
    Put(msg, cap, "refilled: health %.0f -> %.0f, stamina %.0f -> %.0f, battery %.0f -> %.0f", h0, h1, sta0, sta1, bat0, bat1);
    return true;
}

std::uintptr_t DebugSettings() {
    const std::uintptr_t ds = mem::Q(A(kDebugSettingsGlobal));
    if (!ds || mem::RttiOf(ds) != A(kDebugSettingsRtti)) return 0;
    return ds;
}

bool SetGod(int mode, char* msg, std::size_t cap) {
    if (!Gate(Tool::player, msg, cap) || !Has(kFGod, msg, cap)) return false;
    if (!devrules::ValidGodMode(mode)) return Put(msg, cap, "god mode 0 off, 1 on, 2 invulnerable"), false;
    const std::uintptr_t ds = DebugSettings();
    if (!ds) return Put(msg, cap, "the game's DebugSettings object is not there (or not a DebugSettings)"), false;
    std::int32_t before = -1, after = -1;
    const std::int32_t v = mode;
    mem::Rd(ds + kDsGod, before);
    if (!WriteBytes(ds + kDsGod, &v, 4) || !mem::Rd(ds + kDsGod, after) || after != v) return Put(msg, cap, "could not write DebugSettings +0x24"), false;
    g_god_set = mode;
    Log("[dev] god mode: DebugSettings %p +0x24 %d -> %d (%s)", reinterpret_cast<void*>(ds), before, after, devrules::GodModeName(mode));
    Put(msg, cap, "god mode %s (was %s)", devrules::GodModeName(mode), devrules::GodModeName(before));
    return true;
}

bool SetAmmo(bool on, char* msg, std::size_t cap) {
    if (!Gate(Tool::player, msg, cap) || !Has(kFGod, msg, cap)) return false;
    const std::uintptr_t ds = DebugSettings();
    if (!ds) return Put(msg, cap, "the game's DebugSettings object is not there"), false;
    std::uint8_t before = 0, after = 0;
    const std::uint8_t v = on ? 1 : 0;
    mem::Rd(ds + kDsAmmo, before);
    if (!WriteBytes(ds + kDsAmmo, &v, 1) || !mem::Rd(ds + kDsAmmo, after) || after != v) return Put(msg, cap, "could not write DebugSettings +0x28"), false;
    Log("[dev] infinite ammo: DebugSettings +0x28 %u -> %u", before, after);
    Put(msg, cap, "infinite ammo %s", on ? "on" : "off");
    return true;
}

// ---------------------------------------------------------------- world
bool Weather(int code, char* msg, std::size_t cap) {
    devrules::Verdict v;
    if (!Gate(Tool::world, msg, cap, &v) || !Has(kFWeather, msg, cap)) return false;
    devrules::Weather w;
    if (!devrules::WeatherFromCode(code, &w)) return Put(msg, cap, "weather 0 clear, 1 timefall, 2 heavy timefall, 3 the game's mix"), false;
    Sam s;
    if (const char* why = Playing(&s)) return Put(msg, cap, "weather refused: %s", why), false;
    if (!mem::Q(A(kWeatherGlobal))) return Put(msg, cap, "no weather manager (no world?)"), false;
    const int area = CallArea();
    if (area < 0 || area == 0xFFFF) return Put(msg, cap, "no current area"), false;
    for (int r = 0; r < devrules::kWeatherRegions; ++r)
        if (!CallForecast(static_cast<std::uint16_t>(area), static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(w))) {
            g_features &= ~kFWeather;
            return Put(msg, cap, "SetForecastTypeOfRegionDirectEx faulted; weather off for this run"), false;
        }
    const bool reset = CallVoidU8(A(kForecastReset), 1);
    Log("[dev] weather: forecast type %d (%s) for the %d regions of area %d; forecast time reset %s", static_cast<int>(w), devrules::WeatherName(w),
        devrules::kWeatherRegions, area, reset ? "ok" : "FAULTED");
    Put(msg, cap, "weather: %s (area %d)%s", devrules::WeatherName(w), area, v.route == devrules::Route::local_only ? "; your world only" : "");
    return true;
}

bool Enemies(int op, char* msg, std::size_t cap) {
    devrules::Verdict v;
    if (!Gate(Tool::world, msg, cap, &v)) return false;
    const unsigned f = op == 3 ? kFBts : kFMules;
    if (op < 1 || op > 3) return Put(msg, cap, "enemies 1 resurrect the MULEs / terrorists, 2 remove them, 3 reset the BTs"), false;
    if (!Has(f, msg, cap)) return false;
    Sam s;
    if (const char* why = Playing(&s)) return Put(msg, cap, "refused: %s", why), false;
    const std::uintptr_t mgr = mem::Q(A(op == 3 ? kGazerGlobal : kMuleGlobal));
    if (!mgr) return Put(msg, cap, "no %s manager (no world?)", op == 3 ? "BT" : "MULE"), false;
    std::int32_t members = -1;
    if (op != 3) mem::Rd(mgr + 0x30, members);
    bool ok = false;
    if (op == 1) ok = CallVoid0(A(kMuleResurrect));
    else if (op == 2) ok = CallVoidU8(A(kMuleRemoveAll), 0);
    else ok = CallVoid0(A(kGazerReset));
    if (!ok) {
        g_features &= ~f;
        return Put(msg, cap, "the game call faulted; %s off for this run", FeatureName(f)), false;
    }
    const char* what = op == 1 ? "every MULE / terrorist resurrected (ResetAndResurrectAllMules)" : op == 2 ? "every MULE / terrorist removed from the world"
                                                                                                    : "the BTs reset (ResetAllGazers)";
    Log("[dev] enemies: %s; manager %p, %d members", what, reinterpret_cast<void*>(mgr), members);
    Put(msg, cap, "%s%s", what, v.route == devrules::Route::local_only ? "; your world only" : "");
    return true;
}

bool Items(int op, double n, char* msg, std::size_t cap) {
    devrules::Verdict v;
    if (!Gate(Tool::world, msg, cap, &v)) return false;
    Sam s;
    if (const char* why = Playing(&s)) return Put(msg, cap, "refused: %s", why), false;
    if (op == 1) {
        if (!Has(kFBoots, msg, cap)) return false;
        const std::uintptr_t st = mem::Q(s.e + kSamStatus);
        if (!st) return Put(msg, cap, "Sam's status is not there"), false;
        if (!CallShoes(st)) {
            g_features &= ~kFBoots;
            return Put(msg, cap, "RecoverOrSupplyShoes faulted; boots off for this run"), false;
        }
        Log("[dev] items: RecoverOrSupplyShoes (status %p)", reinterpret_cast<void*>(st));
        return Put(msg, cap, "boots repaired or new ones supplied"), true;
    }
    if (op == 2) {
        if (!Has(kFChiral, msg, cap)) return false;
        const int count = n >= 1 && n <= 100000 ? static_cast<int>(n) : 100;
        const std::uintptr_t ps = mem::Q(A(kPlayerSystemGlobal));
        if (!ps) return Put(msg, cap, "no DSPlayerSystem"), false;
        std::int32_t before = 0, after = 0, max = 0;
        mem::Rd(ps + 0x1B7C, before);
        mem::Rd(ps + 0x1B80, max);
        if (!CallChiral(count)) {
            g_features &= ~kFChiral;
            return Put(msg, cap, "AddChiralCrystalCount faulted; off for this run"), false;
        }
        mem::Rd(ps + 0x1B7C, after);
        Log("[dev] items: AddChiralCrystalCount(%d): %d -> %d (max %d)", count, before, after, max);
        return Put(msg, cap, "chiral crystals %d -> %d (max %d)", before, after, max), true;
    }
    return Put(msg, cap, "items 1 boots, 2 [n] chiral crystals"), false;
}

// ---------------------------------------------------------------- places
struct PlaceFull {
    Place p;
    int area = 0, type = 0;
    std::uint32_t order = 0;
    float rot[9] = {};
};
constexpr int kAllPlaces = 512;  // every place of one area (area 200 of the 100% save: more than 64); the view shows the nearest 64
PlaceFull g_places[kAllPlaces];
int g_place_count = 0;
int g_places_area = -1;
bool g_text_logged = false;

// The localized description's text, when it is readable as printable ASCII at one of the resource's pointer fields
// (LocalizedTextResource, 0x38 bytes: its layout is not proven; the first one found is logged once).
bool ReadText(std::uintptr_t lt, char* out, std::size_t cap) {
    if (!lt) return false;
    for (std::uintptr_t off = 0x20; off < 0x38; off += 8) {
        const std::uintptr_t p = mem::Q(lt + off);
        char s[64] = {};
        if (!p || !mem::Read(p, s, sizeof(s) - 1)) continue;
        if (devrules::PrintableName(s, sizeof(s))) {
            Put(out, cap, "%s", s);
            return true;
        }
    }
    return false;
}

void ReadPlaces(const Sam* sam) {
    g_place_count = 0;
    if (!(g_features & kFPlaces)) return;
    const int area = CallArea();
    g_places_area = area;
    const std::uintptr_t ms = mem::Q(A(kMissionSystemGlobal));
    if (!ms || area < 0) return;
    std::uint32_t count = 0, capacity = 0;
    const std::uintptr_t data = mem::Q(ms + kDpArray + 8);
    if (!mem::Rd(ms + kDpArray, count) || !mem::Rd(ms + kDpArray + 4, capacity) || !data || count > 4096 || count > capacity) return;
    for (std::uint32_t i = 0; i < count && g_place_count < kAllPlaces; ++i) {
        const std::uintptr_t r = mem::Q(data + 8ull * i);
        if (!r || mem::RttiOf(r) != A(kDpRtti)) continue;
        std::uint16_t a = 0;
        std::uint8_t type = 0;
        std::uint32_t order = 0;
        double pos[3] = {};
        float rot[9] = {};
        if (!mem::Rd(r + kDpArea, a) || !mem::Rd(r + kDpType, type) || !mem::Rd(r + kDpOrder, order) || !mem::Read(r + kDpXf, pos, sizeof(pos)) ||
            !mem::Read(r + kDpXf + 0x18, rot, sizeof(rot)))
            continue;
        if (a != area || !std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2]) || (pos[0] == 0 && pos[1] == 0)) continue;
        PlaceFull& f = g_places[g_place_count++];
        f.area = a, f.type = type, f.order = order;
        std::memcpy(f.p.pos, pos, sizeof(pos));
        std::memcpy(f.rot, rot, sizeof(rot));
        char name[64] = {};
        const std::uintptr_t lt = mem::Q(r + kDpText);
        ReadText(lt, name, sizeof(name));
        if (!g_text_logged && lt) {
            g_text_logged = true;
            std::uint64_t w[7] = {};
            mem::Read(lt, w, sizeof(w));
            Log("[dev] places: the first description text resource %p: %016llX %016llX %016llX %016llX %016llX %016llX %016llX -> %s", reinterpret_cast<void*>(lt),
                static_cast<unsigned long long>(w[0]), static_cast<unsigned long long>(w[1]), static_cast<unsigned long long>(w[2]),
                static_cast<unsigned long long>(w[3]), static_cast<unsigned long long>(w[4]), static_cast<unsigned long long>(w[5]),
                static_cast<unsigned long long>(w[6]), name[0] ? name : "(no readable text)");
        }
        if (name[0]) devrules::PlaceLabel(name, type, order, f.p.label, sizeof(f.p.label));
        else if (type < g_enum_count && g_enum_names[type][0]) Put(f.p.label, sizeof(f.p.label), "%s #%u", g_enum_names[type], order);
        else devrules::PlaceLabel(nullptr, type, order, f.p.label, sizeof(f.p.label));
        f.p.dist_m = 0;
        if (sam) {
            const double dx = pos[0] - sam->xf.pos.x, dy = pos[1] - sam->xf.pos.y, dz = pos[2] - sam->xf.pos.z;
            f.p.dist_m = static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz));
        }
    }
    std::sort(g_places, g_places + g_place_count, [](const PlaceFull& x, const PlaceFull& y) { return x.p.dist_m < y.p.dist_m; });
}

bool Places(char* msg, std::size_t cap) {
    if (!Gate(Tool::player, msg, cap) || !Has(kFPlaces, msg, cap)) return false;
    Sam s;
    if (const char* why = Playing(&s)) return Put(msg, cap, "places refused: %s", why), false;
    ReadPlaces(&s);
    for (int i = 0; i < g_place_count; ++i) {
        const PlaceFull& f = g_places[i];
        Log("[dev] place %d: %s (type %d, order %u, area %d) at (%.2f, %.2f, %.2f), %.0f m", i, f.p.label, f.type, f.order, f.area, f.p.pos[0], f.p.pos[1],
            f.p.pos[2], f.p.dist_m);
    }
    Put(msg, cap, "%d places in area %d (logged)", g_place_count, g_places_area);
    return g_place_count > 0;
}

bool GoPlace(int index, char* msg, std::size_t cap) {
    if (!Gate(Tool::player, msg, cap) || !Has(kFPlaces, msg, cap)) return false;
    Sam s;
    if (const char* why = Playing(&s)) return Put(msg, cap, "teleport refused: %s", why), false;
    ReadPlaces(&s);
    if (index < 0 || index >= g_place_count) return Put(msg, cap, "no place #%d (%d in this area)", index, g_place_count), false;
    const PlaceFull& f = g_places[index];
    // facing the place's own forward row; mode 1 (a short ground probe at its height: inside a building it stays on the floor)
    char why[160] = {};
    const bool ok = game::TeleportLocalPlayerFacing(f.p.pos[0], f.p.pos[1], f.p.pos[2] + 0.3, f.rot[3], f.rot[4], game::TeleportMode::place, why, sizeof(why)) ||
                    game::TeleportLocalPlayer(f.p.pos[0], f.p.pos[1], f.p.pos[2] + 0.3, game::TeleportMode::place, why, sizeof(why));
    Log("[dev] place %d (%s): %s", index, f.p.label, why);
    Put(msg, cap, ok ? "to %s: %s" : "teleport to %s failed: %s", f.p.label, why);
    return ok;
}

// ---------------------------------------------------------------- dev_goto: a place anywhere in this area, then the ground
// The teleport is the debug menu's own (game::TeleportLocalPlayer: PlaceOn mode 1 at a known z, mode 2 "the topmost ground" for
// z 0). Far places stream in after the teleport: every tick (game thread) devrules::GotoAdvance decides whether Sam stands there
// yet, fell through ground that was not loaded (placed again) or never got there (failed after 25 s).
struct GotoRun {
    bool on = false;
    devrules::GotoTrack t;
};
GotoRun g_goto;
SRWLOCK g_goto_lock = SRWLOCK_INIT;
GotoState g_goto_state;  // published (g_goto_lock)
std::uint32_t g_goto_serial = 0;

void PublishGoto(GotoState::Phase phase, const char* fmt, ...) {
    GotoState st;
    st.phase = phase;
    st.serial = g_goto_serial;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(st.text, sizeof(st.text), fmt, ap);
    va_end(ap);
    AcquireSRWLockExclusive(&g_goto_lock);
    g_goto_state = st;
    ReleaseSRWLockExclusive(&g_goto_lock);
}

bool PlaceSam(const devrules::GotoTrack& t, char* why, std::size_t cap) {
    if (t.z == 0) return game::TeleportLocalPlayer(t.x, t.y, game::kSnapRayCentreZ, game::TeleportMode::snap_to_ground, why, cap);
    return game::TeleportLocalPlayer(t.x, t.y, t.z + 0.5, game::TeleportMode::place, why, cap);
}

// A goto refused before it placed Sam still takes a serial and publishes 'failed' (any older goto still running is
// dropped): a 'waitworld <s> goto' after it then fails at once instead of reading the older goto's 'settled' (an
// optional '?goto' step whose action was refused would otherwise pass without Sam having moved).
bool RefuseGoto(const char* msg) {
    if (g_goto.on) {
        g_goto.on = false;
        PublishGoto(GotoState::failed, "goto #%u: replaced by a newer goto", g_goto_serial);
    }
    ++g_goto_serial;
    PublishGoto(GotoState::failed, "goto #%u: %s", g_goto_serial, msg);
    Log("[dev] goto #%u: %s", g_goto_serial, msg);
    return false;
}

bool Goto(double x, double y, double z, double now_ms, char* msg, std::size_t cap) {
    if (!Gate(Tool::world, msg, cap)) return RefuseGoto(msg);
    if (!devrules::GotoTargetOk(x, y, z)) return Put(msg, cap, "goto: x y [z] in the map's range (z 0 = the ground under x, y)"), RefuseGoto(msg);
    Sam s;
    if (const char* why = Playing(&s)) return Put(msg, cap, "goto refused: %s", why), RefuseGoto(msg);
    char why[160] = {};
    const int gate = game::PlacementGate(why, sizeof(why));
    if (gate != 0) return Put(msg, cap, "goto refused (on a vehicle? get off first): %s", why), RefuseGoto(msg);
    if (g_goto.on) PublishGoto(GotoState::failed, "goto #%u: replaced by a newer goto", g_goto_serial);
    g_goto = GotoRun{};
    g_goto.t.x = x, g_goto.t.y = y, g_goto.t.z = z;
    g_goto.t.t0 = g_goto.t.last_place = now_ms;
    ++g_goto_serial;
    const double dx = x - s.xf.pos.x, dy = y - s.xf.pos.y;
    const double dist = std::sqrt(dx * dx + dy * dy);
    char zs[32];
    if (z == 0) Put(zs, sizeof(zs), "the ground");
    else Put(zs, sizeof(zs), "%.2f", z);
    if (!PlaceSam(g_goto.t, why, sizeof(why))) {
        PublishGoto(GotoState::failed, "goto #%u: the teleport was refused: %s", g_goto_serial, why);
        Log("[dev] goto #%u: to (%.2f, %.2f, %s) refused: %s", g_goto_serial, x, y, zs, why);
        return Put(msg, cap, "goto #%u refused: %s", g_goto_serial, why), false;
    }
    g_goto.on = true;
    Log("[dev] goto #%u: from (%.2f, %.2f, %.2f) to (%.2f, %.2f, %s), %.0f m: %s", g_goto_serial, s.xf.pos.x, s.xf.pos.y, s.xf.pos.z, x, y, zs, dist, why);
    PublishGoto(GotoState::moving, "goto #%u: placed, waiting for the ground", g_goto_serial);
    return Put(msg, cap, "goto #%u: %.0f m to (%.0f, %.0f, %s); waiting for the ground", g_goto_serial, dist, x, y, zs), true;
}

void GotoTick(double now_ms) {
    if (!g_goto.on) return;
    Sam s;
    const char* block = Playing(&s);
    double pos[3] = {s.xf.pos.x, s.xf.pos.y, s.xf.pos.z};
    char why[160] = {};
    const bool playable = !block && game::PlacementGate(why, sizeof(why)) == 0;
    if (!s.e) pos[0] = pos[1] = pos[2] = 1e9;  // no player (a load): never "at" the target
    const devrules::GotoStep step = devrules::GotoAdvance(g_goto.t, playable, pos, now_ms);
    const double secs = (now_ms - g_goto.t.t0) / 1000.0;
    const char* reason = block ? block : why;
    if (step == devrules::GotoStep::place_again) {
        const bool ok = PlaceSam(g_goto.t, why, sizeof(why));
        Log("[dev] goto #%u: Sam at z %.2f, %.1f m below the target after %.1f s (the ground was not in yet): placed again (#%d): %s", g_goto_serial, pos[2],
            g_goto.t.z - pos[2], secs, g_goto.t.placements, ok ? why : "refused");
    } else if (step == devrules::GotoStep::settled) {
        g_goto.on = false;
        Log("[dev] goto #%u: settled at (%.2f, %.2f, %.2f) after %.1f s (%d placement(s))", g_goto_serial, pos[0], pos[1], pos[2], secs, g_goto.t.placements);
        PublishGoto(GotoState::settled, "goto #%u: settled at (%.2f, %.2f, %.2f) after %.1f s", g_goto_serial, pos[0], pos[1], pos[2], secs);
    } else if (step == devrules::GotoStep::timeout) {
        g_goto.on = false;
        Log("[dev] goto #%u: NOT settled after %.1f s: Sam at (%.2f, %.2f, %.2f)%s%s", g_goto_serial, secs, pos[0], pos[1], pos[2], reason[0] ? "; " : "", reason);
        PublishGoto(GotoState::failed, "goto #%u: NOT settled after %.0f s (Sam at %.0f, %.0f, %.0f%s%s)", g_goto_serial, secs, pos[0], pos[1], pos[2],
                    reason[0] ? "; " : "", reason);
    }
}

// ---------------------------------------------------------------- boots' life (dev_boots)
struct Boots {
    float life = -1, max = -1, spare = -1, over = -1;
    std::uint8_t flags = 0, type = 0;
    std::uintptr_t item = 0, status = 0;
};
bool ReadBoots(const Sam& s, Boots* b) {
    b->status = mem::Q(s.e + kSamStatus);
    if (!b->status) return false;
    const std::uintptr_t st = b->status;
    return mem::Rd(st + kShoesLife, b->life) && mem::Rd(st + kShoesMax, b->max) && mem::Rd(st + kShoesSpare, b->spare) &&
           mem::Rd(st + kShoesOver, b->over) && mem::Rd(st + kShoesFlags, b->flags) && mem::Rd(st + kShoesType, b->type) && mem::Rd(st + kShoesItem, b->item);
}
// The boots' durability is two parts (the setter 0x142741720): the first +0x334 (200) points at +0x33C, the rest above it at
// +0x32C up to +0x330 (3400); SetShoesLifeByRate sets their sum to rate x (+0x334 + +0x330). Run 20261121-084433-spawn-showcase:
// 200 + 926.6 of 200 + 3400 before, 40 % gave 200 + 1240 = 1440 of 3600.
float BootsNow(const Boots& b) { return b.life + (b.over > 0 ? b.over : 0); }
float BootsMax(const Boots& b) { return b.max + (b.spare > 0 ? b.spare : 0); }
void FormatBoots(const Boots& b, char* out, std::size_t cap) {
    const float now = BootsNow(b), max = BootsMax(b);
    Put(out, cap, "durability %.1f / %.1f (%.0f%%; first part %.1f / %.1f, rest %.1f / %.1f), worn item %s, flags 0x%02X, type %u", now, max,
        max > 0 ? 100.0 * now / max : 0.0, b.life, b.max, b.over, b.spare, b.item ? "set" : "none", b.flags, b.type);
}
bool CallShoesRate(std::uintptr_t status, float rate) noexcept {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, float, bool)>(A(kShoesByRate))(status, rate, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallShoesForcibly(std::uint8_t type) noexcept {
    __try {
        reinterpret_cast<void (*)(std::uint8_t)>(A(kSetShoesForcibly))(type);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool g_boots_meter = false;
double g_boots_next = 0;
float g_boots_last = -1;

bool BootsCmd(int op, double v, char* msg, std::size_t cap) {
    if (!Gate(op == 1 ? Tool::world : Tool::player, msg, cap) || !Has(kFBootsLife, msg, cap)) return false;
    if (op == 2) {
        g_boots_meter = v != 0;
        g_boots_last = -1;
        Log("[dev] boots meter %s", g_boots_meter ? "on (one line a second)" : "off");
        return Put(msg, cap, "boots meter %s", g_boots_meter ? "on" : "off"), true;
    }
    Sam s;
    if (!ReadSam(&s)) return Put(msg, cap, "no local player"), false;
    Boots b;
    if (!ReadBoots(s, &b)) return Put(msg, cap, "Sam's status is not there"), false;
    char line[200];
    FormatBoots(b, line, sizeof(line));
    if (op == 3) {
        // round 6 T3: SetShoesForcibly(type) (0x1426E7430, byte-checked above): the request byte status +0x3B8; the status
        // tick (0x14273251B) equips boots of that type when it differs from the worn type (status +0x328) and clears it
        // (0xB). Hidden runs / sandboxed saves only (Gate world): it changes what the save's Sam wears.
        if (!(v >= 0 && v <= 10)) return Put(msg, cap, "boots 3 <type 0..10> (0 barefoot, 2 sole grass, 3 porter, 4-6 all-terrain, 7 mule, 8 terrorist, 9 chiral, 10 reflect)"), false;
        if (!Gate(Tool::world, msg, cap)) return false;
        if (const char* why = Playing(&s)) return Put(msg, cap, "refused: %s", why), false;
        if (!CallShoesForcibly(static_cast<std::uint8_t>(v))) {
            g_features &= ~kFBootsLife;
            return Put(msg, cap, "SetShoesForcibly faulted; boots' life off for this run"), false;
        }
        std::uint8_t worn_type = 0xFF;
        mem::Rd(b.status + kShoesWornType, worn_type);
        Log("[dev] boots: SetShoesForcibly(%.0f) requested (worn type now %u; the status tick equips it): %s", v, worn_type, line);
        return Put(msg, cap, "boots type %.0f requested (was %u)", v, worn_type), true;
    }
    if (op == 0) {
        Log("[dev] boots: %s", line);
        return Put(msg, cap, "boots: %s", line), true;
    }
    if (op != 1) return Put(msg, cap, "boots 0 log, 1 <percent> set the life, 2 <0/1> meter, 3 <type> equip a boot type"), false;
    float rate = 0;
    if (!devrules::BootsRate(v, &rate)) return Put(msg, cap, "boots 1 <percent 0..100>"), false;
    if (const char* why = Playing(&s)) return Put(msg, cap, "refused: %s", why), false;
    if (!b.item) return Put(msg, cap, "no boots worn (status +0x368 empty): the game's setter would do nothing"), false;
    if (!CallShoesRate(b.status, rate)) {
        g_features &= ~kFBootsLife;
        return Put(msg, cap, "SetShoesLifeByRate faulted; boots' life off for this run"), false;
    }
    Boots a;
    ReadBoots(s, &a);
    char after[200];
    FormatBoots(a, after, sizeof(after));
    Log("[dev] boots: set to %.0f%% (SetShoesLifeByRate %.3f): %s -> %s", v, rate, line, after);
    return Put(msg, cap, "boots durability %.1f -> %.1f / %.1f", BootsNow(b), BootsNow(a), BootsMax(a)), true;
}

void BootsTick(double now_ms) {
    if (!g_boots_meter || now_ms < g_boots_next || !(g_features & kFBootsLife)) return;
    g_boots_next = now_ms + 1000;
    Sam s;
    Boots b;
    if (!ReadSam(&s) || !ReadBoots(s, &b)) return;
    const std::uintptr_t parent = mem::Q(s.e + 0x70);  // entity +0x70: the parent (game_actions.cpp kEntParent)
    char line[200];
    FormatBoots(b, line, sizeof(line));
    Log("[dev] boots meter: %s; %+.2f in 1 s; Sam at (%.1f, %.1f, %.1f)%s", line, g_boots_last < 0 ? 0.0 : static_cast<double>(BootsNow(b) - g_boots_last),
        s.xf.pos.x, s.xf.pos.y, s.xf.pos.z, parent ? ", attached to a parent (a vehicle)" : "");
    g_boots_last = BootsNow(b);
}

// ---------------------------------------------------------------- view
void FillView(double now_ms) {
    View v;
    v.enabled = g_enabled;
    v.test_real = g_test_real;
    const devrules::Session s = SessionNow();
    Put(v.session, sizeof(v.session), "%s%s", s.saves_redirected ? "sandboxed save (Local test / test save / hidden run)" : "real save",
        g_test_real ? " (test override)" : "");
    const Tool tools[4] = {Tool::player, Tool::world, Tool::vehicle_spawn, Tool::vehicle_edit};
    for (int i = 0; i < 4; ++i) {
        const devrules::Verdict d = devrules::Decide(tools[i], s);
        v.ok[i] = d.ok();
        Put(v.why[i], sizeof(v.why[i]), "%s", d.why);
        Put(v.note[i], sizeof(v.note[i]), "%s",
            d.route == devrules::Route::local_only ? "your world only (not shared)" : d.route == devrules::Route::ask_host ? "the host's game decides" : "");
    }
    const vshare::DevChecks vc = vshare::DevCheckResult();
    v.veh_pools = vc.pools, v.veh_spawn = vc.spawn, v.veh_repair = vc.repair;
    if (g_off[0] && vc.off[0]) Put(v.off, sizeof(v.off), "%s; %s", g_off, vc.off);
    else Put(v.off, sizeof(v.off), "%s", g_off[0] ? g_off : vc.off);
    if (g_features & kFGod) {
        if (const std::uintptr_t ds = DebugSettings()) {
            std::int32_t god = -1;
            std::uint8_t ammo = 0;
            if (mem::Rd(ds + kDsGod, god)) v.god = god;
            if (mem::Rd(ds + kDsAmmo, ammo)) v.ammo = ammo;
        }
    }
    v.area = g_places_area;
    v.place_count = (std::min)(g_place_count, View::kMaxPlaces);
    for (int i = 0; i < v.place_count; ++i) v.places[i] = g_places[i].p;
    (void)now_ms;
    AcquireSRWLockExclusive(&g_view_lock);
    v.serial = g_view.serial + 1;
    g_view = v;
    ReleaseSRWLockExclusive(&g_view_lock);
}

}  // namespace

void Install(const Config& config) noexcept {
    g_role = config.role == Role::host ? 1 : config.role == Role::guest ? 2 : 0;
    g_host_save = config.host_save && (g_role == 1 || g_role == 2);
    g_enabled = config.dev_tools || automation::Enabled();
    Log("[dev] dev tools %s (%s); world tools: %s", g_enabled ? "ON" : "off",
        config.dev_tools ? "dev_tools=1" : automation::Enabled() ? "hidden run" : "dev_tools=0",
        !g_enabled ? "-" : automation::SavesRedirected() ? "this game runs on a sandboxed save" : "refused (this game plays on its real save)");
}

bool Enabled() noexcept { return g_enabled; }

const char* HostRefusesGuestDev() noexcept {
    if (!g_enabled) return "refused by the host: its dev tools are off";
    if (!devrules::HostAllowsDevRequest(automation::SavesRedirected() && !g_test_real)) return devrules::kHostRealReason;
    return nullptr;
}

const char* RealSaveRefusal() noexcept {
    return automation::SavesRedirected() && !g_test_real ? nullptr : devrules::kRealSaveReason;
}

void Tick(double now_ms) noexcept {
    g_now_ms = now_ms;
    quickstart::Tick(now_ms);  // hidden runs' "start" (quickstart.cpp): the title's Continue / Load, no menu steps
    vshare::DevTick(now_ms);
    story::Tick(now_ms, g_enabled, SessionNow());  // the story tools (story_state.cpp): the probe, the slots, a pending load
    if (g_enabled) {
        GotoTick(now_ms);
        BootsTick(now_ms);
    }
    if (!g_enabled || now_ms < g_next_view) return;
    g_next_view = now_ms + 1000;
    Check();
    // The place list calls the game's GetCurrentArea: only while the world is played. During a load its slow path waited on
    // the loader and held the game thread (run 20261120-040412: the guest's load stood 200 s).
    Sam s;
    if (!Playing(&s)) ReadPlaces(&s);
    FillView(now_ms);
}

bool Command(Op op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    switch (op) {
        case Op::status: {
            if (static_cast<int>(v[0]) == 2) {  // hidden runs: the open menu shows the Dev tools page (screenshots)
                if (!automation::Enabled()) return Put(msg, cap, "hidden runs only"), false;
                overlay::menu::SelectTab(5);
                return Put(msg, cap, "the menu shows the Dev tools page"), true;
            }
            if (static_cast<int>(v[0]) == 3) {  // hidden runs: Sam v1 metres up (a fall test for god mode)
                if (!automation::Enabled()) return Put(msg, cap, "hidden runs only"), false;
                Sam s;
                if (!ReadSam(&s)) return Put(msg, cap, "no local player"), false;
                const double up = v[1] > 0 && v[1] <= 200 ? v[1] : 30;
                char why[160] = {};
                const bool ok = game::TeleportLocalPlayer(s.xf.pos.x, s.xf.pos.y, s.xf.pos.z + up, game::TeleportMode::place, why, sizeof(why));
                Log("[dev] test: Sam %.0f m up from z %.2f: %s", up, s.xf.pos.z, why);
                return Put(msg, cap, "%.0f m up: %s", up, why), ok;
            }
            if (static_cast<int>(v[0]) == 1) {
                if (!automation::Enabled()) return Put(msg, cap, "hidden runs only"), false;
                g_test_real = v[1] != 0;
                Log("[dev] test: %s", g_test_real ? "acting as if this game played on its real save" : "the real save state again");
                return Put(msg, cap, "test real-save override %s", g_test_real ? "ON" : "off"), true;
            }
            Check();
            const devrules::Session s = SessionNow();
            const Tool tools[4] = {Tool::player, Tool::world, Tool::vehicle_spawn, Tool::vehicle_edit};
            const char* names[4] = {"player", "world", "vehicle spawn", "vehicle edit"};
            for (int i = 0; i < 4; ++i) {
                const devrules::Verdict d = devrules::Decide(tools[i], s);
                Log("[dev] status: %s tools: %s%s", names[i], d.ok() ? "allowed" : d.why,
                    d.route == devrules::Route::local_only ? " (this world only)" : d.route == devrules::Route::ask_host ? " (the host decides)" : "");
            }
            // the Player tab's time of day and day/night cycle (debug_actions.cpp): the real-save rule without the dev tools
            const char* time_why = RealSaveRefusal();
            Log("[dev] status: time of day: %s", time_why ? time_why : "allowed");
            return Put(msg, cap, "dev tools %s, %s, role %d, host_save %d; byte checks: %s", g_enabled ? "on" : "off",
                       s.saves_redirected ? "sandboxed save" : "real save", g_role, g_host_save ? 1 : 0, g_off[0] ? g_off : "all ok"),
                   true;
        }
        case Op::vehicle: {
            if (!Gate(Tool::vehicle_spawn, msg, cap)) return false;
            Sam s;
            if (const char* why = Playing(&s)) return Put(msg, cap, "refused: %s", why), false;
            return vshare::DevSpawn(static_cast<int>(v[0]), static_cast<int>(v[1]), v[2], msg, cap);
        }
        case Op::vehicle_remove:
            if (!Gate(Tool::vehicle_edit, msg, cap)) return false;
            return vshare::DevRemove(v[0], v[1] != 0, msg, cap);
        case Op::vehicle_fix:
            if (!Gate(Tool::vehicle_edit, msg, cap)) return false;
            return vshare::DevRepair(v[0], msg, cap);
        case Op::refill: return Refill(msg, cap);
        case Op::god: return SetGod(static_cast<int>(v[0]), msg, cap);
        case Op::ammo: return SetAmmo(v[0] != 0, msg, cap);
        case Op::weather: return Weather(static_cast<int>(v[0]), msg, cap);
        case Op::enemies: return Enemies(static_cast<int>(v[0]), msg, cap);
        case Op::places: return Places(msg, cap);
        case Op::place: return GoPlace(static_cast<int>(v[0]), msg, cap);
        case Op::items: return Items(static_cast<int>(v[0]), v[1], msg, cap);
        case Op::go_to: return Goto(v[0], v[1], v[2], g_now_ms, msg, cap);
        case Op::boots: return BootsCmd(static_cast<int>(v[0]), v[1], msg, cap);
    }
    return Put(msg, cap, "unknown dev op"), false;
}

View GetView() noexcept {
    AcquireSRWLockShared(&g_view_lock);
    View v = g_view;
    ReleaseSRWLockShared(&g_view_lock);
    return v;
}

GotoState GetGotoState() noexcept {
    AcquireSRWLockShared(&g_goto_lock);
    GotoState g = g_goto_state;
    ReleaseSRWLockShared(&g_goto_lock);
    return g;
}

std::uint32_t GotoSerial() noexcept { return GetGotoState().serial; }

}  // namespace st::dev
