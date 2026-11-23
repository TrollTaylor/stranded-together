#include "hood_fold.hpp"

#include <windows.h>
#include <intrin.h>

#include <cstdarg>
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

// ---------------------------------------------------------------- the structures (ds.exe; rounds 12-13, section 37.8)
// Entity +0xB8 SkinnedModel -> +0x40 SkinnedModelRep -> +0x28 the model drawable (suit_colour.cpp's chain);
// drawable +0x40 float LodDistanceScale (DrawableObjectInstance +0x30), +0x98 a ModelRep (hers a SkinnedModelRep): +0x40 i32 entry count, +0x48
// entries of 0x1B0 (one per Model part; her body: 1). Entry +0xE0 the mesh resource (her LodMeshResource 96d8545e),
// +0xF0 the holder of its own mesh instance tree (built per entry by 0x142391820) {u64 tree, u32 capacity}, +0x108 i32 (-1: a rebuild pending),
// +0x10C u8 the view mask the game's part visibility gives every slot, +0x110 an alternate holder (a skinned instance:
// {u64 holder*, i32 start node}).
// The tree: +0 u32 bytes used (after +0x20), +0x20 the root node. A node: +0 resource, +8 u16 count, +0xA u8 type, +0x10
// i32 next sibling (relative to node+0x10), +0x18 i32 first child (relative to node+0x18; types 2 and 3).
//   type 3 LodMesh: +0x48 float[count + 1] the LOD thresholds (the parts' distances copied at build, then MaxDistance);
//   type 2 MultiMesh (her LOD0: the meshes A, B, C, D);
//   type 4 a skinned mesh (mesh D: RTTI RegularSkinnedMeshResource): +0x48 a slot per primitive {+0 record*, +8 u8 view mask, +0xA flags (bit 3:
//     +0 is a record)}; then the bindings pointer (her LodMesh's merged SkinnedMeshIndexedJointBindings, or the mesh's
//     own), then per-primitive skin data (+0xC: the skin mode).
// LodMeshResource: +0x80 MaxDistance, +0x88 Array<LodMeshResourcePart {Ref Mesh, float Distance}>, +0x98 the merged
// bindings it builds once for its instance trees. MultiMeshResource +0x80 Array<Ref<MeshResourceBase>>. Mesh +0x98 its own
// bindings. SkinnedMeshIndexedJointBindings (0x50 bytes): +0x20 Array<u16> joints, +0x30 Array<Mat44> inverse binds,
// +0x40 DataHash. RTTIRefObject +0x8 its GUID.
constexpr std::uintptr_t kDrLodScale = 0x40, kDrMeshes = 0x98, kRepCount = 0x40, kRepEntries = 0x48, kMeshEntry = 0x1B0;
constexpr std::uintptr_t kEntryLod = 0xE0, kEntryHolder = 0xF0, kEntryBuild = 0x108, kEntryMask = 0x10C, kEntryAlt = 0x110;
constexpr std::uintptr_t kLodMax = 0x80, kLodParts = 0x88, kLodMerged = 0x98, kLodHier = 0x72, kMultiMeshes = 0x80, kMeshBinds = 0x98;
constexpr std::uintptr_t kBindJoints = 0x20, kBindMatrices = 0x30, kBindHash = 0x40, kBindSize = 0x50, kBindVtable = 0x3C11F18;
constexpr std::uintptr_t kTreeNodes = 0x20, kNodeThresh = 0x48, kSlotRec = 0x48, kSlotMask = 0x50, kSlotFlags = 0x52, kNodeBinds = 0x58,
                         kNodeMode = 0x6C, kDNodeSize = 0x70;
constexpr std::size_t kMat = 64;
// mesh D's GUID 7f533fe6-3f35-8430-b5c9-d4ea7da84faf as stored (the first two fields little-endian)
constexpr std::uint8_t kMeshDGuid[16] = {0xE6, 0x3F, 0x53, 0x7F, 0x35, 0x3F, 0x30, 0x84, 0xB5, 0xC9, 0xD4, 0xEA, 0x7D, 0xA8, 0x4F, 0xAF};

// The code that makes these layouts what they are (checked once, before the first write; a mismatch: off).
struct Sig {
    std::uintptr_t rva;
    const char* bytes;
    const char* what;
};
constexpr Sig kSigs[] = {
    {0x1B19BFC, "B8 50 00 00 00 40 88 34 10 48 8D 40 10", "the slot mask at node+0x50, stride 0x10 (the part visibility)"},
    // her entry has +0x110 set: the part visibility 0x142395DE0 goes through 0x141D914D0 (its tail jump to 0x141D914F0),
    // whose loop writes the entry's mask into node+0x50 of every slot of node types 1, 4, 5, 7, 8 (0x1B2) and 6
    {0x2395E0F, "0F BE D0 4D 85 C0 74 1A 49 8B C8 E8 B1 B6 9F FF", "an entry with +0x110: its mask through 0x141D914D0"},
    {0x1D914D0, "8B 41 0C 89 51 0C 3B 41 10 0F 84 11 00 00 00 C3", "0x141D914D0 goes on to the slot-mask loop"},
    {0x1D915B7, "BB B2 01 00 00 0F 1F 40 00 0F B6 4A 0A 80 F9 08 77 05 0F A3 CB 72 05 80 F9 06 75 26 0F B7 4A 08 48 85 C9 74 1D B8 50 00 00 00 40 88 34 10 48 8D 40 10",
     "the slot masks at node+0x50, stride 0x10, node types 1/4/5/6/7/8 (her entry's part visibility)"},
    {0x1C9B093, "4C 8D 71 48", "the culler's slots at node+0x48"},
    {0x1C9B100, "41 84 4E 08 0F 84", "the culler skips a slot whose mask is 0"},
    {0x2395DF7, "0F B6 81 A8 01 00 00", "entry+0x1A8"},
    {0x2395E02, "4C 8B 81 10 01 00 00 88 81 0C 01 00 00", "entry+0x110 / +0x10C"},
    {0x2395E31, "48 8D 81 F0 00 00 00", "entry+0xF0 the tree holder"},
    {0x23A8C3F, "48 69 C8 B0 01 00 00 49 03 49 48 E9", "ModelRep entries +0x48, 0x1B0"},
    {0x1C9996C, "48 63 4A 10", "a node's sibling at +0x10"},
    {0x1C9999A, "48 63 47 18 48 83 C7 18", "a group node's first child at +0x18"},
    {0x1C957FE, "8B 82 E8 00 00 00", "a skinned node's count from the mesh's primitives"},
    {0x1C95810, "66 C7 41 0A 04 00", "skinned node type 4"},
    {0x1C95793, "66 C7 40 0A 02 00", "MultiMesh node type 2"},
    {0x1C95745, "C7 41 08 00 00 03 00", "LodMesh node type 3"},
    {0x1C98942, "41 89 44 8E 48", "the LOD thresholds copied to node+0x48"},
    {0x1C99CB6, "49 8D 56 48", "the LOD choice reads node+0x48"},
    {0x1C98F4F, "48 8B 8D 98 00 00 00", "a skinned node's bindings: the mesh's own as the fallback"},
    {0x1C98F5D, "48 89 4C C7 48", "a skinned node's bindings after its slots"},
    {0x1D92FA7, "41 0F B7 71 08", "the skinning reads a node's count"},
    {0x1D92FB6, "4D 8B 5C C1 48", "the skinning reads a node's bindings"},
    {0x1D92FBF, "49 63 4B 20 49 8B 43 28 4D 8B 43 38", "the skinning reads the joints and inverse binds"},
    {0x1D933A5, "4C 39 45 98 0F 84", "the palette rebuilt for another inverse bind array"},
    {0x245B560, "48 8B 41 28 C5 FA 11 48 40 C3", "LodDistanceScale at drawable+0x40 (ModelLodBiasEvent)"},
    {0x1D5CB0C, "C4 C1 7A 10 46 30", "LodDistanceScale read by the cull"},
};

