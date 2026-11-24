// The shared detour on our Sam's MsgDamage handler (sam_damage.hpp).
#include "sam_damage.hpp"

#include <windows.h>

#include "MinHook.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "log.hpp"
#include "partner_source.hpp"

namespace st::samdmg {
namespace {
namespace mem = partner::mem;
constexpr std::uintptr_t kImage = 0x140000000ull;
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

constexpr std::uintptr_t kSamDamage = 0x14271BC00;  // DSPlayerEntity's MsgDamage handler
constexpr const char* kSamDamageSig = "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8B DA 48 8B 89 20 05 00 00";

using FnDamage = void (*)(std::uintptr_t, std::uintptr_t);
FnDamage o_damage = nullptr;
bool g_tried = false, g_in = false;
char g_off[160] = "not installed";
Observer g_obs[4] = {};
std::atomic<unsigned> g_nobs{0};
Filter g_flt[4] = {};
const char* g_flt_name[4] = {};
std::atomic<unsigned> g_nflt{0};
std::atomic<unsigned> g_seen{0}, g_faults{0};
std::atomic<unsigned> g_dropped[4] = {};

bool RunObserver(Observer o, std::uintptr_t sam, std::uintptr_t msg) noexcept {
    __try {
        o(sam, msg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool RunFilter(Filter f, const View& v, Verdict* out) noexcept {
    __try {
        *out = f(v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void H_Damage(std::uintptr_t sam, std::uintptr_t msg) {
    g_seen.fetch_add(1, std::memory_order_relaxed);
    const unsigned no = g_nobs.load(std::memory_order_acquire);
    for (unsigned i = 0; i < no && i < 4; ++i)
        if (!RunObserver(g_obs[i], sam, msg)) g_faults.fetch_add(1, std::memory_order_relaxed);
    const unsigned nf = g_nflt.load(std::memory_order_acquire);
    if (nf) {
        const View v = Read(sam, msg);
        for (unsigned i = 0; i < nf && i < 4; ++i) {
            Verdict d = Verdict::keep;
            if (!RunFilter(g_flt[i], v, &d)) {
                g_faults.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (d == Verdict::drop) {
                g_dropped[i].fetch_add(1, std::memory_order_relaxed);
                // what the handler itself does with a message its damage component refuses
                *reinterpret_cast<volatile std::uint8_t*>(msg + 8) = 1;
                return;
            }
        }
    }
    o_damage(sam, msg);
}
}  // namespace

View Read(std::uintptr_t sam, std::uintptr_t msg) noexcept {
    View v;
    v.sam = sam, v.msg = msg;
    if (!msg) return v;
    v.di = msg + 0x20;
    v.ref = mem::Q(v.di + 0x28);
    v.ctx = mem::Q(v.di + 0x40);
    if (v.ctx) {
        const std::uintptr_t r = mem::Q(v.ctx + 0x68);  // 0x14245EAF0: [ctx + 0x68] - 0x20
        v.attacker = r ? r - 0x20 : 0;
    }
    float a = 0;
    if (mem::Rd(v.di + 0x78, a) && std::isfinite(a)) v.amount = a;
    double h[3] = {};
    if (mem::Read(v.di + 0x80, h, sizeof(h)) && std::isfinite(h[0]) && std::isfinite(h[1]) && std::isfinite(h[2])) std::memcpy(v.hit, h, sizeof(h));
    return v;
}

bool Ensure(const char* who) noexcept {
    if (g_tried) return g_in;
    g_tried = true;
    if (!mem::CodeMatches(A(kSamDamage), kSamDamageSig)) {
        std::snprintf(g_off, sizeof(g_off), "byte check FAILED: Sam's MsgDamage handler at 0x%llX", static_cast<unsigned long long>(kSamDamage));
        Log("[samdmg] off: %s (asked by %s)", g_off, who ? who : "?");
        return false;
    }
    void* t = reinterpret_cast<void*>(A(kSamDamage));
    MH_STATUS s = MH_CreateHook(t, reinterpret_cast<void*>(&H_Damage), reinterpret_cast<void**>(&o_damage));
    if (s == MH_OK) s = MH_EnableHook(t);
    if (s != MH_OK) {
        std::snprintf(g_off, sizeof(g_off), "not hooked (%s)", MH_StatusToString(s));
        Log("[samdmg] off: %s (asked by %s)", g_off, who ? who : "?");
        return false;
    }
    g_in = true;
    g_off[0] = 0;
    Log("[samdmg] Sam's MsgDamage handler hooked (asked first by %s; 1/1 byte check ok)", who ? who : "?");
    return true;
}

bool Installed() noexcept { return g_in; }
const char* OffReason() noexcept { return g_in ? "" : g_off; }

bool Observe(Observer o) noexcept {
    if (!g_in || !o) return false;
    const unsigned n = g_nobs.load();
    if (n >= 4) return false;
    g_obs[n] = o;
    g_nobs.store(n + 1, std::memory_order_release);
    return true;
}

bool Register(Filter f, const char* name) noexcept {
    if (!g_in || !f) return false;
    const unsigned n = g_nflt.load();
    if (n >= 4) return false;
    g_flt[n] = f;
    g_flt_name[n] = name;
    g_nflt.store(n + 1, std::memory_order_release);
    Log("[samdmg] filter %u: %s", n, name ? name : "?");
    return true;
}

void Counts(unsigned* seen, unsigned (&dropped)[4], unsigned* faults) noexcept {
    if (seen) *seen = g_seen.load();
    for (int i = 0; i < 4; ++i) dropped[i] = g_dropped[i].load();
    if (faults) *faults = g_faults.load();
}

}  // namespace st::samdmg
