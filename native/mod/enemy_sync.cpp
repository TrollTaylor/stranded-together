// Enemy sync P1-0: the freeze probe (enemy_sync.hpp; docs/enemy-sync.md 4.7).
#include "enemy_sync.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "automation.hpp"
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

void Tick(game::Entity* /*local*/, bool /*local_ok*/, double now) noexcept {
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

}  // namespace st::esync
