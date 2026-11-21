// The carried passenger's camera. See ride_camera.hpp and docs/real-partner-body.md section 24.15.
#include "ride_camera.hpp"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "automation.hpp"
#include "MinHook.h"
#include "log.hpp"
#include "nametags.hpp"
#include "partner_source.hpp"

namespace st::ridecam {
namespace {
namespace mem = partner::mem;

constexpr std::uintptr_t kImage = 0x140000000ull;
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

// ---------------------------------------------------------------- ds.exe (Director's Cut)
// The player's DSCameraMode (RTTI DSCameraMode, 0x1C20 bytes): the controller (DSPlayerController) refers to it at +0x54D0
// through its WeakPtrRTTITarget base (mode + 0x20).
constexpr std::uintptr_t kModeVt = 0x143D6F398;       // DSCameraMode
constexpr std::uintptr_t kModeWeakVt = 0x143D6F450;   // its WeakPtrRTTITarget base at +0x20
constexpr std::uintptr_t kAroundVt = 0x143D6F610;     // the Around module (the fallback module: on foot, rides)
constexpr std::uintptr_t kModeVt0 = 0x14269AF60, kModeWeakVt0 = 0x1426833B0, kAroundSelect = 0x1426DB7F0;
constexpr std::uintptr_t kOnRoad = 0x142F6B370;       // (vehicle, 1, 1): its wheels on road materials (pure reads)
// The camera's collision filter: every camera query (the collision cast FUN_1426C8D40 and the other camera probes) sets up
// its filter with FilterIgnorePlayer(filter, player, layer) = 0x14382FD30: the entity tree the player is MOUNTED on (his
// MounterComponent's mount, +0x50 -> +0x48: the vehicle he drives), else the player alone, goes on the filter's ignore
// list (IgnoreEntityTree 0x142380B80). So the driver's camera never collides with his own vehicle; the carried passenger
// is mounted on nothing and his ride camera stopped at the vehicle's body (inside the cab: hidden run
// 20261120-183653-seat-pose). The detour adds the ridden vehicle's tree for our carried Sam, for the camera's callers only.
constexpr std::uintptr_t kFilterIgnorePlayer = 0x14382FD30;
constexpr std::uintptr_t kIgnoreEntityTree = 0x142380B80;
constexpr std::uintptr_t kFilterAddIgnore = 0x141C390E0;  // (filter, entity + 0x38): one entity on the filter's ignore list
// The camera's own collision filter (FUN_142698A50(mode, filter), called by the base FUN_1426CE0C0, the position pass
// FUN_1426DDFF0 and two more): the player's PARENT entity (Sam +0x70: the vehicle he sits in when he drives) is ignored
// (0x142380B00(entity, filter): entity + 0x38 on the list, then the filter's finish), then the mode's per-frame ignore list
// (+0xF08). A carried passenger has no parent: his camera's pull-in stopped at the vehicle's walls (runs 20261120-184648 /
// -185030: 3.1 m from the base with 11.25 in use, inside the truck, whatever the direction). The detour puts the ridden
// vehicle on that filter first, as the parent is for the driver.
constexpr std::uintptr_t kCamFilterBuild = 0x142698A50;
constexpr std::uintptr_t kIgnoreParent = 0x142380B00;
constexpr std::uintptr_t kEFlags = 0x88, kEFirstChild = 0x78, kENextSibling = 0x80;
constexpr std::uintptr_t kCamCodeLo = 0x142660000, kCamCodeHi = 0x142700000;  // the camera's code (DSCameraMode and its modules)
constexpr std::uintptr_t kCtlMode = 0x54D0;           // controller -> mode + 0x20
constexpr std::uintptr_t kMPlayer = 0x278, kMCtl = 0x280;
constexpr std::uintptr_t kMParam = 0x578;             // u16: the parameter in use
constexpr std::uintptr_t kMParamPtr = 0x580;          // DSCameraParameter* in use (+0x2C its BaseType)
constexpr std::uintptr_t kMReqAround = 0x598;         // u16: the Around module's request (0x17B none; reset every update)
constexpr std::uintptr_t kMReqFlags = 0x5BC;          // u32: the request's flags (the ride action writes 0xF)
constexpr std::uintptr_t kMBase = 0x610;              // float3: the camera's base (focus) this update
constexpr std::uintptr_t kMManual = 0x630;            // float4: the "Manual" base (BaseType 3)
constexpr std::uintptr_t kMDist = 0x680;              // float: the distance in use
constexpr std::uintptr_t kMPitch = 0xC10, kMHeading = 0xC14;
constexpr std::uintptr_t kMInVehicle = 0x147B;        // bit 0x80: in a vehicle (cleared every update)
constexpr std::uintptr_t kMVehYaw = 0x730;            // float4: the vehicle's yaw quaternion (the Around module's follow, 0x1426C4CD0)
constexpr std::uintptr_t kMVehYawFlags = 0x147F;      // bits 0x20 / 0x40: that quaternion is set
constexpr std::uintptr_t kVCamPoint = 0xA60;          // vehicle: float3, its camera point in its own frame (0x14283E090: + 0xA40 of +0x20)
constexpr std::uintptr_t kParamBaseType = 0x2C;

struct Site {
    std::uintptr_t va;
    const char* sig;
    const char* name;
};
// clang-format off
constexpr Site kCamSites[] = {
    {0x1426BEF67, "48 8B 8B 68 05 00 00 48 8B 01 FF 50 30 0F B7 8B 78 05 00 00", "camera update: the active module's select (vt+0x30), the parameter in use (+0x578)"},
    {0x1426BEFF4, "C7 83 88 05 00 00 7B 01 7B 01", "camera update: the requests reset (+0x588, 0x17B)"},
    {0x1426BF020, "C7 83 98 05 00 00 7B 01 7B 01", "camera update: the Around request reset (+0x598)"},
    {0x1426DC07C, "4C 8B 46 08 66 45 39 A0 98 05 00 00 74 7F 41 8B 90 BC 05 00 00", "Around select: the request (+0x598) and its flags (+0x5BC)"},
    {0x1426CE127, "0F B6 5A 2C", "camera base: the parameter's BaseType (+0x2C)"},
    {0x1426CE300, "C5 F8 10 87 30 06 00 00", "camera base: Manual = +0x630"},
    {0x14269535A, "4C 89 A3 D0 54 00 00", "camera attach: controller +0x54D0 = the mode + 0x20"},
    {0x1426952FA, "4C 89 BE 78 02 00 00 E8 ?? ?? ?? ?? 48 89 86 80 02 00 00", "camera attach: +0x278 the player, +0x280 the controller"},
    {0x142775A0C, "4C 8B A8 D0 54 00 00", "ride action: controller +0x54D0"},
    {0x142775B2B, "BB 2B 00 00 00", "ride action: the truck's parameter 0x2B"},
    {0x142775C0A, "49 8D 85 10 06 00 00 48 0F 44 C1", "ride action: the Manual base (+0x630)"},
    {0x142775C2C, "49 8D 85 78 05 00 00 48 0F 44 C1 B9 BC 05 00 00", "ride action: the request (+0x598)"},
    {0x142775C4E, "49 8D 85 9C 05 00 00 48 0F 44 C1", "ride action: the flags (+0x5BC)"},
    {0x142775C6E, "49 8D 85 5B 14 00 00 48 0F 44 C1", "ride action: in a vehicle (+0x147B)"},
    {0x1426D89B6, "C5 B2 58 86 14 0C 00 00", "camera look: the heading (+0xC14) plus the input"},
    {0x1426D8A06, "C5 FA 11 86 14 0C 00 00", "camera look: the heading stored"},
    {0x142775BBB, "48 8D 55 A0 48 8D 4C 24 70 E8", "ride action: the base point of the vehicle (0x14283E090)"},
    {0x14283E0D4, "B9 60 0A 00 00 48 85 C0 BA C8 00 00 00 4C 8D 80 40 0A 00 00 4C 0F 44 C1 48 8D 88 A8 00 00 00", "the base point: +0xA60 in the transform +0xC8"},
    {0x142775D2A, "49 8D 85 10 07 00 00 B9 30 07 00 00 4D 85 ED C5 F8 10 45 00 48 0F 44 C1 BA 7F 14 00 00 C5 FA 7F 00 49 8D 85 5F 14 00 00 48 0F 44 C2 80 08 60",
     "ride action: the yaw quaternion (+0x730) and its flags (+0x147F |= 0x60)"},
};
constexpr Site kCollSites[] = {
    {kFilterIgnorePlayer, "48 89 5C 24 18 57 48 83 EC 20 44 89 41 08 48 8B DA 48 8B F9 48 85 D2 74 30 48 8D 8A 90 00 00 00 48 8D 15 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 85 C0 74 0D 48 8B 40 50 48 85 C0 74 04 48 8B 58 48 48 8B D7 48 8B CB E8",
     "the camera filter's player ignore (the mounted-on entity tree)"},
    {kIgnoreEntityTree, "4C 8B C2 45 33 C9 48 8B D1 48 8B 0D ?? ?? ?? ?? 48 8B 89 18 04 00 00 E9", "the entity tree ignore"},
    {0x1426C8FB7, "48 8B 97 78 02 00 00 48 8B CE E8", "the camera collision cast's filter setup (the player +0x278)"},
    {kFilterAddIgnore, "48 89 5C 24 08 57 48 83 EC 20 48 8D 59 18 48 8B FA 8B 13 3B 51 1C 75 0B 83 C2 10 48 8B CB E8", "the filter's ignore-list append (+0x18)"},
    {kCamFilterBuild, "40 53 55 56 57 48 83 EC 28 48 8B 81 78 02 00 00 48 8B EA 4C 89 64 24 60 4C 89 7C 24 20 4C 8B F9 48 8B 78 70 48 85 FF 74 77",
     "the camera filter build (the player +0x278, his parent +0x70)"},
    {0x142698ADD, "49 8B 8F 78 02 00 00 48 8B D5 48 8B 49 70 E8", "the camera filter build: the parent's ignore"},
    {kIgnoreParent, "40 53 48 83 EC 20 48 8B DA 33 C0 48 85 C9 48 8D 51 38 48 8B CB 48 0F 44 D0 E8 ?? ?? ?? ?? 48 8B CB 48 83 C4 20 5B E9", "the entity ignore (+0x38, the filter's finish)"},
};
constexpr const char* kOnRoadSig = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 8B F1 45 0F B6 F0 48 8B 89 30 03 00 00";
// clang-format on

using FnOnRoad = int (*)(std::uintptr_t, std::uint8_t, std::uint8_t);
using FnFilterIgnorePlayer = void (*)(std::uintptr_t, std::uintptr_t, std::uint32_t);
using FnIgnoreTree = void (*)(std::uintptr_t, std::uintptr_t);
FnFilterIgnorePlayer o_filter = nullptr;
using FnCamFilterBuild = void (*)(std::uintptr_t, std::uintptr_t);
FnCamFilterBuild o_build = nullptr;
std::atomic<std::uintptr_t> g_ign_mode{0};                   // our carried Sam's camera mode (the feed)
std::atomic<std::uint32_t> g_ign_builds{0};
std::atomic<std::uintptr_t> g_ign_sam{0}, g_ign_vehicle{0};  // our carried Sam and the vehicle he rides (the feed)
std::atomic<std::uint64_t> g_ign_tick{0};                    // GetTickCount64 of the newest feed
std::atomic<std::uint32_t> g_ign_adds{0}, g_ign_faults{0};
std::atomic<bool> g_ign_enabled{true};                       // ride_test 25
bool g_coll_hooked = false;
char g_coll_off[160] = "not installed";

bool g_checks_ok = false, g_cfg_on = true, g_onroad_ok = false;
std::atomic<bool> g_on{false};
std::atomic<bool> g_feed_enabled{true};  // ride_test 20
std::atomic<bool> g_verbose{false};      // ride_test 21
char g_off[200] = "not installed";

// the feed (game thread)
struct FeedState {
    std::uintptr_t sam = 0, mode = 0, ctl = 0;
    bool logged = false, warned = false;
    unsigned ticks = 0, faults = 0;
    std::uint16_t want = 0;
    double since = 0;
};
FeedState g_f;
unsigned g_feeds = 0;

// The mode of a Sam (verified), 0 when not.
std::uintptr_t ModeOf(std::uintptr_t sam, std::uintptr_t* ctl_out) noexcept {
    if (!sam) return 0;
    const std::uintptr_t ctl = partner::FindComponentByName(reinterpret_cast<game::Entity*>(sam), "DSPlayerController");
    const std::uintptr_t ref = ctl ? mem::Q(ctl + kCtlMode) : 0;
    if (!ref) return 0;
    const std::uintptr_t m = ref - 0x20;
    if (mem::Q(m) != A(kModeVt) || mem::Q(m + 0x20) != A(kModeWeakVt) || mem::Q(m + kMPlayer) != sam || mem::Q(m + kMCtl) != ctl) return 0;
    if (ctl_out) *ctl_out = ctl;
    return m;
}
// still the same (cheap: every tick)
bool ModeStill(std::uintptr_t m, std::uintptr_t sam, std::uintptr_t ctl) noexcept {
    return m && mem::Q(m) == A(kModeVt) && mem::Q(m + kMPlayer) == sam && mem::Q(m + kMCtl) == ctl && mem::Q(ctl + kCtlMode) == m + 0x20;
}

std::uint16_t RideParam(std::uint8_t kind, bool on_road) noexcept {
    // the native ride action's table (state 2, 0x142775A1F..): normal / on a road (boost and race: not fed)
    switch (kind) {
        case 1: return on_road ? 0x30 : 0x2D;  // roadster (the game's "car")
        case 2: return on_road ? 0x2E : 0x2B;  // truck
        case 3: return on_road ? 0x2F : 0x2C;  // trike (the game's "bike")
        default: return 0;
    }
}

bool CallOnRoad(std::uintptr_t vehicle, int* out) noexcept {
    __try {
        *out = reinterpret_cast<FnOnRoad>(A(kOnRoad))(vehicle, 1, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The ride action's four writes (0x142775C0A..0x142775C7D), on a verified mode.
// The vehicle's yaw quaternion for +0x730: the ride action builds it with the game's matrix / angle / quaternion helpers
// (0x142775C80..0x142775D25: yaw and pitch of the vehicle, no roll); calling them faulted (run 20261120-191327: a 4-argument
// decomposition), so it is computed here from the forward axis. The convention (q or its conjugate) is measured on the
// driver's game against the native value (the meter logs both: ride_test 27).
bool YawQuat(const float (&r)[3][3], int conv, float (&q)[4]) noexcept {
    const double fx = r[1][0], fy = r[1][1], fz = r[1][2];
    const double h = std::sqrt(fx * fx + fy * fy);
    if (!(h > 1e-3)) return false;
    const double yaw = std::atan2(-fx, fy);             // about up (z), 0 = forward along +y
    const double pitch = std::atan2(fz, h);             // about the right axis (x), nose up > 0
    const double cy = std::cos(yaw / 2), sy = std::sin(yaw / 2), cp = std::cos(pitch / 2), sp = std::sin(pitch / 2);
    // Rz(yaw) * Rx(pitch)
    double x = cy * sp, y = sy * sp, z = sy * cp, w = cy * cp;
    if (conv == 2) x = -x, y = -y, z = -z;               // the conjugate
    q[0] = static_cast<float>(x), q[1] = static_cast<float>(y), q[2] = static_cast<float>(z), q[3] = static_cast<float>(w);
    return true;
}
std::atomic<int> g_yaw_mode{1};  // ride_test 27: the follow reference 0 off, 1 q (= the native value: run 20261120-191630), 2 its conjugate
bool WriteFeed(std::uintptr_t m, std::uint16_t param, const float (&base)[4], const float* yaw) noexcept {
    __try {
        *reinterpret_cast<volatile std::uint16_t*>(m + kMReqAround) = param;
        *reinterpret_cast<volatile std::uint32_t*>(m + kMReqFlags) = 0xF;
        volatile float* b = reinterpret_cast<volatile float*>(m + kMManual);
        b[0] = base[0], b[1] = base[1], b[2] = base[2], b[3] = base[3];
        volatile std::uint8_t* f = reinterpret_cast<volatile std::uint8_t*>(m + kMInVehicle);
        *f = static_cast<std::uint8_t>(*f | 0x80);
        if (yaw) {
            volatile float* y = reinterpret_cast<volatile float*>(m + kMVehYaw);
            y[0] = yaw[0], y[1] = yaw[1], y[2] = yaw[2], y[3] = yaw[3];
            volatile std::uint8_t* g = reinterpret_cast<volatile std::uint8_t*>(m + kMVehYawFlags);
            *g = static_cast<std::uint8_t>(*g | 0x60);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------- the meter
// Half extents of each kind's body in its own frame (right, forward, up; from the origin at its floor): approximate,
// for "the camera inside the vehicle" only.
struct Box {
    float x, y0, y1, z0, z1;
};
constexpr Box kBoxes[4] = {{0, 0, 0, 0, 0}, {1.0f, -2.2f, 2.1f, 0.0f, 1.4f}, {1.35f, -3.9f, 3.5f, 0.0f, 3.1f}, {0.5f, -1.3f, 1.3f, 0.0f, 1.3f}};

struct MeterAcc {
    double sum_d = 0, min_d = 1e9, max_d = 0, sum_h = 0, sum_behind = 0, sum_head = 0, max_head = 0, sum_bv = 0, max_bv = 0, sum_dist = 0, max_step = 0;
    unsigned n = 0, steps03 = 0, steps10 = 0, inside = 0, missed = 0, ride_param = 0, manual = 0, frames = 0;
    double last_cam[3] = {}, last_base[3] = {};
    bool have_last = false;
    double log_ms = 0;
    std::uint16_t param = 0, want = 0;
    int type = -1;
    bool carried = false;
    std::uint8_t kind = 0;
    std::uintptr_t mode = 0, ctl = 0, sam = 0;
};
MeterAcc g_m;

bool ReadXfPos(std::uintptr_t e, double (&p)[3], float (&rows)[3][3]) noexcept {
    std::uint8_t b[0x3C];
    if (!e || !mem::Read(e + 0xC8, b, sizeof(b))) return false;
    std::memcpy(p, b, 0x18);
    std::memcpy(rows, b + 0x18, 0x24);
    for (double v : p)
        if (!std::isfinite(v) || std::fabs(v) > 1e7) return false;
    return true;
}

bool CallIgnoreTree(std::uintptr_t vehicle, std::uintptr_t filter) noexcept {
    __try {
        reinterpret_cast<FnIgnoreTree>(A(kIgnoreEntityTree))(vehicle, filter);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallIgnoreParent(std::uintptr_t vehicle, std::uintptr_t filter) noexcept {
    __try {
        reinterpret_cast<FnIgnoreTree>(A(kIgnoreParent))(vehicle, filter);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The camera's thread: our carried Sam's mode, a feed in the last 250 ms: the ridden vehicle ignored as a driver's parent.
void H_CamFilterBuild(std::uintptr_t mode, std::uintptr_t filter) {
    if (mode && mode == g_ign_mode.load(std::memory_order_acquire) && g_ign_enabled.load(std::memory_order_relaxed)) {
        const std::uintptr_t v = g_ign_vehicle.load(std::memory_order_acquire);
        if (v && GetTickCount64() - g_ign_tick.load(std::memory_order_acquire) <= 250) {
            if (CallIgnoreParent(v, filter)) g_ign_builds.fetch_add(1, std::memory_order_relaxed);
            else g_ign_faults.fetch_add(1, std::memory_order_relaxed);
        }
    }
    o_build(mode, filter);
}
// The vehicle and its child entities (+0x78 first child, +0x80 next sibling), depth first, fault-free reads.
constexpr unsigned kMaxTree = 48;
unsigned TreeOf(std::uintptr_t root, std::uintptr_t (&out)[kMaxTree]) noexcept {
    unsigned n = 0;
    std::uintptr_t stack[kMaxTree];
    unsigned sp = 0;
    stack[sp++] = root;
    while (sp && n < kMaxTree) {
        const std::uintptr_t e = stack[--sp];
        if (!e) continue;
        out[n++] = e;
        for (std::uintptr_t c = mem::Q(e + kEFirstChild), guard = 0; c && guard < kMaxTree && sp < kMaxTree; c = mem::Q(c + kENextSibling), ++guard)
            stack[sp++] = c;
    }
    return n;
}
// The camera's thread. Our carried Sam, a camera caller, a feed in the last 250 ms: the ridden vehicle's tree too (as the
// game does for the vehicle a player drives).
void H_FilterIgnorePlayer(std::uintptr_t filter, std::uintptr_t player, std::uint32_t layer) {
    o_filter(filter, player, layer);
    if (!player || player != g_ign_sam.load(std::memory_order_acquire) || !g_ign_enabled.load(std::memory_order_relaxed)) return;
    const std::uintptr_t ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (ret < A(kCamCodeLo) || ret >= A(kCamCodeHi)) return;
    const std::uintptr_t v = g_ign_vehicle.load(std::memory_order_acquire);
    if (!v || GetTickCount64() - g_ign_tick.load(std::memory_order_acquire) > 250) return;
    if (CallIgnoreTree(v, filter)) g_ign_adds.fetch_add(1, std::memory_order_relaxed);
    else g_ign_faults.fetch_add(1, std::memory_order_relaxed);
}
void InstallCollision() {
    for (const Site& s : kCollSites)
        if (!mem::CodeMatches(A(s.va), s.sig)) {
            std::snprintf(g_coll_off, sizeof(g_coll_off), "byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
            Log("[ridecam] %s: the passenger's camera collides with the vehicle he rides (it may sit inside it)", g_coll_off);
            return;
        }
    if (mem::RipTarget(A(0x1426C8FC1), 1, 5) != A(kFilterIgnorePlayer) || mem::RipTarget(A(0x14234A690), 1, 5) != A(kIgnoreParent) ||
        mem::RipTarget(A(0x142698AEB), 1, 5) != A(0x14234A690)) {
        std::snprintf(g_coll_off, sizeof(g_coll_off), "the collision cast's call does not reach 0x14382FD30");
        Log("[ridecam] byte check FAILED: %s", g_coll_off);
        return;
    }
    void* t = reinterpret_cast<void*>(A(kFilterIgnorePlayer));
    void* t2 = reinterpret_cast<void*>(A(kCamFilterBuild));
    MH_STATUS s = MH_CreateHook(t, reinterpret_cast<void*>(&H_FilterIgnorePlayer), reinterpret_cast<void**>(&o_filter));
    if (s == MH_OK) {
        s = MH_CreateHook(t2, reinterpret_cast<void*>(&H_CamFilterBuild), reinterpret_cast<void**>(&o_build));
        if (s != MH_OK) MH_RemoveHook(t);
    }
    if (s == MH_OK) s = MH_EnableHook(t2);
    if (s == MH_OK) {
        s = MH_EnableHook(t);
        if (s != MH_OK) MH_DisableHook(t2);
    }
    if (s != MH_OK) {
        std::snprintf(g_coll_off, sizeof(g_coll_off), "the hook failed (%s)", MH_StatusToString(s));
        Log("[ridecam] the camera filter detour is OFF: %s", g_coll_off);
        return;
    }
    g_coll_hooked = true;
    g_coll_off[0] = 0;
}

}  // namespace

void Install(const Config& config) noexcept {
    g_cfg_on = config.ride_camera;
    unsigned bad = 0;
    for (const Site& s : kCamSites)
        if (!mem::CodeMatches(A(s.va), s.sig)) {
            ++bad;
            Log("[ridecam] byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
            if (bad == 1) std::snprintf(g_off, sizeof(g_off), "this game build differs (%s)", s.name);
        }
    struct Slot {
        std::uintptr_t vt;
        int slot;
        std::uintptr_t fn;
        const char* name;
    };
    const Slot slots[] = {{kModeVt, 0, kModeVt0, "the DSCameraMode vtable"},
                          {kModeWeakVt, 0, kModeWeakVt0, "its weak base's vtable"},
                          {kAroundVt, 6, kAroundSelect, "the Around module's select (vtable slot 6)"}};
    for (const Slot& s : slots)
        if (mem::Q(A(s.vt) + 8ull * s.slot) != A(s.fn)) {
            ++bad;
            Log("[ridecam] byte check FAILED: %s (0x%llX slot %d)", s.name, static_cast<unsigned long long>(s.vt), s.slot);
            if (bad == 1) std::snprintf(g_off, sizeof(g_off), "this game build differs (%s)", s.name);
        }
    g_checks_ok = bad == 0;
    g_onroad_ok = mem::CodeMatches(A(kOnRoad), kOnRoadSig);
    const unsigned n = static_cast<unsigned>(sizeof(kCamSites) / sizeof(kCamSites[0])) + 3;
    if (!g_checks_ok) {
        Log("[ridecam] the passenger's ride camera OFF: %s (%u of %u checks failed): a carried passenger keeps the on-foot camera", g_off, bad, n);
        return;
    }
    if (!g_cfg_on) {
        std::snprintf(g_off, sizeof(g_off), "session.ini ride_camera=0");
        Log("[ridecam] the passenger's ride camera OFF: %s (%u byte checks ok): the on-foot camera (v0.20)", g_off, n);
        return;
    }
    g_off[0] = 0;
    InstallCollision();
    g_on.store(true);
    Log("[ridecam] the passenger's ride camera ON: %u byte checks; the game's own ride camera (truck 0x2B, roadster 0x2D, trike 0x2C%s) on the "
        "vehicle the passenger rides; its collision ignores that vehicle %s%s",
        n, g_onroad_ok ? "; on a road 0x2E / 0x30 / 0x2F" : "; the on-road variants OFF: the wheel check differs",
        g_coll_hooked ? "(the camera filter detours: 7 more sites, 3 call targets)" : "NOT: ", g_coll_hooked ? "" : g_coll_off);
}

bool Active() noexcept { return g_on.load(); }
bool FilterInitHooked() noexcept { return g_coll_hooked; }
const char* OffReason() noexcept { return g_on.load() ? "" : g_off; }

void Feed(game::Entity* sam_e, std::uintptr_t vehicle, std::uint8_t kind, const game::WorldTransform& xf, const double (&lead)[3]) noexcept {
    if (!g_on.load() || !sam_e || !vehicle) return;
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(sam_e);
    if (g_f.sam != sam || !ModeStill(g_f.mode, sam, g_f.ctl)) {
        const bool first = g_f.sam == 0;
        const unsigned ticks = g_f.ticks;
        g_f = FeedState{};
        g_f.sam = sam;
        g_f.mode = ModeOf(sam, &g_f.ctl);
        if (!g_f.mode) {
            if (first) Log("[ridecam] our Sam's camera mode is not found (DSPlayerController +0x54D0, its vtables): the on-foot camera stays");
            return;
        }
        ++g_feeds;
        if (!first) Log("[ridecam] our Sam's camera mode changed (after %u fed ticks): found again", ticks);
    }
    // the camera's collision: the vehicle as his parent (also with the feed off: ride_test 20 0, the on-foot camera close up)
    g_ign_vehicle.store(vehicle, std::memory_order_release);
    g_ign_sam.store(sam, std::memory_order_release);
    g_ign_mode.store(g_f.mode, std::memory_order_release);
    g_ign_tick.store(GetTickCount64(), std::memory_order_release);
    if (!g_feed_enabled.load(std::memory_order_relaxed)) return;
    int road = 0;
    if (g_onroad_ok && !CallOnRoad(vehicle, &road)) {
        g_onroad_ok = false;
        Log("[ridecam] FAULT in the on-road wheel check: the plain ride parameters from now on");
        road = 0;
    }
    const std::uint16_t param = RideParam(kind, road != 0);
    if (!param) return;
    // the base: the vehicle's camera point (as the ride action's), one frame ahead
    float local[3] = {};
    if (!mem::Read(vehicle + kVCamPoint, local, sizeof(local)) || !std::isfinite(local[0]) || !std::isfinite(local[1]) || !std::isfinite(local[2]) ||
        std::fabs(local[0]) > 10 || std::fabs(local[1]) > 10 || std::fabs(local[2]) > 10)
        local[0] = local[1] = local[2] = 0;
    const float(&r)[3][3] = xf.rot.m;
    double base[3];
    const double org[3] = {xf.pos.x, xf.pos.y, xf.pos.z};
    for (int k = 0; k < 3; ++k) base[k] = org[k] + local[0] * r[0][k] + local[1] * r[1][k] + local[2] * r[2][k] + lead[k];
    const float b[4] = {static_cast<float>(base[0]), static_cast<float>(base[1]), static_cast<float>(base[2]), 0.0f};
    float yq[4] = {};
    const int ym = g_yaw_mode.load(std::memory_order_relaxed);
    const bool have_yaw = ym != 0 && YawQuat(r, ym, yq);
    if (!WriteFeed(g_f.mode, param, b, have_yaw ? yq : nullptr)) {
        ++g_f.faults;
        Log("[ridecam] FAULT writing the ride camera request: off for the session");
        g_on.store(false);
        std::snprintf(g_off, sizeof(g_off), "a fault in its write");
        return;
    }
    g_f.want = param;
    if (!g_f.logged) {
        g_f.logged = true;
        std::uintptr_t tree[kMaxTree];
        const unsigned nt = TreeOf(vehicle, tree);
        unsigned flagged = 0;
        for (unsigned i = 0; i < nt; ++i) flagged += (mem::Q(tree[i] + kEFlags) >> 14) & 1;
        const std::uint64_t vf = mem::Q(vehicle + kEFlags);
        Log("[ridecam] the passenger's ride camera: parameter 0x%02X (%s%s) on the vehicle's camera point (%.2f, %.2f, %.2f in its frame) at (%.1f, "
            "%.1f, %.1f); yaw quaternion %s (%.3f, %.3f, %.3f, %.3f); mode 0x%llX; the vehicle's tree %u entities (%u with the ignorable flag; its own "
            "flags bit 14 %d, bit 31 %d)",
            param, kind == 1 ? "roadster" : kind == 2 ? "truck" : "trike", road ? ", on a road" : "", local[0], local[1], local[2], base[0], base[1], base[2],
            have_yaw ? "on" : "OFF", yq[0], yq[1], yq[2], yq[3], static_cast<unsigned long long>(g_f.mode), nt, flagged, static_cast<int>((vf >> 14) & 1),
            static_cast<int>((vf >> 31) & 1));
    }
    ++g_f.ticks;
}

// The heading's slow follow (v0.21, section 24.15). The game's ride camera follows the ridden vehicle only for its driver
// (the player's own ride id; the yaw quaternion alone did not do it: run 20261120-192044, the passenger's heading stayed put
// while the driver's swung behind the truck). While the vehicle moves (over 3 m/s) and the passenger has not turned the
// camera for 2 s, his heading (+0xC14) eases toward the vehicle's forward (k 0.2 / s, at most 25 deg/s, as the driver's
// measured recentring); any look input (the heading changed by something else than us) pauses it. The heading's sign
// against the world is measured once by a 0.02 rad nudge (the camera's own forward from the view object).
struct FollowState {
    std::uintptr_t mode = 0;
    bool have_written = false;
    float written = 0;
    double idle_since = 0, last_ms = 0;
    int sign = 0;                 // +1 / -1: the camera's world yaw against +0xC14; 0 not measured
    bool probing = false;
    double probe_yaw = 0;
    float probe_from = 0;
    unsigned eased = 0, paused = 0;
};
FollowState g_fol;
std::atomic<bool> g_follow_enabled{true};  // ride_test 28 0/1
double WrapPi(double a) noexcept {
    while (a > 3.141592653589793) a -= 6.283185307179586;
    while (a < -3.141592653589793) a += 6.283185307179586;
    return a;
}
bool WriteHeading(std::uintptr_t m, float h) noexcept {
    __try {
        *reinterpret_cast<volatile float*>(m + kMHeading) = h;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Follow(game::Entity* sam_e, const game::WorldTransform& xf, double speed_mps) noexcept {
    const std::uintptr_t m = g_f.mode;
    if (!g_on.load() || !g_f.ticks || !m || !g_follow_enabled.load(std::memory_order_relaxed)) return;
    const double now = static_cast<double>(GetTickCount64());
    if (g_fol.mode != m) g_fol = FollowState{}, g_fol.mode = m, g_fol.idle_since = now, g_fol.last_ms = now;
    float h = 0;
    double cam[3], fwd[3];
    if (!mem::Rd(m + kMHeading, h) || !std::isfinite(h) || !nametags::CameraView(sam_e, cam, fwd)) return;
    const double dt = (std::min)(0.1, (std::max)(0.0, (now - g_fol.last_ms) / 1000.0));
    g_fol.last_ms = now;
    const double cam_yaw = std::atan2(fwd[1], fwd[0]);
    if (g_fol.probing) {
        g_fol.probing = false;
        const double d = WrapPi(cam_yaw - g_fol.probe_yaw);
        if (std::fabs(d) > 0.005 && std::fabs(d) < 0.2) {
            g_fol.sign = d > 0 ? 1 : -1;
            Log("[ridecam] heading follow: the camera turned %+.3f rad for +0.020 at +0xC14: sign %+d", d, g_fol.sign);
        }
        g_fol.have_written = WriteHeading(m, g_fol.probe_from), g_fol.written = g_fol.probe_from;
        return;
    }
    if (g_fol.have_written && std::fabs(h - g_fol.written) > 1e-4f) {
        g_fol.idle_since = now;  // the passenger turned the camera (or the game did)
        ++g_fol.paused;
    }
    g_fol.have_written = false;
    if (speed_mps < 3.0 || now - g_fol.idle_since < 2000.0) return;
    if (g_fol.sign == 0) {
        g_fol.probing = true, g_fol.probe_yaw = cam_yaw, g_fol.probe_from = h;
        g_fol.have_written = WriteHeading(m, h + 0.02f), g_fol.written = h + 0.02f;
        return;
    }
    const double veh_yaw = std::atan2(xf.rot.m[1][1], xf.rot.m[1][0]);
    const double err = WrapPi(veh_yaw - cam_yaw);
    const double max_step = 25.0 * 0.017453292519943295 * dt;
    double step = err * 0.2 * dt;
    step = step > max_step ? max_step : step < -max_step ? -max_step : step;
    if (std::fabs(step) < 1e-5) return;
    const float nh = static_cast<float>(WrapPi(h + g_fol.sign * step));
    if (WriteHeading(m, nh)) g_fol.have_written = true, g_fol.written = nh, ++g_fol.eased;
}

void Stop(const char* why) noexcept {
    if (g_fol.eased || g_fol.paused) Log("[ridecam] heading follow: %u eased ticks, %u look inputs (paused 2 s each)", g_fol.eased, g_fol.paused);
    g_fol = FollowState{};
    g_ign_sam.store(0, std::memory_order_release);
    g_ign_mode.store(0, std::memory_order_release);
    g_ign_vehicle.store(0, std::memory_order_release);
    if (g_f.sam && g_f.ticks)
        Log("[ridecam] the passenger's ride camera stops (%s) after %u fed ticks (the vehicle ignored in %u camera filters and %u player filters%s): "
            "the game blends back to the on-foot camera",
            why, g_f.ticks, g_ign_builds.exchange(0), g_ign_adds.exchange(0), g_ign_faults.load() ? ", FAULTS in the ignore" : "");
    g_f = FeedState{};
}

void Meter(game::Entity* local, std::uintptr_t vehicle, std::uint8_t kind, bool carried, double now_ms) noexcept {
    if (!g_checks_ok || !local || !vehicle || kind < 1 || kind > 3) {
        g_m = MeterAcc{};
        return;
    }
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(local);
    if (g_m.sam != sam || g_m.kind != kind || g_m.carried != carried || !ModeStill(g_m.mode, sam, g_m.ctl)) {
        g_m = MeterAcc{};
        g_m.sam = sam, g_m.kind = kind, g_m.carried = carried, g_m.log_ms = now_ms;
        g_m.mode = ModeOf(sam, &g_m.ctl);
        if (!g_m.mode) return;
    }
    const std::uintptr_t m = g_m.mode;
    double cam[3], fwd[3], vp[3];
    float rows[3][3];
    std::uint16_t param = 0;
    float base_f[3] = {}, dist = 0;
    if (!nametags::CameraView(local, cam, fwd) || !ReadXfPos(vehicle, vp, rows) || !mem::Rd(m + kMParam, param) || !mem::Read(m + kMBase, base_f, sizeof(base_f)) ||
        !mem::Rd(m + kMDist, dist))
        return;
    std::uint8_t type = 0xFF;
    const std::uintptr_t pp = mem::Q(m + kMParamPtr);
    if (pp) mem::Rd(pp + kParamBaseType, type);
    ++g_m.frames;
    const double base[3] = {base_f[0], base_f[1], base_f[2]};
    // a new camera frame (the view object changes once per camera update; the tick may see the same one twice)
    if (g_m.have_last && cam[0] == g_m.last_cam[0] && cam[1] == g_m.last_cam[1] && cam[2] == g_m.last_cam[2]) return;
    double rel[3], d2 = 0, bv2 = 0, step2 = 0;
    for (int k = 0; k < 3; ++k) {
        rel[k] = cam[k] - vp[k];
        d2 += rel[k] * rel[k];
        bv2 += (base[k] - vp[k]) * (base[k] - vp[k]);
        const double s = (cam[k] - base[k]) - (g_m.last_cam[k] - g_m.last_base[k]);
        step2 += s * s;
    }
    const double d = std::sqrt(d2), bv = std::sqrt(bv2);
    const double lx = rel[0] * rows[0][0] + rel[1] * rows[0][1] + rel[2] * rows[0][2];
    const double ly = rel[0] * rows[1][0] + rel[1] * rows[1][1] + rel[2] * rows[1][2];
    const double lz = rel[0] * rows[2][0] + rel[1] * rows[2][1] + rel[2] * rows[2][2];
    const Box& bx = kBoxes[kind];
    if (std::fabs(lx) < bx.x && ly > bx.y0 && ly < bx.y1 && lz > bx.z0 && lz < bx.z1) ++g_m.inside;
    // the camera's heading against the vehicle's (horizontal)
    const double ch = std::atan2(fwd[1], fwd[0]), vh = std::atan2(rows[1][1], rows[1][0]);
    double dh = std::fabs(ch - vh);
    while (dh > 3.141592653589793) dh = std::fabs(dh - 6.283185307179586);
    dh *= 57.29577951308232;
    if (g_m.have_last) {
        const double st = std::sqrt(step2);
        if (st > 0.3) ++g_m.steps03;
        if (st > 1.0) ++g_m.steps10;
        if (st > g_m.max_step) g_m.max_step = st;
    }
    std::memcpy(g_m.last_cam, cam, sizeof(cam));
    std::memcpy(g_m.last_base, base, sizeof(base));
    g_m.have_last = true;
    ++g_m.n;
    g_m.sum_d += d, g_m.min_d = (std::min)(g_m.min_d, d), g_m.max_d = (std::max)(g_m.max_d, d);
    g_m.sum_h += cam[2] - vp[2];
    g_m.sum_behind += -ly;
    g_m.sum_head += dh, g_m.max_head = (std::max)(g_m.max_head, dh);
    g_m.sum_bv += bv, g_m.max_bv = (std::max)(g_m.max_bv, bv);
    g_m.sum_dist += dist;
    const std::uint16_t want = RideParam(kind, false);
    const bool ride = param == want || param == RideParam(kind, true);
    if (ride) ++g_m.ride_param;
    if (type == 3) ++g_m.manual;
    if (carried && g_f.ticks && !ride) ++g_m.missed;
    g_m.param = param, g_m.type = type == 0xFF ? -1 : type, g_m.want = carried ? g_f.want : 0;
    const double every = g_verbose.load(std::memory_order_relaxed) ? 1000.0 : 5000.0;
    if (now_ms - g_m.log_ms < every || !g_m.n) return;
    const double n = g_m.n;
    float pitch = 0, heading = 0;
    mem::Rd(m + kMPitch, pitch);
    mem::Rd(m + kMHeading, heading);
    Log("[ridecam] %s %s: parameter 0x%02X, BaseType %d, distance in use %.2f m; the camera to the vehicle mean %.2f m (min %.2f, max %.2f), "
        "%.2f m above it, %.2f m behind it, heading %.0f deg off its own (max %.0f); base vs the vehicle mean %.2f m (max %.2f); steps over 0.3 m "
        "%u, over 1 m %u (max %.2f m) in %u camera frames; inside the vehicle %u; the ride parameter in %u, Manual in %u%s; pitch %.2f heading %.2f",
        carried ? "passenger" : "driver", kind == 1 ? "roadster" : kind == 2 ? "truck" : "trike", g_m.param, g_m.type, g_m.sum_dist / n,
        g_m.sum_d / n, g_m.min_d, g_m.max_d, g_m.sum_h / n, g_m.sum_behind / n, g_m.sum_head / n, g_m.max_head, g_m.sum_bv / n, g_m.max_bv, g_m.steps03,
        g_m.steps10, g_m.max_step, g_m.n, g_m.inside, g_m.ride_param, g_m.manual, carried ? (g_f.ticks ? "" : " (not fed)") : "", pitch, heading);
    if (carried && g_m.missed) Log("[ridecam] passenger: %u camera frames without the ride parameter while it was requested", g_m.missed);
    {
        // the follow reference: what the game's ride action wrote (the driver) or we did (the passenger), against ours
        float nq[4] = {}, q1[4] = {}, q2[4] = {};
        std::uint8_t fl = 0;
        mem::Read(m + kMVehYaw, nq, sizeof(nq));
        mem::Rd(m + kMVehYawFlags, fl);
        YawQuat(rows, 1, q1);
        YawQuat(rows, 2, q2);
        Log("[ridecam] %s: yaw quaternion in the camera (%.4f, %.4f, %.4f, %.4f) flags 0x%02X; ours q (%.4f, %.4f, %.4f, %.4f), conjugate (%.4f, %.4f, "
            "%.4f, %.4f); vehicle forward (%.3f, %.3f, %.3f)",
            carried ? "passenger" : "driver", nq[0], nq[1], nq[2], nq[3], fl, q1[0], q1[1], q1[2], q1[3], q2[0], q2[1], q2[2], q2[3], rows[1][0], rows[1][1],
            rows[1][2]);
    }
    const MeterAcc keep = g_m;
    g_m = MeterAcc{};
    g_m.sam = keep.sam, g_m.kind = keep.kind, g_m.carried = keep.carried, g_m.mode = keep.mode, g_m.ctl = keep.ctl, g_m.log_ms = now_ms;
    std::memcpy(g_m.last_cam, keep.last_cam, sizeof(g_m.last_cam));
    std::memcpy(g_m.last_base, keep.last_base, sizeof(g_m.last_base));
    g_m.have_last = keep.have_last;
}

bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    switch (op) {
        case 20:
            g_feed_enabled.store(v[1] != 0);
            Log("[ridecam] test: the passenger's ride camera feed %s", v[1] != 0 ? "ON" : "OFF (v0.20: the on-foot camera)");
            return std::snprintf(msg, cap, "ride camera feed %s (%s)", v[1] != 0 ? "on" : "off", g_on.load() ? "installed" : g_off), true;
        case 21:
            g_verbose.store(v[1] != 0);
            return std::snprintf(msg, cap, "camera meter every %s", v[1] != 0 ? "1 s" : "5 s"), true;
        case 27:
            g_yaw_mode.store(v[1] >= 0 && v[1] <= 2 ? static_cast<int>(v[1]) : 0);
            Log("[ridecam] test: the vehicle's yaw quaternion (the camera's follow) %s", v[1] == 1 ? "ON (q)" : v[1] == 2 ? "ON (conjugate)" : "OFF");
            return std::snprintf(msg, cap, "yaw quaternion mode %d", g_yaw_mode.load()), true;
        case 28:
            g_follow_enabled.store(v[1] != 0);
            Log("[ridecam] test: the heading follow %s", v[1] != 0 ? "ON" : "OFF");
            return std::snprintf(msg, cap, "heading follow %s", v[1] != 0 ? "on" : "off"), true;
        case 25:
            g_ign_enabled.store(v[1] != 0);
            Log("[ridecam] test: the camera collision %s the ridden vehicle", v[1] != 0 ? "ignores" : "does NOT ignore");
            return std::snprintf(msg, cap, "camera ignores the vehicle: %s (%s)", v[1] != 0 ? "on" : "off", g_coll_hooked ? "hooked" : g_coll_off), true;
        default: return std::snprintf(msg, cap, "unknown op %d", op), false;
    }
}

}  // namespace st::ridecam
