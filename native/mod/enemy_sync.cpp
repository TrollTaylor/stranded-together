// Enemy sync: P1a the mirrors and P1-0 the freeze probe (enemy_sync.hpp; docs/enemy-sync.md 8 and 7.2).
#include "enemy_sync.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <vector>

#include "automation.hpp"
#include "coop_session.hpp"
#include "enemy_rules.hpp"
#include "enemy_wire.hpp"
#include "link.hpp"
#include "npc_damage.hpp"
#include "enemy_census.hpp"
#include "enemy_combat.hpp"
#include "enemy_probe_rules.hpp"
#include "game_actions.hpp"
#include "log.hpp"
#include "partner_source.hpp"
#include "real_body.hpp"
#include "sam_damage.hpp"
#include "save_guard.hpp"
#include "shot_sync.hpp"
#include "enemy_l1_probe.hpp"
#include "enemy_target.hpp"
#include "enemy_phit.hpp"
#include "truck_v3.hpp"
#include "vehicle_share.hpp"
#include "vehicle_truck_rules.hpp"

namespace st::esync {
namespace {
namespace mem = partner::mem;
using game::WorldTransform;

constexpr std::uintptr_t kImage = 0x140000000ull;
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

// ---------------------------------------------------------------- game addresses (ds.exe, Director's Cut; r-enemy research)
constexpr std::uintptr_t kRegGlobal = 0x147BBB8C0;      // the game actor registry
constexpr std::uintptr_t kRegLookup = 0x1422FA980;      // its lookup (the group layout the walk reads)
constexpr std::uintptr_t kPostToActor = 0x1422FB870;    // PostToActor(reg, id, msg, immediate): immediate = handled at once
constexpr std::uintptr_t kOccupiedCtor = 0x1422FD130;   // MsgGameActorSequenceOccupied's constructor: vtable, +8 = 0, +0x10 = 0
constexpr std::uintptr_t kOccupiedVt = 0x143CFC0B8;
constexpr std::uintptr_t kRequiredVt = 0x143CFC0A0;     // MsgGameActorSequenceRequired (the same layout)
constexpr std::uintptr_t kRequiredHandler = 0x142AE7600;  // the MULE's: +0xEA4 bit 13 = msg +0x10
constexpr std::uintptr_t kOccupiedHandler = 0x142AC3090;  // the MULE's Occupied handler
constexpr std::uintptr_t kMuleVt = 0x143DB7078;         // DSMuleGameActor
constexpr std::uintptr_t kVtSet = 0x142ACFEB0;          // vt+0x10: the actor's transform (+0x28, +0xEB0)
constexpr std::uintptr_t kVtPlace = 0x142AD20A0;        // vt+0x40: place on the ground (not used on a realized MULE)
constexpr std::uintptr_t kVtHidden = 0x142AB1090;       // vt+0x48 SetHidden
constexpr std::uintptr_t kVtSleep = 0x1422F7FE0;        // vt+0x50 SetSleeping
constexpr std::uintptr_t kVtIsHidden = 0x1422F7D30;     // vt+0x58
constexpr std::uintptr_t kVtIsSleeping = 0x1422F7D40;   // vt+0x60
constexpr std::uintptr_t kActorTeleport = 0x1422F8D90;  // vt+0x10, then (realized) the entity teleport 0x14235A010
constexpr std::uintptr_t kEntitySetXf = 0x142361EA0;    // Entity_ExportedSetWorldTransform
constexpr std::uintptr_t kNpcWeaponUpdate = 0x142BB5C70;  // returns at once while weapon +0x268 is set
constexpr std::uintptr_t kMuleWeaponOcc = 0x142B07FB0;  // the MULE weapon component's Occupied
// round 23 B (enemy_mirror_vehicle's second test; re-vehicles 1.3): the MULE ride action FUN_142B51E70 sets rbx = actor + 0x3C0 (the
// seat) behind the +0xEA0 == 1 gate, then [rbx + 0xA60] = actor +0xE20 = the vehicle actor id (& 0x1FFFFF); the actor's
// constructor sets +0xE20 from 0x144AF1150 (0x1FFFFF: none) and +0x3C0 = -1.
constexpr std::uintptr_t kRideSeatSite = 0x142B51F62;
constexpr std::uintptr_t kRideIdSite = 0x142B52067;
constexpr std::uintptr_t kActorVehNoneSite = 0x142A9824C;
constexpr std::uintptr_t kVehIdNoneGlobal = 0x144AF1150;
constexpr std::uintptr_t kActVehId = 0xE20, kActSeat = 0x3C0;

struct Site {
    std::uintptr_t va;
    const char* sig;
    const char* name;
};
// clang-format off
constexpr Site kSites[] = {
    {kRegLookup,       "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 20 3B 15 B8 67 7F 02 8B DA 0F 84 86 00 00 00", "the actor registry lookup"},
    {kPostToActor,     "40 55 57 48 83 EC 48 48 8B 05 B2 57 C6 02 48 33 C4 48 89 44 24 38 8B FA", "PostToActor"},
    {kOccupiedCtor,    "48 85 D2 74 12 48 8D 05 7C EF 9F 01 C6 42 08 00 48 89 02 C6 42 10 00 C3", "MsgGameActorSequenceOccupied ctor (+8, +0x10)"},
    {kRequiredHandler, "44 8B 81 A4 0E 00 00 41 8B C0 0F BA E8 0D 41 0F BA F0 0D 80 7A 10 00", "the MULE's Required handler (+0xEA4 bit 13)"},
    {kOccupiedHandler, "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 48 8B EC 48 83 EC 60 48", "the MULE's Occupied handler"},
    {kVtSet,           "C5 F8 10 02 C5 F8 11 41 28 C5 FB 10 4A 10 4C 8D 42 18", "actor vt+0x10 (transform +0x28)"},
    {kVtPlace,         "48 89 5C 24 20 57 48 81 EC A0 00 00 00 48 8B 05 7C EF 48 02 48 33 C4 48", "actor vt+0x40"},
    {kVtHidden,        "48 89 5C 24 18 55 56 57 48 83 EC 40 48 8B 05 8D FF 4A 02 48 33 C4 48 89", "actor vt+0x48 SetHidden"},
    {kVtSleep,         "44 8B 81 88 00 00 00 41 8B C0 D1 E8 24 01 3A C2 74 45", "actor vt+0x50 SetSleeping"},
    {kVtIsHidden,      "0F B6 81 88 00 00 00 24 01 C3", "actor vt+0x58 IsHidden"},
    {kVtIsSleeping,    "8B 81 88 00 00 00 D1 E8 24 01 C3", "actor vt+0x60 IsSleeping"},
    {kActorTeleport,   "40 53 48 83 EC 20 48 8B 01 48 8B D9 FF 50 10 83 7B 24 02 75 3A 48 8B 43", "the actor teleport"},
    {kEntitySetXf,     "48 85 C9 74 1C 53 48 83 EC 20 48 8B D9 E8 CE E0 F7 FF 33 D2 48 8B CB E8", "Entity_ExportedSetWorldTransform"},
    {kNpcWeaponUpdate, "40 53 48 83 EC 20 80 B9 68 02 00 00 00 48 8B D9 0F 85 D6 00 00 00", "the NPC weapon update (+0x268)"},
    {kMuleWeaponOcc,   "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 E8 2B E1 0A 00 48 8B 8F", "the MULE weapon's Occupied"},
};
// clang-format on

using FnPost = void (*)(std::uintptr_t reg, std::uint32_t id, void* msg, std::uint8_t immediate);
using FnActorXf = void (*)(std::uintptr_t actor, const WorldTransform* xf);
using FnActorBool = void (*)(std::uintptr_t actor, std::uint8_t on);
using FnEntityXf = void (*)(std::uintptr_t entity, const WorldTransform* xf);

constexpr std::uintptr_t kActId = 0x08, kActInUse = 0x20, kActState = 0x24, kActEntRef = 0x68, kActFlags88 = 0x88, kMule124 = 0x124, kMuleSeq = 0xEA4;

bool g_checked = false, g_ok = false;
unsigned g_n = 0, g_pass = 0;

// the probe's MULE
struct Pick {
    std::uint32_t id = 0;
    std::uintptr_t actor = 0, entity = 0, sm = 0;
};
Pick g_pick;
std::atomic<std::uintptr_t> g_watch_entity{0};
std::atomic<unsigned> g_hits_by{0};
bool g_observing = false;
// enemy_probe 5 (hidden runs): the shield, every damage message on our Sam dropped while it is on. The camp knocked him out
// despite dev_god 2 (health and the stun still applied) in runs 20261123-204901, 20261123-215644 and 20261124-081850 (after
// the decision lever's release: `snap refused: game state: Faint`), which ended each probe early. A filter after the shots'
// own (the observer above counts the MULE's hits first, so the probe's hits are unchanged).
std::atomic<bool> g_shield{false};
std::atomic<unsigned> g_shield_drops{0};
bool g_shield_reg = false;
samdmg::Verdict ShieldFilter(const samdmg::View& /*v*/) {
    if (!g_shield.load(std::memory_order_relaxed)) return samdmg::Verdict::keep;
    g_shield_drops.fetch_add(1, std::memory_order_relaxed);
    return samdmg::Verdict::drop;
}

// the running test
struct Test {
    bool on = false;
    int lever = 0, mode = 0, path = 0;
    double secs = 8;
    double t0 = 0, t_end = 0, watch_end = 0;
    int phase = 0;  // 1 frozen and driven, 2 released and watched
    WorldTransform start{}, rel{};
    double heading[3] = {};
    std::vector<double> err;
    double self_moved = 0, moved_after = 0, turned_after = 0;
    double next_sample = 0, next_teleport = 0;
    unsigned shots0 = 0, hits0 = 0, shots = 0, hits = 0;
    bool posted_occ = false, posted_req = false, slept = false;
    std::uint32_t ea4_frozen = 0, ea4_released = 0;
    std::uint32_t anim_calls = 0;
    double anim_secs = 0;
    unsigned set_calls = 0, teleports = 0, faults = 0;
};
Test g_t;
double g_secs = 8;
int g_path = 0;
unsigned g_control_shots = 0, g_control_hits = 0;
bool g_have_control = false;
// the pose test (enemy_probe 2)
bool g_pose_on = false;
double g_pose_end = 0, g_pose_t0 = 0;
double g_now = 0;

// ---------------------------------------------------------------- reads
bool ReadXf(std::uintptr_t e, WorldTransform& out) noexcept {
    std::uint8_t b[0x3C];
    if (!e || !mem::Read(e + 0xC8, b, sizeof(b))) return false;
    std::memcpy(&out.pos, b, 0x18);
    std::memcpy(&out.rot, b + 0x18, 0x24);
    return std::isfinite(out.pos.x) && std::isfinite(out.pos.y) && std::isfinite(out.pos.z);
}
double Dist(const WorldTransform& a, const WorldTransform& b) noexcept {
    const double dx = a.pos.x - b.pos.x, dy = a.pos.y - b.pos.y, dz = a.pos.z - b.pos.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
double YawDeg(const WorldTransform& t) noexcept { return std::atan2(t.rot.m[1][1], t.rot.m[1][0]) * 180.0 / 3.14159265358979323846; }
std::uint32_t U32(std::uintptr_t a) noexcept {
    std::uint32_t v = 0;
    mem::Rd(a, v);
    return v;
}
std::uint64_t U64(std::uintptr_t a) noexcept {
    std::uint64_t v = 0;
    mem::Rd(a, v);
    return v;
}
bool StillOurs() noexcept {
    if (!g_pick.actor) return false;
    std::uint8_t in_use = 0;
    std::uint32_t id = 0;
    return mem::Q(g_pick.actor) == A(kMuleVt) && mem::Rd(g_pick.actor + kActInUse, in_use) && in_use && mem::Rd(g_pick.actor + kActId, id) && id == g_pick.id;
}
std::uint8_t WeaponFlag(std::uintptr_t entity) noexcept {
    const std::uintptr_t c = partner::FindComponentByName(reinterpret_cast<game::Entity*>(entity), "DSNpcWeaponComponent");
    std::uint8_t b = 0xFF;
    if (c) mem::Rd(c + 0x268, b);
    return b;
}

// ---------------------------------------------------------------- the calls (SEH, POD only)
bool Post(std::uint32_t id, std::uintptr_t vt, std::uint8_t value) noexcept {
    alignas(16) std::uint8_t msg[0x40] = {};
    *reinterpret_cast<std::uintptr_t*>(msg) = A(vt);
    msg[0x10] = value;
    const std::uintptr_t reg = mem::Q(A(kRegGlobal));
    if (!reg) return false;
    __try {
        reinterpret_cast<FnPost>(A(kPostToActor))(reg, id, msg, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool ActorBool(std::uintptr_t actor, std::uintptr_t fn, std::uint8_t on) noexcept {
    __try {
        reinterpret_cast<FnActorBool>(A(fn))(actor, on);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool ActorXf(std::uintptr_t actor, std::uintptr_t fn, const WorldTransform& xf) noexcept {
    __try {
        reinterpret_cast<FnActorXf>(A(fn))(actor, &xf);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool EntityXf(std::uintptr_t e, const WorldTransform& xf) noexcept {
    __try {
        reinterpret_cast<FnEntityXf>(A(kEntitySetXf))(e, &xf);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------- the observer (Sam's damage handler, worker threads)
void HitObserver(std::uintptr_t sam, std::uintptr_t msg) {
    const std::uintptr_t w = g_watch_entity.load(std::memory_order_relaxed);
    if (!w) return;
    const samdmg::View v = samdmg::Read(sam, msg);
    if (v.attacker == w) g_hits_by.fetch_add(1, std::memory_order_relaxed);
}

bool Check(char* msg, std::size_t cap) {
    if (g_checked) return g_ok;
    g_checked = true;
    unsigned n = 0, ok = 0;
    for (const Site& s : kSites) {
        ++n;
        if (mem::CodeMatches(A(s.va), s.sig)) ++ok;
        else Log("[enemy] byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
    }
    // the Occupied vtable from the checked constructor; the Required vtable in the image, its first entry in .text
    ++n;
    if (mem::RipTarget(A(kOccupiedCtor + 5), 3, 7) == A(kOccupiedVt)) ++ok;
    else Log("[enemy] byte check FAILED: the Occupied vtable");
    ++n;
    if (mem::InImage(A(kRequiredVt)) && mem::InText(mem::Q(A(kRequiredVt)))) ++ok;
    else Log("[enemy] byte check FAILED: the Required vtable");
    // DSMuleGameActor's vtable entries the probe calls
    const struct {
        std::uintptr_t off, fn;
    } kVt[] = {{0x10, kVtSet}, {0x40, kVtPlace}, {0x48, kVtHidden}, {0x50, kVtSleep}, {0x58, kVtIsHidden}, {0x60, kVtIsSleeping}};
    for (const auto& e : kVt) {
        ++n;
        if (mem::Q(A(kMuleVt) + e.off) == A(e.fn)) ++ok;
        else Log("[enemy] byte check FAILED: DSMuleGameActor vt+0x%llX", static_cast<unsigned long long>(e.off));
    }
    g_n = n, g_pass = ok;
    g_ok = ok == n;
    Log("[enemy] probe byte checks: %u/%u ok%s", ok, n, g_ok ? "" : " (the probe is off)");
    if (!g_ok) std::snprintf(msg, cap, "byte checks %u/%u", ok, n);
    if (g_ok && !g_observing) g_observing = samdmg::Observe(&HitObserver);
    return g_ok;
}

bool PickNearest(double radius, char* msg, std::size_t cap) {
    game::Entity* le = nullptr;
    WorldTransform sx{};
    if (!game::TryGetLocalPlayer(&le) || !le || !game::TryGetTransform(le, &sx)) return std::snprintf(msg, cap, "no local Sam"), false;
    const std::uintptr_t reg = mem::Q(A(kRegGlobal));
    std::int32_t ngroups = 0;
    if (!reg || !mem::Rd(reg + 0x440, ngroups) || ngroups <= 0 || ngroups > 512) return std::snprintf(msg, cap, "no actor registry"), false;
    const std::uintptr_t garr = mem::Q(reg + 0x448);
    Pick best;
    double bd = radius > 0 ? radius : 120.0;
    unsigned seen = 0, realized = 0;
    for (std::int32_t gi = 0; gi < ngroups; ++gi) {
        const std::uintptr_t g = mem::Q(garr + 8ull * static_cast<unsigned>(gi));
        std::int32_t count = 0;
        if (!g || !mem::Rd(g + 0x40, count) || count <= 0 || count > 8192) continue;
        const std::uintptr_t arr = mem::Q(g + 0x48);
        if (!arr) continue;
        for (std::int32_t s = 0; s < count; ++s) {
            const std::uintptr_t o = mem::Q(arr + 8ull * static_cast<unsigned>(s));
            std::uint8_t in_use = 0;
            std::uint32_t id = 0, state = 0;
            if (!o || !mem::Rd(o + kActInUse, in_use) || !in_use || !mem::Rd(o + kActId, id) || !enemy::IsHumanEnemyId(id)) continue;
            ++seen;
            if (mem::Q(o) != A(kMuleVt) || !mem::Rd(o + kActState, state) || state != 2) continue;
            const std::uintptr_t ref = mem::Q(o + kActEntRef);
            const std::uintptr_t e = ref ? ref - 0x20 : 0;
            std::uint64_t ef = 0;
            if (!e || !mem::Rd(e + 0x88, ef) || (ef & 0x80)) continue;  // dead
            if ((U32(o + kMule124) >> 12) & 1u) continue;                 // sequence-only
            if ((U32(o + kMuleSeq) >> 14) & 1u) continue;                 // a sequence holds it
            const std::uint32_t ms = U32(o + 0xEA0);                     // P2: the MULE state (3 / 5 dead, 4 down): a calm one only
            if (ms == 3 || ms == 4 || ms == 5) continue;
            ++realized;
            WorldTransform xf{};
            if (!ReadXf(e, xf)) continue;
            const double d = Dist(xf, sx);
            if (d < bd) bd = d, best = Pick{id, o, e, mem::Q(e + 0xB8)};
        }
    }
    if (!best.actor) return std::snprintf(msg, cap, "no realized MULE within %.0f m (%u enemies, %u realized and free)", radius, seen, realized), false;
    g_pick = best;
    g_watch_entity.store(best.entity);
    shots::ArmOwnerCounts(true);
    Log("[enemy] probe pick id=%05X dist=%.1f m ea4=0x%X a88=0x%X e88=0x%llX weapon268=%d sm=%p (%u enemies, %u realized and free within reach)", best.id, bd,
        U32(best.actor + kMuleSeq), U32(best.actor + kActFlags88), static_cast<unsigned long long>(U64(best.entity + 0x88)),
        static_cast<int>(WeaponFlag(best.entity)), reinterpret_cast<void*>(best.sm), seen, realized);
    std::snprintf(msg, cap, "picked %05X at %.1f m", best.id, bd);
    return true;
}

void Release(const char* why) {
    if (!g_pick.actor || !StillOurs()) return;
    if (g_t.posted_occ) Post(g_pick.id, kOccupiedVt, 0), g_t.posted_occ = false;
    if (g_t.posted_req || g_t.lever == 1 || g_t.lever == 2 || g_t.lever == 4) Post(g_pick.id, kRequiredVt, 0), g_t.posted_req = false;  // Occupied{0} keeps bit 13
    if (g_t.slept) ActorBool(g_pick.actor, kVtSleep, 0), g_t.slept = false;
    if (U32(g_pick.actor + kActFlags88) & 1u) ActorBool(g_pick.actor, kVtHidden, 0);
    Log("[enemy] probe release (%s): ea4=0x%X a88=0x%X", why, U32(g_pick.actor + kMuleSeq), U32(g_pick.actor + kActFlags88));
}

WorldTransform Commanded(double t) {
    WorldTransform c = g_t.start;
    if (g_t.path == 1) {
        c.pos.x += g_t.heading[0] * t, c.pos.y += g_t.heading[1] * t, c.pos.z += g_t.heading[2] * t;
    }
    return c;
}

double Pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const std::size_t i = static_cast<std::size_t>(std::floor(p * static_cast<double>(v.size() - 1) + 0.5));
    return v[i < v.size() ? i : v.size() - 1];
}

void Finish() {
    const double p50 = Pct(g_t.err, 0.5), p95 = Pct(g_t.err, 0.95), mx = g_t.err.empty() ? 0 : *std::max_element(g_t.err.begin(), g_t.err.end());
    const bool hold = g_t.mode == 0 ? false : (p95 < 0.3 && (g_t.path == 1 || mx < 0.5));
    const char* silent = "INCONCLUSIVE";
    if (g_t.lever == 0) {
        g_control_shots = g_t.shots, g_control_hits = g_t.hits, g_have_control = true;
        silent = g_t.shots || g_t.hits ? "CONTROL-ACTIVE" : "CONTROL-SILENT";
    } else if (g_t.shots || g_t.hits) {
        silent = "FAIL";
    } else if (g_have_control && (g_control_shots || g_control_hits)) {
        silent = "PASS";
    }
    const bool release = !((g_t.ea4_released >> 13) & 1u) && !((g_t.ea4_released >> 14) & 1u) && (g_t.moved_after >= 1.0 || g_t.turned_after >= 20.0);
    const double anim = g_t.anim_secs > 0 ? g_t.anim_calls / g_t.anim_secs : 0;
    Log("[enemy] probe lever=%d mode=%d path=%d done: err p50 %.2f p95 %.2f max %.2f m; self-moved %.2f m; shots %u hits %u (control: shots %u, hits %u); "
        "animate %.1f/s; set calls %u teleports %u faults %u; frozen ea4 0x%X; release: ea4 0x%X->0x%X, moved %.2f m / turned %.1f deg in 15 s => hold=%s "
        "silent=%s release=%s",
        g_t.lever, g_t.mode, g_t.path, p50, p95, mx, g_t.self_moved, g_t.shots, g_t.hits, g_control_shots, g_control_hits, anim, g_t.set_calls, g_t.teleports,
        g_t.faults, g_t.ea4_frozen, g_t.ea4_frozen, g_t.ea4_released, g_t.moved_after, g_t.turned_after, g_t.mode == 0 ? "n/a" : hold ? "PASS" : "FAIL", silent,
        release ? "PASS" : "FAIL");
    g_t.on = false;
    g_t.phase = 0;
}

bool Start(int lever, int mode, char* msg, std::size_t cap) {
    if (g_t.on) return std::snprintf(msg, cap, "a test runs"), false;
    if (!StillOurs()) return std::snprintf(msg, cap, "no picked MULE (enemy_probe 0)"), false;
    std::uint32_t state = 0;
    if (!mem::Rd(g_pick.actor + kActState, state) || state != 2) return std::snprintf(msg, cap, "the MULE left the world"), false;
    const std::uintptr_t ref = mem::Q(g_pick.actor + kActEntRef);
    g_pick.entity = ref ? ref - 0x20 : 0;
    g_pick.sm = g_pick.entity ? mem::Q(g_pick.entity + 0xB8) : 0;
    g_watch_entity.store(g_pick.entity);
    Test t;
    t.on = true;
    t.lever = lever, t.mode = mode, t.path = g_path, t.secs = g_secs;
    if (!ReadXf(g_pick.entity, t.start)) return std::snprintf(msg, cap, "unreadable"), false;
    t.heading[0] = t.start.rot.m[1][0], t.heading[1] = t.start.rot.m[1][1], t.heading[2] = 0;
    const double hl = std::sqrt(t.heading[0] * t.heading[0] + t.heading[1] * t.heading[1]);
    if (hl > 1e-6) t.heading[0] /= hl, t.heading[1] /= hl;
    t.t0 = g_now;
    t.t_end = g_now + t.secs * 1000.0;
    t.phase = 1;
    t.shots0 = shots::ShotsBy(g_pick.entity);
    t.hits0 = g_hits_by.load();
    t.next_sample = g_now + 500;
    g_t = t;
    bool ok = true;
    if (lever == 1 || lever == 2 || lever == 4) ok &= Post(g_pick.id, kOccupiedVt, 1), g_t.posted_occ = true;
    if (lever == 2) ok &= Post(g_pick.id, kRequiredVt, 1), g_t.posted_req = true;
    if (lever == 3 || lever == 4) ok &= ActorBool(g_pick.actor, kVtSleep, 1), g_t.slept = true;
    if (!ok) {
        ++g_t.faults;
        Log("[enemy] FAULT in the freeze lever %d: released", lever);
        Release("fault");
        g_t.on = false;
        return std::snprintf(msg, cap, "fault"), false;
    }
    g_t.ea4_frozen = U32(g_pick.actor + kMuleSeq);
    if (g_pick.sm) realbody::SetEnemyPoseSlot(g_pick.sm, 1);
    realbody::GetEnemySlotStats(true);
    Log("[enemy] probe lever=%d mode=%d path=%d: frozen for %.0f s (ea4 0x%X a88 0x%X weapon268 %d)", lever, mode, g_path, g_secs, g_t.ea4_frozen,
        U32(g_pick.actor + kActFlags88), static_cast<int>(WeaponFlag(g_pick.entity)));
    return std::snprintf(msg, cap, "lever %d mode %d for %.0f s", lever, mode, g_secs), true;
}

void Drive() {
    const double t = (g_now - g_t.t0) / 1000.0;
    const WorldTransform c = Commanded(t);
    bool ok = true;
    if (g_t.mode == 1 || (g_t.mode == 3 && g_now < g_t.next_teleport)) {
        ok = ActorXf(g_pick.actor, kVtSet, c) && EntityXf(g_pick.entity, c);
        ++g_t.set_calls;
    } else if (g_t.mode == 2 || g_t.mode == 3) {
        ok = ActorXf(g_pick.actor, kActorTeleport, c);
        ++g_t.teleports;
        g_t.next_teleport = g_now + 500;
    }
    if (!ok) {
        ++g_t.faults;
        Log("[enemy] FAULT in the placement (mode %d): released", g_t.mode);
        Release("fault");
        g_t.phase = 2;
        g_t.watch_end = g_now;
    }
}

// The picked MULE is gone: nothing of it stays in the pose hook (its slot cleared, the watch dropped, a running pose test ended
// without re-arming the slot) and the pick is forgotten.
void ForgetPick() {
    g_t.on = false;
    g_pose_on = false;
    g_watch_entity.store(0);
    realbody::SetEnemyPoseSlot(0, 0);
    g_pick.entity = 0, g_pick.sm = 0;
}

void TickBody(double now) {
    g_now = now;
    if (g_pose_on && now >= g_pose_end) {
        g_pose_on = false;
        const realbody::EnemySlotStats s = realbody::GetEnemySlotStats(false);
        const double secs = (now - g_pose_t0) / 1000.0;
        if (g_pick.sm) realbody::SetEnemyPoseSlot(g_pick.sm, 1);
        Log("[enemy] probe pose: animate %.1f/s, max joint error %.2f deg (writes %u, rejects %u, faults %u over %.1f s)", secs > 0 ? s.calls / secs : 0.0,
            static_cast<double>(s.max_err_deg), s.writes, s.rejects, s.faults, secs);
    }
    if (!g_t.on) return;
    if (!StillOurs()) {
        Log("[enemy] probe lever=%d mode=%d path=%d done: ABANDONED: the MULE is gone (unloaded)", g_t.lever, g_t.mode, g_t.path);
        ForgetPick();
        return;
    }
    {
        // still realized, with the same entity (a MULE taken out of the world leaves a stale entity: never written)
        std::uint32_t state = 0;
        const std::uintptr_t ref = mem::Q(g_pick.actor + kActEntRef);
        if (!mem::Rd(g_pick.actor + kActState, state) || state != 2 || !ref || ref - 0x20 != g_pick.entity) {
            Log("[enemy] probe lever=%d mode=%d path=%d done: ABANDONED: the MULE left the world (state %u, entity %p -> %p)", g_t.lever, g_t.mode, g_t.path, state,
                reinterpret_cast<void*>(g_pick.entity), reinterpret_cast<void*>(ref ? ref - 0x20 : 0));
            ForgetPick();
            return;
        }
    }
    if (g_t.phase == 1) {
        if (g_t.mode) Drive();
        if (now >= g_t.next_sample) {
            g_t.next_sample = now + 500;
            WorldTransform cur{};
            if (ReadXf(g_pick.entity, cur)) {
                const WorldTransform c = Commanded((now - g_t.t0) / 1000.0);
                g_t.err.push_back(Dist(cur, c));
                const double sm = Dist(cur, g_t.start);
                if (sm > g_t.self_moved) g_t.self_moved = sm;
            }
        }
        if (now >= g_t.t_end) {
            const realbody::EnemySlotStats s = realbody::GetEnemySlotStats(false);
            g_t.anim_calls = s.calls;
            g_t.anim_secs = (now - g_t.t0) / 1000.0;
            g_t.shots = shots::ShotsBy(g_pick.entity) - g_t.shots0;
            g_t.hits = g_hits_by.load() - g_t.hits0;
            Release("the test's end");
            g_t.ea4_released = U32(g_pick.actor + kMuleSeq);
            ReadXf(g_pick.entity, g_t.rel);
            g_t.phase = 2;
            g_t.watch_end = now + 15000;
        }
    } else if (g_t.phase == 2) {
        WorldTransform cur{};
        if (ReadXf(g_pick.entity, cur)) {
            const double d = Dist(cur, g_t.rel);
            if (d > g_t.moved_after) g_t.moved_after = d;
            double dy = std::fabs(YawDeg(cur) - YawDeg(g_t.rel));
            if (dy > 180) dy = 360 - dy;
            if (dy > g_t.turned_after) g_t.turned_after = dy;
        }
        if (now >= g_t.watch_end) Finish();
    }
}
bool TickGuarded(double now) noexcept {
    __try {
        TickBody(now);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool Probe(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
    if (!Check(msg, cap)) return false;
    switch (op) {
        case 0: return PickNearest(v[1], msg, cap);
        case 1: return Start(static_cast<int>(v[1]), static_cast<int>(v[2]), msg, cap);
        case 2: {
            if (!g_pick.sm) return std::snprintf(msg, cap, "no SkinnedModel"), false;
            if (!realbody::SetEnemyPoseSlot(g_pick.sm, 3)) return std::snprintf(msg, cap, "the pose slot refused it (no pose hook, or not the 77-joint layout)"), false;
            realbody::GetEnemySlotStats(true);
            g_pose_on = true;
            g_pose_t0 = g_now;
            g_pose_end = g_now + (v[1] > 0 ? v[1] : 4) * 1000.0;
            Log("[enemy] probe pose: our Sam's pose onto the MULE for %.0f s", v[1] > 0 ? v[1] : 4.0);
            return std::snprintf(msg, cap, "pose test"), true;
        }
        case 3:
            g_secs = v[1] > 0 ? v[1] : 8;
            g_path = static_cast<int>(v[2]);
            return std::snprintf(msg, cap, "tests: %.0f s, path %d", g_secs, g_path), true;
        case 4: {
            // our Sam v[1] m (default 10) from the picked MULE, facing it (the stills frame it)
            if (!StillOurs()) return std::snprintf(msg, cap, "no picked MULE"), false;
            WorldTransform mx{};
            game::Entity* le = nullptr;
            WorldTransform sx{};
            if (!ReadXf(g_pick.entity, mx) || !game::TryGetLocalPlayer(&le) || !le || !game::TryGetTransform(le, &sx)) return std::snprintf(msg, cap, "unreadable"), false;
            double dx = sx.pos.x - mx.pos.x, dy = sx.pos.y - mx.pos.y;
            const double l = std::sqrt(dx * dx + dy * dy);
            if (l < 0.5) dx = 1, dy = 0;
            else dx /= l, dy /= l;
            const double d = v[1] > 0 ? v[1] : 10.0;
            char why[160] = {};
            const bool ok = game::TeleportLocalPlayerFacing(mx.pos.x + dx * d, mx.pos.y + dy * d, game::kSnapRayCentreZ, -dx, -dy, game::TeleportMode::snap_to_ground, why, sizeof(why));
            Log("[enemy] probe: our Sam %.1f m from %05X, facing it: %s", d, g_pick.id, why);
            return std::snprintf(msg, cap, "%s", why), ok;
        }
        case 5: {
            // the shield (above): 1 on, 0 off
            if (!g_shield_reg) g_shield_reg = samdmg::Register(&ShieldFilter, "enemy probe shield (hidden runs: every hit on our Sam)");
            if (!g_shield_reg) return std::snprintf(msg, cap, "no shield: the Sam damage detour is not in or full (%s)", samdmg::OffReason()), false;
            g_shield.store(v[1] != 0);
            Log("[enemy] probe shield %s: every damage message on our Sam dropped while on (%u dropped so far)", v[1] != 0 ? "on" : "off", g_shield_drops.load());
            return std::snprintf(msg, cap, "shield %s (%u dropped)", v[1] != 0 ? "on" : "off", g_shield_drops.load()), true;
        }
        case 9:
            Release("enemy_probe 9");
            if (g_pick.sm) realbody::SetEnemyPoseSlot(0, 0);
            g_t.on = false;
            g_pose_on = false;
            return std::snprintf(msg, cap, "released"), true;
        default: return std::snprintf(msg, cap, "unknown op %d", op), false;
    }
}

static void ProbeTick(game::Entity* /*local*/, bool /*local_ok*/, double now) noexcept {
    if (!g_t.on && !g_pose_on) {
        g_now = now;
        return;
    }
    if (!TickGuarded(now)) {
        Log("[enemy] FAULT in the probe tick: released");
        g_t.on = false;
        g_pose_on = false;
        Release("fault");
    }
}

std::uintptr_t PickedEntity() noexcept {
    std::uint32_t state = 0;
    if (!StillOurs() || !mem::Rd(g_pick.actor + kActState, state) || state != 2) return 0;
    const std::uintptr_t ref = mem::Q(g_pick.actor + kActEntRef);
    return ref ? ref - 0x20 : 0;
}

// ====================================================================================================================================
// P1a: mirrors, the host owning every enemy (docs/enemy-sync.md 8)
// ====================================================================================================================================
namespace {

constexpr std::uintptr_t kVtCutsceneRel = 0x142DCA0F0;  // DSMuleGameActor vt+0x138: hide + sleep in a cutscene (Occupied{0}'s call)
constexpr std::uintptr_t kRePath = 0x142AE5000;         // the patrol re-path after a placement (when +0x1088), variant B only
constexpr std::uintptr_t kPlaceRealized = 0x142AD21E0;  // the realized placement (checked only: P1a never calls it)
constexpr std::uintptr_t kActXf = 0x28, kActSave = 0xD0, kActRePathFlag = 0x1089;
constexpr std::size_t kActSaveSize = 0x3C;  // +0xD0..+0x10C, restored after vt+0x40 (as 0x142B5BAE0 does)
constexpr unsigned kMaxMirrors = 64;
constexpr double kFarRadiusM = 1000.0;

using FnPlace3 = void (*)(std::uintptr_t actor, const WorldTransform* xf, std::uint8_t one);
using FnActor = void (*)(std::uintptr_t actor);

Config g_ecfg;
bool g_want = false;          // enemy_sync=1
double g_hostp[3] = {};       // the guest: the host's Sam (the partner body here) this tick (M0's quick path)
bool g_have_hostp = false;
bool g_p1_ok = false;         // byte checks passed
bool g_varb_ok = false;       // variant B's re-path checked
bool g_ride_ok = false;       // round 23 B: the ride fields' byte checks (+0xE20 / +0x3C0: enemy_mirror_vehicle's second test)
bool g_sam_filter = false, g_npc_filter = false, g_observer = false;
bool g_switch = true;         // enemy_sync 1 / 2
bool g_stopped = false;       // a fault: off for the session
int g_place_variant = 0;      // enemy_sync 6: 0 A, 1 B
int g_tp_cadence = 0;         // enemy_sync 7: 3 = a teleport every 500 ms between the sets
std::atomic<bool> g_guard{true};
erules::KeySet g_mset;        // the frozen mirrors: entity -> actor id (any thread)
erules::KeySet g_oset;        // P1b, the guest: the enemies she owns (entity -> actor id; the observer)
std::atomic<unsigned> g_owned_hits_logged{0}, g_owned_hits{0};
// Round 24 (the M2 probe P2): the owned hits on our Sam per attacker id (the observer's worker threads: a slot claimed by a CAS of
// its id; at most 16 ids a session, the rest counted in the total only)
struct HitsById {
    std::atomic<std::uint32_t> id{0};
    std::atomic<unsigned> n{0};
};
HitsById g_owned_by_id[16];
void CountOwnedHit(std::uint32_t id) noexcept {
    if (!id) return;
    for (HitsById& s : g_owned_by_id) {
        std::uint32_t cur = s.id.load(std::memory_order_relaxed);
        if (cur == 0 && s.id.compare_exchange_strong(cur, id, std::memory_order_relaxed)) cur = id;
        if (cur == id) {
            s.n.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}
std::atomic<bool> g_any_frozen{false};
double g_tnow = 0;
game::Entity* g_body = nullptr;  // the partner body (the tick's)
std::uint32_t g_look_id = 0;      // the newest enemy_look's enemy (enemy_shot_test 0 takes it while it is a live mirror)
std::uint32_t g_outcome_id = 0;   // P1c: the newest outcome set here (enemy_camp 3, the guest) or carried here (the host): enemy_look -9
// P1b (docs/enemy-sync.md 10): the host's combat generation (the host bumps it on every reset; the guest keeps the newest seen)
std::uint32_t g_own_gen = 1;

// the world and the gate
std::uint32_t g_world = 0, g_gen = 0xFFFFFFFFu;
bool g_ready = false, g_private = false;
double g_ready_ms = 0;
bool g_fp_done = false;
std::uint16_t g_count = 0;
std::uint32_t g_ids = 0, g_guids = 0;
std::atomic<bool> g_mirroring{false};  // also read by the damage observer (worker threads)
std::atomic<int> g_role{0};
erules::FpGate g_last_gate = erules::FpGate::not_ours;
bool g_differ_logged = false, g_gate_logged = false;
double g_next_fp_try = 0, g_fp_retry = 0;

// guards' counters (worker threads)
std::atomic<unsigned> g_att_drop{0}, g_att_seen{0}, g_att_logged{0}, g_att_obs_logged{0}, g_host_hits_logged{0};
std::atomic<unsigned> g_tgt_drop{0}, g_tgt_ours{0}, g_tgt_replays{0}, g_tgt_others{0}, g_tgt_rcx{0}, g_tgt_rcx20{0}, g_tgt_rcx38{0};
std::atomic<std::uintptr_t> g_our_sam{0};

// ---------------------------------------------------------------- shared reads
struct LocalEnemy {
    std::uint32_t id = 0;
    std::uintptr_t actor = 0;
};
WorldTransform XfFromYaw(const WorldTransform& cur, const double (&pos)[3], float yaw) {
    // the current rotation turned about the up axis to the wanted yaw (keeps the game's own row convention and any tilt)
    WorldTransform x = cur;
    x.pos.x = pos[0], x.pos.y = pos[1], x.pos.z = pos[2];
    const double now = std::atan2(cur.rot.m[1][1], cur.rot.m[1][0]);
    const double d = static_cast<double>(yaw) - now, c = std::cos(d), s = std::sin(d);
    for (int r = 0; r < 3; ++r) {
        const double rx = cur.rot.m[r][0], ry = cur.rot.m[r][1];
        x.rot.m[r][0] = static_cast<float>(rx * c - ry * s);
        x.rot.m[r][1] = static_cast<float>(rx * s + ry * c);
    }
    return x;
}
float YawOfXf(const WorldTransform& t) { return static_cast<float>(std::atan2(t.rot.m[1][1], t.rot.m[1][0])); }
double Dist3(const double (&a)[3], const double (&b)[3]) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}
bool LocalPlace(game::Entity* e, double (&p)[3]) {
    WorldTransform x{};
    if (!e || !ReadXf(reinterpret_cast<std::uintptr_t>(e), x)) return false;
    p[0] = x.pos.x, p[1] = x.pos.y, p[2] = x.pos.z;
    return true;
}
// The registry's enemy actors in use (the census's walk; ids and actors only).
void WalkEnemies(std::vector<LocalEnemy>& out) {
    out.clear();
    const std::uintptr_t reg = mem::Q(A(kRegGlobal));
    std::int32_t ngroups = 0;
    if (!reg || !mem::Rd(reg + 0x440, ngroups) || ngroups <= 0 || ngroups > 512) return;
    const std::uintptr_t garr = mem::Q(reg + 0x448);
    for (std::int32_t gi = 0; gi < ngroups; ++gi) {
        const std::uintptr_t g = mem::Q(garr + 8ull * static_cast<unsigned>(gi));
        std::int32_t count = 0;
        if (!g || !mem::Rd(g + 0x40, count) || count <= 0 || count > 8192) continue;
        const std::uintptr_t arr = mem::Q(g + 0x48);
        if (!arr) continue;
        static std::vector<std::uintptr_t> objs;
        objs.resize(static_cast<std::size_t>(count));
        if (!mem::Read(arr, objs.data(), objs.size() * 8)) continue;
        for (const std::uintptr_t o : objs) {
            if (!o) continue;
            std::uint8_t head[0x28] = {};
            if (!mem::Read(o, head, sizeof(head)) || !head[kActInUse]) continue;
            std::uint32_t id = 0;
            std::memcpy(&id, head + kActId, 4);
            if (enemy::IsHumanEnemyId(id)) out.push_back(LocalEnemy{id, o});
        }
    }
}
// One enemy actor's state now (false: not the same in-use MULE actor any more).
struct ActorNow {
    std::uint32_t state = 0;
    std::uintptr_t entity = 0;
    bool realized = false, dead = false, hidden_or_asleep = false, seq_only = false, held = false;
    std::uint32_t a88 = 0, ea4 = 0;
    bool have_xf = false;
    WorldTransform xf{};
    double pos[3] = {};
};
bool ReadActor(std::uintptr_t actor, std::uint32_t id, ActorNow& a) {
    std::uint8_t in_use = 0;
    std::uint32_t aid = 0;
    if (!actor || mem::Q(actor) != A(kMuleVt) || !mem::Rd(actor + kActInUse, in_use) || !in_use || !mem::Rd(actor + kActId, aid) || aid != id) return false;
    mem::Rd(actor + kActState, a.state);
    const std::uintptr_t ref = mem::Q(actor + kActEntRef);
    a.entity = ref ? ref - 0x20 : 0;
    a.realized = a.state == 2 && a.entity;
    a.a88 = U32(actor + kActFlags88);
    a.ea4 = U32(actor + kMuleSeq);
    a.hidden_or_asleep = (a.a88 & 3u) != 0;
    a.seq_only = ((U32(actor + kMule124) >> 12) & 1u) != 0;
    a.held = ((a.ea4 >> 14) & 1u) != 0;
    if (a.realized) {
        std::uint64_t ef = 0;
        a.dead = mem::Rd(a.entity + 0x88, ef) && (ef & 0x80);
        a.have_xf = ReadXf(a.entity, a.xf);
    }
    if (!a.have_xf) {
        double p[3] = {};
        float rot[9] = {};
        if (mem::Read(actor + kActXf, p, sizeof(p)) && std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) && std::fabs(p[0]) < 1e7 &&
            mem::Read(actor + kActXf + 0x18, rot, sizeof(rot))) {
            a.xf.pos.x = p[0], a.xf.pos.y = p[1], a.xf.pos.z = p[2];
            std::memcpy(&a.xf.rot, rot, sizeof(rot));
            a.have_xf = true;
        }
    }
    if (a.have_xf) a.pos[0] = a.xf.pos.x, a.pos[1] = a.xf.pos.y, a.pos[2] = a.xf.pos.z;
    return true;
}
bool InCutscene() {
    std::uint64_t flags = 0;
    std::uint32_t mask = 0;
    if (!game::ReadStopGates(&flags, &mask)) return false;
    return (flags & game::kGameStateCutscene) != 0;
}
bool InPrivateRoom() {
    std::uint64_t flags = 0;
    std::uint32_t mask = 0;
    if (!game::ReadStopGates(&flags, &mask)) return false;
    return (flags & game::kGameStatePrivateRoom) != 0;
}
bool ActorPlaceFar(std::uintptr_t actor, const WorldTransform& xf, bool variant_b) noexcept {
    std::uint8_t save[kActSaveSize];
    if (!mem::Read(actor + kActSave, save, sizeof(save))) return false;
    __try {
        reinterpret_cast<FnPlace3>(A(kVtPlace))(actor, &xf, 1);
        std::memcpy(reinterpret_cast<void*>(actor + kActSave), save, sizeof(save));
        if (variant_b) {
            *reinterpret_cast<volatile std::uint8_t*>(actor + kActRePathFlag) = 1;
            reinterpret_cast<FnActor>(A(kRePath))(actor);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------- the guards (worker threads)
void LogDi(const char* head, const samdmg::View& v) {
    std::uintptr_t w[16] = {};
    mem::Read(v.di, w, sizeof(w));
    const char* rn = v.attacker ? mem::RttiName(mem::RttiOf(v.attacker)) : nullptr;
    std::uint32_t id = 0;
    g_mset.Find(v.attacker, &id);
    LogLater("%s (attacker %p %s = mirror %05X, amount %.1f; DamageInfo +0..+0x78: %llX %llX %llX %llX %llX %llX %llX %llX %llX %llX %llX %llX %llX %llX %llX %llX)", head,
        reinterpret_cast<void*>(v.attacker), rn ? rn : "?", id, static_cast<double>(v.amount), static_cast<unsigned long long>(w[0]),
        static_cast<unsigned long long>(w[1]), static_cast<unsigned long long>(w[2]), static_cast<unsigned long long>(w[3]), static_cast<unsigned long long>(w[4]),
        static_cast<unsigned long long>(w[5]), static_cast<unsigned long long>(w[6]), static_cast<unsigned long long>(w[7]), static_cast<unsigned long long>(w[8]),
        static_cast<unsigned long long>(w[9]), static_cast<unsigned long long>(w[10]), static_cast<unsigned long long>(w[11]), static_cast<unsigned long long>(w[12]),
        static_cast<unsigned long long>(w[13]), static_cast<unsigned long long>(w[14]), static_cast<unsigned long long>(w[15]));
}
bool MirrorAttacker(std::uintptr_t attacker) {
    if (!attacker) return false;
    if (g_mset.Find(attacker)) return true;
    const std::uintptr_t owner = mem::Q(attacker + 0x300);  // a weapon instance of a mirror [I]
    if (owner && g_mset.Find(owner)) return true;
    // a weapon entity (DSWeaponEntity: the host's MULE hits name it, run 20261124-102043): its instance (+0x338), whose +0x300 is
    // the holder (the Enqueue's own test for our Sam)
    const std::uintptr_t inst = mem::Q(attacker + 0x338);
    const std::uintptr_t holder = inst ? mem::Q(inst + 0x300) : 0;
    return holder && g_mset.Find(holder);
}
samdmg::Verdict MirrorAttackerFilter(const samdmg::View& v) {
    if (ephit::IsOwnApply(v.ctx)) return samdmg::Verdict::keep;  // round 28 E2: this game's own PHIT apply (the MULE's mirror as its attacker)
    if (!g_any_frozen.load(std::memory_order_relaxed) || !MirrorAttacker(v.attacker)) return samdmg::Verdict::keep;
    g_att_seen.fetch_add(1, std::memory_order_relaxed);
    erules::DamageIn d;
    d.mirror_attacker = true;
    d.guard = g_guard.load(std::memory_order_relaxed);
    const bool drop = erules::SamPolicy(d) == erules::Dmg::drop;
    if (drop) g_att_drop.fetch_add(1, std::memory_order_relaxed);
    if (g_att_logged.fetch_add(1, std::memory_order_relaxed) < 12) LogDi(drop ? "[enemy] mirror hit on our Sam: dropped" : "[enemy] mirror hit on our Sam: counted only (guard off)", v);
    return drop ? samdmg::Verdict::drop : samdmg::Verdict::keep;
}
// Observer (before the filters): the first 16 hits on our Sam while a mirror is frozen (guest), the first 8 on the host.
void MirrorHitObserver(std::uintptr_t sam, std::uintptr_t msg) {
    if (g_role == 1) {
        if (g_host_hits_logged.load(std::memory_order_relaxed) >= 8 || !g_mirroring) return;
        const samdmg::View v = samdmg::Read(sam, msg);
        const char* rn = v.attacker ? mem::RttiName(mem::RttiOf(v.attacker)) : nullptr;
        if (g_host_hits_logged.fetch_add(1, std::memory_order_relaxed) < 8)
            LogLater("[enemy] host: a hit on our Sam: attacker %p %s, amount %.1f (the reference for the guest's kept enemy rounds)", reinterpret_cast<void*>(v.attacker),
                rn ? rn : "?", static_cast<double>(v.amount));
        return;
    }
    if (g_oset.Size()) {
        // P1b: an enemy she owns hit her Sam (its own AI: before the filters, so the shield's drops count too)
        const samdmg::View v = samdmg::Read(sam, msg);
        std::uint32_t id = 0;
        bool owned = v.attacker && g_oset.Find(v.attacker, &id);
        if (!owned && v.attacker) {
            const std::uintptr_t holder = mem::Q(v.attacker + 0x300);
            const std::uintptr_t inst = mem::Q(v.attacker + 0x338);
            owned = (holder && g_oset.Find(holder, &id)) || (inst && mem::Q(inst + 0x300) && g_oset.Find(mem::Q(inst + 0x300), &id));
        }
        if (owned) {
            g_owned_hits.fetch_add(1, std::memory_order_relaxed);
            CountOwnedHit(id);
            if (g_owned_hits_logged.fetch_add(1, std::memory_order_relaxed) < 8) {
                const char* rn = mem::RttiName(mem::RttiOf(v.attacker));
                LogLater("[enemy] an owned enemy hit our Sam: attacker %p %s id %05X, amount %.1f (the shield %s)", reinterpret_cast<void*>(v.attacker), rn ? rn : "?", id,
                         static_cast<double>(v.amount), g_shield.load(std::memory_order_relaxed) ? "drops it" : "off");
            }
            return;
        }
    }
    if (!g_any_frozen.load(std::memory_order_relaxed) || g_att_obs_logged.load(std::memory_order_relaxed) >= 16) return;
    const samdmg::View v = samdmg::Read(sam, msg);
    if (g_att_obs_logged.fetch_add(1, std::memory_order_relaxed) >= 16) return;
    const char* rn = v.attacker ? mem::RttiName(mem::RttiOf(v.attacker)) : nullptr;
    LogLater("[enemy] a hit on our Sam while mirrors are frozen: attacker %p %s, a mirror: %s, amount %.1f, ref %p", reinterpret_cast<void*>(v.attacker), rn ? rn : "?",
        MirrorAttacker(v.attacker) ? "yes" : "no", static_cast<double>(v.amount), reinterpret_cast<void*>(v.ref));
}
npcdmg::Verdict MirrorTargetFilter(std::uintptr_t receiver, std::uintptr_t /*msg*/, const samdmg::View& v) {
    if (!g_any_frozen.load(std::memory_order_relaxed) || !receiver) return npcdmg::Verdict::keep;
    std::uint32_t id = 0;
    bool hit = false;
    if (g_mset.Find(receiver, &id, false)) hit = true, g_tgt_rcx.fetch_add(1, std::memory_order_relaxed);
    else if (g_mset.Find(receiver, &id, true)) hit = true, g_tgt_rcx20.fetch_add(1, std::memory_order_relaxed);
    else if (g_mset.Find(receiver - 0x38, &id, false)) hit = true, g_tgt_rcx38.fetch_add(1, std::memory_order_relaxed);
    if (!hit) return npcdmg::Verdict::keep;
    const std::uintptr_t sam = g_our_sam.load(std::memory_order_relaxed);
    if (sam && erules::NearKey(v.attacker, sam)) g_tgt_ours.fetch_add(1, std::memory_order_relaxed);
    else if (shots::IsReplayHit(v)) g_tgt_replays.fetch_add(1, std::memory_order_relaxed);
    else g_tgt_others.fetch_add(1, std::memory_order_relaxed);
    if (!g_guard.load(std::memory_order_relaxed)) return npcdmg::Verdict::keep;
    g_tgt_drop.fetch_add(1, std::memory_order_relaxed);
    return npcdmg::Verdict::drop;
}

// ---------------------------------------------------------------- the host
struct HostEnemy {
    std::uint32_t id = 0;
    std::uintptr_t actor = 0;
    ActorNow now;
    double dist = -1;
    double last_pos[3] = {};
    double last_t = -1e18;
    float vel[3] = {};
    erules::VehTest veh = erules::VehTest::none;  // round 23 B: in a vehicle (every 1 s; SEN1 kInVehicle)
    double veh_next = 0;
};
std::vector<HostEnemy> g_host;
double g_host_walk_next = 0;
erules::Schedule g_sched;
erules::PoseSlots g_pslots;      // the captured enemies (logical slots k: SEP1's slot byte)
std::uintptr_t g_pslot_sm[erules::kPoseSlots] = {};
int g_pslot_real[erules::kPoseSlots] = {-1, -1, -1, -1, -1, -1, -1, -1};  // k -> the realbody slot (g_palloc)
double g_pslot_next[erules::kPoseSlots] = {};
std::uint32_t g_pslot_seq[erules::kPoseSlots] = {};
// P1b: the 8 enemy pose slots are shared by apply (mirrors, either game) and capture (the enemies this game owns): one allocator,
// apply first
erules::PoseAlloc g_palloc;
std::uint32_t g_sen_seq = 0;
struct SenderStats {
    unsigned sen = 0, entries = 0, c10 = 0, c2 = 0, c02 = 0, sep = 0, bytes = 0, no_guest = 0;
} g_ss, g_ss_logged;
double g_ss_next = 0;
std::uint64_t g_shooter_keys[erules::KeySet::kCap];
std::uint32_t g_shooter_vals[erules::KeySet::kCap];

void CaptureSlotOff(unsigned k) {
    if (g_pslot_real[k] >= 0) {
        if (g_palloc.At(g_pslot_real[k]) == erules::PoseAlloc::kCapture) {
            realbody::SetEnemySlot(static_cast<unsigned>(g_pslot_real[k]), 0, realbody::EnemySlotMode::off);
            g_palloc.Free(g_pslot_real[k]);
        }
        g_pslot_real[k] = -1;
    }
    g_pslot_sm[k] = 0;
}
void HostResetPoses() {
    for (unsigned k = 0; k < erules::kPoseSlots; ++k) CaptureSlotOff(k);
    g_pslots.Reset();
}
// The capture slots for the chosen enemies (2 Hz): `sm_of(id)` the realized SkinnedModel of an enemy we own (0 none).
template <class SmOf>
void UpdateCaptureSlots(const erules::PoseCand* pc, unsigned n, SmOf sm_of) {
    g_pslots.Update(pc, n, static_cast<unsigned>(g_ecfg.enemy_pose_count));
    for (unsigned k = 0; k < erules::kPoseSlots; ++k) {
        const std::uint32_t id = g_pslots.Id(k);
        const std::uintptr_t sm = id ? sm_of(id) : 0;
        if (sm == g_pslot_sm[k] && (sm == 0 || g_pslot_real[k] >= 0)) continue;
        CaptureSlotOff(k);
        if (!sm) continue;
        const int real = g_palloc.Take(erules::PoseAlloc::kCapture, static_cast<unsigned>(g_ecfg.enemy_pose_count));
        if (real < 0) continue;  // the mirrors have them (apply first)
        if (realbody::SetEnemySlot(static_cast<unsigned>(real), sm, realbody::EnemySlotMode::capture)) g_pslot_real[k] = real, g_pslot_sm[k] = sm;
        else g_palloc.Free(real);
        g_pslot_seq[k] = 0;
    }
}
// SEP1: each capture slot at its period, a new capture only.
void SendCaptures(double now) {
    for (unsigned k = 0; k < erules::kPoseSlots; ++k) {
        if (!g_pslot_sm[k] || g_pslot_real[k] < 0 || now < g_pslot_next[k]) continue;
        g_pslot_next[k] = now + g_pslots.Period(k);
        static ewire::EnemyPose p;
        double cms = 0;
        if (!realbody::CaptureEnemyFrame(static_cast<unsigned>(g_pslot_real[k]), &p.joints, &cms) || p.joints.seq == g_pslot_seq[k]) continue;
        g_pslot_seq[k] = p.joints.seq;
        p.slot = static_cast<std::uint8_t>(k);
        p.actor_id = g_pslots.Id(k);
        p.sender_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(now) & 0xFFFFFFFFull);
        p.joints.sender_ms = p.sender_ms;
        std::uint8_t buf[ewire::kSepMaxSize];
        const std::size_t size = ewire::EncodeEnemyPose(p, buf, sizeof(buf));
        if (size && link::SendCombatFrame(buf, size)) ++g_ss.sep, g_ss.bytes += static_cast<unsigned>(size);
    }
}
void HostReset() {
    HostResetPoses();
    g_host.clear();
    g_sched.Reset();
    g_host_walk_next = 0;
    shots::SetEnemyShooters(nullptr, nullptr, 0);
}

// round 23 B (below): the in-vehicle test of one MULE (erules::VehicleTest; the ride fields only with their byte checks) and its
// change line (the first 24).
erules::VehTest VehicleOf(std::uint32_t id, std::uintptr_t actor, std::uintptr_t entity, std::uint32_t* e20, std::int32_t* seat);
void NoteVehicle(std::uint32_t id, const char* who, erules::VehTest was, erules::VehTest now_t, std::uint32_t e20, std::int32_t seat);
void HostWalk(const double (&guest)[3], bool have_guest, double now) {
    std::vector<LocalEnemy> all;
    WalkEnemies(all);
    std::vector<HostEnemy> next;
    next.reserve(all.size());
    for (const LocalEnemy& le : all) {
        HostEnemy h;
        for (const HostEnemy& o : g_host)
            if (o.id == le.id) {
                h = o;
                break;
            }
        h.id = le.id, h.actor = le.actor;
        if (!ReadActor(le.actor, le.id, h.now)) continue;
        h.dist = have_guest && h.now.have_xf ? Dist3(h.now.pos, guest) : -1;
        if (g_ecfg.enemy_mirror_vehicle && now >= h.veh_next) {
            // round 23 B: every 1 s for each enemy the host sends (not only at a mirror's entry)
            h.veh_next = now + erules::kVehicleCheckMs;
            std::uint32_t e20 = erules::kVehicleIdNone;
            std::int32_t seat = -1;
            const erules::VehTest was = h.veh;
            h.veh = h.now.realized ? VehicleOf(h.id, h.actor, h.now.entity, &e20, &seat) : erules::VehTest::none;
            if (h.veh != was) NoteVehicle(h.id, "the host's", was, h.veh, e20, seat);
        }
        next.push_back(h);
    }
    g_host.swap(next);
}
// P1b (below): the epoch byte SEN1 carries, the phase (+0x110, 0xFF unknown), whether the host skips an enemy (the guest owns
// it, or a hand-over is pending: its copy is frozen here), the ownership tick.
std::uint8_t OwnEpochByte(std::uint32_t id);
std::uint8_t PhaseOf(std::uintptr_t actor);
bool HostSkips(std::uint32_t id);
void OwnHostTick(const TickIn& in, double now, const double (&guest)[3], bool have_guest);
// A fresh read of one enemy for its SEN1 entry (place, state, velocity from the last two samples).
bool HostEntry(HostEnemy& h, double now, const double (&guest)[3], bool have_guest, ewire::Enemy& e) {
    if (!ReadActor(h.actor, h.id, h.now) || !h.now.have_xf) return false;
    if (have_guest) h.dist = Dist3(h.now.pos, guest);
    if (h.last_t > -1e17 && now - h.last_t > 20 && now - h.last_t < 3000) {
        const double dt = (now - h.last_t) / 1000.0;
        for (int k = 0; k < 3; ++k) h.vel[k] = static_cast<float>((h.now.pos[k] - h.last_pos[k]) / dt);
    } else if (now - h.last_t >= 3000) {
        h.vel[0] = h.vel[1] = h.vel[2] = 0;
    }
    std::memcpy(h.last_pos, h.now.pos, sizeof(h.last_pos));
    h.last_t = now;
    e.actor_id = h.id;
    e.owner_epoch = OwnEpochByte(h.id);
    e.phase = PhaseOf(h.actor);
    e.state = static_cast<std::uint8_t>((h.now.realized ? ewire::kRealized : 0) | (h.now.dead ? ewire::kDead : 0) | (h.now.hidden_or_asleep ? ewire::kHidden : 0) |
                                        (h.now.held ? ewire::kSeqHeld : 0) | (g_pslots.SlotOf(h.id) >= 0 ? ewire::kPosed : 0) |
                                        ewire::DefeatBits(ecombat::HostDefeat(h.id)));  // P2: the outcome (the OUTCOME's redundancy)
    const double sp = std::sqrt(static_cast<double>(h.vel[0]) * h.vel[0] + static_cast<double>(h.vel[1]) * h.vel[1]);
    e.flags = static_cast<std::uint8_t>((sp > 0.2 ? ewire::kMoving : 0) | (h.veh != erules::VehTest::none ? ewire::kInVehicle : 0));
    for (int k = 0; k < 3; ++k) e.pos[k] = h.now.pos[k], e.vel[k] = h.vel[k];
    e.yaw = YawOfXf(h.now.xf);
    return true;
}

void HostTick(const TickIn& in, double now) {
    double guest[3] = {};
    const bool have_guest = in.body && LocalPlace(in.body, guest);
    OwnHostTick(in, now, guest, have_guest);  // P1b (the guest-owned ones are followed even with no partner body here)
    if (!have_guest) {
        ++g_ss.no_guest;
        return;  // the guest is in another region (no partner body here): nothing to mirror
    }
    if (now >= g_host_walk_next) {
        g_host_walk_next = now + 500;
        HostWalk(guest, have_guest, now);
        // the pose slots (2 Hz): the nearest realized within 120 m (P1b: not the guest's: they are her captures)
        std::vector<erules::PoseCand> pc;
        // P2: a dead or down one is still posed (it lies where the host's lies), in the 5 Hz half, never taking a live one's fast rank
        for (const HostEnemy& h : g_host) {
            const bool down = h.now.dead || ecombat::HostDefeat(h.id) != 0;
            pc.push_back(erules::PoseCand{h.id, h.dist, h.now.realized && !HostSkips(h.id), down});
        }
        UpdateCaptureSlots(pc.data(), static_cast<unsigned>(pc.size()), [](std::uint32_t id) -> std::uintptr_t {
            for (const HostEnemy& h : g_host)
                if (h.id == id && h.now.realized) return mem::Q(h.now.entity + 0xB8);
            return 0;
        });
        // the shooters for the enemy shots: the 10 Hz class (entity -> actor id); P1b: not the guest's (frozen here)
        unsigned ns = 0;
        for (const HostEnemy& h : g_host)
            if (ns < erules::KeySet::kCap && h.now.realized && h.dist >= 0 && h.dist <= g_ecfg.enemy_mirror_radius && !HostSkips(h.id))
                g_shooter_keys[ns] = h.now.entity, g_shooter_vals[ns] = h.id, ++ns;
        shots::SetEnemyShooters(g_shooter_keys, g_shooter_vals, ns);
    }
    // SEN1: the schedule's picks, read fresh, at most 2 frames (P1b: the guest's and the pending ones skipped)
    std::vector<erules::Cand> cand;
    cand.reserve(g_host.size());
    std::vector<unsigned> cidx;
    cidx.reserve(g_host.size());
    for (unsigned i = 0; i < g_host.size(); ++i)
        if (!HostSkips(g_host[i].id)) cand.push_back(erules::Cand{g_host[i].id, g_host[i].dist}), cidx.push_back(i);
    unsigned pick[erules::Schedule::kPerTick];
    const unsigned np = g_sched.Pick(cand.data(), static_cast<unsigned>(cand.size()), now, g_ecfg.enemy_mirror_radius, pick, erules::Schedule::kPerTick);
    static ewire::EnemyFrame f;
    unsigned at = 0;
    while (at < np) {
        f.n = 0;
        f.flags = 0;
        f.seq = ++g_sen_seq;
        f.sender_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(now) & 0xFFFFFFFFull);
        f.gen_tag = g_own_gen;
        if (InCutscene()) f.flags |= ewire::kSenderCutscene;
        for (; at < np && f.n < ewire::kSenMax; ++at) {
            HostEnemy& h = g_host[cidx[pick[at]]];
            ewire::Enemy e;
            if (!HostEntry(h, now, guest, have_guest, e)) continue;
            const erules::Class c = erules::ClassOf(h.dist, g_ecfg.enemy_mirror_radius);
            (c == erules::Class::near10 ? g_ss.c10 : c == erules::Class::mid2 ? g_ss.c2 : g_ss.c02)++;
            f.e[f.n++] = e;
        }
        if (!f.n) continue;
        std::uint8_t buf[ewire::kSenMaxSize];
        const std::size_t size = ewire::EncodeEnemies(f, buf, sizeof(buf));
        if (size && link::SendCombatFrame(buf, size)) ++g_ss.sen, g_ss.entries += f.n, g_ss.bytes += static_cast<unsigned>(size);
    }
    SendCaptures(now);  // SEP1
    if (now >= g_ss_next) {
        const double secs = g_ss_next > 0 ? 5.0 : 0;
        g_ss_next = now + 5000;
        if (secs > 0) {
            const SenderStats d{g_ss.sen - g_ss_logged.sen, g_ss.entries - g_ss_logged.entries, g_ss.c10 - g_ss_logged.c10, g_ss.c2 - g_ss_logged.c2,
                                g_ss.c02 - g_ss_logged.c02, g_ss.sep - g_ss_logged.sep, g_ss.bytes - g_ss_logged.bytes, g_ss.no_guest - g_ss_logged.no_guest};
            Log("[enemy] sender: SEN1 %.1f/s (entries %.1f/s: 10 Hz %u, 2 Hz %u, rest %u), SEP1 %.1f/s (slots %u), bytes %.0f/s; %zu enemies, shooters %u",
                d.sen / secs, d.entries / secs, d.c10, d.c2, d.c02, d.sep / secs, g_pslots.Used(), d.bytes / secs, g_host.size(), shots::EnemyShooterCount());
        }
        g_ss_logged = g_ss;
    }
}

// ---------------------------------------------------------------- the guest
struct PoseRx {
    wire::JointFrame a, b;
    double ta = -1e18, tb = -1e18;  // the sender's unwrapped ms
    double rx = -1e18;
    bool foreign_logged = false;
};
struct Mirror {
    std::uint32_t id = 0;
    std::uintptr_t actor = 0, entity = 0, sm = 0;
    double t_frozen = 0, last_tp = -1e18, next_err = 0;
    int slot = -1;
    std::uint32_t ea4_entry = 0;
    bool bit13_pre = false;  // +0xEA4 bit 13 already set before our Occupied{1} (the game's: Required{0} never clears it)
    double err = 0;
    bool frozen = false;
    bool own = false;        // P1b, the host: its copy of an enemy the guest owns (no lease: the ownership rules give it back)
    const char* why = "";    // a release held by a cutscene: its reason
    // round 23 B (docs/enemy-sync.md 14.8): the diagnostics, the not-holding rule, the in-vehicle test
    erules::TeleportRing tp;  // the driver's newest teleports
    unsigned tp5 = 0;         // teleports in the 5 s window
    double err_max5 = 0;      // its largest error in the 5 s window
    double veh_next = 0;      // the next in-vehicle test (1 s)
    erules::VehTest veh = erules::VehTest::none;
    std::uint32_t veh_e20 = erules::kVehicleIdNone;
    std::int32_t veh_seat = -1;
    bool nothold_logged = false;  // a follower (the host) not holding: logged once
    bool posed_once = false;      // round 23 C: it has had the owner's pose here (a frame pushed) since its freeze
    // round 28 T1 (vehicle_mule_v3): since when its host copy is in a vehicle with no truck of hers near (0: not), the truck
    double v3_unseated_since = 0;
    std::uint32_t v3_truck = 0;
};
std::map<std::uint32_t, erules::Track> g_tracks;
std::map<std::uint32_t, PoseRx> g_prx;
std::vector<Mirror> g_mirrors;
std::vector<LocalEnemy> g_local;
double g_local_next = 0, g_far_next = 0;
erules::SenderClock g_clock;
// The cutscene hold, stepped once a tick by Tick (whatever the gate): 0 normal, 1 hold, 2 release now (erules::CutsceneHold).
// Occupied{0} calls vt+0x138 (0x142DCA0F0: hide and sleep the actor) during a cutscene, so no release posts while it holds:
// a mirror given back then (the link, the gate, the switch, a fault) stays frozen in g_pending (the guards still match it)
// until the hold ends (review rv-safety 2).
erules::CutsceneHold g_cut;
int g_cut_step = 0;
bool g_holding = false;
std::vector<Mirror> g_pending;
// The far placement's originals (the guest's own places of the copies it moved, by id): put back on off, stop and the switch
// (rv-safety 7; an actor realized since is the game's again: forgotten).
struct FarOrig {
    std::uintptr_t actor = 0;
    WorldTransform xf{};
};
std::map<std::uint32_t, FarOrig> g_far_orig;
struct GuestStats {
    unsigned rx_sen = 0, rx_bad = 0, rx_entries = 0, rx_sep = 0, sep_bad = 0, sep_refused = 0, sep_foreign = 0;
    unsigned on = 0, released = 0, refused = 0, taken = 0, sets = 0, teleports = 0, no_pose = 0, far_placed = 0, far_faults = 0, set_faults = 0;
    unsigned host_refused = 0, host_released = 0, host_hidden_now = 0;  // M0
} g_gs;
std::vector<double> g_err_win;
unsigned g_sets_win = 0, g_tp_win = 0;
double g_gs_next = 0, g_att_next = 0;
unsigned g_logged_refusals = 0;
std::map<std::uint32_t, int> g_refused_once;  // id -> refusal logged
struct FarCheck {
    std::uint32_t id;
    std::uintptr_t actor;
    double due;
};
std::vector<FarCheck> g_far_checks;
std::vector<double> g_drift;
// round 23 B: the 30 s error summary's samples, the ids logged once over 20 m, the counts for the summary
std::vector<double> g_err_30;
double g_sum_next = 0;
std::set<std::uint32_t> g_over20;
struct MirrorB {
    unsigned worst = 0, over20 = 0, nothold = 0, vehicle = 0, retries = 0;
} g_mb;

// The MULE weapon component's flag +0x268 (the weapon update returns at once while it is set): our Occupied{1} sets it (the
// weapon's Occupied handler 0x142B07FB0 -> 0x142BB60F0). Its value before our freeze, by id; put back after our release when
// the release left it set (an unfrozen MULE that cannot fire: run 20261124-151942, her owned MULEs alerted 50 s without a round).
std::map<std::uint32_t, std::uint8_t> g_w268_pre;
std::atomic<unsigned> g_w268_restored{0};
std::uintptr_t EntityOfActor(std::uintptr_t actor) {
    const std::uintptr_t ref = mem::Q(actor + kActEntRef);
    return ref ? ref - 0x20 : 0;
}
void NoteWeaponPre(std::uint32_t id, std::uintptr_t actor) {
    const std::uintptr_t e = EntityOfActor(actor);
    if (e) g_w268_pre[id] = WeaponFlag(e);
}
bool WriteByte(std::uintptr_t at, std::uint8_t v) noexcept {
    __try {
        *reinterpret_cast<volatile std::uint8_t*>(at) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void RestoreWeapon(std::uint32_t id, std::uintptr_t actor) {
    const auto it = g_w268_pre.find(id);
    if (it == g_w268_pre.end()) return;
    const std::uint8_t pre = it->second;
    g_w268_pre.erase(it);
    const std::uintptr_t e = EntityOfActor(actor);
    const std::uintptr_t c = e ? partner::FindComponentByName(reinterpret_cast<game::Entity*>(e), "DSNpcWeaponComponent") : 0;
    std::uint8_t now = 0xFF;
    if (!c || !mem::Rd(c + 0x268, now) || pre != 0 || now != 1) return;
    // a MULE that is down, dead (+0xEA0 3 / 4 / 5) or hidden / asleep (a88 & 3) now: the game may have set the flag itself
    // (review rv-safety 13): never re-armed by us
    const std::uint32_t st = U32(actor + 0xEA0), a88 = U32(actor + kActFlags88);
    if (st == 3 || st == 4 || st == 5 || (a88 & 3u)) {
        if (g_w268_restored.fetch_add(1) < 12)
            Log("[enemy] weapon flag kept id=%05X (+0x268 1: the MULE is down, dead or hidden now: state %u a88 0x%X)", id, st, a88);
        return;
    }
    if (WriteByte(c + 0x268, 0) && g_w268_restored.fetch_add(1) < 12)
        Log("[enemy] weapon flag restored id=%05X (+0x268 1 -> 0 after our release; it was 0 before our freeze)", id);
}
void PublishMirrors() {
    std::uint64_t k[erules::KeySet::kCap];
    std::uint32_t v[erules::KeySet::kCap];
    unsigned n = 0;
    for (const Mirror& m : g_mirrors)
        if (m.frozen && m.entity && n < erules::KeySet::kCap) k[n] = m.entity, v[n] = m.id, ++n;
    for (const Mirror& m : g_pending)
        if (m.entity && n < erules::KeySet::kCap) k[n] = m.entity, v[n] = m.id, ++n;
    g_mset.Publish(k, v, n);
    g_any_frozen.store(n > 0, std::memory_order_relaxed);
}
// P2: the mirrors hidden and put to sleep here because the host's copy is dead (PostRelease). Restored (unhidden
// and woken, only while it is still the same in-use MULE with those a88 bits) when the host's is alive again (a revive, a new
// host epoch), and on link loss, the gate off, a reset or a fault (review rv-safety 7: what we write is restored).
std::map<std::uint32_t, std::uintptr_t> g_dead_hidden;
void HideDead(std::uint32_t id, std::uintptr_t actor, const char* why) {
    const bool hid = ActorBool(actor, kVtHidden, 1), slept = ActorBool(actor, kVtSleep, 1);
    g_dead_hidden[id] = actor;
    Log("[enemy] mirror id=%05X %s: hidden and asleep here (the host's is dead; a88 0x%X%s)", id, why, U32(actor + kActFlags88), hid && slept ? "" : ", a call FAULTED");
}
void UnhideDead(bool all, const char* why) {
    for (auto it = g_dead_hidden.begin(); it != g_dead_hidden.end();) {
        if (!all && ecombat::GuestDefeat(it->first) == static_cast<std::uint8_t>(erules::Defeat::dead)) {
            ++it;
            continue;
        }
        ActorNow a;
        if (ReadActor(it->second, it->first, a) && (a.a88 & 3u)) {
            bool ok = true;
            if (a.a88 & 1u) ok = ActorBool(it->second, kVtHidden, 0) && ok;
            if (a.a88 & 2u) ok = ActorBool(it->second, kVtSleep, 0) && ok;
            Log("[enemy] mirror id=%05X shown and woken again (%s; a88 0x%X -> 0x%X%s)", it->first, why, a.a88, U32(it->second + kActFlags88), ok ? "" : ", a call FAULTED");
        }
        it = g_dead_hidden.erase(it);
    }
}
// M0 (enemy_host_guard): the mirrors given back hidden and asleep here because the host's copy is hidden there (never a live MULE
// beside her that the host's world put away). Shown and woken again (only the a88 bits we set, only while it is still the same
// in-use MULE) once the host's newest frame says realized and not hidden, the frames stop, or on off / reset.
struct HostHid {
    std::uintptr_t actor = 0;
    std::uint32_t a88_pre = 0;
};
std::map<std::uint32_t, HostHid> g_host_hid;
void HideHostHidden(std::uint32_t id, std::uintptr_t actor) {
    ActorNow a;
    if (!ReadActor(actor, id, a)) return;
    HostHid h;
    h.actor = actor, h.a88_pre = a.a88 & 3u;
    bool ok = true;
    if (!(h.a88_pre & 1u)) ok = ActorBool(actor, kVtHidden, 1) && ok;
    if (!(h.a88_pre & 2u)) ok = ActorBool(actor, kVtSleep, 1) && ok;
    g_host_hid[id] = h;
    g_gs.host_hidden_now = static_cast<unsigned>(g_host_hid.size());
    Log("[enemy] mirror id=%05X released hidden: hidden and asleep here while the host's is hidden (a88 0x%X -> 0x%X%s)", id, a.a88, U32(actor + kActFlags88),
        ok ? "" : ", a call FAULTED");
}
void UnhideHostHidden(bool all, const char* why, double now) {
    for (auto it = g_host_hid.begin(); it != g_host_hid.end();) {
        if (!all) {
            const auto t = g_tracks.find(it->first);
            const erules::Sample* z = t != g_tracks.end() ? t->second.Newest() : nullptr;
            if (erules::KeepHostHidden(z != nullptr, z ? now - t->second.rx_ms : 1e18, z && (z->state & ewire::kRealized), z && (z->state & ewire::kHidden))) {
                ++it;
                continue;
            }
        }
        ActorNow a;
        if (ReadActor(it->second.actor, it->first, a)) {
            bool ok = true;
            if ((a.a88 & 1u) && !(it->second.a88_pre & 1u)) ok = ActorBool(it->second.actor, kVtHidden, 0) && ok;
            if ((a.a88 & 2u) && !(it->second.a88_pre & 2u)) ok = ActorBool(it->second.actor, kVtSleep, 0) && ok;
            Log("[enemy] mirror id=%05X shown and woken again (%s; a88 0x%X -> 0x%X%s)", it->first, all ? why : "the host's is shown again or silent", a.a88,
                U32(it->second.actor + kActFlags88), ok ? "" : ", a call FAULTED");
        }
        it = g_host_hid.erase(it);
    }
    g_gs.host_hidden_now = static_cast<unsigned>(g_host_hid.size());
}
// Round 23 B (enemy_mirror_hold_check; docs/enemy-sync.md 14.8): the mirrors given back hidden and asleep here because they did not
// hold the owner's place (erules::NotHolding). M0's hide (only the a88 bits we set, only while it is the same in-use MULE); shown
// and woken again after 5 s (then the entry may mirror it again), or on off / reset. Its shots are still replayed from the owner's
// place meanwhile (NotHoldingShooter).
struct NotHold {
    std::uintptr_t actor = 0;
    std::uint32_t a88_pre = 0;
    double t = 0;
};
std::map<std::uint32_t, NotHold> g_nothold;
std::map<std::uint32_t, unsigned> g_nothold_n;  // id -> times hidden (the first is logged with its reason; then the first 3 and every 10th)
void HideNotHolding(std::uint32_t id, std::uintptr_t actor, double now) {
    ActorNow a;
    if (!ReadActor(actor, id, a)) return;
    NotHold h;
    h.actor = actor, h.a88_pre = a.a88 & 3u, h.t = now;
    bool ok = true;
    if (!(h.a88_pre & 1u)) ok = ActorBool(actor, kVtHidden, 1) && ok;
    if (!(h.a88_pre & 2u)) ok = ActorBool(actor, kVtSleep, 1) && ok;
    g_nothold[id] = h;
    const unsigned n = ++g_nothold_n[id];
    if (n <= 3 || n % 10 == 0)
        Log("[enemy] mirror id=%05X not holding (#%u): hidden and asleep here for %.0f s, then tried again (a88 0x%X -> 0x%X%s); its shots are still replayed "
            "from the owner's place", id, n, erules::kNotHoldRetryMs / 1000.0, a.a88, U32(actor + kActFlags88), ok ? "" : ", a call FAULTED");
}
void UnhideNotHolding(bool all, const char* why, double now) {
    for (auto it = g_nothold.begin(); it != g_nothold.end();) {
        if (!all && !erules::NotHoldRetry(now - it->second.t)) {
            ++it;
            continue;
        }
        ActorNow a;
        if (ReadActor(it->second.actor, it->first, a)) {
            bool ok = true;
            if ((a.a88 & 1u) && !(it->second.a88_pre & 1u)) ok = ActorBool(it->second.actor, kVtHidden, 0) && ok;
            if ((a.a88 & 2u) && !(it->second.a88_pre & 2u)) ok = ActorBool(it->second.actor, kVtSleep, 0) && ok;
            const unsigned n = g_nothold_n[it->first];
            if (all || n <= 3 || n % 10 == 0)
                Log("[enemy] mirror id=%05X shown and woken again (%s; a88 0x%X -> 0x%X%s)", it->first, all ? why : "not holding: the 5 s retry", a.a88,
                    U32(it->second.actor + kActFlags88), ok ? "" : ", a call FAULTED");
        }
        if (!all) ++g_mb.retries;
        it = g_nothold.erase(it);
    }
}
// Round 23 C (enemy_down_hold; docs/enemy-sync.md 14.9): the other player is down (death or KnotSpace, from its SEH1 beacon or the
// enemy_sync 9 fake). The partner's code in force here and ours; the guest's mirrors hidden while the host's Sam is down (kept
// frozen; only the hidden bit we set, only while it is the same in-use MULE), shown again when he is back, on every release of that
// mirror, and on off / reset (a new world forgets them untouched). The host's hidden followers go through the follower guard's own
// table (g_fhid). g_down_held: the ids held frozen in their last pose now (both roles).
static_assert(erules::kPlayerDownMax == shotw::kBeaconDownMax, "the beacon's down byte carries erules::PlayerDown");
std::uint8_t g_peer_down = 0;   // the partner's player: erules::PlayerDown in force here (0 up)
std::uint8_t g_our_down = 0;    // ours, as the beacon is told (latched, or the fake)
int g_peer_fake = -1, g_our_fake = -1;  // enemy_sync 9 (hidden runs): -1 none
erules::DownLatch g_our_latch;
double g_down_t0 = 0, g_down_own_next = 0;
struct DownHid {
    std::uintptr_t actor = 0;
    bool pre = false;  // already hidden before ours
};
std::map<std::uint32_t, DownHid> g_down_hid;   // the guest's
std::set<std::uint32_t> g_down_held;           // both roles: held now
std::set<std::uint32_t> g_down_fhid;           // the host: followers hidden by the down rule (in g_fhid)
struct DownStats {
    unsigned holds = 0, hides = 0, refused = 0, no_change = 0, resumed = 0, downs = 0;
} g_ds;
bool DownOn() { return g_ecfg.enemy_down_hold && g_peer_down != 0; }
unsigned DownResume(const char* why);  // (below: shows every copy the hold hid)
bool DownFreeze() { return erules::DownFreezesOwnership(g_ecfg.enemy_down_hold, g_our_down != 0, g_peer_down != 0); }
void DownHideMirror(std::uint32_t id, std::uintptr_t actor, const char* why) {
    if (g_down_hid.count(id)) return;
    ActorNow a;
    if (!ReadActor(actor, id, a)) return;
    DownHid h;
    h.actor = actor, h.pre = (a.a88 & 1u) != 0;
    const bool ok = h.pre || ActorBool(actor, kVtHidden, 1);
    g_down_hid[id] = h;
    ++g_ds.hides;
    Log("[enemy] down hold id=%05X: hidden here, kept frozen (%s; a88 0x%X -> 0x%X%s)", id, why, a.a88, U32(actor + kActFlags88), ok ? "" : ", the call FAULTED");
}
// One down-hidden mirror shown again (before any release posts: a copy released while hidden here could never fire again).
bool DownUnhideMirror(std::uint32_t id, const char* why) {
    const auto it = g_down_hid.find(id);
    if (it == g_down_hid.end()) return false;
    ActorNow a;
    if (!it->second.pre && ReadActor(it->second.actor, id, a)) {
        const bool ok = !(a.a88 & 1u) || ActorBool(it->second.actor, kVtHidden, 0);
        Log("[enemy] down hold id=%05X: shown again (%s; a88 0x%X -> 0x%X%s)", id, why, a.a88, U32(it->second.actor + kActFlags88), ok ? "" : ", the call FAULTED");
    }
    g_down_hid.erase(it);
    return true;
}
void DownUnhideAll(const char* why) {
    while (!g_down_hid.empty()) DownUnhideMirror(g_down_hid.begin()->first, why);
    g_down_held.clear();
}
// Round 23 review (erules::DownKeepsOff): the guest's sync turned off, or her Sam is not there, while the host's Sam is down (his
// reload after a death resets his fingerprint, so her gate goes to "waiting"; or his beacons stop meanwhile). Every frozen copy is
// kept frozen as it is: the hidden ones stay hidden, the others are held (nothing written: no place, no pose, no posts). Never
// given back alive. When he is back, DownResume shows and resumes them and the usual off release follows on the next tick; a world
// change forgets them, a link change, the switch off or a fault releases them as before. One line per spell.
bool g_down_kept = false;          // the keep is in force (logged once)
bool g_down_nothold_logged = false;  // the deferred not-holding retry was logged for this spell
bool g_down_nobeacon_logged = false; // DownTick: the partner down and its beacon gone (logged once per spell)
void DownKeep(const char* why) {
    unsigned held = 0;
    for (const Mirror& m : g_mirrors) {
        if (!m.frozen || g_down_hid.count(m.id)) continue;
        if (g_down_held.insert(m.id).second) ++g_ds.holds;
        ++held;
    }
    if (g_down_kept) return;
    g_down_kept = true;
    Log("[enemy] down hold: sync off (%s) while the host's Sam is down: %u mirrors kept frozen (held %u, hidden %zu), none given back alive; "
        "released when he is back or the world changes", why, held + static_cast<unsigned>(g_down_hid.size()), held, g_down_hid.size());
}
void FreeSlot(Mirror& m) {
    if (m.slot >= 0) {
        if (g_palloc.At(m.slot) == erules::PoseAlloc::kApply) {
            realbody::SetEnemySlot(static_cast<unsigned>(m.slot), 0, realbody::EnemySlotMode::off);
            g_palloc.Free(m.slot);
        }
        m.slot = -1;
    }
}
// Round 28 T1 (vehicle_mule_v3; docs/enemy-sync.md 14.21; erules::SeatedV3): the guest's mirrors whose host copy sits in a truck
// she follows (the host's MULE drives it, vehicle_drive's follower holds her copy): kept frozen, hidden and asleep here (no
// place, no pose, no posts; M0's pattern: only the a88 bits we set, only while it is the same in-use MULE), never given back alive
// to board her copy. Shown and woken again when its host copy is out of the vehicle (then driven to the host's place as usual),
// before any release posts, and on off / reset / a fault; a new world forgets them untouched (an address and id can repeat).
struct SeatHid {
    std::uintptr_t actor = 0;
    std::uint32_t a88_pre = 0;
    std::uint32_t truck = 0;
    double since = 0;
};
std::map<std::uint32_t, SeatHid> g_seat_hid;
unsigned g_seat_hides = 0, g_seat_shows = 0, g_seat_entries = 0;
void SeatHideMirror(Mirror& m, double now) {
    const auto it = g_seat_hid.find(m.id);
    if (it != g_seat_hid.end() && it->second.actor == m.actor) {
        it->second.truck = m.v3_truck ? m.v3_truck : it->second.truck;
        return;
    }
    ActorNow a;
    if (!ReadActor(m.actor, m.id, a)) return;
    SeatHid h;
    h.actor = m.actor, h.a88_pre = a.a88 & 3u, h.truck = m.v3_truck, h.since = now;
    bool ok = true;
    if (!(h.a88_pre & 1u)) ok = ActorBool(m.actor, kVtHidden, 1) && ok;
    if (!(h.a88_pre & 2u)) ok = ActorBool(m.actor, kVtSleep, 1) && ok;
    g_seat_hid[m.id] = h;
    FreeSlot(m);  // no pose for a hidden copy
    ++g_seat_hides;
    Log("[enemy] mirror id=%05X seated in V3 truck 0x%06X: kept frozen, hidden and asleep here (#%u; a88 0x%X -> 0x%X%s)", m.id, h.truck, g_seat_hides, a.a88,
        U32(m.actor + kActFlags88), ok ? "" : ", a call FAULTED");
}
bool SeatShowMirror(std::uint32_t id, const char* why, double now) {
    const auto it = g_seat_hid.find(id);
    if (it == g_seat_hid.end()) return false;
    ActorNow a;
    if (ReadActor(it->second.actor, id, a)) {
        bool ok = true;
        if ((a.a88 & 1u) && !(it->second.a88_pre & 1u)) ok = ActorBool(it->second.actor, kVtHidden, 0) && ok;
        if ((a.a88 & 2u) && !(it->second.a88_pre & 2u)) ok = ActorBool(it->second.actor, kVtSleep, 0) && ok;
        ++g_seat_shows;
        Log("[enemy] mirror id=%05X out of V3 truck 0x%06X: shown and woken again here (%s; seated %.1f s; a88 0x%X -> 0x%X%s)", id, it->second.truck, why,
            now > it->second.since ? (now - it->second.since) / 1000.0 : 0.0, a.a88, U32(it->second.actor + kActFlags88), ok ? "" : ", a call FAULTED");
    }
    g_seat_hid.erase(it);
    return true;
}
void SeatShowAll(const char* why, double now) {
    while (!g_seat_hid.empty()) SeatShowMirror(g_seat_hid.begin()->first, why, now);
}
bool ReleaseHeld() { return g_holding || InCutscene(); }
// The posts that give one mirror back: only while it is still the same in-use MULE actor AND still frozen by us (+0xEA4 bit 14:
// cleared means a game sequence took it: forgotten, nothing posted, as the lease's "taken"); Required{0} only when bit 13 was
// clear before our freeze (Occupied{0} keeps bit 13; a bit the game set is never cleared by us). True: the posts were made (and
// the weapon flag restored) now.
bool PostRelease(const Mirror& m, const char* why) {
    std::uint8_t in_use = 0;
    std::uint32_t id = 0;
    const bool same = mem::Q(m.actor) == A(kMuleVt) && mem::Rd(m.actor + kActInUse, in_use) && in_use && mem::Rd(m.actor + kActId, id) && id == m.id;
    const std::uint32_t before = same ? U32(m.actor + kMuleSeq) : 0;
    const bool ours = same && ((before >> 14) & 1u);
    if (ours) {
        Post(m.id, kOccupiedVt, 0);
        if (!m.bit13_pre) Post(m.id, kRequiredVt, 0);
        RestoreWeapon(m.id, m.actor);
    }
    // P2 (docs/enemy-sync.md 9): the host's copy is dead: hers is never given back alive (it would stand up in her world); hidden
    // and asleep here (Eligible then refuses it). Down: given back alive (it stands up until she returns; it is frozen lying again).
    const std::uint8_t defeat = same ? ecombat::GuestDefeat(m.id) : 0;
    if (same && erules::DeadRelease(defeat) == erules::ReleaseAs::hide_sleep) {
        HideDead(m.id, m.actor, "released dead");
    } else if (same && defeat) {
        Log("[enemy] mirror id=%05X released while %s on the host: given back alive here (frozen lying again when she returns)", m.id, erules::DefeatText(defeat));
    }
    ++g_gs.released;
    Log("[enemy] lease released id=%05X: %s (ea4 0x%X->0x%X)%s", m.id, why, before, same ? U32(m.actor + kMuleSeq) : 0,
        !same ? " (the actor is gone: forgotten)" : !ours ? " (bit 14 already clear: a game sequence took it; forgotten, nothing posted)" : "");
    return ours;
}
// Give one mirror back (only one we froze). During a cutscene hold: kept frozen in g_pending, released when the hold ends.
// True: posted now (not held, still ours).
bool ReleaseMirror(Mirror& m, const char* why) {
    if (!m.frozen) return false;
    DownUnhideMirror(m.id, "released");  // round 23 C: shown first (never released hidden)
    SeatShowMirror(m.id, "released", 0);  // round 28 T1: likewise
    g_down_held.erase(m.id);
    m.frozen = false;
    FreeSlot(m);
    if (ReleaseHeld()) {
        Mirror p = m;
        p.why = why;
        g_pending.push_back(p);
        PublishMirrors();  // still matched by the guards (it is still frozen)
        Log("[enemy] lease release id=%05X held: %s during a cutscene (posted when the hold ends)", m.id, why);
        return false;
    }
    PublishMirrors();  // the guards stop matching it at once
    return PostRelease(m, why);
}
void ReleaseAll(const char* why) {
    bool any = false;
    for (Mirror& m : g_mirrors) {
        if (m.frozen) ReleaseMirror(m, why), any = true;
        FreeSlot(m);
    }
    g_mirrors.clear();
    PublishMirrors();
    if (any) Log("[enemy] every mirror released (%s)%s", why, g_pending.empty() ? "" : "; the cutscene holds some");
}
// The held releases, once the cutscene hold ends (Tick).
void DrainPending() {
    if (g_pending.empty() || ReleaseHeld()) return;
    std::vector<Mirror> p;
    p.swap(g_pending);
    PublishMirrors();
    for (const Mirror& m : p) PostRelease(m, m.why);
    Log("[enemy] %zu held release(s) posted (the cutscene hold ended)", p.size());
}
// The far placement undone: every copy we moved and that is still unrealized here goes back to its own place.
void RestoreFar(const char* why) {
    if (g_far_orig.empty()) return;
    unsigned back = 0, gone = 0;
    for (const auto& kv : g_far_orig) {
        ActorNow a;
        if (!ReadActor(kv.second.actor, kv.first, a) || a.state != 0 || a.entity) {
            ++gone;
            continue;
        }
        if (ActorPlaceFar(kv.second.actor, kv.second.xf, false)) ++back;
        else ++gone, ++g_gs.far_faults;
    }
    Log("[enemy] far placement undone (%s): %u copies back at their own places, %u forgotten (realized or gone)", why, back, gone);
    g_far_orig.clear();
}
void GuestReset() {
    UnhideDead(true, "reset");
    UnhideHostHidden(true, "reset", 0);
    UnhideNotHolding(true, "reset", 0);
    SeatShowAll("reset", 0);  // round 28 T1
    g_nothold_n.clear();
    g_over20.clear();
    ReleaseAll("reset");
    RestoreFar("reset");
    g_tracks.clear();
    g_prx.clear();
    g_clock.Reset();
    g_local.clear();
    g_far_checks.clear();
    g_refused_once.clear();
}

// P1b (below): whether this game takes a peer's SEN1 entry (the host: only the guest's owned ones with their epoch; the guest:
// never one she owns), and the host's generation a guest frame names (the guest: a newer one resets her table).
bool AcceptPeerEntry(const ewire::EnemyFrame& f, const ewire::Enemy& e, double now);
bool AcceptPeerFrame(const ewire::EnemyFrame& f);
bool AcceptPeerPose(std::uint32_t id);
bool IsOwnedHere(std::uint32_t id);                 // the guest owns it (her copy runs its AI)
void OwnGuestTick(const TickIn& in, double now);  // the guest's owned enemies
// The peer's SEN1 / SEP1 (P1a: the guest; P1b: both, the host for the enemies the guest owns).
void GuestDrain(double now) {
    std::uint8_t buf[1100];
    std::size_t n = 0;
    std::uint32_t fgen = 0;
    static ewire::EnemyFrame f;
    while (link::PopCombatFrame(link::CombatFrame::enemies, buf, sizeof(buf), n, fgen)) {
        if (fgen != link::Generation()) continue;
        if (!ewire::DecodeEnemies(buf, n, f)) {
            ++g_gs.rx_bad;
            continue;
        }
        if (!AcceptPeerFrame(f)) continue;
        ++g_gs.rx_sen;
        const double sent = g_clock.Unwrap(f.sender_ms);
        g_clock.Note(sent, now);
        for (unsigned i = 0; i < f.n; ++i) {
            const ewire::Enemy& e = f.e[i];
            if (!AcceptPeerEntry(f, e, now)) continue;
            erules::Sample s;
            s.t = sent;
            std::memcpy(s.pos, e.pos, sizeof(s.pos));
            s.yaw = e.yaw;
            std::memcpy(s.vel, e.vel, sizeof(s.vel));
            s.state = e.state;
            s.phase = e.phase, s.flags = e.flags;  // round 23 B
            erules::Track& tr = g_tracks[e.actor_id];
            const std::uint8_t was = tr.Newest() ? ewire::SenDefeat(tr.Newest()->state) : 0;
            tr.Push(s, now);
            // round 23: the replay flush (its down outcome reached this game by the partner's SEN1)
            if (g_ecfg.enemy_replay_flush && erules::FlushOnDefeat(was, ewire::SenDefeat(e.state))) shots::FlushEnemyReplays(e.actor_id, "the partner's SEN1", true);
            if (ewire::DefeatOfState(e.state) && !(f.flags & ewire::kFromGuest)) ecombat::NoteSenDefeat(e.actor_id, ewire::DefeatOfState(e.state));
            ++g_gs.rx_entries;
        }
    }
    static ewire::EnemyPose p;
    while (link::PopCombatFrame(link::CombatFrame::poses, buf, sizeof(buf), n, fgen)) {
        if (fgen != link::Generation()) continue;
        if (!ewire::DecodeEnemyPose(buf, n, p)) {
            ++g_gs.sep_bad;
            continue;
        }
        ++g_gs.rx_sep;
        if (!AcceptPeerPose(p.actor_id)) continue;
        PoseRx& r = g_prx[p.actor_id];
        if (p.foreign_layout) {
            ++g_gs.sep_foreign;
            if (!r.foreign_logged) r.foreign_logged = true, Log("[enemy] pose refused id=%05X: layout %08X (not the shared human layout)", p.actor_id, p.joints.layout_hash);
            continue;
        }
        const double t = g_clock.Unwrap(p.sender_ms);
        if (t <= r.tb) continue;
        r.a = r.b, r.ta = r.tb;
        r.b = p.joints, r.tb = t;
        r.rx = now;
    }
}

Mirror* FindMirror(std::uint32_t id) {
    for (Mirror& m : g_mirrors)
        if (m.id == id) return &m;
    return nullptr;
}
std::uintptr_t LocalActor(std::uint32_t id) {
    for (const LocalEnemy& l : g_local)
        if (l.id == id) return l.actor;
    return 0;
}

void RefuseOnce(std::uint32_t id, const char* why) {
    ++g_gs.refused;
    int& r = g_refused_once[id];
    if (r || g_logged_refusals >= 40) return;
    r = 1;
    ++g_logged_refusals;
    Log("[enemy] mirror refused id=%05X: %s", id, why);
}

// A MULE seated in a vehicle: its entity's parent chain (+0x70, up to 4 levels) names a vehicle class (untested: refused).
bool InVehicle(std::uintptr_t entity) {
    std::uintptr_t p = entity;
    for (int d = 0; p && d < 4; ++d) {
        p = mem::Q(p + 0x70);
        if (p && shotw::ClassifyHit(mem::RttiName(mem::RttiOf(p))) == shotw::HitClass::vehicle) return true;
    }
    return false;
}
// Round 23 B: the parent chain first; if it does not reach a vehicle, the actor's ride fields (+0xE20 the vehicle actor id, +0x3C0 the
// seat), only with their byte checks. The fault-free reader; a read that fails on a live actor turns that test off for the run.
// Round 23 D: then the shared vehicles' truck rules (a vehicle of this game whose driver seat names this id; vehicle_mule_hold).
erules::VehTest VehicleOf(std::uint32_t id, std::uintptr_t actor, std::uintptr_t entity, std::uint32_t* e20, std::int32_t* seat) {
    std::uint32_t v = erules::kVehicleIdNone;
    std::int32_t s = -1;
    const bool chain = entity && InVehicle(entity);
    bool ride = false;
    if (!chain && actor && g_ride_ok && g_ecfg.enemy_mirror_vehicle) {
        if (mem::Rd(actor + kActVehId, v) && mem::Rd(actor + kActSeat, s)) {
            ride = true;
        } else {
            g_ride_ok = false;
            Log("[enemy] vehicle test: the read of actor %p +0xE20 / +0x3C0 FAILED: the ride-field test is off for the run (the parent chain stays)",
                reinterpret_cast<void*>(actor));
        }
    }
    if (e20) *e20 = v;
    if (seat) *seat = s;
    const bool driver = !chain && !(ride && erules::SeatedByRide(v, s)) && id && vshare::TruckDriverOf(id);
    return erules::VehicleTest(chain, ride, v, s, driver);
}
unsigned g_veh_lines = 0;
void NoteVehicle(std::uint32_t id, const char* who, erules::VehTest was, erules::VehTest now_t, std::uint32_t e20, std::int32_t seat) {
    if (g_veh_lines >= 24) return;
    ++g_veh_lines;
    if (now_t != erules::VehTest::none)
        Log("[enemy] vehicle id=%05X: %s copy is in a vehicle (test: %s; +0xE20 0x%X seat %d)", id, who, erules::VehTestText(now_t), e20, seat);
    else
        Log("[enemy] vehicle id=%05X: %s copy is out of its vehicle (was: %s)", id, who, erules::VehTestText(was));
}

void TryEnter(std::uint32_t id, const erules::Track& tr, double now, const double (&sam)[3], bool cutscene) {
    const erules::Sample* z = tr.Newest();
    if (!z || g_mirrors.size() >= kMaxMirrors) return;
    if (Dist3(z->pos, sam) > g_ecfg.enemy_mirror_radius) return;
    const std::uintptr_t actor = LocalActor(id);
    if (!actor) return;
    ActorNow a;
    if (!ReadActor(actor, id, a)) return;
    erules::EligibleIn e;
    e.fresh = now - tr.rx_ms <= erules::FreshMs(erules::Class::near10);
    e.realized = a.realized;
    e.dead = a.dead;
    e.hidden_or_asleep = a.hidden_or_asleep;
    e.sequence_only = a.seq_only;
    e.held_here = a.held;
    e.held_on_host = (z->state & ewire::kSeqHeld) != 0;
    e.cutscene = cutscene;
    // round 23 B (enemy_mirror_vehicle): both tests here, and the owner's SEN1 bit (off: the parent chain only, as before)
    e.in_vehicle = a.realized && (g_ecfg.enemy_mirror_vehicle ? VehicleOf(id, actor, a.entity, nullptr, nullptr) != erules::VehTest::none : InVehicle(a.entity));
    e.host_in_vehicle = g_ecfg.enemy_mirror_vehicle && (z->flags & ewire::kInVehicle) != 0;
    // round 28 T1 (vehicle_mule_v3): a host copy seated in a truck she follows is taken hidden (never placed, never left free to
    // board her copy)
    std::uint32_t v3_truck = 0;
    if (e.host_in_vehicle && !e.in_vehicle && truckv3::V3Live()) e.v3_seat = truckv3::V3SeatNear(z->pos, trucks::kV3SeatNearM, &v3_truck);
    e.owner_down = DownOn();  // round 23 C: no new mirror while the host's Sam is down (counted, not logged per id)
    // M0 (enemy_host_guard): the host's copy unloaded or hidden there, or its place far from ours: never a statue
    double pos0[3] = {};
    float yaw0 = 0;
    const bool have_pos0 = tr.At(g_clock.Render(now), pos0, &yaw0);
    if (g_ecfg.enemy_host_guard) {
        e.host_realized = (z->state & ewire::kRealized) != 0;
        e.host_hidden = (z->state & ewire::kHidden) != 0;
        e.entry_err_m = have_pos0 && a.have_xf ? Dist3(pos0, a.pos) : -1;
        e.entry_err_max_m = g_ecfg.enemy_entry_err_max;
        e.host_dist_m = g_have_hostp ? Dist3(z->pos, g_hostp) : -1;
        e.host_far_m = g_ecfg.enemy_host_far;
    }
    const erules::Refusal r = erules::Eligible(e);
    if (r == erules::Refusal::owner_down) return void(++g_ds.refused);
    if (r != erules::Refusal::ok) {
        if (r == erules::Refusal::host_unrealized || r == erules::Refusal::host_hidden || r == erules::Refusal::entry_far || r == erules::Refusal::host_far) {
            ++g_gs.host_refused;
            if (r == erules::Refusal::entry_far) {  // logged once, with the distance (not again by RefuseOnce)
                ++g_gs.refused;
                int& once = g_refused_once[id];
                if (!once && g_logged_refusals < 40) {
                    once = 1, ++g_logged_refusals;
                    Log("[enemy] mirror refused id=%05X: the host's place is %.1f m from ours (enemy_entry_err_max %.0f)", id, e.entry_err_m, e.entry_err_max_m);
                }
                return;
            }
        }
        if (r != erules::Refusal::not_realized && r != erules::Refusal::stale && r != erules::Refusal::dead) RefuseOnce(id, erules::RefusalText(r));
        return;
    }
    if (!g_sam_filter) return RefuseOnce(id, "the Sam damage filter is not in (a frozen copy could hurt her)");
    // the guards and the enemy rounds (review rv-safety 6): a frozen copy takes no bullet here (the drop at the bullet), its own fire
    // is dropped (the Enqueue), and with enemy_shots the host's rounds can be replayed from it (else every frozen MULE near her
    // would be harmless to her)
    if (!shots::MirrorGuardReady()) return RefuseOnce(id, "the shots' hooks are not in (the Enqueue or the drop at the bullet): a frozen copy could be shot here");
    if (g_ecfg.enemy_shots && !shots::EnemyReplayReady()) return RefuseOnce(id, "the enemy rounds cannot be replayed here (enemy_shots=1)");
    // freeze, then read it back
    const std::uint32_t ea4_pre = U32(actor + kMuleSeq);
    NoteWeaponPre(id, actor);
    if (!Post(id, kOccupiedVt, 1)) return RefuseOnce(id, "the post faulted");
    const std::uint32_t ea4 = U32(actor + kMuleSeq);
    if (!((ea4 >> 14) & 1u)) return RefuseOnce(id, "freeze did not take");
    Mirror m;
    m.id = id, m.actor = actor, m.entity = a.entity, m.sm = mem::Q(a.entity + 0xB8);
    m.frozen = true, m.t_frozen = now, m.ea4_entry = ea4;
    m.bit13_pre = ((ea4_pre >> 13) & 1u) != 0;
    double pos[3];
    float yaw = 0;
    double err = -1;
    if (tr.At(g_clock.Render(now), pos, &yaw)) {
        err = Dist3(pos, a.pos);
        const WorldTransform xf = XfFromYaw(a.xf, pos, yaw);
        if (!e.v3_seat && ActorXf(actor, kActorTeleport, xf)) m.last_tp = now, ++g_gs.teleports;  // (round 28 T1: seated: not placed)
    }
    m.v3_truck = v3_truck;
    g_mirrors.push_back(m);
    PublishMirrors();
    ++g_gs.on;
    if (e.v3_seat) ++g_seat_entries;
    Log("[enemy] mirror on id=%05X (dist %.1f m, err at entry %.2f m, ea4 0x%X)%s", id, Dist3(z->pos, sam), err, ea4,
        e.v3_seat ? " (seated in a V3 truck on the host: not placed, hidden here)" : "");
    if (e.v3_seat) SeatHideMirror(g_mirrors.back(), now);
}

// ---------------------------------------------------------------- round 23: the tie-up relay's pose hold (docs/enemy-sync.md 14)
struct HoldRec {
    bool on = false;
    double t0 = 0, until = 0, end = -1e18;
    int slot = -1;              // the slot switched to capture while held (-1 none)
    std::uintptr_t sm = 0;
    bool have_native = false;   // the copy's own pose captured while held (the blend's start)
    wire::JointFrame native;
};
std::map<std::uint32_t, HoldRec> g_hold;
// The copy's slot back to apply (its mode was capture while held): only while the mirror still has that slot for that model
// (a slot freed meanwhile, or given to another mirror, is never touched).
void HoldSlotBack(HoldRec& h, const Mirror& m) {
    if (h.slot >= 0) {
        if (m.slot == h.slot && m.sm == h.sm && h.sm && g_palloc.At(h.slot) == erules::PoseAlloc::kApply)
            realbody::SetEnemySlot(static_cast<unsigned>(h.slot), h.sm, realbody::EnemySlotMode::apply);
        h.slot = -1;
    }
}
// True: the copy is held now (no transform write, no pose push this tick).
bool HoldTick(Mirror& m, double now) {
    const auto it = g_hold.find(m.id);
    if (it == g_hold.end()) return false;
    HoldRec& h = it->second;
    if (h.on && now >= h.until) {
        h.on = false, h.end = now;
        HoldSlotBack(h, m);
        Log("[enemy] restrain hold id=%05X ended: timeout (%.1f s, no owner outcome): the copy re-posed with the owner's frames", m.id, (now - h.t0) / 1000.0);
    }
    if (!h.on) {
        HoldSlotBack(h, m);
        if (now - h.end >= erules::kRestrainBlendMs) g_hold.erase(it);
        return false;
    }
    // the copy's own animation plays: its slot captures it (the blend's start) instead of applying the owner's frames
    if (m.slot >= 0 && h.slot < 0 && g_palloc.At(m.slot) == erules::PoseAlloc::kApply &&
        realbody::SetEnemySlot(static_cast<unsigned>(m.slot), m.sm, realbody::EnemySlotMode::capture))
        h.slot = m.slot, h.sm = m.sm;
    double ms = 0;
    if (h.slot >= 0 && realbody::CaptureEnemyFrame(static_cast<unsigned>(h.slot), &h.native, &ms)) h.have_native = true;
    return true;
}
// The blend back: the owner's frame f mixed from the held native pose (250 ms), else f itself.
const wire::JointFrame& HoldBlend(std::uint32_t id, const wire::JointFrame& f) {
    const auto it = g_hold.find(id);
    if (it == g_hold.end() || it->second.on || !it->second.have_native) return f;
    static wire::JointFrame out;
    wire::BlendJoints(it->second.native, f, erules::BlendBack(g_tnow - it->second.end), out);
    return out;
}

void GivePose(Mirror& m, const erules::Track& tr, double render, bool posed_on_host) {
    auto it = g_prx.find(m.id);
    const bool have = it != g_prx.end() && it->second.tb > -1e17 && posed_on_host;
    if (!have) {
        if (m.slot >= 0 && g_tnow - (it != g_prx.end() ? it->second.rx : -1e18) > 1500) FreeSlot(m);
        ++g_gs.no_pose;
        return;
    }
    (void)tr;
    if (m.slot < 0) {
        // apply first (P1b: a capture of an enemy this game owns gives its slot up)
        bool stolen = false;
        const int k = m.sm ? g_palloc.Take(erules::PoseAlloc::kApply, static_cast<unsigned>(g_ecfg.enemy_pose_count), &stolen) : -1;
        if (k >= 0 && stolen)
            for (unsigned c = 0; c < erules::kPoseSlots; ++c)
                if (g_pslot_real[c] == k) g_pslot_real[c] = -1, g_pslot_sm[c] = 0;  // the slot is the apply's now (its mode set below)
        if (k >= 0 && realbody::SetEnemySlot(static_cast<unsigned>(k), m.sm, realbody::EnemySlotMode::apply)) m.slot = k;
        else if (k >= 0) g_palloc.Free(k);
        if (m.slot < 0) {
            ++g_gs.no_pose;
            return;
        }
    }
    const PoseRx& r = it->second;
    static wire::JointFrame out;
    m.posed_once = true;  // round 23 C (DownIn.posed)
    if (r.ta > -1e17 && render > r.ta && render < r.tb) {
        wire::BlendJoints(r.a, r.b, static_cast<float>((render - r.ta) / (r.tb - r.ta)), out);
        realbody::PushEnemyFrame(static_cast<unsigned>(m.slot), HoldBlend(m.id, out));
    } else {
        realbody::PushEnemyFrame(static_cast<unsigned>(m.slot), HoldBlend(m.id, render <= r.ta && r.ta > -1e17 ? r.a : r.b));
    }
}

// Round 23 B (docs/enemy-sync.md 14.8): one mirror's full diagnostics line (log only): `head` "worst", "first over 20 m" or "not
// holding". The owner's side from its newest SEN1 sample (state, phase, entry flags, in vehicle, a sequence hold there, the frame's
// age), ours read now (realized, bit 14, the in-vehicle test, the entity's parent, the pose).
void MirrorDiag(const char* head, const Mirror& m, double err, double now) {
    const char* owner = g_role == 1 ? "guest" : "host";
    const auto t = g_tracks.find(m.id);
    const erules::Sample* z = t != g_tracks.end() ? t->second.Newest() : nullptr;
    const std::uint8_t st = z ? z->state : 0, ph = z ? z->phase : erules::kPhaseUnknown, fl = z ? z->flags : 0;
    ActorNow a;
    const bool same = ReadActor(m.actor, m.id, a);
    const std::uintptr_t parent = same && a.entity ? mem::Q(a.entity + 0x70) : 0;
    const char* pn = parent ? mem::RttiName(mem::RttiOf(parent)) : nullptr;
    char anim[96];
    const auto pr = g_prx.find(m.id);
    const double pose_age = pr != g_prx.end() && pr->second.rx > -1e17 ? now - pr->second.rx : -1;
    if (g_hold.count(m.id) && g_hold[m.id].on) std::snprintf(anim, sizeof(anim), "its own (a restrain hold)");
    else if (m.slot >= 0) std::snprintf(anim, sizeof(anim), "the %s's pose in slot %d (newest %.0f ms ago)", owner, m.slot, pose_age);
    else std::snprintf(anim, sizeof(anim), "its own idle (no pose slot; the %s poses it: %d)", owner, (st & ewire::kPosed) ? 1 : 0);
    Log("[enemy] mirror %s id=%05X err %.1f m (teleports %u in 5 s, held %.1f s): %s state 0x%02X (realized %d hidden %d dead %d defeat %u), phase %u (%s), bits "
        "0x%02X, %s in vehicle %d, %s seq %d (frame age %.0f ms); here realized %d, held %d, in vehicle %d (%s), parent %s, anim %s; ea4 0x%X a88 0x%X",
        head, m.id, err, m.tp5, (now - m.t_frozen) / 1000.0, owner, st, (st & ewire::kRealized) ? 1 : 0, (st & ewire::kHidden) ? 1 : 0, (st & ewire::kDead) ? 1 : 0,
        ewire::DefeatOfState(st), ph, erules::PhaseText(ph), fl, owner, (fl & ewire::kInVehicle) ? 1 : 0, owner, (st & ewire::kSeqHeld) ? 1 : 0,
        z ? now - t->second.rx_ms : -1.0, same && a.realized ? 1 : 0, same && a.held ? 1 : 0, m.veh != erules::VehTest::none ? 1 : 0, erules::VehTestText(m.veh),
        parent ? (pn ? pn : "?") : "none", anim, same ? a.ea4 : 0, same ? a.a88 : 0);
}
// One frozen mirror driven to its owner's place (either game: P1b's host follows the guest's). True: it does not hold that place
// (erules::NotHolding, enemy_mirror_hold_check; the error measured before this tick's writes): nothing written this tick.
bool DriveMirror(Mirror& m, const ActorNow& a, const double (&pos)[3], float yaw, double now) {
    const double err = Dist3(pos, a.pos);
    const double yerr = erules::YawDiffDeg(yaw, YawOfXf(a.xf));
    if (now >= m.next_err) m.next_err = now + 500, g_err_win.push_back(err), m.err = err;
    if (err > m.err_max5) m.err_max5 = err;
    // round 23 B: the first error over 20 m of each id (one full line), then the not-holding rule
    if (erules::MirrorFirstFull(err, g_over20.count(m.id) != 0)) {
        g_over20.insert(m.id);
        ++g_mb.over20;
        MirrorDiag("first over 20 m", m, err, now);
    }
    if (erules::NotHolding(g_ecfg.enemy_mirror_hold_check, err, m.tp.Within(now, erules::kNotHoldWindowMs))) {
        if (!m.own || !m.nothold_logged) MirrorDiag("not holding", m, err, now);
        if (m.own) m.nothold_logged = true;  // the host's follower: logged only (never given back here: its owner is her game)
        else return true;
    }
    erules::Drive d = erules::Band(err, yerr, now, m.last_tp);
    if (g_tp_cadence == 3 && now - m.last_tp >= 500) d = erules::Drive::teleport;
    const WorldTransform xf = XfFromYaw(a.xf, pos, yaw);
    if (d == erules::Drive::set) {
        if (ActorXf(m.actor, kVtSet, xf) && EntityXf(m.entity, xf)) ++g_gs.sets, ++g_sets_win;
        else ++g_gs.set_faults;
    } else if (d == erules::Drive::teleport) {
        if (ActorXf(m.actor, kActorTeleport, xf)) ++g_gs.teleports, ++g_tp_win, m.last_tp = now, m.tp.Note(now), ++m.tp5;
        else ++g_gs.set_faults;
    }
    return false;
}
// The 5 s mirrors line (either game).
void MirrorLine(double now) {
    if (now < g_gs_next) return;
    g_gs_next = now + 5000;
    unsigned frozen = 0, posed = 0;
    for (const Mirror& m : g_mirrors) frozen += m.frozen ? 1 : 0, posed += m.slot >= 0 ? 1 : 0;
    unsigned applies = 0, nof = 0;
    for (unsigned k = 0; k < erules::kPoseSlots; ++k) {
        const realbody::EnemySlotStats s = realbody::GetEnemySlotStats(k, true);
        if (g_palloc.At(static_cast<int>(k)) == erules::PoseAlloc::kApply) applies += s.applies, nof += s.no_frame;
    }
    if (!g_mirrors.empty() || !g_err_win.empty() || g_gs.far_placed) {
        Log("[enemy] mirrors: n=%zu frozen=%u err p50 %.2f p95 %.2f max %.2f m (%zu samples); set %.0f/s teleports %u; poses n=%u (%.1f applies/s each, no pose %u, "
            "no frame %u); far placed %u (back-drift p50 %.2f p95 %.2f m, %zu checked); tracks %zu",
            g_mirrors.size(), frozen, Pct(g_err_win, 0.5), Pct(g_err_win, 0.95), g_err_win.empty() ? 0.0 : *std::max_element(g_err_win.begin(), g_err_win.end()),
            g_err_win.size(), g_sets_win / 5.0, g_tp_win, posed, posed ? applies / 5.0 / posed : 0.0, g_gs.no_pose, nof, g_gs.far_placed, Pct(g_drift, 0.5),
            Pct(g_drift, 0.95), g_drift.size(), g_tracks.size());
    }
    // round 23 B: the worst mirror while the window's p95 is above 5 m (log only)
    if (!g_err_win.empty() && erules::MirrorWorstDue(Pct(g_err_win, 0.95))) {
        const Mirror* w = nullptr;
        for (const Mirror& m : g_mirrors)
            if (m.frozen && (!w || m.err_max5 > w->err_max5)) w = &m;
        if (w) ++g_mb.worst, MirrorDiag("worst", *w, w->err_max5, now);
    }
    for (Mirror& m : g_mirrors) m.err_max5 = 0, m.tp5 = 0;
    g_err_30.insert(g_err_30.end(), g_err_win.begin(), g_err_win.end());
    if (now >= g_sum_next) {
        const bool first = g_sum_next == 0;
        g_sum_next = now + erules::kMirrorSummaryMs;
        if (!first && (!g_err_30.empty() || !g_nothold.empty() || g_mb.nothold || g_mb.vehicle))
            Log("[enemy] mirror error p50 %.2f p95 %.2f max %.2f m (n %zu samples in 30 s, %zu mirrors now; worst lines %u, first over 20 m %u; not holding: %zu hidden "
                "now, %u in all, %u retries; vehicle releases %u)",
                Pct(g_err_30, 0.5), Pct(g_err_30, 0.95), g_err_30.empty() ? 0.0 : *std::max_element(g_err_30.begin(), g_err_30.end()), g_err_30.size(),
                g_mirrors.size(), g_mb.worst, g_mb.over20, g_nothold.size(), g_mb.nothold, g_mb.retries, g_mb.vehicle);
        g_err_30.clear();
    }
    g_err_win.clear(), g_drift.clear();
    g_sets_win = g_tp_win = 0;
    g_gs.no_pose = 0;
}

bool CampOwnedHere(std::uintptr_t actor);  // round 25 V1 (below)
std::set<std::uint32_t> g_v1_logged;
void GuestTick(const TickIn& in, double now, bool off, const char* off_why) {
    GuestDrain(now);
    double sam[3] = {};
    const bool have_sam = in.local_ok && LocalPlace(in.local, sam);
    g_have_hostp = in.body && LocalPlace(in.body, g_hostp);  // M0's quick path: the host's Sam
    const bool cutscene = InCutscene();
    const int cut = g_cut_step;  // stepped by Tick
    if (cut == 2) ReleaseAll("cutscene");
    if (g_holding) return;  // nothing driven, posed or posted
    if (!g_dead_hidden.empty()) UnhideDead(off, off ? off_why : "the host's is alive again");
    if (!g_host_hid.empty()) UnhideHostHidden(off, off ? off_why : "", now);
    // round 23 B: the 5 s retry (and the off unhide); round 23 review: not while the host's Sam is down (shown then, the entry would
    // refuse it and leave a live MULE beside her)
    if (!g_nothold.empty()) {
        if (erules::NotHoldUnhideNow(false, DownOn())) {
            g_down_nothold_logged = false;
            UnhideNotHolding(off, off ? off_why : "", now);
        } else if (!g_down_nothold_logged) {
            g_down_nothold_logged = true;
            Log("[enemy] down hold: %zu not-holding copies stay hidden and asleep here until the host's Sam is back (no 5 s retry meanwhile)", g_nothold.size());
        }
    }
    if (off || !have_sam) {
        // round 23 review: sync off (the host's reload after his death turns the gate off) or no local Sam while the host's Sam is
        // down: every frozen copy kept frozen as it is (held, or hidden by the hold), never given back alive
        if (erules::DownKeepsOff(g_ecfg.enemy_down_hold, g_peer_down, true)) {
            DownKeep(off ? off_why : "no local Sam");
            return;
        }
        g_down_kept = false;
        if (!g_mirrors.empty()) ReleaseAll(off ? off_why : "no local Sam");
        if (off) RestoreFar(off_why);
        return;
    }
    g_down_kept = false;
    if (now >= g_local_next) {
        g_local_next = now + 500;
        WalkEnemies(g_local);
    }
    const double render = g_clock.Render(now);
    // lease and drive
    bool changed = false;
    for (Mirror& m : g_mirrors) {
        if (!m.frozen) continue;
        const auto it = g_tracks.find(m.id);
        ActorNow a;
        const bool same = ReadActor(m.actor, m.id, a);
        erules::LeaseIn l;
        l.frozen_by_us = true;
        l.bit14_now = same && a.held;
        l.realized_here = same && a.realized && a.entity == m.entity;
        // M0 (enemy_host_guard): the newest SEN1 sample's host bits (P1b: an own back with no host frame yet keeps the defaults)
        if (g_ecfg.enemy_host_guard && it != g_tracks.end() && it->second.Newest() && !IsOwnedHere(m.id)) {
            const std::uint8_t st = it->second.Newest()->state;
            l.host_guard = true;
            l.host_realized = (st & ewire::kRealized) != 0;
            l.host_hidden = (st & ewire::kHidden) != 0;
            l.host_far_m = g_ecfg.enemy_host_far;
            if (g_have_hostp && it->second.Newest()) l.host_dist_m = Dist3(it->second.Newest()->pos, g_hostp);
        }
        // (P1b: a mirror frozen again on an OWN back has no host frame yet: its silence clock starts at the freeze)
        l.frame_age_ms = now - (std::max)(it != g_tracks.end() ? it->second.rx_ms : -1e18, m.t_frozen);
        double pos[3] = {};
        float yaw = 0;
        const bool have_target = it != g_tracks.end() && it->second.At(render, pos, &yaw);
        l.dist_m = have_target ? Dist3(pos, sam) : 0;
        l.cls = erules::ClassOf(l.dist_m, g_ecfg.enemy_mirror_radius);
        l.radius_m = g_ecfg.enemy_mirror_radius;
        // round 23 B (enemy_mirror_vehicle): our copy tested every 1 s, the owner's by its newest SEN1 entry
        if (g_ecfg.enemy_mirror_vehicle) {
            if (same && now >= m.veh_next) {
                m.veh_next = now + erules::kVehicleCheckMs;
                m.veh = a.realized ? VehicleOf(m.id, m.actor, a.entity, &m.veh_e20, &m.veh_seat) : erules::VehTest::none;
            }
            l.vehicle_check = true;
            l.in_vehicle_here = m.veh != erules::VehTest::none;
            l.in_vehicle_host = it != g_tracks.end() && it->second.Newest() && (it->second.Newest()->flags & ewire::kInVehicle);
            // round 28 T1 (vehicle_mule_v3): its host copy in a vehicle that is a truck she follows (within 5 m of the host's place)
            if (l.in_vehicle_host && !l.in_vehicle_here && truckv3::V3Live()) {
                std::uint32_t truck = 0;
                l.v3_live = true;
                l.v3_seat = truckv3::V3SeatNear(it->second.Newest()->pos, trucks::kV3SeatNearM, &truck);
                if (l.v3_seat) m.v3_truck = truck, m.v3_unseated_since = 0;
                else if (m.v3_unseated_since == 0) m.v3_unseated_since = now;
                l.v3_unseated_ms = l.v3_seat ? 0 : now - m.v3_unseated_since;
            } else {
                m.v3_unseated_since = 0;
            }
            // round 25 V1 (enemy_single_hold_vehicle): while a member of its camp is hers as a single take, its in-vehicle release waits
            if (g_ecfg.enemy_single_hold_vehicle && (l.in_vehicle_here || l.in_vehicle_host) && CampOwnedHere(m.actor)) {
                l.hold_vehicle_single = true;
                if (g_v1_logged.insert(m.id).second)
                    Log("[enemy] mirror id=%05X in a vehicle: release held (V1: a member of its camp is hers alone; frozen at the host's frames until that ends)", m.id);
            }
        }
        const erules::LeaseVerdict v = erules::Lease(l);
        if (v == erules::LeaseVerdict::forget_taken) {
            ++g_gs.taken;
            m.frozen = false;
            changed = true;
            FreeSlot(m);
            SeatShowMirror(m.id, "taken by a game sequence", now);  // round 28 T1: never left hidden
            Log("[enemy] lease released id=%05X: taken (a game sequence cleared bit 14: forgotten, nothing posted; ea4 0x%X)", m.id, a.ea4);
            continue;
        }
        // round 23 C (enemy_down_hold; docs/enemy-sync.md 14.9): the host's Sam is down: near ones held frozen in their last pose (no
        // place, no pose, no posts), never posed ones and far down ones hidden here (kept frozen), never given back alive
        if (g_peer_down && !IsOwnedHere(m.id)) {
            erules::DownIn d;
            d.posed = m.slot >= 0 || m.posed_once;
            d.hidden_here = same && (a.a88 & 3u) && !g_down_hid.count(m.id);
            d.defeat = ecombat::GuestDefeat(m.id);
            d.dist_m = same && a.have_xf ? Dist3(a.pos, sam) : -1;
            d.radius_m = g_ecfg.enemy_down_hold_radius;
            const erules::DownAct act = erules::DownLease(g_ecfg.enemy_down_hold, true, v, d);
            if (act == erules::DownAct::hold) {
                if (g_down_held.insert(m.id).second) ++g_ds.holds;
                continue;
            }
            if (act == erules::DownAct::hide) {
                g_down_held.erase(m.id);
                if (same) DownHideMirror(m.id, m.actor, d.dist_m >= 0 && d.dist_m > d.radius_m ? "far and down on the host while the host's Sam is down" : "never posed while the host's Sam is down");
                continue;
            }
        }
        g_down_held.erase(m.id);
        DownUnhideMirror(m.id, "the usual rules again");  // (only one we hid: a far standing one, or the hold turned off)
        // round 28 T1: seated in the truck she follows: hidden and asleep, nothing written; out of it: shown first
        if (v == erules::LeaseVerdict::seated) {
            if (same) SeatHideMirror(m, now);
            continue;
        }
        SeatShowMirror(m.id, l.in_vehicle_host ? "no truck of hers near its host copy" : "its host copy is out of the vehicle", now);
        if (erules::Releases(v)) {
            if (v == erules::LeaseVerdict::release_host_unrealized || v == erules::LeaseVerdict::release_host_hidden || v == erules::LeaseVerdict::release_host_far)
                ++g_gs.host_released;
            if (v == erules::LeaseVerdict::release_vehicle) {
                ++g_mb.vehicle;
                Log("[enemy] mirror id=%05X in a vehicle (%s; our test: %s, +0xE20 0x%X seat %d): released to its own game until vehicles are synced", m.id,
                    erules::VehicleSideText(l.in_vehicle_here, l.in_vehicle_host), erules::VehTestText(m.veh), m.veh_e20, m.veh_seat);
            }
            const std::uintptr_t actor = m.actor;
            const std::uint32_t id = m.id;
            // M0's hide (enemy_host_hide): only once the release really posted (its weapon flag restored first); a release a
            // cutscene holds is given back alive later (review r20 enemy 3: hidden while still frozen, it could never fire again)
            const bool posted = ReleaseMirror(m, erules::LeaseText(v));
            if (erules::HideOnRelease(v, g_ecfg.enemy_host_hide, posted) && same) HideHostHidden(id, actor);
            continue;
        }
        if (!have_target || !a.have_xf) continue;
        if (HoldTick(m, now)) continue;  // round 23: a tie-up shown by the copy's own animation (no writes while held)
        if (DriveMirror(m, a, pos, yaw, now)) {
            // round 23 B: not holding the host's place: given back, then hidden and asleep (M0's hide, never a statue), retried in 5 s
            ++g_mb.nothold;
            const std::uintptr_t actor = m.actor;
            const std::uint32_t id = m.id;
            const bool posted = ReleaseMirror(m, "not holding (the host's place, after 3 teleports in 2 s)");
            if (posted && same) HideNotHolding(id, actor, now);
            continue;
        }
        GivePose(m, it->second, render, it->second.Newest() && (it->second.Newest()->state & ewire::kPosed));
    }
    if (changed) PublishMirrors();
    g_mirrors.erase(std::remove_if(g_mirrors.begin(), g_mirrors.end(), [](const Mirror& m) { return !m.frozen; }), g_mirrors.end());
    OwnGuestTick(in, now);  // P1b: the enemies she owns (SEN1, SEP1, shooters, damage)
    // entry (a few a tick)
    unsigned tries = 0;
    for (const auto& kv : g_tracks) {
        if (tries >= 4) break;
        if (FindMirror(kv.first) || IsOwnedHere(kv.first) || g_nothold.count(kv.first)) continue;  // (round 23 B: hidden as not holding)
        const erules::Sample* z = kv.second.Newest();
        if (!z || now - kv.second.rx_ms > erules::FreshMs(erules::Class::near10) || Dist3(z->pos, sam) > g_ecfg.enemy_mirror_radius) continue;
        ++tries;
        TryEnter(kv.first, kv.second, now, sam, cutscene);
    }
    // the copies out of her world (2 Hz): placed at the host's places
    if (g_ecfg.enemy_place_far && now >= g_far_next) {
        g_far_next = now + 500;
        unsigned placed = 0;
        for (const auto& kv : g_tracks) {
            if (placed >= 16) break;
            const erules::Sample* z = kv.second.Newest();
            if (!z || now - kv.second.rx_ms > 1500 || Dist3(z->pos, sam) > kFarRadiusM || FindMirror(kv.first) || IsOwnedHere(kv.first)) continue;
            const std::uintptr_t actor = LocalActor(kv.first);
            ActorNow a;
            if (!actor || !ReadActor(actor, kv.first, a) || a.state != 0 || a.entity || !a.have_xf || a.held || a.seq_only) continue;
            double pos[3];
            float yaw = 0;
            if (!kv.second.At(render, pos, &yaw) || Dist3(pos, a.pos) < 1.0) continue;
            if (!g_far_orig.count(kv.first)) g_far_orig[kv.first] = FarOrig{actor, a.xf};  // its own place, once
            if (ActorPlaceFar(actor, XfFromYaw(a.xf, pos, yaw), g_place_variant == 1 && g_varb_ok)) {
                ++g_gs.far_placed, ++placed;
                if (g_far_checks.size() < 64) g_far_checks.push_back(FarCheck{kv.first, actor, now + 1000});
            } else {
                ++g_gs.far_faults;
            }
        }
        // a copy realized since (or gone) is the game's again: never put back
        for (auto it = g_far_orig.begin(); it != g_far_orig.end();) {
            ActorNow a;
            if (!ReadActor(it->second.actor, it->first, a) || a.state != 0 || a.entity) it = g_far_orig.erase(it);
            else ++it;
        }
        for (std::size_t i = 0; i < g_far_checks.size();) {
            const FarCheck c = g_far_checks[i];
            if (now < c.due) {
                ++i;
                continue;
            }
            g_far_checks.erase(g_far_checks.begin() + static_cast<std::ptrdiff_t>(i));
            const auto it = g_tracks.find(c.id);
            ActorNow a;
            double pos[3];
            float yaw = 0;
            if (it != g_tracks.end() && ReadActor(c.actor, c.id, a) && a.have_xf && it->second.At(render, pos, &yaw)) g_drift.push_back(Dist3(pos, a.pos));
        }
    }
    MirrorLine(now);  // the 5 s line
}

// ====================================================================================================================================
// P1b: ownership by proximity (docs/enemy-sync.md 10). The host decides (erules::HostDecide at 2 Hz); a hand-over freezes the old
// owner's copy first, then the new owner unfreezes its own (SCB1 OWN / OWNACK on the reliable channel 6).
// ====================================================================================================================================
constexpr std::uintptr_t kSetPhase = 0x142ACF630;        // SetPhase(actor, phase): +0x110 exact (DsMuleSetPhase1's)
constexpr std::uintptr_t kRisePhase = 0x142ACE9D0;       // RisePhase(actor, phase): max(old, new) (DsMuleRisePhase1's)
constexpr std::uintptr_t kGetPhaseRead = 0x142ECFF7D;    // DsMuleGetPhase0's read of +0x110
constexpr std::uintptr_t kCampLookup = 0x142AF93F0;      // the MULE manager's camp (CP) lookup: +0xD8 count, +0xE0 the CP actor ids
constexpr std::uintptr_t kCampPhaseSite = 0x142B649DD;   // MuleCP_ForceSetCPPhase: the CP's phase machine at +0x208 (phase +0x210)
constexpr std::uintptr_t kCampMembersSite = 0x142B37FD2; // the phase push: the members +0x4C0 count / +0x4C8 DSMuleGameActor*[]
constexpr std::uintptr_t kMuleMgr = 0x147BF09D8;
constexpr std::uintptr_t kActPhase = 0x110, kCpPhase = 0x210, kCpCount = 0x4C0, kCpMembers = 0x4C8, kMgrCpCount = 0xD8, kMgrCpIds = 0xE0;
using FnPhase = void (*)(std::uintptr_t actor, std::int32_t phase);

bool g_own_want = false;    // enemy_owner=1
bool g_own_ok = false;      // its byte checks
bool g_cp_ok = false;       // the camp group's checks (optional: the camp phase unknown without it)
bool g_own_switch = true;   // enemy_sync 8 0 / 1
bool g_own_on = false;      // live: both beacons carry kPartCamp, mirroring on
struct OwnStats {
    unsigned takes = 0, backs = 0, forced = 0, refused = 0, no_owner = 0, two_thinking = 0, sen_out = 0, stale = 0, taken_here = 0, given_here = 0,
             refused_here = 0, resets = 0;
} g_os;
std::uint32_t g_own_seq = 0;
std::vector<std::vector<std::uint8_t>> g_own_in, g_own_out;
double g_own_walk_next = 0, g_own_log_next = 0, g_hfar_next = 0, g_gsen_next = 0, g_gwalk_next = 0, g_cp_next = 0;
std::vector<double> g_change_times, g_refuse_times;
bool g_guest_cut = false;  // her newest frame says she is in a cutscene
bool g_cp_logged = false;
// P1c (round 21, docs/enemy-sync.md 13): camps change owner as a whole. A camp's key: kCampCp | the CP actor id, or kCampLoose | the
// MULE's id (a MULE in no camp list: a camp of one, P1b's distances).
constexpr std::uint64_t kCampCp = 1ull << 32, kCampLoose = 2ull << 32;
// A member actor -> its CP camp and that camp's phase (CampWalk, 2 Hz). Kept kCampKeyKeepMs after the last walk that listed it: a
// partial or failed walk (a CP list emptied while loading) never turns a camp into loose ones (review rv-gameplay 12)
struct CampSeen {
    std::uint64_t key = 0;
    std::uint8_t phase = erules::kPhaseUnknown;
    double t = 0;
    std::uintptr_t cp = 0;  // the CP actor (round 24: its phase read live by CampViewOf)
};
std::map<std::uintptr_t, CampSeen> g_camp_of;
unsigned g_camp_conflicts = 0;
struct CampRec {
    double hold_until = -1e18, kept_next = 0, t_change = -1e18;
    double close_since = -1;  // (round 27 M4, take first) she within its engage distance without a take since (-1 not)
    bool refused = false, force_take = false;
    erules::CampIn last;
    erules::OwnVerdict v;
    unsigned n = 0, hers = 0;
};
std::map<std::uint64_t, CampRec> g_camps;
// Round 27 M4, take first (erules::CampTakeFirst; enemy_target_pick bit 64): the members of the camps it holds now, rebuilt at
// every 2 Hz camp walk (empty when no walk ran within kTakeFirstStaleMs)
struct TakeFirstRec {
    std::uint32_t cp = 0;
    double d_h = -1, d_g = -1;
};
std::map<std::uint32_t, TakeFirstRec> g_take_first;
double g_take_first_at = -1e18;
constexpr double kTakeFirstStaleMs = 1500.0;
double g_camp_last_take = -1e18;
std::uint64_t g_cnt_key = 0;  // the "no take" line's nearest camp, its reason
const char* g_cnt_why = nullptr;
double g_cnt_next = 0;
bool g_camp_carry = true;     // enemy_owner_outcomes
bool g_follow_guard = true;   // enemy_follow_guard
bool g_relay_want = true;     // enemy_owner_hits
struct CarryWait {
    std::uint32_t id = 0;
    std::uintptr_t actor = 0;
    int k = 0;
    double until = 0;
    std::uint8_t d = 0;  // her defeat (erules::Defeat)
};
// outcomes deferred (a hit still queued on the copy; the host's game saving; a cutscene holding the copy's return: review
// rv-correctness R4 / R5) and the carried ones read back a second later (spec 6.2; review rv-correctness R10)
std::vector<CarryWait> g_carry, g_readback;
unsigned g_carried = 0, g_carry_failed = 0, g_carry_down = 0, g_guard_hidden = 0;
struct FollowHid {
    std::uintptr_t actor = 0;
    bool pre = false;  // hidden before us
};
std::map<std::uint32_t, FollowHid> g_fhid;      // the host's followers the guard hid
std::map<std::uint32_t, double> g_fbad_since;   // her frames say unrealized / held since
// Members out of their camp's takes (her split refusals): id -> until (erules::kCampNoTakeWorld: this world; review rv-gameplay 1)
std::map<std::uint32_t, double> g_no_take;
unsigned g_stragglers = 0;
// her body's last place (held erules::kGuestPlaceHoldMs when it is missing: review rv-gameplay 14)
double g_guest_last[3] = {}, g_guest_last_t = -1e18;
bool NoTake(std::uint32_t id, double now) {
    const auto it = g_no_take.find(id);
    if (it == g_no_take.end()) return false;
    if (it->second < 0 || now < it->second) return true;
    g_no_take.erase(it);
    return false;
}
std::uint64_t CampKeyOf(std::uint32_t id, std::uintptr_t actor) {
    const auto it = g_camp_of.find(actor);
    return it != g_camp_of.end() ? it->second.key : kCampLoose | id;
}
const char* CampName(std::uint64_t key) {
    static char b[4][24];
    static unsigned i = 0;
    char* s = b[i++ & 3];
    std::snprintf(s, 24, "%s %05X", (key >> 32) == 1 ? "cp" : "loose", static_cast<unsigned>(key & 0xFFFFFFFFu));
    return s;
}
// Round 24 (the host: enemy_own <id> 3 / 4; docs/enemy-sync.md 14.17; real games too since round 26, 14.19): ONE member handed to her game alone, the rest of
// its camp left with its owner. While it is hers its camp is held out of the camp decisions (CampDecide, the stragglers, the loose
// rule), so nothing pulls the rest over or takes it back: only enemy_own <id> 4 (or 1), her refusal, the OWNACK timeout, a vehicle
// or a reset end it (SingleEnd at its return; the camp is decided again after the usual 15 s hold).
struct SingleRec {
    std::uint64_t key = 0;
    double t = 0;
};
std::map<std::uint32_t, SingleRec> g_single;  // id -> its camp at the take
struct SingleReq {
    std::uint32_t id = 0;
    bool to_guest = true;
    double held_until = 0;  // round 25 R1: a RULE-B hold's deadline (0: not held yet)
};
// round 25 R1 (enemy_single_rule): the MULEs that were hers earlier in this world (singles handed over), for RULE-W
std::set<std::uint32_t> g_was_hers;
constexpr double kRuleBHoldMs = 20000.0;
std::vector<SingleReq> g_single_req;  // enemy_own 3 / 4: done at the next walk
double g_single_line_next = 0;
bool SingleCamp(std::uint64_t key) {
    for (const auto& kv : g_single)
        if (kv.second.key == key) return true;
    return false;
}
void SingleEnd(std::uint32_t id, const char* how) {
    const auto it = g_single.find(id);
    if (it == g_single.end()) return;
    CampRec& cr = g_camps[it->second.key];
    cr.hold_until = g_tnow + erules::kCampHoldMs, cr.t_change = g_tnow, cr.force_take = false;
    g_camp_last_take = g_tnow;
    Log("[enemy] single OWN id=%05X back here (%s; hers %.1f s): its camp %s is decided again after the %.0f s hold", id, how, (g_tnow - it->second.t) / 1000.0,
        CampName(it->second.key), erules::kCampHoldMs / 1000.0);
    g_single.erase(it);
}
std::uint32_t g_gsen_seq = 0;

struct HostRec {
    erules::HostOwn e;
    std::uintptr_t actor = 0;
    bool far_follow = false;       // the host's copy is unrealized: placed at her frames (2 Hz)
    double t_rx = -1e18;           // her newest accepted frame for it (or the OWNACK)
    std::uint8_t g_phase = erules::kPhaseUnknown, g_state = 0;
    bool g_damaged = false;
    bool damaged = false, have_base = false;
    float base_h = 0, base_s = 0;
    bool forced = false;           // enemy_own <id> 1, or a game sequence took the host's copy
    double d_g = -1, d_h = -1;
    std::uint8_t phase = erules::kPhaseUnknown, cp = erules::kPhaseUnknown;
    double bad_since = -1;         // her fresh frames say unrealized or held there since (< 0: not)
};
std::map<std::uint32_t, HostRec> g_hown;
struct GuestRec {
    erules::GuestOwn g;
    std::uintptr_t actor = 0, entity = 0;
    double t_taken = 0;
    bool have_base = false, damaged = false;
    float base_h = 0, base_s = 0;
    double last_pos[3] = {}, last_t = -1e18;
    float vel[3] = {};
    bool have_pos = false;
    std::uint8_t last_phase = 0xFF;  // logged on change
    erules::VehTest veh = erules::VehTest::none;  // round 23 B: in a vehicle (every 1 s; her SEN1 kInVehicle)
    double veh_next = 0;
    bool alone = false;  // round 26: taken here as a single take (eprobe::TakeAlone; V1 holds its camp's in-vehicle release)
};
std::map<std::uint32_t, GuestRec> g_gown;
unsigned g_drain_forced = 0;  // round 26: the forced takes (OWN to her, reason 7) in the batch OwnDrain is handling now

std::uint8_t PhaseOf(std::uintptr_t actor) {
    if (!g_own_ok || !actor) return erules::kPhaseUnknown;
    std::int32_t p = -1;
    return mem::Rd(actor + kActPhase, p) && p >= 0 && p <= erules::kPhaseMax ? static_cast<std::uint8_t>(p) : erules::kPhaseUnknown;
}
bool CallPhase(std::uintptr_t fn, std::uintptr_t actor, int phase) noexcept {
    __try {
        reinterpret_cast<FnPhase>(A(fn))(actor, phase);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
std::uint8_t OwnEpochByte(std::uint32_t id) {
    const auto it = g_hown.find(id);
    return static_cast<std::uint8_t>(it != g_hown.end() ? it->second.e.epoch & 0xFF : 0);
}
bool HostSkips(std::uint32_t id) {
    if (g_hown.empty()) return false;
    const auto it = g_hown.find(id);
    return it != g_hown.end() && (it->second.e.owner == erules::Owner::guest || it->second.e.frozen);
}
bool IsOwnedHere(std::uint32_t id) {
    if (g_gown.empty()) return false;
    const auto it = g_gown.find(id);
    return it != g_gown.end() && it->second.g.owned;
}
unsigned GuestOwnedCount() {
    unsigned n = 0;
    for (const auto& kv : g_hown) n += kv.second.e.owner == erules::Owner::guest ? 1u : 0u;
    return n;
}
unsigned OwnedHereCount() {
    unsigned n = 0;
    for (const auto& kv : g_gown) n += kv.second.g.owned ? 1u : 0u;
    return n;
}
void PublishOwned() {
    std::uint64_t k[erules::KeySet::kCap];
    std::uint32_t v[erules::KeySet::kCap];
    unsigned n = 0;
    for (const auto& kv : g_gown)
        if (kv.second.g.owned && kv.second.entity && n < erules::KeySet::kCap) k[n] = kv.second.entity, v[n] = kv.first, ++n;
    g_oset.Publish(k, v, n);
}
// Vitals: a drop of health or stamina since the base (the last owner change) latches `damaged`; the base follows a rise.
void NoteVitals(std::uintptr_t entity, bool& have_base, float& bh, float& bs, bool& damaged) {
    float h = 0, s = 0;
    if (damaged || !entity || !ecombat::Vitals(entity, &h, &s)) return;
    if (!have_base) {
        have_base = true, bh = h, bs = s;
        return;
    }
    if (h < bh - 0.5f || s < bs - 0.5f) damaged = true;
    else bh = (std::max)(bh, h), bs = (std::max)(bs, s);
}

// ---- the camps (2 Hz): the MULE manager's CP ids -> the CP actors -> their members -> the camp phase
void WalkActorsById(const std::vector<std::uint32_t>& want, std::vector<LocalEnemy>& out) {
    out.clear();
    const std::uintptr_t reg = mem::Q(A(kRegGlobal));
    std::int32_t ngroups = 0;
    if (want.empty() || !reg || !mem::Rd(reg + 0x440, ngroups) || ngroups <= 0 || ngroups > 512) return;
    const std::uintptr_t garr = mem::Q(reg + 0x448);
    for (std::int32_t gi = 0; gi < ngroups; ++gi) {
        const std::uintptr_t g = mem::Q(garr + 8ull * static_cast<unsigned>(gi));
        std::int32_t count = 0;
        if (!g || !mem::Rd(g + 0x40, count) || count <= 0 || count > 8192) continue;
        const std::uintptr_t arr = mem::Q(g + 0x48);
        if (!arr) continue;
        static std::vector<std::uintptr_t> objs;
        objs.resize(static_cast<std::size_t>(count));
        if (!mem::Read(arr, objs.data(), objs.size() * 8)) continue;
        for (const std::uintptr_t o : objs) {
            if (!o) continue;
            std::uint8_t head[0x28] = {};
            if (!mem::Read(o, head, sizeof(head)) || !head[kActInUse]) continue;
            std::uint32_t id = 0;
            std::memcpy(&id, head + kActId, 4);
            if (std::binary_search(want.begin(), want.end(), id)) out.push_back(LocalEnemy{id, o});
        }
    }
}
void CampAge(double now) {
    for (auto it = g_camp_of.begin(); it != g_camp_of.end();) it = now - it->second.t > erules::kCampKeyKeepMs ? g_camp_of.erase(it) : std::next(it);
}
void CampWalk(double now) {
    if (!g_cp_ok || now < g_cp_next) return;
    g_cp_next = now + 500;
    // (a failed walk keeps the last good keys until they age out)
    const std::uintptr_t mgr = mem::Q(A(kMuleMgr));
    std::int32_t n = 0;
    if (!mgr || !mem::Rd(mgr + kMgrCpCount, n) || n <= 0 || n > 512) return CampAge(now);
    std::vector<std::uint32_t> want(static_cast<std::size_t>(n));
    const std::uintptr_t ids = mem::Q(mgr + kMgrCpIds);
    if (!ids || !mem::Read(ids, want.data(), want.size() * 4)) return CampAge(now);
    std::map<std::uintptr_t, std::uint64_t> seen;  // this walk's (the first camp listing a MULE keeps it)
    std::sort(want.begin(), want.end());
    std::vector<LocalEnemy> cps;
    WalkActorsById(want, cps);
    unsigned members = 0;
    const char* cls = nullptr;
    for (const LocalEnemy& cp : cps) {
        std::int32_t ph = -1, cnt = 0;
        if (!mem::Rd(cp.actor + kCpPhase, ph) || ph < 0 || ph > erules::kPhaseMax) continue;
        if (!mem::Rd(cp.actor + kCpCount, cnt) || cnt <= 0 || cnt > 128) continue;
        const std::uintptr_t list = mem::Q(cp.actor + kCpMembers);
        if (!list) continue;
        for (std::int32_t i = 0; i < cnt; ++i) {
            const std::uintptr_t m = mem::Q(list + 8ull * static_cast<unsigned>(i));
            if (!m || mem::Q(m) != A(kMuleVt)) continue;
            ++members;
            // a MULE in two camp lists: the first camp keeps it (logged, the first 8)
            const auto ins = seen.emplace(m, kCampCp | cp.id);
            if (!ins.second) {
                if (ins.first->second != (kCampCp | cp.id) && g_camp_conflicts++ < 8)
                    Log("[enemy] camp conflict: a member of %s is also listed by %s (kept in the first)", CampName(ins.first->second), CampName(kCampCp | cp.id));
                continue;
            }
            g_camp_of[m] = CampSeen{kCampCp | cp.id, static_cast<std::uint8_t>(ph), now, cp.actor};
        }
        if (!cls) cls = mem::RttiName(mem::RttiOf(cp.actor));
    }
    CampAge(now);
    if (!g_cp_logged) {
        g_cp_logged = true;
        Log("[enemy] camps: n=%zu of %d ids (class %s), members n=%u", cps.size(), n, cls ? cls : "?", members);
    }
}
std::uint8_t CampPhaseOf(std::uintptr_t actor) {
    const auto it = g_camp_of.find(actor);
    return it != g_camp_of.end() ? it->second.phase : erules::kPhaseUnknown;
}

// The capture slot of one enemy off at once (it changes owner: its model is about to be a mirror's).
void CaptureOffFor(std::uint32_t id) {
    for (unsigned k = 0; k < erules::kPoseSlots; ++k)
        if (g_pslots.Id(k) == id) CaptureSlotOff(k);
}
// ---- freeze / unfreeze a copy here
bool FreezeCopy(std::uint32_t id, std::uintptr_t actor, bool* bit13_pre, std::uint32_t* ea4_out) {
    const std::uint32_t pre = U32(actor + kMuleSeq);
    if (bit13_pre) *bit13_pre = ((pre >> 13) & 1u) != 0;
    NoteWeaponPre(id, actor);
    if (!Post(id, kOccupiedVt, 1)) return false;
    const std::uint32_t ea4 = U32(actor + kMuleSeq);
    if (ea4_out) *ea4_out = ea4;
    return ((ea4 >> 14) & 1u) != 0;
}
void UnfreezeCopy(std::uint32_t id, std::uintptr_t actor, bool bit13_pre) {
    if (!((U32(actor + kMuleSeq) >> 14) & 1u)) return;  // not ours any more: nothing posted
    Post(id, kOccupiedVt, 0);
    if (!bit13_pre) Post(id, kRequiredVt, 0);  // a bit 13 the game set is never cleared by us
    RestoreWeapon(id, actor);
}
void AddFollower(std::uint32_t id, std::uintptr_t actor, const ActorNow& a, bool bit13_pre, std::uint32_t ea4, double now, bool own) {
    Mirror m;
    m.id = id, m.actor = actor, m.entity = a.entity, m.sm = mem::Q(a.entity + 0xB8);
    m.frozen = true, m.t_frozen = now, m.ea4_entry = ea4, m.bit13_pre = bit13_pre, m.own = own;
    g_mirrors.push_back(m);
    PublishMirrors();
}
// Takes one mirror out of the list (its apply slot freed, nothing posted). False: none.
bool TakeMirrorOut(std::uint32_t id, Mirror* out) {
    for (std::size_t i = 0; i < g_mirrors.size(); ++i) {
        if (g_mirrors[i].id != id) continue;
        FreeSlot(g_mirrors[i]);
        if (out) *out = g_mirrors[i];
        g_mirrors.erase(g_mirrors.begin() + static_cast<std::ptrdiff_t>(i));
        PublishMirrors();
        return true;
    }
    return false;
}

// ---- the messages
void OwnSend(std::uint8_t type, const ewire::Own& o) {
    std::uint8_t buf[ewire::kScbHeader + ewire::kOwnSize];
    ewire::ScbHeader h;
    h.guids_fp = g_fp_done ? g_guids : 0, h.seq = g_own_seq + 1;
    const std::size_t n = ewire::EncodeOwns(h, type, &o, 1, buf, sizeof(buf));
    if (!n) {
        Log("[enemy] OWN id=%05X: the message did not encode (type %u)", o.id, type);
        return;
    }
    ++g_own_seq;
    g_own_out.emplace_back(buf, buf + n);
}
void OwnSendReset() {
    std::uint8_t buf[ewire::kScbHeader + ewire::kOwnResetSize];
    ewire::ScbHeader h;
    h.guids_fp = g_fp_done ? g_guids : 0, h.seq = ++g_own_seq;
    const std::size_t n = ewire::EncodeOwnReset(h, g_own_gen, buf, sizeof(buf));
    if (n) g_own_out.emplace_back(buf, buf + n);
}
void OwnFlush() {
    while (!g_own_out.empty() && link::SendCombat(g_own_out.front().data(), g_own_out.front().size())) g_own_out.erase(g_own_out.begin());
}
ewire::Own OwnOf(std::uint32_t id, std::uint16_t epoch, erules::Owner owner, std::uint8_t reason, std::uint8_t phase) {
    ewire::Own o;
    o.gen = g_own_gen, o.id = id, o.epoch = epoch, o.owner = static_cast<std::uint8_t>(owner), o.reason = reason, o.phase = phase;
    o.health = std::numeric_limits<float>::quiet_NaN();
    return o;
}
void PlaceInto(ewire::Own& o, const double (&pos)[3], float yaw, const float* vel) {
    std::memcpy(o.pos, pos, sizeof(o.pos));
    o.yaw = yaw;
    if (vel) std::memcpy(o.vel, vel, sizeof(o.vel));
    o.flags |= ewire::kOwnHasPlace;
}

// ---- the host
// The host's copy given back to its AI (a back's end, a refusal, a reset): placed at her place (her OWNACK / her newest frame),
// its phase raised to hers (capped at Caution, RisePhase: escalation only), then released (Occupied{0}, Required{0} by bit 13).
void FollowerShow(std::uint32_t id, const char* why);
void CarryOutcomeTo(std::uint32_t id, std::uintptr_t actor, std::uint8_t her_state, double now);
void HostBackDone(std::uint32_t id, HostRec& r, const double* pos, float yaw, std::uint8_t phase, const char* how, std::uint8_t her_state = 0) {
    FollowerShow(id, "back here");
    ActorNow a;
    const bool same = ReadActor(r.actor, id, a);
    double err = -1;
    char ph[64] = "phase unknown";
    const std::uint32_t ea4_before = same ? a.ea4 : 0;
    if (same) {
        if (pos && a.have_xf) {
            const double p[3] = {pos[0], pos[1], pos[2]};
            err = Dist3(p, a.pos);
            const WorldTransform xf = XfFromYaw(a.xf, p, yaw);
            // her place moves the host's copy (it persists in the host's save) only near where the host's follower stands: a copy of
            // hers that fell through the ground, a stale or a bad frame never relocates the host's enemy (review rv-safety 5)
            if (!erules::BackPlaceOk(err, p[2] - a.pos[2])) {
                static unsigned refused = 0;
                if (refused++ < 20)
                    Log("[enemy] OWN id=%05X: her place refused (err %.1f m, dz %.1f m; the bounds %.0f m, %.0f m down): released where it stands", id, err,
                        p[2] - a.pos[2], erules::kBackPlaceMaxM, erules::kBackPlaceMaxDropM);
            } else if (a.realized) {
                if (err > erules::kBandStillM) ActorXf(r.actor, kActorTeleport, xf);
            } else if (err > 1.0) {
                ActorPlaceFar(r.actor, xf, false);
            }
        }
        const std::uint8_t want = erules::ReturnPhase(phase), before = PhaseOf(r.actor);
        if (want != erules::kPhaseUnknown && before != erules::kPhaseUnknown && want > before && CallPhase(kRisePhase, r.actor, want))
            std::snprintf(ph, sizeof(ph), "phase raised %u->%u", before, PhaseOf(r.actor));
        else if (before != erules::kPhaseUnknown)
            std::snprintf(ph, sizeof(ph), "phase kept %u", before);
    }
    Mirror m;
    if (TakeMirrorOut(id, &m)) {
        if (ReleaseHeld() && !saveguard::Busy()) {  // (never held while the host's game saves: the save would capture it frozen)
            m.why = "an ownership return", m.own = false;
            g_pending.push_back(m);  // a cutscene: posted when the hold ends (P1a's)
            PublishMirrors();
        } else if (same) {
            UnfreezeCopy(id, r.actor, m.bit13_pre);
        }
    }
    r.far_follow = false;
    g_tracks.erase(id), g_prx.erase(id);
    Log("[enemy] OWN id=%05X back here (epoch %u; %s; err %.2f m; %s; health not carried; released ea4 0x%X->0x%X)", id, r.e.epoch, how, err, ph, ea4_before,
        same ? U32(r.actor + kMuleSeq) : 0);
    // P1c: her outcome on it (down or dead on her side) carried onto the host's copy: never back standing
    if (same && her_state) CarryOutcomeTo(id, r.actor, her_state, g_tnow);
    SingleEnd(id, how);  // round 24: a single hand-over over
}
// The state byte of her newest frame for it (0: none)
std::uint8_t HerState(std::uint32_t id) {
    const auto it = g_tracks.find(id);
    const erules::Sample* z = it != g_tracks.end() ? it->second.Newest() : nullptr;
    return z ? z->state : 0;
}
bool PendingHas(std::uint32_t id) {
    for (const Mirror& m : g_pending)
        if (m.id == id) return true;
    return false;
}
// One try of the setter: true when it is settled (done, failed or down here already), false: try again (deferred).
bool CarryTry(const CarryWait& w, double now, bool last) {
    static const char* const kK[3] = {"faint", "sleep", "restrained"};
    const char* her = erules::DefeatText(w.d);
    // (the host's game saving or a cutscene holding the copy's return: the copy is still ours, frozen; never set under a save)
    if (saveguard::Busy() || PendingHas(w.id)) {
        if (!last) return false;
        ++g_carry_failed;
        Log("[enemy] OWN id=%05X her outcome %s NOT carried: its return was held past %.0f s (a save or a cutscene)", w.id, her, erules::kCarryWaitMs / 1000.0);
        return true;
    }
    const int res = ecombat::SetOutcome(w.actor, w.id, w.k);
    if (res == 0 && !last) return false;
    if (res == -2) {
        ++g_carry_down;
        Log("[enemy] OWN id=%05X her outcome %s not carried: down here already (%s)", w.id, her, erules::DefeatText(static_cast<std::uint8_t>(ecombat::OutcomeNow(w.actor, w.id))));
        return true;
    }
    ++(res > 0 ? g_carried : g_carry_failed);
    if (res > 0) {
        g_outcome_id = w.id;
        CarryWait rb = w;
        rb.until = now + 1000.0;
        g_readback.push_back(rb);
    }
    Log("[enemy] OWN id=%05X outcome carried: her %s -> %s here (k %d)%s", w.id, her, kK[w.k], w.k,
        res > 0 ? "" : res == 0 ? ": still a hit queued (back standing)" : ": the setter FAILED or it left this world (back standing)");
    return true;
}
void CarryOutcomeTo(std::uint32_t id, std::uintptr_t actor, std::uint8_t her_state, double now) {
    const std::uint8_t dv = ewire::DefeatOfState(her_state);
    const erules::Defeat d = (her_state & ewire::kDead) ? erules::Defeat::dead : dv > 5 ? erules::Defeat::other : static_cast<erules::Defeat>(dv);
    if (d == erules::Defeat::none) return;
    if (d == erules::Defeat::dead) g_no_take[id] = erules::kCampNoTakeWorld;  // her corpse: never taken again this world (rv-gameplay 1 / 10)
    const int k = erules::CarryOutcome(d, g_camp_carry && ecombat::CarryReady());
    if (k < 0) {
        ++g_carry_failed;
        Log("[enemy] OWN id=%05X her outcome %s NOT carried (enemy_owner_outcomes=0 or its checks): back standing", id, erules::DefeatText(static_cast<std::uint8_t>(d)));
        return;
    }
    CarryWait w{id, actor, k, now + erules::kCarryWaitMs, static_cast<std::uint8_t>(d)};
    if (CarryTry(w, now, false)) return;
    g_carry.push_back(w);
    Log("[enemy] OWN id=%05X her outcome %s: carry deferred (%s)", id, erules::DefeatText(w.d),
        saveguard::Busy() ? "the host's game saves" : PendingHas(id) ? "a cutscene holds its return" : "a hit queued on the copy");
}
void CarryTick(double now) {
    for (std::size_t i = 0; i < g_carry.size();) {
        if (!CarryTry(g_carry[i], now, now >= g_carry[i].until)) {
            ++i;
            continue;
        }
        g_carry.erase(g_carry.begin() + static_cast<std::ptrdiff_t>(i));
    }
    for (std::size_t i = 0; i < g_readback.size();) {
        const CarryWait& w = g_readback[i];
        if (now < w.until) {
            ++i;
            continue;
        }
        const int got = ecombat::OutcomeNow(w.actor, w.id);
        const int want = w.k + 1;  // k 0 faint, 1 sleep, 2 restrained -> erules::Defeat 1, 2, 3
        if (got != want) ++g_carry_failed;  // (a mismatch counts as a fallback: spec 6.2)
        Log("[enemy] OWN id=%05X outcome readback: k %d -> now %s%s", w.id, w.k, got < 0 ? "unread (it left this world)" : erules::DefeatText(static_cast<std::uint8_t>(got)),
            got == want ? "" : got == 0 ? ": NOT TAKEN (a fallback)" : ": a mismatch (a fallback)");
        g_readback.erase(g_readback.begin() + static_cast<std::ptrdiff_t>(i));
    }
}
// P1c: the host's follower guard (M0 the other way): a frozen copy of hers hidden while her frames say unrealized or held there
// (a second), or none came for 3 s (erules::FollowerGuard); shown again when they say realized and free, and at every return.
void FollowerShow(std::uint32_t id, const char* why) {
    g_fbad_since.erase(id);
    const auto it = g_fhid.find(id);
    if (it == g_fhid.end()) return;
    ActorNow a;
    bool ok = true;
    if (ReadActor(it->second.actor, id, a) && (a.a88 & 1u) && !it->second.pre) ok = ActorBool(it->second.actor, kVtHidden, 0);
    Log("[enemy] OWN id=%05X: the host's follower shown again (%s%s)", id, why, ok ? "" : ", the call FAULTED");
    g_fhid.erase(it);
}
void FollowerGuardTick(std::uint32_t id, std::uintptr_t actor, const ActorNow& a, double t_rx, double now) {
    if (!g_follow_guard) return;
    const auto t = g_tracks.find(id);
    const erules::Sample* z = t != g_tracks.end() ? t->second.Newest() : nullptr;
    // hidden there counts too, unless it is down or dead there (a down MULE is never hidden here: spec 8.1 / 8.3; review
    // rv-correctness N6)
    const bool down = z && ((z->state & ewire::kDead) || ewire::DefeatOfState(z->state) != 0);
    const bool bad = z && (!(z->state & ewire::kRealized) || (z->state & ewire::kSeqHeld) || ((z->state & ewire::kHidden) && !down));
    double since = 0;
    if (bad) {
        auto ins = g_fbad_since.emplace(id, now);
        since = ins.first->second;
    } else {
        g_fbad_since.erase(id);
    }
    const bool hide = erules::FollowerGuard(now - t_rx, bad ? now - since : 0) == erules::Guard::hide;
    const bool hidden = g_fhid.count(id) != 0;
    if (hide && !hidden) {
        FollowHid h;
        h.actor = actor, h.pre = (a.a88 & 1u) != 0;
        const bool ok = h.pre || ActorBool(actor, kVtHidden, 1);
        g_fhid[id] = h;
        ++g_guard_hidden;
        Log("[enemy] OWN id=%05X: the host's follower hidden (her copy %s%s)", id,
            !z || now - t_rx > erules::kNoEntryHideMs ? "sent no frame for 3 s" : !(z->state & ewire::kRealized) ? "is not realized there"
            : (z->state & ewire::kSeqHeld) ? "is held by a sequence there" : "is hidden there",
            ok ? "" : ", the call FAULTED");
    } else if (!hide && hidden) {
        FollowerShow(id, "her frames say realized and free again");
    }
}
void HostTakeOne(const HostEnemy& h, HostRec& r, double now, erules::OwnReason why) {
    const bool forced = why == erules::OwnReason::forced;
    const ActorNow& a = h.now;
    if (!a.have_xf) return;
    bool b13 = false;
    std::uint32_t ea4 = a.ea4;
    const bool ok = !a.realized || FreezeCopy(h.id, h.actor, &b13, &ea4);
    if (!erules::HostTake(r.e, now, ok, erules::kCampHoldMs, erules::kCampRefuseHoldMs)) {
        if (!ok) {
            if ((ea4 >> 14) & 1u) UnfreezeCopy(h.id, h.actor, b13);
            Log("[enemy] OWN id=%05X not handed over: the host's copy did not freeze (ea4 0x%X); hold 15 s", h.id, ea4);
        }
        return;
    }
    CaptureOffFor(h.id);
    if (a.realized) AddFollower(h.id, h.actor, a, b13, ea4, now, true);
    else r.far_follow = true;
    r.damaged = false, r.have_base = false, r.forced = false, r.t_rx = now;
    r.g_phase = erules::kPhaseUnknown, r.g_state = 0, r.g_damaged = false;
    g_tracks.erase(h.id), g_prx.erase(h.id);
    g_change_times.push_back(now);
    ++g_os.takes;
    // an unrealized copy's place and phase are stale here (its AI does not run on the host): neither is sent, her copy stays where
    // her game has it (review rv-correctness B1: a take of an unloaded camp teleported her running MULEs to the host's stale posts)
    const std::uint8_t phase = a.realized ? PhaseOf(h.actor) : erules::kPhaseUnknown;
    ewire::Own o = OwnOf(h.id, r.e.epoch, erules::Owner::guest, static_cast<std::uint8_t>(why), phase);
    if (a.realized) PlaceInto(o, a.pos, YawOfXf(a.xf), h.vel);
    OwnSend(ewire::kScbOwn, o);
    Log("[enemy] OWN id=%05X -> guest (%s: guest %.1f m, host %.1f m; epoch %u; phase %u; host copy %s)", h.id, forced ? "forced" : erules::OwnReasonText(why), r.d_g, r.d_h,
        r.e.epoch, phase, a.realized ? "frozen" : "far follower");
}
void HostBackStart(std::uint32_t id, HostRec& r, erules::OwnReason why, bool forced, double now) {
    const double silence = now - r.t_rx;
    if (!erules::HostBack(r.e, now, erules::kCampHoldMs)) return;
    g_change_times.push_back(now);
    ++(forced ? g_os.forced : g_os.backs);
    ewire::Own o = OwnOf(id, r.e.epoch, erules::Owner::host, static_cast<std::uint8_t>(why), r.g_phase);
    OwnSend(ewire::kScbOwn, o);
    char d[64];
    switch (why) {
        case erules::OwnReason::far_off: std::snprintf(d, sizeof(d), "far %.1f m", r.d_g); break;
        case erules::OwnReason::host_near: std::snprintf(d, sizeof(d), "host near %.1f m", r.d_h); break;
        case erules::OwnReason::silence: std::snprintf(d, sizeof(d), "silence %.1f s", silence / 1000.0); break;
        case erules::OwnReason::range: std::snprintf(d, sizeof(d), "range %.0f m", r.d_g); break;
        default: std::snprintf(d, sizeof(d), "%s", erules::OwnReasonText(why)); break;
    }
    Log("[enemy] OWN id=%05X -> host (%s; epoch %u; guest phase %u)", id, d, r.e.epoch, r.g_phase);
}
void HostAck(const ewire::Own& o, double now) {
    const auto it = g_hown.find(o.id);
    if (it == g_hown.end()) return;
    HostRec& r = it->second;
    const double sent = r.e.t_sent;
    const bool was_guest = r.e.owner == erules::Owner::guest;
    const erules::AckAct act = erules::HostOnAck(r.e, o.epoch, o.result, now, erules::kCampRefuseHoldMs);
    if (act == erules::AckAct::ignore) {
        ++g_os.stale;
        return;
    }
    if (now - sent > 1000.0) ++g_os.no_owner;  // a frozen copy and no thinking one for more than a second
    if (act == erules::AckAct::taken) {
        r.t_rx = now;
        return;
    }
    if (act == erules::AckAct::refused_back) {
        ++g_os.refused;
        // P1c: one member's state there (not mirrored, not realized, dead, held, hidden) splits: this one stays the host's and out of
        // its camp's takes (dead: this world; else 15 s); a cutscene, her cap, an alerted camp or a fault send the camp back. Only
        // those count toward the refusal cap, once a camp take (review rv-correctness B2 / B3, rv-gameplay 1)
        const bool split = erules::CampRefuseSplits(o.refusal);
        if (split) {
            const double ms = erules::CampNoTakeMs(o.refusal);
            g_no_take[o.id] = ms < 0 ? erules::kCampNoTakeWorld : now + ms;
        } else {
            CampRec& cr = g_camps[CampKeyOf(o.id, r.actor)];
            if (!cr.refused) g_refuse_times.push_back(now);
            cr.refused = true;
        }
        Log("[enemy] OWN id=%05X refused by the guest (%s): the host's again (epoch %u), hold 15 s%s", o.id, erules::OwnRefuseText(o.refusal), r.e.epoch,
            split ? (erules::CampNoTakeMs(o.refusal) < 0 ? "; its camp stays hers, it out of the camp's takes this world" : "; its camp stays hers, it out of the camp's takes 15 s")
                  : "; its camp goes back");
        HostBackDone(o.id, r, nullptr, 0, erules::kPhaseUnknown, "refused: it never left");
        return;
    }
    (void)was_guest;
    if (o.refusal == static_cast<std::uint8_t>(erules::OwnRefuse::fault)) ++g_os.two_thinking;  // her freeze did not take
    HostBackDone(o.id, r, (o.flags & ewire::kOwnHasPlace) ? o.pos : nullptr, o.yaw, o.phase, "her OWNACK", HerState(o.id));
}
// The host's followers: frozen copies driven and posed from her frames; far followers (unrealized here) placed at 2 Hz.
void HostFollow(double now, const double* hs) {
    const double render = g_clock.Render(now);
    bool changed = false;
    for (Mirror& m : g_mirrors) {
        if (!m.frozen || !m.own) continue;
        const auto rit = g_hown.find(m.id);
        ActorNow a;
        const bool same = ReadActor(m.actor, m.id, a);
        if (!same || !a.realized || a.entity != m.entity) {
            FreeSlot(m);
            m.frozen = false, changed = true;
            FollowerShow(m.id, "it left the host's world");
            if (same) UnfreezeCopy(m.id, m.actor, m.bit13_pre);
            if (rit != g_hown.end()) rit->second.far_follow = true;
            Log("[enemy] OWN id=%05X: the host's copy left its world: a far follower now", m.id);
            continue;
        }
        if (!a.held) {
            FollowerShow(m.id, "a game sequence took it");
            FreeSlot(m);
            m.frozen = false, changed = true;
            if (rit != g_hown.end()) rit->second.forced = true;
            Log("[enemy] OWN id=%05X: a game sequence took the host's copy (bit 14 cleared, ea4 0x%X): given back to the host", m.id, a.ea4);
            continue;
        }
        // round 23 C (enemy_down_hold; docs/enemy-sync.md 14.9): her Fragile is down: near ones held frozen in their last pose (no
        // guard, no place, no pose), never posed ones and far down ones hidden here by the follower guard's table (shown again when
        // she is back, or at any return); never handed back alive meanwhile (DownFreezesOwnership)
        if (DownOn()) {
            const auto t0 = g_tracks.find(m.id);
            const erules::Sample* z0 = t0 != g_tracks.end() ? t0->second.Newest() : nullptr;
            erules::DownIn d;
            d.posed = m.slot >= 0 || m.posed_once;
            d.hidden_here = (a.a88 & 3u) && !g_fhid.count(m.id);
            d.defeat = z0 ? ewire::SenDefeat(z0->state) : 0;
            const double hp[3] = {hs ? hs[0] : 0.0, hs ? hs[1] : 0.0, hs ? hs[2] : 0.0};
            d.dist_m = hs && a.have_xf ? Dist3(a.pos, hp) : -1;
            d.radius_m = g_ecfg.enemy_down_hold_radius;
            const erules::DownAct act = erules::DownHold(d);
            if (act == erules::DownAct::hold) {
                if (g_down_held.insert(m.id).second) ++g_ds.holds;
                continue;
            }
            if (act == erules::DownAct::hide) {
                g_down_held.erase(m.id);
                if (!g_fhid.count(m.id)) {
                    FollowHid h;
                    h.actor = m.actor, h.pre = (a.a88 & 1u) != 0;
                    const bool ok = h.pre || ActorBool(m.actor, kVtHidden, 1);
                    g_fhid[m.id] = h;
                    g_down_fhid.insert(m.id);
                    ++g_ds.hides;
                    Log("[enemy] down hold id=%05X: the host's follower hidden here, kept frozen (%s while her Fragile is down; a88 0x%X%s)", m.id,
                        d.dist_m > d.radius_m ? "far and down there" : "never posed", a.a88, ok ? "" : ", the call FAULTED");
                }
                continue;
            }
        }
        g_down_held.erase(m.id);
        if (g_down_fhid.erase(m.id)) FollowerShow(m.id, "the usual rules again (far and standing)");
        if (rit != g_hown.end()) FollowerGuardTick(m.id, m.actor, a, rit->second.t_rx, now);
        const auto it = g_tracks.find(m.id);
        // round 23 B (enemy_mirror_vehicle): the host's follower or her copy in a vehicle: given back to the host's game (that member
        // only; out of its camp's takes for 15 s, tested again then) until vehicles are synced
        if (g_ecfg.enemy_mirror_vehicle && rit != g_hown.end() && now >= m.veh_next) {
            m.veh_next = now + erules::kVehicleCheckMs;
            const erules::VehTest was = m.veh;
            m.veh = VehicleOf(m.id, m.actor, a.entity, &m.veh_e20, &m.veh_seat);
            if (m.veh != was) NoteVehicle(m.id, "the host's follower", was, m.veh, m.veh_e20, m.veh_seat);
            const bool hers = it != g_tracks.end() && it->second.Newest() && (it->second.Newest()->flags & ewire::kInVehicle);
            const bool here = m.veh != erules::VehTest::none;
            if ((here || hers) && rit->second.e.owner == erules::Owner::guest && !rit->second.e.pending && !DownFreeze()) {  // (round 23 C)
                ++g_mb.vehicle;
                g_no_take[m.id] = now + erules::kCampHoldMs;
                Log("[enemy] follower id=%05X in a vehicle (%s; our test: %s, +0xE20 0x%X seat %d): given back to the host's game until vehicles are synced "
                    "(out of its camp's takes %.0f s)", m.id, erules::VehicleSideText(here, hers), erules::VehTestText(m.veh), m.veh_e20, m.veh_seat,
                    erules::kCampHoldMs / 1000.0);
                HostBackStart(m.id, rit->second, erules::OwnReason::forced, true, now);
            }
        }
        if (it == g_tracks.end()) continue;
        double pos[3];
        float yaw = 0;
        if (!it->second.At(render, pos, &yaw) || !a.have_xf) continue;
        if (HoldTick(m, now)) continue;  // round 23 (the host's tie-up of her owned one)
        DriveMirror(m, a, pos, yaw, now);  // (not holding: logged only here; her game owns it)
        GivePose(m, it->second, render, it->second.Newest() && (it->second.Newest()->state & ewire::kPosed));
    }
    if (changed) {
        g_mirrors.erase(std::remove_if(g_mirrors.begin(), g_mirrors.end(), [](const Mirror& x) { return !x.frozen; }), g_mirrors.end());
        PublishMirrors();
    }
    // a far follower realized here is frozen at once (every tick: its AI must not run a tick on the host; review rv-correctness
    // R6); its placement at her frames stays 2 Hz
    const bool place = now >= g_hfar_next;
    if (place) g_hfar_next = now + 500;
    const bool busy = saveguard::Busy();
    for (auto& kv : g_hown) {
        HostRec& r = kv.second;
        if (!r.far_follow || !r.e.frozen) continue;
        ActorNow a;
        if (!ReadActor(r.actor, kv.first, a) || !a.have_xf) continue;
        if (a.realized) {
            if (busy) continue;  // (never frozen while the host's game saves: OwnHostTick's reset hands it back first)
            // back in the host's world: frozen again as a follower (a game sequence's hold: given back)
            bool b13 = false;
            std::uint32_t ea4 = 0;
            if (!a.held && FreezeCopy(kv.first, r.actor, &b13, &ea4)) {
                AddFollower(kv.first, r.actor, a, b13, ea4, now, true);
                r.far_follow = false;
                Log("[enemy] OWN id=%05X: the host's copy is realized here again: frozen (a follower; ea4 0x%X)", kv.first, ea4);
            } else {
                r.forced = true;
            }
            continue;
        }
        if (!place) continue;
        const auto it = g_tracks.find(kv.first);
        double pos[3];
        float yaw = 0;
        if (it == g_tracks.end() || !it->second.At(render, pos, &yaw) || Dist3(pos, a.pos) < 1.0) continue;
        if (ActorPlaceFar(r.actor, XfFromYaw(a.xf, pos, yaw), false)) ++g_gs.far_placed;
        else ++g_gs.far_faults;
    }
}
// A reset on the host: every enemy the host's at once (its copies placed at her newest frames and released), a new generation.
void HostOwnReset(const char* why, bool announce, bool world_gone) {
    unsigned n = 0;
    if (world_gone) g_was_hers.clear();  // round 25 R1: "earlier in this world"
    for (auto& kv : g_hown) {
        HostRec& r = kv.second;
        if (r.e.owner == erules::Owner::guest) ++n;
        if (r.e.frozen && !world_gone) {
            const auto it = g_tracks.find(kv.first);
            const erules::Sample* z = it != g_tracks.end() ? it->second.Newest() : nullptr;
            r.e.owner = erules::Owner::host;  // (for the line)
            HostBackDone(kv.first, r, z ? z->pos : nullptr, z ? z->yaw : 0.0f, r.g_phase, why, z ? z->state : 0);
        }
    }
    if (world_gone) {
        // that world's actors are gone: nothing to post for them. But `ready` also drops for a private room or a short WorldReady
        // dropout, with the same actors (review rv-safety 1): a follower that still reads as the same in-use MULE frozen by us
        // (bit 14) is unfrozen here, as P1a's ReleaseAll does, else it would stay frozen in the host's world (and its save)
        unsigned kept = 0;
        for (Mirror& m : g_mirrors)
            if (m.own) {
                FreeSlot(m);
                ActorNow a;
                if (m.frozen && ReadActor(m.actor, m.id, a) && a.held) UnfreezeCopy(m.id, m.actor, m.bit13_pre), ++kept;
                m.frozen = false;
            }
        g_mirrors.erase(std::remove_if(g_mirrors.begin(), g_mirrors.end(), [](const Mirror& x) { return x.own; }), g_mirrors.end());
        PublishMirrors();
        if (kept) Log("[enemy] own reset (%s): %u host copies still in this world: released here", why, kept);
    }
    g_hown.clear();
    g_single.clear(), g_single_req.clear();  // (round 24)
    for (auto& kv : g_camps) kv.second.refused = kv.second.force_take = false;
    if (world_gone) {
        // that world's camps, keys and split members go with it (review rv-correctness N1)
        g_fhid.clear(), g_fbad_since.clear(), g_carry.clear(), g_readback.clear();
        g_camps.clear(), g_camp_of.clear(), g_no_take.clear();
        g_guest_last_t = -1e18;
    }
    ++g_own_gen;
    if (announce) OwnSendReset();
    ++g_os.resets;
    Log("[enemy] own reset (%s): every enemy is the host's (gen %u; %u were the guest's)", why, g_own_gen, n);
}
// P1c: the camps at the host's 2 Hz walk (erules::CampDecide). Every enemy of the host's world is in one camp (its CP's, else one
// of its own). A take hands every takeable member over (P1b's OWN each), a back every member she owns. One take a tick.
// A host-owned member a take would hand over: standing, free and shown here, not on the no-take list (her split refusals).
bool HostTakeable(const HostEnemy& h, double now) {
    const bool down = h.now.dead || ecombat::HostDefeat(h.id) != 0;
    // (round 23 B: the walk's 1 s in-vehicle test too: the ride fields)
    if (down || h.now.seq_only || h.now.held || (h.now.realized && (h.now.hidden_or_asleep || InVehicle(h.now.entity) || h.veh != erules::VehTest::none))) return false;
    return !NoTake(h.id, now);
}
void CampTake(std::uint64_t key, CampRec& cr, const std::vector<const HostEnemy*>& ms, const erules::CampIn& ci, erules::OwnReason why, double now,
              const double (&guest)[3], bool have_guest, const double (&hs)[3], bool have_host) {
    unsigned n = 0, failed = 0;
    char ids[200] = "";
    std::size_t len = 0;
    for (const HostEnemy* h : ms) {
        HostRec& r = g_hown[h->id];
        r.actor = h->actor;
        if (r.e.owner == erules::Owner::guest || r.e.pending) continue;
        if (!HostTakeable(*h, now)) continue;
        r.d_g = have_guest ? Dist3(h->now.pos, guest) : -1, r.d_h = have_host ? Dist3(h->now.pos, hs) : -1;
        HostTakeOne(*h, r, now, why);
        if (r.e.owner == erules::Owner::guest) {
            ++n;
            if (len + 8 < sizeof(ids)) len += static_cast<std::size_t>(std::snprintf(ids + len, sizeof(ids) - len, "%s%05X", n > 1 ? " " : "", h->id));
        } else {
            ++failed;
        }
    }
    if (failed && n) {
        // a member's copy did not freeze: the camp back at once (never a camp split by a fault)
        for (const HostEnemy* h : ms) {
            const auto it = g_hown.find(h->id);
            if (it != g_hown.end() && it->second.e.owner == erules::Owner::guest) HostBackStart(h->id, it->second, erules::OwnReason::refused, true, now);
        }
    }
    cr.hold_until = now + (failed ? erules::kCampRefuseHoldMs : erules::kCampHoldMs);
    cr.t_change = now, cr.refused = cr.force_take = false;
    g_camp_last_take = now;
    Log("[enemy] camp %s -> guest (%s: n=%u of %u, guest %.1f m, host %.1f m, r %.0f m, engage %.0f m, margin %.0f m, loaded %d, phase %u, camp %u; ids %s)%s",
        CampName(key), erules::OwnReasonText(why), n, ci.members, ci.d_g, ci.d_h, ci.r, erules::CampEngage(ci.r), erules::CampMargin(ci.r), ci.host_loaded ? 1 : 0, ci.phase,
        ci.cp_phase, ids, failed ? (n ? "; a copy did not freeze: back at once" : "; no copy froze") : "");
}
void CampBack(std::uint64_t key, CampRec& cr, const std::vector<const HostEnemy*>& ms, const erules::CampIn& ci, const erules::OwnVerdict& v, double now) {
    unsigned n = 0;
    for (const HostEnemy* h : ms) {
        const auto it = g_hown.find(h->id);
        if (it == g_hown.end() || it->second.e.owner != erules::Owner::guest) continue;
        it->second.forced = false;
        it->second.d_g = ci.d_g, it->second.d_h = ci.d_h;
        HostBackStart(h->id, it->second, v.reason, v.act == erules::OwnAct::forced_back, now);
        ++n;
    }
    cr.hold_until = now + erules::kCampHoldMs;
    cr.t_change = now, cr.refused = cr.force_take = false;
    g_camp_last_take = now;  // a back is a change too: one change at a time (review rv-gameplay 13)
    Log("[enemy] camp %s -> host (%s%s: n=%u, guest %.1f m, host %.1f m, r %.0f m, her phase %u, a member down there %d, silence %.1f s)", CampName(key),
        erules::OwnReasonText(v.reason), v.act == erules::OwnAct::forced_back && v.reason != erules::OwnReason::forced ? ", forced" : "", n, ci.d_g, ci.d_h, ci.r, ci.phase, ci.down_guest ? 1 : 0,
        ci.silence_ms / 1000.0);
}
// One camp member as erules::CampAggregate reads it (her newest frame for hers; the host's read for the host's).
erules::CampMember MemberOf(const HostEnemy& h, double now) {
    erules::CampMember e;
    const auto it = g_hown.find(h.id);
    HostRec* r = it != g_hown.end() ? &it->second : nullptr;
    e.hers = r && r->e.owner == erules::Owner::guest;
    e.pending = r && r->e.pending;
    if (e.pending) e.ack_wait_ms = now - r->e.t_sent;
    e.realized_host = h.now.realized;
    std::memcpy(e.pos, h.now.pos, sizeof(e.pos));
    if (e.hers) {
        const auto t = g_tracks.find(h.id);
        const erules::Sample* z = t != g_tracks.end() ? t->second.Newest() : nullptr;
        if (z) std::memcpy(e.pos, z->pos, sizeof(e.pos));
        e.standing = !(z && ((z->state & ewire::kDead) || ewire::DefeatOfState(z->state) != 0));
        e.silence_ms = now - r->t_rx;
        // (a fresh frame only: a stale one is the silence's)
        const bool bad = z && now - r->t_rx < 1000.0 && (!(z->state & ewire::kRealized) || (z->state & ewire::kSeqHeld));
        if (bad) {
            if (r->bad_since < 0) r->bad_since = now;
            e.bad_guest = true, e.bad_for_ms = now - r->bad_since;
        } else {
            r->bad_since = -1;
        }
        e.forced = r->forced;
        e.phase = r->g_phase;
    } else {
        e.standing = !(h.now.dead || ecombat::HostDefeat(h.id) != 0);
        e.takeable = HostTakeable(h, now);
        e.phase = e.standing ? PhaseOf(h.actor) : erules::kPhaseUnknown;
    }
    return e;
}
// A straggler of a camp she owns (down, held or in a vehicle on the host at the take, standing and free now): taken on its own
// (review rv-gameplay 5). True: one taken.
bool CampStraggler(std::uint64_t key, const std::vector<const HostEnemy*>& ms, double now, const double (&guest)[3], bool have_guest, const double (&hs)[3],
                   bool have_host) {
    for (const HostEnemy* h : ms) {
        const auto it = g_hown.find(h->id);
        if (it != g_hown.end() && (it->second.e.owner == erules::Owner::guest || it->second.e.pending)) continue;
        if (!HostTakeable(*h, now)) continue;
        HostRec& r = g_hown[h->id];
        r.actor = h->actor;
        r.d_g = have_guest ? Dist3(h->now.pos, guest) : -1, r.d_h = have_host ? Dist3(h->now.pos, hs) : -1;
        HostTakeOne(*h, r, now, erules::OwnReason::proximity);
        g_camp_last_take = now;
        if (r.e.owner != erules::Owner::guest) return false;
        ++g_stragglers;
        Log("[enemy] camp %s: straggler id=%05X -> guest (it stands and is free on the host now; the camp is hers)", CampName(key), h->id);
        return true;
    }
    return false;
}
void CampHostTick(double now, const double (&guest_in)[3], bool have_guest_in, const double (&hs)[3], bool have_host, bool host_cut, bool guard_ready) {
    // her place: held kGuestPlaceHoldMs when her body is missing (a vehicle, a respawn, a hiccup: review rv-gameplay 14)
    double guest[3] = {guest_in[0], guest_in[1], guest_in[2]};
    bool have_guest = have_guest_in;
    if (have_guest) std::memcpy(g_guest_last, guest, sizeof(guest)), g_guest_last_t = now;
    else if (now - g_guest_last_t <= erules::kGuestPlaceHoldMs) std::memcpy(guest, g_guest_last, sizeof(guest)), have_guest = true;
    std::map<std::uint64_t, std::vector<const HostEnemy*>> views;
    std::set<std::uint32_t> seen;
    for (const HostEnemy& h : g_host)
        if (h.now.have_xf) views[CampKeyOf(h.id, h.actor)].push_back(&h), seen.insert(h.id);
    const unsigned guest_owned = GuestOwnedCount();
    bool any_pending = false;
    for (const auto& kv : g_hown) any_pending = any_pending || kv.second.e.pending;
    const bool carry_ok = g_camp_carry && ecombat::CarryReady();
    const double range = g_ecfg.enemy_mirror_radius + erules::kOwnRangeSlackM;
    // round 23 C (docs/enemy-sync.md 14.9): either player down: no take, no back, no forced back (critique R7c: no OWN hand-over)
    const bool down_freeze = DownFreeze();
    unsigned down_kept = 0;
    bool took = false;
    std::uint64_t near_key = 0;
    double near_d = 1e18;
    const char* near_why = nullptr;
    erules::CampIn near_in;
    std::vector<erules::CampMember> mm;
    g_take_first.clear();
    g_take_first_at = now;
    for (auto& cv : views) {
        const std::uint64_t key = cv.first;
        const std::vector<const HostEnemy*>& ms = cv.second;
        // round 24: a single member of it is hers (enemy_own <id> 3): the camp is not decided (no take, back or straggler)
        if (SingleCamp(key)) {
            if (now >= g_single_line_next) {
                g_single_line_next = now + 5000;
                unsigned hers = 0;
                for (const HostEnemy* h : ms) {
                    const auto o = g_hown.find(h->id);
                    hers += o != g_hown.end() && o->second.e.owner == erules::Owner::guest ? 1u : 0u;
                }
                Log("[enemy] camp %s held: a single member is hers (n=%zu, hers %u; no camp decision until it is back; every 5 s)", CampName(key), ms.size(), hers);
            }
            continue;
        }
        mm.clear();
        for (const HostEnemy* h : ms) mm.push_back(MemberOf(*h, now));
        erules::CampIn ci;
        const erules::CampAgg ag = erules::CampAggregate(mm.data(), mm.size(), have_guest ? guest : nullptr, have_host ? hs : nullptr, ci);
        // a host-owned camp nowhere near her: nothing to decide (and no record)
        if (ag.hers == 0 && ag.pend == 0 && (!have_guest || ci.d_g > erules::kCampEngageMaxM + 20.0) && !g_camps.count(key)) continue;
        CampRec& cr = g_camps[key];
        ci.cp_phase = (key >> 32) == 1 ? CampPhaseOf(ms.front()->actor) : erules::kPhaseUnknown;
        ci.guest_ok = !g_guest_cut && g_refuse_times.size() < 3 && !down_freeze;
        ci.host_ok = !host_cut && guard_ready;
        ci.hold_over = now >= cr.hold_until;
        ci.guest_owned = guest_owned;
        ci.since_change_ms = now - g_camp_last_take;
        ci.other_pending = (any_pending && ag.pend == 0) || took;
        ci.range_m = range;
        ci.refused = cr.refused;
        ci.outcomes_ok = carry_ok;
        erules::OwnVerdict v = erules::CampDecide(ci);
        if (cr.force_take && ci.owner == erules::Owner::host && !ci.pending && !took) v = {erules::OwnAct::take, erules::OwnReason::forced};
        if (down_freeze && v.act != erules::OwnAct::none) v = {erules::OwnAct::none, v.reason}, ++down_kept;
        cr.last = ci, cr.v = v, cr.n = ci.members, cr.hers = ag.hers;
        // (round 27 M4) take first: her time within its engage distance without a take, then the rule
        const bool close = ci.owner == erules::Owner::host && ci.d_g >= 0 && ci.d_g <= erules::CampEngage(ci.r) && v.act != erules::OwnAct::take;
        if (!close) cr.close_since = -1;
        else if (cr.close_since < 0) cr.close_since = now;
        if (!down_freeze && v.act != erules::OwnAct::take && erules::CampTakeFirst(ci, cr.close_since < 0 ? 0.0 : now - cr.close_since))
            for (const HostEnemy* h : ms) g_take_first[h->id] = TakeFirstRec{static_cast<std::uint32_t>(key & 0xFFFFFFFFu), ci.d_h, ci.d_g};
        if (v.act == erules::OwnAct::take) {
            CampTake(key, cr, ms, ci, v.reason, now, guest, have_guest, hs, have_host);
            took = true;
            any_pending = true;
        } else if (v.act == erules::OwnAct::back || v.act == erules::OwnAct::forced_back) {
            CampBack(key, cr, ms, ci, v, now);
        } else if (ag.hers && !took && !down_freeze && erules::CampStragglerOk(ci) && CampStraggler(key, ms, now, guest, have_guest, hs, have_host)) {
            took = true;
            any_pending = true;
        } else if (ag.hers && !ci.pending && now >= cr.kept_next) {
            cr.kept_next = now + 10000;
            Log("[enemy] camp %s kept by the guest (n=%u of %u, guest %.1f m, host %.1f m, r %.0f m, her phase %u (%s), hold %.1f s): %s", CampName(key), ag.hers, ci.members,
                ci.d_g, ci.d_h, ci.r, ci.phase, erules::PhaseText(ci.phase), (std::max)(0.0, (cr.hold_until - now) / 1000.0), erules::CampStaysWhy(ci));
        } else if (!ag.hers && ci.d_g >= 0 && ci.d_g <= erules::CampEngage(ci.r) && ci.d_g < near_d) {
            near_d = ci.d_g, near_key = key, near_why = erules::CampTakeBlock(ci), near_in = ci;
        }
    }
    // the ones she owns that are in no camp of the host's view (unloaded or unread on the host): each decided as a loose camp of one
    // from her frames (silence, range, far; the host's distances do not count unloaded: review rv-gameplay 11)
    for (auto& kv : g_hown) {
        HostRec& r = kv.second;
        if (r.e.owner != erules::Owner::guest || seen.count(kv.first) || g_single.count(kv.first)) continue;  // (round 24: a single stays hers)
        HostEnemy h;
        h.id = kv.first, h.actor = r.actor;
        const auto t = g_tracks.find(kv.first);
        const erules::Sample* z = t != g_tracks.end() ? t->second.Newest() : nullptr;
        if (z) std::memcpy(h.now.pos, z->pos, sizeof(h.now.pos));
        else h.now.pos[0] = h.now.pos[1] = h.now.pos[2] = 1e7;  // (no frame: the silence or the range decides it)
        erules::CampMember e = MemberOf(h, now);
        e.realized_host = false;
        erules::CampIn ci;
        erules::CampAggregate(&e, 1, have_guest ? guest : nullptr, have_host ? hs : nullptr, ci);
        CampRec& cr = g_camps[kCampLoose | kv.first];
        ci.hold_over = now >= cr.hold_until;
        ci.range_m = range;
        ci.outcomes_ok = carry_ok;
        const erules::OwnVerdict v = erules::CampDecide(ci);
        if (v.act != erules::OwnAct::back && v.act != erules::OwnAct::forced_back) continue;
        if (down_freeze) {
            ++down_kept;
            continue;
        }
        r.forced = false, r.d_g = ci.d_g, r.d_h = ci.d_h;
        HostBackStart(kv.first, r, v.reason, v.act == erules::OwnAct::forced_back, now);
        cr.hold_until = now + erules::kCampHoldMs, cr.t_change = now;
        g_camp_last_take = now;
        Log("[enemy] OWN id=%05X not in the host's view: -> host (%s, guest %.1f m, silence %.1f s)", kv.first, erules::OwnReasonText(v.reason), ci.d_g,
            ci.silence_ms / 1000.0);
    }
    // round 24 (review c-review): a single take's own forced backs still apply while its camp is held (else a lost OWNACK, her
    // silence, the range, its copy unrealized there or a game sequence on the host's copy would leave it hers and its camp held
    // until a reset): decided as a camp of one, the proximity backs left out (hold_over false: only enemy_own <id> 4 ends it so)
    for (const auto& sv : g_single) {
        const auto it = g_hown.find(sv.first);
        if (it == g_hown.end() || it->second.e.owner != erules::Owner::guest) continue;
        HostRec& r = it->second;
        HostEnemy h;
        const HostEnemy* hp = nullptr;
        for (const HostEnemy& x : g_host)
            if (x.id == sv.first && x.now.have_xf) hp = &x;
        if (!hp) {
            h.id = sv.first, h.actor = r.actor;
            const auto t = g_tracks.find(sv.first);
            const erules::Sample* z = t != g_tracks.end() ? t->second.Newest() : nullptr;
            if (z) std::memcpy(h.now.pos, z->pos, sizeof(h.now.pos));
            else h.now.pos[0] = h.now.pos[1] = h.now.pos[2] = 1e7;
            hp = &h;
        }
        erules::CampMember e = MemberOf(*hp, now);
        if (hp == &h) e.realized_host = false;
        erules::CampIn ci;
        erules::CampAggregate(&e, 1, have_guest ? guest : nullptr, have_host ? hs : nullptr, ci);
        ci.hold_over = false;
        ci.range_m = range;
        ci.outcomes_ok = carry_ok;
        const erules::OwnVerdict v = erules::CampDecide(ci);
        if (v.act != erules::OwnAct::forced_back) continue;
        if (down_freeze) {
            ++down_kept;
            continue;
        }
        r.forced = false, r.d_g = ci.d_g, r.d_h = ci.d_h;
        HostBackStart(sv.first, r, v.reason, true, now);
        g_camp_last_take = now;
        Log("[enemy] single OWN id=%05X -> host (%s; guest %.1f m, silence %.1f s): its camp is decided again once it is back", sv.first,
            erules::OwnReasonText(v.reason), ci.d_g, ci.silence_ms / 1000.0);
    }
    if (down_kept && now >= g_down_own_next) {
        g_down_own_next = now + 10000;
        g_ds.no_change += down_kept;
        Log("[enemy] down hold: %u ownership change(s) not made (%s down: no hand-over until both are back; every 10 s)", down_kept,
            g_our_down && g_peer_down ? "both players" : g_our_down ? "our Sam" : "her Fragile");
    }
    // the nearest blocked camp's reason (changed, or every 10 s while one is within its engage distance)
    if (near_key && near_why && (near_key != g_cnt_key || near_why != g_cnt_why || now >= g_cnt_next)) {
        g_cnt_key = near_key, g_cnt_why = near_why, g_cnt_next = now + 10000;
        // round 23 B (design 7.4): the margin reason names this camp's own H and how much farther the host is
        char md[160] = "";
        const double h = erules::CampMargin(near_in.r);
        if (std::strcmp(near_why, "the host not its margin farther") == 0)
            std::snprintf(md, sizeof(md), " (the host %.1f m farther than her; this camp's margin H = clamp(0.3 x %.0f + 10, 15, 40) = %.0f m)",
                          near_in.d_h >= 0 ? near_in.d_h - near_in.d_g : -1.0, near_in.r, h);
        Log("[enemy] camp: no take %s (n=%u, guest %.1f m, host %.1f m, r %.0f m, engage %.0f m, margin %.0f m, loaded %d, phase %u, camp %u, hold %.1f s): %s%s",
            CampName(near_key), near_in.members, near_in.d_g, near_in.d_h, near_in.r, erules::CampEngage(near_in.r), h,
            near_in.host_loaded ? 1 : 0, near_in.phase, near_in.cp_phase, (std::max)(0.0, (g_camps[near_key].hold_until - now) / 1000.0), near_why, md);
    }
    if (g_camps.size() > 512) {
        for (auto it = g_camps.begin(); it != g_camps.end();) it = it->second.hers || now < it->second.hold_until ? std::next(it) : g_camps.erase(it);
    }
}
// Round 24: enemy_own <id> 3 / 4 at the walk: one member to her game alone (P1b's OWN; its camp held out of the camp decisions while
// it is hers) or back.
void SingleTick(double now, const double (&guest)[3], bool have_guest, const double (&hs)[3], bool have_host) {
    if (g_single_req.empty()) return;
    std::vector<SingleReq> req, keep;
    req.swap(g_single_req);
    for (const SingleReq& q0 : req) {
        SingleReq q = q0;
        if (!q.to_guest) {
            // round 25 R1: a give-back asked while RULE-B still holds its take: the hold ends (never handed over later)
            const std::size_t held = keep.size();
            keep.erase(std::remove_if(keep.begin(), keep.end(), [&](const SingleReq& k) { return k.id == q.id; }), keep.end());
            if (keep.size() != held) {
                Log("[enemy] single OWN id=%05X: its RULE-B hold dropped (a give-back was asked)", q.id);
                continue;
            }
            const auto it = g_hown.find(q.id);
            const char* why = it == g_hown.end() ? "never handed over" : it->second.e.pending ? "a change is pending" : it->second.e.owner != erules::Owner::guest ? "not hers" : nullptr;
            if (why) {
                Log("[enemy] single OWN id=%05X not given back: %s", q.id, why);
                continue;
            }
            HostRec& r = it->second;
            r.forced = false;
            HostBackStart(q.id, r, erules::OwnReason::forced, true, now);
            g_camp_last_take = now;
            Log("[enemy] single OWN id=%05X -> host (forced give-back; epoch %u%s)", q.id, r.e.epoch, g_single.count(q.id) ? "" : "; it was not a single take");
            continue;
        }
        const HostEnemy* h = nullptr;
        for (const HostEnemy& x : g_host)
            if (x.id == q.id) h = &x;
        const auto own = g_hown.find(q.id);
        const char* why = !h || !h->now.have_xf ? "not an enemy in the host's view"
                          : DownFreeze() ? "a player is down (no hand-over)"
                          : InCutscene() || saveguard::Busy() ? "the host is in a cutscene or saving"
                          : own != g_hown.end() && (own->second.e.owner == erules::Owner::guest || own->second.e.pending) ? "already hers or pending"
                          : !HostTakeable(*h, now) ? "not takeable (down, held, hidden, in a vehicle or out of the takes)"
                          : nullptr;
        if (why) {
            Log("[enemy] single OWN id=%05X not handed over: %s", q.id, why);
            continue;
        }
        const std::uint64_t key = CampKeyOf(h->id, h->actor);
        // round 25 R1 (enemy_single_rule): RULE-W (a camp with a member hers earlier: the whole camp) and RULE-B (only while it
        // faces her: within 60 deg, <= 25 m, alerted; else held, re-checked at this walk for 20 s)
        if (g_ecfg.enemy_single_rule) {
            erules::SingleIn si;
            si.rule_on = true;
            si.cp_phase = -1;  // (her CP's phase is not known to the host)
            for (const HostEnemy& x : g_host)
                if (x.id != h->id && CampKeyOf(x.id, x.actor) == key && g_was_hers.count(x.id)) ++si.earlier_hers;
            si.camp_allowed = (key & kCampCp) != 0 && !SingleCamp(key);
            if (have_guest) {
                si.dist_m = Dist3(h->now.pos, guest);
                si.off_deg = eprobe::OffDeg(YawOfXf(h->now.xf), h->now.pos[0], h->now.pos[1], guest[0], guest[1]);
            }
            const std::uint8_t ph = PhaseOf(h->actor);
            si.alerted = ph != erules::kPhaseUnknown && ph >= 4;
            const erules::SingleVerdict sv = erules::SingleRule(si);
            if (sv == erules::SingleVerdict::camp || sv == erules::SingleVerdict::hold_walk) {
                Log("[enemy] single OWN id=%05X held: %s (%u other member(s) of %s hers earlier; her CP unknown here)%s", h->id, erules::SingleVerdictText(sv),
                    si.earlier_hers, CampName(key), sv == erules::SingleVerdict::camp ? ": the whole camp is taken instead" : ": no camp take possible, not switched");
                if (sv == erules::SingleVerdict::camp) g_camps[key].force_take = true, g_camps[key].hold_until = 0;
                continue;
            }
            if (sv == erules::SingleVerdict::hold_behind) {
                const bool first = q.held_until == 0;
                if (first) q.held_until = now + kRuleBHoldMs;
                if (now > q.held_until) {
                    Log("[enemy] single OWN id=%05X not handed over: RULE-B held it %.0f s (Sam %.1f m, %.0f deg off, phase %u)", h->id, kRuleBHoldMs / 1000.0, si.dist_m,
                        si.off_deg, ph);
                    continue;
                }
                if (first)
                    Log("[enemy] single OWN id=%05X held: RULE-B (Sam %.1f m, %.0f deg off its facing, phase %u; needs <= %.0f m, <= %.0f deg, alerted)", h->id, si.dist_m,
                        si.off_deg, ph, erules::kSingleFaceM, erules::kSingleFaceDeg);
                keep.push_back(q);
                continue;
            }
            if (q.held_until > 0) Log("[enemy] single OWN id=%05X RULE-B passed after %.1f s (Sam %.1f m, %.0f deg off)", h->id, (now - (q.held_until - kRuleBHoldMs)) / 1000.0, si.dist_m, si.off_deg);
            else Log("[enemy] single OWN id=%05X R1: a single take (RULE-W: no member of %s hers earlier; RULE-B passed: Sam %.1f m, %.0f deg off, phase %u)", h->id,
                     CampName(key), si.dist_m, si.off_deg, ph);
        }
        HostRec& r = g_hown[h->id];
        r.actor = h->actor;
        r.d_g = have_guest ? Dist3(h->now.pos, guest) : -1, r.d_h = have_host ? Dist3(h->now.pos, hs) : -1;
        HostTakeOne(*h, r, now, erules::OwnReason::forced);
        g_camp_last_take = now;
        if (r.e.owner != erules::Owner::guest) {
            Log("[enemy] single OWN id=%05X not handed over: the host's copy did not freeze", q.id);
            continue;
        }
        g_single[h->id] = SingleRec{key, now};
        g_was_hers.insert(h->id);
        CampRec& cr = g_camps[key];
        cr.force_take = false, cr.t_change = now;
        unsigned left = 0, hers = 0;
        for (const HostEnemy& x : g_host) {
            if (x.id == h->id || CampKeyOf(x.id, x.actor) != key) continue;
            const auto o = g_hown.find(x.id);
            ++(o != g_hown.end() && o->second.e.owner == erules::Owner::guest ? hers : left);
        }
        Log("[enemy] single OWN id=%05X -> guest (forced; camp members left: %u; %s, %u other member(s) hers already; guest %.1f m, host %.1f m; epoch %u; "
            "its camp is not decided until it is back)",
            h->id, left, CampName(key), hers, r.d_g, r.d_h, r.e.epoch);
    }
    for (const SingleReq& q : keep) g_single_req.push_back(q);
}
void OwnHostTick(const TickIn& in, double now, const double (&guest)[3], bool have_guest) {
    if (!g_own_on) return;
    double hs[3] = {};
    const bool have_host = in.local_ok && LocalPlace(in.local, hs);
    GuestDrain(now);  // her SEN1 / SEP1 (the enemies she owns)
    HostFollow(now, have_host ? hs : nullptr);
    // a back whose OWNACK did not come: released anyway from her newest frame
    for (auto& kv : g_hown) {
        HostRec& r = kv.second;
        if (!erules::HostBackTimedOut(r.e, now)) continue;
        ++g_os.no_owner;
        const auto it = g_tracks.find(kv.first);
        const erules::Sample* z = it != g_tracks.end() ? it->second.Newest() : nullptr;
        HostBackDone(kv.first, r, z ? z->pos : nullptr, z ? z->yaw : 0.0f, r.g_phase, "no OWNACK in 3 s: her newest frame", z ? z->state : 0);
    }
    // a save of the host's world never captures a copy we froze: everything back first. Every frozen copy counts:
    // the followers' and the returns a cutscene holds (g_pending: the host freezes no P1a mirror, so all of them are its own;
    // review rv-safety 4). (Busy is polled once a tick: a capture in the very frame a save is posted is not ruled out.)
    // (a far follower is not frozen: unrealized on the host, its AI not running, nothing of ours in its state. A save with only far
    // followers keeps her camps hers: review rv-gameplay 2, rv-correctness R4)
    bool any_frozen = !g_pending.empty();
    for (const Mirror& m : g_mirrors) any_frozen = any_frozen || (m.own && m.frozen);
    if (any_frozen && saveguard::Busy()) {
        if (!g_pending.empty()) {
            std::vector<Mirror> p;
            p.swap(g_pending);
            PublishMirrors();
            for (const Mirror& m : p) PostRelease(m, "the host's game saves (a cutscene held it)");
            Log("[enemy] own: %zu held return(s) posted at once (the host's game saves)", p.size());
        }
        HostOwnReset("the host's game saves", true, false);
        return;
    }
    MirrorLine(now);
    CarryTick(now);
    if (now < g_own_walk_next) return;
    g_own_walk_next = now + 500;
    CampWalk(now);
    g_change_times.erase(std::remove_if(g_change_times.begin(), g_change_times.end(), [&](double t) { return now - t >= 1000.0; }), g_change_times.end());
    g_refuse_times.erase(std::remove_if(g_refuse_times.begin(), g_refuse_times.end(), [&](double t) { return now - t >= 60000.0; }), g_refuse_times.end());
    const bool host_cut = InCutscene() || saveguard::Busy();  // (no take while the host's game saves either)
    const bool guard_ready = g_sam_filter && shots::MirrorGuardReady();
    double hsp[3] = {hs[0], hs[1], hs[2]};
    SingleTick(now, guest, have_guest, hsp, have_host);  // (round 24: enemy_own 3 / 4)
    CampHostTick(now, guest, have_guest, hsp, have_host, host_cut, guard_ready);
}

// ---- the guest
void GuestAck(std::uint32_t id, std::uint16_t epoch, erules::Owner owner, std::uint8_t result, std::uint8_t refusal, std::uintptr_t actor) {
    ewire::Own o = OwnOf(id, epoch, owner, static_cast<std::uint8_t>(erules::OwnReason::ack), PhaseOf(actor));
    o.result = result, o.refusal = refusal;
    ActorNow a;
    if (actor && ReadActor(actor, id, a) && a.have_xf) {
        const auto it = g_gown.find(id);
        PlaceInto(o, a.pos, YawOfXf(a.xf), it != g_gown.end() ? it->second.vel : nullptr);
    }
    OwnSend(ewire::kScbOwnAck, o);
}
void GuestTake(const ewire::Own& o, GuestRec& r, double now) {
    Mirror* m = FindMirror(o.id);
    ActorNow a;
    erules::OwnRefuse code = erules::OwnRefuse::none;
    // P1c: a member of a camp the host takes that is no mirror here (P1a never entered it: not realized on the host, the host's Sam
    // far) already runs in her world: taken as it stands (nothing unfrozen) when it is realized, alive, free and shown here
    const std::uintptr_t loose = (!m || !m->frozen) ? LocalActor(o.id) : 0;
    if (loose) {
        if (!ReadActor(loose, o.id, a) || !a.realized) code = erules::OwnRefuse::not_realized;
        else if (a.dead) code = erules::OwnRefuse::dead;
        else if (a.held) code = erules::OwnRefuse::held;  // a game sequence holds it here
        else if (a.hidden_or_asleep) code = erules::OwnRefuse::hidden;
        else if (g_holding || InCutscene()) code = erules::OwnRefuse::cutscene;
        else if (OwnedHereCount() >= erules::kMaxGuestOwnedMembers) code = erules::OwnRefuse::cap;
    } else if (!m || !m->frozen) code = erules::OwnRefuse::not_mirror;
    else if (!ReadActor(m->actor, o.id, a) || !a.realized || a.entity != m->entity) code = erules::OwnRefuse::not_realized;
    else if (a.dead) code = erules::OwnRefuse::dead;
    else if (!a.held) code = erules::OwnRefuse::held;  // bit 14 not ours any more: a game sequence has it
    else if (g_holding || InCutscene()) code = erules::OwnRefuse::cutscene;
    else if (OwnedHereCount() >= erules::kMaxGuestOwnedMembers) code = erules::OwnRefuse::cap;
    // (her camp alerted: taken all the same; its push makes it join her camp's fight here, logged below. Refusing would keep
    // every MULE of a camp that once saw her the host's until that camp calms: run 20261124-151942)
    if (code != erules::OwnRefuse::none) {
        r.g.owned = false;
        ++g_os.refused_here;
        GuestAck(o.id, o.epoch, erules::Owner::host, ewire::kOwnRefused, static_cast<std::uint8_t>(code), m ? m->actor : 0);
        Log("[enemy] OWN id=%05X refused here (epoch %u): %s", o.id, o.epoch, erules::OwnRefuseText(static_cast<std::uint8_t>(code)));
        return;
    }
    const std::uintptr_t actor = loose ? loose : m->actor;
    // the host's place and phase: a mirror only (it stood frozen at the host's frames). A loose one already runs in her world: as it
    // stands, its phase hers (review rv-correctness B1)
    double err = 0;
    if (!loose && (o.flags & ewire::kOwnHasPlace)) {
        const double p[3] = {o.pos[0], o.pos[1], o.pos[2]};
        err = Dist3(p, a.pos);
        if (err > erules::kBandStillM) ActorXf(actor, kActorTeleport, XfFromYaw(a.xf, p, o.yaw));
    }
    // round 25: her take hooks (the D3 lines, C1 before the SetPhase; K1 / T1 after the unfreeze). "alone": no other member of its
    // camp is hers here (her game does not know the host's single). Round 26 (the levers on in real games): and a forced take (the
    // host's enemy_own), so the first member of an ordinary camp take (proximity) is never "alone"; and the only forced take of
    // its OWN batch, so the first member of a forced whole-camp take (enemy_own .. 2, R1's camp verdict) is not either
    CampView cv_before;
    const bool forced = o.reason == static_cast<std::uint8_t>(erules::OwnReason::forced);
    const bool camp_read = forced && CampViewOf(o.id, &cv_before);
    const bool alone = eprobe::TakeAlone(forced, camp_read, cv_before.owned, g_drain_forced);
    r.alone = alone;
    ecombat::TakeBefore(o.id, actor, alone);
    // (calm there: SetPhase, exact)
    const std::uint8_t ph_before = PhaseOf(actor);
    if (!loose && o.phase <= erules::kPhaseMax && ph_before != erules::kPhaseUnknown && ph_before != o.phase) CallPhase(kSetPhase, actor, o.phase);
    // released here: her AI runs it from now (her hits on it are real)
    const std::uint32_t ea4_before = U32(actor + kMuleSeq);
    Mirror mm;
    if (!loose) {
        TakeMirrorOut(o.id, &mm);
        UnfreezeCopy(o.id, actor, mm.bit13_pre);
    }
    const std::uint32_t ea4_after = U32(actor + kMuleSeq);
    r.g.owned = true;
    r.actor = actor, r.entity = a.entity, r.t_taken = now;
    r.have_base = false, r.damaged = false, r.last_t = -1e18, r.have_pos = false;
    r.vel[0] = r.vel[1] = r.vel[2] = 0;
    g_tracks.erase(o.id), g_prx.erase(o.id);
    PublishOwned();
    ++g_os.taken_here;
    g_gsen_next = 0;  // her SEN1 for it at once
    GuestAck(o.id, o.epoch, erules::Owner::guest, ewire::kOwnTaken, 0, actor);
    Log("[enemy] OWN id=%05X taken here (epoch %u; err %.2f m; phase %u->%u; her camp %u; ea4 0x%X->0x%X; weapon268 %d%s)", o.id, o.epoch, err, ph_before,
        PhaseOf(actor), CampPhaseOf(actor), ea4_before, ea4_after, a.entity ? static_cast<int>(WeaponFlag(a.entity)) : -1, loose ? "; no mirror here: it ran here already" : "");
    ecombat::TakeAfter(o.id, actor, a.entity, alone);
}
// Her copy frozen again (a P1a mirror from now: the host's frames drive it; its lease's silence clock starts now).
bool GuestRefreeze(std::uint32_t id, GuestRec& r, double now, std::uint32_t* ea4_out, bool* realized_out, bool* dead_out = nullptr) {
    ActorNow a;
    const bool same = ReadActor(r.actor, id, a);
    bool frozen = false, b13 = false;
    std::uint32_t ea4 = same ? a.ea4 : 0;
    CaptureOffFor(id);
    const std::uint32_t st = same ? U32(r.actor + 0xEA0) : 0;
    if (same && a.realized && (a.dead || st == 3 || st == 5)) {
        // dead here (she killed it while it was hers): never frozen and posed from the host's standing frames (a corpse that
        // walks; review rv-safety 6). Left lying as her world's own corpse (nothing written; Eligible refuses a dead one, so it
        // is never mirrored again). The host's copy comes back alive: the known gap (her outcomes do not reach the host yet,
        // docs/enemy-sync.md 10)
        Log("[enemy] OWN id=%05X dead here: not frozen again (her world's corpse; the host's copy is released alive)", id);
        if (dead_out) *dead_out = true;
    } else if (same && a.realized) {
        frozen = FreezeCopy(id, r.actor, &b13, &ea4);
        if (frozen) {
            AddFollower(id, r.actor, a, b13, ea4, now, false);
            ++g_gs.on;
            Log("[enemy] mirror on id=%05X (given back to the host: frozen again here, ea4 0x%X; the host's frames from now)", id, ea4);
            ecombat::Refreeze(id, a.entity);  // round 25 T1
        }
    }
    r.g.owned = false, r.alone = false;
    ecombat::TakeEnded(id, true);  // round 25 R2: its role back, frozen or not
    g_tracks.erase(id), g_prx.erase(id);
    if (ea4_out) *ea4_out = ea4;
    if (realized_out) *realized_out = same && a.realized;
    return frozen;
}
void GuestGiveBack(const ewire::Own& o, GuestRec& r, double now) {
    std::uint32_t ea4 = 0;
    bool realized = false, dead = false;
    const bool frozen = GuestRefreeze(o.id, r, now, &ea4, &realized, &dead);
    PublishOwned();
    ++g_os.given_here;
    std::uint8_t refusal = 0;
    if (realized && !frozen && !dead) refusal = static_cast<std::uint8_t>(erules::OwnRefuse::fault), ++g_os.two_thinking;
    GuestAck(o.id, o.epoch, erules::Owner::host, ewire::kOwnReleased, refusal, r.actor);
    Log("[enemy] OWN id=%05X given back here (epoch %u; %s ea4 0x%X)", o.id, o.epoch,
        frozen ? "frozen" : dead ? "dead here: left lying, not frozen;" : realized ? "the freeze did NOT take (it runs here until P1a's next entry):" : "not realized here:",
        ea4);
}
// A reset on the guest: `refreeze` (the link and mirroring stay: a new generation) her copies are frozen again as P1a mirrors;
// else (the link, the gate, a world) she is solo again: they keep running in her world. `world_gone` (round 26): that world's
// actors are gone or going (R2's role is then never written back: an address can repeat after a load).
void GuestOwnReset(const char* why, bool refreeze, double now, bool world_gone = false) {
    unsigned n = 0;
    for (auto& kv : g_gown) {
        if (!kv.second.g.owned) continue;
        ++n;
        if (refreeze) GuestRefreeze(kv.first, kv.second, now, nullptr, nullptr);
        else ecombat::TakeEnded(kv.first, false, world_gone);  // round 25 R2: left running here: its own role back (not when the world is gone)
    }
    g_gown.clear();
    PublishOwned();
    if (n || refreeze) ++g_os.resets, Log("[enemy] own reset (%s): every enemy is the host's (gen %u; %u were the guest's%s)", why, g_own_gen, n,
                                         n ? refreeze ? ", frozen again here" : ", left running here (solo)" : "");
}
void GuestOwnMsg(const ewire::Own& o, double now) {
    if (erules::GenNewer(o.gen, g_own_gen)) {
        g_own_gen = o.gen;
        GuestOwnReset("a newer generation", true, now);
    } else if (o.gen != g_own_gen) {
        ++g_os.stale;
        return;
    }
    GuestRec& r = g_gown[o.id];
    switch (erules::GuestOnOwn(r.g, o.epoch, o.owner == 2 ? erules::Owner::guest : erules::Owner::host)) {
        case erules::GuestAct::ignore: ++g_os.stale; return;
        case erules::GuestAct::ack_taken: GuestAck(o.id, o.epoch, erules::Owner::guest, ewire::kOwnTaken, 0, r.actor); return;
        case erules::GuestAct::ack_released: GuestAck(o.id, o.epoch, erules::Owner::host, ewire::kOwnReleased, 0, r.actor); return;
        case erules::GuestAct::take: GuestTake(o, r, now); return;
        case erules::GuestAct::release: GuestGiveBack(o, r, now); return;
    }
}
void OwnGuestTick(const TickIn& in, double now) {
    if (!g_own_on) return;
    CampWalk(now);
    // the owned ones: what they are now
    if (now >= g_gwalk_next) {
        g_gwalk_next = now + 500;
        double body[3] = {};
        const bool have_body = in.body && LocalPlace(in.body, body);
        std::vector<erules::PoseCand> pc;
        unsigned ns = 0;
        bool entities_changed = false;
        for (auto& kv : g_gown) {
            GuestRec& r = kv.second;
            if (!r.g.owned) continue;
            ActorNow a;
            if (!ReadActor(r.actor, kv.first, a)) continue;
            if (a.realized && a.entity != r.entity) r.entity = a.entity, entities_changed = true;
            if (a.realized) NoteVitals(a.entity, r.have_base, r.base_h, r.base_s, r.damaged);
            if (g_ecfg.enemy_mirror_vehicle && now >= r.veh_next) {
                // round 23 B: every 1 s (her SEN1's kInVehicle: the host gives the member back while it is seated)
                r.veh_next = now + erules::kVehicleCheckMs;
                std::uint32_t e20 = erules::kVehicleIdNone;
                std::int32_t seat = -1;
                const erules::VehTest was = r.veh;
                r.veh = a.realized ? VehicleOf(kv.first, r.actor, a.entity, &e20, &seat) : erules::VehTest::none;
                if (r.veh != was) NoteVehicle(kv.first, "her owned", was, r.veh, e20, seat);
            }
            if (const std::uint8_t ph = PhaseOf(r.actor); ph != r.last_phase) {
                if (r.last_phase != 0xFF)
                    Log("[enemy] owned id=%05X phase %u -> %u (%s; its camp %u; weapon268 %d)", kv.first, r.last_phase, ph, erules::PhaseText(ph), CampPhaseOf(r.actor),
                        a.realized ? static_cast<int>(WeaponFlag(a.entity)) : -1);
                r.last_phase = ph;
            }
            if (a.realized && ns < erules::KeySet::kCap) g_shooter_keys[ns] = a.entity, g_shooter_vals[ns] = kv.first, ++ns;
            const bool down = a.dead || erules::OutcomeOf(U32(r.actor + 0xEA0), a.ea4) != erules::Defeat::none;
            pc.push_back(erules::PoseCand{kv.first, have_body && a.have_xf ? Dist3(a.pos, body) : -1, a.realized, down});
        }
        if (entities_changed) PublishOwned();
        shots::SetEnemyShooters(g_shooter_keys, g_shooter_vals, ns);
        UpdateCaptureSlots(pc.data(), static_cast<unsigned>(pc.size()), [](std::uint32_t id) -> std::uintptr_t {
            const auto it = g_gown.find(id);
            ActorNow a;
            return it != g_gown.end() && it->second.g.owned && ReadActor(it->second.actor, id, a) && a.realized ? mem::Q(a.entity + 0xB8) : 0;
        });
    }
    SendCaptures(now);
    // SEN1 for every owned one at 10 Hz (her frames: the host's follower and its ownership rules)
    if (now < g_gsen_next) return;
    g_gsen_next = now + 100;
    static ewire::EnemyFrame f;
    f.n = 0;
    f.flags = static_cast<std::uint16_t>(ewire::kFromGuest | (InCutscene() ? ewire::kSenderCutscene : 0));
    f.seq = ++g_gsen_seq;
    f.sender_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(now) & 0xFFFFFFFFull);
    f.gen_tag = g_own_gen;
    for (auto& kv : g_gown) {
        GuestRec& r = kv.second;
        if (!r.g.owned || f.n >= ewire::kSenMax) continue;
        ActorNow a;
        const bool same = ReadActor(r.actor, kv.first, a);
        if (same && a.have_xf) {
            if (r.last_t > -1e17 && now - r.last_t > 20 && now - r.last_t < 3000) {
                const double dt = (now - r.last_t) / 1000.0;
                for (int k = 0; k < 3; ++k) r.vel[k] = static_cast<float>((a.pos[k] - r.last_pos[k]) / dt);
            }
            std::memcpy(r.last_pos, a.pos, sizeof(r.last_pos));
            r.last_t = now, r.have_pos = true;
        }
        if (!r.have_pos) continue;
        ewire::Enemy& e = f.e[f.n];
        e = ewire::Enemy{};
        e.actor_id = kv.first;
        e.owner_epoch = static_cast<std::uint8_t>(r.g.epoch & 0xFF);
        e.state = static_cast<std::uint8_t>((same && a.realized ? ewire::kRealized : 0) | (same && a.dead ? ewire::kDead : 0) |
                                            (same && a.hidden_or_asleep ? ewire::kHidden : 0) | (same && a.held ? ewire::kSeqHeld : 0) |
                                            (g_pslots.SlotOf(kv.first) >= 0 ? ewire::kPosed : 0) |
                                            // her copy's outcome (dead or down: the host keeps it hers, never back by proximity)
                                            (same ? ewire::DefeatBits(static_cast<std::uint8_t>(erules::OutcomeOf(U32(r.actor + 0xEA0), a.ea4))) : 0));
        e.phase = same ? PhaseOf(r.actor) : erules::kPhaseUnknown;
        const double sp = std::sqrt(static_cast<double>(r.vel[0]) * r.vel[0] + static_cast<double>(r.vel[1]) * r.vel[1]);
        e.flags = static_cast<std::uint8_t>(ewire::kOwnerGuest | (sp > 0.2 ? ewire::kMoving : 0) | (r.damaged ? ewire::kDamaged : 0) |
                                            (r.veh != erules::VehTest::none ? ewire::kInVehicle : 0));
        for (int k = 0; k < 3; ++k) e.pos[k] = r.last_pos[k], e.vel[k] = r.vel[k];
        e.yaw = same && a.have_xf ? YawOfXf(a.xf) : 0.0f;
        ++f.n;
    }
    if (!f.n) return;
    std::uint8_t buf[ewire::kSenMaxSize];
    const std::size_t size = ewire::EncodeEnemies(f, buf, sizeof(buf));
    if (size && link::SendCombatFrame(buf, size)) ++g_os.sen_out;
}

// ---- the peer's frames under P1b's rules (GuestDrain)
bool AcceptPeerFrame(const ewire::EnemyFrame& f) {
    if (g_role == 1) {
        // the host takes only her frames (her owned enemies), this generation
        if (!(f.flags & ewire::kFromGuest) || !g_own_on) return false;
        g_guest_cut = (f.flags & ewire::kSenderCutscene) != 0;
        if (f.gen_tag != g_own_gen) {
            ++g_os.stale;
            return false;
        }
        return true;
    }
    if (f.flags & ewire::kFromGuest) return false;
    // the guest: a newer host generation resets her table (her copies frozen again)
    if (g_own_on && erules::GenNewer(f.gen_tag, g_own_gen)) {
        g_own_gen = f.gen_tag;
        GuestOwnReset("a newer generation (SEN1)", true, g_tnow);
    } else if (!g_own_on && f.gen_tag) {
        g_own_gen = f.gen_tag;
    }
    return true;
}
bool AcceptPeerEntry(const ewire::EnemyFrame& f, const ewire::Enemy& e, double now) {
    if (g_role == 1) {
        const auto it = g_hown.find(e.actor_id);
        if (it == g_hown.end() || it->second.e.owner != erules::Owner::guest || !(e.flags & ewire::kOwnerGuest) ||
            e.owner_epoch != static_cast<std::uint8_t>(it->second.e.epoch & 0xFF)) {
            ++g_os.stale;
            return false;
        }
        HostRec& r = it->second;
        r.t_rx = now, r.g_phase = e.phase, r.g_state = e.state, r.g_damaged = (e.flags & ewire::kDamaged) != 0;
        return true;
    }
    (void)f;
    if (IsOwnedHere(e.actor_id)) {
        ++g_os.stale;  // the host's frame of one she owns (in flight across the hand-over)
        return false;
    }
    return true;
}
bool AcceptPeerPose(std::uint32_t id) {
    if (g_role == 1) {
        const auto it = g_hown.find(id);
        return it != g_hown.end() && it->second.e.owner == erules::Owner::guest;
    }
    return !IsOwnedHere(id);
}

// ---- the SCB1 ownership messages (game thread)
void OwnDrain(double now) {
    if (!ecombat::DrainsCombat()) {
        // ecombat is not draining the reliable channel: the ownership messages are taken here, the rest dropped
        std::uint8_t buf[1100];
        std::size_t n = 0;
        std::uint32_t gen = 0;
        while (link::PopCombat(buf, sizeof(buf), n, gen)) {
            if (gen != link::Generation() || n < 5) continue;
            if (buf[4] == ewire::kScbOwn || buf[4] == ewire::kScbOwnAck || buf[4] == ewire::kScbOwnReset) g_own_in.emplace_back(buf, buf + n);
        }
    }
    if (g_own_in.empty()) return;
    std::vector<std::vector<std::uint8_t>> in;
    in.swap(g_own_in);
    static ewire::Own os[ewire::kOwnMax];
    // round 26: the forced takes to her in this batch (a forced whole-camp take is one OWN per member, sent in one host tick: none of
    // them counts as a single take, eprobe::TakeAlone)
    g_drain_forced = 0;
    if (g_role == 2 && g_own_on)
        for (const auto& m : in) {
            ewire::ScbHeader h;
            if (!ewire::DecodeScbHeader(m.data(), m.size(), h) || h.type != ewire::kScbOwn || !ewire::DecodeOwns(m.data(), m.size(), h, os)) continue;
            for (unsigned i = 0; i < h.n; ++i)
                if (os[i].owner == 2 && os[i].reason == static_cast<std::uint8_t>(erules::OwnReason::forced)) ++g_drain_forced;
        }
    for (const auto& m : in) {
        ewire::ScbHeader h;
        if (!ewire::DecodeScbHeader(m.data(), m.size(), h) || (g_fp_done && h.guids_fp != g_guids)) {
            ++g_os.stale;
            continue;
        }
        if (h.type == ewire::kScbOwnReset) {
            std::uint32_t gen = 0;
            if (g_role == 2 && ewire::DecodeOwnReset(m.data(), m.size(), h, &gen) && erules::GenNewer(gen, g_own_gen)) {
                g_own_gen = gen;
                GuestOwnReset("the host's reset", true, now);
            }
            continue;
        }
        if (!g_own_on || !ewire::DecodeOwns(m.data(), m.size(), h, os)) {
            ++g_os.stale;
            continue;
        }
        for (unsigned i = 0; i < h.n; ++i) {
            if (h.type == ewire::kScbOwn && g_role == 2) GuestOwnMsg(os[i], now);
            else if (h.type == ewire::kScbOwnAck && g_role == 1 && os[i].gen == g_own_gen) HostAck(os[i], now);
            else ++g_os.stale;
        }
    }
    g_drain_forced = 0;
}

// A reset of the whole table (a world or the link changed, the gate, the switch, a fault): the host's copies released (placed at
// her newest frames unless that world is gone), the guest's left running (solo) or frozen again (`refreeze`: mirroring stays);
// `announce`: OWNRESET to the guest (the link stays).
void OwnHardReset(const char* why, bool world_gone, bool announce, bool refreeze) {
    if (!g_hown.empty() || (g_role == 1 && g_own_on)) HostOwnReset(why, announce, world_gone);
    if (!g_gown.empty() || (g_role == 2 && g_own_on)) GuestOwnReset(why, refreeze && !world_gone, g_tnow, world_gone);
    if (g_role == 2) HostResetPoses();  // her captures
    g_hown.clear(), g_gown.clear();
    PublishOwned();
    g_own_in.clear();
    if (!announce) g_own_out.clear();
    g_change_times.clear(), g_refuse_times.clear();
    g_guest_cut = false;
    g_own_on = false;
}

void OwnLine(double now, bool force) {
    if (!force && (!g_own_on || now < g_own_log_next)) return;
    g_own_log_next = now + 5000;
    char ids[160] = {};
    std::size_t at = 0;
    unsigned here = 0, pending = 0;
    if (g_role == 2) {
        for (const auto& kv : g_gown)
            if (kv.second.g.owned) {
                ++here;
                if (at + 8 < sizeof(ids)) at += static_cast<std::size_t>(std::snprintf(ids + at, sizeof(ids) - at, "%s%05X", at ? " " : "", kv.first));
            }
    } else {
        here = static_cast<unsigned>(g_host.size()) - (std::min)(static_cast<unsigned>(g_host.size()), GuestOwnedCount());
        for (const auto& kv : g_hown)
            if (kv.second.e.owner == erules::Owner::guest && at + 8 < sizeof(ids))
                at += static_cast<std::size_t>(std::snprintf(ids + at, sizeof(ids) - at, "%s%05X", at ? " " : "", kv.first));
    }
    for (const auto& kv : g_hown) pending += kv.second.e.pending ? 1u : 0u;
    static unsigned sen_logged = 0;
    const double sen = (g_os.sen_out - sen_logged) / 5.0;
    sen_logged = g_os.sen_out;
    Log("[enemy] owned here: n=%u (ids %s); guest-owned n=%u (%s); pending %u; no-owner>1s %u; two-thinking %u; SEN1 out %.1f/s; changes: take %u back %u forced %u "
        "refused %u; here: taken %u given back %u refused %u; stale %u; owned hits on our Sam %u",
        here, g_role == 2 && at ? ids : "-", g_role == 1 ? GuestOwnedCount() : here, g_role == 1 && at ? ids : "-", pending, g_os.no_owner, g_os.two_thinking, sen,
        g_os.takes, g_os.backs, g_os.forced, g_os.refused, g_os.taken_here, g_os.given_here, g_os.refused_here, g_os.stale, g_owned_hits.load());
}

// The ownership byte checks. own false (round 27 run 2, enemy_owner=0): the same sites only for the camp walk the L1 probe's re-calm
// runs on demand (CampWalkNow): g_cp_ok, ownership stays off.
bool OwnChecks(bool own = true) {
    struct S {
        std::uintptr_t va;
        const char* sig;
        const char* name;
        bool cp;
    };
    // clang-format off
    const S sites[] = {
        {kSetPhase,        "3B 91 10 01 00 00 74 0A C7 81 18 01 00 00 00 00 80 3F 8D 42 FD 89 91 10 01 00 00 83 F8 02 77 0A", "SetPhase", false},
        {kRisePhase,       "8B 81 10 01 00 00 3B D0 44 8B C0 44 0F 4F C2 44 3B C0 74 0A C7 81 18 01 00 00 00 00 80 3F", "RisePhase", false},
        {kGetPhaseRead,    "48 8B 43 30 48 8B 48 58 48 85 C9 74 08 8B 91 10 01 00 00 EB 05 BA FF 00 00 00", "GetPhase's read of +0x110", false},
        {kCampLookup,      "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 99 E0 00 00 00 48 8B FA 48 63 81 D8 00 00 00", "the camp lookup (+0xD8 / +0xE0)", true},
        {kCampPhaseSite,   "48 8D 98 08 02 00 00 83 FF 05", "the camp phase (+0x210)", true},
        {kCampMembersSite, "48 63 86 C0 04 00 00 4C 8B B6 C8 04 00 00", "the camp members (+0x4C0 / +0x4C8)", true},
    };
    // clang-format on
    unsigned n = 0, ok = 0;
    bool main_ok = true, cp_ok = true;
    for (const S& s : sites) {
        ++n;
        if (mem::CodeMatches(A(s.va), s.sig)) {
            ++ok;
        } else {
            Log("[enemy] byte check FAILED: %s at 0x%llX (own)", s.name, static_cast<unsigned long long>(s.va));
            (s.cp ? cp_ok : main_ok) = false;
        }
    }
    g_cp_ok = main_ok && cp_ok;
    if (own) Log("[enemy] own byte checks: %u/%u ok%s", ok, n, !main_ok ? " (ownership off)" : !cp_ok ? " (the camp phase unknown)" : "");
    else Log("[enemy] camp byte checks (enemy_owner=0: the camp walk on demand only): %u/%u ok%s", ok, n, g_cp_ok ? "" : " (no camp walk)");
    return main_ok;
}

// ---------------------------------------------------------------- the gate and the tick
void AttackLine(const char* why) {
    unsigned seen = 0, dropped = 0;
    shots::MirrorShotCounts(&seen, &dropped);
    Log("[enemy] mirror attacks%s: shots %u (dropped %u), hits on our Sam dropped %u (seen %u), kept 0; hits on mirrors dropped %u (ours %u replays %u others %u), "
        "rcx matched %u (+-0x20 %u, -0x38 %u); bullet hits on mirrors dropped at the bullet %u (our Sam's own rounds %u, replays %u)",
        why, seen, dropped, g_att_drop.load(), g_att_seen.load(), g_tgt_drop.load(), g_tgt_ours.load(), g_tgt_replays.load(), g_tgt_others.load(), g_tgt_rcx.load(),
        g_tgt_rcx20.load(), g_tgt_rcx38.load(), shots::MirrorBulletDrops(), shots::MirrorBulletDropsOurs(), shots::MirrorBulletDropsReplays());
}

std::uint8_t OurParts() {
    if (!g_want || !g_p1_ok || !g_switch || g_stopped) return 0;
    std::uint8_t p = shotw::kPartMirror;
    // P1b: ownership (enemy_owner, its byte checks, enemy_sync 8; the host also needs the save reads: no save captures a copy it froze)
    if (g_own_want && g_own_ok && g_own_switch && (g_role != 1 || saveguard::ReadsOk())) p |= shotw::kPartCamp;  // (P1c: per camp. Never kPartOwn: a v0.33 host would run P1b against us; review rv-correctness R1)
    return p;
}

void Stop(const char* step) {
    l1probe::ReleaseBeforeFree("enemy sync stopped");  // round 27 M1 (C1)
    etarget::Stop("enemy sync stopped");             // round 27 M2 / M3: the partner stimulus released, the pick and redirects off
    g_stopped = true;
    Log("[enemy] FAULT in %s: enemy sync stopped", step);
    UnhideDead(true, "a fault");
    UnhideHostHidden(true, "a fault", 0);  // (review r20 enemy 1: Tick never reaches GuestTick again)
    UnhideNotHolding(true, "a fault", 0);  // round 23 review: the not-holding copies too (GuestTick's retry never runs again)
    SeatShowAll("a fault", 0);             // round 28 T1
    g_nothold_n.clear();
    DownResume("a fault");  // round 23 C
    g_peer_down = g_our_down = 0;
    shots::SetPlayerDown(0);
    OwnHardReset("a fault", false, false, false);
    ReleaseAll("fault");
    RestoreFar("fault");
    HostReset();
    g_mirroring = false;
    shots::SetEnemyShots(0);
    shots::SetEnemyBeacon(0, 0, 0, 0, 0);
}

// ---- round 23 C (enemy_down_hold; docs/enemy-sync.md 14.9): a player down (death or KnotSpace). Ours read from the game state
// (GameOver or KnotSpace; a faint never counts), latched (back after 1 s of not down), told to the partner in our SEH1 beacon
// (byte 22: only to a partner that advertises kPartRestrain). The partner's from its beacon, or the enemy_sync 9 fake (hidden runs).
const char* DownWho(bool ours) {
    const int role = g_role.load();
    return (role == 1) == ours ? "host Sam" : "guest Fragile";
}
const char* DownNoun() { return g_role.load() == 1 ? "followers" : "mirrors"; }
unsigned DownHiddenNow() {
    unsigned n = static_cast<unsigned>(g_down_hid.size());
    for (const std::uint32_t id : g_down_fhid) n += g_fhid.count(id) ? 1u : 0u;
    return n;
}
// The partner back (or the hold ended): every copy we hid shown again, the held ones driven again from the next tick.
unsigned DownResume(const char* why) {
    unsigned shown = 0;
    while (!g_down_hid.empty()) shown += DownUnhideMirror(g_down_hid.begin()->first, why) ? 1u : 0u;
    for (const std::uint32_t id : g_down_fhid) {
        if (!g_fhid.count(id)) continue;
        FollowerShow(id, why);
        ++shown;
    }
    g_down_fhid.clear();
    g_down_held.clear();
    g_down_kept = false;
    return shown;
}
// A world change: that world's actors are gone (an address and id can repeat after a reload): forgotten, never touched. The hold ends
// (a partner still down is seen again from its beacon next tick: no new mirror meanwhile).
void DownForget(const char* why) {
    if (g_peer_down || !g_down_hid.empty() || !g_down_held.empty())
        Log("[enemy] down hold ended: %s (%zu held, %zu hidden forgotten)", why, g_down_held.size(), g_down_hid.size());
    g_down_hid.clear(), g_down_held.clear(), g_down_fhid.clear();
    g_peer_down = 0;
    g_down_kept = false;
}
// The announce line once the first pass under the hold has run (GuestTick / HostFollow this tick).
bool g_down_announce = false;
const char* g_down_src = "";
void DownAnnounce() {
    g_down_announce = false;
    unsigned usual = 0;
    const bool host = g_role.load() == 1;
    for (const Mirror& m : g_mirrors)
        if (m.frozen && m.own == host && !g_down_held.count(m.id) && !g_down_hid.count(m.id) && !g_down_fhid.count(m.id)) ++usual;
    Log("[enemy] %s down (%s; %s): holding %zu %s, hiding %u (%u far and standing under the usual rules; radius %.0f m; no new %s, no ownership change "
        "until back)", DownWho(false), erules::PlayerDownText(g_peer_down), g_down_src, g_down_held.size(), DownNoun(), DownHiddenNow(), usual,
        g_ecfg.enemy_down_hold_radius, host ? "take" : "mirror");
}
void DownTick(double now, bool have_peer, const shotw::Beacon& peer) {
    const int role = g_role.load();
    if (role != 1 && role != 2) {
        if (g_peer_down || g_our_down) {
            const unsigned shown = DownResume("no partner role");
            Log("[enemy] down hold ended: no co-op role (shown again %u)", shown);
        }
        g_peer_down = g_our_down = 0;
        g_our_latch.Reset();
        shots::SetPlayerDown(0);
        return;
    }
    // ours
    std::uint64_t flags = 0;
    std::uint32_t mask = 0;
    const std::uint8_t raw = game::ReadStopGates(&flags, &mask) ? erules::DownOfState(flags) : std::uint8_t{0};
    const std::uint8_t latched = g_our_latch.Step(raw, now);
    const std::uint8_t ours = g_our_fake >= 0 ? static_cast<std::uint8_t>(g_our_fake) : latched;
    const std::uint8_t carried = shots::SetPlayerDown(ours);
    if (ours != g_our_down) {
        if (ours)
            Log("[enemy] our %s down (%s%s): told the partner in our beacon%s", role == 1 ? "Sam" : "Fragile", erules::PlayerDownText(ours),
                g_our_fake >= 0 ? "; enemy_sync 9 fake" : "", carried == ours ? "" : " (not sent: the partner's build or settings cannot take it)");
        else
            // round 23 F: a cleared fake (its codes are the test ones) is no "1 s not down"
            Log("[enemy] our %s back (%s): told the partner", role == 1 ? "Sam" : "Fragile",
                g_our_down == static_cast<std::uint8_t>(erules::PlayerDown::test_down) || g_our_down == static_cast<std::uint8_t>(erules::PlayerDown::test_limbo)
                    ? "enemy_sync 9 fake cleared"
                    : "1 s not down");
        g_our_down = ours;
    }
    // a frozen copy we hid that is no frozen mirror any more (taken by a game sequence, handed over): shown again
    for (auto it = g_down_hid.begin(); it != g_down_hid.end();) {
        const Mirror* m = FindMirror(it->first);
        if (m && m->frozen) {
            ++it;
            continue;
        }
        const std::uint32_t id = it->first;
        ++it;
        DownUnhideMirror(id, "no longer a frozen mirror here");
    }
    // the partner's
    // (review: no beacon, none yet or none for 20 s, keeps the code in force: "unknown", never "back")
    const std::uint8_t pd = erules::PeerDownNow(g_peer_fake, have_peer, peer.player_down, g_peer_down);
    if (!have_peer && g_peer_fake < 0 && g_peer_down && !g_down_nobeacon_logged) {
        g_down_nobeacon_logged = true;
        Log("[enemy] %s down, and no beacon from the partner now: the hold kept (no beacon is no \"back\")", DownWho(false));
    } else if (have_peer) {
        g_down_nobeacon_logged = false;
    }
    const erules::DownChange ch = erules::DownStep(g_peer_down, pd);
    if (ch == erules::DownChange::none) return;
    g_down_src = g_peer_fake >= 0 ? "enemy_sync 9 fake" : have_peer ? "its beacon" : "no beacon";
    if (ch == erules::DownChange::back) {
        const unsigned held = static_cast<unsigned>(g_down_held.size());
        const unsigned shown = DownResume(role == 1 ? "her Fragile is back" : "the host's Sam is back");
        g_peer_down = 0;
        g_down_announce = false;
        ++g_ds.resumed;
        if (g_ecfg.enemy_down_hold)
            Log("[enemy] %s back: resuming %u (held %u, shown again %u; down %.1f s; %s)", DownWho(false), held + shown, held, shown, (now - g_down_t0) / 1000.0,
                g_down_src);
        else
            Log("[enemy] %s back (%s; enemy_down_hold=0)", DownWho(false), g_down_src);
        return;
    }
    if (ch == erules::DownChange::down) g_down_t0 = now, ++g_ds.downs;
    g_peer_down = pd;
    if (!g_ecfg.enemy_down_hold) {
        Log("[enemy] %s down (%s; %s): enemy_down_hold=0, the old rules (its copies given back to their own game)", DownWho(false), erules::PlayerDownText(pd),
            g_down_src);
        return;
    }
    if (ch == erules::DownChange::reason) {
        Log("[enemy] %s still down, now %s (%s)", DownWho(false), erules::PlayerDownText(pd), g_down_src);
        return;
    }
    g_down_announce = true;  // logged after this tick's pass (DownAnnounce)
}

void P1Tick(const TickIn& in, double now) {
    g_tnow = now;
    g_body = in.body;
    g_our_sam.store(in.local_ok ? reinterpret_cast<std::uintptr_t>(in.local) : 0, std::memory_order_relaxed);
    g_role = in.net ? in.role : 0;
    // the world: its counter, the link session, the private room
    std::uint32_t counter = 0;
    const bool ready = coop::WorldReady(&counter) && !InPrivateRoom();
    const std::uint32_t gen = link::Generation();
    if (ready != g_ready || counter != g_world || gen != g_gen) {
        const bool world_changed = counter != g_world || ready != g_ready;
        if (g_mirroring || !g_mirrors.empty()) Log("[enemy] the world or the link changed (world %u -> %u%s, link %u -> %u): every mirror released", g_world, counter,
                                                   ready ? "" : ", not ready", g_gen, gen);
        if (g_own_on || !g_hown.empty() || !g_gown.empty()) OwnHardReset(world_changed ? "a world changed" : "the link changed", world_changed, false, false);
        if (counter != g_world) {
            DownForget("the world changed");  // round 23 C
            // a new world: the old one's actors are gone (an address and id can repeat after a reload): forgotten, never touched
            // (review r20 enemy 1)
            if (!g_dead_hidden.empty() || !g_host_hid.empty() || !g_nothold.empty())
                Log("[enemy] the world changed: %zu + %zu + %zu hidden mirror(s) of the old world forgotten", g_dead_hidden.size(), g_host_hid.size(),
                    g_nothold.size());
            g_dead_hidden.clear(), g_host_hid.clear();
            g_nothold.clear(), g_nothold_n.clear();  // round 23 review: the not-holding ones too (never touched in the new world)
            if (!g_seat_hid.empty()) Log("[enemy] the world changed: %zu seated (V3) mirror(s) of the old world forgotten", g_seat_hid.size());
            g_seat_hid.clear();  // round 28 T1 (likewise)
            g_gs.host_hidden_now = 0;
        } else {
            UnhideDead(true, world_changed ? "the world changed" : "the link changed");
            UnhideHostHidden(true, world_changed ? "the world changed" : "the link changed", now);
            UnhideNotHolding(true, world_changed ? "the world changed" : "the link changed", now);  // round 23 review
            const unsigned shown = DownResume(world_changed ? "the world changed" : "the link changed");  // round 23 C
            if (g_peer_down) Log("[enemy] down hold ended: %s (shown again %u)", world_changed ? "the world changed" : "the link changed", shown);
            g_peer_down = 0;
        }
        if (gen != g_gen) {
            if (g_peer_fake >= 0 || g_our_fake >= 0) Log("[enemy] enemy_sync 9: the fakes cleared (the link changed)");
            g_peer_fake = g_our_fake = -1;
        }
        ReleaseAll(world_changed ? "world" : "link");
        if (world_changed) g_far_orig.clear();  // that world's actors: nothing to put back
        else RestoreFar("link");
        HostReset();
        g_tracks.clear(), g_prx.clear(), g_clock.Reset();
        if (world_changed) g_fp_done = false, g_count = 0, g_ids = g_guids = 0, g_differ_logged = false, g_fp_retry = 0;
        g_ready = ready, g_world = counter, g_gen = gen;
        g_ready_ms = now;
        g_mirroring = false;
    }
    // round 23 review: the not-holding copies shown and woken again once this game is no guest (link lost: GuestTick no longer runs)
    if (g_role != 2 && !g_nothold.empty()) UnhideNotHolding(true, "no guest role", now);
    // the fingerprint, once per world (5 s after it is ready)
    if (g_ready && !g_fp_done && now - g_ready_ms >= 5000 && now >= g_next_fp_try) {
        g_next_fp_try = now + 5000;
        std::uint32_t ids = 0, guids = 0;
        std::uint16_t count = 0;
        if (enemy::Fingerprint(&ids, &guids, &count)) {
            g_fp_done = true, g_ids = ids, g_guids = guids, g_count = count;
            Log("[enemy] fingerprint (world %u): count %u, ids %08X guids %08X", g_world, count, ids, guids);
        }
    }
    const std::uint8_t parts = OurParts();
    shots::SetEnemyBeacon(parts, g_fp_done ? g_count : 0, g_fp_done ? g_ids : 0, g_fp_done ? g_guids : 0, parts ? 1 : 0);
    // the gate
    shotw::Beacon peer;
    double age = 1e18;
    const bool have_peer = shots::PeerBeacon(&peer, &age);
    const erules::Print ours{parts, g_fp_done ? g_count : std::uint16_t{0}, g_fp_done ? g_ids : 0, g_fp_done ? g_guids : 0};
    const erules::Print theirs{peer.parts, peer.enemy_count, peer.ids_fp, peer.guids_fp};
    const erules::FpGate gate = in.net && g_ready && (g_role == 1 || g_role == 2) ? erules::FingerprintGate(ours, theirs, have_peer, age) : erules::FpGate::not_ours;
    DownTick(now, have_peer, peer);  // round 23 C: ours told, the partner's read (before this tick's pass)
    const bool on = gate == erules::FpGate::on;
    if (gate == erules::FpGate::differ && !g_differ_logged) {
        g_differ_logged = true;
        Log("[enemy] fingerprints differ (ids ours %08X theirs %08X, guids ours %08X theirs %08X, count %u/%u): enemy sync off until they match (ours "
            "recomputed every 30 s)", ours.ids, theirs.ids, ours.guids, theirs.guids, ours.count, theirs.count);
    }
    // spawn timing can make the two first prints differ (review rv-safety 10): ours again every 30 s while they do, logged on change
    if (gate == erules::FpGate::differ && g_fp_done && now >= g_fp_retry) {
        if (g_fp_retry > 0) {
            std::uint32_t ids = 0, guids = 0;
            std::uint16_t count = 0;
            if (enemy::Fingerprint(&ids, &guids, &count) && (ids != g_ids || guids != g_guids || count != g_count)) {
                Log("[enemy] fingerprint recomputed (world %u): count %u, ids %08X guids %08X (was %u, %08X, %08X)", g_world, count, ids, guids, g_count, g_ids, g_guids);
                g_ids = ids, g_guids = guids, g_count = count;
                g_differ_logged = false;
            }
        }
        g_fp_retry = now + 30000;
    }
    if (on != g_mirroring) {
        g_mirroring = on;
        if (on)
            Log("[enemy] sync on (%s): count %u, fp ids %08X guids %08X (equal: key = actor id); freeze occupied, poses %d, radius %.0f m, shots %d", g_role == 1 ? "host" : "guest",
                g_count, g_ids, g_guids, g_ecfg.enemy_pose_count, g_ecfg.enemy_mirror_radius, g_ecfg.enemy_shots ? 1 : 0);
        else
            Log("[enemy] sync off: %s", erules::FpGateText(gate));
        if (!on) {
            // round 23 review: not while the host's Sam is down on the guest (GuestTick keeps them frozen: DownKeep)
            if (erules::DownKeepsOff(g_ecfg.enemy_down_hold, g_peer_down, g_role == 2)) DownKeep(erules::FpGateText(gate));
            else ReleaseAll("off");
            HostReset();
        }
    } else if (!on && gate != g_last_gate && g_ready && in.net) {
        Log("[enemy] sync waiting: %s", erules::FpGateText(gate));
    }
    g_last_gate = gate;
    // P1c: ownership while both beacons carry kPartCamp (and mirroring is on). This build advertises kPartCamp alone: a v0.33 partner
    // (kPartOwn alone, per-enemy P1b) sees no ownership part and runs none with us
    constexpr std::uint8_t kOwnParts = shotw::kPartCamp;
    const bool own_on = on && (parts & kOwnParts) == kOwnParts && have_peer && (peer.parts & kOwnParts) == kOwnParts;
    if (own_on != g_own_on) {
        if (own_on) {
            g_own_on = true;
            g_own_walk_next = g_hfar_next = g_gsen_next = g_gwalk_next = g_cp_next = 0;
            Log("[enemy] own on (parts ours 0x%02X theirs 0x%02X; gen %u)", parts, peer.parts, g_own_gen);
        } else {
            const char* why = !on                                   ? "mirroring off"
                              : (parts & kOwnParts) != kOwnParts       ? "off here"
                              : have_peer && (peer.parts & shotw::kPartOwn) ? "parts: the partner's build has no camp ownership"
                                                                        : "off on the partner";
            Log("[enemy] own off: %s (gen %u)", why, g_own_gen);
            OwnHardReset(why, false, on, on);
        }
    }
    if (in.net) OwnDrain(now);
    shots::SetEnemyShots(on && g_ecfg.enemy_shots ? (g_own_on ? shotw::kEnemyCapture | shotw::kEnemyReplay : g_role.load()) : 0);
    if (g_role == 1) {
        if (on) HostTick(in, now);
    } else if (g_role == 2) {
        GuestTick(in, now, !on, gate == erules::FpGate::differ ? "world" : "off");
    }
    if (g_down_announce) DownAnnounce();  // round 23 C (after this tick's pass)
    if (in.net) OwnFlush();
    OwnLine(now, false);
    if ((g_any_frozen.load() || g_att_drop.load() || g_tgt_drop.load() || shots::MirrorBulletDrops()) && now >= g_att_next) {
        g_att_next = now + 10000;
        AttackLine("");
    }
}
void HoldTick(double now) {
    g_cut_step = g_cut.Step(InCutscene(), now);
    g_holding = g_cut_step == 1;
    DrainPending();
}
bool HoldGuarded(double now) noexcept {
    __try {
        HoldTick(now);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool P1Guarded(const TickIn& in, double now) noexcept {
    __try {
        P1Tick(in, now);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool MirrorChecks() {
    char msg[96];
    const bool probe = Check(msg, sizeof(msg));
    unsigned n = g_n, ok = g_pass;
    struct S {
        std::uintptr_t va;
        const char* sig;
        const char* name;
        bool varb;
    };
    const S extra[] = {
        {kPlaceRealized, "40 53 56 57 48 81 EC B0 00 00 00 48 8B 05 3E EE 48 02 48 33 C4 48 89 84", "PlaceRealized", false},
        {kRePath, "40 53 48 83 EC 20 80 B9 88 10 00 00 00 48 8B D9 74 33", "the patrol re-path (variant B)", true},
    };
    bool all = probe;
    for (const S& s : extra) {
        ++n;
        if (mem::CodeMatches(A(s.va), s.sig)) {
            ++ok;
            if (s.varb) g_varb_ok = true;
        } else {
            Log("[enemy] byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
            if (!s.varb) all = false;
        }
    }
    ++n;
    if (mem::Q(A(kMuleVt) + 0x138) == A(kVtCutsceneRel)) ++ok;
    else Log("[enemy] byte check FAILED: DSMuleGameActor vt+0x138"), all = false;
    Log("[enemy] mirror byte checks: %u/%u ok%s", ok, n, all ? "" : " (enemy sync off)");
    return all;
}
// Round 23 B: the ride fields (+0xE20 the vehicle actor id, +0x3C0 the seat) for enemy_mirror_vehicle's second test. A mismatch turns
// only that test off (the parent chain stays).
bool RideChecks() {
    const Site sites[] = {
        {kRideSeatSite, "49 8B 45 58 48 85 C0 48 8D 98 C0 03 00 00 49 0F 44 DF 74 18 83 B8 A0 0E 00 00 01", "the ride action's seat (+0x3C0, the +0xEA0 gate)"},
        {kRideIdSite, "49 8B 46 68 4C 8D B3 68 0A 00 00 48 85 C0 44 89 A3 60 0A 00 00", "the ride action's vehicle id (+0x3C0 + 0xA60 = +0xE20)"},
        {kActorVehNoneSite, "8B 05 FE 8E 05 02 48 8D 8B 28 0E 00 00 89 83 20 0E 00 00", "the MULE constructor's +0xE20 = none"},
    };
    unsigned n = 0, ok = 0;
    for (const Site& s : sites) {
        ++n;
        if (mem::CodeMatches(A(s.va), s.sig)) ++ok;
        else Log("[enemy] byte check FAILED: %s at 0x%llX (vehicle test)", s.name, static_cast<unsigned long long>(s.va));
    }
    ++n;
    std::uint32_t none = 0;
    if (mem::Rd(A(kVehIdNoneGlobal), none) && none == erules::kVehicleIdNone) ++ok;
    else Log("[enemy] byte check FAILED: the none id at 0x%llX is 0x%X, not 0x1FFFFF (vehicle test)", static_cast<unsigned long long>(kVehIdNoneGlobal), none);
    Log("[enemy] vehicle test byte checks: %u/%u ok%s", ok, n, ok == n ? "" : " (the ride-field test off; the parent chain stays)");
    return ok == n;
}

}  // namespace

// ---------------------------------------------------------------- public (P1a)
void Install(const Config& config) noexcept {
    g_ecfg = config;
    g_want = config.enemy_sync && config.enemy_freeze == 1;
    g_guard.store(config.enemy_mirror_guard);
    if (!g_want) {
        Log("[enemy] sync off (enemy_sync=%d%s)", config.enemy_sync ? 1 : 0, config.enemy_freeze == 1 ? "" : ", enemy_freeze not built");
        return;
    }
    if (!config.host_save) {
        // the guest writes the host's places into her world's actors (the drive, the far placement): only into the host-held
        // session's world, which her game never saves (coop::WorldReady is false without it)
        g_want = false;
        Log("[enemy] sync off: it needs host_save=1 (the guest's own save world is never mirrored)");
        return;
    }
    g_p1_ok = MirrorChecks();
    if (!g_p1_ok) {
        Log("[enemy] sync OFF: byte checks");
        return;
    }
    g_sam_filter = samdmg::Ensure("enemy") && samdmg::Register(&MirrorAttackerFilter, "enemy: hits from frozen mirrors");
    g_observer = samdmg::Installed() && samdmg::Observe(&MirrorHitObserver);
    g_npc_filter = npcdmg::Ensure("enemy") && npcdmg::Register(&MirrorTargetFilter, "enemy: hits on frozen mirrors (P1a: the host decides)");
    if (!g_sam_filter) Log("[enemy] the Sam damage filter is not in (%s): the guest freezes nothing (the host still sends)", samdmg::OffReason());
    if (!g_npc_filter) Log("[enemy] the NPC damage filter is not in (%s): hits on mirrors are not dropped", npcdmg::OffReason());
    Log("[enemy] sync installed: radius %.0f m, poses %d, far placement %d, shots %d (hurt %d, sound %d), guard %d", config.enemy_mirror_radius, config.enemy_pose_count,
        config.enemy_place_far ? 1 : 0, config.enemy_shots ? 1 : 0, config.enemy_shots_hurt ? 1 : 0, config.enemy_shots_sound ? 1 : 0, config.enemy_mirror_guard ? 1 : 0);
    // round 23 B: the mirror diagnostics (always), the not-holding rule, the in-vehicle test (docs/enemy-sync.md 14.8)
    g_ride_ok = config.enemy_mirror_vehicle && RideChecks();
    Log("[enemy] mirror checks (round 23 B): hold check %d (error above %.0f m after %u teleports within %.0f s: given back hidden, retried every %.0f s), vehicle "
        "test %d (every %.0f s: the parent chain%s; SEN1 kInVehicle; a seated copy on either side given back to its own game); diagnostics: the worst mirror every "
        "5 s while p95 > %.0f m, one line per id over %.0f m, the error summary every %.0f s",
        config.enemy_mirror_hold_check ? 1 : 0, erules::kNotHoldErrM, erules::kNotHoldTeleports, erules::kNotHoldWindowMs / 1000.0, erules::kNotHoldRetryMs / 1000.0,
        config.enemy_mirror_vehicle ? 1 : 0, erules::kVehicleCheckMs / 1000.0, g_ride_ok ? ", then the actor's +0xE20 / +0x3C0" : " only",
        erules::kMirrorWorstP95M, erules::kMirrorFirstFullM, erules::kMirrorSummaryMs / 1000.0);
    // P1b: ownership by proximity (enemy_owner)
    g_own_want = config.enemy_owner;
    g_camp_carry = config.enemy_owner_outcomes, g_follow_guard = config.enemy_follow_guard, g_relay_want = config.enemy_owner_hits;
    if (g_own_want) {
        g_own_ok = OwnChecks();
        if (!g_own_ok) Log("[enemy] own OFF: byte checks (P1a stays on)");
        // round 23 B (design 7.4): each camp has its own engage E and margin H (by its spread r); the text names the formulas, and each
        // camp's line ("camp ... margin H .. m", "no take ... the host .. m farther, its margin H .. m") prints the camp's own numbers
        else Log("[enemy] own installed (per camp, P1c): take within the camp's engage E = clamp(r + %.0f, %.0f, %.0f) m of her (the host the camp's margin "
                 "H = clamp(%.1f r + %.0f, %.0f, %.0f) m farther, printed per camp, and beyond %.0f m; a camp the host has not loaded: her distance only), back beyond "
                 "E + %.0f m, the host within %.0f m or H nearer (calm), forced beyond %.0f m, silence %.0f s; "
                 "at most %u hers; hold %.0f s; outcomes %d, follower guard %d, host hits relayed %d",
                 erules::kCampEngageAddM, erules::kCampEngageMinM, erules::kCampEngageMaxM, erules::kCampMarginK, erules::kCampMarginAddM, erules::kCampMarginMinM,
                 erules::kCampMarginMaxM, erules::kTakeHostMinM, erules::kCampLeftAddM, erules::kBackHostNearM, config.enemy_mirror_radius + erules::kOwnRangeSlackM,
                 erules::kOwnSilenceMs / 1000.0, erules::kMaxGuestOwnedMembers, erules::kCampHoldMs / 1000.0, g_camp_carry ? 1 : 0, g_follow_guard ? 1 : 0,
                 g_relay_want ? 1 : 0);
    } else {
        Log("[enemy] own off (enemy_owner=0)");
        OwnChecks(false);  // round 27 run 2: the CP reads for the L1 probe's re-calm (CampWalkNow); g_own_ok stays false
    }
}

void Tick(const TickIn& in, double now) noexcept {
    LogFlushLater();  // the guards' lines (worker threads only format them)
    ProbeTick(in.local, in.local_ok, now);
    if (!g_want || !g_p1_ok) return;
    // the cutscene hold and the releases it held (also after a fault-stop)
    if (!HoldGuarded(now)) {
        if (!g_stopped) Stop("the cutscene hold");
        return;
    }
    if (g_stopped) return;
    if (!g_switch) {
        // the mirrors hidden here are shown again while the switch is off (review r20 enemy 1)
        if (!g_dead_hidden.empty()) UnhideDead(true, "off");
        if (!g_host_hid.empty()) UnhideHostHidden(true, "off", now);
        if (!g_nothold.empty()) UnhideNotHolding(true, "off", now);
        if (!g_seat_hid.empty()) SeatShowAll("off", now);  // round 28 T1
        if (!g_down_hid.empty() || !g_down_fhid.empty()) DownResume("off");  // round 23 C
        if (!g_mirrors.empty() || g_mirroring) {
            OwnHardReset("off (enemy_sync 2)", false, true, false);
            OwnFlush();
            ReleaseAll("off");
            HostReset();
            g_mirroring = false;
            shots::SetEnemyShots(0);
        }
        RestoreFar("off");
        shots::SetEnemyBeacon(0, 0, 0, 0, 0);
        return;
    }
    if (!P1Guarded(in, now)) Stop("the tick");
}

bool IsFrozenMirror(std::uintptr_t entity) noexcept { return g_any_frozen.load(std::memory_order_relaxed) && g_mset.Find(entity); }
bool AnyFrozen() noexcept { return g_any_frozen.load(std::memory_order_relaxed); }
bool HoldCopy(std::uint32_t id, double ms, const char* why) noexcept {
    for (const Mirror& m : g_mirrors)
        if (m.id == id && m.frozen) {
            HoldRec& h = g_hold[id];
            const bool fresh = !h.on;
            if (fresh) h.t0 = g_tnow, h.have_native = false;
            h.on = true;
            h.until = (std::max)(h.until, g_tnow + ms);
            if (fresh) Log("[enemy] restrain hold id=%05X: the copy's own animation kept (%s; up to %.1f s): no transform writes, no pose apply", id, why ? why : "?", ms / 1000.0);
            return true;
        }
    return false;
}
void ReleaseHold(std::uint32_t id, const char* why) noexcept {
    const auto it = g_hold.find(id);
    if (it == g_hold.end() || !it->second.on) return;
    it->second.on = false, it->second.end = g_tnow;
    Log("[enemy] restrain hold id=%05X released: %s (held %.1f s): the owner's frames blended back in over %.0f ms", id, why ? why : "?", (g_tnow - it->second.t0) / 1000.0,
        erules::kRestrainBlendMs);
}
bool Holding(std::uint32_t id) noexcept {
    const auto it = g_hold.find(id);
    return it != g_hold.end() && it->second.on;
}
std::uint8_t PeerDefeat(std::uint32_t id) noexcept {
    const auto it = g_tracks.find(id);
    const erules::Sample* z = it != g_tracks.end() ? it->second.Newest() : nullptr;
    return z ? ewire::SenDefeat(z->state) : 0;
}
unsigned FrozenCopies(std::uint32_t* ids, std::uintptr_t* actors, std::uintptr_t* entities, unsigned cap) noexcept {
    unsigned n = 0;
    for (const Mirror& m : g_mirrors)
        if (m.frozen && n < cap) ids[n] = m.id, actors[n] = m.actor, entities[n] = m.entity, ++n;
    return n;
}
bool GuardOn() noexcept { return g_guard.load(std::memory_order_relaxed); }
std::uintptr_t MirrorEntity(std::uint32_t actor_id) noexcept {
    for (const Mirror& m : g_mirrors)
        if (m.id == actor_id && m.frozen) return m.entity;
    return 0;
}
bool MirrorIdOf(std::uintptr_t entity, std::uint32_t* id) noexcept { return g_any_frozen.load(std::memory_order_relaxed) && g_mset.Find(entity, id); }
bool NotHoldingShooter(std::uint32_t actor_id, double (&place)[3], std::uintptr_t* entity) noexcept {
    const auto h = g_nothold.find(actor_id);
    if (h == g_nothold.end() || !g_clock.have) return false;
    const auto t = g_tracks.find(actor_id);
    float yaw = 0;
    if (t == g_tracks.end() || !t->second.At(g_clock.Render(g_tnow), place, &yaw)) return false;
    if (entity) {
        ActorNow a;
        *entity = ReadActor(h->second.actor, actor_id, a) && a.realized ? a.entity : 0;
    }
    return true;
}
GateView Gate() noexcept {
    GateView g;
    g.role = g_role.load();
    g.mirroring = g_mirroring.load();
    g.guids = g_fp_done ? g_guids : 0;
    g.world = g_world;
    g.gen = g_gen;
    return g;
}
unsigned NearEnemies(std::uint32_t* ids, std::uintptr_t* actors, unsigned cap) noexcept {
    unsigned n = 0;
    if (g_role != 1 || !g_mirroring) return 0;
    for (const HostEnemy& h : g_host)
        if (n < cap && h.now.realized && h.dist >= 0 && h.dist <= g_ecfg.enemy_mirror_radius) ids[n] = h.id, actors[n] = h.actor, ++n;
    return n;
}
bool MirrorOf(std::uint32_t actor_id, std::uintptr_t* actor, std::uintptr_t* entity) noexcept {
    for (const Mirror& m : g_mirrors)
        if (m.id == actor_id && m.frozen) {
            if (actor) *actor = m.actor;
            if (entity) *entity = m.entity;
            return true;
        }
    return false;
}
std::uint32_t PickedId() noexcept { return g_pick.actor && StillOurs() ? g_pick.id : 0; }
std::uint32_t LookId() noexcept { return g_look_id; }
bool RenderClock(double* render_ms) noexcept {
    if (!g_clock.have) return false;
    *render_ms = g_clock.Render(g_tnow);
    return true;
}

bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    switch (op) {
        case 0: {
            unsigned frozen = 0, posed = 0;
            for (const Mirror& m : g_mirrors) frozen += m.frozen ? 1 : 0, posed += m.slot >= 0 ? 1 : 0;
            shotw::Beacon peer;
            double age = 0;
            const bool hp = shots::PeerBeacon(&peer, &age);
            char es[512] = {};
            shots::EnemyShotStatus(es, sizeof(es));
            unsigned seen = 0, dropped = 0;
            shots::MirrorShotCounts(&seen, &dropped);
            Log("[enemy] sync: role %s, %s, parts ours 0x%02X theirs 0x%02X%s, fp ids %08X guids %08X count %u (%s), mirrors n=%zu frozen=%u posed=%u/%d, SEN1 rx %u (bad %u) "
                "SEP1 rx %u, sent: SEN1 %u SEP1 %u; %s; guards: attacker drops %u, target drops %u, shot drops %u",
                g_role == 1 ? "host" : g_role == 2 ? "guest" : "solo", !g_want ? "off (enemy_sync=0)" : g_stopped ? "STOPPED" : !g_switch ? "off (enemy_sync 2)" : g_mirroring ? "on" : "waiting",
                OurParts(), hp ? peer.parts : 0, hp ? "" : " (no beacon)", g_ids, g_guids, g_count,
                hp && peer.ids_fp == g_ids && peer.guids_fp == g_guids && peer.enemy_count == g_count && g_count ? "equal" : "differ", g_mirrors.size(), frozen, posed,
                g_ecfg.enemy_pose_count, g_gs.rx_sen, g_gs.rx_bad, g_gs.rx_sep, g_ss.sen, g_ss.sep, es, g_att_drop.load(), g_tgt_drop.load(), dropped);
            Log("[enemy] host guard (M0): %s, entries refused %u (host unrealized / hidden / far), mirrors released %u (host unrealized / hidden / far), "
                "hidden here for the host now %u; entry err max %.0f m, host far %.0f m",
                g_ecfg.enemy_host_guard ? "on" : "off (enemy_host_guard=0)", g_gs.host_refused, g_gs.host_released, g_gs.host_hidden_now, g_ecfg.enemy_entry_err_max,
                g_ecfg.enemy_host_far);
            Log("[enemy] down hold: %s radius %.0f m, ours %s (%s), partner %s (%s), held %zu, hidden %u, ownership %s; downs %u, resumed %u, holds %u, hides %u, "
                "entries refused %u, ownership changes not made %u",
                g_ecfg.enemy_down_hold ? "on" : "off (enemy_down_hold=0)", g_ecfg.enemy_down_hold_radius, erules::PlayerDownText(g_our_down),
                g_our_fake >= 0 ? "fake" : "game state", erules::PlayerDownText(g_peer_down), g_peer_fake >= 0 ? "fake" : "beacon", g_down_held.size(),
                DownHiddenNow(), DownFreeze() ? "frozen" : "free", g_ds.downs, g_ds.resumed, g_ds.holds, g_ds.hides, g_ds.refused, g_ds.no_change);
            AttackLine(" (status)");
            ecombat::Status();
            {
                unsigned pending = 0;
                for (const auto& kv : g_hown) pending += kv.second.e.pending ? 1u : 0u;
                Log("[enemy] own: %s (%s), owned here n=%u, guest-owned n=%u, pending n=%u, changes take %u back %u forced %u refused %u, no-owner>1s %u, "
                    "two-thinking %u, resets %u, gen %u",
                    g_own_on ? "on" : "off",
                    !g_own_want ? "enemy_owner=0" : !g_own_ok ? "byte checks" : !g_own_switch ? "enemy_sync 8 0" : g_own_on ? "both beacons" : "waiting for the gate",
                    g_role == 2 ? OwnedHereCount() : static_cast<unsigned>(g_host.size()) - (std::min)(static_cast<unsigned>(g_host.size()), GuestOwnedCount()),
                    g_role == 2 ? OwnedHereCount() : GuestOwnedCount(), pending, g_os.takes, g_os.backs, g_os.forced, g_os.refused, g_os.no_owner, g_os.two_thinking,
                    g_os.resets, g_own_gen);
            }
            return std::snprintf(msg, cap, "mirrors n=%zu frozen=%u (%s)", g_mirrors.size(), frozen, g_mirroring ? "on" : "off"), true;
        }
        case 5: {
            // the ownership table
            unsigned n = 0;
            for (const auto& kv : g_hown) {
                const HostRec& r = kv.second;
                if (r.e.owner != erules::Owner::guest && !r.e.pending && r.e.epoch == 0) continue;
                ++n;
                Log("[enemy] own id=%05X owner %s epoch %u pending %.0f ms phase %u guest phase %u damaged %d d_g %.1f d_h %.1f hold %.1f s follower %s", kv.first,
                    r.e.owner == erules::Owner::guest ? "guest" : "host", r.e.epoch, r.e.pending ? g_tnow - r.e.t_sent : 0.0, r.phase, r.g_phase,
                    (r.damaged || r.g_damaged) ? 1 : 0, r.d_g, r.d_h, (std::max)(0.0, (r.e.hold_until - g_tnow) / 1000.0),
                    !r.e.frozen ? "none" : r.far_follow ? "far" : "mirror");
            }
            for (const auto& kv : g_gown) {
                ++n;
                Log("[enemy] own id=%05X owner %s epoch %u (here) damaged %d phase %u", kv.first, kv.second.g.owned ? "guest" : "host", kv.second.g.epoch,
                    kv.second.damaged ? 1 : 0, PhaseOf(kv.second.actor));
            }
            OwnLine(g_tnow, true);
            return std::snprintf(msg, cap, "%u ownership rows", n), true;
        }
        case 9: {
            // round 23 C (hidden runs): a player down faked, fed where the beacon's code goes. 9 <1 host | 2 guest> <0 clear | 1 down | 2 limbo>:
            // our own player's fake also goes out in our beacon (the partner holds as for a real one); the partner's acts here only
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            const int who = static_cast<int>(v[1]), what = static_cast<int>(v[2]);
            const int role = g_role.load();
            if ((who != 1 && who != 2) || what < 0 || what > 2) return std::snprintf(msg, cap, "enemy_sync 9 <1 host|2 guest> <0 clear|1 down|2 limbo>"), false;
            if (role != 1 && role != 2) return std::snprintf(msg, cap, "no co-op role"), false;
            const int code = what == 0 ? -1 : what == 1 ? static_cast<int>(erules::PlayerDown::test_down) : static_cast<int>(erules::PlayerDown::test_limbo);
            const bool ours = who == role;
            (ours ? g_our_fake : g_peer_fake) = code;
            Log("[enemy] enemy_sync 9: %s %s (%s; applied at the next tick)", who == 1 ? "host Sam" : "guest Fragile",
                code < 0 ? "fake cleared" : erules::PlayerDownText(static_cast<std::uint8_t>(code)), ours ? "ours: also sent in our beacon" : "the partner's: here only");
            return std::snprintf(msg, cap, "%s %s", who == 1 ? "host" : "guest", code < 0 ? "clear" : what == 1 ? "down" : "limbo"), true;
        }
        case 8:
            g_own_switch = v[1] != 0;
            Log("[enemy] ownership %s (enemy_sync 8)", g_own_switch ? "on" : "off: everything back to the host");
            return std::snprintf(msg, cap, "ownership %s", g_own_switch ? "on" : "off"), true;
        case 1:
            if (!g_want || !g_p1_ok) return std::snprintf(msg, cap, "enemy sync is not installed (enemy_sync=0 or byte checks)"), false;
            if (g_stopped) return std::snprintf(msg, cap, "stopped after a fault"), false;
            g_switch = true;
            Log("[enemy] sync on (debug action)");
            return std::snprintf(msg, cap, "on"), true;
        case 2: {
            const std::size_t n = g_mirrors.size();
            g_switch = false;
            OwnHardReset("off (enemy_sync 2)", false, true, false);
            OwnFlush();
            ReleaseAll("off");
            RestoreFar("off");
            HostReset();
            g_mirroring = false;
            shots::SetEnemyShots(0);
            Log("[enemy] sync off (debug action): %zu mirrors released", n);
            return std::snprintf(msg, cap, "off, %zu released", n), true;
        }
        case 3: {
            for (const Mirror& m : g_mirrors) {
                const auto it = g_tracks.find(m.id);
                const auto pr = g_prx.find(m.id);
                Log("[enemy] mirror id=%05X frozen %d err %.2f m lease age %.0f ms slot %d pose age %.0f ms a88 0x%X ea4 0x%X entity %p", m.id, m.frozen ? 1 : 0, m.err,
                    it != g_tracks.end() ? g_tnow - it->second.rx_ms : -1.0, m.slot, pr != g_prx.end() ? g_tnow - pr->second.rx : -1.0, U32(m.actor + kActFlags88),
                    U32(m.actor + kMuleSeq), reinterpret_cast<void*>(m.entity));
            }
            Log("[enemy] mirror table: %zu (tracks %zu)", g_mirrors.size(), g_tracks.size());
            return std::snprintf(msg, cap, "%zu mirrors", g_mirrors.size()), true;
        }
        case 4:
            ecombat::OutcomeTable();
            return std::snprintf(msg, cap, "the outcome table"), true;
        case 6:
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            if (v[1] != 0 && !g_varb_ok) return std::snprintf(msg, cap, "variant B's byte check failed"), false;
            g_place_variant = v[1] != 0 ? 1 : 0;
            Log("[enemy] far placement: variant %s", g_place_variant ? "B (+0x1089 and the re-path)" : "A (save, place, restore)");
            return std::snprintf(msg, cap, "variant %s", g_place_variant ? "B" : "A"), true;
        case 7:
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            g_tp_cadence = static_cast<int>(v[1]) == 3 ? 3 : 0;
            Log("[enemy] the driver's teleport cadence: %s", g_tp_cadence ? "every 500 ms" : "above 2 m only");
            return std::snprintf(msg, cap, "cadence %d", g_tp_cadence), true;
        default: return std::snprintf(msg, cap, "unknown op %d (0 status, 1 on, 2 off, 3 table, 4 outcomes, 5 ownership, 6 variant, 7 cadence, 8 0/1 ownership, 9 1|2 0|1|2 a player down faked)", op), false;
    }
}

namespace {
// Run 3: the realized enemy `id` (its entity's vtable in the image before any read) and its transform; false when not here.
bool RealizedXf(std::uint32_t id, WorldTransform& xf, double (&pos)[3]) noexcept {
    std::vector<LocalEnemy> all;
    WalkEnemies(all);
    for (const LocalEnemy& e : all) {
        if (e.id != id) continue;
        ActorNow a;
        if (!ReadActor(e.actor, e.id, a) || !a.realized || a.dead || !a.entity || !mem::InImage(mem::Q(a.entity))) continue;
        if (!ReadXf(a.entity, xf)) continue;
        pos[0] = xf.pos.x, pos[1] = xf.pos.y, pos[2] = xf.pos.z;
        return true;
    }
    return false;
}
}  // namespace

bool PlaceLocalOffLine(std::uint32_t id, double dist, double deg, int mode, char* why, std::size_t cap) noexcept {
    if (why && cap) why[0] = 0;
    if (!automation::Enabled()) return std::snprintf(why, cap, "hidden runs only"), false;
    if (!id || !(dist > 0.5) || dist > 1000.0) return std::snprintf(why, cap, "bad id or distance"), false;
    WorldTransform mx{};
    double mp[3] = {};
    if (!RealizedXf(id, mx, mp)) return std::snprintf(why, cap, "id %05X not realized here", id), false;
    double fx = mx.rot.m[1][0], fy = mx.rot.m[1][1];
    const double fl = std::sqrt(fx * fx + fy * fy);
    if (!(fl > 1e-6)) return std::snprintf(why, cap, "id %05X has no facing", id), false;
    fx /= fl, fy /= fl;
    double dx = fx, dy = fy;
    if (mode == 0) {
        // its line to the partner body (validated: the body's vtable in the image), turned `deg` toward its facing
        WorldTransform bx{};
        const std::uintptr_t body = reinterpret_cast<std::uintptr_t>(g_body);
        if (!body || !mem::InImage(mem::Q(body)) || !ReadXf(body, bx)) return std::snprintf(why, cap, "no partner body"), false;
        double ux = bx.pos.x - mp[0], uy = bx.pos.y - mp[1];
        const double ul = std::sqrt(ux * ux + uy * uy);
        if (ul < 0.5) return std::snprintf(why, cap, "the partner stands on it"), false;
        ux /= ul, uy /= ul;
        const double r = deg * 3.14159265358979323846 / 180.0, c = std::cos(r), s = std::sin(r);
        const double ax = ux * c - uy * s, ay = ux * s + uy * c;  // +deg
        const double bx2 = ux * c + uy * s, by2 = -ux * s + uy * c;  // -deg
        if (ax * fx + ay * fy >= bx2 * fx + by2 * fy) dx = ax, dy = ay;
        else dx = bx2, dy = by2;
    }
    char tp[160] = {};
    const bool ok = game::TeleportLocalPlayerFacing(mp[0] + dx * dist, mp[1] + dy * dist, game::kSnapRayCentreZ, -dx, -dy, game::TeleportMode::snap_to_ground, tp,
                                                    sizeof(tp));
    std::snprintf(why, cap, "%s", tp);
    return ok;
}

bool Look(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
    if (v[0] == -11) {
        // run 3 (round 27, TB): our Sam v[1] m from the last enemy_look's enemy, v[2] deg (default 25) off its line to her, on its
        // facing's side, facing it (both players in its view, apart)
        const std::uint32_t id = g_look_id;
        if (!id) return std::snprintf(msg, cap, "no enemy_look yet"), false;
        const double d = v[1] > 0 ? v[1] : 15.0, deg = v[2] > 0 && v[2] < 90 ? v[2] : 25.0;
        char why[160] = {};
        WorldTransform mx{};
        double mp[3] = {};
        const bool have = RealizedXf(id, mx, mp);
        const bool ok = PlaceLocalOffLine(id, d, deg, 0, why, sizeof(why));
        Log("[enemy] look id=%05X dist %.1f m (%.0f deg off its line to her, on its facing's side; it stands at %.2f, %.2f, %.2f): %s", id, d, deg,
            have ? mp[0] : 0.0, have ? mp[1] : 0.0, have ? mp[2] : 0.0, why);
        return std::snprintf(msg, cap, "id %05X: %s", id, why), ok;
    }
    game::Entity* le = nullptr;
    WorldTransform sx{};
    if (!game::TryGetLocalPlayer(&le) || !le || !game::TryGetTransform(le, &sx)) return std::snprintf(msg, cap, "no local Sam"), false;
    const double sp[3] = {sx.pos.x, sx.pos.y, sx.pos.z};
    const bool behind = v[0] == -3;  // behind the newest shooter, looking along its line of fire at the partner
    const bool side = v[0] == -4;    // beside the newest shooter's line of fire, facing it (its muzzle and rounds across the view)
    const bool newest_hit = v[0] == -5;  // P2: the newest HIT's enemy (down and dead ones allowed)
    const bool picked = v[0] == -6;      // P2-0: the probe's pick
    const bool owned = v[0] == -7;       // P1b: the nearest enemy the guest owns (the guest: hers; the host: its copy)
    const bool front40 = v[0] == -10;    // round 27 M1 (arm D): as -8, but 40 deg off its facing (her and our Sam both in view)
    const bool front = v[0] == -8 || front40;  // round 20 M2: in front of the newest enemy_look's enemy, along its facing, facing it
    const bool outcome = v[0] == -9;     // P1c: the newest outcome set (enemy_camp 3) or carried here
    std::uint32_t owned_id = 0;
    if (owned) {
        double bd = 1e18;
        std::vector<std::uint32_t> ids;
        for (const auto& kv : g_gown)
            if (kv.second.g.owned) ids.push_back(kv.first);
        for (const auto& kv : g_hown)
            if (kv.second.e.owner == erules::Owner::guest) ids.push_back(kv.first);
        std::vector<LocalEnemy> lst;
        WalkEnemies(lst);
        for (const LocalEnemy& e : lst) {
            if (std::find(ids.begin(), ids.end(), e.id) == ids.end()) continue;
            ActorNow a;
            if (!ReadActor(e.actor, e.id, a) || !a.realized || !a.have_xf) continue;
            const double d = Dist3(a.pos, sp);
            if (d < bd) bd = d, owned_id = e.id;
        }
        if (!owned_id) return std::snprintf(msg, cap, "no realized enemy the guest owns"), false;
    }
    const std::uint32_t want = v[0] > 0 ? static_cast<std::uint32_t>(v[0])
                               : v[0] == -2 || behind || side ? shots::LastEnemyShooter()
                               : newest_hit ? ecombat::LastHitId()
                               : picked ? PickedId()
                               : owned  ? owned_id
                               : front  ? g_look_id
                               : outcome ? g_outcome_id
                                        : 0;
    if (picked && !want) return std::snprintf(msg, cap, "no probe pick (enemy_probe 0)"), false;
    if ((v[0] == -2 || behind || side) && !want) return std::snprintf(msg, cap, "no enemy shot replayed yet"), false;
    if (newest_hit && !want) return std::snprintf(msg, cap, "no HIT yet"), false;
    if (front && !want) return std::snprintf(msg, cap, "no enemy_look yet"), false;
    if (outcome && !want) return std::snprintf(msg, cap, "no outcome set or carried here yet"), false;
    WorldTransform fxf{};
    bool have_fxf = false;
    const bool faced = v[0] == -1;
    std::uint32_t best_id = 0;
    double best = 1e18, bpos[3] = {};
    std::uintptr_t bent = 0;
    const char* how = "";
    // the candidates: the guest's frozen mirrors, the host's realized enemies
    std::vector<LocalEnemy> all;
    WalkEnemies(all);
    double face_from[3] = {}, face_dir[2] = {};
    if (faced) {
        // the enemy the partner body faces (the guest looked at it first): the smallest angle off its forward, within 25 deg and 60 m
        game::Entity* body = g_body;
        WorldTransform bx{};
        if (!body || !ReadXf(reinterpret_cast<std::uintptr_t>(body), bx)) return std::snprintf(msg, cap, "no partner body"), false;
        face_from[0] = bx.pos.x, face_from[1] = bx.pos.y, face_from[2] = bx.pos.z;
        face_dir[0] = bx.rot.m[1][0], face_dir[1] = bx.rot.m[1][1];
        const double l = std::sqrt(face_dir[0] * face_dir[0] + face_dir[1] * face_dir[1]);
        if (l < 1e-6) return std::snprintf(msg, cap, "the partner body has no heading"), false;
        face_dir[0] /= l, face_dir[1] /= l;
    }
    for (const LocalEnemy& e : all) {
        ActorNow a;
        if (!ReadActor(e.actor, e.id, a) || !a.realized || (a.dead && !newest_hit) || !a.have_xf) continue;
        if (want && e.id != want) continue;
        if (!want && g_role == 2 && !g_mset.Find(a.entity)) continue;  // the guest: frozen mirrors only
        double score = Dist3(a.pos, sp);
        if (faced) {
            const double dx = a.pos[0] - face_from[0], dy = a.pos[1] - face_from[1];
            const double d = std::sqrt(dx * dx + dy * dy);
            if (d < 1 || d > 60) continue;
            const double c = (dx * face_dir[0] + dy * face_dir[1]) / d;
            const double ang = std::acos(c > 1 ? 1 : c < -1 ? -1 : c) * 180.0 / 3.14159265358979323846;
            if (ang > 25) continue;
            score = ang * 4 + d * 0.05;
        }
        if (score < best) best = score, best_id = e.id, bent = a.entity, std::memcpy(bpos, a.pos, sizeof(bpos)), fxf = a.xf, have_fxf = true;
    }
    how = front40 ? "in front of it, 40 deg off its facing" : front ? (v[1] < 0 ? "behind it, along its facing" : "in front of it, along its facing") : owned ? (g_role == 1 ? "the nearest the guest owns (its copy here)" : "the nearest she owns") : newest_hit ? (g_role == 1 ? "the newest applied HIT's enemy" : "the newest sent HIT's enemy") : side ? "beside the newest replayed shooter's line to the partner" : behind ? "behind the newest replayed shooter, toward the partner" : v[0] == -2 ? (g_role == 1 ? "the newest sent shooter" : "the newest replayed shooter") : want ? "by id" : faced ? "the one the partner faces" : g_role == 2 ? "the nearest frozen mirror" : "the nearest realized";
    if (!best_id && faced) {
        // none within 25 deg of her heading: the one nearest the partner body
        for (const LocalEnemy& e : all) {
            ActorNow a;
            if (!ReadActor(e.actor, e.id, a) || !a.realized || a.dead || !a.have_xf) continue;
            if (g_role == 2 && !g_mset.Find(a.entity)) continue;
            const double d = Dist3(a.pos, face_from);
            if (d < best) best = d, best_id = e.id, bent = a.entity, std::memcpy(bpos, a.pos, sizeof(bpos));
        }
        how = "the one nearest the partner (none in her view)";
    }
    if (!best_id) return std::snprintf(msg, cap, "no enemy (%s)", how), false;
    double dx = sp[0] - bpos[0], dy = sp[1] - bpos[1];
    const double l = std::sqrt(dx * dx + dy * dy);
    if (l < 0.5) dx = 1, dy = 0;
    else dx /= l, dy /= l;
    if (front && have_fxf) {
        const double fl = std::sqrt(static_cast<double>(fxf.rot.m[1][0]) * fxf.rot.m[1][0] + static_cast<double>(fxf.rot.m[1][1]) * fxf.rot.m[1][1]);
        if (fl > 1e-6) dx = fxf.rot.m[1][0] / fl, dy = fxf.rot.m[1][1] / fl;
        if (front40) {
            const double c = std::cos(40.0 * 3.14159265358979323846 / 180.0), s = std::sin(40.0 * 3.14159265358979323846 / 180.0);
            const double rx = dx * c - dy * s, ry = dx * s + dy * c;
            dx = rx, dy = ry;
        }
    }
    // round 23 E: in front mode a negative distance places our Sam that far behind it (along its facing), facing its back
    const double d = v[1] > 0 ? v[1] : front && v[1] < 0 ? v[1] : 12.0;
    const double face = d < 0 ? 1.0 : -1.0;
    char why[160] = {};
    WorldTransform bx{};
    if (side && g_body && ReadXf(reinterpret_cast<std::uintptr_t>(g_body), bx)) {
        // d m off the shooter's line to the partner, at a quarter of the way, facing the shooter; the next steered aim points at it
        double ux = bx.pos.x - bpos[0], uy = bx.pos.y - bpos[1];
        const double ul = std::sqrt(ux * ux + uy * uy);
        if (ul > 0.5) {
            ux /= ul, uy /= ul;
            const double along = ul * 0.25 < 6.0 ? ul * 0.25 : 6.0;
            const double x = bpos[0] + ux * along + uy * d, y = bpos[1] + uy * along - ux * d;
            double fx = bpos[0] - x, fy = bpos[1] - y;
            const double fl = std::sqrt(fx * fx + fy * fy);
            fx /= fl, fy /= fl;
            const bool ok = game::TeleportLocalPlayerFacing(x, y, game::kSnapRayCentreZ, fx, fy, game::TeleportMode::snap_to_ground, why, sizeof(why));
            const double aim[3] = {bpos[0] + ux * along * 0.5, bpos[1] + uy * along * 0.5, bpos[2] + 1.3};
            shots::SetAimTarget(aim, 20000);
            Log("[enemy] look id=%05X dist %.1f m (%s; it stands at %.2f, %.2f, %.2f, the partner %.1f m beyond): %s", best_id, d, how, bpos[0], bpos[1], bpos[2], ul, why);
            g_look_id = best_id;
            return std::snprintf(msg, cap, "id %05X: %s", best_id, why), ok;
        }
    }
    if (behind && g_body && ReadXf(reinterpret_cast<std::uintptr_t>(g_body), bx)) {
        // d m behind the shooter on the line from the partner through it, 2 m to the side, facing the partner: the shooter's gun
        // and its rounds' path in view
        double ux = bx.pos.x - bpos[0], uy = bx.pos.y - bpos[1];
        const double ul = std::sqrt(ux * ux + uy * uy);
        if (ul > 0.5) {
            ux /= ul, uy /= ul;
            const double x = bpos[0] - ux * d + uy * 2.0, y = bpos[1] - uy * d - ux * 2.0;
            const bool ok = game::TeleportLocalPlayerFacing(x, y, game::kSnapRayCentreZ, ux, uy, game::TeleportMode::snap_to_ground, why, sizeof(why));
            Log("[enemy] look id=%05X dist %.1f m (%s; it stands at %.2f, %.2f, %.2f, the partner %.1f m beyond): %s", best_id, d, how, bpos[0], bpos[1], bpos[2], ul, why);
            g_look_id = best_id;
            return std::snprintf(msg, cap, "id %05X: %s", best_id, why), ok;
        }
    }
    const bool ok = game::TeleportLocalPlayerFacing(bpos[0] + dx * d, bpos[1] + dy * d, game::kSnapRayCentreZ, face * dx, face * dy, game::TeleportMode::snap_to_ground,
                                                    why, sizeof(why));
    const double up = v[2] > 0.2 && v[2] < 2.0 ? v[2] : 1.2;  // the aim point's height over its root (1.2 m: a standing chest)
    const double chest[3] = {bpos[0], bpos[1], bpos[2] + up};
    // a steered aim (shots 4 <deg> 1) in the next 20 s points at it, followed live (run 20261124-114315: the mirror walked 7 s
    // while the aim settled on the place it had left, and her burst went over it)
    if (bent) shots::SetAimEntity(bent, up, 60000);
    else shots::SetAimTarget(chest, 20000);
    Log("[enemy] look id=%05X dist %.1f m (%s; it stands at %.2f, %.2f, %.2f): %s", best_id, d, how, bpos[0], bpos[1], bpos[2], why);
    g_look_id = best_id;
    return std::snprintf(msg, cap, "id %05X: %s", best_id, why), ok;
}

void QueueOwnMessage(const std::uint8_t* bytes, std::size_t size) noexcept {
    if (!bytes || !size || size > 1100 || g_own_in.size() >= 64) return;
    g_own_in.emplace_back(bytes, bytes + size);
}

bool OwnRelay() noexcept { return g_own_on && g_relay_want && g_role == 1; }
bool OwnedHere(std::uint32_t id) noexcept { return g_role == 2 && IsOwnedHere(id); }

unsigned OwnedHitsOn(std::uint32_t id) noexcept {
    if (!id) return g_owned_hits.load(std::memory_order_relaxed);
    for (const HitsById& s : g_owned_by_id)
        if (s.id.load(std::memory_order_relaxed) == id) return s.n.load(std::memory_order_relaxed);
    return 0;
}

namespace {
// round 25 V1: another member of this actor's camp is hers here, taken as a single take (round 26 review: singles only, as the key
// says; with V1 on by default an ordinary camp take must not hold its in-vehicle members frozen)
bool CampOwnedHere(std::uintptr_t actor) {
    const auto it = g_camp_of.find(actor);
    if (it == g_camp_of.end()) return false;
    for (const auto& g : g_gown) {
        if (!g.second.g.owned || !g.second.alone || g.second.actor == actor) continue;
        const auto o = g_camp_of.find(g.second.actor);
        if (o != g_camp_of.end() && o->second.key == it->second.key) return true;
    }
    return false;
}
}  // namespace
bool TakeFirstOf(std::uint32_t id, TakeFirstInfo* info) noexcept {
    if (!g_own_on || g_tnow - g_take_first_at > kTakeFirstStaleMs) return false;
    const auto it = g_take_first.find(id);
    if (it == g_take_first.end()) return false;
    if (info) info->cp = it->second.cp, info->d_h = it->second.d_h, info->d_g = it->second.d_g;
    return true;
}
bool OwnOn() noexcept { return g_own_on; }
bool CampWalkNow() noexcept {
    if (!g_cp_ok) return false;
    g_cp_next = 0;  // (its own 500 ms limit is for the 2 Hz walk)
    CampWalk(g_tnow);
    return true;
}
// round 25: the camp of <id> here, its CP actor and its other members (ecombat's D3-D5 and levers)
bool CampOfId(std::uint32_t id, CampOf* out) noexcept {
    if (!out) return false;
    *out = CampOf{};
    std::uintptr_t actor = 0;
    if (g_role == 2) {
        const auto it = g_gown.find(id);
        if (it != g_gown.end() && it->second.g.owned) actor = it->second.actor;
        if (!actor)
            if (const Mirror* m = FindMirror(id)) actor = m->actor;
        if (!actor) actor = LocalActor(id);
    } else {
        for (const HostEnemy& h : g_host)
            if (h.id == id) actor = h.actor;
    }
    if (!actor) return false;
    const auto it = g_camp_of.find(actor);
    if (it == g_camp_of.end()) return true;
    out->cp_id = static_cast<std::uint32_t>(it->second.key & 0xFFFFFFFFu);
    out->cp = g_cp_ok && (it->second.key & kCampCp) ? it->second.cp : 0;
    for (const auto& kv : g_camp_of) {
        if (kv.first == actor || kv.second.key != it->second.key || out->n >= kCampOfMax) continue;
        CampMember& c = out->m[out->n++];
        c.actor = kv.first, c.id = U32(kv.first + kActId), c.entity = EntityOfActor(kv.first), c.phase = PhaseOf(kv.first);
        bool frozen = false, owned = false;
        for (const Mirror& m : g_mirrors) frozen = frozen || (m.actor == kv.first && m.frozen);
        if (g_role == 2)
            for (const auto& g : g_gown) owned = owned || (g.second.g.owned && g.second.actor == kv.first);
        c.state = frozen ? 1 : owned ? 2 : 0;
    }
    return true;
}
// round 25 e F1: her copy turned in place to `yaw` (radians) with the take's own teleport (the copy still frozen). 1 done, 0 not
// done, -1 a fault (the caller turns F1 off)
int TurnInPlace(std::uint32_t id, std::uintptr_t actor, float yaw) noexcept {
    ActorNow a;
    if (g_role != 2 || !actor || !ReadActor(actor, id, a) || !a.realized || !a.have_xf) return 0;
    return ActorXf(actor, kActorTeleport, XfFromYaw(a.xf, a.pos, yaw)) ? 1 : -1;
}
bool CampViewOf(std::uint32_t id, CampView* out) noexcept {
    if (!out) return false;
    *out = CampView{};
    std::uintptr_t actor = 0;
    if (g_role == 2) {
        const auto it = g_gown.find(id);
        if (it != g_gown.end() && it->second.g.owned) actor = it->second.actor;
        if (!actor)
            if (const Mirror* m = FindMirror(id)) actor = m->actor;
        if (!actor) actor = LocalActor(id);
    } else {
        for (const HostEnemy& h : g_host)
            if (h.id == id) actor = h.actor;
    }
    if (!actor) return false;
    out->phase = PhaseOf(actor);
    out->single = g_single.count(id) != 0;
    const auto it = g_camp_of.find(actor);
    if (it == g_camp_of.end()) return true;  // loose (or the walk has not listed it)
    out->cp_id = static_cast<std::uint32_t>(it->second.key & 0xFFFFFFFFu);
    out->cp_phase = it->second.phase;
    std::int32_t ph = -1;
    if (g_cp_ok && it->second.cp && mem::Rd(it->second.cp + kCpPhase, ph) && ph >= 0 && ph <= erules::kPhaseMax) out->cp_phase = static_cast<std::uint8_t>(ph), out->cp_live = true;
    for (const auto& kv : g_camp_of) {
        if (kv.first == actor || kv.second.key != it->second.key) continue;
        ++out->members;
        bool frozen = false, owned = false;
        for (const Mirror& m : g_mirrors) frozen = frozen || (m.actor == kv.first && m.frozen);
        if (g_role == 2)
            for (const auto& g : g_gown) owned = owned || (g.second.g.owned && g.second.actor == kv.first);
        ++(frozen ? out->frozen : owned ? out->owned : out->running);
    }
    return true;
}

// enemy_camp <op> <id | 0> <k> (hidden runs): 0 the camp table (`[enemy] camp row ...`); 3 her outcome k (0 faint, 1 sleep, 2 restrained)
// on this game's copy of <id> (0: enemy_look's), the outcome setter (a test of the carry: the guest downs one she owns); 4 (the guest)
// her owned copy of <id> (0: the nearest she owns) hidden (k 1) or shown (k 0) here: a test of the host's follower guard.
bool Camp(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
    const int op = static_cast<int>(v[0]);
    if (op == 0) {
        unsigned rows = 0;
        for (const auto& kv : g_camps) {
            const erules::CampIn& c = kv.second.last;
            if (!kv.second.hers && !(c.d_g >= 0 && c.d_g <= erules::CampEngage(c.r) + 60.0)) continue;
            ++rows;
            Log("[enemy] camp row %s: owner %s, n=%u hers %u, guest %.1f m, host %.1f m, r %.0f m, engage %.0f m, margin %.0f m, loaded %d, phase %u, camp %u, "
                "pending %d, hold %.1f s, last %s",
                CampName(kv.first), kv.second.hers ? "guest" : "host", kv.second.n, kv.second.hers, c.d_g, c.d_h, c.r, erules::CampEngage(c.r), erules::CampMargin(c.r),
                c.host_loaded ? 1 : 0, c.phase, c.cp_phase, c.pending ? 1 : 0, (std::max)(0.0, (kv.second.hold_until - g_tnow) / 1000.0),
                erules::OwnReasonText(kv.second.v.reason));
        }
        Log("[enemy] camp rows n=%u (of %zu known; owned here n=%u; carried %u, carry failed %u, down already %u, followers hidden %u now %zu, stragglers %u, "
            "no-take %zu)",
            rows, g_camps.size(), OwnedHereCount(), g_carried, g_carry_failed, g_carry_down, g_guard_hidden, g_fhid.size(), g_stragglers, g_no_take.size());
        return std::snprintf(msg, cap, "camp rows n=%u", rows), true;
    }
    if (op == 3) {
        const int k = static_cast<int>(v[2]);
        std::uint32_t id = v[1] > 0 ? static_cast<std::uint32_t>(v[1]) : LookId();
        if (!id || k < 0 || k > 2) return std::snprintf(msg, cap, "enemy_camp 3 <id | 0> <k 0..2>"), false;
        std::uintptr_t actor = 0;
        if (g_role == 2) {
            const auto it = g_gown.find(id);
            if (it != g_gown.end() && it->second.g.owned) actor = it->second.actor;
        } else {
            for (const HostEnemy& h : g_host)
                if (h.id == id) actor = h.actor;
        }
        if (!actor) return std::snprintf(msg, cap, "%05X: not %s here", id, g_role == 2 ? "owned" : "an enemy"), false;
        const int res = ecombat::SetOutcome(actor, id, k);
        if (res > 0) g_outcome_id = id;
        ActorNow a;
        const bool same = ReadActor(actor, id, a);
        Log("[enemy] camp test: outcome k %d on %05X here: %s (state %u, ea4 0x%X)", k, id,
            res > 0 ? "set" : res == 0 ? "deferred (a hit queued)" : res == -2 ? "down here already" : "FAILED", same ? U32(actor + 0xEA0) : 0, same ? a.ea4 : 0);
        return std::snprintf(msg, cap, "%05X outcome k %d: %s", id, k, res > 0 ? "set" : "not set"), res > 0;
    }
    if (op == 4) {
        if (g_role != 2) return std::snprintf(msg, cap, "the guest's test"), false;
        const bool hide = v[2] != 0;
        static std::uint32_t last_hidden = 0;  // (show 0: the one hidden last)
        const std::uint32_t want = v[1] > 0 ? static_cast<std::uint32_t>(v[1]) : hide ? 0 : last_hidden;
        std::uint32_t id = 0;
        std::uintptr_t actor = 0;
        double sp[3] = {};
        const bool have_sp = LocalPlace(reinterpret_cast<game::Entity*>(g_our_sam.load()), sp);
        double bd = 1e18;
        for (const auto& kv : g_gown) {
            if (!kv.second.g.owned || (want && kv.first != want)) continue;
            ActorNow a;
            if (!ReadActor(kv.second.actor, kv.first, a) || !a.realized || a.dead) continue;
            // picked by distance: a standing one only (the host's follower guard never hides a down one, spec 8.1 / 8.3, so a
            // down pick would test nothing; gate fix round 23); a given id is taken as it is
            if (!want && erules::OutcomeOf(U32(kv.second.actor + 0xEA0), a.ea4) != erules::Defeat::none) continue;
            const double d = have_sp && a.have_xf ? Dist3(a.pos, sp) : 1e17;
            if (d < bd) bd = d, actor = kv.second.actor, id = kv.first;
        }
        if (!actor) return std::snprintf(msg, cap, "no %sowned copy here%s", want ? "" : "standing ", want ? " for that id" : ""), false;
        const bool ok = ActorBool(actor, kVtHidden, hide ? 1 : 0);
        if (hide) last_hidden = id;
        Log("[enemy] camp test: her owned copy %05X %s here%s", id, hide ? "hidden" : "shown", ok ? "" : " (the call FAULTED)");
        return std::snprintf(msg, cap, "%05X %s", id, hide ? "hidden" : "shown"), ok;
    }
    return std::snprintf(msg, cap, "enemy_camp 0 (the table) | 3 <id> <k> | 4 <id> <1 hide | 0 show>"), false;
}

bool MuleRideInfo(std::uint32_t actor_id, char* camp, std::size_t cap, std::int32_t* seat) noexcept {
    if (camp && cap) std::snprintf(camp, cap, "-");
    if (seat) *seat = -1;
    if (!g_want || !g_p1_ok || !actor_id) return false;
    const std::uintptr_t actor = LocalActor(actor_id);
    if (!actor) return false;
    const auto it = g_camp_of.find(actor);
    if (camp && cap && it != g_camp_of.end()) std::snprintf(camp, cap, "%s", CampName(it->second.key));
    std::int32_t s = -1;
    if (seat && g_ride_ok && mem::Rd(actor + kActSeat, s)) *seat = s;
    return true;
}
bool RideFields(std::uintptr_t actor, std::uint32_t* e20, std::int32_t* seat) noexcept {
    static int checked = -1;  // the probe's own run of the ride checks (-1 not yet)
    if (!g_ride_ok && checked < 0) checked = RideChecks() ? 1 : 0;
    if (!actor || !(g_ride_ok || checked == 1)) return false;
    std::uint32_t v = 0;
    std::int32_t s = -1;
    if (!mem::Rd(actor + kActVehId, v) || !mem::Rd(actor + kActSeat, s)) return false;
    if (e20) *e20 = v;
    if (seat) *seat = s;
    return true;
}

// round 26 (docs/enemy-sync.md 14.19, at the user's request): no longer hidden runs only. What it starts is what ownership does by
// itself (the host's copy frozen, her copy placed, its phase set and released; back: frozen again), for one MULE or its camp.
bool Own(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (g_role != 1) return std::snprintf(msg, cap, "the host's action"), false;
    if (!g_own_on) return std::snprintf(msg, cap, "ownership is not on (enemy_owner=1 on both, the gate)"), false;
    const int to = static_cast<int>(v[1]);
    if (to < 1 || to > 4) return std::snprintf(msg, cap, "owner 1 host | 2 guest (its camp) | 3 guest (this one alone) | 4 host (a single back)"), false;
    double body[3] = {};
    const bool have_body = g_body && LocalPlace(g_body, body);
    std::uint32_t id = v[0] > 0 ? static_cast<std::uint32_t>(v[0]) : 0;
    if (!id) {
        // 0: the nearest the host owns to the partner body; -5 / -7: the nearest the guest owns
        double bd = 1e18;
        for (const HostEnemy& h : g_host) {
            const auto it = g_hown.find(h.id);
            const bool hers = it != g_hown.end() && it->second.e.owner == erules::Owner::guest;
            if ((v[0] < 0) != hers || !h.now.have_xf || !have_body || (!hers && (!h.now.realized || h.now.dead))) continue;
            const double d = Dist3(h.now.pos, body);
            if (d < bd) bd = d, id = h.id;
        }
        if (!id) return std::snprintf(msg, cap, "no candidate"), false;
    }
    // round 24: a single hand-over (3) and its give-back (4; 1 on a single take does the same): at the next walk
    if (to == 3 || to == 4 || (to == 1 && g_single.count(id))) {
        bool known = to != 3;
        for (const HostEnemy& h : g_host) known = known || h.id == id;
        if (!known) return std::snprintf(msg, cap, "%05X is not an enemy here", id), false;
        g_single_req.push_back(SingleReq{id, to == 3});
        g_own_walk_next = 0;
        Log("[enemy] own: %05X alone %s (enemy_own %d; the next walk)", id, to == 3 ? "forced to the guest" : "forced back to the host", to);
        return std::snprintf(msg, cap, "%05X alone -> %s (forced, next walk)", id, to == 3 ? "guest" : "host"), true;
    }
    if (to == 1) {
        const auto it = g_hown.find(id);
        if (it == g_hown.end() || it->second.e.owner != erules::Owner::guest) return std::snprintf(msg, cap, "%05X is not the guest's", id), false;
        it->second.forced = true;
        Log("[enemy] own: %05X forced back to the host (enemy_own; the next walk)", id);
        return std::snprintf(msg, cap, "%05X back (forced)", id), true;
    }
    for (const HostEnemy& h : g_host) {
        if (h.id != id) continue;
        const auto it = g_hown.find(id);
        if (it != g_hown.end() && (it->second.e.owner == erules::Owner::guest || it->second.e.pending))
            return std::snprintf(msg, cap, "%05X: already hers or pending", id), false;
        // P1c: its whole camp, at the next walk
        const std::uint64_t key = CampKeyOf(id, h.actor);
        if (SingleCamp(key)) return std::snprintf(msg, cap, "camp %s: a single member is hers (enemy_own <id> 4 first)", CampName(key)), false;
        g_camps[key].force_take = true, g_camps[key].hold_until = 0;
        g_own_walk_next = 0;
        Log("[enemy] own: the camp %s of %05X forced to the guest (enemy_own; the next walk)", CampName(key), id);
        return std::snprintf(msg, cap, "camp %s of %05X -> guest (forced, next walk)", CampName(key), id), true;
    }
    return std::snprintf(msg, cap, "%05X is not an enemy here", id), false;
}

bool ShotTest(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
    if (g_role != 2) return std::snprintf(msg, cap, "the guest's test"), false;
    std::uint32_t want = v[0] > 0 ? static_cast<std::uint32_t>(v[0]) : 0;
    if (!want && g_look_id && MirrorEntity(g_look_id)) want = g_look_id;
    double sp[3] = {};
    if (!LocalPlace(reinterpret_cast<game::Entity*>(g_our_sam.load()), sp)) return std::snprintf(msg, cap, "no local Sam"), false;
    const Mirror* best = nullptr;
    double bd = 1e18;
    for (const Mirror& m : g_mirrors) {
        if (!m.frozen || (want && m.id != want)) continue;
        WorldTransform x{};
        if (!ReadXf(m.entity, x)) continue;
        const double p[3] = {x.pos.x, x.pos.y, x.pos.z};
        const double d = Dist3(p, sp);
        if (d < bd) bd = d, best = &m;
    }
    if (!best) return std::snprintf(msg, cap, "no live mirror"), false;
    return shots::EnemyShotTest(best->entity, best->id, v[1] != 0, msg, cap);
}

bool PeerDownNow() noexcept { return g_peer_down != 0; }
bool OurDownNow() noexcept { return g_our_down != 0; }
bool CutsceneNow() noexcept { return InCutscene(); }

}  // namespace st::esync
