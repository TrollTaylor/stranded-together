// Enemy sync P2: her hits on the host's enemies and their outcomes (enemy_combat.hpp; docs/enemy-sync.md 9).
#include "enemy_combat.hpp"

#include <windows.h>

#include "MinHook.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "automation.hpp"
#include "config_values.hpp"
#include "coop_session.hpp"
#include "enemy_census.hpp"
#include "enemy_probe_rules.hpp"
#include "enemy_rules.hpp"
#include "enemy_sync.hpp"
#include "enemy_wire.hpp"
#include "link.hpp"
#include "log.hpp"
#include "partner_source.hpp"
#include "partner_tackle.hpp"
#include "real_body.hpp"
#include "shot_sync.hpp"
#include "shot_wire.hpp"

namespace st::ecombat {
static_assert(ewire::kHitResultMax + 1u == erules::kHitResults, "the HITRES result range (enemy_wire) is erules::HitResult's");
namespace {
namespace mem = partner::mem;
using game::WorldTransform;

constexpr std::uintptr_t kImage = 0x140000000ull;
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

// ---------------------------------------------------------------- game addresses (ds.exe, Director's Cut; round 17 r-p2 research)
constexpr std::uintptr_t kOnHit = 0x142349A10;         // Entity OnHit: receiver vt+0x10 for every entity (receiver = entity + 0x38)
constexpr std::uintptr_t kDmgSys = 0x147BC4E70;        // the damage system OnHit jumps through (vt+0x40 QueueDamage)
constexpr std::uintptr_t kMuleDmg = 0x142B020F0;       // DSMuleDamageComponent's MsgDamage handler (component, msg)
constexpr std::uintptr_t kMuleDmgThunk = 0x142B2D250;  // the handler table's thunk to it
constexpr std::uintptr_t kCtxCreate = 0x14298AC90;     // the attack context (owner entity | 0, u16 attack)
constexpr std::uintptr_t kCtxParams = 0x14294DDF0;     // its parameter block P = ctx + 0x120 (after its type check)
constexpr std::uintptr_t kAttackOf = 0x14294DD50;      // a DamageInfo's P (checked only)
constexpr std::uintptr_t kDIInit = 0x142314190;        // the DamageInfo init (DI, ref, 16 B, amount, &impulse, a20, -1, part)
constexpr std::uintptr_t kRelease = 0x142463390;       // a handle's release (DI + 0x28)
constexpr std::uintptr_t kHandleSet = 0x142463410;     // a handle's set (checked only: DIInit's call)
constexpr std::uintptr_t kG16 = 0x147BC3B80;           // BulletDamageA's 16 bytes for DI+0 (its lea at 0x14291216C)
constexpr std::uintptr_t kAttackReg = 0x147BEB0F8;     // the attack registry (+0x60 count, +0x68 list; entry +0x20 u16 attack)
constexpr std::uintptr_t kCtxType = 0x144EC8160;       // the context's type id (u16) and range (u16 at +2)
constexpr std::uintptr_t kMuleDmgVt = 0x143DB9D28;     // DSMuleDamageComponent's vtable
constexpr std::uintptr_t kRegGlobal = 0x147BBB8C0;     // the game actor registry (vt+0x40: the local player's actor id)
constexpr std::uintptr_t kMuleVt = 0x143DB7078;        // DSMuleGameActor

constexpr std::uintptr_t kActId = 0x08, kActInUse = 0x20, kActState = 0x24, kActEntRef = 0x68, kActFlags88 = 0x88, kActMule = 0xEA0, kActDown = 0xEA4,
                         kActFlagsEA8 = 0xEA8;
constexpr std::uintptr_t kCompHealth = 0x1EC, kCompStamina = 0x1F0, kCompSleep = 0x1F4, kCompRestraint = 0x1F8, kCompDefeat = 0x314, kCompHead = 0x528;
constexpr std::uintptr_t kEParent = 0x70, kEComponents = 0x90, kReceiverOfEntity = 0x38;

enum Group : unsigned { kGOnHit = 1, kGMule = 2, kGStamps = 4, kGApply = 8, kGOutcome = 16, kGEntry = 32, kGCarry = 64 };
constexpr std::uintptr_t kOutcomeSet = 0x142AA8000;   // P1c: the MULE outcome setter (DSMuleGameActor*, k 0 faint, 1 sleep, 2 restrained, 3 dead)
struct Site {
    std::uintptr_t va;
    const char* sig;
    const char* name;
    unsigned group;
};
// clang-format off
constexpr Site kSites[] = {
    {kOnHit,        "48 8B 05 59 B4 87 05 4C 8B C2 48 8D 51 C8 48 8B C8 4C 8B 08 49 FF 61 40", "Entity OnHit", kGOnHit},
    {kMuleDmg,      "4C 8B DC 55 57 49 8D 6B A1 48 81 EC F8 00 00 00 48 8B 05 29 EF 45 02 48 33 C4 48 89 45 27 48 8B", "the MULE damage handler", kGMule},
    {kMuleDmgThunk, "E9 9B 4E FD FF", "the MULE damage handler's thunk", kGMule},
    {0x142B0210E,   "48 8B 41 48", "the handler's entity (+0x48)", kGMule},
    {0x142B0216C,   "48 8D 8F B0 01 00 00", "the handler's serial ring (+0x1B0)", kGMule},
    {0x142B0221D,   "FF 90 58 01 00 00", "the handler's entry builder call (vt+0x158)", kGMule},
    {0x14291224C,   "8B 87 E0 00 00 00 44 8B C5 89 83 28 01 00 00 8B 87 94 00 00 00 89 83 30 01 00 00 44 0F B7 4F 5C 66 44 89 8B 2C 01 00 00",
                    "BulletDamageA's stamps (serial, +0x130, attack)", kGStamps},
    {0x1429122BD,   "48 89 93 38 01 00 00 48 8D 54 24 60 0F B6 87 6D 01 00 00 88 83 80 01 00 00 8B 87 38 01 00 00 89 83 84 01 00 00 "
                    "8B 87 D4 00 00 00 89 83 88 01 00 00 48 8B 06 FF 50 10", "BulletDamageA's stamps (resource, +0x180, power, +0x188) and its call", kGStamps},
    {0x14291216C,   "4C 8D 05 0D 1A 2B 05", "BulletDamageA's 16 bytes", kGStamps},
    {0x142912274,   "4C 8B 1D 7D 8E 2D 05", "BulletDamageA's attack registry", kGStamps},
    {kCtxCreate,    "48 89 5C 24 18 48 89 74 24 20 55 57 41 56 48 8D 6C 24 80 48 81 EC 80 01 00 00 48 8B 05 7F 63 5D", "the attack context", kGApply},
    {kCtxParams,    "40 53 48 83 EC 20 48 8B D9 48 85 C9 74 49 48 8B 01 FF 10 44 0F B7 05 55", "the context's parameters", kGApply},
    {kAttackOf,     "48 89 5C 24 08 57 48 83 EC 20 48 8B 41 28 BA 38 00 00 00 48 85 C0 48 8D", "a DamageInfo's parameters", kGApply},
    {kDIInit,       "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 C4 C1 78 10 00 48 8B 44 24 60 33 ED", "the DamageInfo init", kGApply},
    {0x14231420B,   "89 41 68", "the DamageInfo's part (+0x68)", kGApply},
    {0x142314247,   "C5 FA 11 73 78", "the DamageInfo's amount (+0x78)", kGApply},
    {kRelease,      "40 53 48 83 EC 20 48 8B D9 48 8B 49 18 48 85 C9 74 05 E8", "the handle release", kGApply},
    {kHandleSet,    "40 53 48 83 EC 20 48 8B D9 E8 22 00 00 00 48 8B 0B 33 D2 48 85 C9 48 8D 41 E0 48 0F 44 C2", "the handle set", kGApply},
    {0x142ACEDB0,   "48 89 5C 24 10 57 48 83 EC 40 48 8B 05 6F 22 49 02 48 33 C4 48 89 44 24", "the MULE state setter", kGOutcome},
    // P1c (round 21): the outcome setter, its realized path's damage component (+0xE90) and two calls; a mismatch turns only the
    // outcome carry off (her downs then keep a camp hers)
    {kOutcomeSet,   "48 89 5C 24 10 57 48 83 EC 70 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 60 83 79 24 02 48 8B D9", "the MULE outcome setter", kGCarry},
    {0x142AA8022,   "48 8B 89 90 0E 00 00 48 85 C9 74 4A", "the outcome setter's damage component (+0xE90)", kGCarry},
    {0x142AF6930,   "4C 8B DC 53 48 81 EC F0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 D0 00 00 00", "the damage component's outcome", kGCarry},
    {0x142AF97D0,   "40 53 48 83 EC 20 83 79 50 00 48 8B D9 7E 0F", "the damage component's queue drain (+0x50)", kGCarry},
    // (spec 6.1; review rv-correctness R10: read with tools/re/dsre.py)
    {0x142AF8FC0,   "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 49 8B D8 8B F2 48 8B F9 E8", "the damage component's entry", kGCarry},
    {0x142AC09B9,   "48 89 87 90 0E 00 00 48 85 C0", "the damage component's store (+0xE90)", kGCarry},
    {0x142ACEE3B,   "89 BB A0 0E 00 00", "the MULE state (+0xEA0)", kGOutcome},
    {0x142B1E190,   "48 89 5C 24 08 57 48 83 EC 20 48 8B 81 78 06 00 00 48 8B D9 8B FA 48 8B", "the MULE down setter", kGOutcome},
    {0x142B26348,   "C5 7A 10 81 EC 01 00 00", "health (+0x1EC)", kGOutcome},
    {0x142B2690C,   "C5 F8 2F B6 F0 01 00 00", "stamina (+0x1F0)", kGOutcome},
    {0x142B2693A,   "C5 F8 2F 86 E4 01 00 00", "the sleep maximum (+0x1E4)", kGOutcome},
    {0x142B269C8,   "C5 FA 10 86 F8 01 00 00", "restraint (+0x1F8)", kGOutcome},
    {0x142B269B0,   "8B 86 E0 01 00 00", "the stamina maximum (+0x1E0)", kGOutcome},
    {0x142B26923,   "C7 86 14 03 00 00 01 00 00 00", "the defeat now (+0x314 = 1)", kGOutcome},
    {0x142B079C2,   "8B 83 DC 01 00 00", "the revive (+0x1DC)", kGOutcome},
    {0x142EDA10A,   "8B 81 A0 0E 00 00", "DsIsMuleDeadOrFainted (+0xEA0)", kGOutcome},
    {0x142AF8A24,   "8B 42 68", "the entry's part (DI+0x68)", kGEntry},
    {0x142AF8A2A,   "41 89 41 60", "the entry's part (+0x60)", kGEntry},
    {0x142AF8A58,   "41 8B 40 20 41 89 41 64", "the entry's attacker id (P+0x20 -> +0x64)", kGEntry},
    {0x142AF8A31,   "41 8B 40 08", "the entry's serial (P+0x08)", kGEntry},
    {0x142AF8A38,   "41 89 41 50", "the entry's serial (+0x50)", kGEntry},
    {0x142AF8A4E,   "41 0F B7 40 0C", "the entry's attack (P+0x0C)", kGEntry},
};
// clang-format on

using FnOnHit = void (*)(std::uintptr_t receiver, std::uintptr_t di);
using FnMuleDmg = void (*)(std::uintptr_t component, std::uintptr_t msg);
using FnCtxCreate = std::uintptr_t (*)(std::uintptr_t owner, std::uint16_t attack);
using FnCtxParams = std::uintptr_t (*)(std::uintptr_t ctx);
using FnDIInit = void* (*)(void* di, std::uintptr_t ref, const void* g16, float amount, const float* impulse4, float a20, float m1, std::uint32_t part);
using FnRelease = void (*)(void* handle);
using FnPlayerId = const std::uint32_t* (*)(std::uintptr_t reg, void* out);

FnOnHit o_onhit = nullptr;
FnMuleDmg o_muledmg = nullptr;

// ---------------------------------------------------------------- state shared with the detours (any thread)
thread_local int t_bullet = 0;         // inside BulletDamageA / B's original (shot_sync)
thread_local bool t_applying = false;  // the host's own apply (its OnHit passes)
std::atomic<bool> g_g2{false}, g_g3{false};   // the detours are in
std::atomic<bool> g_stop{false};              // a fault: everything passes through, nothing captured or applied
std::atomic<bool> g_capture{false};           // the guest captures her hits on mirrors (hits on)
std::atomic<std::uintptr_t> g_sam{0};
std::atomic<std::uint32_t> g_player_id{0};    // the registry's local player actor id (her attacks carry it in P+0x20; 0 is the player's: P2-0)
std::atomic<bool> g_player_ok{false};         // ... read
std::atomic<std::uintptr_t> g_watch{0};       // enemy_hit_watch's entity
std::atomic<std::uint32_t> g_watch_id{0};
std::atomic<unsigned> g_watch_logged{0};
std::atomic<std::uint16_t> g_watch_attack{0};  // the newest attack seen on the watched enemy (enemy_hit 0 takes it)
std::atomic<unsigned> g_a_mirror{0}, g_a_ours{0}, g_a_capt{0}, g_a_others{0}, g_a_ctx_bad{0}, g_m_mirror{0}, g_m_ours{0}, g_b_pass{0}, g_b_noent{0},
    g_ring_full{0}, g_faults{0}, g_a_noattack{0};
std::atomic<unsigned> g_ctx_logged{0};

// The capture (a POD the ring carries to the game thread).
struct Cap {
    std::uint32_t key = 0;
    std::uint8_t depth = 0, kind = 0, flags = 0, b12E = 0, b180 = 0;
    bool lethal = false, have_point = false, ctx_ok = false;
    std::uint16_t attack = 0;
    std::uint32_t serial = 0, v130 = 0, v184 = 0, v188 = 0, attacker_id = 0, part = 0xFFFFFFFFu;
    float amount = 0, mult = 1, a20 = 1;
    double point[3] = {};
    float normal[3] = {}, impulse[3] = {};
    std::uintptr_t entity = 0, attacker = 0;
};
// A bounded multi-producer ring, one consumer (the game thread; Vyukov's bounded queue).
constexpr unsigned kRing = 64;
struct Cell {
    std::atomic<std::uint32_t> seq{0};
    Cap c;
};
Cell g_ring[kRing];
std::atomic<std::uint32_t> g_enq{0};
std::uint32_t g_deq = 0;
void RingInit() noexcept {
    for (unsigned i = 0; i < kRing; ++i) g_ring[i].seq.store(i, std::memory_order_relaxed);
    g_enq.store(0), g_deq = 0;
}
bool RingPush(const Cap& c) noexcept {
    std::uint32_t pos = g_enq.load(std::memory_order_relaxed);
    for (;;) {
        Cell& cell = g_ring[pos % kRing];
        const std::uint32_t seq = cell.seq.load(std::memory_order_acquire);
        const std::int32_t dif = static_cast<std::int32_t>(seq - pos);
        if (dif == 0) {
            if (g_enq.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                cell.c = c;
                cell.seq.store(pos + 1, std::memory_order_release);
                return true;
            }
        } else if (dif < 0) {
            return false;  // full
        } else {
            pos = g_enq.load(std::memory_order_relaxed);
        }
    }
}
bool RingPop(Cap& out) noexcept {
    Cell& cell = g_ring[g_deq % kRing];
    const std::uint32_t seq = cell.seq.load(std::memory_order_acquire);
    if (static_cast<std::int32_t>(seq - (g_deq + 1)) < 0) return false;
    out = cell.c;
    cell.seq.store(g_deq + kRing, std::memory_order_release);
    ++g_deq;
    return true;
}

// ---------------------------------------------------------------- fault-free reads
std::uint32_t U32(std::uintptr_t a) noexcept {
    std::uint32_t v = 0;
    mem::Rd(a, v);
    return v;
}
std::uint16_t U16(std::uintptr_t a) noexcept {
    std::uint16_t v = 0;
    mem::Rd(a, v);
    return v;
}
std::uint8_t U8(std::uintptr_t a) noexcept {
    std::uint8_t v = 0;
    mem::Rd(a, v);
    return v;
}
float F32(std::uintptr_t a) noexcept {
    float v = 0;
    mem::Rd(a, v);
    return v;
}
bool ReadXf(std::uintptr_t e, WorldTransform& out) noexcept {
    std::uint8_t b[0x3C];
    if (!e || !mem::Read(e + 0xC8, b, sizeof(b))) return false;
    std::memcpy(&out.pos, b, 0x18);
    std::memcpy(&out.rot, b + 0x18, 0x24);
    return std::isfinite(out.pos.x) && std::isfinite(out.pos.y) && std::isfinite(out.pos.z);
}
double YawOf(const WorldTransform& t) noexcept { return std::atan2(static_cast<double>(t.rot.m[1][1]), static_cast<double>(t.rot.m[1][0])); }
bool NearKey(std::uintptr_t a, std::uintptr_t b) noexcept { return erules::NearKey(a, b); }
std::uintptr_t ReceiverEntity(std::uintptr_t receiver) noexcept {
    std::uint8_t ent = 0;
    return receiver && mem::Rd(receiver + 8, ent) && ent ? receiver - kReceiverOfEntity : 0;
}
// A weapon or a bullet of our Sam's (the shots' Enqueue test: the owner itself, its +0x300 holder, a weapon entity's instance holder).
bool OwnedBySam(std::uintptr_t o, std::uintptr_t sam) noexcept {
    if (!o || !sam) return false;
    if (NearKey(o, sam) || mem::Q(o + 0x300) == sam) return true;
    const std::uintptr_t inst = mem::Q(o + 0x338);
    return inst && mem::Q(inst + 0x300) == sam;
}
bool PartOfSam(std::uintptr_t e, std::uintptr_t sam) noexcept {
    for (int d = 0; e && sam && d < 4; ++d) {
        e = mem::Q(e + kEParent);
        if (e == sam) return true;
    }
    return false;
}
// The vehicle she drives (best effort, untested in a run): a vehicle-class attacker in our Sam's parent chain.
bool OurVehicle(std::uintptr_t attacker, std::uintptr_t sam) noexcept {
    if (!attacker || !sam) return false;
    std::uintptr_t p = sam;
    for (int d = 0; p && d < 4; ++d) {
        p = mem::Q(p + kEParent);
        if (p && NearKey(p, attacker)) return shotw::ClassifyHit(mem::RttiName(mem::RttiOf(p))) == shotw::HitClass::vehicle;
    }
    return false;
}
// The attack context's type, as the game checks it (0x14294DDF0): its RTTI's type id within the context type's range.
bool CtxTypeOk(std::uintptr_t ctx) noexcept {
    if (!ctx) return false;
    const std::uintptr_t rt = mem::RttiOf(ctx);
    if (!rt) return false;
    const std::uint16_t id = U16(rt), base = U16(A(kCtxType)), range = U16(A(kCtxType + 2));
    if ((id | base) & 0x8000) return false;  // the game's slow path (a derived type table): not taken here
    return static_cast<std::uint16_t>(id - base) <= range;
}
std::uintptr_t AttackEntry(std::uint16_t attack) noexcept {
    const std::uintptr_t reg = mem::Q(A(kAttackReg));
    std::int32_t n = 0;
    if (!reg || !mem::Rd(reg + 0x60, n) || n <= 0 || n > 4096) return 0;
    const std::uintptr_t list = mem::Q(reg + 0x68);
    for (std::int32_t i = 0; list && i < n; ++i) {
        const std::uintptr_t e = mem::Q(list + 8ull * static_cast<unsigned>(i));
        if (e && U16(e + 0x20) == attack) return e;
    }
    return 0;
}
// The damage component cache (entity -> component; review rv-behaviour 5: the host reads its outcomes many times a second). A hit is
// checked with two reads: the component's vtable and its entity (+0x48, the MULE handler's). Cleared on a world change.
std::map<std::uintptr_t, std::uintptr_t> g_comp_cache;
std::uintptr_t DamageComponent(std::uintptr_t entity) noexcept;
std::uintptr_t CachedComponent(std::uintptr_t entity) noexcept {
    if (!entity) return 0;
    const auto it = g_comp_cache.find(entity);
    if (it != g_comp_cache.end()) {
        if (mem::Q(it->second) == A(kMuleDmgVt) && mem::Q(it->second + 0x48) == entity) return it->second;
        g_comp_cache.erase(it);
    }
    const std::uintptr_t c = DamageComponent(entity);
    if (c && mem::Q(c + 0x48) == entity) {
        if (g_comp_cache.size() >= 256) g_comp_cache.clear();
        g_comp_cache[entity] = c;
    }
    return c;
}
std::uintptr_t DamageComponent(std::uintptr_t entity) noexcept {
    std::int32_t n = 0;
    if (!entity || !mem::Rd(entity + kEComponents, n) || n <= 0 || n > 96) return 0;
    const std::uintptr_t list = mem::Q(entity + kEComponents + 8);
    for (std::int32_t i = 0; list && i < n; ++i) {
        const std::uintptr_t c = mem::Q(list + 8ull * static_cast<unsigned>(i));
        if (c && mem::Q(c) == A(kMuleDmgVt)) return c;
    }
    return 0;
}

// Reads a DamageInfo and its context into a capture (fault-free).
void ReadHit(std::uintptr_t di, Cap& c) noexcept {
    std::uint8_t d[0xA0] = {};
    if (!mem::Read(di, d, sizeof(d))) return;
    std::memcpy(c.impulse, d + 0x10, 12);
    std::memcpy(&c.a20, d + 0x20, 4);
    std::uintptr_t holder = 0, node = 0;
    std::memcpy(&holder, d + 0x28, 8);
    std::memcpy(&node, d + 0x40, 8);
    std::memcpy(&c.mult, d + 0x48, 4);
    std::memcpy(c.normal, d + 0x50, 12);
    std::memcpy(&c.part, d + 0x68, 4);
    std::memcpy(&c.amount, d + 0x78, 4);
    std::memcpy(c.point, d + 0x80, 24);
    c.have_point = d[0x98] != 0 && std::isfinite(c.point[0]) && std::isfinite(c.point[1]) && std::isfinite(c.point[2]);
    const std::uintptr_t cref = holder ? mem::Q(holder + 0x18) : 0;
    const std::uintptr_t ctx = cref ? cref - 0x20 : 0;
    const std::uintptr_t aref = node ? mem::Q(node + 0x68) : 0;
    c.attacker = aref ? aref - 0x20 : 0;
    std::uint8_t p[0x70] = {};
    if (ctx && mem::Read(ctx + 0x120, p, sizeof(p))) {
        std::memcpy(&c.serial, p + 0x08, 4);
        std::memcpy(&c.attack, p + 0x0C, 2);
        c.b12E = p[0x0E];
        std::memcpy(&c.v130, p + 0x10, 4);
        std::uintptr_t res = 0;
        std::memcpy(&res, p + 0x18, 8);
        std::memcpy(&c.attacker_id, p + 0x20, 4);
        c.b180 = p[0x60];
        std::memcpy(&c.v184, p + 0x64, 4);
        std::memcpy(&c.v188, p + 0x68, 4);
        c.lethal = res && F32(res + 0x24) > 0.0f;
        // the type as the game checks it, or (its slow path) the attack's own registry entry in P+0x18
        c.ctx_ok = CtxTypeOk(ctx) || (erules::AttackOk(c.attack) && res && U16(res + 0x20) == c.attack);
    }
    for (int k = 0; k < 3; ++k) {
        if (!std::isfinite(c.normal[k])) c.normal[k] = 0;
        if (!std::isfinite(c.impulse[k])) c.impulse[k] = 0;
    }
    if (!std::isfinite(c.amount)) c.amount = 0;
    if (!std::isfinite(c.mult)) c.mult = 1;
    if (!std::isfinite(c.a20)) c.a20 = 1;
}

void LogWatch(const char* where, std::uintptr_t entity, const Cap& c, int depth, std::uintptr_t receiver) noexcept {
    if (g_watch_logged.fetch_add(1, std::memory_order_relaxed) >= (automation::Enabled() ? 200u : 20u)) return;
    const std::uintptr_t comp = DamageComponent(entity);
    const std::uintptr_t sam = g_sam.load(std::memory_order_relaxed);
    const std::uint32_t pid = g_player_id.load(std::memory_order_relaxed);
    const char* an = c.attacker ? mem::RttiName(mem::RttiOf(c.attacker)) : nullptr;
    const char* en = mem::RttiName(mem::RttiOf(entity));
    const std::uintptr_t vt10 = receiver ? mem::Q(mem::Q(receiver) + 0x10) : 0;
    LogLater("[enemy] onhit watch (%s): id %05X attack 0x%X kind %s serial 0x%X part %u amount %.1f mult %.2f a20 %.2f attacker %p %s (our Sam %d, his %d) "
             "P+0x20 0x%X (our id 0x%X: %s) b12E %u v130 %u b180 %u power %.3f v188 %u lethal %d ctx %s; receiver %s vt10 %s depth %d; point (%.2f, %.2f, %.2f) "
             "valid %d; its health %.1f stamina %.1f",
             where, g_watch_id.load(), c.attack, t_bullet ? "bullet" : "job", c.serial, c.part, static_cast<double>(c.amount), static_cast<double>(c.mult),
             static_cast<double>(c.a20), reinterpret_cast<void*>(c.attacker), an ? an : "?", sam && NearKey(c.attacker, sam) ? 1 : 0,
             OwnedBySam(c.attacker, sam) || PartOfSam(c.attacker, sam) ? 1 : 0, c.attacker_id, pid, g_player_ok.load() && pid == c.attacker_id ? "equal" : "differ", c.b12E,
             c.v130, c.b180, [&] {
                 float f = 0;
                 std::memcpy(&f, &c.v184, 4);
                 return static_cast<double>(f);
             }(),
             c.v188, c.lethal ? 1 : 0, c.ctx_ok ? "ok" : "UNCHECKED", en ? en : "?", !receiver ? "-" : vt10 == A(kOnHit) ? "OnHit" : "other", depth, c.point[0],
             c.point[1], c.point[2], c.have_point ? 1 : 0, comp ? static_cast<double>(F32(comp + kCompHealth)) : -1.0,
             comp ? static_cast<double>(F32(comp + kCompStamina)) : -1.0);
}

// A hit on an entity (OnHit: the receiver; the MULE handler: its entity). True: a mirror's (dropped).
bool HitBody(std::uintptr_t receiver, std::uintptr_t di, std::uintptr_t entity, std::uint8_t kind) noexcept {
    const std::uintptr_t e = entity ? entity : ReceiverEntity(receiver);
    if (!e) return false;
    std::uint32_t id = 0;
    int depth = -1, wdepth = -1;
    const std::uintptr_t w = g_watch.load(std::memory_order_relaxed);
    std::uintptr_t p = e;
    for (int d = 0; p && d < 5; ++d, p = mem::Q(p + kEParent)) {
        if (depth < 0 && esync::MirrorIdOf(p, &id)) depth = d;
        if (wdepth < 0 && w && NearKey(p, w)) wdepth = d;
        if (depth >= 0 && (wdepth >= 0 || !w)) break;
    }
    if (depth < 0 && wdepth < 0) return false;
    Cap c;
    ReadHit(di, c);
    if (wdepth >= 0 && erules::AttackOk(c.attack)) g_watch_attack.store(c.attack, std::memory_order_relaxed);
    if (wdepth >= 0) LogWatch(kind == ewire::kKindDirect ? "at the MULE handler" : "at OnHit", w, c, wdepth, receiver);
    if (depth < 0) return false;  // watched only: the hit passes
    // a frozen mirror: never the game's here (the host decides)
    const std::uintptr_t sam = g_sam.load(std::memory_order_relaxed);
    const std::uint32_t pid = g_player_id.load(std::memory_order_relaxed);
    const bool pid_ok = g_player_ok.load(std::memory_order_relaxed);
    if (sam && c.attacker && NearKey(c.attacker, sam)) c.flags |= ewire::kHitAttackerSam;
    else if (sam && (OwnedBySam(c.attacker, sam) || PartOfSam(c.attacker, sam))) c.flags |= ewire::kHitOurWeapon;
    else if (OurVehicle(c.attacker, sam)) c.flags |= ewire::kHitOurVehicle;
    const bool by_id = pid_ok && c.attacker_id == pid;
    const bool ours = (c.flags & (ewire::kHitAttackerSam | ewire::kHitOurWeapon | ewire::kHitOurVehicle)) != 0 || by_id;
    if (!c.ctx_ok) g_a_ctx_bad.fetch_add(1, std::memory_order_relaxed);
    if (kind == ewire::kKindDirect) {
        g_m_mirror.fetch_add(1, std::memory_order_relaxed);
        if (ours) g_m_ours.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_a_mirror.fetch_add(1, std::memory_order_relaxed);
        (ours ? g_a_ours : g_a_others).fetch_add(1, std::memory_order_relaxed);
    }
    // her aim's notices (0x180 AimTarget, about 30 a second while she aims at a mirror: run 20261124-141609) and any attack a HIT
    // cannot carry: dropped, never sent
    const bool sendable = erules::AttackOk(c.attack);
    if (ours && !sendable) g_a_noattack.fetch_add(1, std::memory_order_relaxed);
    const erules::HitV v = erules::HitVerdict(true, ours && c.ctx_ok && sendable, g_capture.load(std::memory_order_relaxed));
    if (v == erules::HitV::capture) {
        c.key = id;
        c.depth = static_cast<std::uint8_t>(depth);
        c.kind = kind ? kind : (t_bullet ? ewire::kKindBullet : ewire::kKindJob);
        if (depth > 0) c.flags |= ewire::kHitOnPart;
        if (c.lethal) c.flags |= ewire::kHitLethal;
        if (c.part != 0xFFFFFFFFu) c.flags |= ewire::kHitPartValid;
        c.entity = e;
        if (kind != ewire::kKindDirect) g_a_capt.fetch_add(1, std::memory_order_relaxed);
        if (!RingPush(c)) g_ring_full.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_ctx_logged.load(std::memory_order_relaxed) < 4 && ours && sendable && g_ctx_logged.fetch_add(1, std::memory_order_relaxed) < 4)
        LogLater("[enemy] her hit on mirror %05X (%s, depth %d): attack 0x%X serial 0x%X part %u, attacker %p (Sam %d, weapon/part %d, vehicle %d, P+0x20 our id %d), "
                 "ctx %s, %s",
                 id, kind == ewire::kKindDirect ? "the MULE handler" : t_bullet ? "a bullet at OnHit" : "OnHit", depth, c.attack, c.serial, c.part,
                 reinterpret_cast<void*>(c.attacker), (c.flags & ewire::kHitAttackerSam) ? 1 : 0, (c.flags & ewire::kHitOurWeapon) ? 1 : 0,
                 (c.flags & ewire::kHitOurVehicle) ? 1 : 0, by_id ? 1 : 0, c.ctx_ok ? "ok" : "unreadable (not sent)",
                 v == erules::HitV::capture ? "captured (a HIT)" : "dropped (hits off)");
    return true;
}
int HitGuarded(std::uintptr_t receiver, std::uintptr_t di, std::uintptr_t entity, std::uint8_t kind) noexcept {
    __try {
        return HitBody(receiver, di, entity, kind) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
void DetourFault(const char* where) noexcept {
    if (g_faults.fetch_add(1, std::memory_order_relaxed) == 0) {
        g_stop.store(true);
        g_capture.store(false);
        LogLater("[enemy] hits FAULT in %s: hits stopped (the detours pass through, nothing applied)", where);
    }
}
void H_OnHit(std::uintptr_t receiver, std::uintptr_t di) {
    if (!t_applying && !g_stop.load(std::memory_order_relaxed) && (esync::AnyFrozen() || g_watch.load(std::memory_order_relaxed))) {
        const int r = HitGuarded(receiver, di, 0, 0);
        if (r > 0) return;  // a mirror's: dropped (captured when hers)
        if (r < 0) DetourFault("the OnHit detour");
    }
    o_onhit(receiver, di);
}
void H_MuleDmg(std::uintptr_t component, std::uintptr_t msg) {
    if (!g_stop.load(std::memory_order_relaxed) && (esync::AnyFrozen() || g_watch.load(std::memory_order_relaxed))) {
        const int r = HitGuarded(0, msg + 0x20, mem::Q(component + 0x48), ewire::kKindDirect);
        if (r > 0) return;
        if (r < 0) DetourFault("the MULE handler detour");
    }
    o_muledmg(component, msg);
}

// ---------------------------------------------------------------- the game thread
Config g_cfg;
bool g_want = false, g_installed = false;
unsigned g_groups = 0, g_checks_n = 0, g_checks_ok = 0;
double g_now = 0;
std::uint64_t g_ticks = 0;
int g_role = 0;
bool g_hits_on = false, g_logged_state = false;
std::uint32_t g_world = 0xFFFFFFFFu, g_gen = 0xFFFFFFFFu;
bool g_world_ready = false;
bool g_resync_due = false;
std::uint32_t g_credit_id = 0, g_reg_id = 0;
bool g_ids_done = false, g_credit_ok = false;
std::uint32_t g_last_hit_id = 0;
std::uint32_t g_scb_seq = 0, g_hit_seq = 0;
game::Entity* g_body = nullptr;
// the mode of enemy_hit (hidden runs): part (-1 none, -2 head, -3 chest, >= 0 an index), owner / path (-1 the session's)
int g_mode_part = -3, g_mode_owner = -1, g_mode_path = -1;
// the guest
erules::HitDedupe g_dedupe;
std::vector<ewire::Hit> g_out;
unsigned g_sent = 0, g_deduped = 0, g_lethal_refused = 0, g_far = 0, g_off_dropped = 0, g_bad_attack = 0, g_unready = 0, g_res_rx = 0, g_oc_rx = 0;
unsigned g_bad_values = 0, g_stale_dropped = 0, g_out_capped = 0;
std::uint8_t g_seen_epoch = 0;  // the host's outcome epoch her table holds (0: none yet)
constexpr std::size_t kOutCap = 64;          // her HITs waiting for the link (the oldest dropped beyond it)
constexpr std::uint32_t kHitMaxAgeMs = 1000;  // a HIT older than this when the link takes it is dropped (never a late burst)
unsigned g_res_by[erules::kHitResults] = {};
bool g_lethal_notice = false;
struct GuestRec {
    erules::OutcomeRec rec;
    std::uint8_t state = 0, downbits = 0, cause = 0;
    float health = 0, stamina = 0;
    std::uint32_t last_seq = 0;
    bool sen_dead = false;
};
std::map<std::uint32_t, GuestRec> g_gout;
// the host
erules::RateLimit g_rate;
std::vector<ewire::HitRes> g_res_out;
std::vector<ewire::Outcome> g_oc_out;
std::uint8_t g_oc_epoch = 0;   // the host's outcome epoch (1..255, a new one each ResetWorld: its versions start again)
double g_oc_full_next = 0;     // the host's full outcome read (10 Hz; every tick only the enemies she hit in the last 3 s)
bool g_relay_on = false;
unsigned g_relay_in = 0, g_relay_applied = 0, g_relay_not_owned = 0;
unsigned g_rx = 0, g_applied = 0, g_refused_by[erules::kHitResults] = {}, g_wrong_role = 0, g_bad_msg = 0;
struct HostRec {
    bool have = false;
    std::uint8_t defeat = 0, state = 0;
    std::uint16_t ver = 0;
    std::uint32_t ea4 = 0, last_seq = 0;
    float health = 0, stamina = 0;
    double last_hit_ms = -1e18;
    std::uintptr_t actor = 0;
    std::uint32_t r_seq = 0;      // round 23: the newest restrain HIT (kind 5) applied to it (its cause text when last_seq is it)
    std::uint16_t r_attack = 0;
    std::uint8_t r_k = 0, r_src = 0;
};
std::map<std::uint32_t, HostRec> g_hout;
const char* RestrainWhat(std::uint16_t attack, std::uint8_t k, std::uint8_t src) noexcept;  // round 23 (below)
// ---- round 23: the tie-up relay (docs/enemy-sync.md 14; erules::RestrainWatch / RestrainGate)
// the acting side: a watch per copy (opened by a strand / bola HIT of ours, which is held back, or by the scan seeing the copy go
// down); our sent restrains waiting for the owner; the owner: its setter calls and readbacks; the guest's HITRES for the host's.
struct RWatch {
    double t0 = 0;
    std::uint8_t copy_was = 0;
    bool have_held = false, fallback = false, test = false;
    erules::RestrainTrig trig = erules::RestrainTrig::none;
    ewire::Hit held;
    unsigned extra = 0;  // further strand / bola HITs on it while open (held back too)
};
std::map<std::uint32_t, RWatch> g_rwatch;
struct RSent {
    std::uint32_t id = 0, seq = 0;
    double t = 0;
    std::uint8_t k = 0, src = 0;
    bool answered = false, test = false;
};
std::vector<RSent> g_rsent;
struct RBack {
    std::uint32_t id = 0, seq = 0;
    std::uintptr_t actor = 0;
    double t = 0;
    std::uint8_t k = 0, src = 0, was = 0;
    std::uint16_t attack = 0;
    bool set = false, relay = false;
};
std::vector<RBack> g_rback;
std::vector<ewire::HitRes> g_rres_out;              // the guest: HITRES for the host's relayed restrains
std::map<std::uint32_t, std::uint8_t> g_scan_was;   // the acting side's scan: each frozen copy's defeat at the last read
double g_scan_next = 0;
bool g_restrain_send = false;                       // ours on, both beacons carry kPartRestrain (the guest; the host while it relays)
bool g_restrain_logged = false;
bool g_restrain_test_next = false;                  // enemy_restrain 1: the next capture's watch is a test (the fallback's evidence waived)
unsigned g_r_watches = 0, g_r_sent = 0, g_r_native = 0, g_r_fallback = 0, g_r_released = 0, g_r_closed = 0, g_r_swallowed = 0, g_r_rx = 0,
         g_r_applied = 0, g_r_refused = 0, g_r_answers = 0, g_r_answer_refused = 0, g_r_noanswer = 0;
constexpr double kRestrainScanM = 30.0;  // the scan's reach around our Sam (a bola flies)
struct Effect {
    std::uint32_t id = 0, seq = 0;
    std::uintptr_t actor = 0, comp = 0;
    float before[4] = {};
    std::uint32_t st0 = 0, ea4_0 = 0;
    std::uint64_t due = 0;
    bool late = false;
};
std::vector<Effect> g_effects;
// enemy_hit's queue (hidden runs)
struct TestRun {
    std::uint16_t attack = 0;
    int left = 0;
    std::uint32_t id = 0;
    double next = 0;
    bool send = false;  // enemy_hit_send (the guest's HIT) instead of a local apply
} g_test;
// the actor cache (id -> actor; the registry walk at most every 500 ms)
std::vector<std::pair<std::uint32_t, std::uintptr_t>> g_actors;
double g_actors_at = -1e18;

unsigned LogCap() noexcept { return automation::Enabled() ? 1000u : 40u; }
bool LogNth(unsigned n) noexcept { return n <= LogCap() || n % 20 == 0; }

void WalkActors() noexcept {
    g_actors.clear();
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
            g_actors.emplace_back(id, o);
        }
    }
}
bool SameMule(std::uintptr_t actor, std::uint32_t id) noexcept {
    std::uint8_t in_use = 0;
    return actor && mem::Q(actor) == A(kMuleVt) && mem::Rd(actor + kActInUse, in_use) && in_use && U32(actor + kActId) == id;
}
std::uintptr_t ActorOf(std::uint32_t id) noexcept {
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& a : g_actors)
            if (a.first == id && SameMule(a.second, id)) return a.second;
        if (pass == 0 && g_now - g_actors_at > 200) WalkActors();
        else break;
    }
    return 0;
}
struct MuleNow {
    std::uintptr_t actor = 0, entity = 0, comp = 0;
    std::uint32_t state = 0, a88 = 0, ea0 = 0, ea4 = 0;
    bool realized = false, have_xf = false;
    WorldTransform xf{};
    float vals[4] = {};  // health, stamina, sleep, restraint
};
bool ReadMule(std::uintptr_t actor, std::uint32_t id, MuleNow& m) noexcept {
    if (!SameMule(actor, id)) return false;
    m.actor = actor;
    m.state = U32(actor + kActState);
    const std::uintptr_t ref = mem::Q(actor + kActEntRef);
    m.entity = ref ? ref - 0x20 : 0;
    m.realized = m.state == 2 && m.entity;
    m.a88 = U32(actor + kActFlags88);
    m.ea0 = U32(actor + kActMule);
    m.ea4 = U32(actor + kActDown);
    if (m.realized) {
        m.have_xf = ReadXf(m.entity, m.xf);
        m.comp = CachedComponent(m.entity);
        if (m.comp) {
            m.vals[0] = F32(m.comp + kCompHealth), m.vals[1] = F32(m.comp + kCompStamina), m.vals[2] = F32(m.comp + kCompSleep);
            m.vals[3] = F32(m.comp + kCompRestraint);
        }
    }
    return true;
}
// The registry's local player actor id (vt+0x40, as the MULE processing reads it). False: not read.
bool RegistryPlayerId(std::uint32_t* out) noexcept {
    const std::uintptr_t reg = mem::Q(A(kRegGlobal));
    const std::uintptr_t fn = reg ? mem::Q(mem::Q(reg) + 0x40) : 0;
    if (!fn || !mem::InText(fn)) return false;
    alignas(16) std::uint8_t buf[64] = {};
    const std::uint32_t* p = nullptr;
    __try {
        p = reinterpret_cast<FnPlayerId>(fn)(reg, buf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return p && mem::Rd(reinterpret_cast<std::uintptr_t>(p), *out);
}

// ---------------------------------------------------------------- the apply (game thread, SEH, POD only)
struct ApplyArgs {
    std::uintptr_t recv = 0, entity = 0, owner = 0, filter = 0;
    std::uint16_t attack = 0;
    std::uint8_t b12E = 0, b180 = 0;
    std::uint32_t serial = 0, v130 = 0, v184 = 0, v188 = 0, credit_id = 0;
    bool stamp_credit = false;
    float amount = 0, mult = 1, a20 = 1;
    std::uint32_t part = 0xFFFFFFFFu;
    double point[3] = {};
    float normal[4] = {0, 0, 1, 0};
    float impulse[4] = {};
    int path = 0;
};
struct ApplyOut {
    std::uintptr_t ctx = 0, res = 0;
    std::uint16_t p0c = 0;
    std::uint32_t p20_before = 0, p20_after = 0, serial_before = 0;
    bool res_stamped = false;
    int step = 0;
    std::uint32_t q_part = 0xFFFFFFFFu;
    unsigned q_seen = 0, q_kept = 0;
    double q_hit[3] = {};
};
struct QueryUser {
    std::uintptr_t recv = 0, entity = 0;
    float mult = 1;
    unsigned seen = 0, kept = 0;
    std::uint32_t part = 0xFFFFFFFFu;
    double hit[3] = {};
};
char QueryCb(std::uintptr_t /*ctx*/, std::uintptr_t receiver, std::uintptr_t di, std::uintptr_t user) {
    auto* u = reinterpret_cast<QueryUser*>(user);
    ++u->seen;
    if (!(receiver == u->recv || receiver == u->entity || ReceiverEntity(receiver) == u->entity)) return 0;
    ++u->kept;
    std::uint32_t part = 0xFFFFFFFFu;
    mem::Rd(di + 0x68, part);
    u->part = part;
    mem::Read(di + 0x80, u->hit, sizeof(u->hit));
    *reinterpret_cast<float*>(di + 0x48) = u->mult;
    return 1;
}
// 1 delivered, 0 refused (out.step names the step), -1 a fault.
int ApplyCall(const ApplyArgs& a, ApplyOut& o) noexcept {
    __try {
        o.step = 1;
        const std::uintptr_t ctx = reinterpret_cast<FnCtxCreate>(A(kCtxCreate))(a.owner, a.attack);
        o.ctx = ctx;
        if (!ctx) return 0;
        o.step = 2;
        const std::uintptr_t P = reinterpret_cast<FnCtxParams>(A(kCtxParams))(ctx);
        if (P != ctx + 0x120) return 0;
        o.p0c = *reinterpret_cast<volatile std::uint16_t*>(P + 0x0C);
        if (o.p0c != a.attack) *reinterpret_cast<volatile std::uint16_t*>(P + 0x0C) = a.attack;  // as BulletDamageA stamps +0x12C
        o.step = 3;
        o.res = *reinterpret_cast<volatile std::uintptr_t*>(P + 0x18);
        if (!o.res || U16(o.res + 0x20) != a.attack) {
            const std::uintptr_t e = AttackEntry(a.attack);
            if (!e) return 0;
            *reinterpret_cast<volatile std::uintptr_t*>(P + 0x18) = e;
            o.res = e, o.res_stamped = true;
        }
        o.serial_before = *reinterpret_cast<volatile std::uint32_t*>(P + 0x08);
        *reinterpret_cast<volatile std::uint32_t*>(P + 0x08) = a.serial;
        *reinterpret_cast<volatile std::uint8_t*>(P + 0x0E) = a.b12E;
        *reinterpret_cast<volatile std::uint32_t*>(P + 0x10) = a.v130;
        *reinterpret_cast<volatile std::uint8_t*>(P + 0x60) = a.b180;
        *reinterpret_cast<volatile std::uint32_t*>(P + 0x64) = a.v184;
        *reinterpret_cast<volatile std::uint32_t*>(P + 0x68) = a.v188;
        o.p20_before = *reinterpret_cast<volatile std::uint32_t*>(P + 0x20);
        if (a.stamp_credit) *reinterpret_cast<volatile std::uint32_t*>(P + 0x20) = a.credit_id;
        o.p20_after = *reinterpret_cast<volatile std::uint32_t*>(P + 0x20);
        o.step = 4;
        if (a.path == 0) {
            alignas(16) std::uint8_t di[0xB0];
            std::memset(di, 0, sizeof(di));
            alignas(16) float imp[4] = {a.impulse[0], a.impulse[1], a.impulse[2], 0};
            reinterpret_cast<FnDIInit>(A(kDIInit))(di, *reinterpret_cast<volatile std::uintptr_t*>(ctx + 0xE8), reinterpret_cast<const void*>(A(kG16)), a.amount, imp,
                                                   a.a20, -1.0f, a.part);
            std::memcpy(di + 0x80, a.point, 24);
            di[0x98] = 1;
            std::memcpy(di + 0x50, a.normal, 16);
            std::memcpy(di + 0x48, &a.mult, 4);
            std::memset(di + 0x60, 0, 8);
            o.step = 5;
            const std::uintptr_t vt10 = *reinterpret_cast<volatile std::uintptr_t*>(*reinterpret_cast<volatile std::uintptr_t*>(a.recv) + 0x10);
            t_applying = true;
            reinterpret_cast<void (*)(std::uintptr_t, void*)>(vt10)(a.recv, di);
            t_applying = false;
            o.step = 6;
            reinterpret_cast<FnRelease>(A(kRelease))(di + 0x28);
        } else {
            QueryUser u;
            u.recv = a.recv, u.entity = a.entity, u.mult = a.mult;
            o.step = 7;
            const int r = vhit::QueryHit(a.filter, ctx, a.point, 0.25f, a.amount, &QueryCb, reinterpret_cast<std::uintptr_t>(&u));
            o.q_seen = u.seen, o.q_kept = u.kept, o.q_part = u.part;
            std::memcpy(o.q_hit, u.hit, sizeof(o.q_hit));
            if (r < 0) return -1;
            if (r == 0) return 0;
            o.step = 8;
            if (u.kept == 0) return 0;  // the sphere held other receivers only: nothing reached this MULE
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        t_applying = false;
        return -1;
    }
}
const char* StepText(int step, int path) noexcept {
    switch (step) {
        case 1: return "no context";
        case 2: return "not an attack context";
        case 3: return "no attack resource";
        case 4: return "the DamageInfo init";
        case 5: return "the receiver call";
        case 6: return "the release";
        case 7: return path ? "the query (its byte checks)" : "?";
        case 8: return "the query kept nothing (no part of this MULE in the sphere)";
    }
    return "?";
}

void HitsFault(const char* step) {
    if (g_stop.exchange(true)) return;
    g_capture.store(false);
    Log("[enemy] hits FAULT in %s: hits stopped (the detours pass through, nothing applied)", step);
    shots::SetHitsPart(false, "stopped after a fault)");
}

// The owner entity of the host's context by mode (0 credit, 1 body, 2 none, 3 sam).
std::uintptr_t OwnerFor(int mode, bool* fell_back) noexcept {
    *fell_back = false;
    if (mode == 2) return 0;
    if (mode == 3) return g_sam.load();
    game::Entity* b = g_body ? g_body : realbody::LiveEntity();
    if (!b) *fell_back = true;
    return reinterpret_cast<std::uintptr_t>(b);
}
// The player's attacker id for the credit mode: one Sam-owned context's P+0x20, once a world (also logged against the registry's).
void CreditIds() noexcept {
    if (g_ids_done) return;
    g_ids_done = true;
    g_reg_id = 0;
    const bool reg_ok = RegistryPlayerId(&g_reg_id);
    g_player_id.store(g_reg_id);
    g_player_ok.store(reg_ok);
    const std::uintptr_t sam = g_sam.load();
    if (!sam || !(g_groups & kGApply) || g_role == 2) return;  // the guest makes no context in her world (her id is the registry's)
    std::uintptr_t ctx = 0, P = 0;
    __try {
        ctx = reinterpret_cast<FnCtxCreate>(A(kCtxCreate))(sam, 0x47);
        P = ctx ? reinterpret_cast<FnCtxParams>(A(kCtxParams))(ctx) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ctx = 0, P = 0;
    }
    g_credit_ok = ctx && P == ctx + 0x120;
    if (g_credit_ok) g_credit_id = U32(P + 0x20);
    else g_credit_id = g_reg_id, g_credit_ok = reg_ok;
    Log("[enemy] hits: the player's attacker id 0x%X (a context owned by our Sam: P+0x20%s), the registry's player id 0x%X%s: %s", g_credit_id,
        ctx && P == ctx + 0x120 ? "" : ": no context, the registry's", g_reg_id, reg_ok ? "" : " (not read)",
        reg_ok && g_credit_id == g_reg_id ? "equal (the credit mode works)" : "DIFFER (the credit mode stamps the context's)");
}

struct ApplyResult {
    erules::HitResult r = erules::HitResult::off;
    ApplyOut out;
    MuleNow m;
    int owner_mode = 0;
    bool owner_fell_back = false;
    double world_pt[3] = {};
};
// Applies one hit to this game's copy of id (the host's HIT, or enemy_hit's test). `local`: the point in its yaw frame.
void ApplyOne(std::uint32_t id, const ewire::Hit& h, const double (&local)[3], const double (&normal_l)[3], const double (&impulse_l)[3], int owner_mode,
              int path, std::uint32_t host_serial, ApplyResult& res) {
    res.owner_mode = owner_mode;
    const std::uintptr_t actor = ActorOf(id);
    if (!actor || !ReadMule(actor, id, res.m) || !res.m.realized || !res.m.have_xf) {
        res.r = erules::HitResult::unrealized;
        return;
    }
    const double root[3] = {res.m.xf.pos.x, res.m.xf.pos.y, res.m.xf.pos.z};
    const double yaw = YawOf(res.m.xf);
    erules::ToWorld(root, yaw, local, res.world_pt);
    double nw[3], iw[3];
    erules::DirToWorld(yaw, normal_l, nw);
    erules::DirToWorld(yaw, impulse_l, iw);
    const std::uintptr_t recv = res.m.entity + kReceiverOfEntity;
    std::uint8_t ent = 0;
    if (!mem::Rd(recv + 8, ent) || !ent || mem::Q(mem::Q(recv) + 0x10) != A(kOnHit)) {
        res.r = erules::HitResult::receiver;
        static int logged = 0;
        if (logged++ < 4) {
            const char* n = mem::RttiName(mem::RttiOf(res.m.entity));
            Log("[enemy] HIT refused id=%05X: its receiver is not an entity receiver with OnHit (%s, byte %u, vt+0x10 %p)", id, n ? n : "?", ent,
                reinterpret_cast<void*>(mem::Q(mem::Q(recv) + 0x10)));
        }
        return;
    }
    ApplyArgs a;
    a.recv = recv, a.entity = res.m.entity;
    a.owner = OwnerFor(owner_mode, &res.owner_fell_back);
    a.filter = a.owner;
    a.attack = h.attack, a.b12E = h.b12E, a.b180 = h.b180;
    a.serial = host_serial, a.v130 = h.v130, a.v184 = h.v184, a.v188 = h.v188;
    if (owner_mode == 0 && g_credit_ok) a.credit_id = g_credit_id, a.stamp_credit = true;
    a.amount = h.amount, a.mult = h.mult, a.a20 = h.a20, a.part = h.part;
    std::memcpy(a.point, res.world_pt, sizeof(a.point));
    const bool nvalid = (h.flags & ewire::kHitNormalValid) != 0;
    for (int k = 0; k < 3; ++k) a.normal[k] = nvalid ? static_cast<float>(nw[k]) : (k == 2 ? 1.0f : 0.0f), a.impulse[k] = static_cast<float>(iw[k]);
    a.normal[3] = 0;
    a.path = path;
    const int r = ApplyCall(a, res.out);
    if (r < 0) {
        res.r = erules::HitResult::fault;
        HitsFault(path ? "the apply (query)" : "the apply (direct)");
        return;
    }
    if (r == 0) {
        res.r = erules::HitResult::fault;
        Log("[enemy] HIT not delivered id=%05X: %s (ctx %p)", id, StepText(res.out.step, path), reinterpret_cast<void*>(res.out.ctx));
        return;
    }
    res.r = erules::HitResult::applied;
    Effect e;
    e.id = id, e.seq = h.hit_seq, e.actor = actor, e.comp = res.m.comp;
    std::memcpy(e.before, res.m.vals, sizeof(e.before));
    e.st0 = res.m.ea0, e.ea4_0 = res.m.ea4;
    e.due = g_ticks + 2;
    if (g_effects.size() < 64) g_effects.push_back(e);
    g_last_hit_id = id;
    HostRec& rec = g_hout[id];
    rec.last_hit_ms = g_now, rec.last_seq = h.hit_seq, rec.actor = actor;
}

void EffectsTick() {
    for (std::size_t i = 0; i < g_effects.size();) {
        Effect& e = g_effects[i];
        if (g_ticks < e.due) {
            ++i;
            continue;
        }
        MuleNow m;
        const bool ok = ReadMule(e.actor, e.id, m);
        const bool changed = ok && (m.vals[0] != e.before[0] || m.vals[1] != e.before[1] || m.vals[2] != e.before[2] || m.vals[3] != e.before[3] || m.ea0 != e.st0);
        if (ok && !changed && !e.late) {
            e.late = true, e.due = g_ticks + 8;  // the component's update may come a few frames after the flush
            ++i;
            continue;
        }
        static unsigned logged = 0;
        if (LogNth(++logged))
            Log("[enemy] HIT effect id=%05X (#%u): health %.1f -> %.1f, stamina %.1f -> %.1f, sleep %.1f -> %.1f, restraint %.1f -> %.1f, state %u -> %u (bits 0x%X -> "
                "0x%X)%s%s",
                e.id, e.seq, e.before[0], m.vals[0], e.before[1], m.vals[1], e.before[2], m.vals[2], e.before[3], m.vals[3], e.st0, m.ea0, e.ea4_0, m.ea4,
                e.late ? " (10 ticks)" : " (2 ticks)", ok ? "" : " (the MULE is gone)");
        g_effects.erase(g_effects.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

// ---------------------------------------------------------------- the host: HITs in, HITRES and OUTCOMEs out
// HITRES out (the host's for her HITs; round 23: the guest's for the host's relayed restrains)
void SendRes(std::vector<ewire::HitRes>& out, std::uint32_t guids) {
    std::uint8_t buf[1024];
    while (!out.empty()) {
        const unsigned n = static_cast<unsigned>(out.size() < ewire::kHitResMax ? out.size() : ewire::kHitResMax);
        ewire::ScbHeader h;
        h.guids_fp = guids, h.seq = g_scb_seq + 1;
        const std::size_t size = ewire::EncodeHitRes(h, out.data(), n, buf, sizeof(buf));
        if (!size) {
            out.erase(out.begin(), out.begin() + n);
            continue;
        }
        if (!link::SendCombat(buf, size)) break;  // the window is full: the next tick
        ++g_scb_seq;
        out.erase(out.begin(), out.begin() + n);
    }
}
void SendOut(std::uint32_t guids) {
    std::uint8_t buf[1024];
    SendRes(g_res_out, guids);
    while (!g_oc_out.empty()) {
        const unsigned n = static_cast<unsigned>(g_oc_out.size() < ewire::kOutcomeMax ? g_oc_out.size() : ewire::kOutcomeMax);
        ewire::ScbHeader h;
        h.guids_fp = guids, h.seq = g_scb_seq + 1;
        const std::size_t size = ewire::EncodeOutcomes(h, g_oc_out.data(), n, buf, sizeof(buf));
        if (!size) {
            g_oc_out.erase(g_oc_out.begin(), g_oc_out.begin() + n);
            continue;
        }
        if (!link::SendCombat(buf, size)) break;
        ++g_scb_seq;
        g_oc_out.erase(g_oc_out.begin(), g_oc_out.begin() + n);
    }
}
ewire::Outcome OutcomeMsg(std::uint32_t id, const HostRec& r, std::uint8_t cause) {
    ewire::Outcome o;
    o.key = id, o.defeat = r.defeat, o.cause = cause, o.ver = r.ver, o.state = static_cast<std::uint8_t>(r.state > 255 ? 255 : r.state);
    o.downbits = erules::DownBits(r.ea4), o.phase = 0xFF;
    o.health = std::isfinite(r.health) ? r.health : 0, o.stamina = std::isfinite(r.stamina) ? r.stamina : 0;
    o.last_seq = r.last_seq;
    o.epoch = g_oc_epoch;
    return o;
}
// relay (P1c, on the guest): the host's hit on one she owns. Its own counters and log (review rv-correctness N3: the host-side
// g_rx / g_applied / g_refused_by count her HITs on the host's enemies only); no HITRES.
erules::HitResult HostHit(const ewire::Hit& h, std::uint32_t msg_guids, const esync::GateView& gv, bool relay = false) {
    if (!relay) ++g_rx;
    const std::uintptr_t actor = ActorOf(h.key);
    MuleNow m;
    const bool found = actor && ReadMule(actor, h.key, m);
    erules::ApplyIn in;
    in.host = g_role == 1 || relay;  // (relay: the guest applies the host's hit on one she owns, P1c)
    in.mirroring = gv.mirroring;
    in.hits_on = g_hits_on && !g_stop.load();
    in.fp_equal = msg_guids == gv.guids && gv.guids != 0;
    in.attack_ok = erules::AttackOk(h.attack);
    in.kind_ok = h.kind != ewire::kKindTest || automation::Enabled();  // a test HIT (real damage) only in a hidden run
    in.values_ok = erules::HitValuesOk(h.amount, h.mult, h.a20, h.v184, h.part);
    // the host's own lethal policy: the HIT's flag or the host's registry entry for the attack (damage +0x24 > 0)
    if (in.attack_ok) {
        const std::uintptr_t res = AttackEntry(h.attack);
        in.lethal = (h.flags & ewire::kHitLethal) != 0 || (res && F32(res + 0x24) > 0.0f);
    }
    in.lethal_allowed = g_cfg.enemy_hits_lethal;
    in.found = found;
    in.realized = found && m.realized && m.have_xf;
    in.state = found ? m.ea0 : 0;
    in.hidden_or_asleep = found && (m.a88 & 3u) != 0;
    in.held = found && ((m.ea4 >> 14) & 1u) != 0;
    const double local[3] = {h.point[0], h.point[1], h.point[2]};
    in.point_m = std::sqrt(local[0] * local[0] + local[1] * local[1] + local[2] * local[2]);
    // the rate last (only a HIT that would be applied counts against it)
    erules::HitResult r = erules::ApplyGate(in);
    if (r == erules::HitResult::applied && !g_rate.Allow(h.key, g_now, static_cast<unsigned>(g_cfg.enemy_hit_rate), 60)) r = erules::HitResult::rate;
    ApplyResult ar;
    const std::uint32_t hs = erules::HostSerial(h.serial, h.hit_seq);
    if (r == erules::HitResult::applied) {
        const double nl[3] = {h.normal[0], h.normal[1], h.normal[2]}, il[3] = {h.impulse[0], h.impulse[1], h.impulse[2]};
        ApplyOne(h.key, h, local, nl, il, g_cfg.enemy_hit_owner, g_cfg.enemy_hit_path, hs, ar);
        r = ar.r;
    }
    if (relay) return r;
    if (r == erules::HitResult::applied) {
        ++g_applied;
        if (LogNth(g_applied))
            Log("[enemy] HIT applied #%u id=%05X attack 0x%X kind %u part %d owner %s%s (ctx %p, serial 0x%08X, attacker id 0x%X -> 0x%X%s), local (%.2f, %.2f, %.2f) "
                "world (%.2f, %.2f, %.2f), %s delivered",
                h.hit_seq, h.key, h.attack, h.kind, static_cast<int>(h.part), cfgval::HitOwnerName(g_cfg.enemy_hit_owner), ar.owner_fell_back ? " (no body: owner 0)" : "",
                reinterpret_cast<void*>(ar.out.ctx), hs, ar.out.p20_before, ar.out.p20_after, ar.out.res_stamped ? ", resource stamped" : "", local[0], local[1],
                local[2], ar.world_pt[0], ar.world_pt[1], ar.world_pt[2], g_cfg.enemy_hit_path ? "query" : "direct");
    } else {
        ++g_refused_by[static_cast<unsigned>(r) < erules::kHitResults ? static_cast<unsigned>(r) : static_cast<unsigned>(erules::HitResult::fault)];
        if (LogNth(g_rx - g_applied))
            Log("[enemy] HIT refused #%u id=%05X attack 0x%X kind %u: %s (state %u, realized %d, a88 0x%X, ea4 0x%X, point %.2f m; amount %.2f mult %.2f a20 %.2f "
                "part %d; lethal %d, enemy_hits_lethal %d)",
                h.hit_seq, h.key, h.attack, h.kind, erules::HitResultText(static_cast<std::uint8_t>(r)), in.state, in.realized ? 1 : 0, found ? m.a88 : 0,
                found ? m.ea4 : 0, in.point_m, static_cast<double>(h.amount), static_cast<double>(h.mult), static_cast<double>(h.a20), static_cast<int>(h.part),
                in.lethal ? 1 : 0, g_cfg.enemy_hits_lethal ? 1 : 0);
    }
    ewire::HitRes hr;
    hr.key = h.key, hr.hit_seq = h.hit_seq, hr.result = static_cast<std::uint8_t>(r), hr.owner = static_cast<std::uint8_t>(g_cfg.enemy_hit_owner);
    hr.defeat = static_cast<std::uint8_t>(found ? erules::OutcomeOf(m.ea0, m.ea4) : erules::Defeat::none);
    g_res_out.push_back(hr);
    return r;
}
// The outcome reader: the 10 Hz class and every enemy hit in the last 30 s.
// The full read at 10 Hz; between, only the enemies she hit in the last 3 s (their outcome is the one she waits for).
void HostOutcomes(bool send) {
    std::uint32_t ids[64];
    std::uintptr_t actors[64];
    const bool full = g_now >= g_oc_full_next;
    if (full) g_oc_full_next = g_now + 100;
    unsigned n = full ? esync::NearEnemies(ids, actors, 64) : 0;
    for (auto& kv : g_hout)
        if (n < 64 && g_now - kv.second.last_hit_ms <= (full ? 30000 : 3000)) {
            bool dup = false;
            for (unsigned i = 0; i < n && !dup; ++i) dup = ids[i] == kv.first;
            if (!dup) ids[n] = kv.first, actors[n] = kv.second.actor ? kv.second.actor : ActorOf(kv.first), ++n;
        }
    for (unsigned i = 0; i < n; ++i) {
        MuleNow m;
        if (!ReadMule(actors[i], ids[i], m)) continue;
        const std::uint8_t d = static_cast<std::uint8_t>(erules::OutcomeOf(m.ea0, m.ea4));
        HostRec& r = g_hout[ids[i]];
        r.actor = actors[i];
        const bool first = !r.have;
        const std::uint8_t was = r.defeat;
        if (m.comp) r.health = m.vals[0], r.stamina = m.vals[1];
        r.state = static_cast<std::uint8_t>(m.ea0 > 255 ? 255 : m.ea0);
        r.ea4 = m.ea4;
        if (!first && d == was) continue;
        r.have = true;
        if (first && d == 0) continue;
        r.defeat = d;
        ++r.ver;
        const bool guest = g_now - r.last_hit_ms <= 3000;
        // round 23: her newest HIT a restrain (kind 5): its kind in the cause
        const char* rwhat = guest && r.r_seq && r.r_seq == r.last_seq ? RestrainWhat(r.r_attack, r.r_k, r.r_src) : nullptr;
        const std::uint8_t cause = static_cast<std::uint8_t>(!first && was == static_cast<std::uint8_t>(erules::Defeat::dead) ? erules::Cause::revive
                                                             : guest                                                            ? erules::Cause::guest
                                                                                                                                : erules::Cause::world);
        static unsigned logged = 0;
        if (LogNth(++logged) || guest)
            Log("[enemy] OUTCOME id=%05X %s -> %s (state %u, bits 0x%X; cause %s%s) ver %u; health %.1f stamina %.1f", ids[i], first ? "none" : erules::DefeatText(was),
                erules::DefeatText(d), m.ea0, m.ea4, erules::CauseText(cause),
                guest ? (" HIT #" + std::to_string(r.last_seq) + (rwhat ? std::string(", ") + rwhat : std::string())).c_str() : "", r.ver, r.health, r.stamina);
        if (send) g_oc_out.push_back(OutcomeMsg(ids[i], r, cause));
    }
}

// ---------------------------------------------------------------- round 23: the tie-up relay (docs/enemy-sync.md 14)
const char* RestrainSrcText(std::uint8_t src) noexcept {
    return src == ewire::kRestrainNative ? "native: the copy's +0xEA4" : src == ewire::kRestrainFallback ? "fallback: weapon 41 + attack 0x15C at contact"
           : src == ewire::kRestrainTest                                ? "test (enemy_restrain)"
                                                                        : "?";
}
const char* RestrainWhat(std::uint16_t attack, std::uint8_t k, std::uint8_t src) noexcept {
    if (k == erules::kFaintK) return src == ewire::kRestrainTest ? "knockout (test)" : "knockout";
    if (attack == erules::kAttackBola) return src == ewire::kRestrainTest ? "bola (test)" : "bola";
    return src == ewire::kRestrainFallback ? "strand (fallback)" : src == ewire::kRestrainTest ? "strand (test)" : "strand";
}
// Our copy's outcome now (erules::Defeat; 0 when unreadable or not the same MULE).
std::uint8_t CopyDefeat(std::uintptr_t actor, std::uint32_t id) {
    MuleNow m;
    return actor && ReadMule(actor, id, m) ? static_cast<std::uint8_t>(erules::OutcomeOf(m.ea0, m.ea4)) : 0;
}
// The owner's outcome as known here: the guest by the host's OUTCOMEs, the host by her SEN1 defeat bits.
std::uint8_t OwnerDefeat(std::uint32_t id) { return g_role == 2 ? GuestDefeat(id) : esync::PeerDefeat(id); }
bool RestrainPending(std::uint32_t id) {
    for (const RSent& s : g_rsent)
        if (s.id == id) return true;
    return false;
}
// A strand / bola HIT of ours on a copy: held back while the copy is watched. True: held (not sent now).
bool RestrainOpen(const ewire::Hit& h, erules::RestrainTrig trig, bool test) {
    std::uintptr_t actor = 0;
    if (!esync::MirrorOf(h.key, &actor, nullptr)) return false;
    if (RestrainPending(h.key)) {
        // a restrain of ours on it waits for the owner: the stun of this contact is not sent either
        if (LogNth(++g_r_swallowed)) Log("[enemy] restrain: HIT #%u id=%05X (%s, attack 0x%X) not sent: a restrain of ours on it waits for the owner", h.hit_seq, h.key,
                                         erules::RestrainTrigText(trig), h.attack);
        return true;
    }
    auto it = g_rwatch.find(h.key);
    if (it != g_rwatch.end()) {
        ++it->second.extra;
        if (!it->second.have_held) it->second.held = h, it->second.have_held = true, it->second.trig = trig;
        it->second.fallback = it->second.fallback || erules::RestrainFallback(h.attack, h.b12E, h.her_dist_m, test);
        return true;
    }
    RWatch w;
    w.t0 = g_now;
    w.copy_was = CopyDefeat(actor, h.key);
    w.have_held = true, w.held = h, w.trig = trig, w.test = test;
    w.fallback = erules::RestrainFallback(h.attack, h.b12E, h.her_dist_m, test);
    g_rwatch[h.key] = w;
    ++g_r_watches;
    esync::HoldCopy(h.key, erules::kRestrainHoldMs, "a strand / bola contact");
    Log("[enemy] restrain watch id=%05X opened: %s HIT #%u (attack 0x%X weapon %u, our Sam %.1f m%s) held up to %.1f s; the copy's +0xEA4 read each tick (now %s)%s",
        h.key, erules::RestrainTrigText(trig), h.hit_seq, h.attack, h.b12E, static_cast<double>(h.her_dist_m), w.fallback ? "; the fallback's evidence" : "",
        erules::kRestrainWatchMs / 1000.0, erules::DefeatText(w.copy_was), test ? " [enemy_restrain test]" : "");
    return true;
}
// One HIT of kind 5 out (the same FlushHits as every HIT).
void SendRestrain(std::uint32_t id, const RWatch& w, std::uint8_t k, std::uint8_t src) {
    ewire::Hit h;
    h.key = id, h.hit_seq = ++g_hit_seq, h.kind = ewire::kKindRestrain, h.flags = ewire::kHitAttackerSam;
    h.attack = w.have_held && erules::AttackOk(w.held.attack) ? w.held.attack : erules::kAttackStrand;
    h.serial = w.have_held ? w.held.serial : 0;
    h.b12E = w.have_held ? w.held.b12E : 0, h.b180 = k, h.depth = src;
    h.v184 = 0x3F800000u, h.part = 0xFFFFFFFFu, h.amount = 0, h.mult = 1, h.a20 = 1;
    h.t_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(g_now) & 0xFFFFFFFFull);
    h.her_dist_m = w.have_held ? w.held.her_dist_m : 0;
    if (g_out.size() >= kOutCap) g_out.erase(g_out.begin()), ++g_out_capped;
    g_out.push_back(h);
    ++g_r_sent;
    (src == ewire::kRestrainFallback ? g_r_fallback : g_r_native) += src == ewire::kRestrainTest ? 0 : 1;
    RSent s;
    s.id = id, s.seq = h.hit_seq, s.t = g_now, s.k = k, s.src = src, s.test = w.test;
    g_rsent.push_back(s);
    esync::HoldCopy(id, erules::kRestrainHoldMs, "a restrain sent");
    g_last_hit_id = id;
    Log("[enemy] restrain HIT sent #%u id=%05X k %u (%s; %s): to the %s; %s%s", h.hit_seq, id, k, RestrainWhat(h.attack, k, src), RestrainSrcText(src),
        g_role == 2 ? "host" : "guest", w.have_held ? "the held stun HIT not sent" : "no HIT of ours was held", w.test ? " [enemy_restrain test]" : "");
}
// The acting side, each tick: the watches, the scan, our sent restrains' expiry.
void RestrainActTick() {
    for (auto it = g_rwatch.begin(); it != g_rwatch.end();) {
        const std::uint32_t id = it->first;
        RWatch& w = it->second;
        std::uintptr_t actor = 0;
        const bool frozen = esync::MirrorOf(id, &actor, nullptr);
        erules::WatchIn in;
        in.age_ms = g_now - w.t0;
        in.frozen = frozen && g_restrain_send;
        in.copy = frozen ? CopyDefeat(actor, id) : 0;
        in.copy_was = w.copy_was;
        in.owner = OwnerDefeat(id);
        in.held_hit = w.have_held;
        in.fallback = w.fallback;
        const erules::WatchDo d = erules::RestrainWatch(in);
        if (d == erules::WatchDo::wait) {
            ++it;
            continue;
        }
        if (d == erules::WatchDo::send_restrain || d == erules::WatchDo::send_faint) {
            SendRestrain(id, w, d == erules::WatchDo::send_restrain ? erules::kRestrainK : erules::kFaintK, ewire::kRestrainNative);
        } else if (d == erules::WatchDo::fallback) {
            SendRestrain(id, w, erules::kRestrainK, w.test ? ewire::kRestrainTest : ewire::kRestrainFallback);
        } else if (d == erules::WatchDo::release) {
            // no tie-up seen and no fallback: the held HIT goes as before (its age from now: FlushHits' 1 s)
            ewire::Hit h = w.held;
            h.t_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(g_now) & 0xFFFFFFFFull);
            if (g_out.size() >= kOutCap) g_out.erase(g_out.begin()), ++g_out_capped;
            g_out.push_back(h);
            ++g_r_released;
            esync::ReleaseHold(id, "no tie-up seen");
            Log("[enemy] restrain watch id=%05X closed after %.1f s: %s (#%u attack 0x%X; the copy %s, the owner %s)", id, in.age_ms / 1000.0, erules::WatchDoText(d),
                h.hit_seq, h.attack, erules::DefeatText(in.copy), erules::DefeatText(in.owner));
        } else {
            ++g_r_closed;
            esync::ReleaseHold(id, !in.frozen ? "no longer a copy here" : "the owner has it down already");
            Log("[enemy] restrain watch id=%05X closed after %.1f s: %s (%s; the copy %s, the owner %s)%s", id, in.age_ms / 1000.0, erules::WatchDoText(d),
                !in.frozen ? "no longer a frozen copy here (or the relay is off)" : erules::DownDefeat(in.owner) ? "down there already" : "no tie-up seen",
                erules::DefeatText(in.copy), erules::DefeatText(in.owner), w.have_held ? "; the held HIT not sent" : "");
        }
        it = g_rwatch.erase(it);
    }
    // our sent restrains: answered by the owner's outcome (the guest: its OUTCOME, GuestOutcome; the host: her SEN1), else 3 s
    for (auto it = g_rsent.begin(); it != g_rsent.end();) {
        const std::uint8_t od = OwnerDefeat(it->id);
        if (g_role == 1 && erules::DownDefeat(od)) {
            esync::ReleaseHold(it->id, "her outcome (SEN1)");
            Log("[enemy] restrain HIT #%u id=%05X: her copy is %s now (SEN1)", it->seq, it->id, erules::DefeatText(od));
            if (g_cfg.enemy_replay_flush) shots::FlushEnemyReplays(it->id, "her SEN1 after our restrain");
            it = g_rsent.erase(it);
            continue;
        }
        if (g_now - it->t > erules::kRestrainHoldMs) {
            if (LogNth(++g_r_noanswer)) Log("[enemy] restrain HIT #%u id=%05X: no owner outcome in %.0f s (the hold ends itself)", it->seq, it->id, erules::kRestrainHoldMs / 1000.0);
            it = g_rsent.erase(it);
            continue;
        }
        ++it;
    }
    // the scan: a frozen copy near our Sam that goes down here with no HIT of ours seen (a knockout, a tie-up by another path)
    if (g_now < g_scan_next) return;
    g_scan_next = g_now + 100;
    WorldTransform sx{};
    const bool have_sam = ReadXf(g_sam.load(), sx);
    std::uint32_t ids[64];
    std::uintptr_t actors[64], ents[64];
    const unsigned n = esync::FrozenCopies(ids, actors, ents, 64);
    std::map<std::uint32_t, std::uint8_t> next;
    for (unsigned i = 0; i < n; ++i) {
        MuleNow m;
        if (!ReadMule(actors[i], ids[i], m) || !m.have_xf) continue;
        const std::uint8_t d = static_cast<std::uint8_t>(erules::OutcomeOf(m.ea0, m.ea4));
        next[ids[i]] = d;
        if (!have_sam || !g_restrain_send) continue;
        const double dx = m.xf.pos.x - sx.pos.x, dy = m.xf.pos.y - sx.pos.y, dz = m.xf.pos.z - sx.pos.z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > kRestrainScanM) continue;
        const auto p = g_scan_was.find(ids[i]);
        if (p == g_scan_was.end() || !erules::FlushOnDefeat(p->second, d)) continue;
        if (d != static_cast<std::uint8_t>(erules::Defeat::restrained) && d != static_cast<std::uint8_t>(erules::Defeat::faint)) continue;
        if (g_rwatch.count(ids[i]) || RestrainPending(ids[i])) continue;
        RWatch w;
        w.t0 = g_now, w.copy_was = p->second;
        g_rwatch[ids[i]] = w;
        ++g_r_watches;
        esync::HoldCopy(ids[i], erules::kRestrainHoldMs, "the copy went down here");
        Log("[enemy] restrain watch id=%05X opened: the copy went %s here (+0xEA4 0x%X; no strand / bola HIT of ours seen)", ids[i], erules::DefeatText(d), m.ea4);
    }
    g_scan_was.swap(next);
}
// The acting side: the owner's HITRES for one of our restrains (true: it was one).
bool RestrainAnswer(const ewire::HitRes& r) {
    for (auto it = g_rsent.begin(); it != g_rsent.end(); ++it) {
        if (it->id != r.key || it->seq != r.hit_seq) continue;
        ++g_r_answers;
        if (r.result == static_cast<std::uint8_t>(erules::HitResult::applied)) {
            it->answered = true;
            Log("[enemy] restrain HIT #%u id=%05X applied by the %s (its outcome now %s): the copy held until the owner's outcome", r.hit_seq, r.key,
                g_role == 2 ? "host" : "guest", erules::DefeatText(r.defeat));
        } else {
            ++g_r_answer_refused;
            esync::ReleaseHold(r.key, "refused by the owner");
            Log("[enemy] restrain HIT #%u id=%05X refused by the %s: %s (its outcome %s): the copy re-posed with the owner's frames", r.hit_seq, r.key,
                g_role == 2 ? "host" : "guest", erules::HitResultText(r.result), erules::DefeatText(r.defeat));
            g_rsent.erase(it);
        }
        return true;
    }
    return false;
}
// The owner: one HIT of kind 5 (the host: hers; the guest: the host's on one she owns, relayed). Answers with a HITRES.
void OwnerRestrain(const ewire::Hit& h, std::uint32_t msg_guids, const esync::GateView& gv, bool relay) {
    ++g_r_rx;
    const std::uintptr_t actor = ActorOf(h.key);
    MuleNow m;
    const bool found = actor && ReadMule(actor, h.key, m);
    erules::RestrainIn in;
    in.on = g_cfg.enemy_restrain_relay && g_hits_on && !g_stop.load() && CarryReady();
    in.peer_part = shots::PeerRestrain();
    in.k_ok = h.b180 == erules::kRestrainK || h.b180 == erules::kFaintK;
    in.fp_equal = msg_guids == gv.guids && gv.guids != 0;
    in.owned = relay ? esync::OwnedHere(h.key) : !esync::MirrorOf(h.key, nullptr, nullptr);  // the host freezes only her owned ones
    in.found = found;
    in.realized = found && m.realized && m.have_xf;
    in.state = found ? m.ea0 : 0;
    in.hidden_or_asleep = found && (m.a88 & 3u) != 0;
    const std::uint8_t was = found ? static_cast<std::uint8_t>(erules::OutcomeOf(m.ea0, m.ea4)) : 0;
    in.defeat_now = was;
    erules::HitResult r = erules::RestrainGate(in);
    int rc = 2;
    if (r == erules::HitResult::applied) {
        rc = SetOutcome(actor, h.key, h.b180);
        r = erules::RestrainSetResult(rc);
    }
    const char* from = relay ? "host" : "guest";
    if (r == erules::HitResult::applied) {
        ++g_r_applied;
        RBack b;
        b.id = h.key, b.seq = h.hit_seq, b.actor = actor, b.t = g_now, b.k = h.b180, b.src = h.depth, b.was = was, b.attack = h.attack, b.set = rc == 1, b.relay = relay;
        g_rback.push_back(b);
        if (!relay) {
            HostRec& rec = g_hout[h.key];
            rec.last_hit_ms = g_now, rec.last_seq = h.hit_seq, rec.actor = actor;
            rec.r_seq = h.hit_seq, rec.r_attack = h.attack, rec.r_k = h.b180, rec.r_src = h.depth;
        }
        Log("[enemy] restrain HIT #%u id=%05X from the %s (%s; %s): outcome setter k %u %s (same MULE, outcome none before)", h.hit_seq, h.key, from,
            RestrainWhat(h.attack, h.b180, h.depth), RestrainSrcText(h.depth), h.b180, rc == 1 ? "called" : "deferred (a hit of this frame queued: next tick)");
    } else {
        ++g_r_refused;
        Log("[enemy] restrain HIT #%u id=%05X from the %s refused: %s (k %u; state %u, realized %d, a88 0x%X, outcome %s; relay %d, peer part %d, carry %s)", h.hit_seq,
            h.key, from, erules::HitResultText(static_cast<std::uint8_t>(r)), h.b180, in.state, in.realized ? 1 : 0, found ? m.a88 : 0, erules::DefeatText(was),
            g_cfg.enemy_restrain_relay ? 1 : 0, in.peer_part ? 1 : 0, CarryReady() ? "ready" : "off");
    }
    ewire::HitRes hr;
    hr.key = h.key, hr.hit_seq = h.hit_seq, hr.result = static_cast<std::uint8_t>(r), hr.owner = static_cast<std::uint8_t>(g_cfg.enemy_hit_owner);
    hr.defeat = found ? static_cast<std::uint8_t>(OutcomeNow(actor, h.key) >= 0 ? OutcomeNow(actor, h.key) : 0) : 0;
    (relay ? g_rres_out : g_res_out).push_back(hr);
}
// The owner: a deferred setter tried again (up to 1 s), the readback 1 s after it.
void RestrainOwnerTick() {
    for (auto it = g_rback.begin(); it != g_rback.end();) {
        RBack& b = *it;
        if (!b.set) {
            const int rc = SetOutcome(b.actor, b.id, b.k);
            if (rc == 1) b.set = true, b.t = g_now;
            else if (rc != 0 || g_now - b.t > 1000) {
                Log("[enemy] restrain HIT #%u id=%05X: the deferred setter %s", b.seq, b.id, rc == -2 ? "found it down already" : rc == 0 ? "stayed deferred for 1 s: dropped" : "could not run");
                it = g_rback.erase(it);
                continue;
            }
        }
        if (!b.set || g_now - b.t < 1000) {
            ++it;
            continue;
        }
        const int now_d = OutcomeNow(b.actor, b.id);
        const std::uint8_t want = b.k == erules::kRestrainK ? static_cast<std::uint8_t>(erules::Defeat::restrained) : static_cast<std::uint8_t>(erules::Defeat::faint);
        if (now_d == static_cast<int>(want))
            Log("[enemy] OUTCOME id=%05X %s -> %s (cause %s HIT #%u, %s): read back 1 s after the setter k %u", b.id, erules::DefeatText(b.was), erules::DefeatText(want),
                b.relay ? "host" : "guest", b.seq, RestrainWhat(b.attack, b.k, b.src), b.k);
        else
            Log("[enemy] restrain readback id=%05X (HIT #%u): k %u -> now %s (not %s)", b.id, b.seq, b.k, now_d < 0 ? "not the same MULE" : erules::DefeatText(static_cast<std::uint8_t>(now_d)),
                erules::DefeatText(want));
        it = g_rback.erase(it);
    }
}

// ---------------------------------------------------------------- the guest: captures out, HITRES and OUTCOMEs in
void GuestCapture(const Cap& c, bool hits_on) {
    if (!hits_on) {
        ++g_off_dropped;
        return;
    }
    if (!erules::AttackOk(c.attack)) {
        ++g_bad_attack;
        return;
    }
    if (!erules::HitValuesOk(c.amount, c.mult, c.a20, c.v184, c.part)) {
        // outside the ranges the probe saw: never sent (the host refuses them too: `values`)
        if (LogNth(++g_bad_values))
            Log("[enemy] HIT not sent id=%05X attack 0x%X: values out of range (amount %.2f mult %.2f a20 %.2f power bits 0x%08X part %d)", c.key, c.attack,
                static_cast<double>(c.amount), static_cast<double>(c.mult), static_cast<double>(c.a20), c.v184, static_cast<int>(c.part));
        return;
    }
    if (g_dedupe.Seen(c.key, c.serial, c.attack, g_now)) {
        ++g_deduped;
        return;
    }
    if (erules::LethalRefused(c.lethal, g_cfg.enemy_hits_lethal)) {
        ++g_lethal_refused;
        if (!g_lethal_notice) g_lethal_notice = true, Log("[enemy] hits: her lethal rounds are refused here (enemy_hits_lethal=0; attack 0x%X does damage)", c.attack);
        return;
    }
    // the mirror's frame now (yaw only)
    std::uintptr_t actor = 0, entity = 0;
    if (!esync::MirrorOf(c.key, &actor, &entity)) entity = c.entity;
    WorldTransform x{};
    if (!ReadXf(entity, x)) {
        ++g_unready;
        return;
    }
    const double root[3] = {x.pos.x, x.pos.y, x.pos.z};
    const double yaw = YawOf(x);
    double pw[3] = {c.point[0], c.point[1], c.point[2]};
    if (!c.have_point) pw[0] = root[0], pw[1] = root[1], pw[2] = root[2] + 1.2;  // no point: the chest
    double pl[3], nl[3], il[3];
    erules::ToLocal(root, yaw, pw, pl);
    const double nw[3] = {c.normal[0], c.normal[1], c.normal[2]}, iw[3] = {c.impulse[0], c.impulse[1], c.impulse[2]};
    erules::DirToLocal(yaw, nw, nl);
    erules::DirToLocal(yaw, iw, il);
    if (!(std::fabs(pl[0]) <= erules::kHitLocalMaxM && std::fabs(pl[1]) <= erules::kHitLocalMaxM && std::fabs(pl[2]) <= erules::kHitLocalMaxM)) {
        ++g_far;
        return;
    }
    ewire::Hit h;
    h.key = c.key, h.hit_seq = ++g_hit_seq, h.attack = c.attack, h.kind = c.kind, h.flags = c.flags;
    const double nlen = std::sqrt(nl[0] * nl[0] + nl[1] * nl[1] + nl[2] * nl[2]);
    if (nlen > 0.5 && nlen < 1.5) h.flags |= ewire::kHitNormalValid;
    if (il[0] != 0 || il[1] != 0 || il[2] != 0) h.flags |= ewire::kHitImpulseValid;
    h.serial = c.serial, h.b12E = c.b12E, h.b180 = c.b180, h.depth = c.depth, h.v130 = c.v130, h.v184 = c.v184, h.v188 = c.v188, h.part = c.part;
    h.amount = c.amount, h.mult = c.mult, h.a20 = c.a20;
    for (int k = 0; k < 3; ++k) h.point[k] = static_cast<float>(pl[k]), h.normal[k] = static_cast<float>(nlen > 0.5 ? nl[k] / nlen : 0), h.impulse[k] = static_cast<float>(il[k]);
    h.t_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(g_now) & 0xFFFFFFFFull);
    double sp[3] = {};
    WorldTransform sx{};
    if (ReadXf(g_sam.load(), sx)) sp[0] = sx.pos.x, sp[1] = sx.pos.y, sp[2] = sx.pos.z;
    const double dist = std::sqrt((pw[0] - sp[0]) * (pw[0] - sp[0]) + (pw[1] - sp[1]) * (pw[1] - sp[1]) + (pw[2] - sp[2]) * (pw[2] - sp[2]));
    h.her_dist_m = static_cast<float>(dist < 6553.0 ? dist : 6553.0);
    // round 23: a strand / bola contact on a copy is held back while the copy is watched (the tie-up relay)
    if (g_restrain_send) {
        const erules::RestrainTrig trig = erules::RestrainTrigger(h.attack, h.b12E);
        const bool test = g_restrain_test_next;
        g_restrain_test_next = false;
        if (trig != erules::RestrainTrig::none && RestrainOpen(h, trig, test)) return;
    }
    if (g_out.size() >= kOutCap) g_out.erase(g_out.begin()), ++g_out_capped;  // the link is stalled: the oldest goes
    g_out.push_back(h);
    ++g_sent;
    g_last_hit_id = c.key;
    static const char* const kKind[5] = {"?", "bullet", "sphere/job", "direct", "test"};
    if (LogNth(g_sent))
        Log("[enemy] HIT sent #%u id=%05X attack 0x%X kind %s part %d amount %.1f serial 0x%X%s local (%.2f, %.2f, %.2f) m, her distance %.1f m", h.hit_seq, h.key,
            h.attack, kKind[h.kind < 5 ? h.kind : 0], static_cast<int>(h.part), static_cast<double>(h.amount), h.serial, (h.flags & ewire::kHitLethal) ? " lethal" : "",
            pl[0], pl[1], pl[2], dist);
}
void FlushHits(std::uint32_t guids) {
    std::uint8_t buf[ewire::kScbMaxSize];
    // a HIT that waited more than a second (a full window, a stalled link) is dropped, never applied late in a burst
    const std::uint32_t now_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(g_now) & 0xFFFFFFFFull);
    const std::size_t before = g_out.size();
    g_out.erase(std::remove_if(g_out.begin(), g_out.end(), [&](const ewire::Hit& x) { return now_ms - x.t_ms > kHitMaxAgeMs; }), g_out.end());
    if (g_out.size() != before) {
        g_stale_dropped += static_cast<unsigned>(before - g_out.size());
        static unsigned logged = 0;
        if (LogNth(++logged)) Log("[enemy] hits: %zu HIT(s) older than %u ms dropped before sending (the link was stalled; %u in all)", before - g_out.size(), kHitMaxAgeMs,
                                  g_stale_dropped);
    }
    while (!g_out.empty()) {
        const unsigned n = static_cast<unsigned>(g_out.size() < ewire::kHitMax ? g_out.size() : ewire::kHitMax);
        ewire::ScbHeader h;
        h.guids_fp = guids, h.seq = g_scb_seq + 1;
        const std::size_t size = ewire::EncodeHits(h, g_out.data(), n, buf, sizeof(buf));
        if (!size) {
            g_out.erase(g_out.begin(), g_out.begin() + n);
            continue;
        }
        if (!link::SendCombat(buf, size)) break;
        ++g_scb_seq;
        g_out.erase(g_out.begin(), g_out.begin() + n);
    }
}
void GuestOutcome(const ewire::Outcome& o) {
    ++g_oc_rx;
    // a new host epoch (the host's world or link session changed: its versions start again): her table goes first (review
    // rv-safety 8: else a reloaded host's outcomes would be older than hers and dead would stay stuck)
    if (o.epoch && o.epoch != g_seen_epoch) {
        if (g_seen_epoch)
            Log("[enemy] hits: the host's outcome epoch %u -> %u (its world changed): her outcome table (%zu) dropped", g_seen_epoch, o.epoch, g_gout.size());
        g_gout.clear();
        g_seen_epoch = o.epoch;
    }
    GuestRec& g = g_gout[o.key];
    const std::uint8_t was = g.rec.have ? g.rec.defeat : 0;
    const bool changed = erules::MergeOutcome(g.rec, o.defeat, o.ver, o.cause);
    if (changed && o.cause == static_cast<std::uint8_t>(erules::Cause::revive)) g.sen_dead = false;
    if (g.rec.ver == o.ver) g.state = o.state, g.downbits = o.downbits, g.health = o.health, g.stamina = o.stamina, g.last_seq = o.last_seq, g.cause = o.cause;
    if (!changed) return;
    const char* what = o.defeat == 0 ? "the mirror gets up with the host's frames"
                       : o.defeat == static_cast<std::uint8_t>(erules::Defeat::dead) ? "the mirror lies dead with the host's frames (hidden and asleep here when it is released)"
                                                                                       : "the mirror lies with the host's frames";
    Log("[enemy] OUTCOME id=%05X %s -> %s ver %u (cause %s; state %u bits 0x%X, health %.1f stamina %.1f, last HIT #%u): %s", o.key, erules::DefeatText(was),
        erules::DefeatText(o.defeat), o.ver, erules::CauseText(o.cause), o.state, o.downbits, static_cast<double>(o.health), static_cast<double>(o.stamina), o.last_seq,
        what);
    // round 23: the owner's down outcome ends a tie-up hold on the copy, and the shooter's queued replays go
    if (erules::DownDefeat(o.defeat)) {
        if (esync::Holding(o.key)) esync::ReleaseHold(o.key, "the host's OUTCOME");
        g_rsent.erase(std::remove_if(g_rsent.begin(), g_rsent.end(), [&](const RSent& s) { return s.id == o.key; }), g_rsent.end());
        if (g_cfg.enemy_replay_flush && erules::FlushOnDefeat(was, o.defeat)) shots::FlushEnemyReplays(o.key, "the host's OUTCOME");
    }
}

void DrainCombat(const esync::GateView& gv) {
    std::uint8_t buf[1100];
    std::size_t n = 0;
    std::uint32_t gen = 0;
    static ewire::Hit hits[ewire::kHitMax];
    static ewire::HitRes res[ewire::kHitResMax];
    static ewire::Outcome oc[ewire::kOutcomeMax];
    while (link::PopCombat(buf, sizeof(buf), n, gen)) {
        if (gen != link::Generation()) continue;
        ewire::ScbHeader h;
        if (!ewire::DecodeScbHeader(buf, n, h)) {
            ++g_bad_msg;
            continue;
        }
        if (h.type == ewire::kScbHit) {
            if (g_role != 1 && g_role != 2) {
                ++g_wrong_role;
                continue;
            }
            if (!ewire::DecodeHits(buf, n, h, hits)) {
                ++g_bad_msg;
                continue;
            }
            if (g_role == 2) {
                // P1c: the host's hits on its frozen copies of the ones she owns, applied here to hers (no HITRES, no OUTCOME back: her
                // copy's outcome reaches the host through her SEN1 and the return's outcome carry). Round 23: a restrain (kind 5)
                // is answered with a HITRES (g_rres_out).
                for (unsigned i = 0; i < h.n; ++i) {
                    if (hits[i].kind == ewire::kKindRestrain) {
                        OwnerRestrain(hits[i], h.guids_fp, gv, true);
                        continue;
                    }
                    if (!esync::OwnedHere(hits[i].key)) {
                        ++g_relay_not_owned;
                        if (LogNth(g_relay_not_owned)) Log("[enemy] relayed HIT #%u id=%05X refused: not owned here (now)", hits[i].hit_seq, hits[i].key);
                        continue;
                    }
                    ++g_relay_in;
                    const erules::HitResult rr = HostHit(hits[i], h.guids_fp, gv, true);
                    const bool applied = rr == erules::HitResult::applied;
                    if (applied) ++g_relay_applied;
                    static const char* const kRelayKind[5] = {"?", "bullet", "sphere/job", "direct", "test"};
                    if (LogNth(g_relay_in))
                        Log("[enemy] relayed HIT #%u id=%05X attack 0x%X from the host (kind %s): %s%s (relayed %u, applied %u)", hits[i].hit_seq, hits[i].key,
                            hits[i].attack, kRelayKind[hits[i].kind < 5 ? hits[i].kind : 0], applied ? "applied to hers" : "refused: ",
                            applied ? "" : erules::HitResultText(static_cast<std::uint8_t>(rr)), g_relay_in, g_relay_applied);
                }
                g_res_out.clear(), g_oc_out.clear();
                continue;
            }
            for (unsigned i = 0; i < h.n; ++i) {
                if (hits[i].kind == ewire::kKindRestrain) OwnerRestrain(hits[i], h.guids_fp, gv, false);  // round 23
                else HostHit(hits[i], h.guids_fp, gv);
            }
        } else if (h.type == ewire::kScbResync) {
            if (g_role != 1) {
                ++g_wrong_role;
                continue;
            }
            unsigned k = 0;
            for (const auto& kv : g_hout)
                if (kv.second.have && kv.second.defeat) g_oc_out.push_back(OutcomeMsg(kv.first, kv.second, static_cast<std::uint8_t>(erules::Cause::resync))), ++k;
            Log("[enemy] hits: RESYNC from the guest: %u outcome(s) sent", k);
        } else if (h.type == ewire::kScbHitRes) {
            // round 23: the host takes HITRES only for its relayed restrains (her answers)
            const bool host_restrain = g_role == 1 && (g_relay_on || !g_rsent.empty());
            if ((g_role != 2 && !host_restrain) || !ewire::DecodeHitRes(buf, n, h, res)) {
                ++(g_role != 2 && !host_restrain ? g_wrong_role : g_bad_msg);
                continue;
            }
            if (g_role == 1) {
                for (unsigned i = 0; i < h.n; ++i)
                    if (!RestrainAnswer(res[i])) ++g_wrong_role;
                continue;
            }
            for (unsigned i = 0; i < h.n; ++i) {
                if (RestrainAnswer(res[i])) continue;
                ++g_res_rx;
                ++g_res_by[res[i].result < erules::kHitResults ? res[i].result : 7];
                if (LogNth(g_res_rx)) {
                    if (res[i].result == 0)
                        Log("[enemy] HITRES #%u id=%05X applied (owner %s; its defeat now %s)", res[i].hit_seq, res[i].key, cfgval::HitOwnerName(res[i].owner),
                            erules::DefeatText(res[i].defeat));
                    else
                        Log("[enemy] HITRES #%u id=%05X refused: %s", res[i].hit_seq, res[i].key, erules::HitResultText(res[i].result));
                }
            }
        } else if (h.type == ewire::kScbOutcome) {
            if (g_role != 2 || !ewire::DecodeOutcomes(buf, n, h, oc)) {
                ++(g_role != 2 ? g_wrong_role : g_bad_msg);
                continue;
            }
            for (unsigned i = 0; i < h.n; ++i) GuestOutcome(oc[i]);
        } else if (h.type == ewire::kScbOwn || h.type == ewire::kScbOwnAck || h.type == ewire::kScbOwnReset) {
            esync::QueueOwnMessage(buf, n);  // P1b: enemy_sync's (its next tick)
        }
    }
}

void ResetWorld() {
    g_gout.clear();
    g_seen_epoch = 0;
    g_oc_epoch = static_cast<std::uint8_t>(g_oc_epoch >= 255 ? 1 : g_oc_epoch + 1);
    g_oc_full_next = 0;
    g_comp_cache.clear();
    g_hout.clear();
    g_out.clear();
    g_res_out.clear();
    g_oc_out.clear();
    g_effects.clear();
    g_dedupe.Reset();
    g_rate.Reset();
    g_rwatch.clear(), g_rsent.clear(), g_rback.clear(), g_rres_out.clear(), g_scan_was.clear();
    g_actors.clear();
    g_actors_at = -1e18;
    g_ids_done = false;
    g_resync_due = true;
    Cap c;
    while (RingPop(c)) {
    }
}

void TargetTick();  // the target probe (below)
void ProbeTickGuarded() noexcept;  // round 23 E: the M2 probes (below)
void TakeTickGuarded() noexcept;   // round 25: K1's 2 Hz writes and C1's follow-up after her take (below)
void TakeChecks() noexcept;        // round 25: the take levers' byte checks (below)
void StimTick() noexcept;  // her body's visual stimulus (below): dropped when her body or its agent changes
void StimForget(const char* why) noexcept;  // forgotten without the remove (no longer ours: reused, or the old world's)
void TestTick() {
    if (g_test.left <= 0 || g_now < g_test.next) return;
    --g_test.left;
    g_test.next = g_now + 150;
    const std::uintptr_t actor = ActorOf(g_test.id);
    MuleNow m;
    if (!actor || !ReadMule(actor, g_test.id, m) || !m.realized || !m.have_xf) {
        Log("[enemy] hit test: %05X is not realized here", g_test.id);
        g_test.left = 0;
        return;
    }
    const std::uintptr_t res = AttackEntry(g_test.attack);
    const bool head = g_mode_part == -2;
    double local[3] = {0, 0, head ? 1.62 : 1.2};
    const double root[3] = {m.xf.pos.x, m.xf.pos.y, m.xf.pos.z};
    const double yaw = YawOf(m.xf);
    if (g_test.send) {
        // the guest's test HIT (kind 4) through the capture path
        Cap c;
        c.key = g_test.id, c.kind = ewire::kKindTest, c.attack = g_test.attack, c.v184 = 0x3F800000u, c.part = 0xFFFFFFFFu, c.entity = m.entity;
        c.lethal = res && F32(res + 0x24) > 0.0f;
        if (c.lethal) c.flags |= ewire::kHitLethal;
        c.have_point = true;
        erules::ToWorld(root, yaw, local, c.point);
        c.normal[0] = static_cast<float>(std::cos(yaw)), c.normal[1] = static_cast<float>(std::sin(yaw)), c.normal[2] = 0;
        GuestCapture(c, g_hits_on);
        return;
    }
    ewire::Hit h;
    h.key = g_test.id, h.hit_seq = ++g_hit_seq, h.attack = g_test.attack, h.kind = ewire::kKindTest, h.v184 = 0x3F800000u;
    h.part = g_mode_part >= 0 ? static_cast<std::uint32_t>(g_mode_part) : head && m.comp ? U32(m.comp + kCompHead) : 0xFFFFFFFFu;
    if (h.part != 0xFFFFFFFFu) h.flags |= ewire::kHitPartValid;
    h.flags |= ewire::kHitNormalValid;
    const double nl[3] = {1, 0, 0}, il[3] = {0, 0, 0};
    const int owner = g_mode_owner >= 0 ? g_mode_owner : g_cfg.enemy_hit_owner;
    const int path = g_mode_path >= 0 ? g_mode_path : g_cfg.enemy_hit_path;
    CreditIds();
    ApplyResult ar;
    ApplyOne(g_test.id, h, local, nl, il, owner, path, erules::HostSerial(0, h.hit_seq), ar);
    Log("[enemy] hit test #%u id=%05X attack 0x%X part %d (%s) owner %s%s path %s: %s (ctx %p, P+0x0C 0x%X, serial 0x%08X, attacker id 0x%X -> 0x%X%s)%s", h.hit_seq,
        g_test.id, g_test.attack, static_cast<int>(h.part), head ? "head" : "chest", cfgval::HitOwnerName(owner), ar.owner_fell_back ? " (no body: owner 0)" : "",
        path ? "query" : "direct", erules::HitResultText(static_cast<std::uint8_t>(ar.r)), reinterpret_cast<void*>(ar.out.ctx), ar.out.p0c,
        erules::HostSerial(0, h.hit_seq), ar.out.p20_before, ar.out.p20_after, ar.out.res_stamped ? ", resource stamped" : "",
        path ? [&] {
            static char q[160];
            std::snprintf(q, sizeof(q), "; query: %u in the sphere, %u kept, DI part %d at (%.2f, %.2f, %.2f)", ar.out.q_seen, ar.out.q_kept,
                          static_cast<int>(ar.out.q_part), ar.out.q_hit[0], ar.out.q_hit[1], ar.out.q_hit[2]);
            return static_cast<const char*>(q);
        }()
             : "");
    if (path && ar.out.q_kept) Log("[enemy] hit test: part %s=%d (query; the component's head part %d)", head ? "head" : "chest", static_cast<int>(ar.out.q_part),
                                   m.comp ? static_cast<int>(U32(m.comp + kCompHead)) : -1);
}

void TickBody(const TickIn& in, double now) {
    g_now = now;
    ++g_ticks;
    g_body = in.body;
    g_sam.store(in.local_ok ? reinterpret_cast<std::uintptr_t>(in.local) : 0, std::memory_order_relaxed);
    g_role = in.net ? in.role : 0;
    std::uint32_t counter = 0;
    // the session's world; without a session (solo, the P2-0 probe) our Sam in the world (no outcome leaves this game then)
    const bool ready = coop::WorldReady(&counter) || (coop::SessionRole() == 0 && g_role == 0 && in.local_ok);
    const std::uint32_t gen = link::Generation();
    if (ready != g_world_ready || counter != g_world || gen != g_gen) {
        // the probe's stimulus belongs to the old world's pool: forgotten, never removed there (review r20 enemy 5)
        if (!ready || counter != g_world) StimForget("the world changed");
        g_world_ready = ready, g_world = counter, g_gen = gen;
        ResetWorld();
    }
    StimTick();  // (after the world check: never a remove on the old world's pool)
    if (!ready) return;
    if (!g_ids_done && in.local_ok) CreditIds();
    const esync::GateView gv = esync::Gate();
    const bool ours = g_want && !g_stop.load();
    shots::SetHitsPart(ours && (g_role == 1 || g_role == 2), !g_want ? "enemy_hits=0)" : g_stop.load() ? "stopped after a fault)" : "solo)");
    shotw::Beacon peer;
    double age = 1e18;
    const bool have_peer = shots::PeerBeacon(&peer, &age);
    const bool on = ours && (g_role == 1 || g_role == 2) && gv.mirroring && have_peer && (peer.parts & shotw::kPartHits);
    if (on != g_hits_on) {
        const char* who = g_role == 1 ? "host" : g_role == 2 ? "guest" : "solo";
        if (on)
            Log("[enemy] hits on (%s): owner %s, path %s, lethal %d, rate %d (both beacons carry the hits part, mirroring on)", who,
                cfgval::HitOwnerName(g_cfg.enemy_hit_owner), g_cfg.enemy_hit_path ? "query" : "direct", g_cfg.enemy_hits_lethal ? 1 : 0, g_cfg.enemy_hit_rate);
        else
            Log("[enemy] hits off (%s): %s", who,
                !ours ? (g_want ? "stopped" : "enemy_hits=0") : !gv.mirroring ? "mirroring off" : !have_peer ? "no beacon" : "the partner has no hits part");
        if (on && g_role == 2) g_resync_due = true;
        g_hits_on = on;
    }
    // P1c: the host captures its hits on its frozen copies of hers (its followers: the host freezes no other copy) and relays them
    const bool relay = on && g_role == 1 && esync::OwnRelay();
    if (relay != g_relay_on) g_relay_on = relay, Log("[enemy] hits: the host's hits on her owned ones %s", relay ? "relayed to her (enemy_owner_hits)" : "not relayed");
    g_capture.store(on && (g_role == 2 || relay), std::memory_order_relaxed);
    // round 23: the tie-up relay (the beacon part: this game takes kind 5 and can run the outcome setter)
    const bool r_part = ours && g_cfg.enemy_restrain_relay && CarryReady();
    shots::SetRestrainPart(r_part);
    const bool r_send = on && r_part && shots::PeerRestrain() && (g_role == 2 || relay);
    if (r_send != g_restrain_send || !g_restrain_logged) {
        if (r_send || g_restrain_logged)
            Log("[enemy] restrain relay %s", r_send ? "on: a strand / bola contact or a knockout of a copy goes to its owner as HIT kind 5 (both beacons carry the restrain part)"
                                             : !g_cfg.enemy_restrain_relay                    ? "off (enemy_restrain_relay=0)"
                                             : !CarryReady()                                  ? "off (the outcome setter's checks did not pass)"
                                             : !on                                            ? "off (hits off)"
                                             : g_role == 1 && !relay                          ? "off here (the host sends only while it relays: enemy_owner)"
                                                                                              : "off (the partner's beacon has no restrain part: an older build)");
        g_restrain_logged = g_restrain_logged || r_send;
        g_restrain_send = r_send;
    }
    // the captures (the guest; the host's relay; a solo run drains and forgets them)
    Cap c;
    unsigned took = 0;
    while (took < 64 && RingPop(c)) {
        ++took;
        if (g_role == 2 || relay) GuestCapture(c, on);
    }
    TestTick();
    TargetTick();
    ProbeTickGuarded();
    TakeTickGuarded();
    if (g_restrain_send || !g_rwatch.empty() || !g_rsent.empty()) RestrainActTick();
    if (in.net) DrainCombat(gv);
    if (!g_rback.empty()) RestrainOwnerTick();
    if (g_role == 2 && !g_rres_out.empty()) SendRes(g_rres_out, gv.guids);  // round 23: her answers to the host's restrains
    if (g_role == 2 && on) {
        if (g_resync_due) {
            std::uint8_t buf[ewire::kScbHeader];
            ewire::ScbHeader h;
            h.guids_fp = gv.guids, h.seq = g_scb_seq + 1;
            if (ewire::EncodeResync(h, buf, sizeof(buf)) && link::SendCombat(buf, sizeof(buf))) ++g_scb_seq, g_resync_due = false, Log("[enemy] hits: RESYNC sent");
        }
        FlushHits(gv.guids);
    } else if (relay) {
        FlushHits(gv.guids);
    } else if (!g_out.empty()) {
        g_off_dropped += static_cast<unsigned>(g_out.size());
        g_out.clear();
    }
    if ((g_role == 1 && gv.mirroring) || g_role == 0) HostOutcomes(on && g_role == 1);  // solo: the test hits' enemies
    if (g_role == 1 && on) SendOut(gv.guids);
    else if (g_role == 1) g_res_out.clear(), g_oc_out.clear();
    EffectsTick();
}
bool TickGuarded(const TickIn& in, double now) noexcept {
    __try {
        TickBody(in, now);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------- start-up
bool Checks() {
    unsigned n = 0, ok = 0, bad = 0;
    const char* first = nullptr;
    for (const Site& s : kSites) {
        ++n;
        if (mem::CodeMatches(A(s.va), s.sig)) ++ok;
        else bad |= s.group, first = first ? first : s.name, Log("[enemy] hits: byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
    }
    const struct {
        bool okv;
        unsigned group;
        const char* name;
    } data[] = {
        {mem::RipTarget(A(kOnHit), 3, 7) == A(kDmgSys), kGOnHit, "OnHit's damage system global"},
        {mem::RipTarget(A(kMuleDmgThunk), 1, 5) == A(kMuleDmg), kGMule, "the thunk's target"},
        {mem::RipTarget(A(0x14291216C), 3, 7) == A(kG16), kGStamps, "BulletDamageA's 16 bytes (0x147BC3B80)"},
        {mem::RipTarget(A(0x142912274), 3, 7) == A(kAttackReg), kGStamps, "the attack registry (0x147BEB0F8)"},
        {mem::Q(A(kMuleDmgVt)) == A(0x142AFB290) && mem::Q(A(kMuleDmgVt) + 0x158) == A(0x142AF89F0), kGOutcome, "DSMuleDamageComponent's vtable"},
        {mem::InImage(A(kCtxType)) && mem::InImage(A(kRegGlobal)), kGApply, "the context type, the actor registry"},
    };
    for (const auto& d : data) {
        ++n;
        if (d.okv) ++ok;
        else bad |= d.group, first = first ? first : d.name, Log("[enemy] hits: byte check FAILED: %s", d.name);
    }
    g_checks_n = n, g_checks_ok = ok;
    g_groups = (kGOnHit | kGMule | kGStamps | kGApply | kGOutcome | kGEntry | kGCarry) & ~bad;
    Log("[enemy] hits: byte checks %u/%u ok (onhit %s, muledmg %s, stamps %s, apply %s, outcome %s, entry %s)", ok, n, (bad & kGOnHit) ? "FAILED" : "ok",
        (bad & kGMule) ? "FAILED" : "ok", (bad & kGStamps) ? "FAILED" : "ok", (bad & kGApply) ? "FAILED" : "ok", (bad & kGOutcome) ? "FAILED" : "ok",
        (bad & kGEntry) ? "FAILED" : "ok");
    Log("[enemy] hits: outcome carry checks %s (P1c)", (bad & kGCarry) ? "FAILED: her downs keep a camp hers" : "ok");
    // the MULE handler (G3) is a backstop: its own failure leaves it out; the carry (P1c) only turns itself off; every other group
    // is needed
    return !(bad & ~static_cast<unsigned>(kGMule | kGCarry));
}

}  // namespace

// ---------------------------------------------------------------- public
void Install(const Config& config) noexcept {
    g_cfg = config;
    // round 25: the take levers' sites, checked at start-up when a lever key or the probes are on (a mismatch turns that lever off).
    // Round 26 review: the keys are on by default, so only with enemy sync on (as the hits below): nothing new with enemy_sync=0
    if ((config.enemy_sync || automation::Enabled()) &&
        (config.enemy_take_know || config.enemy_take_camp || config.enemy_ticket_return || config.enemy_m2_probes || config.enemy_take_role ||
         config.enemy_take_face))
        TakeChecks();
    RingInit();
    if (!config.enemy_hits) {
        shots::SetHitsPart(false, "enemy_hits=0)");
        Log("[enemy] hits OFF: enemy_hits=0");
        return;
    }
    if (!config.enemy_sync && !automation::Enabled()) {
        shots::SetHitsPart(false, "enemy_sync=0)");
        Log("[enemy] hits OFF: enemy sync is off (enemy_sync=0)");
        return;
    }
    if (!Checks()) {
        shots::SetHitsPart(false, "byte checks)");
        Log("[enemy] hits OFF: byte check FAILED (this game build differs)");
        return;
    }
    MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(A(kOnHit)), reinterpret_cast<void*>(&H_OnHit), reinterpret_cast<void**>(&o_onhit));
    if (s == MH_OK) s = MH_EnableHook(reinterpret_cast<void*>(A(kOnHit)));
    if (s != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(A(kOnHit)));
        shots::SetHitsPart(false, "OnHit not hooked)");
        Log("[enemy] hits OFF: Entity OnHit not hooked (%s)", MH_StatusToString(s));
        return;
    }
    g_g2.store(true);
    if (g_groups & kGMule) {
        MH_STATUS m = MH_CreateHook(reinterpret_cast<void*>(A(kMuleDmg)), reinterpret_cast<void*>(&H_MuleDmg), reinterpret_cast<void**>(&o_muledmg));
        if (m == MH_OK) m = MH_EnableHook(reinterpret_cast<void*>(A(kMuleDmg)));
        if (m == MH_OK) g_g3.store(true);
        else MH_RemoveHook(reinterpret_cast<void*>(A(kMuleDmg))), Log("[enemy] hits: the MULE damage handler not hooked (%s): the direct senders are not caught", MH_StatusToString(m));
    } else {
        Log("[enemy] hits: the MULE damage handler's checks failed: the direct senders are not caught (OnHit takes the rest)");
    }
    g_want = true;
    g_installed = true;
    Log("[enemy] hits installed: owner %s, path %s, lethal %d, rate %d/s; OnHit hooked, the MULE handler %s (both games need enemy_hits=1)",
        cfgval::HitOwnerName(config.enemy_hit_owner), config.enemy_hit_path ? "query" : "direct", config.enemy_hits_lethal ? 1 : 0, config.enemy_hit_rate,
        g_g3.load() ? "hooked" : "not hooked");
}

void Tick(const TickIn& in, double now) noexcept {
    if (!g_installed) return;
    if (g_stop.load()) {
        if (g_hits_on || !g_logged_state) shots::SetHitsPart(false, "stopped after a fault)"), g_capture.store(false), g_hits_on = false, g_logged_state = true;
        return;
    }
    if (!TickGuarded(in, now)) HitsFault("the tick");
}

// P1c: the attack registry's ids as a bit set (512 bits; ids above 0x1FF are not counted). -1: unknown (the stamps group's check).
int AttackRegistry(std::uint8_t (&bits)[64], std::uint16_t* max_id) noexcept {
    std::memset(bits, 0, sizeof(bits));
    if (max_id) *max_id = 0;
    if (!(g_groups & kGStamps)) return -1;
    const std::uintptr_t reg = mem::Q(A(kAttackReg));
    std::int32_t n = 0;
    if (!reg || !mem::Rd(reg + 0x60, n) || n <= 0 || n > 4096) return -1;
    const std::uintptr_t list = mem::Q(reg + 0x68);
    int count = 0;
    for (std::int32_t i = 0; list && i < n; ++i) {
        const std::uintptr_t e = mem::Q(list + 8ull * static_cast<unsigned>(i));
        if (!e) continue;
        const std::uint16_t a = U16(e + 0x20);
        if (a >= 0x200) continue;
        bits[a >> 3] = static_cast<std::uint8_t>(bits[a >> 3] | (1u << (a & 7)));
        ++count;
        if (max_id && a > *max_id) *max_id = a;
    }
    return count;
}
// P1c: her outcome on a MULE here (the outcome setter; k 0 faint, 1 sleep, 2 restrained). 1 done, 0 deferred (its damage queue is
// not empty: the next tick), -1 not possible (the checks, not a MULE, a fault).
namespace {
using FnOutcome = void (*)(std::uintptr_t actor, std::int32_t k);
bool CallOutcome(std::uintptr_t actor, std::int32_t k) noexcept {
    __try {
        reinterpret_cast<FnOutcome>(A(kOutcomeSet))(actor, k);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace
bool CarryReady() noexcept { return g_installed && (g_groups & kGCarry) && (g_groups & kGOutcome) && !g_stop.load(std::memory_order_relaxed); }
int SetOutcome(std::uintptr_t actor, std::uint32_t id, int k) noexcept {
    if (!CarryReady() || !actor || k < 0 || k > 2 || !SameMule(actor, id)) return -1;
    // never twice, never over the host's own outcome (spec 6.2; review rv-correctness R10, rv-gameplay 9)
    if (erules::OutcomeOf(U32(actor + kActMule), U32(actor + kActDown)) != erules::Defeat::none) return -2;
    std::int32_t st = 0;
    if (!mem::Rd(actor + kActState, st)) return -1;
    if (st == 2) {
        const std::uintptr_t dmg = mem::Q(actor + 0xE90);
        if (dmg) {
            std::int32_t q = 0;
            if (mem::Q(dmg) != A(kMuleDmgVt) || !mem::Rd(dmg + 0x50, q)) return -1;
            if (q != 0) return 0;  // a hit of this frame still queued: the setter's drain would run it now
        }
    }
    return CallOutcome(actor, k) ? 1 : -1;
}
int OutcomeNow(std::uintptr_t actor, std::uint32_t id) noexcept {
    if (!actor || !SameMule(actor, id)) return -1;
    return static_cast<int>(erules::OutcomeOf(U32(actor + kActMule), U32(actor + kActDown)));
}

bool CaptureOurRounds() noexcept { return g_capture.load(std::memory_order_relaxed) && g_g2.load(std::memory_order_relaxed) && !g_stop.load(std::memory_order_relaxed); }
bool IsOnHitReceiver(std::uintptr_t receiver) noexcept {
    std::uint8_t ent = 0;
    if (!receiver || !mem::Rd(receiver + 8, ent) || !ent) return false;
    return mem::Q(mem::Q(receiver) + 0x10) == A(kOnHit);
}
void MarkBullet(bool on) noexcept { t_bullet = on ? 1 : 0; }
void CountBullet(bool passed) noexcept { (passed ? g_b_pass : g_b_noent).fetch_add(1, std::memory_order_relaxed); }

std::uint8_t HostDefeat(std::uint32_t id) noexcept {
    const auto it = g_hout.find(id);
    return it != g_hout.end() && it->second.have ? it->second.defeat : 0;
}
std::uint8_t GuestDefeat(std::uint32_t id) noexcept {
    const auto it = g_gout.find(id);
    if (it == g_gout.end()) return 0;
    if (it->second.sen_dead) return static_cast<std::uint8_t>(erules::Defeat::dead);  // SEN1 said dead (only a revive OUTCOME clears it)
    return it->second.rec.have ? it->second.rec.defeat : 0;
}
void NoteSenDefeat(std::uint32_t id, std::uint8_t defeat) noexcept {
    if (!g_installed || defeat != static_cast<std::uint8_t>(erules::Defeat::dead)) return;
    GuestRec& g = g_gout[id];
    if (!g.sen_dead) g.sen_dead = true;
}
std::uint32_t LastHitId() noexcept { return g_last_hit_id; }
bool DrainsCombat() noexcept { return g_installed && !g_stop.load(std::memory_order_relaxed); }
bool Vitals(std::uintptr_t entity, float* health, float* stamina) noexcept {
    const std::uintptr_t comp = DamageComponent(entity);
    if (!comp) return false;
    const float h = F32(comp + kCompHealth), s = F32(comp + kCompStamina);
    if (!std::isfinite(h) || !std::isfinite(s)) return false;
    if (health) *health = h;
    if (stamina) *stamina = s;
    return true;
}

void Status() noexcept {
    if (!g_installed) {
        Log("[enemy] hits: not installed (enemy_hits=%d)", g_cfg.enemy_hits ? 1 : 0);
        return;
    }
    Log("[enemy] hits on mirrors at OnHit %u (ours %u -> HIT %u; others %u), at the MULE handler %u (ours %u), passed from the bullet %u, ours on a non-entity part %u, "
        "deduped %u, lethal refused %u, far %u, ring full %u; sent %u (hits %s, off-dropped %u, not an attack (her aim) %u, unreadable ctx %u, values out of range "
        "%u, stale %u, capped %u); HITRES rx %u (applied %u, refused %u)",
        g_a_mirror.load(), g_a_ours.load(), g_a_capt.load(), g_a_others.load(), g_m_mirror.load(), g_m_ours.load(), g_b_pass.load(), g_b_noent.load(), g_deduped,
        g_lethal_refused, g_far, g_ring_full.load(), g_sent, g_hits_on ? "on" : "off", g_off_dropped, g_a_noattack.load() + g_bad_attack, g_a_ctx_bad.load(),
        g_bad_values, g_stale_dropped, g_out_capped, g_res_rx, g_res_by[0], g_res_rx - g_res_by[0]);
    Log("[enemy] hits: rx %u applied %u refused {off %u, dead %u, unrealized %u, world %u, rate %u, far %u, fault %u, receiver %u, attack %u, hidden %u, lethal %u, "
        "values %u}; outcomes %zu tracked (epoch %u); bad messages %u, wrong role %u%s",
        g_rx, g_applied, g_refused_by[1], g_refused_by[2], g_refused_by[3], g_refused_by[4], g_refused_by[5], g_refused_by[6], g_refused_by[7], g_refused_by[8],
        g_refused_by[9], g_refused_by[10], g_refused_by[11], g_refused_by[12], g_role == 1 ? g_hout.size() : g_gout.size(), g_role == 1 ? g_oc_epoch : g_seen_epoch,
        g_bad_msg, g_wrong_role, g_stop.load() ? "; STOPPED" : "");
    if (g_relay_on || g_relay_in) Log("[enemy] hits relay (P1c): %s; relayed in n=%u applied %u, not owned %u; carry %s", g_relay_on ? "the host relays" : "in", g_relay_in,
                                      g_relay_applied, g_relay_not_owned, CarryReady() ? "ready" : "off");
    Log("[enemy] restrain relay (round 23): %s (enemy_restrain_relay=%d, peer part %d, carry %s); watches %u (open %zu), sent %u (native %u, fallback %u), held HITs "
        "released %u, closed %u, swallowed %u; answers %u (refused %u); as owner: rx %u applied %u refused %u; holds pending %zu",
        g_restrain_send ? "on" : "off", g_cfg.enemy_restrain_relay ? 1 : 0, shots::PeerRestrain() ? 1 : 0, CarryReady() ? "ready" : "off", g_r_watches, g_rwatch.size(),
        g_r_sent, g_r_native, g_r_fallback, g_r_released, g_r_closed, g_r_swallowed, g_r_answers, g_r_answer_refused, g_r_rx, g_r_applied, g_r_refused, g_rsent.size());
}
void OutcomeTable() noexcept {
    unsigned n = 0;
    for (const auto& kv : g_gout) {
        // her own copy's vitals (a frozen mirror: the host decides, so they never change here; the gate checks them)
        char mine[96] = "; her copy: not a mirror now";
        std::uintptr_t actor = 0, entity = 0;
        float mh = 0, ms = 0;
        if (esync::MirrorOf(kv.first, &actor, &entity) && Vitals(entity, &mh, &ms))
            std::snprintf(mine, sizeof(mine), "; her copy: health %.1f stamina %.1f", static_cast<double>(mh), static_cast<double>(ms));
        Log("[enemy] outcome id=%05X %s ver %u (cause %s) state %u bits 0x%X health %.1f stamina %.1f last HIT #%u%s%s", kv.first,
            erules::DefeatText(GuestDefeat(kv.first)), kv.second.rec.ver, erules::CauseText(kv.second.cause), kv.second.state, kv.second.downbits,
            static_cast<double>(kv.second.health), static_cast<double>(kv.second.stamina), kv.second.last_seq, kv.second.sen_dead ? " (SEN1: dead)" : "", mine);
        ++n;
    }
    for (const auto& kv : g_hout) {
        if (!kv.second.have) continue;
        Log("[enemy] outcome id=%05X %s ver %u state %u bits 0x%X health %.1f stamina %.1f last HIT #%u (host)", kv.first, erules::DefeatText(kv.second.defeat),
            kv.second.ver, kv.second.state, kv.second.ea4, static_cast<double>(kv.second.health), static_cast<double>(kv.second.stamina), kv.second.last_seq);
        ++n;
    }
    Log("[enemy] outcome table: %u", n);
}

// ---------------------------------------------------------------- debug actions
namespace {
bool Hidden(char* msg, std::size_t cap) {
    if (automation::Enabled()) return true;
    std::snprintf(msg, cap, "hidden runs only");
    return false;
}
bool Ready(char* msg, std::size_t cap) {
    if (!g_installed) return std::snprintf(msg, cap, "enemy hits not installed (enemy_hits=0 or byte checks)"), false;
    if (g_stop.load()) return std::snprintf(msg, cap, "stopped after a fault"), false;
    return true;
}
// id > 0; 0: the probe's pick, else the newest HIT's enemy, else the nearest realized live MULE within 150 m of our Sam.
std::uint32_t Target(double v) {
    if (v > 0) return static_cast<std::uint32_t>(v);
    if (const std::uint32_t p = esync::PickedId()) return p;
    if (g_last_hit_id) return g_last_hit_id;
    WorldTransform sx{};
    if (!ReadXf(g_sam.load(), sx)) return 0;
    WalkActors();
    std::uint32_t best = 0;
    double bd = 150;
    for (const auto& a : g_actors) {
        MuleNow m;
        if (!ReadMule(a.second, a.first, m) || !m.realized || !m.have_xf || m.ea0 == 3 || m.ea0 == 4 || m.ea0 == 5) continue;
        const double dx = m.xf.pos.x - sx.pos.x, dy = m.xf.pos.y - sx.pos.y, dz = m.xf.pos.z - sx.pos.z;
        const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d < bd) bd = d, best = a.first;
    }
    return best;
}
void DumpBlock(int tag, std::uint32_t id, const char* what, std::uintptr_t base, std::uintptr_t from, std::uintptr_t to) {
    for (std::uintptr_t off = from; off < to; off += 32) {
        std::uint8_t b[32] = {};
        const bool ok = mem::Read(base + off, b, sizeof(b));
        char hex[32 * 3 + 1] = {};
        for (int i = 0; i < 32; ++i) std::snprintf(hex + 3 * i, 4, "%02X ", b[i]);
        Log("[enemy] dump tag=%d id=%05X %s +0x%03llX: %s%s", tag, id, what, static_cast<unsigned long long>(off), ok ? hex : "unreadable", "");
    }
}
}  // namespace

bool Dump(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!Hidden(msg, cap)) return false;
    const std::uint32_t id = Target(v[0]);
    const std::uintptr_t actor = id ? ActorOf(id) : 0;
    MuleNow m;
    if (!actor || !ReadMule(actor, id, m)) return std::snprintf(msg, cap, "no MULE"), false;
    const int tag = static_cast<int>(v[1]);
    Log("[enemy] dump tag=%d id=%05X: actor %p (state %u a88 0x%X ea0 %u ea4 0x%X ea8 0x%X), entity %p, component %p (health %.1f stamina %.1f sleep %.1f restraint "
        "%.1f, defeat %u, head part %u)",
        tag, id, reinterpret_cast<void*>(actor), m.state, m.a88, m.ea0, m.ea4, U32(actor + kActFlagsEA8), reinterpret_cast<void*>(m.entity),
        reinterpret_cast<void*>(m.comp), m.vals[0], m.vals[1], m.vals[2], m.vals[3], m.comp ? U32(m.comp + kCompDefeat) : 0, m.comp ? U32(m.comp + kCompHead) : 0);
    DumpBlock(tag, id, "actor", actor, 0, 0x1000);
    if (m.comp) DumpBlock(tag, id, "component", m.comp, 0, 0x700);
    if (m.entity) DumpBlock(tag, id, "entity", m.entity, 0x80, 0xA0);
    return std::snprintf(msg, cap, "dumped %05X", id), true;
}

bool AttackInfo(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!Hidden(msg, cap)) return false;
    const std::uint16_t attack = static_cast<std::uint16_t>(v[0]);
    const std::uintptr_t e = AttackEntry(attack);
    if (!e) return std::snprintf(msg, cap, "attack 0x%X not in the registry", attack), false;
    std::uint64_t fl = 0;
    mem::Rd(e + 0x90, fl);
    Log("[enemy] attack 0x%X: entry %p damage %.2f (+0x24) +0x28 %.2f stun %.2f (+0x2C), parts +0x30 %.2f +0x34 %.2f +0x38 %.2f +0x3C %.2f +0x40 %.2f +0x44 %.2f "
        "+0x48 %.2f +0x4C %.2f +0x50 %.2f, ranges +0x68 %.1f +0x6C %.1f, flags +0x90 0x%llX (bit6 dedupe forever %d, bit7 falloff %d, bit8 DI amount %d, bit10 "
        "instant %d): %s",
        attack, reinterpret_cast<void*>(e), F32(e + 0x24), F32(e + 0x28), F32(e + 0x2C), F32(e + 0x30), F32(e + 0x34), F32(e + 0x38), F32(e + 0x3C), F32(e + 0x40),
        F32(e + 0x44), F32(e + 0x48), F32(e + 0x4C), F32(e + 0x50), F32(e + 0x68), F32(e + 0x6C), static_cast<unsigned long long>(fl), static_cast<int>((fl >> 6) & 1),
        static_cast<int>((fl >> 7) & 1), static_cast<int>((fl >> 8) & 1), static_cast<int>((fl >> 10) & 1), F32(e + 0x24) > 0 ? "lethal" : "non-lethal");
    return std::snprintf(msg, cap, "attack 0x%X", attack), true;
}

bool CtxTest(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!Hidden(msg, cap) || !Ready(msg, cap)) return false;
    if (!(g_groups & kGApply)) return std::snprintf(msg, cap, "the apply checks failed"), false;
    const std::uint16_t attack = static_cast<std::uint16_t>(v[0]);
    const int mode = static_cast<int>(v[1]);
    bool fell = false;
    const std::uintptr_t owner = OwnerFor(mode == 0 ? 1 : mode, &fell);
    if ((mode == 0 || mode == 1) && fell) return std::snprintf(msg, cap, "no partner body (real_spawn first)"), false;
    std::uintptr_t ctx = 0, P = 0;
    __try {
        ctx = reinterpret_cast<FnCtxCreate>(A(kCtxCreate))(owner, attack);
        P = ctx ? reinterpret_cast<FnCtxParams>(A(kCtxParams))(ctx) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        HitsFault("enemy_ctx");
        return std::snprintf(msg, cap, "FAULT"), false;
    }
    if (!ctx || P != ctx + 0x120) return std::snprintf(msg, cap, "no context (ctx %p P %p)", reinterpret_cast<void*>(ctx), reinterpret_cast<void*>(P)), false;
    const std::uintptr_t res = mem::Q(P + 0x18);
    float power = 0;
    mem::Rd(P + 0x64, power);
    const char* n = mem::RttiName(mem::RttiOf(ctx));
    std::uint32_t reg = 0xFFFFFFFFu;
    RegistryPlayerId(&reg);
    Log("[enemy] ctx attack 0x%X owner %s (entity %p): ctx %p %s vt %p, type %s; P+0x08 serial 0x%X, +0x0C attack 0x%X, +0x0E %u, +0x10 %u, +0x18 resource %p (its attack "
        "0x%X), +0x20 attacker id 0x%X, +0x60 %u, +0x64 power %.3f, +0x68 %u; Q(ctx+0x68) %p, Q(ctx+0xE8) %p; the registry's player id 0x%X (%s)",
        attack, mode == 3 ? "sam" : mode == 2 ? "none" : "body", reinterpret_cast<void*>(owner), reinterpret_cast<void*>(ctx), n ? n : "?",
        reinterpret_cast<void*>(mem::Q(ctx)), CtxTypeOk(ctx) ? "ok" : "NOT the context type", U32(P + 0x08), U16(P + 0x0C), static_cast<unsigned>(U16(P + 0x0E) & 0xFF),
        U32(P + 0x10), reinterpret_cast<void*>(res), res ? U16(res + 0x20) : 0, U32(P + 0x20), static_cast<unsigned>(U16(P + 0x60) & 0xFF), static_cast<double>(power),
        U32(P + 0x68), reinterpret_cast<void*>(mem::Q(ctx + 0x68)), reinterpret_cast<void*>(mem::Q(ctx + 0xE8)), reg,
        U32(P + 0x20) == reg ? "equal" : "differ");
    return std::snprintf(msg, cap, "ctx %p attacker id 0x%X", reinterpret_cast<void*>(ctx), U32(P + 0x20)), true;
}

