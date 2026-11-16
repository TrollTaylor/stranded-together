// Hologram catalog: see holo_catalog.hpp and docs/character-catalog.md.
#include "holo_catalog.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <vector>

#include "automation.hpp"
#include "game_api.hpp"
#include "log.hpp"
#include "overlay/overlay.hpp"
#include "overlay/overlay_renderer.hpp"
#include "partner_holo.hpp"
#include "partner_source.hpp"

namespace st::catalog {
namespace {

namespace mem = partner::mem;
using mem::Q;
using mem::Rd;

// ---------------------------------------------------------------- layout (docs/character-catalog.md)
// DSCollectorsItemResource (RTTI 0x144DF3B80, 0xA0 bytes)
constexpr std::uintptr_t kItemHousehold = 0x28;  // Ref<DSHouseholdInfoResource>; +0x44 MajorMemberId (u8 enum)
constexpr std::uintptr_t kItemNameText = 0x30;   // Ref<LocalizedTextResource>
constexpr std::uintptr_t kItemText = 0x40;       // Ref<LocalizedTextResource>
constexpr std::uintptr_t kHouseholdMember = 0x44;
// LocalizedTextResource: +0x20 byte text, +0x28 u16 byte length (0x141DEBD70 -> 0x141918FF0);
// a byte 0x01 is followed by one UTF-16 unit (lo, hi), any other byte is itself (0x14191D260).
constexpr std::uintptr_t kLocText = 0x20;
constexpr std::uintptr_t kLocLen = 0x28;
// EntityResource / DSControlledEntityResource
constexpr std::uintptr_t kResLocation = 0x30;     // String (StreamingRefTarget.Location)
constexpr std::uintptr_t kResComponents = 0x90;   // Array<Ref<EntityComponentResource>>: i32 count, data +0x98
constexpr std::uintptr_t kResName = 0x130;        // String (DSControlledEntityResource.Name)
constexpr std::uintptr_t kResAiType = 0x150;      // EAIEntityType (1 = Humanoid)
// Entity
constexpr std::uintptr_t kEntFlags = 0x88;
constexpr std::uintptr_t kEntResourceRef = 0x58;  // StreamingRef<EntityResource>
constexpr std::uintptr_t kEntPos = 0xC8;          // 3 doubles
constexpr std::uintptr_t kEntArtParts = 0x180;    // ArtPartsData component (ArtPartsData_sExportedSetShaderVariableAsFloat)
constexpr std::uintptr_t kCompResource = 0x30;
constexpr std::uintptr_t kArtSkeleton = 0x40;     // ArtPartsDataResource.Skeleton
constexpr std::uintptr_t kSkelJoints = 0x20;      // Skeleton.Joints count (i32)
constexpr std::uintptr_t kSkelLayoutHash = 0x70;  // Skeleton.SkeletonLayoutHash
// DSSimpleAnimationComponent (RTTI 0x144BAAC00, 0x140 bytes) and its resource
constexpr std::uintptr_t kSaPlayer = 0x50;        // clip player: [p] clip, p+8 state (1 playing), +0xC time, +0x10 rate, +0x14 mode
constexpr std::uintptr_t kSaCurrent = 0x12C;      // current clip index
constexpr std::uintptr_t kSaGestureTimer = 0x9C;  // float; the welcome holo starts a gesture only at 0
constexpr std::uintptr_t kSarClips = 0x20;        // Array<Ref<SkeletonAnimationResource>>
constexpr std::uintptr_t kSarInterp = 0x30;       // Array<float> AnimationInterpSec
constexpr std::uintptr_t kSarLoopDefault = 0x50;  // bool LoopDefaultAnimation, +0x51 RandomizDefaultAnimation, +0x54 float
// SkeletonAnimationResource (0xA0 bytes)
constexpr std::uintptr_t kClipLocation = 0x28;
constexpr std::uintptr_t kClipSkeleton = 0x38;
constexpr std::uintptr_t kClipSampleRate = 0x50;
constexpr std::uintptr_t kClipDuration = 0x54;
constexpr std::uintptr_t kClipLoop = 0x58;        // ELoopMode: 0 Off, 1 On, 2 Hold, 3 PingPong
constexpr std::uintptr_t kClipDeltaRot = 0x60;    // Quat
constexpr std::uintptr_t kClipDeltaPos = 0x70;    // Vec3 LocomotionDeltaTranslation
// ShaderAnimComponentResource: three Array<Ref<ShaderAnimCurveSet>> (+0x20 start, +0x30 loop, +0x40 end);
// ShaderAnimCurveSet +0x20 VariableID, +0x28 ModelPartName (Strings)
// DSPhotoModeModelOpacityComponentResource +0x20 ShaderVariableName (String)
// Census
constexpr std::uintptr_t kWorldGlobal = 0x7BC4E70;      // checked by partner_holo (RemoveEntity decodes it)
constexpr std::uintptr_t kWorldVtable = 0x3D5D140;
constexpr std::uintptr_t kWorldAddLock = 0x357E024;     // lea rbx,[rsi+0x430] in World::AddEntity 0x14357DFB0
constexpr std::uintptr_t kWorldAddArray = 0x357E041;    // lea rcx,[rsi+0x438]
constexpr std::uintptr_t kWorldEntities = 0x438;        // i32 count, data +0x440 (ControlledEntity*)
constexpr std::uintptr_t kSubspaceLoad = 0x26A27AB;     // mov rdi,[rip+X] (X = 0x147BE79B0); [rdi] = DSPlayerSubspaceConfig
constexpr std::uintptr_t kSubspaceGlobal = 0x7BE79B0;
constexpr std::uintptr_t kPlayerWakeOverride = 0x2757500;

constexpr double kDefaultDistanceM = 5.0;
// Placement: Sam's rotation rows are (right, forward, up); row 1 is the direction he walks (checked in
// run 20261116-165905). The camera sits behind and to the right of Sam, so the item stands ahead and to
// the right, clear of his body in the shot.
constexpr double kSideM = 1.5;
constexpr int kForwardRow = 1;
constexpr double kLoadTimeoutMs = 15000;
constexpr double kSettleMs = 1500;
constexpr double kCaptureTimeoutMs = 8000;
constexpr double kGapMs = 400;
constexpr std::uint8_t kTypeHolo = 4, kTypeSpecialHolo = 0x14;

const char* const kMembers[] = {"None",     "Diehardman", "Deadman",             "Heartman",
                                "Mama",     "Lockne",     "Fragile",             "BB",
                                "BT",       "BridgesOrganization", "PorterMule", "ChiralArtistDaughter",
                                "Langdon"};

// ---------------------------------------------------------------- small readers
std::string Clean(const char* p, std::size_t n) {
    std::string s(p, n);
    for (char& c : s)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = ' ';
    return s;
}

// Decima String: char* to NUL-terminated text; i32 length at -8 (0x14190C410).
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

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

const char* TypeOf(std::uintptr_t obj) {
    const char* n = obj ? partner::TypeName(reinterpret_cast<const void*>(obj)) : nullptr;
    return n ? n : "?";
}

bool IsType(std::uintptr_t obj, const char* name) { return obj && std::strcmp(TypeOf(obj), name) == 0; }

// LocalizedTextResource -> UTF-8 (current language), newlines shown as " / ".
std::string LocText(std::uintptr_t lt, std::size_t max_chars) {
    if (!lt) return "-";
    if (!IsType(lt, "LocalizedTextResource")) return std::string("?") + TypeOf(lt);
    const std::uintptr_t p = Q(lt + kLocText);
    std::uint16_t len = 0;
    if (!p || !Rd(lt + kLocLen, len) || !len) return "";
    std::vector<std::uint8_t> b(len);
    if (!mem::Read(p, b.data(), b.size())) return "?unreadable";
    std::wstring w;
    for (std::size_t i = 0; i < b.size() && b[i] && w.size() < max_chars;) {
        if (b[i] == 1 && i + 2 < b.size()) {
            w += static_cast<wchar_t>(b[i + 1] | (b[i + 2] << 8));
            i += 3;
        } else {
            w += static_cast<wchar_t>(b[i]);
            ++i;
        }
    }
    std::wstring out;
    for (const wchar_t c : w) {
        if (c == L'\n') out += L" / ";
        else if (c == L'\r' || c == L'|') continue;
        else out += c < 0x20 ? L' ' : c;
    }
    return Utf8(out);
}

// StreamingRef field -> loaded resource (0x141DDE990: loaded when (int8)(flags >> 52) < 0).
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

bool HasComponentNamed(game::Entity* e, const char* prefix) {
    const Arr a = ReadArr(reinterpret_cast<std::uintptr_t>(e) + kResComponents, 512);
    for (std::int32_t i = 0; i < a.n; ++i)
        if (std::strncmp(TypeOf(Q(a.data + 8 * static_cast<std::uintptr_t>(i))), prefix, std::strlen(prefix)) == 0) return true;
    return false;
}

// "Resource P: Type Location=".." Name=".." AIEntityType=N; K components: ..."
std::string DescribeResource(std::uintptr_t res) {
    if (!res) return "(none)";
    char buf[512];
    const char* t = TypeOf(res);
    std::string extra;
    if (mem::DerivesFrom(mem::RttiOf(res), "DSControlledEntityResource")) {
        std::uint8_t ai = 0;
        Rd(res + kResAiType, ai);
        char b2[256];
        std::snprintf(b2, sizeof(b2), " Name=\"%s\" AIEntityType=%u%s", Str(Q(res + kResName)).c_str(), ai,
                      ai == 1 ? " (Humanoid)" : "");
        extra = b2;
    }
    int nc = 0;
    const std::string comps = ListTypes(res + kResComponents, &nc);
    std::snprintf(buf, sizeof(buf), "%p %s Location=\"%s\"%s; %d component resources: ", reinterpret_cast<void*>(res), t,
                  Str(Q(res + kResLocation), 200).c_str(), extra.c_str(), nc);
    return buf + comps;
}

struct SkelInfo {
    std::uintptr_t skel = 0;
    std::int32_t joints = -1;
    std::uint32_t layout = 0;
};
SkelInfo SkeletonInfo(std::uintptr_t skel) {
    SkelInfo s;
    s.skel = skel;
    if (!skel) return s;
    Rd(skel + kSkelJoints, s.joints);
    Rd(skel + kSkelLayoutHash, s.layout);
    return s;
}

// [[e+0x180]+0x30]+0x40 (ArtPartsDataResource.Skeleton), type-checked.
SkelInfo ArtSkeleton(std::uintptr_t e) {
    const std::uintptr_t comp = Q(e + kEntArtParts);
    const std::uintptr_t res = comp ? Q(comp + kCompResource) : 0;
    if (!IsType(res, "ArtPartsDataResource")) return {};
    return SkeletonInfo(Q(res + kArtSkeleton));
}

bool LocalPlayer(game::Entity** e, game::WorldTransform* wt) {
    *e = nullptr;
    if (!game::TryGetLocalPlayer(e) || !*e) return false;
    return game::TryGetTransform(*e, wt);
}

// ---------------------------------------------------------------- census
bool g_census_done = false;

std::uintptr_t World() {
    static int ok = -1;
    if (ok < 0) {
        const std::uintptr_t b = mem::Base();
        ok = mem::CodeMatches(b + kWorldAddLock, "48 8D 9E 30 04 00 00 48 8B CB") &&
             mem::CodeMatches(b + kWorldAddArray, "48 8D 8E 38 04 00 00 48 8B D7 E8");
        if (!ok) Log("[catalog] World::AddEntity bytes differ; the entity census is unavailable");
    }
    if (!ok) return 0;
    const std::uintptr_t w = Q(mem::Base() + kWorldGlobal);
    return w && Q(w) == mem::Base() + kWorldVtable ? w : 0;
}

bool InWorldList(std::uintptr_t e) {
    const std::uintptr_t w = World();
    if (!w) return false;
    const Arr a = ReadArr(w + kWorldEntities, 200000);
    std::vector<std::uintptr_t> v(static_cast<std::size_t>(a.n));
    if (a.n && !mem::Read(a.data, v.data(), v.size() * sizeof(std::uintptr_t))) return false;
    return std::find(v.begin(), v.end(), e) != v.end();
}

void Census() {
    const std::uintptr_t b = mem::Base();
    // Subspace config: [[0x147BE79B0]+0] (loader 0x1426A27AB).
    if (mem::CodeMatches(b + kSubspaceLoad, "48 8B 3D ?? ?? ?? ??") && mem::RipTarget(b + kSubspaceLoad, 3, 7) == b + kSubspaceGlobal) {
        const std::uintptr_t holder = Q(b + kSubspaceGlobal);
        const std::uintptr_t cfg = holder ? Q(holder) : 0;
        if (!IsType(cfg, "DSPlayerSubspaceConfig")) {
            Log("[catalog] census: subspace config not loaded (holder %p, object %p %s)", reinterpret_cast<void*>(holder),
                reinterpret_cast<void*>(cfg), cfg ? TypeOf(cfg) : "");
        } else {
            const struct {
                std::uintptr_t off;
                const char* name;
            } f[] = {{0x28, "SubspacePlayerResource"}, {0x30, "SubspaceShelfResource"}, {0x38, "_0"}};
            for (const auto& x : f)
                Log("[catalog] census: subspace %s = %s", x.name, DescribeResource(Q(cfg + x.off)).c_str());
        }
    } else {
        Log("[catalog] census: subspace loader bytes differ at ds.exe+0x%llX",
            static_cast<unsigned long long>(kSubspaceLoad));
    }
    // ControlledEntities in the world (World::AddEntity 0x14357DFB0 keeps them at +0x438).
    game::Entity* sam = nullptr;
    game::WorldTransform swt{};
    const bool have_sam = LocalPlayer(&sam, &swt);
    const std::uintptr_t w = World();
    if (!w) {
        Log("[catalog] census: world not available");
        return;
    }
    const Arr a = ReadArr(w + kWorldEntities, 200000);
    std::vector<std::uintptr_t> v(static_cast<std::size_t>(a.n));
    if (a.n && !mem::Read(a.data, v.data(), v.size() * sizeof(std::uintptr_t))) {
        Log("[catalog] census: world entity array unreadable (%d)", a.n);
        return;
    }
    std::map<std::string, int> by_type;
    int nearby = 0, logged = 0;
    for (const std::uintptr_t e : v) {
        const char* t = TypeOf(e);
        ++by_type[t];
        double pos[3] = {};
        if (!have_sam || !mem::Read(e + kEntPos, pos, sizeof(pos))) continue;
        const double dx = pos[0] - swt.pos.x, dy = pos[1] - swt.pos.y, dz = pos[2] - swt.pos.z;
        const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > 200.0) continue;
        ++nearby;
        if (logged >= 120) continue;
        ++logged;
        auto* ent = reinterpret_cast<game::Entity*>(e);
        std::uint64_t flags = 0;
        Rd(e + kEntFlags, flags);
        const std::uintptr_t res = RefResource(e + kEntResourceRef);
        std::string name, loc;
        std::uint8_t ai = 0;
        if (res) {
            loc = Str(Q(res + kResLocation), 120);
            if (mem::DerivesFrom(mem::RttiOf(res), "DSControlledEntityResource")) {
                name = Str(Q(res + kResName), 60);
                Rd(res + kResAiType, ai);
            }
        }
        Log("[catalog] census: %6.1f m %p %s flags=0x%llX%s res=%s name=\"%s\" ai=%u%s%s%s%s loc=\"%s\"", dist,
            reinterpret_cast<void*>(e), t, static_cast<unsigned long long>(flags), e == reinterpret_cast<std::uintptr_t>(sam) ? " (SAM)" : "",
            res ? TypeOf(res) : "-", name.c_str(), ai, partner::FindComponentByName(ent, "AnimationManager") ? " +AnimationManager" : "",
            partner::FindComponentByName(ent, "DSGroundMovementComponent") ? " +GroundMovement" : "",
            partner::FindComponentByName(ent, "DSSimpleAnimationComponent") ? " +SimpleAnimation" : "",
            partner::FindComponentByName(ent, "DSWelcomeHoloComponent") ? " +WelcomeHolo" : "", loc.c_str());
        // Story-character holograms placed by cinematics (Fragile, Die-Hardman, ...): their full
        // makeup, as candidates for a later synchronous CreateEntity from the loaded resource.
        if (loc.find("holo") != std::string::npos || name.find("holo") != std::string::npos) {
            partner::DescribeEntity(ent, "census holo");
            const SkelInfo own = ArtSkeleton(e), samsk = have_sam ? ArtSkeleton(reinterpret_cast<std::uintptr_t>(sam)) : SkelInfo{};
            Log("[catalog] census:   resource %s", DescribeResource(res).c_str());
            Log("[catalog] census:   skeleton %p (%d joints, layout %08X) vs Sam's %p: %s", reinterpret_cast<void*>(own.skel), own.joints,
                own.layout, reinterpret_cast<void*>(samsk.skel),
                own.skel && own.skel == samsk.skel ? "SAME" : (own.layout && own.layout == samsk.layout ? "same-layout" : "different"));
        }
    }
    std::string types;
    for (const auto& kv : by_type) types += (types.empty() ? "" : ", ") + kv.first + "=" + std::to_string(kv.second);
    Log("[catalog] census: world %p holds %d ControlledEntities (%d within 200 m of Sam, %d logged): %s",
        reinterpret_cast<void*>(w), a.n, nearby, logged, types.c_str());
}

// ---------------------------------------------------------------- per item
struct Entry {
    std::uint32_t hash = 0, display_no = 0;
    std::uint8_t type = 0;
};

std::vector<Entry> SortedList(const std::vector<holo::Item>& items) {
    std::vector<Entry> out;
    for (const holo::Item& it : items)
        if (it.type == kTypeHolo || it.type == kTypeSpecialHolo) out.push_back({it.hash, it.display_no, it.type});
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        if (a.type != b.type) return a.type < b.type;
        if (a.display_no != b.display_no) return a.display_no < b.display_no;
        return a.hash < b.hash;
    });
    return out;
}

