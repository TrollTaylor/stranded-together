#include "partner_source.hpp"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "log.hpp"

namespace st::partner {
namespace {

// ---- ds.exe addresses (RVAs, image base 0x140000000; evidence in docs/reverse-engineering.md
// and docs/partner-hologram.md)
constexpr std::uintptr_t kEchoManagerGlobal = 0x7BE8668;     // DSPlayerEchoManager*
constexpr std::uintptr_t kEchoMgrConfig = 0x418;              // DSPlayerEchoManagerConfig* (borrowed)
constexpr std::uintptr_t kEchoMgrLock = 0x468;                // SRWLOCK guarding +0x418
constexpr std::uintptr_t kEchoConfigRtti = 0x4BC96B0;        // RTTI DSPlayerEchoManagerConfig
constexpr std::uintptr_t kCfgEntityResource = 0x20;           // Ref<EntityResource> EntityResource
constexpr std::uintptr_t kCfgKnotSpaceResource = 0x28;        // Ref<EntityResource> KnotSpaceEntityResource
constexpr std::uintptr_t kTraceMgrGlobal = 0x7BE8680;        // DSPlayerTracePointManager* ([mgr] = config)
constexpr std::uintptr_t kTraceMgrLoad = 0x273785F;          // 0x14273785F: mov rax,[rip+kTraceMgrGlobal]; mov [r11+18],rbx
constexpr std::uintptr_t kTraceConfigRtti = 0x4BC8EE0;       // RTTI DSPlayerTracePointConfig
constexpr std::uintptr_t kTraceEntityResources = 0x58;       // Array<Ref<EntityResource>>: i32 count, data at +0x60
constexpr std::uintptr_t kSharedEntityBuilder = 0x2366570;    // EntityResource vt+0x80 used by plain resources
constexpr std::uintptr_t kBaseEntityWake = 0x23614F0;        // Entity vt+0xA8 (base Wake)
constexpr std::uintptr_t kExportedSetVisible = 0x2361E90;    // test rcx,rcx; jne SetVisible
constexpr std::uintptr_t kRefCount = 0x18;                    // RTTIRefObject refcount (low 22 bits)
constexpr std::uintptr_t kResComponents = 0x90;               // {i32 count, i32 cap} + data ptr at +0x98
constexpr std::uintptr_t kEntComponents = 0x90;
constexpr std::uintptr_t kEntFlags = 0x88;
constexpr std::uintptr_t kEntModel = 0xB8;
constexpr std::uintptr_t kRttiName = 0x40;
constexpr std::uintptr_t kRttiBaseCount = 0x06;
constexpr std::uintptr_t kRttiBases = 0x60;

struct Range {
    std::uintptr_t lo = 0, hi = 0;
    bool Has(std::uintptr_t a) const { return a >= lo && a < hi; }
};
Range g_image, g_text;

void InitRanges() {
    if (g_image.lo) return;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        if (std::memcmp(s->Name, ".text", 6) == 0) g_text = {base + s->VirtualAddress, base + s->VirtualAddress + s->Misc.VirtualSize};
    }
    g_image = {base, base + nt->OptionalHeader.SizeOfImage};
}

}  // namespace

// ------------------------------------------------------------------ mem
namespace mem {

std::uintptr_t Base() {
    InitRanges();
    return g_image.lo;
}

bool InImage(std::uintptr_t a) {
    InitRanges();
    return g_image.Has(a);
}

bool InText(std::uintptr_t a) {
    InitRanges();
    return g_text.Has(a);
}

bool Read(std::uintptr_t addr, void* out, std::size_t n) {
    if (addr < 0x10000) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(addr), out, n, &got) && got == n;
}

std::uintptr_t Q(std::uintptr_t addr) {
    std::uintptr_t v = 0;
    return Rd(addr, v) ? v : 0;
}

namespace {
// Decima's GetRTTI-style getters are one instruction:
//   lea rax,[rip+X]; ret   -> X
//   mov rax,[rip+X]; ret   -> *X
std::uintptr_t DecodeGetter(std::uintptr_t fn) {
    InitRanges();
    if (!g_text.Has(fn)) return 0;
    std::uint8_t b[8];
    if (!Read(fn, b, sizeof(b)) || b[0] != 0x48 || b[2] != 0x05 || b[7] != 0xC3) return 0;
    std::int32_t rel = 0;
    std::memcpy(&rel, b + 3, 4);
    const std::uintptr_t x = fn + 7 + static_cast<std::intptr_t>(rel);
    if (b[1] == 0x8D) return g_image.Has(x) ? x : 0;
    if (b[1] == 0x8B) {
        const std::uintptr_t v = Q(x);
        return g_image.Has(v) ? v : 0;
    }
    return 0;
}

bool DerivesFromDepth(std::uintptr_t rtti, const char* name, int depth) {
    if (!rtti || depth > 12) return false;
    const char* n = RttiName(rtti);
    if (n && std::strcmp(n, name) == 0) return true;
    std::uint8_t count = 0;
    if (!Rd(rtti + kRttiBaseCount, count) || count > 16) return false;
    const std::uintptr_t bases = Q(rtti + kRttiBases);
    for (unsigned i = 0; i < count; ++i)
        if (DerivesFromDepth(Q(bases + 16 * i), name, depth + 1)) return true;
    return false;
}
}  // namespace

