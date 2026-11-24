// Enemy sync: P1a the mirrors and P1-0 the freeze probe (enemy_sync.hpp; docs/enemy-sync.md 8 and 7.2).
#include "enemy_sync.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <vector>

#include "automation.hpp"
#include "coop_session.hpp"
#include "enemy_rules.hpp"
#include "enemy_wire.hpp"
#include "link.hpp"
#include "npc_damage.hpp"
#include "enemy_census.hpp"
#include "enemy_combat.hpp"
#include "game_actions.hpp"
#include "log.hpp"
#include "partner_source.hpp"
#include "real_body.hpp"
#include "sam_damage.hpp"
#include "save_guard.hpp"
#include "shot_sync.hpp"

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
bool g_p1_ok = false;         // byte checks passed
bool g_varb_ok = false;       // variant B's re-path checked
bool g_sam_filter = false, g_npc_filter = false, g_observer = false;
bool g_switch = true;         // enemy_sync 1 / 2
bool g_stopped = false;       // a fault: off for the session
int g_place_variant = 0;      // enemy_sync 6: 0 A, 1 B
int g_tp_cadence = 0;         // enemy_sync 7: 3 = a teleport every 500 ms between the sets
std::atomic<bool> g_guard{true};
erules::KeySet g_mset;        // the frozen mirrors: entity -> actor id (any thread)
erules::KeySet g_oset;        // P1b, the guest: the enemies she owns (entity -> actor id; the observer)
std::atomic<unsigned> g_owned_hits_logged{0}, g_owned_hits{0};
std::atomic<bool> g_any_frozen{false};
double g_tnow = 0;
game::Entity* g_body = nullptr;  // the partner body (the tick's)
std::uint32_t g_look_id = 0;      // the newest enemy_look's enemy (enemy_shot_test 0 takes it while it is a live mirror)
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

