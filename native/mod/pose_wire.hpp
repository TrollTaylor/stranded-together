// Stranded Together pose frame: rides as a link v2 FRAME (unreliable, latest
// wins). Pure code, no game or Windows dependencies (unit-tested).
//
//   'STP1' | u32 seq | u32 sender_ms | u32 flags | f64 x | f64 y | f64 z |
//   f32 qx | f32 qy | f32 qz | f32 qw | f32 speed            (60 bytes, LE)
#pragma once
#include <cstddef>
#include <cstdint>

namespace st::wire {

inline constexpr std::size_t kPoseFrameSize = 60;
inline constexpr std::uint8_t kPoseMagic[4] = {'S', 'T', 'P', '1'};

enum PoseFlags : std::uint32_t {
    kPoseValid = 1u << 0,     // sender has a live local player
    kPoseInVehicle = 1u << 1, // the sender rides something (a parent entity: vehicle, mount); player collision
};

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

struct Pose {
    std::uint32_t seq = 0;
    std::uint32_t sender_ms = 0;
    std::uint32_t flags = 0;
    double x = 0, y = 0, z = 0;
    Quat rot;
    float speed = 0;  // m/s, informational
};

std::size_t EncodePose(const Pose& p, std::uint8_t (&out)[kPoseFrameSize]) noexcept;
// Rejects bad magic/size, NaN/inf, |coord| > 1e7, non-unit quaternions.
bool DecodePose(const std::uint8_t* data, std::size_t size, Pose& out) noexcept;

// Rotation helpers. m is a row-major 3x3 rotation (Decima RotMatrix rows).
Quat QuatFromMatrix(const float (&m)[3][3]) noexcept;
void MatrixFromQuat(const Quat& q, float (&m)[3][3]) noexcept;
Quat Slerp(const Quat& a, const Quat& b, float t) noexcept;
Quat Normalize(const Quat& q) noexcept;

// ---------------------------------------------------------------- joint pose frame (STJ1)
// The sender's finished local skeleton pose (docs/real-partner-body.md): every joint's
// local rotation plus the translation of a few joints (root, pelvis), so a body on the
// same skeleton layout can play the sender's real animation. Rides next to STP1 in the
// same datagram (own magic, own sequence state on the receiver).
//
//   'STJ1' | u32 seq | u32 sender_ms | u32 layout_hash | u8 joint_count | u8 body_id |
//   u8 flags | u8 reserved                                                   (20 bytes)
//   joint_count x 6 bytes: smallest-three quaternion (bits 0-1 index of the dropped
//                          largest component, made positive; 3 x 15 bits, value
//                          (v / (1/sqrt 2)) * 16383 + 16384; bits 47 spare)
//   u8 n | n x {u8 joint, i16 x, i16 y, i16 z} translations in units of 1e-4 m (+-3.27 m)
//   [flags & kJointsOutfit] 8 bytes: the sender's Sam outfit (Outfit below, in field order;
//                          docs/real-partner-body.md section 9)
inline constexpr std::uint8_t kJointMagic[4] = {'S', 'T', 'J', '1'};
inline constexpr unsigned kMaxJoints = 80;  // Sam's layout has 77
inline constexpr unsigned kMaxJointTranslations = 4;
inline constexpr std::uint32_t kSamLayoutHash = 0xBE39C83Fu;
inline constexpr std::size_t kJointHeaderSize = 20;
inline constexpr std::size_t kOutfitSize = 8;
inline constexpr std::size_t JointFrameSize(unsigned joints, unsigned translations, bool outfit = false) {
    return kJointHeaderSize + joints * 6u + 1u + translations * 7u + (outfit ? kOutfitSize : 0u);
}
inline constexpr std::size_t kMaxJointFrameSize = JointFrameSize(kMaxJoints, kMaxJointTranslations, true);

enum JointFlags : std::uint8_t {
    kJointsValid = 1u << 0,
    kJointsOutfit = 1u << 1,  // the 8-byte outfit block follows the translations
    kJointsKnown = kJointsValid | kJointsOutfit,  // a frame with other bits may carry more blocks (ignored)
};

// The sender's own Sam outfit as the game keeps it (player inventory component +0x4BDC..+0x4BE1,
// the streaming-control body variant, the applied "exos" art-parts variation). 0xFF = unknown /
// none. The receiver dresses a Sam body with it (exos variation + suit colour); the cap and
// glasses bytes are carried for later use.
struct Outfit {
    std::uint8_t variant = 0xFF;      // DSPlayerBodyVariant index: 0 porter suit, 2 MULE suit, ... 7 skeleton only
    std::uint8_t exos = 0xFF;         // index into the porter suit's 44 ArtPartsVariations (suit, glove, exoskeleton tints)
    std::uint8_t suit_colour = 0xFF;  // 0..14 palette (shader variable), 15 silver, 16 gold (both folded into exos)
    std::uint8_t cap_colour = 0xFF;
    std::uint8_t glasses_a_lens = 0xFF, glasses_b_lens = 0xFF, glasses_a_frame = 0xFF, glasses_b_frame = 0xFF;
    bool operator==(const Outfit& o) const {
        return variant == o.variant && exos == o.exos && suit_colour == o.suit_colour && cap_colour == o.cap_colour &&
               glasses_a_lens == o.glasses_a_lens && glasses_b_lens == o.glasses_b_lens && glasses_a_frame == o.glasses_a_frame &&
               glasses_b_frame == o.glasses_b_frame;
    }
    bool operator!=(const Outfit& o) const { return !(*this == o); }
};
inline constexpr std::uint8_t kPorterVariant = 0;  // Outfit::variant of the porter suit
inline constexpr std::uint8_t kExosCount = 44;     // the porter suit's ArtPartsVariations
// The exos index a receiver may use: it indexes the porter suit's list only, so it counts only
// while the sender wears the porter suit (another variant's index belongs to another list).
// 0xFF = none / unknown.
inline constexpr std::uint8_t PorterExos(const Outfit& o) {
    return o.variant == kPorterVariant && o.exos < kExosCount ? o.exos : std::uint8_t{0xFF};
}

struct JointFrame {
    std::uint32_t seq = 0;
    std::uint32_t sender_ms = 0;
    std::uint32_t layout_hash = kSamLayoutHash;
    std::uint8_t joint_count = 0;
    std::uint8_t body_id = 0;
    std::uint8_t flags = 0;
    Quat q[kMaxJoints];
    std::uint8_t t_count = 0;
    std::uint8_t t_joint[kMaxJointTranslations] = {};
    float t[kMaxJointTranslations][3] = {};
    Outfit outfit;  // meaningful when flags & kJointsOutfit
};

// Returns the encoded size (0 when the frame cannot be encoded: too many joints, a
// translation joint out of range, NaN). `out` should hold kMaxJointFrameSize bytes.
std::size_t EncodeJoints(const JointFrame& f, std::uint8_t* out, std::size_t cap) noexcept;
// Rejects bad magic, a size that does not match the counts (and the outfit flag),
// joint_count 0 or above kMaxJoints, a translation joint >= joint_count.
bool DecodeJoints(const std::uint8_t* data, std::size_t size, JointFrame& out) noexcept;
// Per joint nlerp with the hemisphere fix (q and -q are the same rotation), then
// renormalised; translations lerped when both frames carry the same joints (else b's);
// header fields and the outfit are b's.
void BlendJoints(const JointFrame& a, const JointFrame& b, float t, JointFrame& out) noexcept;
// Angle in degrees between two rotations (0..180).
float AngleBetween(const Quat& a, const Quat& b) noexcept;

// ---------------------------------------------------------------- player name frame (STN1)
// The name this player wants shown above their head on the other player's screen (name tags,
// docs/real-partner-body.md section 10). Rides as one more FRAME message in a pose datagram on
// the first sends of a session, about once a second after that and right after a change. A
// receiver of another build never sees it (the HELLO build check), and DecodePose / DecodeJoints
// reject its magic, so an older decoder would drop it silently.
//
//   'STN1' | u8 version (1) | u8 flags | u8 len (0..64) | u8 reserved | UTF-8 name[len]   (8 + len)
inline constexpr std::uint8_t kNameMagic[4] = {'S', 'T', 'N', '1'};
inline constexpr std::uint8_t kNameVersion = 1;
inline constexpr std::size_t kNameHeaderSize = 8;
inline constexpr unsigned kMaxNameBytes = 64;
inline constexpr unsigned kMaxNameCodepoints = 32;
inline constexpr std::size_t kMaxNameFrameSize = kNameHeaderSize + kMaxNameBytes;

enum NameFlags : std::uint8_t {
    kNameFromHost = 1u << 0,      // the sender is the session's host
    kNameKnown = kNameFromHost,   // other bits are cleared by the decoder (a newer build's)
};

struct NameFrame {
    std::uint8_t flags = 0;
    std::uint8_t len = 0;                // bytes in name, without the terminator
    char name[kMaxNameBytes + 1] = {};   // UTF-8, sanitized (SanitizeName), NUL-terminated
};

// Cleans a display name: invalid UTF-8 bytes, C0/C1 controls, DEL, bidi and other invisible
// format characters (U+061C, U+200B..U+200F, U+2028..U+202E, U+2060..U+206F, U+FEFF) are dropped,
// runs of white space become one space, leading/trailing space is trimmed, and the result is cut
// at a code-point boundary to at most kMaxNameCodepoints code points and min(cap - 1,
// kMaxNameBytes) bytes. Always NUL-terminates (cap > 0); returns the length.
std::size_t SanitizeName(const char* in, std::size_t n, char* out, std::size_t cap) noexcept;
// Encodes f (its name is sanitized again on the way). Returns the size, 0 when cap is too small.
std::size_t EncodeName(const NameFrame& f, std::uint8_t* out, std::size_t cap) noexcept;
// Rejects bad magic, another version, len > kMaxNameBytes and a size other than 8 + len. The
// name comes out sanitized (a peer cannot smuggle controls into the overlay or the log).
bool DecodeName(const std::uint8_t* data, std::size_t size, NameFrame& out) noexcept;

// ---------------------------------------------------------------- gear layout frame (STC1)
// What the sender carries (docs/real-partner-body.md section 12): the backpack, the Odradek, the
// BB pod and every cargo piece on the back, arms, waist and hands, each as a kind (+ the game's
// EDSBaggage2Model and colour variation for cargo) and a pose relative to an anchor: a joint of
// Sam's 77-joint skeleton, or an earlier item (cargo sits on the backpack). The receiver puts
// prop entities there on the partner's body. Sent in its own datagram (FRAME message) when the
// layout changes and about once a second otherwise; own magic, own sequence filter.
//
//   'STC1' | u32 seq | u8 version (3) | u8 count | u8 flags | u8 dropped | u16 layout hash |
//   u16 reserved                                                              (16 bytes)
//   [flags bit 1, kGearParts: u64 apply[2] | u64 visible[2]                   (32 bytes)]
//   [flags bit 2, kGearPackParts: u64 apply | u64 visible, the backpack's ArtParts (16 bytes)]
//   count x 18 bytes: u8 kind | u8 model | u8 variation | u8 joint | u8 parent | u8 res |
//                     i16 x, y, z (1 mm, +-32 m) | 48-bit smallest-three rotation (as STJ1)
// Version 2 (v0.8): the `entity` kind (a gear entity of the game's own, named by `res`, an id of
// the sender's resource table, sent as STR1 frames below) and the parts block (which of Sam's own
// ArtParts the sender shows: exoskeleton, BB pod pipe, hip gear, pouches). Version 1 frames are
// refused (mixed builds never meet: HELLO build check). Version 3 (after v0.9): the `weapon` kind;
// version 2 frames are refused.
inline constexpr std::uint8_t kGearMagic[4] = {'S', 'T', 'C', '1'};
inline constexpr std::uint8_t kGearVersion = 3;
inline constexpr std::size_t kGearHeaderSize = 16;
inline constexpr std::size_t kGearPartsSize = 32;
inline constexpr std::size_t kGearPackPartsSize = 16;
inline constexpr std::size_t kGearItemSize = 18;
inline constexpr unsigned kMaxGearItems = 56;
inline constexpr unsigned kMaxGearParts = 128;  // ArtParts indices the parts block can name
inline constexpr std::size_t GearFrameSize(unsigned count, bool parts = false, bool pack_parts = false) {
    return kGearHeaderSize + (parts ? kGearPartsSize : 0) + (pack_parts ? kGearPackPartsSize : 0) + count * kGearItemSize;
}
inline constexpr std::size_t kMaxGearFrameSize = GearFrameSize(kMaxGearItems, true, true);  // 1072
inline constexpr std::uint8_t kGearNoParent = 0xFF;  // GearItem::parent: placed on `joint`
inline constexpr std::uint8_t kGearNone = 0xFF;      // GearItem::model / variation: none / unknown

enum class GearKind : std::uint8_t {
    backpack = 1,  // the backpack frame
    odradek = 2,   // the Odradek on the left shoulder
    bb_pod = 3,    // the BB pod on the chest
    cargo = 4,     // one baggage piece; model = EDSBaggage2Model
    entity = 5,    // one of the game's own gear entities (BB pod, accessory, what hangs on the backpack ...):
                   // `res` names its resource in the sender's table (STR1)
    weapon = 6,    // version 3: a weapon or hand tool the sender shows (DSWeaponEntity: in a hand, on the backpack
                   // hanger): `res` names the sender's weapon resource (STR1), `model` its EDSWeaponId, `variation`
                   // its level. The receiver never spawns that resource (a copy crashes the game): it shows a
                   // cinematic prop with the same model, or nothing (docs/real-partner-body.md section 17)
};
inline constexpr std::uint8_t kGearKindMax = 6;
// The kinds whose `res` is a resource id of the sender's table (sent, part of the layout hash).
inline constexpr bool GearHasRes(GearKind k) { return k == GearKind::entity || k == GearKind::weapon; }
inline constexpr unsigned kMaxGearRes = 64;  // resource ids 0..63

enum GearFlags : std::uint8_t {
    kGearValid = 1u << 0,   // the sender has a live player whose gear was read
    kGearParts = 1u << 1,   // the parts block follows the header
    kGearPackParts = 1u << 2,  // the backpack's parts block follows (after the parts block)
    kGearKnown = kGearValid | kGearParts | kGearPackParts,
};

struct GearItem {
    GearKind kind = GearKind::cargo;
    std::uint8_t model = kGearNone;      // EDSBaggage2Model (cargo), kGearNone otherwise
    std::uint8_t variation = kGearNone;  // index into the baggage config's colour variations; kGearNone = the prop's
                                         // own look (what this build always sends: the piece's variation is not read)
    std::uint8_t joint = 0;              // Sam-skeleton joint (0..76) the item hangs on (parent == kGearNoParent)
    std::uint8_t parent = kGearNoParent; // or an earlier item's index (cargo stacked on the backpack)
    std::uint8_t res = kGearNone;        // entity, weapon: the resource id (< kMaxGearRes); kGearNone otherwise
    float p[3] = {};                     // position in the anchor's frame (m)
    Quat q;                              // rotation in the anchor's frame
};

struct GearFrame {
    std::uint32_t seq = 0;
    std::uint8_t flags = 0;
    std::uint8_t count = 0;
    std::uint8_t dropped = 0;  // items the sender had to leave out (over kMaxGearItems)
    // kGearParts: Sam's own ArtParts (index < kMaxGearParts) the sender reports (apply) and which of
    // them it shows (visible, a subset of apply). The receiver mirrors them on a Sam body only.
    std::uint64_t parts_apply[2] = {};
    std::uint64_t parts_visible[2] = {};
    // kGearPackParts: the backpack entity's own ArtParts (index < 64: its bag, rain cover, pouches);
    // the receiver mirrors them on the backpack prop when its parts are the same resources.
    std::uint64_t pack_apply = 0;
    std::uint64_t pack_visible = 0;
    GearItem items[kMaxGearItems];
};

// ---------------------------------------------------------------- gear resource frame (STR1)
// One entry of the sender's gear resource table: the id the `entity` items use, the entity
// resource's path (no ".core") and its raw GUID (the bytes the receiver's streaming manager takes).
// Sent when an id is first used and round-robin about every 250 ms (a lost frame is resent).
//
//   'STR1' | u8 version (1) | u8 id | u8 path length n | u8 reserved | 16-byte GUID | n path bytes
inline constexpr std::uint8_t kGearResMagic[4] = {'S', 'T', 'R', '1'};
inline constexpr std::uint8_t kGearResVersion = 1;
inline constexpr std::size_t kGearResHeaderSize = 24;
inline constexpr std::size_t kMaxGearResPath = 160;
inline constexpr std::size_t kMaxGearResFrameSize = kGearResHeaderSize + kMaxGearResPath;

struct GearRes {
    std::uint8_t id = 0;
    std::uint8_t guid[16] = {};
    char path[kMaxGearResPath + 1] = {};
};
// A path the receiver may load: 1..160 bytes of [a-z0-9_/.-], starting "ds/entities/", no "..".
bool GearResPathOk(const char* path) noexcept;
// The receiver's own check of a partner resource before anything is loaded from it (partner_gear.cpp
// SetPartnerRes): only the cores whose entities were mirrored in hidden runs (the backpack's items,
// ds_player_inventory, the stowed weapons, sam_cant, the hanging boots) and none of the entities the
// level-1 mirror policy refuses that the census found on Sam (the BB pod entity unless allow_pod_entity,
// the cuff device, the baby, the boots, the chiral cylinder, the backpack belt). why: the reason when not.
bool GearResProven(const char* path, const std::uint8_t (&guid)[16], bool allow_pod_entity, const char** why) noexcept;
// A weapon's own resource (ds/entities/weapons/..., but not the three proven entity resources of that
// folder: weapon_collection, sam_cant, sam_boot): its entity is a DSWeaponEntity, whose copy crashes the
// game (real-partner-body.md section 17.1). The receiver never loads one, at any mirror level; it only
// keeps it to show the partner's weapon items as props.
bool GearResWeaponOnly(const char* path) noexcept;
// A GUID as the logs print it (8-4-4-4-12, the first three groups little-endian): 37 bytes with the 0.
void GearGuidText(const std::uint8_t (&guid)[16], char (&out)[37]) noexcept;
std::size_t EncodeGearRes(const GearRes& r, std::uint8_t* out, std::size_t cap) noexcept;
// Rejects bad magic / version, id >= kMaxGearRes, a size other than 24 + n, and a path that is
// not GearResPathOk.
bool DecodeGearRes(const std::uint8_t* data, std::size_t size, GearRes& out) noexcept;

// ---------------------------------------------------------------- gear look frame (STV1)
// How one item of the sender's current gear layout looks (docs/real-partner-body.md section 13): the
// art-parts variations applied to the game's entity (texture-set swaps: the gold / silver cap, the
// backpack's paint, the rain cover's level, a shield's level, a case's colour), by resource GUID in
// component order, and the live entries of its ArtPartsShaderVariableComponent (colour indices, lens
// colours ...). The receiver resolves each GUID among the resources its own game already holds and
// applies both to its copy of that item. Sent when an item's look changes and round-robin; keyed by
// (layout hash, item index): a look for another layout is ignored.
//
//   'STV1' | u8 version (1) | u8 item | u16 layout hash | u8 kind | u8 res | u8 nvar | u8 nsv |
//   u64 parts apply | u64 parts shown                                                         (28 bytes)
//   nvar x 16-byte GUID
//   nsv x { u32 hash | u16 part (0x1FF = every part) | u8 count (1..4) | u8 0 | f32 v[4] }   (24 bytes each)
inline constexpr std::uint8_t kGearLookMagic[4] = {'S', 'T', 'V', '1'};
inline constexpr std::uint8_t kGearLookVersion = 1;
inline constexpr std::size_t kGearLookHeaderSize = 28;
inline constexpr std::size_t kGearLookVarSize = 24;
inline constexpr unsigned kMaxLookVariations = 8;
inline constexpr unsigned kMaxLookShaderVars = 32;
inline constexpr std::uint16_t kLookAllParts = 0x1FF;
inline constexpr std::size_t GearLookSize(unsigned nvar, unsigned nsv) { return kGearLookHeaderSize + nvar * 16u + nsv * kGearLookVarSize; }
inline constexpr std::size_t kMaxGearLookFrameSize = GearLookSize(kMaxLookVariations, kMaxLookShaderVars);  // 924

struct GearShaderVar {
    std::uint32_t hash = 0;
    std::uint16_t part = kLookAllParts;  // an ArtParts index of the entity, or kLookAllParts
    std::uint8_t count = 1;              // values used (1..4)
    float v[4] = {};
};
struct GearLook {
    std::uint8_t item = 0;     // index in the sender's gear frame (< kMaxGearItems)
    std::uint16_t layout = 0;  // GearLayoutHash of that frame
    GearKind kind = GearKind::entity;
    std::uint8_t res = kGearNone;  // entity, weapon: the resource id (as the item's); kGearNone otherwise
    std::uint8_t nvar = 0, nsv = 0;
    // The entity's own ArtParts (index < 64) the sender reports, and which of them are shown (the rain
    // cover's size for the load, a pouch's state ...); shown bits outside apply are dropped.
    std::uint64_t parts_apply = 0, parts_visible = 0;
    std::uint8_t guid[kMaxLookVariations][16] = {};
    GearShaderVar sv[kMaxLookShaderVars];
};
// Rejects (returns 0) item >= kMaxGearItems, an unknown kind, nvar / nsv over the caps, a count
// outside 1..4, a part above 0x1FF and non-finite values.
std::size_t EncodeGearLook(const GearLook& l, std::uint8_t* out, std::size_t cap) noexcept;
// The same checks, plus bad magic / version and a size other than GearLookSize(nvar, nsv). Unused
// value slots (index >= count) come out 0.
bool DecodeGearLook(const std::uint8_t* data, std::size_t size, GearLook& out) noexcept;
// A hash of what the look shows (the parts, the variations in order, each variable's hash / part / count and
// its values rounded to 1/256), not of item / layout: equal looks hash equal.
std::uint32_t GearLookHash(const GearLook& l) noexcept;

// ---------------------------------------------------------------- gear skinned pose frame (STK1)
// The local joint pose of one skinned item of the sender's current gear layout (the backpack: its
// frame extends with the cargo stacked on it, its cover joints shape the rain cover; docs/real-
// partner-body.md section 13). Rotations as STJ1's smallest-three, translations in 1e-4 m.
//
//   'STK1' | u8 version (1) | u8 item | u16 layout hash | u32 skeleton layout hash | u8 n | u8 0 | u16 0  (16 bytes)
//   n x { 6-byte rotation | i16 x, y, z }                                             (12 bytes each)
inline constexpr std::uint8_t kGearPoseMagic[4] = {'S', 'T', 'K', '1'};
inline constexpr std::uint8_t kGearPoseVersion = 1;
inline constexpr std::size_t kGearPoseHeaderSize = 16;
inline constexpr unsigned kMaxGearPoseJoints = 64;
inline constexpr std::size_t GearPoseSize(unsigned n) { return kGearPoseHeaderSize + n * 12u; }
inline constexpr std::size_t kMaxGearPoseFrameSize = GearPoseSize(kMaxGearPoseJoints);  // 784

struct GearPose {
    std::uint8_t item = 0;
    std::uint16_t layout = 0;
    std::uint32_t skeleton = 0;  // the skeleton's layout hash (the receiver's copy must have the same)
    std::uint8_t n = 0;          // joints (1..kMaxGearPoseJoints)
    Quat q[kMaxGearPoseJoints];
    float t[kMaxGearPoseJoints][3] = {};
};
// Rejects (0) item >= kMaxGearItems, n 0 or over the cap, a non-finite rotation, a translation
// beyond +-3.2 m.
std::size_t EncodeGearPose(const GearPose& p, std::uint8_t* out, std::size_t cap) noexcept;
bool DecodeGearPose(const std::uint8_t* data, std::size_t size, GearPose& out) noexcept;
// The largest difference between two poses of the same joint count: translation (m), rotation (deg).
// False when the counts or skeletons differ.
bool GearPoseDiff(const GearPose& a, const GearPose& b, float* max_m, float* max_deg) noexcept;

// The layout: kinds, models, variations and anchors (not the poses), 16-bit FNV-1a. Two frames
// with the same hash need the same props.
std::uint16_t GearLayoutHash(const GearFrame& f) noexcept;
// Returns the size, 0 when the frame cannot be encoded (count over the cap, an unknown kind, a
// joint >= 77, a parent that is not an earlier item, NaN, a position beyond +-32 m).
std::size_t EncodeGear(const GearFrame& f, std::uint8_t* out, std::size_t cap) noexcept;
// Rejects bad magic, another version, a size other than 16 + 18 * count, count over the cap, an
// unknown kind, a bad anchor (as EncodeGear) and a layout hash that does not match the items.
// Unknown flag bits are cleared.
bool DecodeGear(const std::uint8_t* data, std::size_t size, GearFrame& out) noexcept;
// The largest change between two frames with the same layout: position (m) and rotation (deg).
// False when the layouts differ.
bool GearPoseDelta(const GearFrame& a, const GearFrame& b, float* max_m, float* max_deg) noexcept;

// Prop reuse across a layout change (the receiver's props are keyed by what they show, not by the
// item index: one piece unloaded must not respawn every later prop). old_keys[i]: the key of the
// prop held in slot i (-1: none); want[j]: the key item j needs (-1: no prop). out[j] = the old
// slot whose prop item j takes over, or -1 (a new prop). The same index is kept when its key
// matches, then the first unused old slot with that key. Each old slot is used at most once.
void GearAssignProps(const int* old_keys, unsigned old_n, const int* want, unsigned want_n, int* out) noexcept;

// Retry schedule of a prop that failed to start or load (transient: a load the game cancelled, the
// streaming manager busy, no local player during a load): 2 s, 4 s, 8 s, ... at most 30 s after
// `failures` (1-based) failures in a row.
double GearRetryDelayMs(unsigned failures) noexcept;

}  // namespace st::wire
