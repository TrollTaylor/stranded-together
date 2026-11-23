#include "hood_fold.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "exo_parts.hpp"
#include "log.hpp"
#include "partner_source.hpp"

namespace st::hoodfold {
namespace {

namespace mem = partner::mem;
using mem::Q;
using mem::Rd;

// ---------------------------------------------------------------- the structures (Decima RTTI, ds.exe; round 12)
// Entity +0xB8 SkinnedModel -> +0x40 SkinnedModelRep -> +0x28 the model drawable (suit_colour.cpp's chain, 0x1422BBB80);
// drawable +0x98 -> +0x40 i32 mesh count, +0x48 entries of 0x1B0, entry +0xE0 the LodMeshResource (census of run
// 20261122-154545: her body has one entry, LodMeshResource 96d8545e).
// LodMeshResource (RTTI 0x14488B490): +0x88 Meshes, Array<LodMeshResourcePart> {u32 count, u32 capacity, T* data}; a part
// {+0x0 Ref<MeshResourceBase> Mesh, +0x8 float Distance} (stride found and checked at run time).
// MultiMeshResource (0x14488B880): +0x80 Meshes, Array<Ref<MeshResourceBase>>.
// RegularSkinnedMeshResource (0x144A630D0, 0xC0 bytes): +0x98 Ref<SkinnedMeshIndexedJointBindings>.
// SkinnedMeshIndexedJointBindings (0x144A63010): +0x20 JointIndexList Array<u16>, +0x30 InverseBindMatrices Array<Mat44>.
// RTTIRefObject: +0x8 its GUID.
constexpr std::uintptr_t kDrMeshes = 0x98, kMeshEntry = 0x1B0, kEntryLod = 0xE0;
constexpr std::uintptr_t kLodParts = 0x88, kMultiMeshes = 0x80, kRsmBindings = 0x98;
constexpr std::uintptr_t kBindJoints = 0x20, kBindMatrices = 0x30;
constexpr std::size_t kMat = 64;
// mesh D's GUID 7f533fe6-3f35-8430-b5c9-d4ea7da84faf as stored (the first two fields little-endian)
constexpr std::uint8_t kMeshDGuid[16] = {0xE6, 0x3F, 0x53, 0x7F, 0x35, 0x3F, 0x30, 0x84, 0xB5, 0xC9, 0xD4, 0xEA, 0x7D, 0xA8, 0x4F, 0xAF};
// the first inverse bind matrix of mesh D (binding 0 = Spine5) from the game data: rows 0 and 1
constexpr float kMat0[8] = {-1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -0.1045f, 0.9945f, 0.0f};

struct Target {
    std::uintptr_t lod = 0;        // the LodMeshResource
    std::uintptr_t mesh = 0;       // mesh D
    std::uintptr_t binds = 0;      // its SkinnedMeshIndexedJointBindings
    std::uintptr_t mats = 0;       // its InverseBindMatrices data
    std::uintptr_t lod_parts = 0;  // the LOD parts' data, stride, count (0: no pin)
    std::uintptr_t lod_stride = 0;
    unsigned lod_n = 0;
    float lod_dist[8] = {};
    std::uint8_t saved[exo::kFoldRingCount][kMat] = {};  // the roll's matrices as the game had them
    exo::FoldPlan applied;
    bool dirty = false;            // something of ours may be in the resource (a write begun): written back before it is forgotten
    bool valid = false;
    unsigned misses = 0;           // ticks in a row no body of ours drew it (exo::FoldDrop)
    double miss_since = 0;
};

struct State {
    bool cfg = true;
    bool off = false;  // a mismatch or a failed write: no hiding this session
    char why[200] = "";
    int method = 1;    // gear 34: 0 never, 1 the inverse binds
    bool pin = true;
    Target t;
    unsigned hides = 0, shows = 0, logs = 0;
    unsigned find_fails = 0;   // finds failed in a row (exo::kFoldFindTries)
    double find_next = 0;
    unsigned back_tries = 0;   // write-back attempts after the hiding went off with something still ours in the resource
    double back_next = 0;
} g;

const char* TypeOf(std::uintptr_t o) {
    const char* n = o ? mem::RttiName(mem::RttiOf(o)) : nullptr;
    return n ? n : "?";
}
bool Is(std::uintptr_t o, const char* name) { return o && std::strcmp(TypeOf(o), name) == 0; }

bool Write(std::uintptr_t a, const void* in, std::size_t n) {
    SIZE_T put = 0;
    return a >= 0x10000 && WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), in, n, &put) && put == n;
}

