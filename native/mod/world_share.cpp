// Shared structures, game side (world sync W1 structures, W2 contributions, W3 ladders and climbing ropes, W4 ziplines; world_share.hpp,
// docs/world-sync.md).
#include "world_share.hpp"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <vector>

#include "MinHook.h"
#include "automation.hpp"
#include "coop_session.hpp"
#include "dev_rules.hpp"
#include "dev_tools.hpp"
#include "game_actions.hpp"
#include "link.hpp"
#include "log.hpp"
#include "nametags.hpp"
#include "online_gate.hpp"
#include "partner_source.hpp"
#include "world_rules.hpp"
#include "world_wire.hpp"

namespace st::wshare {
namespace {
namespace mem = partner::mem;
using game::WorldTransform;

static_assert(wwire::kChannel == link::kWorldEventChannel, "world events use link channel 1");

constexpr std::uintptr_t kImage = 0x140000000ull;
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

// ---------------------------------------------------------------- game addresses (ds.exe, Director's Cut)
constexpr std::uintptr_t kMgrGlobal = 0x147BF4960;     // DSConstructionPointManager*
constexpr std::uintptr_t kEnqueueInfo = 0x142C47AF0;   // (mgr, info) -> info+0x48: the creation queue, under the manager's CS
constexpr std::uintptr_t kFindById = 0x142C57350;      // (mgr, id) -> cp or 0
constexpr std::uintptr_t kNewId = 0x142C5BD80;         // (mgr, net type) -> id (Player: ++[mgr+0x572D34] + 10000)
constexpr std::uintptr_t kInfoCtor = 0x14277FDE0;      // (info)
constexpr std::uintptr_t kInfoDtor = 0x1428ACA00;      // (info)
constexpr std::uintptr_t kExportInfo = 0x142C575D0;    // cp vt+0x48 (base): cp -> info
constexpr std::uintptr_t kInitFromInfo = 0x142C32760;  // cp vt+0x50 (base)
constexpr std::uintptr_t kCpBaseVt = 0x143DD9408;
constexpr std::uintptr_t kTypeToNet = 0x142CCF1D0;     // (u8 type) -> net type
constexpr std::uintptr_t kTypeToCat = 0x142CCF0F0;     // (u8 type) -> category
constexpr std::uintptr_t kOwnerHandle = 0x1434ABDB0;   // (u32* h, idx 1 = the local user)
constexpr std::uintptr_t kIdCopy = 0x1434AC430;        // (dst, src)
constexpr std::uintptr_t kIdFree = 0x1434ABFF0;        // (h)
constexpr std::uintptr_t kSetLastBuilt = 0x142CB5280;  // (mgr, const WorldTransform* pcc, id, type): the printer
constexpr std::uintptr_t kRemoveManually = 0x142CA4060;// (cp): the dismantle
constexpr std::uintptr_t kRemoveFn = 0x142CA3E00;        // (cp, bool keep_cargo): the removal bits and the stored cargo (collapse, placement re-check)
constexpr std::uintptr_t kInitInfo = 0x142CD4EF0;      // (info, type, const WorldTransform*, id): memset, ctor, type, id, current area, yaw-only transform
// W2: contributions, upgrades, repair (decompiled; world_rules.hpp "contributions")
constexpr std::uintptr_t kAddOwn = 0x142C41540;        // (cp, const i32 m[6]): +0x210 += m; telemetry / stats / likes (non-own); never the level
constexpr std::uintptr_t kAddOthers = 0x142C321A0;     // (cp, u64 stamp, const i32 m[6], bool force, road): +0x248 += m below max level; nothing online
constexpr std::uintptr_t kAddRepair = 0x142C41CB0;     // (cp, const i32 m[6]): +0x260 += m; telemetry / stats
constexpr std::uintptr_t kStatus = 0x142C59770;        // (cp, i32 need_rem[6] | 0, i32 invested[6] | 0) -> 0 enough, 1 missing, 2 max, 3 none, 4 running
constexpr std::uintptr_t kQueueLevelUp = 0x142C69170;  // (mgr, cp) -> id | -1: the id into the level-up list +0x56A2A8 (under the CS)
constexpr std::uintptr_t kNeedToLevel = 0x142C5D9D0;   // (cp, level, i32 out[6]): what levels 1..level need in all
constexpr std::uintptr_t kInvested = 0x142C58A50;      // (cp, i32 out[6]): own + contributor list + others
constexpr std::uintptr_t kReached = 0x142C35CC0;       // (cp) -> the level the invested materials reach (the level-up's target)
constexpr std::uintptr_t kRepairDurBase = 0x142CA2D30; // cp vt+0x140 (base): (cp, float seconds) durability += s, capped at the level's
// W3: ladders (DSLadder, vtable 0x143DE6208) and climbing anchors / ropes (DSFieldRopeConstructionPoint, 0x143DE5E98)
constexpr std::uintptr_t kLadderVt = 0x143DE6208, kLadderExport = 0x142D3C870, kLadderInit = 0x142D2DD00;
constexpr std::uintptr_t kRopeVt = 0x143DE5E98, kRopeExport = 0x142D24470, kRopeInit = 0x142D24530;
constexpr std::uintptr_t kRopeCommitEnqueue = 0x1428D82A9;  // the anchor branch of the player's commit 0x1428D8070: EnqueueInfo
// W4: ziplines (DSZiplineConstructionPoint, vtable 0x143DDCC38). ExportInfo 0x142CE5A40 / InitFromInfo 0x142CE5A80 add one
// byte (record +0x578 <-> cp+0x520). The activation (vt+0x10 0x142CE5520) collects the ziplines of its area within the
// config's range (+0x4AC) with 0x142CAA000(mgr, area, pos, category 9, range, out) and links each both ways (the 0x18-byte
// handle arrays cp+0x528 {i32 count, i32 cap, entries}: entry +0 = the other zipline + 0x18). Not saved: each world links
// its ziplines itself, so the same ziplines link the same.
constexpr std::uintptr_t kZipVt = 0x143DDCC38, kZipExport = 0x142CE5A40, kZipInit = 0x142CE5A80, kZipActivate = 0x142CE5520, kZipCollect = 0x142CAA000;
constexpr std::uintptr_t kZipLinks = 0x528;
// The game's placement check (the build preview's 0x1428B1C60 and the re-checks call it): (mgr, info, reasons[0x23] x 16 B,
// result[0x23 B], out, ignore entity | 0, u8, u8) -> ok; it snaps the record's transform (+8 / +0x20, +0x680) and, for a
// ladder (0x142C3C070) / rope (0x142C3F120), fills their geometry. The reasons are {u64, array}: freed with 0x1418F9610.
constexpr std::uintptr_t kPlaceCheck = 0x142C38200, kReasonFree = 0x1418F9610;
// The ladder's / rope's placement re-check (vt+0x60: the ground no longer holds it -> the removal bits) and the ladder's
// ground re-attach (0x142D2E5B0, also called by the rope's): a removal they make is the game's own, not a pick-up.
constexpr std::uintptr_t kLadderRecheck = 0x142D2E340, kLadderAttach = 0x142D2E5B0, kRopeRecheck = 0x142D24610;
// manager fields
constexpr std::uintptr_t kMgrCfg = 0x70, kMgrPool = 0x80, kLiveCount = 0x56A258, kLiveData = 0x56A260, kQueueCount = 0x56A278, kMgrCs = 0x56A5B8,
                         kPlayerCounter = 0x572D34, kLevelUpCount = 0x56A2A8, kLevelUpData = 0x56A2B0;
constexpr std::uintptr_t kSlot = 0x710;
constexpr unsigned kSlots = 3140;
// construction point fields
constexpr std::uintptr_t kCpId = 0x30, kCpType = 0x34, kCpCat = 0x35, kCpState = 0x36, kCpPos = 0x40, kCpLevel = 0x84, kCpEntity = 0x88, kCpCfg = 0x90,
                         kCpUuid = 0x98, kCpAA = 0xAA, kCpAB = 0xAB, kCpAD = 0xAD, kCpNet = 0xB2, kCpServer = 0xB8, kCpDur = 0xD8, kCpOwn = 0x210,
                         kCpOthers = 0x248, kCpRepair = 0x260, kCpList = 0x228, kCpA8 = 0xA8, kCpStamp = 0x458;
// per-category config (DSConstructionPointConfig) fields
constexpr std::uintptr_t kCfgLimitPlayer = 0x90, kCfgMaxLevel = 0x98, kCfgDurability = 0xA0 /* float[5] per level */, kCfgRepairW = 0x210 /* float[6] */;

enum Group : unsigned { kCore = 1u, kBuild = 2u, kRemove = 4u, kContrib = 8u, kLadder = 16u, kZipline = 32u };
struct Site {
    std::uintptr_t va;
    const char* sig;
    const char* name;
    unsigned group;
};
// clang-format off
constexpr Site kSites[] = {
    {0x142CD6C1D,     "48 83 3D ?? ?? ?? ?? 00", "manager global (CreateConstruction)", kCore},
    {kEnqueueInfo,    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 99 B8 A5 56 00 48 8B F1", "EnqueueInfo", kCore},
    {0x142C85CE0,     "4C 8B DC 55 41 55 49 8D AB B8 F7 FF FF 48 81 EC 38 09 00 00", "ProcessCreationQueue", kCore},
    {0x142C86400,     "80 BD 05 07 00 00 00", "ProcessCreationQueue: the new-build byte +0x645", kCore},
    {0x142C86432,     "FF 50 28 C7 86 18 05 00 00 00 00 F0 42", "ProcessCreationQueue: RegisterNew call", kCore},
    {kFindById,       "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 30 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 28 8B DA", "FindById", kCore},
    {kInfoCtor,       "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 50 C6 01 00 B8 FF FF 00 00", "record ctor", kCore},
    {kInfoDtor,       "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B E9 48 81 C1 70 06 00 00", "record dtor", kCore},
    {kExportInfo,     "48 89 6C 24 20 56 57 41 57 48 83 EC 20 8B 41 30", "ExportInfo", kCore},
    {kInitFromInfo,   "48 89 5C 24 18 48 89 6C 24 20 56 57 41 54 41 56 41 57 48 83 EC 30 8B 42 48", "InitFromInfo", kCore},
    {0x142C32AB4,     "0F B6 87 2E 06 00 00 F6 D8", "InitFromInfo: the offline byte +0x62E", kCore},
    {kTypeToNet,      "0F B6 C1 83 F8 2D 77 21 48 8D 15 ?? ?? ?? ?? 0F B6 84 02 08 F2 CC 02", "TypeToNetType", kCore},
    {kTypeToCat,      "0F B6 C1 83 F8 2D 77 4E 48 8D 15 ?? ?? ?? ?? 0F B6 84 02 94 F1 CC 02", "TypeToCategory", kCore},
    {kNewId,          "40 53 44 0F B6 DA 48 8B D9 84 D2 74 36 80 FA 02", "NewId", kBuild},
    {kOwnerHandle,    "40 53 48 83 EC 20 48 8B D9 89 11 48 8B 0D ?? ?? ?? ??", "owner handle", kBuild},
    {kIdCopy,         "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B FA 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 21 8B 02 39 03", "owner handle copy", kBuild},
    {kIdFree,         "48 8B D1 48 8B 0D ?? ?? ?? ?? 48 85 C9 0F 85", "owner handle free", kBuild},
    {kSetLastBuilt,   "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 8D 99 B8 A5 56 00 48 8B F9", "SetLastBuilt", kBuild},
    {0x1428D8756,     "C6 83 F5 07 00 00 01", "the PCC commit sets +0x645", kBuild},
    {0x1428D8767,     "E8 ?? ?? ?? ??", "the PCC commit's EnqueueInfo call", kBuild},
    {0x1428D87F2,     "E8 ?? ?? ?? ??", "the PCC commit's SetLastBuilt call", kBuild},
    {kInitInfo,       "48 89 5C 24 10 56 57 41 56 48 81 EC D0 00 00 00", "InitInfo (the PCC preview's record)", kBuild},
    {kRemoveManually, "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 30 80 89 AA 00 00 00 40", "RemoveManually", kRemove},
    {kRemoveFn,       "40 53 56 41 56 B8 10 F5 00 00 E8", "Remove (the removal bits and the stored cargo)", kLadder},
    // W2 contributions (group contrib: off -> only contributions are off)
    {kAddOwn,         "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 70 FF FF FF 48 81 EC 90 01 00 00", "AddOwn (AddMaterials)", kContrib},
    {kAddOthers,      "40 53 57 41 57 48 83 EC 40 4C 8B BC 24 80 00 00 00 49 8B F8 48 8B D9 45 84 C9 75 0D 48 39 91 58", "AddOthers", kContrib},
    {kAddRepair,      "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 80 48 81 EC 80 01 00 00", "AddRepair", kContrib},
    {kStatus,         "40 55 53 56 57 41 56 48 8B EC 48 83 EC 70 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 F8 33 C0 4C 8B", "Status (materials for the next level)", kContrib},
    {kQueueLevelUp,   "48 89 74 24 18 57 48 83 EC 20 48 8B 81 60 A2 56 00 48 8B FA 4C 63 81 58 A2 56 00 48 8B F1", "QueueLevelUp", kContrib},
    {kNeedToLevel,    "40 55 56 41 56 41 57 48 83 EC 48 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 30 33 C0 4D 8B F0", "NeedToLevel", kContrib},
    {kInvested,       "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 33 C0 48 8B F1 48 89 02 48 89 42 08 48 89 42 10", "Invested", kContrib},
    {kReached,        "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 70 48 8B 05 ?? ?? ?? ?? 48 33 C4", "the level materials reach", kContrib},
    {0x142CBAD0A,     "4C 8B B6 B0 A2 56 00", "the level-up list's consumer (+0x56A2B0)", kContrib},
    {0x142CBAD51,     "E8 ?? ?? ?? ?? 4C 8B 03", "the consumer: the level materials reach", kContrib},
    {0x142CBAD5E,     "41 FF 90 C0 00 00 00", "the consumer: StartLevelUp (vt+0xC0)", kContrib},
    {0x142CF9968,     "E8 ?? ?? ?? ??", "the terminal commit's AddOwn call", kContrib},
    {0x142CF9C5C,     "E8 ?? ?? ?? ??", "the terminal commit's QueueLevelUp call", kContrib},
    {0x142D32B92,     "E8 ?? ?? ?? ??", "the repair commit's AddRepair call", kContrib},
    {kRepairDurBase,  "48 83 EC 48 8B 81 84 00 00 00 BA 01 00 00 00 4C 8B 81 90 00 00 00", "the durability repair (vt+0x140)", kContrib},
    // W3 ladders and climbing ropes (group ladder: off -> only ladders and ropes are off)
    {kLadderExport,   "48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 E8", "the ladder's ExportInfo (vt+0x48)", kLadder},
    {kLadderInit,     "48 8B C4 53 48 81 EC A0 00 00 00 48 89 70 10 48 8B D9 48 89 78 18", "the ladder's InitFromInfo (vt+0x50)", kLadder},
    {kRopeExport,     "48 89 5C 24 08 57 48 83 EC 30 48 8B FA 48 8B D9 E8", "the rope's ExportInfo (vt+0x48)", kLadder},
    {kRopeInit,       "48 89 5C 24 10 57 48 83 EC 20 48 8B DA 48 8B F9 E8", "the rope's InitFromInfo (vt+0x50)", kLadder},
    {kRopeCommitEnqueue, "E8 ?? ?? ?? ?? 48 8B 4B 38 33 D2 8B 84 24 98 01 00 00 89 81 18 0B 00 00", "the anchor commit's EnqueueInfo call", kLadder},
    {kPlaceCheck,     "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D8 48 81 EC 38 01 00 00", "the placement check", kLadder},
    {kReasonFree,     "48 8B 49 08 E9", "the placement check's reason free", kLadder},
    {kLadderRecheck,  "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D AC 24 20 FE FF FF 48 81", "the ladder's placement re-check (vt+0x60)", kLadder},
    {kLadderAttach,   "48 8B C4 53 57 41 54 41 57 48 81 EC B8 0A 00 00", "the ladder's ground re-attach", kLadder},
    {kRopeRecheck,    "48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8D 6C 24 80 48 81 EC 80 01 00 00", "the rope's placement re-check (vt+0x60)", kLadder},
    // W4 ziplines (group zipline: off -> only ziplines are off)
    {kZipExport,      "48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? 0F B6 83 20 05 00 00", "the zipline's ExportInfo (vt+0x48)", kZipline},
    {kZipInit,        "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 E8 ?? ?? ?? ?? 0F B6 83 78 05 00 00", "the zipline's InitFromInfo (vt+0x50)", kZipline},
    {kZipActivate,    "4C 8B DC 57 48 81 EC 80 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 58", "the zipline's activation (vt+0x10)", kZipline},
    {0x142CE5575,     "41 B1 09 4C 89 64 24 48 4C 89 64 24 50 C5 FA 10 80 AC 04 00 00", "the activation: category 9 within the config's range", kZipline},
    {0x142CE5590,     "E8 ?? ?? ?? ??", "the activation: the zipline collector call", kZipline},
};
// clang-format on

// ---------------------------------------------------------------- timing
constexpr double kCensusMs = 500;
constexpr double kSettleMs = 3000;      // after the world is ready: the first census, then HELLO / SNAP_REQ
constexpr double kSnapRetryMs = 6000;   // the guest asks again without an answer
constexpr double kCheckMs = 2000;
constexpr double kHealGapMs = 10000;
constexpr double kVerifyMs = 3000;
constexpr double kMirrorPendingMs = 6000;
constexpr double kHostNewMs = 30000;
constexpr double kNotReadyHoldMs = 30000;
constexpr double kDurMs = 5000;         // W2: durabilities (DUR) every 5 s
constexpr double kStatePendingMs = 15000;

// ---------------------------------------------------------------- state
std::atomic<bool> g_on{false};
int g_role = 0;  // 1 host, 2 guest
char g_off[220] = "not installed";
bool g_checked = false, g_checks_ok = false, g_remove_ok = false;
unsigned g_groups_ok = 0;
bool g_stopped = false;
int g_anim_mode = 0;  // Config::world_anim
bool g_types_ok = false;

using FnEnqueue = std::uint32_t (*)(std::uintptr_t, std::uintptr_t);
using FnFind = std::uintptr_t (*)(std::uintptr_t, std::int32_t);
using FnNewId = std::int32_t (*)(std::uintptr_t, std::uint8_t);
using FnInfo = void (*)(std::uintptr_t);
using FnExport = void (*)(std::uintptr_t, std::uintptr_t);
using FnTypeMap = std::uint8_t (*)(std::uint8_t);
using FnOwner = std::uintptr_t (*)(std::uint32_t*, std::uint32_t);
using FnIdCopy = void (*)(std::uintptr_t, std::uint32_t*);
using FnIdFree = void (*)(std::uint32_t*);
using FnSetLast = void (*)(std::uintptr_t, const void*, std::uint32_t, std::uint8_t);
using FnRemMan = void (*)(std::uintptr_t);
using FnInitInfo = std::uintptr_t (*)(std::uintptr_t, std::uint8_t, const void*, std::int32_t);
FnEnqueue o_enqueue = nullptr;
FnSetLast o_setlast = nullptr;
FnRemMan o_remman = nullptr;
// W2
using FnMats = void (*)(std::uintptr_t, const std::int32_t*);
using FnAddOthers = void (*)(std::uintptr_t, std::uint64_t, const std::int32_t*, char, void*);
using FnStatus = std::uint8_t (*)(std::uintptr_t, std::int32_t*, std::int32_t*);
using FnQueue = std::int32_t (*)(std::uintptr_t, std::uintptr_t);
using FnNeed = void (*)(std::uintptr_t, std::uint32_t, std::int32_t*);
using FnInvested = void (*)(std::uintptr_t, std::int32_t*);
using FnReached = std::int32_t (*)(std::uintptr_t);
using FnRepairDur = void (*)(std::uintptr_t, float);
FnMats o_addown = nullptr;
FnMats o_addrepair = nullptr;
FnQueue o_queue = nullptr;
bool g_contrib_ok = false;                     // config world_contrib, the contrib byte checks, the detours in
std::atomic<bool> g_contrib_share{false};      // guest: the host takes contributions now (its HELLO said contrib, our snapshot is done)
thread_local bool t_dev_mats = false;          // DevMats on the guest: the MATS goes with the dev flag

// W3: ladders and climbing ropes: config world_ladders and the ladder byte checks (g_ladders_ok); shared now = the
// partner's HELLO has parts bit2 too (g_ladders_share, any thread).
bool g_ladders_ok = false;
std::atomic<bool> g_ladders_share{false};
// The game's own Remove(cp, keep_cargo) on a ladder / rope, and who called it: the placement re-checks (the ladder's
// vt+0x60 0x142D2E340 and its ground re-attach 0x142D2E5B0, the base re-check 0x142C346D0, ...) remove a ladder the ground
// no longer holds. Such a removal is NOT her pick-up: the host keeps its ladder and sends it again (LOST).
constexpr std::uintptr_t kAutoRemoveReturns[] = {0x142C34EC9, 0x142C61F52, 0x142CF5DD3, 0x142D1FB89, 0x142D2EC16, 0x142D2E526,
                                                 0x142D2E340, 0x142D2E5B0, 0x142D24610};  // + the re-check detours' own marks
using FnRemove = void (*)(std::uintptr_t, char);
FnRemove o_remove = nullptr;
using Fn4 = void (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
Fn4 o_ladder_recheck = nullptr, o_ladder_attach = nullptr, o_rope_recheck = nullptr;
std::map<std::uint32_t, std::uintptr_t> g_removed_by;  // id -> the caller's return address (VA), under g_box
std::atomic<unsigned> c_pickups_sent{0}, c_pickups_applied{0}, c_ladders_unshared{0};
// The structures this game tracks (its census): the PCC structures, and ladders / ropes when the W3 part is on here.
// W4: ziplines: config world_ziplines and the zipline byte checks (g_zip_ok); shared now = the partner's HELLO has parts bit3.
bool g_zip_ok = false;
std::atomic<bool> g_zip_share{false};
std::atomic<unsigned> c_zip_unshared{0};
bool Tracked(int type) noexcept { return wrules::IsSynced(type, g_ladders_ok, g_zip_ok); }
// ... and shares with the partner now (ladders / ropes, ziplines only when both games have that part).
bool SharedType(int type) noexcept {
    return wrules::IsSynced(type, g_ladders_ok && g_ladders_share.load(std::memory_order_acquire), g_zip_ok && g_zip_share.load(std::memory_order_acquire));
}
// The contributable structures (W2) with W4's ziplines when that part is shared, and the durabilities DUR carries.
bool ContribT(int type) noexcept { return wrules::Contributable(type, g_zip_ok && g_zip_share.load(std::memory_order_acquire)); }
bool DurT(int type) noexcept {
    return wrules::DurShared(type, g_ladders_ok && g_ladders_share.load(std::memory_order_acquire), g_zip_ok && g_zip_share.load(std::memory_order_acquire));
}
// What a structure type's part is called (the "not shared" lines).
const char* PartName(int type) noexcept { return wrules::Zipline(type) ? "ziplines" : wrules::LadderOrRope(type) ? "ladders and ropes" : "structures"; }

thread_local bool t_dev_build = false;  // DevBuild on the guest: the held record goes as a dev GBUILD
thread_local bool t_dev_remove = false; // round 5: DevRemove on the guest: its REMREQ goes with the dev flag (the host decides)
std::set<std::uint32_t> g_dev_manual;   // ... the ids it dismantled (under g_box)
constexpr std::uintptr_t kDevRemoveMark = 1;  // g_removed_by: a dev removal (removal bits), not a pick-up
thread_local bool t_dev_anim = true;    // ... with or without the build-up

// ---- the detours' inboxes (any thread -> game thread), under g_box
SRWLOCK g_box = SRWLOCK_INIT;
struct Hold {               // guest: a held local build
    std::uint32_t q = 0;    // the id her commit allocated (never enqueued)
    std::uint16_t n = 0;    // request number
    wwire::Record rec;
    bool has_pcc = false;
    wwire::Xf pcc;
    bool dev = false;
    bool anim = true;       // the build-up and printer (a real commit always; a dev build as asked)
    double t = 0;           // game-thread time it was first seen (0 = new)
    bool sent = false;
    std::uint32_t sent_gen = 0;
};
std::vector<Hold> g_holds;
struct HostNew {            // host: a structure created this session (its own commit, a dev build, a guest's build)
    std::uint32_t id = 0;
    wwire::Record rec;
    bool has_pcc = false;
    wwire::Xf pcc;
    wwire::Origin origin = wwire::Origin::host;
    wwire::ReqId req;
    bool anim = true;
    double t = 0;
};
std::vector<HostNew> g_host_new;
std::vector<std::uint32_t> g_manual_ids;  // RemoveManually seen (not the mod's): host classify / guest REMREQ
std::vector<std::uint32_t> g_local_only;  // guest: structures built while the host's game was not known to share (kept)
std::atomic<unsigned> c_held{0}, c_passed{0}, c_setlast_held{0};
struct MatsIn {             // guest: a local contribution / repair the detour saw (any thread -> game thread)
    std::uint32_t local = 0;
    std::uint8_t type = 0;
    double pos[3] = {};
    wwire::MatsKind kind = wwire::MatsKind::build;
    std::int32_t m[6] = {};
    bool dev = false;
};
std::vector<MatsIn> g_mats_in;
std::atomic<unsigned> c_lvl_swallowed{0}, c_mats_local{0}, c_mats_unshared{0}, c_lvl_double{0};

// ---- game thread
struct CEnt {
    std::uintptr_t cp = 0;
    std::uint32_t id = 0;
    std::uint8_t type = 0, cat = 0, state = 0, net = 0;
    std::uint32_t level = 0;
    double pos[3] = {};
    std::uint8_t aa = 0, ab = 0, ad = 0;
    float dur = 0;
    // W2 (contributable structures only, `mats` true)
    bool mats = false;
    bool running = false;   // cp+0xA8 bit 7: a level-up (or the first build-up) runs
    std::uint32_t max_level = 0;
    float max_dur = 0;      // the level's durability (config +0xA0)
    std::int32_t own[6] = {}, inv[6] = {}, rep[6] = {}, list[6] = {};
    bool removing() const noexcept { return (aa & 0x40) != 0 || state == 3; }
};
struct Census {
    bool ok = false;
    double t = -1e18;
    std::uintptr_t mgr = 0;
    std::vector<CEnt> all;
    std::map<std::uint32_t, std::size_t> synced;  // id -> index into all (synced, not removing)
    std::map<std::uint32_t, std::size_t> contrib; // W2: id -> index (contributable, not removing)
    std::set<std::uint32_t> lvl_queued;           // W2: the ids in the level-up list +0x56A2A8
} g_cen;

double g_now = 0;
std::uint32_t g_gen = 0;
bool g_connected = false;
bool g_ready = false;
std::uint32_t g_world = 0;
double g_ready_since = 0;
bool g_have_partner = false;
WorldTransform g_partner{};

std::uint32_t g_seq = 0;
wrules::SenderWindow g_rx_seq;  // round 5: per sender process (its HELLO's nonce): a restarted partner starts a fresh window
std::deque<std::vector<std::uint8_t>> g_out;
std::uint32_t g_nonce = 0;
std::uint16_t g_req_n = 0;

// peer
bool g_peer_hello = false, g_peer_on = false;
std::uint8_t g_peer_parts = 0;
bool g_hello_sent = false;
bool g_peer_off_logged = false;

// host
bool g_host_base = false;                  // the census diff has a base for this world
std::map<std::uint32_t, CEnt> g_prev;      // the previous census's synced set
bool g_guest_in_world = false;             // the guest's SNAP_REQ named our world
double g_last_check = 0;
std::uint16_t g_digest_serial = 0;
struct HostReq {
    wwire::ReqId req;
    std::uint32_t hid = 0;
    wrules::PosKey key;
};
std::vector<HostReq> g_host_reqs;
struct PendingGb {
    wwire::GBuild gb;
    std::uint16_t flags = 0;
    double t = 0;
};
std::vector<PendingGb> g_pending_gb;       // the host was not ready yet
std::set<std::uint32_t> g_rem_sent;
std::set<std::uint32_t> g_picked_up;       // W3: ladders / ropes the guest picked up (REMREQ pickup): no REM back
std::set<std::uint32_t> g_host_picked;     // round 5: ladders / ropes the host picked up itself (a guest REMREQ pickup after it: both did)

// guest
bool g_snap_ok = false;                    // a snapshot completed in this world ("Ready")
unsigned g_snaps = 0;
double g_snap_asked = -1e18;
bool g_snap_waiting = false;
wwire::DigestAssembler g_asm;
std::map<std::uint32_t, std::uint32_t> g_h2g, g_g2h;  // mapped ids only (exact ones are equal)
std::set<std::uint32_t> g_bound;           // host ids bound in this world
std::map<std::uint32_t, double> g_mirror_pending;  // hid -> when MirrorCreate enqueued it
struct Verify {
    std::uint32_t hid = 0, local = 0;
    std::uint8_t type = 0;
    double t = 0;
};
std::vector<Verify> g_verify;
std::set<std::uint32_t> g_rem_got;         // REMs applied (no LOST for these)
struct Seen {
    std::uint8_t type = 0;
    float dur = 0;
    double x = 0, y = 0, z = 0;
};
std::map<std::uint32_t, Seen> g_last_seen;  // W3: bound structures as the last census saw them (a pick-up leaves no trace)
std::set<std::uint32_t> g_remreq_sent;
// round 5: the REMREQs (dismantle, pick-up) the host may not have received: sent again once per new link session (the link
// starts its reliable channel afresh then); kept until a snapshot asked after them completes (the host had them by then).
struct PendRem {
    wwire::Rem r;
    std::uint16_t flags = 0;
    std::uint32_t gen = 0;
    double t = 0;
};
std::map<std::uint32_t, PendRem> g_remreq_pend;
std::uint8_t g_snap_reason = 0;            // the next SNAP_REQ's reason (2 after a new link session in the same world)
std::uint8_t g_snap_sent_reason = 0;       // the reason of the last one sent
unsigned g_heal_idle = 0;                  // heals in a row that changed nothing (the heal gap backs off)
std::map<std::uint32_t, unsigned> g_lost_n;  // round 5: LOSTs sent per host id in this world
std::set<std::uint32_t> g_lost_given;      // ... given up: her world keeps removing it
unsigned g_check_miss = 0;
double g_last_heal = -1e18;
bool g_hold_sends = false;                 // world_sync 5 (tests)
bool g_limits_raised = false;
struct Confirm {
    wwire::ReqId req;
    std::uint32_t hid = 0;
};

// W2 host
struct SKey {
    std::uint32_t level = 0;
    std::uint8_t lu = 0;
    std::uint64_t h = 0;
    bool operator!=(const SKey& o) const noexcept { return level != o.level || lu != o.lu || h != o.h; }
};
std::map<std::uint32_t, SKey> g_skey;       // the last STATE key per structure (base: the census after the world is ready)
bool g_state_base = false;
std::set<std::uint32_t> g_state_dirty;      // changed since this world loaded: STATE again at the guest's join
std::set<std::uint32_t> g_recheck;          // after a guest contribution: queue the next level-up once the running one ends
std::map<std::uint32_t, float> g_dur_sent;  // the last durability sent (cleared at the guest's join: all again)
double g_last_dur = 0;
wrules::ReqLog g_mats_log;                  // the guest contributions applied (a retry is answered, not applied twice)
// W2 guest
struct GMats {
    wwire::Mats m;
    std::uint16_t flags = 0;
    bool sent = false;
    std::uint32_t sent_gen = 0;
    double sent_t = 0;
};
std::vector<GMats> g_mats_pending;
constexpr double kMatsAnswerMs = 20000;     // no answer from the host: given up (its STATE / the join check fix the numbers)           // our contributions the host has not answered yet
struct PendState {
    wwire::State st;
    double t = 0;
};
std::map<std::uint32_t, PendState> g_state_pending;  // STATE for a structure not (yet) in our world
unsigned c_dur_sent = 0, c_dur_sent_lr = 0;  // W4: durabilities the host sent (of them ladders / ropes)
unsigned c_mats_applied = 0, c_mats_dropped = 0, c_levelups_host = 0, c_levelups_guest = 0, c_state_applied = 0, c_dur_applied = 0, c_excess = 0,
         c_mats_retry = 0;

// counters
unsigned c_tx[17] = {}, c_rx[17] = {}, c_bad = 0, c_stale = 0, c_dup = 0;
unsigned c_mirrors = 0, c_mirror_fail = 0, c_mirror_exact = 0, c_mirror_mapped = 0, c_created_for_guest = 0, c_rem_applied = 0, c_rej = 0;

// ---------------------------------------------------------------- small helpers
std::uintptr_t Mgr() noexcept { return mem::Q(A(kMgrGlobal)); }
bool InPool(std::uintptr_t mgr, std::uintptr_t cp) noexcept {
    if (!mgr || cp < mgr + kMgrPool) return false;
    const std::uintptr_t off = cp - (mgr + kMgrPool);
    return off < kSlots * kSlot && off % kSlot == 0;
}
bool MgrValid(std::uintptr_t mgr) noexcept {
    static std::uintptr_t s_ok = 0;
    if (!mgr) return false;
    if (mgr == s_ok) return true;
    const char* n = mem::RttiName(mem::RttiOf(mgr));
    if (n && std::strcmp(n, "DSConstructionPointManager") == 0) {
        s_ok = mgr;
        return true;
    }
    return false;
}
template <class T>
T At(std::uintptr_t a) noexcept {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T));
    return v;
}

void ReadCEnt(std::uintptr_t cp, CEnt& e) noexcept {
    e.cp = cp;
    e.id = At<std::uint32_t>(cp + kCpId);
    e.type = At<std::uint8_t>(cp + kCpType);
    e.cat = At<std::uint8_t>(cp + kCpCat);
    e.state = At<std::uint8_t>(cp + kCpState);
    e.level = At<std::uint32_t>(cp + kCpLevel);
    std::memcpy(e.pos, reinterpret_cast<const void*>(cp + kCpPos), 24);
    e.aa = At<std::uint8_t>(cp + kCpAA);
    e.ab = At<std::uint8_t>(cp + kCpAB);
    e.ad = At<std::uint8_t>(cp + kCpAD);
    e.net = At<std::uint8_t>(cp + kCpNet);
    e.dur = At<float>(cp + kCpDur);
}
bool ServerIdEmpty(std::uintptr_t cp) noexcept {
    const std::uintptr_t s = At<std::uintptr_t>(cp + kCpServer);
    std::int32_t len = 0;
    return !s || !mem::Rd(s - 8, len) || len == 0;
}

// W2: a contributable structure's numbers (read only): own +0x210, the contributor list +0x228 (0x38-byte entries, int[6]
// at +0x10), others +0x248 (Invested 0x142C58A50 = their sum), repair +0x260, the running level-up (cp+0xA8 bit 7), the
// config's last level and the level's durability.
bool ReadMats(std::uintptr_t cp, CEnt& e) noexcept {
    std::int32_t oth[6] = {};
    if (!mem::Read(cp + kCpOwn, e.own, sizeof(e.own)) || !mem::Read(cp + kCpOthers, oth, sizeof(oth)) || !mem::Read(cp + kCpRepair, e.rep, sizeof(e.rep)))
        return false;
    std::memset(e.list, 0, sizeof(e.list));
    std::int32_t ln = 0;
    std::uintptr_t ld = 0;
    if (mem::Rd(cp + kCpList, ln) && mem::Rd(cp + kCpList + 8, ld) && ln > 0 && ln <= 64 && ld)
        for (std::int32_t k = 0; k < ln; ++k) {
            std::int32_t v[6] = {};
            if (!mem::Read(ld + static_cast<std::uintptr_t>(k) * 0x38 + 0x10, v, sizeof(v))) break;
            for (int i = 0; i < 6; ++i) e.list[i] += v[i];
        }
    for (int i = 0; i < 6; ++i) e.inv[i] = e.own[i] + e.list[i] + oth[i];
    std::uint8_t a8 = 0;
    mem::Rd(cp + kCpA8, a8);
    e.running = (a8 & 0x80) != 0;
    const std::uintptr_t cfg = mem::Q(cp + kCpCfg);
    e.max_level = 0;
    e.max_dur = 0;
    if (cfg) {
        mem::Rd(cfg + kCfgMaxLevel, e.max_level);
        const std::uint32_t li = e.level > 1 ? (e.level - 1 > 4 ? 4 : e.level - 1) : 0;
        mem::Rd(cfg + kCfgDurability + li * 4, e.max_dur);
    }
    e.mats = true;
    return true;
}

// The live list, read on the game thread. Returns false without a manager.
bool RunCensus(Census& c) noexcept {
    c.ok = false;
    c.all.clear();
    c.synced.clear();
    c.contrib.clear();
    const std::uintptr_t mgr = Mgr();
    if (!MgrValid(mgr)) return false;
    c.mgr = mgr;
    std::int32_t n = 0;
    std::uintptr_t data = 0;
    if (!mem::Rd(mgr + kLiveCount, n) || !mem::Rd(mgr + kLiveData, data) || n < 0 || n > static_cast<std::int32_t>(kSlots) || (n && !data)) return false;
    std::vector<std::uintptr_t> ptrs(static_cast<std::size_t>(n));
    if (n && !mem::Read(data, ptrs.data(), ptrs.size() * 8)) return false;
    c.all.reserve(ptrs.size());
    for (std::uintptr_t cp : ptrs) {
        if (!InPool(mgr, cp)) continue;
        CEnt e;
        ReadCEnt(cp, e);
        c.all.push_back(e);
    }
    for (std::size_t i = 0; i < c.all.size(); ++i) {
        CEnt& e = c.all[i];
        if (Tracked(e.type) && e.net == 1 && !e.removing()) c.synced[e.id] = i;
        if (ContribT(e.type) && !e.removing() && ReadMats(e.cp, e)) c.contrib[e.id] = i;
    }
    c.lvl_queued.clear();
    {
        auto* cs = reinterpret_cast<CRITICAL_SECTION*>(mgr + kMgrCs);
        EnterCriticalSection(cs);
        std::int32_t qn = 0;
        std::uintptr_t qd = 0;
        if (mem::Rd(mgr + kLevelUpCount, qn) && mem::Rd(mgr + kLevelUpData, qd) && qn > 0 && qn < 4096 && qd)
            for (std::int32_t i = 0; i < qn; ++i) {
                std::uint32_t id = 0;
                if (mem::Rd(qd + static_cast<std::uintptr_t>(i) * 4, id)) c.lvl_queued.insert(id);
            }
        LeaveCriticalSection(cs);
    }
    c.ok = true;
    return true;
}
const CEnt* CenById(std::uint32_t id) noexcept {
    auto it = g_cen.synced.find(id);
    return it == g_cen.synced.end() ? nullptr : &g_cen.all[it->second];
}
// the host's id of a guest CP (guest: mapped ids, else equal)
std::uint32_t HidOf(std::uint32_t local) noexcept {
    if (g_role != 2) return local;
    auto it = g_g2h.find(local);
    return it == g_g2h.end() ? local : it->second;
}
std::uint32_t LocalOf(std::uint32_t hid) noexcept {
    if (g_role != 2) return hid;
    auto it = g_h2g.find(hid);
    return it == g_h2g.end() ? hid : it->second;
}
std::vector<wwire::Entry> Entries() {
    std::vector<wwire::Entry> out;
    for (const auto& kv : g_cen.synced) {
        const CEnt& c = g_cen.all[kv.second];
        if (g_role == 2 && std::find(g_local_only.begin(), g_local_only.end(), c.id) != g_local_only.end()) continue;
        if (!SharedType(c.type)) continue;  // ladders / ropes while the partner does not share them
        wwire::Entry e;
        e.hid = HidOf(c.id);
        e.type = c.type;
        e.state = c.state;
        e.level = static_cast<std::uint8_t>(c.level > 5 ? 5 : c.level);
        e.flags = static_cast<std::uint8_t>(((c.ab & 0x80) ? wwire::kEntOffline : 0) | (ServerIdEmpty(c.cp) ? 0 : wwire::kEntUploaded));
        std::memcpy(e.pos, c.pos, sizeof(e.pos));
        out.push_back(e);
    }
    return out;
}

bool LocalSam(WorldTransform* out) noexcept {
    game::Entity* e = nullptr;
    if (!game::TryGetLocalPlayer(&e) || !e) return false;
    const auto a = reinterpret_cast<std::uintptr_t>(e);
    return mem::Read(a + game::entity_off::kPosition, &out->pos, sizeof(out->pos)) && mem::Read(a + game::entity_off::kRotation, &out->rot, sizeof(out->rot));
}
void XfFrom(const WorldTransform& w, wwire::Xf& x) noexcept {
    x.pos[0] = w.pos.x, x.pos[1] = w.pos.y, x.pos[2] = w.pos.z;
    std::memcpy(x.rot, &w.rot, 36);
}

// ---------------------------------------------------------------- game calls (game thread, inside the Tick's / Command's SEH)
std::uintptr_t FindCp(std::uintptr_t mgr, std::uint32_t id) { return reinterpret_cast<FnFind>(A(kFindById))(mgr, static_cast<std::int32_t>(id)); }
std::uint8_t GameCat(std::uint8_t t) { return reinterpret_cast<FnTypeMap>(A(kTypeToCat))(t); }
std::uint8_t GameNet(std::uint8_t t) { return reinterpret_cast<FnTypeMap>(A(kTypeToNet))(t); }

struct InfoBuf {
    alignas(16) std::uint8_t b[wwire::kInfoSize + 16];
    std::uintptr_t p() noexcept { return reinterpret_cast<std::uintptr_t>(b); }
    void Ctor() {
        std::memset(b, 0, sizeof(b));
        reinterpret_cast<FnInfo>(A(kInfoCtor))(p());
    }
    void Dtor() { reinterpret_cast<FnInfo>(A(kInfoDtor))(p()); }
};

// The live CP's record (the game's own ExportInfo, cp vt+0x48, into a ctor-built temporary).
bool ExportRecord(std::uintptr_t cp, wwire::Record* out) {
    const std::uintptr_t vt = mem::Q(cp);
    const std::uintptr_t fn = vt ? mem::Q(vt + 0x48) : 0;
    if (!fn || !mem::InText(fn)) return false;
    InfoBuf info;
    info.Ctor();
    reinterpret_cast<FnExport>(fn)(cp, info.p());
    wwire::RecordFromInfo(info.b, out);
    info.Dtor();
    return true;
}

void RaiseCounter(std::uintptr_t mgr, std::uint32_t hid) {
    std::int32_t cur = 0;
    if (!mem::Rd(mgr + kPlayerCounter, cur)) return;
    const std::int32_t to = wrules::RaisedCounter(cur, hid);
    if (to != cur) *reinterpret_cast<std::int32_t*>(mgr + kPlayerCounter) = to;
}

// Creates a structure through the game's own queue. Returns the id (0 on failure).
//   want_id: the id to use (0 = NewId). offline: record +0x62E. anim: +0x645 (the build-up) and, with a pcc, the printer.
std::uint32_t CreateStructure(const wwire::Record& rec, std::uint32_t want_id, bool offline, bool anim, const wwire::Xf* pcc) {
    const std::uintptr_t mgr = Mgr();
    if (!MgrValid(mgr)) return 0;
    InfoBuf info;
    info.Ctor();
    wwire::RecordToInfo(rec, info.b);
    std::memset(info.b + 0x54, 0, 0x10);  // UUID 0: a non-zero equal UUID would make the queue MOVE an existing CP
    std::uint32_t id = want_id;
    if (!id) id = static_cast<std::uint32_t>(reinterpret_cast<FnNewId>(A(kNewId))(mgr, 1));
    std::memcpy(info.b + 0x48, &id, 4);
    std::uint32_t h = 0;
    reinterpret_cast<FnOwner>(A(kOwnerHandle))(&h, 1);
    reinterpret_cast<FnIdCopy>(A(kIdCopy))(info.p() + 0x50, &h);
    reinterpret_cast<FnIdFree>(A(kIdFree))(&h);
    info.b[0x62E] = offline ? 1 : 0;
    const bool play = anim && g_anim_mode != 3 && g_anim_mode != 2;
    info.b[0x644] = g_anim_mode == 2 ? 1 : 0;
    info.b[0x645] = play ? 1 : 0;
    if (play) std::memset(info.b + 0x78, 0, 4);  // no level-up timer copied into a fresh build
    {
        gate::ScopeMirroring scope;
        reinterpret_cast<FnEnqueue>(A(kEnqueueInfo))(mgr, info.p());
        if (play && pcc && wrules::HasPrinter(rec.Type())) reinterpret_cast<FnSetLast>(A(kSetLastBuilt))(mgr, pcc, id, rec.Type());
    }
    info.Dtor();
    return id;
}

// The game's own removal request (RemoveConstructionsInArea 0x142CD9760): cp+0xAA |= 0x40, cp+0xAB |= 1, under the CS.
void RemoveBits(std::uintptr_t mgr, std::uintptr_t cp) {
    auto* cs = reinterpret_cast<CRITICAL_SECTION*>(mgr + kMgrCs);
    EnterCriticalSection(cs);
    *reinterpret_cast<std::uint8_t*>(cp + kCpAA) |= 0x40;
    *reinterpret_cast<std::uint8_t*>(cp + kCpAB) |= 0x01;
    LeaveCriticalSection(cs);
}
void Dismantle(std::uintptr_t cp) {
    gate::ScopeMirroring scope;
    gate::NoteModCp(cp);
    (o_remman ? o_remman : reinterpret_cast<FnRemMan>(A(kRemoveManually)))(cp);
}

// ---- W2 game calls (game thread, inside the Tick's / Command's SEH)
std::uint8_t GameStatus(std::uintptr_t cp, std::int32_t* need_rem = nullptr, std::int32_t* inv = nullptr) {
    return reinterpret_cast<FnStatus>(A(kStatus))(cp, need_rem, inv);
}
void GameNeed(std::uintptr_t cp, std::uint32_t level, std::int32_t (&out)[6]) {
    alignas(8) std::int32_t b[6] = {};
    reinterpret_cast<FnNeed>(A(kNeedToLevel))(cp, level, b);
    std::memcpy(out, b, sizeof(out));
}
void GameInvested(std::uintptr_t cp, std::int32_t (&out)[6]) {
    alignas(8) std::int32_t b[6] = {};
    reinterpret_cast<FnInvested>(A(kInvested))(cp, b);
    std::memcpy(out, b, sizeof(out));
}
std::int32_t GameReached(std::uintptr_t cp) { return reinterpret_cast<FnReached>(A(kReached))(cp); }
bool LevelUpQueued(std::uintptr_t mgr, std::uint32_t id) {
    bool found = false;
    auto* cs = reinterpret_cast<CRITICAL_SECTION*>(mgr + kMgrCs);
    EnterCriticalSection(cs);
    std::int32_t qn = 0;
    std::uintptr_t qd = 0;
    if (mem::Rd(mgr + kLevelUpCount, qn) && mem::Rd(mgr + kLevelUpData, qd) && qn > 0 && qn < 4096 && qd)
        for (std::int32_t i = 0; i < qn && !found; ++i) {
            std::uint32_t v = 0;
            if (mem::Rd(qd + static_cast<std::uintptr_t>(i) * 4, v) && v == id) found = true;
        }
    LeaveCriticalSection(cs);
    return found;
}
// The game's own level-up request, as the terminal commit makes it (its own consumer then starts StartLevelUp with the level
// the materials reach). Under the mod's scope: our detour passes it, the gate drops anything online inside.
void QueueLevelUpNow(std::uintptr_t mgr, std::uintptr_t cp) {
    gate::ScopeMirroring scope;
    (o_queue ? o_queue : reinterpret_cast<FnQueue>(A(kQueueLevelUp)))(mgr, cp);
}
std::uint32_t MaxLevel(std::uintptr_t cp) {
    std::uint32_t v = 0;
    const std::uintptr_t cfg = mem::Q(cp + kCpCfg);
    if (cfg) mem::Rd(cfg + kCfgMaxLevel, v);
    return v;
}
// The repair commit's durability step: seconds = sum(m[i] * config +0x210[i]), then cp vt+0x140 (capped at the level's).
float RepairSeconds(std::uintptr_t cp, const std::int32_t (&m)[6]) {
    const std::uintptr_t cfg = mem::Q(cp + kCpCfg);
    float w[6] = {};
    if (!cfg || !mem::Read(cfg + kCfgRepairW, w, sizeof(w))) return 0;
    float s = 0;
    for (int i = 0; i < 6; ++i) s += static_cast<float>(m[i]) * w[i];
    return s;
}
void RepairDurability(std::uintptr_t cp, float seconds) {
    const std::uintptr_t vt = mem::Q(cp);
    const std::uintptr_t fn = vt ? mem::Q(vt + 0x140) : 0;
    if (!fn || !mem::InText(fn) || !(seconds > 0)) return;
    reinterpret_cast<FnRepairDur>(fn)(cp, seconds);
}

// ---------------------------------------------------------------- outbox
bool Peer() noexcept { return g_connected && g_peer_hello && g_peer_on; }
void Queue(std::uint8_t* buf, std::size_t n, std::uint8_t type) {
    if (!n) {
        ++c_bad;
        Log("[world] encode FAILED for %s (not sent)", wwire::MsgName(type));
        return;
    }
    if (g_out.size() >= 512) {
        Log("[world] outbox full: %s dropped", wwire::MsgName(type));
        return;
    }
    g_out.emplace_back(buf, buf + n);
    if (type < 17) ++c_tx[type];
}
wwire::Header Hdr(wwire::Msg t, std::uint16_t flags = 0) {
    wwire::Header h;
    h.type = static_cast<std::uint8_t>(t);
    h.flags = flags;
    h.host_world = g_world;
    h.seq = ++g_seq;
    return h;
}
void Flush() {
    while (!g_out.empty() && g_connected && link::WorldEventsQueued() < 24) {
        if (!link::SendWorldEvent(g_out.front().data(), g_out.front().size())) break;
        g_out.pop_front();
    }
}

void SendHello() {
    wwire::Hello b;
    b.on = 1;
    b.parts = static_cast<std::uint8_t>(1u | (g_contrib_ok ? 2u : 0u) | (g_ladders_ok ? 4u : 0u) | (g_zip_ok ? 8u : 0u));
    b.anim_mode = static_cast<std::uint8_t>(g_anim_mode);
    b.ready = g_ready && g_cen.ok ? 1 : 0;
    b.world = g_world;
    b.sender = g_nonce;
    std::uint8_t buf[wwire::kMaxMessage];
    Queue(buf, wwire::EncodeHello(Hdr(wwire::Msg::hello), b, buf, sizeof(buf)), 1);
    g_hello_sent = true;
}

// ---------------------------------------------------------------- host
std::uint64_t g_bucket_cache[wwire::kBuckets];
void Buckets(std::uint64_t (&b)[wwire::kBuckets], std::uint32_t* count) {
    const std::vector<wwire::Entry> e = Entries();
    wwire::BucketHashes(e, b);
    if (count) *count = static_cast<std::uint32_t>(e.size());
}

bool HostNewTake(std::uint32_t id, HostNew* out) {
    bool got = false;
    AcquireSRWLockExclusive(&g_box);
    for (auto it = g_host_new.begin(); it != g_host_new.end(); ++it)
        if (it->id == id) {
            *out = *it;
            g_host_new.erase(it);
            got = true;
            break;
        }
    ReleaseSRWLockExclusive(&g_box);
    return got;
}

void SendAdd(std::uint32_t hid, std::uintptr_t cp, const HostNew* hn) {
    wwire::Add a;
    a.hid = hid;
    if (hn) {
        a.origin = hn->origin;
        a.req = hn->req;
        a.has_pcc = hn->has_pcc;
        a.pcc = hn->pcc;
        a.rec = hn->rec;
    } else {
        a.origin = wwire::Origin::save;
        if (!ExportRecord(cp, &a.rec)) {
            Log("[world] host: hid %u: its record could not be read (not sent)", hid);
            return;
        }
    }
    const bool anim = hn && hn->anim;
    std::uint8_t buf[wwire::kMaxMessage];
    Queue(buf, wwire::EncodeAdd(Hdr(wwire::Msg::add, anim ? wwire::kFlagAnim : 0), a, buf, sizeof(buf)), 7);
}

void HostDiff() {
    std::map<std::uint32_t, CEnt> cur;
    for (const auto& kv : g_cen.synced) cur[kv.first] = g_cen.all[kv.second];
    if (!g_host_base) {
        g_prev = cur;
        g_host_base = true;
        unsigned player = 0;
        for (const CEnt& c : g_cen.all)
            if (c.net == 1) ++player;
        Log("[world] host: world ready (world %u): %zu shared structures (player %u, all %zu)", g_world, cur.size(), player, g_cen.all.size());
        // structures created before the base (a build in the first census tick) are announced by the census of the next one
        return;
    }
    // added ones and the categories they are in (an eviction is a removal in such a category in the same tick)
    std::set<int> added_cats;
    std::vector<std::uint32_t> added;
    for (const auto& kv : cur)
        if (!g_prev.count(kv.first)) {
            added.push_back(kv.first);
            added_cats.insert(kv.second.cat);
        }
    // removals first
    std::vector<std::uint32_t> manual;
    AcquireSRWLockExclusive(&g_box);
    manual.swap(g_manual_ids);
    ReleaseSRWLockExclusive(&g_box);
    for (const auto& kv : g_prev) {
        if (cur.count(kv.first)) continue;
        // still in the world but flagged, or gone: the newest look at it
        CEnt now = kv.second;
        bool flagged_now = false;
        for (const CEnt& c : g_cen.all)
            if (c.id == kv.first && c.cp == kv.second.cp) {
                now = c;
                flagged_now = true;
                break;
            }
        const bool noted = std::find(manual.begin(), manual.end(), kv.first) != manual.end();
        // the last census's durability for a ladder / rope that is gone (a picked-up one leaves the live list at once)
        const float dur = flagged_now ? now.dur : kv.second.dur;
        const bool lr = wrules::LadderOrRope(now.type);
        const wrules::Removal why = wrules::ClassifyRemoval((now.ad & 0x80) != 0, noted, dur, added_cats.count(now.cat) != 0, lr);
        if (!g_rem_sent.insert(kv.first).second) continue;
        if (lr && why == wrules::Removal::pickup && !g_picked_up.count(kv.first)) g_host_picked.insert(kv.first);
        const bool share = SharedType(now.type);
        Log("[world] host: hid %u (%s) removed (%s)%s -> %s", kv.first, wrules::TypeName(now.type), wwire::RemName(static_cast<std::uint8_t>(why)),
            flagged_now ? "" : ", gone", share ? "REM" : "not sent (the partner does not share this part)");
        if (lr && g_picked_up.erase(kv.first)) continue;  // the guest's own pick-up (REMREQ): she removed hers already
        if (share && g_guest_in_world && Peer()) {
            wwire::Rem r;
            r.hid = kv.first;
            r.type = now.type;
            std::memcpy(r.pos, now.pos, sizeof(r.pos));
            r.reason = static_cast<wwire::RemReason>(why);
            std::uint8_t buf[wwire::kMaxMessage];
            Queue(buf, wwire::EncodeRem(Hdr(wwire::Msg::rem, why == wrules::Removal::manual ? wwire::kFlagAnim : 0), r, buf, sizeof(buf)), 10);
        }
    }
    // then the new ones
    for (std::uint32_t id : added) {
        const CEnt& c = cur[id];
        HostNew hn;
        const bool known = HostNewTake(id, &hn);
        g_rem_sent.erase(id);
        const bool share = SharedType(c.type);
        Log("[world] host: new structure hid %u type %u (%s) origin %s anim %d printer %d offline %d -> %s", id, c.type, wrules::TypeName(c.type),
            known ? (hn.origin == wwire::Origin::guest ? "guest" : "host") : "other", known && hn.anim ? 1 : 0, known && hn.has_pcc ? 1 : 0,
            (c.ab & 0x80) ? 1 : 0,
            !share ? "not sent (the partner does not share this part)" : g_guest_in_world && Peer() ? "ADD" : "not sent (the guest is not in this world)");
        if (share && g_guest_in_world && Peer()) SendAdd(id, c.cp, known ? &hn : nullptr);
    }
    g_prev.swap(cur);
}

// W3 probe (world_sync 14 <id | 0 nearest ladder / rope> / 14 -1 stop): every census, a line when the watched structure's
// state or flag bytes change or it leaves the live list (how a pick-up looks from the census).
struct Watch {
    std::uint32_t id = 0;
    bool present = false;
    std::uint8_t st = 0, b[10] = {};
    float dur = 0;
};
std::vector<Watch> g_watch;
// W3 (hidden runs, world_sync 15): the game camera turned toward a structure (a rope: down its slope), checked every tick;
// a scenario turns the camera with short right-stick pulses until "[world] camera aimed #n" (as vehicle_sync 18).
struct Aim {
    bool on = false;
    double x = 0, y = 0, tol = 15;
    unsigned n = 0;
    double next_log = 0;
} g_aim;
void AimTick(game::Entity* local, double now) {
    if (!g_aim.on || !local) return;
    WorldTransform s{};
    double cf[2] = {};
    if (!game::TryGetTransform(local, &s) || !nametags::CameraForward(local, cf)) return;
    double diff = (std::atan2(g_aim.y - s.pos.y, g_aim.x - s.pos.x) - std::atan2(cf[1], cf[0])) * 57.29577951308232;
    while (diff > 180) diff -= 360;
    while (diff < -180) diff += 360;
    if (std::fabs(diff) > g_aim.tol) {
        if (now >= g_aim.next_log) g_aim.next_log = now + 1000, Log("[world] camera aim: %.1f deg off", diff);
        return;
    }
    g_aim.on = false;
    Log("[world] camera aimed #%u (%.1f deg off)", ++g_aim.n, diff);
}
void WatchTick() {
    for (Watch& w : g_watch) {
        const CEnt* hit = nullptr;
        for (const CEnt& c : g_cen.all)
            if (c.id == w.id) hit = &c;
        Watch now = w;
        now.present = hit != nullptr;
        if (hit) {
            now.st = hit->state;
            mem::Read(hit->cp + 0xA8, now.b, sizeof(now.b));
            now.dur = hit->dur;
        }
        if (now.present != w.present || now.st != w.st || std::memcmp(now.b, w.b, sizeof(w.b)) != 0)
            Log("[world] watch id %u: %s state %u flags A8..B1 %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X dur %.0f", w.id, now.present ? "live" : "GONE from the live list",
                now.st, now.b[0], now.b[1], now.b[2], now.b[3], now.b[4], now.b[5], now.b[6], now.b[7], now.b[8], now.b[9], static_cast<double>(now.dur));
        w = now;
    }
}

void HostCheck() {
    if (!g_guest_in_world || !Peer() || g_now - g_last_check < kCheckMs) return;
    g_last_check = g_now;
    wwire::Check c;
    std::uint64_t b[wwire::kBuckets];
    Buckets(b, &c.count);
    c.hash = wwire::CheckHash(b);
    std::uint8_t buf[wwire::kMaxMessage];
    Queue(buf, wwire::EncodeCheck(Hdr(wwire::Msg::check), c, buf, sizeof(buf)), 15);
}

// ---------------------------------------------------------------- W2: contributions (host)
bool ContribOn() noexcept { return g_contrib_ok && (g_peer_parts & 2u) && Peer(); }
const char* MatName(int i) noexcept {
    static const char* kNames[6] = {"crystals", "resins", "metals", "ceramics", "chemicals", "alloys"};
    return i >= 0 && i < 6 ? kNames[i] : "?";
}
void MatsStr(const std::int32_t (&m)[6], char* out, std::size_t cap) {
    std::snprintf(out, cap, "%d/%d/%d/%d/%d/%d", m[0], m[1], m[2], m[3], m[4], m[5]);
}
std::uint8_t LuOf(const CEnt& c) noexcept { return c.running ? 2 : g_cen.lvl_queued.count(c.id) ? 1 : 0; }
SKey KeyOf(const CEnt& c) noexcept {
    SKey k;
    k.level = c.level;
    k.lu = LuOf(c);
    std::uint64_t h = 14695981039346656037ull;
    auto mix = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    mix(c.own, sizeof(c.own));
    mix(c.inv, sizeof(c.inv));
    mix(c.rep, sizeof(c.rep));
    k.h = h;
    return k;
}
float DurClamp(float d) noexcept { return !std::isfinite(d) ? 0.0f : d < -1000.0f ? -1000.0f : d > 1.0e8f ? 1.0e8f : d; }
void SendState(const CEnt& c, wwire::StateWhy why, const wwire::ReqId* req = nullptr, const std::int32_t* excess = nullptr) {
    wwire::State st;
    st.hid = c.id;
    st.type = c.type;
    std::memcpy(st.pos, c.pos, sizeof(st.pos));
    st.level = static_cast<std::uint8_t>(c.level > wwire::kMaxLevelWire ? wwire::kMaxLevelWire : c.level);
    st.max_level = static_cast<std::uint8_t>(c.max_level > wwire::kMaxLevelWire ? wwire::kMaxLevelWire : c.max_level);
    st.levelup = static_cast<wwire::LevelUp>(LuOf(c));
    st.why = why;
    for (int i = 0; i < 6; ++i) {
        st.own[i] = (std::max)(0, (std::min)(c.own[i], wwire::kMaxTotal));
        st.invested[i] = (std::max)(0, (std::min)(c.inv[i], wwire::kMaxTotal));
        st.repair[i] = (std::max)(0, (std::min)(c.rep[i], wwire::kMaxTotal));
        st.excess[i] = excess ? (std::max)(0, (std::min)(excess[i], wwire::kMaxMat)) : 0;
    }
    st.dur = DurClamp(c.dur);
    if (req) st.req = *req;
    std::uint8_t buf[wwire::kMaxMessage];
    Queue(buf, wwire::EncodeState(Hdr(wwire::Msg::state), st, buf, sizeof(buf)), 13);
}
// A fresh look at one structure (after the mod changed it), as the census reads it.
bool FreshEnt(std::uintptr_t cp, CEnt& e) {
    ReadCEnt(cp, e);
    if (!ReadMats(cp, e)) return false;
    const std::uintptr_t mgr = g_cen.mgr ? g_cen.mgr : Mgr();
    if (MgrValid(mgr) && LevelUpQueued(mgr, e.id)) g_cen.lvl_queued.insert(e.id);
    return true;
}

// Every census on the host: a changed level, level-up or materials -> STATE (absolute); the queued level-ups after the
// guest's contributions.
void HostStateDiff() {
    if (!g_state_base) {
        for (const auto& kv : g_cen.contrib) g_skey[kv.first] = KeyOf(g_cen.all[kv.second]);
        g_state_base = true;
        Log("[world] host: contributions: %zu structures tracked (level, materials, durability)%s", g_cen.contrib.size(), g_contrib_ok ? "" : " (contributions OFF)");
        return;
    }
    const bool send = ContribOn() && g_guest_in_world;
    for (const auto& kv : g_cen.contrib) {
        const CEnt& c = g_cen.all[kv.second];
        const SKey k = KeyOf(c);
        auto it = g_skey.find(kv.first);
        if (it == g_skey.end()) {
            g_skey[kv.first] = k;  // a new structure: its ADD carries its numbers
            continue;
        }
        if (!(k != it->second)) continue;
        const bool lvl = k.level != it->second.level || k.lu != it->second.lu;
        it->second = k;
        g_state_dirty.insert(kv.first);
        if (!send) continue;
        SendState(c, wwire::StateWhy::change);
        if (lvl)
            Log("[world] host: hid %u (%s) level %u%s -> STATE", c.id, wrules::TypeName(c.type), c.level,
                k.lu == 2 ? " (a level-up runs)" : k.lu == 1 ? " (a level-up queued)" : "");
    }
    // after a guest contribution: the next level-up once the running one ended (materials beyond the next level count toward
    // the one after, as at the game's own terminal)
    for (auto it = g_recheck.begin(); it != g_recheck.end();) {
        auto ci = g_cen.contrib.find(*it);
        if (ci == g_cen.contrib.end()) {
            it = g_recheck.erase(it);
            continue;
        }
        const CEnt& c = g_cen.all[ci->second];
        const bool queued = g_cen.lvl_queued.count(c.id) != 0;
        if (c.running || queued) {
            ++it;
            continue;
        }
        const std::uint8_t st = GameStatus(c.cp);
        if (wrules::HostShouldQueueLevelUp(st, c.running, queued)) {
            QueueLevelUpNow(g_cen.mgr, c.cp);
            ++c_levelups_host;
            Log("[world] host: hid %u (%s) level %u: the materials reach the next level too: level-up queued", c.id, wrules::TypeName(c.type), c.level);
            ++it;
            continue;
        }
        it = g_recheck.erase(it);
    }
}

// Every 5 s: the durabilities that moved (timefall, repair) -> DUR, at most 200 per round.
void HostDur() {
    if (!ContribOn() || !g_guest_in_world || g_now - g_last_dur < kDurMs) return;
    g_last_dur = g_now;
    wwire::Dur d;
    unsigned msgs = 0;
    for (const auto& kv : g_cen.contrib) {
        const CEnt& c = g_cen.all[kv.second];
        if (!std::isfinite(c.dur)) continue;
        auto it = g_dur_sent.find(c.id);
        if (it != g_dur_sent.end() && !wrules::DurWorthSending(it->second, c.dur, c.max_dur)) continue;
        g_dur_sent[c.id] = c.dur;
        d.e.push_back({c.id, DurClamp(c.dur)});
        if (d.e.size() == wwire::kDurMax) {
            std::uint8_t buf[wwire::kMaxMessage];
            c_dur_sent += static_cast<unsigned>(d.e.size());
            Queue(buf, wwire::EncodeDur(Hdr(wwire::Msg::dur), d, buf, sizeof(buf)), 14);
            d.e.clear();
            if (++msgs >= 2) return;
        }
    }
    // W4 (the W3 gap): the shared ladders' and ropes' durability too (no levels, not in the contrib set): each world decays
    // its own copy in timefall, so the host's numbers win as for the structures
    for (const auto& kv : g_cen.synced) {
        const CEnt& c = g_cen.all[kv.second];
        if (!wrules::LadderOrRope(c.type) || !DurT(c.type) || !std::isfinite(c.dur)) continue;
        auto it = g_dur_sent.find(c.id);
        if (it != g_dur_sent.end() && !wrules::DurWorthSending(it->second, c.dur, 0.0f)) continue;
        g_dur_sent[c.id] = c.dur;
        d.e.push_back({c.id, DurClamp(c.dur)});
        ++c_dur_sent_lr;
        if (d.e.size() == wwire::kDurMax) {
            std::uint8_t buf[wwire::kMaxMessage];
            c_dur_sent += static_cast<unsigned>(d.e.size());
            Queue(buf, wwire::EncodeDur(Hdr(wwire::Msg::dur), d, buf, sizeof(buf)), 14);
            d.e.clear();
            if (++msgs >= 3) return;
        }
    }
    if (!d.e.empty()) {
        std::uint8_t buf[wwire::kMaxMessage];
        c_dur_sent += static_cast<unsigned>(d.e.size());
        Queue(buf, wwire::EncodeDur(Hdr(wwire::Msg::dur), d, buf, sizeof(buf)), 14);
    }
}

// The guest joined our world (its SNAP_REQ): the numbers that changed since this world loaded, then every durability.
void HostJoinState() {
    if (!ContribOn()) return;
    unsigned n = 0;
    for (std::uint32_t id : g_state_dirty) {
        auto ci = g_cen.contrib.find(id);
        if (ci == g_cen.contrib.end()) continue;
        SendState(g_cen.all[ci->second], wwire::StateWhy::join);
        ++n;
    }
    g_dur_sent.clear();
    g_last_dur = -1e18;
    Log("[world] host: contributions: STATE for %u structure(s) changed since this world loaded; durabilities follow (%zu tracked)", n, g_cen.contrib.size());
}

// The guest's contribution (MATS) on the host's structure: the game's own "others contributed" bucket (AddOthers: no
// telemetry, likes or stats), the game's own Status decides the level-up (QueueLevelUp: the level-up and its animation),
// then STATE (absolute) back.
void HostApplyMats(const wwire::Mats& b, std::uint16_t flags) {
    char rq[24];
    std::snprintf(rq, sizeof(rq), "#%u", b.req.n);
    const char* kind = b.kind == wwire::MatsKind::repair ? "repair" : "contribution";
    const std::uintptr_t cp = FindCp(g_cen.mgr, b.hid);
    CEnt c;
    if (g_mats_log.Seen(b.req.nonce, b.req.n)) {
        ++c_mats_retry;
        Log("[world] host: the guest's %s %s on hid %u again (retry): answered, not applied twice", kind, rq, b.hid);
        if (cp && FreshEnt(cp, c)) SendState(c, wwire::StateWhy::retry, &b.req);
        return;
    }
    const char* why = nullptr;
    if ((flags & wwire::kFlagDev) && !dev::DecideNow(devrules::Tool::structure).ok()) why = "a dev contribution and our saves are real";
    else if (!cp || !FreshEnt(cp, c)) why = "we do not have it";
    else if (!ContribT(c.type)) why = "not a shared type";
    else if (!wrules::GuardOk(c.type, c.pos, b.type, b.pos)) why = "the guard differs";
    else if (c.removing() || c.state < 1 || c.state > 2) why = "it is being removed";
    if (why) {
        ++c_mats_dropped;
        g_mats_log.Add(b.req.nonce, b.req.n);
        Log("[world] host: the guest's %s %s on hid %u DROPPED: %s%s", kind, rq, b.hid, why, cp ? " (STATE back)" : "");
        if (cp && FreshEnt(cp, c)) SendState(c, wwire::StateWhy::applied, &b.req, b.m);
        return;
    }
    char ms[64], es[64] = "0";
    std::int32_t excess[6] = {};
    const std::uint32_t lvl0 = c.level;
    std::uint8_t st = 0;
    bool queued_now = false;
    if (b.kind == wwire::MatsKind::build) {
        std::int32_t need_total[6] = {}, inv[6] = {};
        GameNeed(cp, MaxLevel(cp), need_total);
        GameInvested(cp, inv);
        const wrules::Clamp cl = wrules::ClampContribution(b.m, need_total, inv);
        if (cl.any_applied) {
            gate::ScopeMirroring scope;
            reinterpret_cast<FnAddOthers>(A(kAddOthers))(cp, At<std::uint64_t>(cp + kCpStamp), cl.applied, 1, nullptr);
        }
        std::memcpy(excess, cl.excess, sizeof(excess));
        if (cl.any_excess) ++c_excess;
        MatsStr(cl.applied, ms, sizeof(ms));
        if (cl.any_excess) MatsStr(cl.excess, es, sizeof(es));
        st = GameStatus(cp);
        const bool running = (At<std::uint8_t>(cp + kCpA8) & 0x80) != 0;
        const bool queued = LevelUpQueued(g_cen.mgr, c.id);
        if (wrules::HostShouldQueueLevelUp(st, running, queued)) {
            QueueLevelUpNow(g_cen.mgr, cp);
            queued_now = true;
            ++c_levelups_host;
        }
        g_recheck.insert(c.id);
    } else {
        // round 5: the durability only. Not AddRepair: its bucket (+0x260) is what the materials upload (0x142CB5BC0) sends
        // as absolute totals when the host itself next pays at that terminal; the guest's materials must never reach the
        // server under the host's account. The guest's own +0x260 follows the host's (STATE).
        const float secs = RepairSeconds(cp, b.m);
        {
            gate::ScopeMirroring scope;
            RepairDurability(cp, secs);
        }
        MatsStr(b.m, ms, sizeof(ms));
        std::snprintf(es, sizeof(es), "durability +%.0f s", static_cast<double>(secs));
    }
    ++c_mats_applied;
    g_mats_log.Add(b.req.nonce, b.req.n);
    FreshEnt(cp, c);
    g_skey[c.id] = KeyOf(c);
    g_state_dirty.insert(c.id);
    if (b.kind == wwire::MatsKind::build)
        Log("[world] host: the guest's %s %s on hid %u (%s): applied %s, excess %s, level %u, status %u%s", kind, rq, c.id, wrules::TypeName(c.type), ms, es, lvl0, st,
            queued_now ? ": level-up queued" : "");
    else
        Log("[world] host: the guest's %s %s on hid %u (%s): applied %s, %s, level %u", kind, rq, c.id, wrules::TypeName(c.type), ms, es, lvl0);
    SendState(c, wwire::StateWhy::applied, &b.req, excess);
}

// round 5: a request bound earlier still names that structure: same type and place (an id is reused after a reload).
std::uintptr_t ReqCp(const HostReq& r, int type, const double (&place)[3]) {
    const std::uintptr_t cp = FindCp(g_cen.mgr, r.hid);
    if (!cp) return 0;
    CEnt e;
    ReadCEnt(cp, e);
    return !e.removing() && wrules::GuardOk(e.type, e.pos, type, place) ? cp : 0;
}
void HostApplyGBuild(const wwire::GBuild& gb, std::uint16_t flags) {
    char rq[32];
    std::snprintf(rq, sizeof(rq), "#%u", gb.req.n);
    double place[3];
    gb.rec.Pos(place);
    // a known request: answer again
    for (const HostReq& r : g_host_reqs)
        if (r.req == gb.req) {
            const std::uintptr_t cp = ReqCp(r, gb.rec.Type(), place);
            Log("[world] host: the guest's build %s again (retry): answered with hid %u", rq, r.hid);
            if (cp) {
                HostNew hn;
                hn.id = r.hid;
                hn.origin = wwire::Origin::guest;
                hn.req = gb.req;
                hn.anim = false;
                hn.rec = gb.rec;
                if (!ExportRecord(cp, &hn.rec)) hn.rec = gb.rec;
                SendAdd(r.hid, cp, &hn);
            }
            return;
        }
    const wrules::PosKey key = wrules::MakePosKey(gb.rec.Type(), place);
    for (const HostReq& r : g_host_reqs)
        if (r.key == key && ReqCp(r, gb.rec.Type(), place)) {
            Log("[world] host: the guest's build %s matches hid %u (same type and place): bound, not created again", rq, r.hid);
            g_host_reqs.push_back({gb.req, r.hid, key});
            HostNew hn;
            hn.id = r.hid;
            hn.origin = wwire::Origin::guest;
            hn.req = gb.req;
            hn.anim = false;
            hn.rec = gb.rec;
            SendAdd(r.hid, ReqCp(r, gb.rec.Type(), place), &hn);
            return;
        }
    wrules::GuestBuildIn in;
    in.record_valid = wwire::RecordValid(gb.rec);
    in.host_ready = g_ready && g_host_base;
    in.type = gb.rec.Type();
    in.ladders = g_ladders_ok && g_ladders_share.load(std::memory_order_acquire);
    in.ziplines = g_zip_ok && g_zip_share.load(std::memory_order_acquire);
    std::memcpy(in.place, place, sizeof(place));
    in.has_pcc = gb.has_pcc;
    std::memcpy(in.pcc, gb.pcc.pos, sizeof(in.pcc));
    if (g_have_partner) {
        in.body[0] = g_partner.pos.x, in.body[1] = g_partner.pos.y, in.body[2] = g_partner.pos.z;
    } else {
        std::memcpy(in.body, gb.body, sizeof(in.body));
    }
    in.dev = (flags & wwire::kFlagDev) != 0;
    in.host_allows_dev = dev::DecideNow(devrules::Tool::structure).ok();
    const wrules::GuestBuild v = wrules::ValidateGuestBuild(in);
    if (v != wrules::GuestBuild::ok) {
        static const wwire::RejReason kMap[] = {wwire::RejReason::invalid, wwire::RejReason::invalid, wwire::RejReason::type, wwire::RejReason::disabled,
                                                wwire::RejReason::too_far, wwire::RejReason::not_ready, wwire::RejReason::host_real};
        const wwire::RejReason why = kMap[static_cast<int>(v)];
        Log("[world] host: the guest's build %s type %u (%s) REFUSED: %s", rq, in.type, wrules::TypeName(in.type), wwire::RejName(static_cast<std::uint8_t>(why)));
        wwire::Rej rj;
        rj.req = gb.req;
        rj.reason = why;
        std::uint8_t buf[wwire::kMaxMessage];
        Queue(buf, wwire::EncodeRej(Hdr(wwire::Msg::rej), rj, buf, sizeof(buf)), 9);
        ++c_rej;
        return;
    }
    const bool anim = (flags & wwire::kFlagAnim) != 0;
    const std::uint32_t hid = CreateStructure(gb.rec, 0, true, anim, gb.has_pcc ? &gb.pcc : nullptr);
    if (!hid) {
        Log("[world] host: the guest's build %s: the structure could not be created", rq);
        return;
    }
    ++c_created_for_guest;
    g_host_reqs.push_back({gb.req, hid, key});
    HostNew hn;
    hn.id = hid;
    hn.rec = gb.rec;
    hn.has_pcc = gb.has_pcc;
    hn.pcc = gb.pcc;
    hn.origin = wwire::Origin::guest;
    hn.req = gb.req;
    hn.anim = anim;
    hn.t = g_now;
    AcquireSRWLockExclusive(&g_box);
    g_host_new.push_back(hn);
    ReleaseSRWLockExclusive(&g_box);
    Log("[world] host: the guest's build %s: created hid %u type %u (%s) offline 1 anim %d printer %d%s", rq, hid, in.type, wrules::TypeName(in.type), anim ? 1 : 0,
        anim && gb.has_pcc && wrules::HasPrinter(in.type) ? 1 : 0, in.dev ? " (dev)" : "");
}

void HostOnMessage(const wwire::Header& h, const std::uint8_t* m, std::size_t n) {
    const auto t = static_cast<wwire::Msg>(h.type);
    if (t == wwire::Msg::snap_req) {
        wwire::Header hh;
        wwire::SnapReq b;
        if (!wwire::DecodeSnapReq(m, n, &hh, &b)) return void(++c_bad);
        if (!g_ready || !g_cen.ok || !g_host_base) {
            Log("[world] host: SNAP_REQ (guest world %u) before our world is ready: the guest asks again", b.guest_world);
            return;
        }
        if (b.guest_world != g_world) {
            Log("[world] host: SNAP_REQ names world %u, ours is %u: not answered (the guest loads ours)", b.guest_world, g_world);
            g_guest_in_world = false;
            return;
        }
        g_guest_in_world = true;
        wwire::Sum s;
        Buckets(s.buckets, &s.count);
        std::uint8_t buf[wwire::kMaxMessage];
        Queue(buf, wwire::EncodeSum(Hdr(wwire::Msg::sum), s, buf, sizeof(buf)), 3);
        Log("[world] host: SNAP_REQ (reason %u): SUM sent (%u shared structures)", b.reason, s.count);
        HostJoinState();
        return;
    }
    if (h.host_world != g_world) return void(++c_stale);
    switch (t) {
        case wwire::Msg::bucket_req: {
            wwire::Header hh;
            wwire::BucketReq b;
            if (!wwire::DecodeBucketReq(m, n, &hh, &b)) return void(++c_bad);
            const auto parts = wwire::ChunkDigest(Entries(), b.mask, ++g_digest_serial);
            std::size_t total = 0;
            for (const auto& d : parts) {
                total += d.entries.size();
                std::uint8_t buf[wwire::kMaxMessage];
                Queue(buf, wwire::EncodeDigest(Hdr(wwire::Msg::digest), d, buf, sizeof(buf)), 5);
            }
            Log("[world] host: BUCKET_REQ mask %016llX: DIGEST #%u, %zu entries in %zu part(s)", static_cast<unsigned long long>(b.mask), g_digest_serial, total,
                parts.size());
            break;
        }
        case wwire::Msg::need: {
            wwire::Header hh;
            wwire::Need b;
            if (!wwire::DecodeNeed(m, n, &hh, &b)) return void(++c_bad);
            unsigned sent = 0;
            for (std::uint32_t hid : b.hids) {
                const CEnt* c = CenById(hid);
                if (!c) continue;
                SendAdd(hid, c->cp, nullptr);
                ++sent;
            }
            Log("[world] host: NEED %zu: ADD (anim 0) for %u", b.hids.size(), sent);
            break;
        }
        case wwire::Msg::lost: {
            wwire::Header hh;
            wwire::Lost b;
            if (!wwire::DecodeLost(m, n, &hh, &b)) return void(++c_bad);
            const CEnt* c = CenById(b.hid);
            Log("[world] host: LOST hid %u from the guest: %s", b.hid, c ? "ADD again (anim 0)" : "we do not have it either");
            if (c) SendAdd(b.hid, c->cp, nullptr);
            break;
        }
        case wwire::Msg::gbuild: {
            wwire::Header hh;
            wwire::GBuild b;
            if (!wwire::DecodeGBuild(m, n, &hh, &b)) return void(++c_bad);
            Log("[world] host: GBUILD #%u (nonce %08X) type %u (%s)%s%s", b.req.n, b.req.nonce, b.rec.Type(), wrules::TypeName(b.rec.Type()),
                (h.flags & wwire::kFlagDev) ? " dev" : "", (h.flags & wwire::kFlagRetry) ? " retry" : "");
            if (!g_ready || !g_host_base) {
                g_pending_gb.push_back({b, h.flags, g_now});
                return;
            }
            HostApplyGBuild(b, h.flags);
            break;
        }
        case wwire::Msg::mats: {
            wwire::Header hh;
            wwire::Mats b;
            if (!wwire::DecodeMats(m, n, &hh, &b)) return void(++c_bad);
            if (!g_contrib_ok) {
                Log("[world] host: MATS #%u: contributions are off here: dropped", b.req.n);
                break;
            }
            HostApplyMats(b, h.flags);
            break;
        }
        case wwire::Msg::remreq: {
            wwire::Header hh;
            wwire::Rem b;
            if (!wwire::DecodeRem(m, n, &hh, &b)) return void(++c_bad);
            const CEnt* c = CenById(b.hid);
            // round 5: a dev removal (dev_build_remove on the guest) only where our own saves are sandboxed
            if ((h.flags & wwire::kFlagDev) && !dev::DecideNow(devrules::Tool::structure).ok()) {
                Log("[world] host: REMREQ hid %u (%s) is a dev removal and our saves are real: REFUSED%s", b.hid, wwire::RemName(static_cast<std::uint8_t>(b.reason)),
                    c ? ", ADD again (anim 0)" : "");
                if (c) SendAdd(b.hid, c->cp, nullptr);
                break;
            }
            if (!c && b.reason == wwire::RemReason::pickup && g_host_picked.count(b.hid)) {
                // both players picked up the same ladder / rope before either heard of the other's: one item each. Our world and
                // save are right (it was picked up once here); her extra item lives in her sandboxed copy of our save only.
                Log("[world] host: REMREQ pickup hid %u: we picked it up ourselves first (both players did at once): nothing to remove", b.hid);
                break;
            }
            if (!c || !wrules::GuardOk(c->type, c->pos, b.type, b.pos)) {
                Log("[world] host: REMREQ hid %u (%s): %s: ADD again (anim 0)", b.hid, wwire::RemName(static_cast<std::uint8_t>(b.reason)),
                    c ? "the guard differs" : "we do not have it");
                if (c) SendAdd(b.hid, c->cp, nullptr);
                break;
            }
            if (wrules::LadderOrRope(c->type) || b.reason == wwire::RemReason::pickup) {
                // W3: she picked up her copy of a ladder / rope (the game gave HER the item back). Ours goes with the game's
                // removal bits, never RemoveManually or a pick-up: the host gets no item. No REM back (hers is gone).
                {
                    gate::ScopeMirroring scope;
                    gate::NoteModCp(c->cp);
                    RemoveBits(g_cen.mgr, c->cp);
                }
                g_picked_up.insert(b.hid);
                ++c_rem_applied;
                c_pickups_applied.fetch_add(1);
                Log("[world] host: the guest picked up hid %u (%s): removed here too (removal bits, no item)", b.hid, wrules::TypeName(c->type));
                break;
            }
            Dismantle(c->cp);
            ++c_rem_applied;
            Log("[world] host: the guest dismantled hid %u (%s): removed here too (RemoveManually)", b.hid, wrules::TypeName(c->type));
            AcquireSRWLockExclusive(&g_box);
            g_manual_ids.push_back(b.hid);
            ReleaseSRWLockExclusive(&g_box);
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------- guest
void SendSnapReq(std::uint8_t reason) {
    wwire::SnapReq b;
    b.guest_world = g_world;
    b.reason = reason;
    g_snap_sent_reason = reason;
    std::uint8_t buf[wwire::kMaxMessage];
    Queue(buf, wwire::EncodeSnapReq(Hdr(wwire::Msg::snap_req), b, buf, sizeof(buf)), 2);
    g_snap_asked = g_now;
    g_snap_waiting = true;
    g_asm.Reset();
}

bool MirrorPending(std::uint32_t hid) {
    auto it = g_mirror_pending.find(hid);
    if (it == g_mirror_pending.end()) return false;
    if (g_now - it->second > kMirrorPendingMs) {
        g_mirror_pending.erase(it);
        return false;
    }
    return true;
}

// The guest mirrors a host structure (idempotent). Returns its local id (0 = not created).
std::uint32_t GuestMirror(std::uint32_t hid, const wwire::Record& rec, bool anim, const wwire::Xf* pcc, const char* why) {
    const std::uintptr_t mgr = g_cen.mgr ? g_cen.mgr : Mgr();
    if (!MgrValid(mgr)) return 0;
    double hp[3];
    rec.Pos(hp);
    // already here?
    const std::uint32_t local0 = LocalOf(hid);
    if (const std::uintptr_t cp = FindCp(mgr, local0)) {
        CEnt e;
        ReadCEnt(cp, e);
        if (!e.removing() && wrules::GuardOk(e.type, e.pos, rec.Type(), hp)) {
            g_bound.insert(hid);
            return local0;
        }
        if (local0 == hid && !e.removing()) {
            // the same id is something else here: it goes, the host's comes under a mapped id
            Log("[world] guest: id %u here is type %u at (%.1f, %.1f, %.1f), the host's is type %u: ours removed", hid, e.type, e.pos[0], e.pos[1], e.pos[2],
                rec.Type());
            RemoveBits(mgr, cp);
        }
    }
    if (MirrorPending(hid)) return LocalOf(hid);
    std::uint32_t want = 0;
    bool exact = false;
    if (!FindCp(mgr, hid) && !g_g2h.count(hid)) {
        want = hid;
        exact = true;
    }
    const std::uint32_t id = CreateStructure(rec, want, true, anim, pcc);
    if (!id) return 0;
    if (exact) {
        RaiseCounter(mgr, hid);
        ++c_mirror_exact;
    } else {
        g_h2g[hid] = id;
        g_g2h[id] = hid;
        ++c_mirror_mapped;
    }
    ++c_mirrors;
    g_mirror_pending[hid] = g_now;
    g_bound.insert(hid);
    g_rem_got.erase(hid);
    g_remreq_sent.erase(hid);  // round 5: the host kept it (a refused REMREQ): a later dismantle of it is sent again
    g_remreq_pend.erase(hid);
    g_verify.push_back({hid, id, rec.Type(), g_now});
    Log("[world] guest: mirror hid %u -> gid %u (%s) type %u (%s) anim %d printer %d (%s)", hid, id, exact ? "exact" : "mapped", rec.Type(),
        wrules::TypeName(rec.Type()), anim ? 1 : 0, anim && pcc && wrules::HasPrinter(rec.Type()) ? 1 : 0, why);
    return id;
}

void GuestVerify() {
    for (auto it = g_verify.begin(); it != g_verify.end();) {
        if (g_now - it->t < kVerifyMs) {
            ++it;
            continue;
        }
        const std::uintptr_t cp = FindCp(g_cen.mgr, it->local);
        if (cp) {
            CEnt e;
            ReadCEnt(cp, e);
            Log("[world] guest: mirror hid %u ok: gid %u state %u level %u offline %d server id %s", it->hid, it->local, e.state, e.level, (e.ab & 0x80) ? 1 : 0,
                ServerIdEmpty(cp) ? "empty" : "SET");
            g_mirror_pending.erase(it->hid);
        } else {
            ++c_mirror_fail;
            Log("[world] guest: mirror FAIL hid %u: gid %u not in the world %.0f s after the queue", it->hid, it->local, kVerifyMs / 1000);
        }
        it = g_verify.erase(it);
    }
}

void RaiseGuestLimits() {
    if (g_limits_raised) return;
    const std::uintptr_t mgr = g_cen.mgr;
    const std::uintptr_t mcfg = mem::Q(mgr + kMgrCfg);
    if (!mcfg) return;
    unsigned raised = 0;
    char note[200] = {};
    std::size_t k = 0;
    for (int cat = 0; cat < 18; ++cat) {
        if (!wrules::SyncedCategory(cat, g_ladders_ok, g_zip_ok)) continue;
        const std::uintptr_t cfg = mem::Q(mcfg + 0x40 + static_cast<std::uintptr_t>(cat) * 8);
        const char* rn = cfg ? mem::RttiName(mem::RttiOf(cfg)) : nullptr;
        if (!rn || !std::strstr(rn, "ConstructionPointConfig")) continue;
        std::uint32_t lim = 0;
        if (!mem::Rd(cfg + kCfgLimitPlayer, lim) || lim > 100000) continue;
        *reinterpret_cast<std::uint32_t*>(cfg + kCfgLimitPlayer) = lim + 256;
        ++raised;
        if (k < sizeof(note) - 16) k += static_cast<std::size_t>(std::snprintf(note + k, sizeof(note) - k, "%s%d:%u", k ? " " : "", cat, lim));
    }
    g_limits_raised = true;
    Log("[world] guest: own evictions off: %u category limits raised by 256 in memory (was %s); only the host's evictions (REM evicted) remove", raised, note);
}

// A snapshot completed: the REMREQs sent in this link session before the SNAP_REQ it answers reached the host (one ordered
// channel); the heal backs off while heals change nothing.
void SnapshotDone(bool changed) {
    for (auto it = g_remreq_pend.begin(); it != g_remreq_pend.end();) {
        if (it->second.gen == g_gen && it->second.t <= g_snap_asked) it = g_remreq_pend.erase(it);
        else ++it;
    }
    if (g_snap_sent_reason == 1 && !changed) ++g_heal_idle;
    else g_heal_idle = 0;
    g_snap_reason = 0;
}

void GuestBindDigest() {
    const std::uint64_t mask = g_asm.Mask();
    std::map<std::uint32_t, const wwire::Entry*> host;
    for (const wwire::Entry& e : g_asm.Entries()) host[e.hid] = &e;
    unsigned bound = 0, need = 0, removed = 0, replaced = 0, given_up = 0;
    std::vector<std::uint32_t> needs;
    for (const auto& kv : host) {
        const wwire::Entry& e = *kv.second;
        const std::uint32_t local = LocalOf(e.hid);
        const CEnt* c = CenById(local);
        if (c && g_role == 2 && HidOf(c->id) != e.hid) c = nullptr;
        const wrules::Bind b = wrules::BindEntry(c != nullptr, c ? c->type : -1, c ? c->pos : e.pos, e.type, e.pos);
        if (b == wrules::Bind::bound) {
            g_bound.insert(e.hid);
            ++bound;
        } else {
            if (b == wrules::Bind::replace) {
                RemoveBits(g_cen.mgr, c->cp);
                ++replaced;
            }
            if (g_lost_given.count(e.hid) && b != wrules::Bind::replace) {
                ++given_up;  // her world keeps removing it (LOST given up): not asked for again in this world
                continue;
            }
            if (!MirrorPending(e.hid)) needs.push_back(e.hid);
            ++need;
        }
    }
    // ours in those buckets that the host does not have (not a build of our own the host never heard of)
    for (const auto& kv : g_cen.synced) {
        const CEnt& c = g_cen.all[kv.second];
        const std::uint32_t hid = HidOf(c.id);
        if (!SharedType(c.type)) continue;  // ladders / ropes while the host does not share them: ours stay
        if (!((mask >> wwire::BucketOf(hid)) & 1u) || host.count(hid) || MirrorPending(hid)) continue;
        if (std::find(g_local_only.begin(), g_local_only.end(), c.id) != g_local_only.end()) continue;
        RemoveBits(g_cen.mgr, c.cp);
        g_bound.erase(hid);
        ++removed;
        Log("[world] guest: hid %u (%s) is not in the host's world: removed here", hid, wrules::TypeName(c.type));
    }
    for (std::size_t i = 0; i < needs.size(); i += wwire::kNeedMax) {
        wwire::Need nd;
        nd.hids.assign(needs.begin() + static_cast<std::ptrdiff_t>(i), needs.begin() + static_cast<std::ptrdiff_t>((std::min)(needs.size(), i + wwire::kNeedMax)));
        std::uint8_t buf[wwire::kMaxMessage];
        Queue(buf, wwire::EncodeNeed(Hdr(wwire::Msg::need), nd, buf, sizeof(buf)), 6);
    }
    ++g_snaps;
    g_snap_ok = true;
    g_snap_waiting = false;
    Log("[world] guest: snapshot #%u complete: bound %u, created %u, removed %u, replaced %u, given up %u (buckets %016llX)", g_snaps, bound, need, removed, replaced,
        given_up, static_cast<unsigned long long>(mask));
    SnapshotDone(need != 0 || removed != 0 || replaced != 0);
    RaiseGuestLimits();
}

// ---------------------------------------------------------------- W2: contributions (guest)
bool MatsPendingFor(std::uint32_t hid) noexcept {
    for (const GMats& g : g_mats_pending)
        if (g.m.hid == hid) return true;
    return false;
}
// The host's numbers for one structure, written absolute (own, others so that Invested equals the host's, repair,
// durability); her level-up follows the host's (QueueLevelUp: the game's own level-up and animation). false: not here yet.
bool GuestApplyState(const wwire::State& st) {
    if (MirrorPending(st.hid)) return false;
    const std::uint32_t local = LocalOf(st.hid);
    const std::uintptr_t cp = FindCp(g_cen.mgr, local);
    if (!cp) return false;
    CEnt e;
    if (!FreshEnt(cp, e)) return false;
    if (!wrules::GuardOk(e.type, e.pos, st.type, st.pos)) {
        Log("[world] guest: STATE hid %u: the guard differs (ours type %u at %.1f, %.1f): ignored", st.hid, e.type, e.pos[0], e.pos[1]);
        return true;
    }
    // our own contribution is on its way: the host's answer (applied) writes the numbers
    bool ours = false;
    for (auto it = g_mats_pending.begin(); it != g_mats_pending.end(); ++it)
        if (st.req.nonce == g_nonce && it->m.req == st.req) {
            ours = true;
            const bool any_excess = wwire::MatsValid(st.excess, wwire::kMaxMat, true);
            char ex[64];
            MatsStr(st.excess, ex, sizeof(ex));
            Log("[world] guest: our %s #%u on hid %u answered by the host (%s): level %u%s%s%s", it->m.kind == wwire::MatsKind::repair ? "repair" : "contribution",
                it->m.req.n, st.hid, st.why == wwire::StateWhy::retry ? "retry" : "applied", st.level,
                st.levelup == wwire::LevelUp::running ? ", a level-up runs" : st.levelup == wwire::LevelUp::queued ? ", a level-up queued" : "",
                any_excess ? "; NOT needed (the structure was ahead): " : "", any_excess ? ex : "");
            if (any_excess) ++c_excess;
            g_mats_pending.erase(it);
            break;
        }
    if (!ours && MatsPendingFor(st.hid)) return true;  // a newer STATE comes with the answer
    if (std::memcmp(e.inv, st.invested, sizeof(e.inv)) != 0 || std::fabs(e.dur - st.dur) > 0.02f * (std::fabs(st.dur) + 1.0f)) {
        char a[64], b[64];
        MatsStr(e.inv, a, sizeof(a));
        MatsStr(st.invested, b, sizeof(b));
        Log("[world] guest: STATE hid %u (%s): invested %s -> %s, durability %.0f -> %.0f s, level %u (host %u)", st.hid, wrules::TypeName(e.type), a, b,
            static_cast<double>(e.dur), static_cast<double>(st.dur), e.level, st.level);
    }
    std::int32_t others[6];
    wrules::OthersFor(st.invested, st.own, e.list, others);
    {
        auto* cs = reinterpret_cast<CRITICAL_SECTION*>(g_cen.mgr + kMgrCs);
        EnterCriticalSection(cs);
        std::memcpy(reinterpret_cast<void*>(cp + kCpOwn), st.own, sizeof(st.own));
        std::memcpy(reinterpret_cast<void*>(cp + kCpOthers), others, sizeof(others));
        std::memcpy(reinterpret_cast<void*>(cp + kCpRepair), st.repair, sizeof(st.repair));
        if (std::fabs(e.dur - st.dur) > 1.0f) *reinterpret_cast<float*>(cp + kCpDur) = st.dur;
        LeaveCriticalSection(cs);
    }
    ++c_state_applied;
    const bool queued = LevelUpQueued(g_cen.mgr, e.id);
    const std::int32_t reached = GameReached(cp);
    if (wrules::GuestShouldQueueLevelUp(st.level, static_cast<int>(st.levelup), static_cast<int>(e.level), e.running, queued, reached)) {
        QueueLevelUpNow(g_cen.mgr, cp);
        ++c_levelups_guest;
        Log("[world] guest: hid %u (%s): the host's level %u%s; ours %u -> level-up queued (the materials reach %d)", st.hid, wrules::TypeName(e.type), st.level,
            st.levelup == wwire::LevelUp::running ? " (its level-up runs)" : st.levelup == wwire::LevelUp::queued ? " (its level-up queued)" : "", e.level, reached);
    } else if (st.level != e.level && !e.running && !queued) {
        Log("[world] guest: hid %u (%s): the host's level %u, ours %u (the materials reach %d): no level-up queued", st.hid, wrules::TypeName(e.type), st.level, e.level,
            reached);
    }
    return true;
}

void GuestStateMsg(const wwire::State& st) {
    if (GuestApplyState(st)) {
        g_state_pending.erase(st.hid);
        return;
    }
    g_state_pending[st.hid] = {st, g_now};
}

void GuestDurMsg(const wwire::Dur& d) {
    unsigned n = 0, moved = 0;
    std::uint32_t ex_hid = 0;
    float ex_from = 0, ex_to = 0;
    for (const wwire::DurEnt& x : d.e) {
        const std::uintptr_t cp = FindCp(g_cen.mgr, LocalOf(x.hid));
        if (!cp) continue;
        std::uint8_t type = 0;
        float cur = 0;
        if (!mem::Rd(cp + kCpType, type) || !DurT(type) || !mem::Rd(cp + kCpDur, cur)) continue;
        if (std::fabs(cur - x.dur) > 0.02f * (std::fabs(x.dur) + 1.0f)) {
            if (!moved || std::fabs(cur - x.dur) > std::fabs(ex_from - ex_to)) ex_hid = x.hid, ex_from = cur, ex_to = x.dur;
            ++moved;
        }
        *reinterpret_cast<float*>(cp + kCpDur) = x.dur;
        ++n;
    }
    c_dur_applied += n;
    if (moved)
        Log("[world] guest: DUR %zu: %u applied, %u moved more than 2 %% (the most: hid %u %.0f -> %.0f s)", d.e.size(), n, moved, ex_hid, static_cast<double>(ex_from),
            static_cast<double>(ex_to));
}

// Our local contributions (the detours' inbox) -> MATS (pending until the host's STATE answers it; sent again after a new
// link session with the retry flag; the host answers a known request without applying it twice).
void GuestMatsTick() {
    std::vector<MatsIn> in;
    AcquireSRWLockExclusive(&g_box);
    in.swap(g_mats_in);
    ReleaseSRWLockExclusive(&g_box);
    for (const MatsIn& x : in) {
        GMats g;
        g.m.req.nonce = g_nonce;
        AcquireSRWLockExclusive(&g_box);
        g.m.req.n = ++g_req_n;
        ReleaseSRWLockExclusive(&g_box);
        g.m.hid = HidOf(x.local);
        g.m.type = x.type;
        std::memcpy(g.m.pos, x.pos, sizeof(g.m.pos));
        g.m.kind = x.kind;
        std::memcpy(g.m.m, x.m, sizeof(g.m.m));
        g.flags = x.dev ? wwire::kFlagDev : 0;
        char ms[64];
        MatsStr(x.m, ms, sizeof(ms));
        Log("[world] guest: our %s on hid %u (%s) %s -> MATS #%u", x.kind == wwire::MatsKind::repair ? "repair" : "contribution", g.m.hid, wrules::TypeName(x.type), ms,
            g.m.req.n);
        g_mats_pending.push_back(g);
    }
    for (auto it = g_mats_pending.begin(); it != g_mats_pending.end();) {
        if (it->sent && it->sent_gen == g_gen && g_now - it->sent_t > kMatsAnswerMs) {
            Log("[world] guest: MATS #%u on hid %u: no answer from the host in %.0f s: given up (its numbers win)", it->m.req.n, it->m.hid, kMatsAnswerMs / 1000);
            it = g_mats_pending.erase(it);
        } else {
            ++it;
        }
    }
    if (g_hold_sends || !ContribOn() || !g_snap_ok) return;
    for (GMats& g : g_mats_pending) {
        if (g.sent && g.sent_gen == g_gen) continue;
        const std::uint16_t flags = static_cast<std::uint16_t>(g.flags | (g.sent ? wwire::kFlagRetry : 0));
        std::uint8_t buf[wwire::kMaxMessage];
        Queue(buf, wwire::EncodeMats(Hdr(wwire::Msg::mats, flags), g.m, buf, sizeof(buf)), 12);
        if (g.sent) Log("[world] guest: MATS #%u sent again (retry)", g.m.req.n);
        g.sent = true;
        g.sent_gen = g_gen;
        g.sent_t = g_now;
    }
}

void GuestStatePendingTick() {
    for (auto it = g_state_pending.begin(); it != g_state_pending.end();) {
        if (GuestApplyState(it->second.st)) {
            it = g_state_pending.erase(it);
        } else if (g_now - it->second.t > kStatePendingMs) {
            Log("[world] guest: STATE hid %u: not in our world after %.0f s: dropped (the join check heals)", it->first, kStatePendingMs / 1000);
            it = g_state_pending.erase(it);
        } else {
            ++it;
        }
    }
}

void GuestOnMessage(const wwire::Header& h, const std::uint8_t* m, std::size_t n) {
    if (h.host_world != g_world) return void(++c_stale);
    const auto t = static_cast<wwire::Msg>(h.type);
    switch (t) {
        case wwire::Msg::sum: {
            wwire::Header hh;
            wwire::Sum b;
            if (!wwire::DecodeSum(m, n, &hh, &b)) return void(++c_bad);
            std::uint64_t mine[wwire::kBuckets];
            std::uint32_t count = 0;
            Buckets(mine, &count);
            std::uint64_t mask = 0;
            for (int i = 0; i < wwire::kBuckets; ++i)
                if (mine[i] != b.buckets[i]) mask |= 1ull << i;
            if (!mask) {
                // everything equal: every shared structure is bound
                for (const auto& kv : g_cen.synced)
                    if (SharedType(g_cen.all[kv.second].type)) g_bound.insert(HidOf(kv.first));
                ++g_snaps;
                g_snap_ok = true;
                g_snap_waiting = false;
                Log("[world] guest: snapshot #%u complete: bound %u, created 0, removed 0, replaced 0 (every bucket equal)", g_snaps, count);
                SnapshotDone(true);
                RaiseGuestLimits();
            } else {
                wwire::BucketReq br;
                br.mask = mask;
                std::uint8_t buf[wwire::kMaxMessage];
                Queue(buf, wwire::EncodeBucketReq(Hdr(wwire::Msg::bucket_req), br, buf, sizeof(buf)), 4);
                Log("[world] guest: SUM: host %u, ours %u; buckets differ %016llX: BUCKET_REQ", b.count, count, static_cast<unsigned long long>(mask));
            }
            break;
        }
        case wwire::Msg::digest: {
            wwire::Header hh;
            wwire::Digest b;
            if (!wwire::DecodeDigest(m, n, &hh, &b)) return void(++c_bad);
            g_asm.Add(b);
            if (g_asm.Complete()) {
                GuestBindDigest();
                g_asm.Reset();
            }
            break;
        }
        case wwire::Msg::add: {
            wwire::Header hh;
            wwire::Add b;
            if (!wwire::DecodeAdd(m, n, &hh, &b)) return void(++c_bad);
            if (!SharedType(b.rec.Type())) {
                Log("[world] guest: ADD hid %u type %u (%s) is not a shared type here: ignored", b.hid, b.rec.Type(), wrules::TypeName(b.rec.Type()));
                break;
            }
            bool mine = false;
            Hold hold;
            if (b.origin == wwire::Origin::guest && b.req.nonce == g_nonce) {
                AcquireSRWLockExclusive(&g_box);
                for (auto it = g_holds.begin(); it != g_holds.end(); ++it)
                    if (it->n == b.req.n) {
                        hold = *it;
                        g_holds.erase(it);
                        mine = true;
                        break;
                    }
                ReleaseSRWLockExclusive(&g_box);
            }
            const bool anim = (h.flags & wwire::kFlagAnim) != 0;
            const wwire::Xf* pcc = mine && hold.has_pcc ? &hold.pcc : b.has_pcc ? &b.pcc : nullptr;
            const std::uint32_t id = GuestMirror(b.hid, b.rec, anim, pcc, b.origin == wwire::Origin::guest ? (mine ? "our build" : "a build of ours") : b.origin == wwire::Origin::host ? "the host's build" : "the host's world");
            if (mine) Log("[world] guest: build #%u confirmed as hid %u (gid %u)", b.req.n, b.hid, id);
            break;
        }
        case wwire::Msg::rej: {
            wwire::Header hh;
            wwire::Rej b;
            if (!wwire::DecodeRej(m, n, &hh, &b)) return void(++c_bad);
            bool had = false;
            AcquireSRWLockExclusive(&g_box);
            for (auto it = g_holds.begin(); it != g_holds.end(); ++it)
                if (b.req.nonce == g_nonce && it->n == b.req.n) {
                    g_holds.erase(it);
                    had = true;
                    break;
                }
            ReleaseSRWLockExclusive(&g_box);
            Log("[world] guest: build #%u REFUSED by the host: %s%s", b.req.n, wwire::RejName(static_cast<std::uint8_t>(b.reason)), had ? "" : " (not pending)");
            break;
        }
        case wwire::Msg::rem: {
            wwire::Header hh;
            wwire::Rem b;
            if (!wwire::DecodeRem(m, n, &hh, &b)) return void(++c_bad);
            g_rem_got.insert(b.hid);
            g_bound.erase(b.hid);
            g_mirror_pending.erase(b.hid);
            const std::uint32_t local = LocalOf(b.hid);
            const std::uintptr_t cp = FindCp(g_cen.mgr, local);
            CEnt e;
            if (cp) ReadCEnt(cp, e);
            if (!cp || e.removing()) {
                if (!cp && b.reason == wwire::RemReason::pickup && g_remreq_sent.count(b.hid))
                    Log("[world] guest: REM pickup hid %u: the host picked it up while we did (both at once): one item each; the host's save counts its own "
                        "pick-up only, our extra item lives in this sandboxed session", b.hid);
                else
                    Log("[world] guest: REM hid %u (%s): %s", b.hid, wwire::RemName(static_cast<std::uint8_t>(b.reason)), cp ? "already being removed" : "not here");
                g_remreq_pend.erase(b.hid);
                break;
            }
            if (!wrules::GuardOk(e.type, e.pos, b.type, b.pos)) {
                Log("[world] guest: REM hid %u: the guard differs (ours type %u at %.1f, %.1f): kept", b.hid, e.type, e.pos[0], e.pos[1]);
                break;
            }
            const bool anim = (h.flags & wwire::kFlagAnim) != 0 && b.reason == wwire::RemReason::manual && g_remove_ok;
            if (anim) Dismantle(cp);
            else RemoveBits(g_cen.mgr, cp);
            ++c_rem_applied;
            Log("[world] guest: REM hid %u (%s) %s: removed gid %u (%s)", b.hid, wrules::TypeName(e.type), wwire::RemName(static_cast<std::uint8_t>(b.reason)), local,
                anim ? "dismantle animation" : "removal bits");
            break;
        }
        case wwire::Msg::state: {
            wwire::Header hh;
            wwire::State b;
            if (!wwire::DecodeState(m, n, &hh, &b)) return void(++c_bad);
            if (g_contrib_ok) GuestStateMsg(b);
            break;
        }
        case wwire::Msg::dur: {
            wwire::Header hh;
            wwire::Dur b;
            if (!wwire::DecodeDur(m, n, &hh, &b)) return void(++c_bad);
            if (g_contrib_ok) GuestDurMsg(b);
            break;
        }
        case wwire::Msg::check: {
            wwire::Header hh;
            wwire::Check b;
            if (!wwire::DecodeCheck(m, n, &hh, &b)) return void(++c_bad);
            if (!g_snap_ok || g_snap_waiting) break;
            std::uint64_t mine[wwire::kBuckets];
            std::uint32_t count = 0;
            Buckets(mine, &count);
            if (wwire::CheckHash(mine) == b.hash) {
                g_check_miss = 0;
                break;
            }
            ++g_check_miss;
            if (g_check_miss >= 2 && g_now - g_last_heal > wrules::HealGapMs(kHealGapMs, g_heal_idle) && g_mirror_pending.empty()) {
                g_last_heal = g_now;
                g_check_miss = 0;
                Log("[world] guest: CHECK differs twice (host %u, ours %u): heal (SNAP_REQ)", b.count, count);
                SendSnapReq(1);
            }
            break;
        }
        default:
            break;
    }
}

void QueueRemReq(const wwire::Rem& r, std::uint16_t flags) {
    std::uint8_t buf[wwire::kMaxMessage];
    Queue(buf, wwire::EncodeRem(Hdr(wwire::Msg::remreq, flags), r, buf, sizeof(buf)), 11);
    PendRem& p = g_remreq_pend[r.hid];
    p.r = r;
    p.flags = static_cast<std::uint16_t>(flags & ~wwire::kFlagRetry);
    p.gen = g_gen;
    p.t = g_now;
    if (g_remreq_pend.size() > 128) g_remreq_pend.erase(g_remreq_pend.begin());
}

void GuestTick() {
    // round 5: the REMREQs a dropped link session may have lost, again (before the SNAP_REQ: the host applies them first)
    if (Peer())
        for (auto& kv : g_remreq_pend) {
            if (kv.second.gen == g_gen) continue;
            const wwire::Rem r = kv.second.r;
            QueueRemReq(r, static_cast<std::uint16_t>(kv.second.flags | wwire::kFlagRetry));
            Log("[world] guest: REMREQ hid %u (%s) sent again after a new link session", r.hid, wwire::RemName(static_cast<std::uint8_t>(r.reason)));
        }
    // join: HELLO, then the snapshot (reason 2 after a new link session in the same world)
    if (!g_snap_ok && !g_snap_waiting && Peer() && g_now - g_ready_since > kSettleMs && g_now - g_snap_asked > kSnapRetryMs) SendSnapReq(g_snap_reason);
    if (g_snap_waiting && g_now - g_snap_asked > kSnapRetryMs) g_snap_waiting = false;  // asked again next tick
    // the held builds
    std::vector<Hold> to_send;
    AcquireSRWLockExclusive(&g_box);
    for (Hold& h : g_holds) {
        if (h.t == 0) h.t = g_now;
        if (!h.sent || h.sent_gen != g_gen) to_send.push_back(h);
    }
    ReleaseSRWLockExclusive(&g_box);
    if (!g_hold_sends && g_snap_ok && Peer())
        for (const Hold& h : to_send) {
            wwire::GBuild gb;
            gb.req.nonce = g_nonce;
            gb.req.n = h.n;
            gb.rec = h.rec;
            gb.has_pcc = h.has_pcc;
            gb.pcc = h.pcc;
            WorldTransform sam{};
            if (LocalSam(&sam)) gb.body[0] = sam.pos.x, gb.body[1] = sam.pos.y, gb.body[2] = sam.pos.z;
            else h.rec.Pos(gb.body);
            const std::uint16_t flags = static_cast<std::uint16_t>((h.dev ? wwire::kFlagDev : 0) | (h.anim ? wwire::kFlagAnim : 0) | (h.sent ? wwire::kFlagRetry : 0));
            std::uint8_t buf[wwire::kMaxMessage];
            Queue(buf, wwire::EncodeGBuild(Hdr(wwire::Msg::gbuild, flags), gb, buf, sizeof(buf)), 8);
            Log("[world] guest: local build type %u (%s) held, request #%u %s", h.rec.Type(), wrules::TypeName(h.rec.Type()), h.n, h.sent ? "sent again (retry)" : "sent");
            AcquireSRWLockExclusive(&g_box);
            for (Hold& x : g_holds)
                if (x.n == h.n) x.sent = true, x.sent_gen = g_gen;
            ReleaseSRWLockExclusive(&g_box);
        }
    // dismantles of bound structures -> REMREQ
    std::vector<std::uint32_t> manual;
    AcquireSRWLockExclusive(&g_box);
    manual.swap(g_manual_ids);
    ReleaseSRWLockExclusive(&g_box);
    for (std::uint32_t local : manual) {
        AcquireSRWLockExclusive(&g_box);
        const bool dev = g_dev_manual.erase(local) != 0;
        ReleaseSRWLockExclusive(&g_box);
        const std::uint32_t hid = HidOf(local);
        if (!g_bound.count(hid) || !g_remreq_sent.insert(hid).second) continue;
        const std::uintptr_t cp = FindCp(g_cen.mgr, local);
        if (!cp) continue;
        CEnt e;
        ReadCEnt(cp, e);
        wwire::Rem r;
        r.hid = hid;
        r.type = e.type;
        std::memcpy(r.pos, e.pos, sizeof(r.pos));
        r.reason = wwire::RemReason::manual;
        g_bound.erase(hid);
        QueueRemReq(r, dev ? wwire::kFlagDev : 0);
        Log("[world] guest: we dismantled hid %u (%s) -> REMREQ%s", hid, wrules::TypeName(e.type), dev ? " (dev: the host decides)" : "");
    }
    // bound structures that vanished without a REM -> LOST; W3: a ladder / rope she picked up (durability left) -> REMREQ pickup.
    // Round 5: a pick-up removes the host's REAL ladder / rope, so it needs the circumstances of one (wrules::PickupPlausible:
    // a world in the census, at most two such vanishes in this tick, her body next to it); never on an empty live list.
    unsigned lr_cand = 0;
    WorldTransform body{};
    const bool have_body = g_snap_ok && !g_cen.all.empty() && LocalSam(&body);
    if (g_snap_ok && !g_cen.all.empty())
        for (std::uint32_t hid : g_bound) {
            if (CenById(LocalOf(hid)) || g_rem_got.count(hid) || g_remreq_sent.count(hid) || MirrorPending(hid)) continue;
            const auto ls = g_last_seen.find(hid);
            if (ls != g_last_seen.end() && wrules::GuestVanished(ls->second.type, ls->second.dur) == wrules::Vanished::pickup) ++lr_cand;
        }
    if (g_snap_ok && !g_cen.all.empty())
        for (auto it = g_bound.begin(); it != g_bound.end();) {
            const std::uint32_t hid = *it;
            if (const CEnt* c = CenById(LocalOf(hid))) {
                g_last_seen[hid] = {c->type, c->dur, c->pos[0], c->pos[1], c->pos[2]};
                ++it;
                continue;
            }
            if (MirrorPending(hid) || g_rem_got.count(hid) || g_remreq_sent.count(hid)) {
                ++it;
                continue;
            }
            const auto ls = g_last_seen.find(hid);
            std::uintptr_t by = 0;
            AcquireSRWLockExclusive(&g_box);
            if (auto rb = g_removed_by.find(LocalOf(hid)); rb != g_removed_by.end()) by = rb->second, g_removed_by.erase(rb);
            ReleaseSRWLockExclusive(&g_box);
            bool automatic = by == kDevRemoveMark;
            for (std::uintptr_t r : kAutoRemoveReturns)
                if (by == r) automatic = true;
            // a pick-up leaves the live list without the removal bits (watch, run 20261121-174141); a construction point still
            // listed with them (+0xAA bit 0x40) is being removed by the game (RemoveConstructionsInArea, a collapse ...): not hers
            for (const CEnt& c : g_cen.all)
                if (c.id == LocalOf(hid) && (c.aa & 0x40) && !automatic) {  // not state 3 alone: a rope's recovery passes through state 3 (run 20261121-184858)
                    automatic = true;
                    Log("[world] guest: hid %u carries the game's removal bits (aa %02X state %u): the game's removal, not a pick-up", hid, c.aa, c.state);
                }
            if (automatic) Log("[world] guest: hid %u was removed by the game itself (Remove from 0x%llX: a placement re-check), not picked up", hid,
                               static_cast<unsigned long long>(by));
            bool pickup = !automatic && ls != g_last_seen.end() && wrules::GuestVanished(ls->second.type, ls->second.dur) == wrules::Vanished::pickup;
            if (pickup) {
                wrules::PickupIn pin;
                pin.census_has_world = true;
                pin.vanished_lr = lr_cand;
                pin.have_body = have_body;
                if (have_body) {
                    const double at[3] = {ls->second.x, ls->second.y, ls->second.z};
                    const double me[3] = {body.pos.x, body.pos.y, body.pos.z};
                    pin.dist_m = wrules::Dist(at, me);
                }
                if (!wrules::PickupPlausible(pin)) {
                    pickup = false;
                    char why[96];
                    if (!pin.have_body) std::snprintf(why, sizeof(why), "her body's place is unknown");
                    else if (pin.vanished_lr > wrules::kPickupsPerTick) std::snprintf(why, sizeof(why), "%u ladders / ropes went at once", pin.vanished_lr);
                    else std::snprintf(why, sizeof(why), "%.0f m from her body", pin.dist_m);
                    Log("[world] guest: hid %u (%s) vanished but not as her pick-up (%s): LOST, the host keeps it", hid, wrules::TypeName(ls->second.type), why);
                }
            }
            if (pickup) {
                wwire::Rem r;
                r.hid = hid;
                r.type = ls->second.type;
                r.pos[0] = ls->second.x, r.pos[1] = ls->second.y, r.pos[2] = ls->second.z;
                r.reason = wwire::RemReason::pickup;
                g_remreq_sent.insert(hid);
                QueueRemReq(r, 0);
                c_pickups_sent.fetch_add(1);
                Log("[world] guest: we picked up hid %u (%s, durability %.0f s left%s) -> REMREQ pickup", hid, wrules::TypeName(r.type), static_cast<double>(ls->second.dur),
                    by ? ", the game's Remove was called" : "");
                g_last_seen.erase(ls);
                it = g_bound.erase(it);
                continue;
            }
            unsigned& lost_n = g_lost_n[hid];
            if (wrules::LostGiveUp(lost_n)) {
                // round 5: her world keeps removing this mirror (its ground re-check, an area not realized): no endless LOST / ADD
                g_lost_given.insert(hid);
                Log("[world] guest: bound hid %u vanished again after %u LOSTs: given up for this world (our world keeps removing it)", hid, lost_n);
                it = g_bound.erase(it);
                continue;
            }
            ++lost_n;
            Log("[world] guest: bound hid %u vanished without a REM -> LOST (%u/%u)", hid, lost_n, wrules::kLostMax);
            wwire::Lost l;
            l.hid = hid;
            std::uint8_t buf[wwire::kMaxMessage];
            Queue(buf, wwire::EncodeLost(Hdr(wwire::Msg::lost), l, buf, sizeof(buf)), 16);
            it = g_bound.erase(it);
        }
    GuestVerify();
    // W2
    g_contrib_share.store(ContribOn() && g_snap_ok, std::memory_order_release);
    if (g_contrib_ok) {
        GuestMatsTick();
        GuestStatePendingTick();
    }
}

// ---------------------------------------------------------------- messages
void OnMessage(const std::uint8_t* m, std::size_t n) {
    wwire::Header h;
    if (!wwire::ReadHeader(m, n, &h)) return void(++c_bad);
    if (h.type == static_cast<std::uint8_t>(wwire::Msg::hello)) {
        // round 5: the HELLO names the sending game process; a new one (a restarted partner, a new host) starts a fresh
        // duplicate window: every process numbers its messages from 1. A repeated HELLO is applied again (it only states).
        wwire::Header hh;
        wwire::Hello b;
        if (!wwire::DecodeHello(m, n, &hh, &b)) return void(++c_bad);
        const std::uint32_t was = g_rx_seq.sender();
        if (g_rx_seq.Sender(b.sender) && was)
            Log("[world] the partner's game is a new process (%08X, was %08X): its messages are numbered afresh", b.sender, was);
        g_rx_seq.Fresh(h.seq);
        ++c_rx[h.type];
        const bool first = !g_peer_hello;
        g_peer_hello = true;
        g_peer_on = b.on == 1;
        g_peer_parts = b.parts;
        if (g_peer_on && g_contrib_ok && !(b.parts & 2u)) Log("[world] contributions OFF: the partner's game does not share them (each world keeps its own numbers)");
        g_ladders_share.store(g_peer_on && g_ladders_ok && (b.parts & 4u) != 0, std::memory_order_release);
        if (first && g_peer_on && g_ladders_ok && !(b.parts & 4u)) Log("[world] ladders and ropes OFF: the partner's game does not share them (each world keeps its own)");
        g_zip_share.store(g_peer_on && g_zip_ok && (b.parts & 8u) != 0, std::memory_order_release);
        if (first && g_peer_on && g_zip_ok && !(b.parts & 8u)) Log("[world] ziplines OFF: the partner's game does not share them (each world keeps its own)");
        if (!g_peer_on && !g_peer_off_logged) {
            g_peer_off_logged = true;
            Log("[world] OFF: the partner's game has world sync off: nothing is shared");
        }
        if (first) Log("[world] %s: the partner's HELLO: world sync %s, parts %u, anim %u, its world %u", g_role == 1 ? "host" : "guest", g_peer_on ? "on" : "off",
                       b.parts, b.anim_mode, b.world);
        if (!g_hello_sent || first) SendHello();
        return;
    }
    if (!g_rx_seq.Fresh(h.seq)) return void(++c_dup);
    if (h.type < 17) ++c_rx[h.type];
    if (!g_peer_on || !g_ready || !g_cen.ok) return;  // nothing to apply to; the join check catches up
    if (g_role == 1) HostOnMessage(h, m, n);
    else GuestOnMessage(h, m, n);
}

// ---------------------------------------------------------------- the tick
void NewLinkSession() {
    g_peer_hello = false;
    g_peer_on = false;
    g_peer_parts = 0;
    g_contrib_share.store(false);
    g_ladders_share.store(false);
    g_zip_share.store(false);
    g_hello_sent = false;
    g_out.clear();
    g_guest_in_world = false;
    g_snap_waiting = false;
    g_snap_asked = -1e18;
    g_asm.Reset();
    // round 5: the guest joins again in the same world: the host forgot her (g_guest_in_world) and messages of the old session
    // may be lost both ways (the link starts afresh): a new snapshot (SNAP_REQ reconnect) rebinds and heals, the REMREQs go again
    if (g_role == 2 && g_ready) {
        if (g_snap_ok) Log("[world] guest: a new link session in the same world: the snapshot again (SNAP_REQ reconnect), %zu REMREQ(s) again", g_remreq_pend.size());
        g_snap_ok = false;
        g_snap_reason = 2;
        g_check_miss = 0;
    }
    Log("[world] a new link session (#%u): HELLO again", g_gen);
}
void NewWorld(std::uint32_t counter, bool ready) {
    Log("[world] %s: world %s (#%u)", g_role == 1 ? "host" : "guest", ready ? "ready" : "not ready", counter);
    g_ready = ready;
    g_world = counter;
    g_ready_since = g_now;
    g_host_base = false;
    g_prev.clear();
    g_rem_sent.clear();
    g_guest_in_world = false;
    g_cen = Census{};
    g_snap_ok = false;
    g_snap_waiting = false;
    g_snap_asked = -1e18;
    g_asm.Reset();
    g_h2g.clear();
    g_g2h.clear();
    g_bound.clear();
    g_mirror_pending.clear();
    g_verify.clear();
    g_rem_got.clear();
    g_remreq_sent.clear();
    g_last_seen.clear();
    g_picked_up.clear();
    g_host_picked.clear();
    g_remreq_pend.clear();
    g_snap_reason = 0;
    g_heal_idle = 0;
    g_lost_n.clear();
    g_lost_given.clear();
    g_host_reqs.clear();  // round 5: ids are reused after a reload: a request is bound in its own world only
    g_check_miss = 0;
    g_pending_gb.clear();
    // W2
    g_skey.clear();
    g_state_base = false;
    g_state_dirty.clear();
    g_recheck.clear();
    g_dur_sent.clear();
    g_state_pending.clear();
    g_contrib_share.store(false);
    if (g_role == 2 && !g_mats_pending.empty()) Log("[world] guest: %zu contribution(s) not answered by the host dropped: our world reloaded", g_mats_pending.size());
    g_mats_pending.clear();
    AcquireSRWLockExclusive(&g_box);
    g_mats_in.clear();
    if (g_role == 2 && !g_holds.empty()) Log("[world] guest: %zu held build(s) dropped: our world reloaded", g_holds.size());
    g_holds.clear();
    g_host_new.clear();
    g_manual_ids.clear();
    g_dev_manual.clear();
    g_local_only.clear();
    ReleaseSRWLockExclusive(&g_box);
    g_hello_sent = false;  // tell the partner our new world
}

void TickBody(const TickIn& in, double now) {
    g_now = now;
    g_connected = link::GetInfo().connected;
    g_have_partner = in.have_partner;
    if (in.have_partner) g_partner = in.partner;
    const std::uint32_t gen = link::Generation();
    if (gen != g_gen) {
        g_gen = gen;
        NewLinkSession();
    }
    std::uint32_t counter = 0;
    const bool ready = coop::WorldReady(&counter);
    if (ready != g_ready || (ready && counter != g_world)) NewWorld(counter, ready);
    if (g_ready && now - g_cen.t >= kCensusMs) {
        g_cen.t = now;
        RunCensus(g_cen);
        if (g_cen.ok && !g_watch.empty()) WatchTick();
        if (g_cen.ok && g_role == 1 && now - g_ready_since > kSettleMs) {
            HostDiff();
            if (g_contrib_ok) HostStateDiff();
        }
    }
    if (g_connected && !g_hello_sent) SendHello();
    if (g_aim.on) AimTick(in.local, now);
    // incoming, in order
    std::uint8_t buf[wwire::kMaxMessage];
    for (int i = 0; i < 64; ++i) {
        std::size_t n = 0;
        std::uint32_t g = 0;
        if (!link::PopWorldEvent(buf, sizeof(buf), n, g)) break;
        if (g != g_gen) continue;
        OnMessage(buf, n);
    }
    if (g_ready && g_cen.ok) {
        if (g_role == 1) {
            HostCheck();
            if (g_contrib_ok && g_state_base) HostDur();
            for (auto it = g_pending_gb.begin(); it != g_pending_gb.end();) {
                if (g_host_base) {
                    HostApplyGBuild(it->gb, it->flags);
                    it = g_pending_gb.erase(it);
                } else if (now - it->t > kNotReadyHoldMs) {
                    wwire::Rej rj;
                    rj.req = it->gb.req;
                    rj.reason = wwire::RejReason::not_ready;
                    std::uint8_t b2[wwire::kMaxMessage];
                    Queue(b2, wwire::EncodeRej(Hdr(wwire::Msg::rej), rj, b2, sizeof(b2)), 9);
                    it = g_pending_gb.erase(it);
                } else {
                    ++it;
                }
            }
            // stale host_new entries (a build that never appeared)
            AcquireSRWLockExclusive(&g_box);
            for (auto it = g_host_new.begin(); it != g_host_new.end();) {
                if (it->t == 0) it->t = now;
                if (now - it->t > kHostNewMs) it = g_host_new.erase(it);
                else ++it;
            }
            ReleaseSRWLockExclusive(&g_box);
        } else {
            GuestTick();
        }
    }
    Flush();
}

bool TickGuarded(const TickIn& in, double now) noexcept {
    __try {
        TickBody(in, now);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------- detours (any thread: copy and pass)
std::uint32_t H_enqueue(std::uintptr_t mgr, std::uintptr_t info) {
    if (!g_on.load(std::memory_order_acquire) || gate::Mirroring() || !info) return o_enqueue(mgr, info);
    std::uint8_t type = 0, fresh = 0;
    std::uint32_t q = 0;
    if (!mem::Rd(info, type) || !mem::Rd(info + 0x645, fresh) || !mem::Rd(info + 0x48, q)) return o_enqueue(mgr, info);
    if (!fresh || !Tracked(type)) return o_enqueue(mgr, info);
    std::uint8_t raw[wwire::kInfoSize];
    if (!mem::Read(info, raw, sizeof(raw))) return o_enqueue(mgr, info);
    if (g_role == 2) {
        if (g_peer_on && !SharedType(type)) {
            // W3 / W4: a ladder / rope or a zipline while the host's game does not share that part: hers stays hers
            AcquireSRWLockExclusive(&g_box);
            g_local_only.push_back(q);
            ReleaseSRWLockExclusive(&g_box);
            ++c_passed;
            (wrules::Zipline(type) ? c_zip_unshared : c_ladders_unshared).fetch_add(1);
            Log("[world] guest: local build id %u type %u (%s) NOT shared: the host's game does not share %s", q, type, wrules::TypeName(type), PartName(type));
            return o_enqueue(mgr, info);
        }
        if (!g_peer_on) {
            // the host's game is not known to share structures (yet): her build stays hers, as without world sync
            AcquireSRWLockExclusive(&g_box);
            g_local_only.push_back(q);
            ReleaseSRWLockExclusive(&g_box);
            ++c_passed;
            Log("[world] guest: local build id %u type %u (%s) NOT shared: the host's game has not said it shares structures", q, type, wrules::TypeName(type));
            return o_enqueue(mgr, info);
        }
        Hold h;
        h.q = q;
        wwire::RecordFromInfo(raw, &h.rec);
        h.dev = t_dev_build;
        h.anim = !t_dev_build || t_dev_anim;
        AcquireSRWLockExclusive(&g_box);
        h.n = ++g_req_n;
        g_holds.push_back(h);
        ReleaseSRWLockExclusive(&g_box);
        ++c_held;
        return q;  // the original returns info+0x48 [P]
    }
    const std::uint32_t r = o_enqueue(mgr, info);
    HostNew hn;
    hn.id = q;
    wwire::RecordFromInfo(raw, &hn.rec);
    hn.origin = wwire::Origin::host;
    hn.anim = true;
    AcquireSRWLockExclusive(&g_box);
    g_host_new.push_back(hn);
    ReleaseSRWLockExclusive(&g_box);
    return r;
}

void H_setlast(std::uintptr_t mgr, const void* wt, std::uint32_t id, std::uint8_t type) {
    if (!g_on.load(std::memory_order_acquire) || gate::Mirroring() || !wt) return o_setlast(mgr, wt, id, type);
    wwire::Xf x;
    const bool ok = mem::Read(reinterpret_cast<std::uintptr_t>(wt), &x, 0x3C);
    bool held = false;
    AcquireSRWLockExclusive(&g_box);
    if (g_role == 2) {
        for (Hold& h : g_holds)
            if (h.q == id) {
                h.has_pcc = ok;
                h.pcc = x;
                held = true;
            }
    } else {
        for (HostNew& h : g_host_new)
            if (h.id == id) {
                h.has_pcc = ok;
                h.pcc = x;
            }
    }
    ReleaseSRWLockExclusive(&g_box);
    if (held) {
        ++c_setlast_held;
        return;  // the printer plays when the host's answer arrives
    }
    o_setlast(mgr, wt, id, type);
}

void H_remman(std::uintptr_t cp) {
    if (g_on.load(std::memory_order_acquire) && !gate::Mirroring() && cp) {
        std::uint32_t id = 0;
        std::uint8_t type = 0;
        if (mem::Rd(cp + kCpId, id) && mem::Rd(cp + kCpType, type) && Tracked(type)) {
            AcquireSRWLockExclusive(&g_box);
            g_manual_ids.push_back(id);
            if (t_dev_remove) g_dev_manual.insert(id);
            ReleaseSRWLockExclusive(&g_box);
        }
    }
    o_remman(cp);
}

// W3: the game's own Remove on a ladder / rope: remember who called it (a placement re-check is not a pick-up).
void H_remove(std::uintptr_t cp, char keep) {
    const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (g_on.load(std::memory_order_acquire) && !gate::Mirroring() && cp) {
        std::uint32_t id = 0;
        std::uint8_t type = 0;
        if (mem::Rd(cp + kCpId, id) && mem::Rd(cp + kCpType, type) && wrules::LadderOrRope(type)) {
            const std::uintptr_t va = ret - mem::Base() + kImage;
            AcquireSRWLockExclusive(&g_box);
            g_removed_by[id] = va;
            if (g_removed_by.size() > 256) g_removed_by.erase(g_removed_by.begin());
            ReleaseSRWLockExclusive(&g_box);
            Log("[world] the game's Remove on id %u (%s, keep cargo %d) from 0x%llX", id, wrules::TypeName(type), keep ? 1 : 0, static_cast<unsigned long long>(va));
        }
    }
    o_remove(cp, keep);
}

// W3: the game's own placement re-checks of a ladder / rope: a removal they flag (cp+0xAA bit 0x40 set during the call) is
// the game's, not a pick-up (the guest then sends LOST, not REMREQ). The arguments pass through as four integer registers.
void ReCheck(Fn4 orig, std::uintptr_t va, std::uintptr_t cp, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d) {
    std::uint8_t before = 0;
    const bool watch = g_on.load(std::memory_order_acquire) && cp && mem::Rd(cp + kCpAA, before) && !(before & 0x40);
    orig(cp, b, c, d);
    std::uint8_t after = 0;
    if (!watch || !mem::Rd(cp + kCpAA, after) || !(after & 0x40)) return;
    std::uint32_t id = 0;
    std::uint8_t type = 0;
    if (!mem::Rd(cp + kCpId, id) || !mem::Rd(cp + kCpType, type) || !wrules::LadderOrRope(type)) return;
    AcquireSRWLockExclusive(&g_box);
    g_removed_by[id] = va;
    if (g_removed_by.size() > 256) g_removed_by.erase(g_removed_by.begin());
    ReleaseSRWLockExclusive(&g_box);
    Log("[world] id %u (%s): the game's placement re-check 0x%llX removed it (the ground does not hold it)", id, wrules::TypeName(type), static_cast<unsigned long long>(va));
}
void H_ladder_recheck(std::uintptr_t a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d) { ReCheck(o_ladder_recheck, kLadderRecheck, a, b, c, d); }
void H_ladder_attach(std::uintptr_t a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d) { ReCheck(o_ladder_attach, kLadderAttach, a, b, c, d); }
void H_rope_recheck(std::uintptr_t a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d) { ReCheck(o_rope_recheck, kRopeRecheck, a, b, c, d); }

// W2: the guest's own contribution / repair on a shared structure: it runs here (her terminal shows it at once; the gate
// drops everything online on the guest), and the amounts go to the host (MATS) from the game thread.
// Round 5: only the terminal's own commits are the player's contribution / repair (AddOwn from the terminal commit 0x142CF9968,
// AddRepair from the repair commit 0x142D32B92, both byte-checked calls), or the dev tool's. AddOwn has a second caller,
// the story's SetConstructionLevel export (0x142CDB16E): the host's own copy of the script does that in its world.
constexpr std::uintptr_t kTerminalAddOwnRet = 0x142CF996D;
constexpr std::uintptr_t kRepairAddRepairRet = 0x142D32B97;
std::atomic<unsigned> c_mats_other_caller{0};
void MatsDetour(std::uintptr_t cp, const std::int32_t* m, wwire::MatsKind kind, FnMats orig, std::uintptr_t ret) {
    orig(cp, m);
    if (g_role != 2 || !g_on.load(std::memory_order_acquire) || gate::Mirroring() || !cp || !m) return;
    std::uint8_t type = 0;
    std::uint32_t id = 0;
    if (!mem::Rd(cp + kCpType, type) || !mem::Rd(cp + kCpId, id) || !ContribT(type)) return;
    if (!g_contrib_share.load(std::memory_order_acquire)) {
        if (c_mats_unshared.fetch_add(1) == 0)
            Log("[world] guest: a %s on id %u (%s) is NOT shared: the host does not take contributions now (stays in our world)",
                kind == wwire::MatsKind::repair ? "repair" : "contribution", id, wrules::TypeName(type));
        return;
    }
    const std::uintptr_t ret_va = ret - mem::Base() + kImage;
    if (!t_dev_mats && ret_va != (kind == wwire::MatsKind::repair ? kRepairAddRepairRet : kTerminalAddOwnRet)) {
        if (c_mats_other_caller.fetch_add(1) < 4)
            Log("[world] guest: a %s on id %u (%s) from 0x%llX (not the terminal: a story script's SetConstructionLevel): not sent (the host's game does its own)",
                kind == wwire::MatsKind::repair ? "repair" : "contribution", id, wrules::TypeName(type), static_cast<unsigned long long>(ret_va));
        return;
    }
    MatsIn x;
    x.local = id;
    x.type = type;
    mem::Read(cp + kCpPos, x.pos, sizeof(x.pos));
    x.kind = kind;
    std::memcpy(x.m, m, sizeof(x.m));
    x.dev = t_dev_mats;
    AcquireSRWLockExclusive(&g_box);
    g_mats_in.push_back(x);
    ReleaseSRWLockExclusive(&g_box);
    ++c_mats_local;
}
void H_addown(std::uintptr_t cp, const std::int32_t* m) {
    MatsDetour(cp, m, wwire::MatsKind::build, o_addown, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
}
void H_addrepair(std::uintptr_t cp, const std::int32_t* m) {
    MatsDetour(cp, m, wwire::MatsKind::repair, o_addrepair, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
}
// The guest's own level-up request on a shared structure is the host's call: swallowed here (the host queues it after her
// MATS and its STATE makes hers follow), so each level-up plays exactly once in each world.
std::int32_t H_queue(std::uintptr_t mgr, std::uintptr_t cp) {
    if (g_role == 2 && g_on.load(std::memory_order_acquire) && g_contrib_share.load(std::memory_order_acquire) && !gate::Mirroring() && cp) {
        std::uint8_t type = 0;
        std::int32_t id = -1;
        if (mem::Rd(cp + kCpType, type) && mem::Rd(cp + kCpId, id) && ContribT(type)) {
            if (c_lvl_swallowed.fetch_add(1) < 8) Log("[world] guest: our level-up request on id %d (%s): the host's call (swallowed; its STATE starts ours)", id, wrules::TypeName(type));
            return id;  // the original returns the id
        }
    }
    // The host: a level-up request for a shared structure whose level-up already waits in the list or runs (the host's own
    // terminal right after the guest's contribution queued it, or the other way round) is dropped, so it plays once. The
    // game alone never meets this: only one player pays into its own world.
    if (g_role == 1 && g_contrib_ok && g_on.load(std::memory_order_acquire) && !gate::Mirroring() && cp && mgr) {
        std::uint8_t type = 0, a8 = 0;
        std::int32_t id = -1;
        if (mem::Rd(cp + kCpType, type) && mem::Rd(cp + kCpId, id) && mem::Rd(cp + kCpA8, a8) && ContribT(type) &&
            ((a8 & 0x80) || LevelUpQueued(mgr, static_cast<std::uint32_t>(id)))) {
            ++c_lvl_double;
            Log("[world] host: a second level-up request on hid %d (%s) while one %s: dropped (it plays once)", id, wrules::TypeName(type),
                (a8 & 0x80) ? "runs" : "is queued");
            return id;
        }
    }
    return o_queue(mgr, cp);
}

bool HookOne(std::uintptr_t va, void* det, void** orig, const char* name) {
    void* t = reinterpret_cast<void*>(A(va));
    MH_STATUS s = MH_CreateHook(t, det, orig);
    if (s == MH_OK) s = MH_EnableHook(t);
    Log("[world] hook %s 0x%llX: %s", name, static_cast<unsigned long long>(va), s == MH_OK ? "ok" : MH_StatusToString(s));
    return s == MH_OK;
}

// ---------------------------------------------------------------- byte checks (also for the read-only tools)
bool TypesMatchBody() {
    for (int t = 0; t < wrules::kTypeCount; ++t)
        if (GameCat(static_cast<std::uint8_t>(t)) != wrules::kTypes[t].category || GameNet(static_cast<std::uint8_t>(t)) != wrules::kTypes[t].net) return false;
    return true;
}
bool TypesMatch() noexcept {
    __try {
        return TypesMatchBody();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void RunChecks(const Config* cfg) {
    if (g_checked) return;
    g_checked = true;
    unsigned bad = 0;
    const char* first = nullptr;
    const bool test_fail = cfg && automation::Enabled() && !cfg->world_test_fail.empty();
    for (const Site& s : kSites) {
        bool ok = mem::CodeMatches(A(s.va), s.sig);
        if (ok && test_fail && std::strstr(s.name, cfg->world_test_fail.c_str())) {
            ok = false;
            Log("[world] world_test_fail=%s: treating '%s' as failed (hidden test)", cfg->world_test_fail.c_str(), s.name);
        }
        if (!ok) {
            bad |= s.group;
            Log("[world] byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
            if (!first) first = s.name;
        }
    }
    auto structural = [&](bool ok, const char* what, unsigned group) {
        if (ok) return;
        bad |= group;
        Log("[world] byte check FAILED: %s", what);
        if (!first) first = what;
    };
    structural(mem::RipTarget(A(0x142CD6C1D), 3, 8) == A(kMgrGlobal), "the manager global's address", kCore);
    structural(mem::RipTarget(A(0x1428D8767), 1, 5) == A(kEnqueueInfo), "the PCC commit calls EnqueueInfo", kBuild);
    structural(mem::RipTarget(A(0x1428D87F2), 1, 5) == A(kSetLastBuilt), "the PCC commit calls SetLastBuilt", kBuild);
    structural(mem::Q(A(kCpBaseVt) + 0x48) == A(kExportInfo) && mem::Q(A(kCpBaseVt) + 0x50) == A(kInitFromInfo), "the construction point vtable (+0x48 ExportInfo, +0x50 InitFromInfo)", kCore);
    // the type table against the game's own pure maps (they only index a byte table)
    if (!(bad & kCore)) {
        const bool types = TypesMatch();
        g_types_ok = types;
        structural(types, "the structure type table (TypeToCategory / TypeToNetType differ from world_rules.hpp)", kCore);
    }
    structural(mem::RipTarget(A(0x142CBAD51), 1, 5) == A(kReached), "the level-up consumer calls the level materials reach", kContrib);
    structural(mem::RipTarget(A(0x142CF9968), 1, 5) == A(kAddOwn) && mem::RipTarget(A(0x142CF9C5C), 1, 5) == A(kQueueLevelUp),
               "the terminal commit calls AddOwn and QueueLevelUp", kContrib);
    structural(mem::RipTarget(A(0x142D32B92), 1, 5) == A(kAddRepair), "the repair commit calls AddRepair", kContrib);
    structural(mem::Q(A(kCpBaseVt) + 0x140) == A(kRepairDurBase), "the construction point vtable (+0x140 durability repair)", kContrib);
    structural(mem::Q(A(kLadderVt) + 0x48) == A(kLadderExport) && mem::Q(A(kLadderVt) + 0x50) == A(kLadderInit) && mem::Q(A(kRopeVt) + 0x48) == A(kRopeExport) &&
                   mem::Q(A(kRopeVt) + 0x50) == A(kRopeInit),
               "the ladder / rope vtables (+0x48 ExportInfo, +0x50 InitFromInfo)", kLadder);
    structural(mem::RipTarget(A(kLadderExport) + 0x10, 1, 5) == A(kExportInfo) && mem::RipTarget(A(kRopeExport) + 0x10, 1, 5) == A(kExportInfo) &&
                   mem::RipTarget(A(kRopeInit) + 0x10, 1, 5) == A(kInitFromInfo),
               "the ladder / rope record functions call the base ones", kLadder);
    structural(mem::RipTarget(A(kRopeCommitEnqueue), 1, 5) == A(kEnqueueInfo), "the anchor commit calls EnqueueInfo", kLadder);
    structural(mem::Q(A(kLadderVt) + 0x60) == A(kLadderRecheck) && mem::Q(A(kRopeVt) + 0x60) == A(kRopeRecheck), "the ladder / rope vtables (+0x60 re-check)", kLadder);
    structural(mem::RipTarget(A(0x1428B200D), 1, 5) == A(kPlaceCheck), "the build preview calls the placement check", kLadder);
    structural(mem::Q(A(kZipVt) + 0x10) == A(kZipActivate) && mem::Q(A(kZipVt) + 0x48) == A(kZipExport) && mem::Q(A(kZipVt) + 0x50) == A(kZipInit),
               "the zipline vtable (+0x10 activation, +0x48 ExportInfo, +0x50 InitFromInfo)", kZipline);
    structural(mem::RipTarget(A(kZipExport) + 0x10, 1, 5) == A(kExportInfo) && mem::RipTarget(A(kZipInit) + 0x10, 1, 5) == A(kInitFromInfo) &&
                   mem::RipTarget(A(0x142CE5590), 1, 5) == A(kZipCollect),
               "the zipline record functions call the base ones, the activation the collector", kZipline);
    g_groups_ok = ~bad & 63u;
    g_checks_ok = (g_groups_ok & (kCore | kBuild)) == (kCore | kBuild);
    g_remove_ok = (g_groups_ok & kRemove) != 0;
    Log("[world] byte checks: %s%s%s (core %s, build %s, remove %s, contrib %s, ladder %s, zipline %s)", g_checks_ok ? "ok" : "FAILED", first ? ": first " : "",
        first ? first : "", (g_groups_ok & kCore) ? "ok" : "off", (g_groups_ok & kBuild) ? "ok" : "off", g_remove_ok ? "ok" : "off", (g_groups_ok & kContrib) ? "ok" : "off",
        (g_groups_ok & kLadder) ? "ok" : "off", (g_groups_ok & kZipline) ? "ok" : "off");
    if (!g_checks_ok) std::snprintf(g_off, sizeof(g_off), "this game build differs (%s)", first ? first : "byte check");
}

// ---------------------------------------------------------------- tools
void Put(char* msg, std::size_t cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, cap, fmt, ap);
    va_end(ap);
}

bool Dump(const double (&v)[3], char* msg, std::size_t cap) {
    Census c;
    if (!RunCensus(c)) return Put(msg, cap, "no construction manager (no world?)"), false;
    const int want_cat = v[0] == 0 && v[1] == 0 ? -1 : static_cast<int>(v[0]);
    const int nmax = v[1] > 0 ? static_cast<int>(v[1]) : 16;
    const int want_net = v[2] > 0 ? static_cast<int>(v[2]) - 1 : -1;  // W3: 1 stage, 2 player, 3 net (0 all)
    unsigned counts[18][3] = {};
    unsigned offline = 0, uploaded = 0, removing = 0;
    for (const CEnt& e : c.all) {
        if (e.cat < 18 && e.net < 3) ++counts[e.cat][e.net];
        if (e.ab & 0x80) ++offline;
        if (e.removing()) ++removing;
        if (e.net == 1 && !ServerIdEmpty(e.cp)) ++uploaded;
    }
    std::int32_t pc = 0, nc = 0, qn = 0;
    mem::Rd(c.mgr + kPlayerCounter, pc);
    mem::Rd(c.mgr + kPlayerCounter + 4, nc);
    mem::Rd(c.mgr + kQueueCount, qn);
    Log("[world] dump: %zu structures (shared %zu), offline %u, player uploaded %u, removing %u; counters player %d net %d; queue %d; %s", c.all.size(),
        c.synced.size(), offline, uploaded, removing, pc, nc, qn, g_role == 1 ? "host" : g_role == 2 ? "guest" : "solo");
    for (int cat = 0; cat < 18; ++cat)
        if (counts[cat][0] + counts[cat][1] + counts[cat][2])
            Log("[world] dump: category %2d: stage %u, player %u, net %u", cat, counts[cat][0], counts[cat][1], counts[cat][2]);
    WorldTransform sam{};
    const bool have = LocalSam(&sam);
    std::vector<std::pair<double, const CEnt*>> nearby;
    for (const CEnt& e : c.all) {
        if (want_cat >= 0 && e.cat != want_cat) continue;
        if (want_net >= 0 && e.net != want_net) continue;
        const double d = have ? std::hypot(e.pos[0] - sam.pos.x, e.pos[1] - sam.pos.y) : 0;
        nearby.emplace_back(d, &e);
    }
    std::sort(nearby.begin(), nearby.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (int i = 0; i < nmax && i < static_cast<int>(nearby.size()); ++i) {
        const CEnt& e = *nearby[static_cast<std::size_t>(i)].second;
        std::uint64_t uuid[2] = {};
        mem::Read(e.cp + kCpUuid, uuid, 16);
        std::int32_t own[6] = {};
        mem::Read(e.cp + kCpOwn, own, sizeof(own));
        Log("[world] dump: id %u type %u (%s) net %u state %u level %u at (%.2f, %.2f, %.2f) %.0f m dur %.0f mats %d/%d/%d/%d/%d/%d offline %d server id %s uuid %s%s",
            e.id, e.type, wrules::TypeName(e.type), e.net, e.state, e.level, e.pos[0], e.pos[1], e.pos[2], nearby[static_cast<std::size_t>(i)].first, e.dur,
            own[0], own[1], own[2], own[3], own[4], own[5], (e.ab & 0x80) ? 1 : 0, ServerIdEmpty(e.cp) ? "empty" : "set", uuid[0] || uuid[1] ? "set" : "0",
            e.removing() ? " REMOVING" : "");
    }
    Put(msg, cap, "%zu structures, %zu shared; see the log", c.all.size(), c.synced.size());
    return true;
}

const CEnt* Nearest(Census& c, std::uint32_t id, bool synced_only) {
    if (id) {
        for (const CEnt& e : c.all)
            if (e.id == id) return &e;
        return nullptr;
    }
    WorldTransform sam{};
    if (!LocalSam(&sam)) return nullptr;
    const CEnt* best = nullptr;
    double bd = 1e18;
    for (const CEnt& e : c.all) {
        if (synced_only && (!(Tracked(e.type) || (!Active() && wrules::IsSynced(e.type, true, true))) || e.removing())) continue;  // solo tools: ladders too
        const double d = std::hypot(e.pos[0] - sam.pos.x, e.pos[1] - sam.pos.y);
        if (d < bd) bd = d, best = &e;
    }
    return best;
}

bool RoundTrip(std::uint32_t id, char* msg, std::size_t cap) {
    Census c;
    if (!RunCensus(c)) return Put(msg, cap, "no construction manager"), false;
    const CEnt* e = Nearest(c, id, true);
    if (!e) return Put(msg, cap, "no such structure"), false;
    wwire::Record r1, r2;
    if (!ExportRecord(e->cp, &r1) || !ExportRecord(e->cp, &r2)) return Put(msg, cap, "ExportInfo not callable"), false;
    // the record against the live fields
    double p[3];
    r1.Pos(p);
    const bool pos_ok = std::fabs(p[0] - e->pos[0]) < 1e-6 && std::fabs(p[1] - e->pos[1]) < 1e-6 && std::fabs(p[2] - e->pos[2]) < 1e-6;
    const bool type_ok = r1.Type() == e->type;
    const bool level_ok = r1.Level() == e->level;
    std::int32_t own[6] = {}, rec_own[6] = {};
    mem::Read(e->cp + kCpOwn, own, sizeof(own));
    std::memcpy(rec_own, r1.b + wwire::RecordOffset(0x90), sizeof(rec_own));
    const bool mats_ok = std::memcmp(own, rec_own, sizeof(own)) == 0;
    // Record -> info -> Record
    InfoBuf info;
    info.Ctor();
    wwire::RecordToInfo(r1, info.b);
    wwire::Record r3;
    wwire::RecordFromInfo(info.b, &r3);
    info.Dtor();
    const bool stable = r1 == r2, back = r1 == r3, valid = wwire::RecordValid(r1);
    const bool all = stable && back && valid && pos_ok && type_ok && level_ok && mats_ok;
    Log("[world] round trip id %u type %u (%s): %s (stable %d, back %d, valid %d, pos %d, type %d, level %d, mats %d; offline byte %u, state %u, dur %.0f)", e->id, e->type,
        wrules::TypeName(e->type), all ? "EQUAL" : "DIFFERS", stable, back, valid, pos_ok, type_ok, level_ok, mats_ok, r1.Flag(0x62E), r1.State(),
        static_cast<double>(r1.Durability()));
    Put(msg, cap, "round trip id %u: %s", e->id, all ? "equal" : "differs");
    return all;
}

bool CountCheck(int type, double r, char* msg, std::size_t cap) {
    Census c;
    if (!RunCensus(c)) return Put(msg, cap, "no construction manager"), false;
    WorldTransform sam{};
    if (!LocalSam(&sam)) return Put(msg, cap, "no local Sam"), false;
    unsigned n = 0, off = 0;
    char ids[200] = {};
    std::size_t k = 0;
    unsigned lvl = 0;
    for (const CEnt& e : c.all) {
        if (e.removing()) continue;
        if (type >= 0 ? e.type != type : !Tracked(e.type)) continue;
        if (std::hypot(e.pos[0] - sam.pos.x, e.pos[1] - sam.pos.y) > r) continue;
        ++n;
        if (e.ab & 0x80) ++off;
        lvl = e.level;
        if (k < sizeof(ids) - 16) k += static_cast<std::size_t>(std::snprintf(ids + k, sizeof(ids) - k, "%s%u", k ? " " : "", e.id));
    }
    Log("[world] check: type %d within %.0f m: %u (ids %s) offline %u level %u", type, r, n, n ? ids : "-", off, lvl);
    Put(msg, cap, "%u within %.0f m (offline %u)", n, r, off);
    return true;
}

bool OneCp(std::uint32_t id, char* msg, std::size_t cap) {
    Census c;
    if (!RunCensus(c)) return Put(msg, cap, "no construction manager"), false;
    const CEnt* e = Nearest(c, id, true);
    if (!e) return Put(msg, cap, "no such structure"), false;
    std::int32_t own[6] = {}, oth[6] = {};
    mem::Read(e->cp + kCpOwn, own, sizeof(own));
    mem::Read(e->cp + kCpOthers, oth, sizeof(oth));
    Log("[world] structure id %u (hid %u) type %u (%s) state %u level %u at (%.2f, %.2f, %.2f) dur %.1f own %d/%d/%d/%d/%d/%d others %d/%d/%d/%d/%d/%d flags aa %02X ab %02X ad %02X "
        "offline %d server id %s entity %s",
        e->id, HidOf(e->id), e->type, wrules::TypeName(e->type), e->state, e->level, e->pos[0], e->pos[1], e->pos[2], static_cast<double>(e->dur), own[0], own[1], own[2],
        own[3], own[4], own[5], oth[0], oth[1], oth[2], oth[3], oth[4], oth[5], e->aa, e->ab, e->ad, (e->ab & 0x80) ? 1 : 0, ServerIdEmpty(e->cp) ? "empty" : "set",
        mem::Q(e->cp + kCpEntity) ? "yes" : "no");
    if (wrules::LadderOrRope(e->type)) {
        wwire::Record r;
        if (ExportRecord(e->cp, &r)) {
            std::int32_t i578 = 0;
            float f[5] = {};
            std::memcpy(&i578, r.b + wwire::RecordOffset(0x578), 4);
            std::memcpy(f, r.b + wwire::RecordOffset(0x578), 20);
            if (wrules::CategoryOf(e->type) == 10)
                Log("[world] ladder id %u (hid %u): mode %d lengths %.2f / %.2f flags %d %d; level %u dur %.0f", e->id, HidOf(e->id), i578, static_cast<double>(f[1]),
                    static_cast<double>(f[2]), static_cast<int>(reinterpret_cast<const std::int32_t*>(f)[3]), static_cast<int>(reinterpret_cast<const std::int32_t*>(f)[4]),
                    e->level, static_cast<double>(e->dur));
            else
                Log("[world] rope id %u (hid %u): length %.2f direction (%.2f, %.2f, %.2f) longest %.2f; level %u dur %.0f", e->id, HidOf(e->id), static_cast<double>(f[0]),
                    static_cast<double>(f[1]), static_cast<double>(f[2]), static_cast<double>(f[3]), static_cast<double>(f[4]), e->level, static_cast<double>(e->dur));
        }
    }
    if ((g_groups_ok & kContrib) && (ContribT(e->type) || (!Active() && wrules::Contributable(e->type, true)))) {
        CEnt f;
        if (FreshEnt(e->cp, f)) {
            std::int32_t need_rem[6] = {}, inv[6] = {}, next[6] = {};
            const std::uint8_t st = GameStatus(e->cp, need_rem, inv);
            for (int i = 0; i < 6; ++i) need_rem[i] = need_rem[i] > inv[i] ? need_rem[i] - inv[i] : 0;  // both relative to the current level
            GameNeed(e->cp, f.level + 1, next);
            const bool queued = LevelUpQueued(c.mgr, f.id);
            char a[64], b[64], d[64], r[64];
            MatsStr(f.inv, a, sizeof(a));
            MatsStr(need_rem, b, sizeof(b));
            MatsStr(next, d, sizeof(d));
            MatsStr(f.rep, r, sizeof(r));
            Log("[world] materials id %u (hid %u) %s: level %u of %u, status %u (%s)%s%s; invested %s, the next level needs %s in all (%s still), repair %s, list %d, "
                "dur %.0f of %.0f; reaches %d",
                f.id, HidOf(f.id), wrules::TypeName(f.type), f.level, f.max_level, st,
                st == 0 ? "enough" : st == 1 ? "missing" : st == 2 ? "max level" : st == 3 ? "no level-up" : st == 4 ? "a level-up runs" : "?",
                f.running ? ", running" : "", queued ? ", queued" : "", a, d, b, r, f.list[0] + f.list[1] + f.list[2] + f.list[3] + f.list[4] + f.list[5],
                static_cast<double>(f.dur), static_cast<double>(f.max_dur), GameReached(e->cp));
        }
    }
    Put(msg, cap, "id %u level %u state %u", e->id, e->level, e->state);
    return true;
}

bool Status(char* msg, std::size_t cap) {
    std::size_t holds = 0;
    AcquireSRWLockShared(&g_box);
    holds = g_holds.size();
    ReleaseSRWLockShared(&g_box);
    Log("[world] status: %s %s%s; world %u ready %d census %d (%zu shared); peer hello %d on %d; %s; bound %zu mapped %zu pending mirrors %zu holds %zu; "
        "anim %d; contributions %s; ladders and ropes %s; ziplines %s; gate %s",
        g_role == 1 ? "host" : g_role == 2 ? "guest" : "solo", Active() ? "ON" : "OFF", Active() ? "" : g_off, g_world, g_ready ? 1 : 0, g_cen.ok ? 1 : 0,
        g_cen.synced.size(), g_peer_hello ? 1 : 0, g_peer_on ? 1 : 0,
        g_role == 1 ? (g_guest_in_world ? "the guest is in our world" : "the guest is not in our world") : (g_snap_ok ? "snapshot complete" : "no snapshot yet"),
        g_bound.size(), g_h2g.size(), g_mirror_pending.size(), holds, g_anim_mode,
        !g_contrib_ok ? "OFF" : (g_peer_parts & 2u) ? (g_role == 2 ? (g_contrib_share.load() ? "shared" : "not yet (joining)") : "shared") : "on here (the partner's: not yet)",
        !g_ladders_ok ? "OFF" : g_ladders_share.load() ? "shared" : "on here (the partner's: not yet)",
        !g_zip_ok ? "OFF" : g_zip_share.load() ? "shared" : "on here (the partner's: not yet)", gate::Ok() ? "on" : gate::OffReason());
    Put(msg, cap, "world sync %s", Active() ? (g_role == 2 ? (g_snap_ok ? "ready" : "joining") : "on") : g_off);
    return true;
}

void Counters() {
    char line[400];
    std::size_t k = 0;
    for (int t = 1; t <= 16; ++t)
        if (c_tx[t] || c_rx[t]) k += static_cast<std::size_t>(std::snprintf(line + k, sizeof(line) - k, "%s %u/%u ", wwire::MsgName(static_cast<std::uint8_t>(t)), c_tx[t], c_rx[t]));
    line[k] = 0;
    Log("[world] counters: messages (sent/received) %s; bad %u stale %u dup %u; mirrors %u (exact %u, mapped %u, FAIL %u); created for the guest %u; removals applied %u; "
        "refused %u; held %u passed %u printer held %u",
        k ? line : "none ", c_bad, c_stale, c_dup, c_mirrors, c_mirror_exact, c_mirror_mapped, c_mirror_fail, c_created_for_guest, c_rem_applied, c_rej, c_held.load(),
        c_passed.load(), c_setlast_held.load());
    Log("[world] counters W2: contributions applied %u (dropped %u, retries %u), level-ups queued: host %u guest %u, guest level-up requests swallowed %u, double requests dropped %u, our MATS %u "
        "(not shared %u, pending %zu), STATE applied %u (pending %zu), durabilities applied %u, excess %u",
        c_mats_applied, c_mats_dropped, c_mats_retry, c_levelups_host, c_levelups_guest, c_lvl_swallowed.load(), c_lvl_double.load(), c_mats_local.load(), c_mats_unshared.load(),
        g_mats_pending.size(), c_state_applied, g_state_pending.size(), c_dur_applied, c_excess);
    Log("[world] counters W3: ladders / ropes %s; picked up by the guest: sent %u, applied %u; not shared %u", !g_ladders_ok ? "OFF" : g_ladders_share.load() ? "shared" : "on here only",
        c_pickups_sent.load(), c_pickups_applied.load(), c_ladders_unshared.load());
    Log("[world] counters W4: ziplines %s (%zu in the census); not shared %u; durabilities sent %u (ladders / ropes %u)", !g_zip_ok ? "OFF" : g_zip_share.load() ? "shared" : "on here only",
        static_cast<std::size_t>(std::count_if(g_cen.synced.begin(), g_cen.synced.end(), [](const auto& kv) { return wrules::Zipline(g_cen.all[kv.second].type); })),
        c_zip_unshared.load(), c_dur_sent, c_dur_sent_lr);
    gate::LogCounts("counts");
}

// W4: a zipline's links (its activation's handle array cp+0x528: {i32 count, i32 cap, entries of 0x18 bytes}; entry +0 =
// the linked zipline + 0x18): their ids, sorted. false if the array does not look right.
bool ZipLinks(std::uintptr_t mgr, std::uintptr_t cp, std::vector<std::uint32_t>& out) noexcept {
    out.clear();
    std::int32_t n = 0;
    std::uintptr_t data = 0;
    if (!mem::Rd(cp + kZipLinks, n) || !mem::Rd(cp + kZipLinks + 8, data) || n < 0 || n > 256 || (n && !data)) return false;
    for (std::int32_t i = 0; i < n; ++i) {
        std::uintptr_t h = 0;
        if (!mem::Rd(data + static_cast<std::uintptr_t>(i) * 0x18, h) || !h) continue;
        const std::uintptr_t other = h - 0x18;
        std::uint32_t id = 0;
        if (!InPool(mgr, other) || !mem::Rd(other + kCpId, id)) continue;
        out.push_back(id);
    }
    std::sort(out.begin(), out.end());
    return true;
}
// world_sync 16 <id | 0 = every zipline within v[2] m (default 300) of Sam>: the links, one line each
bool ZipReport(std::uint32_t id, double r, char* msg, std::size_t cap) {
    Census c;
    if (!RunCensus(c)) return Put(msg, cap, "no construction manager"), false;
    WorldTransform sam{};
    const bool have_sam = LocalSam(&sam);
    unsigned n = 0, links = 0;
    for (const CEnt& e : c.all) {
        if (!wrules::Zipline(e.type) || e.removing()) continue;
        if (id ? e.id != id : (!have_sam || std::hypot(e.pos[0] - sam.pos.x, e.pos[1] - sam.pos.y) > r)) continue;
        std::vector<std::uint32_t> l;
        if (!ZipLinks(c.mgr, e.cp, l)) continue;
        char ids[300];
        std::size_t k = 0;
        ids[0] = 0;
        for (std::uint32_t x : l)
            if (k < sizeof(ids) - 12) k += static_cast<std::size_t>(std::snprintf(ids + k, sizeof(ids) - k, " %u", x));
        Log("[world] zipline id %u (hid %u, %s, net %u, level %u, state %u) at (%.2f, %.2f, %.2f): %zu link(s):%s", e.id, HidOf(e.id), wrules::TypeName(e.type), e.net, e.level,
            e.state, e.pos[0], e.pos[1], e.pos[2], l.size(), l.empty() ? " none" : ids);
        ++n;
        links += static_cast<unsigned>(l.size());
    }
    Put(msg, cap, "%u zipline(s), %u link(s)", n, links);
    return n > 0;
}

// W3: the ladder / rope fields a dev placement writes (the commit's record carries them from the build preview).
struct DevSub {
    bool on = false;
    std::int32_t ladder_mode = 0;
    float length = 0;      // ladder: both lengths (0 = the game's 9.65 m); rope: its length (1..30 m)
    std::uint32_t level = 0;
};
bool DevPlace(int type, bool anim, double ahead, const DevSub& sub, char* msg, std::size_t cap);
bool DevBuild(const double (&v)[3], char* msg, std::size_t cap) {
    const int type = static_cast<int>(v[0]);
    if (wrules::LadderOrRope(type)) return Put(msg, cap, "ladders and ropes: dev_ladder / dev_rope"), false;
    return DevPlace(type, v[1] != 0, v[2] > 0 ? v[2] : 8, DevSub{}, msg, cap);
}
// dev_ladder <length m (0 = 9.65)> <mode 0/1> [m ahead, default 3]; dev_rope <length 1..30> <level 0..2> [m ahead, default 3]
bool DevLadderRope(bool rope, const double (&v)[3], char* msg, std::size_t cap) {
    DevSub sub;
    sub.on = true;
    sub.length = static_cast<float>(v[0]);
    if (rope) {
        if (!(sub.length >= 1.0f && sub.length <= 30.0f)) sub.length = 10.0f;
        sub.level = v[1] >= 0 && v[1] <= 2 ? static_cast<std::uint32_t>(v[1]) : 0;
    } else {
        if (!(sub.length > 0.0f && sub.length <= 30.0f)) sub.length = 0.0f;
        sub.ladder_mode = v[1] != 0 ? 1 : 0;
    }
    return DevPlace(rope ? 28 : 25, true, v[2] > 0 ? v[2] : 3, sub, msg, cap);
}
bool DevPlace(int type, bool anim, double ahead, const DevSub& sub, char* msg, std::size_t cap) {
    const devrules::Verdict d = dev::DecideNow(devrules::Tool::structure);
    if (!d.ok()) return Put(msg, cap, "%s", d.why), false;
    if (!g_checks_ok) return Put(msg, cap, "structures are off: %s", g_off), false;
    if (wrules::NetOf(type) != 1) return Put(msg, cap, "type %d is not a player structure type", type), false;
    if (sub.on && !g_ladders_ok && (Active() || g_role != 0)) return Put(msg, cap, "ladders and ropes are off here"), false;
    if (wrules::Zipline(type) && !g_zip_ok && (Active() || g_role != 0)) return Put(msg, cap, "ziplines are off here"), false;
    WorldTransform sam{};
    if (!LocalSam(&sam)) return Put(msg, cap, "no local Sam"), false;
    const std::uintptr_t mgr = Mgr();
    if (!MgrValid(mgr)) return Put(msg, cap, "no construction manager"), false;
    // a record as the PCC's preview makes it: type, the current area, the transform (Sam's yaw), level 0
    double pos[3];
    const double p0[3] = {sam.pos.x, sam.pos.y, sam.pos.z};
    devrules::Ahead(p0, sam.rot.m[1][0], sam.rot.m[1][1], ahead, pos);
    pos[2] = sam.pos.z;
    WorldTransform at = sam;
    at.pos.x = pos[0], at.pos.y = pos[1], at.pos.z = pos[2];
    InfoBuf info;
    std::memset(info.b, 0, sizeof(info.b));
    reinterpret_cast<FnInitInfo>(A(kInitInfo))(info.p(), static_cast<std::uint8_t>(type), &at, 0);
    // "Under construction" (record +0x623 -> cp+0xA8 bit 7): the CP update runs the level 0 -> 1 build timer only with it
    // (0x142CB8790: `(char)cp[0xA8] < 0`), and the build machine stays attached while it is set (0x142C809F0). The PCC
    // preview's record carries it; InitInfo does not. Run 20261121-151026: without it the CP stayed state 0, level 0.
    if (anim) info.b[0x623] = 1;
    if (sub.on) {
        // As the player's commit (0x1428D8070): the level (the anchor: 0 / 1 / 2 by item), the durability from the category
        // config's per-level timefall seconds (config +0xA0[level]), the subclass fields the InitFromInfo reads.
        std::memcpy(info.b + 0x4C, &sub.level, 4);
        const std::uintptr_t mcfg = mem::Q(mgr + kMgrCfg);
        const std::uintptr_t cfg = mcfg ? mem::Q(mcfg + 0x40 + static_cast<std::uintptr_t>(wrules::CategoryOf(type)) * 8) : 0;
        float dur = 0;
        if (cfg) mem::Rd(cfg + kCfgDurability + (sub.level > 4 ? 4 : sub.level) * 4, dur);
        if (!(dur > 0)) dur = 3600.0f * 24;
        std::memcpy(info.b + 0x7C, &dur, 4);
        if (wrules::CategoryOf(type) == 10) {
            std::memcpy(info.b + 0x578, &sub.ladder_mode, 4);
            std::memcpy(info.b + 0x57C, &sub.length, 4);
            std::memcpy(info.b + 0x580, &sub.length, 4);
        } else {
            const float dir[3] = {0.0f, 1.0f, 0.0f}, longest = 30.0f;  // the rope ExportInfo's own defaults (0x142D24470)
            std::memcpy(info.b + 0x578, &sub.length, 4);
            std::memcpy(info.b + 0x57C, dir, 12);
            std::memcpy(info.b + 0x588, &longest, 4);
        }
    }
    if (sub.on) {
        // The game's own placement check, as the build preview runs it: it snaps the transform onto the ground / an edge and
        // fills the ladder's / rope's geometry. A spot it refuses would be removed by the ladder's own re-check (vt+0x60) a
        // moment later (run 20261121-173040: a ladder on the flat road, gone after 3 s), so the dev placement tries Sam's
        // front at a few distances and turns and takes the first spot the game accepts.
        const double d0 = ahead;
        const double yaws[] = {0, 0.5, -0.5, 1.0, -1.0, 1.57, -1.57, 2.4, -2.4, 3.14};
        const double dists[] = {0, 1.5, -1.0, 3.0, 5.0};
        bool ok = false;
        int tries = 0;
        const double fx = sam.rot.m[1][0], fy = sam.rot.m[1][1];
        for (double dd : dists) {
            for (double yw : yaws) {
                ++tries;
                const double c = std::cos(yw), sn = std::sin(yw);
                const double dx = fx * c - fy * sn, dy = fx * sn + fy * c;
                const double dist = d0 + dd > 0.5 ? d0 + dd : 0.5;
                WorldTransform t = sam;
                t.pos.x = sam.pos.x + dx * dist, t.pos.y = sam.pos.y + dy * dist, t.pos.z = sam.pos.z;
                // the yaw turned with the offset: the structure faces away from Sam
                for (int r = 0; r < 3; ++r) {
                    const float ax = sam.rot.m[r][0], ay = sam.rot.m[r][1];
                    t.rot.m[r][0] = static_cast<float>(ax * c - ay * sn);
                    t.rot.m[r][1] = static_cast<float>(ax * sn + ay * c);
                }
                InfoBuf ti;
                std::memset(ti.b, 0, sizeof(ti.b));
                reinterpret_cast<FnInitInfo>(A(kInitInfo))(ti.p(), static_cast<std::uint8_t>(type), &t, 0);
                std::memcpy(ti.b + 0x4C, info.b + 0x4C, 4);
                std::memcpy(ti.b + 0x7C, info.b + 0x7C, 4);
                std::memcpy(ti.b + 0x578, info.b + 0x578, 0x14);
                ti.b[0x623] = info.b[0x623];
                alignas(16) std::uint8_t reasons[0x23 * 16] = {};
                alignas(16) std::uint8_t result[0x40] = {}, out5[0x40] = {};
                using FnCheck = bool (*)(std::uintptr_t, std::uintptr_t, void*, void*, void*, void*, std::uint8_t, std::uint8_t);
                const bool pass = reinterpret_cast<FnCheck>(A(kPlaceCheck))(mgr, ti.p(), reasons, result, out5, nullptr, 0, 0);
                for (int k = 0x22; k >= 0; --k) reinterpret_cast<void (*)(void*)>(A(kReasonFree))(reasons + k * 16);
                if (pass) {
                    // the checked record replaces ours (its snapped transform, the geometry the check filled in)
                    wwire::Record checked;
                    wwire::RecordFromInfo(ti.b, &checked);  // the plain-data ranges only (never the ctor-made members)
                    wwire::RecordToInfo(checked, info.b);
                    ok = true;
                    double sp[3];
                    std::memcpy(sp, info.b + 8, 24);
                    std::int32_t mode = 0;
                    float l[2] = {};
                    std::memcpy(&mode, info.b + 0x578, 4);
                    std::memcpy(l, info.b + 0x57C, 8);
                    Log("[world] dev %s: the game's placement check accepts try %d (%.1f m, turn %.2f rad): at (%.2f, %.2f, %.2f), fields %d %.2f %.2f", wrules::CategoryOf(type) == 10 ? "ladder" : "rope",
                        tries, dist, yw, sp[0], sp[1], sp[2], mode, static_cast<double>(l[0]), static_cast<double>(l[1]));
                    pos[0] = sp[0], pos[1] = sp[1], pos[2] = sp[2];
                    at.pos.x = sp[0], at.pos.y = sp[1], at.pos.z = sp[2];
                }
                ti.Dtor();
                if (ok) break;
            }
            if (ok) break;
        }
        if (!ok) {
            info.Dtor();
            Log("[world] dev %s: the game's placement check refused all %d spots around Sam", wrules::CategoryOf(type) == 10 ? "ladder" : "rope", tries);
            return Put(msg, cap, "no spot the game accepts for a %s here (%d tries)", wrules::TypeName(type), tries), false;
        }
    }
    wwire::Record rec;
    wwire::RecordFromInfo(info.b, &rec);
    info.Dtor();
    // The printer's place: the PCC commit passes the build preview's transform (player+0x308 -> +0xA8: the hologram at the
    // structure's spot), and the structure's update starts printing only once the printer stands on its target (distance
    // under 0.1 m, 0x142CB8790 -> 0x142CB4EE0). Run 20261121-150632: a printer at Sam's feet never started (state 0, level 0).
    wwire::Xf pcc;
    XfFrom(at, pcc);
    if (d.route == devrules::Route::ask_host || g_role == 2) {
        // the guest: the game's own path, held by our detour like her commit, then a dev GBUILD
        if (!Active()) return Put(msg, cap, "world sync is off: %s", g_off), false;
        InfoBuf gi;
        gi.Ctor();
        wwire::RecordToInfo(rec, gi.b);
        const std::int32_t q = reinterpret_cast<FnNewId>(A(kNewId))(mgr, 1);
        std::memcpy(gi.b + 0x48, &q, 4);
        std::uint32_t h = 0;
        reinterpret_cast<FnOwner>(A(kOwnerHandle))(&h, 1);
        reinterpret_cast<FnIdCopy>(A(kIdCopy))(gi.p() + 0x50, &h);
        reinterpret_cast<FnIdFree>(A(kIdFree))(&h);
        gi.b[0x645] = 1;
        t_dev_build = true;
        t_dev_anim = anim;
        H_enqueue(mgr, gi.p());  // through the detour, as the commit's call
        if (anim && wrules::HasPrinter(type)) H_setlast(mgr, &at, static_cast<std::uint32_t>(q), static_cast<std::uint8_t>(type));
        t_dev_build = false;
        gi.Dtor();
        Log("[world] dev build: type %d (%s) at (%.2f, %.2f, %.2f): our local id %d %s", type, wrules::TypeName(type), pos[0], pos[1], pos[2], q,
            g_peer_on ? "held for the host (GBUILD, dev)" : "NOT shared (no host sharing)");
        Put(msg, cap, "type %d: asked the host", type);
        return true;
    }
    const std::uint32_t id = CreateStructure(rec, 0, true, anim, &pcc);
    if (!id) return Put(msg, cap, "the structure could not be created"), false;
    if (Active() && g_role == 1) {
        HostNew hn;
        hn.id = id;
        hn.rec = rec;
        hn.has_pcc = anim && wrules::HasPrinter(type);
        hn.pcc = pcc;
        hn.origin = wwire::Origin::host;
        hn.anim = anim;
        hn.t = g_now;
        AcquireSRWLockExclusive(&g_box);
        g_host_new.push_back(hn);
        ReleaseSRWLockExclusive(&g_box);
    }
    Log("[world] dev build: id %u type %d (%s) anim %d at (%.2f, %.2f, %.2f) offline 1%s", id, type, wrules::TypeName(type), anim ? 1 : 0, pos[0], pos[1], pos[2],
        sub.on ? (wrules::CategoryOf(type) == 10 ? " (ladder)" : " (rope)") : "");
    Put(msg, cap, "id %u type %d (%s)", id, type, wrules::TypeName(type));
    return true;
}

bool DevRemove(const double (&v)[3], char* msg, std::size_t cap) {
    const devrules::Verdict d = dev::DecideNow(devrules::Tool::structure);
    if (!d.ok()) return Put(msg, cap, "%s", d.why), false;
    if (!g_checks_ok || !g_remove_ok) return Put(msg, cap, "structures are off: %s", g_off), false;
    Census c;
    if (!RunCensus(c)) return Put(msg, cap, "no construction manager"), false;
    const CEnt* e = Nearest(c, static_cast<std::uint32_t>(v[0]), true);
    if (!e) return Put(msg, cap, "no such structure"), false;
    const bool manual = v[1] != 0 || v[0] == 0;
    if (manual) {
        // through our detour, as the player's dismantle (the guest then sends REMREQ); without the detour (solo) the game's own call
        if (Active() && o_remman) {
            t_dev_remove = g_role == 2;  // the guest's REMREQ goes with the dev flag: the host applies it only on sandboxed saves
            H_remman(e->cp);
            t_dev_remove = false;
        } else {
            gate::ScopeMirroring scope;
            reinterpret_cast<FnRemMan>(A(kRemoveManually))(e->cp);
        }
    } else {
        AcquireSRWLockExclusive(&g_box);
        g_removed_by[e->id] = kDevRemoveMark;  // not a pick-up (the guest sends LOST: the host keeps its structure)
        ReleaseSRWLockExclusive(&g_box);
        RemoveBits(c.mgr, e->cp);
    }
    Log("[world] dev remove: id %u (%s) %s", e->id, wrules::TypeName(e->type), manual ? "dismantled (RemoveManually)" : "removal bits");
    Put(msg, cap, "id %u removed", e->id);
    return true;
}

bool DevDecay(const double (&v)[3], char* msg, std::size_t cap) {
    const devrules::Verdict d = dev::DecideNow(devrules::Tool::structure);
    if (!d.ok() || d.route == devrules::Route::ask_host) return Put(msg, cap, "%s", d.ok() ? "host only" : d.why), false;
    Census c;
    if (!RunCensus(c)) return Put(msg, cap, "no construction manager"), false;
    const CEnt* e = Nearest(c, static_cast<std::uint32_t>(v[0]), true);
    if (!e) return Put(msg, cap, "no such structure"), false;
    const float s = static_cast<float>(v[1]);
    *reinterpret_cast<float*>(e->cp + kCpDur) = s;
    Log("[world] dev decay: id %u (%s) durability %.1f -> %.1f s", e->id, wrules::TypeName(e->type), static_cast<double>(e->dur), static_cast<double>(s));
    Put(msg, cap, "id %u durability %.1f", e->id, static_cast<double>(s));
    return true;
}

// dev_build_mats <id | 0 nearest> <kind 0 contribute | 1 repair> <0 < f <= 1: that share of what the next level still needs
// | repair: units of the material that repairs most>: the terminal commit's steps without the UI or the inventory: AddOwn /
// AddRepair THROUGH our detours (the guest's go to the host as MATS), then, as the commit, Status 0 -> QueueLevelUp (through
// the detour: swallowed on the guest, the host's call).
bool DevMats(const double (&v)[3], char* msg, std::size_t cap) {
    const devrules::Verdict d = dev::DecideNow(devrules::Tool::structure);
    if (!d.ok()) return Put(msg, cap, "%s", d.why), false;
    if (!(g_groups_ok & kContrib)) return Put(msg, cap, "contributions are off (byte check)"), false;
    Census c;
    if (!RunCensus(c)) return Put(msg, cap, "no construction manager"), false;
    const CEnt* e0 = Nearest(c, static_cast<std::uint32_t>(v[0]), true);
    if (!e0) return Put(msg, cap, "no such structure"), false;
    CEnt e;
    if (!FreshEnt(e0->cp, e) || !(ContribT(e.type) || (!Active() && wrules::Contributable(e.type, true)))) return Put(msg, cap, "id %u is not a shared structure type", e0->id), false;
    const bool repair = v[1] != 0;
    std::int32_t m[6] = {};
    char ms[64];
    if (!repair) {
        // Status's outputs are relative to the current level: what the next level needs on its own and what is invested
        // beyond the current level (0x142C59770: both minus NeedToLevel(level)); still needed = their difference.
        std::int32_t need_rem[6] = {}, inv_rel[6] = {};
        const std::uint8_t st = GameStatus(e.cp, need_rem, inv_rel);
        for (int i = 0; i < 6; ++i) need_rem[i] = need_rem[i] > inv_rel[i] ? need_rem[i] - inv_rel[i] : 0;
        if (st == 2 || st == 3 || st == 4)
            return Put(msg, cap, "id %u level %u: %s", e.id, e.level, st == 2 ? "at its last level" : st == 3 ? "has no levels" : "a level-up runs"), false;
        const double f = v[2] > 0 && v[2] <= 1 ? v[2] : 1.0;
        bool any = false;
        for (int i = 0; i < 6; ++i) {
            m[i] = static_cast<std::int32_t>(std::ceil(need_rem[i] * f));
            if (m[i] > 0) any = true;
        }
        if (!any && st != 0) return Put(msg, cap, "id %u: nothing needed", e.id), false;
    } else {
        const std::uintptr_t cfg = mem::Q(e.cp + kCpCfg);
        float w[6] = {};
        if (cfg) mem::Read(cfg + kCfgRepairW, w, sizeof(w));
        int best = 0;
        for (int i = 1; i < 6; ++i)
            if (w[i] > w[best]) best = i;
        m[best] = v[2] > 0 ? static_cast<std::int32_t>(v[2]) : 10;
    }
    MatsStr(m, ms, sizeof(ms));
    const std::uintptr_t mgr = c.mgr;
    bool sum = false;
    for (std::int32_t x : m)
        if (x > 0) sum = true;
    t_dev_mats = true;
    if (repair) {
        RepairDurability(e.cp, RepairSeconds(e.cp, m));
        (o_addrepair ? reinterpret_cast<FnMats>(&H_addrepair) : reinterpret_cast<FnMats>(A(kAddRepair)))(e.cp, m);
    } else if (sum) {
        (o_addown ? reinterpret_cast<FnMats>(&H_addown) : reinterpret_cast<FnMats>(A(kAddOwn)))(e.cp, m);
    }
    t_dev_mats = false;
    std::uint8_t st = 0;
    bool queued = false;
    if (!repair) {
        st = GameStatus(e.cp);
        if (st == 0) {
            if (o_queue) H_queue(mgr, e.cp);
            else reinterpret_cast<FnQueue>(A(kQueueLevelUp))(mgr, e.cp);
            queued = true;
        }
    }
    if (repair) {
        float dur = 0;
        mem::Rd(e.cp + kCpDur, dur);
        Log("[world] dev repair: id %u (hid %u, %s) level %u: repair %s -> durability %.0f -> %.0f s", e.id, HidOf(e.id), wrules::TypeName(e.type), e.level, ms,
            static_cast<double>(e.dur), static_cast<double>(dur));
    } else {
        Log("[world] dev contribution: id %u (hid %u, %s) level %u: materials %s -> status %u%s", e.id, HidOf(e.id), wrules::TypeName(e.type), e.level, ms, st,
            queued ? ", level-up requested (the terminal's QueueLevelUp)" : "");
    }
    Put(msg, cap, "id %u: %s %s", e.id, repair ? "repair" : "materials", ms);
    return true;
}

bool CommandBody(Tool tool, const double (&v)[3], char* msg, std::size_t cap) {
    RunChecks(nullptr);
    if (!(g_groups_ok & kCore)) return Put(msg, cap, "structures are off: %s", g_off), false;
    // round 5: a hidden run's game is signed in with the player's account: no world-changing tool without the online gate
    if (automation::Enabled() && !gate::Ok() && tool != Tool::dump && tool != Tool::sync && tool != Tool::decay)
        return Put(msg, cap, "the online gate is not in (%s): world-changing tools are refused in hidden runs", gate::OffReason()), false;
    switch (tool) {
        case Tool::dump: return Dump(v, msg, cap);
        case Tool::build: return DevBuild(v, msg, cap);
        case Tool::remove: return DevRemove(v, msg, cap);
        case Tool::decay: return DevDecay(v, msg, cap);
        case Tool::mats: return DevMats(v, msg, cap);
        case Tool::ladder: return DevLadderRope(false, v, msg, cap);
        case Tool::rope: return DevLadderRope(true, v, msg, cap);
        case Tool::sync: break;
    }
    const int op = static_cast<int>(v[0]);
    switch (op) {
        case 0: return Status(msg, cap);
        case 1:
            if (!Active()) return Put(msg, cap, "off"), false;
            if (g_role == 2) SendSnapReq(1);
            else g_last_check = -1e18;
            return Put(msg, cap, g_role == 2 ? "SNAP_REQ sent" : "CHECK now"), true;
        case 2: {
            Log("[world] binding: %zu bound, %zu mapped", g_bound.size(), g_h2g.size());
            for (const auto& kv : g_h2g) Log("[world] binding: hid %u -> gid %u (mapped)", kv.first, kv.second);
            return Put(msg, cap, "%zu bound, %zu mapped", g_bound.size(), g_h2g.size()), true;
        }
        case 3: Counters(); return Put(msg, cap, "counters logged; gate dropped %u", gate::DroppedTotal()), true;
        case 4: return OneCp(static_cast<std::uint32_t>(v[1]), msg, cap);
        case 5:
            if (!automation::Enabled()) return Put(msg, cap, "hidden runs only"), false;
            g_hold_sends = v[1] != 0;
            Log("[world] test: the guest's GBUILDs %s", g_hold_sends ? "held back" : "sent again");
            return Put(msg, cap, "hold %d", g_hold_sends ? 1 : 0), true;
        case 10: return CountCheck(static_cast<int>(v[1]), v[2] > 0 ? v[2] : 60, msg, cap);
        case 11: return RoundTrip(static_cast<std::uint32_t>(v[1]), msg, cap);
        case 12: {
            // W3 (hidden runs): Sam to a structure, `v[2]` m (default 1) short of it on the line from where he stands, through
            // dev_goto (then "waitworld 45 goto"): a ladder's / rope's prompts ("Hold START: Collapse Ladder / Recover Rope")
            // show only within reach of it.
            if (!automation::Enabled()) return Put(msg, cap, "hidden runs only"), false;
            Census c;
            if (!RunCensus(c)) return Put(msg, cap, "no construction manager"), false;
            const CEnt* e = Nearest(c, static_cast<std::uint32_t>(v[1]), true);
            WorldTransform sam{};
            if (!e || !LocalSam(&sam)) return Put(msg, cap, "no such structure"), false;
            const double dx = sam.pos.x - e->pos[0], dy = sam.pos.y - e->pos[1], dl = std::hypot(dx, dy);
            const double back = v[2] > 0 ? v[2] : 1.0;
            const double g[3] = {e->pos[0] + (dl > 0.01 ? dx / dl * back : 0), e->pos[1] + (dl > 0.01 ? dy / dl * back : 0), 0};
            Log("[world] to structure id %u (%s) at (%.2f, %.2f, %.2f): Sam to (%.2f, %.2f), %.1f m short", e->id, wrules::TypeName(e->type), e->pos[0], e->pos[1], e->pos[2], g[0],
                g[1], back);
            return dev::Command(dev::Op::go_to, g, msg, cap);
        }
        case 15: {
            // W3 (hidden runs): aim the camera at structure v[1] (0 nearest ladder / rope); a rope: 6 m down its direction (the
            // edge it hangs over), within v[2] deg (default 15)
            if (!automation::Enabled()) return Put(msg, cap, "hidden runs only"), false;
            Census c;
            if (!RunCensus(c)) return Put(msg, cap, "no construction manager"), false;
            const CEnt* e = nullptr;
            if (v[1] == 0) {
                // the nearest ladder / rope of any owner (the 100% save's online ones mark the edges)
                WorldTransform sam{};
                double bd = 1e18;
                if (LocalSam(&sam))
                    for (const CEnt& x : c.all) {
                        if (!wrules::LadderOrRope(x.type) || x.removing()) continue;
                        const double dd = std::hypot(x.pos[0] - sam.pos.x, x.pos[1] - sam.pos.y);
                        if (dd < bd) bd = dd, e = &x;
                    }
            } else {
                e = Nearest(c, static_cast<std::uint32_t>(v[1]), true);
            }
            if (!e) return Put(msg, cap, "no such structure"), false;
            double tx = e->pos[0], ty = e->pos[1];
            if (wrules::CategoryOf(e->type) == 11) {
                wwire::Record r;
                float d[3] = {};
                if (ExportRecord(e->cp, &r)) std::memcpy(d, r.b + wwire::RecordOffset(0x57C), 12);
                const double l = std::hypot(d[0], d[1]);
                if (l > 0.1) tx += d[0] / l * 6, ty += d[1] / l * 6;
            }
            g_aim.on = true, g_aim.x = tx, g_aim.y = ty, g_aim.tol = v[2] >= 1 && v[2] <= 45 ? v[2] : 15;
            Log("[world] camera aim at (%.1f, %.1f) for id %u (%s)", tx, ty, e->id, wrules::TypeName(e->type));
            return Put(msg, cap, "aiming at id %u", e->id), true;
        }
        case 14: {
            const int id = static_cast<int>(v[1]);
            if (id < 0) {
                g_watch.clear();
                return Put(msg, cap, "watch off"), true;
            }
            Census c;
            if (!RunCensus(c)) return Put(msg, cap, "no construction manager"), false;
            const CEnt* best = nullptr;
            if (id > 0) {
                for (const CEnt& e : c.all)
                    if (e.id == static_cast<std::uint32_t>(id)) best = &e;
            } else {
                WorldTransform sam{};
                double bd = 1e18;
                if (LocalSam(&sam))
                    for (const CEnt& e : c.all) {
                        if (!wrules::LadderOrRope(e.type) || e.removing()) continue;
                        const double dd = std::hypot(e.pos[0] - sam.pos.x, e.pos[1] - sam.pos.y);
                        if (dd < bd) bd = dd, best = &e;
                    }
            }
            if (!best) return Put(msg, cap, "no such structure"), false;
            Watch w;
            w.id = best->id;
            g_watch.push_back(w);
            Log("[world] watch id %u (%s) at (%.2f, %.2f, %.2f): every census logs a change", best->id, wrules::TypeName(best->type), best->pos[0], best->pos[1], best->pos[2]);
            return Put(msg, cap, "watching id %u", best->id), true;
        }
        case 16: return ZipReport(static_cast<std::uint32_t>(v[1]), v[2] > 0 ? v[2] : 300, msg, cap);
        case 13:
            Log("[world] gate: %s; online service vtable 0x143EDECC8 slots +0x28 RegisterNew, +0x38 drain (one structure per call), +0xB8 telemetry",
                gate::Ok() ? "ON" : gate::OffReason());
            gate::LogCounts("gate");
            return Put(msg, cap, "gate %s", gate::Ok() ? "on" : "off"), true;
        default: return Put(msg, cap, "world_sync: 0 status, 1 snapshot, 2 binding, 3 counters, 4 id, 5 0/1 hold, 10 type m, 11 id, 12 id m: Sam to it, 13 gate, 14 id watch, 16 id: zipline links"), false;
    }
}

bool CommandGuarded(Tool tool, const double (&v)[3], char* msg, std::size_t cap, bool* ok) noexcept {
    __try {
        *ok = CommandBody(tool, v, msg, cap);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

// ---------------------------------------------------------------- public
void Install(const Config& config) noexcept {
    g_role = config.role == Role::host ? 1 : config.role == Role::guest ? 2 : 0;
    g_anim_mode = config.world_anim;
    if (g_anim_mode == 1) {
        Log("[world] world_anim=bits is not in this build: the game's own new-build path (645) instead");
        g_anim_mode = 0;
    }
    g_nonce = static_cast<std::uint32_t>(GetTickCount64() ^ (GetCurrentProcessId() << 16)) | 1u;
    bool wanted = false;
    if (!config.world_sync) std::snprintf(g_off, sizeof(g_off), "world_sync=0%s", config.world_sync_set ? "" : " (not in session.ini and no host-held save session)");
    else if (!config.host_save) std::snprintf(g_off, sizeof(g_off), "needs the host-held save session (host_save=1)");
    else if (g_role == 0) std::snprintf(g_off, sizeof(g_off), "role %s (only host and guest)", RoleName(config.role));
    else if (!coop::Active()) std::snprintf(g_off, sizeof(g_off), "the co-op save session is off");
    else wanted = true;
    gate::Install(config, wanted);
    RunChecks(&config);
    if (!wanted) {
        Log("[world] world sync OFF: %s", g_off);
        return;
    }
    if (!g_checks_ok) {
        Log("[world] world sync OFF: %s", g_off);
        return;
    }
    if (!gate::Ok()) {
        std::snprintf(g_off, sizeof(g_off), "the online gate is not in (%s)", gate::OffReason());
        Log("[world] world sync OFF: %s", g_off);
        return;
    }
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        std::snprintf(g_off, sizeof(g_off), "MinHook init failed");
        Log("[world] world sync OFF: %s", g_off);
        return;
    }
    bool hooks = HookOne(kEnqueueInfo, reinterpret_cast<void*>(&H_enqueue), reinterpret_cast<void**>(&o_enqueue), "EnqueueInfo") &&
                 HookOne(kSetLastBuilt, reinterpret_cast<void*>(&H_setlast), reinterpret_cast<void**>(&o_setlast), "SetLastBuilt");
    if (hooks && g_remove_ok)
        g_remove_ok = HookOne(kRemoveManually, reinterpret_cast<void*>(&H_remman), reinterpret_cast<void**>(&o_remman), "RemoveManually");
    if (hooks && config.world_contrib && (g_groups_ok & kContrib)) {
        g_contrib_ok = HookOne(kAddOwn, reinterpret_cast<void*>(&H_addown), reinterpret_cast<void**>(&o_addown), "AddOwn") &&
                       HookOne(kAddRepair, reinterpret_cast<void*>(&H_addrepair), reinterpret_cast<void**>(&o_addrepair), "AddRepair") &&
                       HookOne(kQueueLevelUp, reinterpret_cast<void*>(&H_queue), reinterpret_cast<void**>(&o_queue), "QueueLevelUp");
        if (!g_contrib_ok) Log("[world] contributions OFF: a detour could not be installed (the ones that went in pass everything through)");
    } else if (hooks) {
        Log("[world] contributions OFF: %s", !config.world_contrib ? "world_contrib=0" : "this game build differs (contrib byte check)");
    }
    if (hooks && config.world_ladders && (g_groups_ok & kLadder) && g_remove_ok) {
        g_ladders_ok = HookOne(kRemoveFn, reinterpret_cast<void*>(&H_remove), reinterpret_cast<void**>(&o_remove), "Remove") &&
                       HookOne(kLadderRecheck, reinterpret_cast<void*>(&H_ladder_recheck), reinterpret_cast<void**>(&o_ladder_recheck), "ladder re-check") &&
                       HookOne(kLadderAttach, reinterpret_cast<void*>(&H_ladder_attach), reinterpret_cast<void**>(&o_ladder_attach), "ladder re-attach") &&
                       HookOne(kRopeRecheck, reinterpret_cast<void*>(&H_rope_recheck), reinterpret_cast<void**>(&o_rope_recheck), "rope re-check");
        if (!g_ladders_ok) Log("[world] ladders and ropes OFF: a detour could not be installed (the ones that went in pass everything through)");
    } else if (hooks) {
        Log("[world] ladders and ropes OFF: %s", !config.world_ladders ? "world_ladders=0" : !g_remove_ok ? "removals are off (byte check)" : "this game build differs (ladder byte check)");
    }
    // W4: ziplines need no detour of their own (the PCC commit, EnqueueInfo, SetLastBuilt, RemoveManually and the W2 ones)
    if (hooks && config.world_ziplines && (g_groups_ok & kZipline)) {
        g_zip_ok = true;
    } else if (hooks) {
        Log("[world] ziplines OFF: %s", !config.world_ziplines ? "world_ziplines=0" : "this game build differs (zipline byte check)");
    }
    if (!hooks) {
        std::snprintf(g_off, sizeof(g_off), "a detour could not be installed");
        Log("[world] world sync OFF: %s (the detours that went in pass everything through)", g_off);
        return;
    }
    g_off[0] = 0;
    gate::SetSyncOn(true);
    g_on.store(true, std::memory_order_release);
    Log("[world] world sync ON (%s): shared structures (PCC structures%s%s; anim %s); removals %s; the guest's builds %s; contributions %s", g_role == 1 ? "host" : "guest",
        g_ladders_ok ? ", ladders and climbing ropes" : "", g_zip_ok ? ", ziplines" : "",
        g_anim_mode == 2 ? "loaded" : g_anim_mode == 3 ? "off" : "645", g_remove_ok ? "mirrored" : "OFF (byte check)",
        g_role == 2 ? "are held and created by the host" : "come as GBUILD requests",
        g_contrib_ok ? (g_role == 2 ? "go to the host (MATS), its level-ups come back (STATE)" : "shared (MATS in, STATE / DUR out)") : "OFF");
}

bool Active() noexcept { return g_on.load(std::memory_order_acquire); }
const char* OffReason() noexcept { return Active() ? "" : g_off; }

void Tick(const TickIn& in, double now) noexcept {
    if (g_role == 0 || !g_on.load(std::memory_order_acquire)) return;
    if (!coop::Active()) {
        EmergencyStop();
        std::snprintf(g_off, sizeof(g_off), "the co-op save session stopped");
        Log("[world] the co-op save session stopped: world sync stops (the detours pass everything through)");
        return;
    }
    if (!TickGuarded(in, now)) {
        Log("[world] FAULT in the world sync tick: stopped (the detours pass everything through)");
        EmergencyStop();
    }
}

void EmergencyStop() noexcept {
    if (!g_on.exchange(false)) return;
    g_stopped = true;
    // round 5: nothing is shared any more: the guest's detours pass her contributions and level-ups through again
    g_contrib_share.store(false, std::memory_order_release);
    g_ladders_share.store(false, std::memory_order_release);
    g_zip_share.store(false, std::memory_order_release);
    // the host falls back to vanilla; a world-sync guest's gate stays closed (gate::Install locked it: her world is still a
    // copy of the host's save)
    gate::SetSyncOn(false);
    std::snprintf(g_off, sizeof(g_off), "stopped after a fault");
}

bool Command(Tool tool, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    bool ok = false;
    if (!CommandGuarded(tool, v, msg, cap, &ok)) {
        std::snprintf(msg, cap, "FAULT in the world tool (see the log)");
        Log("[world] FAULT in a world tool (%d): world sync stops", static_cast<int>(tool));
        EmergencyStop();
        return false;
    }
    return ok;
}

// The shots' aimed test (shot_sync.cpp, hidden runs): the structure nearest our Sam, its place and durability (cp +0xD8).
bool NearestStructure(double max_m, int want_type, std::uint32_t* id, int* type, double (&pos)[3], float* durability) noexcept {
    Census c;
    WorldTransform sam{};
    if (!RunCensus(c) || !LocalSam(&sam)) return false;
    const CEnt* e = nullptr;
    double bd = max_m > 0 ? max_m : 30.0;
    for (const CEnt& x : c.all) {
        if ((want_type && x.type != want_type) || x.removing()) continue;
        const double d = std::hypot(x.pos[0] - sam.pos.x, x.pos[1] - sam.pos.y);
        if (d < bd) bd = d, e = &x;
    }
    if (!e) return false;
    float d = 0;
    if (!mem::Rd(e->cp + kCpDur, d) || !std::isfinite(d)) return false;
    *id = e->id;
    *type = e->type;
    pos[0] = e->pos[0], pos[1] = e->pos[1], pos[2] = e->pos[2];
    *durability = d;
    return true;
}

}  // namespace st::wshare

namespace st::game {
bool WorldShareCommand(int tool, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (tool < 0 || tool > 7) {
        std::snprintf(msg, cap, "unknown world tool %d", tool);
        return false;
    }
    return wshare::Command(static_cast<wshare::Tool>(tool), v, msg, cap);
}
}  // namespace st::game
