// The destroy guard (entity_remove_hook.hpp; docs/enemy-sync.md 16.2).
#include "entity_remove_hook.hpp"

#include <windows.h>

#include "MinHook.h"

#include <atomic>
#include <cstdio>

#include "enemy_stim_core.hpp"
#include "enemy_target.hpp"
#include "log.hpp"

namespace st::rmhook {
namespace {
namespace mem = partner::mem;
using stimcore::A;

constexpr std::uintptr_t kRenVa = 0x14237B390;      // World::RemoveEntityNow (world, entity, flag, -)
constexpr std::uintptr_t kRenMark = 0x14237B3C4;    // rdi = rdx (the entity), r15 = rcx; lock or [rdx+0x88], 0x80000000 (bit 31 first)
constexpr std::uintptr_t kRenCallA = 0x14237AF33;   // its two callers' rel32 calls
constexpr std::uintptr_t kRenCallB = 0x142379FBA;
constexpr std::uintptr_t kRenAppend = 0x14237B7AB;  // lea rcx, [r15+0x2F0]; call: the destroy list's append (after the hook)
// op 13: the child loop's removal (0x14237B5F6..651): [0x147BC4E70] (mov rcx, [rip]; 0x14237B634) vt+0x20 (world, child, 0, 0)
constexpr std::uintptr_t kWorldSite = 0x14237B634, kWorldGlobal = 0x147BC4E70, kWorldVt = 0x143D5D140, kWorldRemove = 0x143587110;

using FnRen = void (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
FnRen o_ren = nullptr;
std::atomic<std::uintptr_t> g_watch[2]{}, g_removed[2]{};
std::atomic<unsigned> g_calls{0}, g_matched{0};
std::atomic<std::uint32_t> g_tid{0};
std::atomic<bool> g_sim{false};
std::atomic<bool> g_ready{false};
bool g_tried = false, g_world_ok = false;
unsigned g_ok = 0, g_total = 0;
char g_state[112] = "not installed";

// the match (any thread): no lock, no allocation, no log. `only` >= 0: that slot alone (op 12)
void Match(std::uintptr_t e, bool sim, int only) noexcept {
    if (!e) return;
    for (int i = 0; i < 2; ++i) {
        if (only >= 0 && i != only) continue;
        // (review) seq_cst: the store-load pair with Make (it stores the watch, then loads the removed mark; this stores the
        // removed mark, then loads the watch): at least one side sees the other
        if (g_watch[i].load(std::memory_order_seq_cst) != e) continue;
        g_removed[i].store(e, std::memory_order_seq_cst);
        g_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);  // a TEB read
        g_sim.store(sim, std::memory_order_relaxed);
        g_matched.fetch_add(1, std::memory_order_relaxed);
        if (i == kWatchStim) etarget::GameRemoving(e, sim);  // lock-free (enemy_target.hpp)
    }
}
void H_Ren(std::uintptr_t world, std::uintptr_t e, std::uintptr_t a3, std::uintptr_t a4) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    Match(e, false, -1);
    o_ren(world, e, a3, a4);
}

void Check(bool ok, const char* name, const char** first_bad) noexcept {
    ++g_total;
    if (ok) {
        ++g_ok;
        return;
    }
    Log("[enemy] tgt destroy guard byte check FAILED: %s", name);
    if (!*first_bad) *first_bad = name;
}
int SehCall4(std::uintptr_t fn, std::uintptr_t a, std::uintptr_t b) noexcept {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t)>(fn)(a, b, 0, 0);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
}  // namespace

