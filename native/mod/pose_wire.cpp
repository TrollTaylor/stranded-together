#include "pose_wire.hpp"

#include <cmath>
#include <cstdio>
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
    const bool outfit = (f.flags & kJointsOutfit) != 0;
    const std::size_t size = JointFrameSize(f.joint_count, f.t_count, outfit);
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
    if (outfit) {
        const Outfit& o = f.outfit;
        const std::uint8_t b[kOutfitSize] = {o.variant,        o.exos,           o.suit_colour,     o.cap_colour,
                                             o.glasses_a_lens, o.glasses_b_lens, o.glasses_a_frame, o.glasses_b_frame};
        for (std::uint8_t v : b) Put(p, v);
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
    const bool outfit = (f.flags & kJointsOutfit) != 0;
    // Flag bits this build does not know announce blocks after the known ones (a newer build):
    // the frame may then be longer, and the rest is ignored. Known flags only: the exact size.
    const bool unknown = (f.flags & ~kJointsKnown) != 0;
    const std::size_t want = JointFrameSize(f.joint_count, f.t_count, outfit);
    if (f.t_count > kMaxJointTranslations || (unknown ? size < want : size != want)) return false;
    f.flags = static_cast<std::uint8_t>(f.flags & kJointsKnown);
    for (unsigned i = 0; i < f.t_count; ++i) {
        f.t_joint[i] = Get<std::uint8_t>(p);
        if (f.t_joint[i] >= f.joint_count) return false;
        for (unsigned a = 0; a < 3; ++a) f.t[i][a] = static_cast<float>(Get<std::int16_t>(p)) / 10000.0f;
    }
    if (outfit) {
        Outfit& o = f.outfit;
        std::uint8_t* b[kOutfitSize] = {&o.variant,        &o.exos,           &o.suit_colour,     &o.cap_colour,
                                        &o.glasses_a_lens, &o.glasses_b_lens, &o.glasses_a_frame, &o.glasses_b_frame};
        for (std::uint8_t* v : b) *v = Get<std::uint8_t>(p);
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
    out.outfit = b.outfit;
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

// ---------------------------------------------------------------- STN1
namespace {

// One code point from in[i..n): its value and length, or 0 for an invalid sequence (overlong,
// surrogate, above U+10FFFF, truncated, stray continuation byte).
unsigned NextCodepoint(const unsigned char* in, std::size_t i, std::size_t n, std::uint32_t& cp) {
    const unsigned c = in[i];
    if (c < 0x80) {
        cp = c;
        return 1;
    }
    unsigned len = 0;
    std::uint32_t v = 0, min = 0;
    if ((c & 0xE0) == 0xC0) len = 2, v = c & 0x1F, min = 0x80;
    else if ((c & 0xF0) == 0xE0) len = 3, v = c & 0x0F, min = 0x800;
    else if ((c & 0xF8) == 0xF0) len = 4, v = c & 0x07, min = 0x10000;
    else return 0;
    if (i + len > n) return 0;
    for (unsigned k = 1; k < len; ++k) {
        const unsigned d = in[i + k];
        if ((d & 0xC0) != 0x80) return 0;
        v = (v << 6) | (d & 0x3F);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return 0;
    cp = v;
    return len;
}

bool Invisible(std::uint32_t cp) {
    return cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F) || cp == 0x061C || (cp >= 0x200B && cp <= 0x200F) ||
           (cp >= 0x2028 && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x206F) || cp == 0xFEFF;
}

bool Space(std::uint32_t cp) { return cp == 0x20 || cp == 0xA0 || cp == 0x3000 || (cp >= 0x2000 && cp <= 0x200A); }

}  // namespace

std::size_t SanitizeName(const char* in_chars, std::size_t n, char* out, std::size_t cap) noexcept {
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!in_chars) return 0;
    const auto* in = reinterpret_cast<const unsigned char*>(in_chars);
    const std::size_t max_bytes = cap - 1 < kMaxNameBytes ? cap - 1 : kMaxNameBytes;
    std::size_t o = 0;
    unsigned cps = 0;
    bool pending_space = false;
    for (std::size_t i = 0; i < n && in[i];) {
        std::uint32_t cp = 0;
        const unsigned len = NextCodepoint(in, i, n, cp);
        if (!len) {  // drop one byte and resynchronise
            ++i;
            continue;
        }
        const std::size_t at = i;
        i += len;
        if (Invisible(cp)) continue;
        if (Space(cp)) {
            pending_space = o > 0;  // leading space is trimmed
            continue;
        }
        const std::size_t need = len + (pending_space ? 1u : 0u);
        const unsigned need_cps = 1u + (pending_space ? 1u : 0u);
        if (o + need > max_bytes || cps + need_cps > kMaxNameCodepoints) break;
        if (pending_space) {
            out[o++] = ' ';
            ++cps;
            pending_space = false;
        }
        std::memcpy(out + o, in + at, len);
        o += len;
        ++cps;
    }
    out[o] = 0;  // trailing space was never written
    return o;
}

std::size_t EncodeName(const NameFrame& f, std::uint8_t* out, std::size_t cap) noexcept {
    char clean[kMaxNameBytes + 1];
    const std::size_t len = SanitizeName(f.name, f.len < sizeof(f.name) ? f.len : sizeof(f.name) - 1, clean, sizeof(clean));
    const std::size_t size = kNameHeaderSize + len;
    if (!out || cap < size) return 0;
    std::memcpy(out, kNameMagic, 4);
    out[4] = kNameVersion;
    out[5] = static_cast<std::uint8_t>(f.flags & kNameKnown);
    out[6] = static_cast<std::uint8_t>(len);
    out[7] = 0;
    std::memcpy(out + kNameHeaderSize, clean, len);
    return size;
}

bool DecodeName(const std::uint8_t* data, std::size_t size, NameFrame& out) noexcept {
    if (!data || size < kNameHeaderSize || std::memcmp(data, kNameMagic, 4) != 0) return false;
    if (data[4] != kNameVersion) return false;
    const unsigned len = data[6];
    if (len > kMaxNameBytes || size != kNameHeaderSize + len) return false;
    NameFrame v;
    v.flags = static_cast<std::uint8_t>(data[5] & kNameKnown);
    v.len = static_cast<std::uint8_t>(SanitizeName(reinterpret_cast<const char*>(data + kNameHeaderSize), len, v.name, sizeof(v.name)));
    out = v;
    return true;
}

// ---------------------------------------------------------------- STC1
namespace {
constexpr unsigned kGearJoints = 77;  // Sam's skeleton (kSamLayoutHash)

bool GearItemOk(const GearItem& it, unsigned index) {
    const auto k = static_cast<std::uint8_t>(it.kind);
    if (k < 1 || k > kGearKindMax) return false;
    if (GearHasRes(it.kind) && it.res >= kMaxGearRes) return false;
    if (it.kind == GearKind::boots) return it.parent == kGearNoParent && it.joint == 0;
    if (it.parent == kGearNoParent) return it.joint < kGearJoints;
    return it.parent < index;
}
// The boots block goes with exactly one boots item (version 4).
bool GearBootsOk(const GearFrame& f) {
    unsigned boots = 0;
    for (unsigned i = 0; i < f.count && i < kMaxGearItems; ++i) boots += f.items[i].kind == GearKind::boots;
    const bool block = (f.flags & kGearBoots) != 0;
    if (boots > 1 || (boots == 1) != block) return false;
    return !block || (f.boots_visible & ~f.boots_apply) == 0;
}
}  // namespace

std::uint16_t GearLayoutHash(const GearFrame& f) noexcept {
    std::uint32_t h = 2166136261u;
    auto mix = [&h](std::uint8_t b) {
        h ^= b;
        h *= 16777619u;
    };
    mix(f.count);
    for (unsigned i = 0; i < f.count && i < kMaxGearItems; ++i) {
        const GearItem& it = f.items[i];
        mix(static_cast<std::uint8_t>(it.kind));
        mix(it.model);
        mix(it.variation);
        mix(it.parent == kGearNoParent ? it.joint : std::uint8_t{0});
        mix(it.parent);
        mix(GearHasRes(it.kind) ? it.res : kGearNone);
    }
    return static_cast<std::uint16_t>((h >> 16) ^ (h & 0xFFFFu));
}

std::size_t EncodeGear(const GearFrame& f, std::uint8_t* out, std::size_t cap) noexcept {
    if (!out || f.count > kMaxGearItems) return 0;
    const bool parts = (f.flags & kGearParts) != 0, pack = (f.flags & kGearPackParts) != 0, boots = (f.flags & kGearBoots) != 0;
    const std::size_t size = GearFrameSize(f.count, parts, pack, boots);
    if (cap < size || !GearBootsOk(f)) return 0;
    for (unsigned i = 0; i < f.count; ++i) {
        const GearItem& it = f.items[i];
        if (!GearItemOk(it, i) || !FiniteQ(it.q)) return 0;
        for (float v : it.p)
            if (!std::isfinite(v) || std::fabs(v) > 32.0f) return 0;
    }
    std::uint8_t* p = out;
    std::memcpy(p, kGearMagic, 4);
    p += 4;
    Put(p, f.seq);
    Put(p, kGearVersion);
    Put(p, f.count);
    Put(p, static_cast<std::uint8_t>(f.flags & kGearKnown));
    Put(p, f.dropped);
    Put(p, GearLayoutHash(f));
    Put(p, std::uint16_t{0});
    if (parts) {
        for (unsigned k = 0; k < 2; ++k) Put(p, f.parts_apply[k]);
        for (unsigned k = 0; k < 2; ++k) Put(p, static_cast<std::uint64_t>(f.parts_visible[k] & f.parts_apply[k]));
    }
    if (pack) {
        Put(p, f.pack_apply);
        Put(p, static_cast<std::uint64_t>(f.pack_visible & f.pack_apply));
    }
    if (boots) {
        Put(p, f.boots_apply);
        Put(p, f.boots_visible);
    }
    for (unsigned i = 0; i < f.count; ++i) {
        const GearItem& it = f.items[i];
        Put(p, static_cast<std::uint8_t>(it.kind));
        Put(p, it.model);
        Put(p, it.variation);
        Put(p, it.parent == kGearNoParent ? it.joint : std::uint8_t{0});
        Put(p, it.parent);
        Put(p, GearHasRes(it.kind) ? it.res : kGearNone);
        for (float v : it.p) Put(p, static_cast<std::int16_t>(std::lround(v * 1000.0f)));
        const std::uint64_t b = PackQuat(it.q);
        for (unsigned k = 0; k < 6; ++k) *p++ = static_cast<std::uint8_t>(b >> (8 * k));
    }
    return static_cast<std::size_t>(p - out);
}

bool DecodeGear(const std::uint8_t* data, std::size_t size, GearFrame& out) noexcept {
    if (!data || size < kGearHeaderSize || std::memcmp(data, kGearMagic, 4) != 0) return false;
    const std::uint8_t* p = data + 4;
    static thread_local GearFrame f;
    f = GearFrame{};
    f.seq = Get<std::uint32_t>(p);
    if (Get<std::uint8_t>(p) != kGearVersion) return false;
    f.count = Get<std::uint8_t>(p);
    f.flags = static_cast<std::uint8_t>(Get<std::uint8_t>(p) & kGearKnown);
    f.dropped = Get<std::uint8_t>(p);
    const std::uint16_t hash = Get<std::uint16_t>(p);
    Get<std::uint16_t>(p);
    const bool parts = (f.flags & kGearParts) != 0, pack = (f.flags & kGearPackParts) != 0, boots = (f.flags & kGearBoots) != 0;
    if (f.count > kMaxGearItems || size != GearFrameSize(f.count, parts, pack, boots)) return false;
    if (parts) {
        for (unsigned k = 0; k < 2; ++k) f.parts_apply[k] = Get<std::uint64_t>(p);
        for (unsigned k = 0; k < 2; ++k) f.parts_visible[k] = Get<std::uint64_t>(p) & f.parts_apply[k];
    }
    if (pack) {
        f.pack_apply = Get<std::uint64_t>(p);
        f.pack_visible = Get<std::uint64_t>(p) & f.pack_apply;
    }
    if (boots) {
        f.boots_apply = Get<std::uint32_t>(p);
        f.boots_visible = Get<std::uint32_t>(p);
    }
    for (unsigned i = 0; i < f.count; ++i) {
        GearItem& it = f.items[i];
        it.kind = static_cast<GearKind>(Get<std::uint8_t>(p));
        it.model = Get<std::uint8_t>(p);
        it.variation = Get<std::uint8_t>(p);
        it.joint = Get<std::uint8_t>(p);
        it.parent = Get<std::uint8_t>(p);
        it.res = Get<std::uint8_t>(p);
        if (!GearHasRes(it.kind)) it.res = kGearNone;
        for (float& v : it.p) v = static_cast<float>(Get<std::int16_t>(p)) / 1000.0f;
        std::uint64_t b = 0;
        for (unsigned k = 0; k < 6; ++k) b |= static_cast<std::uint64_t>(*p++) << (8 * k);
        it.q = UnpackQuat(b);
        if (!GearItemOk(it, i)) return false;
        if (it.parent != kGearNoParent) it.joint = 0;
    }
    if (GearLayoutHash(f) != hash || !GearBootsOk(f)) return false;
    out = f;
    return true;
}

bool GearResPathOk(const char* path) noexcept {
    if (!path) return false;
    const std::size_t n = std::strlen(path);
    if (n < 13 || n > kMaxGearResPath || std::strncmp(path, "ds/entities/", 12) != 0 || std::strstr(path, "..")) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const char c = path[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '/' || c == '.' || c == '-')) return false;
    }
    return true;
}

