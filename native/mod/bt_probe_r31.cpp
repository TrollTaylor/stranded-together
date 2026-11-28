// Round 31 stage 0 (docs/enemy-sync.md section 25): bt_probe ops 15..36, the probes the round 31 BT features still need; see
// bt_probe_r31.hpp for the rules. Every address below was read from ds.exe (Director's Cut) and is byte-checked on the first op;
// a failed group refuses only its own ops. Game writes go through WriteProcessMemory (a stale address fails, never raises),
// game calls run under SEH on the game thread, and the detours only count, copy into a lock-free ring and (for the ops that
// say so) write the probe's own fields.
#include "bt_probe_r31.hpp"

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

#include "automation.hpp"
#include "bt_probe_internal.hpp"
#include "bt_rules.hpp"
#include "bt_world.hpp"
#include "game_actions.hpp"
#include "game_api.hpp"
#include "log.hpp"
#include "partner_source.hpp"
#include "ride_together.hpp"

namespace st::bt::r31 {
namespace {

namespace mem = partner::mem;
namespace R = btrules;
namespace I = st::bt::internal;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using uptr = std::uintptr_t;

constexpr uptr kImage = 0x140000000ull;
uptr A(uptr va) noexcept { return mem::Base() + (va - kImage); }
unsigned long long Va(uptr a) noexcept { return a ? static_cast<unsigned long long>(a - mem::Base() + kImage) : 0ull; }
unsigned long long H(uptr a) noexcept { return static_cast<unsigned long long>(a); }

void Put(char* msg, std::size_t cap, const char* fmt, ...) {
    if (!msg || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, cap, fmt, ap);
    va_end(ap);
}

// Guarded writes: WriteProcessMemory on our own process (a stale address fails instead of raising into the game's VEH).
bool WrN(uptr addr, const void* p, std::size_t n) noexcept {
    if (addr < 0x10000) return false;
    SIZE_T got = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(addr), p, n, &got) && got == n;
}
template <class T>
bool Wr(uptr addr, const T& v) noexcept {
    return WrN(addr, &v, sizeof(T));
}
float RdF(uptr addr, float def = -1.0f) noexcept {
    float f = 0;
    return mem::Rd(addr, f) && std::isfinite(f) ? f : def;
}
u32 RdU(uptr addr, u32 def = 0xFFFFFFFFu) noexcept {
    u32 v = 0;
    return mem::Rd(addr, v) ? v : def;
}
int RdI(uptr addr, int def = -1) noexcept {
    std::int32_t v = 0;
    return mem::Rd(addr, v) ? v : def;
}
unsigned RdB(uptr addr, unsigned def = 0xFFu) noexcept {
    u8 v = 0;
    return mem::Rd(addr, v) ? v : def;
}
u32 Fnv(u32 h, const u8* p, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

// ================================================================ game addresses (ds.exe DC; the round 31 maps, verified in the binary)
// ---- weather (ws = [0x147BF83B8], the WeatherSystem; its rows; the job FUN_142E183F0(ws, dt), entered by a jmp at 0x142E045A8)
constexpr uptr kWsGlobal = 0x147BF83B8, kWxJob = 0x142E183F0, kWxPre = 0x142E17CF0;
constexpr uptr kRegionFn = 0x142E0C6B0, kRainClampFn = 0x142E0C3A0, kRainRawFn = 0x142E0C3F0;
constexpr uptr kSpotRainFn = 0x142E14F60, kSpotDryFn = 0x142E14FA0, kSkyFn = 0x142E1FED0, kClimateGlobal = 0x14578C150;
constexpr uptr kWsCell = 0x30, kWsW = 0x38, kWsH = 0x3C, kWsMask = 0x48, kWsSpotN = 0x90, kWsSpots = 0x98, kWsRows = 0x8A0;
constexpr uptr kWsSlot = 0x64A0, kWsFrac = 0x64A4, kWsClock = 0x64A8, kWsLast = 0x64AC, kWsNext = 0x64B0, kWsLen = 0x64B4, kWsHold = 0x64B8;
constexpr uptr kWsGrid = 0x64D0, kWs6508 = 0x6508, kWs650A = 0x650A, kWs650B = 0x650B, kWsInstant = 0x6515, kWsSky = 0x6518;
constexpr uptr kWsJob = 0x6998, kWsRainW = 0x69E0, kWsRegionMap = 0x6A00;
constexpr uptr kRowType = 0x00, kRowWin0 = 0x04, kRowWin1 = 0x08, kRowBase = 0x0C, kRowEff = 0x2C, kRowForced = 0x4C, kRowBits = 0x58;
constexpr uptr kRowSize = 0x5C, kSpotSize = 0x30;
constexpr float kSlotLen = 600.0f;
constexpr u32 kJobQueued = 0x2000BEEFu, kJobDone = 0x7111BEEFu;
uptr RowAt(uptr ws, int slot, int r) noexcept { return ws + kWsRows + (static_cast<uptr>(slot) * 0x40 + static_cast<uptr>(r)) * kRowSize; }
// ---- the gazers (the manager [0x147BEBEC0]; an actor: +0x24 state, +0x68 entity ref, +0xCC reason, +0x180 health, +0x1B4 flags)
constexpr uptr kGazerGlobal = 0x147BEBEC0, kMgrRes = 0x68, kMgr238 = 0x238, kMgr239 = 0x239;
constexpr uptr kActId = 0x08, kActState = 0x24, kActEntRef = 0x68, kActRot = 0x40, kActReason = 0xCC, kActHealth = 0x180, kActPrevHealth = 0x2B0;
constexpr uptr kActFlags = 0x1B4, kActKind = 0x214, kActMode = 0x1A8, kActAction = 0x200, kActBlend = 0x20C, kActDist = 0x204;
constexpr uptr kActVis = 0xB0, kActApproach = 0xB4, kActRainT = 0xBC, kActVisPos = 0x168;
constexpr u32 kFlagArmed = 0x80000u, kFlagNear = 0x1000u;
constexpr uptr kGzActionFn = 0x1429C49D0, kGzMsgFn = 0x14299EFB0, kGzB1Fn = 0x1429B1CD0;
constexpr u32 kActFound = 0x6221E6CBu, kActCatch = 0x5A7E4B1Du, kMsgCaught = 0x682DFB3Bu;
// ---- the hunters (hm = [0x147BECBD0])
constexpr uptr kHunterGlobal = 0x147BECBD0, kHunterTick = 0x1429E1370, kCrowdA = 0x1429E29E0, kCrowdB = 0x1429E5D50;
constexpr uptr kHuntEndFn = 0x1429B9660, kHunterAreaFn = 0x1429B8D60, kInTarFn = 0x1426EC610;
constexpr uptr kHmPull = 0x50, kHmPullList = 0x58, kHmCircN = 0x60, kHmCirc = 0x70, kHmMeshN = 0xC0, kHmMesh = 0xD0, kHmLock = 0x130;
constexpr uptr kHmRes = 0x138, kHmArea = 0x140;
constexpr int kTarCap = 10;
// [Sam+0x54C0] (the locomotion FUN_1428097C0's rdi): +0x848 the per-update tar max (written 0x14280AFCF, folded into +0x844 at
// 0x14280B054..0x14280B0A1, then CLEARED at 0x14280B0C7 on every path: a Tick reads 0 there); +0x844 the smoothed tar value (read
// this one); +0x50 "the +0xD0 list is non-empty" (mov bl,1 at 0x14280AF41 before any inside test; NOT the inside cue).
constexpr uptr kSamParam = 0x54C0, kParTarList = 0x50, kParTarSm = 0x844, kParTar = 0x848;
constexpr float kTarInsideIn = 0.5f, kTarInsideMin = 1.0f;  // the locomotion's inside test: 2D d < max(r - 0.5, 1.0) (0x14280AEC4)
// ---- the voidout (FUN_14299B1B0's own blast sequence) and the post-process effector creator
constexpr uptr kVoidFn = 0x14299B1B0, kBlastInit = 0x14234F3E0, kWorldGlobal = 0x147BC4E70, kRefRelease = 0x14191C810, kRefAdd = 0x14191C790;
constexpr uptr kFxCreate = 0x141CAD420;
// ---- the catchers (cm = [0x147BEE060]) and the craters
constexpr uptr kCatcherGlobal = 0x147BEE060, kCatTick = 0x142A56D30, kCatNotify = 0x142A1C0E0, kCatDrop = 0x142A016C0, kDropList = 0x14298B200;
constexpr uptr kRespawnFn = 0x1429A76D0, kCatCrater = 0x142A003D0, kCatCmdFn = 0x142A928C0, kAreaFn = 0x14262A8C0, kAreaGlobal = 0x147BE6318;
constexpr uptr kGrowFn = 0x142A6A260, kCraterReg = 0x147C09778;
constexpr uptr kCmCount = 0x88, kCmData = 0x90, kCmLock = 0xE8, kCmCraters = 0x280, kCmPending = 0x290, kCmGen = 0x2A8, kCmDirty = 0x2AC;
constexpr uptr kCmTerrN = 0x308, kCmTerr = 0x310;
constexpr std::size_t kCatRecSize = 0x110, kCraterSize = 0x60;
// ---- the stats (FUN_142F21230 / FUN_142F21B00 add 1 to stat 0x79 / 0x7A of [0x147BFD960] + 0x10, kept XOR-masked)
constexpr uptr kStatsGlobal = 0x147BFD960;

// ================================================================ the byte checks (on the first op)
enum Group : unsigned {
    kGWx = 1u, kGHash = 2u, kGSpot = 4u, kGHunt = 8u, kGTar = 16u, kGGz = 32u, kGVoid = 64u, kGFx = 128u, kGCrater = 256u, kGCat = 512u,
    kGStats = 1024u, kGSky = 2048u, kGClimate = 4096u,
};
constexpr unsigned kAllGroups = 8191u;
struct GroupName {
    unsigned g;
    const char* name;
    const char* ops;
};
constexpr GroupName kGroupNames[] = {
    {kGWx, "wx", "15, 16 weather, 18"}, {kGHash, "hash", "17"},        {kGSpot, "spot", "19"},        {kGHunt, "hunt", "16 hunt, 24, 25"},
    {kGTar, "tar", "26"},                {kGGz, "gz", "16 gazer, 20-23"}, {kGVoid, "void", "29, 35"},    {kGFx, "fx", "16 fx, 29 fx 1-3"},
    {kGCrater, "crater", "28, 30"},      {kGCat, "catcher", "16 catcher, 31-33"}, {kGStats, "stats", "36"}, {kGSky, "sky", "34"},
    {kGClimate, "climate", "15's climate byte"},
};
struct Site {
    uptr va;
    const char* sig;
    const char* name;
    unsigned group;
};
struct Rip {
    uptr insn;
    unsigned off, len;
    uptr want;
    const char* what;
    unsigned group;
};
struct Data {  // a constant the code reads (its exact bits)
    uptr va;
    u64 bits;
    unsigned size;
    const char* what;
    unsigned group;
};
// clang-format off
constexpr Site kSites[] = {
    // wx: the job, its jmp, the pre-job, the slot read in the rain at a point, the instant-grid flag, the rows, the region / rain calls
    {kWxJob, "48 8B C4 53 55 56 41 54 41 55 48 81 EC 30 01 00 00 0F B6 99 68 69 00 00 48 8B F1", "the forecast job FUN_142E183F0", kGWx},
    {0x142E045A0, "C5 FA 10 89 90 69 00 00 E9 43 3E 01 00", "the job's jmp (dt = ws+0x6990) at 0x142E045A0", kGWx},
    {kWxPre, "48 89 5C 24 18 55 56 57 48 83 EC 60 48 8B D9 C5 F8 29 74 24 50 48 8B 0D 7C F1 DD 04", "the pre-job FUN_142E17CF0", kGWx},
    {0x142E0C4D6, "4C 63 81 A0 64 00 00", "the rain's slot read +0x64A0 (0x142E0C4D6)", kGWx},
    {0x142E19BA3, "C6 86 15 65 00 00 00", "the instant-grid flag +0x6515 (0x142E19BA3)", kGWx},
    {0x142E1FE79, "66 3B F0 75 29 48 63 8B A0 64 00 00 48 C1 E1 06 48 83 C1 18 40 0F B6 C5 48 03 C8 40 0F B6 C7 48 6B D1 5C 89 04 1A",
     "the weather rows (0x142E1FE79)", kGWx},
    {kRegionFn, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30", "the region at a point FUN_142E0C6B0", kGWx},
    {kRainClampFn, "40 53 48 83 EC 20 48 8B D9 E8 42 00 00 00 C5 F8 28 D0 C5 FA", "the clamped rain FUN_142E0C3A0", kGWx},
    {kRainRawFn, "48 83 EC 48 C5 FB 10 42 10 C5 FB 5C 49 18 C5 FB 10 52 08 C5 EB 5C 41 10", "the raw rain FUN_142E0C3F0", kGWx},
    // hash: the job's noise and its thresholds
    {0x142E186C0, "C5 7A 10 15 B4 12 FF 00 C5 7B 10 1D B8 EC F9 00 C5 7A 10 25 2C F7 DE 00 C5 7B 10 2D D0 EC F9 00 C5 7B 10 35 D8 6B D7 00 "
                  "C5 7B 10 3D D0 EC F9 00", "the hash constants' loads (0x142E186C0)", kGHash},
    {0x142E18744, "C5 78 2F 4B 04 72 10 C5 FA 10 43 08 C4 C1 78 2F C1 0F 83 FE 00 00 00", "the generation window +4 / +8 (0x142E18744)", kGHash},
    {0x142E187F8, "48 63 03 48 8D 0D CE F6 E5 01 48 03 C0 C5 CA 5C D0 C5 FA 10 64 C1 08 C5 DA 58 44 C1 04 C5 FA 10 5C C1 0C C5 FA 58 CB",
     "the type table thresholds (0x142E187F8)", kGHash},
    {0x142E18859, "C5 FA 10 35 2F F7 D6 00", "the 0.5f reload (0x142E18859)", kGHash},
    // spot: the two rain-spot wrappers, their add, the hunter area's call
    {kSpotRainFn, "48 83 EC 48 0F B6 44 24 78 C5 FA 10 05 97 A3 D7 00 88 44 24 38 48 8B 44", "the rain spot FUN_142E14F60 (kind 2)", kGSpot},
    {kSpotDryFn, "48 83 EC 48 0F B6 44 24 78 C5 FA 10 05 E3 2F D7 00 88 44 24 38 48 8B 44", "the dry spot FUN_142E14FA0 (kind 0)", kGSpot},
    {0x142E15DF0, "48 89 5C 24 10 48 89 74 24 18 57 48 81 EC 80 00 00 00 48 8D 99 A8 69 00", "the spot add FUN_142E15DF0 (lock +0x69A8)", kGSpot},
    {0x1429D9B9C, "C5 FA 10 15 BC C4 1F 01", "the hunter area's spot radius (0x1429D9B9C)", kGSpot},
    // hunt: the hunter tick, the crowd updates, the end, the area, the +0x140 reader, the hands aggregate
    {kHunterTick, "4C 8B DC 53 48 81 EC 90 00 00 00 C5 F8 29 74 24 70 48 8B D9 49 89 6B 08 33 ED 49 89 73 10 48 8D B1 70 01 00 00",
     "the hunter tick FUN_1429E1370", kGHunt},
    {kCrowdA, "4C 8B DC 56 48 81 EC 30 05 00 00 48 8B 05 3E E6 57 02 48 33 C4", "the crowd update FUN_1429E29E0", kGHunt},
    {kCrowdB, "4C 8B DC 49 89 5B 10 49 89 73 18 57 41 56 41 57 48 81 EC 20 04 00 00", "the crowd update FUN_1429E5D50", kGHunt},
    {kHuntEndFn, "4C 8B DC 55 53 48 8B EC 48 83 EC 58 48 8B 05 BD 79 5A 02 48 33 C4 48 89 45 F0 48 8B", "the hunt end FUN_1429B9660", kGHunt},
    {kHunterAreaFn, "40 55 53 41 54 41 55 41 56 48 8B EC 48 83 EC 70 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 E8 48 83 B9 38 01 00 00 00",
     "the hunter area FUN_1429B8D60", kGHunt},
    {0x1427AD2F0, "48 89 5C 24 08 57 48 83 EC 20 48 8D 99 28 01 00 00 48 8B F9 48 8B CB FF", "the +0x140 reader FUN_1427AD2F0", kGHunt},
    {0x1429E15A0, "48 8B 0F 8B 01 4C 8D 71 18 01 03", "the hands aggregate hm+0x00 (0x1429E15A0)", kGHunt},
    // tar: the +0xD0 list add, the locomotion's max and cue, the record reader, the in-tar call
    {0x1429BEEB4, "8B 83 C0 00 00 00 83 F8 0A 74 0E 48 89 AC C3 D0 00 00 00 FF 83 C0 00 00", "the tar list add hm+0xC0 / +0xD0 (0x1429BEEB4)", kGTar},
    {0x14280AFC2, "C5 FA 10 87 48 08 00 00 C4 C1 7A 5F CB C5 FA 11", "the tar max write +0x848 (cleared each update; 0x14280AFC2)", kGTar},
    {0x14280AFEF, "3A 5F 50 74 60 88 5F 50 84 DB", "the tar list non-empty byte +0x50 (0x14280AFEF)", kGTar},
    {0x14280AEC4, "C5 7A 10 2D C4 D0 37 01 C5 7A 10 1D C0 D0 37 01", "the tar inside test 0.5 / 1.0 loads (0x14280AEC4)", kGTar},
    {0x14280B054, "C5 FA 10 87 44 08 00 00 C5 FA 10 8F 48 08 00 00", "the tar smoothed +0x844 read (0x14280B054)", kGTar},
    {0x14280B0C7, "44 89 A7 48 08 00 00", "the tar max +0x848 clear (0x14280B0C7)", kGTar},
    {0x1429BE1F0, "40 55 48 83 EC 40 48 89 5C 24 58 48 8D A9 30 01 00 00 48 89 74 24 60 48", "the tar record reader FUN_1429BE1F0", kGTar},
    {kInTarFn, "48 85 C9 0F 84 A0 00 00 00 57 48 83 EC 30 C5 F8 29 74 24 20 48 8B F9 48", "the in-tar call FUN_1426EC610", kGTar},
    // gz: the AI action and message handlers and their cases, the gazer manager's tick, the agitation reads
    {kGzActionFn, "4C 8B DC 56 57 48 81 EC B8 01 00 00 48 8B 05 4D C6 59 02 48 33 C4 48 89", "the AI action FUN_1429C49D0", kGGz},
    {0x1429C4BC4, "3D 1D 4B 7E 5A", "the catch action case 0x5A7E4B1D (0x1429C4BC4)", kGGz},
    {0x1429C4BCF, "3D CB E6 21 62", "the FOUND action case 0x6221E6CB (0x1429C4BCF)", kGGz},
    {kGzMsgFn, "40 57 48 83 EC 60 48 8B 05 73 20 5C 02 48 33 C4 48 89 44 24 50 8B 42 10 48 8B F9", "the actor message FUN_14299EFB0", kGGz},
    {0x14299F207, "3D 3B FB 2D 68 0F 84 BE 00 00 00", "the caught message case 0x682DFB3B (0x14299F207)", kGGz},
    {kGzB1Fn, "40 53 48 83 EC 30 48 8B D9 48 8B 0D 80 C3 23 05 E8 6B 33 06 00 84 C0 75 29", "FUN_1429B1CD0", kGGz},
    {0x1429D66C1, "C5 FA 10 47 7C", "the agitation comp+0x7C (0x1429D66C1)", kGGz},
    {0x1429D66E1, "44 38 AF 80 00 00 00", "the agitation comp+0x80 (0x1429D66E1)", kGGz},
    {0x1429D66EA, "48 8B 97 60 01 00 00", "the graph comp+0x160 (0x1429D66EA)", kGGz},
    {0x1429D66D6, "4C 8B 87 98 01 00 00", "the graph vars comp+0x198 (0x1429D66D6)", kGGz},
    {0x1429D66F6, "48 63 82 90 00 00 00", "the agitation index [+0x160]+0x90 (0x1429D66F6)", kGGz},
    {0x1429D6701, "49 8B 40 58 48 03 C9 C5 F8 2F C7 C5 FA 10 54 C8 20", "the agitation value [[+0x198]+0x58]+idx*0x30+0x20 (0x1429D6701)", kGGz},
    // void: the voidout's resource and blast sequence
    {0x14299B1F6, "48 8B 57 68", "the voidout's resource +0x68 (0x14299B1F6)", kGVoid},
    {0x14299B22D, "C5 FA 10 42 54", "the voidout's far distance +0x54 (0x14299B22D)", kGVoid},
    {0x14299B273, "48 8B 4A 30 EB 04 48 8B 4A 28", "the far / near blast resources +0x30 / +0x28 (0x14299B273)", kGVoid},
    {0x14299B29E, "4C 8B 80 80 00 00 00", "the blast create vt+0x80 (0x14299B29E)", kGVoid},
    {0x14299B2BE, "41 FF D0 33 D2 48 8B C8 4C 8B F0 E8", "the blast create call (0x14299B2BE)", kGVoid},
    {kBlastInit, "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24", "the blast init FUN_14234F3E0", kGVoid},
    {0x14299B2CE, "48 8B 0D 9B 9B 22 05", "the world global load (0x14299B2CE)", kGVoid},
    {0x14299B2E3, "4C 8B 11 41 FF 52 18", "the world add vt+0x18 (0x14299B2E3)", kGVoid},
    {kRefRelease, "40 57 48 83 EC 20 48 8B F9 48 8B 09 48 85 C9 0F 84 8D 00 00 00", "the ref release FUN_14191C810", kGVoid},
    {kRefAdd, "40 57 48 83 EC 20 48 8B F9 48 8B 09 48 85 C9 74 58", "the ref add FUN_14191C790", kGVoid},
    {kVoidFn, "48 8B C4 48 89 78 18 41 54 41 56 41 57 48 81 EC 80 00 00 00 48 83 79 68 00 45 0F B6 F9 45 0F B6 E0 4C 8B F2 48 8B F9 "
              "0F 84 AE 01 00 00", "the voidout FUN_14299B1B0", kGVoid},
    // fx: the effector creator and its only call
    {kFxCreate, "48 89 5C 24 18 55 56 57 48 83 EC 20 48 8B F9 41 0F B6 F0 48 8D 0D A6 19 D5 02 48 8B EA E8 AE 3E D4 FF",
     "the effector creator FUN_141CAD420", kGFx},
    {0x141BB69D0, "E8 4B 6A 0F 00", "the effector creator's only call (0x141BB69D0)", kGFx},
    // crater: the catcher crater build, its lock, its record init, the pending append and grow, the dirty flag, the merge, the registry
    {kCatCrater, "4C 8B DC 55 53 49 8D AB A8 FE FF FF 48 81 EC 48 02 00 00 48 8B 05 46 0C 56 02 48 33 C4 48 89 85",
     "the catcher crater FUN_142A003D0", kGCrater},
    {0x142A57C3F, "E8 8C 87 FA FF", "its only call (0x142A57C3F)", kGCrater},
    {0x142A003F8, "48 8D 99 E8 00 00 00", "its lock cm+0xE8 (0x142A003F8)", kGCrater},
    {0x142A004CB, "41 FF 86 A8 02 00 00", "the generation ++cm+0x2A8 (0x142A004CB)", kGCrater},
    {0x142A004D2, "C5 FA 10 05 66 20 3B 01", "the record radius 107 load (0x142A004D2)", kGCrater},
    {0x142A004ED, "C5 FA 10 0D 43 20 3B 01", "the record delay 8.1 load (0x142A004ED)", kGCrater},
    {kAreaFn, "48 8B 05 21 E8 4B 05 48 85 C0 74 0E 80 B8 A8 00 00 00 00 74 05 0F B7 40", "the area id FUN_14262A8C0", kGCrater},
    {0x142A00870, "49 8D B6 90 02 00 00 8B 3E 48 8B 4E 08 FF C7 3B 7E 04 7E 39", "the pending append cm+0x290 (0x142A00870)", kGCrater},
    {kGrowFn, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 49 63 D8", "the array grow FUN_142A6A260", kGCrater},
    {0x142A008BD, "48 63 16 4C 8B BC 24 30 02 00 00 48 8D 3C 52 48 C1 E7 05", "the 0x60 record slot (0x142A008BD)", kGCrater},
    {0x142A009CA, "41 C6 86 AC 02 00 00 01", "the dirty flag cm+0x2AC (0x142A009CA)", kGCrater},
    {0x142A74D90, "4C 8B DC 55 56 41 55 49 8D 6B A1 48 81 EC A0 00 00 00 48 8B 05 87 C2 4E", "the crater merge FUN_142A74D90", kGCrater},
    {0x142A9298D, "41 C6 82 AD 02 00 00 01", "cm+0x2AD (0x142A9298D)", kGCrater},
    {0x1432E4349, "83 B8 80 00 00 00 FF", "the crater registry +0x80 (0x1432E4349)", kGCrater},
    {0x1432E4359, "C5 FA 10 58 78", "the crater registry +0x78 (0x1432E4359)", kGCrater},
    // catcher: the tick, the notify, the drop roll, the respawn, the command, the record type reads, the territories
    {kCatTick, "40 55 56 41 54 48 8D AC 24 C0 FB FF FF 48 81 EC 40 05 00 00", "the catcher tick FUN_142A56D30", kGCat},
    {kCatNotify, "40 55 53 41 56 41 57 48 8D 6C 24 88 48 81 EC 78 01 00 00 48 8B 05 36 4F 54 02 48 33 C4 48 89 45 48 48 63 81 88 00 00 00",
     "the catcher notify FUN_142A1C0E0", kGCat},
    {0x142A1C16F, "8B 56 24 48 8B 0D", "the notify's record id (0x142A1C16F)", kGCat},
    {0x142A1C18C, "44 0F B6 66 28", "the notify's record type +0x28 (0x142A1C18C)", kGCat},
    {0x142A02036, "44 0F B6 60 58", "the drop's type read (0x142A02036)", kGCat},
    {0x142A02078, "44 88 A4 24 48 01 00 00", "the drop's type store (0x142A02078)", kGCat},
    {kCatDrop, "48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 60 48 8B 05 52 F9", "the drop roll FUN_142A016C0", kGCat},
    {kDropList, "4C 8B DC 49 89 73 20 57 41 56 41 57 48 81 EC A0 02 00 00 C4 41 78 29 4B", "the drop list FUN_14298B200", kGCat},
    {kRespawnFn, "48 83 EC 58 48 8B 05 55 99 5B 02 48 33 C4 48 89 44 24 48 33 D2 48 89 4C 24 38 48 8B", "EnableRespawnGazers FUN_1429A76D0", kGCat},
    {kCatCmdFn, "40 55 48 8D 6C 24 A9 48 81 EC D0 00 00 00 48 8B 05 5B E7 4C 02 48 33 C4 48 89 45 27 4C 8B 15 7D", "the catcher command FUN_142A928C0",
     kGCat},
    {0x142A0B7D2, "48 63 87 08 03 00 00", "the territory count cm+0x308 (0x142A0B7D2)", kGCat},
    {0x142A0B7DB, "48 8B B7 10 03 00 00", "the territory list cm+0x310 (0x142A0B7DB)", kGCat},
    {0x142A0B820, "48 8B 47 70 44 38 70 58", "the territory unit type [+0x70]+0x58 (0x142A0B820)", kGCat},
    {0x142A0B885, "C5 FB 10 57 28 C5 FB 10 47 30", "the territory position +0x28 / +0x30 (0x142A0B885)", kGCat},
    {0x142A0B8A7, "C5 FB 10 4F 20", "the territory position +0x20 (0x142A0B8A7)", kGCat},
    // stats
    {0x142F21230, "44 8B C2 48 83 C1 10 BA 79 00 00 00 41 B9 FF C9 9A 3B E9 29 15 00 00", "the stat 0x79 add FUN_142F21230", kGStats},
    {0x142F21B00, "44 8B C2 48 83 C1 10 BA 7A 00 00 00 41 B9 FF C9 9A 3B E9 59 0C 00 00", "the stat 0x7A add FUN_142F21B00", kGStats},
    {0x142F227AA, "4C 8D B1 F8 04 00 00", "the stat values +0x4F8 (0x142F227AA)", kGStats},
    {0x142F227F9, "8B 94 BE C8 08 00 00 33 94 AE A8 08 00 00", "the stat masks +0x8C8 / +0x8A8 (0x142F227F9)", kGStats},
    // sky
    {kSkyFn, "48 8B 05 E1 84 DD 04 48 85 C0 74 15 84 C9 74 11 83 88 18 65 00 00 01 C7 80 1C 65 00 00 CD CC 4C", "SetStartControlFlags FUN_142E1FED0",
     kGSky},
    // climate
    {0x1429A48B9, "48 8B 9B B0 01 00 00", "the climate +0x1B0 (0x1429A48B9)", kGClimate},
    {0x1429A4904, "80 BB 10 01 00 00 00", "the climate byte +0x110 (0x1429A4904)", kGClimate},
};
constexpr Rip kRips[] = {
    {0x142E1FE34, 3, 7, kWsGlobal, "the weather global 0x147BF83B8", kGWx},
    {0x142E187BF, 4, 8, 0x143DB73B8, "the hash constant 38.0265 (0x142E187BF)", kGHash},
    {0x142E187CB, 4, 8, 0x143DB73A0, "the hash constant 6.18279 (0x142E187CB)", kGHash},
    {0x142E187E3, 4, 8, 0x143DB73E0, "the hash multiplier 43758.5453 (0x142E187E3)", kGHash},
    {0x142E187F8 + 3, 3, 7, 0x144C77ED0, "the forecast type table 0x144C77ED0", kGHash},
    {0x142E18859, 4, 8, 0x143B87F90, "the 0.5f at 0x143B87F90", kGHash},
    {0x1429D9BB8, 1, 5, kSpotRainFn, "the hunter area's call to FUN_142E14F60 (0x1429D9BB8)", kGSpot},
    {0x1429C4EFA, 1, 5, kHunterAreaFn, "the call to FUN_1429B8D60 (FUN_1429C49D0)", kGHunt},
    {0x1429E1679, 1, 5, kInTarFn, "the hunter tick's call to FUN_1426EC610 (0x1429E1679)", kGTar},
    {0x14280AEC4, 4, 8, 0x143B87F90, "the tar inside test's 0.5f 0x143B87F90", kGTar},
    {0x14280AECC, 4, 8, 0x143B87F94, "the tar inside test's 1.0f 0x143B87F94", kGTar},
    {0x14299B2C9, 1, 5, kBlastInit, "the voidout's call to FUN_14234F3E0", kGVoid},
    {0x14299B2CE, 3, 7, kWorldGlobal, "the world global 0x147BC4E70", kGVoid},
    {0x14299B2FB, 1, 5, kRefRelease, "the voidout's call to FUN_14191C810", kGVoid},
    {0x14299B306, 1, 5, kRefAdd, "the voidout's call to FUN_14191C790", kGVoid},
    {0x14299B296, 4, 8, 0x144591D70, "the identity rotation 0x144591D70", kGVoid},
    {0x141BB69D0, 1, 5, kFxCreate, "the call to FUN_141CAD420", kGFx},
    {0x142A57C3F, 1, 5, kCatCrater, "the call to FUN_142A003D0", kGCrater},
    {0x142A004D2, 4, 8, 0x143DB2540, "the record radius 0x143DB2540", kGCrater},
    {0x142A004ED, 4, 8, 0x143DB2538, "the record delay 0x143DB2538", kGCrater},
    {0x142A004E1, 3, 7, kAreaGlobal, "the area global 0x147BE6318", kGCrater},
    {0x142A00507, 1, 5, kAreaFn, "the call to FUN_14262A8C0", kGCrater},
    {0x142A0089F, 1, 5, kGrowFn, "the call to FUN_142A6A260", kGCrater},
    {0x1432E4312, 3, 7, kCraterReg, "the crater registry 0x147C09778", kGCrater},
    {0x142A928DC, 3, 7, kCatcherGlobal, "the catcher manager 0x147BEE060 (FUN_142A928C0)", kGCat},
    {0x142A01A98, 1, 5, kDropList, "the drop roll's call to FUN_14298B200", kGCat},
    {0x142995F35, 3, 7, kStatsGlobal, "the stats global 0x147BFD960", kGStats},
    {kSkyFn, 3, 7, kWsGlobal, "SetStartControlFlags' weather global", kGSky},
    {0x1429A48AA, 3, 7, kClimateGlobal, "the climate global 0x14578C150", kGClimate},
};
constexpr Data kData[] = {
    {0x143E0997C, 0x45631000ull, 4, "3633.0f", kGHash},
    {0x143C07E04, 0x3C000000ull, 4, "0.0078125f", kGHash},
    {0x143B87F90, 0x3F000000ull, 4, "0.5f", kGHash},
    {0x143DB7388, 0x40008A043AC5763Cull, 8, "2.067390879775102", kGHash},
    {0x143DB73B0, 0x4028E6FF9438578Aull, 8, "12.451168662908248", kGHash},
    {0x143B8F2C0, 0x3FE0000000000000ull, 8, "0.5", kGHash},
    {0x143B87F94, 0x3F800000ull, 4, "1.0f (the tar inside test)", kGTar},
    {0x143DB73C0, 0x404AB23FD5AE0396ull, 8, "53.392573080032136", kGHash},
    {0x143DB73B8, 0x40430364C2A3C56Bull, 8, "38.026512460676564", kGHash},
    {0x143DB73A0, 0x4018BB2C055EBF4Eull, 8, "6.182785114200511", kGHash},
    {0x143DB73E0, 0x40E55DD17318FC50ull, 8, "43758.5453", kGHash},
    {0x143DB2540, 0x42D60000ull, 4, "107.0f", kGCrater},
    {0x143DB2538, 0x4101999Aull, 4, "8.1f", kGCrater},
};
// clang-format on
constexpr std::uintptr_t kTypeTable = 0x144C77ED0;  // 14 x {sunny, cloudy, rain, heavy}: compared with btrules::kForecastTable
constexpr std::uintptr_t kIdentity = 0x144591D70;   // 9 floats: the identity rotation the blast is made with

bool g_checked = false;
unsigned g_groups = 0;

unsigned Check() {
    if (g_checked) return g_groups;
    g_checked = true;
    if (!mem::Base()) {
        Log("[bt] r31 probe byte checks: ds.exe base unknown (every round 31 probe refuses)");
        return g_groups = 0;
    }
    unsigned groups = kAllGroups;
    int ok = 0, total = 0;
    auto fail = [&](unsigned g, const char* what) {
        for (const GroupName& n : kGroupNames)
            if (n.g == g) Log("[bt] r31 probe byte check FAILED: %s (%s): ops %s off", n.name, what, n.ops);
        groups &= ~g;
    };
    for (const Site& s : kSites) {
        ++total;
        // stage 0 (two-games): bt_sync's voidout hook (bt_sync >= 1) puts MinHook's jmp on this prologue; I::VoidPrologueOk checks
        // the bytes it moved in its trampoline and the rest in place (a jmp that is not bt_sync's fails it)
        if (s.va == kVoidFn ? I::VoidPrologueOk() : mem::CodeMatches(A(s.va), s.sig)) ++ok;
        else fail(s.group, s.name);
    }
    for (const Rip& r : kRips) {
        ++total;
        if (mem::RipTarget(A(r.insn), r.off, r.len) == A(r.want)) ++ok;
        else fail(r.group, r.what);
    }
    for (const Data& d : kData) {
        ++total;
        u64 v = 0;
        if (mem::Read(A(d.va), &v, d.size) && v == d.bits) ++ok;
        else fail(d.group, d.what);
    }
    ++total;  // the type table, byte for byte as btrules has it (the hash's thresholds)
    {
        R::ForecastProb t[R::kForecastTypeCount];
        if (mem::Read(A(kTypeTable), t, sizeof(t)) && std::memcmp(t, R::kForecastTable, sizeof(t)) == 0) ++ok;
        else fail(kGHash, "the type table 0x144C77ED0 differs from btrules::kForecastTable");
    }
    ++total;
    {
        float m[9];
        const float want[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        if (mem::Read(A(kIdentity), m, sizeof(m)) && std::memcmp(m, want, sizeof(m)) == 0) ++ok;
        else fail(kGVoid, "the identity rotation 0x144591D70");
    }
    ++total;  // the actor's vt+0x10 (FUN_1422F8AA0: it copies a transform into the actor +0x28 / +0x40 only; op 23 logs it)
    if (mem::Q(A(0x143DA5778)) == A(0x1422F8AA0)) ++ok;
    else fail(kGGz, "the gazer actor's vt+0x10 0x143DA5778");
    g_groups = groups;
    char line[512];
    std::size_t at = 0;
    for (const GroupName& n : kGroupNames)
        if (at + 32 < sizeof(line))
            at += static_cast<std::size_t>(std::snprintf(line + at, sizeof(line) - at, "%s%s %s", at ? ", " : "", n.name, (groups & n.g) ? "ok" : "OFF"));
    Log("[bt] r31 probe byte checks: %d/%d ok; %s", ok, total, line);
    return groups;
}

// ================================================================ the detours (pass-through; atomics, guarded reads and the ring only)
std::atomic<bool> g_pass{false};   // EmergencyStop: every detour straight through
std::atomic<bool> g_es{false};     // EmergencyStop seen: the next Tick ends every op
std::atomic<unsigned long> g_tick_tid{0};

enum HookId : int {
    kHJob, kHPre, kHHunter, kHCrowdA, kHCrowdB, kHGzAction, kHCatTick, kHGzB1, kHCatNotify, kHFx,  // op 16's ten
    kHGzMsg, kHHuntEnd, kHDrop, kHDropList, kHRespawn, kHCatCrater, kHInTar, kHCount
};
constexpr unsigned kTids = 8;
struct HookStat {
    std::atomic<unsigned> tick{0}, other{0}, over{0};
    std::atomic<unsigned long> tids[kTids] = {};
};
HookStat g_stat[kHCount];
void Count(int h) noexcept {
    HookStat& s = g_stat[h];
    const unsigned long tid = GetCurrentThreadId();
    (tid == g_tick_tid.load(std::memory_order_relaxed) ? s.tick : s.other).fetch_add(1, std::memory_order_relaxed);
    for (unsigned i = 0; i < kTids; ++i) {
        unsigned long v = s.tids[i].load(std::memory_order_relaxed);
        if (v == tid) return;
        if (v == 0) {
            if (s.tids[i].compare_exchange_strong(v, tid, std::memory_order_relaxed) || v == tid) return;
        }
    }
    s.over.fetch_add(1, std::memory_order_relaxed);
}
bool OnTick() noexcept { return GetCurrentThreadId() == g_tick_tid.load(std::memory_order_relaxed); }

// ---- the event ring (Vyukov bounded MPMC; the game thread's Tick is the only consumer)
enum EvKind : u8 { kEvNone, kEvGzAction, kEvGzMsg, kEvHuntEnd, kEvCatNotify, kEvCatDrop, kEvRespawn, kEvCatCrater, kEvFx };
struct Ev {
    u8 kind = kEvNone, tick = 0, flag = 0;
    u32 a = 0, b = 0, c = 0;
    u64 p = 0, q = 0;
    double pos[3] = {0, 0, 0};
    u8 g[24] = {};
};
constexpr unsigned kRing = 256;
struct Cell {
    std::atomic<unsigned> seq;
    Ev e;
};
Cell g_ring[kRing];
std::atomic<unsigned> g_enq{0}, g_lost{0};
unsigned g_deq = 0;
bool g_ring_init = false;
void RingInit() {
    if (g_ring_init) return;
    for (unsigned i = 0; i < kRing; ++i) g_ring[i].seq.store(i, std::memory_order_relaxed);
    g_ring_init = true;
}
void Push(const Ev& e) noexcept {
    if (!g_ring_init) return;
    unsigned pos = g_enq.load(std::memory_order_relaxed);
    for (;;) {
        Cell& c = g_ring[pos % kRing];
        const unsigned seq = c.seq.load(std::memory_order_acquire);
        const int dif = static_cast<int>(seq - pos);
        if (dif == 0) {
            if (g_enq.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                c.e = e;
                c.seq.store(pos + 1, std::memory_order_release);
                return;
            }
        } else if (dif < 0) {
            g_lost.fetch_add(1, std::memory_order_relaxed);
            return;
        } else {
            pos = g_enq.load(std::memory_order_relaxed);
        }
    }
}
bool Pop(Ev* out) noexcept {
    if (!g_ring_init) return false;
    Cell& c = g_ring[g_deq % kRing];
    const unsigned seq = c.seq.load(std::memory_order_acquire);
    if (static_cast<int>(seq - (g_deq + 1)) != 0) return false;
    *out = c.e;
    c.seq.store(g_deq + kRing, std::memory_order_release);
    ++g_deq;
    return true;
}

// ---- what the detours do beyond counting (set by the ops on the game thread)
std::atomic<bool> g_trace_gz{false}, g_cat_log{false}, g_hunt_watch{false}, g_notify_watch{false};
// op 18: 1 apply (before the job), 2 restore at the next job run; the region, the state, the first apply's instant grid
std::atomic<int> g_wx_mode{0}, g_wx_region{-1}, g_wx_state{0}, g_wx_slot{-1};
std::atomic<bool> g_wx_first{false};
std::atomic<unsigned> g_wx_applies{0}, g_wx_restores{0}, g_wx_skips{0};
// op 16: [[ws+0x6998]+0x28] at FUN_142E17CF0's entry
std::atomic<unsigned> g_job_idle{0}, g_job_queued{0}, g_job_done{0}, g_job_other{0};
// op 22: the mirrored gazer and its drops
std::atomic<uptr> g_mir_actor{0}, g_mir_comp{0};
std::atomic<u32> g_mir_id{0};  // review (crash 2): the mirrored gazer's id (+0x08) when op 22 picked it
std::atomic<int> g_mir_kind{0}, g_mir_variant{0};
std::atomic<bool> g_mir_lost{false};  // MirrorAfter found the mirrored gazer's component stale (g_mir_lost_why says how)
std::atomic<int> g_mir_lost_why{0};   // review (crash 3): MirrorStale's code
std::atomic<uptr> g_mir_ent{0};       // review (crash 3): the entity (+0x68 - 0x20) at the pick; the component lives only with it
std::atomic<unsigned> g_drop_found{0}, g_drop_catch{0}, g_drop_caught{0}, g_mir_writes{0};
// op 26: FUN_1426EC610 (the in-tar call) since the record went in
std::atomic<unsigned> g_intar_calls{0};
// op 29: the effector window (fx 1 log, 2 / 3 the fade), the matched resource and its saved fade
std::atomic<int> g_fx_mode{0};
std::atomic<bool> g_fx_window{false};
std::atomic<uptr> g_fx_res{0}, g_fx_inst{0};
std::atomic<u32> g_fx_start0{0}, g_fx_range0{0};
std::atomic<bool> g_fx_saved{false};
std::atomic<unsigned> g_fx_seen{0};
// op 32: the drop roll's list (thread-local inside FUN_142A016C0)
thread_local bool t_drop = false;
thread_local int t_drop_n = -1;

// the voidout post-process effector's GUID 1d2c4389-a492-de42-85f4-f43997bf6efb, as text bytes and in the mixed-endian order
constexpr u8 kFxGuidA[16] = {0x1d, 0x2c, 0x43, 0x89, 0xa4, 0x92, 0xde, 0x42, 0x85, 0xf4, 0xf4, 0x39, 0x97, 0xbf, 0x6e, 0xfb};
constexpr u8 kFxGuidB[16] = {0x89, 0x43, 0x2c, 0x1d, 0x92, 0xa4, 0x42, 0xde, 0x85, 0xf4, 0xf4, 0x39, 0x97, 0xbf, 0x6e, 0xfb};
// 0 no match, 1..4: text order at +0x10 / +0x18, mixed order at +0x10 / +0x18
int FxGuidMatch(const u8* g24) noexcept {
    if (std::memcmp(g24, kFxGuidA, 16) == 0) return 1;
    if (std::memcmp(g24 + 8, kFxGuidA, 16) == 0) return 2;
    if (std::memcmp(g24, kFxGuidB, 16) == 0) return 3;
    if (std::memcmp(g24 + 8, kFxGuidB, 16) == 0) return 4;
    return 0;
}

using FnF = void (*)(void*, float, u64, u64);
using FnU = u64 (*)(u64, u64, u64, u64);
FnF g_oF[kHCount] = {};
FnU g_oU[kHCount] = {};
bool g_in[kHCount] = {};

// op 18's write (before the job runs): the probe's region gets all 32 base slots = state, its window covers the 32 slots from
// the clock (no shift, no regeneration), its own cancel / force bits (0, 2) off, its forced overrides 0; the first apply asks
// the job for the instant grid (+0x6515).
void WxApplyPre(uptr ws) noexcept {
    std::int32_t slot = -1;
    float clock = 0;
    const int r = g_wx_region.load(std::memory_order_relaxed);
    if (!mem::Rd(ws + kWsSlot, slot) || slot < 0 || slot > 3 || r < 0 || r > 63 || !mem::Rd(ws + kWsClock, clock) || !std::isfinite(clock)) {
        g_wx_skips.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_wx_slot.store(slot, std::memory_order_relaxed);
    const uptr row = RowAt(ws, slot, r);
    u8 base[32];
    std::memset(base, static_cast<u8>(g_wx_state.load(std::memory_order_relaxed)), sizeof(base));
    WrN(row + kRowBase, base, sizeof(base));
    const float win[2] = {clock, clock + 32.0f * kSlotLen};
    WrN(row + kRowWin0, win, sizeof(win));
    const u32 zero3[3] = {0, 0, 0};
    WrN(row + kRowForced, zero3, sizeof(zero3));
    u32 bits = 0;
    if (mem::Rd(row + kRowBits, bits)) Wr<u32>(row + kRowBits, bits & ~5u);
    if (g_wx_first.exchange(false)) Wr<u8>(ws + kWsInstant, 1);
    g_wx_applies.fetch_add(1, std::memory_order_relaxed);
}
void WxApplyPost(uptr ws) noexcept {  // a shift's random roll in this same run (bit 0) undone
    const int slot = g_wx_slot.load(std::memory_order_relaxed), r = g_wx_region.load(std::memory_order_relaxed);
    if (slot < 0 || slot > 3 || r < 0 || r > 63) return;
    u32 bits = 0;
    const uptr row = RowAt(ws, slot, r);
    if (mem::Rd(row + kRowBits, bits) && (bits & 1u)) Wr<u32>(row + kRowBits, bits & ~1u);
}
// the restore: the window closed (+8 = +4) and the regeneration due now (+0x64AC = +0x64B0 = clock - 1): the job regenerates the
// region from its own type (the game's own deterministic forecast)
bool WxRestore(uptr ws) noexcept {
    const int slot = g_wx_slot.load(std::memory_order_relaxed), r = g_wx_region.load(std::memory_order_relaxed);
    float clock = 0, w0 = 0;
    if (slot < 0 || slot > 3 || r < 0 || r > 63 || !mem::Rd(ws + kWsClock, clock)) return false;
    const uptr row = RowAt(ws, slot, r);
    if (!mem::Rd(row + kRowWin0, w0) || !Wr<float>(row + kRowWin1, w0)) return false;
    const float due[2] = {clock - 1.0f, clock - 1.0f};
    WrN(ws + kWsLast, due, sizeof(due));
    g_wx_restores.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void JobDetour(void* self, float dt, u64 r8, u64 r9) {
    if (g_pass.load(std::memory_order_relaxed) || !self) return g_oF[kHJob](self, dt, r8, r9);
    Count(kHJob);
    const uptr ws = reinterpret_cast<uptr>(self);
    const int mode = g_wx_mode.load(std::memory_order_acquire);
    if (mode == 1) WxApplyPre(ws);
    else if (mode == 2 && WxRestore(ws)) g_wx_mode.store(0, std::memory_order_release);
    g_oF[kHJob](self, dt, r8, r9);
    if (mode == 1) WxApplyPost(ws);
}
void PreDetour(void* self, float dt, u64 r8, u64 r9) {
    if (!g_pass.load(std::memory_order_relaxed) && self) {
        Count(kHPre);
        const uptr job = mem::Q(reinterpret_cast<uptr>(self) + kWsJob);
        u32 st = 0;
        if (job && mem::Rd(job + 0x28, st))
            (st == 0 ? g_job_idle : st == kJobQueued ? g_job_queued : st == kJobDone ? g_job_done : g_job_other).fetch_add(1, std::memory_order_relaxed);
        else
            g_job_other.fetch_add(1, std::memory_order_relaxed);
    }
    g_oF[kHPre](self, dt, r8, r9);
}
void CrowdADetour(void* self, float dt, u64 r8, u64 r9) {
    if (!g_pass.load(std::memory_order_relaxed)) Count(kHCrowdA);
    g_oF[kHCrowdA](self, dt, r8, r9);
}
void CrowdBDetour(void* self, float dt, u64 r8, u64 r9) {
    if (!g_pass.load(std::memory_order_relaxed)) Count(kHCrowdB);
    g_oF[kHCrowdB](self, dt, r8, r9);
}
void InTarDetour(void* self, float v, u64 r8, u64 r9) {
    if (!g_pass.load(std::memory_order_relaxed)) Count(kHInTar), g_intar_calls.fetch_add(1, std::memory_order_relaxed);
    g_oF[kHInTar](self, v, r8, r9);
}
u64 HunterDetour(u64 a, u64 b, u64 c, u64 d) {
    if (!g_pass.load(std::memory_order_relaxed)) Count(kHHunter);
    return g_oU[kHHunter](a, b, c, d);
}
u64 CatTickDetour(u64 a, u64 b, u64 c, u64 d) {
    if (!g_pass.load(std::memory_order_relaxed)) Count(kHCatTick);
    return g_oU[kHCatTick](a, b, c, d);
}
u64 GzB1Detour(u64 a, u64 b, u64 c, u64 d) {
    if (!g_pass.load(std::memory_order_relaxed)) Count(kHGzB1);
    return g_oU[kHGzB1](a, b, c, d);
}
// DSGazerComponent MsgDsAiAction FUN_1429C49D0(comp, msg): the action hash at msg+0x10 (+0x14 set: the handler skips it)
u64 GzActionDetour(u64 comp, u64 msg, u64 c, u64 d) {
    if (g_pass.load(std::memory_order_relaxed)) return g_oU[kHGzAction](comp, msg, c, d);
    Count(kHGzAction);
    u32 hash = 0, skip = 0;
    mem::Rd(msg + 0x10, hash), mem::Rd(msg + 0x14, skip);
    const uptr mir = g_mir_comp.load(std::memory_order_relaxed);
    if (mir && comp == mir && (hash == kActFound || hash == kActCatch)) {
        (hash == kActFound ? g_drop_found : g_drop_catch).fetch_add(1, std::memory_order_relaxed);
        return 0;  // dropped (a void handler)
    }
    if (g_trace_gz.load(std::memory_order_relaxed)) {
        Ev e;
        e.kind = kEvGzAction, e.tick = OnTick(), e.a = hash, e.b = skip, e.p = comp;
        const uptr ref = mem::Q(comp + 0x110);
        e.q = ref > 0xE8 ? ref - 0xE8 : 0;  // the actor
        e.c = e.q ? RdU(e.q + kActFlags) : 0xFFFFFFFFu;
        Push(e);
    }
    return g_oU[kHGzAction](comp, msg, c, d);
}
// the gazer actor's message handler FUN_14299EFB0(actor, msg): the message hash at msg+0x10
u64 GzMsgDetour(u64 actor, u64 msg, u64 c, u64 d) {
    if (g_pass.load(std::memory_order_relaxed)) return g_oU[kHGzMsg](actor, msg, c, d);
    Count(kHGzMsg);
    u32 hash = 0;
    mem::Rd(msg + 0x10, hash);
    const uptr mir = g_mir_actor.load(std::memory_order_relaxed);
    u32 mid = 0;
    if (mir && actor == mir && hash == kMsgCaught && mem::Rd(actor + kActId, mid) && mid == g_mir_id.load(std::memory_order_relaxed)) {
        g_drop_caught.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    if (g_trace_gz.load(std::memory_order_relaxed)) {
        Ev e;
        e.kind = kEvGzMsg, e.tick = OnTick(), e.a = hash, e.q = actor;
        Push(e);
    }
    return g_oU[kHGzMsg](actor, msg, c, d);
}
// the hunt end FUN_1429B9660(hm): who ended it (the return address)
u64 HuntEndDetour(u64 a, u64 b, u64 c, u64 d) {
    if (!g_pass.load(std::memory_order_relaxed)) {
        Count(kHHuntEnd);
        if (g_hunt_watch.load(std::memory_order_relaxed)) {
            Ev e;
            e.kind = kEvHuntEnd, e.tick = OnTick(), e.p = reinterpret_cast<uptr>(_ReturnAddress()), e.q = a;
            Push(e);
        }
    }
    return g_oU[kHHuntEnd](a, b, c, d);
}
// the catcher notify FUN_142A1C0E0(cm, id, hash, 0)
u64 CatNotifyDetour(u64 cm, u64 id, u64 hash, u64 d) {
    if (!g_pass.load(std::memory_order_relaxed)) {
        Count(kHCatNotify);
        if (g_cat_log.load(std::memory_order_relaxed) || g_notify_watch.load(std::memory_order_relaxed)) {
            Ev e;
            e.kind = kEvCatNotify, e.tick = OnTick(), e.a = static_cast<u32>(id), e.b = static_cast<u32>(hash), e.p = cm;
            Push(e);
        }
    }
    return g_oU[kHCatNotify](cm, id, hash, d);
}
// the drop roll FUN_142A016C0(cm, pos, param) and its list FUN_14298B200(pos, list {i32 count, ...}) inside it
u64 DropDetour(u64 cm, u64 pos, u64 param, u64 d) {
    if (g_pass.load(std::memory_order_relaxed) || !g_cat_log.load(std::memory_order_relaxed)) return g_oU[kHDrop](cm, pos, param, d);
    Count(kHDrop);
    Ev e;
    e.kind = kEvCatDrop, e.tick = OnTick(), e.p = param;
    e.flag = mem::Read(pos, e.pos, sizeof(e.pos)) ? 1 : 0;
    t_drop = true, t_drop_n = -1;
    const u64 r = g_oU[kHDrop](cm, pos, param, d);
    t_drop = false;
    e.a = static_cast<u32>(t_drop_n);
    Push(e);
    return r;
}
u64 DropListDetour(u64 pos, u64 list, u64 c, u64 d) {
    const u64 r = g_oU[kHDropList](pos, list, c, d);
    if (t_drop && !g_pass.load(std::memory_order_relaxed)) {
        Count(kHDropList);
        std::int32_t n = -1;
        if (mem::Rd(list, n) && n > t_drop_n) t_drop_n = n;
    }
    return r;
}
// EnableRespawnGazers FUN_1429A76D0(scene): the scene's GUID (+0x98) as the field id
u64 RespawnDetour(u64 scene, u64 b, u64 c, u64 d) {
    if (!g_pass.load(std::memory_order_relaxed) && g_cat_log.load(std::memory_order_relaxed)) {
        Count(kHRespawn);
        Ev e;
        e.kind = kEvRespawn, e.tick = OnTick(), e.p = scene;
        u8 guid[16];
        if (mem::Read(scene + 0x98, guid, sizeof(guid))) e.a = R::GuidHash(guid), e.flag = 1;
        Push(e);
    }
    return g_oU[kHRespawn](scene, b, c, d);
}
// the catcher crater FUN_142A003D0(cm, id): the record's position
u64 CatCraterDetour(u64 cm, u64 id, u64 c, u64 d) {
    if (!g_pass.load(std::memory_order_relaxed) && g_cat_log.load(std::memory_order_relaxed)) {
        Count(kHCatCrater);
        Ev e;
        e.kind = kEvCatCrater, e.tick = OnTick(), e.a = static_cast<u32>(id), e.p = cm;
        std::int32_t n = 0;
        const uptr data = mem::Q(cm + kCmData);
        if (data && mem::Rd(cm + kCmCount, n) && n > 0 && n <= 64)
            for (std::int32_t i = 0; i < n; ++i) {
                const uptr rec = data + static_cast<uptr>(i) * kCatRecSize;
                if (RdU(rec + 0x20) == static_cast<u32>(id)) {
                    e.flag = mem::Read(rec + 0x68, e.pos, sizeof(e.pos)) ? 1 : 0;
                    e.b = RdB(rec + 0x28);
                    break;
                }
            }
        Push(e);
    }
    return g_oU[kHCatCrater](cm, id, c, d);
}
// the post-process effector creator FUN_141CAD420(res, world, attach) -> instance
u64 FxDetour(u64 res, u64 world, u64 attach, u64 d) {
    if (g_pass.load(std::memory_order_relaxed)) return g_oU[kHFx](res, world, attach, d);
    Count(kHFx);
    const bool window = g_fx_window.load(std::memory_order_acquire);
    Ev e;
    int match = 0;
    if (window) {
        e.kind = kEvFx, e.tick = OnTick(), e.p = res;
        // stage 0 (run 20261127-205812): +0x10 held the GUID's second half (85f4f43997bf6efb), so the GUID starts at +0x08
        if (mem::Read(res + 0x08, e.g, sizeof(e.g))) match = FxGuidMatch(e.g);
        e.a = static_cast<u32>(match);
        float fade0[2] = {-1.0f, -1.0f};  // FadeStart / FadeRange before any write (the design's 11500 / 11800)
        if (mem::Read(res + 0x28, fade0, sizeof(fade0))) e.pos[0] = fade0[0], e.pos[1] = fade0[1];
        const int fx = g_fx_mode.load(std::memory_order_relaxed);
        if (match && (fx == 2 || fx == 3) && !g_fx_saved.load(std::memory_order_relaxed)) {
            u32 s0 = 0, r0 = 0;
            if (mem::Rd(res + 0x28, s0) && mem::Rd(res + 0x2C, r0)) {
                g_fx_start0.store(s0), g_fx_range0.store(r0), g_fx_res.store(res);
                g_fx_saved.store(true, std::memory_order_release);
                const float fade[2] = {fx == 2 ? 0.0f : 150.0f, fx == 2 ? 1.0f : 450.0f};
                e.b = WrN(res + 0x28, fade, sizeof(fade)) ? 1u : 0u;
            }
        }
    }
    const u64 inst = g_oU[kHFx](res, world, attach, d);
    if (window) {
        e.q = inst;
        if (match && !g_fx_inst.load(std::memory_order_relaxed)) g_fx_inst.store(inst);
        g_fx_seen.fetch_add(1, std::memory_order_relaxed);
        Push(e);
    }
    return inst;
}

struct HookDef {
    uptr va;
    const char* name;
    unsigned group;
    void* detour;
    bool f;  // a (void*, float, ..) detour
};
const HookDef kHooks[kHCount] = {
    {kWxJob, "FUN_142E183F0 (the forecast job)", kGWx, reinterpret_cast<void*>(&JobDetour), true},
    {kWxPre, "FUN_142E17CF0 (the pre-job)", kGWx, reinterpret_cast<void*>(&PreDetour), true},
    {kHunterTick, "FUN_1429E1370 (the hunter tick)", kGHunt, reinterpret_cast<void*>(&HunterDetour), false},
    {kCrowdA, "FUN_1429E29E0 (a crowd update)", kGHunt, reinterpret_cast<void*>(&CrowdADetour), true},
    {kCrowdB, "FUN_1429E5D50 (a crowd update)", kGHunt, reinterpret_cast<void*>(&CrowdBDetour), true},
    {kGzActionFn, "FUN_1429C49D0 (the gazer AI action)", kGGz, reinterpret_cast<void*>(&GzActionDetour), false},
    {kCatTick, "FUN_142A56D30 (the catcher tick)", kGCat, reinterpret_cast<void*>(&CatTickDetour), false},
    {kGzB1Fn, "FUN_1429B1CD0", kGGz, reinterpret_cast<void*>(&GzB1Detour), false},
    {kCatNotify, "FUN_142A1C0E0 (the catcher notify)", kGCat, reinterpret_cast<void*>(&CatNotifyDetour), false},
    {kFxCreate, "FUN_141CAD420 (the effector creator)", kGFx, reinterpret_cast<void*>(&FxDetour), false},
    {kGzMsgFn, "FUN_14299EFB0 (the gazer message)", kGGz, reinterpret_cast<void*>(&GzMsgDetour), false},
    {kHuntEndFn, "FUN_1429B9660 (the hunt end)", kGHunt, reinterpret_cast<void*>(&HuntEndDetour), false},
    {kCatDrop, "FUN_142A016C0 (the drop roll)", kGCat, reinterpret_cast<void*>(&DropDetour), false},
    {kDropList, "FUN_14298B200 (the drop list)", kGCat, reinterpret_cast<void*>(&DropListDetour), false},
    {kRespawnFn, "FUN_1429A76D0 (EnableRespawnGazers)", kGCat, reinterpret_cast<void*>(&RespawnDetour), false},
    {kCatCrater, "FUN_142A003D0 (the catcher crater)", kGCat, reinterpret_cast<void*>(&CatCraterDetour), false},
    {kInTarFn, "FUN_1426EC610 (the in-tar call)", kGTar, reinterpret_cast<void*>(&InTarDetour), true},
};

// One hook in (game thread). False (and the reason in msg) when its group failed or MinHook refused.
bool HookIn(int h, char* msg, std::size_t cap) {
    if (g_in[h]) return true;
    if (g_pass.load()) return Put(msg, cap, "bt probe: stopped (emergency stop)"), false;
    const HookDef& d = kHooks[h];
    if (!(Check() & d.group)) return Put(msg, cap, "bt probe: %s: its byte checks failed (log)", d.name), false;
    if (!I::MhInit(msg, cap)) return false;
    RingInit();
    void* target = reinterpret_cast<void*>(A(d.va));
    void** orig = d.f ? reinterpret_cast<void**>(&g_oF[h]) : reinterpret_cast<void**>(&g_oU[h]);
    MH_STATUS s = MH_CreateHook(target, d.detour, orig);
    if (s == MH_OK) s = MH_EnableHook(target);
    g_in[h] = s == MH_OK;
    Log("[bt] r31 probe hook %s: %s", d.name, g_in[h] ? "in (pass-through)" : MH_StatusToString(s));
    if (!g_in[h]) Put(msg, cap, "bt probe: the hook on %s failed: %s", d.name, MH_StatusToString(s));
    return g_in[h];
}
void ThreadsLine(int h) {
    const HookStat& s = g_stat[h];
    unsigned k = 0;
    for (unsigned i = 0; i < kTids; ++i) k += s.tids[i].load() ? 1u : 0u;
    Log("[bt] threads %s: tick %u, other %u (threads %u%s)%s", kHooks[h].name, s.tick.load(), s.other.load(), k, s.over.load() ? "+" : "",
        g_in[h] ? "" : " (not hooked)");
}

// ================================================================ guarded game calls (game thread; POD only inside __try)
bool SehRain(uptr fn, uptr ws, const double* p, float* out) noexcept {
    __try {
        *out = reinterpret_cast<float (*)(void*, const double*)>(fn)(reinterpret_cast<void*>(ws), p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SehRegion(uptr ws, const double* p, int* out) noexcept {
    __try {
        *out = reinterpret_cast<int (*)(void*, const double*)>(A(kRegionFn))(reinterpret_cast<void*>(ws), p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// FUN_142E14F60 (kind 2, rain) / FUN_142E14FA0 (kind 0, dry) (ws, pos, radius, life, owner, oneshot): both go to FUN_142E15DF0,
// which takes the weather lock +0x69A8 itself and only compares the owner (cmp [spot+0x20], owner)
bool SehSpot(uptr fn, uptr ws, const double* p, float radius, float life, void* owner) noexcept {
    __try {
        reinterpret_cast<void (*)(void*, const double*, float, float, void*, u8)>(fn)(reinterpret_cast<void*>(ws), p, radius, life, owner, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SehU8(uptr fn, u8 v) noexcept {  // FUN_142E1FED0(flag) SetStartControlFlags, FUN_142A928C0(cmd) the catcher script command
    __try {
        reinterpret_cast<void (*)(u8)>(fn)(v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SehHunterArea(uptr hm, const double* p, uptr* out) noexcept {
    __try {
        *out = reinterpret_cast<uptr (*)(void*, const double*, char, u8)>(A(kHunterAreaFn))(reinterpret_cast<void*>(hm), p, 0, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SehVoid(uptr mgr, const double* p, u8 caused) noexcept {  // FUN_14299B1B0(mgr, pos, caused, baggage): the real voidout
    __try {
        reinterpret_cast<void (*)(void*, const double*, u8, u8)>(A(kVoidFn))(reinterpret_cast<void*>(mgr), p, caused, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SehRef(uptr fn, u64* slot) noexcept {  // FUN_14191C790 (add) / FUN_14191C810 (release) on a ref slot
    __try {
        reinterpret_cast<void (*)(u64*)>(fn)(slot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
struct BlastXf {  // what FUN_14299B1B0 builds at rsp+0x40 for the resource's vt+0x80: the position and the identity rotation (0x3C B)
    double pos[3];
    float rot[9];
};
// FUN_14299B1B0's own blast sequence (0x14299B26F..0x14299B306) without its GameOver: the entity from the (far) annihilation
// resource, FUN_14234F3E0(e, 0), the world's vt+0x18(world, e, 0, 0, 0); the ref goes into our own slot (+0x2B0 untouched).
bool SehBlast(uptr er, const BlastXf* xf, uptr world, uptr* ent) noexcept {
    __try {
        const uptr fn = *reinterpret_cast<uptr*>(*reinterpret_cast<uptr*>(er) + 0x80);
        const uptr e = reinterpret_cast<uptr (*)(void*, const BlastXf*)>(fn)(reinterpret_cast<void*>(er), xf);
        *ent = e;
        if (!e) return true;
        reinterpret_cast<void (*)(void*, u8)>(A(kBlastInit))(reinterpret_cast<void*>(e), 0);
        const uptr add = *reinterpret_cast<uptr*>(*reinterpret_cast<uptr*>(world) + 0x18);
        reinterpret_cast<void (*)(void*, void*, u64, u64, u64)>(add)(reinterpret_cast<void*>(world), reinterpret_cast<void*>(e), 0, 0, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The catcher manager's pending crater append, as FUN_142A003D0 does it (0x142A00870..0x142A0095D), under its lock cm+0xE8.
// 0 ok, 1 faulted, 2 the grow failed. *gen / *index: the record's.
struct CraterIn {
    double pos[3];
    float radius, yaw;
    u32 flags;
};
int SehCraterAppend(uptr cm, const CraterIn* in, int* gen_out, int* index_out) noexcept {
    int rc = 1;
    __try {
        CRITICAL_SECTION* cs = reinterpret_cast<CRITICAL_SECTION*>(cm + kCmLock);
        EnterCriticalSection(cs);
        __try {
            std::int32_t* gen = reinterpret_cast<std::int32_t*>(cm + kCmGen);
            std::int32_t* n = reinterpret_cast<std::int32_t*>(cm + kCmPending);
            std::int32_t* cap = reinterpret_cast<std::int32_t*>(cm + kCmPending + 4);
            uptr* data = reinterpret_cast<uptr*>(cm + kCmPending + 8);
            const std::int32_t g = ++*gen;
            if (*n + 1 > *cap) {
                const std::int32_t want = *n + 1, newcap = want + want / 8 + 2;
                const uptr nd = reinterpret_cast<uptr (*)(void*, uptr, std::int32_t)>(A(kGrowFn))(reinterpret_cast<void*>(n), *data, newcap);
                if (!nd) {
                    rc = 2;
                    __leave;
                }
                *data = nd;
                if (*cap < want) {
                    rc = 2;
                    __leave;
                }
            }
            u8* rec = reinterpret_cast<u8*>(*data + static_cast<uptr>(*n) * kCraterSize);
            std::memset(rec, 0, kCraterSize);
            std::memcpy(rec + 0x18, in->pos, sizeof(in->pos));
            *reinterpret_cast<float*>(rec + 0x30) = in->radius;
            *reinterpret_cast<float*>(rec + 0x44) = 8.1f;
            *reinterpret_cast<std::int32_t*>(rec + 0x48) = g;
            const uptr area_obj = *reinterpret_cast<uptr*>(A(kAreaGlobal));
            const u32 area = reinterpret_cast<u32 (*)(void*)>(A(kAreaFn))(reinterpret_cast<void*>(area_obj));
            *reinterpret_cast<u32*>(rec + 0x50) = area & 0xFFFFu;
            *reinterpret_cast<float*>(rec + 0x54) = in->yaw;
            *reinterpret_cast<u32*>(rec + 0x58) = in->flags;
            *index_out = *n;
            ++*n;
            *reinterpret_cast<u8*>(cm + kCmDirty) = 1;
            *gen_out = g;
            rc = 0;
        } __finally {
            LeaveCriticalSection(cs);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rc = 1;
    }
    return rc;
}
// Op 26's record into (in = true) or out of the hunter manager's list (count at +cnt, ptr[10] at +arr), under hm+0x130.
// 1 done, 0 not (full / not found), -1 faulted.
int SehTarList(uptr hm, uptr cnt, uptr arr, uptr rec, bool in, int* count_out) noexcept {
    int rc = -1;
    __try {
        SRWLOCK* lock = reinterpret_cast<SRWLOCK*>(hm + kHmLock);
        AcquireSRWLockExclusive(lock);
        __try {
            std::int32_t* n = reinterpret_cast<std::int32_t*>(hm + cnt);
            uptr* a = reinterpret_cast<uptr*>(hm + arr);
            rc = 0;
            if (in) {
                if (*n >= 0 && *n < kTarCap) a[(*n)++] = rec, rc = 1;
            } else {
                for (std::int32_t i = 0; i < *n && i < kTarCap; ++i)
                    if (a[i] == rec) {
                        for (std::int32_t j = i; j + 1 < *n && j + 1 < kTarCap; ++j) a[j] = a[j + 1];
                        a[--*n] = 0;
                        rc = 1;
                        break;
                    }
            }
            *count_out = *n;
        } __finally {
            ReleaseSRWLockExclusive(lock);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rc = -1;
    }
    return rc;
}

// ================================================================ shared readers
uptr Ws() noexcept { return mem::Q(A(kWsGlobal)); }
uptr GzMgr() noexcept { return mem::Q(A(kGazerGlobal)); }
uptr Hm() noexcept { return mem::Q(A(kHunterGlobal)); }
uptr Cm() noexcept { return mem::Q(A(kCatcherGlobal)); }
uptr SamEnt() noexcept {
    game::Entity* e = nullptr;
    return game::TryGetLocalPlayer(&e) ? reinterpret_cast<uptr>(e) : 0;
}
// review (semantics 1): the smoothed tar value +0x844 (the +0x848 max is cleared at the end of every locomotion update).
float SamTar(float* raw848 = nullptr) noexcept {
    const uptr s = SamEnt();
    const uptr p = s ? mem::Q(s + kSamParam) : 0;
    if (raw848) *raw848 = p ? RdF(p + kParTar) : -1.0f;
    return p ? RdF(p + kParTarSm) : -1.0f;
}
float Health() noexcept {
    game::Diagnostics d;
    game::ReadDiagnostics(&d);
    return d.health_valid ? d.health : -1.0f;
}
u64 GameState() noexcept {
    u64 st = 0;
    u32 load = 0;
    return game::ReadStopGates(&st, &load) ? st : ~0ull;
}
bool GameOverNow() noexcept {
    const u64 st = GameState();
    return st != ~0ull && (st & (1ull << 13));
}
const char* YesNo(bool b) noexcept { return b ? "yes" : "no"; }
void FmtPos(char* b, std::size_t n, const double* p) { std::snprintf(b, n, "%.1f %.1f %.1f", p[0], p[1], p[2]); }
float RainRawAt(uptr ws, const double* p, bool* ok) noexcept {
    float r = 0;
    *ok = ws && SehRain(A(kRainRawFn), ws, p, &r);
    return r;
}
// the stats FUN_142F21230 / FUN_142F21B00 add to: [0x147BFD960] + 0x10, kept XOR-masked per id
bool StatRead(u32 id, u32* out) noexcept {
    const uptr g = mem::Q(A(kStatsGlobal));
    if (!g) return false;
    const uptr t = g + 0x10;
    u32 raw = 0, k1 = 0, k2 = 0;
    if (!mem::Rd(t + 0x4F8 + id * 4, raw) || !mem::Rd(t + 0x8C8 + ((id >> 3) & 7) * 4, k1) || !mem::Rd(t + 0x8A8 + (id & 7) * 4, k2)) return false;
    *out = raw ^ k1 ^ k2;
    return true;
}
double YawDeg(float fx, float fy) noexcept { return std::atan2(static_cast<double>(fy), static_cast<double>(fx)) * 57.29577951308232; }
u32 g_rng = 0;
float Rand01() noexcept {
    if (!g_rng) g_rng = static_cast<u32>(GetTickCount64()) | 1u;
    g_rng ^= g_rng << 13, g_rng ^= g_rng >> 17, g_rng ^= g_rng << 5;
    return static_cast<float>(g_rng >> 8) / 16777216.0f;
}

// The gazers: the census (bt_sync) and a pick by locator (LocArg; 0 0 = the nearest that fits)
constexpr int kGzCap = 160;
I::GzInfo g_gz[kGzCap];
int g_gz_n = 0;
double g_gz_ms = -1e18;
int Census(double now) noexcept {
    if (now - g_gz_ms < 250.0) return g_gz_n;
    g_gz_ms = now;
    return g_gz_n = I::Gazers(g_gz, kGzCap);
}
u32 LocOf(uptr actor) noexcept {
    for (int i = 0; i < g_gz_n; ++i)
        if (g_gz[i].actor == actor) return g_gz[i].loc;
    return 0;
}
double GzDist(const I::GzInfo& z, const double* me) noexcept { return z.pos_ok ? R::Dist3(z.pos, me) : z.pdist; }
uptr GzEntity(uptr actor) noexcept;
uptr GzComp(uptr actor) noexcept;
// stage 0 (run 20261127-205501, op 22 "gazer 40AB1A93 has no DSGazerComponent"): the nearest gazer may have no entity (+0x68) or
// none with the component; kPickEntity (23) and kPickFar40Comp (22) skip those and say how many they skipped.
enum PickRule { kPickAny, kPickFar40, kPickKillable, kPickEntity, kPickFar40Comp };
void PickBareLog(const I::GzInfo& z, double d) {
    const uptr ref = mem::Q(z.actor + kActEntRef), ent = GzEntity(z.actor);
    Log("[bt] gz pick: the nearest skipped gazer loc %08X (%.0f m, state %d, flags 0x%X): actor +0x68 0x%llX, entity 0x%llX, DSGazerComponent %s", z.loc, d,
        z.state, z.flags, H(ref), H(ent), ent ? "none" : "n/a");
    if (ent) partner::DescribeEntity(reinterpret_cast<game::Entity*>(ent), "[bt] gz pick: that gazer's entity");
}
const I::GzInfo* PickGazer(double hi, double lo, PickRule rule, char* why, std::size_t cap) {
    u32 loc = 0;
    if (!R::LocArg(hi, lo, &loc)) return Put(why, cap, "not a locator (hi lo: two 16-bit halves, 0 0 the nearest)"), nullptr;
    g_gz_ms = -1e18;
    const int n = Census(I::NowMs());
    if (n < 0) return Put(why, cap, "the gazer list failed bt_sync's byte checks"), nullptr;
    double me[3];
    if (!I::LocalPos(me)) return Put(why, cap, "no local player"), nullptr;
    const I::GzInfo* best = nullptr;
    const I::GzInfo* bare = nullptr;
    double bd = 1e18, bare_d = 1e18;
    int no_ent = 0, no_comp = 0;
    for (int i = 0; i < n; ++i) {
        const I::GzInfo& z = g_gz[i];
        if (loc && z.loc != loc) continue;
        if ((rule == kPickFar40 || rule == kPickFar40Comp) && !(RdF(z.actor + kActDist) > 40.0f)) continue;
        if ((rule == kPickEntity && !GzEntity(z.actor)) || (rule == kPickFar40Comp && !GzComp(z.actor))) {
            ++(GzEntity(z.actor) ? no_comp : no_ent);
            const double d = GzDist(z, me);
            if (!bare || d < bare_d) bare = &z, bare_d = d;
            continue;
        }
        if (rule == kPickKillable &&
            R::KillGuard(z.state, RdF(z.actor + kActHealth, 0), RdF(z.actor + kActPrevHealth, 0), RdU(z.actor + kActFlags, 0)) != R::kKillOk)
            continue;
        const double d = GzDist(z, me);
        if (!best || d < bd) best = &z, bd = d;
    }
    if (bare) PickBareLog(*bare, bare_d);
    if (!best && bare)
        Put(why, cap, "no gazer%s fits: %d without an entity (+0x68), %d whose entity has no DSGazerComponent (log)", rule == kPickFar40Comp ? " beyond 40 m" : "",
            no_ent, no_comp);
    else if (!best) Put(why, cap, loc ? "gazer %08X not streamed here (or it does not fit)" : "no gazer here fits", loc);
    return best;
}
uptr GzEntity(uptr actor) noexcept {
    const uptr ref = mem::Q(actor + kActEntRef);
    return ref > 0x20 ? ref - 0x20 : 0;
}
uptr GzComp(uptr actor) noexcept {
    const uptr e = GzEntity(actor);
    return e ? partner::FindComponentByName(reinterpret_cast<game::Entity*>(e), "DSGazerComponent") : 0;
}

// ================================================================ op 15 wx_dump (read only)
u8 g_buf[128 * 128 * 4];
void DumpWx(const char* tag) {
    const uptr ws = Ws();
    if (!ws) return Log("[bt] wx2%s: no weather system (no world?)", tag);
    std::int32_t slot = RdI(ws + kWsSlot), w = RdI(ws + kWsW), h = RdI(ws + kWsH);
    u64 mask = 0;
    mem::Rd(ws + kWsMask, mask);
    u32 gh = 0, rh = 0;
    if (w > 0 && h > 0 && w <= 128 && h <= 128) {
        const std::size_t cells = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
        const uptr grid = mem::Q(ws + kWsGrid), map = mem::Q(ws + kWsRegionMap);
        if (grid && mem::Read(grid, g_buf, cells * 4)) gh = Fnv(2166136261u, g_buf, cells * 4);
        if (map && mem::Read(map, g_buf, cells)) rh = Fnv(2166136261u, g_buf, cells);
    }
    const int spots = RdI(ws + kWsSpotN);
    Log("[bt] wx2%s: slot %d mask %016llX clock %.1f last %.1f next %.1f hold %.1f frac %.2f 6508 %u 650A %u 650B %u 6515 %u 6518 0x%X rainw %.3f "
        "spots %d gridhash %08X (%d x %d) regionmap %08X",
        tag, slot, static_cast<unsigned long long>(mask), RdF(ws + kWsClock), RdF(ws + kWsLast), RdF(ws + kWsNext), RdF(ws + kWsHold),
        RdF(ws + kWsFrac), RdB(ws + kWs6508), RdB(ws + kWs650A), RdB(ws + kWs650B), RdB(ws + kWsInstant), RdU(ws + kWsSky), RdF(ws + kWsRainW),
        spots, gh, w, h, rh);
    if (slot >= 0 && slot <= 3)
        for (int r = 0; r < 64; ++r) {
            if (!((mask >> r) & 1ull)) continue;
            const uptr row = RowAt(ws, slot, r);
            u8 rb[kRowSize];
            if (!mem::Read(row, rb, sizeof(rb))) continue;
            u32 type = 0, f0 = 0, f1 = 0, f2 = 0, bits = 0;
            float w0 = 0, w1 = 0;
            std::memcpy(&type, rb + kRowType, 4), std::memcpy(&w0, rb + kRowWin0, 4), std::memcpy(&w1, rb + kRowWin1, 4);
            std::memcpy(&f0, rb + kRowForced, 4), std::memcpy(&f1, rb + kRowForced + 4, 4), std::memcpy(&f2, rb + kRowForced + 8, 4);
            std::memcpy(&bits, rb + kRowBits, 4);
            char base[9], eff[9];
            for (int i = 0; i < 8; ++i) base[i] = static_cast<char>('0' + (rb[kRowBase + i] % 10)), eff[i] = static_cast<char>('0' + (rb[kRowEff + i] % 10));
            base[8] = eff[8] = 0;
            Log("[bt] wx2%s %d: type %u win %.1f..%.1f base %s eff %s forced %08X/%08X/%08X bits %X", tag, r, type, w0, w1, base, eff, f0, f1, f2, bits);
        }
    const uptr sp = mem::Q(ws + kWsSpots);
    for (int i = 0; sp && i < spots && i < 32; ++i) {
        u8 s[kSpotSize];
        if (!mem::Read(sp + static_cast<uptr>(i) * kSpotSize, s, sizeof(s))) break;
        float cx = 0, cy = 0, rad = 0, f14 = 0, life = 0;
        u64 owner = 0;
        std::memcpy(&cx, s, 4), std::memcpy(&cy, s + 4, 4), std::memcpy(&rad, s + 0x10, 4), std::memcpy(&f14, s + 0x14, 4);
        std::memcpy(&life, s + 0x18, 4), std::memcpy(&owner, s + 0x20, 8);
        Log("[bt] wx2%s spot %d: kind %u r %.2f life %.1f (+0x14 %.2f) owner 0x%llX at (cell %.2f %.2f)", tag, i, s[0x28], rad, life, f14,
            static_cast<unsigned long long>(owner), cx, cy);
    }
    double me[3];
    if (I::LocalPos(me)) {
        int region = -1;
        float raw = 0, cl = 0;
        const bool rok = SehRegion(ws, me, &region), aok = SehRain(A(kRainRawFn), ws, me, &raw), cok = SehRain(A(kRainClampFn), ws, me, &cl);
        const uptr mgr = GzMgr();
        unsigned climate = 0xFFu;
        if (Check() & kGClimate) {
            const uptr c0 = mem::Q(A(kClimateGlobal));
            const uptr c1 = c0 ? mem::Q(c0 + 0x1B0) : 0;
            if (c1) climate = RdB(c1 + 0x110);
        }
        const uptr ro = mem::Q(A(0x144C73850));
        Log("[bt] wx2%s here: region %d raw %.3f clamped %.3f mgr238 %u 239 %u climate %u%s (rain object %s)", tag, rok ? region : -1, aok ? raw : 99.0f,
            cok ? cl : 99.0f, mgr ? RdB(mgr + kMgr238) : 0xFFu, mgr ? RdB(mgr + kMgr239) : 0xFFu, climate, climate == 0xFFu ? " (none)" : "",
            ro && mem::Q(ro) == ws ? "= ws" : "differs");
    }
}
struct Wx15 {
    bool on = false;
    double end_ms = 0, next_ms = 0;
} g_o15;

// ================================================================ op 16 hooks_threads
struct Th16 {
    bool in = false, armed = false;
    double read_ms = 0;
} g_o16;
void ThreadsRead() {
    for (int h = kHJob; h <= kHFx; ++h) ThreadsLine(h);
    for (int h = kHGzMsg; h < kHCount; ++h)
        if (g_in[h]) ThreadsLine(h);
    Log("[bt] wx job state at 17CF0: idle %u, queued %u, done %u, other %u", g_job_idle.load(), g_job_queued.load(), g_job_done.load(), g_job_other.load());
}

// ================================================================ op 17 wx_hash (read only)
bool OpHash(char* msg, std::size_t cap) {
    const uptr ws = Ws();
    const std::int32_t slot = ws ? RdI(ws + kWsSlot) : -1;
    if (slot < 0 || slot > 3) return Put(msg, cap, "bt probe wx_hash: no weather system (no world?)"), false;
    u64 mask = 0;
    mem::Rd(ws + kWsMask, mask);
    int regions = 0, slots = 0, exact = 0, prev = 0, bad = 0, logged = 0, shifted = 0, golden = 0, golden_r = -1;
    for (int r = 0; r < 64; ++r) {
        if (!((mask >> r) & 1ull)) continue;
        const uptr row = RowAt(ws, slot, r);
        std::int32_t type = -1;
        float w0 = 0, w1 = 0;
        u8 base[32];
        if (!mem::Rd(row + kRowType, type) || !mem::Rd(row + kRowWin0, w0) || !mem::Rd(row + kRowWin1, w1) || !mem::Read(row + kRowBase, base, 32)) continue;
        if (type < 0 || type >= R::kForecastTypeCount || !(w1 > w0)) continue;  // no table row / a collapsed window (nothing generated)
        ++regions;
        // review (rules 1): replay the job's own steps from the row's clock (+0x04): T += len in float while T < clock + 32 len, each
        // byte at (int)((T - clock) / len), the later write winning. Only the indices written up to the row's last T (+0x08) count.
        int step_of[R::kWxSlots];
        float t_of[R::kWxSlots];
        R::ForecastReplay(w0, w0 + 32.0f * kSlotLen, step_of, t_of, kSlotLen);
        for (int i = 0; i < 32; ++i) {
            if (step_of[i] < 0) continue;  // no step lands here (truncation: the previous index took two)
            const float t = t_of[i];
            if (!(t <= w1)) continue;  // past the row's last generated slot time
            ++slots;
            if (step_of[i] != i) ++shifted;
            const u8 want = R::ForecastSlot(t, r, type), before = R::ForecastSlot(t - kSlotLen, r, type);
            if (base[i] == want) {
                ++exact;
                // review (semantics 5): the golden vectors for btrules' kForecastGolden, one per region (at most 8)
                if (golden < 8 && golden_r != r)
                    golden_r = r, ++golden, Log("[bt] wx golden: T %.9g region %d type %d slot %d game %u (clock %.9g, step %d)", t, r, type, i, base[i], w0, step_of[i]);
                continue;
            }
            if (base[i] == before) {
                ++prev;
                continue;
            }
            ++bad;
            if (logged++ < 4)
                Log("[bt] wx hash mismatch: region %d type %d slot %d T %.9g (step %d): game %u, formula %u (previous slot %u)", r, type, i, t, step_of[i], base[i],
                    want, before);
        }
    }
    Log("[bt] wx hash: %d regions, %d slots, %d mismatches (exact %d, previous-slot %d, truncation-shifted %d, golden lines %d)", regions, slots, bad, exact,
        prev, shifted, golden);
    return Put(msg, cap, "bt probe wx_hash: %d regions, %d slots, %d mismatches (log)", regions, slots, bad), true;
}

// ================================================================ op 18 wx_apply (s)
struct Wx18 {
    bool on = false, restoring = false, restored = false;
    double start_ms = 0, end_ms = 0, next_ms = 0, restore_ms = 0, done_ms = 0;
    int region = -1, state = 0;
} g_o18;
u8 Eff0(uptr ws, int region) noexcept {
    const std::int32_t slot = RdI(ws + kWsSlot);
    return slot >= 0 && slot <= 3 && region >= 0 && region < 64 ? static_cast<u8>(RdB(RowAt(ws, slot, region) + kRowEff)) : 0xFF;
}
void WxApplyLine(double now) {
    const uptr ws = Ws();
    double me[3];
    bool ok = false;
    float raw = 0;
    if (ws && I::LocalPos(me)) raw = RainRawAt(ws, me, &ok);
    const uptr mgr = GzMgr();
    Log("[bt] wx apply +%.0f: eff0 %u raw %s%.3f at Sam mgr238 %u (applies %u, skips %u)", (now - g_o18.start_ms) / 1000.0, ws ? Eff0(ws, g_o18.region) : 0xFF,
        ok ? "" : "(unreadable) ", raw, mgr ? RdB(mgr + kMgr238) : 0xFFu, g_wx_applies.load(), g_wx_skips.load());
    const int n = Census(now);
    if (n <= 0 || !I::LocalPos(me)) return;
    int used[4] = {-1, -1, -1, -1};
    for (int k = 0; k < 4; ++k) {
        double bd = 1e18;
        for (int i = 0; i < n; ++i) {
            if (i == used[0] || i == used[1] || i == used[2]) continue;
            const double d = GzDist(g_gz[i], me);
            if (d < bd) bd = d, used[k] = i;
        }
        if (used[k] < 0) break;
        const I::GzInfo& z = g_gz[used[k]];
        bool rok = false;
        const float r = z.pos_ok ? RainRawAt(ws, z.pos, &rok) : 0.0f;
        Log("[bt] wx apply gazer loc %08X (%.0f m): +0xCC 0x%08X +0x180 %.1f +0x1B4 0x%X rain %s%.3f", z.loc, bd, RdU(z.actor + kActReason),
            RdF(z.actor + kActHealth), RdU(z.actor + kActFlags), rok ? "" : "(unreadable) ", r);
    }
}

// ================================================================ op 19 wx_spot (s): the owner is a guard page
std::atomic<uptr> g_guard{0};
std::atomic<unsigned> g_guard_touch{0};
PVOID g_veh = nullptr;
LONG CALLBACK GuardVeh(EXCEPTION_POINTERS* x) {
    const EXCEPTION_RECORD* r = x ? x->ExceptionRecord : nullptr;
    if (r && r->ExceptionCode == STATUS_GUARD_PAGE_VIOLATION && r->NumberParameters >= 2) {
        const uptr a = static_cast<uptr>(r->ExceptionInformation[1]), g = g_guard.load(std::memory_order_relaxed);
        if (g && a >= g && a < g + 4096) {
            g_guard_touch.fetch_add(1, std::memory_order_relaxed);
            return EXCEPTION_CONTINUE_EXECUTION;  // the guard is one-shot: the access now goes through (the Tick re-arms it)
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
struct Spot19 {
    bool on = false;
    double start_ms = 0, next_ms = 0, refresh_ms = 0, refresh_end_ms = 0;
    double pos[3] = {0, 0, 0};
    float radius = 50;
    int kind = 2, mode = 0, refreshes = 0;
    unsigned touch_seen = 0;
} g_o19;
uptr Owner() noexcept { return g_guard.load() + 0x100; }
bool SpotNow() {
    const uptr ws = Ws();
    return ws && SehSpot(A(g_o19.kind == 2 ? kSpotRainFn : kSpotDryFn), ws, g_o19.pos, g_o19.radius, 10.0f, reinterpret_cast<void*>(Owner()));
}
void GuardRearm() {
    const uptr g = g_guard.load();
    DWORD old = 0;
    if (g && g_guard_touch.load() != g_o19.touch_seen) {
        g_o19.touch_seen = g_guard_touch.load();
        VirtualProtect(reinterpret_cast<void*>(g), 4096, PAGE_READWRITE | PAGE_GUARD, &old);
    }
}
void SpotEnd(const char* why) {
    if (!g_o19.on) return;
    g_o19.on = false;
    const unsigned t = g_guard_touch.load();
    Log("[bt] wx spot owner: %s (%u touches; %d refreshes; %s)", t ? "TOUCHED" : "never touched", t, g_o19.refreshes, why);
    const uptr g = g_guard.load();
    DWORD old = 0;
    if (g) VirtualProtect(reinterpret_cast<void*>(g), 4096, PAGE_READWRITE, &old);  // kept allocated: the game may still hold the key
    if (g_veh) RemoveVectoredExceptionHandler(g_veh), g_veh = nullptr;
}
void SpotLine(double now) {
    const uptr ws = Ws();
    bool ok = false;
    const float raw = ws ? RainRawAt(ws, g_o19.pos, &ok) : 0.0f;
    const int n = ws ? RdI(ws + kWsSpotN) : -1;
    const uptr sp = ws ? mem::Q(ws + kWsSpots) : 0;
    bool ours = false;
    float life = -1;
    for (int i = 0; sp && i < n && i < 64; ++i) {
        u64 owner = 0;
        if (mem::Rd(sp + static_cast<uptr>(i) * kSpotSize + 0x20, owner) && owner == Owner()) {
            ours = true, life = RdF(sp + static_cast<uptr>(i) * kSpotSize + 0x18);
            break;
        }
    }
    Log("[bt] wx spot +%.1f: raw %s%.3f at the spot, spots %d, ours %s life %.1f", (now - g_o19.start_ms) / 1000.0, ok ? "" : "(unreadable) ", raw, n,
        YesNo(ours), life);
}

// ================================================================ op 20 gz_dump (read only)
struct Gz20 {
    bool on = false;
    double end_ms = 0, next_ms = 0;
} g_o20;
// review (semantics 3): the graph's agitation var. [comp+0x160]+0x90 is an int32 INDEX (movsxd at 0x1429D66F6 / 0x1429D6738); the
// value is the float [[comp+0x198]+0x58] + idx*0x30 + 0x20 (0x1429D66D6, 0x1429D6701, 0x1429D670C). *idx: as read (-1 unreadable);
// -1.0f when the index is outside 0..255 or the entry is unreadable.
float GzAgit(uptr comp, int* idx) noexcept {
    *idx = -1;
    const uptr graph = comp ? mem::Q(comp + 0x160) : 0;
    std::int32_t i = -1;
    if (!graph || !mem::Rd(graph + 0x90, i)) return -1.0f;
    *idx = i;
    if (i < 0 || i > 255) return -1.0f;
    const uptr vars = mem::Q(comp + 0x198);
    const uptr data = vars ? mem::Q(vars + 0x58) : 0;
    float v = -1.0f;
    if (!data || !mem::Rd(data + static_cast<uptr>(i) * 0x30 + 0x20, v)) return -1.0f;
    return v;
}
void DumpGz(double now) {
    const int n = Census(now);
    double me[3];
    if (n < 0) return Log("[bt] gz dump: the gazer list failed bt_sync's byte checks");
    if (!I::LocalPos(me)) return;
    int shown = 0;
    for (int i = 0; i < n; ++i) {
        const I::GzInfo& z = g_gz[i];
        const double d = GzDist(z, me);
        if (!(d <= 60.0)) continue;
        ++shown;
        const uptr a = z.actor, ent = GzEntity(a), comp = GzComp(a);
        double vp[3] = {0, 0, 0}, ep[3] = {0, 0, 0};
        mem::Read(a + kActVisPos, vp, sizeof(vp));
        if (ent) mem::Read(ent + 0xC8, ep, sizeof(ep));
        int gi = -1;
        const float agit = GzAgit(comp, &gi);
        Log("[bt] gz dump loc %08X (%.0f m): actor 0x%llX id %08X state %d flags 0x%X kind %u mode %u action 0x%08X blend %.2f dist %.1f vis %.2f "
            "approach %.1f rainT %.2f vis pos (%.1f %.1f %.1f) entity 0x%llX (%.1f %.1f %.1f) comp 0x%llX +0x80 %u +0x7C %.2f +0x90 0x%08X +0x94 0x%08X +0x98 %.2f "
            "agit %.3f (graph var idx %d) health %.2f prev %.2f",
            z.loc, d, H(a), z.id, RdI(a + kActState), RdU(a + kActFlags), RdB(a + kActKind), RdU(a + kActMode), RdU(a + kActAction), RdF(a + kActBlend),
            RdF(a + kActDist), RdF(a + kActVis), RdF(a + kActApproach), RdF(a + kActRainT), vp[0], vp[1], vp[2], H(ent), ep[0], ep[1], ep[2], H(comp),
            comp ? RdB(comp + 0x80) : 0xFFu, comp ? RdF(comp + 0x7C) : -1.0f, comp ? RdU(comp + 0x90) : 0u, comp ? RdU(comp + 0x94) : 0u,
            comp ? RdF(comp + 0x98) : -1.0f, agit, gi, RdF(a + kActHealth), RdF(a + kActPrevHealth));
        if (shown == 1) {  // the realize rule (0x142995C94): range [[actor+0x10]+0x80]+0x70 (+10 m kept), budget [..]+0x24
            std::uint64_t unit = 0, cfg = 0;
            if (mem::Rd(a + 0x10, unit) && unit && mem::Rd(static_cast<uptr>(unit) + 0x80, cfg) && cfg)
                Log("[bt] gz realize: unit 0x%llX cfg 0x%llX range %.1f m budget %d", static_cast<unsigned long long>(unit),
                    static_cast<unsigned long long>(cfg), RdF(static_cast<uptr>(cfg) + 0x70), RdI(static_cast<uptr>(cfg) + 0x24));
        }
    }
    int realized = 0;
    for (int i = 0; i < n; ++i) realized += RdI(g_gz[i].actor + kActState) == 2;
    Log("[bt] gz dump: %d of %d gazers within 60 m (realized, state 2: %d)", shown, n, realized);
}

// ================================================================ op 21 gz_trace (log only)
struct HashCount {
    u32 hash = 0;
    unsigned n = 0;
};
struct Gz21 {
    bool on = false;
    double end_ms = 0;
    unsigned actions = 0, msgs = 0, lines = 0;
    HashCount act[24], msg[24];
} g_o21;
void CountHash(HashCount* t, u32 h) {
    for (int i = 0; i < 24; ++i) {
        if (t[i].n && t[i].hash == h) return void(++t[i].n);
        if (!t[i].n) {
            t[i].hash = h, t[i].n = 1;
            return;
        }
    }
}
void HashList(char* b, std::size_t n, const HashCount* t) {
    std::size_t at = 0;
    b[0] = 0;
    for (int i = 0; i < 24 && t[i].n && at + 24 < n; ++i)
        at += static_cast<std::size_t>(std::snprintf(b + at, n - at, "%s0x%08X x%u", i ? ", " : "", t[i].hash, t[i].n));
}
void TraceEnd(const char* why) {
    if (!g_o21.on) return;
    g_o21.on = false;
    g_trace_gz.store(false);
    char a[640], m[640];
    HashList(a, sizeof(a), g_o21.act), HashList(m, sizeof(m), g_o21.msg);
    Log("[bt] gz trace done (%s): %u actions (%s), %u messages (%s)", why, g_o21.actions, a[0] ? a : "none", g_o21.msgs, m[0] ? m : "none");
}

// ================================================================ op 22 gz_mirror (s)
struct Gz22 {
    bool on = false, gameover = false;
    double start_ms = 0, next_ms = 0;
    uptr actor = 0, comp = 0, ent = 0;
    u32 loc = 0;
    int kind = 2, variant = 0, n = 0;
    // review (agitation restore): the gazer's own +0x80 / +0x7C, read before the first write and written back at the end; the
    // drops stay on for this actor until +0x7C reads its own value back (MirrorBack, once a second, up to 5 s)
    unsigned own80 = 0;
    float own7C = 0;
    bool back = false;
    int back_tries = 0;
    double back_ms = 0;
} g_o22;
// review (crash 3): the component pointer is good only while the gazer stays realized with the entity it had at the pick. Leaving
// state 2 runs vt+0xF0, which frees the entity and its DSGazerComponent while the light actor (and its id) stays; Wr is
// WriteProcessMemory, which does not fail on a freed heap block. So every write re-checks: the id, state 2, the same entity, and
// the component's back pointer (comp+0x110 - 0xE8 == the actor). 0 = still good.
int MirrorStale(uptr actor, uptr comp, uptr ent) noexcept {
    u32 id = 0;
    if (!actor || !mem::Rd(actor + kActId, id) || id != g_mir_id.load(std::memory_order_relaxed)) return 1;
    if (RdI(actor + kActState) != 2) return 2;
    if (!ent || GzEntity(actor) != ent) return 3;
    const uptr back = comp ? mem::Q(comp + 0x110) : 0;
    if (back <= 0xE8 || back - 0xE8 != actor) return 4;
    return 0;
}
const char* StaleText(int w) noexcept {
    switch (w) {
        case 1: return "the gazer is gone: another id at its actor's address";
        case 2: return "the gazer is no longer realized (state != 2: its entity and component were released)";
        case 3: return "the gazer's entity changed (+0x68)";
        case 4: return "the component no longer points back at the gazer";
        default: return "stale";
    }
}
void MirrorAfter(std::uintptr_t actor) noexcept {  // bt_world's gazer update, right after the original: the GM writes
    if (actor != g_mir_actor.load(std::memory_order_relaxed) || g_pass.load(std::memory_order_relaxed)) return;
    const uptr comp = g_mir_comp.load(std::memory_order_relaxed);
    if (const int w = MirrorStale(actor, comp, g_mir_ent.load(std::memory_order_relaxed))) {
        g_mir_actor.store(0), g_mir_comp.store(0);  // no more writes or drops; the next Tick ends the op
        g_mir_lost_why.store(w), g_mir_lost.store(true);
        return;
    }
    Wr<u8>(actor + kActKind, static_cast<u8>(g_mir_kind.load(std::memory_order_relaxed)));
    Wr<u8>(comp + 0x80, 1), Wr<float>(comp + 0x7C, 10.0f);
    if (g_mir_variant.load(std::memory_order_relaxed) == 1) {
        u32 f = 0;
        if (mem::Rd(actor + kActFlags, f)) Wr<u32>(actor + kActFlags, f & ~(kFlagArmed | kFlagNear));
    }
    g_mir_writes.fetch_add(1, std::memory_order_relaxed);
}
enum MirrorEndMode { kEndCheck, kEndNow, kEndNoWrite };  // write back + check later; write back, drops off now; no write (lost, load)
void MirrorEnd(const char* why, MirrorEndMode mode) {
    if (g_o22.back && mode != kEndCheck) {  // a pending check cut short (emergency stop, a load): the drops end now
        g_o22.back = false;
        g_mir_actor.store(0), g_mir_comp.store(0);
        Log("[bt] gz mirror back loc %08X: not checked (%s)", g_o22.loc, why);
        return;
    }
    if (!g_o22.on) return;
    g_o22.on = false;
    btworld::SetAfterUpdate(nullptr);  // no more writes (MirrorAfter and this Tick both run on the game thread)
    const int w = mode == kEndNoWrite ? -1 : MirrorStale(g_o22.actor, g_o22.comp, g_o22.ent);
    if (!w) {
        Wr<u8>(g_o22.comp + 0x80, static_cast<u8>(g_o22.own80)), Wr<float>(g_o22.comp + 0x7C, g_o22.own7C);
        if (mode == kEndCheck) g_o22.back = true, g_o22.back_tries = 0, g_o22.back_ms = I::NowMs() + 1000.0;  // drops stay on
    }
    if (!g_o22.back) g_mir_actor.store(0), g_mir_comp.store(0);
    Log("[bt] gz mirror done loc %08X (%s): writes %u, drops %u/%u/%u, GameOver seen %s; own +0x80 %u +0x7C %.2f %s", g_o22.loc, why, g_mir_writes.load(),
        g_drop_found.load(), g_drop_catch.load(), g_drop_caught.load(), YesNo(g_o22.gameover), g_o22.own80, g_o22.own7C,
        !w ? "written back" : w < 0 ? "not written back (a load or the gazer lost)" : StaleText(w));
    if (!g_o22.back) Log("[bt] gz mirror back loc %08X: not checked (%s)", g_o22.loc, w > 0 ? StaleText(w) : why);  // one back line per mirror
}
// review (agitation restore): once a second after the end, up to 5 s: did +0x80 / +0x7C go back to the gazer's own values? The drops
// end here. "back NO" is a finding (the scenario's nolog), not a crash.
void MirrorBack(double now) {
    if (!g_o22.back || now < g_o22.back_ms) return;
    const int w = MirrorStale(g_o22.actor, g_o22.comp, g_o22.ent);
    const unsigned b80 = w ? 0xFFu : RdB(g_o22.comp + 0x80);
    const float b7C = w ? -1.0f : RdF(g_o22.comp + 0x7C);
    const bool ok = !w && b80 == g_o22.own80 && std::fabs(b7C - g_o22.own7C) < 0.01f;
    ++g_o22.back_tries;
    if (!w && !ok && g_o22.back_tries < 5) return void(g_o22.back_ms = now + 1000.0);
    g_o22.back = false;
    g_mir_actor.store(0), g_mir_comp.store(0);  // the drops end
    Log("[bt] gz mirror back loc %08X after %d s: +0x80 %u +0x7C %.2f vs own %u %.2f: %s", g_o22.loc, g_o22.back_tries, b80, b7C, g_o22.own80, g_o22.own7C,
        w ? StaleText(w) : ok ? "back yes" : "back NO");
}
void MirrorLine(double now) {
    const uptr a = g_o22.actor, comp = g_o22.comp, hm = Hm();
    int gi = -1;
    const float agit = GzAgit(comp, &gi);
    const u64 st = GameState();
    if (GameOverNow()) g_o22.gameover = true;
    Log("[bt] gz mirror +%.0f loc %08X: flags 0x%X kind %u agit %.3f (idx %d) (+0x80 %u +0x7C %.1f) vis +0xB0 %.2f dist %.1f, drops %u/%u/%u, writes %u, "
        "hm140 0x%llX, state %d, game 0x%llX",
        (now - g_o22.start_ms) / 1000.0, g_o22.loc, RdU(a + kActFlags), RdB(a + kActKind), agit, gi, comp ? RdB(comp + 0x80) : 0xFFu,
        comp ? RdF(comp + 0x7C) : -1.0f, RdF(a + kActVis), RdF(a + kActDist), g_drop_found.load(), g_drop_catch.load(), g_drop_caught.load(),
        g_mir_writes.load(), H(hm ? mem::Q(hm + kHmArea) : 0), RdI(a + kActState), static_cast<unsigned long long>(st));
}

// ================================================================ op 23 gz_yaw (s)
struct Gz23 {
    bool on = false;
    double set_ms = 0;
    uptr actor = 0, ent = 0;
    u32 loc = 0;
    double a = 0, b = 0;
} g_o23;
bool EntYaw(uptr ent, double* yaw) {
    game::WorldTransform wt{};
    if (!ent || !game::TryGetTransform(reinterpret_cast<game::Entity*>(ent), &wt)) return false;
    *yaw = YawDeg(wt.rot.m[1][0], wt.rot.m[1][1]);
    return true;
}
double ActorYaw(uptr actor) noexcept { return YawDeg(RdF(actor + kActRot + 12, 0), RdF(actor + kActRot + 16, 0)); }

// ================================================================ op 24 hunt_dump (read only)
struct Hunt24 {
    bool on = false;
    double end_ms = 0, next_ms = 0, quiet_ms = 0;
    int samples = 0;
} g_o24;
void HuntLines(const char* tag) {
    const uptr hm = Hm();
    if (!hm) return Log("[bt] hunt%s: no hunter manager", tag);
    u8 b[0x30];
    if (!mem::Read(hm, b, sizeof(b))) return Log("[bt] hunt%s: unreadable", tag);
    std::int32_t hands = 0;
    float pull = 0, dx = 0, dy = 0, mx = 0;
    std::memcpy(&hands, b, 4), std::memcpy(&pull, b + 4, 4), std::memcpy(&dx, b + 8, 4), std::memcpy(&dy, b + 0xC, 4), std::memcpy(&mx, b + 0x10, 4);
    const int pulls = RdI(hm + kHmPull), circles = RdI(hm + kHmCircN), meshes = RdI(hm + kHmMeshN);
    const uptr area = mem::Q(hm + kHmArea);
    Log("[bt] hunt%s: hands %d pull %.2f dir (%.2f %.2f) max %.2f grab %u/%u, pulls %d circles %d meshes %d, +0x140 0x%llX +0x158 0x%llX +0x170 %d "
        "+0x240 %d gold %u",
        tag, hands, pull, dx, dy, mx, b[0x14], b[0x15], pulls, circles, meshes, H(area), H(mem::Q(hm + 0x158)), RdI(hm + 0x170), RdI(hm + 0x240),
        RdB(hm + 0x244));
    const uptr list = mem::Q(hm + kHmPullList);
    for (int i = 0; list && i < pulls && i < 8; ++i) {
        const uptr ent = mem::Q(list + static_cast<uptr>(i) * 8);
        if (ent < 0x230) continue;
        const uptr c = ent - 0x230;
        double p[3] = {0, 0, 0};
        mem::Read(c + 0x260, p, sizeof(p));
        Log("[bt] crowd %d: comp 0x%llX state %d target %08X type %u +0x8B8 0x%X +0x228 0x%08X +0xE0 0x%08X hands %d +0x230 %d +0x234 %.2f +0x240 %.2f "
            "+0x244 %u pos (%.1f %.1f %.1f) r %.1f, local %s",
            i, H(c), RdI(c + 0x108), RdU(c + 0x140), RdU(c + 0x150), RdU(c + 0x8B8), RdU(c + 0x228), RdU(c + 0xE0), RdI(c + 0x2B0), RdI(c + 0x230),
            RdF(c + 0x234), RdF(c + 0x240), RdB(c + 0x244), p[0], p[1], p[2], RdF(c + 0x278), YesNo(area && mem::Q(c + 0x48) == area - 0x20));
    }
}
bool HuntUp() noexcept {
    const uptr hm = Hm();
    return hm && (mem::Q(hm + kHmArea) || RdI(hm + kHmPull, 0) > 0);
}

// ================================================================ op 25 hunt_self (s)
struct Hunt25 {
    bool on = false, ended = false;
    double start_ms = 0, next_ms = 0, end_ms = 0, stop_ms = 0;
    int variant = 0;
} g_o25;
const char* HuntCaller(uptr ret) noexcept {
    struct Rg {
        uptr a, b;
        const char* name;
    };
    static const Rg kRg[] = {{0x1429C6CF0, 0x1429C6F3D, "c6cf0"}, {0x1429D9080, 0x1429D9389, "d9080"}, {0x1429E29E0, 0x1429E3609, "e29e0"},
                             {0x1429E5D50, 0x1429E6D34, "e5d50"}};
    for (const Rg& r : kRg)
        if (ret >= A(r.a) && ret < A(r.b)) return r.name;
    return "other";
}
void HuntSelfLine(double now) {
    const uptr hm = Hm(), s = SamEnt();
    u8 b[0x18] = {};
    if (hm) mem::Read(hm, b, sizeof(b));
    std::int32_t hands = 0;
    float pull = 0, mx = 0;
    std::memcpy(&hands, b, 4), std::memcpy(&pull, b + 4, 4), std::memcpy(&mx, b + 0x10, 4);
    const uptr p428 = s ? mem::Q(s + 0x428) : 0;
    float raw = -1.0f;
    const float tar = SamTar(&raw);
    Log("[bt] hunt self +%.2f: hands %d pull %.2f max %.2f grab %u, members %d, Sam tar +0x844 %.3f (+0x848 %.3f), +0x7608 %.2f, +0x140 0x%llX "
        "+0x158 0x%llX, health %.1f, game 0x%llX",
        (now - g_o25.start_ms) / 1000.0, hands, pull, mx, b[0x14], hm ? RdI(hm + kHmPull) : -1, tar, raw, p428 ? RdF(p428 + 0x7608) : -1.0f,
        H(hm ? mem::Q(hm + kHmArea) : 0), H(hm ? mem::Q(hm + 0x158) : 0), Health(), static_cast<unsigned long long>(GameState()));
}
void HuntSelfEnd(const char* why) {
    if (!g_o25.on) return;
    g_o25.on = false;
    g_hunt_watch.store(false);
    Log("[bt] hunt self done (%s): end seen %s", why, YesNo(g_o25.ended));
}

// ================================================================ op 26 tar_record (s)
struct alignas(16) TarRec {  // the 0x20-byte record both lists read: f64 pos[3], f32 radius (+0x18), f32 height (+0x1C)
    double pos[3];
    float radius, height;
};
TarRec g_tar{};
struct Tar26 {
    bool on = false, in = false, cue = false, fact_hooked = false;  // cue: +0x50 seen set (the list is non-empty; not "inside")
    float max_in = -1, max_out = -1;  // the highest +0x844 with Sam inside / outside the record (the locomotion's own test)
    int n_in = 0, n_out = 0;
    double start_ms = 0, next_ms = 0;
    int list = 0;
    unsigned intar0 = 0;
    uptr hm = 0;
} g_o26;
void TarOut(const char* why) {
    if (!g_o26.on) return;
    g_o26.on = false;
    int rc = 0, n = -1;
    if (g_o26.in) {
        const uptr hm = Hm();
        rc = hm == g_o26.hm ? SehTarList(hm, g_o26.list ? kHmCircN : kHmMeshN, g_o26.list ? kHmCirc : kHmMesh, reinterpret_cast<uptr>(&g_tar), false, &n) : 0;
        g_o26.in = false;
    }
    Log("[bt] tar rec out (%s): list %d, removed %s, count %d; +0x844 max inside %.3f (%d samples), outside %.3f (%d samples)", why, g_o26.list,
        rc == 1 ? "yes" : rc < 0 ? "FAULTED" : "no (not found)", n, g_o26.max_in, g_o26.n_in, g_o26.max_out, g_o26.n_out);
}
void TarLine(double now) {
    const uptr s = SamEnt(), p = s ? mem::Q(s + kSamParam) : 0, hm = Hm();
    const unsigned cue = p ? RdB(p + kParTarList) : 0xFFu;
    if (cue != 0 && cue != 0xFFu) g_o26.cue = true;
    double me[3] = {0, 0, 0};
    const bool have = I::LocalPos(me);
    // review (semantics 2): "inside" by the locomotion's own test (0x14280AF50..0x14280AFC0): the 2D distance on pos[0] / pos[1]
    // below max(r - 0.5, 1.0), and Sam's pos[2] below the record's pos[2] + height.
    const double dx = me[0] - g_tar.pos[0], dy = me[1] - g_tar.pos[1], d2 = std::sqrt(dx * dx + dy * dy);
    const double rin = std::max(static_cast<double>(g_tar.radius) - kTarInsideIn, static_cast<double>(kTarInsideMin));
    const double top = g_tar.pos[2] + g_tar.height;
    const bool inside = have && d2 < rin && me[2] < top;
    float raw = -1.0f;
    const float tar = p ? RdF(p + kParTarSm) : -1.0f;
    if (p) raw = RdF(p + kParTar);
    if (have && p) {
        float& mx = inside ? g_o26.max_in : g_o26.max_out;
        mx = std::max(mx, tar);
        ++(inside ? g_o26.n_in : g_o26.n_out);
    }
    const unsigned calls = g_intar_calls.load() - g_o26.intar0;
    char fact[32];
    if (g_o26.fact_hooked) std::snprintf(fact, sizeof(fact), "%s (%u calls)", YesNo(calls > 0), calls);
    else std::snprintf(fact, sizeof(fact), "n/a (no hook)");
    Log("[bt] tar rec +%.1f: inside %s (2D %.1f m vs %.1f, z %.1f vs top %.1f), +0x844 %.3f (+0x848 %.3f), list non-empty +0x50 %u (seen %s), "
        "fact in_tar %s, hm140 0x%llX, cargo n/a (screenshot), health %.1f, game 0x%llX",
        (now - g_o26.start_ms) / 1000.0, have ? YesNo(inside) : "unknown", have ? d2 : -1.0, rin, have ? me[2] : 0.0, top, tar, raw, cue, YesNo(g_o26.cue),
        fact, H(hm ? mem::Q(hm + kHmArea) : 0), Health(), static_cast<unsigned long long>(GameState()));
}

// ================================================================ op 28 crater_dump (read only)
void DumpCraters(const char* tag) {
    const uptr cm = Cm();
    if (!cm) return Log("[bt] crater%s: no catcher manager", tag);
    const int n = RdI(cm + kCmCraters), p = RdI(cm + kCmPending);
    Log("[bt] crater%s: %d records, pending %d, gen %d, dirty %u, +0x2AD %u, +0x2B0 %d", tag, n, p, RdI(cm + kCmGen), RdB(cm + kCmDirty), RdB(cm + 0x2AD),
        RdI(cm + 0x2B0));
    for (int list = 0; list < 2; ++list) {
        const int cnt = list ? p : n;
        const uptr data = mem::Q(cm + (list ? kCmPending : kCmCraters) + 8);
        for (int i = 0; data && i < cnt && i < 16; ++i) {
            const uptr r = data + static_cast<uptr>(i) * kCraterSize;
            double pos[3] = {0, 0, 0};
            mem::Read(r + 0x18, pos, sizeof(pos));
            const uptr ref = mem::Q(r);
            Log("[bt] crater%s %s %d: entity 0x%llX at (%.1f %.1f %.1f) r %.1f active %u handle %d delay %.2f gen %d age %d area %u yaw %.2f flags 0x%X", tag,
                list ? "pend" : "rec", i, H(ref > 0x20 ? ref - 0x20 : 0), pos[0], pos[1], pos[2], RdF(r + 0x30), RdB(r + 0x3C), RdI(r + 0x40),
                RdF(r + 0x44), RdI(r + 0x48), RdI(r + 0x4C), RdU(r + 0x50), RdF(r + 0x54), RdU(r + 0x58));
        }
    }
    const uptr reg = mem::Q(A(kCraterReg));
    if (!reg) return Log("[bt] crater%s registry: none", tag);
    Log("[bt] crater%s registry 0x%llX: closest (%.1f %.1f) r %.1f index %d, list %d (0x%llX)", tag, H(reg), RdF(reg + 0x70), RdF(reg + 0x74),
        RdF(reg + 0x78), RdI(reg + 0x80), RdI(reg + 0x90), H(mem::Q(reg + 0x98)));
}

// ================================================================ op 29 blast (s)
// stage 0 (run 20261127-205812 "ref released no"): FUN_14191C790 / FUN_14191C810 take a 24-byte node ([0] the target = entity +0x20,
// [1] / [2] the target's list links: add writes node[2] and the old head's +8, release writes through node[1] / node[2]), as the
// voidout's own mgr +0x2B0. The old single u64 let the add write past it (into .ent and the next global) and the release fault.
alignas(16) u64 g_blast_ref[3] = {};
// Set when the ref add or release faulted: the node may still be in an entity's list, and the fault may have left that list's
// hashed SRW lock (DAT_144f6bf00[crc32 & 0x3f]) held. g_blast_ref is then never rewritten and add / release never called again.
bool g_blast_dead = false;
struct Blast29 {
    bool on = false;
    double start_ms = 0, next_ms = 0, fx_end_ms = 0, end_ms = 0;
    int fx = 0;
    unsigned flow0 = 0;
    bool linked = false;  // g_blast_ref is in the entity's list
    uptr ent = 0;
} g_o29;
void FxRestore() {
    if (!g_fx_saved.load()) return;
    const uptr res = g_fx_res.load();
    const u32 v[2] = {g_fx_start0.load(), g_fx_range0.load()};
    const bool ok = WrN(res + 0x28, v, sizeof(v));
    g_fx_saved.store(false);
    float f[2];
    std::memcpy(f, v, sizeof(f));
    Log("[bt] fx fade restored on res 0x%llX: FadeStart %.1f FadeRange %.1f (%s)", H(res), f[0], f[1], ok ? "ok" : "FAILED");
}
void BlastEnd(const char* why) {
    if (!g_o29.on) return;
    g_o29.on = false;
    g_fx_window.store(false);
    FxRestore();
    bool rel = false;
    const bool cleared = g_o29.linked && g_blast_ref[0] == 0;  // the entity went first: its list cleared our node
    if (g_o29.linked && !g_blast_dead) {
        rel = SehRef(A(kRefRelease), g_blast_ref);
        if (!rel) g_blast_dead = true;
    }
    g_o29.linked = false;
    Log("[bt] blast done (%s): ref released %s%s%s, flow calls %u, Sam health %.1f, game 0x%llX", why, YesNo(rel), cleared ? " (the game had cleared it)" : "",
        g_blast_dead ? " (the ref call FAULTED: no more blasts)" : "",
        I::FlowCalls() - g_o29.flow0, Health(),
        static_cast<unsigned long long>(GameState()));
}

// ================================================================ op 30 crater_add (s)
struct Crater30 {
    bool on = false, l9 = false;
    double start_ms = 0;
    int gen = 0;
} g_o30;
uptr CraterByGen(int gen, unsigned* active) {
    const uptr cm = Cm();
    const int n = cm ? RdI(cm + kCmCraters) : 0;
    const uptr data = cm ? mem::Q(cm + kCmCraters + 8) : 0;
    for (int i = 0; data && i < n && i < 64; ++i) {
        const uptr r = data + static_cast<uptr>(i) * kCraterSize;
        if (RdI(r + 0x48) == gen) {
            *active = RdB(r + 0x3C);
            return r;
        }
    }
    return 0;
}
void CraterLine(const char* at) {
    unsigned active = 0xFFu;
    const uptr r = CraterByGen(g_o30.gen, &active);
    const uptr ref = r ? mem::Q(r) : 0;
    Log("[bt] crater add %s: gen %d %s, active %u entity 0x%llX, Sam health %.1f, game 0x%llX", at, g_o30.gen, r ? "in the records" : "not in the records",
        active, H(ref > 0x20 ? ref - 0x20 : 0), Health(), static_cast<unsigned long long>(GameState()));
}

// ================================================================ op 31 cat_dump, op 32 cat_log, op 33 cat_bring
unsigned CatType(u32 id) noexcept {
    const uptr cm = Cm();
    const int n = cm ? RdI(cm + kCmCount) : 0;
    const uptr data = cm ? mem::Q(cm + kCmData) : 0;
    for (int i = 0; data && i < n && i < 64; ++i) {
        const uptr r = data + static_cast<uptr>(i) * kCatRecSize;
        if (RdU(r + 0x20) == id) return RdB(r + 0x28);
    }
    return 0xFFu;
}
void DumpCat(const char* tag) {
    const uptr cm = Cm();
    if (!cm) return Log("[bt] cat%s: no catcher manager", tag);
    const int n = RdI(cm + kCmCount);
    Log("[bt] cat%s mgr: records %d, +0x2A8 %d +0x2AC %u +0x2B0 %.3f +0x2CC %u, bring +0x110 %u +0x112 %u", tag, n, RdI(cm + kCmGen), RdB(cm + kCmDirty),
        RdF(cm + 0x2B0), RdB(cm + 0x2CC), RdB(cm + 0x110), RdB(cm + 0x112));
    const uptr data = mem::Q(cm + kCmData);
    for (int i = 0; data && i < n && i < 8; ++i) {
        const uptr r = data + static_cast<uptr>(i) * kCatRecSize;
        double p[3] = {0, 0, 0}, bar[3] = {0, 0, 0};
        mem::Read(r + 0x68, p, sizeof(p)), mem::Read(r + 0xB0, bar, sizeof(bar));
        Log("[bt] cat%s rec %d: type %u id %08X regid %08X +0x2C 0x%X at (%.1f %.1f %.1f) bar (%.1f %.1f %.1f) area %.1f gone %u hp %.1f sub %.1f slot %d", tag,
            i, RdB(r + 0x28), RdU(r + 0x20), RdU(r + 0x24), RdU(r + 0x2C), p[0], p[1], p[2], bar[0], bar[1], bar[2], RdF(r + 0xE0), RdB(r + 0xF0),
            RdF(r + 0xF4), RdF(r + 0x100), RdI(r + 0x104));
        const uptr ref = mem::Q(r);
        const uptr comp = ref > 0x20 ? partner::FindComponentByName(reinterpret_cast<game::Entity*>(ref - 0x20), "DSCatcherBaseComponent") : 0;
        if (!comp) continue;
        const float mx = RdF(comp + 0x138), life = RdF(comp + 0x13C);
        Log("[bt] cat%s comp %d: 0x%llX id %08X life %.1f / %.1f (%.1f%% vs rec +0xF4 %.1f) annihilation +0x290 %.2f weather +0x1B8 0x%llX", tag, i, H(comp),
            RdU(comp + 0x58), life, mx, mx > 0 ? life / mx * 100.0f : -1.0f, RdF(r + 0xF4), RdF(comp + 0x290), H(mem::Q(comp + 0x1B8)));
    }
    const int tn = RdI(cm + kCmTerrN);
    const uptr ta = mem::Q(cm + kCmTerr);
    double me[3] = {0, 0, 0};
    const bool have = I::LocalPos(me);
    for (int j = 0; ta && j < tn && j < 16; ++j) {
        const uptr loc = mem::Q(ta + static_cast<uptr>(j) * 8);
        double p[3] = {0, 0, 0};
        if (!loc || !mem::Read(loc + 0x20, p, sizeof(p))) continue;
        const uptr unit = mem::Q(loc + 0x70);
        Log("[bt] cat%s terr %d: type %u at (%.1f %.1f %.1f) radii %.1f / %.1f, unit resource 0x%llX, loaded n/a (not read), %.0f m", tag, j,
            unit ? RdB(unit + 0x58) : 0xFFu, p[0], p[1], p[2], RdF(loc + 0x80), RdF(loc + 0x84), H(unit), have ? R::Dist3(me, p) : -1.0);
    }
}
struct Cat32 {
    bool on = false;
    double end_ms = 0;
    unsigned lines = 0;
} g_o32;
void CatLogEnd(const char* why) {
    if (!g_o32.on) return;
    g_o32.on = false;
    g_cat_log.store(false);
    Log("[bt] cat log done (%s): %u lines", why, g_o32.lines);
}
struct Cat33 {
    bool on = false, d1 = false, seen = false;
    double start_ms = 0;
    int cmd = 0, n0 = 0;
} g_o33;
constexpr u32 kNotifySpawned = 0x63DB9ED5u;

// ================================================================ op 34 bt_sky (s)
struct Sky34 {
    bool on = false;
    double start_ms = 0, end_ms = 0, next_ms = 0;
    unsigned calls = 0;
} g_o34;
void SkyEnd(const char* why) {
    if (!g_o34.on) return;
    g_o34.on = false;
    const uptr ws = Ws();
    u32 v = 0;
    const bool ok = ws && mem::Rd(ws + kWsSky, v) && Wr<u32>(ws + kWsSky, v & ~1u);
    Log("[bt] sky done (%s): %u calls, +0x6518 bit 0 cleared %s, 6508 %u", why, g_o34.calls, YesNo(ok), ws ? RdB(ws + kWs6508) : 0xFFu);
}

// ================================================================ op 35 void_real (s), last step only
struct Void35 {
    bool on = false, loaded = false, back = false;
    double start_ms = 0, back_ms = 0;
    unsigned flow0 = 0;
} g_o35;

// ================================================================ op 36 kill_look (s)
struct Kill36 {
    bool on = false;
    double start_ms = 0;
    uptr actor = 0;
    u32 id = 0, loc = 0, reason = 0;
    u32 s79 = 0, s7a = 0;
    bool s_ok = false, event = false;
} g_o36;
constexpr u32 kReasonCodes[4] = {R::kKillRainGone, R::kKillStatsB, R::kKillStatsA, R::kKillCrater};

// ================================================================ the ring's events (Tick)
void DrainRing() {
    Ev e;
    int k = 0;
    while (k++ < 128 && Pop(&e)) {
        switch (e.kind) {
            case kEvGzAction:
                if (!g_o21.on) break;
                ++g_o21.actions, CountHash(g_o21.act, e.a);
                if (g_o21.lines++ < 300)
                    Log("[bt] gz action 0x%08X%s comp 0x%llX actor loc %08X flags 0x%X thread %s", e.a, e.b ? " (skip set)" : "", H(e.p), LocOf(e.q), e.c,
                        e.tick ? "tick" : "other");
                break;
            case kEvGzMsg:
                if (!g_o21.on) break;
                ++g_o21.msgs, CountHash(g_o21.msg, e.a);
                if (g_o21.lines++ < 300) Log("[bt] gz msg 0x%08X loc %08X thread %s", e.a, LocOf(e.q), e.tick ? "tick" : "other");
                break;
            case kEvHuntEnd:
                if (!g_o25.on) break;
                g_o25.ended = true;
                Log("[bt] hunt end: caller %s site 0x%llX after %.2f s (hm 0x%llX, thread %s)", HuntCaller(e.p), Va(e.p), (I::NowMs() - g_o25.start_ms) / 1000.0,
                    H(e.q), e.tick ? "tick" : "other");
                if (g_o25.variant == 0) g_o25.stop_ms = I::NowMs() + 1000.0;
                break;
            case kEvCatNotify:
                if (e.b == kNotifySpawned && g_o33.on) g_o33.seen = true;
                if (g_o32.on && g_o32.lines++ < 400) Log("[bt] cat notify id %08X hash 0x%08X type %u (thread %s)", e.a, e.b, CatType(e.a), e.tick ? "tick" : "other");
                break;
            case kEvCatDrop:
                if (g_o32.on && g_o32.lines++ < 400)
                    Log("[bt] cat drop at (%.1f %.1f %.1f)%s param 0x%llX total %d", e.pos[0], e.pos[1], e.pos[2], e.flag ? "" : " (unreadable)", H(e.p),
                        static_cast<int>(e.a));
                break;
            case kEvRespawn:
                if (g_o32.on && g_o32.lines++ < 400) {
                    const char* f = e.flag ? R::FieldName(e.a) : nullptr;
                    Log("[bt] gz respawn on scene 0x%llX field %08X%s%s", H(e.p), e.a, f ? " " : "", f ? f : "");
                }
                break;
            case kEvCatCrater:
                if (g_o32.on && g_o32.lines++ < 400)
                    Log("[bt] cat crater id %08X at (%.1f %.1f %.1f)%s type %u", e.a, e.pos[0], e.pos[1], e.pos[2], e.flag ? "" : " (no record)", e.b);
                break;
            case kEvFx: {
                char g[49];
                for (int i = 0; i < 24; ++i) std::snprintf(g + i * 2, 3, "%02x", e.g[i]);
                Log("[bt] fx effector res 0x%llX guid@+0x08 %s match %u, +0x28 %.1f +0x2C %.1f%s inst 0x%llX", H(e.p), g, e.a, e.pos[0], e.pos[1],
                    e.b ? " (fade written)" : "", H(e.q));
                break;
            }
            default: break;
        }
    }
    const unsigned lost = g_lost.exchange(0);
    if (lost) Log("[bt] r31 probe: %u detour events lost (the ring was full)", lost);
}

// ================================================================ ending
void EndAll(const char* why, bool keep35) {
    if (g_o15.on) g_o15.on = false, Log("[bt] wx dump ended (%s)", why);
    if (g_o16.armed) g_o16.armed = false, ThreadsRead();
    if (g_o18.on || g_o18.restoring) {
        g_o18.on = g_o18.restoring = false;
        if (g_wx_mode.load() == 1) g_wx_mode.store(2);
        const uptr ws = Ws();
        const uptr job = ws ? mem::Q(ws + kWsJob) : 0;
        u32 st = 1;
        if (job) mem::Rd(job + 0x28, st);
        bool done = false;
        if (g_pass.load() || st == 0 || st == kJobDone) done = ws && WxRestore(ws), g_wx_mode.store(0);
        Log("[bt] wx apply ended (%s): restore %s", why, done ? "written from the Tick" : g_wx_mode.load() == 2 ? "left to the next job run" : "not written (no world)");
    }
    SpotEnd(why);
    if (g_o20.on) g_o20.on = false, Log("[bt] gz dump ended (%s)", why);
    TraceEnd(why);
    MirrorEnd(why, I::LoadingNow() ? kEndNoWrite : kEndNow);  // a load frees the gazers: nothing is written then
    g_o23.on = false;
    if (g_o24.on) g_o24.on = false, Log("[bt] hunt dump ended (%s): %d samples with a hunt up", why, g_o24.samples);
    HuntSelfEnd(why);
    TarOut(why);
    BlastEnd(why);
    if (g_o30.on) g_o30.on = false, Log("[bt] crater add ended (%s)", why);
    CatLogEnd(why);
    g_o33.on = false, g_notify_watch.store(false);
    SkyEnd(why);
    if (g_o36.on) g_o36.on = false, Log("[bt] kill look ended (%s)", why);
    if (!keep35 && g_o35.on) g_o35.on = false, Log("[bt] void real ended (%s)", why);
}

// ================================================================ the ops (the action, game thread)
bool Need(unsigned groups, const char* name, char* msg, std::size_t cap) {
    if ((Check() & groups) == groups) return true;
    return Put(msg, cap, "bt probe %s: off (its byte checks failed; log)", name), false;
}
double Arg(const double (&a)[5], int i, double def) noexcept { return std::isfinite(a[i]) && a[i] != 0 ? a[i] : def; }

bool Op15(const double (&a)[5], char* msg, std::size_t cap) {
    if (!Need(kGWx, "wx_dump", msg, cap)) return false;
    DumpWx("");
    const double s = Arg(a, 1, 0);
    if (s > 0) g_o15.on = true, g_o15.end_ms = I::NowMs() + std::min(s, 600.0) * 1000.0, g_o15.next_ms = I::NowMs() + 5000.0;
    return Put(msg, cap, "bt probe wx_dump: logged%s", s > 0 ? " (every 5 s)" : ""), true;
}
bool Op16(const double (&a)[5], char* msg, std::size_t cap) {
    if (g_o16.in) {
        g_o16.armed = false;
        ThreadsRead();
        return Put(msg, cap, "bt probe hooks_threads: read (log)"), true;
    }
    int in = 0;
    char why[160] = "";
    for (int h = kHJob; h <= kHFx; ++h)
        if (HookIn(h, why, sizeof(why))) ++in;
    if (!in) return Put(msg, cap, "%s", why[0] ? why : "bt probe hooks_threads: no hook went in"), false;
    g_o16.in = g_o16.armed = true;
    g_o16.read_ms = I::NowMs() + Arg(a, 1, 10) * 1000.0;
    Log("[bt] threads: %d of 10 pass-through counters in; the counts at +%.0f s or on the next bt_probe 16", in, Arg(a, 1, 10));
    return Put(msg, cap, "bt probe hooks_threads: %d of 10 in (log)", in), true;
}
bool Op18(const double (&a)[5], char* msg, std::size_t cap) {
    if (!Need(kGWx, "wx_apply", msg, cap)) return false;
    if (g_o18.on || g_wx_mode.load()) return Put(msg, cap, "bt probe wx_apply: the last one is still on (or restoring)"), false;
    const double r = a[1], st = a[2], s = Arg(a, 3, 60);
    if (!(st >= 0 && st <= 3 && st == std::floor(st))) return Put(msg, cap, "bt probe wx_apply: state 0..3 (sunny, cloudy, rain, heavy)"), false;
    const uptr ws = Ws();
    if (!ws) return Put(msg, cap, "bt probe wx_apply: no weather system"), false;
    int region = static_cast<int>(r);
    if (r == -1) {
        double me[3];
        if (!I::LocalPos(me) || !SehRegion(ws, me, &region)) return Put(msg, cap, "bt probe wx_apply: Sam's region unreadable"), false;
    }
    if (!(region >= 0 && region < 64) || static_cast<double>(region) != std::floor(r == -1 ? region : r))
        return Put(msg, cap, "bt probe wx_apply: region 0..63 or -1 (Sam's)"), false;
    if (!HookIn(kHJob, msg, cap)) return false;
    g_wx_region.store(region), g_wx_state.store(static_cast<int>(st)), g_wx_slot.store(-1);
    g_wx_applies.store(0), g_wx_skips.store(0), g_wx_restores.store(0);
    g_wx_first.store(true);
    g_wx_mode.store(1, std::memory_order_release);
    g_o18 = Wx18{};
    g_o18.on = true, g_o18.region = region, g_o18.state = static_cast<int>(st);
    g_o18.start_ms = I::NowMs(), g_o18.end_ms = g_o18.start_ms + std::min(s, 300.0) * 1000.0, g_o18.next_ms = g_o18.start_ms + 5000.0;
    Log("[bt] wx apply on: region %d (%s) all 32 slots = %d for %.0f s (eff0 now %u)", region, r == -1 ? "Sam's" : "given", static_cast<int>(st), s, Eff0(ws, region));
    return Put(msg, cap, "bt probe wx_apply: region %d = %d for %.0f s (log)", region, static_cast<int>(st), s), true;
}
bool Op19(const double (&a)[5], char* msg, std::size_t cap) {
    if (!Need(kGSpot, "wx_spot", msg, cap)) return false;
    if (g_o19.on) return Put(msg, cap, "bt probe wx_spot: the last one is still watched"), false;
    const double m = Arg(a, 1, 40), radius = Arg(a, 2, 50), kind = a[3], mode = a[4];
    if (!(kind == 0 || kind == 2) || !(mode == 0 || mode == 1)) return Put(msg, cap, "bt probe wx_spot: kind 2 (rain) or 0 (dry), mode 0 (once) or 1 (20 s)"), false;
    if (!(radius > 0 && radius <= 500 && m >= 0 && m <= 500)) return Put(msg, cap, "bt probe wx_spot: m 0..500, radius 1..500"), false;
    double me[3], fx = 0, fy = 0;
    if (!I::SamFacing(me, &fx, &fy) || !R::AheadOf(me, fx, fy, m, g_o19.pos)) return Put(msg, cap, "bt probe wx_spot: no local player transform"), false;
    if (!g_guard.load()) {
        void* p = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!p) return Put(msg, cap, "bt probe wx_spot: no page for the owner key"), false;
        g_guard.store(reinterpret_cast<uptr>(p));
    }
    g_guard_touch.store(0);
    g_o19.touch_seen = 0;
    DWORD old = 0;
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, &GuardVeh);
    if (!g_veh || !VirtualProtect(reinterpret_cast<void*>(g_guard.load()), 4096, PAGE_READWRITE | PAGE_GUARD, &old))
        return Put(msg, cap, "bt probe wx_spot: the guard page failed"), false;
    g_o19.radius = static_cast<float>(radius), g_o19.kind = static_cast<int>(kind), g_o19.mode = static_cast<int>(mode), g_o19.refreshes = 0;
    if (!SpotNow()) {
        g_o19.on = true;
        SpotEnd("the spot call faulted");
        return Put(msg, cap, "bt probe wx_spot: the spot call faulted"), false;
    }
    const double now = I::NowMs();
    g_o19.on = true, g_o19.start_ms = now, g_o19.next_ms = now + 500.0, g_o19.refresh_ms = now + 2000.0, g_o19.refresh_end_ms = now + 20000.0;
    char p[64];
    FmtPos(p, sizeof(p), g_o19.pos);
    Log("[bt] wx spot on: kind %d at (%s) %.0f m ahead r %.0f m life 10 s, %s, owner 0x%llX (a guard page)", g_o19.kind, p, m, radius,
        g_o19.mode ? "refreshed every 2 s for 20 s" : "once", H(Owner()));
    return Put(msg, cap, "bt probe wx_spot: kind %d %.0f m ahead (log for 40 s)", g_o19.kind, m), true;
}
bool Op20(const double (&a)[5], char* msg, std::size_t cap) {
    DumpGz(I::NowMs());
    const double s = Arg(a, 1, 0);
    if (s > 0) g_o20.on = true, g_o20.end_ms = I::NowMs() + std::min(s, 300.0) * 1000.0, g_o20.next_ms = I::NowMs() + 1000.0;
    return Put(msg, cap, "bt probe gz_dump: logged%s", s > 0 ? " (once a second)" : ""), true;
}
bool Op21(const double (&a)[5], char* msg, std::size_t cap) {
    if (!Need(kGGz, "gz_trace", msg, cap)) return false;
    if (!HookIn(kHGzAction, msg, cap) || !HookIn(kHGzMsg, msg, cap)) return false;
    g_o21 = Gz21{};
    g_o21.on = true, g_o21.end_ms = I::NowMs() + std::min(Arg(a, 1, 20), 300.0) * 1000.0;
    g_gz_ms = -1e18, Census(I::NowMs());
    g_trace_gz.store(true);
    Log("[bt] gz trace on for %.0f s", Arg(a, 1, 20));
    return Put(msg, cap, "bt probe gz_trace: on for %.0f s (log)", Arg(a, 1, 20)), true;
}
bool Op22(const double (&a)[5], char* msg, std::size_t cap) {
    if (!Need(kGGz, "gz_mirror", msg, cap)) return false;
    if (g_o22.on || g_o22.back) return Put(msg, cap, "bt probe gz_mirror: the last one is still on (or its write-back check)"), false;
    const double kind = a[3], variant = a[4];
    if (!(kind >= 0 && kind <= 4 && kind == std::floor(kind)) || !(variant == 0 || variant == 1))
        return Put(msg, cap, "bt probe gz_mirror: kind 0..4, variant 0 (writes and drops) or 1 (also ARMED / NEAR cleared)"), false;
    char why[96];
    const I::GzInfo* z = PickGazer(a[1], a[2], kPickFar40Comp, why, sizeof(why));
    if (!z) return Put(msg, cap, "bt probe gz_mirror: %s", why), false;
    const uptr comp = GzComp(z->actor), ent = GzEntity(z->actor);
    if (!comp) return Put(msg, cap, "bt probe gz_mirror: gazer %08X has no DSGazerComponent", z->loc), false;
    g_mir_id.store(z->id);
    if (const int w = MirrorStale(z->actor, comp, ent)) return Put(msg, cap, "bt probe gz_mirror: gazer %08X: %s", z->loc, StaleText(w)), false;
    u8 own80 = 0;
    float own7C = 0;
    if (!mem::Rd(comp + 0x80, own80) || !mem::Rd(comp + 0x7C, own7C) || !std::isfinite(own7C))
        return Put(msg, cap, "bt probe gz_mirror: gazer %08X: its +0x80 / +0x7C are unreadable", z->loc), false;
    if (!I::InstallGz(msg, cap) || !HookIn(kHGzAction, msg, cap) || !HookIn(kHGzMsg, msg, cap)) return false;
    g_drop_found.store(0), g_drop_catch.store(0), g_drop_caught.store(0), g_mir_writes.store(0);
    g_mir_kind.store(static_cast<int>(kind)), g_mir_variant.store(static_cast<int>(variant));
    g_mir_lost.store(false), g_mir_lost_why.store(0), g_mir_ent.store(ent);
    g_mir_comp.store(comp), g_mir_actor.store(z->actor);
    btworld::SetAfterUpdate(&MirrorAfter);
    g_o22 = Gz22{};
    g_o22.on = true, g_o22.actor = z->actor, g_o22.comp = comp, g_o22.ent = ent, g_o22.loc = z->loc, g_o22.kind = static_cast<int>(kind),
    g_o22.variant = static_cast<int>(variant), g_o22.own80 = own80, g_o22.own7C = own7C;
    g_o22.start_ms = I::NowMs(), g_o22.next_ms = g_o22.start_ms;
    Log("[bt] gz mirror on: loc %08X (%.0f m, +0x204 %.1f) kind %d variant %d, comp 0x%llX entity 0x%llX, own +0x80 %u +0x7C %.2f", z->loc, z->pdist,
        RdF(z->actor + kActDist), g_o22.kind, g_o22.variant, H(comp), H(ent), g_o22.own80, g_o22.own7C);
    return Put(msg, cap, "bt probe gz_mirror: loc %08X for 20 s (log)", z->loc), true;
}
bool Op23(const double (&a)[5], char* msg, std::size_t cap) {
    if (!Need(kGGz, "gz_yaw", msg, cap)) return false;
    const double deg = a[3];
    if (!std::isfinite(deg) || std::fabs(deg) > 360) return Put(msg, cap, "bt probe gz_yaw: deg -360..360"), false;
    char why[96];
    const I::GzInfo* z = PickGazer(a[1], a[2], kPickEntity, why, sizeof(why));
    if (!z) return Put(msg, cap, "bt probe gz_yaw: %s", why), false;
    const uptr ent = GzEntity(z->actor);
    game::WorldTransform wt{};
    if (!ent || !game::TryGetTransform(reinterpret_cast<game::Entity*>(ent), &wt)) return Put(msg, cap, "bt probe gz_yaw: the entity transform is unreadable"), false;
    const double before = YawDeg(wt.rot.m[1][0], wt.rot.m[1][1]);
    const double rad = deg / 57.29577951308232, c = std::cos(rad), s = std::sin(rad);
    for (int r = 0; r < 3; ++r) {
        const double x = wt.rot.m[r][0], y = wt.rot.m[r][1];
        wt.rot.m[r][0] = static_cast<float>(x * c - y * s), wt.rot.m[r][1] = static_cast<float>(x * s + y * c);
    }
    if (!game::TrySetTransform(reinterpret_cast<game::Entity*>(ent), wt)) return Put(msg, cap, "bt probe gz_yaw: the transform write failed"), false;
    double after = before;
    EntYaw(ent, &after);
    g_o23 = Gz23{};
    g_o23.on = true, g_o23.actor = z->actor, g_o23.ent = ent, g_o23.loc = z->loc, g_o23.a = before, g_o23.b = after, g_o23.set_ms = I::NowMs();
    return Put(msg, cap, "bt probe gz_yaw: loc %08X %.1f -> %.1f (log next frame)", z->loc, before, after), true;
}
bool Op24(const double (&a)[5], char* msg, std::size_t cap) {
    if (!Need(kGHunt, "hunt_dump", msg, cap)) return false;
    HuntLines("");
    const double s = Arg(a, 1, 0);
    if (s > 0) g_o24 = Hunt24{}, g_o24.on = true, g_o24.end_ms = I::NowMs() + std::min(s, 300.0) * 1000.0, g_o24.next_ms = I::NowMs() + 250.0;
    return Put(msg, cap, "bt probe hunt_dump: logged%s", s > 0 ? " (every 250 ms while a hunt is up)" : ""), true;
}
bool Op25(const double (&a)[5], char* msg, std::size_t cap) {
    if (!Need(kGHunt, "hunt_self", msg, cap)) return false;
    if (ride::Carried()) return Put(msg, cap, "bt probe hunt_self: refused during a carry"), false;
    if (g_o25.on) return Put(msg, cap, "bt probe hunt_self: the last one is still watched"), false;
    const double v = a[1];
    if (!(v == 0 || v == 1)) return Put(msg, cap, "bt probe hunt_self: variant 0 or 1"), false;
    const uptr hm = Hm();
    double me[3];
    if (!hm || !mem::Q(hm + kHmRes)) return Put(msg, cap, "bt probe hunt_self: no hunter manager or no resource (+0x138)"), false;
    if (!I::LocalPos(me)) return Put(msg, cap, "bt probe hunt_self: no local player"), false;
    if (!HookIn(kHHuntEnd, msg, cap)) return false;
    const uptr area0 = mem::Q(hm + kHmArea);
    g_o25 = Hunt25{};
    g_o25.on = true, g_o25.variant = static_cast<int>(v), g_o25.start_ms = I::NowMs(), g_o25.next_ms = g_o25.start_ms + 250.0;
    g_o25.end_ms = g_o25.start_ms + 30000.0;
    g_hunt_watch.store(true);
    uptr ent = 0;
    const bool ok = SehHunterArea(hm, me, &ent);
    const uptr area1 = mem::Q(hm + kHmArea);
    Log("[bt] hunt self at Sam: FUN_1429B8D60 %s 0x%llX, +0x140 0x%llX -> 0x%llX (variant %d)", ok ? "returned" : "FAULTED", H(ent), H(area0), H(area1), g_o25.variant);
    if (!ok) return HuntSelfEnd("the call faulted"), Put(msg, cap, "bt probe hunt_self: the call faulted"), false;
    if (g_o25.variant == 1) {
        const uptr ae = area1 > 0x20 ? area1 - 0x20 : 0;
        const uptr crowd = ae ? partner::FindComponentByName(reinterpret_cast<game::Entity*>(ae), "DSHunterCrowdComponent") : 0;
        u32 f = 0;
        const bool rd = crowd && mem::Rd(crowd + 0x8B8, f);
        const bool wr = rd && Wr<u32>(crowd + 0x8B8, f & ~1u);
        Log("[bt] hunt self v1: crowd 0x%llX +0x8B8 0x%X -> 0x%X (%s)", H(crowd), f, f & ~1u, wr ? "written" : "NOT written: no crowd component");
    }
    return Put(msg, cap, "bt probe hunt_self: variant %d (log)", g_o25.variant), true;
}
bool Op26(const double (&a)[5], char* msg, std::size_t cap) {
    if (!Need(kGHunt, "tar_record", msg, cap)) return false;
    if (ride::Carried()) return Put(msg, cap, "bt probe tar_record: refused during a carry"), false;
    if (g_o26.on) return Put(msg, cap, "bt probe tar_record: the last one is still in"), false;
    const double m = std::isfinite(a[1]) ? a[1] : 0, radius = Arg(a, 2, 5), list = a[3];
    if (!(m >= 0 && m <= 50) || !(radius > 0 && radius <= 50) || !(list == 0 || list == 1))
        return Put(msg, cap, "bt probe tar_record: m 0..50, radius 1..50, list 0 (+0xD0 meshes) or 1 (+0x70 circles)"), false;
    const uptr hm = Hm();
    if (!hm || !mem::Q(hm + kHmRes)) return Put(msg, cap, "bt probe tar_record: no hunter manager or no resource (+0x138)"), false;
    double me[3], fx = 0, fy = 0;
    if (!I::SamFacing(me, &fx, &fy) || !R::AheadOf(me, fx, fy, m, g_tar.pos)) return Put(msg, cap, "bt probe tar_record: no local player transform"), false;
    g_tar.radius = static_cast<float>(radius), g_tar.height = 2.0f;
    g_o26 = Tar26{};
    g_o26.list = static_cast<int>(list), g_o26.hm = hm;
    char why[96];
    g_o26.fact_hooked = (Check() & kGTar) && HookIn(kHInTar, why, sizeof(why));
    g_o26.intar0 = g_intar_calls.load();
    int n = -1;
    const int rc = SehTarList(hm, g_o26.list ? kHmCircN : kHmMeshN, g_o26.list ? kHmCirc : kHmMesh, reinterpret_cast<uptr>(&g_tar), true, &n);
    if (rc != 1) {
        Log("[bt] tar rec: not added (%s, count %d)", rc < 0 ? "FAULTED" : "the list is full (10)", n);
        return Put(msg, cap, "bt probe tar_record: not added (log)"), false;
    }
    g_o26.on = g_o26.in = true, g_o26.start_ms = I::NowMs(), g_o26.next_ms = g_o26.start_ms;
    char p[64];
    FmtPos(p, sizeof(p), g_tar.pos);
    Log("[bt] tar rec in: list %d (%s %d/10) at (%s) r %.1f h 2.0, record 0x%llX", g_o26.list, g_o26.list ? "+0x70" : "+0xD0", n, p, radius,
        H(reinterpret_cast<uptr>(&g_tar)));
    return Put(msg, cap, "bt probe tar_record: in for 30 s (log)"), true;
}
bool Op29(const double (&a)[5], char* msg, std::size_t cap) {
    if (!Need(kGVoid, "blast", msg, cap)) return false;
    if (!I::VoidOk()) return Put(msg, cap, "bt probe blast: off (bt_probe 11's byte checks; log)"), false;
    if (g_o29.on) return Put(msg, cap, "bt probe blast: the last one is still up"), false;
    if (g_blast_dead) return Put(msg, cap, "bt probe blast: the ref call faulted before (not called again)"), false;
    const double m = Arg(a, 1, 600), fx = a[2];
    if (!(fx >= 0 && fx <= 3 && fx == std::floor(fx)) || !(m >= 0 && m <= 3000)) return Put(msg, cap, "bt probe blast: m 0..3000, fx 0..3"), false;
    if (fx >= 1 && (!Need(kGFx, "blast fx", msg, cap) || !HookIn(kHFx, msg, cap))) return false;
    if (!I::FlowGate(msg, cap)) return false;
    const uptr mgr = GzMgr(), res = mgr ? mem::Q(mgr + kMgrRes) : 0, world = mem::Q(A(kWorldGlobal));
    if (!res || !world) return Put(msg, cap, "bt probe blast: no gazer manager resource (+0x68) or no world"), false;
    double me[3], f0 = 0, f1 = 0;
    BlastXf xf{};
    if (!I::SamFacing(me, &f0, &f1) || !R::AheadOf(me, f0, f1, m, xf.pos)) return Put(msg, cap, "bt probe blast: no local player transform"), false;
    const float rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::memcpy(xf.rot, rot, sizeof(rot));
    const float far_m = RdF(res + 0x54);
    const bool is_far = far_m > 0 && R::Dist3(me, xf.pos) >= far_m;
    const uptr er = mem::Q(res + (is_far ? 0x30 : 0x28));
    if (!er) return Put(msg, cap, "bt probe blast: no annihilation resource"), false;
    g_fx_mode.store(static_cast<int>(fx)), g_fx_saved.store(false), g_fx_inst.store(0), g_fx_seen.store(0);
    g_fx_window.store(fx >= 1, std::memory_order_release);
    g_o29 = Blast29{};
    g_o29.fx = static_cast<int>(fx), g_o29.flow0 = I::FlowCalls();
    uptr e = 0;
    const bool ok = SehBlast(er, &xf, world, &e);
    char p[64];
    FmtPos(p, sizeof(p), xf.pos);
    if (!ok || !e) {
        g_fx_window.store(false);
        FxRestore();
        Log("[bt] blast at (%s) m %.0f: %s", p, m, ok ? "no entity" : "FAULTED");
        return Put(msg, cap, "bt probe blast: %s", ok ? "no entity" : "faulted"), false;
    }
    g_blast_ref[0] = e + 0x20, g_blast_ref[1] = 0, g_blast_ref[2] = 0;
    g_o29.linked = SehRef(A(kRefAdd), g_blast_ref);
    if (!g_o29.linked) g_blast_dead = true, Log("[bt] blast: the ref add FUN_14191C790 FAULTED (no ref held; no more blasts)");
    const double now = I::NowMs();
    g_o29.on = true, g_o29.ent = e, g_o29.start_ms = now, g_o29.next_ms = now + 500.0, g_o29.fx_end_ms = now + 9000.0, g_o29.end_ms = now + 16000.0;
    Log("[bt] blast at (%s) m %.0f: entity 0x%llX (%s art), flow calls %u, fx %d", p, m, H(e), is_far ? "far" : "near", I::FlowCalls() - g_o29.flow0, g_o29.fx);
    return Put(msg, cap, "bt probe blast: %.0f m ahead, fx %d (log for 16 s)", m, g_o29.fx), true;
}
bool Op30(const double (&a)[5], char* msg, std::size_t cap) {
    if (!Need(kGCrater, "crater_add", msg, cap)) return false;
    const double m = std::isfinite(a[1]) ? a[1] : 0, radius = Arg(a, 2, 107), flags = a[3];
    if (!(m >= 0 && m <= 3000) || !(radius > 0 && radius <= 500) || !(flags >= 0 && flags <= 3 && flags == std::floor(flags)))
        return Put(msg, cap, "bt probe crater_add: m 0..3000, radius 1..500, flags 0..3 (bit 0 IsDisableCrater, bit 1 remove)"), false;
    const uptr cm = Cm();
    if (!cm) return Put(msg, cap, "bt probe crater_add: no catcher manager"), false;
    double me[3], fx = 0, fy = 0;
    CraterIn in{};
    if (!I::SamFacing(me, &fx, &fy) || !R::AheadOf(me, fx, fy, m, in.pos)) return Put(msg, cap, "bt probe crater_add: no local player transform"), false;
    in.radius = static_cast<float>(radius), in.flags = static_cast<u32>(flags), in.yaw = Rand01() * 6.2831853f;
    int gen = 0, idx = -1;
    const int rc = SehCraterAppend(cm, &in, &gen, &idx);
    char p[64];
    FmtPos(p, sizeof(p), in.pos);
    if (rc != 0) {
        Log("[bt] crater add at (%s): %s", p, rc == 2 ? "the pending list did not grow (not added)" : "FAULTED");
        return Put(msg, cap, "bt probe crater_add: %s", rc == 2 ? "not added" : "faulted"), false;
    }
    g_o30 = Crater30{};
    g_o30.on = true, g_o30.gen = gen, g_o30.start_ms = I::NowMs();
    Log("[bt] crater add: pending, gen %d (pending %d) at (%s) %.0f m ahead r %.0f flags %u", gen, idx, p, m, radius, in.flags);
    return Put(msg, cap, "bt probe crater_add: pending gen %d (log at +9 / +12 s)", gen), true;
}
bool Op32(const double (&a)[5], char* msg, std::size_t cap) {
    if (!Need(kGCat, "cat_log", msg, cap)) return false;
    for (int h : {kHCatNotify, kHCatCrater, kHDrop, kHDropList, kHRespawn})
        if (!HookIn(h, msg, cap)) return false;
    g_o32 = Cat32{};
    g_o32.on = true, g_o32.end_ms = I::NowMs() + std::min(Arg(a, 1, 60), 900.0) * 1000.0;
    g_cat_log.store(true);
    Log("[bt] cat log on for %.0f s", Arg(a, 1, 60));
    return Put(msg, cap, "bt probe cat_log: on for %.0f s (log)", Arg(a, 1, 60)), true;
}
bool Op33(const double (&a)[5], char* msg, std::size_t cap) {
    if (!Need(kGCat, "cat_bring", msg, cap)) return false;
    const double cmd = a[1];
    if (!(cmd == 0 || cmd == 1 || cmd == 4)) return Put(msg, cap, "bt probe cat_bring: cmd 0 (bring), 1 (kill), 4 (force deactivate)"), false;
    if (!HookIn(kHCatNotify, msg, cap)) return false;
    const uptr cm = Cm();
    g_o33 = Cat33{};
    g_o33.cmd = static_cast<int>(cmd), g_o33.n0 = cm ? RdI(cm + kCmCount) : -1;
    g_notify_watch.store(true);
    if (!SehU8(A(kCatCmdFn), static_cast<u8>(cmd))) {
        g_notify_watch.store(false);
        Log("[bt] cat bring: FUN_142A928C0(%d) FAULTED", g_o33.cmd);
        return Put(msg, cap, "bt probe cat_bring: the call faulted"), false;
    }
    g_o33.on = true, g_o33.start_ms = I::NowMs();
    Log("[bt] cat bring: FUN_142A928C0(%d) called, records %d", g_o33.cmd, g_o33.n0);
    return Put(msg, cap, "bt probe cat_bring: cmd %d (dumps at +1 / +5 s)", g_o33.cmd), true;
}
bool Op34(const double (&a)[5], char* msg, std::size_t cap) {
    if (!Need(kGSky, "bt_sky", msg, cap)) return false;
    if (g_o34.on) return Put(msg, cap, "bt probe bt_sky: already on"), false;
    const double s = Arg(a, 1, 10);
    if (!(s > 0 && s <= 120)) return Put(msg, cap, "bt probe bt_sky: s 1..120"), false;
    const uptr ws = Ws();
    if (!ws) return Put(msg, cap, "bt probe bt_sky: no weather system"), false;
    g_o34 = Sky34{};
    g_o34.on = true, g_o34.start_ms = I::NowMs(), g_o34.end_ms = g_o34.start_ms + s * 1000.0, g_o34.next_ms = g_o34.start_ms + 1000.0;
    Log("[bt] sky on for %.0f s: 6508 %u 6518 0x%X", s, RdB(ws + kWs6508), RdU(ws + kWsSky));
    return Put(msg, cap, "bt probe bt_sky: %.0f s (log)", s), true;
}
bool Op35(char* msg, std::size_t cap) {
    if (!Need(kGVoid, "void_real", msg, cap)) return false;
    if (!I::VoidOk()) return Put(msg, cap, "bt probe void_real: off (bt_probe 11's byte checks; log)"), false;
    // Solo only (design 641): with bt_sync's voidout hook in, this call would be recorded and relayed to the partner as a VOID
    // from an unknown caller (the return address is in this DLL).
    if (I::VoidHookIn()) return Put(msg, cap, "bt probe void_real: refused with bt_sync's voidout hook in (solo only)"), false;
    if (ride::Carried()) return Put(msg, cap, "bt probe void_real: refused during a carry"), false;
    if (g_o35.on) return Put(msg, cap, "bt probe void_real: already ran"), false;
    if (!I::FlowGate(msg, cap)) return false;
    const uptr mgr = GzMgr();
    double me[3];
    if (!mgr || !mem::Q(mgr + kMgrRes)) return Put(msg, cap, "bt probe void_real: no gazer manager resource (+0x68)"), false;
    if (!I::LocalPos(me)) return Put(msg, cap, "bt probe void_real: no local player"), false;
    EndAll("the real voidout", true);
    g_o35 = Void35{};
    g_o35.flow0 = I::FlowCalls();
    Log("[bt] void real: the voidout at Sam (the game's own GameOver and respawn follow; the last step of a run)");
    if (!SehVoid(mgr, me, 1)) {
        Log("[bt] void real: FAULTED");
        return Put(msg, cap, "bt probe void_real: faulted"), false;
    }
    const unsigned flow = I::FlowCalls() - g_o35.flow0;
    Log("[bt] void real: flow %s (flow calls %u)", flow ? "ran" : "NOT called", flow);
    g_o35.on = true, g_o35.start_ms = I::NowMs();
    return Put(msg, cap, "bt probe void_real: flow calls %u (dumps after the respawn)", flow), true;
}
// stage 0 (run 20261127-210103, op 36 "the local player id is unknown"): its own read of the local player id, as bt_world::PlayerId
// (the gazer update's registry [0x147BBB8C0] vt+0x40(reg, out) -> a pointer to the id; bt_world's checks of that call, bit
// kBitPlayer), with no bt_sync. The local Sam's id is 0 (bt_sync's "me 00000" lines), so 0 is a real id; 0x1FFFFF (none) is not.
constexpr uptr kRegGlobal = 0x147BBB8C0;
bool g_pid_dead = false;
const u32* SehPlayerId(uptr fn, uptr reg, void* buf, bool* fault) noexcept {
    __try {
        return reinterpret_cast<const u32* (*)(uptr, void*)>(fn)(reg, buf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = true;
        return nullptr;
    }
}
bool LocalPlayerId(u32* out, char* why, std::size_t cap) noexcept {
    if (g_pid_dead) return Put(why, cap, "the player id call faulted before (not called again)"), false;
    if (!(btworld::Check() & btworld::kBitPlayer)) return Put(why, cap, "bt_world's player id checks failed (log)"), false;
    const uptr reg = mem::Q(A(kRegGlobal));
    const uptr vt = reg ? mem::Q(reg) : 0;
    const uptr fn = vt ? mem::Q(vt + 0x40) : 0;
    if (!fn || !mem::InText(fn)) return Put(why, cap, "registry 0x%llX vt 0x%llX: vt+0x40 0x%llX is not code", H(reg), H(vt), H(fn)), false;
    alignas(16) u8 buf[64] = {};
    bool fault = false;
    const u32* p = SehPlayerId(fn, reg, buf, &fault);
    if (fault) return g_pid_dead = true, Put(why, cap, "the registry's vt+0x40 call FAULTED"), false;
    u32 v = 0;
    if (!p || !mem::Rd(reinterpret_cast<uptr>(p), v)) return Put(why, cap, "the call returned 0x%llX (unreadable)", H(reinterpret_cast<uptr>(p))), false;
    if ((v & 0x1FFFFFu) == R::kActorNone) return Put(why, cap, "the id reads 0x%X (none)", v), false;
    *out = v;
    return true;
}
bool Op36(const double (&a)[5], char* msg, std::size_t cap) {
    if (!Need(kGStats, "kill_look", msg, cap)) return false;
    if (!I::GazerKillOk()) return Put(msg, cap, "bt probe kill_look: off (bt_probe 10's byte checks; log)"), false;
    if (g_o36.on) return Put(msg, cap, "bt probe kill_look: the last one is still watched"), false;
    const double code = a[3];
    if (!(code >= 0 && code <= 3 && code == std::floor(code)))
        return Put(msg, cap, "bt probe kill_look: reason code 0 (0x01322187 rain gone), 1 (0x74FB05EE), 2 (0x523D4DC0), 3 (0x5414860D crater)"), false;
    char why[96];
    const I::GzInfo* z = PickGazer(a[1], a[2], kPickKillable, why, sizeof(why));
    if (!z) return Put(msg, cap, "bt probe kill_look: %s", why), false;
    u32 me = 0;
    char pwhy[128] = "";
    if (!LocalPlayerId(&me, pwhy, sizeof(pwhy))) return Put(msg, cap, "bt probe kill_look: the local player id is unknown: %s", pwhy), false;
    if (!I::InstallGz(msg, cap)) return false;
    g_o36 = Kill36{};
    g_o36.reason = kReasonCodes[static_cast<int>(code)], g_o36.actor = z->actor, g_o36.id = z->id, g_o36.loc = z->loc;
    g_o36.s_ok = StatRead(0x79, &g_o36.s79) && StatRead(0x7A, &g_o36.s7a);
    if (!btworld::ArmKill(z->actor, z->id, R::kTagProbe, g_o36.reason, me ? me : btworld::kKillerZero))
        return Put(msg, cap, "bt probe kill_look: not armed (a kill slot is busy)"), false;
    g_o36.on = true, g_o36.start_ms = I::NowMs();
    Log("[bt] kill look loc %08X reason 0x%08X killer %08X: armed (stats 0x79 %u 0x7A %u%s)", z->loc, g_o36.reason, me, g_o36.s79, g_o36.s7a,
        g_o36.s_ok ? "" : ", unreadable");
    return Put(msg, cap, "bt probe kill_look: loc %08X reason 0x%08X (log at +5 s)", z->loc, g_o36.reason), true;
}

// ================================================================ the Tick's timed ops
void TickOps(double now) {
    if (g_o15.on) {
        if (now >= g_o15.end_ms) g_o15.on = false, Log("[bt] wx dump done");
        else if (now >= g_o15.next_ms) g_o15.next_ms = now + 5000.0, DumpWx(" +");
    }
    if (g_o16.armed && now >= g_o16.read_ms) g_o16.armed = false, ThreadsRead();
    if (g_o18.on) {
        if (now >= g_o18.end_ms) {
            g_o18.on = false, g_o18.restoring = true, g_o18.restore_ms = now;
            g_wx_mode.store(2, std::memory_order_release);
            WxApplyLine(now);
            Log("[bt] wx apply: restoring (the job regenerates region %d from its own type)", g_o18.region);
        } else if (now >= g_o18.next_ms) {
            g_o18.next_ms = now + 5000.0;
            WxApplyLine(now);
        }
    }
    if (g_o18.restoring) {
        const uptr ws = Ws();
        if (g_wx_mode.load() == 2 && now - g_o18.restore_ms > 3000.0 && ws) {  // the job did not run: write it from here when it is idle
            const uptr job = mem::Q(ws + kWsJob);
            u32 st = 1;
            if (job && mem::Rd(job + 0x28, st) && (st == 0 || st == kJobDone) && WxRestore(ws)) g_wx_mode.store(0), Log("[bt] wx apply: restore written from the Tick");
        }
        if (g_wx_mode.load() == 0 && !g_o18.done_ms) g_o18.done_ms = now + 2000.0;
        if (g_o18.done_ms && now >= g_o18.done_ms) {
            g_o18.restoring = false, g_o18.done_ms = 0;
            const std::int32_t slot = ws ? RdI(ws + kWsSlot) : -1;
            Log("[bt] wx apply restored: eff0 %u (own forecast; type %d, restores %u)", ws ? Eff0(ws, g_o18.region) : 0xFF,
                ws && slot >= 0 && slot <= 3 ? RdI(RowAt(ws, slot, g_o18.region) + kRowType) : -1, g_wx_restores.load());
        }
    }
    if (g_o19.on) {
        GuardRearm();
        if (g_o19.mode == 1 && now >= g_o19.refresh_ms && now < g_o19.refresh_end_ms) g_o19.refresh_ms = now + 2000.0, g_o19.refreshes += SpotNow() ? 1 : 0;
        if (now >= g_o19.next_ms) g_o19.next_ms = now + 500.0, SpotLine(now);
        if (now - g_o19.start_ms >= 40000.0) SpotEnd("40 s");
    }
    if (g_o20.on) {
        if (now >= g_o20.end_ms) g_o20.on = false, Log("[bt] gz dump done");
        else if (now >= g_o20.next_ms) g_o20.next_ms = now + 1000.0, DumpGz(now);
    }
    if (g_o21.on) {
        Census(now);
        if (now >= g_o21.end_ms) TraceEnd("time");
    }
    if (g_o22.on) {
        if (g_mir_lost.exchange(false)) MirrorEnd(StaleText(g_mir_lost_why.load()), kEndNoWrite);
        else if (now >= g_o22.next_ms) g_o22.next_ms = now + 1000.0, MirrorLine(now);
        if (g_o22.on && now - g_o22.start_ms >= 20000.0) MirrorEnd("20 s", kEndCheck);
    }
    MirrorBack(now);
    if (g_o23.on && now > g_o23.set_ms) {
        g_o23.on = false;
        double c = -999;
        const bool ok = EntYaw(g_o23.ent, &c);
        Log("[bt] gz yaw loc %08X: entity yaw %.1f -> %.1f, next frame %s%.1f, actor +0x28 %.1f", g_o23.loc, g_o23.a, g_o23.b, ok ? "" : "(unreadable) ", c,
            ActorYaw(g_o23.actor));
    }
    if (g_o24.on) {
        if (now >= g_o24.end_ms) g_o24.on = false, Log("[bt] hunt dump done: %d samples with a hunt up", g_o24.samples);
        else if (now >= g_o24.next_ms) {
            g_o24.next_ms = now + 250.0;
            if (HuntUp()) ++g_o24.samples, HuntLines(" +");
            else if (now >= g_o24.quiet_ms) g_o24.quiet_ms = now + 1000.0, Log("[bt] hunt +: none up");
        }
    }
    if (g_o25.on) {
        if (g_o25.variant == 1 && now >= g_o25.next_ms) g_o25.next_ms = now + 250.0, HuntSelfLine(now);
        if (now >= g_o25.end_ms) HuntSelfEnd("30 s");
        else if (g_o25.stop_ms && now >= g_o25.stop_ms) HuntSelfEnd("the hunt ended");
    }
    if (g_o26.on) {
        if (now >= g_o26.next_ms) g_o26.next_ms = now + 100.0, TarLine(now);
        if (now - g_o26.start_ms >= 30000.0) TarOut("30 s");
        else if (g_o26.on && Hm() != g_o26.hm) TarOut("the hunter manager changed");
    }
    if (g_o29.on) {
        if (g_o29.fx == 1 && now >= g_o29.next_ms) {
            g_o29.next_ms = now + 500.0;
            const uptr inst = g_fx_inst.load();
            if (inst) Log("[bt] fx effector inst 0x%llX +%.1f s: w %.3f", H(inst), (now - g_o29.start_ms) / 1000.0, RdF(inst + 0x68));
        }
        if (now >= g_o29.fx_end_ms && g_fx_window.load()) {
            g_fx_window.store(false);
            Log("[bt] fx window closed: %u effector calls, matched inst 0x%llX", g_fx_seen.load(), H(g_fx_inst.load()));
            FxRestore();
        }
        if (now >= g_o29.end_ms) BlastEnd("16 s");
    }
    if (g_o30.on) {
        const double t = now - g_o30.start_ms;
        if (!g_o30.l9 && t >= 9000.0) g_o30.l9 = true, CraterLine("+9 s");
        if (t >= 12000.0) g_o30.on = false, CraterLine("+12 s");
    }
    if (g_o32.on && now >= g_o32.end_ms) CatLogEnd("time");
    if (g_o33.on) {
        const double t = now - g_o33.start_ms;
        if (!g_o33.d1 && t >= 1000.0) g_o33.d1 = true, DumpCat(" +1s");
        if (t >= 5000.0) {
            g_o33.on = false, g_notify_watch.store(false);
            DumpCat(" +5s");
            const uptr cm = Cm();
            const int n = cm ? RdI(cm + kCmCount) : -1;
            const uptr data = cm ? mem::Q(cm + kCmData) : 0;
            Log("[bt] cat bring: record %d (was %d), type %u, notify 0x63db9ed5 seen %s", n, g_o33.n0, data && n > 0 ? RdB(data + static_cast<uptr>(n - 1) * kCatRecSize + 0x28) : 0xFFu,
                YesNo(g_o33.seen));
        }
    }
    if (g_o34.on) {
        g_o34.calls += SehU8(A(kSkyFn), 1) ? 1 : 0;
        if (now >= g_o34.next_ms) {
            g_o34.next_ms = now + 1000.0;
            const uptr ws = Ws();
            Log("[bt] sky +%.0f: 6508 %u 6518 0x%X", (now - g_o34.start_ms) / 1000.0, ws ? RdB(ws + kWs6508) : 0xFFu, ws ? RdU(ws + kWsSky) : 0u);
        }
        if (now >= g_o34.end_ms) SkyEnd("time");
    }
    if (g_o36.on && now - g_o36.start_ms >= 5000.0) {
        g_o36.on = false;
        u32 a79 = 0, a7a = 0;
        const bool ok = g_o36.s_ok && StatRead(0x79, &a79) && StatRead(0x7A, &a7a);
        Log("[bt] kill look loc %08X reason 0x%08X: death anim (screenshot), stats d %s%d / %d (0x79 %u -> %u, 0x7A %u -> %u), event %s", g_o36.loc, g_o36.reason,
            ok ? "" : "(unreadable) ", static_cast<int>(a79 - g_o36.s79), static_cast<int>(a7a - g_o36.s7a), g_o36.s79, a79, g_o36.s7a, a7a,
            YesNo(g_o36.event));
    }
}
// review (semantics 4): every path ends with one "[bt] void real: done (<how>)" line within 150 s, so the scenario waits on that
// line alone (a failed optional step fails the run too).
void Void35Done(const char* how) {
    g_o35.on = false;
    Log("[bt] void real: %s:", how);
    DumpCraters(" (void real)");
    DumpWx(" (void real)");
    Log("[bt] void real: done (%s, flow calls %u)", how, I::FlowCalls() - g_o35.flow0);
}
void Tick35(double now) {
    if (!g_o35.on) return;
    if (!g_o35.loaded) {
        if (I::LoadingNow()) g_o35.loaded = true, Log("[bt] void real: the load began (+%.1f s)", (now - g_o35.start_ms) / 1000.0);
        else if (now - g_o35.start_ms > 120000.0) Void35Done("no load within 120 s");
        return;
    }
    if (!g_o35.back) {
        double me[3];
        if (!I::LoadingNow() && I::LocalPos(me)) g_o35.back = true, g_o35.back_ms = now, Log("[bt] void real: Sam is back (+%.1f s)", (now - g_o35.start_ms) / 1000.0);
        else if (now - g_o35.start_ms > 150000.0) Void35Done("Sam not back within 150 s");
        return;
    }
    if (now - g_o35.back_ms >= 5000.0) Void35Done("after the respawn");
}

}  // namespace

// ================================================================ the public side
bool Command(int op, const double (&a)[5], char* msg, std::size_t cap) noexcept {
    g_tick_tid.store(GetCurrentThreadId());
    if (!automation::Enabled()) return Put(msg, cap, "bt probe %d: hidden runs only", op), false;
    if (g_es.load() || g_pass.load()) return Put(msg, cap, "bt probe %d: stopped (emergency stop)", op), false;
    static constexpr bool kSandboxed[37] = {false, false, false, false, false, false, false, false, false, false, false, false, false,
                                            false, false, false, false, false, true,  true,  false, false, true,  true,  false, true,
                                            true,  false, false, true,  true,  false, false, true,  true,  true,  true};
    if (op < 15 || op > 36) return Put(msg, cap, "bt probe: op %d is not a round 31 op", op), false;
    if (kSandboxed[op] && !automation::SavesRedirected()) return Put(msg, cap, "bt probe %d: only on a sandboxed save (a hidden run)", op), false;
    if (op != 35 && I::LoadingNow()) return Put(msg, cap, "bt probe %d: not during a load", op), false;
    Check();
    switch (op) {
        case 15: return Op15(a, msg, cap);
        case 16: return Op16(a, msg, cap);
        case 17: return Need(kGHash | kGWx, "wx_hash", msg, cap) && OpHash(msg, cap);
        case 18: return Op18(a, msg, cap);
        case 19: return Op19(a, msg, cap);
        case 20: return Op20(a, msg, cap);
        case 21: return Op21(a, msg, cap);
        case 22: return Op22(a, msg, cap);
        case 23: return Op23(a, msg, cap);
        case 24: return Op24(a, msg, cap);
        case 25: return Op25(a, msg, cap);
        case 26: return Op26(a, msg, cap);
        case 27: return Put(msg, cap, "bt probe 27 (hunt_mirror): not built (stage 9 research)"), false;
        case 28:
            if (!Need(kGCrater, "crater_dump", msg, cap)) return false;
            DumpCraters("");
            return Put(msg, cap, "bt probe crater_dump: logged"), true;
        case 29: return Op29(a, msg, cap);
        case 30: return Op30(a, msg, cap);
        case 31:
            if (!Need(kGCat, "cat_dump", msg, cap)) return false;
            DumpCat("");
            return Put(msg, cap, "bt probe cat_dump: logged"), true;
        case 32: return Op32(a, msg, cap);
        case 33: return Op33(a, msg, cap);
        case 34: return Op34(a, msg, cap);
        case 35: return Op35(msg, cap);
        case 36: return Op36(a, msg, cap);
    }
    return false;
}

void Tick(double now_ms) noexcept {
    g_tick_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (!g_checked) return;  // no round 31 op ran: nothing to do
    if (g_es.load()) {
        g_es.store(false);
        EndAll("emergency stop", false);
        return;
    }
    DrainRing();
    if (I::LoadingNow()) EndAll("a load", true);
    else TickOps(now_ms);
    Tick35(now_ms);
}

void OnWorldEvent(const btworld::Event& e) noexcept {
    if (e.tag != R::kTagProbe || e.actor != g_o36.actor || !g_o36.actor) return;
    g_o36.event = true;
    Log("[bt] kill look event loc %08X: reason 0x%08X killer %08X applied %d edge %s flags 0x%X", g_o36.loc, e.reason, e.killer, e.applied, YesNo(e.edge), e.flags);
}

void EmergencyStop() noexcept {
    g_pass.store(true);
    g_trace_gz.store(false), g_cat_log.store(false), g_hunt_watch.store(false), g_notify_watch.store(false), g_fx_window.store(false);
    g_mir_actor.store(0), g_mir_comp.store(0);
    btworld::SetAfterUpdate(nullptr);
    g_es.store(true);  // a later Tick (if one ever runs) ends whatever this undo left
    // review (crash 1): after a tick fault the tick never runs again (dllmain), so the undo is done here, now, on the game thread
    // (inside GuardedStop's SEH): the forecast restore written directly (g_pass), the tar record out, the fade back and the blast
    // ref released, the sky bit cleared, the spot's guard page and VEH removed. Every end is idempotent (each checks its .on).
    if (g_checked) EndAll("emergency stop", false);
}

}  // namespace st::bt::r31
