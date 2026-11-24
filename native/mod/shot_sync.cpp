// Shots, ship A (shot_sync.hpp; docs/enemy-sync.md P3a).
#include "shot_sync.hpp"

#include <windows.h>
#include <psapi.h>
#include <intrin.h>

#include "MinHook.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "automation.hpp"
#include "game_actions.hpp"
#include "link.hpp"
#include "log.hpp"
#include "nametags.hpp"
#include "overlay/overlay_input.hpp"
#include "partner_source.hpp"
#include "real_body.hpp"
#include "sam_damage.hpp"
#include "shot_wire.hpp"

namespace st::shots {
namespace {
namespace mem = partner::mem;
using game::WorldTransform;

constexpr std::uintptr_t kImage = 0x140000000ull;
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

// ---------------------------------------------------------------- game addresses (ds.exe, Director's Cut)
constexpr std::uintptr_t kEnqueue = 0x142916860;        // DSBulletSystem::Enqueue(system, record 0xF0): one bullet; 16 a frame
constexpr std::uintptr_t kEnqueueSlots = 0x142916886;   // count +0xC < 16, serial +8, slot = system + count * 0xF0
constexpr std::uintptr_t kEnqueueCopy = 0x1429168BC;    // the record copied to slot + 0x10, the serial to slot + 0xEC
constexpr std::uintptr_t kCreateBullet = 0x14294AD40;   // the script export's thunk: the byte arguments, then jmp the body
constexpr std::uintptr_t kCreateBody = 0x14294D340;     // CreateBullet's body
constexpr std::uintptr_t kRegInsn = 0x14294D3FD;        // mov rdi, [rip -> 0x147BEB0F8]: the weapon / magazine registry
constexpr std::uintptr_t kSysInsn = 0x14294DBA9;        // mov rcx, [rip -> 0x147BEB108]; lea rdx, rec; call Enqueue
constexpr std::uintptr_t kRegGlobal = 0x147BEB0F8, kSysGlobal = 0x147BEB108;
constexpr std::uintptr_t kWeaponById = 0x1429252E0;     // registry +0x10 count, +0x18 list, key u8 entry +0x20 (0 when missing)
constexpr std::uintptr_t kMagById = 0x142923E60;        // registry +0x20 count, +0x28 list, key u8 entry +0x20 (list[-1] when missing)
constexpr std::uintptr_t kCurWeapon = 0x1427A4390;      // the inventory's current weapon (slot +0x1C78, instances +0xD0 + 0x70 * slot)
constexpr std::uintptr_t kHumanoidDmg = 0x1437393D0;    // Humanoid MsgDamage handler (MULEs, Demens, warriors)
constexpr std::uintptr_t kEnqueueCallers[] = {0x142918FC4, 0x14294CFA3, 0x14294DBB8, 0x142984727, 0x142DF8A90};
constexpr std::uintptr_t kRegDummy = 0x5D718;           // registry +0x5D718: the system dummy entity's reference (CreateBullet's owner)
constexpr std::uintptr_t kPlayerVtable = 0x3D7D0D8, kPInv = 0x54C8, kCompOwner = 0x48;
constexpr unsigned kHandJoint = 26;                     // MTP_R_Hand_a (the 77-joint layout)

struct Site {
    std::uintptr_t va;
    const char* sig;
    const char* name;
    bool npc;  // only the Humanoid drop needs it
};
// clang-format off
constexpr Site kSites[] = {
    {kEnqueue,      "40 53 55 48 83 EC 28 48 89 74 24 50 48 8D 99 18 A0 02 00 48 8B F1 4C 89", "the bullet Enqueue", false},
    {kEnqueueSlots, "4C 63 46 0C 41 83 F8 10 7C 0A BD FF FF 00 00 E9 41 01 00 00 8B 6E 08 49 8B D6 48 89 7C 24 58 49 69 F8 F0 00 00 00", "the Enqueue's slots", false},
    {kEnqueueCopy,  "48 8D 4F 10 E8 7B 80 FF FF 89 AF EC 00 00 00 48 8B 4F 30", "the Enqueue's record copy and serial", false},
    {kCreateBullet, "0F B6 44 24 70 88 44 24 70 0F B6 44 24 68 88 44 24 68 0F B6 44 24 60 88 44 24 60 0F B6 44 24 58 88 44 24 58 "
                    "0F B6 44 24 50 88 44 24 50 0F B6 44 24 48 88 44 24 48 0F B6 44 24 40 88 44 24 40 0F B6 44 24 38 88 44 24 38 "
                    "0F B7 44 24 30 66 89 44 24 30 E9 A9 25 00 00", "CreateBullet (the thunk's argument layout)", false},
    {kCreateBody,   "40 56 57 41 54 41 55 41 57 48 81 EC E0 03 00 00 C5 78 29 94 24 90 03 00", "CreateBullet's body", false},
    {kRegInsn,      "48 8B 3D F4 DC 29 05", "the registry global in CreateBullet", false},
    {kSysInsn,      "48 8B 0D 58 D5 29 05 48 8D 94 24 40 02 00 00 E8 A3 8C FC FF", "the bullet system global and CreateBullet's Enqueue call", false},
    {kWeaponById,   "48 89 5C 24 08 48 63 41 10 45 33 D2 48 8B D9 85 C0 7E 24 4C 8B 49 18 4C 8B D8 45 8B C2 0F 1F 00 49 8B 01 3A 50 20", "WeaponById (+0x10 / +0x18, key +0x20)", false},
    {kMagById,      "48 89 5C 24 08 48 63 41 20 45 33 D2 48 8B D9 85 C0 7E 24 4C 8B 49 28 4C 8B D8 45 8B C2 0F 1F 00 49 8B 01 3A 50 20", "MagazineById (+0x20 / +0x28, key +0x20)", false},
    {kCurWeapon,    "0F B6 81 78 1C 00 00 48 6B D0 70 48 8B 84 0A D0 00 00 00 33 C9 80 78 12", "the current-weapon getter", false},
    {kHumanoidDmg,  "40 55 57 41 54 41 56 48 81 EC 68 01 00 00 C5 F8 29 B4 24 50 01 00 00 48", "Humanoid MsgDamage handler", true},
};
// clang-format on

using FnEnqueue = int (*)(std::uintptr_t sys, std::uintptr_t rec);
using FnCreate = void (*)(const double* origin, const double* target, std::uint32_t actor, float speed, std::uint32_t a5, std::uint16_t attack,
                          std::uint8_t a7, std::uint8_t p8, std::uint8_t p9, std::uint8_t p10, std::uint8_t p11, std::uint8_t p12, std::uint8_t weapon,
                          std::uint8_t mag);
using FnDamage = void (*)(std::uintptr_t, std::uintptr_t);

// ---------------------------------------------------------------- config / state
Config g_cfg;
bool g_checks_ok = false, g_npc_checks_ok = false;
unsigned g_checks_n = 0, g_checks_pass = 0;
bool g_hooked = false, g_npc_hooked = false, g_filter_registered = false;
std::atomic<bool> g_on{false};        // capture, send, replay (shots 1 / 2 live)
std::atomic<bool> g_stopped{false};   // a fault: off for the session
char g_off[200] = "shots_sync=0";
FnEnqueue o_enqueue = nullptr;
FnDamage o_humanoid = nullptr;
std::atomic<std::uintptr_t> g_sam{0};    // our Sam (the tick)
std::atomic<std::uintptr_t> g_dummy{0};  // the bullet system's dummy owner entity (CreateBullet's bullets; diagnostics only)
double g_now = 0;

// the Enqueue detour's capture ring (any thread; under g_cap_lock)
constexpr std::size_t kRecSize = 0xF0;
struct Cap {
    std::uint8_t rec[kRecSize];
    double ms;
};
constexpr unsigned kCapRing = 64;
SRWLOCK g_cap_lock = SRWLOCK_INIT;
Cap g_cap[kCapRing];
unsigned g_cap_head = 0, g_cap_count = 0;
std::atomic<unsigned> g_captured{0}, g_cap_full{0}, g_others{0}, g_det_faults{0}, g_echo_skips{0};
std::atomic<unsigned> g_diag_enq{0}, g_diag_ours{0};
// the probe's owner table
std::atomic<bool> g_own_armed{false};
struct OwnCount {
    std::uintptr_t e;
    unsigned n;
};
OwnCount g_own[16];

// our replays (game thread): what the detour patches in, and what it found
struct Patch {
    std::uint8_t r05 = 0, r58 = 0, r5A = 0, rD0 = 0, rD1 = 0;
    float r94 = 0;
    bool on = false;
};
thread_local bool t_replaying = false;
thread_local Patch t_patch;
thread_local bool t_enqueued = false;
thread_local std::uint64_t t_refs[shotw::kReplayRefs] = {};
thread_local unsigned t_nrefs = 0;
thread_local std::uint8_t t_slot_rec[kRecSize];
thread_local bool t_have_slot = false;

// the replay set (the damage detours read it on their threads)
SRWLOCK g_rs_lock = SRWLOCK_INIT;
shotw::ReplaySet g_rs;

// damage verdicts (worker threads)
std::atomic<unsigned> g_drop_sam_ref{0}, g_seen_sam_dummy{0}, g_drop_npc{0}, g_kept_ff{0}, g_kept_test{0};
std::atomic<unsigned> g_dmg_logged{0};
// shots_test (game thread sets, the filter reads)
std::atomic<bool> g_test_armed{false}, g_test_nofilter{false};
std::atomic<int> g_test_ref_match{0}, g_test_dummy_match{0}, g_test_how{-1}, g_test_slot{-1};
std::atomic<int> g_test_dropped{0};
double g_test_t0 = 0;
bool g_test_wait = false;

// counters (game thread)
unsigned g_sent = 0, g_sent_pulls = 0, g_sent_frames = 0, g_cap_dropped = 0, g_no_peer = 0;
unsigned g_replayed = 0, g_rep_muzzle = 0, g_rep_mag = 0, g_rep_late = 0, g_rep_now = 0, g_rep_no_enqueue = 0, g_rep_faults = 0;
unsigned g_ref_weapon = 0, g_ref_mag = 0, g_ref_attack = 0, g_ref_origin = 0, g_ref_target = 0, g_rx_bad = 0, g_rx_dup = 0, g_rx = 0, g_hold_full = 0;
unsigned g_ref_filter = 0, g_ref_category = 0, g_ref_speed = 0, g_ref_set_full = 0;
unsigned g_weapon_hist_id[8] = {}, g_weapon_hist_n[8] = {}, g_mag_hist_id[8] = {}, g_mag_hist_n[8] = {};
unsigned g_logged_sent = 0, g_logged_rep = 0, g_logged_drop = 0;
double g_next_log_ms = 0;
bool g_reg_logged = false;

// the shooter side
std::uint32_t g_next_shot_id = 0, g_seq = 0;
shotw::PullGroup g_pull;
shotw::RateCap g_cap_rate;
shotw::Shot g_last_sent[shotw::kMaxShots];
unsigned g_last_sent_n = 0;
shotw::Shot g_last_shot;  // the newest captured shot (shots_test's ids)
bool g_have_last = false;
std::uint8_t g_cur_weapon = 0, g_cur_mag = 0;

// the beacon
std::uint8_t g_peer_parts = 0;
bool g_have_peer = false;
double g_peer_ms = -1e18, g_beacon_next = 0, g_beacon_fast_until = 0;
std::uint32_t g_gen = 0xFFFFFFFFu;
std::uint8_t g_last_logged_parts = 0xFF, g_last_logged_peer = 0xFF;

// the receiver side
shotw::Dedupe g_dedupe;
struct Held {
    shotw::Shot s;
    double due = 0;      // the sender's unwrapped ms (the pose play time's clock)
    double rx = 0;       // our clock
    bool echo = false;
    bool test = false;
};
constexpr unsigned kHoldMax = 64;
Held g_hold[kHoldMax];
unsigned g_nhold = 0;
bool g_echo = false;
double g_echo_off = 2.5;
// the partner body's place (the tick; shots 3 stands our Sam beside it)
bool g_have_body_xf = false;
WorldTransform g_body_xf{};
// the partner body's chest (spine joint 4 of the 77-joint layout; the root + 1.2 m when unreadable): shots 4's steered aim point
double g_body_chest[3] = {};
// shots 4 (hidden runs): the game camera turned toward the partner body (a scenario pulses the right stick until "aimed").
// shots 4 <deg> 1: steered (the round 15 prove, run 20261123-221121: the pulsed yaw left the camera's pitch where the teleport
// put it, and a 120 ms pulse still turned the camera on after "aimed": the guest's 7 shots went past the host's left foot into
// the ground). The mod holds LT (the aim stance) and drives the virtual right stick itself every tick until the camera's ray
// (its position and 3D forward) points at the body's chest in yaw AND pitch within deg, the stick released and the aim still
// within deg 250 ms later. Each axis adapts (run 20261124-081004: in the aim stance the camera stalled at 11 deg yaw / 6 deg
// pitch with the stick at 13000-15000 / 9000): an axis driven for 400 ms that gained less than 0.5 deg gets 4000 more stick, an
// overshoot (the error's sign turned) halves that again, and an error that grew by 3 deg on the same side turns its sign.
struct AimAxis {
    int sign = 1;          // rx > 0 turns the camera right (yaw error up), ry > 0 looks up: flipped if the error grows
    double boost = 0;      // extra stick over the 9000 floor
    double win_e = 1e9;    // the window's signed error at its start
    bool drove = false;
    std::int16_t Stick(double err_deg, double tol) const noexcept {
        const double a = std::fabs(err_deg);
        if (a <= tol * 0.5) return 0;
        const double lo = 9000.0 + boost;
        double m = a * 1200.0;
        m = m < lo ? lo : m;
        m = m > 30000.0 ? 30000.0 : m;
        return static_cast<std::int16_t>(err_deg < 0 ? -m : m);
    }
    // the window's end: returns 1 when the sign was flipped, 2 when the boost grew, 3 when it was halved, else 0
    int Window(double e) noexcept {
        int r = 0;
        if (drove && win_e < 1e8) {
            const bool same = (e < 0) == (win_e < 0);
            if (same && std::fabs(e) > std::fabs(win_e) + 3) sign = -sign, r = 1;
            else if (same && std::fabs(win_e) - std::fabs(e) < 0.5) boost = boost + 4000 > 21000 ? 21000 : boost + 4000, r = 2;
            else if (!same && boost > 0) boost *= 0.5, r = 3;
        }
        win_e = e;
        drove = false;
        return r;
    }
};
struct Aim {
    bool on = false;
    double tol = 12;
    unsigned n = 0;
    double next_log = 0;
    bool steer = false;
    double start = 0, settle_until = 0;
    double win_t = 0;
    AimAxis yaw, pitch;
} g_aim;
void AimPad(std::int16_t rx, std::int16_t ry) noexcept {
    overlay::input::PadState p;
    p.connected = true;
    p.lt = 255;  // the aim stance: the camera that aims the shot
    p.rx = rx, p.ry = ry;
    overlay::input::SetVirtualPad(p);
}
void AimSteerTick(game::Entity* local) {
    double cp[3] = {}, cf[3] = {};
    if (!nametags::CameraView(local, cp, cf)) return;
    const double d[3] = {g_body_chest[0] - cp[0], g_body_chest[1] - cp[1], g_body_chest[2] - cp[2]};
    const double h = std::sqrt(d[0] * d[0] + d[1] * d[1]);
    double ey = (std::atan2(d[1], d[0]) - std::atan2(cf[1], cf[0])) * 57.29577951308232;
    while (ey > 180) ey -= 360;
    while (ey < -180) ey += 360;
    const double ep = (std::atan2(d[2], h) - std::asin(cf[2] < -1 ? -1 : cf[2] > 1 ? 1 : cf[2])) * 57.29577951308232;
    const bool in = std::fabs(ey) <= g_aim.tol && std::fabs(ep) <= g_aim.tol;
    if (in) {
        if (g_aim.settle_until == 0) {
            g_aim.settle_until = g_now + 250;
            AimPad(0, 0);
            return;
        }
        if (g_now < g_aim.settle_until) return;
        g_aim.on = false;
        AimPad(0, 0);  // LT stays held: the scenario's next pad step fires from this aim
        Log("[shots] camera aimed #%u at the partner body (steered: yaw %.1f, pitch %.1f deg off, %.1f m; stick signs %d %d, boosts %.0f %.0f)", ++g_aim.n,
            ey, ep, std::sqrt(h * h + d[2] * d[2]), g_aim.yaw.sign, g_aim.pitch.sign, g_aim.yaw.boost, g_aim.pitch.boost);
        return;
    }
    g_aim.settle_until = 0;
    if (g_now - g_aim.start > 30000) {
        g_aim.on = false;
        overlay::input::SetVirtualPad(overlay::input::PadState{true});
        Log("[shots] camera aim: gave up after 30 s (yaw %.1f, pitch %.1f deg off)", ey, ep);
        return;
    }
    if (g_now - g_aim.win_t >= 400) {
        g_aim.win_t = g_now;
        static const char* const kWhat[4] = {"", "stick sign flipped", "more stick", "less stick (overshoot)"};
        const int ry_ = g_aim.yaw.Window(ey), rp_ = g_aim.pitch.Window(ep);
        if (ry_ == 1 || rp_ == 1 || ((ry_ | rp_) != 0 && g_now >= g_aim.next_log))
            Log("[shots] camera aim: yaw %s, pitch %s (boosts %.0f %.0f)", ry_ ? kWhat[ry_] : "as is", rp_ ? kWhat[rp_] : "as is", g_aim.yaw.boost,
                g_aim.pitch.boost);
    }
    const std::int16_t rx = static_cast<std::int16_t>(-g_aim.yaw.sign * g_aim.yaw.Stick(ey, g_aim.tol));
    const std::int16_t ry = static_cast<std::int16_t>(g_aim.pitch.sign * g_aim.pitch.Stick(ep, g_aim.tol));
    g_aim.yaw.drove = g_aim.yaw.drove || rx != 0;
    g_aim.pitch.drove = g_aim.pitch.drove || ry != 0;
    AimPad(rx, ry);
    if (g_now >= g_aim.next_log) g_aim.next_log = g_now + 1000, Log("[shots] camera aim: yaw %.1f, pitch %.1f deg off (steering, stick %d %d)", ey, ep, rx, ry);
}
void AimTick(game::Entity* local) {
    if (!g_aim.on || !local || !g_have_body_xf) return;
    if (g_aim.steer) return AimSteerTick(local);
    WorldTransform s{};
    double cf[2] = {};
    if (!game::TryGetTransform(local, &s) || !nametags::CameraForward(local, cf)) return;
    double diff = (std::atan2(g_body_xf.pos.y - s.pos.y, g_body_xf.pos.x - s.pos.x) - std::atan2(cf[1], cf[0])) * 57.29577951308232;
    while (diff > 180) diff -= 360;
    while (diff < -180) diff += 360;
    if (std::fabs(diff) > g_aim.tol) {
        if (g_now >= g_aim.next_log) g_aim.next_log = g_now + 1000, Log("[shots] camera aim: %.1f deg off", diff);
        return;
    }
    g_aim.on = false;
    Log("[shots] camera aimed #%u at the partner body (%.1f deg off)", ++g_aim.n, diff);
}
// the leak test
int g_leak_left = 0, g_leak_done = 0;
SIZE_T g_leak_ws0 = 0;

// ---------------------------------------------------------------- reads
std::uintptr_t Reg() noexcept { return mem::Q(A(kRegGlobal)); }
bool ReadXf(std::uintptr_t e, WorldTransform& out) noexcept {
    std::uint8_t b[0x3C];
    if (!e || !mem::Read(e + 0xC8, b, sizeof(b))) return false;
    std::memcpy(&out.pos, b, 0x18);
    std::memcpy(&out.rot, b + 0x18, 0x24);
    return std::isfinite(out.pos.x) && std::isfinite(out.pos.y) && std::isfinite(out.pos.z) && std::fabs(out.pos.x) < 1e7 && std::fabs(out.pos.y) < 1e7;
}
double Dist3(const double (&a)[3], const double (&b)[3]) noexcept {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// A registry list (count at +cnt, list at +lst) holds an entry keyed `id` (u8 at entry + 0x20): the entry, else 0.
std::uintptr_t RegFind(std::uintptr_t cnt, std::uintptr_t lst, std::uint8_t id) noexcept {
    const std::uintptr_t reg = Reg();
    std::int32_t n = 0;
    if (!id || !reg || !mem::Rd(reg + cnt, n) || n <= 0 || n > 4096) return 0;
    const std::uintptr_t list = mem::Q(reg + lst);
    if (!list) return 0;
    for (std::int32_t i = 0; i < n; ++i) {
        const std::uintptr_t e = mem::Q(list + 8ull * static_cast<unsigned>(i));
        std::uint8_t k = 0;
        if (e && mem::Rd(e + 0x20, k) && k == id) return e;
    }
    return 0;
}
shotw::IdCheck WeaponCheck(std::uint8_t id) noexcept {
    shotw::IdCheck c;
    const std::uintptr_t w = RegFind(0x10, 0x18, id);
    c.listed = w != 0;
    if (w) {
        const std::uintptr_t ch = mem::Q(w + 0x80);
        std::int32_t n = -1;
        c.child_ok = ch && mem::Rd(ch + 0x90, n) && n >= 0 && n <= 64 && (n == 0 || mem::Q(ch + 0x98) != 0);
    }
    return c;
}
shotw::IdCheck MagCheck(std::uint8_t id) noexcept {
    shotw::IdCheck c;
    c.listed = RegFind(0x20, 0x28, id) != 0;
    return c;
}

// Our Sam's current weapon (the inventory's, as the gear watch reads it): the registry keys CreateBullet takes.
void ReadCurrentWeapon(std::uintptr_t p, std::uint8_t* weapon, std::uint8_t* mag) noexcept {
    *weapon = 0, *mag = 0;
    if (!p || mem::Q(p) != mem::Base() + kPlayerVtable) return;
    const std::uintptr_t inv = mem::Q(p + kPInv);
    if (!inv || mem::Q(inv + kCompOwner) != p) return;
    std::uint8_t slot = 0xFF;
    if (!mem::Rd(inv + 0x1C78, slot) || slot >= 64) return;
    const std::uintptr_t inst = mem::Q(inv + 0xD0 + 0x70ull * slot);
    std::uint8_t id = 0x5D;
    if (!inst || !mem::Rd(inst + 0x12, id) || id >= 0x5D) return;
    // the registry key: the weapon parameter's Id (+0x20), the same as the instance's +0x12
    const std::uintptr_t prm = mem::Q(inst + 0x18);
    std::uint8_t key = 0;
    *weapon = prm && mem::Rd(prm + 0x20, key) && key ? key : id;
    std::uint8_t mi = 0xFF;
    if (mem::Rd(inst + 0x2F8, mi) && mi < 8) {
        const std::uintptr_t mp = mem::Q(inst + 0x40ull * mi + 0x38);
        std::uint8_t mk = 0;
        if (mp && mem::Rd(mp + 0x20, mk)) *mag = mk;
    }
}

// ---------------------------------------------------------------- the Enqueue detour (any thread)
void CountOwner(std::uintptr_t owner) noexcept {
    AcquireSRWLockExclusive(&g_cap_lock);
    int free_i = -1;
    bool done = false;
    for (int i = 0; i < 16; ++i) {
        if (g_own[i].e == owner) {
            ++g_own[i].n;
            done = true;
            break;
        }
        if (!g_own[i].e && free_i < 0) free_i = i;
    }
    if (!done && free_i >= 0) g_own[free_i] = OwnCount{owner, 1};
    ReleaseSRWLockExclusive(&g_cap_lock);
}

int H_Enqueue(std::uintptr_t sys, std::uintptr_t rec) {
    if (t_replaying) {
        // our replay (CreateBullet built the record on its stack): make it the original field for field but its owner, then
        // keep the attack references the bullet carries (the echo guard: never captured)
        g_echo_skips.fetch_add(1, std::memory_order_relaxed);
        t_nrefs = 0;
        if (t_patch.on) {
            auto* r = reinterpret_cast<volatile std::uint8_t*>(rec);
            r[0x05] = t_patch.r05, r[0x58] = t_patch.r58, r[0x5A] = t_patch.r5A, r[0xD0] = t_patch.rD0, r[0xD1] = t_patch.rD1;
            *reinterpret_cast<volatile float*>(rec + 0x94) = t_patch.r94;
        }
        const std::uint64_t pre = mem::Q(rec + 0x38), pre18 = mem::Q(rec + 0x38 + 0x18);
        const int serial = o_enqueue(sys, rec);
        t_enqueued = serial != 0xFFFF;
        t_have_slot = false;
        if (t_enqueued) {
            for (unsigned i = 0; i < 16; ++i) {
                const std::uintptr_t slot = sys + 0xF0ull * i;
                std::int32_t sn = -1;
                if (mem::Rd(slot + 0xEC, sn) && sn == serial) {
                    const std::uint64_t h = mem::Q(slot + 0x48), h18 = mem::Q(slot + 0x48 + 0x18);
                    if (h) t_refs[t_nrefs++] = h;
                    if (h18 && t_nrefs < shotw::kReplayRefs) t_refs[t_nrefs++] = h18;
                    t_have_slot = mem::Read(slot + 0x10, t_slot_rec, kRecSize);
                    break;
                }
            }
        }
        if (pre && t_nrefs < shotw::kReplayRefs) t_refs[t_nrefs++] = pre;
        if (pre18 && t_nrefs < shotw::kReplayRefs) t_refs[t_nrefs++] = pre18;
        return serial;
    }
    const std::uintptr_t oref = mem::Q(rec + 0x20);
    const std::uintptr_t owner = oref ? oref - 0x20 : 0;
    const std::uintptr_t sam = g_sam.load(std::memory_order_relaxed);
    if (g_own_armed.load(std::memory_order_relaxed) && owner) CountOwner(owner);
    // ours: the record's owner is our Sam (the fire path writes [weapon instance + 0x300] + 0x20), or an object whose +0x300 is
    // our Sam (a weapon instance), or a weapon entity whose instance's (+0x338) +0x300 is
    int how = 0;
    if (owner && sam) {
        if (owner == sam) how = 1;
        else if (mem::Q(owner + 0x300) == sam) how = 2;
        else if (const std::uintptr_t inst = mem::Q(owner + 0x338); inst && mem::Q(inst + 0x300) == sam) how = 3;
    }
    if (g_diag_enq.fetch_add(1, std::memory_order_relaxed) < 3 || (how && g_diag_ours.fetch_add(1, std::memory_order_relaxed) < 2)) {
        const char* rn = owner ? mem::RttiName(mem::RttiOf(owner)) : nullptr;
        std::uint16_t atk = 0;
        mem::Rd(rec + 0x5C, atk);
        Log("[shots] enqueue: owner %p (%s) our Sam %p => %s (%d); attack 0x%X; from ds.exe+0x%llX", reinterpret_cast<void*>(owner), rn ? rn : "?",
            reinterpret_cast<void*>(sam), how ? "ours" : "not ours", how, atk,
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - mem::Base()));
    }
    if (how && g_on.load(std::memory_order_relaxed)) {
        Cap c;
        if (mem::Read(rec, c.rec, kRecSize)) {
            c.ms = link::NowMsPrecise();
            AcquireSRWLockExclusive(&g_cap_lock);
            if (g_cap_count < kCapRing) {
                g_cap[(g_cap_head + g_cap_count) % kCapRing] = c;
                ++g_cap_count;
                g_captured.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_cap_full.fetch_add(1, std::memory_order_relaxed);
            }
            ReleaseSRWLockExclusive(&g_cap_lock);
        } else {
            g_det_faults.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        g_others.fetch_add(1, std::memory_order_relaxed);
    }
    return o_enqueue(sys, rec);
}

// ---------------------------------------------------------------- the damage filters (worker threads)
// A hit of one of our replays: its attack reference (or context) is in the replay set (exact, per bullet). The bullet system's
// dummy owner (every CreateBullet bullet's, the game's own scripted ones too) is never a reason to drop: while shots_test waits,
// a hit by it is only noted (DummyNote) for the test line.
bool IsReplayHit(const samdmg::View& v, std::uint8_t* kind, int* how, int* slot) noexcept {
    const double now = link::NowMsPrecise();
    AcquireSRWLockShared(&g_rs_lock);
    const bool found = g_rs.Find(v.ref, now, kind, how, slot) || g_rs.Find(v.ctx, now, kind, how, slot);
    ReleaseSRWLockShared(&g_rs_lock);
    return found;
}
void DummyNote(const samdmg::View& v) noexcept {
    const std::uintptr_t dummy = g_dummy.load(std::memory_order_relaxed);
    if (!dummy || v.attacker != dummy || !g_test_armed.load(std::memory_order_relaxed)) return;
    g_test_dummy_match.store(1);
    g_seen_sam_dummy.fetch_add(1, std::memory_order_relaxed);
}

void LogDamage(const char* who, const samdmg::View& v, int how, int slot, const char* verdict) {
    if (g_dmg_logged.fetch_add(1, std::memory_order_relaxed) >= 12) return;
    std::uintptr_t w[16] = {};
    mem::Read(v.di, w, sizeof(w));
    Log("[shots] replay hit on %s: %s (ref %p ctx %p attacker %p dummy %p; %s%s how %d slot %d) amount %.1f; DamageInfo +0..+0x78: %llX %llX %llX %llX %llX %llX "
        "%llX %llX %llX %llX %llX %llX %llX %llX %llX %llX",
        who, verdict, reinterpret_cast<void*>(v.ref), reinterpret_cast<void*>(v.ctx), reinterpret_cast<void*>(v.attacker),
        reinterpret_cast<void*>(g_dummy.load()), "replay-set match", "", how, slot,
        static_cast<double>(v.amount), static_cast<unsigned long long>(w[0]), static_cast<unsigned long long>(w[1]), static_cast<unsigned long long>(w[2]),
        static_cast<unsigned long long>(w[3]), static_cast<unsigned long long>(w[4]), static_cast<unsigned long long>(w[5]), static_cast<unsigned long long>(w[6]),
        static_cast<unsigned long long>(w[7]), static_cast<unsigned long long>(w[8]), static_cast<unsigned long long>(w[9]), static_cast<unsigned long long>(w[10]),
        static_cast<unsigned long long>(w[11]), static_cast<unsigned long long>(w[12]), static_cast<unsigned long long>(w[13]), static_cast<unsigned long long>(w[14]),
        static_cast<unsigned long long>(w[15]));
}

// Still matched after a fault-stop (g_stopped): replays already in flight keep being dropped until their entries expire.
samdmg::Verdict SamFilter(const samdmg::View& v) {
    std::uint8_t kind = 0;
    int how = -1, slot = -1;
    if (!IsReplayHit(v, &kind, &how, &slot)) {
        DummyNote(v);
        return samdmg::Verdict::keep;
    }
    if (g_test_armed.load(std::memory_order_relaxed)) g_test_ref_match.store(1), g_test_how.store(how), g_test_slot.store(slot);
    if (kind == 3 && g_test_nofilter.load(std::memory_order_relaxed)) {
        g_kept_test.fetch_add(1, std::memory_order_relaxed);
        LogDamage("our Sam", v, how, slot, "kept (test: no filter)");
        return samdmg::Verdict::keep;
    }
    if (g_cfg.shots_friendly_fire && kind != 3) {
        g_kept_ff.fetch_add(1, std::memory_order_relaxed);
        LogDamage("our Sam", v, how, slot, "kept (shots_friendly_fire=1)");
        return samdmg::Verdict::keep;
    }
    g_drop_sam_ref.fetch_add(1, std::memory_order_relaxed);
    if (kind == 3) g_test_dropped.store(1);
    LogDamage("our Sam", v, how, slot, "dropped");
    return samdmg::Verdict::drop;
}

bool NpcMatch(std::uintptr_t e, std::uintptr_t msg) noexcept {
    __try {
        const samdmg::View v = samdmg::Read(e, msg);
        std::uint8_t kind = 0;
        int how = -1, slot = -1;
        if (!IsReplayHit(v, &kind, &how, &slot)) return false;
        if (kind == 3 && g_test_nofilter.load(std::memory_order_relaxed)) return false;
        LogDamage("an NPC", v, how, slot, "dropped");
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void H_Humanoid(std::uintptr_t e, std::uintptr_t msg) {
    if (NpcMatch(e, msg)) {  // also after a fault-stop: replays in flight stay dropped
        g_drop_npc.fetch_add(1, std::memory_order_relaxed);
        *reinterpret_cast<volatile std::uint8_t*>(msg + 8) = 1;  // as the handler does with a message it refuses
        return;
    }
    o_humanoid(e, msg);
}

// ---------------------------------------------------------------- replay (game thread)
struct Replay {
    double origin[3] = {}, target[3] = {};
    std::uint32_t actor = 0xFFFFFFFFu;
    float speed = 0;
    std::uint32_t a5 = 0;
    std::uint16_t attack = 0;
    std::uint8_t a7 = 0, weapon = 0, mag = 0;
    shotw::D0Args d0;
    Patch patch;
    std::uint8_t kind = 1;
};

int CallCreate(const Replay& r) noexcept {
    t_replaying = true;
    t_patch = r.patch;
    t_enqueued = false;
    t_nrefs = 0;
    __try {
        reinterpret_cast<FnCreate>(A(kCreateBullet))(r.origin, r.target, r.actor, r.speed, r.a5, r.attack, r.a7, r.d0.p8, r.d0.p9, r.d0.p10, r.d0.p11, r.d0.p12,
                                                     r.weapon, r.mag);
        t_replaying = false;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        t_replaying = false;
        return -1;
    }
}

void Stop(const char* why) {
    g_stopped.store(true);
    g_on.store(false);
    std::snprintf(g_off, sizeof(g_off), "stopped (%s)", why);
    Log("[shots] FAULT in %s: shots stopped for this session", why);
}

bool CategoryFind(std::uint8_t cat, std::uint16_t* at38, std::uint16_t* at3A) noexcept;  // below (the shooter side)

// Whether this game may replay at all: the Sam damage filter registered, and the NPC drop hooked unless shots_npc_drop=0.
bool CanReplay() noexcept { return g_filter_registered && (g_npc_hooked || !g_cfg.shots_npc_drop); }
const char* CannotReplayWhy() noexcept {
    return !g_filter_registered ? "the Sam damage filter is off" : !CanReplay() ? "the NPC drop is not installed (shots_npc_drop=1)" : "";
}

// One replay of `s` from `origin`. Returns true when the game enqueued it. Every value the game reads under the bullet system's
// lock is checked first: a fault there cannot be recovered (the SEH below is a last resort; the lock would stay held).
bool ReplayShot(const shotw::Shot& s, const double (&origin)[3], std::uint8_t kind, bool muzzle_wanted) {
    if (!CanReplay()) {
        ++g_ref_filter;
        return false;
    }
    if (!shotw::AttackReplayable(s.attack_id)) {
        ++g_ref_attack;
        return false;
    }
    if (!(s.speed >= shotw::kMinReplaySpeed)) {
        ++g_ref_speed;
        return false;
    }
    const std::uint8_t d1 = shotw::MaskD1(s.recD1);
    {
        std::uint16_t a38 = 0, a3A = 0;
        const bool listed = s.rec5A && CategoryFind(s.rec5A, &a38, &a3A);
        if (!shotw::CategoryOk(s.rec5A, d1, listed)) {
            ++g_ref_category;
            return false;
        }
    }
    {
        AcquireSRWLockShared(&g_rs_lock);
        const bool room = g_rs.HasRoom(link::NowMsPrecise());
        ReleaseSRWLockShared(&g_rs_lock);
        if (!room) {
            ++g_ref_set_full;
            return false;
        }
    }
    Replay r;
    std::memcpy(r.origin, origin, sizeof(r.origin));
    for (int k = 0; k < 3; ++k) r.target[k] = origin[k] + static_cast<double>(s.dir[k]) * 150.0;
    r.actor = 0xFFFFFFFFu;  // ship A: never a target actor (an absolute point along the shot)
    r.speed = s.speed;
    r.a5 = s.rec84;
    r.attack = s.attack_id;
    r.a7 = s.rec04;
    r.d0 = shotw::UnpackD0(s.recD0);
    const bool muzzle = muzzle_wanted && (s.flags & shotw::kMuzzle);
    std::uint8_t w = muzzle && g_cfg.shots_muzzle ? s.weapon_id : 0;
    std::uint8_t m = muzzle && g_cfg.shots_sound ? s.magazine_id : 0;
    if (w && !shotw::ValidId(w, WeaponCheck(w))) {
        ++g_ref_weapon;
        w = 0;
    }
    if (m && !shotw::ValidId(m, MagCheck(m))) {
        ++g_ref_mag;
        m = 0;
    }
    r.weapon = w, r.mag = m;
    r.patch.on = true;
    r.patch.r05 = s.rec05, r.patch.r58 = s.rec58, r.patch.r5A = s.rec5A, r.patch.rD0 = s.recD0, r.patch.rD1 = d1, r.patch.r94 = s.rec94;
    r.kind = kind;
    const int res = CallCreate(r);
    if (res < 0) {
        ++g_rep_faults;
        Stop("the CreateBullet replay");
        return false;
    }
    if (!t_enqueued) {
        ++g_rep_no_enqueue;
        return false;
    }
    AcquireSRWLockExclusive(&g_rs_lock);
    g_rs.Add(t_refs, t_nrefs, link::NowMsPrecise(), kind, shotw::ReplayTtlMs(s.speed));
    ReleaseSRWLockExclusive(&g_rs_lock);
    ++g_replayed;
    if (w) ++g_rep_muzzle;
    if (m) ++g_rep_mag;
    return true;
}

// ---------------------------------------------------------------- the shooter side (game thread)
void Hist(unsigned (&ids)[8], unsigned (&ns)[8], unsigned id) {
    for (int i = 0; i < 8; ++i) {
        if (ns[i] && ids[i] == id) {
            ++ns[i];
            return;
        }
        if (!ns[i]) {
            ids[i] = id, ns[i] = 1;
            return;
        }
    }
}

// The attack a bullet category stands for (the Enqueue's own step: registry +0x30 count, +0x38 list, key u8 +0x20; the attack
// u16 at +0x38, or +0x3A when the record's +0xD1 bit 0 is set). Sam's own records carry attack 0 and their category (run
// 20261123-204612): CreateBullet needs the attack id itself.
// The category entry (found: the registry lists `cat`, with its two attack words readable). Also the receiver's check before a
// replay (shotw::CategoryOk): the Enqueue reads list[-1] for a missing category while it holds the bullet system's lock.
bool CategoryFind(std::uint8_t cat, std::uint16_t* at38, std::uint16_t* at3A) noexcept {
    *at38 = *at3A = 0;
    const std::uintptr_t reg = Reg();
    std::int32_t n = 0;
    if (!cat || !reg || !mem::Rd(reg + 0x30, n) || n <= 0 || n > 4096) return false;
    const std::uintptr_t list = mem::Q(reg + 0x38);
    for (std::int32_t i = 0; list && i < n; ++i) {
        const std::uintptr_t e = mem::Q(list + 8ull * static_cast<unsigned>(i));
        std::uint8_t k = 0;
        if (e && mem::Rd(e + 0x20, k) && k == cat) return mem::Rd(e + 0x38, *at38) && mem::Rd(e + 0x3A, *at3A);
    }
    return false;
}

bool BuildShot(const Cap& c, std::uintptr_t sam, shotw::Shot* out) {
    shotw::Shot s;
    float o[3], v[3];
    std::memcpy(o, c.rec + 0x60, sizeof(o));
    std::memcpy(v, c.rec + 0x70, sizeof(v));
    const double sp = std::sqrt(static_cast<double>(v[0]) * v[0] + static_cast<double>(v[1]) * v[1] + static_cast<double>(v[2]) * v[2]);
    if (!(sp > 0.1) || !std::isfinite(sp)) return false;
    for (int k = 0; k < 3; ++k) s.origin[k] = o[k], s.dir[k] = static_cast<float>(v[k] / sp);
    s.speed = static_cast<float>(sp);
    std::uint16_t rec5C = 0, at38 = 0, at3A = 0;
    std::memcpy(&rec5C, c.rec + 0x5C, 2);
    const bool cat_found = !rec5C && c.rec[0x5A] && CategoryFind(c.rec[0x5A], &at38, &at3A);
    s.attack_id = shotw::RecordAttack(rec5C, c.rec[0x5A], cat_found, at38, at3A, c.rec[0xD1]);
    if (rec5C == 0 && c.rec[0x5A]) {
        static unsigned logged = 0;
        if (logged++ < 2) Log("[shots] our record: attack 0 with category %u (D1 0x%02X): the category's attack 0x%X", c.rec[0x5A], c.rec[0xD1], s.attack_id);
    }
    s.rec04 = c.rec[0x04], s.rec05 = c.rec[0x05], s.rec06 = c.rec[0x06], s.rec58 = c.rec[0x58], s.rec5A = c.rec[0x5A];
    s.recD0 = c.rec[0xD0], s.recD1 = shotw::MaskD1(c.rec[0xD1]);
    std::memcpy(&s.rec84, c.rec + 0x84, 4);
    std::memcpy(&s.rec94, c.rec + 0x94, 4);
    if (!std::isfinite(s.rec94)) s.rec94 = 0;
    s.target_actor = shotw::kNoActor;  // ship A: the target actor is never sent (no enemy identity yet)
    s.weapon_id = g_cur_weapon;
    s.magazine_id = g_cur_mag;
    if (g_pull.Next(c.ms, g_cur_weapon)) s.flags |= shotw::kMuzzle;
    const double age = g_now - c.ms;
    s.age_ms = static_cast<std::uint16_t>(age < 0 ? 0 : age > 60000 ? 60000 : age);
    // the hand anchor: the origin from our right hand, in our yaw frame
    WorldTransform xf{};
    double hand[3];
    if (sam && ReadXf(sam, xf) && realbody::JointWorld(reinterpret_cast<game::Entity*>(sam), kHandJoint, hand)) {
        const float fwd[3] = {xf.rot.m[1][0], xf.rot.m[1][1], xf.rot.m[1][2]};
        s.yaw = shotw::YawOf(fwd);
        const double d[3] = {s.origin[0] - hand[0], s.origin[1] - hand[1], s.origin[2] - hand[2]};
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 9.0) {
            shotw::ToYawFrame(d, s.yaw, s.hand_rel);
            s.flags |= shotw::kHandRel;
        }
    }
    if (!shotw::AttackReplayable(s.attack_id) || !(s.speed >= shotw::kMinReplaySpeed)) return false;
    s.shot_id = ++g_next_shot_id;
    *out = s;
    return true;
}

void SendFrames(const shotw::Shot* fresh, unsigned nfresh) {
    // every shot twice: this tick's new ones, and last tick's again (flag resent)
    shotw::Shot all[shotw::kMaxShots * 3];
    unsigned n = 0;
    for (unsigned i = 0; i < nfresh && n < shotw::kMaxShots * 3; ++i) all[n++] = fresh[i];
    for (unsigned i = 0; i < g_last_sent_n && n < shotw::kMaxShots * 3; ++i) {
        all[n] = g_last_sent[i];
        all[n].flags |= shotw::kResent;
        ++n;
    }
    g_last_sent_n = 0;
    for (unsigned i = 0; i < nfresh && i < shotw::kMaxShots; ++i) g_last_sent[g_last_sent_n++] = fresh[i];
    for (unsigned at = 0; at < n; at += shotw::kMaxShots) {
        shotw::Frame f;
        f.n = static_cast<std::uint8_t>(n - at < shotw::kMaxShots ? n - at : shotw::kMaxShots);
        f.seq = ++g_seq;
        f.sender_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(g_now) & 0xFFFFFFFFull);
        std::uint32_t c = 0;
        f.world = c;
        for (unsigned i = 0; i < f.n; ++i) f.shots[i] = all[at + i];
        std::uint8_t buf[shotw::kMaxFrameSize];
        const std::size_t size = shotw::EncodeShots(f, buf, sizeof(buf));
        if (size && link::SendCombatFrame(buf, size)) ++g_sent_frames;
    }
}

void PushHeld(const Held& h) {
    if (g_nhold >= kHoldMax) {
        // the oldest goes
        std::memmove(&g_hold[0], &g_hold[1], sizeof(Held) * (kHoldMax - 1));
        --g_nhold;
        ++g_hold_full;
    }
    g_hold[g_nhold++] = h;
}

void ShooterTick(const TickIn& in) {
    const std::uintptr_t sam = in.local_ok ? reinterpret_cast<std::uintptr_t>(in.local) : 0;
    ReadCurrentWeapon(sam, &g_cur_weapon, &g_cur_mag);
    Cap caps[16];
    unsigned nc = 0;
    AcquireSRWLockExclusive(&g_cap_lock);
    while (g_cap_count && nc < 16) {
        caps[nc++] = g_cap[g_cap_head];
        g_cap_head = (g_cap_head + 1) % kCapRing;
        --g_cap_count;
    }
    ReleaseSRWLockExclusive(&g_cap_lock);
    if (!nc) {
        if (g_last_sent_n && in.net && shotw::BothHave(shotw::kPartFire, g_peer_parts, shotw::kPartFire)) SendFrames(nullptr, 0);  // the resend
        else g_last_sent_n = 0;
        return;
    }
    shotw::Shot fresh[16];
    unsigned nf = 0;
    WorldTransform sx{};
    const bool have_sx = sam && ReadXf(sam, sx);
    for (unsigned i = 0; i < nc; ++i) {
        shotw::Shot s;
        if (!BuildShot(caps[i], sam, &s)) continue;
        if (!g_cap_rate.Take(caps[i].ms)) {
            ++g_cap_dropped;
            continue;
        }
        g_last_shot = s;
        g_have_last = true;
        ++g_sent;
        if (s.flags & shotw::kMuzzle) ++g_sent_pulls;
        Hist(g_weapon_hist_id, g_weapon_hist_n, s.weapon_id);
        Hist(g_mag_hist_id, g_mag_hist_n, s.magazine_id);
        fresh[nf++] = s;
        if (g_echo && have_sx) {
            Held h;
            h.s = s;
            for (int k = 0; k < 3; ++k) h.s.origin[k] += static_cast<double>(sx.rot.m[0][k]) * g_echo_off;  // to our right
            h.s.flags &= static_cast<std::uint8_t>(~shotw::kHandRel);
            h.echo = true;
            h.rx = g_now;
            PushHeld(h);
        }
    }
    if (in.net) {
        if (shotw::BothHave(shotw::kPartFire, g_peer_parts, shotw::kPartFire)) SendFrames(fresh, nf);
        else g_no_peer += nf;
    }
}

// ---------------------------------------------------------------- the beacon (game thread)
// The fire bit says "send me your shots": only while this game can replay them (CanReplay), so a partner never sends what we drop.
std::uint8_t OurParts() noexcept { return g_on.load() && CanReplay() ? shotw::kPartFire : 0; }

void BeaconTick(const TickIn& in) {
    if (!in.net) return;
    const std::uint32_t gen = link::Generation();
    if (gen != g_gen) {
        g_gen = gen;
        g_have_peer = false;
        g_peer_parts = 0;
        g_beacon_fast_until = g_now + 10000;
        g_beacon_next = g_now;
        g_dedupe.Reset();
        g_nhold = 0;
    }
    // the partner's
    std::uint8_t buf[64];
    std::size_t n = 0;
    std::uint32_t fgen = 0;
    while (link::PopCombatFrame(link::CombatFrame::beacon, buf, sizeof(buf), n, fgen)) {
        shotw::Beacon b;
        if (fgen != gen || !shotw::DecodeBeacon(buf, n, b)) continue;
        g_have_peer = true;
        g_peer_ms = g_now;
        g_peer_parts = b.parts;
    }
    if (g_have_peer && g_now - g_peer_ms > 20000) {
        g_have_peer = false;
        g_peer_parts = 0;
    }
    const std::uint8_t ours = OurParts();
    if (ours != g_last_logged_parts || g_peer_parts != g_last_logged_peer) {
        const bool changed_ours = ours != g_last_logged_parts;
        g_last_logged_parts = ours, g_last_logged_peer = g_peer_parts;
        const bool fire = shotw::BothHave(ours, g_peer_parts, shotw::kPartFire);
        Log("[link] combat parts: ours 0x%02X theirs 0x%02X%s: fire %s%s", ours, g_peer_parts, g_have_peer ? "" : " (no beacon yet)", fire ? "on" : "off",
            fire                                   ? ""
            : !(ours & shotw::kPartFire)           ? (g_on.load() ? " (no replays here)" : " (shots off here)")
            : g_have_peer                          ? " (the partner has shots_sync=0)"
                                                   : "");
        if (changed_ours) g_beacon_next = g_now;
    }
    if (g_now < g_beacon_next) return;
    g_beacon_next = g_now + (g_now < g_beacon_fast_until ? 1000 : 5000);
    shotw::Beacon b;
    b.parts = ours;
    b.role = in.role;
    std::uint8_t out[shotw::kBeaconSize];
    if (shotw::EncodeBeacon(b, out, sizeof(out))) link::SendCombatFrame(out, sizeof(out));
}

// ---------------------------------------------------------------- the receiver side (game thread)
void ReceiveTick(const TickIn& in) {
    std::uint8_t buf[1024];
    std::size_t n = 0;
    std::uint32_t fgen = 0;
    static shotw::Frame f;
    while (link::PopCombatFrame(link::CombatFrame::shots, buf, sizeof(buf), n, fgen)) {
        if (fgen != link::Generation()) continue;
        if (!shotw::DecodeShots(buf, n, f)) {
            ++g_rx_bad;
            continue;
        }
        if (!g_on.load()) continue;
        for (unsigned i = 0; i < f.n; ++i) {
            const shotw::Shot& s = f.shots[i];
            if (s.shooter != shotw::kShooterSam) continue;  // enemy shots: the enemy rounds
            if (!g_dedupe.Accept(s.shot_id)) {
                ++g_rx_dup;
                continue;
            }
            ++g_rx;
            Held h;
            h.s = s;
            const std::uint32_t fired = f.sender_ms - s.age_ms;
            h.due = in.have_play ? shotw::UnwrapNear(fired, in.play_ms) : 0;
            h.rx = g_now;
            PushHeld(h);
        }
    }
}

bool BodyHand(game::Entity* body, double (&hand)[3], float* yaw, double (&root)[3]) {
    WorldTransform xf{};
    if (!body || !ReadXf(reinterpret_cast<std::uintptr_t>(body), xf)) return false;
    root[0] = xf.pos.x, root[1] = xf.pos.y, root[2] = xf.pos.z;
    const float fwd[3] = {xf.rot.m[1][0], xf.rot.m[1][1], xf.rot.m[1][2]};
    *yaw = shotw::YawOf(fwd);
    return realbody::JointWorld(body, kHandJoint, hand);
}

void ReplayTick(const TickIn& in) {
    if (!g_nhold) return;
    double hand[3] = {}, root[3] = {};
    float yaw = 0;
    if (!CanReplay()) {
        // without the Sam damage filter (or the NPC drop) a replay could hurt our Sam or this world's MULEs: nothing is replayed
        g_ref_filter += g_nhold;
        g_nhold = 0;
        return;
    }
    const bool have_hand = BodyHand(in.body, hand, &yaw, root);
    unsigned done = 0;
    unsigned keep = 0;
    for (unsigned i = 0; i < g_nhold; ++i) {
        Held& h = g_hold[i];
        bool fire = false;
        if (h.echo || h.test) {
            fire = true;
        } else {
            const shotw::Gate g = shotw::PlayGate(in.body && in.have_play, in.play_ms, h.due);
            if (g == shotw::Gate::hold && g_now - h.rx < 1500) {
                g_hold[keep++] = h;
                continue;
            }
            fire = true;
            if (g == shotw::Gate::late) ++g_rep_late;
            else if (g != shotw::Gate::fire) ++g_rep_now;
        }
        if (!fire) continue;
        if (done >= 8) {  // the bullet system takes 16 a frame; leave the game its share
            g_hold[keep++] = h;
            continue;
        }
        double origin[3];
        if (h.echo) {
            std::memcpy(origin, h.s.origin, sizeof(origin));
        } else {
            // the partner's shots start at the partner body: none drawn here (another region, not spawned yet), none replayed
            if (!in.body || !g_have_body_xf) {
                ++g_ref_origin;
                continue;
            }
            shotw::AnchoredOrigin(h.s, have_hand, hand, yaw, 1.5, origin);
            // the shot must start near the partner body (a stale world, a teleport, a malformed peer: refused)
            const double b[3] = {g_body_xf.pos.x, g_body_xf.pos.y, g_body_xf.pos.z};
            if (Dist3(origin, b) > 30.0) {
                ++g_ref_origin;
                continue;
            }
        }
        ReplayShot(h.s, origin, h.test ? 3 : 1, true);
        ++done;
        if (g_stopped.load()) {
            g_nhold = 0;
            return;
        }
    }
    g_nhold = keep;
}

// ---------------------------------------------------------------- shots_test / the leak test (game thread)
bool TestOrigin(double dist, double up, double (&origin)[3], double (&target)[3], bool into_ground) {
    const std::uintptr_t sam = g_sam.load();
    WorldTransform xf{};
    if (!sam || !ReadXf(sam, xf)) return false;
    const double p[3] = {xf.pos.x, xf.pos.y, xf.pos.z};
    for (int k = 0; k < 3; ++k) origin[k] = p[k] + static_cast<double>(xf.rot.m[1][k]) * dist;
    origin[2] += up;
    if (into_ground) {
        // down into the ground further ahead, away from Sam
        for (int k = 0; k < 3; ++k) target[k] = p[k] + static_cast<double>(xf.rot.m[1][k]) * (dist + 6.0);
        target[2] -= 1.0;
    } else {
        target[0] = p[0], target[1] = p[1], target[2] = p[2] + 1.2;  // his chest
    }
    return true;
}

shotw::Shot TestShot(const double (&origin)[3], const double (&target)[3], int flags) {
    shotw::Shot s = g_last_shot;
    for (int k = 0; k < 3; ++k) s.origin[k] = origin[k];
    const double d[3] = {target[0] - origin[0], target[1] - origin[1], target[2] - origin[2]};
    const double l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int k = 0; k < 3; ++k) s.dir[k] = static_cast<float>(d[k] / (l > 1e-6 ? l : 1));
    s.flags = shotw::kMuzzle;
    if (flags & 2) s.weapon_id = 0;
    if (flags & 4) s.magazine_id = 0;
    return s;
}

SIZE_T WorkingSet() {
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) ? pmc.WorkingSetSize : 0;
}

void LeakTick() {
    if (g_leak_left <= 0) return;
    for (int i = 0; i < 4 && g_leak_left > 0; ++i) {
        double o[3], t[3];
        if (!TestOrigin(10.0, 1.5, o, t, true)) {
            g_leak_left = 0;
            break;
        }
        const shotw::Shot s = TestShot(o, t, 0);
        ReplayShot(s, o, 3, (g_leak_done % 8) == 0);
        --g_leak_left;
        ++g_leak_done;
        if (g_stopped.load()) {
            g_leak_left = 0;
            break;
        }
    }
    if (g_leak_left <= 0) {
        const SIZE_T ws1 = WorkingSet();
        Log("[shots] leak test: %d replays; working set %.1f MB -> %.1f MB (%+.1f MB)", g_leak_done, g_leak_ws0 / 1048576.0, ws1 / 1048576.0,
            (static_cast<double>(ws1) - static_cast<double>(g_leak_ws0)) / 1048576.0);
    }
}

void TestTick() {
    if (!g_test_wait || g_now - g_test_t0 < 2000) return;
    g_test_wait = false;
    const int ref = g_test_ref_match.load(), dm = g_test_dummy_match.load(), dropped = g_test_dropped.load();
    Log("[shots] test: ref match=%d (how %d, word %d), dummy owner seen=%d; %s", ref, g_test_how.load(), g_test_slot.load(), dm,
        dropped == 1                    ? "dropped by the filter (ref match)"
        : g_test_nofilter.load() && ref ? "kept (nofilter)"
        : dm                            ? "a dummy-owner hit only (never dropped)"
                                        : "no damage message of the replay within 2 s");
    g_test_armed.store(false);
    g_test_nofilter.store(false);  // a later leak test (kind 3) keeps the filters
}

float Health() noexcept {
    game::Diagnostics d;
    game::ReadDiagnostics(&d);
    return d.health_valid ? d.health : -1.0f;
}

void StatusLine(char* out, std::size_t cap) {
    unsigned seen = 0, faults = 0, dropped[4] = {};
    samdmg::Counts(&seen, dropped, &faults);
    std::snprintf(out, cap,
                  "%s%s; hooks: enqueue %s, sam damage %s, npc damage %s; captured %u (others %u, ring full %u), sent %u (pulls %u, frames %u, no peer %u, cap %u), "
                  "received %u (dup %u, bad %u), replayed %u (muzzle %u, mag %u, late %u, at once %u, not enqueued %u), refused weapon %u mag %u origin %u "
                  "filter %u category %u speed %u set full %u, faults %u; replay hits dropped: our Sam %u (dummy-owner hits seen in tests %u), NPCs %u, "
                  "kept %u; echo %s; peer parts 0x%02X; weapon %u mag %u; our health %.1f",
                  g_on.load() ? "ON" : "OFF: ", g_on.load() ? "" : g_off, g_hooked ? "on" : "off", g_filter_registered ? "on" : "off", g_npc_hooked ? "on" : "off",
                  g_captured.load(), g_others.load(), g_cap_full.load(), g_sent, g_sent_pulls, g_sent_frames, g_no_peer, g_cap_dropped, g_rx, g_rx_dup, g_rx_bad,
                  g_replayed, g_rep_muzzle, g_rep_mag, g_rep_late, g_rep_now, g_rep_no_enqueue, g_ref_weapon, g_ref_mag, g_ref_origin, g_ref_filter,
                  g_ref_category, g_ref_speed, g_ref_set_full, g_rep_faults + g_det_faults.load(), g_drop_sam_ref.load(), g_seen_sam_dummy.load(), g_drop_npc.load(), g_kept_ff.load() + g_kept_test.load(), g_echo ? "on" : "off", g_peer_parts,
                  g_cur_weapon, g_cur_mag, static_cast<double>(Health()));
}

void LogTick() {
    if (g_now < g_next_log_ms) return;
    g_next_log_ms = g_now + 5000;
    static unsigned logged_others = 0;
    if (g_others.load() != logged_others && g_captured.load() == 0) {
        logged_others = g_others.load();
        Log("[shots] enqueues seen: %u, none of them our Sam's", logged_others);
    }
    if (g_sent != g_logged_sent) {
        g_logged_sent = g_sent;
        char w[96] = {}, m[96] = {};
        std::size_t k = 0;
        for (int i = 0; i < 8 && g_weapon_hist_n[i]; ++i) k += std::snprintf(w + k, sizeof(w) - k, "%s%u:%u", i ? " " : "", g_weapon_hist_id[i], g_weapon_hist_n[i]);
        k = 0;
        for (int i = 0; i < 8 && g_mag_hist_n[i]; ++i) k += std::snprintf(m + k, sizeof(m) - k, "%s%u:%u", i ? " " : "", g_mag_hist_id[i], g_mag_hist_n[i]);
        Log("[shots] sent n=%u (pulls %u, pellets %u, frames %u; attack 0x%X speed %.0f m/s) weapon ids {%s} mag ids {%s}; dropped (cap) %u, no peer %u", g_sent, g_sent_pulls,
            g_sent - g_sent_pulls, g_sent_frames, g_have_last ? g_last_shot.attack_id : 0, g_have_last ? static_cast<double>(g_last_shot.speed) : 0.0, w, m, g_cap_dropped,
            g_no_peer);
    }
    const unsigned refused = g_ref_origin + g_ref_attack + g_ref_filter + g_ref_category + g_ref_speed + g_ref_set_full;
    if (g_replayed + refused + g_rep_no_enqueue != g_logged_rep) {
        g_logged_rep = g_replayed + refused + g_rep_no_enqueue;
        Log("[shots] replayed n=%u (muzzle %u, mag effect %u), late %u, at once %u, not enqueued %u, refused: weapon %u mag %u attack %u origin %u target %u "
            "filter %u category %u speed %u set full %u; received %u (dup %u, bad %u); faults %u",
            g_replayed, g_rep_muzzle, g_rep_mag, g_rep_late, g_rep_now, g_rep_no_enqueue, g_ref_weapon, g_ref_mag, g_ref_attack, g_ref_origin, g_ref_target,
            g_ref_filter, g_ref_category, g_ref_speed, g_ref_set_full, g_rx, g_rx_dup, g_rx_bad, g_rep_faults + g_det_faults.load());
    }
    const unsigned drops = g_drop_sam_ref.load() + g_drop_npc.load() + g_kept_ff.load() + g_kept_test.load();
    if (drops != g_logged_drop) {
        g_logged_drop = drops;
        Log("[shots] replay hits dropped: our Sam %u (replay set %u), enemies %u; kept %u (friendly fire %u, test %u); echo-guard skips %u",
            g_drop_sam_ref.load(), g_drop_sam_ref.load(), g_drop_npc.load(), g_kept_ff.load() + g_kept_test.load(), g_kept_ff.load(), g_kept_test.load(),
            g_echo_skips.load());
    }
}

void RegistryLog() {
    if (g_reg_logged) return;
    const std::uintptr_t reg = Reg();
    std::int32_t nw = 0, nm = 0;
    if (!reg || !mem::Rd(reg + 0x10, nw) || !mem::Rd(reg + 0x20, nm) || nw <= 0) return;
    g_reg_logged = true;
    const std::uintptr_t dref = mem::Q(reg + kRegDummy);
    Log("[shots] registry: %d weapons, %d magazines; the bullet system's dummy owner %p", nw, nm, reinterpret_cast<void*>(dref ? dref - 0x20 : 0));
}

void TickBody(const TickIn& in, double now) {
    g_now = now;
    g_sam.store(in.local_ok ? reinterpret_cast<std::uintptr_t>(in.local) : 0, std::memory_order_relaxed);
    {
        const std::uintptr_t reg = Reg();
        const std::uintptr_t dref = reg ? mem::Q(reg + kRegDummy) : 0;
        g_dummy.store(dref ? dref - 0x20 : 0, std::memory_order_relaxed);
    }
    RegistryLog();
    g_have_body_xf = in.body && ReadXf(reinterpret_cast<std::uintptr_t>(in.body), g_body_xf);
    if (g_aim.on && g_aim.steer && g_have_body_xf) {
        const bool chest = realbody::JointWorld(in.body, 4, g_body_chest) && g_body_chest[2] > g_body_xf.pos.z + 0.6 && g_body_chest[2] < g_body_xf.pos.z + 1.8;
        if (!chest) g_body_chest[0] = g_body_xf.pos.x, g_body_chest[1] = g_body_xf.pos.y, g_body_chest[2] = g_body_xf.pos.z + 1.2;
    }
    if (g_aim.on && in.local_ok) AimTick(in.local);
    BeaconTick(in);
    if (!g_on.load()) {
        AcquireSRWLockExclusive(&g_cap_lock);
        g_cap_count = 0;
        ReleaseSRWLockExclusive(&g_cap_lock);
        g_nhold = 0;
        return;
    }
    ShooterTick(in);
    ReceiveTick(in);
    ReplayTick(in);
    LeakTick();
    TestTick();
    LogTick();
}
bool TickGuarded(const TickIn& in, double now) noexcept {
    __try {
        TickBody(in, now);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

// ---------------------------------------------------------------- public
void Install(const Config& config) noexcept {
    g_cfg = config;
    if (!config.shots_sync) {
        std::snprintf(g_off, sizeof(g_off), "shots_sync=0");
        Log("[shots] off (shots_sync=0)");
        return;
    }
    unsigned n = 0, ok = 0, npc_bad = 0;
    const char* first_bad = nullptr;
    for (const Site& s : kSites) {
        ++n;
        if (mem::CodeMatches(A(s.va), s.sig)) {
            ++ok;
            continue;
        }
        if (s.npc) ++npc_bad;
        else if (!first_bad) first_bad = s.name;
        Log("[shots] byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
    }
    // the globals (RIP targets of the checked instructions) and the five Enqueue callers
    n += 2;
    if (mem::RipTarget(A(kRegInsn), 3, 7) == A(kRegGlobal)) ++ok;
    else if (!first_bad) first_bad = "the registry global", Log("[shots] byte check FAILED: the registry global");
    if (mem::RipTarget(A(kSysInsn), 3, 7) == A(kSysGlobal)) ++ok;
    else if (!first_bad) first_bad = "the bullet system global", Log("[shots] byte check FAILED: the bullet system global");
    for (std::uintptr_t c : kEnqueueCallers) {
        ++n;
        std::uint8_t op = 0;
        if (mem::Rd(A(c), op) && op == 0xE8 && mem::RipTarget(A(c), 1, 5) == A(kEnqueue)) ++ok;
        else {
            if (!first_bad) first_bad = "an Enqueue caller";
            Log("[shots] byte check FAILED: the Enqueue call at 0x%llX", static_cast<unsigned long long>(c));
        }
    }
    g_checks_n = n, g_checks_pass = ok;
    g_checks_ok = first_bad == nullptr;
    g_npc_checks_ok = npc_bad == 0;
    Log("[shots] byte checks: %u/%u ok", ok, n);
    if (!g_checks_ok) {
        std::snprintf(g_off, sizeof(g_off), "this game build differs (%s)", first_bad);
        Log("[shots] OFF: %s", g_off);
        return;
    }
    void* t = reinterpret_cast<void*>(A(kEnqueue));
    MH_STATUS s = MH_CreateHook(t, reinterpret_cast<void*>(&H_Enqueue), reinterpret_cast<void**>(&o_enqueue));
    if (s == MH_OK) s = MH_EnableHook(t);
    g_hooked = s == MH_OK;
    if (!g_hooked) {
        std::snprintf(g_off, sizeof(g_off), "the bullet Enqueue not hooked (%s)", MH_StatusToString(s));
        Log("[shots] OFF: %s", g_off);
        return;
    }
    // the damage filters: our Sam (the shared detour) and the NPCs (shots_npc_drop)
    g_filter_registered = samdmg::Ensure("shots") && samdmg::Register(&SamFilter, "shots (replayed bullets on our Sam)");
    if (config.shots_npc_drop && g_npc_checks_ok) {
        void* h = reinterpret_cast<void*>(A(kHumanoidDmg));
        MH_STATUS hs = MH_CreateHook(h, reinterpret_cast<void*>(&H_Humanoid), reinterpret_cast<void**>(&o_humanoid));
        if (hs == MH_OK) hs = MH_EnableHook(h);
        g_npc_hooked = hs == MH_OK;
        if (!g_npc_hooked) Log("[shots] the Humanoid damage handler not hooked (%s)", MH_StatusToString(hs));
    }
    if (config.shots_npc_drop && !g_npc_hooked) {
        // a replay could hurt this world's MULEs: replays stay off (only shots_npc_drop=0 allows them); the capture and the sends stay on
        Log("[shots] OFF for replays: the NPC drop is not installed (%s): the partner's shots are not replayed here (ours are still sent)",
            g_npc_checks_ok ? "the hook failed" : "the Humanoid handler's byte check failed");
    }
    if (!g_filter_registered) {
        // without the filter a replay could hurt our Sam: replays stay off, the capture and the sends stay on
        Log("[shots] the Sam damage filter is off (%s): the partner's shots are not replayed here (ours are still sent)", samdmg::OffReason());
    }
    const bool on = true;
    g_on.store(on);
    g_off[0] = 0;
    Log("[shots] %s: Enqueue hooked, Sam filter %s, NPC drop %s; muzzle %d, magazine effect %d, friendly fire %d", on ? "ON" : "off",
        g_filter_registered ? "on" : "OFF", g_npc_hooked ? "on" : config.shots_npc_drop ? "OFF (checks)" : "off (shots_npc_drop=0)", config.shots_muzzle ? 1 : 0,
        config.shots_sound ? 1 : 0, config.shots_friendly_fire ? 1 : 0);
}

bool Active() noexcept { return g_on.load(); }
const char* OffReason() noexcept {
    if (g_on.load() || !g_cfg.shots_sync) return "";
    return g_off;
}
bool EnqueueHooked() noexcept { return g_hooked; }

void Tick(const TickIn& in, double now) noexcept {
    if (!g_hooked || g_stopped.load()) return;
    if (!TickGuarded(in, now)) Stop("the shots tick");
}

bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    switch (op) {
        case 3: {
            // hidden runs: our Sam v[1] m (default 7) from the partner body, v[2] deg around its heading (0 in front of it, 90 to its
            // right), facing it: the stills frame the partner's gun
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            if (!g_have_body_xf) return std::snprintf(msg, cap, "no partner body"), false;
            const double d = v[1] > 0 ? v[1] : 7.0, a = v[2] * 3.14159265358979323846 / 180.0;
            const double fx = g_body_xf.rot.m[1][0], fy = g_body_xf.rot.m[1][1], rx = g_body_xf.rot.m[0][0], ry = g_body_xf.rot.m[0][1];
            const double dx = fx * std::cos(a) + rx * std::sin(a), dy = fy * std::cos(a) + ry * std::sin(a);
            const double l = std::sqrt(dx * dx + dy * dy) > 1e-6 ? std::sqrt(dx * dx + dy * dy) : 1.0;
            const double x = g_body_xf.pos.x + dx / l * d, y = g_body_xf.pos.y + dy / l * d;
            char why[160] = {};
            // the topmost ground there (the place's short probe kept a slope's z and dropped Sam through it: run 20261123-205419)
            const bool ok = game::TeleportLocalPlayerFacing(x, y, game::kSnapRayCentreZ, -dx / l, -dy / l, game::TeleportMode::snap_to_ground, why, sizeof(why));
            Log("[shots] test: our Sam %.1f m from the partner body at %.0f deg, facing it: %s", d, v[2], why);
            return std::snprintf(msg, cap, "%s", why), ok;
        }
        case 0: {
            char line[900];
            StatusLine(line, sizeof(line));
            Log("[shots] status: %s", line);
            std::snprintf(msg, cap, "%s", line);
            return true;
        }
        case 4:
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            if (!g_have_body_xf) return std::snprintf(msg, cap, "no partner body"), false;
            g_aim.tol = v[1] >= 1 && v[1] <= 45 ? v[1] : 12;
            g_aim.steer = v[2] == 1;
            g_aim.start = g_aim.win_t = g_now, g_aim.settle_until = 0;
            g_aim.yaw = AimAxis{}, g_aim.pitch = AimAxis{};
            g_aim.on = true;
            Log("[shots] camera aim at the partner body (within %.0f deg%s)", g_aim.tol,
                g_aim.steer ? "; steered: LT held, the right stick driven here, yaw and pitch at its chest" : "");
            return std::snprintf(msg, cap, "aiming"), true;
        case 1:
            if (!g_hooked || g_stopped.load()) return std::snprintf(msg, cap, "not possible: %s", g_off), false;
            g_on.store(true);
            g_off[0] = 0;
            Log("[shots] on (debug action)");
            return std::snprintf(msg, cap, "shots on"), true;
        case 2:
            g_on.store(false);
            std::snprintf(g_off, sizeof(g_off), "off (debug action)");
            Log("[shots] off (debug action)");
            return std::snprintf(msg, cap, "shots off"), true;
        default: return std::snprintf(msg, cap, "unknown op %d (0 status, 1 on, 2 off, 3 m deg: beside the partner, 4 [deg] aim the camera at it)", op), false;
    }
}

bool Test(double dist_m, int flags, int count, char* msg, std::size_t cap) noexcept {
    if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;  // flag 1 hurts our own Sam on purpose
    if (!g_on.load()) return std::snprintf(msg, cap, "shots are off: %s", g_off), false;
    if (!CanReplay()) return std::snprintf(msg, cap, "no replays here: %s", CannotReplayWhy()), false;
    if (!g_have_last) return std::snprintf(msg, cap, "no captured shot yet (fire once first: the test takes its ids)"), false;
    if (flags & 16) {
        g_leak_left = count > 0 ? (count > 400 ? 400 : count) : 200;
        g_leak_done = 0;
        g_leak_ws0 = WorkingSet();
        Log("[shots] leak test: %d replays from 10 m ahead into the ground (4 a tick); working set %.1f MB", g_leak_left, g_leak_ws0 / 1048576.0);
        return std::snprintf(msg, cap, "leak test: %d replays", g_leak_left), true;
    }
    double o[3], t[3];
    if (!TestOrigin(dist_m > 0.5 ? dist_m : 5.0, 1.2, o, t, false)) return std::snprintf(msg, cap, "no local Sam"), false;
    const shotw::Shot s = TestShot(o, t, flags);
    const shotw::IdCheck wc = WeaponCheck(s.weapon_id), mc = MagCheck(s.magazine_id);
    g_test_ref_match.store(0), g_test_dummy_match.store(0), g_test_how.store(-1), g_test_slot.store(-1), g_test_dropped.store(0);
    g_test_nofilter.store((flags & 1) != 0);
    g_test_armed.store(true);
    const bool enq = ReplayShot(s, o, 3, true);
    g_test_t0 = g_now;
    g_test_wait = true;
    std::uint8_t a = 0, b = 0, c = 0, d0 = 0, d1 = 0;
    std::uint16_t atk = 0;
    if (t_have_slot) a = t_slot_rec[0x04], b = t_slot_rec[0x05], c = t_slot_rec[0x5A], d0 = t_slot_rec[0xD0], d1 = t_slot_rec[0xD1], std::memcpy(&atk, t_slot_rec + 0x5C, 2);
    Log("[shots] test: CreateBullet from %.1f m ahead toward our Sam's chest: attack 0x%X weapon %u (%s%s) mag %u (%s), speed %.0f m/s%s: %s; R %llX %llX %llX %llX; "
        "the slot's record: +04 %u +05 %u +5A %u +5C 0x%X +D0 0x%02X +D1 0x%02X (wanted +05 %u +5A %u +D0 0x%02X)",
        dist_m, s.attack_id, s.weapon_id, wc.listed ? "found" : "NOT FOUND", wc.listed && !wc.child_ok ? ", no child list" : "", s.magazine_id,
        mc.listed ? "found" : "NOT FOUND", static_cast<double>(s.speed), (flags & 1) ? " (filter keeps it)" : "", enq ? "enqueued" : "NOT enqueued",
        static_cast<unsigned long long>(t_nrefs > 0 ? t_refs[0] : 0), static_cast<unsigned long long>(t_nrefs > 1 ? t_refs[1] : 0),
        static_cast<unsigned long long>(t_nrefs > 2 ? t_refs[2] : 0), static_cast<unsigned long long>(t_nrefs > 3 ? t_refs[3] : 0), a, b, c, atk, d0, d1, s.rec05,
        s.rec5A, s.recD0);
    return std::snprintf(msg, cap, "test replay %s (attack 0x%X, weapon %u, mag %u)", enq ? "enqueued" : "not enqueued", s.attack_id, s.weapon_id, s.magazine_id), enq;
}

bool Echo(bool on, double offset_m, char* msg, std::size_t cap) noexcept {
    if (on && !automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
    if (on && !CanReplay()) return std::snprintf(msg, cap, "no replays here: %s", CannotReplayWhy()), false;
    if (on && !g_on.load()) return std::snprintf(msg, cap, "shots are off: %s", g_off), false;
    g_echo = on;
    if (offset_m != 0) g_echo_off = offset_m;
    Log("[shots] echo %s (our own shots replayed %.1f m to the right)", on ? "on" : "off", g_echo_off);
    return std::snprintf(msg, cap, "echo %s, %.1f m", on ? "on" : "off", g_echo_off), true;
}

void ArmOwnerCounts(bool on) noexcept {
    AcquireSRWLockExclusive(&g_cap_lock);
    for (auto& o : g_own) o = OwnCount{0, 0};
    ReleaseSRWLockExclusive(&g_cap_lock);
    g_own_armed.store(on);
}

unsigned ShotsBy(std::uintptr_t entity) noexcept {
    unsigned n = 0;
    AcquireSRWLockExclusive(&g_cap_lock);
    for (const auto& o : g_own)
        if (o.e == entity) n = o.n;
    ReleaseSRWLockExclusive(&g_cap_lock);
    return n;
}

}  // namespace st::shots