std::uintptr_t g_logged_list_db = 0;

void LogList(std::uintptr_t db, const std::vector<Entry>& list) {
    if (db == g_logged_list_db) return;
    g_logged_list_db = db;
    std::size_t t4 = 0;
    for (const Entry& e : list) t4 += e.type == kTypeHolo;
    Log("[catalog] list: %zu items (%zu Type 4 Reward_Holo, %zu Type 20 Reward_Special_Holo); index HashId tType #DisplayNo",
        list.size(), t4, list.size() - t4);
    std::string line;
    for (std::size_t i = 0; i < list.size(); ++i) {
        char cell[48];
        std::snprintf(cell, sizeof(cell), "%zu %08X t%u #%u", i, list[i].hash, list[i].type, list[i].display_no);
        line += line.empty() ? "[catalog]   " : " | ";
        line += cell;
        if (i % 8 == 7) {
            Log("%s", line.c_str());
            line.clear();
        }
    }
    if (!line.empty()) Log("%s", line.c_str());
}

void LogItem(int idx, const holo::Item& it, std::string* name_out) {
    std::uint8_t member = 0xFF;
    const std::uintptr_t hh = Q(it.ptr + kItemHousehold);
    if (IsType(hh, "DSHouseholdInfoResource")) Rd(hh + kHouseholdMember, member);
    const std::string name = LocText(Q(it.ptr + kItemNameText), 80);
    const std::string text = LocText(Q(it.ptr + kItemText), 160);
    *name_out = name;
    Log("[catalog] #%d %08X t%u #%u hidden=%d member=%d(%s) sort=%d name=\"%s\" text=\"%s\"", idx, it.hash, it.type,
        it.display_no, it.hidden ? 1 : 0, member == 0xFF ? -1 : member,
        member < sizeof(kMembers) / sizeof(kMembers[0]) ? kMembers[member] : "-", it.sort, name.c_str(), text.c_str());
}

