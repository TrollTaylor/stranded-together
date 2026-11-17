// Animated-body census and Sam's pose memory: see anim_census.hpp.
#include "anim_census.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "automation.hpp"
#include "game_actions.hpp"
#include "game_api.hpp"
#include "log.hpp"
#include "partner_source.hpp"

namespace st::census {
namespace {

namespace mem = partner::mem;
using mem::Q;
using mem::Rd;

// ---------------------------------------------------------------- layout (see holo_catalog.cpp / docs)
constexpr std::uintptr_t kResLocation = 0x30;    // EntityResource: String Location
constexpr std::uintptr_t kResModel = 0x58;       // EntityResource: Ref<ModelResource>
constexpr std::uintptr_t kResComponents = 0x90;  // Array<Ref<EntityComponentResource>>
constexpr std::uintptr_t kResName = 0x130;       // DSControlledEntityResource: String Name
constexpr std::uintptr_t kResAiType = 0x150;     // EAIEntityType (1 = Humanoid)
constexpr std::uintptr_t kEntFlags = 0x88;
constexpr std::uintptr_t kEntResourceRef = 0x58;
constexpr std::uintptr_t kEntComponents = 0x90;
constexpr std::uintptr_t kEntModel = 0xB8;
constexpr std::uintptr_t kEntPos = 0xC8;
constexpr std::uintptr_t kEntArtParts = 0x180;
constexpr std::uintptr_t kCompResource = 0x30;
constexpr std::uintptr_t kCompOwner = 0x48;      // EntityComponent: cptr<Entity> Entity
constexpr std::uintptr_t kArtSkeleton = 0x40;
constexpr std::uintptr_t kSkelJoints = 0x20;
constexpr std::uintptr_t kSkelLayoutHash = 0x70;
// MorphemeAnimationManagerResource (RTTI members): +0x40 Pose InitialPose, +0xC8 Location,
// +0xD8 MorphemeNetworkDef, +0xE0 AnimationSetMap, +0xF0 DefaultSkeleton, +0x100 MotionTableResource
constexpr std::uintptr_t kMamrLocation = 0xC8;
constexpr std::uintptr_t kMamrNetworkDef = 0xD8;
constexpr std::uintptr_t kMamrSkeleton = 0xF0;
// SkinnedModelResource: +0x58 Skeleton
constexpr std::uintptr_t kSmrSkeleton = 0x58;
// DSDeliveryPointInfoResource: +0x20 DescriptionText, +0x48 Locator, +0x68 EDSArea (u16),
// +0x6A EDSConstructionPointType (u16), +0x6C flags, +0x70 locator id, +0x80 WorldTransform
constexpr std::uintptr_t kDpText = 0x20;
constexpr std::uintptr_t kDpArea = 0x68;
constexpr std::uintptr_t kDpType = 0x6A;
constexpr std::uintptr_t kDpFlags = 0x6C;
constexpr std::uintptr_t kDpId = 0x70;
constexpr std::uintptr_t kDpTransform = 0x80;
// DSMuleCPGameActorLocator: +0x70 String Name (GameActorLocator), Orientation in WorldNode (offset logged)
constexpr std::uintptr_t kLocName = 0x70;
// World / subspace (byte-checked exactly like holo_catalog.cpp)
constexpr std::uintptr_t kWorldGlobal = 0x7BC4E70;
constexpr std::uintptr_t kWorldVtable = 0x3D5D140;
constexpr std::uintptr_t kWorldAddLock = 0x357E024;
constexpr std::uintptr_t kWorldAddArray = 0x357E041;
constexpr std::uintptr_t kWorldEntities = 0x438;
constexpr std::uintptr_t kSubspaceLoad = 0x26A27AB;
constexpr std::uintptr_t kSubspaceGlobal = 0x7BE79B0;

// ---------------------------------------------------------------- small readers
std::string Clean(const char* p, std::size_t n) {
    std::string s(p, n);
    for (char& c : s)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = ' ';
    return s;
}

std::string Str(std::uintptr_t p, std::size_t max = 160) {
    if (!p) return "";
    char buf[256] = {};
    if (max > sizeof(buf)) max = sizeof(buf);
    std::int32_t len = -1;
    std::size_t n = 0;
    if (Rd(p - 8, len) && len >= 0 && len <= 65536) {
        n = static_cast<std::size_t>(len) < max ? static_cast<std::size_t>(len) : max;
        if (n && !mem::Read(p, buf, n)) return "?";
    } else {
        for (; n < max; ++n) {
            char c = 0;
            if (!Rd(p + n, c) || !c) break;
            buf[n] = c;
        }
    }
    return Clean(buf, n);
}

const char* TypeOf(std::uintptr_t obj) {
    const char* n = obj ? partner::TypeName(reinterpret_cast<const void*>(obj)) : nullptr;
    return n ? n : "?";
}
bool IsType(std::uintptr_t obj, const char* name) { return obj && std::strcmp(TypeOf(obj), name) == 0; }
bool Derives(std::uintptr_t obj, const char* name) { return obj && mem::DerivesFrom(mem::RttiOf(obj), name); }

std::uintptr_t RefResource(std::uintptr_t field) {
    const std::uintptr_t d = Q(field);
    if (!d) return 0;
    std::uintptr_t blk[2] = {};
    if (!mem::Read(d, blk, sizeof(blk)) || !blk[0]) return 0;
    if (static_cast<std::int8_t>(static_cast<std::uint8_t>(blk[1] >> 52)) >= 0) return 0;
    return Q(blk[0] + 0x20);
}

struct Arr {
    std::int32_t n = 0;
    std::uintptr_t data = 0;
};
Arr ReadArr(std::uintptr_t at, std::int32_t max = 4096) {
    Arr a;
    if (!Rd(at, a.n) || a.n < 0 || a.n > max) a.n = 0;
    a.data = a.n ? Q(at + 8) : 0;
    if (!a.data) a.n = 0;
    return a;
}

std::string ListTypes(std::uintptr_t at, int* count) {
    const Arr a = ReadArr(at, 512);
    *count = a.n;
    std::string s;
    for (std::int32_t i = 0; i < a.n; ++i) {
        if (i) s += ", ";
        s += TypeOf(Q(a.data + 8 * static_cast<std::uintptr_t>(i)));
    }
    return s;
}

// LocalizedTextResource -> ASCII-ish text (non-ASCII shown as '?'), see holo_catalog.cpp.
std::string LocText(std::uintptr_t lt, std::size_t max_chars) {
    if (!lt || !IsType(lt, "LocalizedTextResource")) return "";
    const std::uintptr_t p = Q(lt + 0x20);
    std::uint16_t len = 0;
    if (!p || !Rd(lt + 0x28, len) || !len) return "";
    std::vector<std::uint8_t> b(len);
    if (!mem::Read(p, b.data(), b.size())) return "";
    std::string s;
    for (std::size_t i = 0; i < b.size() && b[i] && s.size() < max_chars;) {
        if (b[i] == 1 && i + 2 < b.size()) {
            const unsigned c = b[i + 1] | (b[i + 2] << 8);
            s += c < 0x80 && c >= 0x20 ? static_cast<char>(c) : '?';
            i += 3;
        } else {
            s += b[i] >= 0x20 && b[i] < 0x7F ? static_cast<char>(b[i]) : ' ';
            ++i;
        }
    }
    return s;
}

struct SkelInfo {
    std::uintptr_t skel = 0;
    std::int32_t joints = -1;
    std::uint32_t layout = 0;
};
SkelInfo SkeletonInfo(std::uintptr_t skel) {
    SkelInfo s;
    if (!skel || !IsType(skel, "Skeleton")) return s;
    s.skel = skel;
    Rd(skel + kSkelJoints, s.joints);
    Rd(skel + kSkelLayoutHash, s.layout);
    return s;
}
SkelInfo ArtSkeleton(std::uintptr_t e) {
    const std::uintptr_t comp = Q(e + kEntArtParts);
    const std::uintptr_t res = comp ? Q(comp + kCompResource) : 0;
    if (!IsType(res, "ArtPartsDataResource")) return {};
    return SkeletonInfo(Q(res + kArtSkeleton));
}
const char* SkelNote(const SkelInfo& s, const SkelInfo& sam) {
    if (!s.skel) return "none";
    if (s.skel == sam.skel) return "SAM'S";
    if (s.layout && s.layout == sam.layout) return "sam-layout";
    return "other";
}

bool LocalPlayer(game::Entity** e, game::WorldTransform* wt) {
    *e = nullptr;
    if (!game::TryGetLocalPlayer(e) || !*e) return false;
    return game::TryGetTransform(*e, wt);
}

std::uintptr_t World() {
    static int ok = -1;
    if (ok < 0) {
        const std::uintptr_t b = mem::Base();
        ok = mem::CodeMatches(b + kWorldAddLock, "48 8D 9E 30 04 00 00 48 8B CB") &&
             mem::CodeMatches(b + kWorldAddArray, "48 8D 8E 38 04 00 00 48 8B D7 E8");
        if (!ok) Log("[census] World::AddEntity bytes differ; the census is unavailable");
    }
    if (!ok) return 0;
    const std::uintptr_t w = Q(mem::Base() + kWorldGlobal);
    return w && Q(w) == mem::Base() + kWorldVtable ? w : 0;
}

bool WorldList(std::vector<std::uintptr_t>* v) {
    v->clear();
    const std::uintptr_t w = World();
    if (!w) return false;
    const Arr a = ReadArr(w + kWorldEntities, 200000);
    v->resize(static_cast<std::size_t>(a.n));
    return !a.n || mem::Read(a.data, v->data(), v->size() * sizeof(std::uintptr_t));
}

std::uintptr_t AkukanResource() {
    const std::uintptr_t b = mem::Base();
    if (!mem::CodeMatches(b + kSubspaceLoad, "48 8B 3D ?? ?? ?? ??") || mem::RipTarget(b + kSubspaceLoad, 3, 7) != b + kSubspaceGlobal)
        return 0;
    const std::uintptr_t holder = Q(b + kSubspaceGlobal);
    const std::uintptr_t cfg = holder ? Q(holder) : 0;
    if (!IsType(cfg, "DSPlayerSubspaceConfig")) return 0;
    return Q(cfg + 0x28);
}

// "<AM type> res=<type> loc=.. net=<type> skel=(joints layout)"
std::string DescribeAnimManager(std::uintptr_t am, const SkelInfo& sam) {
    if (!am) return "-";
    const std::uintptr_t res = Q(am + kCompResource);
    char buf[512];
    std::string extra;
    if (Derives(res, "MorphemeAnimationManagerResource")) {
        const SkelInfo sk = SkeletonInfo(Q(res + kMamrSkeleton));
        char b2[400];
        std::snprintf(b2, sizeof(b2), " loc=\"%s\" net=%s defskel=%p(%d joints, %08X, %s)", Str(Q(res + kMamrLocation), 160).c_str(),
                      TypeOf(Q(res + kMamrNetworkDef)), reinterpret_cast<void*>(sk.skel), sk.joints, sk.layout, SkelNote(sk, sam));
        extra = b2;
    }
    std::snprintf(buf, sizeof(buf), "%s@%p res=%s@%p%s", TypeOf(am), reinterpret_cast<void*>(am), TypeOf(res), reinterpret_cast<void*>(res),
                  extra.c_str());
    return buf;
}

// ---------------------------------------------------------------- census (game thread)
struct Body {
    std::uintptr_t e = 0;
    int prio = 0;
    double dist = 0;
};
std::vector<Body> g_bodies;  // last census, for LookAt
volatile LONG g_scan_running = 0;

struct ScanArgs {
    int tag = 0;
    int mode = 0;
    std::uintptr_t akukan = 0;
    std::uintptr_t sam = 0;
    double sam_pos[3] = {};
    SkelInfo sam_skel;
    std::vector<std::uintptr_t> world;  // sorted
};

void LogAkukan(std::uintptr_t res, const SkelInfo& sam) {
    if (!res) {
        Log("[census] akukan: DSPlayerSubspaceConfig not loaded or loader bytes differ");
        return;
    }
    int nc = 0;
    const std::string comps = ListTypes(res + kResComponents, &nc);
    std::uint8_t ai = 0;
    const bool dce = Derives(res, "DSControlledEntityResource");
    if (dce) Rd(res + kResAiType, ai);
    Log("[census] akukan: resource %p %s loc=\"%s\" derives DSControlledEntityResource=%d Humanoid-ish ai=%u; %d components: %s",
        reinterpret_cast<void*>(res), TypeOf(res), Str(Q(res + kResLocation), 200).c_str(), dce ? 1 : 0, ai, nc, comps.c_str());
    // Every component resource with its most telling field.
    const Arr a = ReadArr(res + kResComponents, 512);
    for (std::int32_t i = 0; i < a.n; ++i) {
        const std::uintptr_t c = Q(a.data + 8 * static_cast<std::uintptr_t>(i));
        std::string extra;
        if (Derives(c, "MorphemeAnimationManagerResource")) {
            const SkelInfo sk = SkeletonInfo(Q(c + kMamrSkeleton));
            char b[400];
            std::snprintf(b, sizeof(b), " loc=\"%s\" net=%s skel=%p (%d joints, layout %08X, %s)", Str(Q(c + kMamrLocation), 160).c_str(),
                          TypeOf(Q(c + kMamrNetworkDef)), reinterpret_cast<void*>(sk.skel), sk.joints, sk.layout, SkelNote(sk, sam));
            extra = b;
        }
        Log("[census] akukan:   component resource %d: %p %s%s", i, reinterpret_cast<void*>(c), TypeOf(c), extra.c_str());
    }
    const std::uintptr_t model = Q(res + kResModel);
    std::string mextra;
    if (Derives(model, "SkinnedModelResource")) {
        const SkelInfo sk = SkeletonInfo(Q(model + kSmrSkeleton));
        char b[200];
        std::snprintf(b, sizeof(b), " skeleton %p (%d joints, layout %08X, %s)", reinterpret_cast<void*>(sk.skel), sk.joints, sk.layout,
                      SkelNote(sk, sam));
        mextra = b;
    }
    Log("[census] akukan: ModelResource (+0x58) %p %s%s; vtable %p (builder vt+0x80 %p)", reinterpret_cast<void*>(model), TypeOf(model),
        mextra.c_str(), reinterpret_cast<void*>(Q(res)), reinterpret_cast<void*>(Q(Q(res) + 0x80)));
}

void LogBody(int idx, std::uintptr_t e, double dist, const SkelInfo& sam_skel, std::uintptr_t akukan, bool is_sam) {
    auto* ent = reinterpret_cast<game::Entity*>(e);
    std::uint64_t flags = 0;
    Rd(e + kEntFlags, flags);
    double pos[3] = {};
    mem::Read(e + kEntPos, pos, sizeof(pos));
    const std::uintptr_t res = RefResource(e + kEntResourceRef);
    std::string name;
    std::uint8_t ai = 0;
    if (Derives(res, "DSControlledEntityResource")) {
        name = Str(Q(res + kResName), 80);
        Rd(res + kResAiType, ai);
    }
    const std::uintptr_t am = partner::FindComponentByName(ent, "AnimationManager");
    const SkelInfo sk = ArtSkeleton(e);
    const std::uintptr_t model = Q(e + kEntModel);
    int nc = 0;
    const std::string comps = ListTypes(e + kEntComponents, &nc);
    Log("[census] body #%d%s %6.1f m %p %s flags=0x%llX visible=%d hidden=%d asleep=%d pos=(%.2f, %.2f, %.2f)", idx, is_sam ? " (SAM)" : "",
        dist, reinterpret_cast<void*>(e), TypeOf(e), static_cast<unsigned long long>(flags), (flags & 0x2) ? 1 : 0, (flags & 0x4) ? 1 : 0,
        (flags & 0x100) ? 1 : 0, pos[0], pos[1], pos[2]);
    Log("[census]   resource %p %s loc=\"%s\" name=\"%s\" ai=%u%s%s", reinterpret_cast<void*>(res), res ? TypeOf(res) : "-",
        res ? Str(Q(res + kResLocation), 200).c_str() : "", name.c_str(), ai, ai == 1 ? " (Humanoid)" : "",
        res && res == akukan ? " <-- AKUKAN SAM RESOURCE" : "");
    Log("[census]   anim %s; skeleton %p (%d joints, layout %08X, %s); model %p %s", DescribeAnimManager(am, sam_skel).c_str(),
        reinterpret_cast<void*>(sk.skel), sk.joints, sk.layout, SkelNote(sk, sam_skel), reinterpret_cast<void*>(model), model ? TypeOf(model) : "");
    for (std::size_t at = 0; at < comps.size() || at == 0; at += 380)
        Log("[census]   %d components%s: %s", nc, at ? " (cont.)" : "", comps.substr(at, 380).c_str());
}

// Priority for LookAt: real NPC humanoids first, holograms last.
int Priority(std::uintptr_t e, std::uintptr_t res, std::uint8_t ai, std::uintptr_t am, const SkelInfo& sk, const SkelInfo& sam) {
    const std::string loc = res ? Str(Q(res + kResLocation), 200) : "";
    const bool holo = loc.find("holo") != std::string::npos;
    int p = 0;
    if (Derives(e, "Humanoid")) p += 8;
    if (ai == 1) p += 4;
    if (Derives(am, "MorphemeAnimationManager")) p += 4;
    if (sk.layout && sk.layout == sam.layout) p += 2;
    if (holo) p -= 10;
    return p;
}

DWORD WINAPI ScanThread(void* p);

// ---------------------------------------------------------------- image helpers (worker thread)
struct Section {
    std::uintptr_t lo = 0, hi = 0;
    char name[9] = {};
};
std::vector<Section> Sections() {
    std::vector<Section> out;
    const std::uintptr_t b = mem::Base();
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!mem::Read(b, &dos, sizeof(dos)) || !mem::Read(b + static_cast<std::uintptr_t>(dos.e_lfanew), &nt, sizeof(nt))) return out;
    const std::uintptr_t sh = b + static_cast<std::uintptr_t>(dos.e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                              nt.FileHeader.SizeOfOptionalHeader;
    for (unsigned i = 0; i < nt.FileHeader.NumberOfSections && i < 64; ++i) {
        IMAGE_SECTION_HEADER s{};
        if (!mem::Read(sh + i * sizeof(s), &s, sizeof(s))) break;
        Section x;
        x.lo = b + s.VirtualAddress;
        x.hi = x.lo + s.Misc.VirtualSize;
        std::memcpy(x.name, s.Name, 8);
        out.push_back(x);
    }
    return out;
}

enum Kind : int { kNone = 0, kAnimMgr = 1, kHumanoid = 2, kDeliveryPoint = 3, kMuleCp = 4, kMuleLoc = 5 };

// Finds every vtable (in .rdata/.data) whose slot 0 is a one-instruction GetRTTI getter
// (lea rax,[rip+X]; ret) for a wanted type. Returns vtable address -> kind.
std::unordered_map<std::uintptr_t, int> FindVtables(int mode) {
    std::unordered_map<std::uintptr_t, int> out;
    const std::vector<Section> secs = Sections();
    Section text;
    for (const Section& s : secs)
        if (std::strcmp(s.name, ".text") == 0) text = s;
    if (!text.lo) return out;
    std::vector<std::uint8_t> code(text.hi - text.lo);
    if (!mem::Read(text.lo, code.data(), code.size())) return out;
    std::unordered_map<std::uintptr_t, int> by_rtti;
    auto classify = [&](std::uintptr_t rtti) -> int {
        auto it = by_rtti.find(rtti);
        if (it != by_rtti.end()) return it->second;
        int k = kNone;
        const char* n = mem::RttiName(rtti);
        if (n) {
            if ((mode & 1) && mem::DerivesFrom(rtti, "AnimationManager")) k = kAnimMgr;
            else if ((mode & 1) && mem::DerivesFrom(rtti, "Humanoid")) k = kHumanoid;
            else if ((mode & 2) && std::strcmp(n, "DSDeliveryPointInfoResource") == 0) k = kDeliveryPoint;
            else if ((mode & 2) && std::strcmp(n, "DSMuleCPGameActorLocator") == 0) k = kMuleCp;
            else if ((mode & 2) && std::strcmp(n, "DSMuleGameActorLocator") == 0) k = kMuleLoc;
        }
        by_rtti[rtti] = k;
        return k;
    };
    for (const Section& s : secs) {
        if (std::strcmp(s.name, ".rdata") != 0 && std::strcmp(s.name, ".data") != 0) continue;
        std::vector<std::uint64_t> q((s.hi - s.lo) / 8);
        if (q.empty() || !mem::Read(s.lo, q.data(), q.size() * 8)) continue;
        for (std::size_t i = 0; i < q.size(); ++i) {
            const std::uint64_t v = q[i];
            if (v < text.lo || v >= text.hi - 8) continue;  // (no v + 8: it wraps for v near 2^64)
            const std::uint8_t* c = code.data() + (v - text.lo);
            if (c[0] != 0x48 || c[1] != 0x8D || c[2] != 0x05 || c[7] != 0xC3) continue;
            std::int32_t rel = 0;
            std::memcpy(&rel, c + 3, 4);
            const std::uintptr_t rtti = static_cast<std::uintptr_t>(v + 7 + static_cast<std::int64_t>(rel));
            if (!mem::InImage(rtti)) continue;
            const int k = classify(rtti);
            if (k != kNone) out[s.lo + i * 8] = k;
        }
    }
    return out;
}

// Calls f(region_base, buffer, size) for every committed private read-write chunk.
template <class F>
std::uint64_t ForEachChunk(F f) {
    const std::size_t kChunk = 4u << 20;
    auto* buf = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kChunk + 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!buf) return 0;
    const auto buf_lo = reinterpret_cast<std::uintptr_t>(buf);
    std::uint64_t scanned = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    for (std::uintptr_t addr = 0x10000; VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi);) {
        const auto lo = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::uintptr_t hi = lo + mbi.RegionSize;
        if (hi <= addr) break;
        addr = hi;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) ||
            mbi.Type == MEM_IMAGE || lo == buf_lo)
            continue;
        for (std::uintptr_t at = lo; at < hi; at += kChunk) {
            const std::size_t n = static_cast<std::size_t>(hi - at < kChunk ? hi - at : kChunk);
            if (!mem::Read(at, buf, n)) continue;
            scanned += n;
            f(at, buf, n);
        }
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    return scanned;
}

