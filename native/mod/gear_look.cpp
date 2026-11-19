#include "gear_look.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "log.hpp"
#include "partner_source.hpp"

namespace st::look {
namespace {

namespace mem = partner::mem;
using mem::Q;
using mem::Rd;

// ---------------------------------------------------------------- ds.exe (RVAs at base 0x140000000)
// Evidence: docs/real-partner-body.md section 13 (decompiles of 0x14284AA30, 0x1422BD420 / 8C0 / CC0,
// 0x1422B03C0, 0x142BD4100, 0x142C541D0 / 0x142C54270, 0x142922E80). Byte-checked once.
constexpr std::uintptr_t kGameSettings = 0x7BEB0F8;  // global: [it] = the item / accessory settings object
constexpr std::uintptr_t kAccSite = 0x284AB8C;       // mov rax,[rip->kGameSettings]; mov rcx,[rax+8]; mov rax,[rcx+0x358]
constexpr std::uintptr_t kItemSite = 0x26BC864;      // mov rcx,[rip->kGameSettings]; call 0x142922E80
constexpr std::uintptr_t kItemLookup = 0x2922E80;    // movsxd rax,[rcx+0x5D6A0] (item infos count; +0x5D6A8 data)
constexpr std::uintptr_t kBagMgr = 0x7BF4968;        // global: DSBaggage2Manager*
constexpr std::uintptr_t kBagCfgSite = 0x26510E0;    // mov rcx,[rip->kBagMgr]; mov rdx,[rcx+8]; mov rcx,[rdx+0x2B8]
constexpr std::uintptr_t kBagLookup = 0x2C541D0;     // (mgr, DSGameBaggageListItem*, u8 flag) -> ArtPartsVariationResource*
constexpr std::uintptr_t kBagLookupCall = 0x2C48B60; // mov r8d,[rdi+0x204]; mov rcx,rbx; shr r8d,5; and r8b,1 (the flag)
constexpr std::uintptr_t kPackVarSite = 0x2BD4126;   // cmp ebx,[rax+0x138] (DSBackpackEntityResource ArtPartsVariations count)
constexpr std::uintptr_t kPackVarSite2 = 0x2BD416D;  // mov rdx,[rax+0x140] (... data)
constexpr std::uintptr_t kPackCfgSite = 0x292553E;   // mov [rbx+0x5D6C0],rcx: the settings' DSBackpackConfig (RainCoverLv2/Lv3Apv +0x30/+0x38)
constexpr std::uintptr_t kCapFn = 0x284AA30;         // the cap colour: apd = [e+0x180]; comp = apd+0xF0
constexpr std::uintptr_t kSvLinkSite = 0x284AA8D;    // mov rcx,[rdi+0x48]; call create; mov [rax+0x80],rdi; mov [rdi+0xF0],rax
constexpr std::uintptr_t kSvEntries = 0x22BD92C;     // mov rbx,[rdi+0x58] (entries)
constexpr std::uintptr_t kSvCount = 0x22BD94F;       // movsxd rsi,[rdi+0x50] (count)
// Called by us (game thread):
constexpr std::uintptr_t kApplyApv = 0x22BC100;      // ApplyArtPartsVariation(entity, ArtPartsVariationResource*)
constexpr std::uintptr_t kRemoveComp = 0x235B910;    // remove a component from its entity
constexpr std::uintptr_t kSvCreate = 0x22B03C0;      // ArtPartsShaderVariableComponent* (entity): find or create
constexpr std::uintptr_t kSvAdd = 0x22BD420;         // (comp, u32* hash, f32* values, count, part)
constexpr std::uintptr_t kSvExists = 0x22BD8C0;      // bool (comp, u32* hash, part)
constexpr std::uintptr_t kSvSet = 0x22BDCC0;         // (comp, u32* hash, f32* values, count, part)
constexpr std::uintptr_t kApvRtti = 0x4AE3AF0, kSvRtti = 0x4AE4520;

constexpr std::uintptr_t kEntComps = 0x90, kEntModel = 0xB8, kEntApd = 0x180, kEntResRef = 0x58;
constexpr std::uintptr_t kCompOwner = 0x48;
constexpr std::uintptr_t kApvRes = 0x30, kApvApplied = 0x50;
constexpr std::uintptr_t kObjGuid = 0x08;
constexpr std::uintptr_t kApdOwner = 0x48, kApdSv = 0xF0, kApdCount = 0x100;
constexpr std::uintptr_t kSvApd = 0x80, kSvN = 0x50, kSvArr = 0x58;
constexpr std::size_t kSvEntry = 0x28;
// A vehicle's paint: the colour index shader variable (the cap's colour index too, section 13.1), set per ArtParts
// index (the roadster's parts 4 / 6, the trikes' 14 / 16, the trucks' 14 / 15 in the 100% save: run 20261118-162226).
constexpr std::uint32_t kVehicleColourHash = 0x3A3714EDu;
// Only the colour index is carried in the look (STV1 item 255). The wear 0x32D5AE44 (the timefall corrosion: 1.0 on the
// 100% save's rust-brown gold truck) and the lights 0x2B11751D / 0x0978E0A1 change while the vehicle drives: they go in
// every STM1 frame (version 3's state block; ReadVehicleState / SetVehicleVars, docs/real-partner-body.md section 22.12).
// Run 20261118-164037 had sent only the first 31 wear entries (STV1's cap): the copy's wheels turned rust-brown, its body
// stayed gold; with all 39 the copy is rust-brown (run 20261118-201916).
constexpr std::uint32_t kVehicleLookHashes[] = {kVehicleColourHash};

// SkinnedModel / Pose (real_body.cpp)
constexpr std::uintptr_t kSmVtable = 0x3D0ADB8, kSmEntity = 0x48, kSmPoses = 0x170, kSmPoseIdx = 0x250, kPoseSize = 0x70;
constexpr std::size_t kHdrSkel = 0x20, kHdrState = 0x28, kHdrLocal = 0x30, kHdrLocalEnd = 0x38;
constexpr std::uintptr_t kSkelCount = 0x20, kSkelJoints = 0x28, kSkelLayout = 0x70;

struct Fns {
    bool read_done = false, read_ok = false;
    bool apply_done = false, apply_ok = false, off = false;
    char why[160] = "not checked";
} g;

const char* TypeOf(std::uintptr_t obj) {
    const char* n = obj ? mem::RttiName(mem::RttiOf(obj)) : nullptr;
    return n ? n : "?";
}
bool IsType(std::uintptr_t obj, const char* name) { return obj && std::strcmp(TypeOf(obj), name) == 0; }
bool IsApv(std::uintptr_t obj) { return IsType(obj, "ArtPartsVariationResource"); }

bool Check(const char* name, std::uintptr_t rva, const char* sig) {
    if (mem::CodeMatches(mem::Base() + rva, sig)) return true;
    std::snprintf(g.why, sizeof(g.why), "%s at ds.exe+0x%llX does not match", name, static_cast<unsigned long long>(rva));
    return false;
}

std::string Guid(const std::uint8_t* g16) {
    char b[40];
    std::snprintf(b, sizeof(b), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", g16[3], g16[2], g16[1], g16[0], g16[5], g16[4],
                  g16[7], g16[6], g16[8], g16[9], g16[10], g16[11], g16[12], g16[13], g16[14], g16[15]);
    return b;
}

std::string CStr(std::uintptr_t p) {
    if (!p) return "";
    std::int32_t len = -1;
    char buf[100] = {};
    if (!Rd(p - 8, len) || len < 0 || len > 4096) return "";
    const std::size_t n = static_cast<std::size_t>(len) < 96 ? static_cast<std::size_t>(len) : 96;
    if (n && !mem::Read(p, buf, n)) return "";
    for (std::size_t i = 0; i < n; ++i)
        if (static_cast<unsigned char>(buf[i]) < 0x20 || static_cast<unsigned char>(buf[i]) > 0x7E) buf[i] = '?';
    return std::string(buf, n);
}
std::string ObjName(std::uintptr_t o) {
    if (!o || !mem::DerivesFrom(mem::RttiOf(o), "ResourceWithName")) return "";
    return CStr(Q(o + 0x20));
}

// A StreamingRef's object when loaded (0x141DDE990: box = [ref]; loaded when (i8)(box[1] >> 52) < 0;
// object = [box[0] + 0x20]).
std::uintptr_t RefObject(std::uintptr_t ref) {
    const std::uintptr_t box = Q(ref);
    std::uintptr_t blk[2] = {};
    if (!box || !mem::Read(box, blk, sizeof(blk)) || !blk[0]) return 0;
    if (static_cast<std::int8_t>(static_cast<std::uint8_t>(blk[1] >> 52)) >= 0) return 0;
    return Q(blk[0] + 0x20);
}

// The entity's own ArtPartsData, checked (+0x180, owner +0x48), and its part count.
bool EntityApd(std::uintptr_t e, std::uintptr_t* apd, std::int32_t* parts) {
    *apd = e ? Q(e + kEntApd) : 0;
    if (!*apd || Q(*apd + kApdOwner) != e || !IsType(*apd, "ArtPartsData")) return false;
    *parts = 0;
    return Rd(*apd + kApdCount, *parts) && *parts >= 0 && *parts <= 512;
}

// The applied ArtPartsVariation components of `e` (owner checked, resource an ArtPartsVariationResource).
struct Var {
    std::uintptr_t comp = 0, res = 0;
    std::uint8_t guid[16] = {};
    std::uint8_t applied = 0;
};
unsigned Variations(std::uintptr_t e, Var* out, unsigned cap, bool applied_only) {
    std::int32_t n = 0;
    if (!e || !Rd(e + kEntComps, n) || n <= 0 || n > 128) return 0;
    std::uintptr_t comps[128] = {};
    if (!mem::Read(Q(e + kEntComps + 8), comps, 8ull * static_cast<unsigned>(n))) return 0;
    unsigned k = 0;
    for (std::int32_t i = 0; i < n; ++i) {
        const std::uintptr_t c = comps[i];
        if (!c || Q(c + kCompOwner) != e) continue;
        const std::uintptr_t r = Q(c + kApvRes);
        if (!IsApv(r)) continue;
        std::uint8_t ap = 0;
        Rd(c + kApvApplied, ap);
        if (applied_only && !ap) continue;
        if (k < cap) {
            out[k].comp = c, out[k].res = r, out[k].applied = ap;
            if (!mem::Read(r + kObjGuid, out[k].guid, 16)) continue;
        }
        ++k;
    }
    return k < cap ? k : cap;
}

// The live shader-variable entries of `e` (ArtPartsData +0xF0, checked to point back at it).
struct SvEntry {
    std::uint64_t key;
    std::uint32_t hash;
    float v[4];
    std::uint8_t count, removed;
};
unsigned ShaderVars(std::uintptr_t e, SvEntry* out, unsigned cap, std::uintptr_t* comp_out = nullptr) {
    std::uintptr_t apd = 0;
    std::int32_t parts = 0;
    if (comp_out) *comp_out = 0;
    if (!EntityApd(e, &apd, &parts)) return 0;
    const std::uintptr_t sv = Q(apd + kApdSv);
    if (!sv || Q(sv + kSvApd) != apd || !IsType(sv, "ArtPartsShaderVariableComponent")) return 0;
    if (comp_out) *comp_out = sv;
    std::int32_t n = 0;
    if (!Rd(sv + kSvN, n) || n <= 0 || n > 256) return 0;
    const std::uintptr_t arr = Q(sv + kSvArr);
    static std::uint8_t buf[256 * kSvEntry];
    const unsigned m = static_cast<unsigned>(n);
    if (!arr || !mem::Read(arr, buf, m * kSvEntry)) return 0;
    unsigned k = 0;
    for (unsigned i = 0; i < m && k < cap; ++i) {
        const std::uint8_t* p = buf + i * kSvEntry;
        SvEntry& s = out[k];
        std::memcpy(&s.key, p, 8);
        std::memcpy(&s.hash, p + 8, 4);
        std::memcpy(s.v, p + 0xC, 16);
        s.count = p[0x1C];
        s.removed = p[0x20];
        ++k;
    }
    return k;
}

// ---------------------------------------------------------------- calls (game thread, SEH)
bool CallApply(std::uintptr_t e, std::uintptr_t res) {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, std::uintptr_t)>(mem::Base() + kApplyApv)(e, res);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallRemove(std::uintptr_t comp) {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t)>(mem::Base() + kRemoveComp)(comp);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallSvCreate(std::uintptr_t e, std::uintptr_t* out) {
    __try {
        *out = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(mem::Base() + kSvCreate)(e);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallSvExists(std::uintptr_t comp, std::uint32_t hash, int part, bool* out) {
    __try {
        std::uint32_t h = hash;
        *out = (reinterpret_cast<std::uint64_t (*)(std::uintptr_t, std::uint32_t*, int)>(mem::Base() + kSvExists)(comp, &h, part) & 0xFF) != 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallSvAddSet(std::uintptr_t comp, std::uint32_t hash, const float* v, int count, int part, bool add) {
    __try {
        std::uint32_t h = hash;
        float vals[4] = {v[0], v[1], v[2], v[3]};
        using Fn = void (*)(std::uintptr_t, std::uint32_t*, float*, int, int);
        reinterpret_cast<Fn>(mem::Base() + (add ? kSvAdd : kSvSet))(comp, &h, vals, count, part);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallBagLookup(std::uintptr_t mgr, std::uintptr_t li, std::uint8_t flag, std::uintptr_t* out) {
    __try {
        *out = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uint8_t)>(mem::Base() + kBagLookup)(mgr, li, flag);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteQ(std::uintptr_t at, std::uintptr_t v) {
    SIZE_T wrote = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(at), &v, sizeof(v), &wrote) && wrote == sizeof(v);
}

// ---------------------------------------------------------------- resource sources (receiver)
// GUID -> ArtPartsVariationResource* among what this game holds. Built lazily; rebuilt when a GUID is
// not found (at most every 10 s).
// Lifetime: only the resident tables (accessory, item infos, backpack config, baggage config: the
// game's settings / managers) are kept across our gear changing. Entries found through our own gear
// (its resource, its components' resources, its applied variations) carry their owner entity and are
// dropped and gathered again before any use once our gear list changed (SetOwnGear, every read).
// The prop's own components are never kept: they are read at the apply that needs them (the prop is
// alive then) and forgotten (a removed prop's resources may be freed).
// Scope: a look's variation is only taken from sources that belong to its item's kind (Allowed).
struct Found {
    std::uint8_t guid[16];
    std::uintptr_t res;
    std::uint8_t src;
    std::uintptr_t owner;  // the entity it was found through (0: a resident table)
};
enum Src : std::uint8_t { kSrcAcc, kSrcItem, kSrcPack, kSrcBag, kSrcOwn, kSrcScan, kSrcProp, kSrcPackCfg, kSrcComp, kSrcCount };
const char* const kSrcNames[kSrcCount] = {"accessory table", "item infos", "our backpack resource", "baggage config", "our own gear",
                                          "resource scan", "the prop's own components", "backpack config (rain cover)", "our gear's component resources"};
constexpr unsigned kMaxFound = 768;
Found g_found[kMaxFound];
unsigned g_found_n = 0;
double g_built_ms = -1e18;
std::uintptr_t g_own[160];
unsigned g_own_n = 0;
std::uint32_t g_own_gen = 0, g_built_gen = 0xFFFFFFFFu;  // SetOwnGear changes / the list the map was built with
std::uint32_t g_src_hits[kSrcCount] = {};

bool Resident(std::uint8_t src) { return src == kSrcAcc || src == kSrcItem || src == kSrcPackCfg || src == kSrcBag; }
std::uintptr_t g_add_owner = 0;  // the owner AddFound records (set around each non-resident source)

void AddFound(std::uintptr_t r, Src src) {
    if (!IsApv(r)) return;
    std::uint8_t gd[16];
    if (!mem::Read(r + kObjGuid, gd, 16)) return;
    const std::uintptr_t owner = Resident(src) ? 0 : g_add_owner;
    // one entry per GUID among the resident tables (the first table is credited) and one per GUID,
    // source and owner otherwise (the same GUID through another source stays a separate entry, so the
    // per-kind scope can still find it; tables come first, so Find credits them)
    for (unsigned i = 0; i < g_found_n; ++i)
        if (std::memcmp(g_found[i].guid, gd, 16) == 0 &&
            ((Resident(g_found[i].src) && Resident(src)) || (g_found[i].src == src && g_found[i].owner == owner)))
            return;
    if (g_found_n >= kMaxFound) return;
    Found& f = g_found[g_found_n++];
    std::memcpy(f.guid, gd, 16);
    f.res = r;
    f.src = src;
    f.owner = owner;
}

// Variation resources referenced by `obj` (its first `bytes`): direct pointers and Decima arrays
// {u32 count, u32 capacity, T* data}. Fault-free; `depth` 1 also looks one level into referenced
// resources.
void ScanApv(std::uintptr_t obj, std::size_t bytes, Src src, int depth) {
    if (!obj || bytes > 0x800) return;
    std::uint8_t buf[0x800];
    if (!mem::Read(obj, buf, bytes)) return;
    for (std::size_t k = 0; k + 8 <= bytes; k += 8) {
        std::uintptr_t v = 0;
        std::memcpy(&v, buf + k, 8);
        if (v > 0x10000 && (v & 7) == 0) {
            const std::uintptr_t rt = mem::RttiOf(v);
            if (rt) {
                const char* tn = mem::RttiName(rt);
                if (tn && std::strcmp(tn, "ArtPartsVariationResource") == 0) AddFound(v, src);
                else if (depth > 0 && mem::DerivesFrom(rt, "Resource")) ScanApv(v, 0x200, src, depth - 1);
            }
        }
        if (k + 16 <= bytes) {
            std::uint32_t cnt = 0, capa = 0;
            std::uintptr_t data = 0;
            std::memcpy(&cnt, buf + k, 4), std::memcpy(&capa, buf + k + 4, 4), std::memcpy(&data, buf + k + 8, 8);
            if (cnt >= 1 && cnt <= 96 && capa >= cnt && capa <= 4096 && data > 0x10000 && (data & 7) == 0) {
                std::uintptr_t el[96];
                if (mem::Read(data, el, 8ull * cnt)) {
                    const std::uintptr_t first = mem::RttiOf(el[0]);
                    const char* tn = first ? mem::RttiName(first) : nullptr;
                    if (tn && std::strcmp(tn, "ArtPartsVariationResource") == 0)
                        for (unsigned i = 0; i < cnt; ++i) AddFound(el[i], src);
                }
            }
        }
    }
}

std::uintptr_t Settings() { return Q(mem::Base() + kGameSettings); }

void SourcesAccessory() {
    const std::uintptr_t s = Settings();
    const std::uintptr_t acc = s ? Q(Q(s + 8) + 0x358) : 0;
    if (!acc) return;
    for (std::uintptr_t off = 0x40; off <= 0x68; off += 8) AddFound(Q(acc + off), kSrcAcc);
    ScanApv(acc, 0x200, kSrcAcc, 0);
}
void SourcesItems() {
    const std::uintptr_t s = Settings();
    std::int32_t n = 0;
    if (!s || !Rd(s + 0x5D6A0, n) || n <= 0 || n > 4096) return;
    const std::uintptr_t arr = Q(s + 0x5D6A8);
    for (std::int32_t i = 0; i < n && arr; ++i) {
        const std::uintptr_t info = Q(arr + 8ull * static_cast<unsigned>(i));
        std::int32_t type = 0;
        if (!info || !Rd(info + 0x38, type) || type != 2) continue;
        const std::uintptr_t v = Q(info + 0x40);
        if (!v) continue;
        AddFound(Q(v), kSrcItem);
        AddFound(Q(v + 8), kSrcItem);
    }
}
void SourcesPack(std::uintptr_t pack) {
    const std::uintptr_t res = pack ? RefObject(pack + kEntResRef) : 0;
    if (!res) return;
    g_add_owner = pack;
    std::int32_t n = 0;
    if (Rd(res + 0x138, n) && n > 0 && n <= 64) {
        const std::uintptr_t data = Q(res + 0x140);
        for (std::int32_t i = 0; i < n && data; ++i) AddFound(Q(data + 8ull * static_cast<unsigned>(i)), kSrcPack);
    }
    ScanApv(res, 0x400, kSrcScan, 1);
    g_add_owner = 0;
}
void SourcesBaggage() {
    const std::uintptr_t mgr = Q(mem::Base() + kBagMgr);
    const std::uintptr_t cfg = mgr ? Q(mgr + 8) : 0;
    std::int32_t n = 0;
    if (cfg && Rd(cfg + 0x2B0, n) && n > 0 && n <= 128) {
        const std::uintptr_t data = Q(cfg + 0x2B8);
        for (std::int32_t i = 0; i < n && data; ++i) AddFound(Q(data + 8ull * static_cast<unsigned>(i)), kSrcBag);
    }
    std::int32_t m = 0;
    if (mgr && Rd(mgr + 0x46DFB0, m) && m > 0 && m <= 256) {
        const std::uintptr_t data = Q(mgr + 0x46DFB8);
        for (std::int32_t i = 0; i < m && data; ++i) {
            const std::uintptr_t a = Q(data + 8ull * static_cast<unsigned>(i));
            if (a) AddFound(Q(a + 0x40), kSrcBag);
        }
    }
}
void SourcesOwn() {
    for (unsigned i = 0; i < g_own_n; ++i) {
        Var v[8];
        const unsigned k = Variations(g_own[i], v, 8, false);
        g_add_owner = g_own[i];
        for (unsigned j = 0; j < k; ++j) AddFound(v[j].res, kSrcOwn);
    }
    g_add_owner = 0;
}
// Each entity's own component resources (DSArmorPlateComponentResource.ArtPartsVariations, the hanging
// boots', the pod's ...): every component's first pointers that are resources, scanned.
// EntityComponent +0x30 is its resource (dsdc_types: EntityComponent.Resource); an
// EntityComponentResource holds e.g. DSArmorPlateComponentResource.ArtPartsVariations (+0x28),
// DSPlayerHangedBootsComponentResource.ArtPartsVariations (+0x20), DSPlayerInventoryComponentResource's
// Odradek / canteen / cuffs variations (+0x2A8..).
void SourcesComponents(std::uintptr_t e, Src src) {
    std::int32_t n = 0;
    if (!e || !Rd(e + kEntComps, n) || n <= 0 || n > 64) return;
    std::uintptr_t comps[64] = {};
    if (!mem::Read(Q(e + kEntComps + 8), comps, 8ull * static_cast<unsigned>(n))) return;
    g_add_owner = e;
    for (std::int32_t i = 0; i < n; ++i) {
        const std::uintptr_t c = comps[i];
        if (!c || Q(c + kCompOwner) != e) continue;
        const std::uintptr_t r = Q(c + 0x30);
        if (!r || IsApv(r) || !mem::DerivesFrom(mem::RttiOf(r), "EntityComponentResource")) continue;
        ScanApv(r, 0x300, src, 0);
    }
    g_add_owner = 0;
}
void SourcesPackConfig() {
    const std::uintptr_t s = Settings();
    const std::uintptr_t bc = s ? Q(s + 0x5D6C0) : 0;
    if (!bc || !IsType(bc, "DSBackpackConfig")) return;
    AddFound(Q(bc + 0x30), kSrcPackCfg);
    AddFound(Q(bc + 0x38), kSrcPackCfg);
}
std::uintptr_t g_player_own = 0;  // our player (its inventory component's resource is a source)
bool g_no_own_applied = false;    // DumpSources: what the tables find without our gear's applied variations
std::uintptr_t g_pack_own = 0;  // our backpack (SetOwnGear's first DSBackpackEntity)

std::uint32_t g_builds = 0, g_own_builds = 0;
// Our own gear's sources only (the resident entries kept): after our gear list changed.
void BuildOwn() {
    unsigned k = 0;
    for (unsigned i = 0; i < g_found_n; ++i)
        if (Resident(g_found[i].src)) g_found[k++] = g_found[i];
    g_found_n = k;
    g_built_gen = g_own_gen;
    SourcesPack(g_pack_own);
    for (unsigned i = 0; i < g_own_n; ++i) SourcesComponents(g_own[i], kSrcComp);
    if (g_player_own) SourcesComponents(g_player_own, kSrcComp);
    if (!g_no_own_applied) SourcesOwn();  // last: a variation the tables hold is credited to them
    ++g_own_builds;
}
void Build(double now) {
    LARGE_INTEGER t0, t1, qf;
    QueryPerformanceCounter(&t0);
    g_found_n = 0;
    g_built_ms = now;
    SourcesAccessory();
    SourcesItems();
    SourcesPackConfig();
    SourcesBaggage();
    BuildOwn();
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&qf);
    if (++g_builds <= 20)
        Log("[look] variation sources gathered: %u resources in %.1f ms (game thread)", g_found_n,
            static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(qf.QuadPart));
}

double NowMs() { return static_cast<double>(GetTickCount64()); }

struct Miss {
    std::uint8_t guid[16];
    double next_ms;
};
Miss g_miss[64];
unsigned g_miss_n = 0;
std::uint32_t g_unresolved_logged = 0;

const Found* Find(const std::uint8_t* gd) {
    for (unsigned i = 0; i < g_found_n; ++i)
        if (std::memcmp(g_found[i].guid, gd, 16) == 0) return &g_found[i];
    return nullptr;
}

// Which sources a look of `kind` may take a variation from: a game entity's (the cap, glasses,
// shields, the rain cover ...) from the accessory / item tables, the backpack config (rain cover
// levels), our player's components, our own gear entity of the same resource and the prop's own
// components; the backpack's from the backpack resource / config and our backpack; a case's from
// the baggage config. Never another item's own gear.
bool SameResource(std::uintptr_t a, std::uintptr_t b) {
    const std::uintptr_t ra = a ? RefObject(a + kEntResRef) : 0;
    return ra && ra == (b ? RefObject(b + kEntResRef) : 0);
}
// An entity carrying a DSPlayerBootsComponent (our boots entity, inventory +0x4D80).
bool IsBootsEntity(std::uintptr_t e) {
    std::int32_t n = 0;
    if (!e || !Rd(e + kEntComps, n) || n <= 0 || n > 64) return false;
    std::uintptr_t comps[64] = {};
    if (!mem::Read(Q(e + kEntComps + 8), comps, 8ull * static_cast<unsigned>(n))) return false;
    for (std::int32_t i = 0; i < n; ++i)
        if (comps[i] && mem::DerivesFrom(mem::RttiOf(comps[i]), "DSPlayerBootsComponent")) return true;
    return false;
}
bool Allowed(const Found& f, wire::GearKind kind, std::uintptr_t prop) {
    switch (kind) {
    case wire::GearKind::entity:
        if (f.src == kSrcAcc || f.src == kSrcItem || f.src == kSrcPackCfg || f.src == kSrcProp) return true;
        if (f.src == kSrcComp || f.src == kSrcOwn) return f.owner && (f.owner == g_player_own || SameResource(f.owner, prop));
        return false;
    case wire::GearKind::boots:
        // the equipped boots (section 21): the prop's own, our player's and our own boots entity's components
        if (f.src == kSrcItem || f.src == kSrcProp) return true;
        if (f.src == kSrcComp || f.src == kSrcOwn)
            return f.owner && (f.owner == g_player_own || SameResource(f.owner, prop) || IsBootsEntity(f.owner));
        return false;
    case wire::GearKind::backpack:
        if (f.src == kSrcPackCfg || f.src == kSrcProp) return true;
        if (f.src == kSrcPack || f.src == kSrcScan || f.src == kSrcComp || f.src == kSrcOwn) return f.owner && f.owner == g_pack_own;
        return false;
    case wire::GearKind::cargo:
        return f.src == kSrcBag || f.src == kSrcProp;
    default:
        return false;
    }
}
// The first entry of a GUID whose source the item's kind allows (`refused`: one was found that it
// does not allow).
const Found* FindFor(const std::uint8_t* gd, wire::GearKind kind, std::uintptr_t prop, bool* refused) {
    for (unsigned i = 0; i < g_found_n; ++i) {
        if (std::memcmp(g_found[i].guid, gd, 16) != 0) continue;
        if (Allowed(g_found[i], kind, prop)) return &g_found[i];
        *refused = true;
    }
    return nullptr;
}
bool StillGuid(std::uintptr_t r, const std::uint8_t* gd) {
    std::uint8_t now_g[16];
    return IsApv(r) && mem::Read(r + kObjGuid, now_g, 16) && std::memcmp(now_g, gd, 16) == 0;
}
std::uint32_t g_refused = 0;
// A GUID left out for now (not asked for again for 15 s; Satisfied does not wait for it).
void Missed(const std::uint8_t* gd, double now) {
    Miss* m = nullptr;
    for (unsigned i = 0; i < g_miss_n; ++i)
        if (std::memcmp(g_miss[i].guid, gd, 16) == 0) m = &g_miss[i];
    if (!m && g_miss_n < 64) {
        m = &g_miss[g_miss_n++];
        std::memcpy(m->guid, gd, 16);
    }
    if (m) m->next_ms = now + 15000;
}
// The resource of a GUID for a look of `kind` on `prop`, or 0. The prop's own components are a
// source too (read now, not kept).
// Vehicle scope (ApplyVehicle): the copy's own component resources and our own vehicles of the sender's resource.
bool g_veh_scope = false;
const std::uintptr_t* g_veh_own = nullptr;
unsigned g_veh_own_n = 0;
std::uint32_t g_veh_resolved = 0, g_veh_unresolved = 0;
// The variation resource with GUID gd that `obj` (its first `bytes`) refers to: direct references and Decima arrays.
std::uintptr_t FindApvIn(std::uintptr_t obj, std::size_t bytes, const std::uint8_t* gd) {
    static std::uint8_t buf[0x400];
    if (!obj || bytes > sizeof(buf) || !mem::Read(obj, buf, bytes)) return 0;
    for (std::size_t k = 0; k + 8 <= bytes; k += 8) {
        std::uintptr_t v = 0;
        std::memcpy(&v, buf + k, 8);
        if (v > 0x10000 && (v & 7) == 0 && StillGuid(v, gd)) return v;
        if (k + 16 <= bytes) {
            std::uint32_t cnt = 0, capa = 0;
            std::uintptr_t data = 0;
            std::memcpy(&cnt, buf + k, 4), std::memcpy(&capa, buf + k + 4, 4), std::memcpy(&data, buf + k + 8, 8);
            if (cnt >= 1 && cnt <= 96 && capa >= cnt && capa <= 4096 && data > 0x10000 && (data & 7) == 0) {
                std::uintptr_t el[96];
                if (mem::Read(data, el, 8ull * cnt) && IsApv(el[0]))
                    for (unsigned i = 0; i < cnt; ++i)
                        if (StillGuid(el[i], gd)) return el[i];
            }
        }
    }
    return 0;
}
std::uintptr_t ResolveVehicle(const std::uint8_t* gd, std::uintptr_t prop) {
    // 1. the copy's own component resources (the trike copy lists its own colour variation there)
    std::int32_t n = 0;
    if (prop && Rd(prop + kEntComps, n) && n > 0 && n <= 64) {
        std::uintptr_t comps[64] = {};
        if (mem::Read(Q(prop + kEntComps + 8), comps, 8ull * static_cast<unsigned>(n)))
            for (std::int32_t i = 0; i < n; ++i) {
                const std::uintptr_t c = comps[i];
                if (!c || Q(c + kCompOwner) != prop) continue;
                const std::uintptr_t r = Q(c + 0x30);
                if (StillGuid(r, gd)) return ++g_veh_resolved, r;
                if (!r || !mem::DerivesFrom(mem::RttiOf(r), "EntityComponentResource")) continue;
                if (const std::uintptr_t f = FindApvIn(r, 0x300, gd)) return ++g_veh_resolved, f;
            }
    }
    // 2. our own vehicles of the sender's resource: their resource's references, their variation components
    for (unsigned i = 0; i < g_veh_own_n && g_veh_own; ++i) {
        const std::uintptr_t e = g_veh_own[i];
        if (const std::uintptr_t f = FindApvIn(e ? RefObject(e + kEntResRef) : 0, 0x400, gd)) return ++g_veh_resolved, f;
        Var v[8];
        const unsigned k = Variations(e, v, 8, false);
        for (unsigned j = 0; j < k; ++j)
            if (std::memcmp(v[j].guid, gd, 16) == 0 && StillGuid(v[j].res, gd)) return ++g_veh_resolved, v[j].res;
    }
    ++g_veh_unresolved;
    Missed(gd, NowMs());
    if (g_veh_unresolved <= 20)
        Log("[look] vehicle variation %s: not held by the copy or by our own vehicles of that resource: left out", Guid(gd).c_str());
    return 0;
}

std::uintptr_t Resolve(const std::uint8_t* gd, std::uintptr_t prop, wire::GearKind kind) {
    if (g_veh_scope) return ResolveVehicle(gd, prop);
    const double now = NowMs();
    if (g_found_n == 0 && now - g_built_ms > 2000) Build(now);
    else if (g_built_gen != g_own_gen) BuildOwn();  // before any use: entries of gear we no longer hold go
    bool refused = false;
    const Found* f = FindFor(gd, kind, prop, &refused);
    if (!f && prop) {
        const unsigned keep = g_found_n;
        SourcesComponents(prop, kSrcProp);
        const Found* pf = FindFor(gd, kind, prop, &refused);
        const std::uintptr_t r = pf && StillGuid(pf->res, gd) ? pf->res : 0;
        g_found_n = keep;  // the prop's entries are not kept
        if (r) {
            ++g_src_hits[kSrcProp];
            return r;
        }
    }
    if (!f && refused) {
        // known, but only through a source this item's kind may not use
        if (++g_refused <= 30) Log("[look] variation %s: only held by sources of another kind of item: left out", Guid(gd).c_str());
        Missed(gd, now);
        return 0;
    }
    if (!f) {
        Miss* m = nullptr;
        for (unsigned i = 0; i < g_miss_n; ++i)
            if (std::memcmp(g_miss[i].guid, gd, 16) == 0) m = &g_miss[i];
        if (m && now < m->next_ms) return 0;
        if (now - g_built_ms > 10000) {
            Build(now);
            f = FindFor(gd, kind, prop, &refused);
        }
        if (!f) {
            Missed(gd, now);
            if (++g_unresolved_logged <= 30) Log("[look] variation %s: not among the resources this game holds (%u known): left out", Guid(gd).c_str(), g_found_n);
            return 0;
        }
    }
    // still the same object (a stale map entry after an unload is rebuilt)
    if (!StillGuid(f->res, gd)) {
        Build(now);
        f = FindFor(gd, kind, prop, &refused);
        if (!f || !StillGuid(f->res, gd)) return 0;
    }
    ++g_src_hits[f->src];
    return f->res;
}
bool Unresolvable(const std::uint8_t* gd) {
    const double now = NowMs();
    for (unsigned i = 0; i < g_miss_n; ++i)
        if (std::memcmp(g_miss[i].guid, gd, 16) == 0) return now < g_miss[i].next_ms;
    return false;
}

std::uint32_t g_applies = 0, g_var_added = 0, g_var_removed = 0, g_sv_set = 0, g_sv_created = 0, g_unresolved = 0, g_sv_capped = 0;
// A copy's shader-variable component never grows past this many entries through us (the game's own
// gear has 1-8; a sender naming ever new hashes cannot grow it without bound). Variables only ever
// get added or set: one the sender stops naming keeps its last value on the copy.
constexpr std::int32_t kSvCap = 48;
// A vehicle copy carries more (docs/real-partner-body.md section 22.12): its paint (1-2), the wear on up to 39 parts and
// up to 24 lights (the truck: 1 + 39 + 12 = 52). The game's own vehicles hold 50-130 entries (run 20261118-162226).
constexpr std::int32_t kSvCapVehicle = 160;
std::int32_t SvCapNow() { return g_veh_scope ? kSvCapVehicle : kSvCap; }

bool CompStillOwned(std::uintptr_t c, std::uintptr_t e) { return c && Q(c + kCompOwner) == e; }

// Step 2 of Apply (and SetVehicleVars): each shader variable added (at most up to the cap) and set, on the entity's
// shader-variable component (created and linked as the game's own cap colour does when it has none).
ApplyResult ApplyVars(std::uintptr_t prop, std::uintptr_t apd, std::int32_t parts, const wire::GearShaderVar* vars, unsigned n, char* why, std::size_t cap,
                      bool* partial) {
    if (!n) return ApplyResult::done;
    std::uintptr_t sv = Q(apd + kApdSv);
    if (sv && (Q(sv + kSvApd) != apd || !IsType(sv, "ArtPartsShaderVariableComponent"))) {
        std::snprintf(why, cap, "its shader variable component does not check out");
        return ApplyResult::failed;
    }
    if (!sv) {
        // as the game's own cap colour does (0x14284AA8D): create, then link both ways
        std::uintptr_t c = 0;
        if (!CallSvCreate(prop, &c)) {
            g.off = true;
            Log("[look] shader variable component create FAULTED: gear looks off for this session");
            return ApplyResult::off;
        }
        if (!c || !IsType(c, "ArtPartsShaderVariableComponent") || Q(c + kCompOwner) != prop) {
            std::snprintf(why, cap, "the created shader variable component does not check out");
            return ApplyResult::failed;
        }
        if (!WriteQ(c + kSvApd, apd) || !WriteQ(apd + kApdSv, c)) {
            std::snprintf(why, cap, "the shader variable component could not be linked");
            return ApplyResult::failed;
        }
        sv = c;
        ++g_sv_created;
    }
    const std::int32_t cap_n = SvCapNow();
    for (unsigned i = 0; i < n; ++i) {
        const wire::GearShaderVar& s = vars[i];
        if (s.part != wire::kLookAllParts && static_cast<std::int32_t>(s.part) >= parts) {
            *partial = true;
            continue;
        }
        const int part = s.part == wire::kLookAllParts ? -1 : static_cast<int>(s.part);
        bool exists = false;
        static const float kZero[4] = {};
        if (!CallSvExists(sv, s.hash, part, &exists)) {
            g.off = true;
            Log("[look] a shader variable call FAULTED: gear looks off for this session");
            return ApplyResult::off;
        }
        std::int32_t have_n = 0;
        if (!exists && (!Rd(sv + kSvN, have_n) || have_n < 0 || have_n >= cap_n)) {
            ++g_sv_capped;
            *partial = true;
            if (why && !why[0]) std::snprintf(why, cap, "shader variable %08X not added: the copy holds %d (cap %d)", s.hash, have_n, cap_n);
            continue;
        }
        if ((!exists && !CallSvAddSet(sv, s.hash, kZero, s.count, part, true)) || !CallSvAddSet(sv, s.hash, s.v, s.count, part, false)) {
            g.off = true;
            Log("[look] a shader variable call FAULTED: gear looks off for this session");
            return ApplyResult::off;
        }
        ++g_sv_set;
    }
    return ApplyResult::done;
}

}  // namespace

bool ReadAvailable() noexcept {
    if (g.read_done) return g.read_ok;
    g.read_done = true;
    const std::uintptr_t b = mem::Base();
    bool ok = Check("ArtParts shader variables link", kSvLinkSite, "48 8B 4F 48 E8 ?? ?? ?? ?? 48 89 B8 80 00 00 00 48 89 87 F0 00 00 00") &&
              Check("cap colour (+0x180)", kCapFn, "48 89 5C 24 18 55 56 57 48 83 EC 50 48 8B B9 80 01 00 00") &&
              Check("shader variable entries (+0x58)", kSvEntries, "48 8B 5F 58") && Check("shader variable count (+0x50)", kSvCount, "48 63 77 50") &&
              Check("accessory table (+0x358)", kAccSite, "48 8B 05 ?? ?? ?? ?? 48 8B 48 08 48 8B 81 58 03 00 00") &&
              Check("item infos lookup site", kItemSite, "48 8B 0D ?? ?? ?? ?? E8") &&
              Check("item infos (+0x5D6A0)", kItemLookup, "48 89 5C 24 08 48 63 81 A0 D6 05 00") &&
              Check("baggage config (+0x2B8)", kBagCfgSite, "48 8B 0D ?? ?? ?? ?? 48 8B 51 08 48 8B 8A B8 02 00 00") &&
              Check("backpack variations (+0x138)", kPackVarSite, "3B 98 38 01 00 00") && Check("backpack variations (+0x140)", kPackVarSite2, "48 8B 90 40 01 00 00") &&
              Check("backpack config (+0x5D6C0)", kPackCfgSite, "48 89 8B C0 D6 05 00 66 87 83 30 D7 05 00");
    if (ok && (mem::RipTarget(b + kAccSite, 3, 7) != b + kGameSettings || mem::RipTarget(b + kItemSite, 3, 7) != b + kGameSettings ||
               mem::RipTarget(b + kItemSite + 7, 1, 5) != b + kItemLookup || mem::RipTarget(b + kBagCfgSite, 3, 7) != b + kBagMgr)) {
        std::snprintf(g.why, sizeof(g.why), "the settings / baggage globals are not where expected");
        ok = false;
    }
    const char* an = mem::RttiName(b + kApvRtti);
    const char* sn = mem::RttiName(b + kSvRtti);
    if (ok && (!an || std::strcmp(an, "ArtPartsVariation") != 0 || !sn || std::strcmp(sn, "ArtPartsShaderVariableComponent") != 0)) {
        std::snprintf(g.why, sizeof(g.why), "the ArtPartsVariation / shader variable RTTI is not where expected (%s / %s)", an ? an : "-", sn ? sn : "-");
        ok = false;
    }
    g.read_ok = ok;
    Log("[look] gear looks: reading %s%s", ok ? "available (variations, shader variables, accessory / item / backpack / baggage tables verified)" : "unavailable: ",
        ok ? "" : g.why);
    return ok;
}

bool ApplyAvailable() noexcept {
    if (g.apply_done) return g.apply_ok && !g.off;
    g.apply_done = true;
    bool ok = ReadAvailable() && Check("ApplyArtPartsVariation", kApplyApv, "48 83 EC 28 48 85 C9 74 19 48 85 D2 74 14 41 B0 01 E8") &&
              Check("component removal", kRemoveComp, "48 8B D1 48 8B 49 48 E9") && Check("shader variables create", kSvCreate, "40 57 48 83 EC 40 48 8B 05") &&
              Check("shader variable add", kSvAdd, "40 53 55 56 57 41 54 41 56 41 57 48 81 EC 80 00 00 00") &&
              Check("shader variable exists", kSvExists, "48 89 5C 24 10 48 89 74 24 20 57 48 83 EC 50") &&
              Check("shader variable set", kSvSet, "40 53 56 57 41 56 41 57 48 83 EC 60") &&
              Check("baggage variation lookup", kBagLookup, "48 89 5C 24 08 48 89 74 24 10 57 4C 8B 89 B8 DF 46 00") &&
              Check("baggage variation call site", kBagLookupCall, "44 8B 87 04 02 00 00 48 8B CB 41 C1 E8 05 41 80 E0 01 C5 F8 77 E8");
    if (ok && mem::RipTarget(mem::Base() + kBagLookupCall + 21, 1, 5) != mem::Base() + kBagLookup) {
        std::snprintf(g.why, sizeof(g.why), "the baggage variation call does not lead where expected");
        ok = false;
    }
    g.apply_ok = ok;
    Log("[look] gear looks: applying %s%s", ok ? "available (ApplyArtPartsVariation, component removal, shader variable create / exists / add / set, "
                                                 "baggage variation lookup verified)"
                                              : "unavailable: ",
        ok ? "" : g.why);
    return ok;
}

bool ReadEntity(std::uintptr_t e, wire::GearLook* out) noexcept {
    if (!out || !ReadAvailable()) return false;
    std::uintptr_t apd = 0;
    std::int32_t parts = 0;
    if (!EntityApd(e, &apd, &parts)) return false;
    out->nvar = 0, out->nsv = 0;
    // its own ArtParts (index < 64): which exist, which are shown (+0x2C bit 1; the rain cover's size)
    out->parts_apply = out->parts_visible = 0;
    const std::uintptr_t parr = Q(apd + kApdCount + 8);
    for (std::int32_t i = 0; i < parts && i < 64 && parr; ++i) {
        const std::uintptr_t part = Q(parr + 8ull * static_cast<unsigned>(i));
        std::uint8_t fl = 0;
        if (!part || !Rd(part + 0x2C, fl)) continue;
        out->parts_apply |= 1ull << i;
        if (fl & 0x2) out->parts_visible |= 1ull << i;
    }
    Var v[wire::kMaxLookVariations];
    const unsigned nv = Variations(e, v, wire::kMaxLookVariations, true);
    for (unsigned i = 0; i < nv; ++i) std::memcpy(out->guid[out->nvar++], v[i].guid, 16);
    static SvEntry sv[256];
    const unsigned ns = ShaderVars(e, sv, 256);
    for (unsigned i = 0; i < ns && out->nsv < wire::kMaxLookShaderVars; ++i) {
        const SvEntry& s = sv[i];
        if (s.removed || s.count < 1 || s.count > 4) continue;
        bool finite = true;
        for (unsigned c = 0; c < s.count; ++c) finite = finite && std::isfinite(s.v[c]);
        if (!finite) continue;
        wire::GearShaderVar& o = out->sv[out->nsv++];
        o.hash = static_cast<std::uint32_t>(s.key & 0xFFFFFFFFu);
        o.part = static_cast<std::uint16_t>((s.key >> 50) & 0x1FF);  // hi dword bits 18..26
        o.count = s.count;
        for (unsigned c = 0; c < 4; ++c) o.v[c] = c < s.count ? s.v[c] : 0.0f;
    }
    return true;
}

bool ReadVehicle(std::uintptr_t e, wire::GearLook* out) noexcept {
    if (!out || !ReadAvailable()) return false;
    std::uintptr_t apd = 0;
    std::int32_t parts = 0;
    if (!EntityApd(e, &apd, &parts)) return false;
    out->nvar = 0, out->nsv = 0;
    out->parts_apply = out->parts_visible = 0;
    Var v[wire::kMaxLookVariations];
    const unsigned nv = Variations(e, v, wire::kMaxLookVariations, true);
    for (unsigned i = 0; i < nv; ++i) std::memcpy(out->guid[out->nvar++], v[i].guid, 16);
    static SvEntry sv[256];
    const unsigned ns = ShaderVars(e, sv, 256);
    for (std::uint32_t want : kVehicleLookHashes)
        for (unsigned i = 0; i < ns && out->nsv < wire::kMaxLookShaderVars; ++i) {
            const SvEntry& s = sv[i];
            if (s.removed || static_cast<std::uint32_t>(s.key & 0xFFFFFFFFu) != want || s.count < 1 || s.count > 4) continue;
            bool finite = true;
            for (unsigned c = 0; c < s.count; ++c) finite = finite && std::isfinite(s.v[c]);
            if (!finite) continue;
            wire::GearShaderVar& o = out->sv[out->nsv++];
            o.hash = want;
            o.part = static_cast<std::uint16_t>((s.key >> 50) & 0x1FF);
            o.count = s.count;
            for (unsigned c = 0; c < 4; ++c) o.v[c] = c < s.count ? s.v[c] : 0.0f;
        }
    return true;
}

bool SatisfiedVehicle(std::uintptr_t copy, const wire::GearLook& want) noexcept {
    g_veh_scope = true;
    const bool r = Satisfied(copy, want);
    g_veh_scope = false;
    return r;
}

ApplyResult ApplyVehicle(std::uintptr_t copy, const wire::GearLook& want, const std::uintptr_t* own, unsigned n_own, char* why,
                         std::size_t cap) noexcept {
    g_veh_scope = true;
    g_veh_own = own, g_veh_own_n = n_own;
    const ApplyResult r = Apply(copy, want, why, cap);
    g_veh_scope = false;
    g_veh_own = nullptr, g_veh_own_n = 0;
    return r;
}

bool ReadBaggage(std::uintptr_t bag, wire::GearLook* out) noexcept {
    if (!out || !ApplyAvailable() || !bag || !IsType(bag, "DSBaggage2")) return false;
    const std::uintptr_t li = Q(bag + 0x28);
    if (!li || !IsType(li, "DSGameBaggageListItem")) return false;
    std::uint32_t f204 = 0;
    if (!Rd(bag + 0x204, f204)) return false;
    const std::uintptr_t mgr = Q(mem::Base() + kBagMgr);
    if (!mgr || !Q(mgr + 8)) return false;
    std::uintptr_t r = 0;
    if (!CallBagLookup(mgr, li, static_cast<std::uint8_t>((f204 >> 5) & 1), &r)) {
        g.off = true;
        Log("[look] the baggage variation lookup FAULTED: gear looks off for this session");
        return false;
    }
    out->nvar = 0, out->nsv = 0;
    if (r && IsApv(r) && mem::Read(r + kObjGuid, out->guid[0], 16)) out->nvar = 1;
    return true;
}

bool ReadConfigVariation(unsigned index, wire::GearLook* out) noexcept {
    if (!out || !ReadAvailable()) return false;
    const std::uintptr_t mgr = Q(mem::Base() + kBagMgr);
    const std::uintptr_t cfg = mgr ? Q(mgr + 8) : 0;
    std::int32_t n = 0;
    if (!cfg || !Rd(cfg + 0x2B0, n) || n <= 0 || n > 128 || static_cast<std::int32_t>(index) >= n) return false;
    const std::uintptr_t r = Q(Q(cfg + 0x2B8) + 8ull * index);
    out->nvar = 0, out->nsv = 0;
    if (IsApv(r) && mem::Read(r + kObjGuid, out->guid[0], 16)) out->nvar = 1;
    return out->nvar == 1;
}

void SetOwnGear(const std::uintptr_t* ents, unsigned n, std::uintptr_t player) noexcept {
    std::uintptr_t prev[160];
    const unsigned prev_n = g_own_n;
    std::memcpy(prev, g_own, sizeof(prev));
    g_own_n = 0;
    g_pack_own = 0;
    for (unsigned i = 0; i < n && g_own_n < 160; ++i) {
        if (!ents[i]) continue;
        g_own[g_own_n++] = ents[i];
        if (!g_pack_own && mem::DerivesFrom(mem::RttiOf(ents[i]), "DSBackpackEntity")) g_pack_own = ents[i];
    }
    if (prev_n != g_own_n || std::memcmp(prev, g_own, sizeof(std::uintptr_t) * g_own_n) != 0 || player != g_player_own) ++g_own_gen;
    g_player_own = player;
}

ApplyResult Apply(std::uintptr_t prop, const wire::GearLook& want, char* why, std::size_t cap) noexcept {
    if (why && cap) why[0] = 0;
    if (!ApplyAvailable()) return ApplyResult::off;
    std::uintptr_t apd = 0;
    std::int32_t parts = 0;
    if (!EntityApd(prop, &apd, &parts)) {
        std::snprintf(why, cap, "its ArtPartsData does not check out");
        return ApplyResult::failed;
    }
    ++g_applies;
    bool partial = false;
    // 1. Variations: unwanted ones go, wanted ones come (in the sender's order).
    Var cur[16];
    const unsigned nc = Variations(prop, cur, 16, false);
    for (unsigned i = 0; i < nc; ++i) {
        bool wanted = false;
        for (unsigned k = 0; k < want.nvar; ++k) wanted = wanted || std::memcmp(cur[i].guid, want.guid[k], 16) == 0;
        if (wanted || !CompStillOwned(cur[i].comp, prop)) continue;
        if (!CallRemove(cur[i].comp)) {
            g.off = true;
            Log("[look] component removal FAULTED: gear looks off for this session");
            return ApplyResult::off;
        }
        ++g_var_removed;
    }
    for (unsigned k = 0; k < want.nvar; ++k) {
        bool have = false;
        for (unsigned i = 0; i < nc; ++i) have = have || std::memcmp(cur[i].guid, want.guid[k], 16) == 0;
        if (have) continue;
        const std::uintptr_t r = Resolve(want.guid[k], prop, want.kind);
        if (!r) {
            ++g_unresolved;
            partial = true;
            if (why && !why[0]) std::snprintf(why, cap, "variation %s not found in this game", Guid(want.guid[k]).c_str());
            continue;
        }
        if (!CallApply(prop, r)) {
            g.off = true;
            Log("[look] ApplyArtPartsVariation FAULTED: gear looks off for this session");
            return ApplyResult::off;
        }
        ++g_var_added;
    }
    // 2. Shader variables (after the variations: a variation change rebuilds the materials).
    const ApplyResult vr = ApplyVars(prop, apd, parts, want.sv, want.nsv, why, cap, &partial);
    if (vr != ApplyResult::done) return vr;
    return partial ? ApplyResult::partial : ApplyResult::done;
}

bool ReadVehicleState(std::uintptr_t e, VehicleState* out) noexcept {
    if (!out || !ReadAvailable()) return false;
    std::uintptr_t apd = 0;
    std::int32_t parts = 0;
    if (!EntityApd(e, &apd, &parts)) return false;
    *out = VehicleState{};
    static SvEntry sv[256];
    const unsigned ns = ShaderVars(e, sv, 256);
    out->entries = ns;
    float wmin = 1e9f, wmax = -1e9f;
    for (unsigned i = 0; i < ns; ++i) {
        const SvEntry& s = sv[i];
        const std::uint32_t h = static_cast<std::uint32_t>(s.key & 0xFFFFFFFFu);
        const unsigned part = static_cast<unsigned>((s.key >> 50) & 0x1FF);
        if (s.removed || s.count < 1 || s.count > 4) continue;
        bool finite = true;
        for (unsigned c = 0; c < s.count; ++c) finite = finite && std::isfinite(s.v[c]);
        if (!finite) continue;
        if (h == wire::kVehWearHash) {
            if (part >= wire::kMaxVehicleWearParts) {
                ++out->left_out;
                continue;
            }
            out->wear_parts[part / 64] |= 1ull << (part % 64);
            if (s.v[0] < wmin) wmin = s.v[0];
            if (s.v[0] > wmax) wmax = s.v[0];
            ++out->wear_n;
        } else if (h == wire::kVehLightHash || (h == wire::kVehGaugeHash && s.count >= 3)) {
            const bool colour = h == wire::kVehGaugeHash;
            const std::uint8_t kind = static_cast<std::uint8_t>(colour ? wire::VehicleLightKind::colour : wire::VehicleLightKind::intensity);
            bool dup = false;
            for (unsigned k = 0; k < out->nl && !dup; ++k) dup = out->light[k].part == part && out->light[k].kind == kind;
            if (dup) continue;  // the census logged some entries twice (removed and added again)
            if (part >= wire::kMaxVehicleWearParts || out->nl >= wire::kMaxVehicleLights) {
                ++out->left_out;
                continue;
            }
            wire::VehicleLight& l = out->light[out->nl++];
            l.part = static_cast<std::uint8_t>(part);
            l.kind = kind;
            const float hi = colour ? wire::kMaxVehicleLightColour : wire::kMaxVehicleLightIntensity;
            for (unsigned c = 0; c < 3; ++c) {
                const float v = (colour || c == 0) ? s.v[c] : 0.0f;
                l.v[c] = v < 0.0f ? 0.0f : (v > hi ? hi : v);
            }
        }
    }
    if (out->wear_n) {
        const float w = wmax < 0.0f ? 0.0f : (wmax > 1.0f ? 1.0f : wmax);
        out->wear = static_cast<std::uint16_t>(std::lround(w * 65535.0f));
        out->wear_spread = wmax - wmin;
    }
    return true;
}

ApplyResult SetVehicleVars(std::uintptr_t copy, const wire::GearShaderVar* vars, unsigned n, char* why, std::size_t cap) noexcept {
    if (why && cap) why[0] = 0;
    if (!ApplyAvailable()) return ApplyResult::off;
    std::uintptr_t apd = 0;
    std::int32_t parts = 0;
    if (!EntityApd(copy, &apd, &parts)) {
        std::snprintf(why, cap, "its ArtPartsData does not check out");
        return ApplyResult::failed;
    }
    g_veh_scope = true;
    bool partial = false;
    const ApplyResult r = ApplyVars(copy, apd, parts, vars, n, why, cap, &partial);
    g_veh_scope = false;
    if (r != ApplyResult::done) return r;
    return partial ? ApplyResult::partial : ApplyResult::done;
}

unsigned ReadVars(std::uintptr_t e, std::uint32_t hash, wire::GearShaderVar* out, unsigned cap) noexcept {
    if (!out || !ReadAvailable()) return 0;
    static SvEntry sv[256];
    const unsigned ns = ShaderVars(e, sv, 256);
    unsigned k = 0;
    for (unsigned i = 0; i < ns && k < cap; ++i) {
        const SvEntry& s = sv[i];
        if (s.removed || static_cast<std::uint32_t>(s.key & 0xFFFFFFFFu) != hash || s.count < 1 || s.count > 4) continue;
        wire::GearShaderVar& o = out[k++];
        o.hash = hash;
        o.part = static_cast<std::uint16_t>((s.key >> 50) & 0x1FF);
        o.count = s.count;
        for (unsigned c = 0; c < 4; ++c) o.v[c] = c < s.count ? s.v[c] : 0.0f;
    }
    return k;
}

bool Satisfied(std::uintptr_t prop, const wire::GearLook& want) noexcept {
    if (!ReadAvailable()) return true;
    Var cur[16];
    const unsigned nc = Variations(prop, cur, 16, false);
    unsigned need = 0, have = 0;
    for (unsigned k = 0; k < want.nvar; ++k) {
        bool found = false;
        for (unsigned i = 0; i < nc; ++i) found = found || std::memcmp(cur[i].guid, want.guid[k], 16) == 0;
        if (found) ++have, ++need;
        else if (!Unresolvable(want.guid[k])) ++need;
    }
    if (have != need || nc != have) return false;
    if (!want.nsv) return true;
    static SvEntry sv[256];
    const unsigned ns = ShaderVars(prop, sv, 256);
    for (unsigned i = 0; i < want.nsv; ++i) {
        const wire::GearShaderVar& w = want.sv[i];
        bool ok = false, present = false;
        for (unsigned j = 0; j < ns && !ok; ++j) {
            const SvEntry& s = sv[j];
            if (s.removed || static_cast<std::uint32_t>(s.key & 0xFFFFFFFFu) != w.hash || ((s.key >> 50) & 0x1FF) != w.part) continue;
            ok = present = true;
            for (unsigned c = 0; c < w.count && c < 4; ++c) ok = ok && std::fabs(s.v[c] - w.v[c]) <= 1e-3f * (1.0f + std::fabs(w.v[c]));
        }
        if (!ok && !present && ns >= static_cast<unsigned>(SvCapNow())) continue;  // not added at the cap (Apply): nothing to wait for
        if (!ok) return false;
    }
    return true;
}

void Counters(char* out, std::size_t cap) noexcept {
    std::snprintf(out, cap,
                  "%u applies: %u variation(s) added, %u removed, %u shader variable set(s), %u component(s) created, %u unresolved (%u of another "
                  "kind's sources), %u variable(s) over the cap; %u known variations (found via accessory %u, items %u, backpack %u, backpack config %u, "
                  "baggage %u, component resources %u, own gear %u, scan %u, prop %u; own-gear sources gathered %u times)%s",
                  g_applies, g_var_added, g_var_removed, g_sv_set, g_sv_created, g_unresolved, g_refused, g_sv_capped, g_found_n, g_src_hits[kSrcAcc],
                  g_src_hits[kSrcItem], g_src_hits[kSrcPack], g_src_hits[kSrcPackCfg], g_src_hits[kSrcBag], g_src_hits[kSrcComp], g_src_hits[kSrcOwn],
                  g_src_hits[kSrcScan], g_src_hits[kSrcProp], g_own_builds, g.off ? "; OFF (a fault)" : "");
    const std::size_t used = std::strlen(out);
    if (used + 1 < cap) std::snprintf(out + used, cap - used, "; vehicle variations resolved %u, left out %u", g_veh_resolved, g_veh_unresolved);
}

// ---------------------------------------------------------------- skinned pose
namespace {
bool PoseHeader(std::uintptr_t e, std::uintptr_t* skel, std::uintptr_t* local, std::int32_t* count, std::uint32_t* layout) {
    const std::uintptr_t sm = e ? Q(e + kEntModel) : 0;
    if (!sm || Q(sm) != mem::Base() + kSmVtable || Q(sm + kSmEntity) != e) return false;
    std::int32_t idx = -1;
    if (!Rd(sm + kSmPoseIdx, idx) || (idx != 0 && idx != 1)) return false;
    std::uint8_t hdr[kPoseSize];
    if (!mem::Read(sm + kSmPoses + static_cast<std::uintptr_t>(idx) * kPoseSize, hdr, sizeof(hdr))) return false;
    std::memcpy(skel, hdr + kHdrSkel, 8);
    std::int32_t state = 0;
    std::memcpy(&state, hdr + kHdrState, 4);
    std::uintptr_t lb = 0, le = 0;
    std::memcpy(&lb, hdr + kHdrLocal, 8), std::memcpy(&le, hdr + kHdrLocalEnd, 8);
    if (!*skel || !IsType(*skel, "Skeleton") || !Rd(*skel + kSkelCount, *count) || *count <= 0 || *count > 512) return false;
    Rd(*skel + kSkelLayout, *layout);
    if (!lb || le < lb || (le - lb) != static_cast<std::uintptr_t>(*count) * 0x30) return false;
    *local = lb;
    return true;
}
}  // namespace

bool PoseSkeleton(std::uintptr_t e, std::uintptr_t* skel, std::uint32_t* layout, std::int32_t* count) noexcept {
    std::uintptr_t local = 0;
    return PoseHeader(e, skel, &local, count, layout);
}

bool ReadPose(std::uintptr_t e, wire::GearPose* out) noexcept {
    std::uintptr_t skel = 0, local = 0;
    std::int32_t n = 0;
    std::uint32_t layout = 0;
    if (!out || !PoseHeader(e, &skel, &local, &n, &layout) || n > static_cast<std::int32_t>(wire::kMaxGearPoseJoints)) return false;
    static float a[wire::kMaxGearPoseJoints][12], b[wire::kMaxGearPoseJoints][12];
    const std::size_t bytes = static_cast<std::size_t>(n) * 0x30;
    if (!mem::Read(local, a, bytes) || !mem::Read(local, b, bytes) || std::memcmp(a, b, bytes) != 0) return false;
    out->skeleton = layout;
    out->n = static_cast<std::uint8_t>(n);
    for (std::int32_t j = 0; j < n; ++j) {
        const float* p = a[j];
        out->q[j] = wire::Quat{p[0], p[1], p[2], p[3]};
        out->t[j][0] = p[4], out->t[j][1] = p[5], out->t[j][2] = p[6];
        if (!std::isfinite(p[0]) || !std::isfinite(p[4]) || !std::isfinite(p[5]) || !std::isfinite(p[6])) return false;
    }
    return true;
}

// ---------------------------------------------------------------- debug
void Dump(const char* tag, std::uintptr_t e, std::uintptr_t ref) noexcept {
    if (!ReadAvailable()) return;
    std::uintptr_t apd = 0;
    std::int32_t parts = 0;
    const bool have_apd = EntityApd(e, &apd, &parts);
    Log("[look] %s %p %s: ArtPartsData %p (%s, %d parts)", tag, reinterpret_cast<void*>(e), TypeOf(e), reinterpret_cast<void*>(apd),
        have_apd ? "checked" : "does not check out", parts);
    {
        wire::GearLook pl;
        if (have_apd && ReadEntity(e, &pl))
            Log("[look] %s   parts (< 64): %016llX, shown %016llX", tag, static_cast<unsigned long long>(pl.parts_apply),
                static_cast<unsigned long long>(pl.parts_visible));
    }
    Var v[16];
    const unsigned nv = Variations(e, v, 16, false);
    for (unsigned i = 0; i < nv; ++i) {
        const Found* f = Find(v[i].guid);
        Log("[look] %s   variation comp %p (%s) applied %u resource %p %s '%s'%s%s", tag, reinterpret_cast<void*>(v[i].comp), TypeOf(v[i].comp), v[i].applied,
            reinterpret_cast<void*>(v[i].res), Guid(v[i].guid).c_str(), ObjName(v[i].res).c_str(), f ? " found via " : "", f ? kSrcNames[f->src] : "");
    }
    {
        std::int32_t nc = 0;
        std::uintptr_t comps[64] = {};
        if (Rd(e + kEntComps, nc) && nc > 0 && nc <= 64 && mem::Read(Q(e + kEntComps + 8), comps, 8ull * static_cast<unsigned>(nc))) {
            std::string line;
            for (std::int32_t i = 0; i < nc; ++i) {
                const std::uintptr_t r = Q(comps[i] + 0x30);
                line += TypeOf(comps[i]);
                if (r && mem::DerivesFrom(mem::RttiOf(r), "EntityComponentResource")) line += std::string("(") + TypeOf(r) + ")";
                line += i + 1 < nc ? ", " : "";
            }
            Log("[look] %s   components: %s", tag, line.c_str());
        }
    }
    static SvEntry sv[256];
    std::uintptr_t comp = 0;
    const unsigned ns = ShaderVars(e, sv, 256, &comp);
    if (comp) Log("[look] %s   shader variables %p: %u entries", tag, reinterpret_cast<void*>(comp), ns);
    for (unsigned i = 0; i < ns && i < 48; ++i) {
        const SvEntry& s = sv[i];
        const std::uint32_t hi = static_cast<std::uint32_t>(s.key >> 32);
        Log("[look] %s     var %08X (+8 %08X) kind %u part %u sub %u/%u count %u removed %u: %.4f %.4f %.4f %.4f", tag, static_cast<unsigned>(s.key & 0xFFFFFFFFu),
            s.hash, hi >> 27, (hi >> 18) & 0x1FF, (hi >> 9) & 0x1FF, hi & 0x1FF, s.count, s.removed, s.v[0], s.v[1], s.v[2], s.v[3]);
    }
    std::uintptr_t skel = 0, local = 0;
    std::int32_t n = 0;
    std::uint32_t layout = 0;
    if (PoseHeader(e, &skel, &local, &n, &layout)) {
        std::uintptr_t rs = 0, rl = 0;
        std::int32_t rn = 0;
        std::uint32_t rlay = 0;
        char cmp[160] = "";
        if (ref && ref != e && PoseHeader(ref, &rs, &rl, &rn, &rlay)) {
            float maxq = 0, maxt = 0;
            const std::int32_t m = n < rn ? n : rn;
            static float a[512][12], b[512][12];
            if (m > 0 && mem::Read(local, a, 0x30ull * m) && mem::Read(rl, b, 0x30ull * m)) {
                for (std::int32_t j = 0; j < m; ++j) {
                    for (int k = 0; k < 4; ++k) maxq = std::fmax(maxq, std::fabs(std::fabs(a[j][k]) - std::fabs(b[j][k])));
                    for (int k = 4; k < 7; ++k) maxt = std::fmax(maxt, std::fabs(a[j][k] - b[j][k]));
                }
            }
            std::snprintf(cmp, sizeof(cmp), "; against %p (skeleton %p, %d joints): %s skeleton, max |q| diff %.4f, max t diff %.4f m",
                          reinterpret_cast<void*>(ref), reinterpret_cast<void*>(rs), rn, rs == skel ? "the same" : "another", maxq, maxt);
        }
        Log("[look] %s   pose: skeleton %p, %d joints, layout %08X%s", tag, reinterpret_cast<void*>(skel), n, layout, cmp);
    }
}

void DumpJoints(const char* tag, std::uintptr_t e) noexcept {
    std::uintptr_t skel = 0, local = 0;
    std::int32_t n = 0;
    std::uint32_t layout = 0;
    if (!PoseHeader(e, &skel, &local, &n, &layout) || n > 128) return;
    static float a[128][12];
    if (!mem::Read(local, a, 0x30ull * n)) return;
    const std::uintptr_t joints = Q(skel + kSkelJoints);
    for (std::int32_t j = 0; j < n; ++j) {
        std::int16_t parent = -1;
        Rd(joints + 0x10ull * j + 8, parent);
        Log("[look] %s joint %2d '%s' parent %d: q %.3f %.3f %.3f %.3f t %.3f %.3f %.3f s %.2f", tag, j, CStr(Q(joints + 0x10ull * j)).c_str(), parent,
            a[j][0], a[j][1], a[j][2], a[j][3], a[j][4], a[j][5], a[j][6], a[j][8]);
    }
}

void DumpResourceVariations(const char* tag, std::uintptr_t e) noexcept {
    if (!ReadAvailable()) return;
    const std::uintptr_t res = e ? RefObject(e + kEntResRef) : 0;
    if (!res) {
        Log("[look] %s %p: its resource is not loaded / unreadable", tag, reinterpret_cast<void*>(e));
        return;
    }
    static std::uint8_t buf[0x400];
    if (!mem::Read(res, buf, sizeof(buf))) return;
    unsigned found = 0;
    for (std::size_t k = 0; k + 8 <= sizeof(buf); k += 8) {
        std::uintptr_t v = 0;
        std::memcpy(&v, buf + k, 8);
        if (v > 0x10000 && (v & 7) == 0 && IsApv(v)) {
            std::uint8_t gd[16] = {};
            mem::Read(v + kObjGuid, gd, 16);
            Log("[look] %s resource %s +0x%03X: %s '%s'", tag, TypeOf(res), static_cast<unsigned>(k), Guid(gd).c_str(), ObjName(v).c_str());
            ++found;
        }
        if (k + 16 <= sizeof(buf)) {
            std::uint32_t cnt = 0, capa = 0;
            std::uintptr_t data = 0;
            std::memcpy(&cnt, buf + k, 4), std::memcpy(&capa, buf + k + 4, 4), std::memcpy(&data, buf + k + 8, 8);
            if (cnt >= 1 && cnt <= 96 && capa >= cnt && capa <= 4096 && data > 0x10000 && (data & 7) == 0) {
                std::uintptr_t el[96];
                if (mem::Read(data, el, 8ull * cnt) && IsApv(el[0]))
                    for (unsigned i = 0; i < cnt; ++i) {
                        std::uint8_t gd[16] = {};
                        if (!IsApv(el[i]) || !mem::Read(el[i] + kObjGuid, gd, 16)) continue;
                        Log("[look] %s resource %s +0x%03X[%u]: %s '%s'", tag, TypeOf(res), static_cast<unsigned>(k), i, Guid(gd).c_str(), ObjName(el[i]).c_str());
                        ++found;
                    }
            }
        }
    }
    Log("[look] %s resource %p %s: %u variation reference(s)", tag, reinterpret_cast<void*>(res), TypeOf(res), found);
}

void DumpSources(const std::uintptr_t* ents, unsigned n, std::uintptr_t player) noexcept {
    if (!ReadAvailable()) return;
    SetOwnGear(ents, n, player);
    // without our own gear first: what the other sources find on their own
    g_no_own_applied = true;
    Build(NowMs());
    g_no_own_applied = false;
    const unsigned without = g_found_n;
    unsigned per[kSrcCount] = {};
    for (unsigned i = 0; i < g_found_n; ++i) ++per[g_found[i].src];
    // which of our gear's applied variations each source holds
    unsigned total = 0, by_tables = 0;
    for (unsigned i = 0; i < n; ++i) {
        Var v[8];
        const unsigned k = Variations(ents[i], v, 8, true);
        for (unsigned j = 0; j < k; ++j) {
            ++total;
            const Found* f = Find(v[j].guid);
            if (f) ++by_tables;
            Log("[look] sources: %p's variation %s '%s' %s%s", reinterpret_cast<void*>(ents[i]), Guid(v[j].guid).c_str(), ObjName(v[j].res).c_str(),
                f ? "found via " : "NOT found without our own gear", f ? kSrcNames[f->src] : "");
        }
    }
    Build(NowMs());
    Log("[look] sources: %u variation resources without our own gear entities (accessory %u, items %u, backpack %u, backpack config %u, baggage %u, "
        "player components %u, scan %u), %u with them; %u of our gear's %u applied variations found without our own gear's applied ones",
        without, per[kSrcAcc], per[kSrcItem], per[kSrcPack], per[kSrcPackCfg], per[kSrcBag], per[kSrcComp], per[kSrcScan], g_found_n, by_tables, total);
}

}  // namespace st::look