bool HitMode(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!Hidden(msg, cap)) return false;
    g_mode_part = static_cast<int>(v[0]);
    g_mode_owner = v[1] >= 0 && v[1] <= 3 ? static_cast<int>(v[1]) : -1;
    g_mode_path = v[2] == 0 || v[2] == 1 ? static_cast<int>(v[2]) : -1;
    Log("[enemy] hit mode: part %s, owner %s, path %s", g_mode_part == -2 ? "head" : g_mode_part == -3 ? "chest" : g_mode_part == -1 ? "none" : "an index",
        g_mode_owner >= 0 ? cfgval::HitOwnerName(g_mode_owner) : "the session's", g_mode_path < 0 ? "the session's" : g_mode_path ? "query" : "direct");
    return std::snprintf(msg, cap, "part %d owner %d path %d", g_mode_part, g_mode_owner, g_mode_path), true;
}

bool HitTest(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!Hidden(msg, cap) || !Ready(msg, cap)) return false;
    if (g_role == 2) return std::snprintf(msg, cap, "the guest sends (enemy_hit_send)"), false;
    const std::uint32_t id = Target(v[2]);
    if (!id) return std::snprintf(msg, cap, "no MULE"), false;
    g_test = TestRun{};
    // attack 0: the newest attack the watch saw on its enemy (her own rifle's), else 0x47
    g_test.attack = v[0] >= 1 ? static_cast<std::uint16_t>(v[0]) : g_watch_attack.load() ? g_watch_attack.load() : std::uint16_t{0x47};
    g_test.left = v[1] >= 1 ? static_cast<int>(v[1]) : 1;
    g_test.id = id;
    g_test.next = 0;
    return std::snprintf(msg, cap, "%d hit(s) of 0x%X on %05X", g_test.left, g_test.attack, id), true;
}

