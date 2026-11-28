// Round 31 stage 6: VB: the partner's voidout blast (faded flash) and CR: the crater record (bt_void.hpp).
//
// Sender (game thread): every voidout of this game (bt_sync's FUN_14299B1B0 detour, OnLocalVoid) goes as VOID2 (13) to a partner
// whose hello offers kCapVoid2: the blast only (crater 0; PF 17). A pass-through detour on the catcher crater FUN_142A003D0(cm, id)
// (any thread: guarded reads and a 4-slot ring only) sees the pending crater record a catcher's annihilation adds (its generation
// cm+0x2A8 moved): the Tick sends it as a catcher VOID2 that wants a crater. bt_sync 9 sends a test VOID2 (hidden runs only).
//
// Receiver (game thread): VOID2 queues a blast (BlastGate: a real-save guest only from 300 m) and, when it wants one, a crater
// (CraterApplyGate, the gazer row refused for good: bt_void 2, the host or a sandboxed guest). The blast is bt_probe 29's sequence
// (FUN_14299B1B0's own, without its GameOver): the (far) annihilation resource's vt+0x80, FUN_14234F3E0(e, 0), the world's vt+0x18,
// a ref node of our own (FUN_14191C790 / FUN_14191C810, released at 16 s). While its 9 s window is open, a pass-through detour on
// the effector creator FUN_141CAD420 writes FadeStart bt_void_flash_m / FadeRange 450 into the voidout effector's resource (its
// GUID checked) and the window's close writes the saved values back (only while ours are still there). The crater is bt_probe
// 30's append into the catcher manager's pending list (cm+0x290, under cm+0xE8): the game makes the record live after 8.1 s and
// keeps it in the save it lands on. CR is DROPPED this round (bt_void_rules.hpp kCraterDropped: CraterGate refuses every crater):
// a guest's crater record was followed by a game-server write in the first gate batch.
//
// Every site is byte-checked once before first use (groups blast, fx, crater, catcher hook; a failed group turns off only what
// needs it, one FAILED line each); every game call is SEH-guarded on the game thread; reads and the fade writes go through
// bt_sync's ReadProcessMemory / WriteProcessMemory helpers. No game-server write.
#include "bt_void.hpp"

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

#include "automation.hpp"
#include "bt_rules.hpp"
#include "bt_sync_internal.hpp"
#include "bt_void_rules.hpp"
#include "config.hpp"
#include "enemy_sync.hpp"
#include "game_actions.hpp"
#include "link.hpp"
#include "log.hpp"
#include "ride_together.hpp"