bool InSorted(const std::vector<std::uintptr_t>& v, std::uintptr_t x) { return std::binary_search(v.begin(), v.end(), x); }

void LogDeliveryPoint(std::uintptr_t d) {
    std::uint16_t area = 0, type = 0;
    std::uint32_t flags = 0;
    std::int32_t id = 0;
    double pos[3] = {};
    Rd(d + kDpArea, area);
    Rd(d + kDpType, type);
    Rd(d + kDpFlags, flags);
    Rd(d + kDpId, id);
    mem::Read(d + kDpTransform, pos, sizeof(pos));
    Log("[census] place: delivery point %p area=%u type=%u flags=0x%X id=%d pos=(%.2f, %.2f, %.2f) text=\"%s\"", reinterpret_cast<void*>(d), area,
        type, flags, id, pos[0], pos[1], pos[2], LocText(Q(d + kDpText), 120).c_str());
}

// WorldNode keeps its WorldTransform somewhere in the first 0x70 bytes; log the first
// plausible double triple and its offset.
void LogLocator(std::uintptr_t l, const char* what) {
    std::uint8_t b[0x70] = {};
    if (!mem::Read(l, b, sizeof(b))) return;
    std::string where = "?";
    for (std::size_t o = 0x10; o + 24 <= sizeof(b); o += 8) {
        double v[3];
        std::memcpy(v, b + o, 24);
        bool ok = true, nonzero = false;
        for (double x : v) {
            if (!std::isfinite(x) || std::fabs(x) > 20000) ok = false;
            if (std::fabs(x) > 1.0) nonzero = true;
        }
        if (ok && nonzero) {
            char c[96];
            std::snprintf(c, sizeof(c), "+0x%zX (%.2f, %.2f, %.2f)", o, v[0], v[1], v[2]);
            where = c;
            break;
        }
    }
    Log("[census] place: %s %p name=\"%s\" transform %s", what, reinterpret_cast<void*>(l), Str(Q(l + kLocName), 100).c_str(), where.c_str());
}

