// Round 27: the visual stimulus primitives shared by the M1 probe (enemy_l1_probe.cpp) and the partner stimulus (enemy_target.cpp;
// docs/enemy-sync.md 15-16). Internal to the mod: the game's create / remove under SEH, the pool lock, the atomic stores and the
// C1 release's first step. POD only (no C++ objects inside the __try blocks). Proven in the M1 hidden runs; keep them as they are.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "partner_source.hpp"

namespace st::stimcore {
namespace mem = partner::mem;

inline constexpr std::uintptr_t kImage = 0x140000000ull;
inline std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

inline constexpr std::uintptr_t kStimMgr = 0x147BE0D38;     // the AI manager: +0x80 the visual pool, +0x88 its live list, +0x3C398 the thread-safe copies
inline constexpr std::uintptr_t kVisCreate = 0x1425249A0;   // (pool, resource, const pos f64[3]*, const vel f32[4]*, agent) -> stimulus
inline constexpr std::uintptr_t kStimRemove = 0x14254CE00;  // (pool, stimulus)
inline constexpr std::uintptr_t kResGet = 0x141DDE990;      // a streaming ref's object
inline constexpr std::uintptr_t kGoalFind = 0x1425BD930;    // (individual + 0x3670, rtti*) -> the goal or 0
inline constexpr std::uintptr_t kRegGlobal = 0x147BBB8C0;   // the game actor registry (vt+0x40: the local player's actor id)
// the stimulus: +0x08 resource, +0x10 agent, +0x18 place f64[3], +0x80 life, +0x84 off, +0x88 actor id, +0x90 velocity f32[4],
// +0xA0, +0xA4 the visibility multiplier, +0xB0, +0xC8 the next one
inline constexpr std::uintptr_t kSRes = 0x08, kSAgent = 0x10, kSPos = 0x18, kSLife = 0x80, kSOff = 0x84, kSId = 0x88, kSVel = 0x90, kSA0 = 0xA0,
                                kSMul = 0xA4, kSB0 = 0xB0, kSNext = 0xC8;
inline constexpr std::uintptr_t kMgrPool = 0x80, kMgrLive = 0x88, kMgrTs = 0x3C398, kPoolCs = 0x1A020;
inline constexpr std::uint32_t kNoActor = 0x1FFFFF;
inline constexpr std::uintptr_t kEAgent = 0x170, kObjRes = 0x3B0, kResLife = 0x2C, kIndGoals = 0x3670;
inline constexpr unsigned kListCap = 600;

// our Sam's AIVisualStimulusResource: getter(Sam + 0x58) -> +0x3B0
inline std::uintptr_t SehResource(std::uintptr_t sam) noexcept {
    __try {
        const std::uintptr_t o = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(A(kResGet))(sam + 0x58);
        return o ? *reinterpret_cast<volatile std::uintptr_t*>(o + kObjRes) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
// the agent's vt+0x10 (the entity the sensor walks; 0x142E4F657), called under SEH
inline std::uintptr_t SehAgentEntity(std::uintptr_t agent) noexcept {
    __try {
        const std::uintptr_t vt = *reinterpret_cast<volatile std::uintptr_t*>(agent);
        if (!vt) return 0;
        const std::uintptr_t fn = *reinterpret_cast<volatile std::uintptr_t*>(vt + 0x10);
        if (!fn || !mem::InText(fn)) return 0;
        return reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(fn)(agent);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
inline std::uintptr_t SehVisCreate(std::uintptr_t mgr, std::uintptr_t res, const double (&pos)[3], const float (&vel)[4], std::uintptr_t agent,
                                   bool* fault) noexcept {
    *fault = false;
    __try {
        return reinterpret_cast<std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, const double*, const float*, std::uintptr_t)>(A(kVisCreate))(
            mgr + kMgrPool, res, pos, vel, agent);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = true;
        return 0;
    }
}
inline int SehStimRemove(std::uintptr_t mgr, std::uintptr_t s) noexcept {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, std::uintptr_t)>(A(kStimRemove))(mgr + kMgrPool, s);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
inline int SehWrite(std::uintptr_t dst, const void* src, std::size_t n) noexcept {
    __try {
        std::memcpy(reinterpret_cast<void*>(dst), src, n);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
inline std::uintptr_t SehGoal(std::uintptr_t ind, std::uintptr_t rtti) noexcept {
    __try {
        return reinterpret_cast<std::uintptr_t (*)(std::uintptr_t, std::uintptr_t)>(A(kGoalFind))(ind + kIndGoals, rtti);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
using FnPlayerId = const std::uint32_t* (*)(std::uintptr_t reg, void* out);
inline bool RegistryPlayerId(std::uint32_t* out) noexcept {
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

// The visual pool's CRITICAL_SECTION (pool + 0x1A020): the remove and the pool's allocator take it (recursive: the remove inside
// ours is fine). The thread-safe rebuild 0x14255F5E0 does NOT take it.
inline int SehEnterCs(std::uintptr_t mgr) noexcept {
    __try {
        EnterCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(mgr + kMgrPool + kPoolCs));
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
inline void SehLeaveCs(std::uintptr_t mgr) noexcept {
    __try {
        LeaveCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(mgr + kMgrPool + kPoolCs));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
struct PoolLock {
    std::uintptr_t mgr;
    bool held;
    explicit PoolLock(std::uintptr_t m) noexcept : mgr(m), held(m && SehEnterCs(m) > 0) {}
    ~PoolLock() {
        if (held) SehLeaveCs(mgr);
    }
    PoolLock(const PoolLock&) = delete;
    PoolLock& operator=(const PoolLock&) = delete;
};
inline int SehStore32(std::uintptr_t a, std::uint32_t v) noexcept {
    __try {
        InterlockedExchange(reinterpret_cast<volatile LONG*>(a), static_cast<LONG>(v));
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// 1 swapped, 0 it held something else (left alone), -1 a fault
inline int SehCas64(std::uintptr_t a, std::uintptr_t want, std::uintptr_t to) noexcept {
    __try {
        return InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(a), static_cast<LONG64>(to), static_cast<LONG64>(want)) ==
                       static_cast<LONG64>(want)
                   ? 1
                   : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

inline bool InLiveList(std::uintptr_t mgr, std::uintptr_t stim) noexcept {
    unsigned n = 0;
    for (std::uintptr_t s = mgr ? mem::Q(mgr + kMgrLive) : 0; s && n < kListCap; s = mem::Q(s + kSNext), ++n)
        if (s == stim) return true;
    return false;
}
// the ownership signatures (review S3/C5): live = our agent with Sam's resource (no stimulus of the game's pairs them); parked
// (after step 1) = no agent, the resource, "no actor"
inline bool OursLive(std::uintptr_t s, std::uintptr_t agent, std::uintptr_t res) noexcept {
    return s && agent && mem::Q(s + kSAgent) == agent && mem::Q(s + kSRes) == res;
}
inline bool OursParked(std::uintptr_t s, std::uintptr_t res) noexcept {
    std::uint32_t id = 0;
    return s && !mem::Q(s + kSAgent) && mem::Q(s + kSRes) == res && mem::Rd(s + kSId, id) && id == kNoActor;
}

// The C1 release's first step (any caller, before the body or its agent can be freed): +0xA4 0, +0x88 "no actor", +0x84 1, then
// +0x10 cleared by a compare-exchange (only while it still holds the agent). Its thread-safe copies (matched by the agent and the
// resource): +0xA4 0 and +0x88 "no actor" only. Nothing is freed here. Returns the faults.
inline unsigned Step1(std::uintptr_t stim, std::uintptr_t agent, std::uintptr_t res, std::uintptr_t mgr) noexcept {
    unsigned faults = 0;
    const std::uint8_t one = 1;
    if (SehStore32(stim + kSMul, 0) < 0) ++faults;
    if (SehStore32(stim + kSId, kNoActor) < 0) ++faults;
    if (SehWrite(stim + kSOff, &one, 1) < 0) ++faults;
    if (SehCas64(stim + kSAgent, agent, 0) < 0) ++faults;
    unsigned n = 0;
    for (std::uintptr_t s = mgr && agent ? mem::Q(mgr + kMgrTs) : 0; s && n < kListCap; s = mem::Q(s + kSNext), ++n)
        if (mem::Q(s + kSAgent) == agent && mem::Q(s + kSRes) == res) {
            if (SehStore32(s + kSMul, 0) < 0) ++faults;
            if (SehStore32(s + kSId, kNoActor) < 0) ++faults;
        }
    return faults;
}

// Round 27 M4 (the destroy guard, docs/enemy-sync.md 16.2): a volatile read under SEH (0 on a fault). Any thread, no
// ReadProcessMemory: the World::RemoveEntityNow detour and the classifier detour may use it.
inline std::uintptr_t SehQ(std::uintptr_t a) noexcept {
    __try {
        return a ? *reinterpret_cast<volatile std::uintptr_t*>(a) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
// Step1 for any thread (the destroy guard's detour, the worker guard): the live record's same four writes, then its thread-safe
// copies walked with SehQ (not mem::Q, a ReadProcessMemory) and given +0xA4 0 and +0x88 "no actor" only: their +0x10 keeps the agent
// (the classifier guard refuses it by value). No lock, no allocation, no log. Returns the faults; `copies` the copies matched.
inline unsigned Step1Any(std::uintptr_t stim, std::uintptr_t agent, std::uintptr_t res, std::uintptr_t mgr, unsigned* copies) noexcept {
    unsigned faults = 0, matched = 0;
    const std::uint8_t one = 1;
    if (SehStore32(stim + kSMul, 0) < 0) ++faults;
    if (SehStore32(stim + kSId, kNoActor) < 0) ++faults;
    if (SehWrite(stim + kSOff, &one, 1) < 0) ++faults;
    if (SehCas64(stim + kSAgent, agent, 0) < 0) ++faults;
    unsigned n = 0;
    for (std::uintptr_t s = mgr && agent ? SehQ(mgr + kMgrTs) : 0; s && n < kListCap; s = SehQ(s + kSNext), ++n)
        if (SehQ(s + kSAgent) == agent && SehQ(s + kSRes) == res) {
            ++matched;
            if (SehStore32(s + kSMul, 0) < 0) ++faults;
            if (SehStore32(s + kSId, kNoActor) < 0) ++faults;
        }
    if (copies) *copies = matched;
    return faults;
}

}  // namespace st::stimcore
