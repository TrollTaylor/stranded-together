// Dev tools, game side. See dev_tools.hpp and docs/debug-actions.md "Dev tools" for the evidence behind every address.
#include "dev_tools.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "automation.hpp"
#include "config.hpp"
#include "game_actions.hpp"
#include "game_api.hpp"
#include "give_rules.hpp"
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
constexpr std::uintptr_t kDpText = 0x20, kDpArea = 0x68, kDpType = 0x6A, kDpLocatorId = 0x70, kDpOrder = 0x78, kDpXf = 0x80;
// A place's stand point (round 18, dev_place): its construction point in DSConstructionPointManager's live list (+0x56A258 i32
// count, +0x56A260 data; GetNearestConstructionPointId 0x142CD8A20 reads both), cp+0x30 id, +0x34 type, +0x40 position,
// +0x468 transform (position, then the rotation's rows), +0x90 its DSConstructionPointConfig: +0x50 / +0x60 the collision box
// (UnrealizeCollisionBoundingBox, local min / max), +0x2D0 TerminalFrontAccessOffset. GetConstructionPointFrontAccessTransform
// (body 0x142CD80E0, export thunk 0x142CD19A0) = the transform moved by that offset along its rows: we read the same fields
// (byte-checked at that function's own reads) and do its math (devrules::FrontAccess).
constexpr std::uintptr_t kCpMgrGlobal = 0x147BF4960;
constexpr std::uintptr_t kCpLiveCount = 0x56A258, kCpLiveData = 0x56A260;
constexpr std::uintptr_t kCpIdOff = 0x30, kCpTypeOff = 0x34, kCpPos40 = 0x40, kCpXf = 0x468, kCpCfgOff = 0x90;
constexpr std::uintptr_t kCfgBoxMin = 0x50, kCfgBoxMax = 0x60, kCfgFront = 0x2D0;
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
// Give (round 22, dev_give; scratchpad round22 re-items + this stage's byte reads). The DSBaggage2Manager script export
// CreateAndAddBaggageToPlayer(u32 nameCode, u8 slot) is the thunk 0x142CD1440 (jmp 0x142CD6A60). Its body builds a 0x90-byte
// request on the stack (+0 u8 type 3, +4 the code, +8 u64 0x1FFFFF so +0xC 0 = the player holder [inferred], +0x10 the u8) and
// pushes it with 0x142C2A9C0 onto the manager's request array ([0x147BF4968] +0x46D9F0 count, +0x46D9F8 data; no lock: the game
// thread only). The manager's update 0x142C95530 reads that array (0x142C95564) and creates the baggage on its next update.
// The body reads the manager global without a null check: we check it first. The catalogue check is the script export
// GetBaggageListItemByName(u32) 0x1433C8CE0 (mov rcx,[0x147C0DD98]; jmp 0x143439DF0: a read-only crc32 hash map at map+0x158,
// capacity +0x164, entries 0x18 bytes): it returns the DSGameBaggageListItem or null. The manager's baggage array (shot_sync.cpp:
// [mgr] +0x20 + i * 0x240, +0x21 non-zero in use, +0x28 the list item, +0x38 position) counts what arrived.
constexpr std::uintptr_t kGiveThunk = 0x142CD1440, kGiveBody = 0x142CD6A60, kGivePush = 0x142C2A9C0, kGiveConsume = 0x142C95564;
constexpr std::uintptr_t kBagLookup = 0x1433C8CE0, kBagLookupBody = 0x143439DF0;
constexpr std::uintptr_t kBagMgrGlobal = 0x147BF4968, kBagListGlobal = 0x147C0DD98;
constexpr std::uintptr_t kBagArray = 0x20, kBagStride = 0x240, kBagUse = 0x21, kBagItem = 0x28, kBagPos = 0x38, kBagHolder = 0x90;
constexpr unsigned kBagMax = 8000;  // the array ends before the temporary list (+0x46DDA8; shot_sync.cpp)

enum Feature : unsigned {
    kFRefill = 1u, kFGod = 2u, kFWeather = 4u, kFMules = 8u, kFBts = 16u, kFPlaces = 32u, kFBoots = 64u, kFChiral = 128u, kFBootsLife = 256u, kFStand = 512u,
    kFGive = 1024u,
};
constexpr unsigned kFLast = kFGive;
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
        case kFStand: return "place stand point";
        case kFGive: return "give";
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
    // A place's stand point: GetConstructionPointFrontAccessTransform's reads (cp+0x4A0 / +0x468 / +0x478 / +0x480, cp+0x90 the
    // config, config+0x2D0 the offset), its export thunk, GetNearestConstructionPointId's live list (+0x56A260 / +0x56A258)
    {0x142CD80E0, "40 53 48 81 EC 50 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 A0 00 00 00 48 8B DA 8B D1 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ??",
     "GetConstructionPointFrontAccessTransform", kFStand},
    {0x142CD8115, "8B 88 A0 04 00 00", "front access: cp+0x4A0", kFStand},
    {0x142CD8168, "C5 78 10 A8 68 04 00 00", "front access: cp+0x468", kFStand},
    {0x142CD8179, "C5 7B 10 B0 78 04 00 00", "front access: cp+0x478", kFStand},
    {0x142CD818A, "C5 7C 10 B8 80 04 00 00", "front access: cp+0x480", kFStand},
    {0x142CD8192, "48 8B 80 90 00 00 00", "front access: cp+0x90 config", kFStand},
    {0x142CD81AD, "C5 F8 10 B0 D0 02 00 00", "front access: config+0x2D0", kFStand},
    {0x142CD19A0, "E9 3B 67 00 00", "GetConstructionPointFrontAccessTransform export thunk", kFStand},
    {0x142CD8A5C, "48 8B B0 60 A2 56 00 48 63 80 58 A2 56 00", "construction point live list (+0x56A260 / +0x56A258)", kFStand},
    // the fields the landing reads besides the front access: GetNearestConstructionPointId hands out the nearest's id (cp+0x30);
    // a live-list walk compares the type byte (cp+0x34 == 0x18); another takes cp+0x90's collision box (config+0x50 min /
    // +0x60 max, checked min <= max) to place it at cp+0x40
    {0x142CD8B3C, "4D 85 FF 74 0E 4D 85 F6 74 15 41 8B 47 30 41 89 06", "nearest construction point's id (cp+0x30)", kFStand},
    {0x142CCECA9, "48 8B 85 60 A2 56 00 40 B7 01 49 8B 1C 06 80 7B 34 18", "construction point type (live list, cp+0x34)", kFStand},
    {0x142CD9377, "48 8B 83 90 00 00 00 80 8B AB 00 00 00 10 80 8B AF 00 00 00 02 80 8B AA 00 00 00 84 C5 F8 10 48 50 C5 F8 11 4C 24 40 C5 F8 10 40 60",
     "construction point config's collision box (config+0x50 / +0x60)", kFStand},
    // Give: CreateAndAddBaggageToPlayer's thunk and body (the request: code at +4 (rsp+0x44), the u8 at +0x10, type 3, the push
    // onto [0x147BF4968] +0x46D9F0), the push, the manager update's read of that array, the catalogue lookup and its body
    {kGiveThunk, "E9 1B 56 00 00", "CreateAndAddBaggageToPlayer export thunk", kFGive},
    {kGiveBody, "4C 8B DC 48 81 EC E8 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4", "CreateAndAddBaggageToPlayer body", kFGive},
    {0x142CD6ACF, "89 4C 24 44 48 8B 0D ?? ?? ?? ?? 88 54 24 50 48 81 C1 F0 D9 46 00 C6 44 24 40 03 48 8D 54 24 40 C5 F8 77 E8 C9 3E F5 FF",
     "CreateAndAddBaggageToPlayer: the request pushed to the manager +0x46D9F0", kFGive},
    {kGivePush, "48 89 5C 24 20 56 48 83 EC 20 4C 8B 41 08 48 8B F2", "baggage request array push", kFGive},
    {kGiveConsume, "4D 8B BE F8 D9 46 00 48 8B F8 49 63 96 F0 D9 46 00", "baggage manager update reads the requests (+0x46D9F8 / +0x46D9F0)", kFGive},
    {kBagLookup, "8B D1 48 8B 0D ?? ?? ?? ?? E9 02 11 07 00", "GetBaggageListItemByName", kFGive},
    {kBagLookupBody, "40 53 4C 8B 81 58 01 00 00 48 8B D9 48 89 74 24 10 8B F2", "baggage list lookup (map +0x158)", kFGive},
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
    unsigned features = kFRefill | kFGod | kFWeather | kFMules | kFBts | kFPlaces | kFBoots | kFChiral | kFBootsLife | kFStand | kFGive;
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
    rip(0x142CD8100, kCpMgrGlobal, kFStand, "the construction point manager global (front access)");
    rip(0x142CD8A28, kCpMgrGlobal, kFStand, "the construction point manager global (live list)");
    rip(0x142CD6AD3, kBagMgrGlobal, kFGive, "the baggage manager global (CreateAndAddBaggageToPlayer)");
    rip(0x1433C8CE2, kBagListGlobal, kFGive, "the baggage list map global (GetBaggageListItemByName)");
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

