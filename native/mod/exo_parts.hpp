// Sam's skeletons (exoskeletons) on a body that is not Sam (docs/real-partner-body.md section 32), the pure part: what the
// game's mesh controller shows of Sam's porter suit, the skeleton types, and which ArtParts the "exo carrier" (a
// demo_sam_clean body that shows only the skeleton pieces, posed from her body) shows. No game memory, no Windows:
// unit-tested in tests/exo_parts_tests.cpp.
//
// A skeleton is not an entity of its own: it is a set of ArtParts of Sam's porter suit (part_sam_body_suiti, meshes from
// sam_sam/core/sam_exos_def), shown and hidden by his mesh controller (player +0xA60) through its per-part masks. The
// exos ArtPartsVariation (0..43) only tints them.
//
// ApplyMeshControl 0x1426D9120, for k = 1..88 (EDSPlayerMesh; the controller maps k to an ArtParts index at +0x38 + 8k,
// -1 none) [V: disassembly, byte-checked by partner_gear.cpp]:
//   asked(k) = root(+0x338) == 0 && mask[0](+0x348) == 0 &&
//              (b[k](+0x8D8 + 8k) != 0 || (c[k](+0xE68 + 8k) == 0 &&
//               (cmpmode[k](byte +0x19A0 + k) ? mask[k](+0x348 + 8k) != cmpval(+0x1998) : mask[k] == 0)))
// b / c are the forced show / hide of reason 29 (SetPartVisible 0x1426B4D80). The self view hides Sam with bit 63 of
// mask[0] (real_body.cpp kOurReason): the sender reads the formula without that bit, so what the game wants of his
// skeleton is known while he is hidden.
//
// The skeleton types (FUN_142805CE0, the inventory update: SetPartVisible reason 5 by the worn skeleton's item kind,
// 0x14294EBE0 / 0x14294E730 / 0x14294EB20 / 0x14294ECA0) [V: decompile]:
//   1 speed        ExoMain 32, ExoSub 38, Speed 39, BatteryEmission 41   (the 100 % save's skeleton)
//   2 all-terrain  ExoMain 32, ExoSub 38, Pile 40, BatteryEmission 41
//   3 power        ExoMain 32, Heavy 42, EmissionHeavy 43
//   4 support      Support 86, EmissionSupport 87
//   none: all of them hidden.
//
// The porter suit's ArtParts (runtime index = decoded index + 1; 0 the suit body mesh, top-level; 1 a container without a
// mesh, top-level, the parent of the suit pieces and of the skeleton; 52 the head container, top-level):
//   k 32 -> part 32 mesh_exomain (parent 1; children 33-37, 39)   k 41 -> 33 batteryemission   k 38 -> 34 exosub
//   k 39 -> 35 speed   k 40 -> 36 pile   k 42 -> 37 heavy (child 38 heavysub, no k: follows it)   k 43 -> 39 emissionheavy
//   k 86 -> 50 support (parent 1; child 51)   k 87 -> 51 emissionsupport
//   k 44 -> 40 legin (+41 lamp, k 46)   k 45 -> 42 legout (+43 lamp, k 47): suit pieces, LegOut worn with a skeleton
#pragma once
#include <cstdint>

