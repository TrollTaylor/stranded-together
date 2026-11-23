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

// ---- Sam's rain hood on the carrier (round 11, docs/real-partner-body.md section 37). Fragile has no hood; the carrier
// (the porter suit's ArtParts) holds every hood mesh. The game's hood state machine (0x1428125A0, inventory +0x4970):
// 0-2 down (2 waits), 3 raising (HoodOff 5 hidden, HoodSwitch 6 shown, the Apd slot's opening clip), 4 the open loop clip,
// 5 until the clip is done, then HoodOn 4; 6 7 8 10 up; 9 closing (HoodOn hidden, the switch and its close clip), 11 the
// close loop, 12 until done, then HoodOff 5 again and 0. The type (+0x4A88, 0x142791EE0): 0 porter, 1 oxi, 2 sea otter (run
// 20261123-103656: the parts 0x14268C100 looks up per type, 71/76/84 for 1 and 69/70/83 for 2, and the stills; round 10's
// notes had 1 and 2 the other way round).
enum class HoodPhase : std::uint8_t { down = 0, raising = 1, up = 2, lowering = 3 };
inline HoodPhase HoodPhaseOf(int state) {
    switch (state) {
        case 3:
        case 4:
        case 5: return HoodPhase::raising;
        case 6:
        case 7:
        case 8:
        case 10: return HoodPhase::up;
        case 9:
        case 11:
        case 12: return HoodPhase::lowering;
        default: return HoodPhase::down;
    }
}
inline const char* HoodPhaseName(HoodPhase p) {
    switch (p) {
        case HoodPhase::down: return "down";
        case HoodPhase::raising: return "raising";
        case HoodPhase::up: return "up";
        case HoodPhase::lowering: return "lowering";
    }
    return "?";
}
inline constexpr int kHoodTypes = 3;
inline const char* HoodTypeName(int type) { return type == 0 ? "porter" : type == 1 ? "oxi" : type == 2 ? "sea otter" : "none"; }
// The porter suit's ArtParts the game shows for each type: {HoodOn, HoodOff, HoodSwitch} (0x14268C100 looks them up by
// name hash; the carrier looks them up the same way at run time: these are the indices it found in part_sam_body_suiti,
// run 20261123-103656; 83 is hoodotterswitch, 84 hoodoxiswitch).
inline constexpr std::uint8_t kHoodPart[kHoodTypes][3] = {{54, 53, 68}, {71, 76, 84}, {69, 70, 83}};
// The Apd animation slot of a type (+0x4A89: porter 2, oxi 5, sea otter 4) and its clips' index into the inventory's clip
// table (+0x30 -> +0x398): opening, open loop, closing, close loop; porter 0-3 (4-7 with the second set), oxi 8-11,
// sea otter 12-15.
inline int HoodAnimSlot(int type) { return type == 0 ? 2 : type == 1 ? 5 : type == 2 ? 4 : -1; }
// A slot the game's calls may be given (0x1422C80B0 only refuses 9: any other byte indexes past the slot table).
inline bool HoodSlotValid(int slot) { return slot == 2 || slot == 4 || slot == 5; }
enum HoodClip : int { kHoodClipOpen = 0, kHoodClipOpenLoop = 1, kHoodClipClose = 2, kHoodClipCloseLoop = 3 };
inline int HoodClipIndex(int type, bool alt, int which) {
    if (which < 0 || which > 3 || type < 0 || type >= kHoodTypes) return -1;
    return (type == 1 ? 8 : type == 2 ? 12 : alt ? 4 : 0) + which;
}
// A hood part the carrier may show: under the head container (52) within 6 hops, not the head itself.
inline bool HoodPartOk(int n, const std::int16_t* parent, int part) {
    if (!parent || part <= kHeadPart || part >= n || part >= static_cast<int>(kMaxParts)) return false;
    int p = parent[part];
    for (int hops = 0; hops < 6 && p >= 0; ++hops, p = parent[p])
        if (p == kHeadPart) return true;
    return false;
}
// Her hat while Sam's hood is up: the game hides Sam's worn hat with the hood; while the hood is drawn up on her, her hat
// hides under it too (section 26.8 keeps it on her otherwise).
inline bool HoodHidesHat(HoodPhase p) { return p == HoodPhase::up; }

