// Shared real vehicles, game side (design milestone M2, stage 1). See vehicle_share.hpp and
// docs/real-partner-body.md section 23.
#include "vehicle_share.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

#include "MinHook.h"
#include "automation.hpp"
#include "coop_session.hpp"
#include "dev_rules.hpp"
#include "dev_tools.hpp"
#include "enemy_sync.hpp"
#include "game_actions.hpp"
#include "link.hpp"
#include "log.hpp"
#include "partner_source.hpp"
#include "pose_wire.hpp"
#include "real_body.hpp"
#include "vehicle_drive.hpp"
#include "vehicle_sync.hpp"

namespace st::vshare {
namespace {
namespace mem = partner::mem;
using game::WorldTransform;

static_assert(vwire::kChannel == link::kVehicleEventChannel, "vehicle events use link channel 3");

constexpr std::uintptr_t kImage = 0x140000000ull;  // the addresses below are ds.exe VAs at its preferred base
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

// ---------------------------------------------------------------- game addresses (ds.exe, Director's Cut)
constexpr std::uintptr_t kRegGlobal = 0x147BFECC0;    // R: the vehicle manager (pools at R+0x28 + i*8, i = EVehicleType)
constexpr std::uintptr_t kParkGlobal = 0x147BF5798;   // PM: the parking manager (9 slots of 0x58 at PM+0xD0)
constexpr std::uintptr_t kEnqueue = 0x142F654E0;      // pool enqueue (pool, 0x68 request): every runtime spawn and online vehicle
constexpr std::uintptr_t kGarageSpawn = 0x142D5CB20;  // garage / parking slot spawn (PM, slot, xf) -> 0x142F830A0
constexpr std::uintptr_t kSpawn = 0x142F830A0;        // spawn (R, record, xf, u8 p4, owner, u8 p6): builds the request, enqueues
constexpr std::uintptr_t kExactSpawn = 0x142DCFFB0;   // pool vfunc +0x30 (pool, desc, id): spawn into exactly that free unit
constexpr std::uintptr_t kWriteEntry = 0x142F7CF20;   // (unit, 0x74 entry out): the persisted entry of a unit (save writer)
constexpr std::uintptr_t kVictim = 0x142F7DB10;       // the eviction victim chooser (pool) -> unit or 0
constexpr std::uintptr_t kTeleport = 0x142F7CA20;     // vehicle RequestTeleport (entity, xf, u8, u8): deferred
constexpr std::uintptr_t kCurArea = 0x14262A860;      // the current area id (u16)
constexpr std::uintptr_t kEmptyString = 0x144562CB0;  // the game's empty string (a static header at -0x10)
constexpr std::uintptr_t kUnitXf = 0x141A87230;       // unit vfunc +0x18: lea rax, [rcx + 0x28]
constexpr std::uintptr_t kPoolVt[4] = {0, 0x143E278E0, 0x143E26B98, 0x143E261C0};    // by kind: roadster, truck, trike
constexpr std::uintptr_t kKindGetter[4] = {0, 0x141925D10, 0x141921DC0, 0x141921EF0};  // pool vfunc +0x150: mov eax, kind
constexpr std::uintptr_t kUnitVt[3] = {0x143E26070, 0x143E26A48, 0x143E27790};       // the three GameActorUnit vtables
constexpr std::uintptr_t kVehVt[4] = {0, 0x143E274D8, 0x143E26790, 0x143E25DB8};     // vehicle entities by kind

// unit (GameActorUnit) fields
constexpr std::uintptr_t kUId = 0x08, kUInUse = 0x20, kUState = 0x24, kUXf = 0x28, kUEntRef = 0x68, kULife = 0x150, kUBattery = 0x154,
                         kURider = 0x158, kUGuid = 0x188, kUFlags = 0x198, kUFlagBytes = 0x19C, kURec2 = 0x1A2, kUArea = 0x1A4;
constexpr std::size_t kUBlock = 0x1B0;
constexpr std::uint32_t kRemoveBit = 0x2000;
// vehicle entity fields
constexpr std::uintptr_t kEUnit = 0x8A8, kELife = 0x6E8;
// the 0x68 spawn request
constexpr std::uintptr_t kReqName = 0x40, kReqRec2 = 0x54, kReqRec14 = 0x58, kReqFlags = 0x60;
// parking manager slots
constexpr std::uintptr_t kPmSlots = 0xD0, kPmSlotSize = 0x58;
constexpr int kPmSlotCount = 9;

enum Group : unsigned {
    kCore = 1u,      // the registry, pools, unit layout, the removal bit, the area
    kGate = 2u,      // the enqueue detour and the request flags
    kSpawnG = 4u,    // 0x142F830A0 and its record layout
    kEvict = 8u,     // the victim chooser and its three callers
    kTeleportG = 16u,
    kExact = 32u,    // the exact-id mirror (pool vfunc +0x30, the loader's desc) and the entry writer (optional)
    kGarage = 64u,   // the garage slot spawn (optional: guest requests go as plain spawns without it)
    kTrucks = 128u,  // round 23 D: the driver seat writes the MULE truck rules read (optional: vehicle_mule_log / _hold off without them)
};
constexpr unsigned kRequired = kCore | kGate | kSpawnG | kEvict | kTeleportG;
struct Site {
    std::uintptr_t va;
    const char* sig;
    const char* name;
    unsigned group;
};
// clang-format off
constexpr Site kSites[] = {
    {0x142F7509C, "48 8B 35 1D 9C C8 04", "vehicle manager global (register)", kCore},
    {0x142F6C86F, "48 8B 7C F7 28", "pools at manager+0x28", kCore},
    {0x142DCFFB0, "48 89 5C 24 18 57 48 83 EC 20 48 8B 41 48 48 8B F9 48 63 49 40 41 8B D8 4C 8B C2 48 8D 14 C8", "pool units +0x48 / count +0x40", kCore},
    {0x142DCFFD7, "80 79 20 00 75 05 39 59 08", "unit in-use +0x20 / id +0x08", kCore},
    {0x142F69A70, "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48", "unit release", kCore},
    {0x142F69A9D, "F7 83 98 01 00 00 00 20 00 00", "unit release: removal bit 0x2000 at +0x198", kCore},
    {0x142F69AA9, "48 8B 43 68 48 85 C0", "unit entity ref +0x68", kCore},
    {0x142FA7AB1, "81 88 98 01 00 00 00 20 00 00", "the game's removal request (or [unit+0x198], 0x2000)", kCore},
    {0x142F761A9, "48 8B 81 A8 08 00 00", "vehicle entity +0x8A8 = its unit", kCore},
    {0x141A87230, "48 8D 41 28 C3", "unit transform +0x28", kCore},
    {0x14262A860, "48 83 EC 28 48 8B 05 ?? ?? ?? ?? 48 85 C0 74 12 80 B8 A8 00 00 00 00 74 09 0F B7 40 48", "current area", kCore},
    {0x142F7DC53, "80 7B 20 00", "victim chooser: in-use", kCore},
    {0x142F7DC59, "F6 83 9E 01 00 00 08", "victim chooser: reserved +0x19E & 8", kCore},
    {0x142F7DC62, "81 BB 58 01 00 00 FF FF 1F 00", "victim chooser: rider +0x158", kCore},
    {0x142F6DA80, "48 89 5C 24 20 57 48 83 EC 20 48 8B DA 48 8B F9 E8 ?? ?? ?? ?? 48 8B 43 48 48 85 C0 74 0D C5 F8 10 40 08 C5 F8 11 87 88 01 00 00 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 66 89 87 A4 01 00 00", "unit spawn: GUID +0x188, area +0x1A4", kCore},
    {kEnqueue,    "40 53 57 48 83 EC 48 48 8B 05 42 BB FF 01 48 33", "pool enqueue", kGate},
    {0x142F98FF0, "43 F7 84 2E A0 02 00 00 00 08 00 00", "queue: online flag 0x800 at request +0x60", kGate},
    {kSpawn,      "40 55 53 56 57 41 56 41 57 48 8D 6C 24 E1 48 81", "spawn from a record", kSpawnG},
    {0x142F830FC, "48 8D 05 ?? ?? ?? ??", "spawn: the empty string", kSpawnG},
    {0x142F8315E, "48 8D 57 08 48 8D 4D D7 C5 F8 77 E8", "spawn: record +8 name", kSpawnG},
    {0x142F8316E, "48 8D 57 10 48 8D 4D E7 E8", "spawn: record +0x10 handle", kSpawnG},
    {0x142F8317B, "0F B7 0F 0F B7 47 02 8B D1 66 89 45 EB 44 8B C1 8B 47 14", "spawn: record +0 / +2 / +0x14", kSpawnG},
    {0x142F831BC, "25 08 CC FF FF", "spawn: request flags", kSpawnG},
    {kVictim,     "40 55 41 54 41 55 41 56 41 57 48 83 EC 70 48 8D", "eviction victim chooser", kEvict},
    {0x142F867BD, "E8 ?? ?? ?? ?? 48 85 C0 74 1D", "victim caller 1 handles 0", kEvict},
    {0x142F867FF, "E8 ?? ?? ?? ?? 48 85 C0 0F 84 47 FE FF FF", "victim caller 2 handles 0", kEvict},
    {0x142F993C9, "E8 ?? ?? ?? ?? 48 85 C0 0F 84 A5 02 00 00", "victim caller 3 handles 0", kEvict},
    {kTeleport,   "48 83 EC 28 C5 F8 10 02 C5 F8 11 81 78 09 00 00 C5 FB 10 4A 10 48 83 C2", "vehicle RequestTeleport", kTeleportG},
    {0x142F70660, "40 53 41 54 41 55 41 56 48 81 EC 28 01 00 00 48", "persistence loader", kExact},
    {0x142F70819, "C7 84 24 A0 00 00 00 B3 A5 91 4A", "loader desc type 0x4A91A5B3", kExact},
    {0x142F70893, "FF 50 30", "loader: pool vfunc +0x30", kExact},
    {kWriteEntry, "40 55 53 57 48 8D 6C 24 B9 48 81 EC B0 00 00 00 48 8B DA 48 8B F9 E8", "persisted entry writer", kExact},
    {kGarageSpawn, "40 53 56 57 48 81 EC 80 01 00 00 48 8B 05 FE 44", "garage slot spawn", kGarage},
    {0x142D6ECEA, "48 8B 1D ?? ?? ?? ??", "parking manager global", kGarage},
    // round 23 D [O]: FUN_142f7f810 writes the unit's rider +0x158 (also 0x1FFFFF), returns on none, +0x170, then +0x19D bit0 for a
    // player (type 0) and bit1 for anyone else; SetDriver writes the entity's seat +0x63C = 1, +0x8B0 = the id, then calls it; and
    // tests a player id (type 0) for its own branch
    {0x142F7F810, "8B 02 89 81 58 01 00 00 8B 02 3B 05 ?? ?? ?? ?? 74 2E 89 81 70 01 00 00 F7 02 00 E0 1F 00 0F B6 81 9D 01 00 00 75 0F 24 FD 0C 01 88 81 9D 01 00 00",
     "truck rules: unit rider +0x158 and +0x19D bits (FUN_142f7f810)", kTrucks},
    {0x142F7C537, "41 BF 01 00 00 00 44 89 BF 3C 06 00 00 41 8B 06 89 87 B0 08 00 00 E8 BE 32 00 00", "truck rules: SetDriver seat +0x63C / +0x8B0", kTrucks},
    {0x142F7C578, "3D FF FF 1F 00 0F 84 C7 01 00 00 A9 00 E0 1F 00 0F 85 BC 01 00 00", "truck rules: SetDriver player-id branch", kTrucks},
};
// clang-format on

// ---------------------------------------------------------------- timing
constexpr double kPollMs = 100;          // the pool poll
constexpr double kSettleMs = 5000;       // after the world is ready: the guest's load window ends, the host announces diffs
constexpr double kGraceMs = 10000;       // after the guest's first binding: mirrors allowed (placement vehicles stream in first)
constexpr double kSnapRetryMs = 5000;    // the guest asks again without a complete snapshot
constexpr double kSnapPeriodMs = 30000;  // and once in a while anyway (vehicles the host moved)
constexpr double kReconcileMs = 1000;
constexpr double kReqTimeoutMs = 10000;  // the host: a guest request with no new vehicle
constexpr double kSummonNoticeMs = 12000;
constexpr double kMirrorExactMs = 5000, kMirrorFallbackMs = 6000;
constexpr double kAnnounceWaitMs = 2000; // the host: a new unit is announced once its actor is in the world (or after this)

// ---------------------------------------------------------------- state
std::atomic<bool> g_on{false};
int g_role = 0;               // 1 host, 2 guest
char g_off[220] = "not installed";
bool g_wanted = false;
unsigned g_groups_ok = 0;
bool g_exact_ok = false, g_garage_ok = false;
bool g_hooked_enqueue = false, g_hooked_victim = false, g_hooked_garage = false;
bool g_stopped = false;

using FnEnqueue = void (*)(std::uintptr_t, std::uintptr_t);
using FnGarage = std::uint64_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t);
using FnVictim = std::uintptr_t (*)(std::uintptr_t);
using FnSpawn = void (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uint8_t, std::uintptr_t, std::uint8_t);
using FnExact = std::uint64_t (*)(std::uintptr_t, std::uintptr_t, std::uint32_t);
using FnWriteEntry = std::uint8_t (*)(std::uintptr_t, std::uintptr_t);
using FnTeleport = void (*)(std::uintptr_t, const void*, std::uint8_t, std::uint8_t);
using FnArea = std::uint16_t (*)();
FnEnqueue o_enqueue = nullptr;
FnGarage o_garage = nullptr;
FnVictim o_victim = nullptr;

// the detours' view (any thread)
thread_local bool t_mod_call = false;
thread_local bool t_dev_call = false;  // DevSpawn: a forwarded request goes as ReqReason::dev
thread_local int t_garage_slot = -1;
thread_local double t_garage_xf[8] = {};  // pos (3 doubles) + rot (9 floats) as 0x3C bytes
thread_local bool t_garage_has_xf = false;
std::atomic<bool> g_load_window{true};    // guest: the save's own spawns replay
std::atomic<bool> g_can_forward{false};   // guest: forward dropped requests
std::atomic<unsigned> g_n_pass{0}, g_n_mod{0}, g_n_load{0}, g_n_online{0}, g_n_forward{0}, g_n_drop{0}, g_n_victim{0}, g_n_victim_refused{0};
std::atomic<std::uint32_t> g_last_refused_id{0};

// forwarded requests (detour thread -> game thread)
SRWLOCK g_fwd_lock = SRWLOCK_INIT;
constexpr unsigned kFwdCap = 16;
vwire::Req g_fwd[kFwdCap];
unsigned g_fwd_n = 0;

// the host's shared-active ids (game thread writes, the victim detour reads)
SRWLOCK g_act_lock = SRWLOCK_INIT;
constexpr unsigned kActCap = 256;
std::uint32_t g_act_ids[kActCap];
unsigned g_act_n = 0;

KeyQuery g_guest_owns = nullptr;
IdQuery g_followed = nullptr;
Handler g_handlers[64] = {};

// ---------------------------------------------------------------- pool poll
struct PUnit {
    std::uintptr_t unit = 0, entity = 0;
    int pool_kind = 0;
    int index = 0;
    std::uint32_t id = 0;
    bool in_use = false;
    std::uint8_t state = 0;
    std::uint32_t flags = 0;
    std::uint8_t fb[4] = {};
    std::uint8_t guid[16] = {};
    std::uint16_t area = 0, rec2 = 0;
    std::uint32_t rider = vwire::kNoRider;
    float life = 0, battery = 0;
    bool has_xf = false;
    WorldTransform xf{};
    bool removed() const noexcept { return (fb[0] & 0x40) != 0; }
    bool reserved() const noexcept { return (fb[2] & 8) != 0; }
    bool alive() const noexcept { return in_use && !removed(); }
    bool ridden() const noexcept { return rider != vwire::kNoRider; }
    bool HasGuid() const noexcept {
        for (std::uint8_t b : guid)
            if (b) return true;
        return false;
    }
    vwire::VKey Key() const noexcept {
        vwire::VKey k;
        k.id = id;
        k.kind = static_cast<std::uint8_t>(pool_kind);
        std::memcpy(k.guid, guid, 16);
        return k;
    }
};
struct PollState {
    bool ok = false;
    double t = 0;
    std::vector<PUnit> units;  // every unit of the three pools (in use or not)
    unsigned in_use = 0;
} g_poll;

// round 23 D: the MULE trucks' holds (the block before TickBody)
trucks::HoldWhy HoldOfHost(std::uint32_t host_id);
trucks::HoldWhy HoldOfPair(std::uint32_t host_id, std::uint32_t local_id);
bool HeldNote(std::uint32_t id, trucks::HoldWhy w, const char* what, double now);
void LogTrucks(const char* who);

bool ReadXfAt(std::uintptr_t a, WorldTransform& out) noexcept {
    std::uint8_t b1[0x3C], b2[0x3C];
    if (!mem::Read(a, b1, sizeof(b1)) || !mem::Read(a, b2, sizeof(b2)) || std::memcmp(b1, b2, sizeof(b1)) != 0) return false;
    std::memcpy(&out.pos, b1, 0x18);
    std::memcpy(&out.rot, b1 + 0x18, 0x24);
    if (!std::isfinite(out.pos.x) || !std::isfinite(out.pos.y) || !std::isfinite(out.pos.z) || std::fabs(out.pos.x) > 1e7 ||
        std::fabs(out.pos.y) > 1e7 || std::fabs(out.pos.z) > 1e7)
        return false;
    bool any = false;
    for (const auto& row : out.rot.m)
        for (float v : row) {
            if (!std::isfinite(v) || std::fabs(v) > 1.5f) return false;
            if (v != 0) any = true;
        }
    return any;
}

int KindOfEntity(std::uintptr_t e) noexcept {
    if (!e) return 0;
    const std::uintptr_t vt = mem::Q(e);
    for (int k = 1; k <= 3; ++k)
        if (vt == A(kVehVt[k])) return k;
    return 0;
}

int KindOfPool(std::uintptr_t pool) noexcept {
    if (!pool) return 0;
    const std::uintptr_t vt = mem::Q(pool);
    for (int k = 1; k <= 3; ++k)
        if (vt == A(kPoolVt[k])) return k;
    return 0;
}

bool UnitVtOk(std::uintptr_t unit) noexcept {
    const std::uintptr_t vt = mem::Q(unit);
    for (std::uintptr_t v : kUnitVt)
        if (vt == A(v)) return true;
    return false;
}

std::uintptr_t Registry() noexcept { return mem::Q(A(kRegGlobal)); }
std::uintptr_t PoolOf(int kind) noexcept {
    const std::uintptr_t r = Registry();
    if (!r || kind < 1 || kind > 3) return 0;
    const std::uintptr_t p = mem::Q(r + 0x28 + static_cast<std::uintptr_t>(kind) * 8);
    return KindOfPool(p) == kind ? p : 0;
}

// One unit, its block read twice (the identity fields equal).
bool ReadUnit(std::uintptr_t u, int kind, int index, PUnit& out) noexcept {
    std::uint8_t a[kUBlock], b[kUBlock];
    if (!mem::Read(u, a, sizeof(a)) || !mem::Read(u, b, sizeof(b))) return false;
    auto same = [&](std::uintptr_t off, std::size_t n) { return std::memcmp(a + off, b + off, n) == 0; };
    if (!same(kUId, 4) || !same(kUInUse, 1) || !same(kUFlags, 4) || !same(kUFlagBytes, 4) || !same(kUGuid, 16) || !same(kUEntRef, 8)) return false;
    PUnit p;
    p.unit = u;
    p.pool_kind = kind;
    p.index = index;
    std::memcpy(&p.id, a + kUId, 4);
    p.in_use = a[kUInUse] != 0;
    p.state = a[kUState];
    std::memcpy(&p.flags, a + kUFlags, 4);
    std::memcpy(p.fb, a + kUFlagBytes, 4);
    std::memcpy(p.guid, a + kUGuid, 16);
    std::memcpy(&p.area, a + kUArea, 2);
    std::memcpy(&p.rec2, a + kURec2, 2);
    std::memcpy(&p.rider, a + kURider, 4);
    std::memcpy(&p.life, a + kULife, 4);
    std::memcpy(&p.battery, a + kUBattery, 4);
    if (vwire::KindOfActorId(p.id) != kind) return false;  // not this pool's unit: the layout is not what we think
    std::uintptr_t er = 0;
    std::memcpy(&er, a + kUEntRef, 8);
    const std::uintptr_t e = er ? er - 0x20 : 0;
    if (p.in_use && e && KindOfEntity(e) == kind && mem::Q(e + kEUnit) == u) {
        p.entity = e;
        p.has_xf = ReadXfAt(e + game::entity_off::kPosition, p.xf);
        float life = 0;
        if (mem::Rd(e + kELife, life) && std::isfinite(life)) p.life = life;
    }
    if (p.in_use && !p.has_xf) {
        // the unit's own transform (unit vfunc +0x18 = +0x28): pos doubles, rot floats
        std::memcpy(&p.xf.pos, a + kUXf, 0x18);
        std::memcpy(&p.xf.rot, a + kUXf + 0x18, 0x24);
        bool ok = std::isfinite(p.xf.pos.x) && std::isfinite(p.xf.pos.y) && std::isfinite(p.xf.pos.z) && std::fabs(p.xf.pos.x) < 1e7 &&
                  std::fabs(p.xf.pos.y) < 1e7 && std::fabs(p.xf.pos.z) < 1e7 && (p.xf.pos.x != 0 || p.xf.pos.y != 0 || p.xf.pos.z != 0);
        for (const auto& row : p.xf.rot.m)
            for (float v : row)
                if (!std::isfinite(v) || std::fabs(v) > 1.5f) ok = false;
        p.has_xf = ok;
    }
    if (!std::isfinite(p.life)) p.life = 0;
    if (!std::isfinite(p.battery)) p.battery = 0;
    if (p.rider > vwire::kNoRider) p.rider = vwire::kNoRider;
    out = p;
    return true;
}

bool Poll(std::vector<PUnit>& out) noexcept {
    out.clear();
    if (!Registry()) return false;
    for (int kind = 1; kind <= 3; ++kind) {
        const std::uintptr_t pool = PoolOf(kind);
        if (!pool) return false;
        std::int32_t n1 = 0, n2 = 0;
        std::uintptr_t arr1 = 0, arr2 = 0;
        if (!mem::Rd(pool + 0x40, n1) || !mem::Rd(pool + 0x48, arr1) || n1 < 0 || n1 > 256 || (n1 && !arr1)) return false;
        std::uintptr_t list[256], again[256];
        if (n1 && (!mem::Read(arr1, list, static_cast<std::size_t>(n1) * 8) || !mem::Read(arr1, again, static_cast<std::size_t>(n1) * 8)))
            return false;
        if (!mem::Rd(pool + 0x40, n2) || !mem::Rd(pool + 0x48, arr2) || n1 != n2 || arr1 != arr2 ||
            (n1 && std::memcmp(list, again, static_cast<std::size_t>(n1) * 8) != 0))
            return false;
        for (int i = 0; i < n1; ++i) {
            if (!list[i] || !UnitVtOk(list[i])) continue;
            PUnit p;
            if (!ReadUnit(list[i], kind, i, p) && !ReadUnit(list[i], kind, i, p)) continue;  // a second try on a torn read
            out.push_back(p);
        }
    }
    return true;
}

const PUnit* PolledById(std::uint32_t id) noexcept {
    for (const PUnit& p : g_poll.units)
        if (p.id == id) return &p;
    return nullptr;
}

// The unit still is what the poll saw (id, in use, GUID), read now.
bool StillSame(const PUnit& p) noexcept {
    std::uint32_t id = 0;
    std::uint8_t use = 0;
    std::uint8_t g[16];
    return mem::Rd(p.unit + kUId, id) && mem::Rd(p.unit + kUInUse, use) && mem::Read(p.unit + kUGuid, g, 16) && id == p.id && (use != 0) == p.in_use &&
           std::memcmp(g, p.guid, 16) == 0;
}

// ---------------------------------------------------------------- game calls (game thread, SEH; POD locals only)
int CallArea() noexcept {
    __try {
        return reinterpret_cast<FnArea>(A(kCurArea))();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
bool OrRemoveBit(std::uintptr_t unit) noexcept {
    __try {
        _InterlockedOr(reinterpret_cast<volatile long*>(unit + kUFlags), static_cast<long>(kRemoveBit));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallTeleport(std::uintptr_t e, const WorldTransform* xf) noexcept {
    __try {
        reinterpret_cast<FnTeleport>(A(kTeleport))(e, xf, 0, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
int CallExact(std::uintptr_t pool, std::uintptr_t desc, std::uint32_t id) noexcept {
    const bool was = t_mod_call;
    t_mod_call = true;
    int r = -1;
    __try {
        r = (reinterpret_cast<FnExact>(mem::Q(mem::Q(pool) + 0x30))(pool, desc, id) & 0xFF) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r = -1;
    }
    t_mod_call = was;
    return r;
}
bool CallSpawn(std::uintptr_t rec, const WorldTransform* xf, std::uint8_t p4, bool mod_call) noexcept {
    const bool was = t_mod_call;
    t_mod_call = mod_call;
    bool ok = true;
    __try {
        reinterpret_cast<FnSpawn>(A(kSpawn))(Registry(), rec, reinterpret_cast<std::uintptr_t>(xf), p4, 0, 0);  // p6 = 0, always
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    t_mod_call = was;
    return ok;
}
int CallGarage(std::uintptr_t pm, std::uintptr_t slot, const WorldTransform* xf) noexcept {
    __try {
        const FnGarage f = o_garage ? o_garage : reinterpret_cast<FnGarage>(A(kGarageSpawn));
        return (f(pm, slot, reinterpret_cast<std::uintptr_t>(xf)) & 0xFF) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// The guest-side garage call goes through our detour (the game's own path).
int CallGarageViaDetour(std::uintptr_t pm, std::uintptr_t slot, const WorldTransform* xf) noexcept {
    __try {
        return (reinterpret_cast<FnGarage>(A(kGarageSpawn))(pm, slot, reinterpret_cast<std::uintptr_t>(xf)) & 0xFF) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
int CallWriteEntry(std::uintptr_t unit, std::uint8_t* out) noexcept {
    __try {
        return (reinterpret_cast<FnWriteEntry>(A(kWriteEntry))(unit, reinterpret_cast<std::uintptr_t>(out)) & 0xFF) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// A spawn record (0x142F830A0 reads +0, +2, +8 String, +0x10 handle, +0x14..+0x1B).
struct Record {
    alignas(8) std::uint8_t b[0x40] = {};
    void Set(std::uint16_t rec0, std::uint16_t rec2, std::uint64_t rec14) noexcept {
        std::memset(b, 0, sizeof(b));
        std::memcpy(b + 0, &rec0, 2);
        std::memcpy(b + 2, &rec2, 2);
        const std::uintptr_t empty = A(kEmptyString);
        std::memcpy(b + 8, &empty, 8);
        std::memcpy(b + 0x14, &rec14, 8);
    }
};

// ---------------------------------------------------------------- outbox (both roles; order kept)
std::deque<std::vector<std::uint8_t>> g_out;
constexpr std::size_t kOutCap = 256;
std::uint64_t g_sent = 0, g_send_fail = 0, g_rx = 0, g_rx_bad = 0;
std::uint32_t g_gen = 0;
bool Queue(const std::uint8_t* b, std::size_t n) {
    if (!n || n > vwire::kMaxMessage || g_out.size() >= kOutCap) return false;
    g_out.emplace_back(b, b + n);
    return true;
}
void Flush() {
    while (!g_out.empty()) {
        const auto& m = g_out.front();
        if (!link::SendVehicleEvent(m.data(), m.size())) {
            ++g_send_fail;
            return;
        }
        ++g_sent;
        g_out.pop_front();
    }
}

// ---------------------------------------------------------------- common
bool g_ready = false;              // coop::WorldReady
std::uint32_t g_counter = 0;       // its load counter
double g_ready_since = 0;
bool g_connected = false;
WorldTransform g_partner{};
bool g_have_partner = false;
double g_first_tick = 0;
bool g_notice_done = false;

bool Settled(double now) noexcept { return g_ready && now - g_ready_since >= kSettleMs; }

// The partner's world (review fix: VOWN / REM / STD1 from another world are dropped). Host: the load counter of the guest's
// last SNAP_REQ; guest: the host's world its newest snapshot or VOWN / REM named. 0: none yet.
std::uint32_t g_peer_world = 0;
// Guest: the host's game answered a SNAP_REQ with a snapshot in this world; since when we wait for an answer (0: not waiting).
bool g_host_answered = false;
double g_snap_wait_since = 0;
constexpr double kHostSilentMs = 20000;  // asked and no snapshot for this long: the host shares no real vehicles (now)
std::atomic<bool> g_host_real_gate{false};  // the gate's view of it (any thread)
bool g_host_silent_logged = false;
double g_now_ms = 0;
bool HostSilent() noexcept { return g_role == 2 && g_snap_wait_since > 0 && g_now_ms - g_snap_wait_since > kHostSilentMs; }

// Host: places of our vehicles in the guest's world while their actor is not spawned here (vehicle_drive's far handback).
std::map<std::uint32_t, WorldTransform> g_place_override;

vwire::SnapEntry EntryOf(const PUnit& p) noexcept {
    vwire::SnapEntry e;
    e.key = p.Key();
    e.state = p.state;
    e.eflags = 0;
    WorldTransform xf = p.xf;
    bool has_xf = p.has_xf;
    if (g_role == 1 && !p.entity && p.alive()) {
        auto o = g_place_override.find(p.id);
        if (o != g_place_override.end()) xf = o->second, has_xf = true;
    }
    if (p.entity) e.eflags |= vwire::kEntEntity;
    if (has_xf) e.eflags |= vwire::kEntXf;
    if (p.removed()) e.eflags |= vwire::kEntRemoved;
    if (p.reserved()) e.eflags |= vwire::kEntReserved;
    if (p.ridden()) e.eflags |= vwire::kEntRidden;
    if (has_xf) {
        e.pos[0] = xf.pos.x, e.pos[1] = xf.pos.y, e.pos[2] = xf.pos.z;
        std::memcpy(e.rot, &xf.rot, sizeof(e.rot));
    }
    e.flags = p.flags;
    std::memcpy(e.flag_bytes, p.fb, 4);
    e.area = p.area;
    e.rec2 = p.rec2;
    e.life = p.life;
    e.battery = p.battery;
    e.rider = p.rider;
    return e;
}
WorldTransform XfOf(const vwire::SnapEntry& e) noexcept {
    WorldTransform t{};
    t.pos.x = e.pos[0], t.pos.y = e.pos[1], t.pos.z = e.pos[2];
    std::memcpy(&t.rot, e.rot, sizeof(e.rot));
    return t;
}
double DistXf(const WorldTransform& a, const double (&b)[3]) noexcept {
    const double p[3] = {a.pos.x, a.pos.y, a.pos.z};
    return vwire::Dist(p, b);
}

bool LocalSam(WorldTransform* out) noexcept {
    game::Entity* e = nullptr;
    if (!game::TryGetLocalPlayer(&e) || !e) return false;
    return ReadXfAt(reinterpret_cast<std::uintptr_t>(e) + game::entity_off::kPosition, *out);
}

void Notice(const char* text, bool warn, double until) { coop::SetVehicleNotice(text, warn, until); }

// ---------------------------------------------------------------- host
struct HostSeen {
    bool alive = false;
    bool announced = false;
    double since = 0;
    // the vehicle in the unit (a unit released and reused within one poll keeps alive = true: review fix): its placement GUID
    // and spawn record +2, constant for one vehicle
    bool sig = false;
    std::uint8_t guid[16] = {};
    std::uint16_t rec2 = 0;
    bool rem_pending = false;  // the old vehicle's REM goes out right before the new one's ADD
    vwire::VKey rem_key;
};
std::map<std::uint32_t, HostSeen> g_hseen;
bool g_hbase = false;  // the baseline after the world became ready: diffs only after it
struct Before {
    std::uint32_t id = 0;
    bool has_pos = false;
    double pos[3] = {};
};
struct HostPendingReq {
    vwire::Req req;
    double t = 0;
    std::uint8_t path = 0;
    std::vector<Before> before;  // the units alive of that kind when it was spawned (and where they stood)
};
void SetSig(HostSeen& s, const PUnit& p) noexcept {
    s.sig = true;
    std::memcpy(s.guid, p.guid, 16);
    s.rec2 = p.rec2;
}
bool SigDiffers(const HostSeen& s, const PUnit& p) noexcept { return s.sig && (std::memcmp(s.guid, p.guid, 16) != 0 || s.rec2 != p.rec2); }
vwire::VKey SeenKey(const HostSeen& s, const PUnit& p) noexcept {
    vwire::VKey k = p.Key();
    std::memcpy(k.guid, s.guid, 16);
    return k;
}
// A new unit answers a guest's request: of its kind, within 40 m of the asked place, and not one that stood there before
// (an id alive before counts when its unit now holds another vehicle: it moved more than 20 m, an eviction's reuse).
bool Answers(const HostPendingReq& h, const PUnit& p) noexcept {
    if (h.req.kind != p.pool_kind || !p.has_xf || DistXf(p.xf, h.req.pos) > 40.0) return false;
    for (const Before& b : h.before)
        if (b.id == p.id) return b.has_pos && DistXf(p.xf, b.pos) > 20.0;
    return true;
}
std::vector<HostPendingReq> g_hreqs;
std::deque<vwire::SnapReq> g_snap_reqs;
std::uint32_t g_snap_id = 0;
vwire::RateLimit g_host_req_limit(4, 10000);
struct ActiveRec {
    double last_near = -1e18, last_drove = -1e18;
    bool riding = false;
};
std::map<std::uint32_t, ActiveRec> g_active;
std::uint64_t g_adds = 0, g_rems = 0, g_snaps = 0, g_reqs_served = 0, g_reqs_refused = 0, g_guest_rems = 0, g_guest_rems_applied = 0;

bool BuildAdd(const PUnit& p, vwire::AddSource src, std::uint32_t token, vwire::Add* out) noexcept {
    vwire::Add a;
    a.token = token;
    a.source = src;
    a.unit = EntryOf(p);
    a.rec0 = static_cast<std::uint16_t>(p.pool_kind | (((p.flags >> 22) & 0x3Fu) << 6));
    a.p4 = 1;
    if (g_exact_ok) {
        alignas(8) std::uint8_t entry[vwire::kEntrySize + 16] = {};
        const int r = CallWriteEntry(p.unit, entry);
        std::uint32_t eid = 0;
        std::memcpy(&eid, entry + 0x2C, 4);
        if (r == 1 && eid == p.id) {
            a.has_entry = true;
            std::memcpy(a.entry, entry, vwire::kEntrySize);
        } else if (r < 0) {
            Log("[vshare] host: the entry writer FAULTED on unit 0x%06X: announced without its entry", p.id);
        }
    }
    *out = a;
    return true;
}

void SendAdd(const PUnit& p, vwire::AddSource src, std::uint32_t token) {
    vwire::Add a;
    BuildAdd(p, src, token, &a);
    std::uint8_t buf[vwire::kMaxMessage];
    const std::size_t n = vwire::EncodeAdd(a, buf, sizeof(buf));
    if (n && Queue(buf, n)) {
        ++g_adds;
        Log("[vshare] host: ADD %s 0x%06X (%s%s) at (%.2f, %.2f, %.2f)%s", vwire::KindName(static_cast<std::uint8_t>(p.pool_kind)), p.id,
            src == vwire::AddSource::host_own ? "our spawn" : src == vwire::AddSource::guest_request ? "the guest's request" : "announced again",
            a.has_entry ? ", entry" : ", no entry", p.xf.pos.x, p.xf.pos.y, p.xf.pos.z, token ? " (request)" : "");
    }
}

void SendRem(const vwire::VKey& k, vwire::RemReason why, std::uint8_t from, bool has_pos, const WorldTransform& at) {
    vwire::Rem r;
    r.key = k;
    r.reason = why;
    r.from = from;
    r.has_pos = has_pos;
    if (has_pos) r.pos[0] = at.pos.x, r.pos[1] = at.pos.y, r.pos[2] = at.pos.z;
    r.world = g_counter;
    std::uint8_t buf[128];
    const std::size_t n = vwire::EncodeRem(r, buf, sizeof(buf));
    if (n && Queue(buf, n)) ++g_rems;
}

void SendResult(std::uint32_t token, vwire::ReqResult res, std::uint8_t path) {
    vwire::Result r;
    r.token = token;
    r.result = res;
    r.path = path;
    std::uint8_t buf[64];
    const std::size_t n = vwire::EncodeResult(r, buf, sizeof(buf));
    if (n) Queue(buf, n);
}

void SendSnapshot(std::uint32_t req_id, const char* why) {
    std::vector<vwire::SnapEntry> entries;
    for (const PUnit& p : g_poll.units)
        if (p.in_use) entries.push_back(EntryOf(p));
    const int area = CallArea();
    const auto chunks = vwire::EncodeSnapshot(++g_snap_id, req_id, g_counter, static_cast<std::uint16_t>(area < 0 ? 0 : area), entries);
    unsigned queued = 0;
    for (const auto& c : chunks)
        if (Queue(c.data(), c.size())) ++queued;
    ++g_snaps;
    Log("[vshare] host: snapshot #%u (%s): %zu vehicles in %u chunk(s), world #%u, area %d", g_snap_id, why, entries.size(), queued, g_counter, area);
    LogTrucks("host: snapshot");  // round 23 D (vehicle_mule_log)
}

// The partner's place and vehicle against our units: the shared-active set the victim detour refuses.
void UpdateActive(double now) {
    double pv[3] = {};
    const float zero[3] = {0, 0, 0};
    const bool partner_vehicle = vsync::PartnerVehicleWorld(now, zero, pv);
    std::uint32_t ids[kActCap];
    unsigned n = 0;
    for (const PUnit& p : g_poll.units) {
        if (!p.alive()) continue;
        ActiveRec& a = g_active[p.id];
        a.riding = false;
        if (p.has_xf) {
            if (g_have_partner) {
                const double pp[3] = {g_partner.pos.x, g_partner.pos.y, g_partner.pos.z};
                if (DistXf(p.xf, pp) <= vwire::kActiveNearM) a.last_near = now;
            }
            if (partner_vehicle && DistXf(p.xf, pv) <= 4.0) {
                a.riding = true;
                a.last_drove = now;
            }
        }
        vwire::ActiveInfo ai;
        ai.partner_riding = a.riding;
        ai.last_near_ms = a.last_near;
        ai.last_drove_ms = a.last_drove;
        if (vwire::SharedActive(ai, now) && n < kActCap) ids[n++] = p.id;
    }
    AcquireSRWLockExclusive(&g_act_lock);
    std::memcpy(g_act_ids, ids, n * sizeof(std::uint32_t));
    g_act_n = n;
    ReleaseSRWLockExclusive(&g_act_lock);
}

bool IsActiveId(std::uint32_t id) noexcept {
    bool hit = false;
    AcquireSRWLockShared(&g_act_lock);
    for (unsigned i = 0; i < g_act_n; ++i)
        if (g_act_ids[i] == id) {
            hit = true;
            break;
        }
    ReleaseSRWLockShared(&g_act_lock);
    return hit;
}

// The host's own world changed: new units are announced (ADD), gone ones too (REM); a new unit near a guest's request
// answers it.
void HostDiff(double now) {
    if (!g_hbase) {
        g_hseen.clear();
        for (const PUnit& p : g_poll.units) {
            HostSeen& s = g_hseen[p.id];
            s.alive = p.alive();
            s.announced = true;  // in the snapshot
            s.since = now;
            if (s.alive) SetSig(s, p);
        }
        g_hbase = true;
        return;
    }
    for (const PUnit& p : g_poll.units) {
        HostSeen& s = g_hseen[p.id];
        const bool alive = p.alive();
        if (alive && !s.alive) {
            s.alive = true;
            s.announced = false;
            s.since = now;
            SetSig(s, p);
        } else if (!alive && s.alive) {
            s.alive = false;
            if (s.announced) {
                SendRem(SeenKey(s, p), p.removed() ? vwire::RemReason::removed_flag : vwire::RemReason::released, 0, p.has_xf, p.xf);
                Log("[vshare] host: REM %s 0x%06X (%s)", vwire::KindName(static_cast<std::uint8_t>(p.pool_kind)), p.id,
                    p.removed() ? "removed, remembered" : "released");
            }
            s.announced = false;
            s.rem_pending = false;
        } else if (alive && s.alive) {
            // the unit was released and reused between two polls (an eviction for a spawn): another vehicle under that id
            bool reused = SigDiffers(s, p);
            if (!reused && s.announced)
                for (const HostPendingReq& h : g_hreqs)
                    if (Answers(h, p)) {
                        bool was_before = false;
                        for (const Before& b : h.before) was_before = was_before || b.id == p.id;
                        reused = was_before;
                    }
            if (reused) {
                if (s.announced) {
                    s.rem_pending = true;
                    s.rem_key = SeenKey(s, p);
                }
                Log("[vshare] host: unit 0x%06X holds another %s now (released and reused within one poll): the old one's REM, then an ADD",
                    p.id, vwire::KindName(static_cast<std::uint8_t>(p.pool_kind)));
                s.announced = false;
                s.since = now;
                SetSig(s, p);
            }
        }
        if (s.alive && !s.announced && p.has_xf && (p.entity || now - s.since >= kAnnounceWaitMs)) {
            if (s.rem_pending) {
                SendRem(s.rem_key, vwire::RemReason::released, 0, false, p.xf);
                s.rem_pending = false;
            }
            // a guest's request this answers?
            std::uint32_t token = 0;
            for (auto it = g_hreqs.begin(); it != g_hreqs.end(); ++it) {
                if (!Answers(*it, p)) continue;
                token = it->req.token;
                Log("[vshare] host: the guest's request #%u became %s 0x%06X after %.1f s", token, vwire::KindName(it->req.kind), p.id,
                    (now - it->t) / 1000.0);
                ++g_reqs_served;
                g_hreqs.erase(it);
                break;
            }
            SendAdd(p, token ? vwire::AddSource::guest_request : vwire::AddSource::host_own, token);
            s.announced = true;
        }
    }
}

void HostServeRequest(const vwire::Req& q, double now) {
    if (q.reason == vwire::ReqReason::dev) {
        // the guest's dev tools (dev_rules.hpp): a dev vehicle would stay in this game's real save
        if (const char* why = dev::HostRefusesGuestDev()) {
            SendResult(q.token, vwire::ReqResult::refused, 0);
            Log("[vshare] host: the guest's dev request #%u refused: %s", q.token, why);
            ++g_reqs_refused;
            return;
        }
        Log("[vshare] host: the guest's request #%u is a dev request: allowed (the dev tools are on, this game runs on a sandboxed save)", q.token);
    }
    if (!Settled(now)) {
        SendResult(q.token, vwire::ReqResult::refused, 0);
        Log("[vshare] host: the guest's request #%u refused (our world is not ready)", q.token);
        ++g_reqs_refused;
        return;
    }
    if (!g_host_req_limit.Allow(now)) {
        SendResult(q.token, vwire::ReqResult::refused, 0);
        Log("[vshare] host: the guest's request #%u refused (more than 4 in 10 s)", q.token);
        ++g_reqs_refused;
        return;
    }
    WorldTransform xf{};
    xf.pos.x = q.pos[0], xf.pos.y = q.pos[1], xf.pos.z = q.pos[2];
    std::memcpy(&xf.rot, q.rot, sizeof(q.rot));
    // Duplicates: the slot's vehicle already out, or one of that kind already standing there (the guest's game asked for a
    // vehicle the host's world already has). It is announced again instead.
    std::uintptr_t pm = 0, slot = 0;
    if (q.garage_slot >= 0 && g_garage_ok) {
        pm = mem::Q(A(kParkGlobal));
        if (pm) slot = pm + kPmSlots + static_cast<std::uintptr_t>(q.garage_slot) * kPmSlotSize;
        std::uint32_t sid = vwire::kNoRider;
        if (slot && mem::Rd(slot + 0x50, sid) && sid != vwire::kNoRider) {
            const PUnit* p = PolledById(sid);
            if (p && p->alive()) {
                SendResult(q.token, vwire::ReqResult::duplicate, 1);
                SendAdd(*p, vwire::AddSource::reannounce, 0);
                Log("[vshare] host: the guest's request #%u: garage slot %d's vehicle 0x%06X is out already (announced again)", q.token, q.garage_slot,
                    sid);
                return;
            }
        }
    }
    for (const PUnit& p : g_poll.units) {
        if (!p.alive() || p.pool_kind != q.kind || !p.has_xf || DistXf(p.xf, q.pos) > 6.0) continue;
        SendResult(q.token, vwire::ReqResult::duplicate, 0);
        SendAdd(p, vwire::AddSource::reannounce, 0);
        Log("[vshare] host: the guest's request #%u: %s 0x%06X stands there already (announced again)", q.token, vwire::KindName(q.kind), p.id);
        return;
    }
    HostPendingReq h;
    h.req = q;
    h.t = now;
    for (const PUnit& p : g_poll.units)
        if (p.alive() && p.pool_kind == q.kind) {
            Before b;
            b.id = p.id;
            b.has_pos = p.has_xf;
            if (p.has_xf) b.pos[0] = p.xf.pos.x, b.pos[1] = p.xf.pos.y, b.pos[2] = p.xf.pos.z;
            h.before.push_back(b);
        }
    if (q.pos[0] == 0 && q.pos[1] == 0 && q.pos[2] == 0) {  // DecodeReq refuses it too
        SendResult(q.token, vwire::ReqResult::refused, 0);
        Log("[vshare] host: the guest's request #%u refused (no place)", q.token);
        ++g_reqs_refused;
        return;
    }
    bool ok = false;
    if (slot) {
        std::uint8_t state = 0;
        std::uint16_t rec0 = 0;
        mem::Rd(slot, state);
        mem::Rd(slot + 8, rec0);
        if ((rec0 & 0xF) == q.kind) {
            const int r = CallGarage(pm, slot, &xf);
            Log("[vshare] host: the guest's request #%u: garage slot %d (state %u) spawn -> %d", q.token, q.garage_slot, state, r);
            ok = r == 1;
            h.path = 1;
        } else {
            Log("[vshare] host: the guest's request #%u: garage slot %d holds a %s here: a plain spawn instead", q.token, q.garage_slot,
                vwire::KindName(static_cast<std::uint8_t>(rec0 & 0xF)));
        }
    }
    if (!h.path) {
        std::uint16_t rec0 = 0;
        std::uint8_t p4 = 0, p6 = 0;
        if (!vwire::RecordFromRequestFlags(q.req_flags, q.kind, &rec0, &p4, &p6)) {
            SendResult(q.token, vwire::ReqResult::refused, 0);
            Log("[vshare] host: the guest's request #%u refused (request flags 0x%X are not a record's)", q.token, q.req_flags);
            ++g_reqs_refused;
            return;
        }
        Record rec;
        rec.Set(rec0, q.rec2, q.rec14);
        ok = CallSpawn(reinterpret_cast<std::uintptr_t>(rec.b), &xf, p4, false);
        h.path = 2;
        Log("[vshare] host: the guest's request #%u: spawn %s (record 0x%04X, p4 %u, p6 %u -> 0) at (%.2f, %.2f, %.2f) %s", q.token,
            vwire::KindName(q.kind), rec0, p4, p6, xf.pos.x, xf.pos.y, xf.pos.z, ok ? "requested" : "FAULTED");
    }
    if (!ok) {
        SendResult(q.token, vwire::ReqResult::failed, h.path);
        ++g_reqs_refused;
        return;
    }
    g_hreqs.push_back(std::move(h));
}

bool PartnerNear(const PUnit& p, double m) noexcept {
    if (!g_have_partner || !p.has_xf) return false;
    const double pp[3] = {g_partner.pos.x, g_partner.pos.y, g_partner.pos.z};
    return DistXf(p.xf, pp) <= m;
}

// The host's own unit is destroyed (its life 0, or the explosion latched): the guest's game destroyed it and the host's
// follower copied that (D2-5), so a removal of it from the guest matches what both worlds hold.
bool HostUnitDestroyed(const PUnit& p) noexcept {
    if (p.life <= 0.0f) return true;
    std::uint32_t latch = 0;
    return p.entity && mem::Rd(p.entity + 0xBD8, latch) && (latch & 0x100000u) != 0;
}

void HostGuestRemoval(const vwire::Rem& r) {
    ++g_guest_rems;
    const PUnit* p = PolledById(r.key.id);
    if (!p || !p->alive()) return;  // gone here too
    // round 23 D: a truck left to each game: the guest's removal is ignored (neither applied nor announced again)
    if (HeldNote(r.key.id, HoldOfHost(r.key.id), "host: the guest's removal ignored", g_now_ms)) return;
    // Review fixes: the host applies a guest removal only to a vehicle the guest owns, that is destroyed here too (a garage
    // store or any other removal in the guest's world never reaches the host's persisted world: the garage relay does not
    // exist), with the guest's last place of it within 5 m of ours (a removal without a place is refused) and the partner
    // body within 60 m of it.
    const bool owns = g_guest_owns && g_guest_owns(r.key);
    const char* why = nullptr;
    if (!owns) why = "it does not own it";
    else if (!HostUnitDestroyed(*p)) why = "it is not destroyed here (storing or removing a shared vehicle works in the host's game only)";
    else if (!PartnerNear(*p, 60.0)) why = "the guest's Sam is not near it";
    if (!why) {
        vwire::HostUnitView v;
        v.in_use = p->in_use, v.id = p->id, v.kind = static_cast<std::uint8_t>(p->pool_kind), v.removed = p->removed(), v.has_pos = p->has_xf;
        std::memcpy(v.guid, p->guid, 16);
        if (p->has_xf) v.pos[0] = p->xf.pos.x, v.pos[1] = p->xf.pos.y, v.pos[2] = p->xf.pos.z;
        if (vwire::HostRemovalAllowed(v, r.key, r.has_pos, r.pos) && StillSame(*p) && OrRemoveBit(p->unit)) {
            ++g_guest_rems_applied;
            Log("[vshare] host: the guest removed %s 0x%06X, which it drove and destroyed: removed here too", vwire::KindName(r.key.kind), r.key.id);
            return;
        }
        why = "the guard refused it (id / GUID / place differ, or no place given)";
    }
    // the host keeps it: the guest mirrors it again
    SendAdd(*p, vwire::AddSource::reannounce, 0);
    Log("[vshare] host: the guest removed %s 0x%06X: kept, announced again (%s)", vwire::KindName(r.key.kind), r.key.id, why);
}

void HostTick(double now) {
    if (!g_poll.ok) return;
    if (Settled(now)) HostDiff(now);
    UpdateActive(now);
    // snapshot requests (in order)
    while (!g_snap_reqs.empty() && Settled(now)) {
        SendSnapshot(g_snap_reqs.front().req_id, "asked");
        g_snap_reqs.pop_front();
    }
    // guest requests that produced nothing
    for (auto it = g_hreqs.begin(); it != g_hreqs.end();) {
        if (now - it->t > kReqTimeoutMs) {
            SendResult(it->req.token, vwire::ReqResult::timeout, it->path);
            Log("[vshare] host: the guest's request #%u produced no vehicle within %.0f s", it->req.token, kReqTimeoutMs / 1000.0);
            ++g_reqs_refused;
            it = g_hreqs.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------- guest
struct HostRec {
    vwire::SnapEntry e;
    bool has_add = false;
    vwire::Add add;
};
std::map<std::uint32_t, HostRec> g_table;
bool g_have_table = false;
std::uint32_t g_table_counter = 0;
vwire::SnapAssembler g_asm;
std::uint32_t g_req_id = 0;
std::uint32_t g_asked_counter = 0;
double g_asked_ms = -1e18, g_table_ms = -1e18;
std::map<std::uint32_t, std::uint32_t> g_bind;   // host id -> guest id
bool g_bound_once = false;
double g_first_bind_ms = 0;
double g_next_reconcile = 0;
bool g_reconcile_now = false;
std::map<std::uint32_t, double> g_cool;          // per host id (actions) / per local id | 0x80000000 (removals)
std::map<std::uint32_t, double> g_removing;      // guest ids we removed ourselves (not reported as the guest's removals)
std::map<std::uint32_t, double> g_need_sent;     // host ids we asked an ADD for
int g_path_pref = 0;                             // tests: 0 auto, 1 exact only, 2 fallback only, 3 re-place only
struct Mirror {
    std::uint32_t host_id = 0;
    std::uint8_t kind = 0;
    int path = 0;          // 1 exact-id, 2 fallback spawn, 3 re-place
    double t = 0;
    WorldTransform xf{};
    double pos[3] = {};
    std::vector<std::uint32_t> before;
    unsigned tried = 0;    // bit per path
};
std::vector<Mirror> g_mirrors;
// Mirrors made by the exact-id path, until their actor is in the world: is it a complete vehicle (its class, its place)?
struct Verify {
    std::uint32_t host_id = 0;
    std::uint8_t kind = 0;
    double t = 0;
    double pos[3] = {};
};
std::vector<Verify> g_verify;
std::uint64_t g_verified_ok = 0, g_verified_bad = 0;

void VerifyTick(double now) {
    for (auto it = g_verify.begin(); it != g_verify.end();) {
        const auto b = g_bind.find(it->host_id);
        const PUnit* p = b != g_bind.end() ? PolledById(b->second) : nullptr;
        if (p && p->alive() && p->entity) {
            const double d = p->has_xf ? DistXf(p->xf, it->pos) : -1;
            const bool kind_ok = KindOfEntity(p->entity) == it->kind && mem::Q(p->entity + kEUnit) == p->unit;
            const bool ok = kind_ok && d >= 0 && d < 5.0;
            ++(ok ? g_verified_ok : g_verified_bad);
            Log("[vshare] guest: mirror 0x%06X (%s, exact-id): its actor is in the world after %.1f s: %s (entity %p, %s, %.2f m from the host's spot, "
                "life %.1f, battery %.2f, flags 0x%08X)",
                it->host_id, vwire::KindName(it->kind), (now - it->t) / 1000.0, ok ? "a complete vehicle" : "INCOMPLETE", reinterpret_cast<void*>(p->entity),
                kind_ok ? "its class" : "WRONG class", d, p->life, p->battery, p->flags);
            it = g_verify.erase(it);
        } else if (!p || !p->alive() || now - it->t > 120000) {
            it = g_verify.erase(it);
        } else {
            ++it;
        }
    }
}
std::map<std::uint32_t, double> g_mirror_failed;  // host id -> when every path failed
std::uint64_t g_mirror_ok[4] = {}, g_mirror_fail[4] = {};
std::uint64_t g_removed_extra = 0, g_removed_wrong = 0, g_removed_host = 0, g_replaced = 0, g_guest_rem_sent = 0;
struct Summon {
    std::uint32_t token = 0;
    std::uint8_t kind = 0;
    double t = 0;
};
std::vector<Summon> g_summons;
std::uint32_t g_next_token = 0;
vwire::RateLimit g_fwd_limit(3, 6000);
// guest ids bound and alive at the last poll -> their host ids and last place (a unit missing from a poll, a torn read, is
// carried over for a few polls: only a unit the poll sees not alive counts as gone)
struct PrevBound {
    std::uint32_t host_id = 0;
    bool has_xf = false;
    WorldTransform xf{};
    unsigned misses = 0;
};
std::map<std::uint32_t, PrevBound> g_prev_bound;
std::map<std::uint32_t, std::uint32_t> g_mirrored;  // guest unit ids the mod mirrored a host vehicle into -> that host id
std::map<std::uint32_t, double> g_rem_told;         // host ids whose removal in our world we told the host -> when
vwire::BindPlan g_last_plan;
std::vector<vwire::SnapEntry> g_last_plan_host;
std::vector<vwire::LocalUnit> g_last_plan_local;
char g_last_plan_line[240] = {};

bool Cool(std::uint32_t key, double now, double ms) {
    auto it = g_cool.find(key);
    if (it != g_cool.end() && now - it->second < ms) return false;
    g_cool[key] = now;
    return true;
}

void SendSnapReq(std::uint8_t reason, double now) {
    vwire::SnapReq r;
    r.req_id = ++g_req_id;
    r.load_counter = g_counter;
    r.reason = reason;
    std::uint8_t buf[64];
    const std::size_t n = vwire::EncodeSnapReq(r, buf, sizeof(buf));
    if (n && Queue(buf, n)) {
        g_asked_ms = now;
        g_asked_counter = g_counter;
        if (g_snap_wait_since <= 0) g_snap_wait_since = now;  // answered by the next complete snapshot
        Log("[vshare] guest: asked the host for its vehicles (#%u, world #%u, %s)", r.req_id, g_counter,
            reason == 0 ? "after the load" : reason == 1 ? "periodic" : "test");
    }
}

void SendNeed(const vwire::VKey& k, std::uint8_t reason, double now) {
    auto it = g_need_sent.find(k.id);
    if (it != g_need_sent.end() && now - it->second < 5000) return;
    g_need_sent[k.id] = now;
    vwire::Need m;
    m.key = k;
    m.reason = reason;
    std::uint8_t buf[64];
    const std::size_t n = vwire::EncodeNeed(m, buf, sizeof(buf));
    if (n) Queue(buf, n);
}

bool RemoveLocal(const PUnit& u, const char* why, double now) {
    if (u.ridden() || !u.alive()) return false;
    {
        // round 23 D: a truck left to each game is never removed here (whatever the binding says)
        char what[120];
        std::snprintf(what, sizeof(what), "guest: not removed (%s)", why);
        if (HeldNote(u.id, HoldOfPair(0, u.id), what, now)) return false;
    }
    if (!Cool(0x80000000u | u.id, now, 10000)) return false;
    if (!StillSame(u)) return false;
    if (!OrRemoveBit(u.unit)) {
        Log("[vshare] guest: FAULT setting the removal bit on 0x%06X", u.id);
        return false;
    }
    g_removing[u.id] = now;
    Log("[vshare] guest: removed %s 0x%06X at (%.2f, %.2f, %.2f) (%s)", vwire::KindName(static_cast<std::uint8_t>(u.pool_kind)), u.id, u.xf.pos.x,
        u.xf.pos.y, u.xf.pos.z, why);
    return true;
}

std::uint32_t LocalForHost(std::uint32_t host_id) {
    auto it = g_bind.find(host_id);
    return it == g_bind.end() ? 0 : it->second;
}

bool MirrorPending(std::uint32_t host_id) {
    for (const Mirror& m : g_mirrors)
        if (m.host_id == host_id) return true;
    return false;
}

int NextPath(const Mirror& m) {
    static const int order[3] = {1, 2, 3};
    for (int p : order) {
        if (m.tried & (1u << p)) continue;
        if (g_path_pref && p != g_path_pref) continue;
        if (p == 1 && !g_exact_ok) continue;
        return p;
    }
    return 0;
}

const char* PathName(int p) { return p == 1 ? "exact-id" : p == 2 ? "fallback spawn" : p == 3 ? "re-place" : "?"; }

// Starts the mirror's next path; false when none is left (or this one could not even start: the caller tries the next).
bool StartPath(Mirror& m, const HostRec& h, double now) {
    const int path = NextPath(m);
    if (!path) return false;
    m.path = path;
    m.tried |= 1u << path;
    m.t = now;
    m.before.clear();
    for (const PUnit& p : g_poll.units)
        if (p.alive() && p.pool_kind == m.kind) m.before.push_back(p.id);
    const std::uintptr_t pool = PoolOf(m.kind);
    if (path == 1) {
        const PUnit* slot = PolledById(m.host_id);
        if (!pool || !slot || slot->in_use || slot->reserved()) {
            Log("[vshare] guest: mirror 0x%06X: exact-id path not possible (the unit with that id is %s)", m.host_id,
                !slot ? "missing" : slot->in_use ? "in use" : "reserved");
            return false;
        }
        // the loader's desc (0x58): type, transform, no placement, {entry, area}
        alignas(16) std::uint8_t desc[0x58] = {};
        alignas(8) std::uint8_t entry[vwire::kEntrySize + 12] = {};
        struct Pair {
            std::uintptr_t entry;
            std::uint16_t area;
        };
        alignas(8) Pair pair{};
        const std::uint32_t type = 0x4A91A5B3u;
        std::memcpy(desc, &type, 4);
        std::memcpy(desc + 8, &m.xf, 0x3C);
        const int area = CallArea();
        if (h.has_add && h.add.has_entry && area >= 0) {
            std::memcpy(entry, h.add.entry, vwire::kEntrySize);
            const std::uint32_t zero = 0;
            std::memcpy(entry + 0x30, &zero, 4);       // no online handle
            entry[0x3E] = 0;                           // LOCAL category: no online registration
            entry[0x41] = static_cast<std::uint8_t>(entry[0x41] & ~4u);  // not destroyed
            std::memcpy(entry + 0x2C, &m.host_id, 4);  // this id
            // The entry's place (+0x10 f32 x3, +0x1C quaternion) is applied after the desc's: an older ADD's place gives way to
            // the newest snapshot's position (the rotation stays the entry's).
            float ep[3];
            std::memcpy(ep, entry + 0x10, sizeof(ep));
            const double epd[3] = {ep[0], ep[1], ep[2]};
            if (vwire::Dist(epd, m.pos) > 1.0) {
                const float np[3] = {static_cast<float>(m.pos[0]), static_cast<float>(m.pos[1]), static_cast<float>(m.pos[2])};
                std::memcpy(entry + 0x10, np, sizeof(np));
                Log("[vshare] guest: mirror 0x%06X: the entry's place was %.1f m from the newest one: moved", m.host_id, vwire::Dist(epd, m.pos));
            }
            pair.entry = reinterpret_cast<std::uintptr_t>(entry);
            pair.area = static_cast<std::uint16_t>(area);
            const std::uintptr_t pp = reinterpret_cast<std::uintptr_t>(&pair);
            std::memcpy(desc + 0x50, &pp, 8);
        }
        const int r = CallExact(pool, reinterpret_cast<std::uintptr_t>(desc), m.host_id);
        Log("[vshare] guest: mirror 0x%06X (%s): exact-id path (pool vfunc +0x30, %s) -> %d", m.host_id, vwire::KindName(m.kind),
            pair.entry ? "with the host's entry" : "no entry", r);
        return r == 1;
    }
    if (path == 2) {
        bool free_unit = false;
        for (const PUnit& p : g_poll.units)
            if (p.pool_kind == m.kind && !p.in_use && !p.reserved()) free_unit = true;
        if (!pool || !free_unit) {
            Log("[vshare] guest: mirror 0x%06X: fallback spawn not possible (no free %s unit)", m.host_id, vwire::KindName(m.kind));
            return false;
        }
        Record rec;
        const std::uint16_t rec0 = h.has_add ? h.add.rec0 : static_cast<std::uint16_t>(m.kind | (((h.e.flags >> 22) & 0x3Fu) << 6));
        rec.Set(rec0, h.e.rec2, 0);
        const bool ok = CallSpawn(reinterpret_cast<std::uintptr_t>(rec.b), &m.xf, h.has_add ? h.add.p4 : 1, true);
        Log("[vshare] guest: mirror 0x%06X (%s): fallback spawn 0x142F830A0 (record 0x%04X, p6 0) %s", m.host_id, vwire::KindName(m.kind), rec0,
            ok ? "requested" : "FAULTED");
        return ok;
    }
    // path 3: a free unbound unit of that kind re-placed
    const PUnit* best = nullptr;
    double bd = 1e18;
    for (const PUnit& p : g_poll.units) {
        if (!p.alive() || p.pool_kind != m.kind || !p.entity || p.ridden() || p.HasGuid()) continue;
        bool bound = false;
        for (const auto& b : g_bind)
            if (b.second == p.id) bound = true;
        if (bound) continue;
        const double d = p.has_xf ? DistXf(p.xf, m.pos) : 1e17;
        if (d < bd) bd = d, best = &p;
    }
    if (!best) {
        Log("[vshare] guest: mirror 0x%06X: re-place not possible (no free unbound %s here)", m.host_id, vwire::KindName(m.kind));
        return false;
    }
    if (!PlaceClear(best->unit, m.kind, m.xf)) {
        Log("[vshare] guest: mirror 0x%06X: re-place not now (the host's spot holds our Sam or another vehicle here)", m.host_id);
        return false;
    }
    const bool ok = RequestTeleport(best->entity, m.xf);
    Log("[vshare] guest: mirror 0x%06X (%s): re-placed our unbound 0x%06X from %.1f m away %s", m.host_id, vwire::KindName(m.kind), best->id, bd,
        ok ? "(RequestTeleport)" : "FAILED");
    if (ok) {
        g_bind[m.host_id] = best->id;
        g_mirrored[best->id] = m.host_id;
        ++g_mirror_ok[3];
        m.path = -3;  // done
    }
    return ok;
}

void StartMirror(const HostRec& h, double now) {
    const std::uint32_t id = h.e.key.id;
    if (MirrorPending(id)) return;
    auto f = g_mirror_failed.find(id);
    if (f != g_mirror_failed.end() && now - f->second < 60000) return;
    Mirror m;
    m.host_id = id;
    m.kind = h.e.key.kind;
    m.xf = XfOf(h.e);
    std::memcpy(m.pos, h.e.pos, sizeof(m.pos));
    while (NextPath(m)) {
        if (StartPath(m, h, now)) {
            if (m.path == -3) return;  // re-placed at once
            g_mirrors.push_back(m);
            return;
        }
        ++g_mirror_fail[m.path > 0 ? m.path : 0];
    }
    g_mirror_failed[id] = now;
    Log("[vshare] guest: mirror 0x%06X (%s) FAILED: no path worked (tried mask 0x%X); again in 60 s", id, vwire::KindName(m.kind), m.tried);
}

void MirrorTick(double now) {
    for (auto it = g_mirrors.begin(); it != g_mirrors.end();) {
        Mirror& m = *it;
        auto tab = g_table.find(m.host_id);
        bool done = false, failed = false;
        if (m.path == 1) {
            // The unit with the host's id is in use: the mirror exists (one id, never a second vehicle). Its actor is spawned by the
            // pool's own update when it is near enough; Verify logs that (and whether it is a complete vehicle) when it happens.
            const PUnit* p = PolledById(m.host_id);
            if (p && p->alive()) {
                Log("[vshare] guest: mirror 0x%06X (%s): exact-id path OK: unit in use after %.1f s (state %u, %s)", m.host_id, vwire::KindName(m.kind),
                    (now - m.t) / 1000.0, p->state, p->entity ? "its actor is in the world" : "its actor spawns when near");
                g_bind[m.host_id] = p->id;
                g_mirrored[p->id] = m.host_id;  // it carries no placement GUID: the binding knows it is that vehicle
                Verify v;
                v.host_id = m.host_id;
                v.kind = m.kind;
                v.t = now;
                std::memcpy(v.pos, m.pos, sizeof(v.pos));
                g_verify.push_back(v);
                done = true;
            } else if (now - m.t > kMirrorExactMs) {
                Log("[vshare] guest: mirror 0x%06X: exact-id path gave no spawned vehicle within %.0f s (unit %s)", m.host_id, kMirrorExactMs / 1000.0,
                    p ? (p->in_use ? "in use, no entity" : "free") : "missing");
                failed = true;
            }
        } else if (m.path == 2) {
            const PUnit* found = nullptr;
            for (const PUnit& p : g_poll.units) {
                if (!p.alive() || p.pool_kind != m.kind || !p.has_xf) continue;
                if (std::find(m.before.begin(), m.before.end(), p.id) != m.before.end()) continue;
                if (DistXf(p.xf, m.pos) > 8.0) continue;
                found = &p;
                break;
            }
            if (found) {
                g_bind[m.host_id] = found->id;
                g_mirrored[found->id] = m.host_id;
                Log("[vshare] guest: mirror 0x%06X (%s): fallback spawn OK: our unit 0x%06X%s, after %.1f s", m.host_id, vwire::KindName(m.kind), found->id,
                    found->id == m.host_id ? " (the same id)" : " (id mapped)", (now - m.t) / 1000.0);
                done = true;
            } else if (now - m.t > kMirrorFallbackMs) {
                Log("[vshare] guest: mirror 0x%06X: fallback spawn gave no new %s within %.0f s", m.host_id, vwire::KindName(m.kind), kMirrorFallbackMs / 1000.0);
                failed = true;
            }
        } else {
            done = true;
        }
        if (done) {
            if (m.path > 0) ++g_mirror_ok[m.path];
            it = g_mirrors.erase(it);
            continue;
        }
        if (failed) {
            ++g_mirror_fail[m.path > 0 ? m.path : 0];
            bool started = false;
            if (tab != g_table.end()) {
                while (NextPath(m)) {
                    if (StartPath(m, tab->second, now)) {
                        started = true;
                        break;
                    }
                    ++g_mirror_fail[m.path > 0 ? m.path : 0];
                }
            }
            if (started && m.path != -3) {
                ++it;
                continue;
            }
            if (!started) {
                g_mirror_failed[m.host_id] = now;
                Log("[vshare] guest: mirror 0x%06X FAILED on every path (tried mask 0x%X); again in 60 s", m.host_id, m.tried);
            }
            it = g_mirrors.erase(it);
            continue;
        }
        ++it;
    }
}

void Reconcile(double now, bool log_always) {
    if (!g_have_table || !g_poll.ok || !Settled(now)) return;
    // a bound unit missing from this poll (a torn read): the plan would take it for absent and mirror a second vehicle
    // (waits at most 10 rounds: a unit that is gone from its pool for good is planned without)
    static unsigned skips = 0;
    bool missing = false;
    for (const auto& b : g_bind)
        if (!PolledById(b.second)) missing = true;
    if (missing && ++skips <= 10) return;
    skips = 0;
    std::vector<vwire::SnapEntry> host;
    host.reserve(g_table.size());
    for (const auto& kv : g_table) host.push_back(kv.second.e);
    std::vector<vwire::LocalUnit> local;
    std::vector<const PUnit*> lp;
    for (const PUnit& p : g_poll.units) {
        if (!p.in_use) continue;
        vwire::LocalUnit u;
        u.id = p.id;
        u.kind = static_cast<std::uint8_t>(p.pool_kind);
        u.has_entity = p.entity != 0;
        u.has_pos = p.has_xf;
        if (p.has_xf) u.pos[0] = p.xf.pos.x, u.pos[1] = p.xf.pos.y, u.pos[2] = p.xf.pos.z;
        std::memcpy(u.guid, p.guid, 16);
        u.removed = p.removed();
        u.ridden = p.ridden();
        for (const auto& b : g_bind)
            if (b.second == p.id && b.first != p.id) u.bound_host_id = b.first;
        if (p.alive()) {
            auto mm = g_mirrored.find(p.id);
            if (mm != g_mirrored.end()) u.mirrored_for = mm->second;
        }
        for (const Mirror& m : g_mirrors)
            if (m.path == 2 && m.kind == u.kind && std::find(m.before.begin(), m.before.end(), p.id) == m.before.end()) u.pending_mirror = true;
        local.push_back(u);
        lp.push_back(&p);
    }
    vwire::BindParams bp;
    bp.mirrors_allowed = g_bound_once && now - g_first_bind_ms >= kGraceMs;
    const int area = CallArea();
    bp.current_area = static_cast<std::uint16_t>(area < 0 ? 0 : area);
    const vwire::BindPlan plan = vwire::PlanBinding(host, local, bp);
    // apply
    std::map<std::uint32_t, std::uint32_t> bind;
    for (const vwire::BindStep& s : plan.steps) {
        const vwire::SnapEntry& h = host[static_cast<std::size_t>(s.host)];
        const PUnit* u = s.local >= 0 ? lp[static_cast<std::size_t>(s.local)] : nullptr;
        const HostRec& rec = g_table[h.key.id];
        const bool followed = g_followed && g_followed(h.key.id);
        switch (s.kind) {
            case vwire::BindKind::bound:
            case vwire::BindKind::bound_unverified:
                bind[h.key.id] = u->id;
                break;
            case vwire::BindKind::replace:
                bind[h.key.id] = u->id;
                if (!followed && u->entity && !HeldNote(h.key.id, HoldOfPair(h.key.id, u->id), "guest: not re-placed onto the host's spot", now)) {
                    const WorldTransform to = XfOf(h);
                    // only where it fits (M2.6): the host's spot may hold our Sam or another vehicle here; tried again next round
                    if (!PlaceClear(u->unit, h.key.kind, to)) {
                        if (Cool(0x40000000u | h.key.id, now, 10000))
                            Log("[vshare] guest: our %s 0x%06X waits to be re-placed onto the host's 0x%06X (%.1f m away): the spot is taken here",
                                vwire::KindName(h.key.kind), u->id, h.key.id, s.dist);
                    } else if (Cool(h.key.id, now, 30000) && RequestTeleport(u->entity, to)) {
                        ++g_replaced;
                        Log("[vshare] guest: re-placed our %s 0x%06X onto the host's 0x%06X (%.1f m away)", vwire::KindName(h.key.kind), u->id, h.key.id, s.dist);
                    }
                }
                break;
            case vwire::BindKind::remove_then_mirror:
                if (followed && u->pool_kind == h.key.kind && std::memcmp(u->guid, h.key.guid, 16) == 0) {
                    // the same vehicle (kind, GUID), driven away from the table's place: the drive module owns or follows it,
                    // so it is never removed here (its corrections put it where the owner has it)
                    bind[h.key.id] = u->id;
                } else if (u->alive()) {
                    if (RemoveLocal(*u, "another vehicle under the host's id", now)) ++g_removed_wrong;
                } else if (bp.mirrors_allowed && !MirrorPending(h.key.id) && !HeldNote(h.key.id, HoldOfHost(h.key.id), "guest: not mirrored", now)) {
                    if (rec.has_add) StartMirror(rec, now);
                    else SendNeed(h.key, 0, now);
                }
                break;
            case vwire::BindKind::remove_local:
                if (RemoveLocal(*u, "the host removed it", now)) ++g_removed_host;
                break;
            case vwire::BindKind::mirror:
                if (!MirrorPending(h.key.id) && !HeldNote(h.key.id, HoldOfHost(h.key.id), "guest: not mirrored", now)) {
                    if (rec.has_add) StartMirror(rec, now);
                    else SendNeed(h.key, 0, now);
                }
                break;
            case vwire::BindKind::wait:
            case vwire::BindKind::gone:
                break;
        }
    }
    // keep the mappings of mirrors in flight and of re-placed units the plan saw as extras this round
    for (const auto& b : g_bind)
        if (!bind.count(b.first) && g_table.count(b.first) && PolledById(b.second) && PolledById(b.second)->alive()) bind[b.first] = b.second;
    g_bind.swap(bind);
    for (const vwire::ExtraStep& x : plan.extras) {
        if (x.kind != vwire::ExtraKind::remove) continue;
        const PUnit* u = lp[static_cast<std::size_t>(x.local)];
        bool mapped = false;
        for (const auto& b : g_bind)
            if (b.second == u->id) mapped = true;
        if (mapped) continue;
        if (RemoveLocal(*u, "not in the host's world", now)) ++g_removed_extra;
    }
    if (!g_bound_once) {
        g_bound_once = true;
        g_first_bind_ms = now;
    }
    char line[240];
    std::snprintf(line, sizeof(line),
                  "%zu host vehicles, %zu ours in use: bound %u (+%u unverified), re-place %u, remove+mirror %u, removed by host %u, mirror %u, wait %u, "
                  "gone %u; extras removed %u, kept %u",
                  host.size(), local.size(), plan.bound, plan.unverified, plan.replace, plan.remove_mirror, plan.remove_local, plan.mirror, plan.wait,
                  plan.gone, plan.extra_remove, plan.extra_kept);
    if (log_always || std::strcmp(line, g_last_plan_line) != 0) {
        Log("[vshare] guest: binding: %s", line);
        std::snprintf(g_last_plan_line, sizeof(g_last_plan_line), "%s", line);
    }
    g_last_plan = plan;
    g_last_plan_host = host;
    g_last_plan_local = local;
}

// A bound vehicle of ours went (and not by our hand): the host decides (M2.4). Runs before the binding forgets it. Only a
// unit this poll saw, not alive, and still not alive read now counts (a unit missing from one poll is a torn read).
void GuestOwnRemovals(double now) {
    for (auto& prev : g_prev_bound) {
        const PUnit* p = PolledById(prev.first);
        if (!p || p->alive()) continue;
        std::uint8_t use = 0, fb0 = 0;
        if (!mem::Rd(p->unit + kUInUse, use) || !mem::Rd(p->unit + kUFlagBytes, fb0) || (use != 0 && !(fb0 & 0x40))) continue;
        auto r = g_removing.find(prev.first);
        if (r != g_removing.end() && now - r->second < 15000) continue;
        auto t = g_table.find(prev.second.host_id);
        if (t == g_table.end() || t->second.e.Has(vwire::kEntRemoved)) continue;
        // round 23 D: a truck left to each game went in our world only (a MULE's truck destroyed or despawned here): not the host's business
        if (HeldNote(prev.second.host_id, HoldOfPair(prev.second.host_id, prev.first), "guest: its going is not told to the host", now)) continue;
        // its last place (the poll before it went): the host refuses a removal without one
        SendRem(t->second.e.key, vwire::RemReason::gone, 1, prev.second.has_xf, prev.second.xf);
        g_rem_told[prev.second.host_id] = now;
        ++g_guest_rem_sent;
        Log("[vshare] guest: our %s 0x%06X (the host's 0x%06X) went in our world: told the host", vwire::KindName(t->second.e.key.kind), prev.first,
            prev.second.host_id);
    }
    for (auto it = g_removing.begin(); it != g_removing.end();)
        it = now - it->second > 60000 ? g_removing.erase(it) : std::next(it);
    for (auto it = g_rem_told.begin(); it != g_rem_told.end();)
        it = now - it->second > 60000 ? g_rem_told.erase(it) : std::next(it);
}

void RememberBound() {
    std::map<std::uint32_t, PrevBound> next;
    for (const auto& b : g_bind) {
        const PUnit* p = PolledById(b.second);
        if (!p || !p->alive()) continue;
        PrevBound& n = next[b.second];
        n.host_id = b.first;
        n.has_xf = p->has_xf;
        n.xf = p->xf;
    }
    // missing from this poll (not seen at all): carried over for up to 5 polls with their last place
    for (const auto& prev : g_prev_bound)
        if (!next.count(prev.first) && !PolledById(prev.first) && prev.second.misses < 5) {
            PrevBound n = prev.second;
            ++n.misses;
            next[prev.first] = n;
        }
    g_prev_bound.swap(next);
    // the mod's mirrors: forgotten once their unit is not alive
    for (auto it = g_mirrored.begin(); it != g_mirrored.end();) {
        const PUnit* p = PolledById(it->first);
        it = p && !p->alive() ? g_mirrored.erase(it) : std::next(it);
    }
}

void GuestForward(double now) {
    vwire::Req got[kFwdCap];
    unsigned n = 0;
    AcquireSRWLockExclusive(&g_fwd_lock);
    n = g_fwd_n;
    for (unsigned i = 0; i < n; ++i) got[i] = g_fwd[i];
    g_fwd_n = 0;
    ReleaseSRWLockExclusive(&g_fwd_lock);
    for (unsigned i = 0; i < n; ++i) {
        vwire::Req& q = got[i];
        if (!g_fwd_limit.Allow(now)) {
            Log("[vshare] guest: a %s request dropped (more than 3 in 6 s)", vwire::KindName(q.kind));
            continue;
        }
        q.token = ++g_next_token;
        if (!q.token) q.token = ++g_next_token;
        std::uint8_t buf[vwire::kMaxMessage];
        const std::size_t len = vwire::EncodeReq(q, buf, sizeof(buf));
        if (!len || !Queue(buf, len)) {
            Log("[vshare] guest: a %s request could not be sent", vwire::KindName(q.kind));
            continue;
        }
        Summon s;
        s.token = q.token;
        s.kind = q.kind;
        s.t = now;
        g_summons.push_back(s);
        char text[120];
        std::snprintf(text, sizeof(text), "Summoning the %s...", vwire::KindName(q.kind));
        Notice(text, false, now + kSummonNoticeMs);
        Log("[vshare] guest: our game asked for a %s%s at (%.2f, %.2f, %.2f): dropped here, sent to the host (request #%u)", vwire::KindName(q.kind),
            q.garage_slot >= 0 ? " (garage slot)" : "", q.pos[0], q.pos[1], q.pos[2], q.token);
    }
    for (auto it = g_summons.begin(); it != g_summons.end();) {
        if (now - it->t > kSummonNoticeMs) {
            Log("[vshare] guest: request #%u: no answer from the host within %.0f s", it->token, kSummonNoticeMs / 1000.0);
            it = g_summons.erase(it);
        } else {
            ++it;
        }
    }
}

void GuestTick(double now) {
    if (!g_ready) return;
    // the host's table after every load, and once in a while
    const bool need = !g_have_table || g_table_counter != g_counter;
    if (Settled(now) && g_connected &&
        ((need && now - g_asked_ms > kSnapRetryMs) || (!need && now - g_asked_ms > kSnapPeriodMs && now - g_table_ms > kSnapPeriodMs)))
        SendSnapReq(need ? 0 : 1, now);
    if (!g_poll.ok) return;
    MirrorTick(now);
    VerifyTick(now);
    if (g_bound_once) GuestOwnRemovals(now);
    if (g_reconcile_now || now >= g_next_reconcile) {
        Reconcile(now, false);
        g_reconcile_now = false;
        g_next_reconcile = now + kReconcileMs;
    }
    RememberBound();
    GuestForward(now);
}

// ---------------------------------------------------------------- incoming
void OnMessage(const std::uint8_t* b, std::size_t n, std::uint32_t gen, double now) {
    std::uint8_t type = 0;
    if (!vwire::PeekType(b, n, &type)) {
        ++g_rx_bad;
        return;
    }
    ++g_rx;
    if (vwire::IsExternalType(type)) {
        if (g_handlers[type]) g_handlers[type](type, b, n, gen, now);
        return;
    }
    switch (static_cast<vwire::MsgType>(type)) {
        case vwire::MsgType::snap_req: {
            vwire::SnapReq r;
            if (g_role != 1 || !vwire::DecodeSnapReq(b, n, r)) break;
            if (r.load_counter != g_peer_world)
                Log("[vshare] host: the guest is in world #%u now (ours #%u)%s", r.load_counter, g_counter,
                    r.load_counter == g_counter ? "" : ": its vehicle messages are dropped until it is in ours");
            g_peer_world = r.load_counter;
            Log("[vshare] host: the guest asks for our vehicles (#%u, its world #%u)", r.req_id, r.load_counter);
            if (g_snap_reqs.size() < 4) g_snap_reqs.push_back(r);
            return;
        }
        case vwire::MsgType::snap: {
            vwire::SnapChunk c;
            if (g_role != 2 || !vwire::DecodeSnapChunk(b, n, c)) break;
            const auto step = g_asm.Add(c);
            if (step == vwire::SnapAssembler::Step::rejected) {
                Log("[vshare] guest: snapshot chunk %u/%u of #%u rejected (out of order or inconsistent)", c.index + 1u, c.count, c.snap_id);
                return;
            }
            if (step == vwire::SnapAssembler::Step::complete) {
                // the snapshot replaces the table (the ADDs' entries of vehicles still there are kept)
                std::map<std::uint32_t, HostRec> t;
                for (const vwire::SnapEntry& e : g_asm.Entries()) {
                    HostRec& h = t[e.key.id];
                    h.e = e;
                    auto old = g_table.find(e.key.id);
                    if (old != g_table.end() && old->second.has_add && old->second.e.key == e.key) {
                        h.has_add = true;
                        h.add = old->second.add;
                    }
                }
                g_table.swap(t);
                g_have_table = true;
                g_table_counter = g_counter;
                g_table_ms = now;
                g_reconcile_now = true;
                g_peer_world = g_asm.LoadCounter();
                g_host_answered = true;
                g_snap_wait_since = 0;
                if (g_host_silent_logged) {
                    g_host_silent_logged = false;
                    Log("[vshare] guest: the host's game answers again: the real shared vehicles are in effect");
                }
                Log("[vshare] guest: the host's snapshot #%u is complete: %zu vehicles (host world #%u, area %u)", g_asm.SnapId(), g_table.size(),
                    g_asm.LoadCounter(), g_asm.Area());
                g_asm.Reset();
                LogTrucks("guest: the host's snapshot");  // round 23 D (vehicle_mule_log)
            }
            return;
        }
        case vwire::MsgType::add: {
            vwire::Add a;
            if (g_role != 2 || !vwire::DecodeAdd(b, n, a)) break;
            HostRec& h = g_table[a.unit.key.id];
            h.e = a.unit;
            h.has_add = true;
            h.add = a;
            g_mirror_failed.erase(a.unit.key.id);
            g_reconcile_now = true;
            if (a.source == vwire::AddSource::guest_request) {
                for (auto it = g_summons.begin(); it != g_summons.end(); ++it)
                    if (it->token == a.token) {
                        Log("[vshare] guest: request #%u answered: the host's %s 0x%06X after %.1f s", a.token, vwire::KindName(it->kind), a.unit.key.id,
                            (now - it->t) / 1000.0);
                        Notice("", false, 0);
                        g_summons.erase(it);
                        break;
                    }
            }
            if (a.source == vwire::AddSource::reannounce) {
                auto told = g_rem_told.find(a.unit.key.id);
                if (told != g_rem_told.end() && now - told->second < 15000) {
                    g_rem_told.erase(told);
                    char text[200];
                    std::snprintf(text, sizeof(text), "The host's game keeps the %s: a shared vehicle is stored or removed in the host's game only.",
                                  vwire::KindName(a.unit.key.kind));
                    Notice(text, true, now + 8000);
                    Log("[vshare] guest: the host kept the %s 0x%06X we removed: it comes back here", vwire::KindName(a.unit.key.kind), a.unit.key.id);
                }
            }
            Log("[vshare] guest: ADD from the host: %s 0x%06X (%s%s)", vwire::KindName(a.unit.key.kind), a.unit.key.id,
                a.source == vwire::AddSource::host_own ? "its spawn" : a.source == vwire::AddSource::guest_request ? "our request" : "announced again",
                a.has_entry ? ", with its entry" : "");
            return;
        }
        case vwire::MsgType::rem: {
            vwire::Rem r;
            if (!vwire::DecodeRem(b, n, r)) break;
            if (g_role == 2 && r.from == 0) g_peer_world = r.world;
            if (r.world != g_counter || (g_role == 1 && g_peer_world != g_counter)) {
                Log("[vshare] %s: dropped a REM of 0x%06X from world #%u (ours #%u%s)", g_role == 1 ? "host" : "guest", r.key.id, r.world, g_counter,
                    g_role == 1 && g_peer_world != g_counter ? ", the guest not in it" : "");
                return;
            }
            if (g_role == 1 && r.from == 1) {
                HostGuestRemoval(r);
                return;
            }
            if (g_role == 2 && r.from == 0) {
                auto it = g_table.find(r.key.id);
                if (it != g_table.end()) {
                    it->second.e.eflags |= vwire::kEntRemoved;
                    it->second.has_add = false;
                }
                g_reconcile_now = true;
                Log("[vshare] guest: REM from the host: %s 0x%06X", vwire::KindName(r.key.kind), r.key.id);
                return;
            }
            break;
        }
        case vwire::MsgType::req: {
            vwire::Req q;
            if (g_role != 1 || !vwire::DecodeReq(b, n, q)) break;
            Log("[vshare] host: the guest's game asks for a %s (request #%u%s) at (%.2f, %.2f, %.2f)", vwire::KindName(q.kind), q.token,
                q.garage_slot >= 0 ? ", garage slot" : "", q.pos[0], q.pos[1], q.pos[2]);
            HostServeRequest(q, now);
            return;
        }
        case vwire::MsgType::req_result: {
            vwire::Result r;
            if (g_role != 2 || !vwire::DecodeResult(b, n, r)) break;
            for (auto it = g_summons.begin(); it != g_summons.end(); ++it)
                if (it->token == r.token) {
                    char text[160];
                    if (r.result == vwire::ReqResult::duplicate) std::snprintf(text, sizeof(text), "The %s is there already.", vwire::KindName(it->kind));
                    else std::snprintf(text, sizeof(text), "The host's game could not summon the %s (%s).", vwire::KindName(it->kind), vwire::ReqResultName(static_cast<std::uint8_t>(r.result)));
                    Notice(text, r.result != vwire::ReqResult::duplicate, now + 6000);
                    g_summons.erase(it);
                    break;
                }
            Log("[vshare] guest: request #%u: %s", r.token, vwire::ReqResultName(static_cast<std::uint8_t>(r.result)));
            return;
        }
        case vwire::MsgType::need: {
            vwire::Need m;
            if (g_role != 1 || !vwire::DecodeNeed(b, n, m)) break;
            const PUnit* p = PolledById(m.key.id);
            if (p && p->alive() && p->pool_kind == m.key.kind) SendAdd(*p, vwire::AddSource::reannounce, 0);
            else Log("[vshare] host: the guest needs 0x%06X, which we no longer have", m.key.id);
            return;
        }
        default:
            break;
    }
    ++g_rx_bad;
    Log("[vshare] dropped a %s message (%zu bytes): not valid here", vwire::MsgTypeName(type), n);
}

void NewLinkSession() {
    g_out.clear();
    g_asm.Reset();
    g_snap_reqs.clear();
    g_hreqs.clear();
    g_have_table = false;  // asked again
    g_asked_ms = -1e18;
    g_hbase = false;       // the host's diff starts over (the guest asks for a snapshot)
    g_peer_world = 0;      // the partner names its world again
    g_host_answered = false;
    g_snap_wait_since = 0;
    Log("[vshare] a new link session: the vehicle tables start over");
}

// ---------------------------------------------------------------- round 23 D: MULE trucks (vehicle_truck_rules.hpp)
// Each poll: every truck of this world (and any vehicle a non-player drives) classified from its unit (+0x158 rider, +0x19D
// bits) and its entity (+0x63C seated, +0x8B0 driver id; the model from its resource); both games send what their trucks say
// (STK1, SVM1 type 50, the host's ids); the hold (trucks::Hold with its 5 s latch) per host id. The binding, the removals and
// vehicle_drive ask the hold. Its own SEH guard: a fault turns the truck rules off for the run (the shared vehicles go on).
constexpr std::uintptr_t kEHasDriver = 0x63C, kEDriver = 0x8B0;
constexpr std::uint32_t kUnboundKey = 0x80000000u;  // guest: a unit that is not bound, by its own id
constexpr unsigned kHoldLines = 24;                 // hold on / off lines per world (then counted)
bool g_mule_log = true, g_mule_hold = true;         // session.ini
bool g_trucks_ok = false;                           // the kTrucks byte checks (with the shared vehicles on)
bool g_trucks_fault = false;                        // the truck tick faulted: off for the run
struct TruckSt {
    std::uint32_t local_id = 0, host_id = 0;
    std::uintptr_t unit = 0, entity = 0;
    trucks::UnitFacts u;
    trucks::Facts f;
    std::int32_t seat = -1;
    char camp[24] = "-";
    bool has_xf = false;
    WorldTransform xf{};
    std::uint32_t uflags = 0;  // unit +0x198 (the census line: its variant bits >> 22)
    std::uint16_t rec2 = 0;
};
std::map<std::uint32_t, TruckSt> g_trucks;  // this world's, by local id (rebuilt every poll)
struct ModelSeen {
    std::uint8_t guid[16] = {};
    trucks::Model m = trucks::Model::unknown;
};
std::map<std::uint32_t, ModelSeen> g_models;  // local id -> its model once read (the entity's resource)
struct ThereFact {
    trucks::Fact f;
    double ms = 0;
};
std::map<std::uint32_t, ThereFact> g_there;  // host id -> the other game's newest facts
struct HoldSt {
    trucks::HoldLatch latch;
    double seen_ms = 0;
    bool player_last = false;  // round 23 review: trucks::PlayerLast (a player drove it last, no non-player since)
    bool player_in = false;    // a player drove it on the previous poll
};
std::map<std::uint32_t, HoldSt> g_holds;  // host id (guest: kUnboundKey | an unbound unit's id) -> its hold
std::vector<trucks::Fact> g_facts_sent;
double g_facts_sent_ms = -1e18;
unsigned g_facts_tx = 0, g_facts_rx = 0, g_facts_bad = 0, g_facts_stale = 0;
bool g_facts_rx_logged = false;
unsigned g_hold_lines = 0, g_hold_lines_dropped = 0;
unsigned g_held_skips = 0;                     // decision points that left a truck alone
std::map<std::uint64_t, double> g_held_note;   // (id, what) -> its last line

bool TrucksOn() { return g_trucks_ok && !g_trucks_fault && g_on.load(); }
bool HoldOn() { return g_mule_hold && TrucksOn(); }

std::uint32_t LocalToHost(std::uint32_t local_id) {
    if (g_role == 1) return local_id;
    for (const auto& b : g_bind)
        if (b.second == local_id) return b.first;
    return 0;
}
std::uint32_t HoldKeyOf(const TruckSt& t) { return t.host_id ? t.host_id : kUnboundKey | t.local_id; }

trucks::HoldWhy HoldOfKey(std::uint32_t key) {
    if (!HoldOn()) return trucks::HoldWhy::none;
    const auto it = g_holds.find(key);
    return it == g_holds.end() ? trucks::HoldWhy::none : it->second.latch.Current();
}
trucks::HoldWhy HoldOfHost(std::uint32_t host_id) { return host_id ? HoldOfKey(host_id) : trucks::HoldWhy::none; }
// A host id and this world's unit (either 0): the host id's hold, else the unit's own (a guest unit whose binding is new).
trucks::HoldWhy HoldOfPair(std::uint32_t host_id, std::uint32_t local_id) {
    const trucks::HoldWhy w = HoldOfHost(host_id);
    if (w != trucks::HoldWhy::none || !local_id) return w;
    const std::uint32_t h = LocalToHost(local_id);
    return HoldOfKey(h ? h : kUnboundKey | local_id);
}
// A decision point leaves the truck alone: counted, one line per truck and decision every 30 s.
bool HeldNote(std::uint32_t id, trucks::HoldWhy w, const char* what, double now) {
    if (w == trucks::HoldWhy::none) return false;
    ++g_held_skips;
    std::uint64_t k = static_cast<std::uint64_t>(id) << 32;
    for (const char* c = what; *c; ++c) k = k * 131u + static_cast<unsigned char>(*c);
    double& last = g_held_note[k];
    if (last == 0 || now - last >= 30000) {
        last = now;
        Log("[vshare] MULE truck 0x%06X left to each game (%s): %s", id, trucks::HoldWhyName(w), what);
    }
    return true;
}

// The model of a truck unit: its entity's resource ("truck_npc" pe_ds_vehicle_celd: a MULE truck); kept per unit (id, GUID).
trucks::Model ModelOfUnit(const PUnit& p) {
    if (p.pool_kind != trucks::kKindTruck) return trucks::Model::player;
    const auto it = g_models.find(p.id);
    if (it != g_models.end() && std::memcmp(it->second.guid, p.guid, 16) == 0) return it->second.m;
    if (!p.entity) return trucks::Model::unknown;
    char path[160] = {};
    std::uint8_t g[16] = {};
    if (!realbody::ResourceIdentity(reinterpret_cast<game::Entity*>(p.entity), path, sizeof(path), g, nullptr)) return trucks::Model::unknown;
    const wire::VehicleResource* r = wire::FindVehicleResource(g);
    trucks::Model m = trucks::Model::unknown;
    if (r) m = trucks::ModelOfKey(r->key, trucks::kKindTruck);
    else if (std::strstr(path, "pe_ds_vehicle_celd")) m = trucks::Model::celd;
    else if (std::strstr(path, "pe_ds_vehicle_ca02")) m = trucks::Model::player;
    if (m != trucks::Model::unknown) {
        ModelSeen s;
        std::memcpy(s.guid, p.guid, 16);
        s.m = m;
        g_models[p.id] = s;
    }
    return m;
}

// One vehicle's facts (fault-free reads of the entity's seat).
trucks::UnitFacts FactsOfUnit(const PUnit& p, trucks::Model m) {
    trucks::UnitFacts u;
    u.kind = static_cast<std::uint8_t>(p.pool_kind);
    u.model = m;
    u.rider = p.rider;
    u.b19d = p.fb[1];
    if (p.entity) {
        std::uint32_t has = 0, drv = trucks::kNoActor;
        if (mem::Rd(p.entity + kEHasDriver, has) && mem::Rd(p.entity + kEDriver, drv)) u.ent = true, u.ent_has = has, u.ent_driver = drv;
    }
    return u;
}

// A unit the truck rules look at: a truck, or any vehicle a non-player rides.
bool TruckCandidate(const PUnit& p) { return p.alive() && (p.pool_kind == trucks::kKindTruck || trucks::NonPlayer(trucks::DriverOf(p.rider))); }

TruckSt ReadTruck(const PUnit& p) {
    TruckSt t;
    t.local_id = p.id;
    t.unit = p.unit, t.entity = p.entity;
    t.u = FactsOfUnit(p, ModelOfUnit(p));
    t.f = trucks::Classify(t.u);
    t.has_xf = p.has_xf, t.xf = p.xf;
    t.uflags = p.flags, t.rec2 = p.rec2;
    if (trucks::NonPlayer(t.f.driver)) esync::MuleRideInfo(t.f.driver_id, t.camp, sizeof(t.camp), &t.seat);
    return t;
}

void HoldLine(std::uint32_t key, bool on, const char* text) {
    if (g_hold_lines >= kHoldLines) {
        ++g_hold_lines_dropped;
        return;
    }
    ++g_hold_lines;
    if (key & kUnboundKey) Log("[vshare] MULE truck (our unbound 0x%06X) %s (%s)", key & ~kUnboundKey, on ? "left to each game" : "shared again", text);
    else Log("[vshare] MULE truck 0x%06X %s (%s)", key, on ? "left to each game" : "shared again", text);
}
// Round 23 review (trucks::PlayerLast): the "a player drove it last" changes (the same line budget).
void PlayerLastLine(std::uint32_t key, const char* text) {
    if (g_hold_lines >= kHoldLines) {
        ++g_hold_lines_dropped;
        return;
    }
    ++g_hold_lines;
    if (key & kUnboundKey) Log("[vshare] MULE truck (our unbound 0x%06X): %s", key & ~kUnboundKey, text);
    else Log("[vshare] MULE truck 0x%06X: %s", key, text);
}

void SendTruckFacts(double now) {
    if (!g_connected || !g_ready || !PeerInOurWorld()) return;
    std::vector<trucks::Fact> cur;
    for (const auto& kv : g_trucks) {
        const TruckSt& t = kv.second;
        if (!t.host_id || t.host_id >= trucks::kNoActor) continue;
        const std::uint8_t seat = t.seat >= 0 && t.seat < 255 ? static_cast<std::uint8_t>(t.seat) : trucks::kNoSeat;
        cur.push_back(trucks::FactOf(t.host_id, t.u.model, t.f, seat));
    }
    // the MULE ones first when there are more than one message holds
    std::stable_sort(cur.begin(), cur.end(), [](const trucks::Fact& a, const trucks::Fact& b) {
        const bool ma = a.model == trucks::Model::celd || trucks::NonPlayer(a.driver), mb = b.model == trucks::Model::celd || trucks::NonPlayer(b.driver);
        return ma != mb ? ma : a.host_id < b.host_id;
    });
    if (cur.size() > trucks::kMaxFacts) cur.resize(trucks::kMaxFacts);
    bool changed = cur.size() != g_facts_sent.size();
    for (std::size_t i = 0; !changed && i < cur.size(); ++i)
        changed = cur[i].host_id != g_facts_sent[i].host_id || trucks::FactChanged(cur[i], g_facts_sent[i]);
    const double since = now - g_facts_sent_ms;
    if (!((changed && since >= trucks::kFactsMinGapMs) || since >= trucks::kFactsAllMs)) return;
    static trucks::FactsMsg m;
    m.from = static_cast<std::uint8_t>(g_role);
    m.world = static_cast<std::uint16_t>((g_role == 1 ? g_counter : g_peer_world) & 0xFFFFu);
    m.n = static_cast<std::uint8_t>(cur.size());
    for (std::size_t i = 0; i < cur.size(); ++i) m.e[i] = cur[i];
    std::uint8_t buf[vwire::kMaxMessage];
    const std::size_t hs = vwire::WriteHeader(trucks::kFactsType, buf, sizeof(buf));
    const std::size_t bs = hs ? trucks::EncodeFacts(m, buf + hs, sizeof(buf) - hs) : 0;
    if (!bs) return;
    g_facts_sent_ms = now;  // (a full outbox: tried again in 1 / 10 s)
    if (!Queue(buf, hs + bs)) return;
    g_facts_sent.swap(cur);
    ++g_facts_tx;
}

void OnTruckFacts(std::uint8_t, const std::uint8_t* msg, std::size_t size, std::uint32_t gen, double now) {
    if (gen != g_gen || !TrucksOn()) return;
    static trucks::FactsMsg m;
    if (size < vwire::kHeaderSize || !trucks::DecodeFacts(msg + vwire::kHeaderSize, size - vwire::kHeaderSize, m) || m.from != (g_role == 1 ? 2 : 1)) {
        if (g_facts_bad++ < 3) Log("[vshare] the partner's truck facts (STK1, %zu bytes) did not validate: dropped", size);
        return;
    }
    const std::uint16_t ours = static_cast<std::uint16_t>((g_role == 1 ? g_counter : g_peer_world) & 0xFFFFu);
    if (m.world != ours) {
        ++g_facts_stale;  // another world's (a reload on either side)
        return;
    }
    g_there.clear();  // every message is the sender's whole list
    unsigned npc = 0, celd = 0;
    for (std::size_t i = 0; i < m.n; ++i) {
        g_there[m.e[i].host_id] = ThereFact{m.e[i], now};
        if (trucks::NonPlayer(m.e[i].driver)) ++npc;
        if (m.e[i].model == trucks::Model::celd) ++celd;
    }
    ++g_facts_rx;
    if (!g_facts_rx_logged) {
        g_facts_rx_logged = true;
        Log("[vshare] the partner's game sends its truck facts: %u trucks (%u MULE trucks, %u driven by a non-player)", m.n, celd, npc);
    }
}

void TrucksTickBody(double now) {
    std::map<std::uint32_t, TruckSt> next;
    for (const PUnit& p : g_poll.units) {
        if (!TruckCandidate(p)) continue;
        TruckSt t = ReadTruck(p);
        t.host_id = LocalToHost(p.id);
        next[p.id] = t;
    }
    g_trucks.swap(next);
    for (auto it = g_there.begin(); it != g_there.end();) it = now - it->second.ms > trucks::kFactsExpireMs ? g_there.erase(it) : std::next(it);
    // the holds: every truck either game names
    std::map<std::uint32_t, const TruckSt*> here;
    for (const auto& kv : g_trucks) here[HoldKeyOf(kv.second)] = &kv.second;
    std::vector<std::uint32_t> keys;
    for (const auto& kv : here) keys.push_back(kv.first);
    for (const auto& kv : g_there)
        if (!here.count(kv.first)) keys.push_back(kv.first);
    const bool on = g_mule_hold;
    for (std::uint32_t key : keys) {
        trucks::HoldIn in;
        in.on = on;
        in.followed = !(key & kUnboundKey) && g_followed && g_followed(key);
        const auto h = here.find(key);
        if (h != here.end()) in.have_here = true, in.here_driver = h->second->f.driver, in.here_model = h->second->u.model;
        const auto t = g_there.find(key);
        if (t != g_there.end()) in.have_there = true, in.there_driver = t->second.f.driver, in.there_model = t->second.f.model;
        HoldSt& st = g_holds[key];
        // round 23 review: a truck a player drove last stays shared (its final placement and PARK frames go out)
        in.player_last = trucks::PlayerLast(st.player_last, in);
        if (in.on && st.player_last && !in.player_last)
            PlayerLastLine(key, "a non-player drives it: a player's last drive no longer keeps it shared");
        else if (in.on && in.player_last && st.player_in && !trucks::PlayerDrives(in) &&
                 ((in.have_here && in.here_model == trucks::Model::celd) || (in.have_there && in.there_model == trucks::Model::celd)))
            PlayerLastLine(key, "the player got out: kept shared (a player drove it last) until a non-player drives it");
        st.player_in = trucks::PlayerDrives(in);
        st.player_last = in.player_last;
        const trucks::HoldWhy raw = trucks::Hold(in);
        st.seen_ms = now;
        const bool was = st.latch.Held();
        bool changed = false;
        const trucks::HoldWhy w = st.latch.Step(raw, trucks::PlayerDrives(in) || !in.on || in.followed, now, &changed);
        if (!changed) continue;
        if (!was) {
            char text[160];
            if (h != here.end())
                std::snprintf(text, sizeof(text), "%s; %s, driver 0x%06X%s%s", trucks::HoldWhyName(w), trucks::ClassName(h->second->f.cls), h->second->f.driver_id,
                              h->second->f.agree ? "" : ", its fields disagree", in.have_there ? "" : ", no facts from the other game");
            else
                std::snprintf(text, sizeof(text), "%s; the other game's %s truck, driver 0x%06X", trucks::HoldWhyName(w), trucks::ModelName(t->second.f.model),
                              t->second.f.driver_id);
            HoldLine(key, true, text);
        } else {
            HoldLine(key, false, trucks::PlayerDrives(in) ? "a player drives it" : in.followed ? "the partner drives it" : !in.on ? "vehicle_mule_hold is off"
                                                                                                                         : "no MULE reason for 5 s");
        }
    }
    // gone from both games: the latch runs out (a truck that went keeps its hold for 5 s: its removal is not told to the host)
    for (auto it = g_holds.begin(); it != g_holds.end();) {
        if (it->second.seen_ms == now) {
            ++it;
            continue;
        }
        const bool was = it->second.latch.Held();
        it->second.latch.Step(trucks::HoldWhy::none, false, now);
        if (it->second.latch.Held()) {
            ++it;
            continue;
        }
        if (was) HoldLine(it->first, false, "gone from both games");
        it = g_holds.erase(it);
    }
    for (auto it = g_held_note.begin(); it != g_held_note.end();) it = now - it->second > 120000 ? g_held_note.erase(it) : std::next(it);
    SendTruckFacts(now);
}

bool TrucksTickGuarded(double now) noexcept {
    __try {
        TrucksTickBody(now);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void TrucksReset() {
    g_trucks.clear();
    g_models.clear();
    g_there.clear();
    g_holds.clear();
    g_facts_sent.clear();
    g_facts_sent_ms = -1e18;
    g_hold_lines = 0;
    g_held_note.clear();
}

void TrucksLinkReset() {
    g_there.clear();
    g_facts_sent.clear();
    g_facts_sent_ms = -1e18;
    g_facts_rx_logged = false;
}

void TrucksTick(double now) {
    if (!TrucksOn() || (!g_mule_log && !g_mule_hold) || !g_poll.ok) return;
    if (TrucksTickGuarded(now)) return;
    g_trucks_fault = true;
    TrucksReset();
    Log("[vshare] FAULT in the MULE truck rules: off for the run (the shared vehicles go on without them: trucks are shared as before)");
}

// The snapshot's truck lines (host: what it sends; guest: what it got): the MULE ones, at most 12, and a summary.
void LogTrucks(const char* who) {
    if (!g_mule_log || !TrucksOn()) return;
    unsigned n = 0, mule = 0, driven = 0, held = 0, lines = 0;
    for (const auto& kv : g_trucks) {
        const TruckSt& t = kv.second;
        ++n;
        const bool m = t.u.model == trucks::Model::celd || trucks::NonPlayer(t.f.driver);
        if (t.u.model == trucks::Model::celd) ++mule;
        if (trucks::NonPlayer(t.f.driver)) ++driven;
        const trucks::HoldWhy w = HoldOfKey(HoldKeyOf(t));
        if (w != trucks::HoldWhy::none) ++held;
        if (!m || lines >= 12) continue;
        ++lines;
        const auto th = g_there.find(t.host_id);
        char there[96] = "no facts";
        if (t.host_id && th != g_there.end())
            std::snprintf(there, sizeof(there), "%s, driver 0x%06X seat %d", trucks::ModelName(th->second.f.model), th->second.f.driver_id,
                          th->second.f.seat == trucks::kNoSeat ? -1 : th->second.f.seat);
        Log("[vshare] %s: truck 0x%06X (host 0x%06X): %s, driver 0x%06X seat %d camp %s%s at (%.1f, %.1f, %.1f); the other game: %s; %s", who, t.local_id,
            t.host_id, trucks::ClassName(t.f.cls), t.f.driver_id, t.seat, t.camp, t.f.agree ? "" : " (its fields disagree)", t.xf.pos.x, t.xf.pos.y,
            t.xf.pos.z, there, w == trucks::HoldWhy::none ? "shared" : trucks::HoldWhyName(w));
    }
    Log("[vshare] %s: trucks: %u here (%u MULE trucks, %u driven by a non-player), %u left to each game; the other game's facts %zu; facts tx %u rx %u "
        "(bad %u, stale %u); decisions left alone %u",
        who, n, mule, driven, held, g_there.size(), g_facts_tx, g_facts_rx, g_facts_bad, g_facts_stale, g_held_skips);
}

// The census (op 9 and enemy::Census): from `units` (a fresh poll).
bool TruckCensusOf(const std::vector<PUnit>& units, int tag, double radius_m, char* msg, std::size_t cap) {
    WorldTransform sam{};
    const bool have_sam = LocalSam(&sam);
    const double sp[3] = {sam.pos.x, sam.pos.y, sam.pos.z};
    unsigned n = 0, listed = 0, cls[6] = {}, npc = 0;
    double best = -1;
    std::uint32_t best_id = 0;
    WorldTransform best_xf{};
    for (const PUnit& p : units) {
        if (!TruckCandidate(p)) continue;
        const TruckSt t = ReadTruck(p);
        ++n;
        ++cls[static_cast<int>(t.f.cls) % 6];
        if (trucks::NonPlayer(t.f.driver)) ++npc;
        const double d = have_sam && t.has_xf ? DistXf(t.xf, sp) : -1;
        if (t.u.model == trucks::Model::celd && d >= 0 && (best < 0 || d < best)) best = d, best_id = t.local_id, best_xf = t.xf;
        if (radius_m > 0 && !(d >= 0 && d <= radius_m)) continue;
        if (listed >= 64) continue;
        const std::uint32_t host = g_on.load() ? LocalToHost(t.local_id) : 0;
        const trucks::HoldWhy w = g_on.load() ? HoldOfPair(host, t.local_id) : trucks::HoldWhy::none;
        Log("[vshare] trucks tag=%d t%02u %s 0x%06X%s: %s (model %s); driver 0x%06X (%s) seat %d camp %s; +0x158 0x%06X +0x19D %02X seat +0x63C %d +0x8B0 "
            "0x%06X%s%s%s; unit +0x198 %08X (variant %u) rec2 %04X; at (%.2f, %.2f, %.2f) %.1f m; %s",
            tag, listed, vwire::KindName(t.u.kind), t.local_id, host && host != t.local_id ? " (host's id differs)" : "", trucks::ClassName(t.f.cls),
            trucks::ModelName(t.u.model), t.f.driver_id, trucks::DriverName(t.f.driver), t.seat, t.camp, t.u.rider, t.u.b19d, t.u.ent ? static_cast<int>(t.u.ent_has) : -1,
            t.u.ent_driver, t.f.follower ? ", the NPC controller (a follower)" : "", t.f.last_npc ? ", a non-player drove it last" : "",
            t.f.agree ? "" : ", its fields DISAGREE", t.uflags, (t.uflags >> 22) & 0x3Fu, t.rec2, t.xf.pos.x, t.xf.pos.y, t.xf.pos.z, d, w == trucks::HoldWhy::none ? "shared" : trucks::HoldWhyName(w));
        ++listed;
    }
    char nearest[96] = "none with a place";
    if (best >= 0) std::snprintf(nearest, sizeof(nearest), "0x%06X %.0f m at (%.0f, %.0f, %.0f)", best_id, best, best_xf.pos.x, best_xf.pos.y, best_xf.pos.z);
    Log("[vshare] trucks tag=%d done: %u (player %u, player NPC-driven %u, MULE empty %u, MULE-driven %u, MULE player-driven %u, model unknown %u), %u driven by "
        "a non-player, %u listed; nearest MULE truck %s",
        tag, n, cls[0], cls[1], cls[2], cls[3], cls[4], cls[5], npc, listed, nearest);
    std::snprintf(msg, cap, "trucks tag %d: %u (MULE empty %u, MULE-driven %u, taken %u; player %u), %u non-player drivers; nearest MULE truck %s", tag, n, cls[2], cls[3],
                  cls[4], cls[0] + cls[1], npc, nearest);
    return true;
}

int TruckCensusBody(int tag, double radius_m, char* msg, std::size_t cap) {
    std::vector<PUnit> units;
    if (!Poll(units)) return std::snprintf(msg, cap, "truck census: the pools are not readable now"), 0;
    return TruckCensusOf(units, tag, radius_m, msg, cap) ? 1 : 0;
}
int TruckCensusGuarded(int tag, double radius_m, char* msg, std::size_t cap) noexcept {
    __try {
        return TruckCensusBody(tag, radius_m, msg, cap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Without the shared vehicles: the pool layout and the truck bytes, checked once.
int g_census_bytes = -1;
bool CensusBytesOk() {
    if (g_on.load()) return (g_groups_ok & (kCore | kTrucks)) == (kCore | kTrucks);
    if (g_census_bytes < 0) {
        bool ok = true;
        for (const Site& s : kSites)
            if ((s.group & (kCore | kTrucks)) && !mem::CodeMatches(A(s.va), s.sig)) {
                ok = false;
                Log("[vshare] truck census: byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
            }
        if (mem::RipTarget(A(0x142F7509C), 3, 7) != A(kRegGlobal)) ok = false;
        g_census_bytes = ok ? 1 : 0;
    }
    return g_census_bytes == 1;
}

void TickBody(const TickIn& in, double now) {
    if (g_first_tick == 0) g_first_tick = now;
    g_now_ms = now;
    g_connected = link::GetInfo().connected;
    g_have_partner = in.have_partner;
    if (in.have_partner) g_partner = in.partner;
    const std::uint32_t gen = link::Generation();
    if (gen != g_gen) {
        g_gen = gen;
        NewLinkSession();
        TrucksLinkReset();
    }
    std::uint32_t counter = 0;
    const bool ready = coop::WorldReady(&counter);
    if (ready != g_ready || (ready && counter != g_counter)) {
        Log("[vshare] %s: world %s (#%u)", g_role == 1 ? "host" : "guest", ready ? "ready" : "not ready", counter);
        g_ready = ready;
        g_counter = counter;
        g_ready_since = now;
        g_hbase = false;
        // the old world's units are never handed out again (SharedUnits / FindNear / Footprints read the poll)
        g_poll.ok = false;
        g_poll.units.clear();
        g_poll.t = -1e18;
        g_place_override.clear();
        TrucksReset();
        if (g_role == 2) {
            g_bind.clear();
            g_mirrors.clear();
            g_verify.clear();
            g_mirror_failed.clear();
            g_prev_bound.clear();
            g_mirrored.clear();
            g_rem_told.clear();
            g_host_answered = false;
            g_snap_wait_since = 0;
            g_asm.Reset();
            g_bound_once = false;
            g_have_table = false;
            g_table.clear();
            g_asked_ms = -1e18;
        }
    }
    // the guest's gate
    if (g_role == 2) {
        const bool settled = Settled(now);
        const bool silent = HostSilent();
        if (silent && !g_host_silent_logged) {
            g_host_silent_logged = true;
            Log("[vshare] guest: no snapshot from the host's game %.0f s after asking: it shares no real vehicles now; our game spawns its own "
                "vehicles and vehicle_sync shows the copies until it answers",
                (now - g_snap_wait_since) / 1000.0);
        }
        g_load_window.store(!settled);
        g_host_real_gate.store(g_host_answered && !silent);
        g_can_forward.store(settled && g_connected && g_bound_once);
    }
    // the polled pools
    if (now - g_poll.t >= kPollMs) {
        g_poll.t = now;
        g_poll.ok = g_ready && Poll(g_poll.units);
        g_poll.in_use = 0;
        for (const PUnit& p : g_poll.units)
            if (p.in_use) ++g_poll.in_use;
        if (!g_poll.ok) g_poll.units.clear();
        TrucksTick(now);  // round 23 D: before the binding and the host's diff (their removals ask the holds; the facts go first)
        if (g_role == 1 && g_poll.ok) HostTick(now);
        if (g_role == 2) GuestTick(now);
    } else if (g_role == 2 && g_reconcile_now) {
        GuestTick(now);
    }
    // incoming, in order
    std::uint8_t buf[vwire::kMaxMessage];
    for (int i = 0; i < 64; ++i) {
        std::size_t n = 0;
        std::uint32_t g = 0;
        if (!link::PopVehicleEvent(buf, sizeof(buf), n, g)) break;
        if (g != g_gen) continue;  // an older session's
        OnMessage(buf, n, g, now);
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

// ---------------------------------------------------------------- detours
// False when the request's transform is not readable or not a real one (it is then dropped, never sent as zeros).
bool ReadReq(std::uintptr_t pool, std::uintptr_t req, vwire::Req* q) noexcept {
    q->kind = static_cast<std::uint8_t>(KindOfPool(pool));
    std::uint8_t xf[0x3C] = {};
    bool read_ok = true;
    if (t_garage_has_xf) std::memcpy(xf, t_garage_xf, sizeof(xf));
    else read_ok = mem::Read(req, xf, sizeof(xf));
    std::memcpy(q->pos, xf, 24);
    std::memcpy(q->rot, xf + 24, 36);
    mem::Rd(req + kReqFlags, q->req_flags);
    mem::Rd(req + kReqRec2, q->rec2);
    mem::Rd(req + kReqRec14, q->rec14);
    q->garage_slot = static_cast<std::int8_t>(t_garage_slot);
    q->reason = t_dev_call ? vwire::ReqReason::dev : t_garage_slot >= 0 ? vwire::ReqReason::garage : vwire::ReqReason::game;
    const std::uintptr_t name = mem::Q(req + kReqName);
    char nm[vwire::kNameChars + 1] = {};
    if (name && name != A(kEmptyString) && mem::Read(name, nm, vwire::kNameChars)) {
        nm[vwire::kNameChars] = 0;
        std::size_t i = 0;
        for (; i < vwire::kNameChars && nm[i]; ++i)
            if (static_cast<unsigned char>(nm[i]) < 0x20 || static_cast<unsigned char>(nm[i]) > 0x7E) nm[i] = '?';
        for (; i <= vwire::kNameChars; ++i) nm[i] = 0;
        std::memcpy(q->name, nm, sizeof(q->name));
    }
    // the request's own transform must be finite, not the origin, and its rows a rotation (EncodeReq / DecodeReq check it too)
    bool ok = read_ok && !(q->pos[0] == 0 && q->pos[1] == 0 && q->pos[2] == 0);
    for (double v : q->pos)
        if (!std::isfinite(v) || std::fabs(v) > 1e7) ok = false;
    for (float v : q->rot)
        if (!std::isfinite(v) || std::fabs(v) > 1.5f) ok = false;
    if (ok && !vwire::RotRowsOk(q->rot)) ok = false;
    if (!ok) {
        std::memset(q->pos, 0, sizeof(q->pos));
        std::memset(q->rot, 0, sizeof(q->rot));
    }
    return ok;
}

void H_enqueue(std::uintptr_t pool, std::uintptr_t req) {
    // co-op stopped (its emergency stop, a failed start): the game's own behaviour
    if (!g_on.load(std::memory_order_acquire) || !coop::Active()) return o_enqueue(pool, req);
    vwire::GateIn in;
    in.host = g_role == 1;
    in.mod_call = t_mod_call;
    in.load_window = g_load_window.load();
    in.can_forward = g_can_forward.load();
    in.host_real = g_host_real_gate.load();
    std::uint32_t flags = 0;
    mem::Rd(req + kReqFlags, flags);
    in.flags = flags;
    switch (vwire::DecideGate(in)) {
        case vwire::Gate::pass:
            if (in.mod_call) ++g_n_mod;
            else if (!in.host && in.load_window) ++g_n_load;
            else ++g_n_pass;
            return o_enqueue(pool, req);
        case vwire::Gate::drop_online:
            ++g_n_online;
            return;
        case vwire::Gate::forward: {
            vwire::Req q;
            if (!ReadReq(pool, req, &q) || q.kind < 1 || q.kind > 3) {
                ++g_n_drop;
                return;
            }
            AcquireSRWLockExclusive(&g_fwd_lock);
            if (g_fwd_n < kFwdCap) g_fwd[g_fwd_n++] = q;
            ReleaseSRWLockExclusive(&g_fwd_lock);
            ++g_n_forward;
            return;
        }
        case vwire::Gate::drop:
            ++g_n_drop;
            return;
    }
}

std::uint64_t H_garage(std::uintptr_t pm, std::uintptr_t slot, std::uintptr_t xf) {
    if (!g_on.load(std::memory_order_acquire) || g_role != 2 || !coop::Active()) return o_garage(pm, slot, xf);
    const int prev_slot = t_garage_slot;
    const bool prev_has = t_garage_has_xf;
    double prev_xf[8];
    std::memcpy(prev_xf, t_garage_xf, sizeof(prev_xf));
    int idx = -1;
    if (pm && slot >= pm + kPmSlots && slot < pm + kPmSlots + kPmSlotCount * kPmSlotSize && (slot - pm - kPmSlots) % kPmSlotSize == 0)
        idx = static_cast<int>((slot - pm - kPmSlots) / kPmSlotSize);
    t_garage_slot = idx;
    t_garage_has_xf = xf && mem::Read(xf, t_garage_xf, 0x3C);
    const std::uint64_t r = o_garage(pm, slot, xf);
    t_garage_slot = prev_slot;
    t_garage_has_xf = prev_has;
    std::memcpy(t_garage_xf, prev_xf, sizeof(prev_xf));
    return r;
}

std::uintptr_t H_victim(std::uintptr_t pool) {
    if (!g_on.load(std::memory_order_acquire) || !coop::Active()) return o_victim(pool);
    ++g_n_victim;
    if (g_role == 2) {
        ++g_n_victim_refused;  // the guest never evicts (M2.5)
        return 0;
    }
    const std::uintptr_t u = o_victim(pool);
    if (!u) return 0;
    std::uint32_t id = 0;
    if (mem::Rd(u + kUId, id) && IsActiveId(id)) {
        ++g_n_victim_refused;
        g_last_refused_id.store(id);
        return 0;
    }
    return u;
}

bool HookOne(std::uintptr_t va, void* det, void** orig, const char* name, bool* flag) {
    void* t = reinterpret_cast<void*>(A(va));
    MH_STATUS s = MH_CreateHook(t, det, orig);
    if (s == MH_OK) s = MH_EnableHook(t);
    Log("[vshare] hook %s 0x%llX: %s", name, static_cast<unsigned long long>(va), s == MH_OK ? "ok" : MH_StatusToString(s));
    if (s == MH_OK) *flag = true;
    return s == MH_OK;
}

// ---------------------------------------------------------------- debug
void Dump(const char* tag) {
    std::vector<PUnit> units;
    if (!Poll(units)) {
        Log("[vshare] dump %s: the pools are not readable (no world?)", tag);
        return;
    }
    unsigned n[4] = {}, total = 0;
    const int area = CallArea();
    for (const PUnit& p : units) {
        if (!p.in_use) continue;
        ++total;
        ++n[p.pool_kind];
        std::uint32_t host_id = 0;
        if (g_role == 2)
            for (const auto& b : g_bind)
                if (b.second == p.id) host_id = b.first;
        char bind[48] = "";
        if (g_role == 2) {
            if (host_id) std::snprintf(bind, sizeof(bind), " bound host 0x%06X", host_id);
            else std::snprintf(bind, sizeof(bind), " unbound");
        }
        Log("[vshare] unit %s: %s #%02d id 0x%06X state %u flags 0x%08X b19c %02X%02X%02X%02X area %u rider 0x%X guid %02X%02X%02X%02X..%02X%02X life %.1f "
            "battery %.2f ent %p %s at (%.2f, %.2f, %.2f)%s",
            tag, vwire::KindName(static_cast<std::uint8_t>(p.pool_kind)), p.index, p.id, p.state, p.flags, p.fb[0], p.fb[1], p.fb[2], p.fb[3], p.area,
            p.rider, p.guid[0], p.guid[1], p.guid[2], p.guid[3], p.guid[14], p.guid[15], p.life, p.battery, reinterpret_cast<void*>(p.entity),
            p.has_xf ? (p.entity ? "entity" : "unit xf") : "no xf", p.has_xf ? p.xf.pos.x : 0, p.has_xf ? p.xf.pos.y : 0, p.has_xf ? p.xf.pos.z : 0, bind);
    }
    WorldTransform sam{};
    const bool have_sam = LocalSam(&sam);
    Log("[vshare] dump %s: %u units in use (roadster %u, truck %u, trike %u), area %d; %s; local Sam at (%.2f, %.2f, %.2f)", tag, total, n[1], n[2], n[3],
        area, g_role == 1 ? "host" : "guest", have_sam ? sam.pos.x : 0, have_sam ? sam.pos.y : 0, have_sam ? sam.pos.z : 0);
}

}  // namespace

// ---------------------------------------------------------------- public
void Install(const Config& config) noexcept {
    g_role = config.role == Role::host ? 1 : config.role == Role::guest ? 2 : 0;
    g_mule_log = config.vehicle_mule_log;
    g_mule_hold = config.vehicle_mule_hold;
    if (!config.host_save) {
        std::snprintf(g_off, sizeof(g_off), "host_save=0 (no shared save, so no shared vehicle identity)");
    } else if (g_role == 0) {
        std::snprintf(g_off, sizeof(g_off), "role %s (only host and guest)", RoleName(config.role));
    } else if (config.vehicle_sync_set && config.vehicle_sync != 5) {
        std::snprintf(g_off, sizeof(g_off), "vehicle_sync=%d in session.ini", config.vehicle_sync);
    } else if (!coop::Active()) {
        std::snprintf(g_off, sizeof(g_off), "the co-op save session is off");
    } else {
        g_wanted = true;
    }
    if (!g_wanted) {
        Log("[vshare] shared real vehicles off: %s; vehicle_sync uses the v0.14 copies", g_off);
        return;
    }
    // byte checks
    unsigned bad_groups = 0;
    const char* first_bad = nullptr;
    unsigned checked = 0;
    for (const Site& s : kSites) {
        ++checked;
        if (!mem::CodeMatches(A(s.va), s.sig)) {
            bad_groups |= s.group;
            Log("[vshare] byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
            if (!first_bad && (s.group & kRequired)) first_bad = s.name;
        }
    }
    // structure: the RIP targets and the vtables
    auto structural = [&](bool ok, const char* what, unsigned group) {
        ++checked;
        if (ok) return;
        bad_groups |= group;
        Log("[vshare] byte check FAILED: %s", what);
        if (!first_bad && (group & kRequired)) first_bad = what;
    };
    structural(mem::RipTarget(A(0x142F7509C), 3, 7) == A(kRegGlobal), "the vehicle manager global's address", kCore);
    structural(mem::RipTarget(A(0x142F830FC), 3, 7) == A(kEmptyString), "the empty string's address", kSpawnG);
    structural(mem::RipTarget(A(0x142D6ECEA), 3, 7) == A(kParkGlobal), "the parking manager global's address", kGarage);
    structural(mem::RipTarget(A(0x142F867BD), 1, 5) == A(kVictim) && mem::RipTarget(A(0x142F867FF), 1, 5) == A(kVictim) &&
                   mem::RipTarget(A(0x142F993C9), 1, 5) == A(kVictim),
               "the victim chooser's three callers", kEvict);
    for (int k = 1; k <= 3; ++k) {
        char what[96];
        std::snprintf(what, sizeof(what), "the %s pool vtable (+0x30 exact spawn, +0x150 kind)", vwire::KindName(static_cast<std::uint8_t>(k)));
        structural(mem::Q(A(kPoolVt[k]) + 0x30) == A(kExactSpawn) && mem::Q(A(kPoolVt[k]) + 0x150) == A(kKindGetter[k]) &&
                       mem::CodeMatches(A(kKindGetter[k]), k == 1 ? "B8 01 00 00 00 C3" : k == 2 ? "B8 02 00 00 00 C3" : "B8 03 00 00 00 C3"),
                   what, kCore);
    }
    for (std::uintptr_t vt : kUnitVt) structural(mem::Q(A(vt) + 0x18) == A(kUnitXf), "a unit vtable's transform getter (+0x18)", kCore);
    g_groups_ok = ~bad_groups & 255u;
    g_exact_ok = (g_groups_ok & kExact) != 0;
    g_garage_ok = (g_groups_ok & kGarage) != 0;
    if (bad_groups & kRequired) {
        std::snprintf(g_off, sizeof(g_off), "this game build differs (%s)", first_bad ? first_bad : "byte check");
        g_wanted = false;
        Log("[vshare] shared real vehicles OFF: %s; the v0.14 solid trike / truck copies stand in (no roadster)", g_off);
        return;
    }
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        std::snprintf(g_off, sizeof(g_off), "MinHook init failed (%s)", MH_StatusToString(init));
        g_wanted = false;
        Log("[vshare] shared real vehicles OFF: %s", g_off);
        return;
    }
    bool hooks = HookOne(kEnqueue, reinterpret_cast<void*>(&H_enqueue), reinterpret_cast<void**>(&o_enqueue), "pool enqueue", &g_hooked_enqueue) &&
                 HookOne(kVictim, reinterpret_cast<void*>(&H_victim), reinterpret_cast<void**>(&o_victim), "eviction victim chooser", &g_hooked_victim);
    if (hooks && g_garage_ok && g_role == 2)
        g_garage_ok = HookOne(kGarageSpawn, reinterpret_cast<void*>(&H_garage), reinterpret_cast<void**>(&o_garage), "garage slot spawn", &g_hooked_garage);
    if (!hooks) {
        // the detours that went in pass everything through (g_on stays false)
        std::snprintf(g_off, sizeof(g_off), "a detour could not be installed");
        g_wanted = false;
        Log("[vshare] shared real vehicles OFF: %s", g_off);
        return;
    }
    g_off[0] = 0;
    g_on.store(true, std::memory_order_release);
    // round 23 D: the MULE truck rules (optional group)
    g_trucks_ok = (g_groups_ok & kTrucks) != 0;
    if (g_trucks_ok) RegisterHandler(trucks::kFactsType, &OnTruckFacts);
    Log("[vshare] MULE trucks (round 23 D): vehicle_mule_log=%d vehicle_mule_hold=%d; %s", g_mule_log ? 1 : 0, g_mule_hold ? 1 : 0,
        !g_trucks_ok ? "OFF: a truck byte check failed (trucks are shared as before)"
        : g_mule_log || g_mule_hold ? "every truck classified each poll, the facts exchanged (STK1, type 50)" : "both off (trucks are shared as before)");
    Log("[vshare] shared real vehicles ON (%s): %u byte checks passed; exact-id mirror path %s; garage relay %s; the guest %s",
        g_role == 1 ? "host" : "guest", checked, g_exact_ok ? "available" : "OFF (byte check)", g_garage_ok ? "on" : "off",
        g_role == 2 ? "never originates a vehicle, never evicts" : "asks the host for every vehicle");
}

bool Active() noexcept { return g_on.load(std::memory_order_acquire); }
const char* OffReason() noexcept { return Active() ? "" : g_off; }

void Tick(const TickIn& in, double now) noexcept {
    if (g_role == 0) return;
    if (!g_on.load(std::memory_order_acquire)) {
        // the reason on screen once (the co-op banner), for a minute
        if (!g_notice_done && g_off[0] && coop::Active()) {
            g_notice_done = true;
            char text[200];
            std::snprintf(text, sizeof(text), "Shared vehicles off: %s. Vehicles show as copies.", g_off);
            coop::SetVehicleNotice(text, true, now + 60000);
        }
        return;
    }
    if (!coop::Active()) {
        // the co-op save session stopped (its own emergency stop): no shared identity any more, the copies stand in
        EmergencyStop();
        std::snprintf(g_off, sizeof(g_off), "the co-op save session stopped");
        Log("[vshare] the co-op save session stopped: the real shared vehicles stop (the detours pass everything through)");
        return;
    }
    if (!TickGuarded(in, now)) {
        Log("[vshare] FAULT in the vehicle sharing tick: stopped (the detours pass everything through)");
        EmergencyStop();
    }
}

void EmergencyStop() noexcept {
    if (!g_on.exchange(false)) return;
    g_stopped = true;
    std::snprintf(g_off, sizeof(g_off), "stopped after a fault");
    g_load_window.store(true);
    g_can_forward.store(false);
}

bool FindByKey(const vwire::VKey& key, Unit* out) noexcept {
    if (!vwire::KeyValid(key)) return false;
    return FindByHostId(key.id, out) && out->key.kind == key.kind;
}

bool FindByHostId(std::uint32_t host_id, Unit* out) noexcept {
    if (!g_on.load() || !g_poll.ok || !out) return false;
    std::uint32_t local = host_id;
    bool bound = g_role == 1;
    if (g_role == 2) {
        local = LocalForHost(host_id);
        if (!local) return false;
        bound = true;
    }
    const PUnit* p = PolledById(local);
    if (!p || !p->alive() || !StillSame(*p)) return false;
    Unit u;
    u.unit = p->unit;
    u.entity = p->entity;
    u.local_id = p->id;
    u.key = p->Key();
    u.key.id = host_id;
    if (g_role == 2) {
        auto t = g_table.find(host_id);
        if (t != g_table.end()) u.key = t->second.e.key;
    }
    u.bound = bound;
    u.has_xf = p->has_xf;
    u.xf = p->xf;
    u.rider = p->rider;
    *out = u;
    return true;
}

bool FindByEntity(std::uintptr_t e, Unit* out) noexcept {
    if (!g_on.load() || !e) return false;
    const std::uintptr_t unit = mem::Q(e + kEUnit);
    std::uint32_t id = 0;
    if (!unit || !mem::Rd(unit + kUId, id)) return false;
    if (g_role == 1) return FindByHostId(id, out);
    for (const auto& b : g_bind)
        if (b.second == id) return FindByHostId(b.first, out);
    return false;
}

bool Bound() noexcept { return g_on.load() && (g_role == 1 ? g_ready : g_bound_once); }

// ---------------------------------------------------------------- round 23 D: MULE trucks
std::size_t Trucks(TruckInfo* out, std::size_t cap) noexcept {
    if (!out || !TrucksOn()) return 0;
    std::size_t n = 0;
    for (const auto& kv : g_trucks) {
        if (n >= cap) return n;
        const TruckSt& t = kv.second;
        TruckInfo& i = out[n++];
        i = TruckInfo{};
        i.host_id = t.host_id, i.local_id = t.local_id, i.kind = t.u.kind, i.cls = t.f.cls, i.model = t.u.model, i.here = true;
        i.driver = t.f.driver_id, i.seat = t.seat;
        std::snprintf(i.camp, sizeof(i.camp), "%s", t.camp);
        const auto th = t.host_id ? g_there.find(t.host_id) : g_there.end();
        if (th != g_there.end())
            i.there = true, i.there_model = th->second.f.model, i.there_driver = th->second.f.driver_id,
            i.there_seat = th->second.f.seat == trucks::kNoSeat ? -1 : th->second.f.seat;
        i.hold = HoldOfKey(HoldKeyOf(t));
    }
    for (const auto& kv : g_there) {
        if (n >= cap) return n;
        bool seen = false;
        for (const auto& t : g_trucks)
            if (t.second.host_id == kv.first) seen = true;
        if (seen) continue;
        TruckInfo& i = out[n++];
        i = TruckInfo{};
        i.host_id = kv.first, i.kind = trucks::kKindTruck, i.there = true, i.there_model = kv.second.f.model, i.there_driver = kv.second.f.driver_id;
        i.there_seat = kv.second.f.seat == trucks::kNoSeat ? -1 : kv.second.f.seat;
        std::snprintf(i.camp, sizeof(i.camp), "-");
        i.hold = HoldOfKey(kv.first);
    }
    return n;
}

bool MuleLog() noexcept { return g_mule_log; }

bool TruckHeld(std::uint32_t host_id, const char** why) noexcept {
    const trucks::HoldWhy w = HoldOfHost(host_id);
    if (why) *why = trucks::HoldWhyName(w);
    return w != trucks::HoldWhy::none;
}

bool TruckDriverOf(std::uint32_t actor_id) noexcept {
    if (!HoldOn() || actor_id >= trucks::kNoActor) return false;
    for (const auto& kv : g_trucks)
        if (kv.second.f.driver_id == actor_id && trucks::NonPlayer(kv.second.f.driver)) return true;
    return false;
}

bool TruckCensus(int tag, double radius_m, char* msg, std::size_t cap) noexcept {
    if (!CensusBytesOk()) return std::snprintf(msg, cap, "truck census: the pool layout or truck bytes differ (see the log)"), false;
    if (g_trucks_fault) return std::snprintf(msg, cap, "truck census: the truck rules faulted this run"), false;
    const int r = TruckCensusGuarded(tag, radius_m, msg, cap);
    if (r < 0) {
        g_trucks_fault = true;
        TrucksReset();
        Log("[vshare] FAULT in the truck census: the MULE truck rules are off for the run");
        std::snprintf(msg, cap, "truck census: FAULT (the truck rules are off for the run)");
    }
    return r == 1;
}

bool RegisterHandler(std::uint8_t type, Handler handler) noexcept {
    if (!vwire::IsExternalType(type) || !handler || g_handlers[type]) return false;
    g_handlers[type] = handler;
    return true;
}

bool SendEvent(const std::uint8_t* msg, std::size_t size) noexcept {
    std::uint8_t type = 0;
    if (!g_on.load() || !vwire::PeekType(msg, size, &type) || !vwire::IsExternalType(type)) return false;
    return Queue(msg, size);
}

void SetGuestOwnsQuery(KeyQuery q) noexcept { g_guest_owns = q; }
void SetFollowedQuery(IdQuery q) noexcept { g_followed = q; }

void NotePartnerDrove(const vwire::VKey& key, double now) noexcept {
    if (g_role == 1 && vwire::KeyValid(key)) g_active[key.id].last_drove = now;
}

bool RequestTeleport(std::uintptr_t e, const WorldTransform& to) noexcept {
    if (!(g_groups_ok & kTeleportG) || !e || !KindOfEntity(e)) return false;
    return CallTeleport(e, &to);
}

namespace {
bool UnitOf(const PUnit& p, std::uint32_t host_id, Unit* out) noexcept {
    Unit u;
    u.unit = p.unit;
    u.entity = p.entity;
    u.local_id = p.id;
    u.key = p.Key();
    u.key.id = host_id;
    if (g_role == 2) {
        auto t = g_table.find(host_id);
        if (t != g_table.end()) u.key = t->second.e.key;
    }
    u.bound = true;
    u.has_xf = p.has_xf;
    u.xf = p.xf;
    u.rider = p.rider;
    *out = u;
    return true;
}
std::uint32_t HostIdOfLocal(std::uint32_t local_id) noexcept {
    if (g_role == 1) return local_id;
    for (const auto& b : g_bind)
        if (b.second == local_id) return b.first;
    return 0;
}
}  // namespace

std::size_t SharedUnits(Unit* out, std::size_t cap) noexcept {
    if (!g_on.load() || !g_poll.ok || !out) return 0;
    std::size_t n = 0;
    for (const PUnit& p : g_poll.units) {
        if (n >= cap) break;
        if (!p.alive()) continue;
        const std::uint32_t hid = HostIdOfLocal(p.id);
        if (!hid) continue;
        UnitOf(p, hid, &out[n++]);
    }
    return n;
}

bool FindNear(std::uint8_t kind, const double (&pos)[3], double radius_m, Unit* out) noexcept {
    if (!g_on.load() || !g_poll.ok || !out) return false;
    const PUnit* best = nullptr;
    std::uint32_t best_hid = 0;
    double bd = radius_m;
    for (const PUnit& p : g_poll.units) {
        if (!p.alive() || !p.has_xf || p.pool_kind != kind) continue;
        const std::uint32_t hid = HostIdOfLocal(p.id);
        if (!hid) continue;
        const double d = DistXf(p.xf, pos);
        if (d <= bd) bd = d, best = &p, best_hid = hid;
    }
    return best && UnitOf(*best, best_hid, out);
}

std::size_t Footprints(std::uintptr_t except_unit, vwire::Footprint* out, std::size_t cap, std::uint32_t* host_ids) noexcept {
    if (!g_on.load() || !g_poll.ok || !out) return 0;
    std::size_t n = 0;
    for (const PUnit& p : g_poll.units) {
        if (n >= cap) break;
        if (!p.alive() || !p.has_xf || p.unit == except_unit) continue;
        if (host_ids) {
            std::uint32_t hid = g_role == 1 ? p.id : 0;
            if (g_role == 2)
                for (const auto& b : g_bind)
                    if (b.second == p.id) {
                        hid = b.first;
                        break;
                    }
            host_ids[n] = hid;
        }
        vwire::Footprint& f = out[n++];
        f.c[0] = p.xf.pos.x, f.c[1] = p.xf.pos.y, f.c[2] = p.xf.pos.z;
        std::memcpy(f.rot, &p.xf.rot, sizeof(f.rot));
        vwire::KindHalfExtents(static_cast<std::uint8_t>(p.pool_kind), f.half);
    }
    return n;
}

double FootprintsMs() noexcept { return g_poll.t; }

void NoteHostPlace(std::uint32_t host_id, const WorldTransform& xf) noexcept {
    if (g_role != 2 || !g_on.load()) return;
    auto t = g_table.find(host_id);
    if (t == g_table.end() || !(t->second.e.eflags & vwire::kEntXf)) return;
    t->second.e.pos[0] = xf.pos.x, t->second.e.pos[1] = xf.pos.y, t->second.e.pos[2] = xf.pos.z;
    std::memcpy(t->second.e.rot, &xf.rot, sizeof(t->second.e.rot));
}

bool WorldReadyNow(std::uint32_t* counter) noexcept {
    if (counter) *counter = g_counter;
    return g_on.load() && g_ready;
}

int Role() noexcept { return g_on.load() ? g_role : 0; }

bool PeerInOurWorld() noexcept { return g_on.load() && g_ready && g_counter != 0 && g_peer_world == g_counter; }

void NotePeerWorld(std::uint32_t w) noexcept {
    if (g_role == 2 && w) g_peer_world = w;
}

bool RealInEffect() noexcept { return g_on.load() && !HostSilent(); }

bool PartnerPlace(double (&pos)[3]) noexcept {
    if (!g_on.load() || !g_have_partner) return false;
    pos[0] = g_partner.pos.x, pos[1] = g_partner.pos.y, pos[2] = g_partner.pos.z;
    return std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2]);
}

bool UnitLive(const Unit& u) noexcept {
    if (!u.unit || !u.entity || !vwire::KeyValid(u.key)) return false;
    std::uint32_t id = 0;
    std::uint8_t use = 0;
    return KindOfEntity(u.entity) == u.key.kind && mem::Q(u.entity + kEUnit) == u.unit && mem::Q(u.unit + kUEntRef) == u.entity + 0x20 &&
           mem::Rd(u.unit + kUId, id) && id == u.local_id && mem::Rd(u.unit + kUInUse, use) && use != 0;
}

bool PlaceClear(std::uintptr_t except_unit, std::uint8_t kind, const WorldTransform& xf) noexcept {
    vwire::Footprint fp;
    fp.c[0] = xf.pos.x, fp.c[1] = xf.pos.y, fp.c[2] = xf.pos.z;
    std::memcpy(fp.rot, &xf.rot, sizeof(fp.rot));
    vwire::KindHalfExtents(kind, fp.half);
    static vwire::Footprint others[256];
    const std::size_t n = Footprints(except_unit, others, 256);
    WorldTransform sam{};
    const bool have_sam = LocalSam(&sam);
    const double sp[3] = {sam.pos.x, sam.pos.y, sam.pos.z};
    return vwire::TeleportClear(fp, have_sam, sp, others, n);
}

void SetPlaceOverride(std::uint32_t id, const WorldTransform& xf) noexcept {
    if (g_role == 1 && g_on.load()) g_place_override[id] = xf;
}
void ClearPlaceOverride(std::uint32_t id) noexcept { g_place_override.erase(id); }

bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    const double now = link::NowMsPrecise();
    switch (op) {
        case 9:  // round 23 D: the truck census (works without the shared vehicles)
            return TruckCensus(static_cast<int>(v[1]), v[2], msg, cap);  // v[0] is the op
        case 0: {
            std::snprintf(msg, cap,
                          "%s %s%s; world %s #%u; poll %s (%u in use); %s; sent %llu (fail %llu) rx %llu (bad %llu); gate pass %u mod %u load %u online %u "
                          "forward %u drop %u; victim %u refused %u",
                          g_role == 1 ? "host" : g_role == 2 ? "guest" : "none", g_on.load() ? "ON" : "OFF: ", g_on.load() ? "" : g_off,
                          g_ready ? "ready" : "not ready", g_counter, g_poll.ok ? "ok" : "no", g_poll.in_use,
                          g_role == 2 ? (g_have_table ? "table ok" : "no table") : "host", static_cast<unsigned long long>(g_sent),
                          static_cast<unsigned long long>(g_send_fail), static_cast<unsigned long long>(g_rx), static_cast<unsigned long long>(g_rx_bad),
                          g_n_pass.load(), g_n_mod.load(), g_n_load.load(), g_n_online.load(), g_n_forward.load(), g_n_drop.load(), g_n_victim.load(),
                          g_n_victim_refused.load());
            Log("[vshare] status: %s", msg);
            if (g_role == 2)
                Log("[vshare] guest: table %zu, bound %zu, mirrors in flight %zu; mirror ok exact %llu fallback %llu re-place %llu, failed %llu/%llu/%llu; "
                    "removed extra %llu wrong %llu host %llu; re-placed %llu; removals told %llu; exact-id mirrors complete %llu incomplete %llu",
                    g_table.size(), g_bind.size(), g_mirrors.size(), static_cast<unsigned long long>(g_mirror_ok[1]),
                    static_cast<unsigned long long>(g_mirror_ok[2]), static_cast<unsigned long long>(g_mirror_ok[3]),
                    static_cast<unsigned long long>(g_mirror_fail[1]), static_cast<unsigned long long>(g_mirror_fail[2]),
                    static_cast<unsigned long long>(g_mirror_fail[3]), static_cast<unsigned long long>(g_removed_extra),
                    static_cast<unsigned long long>(g_removed_wrong), static_cast<unsigned long long>(g_removed_host),
                    static_cast<unsigned long long>(g_replaced), static_cast<unsigned long long>(g_guest_rem_sent),
                    static_cast<unsigned long long>(g_verified_ok), static_cast<unsigned long long>(g_verified_bad));
            else if (g_role == 1)
                Log("[vshare] host: snapshots %llu, ADD %llu, REM %llu, requests served %llu refused %llu, guest removals %llu applied %llu",
                    static_cast<unsigned long long>(g_snaps), static_cast<unsigned long long>(g_adds), static_cast<unsigned long long>(g_rems),
                    static_cast<unsigned long long>(g_reqs_served), static_cast<unsigned long long>(g_reqs_refused),
                    static_cast<unsigned long long>(g_guest_rems), static_cast<unsigned long long>(g_guest_rems_applied));
            return true;
        }
        case 1: return DumpCommand({v[1], v[2], 0}, msg, cap);
        case 2: return SummonCommand({v[1], v[2], 0}, msg, cap);
        case 3: {
            if (!g_on.load()) return std::snprintf(msg, cap, "off: %s", g_off), false;
            if (g_role == 2) {
                SendSnapReq(2, now);
                return std::snprintf(msg, cap, "asked the host for its vehicles"), true;
            }
            if (!g_poll.ok) return std::snprintf(msg, cap, "no pools (no world?)"), false;
            SendSnapshot(0, "test");
            return std::snprintf(msg, cap, "snapshot #%u sent", g_snap_id), true;
        }
        case 4: {
            if (g_role != 2) return std::snprintf(msg, cap, "guest only"), false;
            for (const vwire::BindStep& s : g_last_plan.steps) {
                if (s.host < 0 || static_cast<std::size_t>(s.host) >= g_last_plan_host.size()) continue;
                const vwire::SnapEntry& h = g_last_plan_host[static_cast<std::size_t>(s.host)];
                const vwire::LocalUnit* u = s.local >= 0 ? &g_last_plan_local[static_cast<std::size_t>(s.local)] : nullptr;
                Log("[vshare] plan: host %s 0x%06X%s at (%.2f, %.2f, %.2f) -> %s (%.2f m)", vwire::KindName(h.key.kind), h.key.id,
                    h.key.HasGuid() ? " (GUID)" : "", h.pos[0], h.pos[1], h.pos[2], vwire::BindKindName(s.kind), s.dist);
                if (u) Log("[vshare] plan:   our 0x%06X at (%.2f, %.2f, %.2f)%s%s", u->id, u->pos[0], u->pos[1], u->pos[2], u->ridden ? " ridden" : "",
                           u->removed ? " removed" : "");
            }
            for (const vwire::ExtraStep& x : g_last_plan.extras) {
                const vwire::LocalUnit& u = g_last_plan_local[static_cast<std::size_t>(x.local)];
                Log("[vshare] plan: extra our %s 0x%06X at (%.2f, %.2f, %.2f): %s", vwire::KindName(u.kind), u.id, u.pos[0], u.pos[1], u.pos[2],
                    x.kind == vwire::ExtraKind::remove ? "remove" : x.kind == vwire::ExtraKind::keep_ridden ? "kept (ridden)"
                    : x.kind == vwire::ExtraKind::keep_pending ? "kept (a mirror's)" : "removed already");
            }
            return std::snprintf(msg, cap, "binding: %s", g_last_plan_line[0] ? g_last_plan_line : "none yet"), true;
        }
        case 5: {
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            const int p = static_cast<int>(v[1]);
            if (p < 0 || p > 3) return std::snprintf(msg, cap, "path 0 auto, 1 exact-id, 2 fallback, 3 re-place"), false;
            g_path_pref = p;
            return std::snprintf(msg, cap, "mirror path: %s", p == 0 ? "auto" : PathName(p)), true;
        }
        case 6: {
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            const int kind = static_cast<int>(v[1]);
            const double maxd = v[2] > 0 ? v[2] : 60;
            WorldTransform sam{};
            if (!LocalSam(&sam)) return std::snprintf(msg, cap, "no local player"), false;
            std::vector<PUnit> units;
            if (!Poll(units)) return std::snprintf(msg, cap, "no pools"), false;
            const PUnit* best = nullptr;
            double bd = maxd;
            const double sp[3] = {sam.pos.x, sam.pos.y, sam.pos.z};
            for (const PUnit& p : units) {
                if (!p.alive() || !p.has_xf || p.ridden() || (kind && p.pool_kind != kind)) continue;
                const double d = DistXf(p.xf, sp);
                if (d < bd) bd = d, best = &p;
            }
            if (!best) return std::snprintf(msg, cap, "no %s within %.0f m", kind ? vwire::KindName(static_cast<std::uint8_t>(kind)) : "vehicle", maxd), false;
            if (!StillSame(*best) || !OrRemoveBit(best->unit)) return std::snprintf(msg, cap, "removal failed"), false;
            if (g_role == 2) g_removing[best->id] = now - 20000;  // reported to the host like any removal of ours
            return std::snprintf(msg, cap, "test: removal requested for %s 0x%06X %.1f m away", vwire::KindName(static_cast<std::uint8_t>(best->pool_kind)),
                                 best->id, bd),
                   true;
        }
        case 8: {
            // what the host's save would hold (M2.10 #6, persistence): the game's own persisted entry (the save writer
            // 0x142F7CF20) of every live vehicle of kind v[1] (0 any) within v[2] m (0 = 400) of our Sam: its id and place
            if (g_role != 1 || !g_exact_ok) return std::snprintf(msg, cap, "host only, with the entry writer's byte checks"), false;
            WorldTransform sam{};
            if (!LocalSam(&sam)) return std::snprintf(msg, cap, "no local player"), false;
            const int kind = static_cast<int>(v[1]);
            const double maxd = v[2] > 0 ? v[2] : 400;
            const double sp[3] = {sam.pos.x, sam.pos.y, sam.pos.z};
            unsigned n = 0;
            for (const PUnit& p : g_poll.units) {
                if (!p.alive() || !p.has_xf || (kind && p.pool_kind != kind) || DistXf(p.xf, sp) > maxd) continue;
                alignas(8) std::uint8_t entry[vwire::kEntrySize + 16] = {};
                const int r = CallWriteEntry(p.unit, entry);
                std::uint32_t eid = 0;
                float ep[3] = {};
                std::memcpy(&eid, entry + 0x2C, 4);
                std::memcpy(ep, entry + 0x10, sizeof(ep));
                const double epd[3] = {ep[0], ep[1], ep[2]};
                ++n;
                Log("[vshare] persisted entry of %s 0x%06X: writer %d, id 0x%06X, place (%.2f, %.2f, %.2f), %.2f m from where it stands now", vwire::KindName(
                        static_cast<std::uint8_t>(p.pool_kind)), p.id, r, eid, epd[0], epd[1], epd[2], DistXf(p.xf, epd));
            }
            return std::snprintf(msg, cap, "%u persisted entries logged", n), true;
        }
        case 7:
            return std::snprintf(msg, cap, "gate: pass %u, mod %u, load window %u, online dropped %u, forwarded %u, dropped %u; victim calls %u, refused %u (last 0x%06X)",
                                 g_n_pass.load(), g_n_mod.load(), g_n_load.load(), g_n_online.load(), g_n_forward.load(), g_n_drop.load(), g_n_victim.load(),
                                 g_n_victim_refused.load(), g_last_refused_id.load()),
                   true;
    }
    return std::snprintf(msg, cap, "vehicle_share: 0 status, 1 dump, 2 summon kind [m], 3 snapshot now, 4 binding report, 5 mirror path, 6 test removal, 7 counters, 8 [kind] [m] persisted entries (host), 9 [tag] [m] truck census"),
           false;
}

bool DumpCommand(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!g_wanted && !g_on.load()) {
        // works without the sync too (a diagnostic), when the core bytes match
        bool ok = true;
        for (const Site& s : kSites)
            if ((s.group & kCore) && !mem::CodeMatches(A(s.va), s.sig)) ok = false;
        if (!ok) return std::snprintf(msg, cap, "the pool layout bytes differ"), false;
    }
    char tag[16];
    std::snprintf(tag, sizeof(tag), "T%d", static_cast<int>(v[0]));
    Dump(tag);
    std::vector<PUnit> units;
    const bool ok = Poll(units);
    unsigned total = 0, bound = 0;
    for (const PUnit& p : units) {
        if (!p.in_use) continue;
        ++total;
        if (g_role == 1) ++bound;
        else
            for (const auto& b : g_bind)
                if (b.second == p.id) ++bound;
    }
    return std::snprintf(msg, cap, "dump %s: %u units in use, %u bound%s", tag, total, bound, ok ? "" : " (pools unreadable)"), ok;
}

bool SummonCommand(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    const int kind = static_cast<int>(v[0]);
    if (kind < 1 || kind > 3) return std::snprintf(msg, cap, "kind 1 roadster, 2 truck, 3 trike"), false;
    if (!(g_groups_ok & kSpawnG) || !(g_groups_ok & kCore)) return std::snprintf(msg, cap, "unavailable (%s)", g_off[0] ? g_off : "byte checks"), false;
    const double dist = v[1] > 0 ? v[1] : 8.0;
    WorldTransform sam{};
    if (!LocalSam(&sam)) return std::snprintf(msg, cap, "no local player"), false;
    WorldTransform xf = sam;
    xf.pos.x += sam.rot.m[1][0] * dist;
    xf.pos.y += sam.rot.m[1][1] * dist;
    xf.pos.z += 0.5;
    // a garage slot of that kind first (the game's own garage path)
    if (g_garage_ok) {
        const std::uintptr_t pm = mem::Q(A(kParkGlobal));
        for (int i = 0; pm && i < kPmSlotCount; ++i) {
            const std::uintptr_t slot = pm + kPmSlots + static_cast<std::uintptr_t>(i) * kPmSlotSize;
            std::uint8_t state = 0;
            std::uint16_t rec0 = 0;
            if (!mem::Rd(slot, state) || !mem::Rd(slot + 8, rec0) || state == 3 || (rec0 & 0xF) != kind) continue;
            const int r = CallGarageViaDetour(pm, slot, &xf);
            Log("[vshare] summon %s: garage slot %d (state %u, record 0x%04X) at (%.2f, %.2f, %.2f) -> %d", vwire::KindName(static_cast<std::uint8_t>(kind)), i,
                state, rec0, xf.pos.x, xf.pos.y, xf.pos.z, r);
            if (r == 1) return std::snprintf(msg, cap, "summon %s: garage slot %d asked", vwire::KindName(static_cast<std::uint8_t>(kind)), i), true;
        }
    }
    // a record like one of our vehicles of that kind
    std::vector<PUnit> units;
    if (!Poll(units)) return std::snprintf(msg, cap, "no pools (no world?)"), false;
    const PUnit* like = nullptr;
    for (const PUnit& p : units)
        if (p.alive() && p.pool_kind == kind && !p.HasGuid()) {
            like = &p;
            break;
        }
    for (const PUnit& p : units)
        if (!like && p.alive() && p.pool_kind == kind) like = &p;
    const std::uint16_t rec0 = static_cast<std::uint16_t>(kind | (like ? (((like->flags >> 22) & 0x3Fu) << 6) : 0u));
    Record rec;
    rec.Set(rec0, like ? like->rec2 : 0, 0);
    const bool ok = CallSpawn(reinterpret_cast<std::uintptr_t>(rec.b), &xf, 1, false);
    Log("[vshare] summon %s: spawn (record 0x%04X like 0x%06X) at (%.2f, %.2f, %.2f) %s", vwire::KindName(static_cast<std::uint8_t>(kind)), rec0,
        like ? like->id : 0, xf.pos.x, xf.pos.y, xf.pos.z, ok ? "asked" : "FAULTED");
    return std::snprintf(msg, cap, "summon %s: spawn asked (record 0x%04X)%s", vwire::KindName(static_cast<std::uint8_t>(kind)), rec0,
                         g_role == 2 ? "; the guest's gate forwards it to the host" : ""),
           ok;
}

// ---------------------------------------------------------------- dev tools (dev_tools.cpp; docs/debug-actions.md "Dev tools")
namespace {
// Repair and recharge: the race reset's own sequence (0x142D6ED80), the three vehicle kinds' Repair (vt+0x118), the battery
// object's notify. The variant write (entity +0x4C8) is only read back for the log.
constexpr std::uintptr_t kRepairFn[4] = {0, 0x142F7B090, 0x142F7B140, 0x142F7B320};  // speedcar, truck, motorbike vt+0x118
constexpr std::uintptr_t kBatteryNotify = 0x142F5A260;
constexpr std::uintptr_t kEBattery = 0x898, kELifeMax = 0x6EC, kEVariant = 0x4C8;
constexpr std::uintptr_t kBatCur = 0x08, kBatMax = 0x0C, kBatFlags = 0x48, kBatTimer = 0x34;
// clang-format off
constexpr Site kDevSites[] = {
    {0x142D6EE21, "48 8B 06 48 8B CE FF 90 18 01 00 00 48 8B 8E 98 08 00 00 8B 41 0C 89 41 08 E8 21 B4 1E 00 48 8B 86 98 08 00 00 44 89 70 34",
     "race reset: Repair vt+0x118, battery +0x898 (+8 = +0xC, notify, +0x34 = 0)", 1},
    {0x142F7B090, "40 53 48 83 EC 20 8B 81 EC 06 00 00 48 8B D9 89 81 E8 06 00 00 C7 81 F4", "roadster Repair", 1},
    {0x142F7B140, "48 89 5C 24 10 48 89 7C 24 18 55 48 8B EC 48 83 EC 30 8B 81 EC 06 00 00", "truck Repair", 1},
    {0x142F7B320, "8B 81 EC 06 00 00 89 81 E8 06 00 00 C7 81 F4 06 00 00 00 00 80 3F 80 89", "trike Repair", 1},
    {kBatteryNotify, "40 53 48 83 EC 30 F6 41 48 01 48 8B D9 74 1E 45 33 C9 C7 44 24 20 00 00", "battery state notify", 1},
    {0x142F77023, "8B 82 98 01 00 00 C1 E8 16 83 E0 3F C5 F8 2F 86 E8 06 00 00 89 86 C8 04", "spawn: variant (unit +0x198 >> 22) -> entity +0x4C8", 2},
};
// clang-format on
bool g_dev_checked = false;
unsigned g_dev_groups = 0;   // kCore / kSpawnG when the shared vehicles did not check them
bool g_dev_repair = false, g_dev_variant = false;
char g_dev_off[160] = {};
char g_dev_repair_off[96] = {};  // the first repair site that failed its check, or "faulted this run"

struct DevPending {
    bool active = false;
    int kind = 0, variant = 0;
    double t0 = 0;
    double pos[3] = {};
    bool forwarded = false;
    std::vector<std::uint32_t> before;
};
DevPending g_dev_pending;
std::uint32_t g_dev_last_id = 0;  // the newest vehicle DevSpawn made (its check found it)
int g_dev_last_kind = 0;

void DevCheck() {
    if (g_dev_checked) return;
    g_dev_checked = true;
    unsigned bad = 0;
    if (g_on.load()) {
        g_dev_groups = g_groups_ok;
    } else {
        for (const Site& s : kSites)
            if ((s.group & (kCore | kSpawnG)) && !mem::CodeMatches(A(s.va), s.sig)) {
                bad |= s.group;
                if (!g_dev_off[0]) std::snprintf(g_dev_off, sizeof(g_dev_off), "%s", s.name);
            }
        if (mem::RipTarget(A(0x142F7509C), 3, 7) != A(kRegGlobal)) bad |= kCore;
        if (mem::RipTarget(A(0x142F830FC), 3, 7) != A(kEmptyString)) bad |= kSpawnG;
        for (int k = 1; k <= 3; ++k)
            if (mem::Q(A(kPoolVt[k]) + 0x150) != A(kKindGetter[k])) bad |= kCore;
        for (std::uintptr_t vt : kUnitVt)
            if (mem::Q(A(vt) + 0x18) != A(kUnitXf)) bad |= kCore;
        g_dev_groups = (kCore | kSpawnG) & ~bad;
    }
    bool repair = true, variant = true;
    for (const Site& s : kDevSites)
        if (!mem::CodeMatches(A(s.va), s.sig)) {
            Log("[dev] byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
            if (s.group == 1) {
                if (repair) std::snprintf(g_dev_repair_off, sizeof(g_dev_repair_off), "%s", s.name);
                repair = false;
            } else {
                variant = false;
            }
        }
    for (int k = 1; k <= 3; ++k)
        if (mem::Q(A(kVehVt[k]) + 0x118) != A(kRepairFn[k])) {
            Log("[dev] byte check FAILED: the %s entity vtable +0x118 is not its Repair", vwire::KindName(static_cast<std::uint8_t>(k)));
            if (repair) std::snprintf(g_dev_repair_off, sizeof(g_dev_repair_off), "the %s vtable's Repair slot", vwire::KindName(static_cast<std::uint8_t>(k)));
            repair = false;
        }
    g_dev_repair = repair;
    g_dev_variant = variant;
    Log("[dev] vehicles: pools %s, spawn %s, repair %s, variant read-back %s%s%s", (g_dev_groups & kCore) ? "ok" : "OFF",
        (g_dev_groups & kSpawnG) ? "ok" : "OFF", repair ? "ok" : "OFF", variant ? "ok" : "off", g_dev_off[0] ? "; first mismatch: " : "", g_dev_off);
}

DevChecks Checks() {
    DevCheck();
    DevChecks c;
    c.pools = (g_dev_groups & kCore) != 0;
    c.spawn = c.pools && (g_dev_groups & kSpawnG) != 0;
    c.repair = c.pools && g_dev_repair;
    if (!c.pools || !c.spawn)
        std::snprintf(c.off, sizeof(c.off), "vehicle %s: %s", !c.pools ? "tools" : "spawning", g_dev_off[0] ? g_dev_off : "the pool / spawn byte checks");
    else if (!c.repair)
        std::snprintf(c.off, sizeof(c.off), "vehicle repair: %s", g_dev_repair_off[0] ? g_dev_repair_off : "a byte check");
    return c;
}

bool DevReady(unsigned need, char* msg, std::size_t cap) {
    DevCheck();
    if ((g_dev_groups & need) == need) return true;
    std::snprintf(msg, cap, "vehicle dev tools are off: this game build differs (%s)", g_dev_off[0] ? g_dev_off : "the pool / spawn byte checks");
    return false;
}

bool WriteU32(std::uintptr_t a, std::uint32_t v) {
    SIZE_T put = 0;
    return a >= 0x10000 && WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), &v, sizeof(v), &put) && put == sizeof(v);
}
bool CallRepair(std::uintptr_t fn, std::uintptr_t e) noexcept {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t)>(fn)(e);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallBatteryNotify(std::uintptr_t b) noexcept {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t)>(A(kBatteryNotify))(b);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const PUnit* Nearest(const std::vector<PUnit>& units, const WorldTransform& sam, double max_m, bool need_entity, bool skip_ridden, double* d_out) {
    const PUnit* best = nullptr;
    double bd = max_m;
    const double sp[3] = {sam.pos.x, sam.pos.y, sam.pos.z};
    for (const PUnit& p : units) {
        if (!p.alive() || !p.has_xf || (need_entity && !p.entity) || (skip_ridden && p.ridden())) continue;
        const double d = DistXf(p.xf, sp);
        if (d < bd) bd = d, best = &p;
    }
    if (d_out) *d_out = bd;
    return best;
}
}  // namespace

DevChecks DevCheckResult() noexcept { return Checks(); }

bool DevSpawn(int kind, int variant, double dist, char* msg, std::size_t cap) noexcept {
    if (!devrules::ValidVehicle(kind, variant))
        return std::snprintf(msg, cap, "no such vehicle: kind %d variant %d (roadster 0/1, truck 0/1/2/4/5/6, trike 0/1/2/3/7/8)", kind, variant), false;
    if (!DevReady(kCore | kSpawnG, msg, cap)) return false;
    if (g_dev_pending.active) Log("[dev] vehicle check: the %s variant %d asked before is not checked (a new request)", devrules::KindName(g_dev_pending.kind), g_dev_pending.variant);
    if (!(dist > 0)) dist = 8.0;
    dist = (std::min)(40.0, (std::max)(3.0, dist));
    WorldTransform sam{};
    if (!LocalSam(&sam)) return std::snprintf(msg, cap, "no local player (title screen or loading)"), false;
    std::vector<PUnit> units;
    if (!Poll(units)) return std::snprintf(msg, cap, "no vehicle pools (no world?)"), false;
    unsigned total = 0, used = 0;
    DevPending pend;
    for (const PUnit& p : units) {
        if (p.pool_kind != kind) continue;
        ++total;
        if (p.in_use) ++used;
        if (p.in_use) pend.before.push_back(p.id);
    }
    const char* kn = devrules::KindName(kind);
    if (!total) return std::snprintf(msg, cap, "the %s pool has no units", kn), false;
    if (used >= total)
        return std::snprintf(msg, cap, "the %s pool is full (%u of %u in use): a spawn would evict one of the save's vehicles; remove one first", kn, used,
                             total),
               false;
    const bool forward = g_on.load() && g_role == 2 && coop::Active() && g_host_real_gate.load();
    if (forward && !g_can_forward.load()) return std::snprintf(msg, cap, "the host's game cannot take a request now (not bound or not settled yet)"), false;
    WorldTransform xf = sam;
    const double here[3] = {sam.pos.x, sam.pos.y, sam.pos.z};
    double at[3] = {};
    devrules::Ahead(here, sam.rot.m[1][0], sam.rot.m[1][1], dist, at);
    xf.pos.x = at[0], xf.pos.y = at[1], xf.pos.z = at[2];
    const std::uint16_t rec0 = devrules::VehicleRec0(kind, variant);
    Record rec;
    rec.Set(rec0, 0, 0);
    const bool was = t_dev_call;
    t_dev_call = true;
    const bool ok = CallSpawn(reinterpret_cast<std::uintptr_t>(rec.b), &xf, 1, false);
    t_dev_call = was;
    Log("[dev] vehicle: %s variant %d (record 0x%04X, pool %u/%u in use) at (%.2f, %.2f, %.2f), %.0f m ahead: %s", kn, variant, rec0, used, total, xf.pos.x,
        xf.pos.y, xf.pos.z, dist, !ok ? "FAULTED" : forward ? "asked the host (dev request)" : g_on.load() && g_role == 1 ? "spawn asked (shared with the guest)" : "spawn asked");
    if (!ok) return std::snprintf(msg, cap, "the spawn call faulted"), false;
    pend.active = true;
    pend.kind = kind, pend.variant = variant;
    pend.t0 = link::NowMsPrecise();
    pend.pos[0] = xf.pos.x, pend.pos[1] = xf.pos.y, pend.pos[2] = xf.pos.z;
    pend.forwarded = forward;
    g_dev_pending = std::move(pend);
    if (forward) return std::snprintf(msg, cap, "%s variant %d: asked the host's game (it spawns it only on a sandboxed save)", kn, variant), true;
    return std::snprintf(msg, cap, "%s variant %d asked %.0f m ahead%s", kn, variant, dist, g_on.load() && g_role == 1 ? " (shared with the guest)" : ""), true;
}

void DevTick(double now_ms) noexcept {
    if (!g_dev_pending.active || now_ms - g_dev_pending.t0 < 2500) return;
    std::vector<PUnit> units;
    const PUnit* found = nullptr;
    if (Poll(units))
        for (const PUnit& p : units) {
            if (!p.alive() || p.pool_kind != g_dev_pending.kind) continue;
            if (std::find(g_dev_pending.before.begin(), g_dev_pending.before.end(), p.id) != g_dev_pending.before.end()) continue;
            if (!found || (p.has_xf && found->has_xf && DistXf(p.xf, g_dev_pending.pos) < DistXf(found->xf, g_dev_pending.pos))) found = &p;
        }
    const char* kn = devrules::KindName(g_dev_pending.kind);
    if (!found) {
        if (now_ms - g_dev_pending.t0 < 10000) return;  // a forwarded request: the host's ADD and our mirror take longer
        Log("[dev] vehicle check: NO new %s 10 s after the request (variant %d)%s", kn, g_dev_pending.variant,
            g_dev_pending.forwarded ? "; the host's game refused or did not answer (see [vshare] lines)" : "");
        g_dev_pending.active = false;
        return;
    }
    std::uint32_t ent_variant = 0xFFFFFFFFu;
    if (found->entity && g_dev_variant) mem::Rd(found->entity + kEVariant, ent_variant);
    Log("[dev] vehicle check: new %s 0x%06X, unit variant %u (asked %d), entity %p +0x4C8 %d, state %u, life %.0f, battery %.2f, at (%.2f, %.2f, %.2f), "
        "%.1f m from the asked place%s",
        kn, found->id, (found->flags >> 22) & 0x3Fu, g_dev_pending.variant, reinterpret_cast<void*>(found->entity), static_cast<int>(ent_variant), found->state,
        found->life, found->battery, found->has_xf ? found->xf.pos.x : 0, found->has_xf ? found->xf.pos.y : 0, found->has_xf ? found->xf.pos.z : 0,
        found->has_xf ? DistXf(found->xf, g_dev_pending.pos) : -1.0, found->entity ? "" : " (no actor yet: out of streaming range?)");
    g_dev_last_id = found->id;
    g_dev_last_kind = g_dev_pending.kind;
    g_dev_pending.active = false;
}

// Host with the shared vehicles: the partner drives this unit (through the mod, so the game's own rider field stays empty
// here) or rides on it: the shared-active set's riding mark (UpdateActive) or the follower vdrive runs for the partner.
bool PartnerOnUnit(const PUnit& p) noexcept {
    if (!g_on.load() || g_role != 1) return false;
    const auto it = g_active.find(p.id);
    if (it != g_active.end() && it->second.riding) return true;
    std::uintptr_t pe = 0;
    return p.entity && vdrive::PartnerDrivenEntity(&pe) && pe == p.entity;
}

bool DevRemove(double max_m, bool last, char* msg, std::size_t cap) noexcept {
    if (!DevReady(kCore, msg, cap)) return false;
    if (!(max_m > 0)) max_m = 15.0;
    WorldTransform sam{};
    if (!LocalSam(&sam)) return std::snprintf(msg, cap, "no local player"), false;
    std::vector<PUnit> units;
    if (!Poll(units)) return std::snprintf(msg, cap, "no vehicle pools (no world?)"), false;
    double d = 0;
    const PUnit* best = nullptr;
    const double sp[3] = {sam.pos.x, sam.pos.y, sam.pos.z};
    if (last) {
        for (const PUnit& p : units)
            if (p.alive() && p.id == g_dev_last_id && p.pool_kind == g_dev_last_kind && !p.ridden()) best = &p, d = p.has_xf ? DistXf(p.xf, sp) : -1;
        if (!best) return std::snprintf(msg, cap, "no dev vehicle to remove (none spawned, gone, or someone is on it)"), false;
        if (PartnerOnUnit(*best))
            return std::snprintf(msg, cap, "%s 0x%06X not removed: the partner is on it", vwire::KindName(static_cast<std::uint8_t>(best->pool_kind)), best->id),
                   false;
    } else {
        // the nearest that nobody is on: neither our Sam (the game's rider field) nor the partner (the shared-active set)
        bool partner_skipped = false;
        d = max_m;
        for (const PUnit& p : units) {
            if (!p.alive() || !p.has_xf || p.ridden()) continue;
            const double pd = DistXf(p.xf, sp);
            if (pd >= d) continue;
            if (PartnerOnUnit(p)) {
                partner_skipped = true;
                continue;
            }
            d = pd, best = &p;
        }
        if (!best)
            return std::snprintf(msg, cap, "no vehicle within %.0f m that nobody is on%s", max_m, partner_skipped ? " (the partner is on the nearest)" : ""),
                   false;
    }
    if (!StillSame(*best) || !OrRemoveBit(best->unit)) return std::snprintf(msg, cap, "removal failed"), false;
    Log("[dev] vehicle: removal requested for %s 0x%06X %.1f m away%s", vwire::KindName(static_cast<std::uint8_t>(best->pool_kind)), best->id, d,
        g_on.load() && g_role == 1 ? " (the guest's copy goes too)" : "");
    return std::snprintf(msg, cap, "%s 0x%06X removed (%.1f m away)", vwire::KindName(static_cast<std::uint8_t>(best->pool_kind)), best->id, d), true;
}

bool DevRepair(double max_m, char* msg, std::size_t cap) noexcept {
    if (!DevReady(kCore, msg, cap)) return false;
    if (!g_dev_repair)
        return std::snprintf(msg, cap, "repair is off: %s (see the [dev] lines)", g_dev_repair_off[0] ? g_dev_repair_off : "this game build differs"), false;
    if (!(max_m > 0)) max_m = 10.0;
    WorldTransform sam{};
    if (!LocalSam(&sam)) return std::snprintf(msg, cap, "no local player"), false;
    std::vector<PUnit> units;
    if (!Poll(units)) return std::snprintf(msg, cap, "no vehicle pools (no world?)"), false;
    double d = 0;
    const PUnit* best = Nearest(units, sam, max_m, true, false, &d);
    if (!best) return std::snprintf(msg, cap, "no vehicle within %.0f m", max_m), false;
    const int kind = best->pool_kind;
    const std::uintptr_t e = best->entity;
    const std::uintptr_t vt = mem::Q(e);
    if (kind < 1 || kind > 3 || vt != A(kVehVt[kind]) || mem::Q(vt + 0x118) != A(kRepairFn[kind]))
        return std::snprintf(msg, cap, "the vehicle's vtable is not the checked one"), false;
    const std::uintptr_t b = mem::Q(e + kEBattery);
    std::uint32_t bmax = 0, bflags = 0;
    float life0 = 0, lmax = 0, bat0 = 0;
    if (!b || !mem::Rd(b + kBatMax, bmax) || !mem::Rd(b + kBatFlags, bflags) || !mem::Rd(b + kBatCur, bat0) || !mem::Rd(e + kELife, life0) ||
        !mem::Rd(e + kELifeMax, lmax))
        return std::snprintf(msg, cap, "the vehicle's life / battery is unreadable"), false;
    if (!CallRepair(A(kRepairFn[kind]), e)) {
        g_dev_repair = false;
        std::snprintf(g_dev_repair_off, sizeof(g_dev_repair_off), "Repair faulted this run");
        return std::snprintf(msg, cap, "Repair faulted; repair off for this run"), false;
    }
    bool bat_ok = WriteU32(b + kBatCur, bmax);
    if (bat_ok && !CallBatteryNotify(b)) {
        g_dev_repair = false;
        std::snprintf(g_dev_repair_off, sizeof(g_dev_repair_off), "the battery notify faulted this run");
        return std::snprintf(msg, cap, "the battery notify faulted; repair off for this run"), false;
    }
    bat_ok = bat_ok && WriteU32(b + kBatTimer, 0);
    float life1 = 0, bat1 = 0, bmaxf = 0;
    mem::Rd(e + kELife, life1);
    mem::Rd(b + kBatCur, bat1);
    std::memcpy(&bmaxf, &bmax, 4);
    Log("[dev] vehicle repair: %s 0x%06X %.1f m away: life %.0f -> %.0f / %.0f, battery %.1f -> %.1f / %.1f%s", vwire::KindName(static_cast<std::uint8_t>(kind)),
        best->id, d, life0, life1, lmax, bat0, bat1, bmaxf, bat_ok ? "" : " (battery write FAILED)");
    return std::snprintf(msg, cap, "%s repaired: life %.0f -> %.0f, battery %.0f -> %.0f", vwire::KindName(static_cast<std::uint8_t>(kind)), life0, life1, bat0,
                         bat1),
           bat_ok;
}

// The shots' aimed test (shot_sync.cpp, hidden runs): the nearest vehicle with an entity, its place and life (+0x6E8).
bool NearestVehicleLife(double max_m, std::uintptr_t* entity, double (&pos)[3], float* life) noexcept {
    char why[96];
    if (!DevReady(kCore, why, sizeof(why))) return false;
    WorldTransform sam{};
    if (!LocalSam(&sam)) return false;
    std::vector<PUnit> units;
    if (!Poll(units)) return false;
    const PUnit* best = Nearest(units, sam, max_m > 0 ? max_m : 30.0, true, false, nullptr);
    if (!best) return false;
    float l = 0;
    if (!mem::Rd(best->entity + kELife, l) || !std::isfinite(l)) return false;
    *entity = best->entity;
    pos[0] = best->xf.pos.x, pos[1] = best->xf.pos.y, pos[2] = best->xf.pos.z;
    *life = l;
    return true;
}

}  // namespace st::vshare

namespace st::game {
// debug_actions.cpp's entry (game_actions.hpp): op 1 = dump [tag], op 2 = summon kind [m]; the rest vehicle_share's.
bool VehicleShareCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (op == 1) return vshare::DumpCommand({v[1], v[2], 0}, msg, cap);
    if (op == 2) return vshare::SummonCommand({v[1], v[2], 0}, msg, cap);
    return vshare::Command(op, v, msg, cap);
}
}  // namespace st::game