void Off(const char* fmt, const char* what) {
    g.off = true;
    std::snprintf(g.why, sizeof(g.why), fmt, what);
    Log("[hood] her folded hood is not hidden this session: %s", g.why);
}

// The LodMeshResource a body draws (0: none readable).
std::uintptr_t BodyLod(std::uintptr_t e) {
    if (!e) return 0;
    const std::uintptr_t a = Q(e + 0xB8), rep = a ? Q(a + 0x40) : 0, dr = rep ? Q(rep + 0x28) : 0, m = dr ? Q(dr + kDrMeshes) : 0;
    std::int32_t n = 0;
    if (!m || !Rd(m + 0x40, n) || n < 1 || n > 64) return 0;
    const std::uintptr_t arr = Q(m + 0x48);
    for (std::int32_t i = 0; arr && i < n; ++i) {
        const std::uintptr_t lod = Q(arr + static_cast<std::uintptr_t>(i) * kMeshEntry + kEntryLod);
        if (Is(lod, "LodMeshResource")) return lod;
    }
    return 0;
}

// An Array {u32 count, u32 capacity, T* data} at `a`.
bool ArrayAt(std::uintptr_t a, std::uint32_t* n, std::uintptr_t* data) {
    std::uint32_t c[2] = {};
    if (!mem::Read(a, c, sizeof(c)) || c[0] == 0 || c[0] > 4096 || c[1] < c[0]) return false;
    *n = c[0];
    *data = Q(a + 8);
    return *data != 0;
}

// Finds and checks mesh D under `lod`. False + why.
bool Find(std::uintptr_t lod, Target* t, char* why, std::size_t cap) {
    *t = Target{};
    t->lod = lod;
    std::uint32_t nparts = 0;
    std::uintptr_t parts = 0;
    if (!ArrayAt(lod + kLodParts, &nparts, &parts) || nparts < 1 || nparts > 8)
        return std::snprintf(why, cap, "her LodMeshResource %p has no LOD list at +0x88", reinterpret_cast<void*>(lod)), false;
    // the parts' stride: each part's Mesh a MeshResourceBase, its distances increasing
    for (std::uintptr_t stride : {std::uintptr_t{0x10}, std::uintptr_t{0x20}}) {
        bool ok = true;
        float d[8] = {};
        for (std::uint32_t i = 0; ok && i < nparts; ++i)
            ok = mem::DerivesFrom(mem::RttiOf(Q(parts + i * stride)), "MeshResourceBase") && Rd(parts + i * stride + 8, d[i]);
        if (ok && exo::FoldLodDistancesOk(d, nparts)) {
            t->lod_parts = parts, t->lod_stride = stride, t->lod_n = nparts;
            std::memcpy(t->lod_dist, d, sizeof(d));
            break;
        }
    }
    const std::uintptr_t lod0 = Q(parts);
    if (!Is(lod0, "MultiMeshResource"))
        return std::snprintf(why, cap, "her LOD0 is a %s, not a MultiMeshResource", TypeOf(lod0)), false;
    std::uint32_t nm = 0;
    std::uintptr_t meshes = 0;
    if (!ArrayAt(lod0 + kMultiMeshes, &nm, &meshes) || nm > 16) return std::snprintf(why, cap, "her LOD0 has no mesh list at +0x80"), false;
    for (std::uint32_t i = 0; i < nm && !t->mesh; ++i) {
        const std::uintptr_t m = Q(meshes + 8ull * i);
        std::uint8_t gd[16] = {};
        if (m && mem::Read(m + 8, gd, 16) && std::memcmp(gd, kMeshDGuid, 16) == 0) t->mesh = m;
    }
    if (!t->mesh) return std::snprintf(why, cap, "no mesh 7f533fe6 (her folded hood) among her LOD0's %u meshes", nm), false;
    if (!Is(t->mesh, "RegularSkinnedMeshResource")) return std::snprintf(why, cap, "mesh 7f533fe6 is a %s", TypeOf(t->mesh)), false;
    t->binds = Q(t->mesh + kRsmBindings);
    if (!Is(t->binds, "SkinnedMeshIndexedJointBindings"))
        return std::snprintf(why, cap, "mesh 7f533fe6's +0x98 is a %s, not its joint bindings", TypeOf(t->binds)), false;
    std::uint32_t nj = 0, nmat = 0;
    std::uintptr_t joints = 0;
    std::uint16_t list[exo::kFoldBindings] = {};
    if (!ArrayAt(t->binds + kBindJoints, &nj, &joints) || nj != exo::kFoldBindings || !mem::Read(joints, list, sizeof(list)) ||
        !exo::FoldJointsOk(list, nj))
        return std::snprintf(why, cap, "mesh 7f533fe6's joint list is not the 22 joints of the game data (%u read)", nj), false;
    if (!ArrayAt(t->binds + kBindMatrices, &nmat, &t->mats) || nmat != exo::kFoldBindings)
        return std::snprintf(why, cap, "mesh 7f533fe6 has %u inverse bind matrices, not 22", nmat), false;
    float m0[8] = {};
    if (!mem::Read(t->mats, m0, sizeof(m0))) return std::snprintf(why, cap, "its inverse bind matrices do not read"), false;
    for (int i = 0; i < 8; ++i)
        if (std::fabs(m0[i] - kMat0[i]) > 0.002f)
            return std::snprintf(why, cap, "its first inverse bind matrix is not the game data's (%.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f)", m0[0], m0[1],
                                 m0[2], m0[3], m0[4], m0[5], m0[6], m0[7]),
                   false;
    static const std::uint8_t kZero[kMat] = {};
    unsigned k = 0;
    for (unsigned b = 0; b < exo::kFoldBindings; ++b) {
        if (!exo::FoldRing(b)) continue;
        if (!mem::Read(t->mats + b * kMat, t->saved[k], kMat)) return std::snprintf(why, cap, "a matrix does not read"), false;
        // (a zero matrix is what a hide writes: the game's own are never zero, and a saved zero could not be told from ours)
        if (std::memcmp(t->saved[k], kZero, kMat) == 0) return std::snprintf(why, cap, "roll binding %u's matrix is already zero", b), false;
        ++k;
    }
    t->valid = true;
    return true;
}