void HostWalk(const double (&guest)[3], bool have_guest) {
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
    e.flags = sp > 0.2 ? ewire::kMoving : 0;
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
        HostWalk(guest, have_guest);
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
void FreeSlot(Mirror& m) {
    if (m.slot >= 0) {
        if (g_palloc.At(m.slot) == erules::PoseAlloc::kApply) {
            realbody::SetEnemySlot(static_cast<unsigned>(m.slot), 0, realbody::EnemySlotMode::off);
            g_palloc.Free(m.slot);
        }
        m.slot = -1;
    }
}
bool ReleaseHeld() { return g_holding || InCutscene(); }
// The posts that give one mirror back: only while it is still the same in-use MULE actor AND still frozen by us (+0xEA4 bit 14:
// cleared means a game sequence took it: forgotten, nothing posted, as the lease's "taken"); Required{0} only when bit 13 was
// clear before our freeze (Occupied{0} keeps bit 13; a bit the game set is never cleared by us).
void PostRelease(const Mirror& m, const char* why) {
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
}
// Give one mirror back (only one we froze). During a cutscene hold: kept frozen in g_pending, released when the hold ends.
void ReleaseMirror(Mirror& m, const char* why) {
    if (!m.frozen) return;
    m.frozen = false;
    FreeSlot(m);
    if (ReleaseHeld()) {
        Mirror p = m;
        p.why = why;
        g_pending.push_back(p);
        PublishMirrors();  // still matched by the guards (it is still frozen)
        Log("[enemy] lease release id=%05X held: %s during a cutscene (posted when the hold ends)", m.id, why);
        return;
    }
    PublishMirrors();  // the guards stop matching it at once
    PostRelease(m, why);
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
            g_tracks[e.actor_id].Push(s, now);
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
    e.in_vehicle = a.realized && InVehicle(a.entity);
    const erules::Refusal r = erules::Eligible(e);
    if (r != erules::Refusal::ok) {
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
        if (ActorXf(actor, kActorTeleport, xf)) m.last_tp = now, ++g_gs.teleports;
    }
    g_mirrors.push_back(m);
    PublishMirrors();
    ++g_gs.on;
    Log("[enemy] mirror on id=%05X (dist %.1f m, err at entry %.2f m, ea4 0x%X)", id, Dist3(z->pos, sam), err, ea4);
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
    if (r.ta > -1e17 && render > r.ta && render < r.tb) {
        wire::BlendJoints(r.a, r.b, static_cast<float>((render - r.ta) / (r.tb - r.ta)), out);
        realbody::PushEnemyFrame(static_cast<unsigned>(m.slot), out);
    } else {
        realbody::PushEnemyFrame(static_cast<unsigned>(m.slot), render <= r.ta && r.ta > -1e17 ? r.a : r.b);
    }
}

// One frozen mirror driven to its owner's place (either game: P1b's host follows the guest's).
void DriveMirror(Mirror& m, const ActorNow& a, const double (&pos)[3], float yaw, double now) {
    const double err = Dist3(pos, a.pos);
    const double yerr = erules::YawDiffDeg(yaw, YawOfXf(a.xf));
    if (now >= m.next_err) m.next_err = now + 500, g_err_win.push_back(err), m.err = err;
    erules::Drive d = erules::Band(err, yerr, now, m.last_tp);
    if (g_tp_cadence == 3 && now - m.last_tp >= 500) d = erules::Drive::teleport;
    const WorldTransform xf = XfFromYaw(a.xf, pos, yaw);
    if (d == erules::Drive::set) {
        if (ActorXf(m.actor, kVtSet, xf) && EntityXf(m.entity, xf)) ++g_gs.sets, ++g_sets_win;
        else ++g_gs.set_faults;
    } else if (d == erules::Drive::teleport) {
        if (ActorXf(m.actor, kActorTeleport, xf)) ++g_gs.teleports, ++g_tp_win, m.last_tp = now;
        else ++g_gs.set_faults;
    }
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
    g_err_win.clear(), g_drift.clear();
    g_sets_win = g_tp_win = 0;
    g_gs.no_pose = 0;
}

void GuestTick(const TickIn& in, double now, bool off, const char* off_why) {
    GuestDrain(now);
    double sam[3] = {};
    const bool have_sam = in.local_ok && LocalPlace(in.local, sam);
    const bool cutscene = InCutscene();
    const int cut = g_cut_step;  // stepped by Tick
    if (cut == 2) ReleaseAll("cutscene");
    if (g_holding) return;  // nothing driven, posed or posted
    if (!g_dead_hidden.empty()) UnhideDead(off, off ? off_why : "the host's is alive again");
    if (off || !have_sam) {
        if (!g_mirrors.empty()) ReleaseAll(off ? off_why : "no local Sam");
        if (off) RestoreFar(off_why);
        return;
    }
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
        // (P1b: a mirror frozen again on an OWN back has no host frame yet: its silence clock starts at the freeze)
        l.frame_age_ms = now - (std::max)(it != g_tracks.end() ? it->second.rx_ms : -1e18, m.t_frozen);
        double pos[3] = {};
        float yaw = 0;
        const bool have_target = it != g_tracks.end() && it->second.At(render, pos, &yaw);
        l.dist_m = have_target ? Dist3(pos, sam) : 0;
        l.cls = erules::ClassOf(l.dist_m, g_ecfg.enemy_mirror_radius);
        l.radius_m = g_ecfg.enemy_mirror_radius;
        const erules::LeaseVerdict v = erules::Lease(l);
        if (v == erules::LeaseVerdict::forget_taken) {
            ++g_gs.taken;
            m.frozen = false;
            changed = true;
            FreeSlot(m);
            Log("[enemy] lease released id=%05X: taken (a game sequence cleared bit 14: forgotten, nothing posted; ea4 0x%X)", m.id, a.ea4);
            continue;
        }
        if (erules::Releases(v)) {
            ReleaseMirror(m, erules::LeaseText(v));
            continue;
        }
        if (!have_target || !a.have_xf) continue;
        DriveMirror(m, a, pos, yaw, now);
        GivePose(m, it->second, render, it->second.Newest() && (it->second.Newest()->state & ewire::kPosed));
    }
    if (changed) PublishMirrors();
    g_mirrors.erase(std::remove_if(g_mirrors.begin(), g_mirrors.end(), [](const Mirror& m) { return !m.frozen; }), g_mirrors.end());
    OwnGuestTick(in, now);  // P1b: the enemies she owns (SEN1, SEP1, shooters, damage)
    // entry (a few a tick)
    unsigned tries = 0;
    for (const auto& kv : g_tracks) {
        if (tries >= 4) break;
        if (FindMirror(kv.first) || IsOwnedHere(kv.first)) continue;
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
bool g_own_on = false;      // live: both beacons carry kPartOwn, mirroring on
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
std::map<std::uintptr_t, std::uint8_t> g_cp_of;  // an actor -> its camp's phase (2 Hz)
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
};
std::map<std::uint32_t, HostRec> g_hown;
std::uint32_t g_nt_id = 0;  // the "no take" line's nearest candidate, its reason
const char* g_nt_why = nullptr;
double g_nt_next = 0, g_kept_next = 0;
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
};
std::map<std::uint32_t, GuestRec> g_gown;

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
void CampWalk(double now) {
    if (!g_cp_ok || now < g_cp_next) return;
    g_cp_next = now + 500;
    g_cp_of.clear();
    const std::uintptr_t mgr = mem::Q(A(kMuleMgr));
    std::int32_t n = 0;
    if (!mgr || !mem::Rd(mgr + kMgrCpCount, n) || n <= 0 || n > 512) return;
    std::vector<std::uint32_t> want(static_cast<std::size_t>(n));
    const std::uintptr_t ids = mem::Q(mgr + kMgrCpIds);
    if (!ids || !mem::Read(ids, want.data(), want.size() * 4)) return;
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
            if (m && mem::Q(m) == A(kMuleVt)) g_cp_of[m] = static_cast<std::uint8_t>(ph), ++members;
        }
        if (!cls) cls = mem::RttiName(mem::RttiOf(cp.actor));
    }
    if (!g_cp_logged) {
        g_cp_logged = true;
        Log("[enemy] camps: n=%zu of %d ids (class %s), members n=%u", cps.size(), n, cls ? cls : "?", members);
    }
}
std::uint8_t CampPhaseOf(std::uintptr_t actor) {
    const auto it = g_cp_of.find(actor);
    return it != g_cp_of.end() ? it->second : erules::kPhaseUnknown;
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
void HostBackDone(std::uint32_t id, HostRec& r, const double* pos, float yaw, std::uint8_t phase, const char* how) {
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
}
void HostTakeOne(const HostEnemy& h, HostRec& r, double now, bool forced) {
    const ActorNow& a = h.now;
    if (!a.have_xf) return;
    bool b13 = false;
    std::uint32_t ea4 = a.ea4;
    const bool ok = !a.realized || FreezeCopy(h.id, h.actor, &b13, &ea4);
    if (!erules::HostTake(r.e, now, ok)) {
        if (!ok) {
            if ((ea4 >> 14) & 1u) UnfreezeCopy(h.id, h.actor, b13);
            Log("[enemy] OWN id=%05X not handed over: the host's copy did not freeze (ea4 0x%X); hold 10 s", h.id, ea4);
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
    const std::uint8_t phase = PhaseOf(h.actor);
    ewire::Own o = OwnOf(h.id, r.e.epoch, erules::Owner::guest, static_cast<std::uint8_t>(forced ? erules::OwnReason::forced : erules::OwnReason::proximity), phase);
    PlaceInto(o, a.pos, YawOfXf(a.xf), h.vel);
    OwnSend(ewire::kScbOwn, o);
    Log("[enemy] OWN id=%05X -> guest (%s: guest %.1f m, host %.1f m; epoch %u; phase %u; host copy %s)", h.id, forced ? "forced" : "proximity", r.d_g, r.d_h,
        r.e.epoch, phase, a.realized ? "frozen" : "far follower");
}
void HostBackStart(std::uint32_t id, HostRec& r, erules::OwnReason why, bool forced, double now) {
    const double silence = now - r.t_rx;
    if (!erules::HostBack(r.e, now)) return;
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
    const erules::AckAct act = erules::HostOnAck(r.e, o.epoch, o.result, now);
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
        g_refuse_times.push_back(now);
        Log("[enemy] OWN id=%05X refused by the guest (%s): the host's again (epoch %u), hold 10 s", o.id, erules::OwnRefuseText(o.refusal), r.e.epoch);
        HostBackDone(o.id, r, nullptr, 0, erules::kPhaseUnknown, "refused: it never left");
        return;
    }
    (void)was_guest;
    if (o.refusal == static_cast<std::uint8_t>(erules::OwnRefuse::fault)) ++g_os.two_thinking;  // her freeze did not take
    HostBackDone(o.id, r, (o.flags & ewire::kOwnHasPlace) ? o.pos : nullptr, o.yaw, o.phase, "her OWNACK");
}
// The host's followers: frozen copies driven and posed from her frames; far followers (unrealized here) placed at 2 Hz.
void HostFollow(double now) {
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
            if (same) UnfreezeCopy(m.id, m.actor, m.bit13_pre);
            if (rit != g_hown.end()) rit->second.far_follow = true;
            Log("[enemy] OWN id=%05X: the host's copy left its world: a far follower now", m.id);
            continue;
        }
        if (!a.held) {
            FreeSlot(m);
            m.frozen = false, changed = true;
            if (rit != g_hown.end()) rit->second.forced = true;
            Log("[enemy] OWN id=%05X: a game sequence took the host's copy (bit 14 cleared, ea4 0x%X): given back to the host", m.id, a.ea4);
            continue;
        }
        const auto it = g_tracks.find(m.id);
        if (it == g_tracks.end()) continue;
        double pos[3];
        float yaw = 0;
        if (!it->second.At(render, pos, &yaw) || !a.have_xf) continue;
        DriveMirror(m, a, pos, yaw, now);
        GivePose(m, it->second, render, it->second.Newest() && (it->second.Newest()->state & ewire::kPosed));
    }
    if (changed) {
        g_mirrors.erase(std::remove_if(g_mirrors.begin(), g_mirrors.end(), [](const Mirror& x) { return !x.frozen; }), g_mirrors.end());
        PublishMirrors();
    }
    if (now < g_hfar_next) return;
    g_hfar_next = now + 500;
    for (auto& kv : g_hown) {
        HostRec& r = kv.second;
        if (!r.far_follow || !r.e.frozen) continue;
        ActorNow a;
        if (!ReadActor(r.actor, kv.first, a) || !a.have_xf) continue;
        if (a.realized) {
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
    for (auto& kv : g_hown) {
        HostRec& r = kv.second;
        if (r.e.owner == erules::Owner::guest) ++n;
        if (r.e.frozen && !world_gone) {
            const auto it = g_tracks.find(kv.first);
            const erules::Sample* z = it != g_tracks.end() ? it->second.Newest() : nullptr;
            r.e.owner = erules::Owner::host;  // (for the line)
            HostBackDone(kv.first, r, z ? z->pos : nullptr, z ? z->yaw : 0.0f, r.g_phase, why);
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
    ++g_own_gen;
    if (announce) OwnSendReset();
    ++g_os.resets;
    Log("[enemy] own reset (%s): every enemy is the host's (gen %u; %u were the guest's)", why, g_own_gen, n);
}
void OwnHostTick(const TickIn& in, double now, const double (&guest)[3], bool have_guest) {
    if (!g_own_on) return;
    double hs[3] = {};
    const bool have_host = in.local_ok && LocalPlace(in.local, hs);
    GuestDrain(now);  // her SEN1 / SEP1 (the enemies she owns)
    HostFollow(now);
    // a back whose OWNACK did not come: released anyway from her newest frame
    for (auto& kv : g_hown) {
        HostRec& r = kv.second;
        if (!erules::HostBackTimedOut(r.e, now)) continue;
        ++g_os.no_owner;
        const auto it = g_tracks.find(kv.first);
        const erules::Sample* z = it != g_tracks.end() ? it->second.Newest() : nullptr;
        HostBackDone(kv.first, r, z ? z->pos : nullptr, z ? z->yaw : 0.0f, r.g_phase, "no OWNACK in 3 s: her newest frame");
    }
    // a save of the host's world never captures a copy we froze: everything back first. Every frozen copy counts: the table's,
    // the followers' and the returns a cutscene holds (g_pending: the host freezes no P1a mirror, so all of them are its own;
    // review rv-safety 4). (Busy is polled once a tick: a capture in the very frame a save is posted is not ruled out.)
    bool any_frozen = !g_pending.empty();
    for (const auto& kv : g_hown) any_frozen = any_frozen || kv.second.e.frozen;
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
    if (now < g_own_walk_next) return;
    g_own_walk_next = now + 500;
    CampWalk(now);
    g_change_times.erase(std::remove_if(g_change_times.begin(), g_change_times.end(), [&](double t) { return now - t >= 1000.0; }), g_change_times.end());
    g_refuse_times.erase(std::remove_if(g_refuse_times.begin(), g_refuse_times.end(), [&](double t) { return now - t >= 60000.0; }), g_refuse_times.end());
    unsigned guest_owned = GuestOwnedCount();
    const bool host_cut = InCutscene() || saveguard::Busy();  // (no take while the host's game saves either)
    const bool guard_ready = g_sam_filter && shots::MirrorGuardReady();
    // the host's: take?
    unsigned takes = 0;
    std::uint32_t near_id = 0;
    double near_d = 1e18;
    const char* near_why = nullptr;
    erules::OwnIn near_in;
    for (const HostEnemy& h : g_host) {
        if (!have_guest || !h.now.have_xf) continue;
        const double d_g = Dist3(h.now.pos, guest);
        auto it = g_hown.find(h.id);
        if (it == g_hown.end() && d_g > erules::kTakeM) continue;
        HostRec& r = g_hown[h.id];
        r.actor = h.actor;
        if (r.e.owner == erules::Owner::guest || r.e.pending) continue;
        r.d_g = d_g, r.d_h = have_host ? Dist3(h.now.pos, hs) : -1;
        if (h.now.realized) NoteVitals(h.now.entity, r.have_base, r.base_h, r.base_s, r.damaged);
        r.phase = PhaseOf(h.actor), r.cp = CampPhaseOf(h.actor);
        erules::OwnIn oi;
        oi.owner = erules::Owner::host, oi.pending = r.e.pending;
        oi.d_g = r.d_g, oi.d_h = r.d_h, oi.phase = r.phase, oi.cp_phase = r.cp;
        oi.damaged = r.damaged;
        oi.terminal = h.now.dead || ecombat::HostDefeat(h.id) != 0;
        oi.guest_ok = !g_guest_cut && g_refuse_times.size() < 3;
        oi.host_ok = !host_cut && guard_ready && !h.now.seq_only && !h.now.held && !(h.now.realized && (h.now.hidden_or_asleep || InVehicle(h.now.entity)));
        oi.hold_over = now >= r.e.hold_until;
        oi.guest_owned = guest_owned, oi.changes_this_sec = static_cast<unsigned>(g_change_times.size());
        const char* block = erules::TakeBlock(oi);
        if (!block && takes < 2) {
            HostTakeOne(h, r, now, false);
            ++takes, ++guest_owned;
        } else if (block && d_g <= erules::kTakeM && d_g < near_d) {
            near_d = d_g, near_id = h.id, near_why = block, near_in = oi;
        }
    }
    // the nearest candidate's reason (changed, or every 10 s while one is within 40 m)
    if (near_id && (near_id != g_nt_id || near_why != g_nt_why || now >= g_nt_next)) {
        g_nt_id = near_id, g_nt_why = near_why, g_nt_next = now + 10000;
        Log("[enemy] own: no take id=%05X (guest %.1f m, host %.1f m, phase %u, camp %u, damaged %d, hold %.1f s): %s", near_id, near_in.d_g, near_in.d_h, near_in.phase,
            near_in.cp_phase, near_in.damaged ? 1 : 0, (std::max)(0.0, (g_hown[near_id].e.hold_until - now) / 1000.0), near_why);
    }
    // the guest's: back?
    for (auto& kv : g_hown) {
        HostRec& r = kv.second;
        if (r.e.owner != erules::Owner::guest) continue;
        const auto it = g_tracks.find(kv.first);
        const erules::Sample* z = it != g_tracks.end() ? it->second.Newest() : nullptr;
        // her place of it (her newest frame), else the host's copy
        double p[3] = {};
        bool have_p = false;
        if (z) std::memcpy(p, z->pos, sizeof(p)), have_p = true;
        else {
            ActorNow a;
            if (ReadActor(r.actor, kv.first, a) && a.have_xf) std::memcpy(p, a.pos, sizeof(p)), have_p = true;
        }
        // no partner body here: she is in another region (beyond any range)
        r.d_g = !have_guest ? 1e9 : have_p ? Dist3(p, guest) : -1;
        r.d_h = have_host && have_p ? Dist3(p, hs) : -1;
        erules::OwnIn oi;
        oi.owner = erules::Owner::guest, oi.pending = r.e.pending;
        oi.d_g = r.d_g, oi.d_h = r.d_h, oi.phase = r.g_phase;
        oi.damaged = r.g_damaged;
        oi.terminal = z && ((z->state & ewire::kDead) || ewire::DefeatOfState(z->state) != 0);  // dead or down on her side: stays hers
        oi.silence_ms = now - r.t_rx;
        oi.unrealized_guest = z && now - r.t_rx < 1000.0 && (!(z->state & ewire::kRealized) || (z->state & ewire::kSeqHeld));
        oi.range_m = g_ecfg.enemy_mirror_radius + erules::kOwnRangeSlackM;
        oi.ack_wait_ms = now - r.e.t_sent;
        oi.forced = r.forced;
        oi.hold_over = now >= r.e.hold_until;
        const erules::OwnVerdict v = erules::HostDecide(oi);
        if (v.act == erules::OwnAct::back || v.act == erules::OwnAct::forced_back) {
            r.forced = false;
            HostBackStart(kv.first, r, v.reason, v.act == erules::OwnAct::forced_back, now);
        } else if (!r.e.pending && now >= g_kept_next) {
            // why hers stays hers (every 10 s): her place, the host's, her phase, the hold
            g_kept_next = now + 10000;
            Log("[enemy] own: kept by the guest id=%05X (guest %.1f m, host %.1f m, her phase %u (%s), damaged %d, hold %.1f s, her newest frame %.1f s ago)", kv.first,
                r.d_g, r.d_h, r.g_phase, erules::PhaseText(r.g_phase), r.g_damaged ? 1 : 0, (std::max)(0.0, (r.e.hold_until - now) / 1000.0), (now - r.t_rx) / 1000.0);
        }
    }
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
    if (!m || !m->frozen) code = erules::OwnRefuse::not_mirror;
    else if (!ReadActor(m->actor, o.id, a) || !a.realized || a.entity != m->entity) code = erules::OwnRefuse::not_realized;
    else if (a.dead) code = erules::OwnRefuse::dead;
    else if (!a.held) code = erules::OwnRefuse::held;  // bit 14 not ours any more: a game sequence has it
    else if (g_holding || InCutscene()) code = erules::OwnRefuse::cutscene;
    else if (OwnedHereCount() >= erules::kMaxGuestOwned) code = erules::OwnRefuse::cap;
    // (her camp alerted: taken all the same; its push makes it join her camp's fight here, logged below. Refusing would keep
    // every MULE of a camp that once saw her the host's until that camp calms: run 20261124-151942)
    if (code != erules::OwnRefuse::none) {
        r.g.owned = false;
        ++g_os.refused_here;
        GuestAck(o.id, o.epoch, erules::Owner::host, ewire::kOwnRefused, static_cast<std::uint8_t>(code), m ? m->actor : 0);
        Log("[enemy] OWN id=%05X refused here (epoch %u): %s", o.id, o.epoch, erules::OwnRefuseText(static_cast<std::uint8_t>(code)));
        return;
    }
    const std::uintptr_t actor = m->actor;
    // the host's place
    double err = 0;
    if (o.flags & ewire::kOwnHasPlace) {
        const double p[3] = {o.pos[0], o.pos[1], o.pos[2]};
        err = Dist3(p, a.pos);
        if (err > erules::kBandStillM) ActorXf(actor, kActorTeleport, XfFromYaw(a.xf, p, o.yaw));
    }
    // the host's phase (calm there: SetPhase, exact)
    const std::uint8_t ph_before = PhaseOf(actor);
    if (o.phase <= erules::kPhaseMax && ph_before != erules::kPhaseUnknown && ph_before != o.phase) CallPhase(kSetPhase, actor, o.phase);
    // released here: her AI runs it from now (her hits on it are real)
    const std::uint32_t ea4_before = U32(actor + kMuleSeq);
    Mirror mm;
    TakeMirrorOut(o.id, &mm);
    UnfreezeCopy(o.id, actor, mm.bit13_pre);
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
    Log("[enemy] OWN id=%05X taken here (epoch %u; err %.2f m; phase %u->%u; her camp %u; ea4 0x%X->0x%X; weapon268 %d)", o.id, o.epoch, err, ph_before,
        PhaseOf(actor), CampPhaseOf(actor), ea4_before, ea4_after, a.entity ? static_cast<int>(WeaponFlag(a.entity)) : -1);
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
        }
    }
    r.g.owned = false;
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
// else (the link, the gate, a world) she is solo again: they keep running in her world.
void GuestOwnReset(const char* why, bool refreeze, double now) {
    unsigned n = 0;
    for (auto& kv : g_gown) {
        if (!kv.second.g.owned) continue;
        ++n;
        if (refreeze) GuestRefreeze(kv.first, kv.second, now, nullptr, nullptr);
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
        e.flags = static_cast<std::uint8_t>(ewire::kOwnerGuest | (sp > 0.2 ? ewire::kMoving : 0) | (r.damaged ? ewire::kDamaged : 0));
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
}

// A reset of the whole table (a world or the link changed, the gate, the switch, a fault): the host's copies released (placed at
// her newest frames unless that world is gone), the guest's left running (solo) or frozen again (`refreeze`: mirroring stays);
// `announce`: OWNRESET to the guest (the link stays).
void OwnHardReset(const char* why, bool world_gone, bool announce, bool refreeze) {
    if (!g_hown.empty() || (g_role == 1 && g_own_on)) HostOwnReset(why, announce, world_gone);
    if (!g_gown.empty() || (g_role == 2 && g_own_on)) GuestOwnReset(why, refreeze && !world_gone, g_tnow);
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

bool OwnChecks() {
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
    Log("[enemy] own byte checks: %u/%u ok%s", ok, n, !main_ok ? " (ownership off)" : !cp_ok ? " (the camp phase unknown)" : "");
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
    if (g_own_want && g_own_ok && g_own_switch && (g_role != 1 || saveguard::ReadsOk())) p |= shotw::kPartOwn;
    return p;
}

void Stop(const char* step) {
    g_stopped = true;
    Log("[enemy] FAULT in %s: enemy sync stopped", step);
    UnhideDead(true, "a fault");
    OwnHardReset("a fault", false, false, false);
    ReleaseAll("fault");
    RestoreFar("fault");
    HostReset();
    g_mirroring = false;
    shots::SetEnemyShots(0);
    shots::SetEnemyBeacon(0, 0, 0, 0, 0);
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
        UnhideDead(true, world_changed ? "the world changed" : "the link changed");
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
            ReleaseAll("off");
            HostReset();
        }
    } else if (!on && gate != g_last_gate && g_ready && in.net) {
        Log("[enemy] sync waiting: %s", erules::FpGateText(gate));
    }
    g_last_gate = gate;
    // P1b: ownership while both beacons carry kPartOwn (and mirroring is on)
    const bool own_on = on && (parts & shotw::kPartOwn) && have_peer && (peer.parts & shotw::kPartOwn);
    if (own_on != g_own_on) {
        if (own_on) {
            g_own_on = true;
            g_own_walk_next = g_hfar_next = g_gsen_next = g_gwalk_next = g_cp_next = 0;
            Log("[enemy] own on (parts ours 0x%02X theirs 0x%02X; gen %u)", parts, peer.parts, g_own_gen);
        } else {
            const char* why = !on ? "mirroring off" : !(parts & shotw::kPartOwn) ? "off here" : "off on the partner";
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
    // P1b: ownership by proximity (enemy_owner)
    g_own_want = config.enemy_owner;
    if (g_own_want) {
        g_own_ok = OwnChecks();
        if (!g_own_ok) Log("[enemy] own OFF: byte checks (P1a stays on)");
        else Log("[enemy] own installed: take within %.0f m of her (the host %.0f m farther and beyond %.0f m), back beyond %.0f m or the host within %.0f m (calm), "
                 "forced beyond %.0f m, silence %.0f s; at most %u hers", erules::kTakeM, erules::kTakeMarginM, erules::kTakeHostMinM, erules::kBackFarM,
                 erules::kBackHostNearM, config.enemy_mirror_radius + erules::kOwnRangeSlackM, erules::kOwnSilenceMs / 1000.0, erules::kMaxGuestOwned);
    } else {
        Log("[enemy] own off (enemy_owner=0)");
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
bool GuardOn() noexcept { return g_guard.load(std::memory_order_relaxed); }
std::uintptr_t MirrorEntity(std::uint32_t actor_id) noexcept {
    for (const Mirror& m : g_mirrors)
        if (m.id == actor_id && m.frozen) return m.entity;
    return 0;
}
bool MirrorIdOf(std::uintptr_t entity, std::uint32_t* id) noexcept { return g_any_frozen.load(std::memory_order_relaxed) && g_mset.Find(entity, id); }
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
        default: return std::snprintf(msg, cap, "unknown op %d (0 status, 1 on, 2 off, 3 table, 4 outcomes, 5 ownership, 6 variant, 7 cadence, 8 0/1 ownership)", op), false;
    }
}

bool Look(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
    game::Entity* le = nullptr;
    WorldTransform sx{};
    if (!game::TryGetLocalPlayer(&le) || !le || !game::TryGetTransform(le, &sx)) return std::snprintf(msg, cap, "no local Sam"), false;
    const double sp[3] = {sx.pos.x, sx.pos.y, sx.pos.z};
    const bool behind = v[0] == -3;  // behind the newest shooter, looking along its line of fire at the partner
    const bool side = v[0] == -4;    // beside the newest shooter's line of fire, facing it (its muzzle and rounds across the view)
    const bool newest_hit = v[0] == -5;  // P2: the newest HIT's enemy (down and dead ones allowed)
    const bool picked = v[0] == -6;      // P2-0: the probe's pick
    const bool owned = v[0] == -7;       // P1b: the nearest enemy the guest owns (the guest: hers; the host: its copy)
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
                                        : 0;
    if (picked && !want) return std::snprintf(msg, cap, "no probe pick (enemy_probe 0)"), false;
    if ((v[0] == -2 || behind || side) && !want) return std::snprintf(msg, cap, "no enemy shot replayed yet"), false;
    if (newest_hit && !want) return std::snprintf(msg, cap, "no HIT yet"), false;
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
        if (score < best) best = score, best_id = e.id, bent = a.entity, std::memcpy(bpos, a.pos, sizeof(bpos));
    }
    how = owned ? (g_role == 1 ? "the nearest the guest owns (its copy here)" : "the nearest she owns") : newest_hit ? (g_role == 1 ? "the newest applied HIT's enemy" : "the newest sent HIT's enemy") : side ? "beside the newest replayed shooter's line to the partner" : behind ? "behind the newest replayed shooter, toward the partner" : v[0] == -2 ? (g_role == 1 ? "the newest sent shooter" : "the newest replayed shooter") : want ? "by id" : faced ? "the one the partner faces" : g_role == 2 ? "the nearest frozen mirror" : "the nearest realized";
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
    const double d = v[1] > 0 ? v[1] : 12.0;
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
    const bool ok = game::TeleportLocalPlayerFacing(bpos[0] + dx * d, bpos[1] + dy * d, game::kSnapRayCentreZ, -dx, -dy, game::TeleportMode::snap_to_ground, why, sizeof(why));
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

bool Own(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
    if (g_role != 1) return std::snprintf(msg, cap, "the host's action"), false;
    if (!g_own_on) return std::snprintf(msg, cap, "ownership is not on (enemy_owner=1 on both, the gate)"), false;
    const int to = static_cast<int>(v[1]);
    if (to != 1 && to != 2) return std::snprintf(msg, cap, "owner 1 host or 2 guest"), false;
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
    if (to == 1) {
        const auto it = g_hown.find(id);
        if (it == g_hown.end() || it->second.e.owner != erules::Owner::guest) return std::snprintf(msg, cap, "%05X is not the guest's", id), false;
        it->second.forced = true;
        Log("[enemy] own: %05X forced back to the host (enemy_own; the next walk)", id);
        return std::snprintf(msg, cap, "%05X back (forced)", id), true;
    }
    for (const HostEnemy& h : g_host) {
        if (h.id != id) continue;
        HostRec& r = g_hown[id];
        r.actor = h.actor;
        if (r.e.owner == erules::Owner::guest || r.e.pending) return std::snprintf(msg, cap, "%05X: already hers or pending", id), false;
        r.d_g = have_body && h.now.have_xf ? Dist3(h.now.pos, body) : -1;
        r.e.hold_until = 0;
        HostTakeOne(h, r, g_tnow, true);
        return std::snprintf(msg, cap, "%05X -> guest (forced)", id), r.e.owner == erules::Owner::guest;
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

}  // namespace st::esync
