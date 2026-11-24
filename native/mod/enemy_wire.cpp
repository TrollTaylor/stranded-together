// SEN1 / SEP1 codecs (enemy_wire.hpp).
#include "enemy_wire.hpp"

#include <cmath>
#include <cstring>

#include "enemy_census.hpp"

namespace st::ewire {
namespace {
constexpr double kPi = 3.14159265358979323846;
void Put16(std::uint8_t* p, std::uint16_t v) noexcept { p[0] = static_cast<std::uint8_t>(v), p[1] = static_cast<std::uint8_t>(v >> 8); }
void Put32(std::uint8_t* p, std::uint32_t v) noexcept {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint16_t Get16(const std::uint8_t* p) noexcept { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }
std::uint32_t Get32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
bool EntryOk(const Enemy& e) noexcept {
    if (!enemy::IsHumanEnemyId(e.actor_id)) return false;
    for (int k = 0; k < 3; ++k) {
        if (!std::isfinite(e.pos[k]) || std::fabs(e.pos[k]) >= kMaxCoordM) return false;
        if (!std::isfinite(e.vel[k])) return false;
    }
    return std::isfinite(e.yaw);
}
std::int16_t VelCm(float v) noexcept {
    const double c = std::round(static_cast<double>(v) * 100.0);
    return static_cast<std::int16_t>(c > 32700 ? 32700 : c < -32700 ? -32700 : c);
}
}  // namespace

std::uint16_t YawToU16(float yaw) noexcept {
    double turn = static_cast<double>(yaw) / (2.0 * kPi);
    turn -= std::floor(turn);
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(std::llround(turn * 65536.0)) & 0xFFFFu);
}
float YawFromU16(std::uint16_t u) noexcept {
    double a = u / 65536.0 * 2.0 * kPi;
    if (a >= kPi) a -= 2.0 * kPi;
    return static_cast<float>(a);
}

std::size_t EncodeEnemies(const EnemyFrame& f, std::uint8_t* out, std::size_t cap) noexcept {
    if (!out || f.n == 0 || f.n > kSenMax) return 0;
    const std::size_t size = kSenHeader + kSenEntry * f.n;
    if (cap < size) return 0;
    for (unsigned i = 0; i < f.n; ++i)
        if (!EntryOk(f.e[i])) return 0;
    std::memset(out, 0, size);
    out[0] = 'S', out[1] = 'E', out[2] = 'N', out[3] = '1';
    out[4] = kVersion;
    out[5] = f.n;
    Put16(out + 6, f.flags);
    Put32(out + 8, f.seq);
    Put32(out + 12, f.sender_ms);
    Put32(out + 16, f.gen_tag);
    for (unsigned i = 0; i < f.n; ++i) {
        const Enemy& e = f.e[i];
        std::uint8_t* p = out + kSenHeader + kSenEntry * i;
        Put32(p + 0, e.actor_id);
        p[4] = e.owner_epoch, p[5] = e.state, p[6] = e.phase, p[7] = e.flags;
        for (int k = 0; k < 3; ++k) Put32(p + 8 + 4 * k, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::llround(e.pos[k] * 1000.0))));
        Put16(p + 20, YawToU16(e.yaw));
        for (int k = 0; k < 3; ++k) Put16(p + 22 + 2 * k, static_cast<std::uint16_t>(VelCm(e.vel[k])));
    }
    return size;
}

bool DecodeEnemies(const std::uint8_t* d, std::size_t n, EnemyFrame& out) noexcept {
    if (!d || n < kSenHeader || d[0] != 'S' || d[1] != 'E' || d[2] != 'N' || d[3] != '1' || d[4] != kVersion) return false;
    const unsigned count = d[5];
    if (count == 0 || count > kSenMax || n != kSenHeader + kSenEntry * count) return false;
    static thread_local EnemyFrame f;
    f = EnemyFrame{};
    f.n = static_cast<std::uint8_t>(count);
    f.flags = Get16(d + 6);
    f.seq = Get32(d + 8);
    f.sender_ms = Get32(d + 12);
    f.gen_tag = Get32(d + 16);
    for (unsigned i = 0; i < count; ++i) {
        const std::uint8_t* p = d + kSenHeader + kSenEntry * i;
        Enemy& e = f.e[i];
        e.actor_id = Get32(p + 0);
        e.owner_epoch = p[4], e.state = p[5], e.phase = p[6], e.flags = p[7];
        for (int k = 0; k < 3; ++k) e.pos[k] = static_cast<std::int32_t>(Get32(p + 8 + 4 * k)) / 1000.0;
        e.yaw = YawFromU16(Get16(p + 20));
        for (int k = 0; k < 3; ++k) e.vel[k] = static_cast<float>(static_cast<std::int16_t>(Get16(p + 22 + 2 * k))) / 100.0f;
        if (!EntryOk(e)) return false;
    }
    out = f;
    return true;
}

std::size_t EncodeEnemyPose(const EnemyPose& p, std::uint8_t* out, std::size_t cap) noexcept {
    if (!out || cap < kSepHeader + 1 || p.slot >= kSepSlots || !enemy::IsHumanEnemyId(p.actor_id)) return 0;
    const std::size_t j = wire::EncodeJoints(p.joints, out + kSepHeader, cap - kSepHeader);
    if (!j) return 0;
    out[0] = 'S', out[1] = 'E', out[2] = 'P', out[3] = '1';
    out[4] = kVersion;
    out[5] = p.slot;
    out[6] = out[7] = 0;
    Put32(out + 8, p.actor_id);
    Put32(out + 12, p.sender_ms);
    return kSepHeader + j;
}

bool DecodeEnemyPose(const std::uint8_t* d, std::size_t n, EnemyPose& out) noexcept {
    if (!d || n <= kSepHeader || n > kSepMaxSize || d[0] != 'S' || d[1] != 'E' || d[2] != 'P' || d[3] != '1' || d[4] != kVersion) return false;
    if (d[5] >= kSepSlots || d[6] || d[7]) return false;
    const std::uint32_t id = Get32(d + 8);
    if (!enemy::IsHumanEnemyId(id)) return false;
    if (!wire::DecodeJoints(d + kSepHeader, n - kSepHeader, out.joints)) return false;
    out.slot = d[5];
    out.actor_id = id;
    out.sender_ms = Get32(d + 12);
    out.foreign_layout = out.joints.layout_hash != wire::kSamLayoutHash || out.joints.joint_count != 77;
    return true;
}

}  // namespace st::ewire