// Still the same objects (types re-read, the matrices' data where it was): nothing is written otherwise.
bool Same(const Target& t) {
    return t.valid && Is(t.mesh, "RegularSkinnedMeshResource") && Q(t.mesh + kRsmBindings) == t.binds && Is(t.binds, "SkinnedMeshIndexedJointBindings") &&
           Q(t.binds + kBindMatrices + 8) == t.mats;
}

// The roll's matrices zeroed (hide) or the saved ones (show). All ten are checked first (each one what the game had, or
// what we wrote), so a check that fails writes nothing; then written in their two runs (bindings 0-5 and 10-13: one write
// each, the shortest window a frame could read half of them).
bool SetBinds(Target& t, bool hide) {
    static const std::uint8_t kZero[kMat] = {};
    std::uint8_t cur[exo::kFoldBindings][kMat] = {};
    if (!mem::Read(t.mats, cur, sizeof(cur))) return false;
    unsigned k = 0;
    for (unsigned b = 0; b < exo::kFoldBindings; ++b) {
        if (!exo::FoldRing(b)) continue;
        const bool is_saved = std::memcmp(cur[b], t.saved[k], kMat) == 0, is_zero = std::memcmp(cur[b], kZero, kMat) == 0;
        if (exo::FoldBackOf(is_saved, is_zero) == exo::FoldBack::foreign) return false;  // not ours to write
        std::memcpy(cur[b], hide ? kZero : t.saved[k], kMat);
        ++k;
    }
    t.dirty = true;
    for (unsigned b = 0; b < exo::kFoldBindings;) {
        if (!exo::FoldRing(b)) {
            ++b;
            continue;
        }
        unsigned e = b;
        while (e < exo::kFoldBindings && exo::FoldRing(e)) ++e;
        if (!Write(t.mats + b * kMat, cur[b], (e - b) * kMat)) return false;
        b = e;
    }
    return true;
}
bool SetPin(Target& t, bool pin) {
    if (!t.lod_parts) return false;
    for (unsigned i = 1; i < t.lod_n; ++i) {
        float cur = 0;
        if (!Rd(t.lod_parts + i * t.lod_stride + 8, cur) || (cur != t.lod_dist[i] && cur != exo::kFoldPinDistance)) return false;
    }
    t.dirty = true;
    for (unsigned i = 1; i < t.lod_n; ++i) {
        const float v = pin ? exo::kFoldPinDistance : t.lod_dist[i];
        if (!Write(t.lod_parts + i * t.lod_stride + 8, &v, sizeof(v))) return false;
    }
    return true;
}

