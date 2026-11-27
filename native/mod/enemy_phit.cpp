// Round 28 P1 (docs/enemy-sync.md 17): the enemy attack probe. See enemy_phit.hpp; the pure parts are in enemy_phit_rules.hpp.
#include "enemy_phit.hpp"

#include <windows.h>

#include <intrin.h>

#include "MinHook.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <utility>

#include "automation.hpp"
#include "enemy_phit_rules.hpp"
#include "enemy_sync.hpp"
#include "enemy_wire.hpp"
#include "game_actions.hpp"
#include "link.hpp"
#include "log.hpp"
#include "partner_source.hpp"
#include "real_body.hpp"
#include "sam_damage.hpp"

namespace st::ephit {
namespace mem = partner::mem;
namespace pr = phrules;

namespace {

constexpr std::uintptr_t kImage = 0x140000000;
inline std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }
// a runtime address in ds.exe's image back to its VA, 0 when outside
inline std::uint64_t VaOf(std::uintptr_t a) noexcept { return a && mem::InImage(a) ? static_cast<std::uint64_t>(a - mem::Base() + kImage) : 0; }

// ---------------------------------------------------------------- the three targets (ds.exe VAs) and their byte checks
constexpr std::uintptr_t kSphere = 0x1429934E0;   // the attack sphere query
constexpr std::uintptr_t kCapsule = 0x14298A3F0;  // the attack capsule query
constexpr std::uintptr_t kGrab = 0x1424BAA70;     // MeleeAttackEventInstance Start (the paired grab)

struct Sig {
    std::uintptr_t va;
    const char* sig;
    const char* name;
    unsigned group;  // bit 0 sphere, 1 capsule, 2 grab
};
constexpr unsigned kGSphere = 1, kGCapsule = 2, kGGrab = 4;
// clang-format off
constexpr Sig kSigs[] = {
    // the sphere: the prologue partner_tackle checks too (its check runs at start-up, before this detour can go in)
    {kSphere,       "40 55 53 56 57 41 56 41 57 48 8D AC 24 C8 F4 FF FF 48 81 EC 38 0C 00 00", "the attack sphere query", kGSphere},
    // the capsule: the prologue, then its stack arguments as this detour forwards them (radius xmm3, a5..a9 on the stack)
    {kCapsule,      "48 8B C4 48 89 58 18 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 A8 F4 FF FF 48 81 EC 20 0C 00 00", "the attack capsule query", kGCapsule},
    {0x14298A42B,   "48 8B B5 90 0B 00 00", "the capsule's a7 read", kGCapsule},
    {0x14298A439,   "4C 8B B5 88 0B 00 00", "the capsule's a6 read (filter)", kGCapsule},
    {0x14298A443,   "C5 F8 28 FB", "the capsule's radius (xmm3)", kGCapsule},
    {0x14298A472,   "8B 85 A0 0B 00 00", "the capsule's a9 read", kGCapsule},
    // the grab: the prologue (this in rcx; the +0x30 gate) and the first actor's store at +0x60
    {kGrab,         "40 53 57 48 83 EC 28 BB 01 00 00 00 48 8B F9 89 59 34 E8 ?? ?? ?? ?? 80 7F 30 00 74 7A 0F B6 57 58", "the grab start", kGGrab},
    {0x1424BAAB3,   "48 89 47 60 83 78 34 02", "the grab's +0x60 actor store", kGGrab},
};
// The labelled call sites: an E8 to the expected callee (a mismatch only drops that label; it is logged as a failed check).
struct SiteSig {
    std::uintptr_t call_va, callee;
};
constexpr SiteSig kSiteSigs[] = {
    {0x142B53F99, 0x142993C70}, {0x142AF6090, 0x142993440}, {0x142917C7C, 0x1429933B0}, {0x14291803A, 0x1429933B0},
    {0x142917C22, kCapsule},    {0x142917F81, kCapsule},
};
// clang-format on

using FnSphere = std::uint64_t (*)(std::uintptr_t, const double*, float, float, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                   std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
using FnCapsule = std::uint64_t (*)(std::uintptr_t, const double*, const double*, float, std::uint64_t, std::uint64_t, std::uint64_t,
                                    std::uint64_t, std::uint64_t);
using FnGrab = std::uint64_t (*)(std::uintptr_t);
FnSphere o_sphere = nullptr;
FnCapsule o_capsule = nullptr;
FnGrab o_grab = nullptr;

// ---------------------------------------------------------------- state
bool g_key = false;                        // session.ini enemy_phit_probe
std::atomic<bool> g_stopped{false};        // the emergency stop: pass through
std::atomic<bool> g_watch{false};          // the detours record while on
std::atomic<unsigned> g_in{0};             // the groups whose detour is in
std::atomic<unsigned> g_wrap_ok{0};        // the wrappers whose frame is checked (outer caller resolution)
std::atomic<std::uint32_t> g_game_tid{0};  // the game thread (the tick's)
std::atomic<std::uintptr_t> g_mod_lo{0}, g_mod_hi{0};  // this module's image
unsigned g_checked = 0, g_check_n = 0, g_check_ok = 0;  // the byte checks: groups that passed; counts
unsigned g_site_ok = 0;                    // bit i: kSiteSigs[i] matched
bool g_checks_done = false, g_mh_tried = false, g_mh_ok = false;
unsigned g_tried = 0;
const char* g_text[3] = {"off (not yet)", "off (not yet)", "off (not yet)"};

// the bodies' feet, published by the tick (a seqlock over atomics: a torn read only mixes two ticks)
struct Bodies {
    std::atomic<std::uint32_t> seq{0};
    std::atomic<std::uint64_t> p[3], s[3];
    std::atomic<std::uint32_t> ok{0};  // bit 0 the partner body, bit 1 our Sam
} g_bodies;

// ---------------------------------------------------------------- the ring (many writers: any thread; one reader: the tick)
constexpr unsigned kRing = 2048;
constexpr unsigned kWords = 16;  // P1b: w13..w15 the attacker's pose (kFAtt)
struct Slot {
    std::atomic<std::uint64_t> seq{0};
    std::atomic<std::uint64_t> w[kWords];
};
Slot g_ring[kRing];
std::atomic<std::uint64_t> g_head{0};
std::uint64_t g_tail = 0;  // game thread
// flags (w2 bits 56..63)
constexpr std::uint8_t kFMod = 1, kFPbody = 2, kFSam = 4, kFParams = 8, kFMain = 16, kFOuter = 32, kFRes = 64, kFAtt = 128;

bool InModule(std::uintptr_t a) noexcept {
    const std::uintptr_t lo = g_mod_lo.load(std::memory_order_relaxed), hi = g_mod_hi.load(std::memory_order_relaxed);
    return lo && a >= lo && a < hi;
}

void ReadBodies(pr::V3* p, pr::V3* s, std::uint32_t* ok) noexcept {
    for (int tries = 0; tries < 3; ++tries) {
        const std::uint32_t a = g_bodies.seq.load(std::memory_order_acquire);
        if (a & 1) continue;
        *ok = g_bodies.ok.load(std::memory_order_relaxed);
        p->x = pr::BitsD(g_bodies.p[0].load(std::memory_order_relaxed));
        p->y = pr::BitsD(g_bodies.p[1].load(std::memory_order_relaxed));
        p->z = pr::BitsD(g_bodies.p[2].load(std::memory_order_relaxed));
        s->x = pr::BitsD(g_bodies.s[0].load(std::memory_order_relaxed));
        s->y = pr::BitsD(g_bodies.s[1].load(std::memory_order_relaxed));
        s->z = pr::BitsD(g_bodies.s[2].load(std::memory_order_relaxed));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_bodies.seq.load(std::memory_order_relaxed) == a) return;
    }
    *ok = 0;
}

// Reserve a slot (seq 0: not ready). Returns its index + 1 in *idx.
Slot& Reserve(std::uint64_t* idx) noexcept {
    const std::uint64_t i = g_head.fetch_add(1, std::memory_order_relaxed);
    Slot& s = g_ring[i & (kRing - 1)];
    s.seq.store(0, std::memory_order_relaxed);
    *idx = i;
    return s;
}
inline void W(Slot& s, unsigned k, std::uint64_t v) noexcept { s.w[k].store(v, std::memory_order_relaxed); }
inline void Publish(Slot& s, std::uint64_t i) noexcept { s.seq.store(i + 1, std::memory_order_release); }

// The caller: the inner return address (the detour's own return slot), through a checked wrapper to its caller.
void Caller(void* ret_slot, bool through_wrappers, std::uint32_t* inner_rva, std::uint32_t* outer_rva, std::uint8_t* flags) noexcept {
    const std::uintptr_t inner = *static_cast<const std::uintptr_t*>(ret_slot);  // our own stack
    *inner_rva = 0, *outer_rva = 0;
    if (InModule(inner)) {
        *flags |= kFMod;
        return;
    }
    const std::uint64_t iva = VaOf(inner);
    if (!iva) return;
    *inner_rva = static_cast<std::uint32_t>(iva - kImage);
    if (!through_wrappers) return;
    const int w = pr::WrapperOfReturn(iva);
    if (w < 0 || !(g_wrap_ok.load(std::memory_order_relaxed) & (1u << w))) return;
    std::uintptr_t outer = 0;
    if (!mem::Rd(reinterpret_cast<std::uintptr_t>(ret_slot) + 8 + static_cast<std::uintptr_t>(pr::kWrappers[w].frame), outer)) return;
    if (InModule(outer)) {
        *flags |= kFMod;
        return;
    }
    const std::uint64_t ova = VaOf(outer);
    if (!ova) return;
    *outer_rva = static_cast<std::uint32_t>(ova - kImage);
    *flags |= kFOuter;
}

// One attack query's record (before the original runs); the result is written after it.
void FillQuery(Slot& s, pr::Query q, void* ret_slot, std::uintptr_t ctx, const double* a, const double* b, float radius, float amount,
               std::uintptr_t filter) noexcept {
    std::uint8_t flags = 0;
    std::uint32_t inner = 0, outer = 0;
    Caller(ret_slot, q == pr::Query::sphere, &inner, &outer, &flags);
    W(s, 0, pr::Pack(inner, outer));
    W(s, 1, ctx);
    // the attack context's parameter block P = ctx + 0x120 (enemy_combat.cpp ReadHit's layout)
    std::uint32_t attacker = pr::kNoActor, power = 0, res_def = 0;
    std::uint16_t attack = 0;
    std::uint8_t p[0x68] = {};
    if (ctx && mem::Read(ctx + 0x120, p, sizeof(p))) {
        flags |= kFParams;
        std::memcpy(&attack, p + 0x0C, 2);
        std::memcpy(&attacker, p + 0x20, 4);
        std::memcpy(&power, p + 0x64, 4);
        std::uintptr_t res = 0;
        std::memcpy(&res, p + 0x18, 8);
        float d = 0;
        if (res && mem::Rd(res + 0x24, d)) flags |= kFRes, res_def = pr::FBits(d);
    }
    double ca[3] = {NAN, NAN, NAN}, cb[3] = {NAN, NAN, NAN};
    if (!a || !mem::Read(reinterpret_cast<std::uintptr_t>(a), ca, sizeof(ca))) ca[0] = ca[1] = ca[2] = NAN;
    if (b && !mem::Read(reinterpret_cast<std::uintptr_t>(b), cb, sizeof(cb))) cb[0] = cb[1] = cb[2] = NAN;
    std::uint32_t layer = 0xFFFFFFFF;
    if (filter) mem::Rd(filter + 8, layer);
    // the gaps to the two bodies
    pr::V3 pb, sm;
    std::uint32_t ok = 0;
    ReadBodies(&pb, &sm, &ok);
    const pr::V3 va{ca[0], ca[1], ca[2]}, vb{cb[0], cb[1], cb[2]};
    double gp = NAN, gs = NAN;
    if (ok & 1) gp = q == pr::Query::capsule ? pr::CapsuleGap(va, vb, radius, pb) : pr::SphereGap(va, radius, pb), flags |= kFPbody;
    if (ok & 2) gs = q == pr::Query::capsule ? pr::CapsuleGap(va, vb, radius, sm) : pr::SphereGap(va, radius, sm), flags |= kFSam;
    // P1b: the attacker's pose (ctx+0x68 holds its entity + 0x20, sam_damage.hpp; root +0xC8, facing = rot row 1 at +0xEC / +0xF0):
    // whether the strike was aimed at a body and how far it reached
    float att[6] = {NAN, NAN, NAN, NAN, NAN, NAN};  // to pbody d / deg, to centre d / deg, to Sam d / deg
    if (ctx) {
        const std::uintptr_t r = mem::Q(ctx + 0x68);
        double ap[3] = {};
        float fw[2] = {};
        if (r > 0x20 && mem::Read(r - 0x20 + 0xC8, ap, sizeof(ap)) && mem::Read(r - 0x20 + 0xEC, fw, sizeof(fw))) {
            const pr::V3 at{ap[0], ap[1], ap[2]};
            const double yaw = pr::YawOfFacing(fw[0], fw[1]);
            if (pr::Finite(at) && std::isfinite(yaw)) {
                flags |= kFAtt;
                const pr::V3 c = q == pr::Query::capsule ? pr::Mid(va, vb) : va;
                att[2] = static_cast<float>(pr::HorizDist(at, c)), att[3] = static_cast<float>(pr::FacingOffDeg(yaw, at, c));
                if (ok & 1) att[0] = static_cast<float>(pr::HorizDist(at, pb)), att[1] = static_cast<float>(pr::FacingOffDeg(yaw, at, pb));
                if (ok & 2) att[4] = static_cast<float>(pr::HorizDist(at, sm)), att[5] = static_cast<float>(pr::FacingOffDeg(yaw, at, sm));
            }
        }
    }
    const std::uint32_t tid = GetCurrentThreadId();
    if (tid == g_game_tid.load(std::memory_order_relaxed)) flags |= kFMain;
    W(s, 2, static_cast<std::uint64_t>(attacker) | (static_cast<std::uint64_t>(attack) << 32) | (static_cast<std::uint64_t>(q) << 48) |
                (static_cast<std::uint64_t>(flags) << 56));
    W(s, 3, pr::Pack(pr::FBits(radius), pr::FBits(amount)));
    W(s, 4, pr::DBits(ca[0]));
    W(s, 5, pr::DBits(ca[1]));
    W(s, 6, pr::DBits(ca[2]));
    W(s, 7, pr::Pack(pr::FBits(static_cast<float>(gp)), pr::FBits(static_cast<float>(gs))));
    W(s, 8, pr::Pack(layer, power));
    W(s, 9, pr::Pack(res_def, 0));
    W(s, 10, pr::DBits(cb[0]));
    W(s, 11, pr::DBits(cb[1]));
    W(s, 12, pr::Pack(static_cast<std::uint32_t>(GetTickCount64()), tid));
    W(s, 13, pr::Pack(pr::FBits(att[0]), pr::FBits(att[1])));
    W(s, 14, pr::Pack(pr::FBits(att[2]), pr::FBits(att[3])));
    W(s, 15, pr::Pack(pr::FBits(att[4]), pr::FBits(att[5])));
}
void Result(Slot& s, std::uint64_t ret) noexcept {
    const std::uint64_t w9 = s.w[9].load(std::memory_order_relaxed);
    W(s, 9, pr::Pack(pr::Lo(w9), static_cast<std::uint32_t>(ret)));
}

bool Recording() noexcept { return g_watch.load(std::memory_order_relaxed) && !g_stopped.load(std::memory_order_relaxed); }

// ---------------------------------------------------------------- E2: the PHIT candidates (any thread writes; the tick reads)
// The host only, while the relay is armed: a MULE melee query (phrules::PhitCandidate) whose shape reaches her partner body. The
// detours write the contact point, her feet (the record the gap was taken against), the strike's direction and the gap; fault-free
// reads only (ReadProcessMemory), atomics, no lock, no allocation, no logging.
std::atomic<bool> g_armed{false};             // the tick sets it (RelayArmed)
std::atomic<std::uintptr_t> g_pbody_ent{0};   // the partner body the bodies record holds (the grab's test)
constexpr unsigned kCRing = 256, kCWords = 10;
struct CSlot {
    std::atomic<std::uint64_t> seq{0};
    std::atomic<std::uint64_t> w[kCWords];
};
CSlot g_cring[kCRing];
std::atomic<std::uint64_t> g_chead{0};
std::uint64_t g_ctail = 0;                    // game thread
std::atomic<std::uint32_t> g_c_tested{0};     // MULE melee queries tested while armed (any body)
constexpr std::uint8_t kCGrab = 3;            // w0's kind for a grab engaged at her body (logged only)

bool Armed() noexcept { return g_armed.load(std::memory_order_relaxed) && !g_stopped.load(std::memory_order_relaxed); }

void CPut(std::uint64_t (&w)[kCWords]) noexcept {
    const std::uint64_t i = g_chead.fetch_add(1, std::memory_order_relaxed);
    CSlot& s = g_cring[i & (kCRing - 1)];
    s.seq.store(0, std::memory_order_relaxed);
    for (unsigned k = 0; k < kCWords; ++k) s.w[k].store(w[k], std::memory_order_relaxed);
    s.seq.store(i + 1, std::memory_order_release);
}

void Candidate(pr::Query q, void* ret_slot, std::uintptr_t ctx, const double* a, const double* b, float radius) noexcept {
    const std::uintptr_t inner = *static_cast<const std::uintptr_t*>(ret_slot);  // our own stack
    if (!ctx || InModule(inner)) return;  // the mod's own calls (the PHIT apply, the tackles, the hits) are never relayed
    std::uint8_t p[0x24] = {};
    if (!mem::Read(ctx + 0x120, p, sizeof(p))) return;
    std::uint16_t attack = 0;
    std::uint32_t attacker = pr::kNoActor;
    std::memcpy(&attack, p + 0x0C, 2);
    std::memcpy(&attacker, p + 0x20, 4);
    if (!pr::IsMuleId(attacker) || !pr::PhitAttackOk(attack)) return;
    g_c_tested.fetch_add(1, std::memory_order_relaxed);
    double ca[3] = {}, cb[3] = {};
    if (!a || !mem::Read(reinterpret_cast<std::uintptr_t>(a), ca, sizeof(ca))) return;
    const bool cap = q == pr::Query::capsule;
    if (cap && (!b || !mem::Read(reinterpret_cast<std::uintptr_t>(b), cb, sizeof(cb)))) return;
    pr::V3 pb, sm;
    std::uint32_t ok = 0;
    ReadBodies(&pb, &sm, &ok);
    if (!(ok & 1)) return;
    const pr::V3 va{ca[0], ca[1], ca[2]}, vb{cb[0], cb[1], cb[2]};
    if (!pr::Finite(va) || (cap && !pr::Finite(vb))) return;
    const double gap = cap ? pr::CapsuleGap(va, vb, radius, pb) : pr::SphereGap(va, radius, pb);
    if (!pr::PhitCandidate(false, attacker, attack, gap)) return;
    const pr::V3 c = pr::ContactPoint(cap, va, vb, pb);
    // the attacker's root (ctx+0x68 holds its entity + 0x20): the strike's direction; else the contact from her feet
    const std::uintptr_t r = mem::Q(ctx + 0x68);
    const std::uintptr_t e = r > 0x20 ? r - 0x20 : 0;
    double ap[3] = {};
    float dx = 0, dy = 0;
    if (e && mem::Read(e + 0xC8, ap, sizeof(ap))) pr::StrikeDir({ap[0], ap[1], ap[2]}, c, &dx, &dy);
    std::uint64_t w[kCWords];
    w[0] = static_cast<std::uint64_t>(attacker) | (static_cast<std::uint64_t>(attack) << 32) | (static_cast<std::uint64_t>(cap ? 2 : 1) << 48);
    w[1] = e;
    w[2] = pr::DBits(c.x), w[3] = pr::DBits(c.y), w[4] = pr::DBits(c.z);
    w[5] = pr::DBits(pb.x), w[6] = pr::DBits(pb.y), w[7] = pr::DBits(pb.z);
    w[8] = pr::Pack(pr::FBits(static_cast<float>(gap)), pr::FBits(dx));
    w[9] = pr::Pack(pr::FBits(dy), static_cast<std::uint32_t>(GetTickCount64()));
    CPut(w);
}

std::uint64_t H_Sphere(std::uintptr_t ctx, const double* centre, float radius, float amount, std::uint64_t a5, std::uint64_t a6,
                       std::uint64_t a7, std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11, std::uint64_t a12,
                       std::uint64_t a13, std::uint64_t a14) {
    const bool rec = Recording(), armed = Armed();
    if (!rec && !armed) return o_sphere(ctx, centre, radius, amount, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    void* const ret_slot = _AddressOfReturnAddress();
    if (armed) Candidate(pr::Query::sphere, ret_slot, ctx, centre, nullptr, radius);  // E2 (before the original: its inputs)
    if (!rec) return o_sphere(ctx, centre, radius, amount, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    std::uint64_t i = 0;
    Slot& s = Reserve(&i);
    FillQuery(s, pr::Query::sphere, ret_slot, ctx, centre, nullptr, radius, amount, static_cast<std::uintptr_t>(a5));
    const std::uint64_t ret = o_sphere(ctx, centre, radius, amount, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    Result(s, ret);
    Publish(s, i);
    return ret;
}

std::uint64_t H_Capsule(std::uintptr_t ctx, const double* a, const double* b, float radius, std::uint64_t a5, std::uint64_t a6,
                        std::uint64_t a7, std::uint64_t a8, std::uint64_t a9) {
    const bool rec = Recording(), armed = Armed();
    if (!rec && !armed) return o_capsule(ctx, a, b, radius, a5, a6, a7, a8, a9);
    void* const ret_slot = _AddressOfReturnAddress();
    if (armed) Candidate(pr::Query::capsule, ret_slot, ctx, a, b, radius);  // E2
    if (!rec) return o_capsule(ctx, a, b, radius, a5, a6, a7, a8, a9);
    std::uint64_t i = 0;
    Slot& s = Reserve(&i);
    FillQuery(s, pr::Query::capsule, ret_slot, ctx, a, b, radius, pr::BitsF(static_cast<std::uint32_t>(a5)), static_cast<std::uintptr_t>(a6));
    const std::uint64_t ret = o_capsule(ctx, a, b, radius, a5, a6, a7, a8, a9);
    Result(s, ret);
    Publish(s, i);
    return ret;
}

// An actor's entity (actor+0x60 holds entity+0x20; the grab's own test, decompile 0x1424BAA70), 0 none
std::uintptr_t ActorEntity(std::uintptr_t actor) noexcept {
    if (!actor) return 0;
    const std::uintptr_t r = mem::Q(actor + 0x60);
    return r ? r - 0x20 : 0;
}

std::uint64_t H_Grab(std::uintptr_t self) {
    const bool rec = Recording(), armed = Armed();
    if (!rec && !armed) return o_grab(self);
    void* const ret_slot = _AddressOfReturnAddress();
    const std::uint64_t ret = o_grab(self);
    if (armed && static_cast<std::uint32_t>(ret) == 0) {
        // E2: a grab engaged at her body is logged only (none in the P1 runs)
        const std::uintptr_t pb = g_pbody_ent.load(std::memory_order_relaxed);
        if (pb && (ActorEntity(mem::Q(self + 0x60)) == pb || ActorEntity(mem::Q(self + 0x68)) == pb)) {
            std::uint64_t w[kCWords] = {};
            w[0] = static_cast<std::uint64_t>(pr::kNoActor) | (static_cast<std::uint64_t>(kCGrab) << 48);
            w[1] = VaOf(*static_cast<const std::uintptr_t*>(ret_slot));
            w[9] = pr::Pack(0, static_cast<std::uint32_t>(GetTickCount64()));
            CPut(w);
        }
    }
    if (!rec) return ret;
    std::uint64_t i = 0;
    Slot& s = Reserve(&i);
    std::uint8_t flags = 0;
    std::uint32_t inner = 0, outer = 0;
    Caller(ret_slot, false, &inner, &outer, &flags);
    std::uint8_t b30 = 0;
    mem::Rd(self + 0x30, b30);
    const std::uintptr_t a60 = mem::Q(self + 0x60);
    // +0x68 is only written on the engaged path (ret 0); otherwise it is the last grab's
    const std::uintptr_t a68 = static_cast<std::uint32_t>(ret) == 0 ? mem::Q(self + 0x68) : 0;
    const std::uint32_t tid = GetCurrentThreadId();
    if (tid == g_game_tid.load(std::memory_order_relaxed)) flags |= kFMain;
    W(s, 0, pr::Pack(inner, outer));
    W(s, 1, self);
    W(s, 2, static_cast<std::uint64_t>(pr::kNoActor) | (static_cast<std::uint64_t>(pr::Query::grab) << 48) | (static_cast<std::uint64_t>(flags) << 56));
    W(s, 3, 0);
    W(s, 4, ActorEntity(a60));
    W(s, 5, ActorEntity(a68));
    W(s, 6, pr::Pack(static_cast<std::uint32_t>(ret), b30));
    for (unsigned k = 7; k < 12; ++k) W(s, k, 0);
    W(s, 12, pr::Pack(static_cast<std::uint32_t>(GetTickCount64()), tid));
    for (unsigned k = 13; k < kWords; ++k) W(s, k, 0);
    Publish(s, i);
    return ret;
}

// ---------------------------------------------------------------- byte checks and the hooks (game thread)
void Checks() noexcept {
    if (g_checks_done) return;
    g_checks_done = true;
    unsigned bad = 0;
    for (const Sig& s : kSigs) {
        ++g_check_n;
        if (mem::CodeMatches(A(s.va), s.sig)) {
            ++g_check_ok;
            continue;
        }
        bad |= s.group;
        Log("[phit] byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
    }
    g_checked = (kGSphere | kGCapsule | kGGrab) & ~bad;
    // the wrappers (outer caller resolution only): prologue mov r11, rsp; sub rsp, frame; their call to the sphere
    unsigned wok = 0;
    for (int w = 0; w < 4; ++w) {
        const pr::Wrapper& x = pr::kWrappers[w];
        char sig[64];
        std::snprintf(sig, sizeof(sig), "4C 8B DC 48 81 EC %02X 00 00 00", static_cast<unsigned>(x.frame));
        ++g_check_n;
        const bool ok = mem::CodeMatches(A(x.entry), sig) && mem::CodeMatches(A(x.call_va), "E8") &&
                        mem::RipTarget(A(x.call_va), 1, 5) == A(kSphere);
        if (ok) ++g_check_ok, wok |= 1u << w;
        else Log("[phit] byte check FAILED: the wrapper %s (its callers show as the wrapper)", x.name);
    }
    g_wrap_ok.store(wok);
    for (unsigned k = 0; k < sizeof(kSiteSigs) / sizeof(kSiteSigs[0]); ++k) {
        ++g_check_n;
        const SiteSig& c = kSiteSigs[k];
        if (mem::CodeMatches(A(c.call_va), "E8") && mem::RipTarget(A(c.call_va), 1, 5) == A(c.callee)) {
            ++g_check_ok, g_site_ok |= 1u << k;
        } else {
            Log("[phit] byte check FAILED: the call site 0x%llX (%s) does not call 0x%llX", static_cast<unsigned long long>(c.call_va),
                pr::SiteName(c.call_va), static_cast<unsigned long long>(c.callee));
        }
    }
}

// groups: the detours wanted (the relay's start-up: the sphere and capsule; the probe and its watch: all three, the grab too)
void HooksIn(unsigned groups = kGSphere | kGCapsule | kGGrab) noexcept {
    Checks();
    struct One {
        unsigned bit;
        std::uintptr_t va;
        void* det;
        void** orig;
    } const ones[3] = {{kGSphere, kSphere, reinterpret_cast<void*>(&H_Sphere), reinterpret_cast<void**>(&o_sphere)},
                       {kGCapsule, kCapsule, reinterpret_cast<void*>(&H_Capsule), reinterpret_cast<void**>(&o_capsule)},
                       {kGGrab, kGrab, reinterpret_cast<void*>(&H_Grab), reinterpret_cast<void**>(&o_grab)}};
    for (int i = 0; i < 3; ++i) {
        const unsigned bit = ones[i].bit;
        if (!(groups & bit)) continue;
        if (g_in.load() & bit) continue;
        if (!(g_checked & bit)) {
            g_text[i] = "off (byte checks)";
            continue;
        }
        if (g_tried & bit) continue;  // MinHook refused it once: not again (its text says why)
        if (!g_mh_tried) {
            g_mh_tried = true;
            const MH_STATUS init = MH_Initialize();
            g_mh_ok = init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED;
        }
        if (!g_mh_ok) {
            g_text[i] = "off (MinHook)";
            continue;
        }
        g_tried |= bit;
        MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(A(ones[i].va)), ones[i].det, ones[i].orig);
        if (s == MH_OK) s = MH_EnableHook(reinterpret_cast<void*>(A(ones[i].va)));
        if (s == MH_OK) g_in.fetch_or(bit), g_text[i] = "on";
        else g_text[i] = MH_StatusToString(s);
    }
}

void LogStatus() noexcept {
    Log("[phit] hooks: sphere %s capsule %s grab %s; byte checks %u/%u%s; wrappers 0x%X; watch %s", g_text[0], g_text[1], g_text[2], g_check_ok,
        g_check_n, g_check_ok == g_check_n ? " ok" : "", g_wrap_ok.load(), g_watch.load() ? "on" : "off");
}

// ---------------------------------------------------------------- the watch's numbers (game thread)
struct Totals {
    std::uint32_t calls = 0, sphere = 0, capsule = 0, mod = 0, mule = 0;
    std::uint32_t pbody = 0, pbody_mule = 0, sam = 0, sam_mule = 0, main = 0, worker = 0;
    std::uint32_t grabs = 0, engaged = 0, g_pbody = 0, g_sam = 0, g_other = 0;
    std::uint32_t lost = 0, jumps = 0;
    double max_step = 0, moved = 0;
};
struct Watch {
    double start = 0, end = 0, last_line = 0;
    Totals t;
    std::map<std::uint32_t, pr::Stat> by_attacker;
    std::map<std::uint64_t, pr::Stat> by_site;
    std::map<std::uint16_t, std::uint32_t> attacks;
    std::set<std::pair<std::uint64_t, std::uint64_t>> combos;
    unsigned combo_lines = 0, overlap_lines = 0, grab_lines = 0, call_lines = 0;
    bool have_sam = false, have_start = false;
    pr::V3 sam_last, sam_start;
};
Watch g_w;

const char* Who(std::uintptr_t e, std::uintptr_t pbody, std::uintptr_t sam) noexcept {
    if (!e) return "none";
    if (e == pbody) return "pbody";
    if (e == sam) return "sam";
    return "other";
}
const char* TypeOf(std::uintptr_t e) noexcept {
    if (!e) return "-";
    const char* n = mem::RttiName(mem::RttiOf(e));
    return n ? n : "?";
}
// The caller site's call VA: the outer caller's (through a wrapper) or the inner's; 0 the mod's own call
std::uint64_t SiteOf(std::uint64_t w0, std::uint8_t flags) noexcept {
    if (flags & kFMod) return 0;
    const std::uint32_t inner = pr::Lo(w0), outer = pr::Hi(w0);
    const std::uint32_t r = outer ? outer : inner;
    return r ? kImage + r - 5 : 1;  // 1: unknown (not in the image)
}
const char* SiteLabel(std::uint64_t site) noexcept { return site == 0 ? "mod" : site == 1 ? "unknown" : pr::SiteName(site); }

void Record(const std::uint64_t (&w)[kWords], std::uintptr_t pbody, std::uintptr_t sam) noexcept {
    const std::uint8_t flags = static_cast<std::uint8_t>(w[2] >> 56);
    const auto q = static_cast<pr::Query>((w[2] >> 48) & 0xFF);
    Totals& t = g_w.t;
    if (flags & kFMain) ++t.main;
    else ++t.worker;
    if (q == pr::Query::grab) {
        ++t.grabs;
        const std::uint32_t ret = pr::Lo(w[6]);
        const std::uintptr_t ea = static_cast<std::uintptr_t>(w[4]), eb = static_cast<std::uintptr_t>(w[5]);
        if (ret == 0) {
            ++t.engaged;
            if (ea == pbody || eb == pbody) ++t.g_pbody;
            else if (ea == sam || eb == sam) ++t.g_sam;
            else ++t.g_other;
        }
        if (g_w.grab_lines < 24) {
            ++g_w.grab_lines;
            const std::uint32_t inner = pr::Lo(w[0]);
            Log("[phit] grab: %s (ret %u, +0x30 %u) actor a %s (%s) b %s (%s) caller 0x%llX tid %s", ret == 0 ? "engaged" : "not engaged", ret,
                pr::Hi(w[6]), Who(ea, pbody, sam), TypeOf(ea), Who(eb, pbody, sam), TypeOf(eb),
                inner ? static_cast<unsigned long long>(kImage + inner - 5) : 0ull, (flags & kFMain) ? "main" : "worker");
        }
        return;
    }
    const std::uint32_t attacker = static_cast<std::uint32_t>(w[2] & 0xFFFFFFFFu);
    const std::uint16_t attack = static_cast<std::uint16_t>((w[2] >> 32) & 0xFFFF);
    const float radius = pr::BitsF(pr::Lo(w[3])), amount = pr::BitsF(pr::Hi(w[3]));
    const double gp = (flags & kFPbody) ? static_cast<double>(pr::BitsF(pr::Lo(w[7]))) : NAN;
    const double gs = (flags & kFSam) ? static_cast<double>(pr::BitsF(pr::Hi(w[7]))) : NAN;
    const std::uint32_t layer = pr::Lo(w[8]);
    const float power = pr::BitsF(pr::Hi(w[8]));
    const float res_def = pr::BitsF(pr::Lo(w[9]));
    const std::uint32_t ret = pr::Hi(w[9]);
    const std::uint64_t site = SiteOf(w[0], flags);
    const bool mule = pr::IsMuleId(attacker);
    ++t.calls;
    if (q == pr::Query::capsule) ++t.capsule;
    else ++t.sphere;
    if (flags & kFMod) ++t.mod;
    if (mule) ++t.mule;
    if (pr::Overlaps(gp)) ++t.pbody, t.pbody_mule += mule ? 1 : 0;
    if (pr::Overlaps(gs)) ++t.sam, t.sam_mule += mule ? 1 : 0;
    g_w.by_attacker[attacker].Add(q, attack, site, layer, attacker, radius, gp, gs, ret);
    g_w.by_site[site].Add(q, attack, site, layer, attacker, radius, gp, gs, ret);
    ++g_w.attacks[attack];
    // P1b: the attacker's pose (to pbody d / deg, to the centre d / deg, to Sam d / deg; NaN when not read)
    const bool posed = (flags & kFAtt) != 0;
    const double ap_d = posed ? pr::BitsF(pr::Lo(w[13])) : NAN, ap_o = posed ? pr::BitsF(pr::Hi(w[13])) : NAN;
    const double ac_d = posed ? pr::BitsF(pr::Lo(w[14])) : NAN, ac_o = posed ? pr::BitsF(pr::Hi(w[14])) : NAN;
    const double as_d = posed ? pr::BitsF(pr::Lo(w[15])) : NAN, as_o = posed ? pr::BitsF(pr::Hi(w[15])) : NAN;
    if (posed) {
        g_w.by_attacker[attacker].AddPose(ap_d, ap_o, as_o, ac_d);
        g_w.by_site[site].AddPose(ap_d, ap_o, as_o, ac_d);
    }
    // P1b: every MULE melee call (not only the first of a kind; a kick is ~5 calls), at most 40 lines a watch
    if ((pr::IsMuleMelee(attack) || pr::IsMuleSite(site)) && g_w.call_lines < 40) {
        ++g_w.call_lines;
        Log("[phit] call: %s site 0x%llX (%s) attacker id=%05X attack 0x%X %s r %.2f ret %u tid %s gap pbody %.2f sam %.2f; attacker %s: "
            "to pbody %.2f m %.0f deg, to centre %.2f m %.0f deg, to sam %.1f m %.0f deg; centre %.2f %.2f %.2f",
            pr::QueryName(q), static_cast<unsigned long long>(site), SiteLabel(site), attacker, attack, pr::AttackName(attack), radius, ret,
            (flags & kFMain) ? "main" : "worker", gp, gs, posed ? "posed" : "not read", ap_d, ap_o, ac_d, ac_o, as_d, as_o, pr::BitsD(w[4]),
            pr::BitsD(w[5]), pr::BitsD(w[6]));
    }
    const bool fresh = g_w.combos.insert({site, pr::Pack(attacker, attack)}).second;
    if (fresh && g_w.combo_lines < 48) {
        ++g_w.combo_lines;
        const std::uint32_t inner = pr::Lo(w[0]);
        const int wr = (flags & kFOuter) ? pr::WrapperOfReturn(kImage + inner) : -1;
        Log("[phit] new: %s site 0x%llX (%s%s%s) attacker id=%05X attack 0x%X %s layer 0x%X r %.2f amount %.1f (eff %.1f, power %.2f) "
            "centre %.1f %.1f %.1f ret %u tid %s gap pbody %.2f sam %.2f",
            pr::QueryName(q), static_cast<unsigned long long>(site), SiteLabel(site), wr >= 0 ? ", via " : "", wr >= 0 ? pr::kWrappers[wr].name : "",
            attacker, attack, pr::AttackName(attack), layer, radius, amount, pr::EffectiveAmount(amount, (flags & kFRes) != 0, res_def), power,
            pr::BitsD(w[4]), pr::BitsD(w[5]), pr::BitsD(w[6]), ret, (flags & kFMain) ? "main" : "worker", gp, gs);
    }
    if ((pr::Overlaps(gp) || pr::Overlaps(gs)) && g_w.overlap_lines < 16) {
        ++g_w.overlap_lines;
        Log("[phit] overlap %s: attacker id=%05X attack 0x%X %s site 0x%llX (%s) r %.2f ret %u gap pbody %.2f sam %.2f", pr::Overlaps(gp) ? "pbody" : "sam",
            attacker, attack, pr::AttackName(attack), static_cast<unsigned long long>(site), SiteLabel(site), radius, ret, gp, gs);
    }
}

std::uintptr_t g_last_pbody = 0, g_last_sam = 0;
double g_now = 0;  // the tick's clock (player_sync's ms): the watch's start and end use it, not GetTickCount64

void Drain(bool keep) noexcept {
    const std::uint64_t head = g_head.load(std::memory_order_acquire);
    if (head - g_tail > kRing) {  // overrun: the oldest ones are gone
        if (keep) g_w.t.lost += static_cast<std::uint32_t>(head - g_tail - kRing);
        g_tail = head - kRing;
    }
    while (g_tail < head) {
        Slot& s = g_ring[g_tail & (kRing - 1)];
        const std::uint64_t seq = s.seq.load(std::memory_order_acquire);
        if (seq != g_tail + 1) {
            if (head - g_tail > 256) {  // a writer that never finished (or one overwritten): skip it
                if (keep) ++g_w.t.lost;
                ++g_tail;
                continue;
            }
            break;  // still being written
        }
        std::uint64_t w[kWords];
        for (unsigned k = 0; k < kWords; ++k) w[k] = s.w[k].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        const bool same = s.seq.load(std::memory_order_relaxed) == seq;
        ++g_tail;
        if (!same) {
            if (keep) ++g_w.t.lost;
            continue;
        }
        if (keep) Record(w, g_last_pbody, g_last_sam);
    }
}

void AttacksText(char* out, std::size_t cap, const pr::Tally<6, std::uint16_t>& a) noexcept {
    std::size_t n = 0;
    out[0] = 0;
    for (unsigned i = 0; i < a.used && n < cap; ++i) {
        const int k = std::snprintf(out + n, cap - n, "%s0x%X %s x%u", i ? ", " : "", a.key[i], pr::AttackName(a.key[i]), a.n[i]);
        if (k < 0) break;
        n += static_cast<std::size_t>(k);
    }
    if (a.other && n < cap) std::snprintf(out + n, cap - n, ", +%u more", a.other);
}
void SitesText(char* out, std::size_t cap, const pr::Tally<4, std::uint64_t>& a) noexcept {
    std::size_t n = 0;
    out[0] = 0;
    for (unsigned i = 0; i < a.used && n < cap; ++i) {
        const int k = std::snprintf(out + n, cap - n, "%s0x%llX %s x%u", i ? ", " : "", static_cast<unsigned long long>(a.key[i]), SiteLabel(a.key[i]), a.n[i]);
        if (k < 0) break;
        n += static_cast<std::size_t>(k);
    }
    if (a.other && n < cap) std::snprintf(out + n, cap - n, ", +%u more", a.other);
}
void LayersText(char* out, std::size_t cap, const pr::Tally<3, std::uint32_t>& a) noexcept {
    std::size_t n = 0;
    out[0] = 0;
    for (unsigned i = 0; i < a.used && n < cap; ++i) {
        const int k = std::snprintf(out + n, cap - n, "%s0x%X", i ? "/" : "", a.key[i]);
        if (k < 0) break;
        n += static_cast<std::size_t>(k);
    }
}

void Summary(const char* why, double now) noexcept {
    Drain(true);
    char at[400], st[400], ly[64];
    unsigned lines = 0;
    for (const auto& [id, s] : g_w.by_attacker) {
        if (++lines > 24) break;
        AttacksText(at, sizeof(at), s.attacks);
        SitesText(st, sizeof(st), s.sites);
        LayersText(ly, sizeof(ly), s.layers);
        Log("[phit] attacker id=%05X (%s) calls %u (sphere %u capsule %u) pbody %u near %u min_pbody %.2f m sam %u near %u min_sam %.2f m "
            "hits %u attacks %s sites %s layer %s r %.2f..%.2f; posed %u: faced pbody %u sam %u (<= %.0f deg), att->pbody min %.2f m, reach max %.2f m",
            id, id == pr::kNoActor ? "none" : pr::IsMuleId(id) ? "mule" : "other", s.calls, s.sphere, s.capsule, s.pbody, s.pbody_near,
            s.min_pbody < 1e8 ? s.min_pbody : -1.0, s.sam, s.sam_near, s.min_sam < 1e8 ? s.min_sam : -1.0, s.hits, at, st, ly,
            s.r_max >= 0 ? s.r_min : 0.0f, s.r_max >= 0 ? s.r_max : 0.0f, s.posed, s.faced_pbody, s.faced_sam, pr::kFacedDeg,
            s.min_att_pbody < 1e8 ? s.min_att_pbody : -1.0, s.max_reach);
    }
    lines = 0;
    for (const auto& [site, s] : g_w.by_site) {
        if (++lines > 24) break;
        AttacksText(at, sizeof(at), s.attacks);
        LayersText(ly, sizeof(ly), s.layers);
        Log("[phit] site 0x%llX (%s) calls %u (sphere %u capsule %u) attackers %u pbody %u sam %u attacks %s layer %s",
            static_cast<unsigned long long>(site), SiteLabel(site), s.calls, s.sphere, s.capsule, s.attackers.used + s.attackers.other, s.pbody,
            s.sam, at, ly);
    }
    {
        std::size_t n = 0;
        char buf[600];
        buf[0] = 0;
        for (const auto& [a, c] : g_w.attacks) {
            if (n >= sizeof(buf)) break;
            const int k = std::snprintf(buf + n, sizeof(buf) - n, "%s0x%X %s x%u", n ? ", " : "", a, pr::AttackName(a), c);
            if (k < 0) break;
            n += static_cast<std::size_t>(k);
        }
        Log("[phit] attacks seen: %s", n ? buf : "none");
    }
    const Totals& t = g_w.t;
    Log("[phit] summary (%s): %.0f s, calls %u (sphere %u, capsule %u; mod %u; main %u, worker %u), mule calls %u, pbody overlaps %u (mule %u), "
        "sam overlaps %u (mule %u), grabs %u (engaged %u: at pbody %u, at Sam %u, other %u), sam jumps %u (max step %.1f m, moved %.1f m), lost %u",
        why, (now - g_w.start) / 1000.0, t.calls, t.sphere, t.capsule, t.mod, t.main, t.worker, t.mule, t.pbody, t.pbody_mule, t.sam, t.sam_mule,
        t.grabs, t.engaged, t.g_pbody, t.g_sam, t.g_other, t.jumps, t.max_step, t.moved, t.lost);
}

void Start(double secs, double now) noexcept {
    Drain(false);  // whatever was there before this watch
    g_w = Watch{};
    g_w.start = now, g_w.end = now + secs * 1000.0, g_w.last_line = now;
    g_watch.store(true);
}

bool ReadFeet(std::uintptr_t e, pr::V3* out) noexcept {
    double p[3] = {};
    if (!e || !mem::Read(e + 0xC8, p, sizeof(p))) return false;
    *out = {p[0], p[1], p[2]};
    return pr::Finite(*out);
}

// ---------------------------------------------------------------- E2: the PHIT relay (game thread)
int g_cfg_mode = 0;                 // session.ini enemy_partner_melee (phrules::PartnerMeleeMode)
int g_mode_override = -1;           // enemy_tgt 14 8 (hidden runs)
bool g_face_key = false;            // session.ini enemy_melee_face (enemy_target.cpp reads its own copy: redirect bit 64)
bool g_obs_in = false;              // our observer on Sam's damage handler
std::uint8_t g_role = 0;
std::atomic<std::uintptr_t> g_apply_ctx{0};       // the last PHIT apply's attack context (IsOwnApply)
std::atomic<std::uint64_t> g_apply_ms{0};
std::atomic<std::uint32_t> g_obs_msgs{0};          // our Sam's damage messages of that context (the observer)
std::atomic<std::uint32_t> g_obs_amount{0};        // the last one's amount (f32 bits)

int Mode() noexcept { return g_mode_override >= 0 ? g_mode_override : g_cfg_mode; }

struct RelayState {
    // the host
    std::uint32_t cands = 0, sent = 0, deduped = 0, capped = 0, not_ours = 0, no_world = 0, send_fail = 0, grabs = 0, stale = 0;
    std::uint32_t seq = 0, scb_seq = 0, hellos_rx = 0, hellos_off = 0;
    double hello_at = -1e18;
    bool hello_ok = false;          // the guest's last hello said she applies PHIT
    std::uint32_t hello_gen = 0;    // the link generation it came in (the host's arm is scoped to it)
    bool armed_was = false;
    const char* unarmed = "not yet";
    pr::PhitDedupe dd;
    pr::PhitRate rate;
    double line_at = 0;
    std::uint32_t line_sent = 0, line_cands = 0;
    // the guest
    double hello_next = 0;
    std::uint32_t tx_gen = 0, tx_guids = 0;  // the session and world of the last hello sent
    std::uint8_t tx_flags = 0;               // its flags (0: none sent this session, or the last said "no")
    std::uint32_t hellos_tx = 0, rx = 0, applied = 0, hurt = 0, nodrop = 0, dodged = 0, refused = 0, faults = 0, bad = 0, wrong_role = 0;
    std::uint32_t stale_rx = 0, deduped_rx = 0, capped_rx = 0;  // her own caps on what arrives (a channel-6 backlog lands at once)
    pr::PhitDedupe dd_rx;
    pr::PhitRate rate_rx;
    pr::PhitLag lag_rx;
    double dodge_test_m = 0;
    bool stopped = false;
} g_r;

struct Rx {
    ewire::Phit p;
    std::uint32_t msg_guids = 0, guids = 0;
    bool test = false;
};
constexpr unsigned kRxMax = 16;
Rx g_rx[kRxMax];
unsigned g_rx_n = 0;

struct Pending {
    bool on = false;
    std::uint32_t seq = 0, id = 0, msgs0 = 0;
    std::uint16_t attack = 0;
    float h0 = -1;
    double at = 0;
} g_pend;

float Health() noexcept {
    game::Diagnostics d;
    game::ReadDiagnostics(&d);
    return d.health_valid ? d.health : -1.0f;
}
bool LoadingNow() noexcept {
    std::uint64_t f = 0;
    std::uint32_t m = 0;
    return game::ReadStopGates(&f, &m) && m != 0;
}
void BeforeApply(std::uintptr_t ctx) noexcept {
    g_apply_ms.store(GetTickCount64(), std::memory_order_relaxed);
    g_apply_ctx.store(ctx, std::memory_order_release);
}
// the observer (the damage handler's thread; samdmg runs it inside its SEH guard): our own apply's messages
void ApplyObserver(std::uintptr_t sam, std::uintptr_t msg) {
    const samdmg::View v = samdmg::Read(sam, msg);
    if (!v.ctx || !IsOwnApply(v.ctx)) return;
    g_obs_amount.store(pr::FBits(v.amount), std::memory_order_relaxed);
    g_obs_msgs.fetch_add(1, std::memory_order_relaxed);
}

bool SendPhit(const ewire::Phit& p, bool need_world) noexcept {
    const esync::GateView gv = esync::Gate();
    if (need_world && !gv.guids) return false;
    ewire::ScbHeader h;
    h.guids_fp = gv.guids, h.seq = g_r.scb_seq + 1;
    std::uint8_t buf[ewire::kScbHeader + ewire::kPhitSize];
    const std::size_t size = ewire::EncodePhit(h, p, buf, sizeof(buf));
    if (!size || !link::SendCombat(buf, size)) return false;
    ++g_r.scb_seq;
    return true;
}

void FinishPending(double now, bool forced) noexcept {
    if (!g_pend.on) return;
    const float h = Health();
    const bool hurt = g_pend.h0 >= 0 && h >= 0 && h < g_pend.h0;
    if (!hurt && !forced && now - g_pend.at < 1000.0) return;
    g_pend.on = false;
    g_apply_ctx.store(0, std::memory_order_release);  // the watch is over: a later message at this context's address is not ours
    const std::uint32_t msgs = g_obs_msgs.load() - g_pend.msgs0;
    if (hurt) ++g_r.hurt;
    else ++g_r.nodrop;
    Log("[phit] PHIT applied #%u id=%05X attack 0x%X %s: health %.1f -> %.1f (%s), damage messages %u (amount %.1f)%s", g_pend.seq, g_pend.id,
        g_pend.attack, pr::AttackName(g_pend.attack), static_cast<double>(g_pend.h0), static_cast<double>(h), hurt ? "hurt" : "no drop", msgs,
        static_cast<double>(pr::BitsF(g_obs_amount.load())), g_obs_in ? "" : " (no damage observer)");
}

// The guest: one received (or test) strike on her real Sam.
void ApplyOne(const Rx& x, const ecombat::TickIn& in, double now) noexcept {
    const ewire::Phit& p = x.p;
    ++g_r.rx;
    if (!x.test) {
        // her own freshness test: channel 6 is reliable, so a stall's backlog arrives at once, far behind the best seen
        const std::uint32_t local = static_cast<std::uint32_t>(static_cast<std::uint64_t>(now > 0 ? now : 0));
        const std::int32_t lag = g_r.lag_rx.Observe(local - p.sender_ms, now);
        if (static_cast<double>(lag) > pr::kPhitStaleLagMs) {
            ++g_r.stale_rx;
            if (g_r.stale_rx <= 16)
                Log("[phit] PHIT stale #%u id=%05X attack 0x%X %s: %d ms behind the best seen (> %.0f ms): refused", p.seq, p.id, p.attack,
                    pr::AttackName(p.attack), static_cast<int>(lag), pr::kPhitStaleLagMs);
            return;
        }
    }
    const int mode = Mode();
    const std::uintptr_t sam = in.local_ok ? reinterpret_cast<std::uintptr_t>(in.local) : 0;
    pr::V3 sf;
    const bool sam_ok = sam && ReadFeet(sam, &sf);
    const pr::V3 her{p.her[0], p.her[1], p.her[2]};
    const pr::V3 point{her.x + p.point[0], her.y + p.point[1], her.z + p.point[2]};
    std::uintptr_t actor = 0, ent = 0;
    const bool mirror = esync::MirrorOf(p.id, &actor, &ent) && ent;
    pr::PhitIn g;
    g.key = mode != 0 && !g_r.stopped;
    g.world = x.test || (x.guids && x.msg_guids == x.guids);
    g.valid = pr::PhitValid(p.id, p.attack, point, her);
    g.sam = sam_ok;
    g.on_foot = sam_ok && mem::Q(sam + 0x70) == 0;  // no parent entity (a vehicle, a carrier)
    g.down = esync::OurDownNow();
    g.cutscene = esync::CutsceneNow() || LoadingNow();
    g.moved_m = sam_ok ? pr::Len(pr::Sub(sf, her)) + g_r.dodge_test_m : 0.0;
    g.need_owner = mode == 1, g.owner = mirror;
    const pr::PhitVerdict v = pr::PhitDecide(g);
    if (v == pr::PhitVerdict::dodged) {
        ++g_r.dodged;
        Log("[phit] PHIT dodged #%u id=%05X attack 0x%X %s: she moved %.1f m (> %.0f m) since the host saw it%s", p.seq, p.id, p.attack,
            pr::AttackName(p.attack), g.moved_m, pr::kPhitDodgeM, g_r.dodge_test_m > 0 ? " (the dodge test)" : "");
        return;
    }
    if (v != pr::PhitVerdict::apply) {
        ++g_r.refused;
        if (g_r.refused <= 24) Log("[phit] PHIT refused #%u id=%05X attack 0x%X %s: %s", p.seq, p.id, p.attack, pr::AttackName(p.attack), pr::PhitVerdictText(v));
        return;
    }
    // her own caps (the host's are on what it sends; these on what lands here in one tick): one per MULE + attack, the rate
    if (!x.test && !g_r.dd_rx.Take(p.id, p.attack, now)) {
        ++g_r.deduped_rx;
        return;
    }
    if (!x.test && !g_r.rate_rx.Take(p.id, now)) {
        ++g_r.capped_rx;
        if (g_r.capped_rx <= 16)
            Log("[phit] PHIT capped #%u id=%05X attack 0x%X %s: over the rate here (capped_rx)", p.seq, p.id, p.attack, pr::AttackName(p.attack));
        return;
    }
    FinishPending(now, true);
    ecombat::PhitApplyArgs a;
    a.owner = mode == 1 ? ent : 0;
    a.filter = ent ? ent : reinterpret_cast<std::uintptr_t>(in.body);
    a.sam = sam;
    a.attack = p.attack;
    a.centre[0] = sf.x, a.centre[1] = sf.y, a.centre[2] = sf.z + pr::kChestDz;
    a.radius = pr::kApplyRadius;
    a.amount = mode == 1 ? -1.0f : pr::PhitAmount(ecombat::PhitEntry28(p.attack));
    a.before = &BeforeApply;
    const float h0 = Health();
    const std::uint32_t msgs0 = g_obs_msgs.load();
    ecombat::PhitApplyOut o;
    const int r = ecombat::PhitApply(a, o);
    if (r < 0) {
        ++g_r.faults;
        Log("[phit] PHIT FAULT #%u id=%05X attack 0x%X: the apply faulted (step %d)%s", p.seq, p.id, p.attack, o.step,
            g_r.faults >= 3 ? "; the relay stops here" : "");
        if (g_r.faults >= 3) g_r.stopped = true;
        return;
    }
    if (r == 0) {
        ++g_r.refused;
        static const char* const kStep[9] = {"enemy hits' apply off", "no context", "not an attack context", "no attack entry", "?", "?", "?",
                                             "the query (its byte checks)", "the query held no part of our Sam"};
        if (g_r.refused <= 24)
            Log("[phit] PHIT refused #%u id=%05X attack 0x%X %s: %s (receivers %u)", p.seq, p.id, p.attack, pr::AttackName(p.attack),
                kStep[o.step >= 0 && o.step <= 8 ? o.step : 4], o.seen);
        return;
    }
    ++g_r.applied;
    Log("[phit] PHIT struck #%u id=%05X attack 0x%X %s%s: mode %d (attacker %s, amount %.1f), receivers %u kept %u part %d, moved %.1f m",
        p.seq, p.id, p.attack, pr::AttackName(p.attack), x.test ? " (test)" : "", mode, a.owner ? "its mirror" : "none",
        static_cast<double>(a.amount), o.seen, o.kept, static_cast<int>(o.part), g.moved_m);
    g_pend.on = true, g_pend.seq = p.seq, g_pend.id = p.id, g_pend.attack = p.attack, g_pend.h0 = h0, g_pend.at = now, g_pend.msgs0 = msgs0;
}

void DrainCands(double now, bool armed) noexcept {
    const std::uint64_t head = g_chead.load(std::memory_order_acquire);
    if (head - g_ctail > kCRing) g_ctail = head - kCRing;
    while (g_ctail < head) {
        CSlot& s = g_cring[g_ctail & (kCRing - 1)];
        const std::uint64_t seq = s.seq.load(std::memory_order_acquire);
        if (seq != g_ctail + 1) {
            if (head - g_ctail > 64) {
                ++g_ctail;
                continue;
            }
            break;
        }
        std::uint64_t w[kCWords];
        for (unsigned k = 0; k < kCWords; ++k) w[k] = s.w[k].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        const bool same = s.seq.load(std::memory_order_relaxed) == seq;
        ++g_ctail;
        if (!same || !armed) continue;
        const std::uint8_t kind = static_cast<std::uint8_t>((w[0] >> 48) & 0xFF);
        if (kind == kCGrab) {
            if (++g_r.grabs <= 8) Log("[phit] grab engaged at her body (caller 0x%llX): not relayed (logged only)", static_cast<unsigned long long>(w[1]));
            continue;
        }
        const std::uint32_t id = static_cast<std::uint32_t>(w[0] & 0xFFFFFFFFu);
        const std::uint16_t attack = static_cast<std::uint16_t>((w[0] >> 32) & 0xFFFF);
        const std::uintptr_t e = static_cast<std::uintptr_t>(w[1]);
        ++g_r.cands;
        if (static_cast<std::uint32_t>(GetTickCount64()) - pr::Hi(w[9]) > 1000u) {
            ++g_r.stale;
            continue;
        }
        // the dedupe on the query's own time (the drain can run a hitch after it)
        const std::uint32_t age = static_cast<std::uint32_t>(GetTickCount64()) - pr::Hi(w[9]);
        if (!g_r.dd.Take(id, attack, now - static_cast<double>(age))) {
            ++g_r.deduped;
            continue;
        }
        if (esync::OwnedHere(id) || (e && esync::IsFrozenMirror(e))) {
            ++g_r.not_ours;  // the guest's own copy fights her there
            continue;
        }
        if (!g_r.rate.Take(id, now)) {
            ++g_r.capped;
            continue;
        }
        ewire::Phit p;
        p.id = id, p.seq = g_r.seq + 1, p.attack = attack;
        p.kind = kind == 2 ? ewire::kPhitCapsule : ewire::kPhitSphere;
        const pr::V3 c{pr::BitsD(w[2]), pr::BitsD(w[3]), pr::BitsD(w[4])}, her{pr::BitsD(w[5]), pr::BitsD(w[6]), pr::BitsD(w[7])};
        p.point[0] = static_cast<float>(c.x - her.x), p.point[1] = static_cast<float>(c.y - her.y), p.point[2] = static_cast<float>(c.z - her.z);
        p.her[0] = her.x, p.her[1] = her.y, p.her[2] = her.z;
        const float gap = pr::BitsF(pr::Lo(w[8]));
        p.depth_m = std::isfinite(gap) && gap < 0 ? -gap : 0.0f;
        p.dir[0] = pr::BitsF(pr::Hi(w[8])), p.dir[1] = pr::BitsF(pr::Lo(w[9]));
        p.sender_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(now > 0 ? now : 0));
        if (!esync::Gate().guids) {
            ++g_r.no_world;
            continue;
        }
        if (!SendPhit(p, true)) {
            ++g_r.send_fail;
            if (g_r.send_fail <= 4) Log("[phit] PHIT #%u id=%05X: not sent (it does not encode, or the link did not take it)", p.seq, id);
            continue;
        }
        ++g_r.seq, ++g_r.sent;
        if (g_r.sent <= 32)
            Log("[phit] PHIT sent #%u id=%05X attack 0x%X %s (%s) depth %.2f m, her at %.1f %.1f %.1f", p.seq, id, attack, pr::AttackName(attack),
                kind == 2 ? "capsule" : "sphere", static_cast<double>(p.depth_m), her.x, her.y, her.z);
    }
}

void RelayLine(const char* when) noexcept {
    if (g_role == 1)
        Log("[phit] relay (host)%s%s: %s; queries tested %u, at her %u, sent %u, deduped %u, capped %u, not ours %u, stale %u, no world %u, send "
            "failed %u, grabs at her %u; hellos %u (no %u)",
            when ? " " : "", when ? when : "", g_armed.load() ? "armed" : g_r.unarmed, g_c_tested.load(), g_r.cands, g_r.sent, g_r.deduped,
            g_r.capped, g_r.not_ours, g_r.stale, g_r.no_world, g_r.send_fail, g_r.grabs, g_r.hellos_rx, g_r.hellos_off);
    else
        Log("[phit] relay (guest)%s%s: mode %d%s; hellos sent %u; PHIT rx %u, applied %u (hurt %u, no drop %u), dodged %u, refused %u, faults %u, bad %u; "
            "stale_rx %u, deduped_rx %u, capped_rx %u; apply path %s",
            when ? " " : "", when ? when : "", Mode(), g_r.stopped ? " (STOPPED)" : "", g_r.hellos_tx, g_r.rx, g_r.applied, g_r.hurt, g_r.nodrop,
            g_r.dodged, g_r.refused, g_r.faults, g_r.bad, g_r.stale_rx, g_r.deduped_rx, g_r.capped_rx,
            ecombat::PhitApplyReady() ? "ready" : "not ready");
}

void RelayTick(const ecombat::TickIn& in, double now) noexcept {
    const int mode = Mode();
    if (g_role == 1) {
        const char* why = nullptr;
        if (!mode) why = "unarmed (enemy_partner_melee=0)";
        else if (g_r.stopped) why = "unarmed (stopped)";
        else if (!in.net) why = "unarmed (no link)";
        else if (!(g_in.load() & (kGSphere | kGCapsule))) why = "unarmed (no query detour)";
        else if (!g_r.hello_ok || g_r.hello_gen != link::Generation()) why = "unarmed (no hello from the guest this session: her build, her key or her apply path)";
        else if (!(g_bodies.ok.load() & 1)) why = "unarmed (no partner body)";
        const bool armed = why == nullptr;
        g_armed.store(armed, std::memory_order_release);
        if (!armed) g_r.unarmed = why;
        if (armed != g_r.armed_was) {
            g_r.armed_was = armed;
            Log("[phit] relay (host): %s", armed ? "armed (her game takes PHIT)" : why);
        }
        DrainCands(now, armed);
        if (armed && now - g_r.line_at >= 10000.0 && (g_r.sent != g_r.line_sent || g_r.cands != g_r.line_cands)) {
            g_r.line_at = now, g_r.line_sent = g_r.sent, g_r.line_cands = g_r.cands;
            RelayLine(nullptr);
        }
    } else {
        g_armed.store(false, std::memory_order_relaxed);
        DrainCands(now, false);
    }
    // the guest's hello: once per link session and world, again when what it says changes, then only every 30 s (an older host
    // counts each one as a bad message). It says "applies PHIT" only while her combat drain and apply group run (else the strikes
    // would be dropped unread), and "no" once when that stops, so the host disarms.
    if (g_role == 2 && in.net) {
        const std::uint32_t gen = link::Generation(), guids = esync::Gate().guids;
        if (gen != g_r.tx_gen) g_r.tx_gen = gen, g_r.tx_flags = 0, g_r.tx_guids = 0, g_r.hello_next = 0;  // a new session: nothing said yet
        const bool ready = mode && !g_r.stopped && guids && ecombat::PhitApplyReady();
        const std::uint8_t want = ready ? ewire::kPhitCapApply : 0;
        const bool due = want != g_r.tx_flags || (want && (guids != g_r.tx_guids || now >= g_r.hello_next));
        if (due) {
            ewire::Phit h;
            h.kind = ewire::kPhitHello, h.flags = want;
            h.sender_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(now > 0 ? now : 0));
            if (SendPhit(h, false)) {
                const bool changed = want != g_r.tx_flags;
                g_r.tx_flags = want, g_r.tx_guids = guids, g_r.hello_next = now + pr::kPhitHelloRepeatMs;
                ++g_r.hellos_tx;
                if (changed && want) Log("[phit] hello sent (guest: this game applies PHIT, mode %d)", mode);
                else if (changed)
                    Log("[phit] hello sent (guest: this game no longer applies PHIT: %s)",
                        !mode ? "mode 0" : g_r.stopped ? "stopped" : !guids ? "no world" : "the combat drain or apply path is off");
            }
        }
    }
    // the guest's strikes (and the local test's)
    for (unsigned i = 0; i < g_rx_n; ++i) ApplyOne(g_rx[i], in, now);
    g_rx_n = 0;
    FinishPending(now, false);
}

}  // namespace

void Install(const Config& c) noexcept {
    g_key = c.enemy_phit_probe;
    g_cfg_mode = pr::PartnerMeleeMode(c.enemy_partner_melee);
    g_face_key = c.enemy_melee_face;
    if (!g_key && !g_cfg_mode) {
        if (g_face_key) Log("[phit] enemy_melee_face=1 without enemy_partner_melee: redirect bit 64 never acts (the PHIT relay is off)");
        return;
    }
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&Install), &m) &&
        m) {
        const auto base = reinterpret_cast<std::uintptr_t>(m);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + static_cast<std::uintptr_t>(dos->e_lfanew));
        if (dos->e_magic == IMAGE_DOS_SIGNATURE && nt->Signature == IMAGE_NT_SIGNATURE)
            g_mod_lo.store(base), g_mod_hi.store(base + nt->OptionalHeader.SizeOfImage);
    }
    if (g_key)
        Log("[phit] attack probe key on (logs only; the hooks go in at enemy_tgt 14 1 / 14 3, hidden runs only; module range %s)", g_mod_lo.load() ? "ok" : "unknown");
    if (!g_cfg_mode) return;
    // the relay needs enemy sync (the guest's hello carries its world; mode 1 needs a mirror) in a two-game session: no detours
    // and no damage observer otherwise (solo, echo, host_save=0 without enemy_sync; review round 28 defaults)
    if (!c.enemy_sync || (c.role != Role::host && c.role != Role::guest)) {
        Log("[phit] relay off: enemy_partner_melee=%d needs enemy sync in a two-game session (role %s, enemy_sync %d)", g_cfg_mode,
            RoleName(c.role), c.enemy_sync ? 1 : 0);
        g_cfg_mode = 0;
        return;
    }
    // E2: the detours at start-up (dllmain's install phase, after vhit's byte checks of the sphere prologue); without the module
    // range the mod's own queries could not be told apart: no hooks then
    if (!mem::Base() || !g_mod_lo.load()) {
        Log("[phit] relay key on (enemy_partner_melee=%d) but %s: the relay is off", g_cfg_mode, mem::Base() ? "no module range" : "no game image");
        g_cfg_mode = 0;
        return;
    }
    // only the host observes and relays the hits: the guest gets no detour from the key (its watch and probe ops put theirs in),
    // and the grab detour is the probe's and the watch's alone
    if (c.role == Role::host) {
        HooksIn(kGSphere | kGCapsule);
        if (!(g_in.load() & kGGrab)) g_text[2] = "off (the probe's)";
    } else {
        Checks();  // the byte checks alone (the status line's numbers)
        g_text[0] = g_text[1] = g_text[2] = "off (the guest: not needed)";
    }
    g_obs_in = samdmg::Ensure("ephit") && samdmg::Observe(&ApplyObserver);
    Log("[phit] relay key on: enemy_partner_melee=%d (%s), enemy_melee_face=%d; sphere %s capsule %s grab %s; byte checks %u/%u%s; damage observer %s",
        g_cfg_mode, g_cfg_mode == 1 ? "the MULE's mirror, the game's amount" : "no attacker, the entry's amount", g_face_key ? 1 : 0, g_text[0], g_text[1],
        g_text[2], g_check_ok, g_check_n, g_check_ok == g_check_n ? " ok" : "", g_obs_in ? "on" : samdmg::Installed() ? "table full" : samdmg::OffReason());
}