// One instance tree as read this tick (addresses absolute).
struct TreeRead {
    std::uintptr_t holder = 0, tree = 0, root = 0, lod0 = 0, dnode = 0, dres = 0;
    std::uint32_t used = 0, hflags = 0;
    float t0 = 0, t1 = 0, tmax = 0;
    std::uint8_t mask = 0, flags = 0;
    std::uintptr_t slot = 0, rec = 0;
    std::uint32_t mode = 0;
};
struct BodyRead {
    std::uintptr_t dr = 0, rep = 0, entry = 0, lod = 0, alt = 0;
    std::int32_t build = 0;
    std::uint8_t show = 0;
    float scale = 0;
    TreeRead t[2];
    int nt = 0;
};
// Her LodMeshResource, checked once.
struct Res {
    std::uintptr_t lod = 0, lod0 = 0, d = 0;
    std::uint8_t lod0_guid[16] = {};
    float lod1 = 0;
};
// What is ours in one tree (forgotten when the tree is gone: a rebuilt tree has the game's values again). Where it was
// found (its holder, the entry whose view mask it gives back, her LodMeshResource) for the write-back of a tree the walk
// no longer reaches.
struct TreeState {
    std::uintptr_t tree = 0, dnode = 0, holder = 0, entry = 0, lod = 0;
    exo::FoldTree f;  // the mask, the pin
    bool binds = false;
    std::uintptr_t binds_was = 0, binds_ours = 0;
};
struct Body {
    std::uintptr_t e = 0, dr = 0;
    std::uintptr_t vt = 0;  // the entity's vtable and GUID when it was taken (alive: both still read so)
    game::Guid guid;
    bool id = false;
    TreeState t[2];
    bool scale = false;
    float scale_was = 0;
    bool hidden = false;   // the summary last logged
    bool probed = false;
    bool hood_seen = false;  // its raised hood wanted since it was last shown (the reason logged)
    unsigned hard = 0, soft_logs = 0;
    std::uintptr_t fail_dr = 0, fail_lod = 0;  // where the last hard failure was (another one: counted afresh)
    double next_try = 0;
    char state[96] = "none";
};
// The private bindings (method 2), one per merged bindings object: made once, never freed while the game runs.
struct Priv {
    std::uintptr_t merged = 0, ours = 0;
};

struct State {
    bool cfg = true;
    bool off = false;
    char why[200] = "";
    int code = -1;  // the byte checks: -1 not yet, 0 a mismatch, 1 ok
    int method = exo::kFoldProductMethod;
    int pin = 0;    // gear 34's second number
    Res res[2];
    Body body[2];   // 0 the partner slot, 1 the self view
    Priv priv[4];
    unsigned hides = 0, shows = 0, writes = 0, cas_fails = 0, logs = 0;
} g;

std::uint8_t g_buf[0x4000];

const char* kWho[2] = {"partner", "self"};

const char* TypeOf(std::uintptr_t o) {
    const char* n = o ? mem::RttiName(mem::RttiOf(o)) : nullptr;
    return n ? n : "?";
}
bool Is(std::uintptr_t o, const char* name) { return o && std::strcmp(TypeOf(o), name) == 0; }