struct ClipSummary {
    int clips = 0, looping = 0, moving = 0, moving_loop = 0;
    std::string moving_list;  // "i:speed"
};

ClipSummary LogClips(game::Entity* e, const SkelInfo& own, const SkelInfo& sam) {
    ClipSummary sum;
    const std::uintptr_t comp = partner::FindComponentByName(e, "DSSimpleAnimationComponent");
    if (!comp) {
        Log("[catalog]   no DSSimpleAnimationComponent");
        return sum;
    }
    const std::uintptr_t res = Q(comp + kCompResource);
    if (!IsType(res, "DSSimpleAnimationComponentResource")) {
        Log("[catalog]   DSSimpleAnimationComponent %p resource %p is %s", reinterpret_cast<void*>(comp),
            reinterpret_cast<void*>(res), TypeOf(res));
        return sum;
    }
    const Arr clips = ReadArr(res + kSarClips, 256);
    const Arr interp = ReadArr(res + kSarInterp, 256);
    std::uint8_t loop_default = 0, randomize = 0;
    float randomize_sec = 0;
    Rd(res + kSarLoopDefault, loop_default);
    Rd(res + kSarLoopDefault + 1, randomize);
    Rd(res + kSarLoopDefault + 4, randomize_sec);
    Log("[catalog]   DSSimpleAnimation %p resource %p: %d clips, %d interp values, LoopDefaultAnimation=%u "
        "RandomizDefaultAnimation=%u (%.1f s)",
        reinterpret_cast<void*>(comp), reinterpret_cast<void*>(res), clips.n, interp.n, loop_default, randomize,
        static_cast<double>(randomize_sec));
    sum.clips = clips.n;
    for (std::int32_t i = 0; i < clips.n; ++i) {
        const std::uintptr_t c = Q(clips.data + 8 * static_cast<std::uintptr_t>(i));
        if (!IsType(c, "SkeletonAnimationResource")) {
            Log("[catalog]   clip %d: %p %s", i, reinterpret_cast<void*>(c), TypeOf(c));
            continue;
        }
        std::uint8_t r[0xA0] = {};
        if (!mem::Read(c, r, sizeof(r))) {
            Log("[catalog]   clip %d: %p unreadable", i, reinterpret_cast<void*>(c));
            continue;
        }
        float rate = 0, dur = 0, q[4] = {}, t[3] = {};
        std::uintptr_t skel = 0, loc = 0;
        std::memcpy(&loc, r + kClipLocation, 8);
        std::memcpy(&skel, r + kClipSkeleton, 8);
        std::memcpy(&rate, r + kClipSampleRate, 4);
        std::memcpy(&dur, r + kClipDuration, 4);
        std::memcpy(q, r + kClipDeltaRot, 16);
        std::memcpy(t, r + kClipDeltaPos, 12);
        const std::uint8_t loop = r[kClipLoop];
        const double dist = std::sqrt(double(t[0]) * t[0] + double(t[1]) * t[1] + double(t[2]) * t[2]);
        const double horiz = std::sqrt(double(t[0]) * t[0] + double(t[1]) * t[1]);
        const double w = std::fabs(double(q[3])) > 1.0 ? 1.0 : std::fabs(double(q[3]));
        const double turn = 2.0 * std::acos(w) * 180.0 / 3.14159265358979;
        const double speed = dur > 0.001f ? dist / dur : 0.0;
        float isec = -1.0f;
        if (i < interp.n) Rd(interp.data + 4 * static_cast<std::uintptr_t>(i), isec);
        static const char* const kLoop[] = {"Off", "On", "Hold", "PingPong"};
        const SkelInfo cs = SkeletonInfo(skel);
        const char* skel_note = skel == own.skel ? "own" : (sam.skel && skel == sam.skel ? "SAM'S" : (cs.layout && cs.layout == sam.layout ? "sam-layout" : "other"));
        Log("[catalog]   clip %d: dur=%.2f s rate=%.0f loop=%s dT=(%.2f, %.2f, %.2f) |dT|=%.2f m (horiz %.2f) speed=%.2f m/s "
            "turn=%.0f deg interp=%.2f skel=%p (%s, %d joints, layout %08X) loc=\"%s\"",
            i, static_cast<double>(dur), static_cast<double>(rate), loop < 4 ? kLoop[loop] : "?", static_cast<double>(t[0]),
            static_cast<double>(t[1]), static_cast<double>(t[2]), dist, horiz, speed, turn, static_cast<double>(isec),
            reinterpret_cast<void*>(skel), skel_note, cs.joints, cs.layout, Str(loc, 200).c_str());
        if (loop == 1) ++sum.looping;
        if (horiz > 0.05) {
            ++sum.moving;
            if (loop == 1) ++sum.moving_loop;
            char m[32];
            std::snprintf(m, sizeof(m), "%s%d:%.2f%s", sum.moving_list.empty() ? "" : ",", i, speed, loop == 1 ? "L" : "");
            sum.moving_list += m;
        }
    }
    return sum;
}

