// SFX1 / SEH1 codecs (shot_wire.hpp).
#include "shot_wire.hpp"

#include <cstring>

namespace st::shotw {
namespace {
void Put16(std::uint8_t* p, std::uint16_t v) noexcept { p[0] = static_cast<std::uint8_t>(v), p[1] = static_cast<std::uint8_t>(v >> 8); }
void Put32(std::uint8_t* p, std::uint32_t v) noexcept {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint16_t Get16(const std::uint8_t* p) noexcept { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }
std::uint32_t Get32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
void PutF(std::uint8_t* p, float f) noexcept {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    Put32(p, u);
}
float GetF(const std::uint8_t* p) noexcept {
    const std::uint32_t u = Get32(p);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
constexpr double kMaxCoordM = 2.0e6;  // +-2000 km

bool ShotOk(const Shot& s) noexcept {
    if (!(s.speed > 0.0f) || !std::isfinite(s.speed) || !AttackReplayable(s.attack_id)) return false;
    double l2 = 0;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(s.dir[i]) || !std::isfinite(s.origin[i]) || std::fabs(s.origin[i]) > kMaxCoordM) return false;
        l2 += static_cast<double>(s.dir[i]) * s.dir[i];
    }
    return l2 > 0.81 && l2 < 1.21;
}
std::int16_t ClampI16(double v) noexcept {
    const double r = std::round(v);
    return static_cast<std::int16_t>(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
}
}  // namespace

std::size_t EncodeShots(const Frame& f, std::uint8_t* out, std::size_t cap) noexcept {
    if (f.n == 0 || f.n > kMaxShots || !out) return 0;
    const std::size_t size = kHeaderSize + kShotSize * f.n;
    if (cap < size) return 0;
    for (unsigned i = 0; i < f.n; ++i)
        if (!ShotOk(f.shots[i]) || !ShooterOk(f.flags, f.shots[i].shooter)) return 0;
    std::memset(out, 0, size);
    out[0] = 'S', out[1] = 'F', out[2] = 'X', out[3] = '1';
    out[4] = kVersion;
    out[5] = f.n;
    Put16(out + 6, f.flags);
    Put32(out + 8, f.seq);
    Put32(out + 12, f.sender_ms);
    Put32(out + 16, f.world);
    for (unsigned i = 0; i < f.n; ++i) {
        const Shot& s = f.shots[i];
        std::uint8_t* p = out + kHeaderSize + kShotSize * i;
        Put32(p + 0, s.shot_id);
        Put32(p + 4, s.shooter);
        Put16(p + 8, s.attack_id);
        p[10] = s.weapon_id;
        p[11] = s.magazine_id;
        for (int k = 0; k < 3; ++k) Put32(p + 12 + 4 * k, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::llround(s.origin[k] * 1000.0))));
        double l = std::sqrt(static_cast<double>(s.dir[0]) * s.dir[0] + static_cast<double>(s.dir[1]) * s.dir[1] + static_cast<double>(s.dir[2]) * s.dir[2]);
        for (int k = 0; k < 3; ++k) Put16(p + 24 + 2 * k, static_cast<std::uint16_t>(Q15(static_cast<float>(s.dir[k] / l))));
        const double dm = std::round(static_cast<double>(s.speed) * 10.0);
        Put16(p + 30, static_cast<std::uint16_t>(dm < 1 ? 1 : dm > 65535 ? 65535 : dm));
        Put32(p + 32, s.target_actor);
        for (int k = 0; k < 3; ++k) Put16(p + 36 + 2 * k, static_cast<std::uint16_t>(ClampI16(static_cast<double>(s.target_off[k]) * 100.0)));
        p[42] = s.rec04, p[43] = s.rec05, p[44] = s.rec06, p[45] = s.rec58, p[46] = s.rec5A, p[47] = s.recD0, p[48] = s.recD1;
        p[49] = s.flags;
        Put16(p + 50, s.age_ms);
        Put32(p + 52, s.rec84);
        PutF(p + 56, std::isfinite(s.rec94) ? s.rec94 : 0.0f);
        for (int k = 0; k < 3; ++k) Put16(p + 60 + 2 * k, static_cast<std::uint16_t>(ClampI16(static_cast<double>(s.hand_rel[k]) * 1000.0)));
        double turn = static_cast<double>(s.yaw) / (2.0 * 3.14159265358979323846);
        turn -= std::floor(turn);
        Put16(p + 66, static_cast<std::uint16_t>(static_cast<std::uint32_t>(std::llround(turn * 65536.0)) & 0xFFFFu));
    }
    return size;
}

