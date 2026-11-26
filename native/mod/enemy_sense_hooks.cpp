// The shared sense detours (enemy_sense_hooks.hpp).
#include "enemy_sense_hooks.hpp"

#include <windows.h>

#include "MinHook.h"

#include <atomic>

#include "enemy_l1_probe.hpp"
#include "enemy_stim_core.hpp"
#include "enemy_target.hpp"

namespace st::sense {
namespace {
using stimcore::A;

constexpr std::uintptr_t kClsVa = 0x142E5FA40, kAppVa = 0x142E59860, kPsrVa = 0x142E8DAF0;

using FnCls = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, int*, std::int64_t*, std::uint32_t);
using Fn4 = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
FnCls o_cls = nullptr;
Fn4 o_app = nullptr, o_psr = nullptr;
std::atomic<unsigned> g_in{0};
bool g_mh_tried = false, g_mh_ok = false;
unsigned g_tried = 0;
const char* g_text[3] = {"off (byte checks)", "off (byte checks)", "off (byte checks)"};

std::uintptr_t H_Cls(std::uintptr_t sense, std::uintptr_t stim, std::uintptr_t results, int* type_out, std::int64_t* ent_out, std::uint32_t own) {
    std::uintptr_t r = 0;
    if (!etarget::SenseClsRefuse(stim, own)) {
        r = o_cls(sense, stim, results, type_out, ent_out, own);
        r = etarget::SenseClsAfter(stim, own, results, r, type_out);
    }
    l1probe::SenseCls(stim, r, type_out);
    return r;
}
std::uintptr_t H_App(std::uintptr_t array, std::uintptr_t record, std::uintptr_t r8, std::uintptr_t r9) {
    l1probe::SenseApp(array, record);
    return o_app(array, record, r8, r9);
}
std::uintptr_t H_Psr(std::uintptr_t k, std::uintptr_t p2, std::uintptr_t r8, std::uintptr_t r9) {
    l1probe::SensePsr(k);
    etarget::SensePsr(k);
    return o_psr(k, p2, r8, r9);
}
}  // namespace

unsigned Ensure(unsigned want, unsigned ok, const char* (&text)[3]) noexcept {
    struct One {
        unsigned bit;
        std::uintptr_t va;
        void* det;
        void** orig;
    } const ones[3] = {{kCls, kClsVa, reinterpret_cast<void*>(&H_Cls), reinterpret_cast<void**>(&o_cls)},
                       {kApp, kAppVa, reinterpret_cast<void*>(&H_App), reinterpret_cast<void**>(&o_app)},
                       {kPsr, kPsrVa, reinterpret_cast<void*>(&H_Psr), reinterpret_cast<void**>(&o_psr)}};
    for (int i = 0; i < 3; ++i) {
        const unsigned bit = ones[i].bit;
        if (!(want & bit) || (g_in.load() & bit)) {
            text[i] = g_text[i];
            continue;
        }
        if (!(ok & bit)) {
            text[i] = g_text[i] = "off (byte checks)";
            continue;
        }
        if (g_tried & bit) {  // MinHook refused it once: not again
            text[i] = g_text[i];
            continue;
        }
        if (!g_mh_tried) {
            g_mh_tried = true;
            const MH_STATUS init = MH_Initialize();
            g_mh_ok = init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED;
        }
        if (!g_mh_ok) {
            text[i] = g_text[i] = "off (MinHook)";
            continue;
        }
        g_tried |= bit;
        MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(A(ones[i].va)), ones[i].det, ones[i].orig);
        if (s == MH_OK) s = MH_EnableHook(reinterpret_cast<void*>(A(ones[i].va)));
        if (s == MH_OK) g_in.fetch_or(bit), g_text[i] = "on";
        else g_text[i] = MH_StatusToString(s);
        text[i] = g_text[i];
    }
    return g_in.load();
}

unsigned Installed() noexcept { return g_in.load(); }

}  // namespace st::sense