void LogAnimState(game::Entity* e, const char* when) {
    const std::uintptr_t comp = partner::FindComponentByName(e, "DSSimpleAnimationComponent");
    if (!comp) return;
    std::int32_t cur = -1, state = -1, mode = -1;
    float time = 0, rate = 0, timer = 0;
    std::uintptr_t clip = 0;
    Rd(comp + kSaCurrent, cur);
    Rd(comp + kSaGestureTimer, timer);
    const std::uintptr_t p = Q(comp + kSaPlayer);
    if (p) {
        clip = Q(p);
        Rd(p + 8, state);
        Rd(p + 0xC, time);
        Rd(p + 0x10, rate);
        Rd(p + 0x14, mode);
    }
    Log("[catalog]   anim %s: current=%d player=%p clip=%p state=%d time=%.2f rate=%.2f mode=%d gesture timer=%.2f", when, cur,
        reinterpret_cast<void*>(p), reinterpret_cast<void*>(clip), state, static_cast<double>(time), static_cast<double>(rate),
        mode, static_cast<double>(timer));
}

void LogShaderVars(game::Entity* e) {
    std::string vars;
    if (const std::uintptr_t sa = partner::FindComponentByName(e, "ShaderAnimComponent")) {
        const std::uintptr_t res = Q(sa + kCompResource);
        if (IsType(res, "ShaderAnimComponentResource")) {
            const char* const set[] = {"start", "loop", "end"};
            for (int k = 0; k < 3; ++k) {
                const Arr a = ReadArr(res + 0x20 + 0x10 * static_cast<std::uintptr_t>(k), 64);
                for (std::int32_t i = 0; i < a.n; ++i) {
                    const std::uintptr_t cs = Q(a.data + 8 * static_cast<std::uintptr_t>(i));
                    if (!IsType(cs, "ShaderAnimCurveSet")) continue;
                    vars += std::string(vars.empty() ? "" : ", ") + set[k] + ":" + Str(Q(cs + 0x20), 48) + "@" + Str(Q(cs + 0x28), 48);
                }
            }
        }
    }
    std::string opacity;
    if (const std::uintptr_t po = partner::FindComponentByName(e, "DSPhotoModeModelOpacityComponent")) {
        const std::uintptr_t res = Q(po + kCompResource);
        if (IsType(res, "DSPhotoModeModelOpacityComponentResource")) opacity = Str(Q(res + 0x20), 64);
    }
    if (!vars.empty() || !opacity.empty())
        Log("[catalog]   shader: ShaderAnim vars [%s]; PhotoModeOpacity ShaderVariableName=\"%s\"", vars.c_str(), opacity.c_str());
}

// Checks every body must pass (welcome holograms additionally pass holo::ValidateWelcomeHolo).
const char* BaseChecks(game::Entity* ev) {
    const auto e = reinterpret_cast<std::uintptr_t>(ev);
    const std::uintptr_t vt = Q(e);
    if (!mem::InImage(vt)) return "entity vtable not in ds.exe";
    const std::uintptr_t rtti = mem::RttiOf(e);
    if (!rtti) return "entity type unknown";
    if (mem::DerivesFrom(rtti, "Humanoid") || mem::DerivesFrom(rtti, "DSPlayerEntity")) return "a Humanoid/player entity";
    if (Q(vt + 0xA8) == mem::Base() + kPlayerWakeOverride) return "uses the player's Wake override";
    if (!Q(e + 0xB8)) return "no model (entity +0xB8 null)";
    if (HasComponentNamed(ev, "DSPlayer")) return "has a DSPlayer* component";
    return nullptr;
}

