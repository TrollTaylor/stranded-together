// Round 27 M1: the L1 probe (enemy_l1_probe.hpp; docs/enemy-sync.md 15). Hidden runs only; no gameplay change.
#include "enemy_l1_probe.hpp"

#include <windows.h>

#include "MinHook.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "automation.hpp"
#include "enemy_census.hpp"
#include "enemy_l1_rules.hpp"
#include "enemy_sense_hooks.hpp"
#include "enemy_stim_core.hpp"
#include "enemy_sync.hpp"
#include "enemy_target.hpp"
#include "game_actions.hpp"
#include "log.hpp"
#include "partner_holo.hpp"
#include "partner_source.hpp"
#include "partner_tackle.hpp"
#include "real_body.hpp"
#include "shot_sync.hpp"

namespace st::l1probe {
namespace {
namespace mem = partner::mem;
namespace R = l1rules;
using namespace stimcore;  // the shared stimulus primitives (enemy_stim_core.hpp)
using game::WorldTransform;

constexpr std::uintptr_t kImage = 0x140000000ull;
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }
std::uint64_t RvaOf(std::uintptr_t va) noexcept { return mem::InImage(va) ? va - mem::Base() : 0; }

// ---------------------------------------------------------------- game addresses (ds.exe, Director's Cut; round 27 research)
constexpr std::uintptr_t kStimMgr = 0x147BE0D38;     // the AI manager: +0x80 the visual pool, +0x88 its live list, +0x3C398 the thread-safe copies
constexpr std::uintptr_t kVisCreate = 0x1425249A0;   // (pool, resource, const pos f64[3]*, const vel f32[4]*, agent) -> stimulus
constexpr std::uintptr_t kStimRemove = 0x14254CE00;  // (pool, stimulus): a persistent one back to its pool (nothing when +0x84 != 0,
                                                     // +0x08 null or resource +0x2C != -1)
constexpr std::uintptr_t kResGet = 0x141DDE990;      // a streaming ref's object (a temporary reference, dropped before it returns)
constexpr std::uintptr_t kHumResSite = 0x1437321D5;  // the Humanoid's own stimulus: getter(entity + 0x58) -> +0x3B0 ...
constexpr std::uintptr_t kHumCreateSite = 0x143732253;  // ... the create with it, kept at +0x4A8
constexpr std::uintptr_t kTsSite = 0x142E4F340;      // a reader of the thread-safe visual list ([mgr] + 0x3C398)
constexpr std::uintptr_t kCls = 0x142E5FA40;         // the sense's classifier (sense, stim, results, int* type, i64* entity, u32 own id)
constexpr std::uintptr_t kClsSub = 0x142E5FAB9;      // its actor type switch
constexpr std::uintptr_t kAppend = 0x142E59860;      // a sensor result append (array, record*): record +0 the stimulus, array - 8 the type
constexpr std::uintptr_t kPsr = 0x142E8DAF0;         // DSAINoticeAndKnowledge ProcessSensorResults (K, ...): K+0x40 / +0x48 its buckets
constexpr std::uintptr_t kPsrT1a = 0x142E8DD50, kPsrT1b = 0x142E8DD89;  // its type-1 branch (count +8, records +0x10)
constexpr std::uintptr_t kKnowVt = 0x143E13028;      // DSAINoticeAndKnowledge's vtable (vt+0x38 ProcessSensorResults)
constexpr std::uintptr_t kGoalFind = 0x1425BD930;    // (individual + 0x3670, rtti*) -> the goal or 0
constexpr std::uintptr_t kShootRtti = 0x144C929D0, kShootSite = 0x142F00EB0;  // the shoot goal: +0x28 id, +0x40 mode, +0x48 f64[3]
constexpr std::uintptr_t kAimRtti = 0x144C922A0, kAimSite = 0x142F0DF7F;      // the aim goal: +0x28 the point f64[3]
constexpr std::uintptr_t kDaemonVt = 0x143E15298, kDaemonFn = 0x142EAA340;    // the PlayerInfo daemon: +0x18 the player, +0x40 its centre
constexpr std::uintptr_t kSingletons = 0x147BFAB60, kSingletonSite = 0x142EA05D8;  // the classifier's singleton table (+0x10, +0x18)
// an agent's points: vt+0x80 count (the cone's `mov rax,[rcx]; call [rax+0x80]` at 0x14251D43D), vt+0x88 one (its reload of +0x10)
constexpr std::uintptr_t kAgentCountSite = 0x14251D43D, kAgentPointSite = 0x14251D45A;
constexpr std::uintptr_t kSetPhase = 0x142ACF630;    // SetPhase(actor, phase)
constexpr std::uintptr_t kCpForce = 0x142B649A0;     // MuleCP_ForceSetCPPhase(const u64 guid[2]*, int phase)
constexpr std::uintptr_t kCtxCreate = 0x14298AC90;   // the attack context (owner | 0, u16 attack)
constexpr std::uintptr_t kCtxParams = 0x14294DDF0;   // its parameter block P = ctx + 0x120 (after its type check; enemy_combat's)
constexpr std::uintptr_t kAttackReg = 0x147BEB0F8, kAttackRegSite = 0x142912274;  // the attack registry (+0x60 n, +0x68 entries; +0x20 id)
constexpr std::uintptr_t kRegGlobal = 0x147BBB8C0;   // the game actor registry (vt+0x40: the local player's actor id)
constexpr std::uintptr_t kRegSite = 0x142E5FB61;     // the classifier's vehicle case reads it and calls vt+0x40 (the local player's id)
constexpr std::uintptr_t kMuleVt = 0x143DB7078;      // DSMuleGameActor

constexpr std::uintptr_t kSRes = 0x08, kSAgent = 0x10, kSPos = 0x18, kSLife = 0x80, kSOff = 0x84, kSId = 0x88, kSVel = 0x90, kSA0 = 0xA0, kSMul = 0xA4,
                         kSB0 = 0xB0, kSNext = 0xC8;
constexpr std::uintptr_t kMgrPool = 0x80, kMgrLive = 0x88, kMgrTs = 0x3C398, kPoolCs = 0x1A020;  // the visual pool's lock (pool + 0x1A020)
constexpr std::uint32_t kNoActor = 0x1FFFFF;  // "no actor": the classifier rejects it (type 0xFF > 0x21 at 0x142E5FABF)
constexpr std::uintptr_t kEAgent = 0x170, kObjRes = 0x3B0, kResLife = 0x2C;
constexpr std::uintptr_t kActId = 0x08, kActInUse = 0x20, kActState = 0x24, kActEntRef = 0x68, kActPhase = 0x110, kActDown = 0xEA4;
constexpr std::uintptr_t kIndK = 0x3AD8, kIndGoals = 0x3670, kIndPlanner = 0x3830;
constexpr std::uintptr_t kKX = 0x28, kXAgent = 0x118, kKBucketN = 0x590, kKBuckets = 0x598, kKeType = 0x18, kKePos = 0x20, kKeAge = 0x40, kKeActor = 0x68,
                         kKeSize = 0x80;
constexpr std::uintptr_t kCpGuidRef = 0x18, kCpPh = 0x210, kCpAlert = 0x230;
constexpr int kTypePV = 1, kTypeLK = 85;
constexpr std::uint16_t kDefaultMelee = 0x157;
constexpr unsigned kListCap = 600;

enum Group : unsigned { kGStim = 1, kGCls = 2, kGApp = 4, kGPsr = 8, kGGoal = 16, kGDaemon = 32, kGPhase = 64, kGMelee = 128, kGRead = 256, kGReg = 512 };
struct Site {
    std::uintptr_t va;
    const char* sig;
    const char* name;
    unsigned group;
};
// clang-format off
constexpr Site kSites[] = {
    {kVisCreate,      "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 40 81 B9 10 A0 01 00 00 02 00 00", "the visual stimulus create", kGStim},
    {kStimRemove,     "48 89 74 24 20 57 48 83 EC 30 80 BA 84 00 00 00 00 48 8B FA 48 8B F1 0F 85", "the persistent stimulus remove", kGStim},
    {kResGet,         "48 89 5C 24 20 57 48 83 EC 20 48 8B 01 33 C9 89 4C 24 30 48 89 74 24 40 48 8B 38 48 85 FF 74 33", "the streaming ref getter", kGStim},
    {kHumResSite,     "48 8D 4E 58 E8 ?? ?? ?? ?? 48 83 B8 B0 03 00 00 00", "the Humanoid's resource (+0x58 -> +0x3B0)", kGStim},
    {kHumCreateSite,  "48 8B 90 B0 03 00 00 E8 ?? ?? ?? ?? 48 89 86 A8 04 00 00", "the Humanoid's stimulus create", kGStim},
    {kTsSite,         "48 8B 1D ?? ?? ?? ?? 48 8B 9B 98 C3 03 00", "the thread-safe visual list (+0x3C398)", kGStim},
    {kCls,            "40 55 53 56 57 41 54 41 56 41 57 48 8B EC 48 83 EC 60 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 F0 8B 9A 88 00 00 00", "the classifier", kGCls},
    {kClsSub,         "C1 EF 0D 83 FF 21 0F 87 1F 04 00 00", "the classifier's actor type switch", kGCls},
    {kAppend,         "48 89 5C 24 20 56 48 83 EC 20 48 89 7C 24 38 48 8B F2 8B 39 48 8B D9 48 8B 51 08 FF C7 3B 79 04", "the result append", kGApp},
    {kPsr,            "4C 8B DC 55 53 57 49 8D 6B 88 48 81 EC 60 01 00 00 C4 C1 78 29 73 B8", "ProcessSensorResults", kGPsr},
    {kPsrT1a,         "83 7B 08 00 0F 84", "its type-1 bucket count (+8)", kGPsr},
    {kPsrT1b,         "4C 8B 43 10 BA 01 00 00 00", "its type-1 records (+0x10)", kGPsr},
    {kGoalFind,       "40 53 48 83 EC 30 48 8D 99 B0 00 00 00 48 89 54 24 20 48 8B CB 48 8D 54 24 20 E8", "the goal lookup", kGGoal},
    {kShootSite,      "48 8D 15 ?? ?? ?? ??", "the shoot goal's type", kGGoal},
    {kAimSite,        "48 8D 15 ?? ?? ?? ?? E8", "the aim goal's type and lookup", kGGoal},
    {kDaemonFn,       "40 53 48 81 EC 90 00 00 00 48 8B D9", "the PlayerInfo daemon's vt+0x40", kGDaemon},
    {kSetPhase,       "3B 91 10 01 00 00 74 0A C7 81 18 01 00 00 00 00 80 3F 8D 42 FD 89 91 10 01 00 00 83 F8 02 77 0A", "SetPhase", kGPhase},
    {kCpForce,        "40 57 48 81 EC A0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 88 00 00 00 8B FA 48 8B D1", "ForceSetCPPhase", kGPhase},
    {kCtxCreate,      "48 89 5C 24 18 48 89 74 24 20 55 57 41 56 48 8D 6C 24 80 48 81 EC 80 01 00 00 48 8B 05 7F 63 5D", "the attack context", kGMelee},
    {kCtxParams,      "40 53 48 83 EC 20 48 8B D9 48 85 C9 74 49 48 8B 01 FF 10 44 0F B7 05 55", "the context's parameters", kGMelee},
    {kAttackRegSite,  "4C 8B 1D ?? ?? ?? ??", "the attack registry's read", kGMelee},
    {kRegSite,        "48 8B 0D ?? ?? ?? ?? 48 8D 55 C0 48 8B 01 FF 50 40", "the registry's player id read (vt+0x40)", kGReg},
    {kSingletonSite,  "4C 8B 15 ?? ?? ?? ??", "the singleton table", kGRead},
    {kAgentCountSite, "48 8B 01 FF 90 80 00 00 00 44 8B E0", "the agent's point count (vt+0x80)", kGRead},
    {kAgentPointSite, "48 8B 4F 10 44 8B C2 48 8D 54 24 50 48 8B 01 FF 90 88 00 00 00 C5 F8 10 00", "the agent's point (vt+0x88)", kGRead},
};
// clang-format on

// ---------------------------------------------------------------- the counting detours (any thread: atomics only)
// (the detours themselves: enemy_sense_hooks.cpp, shared with the target pick; they call SenseCls / SenseApp / SensePsr below)
std::atomic<bool> g_off{false};
std::atomic<bool> g_counting{false};  // the probe's first op installed (or found) the hooks
// the detours' match: the agents her / Sam's stimuli carry (the sensor walks the thread-safe COPIES, rebuilt every frame, so a
// copy is matched by its +0x10 agent, never by the live stimulus' address; review round 27 M1). Set by the tick: the host's
// her = the agent of her stimulus of ours, Sam = our Sam's agent; the guest's her = its own Sam's agent, Sam = the partner body's
// agent while a stimulus of ours carries it.
std::atomic<std::uintptr_t> g_match_her{0}, g_match_sam{0}, g_watch_k{0};
// her stimulus of ours for the emergency stop (any thread): its address, agent, resource and manager
std::atomic<std::uintptr_t> g_her_stim{0}, g_her_agent{0}, g_her_res{0}, g_her_mgr{0};
constexpr unsigned kBins = 10;  // types 0..7, 8 another, 9 (the classifier) refused
std::atomic<unsigned> g_cls[3][kBins], g_app[3][kBins], g_rec0[3];  // static: zero
std::atomic<unsigned> g_psr_calls{0}, g_psr_t1{0}, g_dfault{0};

unsigned Bin(std::int64_t t) noexcept { return t >= 0 && t < 8 ? static_cast<unsigned>(t) : 8u; }
int SehI32(std::uintptr_t a, std::int32_t* out) noexcept {
    __try {
        *out = *reinterpret_cast<volatile std::int32_t*>(a);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
int SehPtr(std::uintptr_t a, std::uintptr_t* out) noexcept {
    __try {
        *out = *reinterpret_cast<volatile std::uintptr_t*>(a);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
int WhoAgent(std::uintptr_t a) noexcept {
    if (!a) return 0;
    if (a == g_match_her.load(std::memory_order_relaxed)) return R::kHer;
    if (a == g_match_sam.load(std::memory_order_relaxed)) return R::kSam;
    return 0;
}
// a stimulus copy (the sensor's list): whose, by the agent it carries
int WhoStim(std::uintptr_t s) noexcept {
    std::uintptr_t a = 0;
    return s && SehPtr(s + 0x10, &a) ? WhoAgent(a) : 0;
}
void CountCls(std::uintptr_t stim, std::uintptr_t r, int* type_out) noexcept {
    if (g_off.load(std::memory_order_relaxed) || !g_counting.load(std::memory_order_relaxed)) return;
    if (const int w = WhoStim(stim)) {
        unsigned bin = 9;
        std::int32_t t = -1;
        if ((r & 0xFF) && type_out) bin = SehI32(reinterpret_cast<std::uintptr_t>(type_out), &t) ? Bin(t) : 8u;
        g_cls[w][bin].fetch_add(1, std::memory_order_relaxed);
    }
}
void CountApp(std::uintptr_t array, std::uintptr_t record) noexcept {
    if (!g_off.load(std::memory_order_relaxed) && g_counting.load(std::memory_order_relaxed) && array && record) {
        std::uintptr_t stim = 0;
        if (SehPtr(record, &stim)) {
            if (const int w = WhoStim(stim)) {
                std::int32_t t = -1;
                g_app[w][SehI32(array - 8, &t) ? Bin(static_cast<std::uint32_t>(t)) : 8u].fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            g_dfault.fetch_add(1, std::memory_order_relaxed);
        }
    }
}
// the watched K's type-1 bucket before the game processes it: is record[0]'s stimulus hers or Sam's
void PsrScan(std::uintptr_t k) noexcept {
    __try {
        const std::int32_t n = *reinterpret_cast<volatile std::int32_t*>(k + 0x40);
        const std::uintptr_t arr = *reinterpret_cast<volatile std::uintptr_t*>(k + 0x48);
        if (n <= 0 || n > 64 || !arr) return;
        for (std::int32_t i = 0; i < n; ++i) {
            const std::uintptr_t b = reinterpret_cast<volatile std::uintptr_t*>(arr)[i];
            if (!b || *reinterpret_cast<volatile std::uint32_t*>(b) != 1u || *reinterpret_cast<volatile std::int32_t*>(b + 8) <= 0) continue;
            g_psr_t1.fetch_add(1, std::memory_order_relaxed);
            const std::uintptr_t rec = *reinterpret_cast<volatile std::uintptr_t*>(b + 0x10);
            const std::uintptr_t stim = rec ? *reinterpret_cast<volatile std::uintptr_t*>(rec) : 0;
            g_rec0[WhoStim(stim)].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_dfault.fetch_add(1, std::memory_order_relaxed);
    }
}
void CountPsr(std::uintptr_t k) noexcept {
    if (!g_off.load(std::memory_order_relaxed) && g_counting.load(std::memory_order_relaxed) && k && k == g_watch_k.load(std::memory_order_relaxed)) {
        g_psr_calls.fetch_add(1, std::memory_order_relaxed);
        PsrScan(k);
    }
}
struct Counts {
    unsigned cls[3][kBins] = {}, app[3][kBins] = {}, rec0[3] = {}, psr = 0, t1 = 0, fault = 0;
};
Counts ReadCounts() noexcept {
    Counts c;
    for (unsigned w = 0; w < 3; ++w) {
        for (unsigned b = 0; b < kBins; ++b) c.cls[w][b] = g_cls[w][b].load(), c.app[w][b] = g_app[w][b].load();
        c.rec0[w] = g_rec0[w].load();
    }
    c.psr = g_psr_calls.load(), c.t1 = g_psr_t1.load(), c.fault = g_dfault.load();
    return c;
}
unsigned Sum(const unsigned (&a)[kBins], const unsigned (&b)[kBins]) noexcept {
    unsigned n = 0;
    for (unsigned i = 0; i < kBins; ++i) n += a[i] - b[i];
    return n;
}
// "t0 n, t1 n, ..." of the non-zero bins of a - b
void BinText(char* out, std::size_t cap, const unsigned (&a)[kBins], const unsigned (&b)[kBins]) noexcept {
    std::size_t at = 0;
    out[0] = 0;
    for (unsigned i = 0; i < kBins && at < cap; ++i) {
        const unsigned n = a[i] - b[i];
        if (!n) continue;
        const char* sep = at ? "," : "";
        const int w = i < 8 ? std::snprintf(out + at, cap - at, "%st%u:%u", sep, i, n) : std::snprintf(out + at, cap - at, "%s%s:%u", sep, i == 9 ? "refused" : "other", n);
        if (w > 0) at += static_cast<std::size_t>(w);
    }
    if (!at) std::snprintf(out, cap, "-");
}

// ---------------------------------------------------------------- fault-free reads, SEH calls (POD only)
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
bool ReadXf(std::uintptr_t e, WorldTransform& out) noexcept {
    std::uint8_t b[0x3C];
    if (!e || !mem::Read(e + 0xC8, b, sizeof(b))) return false;
    std::memcpy(&out.pos, b, 0x18);
    std::memcpy(&out.rot, b + 0x18, 0x24);
    return std::isfinite(out.pos.x) && std::isfinite(out.pos.y) && std::isfinite(out.pos.z);
}
bool Pos(std::uintptr_t e, double (&p)[3]) noexcept {
    WorldTransform t{};
    if (!ReadXf(e, t)) return false;
    p[0] = t.pos.x, p[1] = t.pos.y, p[2] = t.pos.z;
    return true;
}
double YawOf(const WorldTransform& t) noexcept { return std::atan2(static_cast<double>(t.rot.m[1][1]), static_cast<double>(t.rot.m[1][0])); }
bool Finite3(const double (&p)[3]) noexcept { return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) && (p[0] != 0 || p[1] != 0 || p[2] != 0); }
const char* NameOf(std::uintptr_t o) noexcept {
    const char* n = o ? mem::RttiName(mem::RttiOf(o)) : nullptr;
    return n ? n : (o ? "?" : "-");
}
std::uintptr_t CompOf(std::uintptr_t e, const char* name) noexcept { return e ? partner::FindComponentByName(reinterpret_cast<game::Entity*>(e), name) : 0; }

int SehAgentCount(std::uintptr_t agent, std::uintptr_t fn) noexcept {
    __try {
        return reinterpret_cast<int (*)(std::uintptr_t)>(fn)(agent);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}
int SehAgentPoint(std::uintptr_t agent, std::uintptr_t fn, int i, std::uint8_t (&out)[32]) noexcept {
    __try {
        alignas(16) std::uint8_t buf[64] = {};
        const std::uint8_t* p = reinterpret_cast<const std::uint8_t* (*)(std::uintptr_t, void*, int)>(fn)(agent, buf, i);
        if (!p) return 0;
        std::memcpy(out, p, sizeof(out));
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
int SehSetPhase(std::uintptr_t actor, int phase) noexcept {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, int)>(A(kSetPhase))(actor, phase);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
int SehCpForce(const std::uint64_t (&guid)[2], int phase) noexcept {
    __try {
        reinterpret_cast<void (*)(const std::uint64_t*, int)>(A(kCpForce))(guid, phase);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// the attack registry's entry for an attack id (enemy_combat's AttackEntry)
std::uintptr_t AttackEntry(std::uint16_t attack) noexcept {
    const std::uintptr_t reg = mem::Q(A(kAttackReg));
    std::int32_t n = 0;
    if (!reg || !mem::Rd(reg + 0x60, n) || n <= 0 || n > 4096) return 0;
    const std::uintptr_t list = mem::Q(reg + 0x68);
    for (std::int32_t i = 0; list && i < n; ++i) {
        const std::uintptr_t e = mem::Q(list + 8ull * static_cast<unsigned>(i));
        std::uint16_t id = 0;
        if (e && mem::Rd(e + 0x20, id) && id == attack) return e;
    }
    return 0;
}
// The query's attack context (review S12/C7): made with no owner, its parameter block checked by the game's own accessor
// (P == ctx + 0x120: its type check passed) and stamped as enemy_combat's ApplyCall does: +0x0C the attack, +0x18 the attack's
// registry entry when missing or another's. 1 ready; 0 refused (*step 1 no context, 2 the parameters, 3 no registry entry); -1 a
// fault.
int SehCtx(std::uint16_t attack, std::uintptr_t entry, std::uintptr_t* out, int* step) noexcept {
    *out = 0;
    __try {
        *step = 1;
        const std::uintptr_t ctx = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t, std::uint16_t)>(A(kCtxCreate))(0, attack);
        if (!ctx) return 0;
        *step = 2;
        const std::uintptr_t P = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(A(kCtxParams))(ctx);
        if (P != ctx + 0x120) return 0;
        if (*reinterpret_cast<volatile std::uint16_t*>(P + 0x0C) != attack) *reinterpret_cast<volatile std::uint16_t*>(P + 0x0C) = attack;
        *step = 3;
        const std::uintptr_t res = *reinterpret_cast<volatile std::uintptr_t*>(P + 0x18);
        if (!res || *reinterpret_cast<volatile std::uint16_t*>(res + 0x20) != attack) {
            if (!entry) return 0;
            *reinterpret_cast<volatile std::uintptr_t*>(P + 0x18) = entry;
        }
        *out = ctx;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// (the pool lock, the atomic stores, the create / remove calls and the release's first step: enemy_stim_core.hpp)

// ---------------------------------------------------------------- state (game thread)
double g_now = 0;
std::uint64_t g_tick = 0;
bool g_checked = false, g_detours_tried = false;
unsigned g_groups = 0, g_detours = 0;
std::uint8_t g_role = 0;
std::uintptr_t g_local = 0, g_body = 0;  // our Sam, the partner body (this tick)
bool g_local_ok = false;
// a create or remove faulted (review S11): the pool's lock may be held for good; no further creates or removes this run
bool g_stim_dead = false;
unsigned g_releases = 0;

struct Her {
    std::uintptr_t stim = 0, res = 0, agent = 0, body = 0, mgr = 0;
    bool pending = false;   // the release's first step is done: a later tick removes or forgets
    bool wait_logged = false;
    std::uint64_t pending_tick = 0;
    bool vis_forced = false;
    float vis = 1.f;
    double t0 = 0, last_at = 0;
    double last_pos[3] = {};
    bool have_last = false;
    std::uint32_t id88 = 0;
} g_her;
unsigned g_made = 0;
struct SamStim {
    std::uintptr_t stim = 0, agent = 0;
    double at = -1e18;
} g_sam;

void StimDead(const char* what) noexcept {
    if (!g_stim_dead) Log("[probe] L1 %s FAULTED: no further stimulus creates or removes this run", what);
    g_stim_dead = true;
}
void Publish() noexcept {
    g_her_res.store(g_her.res, std::memory_order_relaxed);
    g_her_mgr.store(g_her.mgr, std::memory_order_relaxed);
    g_her_agent.store(g_her.agent, std::memory_order_relaxed);
    g_her_stim.store(g_her.stim, std::memory_order_release);
}
void Unpublish() noexcept {
    g_her_stim.store(0, std::memory_order_release);
    g_her_agent.store(0, std::memory_order_relaxed);
    g_her_res.store(0, std::memory_order_relaxed);
    g_her_mgr.store(0, std::memory_order_relaxed);
}
void Forget(const char* why) noexcept {
    Log("[probe] L1 hers forgotten (%s; not removed)", why);
    Unpublish();
    g_her = Her{};
}

// our Sam's live visual stimulus (by his agent; cached, re-found every 2 s when it no longer matches)
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
// the ownership signatures (review S3/C5): live = her agent with Sam's resource (no stimulus of the game's pairs them);
// parked (after step 1) = no agent, the resource, "no actor"
bool OursLive(std::uintptr_t s) noexcept { return stimcore::OursLive(s, g_her.agent, g_her.res); }
bool OursParked(std::uintptr_t s) noexcept { return stimcore::OursParked(s, g_her.res); }

// ---- the release (C1): step 1 at once, step 2 on a later tick
// Step 1 (any caller, before her body or its agent can be freed): the sensors stop taking her stimulus without its agent pointer
// going null under a sensor (review S1/C2: the cone 0x14251D2E0 loads +0x10 twice, at 0x14251D40D and 0x14251D45A, and a
// null or foreign agent crashes in 0x142380C70). The live stimulus: +0xA4 0, +0x88 "no actor" (the classifier rejects it
// before the cone), +0x84 1, then +0x10 cleared by a compare-exchange (only while it still holds her agent). Its thread-safe
// copies (rebuilt every frame from the live list; matched by her agent and the resource): +0xA4 0 and +0x88 "no actor" only;
// their +0x10 is never written. Nothing is freed here.
using stimcore::Step1;
void Release(const char* why) noexcept {
    if (!g_her.stim || g_her.pending) return;
    ++g_releases;
    Unpublish();
    const std::uintptr_t mgr = mem::Q(A(kStimMgr));
    unsigned faults = 0;
    if (mgr && mgr != g_her.mgr) return Forget("the AI manager changed");  // another world's manager: not ours to touch
    if (!mgr) {
        // no manager now (review S4): step 1 on the stimulus only while it still reads as ours; no list walk, no lock
        if (!OursLive(g_her.stim)) return Forget("no AI manager and no longer ours");
        faults = Step1(g_her.stim, g_her.agent, g_her.res, 0);
    } else {
        PoolLock lk(mgr);
        if (!lk.held) {
            StimDead("the pool lock");
            return Forget("the pool lock faulted");
        }
        if (!InLiveList(mgr, g_her.stim) || !OursLive(g_her.stim)) return Forget("no longer ours at the release");
        faults = Step1(g_her.stim, g_her.agent, g_her.res, mgr);
    }
    g_her.pending = true;
    g_her.pending_tick = g_tick;
    Log("[probe] L1 release #%u (%s): +0xA4 0, +0x88 no actor, +0x84 1, agent cleared%s%s; removed on a later tick", g_releases, why,
        mgr ? "" : " (no AI manager: the copies untouched)", faults ? " FAULT" : "");
}
// Step 2 (two ticks later): removed only while it is still in the same manager's live list parked as ours (review S3/C5);
// else forgotten. When the remove does nothing it is parked again (+0x84 1; review S7).
void Step2(bool now) noexcept {
    if (!g_her.pending || (!now && g_tick < g_her.pending_tick + 2)) return;
    if (g_stim_dead) return Forget("an earlier create or remove faulted");
    const std::uintptr_t mgr = mem::Q(A(kStimMgr));
    if (!mgr) {
        if (!g_her.wait_logged) Log("[probe] L1 release: no AI manager; the remove waits");
        g_her.wait_logged = true;
        return;
    }
    if (mgr != g_her.mgr) return Forget("the AI manager changed");
    const std::uintptr_t s = g_her.stim;
    PoolLock lk(mgr);
    if (!lk.held) {
        StimDead("the pool lock");
        return Forget("the pool lock faulted");
    }
    if (!InLiveList(mgr, s) || !OursParked(s)) return Forget("not in the live list as ours any more");
    const float life = F32(g_her.res + kResLife);
    const std::uint8_t zero = 0, one = 1;
    int r = SehWrite(s + kSOff, &zero, 1);
    if (r > 0) r = SehStimRemove(mgr, s);
    if (r < 0) StimDead("the remove");
    const bool still = r >= 0 && InLiveList(mgr, s);
    if (still) SehWrite(s + kSOff, &one, 1);
    Log("[probe] L1 hers removed (%.1f s; resource lifetime %.2f%s)%s", (g_now - g_her.t0) / 1000.0, static_cast<double>(life),
        still ? "; still in the live list: the remove did nothing, parked again (+0x84 1)" : "", r < 0 ? " FAULT" : "");
    g_her = Her{};
}
// a direct remove while her body is alive (op 1 0, op 9); still listed afterwards: released instead
void RemoveNow(const char* why) noexcept {
    if (!g_her.stim) return;
    if (g_her.pending) return Step2(true);
    const std::uintptr_t mgr = mem::Q(A(kStimMgr));
    if (!mgr || g_stim_dead) return Release(why);
    if (mgr != g_her.mgr) return Forget("the AI manager changed");
    {
        PoolLock lk(mgr);
        if (!lk.held) {
            StimDead("the pool lock");
            return Forget("the pool lock faulted");
        }
        if (!InLiveList(mgr, g_her.stim) || !OursLive(g_her.stim)) return Forget("no longer ours at the remove");
        Unpublish();
        const int r = SehStimRemove(mgr, g_her.stim);
        if (r < 0) StimDead("the remove");
        if (r >= 0 && !InLiveList(mgr, g_her.stim)) {
            Log("[probe] L1 hers removed (%s; %.1f s)", why, (g_now - g_her.t0) / 1000.0);
            g_her = Her{};
            return;
        }
        Log("[probe] L1 remove (%s): %s; released instead", why, r < 0 ? "FAULT" : "still in the live list");
    }
    Publish();  // Release unpublishes and parks it
    Release(why);
}

// ---- the actors (host: the registry; guest: the mirrors)
std::vector<std::pair<std::uint32_t, std::uintptr_t>> g_actors;
double g_actors_at = -1e18;
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
// the MULE `id` here: its actor and, when realized, its entity (the guest: her frozen mirror)
bool MuleOf(std::uint32_t id, std::uintptr_t* actor, std::uintptr_t* entity, std::uint32_t* state) noexcept {
    *actor = 0, *entity = 0, *state = 0;
    if (g_role == 2) {
        if (!esync::MirrorOf(id, actor, entity)) return false;
        *state = *actor ? U32(*actor + kActState) : 0;
        return *actor != 0;
    }
    *actor = ActorOf(id);
    if (!*actor) return false;
    *state = U32(*actor + kActState);
    const std::uintptr_t ref = mem::Q(*actor + kActEntRef);
    *entity = *state == 2 && ref ? ref - 0x20 : 0;
    return true;
}
// her and Sam as this game sees them: the host's partner body is her; the guest's own Sam is her, its partner body Sam
std::uintptr_t HerEntity() noexcept { return g_role == 2 ? g_local : g_body; }
std::uintptr_t SamEntity() noexcept { return g_role == 2 ? g_body : g_local; }

// ---- a MULE's knowledge (DSAINoticeAndKnowledge)
bool KnowOf(std::uintptr_t p, std::uintptr_t agent) noexcept {
    if (!p || mem::Q(p) != A(kKnowVt)) return false;
    const std::uintptr_t x = mem::Q(p + kKX);
    return x && mem::Q(x + kXAgent) == agent;
}
std::uintptr_t Individual(std::uintptr_t e) noexcept {
    const std::uintptr_t c = CompOf(e, "AIIndividualComponent");
    return c ? mem::Q(c + 0x50) : 0;
}
// K: the pointer at individual + 0x3AD8, else the object held there; else (review C12d) the path docs/enemy-sync.md names,
// entity + 0x228 -> + 0x720 (a pointer there, else held there); checked by its vtable and agent. how: 1 pointer, 2 inline, 3 the
// entity path's pointer, 4 held on the entity path
std::uintptr_t KnowledgeOf(std::uintptr_t e, std::uintptr_t ind, int* how) noexcept {
    *how = 0;
    const std::uintptr_t agent = e ? mem::Q(e + kEAgent) : 0;
    if (!agent) return 0;
    if (ind) {
        const std::uintptr_t p = mem::Q(ind + kIndK);
        if (KnowOf(p, agent)) return *how = 1, p;
        if (KnowOf(ind + kIndK, agent)) return *how = 2, ind + kIndK;
    }
    const std::uintptr_t q = mem::Q(e + 0x228);
    if (!q) return 0;
    const std::uintptr_t k = mem::Q(q + 0x720);
    if (KnowOf(k, agent)) return *how = 3, k;
    if (KnowOf(q + 0x720, agent)) return *how = 4, q + 0x720;
    return 0;
}
struct KRead {
    int pv_n = 0, lk_n = 0;
    bool pv_her = false, pv_sam = false;
    int lk_who = R::kNone;
    double pv_pos[3] = {}, pv_dist = -1;
    int pv_who = R::kNone;
    std::uint32_t pv_actor = 0;
    float pv_age = 0;
    // run 2: the youngest PV entry at her (s; -1 none), the youngest of any (1e300 none), the nearest entry to either player (m)
    double pv_her_age = -1, pv_min_age = 1e300, pv_min_dist = 1e300;
};
void ReadKnowledge(std::uintptr_t k, bool her_ok, const double (&her)[3], bool sam_ok, const double (&sam)[3], KRead& o) noexcept {
    std::int32_t nb = 0;
    const std::uintptr_t arr = k ? mem::Q(k + kKBuckets) : 0;
    if (!arr || !mem::Rd(k + kKBucketN, nb) || nb <= 0 || nb > 256) return;
    for (std::int32_t b = 0; b < nb; ++b) {
        std::int32_t c = 0;
        const std::uintptr_t ents = mem::Q(arr + 16ull * static_cast<unsigned>(b) + 8);
        if (!mem::Rd(arr + 16ull * static_cast<unsigned>(b), c) || c <= 0 || c > 256 || !ents) continue;
        for (std::int32_t i = 0; i < c; ++i) {
            std::uint8_t raw[kKeSize];
            if (!mem::Read(ents + kKeSize * static_cast<unsigned>(i), raw, kKeSize)) continue;
            std::int32_t type = 0;
            double pos[3] = {};
            std::memcpy(&type, raw + kKeType, 4);
            std::memcpy(pos, raw + kKePos, 24);
            if (type == kTypePV) {
                ++o.pv_n;
                const int w = R::Nearest(pos, her_ok, her, sam_ok, sam);
                if (w == R::kHer) o.pv_her = true;
                if (w == R::kSam) o.pv_sam = true;
                float age_f = 0;
                std::memcpy(&age_f, raw + kKeAge, 4);
                const double age = std::isfinite(age_f) ? static_cast<double>(age_f) : 1e300;
                if (age < o.pv_min_age) o.pv_min_age = age;
                if (w == R::kHer && (o.pv_her_age < 0 || age < o.pv_her_age)) o.pv_her_age = age;
                const double dh = her_ok ? R::SegDist(pos, her) : 1e300, ds = sam_ok ? R::SegDist(pos, sam) : 1e300;
                const double dmin = dh < ds ? dh : ds;
                if (dmin < o.pv_min_dist) o.pv_min_dist = dmin;
                if (o.pv_n == 1 || (w == R::kHer && o.pv_who != R::kHer)) {
                    std::memcpy(o.pv_pos, pos, sizeof(pos));
                    o.pv_who = w;
                    o.pv_dist = w == R::kHer ? R::SegDist(pos, her) : w == R::kSam ? R::SegDist(pos, sam) : -1;
                    std::memcpy(&o.pv_actor, raw + kKeActor, 4);
                    std::memcpy(&o.pv_age, raw + kKeAge, 4);
                }
            } else if (type == kTypeLK) {
                ++o.lk_n;
                const int w = R::Nearest(pos, her_ok, her, sam_ok, sam, 3.0);
                if (w != R::kNone && o.lk_who == R::kNone) o.lk_who = w;
            }
        }
    }
}

// ---- the watch (4 Hz) and the arms
struct Watch {
    bool on = false, arm = false;
    std::uint32_t id = 0;
    char label[8] = {};
    double t0 = 0, until = 0, next = 0, next_counts = 0;
    std::uintptr_t entity = 0, ind = 0, k = 0, cp = 0;
    std::uint32_t cp_id = 0;
    R::Arm a;
    int phase = 0;
    unsigned rounds = 0;
    std::uint32_t goal_id = 0, goal_mode = 0;
    std::uintptr_t goal = 0;
    bool daemon_seen = false, daemon_player_sam = false;
    unsigned melee_n = 0, melee_seen = 0, melee_her = 0, melee_sam = 0;
    Counts c0;
    bool stim = false;
    bool ext = false;  // a target arm (enemy_tgt 4): her stimulus is the partner stimulus (enemy_target.cpp), not the probe's
    unsigned releases0 = 0;
} g_w;
struct Done {
    bool have = false;
    R::Arm a;
    unsigned melee_n = 0, melee_her = 0;
};
Done g_arm_b, g_arm_c, g_arm_d, g_arm_g;
bool g_counts_armed = false;
std::uint32_t g_calm_id = 0;
char g_calm_text[16] = "-";

struct Calm {
    bool on = false;
    std::uint32_t id = 0;
    double t0 = 0, until = 0, next = 0, next_apply = 0;
    int applies = 0, max_applies = 3;  // run 2: re-applied every 500 ms while anything is alerted, about 20 per 12 s (R::CalmMaxApplies)
    unsigned n = 0;
    R::CalmHold hold;
    int cp_forces = 0;                 // (run 3) the CalmApply calls that forced the camp's CP (it read 3..5)
} g_calm;
unsigned g_calm_seq = 0;

struct Crouch {
    bool have = false;
    float a4 = 0, a0 = 0, b0 = 0;
    std::uint32_t f2b0 = 0;
} g_stand;

const char* Who(int w) noexcept { return R::WhoText(w); }

void LogCounts(const char* when) noexcept {
    const Counts c = ReadCounts();
    char ch[96], cs[96], ah[96], as[96];
    const Counts z{};
    BinText(ch, sizeof(ch), c.cls[1], z.cls[1]);
    BinText(cs, sizeof(cs), c.cls[2], z.cls[2]);
    BinText(ah, sizeof(ah), c.app[1], z.app[1]);
    BinText(as, sizeof(as), c.app[2], z.app[2]);
    Log("[probe] L1 counts (%s): classifier her %s, Sam %s; append her %s, Sam %s; watched K processed %u, type-1 %u, record[0] her %u Sam %u "
        "other %u; faults %u",
        when, ch, cs, ah, as, c.psr, c.t1, c.rec0[1], c.rec0[2], c.rec0[0], c.fault);
}

void StartWatch(std::uint32_t id, double secs, const char* label, bool arm) noexcept {
    g_w = Watch{};
    g_w.on = true, g_w.arm = arm, g_w.id = id;
    std::snprintf(g_w.label, sizeof(g_w.label), "%s", label);
    g_w.t0 = g_now, g_w.until = g_now + secs * 1000.0, g_w.next = g_now;
    g_w.next_counts = g_now + 5000;
    g_w.c0 = ReadCounts();
    g_w.stim = g_her.stim != 0 && !g_her.pending;
    g_w.releases0 = g_releases;
    // review C8: the re-calm before this arm timed out (the MULE may still know her)
    g_w.a.calm_timeout = arm && g_calm_id == id && std::strcmp(g_calm_text, "timeout") == 0;
    esync::CampOf co;
    if (esync::CampOfId(id, &co)) g_w.cp = co.cp, g_w.cp_id = co.cp_id;
    g_watch_k.store(0, std::memory_order_relaxed);
}

void SampleWatch() noexcept {
    R::Sample s;
    s.t = g_now - g_w.t0;
    s.phase = g_w.phase;
    s.stim = g_w.ext ? etarget::StimLive() : (g_her.stim && !g_her.pending);  // review C9: her stimulus all through the arm (a release during it: invalid)
    std::uintptr_t actor = 0, entity = 0;
    std::uint32_t state = 0;
    const bool have = MuleOf(g_w.id, &actor, &entity, &state);
    WorldTransform mx{};
    s.realized = have && entity && ReadXf(entity, mx);
    const std::uintptr_t he = HerEntity(), se = SamEntity();
    s.her_ok = Pos(he, s.her);
    s.sam_ok = Pos(se, s.sam);
    if (!s.realized) {
        R::Add(g_w.a, s);
        if (g_w.a.samples % 8 == 1)
            Log("[probe] L1 watch id=%05X t=%.2f: not realized (%s, state %u)", g_w.id, s.t / 1000.0, have ? "actor here" : "no actor", state);
        return;
    }
    s.mule[0] = mx.pos.x, s.mule[1] = mx.pos.y, s.mule[2] = mx.pos.z;
    s.yaw = YawOf(mx);
    if (entity != g_w.entity) {
        g_w.entity = entity;
        g_w.ind = Individual(entity);
        int how = 0;
        g_w.k = KnowledgeOf(entity, g_w.ind, &how);
        g_watch_k.store(g_w.k, std::memory_order_relaxed);
        Log("[probe] L1 watch id=%05X: entity %p individual %p knowledge %p (%s)", g_w.id, reinterpret_cast<void*>(entity), reinterpret_cast<void*>(g_w.ind),
            reinterpret_cast<void*>(g_w.k),
            how == 1   ? "the pointer at +0x3AD8"
            : how == 2 ? "held at +0x3AD8"
            : how == 3 ? "the pointer at entity +0x228 -> +0x720"
            : how == 4 ? "held at entity +0x228 -> +0x720"
                       : "not found");
    }
    KRead kr;
    ReadKnowledge(g_w.k, s.her_ok, s.her, s.sam_ok, s.sam, kr);
    s.pv_her = kr.pv_her, s.pv_sam = kr.pv_sam, s.lk = kr.lk_n > 0, s.lk_who = kr.lk_who;
    // run 2: the notice is a fresh PV at her; a PV at her at the start counts as contamination only when older than her stimulus
    s.pv_her_age = kr.pv_her_age < 0 ? 1e300 : kr.pv_her_age;
    s.pv_age_min = kr.pv_n > 0 && kr.pv_min_age < 1e299 ? kr.pv_min_age : -1;
    {
        const double ms = g_w.ext ? etarget::StimAgeMs() : (g_her.stim && !g_her.pending ? g_now - g_her.t0 : -1.0);
        s.stim_age = ms >= 0 ? ms / 1000.0 : -1.0;
    }
    // the goals (run 2: the shoot goal's +0x68 / +0x6C and its +0x48 place, fault-free reads)
    g_w.goal = 0;
    std::uint32_t g68 = 0, g6c = 0;
    int place_who = -1;  // -1 no place read
    double place_d = -1;
    if (g_w.ind && (g_groups & kGGoal)) {
        const std::uintptr_t shoot = SehGoal(g_w.ind, A(kShootRtti));
        g_w.goal = shoot;
        if (shoot) {
            g_w.goal_id = U32(shoot + 0x28), g_w.goal_mode = U32(shoot + 0x40);
            g68 = U32(shoot + 0x68), g6c = U32(shoot + 0x6C);
            double pl[3] = {};
            if (mem::Read(shoot + 0x48, pl, sizeof(pl)) && Finite3(pl)) {
                place_who = R::Nearest(pl, s.her_ok, s.her, s.sam_ok, s.sam);
                place_d = place_who == R::kHer ? R::SegDist(pl, s.her) : place_who == R::kSam ? R::SegDist(pl, s.sam) : -1.0;
            }
        }
        const std::uintptr_t aim = SehGoal(g_w.ind, A(kAimRtti));
        if (aim && mem::Read(aim + 0x28, s.aim, sizeof(s.aim)) && Finite3(s.aim)) s.aim_ok = true;
    }
    // run 2: the body shoot job's last aim point (enemy_target's bit 32 record; the host's registered MULEs only)
    etarget::ShootStat sh;
    const bool sh_ok = g_role != 2 && etarget::ShootStats(g_w.id, &sh);
    // the PlayerInfo daemon
    double centre[3] = {};
    bool daemon = false;
    if (g_w.ind && (g_groups & kGDaemon)) {
        const std::uintptr_t planner = mem::Q(g_w.ind + kIndPlanner);  // the individual holds a POINTER to the planner (round 27 M3 research)
        std::int32_t n = 0;
        const std::uintptr_t arr = mem::Q(planner + 0x20);
        // +0x18 as the count is not verified: when it does not read as 1..64, the first 64 slots are scanned (fault-free reads)
        if (!planner || !mem::Rd(planner + 0x18, n) || n <= 0 || n > 64) n = 64;
        if (arr)
            for (std::int32_t i = 0; i < n; ++i) {
                const std::uintptr_t d = mem::Q(arr + 8ull * static_cast<unsigned>(i));
                if (!d || mem::Q(d) != A(kDaemonVt)) continue;
                daemon = true;
                g_w.daemon_seen = true;
                g_w.daemon_player_sam = se && mem::Q(d + 0x18) == se;
                if (mem::Read(d + 0x40, centre, sizeof(centre)) && Finite3(centre)) s.daemon = R::Nearest(centre, s.her_ok, s.her, s.sam_ok, s.sam, 3.0);
                break;
            }
    }
    // the CP's alert place
    double alert[3] = {};
    if (g_w.cp && mem::Read(g_w.cp + kCpAlert, alert, sizeof(alert)) && Finite3(alert)) s.cp_who = R::Nearest(alert, s.her_ok, s.her, s.sam_ok, s.sam, 3.0);
    s.shots = g_role == 2 ? 0u : shots::ShotsBy(entity);
    // the new rounds' own lines (review C4: the shot record's origin and velocity), oldest first; the rest on the sample's line
    if (g_w.a.have_shots0 && s.shots > g_w.a.shots0 + g_w.a.fired) {
        const unsigned seen = g_w.a.shots0 + g_w.a.fired;
        for (unsigned i = 0; i < s.shots - seen && i < R::Sample::kRays; ++i) {
            if (!shots::RoundOf(entity, seen + 1 + i, s.ray_o[i], s.ray_d[i])) break;
            s.ray_n = i + 1;
        }
    }
    R::RoundAt round;
    const unsigned fresh = R::Add(g_w.a, s, &round);
    if (fresh) {
        g_w.rounds += fresh;
        // run 2: each new round's angle (deg) off the line to her chest / to Sam's chest and off the MULE's facing (its own line when
        // the shot record had one, else the sample's line), "her/Sam/facing" per round, oldest first (-1 unknown)
        char ang[160] = "";
        std::size_t at = 0;
        double lo[3], ld[3];
        R::RoundLineDir(s.mule, s.yaw, s.aim_ok, s.aim, lo, ld);
        for (unsigned i = 0; i < fresh && i < R::Sample::kRays && at < sizeof(ang); ++i) {
            const bool ray = i < s.ray_n;
            const double(&o)[3] = ray ? s.ray_o[i] : lo;
            const double(&d)[3] = ray ? s.ray_d[i] : ld;
            const double ah = s.her_ok ? R::AngleToChest(o, d, s.her) : -1.0, as = s.sam_ok ? R::AngleToChest(o, d, s.sam) : -1.0;
            const int w = std::snprintf(ang + at, sizeof(ang) - at, "%s%.0f/%.0f/%.0f", at ? " " : "", ah, as, R::AngleToYaw(d, s.yaw));
            if (w > 0) at += static_cast<std::size_t>(w);
        }
        Log("[probe] L1 round id=%05X #%u: closest her %.2f m, Sam %.2f m (%u new; %s); angles her/Sam/facing %s deg; yaw %.0f", g_w.id, g_w.rounds,
            round.her < 1e299 ? round.her : -1.0, round.sam < 1e299 ? round.sam : -1.0, fresh,
            s.ray_n ? "on its own line" : s.aim_ok ? "on the aim line" : "along the facing", at ? ang : "-", s.yaw * 180.0 / R::kPi);
    }
    const double fh = s.her_ok ? R::FacingOff(s.yaw, s.mule, s.her) : -1, fs = s.sam_ok ? R::FacingOff(s.yaw, s.mule, s.sam) : -1;
    char shoot_pt[96] = "-";
    if (sh_ok && sh.have)
        std::snprintf(shoot_pt, sizeof(shoot_pt), "%s mode %d %.1f s (calls %u redir %u)", Who(sh.nearest), sh.mode, sh.age_ms / 1000.0, sh.calls, sh.redirected);
    else if (sh_ok)
        std::snprintf(shoot_pt, sizeof(shoot_pt), "none (calls %u redir %u)", sh.calls, sh.redirected);
    Log("[probe] L1 watch id=%05X t=%.2f: phase %d ea4 0x%X; faces her %.0f deg / Sam %.0f deg; her %.1f m Sam %.1f m; PV %d at %s %.1f m id %05X age "
        "%.1f; LK %d at %s; goal %p id 0x%X mode %u +68 0x%X +6C 0x%X place -> %s %.1f m; shoot pt -> %s; aim -> %s; daemon %s centre -> %s; cp alert -> %s; "
        "rounds %u",
        g_w.id, s.t / 1000.0, static_cast<int>(U32(actor + kActPhase)), U32(actor + kActDown), fh, fs, s.her_ok ? R::Dist2d(s.mule, s.her) : -1.0,
        s.sam_ok ? R::Dist2d(s.mule, s.sam) : -1.0, kr.pv_n, Who(kr.pv_who), kr.pv_dist, kr.pv_actor, static_cast<double>(kr.pv_age), kr.lk_n, Who(kr.lk_who),
        reinterpret_cast<void*>(g_w.goal), g_w.goal_id, g_w.goal_mode, g68, g6c, place_who < 0 ? "-" : Who(place_who), place_d, shoot_pt,
        s.aim_ok ? Who(R::Nearest(s.aim, s.her_ok, s.her, s.sam_ok, s.sam)) : "-", daemon ? (g_w.daemon_player_sam ? "Sam" : "other") : "none",
        daemon ? Who(s.daemon) : "-", Who(s.cp_who), g_w.a.fired);
}

double Ms(double v) noexcept { return v < 0 ? -1.0 : v; }
double Sw(double v) noexcept { return v == R::kNa ? R::kNa : Ms(v); }  // the switch times: -2 n/a stays
void ArmLine(const char* label) noexcept {
    const R::Arm& a = g_w.a;
    const Counts c = ReadCounts();
    const char* cp_at = a.cp_her > a.cp_sam ? "her" : a.cp_sam > a.cp_her ? "Sam" : (a.cp_her ? "both" : "none");
    Log("[probe] L1 result arm %s id=%05X: stim_start=%d stim_n=%d stim_lost=%d releases=%u pv_start=%d calm_timeout=%d goals=%d valid=%d rays=%u "
        "tie=%s calm=%s samples=%d realized=%d noticed=%.0f turned_her=%.0f turned_sam=%.0f turned=%s approached_her=%d "
        "approached_sam=%d start_her=%.1f min_her=%.1f start_sam=%.1f min_sam=%.1f aim_n=%d aim_her=%d aim_sam=%d fired=%u at_her=%u at_sam=%u pv_her=%d "
        "pv_sam=%d lk=%d lk_her=%d lk_sam=%d goal=0x%X mode=%u daemon=%d daemon_her=%d daemon_sam=%d cp_alert=%s switch_sam=%.0f switch_her=%.0f "
        "melee_n=%u melee_seen=%u melee_her=%u melee_sam=%u cls_her=%u cls_sam=%u app_her=%u app_sam=%u psr=%u psr_t1=%u rec0_her=%u rec0_sam=%u k=%d "
        "fired_p=%u/%u/%u at_her_p=%u/%u/%u at_sam_p=%u/%u/%u pv_age0=%.1f stim_age0=%.1f",
        label, g_w.id, g_w.stim ? 1 : 0, a.stim_n, a.stim_lost ? 1 : 0, g_releases - g_w.releases0, a.pv_at_start ? 1 : 0, a.calm_timeout ? 1 : 0,
        a.no_goal_reads ? 0 : 1, R::Invalid(a) ? 0 : 1, a.rays, Who(a.last_target[0]), g_calm_id == g_w.id ? g_calm_text : "-", a.samples, a.realized, Ms(a.noticed), Ms(a.turned_her), Ms(a.turned_sam),
        Who(R::TurnedDuring(a)), a.approached_her ? 1 : 0, a.approached_sam ? 1 : 0, a.start_her, a.min_her < 1e299 ? a.min_her : -1.0, a.start_sam,
        a.min_sam < 1e299 ? a.min_sam : -1.0, a.aim_n, a.aim_her, a.aim_sam, a.fired, a.at_her, a.at_sam, a.pv_her, a.pv_sam, a.lk, a.lk_her, a.lk_sam,
        g_w.goal_id, g_w.goal_mode, g_w.daemon_seen ? 1 : 0, a.daemon_her, a.daemon_sam, cp_at, Sw(a.switch_sam), Sw(a.switch_her), g_w.melee_n,
        g_w.melee_seen, g_w.melee_her, g_w.melee_sam, Sum(c.cls[1], g_w.c0.cls[1]), Sum(c.cls[2], g_w.c0.cls[2]), Sum(c.app[1], g_w.c0.app[1]),
        Sum(c.app[2], g_w.c0.app[2]), c.psr - g_w.c0.psr, c.t1 - g_w.c0.t1, c.rec0[1] - g_w.c0.rec0[1], c.rec0[2] - g_w.c0.rec0[2], g_w.k ? 1 : 0, a.fired_ph[0], a.fired_ph[1],
        a.fired_ph[2], a.at_her_ph[0], a.at_her_ph[1], a.at_her_ph[2], a.at_sam_ph[0], a.at_sam_ph[1], a.at_sam_ph[2], a.pv_age0, a.stim_age0);
}
void EndWatch(const char* why) noexcept {
    if (!g_w.on) return;
    g_w.a.no_goal_reads = !(g_groups & kGGoal);  // review C8: without the goal reads B's aim is unknown, not "never aimed"
    if (g_w.arm || g_role == 2) {
        ArmLine(g_w.arm ? g_w.label : "U21");
        Done* d = nullptr;
        if (std::strcmp(g_w.label, "B") == 0) d = &g_arm_b;
        if (std::strcmp(g_w.label, "C") == 0) d = &g_arm_c;
        if (std::strcmp(g_w.label, "D") == 0) d = &g_arm_d;
        if (std::strcmp(g_w.label, "G") == 0) d = &g_arm_g;
        if (d && g_w.arm) d->have = true, d->a = g_w.a, d->melee_n = g_w.melee_n, d->melee_her = g_w.melee_her;
    } else {
        Log("[probe] L1 watch id=%05X ended (%s): %d samples, noticed %.0f ms, fired %u", g_w.id, why, g_w.a.samples, Ms(g_w.a.noticed), g_w.a.fired);
    }
    LogCounts(g_w.label[0] ? g_w.label : "watch");
    g_w.on = false;
    g_watch_k.store(0, std::memory_order_relaxed);
}

// ---- re-calm
bool CampHas(const esync::CampOf& co, std::uintptr_t actor) noexcept {
    for (unsigned i = 0; i < co.n; ++i)
        if (co.m[i].actor == actor) return true;
    return false;
}
// The calm: SetPhase(0) on the MULE and its camp's members, ForceSetCPPhase(0) on its CP. Run 2: with enemy_owner=0 nothing walks
// the camps, so an unknown camp is walked here first (esync::CampWalkNow; the CP reads are byte-checked at enemy init either way).
// 1 the CP forced; 2 the CP found at rest, not forced; 0 no CP; -1 a fault; -2 the phase sites failed their byte checks.
// Run 3: the CP is forced only when it reads alerted (3..5, l1rules::CalmCpForce); it rests at PreCaution (2). *cp_before its phase
// read first (-2 unread, -1 no CP), *cp_after read back after a force (else the same).
int CalmApply(std::uint32_t id, int* members, int* cp_before = nullptr, int* cp_after = nullptr) noexcept {
    *members = 0;
    if (cp_before) *cp_before = -1;
    if (cp_after) *cp_after = -1;
    if (!(g_groups & kGPhase)) return -2;
    esync::CampOf co;
    bool camp = esync::CampOfId(id, &co);
    if (!camp || (co.n == 0 && !co.cp))
        if (esync::CampWalkNow()) camp = esync::CampOfId(id, &co);
    int faults = 0;
    std::uintptr_t actor = 0, entity = 0;
    std::uint32_t state = 0;
    if (MuleOf(id, &actor, &entity, &state) && actor && (!camp || !CampHas(co, actor))) {
        if (SehSetPhase(actor, 0) < 0) ++faults;
        ++*members;
    }
    for (unsigned i = 0; camp && i < co.n; ++i) {
        if (!co.m[i].actor || !SameMule(co.m[i].actor, co.m[i].id)) continue;
        if (SehSetPhase(co.m[i].actor, 0) < 0) ++faults;
        ++*members;
    }
    bool forced = false, rest = false;
    if (camp && co.cp) {
        std::uint64_t guid[2] = {};
        const std::uintptr_t gref = mem::Q(co.cp + kCpGuidRef);
        if (gref && mem::Read(gref + 8, guid, sizeof(guid)) && (guid[0] || guid[1])) {
            std::int32_t cph = -1;
            const int before = mem::Rd(co.cp + kCpPh, cph) && cph >= 0 && cph <= 15 ? cph : -2;
            if (cp_before) *cp_before = before;
            if (cp_after) *cp_after = before;
            if (R::CalmCpForce(before)) {
                if (SehCpForce(guid, 0) < 0) ++faults;
                else forced = true;
                cph = -1;
                if (cp_after) *cp_after = mem::Rd(co.cp + kCpPh, cph) && cph >= 0 && cph <= 15 ? cph : -2;
            } else {
                rest = true;
            }
        }
    }
    return faults ? -1 : forced ? 1 : rest ? 2 : 0;
}
// One reading for the ready rule (run 2): the target's phase, its camp's members at 3..5, the CP's phase, the PV entries against
// the real her and Sam places, the LK count.
struct CalmRead {
    R::CalmIn in;
    bool k_read = false;  // the MULE's knowledge was found (else not quiet: its PV entries are unknown)
    int lk = -1;
    int pv_who = R::kNone;
    double pv_age = -1;  // the youngest PV entry's age (s; -1 none)
    unsigned members_mid = 0;      // (run 3) the other members at 1..2
    unsigned members_unread = 0;   // (run 3 review) the other members whose phase did not read (in members_up, not members_hot)
    std::uint32_t mid_first = 0;   // the first of those
};
CalmRead CalmReadNow(std::uint32_t id) noexcept {
    CalmRead r;
    std::uintptr_t actor = 0, entity = 0;
    std::uint32_t state = 0;
    const bool have = MuleOf(id, &actor, &entity, &state) && actor;
    std::int32_t ph = -1;
    if (have && mem::Rd(actor + kActPhase, ph) && ph >= 0 && ph <= 15) r.in.phase = ph;
    esync::CampOf co;
    if (esync::CampOfId(id, &co)) {
        for (unsigned i = 0; i < co.n; ++i) {
            if (!co.m[i].actor || co.m[i].actor == actor || !SameMule(co.m[i].actor, co.m[i].id)) continue;
            std::int32_t mp = -1;
            ++r.in.members;
            if (!mem::Rd(co.m[i].actor + kActPhase, mp)) {
                ++r.in.members_up;  // unread: not known calm
                ++r.members_unread;
                continue;
            }
            if (mp >= 3 && mp <= 5) ++r.in.members_hot;
            if (mp > 2) ++r.in.members_up;
            if (mp >= 1 && mp <= 2 && !r.members_mid++) r.mid_first = co.m[i].id;
        }
        if (co.cp) {
            std::int32_t cph = -1;
            r.in.cp_phase = mem::Rd(co.cp + kCpPh, cph) && cph >= 0 && cph <= 15 ? cph : -2;
        }
    }
    if (have && entity) {
        int how = 0;
        const std::uintptr_t k = KnowledgeOf(entity, Individual(entity), &how);
        if (k) {
            double her[3] = {}, sam[3] = {};
            const bool her_ok = Pos(HerEntity(), her), sam_ok = Pos(SamEntity(), sam);
            KRead kr;
            ReadKnowledge(k, her_ok, her, sam_ok, sam, kr);
            r.k_read = true;
            r.in.pv_n = kr.pv_n, r.in.pv_min_age = kr.pv_min_age, r.in.pv_min_dist = kr.pv_min_dist;
            r.lk = kr.lk_n, r.pv_who = kr.pv_who, r.pv_age = kr.pv_n > 0 && kr.pv_min_age < 1e299 ? kr.pv_min_age : -1;
        }
    }
    return r;
}
void CalmTick() noexcept {
    if (!g_calm.on || g_now < g_calm.next) return;
    g_calm.next = g_now + 500;
    const CalmRead r = CalmReadNow(g_calm.id);
    // run 2: re-applied every 500 ms while the target or a member reads phase >= 3 (or the CP is not at 0), up to the cap
    bool applied = false;
    if (R::CalmHot(r.in) && g_now >= g_calm.next_apply && g_calm.applies < g_calm.max_applies) {
        int members = 0;
        if (CalmApply(g_calm.id, &members) == 1) ++g_calm.cp_forces;
        ++g_calm.applies, g_calm.next_apply = g_now + R::kCalmApplyMs, applied = true;
    }
    // ready (run 2): the target and every member at phase <= 2, the CP at 0, every PV entry >= 3 s old and > 3 m from both
    // players, held 1.5 s (a stale PV entry no longer blocks it: it never expires)
    const bool quiet = r.k_read && R::CalmQuiet(r.in);
    const bool ready = R::CalmStep(g_calm.hold, quiet, g_now);
    Log("[probe] L1 calm #%u id=%05X t=%.1f: phase %d, members at 3-5 %u/%u, members at 1-2 %u (first %05X), cp %d, PV %d age %.1f at %s %.1f m, LK %d; "
        "quiet %.1f s%s",
        g_calm.n, g_calm.id, (g_now - g_calm.t0) / 1000.0, r.in.phase, r.in.members_hot, r.in.members, r.members_mid, r.mid_first, r.in.cp_phase,
        r.k_read ? r.in.pv_n : -1, r.pv_age,
        Who(r.pv_who), r.in.pv_min_dist < 1e299 ? r.in.pv_min_dist : -1.0, r.lk, g_calm.hold.since >= 0 ? (g_now - g_calm.hold.since) / 1000.0 : 0.0,
        applied ? "; re-applied" : "");
    if (ready || g_now >= g_calm.until) {
        g_calm_id = g_calm.id;
        std::snprintf(g_calm_text, sizeof(g_calm_text), "%s", ready ? "ready" : "timeout");
        // (run 3) a timeout names the first thing that kept it from quiet (CalmQuiet's order)
        char blocked[96] = "";
        if (!ready) {
            char b[80];
            if (!(r.in.phase >= 0 && r.in.phase <= 2)) std::snprintf(b, sizeof(b), "phase %d", r.in.phase);
            else if (r.in.members_up)
                std::snprintf(b, sizeof(b), "members above 2 or unread %u (at 3-5 %u, unread %u)", r.in.members_up, r.in.members_hot,
                              r.members_unread);
            else if (!(r.in.cp_phase == -1 || (r.in.cp_phase >= 0 && r.in.cp_phase <= R::kCpBaseMax))) std::snprintf(b, sizeof(b), "cp %d", r.in.cp_phase);
            else if (!r.k_read) std::snprintf(b, sizeof(b), "knowledge unread");
            else if (!R::CalmPvStale(r.in))
                std::snprintf(b, sizeof(b), "PV fresh (age %.1f, %.1f m)", r.pv_age, r.in.pv_min_dist < 1e299 ? r.in.pv_min_dist : -1.0);
            else std::snprintf(b, sizeof(b), "quiet only %.1f s", g_calm.hold.since >= 0 ? (g_now - g_calm.hold.since) / 1000.0 : 0.0);
            std::snprintf(blocked, sizeof(blocked), "; blocked by %s", b);
        }
        Log("[probe] L1 calm #%u id=%05X: %s (%.1f s; phase %d; PV %d LK %d; %d applies); members at 3-5 %u/%u, cp %d; CP forces %d, PV age %.1f%s", g_calm.n,
            g_calm.id, g_calm_text, (g_now - g_calm.t0) / 1000.0, r.in.phase, r.k_read ? r.in.pv_n : -1, r.lk, g_calm.applies, r.in.members_hot,
            r.in.members, r.in.cp_phase, g_calm.cp_forces, r.pv_age, blocked);
        g_calm.on = false;
    }
}

// ---- the melee query (op 7): the attack sphere query (partner_tackle's QueryHit) at her chest, made from the MULE's entity with
// an owner-less context of attack 0x157 (VehicleTackle) by default. It is NOT the MULE's own melee: it says only whether the
// game's hit query returns her body to a MULE-made query (PHIT path A) or not (path B; review C7).
struct MeleeUser {
    std::uintptr_t her = 0, sam = 0;
    unsigned seen = 0, hers = 0, sams = 0;
};
// a receiver's entity (enemy_combat's ReceiverEntity: +8 set -> receiver - 0x38), else the receiver itself
std::uintptr_t ReceiverEntity(std::uintptr_t receiver) noexcept {
    std::uint8_t ent = 0;
    return receiver && mem::Rd(receiver + 8, ent) && ent ? receiver - 0x38 : 0;
}
// the receiver is `who`'s: the entity itself, its receiver, or a part of it (the entity's parent chain, +0x70, 5 levels; review C7)
bool ReceiverOf(std::uintptr_t receiver, std::uintptr_t who) noexcept {
    if (!who || !receiver) return false;
    if (receiver == who) return true;
    std::uintptr_t e = ReceiverEntity(receiver);
    for (int d = 0; e && d <= 5; ++d, e = mem::Q(e + 0x70))
        if (e == who) return true;
    return false;
}
char MeleeCb(std::uintptr_t /*ctx*/, std::uintptr_t receiver, std::uintptr_t /*di*/, std::uintptr_t user) {
    auto* u = reinterpret_cast<MeleeUser*>(user);
    ++u->seen;
    if (ReceiverOf(receiver, u->her)) ++u->hers;
    if (ReceiverOf(receiver, u->sam)) ++u->sams;
    return 0;  // nothing delivered
}

// ---- the stimulus (L1)
// the partner body is the live real body the partner owns, not the hologram (review S5/C10: only that body's teardown paths
// call ReleaseBeforeFree first)
const char* BodyRefusal() noexcept {
    if (reinterpret_cast<std::uintptr_t>(realbody::LiveEntity()) != g_body) return "the partner body is not the live real body";
    if (realbody::GetOwner() != realbody::Owner::partner) return "the real body is not the partner's";
    if (reinterpret_cast<std::uintptr_t>(holo::LiveEntity()) == g_body) return "the partner body is the hologram";
    return nullptr;
}
void StimTick() noexcept {
    if (!g_her.stim || g_her.pending) return;
    // early release: the body changed or went, Sam asleep, a load or setup, the manager changed
    std::uint64_t st = 0;
    std::uint32_t mask = 0;
    const char* why = nullptr;
    if (!g_body || g_body != g_her.body) why = "her body changed or went";
    else if (BodyRefusal()) why = "her body is no longer the partner's live real body";
    else if (!g_local_ok) why = "Sam asleep";
    else if (game::LoadOrSetup(&st, &mask)) why = "a load or setup";
    else if (mem::Q(A(kStimMgr)) != g_her.mgr) why = "the AI manager changed";
    else if (mem::Q(g_body + kEAgent) != g_her.agent) why = "her agent changed";
    if (why) return Release(why);
    double p[3] = {};
    if (!Pos(g_body, p)) return;
    double off[3] = {0, 0, 1};
    float a0 = 0, b0 = 0;
    const std::uintptr_t ss = SamStimNow();
    double sp[3] = {}, sam[3] = {};
    if (ss && mem::Read(ss + kSPos, sp, sizeof(sp)) && Pos(g_local, sam) && R::Dist(sp, sam) < 3.0)
        off[0] = sp[0] - sam[0], off[1] = sp[1] - sam[1], off[2] = sp[2] - sam[2];
    if (ss) a0 = F32(ss + kSA0), b0 = F32(ss + kSB0);
    const double at[3] = {p[0] + off[0], p[1] + off[1], p[2] + off[2]};
    // her velocity (review C12e): zero after a jump (> 2 m between ticks: a teleport or a re-place), clamped to 12 m/s
    float vel[4] = {};
    const double dt = (g_now - g_her.last_at) / 1000.0;
    if (g_her.have_last && dt > 0.005 && dt < 1.0 && R::Dist(p, g_her.last_pos) <= 2.0) {
        double v[3], sp2 = 0;
        for (int i = 0; i < 3; ++i) v[i] = (p[i] - g_her.last_pos[i]) / dt, sp2 += v[i] * v[i];
        const double speed = std::sqrt(sp2), k = speed > 12.0 ? 12.0 / speed : 1.0;
        for (int i = 0; i < 3; ++i) vel[i] = static_cast<float>(v[i] * k);
    }
    std::memcpy(g_her.last_pos, p, sizeof(p));
    g_her.have_last = true, g_her.last_at = g_now;
    const float vis = g_her.vis_forced ? g_her.vis : 1.f;
    // the writes under the pool lock, after the stimulus is found in the live list as ours (review S3/S6)
    PoolLock lk(g_her.mgr);
    if (!lk.held) {
        StimDead("the pool lock");
        return Forget("the pool lock faulted");
    }
    if (!InLiveList(g_her.mgr, g_her.stim) || !OursLive(g_her.stim)) return Forget("no longer ours: not in the live list, or its agent or resource changed");
    int r = SehWrite(g_her.stim + kSPos, at, sizeof(at));
    if (r > 0) r = SehWrite(g_her.stim + kSVel, vel, sizeof(vel));
    if (r > 0) r = SehWrite(g_her.stim + kSMul, &vis, 4);
    if (r > 0 && ss) r = SehWrite(g_her.stim + kSA0, &a0, 4);
    if (r > 0 && ss) r = SehWrite(g_her.stim + kSB0, &b0, 4);
    if (r > 0) r = SehWrite(g_her.stim + kSId, &g_her.id88, 4);
    if (r < 0) {
        Log("[probe] L1 tick write FAULT: hers released");
        Release("a write fault");
    }
}

bool Make(char* msg, std::size_t cap) noexcept {
    if (g_her.stim) return std::snprintf(msg, cap, "L1: hers already made"), true;
    const char* why = !(g_groups & kGStim) ? "byte checks" : g_stim_dead ? "an earlier create or remove faulted" : !g_body ? "no partner body"
                      : !g_local || !g_local_ok                         ? "no Sam"
                                                                        : nullptr;
    if (!why) why = BodyRefusal();
    const std::uintptr_t mgr = why ? 0 : mem::Q(A(kStimMgr));
    const std::uintptr_t agent = g_body ? mem::Q(g_body + kEAgent) : 0;
    // review S5, corrected by runs round27-m1/m1b: the agent classes carry no MSVC class name and her agent's vtable is not
    // our Sam's. What the sensor needs (0x142E4F647..0x142E4F6EE) is that the agent's vt+0x10 returns a live entity: it
    // walks that object's +0x70 parent chain (0x142380C70) with no null check. So her agent is accepted when its vt+0x10
    // returns her own body, exactly as our Sam's returns him.
    const std::uintptr_t her_owner = agent ? SehAgentEntity(agent) : 0;
    const std::uintptr_t sam_agent = g_local ? mem::Q(g_local + kEAgent) : 0;
    const std::uintptr_t sam_owner = sam_agent ? SehAgentEntity(sam_agent) : 0;
    const char* an = her_owner && her_owner == g_body ? "(vt+0x10 is her body)" : nullptr;
    Log("[probe] L1 agents: hers %p vt rva %llX vt+0x10 -> %p (body %p); Sam's %p vt rva %llX vt+0x10 -> %p (Sam %p)", reinterpret_cast<void*>(agent),
        static_cast<unsigned long long>(agent ? RvaOf(mem::Q(agent)) : 0), reinterpret_cast<void*>(her_owner), reinterpret_cast<void*>(g_body),
        reinterpret_cast<void*>(sam_agent), static_cast<unsigned long long>(sam_agent ? RvaOf(mem::Q(sam_agent)) : 0), reinterpret_cast<void*>(sam_owner),
        reinterpret_cast<void*>(g_local));
    if (!why && !mgr) why = "no AI manager";
    if (!why && !agent) why = "her body has no agent";
    if (!why && !an) why = "her agent's vt+0x10 is not her body";
    const std::uintptr_t res = why ? 0 : SehResource(g_local);
    const char* rn = res ? mem::RttiName(mem::RttiOf(res)) : nullptr;
    if (!why && (!res || !rn || std::strcmp(rn, "AIVisualStimulusResource") != 0)) why = "Sam's resource is not an AIVisualStimulusResource";
    double p[3] = {};
    if (!why && !Pos(g_body, p)) why = "her body has no place";
    std::uint64_t st = 0;
    std::uint32_t mask = 0;
    if (!why && game::LoadOrSetup(&st, &mask)) why = "a load or setup";
    if (why) {
        Log("[probe] L1 hers not made (%s; agent %p %s; resource %p %s)", why, reinterpret_cast<void*>(agent), an ? an : "-", reinterpret_cast<void*>(res),
            rn ? rn : "-");
        return std::snprintf(msg, cap, "L1 not made: %s", why), true;
    }
    std::uint32_t id = 0;
    const bool id_ok = (g_groups & kGReg) && RegistryPlayerId(&id);
    if (!id_ok) id = 0;
    const double at[3] = {p[0], p[1], p[2] + 1.0};
    const float vel[4] = {};
    bool fault = false;
    const std::uintptr_t s = SehVisCreate(mgr, res, at, vel, agent, &fault);
    if (!s) {
        if (fault) StimDead("the create");
        Log("[probe] L1 hers not made (the create %s)", fault ? "FAULTED" : "returned none: the pool full");
        return std::snprintf(msg, cap, "L1 not made: create"), true;
    }
    const std::uint8_t zero = 0;
    SehWrite(s + kSId, &id, 4);
    SehWrite(s + kSOff, &zero, 1);
    g_her = Her{};
    g_her.stim = s, g_her.res = res, g_her.agent = agent, g_her.body = g_body, g_her.mgr = mgr, g_her.t0 = g_now, g_her.id88 = id;
    Publish();
    ++g_made;
    const std::uintptr_t ss = SamStimNow();
    Log("[probe] L1 hers made #%u (mode L1, id 0x%X%s, stim %p, agent %p %s, resource %p life %.2f; Sam's stim %p resource %p)", g_made, id,
        id_ok ? "" : " (registry unread: 0)", reinterpret_cast<void*>(s), reinterpret_cast<void*>(agent), an, reinterpret_cast<void*>(res),
        static_cast<double>(F32(res + kResLife)), reinterpret_cast<void*>(ss), reinterpret_cast<void*>(ss ? mem::Q(ss + kSRes) : 0));
    StimTick();
    return std::snprintf(msg, cap, "L1 made #%u", g_made), true;
}

// ---- byte checks and the detours
// (run 3) A site whose bytes are a jump into this module is a detour of ours (the shared sense hooks, installed by enemy_target
// before the probe's first check), not a changed game: its E9, then MinHook's relay (FF 25 00000000 + an absolute qword), at most
// three jumps, the last landing inside the mod's own image (GetModuleHandleEx FROM_ADDRESS + the PE's SizeOfImage).
bool OurDetourAt(std::uintptr_t va) noexcept {
    static std::uintptr_t lo = 0, hi = 0;
    if (!lo) {
        HMODULE m = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&OurDetourAt), &m) || !m)
            return false;
        const auto base = reinterpret_cast<std::uintptr_t>(m);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + static_cast<std::uintptr_t>(dos->e_lfanew));
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE) return false;
        lo = base, hi = base + nt->OptionalHeader.SizeOfImage;
    }
    std::uintptr_t at = va;
    for (int hop = 0; hop < 3 && at; ++hop) {
        std::uint8_t b[6] = {};
        if (!mem::Read(at, b, sizeof(b))) return false;
        std::int32_t rel = 0;
        if (b[0] == 0xE9) {
            std::memcpy(&rel, b + 1, 4);
            at = at + 5 + static_cast<std::intptr_t>(rel);
        } else if (b[0] == 0xFF && b[1] == 0x25) {
            std::memcpy(&rel, b + 2, 4);
            at = mem::Q(at + 6 + static_cast<std::intptr_t>(rel));
        } else {
            return false;
        }
        if (at >= lo && at < hi) return true;
    }
    return false;
}
void Checks() noexcept {
    if (g_checked) return;
    g_checked = true;
    unsigned bad = 0, shared = 0, n = 0;
    for (const Site& s : kSites) {
        ++n;
        if (mem::CodeMatches(A(s.va), s.sig)) continue;
        if (OurDetourAt(A(s.va))) shared |= s.group;  // our own detour's jump: the game's bytes are under it
        else bad |= s.group, Log("[probe] L1 byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
    }
    struct Rip {
        std::uintptr_t got, want;
        const char* name;
        unsigned group;
    };
    const Rip rips[] = {
        {mem::RipTarget(A(kHumResSite + 4), 1, 5), A(kResGet), "the Humanoid's getter call", kGStim},
        {mem::RipTarget(A(kHumCreateSite + 7), 1, 5), A(kVisCreate), "the Humanoid's create call", kGStim},
        {mem::RipTarget(A(kTsSite), 3, 7), A(kStimMgr), "the AI manager global", kGStim},
        {mem::Q(A(kKnowVt) + 0x38), A(kPsr), "K's vt+0x38", kGPsr},
        {mem::RipTarget(A(kShootSite), 3, 7), A(kShootRtti), "the shoot goal's type", kGGoal},
        {mem::RipTarget(A(kAimSite), 3, 7), A(kAimRtti), "the aim goal's type", kGGoal},
        {mem::RipTarget(A(kAimSite + 7), 1, 5), A(kGoalFind), "the aim goal's lookup call", kGGoal},
        {mem::Q(A(kDaemonVt) + 0x40), A(kDaemonFn), "the daemon's vt+0x40", kGDaemon},
        {mem::RipTarget(A(kSingletonSite), 3, 7), A(kSingletons), "the singleton table global", kGRead},
        {mem::RipTarget(A(kRegSite), 3, 7), A(kRegGlobal), "the actor registry global", kGReg},
        {mem::RipTarget(A(kAttackRegSite), 3, 7), A(kAttackReg), "the attack registry global", kGMelee},
    };
    for (const Rip& r : rips) {
        ++n;
        if (r.got != r.want) bad |= r.group, Log("[probe] L1 byte check FAILED: %s", r.name);
    }
    g_groups = (kGStim | kGCls | kGApp | kGPsr | kGGoal | kGDaemon | kGPhase | kGMelee | kGRead | kGReg) & ~bad;
    auto ok = [&](unsigned g) { return (bad & g) ? "FAILED" : (shared & g) ? "ok (shared)" : "ok"; };
    Log("[probe] L1 byte checks (%u): stimulus %s, classifier %s, append %s, sensor results %s, goals %s, daemon %s, phase %s, melee %s, reads %s, registry %s",
        n, ok(kGStim), ok(kGCls), ok(kGApp), ok(kGPsr), ok(kGGoal), ok(kGDaemon), ok(kGPhase), ok(kGMelee), ok(kGRead), ok(kGReg));
}
void InstallDetours() noexcept {
    if (g_detours_tried) return;
    g_detours_tried = true;
    const char* text[3] = {"off (byte checks)", "off (byte checks)", "off (byte checks)"};
    const unsigned ok = ((g_groups & kGCls) ? sense::kCls : 0u) | ((g_groups & kGApp) ? sense::kApp : 0u) | ((g_groups & kGPsr) ? sense::kPsr : 0u);
    const unsigned in = sense::Ensure(sense::kCls | sense::kApp | sense::kPsr, ok, text);
    if (in & sense::kCls & ok) g_detours |= kGCls;
    if (in & sense::kApp & ok) g_detours |= kGApp;
    if (in & sense::kPsr & ok) g_detours |= kGPsr;
    g_counting.store(true);
    Log("[probe] L1 detours (read-only counts): classifier %s, append %s, sensor results %s", text[0], text[1], text[2]);
}

// ---- op 0: the reads
void Reads() noexcept {
    char tab[96] = "-";
    if (g_groups & kGRead) {
        const std::uintptr_t t = mem::Q(A(kSingletons));
        std::int32_t n = -1;
        std::uint8_t bytes[8] = {};
        const std::uintptr_t bp = t ? mem::Q(t + 0x18) : 0;
        const bool bok = bp && mem::Read(bp, bytes, sizeof(bytes));
        if (t && mem::Rd(t + 0x10, n))
            std::snprintf(tab, sizeof(tab), "%p n=%d +0x18 %p [%02X %02X %02X %02X %02X %02X %02X %02X]%s", reinterpret_cast<void*>(t), n,
                          reinterpret_cast<void*>(bp), bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bok ? "" : " (unread)");
    }
    char pts[200] = "-";
    double body[3] = {};
    const std::uintptr_t agent = g_body ? mem::Q(g_body + kEAgent) : 0;
    if ((g_groups & kGRead) && agent && Pos(g_body, body)) {
        const std::uintptr_t vt = mem::Q(agent);
        const std::uintptr_t fc = mem::Q(vt + 0x80), fp = mem::Q(vt + 0x88);
        if (mem::InText(fc) && mem::InText(fp)) {
            const int n = SehAgentCount(agent, fc);
            std::size_t at = static_cast<std::size_t>(std::snprintf(pts, sizeof(pts), "n=%d", n));
            for (int i = 0; i < n && i < 3 && at < sizeof(pts); ++i) {
                std::uint8_t raw[32] = {};
                const int r = SehAgentPoint(agent, fp, i, raw);
                double d[3];
                float f[4];
                std::memcpy(d, raw, sizeof(d));
                std::memcpy(f, raw, sizeof(f));
                const int w = std::snprintf(pts + at, sizeof(pts) - at, "; #%d %s f64 off (%.2f,%.2f,%.2f) f32 (%.2f,%.2f,%.2f)", i, r > 0 ? "ok" : "none", d[0] - body[0],
                                            d[1] - body[1], d[2] - body[2], static_cast<double>(f[0]), static_cast<double>(f[1]), static_cast<double>(f[2]));
                if (w > 0) at += static_cast<std::size_t>(w);
            }
        } else {
            std::snprintf(pts, sizeof(pts), "vt+0x80/+0x88 not code");
        }
    }
    const std::uintptr_t ss = SamStimNow();
    double sp[3] = {}, sam[3] = {};
    const bool sok = ss && mem::Read(ss + kSPos, sp, sizeof(sp)) && Pos(g_local, sam);
    const std::uintptr_t sres = ss ? mem::Q(ss + kSRes) : 0;
    const std::uintptr_t gres = (g_groups & kGStim) && g_local && g_local_ok ? SehResource(g_local) : 0;
    Log("[probe] L1 reads: singleton %s; her agent %p points %s", tab, reinterpret_cast<void*>(agent), pts);
    Log("[probe] L1 reads: Sam stim %p off (%.2f,%.2f,%.2f) a0 %.3f a4 %.3f b0 %.3f +0x88 0x%X life %.2f +0x84 %u; resource %p %s +0x20 %p life %.2f; "
        "getter resource %p %s (%s Sam's)",
        reinterpret_cast<void*>(ss), sok ? sp[0] - sam[0] : 0.0, sok ? sp[1] - sam[1] : 0.0, sok ? sp[2] - sam[2] : 0.0,
        static_cast<double>(ss ? F32(ss + kSA0) : 0.f), static_cast<double>(ss ? F32(ss + kSMul) : 0.f), static_cast<double>(ss ? F32(ss + kSB0) : 0.f),
        ss ? U32(ss + kSId) : 0u, static_cast<double>(ss ? F32(ss + kSLife) : 0.f), ss ? static_cast<unsigned>(U8(ss + kSOff)) : 0u,
        reinterpret_cast<void*>(sres), NameOf(sres), reinterpret_cast<void*>(sres ? mem::Q(sres + 0x20) : 0), static_cast<double>(sres ? F32(sres + kResLife) : 0.f),
        reinterpret_cast<void*>(gres), NameOf(gres), gres && gres == sres ? "the same as" : "not");
}

// ---- op 5 (the guest, F): her own stimulus standing / crouched
bool CrouchRead(int crouched, char* msg, std::size_t cap) noexcept {
    const std::uintptr_t ss = SamStimNow();
    if (!ss) {
        Log("[probe] L1 vis (%s): no stimulus of our Sam", crouched ? "crouched" : "standing");
        return std::snprintf(msg, cap, "no stimulus"), true;
    }
    Crouch c;
    c.have = true, c.a4 = F32(ss + kSMul), c.a0 = F32(ss + kSA0), c.b0 = F32(ss + kSB0), c.f2b0 = U32(g_local + 0x2B0);
    const std::uintptr_t q = mem::Q(g_local + 0x2B0);
    Log("[probe] L1 vis (%s): a4 %.3f a0 %.3f b0 %.3f; Sam +0x2B0 u32 0x%X (bit 11 %u) q %p", crouched ? "crouched" : "standing", static_cast<double>(c.a4),
        static_cast<double>(c.a0), static_cast<double>(c.b0), c.f2b0, (c.f2b0 >> 11) & 1u, reinterpret_cast<void*>(q));
    if (!crouched) {
        g_stand = c;
        return std::snprintf(msg, cap, "standing a4 %.3f", static_cast<double>(c.a4)), true;
    }
    Log("[probe] L1 result arm F: standing=%d a4_stand=%.3f a4_crouch=%.3f lower=%d a0_stand=%.3f a0_crouch=%.3f b0_stand=%.3f b0_crouch=%.3f f2b0_stand=0x%X "
        "f2b0_crouch=0x%X",
        g_stand.have ? 1 : 0, static_cast<double>(g_stand.a4), static_cast<double>(c.a4), g_stand.have && c.a4 < g_stand.a4 ? 1 : 0,
        static_cast<double>(g_stand.a0), static_cast<double>(c.a0), static_cast<double>(g_stand.b0), static_cast<double>(c.b0), g_stand.f2b0, c.f2b0);
    return std::snprintf(msg, cap, "crouched a4 %.3f (standing %.3f)", static_cast<double>(c.a4), static_cast<double>(g_stand.a4)), true;
}

// ---- op 7: the melee query
bool Melee(std::uint32_t id, std::uint16_t attack, char* msg, std::size_t cap) noexcept {
    std::uintptr_t actor = 0, entity = 0;
    std::uint32_t state = 0;
    const char* why = !(g_groups & kGMelee) ? "byte checks" : !MuleOf(id, &actor, &entity, &state) || !entity ? "not realized" : !g_body ? "no partner body" : nullptr;
    double her[3] = {};
    if (!why && !Pos(HerEntity(), her)) why = "her place";
    std::uintptr_t ctx = 0;
    int step = 0;
    const int cr = why ? 0 : SehCtx(attack, AttackEntry(attack), &ctx, &step);
    if (!why && cr < 0) why = "the attack context FAULTED";
    if (!why && cr == 0) why = step == 1 ? "no attack context" : step == 2 ? "the attack context's parameters (not the context type)" : "no registry entry for the attack";
    if (why) {
        Log("[probe] L1 melee query id=%05X: skipped (%s)", id, why);
        return std::snprintf(msg, cap, "melee query skipped: %s", why), true;
    }
    MeleeUser u;
    u.her = HerEntity(), u.sam = SamEntity();
    const double chest[3] = {her[0], her[1], her[2] + R::kChest};
    const int r = vhit::QueryHit(entity, ctx, chest, 1.0f, 0.f, &MeleeCb, reinterpret_cast<std::uintptr_t>(&u));
    if (g_w.on && g_w.id == id) ++g_w.melee_n, g_w.melee_seen += u.seen, g_w.melee_her += u.hers, g_w.melee_sam += u.sams;
    Log("[probe] L1 melee query id=%05X (the attack sphere query from its entity, attack 0x%X; not its own melee): r %d (%s); seen %u, her body %u, Sam %u", id, attack, r,
        r > 0 ? "called" : r == 0 ? "the direct path's checks failed" : "FAULT", u.seen, u.hers, u.sams);
    return std::snprintf(msg, cap, "melee query: r %d seen %u hers %u", r, u.seen, u.hers), true;
}

void Decision() noexcept {
    const R::Arm& b = g_arm_b.a;
    const R::Arm& c = g_arm_c.a;
    const R::Arm& d = g_arm_d.a;
    const int dec = R::Decide(b, c, d);
    char missing[16] = {};
    std::snprintf(missing, sizeof(missing), "%s%s%s", g_arm_b.have ? "" : "B", g_arm_c.have ? "" : "C", g_arm_d.have ? "" : "D");
    const char* melee = !g_arm_c.have || !g_arm_c.melee_n ? "-" : g_arm_c.melee_her ? "A" : "B";
    Log("[probe] L1 decision: %s%s%s (valid B %d C %d D %d; B noticed %.0f aim %d/%d fired %u at her %u (%u on their own line); C noticed %.0f turned %.0f "
        "approached %d; D tie %s switch %.0f / %.0f; Sam pull B %d C %d); melee path %s; G turned %s; not run: H2 (the host unseeable), H7 (Sam 400 m "
        "away), real cover",
        R::DecisionText(dec), missing[0] ? "; missing arms " : "", missing, R::Invalid(b) ? 0 : 1, R::Invalid(c) ? 0 : 1, R::Invalid(d) ? 0 : 1, Ms(b.noticed),
        b.aim_her, b.aim_n, b.fired, b.at_her, b.rays, Ms(c.noticed), Ms(c.turned_her), c.approached_her ? 1 : 0, Who(d.last_target[0]), Sw(d.switch_sam),
        Sw(d.switch_her), R::SamPull(b) ? 1 : 0, R::SamPull(c) ? 1 : 0, melee, g_arm_g.have ? Who(R::TurnedDuring(g_arm_g.a)) : "-");
}

std::uint32_t IdArg(double v) noexcept { return v > 0 && v < 4294967296.0 ? static_cast<std::uint32_t>(v) : 0u; }
}  // namespace

bool Command(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
    if (!mem::Base()) return std::snprintf(msg, cap, "no game image"), false;
    Checks();
    InstallDetours();
    const int op = static_cast<int>(v[0]);
    switch (op) {
        case 0:
            Reads();
            LogCounts("op 0");
            return std::snprintf(msg, cap, "L1 byte checks + reads logged"), true;
        case 1:
            if (v[1] > 0) return Make(msg, cap);
            RemoveNow("off");
            return std::snprintf(msg, cap, "L1 off"), true;
        case 2: {
            const double x = v[1] < 0 ? 0 : v[1];
            g_her.vis_forced = true;
            g_her.vis = static_cast<float>(x / 100.0);
            if (g_w.on && std::strcmp(g_w.label, "D") == 0) {
                if (x <= 0 && g_w.phase == 0) g_w.phase = 1;
                else if (x > 0 && g_w.phase == 1) g_w.phase = 2;
            }
            Log("[probe] L1 vis forced %.2f (switch phase %d)", static_cast<double>(g_her.vis), g_w.phase);
            return std::snprintf(msg, cap, "L1 vis %.2f", static_cast<double>(g_her.vis)), true;
        }
        case 3: {
            const std::uint32_t id = IdArg(v[1]);
            if (!id) return std::snprintf(msg, cap, "enemy_l1 3 <id> <s>"), false;
            EndWatch("a new watch");
            StartWatch(id, v[2] > 0 ? v[2] : 20, g_role == 2 ? "U21" : "", false);
            return std::snprintf(msg, cap, "L1 watch %05X", id), true;
        }
        case 4: {
            const int code = static_cast<int>(v[1]);
            const std::uint32_t id = IdArg(v[2]);
            const char* label = code == 1 ? "A" : code == 2 ? "B" : code == 3 ? "C" : code == 4 ? "D" : code == 7 ? "G" : nullptr;
            if (!label || !id) return std::snprintf(msg, cap, "enemy_l1 4 <arm 1|2|3|4|7> <id>"), false;
            EndWatch("a new arm");
            if (!g_counts_armed) {
                shots::ArmOwnerCounts(true);
                g_counts_armed = true;
                Log("[probe] L1 shot counts armed (the enqueue hook %s)", shots::EnqueueHooked() ? "in" : "NOT in: no rounds will count");
            }
            if (code == 4) g_her.vis_forced = false;
            StartWatch(id, 120, label, true);
            Log("[probe] L1 arm %s id=%05X: start (stim %s, cp %05X)", label, id, g_her.stim ? "hers" : "none", g_w.cp_id);
            return std::snprintf(msg, cap, "L1 arm %s", label), true;
        }
        case 5:
            return CrouchRead(v[1] > 0 ? 1 : 0, msg, cap);
        case 6: {
            const std::uint32_t id = IdArg(v[1]);
            if (!id) return std::snprintf(msg, cap, "enemy_l1 6 <id> [s]"), false;
            int members = 0, cp_before = -1, cp_after = -1;
            const int r = CalmApply(id, &members, &cp_before, &cp_after);
            const double secs = v[2] > 0 ? v[2] : 10;
            g_calm = Calm{};
            g_calm.on = true, g_calm.id = id, g_calm.t0 = g_now, g_calm.until = g_now + secs * 1000.0, g_calm.next = g_now + 500;
            g_calm.next_apply = g_now + R::kCalmApplyMs, g_calm.applies = 1, g_calm.n = ++g_calm_seq, g_calm.cp_forces = r == 1 ? 1 : 0;
            g_calm.max_applies = R::CalmMaxApplies(secs);
            // (run 3) the CP forced only when it read alerted (3..5); at rest (0..2) it is left alone
            char cpt[64];
            if (r == 1) std::snprintf(cpt, sizeof(cpt), "forced 0 (was %d, now %d)", cp_before, cp_after);
            else if (r == 2 && cp_before == -2) std::snprintf(cpt, sizeof(cpt), "unread (not forced)");
            else if (r == 2 && cp_before > R::kCpBaseMax) std::snprintf(cpt, sizeof(cpt), "at %d (not 3..5, not forced)", cp_before);
            else if (r == 2) std::snprintf(cpt, sizeof(cpt), "at %d (resting, not forced)", cp_before);
            else std::snprintf(cpt, sizeof(cpt), "%s", r == 0 ? "not found" : r == -2 ? "skipped (byte checks)" : "FAULT");
            Log("[probe] L1 calm #%u id=%05X: phase 0 on %d members, the CP %s", g_calm.n, id, members, cpt);
            return std::snprintf(msg, cap, "L1 calm #%u", g_calm.n), true;
        }
        case 7: {
            const std::uint32_t id = IdArg(v[1]);
            if (!id) return std::snprintf(msg, cap, "enemy_l1 7 <id> [attack]"), false;
            const std::uint16_t attack = v[2] > 0 && v[2] < 65536 ? static_cast<std::uint16_t>(v[2]) : kDefaultMelee;
            return Melee(id, attack, msg, cap);
        }
        case 8:
            if (!g_w.on) return std::snprintf(msg, cap, "no arm"), false;
            EndWatch("the arm ended");
            return std::snprintf(msg, cap, "L1 arm ended"), true;
        case 9:
            EndWatch("stop");
            RemoveNow("stop");
            g_her.vis_forced = false;
            g_calm.on = false;
            if (g_arm_b.have || g_arm_c.have || g_arm_d.have) Decision();
            LogCounts("stop");
            Log("[probe] L1 stop");
            return std::snprintf(msg, cap, "L1 stopped"), true;
        default:
            return std::snprintf(msg, cap, "enemy_l1 <op 0..9>"), false;
    }
}

void Tick(const ecombat::TickIn& in, double now) noexcept {
    g_now = now;
    g_role = in.role;
    g_local = reinterpret_cast<std::uintptr_t>(in.local);
    g_body = reinterpret_cast<std::uintptr_t>(in.body);
    g_local_ok = in.local_ok;
    ++g_tick;
    if (g_her.pending) Step2(false);
    if (g_her.stim) StimTick();
    // the detours' match (by agent; review C1): the host's her = the agent her stimulus of ours carries, Sam = our Sam's; the
    // guest's her = its own Sam's, Sam = the partner body's while a stimulus of ours carries it
    const std::uintptr_t ours = g_her.stim && !g_her.pending ? g_her.agent : etarget::StimAgent();  // (round 27: else the partner stimulus' agent)
    const std::uintptr_t own = g_local ? mem::Q(g_local + kEAgent) : 0;
    g_match_her.store(g_role == 2 ? own : ours, std::memory_order_relaxed);
    g_match_sam.store(g_role == 2 ? ours : own, std::memory_order_relaxed);
    if (!g_checked) return;  // nothing else before the first op
    if (g_calm.on) CalmTick();
    if (g_w.on) {
        if (now >= g_w.until) EndWatch("its time");
        else if (now >= g_w.next) {
            g_w.next = now + 250;
            SampleWatch();
            if (now >= g_w.next_counts) g_w.next_counts = now + 5000, LogCounts(g_w.label[0] ? g_w.label : "watch");
        }
    }
}

void SenseCls(std::uintptr_t stim, std::uintptr_t r, int* type_out) noexcept { CountCls(stim, r, type_out); }
void SenseApp(std::uintptr_t array, std::uintptr_t record) noexcept { CountApp(array, record); }
void SensePsr(std::uintptr_t k) noexcept { CountPsr(k); }

bool TargetArm(const char* label, std::uint32_t id, double secs, char* msg, std::size_t cap) noexcept {
    if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
    if (!mem::Base()) return std::snprintf(msg, cap, "no game image"), false;
    if (!id || !label || !label[0]) return std::snprintf(msg, cap, "no arm"), false;
    Checks();
    InstallDetours();
    EndWatch("a new arm");
    if (!g_counts_armed) {
        shots::ArmOwnerCounts(true);
        g_counts_armed = true;
        Log("[probe] L1 shot counts armed (the enqueue hook %s)", shots::EnqueueHooked() ? "in" : "NOT in: no rounds will count");
    }
    StartWatch(id, secs > 0 ? secs : 120, label, true);
    g_w.ext = true;
    g_w.stim = etarget::StimLive();
    Log("[probe] L1 arm %s id=%05X: start (stim %s, cp %05X)", label, id, g_w.stim ? "partner" : "none", g_w.cp_id);
    return std::snprintf(msg, cap, "L1 arm %s", label), true;
}
bool TargetArmEnd(char* msg, std::size_t cap) noexcept {
    if (!g_w.on || !g_w.ext) return std::snprintf(msg, cap, "no target arm"), false;
    EndWatch("the arm ended");
    return std::snprintf(msg, cap, "L1 arm ended"), true;
}
void TargetArmPhase(bool hidden) noexcept {
    if (!g_w.on || !g_w.ext) return;
    if (hidden && g_w.phase == 0) g_w.phase = 1;
    else if (!hidden && g_w.phase == 1) g_w.phase = 2;
}
std::uint32_t TargetArmId() noexcept { return g_w.on && g_w.ext ? g_w.id : 0u; }

void ReleaseBeforeFree(const char* why) noexcept {
    if (!g_her.stim || g_her.pending) return;
    Release(why ? why : "her body going");
}

void EmergencyStop() noexcept {
    g_off.store(true);
    const std::uintptr_t stim = g_her_stim.exchange(0, std::memory_order_acquire);
    const std::uintptr_t agent = g_her_agent.exchange(0), res = g_her_res.exchange(0), hmgr = g_her_mgr.exchange(0);
    if (!stim || !agent) return;
    const std::uintptr_t mgr = mem::Q(A(kStimMgr));
    if (mgr && mgr != hmgr) return;  // another world's manager: not ours to touch
    if (mem::Q(stim + kSAgent) != agent || mem::Q(stim + kSRes) != res) return;  // no longer ours
    Step1(stim, agent, res, mgr);  // (no lock: any thread, the tick may be stopped mid-frame)
}

void AfterStop() noexcept {
    static unsigned tries = 0;
    if (tries >= 3 || !g_her.stim) return;
    if (tries++ == 0 && !g_her.pending) {
        if (OursParked(g_her.stim)) {  // the emergency stop's step 1 ran
            g_her.pending = true;
            g_her.pending_tick = g_tick;
            Log("[probe] L1 after the emergency stop: hers parked; removing");
        } else {
            Release("after the emergency stop");
        }
    }
    if (g_her.pending) Step2(true);
}

}  // namespace st::l1probe