const char* RttiName(std::uintptr_t rtti) {
    InitRanges();
    if (!g_image.Has(rtti)) return nullptr;
    const std::uintptr_t p = Q(rtti + kRttiName);
    if (!g_image.Has(p)) return nullptr;
    char buf[2] = {};
    if (!Read(p, buf, 1) || !buf[0]) return nullptr;
    return reinterpret_cast<const char*>(p);  // lives in ds.exe .rdata: immutable
}

std::uintptr_t RttiOf(std::uintptr_t obj) {
    InitRanges();
    const std::uintptr_t vt = Q(obj);
    if (!g_image.Has(vt)) return 0;
    return DecodeGetter(Q(vt));
}

bool DerivesFrom(std::uintptr_t rtti, const char* name) { return DerivesFromDepth(rtti, name, 0); }

bool CodeMatches(std::uintptr_t va, const char* sig) {
    InitRanges();
    std::uint8_t want[96] = {};
    bool any[96] = {};
    std::size_t n = 0;
    for (const char* p = sig; *p && n < sizeof(want);) {
        if (*p == ' ') {
            ++p;
            continue;
        }
        if (p[0] == '?') {
            any[n] = true;
            want[n++] = 0;
            p += p[1] == '?' ? 2 : 1;
            continue;
        }
        if (!p[1]) return false;
        const char hex[3] = {p[0], p[1], 0};
        want[n++] = static_cast<std::uint8_t>(std::strtoul(hex, nullptr, 16));
        p += 2;
    }
    if (!n || !g_text.Has(va) || !g_text.Has(va + n - 1)) return false;
    std::uint8_t got[sizeof(want)] = {};
    if (!Read(va, got, n)) return false;
    for (std::size_t i = 0; i < n; ++i)
        if (!any[i] && got[i] != want[i]) return false;
    return true;
}

std::uintptr_t RipTarget(std::uintptr_t insn, unsigned rel_off, unsigned insn_len) {
    std::int32_t rel = 0;
    if (!Rd(insn + rel_off, rel)) return 0;
    return insn + insn_len + static_cast<std::intptr_t>(rel);
}

}  // namespace mem

