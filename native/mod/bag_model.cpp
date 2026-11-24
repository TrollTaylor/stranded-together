#include "bag_model.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "log.hpp"
#include "partner_source.hpp"

namespace st::bagmodel {
namespace {

namespace mem = partner::mem;
using mem::Q;
using mem::Rd;

// ---------------------------------------------------------------- ds.exe (RVAs at base 0x140000000), static analysis round 14
// The baggage manager global and its config (gear_look.cpp checks the same site): mov rcx,[rip->mgr]; mov rdx,[rcx+8]; ...
constexpr std::uintptr_t kBagMgr = 0x7BF4968, kBagCfgSite = 0x26510E0;
// EntityResource (held_items.cpp, section 31.5): ctor vtable stores, GetRTTI, the entity type, the builder thunk.
constexpr std::uintptr_t kEntResVt = 0x3CF3990, kEntResVt2 = 0x3CF3A40, kEntResRtti = 0x4B02810, kEntityTypeGlobal = 0x4B01048;
constexpr std::uintptr_t kEntResCtorVt = 0x236CEF7, kEntResGetRtti = 0x22B7D90, kEntResType = 0x22B7D70;
constexpr std::uintptr_t kBuildThunk = 0x2366570, kBuilder = 0x2373940;
// Entity::Init 0x14234F3E0 (the components from the resource's +0x90 / +0x98; the model's ArtPartsData from 0x1423760C0, which
// finds the model in that list): their first bytes (the finder's up to its `cmp [rcx+0x90],ebx`).
constexpr std::uintptr_t kEntityInit = 0x234F3E0, kModelFinder = 0x23760C0;
// DSBaggage2Config (dsdc_types offsets; the data is checked on use): +0x180 EntityResource (Ref<DSBaggage2EntityResource>),
// +0x198 ModelResources (Array<Ref<SkinnedModelResource>>: i32 count, i32 capacity, +0x1A0 data), +0x300
// CargoStrandEntityResource (Ref<EntityResource>: the host; 3 components of its own at run time, none of them the model, so
// once restored the resource no longer names the baggage model: a lookup after Entity::Init through the entity's resource
// sees the strand's own list, section 39.4).
constexpr std::uintptr_t kCfgEntRes = 0x180, kCfgModels = 0x198, kCfgModelData = 0x1A0, kCfgHost = 0x300;
constexpr std::uintptr_t kRefCount = 0x18, kResComps = 0x90, kResCompData = 0x98, kResEntTab = 0xA0, kResRepTab = 0xD0, kTabCount = 0x20;
constexpr std::uintptr_t kEntModel = 0xB8, kEntRep = 0x60;
constexpr unsigned kModels = 31;
// Round 15 (section 39.6): the representation side. Entity::Init binds each resource component's rep through 0x1423491D0(comp,
// 1) (site 0x14234F833) -> 0x142370C40(entityRep, compRep, 1), which with flag 1 binds rep handlers ONLY from the resolved
// resource's prebuilt rep table (+0xD0, keyed by component+0x30). The strand's table is keyed by its own 3 components, so our
// model's SkinnedModelRep (and the extras' reps) got no handlers at all: no MsgEntityInit, no mesh entries, never drawn. The
// fix registers them with the game's own flag-0 code (0x142370D50 onwards, what every runtime AddComponent does): the handler
// set of the rep's class (cache 0x142380E50 -> 0x142371430) or, when not cached, the class's message table (0x1419F3550 with
// 0x144596930) each entry through 0x142371080, the array freed with 0x1419D5870. All of it on our own entity's dispatcher
// (EntityRep +0x208, under the entity's lock +0x288 unless the global 0x147BC4E7C says no locking), as the game does.
constexpr std::uintptr_t kRegSite = 0x2370D50, kRegNoLock = 0x7BC4E7C, kRegCached = 0x2380E50, kRegSet = 0x2371430, kRegTable = 0x19F3550,
                         kRegTableKind = 0x4596930, kRegOne = 0x2371080, kRegFree = 0x19D5870, kRepBindSite = 0x234F833, kRepBind = 0x23491D0,
                         kRepBindInner = 0x2370C40;
constexpr std::uintptr_t kCompRes = 0x30, kCompRep = 0x40, kCompRepComp = 0x20, kCompRepEntRep = 0x28, kRepEntity = 0x80, kRepDisp = 0x208,
                         kEntLock = 0x288, kEntComps = 0x90;
constexpr std::uintptr_t kDispHandlerCount = 0x10, kDispHandlers = 0x18, kRepSpan = 0x370, kModelRepEntries = 0x40;
bool g_bind = true;  // `gear 36 5 <0/1>`: register the reps' handlers (round 15; off = round 14's behaviour, for comparison)
std::uint32_t g_bound = 0, g_bind_skipped = 0, g_undrawn = 0;

struct Checks {
    bool done = false, ok = false;
    char why[200] = "not checked";
} g_chk;

struct Cfg {
    std::uintptr_t cfg = 0, host = 0;
    std::uintptr_t model[kModels] = {};
    bool ok = false;
    std::int32_t host_comps = -1;
} g_cfg;

std::uintptr_t* g_lists = nullptr;  // one 4-entry component list per model (the model, up to 3 extras; VirtualAlloc, pinned)
bool g_pinned[kModels] = {};
bool g_host_pinned = false;
bool g_off = false;
char g_off_why[200] = {};
std::uint32_t g_created = 0, g_refused = 0, g_faults = 0, g_per[kModels] = {};

bool Check(const char* name, std::uintptr_t rva, const char* sig) {
    if (mem::CodeMatches(mem::Base() + rva, sig)) return true;
    std::snprintf(g_chk.why, sizeof(g_chk.why), "%s at ds.exe+0x%llX does not match", name, static_cast<unsigned long long>(rva));
    return false;
}
bool Is(const char* name, std::uintptr_t got, std::uintptr_t want_rva) {
    if (got == mem::Base() + want_rva) return true;
    std::snprintf(g_chk.why, sizeof(g_chk.why), "%s is %p, not ds.exe+0x%llX", name, reinterpret_cast<void*>(got), static_cast<unsigned long long>(want_rva));
    return false;
}

bool RunChecks() {
    const std::uintptr_t b = mem::Base();
    return Check("baggage config site", kBagCfgSite, "48 8B 0D ?? ?? ?? ?? 48 8B 51 08 48 8B 8A B8 02 00 00") &&
           Is("baggage manager global", mem::RipTarget(b + kBagCfgSite, 3, 7), kBagMgr) &&
           Check("EntityResource ctor vtable stores", kEntResCtorVt, "48 8D 05 ?? ?? ?? ?? C7 41 38 FF FF FF FF 48 89 01 48 8D 05 ?? ?? ?? ?? 48 89 41 28") &&
           Is("EntityResource ctor vtable", mem::RipTarget(b + kEntResCtorVt, 3, 7), kEntResVt) &&
           Is("EntityResource ctor +0x28 vtable", mem::RipTarget(b + kEntResCtorVt + 17, 3, 7), kEntResVt2) &&
           Is("EntityResource vt+0x00", Q(b + kEntResVt), kEntResGetRtti) && Check("EntityResource GetRTTI", kEntResGetRtti, "48 8D 05 ?? ?? ?? ?? C3") &&
           Is("EntityResource RTTI", mem::RipTarget(b + kEntResGetRtti, 3, 7), kEntResRtti) &&
           Is("EntityResource vt+0x68", Q(b + kEntResVt + 0x68), kEntResType) && Check("EntityResource entity type", kEntResType, "48 8B 05 ?? ?? ?? ?? C3") &&
           Is("EntityResource entity type global", mem::RipTarget(b + kEntResType, 3, 7), kEntityTypeGlobal) &&
           Is("EntityResource vt+0x80", Q(b + kEntResVt + 0x80), kBuildThunk) && Check("entity builder thunk", kBuildThunk, "E9") &&
           Is("entity builder thunk target", mem::RipTarget(b + kBuildThunk, 1, 5), kBuilder) &&
           Check("entity builder", kBuilder,
                 "48 89 5C 24 18 48 89 6C 24 20 56 57 41 56 48 83 EC 40 48 8B 05 ?? ?? ?? ?? 48 8B EA 48 89 44 24 28 4C 8B F1 48 8D 05 ?? ?? ?? ?? 48 89 44 24 20 48 8B 01 FF 50 68") &&
           // the model finder: for i < [res+0x90]: [res+0x98][i] (0x1423760C0)
           Check("the resource's model finder", kModelFinder, "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 33 DB 48 8B F1 39 99 90 00 00 00 7E 5E") &&
           Check("Entity::Init", kEntityInit, "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D0 48 81 EC") &&
           // round 15: the rep binding (the cause site, flag 1) and the game's own flag-0 registration we call
           Check("Entity::Init's rep binding site", kRepBindSite, "48 8B 0B B2 01 E8") && Is("the rep binder", mem::RipTarget(b + kRepBindSite + 5, 1, 5), kRepBind) &&
           Check("the rep binder", kRepBind, "40 56 41 57 48 83 EC 38 48 8B 41 48 44 0F B6 FA") &&
           Check("the rep handler binder", kRepBindInner, "48 89 5C 24 20 55 56 57 48 83 EC 50") &&
           Check("the rep handler binder's runtime branch", kRegSite,
                 "38 1D ?? ?? ?? ?? 48 8D AE 08 02 00 00 48 8B F3 75 07 48 8D B2 88 02 00 00 48 8B 07 48 8B CF FF 10 48 8B C8 E8 ?? ?? ?? ?? 48 85 C0 74 1A "
                 "4C 8B C8 88 5C 24 20 4C 8B C7 48 8B D6 48 8B CD E8") &&
           Is("the no-lock global", mem::RipTarget(b + kRegSite, 2, 6), kRegNoLock) && Is("the handler set cache", mem::RipTarget(b + kRegSite + 0x24, 1, 5), kRegCached) &&
           Is("the handler set registration", mem::RipTarget(b + kRegSite + 0x3E, 1, 5), kRegSet) &&
           Check("the table walk", kRegSite + 0x65, "4C 8D 05 ?? ?? ?? ?? 48 8D 54 24 30 E8") &&
           Is("the message table kind", mem::RipTarget(b + kRegSite + 0x65, 3, 7), kRegTableKind) &&
           Is("the message table", mem::RipTarget(b + kRegSite + 0x71, 1, 5), kRegTable) && Check("the per-entry call", kRegSite + 0xA1, "E8") &&
           Is("the per-entry registration", mem::RipTarget(b + kRegSite + 0xA1, 1, 5), kRegOne) && Check("the free", kRegSite + 0xB7, "E8") &&
           Is("the array free", mem::RipTarget(b + kRegSite + 0xB7, 1, 5), kRegFree) &&
           Check("the handler set cache", kRegCached, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 8B 0D") &&
           Check("the handler set registration", kRegSet, "48 89 5C 24 10 48 89 6C 24 18 56 41 56 41 57 48 83 EC 30") &&
           Check("the per-entry registration", kRegOne, "4C 8B DC 56 48 83 EC 60 48 8B 05") &&
           Check("the message table", kRegTable, "48 89 5C 24 08 57 48 83 EC 20 80 79 04 04") && Check("the array free", kRegFree, "48 83 EC 28 4C 8B C1 48 85 C9 0F 84");
}

const char* TypeOf(std::uintptr_t obj) {
    const char* n = obj ? mem::RttiName(mem::RttiOf(obj)) : nullptr;
    return n ? n : "?";
}

bool AddRef(std::uintptr_t obj) {
    __try {
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(obj + kRefCount));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The config as the game has it now (read fault-free, every field checked).
bool ReadCfg(char* why, std::size_t cap) {
    const std::uintptr_t b = mem::Base();
    const std::uintptr_t mgr = Q(b + kBagMgr);
    const std::uintptr_t cfg = mgr ? Q(mgr + 8) : 0;
    if (!cfg) return std::snprintf(why, cap, "no baggage config yet"), false;
    if (g_cfg.ok && g_cfg.cfg == cfg) return true;
    g_cfg = Cfg{};
    if (std::strcmp(TypeOf(cfg), "DSBaggage2Config") != 0) return std::snprintf(why, cap, "the baggage manager's config %p is a %s", reinterpret_cast<void*>(cfg), TypeOf(cfg)), false;
    std::int32_t n = 0;
    const std::uintptr_t data = Q(cfg + kCfgModelData);
    if (!Rd(cfg + kCfgModels, n) || n != static_cast<std::int32_t>(kModels) || !data || !mem::Read(data, g_cfg.model, sizeof(g_cfg.model)))
        return std::snprintf(why, cap, "the config's ModelResources are not 31 readable entries (%d)", n), false;
    for (unsigned i = 0; i < kModels; ++i)
        if (std::strcmp(TypeOf(g_cfg.model[i]), "SkinnedModelResource") != 0)
            return std::snprintf(why, cap, "the config's model %u is a %s", i, TypeOf(g_cfg.model[i])), false;
    // cross-check: the config's own baggage entity resource lists ModelResources[1] (the small case) among its components
    const std::uintptr_t er = Q(cfg + kCfgEntRes);
    std::int32_t en = 0;
    bool found = false;
    if (std::strcmp(TypeOf(er), "DSBaggage2EntityResource") == 0 && Rd(er + kResComps, en) && en > 0 && en <= 64) {
        std::uintptr_t comps[64] = {};
        if (mem::Read(Q(er + kResCompData), comps, 8ull * static_cast<unsigned>(en)))
            for (std::int32_t i = 0; i < en; ++i) found = found || comps[i] == g_cfg.model[1];
    }
    if (!found) return std::snprintf(why, cap, "the config's ModelResources do not match its baggage entity resource (%s, %d components)", TypeOf(er), en), false;
    const std::uintptr_t host = Q(cfg + kCfgHost);
    std::int32_t hn = -1, t0 = -1, t1 = -1;
    if (!host || Q(host) != b + kEntResVt || Q(host + 0x28) != b + kEntResVt2 || std::strcmp(TypeOf(host), "EntityResource") != 0)
        return std::snprintf(why, cap, "the config's cargo strand resource %p is a %s (not a plain EntityResource)", reinterpret_cast<void*>(host), TypeOf(host)), false;
    if (!Rd(host + kResComps, hn) || hn < 0 || hn > 64 || !Rd(host + kResEntTab + kTabCount, t0) || t0 < 0 || t0 > 512 ||
        !Rd(host + kResRepTab + kTabCount, t1) || t1 < 0 || t1 > 512)
        return std::snprintf(why, cap, "the host resource's lists are unreadable (%d components, tables %d / %d)", hn, t0, t1), false;
    g_cfg.cfg = cfg;
    g_cfg.host = host;
    g_cfg.host_comps = hn;
    g_cfg.ok = true;
    Log("[gear] baggage models: config %p checks out (31 models; host EntityResource %p with %d component(s), message tables %d / %d)",
        reinterpret_cast<void*>(cfg), reinterpret_cast<void*>(host), hn, t0, t1);
    return true;
}

// Before each use (round 14 review: ReadCfg's full check runs once per config pointer): the config still names this host and
// this model, the host is still a plain EntityResource with its own component count, the model still a SkinnedModelResource.
bool CheckUse(unsigned model, char* why, std::size_t cap) {
    const std::uintptr_t b = mem::Base(), cfg = g_cfg.cfg, host = g_cfg.host, m = g_cfg.model[model];
    std::int32_t hn = -1;
    const std::uintptr_t data = Q(cfg + kCfgModelData);
    if (Q(cfg + kCfgHost) != host || !data || Q(data + 8ull * model) != m || std::strcmp(TypeOf(cfg), "DSBaggage2Config") != 0)
        return std::snprintf(why, cap, "the baggage config %p no longer names its host / model %u", reinterpret_cast<void*>(cfg), model), false;
    if (Q(host) != b + kEntResVt || Q(host + 0x28) != b + kEntResVt2 || !Rd(host + kResComps, hn) || hn != g_cfg.host_comps)
        return std::snprintf(why, cap, "the cargo strand resource %p changed (%s, %d components, %d checked)", reinterpret_cast<void*>(host), TypeOf(host), hn,
                             g_cfg.host_comps), false;
    if (std::strcmp(TypeOf(m), "SkinnedModelResource") != 0) return std::snprintf(why, cap, "the config's model %u is now a %s", model, TypeOf(m)), false;
    return true;
}

struct Saved {
    std::uint64_t count = 0, data = 0;
    bool have = false;
};
bool SwapIn(std::uintptr_t r, std::uintptr_t* list, unsigned n, Saved* sv) {
    __try {
        auto* q = reinterpret_cast<volatile std::uint64_t*>(r);
        sv->count = q[kResComps / 8], sv->data = q[kResCompData / 8];
        sv->have = true;
        q[kResComps / 8] = static_cast<std::uint64_t>(n) | (static_cast<std::uint64_t>(n) << 32);
        q[kResCompData / 8] = reinterpret_cast<std::uintptr_t>(list);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SwapOut(std::uintptr_t r, const Saved& sv) {
    if (!sv.have) return false;
    __try {
        auto* q = reinterpret_cast<volatile std::uint64_t*>(r);
        q[kResCompData / 8] = sv.data;
        q[kResComps / 8] = sv.count;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void TurnOff(const char* fmt, const char* a) {
    g_off = true;
    std::snprintf(g_off_why, sizeof(g_off_why), fmt, a);
    Log("[gear] %s", g_off_why);
}

// The handlers on an EntityRep's dispatcher (+0x208: +0x10 i32 count, +0x18 entries of 16 bytes, +0 (listener + offset) | 1)
// whose listener lies in a component rep [rep, rep + 0x370) and belongs to it: no other rep of the same entity (`others`, the
// entity rep and its components' reps; one dispatcher holds only that entity's listeners) starts between `comp_rep` and the
// listener, so a neighbouring rep's handlers are never taken for this one's (round 15 review). -1 unreadable.
int BoundTo(std::uintptr_t disp, std::uintptr_t comp_rep, const std::uintptr_t* others = nullptr, unsigned n_others = 0) {
    std::int32_t n = 0;
    const std::uintptr_t h = Q(disp + kDispHandlers);
    if (!Rd(disp + kDispHandlerCount, n) || n < 0 || n > 0x4000 || (n && !h)) return -1;
    int bound = 0;
    std::uint64_t buf[2 * 64];
    for (std::int32_t i = 0; i < n; i += 64) {
        const std::int32_t k = n - i < 64 ? n - i : 64;
        if (!mem::Read(h + 16ull * static_cast<unsigned>(i), buf, 16ull * static_cast<unsigned>(k))) return -1;
        for (std::int32_t j = 0; j < k; ++j) {
            const std::uint64_t l = buf[2 * j] & ~1ull;
            if (l < comp_rep || l >= comp_rep + kRepSpan) continue;
            bool nearer = false;
            for (unsigned o = 0; o < n_others && !nearer; ++o) nearer = others[o] > comp_rep && others[o] <= l;
            bound += !nearer;
        }
    }
    return bound;
}

struct TableArr {  // what 0x1419F3550 fills (the game's caller zeroes both quadwords first): i32 count, the 0x18-byte entries
    std::int32_t n = 0, cap = 0;
    std::uintptr_t data = 0;
};
using GetRttiFn = std::uintptr_t (*)(std::uintptr_t);
using CachedFn = std::uintptr_t (*)(std::uintptr_t);
using SetFn = void (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uint8_t);
using TableFn = void (*)(std::uintptr_t, TableArr*, std::uintptr_t);
using OneFn = void (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uint8_t);
using FreeFn = void (*)(std::uintptr_t);

// 0x142370D50's body for one component rep (flag 0, dedupe 0: the game's own arguments); false on a fault.
bool RegisterRep(std::uintptr_t disp, std::uintptr_t cs, std::uintptr_t comp_rep, bool* cached) {
    const std::uintptr_t b = mem::Base();
    __try {
        const std::uintptr_t get = Q(Q(comp_rep));
        if (!mem::InText(get)) return false;
        const std::uintptr_t rtti = reinterpret_cast<GetRttiFn>(get)(comp_rep);
        if (!rtti) return false;
        const std::uintptr_t set = reinterpret_cast<CachedFn>(b + kRegCached)(rtti);
        *cached = set != 0;
        if (set) {
            reinterpret_cast<SetFn>(b + kRegSet)(disp, cs, comp_rep, set, 0);
            return true;
        }
        TableArr arr{};
        reinterpret_cast<TableFn>(b + kRegTable)(rtti, &arr, b + kRegTableKind);
        for (std::int32_t i = 0; i < arr.n && arr.data; ++i) reinterpret_cast<OneFn>(b + kRegOne)(disp, cs, arr.data + 0x18ull * static_cast<unsigned>(i), comp_rep, 0);
        reinterpret_cast<FreeFn>(b + kRegFree)(arr.data);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Every component of the new entity built from our list (its resource +0x30 is one of `list`) whose rep has no handler on
// the entity's dispatcher gets its rep class's handlers. `note` gets "bound a+b..." per component. False on a fault / a
// layout that does not check out (the caller removes the entity).
bool BindReps(std::uintptr_t e, const std::uintptr_t* list, unsigned n, char* note, std::size_t cap) {
    note[0] = 0;
    const std::uintptr_t rep = Q(e + kEntRep);
    std::int32_t nc = 0;
    const std::uintptr_t data = Q(e + kEntComps + 8);
    if (!rep || Q(rep + kRepEntity) != e || !Rd(e + kEntComps, nc) || nc <= 0 || nc > 64 || !data)
        return std::snprintf(note, cap, "the entity's representation / components do not check out (rep %p, %d components)", reinterpret_cast<void*>(rep), nc), false;
    const std::uintptr_t disp = rep + kRepDisp;
    std::uint8_t nolock = 1;
    if (!Rd(mem::Base() + kRegNoLock, nolock)) return std::snprintf(note, cap, "the no-lock global is unreadable"), false;
    const std::uintptr_t cs = nolock == 0 ? e + kEntLock : 0;
    std::uintptr_t comps[64] = {};
    if (!mem::Read(data, comps, 8ull * static_cast<unsigned>(nc))) return std::snprintf(note, cap, "the component list is unreadable"), false;
    // every rep of the entity: the entity rep and each component's (the listeners' owners on this dispatcher)
    std::uintptr_t reps[65] = {rep};
    unsigned nreps = 1;
    for (std::int32_t i = 0; i < nc; ++i)
        if (const std::uintptr_t r = comps[i] ? Q(comps[i] + kCompRep) : 0) reps[nreps++] = r;
    for (std::int32_t i = 0; i < nc; ++i) {
        const std::uintptr_t c = comps[i], r = c ? Q(c + kCompRes) : 0;
        bool ours = false;
        for (unsigned k = 0; k < n; ++k) ours = ours || (r && r == list[k]);
        if (!ours) continue;
        const std::uintptr_t cr = Q(c + kCompRep);
        const char* t = TypeOf(c);
        if (!cr) {
            std::snprintf(note + std::strlen(note), cap - std::strlen(note), "%s%s no rep", note[0] ? "; " : "", t);
            continue;
        }
        if (Q(cr + kCompRepComp) != c || Q(cr + kCompRepEntRep) != rep)
            return std::snprintf(note, cap, "the %s's rep %p does not point back to it / the entity's rep", t, reinterpret_cast<void*>(cr)), false;
        const int before = BoundTo(disp, cr, reps, nreps);
        if (before < 0) return std::snprintf(note, cap, "the entity rep's handlers are unreadable"), false;
        if (before > 0 || !g_bind) {
            ++g_bind_skipped;
            std::snprintf(note + std::strlen(note), cap - std::strlen(note), "%s%s %d handler(s)%s", note[0] ? "; " : "", t, before,
                          g_bind ? " already" : " (binding off)");
            continue;
        }
        bool cached = false;
        if (!RegisterRep(disp, cs, cr, &cached)) return std::snprintf(note, cap, "registering the %s's rep handlers FAULTED", t), false;
        const int after = BoundTo(disp, cr, reps, nreps);
        ++g_bound;
        std::snprintf(note + std::strlen(note), cap - std::strlen(note), "%s%s rep handlers 0 -> %d (%s)", note[0] ? "; " : "", t, after,
                      cached ? "cached set" : "class table");
    }
    return true;
}

}  // namespace

bool Available(char* why, std::size_t cap) noexcept {
    if (!g_chk.done) {
        g_chk.done = true;
        g_chk.ok = RunChecks();
        if (g_chk.ok) std::snprintf(g_chk.why, sizeof(g_chk.why), "ok");
        Log("[gear] baggage models: %s", g_chk.ok ? "baggage config / entity resource / builder addresses verified (baggage models on)" : g_chk.why);
    }
    if (why && cap) std::snprintf(why, cap, "%s", g_off ? g_off_why : g_chk.why);
    return g_chk.ok && !g_off;
}

bool Create(unsigned model, const std::uintptr_t* extra, unsigned n_extra, const game::WorldTransform& at, game::Entity** out, char* why, std::size_t cap) noexcept {
    *out = nullptr;
    if (!Available(why, cap)) return false;
    if (model >= kModels) return std::snprintf(why, cap, "model %u out of range", model), false;
    if (!ReadCfg(why, cap)) {
        ++g_refused;
        return false;
    }
    if (!CheckUse(model, why, cap)) {
        g_cfg.ok = false;  // the next call reads and checks the config in full again
        ++g_refused;
        return false;
    }
    if (!g_lists) {
        g_lists = static_cast<std::uintptr_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!g_lists) return std::snprintf(why, cap, "no memory"), false;
    }
    const std::uintptr_t r = g_cfg.host, m = g_cfg.model[model];
    if (!g_host_pinned) g_host_pinned = AddRef(r);  // the host and the models stay alive for the process (never released)
    if (!g_pinned[model]) g_pinned[model] = AddRef(m);
    if (!g_host_pinned || !g_pinned[model]) {
        ++g_faults;
        TurnOff("%s", "a baggage model resource could not be pinned: baggage models off for this session");
        std::snprintf(why, cap, "%s", g_off_why);
        return false;
    }
    // the extra components (an animation that poses and bounds the model: the item's own DSSimpleAnimationComponentResource,
    // the weapons' generic ExternalAnimationComponentResource), each checked by type and pinned once
    unsigned n = 1;
    g_lists[4 * model] = m;
    for (unsigned k = 0; k < n_extra && k < 3; ++k) {
        const std::uintptr_t x = extra[k];
        const char* t = TypeOf(x);
        if (!x || (std::strcmp(t, "DSSimpleAnimationComponentResource") != 0 && std::strcmp(t, "ExternalAnimationComponentResource") != 0 &&
                   std::strcmp(t, "StencilBufferComponentResource") != 0))
            continue;
        static std::uintptr_t s_pinned[16] = {};
        bool pinned = false;
        for (std::uintptr_t q : s_pinned) pinned = pinned || q == x;
        for (std::uintptr_t& q : s_pinned) {
            if (pinned) break;
            if (!q) {
                if (AddRef(x)) q = x, pinned = true;
                break;
            }
        }
        if (pinned) g_lists[4 * model + n++] = x;
    }
    Saved sv{};
    if (!SwapIn(r, &g_lists[4 * model], n, &sv)) {
        SwapOut(r, sv);
        ++g_faults;
        return std::snprintf(why, cap, "the host resource could not be prepared"), false;
    }
    game::Entity* e = nullptr;
    const bool created = game::TryCreateEntity(reinterpret_cast<game::EntityResource*>(r), at, "st_bag_model", &e);
    const bool restored = SwapOut(r, sv);
    std::int32_t back = -1;
    if (!restored || !Rd(r + kResComps, back) || back != g_cfg.host_comps || Q(r) != mem::Base() + kEntResVt) {
        // off for the session, and the new entity (if any) removed: nothing is drawn from a call that left the game's resource
        // in doubt (round 14 review). Our list (g_lists) is never freed, so a strand resource left pointing at it stays valid.
        ++g_faults;
        const bool removed = created && e ? partner::Remove(e) : true;
        TurnOff("%s", removed ? "the cargo strand resource was not restored after a baggage model (its entity removed): baggage models off for this session"
                              : "the cargo strand resource was not restored after a baggage model (its entity NOT removed): baggage models off for this session");
        std::snprintf(why, cap, "%s", g_off_why);
        return false;
    }
    if (!created) {
        ++g_faults;
        TurnOff("creating a baggage model (%s) FAULTED: baggage models off for this session", model == 15 ? "weapon case" : "model");
        std::snprintf(why, cap, "%s", g_off_why);
        return false;
    }
    if (!e) {
        ++g_refused;
        return std::snprintf(why, cap, "CreateEntity gave no entity"), false;
    }
    // a plain Entity with the model and no sound; anything else is removed at once (before its first update) and baggage
    // models go off
    const auto ea = reinterpret_cast<std::uintptr_t>(e);
    const std::uintptr_t rep = Q(ea + kEntRep), sm = Q(ea + kEntModel);
    const char* rep_t = rep ? TypeOf(rep) : "none";
    const bool sound = partner::FindComponentByName(e, "SoundComponent") != 0;
    if (std::strcmp(TypeOf(ea), "Entity") != 0 || sound || !sm || std::strstr(rep_t, "Baggage") || std::strstr(rep_t, "Weapon")) {
        const bool removed = partner::Remove(e);
        char what[160];
        std::snprintf(what, sizeof(what), "a baggage model is a %s (representation %s%s%s): %s; baggage models off for this session", TypeOf(ea), rep_t,
                      sound ? ", with a sound" : "", sm ? "" : ", no model", removed ? "removed" : "NOT removed");
        TurnOff("%s", what);
        std::snprintf(why, cap, "%s", g_off_why);
        ++g_refused;
        return false;
    }
    // the representation side (round 15, section 39.6): the model's and extras' reps get their class's handlers
    char note[400];
    if (!BindReps(ea, &g_lists[4 * model], n, note, sizeof(note))) {
        ++g_faults;
        const bool removed = partner::Remove(e);
        char what[300];
        std::snprintf(what, sizeof(what), "a baggage model's reps: %s (%s): baggage models off for this session", note, removed ? "removed" : "NOT removed");
        TurnOff("%s", what);
        std::snprintf(why, cap, "%s", g_off_why);
        return false;
    }
    Log("[gear] baggage model %p reps: %s", static_cast<void*>(e), note);
    ++g_created;
    ++g_per[model];
    *out = e;
    std::snprintf(why, cap, "baggage model %p (%s, representation %s, model %s, %u extra component(s)) of the config's model %u", static_cast<void*>(e), TypeOf(ea),
                  rep_t, TypeOf(sm), n - 1, model);
    return true;
}

void SetBind(bool on) noexcept { g_bind = on; }
void NoteUndrawn() noexcept { ++g_undrawn; }
bool BindOn() noexcept { return g_bind; }

bool RepState(game::Entity* e, int* bound, int* entries) noexcept {
    *bound = -1, *entries = -1;
    const auto ea = reinterpret_cast<std::uintptr_t>(e);
    const std::uintptr_t sm = ea ? Q(ea + kEntModel) : 0, cr = sm ? Q(sm + kCompRep) : 0, rep = ea ? Q(ea + kEntRep) : 0;
    if (!cr || !rep || Q(cr + kCompRepComp) != sm) return false;
    std::uintptr_t reps[65] = {rep};
    unsigned nreps = 1;
    std::int32_t nc = 0;
    const std::uintptr_t data = Q(ea + kEntComps + 8);
    std::uintptr_t comps[64] = {};
    if (Rd(ea + kEntComps, nc) && nc > 0 && nc <= 64 && data && mem::Read(data, comps, 8ull * static_cast<unsigned>(nc)))
        for (std::int32_t i = 0; i < nc; ++i)
            if (const std::uintptr_t r = comps[i] ? Q(comps[i] + kCompRep) : 0) reps[nreps++] = r;
    *bound = BoundTo(rep + kRepDisp, cr, reps, nreps);
    std::int32_t n = -1;
    if (Rd(cr + kModelRepEntries, n)) *entries = n;
    return true;
}

void Info(char* msg, std::size_t cap) noexcept {
    char why[200] = {};
    const bool ok = Available(why, sizeof(why));
    std::snprintf(msg, cap,
                  "baggage models %s (%s): %u created (weapon case S %u, M %u, L %u, ladder %u), %u refused, %u faults; reps bound %u, %u skipped (binding %s); "
                  "%u undrawn (handlers, no meshes: removed)",
                  ok ? "on" : "OFF", why, g_created, g_per[15], g_per[16], g_per[17], g_per[14], g_refused, g_faults, g_bound, g_bind_skipped, g_bind ? "on" : "off",
                  g_undrawn);
}

}  // namespace st::bagmodel
