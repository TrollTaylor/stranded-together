#include "menu_figure.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "game_api.hpp"
#include "log.hpp"
#include "partner_source.hpp"
#include "pose_wire.hpp"

namespace st::menufig {
namespace {

namespace mem = partner::mem;
using mem::Q;
using mem::Rd;

// ---------------------------------------------------------------- ds.exe (RVAs at base 0x140000000)
// Evidence: docs/real-partner-body.md section 15 (decompiles of the subspace ticker 0x1426C19B0, its
// states 0x1426ACB10 / 0x1426AD430 / 0x1426AB100 / 0x1426ADC10, open / close 0x1426B5230 / 0x1426B5290,
// the AKukan constructor 0x1426F3270, the ArtParts setters 0x1422B25F0 / 0x1422B2770 / 0x1422B8FE0 /
// 0x1422B8F10 / 0x1422B94D0 and ApplyMeshControl 0x1426D9120). Byte-checked once.
constexpr std::uintptr_t kWork = 0x7BE79B0;          // global: the subspace work object
constexpr std::uintptr_t kWorkReader = 0x26B5989;    // mov rcx,[rip->kWork]; test rcx,rcx; je +5; call
constexpr std::uintptr_t kTicker = 0x26C19B0;        // the work object's ticker (lock at +0x9C0)
constexpr std::uintptr_t kTickerState = 0x26C19DE;   // mov ecx,[rdi+0x1C]; mov edx,[rax+0x44C]; movzx eax,byte [rdi+0x9C8]
constexpr std::uintptr_t kTickerState2 = 0x26C19FC;  // mov eax,[rdi+0x14]
constexpr std::uintptr_t kCreateStores = 0x26ACC1C;  // state 1: mov [rbx+0x50],rax (the player); mov rax,[rbx+0x30]
constexpr std::uintptr_t kCreateFigure = 0x26ACC2F;  // state 1: mov [rbx+0x58],rcx (the figure)
constexpr std::uintptr_t kOpenKind = 0x26B5253;      // open: movzx eax,[rsi+0x9C8]; and al,0xDF; mov [rsi+0xC],edi; or al,0x10; mov [rsi+8],edi
constexpr std::uintptr_t kAkukanVtSite = 0x26F329E;  // the AKukan constructor: lea rax,[rip->vtable]; mov [rdi],rax
constexpr std::uintptr_t kAkukanVtable = 0x3D7DE48;  // DSPlayerAKukanEntity (RTTI 0x144BCA020, 0x7A0 bytes)
constexpr std::uintptr_t kSmVtable = 0x3D0ADB8;      // SkinnedModel
constexpr std::uintptr_t kSmVtSlot0 = 0x23A0950;
// ArtParts (the same functions and signatures partner_gear.cpp checks).
constexpr std::uintptr_t kApdSetPart = 0x22B2770, kApdSetAll = 0x22B25F0, kModelBegin = 0x2391620, kModelEnd = 0x23966A0;

constexpr std::uintptr_t kWState = 0x14, kWReq = 0x1C, kWKind = 0x08, kWKindNext = 0x0C, kWFlags = 0x9C8, kWWeak = 0x30,
                         kWPlayer = 0x50, kWFigure = 0x58;
constexpr std::uintptr_t kEntModel = 0xB8, kEntWorld = 0xC8, kSmEntity = 0x48, kSmPoses = 0x170, kSmPoseIdx = 0x250;
constexpr std::uintptr_t kPartChildren = 0x08, kPartParent = 0x18, kPartFlags = 0x2C;

struct Sites {
    bool done = false, ok = false, faulted = false;
    char why[200] = "not checked";
} g_sites;

bool Check(const char* name, std::uintptr_t rva, const char* sig) {
    if (mem::CodeMatches(mem::Base() + rva, sig)) return true;
    std::snprintf(g_sites.why, sizeof(g_sites.why), "%s at ds.exe+0x%llX does not match", name, static_cast<unsigned long long>(rva));
    return false;
}

using ModelBeginFn = void (*)(std::uintptr_t, unsigned);
using ModelEndFn = void (*)(std::uintptr_t);
using SetPartFn = void (*)(std::uintptr_t, unsigned long long, int);
using SetAllFn = void (*)(std::uintptr_t, unsigned char, char);

void CloseBracket(std::uintptr_t sm) {
    __try {
        reinterpret_cast<ModelEndFn>(mem::Base() + kModelEnd)(sm);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// idx < 0: every top-level part (the game's own call for the player's Root). Inside the model update
// bracket (the entity's critical section +0x288), which a fault must not leave held.
bool CallSet(std::uintptr_t sm, std::uintptr_t apd, int idx, bool vis) {
    const std::uintptr_t b = mem::Base();
    volatile bool open = false;
    __try {
        reinterpret_cast<ModelBeginFn>(b + kModelBegin)(sm, 1);
        open = true;
        if (idx < 0) reinterpret_cast<SetAllFn>(b + kApdSetAll)(apd, vis ? 1 : 0, 0);
        else reinterpret_cast<SetPartFn>(b + kApdSetPart)(apd, vis ? 1ull : 0ull, idx);
        open = false;
        reinterpret_cast<ModelEndFn>(b + kModelEnd)(sm);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_sites.faulted = true;
        if (open) CloseBracket(sm);
        return false;
    }
}

// The figure's model and parts, checked: the model is a SkinnedModel pointing back at the entity, the
// ArtPartsData (found by its type name) points back at the entity, a sane part count and array.
// The component search (an RTTI walk per component) runs once per figure; later calls re-check the
// remembered model and ArtPartsData with a few reads (vtables, back pointers, count, array).
struct ModelCache {
    std::uintptr_t e = 0, sm = 0, apd = 0, apd_vt = 0, arr = 0;
    std::int32_t n = 0;
} g_mc;
bool ModelAndParts(std::uintptr_t e, std::uintptr_t* sm, std::uintptr_t* apd, std::uintptr_t* arr, std::int32_t* n) {
    *sm = Q(e + kEntModel);
    if (!*sm || Q(*sm) != mem::Base() + kSmVtable || Q(*sm + kSmEntity) != e) return false;
    if (g_mc.e == e && g_mc.sm == *sm && g_mc.apd && Q(g_mc.apd) == g_mc.apd_vt && Q(g_mc.apd + 0x48) == e && Rd(g_mc.apd + 0x100, *n) &&
        *n == g_mc.n && Q(g_mc.apd + 0x108) == g_mc.arr) {
        *apd = g_mc.apd, *arr = g_mc.arr;
        return true;
    }
    g_mc = ModelCache{};
    *apd = partner::FindComponentByName(reinterpret_cast<game::Entity*>(e), "ArtPartsData");
    if (!*apd || Q(*apd + 0x48) != e || !Rd(*apd + 0x100, *n) || *n <= 0 || *n > 512) return false;
    *arr = Q(*apd + 0x108);
    if (!*arr) return false;
    g_mc.e = e, g_mc.sm = *sm, g_mc.apd = *apd, g_mc.apd_vt = Q(*apd), g_mc.arr = *arr, g_mc.n = *n;
    return true;
}

std::uintptr_t PoseSkel(std::uintptr_t sm) {
    std::int32_t idx = -1;
    if (!Rd(sm + kSmPoseIdx, idx) || (idx != 0 && idx != 1)) return 0;
    const std::uintptr_t skel = Q(sm + kSmPoses + static_cast<std::uintptr_t>(idx) * 0x70 + 0x20);
    std::int32_t count = 0;
    std::uint32_t layout = 0;
    if (!skel || !Rd(skel + 0x20, count) || !Rd(skel + 0x70, layout)) return 0;
    return count == 77 && layout == wire::kSamLayoutHash ? skel : 0;
}

bool TopLevel(std::uintptr_t part) {
    std::uintptr_t parent = 1;
    return part && Rd(part + kPartParent, parent) && parent == 0;
}

// The sites were verified (whether or not a call faulted since).
bool SitesVerified() { return g_sites.done && g_sites.ok; }

}  // namespace

bool SitesOk() noexcept {
    if (g_sites.done) return g_sites.ok && !g_sites.faulted;
    g_sites.done = true;
    const std::uintptr_t b = mem::Base();
    bool ok = Check("subspace work reader", kWorkReader, "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 05 E8") &&
              Check("subspace ticker", kTicker, "40 53 48 83 EC 40 48 89 7C 24 60 48 8D 99 C0 09 00 00 48 8B F9") &&
              Check("subspace ticker state reads", kTickerState, "8B 4F 1C 8B 90 4C 04 00 00 0F B6 87 C8 09 00 00") &&
              Check("subspace ticker state", kTickerState2, "8B 47 14 44 88 87 C8 09 00 00 3B C8") &&
              Check("subspace figure stores", kCreateStores, "48 89 43 50 48 8B 43 30") &&
              Check("subspace figure store", kCreateFigure, "48 89 4B 58 E8") &&
              Check("subspace open", kOpenKind, "0F B6 86 C8 09 00 00 24 DF 89 7E 0C 0C 10 89 7E 08") &&
              Check("AKukan constructor vtable store", kAkukanVtSite, "48 8D 05 ?? ?? ?? ?? 48 89 07") &&
              Check("ArtParts part setter", kApdSetPart, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 0F B6 41 38 0F B6 F2 49 63 D8") &&
              Check("ArtParts top-level setter", kApdSetAll, "48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30 48 8B 41 48") &&
              Check("model update begin", kModelBegin, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 79 48 48 8B D9 8B F2") &&
              Check("model update end", kModelEnd, "48 89 5C 24 10 57 48 83 EC 60 48 8B 05");
    if (ok && mem::RipTarget(b + kWorkReader, 3, 7) != b + kWork) {
        std::snprintf(g_sites.why, sizeof(g_sites.why), "the subspace work reader does not read ds.exe+0x%llX", static_cast<unsigned long long>(kWork));
        ok = false;
    }
    if (ok && mem::RipTarget(b + kAkukanVtSite, 3, 7) != b + kAkukanVtable) {
        std::snprintf(g_sites.why, sizeof(g_sites.why), "the AKukan constructor does not store ds.exe+0x%llX", static_cast<unsigned long long>(kAkukanVtable));
        ok = false;
    }
    if (ok && Q(b + kSmVtable) != b + kSmVtSlot0) {
        std::snprintf(g_sites.why, sizeof(g_sites.why), "SkinnedModel vtable does not match");
        ok = false;
    }
    g_sites.ok = ok;
    Log("[menu] cargo menu figure sites %s", ok ? "verified (subspace work object, figure, ArtParts setters, model bracket)" : g_sites.why);
    return ok;
}

const char* Why() noexcept { return g_sites.faulted ? "an ArtParts call faulted (off for this session)" : g_sites.why; }

bool Read(View* v) noexcept {
    *v = View{};
    if (!SitesOk()) return false;
    const std::uintptr_t w = Q(mem::Base() + kWork);
    if (!w) return false;
    v->w = w;
    // one read for the fields +0x08 .. +0x60 (kind, kind asked, state, asked state, weak ref, player,
    // figure), one for the flags
    std::uint8_t hdr[0x58];
    if (!mem::Read(w + kWKind, hdr, sizeof(hdr)) || !Rd(w + kWFlags, v->flags)) return false;
    auto at = [&](std::uintptr_t off, auto* out) { std::memcpy(out, hdr + (off - kWKind), sizeof(*out)); };
    at(kWState, &v->state), at(kWReq, &v->req), at(kWKind, &v->kind), at(kWKindNext, &v->kind_next);
    std::uintptr_t weak = 0, e = 0;
    at(kWPlayer, &v->player), at(kWWeak, &weak), at(kWFigure, &e);
    if (!e || Q(e) != mem::Base() + kAkukanVtable || weak != e + 0x20) return true;
    v->e = e;
    if (partner::ReadFlags(reinterpret_cast<game::Entity*>(e), &v->e_flags)) v->awake = (v->e_flags & game::kEntityFlagSleeping) == 0;
    // The model and parts only while the menu is opening, open or closing (idle in the field: no
    // per-tick component walk).
    if (v->state < 3 || v->state > 6) return true;
    std::uintptr_t sm = 0, apd = 0, arr = 0;
    std::int32_t n = 0;
    if (ModelAndParts(e, &sm, &apd, &arr, &n)) {
        v->sm = sm, v->apd = apd, v->arr = arr, v->parts = n;
        v->skel = PoseSkel(sm);
    }
    double p[3] = {};
    if (mem::Read(e + kEntWorld, p, sizeof(p))) std::memcpy(v->pos, p, sizeof(p));
    return true;
}

bool Open(const View& v) noexcept {
    // State 3 teleports and wakes the figure (it may still read asleep for that tick); 4 fades in, 5 is open.
    return v.w && (v.state == 3 || ((v.state == 4 || v.state == 5) && v.awake)) && v.e && v.sm && v.skel && v.apd && v.arr;
}

// Only Manage Cargo (kind 0), the one kind run in hidden tests; every other kind (terminal cargo
// screens 2 / 5 / 8, the private room's previews 6 / 7, ...) keeps Sam.
bool KindAllowed(const View& v) noexcept { return v.kind == 0 && v.kind_next == 0; }

bool Hide(const View& v, Hidden* h, char* why, std::size_t cap) noexcept {
    *h = Hidden{};
    if (!SitesOk()) {
        std::snprintf(why, cap, "%s", Why());
        return false;
    }
    std::uintptr_t sm = 0, apd = 0, arr = 0;
    std::int32_t n = 0;
    if (!v.e || !ModelAndParts(v.e, &sm, &apd, &arr, &n) || apd != v.apd || arr != v.arr) {
        std::snprintf(why, cap, "the figure's model / ArtParts do not check out");
        return false;
    }
    unsigned top = 0, shown = 0, listed = 0;
    for (std::int32_t i = 0; i < n; ++i) {
        const std::uintptr_t p = Q(arr + 8ull * static_cast<unsigned>(i));
        if (!TopLevel(p)) continue;
        if (top < 64) h->top_idx[top] = static_cast<std::uint32_t>(i);
        ++top;
        std::uint8_t fl = 0;
        if (!Rd(p + kPartFlags, fl) || !(fl & 1)) continue;
        ++shown;
        if (listed < 64) h->idx[listed++] = static_cast<std::uint32_t>(i);
    }
    if (top == 0) {
        std::snprintf(why, cap, "no top-level part among %d", n);
        return false;
    }
    h->all_shown = shown == top;
    if (!h->all_shown && shown > 64) {
        std::snprintf(why, cap, "%u of %u top-level parts shown (more than 64 to remember)", shown, top);
        return false;
    }
    if (!CallSet(sm, apd, -1, false)) {
        // it may have hidden some: put them back (Restore is tried even after a fault)
        Hidden partial = *h;
        partial.e = v.e, partial.apd = apd, partial.arr = arr, partial.parts = n, partial.top = top, partial.was_shown = shown;
        char r[120] = {};
        const bool back = Restore(&partial, r, sizeof(r));
        *h = Hidden{};
        std::snprintf(why, cap, "the ArtParts top-level setter faulted (%s)", back ? "parts put back" : r);
        return false;
    }
    h->e = v.e, h->apd = apd, h->arr = arr, h->parts = n, h->top = top, h->was_shown = shown;
    std::snprintf(why, cap, "%u top-level part(s) of %d hidden (%u were shown)", top, n, shown);
    return true;
}

unsigned Rehide(Hidden* h) noexcept {
    if (!h->e || !SitesOk()) return 0;
    std::uintptr_t sm = 0, apd = 0, arr = 0;
    std::int32_t n = 0;
    if (Q(h->e) != mem::Base() + kAkukanVtable || !ModelAndParts(h->e, &sm, &apd, &arr, &n) || apd != h->apd || arr != h->arr || n != h->parts) return 0;
    unsigned again = 0;
    if (h->top <= 64) {
        // only the top-level parts Hide found (the part array read once)
        static std::uintptr_t ptrs[512];
        if (!mem::Read(arr, ptrs, 8ull * static_cast<unsigned>(n))) return 0;
        for (unsigned k = 0; k < h->top; ++k) {
            const std::uint32_t i = h->top_idx[k];
            std::uint8_t fl = 0;
            if (i < static_cast<std::uint32_t>(n) && TopLevel(ptrs[i]) && Rd(ptrs[i] + kPartFlags, fl) && (fl & 3)) ++again;
        }
    } else {
        for (std::int32_t i = 0; i < n; ++i) {
            const std::uintptr_t p = Q(arr + 8ull * static_cast<unsigned>(i));
            std::uint8_t fl = 0;
            if (TopLevel(p) && Rd(p + kPartFlags, fl) && (fl & 3)) ++again;
        }
    }
    if (again && CallSet(sm, apd, -1, false)) ++h->rehides;
    return again;
}

bool Restore(Hidden* h, char* why, std::size_t cap) noexcept {
    const Hidden was = *h;
    *h = Hidden{};
    if (!was.e) {
        std::snprintf(why, cap, "nothing hidden");
        return true;
    }
    std::uintptr_t sm = 0, apd = 0, arr = 0;
    std::int32_t n = 0;
    // Tried even after an earlier call faulted (each call is SEH-guarded): a figure we hid is never
    // left hidden while it can be put back.
    if (!SitesVerified() || Q(was.e) != mem::Base() + kAkukanVtable || !ModelAndParts(was.e, &sm, &apd, &arr, &n) || apd != was.apd ||
        arr != was.arr || n != was.parts) {
        std::snprintf(why, cap, "the figure no longer checks out: not written");
        return false;
    }
    bool ok = true;
    if (was.all_shown) {
        ok = CallSet(sm, apd, -1, true);
    } else {
        const unsigned m = was.was_shown < 64 ? was.was_shown : 64;
        for (unsigned k = 0; k < m && ok; ++k) ok = CallSet(sm, apd, static_cast<int>(was.idx[k]), true);
    }
    std::snprintf(why, cap, "%s %u top-level part(s)%s", ok ? "put back" : "FAULTED putting back", was.was_shown,
                  was.all_shown ? " (all)" : "");
    return ok;
}

void LogParts(const View& v) noexcept {
    if (!v.e || !v.arr) {
        Log("[menu] parts: no figure");
        return;
    }
    std::uintptr_t sm = 0, apd = 0, arr = 0;
    std::int32_t n = 0;
    if (!ModelAndParts(v.e, &sm, &apd, &arr, &n)) {
        Log("[menu] parts: the figure's model / ArtParts do not check out");
        return;
    }
    unsigned top = 0, asked = 0, eff = 0;
    char line[400];
    std::size_t len = 0;
    line[0] = 0;
    for (std::int32_t i = 0; i < n; ++i) {
        const std::uintptr_t p = Q(arr + 8ull * static_cast<unsigned>(i));
        std::uint8_t fl = 0;
        std::int32_t kids = 0;
        if (!p || !Rd(p + kPartFlags, fl)) continue;
        Rd(p + kPartChildren, kids);
        asked += fl & 1;
        eff += (fl >> 1) & 1;
        const bool t = TopLevel(p);
        top += t;
        int w = std::snprintf(line + len, sizeof(line) - len, "%s%d:%c%c%s%d", len ? " " : "", i, (fl & 1) ? 'a' : '-', (fl & 2) ? 'e' : '-',
                              t ? "T" : "", kids);
        if (w > 0) len += static_cast<std::size_t>(w);
        if (len > 300 || i + 1 == n) {
            Log("[menu] parts %s", line);
            len = 0;
            line[0] = 0;
        }
    }
    Log("[menu] figure %p: %d ArtParts, %u top-level, %u asked visible, %u effectively visible (a = asked, e = effective, T = top-level, then children)",
        reinterpret_cast<void*>(v.e), n, top, asked, eff);
}

}  // namespace st::menufig