// ------------------------------------------------------------------ internals
namespace {
using mem::Q;
using mem::Rd;
using mem::RttiName;
using mem::RttiOf;

std::uintptr_t Base() { return mem::Base(); }

std::string ComponentNames(std::uintptr_t list_at, int* count_out, bool* bad_out) {
    std::int32_t n = 0;
    std::string out;
    *count_out = -1;
    *bad_out = false;
    if (!Rd(list_at, n) || n < 0 || n > 512) return "(unreadable)";
    *count_out = n;
    const std::uintptr_t data = Q(list_at + 8);
    for (std::int32_t i = 0; i < n; ++i) {  // check every entry; only the log line is shortened
        const std::uintptr_t c = Q(data + 8 * static_cast<std::uintptr_t>(i));
        const char* name = TypeName(reinterpret_cast<const void*>(c));
        if (name && (std::strncmp(name, "DSPlayer", 8) == 0 || std::strstr(name, "GameActor") ||
                     std::strncmp(name, "AI", 2) == 0 || std::strncmp(name, "DSAI", 4) == 0))
            *bad_out = true;
        if (i < 48) {
            if (i) out += ", ";
            out += name ? name : "?";
        } else if (i == 48) {
            out += ", ...";
        }
    }
    return out;
}

std::string g_last_outcome, g_last_marker_outcome;
void LogOnce(std::string& last, const std::string& s) {
    if (s == last) return;
    last = s;
    Log("%s", s.c_str());
}

// Validate one candidate resource (reads only). Returns a reason on refusal.
// strict_builder: also require the shared plain-entity builder (vt+0x80); the
// echo resource uses it, trace-point props are not known to.
const char* Validate(std::uintptr_t res, const char* label, bool strict_builder, std::string* detail) {
    char buf[320];
    if (!Base() || !res) return "missing";
    const std::uintptr_t vt = Q(res);
    if (!g_image.Has(vt)) return "bad vtable";
    const char* rtype = RttiName(mem::RttiOf(res));
    const std::uintptr_t builder = Q(vt + 0x80);
    std::uintptr_t etype = 0;
    {
        // vt+0x68 is the "entity type" getter (same one-instruction shape as GetRTTI).
        const std::uintptr_t fn = Q(vt + 0x68);
        std::uint8_t b[8] = {};
        if (g_text.Has(fn) && mem::Read(fn, b, sizeof(b)) && b[0] == 0x48 && b[2] == 0x05 && b[7] == 0xC3) {
            std::int32_t rel = 0;
            std::memcpy(&rel, b + 3, 4);
            const std::uintptr_t x = fn + 7 + static_cast<std::intptr_t>(rel);
            etype = b[1] == 0x8D ? x : (b[1] == 0x8B ? Q(x) : 0);
            if (!g_image.Has(etype)) etype = 0;
        }
    }
    const char* ename = RttiName(etype);
    int ncomp = -1;
    bool bad = false;
    const std::string comps = ComponentNames(res + kResComponents, &ncomp, &bad);
    std::snprintf(buf, sizeof(buf), "[partner] %s resource %p: %s -> builds %s (builder ds.exe+0x%llX); %d components: ",
                  label, reinterpret_cast<void*>(res), rtype ? rtype : "?", ename ? ename : "?",
                  static_cast<unsigned long long>(builder - Base()), ncomp);
    *detail = std::string(buf) + comps;
    if (!ename) return "unknown entity type";
    if (mem::DerivesFrom(etype, "Humanoid")) return "builds a Humanoid (player) entity";
    if (!g_text.Has(builder)) return "entity builder outside ds.exe code";
    if (strict_builder && builder != Base() + kSharedEntityBuilder) return "non-standard entity builder";
    if (bad) return "has player or AI components";
    return nullptr;
}

std::uintptr_t TraceMgrGlobal() {
    // Re-derive the global from the instruction that loads it (0x14273785F) once.
    static std::uintptr_t cached = 1;
    if (cached == 1) {
        const std::uintptr_t insn = Base() + kTraceMgrLoad;
        cached = 0;
        if (mem::CodeMatches(insn, "48 8B 05 ?? ?? ?? ?? 49 89 5B 18") && mem::RipTarget(insn, 3, 7) == Base() + kTraceMgrGlobal)
            cached = Base() + kTraceMgrGlobal;
        else
            Log("[partner] trace-point manager global did not verify at ds.exe+0x%llX; marker body unavailable",
                static_cast<unsigned long long>(kTraceMgrLoad));
    }
    return cached;
}

std::uintptr_t TraceConfig() {
    const std::uintptr_t g = TraceMgrGlobal();
    if (!g) return 0;
    const std::uintptr_t mgr = Q(g);
    return mgr ? Q(mgr) : 0;
}

void AddRef(std::uintptr_t res) { InterlockedIncrement(reinterpret_cast<volatile LONG*>(res + kRefCount)); }

}  // namespace

// ------------------------------------------------------------------ public
const char* TypeName(const void* obj) { return RttiName(RttiOf(reinterpret_cast<std::uintptr_t>(obj))); }

bool Acquire(Source* out) {
    *out = Source{};
    const std::uintptr_t base = Base();
    const std::uintptr_t mgr = Q(base + kEchoManagerGlobal);
    if (!mgr) {
        LogOnce(g_last_outcome, "[partner] echo manager not created yet; waiting");
        return false;
    }
    auto* lock = reinterpret_cast<PSRWLOCK>(mgr + kEchoMgrLock);
    std::uintptr_t probe = 0;
    if (!Rd(mgr + kEchoMgrLock, probe)) return false;
    if (!TryAcquireSRWLockExclusive(lock)) return false;  // being (un)loaded right now: next frame

    bool ok = false;
    std::string report;
    const std::uintptr_t cfg = Q(mgr + kEchoMgrConfig);
    if (!cfg) {
        report = "[partner] echo config not loaded yet; waiting";
    } else if (RttiOf(cfg) != base + kEchoConfigRtti) {
        report = "[partner] echo manager +0x418 is not a DSPlayerEchoManagerConfig; refusing";
    } else {
        const struct {
            std::uintptr_t off;
            const char* label;
        } cands[] = {{kCfgEntityResource, "echo"}, {kCfgKnotSpaceResource, "knot-space echo"}};
        for (const auto& c : cands) {
            const std::uintptr_t res = Q(cfg + c.off);
            std::string detail;
            const char* why = Validate(res, c.label, true, &detail);
            report += (report.empty() ? "" : "\n") + detail + (why ? std::string("  -> REFUSED: ") + why : "  -> OK");
            if (!why) {
                AddRef(res);
                out->kind = Kind::echo;
                out->res = reinterpret_cast<game::EntityResource*>(res);
                out->cfg = cfg;
                out->label = c.label;
                ok = true;
                break;
            }
        }
    }
    ReleaseSRWLockExclusive(lock);
    LogOnce(g_last_outcome, report);
    return ok;
}