// Brings the target to `want` (one step of each kind). False: a check or a write failed (RollBack next, then off).
bool Apply(Target& t, const exo::FoldPlan& want) {
    if (!t.valid) return true;
    if (!Same(t)) return false;
    bool ok = true;
    if (want.binds != t.applied.binds) {
        ok = SetBinds(t, want.binds);
        if (ok) t.applied.binds = want.binds;
    }
    if (ok && want.pin != t.applied.pin) {
        ok = SetPin(t, want.pin);
        if (ok) t.applied.pin = want.pin;
    }
    if (ok) t.dirty = t.applied.Any();
    return ok;
}

// Everything of ours written back whatever `applied` says (a hide or a pin that stopped part way included): every roll
// matrix that is zero gets its saved bytes, every LOD distance that is the pin gets the game's, a foreign one is left and
// the others still go back. True: the resource is as the game had it (then forgotten as clean). Only while the objects
// are still the same (Same): a resource that changed is not written.
bool RollBack(Target& t) {
    if (!t.valid) return true;
    if (!Same(t)) return false;
    static const std::uint8_t kZero[kMat] = {};
    bool clean = true;
    unsigned k = 0;
    for (unsigned b = 0; b < exo::kFoldBindings; ++b) {
        if (!exo::FoldRing(b)) continue;
        std::uint8_t cur[kMat] = {};
        const std::uintptr_t a = t.mats + b * kMat;
        if (!mem::Read(a, cur, kMat)) {
            clean = false;
        } else {
            switch (exo::FoldBackOf(std::memcmp(cur, t.saved[k], kMat) == 0, std::memcmp(cur, kZero, kMat) == 0)) {
                case exo::FoldBack::as_game: break;
                case exo::FoldBack::restore:
                    if (!Write(a, t.saved[k], kMat)) clean = false;
                    break;
                case exo::FoldBack::foreign: clean = false; break;
            }
        }
        ++k;
    }
    for (unsigned i = 1; t.lod_parts && i < t.lod_n; ++i) {
        const std::uintptr_t a = t.lod_parts + i * t.lod_stride + 8;
        float cur = 0;
        if (!Rd(a, cur)) clean = false;
        else if (cur == exo::kFoldPinDistance) clean = Write(a, &t.lod_dist[i], sizeof(float)) && clean;
        else if (cur != t.lod_dist[i]) clean = false;
    }
    if (clean) {
        t.applied = exo::FoldPlan{};
        t.dirty = false;
    }
    return clean;
}

const char* PlanText(const exo::FoldPlan& p, char* buf, std::size_t cap) {
    std::snprintf(buf, cap, "%s%s", p.binds ? "mesh D's 10 roll bindings zeroed" : "", p.pin ? ", LOD0 pinned" : "");
    return buf;
}

bool Ours(const Target& t) { return t.dirty || t.applied.Any(); }

}  // namespace

void Configure(bool on) noexcept {
    g.cfg = on;
    Log("[hood] her folded hood hidden while her raised hood shows: %s (session.ini hood_fold_hide)", on ? "on" : "off");
}
bool On() noexcept { return g.cfg && !g.off; }

void SetTest(int method, bool nopin) noexcept {
    g.method = method < 0 ? 1 : method > 1 ? 1 : method;
    g.pin = method < 0 ? true : !nopin;
    Log("[hood] TEST: her folded hood's method %d (%s), LOD pin %s", g.method, g.method == 0 ? "never hidden" : "the inverse binds", g.pin ? "on" : "off");
}

