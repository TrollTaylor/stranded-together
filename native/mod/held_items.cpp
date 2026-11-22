#include "held_items.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "held_rules.hpp"
#include "log.hpp"
#include "partner_source.hpp"
#include "real_body.hpp"

namespace st::held {
namespace {

namespace mem = partner::mem;
using mem::Q;
using mem::Rd;

// ---------------------------------------------------------------- ds.exe (RVAs at base 0x140000000), static analysis round 6
// EntityResource (RTTI 0x144B02810, size 0x100; bases ResourceWithNameHash, StreamingRefTarget at +0x28):
//   ctor 0x14236CED0 stores the vtables 0x143CF3990 (+0x00) and 0x143CF3A40 (+0x28).
// DSWeaponEntityResource (RTTI 0x144BDE1F0, size 0x1A0): ctor 0x14290C2D0 runs the EntityResource ctor on a 0x100-byte base,
//   then stores 0x143D9C7A0 (+0x00) and 0x143D9C850 (+0x28).
// Both vtables' +0x80 (build the entity) is 0x142366570 = jmp 0x142373940, which allocates res->vt[+0x68]() (EntityResource:
//   0x1422B7D70, `mov rax,[Entity type]; ret`), then entity->vt[+0x28](entity, res) (components from res +0x90 / +0x98),
//   0x14235D1D0(entity, res[+0x48]) (+1 on that object's count) and 0x14235F5A0(entity, (u8) res[+0x40]).
constexpr std::uintptr_t kEntResVt = 0x3CF3990, kEntResVt2 = 0x3CF3A40;
constexpr std::uintptr_t kWeapResVt = 0x3D9C7A0, kWeapResVt2 = 0x3D9C850;
constexpr std::uintptr_t kEntResRtti = 0x4B02810, kEntityTypeGlobal = 0x4B01048;
constexpr std::uintptr_t kBuildThunk = 0x2366570, kBuilder = 0x2373940;
constexpr std::uintptr_t kEntResGetRtti = 0x22B7D90, kEntResType = 0x22B7D70;
constexpr std::uintptr_t kEntResCtorVt = 0x236CEF7, kWeapResCtorVt = 0x290C2EE, kWeapResCtorSize = 0x290C2D8;
// Entity::Init (Entity vt+0x28 = 0x14235DBB0) points the entity's resource reference (+0x58) at the resource through the
// streaming manager (0x141DDB370: the StreamingRefTarget's path +0x30 and the object UUID +0x08 pick the proxy, whose object
// is what the core registry holds under that path + UUID), and the builder's 0x142350A70 -> 0x142385D90 then makes the
// entity's representation from the RESOLVED resource's vt+0x70. So the resource must be R itself, seen as an EntityResource.
constexpr std::uintptr_t kRefCount = 0x18, kResComps = 0x90, kResCompData = 0x98;
// The resource's message-handler tables, built when it loaded (CreateEntity's 0x14234F3E0 copies them onto the new entity,
// 0x142376A50 / 0x142378E30): +0xA0 the entity's, +0xD0 its representation's; each {+0x00 groups, +0x10 message index,
// +0x20 i32 handler count, +0x28 handlers (0x20 bytes: +0x08 the listener, +0x10 the function)}. A weapon resource's hold
// DSWeaponEntity's and DSWeaponEntityRep's handlers (the thunks 0x14294E640..0x14294E6F0, 16 bytes apart: MsgEntityInit
// 0x14294E650 -> 0x14292D290 faulted in run 20261122-130107). A class's table holds only its most derived handler per message
// (the derived one calls its base itself), so the clone's copy gets, for each weapon handler, the plain class's handler of the
// same message (Entity's MsgEntityInit / MsgEntityUpdate, EntityRep's MsgEntityInit / MsgPrepareForRemoval: read from the RTTI
// message tables), or the game's empty handler 0x1418E9B10 (`ret 0`) where the plain class has none: a plain Entity and
// EntityRep with the kept components (run 20261122-132320: emptied without the base handlers, the clones were not drawn).
constexpr std::uintptr_t kResEntTab = 0xA0, kResRepTab = 0xD0, kTabCount = 0x20, kTabData = 0x28, kHandlerFn = 0x10, kHandlerSize = 0x20;
constexpr std::uintptr_t kWeapHandlerFirst = 0x294E640, kWeapHandlerLast = 0x294E6F0, kEmptyHandler = 0x18E9B10;
// Decima RTTI (compound types): +0x09 u8 message handler count, +0x78 the table ({message RTTI, handler} x count).
struct MsgClass {
    const char* name;
    std::uintptr_t rtti, table;
    std::uint8_t count;
};
constexpr MsgClass kWeapEntity = {"DSWeaponEntity", 0x4BDF5E0, 0x4BDFBE0, 9}, kWeapRep = {"DSWeaponEntityRep", 0x4BDF6A0, 0x4BDFC70, 3};
constexpr MsgClass kPlainEntity = {"Entity", 0x4AFCDB0, 0x4AFD580, 26}, kPlainRep = {"EntityRep", 0x4B02680, 0x4B0C8B0, 11};
struct Subst {
    std::uintptr_t from = 0, to = 0;
    char msg[40] = "";
};
Subst g_subst[16];
unsigned g_nsubst = 0;

struct Checks {
    bool done = false, ok = false;
    char why[200] = "not checked";
} g_chk;

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

// DSWeaponEntity's (9) and DSWeaponEntityRep's (3) message handler thunks: `jmp` each; the four classes' message tables as
// expected (count, table); each weapon handler mapped to the plain class's handler of the same message, or the empty one.
bool ClassOk(const MsgClass& c) {
    const std::uintptr_t b = mem::Base();
    std::uint8_t n = 0;
    if (!Rd(b + c.rtti + 0x09, n) || n != c.count || Q(b + c.rtti + 0x78) != b + c.table) {
        std::snprintf(g_chk.why, sizeof(g_chk.why), "%s's message table at ds.exe+0x%llX does not match (%u handlers)", c.name,
                      static_cast<unsigned long long>(c.rtti), n);
        return false;
    }
    return true;
}
std::uintptr_t HandlerOf(const MsgClass& c, std::uintptr_t msg) {
    const std::uintptr_t t = mem::Base() + c.table;
    for (unsigned i = 0; i < c.count; ++i)
        if (Q(t + 16ull * i) == msg) return Q(t + 16ull * i + 8);
    return 0;
}
bool WeaponThunksOk() {
    const std::uintptr_t b = mem::Base();
    for (std::uintptr_t a = kWeapHandlerFirst; a <= kWeapHandlerLast; a += 16)
        if (!Check("DSWeaponEntity message handler thunk", a, "E9 ?? ?? ?? ?? CC")) return false;
    if (!ClassOk(kWeapEntity) || !ClassOk(kWeapRep) || !ClassOk(kPlainEntity) || !ClassOk(kPlainRep)) return false;
    g_nsubst = 0;
    for (const MsgClass* wc : {&kWeapEntity, &kWeapRep}) {
        const MsgClass& plain = wc == &kWeapEntity ? kPlainEntity : kPlainRep;
        const std::uintptr_t t = b + wc->table;
        for (unsigned i = 0; i < wc->count && g_nsubst < sizeof(g_subst) / sizeof(g_subst[0]); ++i) {
            const std::uintptr_t msg = Q(t + 16ull * i), fn = Q(t + 16ull * i + 8);
            if (fn < b + kWeapHandlerFirst || fn > b + kWeapHandlerLast) {
                std::snprintf(g_chk.why, sizeof(g_chk.why), "%s's handler %u at %p is outside the known thunks", wc->name, i, reinterpret_cast<void*>(fn));
                return false;
            }
            Subst& sb = g_subst[g_nsubst++];
            sb.from = fn;
            const std::uintptr_t base = HandlerOf(plain, msg);
            sb.to = base ? base : b + kEmptyHandler;
            const char* mn = mem::RttiName(msg);
            std::snprintf(sb.msg, sizeof(sb.msg), "%s", mn ? mn : "?");
        }
    }
    char list[400] = {};
    for (unsigned i = 0; i < g_nsubst; ++i)
        std::snprintf(list + std::strlen(list), sizeof(list) - std::strlen(list), "%s%s %s", i ? ", " : "", g_subst[i].msg,
                      g_subst[i].to == b + kEmptyHandler ? "-" : "base");
    Log("[gear] held items: the weapon classes' message handlers on a clone: %s", list);
    return true;
}

bool RunChecks() {
    const std::uintptr_t b = mem::Base();
    return Check("EntityResource ctor vtable stores", kEntResCtorVt, "48 8D 05 ?? ?? ?? ?? C7 41 38 FF FF FF FF 48 89 01 48 8D 05 ?? ?? ?? ?? 48 89 41 28") &&
           Is("EntityResource ctor vtable", mem::RipTarget(b + kEntResCtorVt, 3, 7), kEntResVt) &&
           Is("EntityResource ctor +0x28 vtable", mem::RipTarget(b + kEntResCtorVt + 17, 3, 7), kEntResVt2) &&
           Check("DSWeaponEntityResource ctor base size", kWeapResCtorSize, "41 B8 00 01 00 00 48 8B D9 E8 ?? ?? ?? ?? 48 8B CB E8") &&
           Is("DSWeaponEntityResource ctor base ctor", mem::RipTarget(b + kWeapResCtorSize + 17, 1, 5), 0x236CED0) &&
           Check("DSWeaponEntityResource ctor vtable stores", kWeapResCtorVt, "48 8D 05 ?? ?? ?? ?? 48 89 03 48 8D 05 ?? ?? ?? ?? 48 89 43 28") &&
           Is("DSWeaponEntityResource ctor vtable", mem::RipTarget(b + kWeapResCtorVt, 3, 7), kWeapResVt) &&
           Is("DSWeaponEntityResource ctor +0x28 vtable", mem::RipTarget(b + kWeapResCtorVt + 10, 3, 7), kWeapResVt2) &&
           Is("EntityResource vt+0x00", Q(b + kEntResVt), kEntResGetRtti) && Check("EntityResource GetRTTI", kEntResGetRtti, "48 8D 05 ?? ?? ?? ?? C3") &&
           Is("EntityResource RTTI", mem::RipTarget(b + kEntResGetRtti, 3, 7), kEntResRtti) &&
           Is("EntityResource vt+0x68", Q(b + kEntResVt + 0x68), kEntResType) && Check("EntityResource entity type", kEntResType, "48 8B 05 ?? ?? ?? ?? C3") &&
           Is("EntityResource entity type global", mem::RipTarget(b + kEntResType, 3, 7), kEntityTypeGlobal) &&
           Is("EntityResource vt+0x80", Q(b + kEntResVt + 0x80), kBuildThunk) && Is("DSWeaponEntityResource vt+0x80", Q(b + kWeapResVt + 0x80), kBuildThunk) &&
           Check("empty message handler", kEmptyHandler, "C2 00 00 CC") && WeaponThunksOk() &&
           Check("entity builder thunk", kBuildThunk, "E9") && Is("entity builder thunk target", mem::RipTarget(b + kBuildThunk, 1, 5), kBuilder) &&
           Check("entity builder", kBuilder,
                 "48 89 5C 24 18 48 89 6C 24 20 56 57 41 56 48 83 EC 40 48 8B 05 ?? ?? ?? ?? 48 8B EA 48 89 44 24 28 4C 8B F1 48 8D 05 ?? ?? ?? ?? 48 89 44 24 20 48 8B 01 FF 50 68");
}

// ---------------------------------------------------------------- the private resources (one per weapon resource; never freed)
struct Clone {
    std::uintptr_t r = 0;           // the game's DSWeaponEntityResource (+1 held by us)
    std::uintptr_t* arr = nullptr;  // its filtered component list (VirtualAlloc, pinned)
    std::uintptr_t tab[2] = {};     // its entity / representation handler tables with the weapon's handlers emptied (pinned)
    unsigned emptied[2] = {};
    unsigned kept = 0, dropped = 0;
    std::uint8_t guid[16] = {};
    char path[96] = {};
    unsigned created = 0;
};
Clone g_clones[24];
unsigned g_nclones = 0;
std::uint32_t g_created = 0, g_not_owned = 0, g_refused = 0, g_faults = 0, g_approx = 0;
bool g_off = false;
char g_off_why[160] = {};

bool AddRef(std::uintptr_t obj) {
    __try {
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(obj + kRefCount));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const char* TypeOf(std::uintptr_t obj) {
    const char* n = obj ? mem::RttiName(mem::RttiOf(obj)) : nullptr;
    return n ? n : "?";
}

// Our Sam's weapon entity whose resource is (path, guid) (or, `any_guid`, the same path): its resource R.
std::uintptr_t FindResource(game::Entity* local, const char* path, const std::uint8_t (&guid)[16], bool any_guid) {
    const auto p = reinterpret_cast<std::uintptr_t>(local);
    if (!p) return 0;
    struct Node {
        std::uintptr_t e;
        int depth;
    };
    Node stack[256];
    int sp = 0;
    unsigned visited = 0;
    for (std::uintptr_t c = Q(p + 0x78), guard = 0; c && sp < 256 && guard < 128; c = Q(c + 0x80), ++guard) stack[sp++] = {c, 1};
    while (sp > 0 && visited < 400) {
        const Node nd = stack[--sp];
        ++visited;
        if (nd.depth < 3)
            for (std::uintptr_t c = Q(nd.e + 0x78), guard = 0; c && sp < 256 && guard < 64; c = Q(c + 0x80), ++guard) stack[sp++] = {c, nd.depth + 1};
        if (std::strcmp(TypeOf(nd.e), "DSWeaponEntity") != 0) continue;
        char wpath[128] = {};
        std::uint8_t g[16] = {};
        if (!realbody::ResourceIdentity(reinterpret_cast<game::Entity*>(nd.e), wpath, sizeof(wpath), g, nullptr) || std::strcmp(wpath, path) != 0) continue;
        if (!any_guid && std::memcmp(g, guid, 16) != 0) continue;
        game::EntityResource* r = nullptr;
        if (!game::TryGetResource(reinterpret_cast<game::Entity*>(nd.e), &r) || !r) continue;
        return reinterpret_cast<std::uintptr_t>(r);
    }
    return 0;
}

// R's filtered component list (made once per R, pinned: each kept component resource +1, never released).
Clone* CloneFor(std::uintptr_t r, const char* path, const std::uint8_t (&guid)[16], char* why, std::size_t cap) {
    for (unsigned i = 0; i < g_nclones; ++i)
        if (g_clones[i].r == r) return &g_clones[i];
    const std::uintptr_t b = mem::Base();
    if (Q(r) != b + kWeapResVt || Q(r + 0x28) != b + kWeapResVt2 || std::strcmp(TypeOf(r), "DSWeaponEntityResource") != 0) {
        std::snprintf(why, cap, "its resource %p is a %s (vtables %p / %p), not a DSWeaponEntityResource", reinterpret_cast<void*>(r), TypeOf(r),
                      reinterpret_cast<void*>(Q(r)), reinterpret_cast<void*>(Q(r + 0x28)));
        return nullptr;
    }
    if (g_nclones >= sizeof(g_clones) / sizeof(g_clones[0])) {
        std::snprintf(why, cap, "more than %u different held items this session", static_cast<unsigned>(sizeof(g_clones) / sizeof(g_clones[0])));
        return nullptr;
    }
    std::int32_t n = 0;
    if (!Rd(r + kResComps, n) || n <= 0 || n > 64) {
        std::snprintf(why, cap, "its resource's component list is unreadable (%d)", n);
        return nullptr;
    }
    const std::uintptr_t data = Q(r + kResCompData);
    std::uintptr_t comps[64] = {};
    if (!data || !mem::Read(data, comps, sizeof(std::uintptr_t) * static_cast<unsigned>(n))) {
        std::snprintf(why, cap, "its resource's component list is unreadable");
        return nullptr;
    }
    auto* arr = static_cast<std::uintptr_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!arr) {
        std::snprintf(why, cap, "no memory");
        return nullptr;
    }
    unsigned kept = 0, dropped = 0;
    char kept_s[400] = {}, drop_s[300] = {};
    for (std::int32_t i = 0; i < n; ++i) {
        const std::uintptr_t c = comps[i];
        const char* tn = TypeOf(c);
        const bool comp_res = c && mem::DerivesFrom(mem::RttiOf(c), "EntityComponentResource");
        if (comp_res && heldrules::KeepComponent(tn) && AddRef(c)) {
            arr[kept++] = c;
            std::snprintf(kept_s + std::strlen(kept_s), sizeof(kept_s) - std::strlen(kept_s), "%s%s", kept > 1 ? ", " : "", tn);
        } else {
            ++dropped;
            std::snprintf(drop_s + std::strlen(drop_s), sizeof(drop_s) - std::strlen(drop_s), "%s%s", dropped > 1 ? ", " : "", tn);
        }
    }
    if (!kept) {
        VirtualFree(arr, 0, MEM_RELEASE);
        std::snprintf(why, cap, "no drawable component in its resource (%d dropped: %s)", n, drop_s);
        return nullptr;
    }
    // the handler tables (entity, representation), copied with the weapon classes' handlers emptied
    std::uintptr_t tabs[2] = {};
    unsigned emptied[2] = {};
    for (int k = 0; k < 2; ++k) {
        const std::uintptr_t t = r + (k ? kResRepTab : kResEntTab);
        std::int32_t hn = 0;
        const std::uintptr_t hd = Q(t + kTabData);
        if (!Rd(t + kTabCount, hn) || hn < 0 || hn > 512 || (hn && !hd)) {
            VirtualFree(arr, 0, MEM_RELEASE);
            std::snprintf(why, cap, "its resource's message handler table %d is unreadable (%d)", k, hn);
            return nullptr;
        }
        if (!hn) continue;
        auto* cp = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kHandlerSize * static_cast<unsigned>(hn), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!cp || !mem::Read(hd, cp, kHandlerSize * static_cast<unsigned>(hn))) {
            if (cp) VirtualFree(cp, 0, MEM_RELEASE);
            VirtualFree(arr, 0, MEM_RELEASE);
            std::snprintf(why, cap, "its resource's message handler table %d could not be copied", k);
            return nullptr;
        }
        for (std::int32_t i = 0; i < hn; ++i) {
            std::uintptr_t fn = 0;
            std::memcpy(&fn, cp + kHandlerSize * static_cast<unsigned>(i) + kHandlerFn, 8);
            if (fn >= b + kWeapHandlerFirst && fn <= b + kWeapHandlerLast) {
                std::uintptr_t to = b + kEmptyHandler;
                for (unsigned m = 0; m < g_nsubst; ++m)
                    if (g_subst[m].from == fn) to = g_subst[m].to;
                std::memcpy(cp + kHandlerSize * static_cast<unsigned>(i) + kHandlerFn, &to, 8);
                ++emptied[k];
            }
        }
        tabs[k] = reinterpret_cast<std::uintptr_t>(cp);
    }
    AddRef(r);  // R stays alive for the process (the clones' resource; never released)
    Clone& cl = g_clones[g_nclones++];
    cl.r = r;
    cl.arr = arr;
    cl.kept = kept;
    cl.dropped = dropped;
    cl.tab[0] = tabs[0], cl.tab[1] = tabs[1];
    cl.emptied[0] = emptied[0], cl.emptied[1] = emptied[1];
    std::memcpy(cl.guid, guid, 16);
    std::snprintf(cl.path, sizeof(cl.path), "%s", path);
    Log("[gear] held clone '%s': resource %p: %u component(s) kept (%s), %u dropped (%s); weapon message handlers replaced by the plain class's: %u of the "
        "entity's, %u of its representation's",
        path, reinterpret_cast<void*>(r), kept, kept_s, dropped, dropped ? drop_s : "-", emptied[0], emptied[1]);
    return &cl;
}

// R seen as a plain EntityResource with the filtered component list for the one CreateEntity call (game thread), then exactly
// as it was. Everything the creation reads from the resource goes through R itself: the entity's resource reference resolves
// through R's streaming proxy to R (a copy with R's identity resolves to R too; one with its own identity resolves to
// nothing: run 20261122-125554 faulted in 0x142385D90), the entity representation type (vt+0x70: EntityResource's instead of
// DSWeaponEntity's update handlers, the 0x142933FA6 crash of run 20261122-125117) and the components (+0x90 / +0x98).
struct Saved {
    std::uintptr_t vt, vt2, data, tab[2];
    std::uint64_t count;
};
bool SwapIn(std::uintptr_t r, const Clone& cl, Saved* sv) {
    __try {
        auto* q = reinterpret_cast<volatile std::uint64_t*>(r);
        sv->vt = q[0], sv->vt2 = q[0x28 / 8], sv->count = q[kResComps / 8], sv->data = q[kResCompData / 8];
        sv->tab[0] = q[(kResEntTab + kTabData) / 8], sv->tab[1] = q[(kResRepTab + kTabData) / 8];
        if (cl.tab[0]) q[(kResEntTab + kTabData) / 8] = cl.tab[0];
        if (cl.tab[1]) q[(kResRepTab + kTabData) / 8] = cl.tab[1];
        const std::uintptr_t b = mem::Base();
        q[0] = b + kEntResVt;
        q[0x28 / 8] = b + kEntResVt2;
        q[kResComps / 8] = static_cast<std::uint64_t>(cl.kept) | (static_cast<std::uint64_t>(cl.kept) << 32);
        q[kResCompData / 8] = reinterpret_cast<std::uintptr_t>(cl.arr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SwapOut(std::uintptr_t r, const Saved& sv) {
    if (!sv.vt) return false;
    __try {
        auto* q = reinterpret_cast<volatile std::uint64_t*>(r);
        q[(kResEntTab + kTabData) / 8] = sv.tab[0];
        q[(kResRepTab + kTabData) / 8] = sv.tab[1];
        q[kResCompData / 8] = sv.data;
        q[kResComps / 8] = sv.count;
        q[0x28 / 8] = sv.vt2;
        q[0] = sv.vt;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool Available(char* why, std::size_t cap) noexcept {
    if (!g_chk.done) {
        g_chk.done = true;
        g_chk.ok = RunChecks();
        if (g_chk.ok) std::snprintf(g_chk.why, sizeof(g_chk.why), "ok");
        Log("[gear] held items: %s", g_chk.ok ? "entity resource / builder addresses verified (held clones on)" : g_chk.why);
    }
    if (why && cap) std::snprintf(why, cap, "%s", g_off ? g_off_why : g_chk.why);
    return g_chk.ok && !g_off;
}

bool Create(game::Entity* local, const char* path, const std::uint8_t (&guid)[16], const game::WorldTransform& at, game::Entity** out, bool* approx,
            char* why, std::size_t cap) noexcept {
    *out = nullptr;
    *approx = false;
    if (!Available(why, cap)) return false;
    std::uintptr_t r = FindResource(local, path, guid, false);
    if (!r) {
        r = FindResource(local, path, guid, true);
        *approx = r != 0;
    }
    if (!r) {
        ++g_not_owned;
        std::snprintf(why, cap, "not in our own inventory (no weapon entity of that resource under our Sam)");
        return false;
    }
    Clone* cl = CloneFor(r, path, guid, why, cap);
    if (!cl) {
        ++g_refused;
        return false;
    }
    game::Entity* e = nullptr;
    Saved sv{};
    if (!SwapIn(r, *cl, &sv)) {
        SwapOut(r, sv);
        ++g_faults;
        std::snprintf(why, cap, "its resource could not be prepared");
        return false;
    }
    const bool created = game::TryCreateEntity(reinterpret_cast<game::EntityResource*>(r), at, "st_held_item", &e);
    const bool restored = SwapOut(r, sv);
    if (!restored || Q(r) != mem::Base() + kWeapResVt || Q(r + 0x28) != mem::Base() + kWeapResVt2) {
        g_off = true;
        std::snprintf(g_off_why, sizeof(g_off_why), "the resource of '%s' was not restored after a held clone: held clones off for this session", path);
        Log("[gear] %s", g_off_why);
    }
    if (!created) {
        ++g_faults;
        g_off = true;
        std::snprintf(g_off_why, sizeof(g_off_why), "creating a held clone of '%s' FAULTED: held clones off for this session", path);
        Log("[gear] %s", g_off_why);
        std::snprintf(why, cap, "%s", g_off_why);
        return false;
    }
    if (!e) {
        ++g_refused;
        std::snprintf(why, cap, "CreateEntity gave no entity");
        return false;
    }
    // A plain Entity, without the weapon's sound (the filtered list was used) and with no representation of the weapon's:
    // removed at once, before its first update, and held clones go off otherwise.
    const auto ea = reinterpret_cast<std::uintptr_t>(e);
    const std::uintptr_t rep = Q(ea + 0x60);
    const char* rep_t = rep ? TypeOf(rep) : "none";
    const bool sound = partner::FindComponentByName(e, "SoundComponent") != 0;
    if (std::strcmp(TypeOf(ea), "Entity") != 0 || sound || std::strstr(rep_t, "Weapon")) {
        const bool removed = partner::Remove(e);
        g_off = true;
        std::snprintf(g_off_why, sizeof(g_off_why), "a held clone of '%s' is a %s (representation %s%s): %s; held clones off for this session", path,
                      TypeOf(ea), rep_t, sound ? ", with the weapon's sound" : "", removed ? "removed" : "NOT removed");
        Log("[gear] %s", g_off_why);
        std::snprintf(why, cap, "%s", g_off_why);
        ++g_refused;
        return false;
    }
    if (*approx) ++g_approx;
    ++g_created;
    ++cl->created;
    *out = e;
    std::snprintf(why, cap, "held clone %p (%s, representation %s) of resource %p%s", static_cast<void*>(e), TypeOf(ea), rep_t, reinterpret_cast<void*>(r),
                  *approx ? " (another level of the item: approximate)" : "");
    return true;
}

void Info(char* msg, std::size_t cap) noexcept {
    char why[200] = {};
    const bool ok = Available(why, sizeof(why));
    std::snprintf(msg, cap, "held clones %s (%s): %u resource(s) re-typed, %u entities created (%u approximate), %u not owned, %u refused, %u faults",
                  ok ? "on" : "OFF", why, g_nclones, g_created, g_approx, g_not_owned, g_refused, g_faults);
}

}  // namespace st::held