// ---------------------------------------------------------------- dev_give: weapons, tools and equipment to Sam (round 22)
// A press (Give, the game thread) checks the gates, builds the plan (give_rules.hpp: the allow list, the weight budget),
// checks every code through the game's own catalogue lookup (a DSGameBaggageListItem or skipped) and notes which baggages of
// those list items exist. GiveTick then sends dev_give_per_frame requests a tick through CreateAndAddBaggageToPlayer(code,
// dev_give_slot) (the u8 is the destination kind: give_rules.hpp Slot), logs "[dev] give <name>: n added, n skipped (why)" and 3 s later
// counts the new baggages of those list items: "[dev] give <name>: k/n arrived".
bool g_give_cfg = true;
double g_give_max_kg = giverules::kDefaultMaxKg;
int g_give_per_frame = giverules::kDefaultPerFrame;
int g_give_slot = giverules::kDefaultSlot;  // session.ini dev_give_slot: the request's u8 (give_rules.hpp Slot)
char g_give_line[160] = {};
struct GiveRun {
    bool active = false;
    int phase = 0;  // 1 sending, 2 waiting for the check
    giverules::Plan plan;
    std::uintptr_t li[giverules::kMaxPlan] = {};
    int next = 0, added = 0, skipped = 0;
    char why[400] = {};
    double check_at = 0, give_up_at = 0;
    std::vector<std::uint32_t> before;  // the baggage array's indices of those list items before the press
};
GiveRun g_give;