bool DecodeShots(const std::uint8_t* d, std::size_t n, Frame& out) noexcept {
    if (!d || n < kHeaderSize || d[0] != 'S' || d[1] != 'F' || d[2] != 'X' || d[3] != '1' || d[4] != kVersion) return false;
    const unsigned count = d[5];
    if (count == 0 || count > kMaxShots || n != kHeaderSize + kShotSize * count) return false;
    Frame f;
    f.n = static_cast<std::uint8_t>(count);
    f.flags = Get16(d + 6);
    f.seq = Get32(d + 8);
    f.sender_ms = Get32(d + 12);
    f.world = Get32(d + 16);
    for (unsigned i = 0; i < count; ++i) {
        const std::uint8_t* p = d + kHeaderSize + kShotSize * i;
        Shot& s = f.shots[i];
        s.shot_id = Get32(p + 0);
        s.shooter = Get32(p + 4);
        s.attack_id = Get16(p + 8);
        s.weapon_id = p[10];
        s.magazine_id = p[11];
        for (int k = 0; k < 3; ++k) s.origin[k] = static_cast<std::int32_t>(Get32(p + 12 + 4 * k)) / 1000.0;
        for (int k = 0; k < 3; ++k) s.dir[k] = FromQ15(static_cast<std::int16_t>(Get16(p + 24 + 2 * k)));
        s.speed = static_cast<float>(Get16(p + 30)) / 10.0f;
        s.target_actor = Get32(p + 32);
        for (int k = 0; k < 3; ++k) s.target_off[k] = static_cast<float>(static_cast<std::int16_t>(Get16(p + 36 + 2 * k))) / 100.0f;
        s.rec04 = p[42], s.rec05 = p[43], s.rec06 = p[44], s.rec58 = p[45], s.rec5A = p[46], s.recD0 = p[47], s.recD1 = p[48];
        s.flags = p[49];
        s.age_ms = Get16(p + 50);
        s.rec84 = Get32(p + 52);
        s.rec94 = GetF(p + 56);
        if (!std::isfinite(s.rec94)) return false;
        for (int k = 0; k < 3; ++k) s.hand_rel[k] = static_cast<float>(static_cast<std::int16_t>(Get16(p + 60 + 2 * k))) / 1000.0f;
        s.yaw = static_cast<float>(Get16(p + 66) / 65536.0 * 2.0 * 3.14159265358979323846);
        if (!ShotOk(s) || !ShooterOk(f.flags, s.shooter)) return false;
        // normalise the direction back to unit (the q15 rounding)
        const double l = std::sqrt(static_cast<double>(s.dir[0]) * s.dir[0] + static_cast<double>(s.dir[1]) * s.dir[1] + static_cast<double>(s.dir[2]) * s.dir[2]);
        for (int k = 0; k < 3; ++k) s.dir[k] = static_cast<float>(s.dir[k] / l);
    }
    out = f;
    return true;
}

std::size_t EncodeBeacon(const Beacon& b, std::uint8_t* out, std::size_t cap) noexcept {
    if (!out || cap < kBeaconSize) return 0;
    std::memset(out, 0, kBeaconSize);
    out[0] = 'S', out[1] = 'E', out[2] = 'H', out[3] = '1';
    out[4] = kVersion;
    out[5] = b.parts;
    Put16(out + 6, b.enemy_count);
    Put32(out + 8, b.ids_fp);
    Put32(out + 12, b.guids_fp);
    Put32(out + 16, b.world);
    out[20] = b.freeze_mode;
    out[21] = b.role;
    return kBeaconSize;
}

bool DecodeBeacon(const std::uint8_t* d, std::size_t n, Beacon& out) noexcept {
    if (!LooksLikeBeacon(d, n) || d[4] != kVersion || d[20] > 4 || d[21] > 2 || d[22] || d[23]) return false;
    Beacon b;
    b.parts = d[5];
    b.enemy_count = Get16(d + 6);
    b.ids_fp = Get32(d + 8);
    b.guids_fp = Get32(d + 12);
    b.world = Get32(d + 16);
    b.freeze_mode = d[20];
    b.role = d[21];
    out = b;
    return true;
}

}  // namespace st::shotw