bool HitSend(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!Hidden(msg, cap) || !Ready(msg, cap)) return false;
    // the guest's (at a mirror); P1c: also the host's while it relays (at its follower of one she owns: the relay's test)
    if (g_role != 2 && !(g_role == 1 && g_relay_on)) return std::snprintf(msg, cap, "the guest's (the host's only while it relays: enemy_owner)"), false;
    if (!g_hits_on) return std::snprintf(msg, cap, "hits are off"), false;
    std::uint32_t id = v[2] > 0 ? static_cast<std::uint32_t>(v[2]) : esync::LookId();
    if (!id || !esync::MirrorOf(id, nullptr, nullptr)) id = g_last_hit_id;
    if (!id || !esync::MirrorOf(id, nullptr, nullptr)) return std::snprintf(msg, cap, "no live mirror (enemy_look first)"), false;
    g_test = TestRun{};
    g_test.attack = static_cast<std::uint16_t>(v[0]);
    g_test.left = v[1] >= 1 ? static_cast<int>(v[1]) : 1;
    g_test.id = id;
    g_test.send = true;
    return std::snprintf(msg, cap, "%d test HIT(s) of 0x%X on %05X", g_test.left, g_test.attack, id), true;
}

// enemy_restrain <id | 0> <mode> (round 23, hidden runs; the guest, or the host while it relays): the tie-up relay's own send path
// on our frozen copy. 0 native: the outcome setter k 2 on OUR copy, then the watch reads it like a real tie-up (bit 21) and sends
// kind 5; 2 the same with k 0 (a knockout, bit 19); 1 fallback: a synthetic strand HIT (attack 0x15C, weapon 41) through the
// capture path: held, watched 1.5 s, then sent as a test (depth 3) when the copy shows nothing.
bool Restrain(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!Hidden(msg, cap) || !Ready(msg, cap)) return false;
    if (g_role != 2 && !(g_role == 1 && g_relay_on)) return std::snprintf(msg, cap, "the guest's (the host's only while it relays: enemy_owner)"), false;
    if (!g_restrain_send) return std::snprintf(msg, cap, "the restrain relay is off (enemy_restrain_relay, hits, the partner's restrain part)"), false;
    const int mode = static_cast<int>(v[1]);
    if (mode < 0 || mode > 2) return std::snprintf(msg, cap, "mode 0 native | 1 fallback | 2 knockout"), false;
    std::uint32_t id = v[0] > 0 ? static_cast<std::uint32_t>(v[0]) : esync::LookId();
    std::uintptr_t actor = 0, entity = 0;
    if (!id || !esync::MirrorOf(id, &actor, &entity)) {
        // the nearest frozen copy to our Sam
        id = 0;
        WorldTransform sx{};
        if (ReadXf(g_sam.load(), sx)) {
            std::uint32_t ids[64];
            std::uintptr_t actors[64], ents[64];
            const unsigned n = esync::FrozenCopies(ids, actors, ents, 64);
            double bd = 1e18;
            for (unsigned i = 0; i < n; ++i) {
                WorldTransform x{};
                if (!ReadXf(ents[i], x)) continue;
                const double dx = x.pos.x - sx.pos.x, dy = x.pos.y - sx.pos.y, dz = x.pos.z - sx.pos.z;
                const double d = dx * dx + dy * dy + dz * dz;
                if (d < bd) bd = d, id = ids[i], actor = actors[i], entity = ents[i];
            }
        }
    }
    if (!id) return std::snprintf(msg, cap, "no frozen copy here"), false;
    if (g_rwatch.count(id) || RestrainPending(id)) return std::snprintf(msg, cap, "%05X: a restrain on it is open already", id), false;
    if (mode == 1) {
        MuleNow m;
        if (!ReadMule(actor, id, m) || !m.have_xf) return std::snprintf(msg, cap, "%05X unreadable", id), false;
        Cap c;
        static std::uint32_t serial = 0x7E000000u;
        c.key = id, c.kind = ewire::kKindJob, c.attack = erules::kAttackStrand, c.b12E = erules::kWeaponStrand, c.v184 = 0x3F800000u, c.part = 0xFFFFFFFFu;
        c.serial = ++serial, c.entity = entity, c.flags = ewire::kHitAttackerSam;
        const double root[3] = {m.xf.pos.x, m.xf.pos.y, m.xf.pos.z}, local[3] = {0, 0, 1.2};
        c.have_point = true;
        erules::ToWorld(root, YawOf(m.xf), local, c.point);
        g_restrain_test_next = true;
        GuestCapture(c, g_hits_on);
        g_restrain_test_next = false;
        Log("[enemy] enemy_restrain id=%05X: a synthetic strand HIT (attack 0x15C, weapon 41) through the capture path (fallback test)", id);
        return std::snprintf(msg, cap, "%05X: synthetic strand HIT (fallback test)", id), g_rwatch.count(id) > 0;
    }
    const std::uint8_t k = mode == 2 ? erules::kFaintK : erules::kRestrainK;
    RWatch w;
    w.t0 = g_now, w.copy_was = CopyDefeat(actor, id), w.test = true;
    if (erules::DownDefeat(w.copy_was)) return std::snprintf(msg, cap, "%05X: our copy is %s already", id, erules::DefeatText(w.copy_was)), false;
    const int rc = SetOutcome(actor, id, k);
    if (rc < 0) return std::snprintf(msg, cap, "%05X: the outcome setter on our copy: %s", id, rc == -2 ? "down already" : "not possible"), false;
    g_rwatch[id] = w;
    ++g_r_watches;
    esync::HoldCopy(id, erules::kRestrainHoldMs, "enemy_restrain");
    Log("[enemy] enemy_restrain id=%05X: the outcome setter k %u on our copy (%s): the watch reads it next [enemy_restrain test]", id, k, rc == 1 ? "called" : "deferred");
    return std::snprintf(msg, cap, "%05X: k %u on our copy, watched", id, k), true;
}

