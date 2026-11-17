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

// ---------------------------------------------------------------- STJ1
namespace {
constexpr float kInvSqrt2 = 0.70710678118f;
constexpr float kQScale = 16383.0f / kInvSqrt2;

bool FiniteQ(const Quat& q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}

std::uint64_t PackQuat(const Quat& in) {
    const Quat q = Normalize(in);
    const float c[4] = {q.x, q.y, q.z, q.w};
    unsigned big = 0;
    for (unsigned i = 1; i < 4; ++i)
        if (std::fabs(c[i]) > std::fabs(c[big])) big = i;
    const float sign = c[big] < 0 ? -1.0f : 1.0f;
    std::uint64_t bits = big;
    unsigned shift = 2;
    for (unsigned i = 0; i < 4; ++i) {
        if (i == big) continue;
        float v = c[i] * sign * kQScale;
        if (v > 16383.0f) v = 16383.0f;
        if (v < -16383.0f) v = -16383.0f;
        const long r = std::lround(v) + 16384;
        bits |= static_cast<std::uint64_t>(r & 0x7FFF) << shift;
        shift += 15;
    }
    return bits;
}

Quat UnpackQuat(std::uint64_t bits) {
    const unsigned big = static_cast<unsigned>(bits & 3);
    float c[4] = {};
    unsigned shift = 2;
    float sum = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (i == big) continue;
        const long r = static_cast<long>((bits >> shift) & 0x7FFF) - 16384;
        c[i] = static_cast<float>(r) / kQScale;
        sum += c[i] * c[i];
        shift += 15;
    }
    c[big] = sum < 1.0f ? std::sqrt(1.0f - sum) : 0.0f;
    return Normalize(Quat{c[0], c[1], c[2], c[3]});
}
}  // namespace

std::size_t EncodeJoints(const JointFrame& f, std::uint8_t* out, std::size_t cap) noexcept {
    if (!out || f.joint_count == 0 || f.joint_count > kMaxJoints || f.t_count > kMaxJointTranslations) return 0;
    const std::size_t size = JointFrameSize(f.joint_count, f.t_count);
    if (cap < size) return 0;
    std::uint8_t* p = out;
    std::memcpy(p, kJointMagic, 4);
    p += 4;
    Put(p, f.seq);
    Put(p, f.sender_ms);
    Put(p, f.layout_hash);
    Put(p, f.joint_count);
    Put(p, f.body_id);
    Put(p, f.flags);
    Put(p, std::uint8_t{0});
    for (unsigned j = 0; j < f.joint_count; ++j) {
        if (!FiniteQ(f.q[j])) return 0;
        const std::uint64_t b = PackQuat(f.q[j]);
        for (unsigned k = 0; k < 6; ++k) *p++ = static_cast<std::uint8_t>(b >> (8 * k));
    }
    Put(p, f.t_count);
    for (unsigned i = 0; i < f.t_count; ++i) {
        if (f.t_joint[i] >= f.joint_count) return 0;
        Put(p, f.t_joint[i]);
        for (unsigned a = 0; a < 3; ++a) {
            if (!std::isfinite(f.t[i][a])) return 0;
            float v = f.t[i][a] * 10000.0f;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32767.0f) v = -32767.0f;
            Put(p, static_cast<std::int16_t>(std::lround(v)));
        }
    }
    return static_cast<std::size_t>(p - out);
}

