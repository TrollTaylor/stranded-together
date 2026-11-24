// Enemy sync: P1a the mirrors and P1-0 the freeze probe (enemy_sync.hpp; docs/enemy-sync.md 8 and 7.2).
#include "enemy_sync.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "automation.hpp"
#include "coop_session.hpp"
#include "enemy_rules.hpp"
#include "enemy_wire.hpp"
#include "link.hpp"
#include "npc_damage.hpp"
#include "enemy_census.hpp"
#include "game_actions.hpp"
#include "log.hpp"
#include "partner_source.hpp"
#include "real_body.hpp"
#include "sam_damage.hpp"
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
std::atomic<bool> g_any_frozen{false};
double g_tnow = 0;
game::Entity* g_body = nullptr;  // the partner body (the tick's)
std::uint32_t g_look_id = 0;      // the newest enemy_look's enemy (enemy_shot_test 0 takes it while it is a live mirror)

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
erules::PoseSlots g_pslots;
std::uintptr_t g_pslot_sm[erules::kPoseSlots] = {};
double g_pslot_next[erules::kPoseSlots] = {};
std::uint32_t g_pslot_seq[erules::kPoseSlots] = {};
std::uint32_t g_sen_seq = 0;
struct SenderStats {
    unsigned sen = 0, entries = 0, c10 = 0, c2 = 0, c02 = 0, sep = 0, bytes = 0, no_guest = 0;
} g_ss, g_ss_logged;
double g_ss_next = 0;
std::uint64_t g_shooter_keys[erules::KeySet::kCap];
std::uint32_t g_shooter_vals[erules::KeySet::kCap];

void HostResetPoses() {
    for (unsigned k = 0; k < erules::kPoseSlots; ++k)
        if (g_pslot_sm[k]) realbody::SetEnemySlot(k, 0, realbody::EnemySlotMode::off), g_pslot_sm[k] = 0;
    g_pslots.Reset();
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
    e.owner_epoch = 1;
    e.phase = 0xFF;
    e.state = static_cast<std::uint8_t>((h.now.realized ? ewire::kRealized : 0) | (h.now.dead ? ewire::kDead : 0) | (h.now.hidden_or_asleep ? ewire::kHidden : 0) |
                                        (h.now.held ? ewire::kSeqHeld : 0) | (g_pslots.SlotOf(h.id) >= 0 ? ewire::kPosed : 0));
    const double sp = std::sqrt(static_cast<double>(h.vel[0]) * h.vel[0] + static_cast<double>(h.vel[1]) * h.vel[1]);
    e.flags = sp > 0.2 ? ewire::kMoving : 0;
    for (int k = 0; k < 3; ++k) e.pos[k] = h.now.pos[k], e.vel[k] = h.vel[k];
    e.yaw = YawOfXf(h.now.xf);
    return true;
}