void Tick(std::uintptr_t partner_body, bool partner_hide, std::uintptr_t self_body, bool self_hide, double now_ms) noexcept {
    const std::uintptr_t lods[2] = {BodyLod(partner_body), BodyLod(self_body)};
    const bool hide[2] = {partner_hide, self_hide};
    Target& t = g.t;
    // the target's resource must still be drawn by one of our bodies; missing for a few ticks (exo::FoldDrop): written back
    // (it may be gone: only while the objects are still exactly what we left) and forgotten
    if (t.lod && lods[0] != t.lod && lods[1] != t.lod) {
        if (t.misses++ == 0) t.miss_since = now_ms;
        if (!exo::FoldDrop(t.misses, t.miss_since, now_ms)) return;
        if (Ours(t)) {
            const bool back = RollBack(t);
            Log("[hood] her folded hood: the body drawing it is gone (%s)", back ? "shown again" : "not written back: the resource changed");
            if (back) ++g.shows;
        }
        t = Target{};
        g.back_tries = 0;
    } else {
        t.misses = 0;
    }
    // the hiding went off with something of ours still in the resource: written back (a few tries, then left to the emergency stop)
    if (g.off) {
        if (Ours(t) && g.back_tries < 5 && now_ms >= g.back_next) {
            ++g.back_tries;
            g.back_next = now_ms + 1000;
            const bool back = RollBack(t);
            Log("[hood] her folded hood: %s (after the hiding went off, try %u)", back ? "shown again" : "NOT written back yet (the resource changed)",
                g.back_tries);
            if (back) t = Target{};
        }
        return;
    }
    const exo::FoldPick pick = exo::FoldPickOf(t.lod, lods, hide, On());
    if (pick.drop) {
        // one resource at a time: ours is wanted by nobody now, another one is
        if (Ours(t)) {
            const bool back = RollBack(t);
            Log("[hood] her folded hood: %s on the resource no body wants hidden now (another one is)", back ? "shown again" : "not written back: the resource changed");
            if (!back) {
                Off("%s", "a resource of hers changed while it was hidden");
                return;
            }
            ++g.shows;
        }
        t = Target{};
    }
    if (pick.find && !t.lod) {
        if (now_ms < g.find_next) return;
        char why[200] = {};
        Target n;
        if (!Find(pick.find, &n, why, sizeof(why))) {
            if (++g.find_fails >= exo::kFoldFindTries) {
                Off("%s", why);
                return;
            }
            g.find_next = now_ms + exo::kFoldFindRetryMs;
            Log("[hood] her folded hood not found yet (%s): trying again in 1 s (%u of %u)", why, g.find_fails, exo::kFoldFindTries);
            return;
        }
        g.find_fails = 0;
        t = n;
        char d[120] = "no LOD list";
        if (t.lod_parts) {
            int at = std::snprintf(d, sizeof(d), "LOD distances");
            for (unsigned i = 0; i < t.lod_n && at > 0 && at < static_cast<int>(sizeof(d)) - 12; ++i)
                at += std::snprintf(d + at, sizeof(d) - static_cast<std::size_t>(at), " %.2f", static_cast<double>(t.lod_dist[i]));
        }
        Log("[hood] her folded hood found: LodMeshResource %p, mesh D %p (bindings %p, 22 joints checked, matrices %p), %s",
            reinterpret_cast<void*>(t.lod), reinterpret_cast<void*>(t.mesh), reinterpret_cast<void*>(t.binds), reinterpret_cast<void*>(t.mats), d);
    }
    const bool want = pick.want;
    if (!want && !Ours(t)) return;
    const exo::FoldPlan plan = exo::FoldPlanOf(want, g.method, g.pin && t.lod_parts != 0);  // (no LOD list: LOD0 only)
    if (plan == t.applied) return;
    const bool was = t.applied.binds;
    if (!Apply(t, plan)) {
        // everything of ours written back (a hide that stopped part way too), then off for the session
        const bool back = RollBack(t);
        Off("%s", back ? "mesh D's data was not what the mod read or wrote (changed by the game?); everything written back"
                       : "mesh D's data was not what the mod read or wrote (changed by the game?); NOT all written back yet");
        if (back) t = Target{};
        return;
    }
    const bool now_hidden = t.applied.binds;
    if (now_hidden != was || g.logs < 40) {
        ++g.logs;
        char b[160];
        if (now_hidden && !was) ++g.hides;
        if (!now_hidden && was) ++g.shows;
        Log("[hood] her folded hood %s (%s; partner body %s, self body %s; #%u)", now_hidden ? "hidden" : "shown again",
            now_hidden ? PlanText(t.applied, b, sizeof(b)) : "everything written back", partner_hide ? "wants it hidden" : lods[0] ? "not" : "none",
            self_hide ? "wants it hidden" : lods[1] ? "not" : "none", now_hidden ? g.hides : g.shows);
    }
}

void RestoreAll(const char* why) noexcept {
    Target& t = g.t;
    if (!Ours(t)) return;
    const bool ok = RollBack(t);
    Log("[hood] her folded hood: %s (%s)", ok ? "shown again" : "NOT written back (the resource changed)", why);
    if (ok) ++g.shows;
}

const char* Describe(char* buf, std::size_t cap) noexcept {
    char b[160] = "";
    std::snprintf(buf, cap, "folded hood %s: method %d, pin %s, %s; %u hidden, %u shown again%s%s", g.cfg ? (g.off ? "OFF" : "on") : "off (setting)", g.method,
                  g.pin ? "on" : "off", g.t.applied.Any() ? PlanText(g.t.applied, b, sizeof(b)) : g.t.dirty ? "partly written (not all back yet)" : "nothing changed now",
                  g.hides, g.shows, g.off ? ": " : "", g.off ? g.why : "");
    return buf;
}

}  // namespace st::hoodfold