bool HitWatch(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!Hidden(msg, cap) || !Ready(msg, cap)) return false;
    if (v[1] == 0) {
        g_watch.store(0), g_watch_id.store(0);
        Log("[enemy] onhit watch off");
        return std::snprintf(msg, cap, "watch off"), true;
    }
    const std::uint32_t id = Target(v[0]);
    const std::uintptr_t actor = id ? ActorOf(id) : 0;
    MuleNow m;
    if (!actor || !ReadMule(actor, id, m) || !m.realized) return std::snprintf(msg, cap, "no realized MULE"), false;
    g_watch_logged.store(0);
    g_watch_id.store(id);
    g_watch.store(m.entity);
    Log("[enemy] onhit watch on id=%05X (entity %p, component %p: health %.1f stamina %.1f; our player id 0x%X)", id, reinterpret_cast<void*>(m.entity),
        reinterpret_cast<void*>(m.comp), m.vals[0], m.vals[1], g_player_id.load());
    return std::snprintf(msg, cap, "watching %05X", id), true;
}

// ---------------------------------------------------------------- the target probe (round 20 M1; hidden runs only, the host)
// Can a calm MULE be made to go for her partner body? enemy_target reads the AI fields of her body, our Sam and a MULE, and
// pulls one lever at a time (L1 the alert export, L3 the MULE's phase; L2 is enemy_hit 0x180 with the body as owner); after
// each lever the MULE's facing against her body and our Sam is logged once a second (docs/enemy-sync.md 11).
namespace {
constexpr std::uintptr_t kAlertPerfect = 0x1434EF1D0;      // AlertForEntityWithPerfectThreatInfo's export thunk (jmp 0x1434D5A80)
constexpr std::uintptr_t kAlertPerfectBody = 0x1434D5A80;  // (AIIndividualComponent, entity): owner +0x88 bit 7 refuses; a null target +0x170 is a no-op
constexpr std::uintptr_t kAlertPlain = 0x1434EF1E0;        // AlertForEntity's export thunk (jmp 0x1434D59D0)
constexpr std::uintptr_t kAlertPlainBody = 0x1434D59D0;
constexpr std::uintptr_t kIsHostile = 0x142562F10;         // AIFaction IsHostileTowards (a, b): byte a+0x6C+[b+0x28]
constexpr std::uintptr_t kGetFaction = 0x142362980;        // Entity GetFaction: entity+0x178 (checked only)
constexpr std::uintptr_t kRisePhase = 0x142ACE9D0;         // a MULE phase object's raise: +0x110 = max(+0x110, phase)
constexpr std::uintptr_t kEAgent = 0x170, kEFaction = 0x178, kPhaseObj = 0x58, kPhaseNow = 0x110;
enum TGroup : unsigned { kTPerfect = 1, kTPlain = 2, kTAgent = 4, kTHostile = 8, kTPhase = 16 };
// clang-format off
constexpr Site kTargetSites[] = {
    {kAlertPerfect,     "E9 AB 68 FE FF", "AlertForEntityWithPerfectThreatInfo's thunk", kTPerfect},
    {kAlertPerfectBody, "48 83 EC 38 48 85 C9 0F 84 92 00 00 00 48 8B 41 48 4C 8B 80 88 00 00 00 41 C0 E8 07 41 F6 C0 01", "the alert (perfect)", kTPerfect},
    {0x1434D5AAC,       "48 8B 71 50", "the alert's AI individual (+0x50)", kTPerfect},
    {0x1434D5AEF,       "41 B1 01", "the alert's perfect flag", kTPerfect},
    {kAlertPlain,       "E9 EB 67 FE FF", "AlertForEntity's thunk", kTPlain},
    {kAlertPlainBody,   "48 83 EC 38 48 85 C9 0F 84 92 00 00 00 48 8B 41 48 4C 8B 80 88 00 00 00 41 C0 E8 07 41 F6 C0 01", "the alert (plain)", kTPlain},
    {0x142A919EA,       "48 8B BF 70 01 00 00", "the alert's target agent (entity+0x170)", kTAgent},
    {kIsHostile,        "48 85 C9 74 0F 48 85 D2 74 0A 48 63 42 28 0F B6 44 08 6C C3", "AIFaction IsHostileTowards", kTHostile},
    {kGetFaction,       "48 85 C9 74 08 48 8B 81 78 01 00 00 C3", "Entity GetFaction (+0x178)", kTHostile},
    {kRisePhase,        "8B 81 10 01 00 00 3B D0 44 8B C0 44 0F 4F C2 44 3B C0 74 0A C7 81 18 01 00 00 00 00 80 3F", "RisePhase", kTPhase},
    {0x142B67C70,       "48 8B 4F 40 48 8B 79 58", "the phase object (DSMuleComponent+0x58)", kTPhase},
    {0x142B3EA90,       "48 89 47 40", "the MULE AI's DSMuleComponent (+0x40)", kTPhase},
};
// clang-format on
bool g_tchecked = false;
unsigned g_tgroups = 0;
void TargetChecks() {
    if (g_tchecked) return;
    g_tchecked = true;
    unsigned bad = 0, n = 0;
    for (const Site& s : kTargetSites) {
        ++n;
        if (!mem::CodeMatches(A(s.va), s.sig)) bad |= s.group, Log("[enemy] target probe: byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
    }
    if (mem::RipTarget(A(kAlertPerfect), 1, 5) != A(kAlertPerfectBody)) bad |= kTPerfect, Log("[enemy] target probe: byte check FAILED: the perfect thunk's target");
    if (mem::RipTarget(A(kAlertPlain), 1, 5) != A(kAlertPlainBody)) bad |= kTPlain, Log("[enemy] target probe: byte check FAILED: the plain thunk's target");
    g_tgroups = (kTPerfect | kTPlain | kTAgent | kTHostile | kTPhase) & ~bad;
    Log("[enemy] target probe: byte checks (%u sites): alert perfect %s, alert %s, agent %s, hostility %s, phase %s", n, (bad & kTPerfect) ? "FAILED" : "ok",
        (bad & kTPlain) ? "FAILED" : "ok", (bad & kTAgent) ? "FAILED" : "ok", (bad & kTHostile) ? "FAILED" : "ok", (bad & kTPhase) ? "FAILED" : "ok");
}
const char* NameOf(std::uintptr_t o) noexcept {
    const char* n = o ? mem::RttiName(mem::RttiOf(o)) : nullptr;
    return n ? n : (o ? "?" : "-");
}
struct AiView {
    std::uintptr_t e = 0, agent = 0, faction = 0;
    std::int32_t findex = -1;
    bool faction_ok = false;
};
AiView ReadAi(std::uintptr_t e) noexcept {
    AiView v;
    v.e = e;
    if (!e) return v;
    v.agent = mem::Q(e + kEAgent);
    v.faction = mem::Q(e + kEFaction);
    if (v.faction) {
        v.faction_ok = mem::DerivesFrom(mem::RttiOf(v.faction), "AIFaction");
        mem::Rd(v.faction + 0x28, v.findex);
    }
    return v;
}
void LogAi(const char* who, const AiView& v) noexcept {
    Log("[enemy] target read %s: entity %p %s; +0x170 (AI agent) %p %s; +0x178 (faction) %p %s%s index %d", who, reinterpret_cast<void*>(v.e), NameOf(v.e),
        reinterpret_cast<void*>(v.agent), NameOf(v.agent), reinterpret_cast<void*>(v.faction), NameOf(v.faction), v.faction && !v.faction_ok ? " (NOT an AIFaction)" : "",
        static_cast<int>(v.findex));
}
// IsHostileTowards(a's faction, b's): 1 / 0, -1 unknown (no faction, checks failed), -2 a fault
int Hostile(const AiView& a, const AiView& b) noexcept {
    if (!(g_tgroups & kTHostile) || !a.faction_ok || !b.faction_ok) return -1;
    __try {
        return reinterpret_cast<bool (*)(std::uintptr_t, std::uintptr_t)>(A(kIsHostile))(a.faction, b.faction) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}
const char* HostileText(int h) noexcept { return h == 1 ? "hostile" : h == 0 ? "not hostile" : h == -2 ? "FAULT" : "unknown"; }
std::uintptr_t BodyEntity() noexcept {
    game::Entity* b = g_body ? g_body : realbody::LiveEntity();
    return reinterpret_cast<std::uintptr_t>(b);
}
std::uintptr_t CompOf(std::uintptr_t e, const char* name) noexcept { return e ? partner::FindComponentByName(reinterpret_cast<game::Entity*>(e), name) : 0; }
std::uintptr_t PhaseObj(std::uintptr_t mule_entity) noexcept {
    const std::uintptr_t mc = CompOf(mule_entity, "DSMuleComponent");
    return mc ? mem::Q(mc + kPhaseObj) : 0;
}
int SehAlert(bool perfect, std::uintptr_t comp, std::uintptr_t target) noexcept {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, std::uintptr_t)>(A(perfect ? kAlertPerfect : kAlertPlain))(comp, target);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
int SehRise(std::uintptr_t obj, int phase) noexcept {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, int)>(A(kRisePhase))(obj, phase);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// ---- round 20 M2: a visual stimulus for her body (the AI's own perception). Our Sam is seen through his
// AIVisualStimulusComponent: at activation (0x14242CDF0) it creates a persistent AIVisualStimulus in the AI manager's visual
// pool ([0x147BE0D38] + 0x80) with its resource (component+0x30 -> +0x20), its entity's AI agent (+0x170) as the source and the
// agent's place (0x14252D970), and keeps it at component+0x50. Her body (a DSControlledEntity) has an agent but no such
// stimulus, so the sensors never see her. The probe makes one for her agent with Sam's resource (docs/enemy-sync.md 12).
constexpr std::uintptr_t kStimMgr = 0x147BE0D38;     // the AI manager (stimuli): +0x80 the visual pool
constexpr std::uintptr_t kVisCreate = 0x1425249A0;   // (pool, resource, const pos[3] f64*, const vel[4] f32*, agent) -> stimulus
constexpr std::uintptr_t kStimRemove = 0x14254CE00;  // (pool, stimulus): a persistent one back to its pool (by its type)
constexpr std::uintptr_t kAgentPos = 0x14252D970;    // (agent, out[3] f64) -> out: agent vt+0xF8
constexpr std::uintptr_t kVisActivate = 0x14242CDF0; // AIVisualStimulusComponent's activation (checked only)
constexpr std::uintptr_t kCVisRes = 0x30, kCVisStim = 0x50, kCVisMsg = 0x60, kRVis = 0x20;
constexpr std::uintptr_t kSRes = 0x08, kSAgent = 0x10, kSPos = 0x18, kSLife = 0x80, kSOff = 0x84, kSVel = 0x90, kSMul = 0xA4;
enum SGroup : unsigned { kSCreate = 1 };
// clang-format off
constexpr Site kStimSites[] = {
    {kVisCreate,          "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 40 81 B9 10 A0 01 00 00 02 00 00", "the visual stimulus create", kSCreate},
    {kStimRemove,         "48 89 74 24 20 57 48 83 EC 30 80 BA 84 00 00 00 00 48 8B FA 48 8B F1", "the persistent stimulus remove", kSCreate},
    {kAgentPos,           "40 53 48 83 EC 60 48 8B 01 48 8B DA 48 8D 54 24 20 FF 90 F8 00 00 00", "the agent's place", kSCreate},
    {kVisActivate + 0x0E, "48 8B 69 30", "the component's resource (+0x30)", kSCreate},
    {0x14242CEE4,         "4C 8B 35 4D 3E 7B 05", "the AI manager global", kSCreate},
    {0x14242CF31,         "48 8B 5D 20", "the visual resource (+0x20)", kSCreate},
    {0x14242CF69,         "49 8D 8E 80 00 00 00 E8 2B 7A 0F 00", "the visual pool (+0x80) and the create", kSCreate},
    {0x14242CF7D,         "48 89 47 50", "the component's stimulus (+0x50)", kSCreate},
    {0x142516680,         "48 89 5C 24 08 57 48 83 EC 20 48 8D 59 68 48 8B FA", "the stimulus message add", kSCreate},
};
// clang-format on
bool g_schecked = false;
unsigned g_sgroups = 0;
void StimChecks() {
    if (g_schecked) return;
    g_schecked = true;
    unsigned bad = 0, n = 0;
    for (const Site& s : kStimSites) {
        ++n;
        if (!mem::CodeMatches(A(s.va), s.sig)) bad |= s.group, Log("[enemy] stimulus: byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
    }
    if (mem::RipTarget(A(0x14242CEE4), 3, 7) != A(kStimMgr)) bad |= kSCreate, Log("[enemy] stimulus: byte check FAILED: the AI manager global's target");
    g_sgroups = kSCreate & ~bad;
    Log("[enemy] stimulus: byte checks (%u sites): %s", n + 1, (bad & kSCreate) ? "FAILED" : "ok");
}
struct HerStim {
    std::uintptr_t stim = 0, body = 0, agent = 0, res = 0;
    int mode = 0;          // 1 her agent, 2 her agent + Sam's component message, 3 the decoy (Sam's agent at her place)
    bool write_pos = false;  // our tick writes her place into it (when the game does not move it)
    double t0 = 0;
} g_hs;
std::uintptr_t VisComp(std::uintptr_t e) noexcept { return CompOf(e, "AIVisualStimulusComponent"); }
void LogStim(const char* who, std::uintptr_t s) noexcept {
    if (!s) return Log("[enemy] stimulus %s: none", who);
    double p[3] = {};
    mem::Rd(s + kSPos, p);
    const std::uintptr_t res = mem::Q(s + kSRes), ag = mem::Q(s + kSAgent);
    float life = -2.f, vis = 0.f, mul = 0.f;
    mem::Rd(s + kSLife, life);
    mem::Rd(res + 0x40, vis);
    mem::Rd(s + kSMul, mul);
    Log("[enemy] stimulus %s: %p %s; resource %p %s (lifetime %.2f, +0x40 %.3f); agent %p %s; at (%.2f, %.2f, %.2f); life %.2f off %u; +0xA4 %.3f; "
        "messages %d",
        who, reinterpret_cast<void*>(s), NameOf(s), reinterpret_cast<void*>(res), NameOf(res), res ? static_cast<double>(F32(res + 0x2C)) : -9.0,
        static_cast<double>(vis), reinterpret_cast<void*>(ag), NameOf(ag), p[0], p[1], p[2], static_cast<double>(life), static_cast<unsigned>(U8(s + kSOff)),
        static_cast<double>(mul), static_cast<int>(U32(s + 0x68)));
}
void LogVisComp(const char* who, std::uintptr_t e) noexcept {
    const std::uintptr_t c = VisComp(e);
    if (!c) return Log("[enemy] stimulus %s: no AIVisualStimulusComponent (entity %p %s)", who, reinterpret_cast<void*>(e), NameOf(e));
    const std::uintptr_t cres = mem::Q(c + kCVisRes), msg = mem::Q(c + kCVisMsg);
    Log("[enemy] stimulus %s: AIVisualStimulusComponent %p (entity +0x48 %p, resource %p %s -> visual %p, aural %p; +0x3C %u; message +0x60 %p %s)", who,
        reinterpret_cast<void*>(c), reinterpret_cast<void*>(mem::Q(c + 0x48)), reinterpret_cast<void*>(cres), NameOf(cres),
        reinterpret_cast<void*>(cres ? mem::Q(cres + kRVis) : 0), reinterpret_cast<void*>(cres ? mem::Q(cres + 0x28) : 0),
        cres ? static_cast<unsigned>(U8(cres + 0x3C)) : 0u, reinterpret_cast<void*>(msg), NameOf(msg));
    LogStim(who, mem::Q(c + kCVisStim));
}
int SehAgentPos(std::uintptr_t agent, double (&out)[3]) noexcept {
    __try {
        reinterpret_cast<void* (*)(std::uintptr_t, double*)>(A(kAgentPos))(agent, out);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
std::uintptr_t SehVisCreate(std::uintptr_t res, const double (&pos)[3], const float (&vel)[4], std::uintptr_t agent) noexcept {
    __try {
        const std::uintptr_t mgr = mem::Q(A(kStimMgr));
        if (!mgr) return 0;
        // the game holds a reference on the resource around the call (lock xadd +0x18) and drops it after: the stimulus keeps none
        return reinterpret_cast<std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, const double*, const float*, std::uintptr_t)>(A(kVisCreate))(mgr + 0x80, res, pos,
                                                                                                                                         vel, agent);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
int SehStimRemove(std::uintptr_t s) noexcept {
    __try {
        const std::uintptr_t mgr = mem::Q(A(kStimMgr));
        if (!mgr) return 0;
        reinterpret_cast<void (*)(std::uintptr_t, std::uintptr_t)>(A(kStimRemove))(mgr + 0x80, s);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
int SehWrite(std::uintptr_t dst, const void* src, std::size_t n) noexcept {
    __try {
        std::memcpy(reinterpret_cast<void*>(dst), src, n);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
int SehMsgAdd(std::uintptr_t stim, std::uintptr_t m) noexcept {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, const std::uintptr_t*)>(A(0x142516680))(stim, &m);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// (hidden runs only: the probe walks the AI manager's stimulus lists and creates / removes pool entries from the mod's game-thread
// tick without the AI's own lock: if the sensors ran on worker jobs at that moment this could race; a probe-only hazard, review
// r20 enemy 6, docs/enemy-sync.md 12)
void StimRemove(const char* why) noexcept {
    if (!g_hs.stim) return;
    const int r = SehStimRemove(g_hs.stim);
    Log("[enemy] stimulus: hers removed (%s; mode %d, %.1f s)%s", why, g_hs.mode, (g_now - g_hs.t0) / 1000.0, r < 0 ? " FAULT" : "");
    g_hs = HerStim{};
}
// no longer ours (the game reused it for another source, or it is the old world's): forgotten, never put back into the pool
// (a remove would free what the game now owns, silently: review r20 enemy 5)
void StimForget(const char* why) noexcept {
    if (!g_hs.stim) return;
    Log("[enemy] stimulus: hers forgotten (%s; mode %d, %.1f s; not removed)", why, g_hs.mode, (g_now - g_hs.t0) / 1000.0);
    g_hs = HerStim{};
}
// our Sam's live visual stimulus by its source agent (the visual list), or 0
std::uintptr_t FindVisStimOf(std::uintptr_t agent) noexcept {
    const std::uintptr_t mgr = agent ? mem::Q(A(kStimMgr)) : 0;
    if (!mgr) return 0;
    int n = 0;
    for (std::uintptr_t s = mem::Q(mgr + 0x88); s && n < 600; s = mem::Q(s + 0xC8), ++n)
        if (mem::Q(s + kSAgent) == agent) return s;
    return 0;
}
// mode 1 / 2 her agent, 3 the decoy (Sam's agent placed at her body)
bool StimCreate2(int mode, char* msg, std::size_t cap) noexcept;
bool StimCreate(int mode, char* msg, std::size_t cap) noexcept {
    const bool ok = StimCreate2(mode, msg, cap);
    if (!ok) Log("[enemy] stimulus: hers not made (mode %d): %s", mode, msg);
    return ok;
}
bool StimCreate2(int mode, char* msg, std::size_t cap) noexcept {
    if (!(g_sgroups & kSCreate)) return std::snprintf(msg, cap, "the stimulus checks failed"), false;
    // the agent offset (entity+0x170, kEAgent) this reads is the one the target probe's kTAgent site checks (review r20 enemy 9)
    if (!(g_tgroups & kTAgent)) return std::snprintf(msg, cap, "the agent offset's check failed"), false;
    StimRemove("replaced");
    const std::uintptr_t body = BodyEntity(), sam = g_sam.load();
    const std::uintptr_t sc = VisComp(sam);
    const std::uintptr_t cres = sc ? mem::Q(sc + kCVisRes) : 0;
    std::uintptr_t res = cres ? mem::Q(cres + kRVis) : 0;
    std::uintptr_t sstim = sc ? mem::Q(sc + kCVisStim) : 0;
    if (!res && sam) {
        // no component on our Sam (a DSPlayerEntity): his stimulus in the visual list, by his agent
        sstim = FindVisStimOf(mem::Q(sam + kEAgent));
        res = sstim ? mem::Q(sstim + kSRes) : 0;
    }
    if (!body || !res) return std::snprintf(msg, cap, "no body or no Sam's visual resource (no component, no stimulus by his agent)"), false;
    const std::uintptr_t her_agent = mem::Q(body + kEAgent), sam_agent = mem::Q(sam + kEAgent);
    const std::uintptr_t agent = mode == 3 ? sam_agent : her_agent;
    if (!her_agent || !agent) return std::snprintf(msg, cap, "no agent"), false;
    double pos[3] = {};
    if (SehAgentPos(her_agent, pos) < 0) return std::snprintf(msg, cap, "FAULT (agent place)"), false;
    float vel[4] = {};
    const std::uintptr_t s = SehVisCreate(res, pos, vel, agent);
    if (!s) return std::snprintf(msg, cap, "the create gave nothing (pool full or FAULT)"), false;
    float mul = 1.f;
    if (sstim) mem::Rd(sstim + kSMul, mul);
    if (!(mul > 0.f && mul <= 4.f)) mul = 1.f;
    SehWrite(s + kSMul, &mul, sizeof(mul));
    g_hs.stim = s, g_hs.body = body, g_hs.agent = agent, g_hs.res = res, g_hs.mode = mode, g_hs.t0 = g_now;
    g_hs.write_pos = mode == 3;
    if (mode == 2 && sc) {
        // Sam's component also gives his stimulus one message object (+0x60); the probe shares it (its reference taken by the add)
        const std::uintptr_t m = mem::Q(sc + kCVisMsg);
        if (m && SehMsgAdd(s, m) < 0) Log("[enemy] stimulus: the message add FAULT");
    }
    Log("[enemy] stimulus: hers made (mode %d: %s): %p, resource %p (Sam's), agent %p (%s), at (%.2f, %.2f, %.2f), +0xA4 %.3f", mode,
        mode == 3 ? "the decoy: Sam's agent at her place" : mode == 2 ? "her agent + Sam's message" : "her agent", reinterpret_cast<void*>(s),
        reinterpret_cast<void*>(res), reinterpret_cast<void*>(agent), NameOf(agent), pos[0], pos[1], pos[2], static_cast<double>(mul));
    LogStim("hers", s);
    return std::snprintf(msg, cap, "stimulus mode %d made %p", mode, reinterpret_cast<void*>(s)), true;
}
// every tick: drop it when her body or its agent changed; the decoy (and write_pos) follows her body's place
void StimTick() noexcept {
    if (!g_hs.stim) return;
    // reused first: a stimulus the game now owns is forgotten, not removed (review r20 enemy 5)
    if (mem::Q(g_hs.stim + kSAgent) != g_hs.agent || mem::Q(g_hs.stim + kSRes) != g_hs.res) return StimForget("the stimulus was reused by the game");
    const std::uintptr_t body = BodyEntity();
    if (body != g_hs.body || !body || mem::Q(body + kEAgent) == 0 || (g_hs.mode != 3 && mem::Q(body + kEAgent) != g_hs.agent)) return StimRemove("her body changed");
    if (g_hs.write_pos) {
        WorldTransform bx{};
        if (ReadXf(body, bx)) {
            const double p[3] = {bx.pos.x, bx.pos.y, bx.pos.z + 1.0};
            SehWrite(g_hs.stim + kSPos, p, sizeof(p));
        }
    }
}
// the probe's walk of the AI manager's live stimuli (visual list mgr+0x88, next +0xC8; aural list mgr+0x1A0D8, next +0xA0):
// which stimulus (if any) stands for our Sam, his agent's or within 3 m of him, and how many there are by source class
void StimWalk(const char* list, std::uintptr_t head_at, std::uintptr_t next_off) noexcept {
    const std::uintptr_t sam = g_sam.load(), body = BodyEntity();
    const std::uintptr_t sag = sam ? mem::Q(sam + kEAgent) : 0, bag = body ? mem::Q(body + kEAgent) : 0;
    WorldTransform sx{};
    const bool hs = ReadXf(sam, sx);
    struct Cls { const char* name; int n; } cls[10] = {};
    int ncls = 0, total = 0, shown = 0;
    std::uintptr_t s = mem::Q(head_at);
    for (; s && total < 600; s = mem::Q(s + next_off), ++total) {
        const std::uintptr_t ag = mem::Q(s + kSAgent);
        const char* nm = ag ? NameOf(ag) : "(no agent)";
        int k = 0;
        while (k < ncls && std::strcmp(cls[k].name, nm) != 0) ++k;
        if (k < ncls) ++cls[k].n;
        else if (ncls < 10) cls[ncls++] = {nm, 1};
        double p[3] = {};
        mem::Rd(s + kSPos, p);
        const double d = hs ? std::sqrt((p[0] - sx.pos.x) * (p[0] - sx.pos.x) + (p[1] - sx.pos.y) * (p[1] - sx.pos.y)) : 1e9;
        const bool mine = ag && (ag == sag || ag == bag || ag == sam || ag == body);
        if ((mine || d < 3.0) && shown < 8) {
            char who[64];
            std::snprintf(who, sizeof(who), "%s #%d (%s%s, %.1f m from our Sam)", list, total,
                          ag == sag ? "Sam's agent" : ag == bag ? "her agent" : ag == sam ? "Sam's entity" : ag == body ? "her entity" : "other", "", d);
            LogStim(who, s);
            ++shown;
        }
    }
    char buf[512] = {};
    std::size_t at = 0;
    for (int k = 0; k < ncls && at + 64 < sizeof(buf); ++k) at += std::snprintf(buf + at, sizeof(buf) - at, "%s%s x%d", k ? ", " : "", cls[k].name, cls[k].n);
    Log("[enemy] stimulus walk %s: %d live (Sam's agent %p, her agent %p); %d near or ours; by source: %s", list, total, reinterpret_cast<void*>(sag),
        reinterpret_cast<void*>(bag), shown, buf);
}
void StimFollowLog() noexcept {
    if (!g_hs.stim) return;
    double p[3] = {};
    mem::Rd(g_hs.stim + kSPos, p);
    WorldTransform bx{};
    const bool hb = ReadXf(g_hs.body, bx);
    Log("[enemy] stimulus hers: at (%.2f, %.2f, %.2f); her body at (%.2f, %.2f, %.2f) (%.2f m apart); +0xA4 %.3f; life %.2f off %u", p[0], p[1], p[2],
        hb ? bx.pos.x : 0.0, hb ? bx.pos.y : 0.0, hb ? bx.pos.z : 0.0,
        hb ? std::sqrt((p[0] - bx.pos.x) * (p[0] - bx.pos.x) + (p[1] - bx.pos.y) * (p[1] - bx.pos.y)) : -1.0, static_cast<double>(F32(g_hs.stim + kSMul)),
        static_cast<double>(F32(g_hs.stim + kSLife)), static_cast<unsigned>(U8(g_hs.stim + kSOff)));
    const std::uintptr_t sc = VisComp(g_sam.load());
    const std::uintptr_t ss = sc ? mem::Q(sc + kCVisStim) : 0;
    WorldTransform sx{};
    if (ss && ReadXf(g_sam.load(), sx)) {
        mem::Rd(ss + kSPos, p);
        Log("[enemy] stimulus Sam's: at (%.2f, %.2f, %.2f); our Sam at (%.2f, %.2f, %.2f); +0xA4 %.3f", p[0], p[1], p[2], sx.pos.x, sx.pos.y, sx.pos.z,
            static_cast<double>(F32(ss + kSMul)));
    }
}

// the follow: the MULE's facing against her body and our Sam, once a second
struct Follow {
    std::uint32_t id = 0;
    double t0 = 0, until = 0, next = 0;
    char why[48] = {};
} g_follow;
void StartFollow(std::uint32_t id, double secs, const char* why) noexcept {
    g_follow.id = id;
    g_follow.t0 = g_now;
    g_follow.next = g_now;
    g_follow.until = g_now + secs * 1000.0;
    std::snprintf(g_follow.why, sizeof(g_follow.why), "%s", why);
}
double Wrap(double d) noexcept {
    while (d > 180) d -= 360;
    while (d < -180) d += 360;
    return d;
}
void TargetTick() {
    if (!g_follow.id || g_now < g_follow.next) return;
    g_follow.next += 1000;
    const std::uint32_t id = g_follow.id;
    if (g_now > g_follow.until) {
        g_follow.id = 0;
        Log("[enemy] target follow id=%05X done (%s)", id, g_follow.why);
        return;
    }
    const std::uintptr_t actor = ActorOf(id);
    MuleNow m;
    if (!actor || !ReadMule(actor, id, m) || !m.realized || !m.have_xf) {
        Log("[enemy] target follow id=%05X t=%.0f s: not realized here", id, (g_now - g_follow.t0) / 1000.0);
        return;
    }
    constexpr double kDeg = 57.29577951308232;
    const double yaw = YawOf(m.xf) * kDeg;
    char body[96] = "her body: none", sam[96] = "our Sam: none";
    WorldTransform bx{}, sx{};
    if (ReadXf(BodyEntity(), bx)) {
        const double dx = bx.pos.x - m.xf.pos.x, dy = bx.pos.y - m.xf.pos.y;
        std::snprintf(body, sizeof(body), "her body %+.0f deg off its facing at %.1f m", Wrap(std::atan2(dy, dx) * kDeg - yaw), std::sqrt(dx * dx + dy * dy));
    }
    if (ReadXf(g_sam.load(), sx)) {
        const double dx = sx.pos.x - m.xf.pos.x, dy = sx.pos.y - m.xf.pos.y;
        std::snprintf(sam, sizeof(sam), "our Sam %+.0f deg off at %.1f m", Wrap(std::atan2(dy, dx) * kDeg - yaw), std::sqrt(dx * dx + dy * dy));
    }
    const std::uintptr_t po = (g_tgroups & kTPhase) ? PhaseObj(m.entity) : 0;
    Log("[enemy] target follow id=%05X t=%.0f s (%s): at (%.2f, %.2f, %.2f) facing %.0f deg; %s; %s; MULE state %u bits 0x%X a88 0x%X; phase %d; stamina %.1f",
        id, (g_now - g_follow.t0) / 1000.0, g_follow.why, m.xf.pos.x, m.xf.pos.y, m.xf.pos.z, yaw, body, sam, m.ea0, m.ea4, m.a88,
        po ? static_cast<int>(U32(po + kPhaseNow)) : -1, static_cast<double>(m.vals[1]));
    StimFollowLog();
}
}  // namespace

bool TargetProbe(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!Hidden(msg, cap) || !Ready(msg, cap)) return false;
    TargetChecks();
    const int op = static_cast<int>(v[0]);
    if (op == 4) {
        // the hits on her partner body (as a victim), logged at OnHit; they pass
        if (v[1] == 0) {
            g_watch.store(0), g_watch_id.store(0);
            Log("[enemy] target: the partner body's hit watch off");
            return std::snprintf(msg, cap, "body watch off"), true;
        }
        const std::uintptr_t b = BodyEntity();
        if (!b) return std::snprintf(msg, cap, "no partner body"), false;
        g_watch_logged.store(0);
        g_watch_id.store(0);
        g_watch.store(b);
        Log("[enemy] target: the partner body's hit watch on (entity %p %s; every DamageInfo on it logged as 'onhit watch ... id 00000')", reinterpret_cast<void*>(b),
            NameOf(b));
        return std::snprintf(msg, cap, "body watch on"), true;
    }
    if (op == 8 || op == 9) {
        // round 20 M2: the visual stimuli (8 read; 9 <0 remove | 1 her agent | 2 + Sam's message | 3 the decoy> her body's)
        StimChecks();
        if (op == 8) {
            LogVisComp("our Sam", g_sam.load());
            LogVisComp("her body", BodyEntity());
            LogStim("hers (ours)", g_hs.stim);
            if (const std::uintptr_t mgr = mem::Q(A(kStimMgr))) {
                StimWalk("visual", mgr + 0x88, 0xC8);
                StimWalk("aural", mgr + 0x1A0D8, 0xA0);
            } else {
                Log("[enemy] stimulus walk: no AI manager");
            }
            return std::snprintf(msg, cap, "stimuli read"), true;
        }
        const int mode = static_cast<int>(v[1]);
        if (mode == 0) {
            if (!g_hs.stim) return std::snprintf(msg, cap, "none"), true;
            StimRemove("enemy_target 9 0");
            return std::snprintf(msg, cap, "removed"), true;
        }
        if (mode < 1 || mode > 3) return std::snprintf(msg, cap, "9 <0 | 1 | 2 | 3>"), false;
        return StimCreate(mode, msg, cap);
    }
    const std::uint32_t id = Target(v[1]);
    const std::uintptr_t actor = id ? ActorOf(id) : 0;
    MuleNow m;
    if (!actor || !ReadMule(actor, id, m) || !m.realized) return std::snprintf(msg, cap, "no realized MULE"), false;
    const std::uintptr_t body = BodyEntity(), sam = g_sam.load();
    const std::uintptr_t comp = CompOf(m.entity, "AIIndividualComponent");
    const std::uintptr_t po = (g_tgroups & kTPhase) ? PhaseObj(m.entity) : 0;
    switch (op) {
        case 0: {
            const AiView vb = ReadAi(body), vs = ReadAi(sam), vm = ReadAi(m.entity);
            LogAi("her body", vb);
            LogAi("our Sam", vs);
            LogAi("the MULE", vm);
            Log("[enemy] target read: IsHostileTowards MULE->body %s, MULE->Sam %s, body->MULE %s, Sam->MULE %s, Sam->body %s", HostileText(Hostile(vm, vb)),
                HostileText(Hostile(vm, vs)), HostileText(Hostile(vb, vm)), HostileText(Hostile(vs, vm)), HostileText(Hostile(vs, vb)));
            const std::uintptr_t owner = comp ? mem::Q(comp + 0x48) : 0, ind = comp ? mem::Q(comp + 0x50) : 0;
            Log("[enemy] target read id=%05X: AIIndividualComponent %p %s (owner +0x48 %p: %s, individual +0x50 %p %s); her body's %p, our Sam's %p; DSMuleComponent "
                "phase object %p (phase %d, +0x114 %.1f, +0x118 %.2f); MULE state %u bits 0x%X",
                id, reinterpret_cast<void*>(comp), NameOf(comp), reinterpret_cast<void*>(owner), comp && owner == m.entity ? "the MULE" : "NOT the MULE",
                reinterpret_cast<void*>(ind), NameOf(ind), reinterpret_cast<void*>(CompOf(body, "AIIndividualComponent")),
                reinterpret_cast<void*>(CompOf(sam, "AIIndividualComponent")), reinterpret_cast<void*>(po), po ? static_cast<int>(U32(po + kPhaseNow)) : -1,
                po ? static_cast<double>(F32(po + 0x114)) : -1.0, po ? static_cast<double>(F32(po + 0x118)) : -1.0, m.ea0, m.ea4);
            return std::snprintf(msg, cap, "read %05X: body agent %p faction %p", id, reinterpret_cast<void*>(vb.agent), reinterpret_cast<void*>(vb.faction)), true;
        }
        case 1:
        case 2: {
            // L1: the alert export, the target her body (v[2] 0) or our Sam (1, the control)
            const bool perfect = op == 1;
            if (!(g_tgroups & (perfect ? kTPerfect : kTPlain))) return std::snprintf(msg, cap, "the alert's checks failed"), false;
            if (!comp || mem::Q(comp + 0x48) != m.entity) return std::snprintf(msg, cap, "no AIIndividualComponent on %05X", id), false;
            const bool to_sam = v[2] == 1;
            const std::uintptr_t t = to_sam ? sam : body;
            if (!t) return std::snprintf(msg, cap, "no %s", to_sam ? "Sam" : "partner body"), false;
            const AiView vt = ReadAi(t);
            const int r = SehAlert(perfect, comp, t);
            Log("[enemy] target L1 %s id=%05X -> %s %p (%s): its agent (+0x170) %p %s: %s; MULE state %u bits 0x%X phase %d",
                perfect ? "AlertForEntityWithPerfectThreatInfo" : "AlertForEntity", id, to_sam ? "our Sam" : "her body", reinterpret_cast<void*>(t), NameOf(t),
                reinterpret_cast<void*>(vt.agent), NameOf(vt.agent),
                r < 0 ? "FAULT" : vt.agent ? "called (a message to the MULE's AI)" : "called (a no-op: the target has no agent)", m.ea0, m.ea4,
                po ? static_cast<int>(U32(po + kPhaseNow)) : -1);
            if (r < 0) return std::snprintf(msg, cap, "FAULT"), false;
            StartFollow(id, 12, to_sam ? "L1 our Sam" : "L1 her body");
            return std::snprintf(msg, cap, "L1 %s -> %s (agent %p)", perfect ? "perfect" : "plain", to_sam ? "sam" : "body", reinterpret_cast<void*>(vt.agent)), true;
        }
        case 3: {
            // L3: the MULE's phase raised (v[2] 1..5)
            if (!po) return std::snprintf(msg, cap, "no phase object (checks or DSMuleComponent)"), false;
            const int phase = static_cast<int>(v[2]);
            if (phase < 1 || phase > 5) return std::snprintf(msg, cap, "phase 1..5"), false;
            const int before = static_cast<int>(U32(po + kPhaseNow));
            const int r = SehRise(po, phase);
            Log("[enemy] target L3 RisePhase id=%05X: phase object %p %d -> %d (asked %d), +0x114 %.1f, +0x118 %.2f%s", id, reinterpret_cast<void*>(po), before,
                static_cast<int>(U32(po + kPhaseNow)), phase, static_cast<double>(F32(po + 0x114)), static_cast<double>(F32(po + 0x118)), r < 0 ? " FAULT" : "");
            if (r < 0) return std::snprintf(msg, cap, "FAULT"), false;
            StartFollow(id, g_cfg.enemy_m2_probes ? 30 : 12, "L3 phase");  // round 25 D7: the whole P2 window with the probes on
            return std::snprintf(msg, cap, "L3 phase %d -> %d", before, static_cast<int>(U32(po + kPhaseNow))), true;
        }
        case 7:
            StartFollow(id, v[2] > 0 ? v[2] : 10, "follow");
            return std::snprintf(msg, cap, "following %05X", id), true;
        default: return std::snprintf(msg, cap, "unknown op %d (0 read, 1 / 2 L1 perfect / plain -> 0 body | 1 sam, 3 L3 phase, 4 body watch 0/1, 7 follow s, 8 stimuli read, 9 <0..3> her stimulus)", op), false;
    }
}

// ---------------------------------------------------------------- round 23 E: the M2 probes (enemy_m2; docs/enemy-sync.md 14.11)
// Design section 10 "M2 Probes" (critique R2 / R3). Hidden runs with enemy_m2_probes=1 only. Read-only (fault-free reads; game
// calls under SEH), except P2's lever (b a test hit, c the perfect-threat alert, d' Sam's own stimulus +0xA4 for 1 s; the hand-over
// itself is the scenario's enemy_own). Every line starts "[probe] P<n>"; every probe ends with one "[probe] P<n> result ..." line.
// A fault in a probe turns the probes off for the run (never the hits).
namespace {
namespace ep = eprobe;
constexpr std::uintptr_t kKnowVt = 0x143E13028;      // DSAINoticeAndKnowledge's vtable ([O] vt+0x50 = the update 0x142E8E850)
constexpr std::uintptr_t kKnowUpdate = 0x142E8E850;  // the update (reads K+0x28 -> +0x118, the buckets, ORs entry +0x78 with 2)
constexpr std::uintptr_t kKnowFactory = 0x142E8D1A0; // the RTTI factory: the ctor 0x142E936F0, the owner at +0x20
constexpr std::uintptr_t kKnowCtor = 0x142E936F0;
constexpr std::uintptr_t kKX = 0x28, kXAgent = 0x118, kKBucketN = 0x590, kKBuckets = 0x598;
constexpr std::uintptr_t kKeKey = 0x10, kKeType = 0x18, kKeSub = 0x1C, kKePos = 0x20, kKeActor = 0x68, kKeFlags = 0x78, kKeSize = 0x80;
constexpr std::uintptr_t kRay = 0x141C20CE0;         // (world, const double ray[6], result[0x40 each], max, filter, flag) -> hits
constexpr std::uintptr_t kRayWorld = 0x14578C140;    // the physics world global both senses pass
constexpr std::uintptr_t kRayFilterVt = 0x143BA8938; // the aural sense's filter vtable (+8 0x34, +0xC 0xFF, +0x20 an array)
constexpr std::uintptr_t kRayFree = 0x1419D5870;     // the filter's array free (null-safe)
constexpr std::uintptr_t kRayResInit = 0x14240B7E0;  // a result entry's init
constexpr std::uintptr_t kEPerceive = 0x314, kSClass = 0x88, kMgrTsVis = 0x3C398;
enum PGroup : unsigned { kPKnow = 1, kPRay = 2, kPSense = 4, kPStimMul = 8 };
// clang-format off
constexpr Site kProbeSites[] = {
    {kKnowFactory, "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 74 1A 48 8B C8 E8 ?? ?? ?? ?? 48 8B C3 48 89 7B 20", "the knowledge factory", kPKnow},
    {kKnowUpdate,  "48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 57 41 54 41 55 41 56 41 57 48 81 EC 00 09 00 00", "the knowledge update", kPKnow},
    {0x142E8E990,  "49 8B 46 28 48 8D 54 24 60 48 8B 88 18 01 00 00", "the update's agent (+0x28 -> +0x118)", kPKnow},
    {0x142E8EDA0,  "49 8B B6 98 05 00 00 49 63 AE 90 05 00 00", "the buckets (+0x598, +0x590)", kPKnow},
    {0x142E8EE19,  "49 8B 96 98 05 00 00 4D 63 86 90 05 00 00", "the buckets (second read)", kPKnow},
    {0x142E8EE53,  "80 48 78 02 48 83 E8 80 48 3B C1 75 F3", "the entries (0x80 each, flags +0x78)", kPKnow},
    {kRay,         "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC B0 00 00 00 48 8D 6C 24 30 48 89 9D C0 00 00 00", "the raycast", kPRay},
    {0x141C20D0D,  "4C 8B A5 E0 00 00 00", "the raycast's filter (arg 5)", kPRay},
    {0x141C20DAD,  "0F B6 85 E8 00 00 00", "the raycast's flag (arg 6)", kPRay},
    {0x141C20DB4,  "4D 8D 45 18", "the raycast's end (ray+0x18)", kPRay},
    {0x142E4F6F3,  "48 8B 0D ?? ?? ?? ?? 48 8D 45 50 C6 44 24 28 01 4C 8D 85 80 00 00 00 41 B9 01 00 00 00 48 89 44 24 20 48 8D 55 E8 E8 ?? ?? ?? ??", "the visual sense's raycast", kPRay},
    {0x142E81692,  "48 8B 0D ?? ?? ?? ?? 48 8D 45 A0 33 DB C7 45 A8 34 00 00 00 49 8D 56 10 88 5C 24 28 48 8D 3D ?? ?? ?? ?? 48 C7 45 AC FF 00 00 00 "
                   "48 03 D6 48 89 7D A0 44 8D 4B 20 48 89 5D B4 4C 8D 45 40 89 5D BC 48 89 5D C0 48 89 44 24 20 E8 ?? ?? ?? ??", "the aural sense's raycast (its filter)", kPRay},
    {0x142E8171F,  "48 8B 4D C0 48 89 7D A0 E8", "the aural sense's filter free (+0x20)", kPRay},
    {kRayFree,     "48 83 EC 28 4C 8B C1 48 85 C9 0F 84", "the filter array free", kPRay},
    {kRayResInit,  "C5 F8 10 05 38 3C 78 01 33 C0 48 89 01 48 89 41 08 48 89 41 10 48 89 41 18 C5 FA 7F 41 20 89 41 30 48 89 41 38 48 8B C1 C3", "the result init", kPRay},
    {0x142E4F314,  "8B BB 14 03 00 00", "the visual sense's +0x314", kPSense},
    {0x142E4F2F7,  "48 8D B3 18 03 00 00", "the visual sense's lock (+0x318)", kPSense},
    {0x142E4F340,  "48 8B 1D ?? ?? ?? ?? 48 8B 9B 98 C3 03 00", "the thread-safe visual list (AI manager +0x3C398)", kPSense},
    {0x142E5FA60,  "8B 9A 88 00 00 00", "the classifier's stimulus +0x88", kPSense},
    {0x142E5FA70,  "81 E3 FF FF 1F 00", "its id mask (0x1FFFFF)", kPSense},
    {0x14251D376,  "C5 FA 10 8F A4 00 00 00", "a stimulus +0xA4 read", kPStimMul},
};
// clang-format on
bool g_pchecked = false, g_probe_off = false;
unsigned g_pgroups = 0;
void ProbeChecks() {
    if (g_pchecked) return;
    g_pchecked = true;
    unsigned bad = 0, n = 0;
    for (const Site& s : kProbeSites) {
        ++n;
        if (!mem::CodeMatches(A(s.va), s.sig)) bad |= s.group, Log("[probe] byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
    }
    const struct {
        bool okv;
        unsigned group;
        const char* name;
    } data[] = {
        {mem::Q(A(kKnowVt) + 0x50) == A(kKnowUpdate), kPKnow, "the knowledge vtable's update (vt+0x50)"},
        {mem::RipTarget(A(0x142E8D1C4), 1, 5) == A(kKnowCtor), kPKnow, "the factory's ctor"},
        {mem::RipTarget(A(0x142E4F719), 1, 5) == A(kRay) && mem::RipTarget(A(0x142E816DC), 1, 5) == A(kRay), kPRay, "both senses call the raycast"},
        {mem::RipTarget(A(0x142E4F6F3), 3, 7) == A(kRayWorld) && mem::RipTarget(A(0x142E81692), 3, 7) == A(kRayWorld), kPRay, "both pass the world global"},
        {mem::RipTarget(A(0x142E816AE), 3, 7) == A(kRayFilterVt) && mem::RipTarget(A(0x142E81727), 1, 5) == A(kRayFree), kPRay, "the filter vtable and its free"},
        {mem::RipTarget(A(0x142E4F340), 3, 7) == A(kStimMgr), kPSense, "the thread-safe list's AI manager"},
    };
    for (const auto& d : data) {
        ++n;
        if (!d.okv) bad |= d.group, Log("[probe] byte check FAILED: %s", d.name);
    }
    g_pgroups = (kPKnow | kPRay | kPSense | kPStimMul) & ~bad;
    Log("[probe] P0 byte checks (%u): knowledge %s, raycast %s, perception %s, stimulus +0xA4 %s", n, (bad & kPKnow) ? "FAILED" : "ok",
        (bad & kPRay) ? "FAILED" : "ok", (bad & kPSense) ? "FAILED" : "ok", (bad & kPStimMul) ? "FAILED" : "ok");
}
void PushRestoreGuarded(const char* why) noexcept;  // (below: P2 lever d''s stimulus write undone)
void ProbeFault(const char* where) noexcept {
    g_probe_off = true;
    Log("[probe] FAULT in %s: probes off for the run", where);
    PushRestoreGuarded("a probe fault");  // round 23 review: never leave our Sam's stimulus x3 behind (PushTick never runs again)
}
const char* Ok(unsigned b) noexcept { return b ? "ok" : "FAILED"; }
std::uint32_t OurSamId() noexcept {
    std::uint32_t id = 0;
    if (RegistryPlayerId(&id) && id) return id;
    return g_player_id.load();
}
bool Pos3(std::uintptr_t e, double (&p)[3]) noexcept {
    WorldTransform x{};
    if (!ReadXf(e, x)) return false;
    p[0] = x.pos.x, p[1] = x.pos.y, p[2] = x.pos.z;
    return true;
}
double Dist(const double (&a)[3], const double (&b)[3]) noexcept {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// ---- the knowledge object (P1, P5): a bounded search from the MULE's entity, agent, AIIndividualComponent and its individual
// (+0x50) for a DSAINoticeAndKnowledge whose +0x28 -> +0x118 is this MULE's agent, pointed to or held inside a block (the type is
// built in place by its RTTI factory 0x142E90C00 -> ctor 0x142E936F0, so it can be a member: run 20261125-154710 found no pointer
// to one within depth 2); depth 2, at most 60000 reads. When none verifies, the first one seen with another agent is returned,
// marked "unverified".
unsigned ReadBlock(std::uintptr_t p, std::uint64_t* buf, std::size_t bytes) noexcept {
    for (std::size_t n = bytes; n >= 0x40; n /= 2)
        if (mem::Read(p, buf, n)) return static_cast<unsigned>(n / 8);
    return 0;
}
bool Plausible(std::uint64_t v) noexcept { return ep::PlausiblePtr(v, 0, 0) && !mem::InImage(static_cast<std::uintptr_t>(v)); }
bool KnowOf(std::uintptr_t p, std::uintptr_t agent) noexcept {
    if (mem::Q(p) != A(kKnowVt)) return false;
    const std::uintptr_t x = mem::Q(p + kKX);
    return x && mem::Q(x + kXAgent) == agent;
}
std::uintptr_t KnowSearch(std::uintptr_t e, char* path, std::size_t cap, unsigned* reads_out, unsigned* vt_only) {
    *reads_out = 0, *vt_only = 0;
    std::snprintf(path, cap, "-");
    const std::uintptr_t agent = e ? mem::Q(e + kEAgent) : 0;
    if (!agent) return std::snprintf(path, cap, "no agent"), 0;
    const std::uintptr_t comp = CompOf(e, "AIIndividualComponent");
    const std::uintptr_t ind = comp ? mem::Q(comp + 0x50) : 0;
    const struct {
        const char* name;
        std::uintptr_t p;
        std::size_t n;
    } roots[] = {{"individual(comp+0x50)", ind, 0x800}, {"AIIndividualComponent", comp, 0x400}, {"agent(+0x170)", agent, 0x800}, {"entity", e, 0x800}};
    const std::uintptr_t vt = A(kKnowVt);
    static std::uint64_t blk[0x100], blk2[0x100];
    std::uintptr_t unver = 0;
    char unver_path[96] = {};
    struct Child {
        int root;
        unsigned off;
        std::uintptr_t p;
    };
    std::vector<Child> kids;
    std::vector<std::uintptr_t> seen;
    unsigned reads = 0;
    for (int r = 0; r < 4; ++r) {
        if (!roots[r].p) continue;
        const unsigned n = ReadBlock(roots[r].p, blk, roots[r].n);
        ++reads;
        for (unsigned i = 0; i < n; ++i) {
            const std::uint64_t v = blk[i];
            if (v == vt) {  // held inside the root
                const std::uintptr_t q = roots[r].p + i * 8;
                ++reads;
                if (KnowOf(q, agent)) {
                    *reads_out = reads;
                    std::snprintf(path, cap, "%s+0x%X (inside)", roots[r].name, i * 8);
                    return q;
                }
                if (!unver) unver = q, std::snprintf(unver_path, sizeof(unver_path), "%s+0x%X (inside)", roots[r].name, i * 8);
                ++*vt_only;
                continue;
            }
            if (!Plausible(v)) continue;
            const std::uintptr_t p = static_cast<std::uintptr_t>(v);
            ++reads;
            if (mem::Q(p) == vt) {
                if (KnowOf(p, agent)) {
                    *reads_out = reads;
                    std::snprintf(path, cap, "%s+0x%X", roots[r].name, i * 8);
                    return p;
                }
                if (!unver) unver = p, std::snprintf(unver_path, sizeof(unver_path), "%s+0x%X", roots[r].name, i * 8);
                ++*vt_only;
                continue;
            }
            if (kids.size() < 1200) kids.push_back({r, i * 8, p});
        }
    }
    for (const Child& c : kids) {
        if (reads > 60000) break;
        if (std::find(seen.begin(), seen.end(), c.p) != seen.end()) continue;
        seen.push_back(c.p);
        const unsigned n = ReadBlock(c.p, blk2, 0x800);
        ++reads;
        for (unsigned j = 0; j < n && reads <= 60000; ++j) {
            const std::uint64_t v = blk2[j];
            if (v == vt) {  // held inside a child
                const std::uintptr_t q = c.p + j * 8;
                ++reads;
                if (KnowOf(q, agent)) {
                    *reads_out = reads;
                    std::snprintf(path, cap, "%s+0x%X -> +0x%X (inside)", roots[c.root].name, c.off, j * 8);
                    return q;
                }
                if (!unver) unver = q, std::snprintf(unver_path, sizeof(unver_path), "%s+0x%X -> +0x%X (inside)", roots[c.root].name, c.off, j * 8);
                ++*vt_only;
                continue;
            }
            if (!Plausible(v)) continue;
            const std::uintptr_t p = static_cast<std::uintptr_t>(v);
            ++reads;
            if (mem::Q(p) != vt) continue;
            if (KnowOf(p, agent)) {
                *reads_out = reads;
                std::snprintf(path, cap, "%s+0x%X -> +0x%X", roots[c.root].name, c.off, j * 8);
                return p;
            }
            if (!unver) unver = p, std::snprintf(unver_path, sizeof(unver_path), "%s+0x%X -> +0x%X", roots[c.root].name, c.off, j * 8);
            ++*vt_only;
        }
    }
    *reads_out = reads;
    if (unver) return std::snprintf(path, cap, "%s unverified (its +0x28 -> +0x118 is not this agent)", unver_path), unver;
    std::snprintf(path, cap, "not found (%u reads%s)", reads, reads > 60000 ? ", the budget" : "");
    return 0;
}
struct KCache {
    std::uintptr_t entity = 0, k = 0;
    char path[96] = {};
};
KCache g_kcache[8];
unsigned g_kcache_next = 0;
// the knowledge object of a MULE entity (cached per entity while its K still verifies), logged when searched
std::uintptr_t KnowOfEntity(int probe, std::uint32_t id, std::uintptr_t e, char* path, std::size_t cap) {
    const std::uintptr_t agent = mem::Q(e + kEAgent);
    for (KCache& c : g_kcache)
        if (c.entity == e && c.k && KnowOf(c.k, agent)) return std::snprintf(path, cap, "%s (cached)", c.path), c.k;
    unsigned reads = 0, vt_only = 0;
    char p[96];
    const std::uintptr_t k = KnowSearch(e, p, sizeof(p), &reads, &vt_only);
    Log("[probe] P%d knowledge id=%05X entity %p: %s%p at %s (%u reads; %u other DSAINoticeAndKnowledge seen)", probe, id, reinterpret_cast<void*>(e),
        k ? "DSAINoticeAndKnowledge " : "none ", reinterpret_cast<void*>(k), p, reads, vt_only);
    if (k) {
        KCache& c = g_kcache[g_kcache_next++ % 8];
        c.entity = e, c.k = k;
        std::snprintf(c.path, sizeof(c.path), "%s", p);
    }
    std::snprintf(path, cap, "%s", p);
    return k;
}
struct KEntry {
    unsigned bucket = 0, index = 0;
    std::uint64_t key = 0;
    int type = 0, sub = 0;
    double pos[3] = {};
    std::uint32_t actor = 0;
    std::uint8_t flags = 0;
    std::uint8_t raw[kKeSize] = {};
};
constexpr unsigned kKMax = 48;
unsigned KnowEntries(std::uintptr_t k, KEntry* out, unsigned cap, unsigned* buckets) {
    *buckets = 0;
    std::int32_t nb = 0;
    const std::uintptr_t arr = mem::Q(k + kKBuckets);
    if (!mem::Rd(k + kKBucketN, nb) || nb <= 0 || nb > 256 || !arr) return 0;
    *buckets = static_cast<unsigned>(nb);
    unsigned n = 0;
    for (std::int32_t b = 0; b < nb && n < cap; ++b) {
        std::int32_t c = 0;
        const std::uintptr_t ents = mem::Q(arr + 16ull * static_cast<unsigned>(b) + 8);
        if (!mem::Rd(arr + 16ull * static_cast<unsigned>(b), c) || c <= 0 || c > 256 || !ents) continue;
        for (std::int32_t i = 0; i < c && n < cap; ++i) {
            KEntry& e = out[n];
            if (!mem::Read(ents + kKeSize * static_cast<unsigned>(i), e.raw, kKeSize)) continue;
            e.bucket = static_cast<unsigned>(b), e.index = static_cast<unsigned>(i);
            std::memcpy(&e.key, e.raw + kKeKey, 8);
            std::memcpy(&e.type, e.raw + kKeType, 4);
            std::memcpy(&e.sub, e.raw + kKeSub, 4);
            std::memcpy(e.pos, e.raw + kKePos, 24);
            std::memcpy(&e.actor, e.raw + kKeActor, 4);
            e.flags = e.raw[kKeFlags];
            ++n;
        }
    }
    return n;
}
// our Sam's PlayerVisual entry: by his actor id, else the nearest PlayerVisual within 3 m of him; -1 none. how: 1 id, 2 id bits, 3 place
int SamVisual(const KEntry* es, unsigned n, std::uint32_t sam_id, const double (&sam)[3], bool have_sam, int* how) {
    int best = -1;
    double bd = 3.0;
    *how = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (es[i].type != ep::kPlayerVisual) continue;
        const int s = ep::SameActor(es[i].actor, sam_id);
        if (s) return *how = s, static_cast<int>(i);
        if (have_sam) {
            const double d = Dist(es[i].pos, sam);
            if (d < bd) bd = d, best = static_cast<int>(i), *how = 3;
        }
    }
    return best;
}
void TypesText(const KEntry* es, unsigned n, char* out, std::size_t cap) {
    int types[16] = {}, counts[16] = {}, nt = 0;
    for (unsigned i = 0; i < n; ++i) {
        int k = 0;
        while (k < nt && types[k] != es[i].type) ++k;
        if (k < nt) ++counts[k];
        else if (nt < 16) types[nt] = es[i].type, counts[nt++] = 1;
    }
    std::size_t at = 0;
    out[0] = 0;
    for (int k = 0; k < nt && at + 40 < cap; ++k)
        at += static_cast<std::size_t>(std::snprintf(out + at, cap - at, "%s%d %s x%d", k ? ", " : "", types[k], ep::KnowledgeTypeName(types[k]), counts[k]));
    if (!nt) std::snprintf(out, cap, "no entries");
}
void LogSlot(int probe, std::uint32_t id, const KEntry& e) {
    float f[12] = {};
    std::memcpy(f, e.raw + 0x38, sizeof(f));
    Log("[probe] P%d slot id=%05X b%u.%u type %d %s sub %d key 0x%llX actor 0x%X at (%.2f, %.2f, %.2f) flags 0x%X; +0x38.. %.3g %.3g %.3g %.3g %.3g %.3g %.3g "
        "%.3g %.3g %.3g %.3g %.3g",
        probe, id, e.bucket, e.index, e.type, ep::KnowledgeTypeName(e.type), e.sub, static_cast<unsigned long long>(e.key), e.actor, e.pos[0], e.pos[1], e.pos[2],
        static_cast<unsigned>(e.flags), static_cast<double>(f[0]), static_cast<double>(f[1]), static_cast<double>(f[2]), static_cast<double>(f[3]),
        static_cast<double>(f[4]), static_cast<double>(f[5]), static_cast<double>(f[6]), static_cast<double>(f[7]), static_cast<double>(f[8]),
        static_cast<double>(f[9]), static_cast<double>(f[10]), static_cast<double>(f[11]));
}

// the copy (P1, P4: our frozen copy; P5: our own live MULE): entity and whether frozen
std::uintptr_t CopyEntity(std::uint32_t id, bool* frozen) noexcept {
    std::uintptr_t a = 0, e = 0;
    if (esync::MirrorOf(id, &a, &e) && e) return *frozen = true, e;
    *frozen = false;
    const std::uintptr_t actor = ActorOf(id);
    MuleNow m;
    return actor && ReadMule(actor, id, m) && m.realized ? m.entity : 0;
}
std::uint32_t ProbeId(double v, bool frozen_first) noexcept {
    if (v > 0) return static_cast<std::uint32_t>(v);
    if (const std::uint32_t l = esync::LookId()) return l;
    if (frozen_first) {
        std::uint32_t ids[4];
        std::uintptr_t as[4], es[4];
        if (esync::FrozenCopies(ids, as, es, 4)) return ids[0];
    }
    return Target(0);
}

// ---- P1 / P5: the knowledge runs (P5 also the phase, state and facing)
struct KRun {
    int probe = 0;
    std::uint32_t id = 0, world = 0, seq = 0;
    double t0 = 0, until = 0, next = 0, next_line = 0;
    bool frozen = false;
    std::uintptr_t entity = 0, k = 0;
    char path[96] = {};
    ep::FreshTrack ft;
    ep::PhaseTrack ph;
    unsigned samples = 0, slot_lines = 0, max_entries = 0;
    std::uint64_t set_sig = 0;
    double pv_first_ms = -1, d0 = -1;
    std::uint32_t ea0 = 0xFFFFFFFFu, ea4 = 0xFFFFFFFFu, a88 = 0xFFFFFFFFu;
};
KRun g_p1, g_p5;
unsigned g_p5_seq = 0;
const char* MsText(double ms, char* b, std::size_t cap) {
    if (ms < 0) return "never";
    std::snprintf(b, cap, "%.0f ms", ms);
    return b;
}
void KRunEnd(KRun& r, const char* why) {
    if (!r.id) return;
    char a[24], b[24];
    if (r.probe == 1)
        Log("[probe] P1 result id=%05X copy %s: knowledge %s; %u samples, at most %u entries; PlayerVisual of our Sam: %s (fresh %u, stale %u, none %u)%s%s",
            r.id, r.frozen ? "frozen" : "live", r.k ? r.path : "not found", r.samples, r.max_entries, ep::FreshText(r.ft.Verdict()), r.ft.n_fresh, r.ft.n_stale,
            r.ft.n_none, why ? "; ended: " : "", why ? why : "");
    else
        Log("[probe] P5 result #%u id=%05X: Sam %.1f m at the start; phase start %d end %d max %d, %u changes, first raise %s; state 0x%X bits 0x%X a88 0x%X; "
            "knowledge %s; PlayerVisual of our Sam %s (first at %s)%s%s",
            r.seq, r.id, r.d0, r.ph.start == -999 ? -1 : r.ph.start, r.ph.last == -999 ? -1 : r.ph.last, r.ph.max == -999 ? -1 : r.ph.max, r.ph.changes,
            MsText(r.ph.first_raise_ms, a, sizeof(a)), r.ea0, r.ea4, r.a88, r.k ? "found" : "not found", ep::FreshText(r.ft.Verdict()),
            MsText(r.pv_first_ms, b, sizeof(b)), why ? "; ended: " : "", why ? why : "");
    r = KRun{};
}
void KRunSample(KRun& r) {
    if (!r.id || g_now < r.next) return;
    r.next += r.probe == 1 ? 500.0 : 250.0;
    if (g_world != r.world) return KRunEnd(r, "the world changed");
    bool frozen = false;
    const std::uintptr_t e = CopyEntity(r.id, &frozen);
    if (!e || e != r.entity) return KRunEnd(r, "the copy is gone or was re-made");
    if (r.k && mem::Q(r.k) != A(kKnowVt)) r.k = 0;  // gone (an unverified one is kept while it is one)
    const double t = g_now - r.t0;
    double sam[3] = {}, me[3] = {};
    const bool hs = Pos3(g_sam.load(), sam), hm = Pos3(e, me);
    static KEntry es[kKMax];
    unsigned nb = 0, n = 0;
    if (r.k) n = KnowEntries(r.k, es, kKMax, &nb);
    if (n > r.max_entries) r.max_entries = n;
    int how = 0;
    const int pv = r.k ? SamVisual(es, n, OurSamId(), sam, hs, &how) : -1;
    const ep::Fresh f = r.ft.Feed(pv >= 0, pv >= 0 ? ep::EntrySig(es[pv].raw, kKeSize) : 0, g_now);
    if (pv >= 0 && r.pv_first_ms < 0) r.pv_first_ms = t;
    ++r.samples;
    // the entry set's identity (types and keys): its slots logged again when it changes (at most 40 slot lines a probe)
    std::uint64_t ss = 1469598103934665603ull;
    for (unsigned i = 0; i < n; ++i) ss = (ss ^ static_cast<std::uint64_t>(es[i].type) ^ (es[i].key << 8)) * 1099511628211ull;
    const bool set_changed = ss != r.set_sig;
    r.set_sig = ss;
    char types[200];
    TypesText(es, n, types, sizeof(types));
    char pvtxt[160] = "none";
    if (pv >= 0)
        std::snprintf(pvtxt, sizeof(pvtxt), "%s (by %s; at %.1f m from him; flags 0x%X)", ep::FreshText(f), how == 1 ? "id" : how == 2 ? "id bits" : "place",
                      hs ? Dist(es[pv].pos, sam) : -1.0, static_cast<unsigned>(es[pv].flags));
    if (r.probe == 1) {
        Log("[probe] P1 t=%.1f s id=%05X (%s): %u buckets, %u entries: %s; PlayerVisual of our Sam: %s; Sam %.1f m", t / 1000.0, r.id, frozen ? "frozen" : "live",
            nb, n, types, pvtxt, hs && hm ? Dist(sam, me) : -1.0);
    } else {
        // P5: our own MULE's phase, state, facing and knowledge
        const std::uintptr_t actor = ActorOf(r.id);
        MuleNow m;
        const bool hmule = actor && ReadMule(actor, r.id, m) && m.realized;
        const std::uintptr_t po = hmule && (g_tgroups & kTPhase) ? PhaseObj(m.entity) : 0;
        const int phase = po ? static_cast<int>(U32(po + kPhaseNow)) : -1;
        const bool ph_changed = r.ph.Feed(g_now, phase);
        const bool st_changed = hmule && (m.ea0 != r.ea0 || m.ea4 != r.ea4 || m.a88 != r.a88);
        if (hmule) r.ea0 = m.ea0, r.ea4 = m.ea4, r.a88 = m.a88;
        const double off = hmule && m.have_xf && hs ? ep::OffDeg(YawOf(m.xf), m.xf.pos.x, m.xf.pos.y, sam[0], sam[1]) : -1.0;
        if (r.d0 < 0 && hs && hm) r.d0 = Dist(sam, me);
        if (ph_changed || st_changed || set_changed || g_now >= r.next_line) {
            r.next_line = g_now + 1000.0;
            Log("[probe] P5 #%u t=%.2f s id=%05X: phase %d (+0x114 %.2f, +0x118 %.2f) state 0x%X bits 0x%X a88 0x%X; Sam %.1f m, %.0f deg off its facing; %s; "
                "PlayerVisual of our Sam: %s%s",
                r.seq, t / 1000.0, r.id, phase, po ? static_cast<double>(F32(po + 0x114)) : -1.0, po ? static_cast<double>(F32(po + 0x118)) : -1.0, m.ea0, m.ea4,
                m.a88, hs && hm ? Dist(sam, me) : -1.0, off, types, pvtxt, ph_changed ? " (phase changed)" : st_changed ? " (state changed)" : "");
        }
    }
    if (set_changed && r.slot_lines < 40)
        for (unsigned i = 0; i < n && r.slot_lines < 40; ++i, ++r.slot_lines) LogSlot(r.probe, r.id, es[i]);
    if (g_now >= r.until) KRunEnd(r, nullptr);
}
bool KRunStart(KRun& r, int probe, std::uint32_t id, double secs, char* msg, std::size_t cap) {
    if (r.id) KRunEnd(r, "replaced");
    const char* skip = !(g_tgroups & kTAgent) ? "the agent offset's check failed" : !(g_pgroups & kPKnow) ? "the knowledge checks failed" : nullptr;
    bool frozen = false;
    const std::uintptr_t e = id ? CopyEntity(id, &frozen) : 0;
    if (!skip && !e) skip = "no realized copy of it here";
    if (skip && probe == 1) {
        Log("[probe] P1 result id=%05X: skipped (%s)", id, skip);
        return std::snprintf(msg, cap, "P1 skipped: %s", skip), true;
    }
    if (!e) {
        Log("[probe] P5 result #%u id=%05X: skipped (%s)", ++g_p5_seq, id, skip);
        return std::snprintf(msg, cap, "P5 skipped: %s", skip), true;
    }
    r.probe = probe, r.id = id, r.world = g_world, r.t0 = r.next = g_now, r.until = g_now + (secs > 0 ? secs : (probe == 1 ? 6.0 : 8.0)) * 1000.0;
    r.entity = e, r.frozen = frozen;
    if (probe == 5) r.seq = ++g_p5_seq;
    r.k = skip ? 0 : KnowOfEntity(probe, id, e, r.path, sizeof(r.path));
    if (probe == 1 && !r.k) {
        KRunEnd(r, "no knowledge object");
        return std::snprintf(msg, cap, "P1 %05X: no knowledge object", id), true;
    }
    char seq[16] = "";
    if (probe == 5) std::snprintf(seq, sizeof(seq), " #%u", r.seq);
    Log("[probe] P%d start%s id=%05X copy %s for %.1f s: knowledge %s", probe, seq, id, frozen ? "frozen" : "live", (r.until - r.t0) / 1000.0,
        r.k ? r.path : skip ? skip : "not found");
    return std::snprintf(msg, cap, "P%d %05X started", probe, id), true;
}

// ---- P2: the push after a hand-over (the guest)
struct PushRun {
    std::uint32_t id = 0, world = 0;
    int lever = 0;
    double armed = 0, timeout_ms = 20000, next = 0, next_state = 0;
    ep::Push p;
    std::uintptr_t entity = 0, d_stim = 0, d_agent = 0;
    unsigned shots0 = 0, hits0 = 0, all_hits0 = 0;
    float d_mul = 0;
    double d_until = 0, dist = -1;
    char applied[160] = {};
    // round 25: the D1-D5 diagnostics' schedule and the path summary
    double last_diag = -1, next_camp = 0;
    double last_pos[3] = {}, last_pos_t = -1;
    ep::PathTrack path;
    bool autoarm = false;  // round 26: armed by itself at her single take (lever a, read only), not by the debug action
};
PushRun g_push;

// ---- Round 25 (docs/enemy-sync.md 14.18): her single takes. The diagnostics D1-D5 (the P2 window, the guest, reads only) and the
// take levers K1 / C1 / T1 (enemy_sync.cpp GuestTake / GuestRefreeze call TakeBefore / TakeAfter / Refreeze; each behind its key or
// P2 lever e / f / g). Round 26 (14.19, at the user's request): the keys default on and the levers act in real games too (no longer
// hidden runs only), at a take of one MULE alone (a forced take with no other member of its camp hers here) only. Every site
// byte-checked (TakeChecks), every read fault-free, every write and call SEH-guarded: a fault turns that lever off for the session.
constexpr std::uintptr_t kKForceFlag = 0x736, kKForcePos = 0x6A0, kKFocus = 0x5A8, kKeAge = 0x40, kKeMem = 0x44;
constexpr std::uintptr_t kKProcess = 0x142E8DAF0;  // K's ProcessSensorResults (vt+0x38): the forced player visual's reader
constexpr std::uintptr_t kKUpsert = 0x142EA0370;   // the knowledge upsert it calls
constexpr std::uintptr_t kCpForce = 0x142B649A0;   // MuleCP_ForceSetCPPhase(const u64 guid[2]*, int phase)
constexpr std::uintptr_t kTicketReturn = 0x142E60B70, kTicketTake = 0x142E6CE70;  // DSAIGroup's ticket return / receive
constexpr std::uintptr_t kCpPh = 0x210, kCpReq = 0x214, kCpPrev = 0x218, kCpCounter = 0x220, kCpAlert = 0x230, kCpK1Valid = 0x2E8,
                         kCpK1Pos = 0x290, kCpK1Age = 0x2B0, kCpK2Valid = 0x368, kCpK2Pos = 0x310, kCpRR = 0x150, kCpGuidRef = 0x18;
constexpr std::uintptr_t kMPhase = 0x110, kMCounter = 0x114, kMRole = 0x368, kMCpId = 0x374;
constexpr std::uintptr_t kCompIndiv = 0x50, kIndivLinkN = 0x138, kIndivLinks = 0x140, kGroupParent = 0x140, kGroupTicketN = 0x428,
                         kGroupTickets = 0x430;
enum LGroup : unsigned { kLKnow = 1, kLCamp = 2, kLTicket = 4, kLRole = 8, kLFace = 16 };
constexpr std::uintptr_t kRoleGetter = 0x142AFABC0;  // FUN_142afabc0: [comp+0x58]+0x368, DsMuleGetMuleRole0's only read [O]
// clang-format off
constexpr Site kTakeSites[] = {
    {0x142E8E005, "80 BF 36 07 00 00 00", "K1: the reader's flag test (cmp byte [rdi+0x736],0)", kLKnow},
    {0x142E8E022, "C5 FB 10 8F B0 06 00 00", "K1: the reader's z (+0x6B0)", kLKnow},
    {0x142E8E06B, "C5 F8 10 87 A0 06 00 00", "K1: the reader's x, y (+0x6A0)", kLKnow},
    {0x142E8E13C, "E8 ?? ?? ?? ?? C6 87 36 07 00 00 00", "K1: the upsert, then the flag cleared", kLKnow},
    {0x142F02529, "C6 81 36 07 00 00 01", "K1: the game's setter (the flag)", kLKnow},
    {0x142F02535, "C5 F8 11 81 A0 06 00 00", "K1: the game's setter (x, y)", kLKnow},
    {0x142F02542, "C5 FB 11 89 B0 06 00 00", "K1: the game's setter (z)", kLKnow},
    {kCpForce,    "40 57 48 81 EC A0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 88 00 00 00 8B FA 48 8B D1", "C1: ForceSetCPPhase", kLCamp},
    {0x142B649DD, "48 8D 98 08 02 00 00 83 FF 05", "C1: its phase struct (+0x208)", kLCamp},
    {0x142B64A90, "C7 43 08 05 00 00 00 33 FF 48 8B 0D", "C1: its case 5", kLCamp},
    {0x142B64B29, "8D 57 01 48 89 7C 24 28 48 8D 8B 70 02 00 00", "C1: case 5's CP knowledge (+0x270)", kLCamp},
    {kTicketReturn, "48 89 5C 24 20 55 56 57 48 83 EC 20 49 8B E8 48 8B F2 48 8B F9", "T1: the ticket return", kLTicket},
    {kTicketTake,   "48 89 5C 24 20 55 56 57 48 83 EC 20 49 8B E8 48 8B F2 48 8B F9", "T1: the ticket receive", kLTicket},
    {kRoleGetter,   "48 8B 41 58 48 85 C0 74 07 8B 80 68 03 00 00 C3", "R2: the role getter (mov eax,[rax+0x368])", kLRole},
    {0x142ECB40D,   "48 8B 4B 30 E8", "R2: DsMuleGetMuleRole0 calls it (mov rcx,[rbx+0x30])", kLRole},
    {0x1422F8D90,   "40 53 48 83 EC 20 48 8B 01 48 8B D9 FF 50 10", "F1: the actor teleport (the take's own)", kLFace},
};
// clang-format on
bool g_lchecked = false;
unsigned g_lgroups = 0, g_loff = 0;  // the groups whose checks passed; the groups turned off after a fault
void TakeChecks() noexcept {
    if (g_lchecked) return;
    g_lchecked = true;
    ProbeChecks();  // K1 finds K with the probes' knowledge search
    unsigned bad = 0, n = 0;
    for (const Site& s : kTakeSites) {
        ++n;
        if (!mem::CodeMatches(A(s.va), s.sig)) bad |= s.group, Log("[enemy] take byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
    }
    ++n;
    if (mem::Q(A(kKnowVt) + 0x38) != A(kKProcess) || mem::RipTarget(A(0x142E8E13C), 1, 5) != A(kKUpsert))
        bad |= kLKnow, Log("[enemy] take byte check FAILED: K1: the knowledge vtable's ProcessSensorResults (vt+0x38) and its upsert");
    if (!(g_pgroups & kPKnow)) bad |= kLKnow;
    g_lgroups = (kLKnow | kLCamp | kLTicket | kLRole | kLFace) & ~bad;
    Log("[enemy] take byte checks (%u): K1 know %s, C1 camp %s, T1 tickets %s, R2 role %s, F1 face %s", n, Ok(g_lgroups & kLKnow), Ok(g_lgroups & kLCamp),
        Ok(g_lgroups & kLTicket), Ok(g_lgroups & kLRole), Ok(g_lgroups & kLFace));
}
void LeverFault(unsigned group, const char* where) noexcept {
    g_loff |= group;
    Log("[enemy] take lever FAULT in %s: that lever is off for the session", where);
}
// why a lever cannot act now (nullptr: it can)
// (round 26: no longer hidden runs only, docs/enemy-sync.md 14.19)
const char* LeverWhy(unsigned group) noexcept {
    if (!(g_lgroups & group)) return "its byte checks failed";
    if (g_loff & group) return "off after a fault";
    return nullptr;
}
int SehCpForce(const std::uint64_t* guid, int phase) noexcept {
    __try {
        reinterpret_cast<void(__fastcall*)(const std::uint64_t*, int)>(A(kCpForce))(guid, phase);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
int SehTicketReturn(std::uintptr_t group, std::uint64_t key, std::uintptr_t indiv) noexcept {
    __try {
        reinterpret_cast<void(__fastcall*)(std::uintptr_t, std::uint64_t, std::uintptr_t)>(A(kTicketReturn))(group, key, indiv);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// K1's write: only while the game's flag is 0 (its reader clears it): the position first, a fence, then the flag. 1 written, 0 the
// flag was set, -1 a fault.
int SehKnowWrite(std::uintptr_t k, const double (&p)[3]) noexcept {
    __try {
        volatile std::uint8_t* const f = reinterpret_cast<volatile std::uint8_t*>(k + kKForceFlag);
        if (*f != 0) return 0;
        volatile double* const d = reinterpret_cast<volatile double*>(k + kKForcePos);
        d[0] = p[0], d[1] = p[1], d[2] = p[2];
        std::atomic_thread_fence(std::memory_order_seq_cst);
        *f = 1;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// the MSVC RTTI class name (mem::RttiName gives it plain: "DSAIGroup"): `kind` "Group" or "Individual" in a DSAI class
// (DSAIGroup / DSAIIndividual or a subclass of that name)
bool RttiIs(std::uintptr_t p, const char* kind) noexcept {
    const char* n = p ? mem::RttiName(mem::RttiOf(p)) : nullptr;
    return n && std::strncmp(n, "DSAI", 4) == 0 && std::strstr(n, kind) != nullptr;
}
const char* RttiText(std::uintptr_t p) noexcept {
    const char* n = p ? mem::RttiName(mem::RttiOf(p)) : nullptr;
    return n ? n : "?";
}
// a MULE entity's DSAIIndividual (AIIndividualComponent +0x50, its RTTI checked), 0 none
std::uintptr_t IndivOf(std::uintptr_t entity) noexcept {
    const std::uintptr_t comp = CompOf(entity, "AIIndividualComponent");
    const std::uintptr_t ind = comp ? mem::Q(comp + kCompIndiv) : 0;
    return ind && RttiIs(ind, "Individual") ? ind : 0;
}
// its innermost DSAIGroup ([O] FUN_14252b1a0's walk: indiv +0x140 / +0x138 -> obj; obj +0x20 n, +0x18 base (or inline at +0x2B
// aligned) -> base + n x 0x20 - 0x18; the parents via +0x140), 0 none
std::uintptr_t GroupOf(std::uintptr_t ind) noexcept {
    std::int32_t n = 0;
    const std::uintptr_t arr = mem::Q(ind + kIndivLinks);
    if (!ind || !arr || !mem::Rd(ind + kIndivLinkN, n) || n <= 0 || n > 16) return 0;
    for (std::int32_t i = 0; i < n; ++i) {
        const std::uintptr_t obj = mem::Q(arr + 8ull * static_cast<unsigned>(i));
        std::int32_t k = 0;
        if (!obj || !mem::Rd(obj + 0x20, k) || k <= 0 || k > 64) continue;
        std::uintptr_t base = mem::Q(obj + 0x18);
        if (!base) base = (obj + 0x2B) & ~static_cast<std::uintptr_t>(7);
        const std::uintptr_t g = mem::Q(base + 0x20ull * static_cast<unsigned>(k) - 0x18);
        if (g && RttiIs(g, "Group")) return g;
    }
    return 0;
}
struct Ticket {
    std::uint64_t key = 0;
    std::int32_t avail = 0, holders = 0;
    std::uint64_t h[8] = {};  // the first 8 holders
};
unsigned TicketsOf(std::uintptr_t g, Ticket* out, unsigned cap) noexcept {
    std::int32_t n = 0;
    const std::uintptr_t tab = mem::Q(g + kGroupTickets);
    if (!g || !tab || !mem::Rd(g + kGroupTicketN, n) || n <= 0 || n > 64) return 0;
    unsigned c = 0;
    for (std::int32_t i = 0; i < n && c < cap; ++i) {
        Ticket& t = out[c];
        const std::uintptr_t e = tab + 0x20ull * static_cast<unsigned>(i);
        if (!mem::Rd(e, t.key) || !mem::Rd(e + 8, t.avail) || !mem::Rd(e + 0x10, t.holders)) continue;
        const std::uintptr_t ha = mem::Q(e + 0x18);
        const int k = t.holders > 0 && t.holders <= ep::kMaxHolders ? (t.holders < 8 ? t.holders : 8) : 0;
        if (k && ha) mem::Read(ha, t.h, 8ull * static_cast<unsigned>(k));
        ++c;
    }
    return c;
}
// her newest take (singles are one at a time)
struct TakeRec {
    std::uint32_t id = 0, world = 0, cp_id = 0;
    std::uintptr_t actor = 0, entity = 0, k = 0, cp = 0;
    double t = 0;
    int cp_before = -1, lever = -1;
    bool diag = false, tickets = false;
    // K1
    int know_mode = 0;
    double know_secs = 3, last_write = -1, wrote_at = -1, faced_since = -1;
    int writes = 0;
    bool pending = false;
    double w_pos[3] = {}, w_dist = 0, w_off = 0;
    unsigned shots0 = 0, hits0 = 0;
    // C1
    bool c1 = false, c1_checked = false;
    double k_next = 0;  // the knowledge search's next try (2 Hz: it is a few thousand reads)
    int k_tries = 0;
    char text[200] = {};  // what the levers did (P2's lever e / f / g line)
};
TakeRec g_take;
// round 26: the ids she took alone and still owns (T1's give-back return is for those only, not every give-back of a camp)
std::set<std::uint32_t> g_alone;
void PushEnd(const char* why);  // (below)
// round 26: her single take with enemy_m2_probes=1 arms the read-only P2 window (lever a, nothing pushed: the D1-D8 lines) by
// itself, in a real game too; a window the debug action enemy_m2 armed (hidden runs) is kept, an automatic one of another id ends
void AutoArm(std::uint32_t id) {
    if (!g_cfg.enemy_m2_probes || g_probe_off || !g_installed || g_stop.load()) return;
    if (g_push.id && (!g_push.autoarm || g_push.id == id)) return;
    if (g_push.id) PushEnd("replaced by her next single take");
    TargetChecks();
    StimChecks();
    ProbeChecks();
    g_push.id = id, g_push.lever = 0, g_push.world = g_world, g_push.armed = g_push.next = g_now, g_push.timeout_ms = ep::kP2DefaultSecs * 1000.0;
    g_push.autoarm = true;
    shots::ArmOwnerCounts(true);
    Log("[probe] P2 armed id=%05X lever %s: by itself at her single take (enemy_m2_probes=1; read only; %d s after the hand-over)", id, ep::LeverName(0),
        ep::kP2DefaultSecs);
}
// R2: the roles written on her taken MULEs (by id), each restored when it stops being hers (given back, or her own reset)
struct RoleRec {
    std::uint32_t id = 0, cp_id = 0, world = 0;
    std::uintptr_t actor = 0;
    int was = -1, now = -1;
};
std::map<std::uint32_t, RoleRec> g_roles;
int ArmedLever(std::uint32_t id) noexcept { return g_push.id == id ? g_push.lever : -1; }
// D3: her CP actor and the taken member's camp fields
void CampLine(const char* when, std::uint32_t id, std::uintptr_t actor, std::uint32_t cp_id, std::uintptr_t cp) {
    if (!cp) return void(Log("[probe] P2 camp cp %05X %s: no CP actor here", cp_id, when));
    double al[3] = {}, k1[3] = {}, k2[3] = {};
    mem::Read(cp + kCpAlert, al, sizeof(al));
    mem::Read(cp + kCpK1Pos, k1, sizeof(k1));
    mem::Read(cp + kCpK2Pos, k2, sizeof(k2));
    Log("[probe] P2 camp cp %05X %s: phase %d req %d prev %d counter %.2f; alert (%.1f,%.1f,%.1f); entry1 valid %u (%.1f,%.1f,%.1f) age %.2f; "
        "entry2 valid %u (%.1f,%.1f,%.1f); rr %d; member %05X +0x110 %d +0x114 %.2f role %u cp %X",
        cp_id, when, static_cast<int>(U32(cp + kCpPh)), static_cast<int>(U32(cp + kCpReq)), static_cast<int>(U32(cp + kCpPrev)),
        static_cast<double>(F32(cp + kCpCounter)), al[0], al[1], al[2], U32(cp + kCpK1Valid) & 1u, k1[0], k1[1], k1[2], static_cast<double>(F32(cp + kCpK1Age)),
        U32(cp + kCpK2Valid) & 1u, k2[0], k2[1], k2[2], static_cast<int>(U32(cp + kCpRR)), id, actor ? static_cast<int>(U32(actor + kMPhase)) : -1,
        actor ? static_cast<double>(F32(actor + kMCounter)) : 0.0, actor ? U32(actor + kMRole) : 0, actor ? U32(actor + kMCpId) : 0);
}
// D5: the attack tickets of the taken MULE's group chain (its holders as MULE ids; * a frozen copy here)
void TicketsLine(const char* when, std::uint32_t id, std::uintptr_t entity) {
    esync::CampOf co;
    esync::CampOfId(id, &co);
    std::uintptr_t ind[25] = {};
    std::uint32_t ids[25] = {};
    std::uint8_t st[25] = {};
    unsigned ni = 0;
    ind[ni] = IndivOf(entity), ids[ni] = id, st[ni] = 2, ++ni;
    for (unsigned i = 0; i < co.n && ni < 25; ++i)
        if (co.m[i].entity) ind[ni] = IndivOf(co.m[i].entity), ids[ni] = co.m[i].id, st[ni] = co.m[i].state, ++ni;
    std::uintptr_t g = ind[0] ? GroupOf(ind[0]) : 0;
    if (!g) {
        const std::uintptr_t comp = CompOf(entity, "AIIndividualComponent");
        const std::uintptr_t raw = comp ? mem::Q(comp + kCompIndiv) : 0;
        Log("[probe] P2 tickets id=%05X %s: unknown link (%s; component %p, +0x50 %p class %s)", id, when, ind[0] ? "no DSAIGroup on its links" : "no DSAIIndividual",
            reinterpret_cast<void*>(comp), reinterpret_cast<void*>(raw), RttiText(raw));
        return;
    }
    for (int level = 0; g && level < 4; ++level, g = mem::Q(g + kGroupParent)) {
        if (!RttiIs(g, "Group")) break;
        Ticket t[12];
        const unsigned n = TicketsOf(g, t, 12);
        char buf[640];
        std::int32_t raw_n = -1;
        mem::Rd(g + kGroupTicketN, raw_n);
        const std::uintptr_t parent = mem::Q(g + kGroupParent);
        int at = std::snprintf(buf, sizeof(buf), "group %p (%s) level %d: %u entries (+0x428 %d, table %p; parent %p %s)", reinterpret_cast<void*>(g),
                               RttiText(g), level, n, raw_n, reinterpret_cast<void*>(mem::Q(g + kGroupTickets)), reinterpret_cast<void*>(parent),
                               parent ? RttiText(parent) : "-");
        for (unsigned i = 0; i < n && at > 0 && at < static_cast<int>(sizeof(buf)) - 80; ++i) {
            at += std::snprintf(buf + at, sizeof(buf) - at, "; key %llX avail %d holders %d", static_cast<unsigned long long>(t[i].key), t[i].avail, t[i].holders);
            for (int h = 0; h < 8 && h < t[i].holders && at < static_cast<int>(sizeof(buf)) - 16; ++h) {
                int who = -1;
                for (unsigned j = 0; j < ni; ++j)
                    if (ind[j] && ind[j] == t[i].h[h]) who = static_cast<int>(j);
                if (who < 0) at += std::snprintf(buf + at, sizeof(buf) - at, " ?");
                else at += std::snprintf(buf + at, sizeof(buf) - at, " %05X%s", ids[who], st[who] == 1 ? "*" : "");
            }
        }
        Log("[probe] P2 tickets id=%05X %s: %s", id, when, buf);
    }
}
// D4: the other members of its camp running in her world
void MateLines(const char* when, std::uint32_t id, const double (&me)[3]) {
    esync::CampOf co;
    if (!esync::CampOfId(id, &co)) return;
    unsigned shown = 0;
    for (unsigned i = 0; i < co.n && shown < 4; ++i) {
        const esync::CampMember& c = co.m[i];
        if (c.state != 0 || !c.entity) continue;
        ++shown;
        double p[3] = {};
        if (!Pos3(c.entity, p)) continue;
        std::uint32_t e20 = 0;
        std::int32_t seat = -1;
        esync::RideFields(c.actor, &e20, &seat);
        char ride[120] = "";
        const std::uintptr_t parent = mem::Q(c.entity + 0x70);
        double vp[3] = {};
        if (parent && Pos3(parent, vp)) {
            const char* n = mem::RttiName(mem::RttiOf(parent));
            std::snprintf(ride, sizeof(ride), "; parent %s at (%.1f,%.1f,%.1f)", n ? n : "?", vp[0], vp[1], vp[2]);
        }
        Log("[probe] P2 mate id=%05X %s: (%.1f,%.1f,%.1f) %.1f m from %05X; vehicle e20 %X seat %d; phase %u%s", c.id, when, p[0], p[1], p[2], Dist(p, me), id,
            e20, seat, c.phase, ride);
    }
    if (!shown) Log("[probe] P2 mate %s: no other member of cp %05X running here (%u listed)", when, co.cp_id, co.n);
}
// D2: K of the taken MULE
void KnowLine(const char* when, std::uint32_t id, std::uintptr_t entity) {
    if (!(g_pgroups & kPKnow)) return;
    static std::uintptr_t s_miss = 0;  // an entity whose search failed: searched again once a second only
    static double s_miss_next = 0;
    if (entity == s_miss && g_now < s_miss_next) return;
    char path[128];
    const std::uintptr_t k = KnowOfEntity(2, id, entity, path, sizeof(path));
    if (!k) {
        s_miss = entity, s_miss_next = g_now + 1000.0;
        return void(Log("[probe] P2 know id=%05X %s: no knowledge object", id, when));
    }
    static KEntry es[kKMax];
    unsigned buckets = 0;
    const unsigned n = KnowEntries(k, es, kKMax, &buckets);
    char types[200];
    TypesText(es, n, types, sizeof(types));
    double fp[3] = {};
    mem::Read(k + kKForcePos, fp, sizeof(fp));
    std::uint8_t flag = 0;
    mem::Rd(k + kKForceFlag, flag);
    const std::uintptr_t focus = mem::Q(k + kKFocus);
    std::int32_t ft = -1;
    double fpos[3] = {};
    if (focus && mem::Rd(focus + kKeType, ft)) mem::Read(focus + kKePos, fpos, sizeof(fpos));
    Log("[probe] P2 know id=%05X %s: K %p, %u entries in %u buckets (%s); force flag %u at (%.1f,%.1f,%.1f); focus %p type %d at (%.1f,%.1f,%.1f) [I]", id, when,
        reinterpret_cast<void*>(k), n, buckets, types, flag, fp[0], fp[1], fp[2], reinterpret_cast<void*>(focus), focus ? ft : -1, fpos[0], fpos[1], fpos[2]);
    unsigned listed = 0;
    for (unsigned i = 0; i < n && listed < 8; ++i) {
        if (!ep::KnowListed(es[i].type)) continue;
        ++listed;
        float age = 0, memv = 0;
        std::memcpy(&age, es[i].raw + kKeAge, 4);
        std::memcpy(&memv, es[i].raw + kKeMem, 4);
        char e[200];
        ep::KnowEntryText(e, sizeof(e), es[i].type, es[i].pos, age, memv, es[i].actor, es[i].flags, es[i].key);
        Log("[probe] P2 know id=%05X %s entry: %s", id, when, e);
    }
    if (!listed) Log("[probe] P2 know id=%05X %s entry: none", id, when);
}
bool HasType(std::uintptr_t k, int type) {
    static KEntry es[kKMax];
    unsigned buckets = 0;
    const unsigned n = KnowEntries(k, es, kKMax, &buckets);
    for (unsigned i = 0; i < n; ++i)
        if (es[i].type == type) return true;
    return false;
}
// R2 (enemy_take_role=1): role 1 -> 0 while she owns it (the copy still frozen; its AI plans with it from the unfreeze on)
int SehU32Write(std::uintptr_t p, std::uint32_t want, std::uint32_t v) noexcept {
    __try {
        volatile std::uint32_t* const q = reinterpret_cast<volatile std::uint32_t*>(p);
        if (*q != want) return 0;
        *q = v;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
void TakeRole(std::uint32_t id, std::uintptr_t actor, bool alone) {
    if (!g_cfg.enemy_take_role) return;
    std::int32_t role = -1, cpid = 0;
    if (!actor || !mem::Rd(actor + kMRole, role) || !mem::Rd(actor + kMCpId, cpid)) return void(Log("[enemy] take role id=%05X: skipped (no camp fields)", id));
    const int to = ep::RoleWhileHers(1, role);
    if (to < 0) return void(Log("[enemy] take role id=%05X: role %d kept (cp %X)", id, role, static_cast<unsigned>(cpid)));
    const char* why = !alone ? "not taken alone" : LeverWhy(kLRole);
    if (why) return void(Log("[enemy] take role id=%05X: skipped (%s; role %d)", id, why, role));
    const int r = SehU32Write(actor + kMRole, static_cast<std::uint32_t>(role), static_cast<std::uint32_t>(to));
    if (r < 0) return LeverFault(kLRole, "R2 role write");
    if (r == 0) return void(Log("[enemy] take role id=%05X: not written (it changed under us)", id));
    g_roles[id] = RoleRec{id, static_cast<std::uint32_t>(cpid), g_world, actor, role, to};
    Log("[enemy] take role id=%05X: role %d -> %d while hers (cp %X)", id, role, to, static_cast<unsigned>(cpid));
    const std::size_t at = std::strlen(g_take.text);
    std::snprintf(g_take.text + at, sizeof(g_take.text) - at, "%sR2 role %d->%d", at ? "; " : "", role, to);
}
void RoleRestore(std::uint32_t id, const char* when, bool world_gone = false) {
    const auto it = g_roles.find(id);
    if (it == g_roles.end()) return;
    const RoleRec r = it->second;
    g_roles.erase(it);
    if (!r.actor) return;
    // round 26 review: g_world is the one this combat tick last saw; enemy_sync's world-change reset runs earlier in the frame, so the
    // live counter is asked too (a load: not ready, or another counter -> never written: an address can repeat after a load)
    std::uint32_t live = 0;
    const bool ready = coop::WorldReady(&live);
    if (r.world != g_world || (ready && live != r.world) || (world_gone && !ready))
        return void(Log("[enemy] take role id=%05X %s: not restored (the world changed)", id, when));
    std::int32_t cpid = 0, aid = 0;
    // the same actor still (its id at +0x08 is the enemy sync's kActId [O]; its camp id unchanged)
    if (!mem::Rd(r.actor + 0x08, aid) || static_cast<std::uint32_t>(aid) != id || !mem::Rd(r.actor + kMCpId, cpid) || static_cast<std::uint32_t>(cpid) != r.cp_id)
        return void(Log("[enemy] take role id=%05X %s: not restored (the actor changed)", id, when));
    const int w = SehU32Write(r.actor + kMRole, static_cast<std::uint32_t>(r.now), static_cast<std::uint32_t>(r.was));
    if (w < 0) return LeverFault(kLRole, "R2 role restore");
    Log("[enemy] take role id=%05X %s: role %s %d", id, when, w ? "restored to" : "not restored (it changed; now not", w ? r.was : r.now);
}
// D8 (round 25 e, read only): her CP actor's post locators, the list DsGetMuleNearestPostPosition0 (0x142ECCFC0) searches through
// FUN_142B3D600 [O]: CP +0xF0 count, +0xF8 array of locators, each at +0x40/+0x48/+0x50 (doubles); used only when
// [CP +0x9B0] +0x148 != 0 [O]. DsGetMuleMainPostPosition0 (0x142ECA2D0) takes the first one [O]. ReadProcessMemory reads only.
void PostsLine(std::uint32_t id, std::uintptr_t actor, std::uintptr_t cp) {
    if (!cp) return;
    std::int32_t n = 0;
    std::uintptr_t arr = 0, sub = 0;
    std::int32_t on = -1;
    if (!mem::Rd(cp + 0xF0, n) || !mem::Rd(cp + 0xF8, arr)) return void(Log("[probe] P2 posts id=%05X: unreadable", id));
    if (mem::Rd(cp + 0x9B0, sub) && sub) mem::Rd(sub + 0x148, on);
    MuleNow m;
    const bool have = actor && ReadMule(actor, id, m) && m.have_xf;
    char buf[900] = {};
    int len = 0, best = -1;
    double bestd = 1e30;
    const int cnt = n < 0 ? 0 : (n > 12 ? 12 : n);
    for (int i = 0; i < cnt && len < 820; ++i) {
        std::uintptr_t e = 0;
        double p[3] = {};
        if (!mem::Rd(arr + 8u * static_cast<unsigned>(i), e) || !e || !mem::Read(e + 0x40, p, sizeof(p))) {
            len += std::snprintf(buf + len, sizeof(buf) - len, " #%d ?", i);
            continue;
        }
        const double d = have ? std::sqrt((p[0] - m.xf.pos.x) * (p[0] - m.xf.pos.x) + (p[1] - m.xf.pos.y) * (p[1] - m.xf.pos.y)) : -1.0;
        if (have && d < bestd) bestd = d, best = i;
        len += std::snprintf(buf + len, sizeof(buf) - len, " #%d (%.1f,%.1f,%.1f) %.1f m;", i, p[0], p[1], p[2], d);
    }
    Log("[probe] P2 posts id=%05X: cp list %d (on %d), MULE at (%.1f,%.1f); nearest #%d %.1f m;%s", id, n, on, have ? m.xf.pos.x : 0.0,
        have ? m.xf.pos.y : 0.0, best, best >= 0 ? bestd : -1.0, buf);
}
// F1 (enemy_take_face=1): turned in place to face her Sam when she is > 60 deg off its facing, 0.5..30 m (the copy still frozen)
void TakeFace(std::uint32_t id, std::uintptr_t actor, bool alone) {
    if (!g_cfg.enemy_take_face) return;
    MuleNow m;
    double sam[3] = {};
    if (!actor || !ReadMule(actor, id, m) || !m.realized || !m.have_xf || !Pos3(g_sam.load(), sam))
        return void(Log("[enemy] take face id=%05X: skipped (no MULE or Sam read)", id));
    const double yaw0 = YawOf(m.xf), off = ep::OffDeg(yaw0, m.xf.pos.x, m.xf.pos.y, sam[0], sam[1]);
    const double dx = sam[0] - m.xf.pos.x, dy = sam[1] - m.xf.pos.y, d = std::sqrt(dx * dx + dy * dy);
    double yaw = 0;
    if (!ep::FaceYaw(1, yaw0, m.xf.pos.x, m.xf.pos.y, sam[0], sam[1], &yaw))
        return void(Log("[enemy] take face id=%05X: not turned (Sam %.1f m, %.0f deg off)", id, d, off));
    const char* why = !alone ? "not taken alone" : LeverWhy(kLFace);
    if (why) return void(Log("[enemy] take face id=%05X: skipped (%s; Sam %.1f m, %.0f deg off)", id, why, d, off));
    const int turned = esync::TurnInPlace(id, actor, static_cast<float>(yaw));
    if (turned < 0) return LeverFault(kLFace, "F1 turn");
    if (turned == 0) return void(Log("[enemy] take face id=%05X: the turn failed (Sam %.1f m, %.0f deg off)", id, d, off));
    MuleNow m2;
    const double off2 = ReadMule(actor, id, m2) && m2.have_xf ? ep::OffDeg(YawOf(m2.xf), m2.xf.pos.x, m2.xf.pos.y, sam[0], sam[1]) : -1.0;
    Log("[enemy] take face id=%05X: turned from %.0f to %.0f deg off (Sam %.1f m)", id, off, off2, d);
    const std::size_t at = std::strlen(g_take.text);
    std::snprintf(g_take.text + at, sizeof(g_take.text) - at, "%sF1 turned %.0f->%.0f deg", at ? "; " : "", off, off2);
}
// her take, before the member's SetPhase (game thread; enemy_sync.cpp GuestTake): D3 "before", C1
void TakeBeforeBody(std::uint32_t id, std::uintptr_t actor, bool alone) {
    if (alone) g_alone.insert(id), AutoArm(id);
    const int lever = ArmedLever(id);
    // round 26: a member of a camp take (not hers alone) with no P2 window: nothing here (no lever, no line; her single's record kept)
    if (!alone && lever < 0) return;
    g_take = TakeRec{};
    g_take.id = id, g_take.actor = actor, g_take.world = g_world, g_take.t = g_now, g_take.lever = lever;
    g_take.diag = g_cfg.enemy_m2_probes && lever >= 0 && !g_probe_off;
    esync::CampOf co;
    if (esync::CampOfId(id, &co)) g_take.cp = co.cp, g_take.cp_id = co.cp_id;
    std::int32_t ph = -1;
    if (g_take.cp && mem::Rd(g_take.cp + kCpPh, ph) && ph >= 0 && ph <= 7) g_take.cp_before = ph;
    if (g_take.diag) CampLine("before the take", id, actor, g_take.cp_id, g_take.cp), PostsLine(id, actor, g_take.cp);
    TakeRole(id, actor, alone);
    TakeFace(id, actor, alone);
    const int key = lever == 5 ? 2 : g_cfg.enemy_take_camp;
    if (!ep::CampRefreshWanted(key, g_take.cp_before)) {
        if (key) Log("[enemy] take camp cp %05X id=%05X: not forced (her CP at %d; enemy_take_camp=%d forces at >= 4)", g_take.cp_id, id, g_take.cp_before, key);
        return;
    }
    const char* why = !alone ? "not taken alone" : LeverWhy(kLCamp);
    if (!why && !g_take.cp) why = "no CP actor here";
    std::uint64_t guid[2] = {};
    const std::uintptr_t gref = why ? 0 : mem::Q(g_take.cp + kCpGuidRef);
    if (!why && (!gref || !mem::Read(gref + 8, guid, sizeof(guid)) || (!guid[0] && !guid[1]))) why = "no CP guid";
    double sam[3] = {};
    if (!why && !Pos3(g_sam.load(), sam)) why = "no Sam";
    if (why) {
        Log("[enemy] take camp cp %05X id=%05X: skipped (%s)", g_take.cp_id, id, why);
        std::snprintf(g_take.text, sizeof(g_take.text), "C1 skipped: %s", why);
        return;
    }
    const int was[3] = {static_cast<int>(U32(g_take.cp + kCpPh)), static_cast<int>(U32(g_take.cp + kCpReq)), static_cast<int>(U32(g_take.cp + kCpPrev))};
    unsigned below = 0;
    for (unsigned i = 0; i < co.n; ++i) below += co.m[i].phase < 5 ? 1u : 0u;
    const int r = SehCpForce(guid, 5);
    if (r < 0) {
        LeverFault(kLCamp, "C1 ForceSetCPPhase");
        std::snprintf(g_take.text, sizeof(g_take.text), "C1 FAULT");
        return;
    }
    g_take.c1 = true;
    Log("[enemy] take camp cp %05X: forced 5 (was %d/%d/%d; now %d/%d/%d), her Sam at (%.1f,%.1f,%.1f), members < 5: %u (of %u others); for id=%05X",
        g_take.cp_id, was[0], was[1], was[2], static_cast<int>(U32(g_take.cp + kCpPh)), static_cast<int>(U32(g_take.cp + kCpReq)),
        static_cast<int>(U32(g_take.cp + kCpPrev)), sam[0], sam[1], sam[2], below, co.n, id);
    std::snprintf(g_take.text, sizeof(g_take.text), "C1 forced cp %05X to 5 (was %d)", g_take.cp_id, was[0]);
}
// T1: the tickets the frozen copy `copy` (its entity) holds in its innermost group chain, returned once per entry where it really
// is a holder. Returns the count returned.
unsigned ReturnTickets(std::uint32_t copy, std::uintptr_t entity, const char* when) {
    const std::uintptr_t ind = IndivOf(entity);
    std::uintptr_t g = ind ? GroupOf(ind) : 0;
    if (!g) {
        Log("[enemy] take tickets id=%05X %s: unknown link (%s)", copy, when, ind ? "no DSAIGroup" : "no DSAIIndividual");
        return 0;
    }
    unsigned returned = 0, entries = 0;
    for (int level = 0; g && level < 4 && !returned; ++level, g = mem::Q(g + kGroupParent)) {
        if (!RttiIs(g, "Group")) break;
        Ticket t[16];
        const unsigned n = TicketsOf(g, t, 16);
        entries += n;
        for (unsigned i = 0; i < n; ++i) {
            const int k = t[i].holders < 8 ? t[i].holders : 8;
            if (!ep::TicketsToReturn(t[i].h, k, ind)) continue;
            const int r = SehTicketReturn(g, t[i].key, ind);
            if (r < 0) {
                LeverFault(kLTicket, "T1 ticket return");
                return returned;
            }
            ++returned;
            Log("[enemy] take tickets id=%05X %s: returned key %llX at group %p level %d (available %d -> %d, holders %d -> %d)", copy, when,
                static_cast<unsigned long long>(t[i].key), reinterpret_cast<void*>(g), level, t[i].avail,
                static_cast<int>(U32(mem::Q(g + kGroupTickets) + 0x20ull * i + 8)), t[i].holders, static_cast<int>(U32(mem::Q(g + kGroupTickets) + 0x20ull * i + 0x10)));
        }
    }
    if (!returned) Log("[enemy] take tickets id=%05X %s: holds none (%u entries read)", copy, when, entries);
    return returned;
}
// her take, after the unfreeze: D3 "after", D5, K1 armed, T1 for the camp's frozen copies
void TakeAfterBody(std::uint32_t id, std::uintptr_t actor, std::uintptr_t entity, bool alone) {
    if (g_take.id != id) return;
    g_take.entity = entity;
    if (g_take.diag) {
        CampLine("after the take", id, actor, g_take.cp_id, g_take.cp);
        TicketsLine("at the take", id, entity);
    }
    const int lever = g_take.lever;
    const int mode = lever == 4 ? 2 : g_cfg.enemy_take_know;
    if (mode) {
        const char* why = !alone ? "not taken alone" : LeverWhy(kLKnow);
        if (why) {
            Log("[enemy] take know id=%05X: skipped (%s)", id, why);
        } else {
            g_take.know_mode = mode, g_take.know_secs = g_cfg.enemy_take_know_secs;
            shots::ArmOwnerCounts(true);
            g_take.shots0 = shots::ShotsBy(entity), g_take.hits0 = esync::OwnedHitsOn(id);
            const std::size_t at = std::strlen(g_take.text);
            std::snprintf(g_take.text + at, sizeof(g_take.text) - at, "%sK1 armed (mode %d, %.0f s)", at ? "; " : "", mode, g_take.know_secs);
        }
    }
    g_take.tickets = lever == 6 || g_cfg.enemy_ticket_return;
    if (g_take.tickets) {
        const char* why = !alone ? "not taken alone" : LeverWhy(kLTicket);
        if (why) {
            Log("[enemy] take tickets id=%05X: skipped (%s)", id, why);
            g_take.tickets = false;
            return;
        }
        esync::CampOf co;
        unsigned frozen = 0, returned = 0;
        if (esync::CampOfId(id, &co))
            for (unsigned i = 0; i < co.n; ++i)
                if (co.m[i].state == 1 && co.m[i].entity) ++frozen, returned += ReturnTickets(co.m[i].id, co.m[i].entity, "at the take (a frozen copy)");
        Log("[enemy] take tickets id=%05X at the take: %u frozen copies of cp %05X, %u ticket(s) returned", id, frozen, co.cp_id, returned);
        const std::size_t at = std::strlen(g_take.text);
        std::snprintf(g_take.text + at, sizeof(g_take.text) - at, "%sT1 %u returned (%u frozen)", at ? "; " : "", returned, frozen);
    }
}
// K1's 2 Hz writes and C1's follow-up (game thread, the combat tick)
void TakeTick() {
    if (!g_take.id || (!g_take.know_mode && !(g_take.c1 && !g_take.c1_checked) && !g_take.pending)) return;
    if (g_world != g_take.world || !esync::OwnedHere(g_take.id)) {
        if (g_take.know_mode || g_take.pending) Log("[enemy] take know id=%05X: ended (%s) after %d write(s)", g_take.id, g_world != g_take.world ? "the world changed" : "given back", g_take.writes);
        g_take.know_mode = 0, g_take.pending = false, g_take.c1_checked = true;
        return;
    }
    const std::uintptr_t actor = ActorOf(g_take.id);
    MuleNow m;
    double sam[3] = {};
    if (!actor || !ReadMule(actor, g_take.id, m) || !m.realized || !m.have_xf || m.entity != g_take.entity || !Pos3(g_sam.load(), sam)) return;
    const std::uintptr_t agent = mem::Q(m.entity + kEAgent);
    if (!g_take.k || !KnowOf(g_take.k, agent)) {
        g_take.k = 0;
        if (g_now < g_take.k_next) return;
        g_take.k_next = g_now + 500.0, ++g_take.k_tries;
        char path[128];
        g_take.k = KnowOfEntity(2, g_take.id, m.entity, path, sizeof(path));
        if (!g_take.k) {
            if (g_take.c1 && !g_take.c1_checked && g_now - g_take.t >= 500.0) {
                g_take.c1_checked = true;
                Log("[enemy] take camp id=%05X: no knowledge object to check for a CPOrder (K1 not given)", g_take.id);
            }
            if (g_take.know_mode && g_now - g_take.t > g_take.know_secs * 1000.0) {
                Log("[enemy] take know id=%05X: not applied (no knowledge object found in %d tries, %.1f s)", g_take.id, g_take.k_tries, (g_now - g_take.t) / 1000.0);
                g_take.know_mode = 0;
            }
            return;
        }
    }
    const std::uintptr_t k = g_take.k;
    std::uint8_t flag = 0;
    if (!mem::Rd(k + kKForceFlag, flag)) return;
    if (g_take.pending && flag == 0) {
        g_take.pending = false;
        Log("[enemy] take know id=%05X #%d: player visual at (%.1f,%.1f,%.1f) (Sam %.1f m, %.0f deg off); flag consumed in %.0f ms", g_take.id, g_take.writes,
            g_take.w_pos[0], g_take.w_pos[1], g_take.w_pos[2], g_take.w_dist, g_take.w_off, g_now - g_take.wrote_at);
        if (g_take.diag) KnowLine("after K1", g_take.id, m.entity);
    } else if (g_take.pending && g_now - g_take.wrote_at > 1000.0) {
        g_take.pending = false;
        Log("[enemy] take know id=%05X #%d: player visual at (%.1f,%.1f,%.1f) (Sam %.1f m, %.0f deg off); flag NOT consumed in 1 s", g_take.id, g_take.writes,
            g_take.w_pos[0], g_take.w_pos[1], g_take.w_pos[2], g_take.w_dist, g_take.w_off);
    }
    // C1's follow-up: no CPOrder 0.5 s after the force -> K1 once as well (the arm stays C1 for the record)
    if (g_take.c1 && !g_take.c1_checked && g_now - g_take.t >= 500.0) {
        g_take.c1_checked = true;
        if (!HasType(k, ep::kCPOrder)) {
            Log("[enemy] take camp id=%05X: no CPOrder 0.5 s after the force%s", g_take.id,
                g_take.know_mode ? " (K1 already armed)" : LeverWhy(kLKnow) ? ": K1 not possible" : ": K1 given once");
            if (!g_take.know_mode && !LeverWhy(kLKnow)) {
                g_take.know_mode = 1, g_take.know_secs = 1.5;
                shots::ArmOwnerCounts(true);
                g_take.shots0 = shots::ShotsBy(m.entity), g_take.hits0 = esync::OwnedHitsOn(g_take.id);
                g_take.t = g_now;
            }
        } else {
            Log("[enemy] take camp id=%05X: a CPOrder 0.5 s after the force", g_take.id);
        }
    }
    if (!g_take.know_mode) return;
    const double off = ep::OffDeg(YawOf(m.xf), m.xf.pos.x, m.xf.pos.y, sam[0], sam[1]);
    if (ep::WithinCone(off)) {
        if (g_take.faced_since < 0) g_take.faced_since = g_now;
    } else {
        g_take.faced_since = -1;
    }
    const bool attacked = shots::ShotsBy(m.entity) != g_take.shots0 || esync::OwnedHitsOn(g_take.id) != g_take.hits0;
    const ep::KnowStep s = ep::KnowPush(g_now, g_take.t, flag != 0 || g_take.pending, g_take.last_write, g_take.faced_since, attacked, g_take.know_mode, g_take.know_secs);
    if (s == ep::KnowStep::done) {
        Log("[enemy] take know id=%05X: done after %d write(s) (%s; %.1f s after the take)", g_take.id, g_take.writes,
            attacked ? "it attacked" : g_take.faced_since >= 0 && g_now - g_take.faced_since >= ep::kKnowFacedMs ? "facing her 1 s" : "the window ended",
            (g_now - g_take.t) / 1000.0);
        g_take.know_mode = 0;
        return;
    }
    if (s != ep::KnowStep::write) return;
    const int r = SehKnowWrite(k, sam);
    if (r < 0) {
        g_take.know_mode = 0;
        LeverFault(kLKnow, "K1 write");
        return;
    }
    if (r == 0) return;
    ++g_take.writes;
    g_take.pending = true, g_take.last_write = g_take.wrote_at = g_now;
    g_take.w_pos[0] = sam[0], g_take.w_pos[1] = sam[1], g_take.w_pos[2] = sam[2];
    const double me[3] = {m.xf.pos.x, m.xf.pos.y, m.xf.pos.z};
    g_take.w_dist = Dist(me, sam), g_take.w_off = off;
}
int TakeTickSeh() noexcept {
    __try {
        TakeTick();
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
void TakeTickGuarded() noexcept {
    if (!g_take.id) return;
    if (TakeTickSeh() < 0) {
        LeverFault(kLKnow | kLCamp, "the take tick");
        g_take = TakeRec{};
    }
}
// D1 / D2 on the 0.25 s / 1 s schedule, D3 / D4 / D5 once a second (PushTick, after the hand-over)
void P2Diag(const MuleNow& m, const double (&sam)[3], double off) {
    const double t_ms = g_now - g_push.p.t0;
    const double me[3] = {m.xf.pos.x, m.xf.pos.y, m.xf.pos.z};
    const double d = Dist(me, sam);
    if (ep::P2Due(t_ms, g_push.last_diag)) {
        g_push.last_diag = t_ms;
        double v = 0;
        if (g_push.last_pos_t >= 0 && t_ms - g_push.last_pos_t > 1.0) {
            const double dx = me[0] - g_push.last_pos[0], dy = me[1] - g_push.last_pos[1];
            v = std::sqrt(dx * dx + dy * dy) / ((t_ms - g_push.last_pos_t) / 1000.0);
        }
        g_push.last_pos[0] = me[0], g_push.last_pos[1] = me[1], g_push.last_pos[2] = me[2], g_push.last_pos_t = t_ms;
        g_push.path.Add(t_ms / 1000.0, me[0], me[1], d, off);
        char when[32];
        std::snprintf(when, sizeof(when), "t=%.2f", t_ms / 1000.0);
        Log("[probe] P2 pos id=%05X %s: (%.1f,%.1f,%.1f) facing %.0f deg, %.2f m/s; Sam %.1f m, %.0f deg off", g_push.id, when, me[0], me[1], me[2],
            YawOf(m.xf) * 57.29577951308232, v, d, off);
        KnowLine(when, g_push.id, m.entity);
    }
    if (g_now >= g_push.next_camp) {
        g_push.next_camp = g_now + 1000.0;
        char when[32];
        std::snprintf(when, sizeof(when), "t=%.1f", t_ms / 1000.0);
        esync::CampOf co;
        if (esync::CampOfId(g_push.id, &co)) CampLine(when, g_push.id, m.actor, co.cp_id, co.cp);
        MateLines(when, g_push.id, me);
        TicketsLine(when, g_push.id, m.entity);
    }
}
std::uintptr_t SamVisStim() noexcept {
    const std::uintptr_t sam = g_sam.load();
    const std::uintptr_t sc = VisComp(sam);
    const std::uintptr_t s = sc ? mem::Q(sc + kCVisStim) : 0;
    return s ? s : FindVisStimOf(sam ? mem::Q(sam + kEAgent) : 0);
}
void PushRestore(const char* why) {
    if (!g_push.d_stim) return;
    if (mem::Q(g_push.d_stim + kSAgent) == g_push.d_agent) {
        const int r = SehWrite(g_push.d_stim + kSMul, &g_push.d_mul, sizeof(g_push.d_mul));
        Log("[probe] P2 lever d': our Sam's stimulus +0xA4 back to %.3f (%s)%s", static_cast<double>(g_push.d_mul), why, r < 0 ? " FAULT" : "");
    } else {
        Log("[probe] P2 lever d': our Sam's stimulus was reused (not restored: %s)", why);
    }
    g_push.d_stim = 0;
}
// The fault path's restore (ProbeFault): PushRestore behind an SEH guard (its read is fault-free, its write SehWrite); a fault
// here forgets the stimulus (logged).
void PushRestoreGuarded(const char* why) noexcept {
    if (!g_push.d_stim) return;
    __try {
        PushRestore(why);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_push.d_stim = 0;
        Log("[probe] P2 lever d': our Sam's stimulus restore FAULTED (%s): forgotten", why);
    }
}
// Round 24: the handed-over MULE's state in OUR world at the hand-over, every 1 s and at the end: its state (ea0 / ea4), its phase
// (+0x110) and its AI phase object's, its camp actor's (CP) phase here, and the other members of that camp here (frozen copies,
// owned here, running here)
void PushState(const char* when, const MuleNow& m, double off, double dist) {
    esync::CampView cv;
    const bool have = esync::CampViewOf(g_push.id, &cv);
    const std::uintptr_t po = (g_tgroups & kTPhase) && m.entity ? PhaseObj(m.entity) : 0;
    const int ai_phase = po ? static_cast<int>(U32(po + kPhaseNow)) : -1;
    char camp[160];
    if (!have) std::snprintf(camp, sizeof(camp), "camp unread");
    else if (!cv.cp_id) std::snprintf(camp, sizeof(camp), "in no camp list here");
    else
        std::snprintf(camp, sizeof(camp), "camp cp %05X phase %d (%s); other members here %u: frozen %u, owned here %u, running %u", cv.cp_id,
                      cv.cp_phase == 0xFF ? -1 : static_cast<int>(cv.cp_phase), cv.cp_live ? "live" : "the walk's", cv.members, cv.frozen, cv.owned, cv.running);
    Log("[probe] P2 state id=%05X %s: state %u bits 0x%X a88 0x%X; phase %d, AI phase %d; %s; Sam %.1f m, %.0f deg off its facing; shots %u, hits on our Sam %u",
        g_push.id, when, m.ea0, m.ea4, m.a88, have && cv.phase != 0xFF ? static_cast<int>(cv.phase) : -1, ai_phase, camp, dist, off, g_push.p.shots, g_push.p.hits);
}
void PushEnd(const char* why) {
    if (!g_push.id) return;
    PushRestore("the end");
    char t[64], at[160];
    g_push.p.Text(t, sizeof(t));
    g_push.p.AttackLine(at, sizeof(at));
    if (g_push.p.t0 >= 0) {
        char path[256];
        g_push.path.Text(path, sizeof(path));
        Log("[probe] P2 path id=%05X: %s", g_push.id, path);
    }
    if (g_push.p.t0 < 0)
        Log("[probe] P2 result id=%05X lever %s: not handed over here (%s)", g_push.id, ep::LeverName(g_push.lever), why ? why : "no hand-over");
    else
        Log("[probe] P2 result id=%05X lever %s: %s; %s; all owned hits on our Sam %u (Sam %.1f m, %.0f deg off its facing at the hand-over, at least %.0f deg; "
            "%u samples over %.1f s; %s)%s%s",
            g_push.id, ep::LeverName(g_push.lever), t, at, esync::OwnedHitsOn(0) - g_push.all_hits0, g_push.dist, g_push.p.start_off,
            g_push.p.min_off > 360 ? -1.0 : g_push.p.min_off, g_push.p.samples, (g_now - g_push.p.t0) / 1000.0, g_push.applied, why ? "; ended: " : "", why ? why : "");
    g_push = PushRun{};
}
void PushApply(const MuleNow& m) {
    const std::uintptr_t sam = g_sam.load();
    switch (g_push.lever) {
        case 0: std::snprintf(g_push.applied, sizeof(g_push.applied), "nothing applied"); break;
        case 1: {
            if (!(g_groups & kGApply)) {
                std::snprintf(g_push.applied, sizeof(g_push.applied), "lever b skipped: the apply checks failed");
                break;
            }
            ewire::Hit h;
            h.key = g_push.id, h.hit_seq = ++g_hit_seq, h.attack = 0x47, h.kind = ewire::kKindTest, h.v184 = 0x3F800000u, h.part = 0xFFFFFFFFu;
            h.flags |= ewire::kHitNormalValid;
            const double local[3] = {0, 0, 1.2}, nl[3] = {1, 0, 0}, il[3] = {0, 0, 0};
            CreditIds();
            ApplyResult ar;
            ApplyOne(g_push.id, h, local, nl, il, 3, 0, erules::HostSerial(0, h.hit_seq), ar);
            std::snprintf(g_push.applied, sizeof(g_push.applied), "lever b: test hit 0x47 owner our Sam: %s", erules::HitResultText(static_cast<std::uint8_t>(ar.r)));
            break;
        }
        case 2: {
            const std::uintptr_t comp = CompOf(m.entity, "AIIndividualComponent");
            if (!(g_tgroups & kTPerfect) || !comp || mem::Q(comp + 0x48) != m.entity || !sam) {
                std::snprintf(g_push.applied, sizeof(g_push.applied), "lever c skipped: %s", !(g_tgroups & kTPerfect) ? "the alert's checks failed" : "no component or Sam");
                break;
            }
            const int r = SehAlert(true, comp, sam);
            std::snprintf(g_push.applied, sizeof(g_push.applied), "lever c: AlertForEntityWithPerfectThreatInfo -> our Sam: %s", r < 0 ? "FAULT" : "called");
            if (r < 0) ProbeFault("P2 lever c");
            break;
        }
        case 3: {
            if (!(g_pgroups & kPStimMul) || !(g_sgroups & kSCreate) || !(g_tgroups & kTAgent)) {
                std::snprintf(g_push.applied, sizeof(g_push.applied), "lever d' skipped: the stimulus checks failed");
                break;
            }
            const std::uintptr_t s = SamVisStim();
            float mul = 0;
            if (!s || !mem::Rd(s + kSMul, mul) || !(mul > 0.f && mul <= 8.f)) {
                std::snprintf(g_push.applied, sizeof(g_push.applied), "lever d' skipped: no visual stimulus of our Sam (or +0xA4 %.3f)", static_cast<double>(mul));
                break;
            }
            const float up = mul * 3.f;
            const int r = SehWrite(s + kSMul, &up, sizeof(up));
            if (r < 0) {
                std::snprintf(g_push.applied, sizeof(g_push.applied), "lever d': FAULT");
                ProbeFault("P2 lever d'");
                break;
            }
            g_push.d_stim = s, g_push.d_agent = mem::Q(s + kSAgent), g_push.d_mul = mul, g_push.d_until = g_now + 1000.0;
            std::snprintf(g_push.applied, sizeof(g_push.applied), "lever d': our Sam's stimulus %p +0xA4 %.3f -> %.3f for 1 s (a variant, not a new stimulus)",
                          reinterpret_cast<void*>(s), static_cast<double>(mul), static_cast<double>(up));
            break;
        }
        case 4:
        case 5:
        case 6:
            // round 25: K1 / C1 / T1 act in her take itself (TakeBefore / TakeAfter); their own lines are the record
            std::snprintf(g_push.applied, sizeof(g_push.applied), "lever %s at the take: %s", g_push.lever == 4 ? "e" : g_push.lever == 5 ? "f" : "g",
                          g_take.id == g_push.id && g_take.text[0] ? g_take.text : "nothing applied");
            break;
        default: break;
    }
}
void PushTick() {
    if (!g_push.id) return;
    if (g_world != g_push.world) return PushEnd("the world changed");
    if (g_push.d_stim && g_now >= g_push.d_until) PushRestore("1 s");
    if (g_now < g_push.next) return;
    g_push.next = g_now + 100.0;
    const bool owned = esync::OwnedHere(g_push.id);
    if (g_push.p.t0 < 0) {
        if (!owned) {
            if (g_now - g_push.armed > 90000.0) PushEnd("no hand-over in 90 s");
            return;
        }
        const std::uintptr_t actor = ActorOf(g_push.id);
        MuleNow m;
        double sam[3] = {};
        if (!actor || !ReadMule(actor, g_push.id, m) || !m.realized || !m.have_xf || !Pos3(g_sam.load(), sam)) return;  // the next sample
        const double off = ep::OffDeg(YawOf(m.xf), m.xf.pos.x, m.xf.pos.y, sam[0], sam[1]);
        const double me[3] = {m.xf.pos.x, m.xf.pos.y, m.xf.pos.z};
        g_push.dist = Dist(me, sam);
        g_push.entity = m.entity;
        g_push.shots0 = shots::ShotsBy(m.entity);
        g_push.hits0 = esync::OwnedHitsOn(g_push.id), g_push.all_hits0 = esync::OwnedHitsOn(0);
        g_push.p.Owned(g_now, off);
        PushApply(m);
        Log("[probe] P2 owned id=%05X here (%.1f s after the arm): Sam %.1f m, %.0f deg off its facing; MULE state %u bits 0x%X; %s", g_push.id,
            (g_now - g_push.armed) / 1000.0, g_push.dist, off, m.ea0, m.ea4, g_push.applied);
        PushState("at the hand-over", m, off, g_push.dist);
        g_push.next_state = g_now + 1000.0;
        P2Diag(m, sam, off);
        if (g_push.p.Done(g_now, g_push.timeout_ms, true)) PushEnd(nullptr);
        return;
    }
    if (!owned) return PushEnd("given back before the end");
    const std::uintptr_t actor = ActorOf(g_push.id);
    MuleNow m;
    double sam[3] = {};
    if (!actor || !ReadMule(actor, g_push.id, m) || !m.realized || !m.have_xf || m.entity != g_push.entity || !Pos3(g_sam.load(), sam))
        return PushEnd("the copy is gone");
    const double off = ep::OffDeg(YawOf(m.xf), m.xf.pos.x, m.xf.pos.y, sam[0], sam[1]);
    const unsigned shots = shots::ShotsBy(m.entity) - g_push.shots0;
    const unsigned hits = esync::OwnedHitsOn(g_push.id) - g_push.hits0;  // round 24: its melee strikes and owned rounds on our Sam
    const double turn0 = g_push.p.turn_ms, shot0 = g_push.p.shot_ms, melee0 = g_push.p.melee_ms;
    const unsigned hits_before = g_push.p.hits;
    g_push.p.Sample(g_now, off, shots, hits);
    const double me[3] = {m.xf.pos.x, m.xf.pos.y, m.xf.pos.z};
    const double dist = Dist(me, sam);
    if (turn0 < 0 && g_push.p.turn_ms >= 0) Log("[probe] P2 id=%05X within %.0f deg of our Sam after %.0f ms (%.0f deg)", g_push.id, ep::kConeDeg, g_push.p.turn_ms, off);
    if (shot0 < 0 && g_push.p.shot_ms >= 0) Log("[probe] P2 id=%05X first shot after %.0f ms (%u)", g_push.id, g_push.p.shot_ms, shots);
    if (melee0 < 0 && g_push.p.melee_ms >= 0)
        Log("[probe] P2 id=%05X first melee hit on our Sam after %.0f ms (%u hit messages; Sam %.1f m)", g_push.id, g_push.p.melee_ms, hits, dist);
    if (hits_before == 0 && g_push.p.hits > 0 && g_push.p.shot_hits > 0)
        Log("[probe] P2 id=%05X first hit on our Sam by its rounds after %.0f ms (Sam %.1f m)", g_push.id, g_now - g_push.p.t0, dist);
    if (g_now >= g_push.next_state) {
        g_push.next_state += 1000.0;
        char when[32];
        std::snprintf(when, sizeof(when), "t=%.1f s", (g_now - g_push.p.t0) / 1000.0);
        PushState(when, m, off, dist);
    }
    P2Diag(m, sam, off);
    if (g_push.p.Done(g_now, g_push.timeout_ms, true)) PushEnd(nullptr);
}

// ---- P4: the raycast from a copy's eye to our Sam (one call a tick, head and chest in turn)
struct RayRun {
    std::uint32_t id = 0, world = 0;
    std::uintptr_t entity = 0;
    double t0 = 0, until = 0, next_sum = 0, dist = -1;
    unsigned n[2][2] = {};   // [chest, head][clear, blocked]
    int last[2] = {-1, -1};  // 0 clear, 1 blocked
    unsigned calls = 0, odd = 0, turn = 0;
    bool frozen = false;
};
RayRun g_ray;
int SehRay(const double (&ray)[6], double (&hit)[3]) noexcept {
    alignas(16) std::uint8_t res[0x80] = {};
    alignas(16) std::uint8_t filt[0x40] = {};
    __try {
        const std::uintptr_t world = mem::Q(A(kRayWorld));
        if (!world) return -2000;
        reinterpret_cast<void* (*)(void*)>(A(kRayResInit))(res);
        reinterpret_cast<void* (*)(void*)>(A(kRayResInit))(res + 0x40);
        const std::uintptr_t vt = A(kRayFilterVt);
        const std::uint32_t mask = 0x34, ff = 0xFF;
        std::memcpy(filt, &vt, 8);
        std::memcpy(filt + 8, &mask, 4);
        std::memcpy(filt + 0xC, &ff, 4);
        const int r = reinterpret_cast<int (*)(std::uintptr_t, const double*, void*, int, void*, std::uint8_t)>(A(kRay))(world, ray, res, 1, filt, 1);
        std::uintptr_t arr = 0;
        std::memcpy(&arr, filt + 0x20, 8);
        if (arr) reinterpret_cast<void (*)(std::uintptr_t)>(A(kRayFree))(arr);
        std::memcpy(hit, res, sizeof(hit));
        return r;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1000;
    }
}
void RayEnd(const char* why) {
    if (!g_ray.id) return;
    Log("[probe] P4 result id=%05X copy %s: %u calls; chest clear %u blocked %u, head clear %u blocked %u; Sam %.1f m%s%s%s", g_ray.id, g_ray.frozen ? "frozen" : "live",
        g_ray.calls, g_ray.n[0][0], g_ray.n[0][1], g_ray.n[1][0], g_ray.n[1][1], g_ray.dist, g_ray.odd ? " (odd returns seen)" : "", why ? "; ended: " : "",
        why ? why : "");
    g_ray = RayRun{};
}
void RayTick() {
    if (!g_ray.id) return;
    if (g_world != g_ray.world) return RayEnd("the world changed");
    bool frozen = false;
    const std::uintptr_t e = CopyEntity(g_ray.id, &frozen);
    double me[3] = {}, sam[3] = {};
    if (!e || e != g_ray.entity || !Pos3(e, me) || !Pos3(g_sam.load(), sam)) return RayEnd("the copy or our Sam is gone");
    g_ray.dist = Dist(me, sam);
    const int which = static_cast<int>(g_ray.turn++ & 1u);  // 0 chest, 1 head
    const double from[3] = {me[0], me[1], me[2] + 1.6}, to[3] = {sam[0], sam[1], sam[2] + (which ? 1.5 : 1.0)};
    double ray[6] = {}, len = 0, hit[3] = {};
    if (!ep::Shorten(from, to, ep::kRayTrimM, ray, &len)) return;
    const int r = SehRay(ray, hit);
    if (r == -1000) {
        ProbeFault("P4 the raycast");
        return RayEnd("FAULT");
    }
    if (r == -2000) return RayEnd("no physics world");
    ++g_ray.calls;
    if (r < 0 || r > 1) ++g_ray.odd;
    const int now_b = r >= 1 ? 1 : 0;
    ++g_ray.n[which][now_b];
    if (g_ray.last[which] != now_b) {
        Log("[probe] P4 t=%.2f s id=%05X %s %s (hits %d; ray %.1f m; first hit +0 at (%.2f, %.2f, %.2f))", (g_now - g_ray.t0) / 1000.0, g_ray.id,
            which ? "head" : "chest", now_b ? "blocked" : "clear", r, len, hit[0], hit[1], hit[2]);
        g_ray.last[which] = now_b;
    }
    if (g_now >= g_ray.next_sum) {
        g_ray.next_sum = g_now + 500.0;
        Log("[probe] P4 t=%.1f s id=%05X: chest %s, head %s; Sam %.1f m (%u calls)", (g_now - g_ray.t0) / 1000.0, g_ray.id,
            g_ray.last[0] < 0 ? "-" : g_ray.last[0] ? "blocked" : "clear", g_ray.last[1] < 0 ? "-" : g_ray.last[1] ? "blocked" : "clear", g_ray.dist, g_ray.calls);
    }
    if (g_now >= g_ray.until) RayEnd(nullptr);
}

void ProbeTick() {
    KRunSample(g_p1);
    KRunSample(g_p5);
    PushTick();
    RayTick();
}
int ProbeTickSeh() noexcept {
    __try {
        ProbeTick();
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// ---- P3: the perception reads (entity +0x314; the stimulus +0x88 the classifier masks to 0x1FFFFF)
void LogStim88(const char* who, std::uintptr_t s, std::uint32_t sam_id) {
    if (!s) return Log("[probe] P3 %s stimulus: none", who);
    const std::uint32_t c = U32(s + kSClass);
    Log("[probe] P3 %s stimulus %p: +0x88 0x%X (id bits 0x%05X: %s our Sam's 0x%X), +0xA4 %.3f, agent %p %s", who, reinterpret_cast<void*>(s), c, c & 0x1FFFFFu,
        (c & 0x1FFFFFu) == (sam_id & 0x1FFFFFu) ? "the same as" : "not", sam_id, static_cast<double>(F32(s + kSMul)), reinterpret_cast<void*>(mem::Q(s + kSAgent)),
        NameOf(mem::Q(s + kSAgent)));
}
bool P3(char* msg, std::size_t cap) {
    if (!(g_pgroups & kPSense) || !(g_sgroups & kSCreate) || !(g_tgroups & kTAgent)) {
        Log("[probe] P3 result: skipped (the perception, stimulus or agent checks failed)");
        return std::snprintf(msg, cap, "P3 skipped"), true;
    }
    const std::uintptr_t sam = g_sam.load(), body = BodyEntity();
    const std::uint32_t sam_id = OurSamId();
    const std::uint32_t s314 = sam ? U32(sam + kEPerceive) : 0, b314 = body ? U32(body + kEPerceive) : 0;
    Log("[probe] P3 our Sam %p %s: +0x314 0x%X; the partner body %p %s: +0x314 0x%X; our Sam's actor id 0x%X", reinterpret_cast<void*>(sam), NameOf(sam), s314,
        reinterpret_cast<void*>(body), NameOf(body), b314, sam_id);
    const std::uintptr_t ss = SamVisStim();
    LogStim88("our Sam's visual", ss, sam_id);
    if (g_hs.stim) LogStim88("the partner body's (enemy_target 9)", g_hs.stim, sam_id);
    // the thread-safe visual list ([O] AI manager +0x3C398; [I] next +0xC8 as the plain list): the stimuli of our two agents
    const std::uintptr_t mgr = mem::Q(A(kStimMgr));
    const std::uintptr_t sag = sam ? mem::Q(sam + kEAgent) : 0, bag = body ? mem::Q(body + kEAgent) : 0;
    unsigned total = 0, ours = 0;
    for (std::uintptr_t s = mgr ? mem::Q(mgr + kMgrTsVis) : 0; s && total < 600; s = mem::Q(s + 0xC8), ++total) {
        const std::uintptr_t ag = mem::Q(s + kSAgent);
        if (!ag || (ag != sag && ag != bag)) continue;
        if (++ours <= 4) LogStim88(ag == sag ? "thread-safe list: our Sam's" : "thread-safe list: the partner body's", s, sam_id);
    }
    const std::uint32_t c = ss ? U32(ss + kSClass) : 0;
    Log("[probe] P3 result: our Sam +0x314 0x%X, the partner body +0x314 0x%X; our Sam's stimulus +0x88 %s; the thread-safe visual list %u live, %u of our "
        "agents",
        s314, b314, !ss ? "none" : (c & 0x1FFFFFu) == (sam_id & 0x1FFFFFu) ? "carries his id" : "does not carry his id", total, ours);
    return std::snprintf(msg, cap, "P3 read"), true;
}

// ---- P6: a seated MULE's parent chain (+0x70) and ride fields (+0xE20 the vehicle id, +0x3C0 the seat)
bool P6(double id_v, double m_v, char* msg, std::size_t cap) {
    const double radius = m_v > 0 ? m_v : 200.0;
    double sam[3] = {};
    const bool hs = Pos3(g_sam.load(), sam);
    WalkActors();
    unsigned n = 0, seated = 0, by_chain = 0, by_ride = 0, lines = 0;
    for (const auto& a : g_actors) {
        if (id_v > 0 && a.first != static_cast<std::uint32_t>(id_v)) continue;
        MuleNow m;
        if (!ReadMule(a.second, a.first, m) || !m.realized || !m.have_xf) continue;
        const double me[3] = {m.xf.pos.x, m.xf.pos.y, m.xf.pos.z};
        const double d = hs ? Dist(me, sam) : -1.0;
        if (id_v <= 0 && (!hs || d > radius)) continue;
        ++n;
        char chain[320] = {};
        std::size_t at = 0;
        bool veh = false;
        std::uintptr_t p = m.entity;
        for (int k = 0; k < 6 && at + 60 < sizeof(chain); ++k) {
            p = mem::Q(p + kEParent);
            if (!p) break;
            const char* nm = mem::RttiName(mem::RttiOf(p));
            if (nm && shotw::ClassifyHit(nm) == shotw::HitClass::vehicle) veh = true;
            at += static_cast<std::size_t>(std::snprintf(chain + at, sizeof(chain) - at, "%s%p %s", k ? " > " : "", reinterpret_cast<void*>(p), nm ? nm : "?"));
        }
        if (!at) std::snprintf(chain, sizeof(chain), "none");
        std::uint32_t e20 = erules::kVehicleIdNone;
        std::int32_t seat = -1;
        const bool ride = esync::RideFields(m.actor, &e20, &seat);
        const bool rseat = ride && erules::SeatedByRide(e20, seat);
        if (veh) ++by_chain;
        if (rseat) ++by_ride;
        if (veh || rseat) ++seated;
        if ((veh || rseat || id_v > 0) && lines < 24) {
            ++lines;
            Log("[probe] P6 id=%05X %s at %.1f m: parent chain (+0x70) %s; ride fields %s +0xE20 0x%X seat %d; MULE state %u bits 0x%X", a.first,
                veh || rseat ? "SEATED" : "not seated", d, chain, ride ? "read:" : "not read (checks):", e20, seat, m.ea0, m.ea4);
        }
    }
    Log("[probe] P6 result: %u MULEs within %.0f m%s, seated %u (by the parent chain %u, by the ride fields %u)", n, radius, id_v > 0 ? " (by id)" : "", seated,
        by_chain, by_ride);
    return std::snprintf(msg, cap, "P6 %u MULEs, %u seated", n, seated), true;
}

bool M2Body(const double (&v)[3], char* msg, std::size_t cap) {
    const int probe = static_cast<int>(v[0]);
    switch (probe) {
        case 0: {
            Log("[probe] P0 result: knowledge %s, raycast %s, perception %s, stimulus +0xA4 %s; agent %s, phase %s, alert %s, apply %s, stimulus %s; role %d",
                Ok(g_pgroups & kPKnow), Ok(g_pgroups & kPRay), Ok(g_pgroups & kPSense), Ok(g_pgroups & kPStimMul), Ok(g_tgroups & kTAgent), Ok(g_tgroups & kTPhase),
                Ok(g_tgroups & kTPerfect), Ok(g_groups & kGApply), Ok(g_sgroups & kSCreate), g_role);
            return std::snprintf(msg, cap, "P0 checks logged"), true;
        }
        case 1: return KRunStart(g_p1, 1, ProbeId(v[1], true), v[2], msg, cap);
        case 5: return KRunStart(g_p5, 5, ProbeId(v[1], false), v[2], msg, cap);
        case 2: {
            if (g_push.id) PushEnd("replaced");
            const std::uint32_t id = ProbeId(v[1], true);
            // round 25: v[2] = lever + 100 x secs (the window: 0 = 20 s, at most 40); levers 0..6
            int lever = -1, secs = ep::kP2DefaultSecs;
            const bool ok = ep::P2Window(v[2], &lever, &secs);
            if (g_role != 2 || !id || !ok) {
                Log("[probe] P2 result id=%05X lever %s: skipped (%s)", id, ep::LeverName(ok ? lever : -1), g_role != 2 ? "the guest's probe" : !id ? "no id" : "lever 0..6 (+ 100 x secs)");
                return std::snprintf(msg, cap, "P2 skipped"), true;
            }
            g_push.id = id, g_push.lever = lever, g_push.world = g_world, g_push.armed = g_push.next = g_now, g_push.timeout_ms = secs * 1000.0;
            shots::ArmOwnerCounts(true);
            Log("[probe] P2 armed id=%05X lever %s: waiting for the hand-over (%s now; %d s after it, the whole window)", id, ep::LeverName(lever),
                esync::OwnedHere(id) ? "owned here already" : "not owned here", secs);
            return std::snprintf(msg, cap, "P2 armed %05X lever %d", id, lever), true;
        }
        case 3: return P3(msg, cap);
        case 4: {
            if (g_ray.id) RayEnd("replaced");
            const std::uint32_t id = ProbeId(v[1], true);
            bool frozen = false;
            const std::uintptr_t e = id ? CopyEntity(id, &frozen) : 0;
            const char* why = !(g_pgroups & kPRay) ? "the raycast's byte checks or signature failed" : !e ? "no realized copy of it here" : !g_sam.load() ? "no Sam" : nullptr;
            if (why) {
                Log("[probe] P4 skipped (%s)", why);
                Log("[probe] P4 result id=%05X: skipped (%s)", id, why);
                return std::snprintf(msg, cap, "P4 skipped: %s", why), true;
            }
            g_ray.id = id, g_ray.entity = e, g_ray.frozen = frozen, g_ray.world = g_world, g_ray.t0 = g_ray.next_sum = g_now;
            g_ray.until = g_now + (v[2] > 0 ? v[2] : 5.0) * 1000.0;
            Log("[probe] P4 start id=%05X copy %s for %.1f s: eye +1.6 m to our Sam's chest +1.0 / head +1.5, %.1f m off each end, flag 1, max 1", id,
                frozen ? "frozen" : "live", (g_ray.until - g_now) / 1000.0, ep::kRayTrimM);
            return std::snprintf(msg, cap, "P4 %05X started", id), true;
        }
        case 6: return P6(v[1], v[2], msg, cap);
        default: return std::snprintf(msg, cap, "probe 0..6"), false;
    }
}
int M2Seh(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    __try {
        return M2Body(v, msg, cap) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// game thread, from the combat tick (after the target probe)
void ProbeTickGuarded() noexcept {
    if (g_probe_off || (!g_p1.id && !g_p5.id && !g_push.id && !g_ray.id)) return;
    if (ProbeTickSeh() < 0) {
        ProbeFault("the probe tick");  // (restores P2 lever d''s stimulus write first: PushRestoreGuarded)
        g_p1 = KRun{}, g_p5 = KRun{}, g_push = PushRun{}, g_ray = RayRun{};
    }
}
}  // namespace

namespace {
void RefreezeBody(std::uint32_t id, std::uintptr_t entity) {
    // round 26: the key's give-back return only for a MULE she took alone (not every give-back of a camp take)
    if (!((g_take.id == id && g_take.tickets) || (g_cfg.enemy_ticket_return && g_alone.count(id)))) return;
    if (const char* why = LeverWhy(kLTicket)) return void(Log("[enemy] take tickets id=%05X given back: skipped (%s)", id, why));
    ReturnTickets(id, entity, "given back (frozen again)");
}
int HookSeh(int which, std::uint32_t id, std::uintptr_t actor, std::uintptr_t entity, bool alone) noexcept {
    __try {
        if (which == 0) TakeBeforeBody(id, actor, alone);
        else if (which == 1) TakeAfterBody(id, actor, entity, alone);
        else if (which == 2) RefreezeBody(id, entity);
        else RoleRestore(id, alone ? "given back" : "own reset", entity != 0), g_alone.erase(id);  // (entity: TakeEnded's world_gone)
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
bool HookWanted() noexcept {
    return g_role == 2 && (g_cfg.enemy_m2_probes || g_cfg.enemy_take_know || g_cfg.enemy_take_camp || g_cfg.enemy_ticket_return ||
                           g_cfg.enemy_take_role || g_cfg.enemy_take_face);
}
void Hook(int which, std::uint32_t id, std::uintptr_t actor, std::uintptr_t entity, bool alone) noexcept {
    if (!HookWanted()) return;
    TakeChecks();
    if (HookSeh(which, id, actor, entity, alone) < 0) {
        LeverFault(kLKnow | kLCamp | kLTicket | kLRole | kLFace, which == 0 ? "the take (before)" : which == 1 ? "the take (after)" : which == 2 ? "the refreeze" : "the end of hers");
        g_take = TakeRec{};
    }
}
}  // namespace

void TakeBefore(std::uint32_t id, std::uintptr_t actor, bool alone) noexcept { Hook(0, id, actor, 0, alone); }
void TakeAfter(std::uint32_t id, std::uintptr_t actor, std::uintptr_t entity, bool alone) noexcept { Hook(1, id, actor, entity, alone); }
void Refreeze(std::uint32_t id, std::uintptr_t entity) noexcept { Hook(2, id, 0, entity, true); }
void TakeEnded(std::uint32_t id, bool given_back, bool world_gone) noexcept { Hook(3, id, 0, world_gone ? 1u : 0u, given_back); }

bool M2Probe(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!Hidden(msg, cap)) return false;
    if (!g_cfg.enemy_m2_probes) return std::snprintf(msg, cap, "enemy_m2_probes=0 (the M2 probes are off)"), false;
    if (!Ready(msg, cap)) return false;
    if (g_probe_off) return std::snprintf(msg, cap, "the probes are off after a fault"), false;
    TargetChecks();
    StimChecks();
    ProbeChecks();
    const int r = M2Seh(v, msg, cap);
    if (r < 0) {
        ProbeFault("the action");
        return std::snprintf(msg, cap, "FAULT"), false;
    }
    return r == 1;
}

}  // namespace st::ecombat