DWORD WINAPI ScanThread(void* p) {
    ScanArgs* a = static_cast<ScanArgs*>(p);
    const double t0 = static_cast<double>(GetTickCount64());
    const std::unordered_map<std::uintptr_t, int> vts = FindVtables(a->mode);
    int nk[6] = {};
    for (const auto& kv : vts) ++nk[kv.second];
    Log("[census] scan tag=%d: %zu vtables (%d AnimationManager, %d Humanoid, %d DeliveryPoint, %d MuleCP, %d MuleLocator) in %.1f s", a->tag,
        vts.size(), nk[kAnimMgr], nk[kHumanoid], nk[kDeliveryPoint], nk[kMuleCp], nk[kMuleLoc],
        (static_cast<double>(GetTickCount64()) - t0) / 1000.0);
    const std::uintptr_t img_lo = mem::Base();
    std::vector<std::pair<std::uintptr_t, int>> hits;
    const std::uint64_t scanned = ForEachChunk([&](std::uintptr_t at, const std::uint8_t* buf, std::size_t n) {
        const auto* q = reinterpret_cast<const std::uint64_t*>(buf);
        for (std::size_t i = 0; i < n / 8; ++i) {
            const std::uint64_t v = q[i];
            if (v - img_lo >= 0x8000000) continue;  // the image is < 128 MB
            const auto it = vts.find(static_cast<std::uintptr_t>(v));
            if (it != vts.end() && hits.size() < 200000) hits.emplace_back(at + i * 8, it->second);
        }
    });
    Log("[census] scan tag=%d: scanned %.1f GB in %.1f s, %zu vtable hits", a->tag, static_cast<double>(scanned) / 1e9,
        (static_cast<double>(GetTickCount64()) - t0) / 1000.0, hits.size());
    std::set<std::uintptr_t> owners_seen;
    int am_total = 0, am_world = 0, am_outside = 0, hum_outside = 0, places = 0, akukan_live = 0;
    std::map<std::string, int> am_by_owner_type;
    for (const auto& h : hits) {
        const std::uintptr_t o = h.first;
        if (h.second == kAnimMgr) {
            const std::uintptr_t owner = Q(o + kCompOwner);
            if (!owner || !Derives(owner, "Entity")) continue;
            // the component must be in its owner's component list
            const Arr comps = ReadArr(owner + kEntComponents, 512);
            bool listed = false;
            for (std::int32_t i = 0; i < comps.n && !listed; ++i) listed = Q(comps.data + 8 * static_cast<std::uintptr_t>(i)) == o;
            if (!listed) continue;
            ++am_total;
            ++am_by_owner_type[std::string(TypeOf(owner)) + "/" + TypeOf(o)];
            const bool in_world = InSorted(a->world, owner);
            in_world ? ++am_world : ++am_outside;
            const std::uintptr_t res = RefResource(owner + kEntResourceRef);
            if (res && res == a->akukan) ++akukan_live;
            if (in_world && res != a->akukan) continue;
            if (!owners_seen.insert(owner).second || am_outside > 300) continue;
            double pos[3] = {};
            mem::Read(owner + kEntPos, pos, sizeof(pos));
            const double dx = pos[0] - a->sam_pos[0], dy = pos[1] - a->sam_pos[1], dz = pos[2] - a->sam_pos[2];
            std::uint64_t flags = 0;
            Rd(owner + kEntFlags, flags);
            const SkelInfo sk = ArtSkeleton(owner);
            int nc = 0;
            const std::string cl = ListTypes(owner + kEntComponents, &nc);
            Log("[census] outside-list AM: owner %p %s in_world=%d dist=%.1f m flags=0x%llX visible=%d asleep=%d res=%s loc=\"%s\"%s; anim %s; "
                "skeleton (%d joints, %08X, %s); %d components: %s",
                reinterpret_cast<void*>(owner), TypeOf(owner), in_world ? 1 : 0, std::sqrt(dx * dx + dy * dy + dz * dz),
                static_cast<unsigned long long>(flags), (flags & 2) ? 1 : 0, (flags & 0x100) ? 1 : 0, res ? TypeOf(res) : "-",
                res ? Str(Q(res + kResLocation), 160).c_str() : "", res && res == a->akukan ? " <-- AKUKAN" : "",
                DescribeAnimManager(o, a->sam_skel).c_str(), sk.joints, sk.layout, SkelNote(sk, a->sam_skel), nc, cl.c_str());
        } else if (h.second == kHumanoid) {
            if (InSorted(a->world, o) || mem::RttiOf(o) == 0) continue;
            if (!owners_seen.insert(o).second) continue;
            ++hum_outside;
            const std::uintptr_t res = RefResource(o + kEntResourceRef);
            double pos[3] = {};
            mem::Read(o + kEntPos, pos, sizeof(pos));
            Log("[census] outside-list Humanoid: %p %s pos=(%.2f, %.2f, %.2f) res=%s loc=\"%s\"", reinterpret_cast<void*>(o), TypeOf(o), pos[0],
                pos[1], pos[2], res ? TypeOf(res) : "-", res ? Str(Q(res + kResLocation), 160).c_str() : "");
        } else if (h.second == kDeliveryPoint) {
            if (!IsType(o, "DSDeliveryPointInfoResource") || places > 600) continue;
            ++places;
            LogDeliveryPoint(o);
        } else if (h.second == kMuleCp || h.second == kMuleLoc) {
            if (places > 600) continue;
            ++places;
            LogLocator(o, h.second == kMuleCp ? "MULE camp locator" : "MULE locator");
        }
    }
    std::string types;
    for (const auto& kv : am_by_owner_type) types += (types.empty() ? "" : ", ") + kv.first + "=" + std::to_string(kv.second);
    Log("[census] scan tag=%d: %d AnimationManager components (%d owners in the world list, %d outside), %d Humanoids outside the list, "
        "%d places, %d live entities from the akukan resource; by owner/manager type: %s",
        a->tag, am_total, am_world, am_outside, hum_outside, places, akukan_live, types.c_str());
    Log("[census] done tag=%d (scan %.1f s)", a->tag, (static_cast<double>(GetTickCount64()) - t0) / 1000.0);
    delete a;
    InterlockedExchange(&g_scan_running, 0);
    return 0;
}

