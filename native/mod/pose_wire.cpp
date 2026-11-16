#include "pose_wire.hpp"

#include <cmath>
#include <cstring>

namespace st::wire {
namespace {

template <class T>
void Put(std::uint8_t*& p, T v) {
    std::memcpy(p, &v, sizeof(T));
    p += sizeof(T);
}
template <class T>
T Get(const std::uint8_t*& p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return v;
}

bool Finite(double v) { return std::isfinite(v) != 0; }

}  // namespace

std::size_t EncodePose(const Pose& pose, std::uint8_t (&out)[kPoseFrameSize]) noexcept {
    std::uint8_t* p = out;
    std::memcpy(p, kPoseMagic, 4);
    p += 4;
    Put(p, pose.seq);
    Put(p, pose.sender_ms);
    Put(p, pose.flags);
    Put(p, pose.x);
    Put(p, pose.y);
    Put(p, pose.z);
    Put(p, pose.rot.x);
    Put(p, pose.rot.y);
    Put(p, pose.rot.z);
    Put(p, pose.rot.w);
    Put(p, pose.speed);
    return static_cast<std::size_t>(p - out);
}

bool DecodePose(const std::uint8_t* data, std::size_t size, Pose& out) noexcept {
    if (!data || size != kPoseFrameSize || std::memcmp(data, kPoseMagic, 4) != 0) return false;
    const std::uint8_t* p = data + 4;
    Pose v;
    v.seq = Get<std::uint32_t>(p);
    v.sender_ms = Get<std::uint32_t>(p);
    v.flags = Get<std::uint32_t>(p);
    v.x = Get<double>(p);
    v.y = Get<double>(p);
    v.z = Get<double>(p);
    v.rot.x = Get<float>(p);
    v.rot.y = Get<float>(p);
    v.rot.z = Get<float>(p);
    v.rot.w = Get<float>(p);
    v.speed = Get<float>(p);
    const double lim = 1e7;
    if (!Finite(v.x) || !Finite(v.y) || !Finite(v.z) || std::fabs(v.x) > lim || std::fabs(v.y) > lim ||
        std::fabs(v.z) > lim)
        return false;
    const double n = double(v.rot.x) * v.rot.x + double(v.rot.y) * v.rot.y + double(v.rot.z) * v.rot.z +
                     double(v.rot.w) * v.rot.w;
    if (!Finite(n) || n < 0.9 || n > 1.1 || !Finite(v.speed)) return false;
    out = v;
    return true;
}

Quat Normalize(const Quat& q) noexcept {
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(n > 1e-6f)) return Quat{};
    return Quat{q.x / n, q.y / n, q.z / n, q.w / n};
}

// Standard Shepperd conversion for a rotation matrix R (applied as R * v with
// m[row][col]). Round-trips with MatrixFromQuat whatever Decima's handedness.
Quat QuatFromMatrix(const float (&m)[3][3]) noexcept {
    Quat q;
    const float tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0.0f) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m[2][1] - m[1][2]) / s;
        q.y = (m[0][2] - m[2][0]) / s;
        q.z = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        q.w = (m[2][1] - m[1][2]) / s;
        q.x = 0.25f * s;
        q.y = (m[0][1] + m[1][0]) / s;
        q.z = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        const float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        q.w = (m[0][2] - m[2][0]) / s;
        q.x = (m[0][1] + m[1][0]) / s;
        q.y = 0.25f * s;
        q.z = (m[1][2] + m[2][1]) / s;
    } else {
        const float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        q.w = (m[1][0] - m[0][1]) / s;
        q.x = (m[0][2] + m[2][0]) / s;
        q.y = (m[1][2] + m[2][1]) / s;
        q.z = 0.25f * s;
    }
    return Normalize(q);
}

void MatrixFromQuat(const Quat& in, float (&m)[3][3]) noexcept {
    const Quat q = Normalize(in);
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    m[0][0] = 1 - 2 * (yy + zz);
    m[0][1] = 2 * (xy - wz);
    m[0][2] = 2 * (xz + wy);
    m[1][0] = 2 * (xy + wz);
    m[1][1] = 1 - 2 * (xx + zz);
    m[1][2] = 2 * (yz - wx);
    m[2][0] = 2 * (xz - wy);
    m[2][1] = 2 * (yz + wx);
    m[2][2] = 1 - 2 * (xx + yy);
}

Quat Slerp(const Quat& a, const Quat& bin, float t) noexcept {
    Quat b = bin;
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) {
        d = -d;
        b = Quat{-b.x, -b.y, -b.z, -b.w};
    }
    if (d > 0.9995f) {  // nearly identical: nlerp
        return Normalize(Quat{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z), a.w + t * (b.w - a.w)});
    }
    const float th = std::acos(d);
    const float s = std::sin(th);
    const float wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
    return Quat{wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z, wa * a.w + wb * b.w};
}

}  // namespace st::wire
