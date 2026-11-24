// The shared detour on the Humanoid MsgDamage handler (npc_damage.hpp).
#include "npc_damage.hpp"

#include <windows.h>

#include "MinHook.h"

#include <atomic>
#include <cstdio>

#include "log.hpp"
#include "partner_source.hpp"

namespace st::npcdmg {
namespace {
namespace mem = partner::mem;
constexpr std::uintptr_t kImage = 0x140000000ull;
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

constexpr std::uintptr_t kHumanoidDmg = 0x1437393D0;  // Humanoid MsgDamage handler (MULEs, Demens, warriors)
constexpr const char* kHumanoidSig = "40 55 57 41 54 41 56 48 81 EC 68 01 00 00 C5 F8 29 B4 24 50 01 00 00 48";

using FnDamage = void (*)(std::uintptr_t, std::uintptr_t);
FnDamage o_damage = nullptr;
bool g_tried = false, g_in = false;
char g_off[160] = "not installed";
Filter g_flt[4] = {};
const char* g_flt_name[4] = {};
std::atomic<unsigned> g_nflt{0};
std::atomic<unsigned> g_seen{0}, g_faults{0};
std::atomic<unsigned> g_dropped[4] = {};

bool RunFilter(Filter f, std::uintptr_t e, std::uintptr_t msg, const samdmg::View& v, Verdict* out) noexcept {
    __try {
        *out = f(e, msg, v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool ReadView(std::uintptr_t e, std::uintptr_t msg, samdmg::View* v) noexcept {
    __try {
        *v = samdmg::Read(e, msg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void H_Damage(std::uintptr_t e, std::uintptr_t msg) {
    g_seen.fetch_add(1, std::memory_order_relaxed);
    const unsigned nf = g_nflt.load(std::memory_order_acquire);
    samdmg::View v;
    if (nf && msg && ReadView(e, msg, &v)) {
        for (unsigned i = 0; i < nf && i < 4; ++i) {
            Verdict d = Verdict::keep;
            if (!RunFilter(g_flt[i], e, msg, v, &d)) {
                g_faults.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (d == Verdict::drop) {
                g_dropped[i].fetch_add(1, std::memory_order_relaxed);
                *reinterpret_cast<volatile std::uint8_t*>(msg + 8) = 1;  // as the handler does with a message it refuses
                return;
            }
        }
    }
    o_damage(e, msg);
}
}  // namespace

bool Ensure(const char* who) noexcept {
    if (g_tried) return g_in;
    g_tried = true;
    if (!mem::CodeMatches(A(kHumanoidDmg), kHumanoidSig)) {
        std::snprintf(g_off, sizeof(g_off), "byte check FAILED: the Humanoid MsgDamage handler at 0x%llX", static_cast<unsigned long long>(kHumanoidDmg));
        Log("[npcdmg] off: %s (asked by %s)", g_off, who ? who : "?");
        return false;
    }
    void* t = reinterpret_cast<void*>(A(kHumanoidDmg));
    MH_STATUS s = MH_CreateHook(t, reinterpret_cast<void*>(&H_Damage), reinterpret_cast<void**>(&o_damage));
    if (s == MH_OK) s = MH_EnableHook(t);
    if (s != MH_OK) {
        std::snprintf(g_off, sizeof(g_off), "not hooked (%s)", MH_StatusToString(s));
        Log("[npcdmg] off: %s (asked by %s)", g_off, who ? who : "?");
        return false;
    }
    g_in = true;
    g_off[0] = 0;
    Log("[npcdmg] the Humanoid MsgDamage handler hooked (asked first by %s; 1/1 byte check ok)", who ? who : "?");
    return true;
}

bool Installed() noexcept { return g_in; }
const char* OffReason() noexcept { return g_in ? "" : g_off; }

bool Register(Filter f, const char* name) noexcept {
    if (!g_in || !f) return false;
    const unsigned n = g_nflt.load();
    if (n >= 4) return false;
    g_flt[n] = f;
    g_flt_name[n] = name;
    g_nflt.store(n + 1, std::memory_order_release);
    Log("[npcdmg] filter %u: %s", n, name ? name : "?");
    return true;
}

void Counts(unsigned* seen, unsigned (&dropped)[4], unsigned* faults) noexcept {
    if (seen) *seen = g_seen.load();
    for (int i = 0; i < 4; ++i) dropped[i] = g_dropped[i].load();
    if (faults) *faults = g_faults.load();
}

}  // namespace st::npcdmg
