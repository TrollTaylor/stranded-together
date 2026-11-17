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
    kPoseInVehicle = 1u << 1, // reserved
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

}  // namespace st::wire
