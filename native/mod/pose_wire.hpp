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
inline constexpr std::uint8_t kJointMagic[4] = {'S', 'T', 'J', '1'};
inline constexpr unsigned kMaxJoints = 80;  // Sam's layout has 77
inline constexpr unsigned kMaxJointTranslations = 4;
inline constexpr std::uint32_t kSamLayoutHash = 0xBE39C83Fu;
inline constexpr std::size_t kJointHeaderSize = 20;
inline constexpr std::size_t JointFrameSize(unsigned joints, unsigned translations) {
    return kJointHeaderSize + joints * 6u + 1u + translations * 7u;
}
inline constexpr std::size_t kMaxJointFrameSize = JointFrameSize(kMaxJoints, kMaxJointTranslations);

enum JointFlags : std::uint8_t {
    kJointsValid = 1u << 0,
};

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
};

// Returns the encoded size (0 when the frame cannot be encoded: too many joints, a
// translation joint out of range, NaN). `out` should hold kMaxJointFrameSize bytes.
std::size_t EncodeJoints(const JointFrame& f, std::uint8_t* out, std::size_t cap) noexcept;
// Rejects bad magic, a size that does not match the counts, joint_count 0 or above
// kMaxJoints, a translation joint >= joint_count.
bool DecodeJoints(const std::uint8_t* data, std::size_t size, JointFrame& out) noexcept;
// Per joint nlerp with the hemisphere fix (q and -q are the same rotation), then
// renormalised; translations lerped when both frames carry the same joints (else b's).
void BlendJoints(const JointFrame& a, const JointFrame& b, float t, JointFrame& out) noexcept;
// Angle in degrees between two rotations (0..180).
float AngleBetween(const Quat& a, const Quat& b) noexcept;

}  // namespace st::wire
