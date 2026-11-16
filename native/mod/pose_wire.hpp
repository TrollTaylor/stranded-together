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

}  // namespace st::wire
