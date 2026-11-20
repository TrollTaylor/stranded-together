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
    kPosePassenger = 1u << 2, // the sender's Sam is carried as the passenger of the receiver's vehicle (riding together, M3)
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
//   'STC1' | u32 seq | u8 version (4) | u8 count | u8 flags | u8 dropped | u16 layout hash |
//   u16 reserved                                                              (16 bytes)
//   [flags bit 1, kGearParts: u64 apply[2] | u64 visible[2]                   (32 bytes)]
//   [flags bit 2, kGearPackParts: u64 apply | u64 visible, the backpack's ArtParts (16 bytes)]
//   [flags bit 3, kGearBoots: u32 apply | u32 visible, the boots' ArtParts (8 bytes)]
//   count x 18 bytes: u8 kind | u8 model | u8 variation | u8 joint | u8 parent | u8 res |
//                     i16 x, y, z (1 mm, +-32 m) | 48-bit smallest-three rotation (as STJ1)
// Version 2 (v0.8): the `entity` kind (a gear entity of the game's own, named by `res`, an id of
// the sender's resource table, sent as STR1 frames below) and the parts block (which of Sam's own
// ArtParts the sender shows: exoskeleton, BB pod pipe, hip gear, pouches). Version 1 frames are
// refused (mixed builds never meet: HELLO build check). Version 3 (after v0.9): the `weapon` kind;
// version 2 frames are refused. Version 4 (after v0.11): the `boots` kind and its parts block
// (docs/real-partner-body.md section 21); version 3 frames are refused. A frame has the boots block
// exactly when it has a boots item, at most one, on joint 0 with no parent; visible bits outside
// apply are refused.
inline constexpr std::uint8_t kGearMagic[4] = {'S', 'T', 'C', '1'};
inline constexpr std::uint8_t kGearVersion = 4;
inline constexpr std::size_t kGearHeaderSize = 16;
inline constexpr std::size_t kGearPartsSize = 32;
inline constexpr std::size_t kGearPackPartsSize = 16;
inline constexpr std::size_t kGearBootsSize = 8;
inline constexpr std::size_t kGearItemSize = 18;
inline constexpr unsigned kMaxGearItems = 56;
inline constexpr unsigned kMaxGearParts = 128;  // ArtParts indices the parts block can name
inline constexpr unsigned kMaxGearBootsParts = 32;  // the boots' ArtParts the boots block can name
inline constexpr std::size_t GearFrameSize(unsigned count, bool parts = false, bool pack_parts = false, bool boots = false) {
    return kGearHeaderSize + (parts ? kGearPartsSize : 0) + (pack_parts ? kGearPackPartsSize : 0) + (boots ? kGearBootsSize : 0) +
           count * kGearItemSize;
}
inline constexpr std::size_t kMaxGearFrameSize = GearFrameSize(kMaxGearItems, true, true, true);  // 1080
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
    boots = 7,     // version 4: the sender's equipped boots (DSPlayerBootsComponent's entity, part_sam_foot_def); on
                   // joint 0, identity pose; which of their ArtParts show is the boots block. The receiver draws a
                   // cutscene boots prop posed from the body's legs (docs/real-partner-body.md section 21)
};
inline constexpr std::uint8_t kGearKindMax = 7;
// The kinds whose `res` is a resource id of the sender's table (sent, part of the layout hash).
inline constexpr bool GearHasRes(GearKind k) { return k == GearKind::entity || k == GearKind::weapon; }
inline constexpr unsigned kMaxGearRes = 64;  // resource ids 0..63