// Image -> half size (2x2 box filter): ~2.7 MB PNG instead of ~11 MB at 1440p.
overlay::Image Half(const overlay::Image& in) {
    overlay::Image out;
    out.width = in.width / 2;
    out.height = in.height / 2;
    out.rgb.resize(static_cast<std::size_t>(out.width) * out.height * 3);
    for (unsigned y = 0; y < out.height; ++y) {
        const std::uint8_t* r0 = in.rgb.data() + static_cast<std::size_t>(2 * y) * in.width * 3;
        const std::uint8_t* r1 = r0 + static_cast<std::size_t>(in.width) * 3;
        std::uint8_t* o = out.rgb.data() + static_cast<std::size_t>(y) * out.width * 3;
        for (unsigned x = 0; x < out.width; ++x)
            for (unsigned c = 0; c < 3; ++c) {
                const std::size_t a = static_cast<std::size_t>(2 * x) * 3 + c;
                o[x * 3 + c] = static_cast<std::uint8_t>((r0[a] + r0[a + 3] + r1[a] + r1[a + 3] + 2) / 4);
            }
    }
    return out;
}

// ---------------------------------------------------------------- batch state
enum class St : std::uint8_t { idle, spawn, loading, settle, capture, gap };
St g_st = St::idle;
int g_next = 0, g_end = 0, g_first = 0;
double g_until = 0, g_started = 0;
double g_distance = kDefaultDistanceM;
holo::Spawn g_spawn;
game::Entity* g_sam = nullptr;
game::WorldTransform g_at{};
std::uint32_t g_capture = 0;
std::uintptr_t g_removed = 0;
int g_cur = -1;
holo::Item g_item;
std::string g_name, g_result;
bool g_welcome = false;
ClipSummary g_clips;
std::string g_skel_note, g_etype;
int g_ok = 0, g_bad = 0, g_leaks = 0;
std::vector<std::string> g_summary;
double g_now = 0;

void Row(const char* result) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "#%d|%08X|t%u|#%u|%s|%s|welcome=%d|clips=%d|loop=%d|moving=%d|movingloop=%d|%s|skel=%s|%s", g_cur,
                  g_item.hash, g_item.type, g_item.display_no, g_name.c_str(), g_etype.c_str(), g_welcome ? 1 : 0, g_clips.clips,
                  g_clips.looping, g_clips.moving, g_clips.moving_loop, g_clips.moving_list.c_str(), g_skel_note.c_str(), result);
    Log("[catalog] row %s", buf);
    g_summary.push_back(buf);
}

void Finish(const char* why) {
    // "batch <first>..<last> done": scenarios wait for exactly this batch's line.
    Log("[catalog] batch %d..%d done (%s): %d ok, %d not shown, %d spawner leaks", g_first, g_end - 1, why, g_ok, g_bad, g_leaks);
    g_st = St::idle;
}

void EndItem(const char* result, bool ok) {
    char step[200] = {};
    if (g_spawn.sp || g_spawn.realized) {
        const auto removed = reinterpret_cast<std::uintptr_t>(g_spawn.e);
        const bool released = holo::SpawnTeardown(g_spawn, "catalog", step, sizeof(step));
        if (!released) ++g_leaks;
        g_removed = removed;
        Log("[catalog] #%d %08X teardown: %s", g_cur, g_item.hash, step);
    }
    Row(result);
    ok ? ++g_ok : ++g_bad;
    g_cur = -1;
    g_st = St::gap;
    g_until = g_now + kGapMs;
}

void Abort(const char* why) {
    if (g_st == St::capture && g_capture) overlay::CancelCapture(g_capture);
    if (g_cur >= 0) EndItem(why, false);
    Finish(why);
}

bool SamOk() {
    game::Entity* e = nullptr;
    game::WorldTransform wt{};
    if (!LocalPlayer(&e, &wt) || e != g_sam) return false;
    std::uint64_t f = 0;
    partner::ReadFlags(e, &f);
    return (f & game::kEntityFlagSleeping) == 0;
}

void SpawnNext() {
    if (g_next >= g_end) {
        Finish("end of range");
        return;
    }
    std::uintptr_t db = 0;
    std::vector<holo::Item> items;
    if (!holo::ReadItems(&db, &items)) {
        Finish("collectors DB not readable");
        return;
    }
    const std::vector<Entry> list = SortedList(items);
    LogList(db, list);
    if (g_next >= static_cast<int>(list.size())) {
        Finish("end of list");
        return;
    }
    g_cur = g_next++;
    const Entry want = list[static_cast<std::size_t>(g_cur)];
    g_item = holo::Item{};
    for (const holo::Item& it : items)
        if (it.hash == want.hash && it.type == want.type) g_item = it;
    g_name.clear();
    g_etype.clear();
    g_skel_note.clear();
    g_clips = ClipSummary{};
    g_welcome = false;
    if (!g_item.ptr) {
        EndItem("item vanished from the DB", false);
        return;
    }
    LogItem(g_cur, g_item, &g_name);
    if (g_item.empty) {
        EndItem("no display entity", false);
        return;
    }
    // Stand it in front of Sam, facing him (Sam's rotation turned 180 degrees about up).
    game::Entity* sam = nullptr;
    game::WorldTransform swt{};
    if (!LocalPlayer(&sam, &swt) || sam != g_sam) {
        Abort("local player changed");
        return;
    }
    const float* fw = swt.rot.m[kForwardRow];
    const float* rt = swt.rot.m[kForwardRow == 1 ? 0 : 1];
    double fx = fw[0], fy = fw[1];
    double fl = std::sqrt(fx * fx + fy * fy);
    if (fl < 1e-3) fx = 1, fy = 0, fl = 1;
    fx /= fl, fy /= fl;
    double rx = rt[0], ry = rt[1];
    const double rl = std::sqrt(rx * rx + ry * ry);
    if (rl > 1e-3) rx /= rl, ry /= rl;
    g_at = swt;
    g_at.pos.x = swt.pos.x + fx * g_distance + rx * kSideM;
    g_at.pos.y = swt.pos.y + fy * g_distance + ry * kSideM;
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c) g_at.rot.m[r][c] = -swt.rot.m[r][c];
    char why[200] = {};
    if (!holo::SpawnBegin(g_item.ptr, g_item.hash, g_at, &g_spawn, why, sizeof(why))) {
        Log("[catalog] #%d %08X spawn refused: %s", g_cur, g_item.hash, why);
        EndItem("spawn refused", false);
        if (!holo::Available()) Finish("hologram functions unavailable");
        return;
    }
    Log("[catalog] #%d %08X spawning at (%.2f, %.2f, %.2f), %.1f m ahead of Sam: %s", g_cur, g_item.hash, g_at.pos.x,
        g_at.pos.y, g_at.pos.z, g_distance, why);
    g_started = g_now;
    g_st = St::loading;
}