void Tick(const ecombat::TickIn& in, double now) noexcept {
    if ((!g_key && !g_cfg_mode) || g_stopped.load()) return;
    g_game_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_now = now;
    g_role = in.role;
    if (!g_in.load()) {
        // the guest needs no hook; op 6's apply test reads our Sam from here (only the hooked path set it before)
        g_last_sam = in.local_ok ? reinterpret_cast<std::uintptr_t>(in.local) : 0;
        if (g_cfg_mode) RelayTick(in, now);
        return;
    }
    // the bodies: the partner body (the live real body only) and our Sam
    const std::uintptr_t live = reinterpret_cast<std::uintptr_t>(realbody::LiveEntity());
    const std::uintptr_t pbody = in.body && reinterpret_cast<std::uintptr_t>(in.body) == live ? live : 0;
    const std::uintptr_t sam = in.local_ok ? reinterpret_cast<std::uintptr_t>(in.local) : 0;
    pr::V3 pf, sf;
    const bool pok = ReadFeet(pbody, &pf), sok = ReadFeet(sam, &sf);
    g_bodies.seq.fetch_add(1, std::memory_order_acq_rel);
    if (pok) for (int k = 0; k < 3; ++k) g_bodies.p[k].store(pr::DBits(k == 0 ? pf.x : k == 1 ? pf.y : pf.z), std::memory_order_relaxed);
    if (sok) for (int k = 0; k < 3; ++k) g_bodies.s[k].store(pr::DBits(k == 0 ? sf.x : k == 1 ? sf.y : sf.z), std::memory_order_relaxed);
    g_bodies.ok.store((pok ? 1u : 0u) | (sok ? 2u : 0u), std::memory_order_relaxed);
    g_bodies.seq.fetch_add(1, std::memory_order_release);
    g_pbody_ent.store(pok ? pbody : 0, std::memory_order_relaxed);
    g_last_pbody = pok ? pbody : 0, g_last_sam = sok ? sam : 0;
    if (g_cfg_mode) RelayTick(in, now);
    const bool watching = g_watch.load();
    Drain(watching);
    if (!watching) return;
    // our Sam's steps: a jump (> 3 m in a tick) is a teleport (a paired grab warps the bound player)
    if (sok) {
        if (g_w.have_sam) {
            const double step = pr::Len(pr::Sub(sf, g_w.sam_last));
            if (step > g_w.t.max_step) g_w.t.max_step = step;
            if (step > pr::kJumpStepM) {
                ++g_w.t.jumps;
                if (g_w.t.jumps <= 4) Log("[phit] sam jump %.1f m (to %.1f %.1f %.1f)", step, sf.x, sf.y, sf.z);
            }
        }
        if (!g_w.have_start) g_w.have_start = true, g_w.sam_start = sf;
        const double moved = pr::Len(pr::Sub(sf, g_w.sam_start));
        if (moved > g_w.t.moved) g_w.t.moved = moved;
        g_w.sam_last = sf, g_w.have_sam = true;
    }
    if (now - g_w.last_line >= 5000.0) {
        g_w.last_line = now;
        const Totals& t = g_w.t;
        Log("[phit] watch %.0f s: calls %u (mule %u) pbody overlaps %u sam overlaps %u grabs %u (engaged %u) pbody %s sam %s", (now - g_w.start) / 1000.0,
            t.calls, t.mule, t.pbody, t.sam, t.grabs, t.engaged, pok ? "ok" : "none", sok ? "ok" : "none");
    }
    if (now >= g_w.end) {
        g_watch.store(false);
        Summary("timeout", now);
    }
}