bool DecodeJoints(const std::uint8_t* data, std::size_t size, JointFrame& out) noexcept {
    if (!data || size < kJointHeaderSize + 1 || std::memcmp(data, kJointMagic, 4) != 0) return false;
    const std::uint8_t* p = data + 4;
    JointFrame f;
    f.seq = Get<std::uint32_t>(p);
    f.sender_ms = Get<std::uint32_t>(p);
    f.layout_hash = Get<std::uint32_t>(p);
    f.joint_count = Get<std::uint8_t>(p);
    f.body_id = Get<std::uint8_t>(p);
    f.flags = Get<std::uint8_t>(p);
    Get<std::uint8_t>(p);
    if (f.joint_count == 0 || f.joint_count > kMaxJoints) return false;
    if (size < kJointHeaderSize + f.joint_count * 6u + 1u) return false;
    for (unsigned j = 0; j < f.joint_count; ++j) {
        std::uint64_t b = 0;
        for (unsigned k = 0; k < 6; ++k) b |= static_cast<std::uint64_t>(*p++) << (8 * k);
        f.q[j] = UnpackQuat(b);
    }
    f.t_count = Get<std::uint8_t>(p);
    if (f.t_count > kMaxJointTranslations || size != JointFrameSize(f.joint_count, f.t_count)) return false;
    for (unsigned i = 0; i < f.t_count; ++i) {
        f.t_joint[i] = Get<std::uint8_t>(p);
        if (f.t_joint[i] >= f.joint_count) return false;
        for (unsigned a = 0; a < 3; ++a) f.t[i][a] = static_cast<float>(Get<std::int16_t>(p)) / 10000.0f;
    }
    out = f;
    return true;
}

void BlendJoints(const JointFrame& a, const JointFrame& b, float t, JointFrame& out) noexcept {
    if (!(t >= 0.0f)) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const unsigned n = a.joint_count < b.joint_count ? a.joint_count : b.joint_count;
    bool same_t = a.t_count == b.t_count;
    for (unsigned i = 0; same_t && i < a.t_count; ++i) same_t = a.t_joint[i] == b.t_joint[i];
    float tr[kMaxJointTranslations][3] = {};
    for (unsigned i = 0; i < b.t_count; ++i)
        for (unsigned k = 0; k < 3; ++k) tr[i][k] = same_t ? a.t[i][k] + t * (b.t[i][k] - a.t[i][k]) : b.t[i][k];
    Quat q[kMaxJoints];
    for (unsigned j = 0; j < n; ++j) {
        const Quat& qa = a.q[j];
        Quat qb = b.q[j];
        if (qa.x * qb.x + qa.y * qb.y + qa.z * qb.z + qa.w * qb.w < 0) qb = Quat{-qb.x, -qb.y, -qb.z, -qb.w};
        q[j] = Normalize(Quat{qa.x + t * (qb.x - qa.x), qa.y + t * (qb.y - qa.y), qa.z + t * (qb.z - qa.z),
                              qa.w + t * (qb.w - qa.w)});
    }
    // out may alias a or b: everything was computed first.
    out.seq = b.seq;
    out.sender_ms = b.sender_ms;
    out.layout_hash = b.layout_hash;
    out.body_id = b.body_id;
    out.flags = b.flags;
    out.joint_count = static_cast<std::uint8_t>(n);
    for (unsigned j = 0; j < n; ++j) out.q[j] = q[j];
    out.t_count = b.t_count;
    for (unsigned i = 0; i < b.t_count; ++i) {
        out.t_joint[i] = b.t_joint[i];
        for (unsigned k = 0; k < 3; ++k) out.t[i][k] = tr[i][k];
    }
}

float AngleBetween(const Quat& a, const Quat& b) noexcept {
    // Relative rotation conj(a) * b, angle = 2 atan2(|xyz|, |w|): exact near 0 (acos is not).
    const double ax = -a.x, ay = -a.y, az = -a.z, aw = a.w;
    const double rw = aw * b.w - ax * b.x - ay * b.y - az * b.z;
    const double rx = aw * b.x + ax * b.w + ay * b.z - az * b.y;
    const double ry = aw * b.y - ax * b.z + ay * b.w + az * b.x;
    const double rz = aw * b.z + ax * b.y - ay * b.x + az * b.w;
    const double v = std::sqrt(rx * rx + ry * ry + rz * rz);
    return static_cast<float>(2.0 * std::atan2(v, std::fabs(rw)) * 57.29577951308232);
}

}  // namespace st::wire