void HostTick(const TickIn& in, double now) {
    double guest[3] = {};
    const bool have_guest = in.body && LocalPlace(in.body, guest);
    if (!have_guest) {
        ++g_ss.no_guest;
        return;  // the guest is in another region (no partner body here): nothing to mirror
    }
    if (now >= g_host_walk_next) {
        g_host_walk_next = now + 500;
        HostWalk(guest, have_guest);
        // the pose slots (2 Hz): the nearest realized within 120 m
        std::vector<erules::PoseCand> pc;
        for (const HostEnemy& h : g_host) pc.push_back(erules::PoseCand{h.id, h.dist, h.now.realized && !h.now.dead});
        g_pslots.Update(pc.data(), static_cast<unsigned>(pc.size()), static_cast<unsigned>(g_ecfg.enemy_pose_count));
        for (unsigned k = 0; k < erules::kPoseSlots; ++k) {
            const std::uint32_t id = g_pslots.Id(k);
            std::uintptr_t sm = 0;
            for (const HostEnemy& h : g_host)
                if (h.id == id && h.now.realized) sm = mem::Q(h.now.entity + 0xB8);
            if (sm == g_pslot_sm[k]) continue;
            if (g_pslot_sm[k]) realbody::SetEnemySlot(k, 0, realbody::EnemySlotMode::off);
            g_pslot_sm[k] = sm && realbody::SetEnemySlot(k, sm, realbody::EnemySlotMode::capture) ? sm : 0;
            g_pslot_seq[k] = 0;
        }
        // the shooters for the enemy shots: the 10 Hz class (entity -> actor id)
        unsigned ns = 0;
        for (const HostEnemy& h : g_host)
            if (ns < erules::KeySet::kCap && h.now.realized && h.dist >= 0 && h.dist <= g_ecfg.enemy_mirror_radius)
                g_shooter_keys[ns] = h.now.entity, g_shooter_vals[ns] = h.id, ++ns;
        shots::SetEnemyShooters(g_shooter_keys, g_shooter_vals, ns);
    }
    // SEN1: the schedule's picks, read fresh, at most 2 frames
    std::vector<erules::Cand> cand;
    cand.reserve(g_host.size());
    for (const HostEnemy& h : g_host) cand.push_back(erules::Cand{h.id, h.dist});
    unsigned pick[erules::Schedule::kPerTick];
    const unsigned np = g_sched.Pick(cand.data(), static_cast<unsigned>(cand.size()), now, g_ecfg.enemy_mirror_radius, pick, erules::Schedule::kPerTick);
    static ewire::EnemyFrame f;
    unsigned at = 0;
    while (at < np) {
        f.n = 0;
        f.flags = 0;
        f.seq = ++g_sen_seq;
        f.sender_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(now) & 0xFFFFFFFFull);
        f.gen_tag = 0;
        for (; at < np && f.n < ewire::kSenMax; ++at) {
            HostEnemy& h = g_host[pick[at]];
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
    // SEP1: each pose slot at its period, a new capture only
    for (unsigned k = 0; k < erules::kPoseSlots; ++k) {
        if (!g_pslot_sm[k] || now < g_pslot_next[k]) continue;
        g_pslot_next[k] = now + g_pslots.Period(k);
        static ewire::EnemyPose p;
        double cms = 0;
        if (!realbody::CaptureEnemyFrame(k, &p.joints, &cms) || p.joints.seq == g_pslot_seq[k]) continue;
        g_pslot_seq[k] = p.joints.seq;
        p.slot = static_cast<std::uint8_t>(k);
        p.actor_id = g_pslots.Id(k);
        p.sender_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(now) & 0xFFFFFFFFull);
        p.joints.sender_ms = p.sender_ms;
        std::uint8_t buf[ewire::kSepMaxSize];
        const std::size_t size = ewire::EncodeEnemyPose(p, buf, sizeof(buf));
        if (size && link::SendCombatFrame(buf, size)) ++g_ss.sep, g_ss.bytes += static_cast<unsigned>(size);
    }
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
bool g_slot_used[erules::kPoseSlots] = {};

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
void FreeSlot(Mirror& m) {
    if (m.slot >= 0) {
        realbody::SetEnemySlot(static_cast<unsigned>(m.slot), 0, realbody::EnemySlotMode::off);
        g_slot_used[m.slot] = false;
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
    ReleaseAll("reset");
    RestoreFar("reset");
    g_tracks.clear();
    g_prx.clear();
    g_clock.Reset();
    g_local.clear();
    g_far_checks.clear();
    g_refused_once.clear();
}

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
        ++g_gs.rx_sen;
        const double sent = g_clock.Unwrap(f.sender_ms);
        g_clock.Note(sent, now);
        for (unsigned i = 0; i < f.n; ++i) {
            const ewire::Enemy& e = f.e[i];
            erules::Sample s;
            s.t = sent;
            std::memcpy(s.pos, e.pos, sizeof(s.pos));
            s.yaw = e.yaw;
            std::memcpy(s.vel, e.vel, sizeof(s.vel));
            s.state = e.state;
            g_tracks[e.actor_id].Push(s, now);
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
        unsigned used = 0;
        for (bool u : g_slot_used) used += u ? 1 : 0;
        if (used >= static_cast<unsigned>(g_ecfg.enemy_pose_count)) {
            ++g_gs.no_pose;
            return;
        }
        for (unsigned k = 0; k < erules::kPoseSlots; ++k)
            if (!g_slot_used[k]) {
                if (m.sm && realbody::SetEnemySlot(k, m.sm, realbody::EnemySlotMode::apply)) g_slot_used[k] = true, m.slot = static_cast<int>(k);
                break;
            }
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

void GuestTick(const TickIn& in, double now, bool off, const char* off_why) {
    GuestDrain(now);
    double sam[3] = {};
    const bool have_sam = in.local_ok && LocalPlace(in.local, sam);
    const bool cutscene = InCutscene();
    const int cut = g_cut_step;  // stepped by Tick
    if (cut == 2) ReleaseAll("cutscene");
    if (g_holding) return;  // nothing driven, posed or posted
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
        l.frame_age_ms = it != g_tracks.end() ? now - it->second.rx_ms : 1e9;
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
        // drive
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
        GivePose(m, it->second, render, it->second.Newest() && (it->second.Newest()->state & ewire::kPosed));
    }
    if (changed) PublishMirrors();
    g_mirrors.erase(std::remove_if(g_mirrors.begin(), g_mirrors.end(), [](const Mirror& m) { return !m.frozen; }), g_mirrors.end());
    // entry (a few a tick)
    unsigned tries = 0;
    for (const auto& kv : g_tracks) {
        if (tries >= 4) break;
        if (FindMirror(kv.first)) continue;
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
            if (!z || now - kv.second.rx_ms > 1500 || Dist3(z->pos, sam) > kFarRadiusM || FindMirror(kv.first)) continue;
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
    // the 5 s line
    if (now >= g_gs_next) {
        g_gs_next = now + 5000;
        unsigned frozen = 0, posed = 0;
        for (const Mirror& m : g_mirrors) frozen += m.frozen ? 1 : 0, posed += m.slot >= 0 ? 1 : 0;
        unsigned applies = 0, nof = 0;
        for (unsigned k = 0; k < erules::kPoseSlots; ++k) {
            const realbody::EnemySlotStats s = realbody::GetEnemySlotStats(k, true);
            if (g_slot_used[k]) applies += s.applies, nof += s.no_frame;
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
    return shotw::kPartMirror;
}

void Stop(const char* step) {
    g_stopped = true;
    Log("[enemy] FAULT in %s: enemy sync stopped", step);
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
    shots::SetEnemyShots(on && g_ecfg.enemy_shots ? g_role.load() : 0);
    if (g_role == 1) {
        if (on) HostTick(in, now);
    } else if (g_role == 2) {
        GuestTick(in, now, !on, gate == erules::FpGate::differ ? "world" : "off");
    }
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
            return std::snprintf(msg, cap, "mirrors n=%zu frozen=%u (%s)", g_mirrors.size(), frozen, g_mirroring ? "on" : "off"), true;
        }
        case 1:
            if (!g_want || !g_p1_ok) return std::snprintf(msg, cap, "enemy sync is not installed (enemy_sync=0 or byte checks)"), false;
            if (g_stopped) return std::snprintf(msg, cap, "stopped after a fault"), false;
            g_switch = true;
            Log("[enemy] sync on (debug action)");
            return std::snprintf(msg, cap, "on"), true;
        case 2: {
            const std::size_t n = g_mirrors.size();
            g_switch = false;
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
        default: return std::snprintf(msg, cap, "unknown op %d (0 status, 1 on, 2 off, 3 table, 6 variant, 7 cadence)", op), false;
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
    const std::uint32_t want = v[0] > 0 ? static_cast<std::uint32_t>(v[0]) : v[0] == -2 || behind || side ? shots::LastEnemyShooter() : 0;
    if ((v[0] == -2 || behind || side) && !want) return std::snprintf(msg, cap, "no enemy shot replayed yet"), false;
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
        if (!ReadActor(e.actor, e.id, a) || !a.realized || a.dead || !a.have_xf) continue;
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
    how = side ? "beside the newest replayed shooter's line to the partner" : behind ? "behind the newest replayed shooter, toward the partner" : v[0] == -2 ? (g_role == 1 ? "the newest sent shooter" : "the newest replayed shooter") : want ? "by id" : faced ? "the one the partner faces" : g_role == 2 ? "the nearest frozen mirror" : "the nearest realized";
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
