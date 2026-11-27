// Round 28 P2 (trucks M0): the truck controller probe. See truck_v3.hpp. Read-only: every game read is partner::mem (never
// faults); the only hook is a pass-through counting detour on the get-off function, installed at start-up.
#include "truck_v3.hpp"

#include <windows.h>
#include <intrin.h>

#include "MinHook.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

#include "log.hpp"
#include "partner_source.hpp"
#include "truck_v3_rules.hpp"
#include "vehicle_wire.hpp"

#pragma intrinsic(_ReturnAddress)

namespace st::truckv3 {
namespace {
namespace mem = partner::mem;

// ---------------------------------------------------------------- game addresses (ds.exe, Director's Cut)
constexpr std::uintptr_t kVehReg = 0x147BFECC0;      // the vehicle manager (pools at +0x28 + kind * 8)
constexpr std::uintptr_t kActReg = 0x147BBB8C0;      // the game actor registry
constexpr std::uintptr_t kPoolVt[4] = {0, 0x143E278E0, 0x143E26B98, 0x143E261C0};
constexpr std::uintptr_t kUnitVt[3] = {0x143E26070, 0x143E26A48, 0x143E27790};
constexpr std::uintptr_t kVehVt[4] = {0, 0x143E274D8, 0x143E26790, 0x143E25DB8};  // roadster, truck (DSVehicleTruck), trike
constexpr std::uintptr_t kTruckSetDriver = 0x142F7C310;  // truck vt +0x110
constexpr std::uintptr_t kMuleVt = 0x143DB7078;      // DSMuleGameActor
constexpr std::uintptr_t kCtlCtor = 0x142F51C60;     // the NPC drive controller's constructor
constexpr std::uintptr_t kCtlAttach = 0x142F5A590;   // Attach (ctl, vehicle, u32* driver id)
constexpr std::uintptr_t kCtlInVt = 0x143E229B8;     // the controller's input object vtable (ctl +0x30)
constexpr const char* const kKindName[4] = {"?", "roadster", "truck", "trike"};

// unit / entity / actor / ride fields
constexpr std::uintptr_t kUId = 0x08, kUInUse = 0x20, kUEntRef = 0x68;
constexpr std::uintptr_t kEParent = 0x70, kEPos = 0xC8, kEHavok = 0x330, kERide = 0x4CC, kEInput = 0x528, kEBroken = 0x560, kECapsule = 0x5A0,
                         kEHas = 0x63C, kELife = 0x6E8, kEMaxLife = 0x6EC, kEUnit = 0x8A8, kEDriver = 0x8B0, kEExploded = 0xBD8;
constexpr std::uintptr_t kActId = 0x08, kActInUse = 0x20, kActState = 0x24, kActEntRef = 0x68;
constexpr std::uintptr_t kActRide = 0x3C0;  // ride: +0 seat, +0x10 ctl, +0x28 ctl state, +0xA60 vehicle id, +0xA68 handle, +0xA84 bits
constexpr std::uintptr_t kRCtl = 0x10, kRCtlState = 0x28, kRVehId = 0xA60, kRVehRef = 0xA68, kRBits = 0xA84;
constexpr std::uintptr_t kCState = 0x18, kCDriver = 0x20, kCInput = 0x30, kCDriveComp = 0x100;

// ---------------------------------------------------------------- byte checks
enum : unsigned { kGPools = 1, kGVeh = 2, kGActors = 4, kGRide = 8, kGCtl = 16, kGOff = 32, kGAll = 63 };
constexpr unsigned kRequired = kGPools | kGVeh | kGActors;
struct Check {
    std::uintptr_t va;
    const char* sig;
    unsigned group;
    const char* what;
};
constexpr Check kChecks[] = {
    // the vehicle pools (vehicle_share.cpp kCore)
    {0x142F7509C, "48 8B 35 1D 9C C8 04", kGPools, "the vehicle manager global"},
    {0x142F6C86F, "48 8B 7C F7 28", kGPools, "the pools at +0x28 + kind * 8"},
    {0x142DCFFB0, "48 89 5C 24 18 57 48 83 EC 20 48 8B 41 48 48 8B F9 48 63 49 40 41 8B D8 4C 8B C2 48 8D 14 C8", kGPools,
     "a pool's units +0x48 / count +0x40"},
    {0x142DCFFD7, "80 79 20 00 75 05 39 59 08", kGPools, "a unit's in-use +0x20 / id +0x08"},
    {0x142F69AA9, "48 8B 43 68 48 85 C0", kGPools, "a unit's entity ref +0x68"},
    {0x142F761A9, "48 8B 81 A8 08 00 00", kGPools, "the vehicle entity +0x8A8 = its unit"},
    // the vehicle entity (vehicle_share.cpp / vehicle_drive.cpp)
    {0x142F7C537, "41 BF 01 00 00 00 44 89 BF 3C 06 00 00 41 8B 06 89 87 B0 08 00 00 E8 BE 32 00 00", kGVeh,
     "SetDriver: +0x63C has a driver, +0x8B0 the driver id"},
    {0x142F6F4A0, "48 83 EC 28 83 B9 3C 06 00 00 00 74 29 48 8B 91 28 05 00 00 48 85 D2 74", kGVeh, "is driven: +0x63C, input +0x528"},
    {0x142F9BC20, "40 53 48 83 EC 30 83 B9 CC 04 00 00 05 48 8B D9 C5 F8 29 74 24 20 C5 C8", kGVeh, "the ride state +0x4CC"},
    {0x142F8AC5D, "48 8B 83 30 03 00 00 C5 FA 10 80 F0 00 00 00", kGVeh, "the speed [+0x330] +0xF0"},
    // the actor registry (enemy_census.cpp kCore)
    {0x1422FA980,
     "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 20 3B 15 B8 67 7F 02 8B DA 0F 84 86 00 00 00 8B C2 C1 E8 0D 44 0F B6 F0 44 3B B1 B0 04 00 00 "
     "73 74 48 8B B9 48 04 00 00 48 63 81 40 04 00 00 48 8D 2C C7 48 3B FD 74 5D 0F 1F 80 00 00 00 00 48 8B 37 48 8B CE 48 8B 06 FF",
     kGActors, "the actor lookup: groups +0x440 / +0x448"},
    {0x1422FA9DA,
     "50 38 41 3B C6 75 1B 44 8B 46 30 8B D3 81 E2 FF 1F 00 00 41 3B D0 72 0A 8B 4E 40 41 03 C8 3B D1 72 0B 48 83 C7 08 48 3B FD 75 CB EB 1F 48 8B "
     "46 48 81 E3 FF 1F 00 00 41 2B D8 48 63 CB 48 8B 04 C8 48 85 C0 74 06 80 78 20 00 75 02 33 C0 48 8B 5C 24 48 48 8B 6C 24 50",
     kGActors, "a group: +0x40 count, +0x48 actors, actor +0x20 in use"},
    {0x1422D6268, "48 8B 0D 51 56 8E 05 E8 0C 47 02 00 48 85 C0 74 1D 48 8B 48 68", kGActors, "the actor registry global, actor +0x68"},
    // the MULE's ride (enemy_sync.cpp RideChecks + this probe)
    {0x142B51F62, "49 8B 45 58 48 85 C0 48 8D 98 C0 03 00 00 49 0F 44 DF 74 18 83 B8 A0 0E 00 00 01", kGRide, "the MULE's ride at +0x3C0"},
    {0x142B52067, "49 8B 46 68 4C 8D B3 68 0A 00 00 48 85 C0 44 89 A3 60 0A 00 00", kGRide, "ride +0xA60 vehicle id / +0xA68 handle"},
    {0x142A9824C, "8B 05 FE 8E 05 02 48 8D 8B 28 0E 00 00 89 83 20 0E 00 00", kGRide, "the MULE ctor: +0xE20 no vehicle"},
    {0x142A981C4, "48 8D 8B D0 03 00 00", kGRide, "the MULE ctor: ctl at +0x3D0"},
    {0x142A98236, "48 89 BB B0 03 00 00 C7 83 C0 03 00 00 FF FF FF FF E8 14 9A 4B 00", kGRide, "the MULE ctor: seat -1, the ctl ctor"},
    {0x142A9826F, "80 A3 44 0E 00 00 F8", kGRide, "the MULE ctor: +0xE44 bits cleared"},
    {0x142B19479,
     "8B 01 48 8B D9 44 8B 42 08 85 C0 75 30 48 8B 81 68 0A 00 00 33 C9 48 85 C0 44 89 44 24 20 4C 8D 44 24 20 48 8D 50 E0 48 0F 44 D1 48 8D 4B 10 "
     "E8 E3 10 44 00",
     kGRide, "the driver seat: Attach (ride +0x10, [ride +0xA68] - 0x20)"},
    {0x142B194FD, "80 8B 84 0A 00 00 01", kGRide, "ride +0xA84 bit 0 riding"},
    // the NPC drive controller (vehicle_drive.cpp)
    {kCtlCtor, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83", kGCtl, "the controller ctor"},
    {0x142F51CE7, "89 71 70 44 89 71 78 48 89 41 30", kGCtl, "the controller ctor: the input vtable at +0x30"},
    {kCtlAttach, "40 53 55 56 57 41 54 41 57 48 81 EC 38 0C 00 00 48 8B 05 89 6A 00 02 48", kGCtl, "Attach"},
    {0x142F5A624, "48 39 3E 74 13 48 8B CE E8 DF 21 9C FE 48 8B CE 48 89 3E E8 54 21 9C FE", kGCtl, "Attach: the vehicle handle at ctl +0x00"},
    {0x142F5A65A, "48 8D 56 30 48 85 C0 48 8D 48 E0 49 0F 44 CC E8 A2 5C 02 00", kGCtl, "Attach: SetController (vehicle, ctl +0x30)"},
    {0x142F5A68B, "48 8D BD 18 03 00 00 48 89 86 00 01 00 00", kGCtl, "Attach: the AI drive component at ctl +0x100"},
    {0x142F5A785, "C7 46 18 02 00 00 00 41 8B 07 89 46 20", kGCtl, "Attach: state +0x18 = 2, driver +0x20"},
    // the get-off function (its prologue is hooked)
    {kGetOffVa, "40 53 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 40 F6 81 84 0A 00 00 01 48 8B D9 0F 84", kGOff,
     "get-off: +0xA84 bit 0"},
    {0x142AFB035,
     "83 39 00 75 35 83 79 28 03 48 89 7C 24 70 75 0B 33 D2 48 83 C1 10 E8 C0 F2 45 00 83 7B 28 01 76 09 48 8D 4B 10 E8 71 F3 45 00", kGOff,
     "get-off: seat 0 -> SetEngine 0 (state 3), Detach (state > 1)"},
};

std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }
std::uint32_t U32(std::uintptr_t a) noexcept {
    std::uint32_t v = 0;
    return mem::Rd(a, v) ? v : 0;
}
std::int32_t I32(std::uintptr_t a, std::int32_t dflt = -1) noexcept {
    std::int32_t v = 0;
    return mem::Rd(a, v) ? v : dflt;
}
float F32(std::uintptr_t a) noexcept {
    float v = 0;
    return mem::Rd(a, v) && std::isfinite(v) ? v : 0.f;
}

// ---------------------------------------------------------------- state
bool g_key = false;          // vehicle_truck_probe
bool g_checked = false;
unsigned g_ok = 0;           // the groups whose checks passed
unsigned g_pass = 0, g_total = 0;
std::uintptr_t g_image_size = 0;
const char* g_detour = "off";
std::atomic<bool> g_rec{false};  // the detour records (false after the emergency stop)

std::atomic<bool> g_watch{false};  // the emergency stop clears it from any thread
double g_period_ms = 1000, g_next_ms = 0, g_next_drive_ms = 0;
Tally g_tally;

// ---------------------------------------------------------------- the get-off detour (any thread)
using GetOffFn = void(__fastcall*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
GetOffFn g_orig = nullptr;
constexpr unsigned kRing = 32;
struct OffEv {
    std::atomic<std::uint64_t> seq{0};  // index + 1 once complete, 0 while written
    std::atomic<std::uintptr_t> ra{0}, ride{0}, actor_vt{0};
    std::atomic<std::uint32_t> id{0}, veh{0}, bits_before{0}, bits_after{0}, tid{0};
    std::atomic<std::int32_t> seat{0}, state_before{0}, state_after{0};
};
OffEv g_ring[kRing];
std::atomic<std::uint64_t> g_head{0};
std::uint64_t g_tail = 0;  // game thread
std::atomic<unsigned> g_site_n[std::size(kGetOffCallers) + 1];  // the last: unknown callers
std::atomic<unsigned> g_off_total{0};
std::atomic<DWORD> g_game_tid{0};
unsigned g_off_logged = 0, g_off_lost = 0;

void __fastcall H_GetOff(std::uintptr_t ride, std::uintptr_t rdx, std::uintptr_t r8, std::uintptr_t r9) {
    if (!g_rec.load(std::memory_order_relaxed)) return g_orig(ride, rdx, r8, r9);
    const std::uintptr_t ra = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const std::uint64_t idx = g_head.fetch_add(1, std::memory_order_acq_rel);
    OffEv& e = g_ring[idx % kRing];
    e.seq.store(0, std::memory_order_release);
    std::uint32_t bits = 0, id = 0, veh = 0;
    std::int32_t seat = -1, state = -1;
    std::uintptr_t vt = 0;
    if (ride) {
        mem::Rd(ride + kRBits, bits), mem::Rd(ride, seat), mem::Rd(ride + kRCtlState, state), mem::Rd(ride + kRVehId, veh);
        mem::Rd(ride - kActRide, vt), mem::Rd(ride - kActRide + kActId, id);
    }
    e.ra.store(ra, std::memory_order_relaxed), e.ride.store(ride, std::memory_order_relaxed), e.actor_vt.store(vt, std::memory_order_relaxed);
    e.id.store(id, std::memory_order_relaxed), e.veh.store(veh, std::memory_order_relaxed), e.bits_before.store(bits, std::memory_order_relaxed);
    e.seat.store(seat, std::memory_order_relaxed), e.state_before.store(state, std::memory_order_relaxed);
    e.tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_orig(ride, rdx, r8, r9);
    std::uint32_t after = 0;
    std::int32_t state_after = -1;
    if (ride) mem::Rd(ride + kRBits, after), mem::Rd(ride + kRCtlState, state_after);
    e.bits_after.store(after, std::memory_order_relaxed), e.state_after.store(state_after, std::memory_order_relaxed);
    const char* name = nullptr;
    const std::uintptr_t site = GetOffSite(ToVa(ra, mem::Base(), g_image_size), &name);
    unsigned slot = static_cast<unsigned>(std::size(kGetOffCallers));
    for (unsigned i = 0; i < std::size(kGetOffCallers); ++i)
        if (kGetOffCallers[i].site == site) slot = i;
    g_site_n[slot].fetch_add(1, std::memory_order_relaxed);
    g_off_total.fetch_add(1, std::memory_order_relaxed);
    e.seq.store(idx + 1, std::memory_order_release);
}

void DrainOff() noexcept {
    const std::uint64_t head = g_head.load(std::memory_order_acquire);
    if (head - g_tail > kRing) g_off_lost += static_cast<unsigned>(head - g_tail - kRing), g_tail = head - kRing;
    const DWORD game = g_game_tid.load(std::memory_order_relaxed);
    while (g_tail < head) {
        OffEv& e = g_ring[g_tail % kRing];
        const std::uint64_t s = e.seq.load(std::memory_order_acquire);
        if (s == 0) break;  // still being written: the next tick
        if (s != g_tail + 1) {
            ++g_off_lost, ++g_tail;  // overwritten
            continue;
        }
        const std::uintptr_t ra = e.ra.load(std::memory_order_relaxed), ride = e.ride.load(std::memory_order_relaxed),
                             vt = e.actor_vt.load(std::memory_order_relaxed);
        const std::uint32_t id = e.id.load(std::memory_order_relaxed), veh = e.veh.load(std::memory_order_relaxed),
                            bb = e.bits_before.load(std::memory_order_relaxed), ba = e.bits_after.load(std::memory_order_relaxed),
                            tid = e.tid.load(std::memory_order_relaxed);
        const std::int32_t seat = e.seat.load(std::memory_order_relaxed), sb = e.state_before.load(std::memory_order_relaxed),
                           sa = e.state_after.load(std::memory_order_relaxed);
        const bool same = e.seq.load(std::memory_order_acquire) == g_tail + 1;
        ++g_tail;
        if (!same) {
            ++g_off_lost;
            continue;
        }
        if (g_off_logged >= 40) continue;  // counted only (the status line)
        ++g_off_logged;
        const char* name = nullptr;
        const std::uintptr_t va = ToVa(ra, mem::Base(), g_image_size);
        const std::uintptr_t site = GetOffSite(va, &name);
        Log("[truckv3] get-off #%llu caller %s 0x%llX (%s) %s 0x%05X seat %d riding %u -> %u ctl state %d -> %d vehicle 0x%05X ride %p thread %s",
            static_cast<unsigned long long>(g_tail), site ? "site" : "return", static_cast<unsigned long long>(site ? site : va), name,
            vt == A(kMuleVt) ? "MULE" : "actor (not a MULE)", id, seat, bb & 1u, ba & 1u, sb, sa, veh, reinterpret_cast<void*>(ride),
            tid == game ? "game" : "other");
    }
}

// ---------------------------------------------------------------- reads
struct Veh {
    int kind = 0;
    std::uint32_t id = 0;
    std::uintptr_t unit = 0, entity = 0, input = 0;
    std::int32_t has = 0, ride = -1;
    std::uint32_t driver = kNoActor, broken = 0, exploded = 0;
    float speed = 0, life = 0, max_life = 0;
};
struct Mule {
    std::uint32_t id = 0, state = 0, veh_id = kNoActor, bits = 0;
    std::uintptr_t actor = 0, entity = 0, veh = 0;
    std::int32_t seat = -1, ctl_state = -1;
    std::uintptr_t chain = 0;      // the vehicle entity on the entity's parent chain (+0x70), 0 none (MarkChains)
    std::uint32_t chain_id = kNoActor;  // that vehicle's id when it is in the pools
    bool riding() const noexcept { return (bits & 1u) != 0; }
    bool seated() const noexcept { return SeatedOf(riding(), chain != 0); }
    std::uint32_t truck() const noexcept { return SeatedTruck(riding(), veh_id, chain != 0, chain_id); }
};

int KindOfEntity(std::uintptr_t e) noexcept {
    const std::uintptr_t vt = e ? mem::Q(e) : 0;
    for (int k = 1; k <= 3; ++k)
        if (vt && vt == A(kVehVt[k])) return k;
    return 0;
}
bool UnitVtOk(std::uintptr_t u) noexcept {
    const std::uintptr_t vt = mem::Q(u);
    for (std::uintptr_t v : kUnitVt)
        if (vt == A(v)) return true;
    return false;
}

void WalkVehicles(std::vector<Veh>& out) noexcept {
    out.clear();
    const std::uintptr_t r = mem::Q(A(kVehReg));
    if (!r) return;
    for (int kind = 1; kind <= 3; ++kind) {
        const std::uintptr_t pool = mem::Q(r + 0x28 + static_cast<std::uintptr_t>(kind) * 8);
        if (!pool || mem::Q(pool) != A(kPoolVt[kind])) continue;
        std::int32_t n = 0;
        if (!mem::Rd(pool + 0x40, n) || n <= 0 || n > 256) continue;
        const std::uintptr_t arr = mem::Q(pool + 0x48);
        for (std::int32_t i = 0; arr && i < n; ++i) {
            const std::uintptr_t u = mem::Q(arr + 8ull * static_cast<unsigned>(i));
            std::uint8_t use = 0;
            if (!u || !UnitVtOk(u) || !mem::Rd(u + kUInUse, use) || !use) continue;
            const std::uintptr_t ref = mem::Q(u + kUEntRef);
            const std::uintptr_t e = ref ? ref - 0x20 : 0;
            if (!e || KindOfEntity(e) != kind || mem::Q(e + kEUnit) != u) continue;  // not realized (or not the layout we think)
            Veh v;
            v.kind = kind, v.unit = u, v.entity = e, v.id = U32(u + kUId);
            v.input = mem::Q(e + kEInput);
            v.has = I32(e + kEHas, 0), v.ride = I32(e + kERide);
            v.driver = U32(e + kEDriver), v.broken = U32(e + kEBroken), v.exploded = U32(e + kEExploded);
            const std::uintptr_t hk = mem::Q(e + kEHavok);
            v.speed = hk ? F32(hk + 0xF0) : 0.f;
            v.life = F32(e + kELife), v.max_life = F32(e + kEMaxLife);
            out.push_back(v);
        }
    }
}

bool ReadRide(std::uintptr_t actor, Mule& m) noexcept {
    const std::uintptr_t ride = actor + kActRide;
    std::uintptr_t ref = 0;
    if (!mem::Rd(ride, m.seat) || !mem::Rd(ride + kRVehId, m.veh_id) || !mem::Rd(ride + kRBits, m.bits) || !mem::Rd(ride + kRCtlState, m.ctl_state) ||
        !mem::Rd(ride + kRVehRef, ref))
        return false;
    m.veh = ref ? ref - 0x20 : 0;
    return true;
}

void WalkMules(std::vector<Mule>& out) noexcept {
    out.clear();
    const std::uintptr_t reg = mem::Q(A(kActReg));
    std::int32_t ngroups = 0;
    if (!reg || !mem::Rd(reg + 0x440, ngroups) || ngroups <= 0 || ngroups > 512) return;
    const std::uintptr_t garr = mem::Q(reg + 0x448);
    for (std::int32_t gi = 0; garr && gi < ngroups; ++gi) {
        const std::uintptr_t g = mem::Q(garr + 8ull * static_cast<unsigned>(gi));
        std::int32_t count = 0;
        if (!g || !mem::Rd(g + 0x40, count) || count <= 0 || count > 8192) continue;
        const std::uintptr_t arr = mem::Q(g + 0x48);
        for (std::int32_t s = 0; arr && s < count; ++s) {
            const std::uintptr_t o = mem::Q(arr + 8ull * static_cast<unsigned>(s));
            std::uint8_t in_use = 0;
            if (!o || mem::Q(o) != A(kMuleVt) || !mem::Rd(o + kActInUse, in_use) || !in_use) continue;
            Mule m;
            m.actor = o, m.id = U32(o + kActId), m.state = U32(o + kActState);
            if (trucks::ActorType(m.id) != 1) continue;
            if (m.state == 2) {
                const std::uintptr_t ref = mem::Q(o + kActEntRef);
                m.entity = ref ? ref - 0x20 : 0;
            }
            if (g_ok & kGRide) ReadRide(o, m);
            out.push_back(m);
        }
    }
}

// Any in-use actor with this id (the driver may be a non-MULE): its object, or 0.
std::uintptr_t AnyActorOf(std::uint32_t id) noexcept {
    const std::uintptr_t reg = mem::Q(A(kActReg));
    std::int32_t ngroups = 0;
    if (!reg || !mem::Rd(reg + 0x440, ngroups) || ngroups <= 0 || ngroups > 512) return 0;
    const std::uintptr_t garr = mem::Q(reg + 0x448);
    for (std::int32_t gi = 0; garr && gi < ngroups; ++gi) {
        const std::uintptr_t g = mem::Q(garr + 8ull * static_cast<unsigned>(gi));
        std::int32_t count = 0;
        if (!g || !mem::Rd(g + 0x40, count) || count <= 0 || count > 8192) continue;
        const std::uintptr_t arr = mem::Q(g + 0x48);
        for (std::int32_t s = 0; arr && s < count; ++s) {
            const std::uintptr_t o = mem::Q(arr + 8ull * static_cast<unsigned>(s));
            std::uint8_t in_use = 0;
            if (o && mem::Rd(o + kActInUse, in_use) && in_use && U32(o + kActId) == id) return o;
        }
    }
    return 0;
}

const Veh* VehById(const std::vector<Veh>& vs, std::uint32_t id) noexcept {
    for (const Veh& v : vs)
        if (v.id == id) return &v;
    return nullptr;
}
const Veh* VehByEntity(const std::vector<Veh>& vs, std::uintptr_t e) noexcept {
    for (const Veh& v : vs)
        if (e && v.entity == e) return &v;
    return nullptr;
}
const char* RttiNameOf(std::uintptr_t obj) noexcept {
    const char* n = obj ? mem::RttiName(mem::RttiOf(obj)) : nullptr;
    return n ? n : "?";
}

bool ReadXf(std::uintptr_t e, Xf& x) noexcept {
    std::uint8_t b[0x3C];
    if (!e || !mem::Read(e + kEPos, b, sizeof(b))) return false;
    std::memcpy(x.pos, b, 0x18);
    std::memcpy(x.rot, b + 0x18, 0x24);
    return XfSane(x);
}

// The parent chain test (enemy_sync InVehicle, the test that saw MULE 0x0202E seated in round 23's runs): a realized MULE's entity
// +0x70, up to 4 levels, reaches a walked vehicle entity, or a parent whose RTTI names a DSVehicle (then chain_id stays kNoActor).
void MarkChains(std::vector<Mule>& ms, const std::vector<Veh>& vs) noexcept {
    for (Mule& m : ms) {
        m.chain = 0, m.chain_id = kNoActor;
        std::uintptr_t p = m.entity;
        for (int d = 0; p && d < 4 && !m.chain; ++d) {
            p = mem::Q(p + kEParent);
            if (!p) break;
            if (const Veh* v = VehByEntity(vs, p)) {
                m.chain = p, m.chain_id = v->id;
                break;
            }
            const char* n = mem::RttiName(mem::RttiOf(p));
            if (n && std::strstr(n, "DSVehicle")) m.chain = p;
        }
    }
}
const char* ChainText(const Mule& m, const Veh* v) noexcept {
    return !m.chain ? "none" : v && m.chain == v->entity ? "this" : m.chain_id != kNoActor ? "another vehicle" : "a vehicle not in the pools";
}

// truck_v3 9: the walked vehicles, logged when the set (ids, drivers) changes (run 20261126-235943 logged "7 vehicles" and never
// which: a MULE truck missing from the walk and an empty one looked the same).
std::uint64_t g_vehset_hash = 0;
void VehListOnChange(const std::vector<Veh>& vs) noexcept {
    std::uint64_t h = 1469598103934665603ull;
    for (const Veh& v : vs) h = (h ^ ((static_cast<std::uint64_t>(v.id) << 24) ^ v.driver)) * 1099511628211ull;
    if (h == g_vehset_hash) return;
    g_vehset_hash = h;
    for (const Veh& v : vs) {
        Xf x;
        const bool xf = ReadXf(v.entity, x);
        Log("[truckv3] veh %s 0x%05X at (%.1f, %.1f, %.1f) driver 0x%05X (%s) +0x63C %d input %s ride state %d", kKindName[v.kind], v.id,
            xf ? x.pos[0] : 0.0, xf ? x.pos[1] : 0.0, xf ? x.pos[2] : 0.0, v.driver, trucks::DriverName(trucks::DriverOf(v.driver)), v.has,
            !v.input ? "none" : mem::Q(v.input) == A(kCtlInVt) ? "npc-ctl" : RttiNameOf(v.input), v.ride);
    }
}

// ---------------------------------------------------------------- truck_v3 9: one vehicle
void IdentOne(const Veh& v, const std::vector<Mule>& mules) noexcept {
    CtlRead r;
    r.truck_id = v.id, r.truck_entity = v.entity, r.has_driver = v.has, r.driver_id = v.driver, r.input = v.input;
    r.input_vt = v.input ? mem::Q(v.input) : 0;
    r.want_vt = A(kCtlInVt);
    const Mule* m = nullptr;
    for (const Mule& x : mules)
        if (x.id == v.driver) m = &x;
    std::uintptr_t other = 0;
    if (m) {
        r.actor_found = r.actor_is_mule = true;
        r.actor = m->actor;
    } else if ((other = AnyActorOf(v.driver)) != 0) {
        r.actor_found = true;
        r.actor = other;
    }
    r.ride_ok = (g_ok & (kGRide | kGCtl)) == (kGRide | kGCtl);
    if (m && r.ride_ok) {
        const std::uintptr_t ctl = m->actor + kActRide + kRCtl;
        r.ctl30 = ctl + kCInput;
        r.ctl_state = I32(ctl + kCState);
        r.ctl_driver = U32(ctl + kCDriver);
        const std::uintptr_t h = mem::Q(ctl);
        r.ctl_vehicle = h ? h - 0x20 : 0;
        r.drive_comp = mem::Q(ctl + kCDriveComp);
        r.seat = m->seat, r.ride_vehicle_id = m->veh_id, r.ride_vehicle = m->veh, r.ride_bits = static_cast<std::uint8_t>(m->bits);
    }
    const Verdict vd = IdentityOf(r);
    g_tally.Add(vd);
    char mis[160];
    MismatchText(vd.mis, mis, sizeof(mis));
    Log("[truckv3] id %s 0x%05X driver %s 0x%05X: input %p vt %s, ctl+0x30 %p -> %s | ctl state %d driver 0x%05X vehicle %s drive comp %p | "
        "ride seat %d vehicle 0x%05X handle %s riding %u chain %s | +0x63C %d ride state %d speed %.1f km/h life %.0f/%.0f%s%s | %s%s",
        kKindName[v.kind], v.id, r.actor_is_mule ? "MULE" : r.actor_found ? RttiNameOf(r.actor) : "?", v.driver, reinterpret_cast<void*>(v.input),
        !v.input ? "-" : r.input_vt == r.want_vt ? "npc-ctl" : RttiNameOf(v.input), reinterpret_cast<void*>(r.ctl30), IdentityName(vd.id),
        r.ctl_state, r.ctl_driver, !r.ctl_vehicle ? "none" : r.ctl_vehicle == v.entity ? "this" : "OTHER", reinterpret_cast<void*>(r.drive_comp),
        r.seat, r.ride_vehicle_id, !r.ride_vehicle ? "none" : r.ride_vehicle == v.entity ? "this" : "OTHER", r.ride_bits & 1u,
        m ? ChainText(*m, &v) : "-", v.has, v.ride,
        static_cast<double>(v.speed), static_cast<double>(v.life), static_cast<double>(v.max_life), (v.broken & 0x2000u) ? " broken" : "",
        (v.exploded & 0x100000u) ? " exploded" : "", vd.mis ? "mismatch: " : "all agree", mis);
    if (!v.input) return;
    std::uint8_t in[vwire::kDriveInputBytes] = {};
    if (!mem::Read(v.input + 0x08, in, sizeof(in))) return;
    float f[11];
    std::memcpy(f, in, sizeof(f));
    Log("[truckv3] id %s 0x%05X inputs %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f bools %u %u %u pad %02X | "
        "the wire check %s",
        kKindName[v.kind], v.id, static_cast<double>(f[0]), static_cast<double>(f[1]), static_cast<double>(f[2]), static_cast<double>(f[3]),
        static_cast<double>(f[4]), static_cast<double>(f[5]), static_cast<double>(f[6]), static_cast<double>(f[7]), static_cast<double>(f[8]),
        static_cast<double>(f[9]), static_cast<double>(f[10]), in[44], in[45], in[46], in[47],
        vwire::InputsValid(in) ? "accepts" : "REJECTS");
}

void IdentDump() noexcept {
    std::vector<Veh> vs;
    std::vector<Mule> ms;
    WalkVehicles(vs);
    WalkMules(ms);
    MarkChains(ms, vs);
    VehListOnChange(vs);
    unsigned n = 0, same = 0, riding = 0, by_chain = 0;
    for (const Veh& v : vs) {
        if (!trucks::NonPlayer(trucks::DriverOf(v.driver))) continue;
        const unsigned before = g_tally.same;
        IdentOne(v, ms);
        ++n;
        if (g_tally.same != before) ++same;
    }
    // MULEs riding that drive nothing here (passengers, or a vehicle whose seat names someone else)
    for (const Mule& m : ms) {
        if (!m.seated()) continue;
        ++riding;
        if (!m.riding()) ++by_chain;
        const Veh* v = VehById(vs, m.truck());
        if (v && v->driver == m.id) continue;  // logged above
        Log("[truckv3] id rider MULE 0x%05X seat %d vehicle 0x%05X (%s) handle %s ctl state %d bit %u chain %s, the vehicle's driver 0x%05X",
            m.id, m.seat, m.truck(), v ? kKindName[v->kind] : "not in the pools", !m.veh ? "none" : v && m.veh == v->entity ? "this" : "OTHER",
            m.ctl_state, m.bits & 1u, ChainText(m, v), v ? v->driver : kNoActor);
    }
    Log("[truckv3] id done: %zu vehicles, %u non-player-driven, identity same %u/%u, %zu MULEs (%u riding, %u by the parent chain only); run tally "
        "checked %u same %u differs %u other %u all-agree %u no-drive-comp %u",
        vs.size(), n, same, n, ms.size(), riding, by_chain, g_tally.checked, g_tally.same, g_tally.differs, g_tally.other, g_tally.agree,
        g_tally.no_drive_comp);
}

// ---------------------------------------------------------------- truck_v3 8: seats (Q7)
void SeatDump() noexcept {
    std::vector<Veh> vs;
    std::vector<Mule> ms;
    WalkVehicles(vs);
    WalkMules(ms);
    MarkChains(ms, vs);
    unsigned n = 0, done = 0;
    for (const Mule& m : ms) {
        if (!m.seated()) continue;
        ++n;
        const Veh* v = m.riding() ? VehByEntity(vs, m.veh) : nullptr;
        if (!v && m.chain) v = VehByEntity(vs, m.chain);
        if (!v) v = VehById(vs, m.truck());
        Xf mx, vx;
        if (!v || !m.entity || !ReadXf(m.entity, mx) || !ReadXf(v->entity, vx)) {
            Log("[truckv3] seat MULE 0x%05X seat %d vehicle 0x%05X: not compared (%s)", m.id, m.seat, m.truck(),
                !v ? "vehicle not found" : !m.entity ? "MULE not realized" : "a transform unreadable");
            continue;
        }
        ++done;
        double lp[3], lr[3][3];
        LocalOf(vx, mx, lp, lr);
        const std::uintptr_t parent = mem::Q(m.entity + kEParent);
        Log("[truckv3] seat MULE 0x%05X seat %d %s 0x%05X: local (%.3f %.3f %.3f) yaw %.1f rows (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f) "
            "parent %s | MULE (%.2f %.2f %.2f) vehicle (%.2f %.2f %.2f) speed %.1f | bit %u chain %s",
            m.id, m.seat, kKindName[v->kind], v->id, lp[0], lp[1], lp[2], LocalYawDeg(lr), lr[0][0], lr[0][1], lr[0][2], lr[1][0], lr[1][1], lr[1][2],
            lr[2][0], lr[2][1], lr[2][2], !parent ? "none" : parent == v->entity ? "the vehicle" : RttiNameOf(parent), mx.pos[0], mx.pos[1], mx.pos[2],
            vx.pos[0], vx.pos[1], vx.pos[2], static_cast<double>(v->speed), m.bits & 1u, ChainText(m, v));
        // the capsule (Humanoid +0x5A0, a weak ref: the component at ref - 0x20; its +0x48 the entity)
        const std::uintptr_t cref = mem::Q(m.entity + kECapsule);
        const std::uintptr_t cap = cref ? cref - 0x20 : 0;
        std::uint8_t dis = 0, inw = 0;
        std::uint32_t layer = 0;
        const bool rd = cap && mem::Rd(cap + 0x58, dis) && mem::Rd(cap + 0x5C, inw) && mem::Rd(cap + 0x7C, layer);
        Log("[truckv3] seat MULE 0x%05X capsule %p rtti %s back-ref %s disable %u in-world %u layer %u%s", m.id, reinterpret_cast<void*>(cap),
            cap ? RttiNameOf(cap) : "-", !cap ? "-" : mem::Q(cap + 0x48) == m.entity ? "this MULE" : "OTHER", dis, inw, layer, rd ? "" : " (unreadable)");
    }
    Log("[truckv3] seats done: %u riding MULEs, %u compared", n, done);
}

// ---------------------------------------------------------------- truck_v3 1: the watch
constexpr std::size_t kWatchMax = 256;
struct Seen {
    Ride r;
    bool has = false, live = false;
};
Seen g_seen[kWatchMax];
std::size_t g_seen_n = 0;
// the vehicle side: each vehicle's driver seat (+0x8B0) at its previous sample
struct SeenVeh {
    std::uint32_t id = 0, driver = kNoActor;
};
constexpr std::size_t kVehWatchMax = 128;
SeenVeh g_vseen[kVehWatchMax];
std::size_t g_vseen_n = 0;

void WatchDrivers(const std::vector<Veh>& vs, const std::vector<Mule>& ms) noexcept {
    for (const Veh& v : vs) {
        SeenVeh* s = nullptr;
        for (std::size_t i = 0; i < g_vseen_n && !s; ++i)
            if (g_vseen[i].id == v.id) s = &g_vseen[i];
        const bool had = s != nullptr;
        if (!s) {
            if (g_vseen_n >= kVehWatchMax) continue;
            s = &g_vseen[g_vseen_n++];
            s->id = v.id, s->driver = kNoActor;
        }
        const DrvEvent ev = DriverChanged(s->driver, had, v.driver);
        if (ev != DrvEvent::none) {
            Xf x;
            const bool xf = ReadXf(v.entity, x);
            Log("[truckv3] vdriver %s 0x%05X %s: driver 0x%05X -> 0x%05X (%s) +0x63C %d ride state %d speed %.1f input %s at (%.1f, %.1f, %.1f)",
                kKindName[v.kind], v.id, DrvEventName(ev), s->driver, v.driver, trucks::DriverName(trucks::DriverOf(v.driver)), v.has, v.ride,
                static_cast<double>(v.speed), !v.input ? "none" : mem::Q(v.input) == A(kCtlInVt) ? "npc-ctl" : RttiNameOf(v.input),
                xf ? x.pos[0] : 0.0, xf ? x.pos[1] : 0.0, xf ? x.pos[2] : 0.0);
            // a non-player took the driver seat: the identity at once (the MULE side's board line may come a sample later, or never)
            if (ev != DrvEvent::cleared && trucks::NonPlayer(trucks::DriverOf(v.driver))) IdentOne(v, ms);
        }
        s->driver = v.driver;
    }
}

void Watch(double now) noexcept {
    std::vector<Veh> vs;
    std::vector<Mule> ms;
    WalkVehicles(vs);
    WalkMules(ms);
    MarkChains(ms, vs);
    WatchDrivers(vs, ms);
    for (std::size_t i = 0; i < g_seen_n; ++i) g_seen[i].live = false;
    for (const Mule& m : ms) {
        Ride now_r;
        now_r.mule = m.id, now_r.truck = m.truck(), now_r.seat = m.seat, now_r.ctl_state = m.ctl_state, now_r.riding = m.seated();
        Seen* s = nullptr;
        for (std::size_t i = 0; i < g_seen_n && !s; ++i)
            if (g_seen[i].r.mule == m.id) s = &g_seen[i];
        for (std::size_t i = 0; i < g_seen_n && !s; ++i)
            if (!g_seen[i].has && g_seen[i].r.mule == 0xFFFFFFFFu) s = &g_seen[i];  // a gone MULE's slot
        if (!s) {
            if (g_seen_n >= kWatchMax) continue;
            s = &g_seen[g_seen_n++];
        }
        if (s->r.mule != m.id) s->has = false, s->r = Ride{};
        const RideEvent ev = RideChanged(s->r, s->has, now_r, true);
        if (ev != RideEvent::none) {
            const std::uint32_t tid = ev == RideEvent::leave ? s->r.truck : now_r.truck;
            const Veh* v = VehById(vs, tid);
            Log("[truckv3] %s MULE 0x%05X %s 0x%05X seat %d ctl state %d riding %d (the vehicle's driver 0x%05X, +0x63C %d, speed %.1f) bit %u "
                "chain %s",
                RideEventName(ev), m.id, v ? kKindName[v->kind] : "vehicle", tid, ev == RideEvent::leave ? s->r.seat : m.seat, m.ctl_state,
                now_r.riding ? 1 : 0, v ? v->driver : kNoActor, v ? v->has : 0, v ? static_cast<double>(v->speed) : 0.0, m.bits & 1u,
                ChainText(m, v));
            // the driver's identity (WatchDrivers also reads it when the vehicle's seat changes; a second line is harmless)
            if (ev == RideEvent::board && v && v->driver == m.id) IdentOne(*v, ms);
        }
        s->r = now_r, s->has = true, s->live = true;
    }
    for (std::size_t i = 0; i < g_seen_n; ++i) {
        Seen& s = g_seen[i];
        if (s.live || !s.has) continue;
        if (s.r.riding) Log("[truckv3] leave MULE 0x%05X vehicle 0x%05X seat %d (the MULE is gone from the registry)", s.r.mule, s.r.truck, s.r.seat);
        s.has = false;
        s.r = Ride{};
        s.r.mule = 0xFFFFFFFFu;
    }
    if (now < g_next_drive_ms) return;
    g_next_drive_ms = now + 5000;
    for (const Veh& v : vs) {
        if (!trucks::NonPlayer(trucks::DriverOf(v.driver))) continue;
        float f[4] = {};
        if (v.input) mem::Read(v.input + 0x08, f, sizeof(f));
        Log("[truckv3] drive %s 0x%05X driver 0x%05X ride state %d speed %.1f km/h input %s in %.2f %.2f %.2f %.2f life %.0f", kKindName[v.kind], v.id,
            v.driver, v.ride, static_cast<double>(v.speed), !v.input ? "none" : mem::Q(v.input) == A(kCtlInVt) ? "npc-ctl" : "other",
            static_cast<double>(f[0]), static_cast<double>(f[1]), static_cast<double>(f[2]), static_cast<double>(f[3]), static_cast<double>(v.life));
    }
}

void StatusLine(const char* why) noexcept {
    char sites[256];
    std::size_t n = 0;
    sites[0] = 0;
    for (unsigned i = 0; i <= std::size(kGetOffCallers); ++i) {
        const unsigned c = g_site_n[i].load(std::memory_order_relaxed);
        if (!c) continue;
        const int w = i < std::size(kGetOffCallers)
                          ? std::snprintf(sites + n, sizeof(sites) - n, "%s0x%llX x%u", n ? ", " : "",
                                          static_cast<unsigned long long>(kGetOffCallers[i].site), c)
                          : std::snprintf(sites + n, sizeof(sites) - n, "%sunknown x%u", n ? ", " : "", c);
        if (w <= 0 || static_cast<std::size_t>(w) >= sizeof(sites) - n) break;
        n += static_cast<std::size_t>(w);
    }
    Log("[truckv3] status (%s): byte checks %u/%u (groups ok 0x%02X), get-off detour %s, get-off calls %u (%s; lost %u), id checked %u same %u "
        "differs %u other %u all-agree %u no-drive-comp %u, watch %s",
        why, g_pass, g_total, g_ok, g_detour, g_off_total.load(std::memory_order_relaxed), n ? sites : "none", g_off_lost, g_tally.checked,
        g_tally.same, g_tally.differs, g_tally.other, g_tally.agree, g_tally.no_drive_comp, g_watch.load() ? "on" : "off");
}

void RunChecks() noexcept {
    g_checked = true;
    g_ok = kGAll;
    g_pass = g_total = 0;
    auto fail = [](unsigned group, const char* what, std::uintptr_t va) {
        g_ok &= ~group;
        Log("[truckv3] byte check FAILED at 0x%llX: %s", static_cast<unsigned long long>(va), what);
    };
    for (const Check& c : kChecks) {
        ++g_total;
        if (mem::CodeMatches(A(c.va), c.sig)) ++g_pass;
        else fail(c.group, c.what, c.va);
    }
    struct Structural {
        bool ok;
        unsigned group;
        const char* what;
        std::uintptr_t va;
    };
    std::uintptr_t mule_vt = A(kMuleVt);  // RttiOf reads [obj]: a local holding the vtable stands for an object
    const char* mule_name = mem::RttiName(mem::RttiOf(reinterpret_cast<std::uintptr_t>(&mule_vt)));
    const Structural ss[] = {
        {mem::RipTarget(A(0x142F7509C), 3, 7) == A(kVehReg), kGPools, "the vehicle manager global's target", 0x142F7509C},
        {mem::Q(A(kVehVt[2]) + 0x110) == A(kTruckSetDriver), kGVeh, "DSVehicleTruck vt +0x110 = SetDriver", kVehVt[2]},
        {mem::RipTarget(A(0x1422D6268), 3, 7) == A(kActReg), kGActors, "the actor registry global's target", 0x1422D6268},
        {mule_name && std::strcmp(mule_name, "DSMuleGameActor") == 0, kGActors, "the MULE actor vtable's RTTI is DSMuleGameActor", kMuleVt},
        {mem::RipTarget(A(0x142A98247), 1, 5) == A(kCtlCtor), kGRide, "the MULE ctor calls the controller ctor at +0x3D0", 0x142A98247},
        {mem::RipTarget(A(0x142B194A8), 1, 5) == A(kCtlAttach), kGRide, "the driver seat calls Attach", 0x142B194A8},
        {mem::RipTarget(A(0x142F51C7D), 3, 7) == A(kCtlInVt), kGCtl, "the controller ctor's input vtable", 0x142F51C7D},
    };
    for (const Structural& s : ss) {
        ++g_total;
        if (s.ok) ++g_pass;
        else fail(s.group, s.what, s.va);
    }
    for (const KnownCaller& k : kGetOffCallers) {
        ++g_total;
        if (mem::CodeMatches(A(k.site), "E8") && mem::RipTarget(A(k.site), 1, 5) == A(kGetOffVa)) ++g_pass;
        else fail(kGOff, "a get-off call site", k.site);
    }
}

}  // namespace

void Install(bool probe_on) noexcept {
    g_key = probe_on;
    if (!probe_on) return;
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    const std::uintptr_t base = mem::Base();
    if (base && mem::Rd(base, dos) && dos.e_magic == IMAGE_DOS_SIGNATURE && mem::Rd(base + static_cast<std::uintptr_t>(dos.e_lfanew), nt) &&
        nt.Signature == IMAGE_NT_SIGNATURE)
        g_image_size = nt.OptionalHeader.SizeOfImage;
    RunChecks();
    if ((g_ok & kGOff) && g_image_size) {
        const MH_STATUS init = MH_Initialize();
        if (init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED) {
            g_rec.store(true);
            MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(A(kGetOffVa)), reinterpret_cast<void*>(&H_GetOff), reinterpret_cast<void**>(&g_orig));
            if (s == MH_OK) s = MH_EnableHook(reinterpret_cast<void*>(A(kGetOffVa)));
            g_detour = s == MH_OK ? "on" : MH_StatusToString(s);
            if (s != MH_OK) g_rec.store(false);
        } else {
            g_detour = "off (MinHook)";
        }
    } else {
        g_detour = g_image_size ? "off (byte checks)" : "off (image size)";
    }
    Log("[truckv3] byte checks %u/%u ok (groups ok 0x%02X%s), get-off detour %s", g_pass, g_total, g_ok,
        (g_ok & kRequired) == kRequired ? "" : "; REQUIRED groups failed: the probe is off", g_detour);
}

void Tick(double now_ms) noexcept {
    if (!g_key) return;
    g_game_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    DrainOff();
    if (!g_watch.load() || (g_ok & kRequired) != kRequired || now_ms < g_next_ms) return;
    g_next_ms = now_ms + g_period_ms;
    Watch(now_ms);
}

bool Command(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (OpOf(v[0]) == Op::v3) return V3Status(msg, cap);  // round 28 T1: vehicle_mule_v3's status, whatever the probe key
    if (!g_key) return std::snprintf(msg, cap, "vehicle_truck_probe=0"), false;
    if ((g_ok & kRequired) != kRequired) return std::snprintf(msg, cap, "byte checks failed (groups ok 0x%02X)", g_ok), false;
    switch (OpOf(v[0])) {
        case Op::off:
            g_watch.store(false);
            StatusLine("truck_v3 0");
            return std::snprintf(msg, cap, "watch off; status logged"), true;
        case Op::on:
            g_period_ms = PeriodOf(v[1]) * 1000.0;
            g_next_ms = 0, g_next_drive_ms = 0;
            g_watch.store(true);
            Log("[truckv3] watch on (every %.1f s)", g_period_ms / 1000.0);
            return std::snprintf(msg, cap, "watch on (%.1f s)", g_period_ms / 1000.0), true;
        case Op::seats:
            SeatDump();
            return std::snprintf(msg, cap, "seat dump logged"), true;
        case Op::ident:
            IdentDump();
            return std::snprintf(msg, cap, "identity dump logged"), true;
        case Op::v3:
        case Op::bad:
            break;
    }
    return std::snprintf(msg, cap, "op 0 off, 1 [s] watch, 3 v3 status, 8 seats, 9 identity"), false;
}

void EmergencyStop() noexcept {
    g_rec.store(false);
    g_watch.store(false);
}

}  // namespace st::truckv3