bool Command(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!mem::Base()) return std::snprintf(msg, cap, "no game image"), false;
    if (!g_key && !g_cfg_mode) return std::snprintf(msg, cap, "the attack probe and the PHIT relay are off (enemy_phit_probe=0, enemy_partner_melee=0)"), false;
    if (g_stopped.load()) return std::snprintf(msg, cap, "the attack probe is stopped (the emergency stop)"), false;
    const int op = static_cast<int>(v[1]);
    if (op != 3 && !automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only (op 3: the status)"), false;
    const bool relay_op = op >= 5 && op <= 8;
    if (relay_op && !g_cfg_mode) return std::snprintf(msg, cap, "the PHIT relay is off (session.ini enemy_partner_melee=0)"), false;
    if (!relay_op && op != 3 && !g_key) return std::snprintf(msg, cap, "the attack probe is off (session.ini enemy_phit_probe=0)"), false;
    const double now = g_now;  // the last tick's (Command runs on the game thread between ticks)
    switch (op) {
        case 5: {
            g_r.dodge_test_m = v[2] > 0 && v[2] < 1000 ? v[2] : 0.0;
            Log("[phit] dodge test: %s", g_r.dodge_test_m > 0 ? "on" : "off");
            return std::snprintf(msg, cap, "dodge test %.1f m", g_r.dodge_test_m), true;
        }
        case 6: {
            if (g_role != 2) return std::snprintf(msg, cap, "the guest only"), false;
            const std::uint16_t attack = static_cast<std::uint16_t>(v[2] > 0 && v[2] < 65536 ? v[2] : 0x16F);
            const std::uint32_t id = esync::LookId();
            const std::uintptr_t sam = g_last_sam;
            pr::V3 sf;
            if (!pr::IsMuleId(id) || !pr::PhitAttackOk(attack)) return std::snprintf(msg, cap, "no enemy_look MULE, or attack 0x%X not relayed", attack), false;
            if (!sam || !ReadFeet(sam, &sf)) return std::snprintf(msg, cap, "no Sam"), false;
            if (g_rx_n >= kRxMax) return std::snprintf(msg, cap, "the queue is full"), false;
            Rx& x = g_rx[g_rx_n++];
            x = Rx{};
            x.test = true;
            x.p.id = id, x.p.seq = 0, x.p.attack = attack, x.p.kind = ewire::kPhitSphere;
            x.p.her[0] = sf.x, x.p.her[1] = sf.y, x.p.her[2] = sf.z;
            x.p.point[2] = static_cast<float>(pr::kChestDz);
            Log("[phit] apply test queued: id=%05X attack 0x%X %s", id, attack, pr::AttackName(attack));
            return std::snprintf(msg, cap, "apply test queued (id %05X attack 0x%X)", id, attack), true;
        }
        case 7: {
            RelayLine("status");
            return std::snprintf(msg, cap, "relay status logged"), true;
        }
        case 8: {
            const int m = static_cast<int>(v[2]);
            g_mode_override = m >= 0 && m <= 2 ? m : -1;
            Log("[phit] apply mode for this run: %d%s", Mode(), g_mode_override < 0 ? " (the key)" : "");
            return std::snprintf(msg, cap, "mode %d", Mode()), true;
        }
        case 1: {
            HooksIn();
            LogStatus();
            if (!g_in.load()) return std::snprintf(msg, cap, "no hook went in (see [phit] hooks)"), false;
            if (g_watch.load()) Summary("restart", now);
            const double secs = pr::WatchSecs(v[2]);
            Start(secs, now);
            Log("[phit] watch on: %.0f s (sphere %s, capsule %s, grab %s)", secs, g_text[0], g_text[1], g_text[2]);
            return std::snprintf(msg, cap, "attack watch on, %.0f s", secs), true;
        }
        case 0: {
            if (!g_watch.load()) return std::snprintf(msg, cap, "no watch on"), false;
            g_watch.store(false);
            Summary("off", now);
            return std::snprintf(msg, cap, "attack watch off (summary logged)"), true;
        }
        case 2: {
            if (!g_watch.load()) return std::snprintf(msg, cap, "no watch on"), false;
            Summary("now", now);
            return std::snprintf(msg, cap, "summary logged (the watch goes on)"), true;
        }
        case 3: {
            // the hooks go in mid-game (MinHook suspends every thread; the sphere and capsule run on job threads) only in hidden
            // runs; elsewhere op 3 is the status alone (byte checks, what is in)
            const bool hidden = automation::Enabled();
            if (hidden) HooksIn();
            else Checks();
            LogStatus();
            return std::snprintf(msg, cap, "sphere %s, capsule %s, grab %s; byte checks %u/%u%s", g_text[0], g_text[1], g_text[2], g_check_ok,
                                 g_check_n, hidden ? "" : " (status only: the hooks go in in hidden runs)"),
                   hidden ? g_in.load() != 0 : true;
        }
        default: return std::snprintf(msg, cap, "enemy_tgt 14 <1 [s] | 0 | 2 | 3 | 5 <m> | 6 <attack> | 7 | 8 <mode>>"), false;
    }
}