bool Install() noexcept {
    if (g_tried) return g_ready.load();
    g_tried = true;
    if (!mem::Base()) {
        std::snprintf(g_state, sizeof(g_state), "off (no game image)");
        return false;
    }
    const char* bad = nullptr;
    // six checks, all needed for Ready (the gate's "byte checks 6/6")
    Check(mem::CodeMatches(A(kRenVa), "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 80 00 00 00"), "RemoveEntityNow's prologue", &bad);
    Check(mem::CodeMatches(A(kRenMark), "48 8B FA 4C 8B F9 B8 00 00 00 80 F0 48 09 82 88 00 00 00"), "RemoveEntityNow's bit 31 mark", &bad);
    Check(mem::CodeMatches(A(kRenCallA), "E8 58 04 00 00") && mem::RipTarget(A(kRenCallA), 1, 5) == A(kRenVa), "its caller at 0x14237AF33", &bad);
    Check(mem::CodeMatches(A(kRenCallB), "E8 D1 13 00 00") && mem::RipTarget(A(kRenCallB), 1, 5) == A(kRenVa), "its caller at 0x142379FBA", &bad);
    Check(mem::CodeMatches(A(kRenAppend), "49 8D 8F F0 02 00 00 E8"), "the destroy list's append", &bad);
    Check(mem::CodeMatches(A(kWorldSite), "48 8B 0D") && mem::RipTarget(A(kWorldSite), 3, 7) == A(kWorldGlobal), "the child loop's world global", &bad);
    // op 13's own check (not needed for Ready): the world vtable's +0x20 is the removal the child loop calls
    g_world_ok = mem::Q(A(kWorldVt) + 0x20) == A(kWorldRemove);
    if (bad) {
        std::snprintf(g_state, sizeof(g_state), "off (byte checks: %s)", bad);
        return false;
    }
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        std::snprintf(g_state, sizeof(g_state), "off (MinHook %s)", MH_StatusToString(init));
        return false;
    }
    MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(A(kRenVa)), reinterpret_cast<void*>(&H_Ren), reinterpret_cast<void**>(&o_ren));
    if (s == MH_OK) s = MH_EnableHook(reinterpret_cast<void*>(A(kRenVa)));
    if (s != MH_OK) {
        std::snprintf(g_state, sizeof(g_state), "off (MinHook %s)", MH_StatusToString(s));
        return false;
    }
    std::snprintf(g_state, sizeof(g_state), "on");
    g_ready.store(true, std::memory_order_release);
    return true;
}
bool Ready() noexcept { return g_ready.load(std::memory_order_acquire); }
const char* State() noexcept { return g_state; }
void Counts(unsigned* ok, unsigned* total) noexcept {
    if (ok) *ok = g_ok;
    if (total) *total = g_total;
}
void Watch(int slot, std::uintptr_t e) noexcept {
    if (slot < 0 || slot > 1) return;
    if (g_watch[slot].load(std::memory_order_relaxed) == e) return;
    g_removed[slot].store(0, std::memory_order_seq_cst);
    g_watch[slot].store(e, std::memory_order_seq_cst);  // (review) seq_cst (an xchg): Match's store-load pair
}
bool Removed(int slot, std::uintptr_t e) noexcept {
    return slot >= 0 && slot <= 1 && e && g_removed[slot].load(std::memory_order_seq_cst) == e;
}
Stats Snapshot() noexcept {
    Stats s;
    s.calls = g_calls.load(std::memory_order_relaxed);
    s.matched = g_matched.load(std::memory_order_relaxed);
    s.last_tid = g_tid.load(std::memory_order_relaxed);
    s.last_sim = g_sim.load(std::memory_order_relaxed);
    return s;
}
void Simulate(std::uintptr_t e) noexcept { Match(e, true, kWatchStim); }
std::uintptr_t WorldGlobal() noexcept {
    if (!g_ready.load() || !g_world_ok) return 0;
    const std::uintptr_t w = mem::Q(A(kWorldGlobal));
    return w && mem::Q(w) == A(kWorldVt) ? w : 0;
}
int TestRemove(std::uintptr_t e) noexcept {
    const std::uintptr_t w = WorldGlobal();
    if (!w || !e) return 0;
    const std::uintptr_t fn = mem::Q(mem::Q(w) + 0x20);
    if (fn != A(kWorldRemove)) return 0;
    return SehCall4(fn, w, e);
}

}  // namespace st::rmhook