// Compare-and-swap writes: the value changes only while it is still what was read (atomic; a fault is caught).
bool Cas8(std::uintptr_t a, std::uint8_t old, std::uint8_t v) {
    __try {
        return static_cast<std::uint8_t>(_InterlockedCompareExchange8(reinterpret_cast<volatile char*>(a), static_cast<char>(v), static_cast<char>(old))) == old;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool Cas32(std::uintptr_t a, std::uint32_t old, std::uint32_t v) {
    if (a & 3) return false;
    __try {
        return static_cast<std::uint32_t>(_InterlockedCompareExchange(reinterpret_cast<volatile long*>(a), static_cast<long>(v), static_cast<long>(old))) == old;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool Cas64(std::uintptr_t a, std::uint64_t old, std::uint64_t v) {
    if (a & 7) return false;
    __try {
        return static_cast<std::uint64_t>(_InterlockedCompareExchange64(reinterpret_cast<volatile long long*>(a), static_cast<long long>(v), static_cast<long long>(old))) == old;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
std::uint32_t Bits(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
bool CasF(std::uintptr_t a, float old, float v) { return Cas32(a, Bits(old), Bits(v)); }

void Off(const char* fmt, const char* what);

bool CodeOk() {
    if (g.code >= 0) return g.code == 1;
    const std::uintptr_t b = mem::Base();
    for (const Sig& s : kSigs) {
        if (!mem::CodeMatches(b + s.rva, s.bytes)) {
            g.code = 0;
            char w[160];
            std::snprintf(w, sizeof(w), "the code at 0x%llX does not match (%s)", static_cast<unsigned long long>(0x140000000ull + s.rva), s.what);
            Off("%s", w);
            return false;
        }
    }
    g.code = 1;
    Log("[hood] her folded hood: %u code sites verified (the instance tree's slot masks, LOD thresholds and bindings)", static_cast<unsigned>(sizeof(kSigs) / sizeof(kSigs[0])));
    return true;
}

// An Array {u32 count, u32 capacity, T* data} at `a`.
bool ArrayAt(std::uintptr_t a, std::uint32_t* n, std::uintptr_t* data) {
    std::uint32_t c[2] = {};
    if (!mem::Read(a, c, sizeof(c)) || c[0] == 0 || c[0] > 4096 || c[1] < c[0]) return false;
    *n = c[0];
    *data = Q(a + 8);
    return *data != 0;
}
bool GuidIs(std::uintptr_t o, const std::uint8_t (&want)[16]) {
    std::uint8_t gd[16] = {};
    return o && mem::Read(o + 8, gd, 16) && std::memcmp(gd, want, 16) == 0;
}

// Her LodMeshResource checked once (cached): the LOD list, LOD0 a MultiMeshResource, mesh D in it by GUID.
const Res* ResOf(std::uintptr_t lod, char* why, std::size_t cap) {
    for (const Res& r : g.res)
        if (r.lod == lod) return &r;
    Res r;
    r.lod = lod;
    std::uint32_t nparts = 0;
    std::uintptr_t parts = 0;
    if (!ArrayAt(lod + kLodParts, &nparts, &parts) || nparts != 2)
        return std::snprintf(why, cap, "her LodMeshResource %p has no 2-part LOD list at +0x88", reinterpret_cast<void*>(lod)), nullptr;
    float d[2] = {};
    std::uintptr_t stride = 0;
    for (std::uintptr_t s : {std::uintptr_t{0x10}, std::uintptr_t{0x20}}) {
        bool ok = true;
        for (std::uint32_t i = 0; ok && i < nparts; ++i)
            ok = mem::DerivesFrom(mem::RttiOf(Q(parts + i * s)), "MeshResourceBase") && Rd(parts + i * s + 8, d[i]);
        if (ok && exo::FoldLodDistancesOk(d, nparts)) {
            stride = s;
            break;
        }
    }
    if (!stride) return std::snprintf(why, cap, "her LOD list's distances do not read (%.2f %.2f)", static_cast<double>(d[0]), static_cast<double>(d[1])), nullptr;
    r.lod1 = d[1];
    r.lod0 = Q(parts);
    if (!Is(r.lod0, "MultiMeshResource")) return std::snprintf(why, cap, "her LOD0 is a %s, not a MultiMeshResource", TypeOf(r.lod0)), nullptr;
    if (!mem::Read(r.lod0 + 8, r.lod0_guid, 16)) return std::snprintf(why, cap, "her LOD0's GUID does not read"), nullptr;
    std::uint32_t nm = 0;
    std::uintptr_t meshes = 0;
    if (!ArrayAt(r.lod0 + kMultiMeshes, &nm, &meshes) || nm != 4) return std::snprintf(why, cap, "her LOD0 does not list 4 meshes at +0x80"), nullptr;
    for (std::uint32_t i = 0; i < nm && !r.d; ++i) {
        const std::uintptr_t m = Q(meshes + 8ull * i);
        if (GuidIs(m, kMeshDGuid)) r.d = m;
    }
    if (!r.d) return std::snprintf(why, cap, "no mesh 7f533fe6 (her folded hood) among her LOD0's meshes"), nullptr;
    float mx = 0;
    Rd(lod + kLodMax, mx);
    std::uint16_t hier = 0;
    Rd(lod + kLodHier, hier);
    Log("[hood] fold probe: her LodMeshResource %p (LOD distances %.2f %.2f, MaxDistance %.2f, +0x72 %04X, merged bindings %p %s), LOD0 %p, mesh D %p (%s, its own bindings %p %s)",
        reinterpret_cast<void*>(lod), static_cast<double>(d[0]), static_cast<double>(d[1]), static_cast<double>(mx), hier, reinterpret_cast<void*>(Q(lod + kLodMerged)),
        TypeOf(Q(lod + kLodMerged)), reinterpret_cast<void*>(r.lod0), reinterpret_cast<void*>(r.d), TypeOf(r.d), reinterpret_cast<void*>(Q(r.d + kMeshBinds)),
        TypeOf(Q(r.d + kMeshBinds)));
    for (Res& slot : g.res)
        if (!slot.lod) {
            slot = r;
            return &slot;
        }
    g.res[1] = g.res[0];
    g.res[0] = r;
    return &g.res[0];
}

template <class T>
T At(std::uint32_t o) {
    T v;
    std::memcpy(&v, g_buf + o, sizeof(T));
    return v;
}

enum class W { ok, soft, hard };

// One instance tree: root her LodMesh, its first child her LOD0, mesh D's node among LOD0's children (by its resource's
// GUID: the drawn node's resource, whatever the MultiMesh array holds). Everything bounds-checked in the tree's buffer.
W WalkTree(std::uintptr_t holder, const Res& r, const TreeState* had, TreeRead* t, char* why, std::size_t cap, bool probe, const char* who) {
    *t = TreeRead{};
    t->holder = holder;
    t->tree = Q(holder);
    Rd(holder + 8, t->hflags);
    if (!t->tree) return std::snprintf(why, cap, "no instance tree yet"), W::soft;
    if (!Rd(t->tree, t->used) || t->used == 0) return std::snprintf(why, cap, "the instance tree is empty"), W::soft;
    if (t->used < 0xB0 || t->used > sizeof(g_buf)) return std::snprintf(why, cap, "the instance tree's size %u is not a body's", t->used), W::hard;
    if (!mem::Read(t->tree + kTreeNodes, g_buf, t->used)) return std::snprintf(why, cap, "the instance tree does not read"), W::soft;
    const std::uintptr_t base = t->tree + kTreeNodes;
    const std::uint32_t used = t->used;
    const auto in = [&](std::int64_t o, std::uint32_t n) { return o >= 0 && (o & 3) == 0 && static_cast<std::uint64_t>(o) + n <= used; };
    const auto line = [&](std::uint32_t o, const char* what) {
        if (!probe) return;
        const std::uintptr_t res = At<std::uint64_t>(o);
        std::uint8_t gd[4] = {};
        mem::Read(res + 8, gd, 4);
        Log("[hood] fold probe (%s): tree %p +%03X %s: type %u count %u res %p %s (GUID %02x%02x%02x%02x...)", who, reinterpret_cast<void*>(t->tree), o, what, g_buf[o + 0xA],
            At<std::uint16_t>(o + 8), reinterpret_cast<void*>(res), TypeOf(res), gd[3], gd[2], gd[1], gd[0]);
    };
    // the root
    if (!in(0, 0x58)) return std::snprintf(why, cap, "the tree has no root"), W::hard;
    line(0, "root");
    t->root = base;
    t->t0 = At<float>(kNodeThresh);
    t->t1 = At<float>(kNodeThresh + 4);
    t->tmax = At<float>(kNodeThresh + 8);
    const float pin = had && had->f.pin ? had->f.pin_val : 0.0f;
    if (At<std::uint64_t>(0) != r.lod || !exo::FoldRootOk(g_buf[0xA], At<std::uint16_t>(8), t->t0, t->t1, r.lod1, pin))
        return std::snprintf(why, cap, "the tree's root is not her LodMesh (type %u, count %u, res %p, thresholds %.2f %.2f)", g_buf[0xA], At<std::uint16_t>(8),
                             reinterpret_cast<void*>(At<std::uint64_t>(0)), static_cast<double>(t->t0), static_cast<double>(t->t1)),
               W::hard;
    // LOD0
    const std::int32_t c = At<std::int32_t>(0x18);
    const std::int64_t o0 = 0x18 + static_cast<std::int64_t>(c);
    if (!c || !in(o0, 0x20)) return std::snprintf(why, cap, "her LodMesh node has no first child"), W::hard;
    const std::uint32_t l0 = static_cast<std::uint32_t>(o0);
    line(l0, "LOD0");
    if (!exo::FoldLod0Ok(g_buf[l0 + 0xA], At<std::uint16_t>(l0 + 8)) || !GuidIs(At<std::uint64_t>(l0), r.lod0_guid))
        return std::snprintf(why, cap, "her LodMesh node's first child is not her LOD0 (type %u, count %u)", g_buf[l0 + 0xA], At<std::uint16_t>(l0 + 8)), W::hard;
    t->lod0 = base + l0;
    // its children: mesh D's node
    std::int32_t cc = At<std::int32_t>(l0 + 0x18);
    std::int64_t o = cc ? static_cast<std::int64_t>(l0) + 0x18 + cc : -1;
    std::int64_t dn = -1;
    for (int k = 0; k < 4 && in(o, 0x20); ++k) {
        const std::uint32_t u = static_cast<std::uint32_t>(o);
        char what[16];
        std::snprintf(what, sizeof(what), "mesh %c", 'A' + k);
        line(u, what);
        if (dn < 0 && GuidIs(At<std::uint64_t>(u), kMeshDGuid)) dn = o;
        const std::int32_t s = At<std::int32_t>(u + 0x10);
        if (!s) break;
        o = static_cast<std::int64_t>(u) + 0x10 + s;
    }
    if (dn < 0) return std::snprintf(why, cap, "no node of mesh 7f533fe6 under her LOD0"), W::hard;
    const std::uint32_t d = static_cast<std::uint32_t>(dn);
    if (!in(d, kDNodeSize) || !exo::FoldDNodeOk(g_buf[d + 0xA], At<std::uint16_t>(d + 8)))
        return std::snprintf(why, cap, "mesh D's node is not a skinned node of one primitive (type %u, count %u)", g_buf[d + 0xA], At<std::uint16_t>(d + 8)), W::hard;
    t->dnode = base + d;
    t->dres = At<std::uint64_t>(d);
    t->rec = At<std::uint64_t>(d + kSlotRec);
    t->mask = g_buf[d + kSlotMask];
    t->flags = g_buf[d + kSlotFlags];
    t->slot = At<std::uint64_t>(d + kNodeBinds);
    t->mode = At<std::uint32_t>(d + kNodeMode);
    return W::ok;
}

// The alternate holder (entry+0x110's skinned instance, the drawable's own tree at drawable+0x44) holds a tree of type-9
// nodes, each referring to another holder (0x1417CE920: node+0 is that holder). The holder among them whose tree's root is
// her LodMesh (0 none); logged in the probe (which one, and whether it is the entry's own).
std::uintptr_t RefHolder(std::uintptr_t holder, std::uintptr_t lod, std::uintptr_t own, bool probe, const char* who) {
    const std::uintptr_t tree = holder ? Q(holder) : 0;
    std::uint32_t used = 0;
    if (!tree || !Rd(tree, used) || used < 0x18 || used > 0x4000) return 0;
    std::uint8_t b[0x400] = {};
    const std::uint32_t n = used < sizeof(b) ? used : static_cast<std::uint32_t>(sizeof(b));
    if (!mem::Read(tree + kTreeNodes, b, n)) return 0;
    std::uintptr_t found = 0;
    std::int64_t o = 0;
    for (int k = 0; k < 8 && o >= 0 && o + 0x18 <= n; ++k) {
        const std::uint32_t u = static_cast<std::uint32_t>(o);
        std::uintptr_t res = 0;
        std::memcpy(&res, b + u, 8);
        const unsigned type = b[u + 0xA];
        const std::uintptr_t rt = type == 9 && res ? Q(res) : 0, rres = rt ? Q(rt + kTreeNodes) : 0;
        std::uint8_t rtype = 0;
        if (rt) Rd(rt + kTreeNodes + 0xA, rtype);
        if (probe)
            Log("[hood] fold probe (%s): the alternate tree %p +%03X: type %u res %p%s (its tree %p, root type %u res %p%s)", who, reinterpret_cast<void*>(tree), u, type,
                reinterpret_cast<void*>(res), res == own ? " = the entry's own holder" : "", reinterpret_cast<void*>(rt), rtype, reinterpret_cast<void*>(rres),
                rres == lod ? " her LodMesh" : "");
        if (type == 9 && rt && rtype == 3 && rres == lod && !found) found = res;
        std::int32_t sib = 0;
        std::memcpy(&sib, b + u + 0x10, 4);
        if (!sib) break;
        o = static_cast<std::int64_t>(u) + 0x10 + sib;
    }
    return found;
}

// A body (entity) down to its instance trees. Soft: not ready (no drawable, a rebuild pending) or not her suit's drawable
// yet (a look or body swap under way: no ModelRep, no LodMeshResource); hard: her LodMeshResource or tree not as checked.
W WalkBody(std::uintptr_t e, const Body* had, BodyRead* rd, const Res** rr, char* why, std::size_t cap, bool probe, const char* who) {
    *rd = BodyRead{};
    const std::uintptr_t a = e ? Q(e + 0xB8) : 0, sr = a ? Q(a + 0x40) : 0;
    rd->dr = sr ? Q(sr + 0x28) : 0;
    rd->rep = rd->dr ? Q(rd->dr + kDrMeshes) : 0;
    if (!rd->rep) return std::snprintf(why, cap, "the body has no drawable yet"), W::soft;
    // (her body's is a SkinnedModelRep: a ModelRep with the same entries)
    if (!mem::DerivesFrom(mem::RttiOf(rd->rep), "ModelRep"))
        return std::snprintf(why, cap, "the drawable's +0x98 is a %s, not a ModelRep", TypeOf(rd->rep)), W::soft;
    std::int32_t n = 0;
    const std::uintptr_t arr = Q(rd->rep + kRepEntries);
    if (!Rd(rd->rep + kRepCount, n) || n < 1 || n > 64 || !arr) return std::snprintf(why, cap, "the ModelRep has no entries"), W::soft;
    for (std::int32_t i = 0; i < n && !rd->entry; ++i) {
        const std::uintptr_t en = arr + static_cast<std::uintptr_t>(i) * kMeshEntry, lod = Q(en + kEntryLod);
        bool known = false;
        for (const Res& r : g.res) known = known || (r.lod && r.lod == lod);
        if (known || Is(lod, "LodMeshResource")) rd->entry = en, rd->lod = lod;
    }
    if (!rd->entry) return std::snprintf(why, cap, "no entry of the ModelRep draws a LodMeshResource"), W::soft;
    const Res* r = ResOf(rd->lod, why, cap);
    if (!r) return W::hard;
    *rr = r;
    Rd(rd->entry + kEntryBuild, rd->build);
    Rd(rd->entry + kEntryMask, rd->show);
    rd->alt = Q(rd->entry + kEntryAlt);
    Rd(rd->dr + kDrLodScale, rd->scale);
    if (rd->build == -1) return std::snprintf(why, cap, "her instance tree is being rebuilt"), W::soft;
    if (probe)
        Log("[hood] fold probe (%s): drawable %p (LodDistanceScale %.3f), ModelRep %p (%d entries), entry %p: +0x108 %d, +0x10C %02X, +0x110 %p %s (its holder %p, start %d), holder +0xF0 tree %p flags %08X",
            who, reinterpret_cast<void*>(rd->dr), static_cast<double>(rd->scale), reinterpret_cast<void*>(rd->rep), n, reinterpret_cast<void*>(rd->entry), rd->build,
            rd->show, reinterpret_cast<void*>(rd->alt), rd->alt ? TypeOf(rd->alt) : "-", reinterpret_cast<void*>(rd->alt ? Q(rd->alt) : 0),
            rd->alt ? static_cast<int>(static_cast<std::int32_t>(Q(rd->alt + 8))) : 0, reinterpret_cast<void*>(Q(rd->entry + kEntryHolder)),
            static_cast<unsigned>(Q(rd->entry + kEntryHolder + 8)));
    // the entry's own tree, and the one the alternate holder's tree refers to (its type-9 nodes) when that is another one
    std::uintptr_t holders[2] = {rd->entry + kEntryHolder, 0};
    if (rd->alt) {
        const std::uintptr_t h = RefHolder(Q(rd->alt), r->lod, rd->entry + kEntryHolder, probe, who);
        if (h && h != holders[0] && Q(h) != Q(holders[0])) holders[1] = h;
    }
    W first = W::soft;
    char w2[200] = "";
    for (int k = 0; k < 2; ++k) {
        if (!holders[k]) continue;
        const TreeState* ts = nullptr;
        const std::uintptr_t tr = Q(holders[k]);
        for (int j = 0; had && j < 2; ++j)
            if (had->t[j].tree && had->t[j].tree == tr) ts = &had->t[j];
        TreeRead t;
        const W w = WalkTree(holders[k], *r, ts, &t, k == 0 ? why : w2, k == 0 ? cap : sizeof(w2), probe, who);
        if (k == 0) first = w;
        if (w == W::ok) rd->t[rd->nt++] = t;
        else if (k == 1 && probe)
            Log("[hood] fold probe (%s): the alternate holder's tree is not used (%s)", who, w2);
    }
    if (!rd->nt) return first;
    if (probe)
        for (int k = 0; k < rd->nt; ++k) {
            const TreeRead& t = rd->t[k];
            const std::uintptr_t merged = Q(rd->lod + kLodMerged), own = Q(r->d + kMeshBinds);
            const exo::FoldSlot sk = exo::FoldSlotOf(t.slot, merged, own, 0);
            std::uint32_t nj = 0;
            std::uintptr_t jl = 0;
            ArrayAt(t.slot + kBindJoints, &nj, &jl);
            std::uint32_t sgn = 0;
            std::uintptr_t sgd = 0;
            ArrayAt(t.dres + 0xF8, &sgn, &sgd);
            Log("[hood] fold probe (%s): mesh D node %p: res %p %s (%s the MultiMesh's %p %s), slot mask %02X flags %02X record %p (+0x58 %p, ShadingGroup[0] %p), bindings %p %s (%s, %u joints), skin mode %u, LOD thresholds %.2f %.2f %.2f",
                who, reinterpret_cast<void*>(t.dnode), reinterpret_cast<void*>(t.dres), TypeOf(t.dres), t.dres == r->d ? "the same as" : "NOT", reinterpret_cast<void*>(r->d),
                TypeOf(r->d), t.mask, t.flags, reinterpret_cast<void*>(t.rec), reinterpret_cast<void*>(t.rec ? Q(t.rec + 0x58) : 0), reinterpret_cast<void*>(sgd ? Q(sgd) : 0),
                reinterpret_cast<void*>(t.slot), TypeOf(t.slot),
                sk == exo::FoldSlot::merged ? "her LodMesh's merged" : sk == exo::FoldSlot::own ? "mesh D's own" : "the model's", nj, t.mode, static_cast<double>(t.t0),
                static_cast<double>(t.t1), static_cast<double>(t.tmax));
        }
    return W::ok;
}

// Method 2: a private copy of the merged bindings with the roll joints' inverse binds zeroed (0: refused, logged).
std::uintptr_t PrivateBinds(std::uintptr_t merged) {
    for (const Priv& p : g.priv)
        if (p.merged == merged) return p.ours;
    Priv* slot = nullptr;
    for (Priv& p : g.priv)
        if (!p.merged) {
            slot = &p;
            break;
        }
    if (!slot) return 0;
    char why[160] = "";
    std::uint32_t nj = 0, nm = 0;
    std::uintptr_t jl = 0, ml = 0;
    std::uint16_t joints[1024];
    if (Q(merged) != mem::Base() + kBindVtable || !Is(merged, "SkinnedMeshIndexedJointBindings"))
        std::snprintf(why, sizeof(why), "it is a %s", TypeOf(merged));
    else if (!ArrayAt(merged + kBindJoints, &nj, &jl) || !ArrayAt(merged + kBindMatrices, &nm, &ml) || nj != nm || nj > 1024)
        std::snprintf(why, sizeof(why), "its joints (%u) and inverse binds (%u) do not read", nj, nm);
    else if (!mem::Read(jl, joints, nj * 2ull) || !exo::FoldJointsCover(joints, nj))
        std::snprintf(why, sizeof(why), "its %u joints do not hold mesh D's 22", nj);
    if (why[0]) {
        Log("[hood] her folded hood: the merged bindings %p are not copied (%s)", reinterpret_cast<void*>(merged), why);
        slot->merged = merged;  // (not tried again)
        return 0;
    }
    const std::size_t list = (nj * 2ull + 15) & ~std::size_t{15}, size = kBindSize + 16 + list + nj * kMat;
    auto* blk = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!blk) return 0;
    std::uint8_t* obj = blk;
    std::uint8_t* lst = blk + kBindSize + 16;
    std::uint8_t* mats = lst + list;
    if (!mem::Read(merged, obj, kBindSize) || !mem::Read(ml, mats, nj * kMat)) {
        VirtualFree(blk, 0, MEM_RELEASE);
        return 0;
    }
    std::memcpy(lst, joints, nj * 2ull);
    unsigned zeroed = 0;
    for (unsigned i = 0; i < nj; ++i)
        if (exo::FoldRollJoint(joints[i])) std::memset(mats + i * kMat, 0, kMat), ++zeroed;
    const std::uint32_t cnt[2] = {nj, nj};
    const auto lp = reinterpret_cast<std::uintptr_t>(lst), mp = reinterpret_cast<std::uintptr_t>(mats);
    std::memcpy(obj + kBindJoints, cnt, 8);
    std::memcpy(obj + kBindJoints + 8, &lp, 8);
    std::memcpy(obj + kBindMatrices, cnt, 8);
    std::memcpy(obj + kBindMatrices + 8, &mp, 8);
    obj[kBindHash] ^= 0x5A;  // no hash-keyed cache takes it for the shared one
    std::uint32_t rc[2] = {};
    std::memcpy(rc, obj + 0x18, 8);
    slot->merged = merged;
    slot->ours = reinterpret_cast<std::uintptr_t>(obj);
    Log("[hood] her folded hood: private bindings %p made from her merged bindings %p (%u joints, %u roll joints' inverse binds zeroed; +0x18 %08X %08X)", obj,
        reinterpret_cast<void*>(merged), nj, zeroed, rc[0], rc[1]);
    return slot->ours;
}

TreeState* StateFor(Body& b, const BodyRead& rd, const TreeRead& t, std::uintptr_t lod) {
    for (TreeState& s : b.t)
        if (s.tree == t.tree && s.dnode == t.dnode) return &s;
    for (TreeState& s : b.t)
        if (!s.tree) {
            s = TreeState{};
            s.tree = t.tree, s.dnode = t.dnode, s.holder = t.holder, s.entry = rd.entry, s.lod = lod;
            return &s;
        }
    return nullptr;
}
bool Mine(const TreeState& s) { return s.f.mask || s.f.pin || s.binds; }
bool Ours(const Body& b) {
    if (b.scale) return true;
    for (const TreeState& s : b.t)
        if (Mine(s)) return true;
    return false;
}
game::Entity* E(std::uintptr_t e) { return reinterpret_cast<game::Entity*>(e); }
// The body's entity as it was when taken (its vtable and GUID still read so): only then is anything of it walked or
// written back (an entity the game destroyed takes its drawable and trees with it).
bool Alive(const Body& b) {
    std::uintptr_t vt = 0;
    game::Guid gd;
    return b.e && b.id && partner::ReadVtable(E(b.e), &vt) && vt == b.vt && partner::ReadGuid(E(b.e), &gd) && gd == b.guid;
}
std::uintptr_t DrawableOf(std::uintptr_t e) {
    const std::uintptr_t a = e ? Q(e + 0xB8) : 0, sr = a ? Q(a + 0x40) : 0;
    return sr ? Q(sr + 0x28) : 0;
}

void Detail(const char* who, const char* fmt, ...) {
    if (g.logs >= 400) return;
    ++g.logs;
    char m[300];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
    Log("[hood] fold %s: %s", who, m);
}

// One tree's step (exo::FoldStepOf): the mask and the pin, each a compare-and-swap from the value just read.
void Step(TreeState& s, const exo::FoldPlan& plan, std::uint8_t cur, std::uint8_t show, float t1, float lod1, float tmax, const char* who) {
    const exo::FoldStep st = exo::FoldStepOf(s.f, plan, cur, show, t1, lod1, tmax);
    bool mok = false, pok = false;
    if (st.mask >= 0) {
        mok = Cas8(s.dnode + kSlotMask, cur, static_cast<std::uint8_t>(st.mask));
        if (mok) {
            ++g.writes;
            std::uint8_t back = 0xEE;
            Rd(s.dnode + kSlotMask, back);
            Detail(who, "tree %p D node %p: mask %02X -> %02X (read back %02X; entry +0x10C %02X)", reinterpret_cast<void*>(s.tree), reinterpret_cast<void*>(s.dnode), cur,
                   st.mask, back, show);
        } else {
            ++g.cas_fails;
        }
    }
    if (st.pin) {
        const std::uintptr_t th = s.tree + kTreeNodes + kNodeThresh + 4;
        pok = CasF(th, st.pin_from, st.pin_to);
        if (pok) {
            ++g.writes;
            float back = 0;
            Rd(th, back);
            if (plan.pin == exo::FoldPin::node)
                Detail(who, "tree %p: LOD1 threshold %.2f -> %.2f (read back %.2f)", reinterpret_cast<void*>(s.tree), static_cast<double>(st.pin_from),
                       static_cast<double>(st.pin_to), static_cast<double>(back));
            else
                Detail(who, "tree %p: LOD1 threshold back to %.2f (read back %.2f)", reinterpret_cast<void*>(s.tree), static_cast<double>(st.pin_to), static_cast<double>(back));
        } else {
            ++g.cas_fails;
        }
    }
    s.f = exo::FoldStepDone(st, plan, mok, pok);
}

// Mesh D's node's bindings back to what they were while they are still ours (the private bindings, method 2).
void BindsBack(TreeState& s, std::uintptr_t slot, const char* who) {
    if (!s.binds) return;
    if (!s.binds_ours || slot != s.binds_ours) {
        s.binds = false;  // not ours any more
        return;
    }
    if (Cas64(s.dnode + kNodeBinds, slot, s.binds_was)) {
        ++g.writes;
        s.binds = false;
        Detail(who, "tree %p D node %p: bindings back to %p (read back %p)", reinterpret_cast<void*>(s.tree), reinterpret_cast<void*>(s.dnode),
               reinterpret_cast<void*>(s.binds_was), reinterpret_cast<void*>(Q(s.dnode + kNodeBinds)));
    } else {
        ++g.cas_fails;
    }
}

// A tree of ours the walk does not reach (the body's walk failed, or the alternate holder dropped it): written back only
// while its holder still holds it and it still reads as her tree (root her LodMesh of 2 parts, mesh D's node where it
// was). False: rebuilt or gone (nothing of ours left in it).
bool WriteBackHeld(TreeState& s, const char* who) {
    std::uintptr_t res = 0, dres = 0;
    std::uint8_t rtype = 0, dtype = 0, cur = 0, show = 0;
    std::uint16_t rcount = 0, dcount = 0;
    float t1 = 0;
    if (!s.holder || !s.tree || Q(s.holder) != s.tree) return false;
    if (!Rd(s.tree + kTreeNodes, res) || res != s.lod || !Rd(s.tree + kTreeNodes + 0xA, rtype) || !Rd(s.tree + kTreeNodes + 8, rcount) || rtype != 3 || rcount != 2)
        return false;
    if (!Rd(s.dnode, dres) || !GuidIs(dres, kMeshDGuid) || !Rd(s.dnode + 0xA, dtype) || !Rd(s.dnode + 8, dcount) || !exo::FoldDNodeOk(dtype, dcount)) return false;
    if (!Rd(s.dnode + kSlotMask, cur) || !Rd(s.entry + kEntryMask, show) || !Rd(s.tree + kTreeNodes + kNodeThresh + 4, t1)) return false;
    Step(s, exo::FoldPlan{}, cur, show, t1, 0.0f, 0.0f, who);
    BindsBack(s, Q(s.dnode + kNodeBinds), who);
    Detail(who, "tree %p not reached by the walk, still held: written back%s", reinterpret_cast<void*>(s.tree), Mine(s) ? " (a race: tried again)" : "");
    return true;
}

// Brings one body to `plan` (each part only what is ours to change; every write a compare-and-swap of what was read).
void Apply(Body& b, const BodyRead& rd, const Res& r, const exo::FoldPlan& plan, const char* who) {
    // trees of ours the walk no longer reaches: written back while their holder (same drawable) still holds them as her
    // trees; otherwise rebuilt (the game's values again) or gone with their drawable: forgotten
    bool rebuilt = false;
    for (TreeState& s : b.t) {
        if (!s.tree) continue;
        bool seen = false;
        for (int k = 0; k < rd.nt; ++k) seen = seen || (rd.t[k].tree == s.tree && rd.t[k].dnode == s.dnode);
        if (seen) continue;
        if (Mine(s) && b.dr == rd.dr && WriteBackHeld(s, who)) {
            if (!Mine(s)) s = TreeState{};
            continue;
        }
        if (Mine(s)) {
            Detail(who, "tree %p rebuilt or gone (nothing of ours to write back)", reinterpret_cast<void*>(s.tree));
            rebuilt = true;
        }
        s = TreeState{};
    }
    for (int k = 0; k < rd.nt; ++k) {
        const TreeRead& t = rd.t[k];
        TreeState* s = StateFor(b, rd, t, r.lod);
        if (!s) continue;
        // the mask and the LOD pin on the tree's LodMesh node
        Step(*s, plan, t.mask, rd.show, t.t1, r.lod1, t.tmax, who);
        // the private bindings
        const std::uintptr_t merged = Q(rd.lod + kLodMerged), own = Q(r.d + kMeshBinds);
        const exo::FoldSlot sk = exo::FoldSlotOf(t.slot, merged, own, s->binds_ours);
        if (plan.binds && sk != exo::FoldSlot::none && sk != exo::FoldSlot::ours) {  // (PrivateBinds checks the object)
            const std::uintptr_t ours = PrivateBinds(t.slot);
            if (ours && Cas64(t.dnode + kNodeBinds, t.slot, ours)) {
                ++g.writes;
                s->binds = true, s->binds_was = t.slot, s->binds_ours = ours;
                Detail(who, "tree %p D node %p: bindings %p -> %p (read back %p; skin mode %u)", reinterpret_cast<void*>(t.tree), reinterpret_cast<void*>(t.dnode),
                       reinterpret_cast<void*>(t.slot), reinterpret_cast<void*>(ours), reinterpret_cast<void*>(Q(t.dnode + kNodeBinds)), t.mode);
            } else if (ours) {
                ++g.cas_fails;
            }
        } else if (!plan.binds) {
            BindsBack(*s, t.slot, who);
        }
        if (rebuilt && (s->f.mask || s->binds))
            Log("[hood] her folded hood hidden again on the %s body's rebuilt tree %p", who, reinterpret_cast<void*>(t.tree)), rebuilt = false;
    }
    // the LOD pin on the drawable's LodDistanceScale (ours only on the same drawable, while it still holds our value)
    if (b.scale && (b.dr != rd.dr || rd.scale != exo::kFoldScalePin)) b.scale = false;
    if (plan.pin == exo::FoldPin::scale && !b.scale && rd.scale != exo::kFoldScalePin && rd.scale > 0.0f) {
        if (CasF(rd.dr + kDrLodScale, rd.scale, exo::kFoldScalePin)) {
            ++g.writes;
            b.scale = true, b.scale_was = rd.scale;
            Detail(who, "drawable %p: LodDistanceScale %.3f -> %.3f", reinterpret_cast<void*>(rd.dr), static_cast<double>(rd.scale), static_cast<double>(exo::kFoldScalePin));
        }
    } else if (plan.pin != exo::FoldPin::scale && b.scale) {
        if (CasF(rd.dr + kDrLodScale, rd.scale, b.scale_was)) {
            ++g.writes;
            Detail(who, "drawable %p: LodDistanceScale back to %.3f", reinterpret_cast<void*>(rd.dr), static_cast<double>(b.scale_was));
        }
        b.scale = false;
    }
    b.dr = rd.dr;
}

void Summary(Body& b, int who, const char* why) {
    bool mask = false, binds = false, pin = false;
    for (const TreeState& s : b.t) mask |= s.f.mask, binds |= s.binds, pin |= s.f.pin;
    char st[96];
    std::snprintf(st, sizeof(st), "%s%s%s%s", mask ? "the mask" : "", binds ? (mask ? ", the private bindings" : "the private bindings") : "",
                  pin ? (mask || binds ? ", LOD0 pinned" : "LOD0 pinned") : "", b.scale ? (mask || binds || pin ? ", LOD scale pinned" : "LOD scale pinned") : "");
    const bool hidden = mask || binds;
    const bool any = hidden || pin || b.scale;
    if (std::strcmp(st, b.state) == 0 && hidden == b.hidden) return;
    if (hidden && !b.hidden) ++g.hides;
    if (!any && b.state[0] && std::strcmp(b.state, "none") != 0) ++g.shows;
    if (any) Log("[hood] her folded hood %s on the %s body (%s; method %d)", hidden ? "hidden" : "not hidden", kWho[who], st, g.method);
    else if (std::strcmp(b.state, "none") != 0) Log("[hood] her folded hood shown again on the %s body (everything written back; %s)", kWho[who], why);
    std::snprintf(b.state, sizeof(b.state), "%s", any ? st : "none");
    b.hidden = hidden;
}

// Everything of ours on one body written back: through the walk while the body reads as checked; otherwise each tree of
// ours while its holder still holds it as her tree, the scale while the body still draws that drawable (an entity the
// game destroyed: nothing of it is touched, its trees went with it). What a race left is tried again (Tick).
void RestoreBody(int i, const char* why) {
    Body& b = g.body[i];
    if (!b.e || !Ours(b)) return;
    BodyRead rd;
    const Res* r = nullptr;
    char w[200] = "the body is gone";
    const bool alive = Alive(b);
    const W res = alive ? WalkBody(b.e, &b, &rd, &r, w, sizeof(w), false, kWho[i]) : W::soft;
    if (res == W::ok && r) {
        Apply(b, rd, *r, exo::FoldPlan{}, kWho[i]);
    } else {
        for (TreeState& s : b.t) {
            if (!s.tree) continue;
            if (alive && Mine(s) && WriteBackHeld(s, kWho[i]) && Mine(s)) continue;  // a race: tried again
            if (Mine(s)) Detail(kWho[i], "tree %p: nothing of ours to write back (%s)", reinterpret_cast<void*>(s.tree), w);
            s = TreeState{};
        }
        if (b.scale) {
            float sc = 0;
            if (alive && b.dr && DrawableOf(b.e) == b.dr && Rd(b.dr + kDrLodScale, sc) && sc == exo::kFoldScalePin && CasF(b.dr + kDrLodScale, sc, b.scale_was)) {
                ++g.writes;
                Detail(kWho[i], "drawable %p: LodDistanceScale back to %.3f", reinterpret_cast<void*>(b.dr), static_cast<double>(b.scale_was));
            }
            b.scale = false;
        }
    }
    Summary(b, i, why);
}

void Off(const char* fmt, const char* what) {
    if (g.off) return;
    g.off = true;
    std::snprintf(g.why, sizeof(g.why), fmt, what);
    Log("[hood] her folded hood is not hidden this session: %s", g.why);
    for (int i = 0; i < 2; ++i) RestoreBody(i, "the hiding went off");
}

}  // namespace

void Configure(bool on) noexcept {
    g.cfg = on;
    Log("[hood] her folded hood hidden while her raised hood shows: %s (session.ini hood_fold_hide)", on ? "on" : "off");
}
bool On() noexcept { return g.cfg && !g.off; }

namespace {
// Why a body's hide ended (the shown-again line): the setting, its raised hood gone, or none raised (a test method).
const char* ShowReason(const Body& b, bool w) {
    if (w) return "the method hides nothing";
    if (!On()) return "the setting is off";
    return b.hood_seen ? "the raised hood is gone" : "no raised hood";
}
}  // namespace

void SetTest(int method, int pin) noexcept {
    g.method = method < 0 ? exo::kFoldProductMethod : method > exo::kFoldForceBinds ? exo::kFoldProductMethod : method;
    g.pin = method < 0 ? 0 : pin < 0 || pin > 2 ? 0 : pin;
    static const char* kNames[] = {"never hidden", "the mask", "the private bindings", "the LOD pin only", "the mask, the raised hood or not",
                                    "the private bindings, the raised hood or not"};
    Log("[hood] TEST: her folded hood's method %d (%s), LOD pin %s", g.method, kNames[g.method], g.pin == 1 ? "off" : g.pin == 2 ? "the LOD scale" : "the tree's threshold");
}

void Tick(std::uintptr_t partner_body, bool partner_hide, std::uintptr_t self_body, bool self_hide, double now_ms) noexcept {
    const std::uintptr_t es[2] = {partner_body, self_body};
    const bool want[2] = {partner_hide, self_hide};
    for (int i = 0; i < 2; ++i) {
        Body& b = g.body[i];
        // another entity (or none): what was ours on the old one written back first (only while it is still alive and
        // drawn as it was; BodyGoing already did it for a body realbody removes)
        if (b.e != es[i]) {
            if (Ours(b)) RestoreBody(i, "the body went");
            b = Body{};
            b.e = es[i];
        }
        if (!b.e) continue;
        if (!b.id) b.id = partner::ReadVtable(E(b.e), &b.vt) && partner::ReadGuid(E(b.e), &b.guid);
        // the hiding off for the session: what a race left in the one-shot write-back tried again
        if (g.off) {
            if (Ours(b) && now_ms >= b.next_try) {
                b.next_try = now_ms + exo::kFoldFindRetryMs;
                RestoreBody(i, "the hiding went off");
            }
            continue;
        }
        if (want[i]) b.hood_seen = true;
        const bool w = (want[i] || g.method >= exo::kFoldForce) && On();
        if (!w && !Ours(b)) {
            if (std::strcmp(b.state, "none") != 0) Summary(b, i, ShowReason(b, w));
            if (!want[i]) b.hood_seen = false;
            continue;
        }
        if (w && !CodeOk()) return;
        if (now_ms < b.next_try) continue;
        BodyRead rd;
        const Res* r = nullptr;
        char why[200] = "";
        const W res = WalkBody(b.e, &b, &rd, &r, why, sizeof(why), w && !b.probed, kWho[i]);
        if (res != W::ok) {
            if (res == W::hard) {
                // counted afresh on another drawable or LodMeshResource (a look or body swap under way)
                if (rd.dr != b.fail_dr || rd.lod != b.fail_lod) b.hard = 0, b.fail_dr = rd.dr, b.fail_lod = rd.lod;
                b.next_try = now_ms + exo::kFoldFindRetryMs;
                if (++b.hard >= exo::kFoldFindTries) {
                    char m[260];
                    std::snprintf(m, sizeof(m), "the %s body: %s", kWho[i], why);
                    Off("%s", m);
                    return;
                }
                Log("[hood] her folded hood not found yet on the %s body (%s): trying again in 1 s (%u of %u)", kWho[i], why, b.hard, exo::kFoldFindTries);
            } else if (b.soft_logs < 5) {
                ++b.soft_logs;
                Detail(kWho[i], "waiting (%s)", why);
            }
            continue;
        }
        b.hard = 0;
        b.probed = true;
        const exo::FoldPlan plan = exo::FoldPlanOf(w, g.method, g.pin);
        Apply(b, rd, *r, plan, kWho[i]);
        Summary(b, i, ShowReason(b, w));
        if (!want[i] && !Ours(b)) b.hood_seen = false;
    }
}

void BodyGoing(std::uintptr_t body) noexcept {
    if (!body) return;
    for (int i = 0; i < 2; ++i) {
        Body& b = g.body[i];
        if (b.e != body) continue;
        if (Ours(b)) RestoreBody(i, "the body went");
        // its trees go with it: nothing of ours is left to write back later (the next tick only forgets it)
        for (TreeState& s : b.t) s = TreeState{};
        b.scale = false;
        Summary(b, i, "the body went");
    }
}

void RestoreAll(const char* why) noexcept {
    for (int i = 0; i < 2; ++i) RestoreBody(i, why);
}

const char* Describe(char* buf, std::size_t cap) noexcept {
    std::snprintf(buf, cap, "folded hood %s: method %d, pin %d; partner %s, self %s; %u hidden, %u shown again, %u writes, %u raced%s%s", g.cfg ? (g.off ? "OFF" : "on") : "off (setting)",
                  g.method, g.pin, g.body[0].state, g.body[1].state, g.hides, g.shows, g.writes, g.cas_fails, g.off ? ": " : "", g.off ? g.why : "");
    return buf;
}

}  // namespace st::hoodfold