void GearGuidText(const std::uint8_t (&g)[16], char (&out)[37]) noexcept {
    std::snprintf(out, sizeof(out), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
                  g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

bool GearResProven(const char* path, const std::uint8_t (&guid)[16], bool allow_pod_entity, const char** why) noexcept {
    static const char* const kPaths[] = {"ds/entities/item/backpack/backpack_temp", "ds/entities/player/ds_player_inventory",
                                         "ds/entities/weapons/weapon_collection", "ds/entities/weapons/sam_cant", "ds/entities/weapons/sam_boot"};
    // census 20261117-130343: the cuff device / talk entity, the baby / boots entity, the chiral
    // cylinder, the backpack belt
    static const char* const kDenied[] = {"c8061ffe-fa4f-dc45-9cc9-f7982abf247c", "89fbb2a5-ac54-c749-aa5f-d2a2c7950709",
                                          "f5033627-24f0-1441-a7d4-e7f1e90c68fa", "d2464e7e-23eb-e246-9dce-68601e706982"};
    static const char kPodEntity[] = "b5041f35-b5a5-564c-8b84-76587a06f97a";
    const char* dummy = nullptr;
    if (!why) why = &dummy;
    if (!GearResPathOk(path)) return (*why = "not a gear resource path"), false;
    bool known = std::strncmp(path, "ds/entities/item/backpack/", 26) == 0;
    for (const char* k : kPaths) known = known || std::strcmp(path, k) == 0;
    if (!known) return (*why = "not one of the proven gear resources"), false;
    char g[37];
    GearGuidText(guid, g);
    if (std::strcmp(g, kPodEntity) == 0) return allow_pod_entity ? true : ((*why = "the BB pod entity (drawn as the cinematic pod)"), false);
    for (const char* d : kDenied)
        if (std::strcmp(g, d) == 0) return (*why = "an entity the mirror policy refuses (player-bound components)"), false;
    return true;
}

bool GearResWeaponOnly(const char* path) noexcept {
    if (!path || std::strncmp(path, "ds/entities/weapons/", 20) != 0) return false;
    static const char* const kEntities[] = {"ds/entities/weapons/weapon_collection", "ds/entities/weapons/sam_cant", "ds/entities/weapons/sam_boot"};
    for (const char* k : kEntities)
        if (std::strcmp(path, k) == 0) return false;
    return true;
}

std::size_t EncodeGearRes(const GearRes& r, std::uint8_t* out, std::size_t cap) noexcept {
    if (!out || r.id >= kMaxGearRes || !GearResPathOk(r.path)) return 0;
    const std::size_t n = std::strlen(r.path);
    if (cap < kGearResHeaderSize + n) return 0;
    std::uint8_t* p = out;
    std::memcpy(p, kGearResMagic, 4);
    p += 4;
    Put(p, kGearResVersion);
    Put(p, r.id);
    Put(p, static_cast<std::uint8_t>(n));
    Put(p, std::uint8_t{0});
    std::memcpy(p, r.guid, 16);
    p += 16;
    std::memcpy(p, r.path, n);
    p += n;
    return static_cast<std::size_t>(p - out);
}

bool DecodeGearRes(const std::uint8_t* data, std::size_t size, GearRes& out) noexcept {
    if (!data || size < kGearResHeaderSize || std::memcmp(data, kGearResMagic, 4) != 0) return false;
    const std::uint8_t* p = data + 4;
    GearRes r;
    if (Get<std::uint8_t>(p) != kGearResVersion) return false;
    r.id = Get<std::uint8_t>(p);
    const std::size_t n = Get<std::uint8_t>(p);
    Get<std::uint8_t>(p);
    if (r.id >= kMaxGearRes || n > kMaxGearResPath || size != kGearResHeaderSize + n) return false;
    std::memcpy(r.guid, p, 16);
    p += 16;
    std::memcpy(r.path, p, n);
    r.path[n] = 0;
    if (!GearResPathOk(r.path)) return false;
    out = r;
    return true;
}

// ---------------------------------------------------------------- STV1
namespace {
bool LookOk(const GearLook& l) {
    const auto k = static_cast<std::uint8_t>(l.kind);
    if (l.item >= kMaxGearItems || k < 1 || k > kGearKindMax) return false;
    if (l.nvar > kMaxLookVariations || l.nsv > kMaxLookShaderVars) return false;
    for (unsigned i = 0; i < l.nsv; ++i) {
        const GearShaderVar& s = l.sv[i];
        if (s.count < 1 || s.count > 4 || s.part > kLookAllParts) return false;
        for (unsigned c = 0; c < s.count; ++c)
            if (!std::isfinite(s.v[c])) return false;
    }
    return true;
}
}  // namespace

std::size_t EncodeGearLook(const GearLook& l, std::uint8_t* out, std::size_t cap) noexcept {
    if (!out || !LookOk(l)) return 0;
    const std::size_t size = GearLookSize(l.nvar, l.nsv);
    if (cap < size) return 0;
    std::uint8_t* p = out;
    std::memcpy(p, kGearLookMagic, 4);
    p += 4;
    Put(p, kGearLookVersion);
    Put(p, l.item);
    Put(p, l.layout);
    Put(p, static_cast<std::uint8_t>(l.kind));
    Put(p, GearHasRes(l.kind) ? l.res : kGearNone);
    Put(p, l.nvar);
    Put(p, l.nsv);
    Put(p, l.parts_apply);
    Put(p, static_cast<std::uint64_t>(l.parts_visible & l.parts_apply));
    for (unsigned i = 0; i < l.nvar; ++i) {
        std::memcpy(p, l.guid[i], 16);
        p += 16;
    }
    for (unsigned i = 0; i < l.nsv; ++i) {
        const GearShaderVar& s = l.sv[i];
        Put(p, s.hash);
        Put(p, s.part);
        Put(p, s.count);
        Put(p, std::uint8_t{0});
        for (unsigned c = 0; c < 4; ++c) Put(p, c < s.count ? s.v[c] : 0.0f);
    }
    return static_cast<std::size_t>(p - out);
}

bool DecodeGearLook(const std::uint8_t* data, std::size_t size, GearLook& out) noexcept {
    if (!data || size < kGearLookHeaderSize || std::memcmp(data, kGearLookMagic, 4) != 0) return false;
    const std::uint8_t* p = data + 4;
    static thread_local GearLook l;
    l = GearLook{};
    if (Get<std::uint8_t>(p) != kGearLookVersion) return false;
    l.item = Get<std::uint8_t>(p);
    l.layout = Get<std::uint16_t>(p);
    l.kind = static_cast<GearKind>(Get<std::uint8_t>(p));
    l.res = Get<std::uint8_t>(p);
    l.nvar = Get<std::uint8_t>(p);
    l.nsv = Get<std::uint8_t>(p);
    if (l.nvar > kMaxLookVariations || l.nsv > kMaxLookShaderVars || size != GearLookSize(l.nvar, l.nsv)) return false;
    l.parts_apply = Get<std::uint64_t>(p);
    l.parts_visible = Get<std::uint64_t>(p) & l.parts_apply;
    if (!GearHasRes(l.kind)) l.res = kGearNone;
    for (unsigned i = 0; i < l.nvar; ++i) {
        std::memcpy(l.guid[i], p, 16);
        p += 16;
    }
    for (unsigned i = 0; i < l.nsv; ++i) {
        GearShaderVar& s = l.sv[i];
        s.hash = Get<std::uint32_t>(p);
        s.part = Get<std::uint16_t>(p);
        s.count = Get<std::uint8_t>(p);
        Get<std::uint8_t>(p);
        for (unsigned c = 0; c < 4; ++c) {
            const float v = Get<float>(p);
            s.v[c] = c < s.count ? v : 0.0f;
        }
    }
    if (!LookOk(l)) return false;
    out = l;
    return true;
}

std::uint32_t GearLookHash(const GearLook& l) noexcept {
    std::uint32_t h = 2166136261u;
    auto mix = [&h](std::uint32_t v) {
        for (unsigned k = 0; k < 4; ++k) {
            h ^= (v >> (8 * k)) & 0xFFu;
            h *= 16777619u;
        }
    };
    mix(static_cast<std::uint32_t>(l.parts_apply)), mix(static_cast<std::uint32_t>(l.parts_apply >> 32));
    const std::uint64_t vis = l.parts_visible & l.parts_apply;
    mix(static_cast<std::uint32_t>(vis)), mix(static_cast<std::uint32_t>(vis >> 32));
    mix(l.nvar);
    for (unsigned i = 0; i < l.nvar && i < kMaxLookVariations; ++i)
        for (unsigned k = 0; k < 16; k += 4)
            mix(static_cast<std::uint32_t>(l.guid[i][k]) | (static_cast<std::uint32_t>(l.guid[i][k + 1]) << 8) |
                (static_cast<std::uint32_t>(l.guid[i][k + 2]) << 16) | (static_cast<std::uint32_t>(l.guid[i][k + 3]) << 24));
    mix(l.nsv);
    for (unsigned i = 0; i < l.nsv && i < kMaxLookShaderVars; ++i) {
        const GearShaderVar& s = l.sv[i];
        mix(s.hash);
        mix(static_cast<std::uint32_t>(s.part) | (static_cast<std::uint32_t>(s.count) << 16));
        for (unsigned c = 0; c < s.count && c < 4; ++c) {
            const float v = s.v[c];
            const double r = std::isfinite(v) ? std::floor(static_cast<double>(v) * 256.0 + 0.5) : 0.0;
            const double lim = 2147483647.0;
            mix(static_cast<std::uint32_t>(static_cast<std::int32_t>(r > lim ? lim : r < -lim ? -lim : r)));
        }
    }
    return h;
}

// ---------------------------------------------------------------- STK1
std::size_t EncodeGearPose(const GearPose& g, std::uint8_t* out, std::size_t cap) noexcept {
    if (!out || g.item >= kMaxGearItems || g.n == 0 || g.n > kMaxGearPoseJoints) return 0;
    const std::size_t size = GearPoseSize(g.n);
    if (cap < size) return 0;
    for (unsigned j = 0; j < g.n; ++j) {
        if (!FiniteQ(g.q[j])) return 0;
        for (float v : g.t[j])
            if (!std::isfinite(v) || std::fabs(v) > 3.2f) return 0;
    }
    std::uint8_t* p = out;
    std::memcpy(p, kGearPoseMagic, 4);
    p += 4;
    Put(p, kGearPoseVersion);
    Put(p, g.item);
    Put(p, g.layout);
    Put(p, g.skeleton);
    Put(p, g.n);
    Put(p, std::uint8_t{0});
    Put(p, std::uint16_t{0});
    for (unsigned j = 0; j < g.n; ++j) {
        const std::uint64_t b = PackQuat(g.q[j]);
        for (unsigned k = 0; k < 6; ++k) *p++ = static_cast<std::uint8_t>(b >> (8 * k));
        for (float v : g.t[j]) Put(p, static_cast<std::int16_t>(std::lround(v * 10000.0f)));
    }
    return static_cast<std::size_t>(p - out);
}

bool DecodeGearPose(const std::uint8_t* data, std::size_t size, GearPose& out) noexcept {
    if (!data || size < kGearPoseHeaderSize || std::memcmp(data, kGearPoseMagic, 4) != 0) return false;
    const std::uint8_t* p = data + 4;
    static thread_local GearPose g;
    g = GearPose{};
    if (Get<std::uint8_t>(p) != kGearPoseVersion) return false;
    g.item = Get<std::uint8_t>(p);
    g.layout = Get<std::uint16_t>(p);
    g.skeleton = Get<std::uint32_t>(p);
    g.n = Get<std::uint8_t>(p);
    Get<std::uint8_t>(p);
    Get<std::uint16_t>(p);
    if (g.item >= kMaxGearItems || g.n == 0 || g.n > kMaxGearPoseJoints || size != GearPoseSize(g.n)) return false;
    for (unsigned j = 0; j < g.n; ++j) {
        std::uint64_t b = 0;
        for (unsigned k = 0; k < 6; ++k) b |= static_cast<std::uint64_t>(*p++) << (8 * k);
        g.q[j] = UnpackQuat(b);
        for (float& v : g.t[j]) v = static_cast<float>(Get<std::int16_t>(p)) / 10000.0f;
    }
    out = g;
    return true;
}

bool GearPoseDiff(const GearPose& a, const GearPose& b, float* max_m, float* max_deg) noexcept {
    if (a.n != b.n || a.skeleton != b.skeleton) return false;
    float dm = 0, dd = 0;
    for (unsigned j = 0; j < a.n && j < kMaxGearPoseJoints; ++j) {
        const float dx = a.t[j][0] - b.t[j][0], dy = a.t[j][1] - b.t[j][1], dz = a.t[j][2] - b.t[j][2];
        const float m = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float d = AngleBetween(a.q[j], b.q[j]);
        if (m > dm) dm = m;
        if (d > dd) dd = d;
    }
    if (max_m) *max_m = dm;
    if (max_deg) *max_deg = dd;
    return true;
}

bool GearPoseDelta(const GearFrame& a, const GearFrame& b, float* max_m, float* max_deg) noexcept {
    if (a.count != b.count || GearLayoutHash(a) != GearLayoutHash(b)) return false;
    float dm = 0, dd = 0;
    for (unsigned i = 0; i < a.count; ++i) {
        const GearItem& x = a.items[i];
        const GearItem& y = b.items[i];
        const float dx = x.p[0] - y.p[0], dy = x.p[1] - y.p[1], dz = x.p[2] - y.p[2];
        const float m = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float d = AngleBetween(x.q, y.q);
        if (m > dm) dm = m;
        if (d > dd) dd = d;
    }
    if (max_m) *max_m = dm;
    if (max_deg) *max_deg = dd;
    return true;
}

void GearAssignProps(const int* old_keys, unsigned old_n, const int* want, unsigned want_n, int* out) noexcept {
    constexpr unsigned kCap = 256;
    bool used[kCap] = {};
    if (old_n > kCap) old_n = kCap;
    for (unsigned j = 0; j < want_n; ++j) {
        out[j] = -1;
        if (want[j] >= 0 && j < old_n && old_keys[j] == want[j]) {
            out[j] = static_cast<int>(j);
            used[j] = true;
        }
    }
    for (unsigned j = 0; j < want_n; ++j) {
        if (out[j] >= 0 || want[j] < 0) continue;
        for (unsigned i = 0; i < old_n; ++i) {
            if (used[i] || old_keys[i] != want[j]) continue;
            // a slot whose own index still wants its key keeps it (handled above)
            out[j] = static_cast<int>(i);
            used[i] = true;
            break;
        }
    }
}

double GearRetryDelayMs(unsigned failures) noexcept {
    if (failures == 0) return 0;
    double d = 2000;
    for (unsigned i = 1; i < failures && d < 30000; ++i) d *= 2;
    return d > 30000 ? 30000 : d;
}

// ---------------------------------------------------------------- STS1
namespace {
bool SpeechOk(const Speech& s) {
    if (s.kind > kSpeechKindMax || s.n > kMaxSpeechSentences || s.seq == 0) return false;
    if (s.kind == static_cast<std::uint8_t>(SpeechKind::muted) ? s.n != 0 : s.n == 0) return false;
    for (unsigned i = 0; i < s.n; ++i) {
        bool any = false;
        for (std::uint8_t b : s.guid[i]) any |= b != 0;
        if (!any) return false;
    }
    return true;
}
}  // namespace

std::size_t EncodeSpeech(const Speech& s, std::uint8_t* out, std::size_t cap) noexcept {
    if (!out || !SpeechOk(s)) return 0;
    const std::size_t size = SpeechFrameSize(s.n);
    if (cap < size) return 0;
    std::uint8_t* p = out;
    std::memcpy(p, kSpeechMagic, 4);
    p += 4;
    Put(p, kSpeechVersion);
    Put(p, s.kind);
    Put(p, static_cast<std::uint8_t>(s.flags & kSpeechKnown));
    Put(p, s.n);
    Put(p, s.seq);
    Put(p, s.sender_ms);
    Put(p, s.situation);
    for (unsigned i = 0; i < s.n; ++i) {
        std::memcpy(p, s.guid[i], 16);
        p += 16;
    }
    return size;
}

bool DecodeSpeech(const std::uint8_t* data, std::size_t size, Speech& out) noexcept {
    if (!data || size < kSpeechHeaderSize || std::memcmp(data, kSpeechMagic, 4) != 0) return false;
    const std::uint8_t* p = data + 4;
    if (Get<std::uint8_t>(p) != kSpeechVersion) return false;
    Speech s;
    s.kind = Get<std::uint8_t>(p);
    s.flags = static_cast<std::uint8_t>(Get<std::uint8_t>(p) & kSpeechKnown);
    s.n = Get<std::uint8_t>(p);
    if (s.n > kMaxSpeechSentences || size != SpeechFrameSize(s.n)) return false;
    s.seq = Get<std::uint32_t>(p);
    s.sender_ms = Get<std::uint32_t>(p);
    s.situation = Get<std::uint32_t>(p);
    for (unsigned i = 0; i < s.n; ++i) {
        std::memcpy(s.guid[i], p, 16);
        p += 16;
    }
    if (!SpeechOk(s)) return false;
    out = s;
    return true;
}

// ---------------------------------------------------------------- STA1
namespace {
bool SoundItemOk(const SoundItem& it) {
    if (it.op > kSoundOpMax || it.nrtpc > kMaxSoundRtpcs || it.age_ms > kMaxSoundAgeMs || it.fade_ms > kMaxSoundFadeMs ||
        it.category >= kSoundCategories)
        return false;
    const bool no_off = it.off[0] == 0 && it.off[1] == 0 && it.off[2] == 0;
    switch (static_cast<SoundOp>(it.op)) {
    case SoundOp::post:
        if (it.a == 0 || it.b == 0 || it.slot >= kSoundSlots || it.fade_ms != 0) return false;
        break;
    case SoundOp::stop:
        if (it.a != 0 || it.b == 0 || it.slot != 0 || it.tag != 0 || it.nrtpc != 0 || !no_off) return false;
        break;
    case SoundOp::action:
        if (it.a == 0 || it.slot > kSoundActionMax || it.nrtpc != 0 || !no_off) return false;
        break;
    case SoundOp::rtpc:
        if (it.a != 0 || it.b != 0 || it.slot >= kSoundSlots || it.nrtpc == 0 || !no_off) return false;
        break;
    }
    for (unsigned k = 0; k < it.nrtpc; ++k) {
        const float v = it.rtpc_value[k];
        if (it.rtpc_id[k] == 0 || !std::isfinite(v) || std::fabs(v) > 1e7f) return false;
    }
    return true;
}
}  // namespace

bool SoundItemValid(const SoundItem& it) noexcept { return SoundItemOk(it); }

std::size_t SoundFrameSize(const SoundFrame& f) noexcept {
    if (f.n == 0 || f.n > kMaxSoundItems) return 0;
    std::size_t size = kSoundHeaderSize;
    for (unsigned i = 0; i < f.n; ++i) {
        if (!SoundItemOk(f.item[i])) return 0;
        size += SoundItemSize(f.item[i].nrtpc);
    }
    return size;
}

std::size_t EncodeSound(const SoundFrame& f, std::uint8_t* out, std::size_t cap) noexcept {
    const std::size_t size = SoundFrameSize(f);
    if (!out || !size || size > kMaxSoundFrameSize || cap < size) return 0;
    std::uint8_t* p = out;
    std::memcpy(p, kSoundMagic, 4);
    p += 4;
    Put(p, kSoundVersion);
    Put(p, f.n);
    Put(p, f.seq);
    Put(p, f.sender_ms);
    for (unsigned i = 0; i < f.n; ++i) {
        const SoundItem& it = f.item[i];
        Put(p, it.a);
        Put(p, it.b);
        Put(p, it.op);
        Put(p, it.slot);
        Put(p, it.tag);
        Put(p, it.nrtpc);
        Put(p, it.age_ms);
        Put(p, it.fade_ms);
        for (std::int16_t o : it.off) Put(p, o);
        Put(p, it.category);
        Put(p, static_cast<std::uint8_t>(0));
        for (unsigned k = 0; k < it.nrtpc; ++k) {
            Put(p, it.rtpc_id[k]);
            Put(p, it.rtpc_value[k]);
        }
    }
    return size;
}

bool DecodeSound(const std::uint8_t* data, std::size_t size, SoundFrame& out) noexcept {
    if (!data || size < kSoundHeaderSize + kSoundItemSize || size > kMaxSoundFrameSize || std::memcmp(data, kSoundMagic, 4) != 0) return false;
    const std::uint8_t* p = data + 4;
    const std::uint8_t* const end = data + size;
    if (Get<std::uint8_t>(p) != kSoundVersion) return false;
    const std::uint8_t n = Get<std::uint8_t>(p);
    if (n == 0 || n > kMaxSoundItems) return false;
    const std::uint16_t seq = Get<std::uint16_t>(p);
    const std::uint32_t sender_ms = Get<std::uint32_t>(p);
    static thread_local SoundFrame f;  // `out` changes only when the whole frame is valid (3.4 KB: not on the stack)
    for (unsigned i = 0; i < n; ++i) {
        if (static_cast<std::size_t>(end - p) < kSoundItemSize) return false;
        SoundItem it;
        it.a = Get<std::uint32_t>(p);
        it.b = Get<std::uint32_t>(p);
        it.op = Get<std::uint8_t>(p);
        it.slot = Get<std::uint8_t>(p);
        it.tag = Get<std::uint8_t>(p);
        it.nrtpc = Get<std::uint8_t>(p);
        it.age_ms = Get<std::uint16_t>(p);
        it.fade_ms = Get<std::uint16_t>(p);
        for (std::int16_t& o : it.off) o = Get<std::int16_t>(p);
        it.category = Get<std::uint8_t>(p);
        if (Get<std::uint8_t>(p) != 0) return false;
        if (it.nrtpc > kMaxSoundRtpcs || static_cast<std::size_t>(end - p) < it.nrtpc * kSoundRtpcSize) return false;
        for (unsigned k = 0; k < it.nrtpc; ++k) {
            it.rtpc_id[k] = Get<std::uint32_t>(p);
            it.rtpc_value[k] = Get<float>(p);
        }
        if (!SoundItemOk(it)) return false;
        f.item[i] = it;
    }
    if (p != end) return false;
    out.n = n;
    out.seq = seq;
    out.sender_ms = sender_ms;
    for (unsigned i = 0; i < n; ++i) out.item[i] = f.item[i];
    return true;
}

// ---------------------------------------------------------------- vehicle frame (STM1)
namespace {
constexpr VehicleResource kVehicleRes[kVehicleResourceCount] = {
    {"roadster", "ds/entities/vehicle/pe_ds_vehicle_casp", "ff17815d-4a07-0840-9b23-d30f30269e7f", VehicleKind::speedcar, 0xECF43324u},
    {"trike", "ds/entities/vehicle/pe_ds_vehicle_ca03", "3337b48c-a29c-d748-a1bd-5f02f064486a", VehicleKind::motorbike, 0x8DABA9B0u},
    {"trike_kusanagi", "ds/entities/vehicle/pe_ds_vehicle_bkyk", "1d8c8523-3510-ff43-827d-7cea16e6ff37", VehicleKind::motorbike, 0u},
    {"truck", "ds/entities/vehicle/pe_ds_vehicle_ca02", "0f79737e-e401-e249-9d56-1f09e2c5b4ee", VehicleKind::truck, 0xB609EC0Du},
    {"truck_npc", "ds/entities/vehicle/pe_ds_vehicle_celd", "ab30dada-9e77-124a-927c-dc50421618d2", VehicleKind::truck, 0xB609EC0Du},
};

int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" -> uuid.bytes_le (the first three groups little-endian).
bool GuidBytesLe(const char* text, std::uint8_t (&out)[16]) {
    std::uint8_t be[16] = {};
    unsigned n = 0;
    for (const char* p = text; *p; ++p) {
        if (*p == '-') continue;
        const int hi = Hex(p[0]);
        const int lo = p[1] ? Hex(p[1]) : -1;
        if (hi < 0 || lo < 0 || n >= 16) return false;
        be[n++] = static_cast<std::uint8_t>(hi * 16 + lo);
        ++p;
    }
    if (n != 16) return false;
    const std::uint8_t order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    for (unsigned i = 0; i < 16; ++i) out[i] = be[order[i]];
    return true;
}

bool VehicleOk(const VehicleFrame& f) {
    if (f.kind < 1 || f.kind > 3 || f.state > kVehicleStateMax || (f.flags & ~kVehKnown) != 0) return false;
    if (((f.flags & kVehJoints) != 0) != (f.n > 0) || f.n > kMaxVehicleJoints) return false;
    if (f.n > 0 && f.layout == 0) return false;
    const VehicleResource* r = FindVehicleResource(f.guid);
    if (!r || static_cast<std::uint8_t>(r->kind) != f.kind) return false;
    if (!Finite(f.x) || !Finite(f.y) || !Finite(f.z) || std::fabs(f.x) > 1e7 || std::fabs(f.y) > 1e7 || std::fabs(f.z) > 1e7) return false;
    if (!FiniteQ(f.rot) || !FiniteQ(f.seat_rot) || !std::isfinite(f.speed) || std::fabs(f.speed) > 1000.0f) return false;
    for (float v : f.seat) {
        if (!std::isfinite(v) || std::fabs(v) > kMaxVehicleSeatM) return false;
        if (!(f.flags & kVehSeated) && v != 0.0f) return false;
    }
    for (unsigned j = 0; j < f.n; ++j) {
        if (!FiniteQ(f.q[j])) return false;
        for (float v : f.t[j])
            if (!std::isfinite(v) || std::fabs(v) > kMaxVehicleJointM) return false;
    }
    return true;
}

void PutQ6(std::uint8_t*& p, const Quat& q) {
    const std::uint64_t b = PackQuat(q);
    for (unsigned k = 0; k < 6; ++k) *p++ = static_cast<std::uint8_t>(b >> (8 * k));
}
Quat GetQ6(const std::uint8_t*& p) {
    std::uint64_t b = 0;
    for (unsigned k = 0; k < 6; ++k) b |= static_cast<std::uint64_t>(*p++) << (8 * k);
    return UnpackQuat(b);
}
}  // namespace

const VehicleResource* VehicleResources() noexcept { return kVehicleRes; }

const VehicleResource* FindVehicleResource(const std::uint8_t (&guid)[16]) noexcept {
    for (const VehicleResource& r : kVehicleRes) {
        std::uint8_t g[16];
        if (GuidBytesLe(r.guid, g) && std::memcmp(g, guid, 16) == 0) return &r;
    }
    return nullptr;
}

std::size_t EncodeVehicle(const VehicleFrame& f, std::uint8_t* out, std::size_t cap) noexcept {
    if (!out || !VehicleOk(f)) return 0;
    const std::size_t size = VehicleFrameSize(f.n);
    if (cap < size) return 0;
    std::uint8_t* p = out;
    std::memcpy(p, kVehicleMagic, 4);
    p += 4;
    Put(p, kVehicleVersion);
    Put(p, f.kind);
    Put(p, f.state);
    Put(p, f.flags);
    Put(p, f.seq);
    Put(p, f.sender_ms);
    std::memcpy(p, f.guid, 16);
    p += 16;
    Put(p, f.x);
    Put(p, f.y);
    Put(p, f.z);
    PutQ6(p, f.rot);
    Put(p, f.speed);
    for (float v : f.seat) Put(p, static_cast<std::int16_t>(std::lround(v * 1000.0f)));
    PutQ6(p, f.seat_rot);
    Put(p, f.layout);
    Put(p, f.n);
    Put(p, std::uint8_t{0});
    for (unsigned j = 0; j < f.n; ++j) {
        PutQ6(p, f.q[j]);
        for (float v : f.t[j]) Put(p, static_cast<std::int16_t>(std::lround(v * 1000.0f)));
    }
    return static_cast<std::size_t>(p - out);
}

bool DecodeVehicle(const std::uint8_t* data, std::size_t size, VehicleFrame& out) noexcept {
    if (!data || size < kVehicleHeaderSize || std::memcmp(data, kVehicleMagic, 4) != 0) return false;
    const std::uint8_t* p = data + 4;
    static thread_local VehicleFrame f;
    f = VehicleFrame{};
    if (Get<std::uint8_t>(p) != kVehicleVersion) return false;
    f.kind = Get<std::uint8_t>(p);
    f.state = Get<std::uint8_t>(p);
    f.flags = Get<std::uint8_t>(p);
    f.seq = Get<std::uint32_t>(p);
    f.sender_ms = Get<std::uint32_t>(p);
    std::memcpy(f.guid, p, 16);
    p += 16;
    f.x = Get<double>(p);
    f.y = Get<double>(p);
    f.z = Get<double>(p);
    f.rot = GetQ6(p);
    f.speed = Get<float>(p);
    for (float& v : f.seat) v = static_cast<float>(Get<std::int16_t>(p)) / 1000.0f;
    f.seat_rot = GetQ6(p);
    f.layout = Get<std::uint32_t>(p);
    f.n = Get<std::uint8_t>(p);
    if (Get<std::uint8_t>(p) != 0) return false;
    if (f.n > kMaxVehicleJoints || size != VehicleFrameSize(f.n)) return false;
    for (unsigned j = 0; j < f.n; ++j) {
        f.q[j] = GetQ6(p);
        for (float& v : f.t[j]) v = static_cast<float>(Get<std::int16_t>(p)) / 1000.0f;
    }
    if (!VehicleOk(f)) return false;
    out = f;
    return true;
}

}  // namespace st::wire
