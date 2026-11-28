// Round 30 BT S2: the gazer update hook (see bt_world.hpp). The stage-0 probe detour (bt_probe 9 / 10) moved here from bt_sync.cpp,
// so that only one hook on FUN_142995210 exists; bt_sync.cpp keeps the probe ops and the kill relay and calls in here.
#include "bt_world.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "MinHook.h"

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>

#include "bt_rules.hpp"
#include "log.hpp"
#include "partner_source.hpp"

namespace st::btworld {
namespace {

namespace mem = partner::mem;
namespace R = btrules;

constexpr std::uintptr_t kImage = 0x140000000ull;
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

// ---------------------------------------------------------------- ds.exe Director's Cut
constexpr std::uintptr_t kGzUpdate = 0x142995210, kGzVtSlot = 0x143DA5848;
constexpr std::uintptr_t kGazerGlobal = 0x147BEBEC0, kRegGlobal = 0x147BBB8C0;
constexpr std::uintptr_t kAllowedFn = 0x14299C1A0;
// FUN_14299C1A0's own globals (read before the call: a null one would fault inside it)
constexpr std::uintptr_t kAllowedG1 = 0x147BF6E88, kAllowedG2 = 0x147BE6318;
// the gazer actor
constexpr std::uintptr_t kActId = 0x08, kActState = 0x24, kActXf = 0x28, kActReason = 0xCC, kActKiller = 0xD0, kActHealth = 0x180;
constexpr std::uintptr_t kActFlags = 0x1B4, kActPrevHealth = 0x2B0, kActScene = 0x3A8;

struct Site {
    std::uintptr_t va;
    const char* sig;
    const char* name;
    unsigned bits;
};
// clang-format off
constexpr Site kSites[] = {
    {kGzUpdate, "41 56 48 81 EC C0 01 00 00 C5 F8 29 BC 24 80 01 00 00", "the gazer update FUN_142995210", kBitHook | kBitKill},
    {0x1429957C8, "41 C7 86 CC 00 00 00 87 21 32 01 41 8B 46 08 41 89 86 D0 00 00 00 33 C0 41 89 86 80 01 00 00 41 83 8E B4 01 00 00 08",
     "the update's own kill (+0xCC / +0xD0 / +0x180 / +0x1B4)", kBitKill},
    {0x142995D04, "C4 41 78 2F 86 B0 02 00 00 48 8D 35 ?? ?? ?? ?? 0F 83 ?? ?? ?? ?? C4 41 78 2F 86 80 01 00 00",
     "the update's death edge (+0x2B0 vs +0x180)", kBitKill},
    {0x142996A45, "49 8B D4 49 8D 8E 18 02 00 00 E8", "the update's copy +0xE8 -> +0x218 (FUN_14299A3D0)", kBitKill},
    {kAllowedFn, "40 53 48 83 EC 20 48 8B 05 DB AC 25 05 48 8B 48 08 48 B8 4B D6 E9 2F 64 01 18 21 48 85 C8 0F 85 82 00 00 00 48 B8 32 19 44 "
                 "41 00 42 10 00 48 85 C8 75 73 48 8B 05 3E A1 24 05 48 8B 1D DF C1 25 05 83 B8 4C 04 00 00 00 75 5C",
     "BT gameplay allowed FUN_14299C1A0", kBitAllowed},
    {0x1429961B2, "48 8B 0D 07 5D 25 05 E8 E2 5F 00 00 84 C0 0F 84 4F 07 00 00", "the update's call to FUN_14299C1A0", kBitAllowed},
    {0x142995FC9, "48 8B 0D F0 58 22 05 48 8D 94 24 D0 00 00 00 48 8B 01 FF 50 40 41 8B 5E 08",
     "the update's local player id (registry vt+0x40)", kBitPlayer},
    {0x142995BF9, "49 8B 86 A8 03 00 00 48 85 C0 74 09 80 B8 E8 02 00 00 01", "the update's +0x3A8 (the locator's scene)", kBitScene},
};
// clang-format on
struct Rip {
    std::uintptr_t insn;
    unsigned off, len;
    std::uintptr_t want;
    const char* what;
    unsigned bits;
};
constexpr Rip kRips[] = {
    {0x142996A4F, 1, 5, 0x14299A3D0, "the update's call to FUN_14299A3D0", kBitKill},
    {0x1429961B2, 3, 7, kGazerGlobal, "the update's gazer manager global (FUN_14299C1A0's rcx)", kBitAllowed},
    {0x1429961B9, 1, 5, kAllowedFn, "the update's call to FUN_14299C1A0", kBitAllowed},
    {0x14299C1A6, 3, 7, kAllowedG1, "FUN_14299C1A0's first global", kBitAllowed},
    {0x14299C1D3, 3, 7, kAllowedG2, "FUN_14299C1A0's second global", kBitAllowed},
    {0x142995FC9, 3, 7, kRegGlobal, "the update's actor registry global", kBitPlayer},
};

bool g_checked = false;
unsigned g_bits = 0;

// ---------------------------------------------------------------- the detour's state (any thread: atomics only)
using GzFn = void (*)(void*, float, std::uint64_t, std::uint64_t);
GzFn g_orig = nullptr;
bool g_hook = false;                 // game thread
std::atomic<bool> g_pass{false};     // EmergencyStop: straight through
std::atomic<unsigned long> g_tick_tid{0};
std::atomic<unsigned> g_tick_calls{0}, g_other_calls{0}, g_tid_over{0};
constexpr unsigned kTids = 8;
std::atomic<unsigned long> g_tids[kTids];  // static: zero
constexpr unsigned kSlots = 8;
std::atomic<std::uintptr_t> g_slot_actor[kSlots];  // 0 free
std::atomic<std::uint8_t> g_slot_tag[kSlots];
std::atomic<std::uint32_t> g_slot_id[kSlots];  // review (crash 3): the gazer's id (+0x08) when it was armed

// The event ring: a bounded MPMC queue (Vyukov) of 32; the game thread is the only consumer.
constexpr unsigned kRing = 32;
struct Cell {
    std::atomic<unsigned> seq;
    Event e;
};
Cell g_ring[kRing];
std::atomic<unsigned> g_enq{0};
unsigned g_deq = 0;  // the consumer (game thread)
std::atomic<unsigned> g_lost{0};
bool g_ring_init = false;
bool g_player_dead = false, g_allowed_dead = false;  // a faulted call: off for the run

void RingInit() {
    if (g_ring_init) return;
    for (unsigned i = 0; i < kRing; ++i) g_ring[i].seq.store(i, std::memory_order_relaxed);
    g_enq.store(0, std::memory_order_relaxed);
    g_deq = 0;
    g_ring_init = true;
}
void Push(const Event& e) noexcept {
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
            g_lost.fetch_add(1, std::memory_order_relaxed);  // full
            return;
        } else {
            pos = g_enq.load(std::memory_order_relaxed);
        }
    }
}
bool Pop(Event* out) noexcept {
    Cell& c = g_ring[g_deq % kRing];
    const unsigned seq = c.seq.load(std::memory_order_acquire);
    if (static_cast<int>(seq - (g_deq + 1)) != 0) return false;
    *out = c.e;
    c.seq.store(g_deq + kRing, std::memory_order_release);
    ++g_deq;
    return true;
}