// ---------------------------------------------------------------- pose dumps
struct Block {
    std::uintptr_t addr;
    std::uint32_t size;
    std::string label;
    std::vector<std::uint8_t> data;
};

bool HeapPtr(std::uint64_t v) { return v >= 0x10000 && v < 0x7FFFFFFF0000ull && !mem::InImage(static_cast<std::uintptr_t>(v)) && (v & 7) == 0; }

bool AddBlock(std::vector<Block>* out, std::uintptr_t addr, std::uint32_t size, const std::string& label, std::size_t* total) {
    if (*total > (48u << 20)) return false;
    Block b{addr, size, label, std::vector<std::uint8_t>(size)};
    // shrink until readable (end of an allocation / page)
    while (b.size >= 0x40 && !mem::Read(addr, b.data.data(), b.size)) b.size /= 2;
    if (b.size < 0x40) return false;
    b.data.resize(b.size);
    *total += b.size;
    out->push_back(std::move(b));
    return true;
}

int g_pd_tag = 0, g_pd_left = 0, g_pd_index = 0;
double g_pd_at = 0, g_now = 0;
std::uintptr_t g_pd_target = 0;  // 0 = Sam, else a census body (re-checked in the world list)

void DoPoseDump() {
    game::Entity* sam = nullptr;
    game::WorldTransform wt{};
    if (g_pd_target) {
        std::vector<std::uintptr_t> world;
        if (!WorldList(&world) || std::find(world.begin(), world.end(), g_pd_target) == world.end()) {
            Log("[pose] dump tag=%d: body %p left the world", g_pd_tag, reinterpret_cast<void*>(g_pd_target));
            return;
        }
        sam = reinterpret_cast<game::Entity*>(g_pd_target);
        mem::Read(g_pd_target + kEntPos, &wt, 0x3C);
    } else if (!LocalPlayer(&sam, &wt)) {
        Log("[pose] dump tag=%d: no local player", g_pd_tag);
        return;
    }
    const auto s = reinterpret_cast<std::uintptr_t>(sam);
    const std::uintptr_t am = partner::FindComponentByName(sam, "AnimationManager");
    const std::uintptr_t amres = am ? Q(am + kCompResource) : 0;
    const std::uintptr_t model = Q(s + kEntModel);
    const std::uintptr_t modelres = model ? Q(model + kCompResource) : 0;
    const SkelInfo sk = ArtSkeleton(s);
    std::vector<Block> blocks;
    std::size_t total = 0;
    struct Root {
        const char* name;
        std::uintptr_t a;
        std::uint32_t size;
    } roots[] = {{"am", am, 0x400}, {"amres", amres, 0x110}, {"model", model, 0x400}, {"modelres", modelres, 0xC0}, {"sam", s, 0x200}};
    // Level 0 roots, level 1 every heap pointer inside them (0x1400 = 77*0x40 + spare), level 2 heap
    // pointers in the first 0x100 bytes of level-1 blocks (0x400). Every Decima array {i32 n, i32 cap,
    // ptr} with 70 <= n <= 80 (or a multiple of 77 up to 4x) anywhere is dumped in full (n*0x40).
    std::set<std::uintptr_t> seen;
    std::vector<std::string> arrays;
    auto scan_arrays = [&](const Block& b) {
        for (std::size_t o = 0; o + 16 <= b.data.size(); o += 8) {
            std::int32_t n = 0, cap = 0;
            std::uint64_t ptr = 0;
            std::memcpy(&n, b.data.data() + o, 4);
            std::memcpy(&cap, b.data.data() + o + 4, 4);
            std::memcpy(&ptr, b.data.data() + o + 8, 8);
            const bool near77 = (n >= 70 && n <= 80) || n == 154 || n == 231 || n == 308;
            if (!near77 || cap < n || cap > 4096 || !HeapPtr(ptr)) continue;
            char lab[160];
            std::snprintf(lab, sizeof(lab), "%s+0x%zX[arr n=%d]", b.label.c_str(), o, n);
            if (seen.insert(static_cast<std::uintptr_t>(ptr)).second &&
                AddBlock(&blocks, static_cast<std::uintptr_t>(ptr), static_cast<std::uint32_t>(std::min<std::int64_t>(n * 0x40LL, 0x8000)), lab, &total))
                arrays.push_back(std::string(lab) + "@" + std::to_string(ptr));
        }
    };
    for (const Root& r : roots) {
        if (!r.a) continue;
        if (AddBlock(&blocks, r.a, r.size, r.name, &total)) seen.insert(r.a);
    }
    const std::size_t n0 = blocks.size();
    for (std::size_t bi = 0; bi < n0; ++bi) {
        if (blocks[bi].label == "sam") continue;
        for (std::size_t o = 0; o + 8 <= blocks[bi].data.size(); o += 8) {
            std::uint64_t v = 0;
            std::memcpy(&v, blocks[bi].data.data() + o, 8);
            if (!HeapPtr(v) || !seen.insert(static_cast<std::uintptr_t>(v)).second) continue;
            char lab[96];
            std::snprintf(lab, sizeof(lab), "%s+0x%zX", blocks[bi].label.c_str(), o);
            AddBlock(&blocks, static_cast<std::uintptr_t>(v), 0x1400, lab, &total);
        }
    }
    const std::size_t n1 = blocks.size();
    for (std::size_t bi = n0; bi < n1; ++bi) {
        for (std::size_t o = 0; o + 8 <= blocks[bi].data.size() && o < 0x100; o += 8) {
            std::uint64_t v = 0;
            std::memcpy(&v, blocks[bi].data.data() + o, 8);
            if (!HeapPtr(v) || !seen.insert(static_cast<std::uintptr_t>(v)).second) continue;
            char lab[128];
            std::snprintf(lab, sizeof(lab), "%s>+0x%zX", blocks[bi].label.c_str(), o);
            AddBlock(&blocks, static_cast<std::uintptr_t>(v), 0x400, lab, &total);
        }
    }
    const std::size_t n2 = blocks.size();
    for (std::size_t bi = 0; bi < n2; ++bi) scan_arrays(blocks[bi]);
    // References to Sam's Skeleton object inside the dumped blocks (a Pose has Ref<Skeleton> at +0x20).
    std::string skel_refs;
    int nrefs = 0;
    for (const Block& b : blocks)
        for (std::size_t o = 0; o + 8 <= b.data.size(); o += 8) {
            std::uint64_t v = 0;
            std::memcpy(&v, b.data.data() + o, 8);
            if (sk.skel && v == sk.skel && nrefs++ < 40) {
                char c[160];
                std::snprintf(c, sizeof(c), "%s%s+0x%zX", skel_refs.empty() ? "" : ", ", b.label.c_str(), o);
                skel_refs += c;
            }
        }
    // File: "STPD" u32 version, then per block u64 addr, u32 size, u16 label length, label, data.
    wchar_t name[96];
    std::swprintf(name, 96, L"\\pose-%d-%d.bin", g_pd_tag, g_pd_index);
    const std::wstring dir = automation::OutDir();
    bool wrote = false;
    if (!dir.empty()) {
        const HANDLE h = CreateFileW((dir + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            wrote = true;
            const char magic[8] = {'S', 'T', 'P', 'D', 1, 0, 0, 0};
            wrote &= WriteFile(h, magic, 8, &w, nullptr) != 0;
            // header block: Sam's world transform (0x40 bytes) as a pseudo block at address 0
            Block hdr{0, 0x40, "samxf", std::vector<std::uint8_t>(0x40)};
            std::memcpy(hdr.data.data(), &wt, 0x3C);
            blocks.insert(blocks.begin(), std::move(hdr));
            for (const Block& b : blocks) {
                const std::uint64_t addr = b.addr;
                const std::uint32_t size = b.size;
                const auto ll = static_cast<std::uint16_t>(b.label.size());
                wrote &= WriteFile(h, &addr, 8, &w, nullptr) != 0;
                wrote &= WriteFile(h, &size, 4, &w, nullptr) != 0;
                wrote &= WriteFile(h, &ll, 2, &w, nullptr) != 0;
                wrote &= WriteFile(h, b.label.data(), ll, &w, nullptr) != 0;
                wrote &= WriteFile(h, b.data.data(), size, &w, nullptr) != 0;
            }
            CloseHandle(h);
        }
    }
    std::string arr;
    for (const std::string& x : arrays) arr += (arr.empty() ? "" : ", ") + x;
    std::uint64_t flags = 0;
    Rd(s + kEntFlags, flags);
    Log("[pose] dump tag=%d #%d t=%.0f ms: %s %p pos=(%.3f, %.3f, %.3f) flags=0x%llX; AM %s; model %p %s res %p %s; skeleton %p (%d joints, "
        "%08X); %zu blocks %.1f MB -> pose-%d-%d.bin %s",
        g_pd_tag, g_pd_index, g_now, g_pd_target ? TypeOf(s) : "Sam", reinterpret_cast<void*>(s), wt.pos.x, wt.pos.y, wt.pos.z, static_cast<unsigned long long>(flags),
        DescribeAnimManager(am, sk).c_str(), reinterpret_cast<void*>(model), model ? TypeOf(model) : "", reinterpret_cast<void*>(modelres),
        modelres ? TypeOf(modelres) : "", reinterpret_cast<void*>(sk.skel), sk.joints, sk.layout, blocks.size(),
        static_cast<double>(total) / 1e6, g_pd_tag, g_pd_index, wrote ? "written" : "NOT written");
    Log("[pose]   arrays of ~77: %s", arr.empty() ? "none" : arr.c_str());
    Log("[pose]   Sam's Skeleton %p referenced at: %s", reinterpret_cast<void*>(sk.skel), skel_refs.empty() ? "nowhere" : skel_refs.c_str());
}

// ---------------------------------------------------------------- pose scan (worker)
struct PoseScanArgs {
    int tag = 0;
    double pos[3] = {};
    std::uintptr_t skel = 0;
};
volatile LONG g_pscan_running = 0;

DWORD WINAPI PoseScanThread(void* p) {
    const PoseScanArgs a = *static_cast<PoseScanArgs*>(p);
    delete static_cast<PoseScanArgs*>(p);
    const double t0 = static_cast<double>(GetTickCount64());
    const float fx = static_cast<float>(a.pos[0]), fy = static_cast<float>(a.pos[1]), fz = static_cast<float>(a.pos[2]);
    constexpr float kR = 3.0f;
    // Window statistics: 0x4000-byte windows with >= 16 hits of one pattern.
    struct Win {
        int vec3 = 0, col4 = 0, dbl3 = 0, skel = 0;
        std::uintptr_t first = 0;
    };
    std::map<std::uintptr_t, Win> wins;
    std::vector<std::uintptr_t> skel_hits;
    const std::uint64_t scanned = ForEachChunk([&](std::uintptr_t at, const std::uint8_t* buf, std::size_t n) {
        const auto* f = reinterpret_cast<const float*>(buf);
        const std::size_t nf = n / 4;
        for (std::size_t i = 0; i + 12 < nf; ++i) {
            const float x = f[i];
            if (!(x > fx - kR && x < fx + kR)) continue;
            const std::uintptr_t addr = at + i * 4;
            Win& w = wins[addr & ~static_cast<std::uintptr_t>(0x3FFF)];
            if (!w.first) w.first = addr;
            if (f[i + 1] > fy - kR && f[i + 1] < fy + kR && f[i + 2] > fz - kR && f[i + 2] < fz + kR) ++w.vec3;
            if (f[i + 4] > fy - kR && f[i + 4] < fy + kR && f[i + 8] > fz - kR && f[i + 8] < fz + kR) ++w.col4;
        }
        const auto* d = reinterpret_cast<const double*>(buf);
        for (std::size_t i = 0; i + 3 < n / 8; ++i) {
            const double x = d[i];
            const std::uintptr_t addr = at + i * 8;
            if (a.skel && reinterpret_cast<const std::uint64_t*>(buf)[i] == a.skel) {
                if (skel_hits.size() < 4000) skel_hits.push_back(addr);
                ++wins[addr & ~static_cast<std::uintptr_t>(0x3FFF)].skel;
            }
            if (!(x > a.pos[0] - kR && x < a.pos[0] + kR)) continue;
            if (d[i + 1] > a.pos[1] - kR && d[i + 1] < a.pos[1] + kR && d[i + 2] > a.pos[2] - kR && d[i + 2] < a.pos[2] + kR)
                ++wins[addr & ~static_cast<std::uintptr_t>(0x3FFF)].dbl3;
        }
    });
    Log("[pose] scan tag=%d: scanned %.1f GB in %.1f s around (%.2f, %.2f, %.2f); %zu windows with x hits; %zu Skeleton refs", a.tag,
        static_cast<double>(scanned) / 1e9, (static_cast<double>(GetTickCount64()) - t0) / 1000.0, a.pos[0], a.pos[1], a.pos[2], wins.size(),
        skel_hits.size());
    int logged = 0;
    std::vector<std::pair<std::uintptr_t, std::uint32_t>> keep;
    for (const auto& kv : wins) {
        const Win& w = kv.second;
        if (w.vec3 < 16 && w.col4 < 16 && w.dbl3 < 8) continue;
        if (logged++ < 300)
            Log("[pose] scan tag=%d window %p: vec3=%d col4(3x4 translation)=%d dbl3=%d skelrefs=%d first=%p", a.tag, reinterpret_cast<void*>(kv.first),
                w.vec3, w.col4, w.dbl3, w.skel, reinterpret_cast<void*>(w.first));
        if (keep.size() < 400) keep.emplace_back(kv.first, 0x4000u);
    }
    std::string sh;
    for (std::size_t i = 0; i < skel_hits.size() && i < 200; ++i) {
        char c[24];
        std::snprintf(c, sizeof(c), "%s%p", sh.empty() ? "" : " ", reinterpret_cast<void*>(skel_hits[i]));
        sh += c;
        // context around the ref (a Pose would start 0x20 before)
        if (keep.size() < 800) keep.emplace_back((skel_hits[i] - 0x40) & ~static_cast<std::uintptr_t>(7), 0x100u);
    }
    Log("[pose] scan tag=%d Skeleton %p refs: %s", a.tag, reinterpret_cast<void*>(a.skel), sh.c_str());
    // Save the windows (and skeleton-ref contexts) for offline comparison.
    wchar_t name[96];
    std::swprintf(name, 96, L"\\posescan-%d.bin", a.tag);
    const std::wstring dir = automation::OutDir();
    std::size_t bytes = 0;
    if (!dir.empty()) {
        const HANDLE h = CreateFileW((dir + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            const char magic[8] = {'S', 'T', 'P', 'D', 1, 0, 0, 0};
            WriteFile(h, magic, 8, &w, nullptr);
            std::vector<std::uint8_t> buf;
            for (const auto& k : keep) {
                buf.resize(k.second);
                if (!mem::Read(k.first, buf.data(), buf.size())) continue;
                const std::uint64_t addr = k.first;
                const std::uint32_t size = k.second;
                const std::string label = size == 0x4000 ? "win" : "skelref";
                const auto ll = static_cast<std::uint16_t>(label.size());
                WriteFile(h, &addr, 8, &w, nullptr);
                WriteFile(h, &size, 4, &w, nullptr);
                WriteFile(h, &ll, 2, &w, nullptr);
                WriteFile(h, label.data(), ll, &w, nullptr);
                WriteFile(h, buf.data(), size, &w, nullptr);
                bytes += size;
            }
            CloseHandle(h);
        }
    }
    Log("[pose] scan done tag=%d: %d windows logged, %zu bytes saved to posescan-%d.bin", a.tag, logged, bytes, a.tag);
    InterlockedExchange(&g_pscan_running, 0);
    return 0;
}

}  // namespace

// ---------------------------------------------------------------- public
bool StartAnimCensus(double radius_m, int tag, int scan, char* msg, std::size_t cap) noexcept {
    auto say = [&](const char* fmt, auto... args) {
        if (msg && cap) std::snprintf(msg, cap, fmt, args...);
    };
    if (scan && InterlockedCompareExchange(&g_scan_running, 1, 0) != 0) {
        say("a census scan is still running");
        return false;
    }
    game::Entity* sam = nullptr;
    game::WorldTransform swt{};
    const bool have_sam = LocalPlayer(&sam, &swt);
    const SkelInfo sam_skel = have_sam ? ArtSkeleton(reinterpret_cast<std::uintptr_t>(sam)) : SkelInfo{};
    const std::uintptr_t akukan = AkukanResource();
    std::vector<std::uintptr_t> world;
    if (!WorldList(&world)) {
        if (scan) InterlockedExchange(&g_scan_running, 0);
        say("world list not available");
        Log("[census] tag=%d: world list not available", tag);
        return false;
    }
    Log("[census] tag=%d: Sam %p at (%.2f, %.2f, %.2f), skeleton %p (%d joints, %08X); world holds %zu ControlledEntities; radius %.0f m", tag,
        reinterpret_cast<void*>(sam), swt.pos.x, swt.pos.y, swt.pos.z, reinterpret_cast<void*>(sam_skel.skel), sam_skel.joints, sam_skel.layout,
        world.size(), radius_m);
    LogAkukan(akukan, sam_skel);
    std::map<std::string, int> by_type, am_types;
    int bodies = 0, with_am = 0, humanoids = 0, akukan_live = 0;
    std::vector<Body> list;
    for (const std::uintptr_t e : world) {
        ++by_type[TypeOf(e)];
        auto* ent = reinterpret_cast<game::Entity*>(e);
        const std::uintptr_t am = partner::FindComponentByName(ent, "AnimationManager");
        const std::uintptr_t res = RefResource(e + kEntResourceRef);
        if (res && res == akukan) ++akukan_live;
        std::uint8_t ai = 0;
        if (Derives(res, "DSControlledEntityResource")) Rd(res + kResAiType, ai);
        const bool humanoid = Derives(e, "Humanoid");
        const SkelInfo sk = ArtSkeleton(e);
        const bool body77 = sk.joints == 77 && IsType(Q(e + kEntModel), "SkinnedModel");
        if (am) {
            ++with_am;
            ++am_types[TypeOf(am)];
        }
        if (humanoid) ++humanoids;
        if (!am && !humanoid && ai != 1 && !body77) continue;
        double pos[3] = {};
        mem::Read(e + kEntPos, pos, sizeof(pos));
        const double dx = pos[0] - swt.pos.x, dy = pos[1] - swt.pos.y, dz = pos[2] - swt.pos.z;
        const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (radius_m > 0 && dist > radius_m) continue;
        list.push_back({e, Priority(e, res, ai, am, sk, sam_skel), dist});
    }
    std::stable_sort(list.begin(), list.end(), [](const Body& a, const Body& b) {
        if (a.prio != b.prio) return a.prio > b.prio;
        return a.dist < b.dist;
    });
    g_bodies.clear();
    for (const Body& b : list) {
        const bool is_sam = b.e == reinterpret_cast<std::uintptr_t>(sam);
        if (bodies < 150) LogBody(bodies, b.e, b.dist, sam_skel, akukan, is_sam);
        ++bodies;
        if (!is_sam) g_bodies.push_back(b);
    }
    std::string types, ams;
    for (const auto& kv : by_type) types += (types.empty() ? "" : ", ") + kv.first + "=" + std::to_string(kv.second);
    for (const auto& kv : am_types) ams += (ams.empty() ? "" : ", ") + kv.first + "=" + std::to_string(kv.second);
    Log("[census] tag=%d summary: %zu entities (%s); %d with an AnimationManager (%s); %d Humanoid-derived; %d bodies logged; %d live from the "
        "akukan resource",
        tag, world.size(), types.c_str(), with_am, ams.c_str(), humanoids, bodies, akukan_live);
    for (std::size_t i = 0; i < g_bodies.size() && i < 12; ++i) {
        const std::uintptr_t res = RefResource(g_bodies[i].e + kEntResourceRef);
        Log("[census] tag=%d look_at %zu -> %p %s prio=%d %.1f m loc=\"%s\"", tag, i, reinterpret_cast<void*>(g_bodies[i].e), TypeOf(g_bodies[i].e),
            g_bodies[i].prio, g_bodies[i].dist, res ? Str(Q(res + kResLocation), 120).c_str() : "");
    }
    if (!scan) {
        Log("[census] done tag=%d (no scan)", tag);
        say("census tag %d: %d bodies, %d with AnimationManager", tag, bodies, with_am);
        return true;
    }
    auto* args = new (std::nothrow) ScanArgs;
    if (!args) {
        InterlockedExchange(&g_scan_running, 0);
        say("out of memory");
        return false;
    }
    args->tag = tag;
    args->mode = scan;
    args->akukan = akukan;
    args->sam = reinterpret_cast<std::uintptr_t>(sam);
    args->sam_pos[0] = swt.pos.x;
    args->sam_pos[1] = swt.pos.y;
    args->sam_pos[2] = swt.pos.z;
    args->sam_skel = sam_skel;
    args->world = world;
    std::sort(args->world.begin(), args->world.end());
    HANDLE h = CreateThread(nullptr, 0, &ScanThread, args, 0, nullptr);
    if (!h) {
        delete args;
        InterlockedExchange(&g_scan_running, 0);
        say("could not start the scan thread");
        return false;
    }
    CloseHandle(h);
    say("census tag %d: %d bodies, %d with AnimationManager; memory scan running", tag, bodies, with_am);
    return true;
}

bool PoseDump(int tag, int frames, double delay_ms, char* msg, std::size_t cap) noexcept {
    if (g_pd_left > 0) {
        if (msg && cap) std::snprintf(msg, cap, "a pose dump is pending");
        return false;
    }
    g_pd_tag = tag;
    g_pd_target = 0;
    if (tag >= 1000) {  // tag 1000 + n: census body n instead of Sam
        const std::size_t n = static_cast<std::size_t>(tag - 1000);
        if (n >= g_bodies.size()) {
            if (msg && cap) std::snprintf(msg, cap, "no body %zu in the last census", n);
            return false;
        }
        g_pd_target = g_bodies[n].e;
    }
    g_pd_left = frames < 1 ? 1 : (frames > 8 ? 8 : frames);
    g_pd_index = 0;
    g_pd_at = g_now + (delay_ms > 0 ? delay_ms : 0);
    if (msg && cap) std::snprintf(msg, cap, "pose dump tag %d: %d frame(s) after %.0f ms", tag, g_pd_left, delay_ms);
    if (delay_ms <= 0) Tick(g_now);
    return true;
}

bool StartPoseScan(int tag, char* msg, std::size_t cap) noexcept {
    if (InterlockedCompareExchange(&g_pscan_running, 1, 0) != 0) {
        if (msg && cap) std::snprintf(msg, cap, "a pose scan is running");
        return false;
    }
    game::Entity* sam = nullptr;
    game::WorldTransform wt{};
    if (!LocalPlayer(&sam, &wt)) {
        InterlockedExchange(&g_pscan_running, 0);
        if (msg && cap) std::snprintf(msg, cap, "no local player");
        return false;
    }
    auto* a = new (std::nothrow) PoseScanArgs;
    if (!a) {
        InterlockedExchange(&g_pscan_running, 0);
        return false;
    }
    a->tag = tag;
    a->pos[0] = wt.pos.x;
    a->pos[1] = wt.pos.y;
    a->pos[2] = wt.pos.z;
    a->skel = ArtSkeleton(reinterpret_cast<std::uintptr_t>(sam)).skel;
    HANDLE h = CreateThread(nullptr, 0, &PoseScanThread, a, 0, nullptr);
    if (!h) {
        delete a;
        InterlockedExchange(&g_pscan_running, 0);
        if (msg && cap) std::snprintf(msg, cap, "could not start the scan thread");
        return false;
    }
    CloseHandle(h);
    if (msg && cap) std::snprintf(msg, cap, "pose scan tag %d running", tag);
    return true;
}

bool LookAt(int index, double dist_m, char* msg, std::size_t cap) noexcept {
    if (index < 0 || static_cast<std::size_t>(index) >= g_bodies.size()) {
        if (msg && cap) std::snprintf(msg, cap, "no body %d in the last census (%zu)", index, g_bodies.size());
        return false;
    }
    const std::uintptr_t e = g_bodies[static_cast<std::size_t>(index)].e;
    std::vector<std::uintptr_t> world;
    if (!WorldList(&world) || std::find(world.begin(), world.end(), e) == world.end()) {
        if (msg && cap) std::snprintf(msg, cap, "body %d (%p) left the world", index, reinterpret_cast<void*>(e));
        return false;
    }
    double pos[3] = {};
    game::Entity* sam = nullptr;
    game::WorldTransform wt{};
    if (!mem::Read(e + kEntPos, pos, sizeof(pos)) || !LocalPlayer(&sam, &wt)) {
        if (msg && cap) std::snprintf(msg, cap, "positions unreadable");
        return false;
    }
    double fx = wt.rot.m[1][0], fy = wt.rot.m[1][1];
    double fl = std::sqrt(fx * fx + fy * fy);
    if (fl < 1e-3) fx = 1, fy = 0, fl = 1;
    fx /= fl, fy /= fl;
    const double d = dist_m > 0.5 && dist_m < 50 ? dist_m : 4.0;
    const double x = pos[0] - fx * d, y = pos[1] - fy * d, z = pos[2] + 0.3;
    char why[200] = {};
    const bool ok = game::TeleportLocalPlayer(x, y, z, game::TeleportMode::place, why, sizeof(why));
    Log("[census] look_at %d: body %p %s at (%.2f, %.2f, %.2f); Sam to (%.2f, %.2f, %.2f), %.1f m behind it along his facing: %s %s", index,
        reinterpret_cast<void*>(e), TypeOf(e), pos[0], pos[1], pos[2], x, y, z, d, ok ? "ok" : "FAILED", why);
    if (msg && cap) std::snprintf(msg, cap, "look_at %d %s: %s", index, ok ? "ok" : "failed", why);
    return ok;
}

void Tick(double now_ms) noexcept {
    g_now = now_ms;
    if (g_pd_left <= 0 || now_ms < g_pd_at) return;
    DoPoseDump();
    ++g_pd_index;
    --g_pd_left;
}

}  // namespace st::census
