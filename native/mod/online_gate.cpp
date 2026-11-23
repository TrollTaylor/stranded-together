// The online gate (online_gate.hpp; docs/world-sync.md "The online gate").
#include "online_gate.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "MinHook.h"
#include "automation.hpp"
#include "config.hpp"
#include "log.hpp"
#include "partner_source.hpp"
#include "world_rules.hpp"

namespace st::gate {
namespace {
namespace mem = partner::mem;

constexpr std::uintptr_t kImage = 0x140000000ull;
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

constexpr std::uintptr_t kRegisterNew = 0x143485040;
constexpr std::uintptr_t kDrain = 0x143482F30;
constexpr std::uintptr_t kTelemetry = 0x143484AC0;
constexpr std::uintptr_t kUpload = 0x142CB5BC0;
constexpr std::uintptr_t kOtgcCtor = 0x141B91D30;
// W2: the likes manager's sendLike sender (the only builder of the "urn:otgc_send_like" request: it calls the SendLike
// descriptor 0x1432B4590 and the OTGC ctor). Likes queue up from AddOwn on others' structures (0x142C9DDB0) and from
// driving on highways (HighwaysSendLikeProcessInterval): run 20261121-164550-two-games-ride-truck, the guest's game built
// two of them while riding the truck on the highway.
constexpr std::uintptr_t kLikeSend = 0x142E14820;
// W3 (round 7): the road online manager's create-road request (0x1434A13E0, its only caller 0x142E80BC1). It stores the
// road and sets the request flag +0x3090; the manager's update (0x1434A2B10) leaves state 0 for 1 on that flag, builds
// "urn:otgc_create_road" (descriptors 0x1432AA770 / 0x1432AA640, the OTGC ctor) and in state 2 sends
// "urn:otgc_road_upload". Entering the private room from the field calls it: run 20261122-171636-pr-self-off (hidden,
// game_server_writes 1) and the user's world-sync guest (round 7). The caller ignores the result (void).
constexpr std::uintptr_t kRoadRequest = 0x1434A13E0;
constexpr std::uintptr_t kSvcVtable = 0x143EDECC8;
constexpr std::uintptr_t kMgrGlobal = 0x147BF4960;  // DSConstructionPointManager*
constexpr std::uintptr_t kPool = 0x80, kSlot = 0x710;
constexpr unsigned kSlots = 3140;

struct Site {
    std::uintptr_t va;
    const char* sig;
    const char* name;
};
// clang-format off
constexpr Site kSites[] = {
    {kRegisterNew, "48 89 74 24 18 57 48 83 EC 20 48 8B 02 48 8B F1 48 8B CA 48 8B FA FF 10 48 8D 4E 38", "online RegisterNew"},
    {kDrain,       "40 53 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 30 48 89 7C 24 48", "online removal drain"},
    {kTelemetry,   "40 53 55 56 57 41 56 48 83 EC 40 48 8B AC 24 90 00 00 00", "online telemetry"},
    {kUpload,      "40 55 57 41 55 48 8D 6C 24 C0 48 81 EC 40 01 00 00", "materials upload"},
    {0x142CB5C10,  "48 81 C2 B8 00 00 00", "materials upload: the server id +0xB8"},
    {kOtgcCtor,    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B F1 48 89 01", "OTGC request ctor"},
    {0x142CAC843,  "80 BA AB 00 00 00 00", "createObject: offline gate 1 (cp+0xAB bit 7)"},
    {0x142CADA35,  "80 BA AB 00 00 00 00", "createObject: offline gate 2 (cp+0xAB bit 7)"},
    {kLikeSend,    "4C 8B DC 55 41 54 41 57 49 8D AB 48 FF FF FF 48 81 EC A0 01 00 00 48 8B", "likes: the sendLike sender"},
    {0x142E14BC9,  "E8 ?? ?? ?? ??", "likes: the SendLike descriptor call"},
    {0x142E14C04,  "E8 ?? ?? ?? ??", "likes: the OTGC ctor call"},
    {kRoadRequest, "40 56 48 83 EC 30 8B 81 38 30 00 00 48 8B F1 FF C8 83 F8 02 0F 86", "roads: the create-road request"},
    {0x1434A1527,  "C6 86 90 30 00 00 01", "roads: the request flag +0x3090"},
    {0x1434A2C10,  "41 80 BE 90 30 00 00 00 74 1A 41 BF 01 00 00 00", "roads: the update's state 0 -> 1 on +0x3090"},
    {0x142E80BC1,  "E8 ?? ?? ?? ??", "roads: the create-road request's caller"},
};
// clang-format on

std::atomic<bool> g_ok{false};
std::atomic<bool> g_sync_on{false};
bool g_automation = false;
// round 5: set at Install for a world-sync guest and never cleared: a stop of world sync (a fault, the co-op session
// ending) must not reopen her game to the server (her world is still a copy of the host's save)
bool g_guest_locked = false;
int g_role = 0;
char g_off[160] = "not installed";
thread_local int t_mirroring = 0;

std::atomic<std::uint32_t> c_reg{0}, c_drain{0}, c_tel{0}, c_up{0}, c_reg_pass{0}, c_drain_pass{0}, c_tel_pass{0}, c_up_pass{0}, c_otgc{0}, c_otgc_w{0},
    c_like{0}, c_like_pass{0}, c_road{0}, c_road_pass{0};

// the mod's construction points, host: the pool address AND the structure's id (cp+0x30). A pool slot is reused for a
// later structure (the host's own vanilla build): the id tells them apart (round 5).
SRWLOCK g_mod_lock = SRWLOCK_INIT;
constexpr unsigned kModCap = 512;
struct ModCp {
    std::uintptr_t cp;
    std::int32_t id;
};
ModCp g_mod[kModCap];
unsigned g_mod_n = 0;

// OTGC names seen (logged once each)
SRWLOCK g_name_lock = SRWLOCK_INIT;
constexpr unsigned kNameCap = 96;
char g_names[kNameCap][48];
unsigned g_name_n = 0;

using FnReg = void (*)(std::uintptr_t, std::uintptr_t);
using FnDrain = void (*)(std::uintptr_t, std::uintptr_t);
using FnTel = void (*)(std::uintptr_t, std::uintptr_t, std::uint8_t, std::uintptr_t, std::uintptr_t);
using FnUp = std::uint64_t (*)(std::uintptr_t, std::uintptr_t);
using FnOtgc = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t);
using FnLike = void (*)(std::uintptr_t, const void*);
FnLike o_like = nullptr;
// 12 integer arguments (the caller converts its floats with vcvttss2si): passed through as they are
using FnRoad = void (*)(std::uintptr_t, std::uintptr_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                        std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
FnRoad o_road = nullptr;
FnReg o_reg = nullptr;
FnDrain o_drain = nullptr;
FnTel o_tel = nullptr;
FnUp o_up = nullptr;
FnOtgc o_otgc = nullptr;

bool InPool(std::uintptr_t cp) noexcept {
    const std::uintptr_t mgr = mem::Q(A(kMgrGlobal));
    if (!mgr || cp < mgr + kPool) return false;
    const std::uintptr_t off = cp - (mgr + kPool);
    return off < kSlots * kSlot && off % kSlot == 0;
}
bool IsMod(std::uintptr_t cp) noexcept {
    std::int32_t id = 0;
    if (!mem::Rd(cp + 0x30, id)) return false;
    bool hit = false;
    AcquireSRWLockShared(&g_mod_lock);
    for (unsigned i = 0; i < g_mod_n && !hit; ++i) hit = g_mod[i].cp == cp && g_mod[i].id == id;
    ReleaseSRWLockShared(&g_mod_lock);
    return hit;
}
bool Drops(std::uintptr_t cp) noexcept {
    wrules::GateIn g;
    g.role = g_role;
    g.sync_on = g_sync_on.load(std::memory_order_acquire);
    g.automation = g_automation;
    g.mirroring = t_mirroring > 0;
    g.guest_locked = g_guest_locked;
    g.cp_known = cp && InPool(cp);
    if (g.cp_known) {
        std::uint8_t ab = 0;
        mem::Rd(cp + 0xAB, ab);
        g.cp_offline = (ab & 0x80) != 0;
        g.cp_mod = IsMod(cp);
    }
    return wrules::GateDrops(g);
}

void H_reg(std::uintptr_t svc, std::uintptr_t cp) {
    if (Drops(cp)) {
        if (c_reg.fetch_add(1) < 8) {
            std::int32_t id = 0;
            std::uint8_t type = 0;
            mem::Rd(cp + 0x30, id);
            mem::Rd(cp + 0x34, type);
            Log("[gate] dropped RegisterNew of structure %d (%s)%s", id, wrules::TypeName(type), g_automation ? " (hidden run)" : "");
        }
        return;
    }
    ++c_reg_pass;
    o_reg(svc, cp);
}
void H_drain(std::uintptr_t svc, std::uintptr_t cp) {
    if (Drops(cp)) {
        ++c_drain;
        return;
    }
    ++c_drain_pass;
    o_drain(svc, cp);
}
void H_tel(std::uintptr_t svc, std::uintptr_t obj, std::uint8_t kind, std::uintptr_t ev, std::uintptr_t p5) {
    if (Drops(obj)) {
        ++c_tel;
        // the original with no object only releases the event reference (its own early-out path) and returns
        o_tel(svc, 0, kind, ev, p5);
        return;
    }
    ++c_tel_pass;
    o_tel(svc, obj, kind, ev, p5);
}
std::uint64_t H_up(std::uintptr_t handler, std::uintptr_t cp) {
    if (Drops(cp)) {
        ++c_up;
        return 0;
    }
    ++c_up_pass;
    return o_up(handler, cp);
}
std::uintptr_t H_otgc(std::uintptr_t self, std::uintptr_t desc, std::uintptr_t body) {
    const std::uintptr_t r = o_otgc(self, desc, body);
    ++c_otgc;
    // desc[0] = the request name (a Decima string: chars, length at -8)
    char name[48] = {};
    const std::uintptr_t s = mem::Q(desc);
    std::int32_t len = 0;
    if (s && mem::Rd(s - 8, len) && len > 0) {
        const std::size_t n = static_cast<std::size_t>(len) < sizeof(name) - 1 ? static_cast<std::size_t>(len) : sizeof(name) - 1;
        if (mem::Read(s, name, n)) {
            name[n] = 0;
            for (std::size_t i = 0; i < n; ++i)
                if (static_cast<unsigned char>(name[i]) < 0x20 || static_cast<unsigned char>(name[i]) > 0x7E) name[i] = '?';
        } else {
            name[0] = 0;
        }
    }
    if (!name[0]) std::snprintf(name, sizeof(name), "<unreadable>");
    if (wrules::OtgcWriteName(name)) {
        ++c_otgc_w;
        Log("[gate] OTGC WRITE request: %s", name);
        return r;
    }
    bool seen = false;
    AcquireSRWLockExclusive(&g_name_lock);
    for (unsigned i = 0; i < g_name_n && !seen; ++i) seen = std::strcmp(g_names[i], name) == 0;
    if (!seen && g_name_n < kNameCap) std::snprintf(g_names[g_name_n++], sizeof(g_names[0]), "%s", name);
    ReleaseSRWLockExclusive(&g_name_lock);
    if (!seen) Log("[gate] OTGC request (read): %s (first of its kind)", name);
    return r;
}

// The likes sender: dropped in every hidden run (both games) and on a world-sync guest (her world is a copy of the host's
// save: its likes would go out with her account for the host's world). Real use, host or solo: vanilla.
void H_like(std::uintptr_t mgr, const void* p) {
    if (g_automation || (g_role == 2 && (g_guest_locked || g_sync_on.load(std::memory_order_acquire)))) {
        if (c_like.fetch_add(1) == 0) Log("[gate] dropped the likes manager's sendLike%s (further ones counted)", g_automation ? " (hidden run)" : " (world sync guest)");
        return;
    }
    ++c_like_pass;
    o_like(mgr, p);
}

// The create-road request (W3): dropped where the likes are (every hidden run, a world-sync guest). Dropped, the manager
// stays in state 0: no "urn:otgc_create_road", no "urn:otgc_road_upload". Real use, host or solo: vanilla.
void H_road(std::uintptr_t mgr, std::uintptr_t a2, std::uint64_t a3, std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
            std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11, std::uint64_t a12) {
    if (g_automation || (g_role == 2 && (g_guest_locked || g_sync_on.load(std::memory_order_acquire)))) {
        if (c_road.fetch_add(1) < 4)
            Log("[gate] dropped the road manager's create-road request (no urn:otgc_create_road / road_upload)%s",
                g_automation ? " (hidden run)" : " (world sync guest)");
        return;
    }
    ++c_road_pass;
    o_road(mgr, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
}

bool Hook(std::uintptr_t va, void* det, void** orig, const char* name) {
    void* t = reinterpret_cast<void*>(A(va));
    MH_STATUS s = MH_CreateHook(t, det, orig);
    if (s == MH_OK) s = MH_EnableHook(t);
    if (s != MH_OK) Log("[gate] hook %s 0x%llX: %s", name, static_cast<unsigned long long>(va), MH_StatusToString(s));
    return s == MH_OK;
}

}  // namespace

void Install(const Config& config, bool sync_wanted) noexcept {
    g_automation = automation::Enabled();
    g_role = config.role == Role::host ? 1 : config.role == Role::guest ? 2 : 0;
    if (!g_automation && !sync_wanted) {
        std::snprintf(g_off, sizeof(g_off), "not needed (world_sync off, not a hidden run)");
        return;
    }
    for (const Site& s : kSites)
        if (!mem::CodeMatches(A(s.va), s.sig)) {
            Log("[world] byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
            std::snprintf(g_off, sizeof(g_off), "this game build differs (%s)", s.name);
            Log("[gate] OFF: %s", g_off);
            return;
        }
    const std::uintptr_t vt = A(kSvcVtable);
    if (mem::Q(vt + 0x28) != A(kRegisterNew) || mem::Q(vt + 0x38) != A(kDrain) || mem::Q(vt + 0xB8) != A(kTelemetry)) {
        std::snprintf(g_off, sizeof(g_off), "this game build differs (the online service's vtable slots)");
        Log("[world] byte check FAILED: online service vtable 0x%llX slots +0x28/+0x38/+0xB8", static_cast<unsigned long long>(kSvcVtable));
        Log("[gate] OFF: %s", g_off);
        return;
    }
    if (mem::RipTarget(A(0x142E80BC1), 1, 5) != A(kRoadRequest)) {
        std::snprintf(g_off, sizeof(g_off), "this game build differs (the create-road request's caller)");
        Log("[world] byte check FAILED: the create-road request's caller 0x142E80BC1");
        Log("[gate] OFF: %s", g_off);
        return;
    }
    if (mem::RipTarget(A(0x142E14BC9), 1, 5) != A(0x1432B4590) || mem::RipTarget(A(0x142E14C04), 1, 5) != A(kOtgcCtor)) {
        std::snprintf(g_off, sizeof(g_off), "this game build differs (the sendLike sender's calls)");
        Log("[world] byte check FAILED: the sendLike sender 0x142E14820 calls");
        Log("[gate] OFF: %s", g_off);
        return;
    }
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        std::snprintf(g_off, sizeof(g_off), "MinHook init failed");
        return;
    }
    const bool ok = Hook(kRegisterNew, reinterpret_cast<void*>(&H_reg), reinterpret_cast<void**>(&o_reg), "RegisterNew") &&
                    Hook(kDrain, reinterpret_cast<void*>(&H_drain), reinterpret_cast<void**>(&o_drain), "drain") &&
                    Hook(kTelemetry, reinterpret_cast<void*>(&H_tel), reinterpret_cast<void**>(&o_tel), "telemetry") &&
                    Hook(kUpload, reinterpret_cast<void*>(&H_up), reinterpret_cast<void**>(&o_up), "upload") &&
                    Hook(kOtgcCtor, reinterpret_cast<void*>(&H_otgc), reinterpret_cast<void**>(&o_otgc), "OTGC ctor") &&
                    Hook(kLikeSend, reinterpret_cast<void*>(&H_like), reinterpret_cast<void**>(&o_like), "sendLike") &&
                    Hook(kRoadRequest, reinterpret_cast<void*>(&H_road), reinterpret_cast<void**>(&o_road), "create-road request");
    if (!ok) {
        std::snprintf(g_off, sizeof(g_off), "a detour could not be installed");
        Log("[gate] OFF: %s (the detours that went in pass by their rules)", g_off);
        return;
    }
    g_off[0] = 0;
    g_guest_locked = g_role == 2 && sync_wanted;
    g_ok.store(true);
    Log("[gate] online gate ON (%s%s): RegisterNew, removal drain, telemetry, materials upload, likes and create-road %s; OTGC requests logged", g_role == 1 ? "host" : g_role == 2 ? "guest" : "solo",
        g_automation ? ", hidden run" : "", g_automation ? "ALWAYS dropped (hidden run)" : g_guest_locked ? "ALWAYS dropped (a world-sync guest, for the whole session)" : "dropped for the mod's structures");
}

bool Ok() noexcept { return g_ok.load(); }
const char* OffReason() noexcept { return Ok() ? "" : g_off; }
void SetSyncOn(bool on) noexcept { g_sync_on.store(on, std::memory_order_release); }

ScopeMirroring::ScopeMirroring() noexcept { ++t_mirroring; }
ScopeMirroring::~ScopeMirroring() { --t_mirroring; }
bool Mirroring() noexcept { return t_mirroring > 0; }

void NoteModCp(std::uintptr_t cp) noexcept {
    std::int32_t id = 0;
    if (!cp || !mem::Rd(cp + 0x30, id)) return;
    AcquireSRWLockExclusive(&g_mod_lock);
    bool have = false;
    for (unsigned i = 0; i < g_mod_n && !have; ++i)
        if (g_mod[i].cp == cp) {
            g_mod[i].id = id;  // the slot's structure now
            have = true;
        }
    if (!have) {
        if (g_mod_n < kModCap) g_mod[g_mod_n++] = {cp, id};
        else g_mod[(cp / kSlot) % kModCap] = {cp, id};  // full: overwrite one
    }
    ReleaseSRWLockExclusive(&g_mod_lock);
}

Counts GetCounts() noexcept {
    Counts c;
    c.reg = c_reg, c.drain = c_drain, c.telemetry = c_tel, c.upload = c_up;
    c.reg_pass = c_reg_pass, c.drain_pass = c_drain_pass, c.telemetry_pass = c_tel_pass, c.upload_pass = c_up_pass;
    c.otgc = c_otgc, c.otgc_write = c_otgc_w;
    c.likes = c_like, c.likes_pass = c_like_pass;
    c.roads = c_road, c.roads_pass = c_road_pass;
    return c;
}
std::uint32_t DroppedTotal() noexcept { return c_reg + c_drain + c_tel + c_up + c_like + c_road; }
void LogCounts(const char* when) noexcept {
    const Counts c = GetCounts();
    Log("[gate] %s: dropped register %u, drain %u, telemetry %u, upload %u, likes %u, create-road %u (total %u); passed register %u, drain %u, telemetry %u, upload %u, likes %u, create-road %u; "
        "OTGC requests %u (write %u)%s",
        when ? when : "counts", c.reg, c.drain, c.telemetry, c.upload, c.likes, c.roads, DroppedTotal(), c.reg_pass, c.drain_pass, c.telemetry_pass, c.upload_pass,
        c.likes_pass, c.roads_pass, c.otgc, c.otgc_write, Ok() ? "" : " [gate not installed]");
}

}  // namespace st::gate
