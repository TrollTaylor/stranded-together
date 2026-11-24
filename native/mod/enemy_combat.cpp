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
#include <vector>

#include "automation.hpp"
#include "config_values.hpp"
#include "coop_session.hpp"
#include "enemy_census.hpp"
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

enum Group : unsigned { kGOnHit = 1, kGMule = 2, kGStamps = 4, kGApply = 8, kGOutcome = 16, kGEntry = 32 };
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
unsigned g_rx = 0, g_applied = 0, g_refused_by[erules::kHitResults] = {}, g_wrong_role = 0, g_bad_msg = 0;
struct HostRec {
    bool have = false;
    std::uint8_t defeat = 0, state = 0;
    std::uint16_t ver = 0;
    std::uint32_t ea4 = 0, last_seq = 0;
    float health = 0, stamina = 0;
    double last_hit_ms = -1e18;
    std::uintptr_t actor = 0;
};
std::map<std::uint32_t, HostRec> g_hout;
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
void SendOut(std::uint32_t guids) {
    std::uint8_t buf[1024];
    while (!g_res_out.empty()) {
        const unsigned n = static_cast<unsigned>(g_res_out.size() < ewire::kHitResMax ? g_res_out.size() : ewire::kHitResMax);
        ewire::ScbHeader h;
        h.guids_fp = guids, h.seq = g_scb_seq + 1;
        const std::size_t size = ewire::EncodeHitRes(h, g_res_out.data(), n, buf, sizeof(buf));
        if (!size) {
            g_res_out.erase(g_res_out.begin(), g_res_out.begin() + n);
            continue;
        }
        if (!link::SendCombat(buf, size)) break;  // the window is full: the next tick
        ++g_scb_seq;
        g_res_out.erase(g_res_out.begin(), g_res_out.begin() + n);
    }
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
void HostHit(const ewire::Hit& h, std::uint32_t msg_guids, const esync::GateView& gv) {
    ++g_rx;
    const std::uintptr_t actor = ActorOf(h.key);
    MuleNow m;
    const bool found = actor && ReadMule(actor, h.key, m);
    erules::ApplyIn in;
    in.host = g_role == 1;
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
        const std::uint8_t cause = static_cast<std::uint8_t>(!first && was == static_cast<std::uint8_t>(erules::Defeat::dead) ? erules::Cause::revive
                                                             : guest                                                            ? erules::Cause::guest
                                                                                                                                : erules::Cause::world);
        static unsigned logged = 0;
        if (LogNth(++logged) || guest)
            Log("[enemy] OUTCOME id=%05X %s -> %s (state %u, bits 0x%X; cause %s%s) ver %u; health %.1f stamina %.1f", ids[i], first ? "none" : erules::DefeatText(was),
                erules::DefeatText(d), m.ea0, m.ea4, erules::CauseText(cause), guest ? (" HIT #" + std::to_string(r.last_seq)).c_str() : "", r.ver, r.health, r.stamina);
        if (send) g_oc_out.push_back(OutcomeMsg(ids[i], r, cause));
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
            if (g_role != 1) {
                ++g_wrong_role;
                continue;
            }
            if (!ewire::DecodeHits(buf, n, h, hits)) {
                ++g_bad_msg;
                continue;
            }
            for (unsigned i = 0; i < h.n; ++i) HostHit(hits[i], h.guids_fp, gv);
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
            if (g_role != 2 || !ewire::DecodeHitRes(buf, n, h, res)) {
                ++(g_role != 2 ? g_wrong_role : g_bad_msg);
                continue;
            }
            for (unsigned i = 0; i < h.n; ++i) {
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
    g_actors.clear();
    g_actors_at = -1e18;
    g_ids_done = false;
    g_resync_due = true;
    Cap c;
    while (RingPop(c)) {
    }
}

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
        g_world_ready = ready, g_world = counter, g_gen = gen;
        ResetWorld();
    }
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
    g_capture.store(on && g_role == 2, std::memory_order_relaxed);
    // the captures (the guest; a solo run drains and forgets them)
    Cap c;
    unsigned took = 0;
    while (took < 64 && RingPop(c)) {
        ++took;
        if (g_role == 2) GuestCapture(c, on);
    }
    TestTick();
    if (in.net) DrainCombat(gv);
    if (g_role == 2 && on) {
        if (g_resync_due) {
            std::uint8_t buf[ewire::kScbHeader];
            ewire::ScbHeader h;
            h.guids_fp = gv.guids, h.seq = g_scb_seq + 1;
            if (ewire::EncodeResync(h, buf, sizeof(buf)) && link::SendCombat(buf, sizeof(buf))) ++g_scb_seq, g_resync_due = false, Log("[enemy] hits: RESYNC sent");
        }
        FlushHits(gv.guids);
    } else if (g_role == 2 && !g_out.empty()) {
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
    g_groups = (kGOnHit | kGMule | kGStamps | kGApply | kGOutcome | kGEntry) & ~bad;
    Log("[enemy] hits: byte checks %u/%u ok (onhit %s, muledmg %s, stamps %s, apply %s, outcome %s, entry %s)", ok, n, (bad & kGOnHit) ? "FAILED" : "ok",
        (bad & kGMule) ? "FAILED" : "ok", (bad & kGStamps) ? "FAILED" : "ok", (bad & kGApply) ? "FAILED" : "ok", (bad & kGOutcome) ? "FAILED" : "ok",
        (bad & kGEntry) ? "FAILED" : "ok");
    // the MULE handler (G3) is a backstop: its own failure leaves it out; every other group is needed
    return !(bad & ~static_cast<unsigned>(kGMule));
}

}  // namespace

// ---------------------------------------------------------------- public
void Install(const Config& config) noexcept {
    g_cfg = config;
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
    if (g_role != 2) return std::snprintf(msg, cap, "the guest's"), false;
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

}  // namespace st::ecombat