void EmergencyStop() noexcept {
    g_stopped.store(true);
    g_watch.store(false);
    g_armed.store(false);
}

void OnScb(const std::uint8_t* d, std::size_t n, std::uint32_t guids) noexcept {
    ewire::ScbHeader h;
    ewire::Phit p;
    if (!ewire::DecodePhit(d, n, h, p)) {
        ++g_r.bad;
        return;
    }
    if (!g_cfg_mode || g_stopped.load()) return;
    if (p.kind == ewire::kPhitHello) {
        if (g_role != 1) {
            ++g_r.wrong_role;
            return;
        }
        if (!(p.flags & ewire::kPhitCapApply)) {
            ++g_r.hellos_off;
            if (g_r.hello_ok) Log("[phit] hello from the guest: her game no longer applies PHIT");
            g_r.hello_ok = false;
            return;
        }
        g_r.hello_at = g_now;
        const std::uint32_t gen = link::Generation();  // DrainCombat keeps only this generation's messages
        const bool fresh = !g_r.hello_ok || g_r.hello_gen != gen;
        g_r.hello_ok = true, g_r.hello_gen = gen;
        ++g_r.hellos_rx;
        if (fresh) Log("[phit] hello from the guest: her game applies PHIT");
        return;
    }
    if (g_role != 2) {
        ++g_r.wrong_role;
        return;
    }
    if (g_rx_n >= kRxMax) {
        ++g_r.refused;
        return;
    }
    Rx& x = g_rx[g_rx_n++];
    x = Rx{};
    x.p = p, x.msg_guids = h.guids_fp, x.guids = guids;
}

bool RelayArmed() noexcept { return Armed(); }

bool IsOwnApply(std::uintptr_t ctx) noexcept {
    if (!ctx || g_apply_ctx.load(std::memory_order_acquire) != ctx) return false;
    const std::uint64_t at = g_apply_ms.load(std::memory_order_relaxed), now = GetTickCount64();
    return now >= at && now - at <= 2000;
}

}  // namespace st::ephit