enum GearFlags : std::uint8_t {
    kGearValid = 1u << 0,   // the sender has a live player whose gear was read
    kGearParts = 1u << 1,   // the parts block follows the header
    kGearPackParts = 1u << 2,  // the backpack's parts block follows (after the parts block)
    kGearBoots = 1u << 3,      // the boots' parts block follows (after the backpack's); exactly with a boots item
    kGearKnown = kGearValid | kGearParts | kGearPackParts | kGearBoots,
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
    // kGearBoots: the boots entity's own ArtParts (index < 32: mk1 / mk2 / mk3 left and right, soles, wear) the
    // sender reports and shows (visible within apply); the receiver sets them on its boots prop.
    std::uint32_t boots_apply = 0;
    std::uint32_t boots_visible = 0;
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

// STV1 item 255 (after v0.12): the look of the vehicle the sender reports in STM1 (docs/real-partner-body.md section
// 22.7): kind `entity`, res none, `layout` = the index of its drivable resource in VehicleResources() (0..4).
inline constexpr std::uint8_t kLookVehicleItem = 0xFF;
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
// Rejects (returns 0) item >= kMaxGearItems (but kLookVehicleItem with kind entity and a layout below
// kVehicleResourceCount), an unknown kind, nvar / nsv over the caps, a count
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

// ---------------------------------------------------------------- speech frame (STS1)
// One line the sender's player said (docs/audio-research.md sections 7 and 15): the sentences of one talk
// request (MsgDSStartTalk to the sender's own player), by the SentenceResource's 16-byte object GUID, which is
// the same on both games and language-independent (each receiver plays the line in its own voice language).
// Rides the link's reliable ordered channel (kSpeechChannel), so it is resent until acknowledged; the
// receiver also ignores a seq it has seen (idempotent).
//
//   'STS1' | u8 version (1) | u8 kind | u8 flags | u8 n | u32 seq | u32 sender_ms | u32 situation hash  (20 bytes)
//   n x 16-byte sentence GUID (raw bytes as the game keeps them at RTTIRefObject + 0x08)
//
// kind 0 (kSpeechPlayed): n = 1..4 sentences the sender's Sam said; the receiver plays them on the partner.
// kind 1 (kSpeechMuted): n = 0; the sender's player is not Sam, so Sam's line was not sent (statistics only).
// kind 2 (kSpeechOwnLine): n = 1..4; a line of the sender's own character (reserved for Phase 4; this build
//        never sends it, and a receiver resolves it like kind 0, from the Sam lines it holds).
inline constexpr std::uint8_t kSpeechMagic[4] = {'S', 'T', 'S', '1'};
inline constexpr std::uint8_t kSpeechVersion = 1;
inline constexpr std::size_t kSpeechHeaderSize = 20;
inline constexpr unsigned kMaxSpeechSentences = 4;
inline constexpr std::size_t SpeechFrameSize(unsigned n) { return kSpeechHeaderSize + n * 16u; }
inline constexpr std::size_t kMaxSpeechFrameSize = SpeechFrameSize(kMaxSpeechSentences);  // 84
inline constexpr std::uint8_t kSpeechChannel = 2;  // the link's reliable channel for game data (link_protocol.hpp)

enum class SpeechKind : std::uint8_t { played = 0, muted = 1, own_line = 2 };
inline constexpr std::uint8_t kSpeechKindMax = 2;

enum SpeechFlags : std::uint8_t {
    kSpeechFromHost = 1u << 0,  // the sender is the session's host
    kSpeechKnown = kSpeechFromHost,  // other bits are cleared by the decoder (a newer build's)
};

struct Speech {
    std::uint8_t kind = 0;  // SpeechKind
    std::uint8_t flags = 0;
    std::uint8_t n = 0;
    std::uint32_t seq = 0;
    std::uint32_t sender_ms = 0;
    std::uint32_t situation = 0;  // the situation hash (0 = unknown: what this build sends)
    std::uint8_t guid[kMaxSpeechSentences][16] = {};
};
// Returns the size, 0 when it cannot be encoded (see DecodeSpeech for the rules) or cap is too small.
std::size_t EncodeSpeech(const Speech& s, std::uint8_t* out, std::size_t cap) noexcept;
// Rejects bad magic, another version, kind > 2, n > 4, a size other than 20 + 16 n, kind 0 / 2 with n = 0,
// kind 1 with n > 0, seq 0, and an all-zero GUID. Unknown flag bits are cleared.
bool DecodeSpeech(const std::uint8_t* data, std::size_t size, Speech& out) noexcept;

// ---------------------------------------------------------------- sound event frame (STA1)
// The sender's own sounds as Wwise events (docs/audio-research.md sections 7 and 16, Phase 3: footsteps, foley,
// cargo, weapons): each event posted on one of the local player's own Wwise game objects, with the RTPC values set
// on that object, the playing id the sender's Wwise returned (so a later stop finds it) and where it sounded
// relative to one of the player's joints. The receiver posts the same event id (the same banks on both games) on a
// game object of its own kept at the partner body's matching joint, at the pose play time of the event. Rides as a
// FRAME next to STP1 / STJ1 in the pose datagram (unreliable: a lost footstep is better than a late one).
//
//   'STA1' | u8 version (2) | u8 n | u16 seq | u32 sender_ms                                   (12 bytes)
//   n x item (24 bytes): u32 a | u32 b | u8 op | u8 slot | u8 tag | u8 nrtpc | u16 age_ms | u16 fade_ms |
//                        i16 off[3] (cm; world axes, the vehicle's frame on slot 7) | u8 category | u8 flags
//                        nrtpc x {u32 rtpc id | f32 value}                                   (8 bytes each)
//
// op 0 post:   a = event id (!= 0), b = the sender's playing id (!= 0), slot < kSoundSlots, nrtpc <= 12, fade 0.
// op 1 stop:   a = 0, b = the playing id to stop (!= 0), slot / tag / nrtpc / offset 0, fade_ms = the fade.
// op 2 action: a = event id (!= 0), b = a playing id or 0, slot = AkActionOnEventType (0..4), nrtpc 0, fade_ms.
// op 3 rtpc:   a = 0, b = 0, slot < kSoundSlots, nrtpc 1..12 (new values on the tag's object), fade_ms = the
//              interpolation time.
// tag: the sender's game object (1..255, reused after it is unregistered; 0 = none). The item's time is
// sender_ms - age_ms (age <= 2,000). category < kSoundCategories (statistics, and the vehicle rules below).
// Version 2 (2026-11-18, docs/real-partner-body.md section 22.9): category 5 `vehicle` (the sounds of the vehicle the
// sender rides: its SoundComponent's engine, road noise, suspension, battery...) and slot 7 `vehicle` (the offset is
// in the vehicle's own frame, cm: x right, y forward, z up; the receiver's object follows the partner's vehicle copy,
// not a joint). A post or rtpc item has slot 7 exactly when its category is vehicle. flags (a reserved 0 in v1):
// bit 0 kSoundHeld, on a vehicle post only: a loop the sender posts again every kSoundHeldRefreshMs while it plays
// there, with the same playing id (the receiver posts it only when it does not play already, and stops it when no
// such refresh came for kSoundHeldLeaseMs). Other bits 0. Version 1 is refused (mixed builds are refused at HELLO).
inline constexpr std::uint8_t kSoundMagic[4] = {'S', 'T', 'A', '1'};
inline constexpr std::uint8_t kSoundVersion = 2;
inline constexpr std::size_t kSoundHeaderSize = 12, kSoundItemSize = 24, kSoundRtpcSize = 8;
inline constexpr unsigned kMaxSoundItems = 32, kMaxSoundRtpcs = 12;
inline constexpr std::size_t kMaxSoundFrameSize = 512;  // the encoder never writes more (fits next to STP1 + STJ1)
inline constexpr std::uint16_t kMaxSoundAgeMs = 2000, kMaxSoundFadeMs = 10000;
inline constexpr std::size_t SoundItemSize(unsigned nrtpc) { return kSoundItemSize + nrtpc * kSoundRtpcSize; }

enum class SoundOp : std::uint8_t { post = 0, stop = 1, action = 2, rtpc = 3 };
inline constexpr std::uint8_t kSoundOpMax = 3;
// Where on the body the sound is (the joint the receiver's game object follows); 7: on the partner's vehicle (v2).
enum class SoundSlot : std::uint8_t { head = 0, foot_l = 1, foot_r = 2, hand_r = 3, hand_l = 4, back = 5, hips = 6, vehicle = 7 };
inline constexpr unsigned kSoundSlots = 8;
inline constexpr std::uint8_t kSoundSlotVehicle = 7;
inline constexpr std::uint8_t kSoundActionMax = 4;  // AkActionOnEventType: stop, pause, resume, break, release envelope
// What made the sound on the sender (counters on both sides; the vehicle category has its own allow-list and place).
enum class SoundCategory : std::uint8_t { foley = 0, steps = 1, weapon = 2, cargo = 3, gear = 4, vehicle = 5 };
inline constexpr unsigned kSoundCategories = 6;
inline constexpr std::uint8_t kSoundCatVehicle = 5;
inline constexpr std::uint8_t kSoundHeld = 0x01;  // item flags (v2): a held vehicle loop (refreshed, leased)
inline constexpr double kSoundHeldRefreshMs = 1500, kSoundHeldLeaseMs = 5000;

struct SoundItem {
    std::uint32_t a = 0, b = 0;
    std::uint8_t op = 0, slot = 0, tag = 0, nrtpc = 0;
    std::uint16_t age_ms = 0, fade_ms = 0;
    std::int16_t off[3] = {};
    std::uint8_t category = 0;
    std::uint8_t flags = 0;  // v2: kSoundHeld
    std::uint32_t rtpc_id[kMaxSoundRtpcs] = {};
    float rtpc_value[kMaxSoundRtpcs] = {};
};
struct SoundFrame {
    std::uint16_t seq = 0;
    std::uint32_t sender_ms = 0;
    std::uint8_t n = 0;
    SoundItem item[kMaxSoundItems];
};
// The size the frame encodes to (0 when an item is invalid or n is 0 / > kMaxSoundItems).
std::size_t SoundFrameSize(const SoundFrame& f) noexcept;
// Returns the size, 0 when it cannot be encoded (see DecodeSound), cap is too small or it would exceed
// kMaxSoundFrameSize.
std::size_t EncodeSound(const SoundFrame& f, std::uint8_t* out, std::size_t cap) noexcept;
// One item as EncodeSound / DecodeSound check it (the sender drops an invalid item alone, not its whole frame).
bool SoundItemValid(const SoundItem& it) noexcept;
// Rejects bad magic, another version (1 included), n = 0 or > 32, a size that does not match the items exactly or
// exceeds kMaxSoundFrameSize, an unknown op, a slot >= 8 (post / rtpc) or an action > 4, nrtpc > 12, an RTPC count the
// op does not allow, event / playing ids the op needs that are 0, fields the op does not use that are not 0, age >
// 2,000 ms, fade > 10,000 ms, category >= 6, a post / rtpc item on slot 7 whose category is not vehicle (or the other
// way round), unknown flag bits, kSoundHeld on anything but a vehicle post, an RTPC id 0, and a non-finite or huge
// (|v| > 1e7) RTPC value.
bool DecodeSound(const std::uint8_t* data, std::size_t size, SoundFrame& out) noexcept;

// ---------------------------------------------------------------- vehicle frame (STM1)
// The vehicle the sender rides (or has just left), docs/real-partner-body.md section 22: its kind and drivable
// resource, its world transform, the sender's Sam relative to it (the seat) and, optionally, its skinned pose
// (wheels, steering, suspension). A FRAME message in a datagram of its own, at the pose send rate while a vehicle
// is reported; `sender_ms` is on the same axis as STP1's (the receiver plays it at the pose play time).
//
//   'STM1' | u8 version (2) | u8 kind | u8 state | u8 flags | u32 seq | u32 sender_ms |
//   16-byte resource GUID (raw, as the game's StreamingRef holds it) | f64 x, y, z | 6-byte rotation |
//   f32 speed (m/s) | i16 seat x, y, z (1 mm) | 6-byte seat rotation | u32 skeleton layout hash |
//   u8 n | u8 m                                                                            (84 bytes)
//   [flags & kVehJoints] n x { 6-byte rotation | i16 x, y, z (1 mm) }                   (12 bytes each)
//   [flags & kVehPacks]  m x { u8 pack (1 battery, 2 defense) | u8 0 | i16 x, y, z (1 mm) | 6-byte rotation }
//                                                                                        (14 bytes each)
//   [flags & kVehState]  u8 nl | u8 0 | u16 wear (0..65535 = 0..1) | u64 wear parts[2] |
//                        nl x { u8 part | u8 light kind (1 intensity, 2 colour) | f16 v[3] }   (20 + 8 nl bytes)
//   [flags & kVehCargo]  u8 nc | u8 dropped | nc x { u8 model | u8 variation | i16 x, y, z (1 mm) | 6-byte rotation }
//                                                                                        (2 + 14 nc bytes)
// Rotations are STJ1's smallest-three. The seat is the sender's Sam in the vehicle's frame (zero when not seated).
// Version 2 (after v0.12): the packs block (the battery / defense packs the game shows on the vehicle, its child
// entities, each in the vehicle's frame); the byte after n is their count m (it was 0). Version 1 frames are refused
// (mixed builds never meet: the HELLO build check).
// Version 3 (after v0.13, docs/real-partner-body.md section 22.12): the state block (in every frame: the vehicle's wear,
// the timefall corrosion shader variable 0x32D5AE44, one value on the ArtParts the bits name; and its lights, the
// emissive intensity 0x2B11751D and the battery gauge's colour 0x0978E0A1 per ArtParts index: head / tail / brake
// lamps, indicators, the battery gauge, burst) and the cargo block (the pieces in its DSBaggage2 carrier, each in the
// vehicle's frame; sent when they change and every second, absent otherwise: the receiver keeps the last list; a block
// with nc 0 means no cargo). A frame is never larger than kMaxVehicleFrameSize. Version 2 frames are refused.
inline constexpr std::uint8_t kVehicleMagic[4] = {'S', 'T', 'M', '1'};
inline constexpr std::uint8_t kVehicleVersion = 3;
inline constexpr std::size_t kVehicleHeaderSize = 84;
inline constexpr unsigned kMaxVehicleJoints = 64;
inline constexpr unsigned kMaxVehiclePacks = 8;
inline constexpr std::size_t kVehiclePackSize = 14;
inline constexpr float kMaxVehiclePackM = 8.0f;  // |pack offset| per axis
inline constexpr std::size_t kVehicleStateHeaderSize = 20, kVehicleLightSize = 8;
inline constexpr unsigned kMaxVehicleLights = 24;
inline constexpr unsigned kMaxVehicleWearParts = 128;  // ArtParts indices the wear bits can name
inline constexpr std::size_t kVehicleCargoHeaderSize = 2, kVehicleCargoSize = 14;
inline constexpr unsigned kMaxVehicleCargo = 32;
inline constexpr float kMaxVehicleCargoM = 8.0f;          // |piece offset| per axis, in the vehicle's frame
inline constexpr std::uint8_t kVehicleCargoModels = 31;   // EDSBaggage2Model 0..30 (the baggage config's ModelResources)
inline constexpr std::uint8_t kVehicleCargoVariations = 128;  // the baggage config's colour variation index, or kGearNone
inline constexpr float kMaxVehicleLightIntensity = 4096.0f;  // 0x2B11751D (the census: 0, 20, 50)
inline constexpr float kMaxVehicleLightColour = 64.0f;       // 0x0978E0A1 per channel (the census: 0.01 .. 1.0)
// The total frame (the link body less one message header and some room); the encoder never writes more.
inline constexpr std::size_t kMaxVehicleFrameSize = 1140;
inline constexpr std::size_t VehicleFrameSize(unsigned n, unsigned m = 0, int nl = -1, int nc = -1) {
    return kVehicleHeaderSize + n * 12u + m * kVehiclePackSize +
           (nl >= 0 ? kVehicleStateHeaderSize + static_cast<unsigned>(nl) * kVehicleLightSize : 0) +
           (nc >= 0 ? kVehicleCargoHeaderSize + static_cast<unsigned>(nc) * kVehicleCargoSize : 0);
}
inline constexpr float kMaxVehicleSeatM = 4.0f;    // |seat offset| per axis
inline constexpr float kMaxVehicleJointM = 32.0f;  // |joint translation| per axis
inline constexpr std::uint8_t kVehicleStateMax = 15;
// The game's EVehicleType (VehicleEntity +0x4C4).
enum class VehicleKind : std::uint8_t { none = 0, speedcar = 1, truck = 2, motorbike = 3 };
enum VehicleFlags : std::uint8_t {
    kVehSeated = 1u << 0,  // the sender's Sam has this vehicle as its parent entity (the seat is meaningful)
    kVehJoints = 1u << 1,  // the joint block follows (n > 0)
    kVehPacks = 1u << 2,   // version 2: the packs block follows the joints (m > 0)
    kVehState = 1u << 3,   // version 3: the state block (wear, lights) follows the packs
    kVehCargo = 1u << 4,   // version 3: the cargo block follows the state block (nc may be 0: no cargo)
    kVehKnown = kVehSeated | kVehJoints | kVehPacks | kVehState | kVehCargo,
};
// The shader variables the state block carries (gear_look reads and writes them; their hashes are the game's).
inline constexpr std::uint32_t kVehWearHash = 0x32D5AE44u;     // timefall corrosion, one value per vehicle (0..1)
inline constexpr std::uint32_t kVehLightHash = 0x2B11751Du;    // emissive intensity per ArtParts index
inline constexpr std::uint32_t kVehGaugeHash = 0x0978E0A1u;    // the battery gauge's emissive colour (rgb)
enum class VehicleLightKind : std::uint8_t { intensity = 1, colour = 2 };
struct VehicleLight {
    std::uint8_t part = 0;  // ArtParts index (< kMaxVehicleWearParts)
    std::uint8_t kind = 1;  // VehicleLightKind
    float v[3] = {};        // intensity: v[0] (v[1], v[2] 0); colour: rgb
};
struct VehicleCargo {
    std::uint8_t model = 0;               // EDSBaggage2Model (< kVehicleCargoModels)
    std::uint8_t variation = kGearNone;   // the baggage config's colour variation index, kGearNone = the prop's own
    float t[3] = {};                      // in the vehicle's frame (m)
    Quat q;
};
// IEEE half floats (the state block's values): round to nearest; values beyond the half range become infinity.
std::uint16_t FloatToHalf(float v) noexcept;
float HalfToFloat(std::uint16_t h) noexcept;
// The packs the game shows on a vehicle (child entities: ds/entities/vehicle/pe_ds_vehicle_battery_pack and
// pe_ds_vehicle_defense_pack); 0 is not a pack.
enum class VehiclePack : std::uint8_t { none = 0, battery = 1, defense = 2 };
inline constexpr std::uint8_t kVehiclePackMax = 2;
struct VehicleFrame {
    std::uint32_t seq = 0;
    std::uint32_t sender_ms = 0;
    std::uint8_t kind = 0;   // VehicleKind 1..3
    std::uint8_t state = 0;  // the vehicle's ride state (+0x4CC: 0 parked, 1 getting on, 2 riding, 3 getting off, 5 broken ...)
    std::uint8_t flags = 0;
    std::uint8_t guid[16] = {};
    double x = 0, y = 0, z = 0;
    Quat rot;
    float speed = 0;
    float seat[3] = {};
    Quat seat_rot;
    std::uint32_t layout = 0;  // the vehicle's skeleton layout hash (0: unknown)
    std::uint8_t n = 0;        // joints in the block (0 without kVehJoints)
    Quat q[kMaxVehicleJoints];
    float t[kMaxVehicleJoints][3] = {};
    std::uint8_t m = 0;  // packs in the block (0 without kVehPacks)
    std::uint8_t pack[kMaxVehiclePacks] = {};  // VehiclePack 1..2
    float pack_t[kMaxVehiclePacks][3] = {};    // in the vehicle's frame (m)
    Quat pack_q[kMaxVehiclePacks];
    // kVehState (version 3): wear on the parts whose bits are set (no bits: wear 0), the lights
    std::uint16_t wear = 0;
    std::uint64_t wear_parts[2] = {};
    std::uint8_t nl = 0;
    VehicleLight light[kMaxVehicleLights];
    // kVehCargo (version 3): the pieces in the vehicle's carrier (nc 0: none), `dropped` the ones the sender left out
    std::uint8_t nc = 0, cargo_dropped = 0;
    VehicleCargo cargo[kMaxVehicleCargo];
};
// The frame's encoded size (0 when its counts are over the caps).
std::size_t VehicleFrameBytes(const VehicleFrame& f) noexcept;
// The drivable vehicle resources a frame may name (decoded from the game data; docs/real-partner-body.md section 22).
struct VehicleResource {
    const char* key;
    const char* path;
    const char* guid;      // the root resource GUID (text; raw bytes = uuid.bytes_le)
    VehicleKind kind;
    std::uint32_t layout;  // its skeleton's layout hash (0: not known)
};
inline constexpr unsigned kVehicleResourceCount = 5;
const VehicleResource* VehicleResources() noexcept;  // kVehicleResourceCount entries
// The entry whose GUID is `guid` (raw bytes), nullptr when none.
const VehicleResource* FindVehicleResource(const std::uint8_t (&guid)[16]) noexcept;
// Returns the size, 0 when the frame cannot be encoded (any check of DecodeVehicle fails or cap is too small).
std::size_t EncodeVehicle(const VehicleFrame& f, std::uint8_t* out, std::size_t cap) noexcept;
// Rejects bad magic, another version, a size other than the blocks' (VehicleFrameBytes) or over kMaxVehicleFrameSize,
// a kind outside 1..3, a state above 15,
// unknown flag bits, joints flag without joints (or joints without the flag), n over the cap, the packs flag without
// packs (or packs without it), m over the cap, a pack kind outside 1..2, a pack's reserved byte that is not 0, a pack
// offset beyond +-8 m, a GUID that is not a drivable resource of that kind (the allow-list above), NaN / inf anywhere, |coord| > 1e7,
// |speed| > 1000, a seat offset beyond +-4 m (or one at all when not seated), a joint translation beyond +-32 m, and a
// joint block whose layout hash is 0. Version 3: a state block (or state values without its flag) with nl over 24, a
// reserved byte not 0, wear without wear bits, a light on part 128 or more, a light kind outside 1..2, a light (part,
// kind) twice, an intensity outside 0..4096 or with v[1] / v[2] not 0, a colour channel outside 0..64, a non-finite
// half; a cargo block (or pieces without its flag) with nc over 32, a model of 31 or more, a variation of 128 or more
// (but 0xFF), an offset beyond +-8 m, a non-finite rotation.
bool DecodeVehicle(const std::uint8_t* data, std::size_t size, VehicleFrame& out) noexcept;

}  // namespace st::wire