void OnRealized() {
    game::Entity* e = g_spawn.e;
    Log("[catalog] #%d %08X loaded after %.2f s; realized entity %p", g_cur, g_item.hash, (g_now - g_started) / 1000.0, e);
    if (!e) {
        EndItem("loaded but realized no entity", false);
        return;
    }
    partner::DescribeEntity(e, "catalog entity");
    const char* et = partner::TypeName(e);
    g_etype = et ? et : "?";
    // Validate first: nothing else may touch an entity that fails.
    const char* bad = BaseChecks(e);
    std::uintptr_t wcomp = 0;
    g_welcome = partner::FindComponentByName(e, "DSWelcomeHoloComponent") != 0;
    if (!bad && g_welcome) bad = holo::ValidateWelcomeHolo(e, &wcomp);
    if (bad) {
        Log("[catalog] #%d %08X refused: %s", g_cur, g_item.hash, bad);
        EndItem(bad, false);
        return;
    }
    const auto ea = reinterpret_cast<std::uintptr_t>(e);
    Log("[catalog]   entity resource %s", DescribeResource(RefResource(ea + kEntResourceRef)).c_str());
    if (g_item.loaded && g_item.res != RefResource(ea + kEntResourceRef))
        Log("[catalog]   item display resource %s", DescribeResource(g_item.res).c_str());
    Log("[catalog]   has: AnimationManager=%d DSGroundMovement=%d DSWelcomeHolo=%d ShaderAnim=%d DSCoverAndAnimModel=%d "
        "DSFacialRig=%d DSAIIndividual=%d",
        partner::FindComponentByName(e, "AnimationManager") ? 1 : 0,
        partner::FindComponentByName(e, "DSGroundMovementComponent") ? 1 : 0, g_welcome ? 1 : 0,
        partner::FindComponentByName(e, "ShaderAnimComponent") ? 1 : 0, HasComponentNamed(e, "DSCoverAndAnimModel") ? 1 : 0,
        HasComponentNamed(e, "DSFacialRig") ? 1 : 0, HasComponentNamed(e, "DSAIIndividual") ? 1 : 0);
    const SkelInfo own = ArtSkeleton(ea);
    SkelInfo sam;
    if (g_sam) sam = ArtSkeleton(reinterpret_cast<std::uintptr_t>(g_sam));
    const bool same = own.skel && own.skel == sam.skel;
    const bool same_layout = own.layout && own.layout == sam.layout;
    char sk[96];
    std::snprintf(sk, sizeof(sk), "%s", same ? "SAME" : (same_layout ? "same-layout" : (own.skel ? "different" : "none")));
    g_skel_note = sk;
    Log("[catalog]   skeleton: own %p (%d joints, layout %08X), Sam's %p (%d joints, layout %08X): %s",
        reinterpret_cast<void*>(own.skel), own.joints, own.layout, reinterpret_cast<void*>(sam.skel), sam.joints, sam.layout,
        sk);
    g_clips = LogClips(e, own, sam);
    LogShaderVars(e);
    if (g_welcome) {
        holo::LogWelcomeHolo(e, "catalog on realize");
        char why[200] = {};
        const bool shown = holo::SpawnStartVisible(e, why, sizeof(why));
        Log("[catalog]   %s", why);
        if (!shown && !holo::Available()) {
            EndItem("StartVisible faulted", false);
            Finish("hologram functions unavailable");
            return;
        }
    }
    LogAnimState(e, "at realize");
    g_st = St::settle;
    g_until = g_now + kSettleMs;
}

void Capture() {
    game::Entity* e = g_spawn.e;
    std::uint64_t f = 0;
    partner::ReadFlags(e, &f);
    double pos[3] = {};
    mem::Read(reinterpret_cast<std::uintptr_t>(e) + kEntPos, pos, sizeof(pos));
    const double dx = pos[0] - g_at.pos.x, dy = pos[1] - g_at.pos.y, dz = pos[2] - g_at.pos.z;
    Log("[catalog] #%d %08X after %.1f s: flags=0x%llX visible=%d at (%.2f, %.2f, %.2f), %.2f m from where it was placed (dz %.2f)",
        g_cur, g_item.hash, kSettleMs / 1000.0, static_cast<unsigned long long>(f), (f & 0x2) ? 1 : 0, pos[0], pos[1], pos[2],
        std::sqrt(dx * dx + dy * dy + dz * dz), dz);
    LogAnimState(e, "after settle");
    if (g_welcome) holo::LogWelcomeHolo(e, "catalog after settle");
    if (!overlay::Installed() || automation::OutDir().empty()) {
        EndItem("ok (no screenshot: overlay or automation off)", true);
        return;
    }
    g_capture = overlay::RequestCapture();
    if (!g_capture) {
        EndItem("ok (screenshot refused: too many pending)", true);
        return;
    }
    g_st = St::capture;
    g_until = g_now + kCaptureTimeoutMs;
}

void PollCapture() {
    overlay::Image img;
    char why[200] = {};
    const int r = overlay::TakeCapture(g_capture, &img, why, sizeof(why));
    if (r == 0 && g_now < g_until) return;
    if (r == 0) {
        overlay::CancelCapture(g_capture);
        EndItem("ok (screenshot timed out)", true);
        return;
    }
    if (r < 0) {
        Log("[catalog] #%d %08X screenshot failed: %s", g_cur, g_item.hash, why);
        EndItem("ok (screenshot failed)", true);
        return;
    }
    wchar_t name[64];
    std::swprintf(name, 64, L"\\holo-%08X.png", g_item.hash);
    const overlay::Image half = img.width >= 1600 ? Half(img) : img;
    const bool wrote = overlay::WritePngFile(automation::OutDir() + name, half);
    Log("[catalog] #%d %08X screenshot holo-%08X.png %ux%u %s", g_cur, g_item.hash, g_item.hash, half.width, half.height,
        wrote ? "written" : "NOT written");
    EndItem(wrote ? "ok" : "ok (png not written)", true);
}

}  // namespace

bool Start(int start, int count, double distance_m, char* msg, std::size_t cap) noexcept {
    auto say = [&](const char* fmt, auto... args) {
        if (msg && cap) std::snprintf(msg, cap, fmt, args...);
    };
    if (g_st != St::idle) {
        say("a catalog batch is running (item %d of %d..%d)", g_cur, g_next, g_end);
        return false;
    }
    if (start < 0 || count <= 0 || count > 512) {
        say("bad range %d +%d", start, count);
        return false;
    }
    if (!holo::Available()) {
        say("hologram functions unavailable: %s", holo::UnavailableReason());
        return false;
    }
    game::Entity* sam = nullptr;
    game::WorldTransform swt{};
    if (!LocalPlayer(&sam, &swt)) {
        say("no local player");
        return false;
    }
    g_sam = sam;
    if (!g_census_done) {
        g_census_done = true;
        Census();
    }
    Log("[catalog] Sam %p at (%.2f, %.2f, %.2f) rot rows (%.2f %.2f %.2f) (%.2f %.2f %.2f) (%.2f %.2f %.2f)",
        reinterpret_cast<void*>(sam), swt.pos.x, swt.pos.y, swt.pos.z, static_cast<double>(swt.rot.m[0][0]),
        static_cast<double>(swt.rot.m[0][1]), static_cast<double>(swt.rot.m[0][2]), static_cast<double>(swt.rot.m[1][0]),
        static_cast<double>(swt.rot.m[1][1]), static_cast<double>(swt.rot.m[1][2]), static_cast<double>(swt.rot.m[2][0]),
        static_cast<double>(swt.rot.m[2][1]), static_cast<double>(swt.rot.m[2][2]));
    if (holo::GetPhase() != holo::Phase::idle)
        Log("[catalog] note: the partner hologram is %s too (separate spawner)", holo::PhaseName());
    g_next = start;
    g_first = start;
    g_end = start + count;
    g_distance = distance_m > 0.5 && distance_m < 30 ? distance_m : kDefaultDistanceM;
    g_ok = g_bad = g_leaks = 0;
    g_summary.clear();
    g_cur = -1;
    g_removed = 0;
    g_st = St::spawn;
    Log("[catalog] batch: items %d..%d, %.1f m in front of Sam", start, start + count - 1, g_distance);
    say("catalog items %d..%d started; the log says when the batch is finished", start, start + count - 1);
    return true;
}