bool AcquireMarker(Source* out, int want) {
    *out = Source{};
    const std::uintptr_t cfg = TraceConfig();
    if (!cfg) {
        LogOnce(g_last_marker_outcome, "[partner] trace-point config not loaded yet; waiting");
        return false;
    }
    if (RttiOf(cfg) != Base() + kTraceConfigRtti) {
        LogOnce(g_last_marker_outcome, "[partner] trace-point manager config is not a DSPlayerTracePointConfig; refusing");
        return false;
    }
    std::int32_t n = 0;
    if (!Rd(cfg + kTraceEntityResources, n) || n <= 0 || n > 256) {
        LogOnce(g_last_marker_outcome, "[partner] trace-point config has no EntityResources");
        return false;
    }
    const std::uintptr_t data = Q(cfg + kTraceEntityResources + 8);
    std::string report = "[partner] trace-point EntityResources (" + std::to_string(n) + "):";
    int chosen = -1;
    std::uintptr_t chosen_res = 0;
    for (int pass = 0; pass < 2 && chosen < 0; ++pass) {
        for (std::int32_t i = 0; i < n; ++i) {
            if (pass == 0 && want >= 0 && i != want) continue;
            const std::uintptr_t res = Q(data + 8 * static_cast<std::uintptr_t>(i));
            std::string detail;
            char label[24];
            std::snprintf(label, sizeof(label), "marker[%d]", static_cast<int>(i));
            const char* why = Validate(res, label, false, &detail);
            report += "\n" + detail + (why ? std::string("  -> REFUSED: ") + why : "  -> OK");
            if (!why) {
                chosen = i;
                chosen_res = res;
                break;
            }
        }
        if (want < 0) break;  // pass 0 already scanned everything
    }
    if (chosen < 0) {
        LogOnce(g_last_marker_outcome, report + "\n[partner] no usable trace-point prop");
        return false;
    }
    if (TraceConfig() != cfg) return false;  // replaced while we looked: next frame
    AddRef(chosen_res);
    out->kind = Kind::marker;
    out->res = reinterpret_cast<game::EntityResource*>(chosen_res);
    out->cfg = cfg;
    out->label = "marker";
    out->index = chosen;
    LogOnce(g_last_marker_outcome, report + "\n[partner] marker body: trace-point prop #" + std::to_string(chosen));
    return true;
}

void Release(Source& s) {
    if (s.res) {
        auto* rc = reinterpret_cast<volatile LONG*>(reinterpret_cast<std::uintptr_t>(s.res) + kRefCount);
        for (;;) {
            const LONG v = *rc;
            if ((v & 0x3FFFFF) <= 1) {  // ours is the last reference: leak rather than destroy it ourselves
                Log("[partner] %s resource has no other owner; leaving it (no release)", s.label);
                break;
            }
            if (InterlockedCompareExchange(rc, v - 1, v) == v) break;
        }
    }
    s = Source{};
}

bool StillLoaded(const Source& s) {
    if (!s.res) return false;
    if (s.kind == Kind::marker) return TraceConfig() == s.cfg;
    const std::uintptr_t mgr = Q(Base() + kEchoManagerGlobal);
    return mgr && Q(mgr + kEchoMgrConfig) == s.cfg;
}

bool GameRemovingEchoes() {
    // Echo manager update 0x142745CB0 removes its echoes (0x14273C580) when
    // [[0x147BE6318]+0x450] is set or [[0x147BF83C0]+0x5E0] == 2.
    const std::uintptr_t base = Base();
    const std::uintptr_t a = Q(base + 0x7BE6318);
    std::uint8_t flag = 0;
    if (a && Rd(a + 0x450, flag) && flag) return true;
    const std::uintptr_t b = Q(base + 0x7BF83C0);
    std::int32_t state = 0;
    return b && Rd(b + 0x5E0, state) && state == 2;
}