// What the carrier shows of the hood, and the game's Apd calls that unfold it (mirrored on the carrier; `anim` false: the
// timed swap, the raised hood shown when the sender's is up, nothing while it unfolds).
enum class HoodShow : std::uint8_t { none = 0, sw = 1, on = 2 };
inline const char* HoodShowName(HoodShow s) { return s == HoodShow::on ? "up" : s == HoodShow::sw ? "unfolding (the switch mesh)" : "none"; }
enum HoodCall : unsigned {
    kHoodEnable = 1,          // 0x1422B46D0 (apd, slot, 1)
    kHoodPlayOpen = 2,        // 0x1422B4830 (apd, slot, clip opening, 0, 0)
    kHoodPlayOpenLoop = 4,    //   ... open loop (the next tick, as the game's state 4)
    kHoodPlayClose = 8,       //   ... closing
    kHoodPlayCloseLoop = 16,  //   ... close loop
    kHoodDisable = 32,        // 0x1422B46D0 (apd, slot, 0)
};
struct HoodAnim {
    HoodShow show = HoodShow::none;
    int type = -1;          // the type shown or animated (-1 none)
    std::uint8_t mode = 0;  // 0 idle, 1 opening, 2 closing
    std::uint8_t step = 0;  // 1 the clip asked, 2 its loop asked (waiting for the slot to be done)
    double since_ms = 0;
};
inline constexpr double kHoodAnimMinMs = 100, kHoodAnimMaxMs = 3000;
// One tick: `type` the sender's hood type (-1: no reading), `phase` its phase, `anim` the unfold can be played on this
// carrier, `done` the slot's clip is done (0x1422B34E0). Returns the calls to make (HoodCall bits, made in bit order).
inline unsigned HoodAnimTick(HoodAnim& a, int type, HoodPhase phase, bool anim, bool done, double now) {
    unsigned calls = 0;
    if (type != a.type) {
        if (a.mode) calls |= kHoodDisable;
        a = HoodAnim{};
        a.type = type;
    }
    if (type < 0 || type >= kHoodTypes) {
        a.show = HoodShow::none;
        return calls;
    }
    if (!anim) {
        if (a.mode) calls |= kHoodDisable;
        a.mode = a.step = 0;
        a.show = phase == HoodPhase::up ? HoodShow::on : HoodShow::none;
        return calls;
    }
    if (a.mode) {
        if (phase == HoodPhase::down) {  // a load, a teleport: the game reset it
            a.mode = a.step = 0;
            a.show = HoodShow::none;
            return calls | kHoodDisable;
        }
        if (a.step == 1) {
            a.step = 2;
            return calls | (a.mode == 1 ? kHoodPlayOpenLoop : kHoodPlayCloseLoop);
        }
        const double t = now - a.since_ms;
        if ((done && t >= kHoodAnimMinMs) || t >= kHoodAnimMaxMs) {
            a.show = a.mode == 1 ? HoodShow::on : HoodShow::none;
            a.mode = a.step = 0;
            calls |= kHoodDisable;
        }
        return calls;  // the idle rules from the next tick
    }
    switch (phase) {
        case HoodPhase::raising:
            if (a.show != HoodShow::on) {
                a.mode = 1, a.step = 1, a.since_ms = now, a.show = HoodShow::sw;
                calls |= kHoodEnable | kHoodPlayOpen;
            }
            break;
        case HoodPhase::up: a.show = HoodShow::on; break;  // (the game's instant path, a load or a teleport: no unfold)
        case HoodPhase::lowering:
            if (a.show == HoodShow::on) {
                a.mode = 2, a.step = 1, a.since_ms = now, a.show = HoodShow::sw;
                calls |= kHoodEnable | kHoodPlayClose;
            } else {
                a.show = HoodShow::none;
            }
            break;
        case HoodPhase::down: a.show = HoodShow::none; break;
    }
    return calls;
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
// `hood_part` (-1 none, section 37): a hood mesh shown under the head container (HoodPartOk), else the head is off.
inline void CarrierPlan(int n, const std::int16_t* parent, const std::uint64_t* vis, bool leg, int hood_part, std::uint64_t (&touch)[2],
                        std::uint64_t (&asked)[2]) {
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
    // the hood (section 37): the head container and the way down to the hood part asked, every other child on that way off
    // (Sam's hair, face, the other hoods), the hood part's own children (its lamps) left as they are
    if (hood_part >= 0 && HoodPartOk(n, parent, hood_part)) {
        std::uint64_t path[2] = {};
        for (int p = hood_part, hops = 0; p >= 0 && hops < 8; p = parent[p], ++hops) {
            SetBit(path, static_cast<unsigned>(p), true);
            if (p == kHeadPart) break;
        }
        for (int i = 0; i < n && i < static_cast<int>(kMaxParts); ++i) {
            const unsigned u = static_cast<unsigned>(i);
            if (Bit(path, u)) {
                SetBit(touch, u, true);
                SetBit(asked, u, true);
            } else if (parent[i] >= 0 && parent[i] != hood_part && Bit(path, static_cast<unsigned>(parent[i]))) {
                SetBit(touch, u, true);  // off
            }
        }
    }
}
inline void CarrierPlan(int n, const std::int16_t* parent, const std::uint64_t* vis, bool leg, std::uint64_t (&touch)[2], std::uint64_t (&asked)[2]) {
    CarrierPlan(n, parent, vis, leg, -1, touch, asked);
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

// ---- her folded hood (rounds 12-13, docs/real-partner-body.md sections 37.6-37.8; hood_fold.cpp): mesh D of her porter
// suit's LOD0 (fgl_body_suiti mesh_body_lx, RegularSkinnedMeshResource 7f533fe6) skins with these 22 joints of her
// skeleton; the roll on her collar uses only bindings 0-5 and 10-13 (Spine5, the shoulders, neck and head, the collar
// joints), the knee flaps the others (the two sets share no joint).
inline constexpr unsigned kFoldBindings = 22;
inline constexpr std::uint16_t kFoldJoints[kFoldBindings] = {5, 6, 29, 30, 31, 35, 61, 62, 66, 67, 106, 107, 108, 139, 153, 170, 193, 194, 195, 196, 197, 198};
inline constexpr std::uint32_t kFoldRingMask = 0x3C3Fu;  // bindings 0-5, 10-13
inline constexpr unsigned kFoldRingCount = 10;
inline bool FoldRing(unsigned binding) { return binding < kFoldBindings && ((kFoldRingMask >> binding) & 1u) != 0; }
// A skeleton joint the roll is skinned on (one of the 10 above).
inline bool FoldRollJoint(unsigned joint) {
    for (unsigned b = 0; b < kFoldBindings; ++b)
        if (FoldRing(b) && kFoldJoints[b] == joint) return true;
    return false;
}
// A merged joint list (her LodMesh's bindings, every skinned mesh of her body: sorted, unique) holds all 22 of mesh D's.
inline bool FoldJointsCover(const std::uint16_t* list, unsigned n) {
    if (!list || n < kFoldBindings || n > 1024) return false;
    for (unsigned i = 1; i < n; ++i)
        if (list[i] <= list[i - 1]) return false;
    for (unsigned b = 0; b < kFoldBindings; ++b) {
        bool found = false;
        for (unsigned i = 0; i < n && !found; ++i) found = list[i] == kFoldJoints[b];
        if (!found) return false;
    }
    return true;
}
// `gear 34 <method> [pin]` and the product. Methods (round 13, per body: each body's own mesh instance tree):
//   0 never hidden;
//   1 the mask: mesh D's primitive slot in the body's instance tree gets view mask 0 (the byte the game's own ArtPart
//     hide writes; the culler skips the slot in every view): the roll and the two knee flaps go;
//   2 the private bindings: mesh D's node in the body's tree points at a copy of her merged joint bindings with the
//     roll joints' inverse binds zeroed (the roll collapses to a point; the knee flaps stay);
//   3 the LOD pin only (nothing hidden: the control for the stills);
//   4 / 5 the mask / the private bindings on every fragile_suit body of ours, the raised hood or not (hidden tests: her
//     bare collar in the stills, no raised hood over it).
// Pin (the second number): 0 or left out the product (the tree's LOD1 threshold raised), 1 off, 2 the drawable's
// LodDistanceScale.
inline constexpr int kFoldNever = 0, kFoldMask = 1, kFoldBinds = 2, kFoldPinOnly = 3, kFoldForce = 4, kFoldForceBinds = 5;
inline constexpr int kFoldProductMethod = kFoldMask;
enum class FoldPin : std::uint8_t { none, node, scale };
struct FoldPlan {
    bool mask = false, binds = false;
    FoldPin pin = FoldPin::none;
    bool Any() const { return mask || binds || pin != FoldPin::none; }
    bool operator==(const FoldPlan& o) const { return mask == o.mask && binds == o.binds && pin == o.pin; }
};
inline FoldPlan FoldPlanOf(bool want, int method, int pin_arg) {
    FoldPlan p;
    if (!want || method < kFoldMask || method > kFoldForceBinds) return p;
    p.mask = method == kFoldMask || method == kFoldForce;
    p.binds = method == kFoldBinds || method == kFoldForceBinds;
    p.pin = pin_arg == 1 ? FoldPin::none : pin_arg == 2 ? FoldPin::scale : FoldPin::node;
    return p;
}
// The LOD pin on the tree's LodMesh node: its LOD1 threshold raised to its MaxDistance (the last threshold), so LOD0 is
// drawn wherever the body is drawn at all; a MaxDistance that is not finite or beyond 1e9 m: 1e9. 0: no pin possible
// (MaxDistance not above the LOD1 threshold).
inline float FoldPinValue(float lod1, float max_distance) {
    const float v = !(max_distance == max_distance) || max_distance > 1e9f ? 1e9f : max_distance;
    return v > lod1 ? v : 0.0f;
}
// What is ours in one instance tree (the mask on mesh D's slot, the pin on the LodMesh node's LOD1 threshold), and one
// tick's step for it from what was just read: the slot's mask byte (`cur`), the entry's view mask (`show`, entry+0x10C:
// what the game's part visibility writes into every slot; 0 while the game hides the body) and the LOD1 threshold (`t1`;
// her resource's `lod1`, the tree's MaxDistance `tmax`). Every write is a compare-and-swap from the value read.
struct FoldTree {
    bool mask = false, pin = false;
    float pin_val = 0.0f, pin_was = 0.0f;
};
struct FoldStep {
    FoldTree keep;     // what is still ours before the writes (what the game took back, forgotten)
    int mask = -1;     // the byte to write to mesh D's slot (-1 none): 0 the hide, the entry's mask the write-back
    bool pin = false;  // a threshold write, pin_from -> pin_to (the pin, or the write-back of ours)
    float pin_from = 0.0f, pin_to = 0.0f;
};
inline FoldStep FoldStepOf(const FoldTree& s, const FoldPlan& plan, std::uint8_t cur, std::uint8_t show, float t1, float lod1, float tmax) {
    FoldStep st;
    FoldTree& k = st.keep;
    k = s;
    // the mask: the hide while the slot is not 0 (a 0 found is taken as ours: from before a method switch, or the game's
    // own whole-body hide, given back only while the entry shows); given back only while the slot still holds 0 and the
    // entry shows (the game hiding the whole body: every slot 0, nothing to give back; the game wrote the slot: not ours)
    if (plan.mask) {
        if (cur != 0) st.mask = 0;
        else k.mask = true;
    } else if (k.mask) {
        if (cur != 0 || show == 0) k.mask = false;
        else st.mask = show;
    }
    // the pin: ours only while the threshold still holds our value (a rebuild at the same address, or the game's write)
    if (k.pin && t1 != k.pin_val) k.pin = false;
    if (plan.pin == FoldPin::node) {
        const float pv = FoldPinValue(lod1, tmax);
        if (!k.pin && t1 == lod1 && pv > 0.0f) st.pin = true, st.pin_from = t1, st.pin_to = pv;
    } else if (k.pin) {
        st.pin = true, st.pin_from = k.pin_val, st.pin_to = k.pin_was;
    }
    return st;
}
// What is ours after the step's writes (`mask_ok` / `pin_ok`: that compare-and-swap took). A write-back that did not take
// leaves the mask ours (tried again next tick), the pin not (the threshold no longer held our value).
inline FoldTree FoldStepDone(const FoldStep& st, const FoldPlan& plan, bool mask_ok, bool pin_ok) {
    FoldTree s = st.keep;
    if (st.mask >= 0 && mask_ok) s.mask = st.mask == 0;
    if (st.pin) {
        if (plan.pin == FoldPin::node) {
            if (pin_ok) s.pin = true, s.pin_val = st.pin_to, s.pin_was = st.pin_from;
        } else {
            s.pin = false;
        }
    }
    return s;
}
// The drawable's LodDistanceScale while pinned that way (the engine's own ModelLodBiasEvent field): distances x 0.001.
inline constexpr float kFoldScalePin = 0.001f;
// The instance tree's nodes as read: the LodMesh root (type 3, 2 parts, thresholds 0 and her resource's LOD1 distance,
// or our pin while it is ours), her LOD0 (type 2, 4 meshes), mesh D's node (type 4, 1 primitive).
inline bool FoldRootOk(unsigned type, unsigned count, float t0, float t1, float lod1, float pin) {
    return type == 3 && count == 2 && t0 == 0.0f && (t1 == lod1 || (pin > 0.0f && t1 == pin));
}
inline bool FoldLod0Ok(unsigned type, unsigned count) { return type == 2 && count == 4; }
inline bool FoldDNodeOk(unsigned type, unsigned count) { return type == 4 && count == 1; }
// Mesh D's node's bindings slot: the merged bindings her LodMesh built, mesh D's own (the node builder's fallback), the
// private copy while it is ours, or another object (her body's: the model's merged bindings, round 13's runs: 161 joints,
// her LodMesh's slot empty). Which one it is (none: no pointer).
enum class FoldSlot : std::uint8_t { none, merged, own, ours, other };
inline FoldSlot FoldSlotOf(std::uintptr_t slot, std::uintptr_t merged, std::uintptr_t own, std::uintptr_t ours) {
    if (!slot) return FoldSlot::none;
    if (ours && slot == ours) return FoldSlot::ours;
    if (merged && slot == merged) return FoldSlot::merged;
    if (own && slot == own) return FoldSlot::own;
    return FoldSlot::other;
}
// The LOD distances read (parts 0..n-1) look like a LodMeshResource's: part 0 at 0, the others increasing, all finite.
inline bool FoldLodDistancesOk(const float* d, unsigned n) {
    if (!d || n < 2 || n > 8 || !(d[0] >= 0.0f && d[0] < 1.0f)) return false;
    for (unsigned i = 1; i < n; ++i)
        if (!(d[i] > d[i - 1] && d[i] < 10000.0f)) return false;
    return true;
}
// Mesh D not found or not as checked (a structure that does not match, not a tree still being built): tried again
// kFoldFindRetryMs later, the hiding off for the session after kFoldFindTries failures in a row.
inline constexpr unsigned kFoldFindTries = 3;
inline constexpr double kFoldFindRetryMs = 1000.0;

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