namespace st::bt::voidout {
namespace {

namespace R = btrules;
namespace V = btrules::voidout;
namespace I = st::bt::internal;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using uptr = std::uintptr_t;

int g_key = 0;          // bt_void (Install)
int g_flash_m = 150;    // bt_void_flash_m
bool g_standin = false;  // bt_sync 9 <1|0> 4: this game acts as a real-save guest for bt_void's gates (game thread)

// ---------------------------------------------------------------- log (any thread; a token bucket, the overflow counted)
std::atomic_flag g_log_lock = ATOMIC_FLAG_INIT;
V::LogBucket g_bucket;
std::atomic<unsigned> g_log_dropped{0};
void L(const char* fmt, ...) {
    if (g_log_lock.test_and_set(std::memory_order_acquire)) {
        g_log_dropped.fetch_add(1);
        return;
    }
    const bool ok = g_bucket.Take(static_cast<double>(GetTickCount64()));
    g_log_lock.clear(std::memory_order_release);
    if (!ok) {
        g_log_dropped.fetch_add(1);
        return;
    }
    char buf[480];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log("[bt] void: %s", buf);
}
void Put(char* msg, std::size_t cap, const char* fmt, ...) {
    if (!msg || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, cap, fmt, ap);
    va_end(ap);
}
void FmtPos(char* b, std::size_t n, const double* p) { std::snprintf(b, n, "%.1f %.1f %.1f", p[0], p[1], p[2]); }
unsigned long long H(uptr p) { return static_cast<unsigned long long>(I::Va(p) ? I::Va(p) : p); }
uptr A(uptr va) { return I::Addr(va); }
u64 Q(uptr a) {
    u64 v = 0;
    return a && I::Read(a, v) ? v : 0;
}
u32 Bits(float f) {
    u32 b = 0;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}
float Fl(u32 b) {
    float f = 0;
    std::memcpy(&f, &b, sizeof(f));
    return f;
}

// ---------------------------------------------------------------- addresses (ds.exe DC; bt_probe_r31.cpp's, verified in stage 0)
constexpr uptr kVoidFn = 0x14299B1B0, kBlastInit = 0x14234F3E0, kWorldGlobal = 0x147BC4E70, kRefRelease = 0x14191C810, kRefAdd = 0x14191C790;
constexpr uptr kFxCreate = 0x141CAD420, kCatCrater = 0x142A003D0, kAreaFn = 0x14262A8C0, kAreaGlobal = 0x147BE6318, kGrowFn = 0x142A6A260;
constexpr uptr kIdentity = 0x144591D70;  // 9 floats: the identity rotation the voidout makes its blast with
constexpr uptr kMgrRes = 0x68, kResNear = 0x28, kResFar = 0x30, kResFarDist = 0x54;  // the gazer manager's voidout resource
constexpr uptr kFadeOff = 0x28, kGuidOff = 0x08;                                     // the effector resource: FadeStart / FadeRange, GUID
constexpr uptr kCmCount = 0x88, kCmData = 0x90, kCmLock = 0xE8, kCmCraters = 0x280, kCmPending = 0x290, kCmGen = 0x2A8, kCmDirty = 0x2AC;
constexpr std::size_t kCatRecSize = 0x110, kCraterSize = 0x60;
constexpr uptr kHmArea = 0x140;

// ---------------------------------------------------------------- the byte checks (game thread, once, before first use)
enum : unsigned { kGBlast = 1u, kGFx = 2u, kGCrater = 4u, kGCatHook = 8u };
struct Site {
    uptr va;
    const char* sig;
    const char* name;
    unsigned group;
};
struct Rip {
    uptr insn;
    unsigned off, len;
    uptr want;
    const char* what;
    unsigned group;
};
struct Data {
    uptr va;
    u32 bits;
    const char* what;
    unsigned group;
};
// clang-format off
constexpr Site kSites[] = {
    {0x14299B1F6, "48 8B 57 68", "the voidout's resource +0x68 (0x14299B1F6)", kGBlast},
    {0x14299B22D, "C5 FA 10 42 54", "the voidout's far distance +0x54 (0x14299B22D)", kGBlast},
    {0x14299B273, "48 8B 4A 30 EB 04 48 8B 4A 28", "the far / near blast resources +0x30 / +0x28 (0x14299B273)", kGBlast},
    {0x14299B29E, "4C 8B 80 80 00 00 00", "the blast create vt+0x80 (0x14299B29E)", kGBlast},
    {0x14299B2BE, "41 FF D0 33 D2 48 8B C8 4C 8B F0 E8", "the blast create call (0x14299B2BE)", kGBlast},
    {kBlastInit, "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24", "the blast init FUN_14234F3E0", kGBlast},
    {0x14299B2CE, "48 8B 0D 9B 9B 22 05", "the world global load (0x14299B2CE)", kGBlast},
    {0x14299B2E3, "4C 8B 11 41 FF 52 18", "the world add vt+0x18 (0x14299B2E3)", kGBlast},
    {kRefRelease, "40 57 48 83 EC 20 48 8B F9 48 8B 09 48 85 C9 0F 84 8D 00 00 00", "the ref release FUN_14191C810", kGBlast},
    {kRefAdd, "40 57 48 83 EC 20 48 8B F9 48 8B 09 48 85 C9 74 58", "the ref add FUN_14191C790", kGBlast},
    {kFxCreate, "48 89 5C 24 18 55 56 57 48 83 EC 20 48 8B F9 41 0F B6 F0 48 8D 0D A6 19 D5 02 48 8B EA E8 AE 3E D4 FF",
     "the effector creator FUN_141CAD420", kGFx},
    {0x141BB69D0, "E8 4B 6A 0F 00", "the effector creator's only call (0x141BB69D0)", kGFx},
    {0x142A003F8, "48 8D 99 E8 00 00 00", "the crater lock cm+0xE8 (0x142A003F8)", kGCrater},
    {0x142A004CB, "41 FF 86 A8 02 00 00", "the generation ++cm+0x2A8 (0x142A004CB)", kGCrater},
    {0x142A004D2, "C5 FA 10 05 66 20 3B 01", "the record radius 107 load (0x142A004D2)", kGCrater},
    {0x142A004ED, "C5 FA 10 0D 43 20 3B 01", "the record delay 8.1 load (0x142A004ED)", kGCrater},
    {kAreaFn, "48 8B 05 21 E8 4B 05 48 85 C0 74 0E 80 B8 A8 00 00 00 00 74 05 0F B7 40", "the area id FUN_14262A8C0", kGCrater},
    {0x142A00870, "49 8D B6 90 02 00 00 8B 3E 48 8B 4E 08 FF C7 3B 7E 04 7E 39", "the pending append cm+0x290 (0x142A00870)", kGCrater},
    {kGrowFn, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 49 63 D8", "the array grow FUN_142A6A260", kGCrater},
    {0x142A008BD, "48 63 16 4C 8B BC 24 30 02 00 00 48 8D 3C 52 48 C1 E7 05", "the 0x60 record slot (0x142A008BD)", kGCrater},
    {0x142A009CA, "41 C6 86 AC 02 00 00 01", "the dirty flag cm+0x2AC (0x142A009CA)", kGCrater},
    {0x142A74D90, "4C 8B DC 55 56 41 55 49 8D 6B A1 48 81 EC A0 00 00 00 48 8B 05 87 C2 4E", "the crater merge FUN_142A74D90", kGCrater},
    {0x142A9298D, "41 C6 82 AD 02 00 00 01", "cm+0x2AD (0x142A9298D)", kGCrater},
    {kCatCrater, "4C 8B DC 55 53 49 8D AB A8 FE FF FF 48 81 EC 48 02 00 00 48 8B 05 46 0C 56 02 48 33 C4 48 89 85",
     "the catcher crater FUN_142A003D0", kGCatHook},
    {0x142A57C3F, "E8 8C 87 FA FF", "the catcher crater's only call (0x142A57C3F)", kGCatHook},
    {0x142A1C11F, "4D 8B 8F 90 00 00 00 48 8B D0 41 8B CA 49 8D 41 20 44 39 30 74 16 41 FF C0 48 FF C1 48 05 10 01 00 00",
     "the catcher record key +0x20 (0x142A1C11F)", kGCatHook},
    {0x142A1C18C, "44 0F B6 66 28", "the catcher record class +0x28 (0x142A1C18C)", kGCatHook},
    {0x142A87950, "C4 C1 78 10 00 C5 F8 11 41 68 C4 C1 7B 10 48 10 C5 FB 11 49 78", "the catcher record position +0x68 (0x142A87950)", kGCatHook},
};
constexpr Rip kRips[] = {
    {0x14299B2C9, 1, 5, kBlastInit, "the voidout's call to FUN_14234F3E0", kGBlast},
    {0x14299B2CE, 3, 7, kWorldGlobal, "the world global 0x147BC4E70", kGBlast},
    {0x14299B2FB, 1, 5, kRefRelease, "the voidout's call to FUN_14191C810", kGBlast},
    {0x14299B306, 1, 5, kRefAdd, "the voidout's call to FUN_14191C790", kGBlast},
    {0x14299B296, 4, 8, kIdentity, "the identity rotation 0x144591D70", kGBlast},
    {0x141BB69D0, 1, 5, kFxCreate, "the call to FUN_141CAD420", kGFx},
    {0x142A004D2, 4, 8, 0x143DB2540, "the record radius 0x143DB2540", kGCrater},
    {0x142A004ED, 4, 8, 0x143DB2538, "the record delay 0x143DB2538", kGCrater},
    {0x142A004E1, 3, 7, kAreaGlobal, "the area global 0x147BE6318", kGCrater},
    {0x142A00507, 1, 5, kAreaFn, "the call to FUN_14262A8C0", kGCrater},
    {0x142A0089F, 1, 5, kGrowFn, "the call to FUN_142A6A260", kGCrater},
    {0x142A57C3F, 1, 5, kCatCrater, "the call to FUN_142A003D0", kGCatHook},
};
constexpr Data kData[] = {
    {0x143DB2540, 0x42D60000u, "107.0f", kGCrater},
    {0x143DB2538, 0x4101999Au, "8.1f", kGCrater},
};
// clang-format on
constexpr unsigned kAllGroups = kGBlast | kGFx | kGCrater | kGCatHook;
const char* GroupName(unsigned g) {
    switch (g) {
        case kGBlast: return "blast";
        case kGFx: return "fx";
        case kGCrater: return "crater";
        case kGCatHook: return "catcher hook";
    }
    return "?";
}
const char* GroupEffect(unsigned g) {
    switch (g) {
        case kGBlast: return "the partner blast off";
        case kGFx: return "the partner blast off (no faded flash)";
        case kGCrater: return "the crater record off (no add, no catcher crater sent)";
        case kGCatHook: return "no catcher crater sent";
    }
    return "?";
}
bool g_checked = false;
unsigned g_ok_groups = 0;
unsigned Check() noexcept {
    if (g_checked) return g_ok_groups;
    g_checked = true;
    unsigned bad = 0;
    char first[kAllGroups + 1][128] = {};
    auto fail = [&](unsigned g, const char* what) {
        if (!(bad & g)) std::snprintf(first[g], sizeof(first[g]), "%s", what);
        bad |= g;
    };
    int ok = 0, total = 1;
    if (I::VoidPrologueOk()) ++ok;
    else fail(kGBlast, "the voidout FUN_14299B1B0's prologue");
    for (const Site& s : kSites) {
        ++total;
        if (I::SiteOk(s.va, s.sig)) ++ok;
        else fail(s.group, s.name);
    }
    for (const Rip& r : kRips) {
        ++total;
        if (I::RipOk(r.insn, r.off, r.len, r.want)) ++ok;
        else fail(r.group, r.what);
    }
    for (const Data& d : kData) {
        ++total;
        u32 v = 0;
        if (I::Read(A(d.va), v) && v == d.bits) ++ok;
        else fail(d.group, d.what);
    }
    {
        ++total;
        float rot[9] = {};
        const float want[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        if (I::ReadN(A(kIdentity), rot, sizeof(rot)) && std::memcmp(rot, want, sizeof(rot)) == 0) ++ok;
        else fail(kGBlast, "the identity rotation's 9 floats (0x144591D70)");
    }
    for (unsigned g : {kGBlast, kGFx, kGCrater, kGCatHook})
        if (bad & g) L("byte check FAILED: %s: %s (%s)", GroupName(g), first[g], GroupEffect(g));
    g_ok_groups = kAllGroups & ~bad;
    L("byte checks %d/%d ok: blast %s, fx %s, crater %s, catcher hook %s", ok, total, (g_ok_groups & kGBlast) ? "ok" : "OFF",
      (g_ok_groups & kGFx) ? "ok" : "OFF", (g_ok_groups & kGCrater) ? "ok" : "OFF", (g_ok_groups & kGCatHook) ? "ok" : "OFF");
    return g_ok_groups;
}

// ---------------------------------------------------------------- the detours (any thread: atomics and guarded reads only)
using Fn4 = u64 (*)(u64, u64, u64, u64);
Fn4 g_fx_orig = nullptr, g_cat_orig = nullptr;
std::atomic<bool> g_pass{false};  // EmergencyStop: straight through
std::atomic<unsigned long> g_tick_tid{0};

// The fade window. g_fade: 0 open (nothing written), 1 the detour writing, 2 written (the saved values valid), 3 restoring,
// 4 closed. Only the detour moves 0 -> 1 -> 2 (or back to 0 when its write failed); only the close moves 0 / 2 -> 4.
std::atomic<bool> g_win{false};
std::atomic<int> g_fade{4};
std::atomic<u32> g_want_start{0}, g_want_range{0}, g_save_start{0}, g_save_range{0};
std::atomic<uptr> g_fade_res{0};
std::atomic<unsigned> g_fx_calls{0}, g_fx_match{0}, g_fx_fail{0};

constexpr u8 kFxGuidA[16] = {0x1d, 0x2c, 0x43, 0x89, 0xa4, 0x92, 0xde, 0x42, 0x85, 0xf4, 0xf4, 0x39, 0x97, 0xbf, 0x6e, 0xfb};
constexpr u8 kFxGuidB[16] = {0x89, 0x43, 0x2c, 0x1d, 0x92, 0xa4, 0x42, 0xde, 0x85, 0xf4, 0xf4, 0x39, 0x97, 0xbf, 0x6e, 0xfb};
int FxGuidMatch(const u8* g24) noexcept {  // bt_probe_r31's: PF 14 saw 3 (the mixed order at +0x08)
    if (std::memcmp(g24, kFxGuidA, 16) == 0) return 1;
    if (std::memcmp(g24 + 8, kFxGuidA, 16) == 0) return 2;
    if (std::memcmp(g24, kFxGuidB, 16) == 0) return 3;
    if (std::memcmp(g24 + 8, kFxGuidB, 16) == 0) return 4;
    return 0;
}
bool FxResOk(uptr res) noexcept {
    u8 g[24] = {};
    return res && I::ReadN(res + kGuidOff, g, sizeof(g)) && FxGuidMatch(g) != 0;
}
// the post-process effector creator FUN_141CAD420(res, world, attach) -> instance
u64 FxDetour(u64 res, u64 world, u64 attach, u64 d) {
    if (!g_pass.load(std::memory_order_relaxed) && g_win.load(std::memory_order_acquire)) {
        g_fx_calls.fetch_add(1, std::memory_order_relaxed);
        if (FxResOk(static_cast<uptr>(res))) {
            g_fx_match.fetch_add(1, std::memory_order_relaxed);
            int z = 0;
            if (g_fade.compare_exchange_strong(z, 1)) {
                bool wrote = false;
                float cur[2] = {0, 0};
                if (I::ReadN(static_cast<uptr>(res) + kFadeOff, cur, sizeof(cur))) {
                    const V::Fade ours{Fl(g_want_start.load()), Fl(g_want_range.load())};
                    const V::Fade saved = V::FadeToSave(V::Fade{cur[0], cur[1]}, ours);
                    g_save_start.store(Bits(saved.start)), g_save_range.store(Bits(saved.range)), g_fade_res.store(static_cast<uptr>(res));
                    const float w[2] = {ours.start, ours.range};
                    wrote = I::WriteN(static_cast<uptr>(res) + kFadeOff, w, sizeof(w));
                }
                if (!wrote) g_fx_fail.fetch_add(1, std::memory_order_relaxed);
                g_fade.store(wrote ? 2 : 0, std::memory_order_release);
            }
        }
    }
    return g_fx_orig(res, world, attach, d);
}

// The catcher crater FUN_142A003D0(cm, id): the catcher record (by id) before, the pending crater record its generation names after.
struct CatEv {
    double pos[3] = {0, 0, 0}, cat_pos[3] = {0, 0, 0};
    float radius = 0;
    u32 flags = 0;
    i32 gen = 0;
    u8 class1 = 0;
    bool rec_ok = false, cat_ok = false;
};
constexpr unsigned kCatRing = 4;
std::atomic<int> g_cat_st[kCatRing];  // 0 free, 1 writing, 2 ready
CatEv g_cat_ev[kCatRing];
std::atomic<bool> g_cat_armed{false};
std::atomic<unsigned> g_cat_calls{0}, g_cat_none{0}, g_cat_lost{0};
u64 CatDetour(u64 cm, u64 id, u64 c, u64 d) {
    if (g_pass.load(std::memory_order_relaxed) || !g_cat_armed.load(std::memory_order_relaxed)) return g_cat_orig(cm, id, c, d);
    g_cat_calls.fetch_add(1, std::memory_order_relaxed);
    CatEv e;
    const uptr m = static_cast<uptr>(cm);
    i32 n = 0;
    const uptr data = static_cast<uptr>(Q(m + kCmData));
    if (data && I::Read(m + kCmCount, n) && n > 0 && n <= 64)
        for (i32 i = 0; i < n; ++i) {
            const uptr rec = data + static_cast<uptr>(i) * kCatRecSize;
            u32 key = 0;
            if (I::Read(rec + 0x20, key) && key == static_cast<u32>(id)) {
                e.cat_ok = I::ReadN(rec + 0x68, e.cat_pos, sizeof(e.cat_pos));
                u8 cls = 0xFF;
                if (I::Read(rec + 0x28, cls) && cls < R::kCatcherClassMax1) e.class1 = static_cast<u8>(cls + 1);
                break;
            }
        }
    i32 g0 = 0;
    const bool g0_ok = I::Read(m + kCmGen, g0);
    const u64 ret = g_cat_orig(cm, id, c, d);
    i32 g1 = 0;
    if (!g0_ok || !I::Read(m + kCmGen, g1) || g1 == g0) {
        g_cat_none.fetch_add(1, std::memory_order_relaxed);
        return ret;
    }
    e.gen = g1;
    i32 pn = 0;
    const uptr pd = static_cast<uptr>(Q(m + kCmPending + 8));
    if (pd && I::Read(m + kCmPending, pn) && pn > 0 && pn <= 256)
        for (i32 i = pn - 1; i >= 0; --i) {
            const uptr r = pd + static_cast<uptr>(i) * kCraterSize;
            i32 g = 0;
            if (I::Read(r + 0x48, g) && g == g1) {
                e.rec_ok = I::ReadN(r + 0x18, e.pos, sizeof(e.pos)) && I::Read(r + 0x30, e.radius);
                I::Read(r + 0x58, e.flags);
                break;
            }
        }
    for (unsigned i = 0; i < kCatRing; ++i) {
        int z = 0;
        if (g_cat_st[i].compare_exchange_strong(z, 1)) {
            g_cat_ev[i] = e;
            g_cat_st[i].store(2, std::memory_order_release);
            return ret;
        }
    }
    g_cat_lost.fetch_add(1, std::memory_order_relaxed);
    return ret;
}

bool g_fx_in = false, g_fx_tried = false, g_cat_in = false, g_cat_tried = false;
bool HookIn(uptr va, void* detour, Fn4* orig, const char* name) {
    char why[128] = {};
    if (!I::MhInit(why, sizeof(why))) {
        L("hook %s not in: %s", name, why);
        return false;
    }
    void* target = reinterpret_cast<void*>(A(va));
    MH_STATUS s = MH_CreateHook(target, detour, reinterpret_cast<void**>(orig));
    if (s == MH_OK) s = MH_EnableHook(target);
    // MH_ERROR_ALREADY_CREATED: bt_probe hooked it first in this session (a probe op): this feature part stays off.
    L("hook %s: %s", name, s == MH_OK ? "in (pass-through)" : MH_StatusToString(s));
    return s == MH_OK;
}

// ---------------------------------------------------------------- SEH-guarded game calls (game thread; POD only inside __try)
bool SehRef(uptr fn, u64* slot) noexcept {  // FUN_14191C790 (add) / FUN_14191C810 (release) on a ref node
    __try {
        reinterpret_cast<void (*)(u64*)>(fn)(slot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
struct BlastXf {  // what FUN_14299B1B0 builds for the resource's vt+0x80: the position and the identity rotation
    double pos[3];
    float rot[9];
};
bool SehBlast(uptr er, const BlastXf* xf, uptr world, uptr* ent) noexcept {
    __try {
        const uptr fn = *reinterpret_cast<uptr*>(*reinterpret_cast<uptr*>(er) + 0x80);
        const uptr e = reinterpret_cast<uptr (*)(void*, const BlastXf*)>(fn)(reinterpret_cast<void*>(er), xf);
        *ent = e;
        if (!e) return true;
        reinterpret_cast<void (*)(void*, u8)>(A(kBlastInit))(reinterpret_cast<void*>(e), 0);
        const uptr add = *reinterpret_cast<uptr*>(*reinterpret_cast<uptr*>(world) + 0x18);
        reinterpret_cast<void (*)(void*, void*, u64, u64, u64)>(add)(reinterpret_cast<void*>(world), reinterpret_cast<void*>(e), 0, 0, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
struct CraterIn {
    double pos[3];
    float radius, yaw;
    u32 flags;
};
// The catcher manager's pending crater append as FUN_142A003D0 does it, under its lock cm+0xE8. 0 ok, 1 faulted, 2 the grow failed.
int SehCraterAppend(uptr cm, const CraterIn* in, int* gen_out, int* index_out, u32* area_out) noexcept {
    int rc = 1;
    __try {
        CRITICAL_SECTION* cs = reinterpret_cast<CRITICAL_SECTION*>(cm + kCmLock);
        EnterCriticalSection(cs);
        __try {
            i32* gen = reinterpret_cast<i32*>(cm + kCmGen);
            i32* n = reinterpret_cast<i32*>(cm + kCmPending);
            i32* cap = reinterpret_cast<i32*>(cm + kCmPending + 4);
            uptr* data = reinterpret_cast<uptr*>(cm + kCmPending + 8);
            const i32 g = ++*gen;
            if (*n + 1 > *cap) {
                const i32 want = *n + 1, newcap = want + want / 8 + 2;
                const uptr nd = reinterpret_cast<uptr (*)(void*, uptr, i32)>(A(kGrowFn))(reinterpret_cast<void*>(n), *data, newcap);
                if (!nd) {
                    rc = 2;
                    __leave;
                }
                *data = nd;
                if (*cap < want) {
                    rc = 2;
                    __leave;
                }
            }
            u8* rec = reinterpret_cast<u8*>(*data + static_cast<uptr>(*n) * kCraterSize);
            std::memset(rec, 0, kCraterSize);
            std::memcpy(rec + 0x18, in->pos, sizeof(in->pos));
            *reinterpret_cast<float*>(rec + 0x30) = in->radius;
            *reinterpret_cast<float*>(rec + 0x44) = 8.1f;
            *reinterpret_cast<i32*>(rec + 0x48) = g;
            const uptr area_obj = *reinterpret_cast<uptr*>(A(kAreaGlobal));
            const u32 area = reinterpret_cast<u32 (*)(void*)>(A(kAreaFn))(reinterpret_cast<void*>(area_obj));
            *reinterpret_cast<u32*>(rec + 0x50) = area & 0xFFFFu;
            *reinterpret_cast<float*>(rec + 0x54) = in->yaw;
            *reinterpret_cast<u32*>(rec + 0x58) = in->flags;
            *index_out = *n;
            ++*n;
            *reinterpret_cast<u8*>(cm + kCmDirty) = 1;
            *gen_out = g;
            *area_out = area & 0xFFFFu;
            rc = 0;
        } __finally {
            LeaveCriticalSection(cs);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rc = 1;
    }
    return rc;
}

// ---------------------------------------------------------------- the fade restore (game thread; EmergencyStop: any thread)
// 0 nothing was written (closed), 1 restored, 2 the resource changed (left), 3 the write failed, 4 already the saved values,
// -1 the detour is writing (try again), -2 another restore runs.
int FadeClose(bool wait, V::Fade* saved_out) noexcept {
    g_win.store(false, std::memory_order_release);
    for (int tries = 0;; ++tries) {
        int s = g_fade.load(std::memory_order_acquire);
        if (s == 4) return 0;
        if (s == 0) {
            if (g_fade.compare_exchange_strong(s, 4)) return 0;
            continue;
        }
        if (s == 1) {
            if (!wait || tries > 200) return -1;
            Sleep(1);
            continue;
        }
        if (s == 3) return -2;
        if (g_fade.compare_exchange_strong(s, 3)) break;  // s == 2
    }
    const uptr res = g_fade_res.load();
    const V::Fade ours{Fl(g_want_start.load()), Fl(g_want_range.load())};
    const V::Fade saved{Fl(g_save_start.load()), Fl(g_save_range.load())};
    if (saved_out) *saved_out = saved;
    float cur[2] = {0, 0};
    int rc = 2;
    if (FxResOk(res) && I::ReadN(res + kFadeOff, cur, sizeof(cur))) {
        switch (V::RestoreFor(V::Fade{cur[0], cur[1]}, ours, saved)) {
            case V::RestoreCall::write: {
                const float w[2] = {saved.start, saved.range};
                rc = I::WriteN(res + kFadeOff, w, sizeof(w)) ? 1 : 3;
                break;
            }
            case V::RestoreCall::done: rc = 4; break;
            case V::RestoreCall::changed: rc = 2; break;
        }
    }
    g_fade.store(4, std::memory_order_release);
    return rc;
}

// ---------------------------------------------------------------- the near check reads (game thread)
std::atomic<bool> g_flow_ok{false};
V::NearRead NearNow() noexcept {
    V::NearRead r;
    game::Diagnostics d;
    game::ReadDiagnostics(&d);
    r.health = d.health_valid ? d.health : -1.0f;
    r.flow_ok = g_flow_ok.load();
    r.flow_calls = r.flow_ok ? I::FlowCalls() : 0;
    const uptr hm = I::HunterMgr();
    r.hm_ok = hm && I::Read(hm + kHmArea, r.hm140);
    u64 st = 0;
    u32 load = 0;
    r.game_over = game::ReadStopGates(&st, &load) && (st & (1ull << 13)) != 0;
    return r;
}

// ---------------------------------------------------------------- the module's state (game thread unless atomic)
V::Fifo<V::PendingVoid, V::kBlastQueueCap> g_bq;
V::Fifo<V::PendingVoid, V::kCraterQueueCap> g_cq;
struct Seen {  // the last voidouts (local or partner blasts) for the repeat check
    double pos[3] = {0, 0, 0};
    double ms = -1e18;
};
Seen g_seen[8];
unsigned g_seen_at = 0;
void Remember(const double* p, double ms) {
    Seen& s = g_seen[g_seen_at++ % 8];
    std::memcpy(s.pos, p, sizeof(s.pos));
    s.ms = ms;
}
bool Repeated(const double* p, double ms) {
    for (const Seen& s : g_seen)
        if (V::Repeat(s.pos, s.ms, p, ms)) return true;
    for (unsigned i = 0; i < g_bq.n; ++i)
        if (V::Repeat(g_bq.e[i].m.pos, g_bq.e[i].rx_ms, p, ms)) return true;
    return false;
}

alignas(16) u64 g_ref[3] = {};  // the blast's ref node (bt_probe 29's: [0] the entity +0x20, [1] / [2] the list links)
std::atomic<bool> g_ref_held{false};
bool g_blast_dead = false;  // a blast call or the ref faulted: no more partner blasts this session
bool g_vb_off = false;      // the blast or fx checks failed, or the fx hook did not go in
const char* g_vb_off_why = nullptr;
bool g_cr_dead = false;     // the crater append faulted
struct Blast {
    bool on = false, win = false, near_done = false;
    double start_ms = 0, dist = -1;
    bool far_res = false;
    uptr ent = 0;
    u32 id = 0;
    V::NearRead before;
} g_b;
struct Added {  // the craters this game added (information: the +9 s line and the kept / gone line after a load)
    bool on = false, l9 = false;
    int after = 0;  // 0 no load yet, 1 a load ended: look, 2 logged
    double pos[3] = {0, 0, 0};
    float radius = 0;
    int gen = 0;
    double add_ms = 0, look_from = 0, look_until = 0;
} g_added[4];
unsigned g_added_at = 0;

u32 g_void_id = 0;
unsigned g_rx = 0, g_sent = 0, g_blasts = 0, g_craters = 0, g_drops = 0, g_refused = 0;
std::uint64_t g_rng = 0;
float Rand01() {
    if (!g_rng) g_rng = GetTickCount64() * 0x9E3779B97F4A7C15ull | 1ull;
    g_rng ^= g_rng << 13, g_rng ^= g_rng >> 7, g_rng ^= g_rng << 17;
    return static_cast<float>(static_cast<double>(g_rng >> 11) / 9007199254740992.0);
}

// bt_sync's FIELD compare (both censuses in and the records equal); else the host refuses a guest's crater as "the BT records
// differ" (a guest applies the host's without it).
bool RecordsEqualHere() noexcept { return I::RecordsEqual(); }

bool Send(const R::BtsVoid2& m) noexcept {
    u8 buf[R::kBtsVoid2Size] = {};
    const std::size_t n = R::EncodeVoid2(I::Seq(), m, buf, sizeof(buf));
    const bool ok = n && I::SendBts(buf, n);
    if (ok) ++g_sent;
    return ok;
}
bool SendNow() noexcept { return V::SendOk(g_key, I::SyncLevel(), I::SessionRole(), I::PeerOn(), I::Peer().caps); }

// ---------------------------------------------------------------- the blast (game thread)
void CloseWindow(const char* why, bool force) {
    if (!g_b.win) return;
    V::Fade saved;
    const int rc = FadeClose(force, &saved);
    if (rc == -1 || rc == -2) {
        if (!force) return;  // the detour is writing: the next tick
    }
    g_b.win = false;
    switch (rc) {
        case 0: L("voidout flash window closed (%s): no effector made in the window (%u effector calls)", why, g_fx_calls.load()); break;
        case 1: L("voidout flash window closed (%s): fade restored (FadeStart %.0f FadeRange %.0f)", why, saved.start, saved.range); break;
        case 4: L("voidout flash window closed (%s): fade restored (already FadeStart %.0f FadeRange %.0f)", why, saved.start, saved.range); break;
        case 2: L("voidout flash window closed (%s): the effector resource changed (left as it is)", why); break;
        case 3: L("voidout flash window closed (%s): the fade restore write FAILED", why); break;
        default: L("voidout flash window closed (%s): the fade restore did not finish (%d)", why, rc); break;
    }
}
const char* ReleaseRef() {
    if (!g_ref_held.load()) return "none held";
    g_ref_held.store(false);
    const bool cleared = g_ref[0] == 0;  // the entity went first: its list cleared our node (the release is then a no-op)
    if (g_blast_dead) return "left (an earlier ref call faulted)";
    if (!SehRef(A(kRefRelease), g_ref)) {
        g_blast_dead = true;
        return "release FAULTED (no more partner blasts)";
    }
    return cleared ? "released (the game had cleared it)" : "released";
}
void BlastEnd(const char* why) {
    if (!g_b.on) return;
    CloseWindow(why, true);
    g_b.on = false;
    const char* ref = ReleaseRef();
    L("partner voidout blast done (%s): ref %s (VOID2 #%u)", why, ref, g_b.id);
}
void NearLine(double now_ms) {
    g_b.near_done = true;
    char what[200] = {};
    V::NearCheck(g_b.before, NearNow(), what, sizeof(what));
    L("partner blast near check +%.0f s (%.0f m from you): %s", (now_ms - g_b.start_ms) / 1000.0, g_b.dist, what);
}
bool VbUsable(const char** why) {
    if (g_blast_dead) return *why = "an earlier blast call FAULTED (no more partner blasts)", false;
    if (g_vb_off) return *why = g_vb_off_why ? g_vb_off_why : "off", false;
    const unsigned ok = Check();
    if (!(ok & kGBlast)) return g_vb_off = true, g_vb_off_why = *why = "the blast byte checks failed (log)", false;
    if (!(ok & kGFx)) return g_vb_off = true, g_vb_off_why = *why = "the effector byte checks failed (log): no faded flash", false;
    return true;
}
// The head of the blast queue, played (or dropped with the reason when a call fails).
void Play(const V::PendingVoid& p, uptr res, bool far_res, double dist, const TickCtx& ctx) {
    char pos[64];
    FmtPos(pos, sizeof(pos), p.m.pos);
    if (!g_fx_in) {
        if (!g_fx_tried) {
            g_fx_tried = true;
            g_fx_in = HookIn(kFxCreate, reinterpret_cast<void*>(&FxDetour), &g_fx_orig, "FUN_141CAD420 (the effector creator)");
        }
        if (!g_fx_in) {
            g_vb_off = true, g_vb_off_why = "the effector hook did not go in (log): no faded flash";
            ++g_drops;
            L("partner voidout blast dropped: %s (VOID2 #%u)", g_vb_off_why, p.m.void_id);
            return;
        }
    }
    const uptr world = static_cast<uptr>(Q(A(kWorldGlobal)));
    const uptr er = static_cast<uptr>(Q(res + (far_res ? kResFar : kResNear)));
    if (!world || !er) {
        ++g_drops;
        L("partner voidout blast dropped: no %s (VOID2 #%u)", !world ? "world" : "annihilation resource", p.m.void_id);
        return;
    }
    if (ctx.hidden_run && !g_flow_ok.load()) {
        char fw[128] = {};
        g_flow_ok.store(I::FlowGate(fw, sizeof(fw)));
        if (!g_flow_ok.load()) L("the GameOver flow counter is not in (%s): the near check reads health, hm+0x140, GameOver", fw);
    }
    g_b = Blast{};
    g_b.before = NearNow();
    const V::Fade ours = V::FadeFor(g_flash_m);
    g_want_start.store(Bits(ours.start)), g_want_range.store(Bits(ours.range));
    g_fx_calls.store(0), g_fx_match.store(0), g_fx_fail.store(0);
    g_fade.store(0, std::memory_order_release);
    g_win.store(true, std::memory_order_release);
    BlastXf xf{};
    std::memcpy(xf.pos, p.m.pos, sizeof(xf.pos));
    const float rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::memcpy(xf.rot, rot, sizeof(rot));
    uptr e = 0;
    const bool ok = SehBlast(er, &xf, world, &e);
    if (!ok || !e) {
        g_b.win = true;
        CloseWindow("no blast", true);
        if (!ok) g_blast_dead = true;
        ++g_drops;
        L("partner voidout blast at (%s): %s (VOID2 #%u)", pos, ok ? "no entity made" : "the blast call FAULTED (no more partner blasts)", p.m.void_id);
        return;
    }
    g_ref[0] = e + 0x20, g_ref[1] = 0, g_ref[2] = 0;
    const bool linked = SehRef(A(kRefAdd), g_ref);
    g_ref_held.store(linked);
    if (!linked) g_blast_dead = true, L("the ref add FUN_14191C790 FAULTED (no ref held; no more partner blasts)");
    g_b.on = true, g_b.win = true, g_b.start_ms = ctx.now_ms, g_b.dist = dist, g_b.far_res = far_res, g_b.ent = e, g_b.id = p.m.void_id;
    ++g_blasts;
    Remember(p.m.pos, p.rx_ms);
    if (dist >= 0)
        L("partner voidout blast at (%s) %.0f m from you: %s resource, flash fade %.0f/%.0f m (source %s, VOID2 #%u, entity 0x%llX)", pos, dist,
          far_res ? "far" : "near", ours.start, ours.range, V::SourceName(p.m.source), p.m.void_id, H(e));
    else
        L("partner voidout blast at (%s) at an unknown distance: near resource, flash fade %.0f/%.0f m (source %s, VOID2 #%u, entity 0x%llX)", pos,
          ours.start, ours.range, V::SourceName(p.m.source), p.m.void_id, H(e));
}
void BlastStep(const TickCtx& ctx) {
    const double now = ctx.now_ms;
    if (g_b.on) {
        const double t = now - g_b.start_ms;
        if (g_b.win && t >= R::kBlastWindowMs) CloseWindow("9 s", false);
        if (!g_b.near_done && t >= 12000.0) NearLine(now);
        if (t >= R::kBlastRefMs) BlastEnd("16 s");
    }
    while (g_bq.n) {
        V::PendingVoid& p = g_bq.e[0];
        R::BlastIn in;
        in.bt_void = g_key;
        in.pos_ok = R::PosSane(p.m.pos);
        in.age_ms = now - p.rx_ms;
        in.loading = ctx.loading;
        in.not_playing = ctx.playing_why;
        in.blast_active = g_b.on;
        const uptr mgr = I::GazerMgr();
        const uptr res = mgr ? static_cast<uptr>(Q(mgr + kMgrRes)) : 0;
        in.has_resource = res != 0;
        float far_m = -1;
        if (res) I::Read(res + kResFarDist, far_m);
        in.far_m = far_m;
        in.dist_m = ctx.me_ok ? R::Dist3(ctx.me, p.m.pos) : -1.0;
        in.real_save = ctx.real_save || g_standin;
        R::BlastVerdict v = V::BlastFor(ctx.sync_level, in);
        const char* off = nullptr;
        if (v.call == R::BlastCall::play && !VbUsable(&off)) v = R::BlastVerdict{R::BlastCall::drop, off, false};
        if (v.call == R::BlastCall::play) {
            const V::PendingVoid q = p;
            g_bq.PopFront();
            Play(q, res, v.far_res, in.dist_m, ctx);
            return;
        }
        if (v.call == R::BlastCall::hold) {
            if (!p.held_logged) p.held_logged = true, L("partner voidout blast held: %s (VOID2 #%u)", v.why, p.m.void_id);
            return;
        }
        ++g_drops;
        if (in.dist_m >= 0)
            L("partner voidout blast dropped: %s (VOID2 #%u, %.0f m from you)", v.why, p.m.void_id, in.dist_m);
        else
            L("partner voidout blast dropped: %s (VOID2 #%u)", v.why, p.m.void_id);
        g_bq.PopFront();
    }
}

// ---------------------------------------------------------------- the crater (game thread)
void Track(const double* pos, float r, int gen, double now) {
    Added& a = g_added[g_added_at++ % 4];
    a = Added{};
    a.on = true, a.gen = gen, a.radius = r, a.add_ms = now;
    std::memcpy(a.pos, pos, sizeof(a.pos));
}
// One of the catcher manager's crater lists (+0x280 live, +0x290 pending): the record with this gen, or at this place and radius.
uptr FindCrater(uptr cm, uptr list, int gen, const double* pos, float r) {
    i32 n = 0;
    const uptr data = static_cast<uptr>(Q(cm + list + 8));
    if (!data || !I::Read(cm + list, n) || n <= 0) return 0;
    for (i32 i = 0; i < n && i < 512; ++i) {
        const uptr rec = data + static_cast<uptr>(i) * kCraterSize;
        if (pos) {
            double p[3] = {0, 0, 0};
            float rr = 0;
            if (I::ReadN(rec + 0x18, p, sizeof(p)) && I::Read(rec + 0x30, rr) && V::SameCrater(pos, r, p, rr)) return rec;
        } else {
            i32 g = 0;
            if (I::Read(rec + 0x48, g) && g == gen) return rec;
        }
    }
    return 0;
}
void Apply(const V::PendingVoid& p, const TickCtx& ctx) {
    char pos[64];
    FmtPos(pos, sizeof(pos), p.m.pos);
    // Belt and braces (CraterApplyGate says it first): never on a real-save guest.
    if (ctx.role == 2 && (ctx.real_save || g_standin || !ctx.saves_redirected)) {
        ++g_refused;
        L("crater not added: this game plays on its own real save (host_save=0): no crater record (VOID2 #%u)", p.m.void_id);
        return;
    }
    const uptr cm = I::CatcherMgr();
    if (!cm) {
        ++g_refused;
        L("crater not added: no catcher manager (or its byte checks failed) (VOID2 #%u)", p.m.void_id);
        return;
    }
    CraterIn in{};
    std::memcpy(in.pos, p.m.pos, sizeof(in.pos));
    in.radius = R::CraterRadius(p.m.crater_r);
    in.yaw = Rand01() * 6.2831853f;
    in.flags = R::CraterFlags(ctx.me_ok ? R::Dist3(ctx.me, p.m.pos) : -1.0, -1.0, in.radius);
    int gen = 0, idx = -1;
    u32 area = 0;
    const int rc = SehCraterAppend(cm, &in, &gen, &idx, &area);
    if (rc != 0) {
        ++g_refused;
        if (rc == 1) g_cr_dead = true;
        L("crater not added at (%s): %s (VOID2 #%u)", pos, rc == 2 ? "the pending list did not grow" : "the append FAULTED (no more craters)", p.m.void_id);
        return;
    }
    ++g_craters;
    Track(p.m.pos, in.radius, gen, ctx.now_ms);
    L("crater record added: (%s) r %.0f (pending %d, delay 8.1 s, area %u, flags %u, gen %d; %s, VOID2 #%u)", pos, in.radius, idx, area, in.flags, gen,
      V::SourceName(p.m.source), p.m.void_id);
}
void CraterStep(const TickCtx& ctx) {
    const double now = ctx.now_ms;
    while (g_cq.n) {
        V::PendingVoid& p = g_cq.e[0];
        R::CraterApplyIn in;
        in.bt_void = g_key;
        in.role = ctx.role;
        in.sandboxed = ctx.saves_redirected && !ctx.real_save && !g_standin;
        in.sender_sandboxed = ctx.peer && !ctx.partner.real_save;
        const u32 here = esync::Gate().guids;
        in.world_equal = here != 0 && p.m.world != 0 && here == p.m.world;
        in.records_equal = RecordsEqualHere();
        in.flags = p.m.flags;
        in.age_ms = now - p.rx_ms;
        in.loading = ctx.loading;
        in.not_playing = ctx.playing_why;
        in.carried = ctx.carried;
        R::KillVerdict v = V::CraterGate(ctx.sync_level, in);
        if (v.call == R::KillCall::apply) {
            if (g_cr_dead) v = R::KillVerdict{R::KillCall::refuse, "an earlier append FAULTED (no more craters)"};
            else if (!(Check() & kGCrater)) v = R::KillVerdict{R::KillCall::refuse, "the crater byte checks failed (log)"};
        }
        if (v.call == R::KillCall::apply) {
            const V::PendingVoid q = p;
            g_cq.PopFront();
            Apply(q, ctx);
            return;
        }
        if (v.call == R::KillCall::hold) {
            if (!p.held_logged) p.held_logged = true, L("crater held: %s (VOID2 #%u)", v.why, p.m.void_id);
            return;
        }
        ++g_refused;
        L("crater not added: %s (VOID2 #%u)", v.why, p.m.void_id);
        g_cq.PopFront();
    }
}
void TrackStep(const TickCtx& ctx) {
    if (ctx.loading) return;
    const double now = ctx.now_ms;
    for (Added& a : g_added) {
        if (!a.on) continue;
        if (!a.l9 && a.after == 0 && now - a.add_ms >= V::kCraterActiveMs) {
            a.l9 = true;
            const uptr cm = I::CatcherMgr();
            const uptr live = cm ? FindCrater(cm, kCmCraters, a.gen, nullptr, 0) : 0;
            if (live) {
                u8 active = 0xFF;
                I::Read(live + 0x3C, active);
                const uptr ref = static_cast<uptr>(Q(live));
                L("crater record active +9 s: gen %d, active %u, entity 0x%llX", a.gen, active, H(ref > 0x20 ? ref - 0x20 : 0));
            } else {
                const bool pending = cm && FindCrater(cm, kCmPending, a.gen, nullptr, 0);
                L("crater record not live +9 s: gen %d %s", a.gen, pending ? "still pending" : "in neither list");
            }
        }
        if (a.after == 1 && now >= a.look_from) {
            const uptr cm = I::CatcherMgr();
            const bool found = cm && (FindCrater(cm, kCmCraters, 0, a.pos, a.radius) || FindCrater(cm, kCmPending, 0, a.pos, a.radius));
            char pos[64];
            FmtPos(pos, sizeof(pos), a.pos);
            if (found) a.after = 2, L("crater record kept after the load: (%s) r %.0f (gen %d before the load)", pos, a.radius, a.gen);
            else if (now >= a.look_until)
                a.after = 2, L("crater record gone after the load: (%s) r %.0f (a host-held guest reloads the host's save)", pos, a.radius);
        }
    }
}

// ---------------------------------------------------------------- the catcher craters this game made (game thread: the drain)
void CatHookStep(const TickCtx& ctx) {
    const bool want = g_key >= 1 && SendNow();
    if (want && !g_cat_in && !g_cat_tried) {
        const unsigned ok = Check();
        if ((ok & kGCatHook) && (ok & kGCrater)) {
            g_cat_tried = true;
            for (auto& s : g_cat_st) s.store(0);
            g_cat_in = HookIn(kCatCrater, reinterpret_cast<void*>(&CatDetour), &g_cat_orig, "FUN_142A003D0 (the catcher crater)");
            if (!g_cat_in) L("catcher craters are not sent (the hook did not go in)");
        } else {
            g_cat_tried = true;  // Check() logged the failed group
        }
    }
    g_cat_armed.store(want && g_cat_in && !ctx.loading);
    for (unsigned i = 0; i < kCatRing; ++i) {
        if (g_cat_st[i].load(std::memory_order_acquire) != 2) continue;
        const CatEv e = g_cat_ev[i];
        g_cat_st[i].store(0, std::memory_order_release);
        if (!want) continue;
        V::CatcherIn in;
        if (e.rec_ok) std::memcpy(in.pos, e.pos, sizeof(in.pos)), in.radius = e.radius;
        else if (e.cat_ok) std::memcpy(in.pos, e.cat_pos, sizeof(in.pos));
        in.pos_ok = e.rec_ok || e.cat_ok;
        in.class1 = e.class1;
        in.me_ok = ctx.me_ok;
        std::memcpy(in.me, ctx.me, sizeof(in.me));
        in.sender_ms = static_cast<u32>(link::NowMs()), in.void_id = ++g_void_id, in.world = esync::Gate().guids;
        R::BtsVoid2 m;
        char pos[64];
        FmtPos(pos, sizeof(pos), in.pos);
        if (!V::Void2FromCatcher(in, m)) {
            L("catcher crater gen %d not sent: no position", e.gen);
            continue;
        }
        const bool sent = Send(m);
        L("catcher crater gen %d at (%s) r %.0f (%s, %s): VOID2 #%u %s", e.gen, pos, m.crater_r, R::CatcherTypeName(m.class1),
          e.rec_ok ? "its pending record" : "the catcher's place", m.void_id, sent ? "sent" : "NOT sent");
    }
}

}  // namespace

void Install(const Config& config) noexcept {
    g_key = config.bt_void;
    g_flash_m = R::FlashStartM(static_cast<double>(config.bt_void_flash_m));
    for (auto& s : g_cat_st) s.store(0);
    L("bt_void=%d, flash fade %d/%.0f m: %s", g_key, g_flash_m, static_cast<double>(R::kFlashRangeM),
      g_key >= 2 ? (V::CraterDropped() ? "the partner blast; the crater record DROPPED this round (refused: bt_void 2 acts as 1)"
                                      : "the partner blast and the crater record")
      : g_key == 1 ? "the partner blast only"
                   : "off");
}

void OnHello(const PeerHello& peer) noexcept {
    if (g_key < 1) return;
    L("partner hello: VOID2 %s, craters applied there %s", (peer.caps & R::kCapVoid2) ? "yes" : "no", (peer.caps & R::kCapWorldApply) ? "yes" : "no");
}

void OnMsg(std::uint8_t type, const std::uint8_t* msg, std::size_t n, double now_ms) noexcept {
    if (type != R::kBtsVoid2 || g_pass.load()) return;
    R::BtsVoid2 m;
    if (!R::DecodeVoid2(msg, n, m)) {
        L("partner VOID2 refused: it did not decode");
        return;
    }
    ++g_rx;
    char pos[64];
    FmtPos(pos, sizeof(pos), m.pos);
    L("partner VOID2 #%u: %s at (%s), %.0f m from the partner, flags 0x%02X, crater r %.0f%s", m.void_id, V::SourceName(m.source), pos, m.player_dist, m.flags,
      m.crater_r, g_key < 1 ? " (bt_void is 0 here: logged only)" : "");
    if (g_key < 1) return;
    V::PendingVoid p;
    p.m = m, p.rx_ms = now_ms;
    if (Repeated(m.pos, now_ms)) {
        ++g_drops;
        L("partner voidout blast dropped: a repeat of one within 5 s and 100 m (VOID2 #%u)", m.void_id);
    } else if (!g_bq.Push(p)) {
        ++g_drops;
        L("partner voidout blast dropped: the blast queue is full (VOID2 #%u)", m.void_id);
    }
    if (m.flags & R::kV2CraterWanted) {
        if (!g_cq.Push(p)) ++g_refused, L("crater not added: the crater queue is full (VOID2 #%u)", m.void_id);
    }
}

void Tick(const TickCtx& ctx) noexcept {
    g_tick_tid.store(GetCurrentThreadId());
    if (g_key < 1 || g_pass.load() || I::Stopped()) return;
    CatHookStep(ctx);
    BlastStep(ctx);
    CraterStep(ctx);
    TrackStep(ctx);
}

void OnWorldEvent(WorldEvent e, double now_ms) noexcept {
    if (g_key < 1 || g_pass.load()) return;
    const unsigned nb = g_bq.n, nc = g_cq.n;
    switch (e) {
        case WorldEvent::load_start:
            g_bq.Clear(), g_cq.Clear();
            g_cat_armed.store(false);
            for (auto& s : g_cat_st) {
                int r = 2;
                s.compare_exchange_strong(r, 0);
            }
            if (g_b.on) BlastEnd("a load");
            break;
        case WorldEvent::load_end:
            for (Added& a : g_added)
                if (a.on && a.after != 2) a.after = 1, a.look_from = now_ms + V::kAfterLoadFirstMs, a.look_until = now_ms + V::kAfterLoadLastMs;
            break;
        case WorldEvent::link_lost:
        case WorldEvent::partner_gone:
            g_bq.Clear(), g_cq.Clear();
            if (g_b.win) CloseWindow(WorldEventName(e), true);
            break;
    }
    if (nb || nc) L("%s: %u queued blasts and %u queued craters dropped", WorldEventName(e), nb, nc);
}

void EmergencyStop() noexcept {
    g_pass.store(true);
    g_cat_armed.store(false);
    V::Fade saved;
    const int rc = FadeClose(true, &saved);
    // The ref is released only on the game thread (EmergencyStop after a tick fault runs there); elsewhere the static node stays
    // and the game clears it with the entity.
    const char* ref = g_ref_held.load() ? "left for the game (not the game thread)" : "none held";
    if (g_ref_held.load() && GetCurrentThreadId() == g_tick_tid.load()) ref = ReleaseRef();
    L("EmergencyStop: detours through, fade %s, ref %s", rc == 1 || rc == 4 ? "restored" : rc == 0 ? "not written" : "left (the restore did not finish)",
      ref);
}

std::uint16_t Caps() noexcept { return V::Caps(g_key, I::SyncLevel(), I::SessionRole(), I::RealSave()); }

void Status(char* out, std::size_t cap) noexcept {
    if (!out || !cap) return;
    const char* vb = g_key < 1 ? "off" : g_blast_dead ? "DEAD" : g_vb_off ? "OFF" : "on";
    const char* cr = g_key < 2 ? "off" : V::CraterDropped() ? "DROPPED" : g_cr_dead ? "DEAD" : "on";
    std::snprintf(out, cap, "void: bt_void=%d VB %s CR %s; rx %u, sent %u, blasts %u, craters %u, dropped %u, refused %u, cat calls %u%s, log dropped %u",
                  g_key, vb, cr, g_rx, g_sent, g_blasts, g_craters, g_drops, g_refused, g_cat_calls.load(), g_cat_in ? "" : " (no hook)",
                  g_log_dropped.load());
}

bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (op != 9) return Put(msg, cap, "bt sync %d: not bt_void's", op), false;
    if (!automation::Enabled()) return Put(msg, cap, "bt sync 9: the test voidout runs in hidden runs only"), false;
    if (g_key < 1) return Put(msg, cap, "bt sync 9: bt_void is 0 here"), false;
    if (g_pass.load() || I::Stopped()) return Put(msg, cap, "bt sync 9: stopped (emergency stop)"), false;
    V::TestArgs a;
    if (!V::ParseTest(v[1], v[2], a))
        return Put(msg, cap, "bt sync 9 <m 0..3000> <mode: 0 ahead of me, 1 from the partner, +2 a catcher crater; 4 with m 1|0: the real-save stand-in>"), false;
    if (a.standin) {
        g_standin = a.standin_on;
        L("real-save stand-in %s on this game (bt_void's gates only: the blast from 300 m, no crater)", g_standin ? "ON" : "off");
        return Put(msg, cap, "bt sync 9: the real-save stand-in %s", g_standin ? "on" : "off"), true;
    }
    const int role = I::SessionRole();
    if (role != 1 && role != 2) return Put(msg, cap, "bt sync 9: not a two-game role"), false;
    if (!I::PeerOn()) return Put(msg, cap, "bt sync 9: no partner hello yet"), false;
    if (!SendNow()) return Put(msg, cap, "bt sync 9: the partner's hello does not offer VOID2 (bt_void 0 there, or bt_sync 0)"), false;
    double me[3] = {0, 0, 0}, fx = 0, fy = 0, pp[3] = {0, 0, 0}, spot[3] = {0, 0, 0};
    if (!I::SamFacing(me, &fx, &fy)) return Put(msg, cap, "bt sync 9: no local player"), false;
    const bool pp_ok = I::PartnerPos(pp);
    if (!V::TestSpot(me, fx, fy, pp, pp_ok, a.m, a.from_partner, spot))
        return Put(msg, cap, "bt sync 9: no spot (%s)", a.from_partner ? "no partner position" : "no facing"), false;
    R::BtsVoid2 m;
    if (!V::Void2Test(spot, R::Dist3(me, spot), a.crater, static_cast<u32>(link::NowMs()), ++g_void_id, esync::Gate().guids, m))
        return Put(msg, cap, "bt sync 9: a bad spot"), false;
    const bool sent = Send(m);
    char pos[64];
    FmtPos(pos, sizeof(pos), spot);
    const double pd = pp_ok ? R::Dist3(pp, spot) : -1.0;
    L("test VOID2 #%u at (%s): %.0f m from the partner's body, %.0f m from me%s: %s", m.void_id, pos, pd, static_cast<double>(m.player_dist),
      a.crater ? ", a catcher crater r 40 wanted" : "", sent ? "sent" : "NOT sent");
    return Put(msg, cap, "bt sync 9: test VOID2 #%u %s (%.0f m from the partner)", m.void_id, sent ? "sent" : "not sent", pd), sent;
}

void OnLocalVoid(const LocalVoid& v, double now_ms) noexcept {
    if (v.pos_ok && R::PosSane(v.pos)) Remember(v.pos, now_ms);
    if (g_key < 1 || g_pass.load() || !SendNow()) return;
    V::LocalIn in;
    std::memcpy(in.pos, v.pos, sizeof(in.pos));
    in.pos_ok = v.pos_ok, in.caused = v.caused, in.baggage = v.baggage, in.source = v.source, in.far_dist = v.far_dist;
    in.me_ok = I::LocalPos(in.me);
    in.sender_ms = static_cast<u32>(link::NowMs()), in.void_id = ++g_void_id, in.world = esync::Gate().guids;
    R::BtsVoid2 m;
    if (!V::Void2FromLocal(in, m)) {
        L("local voidout not sent: no position");
        return;
    }
    char pos[64];
    FmtPos(pos, sizeof(pos), m.pos);
    const bool sent = Send(m);
    L("local voidout at (%s) (%s, %s, flags 0x%02X): VOID2 #%u %s", pos, V::SourceName(m.source), R::VoidReasonName(m.reason), m.flags, m.void_id,
      sent ? "sent" : "NOT sent");
}

}  // namespace st::bt::voidout