CreateResult Create(const Source& s, const game::WorldTransform& at, const char* name, game::Entity** out) {
    *out = nullptr;
    if (s.kind == Kind::marker) {
        if (!s.res || TraceConfig() != s.cfg) return CreateResult::unloaded;
        if (!game::TryCreateEntity(s.res, at, name, out)) return CreateResult::faulted;
        return *out ? CreateResult::ok : CreateResult::null_entity;
    }
    const std::uintptr_t mgr = Q(Base() + kEchoManagerGlobal);
    std::uintptr_t probe = 0;
    if (!mgr || !Rd(mgr + kEchoMgrLock, probe)) return CreateResult::unloaded;
    auto* lock = reinterpret_cast<PSRWLOCK>(mgr + kEchoMgrLock);
    if (!TryAcquireSRWLockExclusive(lock)) return CreateResult::busy;
    CreateResult r = CreateResult::unloaded;
    if (s.res && Q(mgr + kEchoMgrConfig) == s.cfg) {
        // Creation never takes this lock (the game itself creates echoes while holding it).
        if (!game::TryCreateEntity(s.res, at, name, out)) r = CreateResult::faulted;
        else r = *out ? CreateResult::ok : CreateResult::null_entity;
    }
    ReleaseSRWLockExclusive(lock);
    return r;
}

bool ReadFlags(game::Entity* e, std::uint64_t* out) {
    return Rd(reinterpret_cast<std::uintptr_t>(e) + kEntFlags, *out);
}

void DescribeEntity(game::Entity* e, const char* what) {
    const auto a = reinterpret_cast<std::uintptr_t>(e);
    std::uint64_t flags = 0;
    ReadFlags(e, &flags);
    const std::uintptr_t model = Q(a + kEntModel);
    int n = -1;
    bool bad = false;
    const std::string comps = ComponentNames(a + kEntComponents, &n, &bad);
    const char* tn = TypeName(e);
    const char* mn = model ? TypeName(reinterpret_cast<const void*>(model)) : nullptr;
    Log("[partner] %s %p: %s flags=0x%llX (visible=%d hidden=%d asleep=%d) model=%p %s; %d components: %s", what, e,
        tn ? tn : "?", static_cast<unsigned long long>(flags), (flags & 0x2) ? 1 : 0, (flags & 0x4) ? 1 : 0,
        (flags & 0x100) ? 1 : 0, reinterpret_cast<void*>(model), mn ? mn : "", n, comps.c_str());
}

std::uintptr_t FindComponentByName(game::Entity* e, const char* type_name) {
    const auto a = reinterpret_cast<std::uintptr_t>(e);
    std::int32_t n = 0;
    if (!e || !Rd(a + kEntComponents, n) || n <= 0 || n > 512) return 0;
    const std::uintptr_t data = Q(a + kEntComponents + 8);
    for (std::int32_t i = 0; i < n; ++i) {
        const std::uintptr_t c = Q(data + 8 * static_cast<std::uintptr_t>(i));
        if (c && mem::DerivesFrom(RttiOf(c), type_name)) return c;
    }
    return 0;
}

bool WakeIfAsleep(game::Entity* e) {
    std::uint64_t flags = 0;
    if (!e || !ReadFlags(e, &flags)) return false;
    if (!(flags & 0x100)) return true;
    InitRanges();
    const std::uintptr_t vt = Q(reinterpret_cast<std::uintptr_t>(e));
    if (!g_image.Has(vt) || Q(vt + 0xA8) != Base() + kBaseEntityWake) return false;
    reinterpret_cast<void (*)(game::Entity*)>(Base() + kBaseEntityWake)(e);
    return ReadFlags(e, &flags) && !(flags & 0x100);
}

bool ReadVtable(game::Entity* e, std::uintptr_t* out) {
    return Rd(reinterpret_cast<std::uintptr_t>(e), *out);
}

bool ReadGuid(game::Entity* e, game::Guid* out) {
    const auto a = reinterpret_cast<std::uintptr_t>(e) + game::kEntityGuid;
    return Rd(a, out->lo) && Rd(a + 8, out->hi);
}

bool SetVisible(game::Entity* e, bool visible) {
    const std::uintptr_t fn = Base() + kExportedSetVisible;
    if (!e || !mem::CodeMatches(fn, "48 85 C9 0F 85")) return false;
    reinterpret_cast<void (*)(game::Entity*, bool)>(fn)(e, visible);
    return true;
}

bool Remove(game::Entity* e) {
    const auto remove = game::Get().EntityRemove;
    if (!e || !remove) return false;
    remove(e, true);
    return true;
}

}  // namespace st::partner