void NoteTid(unsigned long tid) noexcept {
    for (unsigned i = 0; i < kTids; ++i) {
        unsigned long v = g_tids[i].load(std::memory_order_relaxed);
        if (v == tid) return;
        if (v == 0) {
            if (g_tids[i].compare_exchange_strong(v, tid, std::memory_order_relaxed)) return;
            if (v == tid) return;
        }
    }
    g_tid_over.fetch_add(1, std::memory_order_relaxed);
}

// The update's own kill (0x1429957C8), guarded by KillGuard. KillGuard's result, or kApplyFault.
int KillNow(std::uintptr_t a) noexcept {
    __try {
        const int g = R::KillGuard(*reinterpret_cast<const std::int32_t*>(a + kActState), *reinterpret_cast<const float*>(a + kActHealth),
                                   *reinterpret_cast<const float*>(a + kActPrevHealth), *reinterpret_cast<const std::uint32_t*>(a + kActFlags));
        if (g != R::kKillOk) return g;
        *reinterpret_cast<std::uint32_t*>(a + kActReason) = R::kKillRainGone;
        *reinterpret_cast<std::uint32_t*>(a + kActKiller) = *reinterpret_cast<const std::uint32_t*>(a + kActId);
        *reinterpret_cast<float*>(a + kActHealth) = 0.0f;
        *reinterpret_cast<std::uint32_t*>(a + kActFlags) |= 8u;
        return R::kKillOk;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kApplyFault;
    }
}
bool ReadU32(std::uintptr_t a, std::uint32_t* out) noexcept {
    __try {
        *out = *reinterpret_cast<const std::uint32_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
float ReadF(std::uintptr_t a) noexcept {
    __try {
        return *reinterpret_cast<const float*>(a);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return std::numeric_limits<float>::quiet_NaN();
    }
}
bool ReadAfter(std::uintptr_t a, Event* e) noexcept {
    __try {
        e->id = *reinterpret_cast<const std::uint32_t*>(a + kActId);
        e->reason = *reinterpret_cast<const std::uint32_t*>(a + kActReason);
        e->killer = *reinterpret_cast<const std::uint32_t*>(a + kActKiller);
        e->flags = *reinterpret_cast<const std::uint32_t*>(a + kActFlags);
        e->prev_after = *reinterpret_cast<const float*>(a + kActPrevHealth);
        const double* p = reinterpret_cast<const double*>(a + kActXf);
        e->pos[0] = p[0], e->pos[1] = p[1], e->pos[2] = p[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void GzDetour(void* self, float dt, std::uint64_t r8, std::uint64_t r9) {
    const std::uintptr_t me = reinterpret_cast<std::uintptr_t>(self);
    if (g_pass.load(std::memory_order_relaxed) || !me) {
        g_orig(self, dt, r8, r9);
        return;
    }
    const unsigned long tid = GetCurrentThreadId();
    (tid == g_tick_tid.load(std::memory_order_relaxed) ? g_tick_calls : g_other_calls).fetch_add(1, std::memory_order_relaxed);
    NoteTid(tid);
    // apply: a kill armed for this actor, before the original (so the update's own death edge processes it)
    int applied = kNotArmed;
    std::uint8_t tag = 0;
    for (unsigned i = 0; i < kSlots; ++i) {
        std::uintptr_t want = me;
        if (g_slot_actor[i].load(std::memory_order_acquire) == me &&
            g_slot_actor[i].compare_exchange_strong(want, 0, std::memory_order_acq_rel)) {
            tag = g_slot_tag[i].load(std::memory_order_relaxed);
            // review (crash 3): the same gazer as armed (its actor freed and another made at that address: not killed)
            std::uint32_t id = 0;
            if (!ReadU32(me + kActId, &id)) applied = kApplyFault;
            else if (id != g_slot_id[i].load(std::memory_order_relaxed)) applied = kApplyStale;
            else applied = KillNow(me);
            break;
        }
    }
    const float prev_before = ReadF(me + kActPrevHealth);
    g_orig(self, dt, r8, r9);
    // detect: the death edge across this update (+0x2B0 above 0 before it, +0x180 not above 0 after it)
    const float health_after = ReadF(me + kActHealth);
    const bool edge = prev_before > 0 && !(health_after > 0) && !std::isnan(health_after);
    if (!edge && applied == kNotArmed) return;
    Event e;
    e.actor = me;
    e.edge = edge;
    e.tag = tag;
    e.applied = applied;
    e.read_ok = ReadAfter(me, &e);
    e.pos_ok = e.read_ok && R::PosSane(e.pos);
    Push(e);
}

void Put(char* msg, std::size_t cap, const char* fmt, ...) {
    if (!msg || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, cap, fmt, ap);
    va_end(ap);
}

int CallAllowed(std::uintptr_t fn, std::uintptr_t mgr) noexcept {
    __try {
        return reinterpret_cast<std::uint8_t (*)(void*)>(fn)(reinterpret_cast<void*>(mgr)) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
using FnPlayerId = const std::uint32_t* (*)(std::uintptr_t reg, void* out);
const std::uint32_t* CallPlayerId(std::uintptr_t fn, std::uintptr_t reg, void* buf, bool* fault) noexcept {
    __try {
        *fault = false;
        return reinterpret_cast<FnPlayerId>(fn)(reg, buf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = true;
        return nullptr;
    }
}

}  // namespace

unsigned Check() noexcept {
    if (g_checked) return g_bits;
    g_checked = true;
    if (!mem::Base()) {
        Log("[bt] world byte checks: ds.exe base unknown (the gazer update hook and the kill apply are off)");
        return g_bits = 0;
    }
    unsigned bits = kBitHook | kBitKill | kBitAllowed | kBitPlayer | kBitScene;
    int ok = 0, total = 0;
    for (const Site& s : kSites) {
        ++total;
        if (mem::CodeMatches(A(s.va), s.sig)) {
            ++ok;
            continue;
        }
        Log("[bt] world byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
        bits &= ~s.bits;
    }
    for (const Rip& r : kRips) {
        ++total;
        if (mem::RipTarget(A(r.insn), r.off, r.len) == A(r.want)) {
            ++ok;
            continue;
        }
        Log("[bt] world byte check FAILED: %s", r.what);
        bits &= ~r.bits;
    }
    ++total;  // the update is the actor's vtable slot (the only way it is reached)
    if (mem::Q(A(kGzVtSlot)) == A(kGzUpdate)) ++ok;
    else Log("[bt] world byte check FAILED: the gazer vtable slot 0x143DA5848"), bits &= ~(kBitHook | kBitKill);
    if (!(bits & kBitHook)) bits &= ~kBitKill;
    g_bits = bits;
    Log("[bt] world byte checks: %d/%d ok; hook %s, kill %s, bt-allowed %s, player id %s, scene owner %s", ok, total, (bits & kBitHook) ? "ok" : "OFF",
        (bits & kBitKill) ? "ok" : "OFF", (bits & kBitAllowed) ? "ok" : "OFF", (bits & kBitPlayer) ? "ok" : "OFF", (bits & kBitScene) ? "ok" : "OFF");
    return bits;
}

bool InstallHook(const char* why, char* msg, std::size_t cap) noexcept {
    if (g_hook) return true;
    if (!(Check() & kBitHook)) return Put(msg, cap, "bt: the gazer update byte checks failed (log)"), false;
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return Put(msg, cap, "bt: MinHook %s", MH_StatusToString(init)), false;
    RingInit();
    g_tick_tid.store(GetCurrentThreadId());
    void* target = reinterpret_cast<void*>(A(kGzUpdate));
    MH_STATUS s = MH_CreateHook(target, reinterpret_cast<void*>(&GzDetour), reinterpret_cast<void**>(&g_orig));
    if (s == MH_OK) s = MH_EnableHook(target);
    g_hook = s == MH_OK;
    Log("[bt] gazer update hook %s (FUN_142995210, for %s): kills armed for a gazer applied before its update, death edges read after it",
        g_hook ? "in" : MH_StatusToString(s), why ? why : "?");
    if (!g_hook) Put(msg, cap, "bt: the gazer update hook failed: %s", MH_StatusToString(s));
    return g_hook;
}

bool HookIn() noexcept { return g_hook; }

bool ArmKill(std::uintptr_t actor, std::uint32_t id, std::uint8_t tag) noexcept {
    if (!g_hook || !actor || g_pass.load() || !(Check() & kBitKill)) return false;
    for (unsigned i = 0; i < kSlots; ++i)
        if (g_slot_actor[i].load() == actor) return false;
    for (unsigned i = 0; i < kSlots; ++i) {
        if (g_slot_actor[i].load() != 0) continue;
        g_slot_tag[i].store(tag, std::memory_order_relaxed);
        g_slot_id[i].store(id, std::memory_order_relaxed);
        std::uintptr_t free_slot = 0;
        if (g_slot_actor[i].compare_exchange_strong(free_slot, actor, std::memory_order_release)) return true;
    }
    return false;
}

bool Disarm(std::uintptr_t actor) noexcept {
    if (!actor) return false;
    for (unsigned i = 0; i < kSlots; ++i) {
        std::uintptr_t want = actor;
        if (g_slot_actor[i].compare_exchange_strong(want, 0)) return true;
    }
    return false;
}

bool Armed(std::uintptr_t actor) noexcept {
    if (!actor) return false;
    for (unsigned i = 0; i < kSlots; ++i)
        if (g_slot_actor[i].load() == actor) return true;
    return false;
}

unsigned TakeEvents(Event* out, unsigned cap, unsigned* lost) noexcept {
    if (lost) *lost = g_lost.exchange(0);
    if (!g_ring_init || !out) return 0;
    unsigned n = 0;
    while (n < cap && Pop(&out[n])) ++n;
    return n;
}

int BtAllowed() noexcept {
    if (g_allowed_dead || !(Check() & kBitAllowed)) return -1;
    const std::uintptr_t g1 = mem::Q(A(kAllowedG1)), g2 = mem::Q(A(kAllowedG2));
    std::uint64_t probe = 0;
    std::uint32_t probe2 = 0;
    if (!g1 || !g2 || !mem::Rd(g1 + 8, probe) || !mem::Rd(g2 + 0x44C, probe2)) return -1;
    const int r = CallAllowed(A(kAllowedFn), mem::Q(A(kGazerGlobal)));
    if (r < 0) {
        g_allowed_dead = true;
        Log("[bt] FUN_14299C1A0 (BT gameplay allowed) FAULTED: not called again this run (partner kills are held)");
    }
    return r;
}

bool PlayerId(std::uint32_t* out) noexcept {
    if (!out || g_player_dead || !(Check() & kBitPlayer)) return false;
    const std::uintptr_t reg = mem::Q(A(kRegGlobal));
    const std::uintptr_t fn = reg ? mem::Q(mem::Q(reg) + 0x40) : 0;
    if (!fn || !mem::InText(fn)) return false;
    alignas(16) std::uint8_t buf[64] = {};
    bool fault = false;
    const std::uint32_t* p = CallPlayerId(fn, reg, buf, &fault);
    if (fault) {
        g_player_dead = true;
        Log("[bt] the registry's player id call FAULTED: not called again this run (only stats-reason kills are relayed)");
        return false;
    }
    return p && mem::Rd(reinterpret_cast<std::uintptr_t>(p), *out);
}

bool SceneOwner(std::uintptr_t actor, std::uintptr_t* out) noexcept {
    if (!out || !actor || !(Check() & kBitScene)) return false;
    return mem::Rd(actor + kActScene, *out);
}

void NoteTickThread() noexcept {
    if (g_hook) g_tick_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
}

void Threads(unsigned* tick, unsigned* other, unsigned* threads, bool* over) noexcept {
    unsigned k = 0;
    for (unsigned i = 0; i < kTids; ++i) k += g_tids[i].load() ? 1u : 0u;
    if (tick) *tick = g_tick_calls.load();
    if (other) *other = g_other_calls.load();
    if (threads) *threads = k;
    if (over) *over = g_tid_over.load() != 0;
}

void EmergencyStop() noexcept {
    g_pass.store(true);
    for (unsigned i = 0; i < kSlots; ++i) g_slot_actor[i].store(0);
}

}  // namespace st::btworld