bool CallBagLookup(std::uint32_t code, std::uintptr_t* out) noexcept {
    __try {
        *out = reinterpret_cast<std::uintptr_t (*)(std::uint32_t)>(A(kBagLookup))(code);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallGive(std::uint32_t code, std::uint8_t slot) noexcept {
    __try {
        reinterpret_cast<void (*)(std::uint32_t, std::uint8_t)>(A(kGiveBody))(code, slot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void AddWhy(char* why, std::size_t cap, const char* fmt, ...) {
    const std::size_t len = std::strlen(why);
    if (len + 4 >= cap) return;
    if (len) std::snprintf(why + len, cap - len, "; ");
    const std::size_t at = std::strlen(why);
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(why + at, cap - at, fmt, ap);
    va_end(ap);
}

// The baggage array's in-use entries whose list item is one of `lis` (indices).
void ScanBags(std::uintptr_t mgr, const std::uintptr_t* lis, int n, std::vector<std::uint32_t>* out) {
    out->clear();
    constexpr unsigned kChunk = 64;
    static std::uint8_t buf[kChunk * kBagStride];
    for (unsigned base = 0; base < kBagMax; base += kChunk) {
        const unsigned cnt = kBagMax - base < kChunk ? kBagMax - base : kChunk;
        if (!mem::Read(mgr + kBagArray + kBagStride * base, buf, kBagStride * cnt)) continue;
        for (unsigned j = 0; j < cnt; ++j) {
            const std::uint8_t* b = buf + kBagStride * j;
            if (!b[kBagUse]) continue;
            std::uint64_t li = 0;
            std::memcpy(&li, b + kBagItem, 8);
            for (int k = 0; k < n; ++k)
                if (li && li == lis[k]) {
                    out->push_back(base + j);
                    break;
                }
        }
    }
}

bool Give(int group, double code, double count, char* msg, std::size_t cap) {
    devrules::Verdict v;
    if (!Gate(Tool::world, msg, cap, &v)) return false;
    if (!g_give_cfg) {
        Log("[dev] give refused: session.ini dev_give=0");
        return Put(msg, cap, "give is off (session.ini dev_give=0)"), false;
    }
    if (!Has(kFGive, msg, cap)) return false;
    if (g_give.active) return Put(msg, cap, "give refused: the last press (%s) is still running", g_give.plan.name), false;
    Sam s;
    if (const char* why = Playing(&s)) return Put(msg, cap, "give refused: %s", why), false;
    giverules::Plan plan;
    if (const char* why = giverules::BuildPlan(group, code, count, g_give_max_kg, &plan)) {
        Log("[dev] give refused: %s", why);
        return Put(msg, cap, "give refused: %s", why), false;
    }
    const std::uintptr_t mgr = mem::Q(A(kBagMgrGlobal)), map = mem::Q(A(kBagListGlobal));
    if (!mgr || !map) return Put(msg, cap, "give refused: no baggage manager or baggage list (no world?)"), false;
    GiveRun r;
    r.plan = giverules::Plan{};
    r.plan.name = plan.name;
    r.skipped = plan.skipped;
    if (plan.skipped) AddWhy(r.why, sizeof(r.why), "over the %.0f kg budget: %s", g_give_max_kg, plan.skipped_names);
    char missing[200] = {};
    int lookup_skipped = 0;
    for (int i = 0; i < plan.n; ++i) {
        const std::uint32_t c = plan.codes[i];
        const giverules::Entry* e = giverules::Find(c);
        std::uintptr_t li = 0;
        bool seen = false;
        for (int k = 0; k < r.plan.n; ++k)
            if (r.plan.codes[k] == c) li = r.li[k], seen = true;
        if (!seen) {
            if (!CallBagLookup(c, &li)) {
                g_features &= ~kFGive;
                Log("[dev] give: GetBaggageListItemByName(%u) faulted; give off for this run", c);
                return Put(msg, cap, "the game's baggage lookup faulted; give off for this run"), false;
            }
            const char* type = li ? mem::RttiName(mem::RttiOf(li)) : nullptr;
            if (!type || std::strcmp(type, "DSGameBaggageListItem") != 0) {
                Log("[dev] give: %s (code %u) is not in the game's baggage list (%p %s): skipped", e ? e->name : "?", c, reinterpret_cast<void*>(li),
                    type ? type : "-");
                li = 0;
            } else {
                // evidence for the list item's fields (re-items 2g: the NameCode at +0x34 [inferred]; partner_gear: +0x30 an id)
                std::uint32_t f30 = 0, f34 = 0;
                mem::Rd(li + 0x30, f30);
                mem::Rd(li + 0x34, f34);
                Log("[dev] give: %s (code %u): list item %p, +0x30 %u, +0x34 %u", e ? e->name : "?", c, reinterpret_cast<void*>(li), f30, f34);
            }
        }
        if (!li) {
            ++lookup_skipped;
            const std::size_t len = std::strlen(missing);
            if (len + 2 < sizeof(missing)) std::snprintf(missing + len, sizeof(missing) - len, "%s%s", len ? ", " : "", e ? e->name : "?");
            continue;
        }
        r.plan.codes[r.plan.n] = c;
        r.li[r.plan.n] = li;
        ++r.plan.n;
        r.plan.kg += e ? e->kg : 0;
    }
    if (lookup_skipped) {
        r.skipped += lookup_skipped;
        AddWhy(r.why, sizeof(r.why), "not in the game's baggage list: %s", missing);
    }
    if (r.plan.n == 0) {
        Log("[dev] give %s: 0 added, %d skipped (%s)", plan.name, r.skipped, r.why[0] ? r.why : "nothing to give");
        Put(g_give_line, sizeof(g_give_line), "%s: nothing given (%s)", plan.name, r.why[0] ? r.why : "nothing to give");
        return Put(msg, cap, "give %s: nothing given (%s)", plan.name, r.why[0] ? r.why : "nothing to give"), false;
    }
    ScanBags(mgr, r.li, r.plan.n, &r.before);
    r.active = true;
    r.phase = 1;
    g_give = r;
    Put(g_give_line, sizeof(g_give_line), "%s: giving %d (%.1f kg)...", plan.name, r.plan.n, r.plan.kg);
    Log("[dev] give %s: %d requests (%.1f kg, budget %.0f kg, %d a tick), %d skipped; %zu of those baggages before%s", plan.name, r.plan.n, r.plan.kg,
        g_give_max_kg, g_give_per_frame, r.skipped, r.before.size(), v.route == devrules::Route::local_only ? "; this world only" : "");
    return Put(msg, cap, "giving %s: %d (%.1f kg)%s%s%s", plan.name, r.plan.n, r.plan.kg, r.skipped ? ", skipped: " : "", r.skipped ? r.why : "",
               v.route == devrules::Route::local_only ? "; your world only" : ""),
           true;
}

void GiveTick(double now_ms) {
    GiveRun& r = g_give;
    if (!r.active) return;
    if (r.phase == 1) {
        Sam s;
        const char* stop = Playing(&s);
        for (int k = 0; !stop && k < g_give_per_frame && r.next < r.plan.n; ++k) {
            if (!mem::Q(A(kBagMgrGlobal))) {
                stop = "the baggage manager went away";
                break;
            }
            if (!CallGive(r.plan.codes[r.next], static_cast<std::uint8_t>(g_give_slot))) {
                g_features &= ~kFGive;
                Log("[dev] give: CreateAndAddBaggageToPlayer(%u) faulted; give off for this run", r.plan.codes[r.next]);
                stop = "the game call faulted (give off for this run)";
                break;
            }
            ++r.next, ++r.added;
        }
        if (stop && r.next < r.plan.n) {
            r.skipped += r.plan.n - r.next;
            AddWhy(r.why, sizeof(r.why), "%d not sent: %s", r.plan.n - r.next, stop);
            r.plan.n = r.next;  // only what was sent is checked
        }
        if (r.next < r.plan.n) return;
        Log("[dev] give %s: %d added, %d skipped%s%s%s", r.plan.name, r.added, r.skipped, r.why[0] ? " (" : "", r.why, r.why[0] ? ")" : "");
        if (r.added == 0) {
            Put(g_give_line, sizeof(g_give_line), "%s: nothing given (%s)", r.plan.name, r.why);
            r.active = false;
            return;
        }
        r.phase = 2;
        r.check_at = now_ms + 3000;
        r.give_up_at = now_ms + 20000;
        return;
    }
    if (now_ms < r.check_at) return;
    Sam s;
    const std::uintptr_t mgr = mem::Q(A(kBagMgrGlobal));
    if (Playing(&s) || !mgr) {
        if (now_ms < r.give_up_at) {
            r.check_at = now_ms + 1000;
            return;
        }
        Log("[dev] give %s: %d added; not checked (the world was not played for 20 s)", r.plan.name, r.added);
        Put(g_give_line, sizeof(g_give_line), "%s: %d added (not checked)", r.plan.name, r.added);
        r.active = false;
        return;
    }
    std::vector<std::uint32_t> after;
    ScanBags(mgr, r.li, r.plan.n, &after);
    int arrived = 0;
    bool done[giverules::kMaxPlan] = {};
    for (int i = 0; i < r.plan.n; ++i) {
        if (done[i]) continue;
        int want = 0;
        for (int k = i; k < r.plan.n; ++k)
            if (r.li[k] == r.li[i]) want++, done[k] = true;
        int got = 0;
        std::uint32_t first = 0xFFFFFFFFu;
        for (std::uint32_t idx : after) {
            if (std::find(r.before.begin(), r.before.end(), idx) != r.before.end()) continue;
            std::uint64_t li = 0;
            if (!mem::Rd(mgr + kBagArray + kBagStride * idx + kBagItem, li) || li != r.li[i]) continue;
            if (first == 0xFFFFFFFFu) first = idx;
            ++got;
        }
        const giverules::Entry* e = giverules::Find(r.plan.codes[i]);
        char where[120] = "-";
        if (first != 0xFFFFFFFFu) {
            const std::uintptr_t b = mgr + kBagArray + kBagStride * first;
            std::uint8_t use = 0;
            double p[3] = {};
            std::uint64_t holder = 0;
            mem::Rd(b + kBagUse, use);
            mem::Read(b + kBagPos, p, sizeof(p));
            mem::Rd(b + kBagHolder, holder);
            const double dx = p[0] - s.xf.pos.x, dy = p[1] - s.xf.pos.y, dz = p[2] - s.xf.pos.z;
            Put(where, sizeof(where), "baggage #%u: +0x21 %u, %.1f m from Sam, holder %p", first, use, std::sqrt(dx * dx + dy * dy + dz * dz),
                reinterpret_cast<void*>(holder));
        }
        Log("[dev] give %s:   %s: %d/%d new (%s)", r.plan.name, e ? e->name : "?", (std::min)(got, want), want, where);
        arrived += (std::min)(got, want);
    }
    Log("[dev] give %s: %d/%d arrived (new baggages of those list items after 3 s)", r.plan.name, arrived, r.added);
    Put(g_give_line, sizeof(g_give_line), "%s: %d added, %d/%d arrived%s", r.plan.name, r.added, arrived, r.added, r.skipped ? " (some skipped: see the log)" : "");
    r.active = false;
}

// ---------------------------------------------------------------- dev_goto: a place anywhere in this area, then the ground
// The teleport is the debug menu's own (game::TeleportLocalPlayer: PlaceOn mode 1 at a known z, mode 2 "the topmost ground" for
// z 0). Far places stream in after the teleport: every tick (game thread) devrules::GotoAdvance decides whether Sam stands there
// yet, fell through ground that was not loaded (placed again) or never got there (failed after 25 s). dev_goto, dev_place and
// Teleport to map marker all start here (StartGoto); each keeps its own gate.

// dev_place's landing (round 18): the candidates, and the structure's box checked once the goto settled.
struct PlaceCheck {
    bool on = false;
    int index = -1, type = 0;
    std::int32_t locator = -1;  // the place's DeliveryPointLocatorId (the construction point's id, runs 183622: 590, 2103, 232)
    bool test_inside = false;   // hidden-run test: the first candidate is the structure's own point (the retry path)
    char label[48] = {};
    double place[3] = {};
    float place_rot[9] = {};
    devrules::LandPlan plan;
    int next = 0;      // the candidate placed now is plan.c[next - 1]
    bool cp = false;   // the construction point was found: its transform below
    std::uintptr_t cp_ptr = 0;
    double cp_pos[3] = {};
    float cp_rot[9] = {};
    double centre[3] = {};  // the ring's centre: cp_pos when found, else the place
    bool box = false;  // its config's collision box is usable
    float mn[3] = {}, mx[3] = {};
};
struct GotoRun {
    bool on = false;
    devrules::GotoTrack t;
    bool facing = false;
    double fx = 0, fy = 0;
    char tag[24] = {};
    PlaceCheck pc;
    double start[3] = {};    // where Sam stood when the goto started (the way back)
    bool returning = false;  // Sam is being put back at start; fail[] says why
    char fail[160] = {};
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

bool PlaceSam(const GotoRun& g, char* why, std::size_t cap) {
    const devrules::GotoTrack& t = g.t;
    const bool snap = t.z == 0 || t.snap;
    const game::TeleportMode mode = snap ? game::TeleportMode::snap_to_ground : game::TeleportMode::place;
    const double z = t.z == 0 ? game::kSnapRayCentreZ : t.snap ? t.z + 10.0 : t.z + 0.5;  // mode 2: the ray's centre (+-2000 m)
    if (g.facing && game::TeleportLocalPlayerFacing(t.x, t.y, z, g.fx, g.fy, mode, why, cap)) return true;
    return game::TeleportLocalPlayer(t.x, t.y, z, mode, why, cap);
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

// The one start of every goto: Sam playable, the placement gate, the first placement, the tracker. `face` (may be null): the
// flat direction Sam faces. `tag` names the caller in the log ("goto", "place 3", "marker C0"). `line` gets "goto #n: ...".
bool StartGoto(double x, double y, double z, const double* face, const char* tag, const PlaceCheck* pc, double now_ms, char* line, std::size_t cap) {
    if (!devrules::GotoTargetOk(x, y, z)) return Put(line, cap, "%s: the target (%.2f, %.2f, %.2f) is off the map", tag, x, y, z), RefuseGoto(line);
    Sam s;
    if (const char* why = Playing(&s)) return Put(line, cap, "%s refused: %s", tag, why), RefuseGoto(line);
    char why[160] = {};
    const int gate = game::PlacementGate(why, sizeof(why));
    if (gate != 0) return Put(line, cap, "%s refused (on a vehicle? get off first): %s", tag, why), RefuseGoto(line);
    if (g_goto.on) PublishGoto(GotoState::failed, "goto #%u: replaced by a newer goto", g_goto_serial);
    g_goto = GotoRun{};
    g_goto.t.x = x, g_goto.t.y = y, g_goto.t.z = z;
    g_goto.t.t0 = g_goto.t.last_place = now_ms;
    if (face && (face[0] * face[0] + face[1] * face[1]) > 1e-6) g_goto.facing = true, g_goto.fx = face[0], g_goto.fy = face[1];
    Put(g_goto.tag, sizeof(g_goto.tag), "%s", tag);
    if (pc) g_goto.pc = *pc;
    g_goto.start[0] = s.xf.pos.x, g_goto.start[1] = s.xf.pos.y, g_goto.start[2] = s.xf.pos.z;
    ++g_goto_serial;
    const double dx = x - s.xf.pos.x, dy = y - s.xf.pos.y;
    const double dist = std::sqrt(dx * dx + dy * dy);
    char zs[32];
    if (z == 0) Put(zs, sizeof(zs), "the ground");
    else Put(zs, sizeof(zs), "%.2f", z);
    if (!PlaceSam(g_goto, why, sizeof(why))) {
        PublishGoto(GotoState::failed, "goto #%u: the teleport was refused: %s", g_goto_serial, why);
        Log("[dev] goto #%u (%s): to (%.2f, %.2f, %s) refused: %s", g_goto_serial, tag, x, y, zs, why);
        return Put(line, cap, "goto #%u refused: %s", g_goto_serial, why), false;
    }
    g_goto.on = true;
    Log("[dev] goto #%u (%s): from (%.2f, %.2f, %.2f) to (%.2f, %.2f, %s), %.0f m: %s", g_goto_serial, tag, s.xf.pos.x, s.xf.pos.y, s.xf.pos.z, x, y, zs, dist,
        why);
    PublishGoto(GotoState::moving, "goto #%u: placed, waiting for the ground", g_goto_serial);
    return Put(line, cap, "goto #%u: %.0f m to (%.0f, %.0f, %s); waiting for the ground", g_goto_serial, dist, x, y, zs), true;
}

bool Goto(double x, double y, double z, double now_ms, char* msg, std::size_t cap) {
    if (!Gate(Tool::world, msg, cap)) return RefuseGoto(msg);
    if (!devrules::GotoTargetOk(x, y, z)) return Put(msg, cap, "goto: x y [z] in the map's range (z 0 = the ground under x, y)"), RefuseGoto(msg);
    return StartGoto(x, y, z, nullptr, "goto", nullptr, now_ms, msg, cap);
}

devrules::LandStep PlaceAfterSettle(const double (&pos)[3], double now_ms);  // places, below
bool PlaceNext(double now_ms, char* why, std::size_t cap);
void LogPlaceNext(const char* what, const double (&pos)[3], bool ok, const char* why);

// The goto failed in a way that leaves Sam stuck (fell through or into the ground, inside a structure): put him back where he
// started, through the same tracker (that ground may need to stream in again). `fail`: the failure, kept for the final line.
bool BeginReturn(double now_ms, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(g_goto.fail, sizeof(g_goto.fail), fmt, ap);
    va_end(ap);
    g_goto.pc.on = false;
    g_goto.returning = true;
    g_goto.facing = false;
    g_goto.t = devrules::GotoTrack{};
    g_goto.t.x = g_goto.start[0], g_goto.t.y = g_goto.start[1], g_goto.t.z = g_goto.start[2];
    g_goto.t.t0 = g_goto.t.last_place = now_ms;
    char why[160] = {};
    const bool ok = devrules::GotoTargetOk(g_goto.start[0], g_goto.start[1], g_goto.start[2]) && g_goto.start[2] != 0 && PlaceSam(g_goto, why, sizeof(why));
    Log("[dev] goto #%u: %s: returning Sam to where he started (%.2f, %.2f, %.2f): %s", g_goto_serial, g_goto.fail, g_goto.start[0], g_goto.start[1],
        g_goto.start[2], ok ? why : why[0] ? why : "refused");
    if (!ok) {
        g_goto.on = false;
        PublishGoto(GotoState::failed, "goto #%u: %s; could NOT return Sam to his start", g_goto_serial, g_goto.fail);
        return false;
    }
    PublishGoto(GotoState::moving, "goto #%u: %s; returning Sam to his start", g_goto_serial, g_goto.fail);
    return true;
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
    PlaceCheck& pc = g_goto.pc;
    if (step == devrules::GotoStep::place_again) {
        // dev_place: a second fall at the same candidate (inside a hill, over a drop) moves on to the next one
        if (pc.on && devrules::LandAfterFall(g_goto.t.placements, false, pc.next, pc.plan.n) == devrules::LandStep::next) {
            char what[96];
            Put(what, sizeof(what), "fell again (z %.2f, %.1f m below it)", pos[2], g_goto.t.z - pos[2]);
            const bool ok = PlaceNext(now_ms, why, sizeof(why));
            LogPlaceNext(what, pos, ok, why);
            return;
        }
        const bool ok = PlaceSam(g_goto, why, sizeof(why));
        Log("[dev] goto #%u: Sam at z %.2f, %.1f m below the target after %.1f s (the ground was not in yet): placed again (#%d): %s", g_goto_serial, pos[2],
            g_goto.t.z - pos[2], secs, g_goto.t.placements, ok ? why : "refused");
    } else if (step == devrules::GotoStep::settled) {
        Log("[dev] goto #%u: settled at (%.2f, %.2f, %.2f) after %.1f s (%d placement(s))%s", g_goto_serial, pos[0], pos[1], pos[2], secs, g_goto.t.placements,
            g_goto.returning ? ", back where he started" : "");
        if (g_goto.returning) {
            g_goto.on = false;
            PublishGoto(GotoState::failed, "goto #%u: %s; Sam is back where he started", g_goto_serial, g_goto.fail);
            return;
        }
        if (pc.on) {
            const devrules::LandStep ls = PlaceAfterSettle(pos, now_ms);
            if (ls == devrules::LandStep::next) return;  // placed at the next candidate: the goto goes on
            if (ls == devrules::LandStep::give_up) {
                BeginReturn(now_ms, "Sam is inside a structure's box beside %s and no landing is left", pc.label);
                return;
            }
        }
        g_goto.on = false;
        PublishGoto(GotoState::settled, "goto #%u: settled at (%.2f, %.2f, %.2f) after %.1f s", g_goto_serial, pos[0], pos[1], pos[2], secs);
    } else if (step == devrules::GotoStep::timeout) {
        const bool fell = s.e && devrules::GotoFell(g_goto.t, pos);
        Log("[dev] goto #%u: NOT settled after %.1f s: Sam at (%.2f, %.2f, %.2f)%s%s%s", g_goto_serial, secs, pos[0], pos[1], pos[2], fell ? ", below the target" : "",
            reason[0] ? "; " : "", reason);
        if (g_goto.returning) {
            g_goto.on = false;
            PublishGoto(GotoState::failed, "goto #%u: %s; returning Sam to his start did NOT settle either", g_goto_serial, g_goto.fail);
            return;
        }
        if (fell && pc.on && devrules::LandAfterFall(g_goto.t.placements, true, pc.next, pc.plan.n) == devrules::LandStep::next) {
            const bool ok = PlaceNext(now_ms, why, sizeof(why));
            LogPlaceNext("NOT settled, below the target", pos, ok, why);
            return;
        }
        // Fell through or into the ground: he would stay there. Walking off or a vehicle (not still) are left alone.
        if (fell && playable) {
            BeginReturn(now_ms, "NOT settled after %.0f s, Sam %.0f m below the target", secs, g_goto.t.z - pos[2]);
            return;
        }
        g_goto.on = false;
        PublishGoto(GotoState::failed, "goto #%u: NOT settled after %.0f s (Sam at %.0f, %.0f, %.0f%s%s)", g_goto_serial, secs, pos[0], pos[1], pos[2],
                    reason[0] ? "; " : "", reason);
    }
}

// ---------------------------------------------------------------- places
struct PlaceFull {
    Place p;
    int area = 0, type = 0;
    std::uint32_t order = 0;
    std::int32_t locator = -1;  // DeliveryPointLocatorId (+0x70)
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
        // The qword after the pointer is the text's length (0xE after the pointer to "Lake Knot City", 14 letters): the text
        // is not always zero-terminated, a 46-letter name read "...Mountain Knot CityL" (run 20261124-183005-place-teleport).
        const std::uint64_t len = off + 8 < 0x38 ? mem::Q(lt + off + 8) : 0;
        if (len > 0 && len < sizeof(s)) s[len] = 0;
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
        std::int32_t locator = -1;
        double pos[3] = {};
        float rot[9] = {};
        if (!mem::Rd(r + kDpArea, a) || !mem::Rd(r + kDpType, type) || !mem::Rd(r + kDpOrder, order) || !mem::Read(r + kDpXf, pos, sizeof(pos)) ||
            !mem::Read(r + kDpXf + 0x18, rot, sizeof(rot)))
            continue;
        if (a != area || !std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2]) || (pos[0] == 0 && pos[1] == 0)) continue;
        mem::Rd(r + kDpLocatorId, locator);
        PlaceFull& f = g_places[g_place_count++];
        f.area = a, f.type = type, f.order = order, f.locator = locator;
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
        Log("[dev] place %d: %s (%s, type %d, order %u, area %d, locator id %d) at (%.2f, %.2f, %.2f), %.0f m", i, f.p.label, devrules::PlaceKind(f.type), f.type,
            f.order, f.area, f.locator, f.p.pos[0], f.p.pos[1], f.p.pos[2], f.p.dist_m);
    }
    Put(msg, cap, "%d places in area %d (logged)", g_place_count, g_places_area);
    return g_place_count > 0;
}

// The place's construction point (DSConstructionPointManager's live list, read on the game thread like world_share.cpp does):
// the one whose id is the place's DeliveryPointLocatorId (the same kind of structure, within kCpIdMatchM: runs 183622 showed
// locator id == construction point id for 590, 2103 and 232), else the nearest of the same kind within kCpMatchM (flat). Its
// transform (cp+0x468, what the game's GetConstructionPointFrontAccessTransform uses), its config's TerminalFrontAccessOffset
// and collision box.
constexpr double kCpMatchM = 3.0, kCpIdMatchM = 10.0;
struct CpInfo {
    std::uintptr_t cp = 0;
    std::int32_t id = -1;
    bool by_id = false;  // matched by the locator id
    double pos40[3] = {}, pos[3] = {};
    float rot[9] = {};
    bool cfg = false;  // a ...ConstructionPointConfig (RTTI name)
    char cfg_name[48] = {};
    float front[3] = {};
    float mn[3] = {}, mx[3] = {};
};
// The live list's pointers (a copy: none kept past the caller).
bool CpLive(std::vector<std::uintptr_t>* out) {
    if (!(g_features & kFStand)) return false;
    const std::uintptr_t mgr = mem::Q(A(kCpMgrGlobal));
    std::int32_t n = 0;
    std::uintptr_t data = 0;
    if (!mgr || !mem::Rd(mgr + kCpLiveCount, n) || !mem::Rd(mgr + kCpLiveData, data) || n <= 0 || n > 8192 || !data) return false;
    out->resize(static_cast<std::size_t>(n));
    return mem::Read(data, out->data(), out->size() * 8);
}
// cp's transform and its config (front access offset, collision box). False when the transform is unreadable.
bool ReadCp(std::uintptr_t cp, CpInfo* c) {
    c->cp = cp;
    mem::Rd(cp + kCpIdOff, c->id);
    mem::Read(cp + kCpPos40, c->pos40, sizeof(c->pos40));
    if (!mem::Read(cp + kCpXf, c->pos, sizeof(c->pos)) || !mem::Read(cp + kCpXf + 0x18, c->rot, sizeof(c->rot))) return false;
    const std::uintptr_t cfg = mem::Q(cp + kCpCfgOff);
    const std::uintptr_t rtti = cfg ? mem::RttiOf(cfg) : 0;
    const std::uintptr_t np = rtti ? mem::Q(rtti + 0x40) : 0;
    if (np && mem::Read(np, c->cfg_name, sizeof(c->cfg_name) - 1) && std::strstr(c->cfg_name, "ConstructionPointConfig"))
        c->cfg = mem::Read(cfg + kCfgFront, c->front, sizeof(c->front)) && mem::Read(cfg + kCfgBoxMin, c->mn, sizeof(c->mn)) &&
                 mem::Read(cfg + kCfgBoxMax, c->mx, sizeof(c->mx));
    return true;
}
bool FindCp(int type, std::int32_t locator, const double (&place)[3], CpInfo* out) {
    std::vector<std::uintptr_t> ptrs;
    if (!CpLive(&ptrs)) return false;
    double best = kCpMatchM * kCpMatchM;
    std::uintptr_t found = 0, by_id = 0;
    for (std::uintptr_t cp : ptrs) {
        std::uint8_t t = 0;
        double p[3] = {};
        if (!cp || !mem::Rd(cp + kCpTypeOff, t) || devrules::PlaceGroup(t) != devrules::PlaceGroup(type) || !mem::Read(cp + kCpXf, p, sizeof(p))) continue;
        const double dx = p[0] - place[0], dy = p[1] - place[1], d = dx * dx + dy * dy;
        std::int32_t id = -1;
        if (locator >= 0 && d <= kCpIdMatchM * kCpIdMatchM && mem::Rd(cp + kCpIdOff, id) && id == locator) {
            by_id = cp;
            break;
        }
        if (d < best) best = d, found = cp;
    }
    if (by_id) found = by_id;
    if (!found) return false;
    out->by_id = by_id != 0;
    return ReadCp(found, out);
}

// Whether Sam (feet at pos) is inside the collision box of a small structure near him other than `own` (post boxes, MULE posts
// and the place's own kind, within kNeighbourM flat): a ring point beside one structure may sit in the next one (a second post
// box, a player-built one by a terminal). Bigger structures (shelters, bridges, terminals) are not checked: Sam may stand
// inside their boxes. `what` names the one he is in.
constexpr double kNeighbourM = 10.0;
bool InsideNeighbour(const double (&pos)[3], std::uintptr_t own, int type, char* what, std::size_t cap) {
    std::vector<std::uintptr_t> ptrs;
    if (!CpLive(&ptrs)) return false;
    for (std::uintptr_t cp : ptrs) {
        std::uint8_t t = 0;
        double p[3] = {};
        if (!cp || cp == own || !mem::Rd(cp + kCpTypeOff, t)) continue;
        if (!devrules::SmallPost(t) && devrules::PlaceGroup(t) != devrules::PlaceGroup(type)) continue;
        if (!mem::Read(cp + kCpXf, p, sizeof(p))) continue;
        const double dx = p[0] - pos[0], dy = p[1] - pos[1];
        if (dx * dx + dy * dy > kNeighbourM * kNeighbourM) continue;
        CpInfo c;
        if (!ReadCp(cp, &c) || !c.cfg || !devrules::BoxUsable(c.mn, c.mx)) continue;
        double local[3] = {};
        if (!devrules::InsideBox(pos, c.pos, c.rot, c.mn, c.mx, local)) continue;
        Put(what, cap, "%s id %d (type %u, local %.2f, %.2f, %.2f)", c.cfg_name, c.id, t, local[0], local[1], local[2]);
        return true;
    }
    return false;
}

// The landing plan of pc (pc.place / place_rot / type / locator set): the structure found now, the stand point, the ring
// around the structure's own position. Logged.
void BuildPlan(PlaceCheck& pc) {
    CpInfo ci;
    pc.cp = FindCp(pc.type, pc.locator, pc.place, &ci);
    double stand[3] = {};
    bool stand_ok = false;
    pc.box = false;
    std::memcpy(pc.centre, pc.place, sizeof(pc.centre));
    if (pc.cp) {
        pc.cp_ptr = ci.cp;
        std::memcpy(pc.cp_pos, ci.pos, sizeof(pc.cp_pos));
        std::memcpy(pc.cp_rot, ci.rot, sizeof(pc.cp_rot));
        std::memcpy(pc.centre, ci.pos, sizeof(pc.centre));
        pc.box = ci.cfg && devrules::BoxUsable(ci.mn, ci.mx);
        if (pc.box) std::memcpy(pc.mn, ci.mn, sizeof(pc.mn)), std::memcpy(pc.mx, ci.mx, sizeof(pc.mx));
        if (ci.cfg) {
            devrules::FrontAccess(ci.pos, ci.rot, ci.front, stand);
            stand_ok = devrules::StandPointOk(pc.centre, stand);
        }
    }
    const double radius = devrules::FallbackRadius(pc.type, pc.box, pc.mn, pc.mx);
    pc.plan = devrules::PlanLanding(pc.centre, pc.cp ? pc.cp_rot : pc.place_rot, stand_ok, stand, radius);
    if (pc.test_inside) {  // hidden-run test: the structure's own point first (inside its box: the retry path)
        devrules::LandPlan& p = pc.plan;
        for (int k = (std::min)(p.n, devrules::kMaxLandings - 1); k > 0; --k) p.c[k] = p.c[k - 1];
        p.c[0] = devrules::Landing{};
        std::memcpy(p.c[0].pos, pc.centre, sizeof(p.c[0].pos));
        p.c[0].rule = "test: the structure's own point";
        p.n = (std::min)(p.n + 1, devrules::kMaxLandings);
    }
    pc.next = 0;
    if (!pc.cp) {
        Log("[dev] place %d (%s): no %s construction point (locator id %d, or within %.0f m)%s; the ring at %.2f m around the place", pc.index, pc.label,
            devrules::PlaceKind(pc.type), pc.locator, kCpMatchM, (g_features & kFStand) ? "" : " (the stand point's byte checks failed)", radius);
        return;
    }
    const double ax = ci.pos40[0] - ci.pos[0], ay = ci.pos40[1] - ci.pos[1], az = ci.pos40[2] - ci.pos[2];
    const double px = ci.pos[0] - pc.place[0], py = ci.pos[1] - pc.place[1];
    const double sx = stand[0] - pc.centre[0], sy = stand[1] - pc.centre[1];
    Log("[dev] place %d (%s): construction point id %d (%s) at +0x468 (%.2f, %.2f, %.2f), %.2f m from the place (+0x40 %.2f m apart), forward (%.2f, %.2f); "
        "config %s: TerminalFrontAccessOffset (%.2f, %.2f, %.2f), box (%.2f, %.2f, %.2f)..(%.2f, %.2f, %.2f) %s; stand point (%.2f, %.2f, %.2f) %.2f m: %s; "
        "ring %.2f m",
        pc.index, pc.label, ci.id, ci.by_id ? "the locator id" : "the nearest", ci.pos[0], ci.pos[1], ci.pos[2], std::sqrt(px * px + py * py),
        std::sqrt(ax * ax + ay * ay + az * az), ci.rot[3], ci.rot[4], ci.cfg ? ci.cfg_name : "(not a ConstructionPointConfig)", ci.front[0], ci.front[1],
        ci.front[2], ci.mn[0], ci.mn[1], ci.mn[2], ci.mx[0], ci.mx[1], ci.mx[2], pc.box ? "usable" : "not used", stand[0], stand[1], stand[2],
        std::sqrt(sx * sx + sy * sy), stand_ok ? "used" : "not used", radius);
}

// Sam at plan candidate pc.next (facing the structure); the tracker restarts there with the same goto serial.
bool PlaceNext(double now_ms, char* why, std::size_t cap) {
    PlaceCheck& pc = g_goto.pc;
    const devrules::Landing& c = pc.plan.c[pc.next++];
    g_goto.t = devrules::GotoTrack{};
    g_goto.t.x = c.pos[0], g_goto.t.y = c.pos[1], g_goto.t.z = c.pos[2];
    g_goto.t.snap = c.snap;
    g_goto.t.t0 = g_goto.t.last_place = now_ms;
    g_goto.fx = pc.centre[0] - c.pos[0], g_goto.fy = pc.centre[1] - c.pos[1];
    g_goto.facing = g_goto.fx * g_goto.fx + g_goto.fy * g_goto.fy > 1e-6;
    return PlaceSam(g_goto, why, cap);
}
// The line after PlaceNext: `what` happened at the previous candidate, the new one.
void LogPlaceNext(const char* what, const double (&pos)[3], bool ok, const char* why) {
    const PlaceCheck& pc = g_goto.pc;
    const devrules::Landing& now = pc.plan.c[pc.next - 1];
    const devrules::Landing& before = pc.plan.c[pc.next >= 2 ? pc.next - 2 : 0];
    Log("[dev] place %d: Sam %s after %s at (%.2f, %.2f, %.2f): trying %s at (%.2f, %.2f, %.2f): %s", pc.index, what, before.rule, pos[0], pos[1], pos[2],
        now.rule, now.pos[0], now.pos[1], now.pos[2], ok ? why : why[0] ? why : "refused");
}

// dev_place after its goto settled: Sam outside the structure's box and every small neighbour's -> stay (done); inside one ->
// the next candidate, placed now (next: the goto goes on); none left -> give_up (GotoTick puts Sam back where he started). A
// structure that streamed in after the teleport is looked up again (its plan then starts over).
devrules::LandStep PlaceAfterSettle(const double (&pos)[3], double now_ms) {
    PlaceCheck& pc = g_goto.pc;
    if (!pc.cp) {
        BuildPlan(pc);  // next = 0: the new plan's first candidate is the one to try if he is inside
        if (pc.cp) Log("[dev] place %d: the construction point streamed in after the teleport", pc.index);
        else pc.next = 1;
    }
    const devrules::Landing& used = pc.plan.c[pc.next > 0 ? pc.next - 1 : 0];
    const double dx = pos[0] - pc.centre[0], dy = pos[1] - pc.centre[1];
    const double d = std::sqrt(dx * dx + dy * dy);
    double local[3] = {};
    char in[120] = {};
    bool inside = false;
    if (pc.cp && pc.box && devrules::InsideBox(pos, pc.cp_pos, pc.cp_rot, pc.mn, pc.mx, local)) {
        inside = true;
        Put(in, sizeof(in), "%s's box (local %.2f, %.2f, %.2f)", pc.label, local[0], local[1], local[2]);
    } else if (InsideNeighbour(pos, pc.cp_ptr, pc.type, in, sizeof(in))) {
        inside = true;
    }
    const devrules::LandStep step = devrules::LandAfterSettle(inside, pc.next, pc.plan.n);
    if (step == devrules::LandStep::stay) {
        if (!pc.cp || !pc.box)
            Log("[dev] place %d settled: Sam %.1f m from %s (%s; no collision box of its own to check, none of a small neighbour's)", pc.index, d, pc.label,
                used.rule);
        else
            Log("[dev] place %d settled: Sam %.1f m from %s, outside its box (local %.2f, %.2f, %.2f; %s) and any small neighbour's", pc.index, d, pc.label,
                local[0], local[1], local[2], used.rule);
        return step;
    }
    if (step == devrules::LandStep::give_up) {
        Log("[dev] place %d: Sam is INSIDE %s after %s and no candidate is left", pc.index, in, used.rule);
        return step;
    }
    char why[160] = {}, what[160];
    Put(what, sizeof(what), "in %s", in);
    const bool ok = PlaceNext(now_ms, why, sizeof(why));
    LogPlaceNext(what, pos, ok, why);
    return step;
}

// dev_place <n> [kind] [mode]: Sam beside the n-th nearest place (of that kind: 0 any, -1 delivery base, 5 post box, 20 MULE
// post ...), at the structure's stand point or the ring around it, facing it; through the goto tracker (far places stream in).
// Hidden runs on their sandboxed save only: mode 1 the place's own point, the old landing (inside a post box: the test's
// control); mode 2 the structure's own point as the first candidate (the box check then moves him to the next: the retry path).
bool GoPlace(int index, int filter, int mode, double now_ms, char* msg, std::size_t cap) {
    if (!Gate(Tool::player, msg, cap) || !Has(kFPlaces, msg, cap)) return RefuseGoto(msg);
    if (mode != 0 && (!automation::Enabled() || RealSaveRefusal()))
        return Put(msg, cap, "place mode %d: hidden runs on a sandboxed save only", mode), RefuseGoto(msg);
    if (mode < 0 || mode > 2) return Put(msg, cap, "place <n> [kind] [mode 0 beside it, 1 its own point, 2 test the retry]"), RefuseGoto(msg);
    bool raw = mode == 1;
    Sam s;
    if (const char* why = Playing(&s)) return Put(msg, cap, "teleport refused: %s", why), RefuseGoto(msg);
    ReadPlaces(&s);
    int pick = -1, k = 0;
    for (int i = 0; i < g_place_count && pick < 0; ++i)
        if (devrules::PlaceFilterMatches(filter, g_places[i].type) && k++ == index) pick = i;
    if (pick < 0) {
        if (filter) Put(msg, cap, "no place #%d of kind %d (%d of that kind, %d in this area)", index, filter, k, g_place_count);
        else Put(msg, cap, "no place #%d (%d in this area)", index, g_place_count);
        return RefuseGoto(msg);
    }
    const PlaceFull& f = g_places[pick];
    const bool own_point = mode == 0 && devrules::LandsOnOwnPoint(f.type);  // bases, shelters, safe houses: the terminal itself
    raw = raw || own_point;
    PlaceCheck pc;
    pc.on = !raw;
    pc.index = pick, pc.type = f.type, pc.locator = f.locator;
    pc.test_inside = mode == 2;
    Put(pc.label, sizeof(pc.label), "%s", f.p.label);
    std::memcpy(pc.place, f.p.pos, sizeof(pc.place));
    std::memcpy(pc.place_rot, f.rot, sizeof(pc.place_rot));
    char tag[24], line[200] = {};
    Put(tag, sizeof(tag), "place %d", pick);
    if (raw) {
        const double face[2] = {f.rot[3], f.rot[4]};
        const bool ok = StartGoto(f.p.pos[0], f.p.pos[1], f.p.pos[2], face, tag, nullptr, now_ms, line, sizeof(line));
        Log("[dev] place %d (%s, %s, type %d): %s, the place's own point (%.2f, %.2f, %.2f): %s", pick, f.p.label, devrules::PlaceKind(f.type), f.type,
            own_point ? "a base / shelter" : "raw", f.p.pos[0], f.p.pos[1], f.p.pos[2], line);
        return Put(msg, cap, own_point ? "to %s (its terminal): %s" : "to %s (its own point, raw): %s", f.p.label, line), ok;
    }
    BuildPlan(pc);
    pc.next = 1;
    const devrules::Landing& c = pc.plan.c[0];  // never a snap candidate (the ring's far one is last)
    const double face[2] = {pc.centre[0] - c.pos[0], pc.centre[1] - c.pos[1]};
    const double d = std::sqrt(face[0] * face[0] + face[1] * face[1]);
    const bool ok = StartGoto(c.pos[0], c.pos[1], c.pos[2], face, tag, &pc, now_ms, line, sizeof(line));
    Log("[dev] place %d (%s, %s, type %d, locator id %d): %s at (%.2f, %.2f, %.2f), %.1f m from the structure: %s", pick, f.p.label,
        devrules::PlaceKind(f.type), f.type, f.locator, c.rule, c.pos[0], c.pos[1], c.pos[2], d, line);
    return Put(msg, cap, ok ? "to %s (%s, %.1f m from it): %s" : "teleport to %s (%s, %.1f m from it) failed: %s", f.p.label, c.rule, d, line), ok;
}

// ---------------------------------------------------------------- map markers (Teleport to map marker; dev_marker)
// The player's own map markers (the map screen's route points, research round 18, docs/debug-actions.md "Teleport to map marker").
// Marker manager global (0x64B0 bytes): +0x5EF8 the user-marker pool (50 x 0x50, slot = letter * 10 + number), +0x5F00 the
// next id. Entry: +0x10 float x, y, z, w (Sam's frame, z up); +0x20 u64 id (placement order); +0x2C bit 0 active; +0x2D type
// (6 = UserMarker); +0x41 / +0x42 ground snap pending (raycast / heightmap); +0x43 reached; +0x4C letter (route A..E); +0x4D number (0..9). +0x08 is
// uninitialised (never read). The area (+0x2E) is 0xFFFF in memory: the mod tags each id with the area it first saw it in.
constexpr std::uintptr_t kMkGlobal = 0x147BFD940;
constexpr std::uintptr_t kMkPool = 0x5EF8;
constexpr int kMkSlots = 50;
constexpr std::uintptr_t kMkEntry = 0x50;
constexpr std::uintptr_t kMkAdd = 0x142F22C90;       // (mgr, const float pos[4], letter, number, u8 snap, u8 reached)
constexpr std::uintptr_t kMkNewRoute = 0x142F29320;  // (mgr) -> the letter for a new route (evicts the oldest one-marker route)
constexpr std::uintptr_t kMkNextNum = 0x142F29430;   // (mgr, letter) -> the next number in that route (-1 full / bad letter)
enum MkFeature : unsigned { kMkRead = 1u, kMkWrite = 2u };
constexpr Site kMkSites[] = {
    {0x142F40090, "48 81 EC E8 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 D0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 85 C0 75 1B", "marker manager accessor",
     kMkRead | kMkWrite},
    {0x142F22D00, "48 89 5C 24 08 48 89 74 24 10 57 41 56 41 57 48 83 EC 20 43 8D 04 80 48 8B F1 48 8B 89 F8 5E 00 00 41 8D 04 41 48 98",
     "user marker add body (+0x5EF8, letter*10+number)", kMkRead | kMkWrite},
    {0x142F22DBE, "48 8B 86 00 5F 00 00 80 4B 2C 01 48 89 43 20 48 FF 86 00 5F 00 00", "user marker id (+0x20 from +0x5F00, +0x2C active)", kMkRead | kMkWrite},
    {0x142F22E02, "C5 F9 7F 43 10 88 43 43 88 4B 41 88 4B 42 44 88 73 4C 44 88 7B 4D", "user marker fields (+0x10 +0x41 +0x42 +0x43 +0x4C +0x4D)", kMkRead | kMkWrite},
    {0x142F36783, "44 0F B6 43 2C 41 80 E0 01 74 0F 80 7B 2D 06 75 09 48 8B 07 48 39 43 20", "user marker type (+0x2C active, +0x2D == 6, +0x20 id)", kMkRead},
    {kCurArea, "48 83 EC 28 48 8B 05 ?? ?? ?? ?? 48 85 C0 74 12 80 B8 A8 00 00 00 00 74 09 0F B7 40 48", "current area", kMkRead | kMkWrite},
    {kMkAdd, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 48 8B E9 41 8B D9 48 8B 0D ?? ?? ?? ?? 41 8B F8 48 8B F2 E8 ?? ?? ?? ?? 66 89 44 24 30 44 8B CB 0F B6 44 24 78 44 8B C7 88 44 24 28 48 8B D6 0F B6 44 24 70 48 8B CD 88 44 24 20 E8 1C 00 00 00",
     "user marker add (area, then the body)", kMkWrite},
    {kMkNewRoute, "40 55 57 41 56 48 83 EC 20 45 33 F6 4C 89 7C 24 50 4C 8B B9 F8 5E 00 00", "user marker new route", kMkWrite},
    {kMkNextNum, "44 8B CA 83 FA 04 77 7C 4C 8B 99 F8 5E 00 00 45 33 C0 49 8D 4B 4C F6 41 E0 01", "user marker next number", kMkWrite},
};
unsigned g_mk_features = 0;
bool g_mk_checked = false;
char g_mk_off[96] = {};
struct MkTag {
    bool set = false;
    std::uint64_t id = 0;
    double x = 0, y = 0;
    int area = -1;
};
MkTag g_mk_tags[kMkSlots];  // per slot: the marker the mod saw there (id, place) and the area it was in then
SRWLOCK g_mk_lock = SRWLOCK_INIT;
MarkerView g_mk_view;  // published (g_mk_lock)
double g_mk_next = 0;
// The overlay wants the view (GetMarkerView, any thread) until this GetTickCount64() time: the 2 Hz view runs only while the menu
// shows the button; without it a light pass every kMkTagMs keeps the area tags of new markers.
volatile LONG64 g_mk_wanted_until = 0;
bool g_mk_view_live = false;
constexpr double kMkViewMs = 500, kMkTagMs = 5000;

void MkCheck() {
    if (g_mk_checked) return;
    g_mk_checked = true;
    unsigned f = kMkRead | kMkWrite;
    int ok = 0, total = 0;
    auto fail = [&](unsigned bits, const char* what) {
        Log("[dev] map markers: byte check FAILED: %s", what);
        if (!g_mk_off[0]) Put(g_mk_off, sizeof(g_mk_off), "%s", what);
        f &= ~bits;
    };
    for (const Site& s : kMkSites) {
        ++total;
        if (mem::CodeMatches(A(s.va), s.sig)) ++ok;
        else fail(s.features, s.name);
    }
    ++total;
    if (mem::RipTarget(A(0x142F400A9), 3, 7) == A(kMkGlobal)) ++ok;
    else fail(kMkRead | kMkWrite, "the marker manager global");
    ++total;
    if (mem::RipTarget(A(0x142F22CB7), 1, 5) == A(kCurArea)) ++ok;
    else fail(kMkWrite, "the add's current-area call");
    g_mk_features = f;
    Log("[dev] map markers: %d/%d byte checks ok; teleport %s, test add %s", ok, total, (f & kMkRead) ? "on" : "OFF", (f & kMkWrite) ? "on" : "OFF");
}

bool CallMkAdd(std::uintptr_t mgr, const float* pos, int letter, int number, std::uint8_t snap, std::uint8_t reached) noexcept {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, const float*, int, int, std::uint8_t, std::uint8_t)>(A(kMkAdd))(mgr, pos, letter, number, snap, reached);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
int CallMkNewRoute(std::uintptr_t mgr) noexcept {
    __try {
        return reinterpret_cast<int (*)(std::uintptr_t)>(A(kMkNewRoute))(mgr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}
int CallMkNextNum(std::uintptr_t mgr, int letter) noexcept {
    __try {
        return reinterpret_cast<int (*)(std::uintptr_t, int)>(A(kMkNextNum))(mgr, letter);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

// The active user markers (reads only; game thread). `area` >= 0: new ids are tagged with it (the area they were first seen in).
int MkRead(devrules::Marker* out, int area) {
    const std::uintptr_t mgr = mem::Q(A(kMkGlobal));
    const std::uintptr_t pool = mgr ? mem::Q(mgr + kMkPool) : 0;
    static std::uint8_t buf[kMkSlots * kMkEntry];
    if (!pool || !mem::Read(pool, buf, sizeof(buf))) return -1;
    int n = 0;
    for (int i = 0; i < kMkSlots; ++i) {
        const std::uint8_t* b = buf + kMkEntry * static_cast<unsigned>(i);
        if (!(b[0x2C] & 1) || b[0x2D] != 6 || !devrules::MarkerSlotOk(b[0x4C], b[0x4D])) continue;
        float p[3] = {};
        std::uint64_t id = 0;
        std::memcpy(p, b + 0x10, sizeof(p));
        std::memcpy(&id, b + 0x20, sizeof(id));
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) continue;
        devrules::Marker& m = out[n++];
        m.slot = i, m.letter = b[0x4C], m.number = b[0x4D], m.id = id;
        m.pos[0] = p[0], m.pos[1] = p[1], m.pos[2] = p[2];
        m.pending = (b[0x41] ? 1 : 0) | (b[0x42] ? 2 : 0);
        m.snapped = m.pending == 0;
        MkTag& t = g_mk_tags[i];
        if (!t.set || !devrules::SameMarker(t.id, t.x, t.y, id, p[0], p[1])) t.set = true, t.id = id, t.x = p[0], t.y = p[1], t.area = area;
        else if (t.area < 0) t.area = area;
        m.area = t.area;
    }
    return n;
}

void PublishMarkers(const MarkerView& in) {
    MarkerView v = in;
    AcquireSRWLockExclusive(&g_mk_lock);
    v.serial = g_mk_view.serial + 1;
    g_mk_view = v;
    ReleaseSRWLockExclusive(&g_mk_lock);
}

void MarkerTick(double now_ms) {
    const bool wanted = static_cast<LONG64>(GetTickCount64()) < InterlockedCompareExchange64(&g_mk_wanted_until, 0, 0);
    if (!wanted && g_mk_view_live) {  // the menu closed: the view goes stale; the next open shows "reading" until the first pass
        g_mk_view_live = false;
        MarkerView v;
        v.checked = g_mk_checked;
        v.ok = (g_mk_features & kMkRead) != 0;
        Put(v.off, sizeof(v.off), "%s", g_mk_off);
        Put(v.block, sizeof(v.block), "reading the map markers");
        PublishMarkers(v);
    }
    if (wanted && !g_mk_view_live) g_mk_next = 0;
    if (now_ms < g_mk_next) return;
    g_mk_next = now_ms + (wanted ? kMkViewMs : kMkTagMs);
    MarkerView v;
    Sam s;
    const char* block = Playing(&s);
    if (!block) MkCheck();
    v.checked = g_mk_checked;
    v.ok = (g_mk_features & kMkRead) != 0;
    Put(v.off, sizeof(v.off), "%s", g_mk_off);
    if (block) Put(v.block, sizeof(v.block), "%s", block);
    if (!block && v.ok) {
        devrules::Marker m[kMkSlots];
        int n = MkRead(m, -1);
        bool untagged = false;
        for (int i = 0; i < n; ++i) untagged |= m[i].area < 0;
        // GetCurrentArea only while the world is played (the load-hang rule in Tick), and without the menu only for a new marker
        if (n > 0 && (wanted || untagged)) v.area = devrules::KnownArea(CallArea());
        if (untagged && v.area >= 0) n = MkRead(m, v.area);
        if (!wanted) return;  // the light pass: the tags only
        v.newest = devrules::PickMarker(m, n > 0 ? n : 0, -1, v.area);
        for (int i = 0; i < n && v.count < MarkerView::kMax; ++i) {
            MarkerView::Item& it = v.items[v.count++];
            devrules::MarkerName(m[i].letter, m[i].number, it.name);
            it.id = m[i].id;
            std::memcpy(it.pos, m[i].pos, sizeof(it.pos));
            it.snapped = m[i].snapped;
            it.area = m[i].area;
            it.here = devrules::MarkerAreaOk(m[i].area, v.area);
            const double dx = m[i].pos[0] - s.xf.pos.x, dy = m[i].pos[1] - s.xf.pos.y;
            it.dist_m = static_cast<float>(std::sqrt(dx * dx + dy * dy));
        }
    }
    if (!wanted) return;
    g_mk_view_live = true;
    PublishMarkers(v);
}

// Teleport to map marker (the Player tab's teleport class: any session, no dev tools needed; the local Sam only). want < 0: the
// newest marker on this map (devrules::MarkerWant: the action's v0 is the id + 1). The position is copied first: the game
// removes a one-marker route once Sam reaches it.
bool MarkerGo(std::int64_t want, double now_ms, char* msg, std::size_t cap) {
    Sam s;
    if (const char* why = Playing(&s)) return Put(msg, cap, "teleport to map marker refused: %s", why), RefuseGoto(msg);
    MkCheck();
    if (!(g_mk_features & kMkRead))
        return Put(msg, cap, "teleport to map marker is off: this game build differs (%s)", g_mk_off[0] ? g_mk_off : "a byte check"), RefuseGoto(msg);
    devrules::Marker m[kMkSlots];
    const int area = devrules::KnownArea(CallArea());
    const int n = MkRead(m, area);
    if (n < 0) return Put(msg, cap, "no map markers (the marker manager is not there)"), RefuseGoto(msg);
    const int i = devrules::PickMarker(m, n, want, area);
    if (i < 0) {
        if (want >= 0) Put(msg, cap, "that map marker is gone (%d left)", n);
        else Put(msg, cap, "no map marker: place one on the map screen first");
        return RefuseGoto(msg);
    }
    const devrules::Marker mk = m[i];  // a copy: the game may remove it once Sam is there
    char name[4];
    devrules::MarkerName(mk.letter, mk.number, name);
    if (!devrules::MarkerAreaOk(mk.area, area)) {
        char there[64], here[64];
        devrules::AreaText(mk.area, mk.area >= 0 ? game::AreaName(static_cast<std::uint16_t>(mk.area)) : nullptr, there, sizeof(there));
        devrules::AreaText(area, area >= 0 ? game::AreaName(static_cast<std::uint16_t>(area)) : nullptr, here, sizeof(here));
        return Put(msg, cap, "map marker %s is on another map (%s); you are in %s", name, there, here), RefuseGoto(msg);
    }
    const double z = devrules::MarkerTargetZ(mk);
    const double face[2] = {mk.pos[0] - s.xf.pos.x, mk.pos[1] - s.xf.pos.y};
    char tag[24], line[200] = {};
    Put(tag, sizeof(tag), "marker %s", name);
    const bool ok = StartGoto(mk.pos[0], mk.pos[1], z, face, tag, nullptr, now_ms, line, sizeof(line));
    Log("[dev] marker %s (id %llu, %s of %d, area %d): at (%.2f, %.2f, %.2f), %s: %s", name, static_cast<unsigned long long>(mk.id),
        want >= 0 ? "picked" : "the newest", n, mk.area, mk.pos[0], mk.pos[1], mk.pos[2],
        z == 0 ? "the terrain height not in yet: the topmost ground" : "its terrain height (placed again if Sam falls)", line);
    return Put(msg, cap, ok ? "to map marker %s: %s" : "teleport to map marker %s failed: %s", name, line), ok;
}

// dev_marker: 0 log the map's user markers; 1 <m east> <m north>: add one there through the game's own add, as the map screen
// does (hidden runs only: it changes the seeded save's markers).
bool MarkerTest(int op, double east, double north, char* msg, std::size_t cap) {
    if (!Gate(Tool::player, msg, cap)) return false;
    Sam s;
    if (const char* why = Playing(&s)) return Put(msg, cap, "dev marker refused: %s", why), false;
    MkCheck();
    if (!(g_mk_features & kMkRead)) return Put(msg, cap, "map markers are off: this game build differs (%s)", g_mk_off), false;
    const std::uintptr_t mgr = mem::Q(A(kMkGlobal));
    if (!mgr) return Put(msg, cap, "no marker manager"), false;
    const int area = devrules::KnownArea(CallArea());
    if (op == 1) {
        if (!automation::Enabled() || RealSaveRefusal()) return Put(msg, cap, "dev marker add: hidden runs on a sandboxed save only"), false;
        if (!(g_mk_features & kMkWrite)) return Put(msg, cap, "dev marker add is off: this game build differs (%s)", g_mk_off), false;
        const int letter = CallMkNewRoute(mgr);
        const int number = letter >= 0 ? CallMkNextNum(mgr, letter) : -1;
        const float pos[4] = {static_cast<float>(s.xf.pos.x + east), static_cast<float>(s.xf.pos.y + north), static_cast<float>(s.xf.pos.z), 0.f};
        const bool ok = devrules::MarkerSlotOk(letter, number) && CallMkAdd(mgr, pos, letter, number, 1, 0);
        char name[4];
        devrules::MarkerName(letter, number, name);
        Log("[dev] marker add %s (letter %d number %d) at (%.2f, %.2f, %.2f), %.0f m east %.0f m north of Sam: %s", name, letter, number, pos[0], pos[1], pos[2],
            east, north, ok ? "added (the game snaps it to the ground)" : "NOT added");
        return Put(msg, cap, ok ? "map marker %s added at (%.0f, %.0f)" : "map marker add failed (%s)", name, pos[0], pos[1]), ok;
    }
    if (op != 0) return Put(msg, cap, "dev marker 0 log, 1 <m east> <m north> add one (hidden runs)"), false;
    devrules::Marker m[kMkSlots];
    const int n = MkRead(m, area);
    std::uint64_t next_id = 0;
    std::uint32_t flags = 0;
    mem::Rd(mgr + 0x5F00, next_id), mem::Rd(mgr + 0x5F08, flags);
    const int newest = devrules::PickMarker(m, n > 0 ? n : 0, -1, area);
    Log("[dev] markers: %d active in the pool, next id %llu, flags 0x%X, area %d", n, static_cast<unsigned long long>(next_id), flags, area);
    for (int i = 0; i < n; ++i) {
        char name[4];
        devrules::MarkerName(m[i].letter, m[i].number, name);
        const double dx = m[i].pos[0] - s.xf.pos.x, dy = m[i].pos[1] - s.xf.pos.y;
        Log("[dev] marker %s: id %llu at (%.2f, %.2f, %.2f), %.0f m, %s, area %d%s", name, static_cast<unsigned long long>(m[i].id), m[i].pos[0], m[i].pos[1],
            m[i].pos[2], std::sqrt(dx * dx + dy * dy), m[i].snapped ? "on the ground"
            : m[i].pending == 1 ? "the terrain height, the game's raycast pending" : "ground snap pending", m[i].area, i == newest ? " (newest)" : "");
    }
    return Put(msg, cap, "%d map markers (logged)", n < 0 ? 0 : n), n >= 0;
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
    v.give_on = g_give_cfg && (g_features & kFGive) != 0;
    v.give_busy = g_give.active;
    Put(v.give_line, sizeof(v.give_line), "%s", !g_give_cfg ? "off (session.ini dev_give=0)" : g_give_line);
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
    g_give_cfg = config.dev_give;
    g_give_max_kg = config.dev_give_max_kg;
    g_give_per_frame = config.dev_give_per_frame;
    g_give_slot = config.dev_give_slot;
    if (g_enabled)
        Log("[dev] give: dev_give=%d, up to %.0f kg a press, %d requests a tick, slot %d", g_give_cfg ? 1 : 0, g_give_max_kg,
            g_give_per_frame, g_give_slot);
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
    GotoTick(now_ms);    // a goto runs only once one was asked (Teleport to map marker needs no dev tools)
    MarkerTick(now_ms);  // the map markers (any session): the view at 2 Hz while the menu shows it, else the area tags every 5 s
    if (g_enabled) BootsTick(now_ms);
    if (g_enabled) GiveTick(now_ms);  // dev_give: the requests a few a tick, then the arrival check
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
        case Op::place: return GoPlace(static_cast<int>(v[0]), static_cast<int>(v[1]), static_cast<int>(v[2]), g_now_ms, msg, cap);
        case Op::items: return Items(static_cast<int>(v[0]), v[1], msg, cap);
        case Op::go_to: return Goto(v[0], v[1], v[2], g_now_ms, msg, cap);
        case Op::boots: return BootsCmd(static_cast<int>(v[0]), v[1], msg, cap);
        case Op::marker_go: return MarkerGo(devrules::MarkerWant(v[0]), g_now_ms, msg, cap);
        case Op::marker_test: return MarkerTest(static_cast<int>(v[0]), v[1], v[2], msg, cap);
        case Op::give: return Give(static_cast<int>(v[0]), v[1], v[2], msg, cap);
    }
    return Put(msg, cap, "unknown dev op"), false;
}

View GetView() noexcept {
    AcquireSRWLockShared(&g_view_lock);
    View v = g_view;
    ReleaseSRWLockShared(&g_view_lock);
    return v;
}

MarkerView GetMarkerView() noexcept {
    InterlockedExchange64(&g_mk_wanted_until, static_cast<LONG64>(GetTickCount64()) + 1500);  // the overlay shows the button
    AcquireSRWLockShared(&g_mk_lock);
    MarkerView v = g_mk_view;
    ReleaseSRWLockShared(&g_mk_lock);
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
