// Round 27 M2 / M3 (enemy_target.hpp; docs/enemy-sync.md 16): the partner stimulus, SPV1, the target pick and the redirects.
#include "enemy_target.hpp"

#include <windows.h>

#include <intrin.h>

#include "MinHook.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

#include "automation.hpp"
#include "enemy_census.hpp"
#include "enemy_l1_probe.hpp"
#include "enemy_l1_rules.hpp"
#include "enemy_rules.hpp"
#include "enemy_sense_hooks.hpp"
#include "enemy_stim_core.hpp"
#include "enemy_sync.hpp"
#include "enemy_target_rules.hpp"
#include "enemy_wire.hpp"
#include "game_actions.hpp"
#include "link.hpp"
#include "log.hpp"
#include "partner_holo.hpp"
#include "partner_source.hpp"
#include "real_body.hpp"
#include "seh_guard.hpp"

namespace st::etarget {
namespace {
namespace mem = partner::mem;
namespace R = erules;
namespace L = l1rules;
using namespace stimcore;  // the shared stimulus primitives (enemy_stim_core.hpp), proven by the M1 probe
using game::WorldTransform;

// ---------------------------------------------------------------- addresses (ds.exe, image base 0x140000000)
constexpr std::uintptr_t kTsSite = 0x142E4F340;   // a reader of the thread-safe visual list: the AI manager global
constexpr std::uintptr_t kRegSite = 0x142E5FB61;  // the classifier's vehicle case: the actor registry global, vt+0x40
constexpr std::uintptr_t kClsVa = 0x142E5FA40, kClsSub = 0x142E5FAB9, kClsFlags = 0x142E5FAFB;
constexpr std::uintptr_t kAppVa = 0x142E59860, kAppStride = 0x142E598E0;
constexpr std::uintptr_t kPsrVa = 0x142E8DAF0, kPsrT1a = 0x142E8DD50, kPsrT1b = 0x142E8DD89, kPsrClear = 0x142E8DFFC, kKnowVt = 0x143E13028,
                         kKInit = 0x1422EE9B5, kPsrForce = 0x142E8E005, kPsrForcePos = 0x142E8E06B;
constexpr std::uintptr_t kMuleMgr = 0x147BF09D8, kMuleVt = 0x143DB7078;
constexpr std::uintptr_t kMuleSite = 0x1428B35F0;  // a reader of the MULE manager global: +0x38 the members, +0x30 their count
// the player objects (review: the classifier's flags and CanShoot's point are the PLAYER OBJECT's +0x2B0 block, not the entity's):
// [0x147BC7568] +0x40 the players, +0x38 their count (0x1423C60B0: player(i) by +0x88); a player's +0x70 is its entity
// (GetLocalDSPlayerEntity 0x142757A30); +0x2B0 its info block (0x14283EC70), whose +0x198 is the point CanShoot returns for id 0
constexpr std::uintptr_t kPlayerMgr = 0x147BC7568, kPlayerMgrSite = 0x142757A34, kPlayerFind = 0x1423C60B0, kPlayerEntSite = 0x142757A58,
                         kPinfoSite = 0x14283EC8C, kAimPtSite = 0x142EC6C72;
constexpr std::uintptr_t kPlEntity = 0x70, kPlInfo = 0x2B0, kPiAimPt = 0x198;
// the redirects (docs/enemy-sync.md 16.4)
constexpr std::uintptr_t kCanShoot = 0x142EC6C30;
constexpr std::uintptr_t kTurnVt = 0x143E1B228, kTurnStart = 0x142F088A0, kTurnUpdate = 0x142F0E540, kTurnAimSite = 0x142F0E562;
constexpr std::uintptr_t kRangeVt = 0x143E1B168, kRangeStart = 0x142F06270, kSymInSite = 0x142F062BC, kSymOutSite = 0x142F06382, kSymIn = 0x147BFB918,
                         kSymOut = 0x147BFB920;
// review: vtable 0x143E1B4C0 is DSAIHTNPrimitiveTaskVehicleMove and its goal AIBodyGoalDSVehicleMove (a drive destination, not
// a weapon point): its own redirect bit 16 (the vehicle chase), her FEET
constexpr std::uintptr_t kVehVt = 0x143E1B4C0, kVehStart = 0x142F09340, kVehUpdate = 0x142F0E770, kVehGoalSite = 0x142F0E796;
constexpr std::uintptr_t kMoveVt = 0x143E1B028, kMoveUpdate = 0x142F0F290, kMoveBlock = 0x142F0F2C1, kMoveFlag = 0x142F0F659, kMoveId = 0x142F0F668,
                         kMoveOverride = 0x142F0F7D6, kMoveRttiSite = 0x142F0F577;
constexpr std::uintptr_t kAimRtti = 0x144C922A0, kVehRtti = 0x144C916E0, kMoveRtti = 0x144C91610;
// (round 27 run 2: no goal cache) the goals' vtables, each found by the only data reference to its GetRTTI getter (vtable[0],
// `lea rax, [rip + rtti]; ret`; mem::RttiOf decodes the same slot): AIBodyGoalDSTurn (the aim goal), AIBodyGoalDSVehicleMove,
// AIBodyGoalDSWalk (the move goal). Byte-checked: the vtable's slot 0 is the getter and the getter returns the RTTI.
constexpr std::uintptr_t kAimGoalVt = 0x143E19B00, kAimGetter = 0x142EFD2C0, kVehGoalVt = 0x143E19728, kVehGetter = 0x142EFD2D0,
                         kMoveGoalVt = 0x143E196B0, kMoveGetter = 0x142EFD300;
// redirect 32, the shoot actor (round 27 run 2; docs/enemy-sync.md 16.4): the body shoot job (0x142EFF4B0, a worker) gives the
// shoot goal's actor id to the MULE's weapon carrier (DSNpcWeaponComponent + 0x50, vtable 0x143DC7B78; +0xA0 its mode: 0 actor,
// 1 place, 2 none; [carrier - 8] its entity) by vt+0xC0 SetTargetActor (call site 0x142EFFA2B, returning to 0x142EFFA41), then
// reads the aim back by vt+0xD0 GetTargetPosition (call site 0x142EFFA9F, returning to 0x142EFFAB0), which turns id 0 into our
// Sam's live body: the muzzle. vt+0xB8 SetTargetPos (0x142BC0760) is the game's own place target (early-out when the place is
// close: safe every frame). Three vtables share these functions: the detours check the carrier's vtable and the return address.
constexpr std::uintptr_t kCarrierVt = 0x143DC7B78, kCarSetActor = 0x142BC0880, kCarSetPos = 0x142BC0760, kCarGetPos = 0x142BAECA0;
constexpr std::uintptr_t kJobVa = 0x142EFF4B0, kJobSetSite = 0x142EFFA2B, kJobSetRet = 0x142EFFA41, kJobGetSite = 0x142EFFA9F, kJobGetRet = 0x142EFFAB0;
constexpr std::uintptr_t kCarMode = 0xA0, kCarEntity = 8;  // (the entity at carrier - 8)
// (run 3) the MULE's carrier is DSMuleWeaponComponent + 0x50 (its constructor 0x142AEE400 runs the base constructor 0x142BA4EA0,
// then stores vtable 0x143DBA800 at +0x50: 0x142AEE446). That vtable's slots are wrappers: +0xC0 (0x142B1FEA0) tests the
// component's type (0x142BAE000 on this - 0x50; type 0x36 skips) and tail-jumps to the base SetTargetActor with the same `this`
// and return address (so kJobSetRet holds); +0xD0 (0x142AFB790) CALLS the base GetTargetPosition (return address 0x142AFB7B8,
// never kJobGetRet: hooked on its own); +0xB8 (0x142B1FE60) calls the base SetTargetPos unless type 0x36. Run 2's filter took only
// the plain NPC vtable: every MULE call was dropped (calls 0, points 0 all run).
constexpr std::uintptr_t kMuleCarVt = 0x143DBA800, kMuleSetPos = 0x142B1FE60, kMuleSetActor = 0x142B1FEA0, kMuleGetPos = 0x142AFB790,
                         kMuleGetRet = 0x142AFB7B8, kMuleCtorStore = 0x142AEE446;
constexpr std::uintptr_t kMuleSetActorType = 0x142B1FEB4, kMuleSetActorJmp = 0x142B1FED5, kMuleGetPosType = 0x142AFB7A9, kMuleGetPosCall = 0x142AFB7B3;
static_assert(kMuleGetRet == kMuleGetPosCall + 5, "the MULE wrapper's base call returns to kMuleGetRet");

// offsets
constexpr std::uintptr_t kActId = 0x08, kActInUse = 0x20, kActState = 0x24, kActEntRef = 0x68;
constexpr std::uintptr_t kIndK = 0x3AD8, kIndComp = 0x3690;
constexpr std::uintptr_t kKX = 0x28, kXAgent = 0x118, kKBucketN = 0x590, kKBuckets = 0x598, kKeType = 0x18, kKePos = 0x20, kKeAge = 0x40, kKeSize = 0x80;
constexpr std::uintptr_t kKForcePos = 0x6A0, kKForce = 0x736;
constexpr std::uintptr_t kEOwnId = 0x314, kEParent = 0x70, kPFlags = 0x2B0, kPWeapon = 0x3F9;
constexpr std::uint32_t kIdMask = 0x1FFFFF;

// the bits
// (M3f) 16: the line-of-sight keep (SightTick's estimate keeps her the target while the bucket lost her record; needs 1);
// 32: her record from ours (that bucket's record[0], our Sam's, rewritten into hers; needs 1 and 16)
enum : unsigned { kPickReorder = 1, kPickH5 = 2, kPickH2 = 4, kPickForce = 8, kPickSight = 16, kPickSynth = 32 };
enum : unsigned { kRedShoot = 1, kRedTurn = 2, kRedApproach = 4, kRedShadow = 8, kRedVehicle = 16, kRedShootActor = 32 };
// the redirect sites (one bit each; redirect bit 2 = 2a | 2b; bit 16 = the vehicle chase; bit 32 = the shoot actor, whose hooks
// are in with bit 32 on, or in a hidden run while the pick or a redirect is wanted: there the aim point is recorded with bit 32
// off too)
enum : unsigned { kS1 = 1, kS2a = 2, kS2b = 4, kSVeh = 8, kS4 = 16, kS32 = 32 };
// the byte-check groups
enum : unsigned { kGStim = 1, kGReg = 2, kGGoal = 4, kGPsr = 8, kGCls = 16, kGClsFlags = 32, kGForce = 64, kGS1 = 128, kGS2a = 256, kGS2b = 512,
                  kGVeh = 1024, kGS4 = 2048, kGMule = 4096, kGPlayer = 8192, kGS32 = 16384 };

struct Site {
    std::uintptr_t va;
    const char* sig;
    const char* name;
    unsigned group;
};
// clang-format off
constexpr Site kSites[] = {
    {kVisCreate,    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 40 81 B9 10 A0 01 00 00 02 00 00", "the visual stimulus create", kGStim},
    {kStimRemove,   "48 89 74 24 20 57 48 83 EC 30 80 BA 84 00 00 00 00 48 8B FA 48 8B F1 0F 85", "the persistent stimulus remove", kGStim},
    {kResGet,       "48 89 5C 24 20 57 48 83 EC 20 48 8B 01 33 C9 89 4C 24 30 48 89 74 24 40 48 8B 38 48 85 FF 74 33", "the streaming ref getter", kGStim},
    {kTsSite,       "48 8B 1D ?? ?? ?? ?? 48 8B 9B 98 C3 03 00", "the thread-safe visual list (+0x3C398)", kGStim},
    {kRegSite,      "48 8B 0D ?? ?? ?? ?? 48 8D 55 C0 48 8B 01 FF 50 40", "the registry's player id read (vt+0x40)", kGReg},
    {kGoalFind,     "40 53 48 83 EC 30 48 8D 99 B0 00 00 00 48 89 54 24 20 48 8B CB 48 8D 54 24 20 E8", "the goal lookup", kGGoal},
    {kPsrVa,        "4C 8B DC 55 53 57 49 8D 6B 88 48 81 EC 60 01 00 00 C4 C1 78 29 73 B8", "ProcessSensorResults", kGPsr},
    {kPsrT1a,       "83 7B 08 00 0F 84", "its type-1 bucket count (+8)", kGPsr},
    {kPsrT1b,       "4C 8B 43 10 BA 01 00 00 00", "its type-1 records (+0x10)", kGPsr},
    {kPsrClear,     "44 89 40 08", "its bucket count clear", kGPsr},
    {kAppStride,    "48 63 03 48 8B 7C 24 38 48 8B C8 48 C1 E1 06 48 03 CA 74 38 48 8B 06 48 89 01", "the append's 0x40 record stride", kGPsr},
    {kKInit,        "4C 89 49 28", "K+0x28 (the individual) at its init", kGPsr},
    {kClsVa,        "40 55 53 56 57 41 54 41 56 41 57 48 8B EC 48 83 EC 60 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 F0 8B 9A 88 00 00 00", "the classifier", kGCls},
    {kClsSub,       "C1 EF 0D 83 FF 21 0F 87 1F 04 00 00", "the classifier's actor type switch", kGCls},
    {kClsFlags,     "48 8B 0D 66 7A D6 04 33 D2 E8 A7 65 56 FF 8B 88 B0 02 00 00 0F BA E1 0B 0F 83 CE 01 00 00 85 C9 0F 89 C3 03 00 00", "the classifier's player flags (+0x2B0 bits 11 / 31)", kGClsFlags},
    {kPsrForce,     "80 BF 36 07 00 00 00", "PSR's forced visual byte (K+0x736)", kGForce},
    {kPsrForcePos,  "C5 F8 10 87 A0 06 00 00", "PSR's forced visual place (K+0x6A0)", kGForce},
    {kCanShoot,     "48 89 5C 24 18 55 56 57 48 83 EC 60 8B DA 48 8B E9 48 8B 0D ?? ?? ?? ?? 81 E2 FF FF 1F 00 49 8B F0 E8 ?? ?? ?? ??", "CanShoot's aim point (bit 1)", kGS1},
    {kTurnStart,    "4C 8B DC 49 89 5B 10 49 89 73 18 49 89 7B 20 41 56", "the turn task's Start (bit 2a)", kGS2a},
    {kTurnUpdate,   "48 89 5C 24 10 57 48 81 EC A0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 90 00 00 00 48 8B D9", "the turn task's Update (bit 2a)", kGS2a},
    {kTurnAimSite,  "48 8D 15 ?? ?? ?? ?? 48 8B 49 10 48 81 C1 70 36 00 00 E8", "the turn Update's aim goal lookup (bit 2a)", kGS2a},
    {kRangeStart,   "48 89 5C 24 10 55 48 8B EC 48 83 EC 60 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 F8", "the range-check task's Start (bit 2b)", kGS2b},
    {kVehStart,     "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55", "the vehicle-move task's Start (bit 16)", kGVeh},
    {kVehUpdate,    "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 70 48 8B 05 ?? ?? ?? ??", "the vehicle-move task's Update (bit 16)", kGVeh},
    {kVehGoalSite,  "48 8D 15 ?? ?? ?? ?? 48 8B 49 10 49 8B F0 48 81 C1 70 36 00 00 E8", "the vehicle-move Update's goal lookup (bit 16)", kGVeh},
    {kMoveUpdate,   "40 55 53 56 57 41 54 41 55 41 57 48 8D 6C 24 F0", "the move task's Update (bit 4)", kGS4},
    {kMoveBlock,    "B8 02 00 00 00 4C 8B BA 90 36 00 00 41 80 BF 24 4D 00 00 00 49 8D 8F A0 02 00 00 75 06 8B 81 80 4A 00 00", "the move Update's request block (bit 4)", kGS4},
    {kMoveFlag,     "0F B6 4E 68 B0 01 F6 C1 02", "the move goal's actor flag (bit 4)", kGS4},
    {kMoveId,       "8B 53 28", "the move task's actor id (bit 4)", kGS4},
    {kMoveOverride, "F6 87 B8 04 00 00 40 74 1F C5 F8 10 87 F8 02 00 00", "the move Update's place override (bit 4)", kGS4},
    {kMoveRttiSite, "48 8D 15 ?? ?? ?? ??", "the move Update's goal type (bit 4)", kGS4},
    {kMuleSite,     "48 8B 05 ?? ?? ?? ?? 48 8B 48 38 48 63 40 30", "the MULE manager's members (+0x38 / +0x30)", kGMule},
    {kPlayerMgrSite, "48 8B 0D ?? ?? ?? ?? 48 85 C9 75 07", "the player manager global (GetLocalDSPlayerEntity)", kGPlayer},
    {kPlayerEntSite, "48 8B 40 70 48 83 C4 28 C3", "a player's entity (+0x70)", kGPlayer},
    {kPlayerFind,   "48 8B 41 40 48 63 49 38 4C 8D 04 C8 49 3B C0", "player(i): the players (+0x40 / +0x38)", kGPlayer},
    {kPinfoSite,    "48 05 B0 02 00 00 48 83 C4 28 C3", "a player's info block (+0x2B0)", kGPlayer},
    {kAimPtSite,    "E8 ?? ?? ?? ?? C5 F8 10 80 98 01 00 00 C5 F8 11 06", "CanShoot's id 0 point (info +0x198)", kGPlayer},
    {kAimGetter,    "48 8D 05 ?? ?? ?? ?? C3", "the aim goal's type getter (bit 2a)", kGS2a},
    {kVehGetter,    "48 8D 05 ?? ?? ?? ?? C3", "the vehicle-move goal's type getter (bit 16)", kGVeh},
    {kMoveGetter,   "48 8D 05 ?? ?? ?? ?? C3", "the move goal's type getter (bit 4)", kGS4},
    {kCarSetActor,  "40 53 48 83 EC 30 83 B9 A0 00 00 00 00 48 8B D9 75 0C 39 91 88 00 00 00 0F 84 85 00 00 00", "the carrier's SetTargetActor (bit 32)", kGS32},
    {kCarSetPos,    "40 53 48 83 EC 30 83 B9 A0 00 00 00 01 48 8B D9 75 60 C5 FB 10 81 98 00 00 00", "the carrier's SetTargetPos (bit 32)", kGS32},
    {kCarGetPos,    "48 8B C4 55 53 57 41 56 48 8D A8 E8 FE FF FF 48 81 EC F8 01 00 00 C5 F8 29 70 C8 C5 F8 29 78 B8", "the carrier's GetTargetPosition (bit 32)", kGS32},
    {kJobVa,        "40 55 57 48 8D 6C 24 F8 48 81 EC 08 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 B0 48 8B 41 58 48 8B F9", "the body shoot job (bit 32)", kGS32},
    {kJobSetSite,   "81 FB FF FF 1F 00 74 10 48 8B 06 8B D3 48 8B CE FF 90 C0 00 00 00 EB 29", "the shoot job's SetTargetActor call (bit 32)", kGS32},
    {kJobGetSite,   "48 8B 06 48 8D 54 24 68 48 8B CE FF 90 D0 00 00 00 48 8D 4C 24 68", "the shoot job's GetTargetPosition call (bit 32)", kGS32},
    {kMuleSetActorType, "48 83 C1 B0 E8 ?? ?? ?? ?? 3C 36 74 19", "the MULE carrier's SetTargetActor type test (bit 32)", kGS32},
    {kMuleSetActorJmp,  "E9", "the MULE carrier's SetTargetActor tail jump (bit 32)", kGS32},
    {kMuleGetPos,       "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8B DA 48 83 C1 B0 E8", "the MULE carrier's GetTargetPosition (bit 32)", kGS32},
    {kMuleGetPosType,   "3C 36 74 19 48 8B D3 48 8B CF E8", "the MULE carrier's GetTargetPosition type test (bit 32)", kGS32},
    {kMuleGetRet,       "48 8B C3", "the MULE carrier's GetTargetPosition after its base call (bit 32)", kGS32},
    {kMuleCtorStore,    "48 8D 05", "the MULE weapon component's carrier vtable store (bit 32)", kGS32},
};
// clang-format on

// ---------------------------------------------------------------- SEH reads and writes (any thread; POD only)
bool Bad(std::uintptr_t a) noexcept { return a < 0x10000; }
bool SQ(std::uintptr_t a, std::uintptr_t* out) noexcept {
    if (Bad(a)) return false;
    __try {
        *out = *reinterpret_cast<volatile std::uintptr_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// the same read in a function that is not noexcept (its scope uses __C_specific_handler, not the _noexcept variant):
// only op 10 uses it, to test that the crash-reporter guard knows both handler kinds
__declspec(noinline) bool SQx(std::uintptr_t a, std::uintptr_t* out) {
    if (Bad(a)) return false;
    __try {
        *out = *reinterpret_cast<volatile std::uintptr_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SU32(std::uintptr_t a, std::uint32_t* out) noexcept {
    if (Bad(a)) return false;
    __try {
        *out = *reinterpret_cast<volatile std::uint32_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SU8(std::uintptr_t a, std::uint8_t* out) noexcept {
    if (Bad(a)) return false;
    __try {
        *out = *reinterpret_cast<volatile std::uint8_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SF32(std::uintptr_t a, float* out) noexcept {
    if (Bad(a)) return false;
    __try {
        *out = *reinterpret_cast<volatile float*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SF64(std::uintptr_t a, double* out) noexcept {
    if (Bad(a)) return false;
    __try {
        *out = *reinterpret_cast<volatile double*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SPut(std::uintptr_t dst, const void* src, std::size_t n) noexcept {
    if (Bad(dst)) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(dst), src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SGet(std::uintptr_t src, void* dst, std::size_t n) noexcept {
    if (Bad(src)) return false;
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// two 0x40-byte sensor records swapped (eight u64 each; the records are POD)
// one byte's bits set / cleared atomically (the game's own workers may touch the same byte); `old` the byte before
bool SOr8(std::uintptr_t a, std::uint8_t bits, std::uint8_t* old) noexcept {
    if (Bad(a)) return false;
    __try {
        *old = static_cast<std::uint8_t>(_InterlockedOr8(reinterpret_cast<volatile char*>(a), static_cast<char>(bits)));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SAnd8(std::uintptr_t a, std::uint8_t mask, std::uint8_t* old) noexcept {
    if (Bad(a)) return false;
    __try {
        *old = static_cast<std::uint8_t>(_InterlockedAnd8(reinterpret_cast<volatile char*>(a), static_cast<char>(mask)));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SSwap64(std::uintptr_t a, std::uintptr_t b) noexcept {
    if (Bad(a) || Bad(b)) return false;
    __try {
        auto* x = reinterpret_cast<volatile std::uint64_t*>(a);
        auto* y = reinterpret_cast<volatile std::uint64_t*>(b);
        for (int i = 0; i < 8; ++i) {
            const std::uint64_t t = x[i];
            x[i] = y[i];
            y[i] = t;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uint32_t U32(std::uintptr_t a) noexcept {
    std::uint32_t v = 0;
    mem::Rd(a, v);
    return v;
}
float F32(std::uintptr_t a) noexcept {
    float v = 0;
    mem::Rd(a, v);
    return v;
}
std::uint8_t U8(std::uintptr_t a) noexcept {
    std::uint8_t v = 0;
    mem::Rd(a, v);
    return v;
}
bool Pos(std::uintptr_t e, double (&p)[3]) noexcept {
    std::uint8_t b[0x18];
    if (!e || !mem::Read(e + 0xC8, b, sizeof(b))) return false;
    std::memcpy(p, b, sizeof(b));
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) && (p[0] != 0 || p[1] != 0 || p[2] != 0);
}
std::uintptr_t Individual(std::uintptr_t e) noexcept {
    const std::uintptr_t c = e ? partner::FindComponentByName(reinterpret_cast<game::Entity*>(e), "AIIndividualComponent") : 0;
    return c ? mem::Q(c + 0x50) : 0;
}
bool KnowOf(std::uintptr_t k, std::uintptr_t agent) noexcept {
    if (!k || mem::Q(k) != A(kKnowVt)) return false;
    const std::uintptr_t x = mem::Q(k + kKX);
    return x && mem::Q(x + kXAgent) == agent;
}

// ---------------------------------------------------------------- the clock the workers share (ms; 0 = never)
std::uint64_t g_epoch = 0;
std::uint64_t TNow() noexcept { return GetTickCount64() - g_epoch + 1; }

// ---------------------------------------------------------------- state the workers read (atomics only)
std::atomic<bool> g_off{false};                     // the emergency stop / a fault / esync's stop: every detour passes through
std::atomic<unsigned> g_pick_live{0}, g_redir_live{0};  // the effective bits (key | override, byte checks, hooks in; 0 when off)
std::atomic<bool> g_rec_live{false};  // bit 32's hooks in and the table usable: the job's aim point recorded (read-only, any bit)
// the goals' vtables (relocated; 0 when their byte checks failed): the goal-backed redirects write only to a goal with this vtable
std::atomic<std::uintptr_t> g_aim_vt{0}, g_veh_vt{0}, g_move_vt{0};
static_assert(kJobSetRet == kJobSetSite + 22 && kJobGetRet == kJobGetSite + 17, "the shoot job's return addresses follow its call sites");
std::atomic<std::uint64_t> g_fresh_ms{1500};
std::atomic<std::uintptr_t> g_p_agent{0}, g_l_sam{0};  // the partner stimulus' agent (live and ours), our Sam's entity
std::atomic<std::uint32_t> g_her_flags{0}, g_own_flags{0};          // H2: the partner's +0x2B0 (SPV1; 0 stale), our Sam's
std::atomic<std::uintptr_t> g_sym_in{0}, g_sym_out{0};              // 2b's symbols ([0x147BFB918] / [0x147BFB920])
// the emergency stop's view of the partner stimulus
std::atomic<std::uintptr_t> g_ps_stim{0}, g_ps_agent{0}, g_ps_res{0}, g_ps_mgr{0};

// the partner record (seqlock; game thread writes once per tick, after the stimulus tick, valid only while our partner stimulus is
// live): feet = the body's root; the aim point = feet + her SPV1 aim height (her own player object's info +0x198 above her root,
// 0.30..2.20 m) when fresh, else +1.30 m
struct PartnerRec {
    std::atomic<std::uint32_t> seq{0};
    std::atomic<double> feet[3], chest[3];
    std::atomic<std::uint64_t> ms{0};
    std::atomic<bool> valid{false};
} g_prec;
enum : int { kFeet = 0, kChest = 1 };
constexpr double kChestDefault = 1.30;
void PubPartner(bool ok, const double (&p)[3], double chest_dz) noexcept {
    const std::uint32_t s = g_prec.seq.load(std::memory_order_relaxed);
    g_prec.seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (int i = 0; i < 3; ++i) {
        g_prec.feet[i].store(ok ? p[i] : 0.0, std::memory_order_relaxed);
        g_prec.chest[i].store(ok ? p[i] + (i == 2 ? chest_dz : 0.0) : 0.0, std::memory_order_relaxed);
    }
    g_prec.ms.store(ok ? TNow() : 0, std::memory_order_relaxed);
    g_prec.valid.store(ok, std::memory_order_relaxed);
    g_prec.seq.store(s + 2, std::memory_order_release);
}
bool ReadPartner(int which, double (&out)[3], std::uint64_t now) noexcept {
    for (int t = 0; t < 4; ++t) {
        const std::uint32_t s1 = g_prec.seq.load(std::memory_order_acquire);
        if (s1 & 1) continue;
        const bool valid = g_prec.valid.load(std::memory_order_relaxed);
        const std::uint64_t ms = g_prec.ms.load(std::memory_order_relaxed);
        for (int i = 0; i < 3; ++i) out[i] = (which == kChest ? g_prec.chest[i] : g_prec.feet[i]).load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_prec.seq.load(std::memory_order_relaxed) != s1) continue;
        return valid && ms && now >= ms && now - ms <= 500;
    }
    return false;
}

// ---------------------------------------------------------------- the tgt table (512 slots keyed by K; the game thread inserts)
constexpr unsigned kSlots = 512, kProbe = 16;
struct Slot {
    std::atomic<std::uintptr_t> key{0};  // K; 0 empty, 1 a tombstone (reused after 2 s)
    std::atomic<std::uintptr_t> ind{0};
    std::atomic<std::uint32_t> id{0};
    std::atomic<std::uint8_t> cur{0}, rule{0}, force{0};
    std::atomic<std::uint64_t> since{0}, seen_p{0}, seen_l{0}, hit_p{0}, hit_l{0};
    std::atomic<float> dp{std::numeric_limits<float>::infinity()}, dl{std::numeric_limits<float>::infinity()};
    std::atomic<std::uint32_t> n_front{0}, n_supp{0}, n_calls{0};
    std::atomic<std::uint32_t> flips{0};                     // her -> Sam changes of the pick (the workers; op 4 resets)
    std::atomic<std::uint64_t> last_p{0};                    // her record last in its PlayerVisual bucket (ms)
    std::atomic<std::uint32_t> max_gap{0};                   // the longest gap between two of those (ms; op 4 resets)
    std::atomic<std::uintptr_t> ent{0};                      // its entity (bit 32's carrier -> slot index; the game thread sets it)
    // bit 32 (the workers; op 4 resets): the shoot job's id-0 SetTargetActor calls seen, those redirected, the aim points recorded
    std::atomic<std::uint32_t> sh_calls{0}, sh_redir{0}, sh_pts{0};
    // (run 3) those aim points by the carrier's mode after the call: 0 actor, 1 place, anything else (op 4 resets)
    std::atomic<std::uint32_t> pt_actor{0}, pt_place{0}, pt_other{0};
    // (run 3 review) the job's GetTargetPosition calls on this MULE's carrier whose wrapper took the component-type-0x36 branch
    // (never reaching the base functions: no SetTargetActor call is seen; op 4 resets)
    std::atomic<std::uint32_t> sh_t36{0};
    // (run 3) the rule of the last change of `cur` (the workers store it before `cur`): a rule-3 switch reads "sticky" one tick
    // later, so the 4 Hz change log prints this one
    std::atomic<std::uint8_t> chg_rule{0};
    // the job's last aim point (its GetTargetPosition result; a seqlock) and the carrier's mode after it
    std::atomic<std::uint32_t> pt_seq{0};
    std::atomic<double> pt[3];
    std::atomic<std::uint64_t> pt_ms{0};
    std::atomic<std::int32_t> pt_mode{-1};
    std::atomic<std::uint32_t> own_seq{0};
    std::atomic<double> own[3];
    std::atomic<std::uint64_t> own_ms{0};
    std::atomic<std::uint64_t> reg_ms{0};  // the last registration pass that listed it: the redirects refuse it older than kRegStaleMs
    // (M3f) pick bit 16: SightTick's last verdict on her (erules::Sight; the game thread stores it, then its time, released) and
    // what PsrOne did with it (the workers; op 4 resets): ticks kept by it, her record written from ours, a synth refused
    std::atomic<std::uint8_t> los_v{0};
    std::atomic<std::uint64_t> los_ms{0};
    std::atomic<std::uint32_t> n_est_keep{0}, n_synth{0}, n_synth_skip{0};
    // (M3f review) the last synth's record fields (the SIGHT line): +0x24 and +0x28 as kept (our Sam's), the velocity at +0x30
    // before (our Sam's) and after (hers, from her stimulus +0x90); x / y only
    std::atomic<float> syn_f24{0}, syn_f28{0}, syn_vs[2] = {}, syn_vh[2] = {};
    std::atomic<std::uint64_t> first_p{0};  // her record first in its bucket since the arm's start (ms; op 4 resets)
    // game thread only
    std::uint64_t arm_ms = 0;               // (M3f) the arm's start (TNow; op 4)
    std::uint64_t los_next = 0;             // its next ray
    unsigned los_n[9] = {};                 // its verdicts by erules::Sight (op 4 resets)
    std::uint8_t los_logged = 0;            // the last verdict logged
    unsigned los_logs = 0;                  // the changes logged in this arm (60 at most)
    std::uint64_t tomb_ms = 0;
    std::uintptr_t entity = 0, agent = 0;
    std::uint8_t logged_cur = 0, logged_rule = 0;
    unsigned pass = 0;
    std::uint64_t pt_seen_ms = 0;            // the last aim point the 4 Hz tally attributed
    unsigned pt_her = 0, pt_sam = 0, pt_none = 0;  // those aim points by the player body they were at (op 4 resets)
    bool mm_logged = false;                  // (run 3) its first pick / place mismatch logged in this arm (op 4 clears)
};
Slot g_slots[kSlots];
constexpr std::uint64_t kRegStaleMs = 1500;  // registration runs at 1 Hz
struct IndIx {
    std::atomic<std::uintptr_t> key{0};  // the individual; 0 empty, 1 a tombstone
    std::atomic<std::int32_t> slot{-1};
    std::uint64_t tomb_ms = 0;
};
IndIx g_ix[kSlots];  // the individual -> its slot
IndIx g_ex[kSlots];  // the entity -> its slot (bit 32: the carrier's [this - 8])

Slot* Find(std::uintptr_t k) noexcept {
    if (k <= 1) return nullptr;
    const unsigned h = R::TgtHash(k);
    for (unsigned i = 0; i < kProbe; ++i) {
        Slot& s = g_slots[(h + i) & (kSlots - 1)];
        const std::uintptr_t v = s.key.load(std::memory_order_acquire);
        if (v == k) return &s;
        if (v == 0) return nullptr;
    }
    return nullptr;
}
// a slot by one of its indexes (the individual's, or the entity's with `by_ent`): registered within kRegStaleMs and still holding it
Slot* FindIx(const IndIx* tab, std::uintptr_t key, bool by_ent) noexcept {
    if (key <= 1) return nullptr;
    const unsigned h = R::TgtHash(key);
    for (unsigned i = 0; i < kProbe; ++i) {
        const IndIx& e = tab[(h + i) & (kSlots - 1)];
        const std::uintptr_t v = e.key.load(std::memory_order_acquire);
        if (v == key) {
            const std::int32_t n = e.slot.load(std::memory_order_acquire);
            if (n < 0 || n >= static_cast<std::int32_t>(kSlots)) return nullptr;
            Slot& s = g_slots[n];
            if (s.key.load(std::memory_order_acquire) <= 1 || (by_ent ? s.ent : s.ind).load(std::memory_order_relaxed) != key) return nullptr;
            const std::uint64_t reg = s.reg_ms.load(std::memory_order_relaxed), now = TNow();
            return reg && now >= reg && now - reg <= kRegStaleMs ? &s : nullptr;
        }
        if (v == 0) return nullptr;
    }
    return nullptr;
}
Slot* FindInd(std::uintptr_t ind) noexcept { return FindIx(g_ix, ind, false); }
Slot* FindEnt(std::uintptr_t ent) noexcept { return FindIx(g_ex, ent, true); }
bool ReadOwn(Slot* s, double (&out)[3], std::uint64_t now) noexcept {
    for (int t = 0; t < 4; ++t) {
        const std::uint32_t s1 = s->own_seq.load(std::memory_order_acquire);
        if (s1 & 1) continue;
        const std::uint64_t ms = s->own_ms.load(std::memory_order_relaxed);
        for (int i = 0; i < 3; ++i) out[i] = s->own[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s->own_seq.load(std::memory_order_relaxed) != s1) continue;
        return ms && now >= ms && now - ms <= 1000;
    }
    return false;
}
// the redirects' test: our partner stimulus live, this individual registered within kRegStaleMs and still holding the same K, its
// pick the partner, her record valid and fresh (< 500 ms)
bool WantHer(std::uintptr_t ind, int which, double (&out)[3], Slot** so = nullptr) noexcept {
    if (!g_p_agent.load(std::memory_order_relaxed)) return false;
    Slot* s = FindInd(ind);
    if (!s || s->cur.load(std::memory_order_relaxed) != R::kPartner) return false;
    std::uintptr_t k = 0;
    if (!SQ(ind + kIndK, &k) || k != s->key.load(std::memory_order_relaxed)) return false;
    if (so) *so = s;
    return ReadPartner(which, out, TNow());
}

// H5: the MULE members' actor ids (bitset over id & 0x1FFF, double-buffered; the game thread publishes the index)
std::atomic<std::uint64_t> g_h5[2][128];
std::atomic<int> g_h5_ix{0};
std::atomic<bool> g_h5_ready{false};
bool H5Has(std::uint32_t own) noexcept {
    const std::uint32_t b = own & 0x1FFFu;
    const int ix = g_h5_ix.load(std::memory_order_acquire);
    return (g_h5[ix][b >> 6].load(std::memory_order_relaxed) >> (b & 63)) & 1u;
}

// H3: hits (any thread) -> the game thread (ring of 64: ms 40 bits | id 21 bits | who 2 bits)
std::atomic<std::uint64_t> g_hits[64];
std::atomic<std::uint32_t> g_hit_head{0};
void PushHit(std::uint32_t id, std::uint8_t who) noexcept {
    const std::uint64_t v = ((TNow() & 0xFFFFFFFFFFull) << 23) | (static_cast<std::uint64_t>(id & kIdMask) << 2) | (who & 3u);
    g_hits[g_hit_head.fetch_add(1, std::memory_order_relaxed) & 63u].store(v, std::memory_order_release);
}

// the worker counters (the tick logs them)
struct WCounts {
    std::atomic<std::uint32_t> faults{0}, h5{0}, h2_refuse{0}, h2_fix{0}, h2_nobucket{0}, psr{0}, r1{0}, r2a{0}, r2a_hold{0}, r2a_off{0}, r2b{0}, rveh{0},
        r4{0}, r4_restore_skip{0}, r4_not_ours{0};
    // bit 32 (all MULEs): the shoot job's id-0 SetTargetActor calls on a registered MULE, those redirected, the aim points recorded
    std::atomic<std::uint32_t> sh_calls{0}, sh_redir{0}, sh_pts{0};
    // soft counters (round 27 run 2): a refused read or a failed check that is NOT a fault (no stop; the 5 s line prints them).
    // onhit_bad: an OnHit attacker chain object whose vtable is not the game's; psr_rec_bad: a PlayerVisual record whose stimulus
    // did not read; turn_goal_bad: a fresh aim / vehicle-move / move goal whose vtable is not its type's (byte-checked)
    std::atomic<std::uint32_t> onhit_bad{0}, psr_rec_bad{0}, turn_goal_bad{0};
    // (run 3) bit 32's filter: a shoot-job SetTargetActor(0) on a carrier of another vtable / on one whose entity is not a
    // registered MULE (not counted as calls)
    std::atomic<std::uint32_t> sh_vt_other{0}, sh_noslot{0};
    // (run 3 review) the job's GetTargetPosition calls through the MULE wrapper that did not reach the base GetTargetPosition: the
    // component-type-0x36 branch, where the base SetTargetActor is never called either (calls 0 with this > 0)
    std::atomic<std::uint32_t> sh_t36{0};
    // (M3f) pick bits 16 / 32 (all MULEs): ticks kept by the line of sight, her record written from ours, a synth refused by its
    // checks; onhit_odd: an OnHit attacker of the game's whose +0x338 is not an object of the game's (information only)
    std::atomic<std::uint32_t> est_keep{0}, synth{0}, synth_skip{0}, onhit_odd{0};
} g_wc;
// (run 3) the first instance of each, kept by the worker that saw it (one compare-exchange) and logged once by the tick
struct FirstSeen {
    std::atomic<int> state{0};  // 0 none, 1 being written, 2 ready, 3 logged
    std::atomic<std::uint64_t> a{0}, b{0}, c{0}, d{0}, e{0};
    void Put(std::uint64_t x0, std::uint64_t x1, std::uint64_t x2, std::uint64_t x3, std::uint64_t x4) noexcept {
        int z = 0;
        if (!state.compare_exchange_strong(z, 1, std::memory_order_acq_rel)) return;
        a.store(x0, std::memory_order_relaxed), b.store(x1, std::memory_order_relaxed), c.store(x2, std::memory_order_relaxed);
        d.store(x3, std::memory_order_relaxed), e.store(x4, std::memory_order_relaxed);
        state.store(2, std::memory_order_release);
    }
    bool Take() noexcept {
        int r = 2;
        return state.compare_exchange_strong(r, 3, std::memory_order_acq_rel);
    }
};
FirstSeen g_first_vt_other, g_first_noslot, g_first_onhit_bad, g_first_onhit_odd;

// ---------------------------------------------------------------- the detours' bodies (worker threads; no alloc / log / game call)
using Fn4 = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
Fn4 o_shoot = nullptr, o_turn_s = nullptr, o_turn_u = nullptr, o_range = nullptr, o_veh_s = nullptr, o_veh_u = nullptr, o_move = nullptr,
    o_set_actor = nullptr, o_get_pos = nullptr, o_mule_get_pos = nullptr;
bool Live(unsigned site) noexcept { return (g_redir_live.load(std::memory_order_relaxed) & site) != 0; }

bool PartnerStim(std::uintptr_t stim) noexcept {
    const std::uintptr_t pa = g_p_agent.load(std::memory_order_relaxed);
    std::uintptr_t ag = 0;
    return pa && SQ(stim + kSAgent, &ag) && ag == pa;
}

// (M3f) pick bit 32: record[0] (our Sam's, `stim0` when PsrOne read it) rewritten into her stimulus, its +0x20 distance hers from
// the MULE and (M3f review) its +0x30..0x3F velocity hers (her stimulus +0x90: the visual sense's writer copies the stimulus's
// +0x90 there, and the entry builder 0x142E9FF30 turns it into a rate). Every input checked first: her stimulus live and ours,
// the MULE's own place and her chest fresh, the distance finite and in (0, 200] m, her velocity read, finite and at most
// kSynthVelMax, record[0] still `stim0` (re-read); else nothing written (synth_skip) and false (PsrOne suppresses instead). A
// refused write is a fault (the stop); true then (nothing more written).
constexpr float kSynthVelMax = 50.0f;
bool SynthOne(Slot* s, std::uintptr_t recs, std::uintptr_t stim0, std::uint64_t now) noexcept {
    const std::uintptr_t ps = g_ps_stim.load(std::memory_order_relaxed);
    double own[3] = {}, her[3] = {};
    std::uintptr_t again = 0;
    bool ok = stim0 && ps && ps != stim0 && PartnerStim(ps) && ReadOwn(s, own, now) && ReadPartner(kChest, her, now);
    double d = 0;
    if (ok) {
        const double dx = her[0] - own[0], dy = her[1] - own[1], dz = her[2] - own[2];
        d = std::sqrt(dx * dx + dy * dy + dz * dz);
        ok = std::isfinite(d) && d > 0 && d <= 200.0;
    }
    float hv[4] = {}, mine[8] = {};  // her stimulus's velocity; record +0x20..0x3F as it is (ours)
    ok = ok && SGet(ps + kSVel, hv, sizeof(hv)) && SGet(recs + 0x20, mine, sizeof(mine));
    for (int i = 0; ok && i < 3; ++i) ok = std::isfinite(hv[i]) && std::fabs(hv[i]) <= kSynthVelMax;
    ok = ok && SQ(recs, &again) && again == stim0;
    if (!ok) {
        s->n_synth_skip.fetch_add(1, std::memory_order_relaxed), g_wc.synth_skip.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const float df = static_cast<float>(d);
    if (!SPut(recs + 0x30, hv, sizeof(hv)) || !SPut(recs + 0x20, &df, sizeof(df)) || !SPut(recs, &ps, sizeof(ps))) {
        g_wc.faults.fetch_add(1);
        return true;
    }
    s->syn_f24.store(mine[1], std::memory_order_relaxed), s->syn_f28.store(mine[2], std::memory_order_relaxed);
    s->syn_vs[0].store(mine[4], std::memory_order_relaxed), s->syn_vs[1].store(mine[5], std::memory_order_relaxed);
    s->syn_vh[0].store(hv[0], std::memory_order_relaxed), s->syn_vh[1].store(hv[1], std::memory_order_relaxed);
    s->n_synth.fetch_add(1, std::memory_order_relaxed), g_wc.synth.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void PsrOne(Slot* s, std::uintptr_t k) noexcept {
    std::int32_t nb = 0;
    std::uintptr_t arr = 0, bucket = 0;
    if (!SU32(k + 0x40, reinterpret_cast<std::uint32_t*>(&nb)) || !SQ(k + 0x48, &arr)) return void(g_wc.faults.fetch_add(1));
    if (nb <= 0 || nb > 64 || !arr) return;
    for (std::int32_t b = 0; b < nb; ++b) {
        std::uintptr_t bp = 0;
        std::uint32_t type = 0;
        if (!SQ(arr + 8ull * static_cast<unsigned>(b), &bp) || !bp || !SU32(bp, &type)) continue;
        if (type == 1) {
            bucket = bp;
            break;
        }
    }
    std::int32_t count = 0;
    std::uintptr_t recs = 0;
    if (bucket && (!SU32(bucket + 8, reinterpret_cast<std::uint32_t*>(&count)) || !SQ(bucket + 0x10, &recs))) return void(g_wc.faults.fetch_add(1));
    const int n = bucket && recs && count > 0 ? (count > 16 ? 16 : count) : 0;
    const std::uintptr_t pa = g_p_agent.load(std::memory_order_relaxed);
    std::uint8_t who[16] = {};
    std::uintptr_t stim0 = 0;  // record[0]'s stimulus as read here (the synth writes only over this one)
    R::PickIn x;
    x.now = TNow();
    for (int i = 0; i < n; ++i) {
        const std::uintptr_t rec = recs + 0x40ull * static_cast<unsigned>(i);
        std::uintptr_t stim = 0, ag = 0;
        float d = 0;
        if (!SQ(rec, &stim) || !stim) {
            who[i] = R::kLocal;
            continue;
        }
        if (i == 0) stim0 = stim;
        // (round 27 run 2) the record's stimulus read fault-free (ReadProcessMemory, no exception): a stale one is a soft count
        if (!mem::Rd(stim + kSAgent, ag)) {
            g_wc.psr_rec_bad.fetch_add(1, std::memory_order_relaxed);
            who[i] = R::kLocal;
            continue;
        }
        SF32(rec + 0x20, &d);
        who[i] = pa && ag == pa ? R::kPartner : R::kLocal;
        if (who[i] == R::kPartner && !x.in_p) x.in_p = true, x.dp = d;
        // (review L8) every other type-1 record is ours: only the local player and his vehicle classify as PlayerVisual
        if (who[i] == R::kLocal && !x.in_l) x.in_l = true, x.dl = d;
    }
    if (x.in_p) {  // (review L5) the longest gap of her presence in the bucket
        const std::uint64_t last = s->last_p.load(std::memory_order_relaxed);
        if (last && x.now > last) {
            const std::uint64_t gap = x.now - last;
            const std::uint32_t g32 = gap > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(gap);
            if (g32 > s->max_gap.load(std::memory_order_relaxed)) s->max_gap.store(g32, std::memory_order_relaxed);
        }
        s->last_p.store(x.now, std::memory_order_relaxed);
        if (!s->first_p.load(std::memory_order_relaxed)) s->first_p.store(x.now, std::memory_order_relaxed);
    }
    x.partner_ok = pa != 0;
    const unsigned pick = g_pick_live.load(std::memory_order_relaxed);
    {  // (M3f) pick bit 16: SightTick's verdict, clear and at most est_ms old
        const std::uint64_t lm = s->los_ms.load(std::memory_order_acquire);
        x.est_p = lm && x.now >= lm && x.now - lm <= R::PickCfg{}.est_ms && s->los_v.load(std::memory_order_relaxed) == static_cast<std::uint8_t>(R::Sight::clear);
    }
    x.hit_p = s->hit_p.load(std::memory_order_relaxed);
    x.hit_l = s->hit_l.load(std::memory_order_relaxed);
    R::PickState st;
    st.cur = static_cast<R::Who>(s->cur.load(std::memory_order_relaxed));
    st.rule = static_cast<R::Rule>(s->rule.load(std::memory_order_relaxed));
    st.since = s->since.load(std::memory_order_relaxed);
    st.seen_p = s->seen_p.load(std::memory_order_relaxed);
    st.seen_l = s->seen_l.load(std::memory_order_relaxed);
    st.last_dp = s->dp.load(std::memory_order_relaxed);
    st.last_dl = s->dl.load(std::memory_order_relaxed);
    R::PickCfg c;
    c.fresh_ms = g_fresh_ms.load(std::memory_order_relaxed);
    c.est_on = (pick & kPickSight) != 0;
    c.synth_on = c.est_on && (pick & kPickSynth) != 0;
    R::PickOut o = R::TargetPick(st, x, c);
    if (o.st.rule == R::rSeenEst) s->n_est_keep.fetch_add(1, std::memory_order_relaxed), g_wc.est_keep.fetch_add(1, std::memory_order_relaxed);
    const std::uint8_t f = s->force.load(std::memory_order_relaxed);
    if (f == R::kPartner || f == R::kLocal) {
        const R::Who fw = static_cast<R::Who>(f);
        o.act = R::ForcedAct(fw, x.in_p, x.in_l);
        if (o.st.cur != fw) o.st.since = x.now;
        o.st.cur = fw, o.st.rule = R::rForced;
    }
    s->seen_p.store(o.st.seen_p, std::memory_order_relaxed);
    s->seen_l.store(o.st.seen_l, std::memory_order_relaxed);
    s->dp.store(o.st.last_dp, std::memory_order_relaxed);
    s->dl.store(o.st.last_dl, std::memory_order_relaxed);
    s->since.store(o.st.since, std::memory_order_relaxed);
    s->rule.store(o.st.rule, std::memory_order_relaxed);
    if (st.cur == R::kPartner && o.st.cur == R::kLocal) s->flips.fetch_add(1, std::memory_order_relaxed);  // (review L4) every flip, logged or not
    // (run 3) the rule that made the change, before the new `cur` (released): the change log reads it with `cur` (acquired)
    if (o.st.cur != st.cur) s->chg_rule.store(o.st.rule, std::memory_order_relaxed);
    s->cur.store(o.st.cur, std::memory_order_release);
    s->n_calls.fetch_add(1, std::memory_order_relaxed);
    const R::Plan p = R::PlanBucket(who, n, o.act);
    if (p.swap > 0) {
        if (SSwap64(recs, recs + 0x40ull * static_cast<unsigned>(p.swap))) s->n_front.fetch_add(1, std::memory_order_relaxed);
        else g_wc.faults.fetch_add(1);
    }
    // (M3f review) aSynthPartner: the synth first; refused, the bucket suppressed (count 0) as bit 16 alone would
    const bool zero_it = p.count0 || (p.synth && !SynthOne(s, recs, stim0, x.now) && p.fallback0);
    if (zero_it) {
        const std::int32_t zero = 0;
        if (SPut(bucket + 8, &zero, 4)) s->n_supp.fetch_add(1, std::memory_order_relaxed);
        else g_wc.faults.fetch_add(1);
    }
}

// (round 27 run 2: no goal cache) a goal-backed redirect's goal, looked up fresh in this call with the game's own lookup (kGoalFind
// 0x1425BD930: a lock-free, allocation-free hash read; the hooked Updates make the same lookup on this worker: 0x142F0E562,
// 0x142F0E796, 0x142F0F577) and accepted only when its vtable (read fault-free) is its type's (byte-checked); else 0
std::uintptr_t FreshGoal(std::uintptr_t ind, std::uintptr_t rtti, const std::atomic<std::uintptr_t>& want_vt) noexcept {
    const std::uintptr_t g = SehGoal(ind, A(rtti));
    std::uintptr_t vt = 0;
    const bool read = g && mem::Rd(g, vt);
    switch (R::CheckGoal(g, read, vt, want_vt.load(std::memory_order_relaxed))) {
        case R::GoalCheck::ok: return g;
        case R::GoalCheck::bad: g_wc.turn_goal_bad.fetch_add(1, std::memory_order_relaxed); return 0;
        default: return 0;
    }
}

// 2a: the turn task's resolved place and its aim goal, after the original (both Start and Update)
bool TurnFix(std::uintptr_t task) noexcept {
    std::uintptr_t ind = 0;
    std::uint32_t mode = 0, id = 0;
    if (!SQ(task + 0x10, &ind) || !SU32(task + 0x60, &mode) || !SU32(task + 0x40, &id) || mode != 2 || (id & kIdMask) != 0) return false;
    double c[3];
    if (!WantHer(ind, kChest, c)) return false;
    if (!SPut(task + 0x20, c, sizeof(c))) return void(g_wc.faults.fetch_add(1)), false;
    // (review L10) the Update adds the task's +0x50 offset (rotated) to the place when it is not zero: counted, not changed
    float off[3] = {};
    if (SGet(task + 0x50, off, sizeof(off)) && (off[0] != 0.f || off[1] != 0.f || off[2] != 0.f)) g_wc.r2a_off.fetch_add(1, std::memory_order_relaxed);
    if (const std::uintptr_t g = FreshGoal(ind, kAimRtti, g_aim_vt)) {
        const std::uint8_t zero = 0;
        if (!SPut(g + 0x28, c, sizeof(c)) || !SPut(g + 0x5C, &zero, 1)) g_wc.faults.fetch_add(1);
    }
    return true;
}
// 16 (review A1: DSAIHTNPrimitiveTaskVehicleMove, a drive destination): the vehicle-move goal's place, her FEET, after the
// original (both Start and Update)
bool VehFix(std::uintptr_t task) noexcept {
    std::uintptr_t ind = 0;
    if (!SQ(task + 0x10, &ind)) return false;
    double feet[3];
    if (!WantHer(ind, kFeet, feet)) return false;
    const std::uintptr_t g = FreshGoal(ind, kVehRtti, g_veh_vt);
    std::uint8_t flags = 0;
    std::uint32_t id = 0;
    if (!g || !SU8(g + 0x68, &flags) || !SU32(g + 0x58, &id) || !(flags & 2) || (id & kIdMask) != 0) return false;
    if (!SPut(g + 0x38, feet, sizeof(feet))) return void(g_wc.faults.fetch_add(1)), false;
    return true;
}

std::uintptr_t H_Shoot(std::uintptr_t daemon, std::uintptr_t id, std::uintptr_t out, std::uintptr_t r9) {
    if (Live(kS1) && (static_cast<std::uint32_t>(id) & kIdMask) == 0) {
        std::uintptr_t ind = 0;
        double c[3];
        if (SQ(daemon + 0x20, &ind) && WantHer(ind, kChest, c)) {
            if (SPut(out, c, sizeof(c))) {
                g_wc.r1.fetch_add(1, std::memory_order_relaxed);
                return 1;
            }
            g_wc.faults.fetch_add(1);
        }
    }
    return o_shoot(daemon, id, out, r9);
}
std::uintptr_t H_TurnStart(std::uintptr_t task, std::uintptr_t args, std::uintptr_t r8, std::uintptr_t r9) {
    const std::uintptr_t r = o_turn_s(task, args, r8, r9);
    if (Live(kS2a) && TurnFix(task)) {
        g_wc.r2a.fetch_add(1, std::memory_order_relaxed);
        if ((r & 0xFFFFFFFFu) == 1) {  // it was done (facing our Sam): keep it running until the aim goal reports done
            g_wc.r2a_hold.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
    }
    return r;
}
std::uintptr_t H_TurnUpdate(std::uintptr_t task, std::uintptr_t a, std::uintptr_t r8, std::uintptr_t r9) {
    const std::uintptr_t r = o_turn_u(task, a, r8, r9);
    if (Live(kS2a) && TurnFix(task)) g_wc.r2a.fetch_add(1, std::memory_order_relaxed);
    return r;
}
std::uintptr_t H_Range(std::uintptr_t task, std::uintptr_t args, std::uintptr_t r8, std::uintptr_t r9) {
    if (Live(kS2b)) {
        std::uintptr_t ind = 0, node = 0, sym = 0, list = 0, rp = 0, idq = 0;
        double range = 0;
        if (SQ(task + 0x10, &ind) && SQ(args + 8, &node) && SQ(node + 8, &sym) && SQ(node + 0x10, &list) && SQ(list + 8, &idq) &&
            (static_cast<std::uint32_t>(idq) & kIdMask) == 0 && SQ(list + 0x10, &rp) && SF64(rp + 8, &range) && std::isfinite(range)) {
            const std::uintptr_t si = g_sym_in.load(std::memory_order_relaxed), so = g_sym_out.load(std::memory_order_relaxed);
            const int which = sym && sym == si ? 0 : sym && sym == so ? 1 : -1;
            double feet[3], own[3];
            Slot* s = nullptr;
            if (which >= 0 && WantHer(ind, kFeet, feet, &s) && ReadOwn(s, own, TNow())) {
                g_wc.r2b.fetch_add(1, std::memory_order_relaxed);
                return static_cast<std::uintptr_t>(R::RangeCheck(which, feet, own, static_cast<float>(range)));
            }
        }
    }
    return o_range(task, args, r8, r9);
}
std::uintptr_t H_VehStart(std::uintptr_t task, std::uintptr_t args, std::uintptr_t r8, std::uintptr_t r9) {
    const std::uintptr_t r = o_veh_s(task, args, r8, r9);
    if (Live(kSVeh) && VehFix(task)) g_wc.rveh.fetch_add(1, std::memory_order_relaxed);
    return r;
}
std::uintptr_t H_VehUpdate(std::uintptr_t task, std::uintptr_t a, std::uintptr_t r8, std::uintptr_t r9) {
    const std::uintptr_t r = o_veh_u(task, a, r8, r9);
    if (Live(kSVeh) && VehFix(task)) g_wc.rveh.fetch_add(1, std::memory_order_relaxed);
    return r;
}
// 4: the move Update with the game's own place override (B+0x4B8 bit 0x40 -> B+0x2F8) set to her feet for the call. The flag
// byte is changed with locked OR / AND (review S7): only the 0x40 bit is ours, and only when it was clear before our OR.
bool SameBytes(std::uintptr_t a, const void* want, std::size_t n) noexcept {
    std::uint8_t now[24] = {};
    return n <= sizeof(now) && SGet(a, now, n) && std::memcmp(now, want, n) == 0;
}
std::uintptr_t H_Move(std::uintptr_t task, std::uintptr_t a, std::uintptr_t r8, std::uintptr_t r9) {
    std::uintptr_t blk = 0;
    std::uint8_t saved[24] = {};
    double feet[3];
    bool did = false;
    if (Live(kS4)) {
        std::uintptr_t ind = 0, comp = 0;
        std::uint32_t id = 0, idx = 0;
        std::uint8_t alt = 0, mf = 0, ov = 0;
        if (SQ(task + 0x10, &ind) && SQ(ind + kIndComp, &comp) && comp && SU8(comp + 0x4D24, &alt) && (alt || SU32(comp + 0x4D20, &idx))) {
            if (alt) idx = 2;
            blk = idx <= 7 ? comp + 0x2A0 + 0x240ull * idx : 0;
            if (blk && SU32(task + 0x28, &id) && (id & kIdMask) == 0 && WantHer(ind, kFeet, feet)) {
                const std::uintptr_t g = FreshGoal(ind, kMoveRtti, g_move_vt);
                if (g && SU8(g + 0x68, &mf) && (mf & 2) && SU8(blk + 0x4B8, &ov) && !(ov & 0x40) && SGet(blk + 0x2F8, saved, sizeof(saved))) {
                    std::uint8_t old = 0;
                    if (!SPut(blk + 0x2F8, feet, sizeof(feet))) {
                        g_wc.faults.fetch_add(1);
                    } else if (!SOr8(blk + 0x4B8, 0x40, &old)) {
                        SPut(blk + 0x2F8, saved, sizeof(saved)), g_wc.faults.fetch_add(1);
                    } else if (old & 0x40) {  // set by the game between our read and our OR: not ours, left alone
                        if (SameBytes(blk + 0x2F8, feet, sizeof(feet))) SPut(blk + 0x2F8, saved, sizeof(saved));
                        g_wc.r4_not_ours.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        did = true;
                    }
                }
            }
        }
    }
    const std::uintptr_t r = o_move(task, a, r8, r9);
    if (did) {
        g_wc.r4.fetch_add(1, std::memory_order_relaxed);
        std::uint8_t old = 0;
        SAnd8(blk + 0x4B8, static_cast<std::uint8_t>(~0x40u), &old);  // only our bit (the game ORs 0x80 in during the call)
        if (SameBytes(blk + 0x2F8, feet, sizeof(feet))) SPut(blk + 0x2F8, saved, sizeof(saved));
        else g_wc.r4_restore_skip.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}

// 32, the shoot actor: the class of a weapon carrier the shoot job called (its vtable read first: the MULE's or the plain NPC's,
// R::CarrierVtOk; then its entity at -8, a registered MULE's slot)
enum class CarrierKind : std::uint8_t { ok, vt_other, noslot };
CarrierKind ClassifyCarrier(std::uintptr_t carrier, Slot** out, std::uintptr_t* vt_out = nullptr, std::uintptr_t* ent_out = nullptr) noexcept {
    *out = nullptr;
    std::uintptr_t vt = 0, ent = 0;
    SQ(carrier, &vt);
    if (vt_out) *vt_out = vt;
    if (!R::CarrierVtOk(vt, A(kMuleCarVt), A(kCarrierVt))) return CarrierKind::vt_other;
    if (!SQ(carrier - kCarEntity, &ent)) return CarrierKind::noslot;
    if (ent_out) *ent_out = ent;
    *out = FindEnt(ent);
    return *out ? CarrierKind::ok : CarrierKind::noslot;
}
// the carrier's SetTargetActor (this, actor id). For the shoot job's call with actor id 0 on a registered MULE's carrier: counted;
// with bit 32 live, the MULE's pick her and her record fresh, the carrier gets her chest by its own SetTargetPos instead (mode 1,
// a place). When she stops being the pick (or her record goes stale) the next frame's SetTargetActor(0) is the original again,
// and the mode differs, so it does not early-out: the carrier is back on its actor. (Run 3) The MULE's vt+0xC0 wrapper reaches
// here by a tail jump (the job's return address kept) only when its component's type is not 0x36; its own SetTargetPos wrapper
// does exactly the base call for such a type, so the redirect calls the base.
std::uintptr_t H_SetActor(std::uintptr_t carrier, std::uintptr_t id, std::uintptr_t r8, std::uintptr_t r9) {
    const bool from_job = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == A(kJobSetRet);
    if (from_job && g_rec_live.load(std::memory_order_relaxed) && !g_off.load(std::memory_order_relaxed) && (static_cast<std::uint32_t>(id) & kIdMask) == 0) {
        Slot* s = nullptr;
        std::uintptr_t vt = 0, ent = 0;
        const CarrierKind kind = ClassifyCarrier(carrier, &s, &vt, &ent);
        if (kind == CarrierKind::vt_other) {
            g_wc.sh_vt_other.fetch_add(1, std::memory_order_relaxed);
            g_first_vt_other.Put(carrier, vt, 0, 0, 0);
        } else if (kind == CarrierKind::noslot) {
            g_wc.sh_noslot.fetch_add(1, std::memory_order_relaxed);
            g_first_noslot.Put(carrier, ent, vt, 0, 0);
        }
        const bool live = Live(kS32);
        double c[3];
        const R::ShootAct act = R::ShootPlan(true, true, static_cast<std::uint32_t>(id), kind == CarrierKind::ok, live,
                                             s && live && WantHer(s->ind.load(std::memory_order_relaxed), kChest, c));
        if (act != R::ShootAct::pass) {
            s->sh_calls.fetch_add(1, std::memory_order_relaxed);
            g_wc.sh_calls.fetch_add(1, std::memory_order_relaxed);
        }
        if (act == R::ShootAct::redirect) {
            // the game's own place target on the same carrier, the call the job itself makes for a place goal (0x142EFFA5C)
            reinterpret_cast<void (*)(std::uintptr_t, const double*)>(A(kCarSetPos))(carrier, c);
            s->sh_redir.fetch_add(1, std::memory_order_relaxed);
            g_wc.sh_redir.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
    }
    return o_set_actor(carrier, id, r8, r9);
}
// the carrier's GetTargetPosition (this, out double3): after the original, the shoot job's call on a registered MULE's carrier
// records the point (read-only; with bit 32 off too) and the carrier's mode
void RecordPoint(std::uintptr_t carrier, std::uintptr_t out) noexcept {
    if (!g_rec_live.load(std::memory_order_relaxed) || g_off.load(std::memory_order_relaxed)) return;
    double p[3];
    std::uint32_t mode = 0;
    Slot* s = nullptr;
    if (ClassifyCarrier(carrier, &s) != CarrierKind::ok || !s || !SGet(out, p, sizeof(p)) || !std::isfinite(p[0]) || !std::isfinite(p[1]) ||
        !std::isfinite(p[2]) || !SU32(carrier + kCarMode, &mode))
        return;
    const std::uint32_t q = s->pt_seq.fetch_add(1, std::memory_order_acq_rel);  // odd: being written
    for (int i = 0; i < 3; ++i) s->pt[i].store(p[i], std::memory_order_relaxed);
    s->pt_mode.store(static_cast<std::int32_t>(mode), std::memory_order_relaxed);
    s->pt_ms.store(TNow(), std::memory_order_relaxed);
    s->pt_seq.store(q + 2, std::memory_order_release);
    s->sh_pts.fetch_add(1, std::memory_order_relaxed);
    (mode == 0 ? s->pt_actor : mode == 1 ? s->pt_place : s->pt_other).fetch_add(1, std::memory_order_relaxed);
    g_wc.sh_pts.fetch_add(1, std::memory_order_relaxed);
}
// (run 3 review) set by H_GetPos when the MULE wrapper's base call reached it (return address kMuleGetRet); H_MuleGetPos clears it
// before the wrapper runs and reads it after: still clear means the wrapper took its type-0x36 branch (no base call)
thread_local bool t_mule_base = false;
// the base GetTargetPosition (the plain NPC carrier's vt+0xD0): the job's own call only (inside the MULE wrapper the return
// address is kMuleGetRet: not recorded twice)
std::uintptr_t H_GetPos(std::uintptr_t carrier, std::uintptr_t out, std::uintptr_t r8, std::uintptr_t r9) {
    const std::uintptr_t ra = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (ra == A(kMuleGetRet)) t_mule_base = true;
    const std::uintptr_t r = o_get_pos(carrier, out, r8, r9);
    if (ra == A(kJobGetRet)) RecordPoint(carrier, out);
    return r;
}
// (run 3) the MULE carrier's vt+0xD0 wrapper (0x142AFB790), entered before its type test: the job's call recorded after it.
// (Run 3 review) the job's call that never reached the base GetTargetPosition is the type-0x36 branch: counted (t36)
std::uintptr_t H_MuleGetPos(std::uintptr_t carrier, std::uintptr_t out, std::uintptr_t r8, std::uintptr_t r9) {
    const std::uintptr_t ra = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    t_mule_base = false;
    const std::uintptr_t r = o_mule_get_pos(carrier, out, r8, r9);
    if (ra == A(kJobGetRet)) {
        if (!t_mule_base && g_rec_live.load(std::memory_order_relaxed) && !g_off.load(std::memory_order_relaxed)) {
            g_wc.sh_t36.fetch_add(1, std::memory_order_relaxed);
            Slot* s = nullptr;
            if (ClassifyCarrier(carrier, &s) == CarrierKind::ok && s) s->sh_t36.fetch_add(1, std::memory_order_relaxed);
        }
        RecordPoint(carrier, out);
    }
    t_mule_base = false;
    return r;
}
// the job's last aim point of a slot (any thread): false when none or torn
bool ReadShootPt(Slot& s, double (&p)[3], std::uint64_t* ms, std::int32_t* mode) noexcept {
    for (int t = 0; t < 4; ++t) {
        const std::uint32_t s1 = s.pt_seq.load(std::memory_order_acquire);
        if (s1 & 1) continue;
        *ms = s.pt_ms.load(std::memory_order_relaxed);
        *mode = s.pt_mode.load(std::memory_order_relaxed);
        for (int i = 0; i < 3; ++i) p[i] = s.pt[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.pt_seq.load(std::memory_order_relaxed) != s1) continue;
        return *ms != 0;
    }
    return false;
}

// ---------------------------------------------------------------- game thread state
struct Cfg {
    int stim = 0;
    bool vis = true;
    double radius = 300.0;
    unsigned pick = 0, redirect = 0;
    int fresh_ms = 1500;
} g_cfg;
bool g_installed = false;
double g_now = 0;
std::uint64_t g_tick = 0;
std::uint8_t g_role = 0;
std::uintptr_t g_local = 0, g_body = 0;
bool g_local_ok = false, g_net = false;

bool g_checked = false;
unsigned g_groups = 0;      // byte-check groups that passed
unsigned g_sites_in = 0;    // redirect sites whose detours are in
unsigned g_sites_tried = 0;
bool g_mh_tried = false, g_mh_ok = false;
unsigned g_sense_in = 0;
bool g_sense_tried = false;
int g_redir_override = -1;  // op 6 (hidden runs): the redirect bits for this run
bool g_stopped = false, g_wiped = false;
double g_stop_at = 0;
const char* g_stop_why = nullptr;
std::uintptr_t g_cs_held = 0;
unsigned g_cs_depth = 0;  // (review S8) the tick's fault handler leaves the lock this many times
// (review S2) the table is valid for one AI manager / MULE manager and outside loads: cleared (and every live bit off) on a change,
// the bits back only after a registration pass on the new one
std::uintptr_t g_seen_ai = 0, g_seen_mule = 0;
bool g_table_ok = false, g_table_was_loading = false;
unsigned g_table_clears = 0;
// (review S3) our own down code (the game's stop gates, latched as esync latches it) for SPV1, whatever enemy_sync is doing
erules::DownLatch g_down_latch;
bool g_our_down = false;
// our own aim height above our root (SPV1; review A3) and the armed MULE (op 4) for op 8's line
std::uint16_t g_aim_cm = 0;
std::uint32_t g_arm_id = 0;
const char* g_arm_label = nullptr;
int g_arm_code = 0;  // its code (1..9; run 2: the geometry rule of op 7 during it)

// the partner stimulus (M2)
struct PStim {
    std::uintptr_t stim = 0, res = 0, agent = 0, body = 0, mgr = 0;
    bool pending = false, wait_logged = false;
    std::uint64_t pending_tick = 0;
    double t0 = 0, last_at = 0;
    double last_pos[3] = {};
    bool have_last = false;
    std::uint32_t id88 = 0;
} g_ps;
bool g_stim_dead = false;
bool g_forced_off = false;     // op 1 0
bool g_vis_forced = false;     // op 2
float g_vis_force = 1.f;
unsigned g_made = 0, g_removed = 0, g_forgotten = 0, g_faults = 0, g_releases = 0;
double g_bad_since = -1;       // a soft precondition failing since (2 s rule)
double g_retry_at = 0;         // after a failed create: 5 s
const char* g_last_not = nullptr;
double g_not_logged_at = -1e18;
double g_status_next = 0, g_tgt_next = 0, g_reg_next = 0, g_own_next = 0, g_place_next = 0, g_log_next = 0;
double g_near_next = 0, g_near_m = -1;
float g_last_vis = 1.f;

// SPV1
ewire::Visibility g_spv;
bool g_spv_have = false;
double g_spv_at = -1e18;
std::uint32_t g_spv_gen = 0;
std::uint16_t g_spv_seq = 0;
unsigned g_spv_sent = 0, g_spv_recv = 0, g_spv_stale = 0, g_spv_bad = 0;
double g_spv_next = 0;

// our Sam's own stimulus (by his agent; re-found every 2 s when it no longer matches)
struct SamStim {
    std::uintptr_t stim = 0, agent = 0;
    double at = -1e18;
} g_sam;

// the actors (the registry walk, 1 s; fixed arrays: no allocation in the tick, review S11)
constexpr unsigned kActorCap = 2048;
struct ActorRec {
    std::uint32_t id;
    std::uintptr_t o;
};
ActorRec g_actors[kActorCap];
unsigned g_nactors = 0;
double g_actors_at = -1e18;
// the MULE members read by the registration pass
constexpr unsigned kMemberCap = 4096;
std::uintptr_t g_members[kMemberCap];
std::uint32_t g_member_ids[kMemberCap];

// the summary counters (deltas of the worker counts)
struct Snap {
    unsigned front = 0, supp = 0, r1 = 0, r2a = 0, r2a_off = 0, r2b = 0, rveh = 0, r4 = 0, r4n = 0, h5 = 0, h2r = 0, h2f = 0, h2nb = 0, faults = 0, forced = 0;
    unsigned sh_calls = 0, sh_redir = 0, sh_pts = 0, pt_her = 0, pt_sam = 0, pt_none = 0;
} g_last_snap;
unsigned g_mismatch = 0, g_forced_vis = 0, g_ind_mismatch = 0, g_registered = 0;
std::uint32_t g_wfaults_seen = 0;
unsigned g_reg_pass = 0;
// (M3f) pick bits 16 / 32: the raycast's self-test (0 not yet, 1 ok, 2 failed, 3 a ray fault), the rays cast, the bits last logged
int g_los_state = 0;
unsigned g_sight_rays = 0, g_sight_rr = 0;
double g_sight_next = 0;
unsigned g_bits_logged = 0;
bool g_bits_logged_any = false;

void StimDead(const char* what) noexcept {
    if (!g_stim_dead) Log("[enemy] pstim %s FAULTED: no further creates or removes this run", what);
    g_stim_dead = true;
    ++g_faults;
}
void Publish() noexcept {
    g_ps_res.store(g_ps.res, std::memory_order_relaxed);
    g_ps_mgr.store(g_ps.mgr, std::memory_order_relaxed);
    g_ps_agent.store(g_ps.agent, std::memory_order_relaxed);
    g_ps_stim.store(g_ps.stim, std::memory_order_release);
}
void Unpublish() noexcept {
    g_p_agent.store(0, std::memory_order_release);
    g_ps_stim.store(0, std::memory_order_release);
    g_ps_agent.store(0, std::memory_order_relaxed);
    g_ps_res.store(0, std::memory_order_relaxed);
    g_ps_mgr.store(0, std::memory_order_relaxed);
}
void Forget(const char* why) noexcept {
    Log("[enemy] pstim forgotten (%s; not removed)", why);
    ++g_forgotten;
    Unpublish();
    g_ps = PStim{};
}
// the pool lock, remembered so the tick's fault handler can leave it
struct Lock {
    std::uintptr_t mgr;
    bool held;
    explicit Lock(std::uintptr_t m) noexcept : mgr(m), held(m && SehEnterCs(m) > 0) {
        if (held) g_cs_held = m, ++g_cs_depth;
    }
    ~Lock() {
        if (!held) return;
        if (g_cs_depth && --g_cs_depth == 0) g_cs_held = 0;
        SehLeaveCs(mgr);
    }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};
bool OursLiveP(std::uintptr_t s) noexcept { return stimcore::OursLive(s, g_ps.agent, g_ps.res); }
bool OursParkedP(std::uintptr_t s) noexcept { return stimcore::OursParked(s, g_ps.res); }

// ---- the release (C1, as the M1 probe proved it): step 1 at once, step 2 two ticks later
void Release(const char* why) noexcept {
    if (!g_ps.stim || g_ps.pending) return;
    ++g_releases;
    Unpublish();
    const std::uintptr_t mgr = mem::Q(A(kStimMgr));
    unsigned faults = 0;
    if (mgr && mgr != g_ps.mgr) return Forget("the AI manager changed");
    if (!mgr) {
        if (!OursLiveP(g_ps.stim)) return Forget("no AI manager and no longer ours");
        faults = Step1(g_ps.stim, g_ps.agent, g_ps.res, 0);
    } else {
        Lock lk(mgr);
        if (!lk.held) {
            StimDead("the pool lock");
            return Forget("the pool lock faulted");
        }
        if (!InLiveList(mgr, g_ps.stim) || !OursLiveP(g_ps.stim)) return Forget("no longer ours at the release");
        faults = Step1(g_ps.stim, g_ps.agent, g_ps.res, mgr);
    }
    if (faults) ++g_faults;
    g_ps.pending = true;
    g_ps.pending_tick = g_tick;
    Log("[enemy] pstim release #%u (%s): +0xA4 0, +0x88 no actor, +0x84 1, agent cleared%s%s; removed on a later tick", g_releases, why,
        mgr ? "" : " (no AI manager: the copies untouched)", faults ? " FAULT" : "");
}
void Step2(bool now) noexcept {
    if (!g_ps.pending || (!now && g_tick < g_ps.pending_tick + 2)) return;
    if (g_stim_dead) return Forget("an earlier create or remove faulted");
    const std::uintptr_t mgr = mem::Q(A(kStimMgr));
    if (!mgr) {
        if (!g_ps.wait_logged) Log("[enemy] pstim release: no AI manager; the remove waits");
        g_ps.wait_logged = true;
        return;
    }
    if (mgr != g_ps.mgr) return Forget("the AI manager changed");
    const std::uintptr_t s = g_ps.stim;
    Lock lk(mgr);
    if (!lk.held) {
        StimDead("the pool lock");
        return Forget("the pool lock faulted");
    }
    if (!InLiveList(mgr, s) || !OursParkedP(s)) return Forget("not in the live list as ours any more");
    const std::uint8_t zero = 0, one = 1;
    int r = SehWrite(s + kSOff, &zero, 1);
    if (r > 0) r = SehStimRemove(mgr, s);
    if (r < 0) StimDead("the remove");
    const bool still = r >= 0 && InLiveList(mgr, s);
    if (still) SehWrite(s + kSOff, &one, 1);
    ++g_removed;
    Log("[enemy] pstim removed (%.1f s%s)%s", (g_now - g_ps.t0) / 1000.0, still ? "; still in the live list: the remove did nothing, parked again" : "",
        r < 0 ? " FAULT" : "");
    g_ps = PStim{};
}
// a direct remove while the partner body is alive (op 1 0, op 9); still listed afterwards: released instead
void RemoveNow(const char* why, bool quiet = false) noexcept {
    if (!g_ps.stim) return;
    if (g_ps.pending) return;  // (review S10) a release in progress: the tick's step 2 finishes it, two ticks after step 1
    const std::uintptr_t mgr = mem::Q(A(kStimMgr));
    if (!mgr || g_stim_dead) return Release(why);
    if (mgr != g_ps.mgr) return Forget("the AI manager changed");
    {
        Lock lk(mgr);
        if (!lk.held) {
            StimDead("the pool lock");
            return Forget("the pool lock faulted");
        }
        if (!InLiveList(mgr, g_ps.stim) || !OursLiveP(g_ps.stim)) return Forget("no longer ours at the remove");
        Unpublish();
        const int r = SehStimRemove(mgr, g_ps.stim);
        if (r < 0) StimDead("the remove");
        if (r >= 0 && !InLiveList(mgr, g_ps.stim)) {
            ++g_removed;
            if (!quiet) Log("[enemy] pstim removed (%s; %.1f s)", why, (g_now - g_ps.t0) / 1000.0);
            g_ps = PStim{};
            return;
        }
        Log("[enemy] pstim remove (%s): %s; released instead", why, r < 0 ? "FAULT" : "still in the live list");
    }
    Publish();
    Release(why);
}

// our Sam's live visual stimulus
std::uintptr_t SamStimNow() noexcept {
    const std::uintptr_t agent = g_local ? mem::Q(g_local + kEAgent) : 0;
    if (!agent) return 0;
    if (g_sam.stim && g_sam.agent == agent && mem::Q(g_sam.stim + kSAgent) == agent) return g_sam.stim;
    if (g_now - g_sam.at < 2000 && g_sam.agent == agent) return 0;
    g_sam = SamStim{};
    g_sam.agent = agent, g_sam.at = g_now;
    const std::uintptr_t mgr = mem::Q(A(kStimMgr));
    unsigned n = 0;
    for (std::uintptr_t s = mgr ? mem::Q(mgr + kMgrLive) : 0; s && n < kListCap; s = mem::Q(s + kSNext), ++n)
        if (mem::Q(s + kSAgent) == agent) {
            g_sam.stim = s;
            break;
        }
    return g_sam.stim;
}

// ---- the actors (both games: the registry's human enemies; the guest's are its mirrors)
void WalkActors() noexcept {
    g_nactors = 0;
    g_actors_at = g_now;
    const std::uintptr_t reg = mem::Q(A(kRegGlobal));
    std::int32_t ngroups = 0;
    if (!reg || !mem::Rd(reg + 0x440, ngroups) || ngroups <= 0 || ngroups > 512) return;
    const std::uintptr_t garr = mem::Q(reg + 0x448);
    for (std::int32_t gi = 0; gi < ngroups; ++gi) {
        const std::uintptr_t g = mem::Q(garr + 8ull * static_cast<unsigned>(gi));
        std::int32_t count = 0;
        if (!g || !mem::Rd(g + 0x40, count) || count <= 0 || count > 8192) continue;
        const std::uintptr_t arr = mem::Q(g + 0x48);
        for (std::int32_t s = 0; arr && s < count; ++s) {
            const std::uintptr_t o = mem::Q(arr + 8ull * static_cast<unsigned>(s));
            std::uint8_t in_use = 0;
            std::uint32_t id = 0;
            if (!o || !mem::Rd(o + kActInUse, in_use) || !in_use || !mem::Rd(o + kActId, id) || !enemy::IsHumanEnemyId(id)) continue;
            if (g_nactors >= kActorCap) return;
            g_actors[g_nactors++] = ActorRec{id, o};
        }
    }
}
// a realized actor's entity (0 when not realized)
std::uintptr_t EntityOf(std::uintptr_t actor) noexcept {
    if (!actor || U32(actor + kActState) != 2) return 0;
    const std::uintptr_t ref = mem::Q(actor + kActEntRef);
    return ref ? ref - 0x20 : 0;
}
std::uintptr_t ActorOf(std::uint32_t id) noexcept {
    for (int pass = 0; pass < 2; ++pass) {
        for (unsigned i = 0; i < g_nactors; ++i) {
            const ActorRec& a = g_actors[i];
            if (a.id == id) {
                std::uint8_t in_use = 0;
                if (mem::Q(a.o) == A(kMuleVt) && mem::Rd(a.o + kActInUse, in_use) && in_use && U32(a.o + kActId) == id) return a.o;
            }
        }
        if (pass == 0 && g_now - g_actors_at > 500) WalkActors();
        else break;
    }
    return 0;
}
// the nearest realized human enemy to `p` (m; -1 none)
double NearestEnemy(const double (&p)[3]) noexcept {
    if (g_now - g_actors_at > 1000) WalkActors();
    double best = -1;
    for (unsigned i = 0; i < g_nactors; ++i) {
        double q[3];
        const std::uintptr_t e = EntityOf(g_actors[i].o);
        if (!e || !Pos(e, q)) continue;
        const double d = L::Dist(p, q);
        if (best < 0 || d < best) best = d;
    }
    return best;
}

// (review: the classifier's flags and CanShoot's id 0 point are the PLAYER OBJECT's, not the entity's) the player object whose
// +0x70 is `entity` (0 none / byte checks); game thread
std::uintptr_t PlayerOf(std::uintptr_t entity) noexcept {
    if (!entity || !(g_groups & kGPlayer)) return 0;
    const std::uintptr_t mgr = mem::Q(A(kPlayerMgr));
    std::int32_t n = 0;
    if (!mgr || !mem::Rd(mgr + 0x38, n) || n <= 0 || n > 16) return 0;
    const std::uintptr_t arr = mem::Q(mgr + 0x40);
    for (std::int32_t i = 0; arr && i < n; ++i) {
        const std::uintptr_t p = mem::Q(arr + 8ull * static_cast<unsigned>(i));
        if (p && mem::Q(p + kPlEntity) == entity) return p;
    }
    return 0;
}
// our own aim height above our root (cm; 0 unknown): the player object's info +0x198 point, 0.20..2.50 m above, within 1 m across
std::uint16_t AimCm(std::uintptr_t player, std::uintptr_t entity) noexcept {
    double ap[3] = {}, root[3] = {};
    if (!player || !Pos(entity, root) || !mem::Read(player + kPlInfo + kPiAimPt, ap, sizeof(ap))) return 0;
    if (!std::isfinite(ap[0]) || !std::isfinite(ap[1]) || !std::isfinite(ap[2]) || L::Dist2d(ap, root) > 1.0) return 0;
    const double dz = ap[2] - root[2];
    return dz >= 0.20 && dz <= 2.50 ? static_cast<std::uint16_t>(dz * 100.0 + 0.5) : 0;
}

// ---- byte checks
void Checks() noexcept {
    if (g_checked) return;
    g_checked = true;
    unsigned bad = 0, n = 0;
    for (const Site& s : kSites) {
        ++n;
        if (!mem::CodeMatches(A(s.va), s.sig)) bad |= s.group, Log("[enemy] tgt byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
    }
    struct Rip {
        std::uintptr_t got, want;
        const char* name;
        unsigned group;
    };
    const Rip rips[] = {
        {mem::RipTarget(A(kTsSite), 3, 7), A(kStimMgr), "the AI manager global", kGStim},
        {mem::RipTarget(A(kRegSite), 3, 7), A(kRegGlobal), "the actor registry global", kGReg},
        {mem::Q(A(kKnowVt) + 0x38), A(kPsrVa), "K's vt+0x38 (ProcessSensorResults)", kGPsr},
        {mem::Q(A(kTurnVt) + 0x30), A(kTurnStart), "the turn task's vt+0x30 (bit 2a)", kGS2a},
        {mem::Q(A(kTurnVt) + 0x48), A(kTurnUpdate), "the turn task's vt+0x48 (bit 2a)", kGS2a},
        {mem::RipTarget(A(kTurnAimSite), 3, 7), A(kAimRtti), "the aim goal's type (bit 2a)", kGS2a},
        {mem::Q(A(kRangeVt) + 0x30), A(kRangeStart), "the range-check task's vt+0x30 (bit 2b)", kGS2b},
        {mem::RipTarget(A(kSymInSite), 3, 7), A(kSymIn), "the range check's InRange symbol (bit 2b)", kGS2b},
        {mem::RipTarget(A(kSymOutSite), 3, 7), A(kSymOut), "the range check's OutOfRange symbol (bit 2b)", kGS2b},
        {mem::Q(A(kVehVt) + 0x30), A(kVehStart), "the vehicle-move task's vt+0x30 (bit 16)", kGVeh},
        {mem::Q(A(kVehVt) + 0x48), A(kVehUpdate), "the vehicle-move task's vt+0x48 (bit 16)", kGVeh},
        {mem::RipTarget(A(kVehGoalSite), 3, 7), A(kVehRtti), "the vehicle-move goal's type (bit 16)", kGVeh},
        {mem::Q(A(kMoveVt) + 0x48), A(kMoveUpdate), "the move task's vt+0x48 (bit 4)", kGS4},
        {mem::RipTarget(A(kMoveRttiSite), 3, 7), A(kMoveRtti), "the move goal's type (bit 4)", kGS4},
        {mem::RipTarget(A(kMuleSite), 3, 7), A(kMuleMgr), "the MULE manager global", kGMule},
        {mem::RipTarget(A(kPlayerMgrSite), 3, 7), A(kPlayerMgr), "the player manager global", kGPlayer},
        {mem::Q(A(kAimGoalVt)), A(kAimGetter), "the aim goal's vtable (bit 2a)", kGS2a},
        {mem::RipTarget(A(kAimGetter), 3, 7), A(kAimRtti), "the aim goal's type (its getter; bit 2a)", kGS2a},
        {mem::Q(A(kVehGoalVt)), A(kVehGetter), "the vehicle-move goal's vtable (bit 16)", kGVeh},
        {mem::RipTarget(A(kVehGetter), 3, 7), A(kVehRtti), "the vehicle-move goal's type (its getter; bit 16)", kGVeh},
        {mem::Q(A(kMoveGoalVt)), A(kMoveGetter), "the move goal's vtable (bit 4)", kGS4},
        {mem::RipTarget(A(kMoveGetter), 3, 7), A(kMoveRtti), "the move goal's type (its getter; bit 4)", kGS4},
        {mem::Q(A(kCarrierVt) + 0xB8), A(kCarSetPos), "the carrier's vt+0xB8 SetTargetPos (bit 32)", kGS32},
        {mem::Q(A(kCarrierVt) + 0xC0), A(kCarSetActor), "the carrier's vt+0xC0 SetTargetActor (bit 32)", kGS32},
        {mem::Q(A(kCarrierVt) + 0xD0), A(kCarGetPos), "the carrier's vt+0xD0 GetTargetPosition (bit 32)", kGS32},
        // (run 3) the MULE's carrier class
        {mem::Q(A(kMuleCarVt) + 0xB8), A(kMuleSetPos), "the MULE carrier's vt+0xB8 SetTargetPos (bit 32)", kGS32},
        {mem::Q(A(kMuleCarVt) + 0xC0), A(kMuleSetActor), "the MULE carrier's vt+0xC0 SetTargetActor (bit 32)", kGS32},
        {mem::Q(A(kMuleCarVt) + 0xD0), A(kMuleGetPos), "the MULE carrier's vt+0xD0 GetTargetPosition (bit 32)", kGS32},
        {mem::CodeMatches(mem::Q(A(kMuleCarVt)), "48 83 E9 50 E9") ? 1u : 0u, 1u, "the MULE carrier's slot 0 (this - 0x50 adjustor; bit 32)", kGS32},
        {mem::RipTarget(A(kMuleSetActorJmp), 1, 5), A(kCarSetActor), "the MULE SetTargetActor's tail jump to the base (bit 32)", kGS32},
        {mem::RipTarget(A(kMuleGetPosCall), 1, 5), A(kCarGetPos), "the MULE GetTargetPosition's base call (bit 32)", kGS32},
        {mem::RipTarget(A(kMuleCtorStore), 3, 7), A(kMuleCarVt), "the MULE weapon component's carrier vtable (bit 32)", kGS32},
    };
    for (const Rip& r : rips) {
        ++n;
        if (r.got != r.want) bad |= r.group, Log("[enemy] tgt byte check FAILED: %s", r.name);
    }
    const unsigned all =
        kGStim | kGReg | kGGoal | kGPsr | kGCls | kGClsFlags | kGForce | kGS1 | kGS2a | kGS2b | kGVeh | kGS4 | kGMule | kGPlayer | kGS32;
    g_groups = all & ~bad;
    // the goal-backed redirects need the goal lookup
    if (!(g_groups & kGGoal)) g_groups &= ~(kGS2a | kGVeh | kGS4);
    g_aim_vt.store((g_groups & kGS2a) ? A(kAimGoalVt) : 0, std::memory_order_release);
    g_veh_vt.store((g_groups & kGVeh) ? A(kVehGoalVt) : 0, std::memory_order_release);
    g_move_vt.store((g_groups & kGS4) ? A(kMoveGoalVt) : 0, std::memory_order_release);
    auto ok = [&](unsigned g) { return (g_groups & g) ? "ok" : "FAILED"; };
    Log("[enemy] tgt byte checks (%u): stimulus %s, registry %s, goals %s, sensor results %s, classifier %s, classifier flags %s, forced visual %s, "
        "MULE members %s, player objects %s, redirect 1 %s, 2a %s, 2b %s, 4 %s, 16 (vehicle chase) %s, 32 (shoot actor) %s; redirect 8 deferred (not "
        "built)",
        n, ok(kGStim), ok(kGReg), ok(kGGoal), ok(kGPsr), ok(kGCls), ok(kGClsFlags), ok(kGForce), ok(kGMule), ok(kGPlayer), ok(kGS1), ok(kGS2a), ok(kGS2b),
        ok(kGS4), ok(kGVeh), ok(kGS32));
}

unsigned RedirWant() noexcept { return g_redir_override >= 0 ? static_cast<unsigned>(g_redir_override) : g_cfg.redirect; }
bool PickOrRedirWanted() noexcept { return g_cfg.pick != 0 || RedirWant() != 0; }
// (run 3 review) bit 32's three hooks go in only when bit 32 is on, or in a hidden run (automation), where the aim point is
// recorded with bit 32 off too (TF0's record); never for players with bit 32 off
unsigned ShootHooksWant() noexcept { return ((RedirWant() & kRedShootActor) || automation::Enabled()) ? kRedShootActor : 0u; }
unsigned SitesFor(unsigned bits) noexcept {
    unsigned s = 0;
    if (bits & kRedShoot) s |= kS1;
    if (bits & kRedTurn) s |= kS2a | kS2b;
    if (bits & kRedApproach) s |= kS4;
    if (bits & kRedVehicle) s |= kSVeh;
    if (bits & kRedShootActor) s |= kS32;
    return s;
}
unsigned SiteOk(unsigned site) noexcept {
    switch (site) {
        case kS1: return g_groups & kGS1;
        case kS2a: return g_groups & kGS2a;
        case kS2b: return g_groups & kGS2b;
        case kSVeh: return g_groups & kGVeh;
        case kS4: return g_groups & kGS4;
        case kS32: return g_groups & kGS32;
        default: return 0;
    }
}
bool Hook(std::uintptr_t va, void* det, void** orig) noexcept {
    MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(A(va)), det, orig);
    if (s == MH_OK) s = MH_EnableHook(reinterpret_cast<void*>(A(va)));
    if (s != MH_OK) Log("[enemy] tgt hook at 0x%llX: %s", static_cast<unsigned long long>(va), MH_StatusToString(s));
    return s == MH_OK;
}
void EnsureRedirects(unsigned bits) noexcept {
    const unsigned want = SitesFor(bits);
    // (run 3) up to three targets per site: bit 32's base SetTargetActor, base GetTargetPosition and the MULE carrier's
    // GetTargetPosition wrapper (in together, or none)
    struct One {
        unsigned site;
        std::uintptr_t va[3];
        void* det[3];
        void** orig[3];
        const char* name;
    } const ones[] = {
        {kS1, {kCanShoot, 0, 0}, {reinterpret_cast<void*>(&H_Shoot), nullptr, nullptr}, {reinterpret_cast<void**>(&o_shoot), nullptr, nullptr}, "1 (CanShoot)"},
        {kS2a,
         {kTurnStart, kTurnUpdate, 0},
         {reinterpret_cast<void*>(&H_TurnStart), reinterpret_cast<void*>(&H_TurnUpdate), nullptr},
         {reinterpret_cast<void**>(&o_turn_s), reinterpret_cast<void**>(&o_turn_u), nullptr},
         "2a (turn)"},
        {kS2b, {kRangeStart, 0, 0}, {reinterpret_cast<void*>(&H_Range), nullptr, nullptr}, {reinterpret_cast<void**>(&o_range), nullptr, nullptr}, "2b (range check)"},
        {kS4, {kMoveUpdate, 0, 0}, {reinterpret_cast<void*>(&H_Move), nullptr, nullptr}, {reinterpret_cast<void**>(&o_move), nullptr, nullptr}, "4 (approach)"},
        {kSVeh,
         {kVehStart, kVehUpdate, 0},
         {reinterpret_cast<void*>(&H_VehStart), reinterpret_cast<void*>(&H_VehUpdate), nullptr},
         {reinterpret_cast<void**>(&o_veh_s), reinterpret_cast<void**>(&o_veh_u), nullptr},
         "16 (vehicle chase)"},
        {kS32,
         {kCarSetActor, kCarGetPos, kMuleGetPos},
         {reinterpret_cast<void*>(&H_SetActor), reinterpret_cast<void*>(&H_GetPos), reinterpret_cast<void*>(&H_MuleGetPos)},
         {reinterpret_cast<void**>(&o_set_actor), reinterpret_cast<void**>(&o_get_pos), reinterpret_cast<void**>(&o_mule_get_pos)},
         "32 (shoot actor)"},
    };
    for (const One& o : ones) {
        if (!(want & o.site) || (g_sites_tried & o.site)) continue;
        g_sites_tried |= o.site;
        if (!SiteOk(o.site)) {
            Log("[enemy] tgt redirect %s: off (byte checks)", o.name);
            continue;
        }
        if (!g_mh_tried) {
            g_mh_tried = true;
            const MH_STATUS init = MH_Initialize();
            g_mh_ok = init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED;
        }
        if (!g_mh_ok) {
            Log("[enemy] tgt redirect %s: off (MinHook)", o.name);
            continue;
        }
        bool in = true;
        int done = 0;
        for (int i = 0; i < 3 && in && o.va[i]; ++i) {
            in = Hook(o.va[i], o.det[i], o.orig[i]);
            if (in) ++done;
        }
        if (!in)
            for (int i = 0; i < done; ++i) MH_DisableHook(reinterpret_cast<void*>(A(o.va[i])));  // all or none
        if (in) g_sites_in |= o.site;
        Log("[enemy] tgt redirect %s: %s%s", o.name, in ? "on" : "off (MinHook refused it)",
            o.site != kS32 ? "" : (RedirWant() & kRedShootActor) ? " (bit 32 on)" : " (bit 32 off: record only, a hidden run)");
    }
}
void EnsureSense(unsigned pick) noexcept {
    unsigned want = 0;
    if (pick & kPickReorder) want |= sense::kPsr;
    if (pick & (kPickH5 | kPickH2)) want |= sense::kCls;
    // (review S5) one try per run: a hook MinHook refused, or one off by its byte checks, is not tried (or logged) again each tick
    if (!want || g_sense_tried) {
        g_sense_in = sense::Installed();
        return;
    }
    g_sense_tried = true;
    const char* text[3] = {"-", "-", "-"};
    const unsigned ok = ((g_groups & kGCls) ? sense::kCls : 0u) | ((g_groups & kGPsr) ? sense::kPsr : 0u);
    g_sense_in = sense::Ensure(want, ok, text);
    Log("[enemy] tgt sense hooks: classifier %s, sensor results %s", (want & sense::kCls) ? text[0] : "not needed", (want & sense::kPsr) ? text[2] : "not needed");
}
// the effective bits the workers see
void Recompute() noexcept {
    if (g_stopped || g_off.load() || !g_table_ok) {
        g_pick_live.store(0), g_redir_live.store(0), g_rec_live.store(false);
        return;
    }
    unsigned p = 0;
    if ((g_cfg.pick & kPickReorder) && (g_groups & kGPsr) && (g_sense_in & sense::kPsr)) p |= kPickReorder;
    if ((g_cfg.pick & kPickH5) && (g_groups & kGCls) && (g_groups & kGMule) && (g_sense_in & sense::kCls)) p |= kPickH5;
    if ((g_cfg.pick & kPickH2) && (g_groups & kGCls) && (g_groups & kGClsFlags) && (g_groups & kGPlayer) && (g_sense_in & sense::kCls)) p |= kPickH2;
    if ((g_cfg.pick & kPickForce) && (g_groups & kGForce) && (g_groups & kGPsr)) p |= kPickForce;
    // (M3f) 16 with 1 live and the raycast's self-test passed; 32 with 16
    if ((g_cfg.pick & kPickSight) && (p & kPickReorder) && g_los_state == 1) p |= kPickSight;
    if ((g_cfg.pick & kPickSynth) && (p & kPickSight)) p |= kPickSynth;
    if ((g_cfg.pick & (kPickSight | kPickSynth)) && (!g_bits_logged_any || ((p ^ g_bits_logged) & (kPickSight | kPickSynth)))) {
        const char* why16 = (p & kPickSight)              ? "the self-test passed, bit 1 live"
                            : !(g_cfg.pick & kPickSight)  ? "not in the key"
                            : !(p & kPickReorder)         ? "bit 1 not live"
                            : g_los_state == 0            ? "waiting for the raycast's self-test"
                            : g_los_state == 2            ? "the raycast's self-test failed"
                                                          : "a ray fault";
        const char* why32 = (p & kPickSynth) ? "bit 16 live" : !(g_cfg.pick & kPickSynth) ? "not in the key" : "bit 16 not live";
        if (!g_bits_logged_any || ((p ^ g_bits_logged) & kPickSight))
            Log("[enemy] tgt pick 16 (line of sight): %s (%s)", (p & kPickSight) ? "on" : "off", why16);
        if ((g_cfg.pick & kPickSynth) && (!g_bits_logged_any || ((p ^ g_bits_logged) & kPickSynth)))
            Log("[enemy] tgt pick 32 (her record from ours): %s (%s)", (p & kPickSynth) ? "on" : "off", why32);
        g_bits_logged = p, g_bits_logged_any = true;
    }
    g_pick_live.store(p, std::memory_order_release);
    g_redir_live.store(SitesFor(RedirWant()) & g_sites_in, std::memory_order_release);
    // bit 32's hooks record the shoot job's aim point whenever they are in (read-only), whatever the redirect bits
    g_rec_live.store((g_sites_in & kS32) != 0 && PickOrRedirWanted(), std::memory_order_release);
}

// ---- the table: registration (1 Hz), own places (10 Hz)
void WipeTable() noexcept {
    for (Slot& s : g_slots) {
        s.key.store(0, std::memory_order_release);
        s.ind.store(0), s.id.store(0), s.cur.store(0), s.rule.store(0), s.force.store(0), s.chg_rule.store(0);
        s.ent.store(0), s.reg_ms.store(0), s.own_ms.store(0), s.pt_ms.store(0), s.los_ms.store(0), s.los_v.store(0);
        s.tomb_ms = 0, s.entity = 0, s.agent = 0, s.logged_cur = 0, s.logged_rule = 0, s.mm_logged = false;
    }
    for (IndIx& e : g_ix) e.key.store(0, std::memory_order_release), e.slot.store(-1), e.tomb_ms = 0;
    for (IndIx& e : g_ex) e.key.store(0, std::memory_order_release), e.slot.store(-1), e.tomb_ms = 0;
    g_registered = 0;
}
void IxPut(IndIx* tab, std::uintptr_t ind, int slot) noexcept {
    const unsigned h = R::TgtHash(ind);
    const std::uint64_t now = TNow();
    IndIx* free_e = nullptr;
    for (unsigned i = 0; i < kProbe; ++i) {
        IndIx& e = tab[(h + i) & (kSlots - 1)];
        const std::uintptr_t v = e.key.load(std::memory_order_relaxed);
        if (v == ind) {
            e.slot.store(slot, std::memory_order_release);
            return;
        }
        if (!free_e && (v == 0 || (v == 1 && now - e.tomb_ms >= 2000))) free_e = &e;
        if (v == 0) break;
    }
    if (!free_e) return;
    free_e->slot.store(slot, std::memory_order_release);
    free_e->key.store(ind, std::memory_order_release);
}
// `slot` >= 0: only while the entry still names that slot (another slot may have taken the key since)
void IxDrop(IndIx* tab, std::uintptr_t ind, int slot = -1) noexcept {
    const unsigned h = R::TgtHash(ind);
    for (unsigned i = 0; i < kProbe; ++i) {
        IndIx& e = tab[(h + i) & (kSlots - 1)];
        const std::uintptr_t v = e.key.load(std::memory_order_relaxed);
        if (v == ind) {
            if (slot >= 0 && e.slot.load(std::memory_order_relaxed) != slot) return;
            e.tomb_ms = TNow();
            e.key.store(1, std::memory_order_release);
            return;
        }
        if (v == 0) return;
    }
}
Slot* Insert(std::uintptr_t k) noexcept {
    if (Slot* s = Find(k)) return s;
    const unsigned h = R::TgtHash(k);
    const std::uint64_t now = TNow();
    for (unsigned i = 0; i < kProbe; ++i) {
        Slot& s = g_slots[(h + i) & (kSlots - 1)];
        const std::uintptr_t v = s.key.load(std::memory_order_relaxed);
        if (v == 0 || (v == 1 && now - s.tomb_ms >= 2000)) {
            s.cur.store(0), s.rule.store(0), s.force.store(0), s.since.store(0), s.seen_p.store(0), s.seen_l.store(0), s.hit_p.store(0), s.hit_l.store(0);
            s.dp.store(std::numeric_limits<float>::infinity()), s.dl.store(std::numeric_limits<float>::infinity());
            s.n_front.store(0), s.n_supp.store(0), s.n_calls.store(0), s.flips.store(0), s.last_p.store(0), s.max_gap.store(0), s.reg_ms.store(0);
            s.sh_calls.store(0), s.sh_redir.store(0), s.sh_pts.store(0), s.pt_ms.store(0), s.pt_mode.store(-1), s.own_ms.store(0);
            s.pt_actor.store(0), s.pt_place.store(0), s.pt_other.store(0), s.chg_rule.store(0), s.sh_t36.store(0);
            s.los_v.store(0), s.los_ms.store(0), s.n_est_keep.store(0), s.n_synth.store(0), s.n_synth_skip.store(0), s.first_p.store(0);
            s.arm_ms = 0, s.los_next = 0, s.los_logged = 0, s.los_logs = 0;
            for (unsigned& c : s.los_n) c = 0;
            s.ind.store(0), s.id.store(0), s.ent.store(0);  // (a reused slot: Register() fills these and the two indexes)
            s.entity = 0, s.agent = 0, s.pass = 0, s.pt_seen_ms = 0, s.pt_her = 0, s.pt_sam = 0, s.pt_none = 0;
            s.logged_cur = 0, s.logged_rule = 0, s.tomb_ms = 0, s.mm_logged = false;
            s.key.store(k, std::memory_order_release);
            return &s;
        }
    }
    return nullptr;
}
void Drop(Slot& s) noexcept {
    const std::uintptr_t ind = s.ind.load(), ent = s.ent.load();
    if (ind) IxDrop(g_ix, ind);
    if (ent) IxDrop(g_ex, ent, static_cast<int>(&s - g_slots));
    s.tomb_ms = TNow();
    s.key.store(1, std::memory_order_release);
    if (g_registered) --g_registered;
}
void Register() noexcept {
    ++g_reg_pass;
    const std::uint64_t now_ms = TNow();
    WalkActors();
    // the MULE members (H5's bitset; the ids to register). Fixed arrays: no allocation in the tick (review S11). A member is not an
    // actor (its id at +0x20); the walk counts only when at least one id resolves to a MULE actor (review S9 / L11).
    const std::uintptr_t mule = (g_groups & kGMule) ? mem::Q(A(kMuleMgr)) : 0;
    std::int32_t members = 0;
    unsigned nids = 0, resolved = 0;
    if (mule && mem::Rd(mule + 0x30, members) && members > 0 && members <= static_cast<std::int32_t>(kMemberCap) &&
        mem::Read(mem::Q(mule + 0x38), g_members, static_cast<std::size_t>(members) * 8)) {
        for (std::int32_t i = 0; i < members; ++i) {
            std::uint32_t id = 0;
            const std::uintptr_t p = g_members[i];
            if (p && mem::Rd(p + 0x20, id) && enemy::IsHumanEnemyId(id)) {
                g_member_ids[nids++] = id;
                if (ActorOf(id)) ++resolved;
            }
        }
    }
    if (resolved) {
        const int back = 1 - g_h5_ix.load();
        std::uint64_t bits[128] = {};
        for (unsigned i = 0; i < nids; ++i) bits[(g_member_ids[i] & 0x1FFFu) >> 6] |= 1ull << (g_member_ids[i] & 63u);
        for (int i = 0; i < 128; ++i) g_h5[back][i].store(bits[i], std::memory_order_relaxed);
        g_h5_ix.store(back, std::memory_order_release);
        g_h5_ready.store(true, std::memory_order_release);
    }
    for (unsigned i = 0; i < nids; ++i) {
        const std::uint32_t id = g_member_ids[i];
        const std::uintptr_t actor = ActorOf(id);
        const std::uintptr_t e = EntityOf(actor);
        const std::uintptr_t agent = e ? mem::Q(e + kEAgent) : 0;
        const std::uintptr_t ind = agent ? Individual(e) : 0;
        const std::uintptr_t k = ind ? mem::Q(ind + kIndK) : 0;
        if (!k || !KnowOf(k, agent)) continue;
        Slot* s = Insert(k);
        if (!s) continue;
        if (s->ind.load() != ind || s->id.load() != id) {
            if (s->ind.load() && s->ind.load() != ind) IxDrop(g_ix, s->ind.load());
            const bool fresh = s->ind.load() == 0;
            s->id.store(id, std::memory_order_relaxed);
            s->ind.store(ind, std::memory_order_release);
            IxPut(g_ix, ind, static_cast<int>(s - g_slots));
            if (fresh) ++g_registered;
            if (mem::Q(k + kKX) != ind && g_ind_mismatch++ == 0)
                Log("[enemy] tgt note: id=%05X K+0x28 %p is not its AIIndividualComponent's individual %p (the pick keys by K; the redirects by the "
                    "individual)",
                    id, reinterpret_cast<void*>(mem::Q(k + kKX)), reinterpret_cast<void*>(ind));
        }
        // bit 32's index: the carrier's entity -> this slot (put every pass: an entity another slot held before is taken back)
        if (s->ent.load() && s->ent.load() != e) IxDrop(g_ex, s->ent.load(), static_cast<int>(s - g_slots));
        s->ent.store(e, std::memory_order_release);
        IxPut(g_ex, e, static_cast<int>(s - g_slots));
        s->entity = e, s->agent = agent, s->pass = g_reg_pass;
        s->reg_ms.store(now_ms, std::memory_order_release);
    }
    // the ones not listed this pass go
    for (Slot& s : g_slots)
        if (s.key.load() > 1 && s.pass != g_reg_pass) Drop(s);
}
void UpdateOwn() noexcept {
    for (Slot& s : g_slots) {
        if (s.key.load(std::memory_order_relaxed) <= 1 || !s.entity) continue;
        double p[3];
        if (!Pos(s.entity, p) || mem::Q(s.entity + kEAgent) != s.agent) continue;
        const std::uint32_t q = s.own_seq.load(std::memory_order_relaxed);
        s.own_seq.store(q + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (int i = 0; i < 3; ++i) s.own[i].store(p[i], std::memory_order_relaxed);
        s.own_ms.store(TNow(), std::memory_order_relaxed);
        s.own_seq.store(q + 2, std::memory_order_release);
    }
}
Slot* SlotOfId(std::uint32_t id) noexcept {
    for (Slot& s : g_slots)
        if (s.key.load(std::memory_order_relaxed) > 1 && s.id.load(std::memory_order_relaxed) == (id & kIdMask)) return &s;
    return nullptr;
}
// H3: the hits drained; pick bit 8: her hit -> K+0x6A0 her chest, then K+0x736 1 (PSR builds a PlayerVisual there next call)
void DrainHits() noexcept {
    for (auto& h : g_hits) {
        const std::uint64_t v = h.exchange(0, std::memory_order_acquire);
        if (!v) continue;
        const std::uint64_t ms = v >> 23;
        const std::uint32_t id = static_cast<std::uint32_t>((v >> 2) & kIdMask);
        const std::uint8_t who = static_cast<std::uint8_t>(v & 3u);
        Slot* s = SlotOfId(id);
        if (!s) continue;
        (who == R::kPartner ? s->hit_p : s->hit_l).store(ms, std::memory_order_relaxed);
        if (who == R::kPartner && (g_pick_live.load() & kPickForce)) {
            // (review S2) K still this MULE's (its vtable, its agent, its individual's +0x3AD8) before the game object is written
            double c[3];
            const std::uintptr_t k = s->key.load(), ind = s->ind.load();
            const std::uint8_t one = 1;
            if (k <= 1 || !ind || !s->agent || !KnowOf(k, s->agent) || mem::Q(ind + kIndK) != k || !ReadPartner(kChest, c, TNow())) continue;
            if (!SPut(k + kKForcePos, c, sizeof(c))) continue;
            std::atomic_thread_fence(std::memory_order_release);  // the place before the flag (PSR reads them on a worker)
            if (SPut(k + kKForce, &one, 1)) ++g_forced_vis;
        }
    }
}
// by place (4 Hz): a MULE's newest PlayerVisual (else its freshest LastKnown) attributed to the nearer player body
// (run 3) `type_out` / `age_out`: the entry it went by (1 PlayerVisual, 85 PlayerLastKnown; its age in s)
int PlaceWho(std::uintptr_t k, bool her_ok, const double (&her)[3], bool sam_ok, const double (&sam)[3], int* type_out = nullptr, float* age_out = nullptr) noexcept {
    std::int32_t nb = 0;
    const std::uintptr_t arr = k ? mem::Q(k + kKBuckets) : 0;
    if (!arr || !mem::Rd(k + kKBucketN, nb) || nb <= 0 || nb > 256) return L::kNone;
    int pv = L::kNone, lk = L::kNone;
    float pv_age = 1e30f, lk_age = 1e30f;
    for (std::int32_t b = 0; b < nb; ++b) {
        std::int32_t c = 0;
        const std::uintptr_t ents = mem::Q(arr + 16ull * static_cast<unsigned>(b) + 8);
        if (!mem::Rd(arr + 16ull * static_cast<unsigned>(b), c) || c <= 0 || c > 256 || !ents) continue;
        for (std::int32_t i = 0; i < c; ++i) {
            std::uint8_t raw[kKeSize];
            if (!mem::Read(ents + kKeSize * static_cast<unsigned>(i), raw, kKeSize)) continue;
            std::int32_t type = 0;
            double pos[3] = {};
            float age = 0;
            std::memcpy(&type, raw + kKeType, 4);
            std::memcpy(pos, raw + kKePos, 24);
            std::memcpy(&age, raw + kKeAge, 4);
            if (type == 1 && age < pv_age) {
                const int w = L::Nearest(pos, her_ok, her, sam_ok, sam);
                if (w != L::kNone) pv = w, pv_age = age;
            } else if (type == 85 && age < lk_age) {
                const int w = L::Nearest(pos, her_ok, her, sam_ok, sam, 3.0);
                if (w != L::kNone) lk = w, lk_age = age;
            }
        }
    }
    if (type_out) *type_out = pv != L::kNone ? 1 : lk != L::kNone ? 85 : 0;
    if (age_out) *age_out = pv != L::kNone ? pv_age : lk != L::kNone ? lk_age : -1.f;
    return pv != L::kNone ? pv : lk;
}
void PlaceTick() noexcept {
    double her[3] = {}, sam[3] = {};
    const bool her_ok = g_body && Pos(g_body, her), sam_ok = g_local && g_local_ok && Pos(g_local, sam);
    const bool hook = (g_pick_live.load() & kPickReorder) != 0;
    unsigned n = 0;
    for (Slot& s : g_slots) {
        if (s.key.load(std::memory_order_relaxed) <= 1 || n >= 64) continue;
        ++n;
        int etype = 0;
        float eage = -1.f;
        const int w = PlaceWho(s.key.load(), her_ok, her, sam_ok, sam, &etype, &eage);
        if (w == L::kNone) continue;
        const std::uint8_t pw = w == L::kHer ? R::kPartner : R::kLocal;
        if (hook) {
            const std::uint8_t cur = s.cur.load(std::memory_order_relaxed);
            if (cur != R::kNone && cur != pw) {
                ++g_mismatch;
                // (run 3) the first per slot per arm, for the [U] of the run's mismatch total
                if (!s.mm_logged)
                    s.mm_logged = true, Log("[enemy] tgt mismatch id=%05X: pick %s, place says %s (entry type %d age %.1f s)", s.id.load(),
                                            R::WhoText(static_cast<R::Who>(cur)), R::WhoText(static_cast<R::Who>(pw)), etype, static_cast<double>(eage));
            }
        } else if (s.force.load() == 0) {  // no reorder hook: the pick by place, for the redirects and the logs
            if (s.cur.load() != pw) s.since.store(TNow(), std::memory_order_relaxed);
            s.rule.store(R::rPlace, std::memory_order_relaxed);
            const std::uint8_t nc = g_p_agent.load() ? pw : (pw == R::kPartner ? R::kNone : pw);
            if (s.cur.load() != nc) s.chg_rule.store(R::rPlace, std::memory_order_relaxed);
            s.cur.store(nc, std::memory_order_release);
        }
    }
}
// bit 32 (4 Hz, with the place tick): each slot's new aim point of the shoot job attributed to the player body it was at
unsigned g_pt_her_all = 0, g_pt_sam_all = 0, g_pt_none_all = 0;
void ShootTally() noexcept {
    double her[3] = {}, sam[3] = {};
    const bool her_ok = g_body && Pos(g_body, her), sam_ok = g_local && g_local_ok && Pos(g_local, sam);
    for (Slot& s : g_slots) {
        if (s.key.load(std::memory_order_relaxed) <= 1) continue;
        double p[3];
        std::uint64_t ms = 0;
        std::int32_t mode = -1;
        if (!ReadShootPt(s, p, &ms, &mode) || ms == s.pt_seen_ms) continue;
        s.pt_seen_ms = ms;
        const int w = L::Nearest(p, her_ok, her, sam_ok, sam);
        if (w == L::kHer) ++s.pt_her, ++g_pt_her_all;
        else if (w == L::kSam) ++s.pt_sam, ++g_pt_sam_all;
        else ++s.pt_none, ++g_pt_none_all;
    }
}
// a slot's shoot numbers (game thread)
void FillShoot(Slot& s, ShootStat& o) noexcept {
    o = ShootStat{};
    o.calls = s.sh_calls.load(std::memory_order_relaxed), o.redirected = s.sh_redir.load(std::memory_order_relaxed);
    o.points = s.sh_pts.load(std::memory_order_relaxed);
    o.at_her = s.pt_her, o.at_sam = s.pt_sam, o.at_none = s.pt_none;
    o.pt_actor = s.pt_actor.load(std::memory_order_relaxed), o.pt_place = s.pt_place.load(std::memory_order_relaxed);
    o.pt_other = s.pt_other.load(std::memory_order_relaxed);
    o.t36 = s.sh_t36.load(std::memory_order_relaxed);
    std::uint64_t ms = 0;
    std::int32_t mode = -1;
    if (!ReadShootPt(s, o.point, &ms, &mode)) return;
    o.have = true, o.mode = mode;
    const std::uint64_t now = TNow();
    o.age_ms = now >= ms ? static_cast<double>(now - ms) : 0.0;
    double her[3] = {}, sam[3] = {};
    const bool her_ok = g_body && Pos(g_body, her), sam_ok = g_local && g_local_ok && Pos(g_local, sam);
    o.d_her = her_ok ? L::SegDist(o.point, her) : -1, o.d_sam = sam_ok ? L::SegDist(o.point, sam) : -1;
    o.nearest = L::Nearest(o.point, her_ok, her, sam_ok, sam);
}
void ShootLine(Slot& s) noexcept {
    ShootStat o;
    FillShoot(s, o);
    Log("[enemy] tgt id=%05X shoot: calls %u redirected %u points %u (at her %u Sam %u none %u); last %s mode %d age %.1f s, her %.1f m, Sam %.1f m; "
        "modes actor %u place %u other %u; t36 %u",
        s.id.load(), o.calls, o.redirected, o.points, o.at_her, o.at_sam, o.at_none, o.have ? L::WhoText(o.nearest) : "-", o.mode,
        o.have ? o.age_ms / 1000.0 : -1.0, o.d_her, o.d_sam, o.pt_actor, o.pt_place, o.pt_other, o.t36);
}
void LogChanges() noexcept {
    for (Slot& s : g_slots) {
        if (s.key.load(std::memory_order_relaxed) <= 1) continue;
        // (run 3) the rule of the change itself (chg_rule, stored before `cur`), not the state's rule now (a rule-3 switch reads
        // "sticky" one tick later)
        const std::uint8_t cur = s.cur.load(std::memory_order_acquire), rule = s.chg_rule.load(std::memory_order_relaxed);
        if (cur == s.logged_cur) continue;
        s.logged_cur = cur, s.logged_rule = rule;
        if (cur == R::kNone) continue;
        Log("[enemy] tgt id=%05X -> %s (%s)", s.id.load(), R::WhoText(static_cast<R::Who>(cur)), R::RuleText(static_cast<R::Rule>(rule)));
    }
}
// (run 3) the first instance of each worker-side diagnostic, logged once from the tick
void LogFirsts() noexcept {
    if (g_first_vt_other.Take())
        Log("[enemy] tgt shoot 32: the job's carrier has vtable 0x%llX (not the MULE's / the NPC's); not counted",
            static_cast<unsigned long long>(g_first_vt_other.b.load()));
    if (g_first_noslot.Take())
        Log("[enemy] tgt shoot 32: carrier 0x%llX entity 0x%llX is not a registered MULE", static_cast<unsigned long long>(g_first_noslot.a.load()),
            static_cast<unsigned long long>(g_first_noslot.b.load()));
    if (g_first_onhit_bad.Take())
        Log("[enemy] tgt onhit_bad first: receiver %05X aref 0x%llX att 0x%llX vt 0x%llX node 0x%llX", static_cast<unsigned>(g_first_onhit_bad.a.load()),
            static_cast<unsigned long long>(g_first_onhit_bad.b.load()), static_cast<unsigned long long>(g_first_onhit_bad.c.load()),
            static_cast<unsigned long long>(g_first_onhit_bad.d.load()), static_cast<unsigned long long>(g_first_onhit_bad.e.load()));
    if (g_first_onhit_odd.Take()) {
        // (M3f) the attacker's class by its RTTI (read here, on the game thread, under the tick's guard)
        const std::uintptr_t att = static_cast<std::uintptr_t>(g_first_onhit_odd.a.load());
        const std::uintptr_t rt = att ? mem::RttiOf(att) : 0;
        const char* name = rt ? mem::RttiName(rt) : nullptr;
        Log("[enemy] tgt onhit odd first: att 0x%llX vt 0x%llX (%s) inst 0x%llX inst_vt 0x%llX", static_cast<unsigned long long>(att),
            static_cast<unsigned long long>(g_first_onhit_odd.b.load()), name ? name : "?", static_cast<unsigned long long>(g_first_onhit_odd.c.load()),
            static_cast<unsigned long long>(g_first_onhit_odd.d.load()));
    }
}
Snap ReadSnap() noexcept {
    Snap n;
    for (Slot& s : g_slots) {
        n.front += s.n_front.load(std::memory_order_relaxed);
        n.supp += s.n_supp.load(std::memory_order_relaxed);
    }
    n.r1 = g_wc.r1.load(), n.r2a = g_wc.r2a.load(), n.r2a_off = g_wc.r2a_off.load(), n.r2b = g_wc.r2b.load(), n.rveh = g_wc.rveh.load();
    n.r4 = g_wc.r4.load(), n.r4n = g_wc.r4_not_ours.load();
    n.h5 = g_wc.h5.load(), n.h2r = g_wc.h2_refuse.load(), n.h2f = g_wc.h2_fix.load(), n.h2nb = g_wc.h2_nobucket.load(), n.faults = g_wc.faults.load();
    n.forced = g_forced_vis;
    n.sh_calls = g_wc.sh_calls.load(), n.sh_redir = g_wc.sh_redir.load(), n.sh_pts = g_wc.sh_pts.load();
    n.pt_her = g_pt_her_all, n.pt_sam = g_pt_sam_all, n.pt_none = g_pt_none_all;
    return n;
}
void Summary(const char* when) noexcept {
    unsigned her = 0, sam = 0;
    for (Slot& s : g_slots) {
        if (s.key.load(std::memory_order_relaxed) <= 1) continue;
        const std::uint8_t c = s.cur.load(std::memory_order_relaxed);
        if (c == R::kPartner) ++her;
        else if (c == R::kLocal) ++sam;
    }
    const Snap n = ReadSnap();
    const Snap& o = g_last_snap;
    // (the reorders / suppress counts restart when a slot is reused; a negative delta reads as the new total)
    auto d = [](unsigned a, unsigned b) { return a >= b ? a - b : a; };
    // (round 27 run 2) bit 32's shoot numbers (deltas) and the soft counters (totals) at the end
    Log("[enemy] tgt%s%s: engaging her %u, Sam %u; reorders %u; suppress %u; redirects 1:%u 2a:%u (offset %u) 2b:%u 4:%u (not ours %u) veh:%u; "
        "h5 %u h2 %u/%u (no type-1 bucket %u) forced %u; registered %u; faults %u; mismatch %u; table clears %u; shoot 32: calls %u redirected %u "
        "points %u (at her %u Sam %u none %u); soft onhit_bad %u psr_rec_bad %u turn_goal_bad %u; shoot32 vt_other %u noslot %u; t36 %u; "
        "sight keeps %u synth %u skip %u rays %u; onhit_odd %u",
        when ? " " : "", when ? when : "", her, sam, d(n.front, o.front), d(n.supp, o.supp), d(n.r1, o.r1), d(n.r2a, o.r2a), d(n.r2a_off, o.r2a_off),
        d(n.r2b, o.r2b), d(n.r4, o.r4), d(n.r4n, o.r4n), d(n.rveh, o.rveh), d(n.h5, o.h5), d(n.h2r, o.h2r), d(n.h2f, o.h2f), d(n.h2nb, o.h2nb),
        d(n.forced, o.forced), g_registered, n.faults, g_mismatch, g_table_clears, d(n.sh_calls, o.sh_calls), d(n.sh_redir, o.sh_redir),
        d(n.sh_pts, o.sh_pts), d(n.pt_her, o.pt_her), d(n.pt_sam, o.pt_sam), d(n.pt_none, o.pt_none), g_wc.onhit_bad.load(), g_wc.psr_rec_bad.load(),
        g_wc.turn_goal_bad.load(), g_wc.sh_vt_other.load(), g_wc.sh_noslot.load(), g_wc.sh_t36.load(), g_wc.est_keep.load(), g_wc.synth.load(),
        g_wc.synth_skip.load(), g_sight_rays, g_wc.onhit_odd.load());
    g_last_snap = n;
}

// ---- the stop paths
void StopAll(const char* why, bool fault) noexcept {
    const bool was = g_stopped;
    g_stopped = true;
    g_stop_at = g_now;
    g_stop_why = why;
    g_pick_live.store(0), g_redir_live.store(0), g_rec_live.store(false);
    g_p_agent.store(0, std::memory_order_release);
    if (g_ps.stim && !g_ps.pending) Release(why);
    if (fault)
        Log("[enemy] tgt FAULT (%s): the partner stimulus released, the table cleared, the pick and every redirect off for this run (made %u removed %u "
            "forgotten %u)",
            why, g_made, g_removed, g_forgotten);
    else if (!was && (g_cfg.stim || PickOrRedirWanted()))
        Log("[enemy] tgt stopped (%s): the partner stimulus released, the pick and every redirect off", why);
}

// ---- SPV1
bool SpvFresh() noexcept { return g_spv_have && g_now - g_spv_at <= 1000; }
void SpvReceive() noexcept {
    std::uint8_t buf[64];
    std::size_t n = 0;
    std::uint32_t gen = 0;
    for (int i = 0; i < 32 && link::PopCombatFrame(link::CombatFrame::visibility, buf, sizeof(buf), n, gen); ++i) {
        ewire::Visibility v;
        if (!ewire::DecodeVisibility(buf, n, v)) {
            ++g_spv_bad;
            continue;
        }
        // a new link session, or none fresh for 1 s (review L9: a partner that restarted its game starts its seq again): any frame
        if (gen != g_spv_gen || !SpvFresh()) g_spv_have = false, g_spv_gen = gen;
        if (g_spv_have && static_cast<std::int16_t>(v.seq - g_spv.seq) <= 0) {
            ++g_spv_stale;
            continue;
        }
        g_spv = v, g_spv_have = true, g_spv_at = g_now;
        ++g_spv_recv;
    }
}
// her aim point's height above her feet (m): her SPV1 aim height when fresh and 0.30..2.20 m, else kChestDefault (review A3)
double HerChestDz(bool* from_spv = nullptr) noexcept {
    const bool ok = SpvFresh() && g_spv.aim_cm >= 30 && g_spv.aim_cm <= 220;
    if (from_spv) *from_spv = ok;
    return ok ? g_spv.aim_cm / 100.0 : kChestDefault;
}
void SpvSend() noexcept {
    if (!g_net || !g_local || !g_local_ok || g_now < g_spv_next) return;
    g_spv_next = g_now + 100;
    ewire::Visibility v;
    const std::uintptr_t ss = SamStimNow();
    if (ss) v.vis = F32(ss + kSMul), v.a0 = F32(ss + kSA0), v.b0 = F32(ss + kSB0);
    if (!std::isfinite(v.vis)) v.vis = 1.f;
    if (!std::isfinite(v.a0)) v.a0 = 0;
    if (!std::isfinite(v.b0)) v.b0 = 0;
    if (g_our_down) v.flags |= ewire::kSpvDown;
    if (esync::CutsceneNow()) v.flags |= ewire::kSpvCutscene;
    v.weapon = U8(g_local + kPWeapon);
    v.health = 0xFF;  // unknown (not read)
    // (review) the classifier's flags word and the aim point are the PLAYER OBJECT's (+0x2B0 / info +0x198), not the entity's
    const std::uintptr_t pl = PlayerOf(g_local);
    v.pinfo_flags = pl ? U32(pl + kPlInfo) : 0;
    v.aim_cm = g_aim_cm = AimCm(pl, g_local);
    v.seq = ++g_spv_seq;
    v.sender_ms = static_cast<std::uint32_t>(link::NowMs());
    std::uint8_t out[ewire::kSpvSize];
    if (ewire::EncodeVisibility(v, out, sizeof(out)) && link::SendCombatFrame(out, sizeof(out))) ++g_spv_sent;
}

// ---- the partner stimulus (M2)
const char* BodyRefusal() noexcept {
    if (!g_body) return "no partner body";
    if (reinterpret_cast<std::uintptr_t>(realbody::LiveEntity()) != g_body) return "the partner body is not the live real body";
    if (realbody::GetOwner() != realbody::Owner::partner) return "the real body is not the partner's";
    if (reinterpret_cast<std::uintptr_t>(holo::LiveEntity()) == g_body) return "the partner body is the hologram";
    return nullptr;
}
// a hard reason: release at once; a soft one: after 2 s
const char* HardReason() noexcept {
    std::uint64_t st = 0;
    std::uint32_t mask = 0;
    if (g_forced_off) return "forced off (op 1 0)";
    if (!g_cfg.stim) return "the key is off";
    // (review S1) the host only: the guest's partner body is the host's Sam, whose enemies the host owns
    if (g_role == 2) return "the guest (host only)";
    if (g_role != 1) return "no session (host only)";
    // (review S1) hidden runs only for now: the real body can be destroyed by the game before ReleaseBeforeFree runs (the
    // body-destroy gap), and no live session may meet that before it is closed
    if (!automation::Enabled()) return "hidden runs only (the game's body-destroy gap)";
    if (!g_net) return "no link";
    if (const char* b = BodyRefusal()) return b;
    if (!g_local || !g_local_ok) return "Sam asleep";
    if (game::LoadOrSetup(&st, &mask)) return "a load or setup";
    if (esync::PeerDownNow() || (SpvFresh() && (g_spv.flags & ewire::kSpvDown))) return "the partner is down";
    return nullptr;
}
const char* SoftReason() noexcept {
    // (review S4) her down / cutscene state comes from SPV1: none fresh is a reason (released after 2 s, no create meanwhile)
    if (!SpvFresh()) return "no fresh SPV1 from the partner";
    if (esync::CutsceneNow()) return "a cutscene here";
    if (SpvFresh() && (g_spv.flags & ewire::kSpvCutscene)) return "a cutscene in the partner's game";
    if (g_now >= g_near_next) {
        g_near_next = g_now + 500;
        double p[3];
        g_near_m = Pos(g_body, p) ? NearestEnemy(p) : -1;
    }
    if (g_near_m < 0 || g_near_m > g_cfg.radius) return "no realized human enemy within the radius";
    return nullptr;
}
void NotMade(const char* why) noexcept {
    if (why == g_last_not && g_now - g_not_logged_at < 30000) return;
    g_last_not = why, g_not_logged_at = g_now;
    Log("[enemy] pstim not made (%s)", why);
}
float VisNow() noexcept {
    if (g_vis_forced) return g_vis_force;
    if (g_cfg.vis && SpvFresh()) return g_spv.vis;
    return 1.f;
}
void StimWrite() noexcept {
    double p[3] = {};
    if (!Pos(g_body, p)) return;
    double off[3] = {0, 0, 1};
    float a0 = 0, b0 = 0;
    const std::uintptr_t ss = SamStimNow();
    double sp[3] = {}, sam[3] = {};
    if (ss && mem::Read(ss + kSPos, sp, sizeof(sp)) && Pos(g_local, sam) && L::Dist(sp, sam) < 3.0)
        off[0] = sp[0] - sam[0], off[1] = sp[1] - sam[1], off[2] = sp[2] - sam[2];
    if (SpvFresh()) a0 = g_spv.a0, b0 = g_spv.b0;
    else if (ss) a0 = F32(ss + kSA0), b0 = F32(ss + kSB0);
    const double at[3] = {p[0] + off[0], p[1] + off[1], p[2] + off[2]};
    float vel[4] = {};
    const double dt = (g_now - g_ps.last_at) / 1000.0;
    if (g_ps.have_last && dt > 0.005 && dt < 1.0 && L::Dist(p, g_ps.last_pos) <= 2.0) {
        double v[3], sp2 = 0;
        for (int i = 0; i < 3; ++i) v[i] = (p[i] - g_ps.last_pos[i]) / dt, sp2 += v[i] * v[i];
        const double speed = std::sqrt(sp2), k = speed > 12.0 ? 12.0 / speed : 1.0;
        for (int i = 0; i < 3; ++i) vel[i] = static_cast<float>(v[i] * k);
    }
    std::memcpy(g_ps.last_pos, p, sizeof(p));
    g_ps.have_last = true, g_ps.last_at = g_now;
    const float vis = VisNow();
    g_last_vis = vis;
    Lock lk(g_ps.mgr);
    if (!lk.held) {
        StimDead("the pool lock");
        return Forget("the pool lock faulted");
    }
    if (!InLiveList(g_ps.mgr, g_ps.stim) || !OursLiveP(g_ps.stim)) return Forget("no longer ours: not in the live list, or its agent or resource changed");
    int r = SehWrite(g_ps.stim + kSPos, at, sizeof(at));
    if (r > 0) r = SehWrite(g_ps.stim + kSVel, vel, sizeof(vel));
    if (r > 0) r = SehWrite(g_ps.stim + kSMul, &vis, 4);
    if (r > 0) r = SehWrite(g_ps.stim + kSA0, &a0, 4);
    if (r > 0) r = SehWrite(g_ps.stim + kSB0, &b0, 4);
    if (r > 0) r = SehWrite(g_ps.stim + kSId, &g_ps.id88, 4);
    if (r < 0) {
        ++g_faults;
        Log("[enemy] pstim tick write FAULT: released");
        Release("a write fault");
    }
}
// the create (both games); `ignore_radius` for op 9's cycles. Returns the refusal, or nullptr when made.
const char* Make(bool ignore_radius, bool quiet) noexcept {
    if (g_ps.stim) return "already made";
    if (!(g_groups & kGStim)) return "byte checks";
    if (g_stim_dead) return "an earlier create or remove faulted";
    if (const char* h = HardReason()) return h;
    if (!ignore_radius)
        if (const char* s = SoftReason()) return s;
    const std::uintptr_t mgr = mem::Q(A(kStimMgr));
    if (!mgr) return "no AI manager";
    const std::uintptr_t agent = mem::Q(g_body + kEAgent);
    if (!agent) return "the partner body has no agent";
    // the agent gate (M1): its vt+0x10 must return the partner body (the sensor walks that entity's parent chain, no null check)
    if (SehAgentEntity(agent) != g_body) return "the partner agent's vt+0x10 is not the partner body";
    const std::uintptr_t res = SehResource(g_local);
    const char* rn = res ? mem::RttiName(mem::RttiOf(res)) : nullptr;
    if (!res || !rn || std::strcmp(rn, "AIVisualStimulusResource") != 0) return "our Sam's resource is not an AIVisualStimulusResource";
    double p[3] = {};
    if (!Pos(g_body, p)) return "the partner body has no place";
    std::uint32_t id = 0;
    const bool id_ok = (g_groups & kGReg) && RegistryPlayerId(&id);
    if (!id_ok) id = 0;
    const double at[3] = {p[0], p[1], p[2] + 1.0};
    const float vel[4] = {};
    bool fault = false;
    const std::uintptr_t s = SehVisCreate(mgr, res, at, vel, agent, &fault);
    if (!s) {
        if (fault) StimDead("the create");
        g_retry_at = g_now + 5000;
        return fault ? "the create FAULTED" : "the create returned none (the pool full); retry in 5 s";
    }
    const std::uint8_t zero = 0;
    SehWrite(s + kSId, &id, 4);  // the identity first (the same tick)
    SehWrite(s + kSOff, &zero, 1);
    g_ps = PStim{};
    g_ps.stim = s, g_ps.res = res, g_ps.agent = agent, g_ps.body = g_body, g_ps.mgr = mgr, g_ps.t0 = g_now, g_ps.id88 = id;
    Publish();
    ++g_made;
    g_bad_since = -1;
    g_last_not = nullptr;
    if (!quiet)
        Log("[enemy] pstim made #%u (id 0x%X%s, stim %p, agent %p, resource %p; nearest enemy %.0f m, radius %.0f)", g_made, id, id_ok ? "" : " (registry unread: 0)",
            reinterpret_cast<void*>(s), reinterpret_cast<void*>(agent), reinterpret_cast<void*>(res), g_near_m, g_cfg.radius);
    StimWrite();
    return nullptr;
}
void StimTick() noexcept {
    if (g_ps.pending) return Step2(false);
    if (g_ps.stim) {
        // the body changed or went, its agent changed, the manager changed: at once
        const char* why = nullptr;
        if (!g_body || g_body != g_ps.body) why = "the partner body changed or went";
        else if (mem::Q(A(kStimMgr)) != g_ps.mgr) why = "the AI manager changed";
        else if (mem::Q(g_body + kEAgent) != g_ps.agent) why = "the partner agent changed";
        else why = HardReason();
        if (why) return Release(why);
        if (const char* soft = SoftReason()) {
            if (g_bad_since < 0) g_bad_since = g_now;
            if (g_now - g_bad_since >= 2000) return Release(soft);
        } else {
            g_bad_since = -1;
        }
        StimWrite();
        return;
    }
    if (!g_cfg.stim || g_forced_off || g_now < g_retry_at) return;
    if (const char* why = Make(false, false)) NotMade(why);
}

void StatusLine() noexcept {
    const char* state = g_ps.stim && !g_ps.pending ? "on" : g_ps.pending ? "releasing" : g_stopped ? "stopped" : "off";
    char age[32];
    if (g_spv_have) std::snprintf(age, sizeof(age), "%.0f ms", g_now - g_spv_at);
    else std::snprintf(age, sizeof(age), "none");
    Log("[enemy] pstim: %s, vis %.2f (SPV1 age %s), made %u removed %u forgotten %u faults %u%s%s", state, static_cast<double>(g_ps.stim ? g_last_vis : VisNow()),
        age, g_made, g_removed, g_forgotten, g_faults, g_last_not && !g_ps.stim ? "; last refusal: " : "", g_last_not && !g_ps.stim ? g_last_not : "");
    bool her_spv = false;
    const double her_dz = HerChestDz(&her_spv);
    Log("[enemy] spv1 sent %u recv %u stale %u bad %u; aim ours %.2f m, hers %.2f m (%s); our down %d", g_spv_sent, g_spv_recv, g_spv_stale, g_spv_bad,
        g_aim_cm / 100.0, her_dz, her_spv ? "SPV1" : "default", g_our_down ? 1 : 0);
}

// (review S2) the table's gate: a load or setup, or another AI / MULE manager, clears the table with every live bit off first; the
// bits come back only after a registration pass outside a load. Returns true while loading.
bool TableGate() noexcept {
    std::uint64_t st = 0;
    std::uint32_t mask = 0;
    const bool loading = game::LoadOrSetup(&st, &mask);
    const std::uintptr_t ai = mem::Q(A(kStimMgr)), mule = (g_groups & kGMule) ? mem::Q(A(kMuleMgr)) : 0;
    const char* why = loading ? "a load or setup" : ai != g_seen_ai ? "the AI manager changed" : mule != g_seen_mule ? "the MULE manager changed" : nullptr;
    g_seen_ai = ai, g_seen_mule = mule;
    if (!why) return false;
    g_pick_live.store(0, std::memory_order_release), g_redir_live.store(0, std::memory_order_release);
    g_rec_live.store(false, std::memory_order_release);  // (review) bit 32's record off before the wipe (Recompute brings it back)
    g_h5_ready.store(false, std::memory_order_release);
    if (g_table_ok || g_registered) {
        WipeTable();
        ++g_table_clears;
        Log("[enemy] tgt table cleared (%s): the pick and the redirects off until a registration pass", why);
    }
    g_table_ok = false;
    g_reg_next = g_now;
    return loading;
}

// ---- (M3f) pick bit 16: the line-of-sight estimate (game thread, 10 Hz, at most kSightRays rays a tick). For every MULE whose
// pick is her: from its eye (its root + l1rules::kEye) to her chest (the partner record), the cheap tests first
// (erules::SightVerdict: range, view cone, pitch, her visibility), then one world ray (ecombat::WorldRay, the visual sense's
// filter; 0.6 m off the MULE's end, kSightHerTrimM off hers). The verdict and its time go to the slot; PsrOne reads them (est_p).
constexpr unsigned kSightRays = 4;
constexpr double kSightHerTrimM = 0.5;
// the MULE's facing (rad) from its rotation's second row (as GeomNow reads it)
bool MuleYaw(std::uintptr_t e, double* yaw) noexcept {
    WorldTransform tr{};
    std::uint8_t xb[0x3C];
    if (!mem::Read(e + 0xC8, xb, sizeof(xb))) return false;
    std::memcpy(&tr.rot, xb + 0x18, 0x24);
    *yaw = std::atan2(static_cast<double>(tr.rot.m[1][1]), static_cast<double>(tr.rot.m[1][0]));
    return std::isfinite(*yaw);
}
// One ray from `from` to `to`, `trim_from` / `trim_to` off its ends (WorldRay's result; `len` the full length)
int SightRay(const double (&from)[3], const double (&to)[3], double trim_from, double trim_to, double (&hit)[3], double* len) noexcept {
    const double d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    const double l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len) *len = l;
    if (!(l > trim_from + trim_to + 0.1)) return 0;
    double a[3], b[3];
    for (int i = 0; i < 3; ++i) a[i] = from[i] + d[i] / l * trim_from, b[i] = to[i] - d[i] / l * trim_to;
    double ignored = 0;
    ++g_sight_rays;
    return ecombat::WorldRay(a, b, 0.0, hit, &ignored);
}
void SightFault() noexcept {
    g_los_state = 3;
    Log("[enemy] tgt FAULT los ray (SEH) -- pick 16/32 off");
    for (Slot& s : g_slots) s.los_ms.store(0, std::memory_order_release);
    Recompute();
}
// Before the first estimate: a ray straight down from our Sam's chest must hit the ground at his root (within 0.75 m); else one
// 1 m beside him (a ray that starts inside his own body may report it: within 1.5 m there, a slope). (M3f review) Never during a
// load or before the first registration pass (TickBody's `loading`, g_table_ok); a miss, no physics world (-2), an odd return
// (-4) or a hit out of tolerance is tried again kSelfTestGapMs later, up to kSelfTestTries attempts, before it fails for the run;
// it fails at once only when the raycast is unchecked or the probes are off (-3), and a fault (-1) goes through SightFault.
constexpr unsigned kSelfTestTries = 10;
constexpr double kSelfTestGapMs = 500.0;
unsigned g_los_tries = 0;
void SightSelfTest() noexcept {
    double root[3] = {};
    if (!g_local || !g_local_ok || !Pos(g_local, root)) return;  // (tried again next tick; not an attempt)
    const double from[3] = {root[0], root[1], root[2] + L::kChest}, to[3] = {root[0], root[1], root[2] - 5.0};
    double hit[3] = {}, len = 0;
    const int r = SightRay(from, to, 0.0, 0.0, hit, &len);
    if (r == -1) return SightFault();
    int r2 = 0;
    bool ok = r > 0 && std::isfinite(hit[2]) && std::fabs(hit[2] - root[2]) <= 0.75;
    if (!ok && r >= 0) {
        const double f2[3] = {root[0] + 1.0, root[1], root[2] + L::kChest}, t2[3] = {root[0] + 1.0, root[1], root[2] - 5.0};
        double h2[3] = {};
        r2 = SightRay(f2, t2, 0.0, 0.0, h2, &len);
        if (r2 == -1) return SightFault();
        if (r2 > 0 && std::isfinite(h2[2]) && std::fabs(h2[2] - root[2]) <= 1.5) ok = true, hit[2] = h2[2];
    }
    ++g_los_tries;
    if (ok) {
        g_los_state = 1;
        Log("[enemy] tgt los self-test: ok (hit %.2f m below the chest; attempt %u)", from[2] - hit[2], g_los_tries);
        return Recompute();
    }
    const bool hard = r == -3 || r2 == -3;  // the raycast unchecked or the probes off: no retry helps
    if (!hard && g_los_tries < kSelfTestTries) {
        g_sight_next = g_now + kSelfTestGapMs;
        Log("[enemy] tgt los self-test: attempt %u of %u missed (%d/%d, hit z %.2f vs root %.2f); trying again", g_los_tries, kSelfTestTries, r, r2, hit[2],
            root[2]);
        return;
    }
    g_los_state = 2;
    Log("[enemy] tgt los self-test: failed (%d/%d, hit z %.2f vs root %.2f; %u attempts) -- %s; pick 16 off", r, r2, hit[2], root[2], g_los_tries,
        hard ? "the raycast is unchecked or the probes are off" : "the raycast misses the ground");
    Recompute();
}
void SightTick(bool loading) noexcept {
    if (!(g_cfg.pick & kPickSight) || g_los_state >= 2 || g_now < g_sight_next) return;
    if (g_los_state == 0 && (loading || !g_table_ok)) return;  // (M3f review) the self-test never runs mid-load
    g_sight_next = g_now + 100;
    if (g_los_state == 0) return SightSelfTest();
    if (!(g_pick_live.load(std::memory_order_relaxed) & kPickSight)) return;
    double her[3] = {};
    const bool her_ok = ReadPartner(kChest, her, TNow());
    const float vis = VisNow();
    const R::SightCfg cfg{};
    unsigned rays = 0, i = 0;
    for (; i < kSlots && rays < kSightRays; ++i) {
        Slot& s = g_slots[(g_sight_rr + i) & (kSlots - 1)];
        if (s.key.load(std::memory_order_relaxed) <= 1 || !s.entity || s.cur.load(std::memory_order_relaxed) != R::kPartner) continue;
        R::SightIn in;
        double m[3] = {}, yaw = 0;
        in.have = her_ok && Pos(s.entity, m) && mem::Q(s.entity + kEAgent) == s.agent && MuleYaw(s.entity, &yaw);
        const double eye[3] = {m[0], m[1], m[2] + L::kEye};
        if (in.have) {
            in.horiz = L::Dist2d(her, m);
            in.bearing_deg = L::FacingOff(yaw, m, her);
            in.pitch_deg = std::atan2(her[2] - eye[2], in.horiz > 0.01 ? in.horiz : 0.01) * 180.0 / L::kPi;
            in.vis = vis;
        }
        R::Sight v = R::SightVerdict(in, cfg);
        double hit[3] = {}, len = -1;
        if (v == R::Sight::none) {
            in.cast = true, in.ray = SightRay(eye, her, 0.6, kSightHerTrimM, hit, &len);
            ++rays;
            if (in.ray == -1) return SightFault();
            v = R::SightVerdict(in, cfg);
        }
        s.los_v.store(static_cast<std::uint8_t>(v), std::memory_order_relaxed);
        s.los_ms.store(TNow(), std::memory_order_release);
        ++s.los_n[static_cast<unsigned>(v) % 9u];
        if (static_cast<std::uint8_t>(v) != s.los_logged && s.los_logs < 60) {
            s.los_logged = static_cast<std::uint8_t>(v), ++s.los_logs;
            const double at = in.cast && in.ray > 0 ? L::Dist(eye, hit) : -1.0;
            Log("[enemy] tgt los id=%05X her %s (%.1f m, bearing %.0f, pitch %.1f, vis %.2f; hit at %.1f of %.1f m)", s.id.load(), R::SightText(v), in.horiz,
                in.bearing_deg, in.pitch_deg, static_cast<double>(in.vis), at, len);
        }
    }
    if (rays >= kSightRays) g_sight_rr = (g_sight_rr + i) & (kSlots - 1);
}
// The SIGHT line (op 7, op 8): the arm's verdicts and what PsrOne did with them, her bucket record's last time into the arm
// (`-` never), the time from the arm's start to her first record (lead) and from her last one to now (tail)
void SightLine(Slot& s) noexcept {
    const std::uint64_t now = TNow(), last = s.last_p.load(), first = s.first_p.load(), a = s.arm_ms;
    char lastb[24] = "-";
    if (last && a && last >= a) std::snprintf(lastb, sizeof(lastb), "%.1f", (last - a) / 1000.0);
    const unsigned lead = first && a && first >= a ? static_cast<unsigned>(first - a) : 0u, tail = last && now >= last ? static_cast<unsigned>(now - last) : 0u;
    const unsigned* n = s.los_n;
    auto at = [&](R::Sight v) { return n[static_cast<unsigned>(v)]; };
    // (M3f review) the last synth's record fields: +0x24 / +0x28 kept (our Sam's), the +0x30 velocity before (Sam) and after (her)
    char recb[128] = "";
    if (s.n_synth.load())
        std::snprintf(recb, sizeof(recb), "; rec +0x24 %.3f +0x28 %.3f vel Sam (%.2f, %.2f) her (%.2f, %.2f)", static_cast<double>(s.syn_f24.load()),
                      static_cast<double>(s.syn_f28.load()), static_cast<double>(s.syn_vs[0].load()), static_cast<double>(s.syn_vs[1].load()),
                      static_cast<double>(s.syn_vh[0].load()), static_cast<double>(s.syn_vh[1].load()));
    Log("[enemy] tgt id=%05X sight: los clear %u blocked %u out %u low_vis %u; keeps %u synth %u skip %u; her bucket last %s s (gap lead %u tail %u ms)%s",
        s.id.load(), at(R::Sight::clear), at(R::Sight::blocked), at(R::Sight::out_range) + at(R::Sight::out_cone) + at(R::Sight::out_pitch),
        at(R::Sight::low_vis), s.n_est_keep.load(), s.n_synth.load(), s.n_synth_skip.load(), lastb, lead, tail, recb);
}

void ArmGeoTick() noexcept;  // op 11 (below)
void TickBody() noexcept {
    if (g_ps.pending) Step2(false);
    if (g_stopped) {
        if (!g_wiped && g_now - g_stop_at >= 2000) g_wiped = true, WipeTable();
        return;
    }
    ArmGeoTick();  // (run 3) op 11's pending arm
    const bool any = g_cfg.stim || PickOrRedirWanted() || g_ps.stim;
    if (!any) return;
    Checks();
    // what the workers match against
    g_l_sam.store(g_local_ok ? g_local : 0, std::memory_order_relaxed);
    const std::uintptr_t pl = g_local && g_local_ok ? PlayerOf(g_local) : 0;
    g_own_flags.store(pl ? U32(pl + kPlInfo) : 0, std::memory_order_relaxed);
    // our own down code (review S3): the stop gates latched, or esync's when it has one
    {
        std::uint64_t f = 0;
        std::uint32_t m = 0;
        const std::uint8_t raw = game::ReadStopGates(&f, &m) ? erules::DownOfState(f) : 0;
        g_our_down = g_down_latch.Step(raw, g_now) != 0 || esync::OurDownNow();
    }
    // SPV1 (while the stimulus key is on, both roles)
    SpvReceive();
    if (g_cfg.stim) SpvSend();
    g_her_flags.store(SpvFresh() ? g_spv.pinfo_flags : 0, std::memory_order_relaxed);
    // M2
    StimTick();
    g_p_agent.store(g_ps.stim && !g_ps.pending ? g_ps.agent : 0, std::memory_order_release);
    // the partner record, after the stimulus tick: valid only while our partner stimulus is live (review S6)
    {
        double p[3] = {};
        const bool ok = g_body && g_ps.stim && !g_ps.pending && !BodyRefusal() && Pos(g_body, p);
        PubPartner(ok, p, HerChestDz());
    }
    if (g_cfg.stim && g_now >= g_status_next) g_status_next = g_now + 10000, StatusLine();
    // M3
    if (PickOrRedirWanted()) {
        const bool loading = TableGate();
        EnsureSense(g_cfg.pick);
        // bit 32's hooks with bit 32 on, or in a hidden run (the aim point record); the redirect itself only with bit 32
        EnsureRedirects(RedirWant() | ShootHooksWant());
        if (!g_sym_in.load()) g_sym_in.store(mem::Q(A(kSymIn)));
        if (!g_sym_out.load()) g_sym_out.store(mem::Q(A(kSymOut)));
        if (!loading && g_now >= g_reg_next) g_reg_next = g_now + 1000, Register(), g_table_ok = true;
        Recompute();
        if (g_now >= g_own_next) g_own_next = g_now + 100, UpdateOwn();
        SightTick(loading);  // (M3f) pick bit 16
        DrainHits();
        if (g_now >= g_place_next) g_place_next = g_now + 250, PlaceTick(), ShootTally(), LogChanges(), LogFirsts();
        const std::uint32_t wf = g_wc.faults.load();
        if (wf != g_wfaults_seen) {
            g_wfaults_seen = wf;
            return StopAll("a detour read fault", true);
        }
        if (g_now >= g_log_next) g_log_next = g_now + 5000, Summary(nullptr);
    } else {
        Recompute();
    }
}
int TickGuarded() noexcept {
    __try {
        TickBody();
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

std::uint32_t IdArg(double v) noexcept { return v > 0 && v < 4294967296.0 ? static_cast<std::uint32_t>(v) & kIdMask : 0u; }
// The arm's geometry (run 2: per arm, l1rules::GeomValid): code 7..9 the melee rule (|dz| <= 1, 2..8 m, bearing <= 45), else
// the gun rule (10..30 m, bearing <= 30, the cone's pitch to her aim point (HerChestDz) <= 15); CS (8) judges our Sam (his chest).
// Run 3: `sep` the angle at the MULE between her and our Sam; `her_valid` her by the arm's rule whatever the arm judges.
struct Geom {
    bool mo = false, ho = false, so = false;
    double m[3] = {}, h[3] = {}, s[3] = {};
    double hdz = 0, hh = -1, hp = 0, hb = -1, sdz = 0, sh = -1, sp = 0, sb = -1, sep = -1;
    bool valid = false, her_valid = false;
    bool her_margin = false;                                  // (run 4) her by l1rules::GeomValidMargin: the placed arms' judge
    double yaw = std::numeric_limits<double>::quiet_NaN();  // (run 4) the MULE's facing (rad); NaN unread
};
bool GeomNow(std::uint32_t id, int code, Geom& g) noexcept {
    g = Geom{};
    if (g_now - g_actors_at > 200) WalkActors();
    const std::uintptr_t e = EntityOf(ActorOf(id));
    g.mo = e && Pos(e, g.m), g.ho = g_body && Pos(g_body, g.h), g.so = g_local && Pos(g_local, g.s);
    if (!g.mo) return false;
    g.hdz = g.ho ? g.h[2] - g.m[2] : 0, g.hh = g.ho ? L::Dist2d(g.h, g.m) : -1;
    g.sdz = g.so ? g.s[2] - g.m[2] : 0, g.sh = g.so ? L::Dist2d(g.s, g.m) : -1;
    // the cone's pitch from the MULE's eye (L::kEye) to the aim point: hers HerChestDz() up, our Sam's chest
    g.hp = g.ho ? L::GeomPitch(g.hdz, HerChestDz(), g.hh) : 0, g.sp = g.so ? L::GeomPitch(g.sdz, L::kChest, g.sh) : 0;
    // (review L7) the bearing off the MULE's facing (its rotation's second row, as the L1 probe reads it); -1 unknown
    WorldTransform t{};
    std::uint8_t xb[0x3C];
    if (mem::Read(e + 0xC8, xb, sizeof(xb))) {
        std::memcpy(&t.rot, xb + 0x18, 0x24);
        const double yaw = std::atan2(static_cast<double>(t.rot.m[1][1]), static_cast<double>(t.rot.m[1][0]));
        if (std::isfinite(yaw)) {
            g.yaw = yaw;
            if (g.ho) g.hb = L::FacingOff(yaw, g.m, g.h);
            if (g.so) g.sb = L::FacingOff(yaw, g.m, g.s);
        }
    }
    g.her_valid = L::GeomValid(code, g.ho, g.hdz, g.hh, g.hb, g.hp);
    g.her_margin = L::GeomValidMargin(code, g.ho, g.hdz, g.hh, g.hb, g.hp);
    g.valid = L::SamArm(code) ? L::GeomValid(code, g.so, g.sdz, g.sh, g.sb, g.sp) : g.her_valid;
    g.sep = L::SepDeg(g.m, g.ho, g.h, g.so, g.s);
    return true;
}
void Geometry(std::uint32_t id, const char* when, int code) noexcept {
    Geom g;
    if (!GeomNow(id, code, g)) return void(Log("[enemy] tgt geometry id=%05X (%s): no realized MULE", id, when));
    Log("[enemy] tgt geometry id=%05X (%s): her dz %.2f m horiz %.1f m pitch %.1f bearing %.0f valid %d; Sam dz %.2f m horiz %.1f m bearing %.0f pitch %.1f; "
        "rule %s (%s); sep %.0f",
        id, when, g.hdz, g.hh, g.hp, g.hb, g.valid ? 1 : 0, g.sdz, g.sh, g.sb, g.sp, L::MeleeArm(code) ? "melee" : "gun", L::SamArm(code) ? "Sam" : "her",
        g.sep);
}
const char* ArmLabel(int code) noexcept {
    switch (code) {
        case 1: return "TA";   // her in the open, Sam far behind
        case 2: return "TB";   // Sam in front too: sticky her
        case 3: return "TF0";  // run 2's repro: engaged on her in front, Sam far in front beyond sight; bit 32 off
        case 4: return "TF1";  // the same with bit 32 on (the shoot actor redirected)
        case 5: return "TE";   // she crouches at 25 m
        case 6: return "TX";   // the control (redirect 0)
        case 7: return "CH";   // melee, her high
        case 8: return "CS";   // melee, Sam high (control)
        case 9: return "CL";   // melee, level ground
        default: return nullptr;
    }
}
// Op 4's body (run 3: op 11 starts the same arm from the tick): the labelled geometry line, the arm's resets, its start line and
// the probe's labelled watch.
bool StartArm(int code, std::uint32_t id, char* msg, std::size_t cap) noexcept {
    const char* label = ArmLabel(code);
    if (!label || !id) return std::snprintf(msg, cap, "enemy_tgt 4 <arm 1..9> <id>"), false;
    Geometry(id, label, code);
    // (review L4) the arm starts from a clean pick: its state, the logged pick and the counters reset. (Round 27 review) Not
    // the pick while her stimulus is already live (TF1 straight after TF0, engaged on her): clearing it would drop every
    // redirect, bit 32's included, until the next PsrOne picks her again (~200 ms of rounds at Sam the test itself made).
    // Only the logged pick and the per-arm counters reset then; LogChanges logs a fresh "-> her" within 250 ms.
    const bool keep_pick = g_ps.stim && !g_ps.pending;
    if (Slot* s = SlotOfId(id)) {
        if (!keep_pick) {
            s->cur.store(R::kNone), s->rule.store(R::rNone), s->chg_rule.store(R::rNone), s->since.store(0), s->seen_p.store(0), s->seen_l.store(0);
            s->dp.store(std::numeric_limits<float>::infinity()), s->dl.store(std::numeric_limits<float>::infinity());
            s->hit_p.store(0), s->hit_l.store(0);
        }
        s->flips.store(0), s->last_p.store(0), s->max_gap.store(0);
        s->sh_calls.store(0), s->sh_redir.store(0), s->sh_pts.store(0), s->pt_ms.store(0);  // bit 32's numbers per arm
        s->pt_actor.store(0), s->pt_place.store(0), s->pt_other.store(0), s->sh_t36.store(0);
        s->logged_cur = 0, s->logged_rule = 0, s->pt_seen_ms = 0, s->pt_her = 0, s->pt_sam = 0, s->pt_none = 0, s->mm_logged = false;
        // (M3f) the line-of-sight numbers per arm (the verdict itself stays: it is current)
        s->n_est_keep.store(0), s->n_synth.store(0), s->n_synth_skip.store(0), s->first_p.store(0);
        s->arm_ms = TNow(), s->los_logged = 0, s->los_logs = 0;
        for (unsigned& c : s->los_n) c = 0;
    }
    g_arm_id = id, g_arm_label = label, g_arm_code = code;
    bool her_spv = false;
    const double her_dz = HerChestDz(&her_spv);
    // (review L3) the visibility the stimulus carries at the start (the gate compares the arms by it)
    Log("[enemy] tgt arm %s id=%05X: start (stim %s, vis %.2f, aim %.2f m %s, pick live %u, redirect sites 0x%X, pick %s)", label, id,
        g_ps.stim && !g_ps.pending ? "made" : "none", static_cast<double>(g_ps.stim ? g_last_vis : VisNow()), her_dz, her_spv ? "SPV1" : "default",
        g_pick_live.load(), g_redir_live.load(), keep_pick ? "kept" : "reset");
    return l1probe::TargetArm(label, id, 120, msg, cap);
}

// ---- op 11 (run 3, hidden runs): the arm at geometry. One pending at a time (a new op 11 or op 8 cancels it); the tick polls it
// every 50 ms: her geometry valid for the arm and her distance in its window (l1rules::ArmHerOk), or 2.5 s, then our Sam placed
// for TF0 / TF1 (170 m, 40 deg off her line) and CS (5 m along its facing), and op 4's start 150 ms later (the ground snap).
// Run 4 (l1rules::ArmPlaces: TB, TF0, TF1, CL): op 11 places her itself. Her not valid (by the margin rule, GeomValidMargin), it
// sends an SCB1 PLACE (ecombat::SendPlace; at most one per kPlaceGapMs) with her point from the MULE's own pose 0.35 s ahead (its
// yaw rate and velocity from the polls) and the candidate's distance and angle (PlaceCandidate: the next one after a ground
// miss); she is judged again once her synced body is within 2 m of the point (150 ms settled), or the PLACE goes again after
// 1.2 s. For TF0 / TF1 her geometry is judged again after our Sam's snap (back to placing her when it fails; Sam placed again
// only when `sep` left 30..50). 6 s, then the arm starts anyway. After the start (TF0, TF1, CL) she is kept in its view (KeepView).
struct ArmGeo {
    bool on = false;
    std::uint32_t id = 0;
    int code = 0;
    double t0 = 0, next = 0, start_at = -1;
    bool judged = false, valid = false;
    double after_ms = 0, horiz = -1, bearing = -1;
    char sam[160] = "not moved";
    // run 4: the placed arms
    bool first = true;              // the first poll not logged yet
    unsigned places = 0, cand = 0;  // PLACEs sent for this arm; the candidate the next one uses
    bool sent = false;              // a PLACE under way: her judged once she arrived and settled, or sent again after kPlaceArriveMs
    double sent_at = -1e18, arrived_at = -1, px = 0, py = 0;
    bool sam_placed = false, recheck = false;
    double sam_at = -1e18;
    // (M3f) TB: placed valid, waiting until the MULE sees her (l1rules::ArmSeen; kArmSeenMs per candidate)
    double seen_wait_at = -1;
    bool want_next = false;  // not seen in time: the next PLACE (the next candidate) before judging her again
} g_ag;
// (run 4) shared by op 11 and the keep-in-view after its start: the MULE's motion, the last PLACE (any arm)
L::MuleMotion g_motion;
std::uint32_t g_motion_id = 0;
double g_place_last = -1e18;
// (run 4) after a placed arm's start (TF0, TF1, CL): her bearing over 20 deg (gun) / 35 deg (melee) or her too far, she is placed
// again in front (candidate 0); it ends at the first of: the slot picks her, the probe's watch notices her, 6 s, op 4 / 8 / 11.
struct KeepView {
    bool on = false;
    std::uint32_t id = 0;
    int code = 0;
    double t0 = 0, next = 0;
    unsigned n = 0;
    bool sent = false;
    double sent_at = -1e18, px = 0, py = 0;
} g_kv;
void ArmGeoCancel(const char* why) noexcept {
    if (!g_ag.on) return;
    const char* label = ArmLabel(g_ag.code);
    Log("[enemy] tgt arm-at-geometry %s id=%05X: cancelled (%s)", label ? label : "?", g_ag.id, why);
    g_ag.on = false;
}
void KeepViewEnd(const char* why) noexcept {
    if (!g_kv.on) return;
    const char* label = ArmLabel(g_kv.code);
    Log("[enemy] tgt arm %s id=%05X: kept in view: %u re-places (%s)", label ? label : "?", g_kv.id, g_kv.n, why);
    g_kv.on = false;
}
void ArmGeoPlace(int mode, double dist, double deg, const char* placed) noexcept {
    char why[160] = {};
    if (esync::PlaceLocalOffLine(g_ag.id, dist, deg, mode, why, sizeof(why))) std::snprintf(g_ag.sam, sizeof(g_ag.sam), "%s", placed);
    else std::snprintf(g_ag.sam, sizeof(g_ag.sam), "not placed (%s)", why);
}
void Motion(std::uint32_t id, const Geom& g) noexcept {
    if (g_motion_id != id) g_motion = L::MuleMotion{}, g_motion_id = id;
    if (g.mo && std::isfinite(g.yaw)) L::MotionStep(g_motion, g_now, g.yaw, g.m[0], g.m[1]);
}
// One PLACE: her point from the MULE's predicted pose and candidate `cand`, and its line. True when the link took it.
bool PlaceHer(std::uint32_t id, int code, const Geom& g, unsigned cand, const char* tag, double* px, double* py, unsigned* cand_used = nullptr) noexcept {
    const char* label = ArmLabel(code);
    g_place_last = g_now;
    if (!g.mo || !std::isfinite(g.yaw)) {
        Log("[enemy] tgt place id=%05X (%s, %s): not sent (the MULE's pose unread)", id, label ? label : "?", tag);
        return false;
    }
    const double lead = L::PlaceLeadDeg(g_motion);
    double lx = 0, ly = 0;
    L::PlaceLeadM(g_motion, &lx, &ly);
    const double mx = g.m[0] + lx, my = g.m[1] + ly, myaw = g.yaw + lead * L::kPi / 180.0;
    // (M3f) every candidate's ground probed (a down ray from 30 m above the MULE's root to 10 m below it, at the point) and the
    // MULE's line of sight to her chest there; l1rules::PlaceChoose picks from `cand` on. The PLACE still carries x / y only (the
    // guest snaps her to its own ground), so a probe off or with no hit falls back to candidate `cand`.
    constexpr int kN = static_cast<int>(L::kPlaceCandidates);
    L::PlaceProbe pr[kN];
    const double aim = HerChestDz();
    const double eye[3] = {g.m[0], g.m[1], g.m[2] + L::kEye};
    // (M3f review) no probe at all after a ray fault (SightFault: g_los_state 3; WorldRay latched it too)
    bool probe_on = g_los_state != 3;
    for (int k = 0; k < kN && probe_on; ++k) {
        const L::PlaceCand ck = L::PlaceCandidate(code, static_cast<unsigned>(k));
        const L::PlacePt q = L::PlacePoint(mx, my, myaw, ck.dist, ck.deg);
        const double top[3] = {q.x, q.y, g.m[2] + 30.0}, bot[3] = {q.x, q.y, g.m[2] - 10.0};
        double hit[3] = {}, len = 0;
        const int r = SightRay(top, bot, 0.0, 0.0, hit, &len);
        if (r < 0 && r != -4) {
            probe_on = false;  // a fault, no world, or the raycast unchecked: no probe at all
            if (r == -1) {
                Log("[enemy] tgt place id=%05X: FAULT in the ground probe (SEH); placed without it", id);
                SightFault();  // (M3f review) bits 16 and 32 off for the run, like SightTick's own fault
            }
            break;
        }
        if (r <= 0 || !std::isfinite(hit[2])) continue;
        pr[k].ok = true, pr[k].dz = hit[2] - g.m[2];
        const double q3[3] = {q.x, q.y, hit[2]};
        pr[k].horiz = L::Dist2d(q3, g.m);
        pr[k].pitch = L::GeomPitch(pr[k].dz, aim, pr[k].horiz);
        const double chest[3] = {q.x, q.y, hit[2] + aim};
        double h2[3] = {}, l2 = 0;
        const int r2 = SightRay(eye, chest, 0.6, kSightHerTrimM, h2, &l2);
        if (r2 == -1) {
            probe_on = false;
            Log("[enemy] tgt place id=%05X: FAULT in the line-of-sight probe (SEH); placed without it", id);
            SightFault();  // (M3f review) bits 16 and 32 off for the run, like SightTick's own fault
            break;
        }
        pr[k].los = r2 == 0;
    }
    int skipped = 0;
    const int chosen = probe_on ? L::PlaceChoose(code, kN, pr, &skipped, static_cast<int>(cand % L::kPlaceCandidates)) : -1;
    const unsigned use = chosen >= 0 ? static_cast<unsigned>(chosen) : cand % L::kPlaceCandidates;
    const L::PlaceCand c = L::PlaceCandidate(code, use);
    const L::PlacePt p = L::PlacePoint(mx, my, myaw, c.dist, c.deg);
    char gz[48];
    if (!probe_on) std::snprintf(gz, sizeof(gz), "- (probe off)");
    else if (!pr[use].ok) std::snprintf(gz, sizeof(gz), "- (probe no hit)");
    else std::snprintf(gz, sizeof(gz), "%.2f (probe hit%s)", pr[use].dz, pr[use].los ? ", los clear" : ", los blocked");
    char why[96] = {};
    std::uint32_t seq = 0;
    if (!ecombat::SendPlace(id, p.x, p.y, p.fx, p.fy, static_cast<std::uint8_t>(code), &seq, why, sizeof(why))) {
        Log("[enemy] tgt place id=%05X (%s, %s): not sent (%s)", id, label ? label : "?", tag, why);
        return false;
    }
    // (M3f) "her before": where she stood when this PLACE was decided (not where it puts her); the ground probe's numbers are the
    // chosen point's
    Log("[enemy] tgt place #%u id=%05X (%s, %s): sent %.1f m %+.0f deg (candidate %u), lead %+.0f deg (rate %+.0f deg/s) %.2f m, to (%.2f, %.2f); "
        "her before horiz %.1f m bearing %.0f dz %.2f pitch %.1f; ground dz %s, chosen %d of %d, skipped %d raised",
        seq, id, label ? label : "?", tag, c.dist, c.deg, use, lead, g_motion.have ? g_motion.rate : 0.0, std::sqrt(lx * lx + ly * ly), p.x, p.y, g.hh,
        g.hb, g.hdz, g.hp, gz, chosen, kN, skipped);
    if (chosen >= 0 && cand_used) *cand_used = static_cast<unsigned>(chosen);
    *px = p.x, *py = p.y;
    return true;
}
void KeepViewTick() noexcept {
    if (!g_kv.on || g_now < g_kv.next) return;
    g_kv.next = g_now + 100;
    const double el = g_now - g_kv.t0;
    char why[80];
    if (Slot* s = SlotOfId(g_kv.id); s && s->cur.load() == R::kPartner) {
        std::snprintf(why, sizeof(why), "it picked her after %.0f ms", el);
        return KeepViewEnd(why);
    }
    if (const double nt = l1probe::TargetArmNoticedMs(); nt >= 0) {
        std::snprintf(why, sizeof(why), "its watch noticed her at %.0f ms", nt);
        return KeepViewEnd(why);
    }
    if (el >= L::kKeepViewMs) return KeepViewEnd("6000 ms, not picked");
    Geom g;
    if (!GeomNow(g_kv.id, g_kv.code, g)) return;
    Motion(g_kv.id, g);
    if (g_kv.sent) {
        const bool arrived = g.ho && L::PlaceArrived(g.h[0], g.h[1], g_kv.px, g_kv.py);
        if (!arrived && g_now - g_kv.sent_at < L::kPlaceArriveMs) return;  // under way
        g_kv.sent = false;
    }
    if (!L::KeepViewOff(g_kv.code, g.hb, g.hh) || g_now - g_place_last < L::kPlaceGapMs) return;
    if (PlaceHer(g_kv.id, g_kv.code, g, 0, "kept in view", &g_kv.px, &g_kv.py)) g_kv.sent = true, g_kv.sent_at = g_now, ++g_kv.n;
}
// The placed arm's poll (run 4): judge her, place her, or start.
void ArmPlaceStep(bool have, const Geom& g, double el) noexcept {
    const char* label = ArmLabel(g_ag.code);
    const bool ok = have && L::ArmHerOk(g_ag.code, g.her_margin, g.hh);
    if (!ok) g_ag.seen_wait_at = -1;  // (M3f review) TB's seen-wait counts only while she stays valid at one point
    if (g_ag.first) {
        g_ag.first = false;
        Log("[enemy] tgt arm-at-geometry %s id=%05X: first poll: her horiz %.1f m bearing %.0f dz %.2f pitch %.1f (%s)", label ? label : "?", g_ag.id,
            have ? g.hh : -1.0, have ? g.hb : -1.0, have ? g.hdz : 0.0, have ? g.hp : 0.0, ok ? "valid" : have ? "not valid: placing her" : "no MULE read");
    }
    bool settled = false;
    if (g_ag.sent) {
        if (g_ag.arrived_at < 0 && have && g.ho && L::PlaceArrived(g.h[0], g.h[1], g_ag.px, g_ag.py)) g_ag.arrived_at = g_now;
        settled = g_ag.arrived_at >= 0 && g_now - g_ag.arrived_at >= L::kPlaceSettleMs;
    }
    const bool judge = !g_ag.sent || settled;
    const bool tf = g_ag.code == 3 || g_ag.code == 4;
    const double limit = L::ArmLimitMs(g_ag.code);
    if (judge && ok && L::ArmWaitsSeen(g_ag.code) && !g_ag.want_next) {
        // (M3f) TB: her placed valid; the arm starts once the MULE sees her (its bucket or the line of sight), else the next candidate
        etarget::SightStat ss;
        const bool have_s = SightStats(g_ag.id, &ss);
        const double ba = have_s && ss.bucket_age_ms != UINT32_MAX ? ss.bucket_age_ms : -1.0;
        const double la = have_s && ss.los_age_ms != UINT32_MAX ? ss.los_age_ms : -1.0;
        const bool clear = have_s && ss.los_v == static_cast<std::uint8_t>(R::Sight::clear);
        if (g_ag.seen_wait_at < 0) g_ag.seen_wait_at = g_now;
        const double w = g_now - g_ag.seen_wait_at;
        if (!L::ArmSeen(ba, clear, la)) {
            if (el < limit && w < L::kArmSeenMs) return;  // waiting
            if (el < limit) {
                Log("[enemy] tgt arm %s: her not seen after %u ms, next candidate", label ? label : "?", static_cast<unsigned>(w));
                g_ag.seen_wait_at = -1, g_ag.want_next = true, ++g_ag.cand;
                return;  // (placed below from the next poll on, kPlaceGapMs after the last PLACE)
            }
            Log("[enemy] tgt arm %s: her not seen after %u ms, at the limit: started anyway", label ? label : "?", static_cast<unsigned>(w));
        } else {
            char lb[24] = "-";
            if (ba >= 0) std::snprintf(lb, sizeof(lb), "%.1f", ba / 1000.0);
            Log("[enemy] tgt arm %s: her seen (bucket %s s, los %s) after %u ms", label ? label : "?", lb,
                have_s && la >= 0 ? R::SightText(static_cast<R::Sight>(ss.los_v)) : "-", static_cast<unsigned>(w));
        }
        g_ag.judged = true, g_ag.valid = true, g_ag.start_at = g_now, g_ag.recheck = false;
        return;
    }
    if (judge && ok && !g_ag.want_next) {
        // TF0 / TF1: our Sam placed (again only when `sep` left 30..50, kPlaceGapMs after his last); the start re-judges her.
        // Never two teleports of our Sam under kPlaceGapMs apart: at the limit inside that gap the arm starts with him where he is.
        const bool sam_again = tf && g_ag.sam_placed && L::SamSepOff(g.sep);
        const bool sam_soon = sam_again && g_now - g_ag.sam_at < L::kPlaceGapMs;
        if (sam_soon && el < limit) return;
        g_ag.judged = true, g_ag.valid = true;
        if (tf && (!g_ag.sam_placed || (sam_again && !sam_soon))) {
            ArmGeoPlace(0, 170.0, 40.0, "placed 170 m 40 deg off her line");
            g_ag.sam_placed = true, g_ag.sam_at = g_now, g_ag.start_at = g_now + 150;
            g_ag.recheck = true;
        } else if (sam_soon) {
            const std::size_t n = std::strlen(g_ag.sam);
            std::snprintf(g_ag.sam + n, sizeof(g_ag.sam) - n, "; not placed again at the limit (sep %.0f deg, %.0f ms after his last)", g.sep,
                          g_now - g_ag.sam_at);
            g_ag.start_at = g_now, g_ag.recheck = false;
        } else {
            g_ag.start_at = g_now, g_ag.recheck = tf;
        }
        return;
    }
    if (el >= limit) {
        g_ag.judged = true, g_ag.valid = false, g_ag.recheck = false;
        if (tf && !g_ag.sam_placed) {
            ArmGeoPlace(0, 170.0, 40.0, "placed 170 m 40 deg off her line");
            g_ag.sam_placed = true, g_ag.sam_at = g_now, g_ag.start_at = g_now + 150;
        } else {
            g_ag.start_at = g_now;
        }
        return;
    }
    // judged not valid, or not arrived in time (an arrived PLACE waits for its settle, never sent again)
    const bool need = judge || (g_ag.arrived_at < 0 && g_now - g_ag.sent_at >= L::kPlaceArriveMs);
    if (!need || g_now - g_place_last < L::kPlaceGapMs) return;
    if (g_ag.sent && settled && L::PlaceGroundMiss(g_ag.code, ok, have ? g.hb : -1.0)) ++g_ag.cand;
    if (PlaceHer(g_ag.id, g_ag.code, g, g_ag.cand, "op 11", &g_ag.px, &g_ag.py, &g_ag.cand))
        g_ag.sent = true, g_ag.sent_at = g_now, g_ag.arrived_at = -1, ++g_ag.places, g_ag.want_next = false, g_ag.seen_wait_at = -1;
}
void ArmGeoTick() noexcept {
    KeepViewTick();
    if (!g_ag.on) return;
    const char* label = ArmLabel(g_ag.code);
    const bool places = L::ArmPlaces(g_ag.code);
    if (g_ag.start_at < 0) {
        if (g_now < g_ag.next) return;
        g_ag.next = g_now + 50;
        const double el = g_now - g_ag.t0;
        Geom g;
        const bool have = GeomNow(g_ag.id, g_ag.code, g);
        if (have) Motion(g_ag.id, g);
        g_ag.horiz = have ? g.hh : -1, g_ag.bearing = have ? g.hb : -1, g_ag.after_ms = el;
        if (L::SamArm(g_ag.code)) {  // CS: her not judged; our Sam 5 m along its facing
            g_ag.judged = false;
            ArmGeoPlace(1, 5.0, 0.0, "placed 5 m along its facing");
            g_ag.start_at = g_now + 150;
            return;
        }
        if (places) return ArmPlaceStep(have, g, el);
        const L::ArmGo go = L::ArmStep(el, have && L::ArmHerOk(g_ag.code, g.her_valid, g.hh));
        if (go == L::ArmGo::wait) return;
        g_ag.judged = true, g_ag.valid = go == L::ArmGo::go_valid;
        if (g_ag.code == 3 || g_ag.code == 4) {
            ArmGeoPlace(0, 170.0, 40.0, "placed 170 m 40 deg off her line");
            g_ag.start_at = g_now + 150;
            return;
        }
        g_ag.start_at = g_now;  // our Sam not moved: at once
    }
    if (g_now < g_ag.start_at) return;
    if (g_ag.recheck) {
        // (run 4) TF0 / TF1 after our Sam's snap: her judged again; not valid, back to placing her (until the limit)
        g_ag.recheck = false;
        Geom g;
        const bool have = GeomNow(g_ag.id, g_ag.code, g);
        if (have) Motion(g_ag.id, g);
        const double el = g_now - g_ag.t0;
        if (!(have && L::ArmHerOk(g_ag.code, g.her_margin, g.hh))) {
            const bool again = el < L::ArmLimitMs(g_ag.code);
            Log("[enemy] tgt arm-at-geometry %s id=%05X: her not valid after our Sam's placement (horiz %.1f m, bearing %.0f, dz %.2f, pitch %.1f); "
                "%s",
                label ? label : "?", g_ag.id, have ? g.hh : -1.0, have ? g.hb : -1.0, have ? g.hdz : 0.0, have ? g.hp : 0.0,
                again ? "placing her again" : "at the limit: started anyway");
            if (again) {
                g_ag.start_at = -1, g_ag.judged = false, g_ag.valid = false, g_ag.sent = false, g_ag.next = g_now;
                return;
            }
            g_ag.valid = false;  // judged stays true: the end line reads "her not valid in 6000 ms ...; started anyway"
        }
        if (have) g_ag.horiz = g.hh, g_ag.bearing = g.hb, g_ag.after_ms = el;
    }
    g_ag.on = false;
    char pl[40] = "";
    if (places) std::snprintf(pl, sizeof(pl), "; %u place%s", g_ag.places, g_ag.places == 1 ? "" : "s");
    if (!g_ag.judged)
        Log("[enemy] tgt arm-at-geometry %s id=%05X: her not judged (a Sam arm; horiz %.1f m, bearing %.0f); Sam %s", label ? label : "?", g_ag.id,
            g_ag.horiz, g_ag.bearing, g_ag.sam);
    else if (g_ag.valid)
        Log("[enemy] tgt arm-at-geometry %s id=%05X: her valid after %u ms (horiz %.1f m, bearing %.0f%s); Sam %s", label ? label : "?", g_ag.id,
            static_cast<unsigned>(g_ag.after_ms), g_ag.horiz, g_ag.bearing, pl, g_ag.sam);
    else
        Log("[enemy] tgt arm-at-geometry %s id=%05X: her not valid in %u ms (horiz %.1f m, bearing %.0f%s); started anyway; Sam %s", label ? label : "?",
            g_ag.id, static_cast<unsigned>(L::ArmLimitMs(g_ag.code)), g_ag.horiz, g_ag.bearing, pl, g_ag.sam);
    char msg[160];
    StartArm(g_ag.code, g_ag.id, msg, sizeof(msg));
    if (places && L::KeepViewArm(g_ag.code)) {
        KeepViewEnd("a new arm");
        g_kv = KeepView{};
        g_kv.on = true, g_kv.id = g_ag.id, g_kv.code = g_ag.code, g_kv.t0 = g_now, g_kv.next = g_now + 100;
    }
}
}  // namespace

void Install(const Config& c) noexcept {
    g_cfg.stim = c.enemy_partner_stim;
    g_cfg.vis = c.enemy_partner_vis;
    g_cfg.radius = c.enemy_partner_stim_radius;
    g_cfg.pick = static_cast<unsigned>(c.enemy_target_pick) & 63u;
    g_cfg.redirect = static_cast<unsigned>(c.enemy_target_redirect) & 0xFFu;
    g_cfg.fresh_ms = c.enemy_target_fresh_ms;
    g_fresh_ms.store(R::FreshMs(c.enemy_target_fresh_ms));
    if (!g_epoch) g_epoch = GetTickCount64();
    g_installed = true;
    if (g_cfg.redirect & kRedShadow) Log("[enemy] tgt: enemy_target_redirect bit 8 (the PlayerInfo shadow) is deferred (not built); ignored");
}

void Tick(const ecombat::TickIn& in, double now) noexcept {
    if (!g_installed || !mem::Base()) return;
    g_now = now;
    g_role = in.role;
    g_local = reinterpret_cast<std::uintptr_t>(in.local);
    g_body = reinterpret_cast<std::uintptr_t>(in.body);
    g_local_ok = in.local_ok;
    g_net = in.net;
    ++g_tick;
    if (g_off.load()) {
        if (g_ps.pending) Step2(false);
        return;
    }
    if (TickGuarded() < 0) {
        for (; g_cs_held && g_cs_depth; --g_cs_depth) SehLeaveCs(g_cs_held);  // (review S8) every level the fault skipped
        g_cs_held = 0, g_cs_depth = 0;
        ++g_faults;
        StopAll("an exception in the tick", true);
    }
}

void ReleaseBeforeFree(const char* why) noexcept {
    if (!g_ps.stim || g_ps.pending) return;
    Release(why ? why : "the partner body going");
}

void Stop(const char* why) noexcept { StopAll(why ? why : "stopped", false); }

void EmergencyStop() noexcept {
    g_off.store(true);
    g_pick_live.store(0), g_redir_live.store(0), g_rec_live.store(false);
    g_p_agent.store(0);
    const std::uintptr_t stim = g_ps_stim.exchange(0, std::memory_order_acquire);
    const std::uintptr_t agent = g_ps_agent.exchange(0), res = g_ps_res.exchange(0), pmgr = g_ps_mgr.exchange(0);
    if (!stim || !agent) return;
    const std::uintptr_t mgr = mem::Q(A(kStimMgr));
    if (mgr && mgr != pmgr) return;
    if (mem::Q(stim + kSAgent) != agent || mem::Q(stim + kSRes) != res) return;
    Step1(stim, agent, res, mgr);  // (no lock: any thread, the tick may be stopped mid-frame)
}

void AfterStop() noexcept {
    static unsigned tries = 0;
    if (tries >= 3 || !g_ps.stim) return;
    if (tries++ == 0 && !g_ps.pending) {
        if (OursParkedP(g_ps.stim)) {
            g_ps.pending = true;
            g_ps.pending_tick = g_tick;
            Log("[enemy] pstim after the emergency stop: parked; removing");
        } else {
            Release("after the emergency stop");
        }
    }
    if (g_ps.pending) Step2(true);
}

bool Command(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!mem::Base()) return std::snprintf(msg, cap, "no game image"), false;
    const int op = static_cast<int>(v[0]);
    if (op != 0 && !automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
    // (review S10) nothing but the status after a stop or a fault: no create, remove or forced state on a stopped module
    if (op != 0 && (g_stopped || g_off.load())) return std::snprintf(msg, cap, "tgt stopped (%s)", g_stop_why ? g_stop_why : "the emergency stop"), false;
    Checks();
    switch (op) {
        case 0: {
            Log("[enemy] tgt status: stim key %d (%s), pick key %u live %u, redirect key %u%s live sites 0x%X (in 0x%X), fresh %d ms, registered %u, SPV1 %s",
                g_cfg.stim, g_ps.stim && !g_ps.pending ? "made" : "none", g_cfg.pick, g_pick_live.load(), g_cfg.redirect,
                g_redir_override >= 0 ? " (overridden)" : "", g_redir_live.load(), g_sites_in, g_cfg.fresh_ms, g_registered, SpvFresh() ? "fresh" : "stale");
            StatusLine();
            Summary("status");
            return std::snprintf(msg, cap, "tgt status logged"), true;
        }
        case 1:
            g_forced_off = v[1] <= 0;
            if (g_forced_off) RemoveNow("forced off (op 1 0)");
            g_retry_at = 0;
            Log("[enemy] tgt stimulus %s", g_forced_off ? "forced off" : "back to the key");
            return std::snprintf(msg, cap, "tgt stimulus %s", g_forced_off ? "off" : "key"), true;
        case 2: {
            if (v[1] < 0) {
                g_vis_forced = false;
                Log("[enemy] tgt vis forced: off (back to SPV1)");
                return std::snprintf(msg, cap, "tgt vis: SPV1"), true;
            }
            g_vis_forced = true;
            g_vis_force = static_cast<float>(v[1] / 100.0);
            l1probe::TargetArmPhase(v[1] <= 0);
            Log("[enemy] tgt vis forced %.2f", static_cast<double>(g_vis_force));
            return std::snprintf(msg, cap, "tgt vis %.2f", static_cast<double>(g_vis_force)), true;
        }
        case 3: {
            const std::uint32_t id = IdArg(v[1]);
            if (!id) return std::snprintf(msg, cap, "enemy_tgt 3 <id> [s]"), false;
            Geometry(id, "watch", 0);
            return l1probe::TargetArm("W", id, v[2] > 0 ? v[2] : 20, msg, cap);
        }
        case 4:
            ArmGeoCancel("op 4");
            KeepViewEnd("op 4");
            return StartArm(static_cast<int>(v[1]), IdArg(v[2]), msg, cap);
        case 5: {
            const std::uint32_t id = IdArg(v[1]);
            Slot* s = id ? SlotOfId(id) : nullptr;
            if (!s) return std::snprintf(msg, cap, "enemy_tgt 5 <registered id> <0 none | 1 her | 2 Sam>"), false;
            const int w = static_cast<int>(v[2]);
            s->force.store(w == 1 ? R::kPartner : w == 2 ? R::kLocal : R::kNone);
            Log("[enemy] tgt id=%05X forced %s", id, w == 1 ? "her" : w == 2 ? "Sam" : "off");
            return std::snprintf(msg, cap, "tgt forced"), true;
        }
        case 6:
            g_redir_override = v[1] < 0 ? -1 : static_cast<int>(v[1]) & 0xFF;
            if (PickOrRedirWanted()) EnsureRedirects(RedirWant() | ShootHooksWant());
            Recompute();
            Log("[enemy] tgt redirect bits for this run: %u%s (live sites 0x%X)", RedirWant(), g_redir_override < 0 ? " (the key)" : "", g_redir_live.load());
            return std::snprintf(msg, cap, "tgt redirect %u", RedirWant()), true;
        case 7: {
            const std::uint32_t id = IdArg(v[1]);
            if (!id) return std::snprintf(msg, cap, "enemy_tgt 7 <id> [arm code]"), false;
            // (run 2) the geometry rule: the code given, else the running arm's when it is this MULE's, else the gun rule
            Geometry(id, "op 7", v[2] > 0 ? static_cast<int>(v[2]) : (g_arm_id == id ? g_arm_code : 0));
            if (Slot* s = SlotOfId(id)) {
                Log("[enemy] tgt id=%05X pick %s; flips her->Sam %u; her bucket gap max %u ms", id, R::WhoText(static_cast<R::Who>(s->cur.load())),
                    s->flips.load(), s->max_gap.load());
                ShootLine(*s);
                SightLine(*s);
            }
            return std::snprintf(msg, cap, "tgt geometry logged"), true;
        }
        case 8:
            ArmGeoCancel("op 8");
            KeepViewEnd("the arm ended (op 8)");
            Summary("arm end");
            if (Slot* s = g_arm_id ? SlotOfId(g_arm_id) : nullptr) {
                Log("[enemy] tgt arm %s id=%05X end: pick %s; flips her->Sam %u; her bucket gap max %u ms", g_arm_label ? g_arm_label : "?", g_arm_id,
                    R::WhoText(static_cast<R::Who>(s->cur.load())), s->flips.load(), s->max_gap.load());
                ShootLine(*s);
                SightLine(*s);
            }
            g_arm_id = 0, g_arm_label = nullptr, g_arm_code = 0;
            return l1probe::TargetArmEnd(msg, cap);
        case 9: {
            const int n = v[1] > 0 && v[1] <= 500 ? static_cast<int>(v[1]) : 50;
            // (review S10) never over a release in progress: its step 2 belongs to the tick
            if (g_ps.pending) return std::snprintf(msg, cap, "pstim cycles: a release is pending; try again"), false;
            if (g_ps.stim) RemoveNow("cycles", true);
            if (g_ps.stim) return std::snprintf(msg, cap, "pstim cycles: the stimulus was released, not removed; try again"), false;
            int made = 0, removed = 0;
            const char* why = nullptr;
            const unsigned f0 = g_faults;
            for (int i = 0; i < n && !g_stim_dead; ++i) {
                why = Make(true, true);
                if (why) break;
                ++made;
                const unsigned r0 = g_removed;
                RemoveNow("cycles", true);
                if (g_removed != r0) ++removed;
                if (g_ps.stim) break;  // released instead (still listed): the tick finishes it
            }
            // (review L1) made again at once (the radius and SPV1 rules apply), so the gate does not wait on a racy tick
            const char* again = nullptr;
            if (!g_ps.stim && !g_stim_dead) again = Make(false, true);
            else again = g_ps.stim ? "a release is pending" : "an earlier create or remove faulted";
            char tail[96];
            if (!again) std::snprintf(tail, sizeof(tail), "made again #%u", g_made);
            else std::snprintf(tail, sizeof(tail), "not made again (%s)", again);
            Log("[enemy] pstim cycles %d: made %d removed %d faults %u%s%s; %s", n, made, removed, g_faults - f0, why ? "; stopped: " : "", why ? why : "", tail);
            return std::snprintf(msg, cap, "pstim cycles %d/%d", made, removed), true;
        }
        case 10: {
            // the crash-reporter guard (seh_guard.hpp): a read under __try of a reserved, never committed page must fault, reach
            // the mod's __except (SQ returns false) and not the game's crash report
            static void* page = nullptr;
            if (!page) page = VirtualAlloc(nullptr, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
            if (!page) return std::snprintf(msg, cap, "seh test: no reserved page"), false;
            const sehguard::Stats s0 = sehguard::Get();
            std::uintptr_t got = 0;
            // once through a noexcept guard (SQ) and once through one that is not (SQx): two handler kinds
            bool read = SQ(reinterpret_cast<std::uintptr_t>(page) + 0x100, &got);
            read = SQx(reinterpret_cast<std::uintptr_t>(page) + 0x200, &got) || read;
            const sehguard::Stats s1 = sehguard::Get();
            const bool ok = !read && s1.absorbed >= s0.absorbed + 2 && s1.reported == s0.reported;
            Log("[seh] test: a guarded read of a reserved page %s; absorbed %llu -> %llu, reported %llu -> %llu (guard %s)",
                read ? "READ (unexpected)" : ok ? "faulted and was caught" : "faulted but was not absorbed twice",
                static_cast<unsigned long long>(s0.absorbed), static_cast<unsigned long long>(s1.absorbed),
                static_cast<unsigned long long>(s0.reported), static_cast<unsigned long long>(s1.reported),
                s1.installed ? "on" : "off");
            return std::snprintf(msg, cap, "seh test %s", ok ? "ok" : "FAILED"), ok;
        }
        case 11: {
            // (run 3) the arm at geometry: returns at once; the tick starts the arm (op 4's lines) when her geometry is valid
            const int code = static_cast<int>(v[2]);
            const std::uint32_t id = IdArg(v[1]);
            if (!ArmLabel(code) || !id) return std::snprintf(msg, cap, "enemy_tgt 11 <id> <arm 1..9>"), false;
            ArmGeoCancel("a new op 11");
            KeepViewEnd("a new op 11");
            g_ag = ArmGeo{};
            g_ag.on = true, g_ag.id = id, g_ag.code = code, g_ag.t0 = g_now, g_ag.next = g_now;
            return std::snprintf(msg, cap, "tgt arm-at-geometry armed"), true;
        }
        default:
            return std::snprintf(msg, cap, "enemy_tgt <op 0..11>"), false;
    }
}

bool StimLive() noexcept { return g_ps.stim && !g_ps.pending; }
double StimAgeMs() noexcept { return g_ps.stim && !g_ps.pending && g_now >= g_ps.t0 ? g_now - g_ps.t0 : -1.0; }
bool ShootStats(std::uint32_t id, ShootStat* out) noexcept {
    Slot* s = out && id ? SlotOfId(id) : nullptr;
    if (!s) return false;
    FillShoot(*s, *out);
    return true;
}
bool SightStats(std::uint32_t id, SightStat* out) noexcept {
    Slot* s = out && id ? SlotOfId(id) : nullptr;
    if (!s) return false;
    const std::uint64_t now = TNow(), last = s->last_p.load(), lm = s->los_ms.load();
    out->bucket_age_ms = last && now >= last ? static_cast<std::uint32_t>(now - last > 0xFFFFFFF0ull ? 0xFFFFFFF0ull : now - last) : UINT32_MAX;
    out->los_v = lm ? s->los_v.load() : 0;
    out->los_age_ms = lm && now >= lm ? static_cast<std::uint32_t>(now - lm > 0xFFFFFFF0ull ? 0xFFFFFFF0ull : now - lm) : UINT32_MAX;
    return true;
}
const char* SightText(std::uint8_t v) noexcept { return R::SightText(static_cast<R::Sight>(v)); }
std::uintptr_t StimAgent() noexcept { return g_ps.stim && !g_ps.pending ? g_ps.agent : 0; }

void NoteHit(std::uint32_t id, bool partner) noexcept {
    if (!g_pick_live.load(std::memory_order_relaxed)) return;
    PushHit(id, partner ? R::kPartner : R::kLocal);
}
// (round 27 run 2) everything read fault-free (ReadProcessMemory: a stale pointer is no exception); the receiver first, so only a hit
// on a human-enemy MULE reads the attacker chain, and each chain object's vtable is checked before its fields (erules::AttackerVerdict).
// enemy_combat has no attacker verdict of its own here (HitBody judges only a guest's frozen mirrors).
void NoteOnHit(std::uintptr_t receiver, std::uintptr_t di) noexcept {
    if (!g_pick_live.load(std::memory_order_relaxed) || g_off.load(std::memory_order_relaxed)) return;
    const std::uintptr_t sam = g_l_sam.load(std::memory_order_relaxed);
    if (!sam || !receiver || !di) return;
    // the receiver: a human enemy (its own id at +0x314 within four parents), a MULE member once H5's list is in
    std::uint8_t ent = 0;
    std::uintptr_t e = mem::Rd(receiver + 8, ent) && ent ? receiver - 0x38 : receiver;
    std::uint32_t id = 0;
    for (int d = 0; e && d <= 4 && !id; ++d) {
        std::uint32_t own = 0;
        if (!mem::InImage(mem::Q(e))) return;
        if (mem::Rd(e + kEOwnId, own) && enemy::IsHumanEnemyId(own & kIdMask)) {
            if (g_h5_ready.load(std::memory_order_acquire) && !H5Has(own)) return;
            id = own;
            break;
        }
        e = mem::Q(e + kEParent);
    }
    if (!id) return;
    // the attacker chain (the DamageInfo's +0x40 node, its +0x68 reference)
    const std::uintptr_t node = mem::Q(di + 0x40), aref = node ? mem::Q(node + 0x68) : 0;
    if (!aref) return;
    const std::uintptr_t att = aref - 0x20;
    R::AttackerView a;
    a.is_sam = att == sam;
    if (!a.is_sam) {
        a.vt_ok = mem::InImage(mem::Q(att));
        if (a.vt_ok) {
            a.owner_sam = mem::Q(att + 0x300) == sam;
            const std::uintptr_t inst = a.owner_sam ? 0 : mem::Q(att + 0x338);
            a.has_inst = inst != 0;
            a.inst_vt_ok = inst && mem::InImage(mem::Q(inst));
            a.inst_owner_sam = a.inst_vt_ok && mem::Q(inst + 0x300) == sam;
        }
    }
    const R::HitBy v = R::AttackerVerdict(a);
    if (v == R::HitBy::bad) {
        // (run 3) the first chain of the run, for the tick's once-line (only what was read above)
        g_first_onhit_bad.Put(id, aref, att, a.is_sam ? 0 : mem::Q(att), node);
        return void(g_wc.onhit_bad.fetch_add(1, std::memory_order_relaxed));
    }
    if (v == R::HitBy::odd) {
        // (M3f) the attacker is the game's, its +0x338 not an object of the game's: information only (no hit pushed)
        const std::uintptr_t inst = mem::Q(att + 0x338);
        g_first_onhit_odd.Put(att, mem::Q(att), inst, inst ? mem::Q(inst) : 0, id);
        return void(g_wc.onhit_odd.fetch_add(1, std::memory_order_relaxed));
    }
    if (v == R::HitBy::sam) PushHit(id, R::kLocal);
}

bool SenseClsRefuse(std::uintptr_t stim, std::uint32_t own) noexcept {
    const unsigned pick = g_pick_live.load(std::memory_order_relaxed);
    if (!(pick & (kPickH5 | kPickH2)) || !PartnerStim(stim)) return false;
    if ((pick & kPickH5) && g_h5_ready.load(std::memory_order_acquire) && (!enemy::IsHumanEnemyId(own & kIdMask) || !H5Has(own))) {
        g_wc.h5.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if ((pick & kPickH2) && R::FlagsReject(g_her_flags.load(std::memory_order_relaxed))) {
        g_wc.h2_refuse.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}
std::uintptr_t SenseClsAfter(std::uintptr_t stim, std::uint32_t /*own*/, std::uintptr_t results, std::uintptr_t r, int* type_out) noexcept {
    if ((r & 0xFF) || !type_out || !(g_pick_live.load(std::memory_order_relaxed) & kPickH2) || !PartnerStim(stim)) return r;
    if (!R::FlagsReject(g_own_flags.load(std::memory_order_relaxed)) || R::FlagsReject(g_her_flags.load(std::memory_order_relaxed))) return r;
    // (review A2) a 1 makes the caller append to the type-1 (PlayerVisual) bucket: only when this result set has one
    std::int32_t nb = 0;
    std::uintptr_t arr = 0;
    bool has1 = false;
    if (!SU32(results, reinterpret_cast<std::uint32_t*>(&nb)) || !SQ(results + 8, &arr)) return r;
    for (std::int32_t b = 0; arr && nb > 0 && nb <= 64 && b < nb && !has1; ++b) {
        std::uintptr_t bp = 0;
        std::uint32_t type = 0;
        has1 = SQ(arr + 8ull * static_cast<unsigned>(b), &bp) && bp && SU32(bp, &type) && type == 1;
    }
    if (!has1) return void(g_wc.h2_nobucket.fetch_add(1, std::memory_order_relaxed)), r;
    const int one = 1;
    if (!SPut(reinterpret_cast<std::uintptr_t>(type_out), &one, sizeof(one))) return r;
    g_wc.h2_fix.fetch_add(1, std::memory_order_relaxed);
    return (r & ~static_cast<std::uintptr_t>(0xFF)) | 1u;
}
void SensePsr(std::uintptr_t k) noexcept {
    if (!(g_pick_live.load(std::memory_order_acquire) & kPickReorder)) return;
    if (Slot* s = Find(k)) {
        g_wc.psr.fetch_add(1, std::memory_order_relaxed);
        PsrOne(s, k);
    }
}

}  // namespace st::etarget