bool Running() noexcept { return g_st != St::idle; }

// ---------------------------------------------------------------- clip survey
namespace {

// SkeletonAnimationResource (RTTI 0x144A621B0): GetRTTI getter 0x141D84EE0 (lea rax,[rtti]; ret) sits in
// slot 0 of the vtables 0x143C10818 and 0x143C10A28 (dsre data refs). The scan accepts an address only
// when RttiOf() decodes it to that RTTI and its Skeleton is a Skeleton.
constexpr std::uintptr_t kClipRtti = 0x4A621B0;
constexpr std::uintptr_t kClipVtables[] = {0x3C10818, 0x3C10A28};
constexpr std::uintptr_t kDsPlayerSystem = 0x7BE7980;        // DSPlayerSystem* (game_actions checks its loads)
constexpr std::uintptr_t kPsExternalAnimation = 0x207C8;     // DSExternalAnimationResource* (research section 2)
constexpr std::uintptr_t kPlayerPoseAnimations = 0xA18;      // DSPlayerEntityResource: Array<PlayerPhotoModePoseAnimation> (0x28 each, +8 Animation)
constexpr std::uintptr_t kPlayerPoseAnimationsRoom = 0xA28;  // ..._PrivateRoom

struct SurveyArgs {
    std::uint32_t sam_layout = 0;
    std::uintptr_t sam_skel = 0;
};
volatile LONG g_survey_running = 0;

struct ClipRow {
    std::uintptr_t clip, skel;
    std::uint32_t layout;
    std::int32_t joints;
    float dur, dist;
    std::string loc;
};

bool ReadClip(std::uintptr_t c, ClipRow* out) {
    std::uint8_t r[0x80] = {};
    if (!mem::Read(c, r, sizeof(r))) return false;
    std::uintptr_t loc = 0, skel = 0;
    std::memcpy(&loc, r + kClipLocation, 8);
    std::memcpy(&skel, r + kClipSkeleton, 8);
    if (!skel || !IsType(skel, "Skeleton")) return false;
    float t[3] = {};
    std::memcpy(&out->dur, r + kClipDuration, 4);
    std::memcpy(t, r + kClipDeltaPos, 12);
    out->clip = c;
    out->skel = skel;
    out->dist = static_cast<float>(std::sqrt(double(t[0]) * t[0] + double(t[1]) * t[1]));
    const SkelInfo si = SkeletonInfo(skel);
    out->layout = si.layout;
    out->joints = si.joints;
    out->loc = Str(loc, 200);
    return true;
}

bool Interesting(const std::string& loc) {
    static const char* const kWords[] = {"walk", "run", "jog", "move", "step", "sprint", "dash", "_mv_", "loco"};
    for (const char* w : kWords)
        if (loc.find(w) != std::string::npos) return true;
    return false;
}

DWORD WINAPI SurveyThread(void* p) {
    const SurveyArgs a = *static_cast<SurveyArgs*>(p);
    delete static_cast<SurveyArgs*>(p);
    const std::uintptr_t b = mem::Base();
    const std::uintptr_t rtti = b + kClipRtti;
    const double t0 = static_cast<double>(GetTickCount64());
    const std::size_t kChunk = 4u << 20;
    auto* buf = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!buf) {
        Log("[survey] no scan buffer");
        InterlockedExchange(&g_survey_running, 0);
        return 0;
    }
    const auto buf_lo = reinterpret_cast<std::uintptr_t>(buf);
    std::vector<std::uintptr_t> cands;
    std::uint64_t scanned = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    for (std::uintptr_t addr = 0x10000; VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi);) {
        const auto lo = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::uintptr_t hi = lo + mbi.RegionSize;
        if (hi <= addr) break;
        addr = hi;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) || mbi.Type == MEM_IMAGE || lo == buf_lo)
            continue;
        for (std::uintptr_t at = lo; at < hi; at += kChunk) {
            const std::size_t n = static_cast<std::size_t>(hi - at < kChunk ? hi - at : kChunk);
            if (!mem::Read(at, buf, n)) continue;
            scanned += n;
            const auto* q = reinterpret_cast<const std::uint64_t*>(buf);
            for (std::size_t i = 0; i < n / 8; ++i)
                if (q[i] == b + kClipVtables[0] || q[i] == b + kClipVtables[1]) cands.push_back(at + i * 8);
        }
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    std::size_t valid = 0, sam_layout = 0, moving = 0, sam_moving = 0, named = 0;
    std::map<std::uint32_t, int> layouts;
    for (const std::uintptr_t c : cands) {
        if (mem::RttiOf(c) != rtti) continue;
        ClipRow r;
        if (!ReadClip(c, &r)) continue;
        ++valid;
        ++layouts[r.layout];
        const bool on_sam = r.layout == a.sam_layout;
        sam_layout += on_sam;
        if (r.dist > 0.1f) ++moving;
        const bool hit = (r.dist > 0.1f || Interesting(r.loc));
        if (on_sam && r.dist > 0.1f) ++sam_moving;
        if (hit && named < 400) {
            ++named;
            Log("[survey] %s clip %p dur=%.2f |dT|=%.2f m speed=%.2f m/s skel=%p (%d joints, layout %08X%s) loc=\"%s\"",
                on_sam ? "SAM-LAYOUT" : "other", reinterpret_cast<void*>(c), static_cast<double>(r.dur), static_cast<double>(r.dist),
                r.dur > 0.01f ? static_cast<double>(r.dist / r.dur) : 0.0, reinterpret_cast<void*>(r.skel), r.joints, r.layout,
                r.skel == a.sam_skel ? ", Sam's own" : "", r.loc.c_str());
        }
    }
    std::string lay;
    for (const auto& kv : layouts) {
        char cell[32];
        std::snprintf(cell, sizeof(cell), "%s%08X=%d", lay.empty() ? "" : ", ", kv.first, kv.second);
        lay += cell;
    }
    Log("[survey] layouts: %s", lay.c_str());
    Log("[survey] done: scanned %.1f GB in %.1f s; %zu vtable hits, %zu SkeletonAnimationResources (%zu on Sam's layout %08X); "
        "%zu with root motion > 0.1 m (%zu on Sam's layout); %zu lines logged",
        static_cast<double>(scanned) / 1e9, (static_cast<double>(GetTickCount64()) - t0) / 1000.0, cands.size(), valid, sam_layout,
        a.sam_layout, moving, sam_moving, named);
    InterlockedExchange(&g_survey_running, 0);
    return 0;
}