namespace st::exo {

// ---- the mesh controller's formula (one k > 0)
struct MeshCtl {
    std::uint64_t root = 0;    // +0x338
    std::uint64_t mask0 = 0;   // +0x348 (the global part mask)
    std::uint64_t b = 0;       // +0x8D8 + 8k (forced show)
    std::uint64_t c = 0;       // +0xE68 + 8k (forced hide)
    std::uint64_t mask = 0;    // +0x348 + 8k
    std::uint64_t cmpval = 0;  // +0x1998
    bool cmpmode = false;      // byte +0x19A0 + k
};
inline constexpr std::uint64_t kSelfViewBit = 1ull << 63;  // real_body.cpp kOurReason in the global mask
// What the game asks of part k, our own self view's bit ignored.
inline bool MeshAsked(const MeshCtl& m, std::uint64_t ignore_mask0 = kSelfViewBit) {
    if (m.root != 0 || (m.mask0 & ~ignore_mask0) != 0) return false;
    if (m.b != 0) return true;
    if (m.c != 0) return false;
    return m.cmpmode ? m.mask != m.cmpval : m.mask == 0;
}

// ---- the skeleton pieces
inline constexpr unsigned kExoMeshCount = 9;
// EDSPlayerMesh k of each piece, and the porter suit's ArtParts index for it (the same order).
inline constexpr std::uint8_t kExoMesh[kExoMeshCount] = {32, 41, 38, 39, 40, 42, 43, 86, 87};
inline constexpr std::uint8_t kExoPart[kExoMeshCount] = {32, 33, 34, 35, 36, 37, 39, 50, 51};
inline constexpr std::uint8_t kHeavySubPart = 38;  // no k: a child of heavy (37), shown with it
inline constexpr std::uint8_t kLegOutPart = 42, kLegOutLampPart = 43;  // suit pieces (k 45 / 47)
inline constexpr std::uint8_t kLegOutMesh = 45, kLegOutLampMesh = 47;
inline constexpr std::uint8_t kExoMainPart = 32, kSupportPart = 50, kContainerPart = 1, kHeadPart = 52, kSuitBodyPart = 0;
inline constexpr int kSuitParts = 107;
inline constexpr unsigned kMaxParts = 128;  // wire::kMaxGearParts

inline bool Bit(const std::uint64_t* w, unsigned i) { return i < kMaxParts && ((w[i >> 6] >> (i & 63)) & 1) != 0; }
inline void SetBit(std::uint64_t* w, unsigned i, bool on) {
    if (i >= kMaxParts) return;
    if (on) w[i >> 6] |= 1ull << (i & 63);
    else w[i >> 6] &= ~(1ull << (i & 63));
}

// The skeleton types (k sets above). 0 none.
enum class Type : std::uint8_t { none = 0, speed = 1, all_terrain = 2, power = 3, support = 4 };
inline constexpr int kTypeCount = 5;
inline const char* TypeName(Type t) {
    switch (t) {
        case Type::none: return "none";
        case Type::speed: return "speed";
        case Type::all_terrain: return "all-terrain";
        case Type::power: return "power";
        case Type::support: return "support";
    }
    return "?";
}
// Is mesh k shown with this type (k one of kExoMesh)?
inline bool TypeShows(Type t, std::uint8_t k) {
    switch (t) {
        case Type::none: return false;
        case Type::speed: return k == 32 || k == 38 || k == 39 || k == 41;
        case Type::all_terrain: return k == 32 || k == 38 || k == 40 || k == 41;
        case Type::power: return k == 32 || k == 42 || k == 43;
        case Type::support: return k == 86 || k == 87;
    }
    return false;
}
// The porter suit's ArtParts bits a type shows (the skeleton pieces only).
inline void TypeParts(Type t, std::uint64_t (&out)[2]) {
    out[0] = out[1] = 0;
    for (unsigned i = 0; i < kExoMeshCount; ++i)
        if (TypeShows(t, kExoMesh[i])) SetBit(out, kExoPart[i], true);
}
// Which type a set of shown skeleton pieces is (none when nothing; -1 when it is no known type).
inline int TypeOfParts(const std::uint64_t* vis) {
    for (int t = 0; t < kTypeCount; ++t) {
        std::uint64_t p[2];
        TypeParts(static_cast<Type>(t), p);
        bool same = true;
        for (unsigned i = 0; i < kExoMeshCount && same; ++i) same = Bit(p, kExoPart[i]) == Bit(vis, kExoPart[i]);
        if (same) return t;
    }
    return -1;
}

// The bits of every skeleton piece (the receiver's "apply" check: a sender in the porter suit names all of them).
inline void ExoMask(std::uint64_t (&out)[2]) {
    out[0] = out[1] = 0;
    for (unsigned i = 0; i < kExoMeshCount; ++i) SetBit(out, kExoPart[i], true);
}
// A sender's parts block names every skeleton piece (it read them on a porter suit with this map) and shows at least one.
inline bool SenderNamesExo(const std::uint64_t* apply) {
    for (unsigned i = 0; i < kExoMeshCount; ++i)
        if (!Bit(apply, kExoPart[i])) return false;
    return true;
}
inline bool AnyExoShown(const std::uint64_t* apply, const std::uint64_t* vis) {
    for (unsigned i = 0; i < kExoMeshCount; ++i)
        if (Bit(apply, kExoPart[i]) && Bit(vis, kExoPart[i])) return true;
    return false;
}
// The sender's map is the porter suit's for every skeleton piece (controller +0x38 + 8k == kExoPart).
inline bool MapIsPorter(const std::int32_t (&idx)[kExoMeshCount]) {
    for (unsigned i = 0; i < kExoMeshCount; ++i)
        if (idx[i] != kExoPart[i]) return false;
    return true;
}

// ---- the carrier (a demo_sam_clean: the same ArtParts as the porter suit, no mesh controller)
// Its tree checked against the porter suit's (parent index per part, -1 top-level). False: not the parts we know.
inline bool CarrierTreeOk(int n, const std::int16_t* parent) {
    if (n != kSuitParts || !parent) return false;
    if (parent[kSuitBodyPart] != -1 || parent[kContainerPart] != -1 || parent[kHeadPart] != -1) return false;
    if (parent[kExoMainPart] != kContainerPart || parent[kSupportPart] != kContainerPart) return false;
    for (int i = 33; i <= 37; ++i)
        if (parent[i] != kExoMainPart) return false;
    if (parent[kHeavySubPart] != 37 || parent[39] != kExoMainPart || parent[51] != kSupportPart) return false;
    if (parent[kLegOutPart] != kContainerPart || parent[kLegOutLampPart] != kLegOutPart) return false;
    return true;
}
// What the carrier's parts are set to: `touch` the parts we set, `asked` their asked-visible bit. The suit body (0) and
// the head container (52) off, every direct child of the container (1) off but the skeleton pieces the sender shows,
// the skeleton subtrees as the sender shows them (heavysub with heavy), the container on. `leg`: the sender's LegOut
// suit piece too (debug, `gear 29`). Only for a tree CarrierTreeOk accepted.
inline void CarrierPlan(int n, const std::int16_t* parent, const std::uint64_t* vis, bool leg, std::uint64_t (&touch)[2], std::uint64_t (&asked)[2]) {
    touch[0] = touch[1] = asked[0] = asked[1] = 0;
    SetBit(touch, kSuitBodyPart, true);
    SetBit(touch, kHeadPart, true);
    SetBit(touch, kContainerPart, true);
    SetBit(asked, kContainerPart, true);
    for (int i = 0; i < n && i < static_cast<int>(kMaxParts); ++i)
        if (parent[i] == kContainerPart) SetBit(touch, static_cast<unsigned>(i), true);  // off unless set below
    // a piece is asked only with its skeleton parent asked (the part setter does not look at the parent: heavysub asked
    // under a hidden heavy read shown in run 20261122-180651), so ExoMain and Support first (kExoPart's order)
    for (unsigned i = 0; i < kExoMeshCount; ++i) {
        const unsigned pi = kExoPart[i];
        const int par = parent[pi];
        const bool parent_ok = par == kContainerPart || (par >= 0 && Bit(asked, static_cast<unsigned>(par)));
        SetBit(touch, pi, true);
        SetBit(asked, pi, Bit(vis, pi) && parent_ok);
    }
    SetBit(touch, kHeavySubPart, true);
    SetBit(asked, kHeavySubPart, Bit(asked, 37));  // heavysub (no k) with heavy
    SetBit(touch, kLegOutLampPart, true);
    SetBit(asked, kLegOutPart, leg && Bit(vis, kLegOutPart));
    SetBit(asked, kLegOutLampPart, leg && Bit(vis, kLegOutLampPart));
}
// The parts that end up shown (asked and every ancestor asked) from asked bits, for checks: never the suit body or the
// head, only skeleton pieces (and, with `leg`, LegOut) and the container.
inline void ShownFromAsked(int n, const std::int16_t* parent, const std::uint64_t* asked, std::uint64_t (&shown)[2]) {
    shown[0] = shown[1] = 0;
    for (int i = 0; i < n && i < static_cast<int>(kMaxParts); ++i) {
        bool on = Bit(asked, static_cast<unsigned>(i));
        for (int p = parent[i], hops = 0; on && p >= 0 && hops < 16; p = parent[p], ++hops) on = Bit(asked, static_cast<unsigned>(p));
        SetBit(shown, static_cast<unsigned>(i), on);
    }
}

// ---- the sender's override (hidden tests, `gear 28 <type>`): the skeleton pieces read replaced by a type's
// -1 off (what the game shows), 0 none, 1..4 a type.
inline void OverrideParts(int type, std::uint64_t* apply, std::uint64_t* vis) {
    if (type < 0 || type >= kTypeCount) return;
    std::uint64_t p[2];
    TypeParts(static_cast<Type>(type), p);
    for (unsigned i = 0; i < kExoMeshCount; ++i) {
        SetBit(apply, kExoPart[i], true);
        SetBit(vis, kExoPart[i], Bit(p, kExoPart[i]));
    }
}

}  // namespace st::exo