void LogPoseArray(std::uintptr_t at, const char* what, const SkelInfo& sam) {
    const Arr a = ReadArr(at, 512);
    Log("[survey] %s: %d entries", what, a.n);
    for (std::int32_t i = 0; i < a.n && i < 64; ++i) {
        const std::uintptr_t c = Q(a.data + 0x28 * static_cast<std::uintptr_t>(i) + 8);
        ClipRow r;
        if (!IsType(c, "SkeletonAnimationResource") || !ReadClip(c, &r)) {
            Log("[survey]   %d: %p %s", i, reinterpret_cast<void*>(c), TypeOf(c));
            continue;
        }
        Log("[survey]   %d: dur=%.2f |dT|=%.2f layout %08X%s loc=\"%s\"", i, static_cast<double>(r.dur), static_cast<double>(r.dist),
            r.layout, r.layout == sam.layout ? " (Sam's)" : "", r.loc.c_str());
    }
}

}  // namespace

bool StartClipSurvey(char* msg, std::size_t cap) noexcept {
    auto say = [&](const char* text) {
        if (msg && cap) std::snprintf(msg, cap, "%s", text);
    };
    if (InterlockedCompareExchange(&g_survey_running, 1, 0) != 0) {
        say("a clip survey is running");
        return false;
    }
    const std::uintptr_t b = mem::Base();
    // The scan trusts the static vtable addresses only after the runtime decode agrees.
    for (const std::uintptr_t vt : kClipVtables) {
        std::uintptr_t probe = b + vt;
        const std::uintptr_t fake = reinterpret_cast<std::uintptr_t>(&probe);  // an "object" whose vtable is vt
        if (mem::RttiOf(fake) != b + kClipRtti) {
            InterlockedExchange(&g_survey_running, 0);
            say("SkeletonAnimationResource vtable did not verify");
            Log("[survey] vtable ds.exe+0x%llX does not decode to SkeletonAnimationResource; survey off",
                static_cast<unsigned long long>(vt));
            return false;
        }
    }
    game::Entity* sam = nullptr;
    game::WorldTransform swt{};
    SkelInfo sk;
    if (LocalPlayer(&sam, &swt)) sk = ArtSkeleton(reinterpret_cast<std::uintptr_t>(sam));
    // Named sources first (research section 2), on the game thread: reads only.
    if (sam) {
        const std::uintptr_t res = RefResource(reinterpret_cast<std::uintptr_t>(sam) + kEntResourceRef);
        if (IsType(res, "DSPlayerEntityResource")) {
            LogPoseArray(res + kPlayerPoseAnimations, "DSPlayerEntityResource.PlayerPoseAnimations", sk);
            LogPoseArray(res + kPlayerPoseAnimationsRoom, "DSPlayerEntityResource.PlayerPoseAnimations_PrivateRoom", sk);
        } else {
            Log("[survey] Sam's resource %p is %s", reinterpret_cast<void*>(res), TypeOf(res));
        }
    }
    const std::uintptr_t ps = Q(b + kDsPlayerSystem);
    const std::uintptr_t ext = ps ? Q(ps + kPsExternalAnimation) : 0;
    if (IsType(ext, "DSExternalAnimationResource")) {
        const Arr a = ReadArr(ext + 0x20, 1024);
        Log("[survey] DSExternalAnimationResource %p: %d AnimationInfoResources", reinterpret_cast<void*>(ext), a.n);
        for (std::int32_t i = 0; i < a.n && i < 64; ++i) {
            const std::uintptr_t c = Q(a.data + 0x20 * static_cast<std::uintptr_t>(i) + 8);
            ClipRow r;
            if (IsType(c, "SkeletonAnimationResource") && ReadClip(c, &r))
                Log("[survey]   %d: dur=%.2f |dT|=%.2f layout %08X%s loc=\"%s\"", i, static_cast<double>(r.dur),
                    static_cast<double>(r.dist), r.layout, r.layout == sk.layout ? " (Sam's)" : "", r.loc.c_str());
            else
                Log("[survey]   %d: %p %s", i, reinterpret_cast<void*>(c), TypeOf(c));
        }
    } else {
        Log("[survey] [DSPlayerSystem]+0x207C8 is %p %s", reinterpret_cast<void*>(ext), ext ? TypeOf(ext) : "");
    }
    auto* args = new (std::nothrow) SurveyArgs{sk.layout, sk.skel};
    HANDLE h = args ? CreateThread(nullptr, 0, &SurveyThread, args, 0, nullptr) : nullptr;
    if (!h) {
        delete args;
        InterlockedExchange(&g_survey_running, 0);
        say("could not start the survey thread");
        return false;
    }
    CloseHandle(h);
    Log("[survey] scanning committed memory for SkeletonAnimationResources (Sam's skeleton layout %08X)", sk.layout);
    say("clip survey started; the log says when it is done");
    return true;
}

void Tick(double now_ms) noexcept {
    g_now = now_ms;
    if (g_st == St::idle) return;
    if (g_st != St::gap && g_st != St::spawn && !SamOk()) {
        Abort("Sam changed or fell asleep (load, cutscene)");
        return;
    }
    switch (g_st) {
        case St::spawn:
            SpawnNext();
            break;
        case St::loading: {
            char why[200] = {};
            switch (holo::SpawnPoll(g_spawn, why, sizeof(why))) {
                case holo::SpawnState::loading:
                    if (now_ms - g_started > kLoadTimeoutMs) {
                        Log("[catalog] #%d %08X did not load within %.0f s", g_cur, g_item.hash, kLoadTimeoutMs / 1000.0);
                        EndItem("load timeout", false);
                    }
                    break;
                case holo::SpawnState::realized:
                    OnRealized();
                    break;
                case holo::SpawnState::cancelled:
                    Log("[catalog] #%d %08X %s", g_cur, g_item.hash, why);
                    EndItem("cancelled by the game", false);
                    break;
                case holo::SpawnState::failed:
                    Log("[catalog] #%d %08X %s", g_cur, g_item.hash, why);
                    ++g_leaks;
                    EndItem("spawner failed", false);
                    if (!holo::Available()) Finish("hologram functions unavailable");
                    break;
            }
            break;
        }
        case St::settle:
            if (!holo::SpawnAlive(g_spawn)) {
                Log("[catalog] #%d %08X entity %p was destroyed by the game during the settle", g_cur, g_item.hash, g_spawn.e);
                EndItem("destroyed by the game", false);
            } else if (now_ms >= g_until) {
                Capture();
            }
            break;
        case St::capture:
            PollCapture();
            break;
        case St::gap:
            if (now_ms < g_until) break;
            if (g_removed && InWorldList(g_removed))
                Log("[catalog] WARNING: removed entity %p is still in the world list %.1f s later", reinterpret_cast<void*>(g_removed),
                    kGapMs / 1000.0);
            g_removed = 0;
            g_st = St::spawn;
            break;
        case St::idle:
            break;
    }
}

}  // namespace st::catalog
