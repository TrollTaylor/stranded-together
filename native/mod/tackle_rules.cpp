// Vehicle hits (M4): the pure part. See tackle_rules.hpp and docs/real-partner-body.md section 25.
#include "tackle_rules.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "vehicle_wire.hpp"

namespace st::trules {
namespace {
void PutU32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v), p[1] = static_cast<std::uint8_t>(v >> 8), p[2] = static_cast<std::uint8_t>(v >> 16),
    p[3] = static_cast<std::uint8_t>(v >> 24);
}
std::uint32_t GetU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}
void PutU16(std::uint8_t* p, std::uint16_t v) { p[0] = static_cast<std::uint8_t>(v), p[1] = static_cast<std::uint8_t>(v >> 8); }
std::uint16_t GetU16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
void PutF32(std::uint8_t* p, float f) {
    std::uint32_t v;
    std::memcpy(&v, &f, 4);
    PutU32(p, v);
}
float GetF32(const std::uint8_t* p) {
    const std::uint32_t v = GetU32(p);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
bool Finite3(const double (&p)[3]) { return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]); }
}  // namespace

const char* PathName(Path p) noexcept {
    switch (p) {
        case Path::direct: return "direct";
        case Path::bracket: return "state-4 bracket";
        default: return "none";
    }
}

Path PathFor(std::uint8_t kind, bool has_mule_slot, bool direct_ok, bool bracket_ok, int forced) noexcept {
    if (kind < 1 || kind > 3) return Path::none;
    if (forced == 1) return direct_ok ? Path::direct : Path::none;
    if (forced == 2) return bracket_ok ? Path::bracket : Path::none;
    (void)has_mule_slot;
    if (direct_ok) return Path::direct;    // the MULE attack (trucks) or the player-vehicle attack, only our Sam (D4-3)
    if (bracket_ok) return Path::bracket;  // the proven player-vehicle hit: only when the direct path's checks failed
    return Path::none;
}

std::uint16_t AttackOf(Path p, bool has_mule_slot) noexcept {
    if (p == Path::direct && has_mule_slot) return kAttackMule;
    if (p == Path::none) return 0;
    return kAttackPlayer;
}

const char* BlockName(Block b) noexcept {
    static const char* const k[] = {"ok",
                                    "off",
                                    "partner vehicle damage is off (session.ini partner_vehicle_damage=0)",
                                    "the partner drives no vehicle we follow",
                                    "the partner does not hold the driver seat",
                                    "no local Sam",
                                    "our Sam is asleep",
                                    "our Sam is not on foot",
                                    "our Sam rides as a passenger",
                                    "a load, menu, cutscene or sequence",
                                    "over 50 m away",
                                    "at 15 km/h or slower",
                                    "held off after a hit (1.5 s)",
                                    "our Sam is not near its attack sphere",
                                    "no hit path for this vehicle"};
    const auto i = static_cast<unsigned>(b);
    return i < sizeof(k) / sizeof(k[0]) ? k[i] : "?";
}

Block Gate(const GateIn& in) noexcept {
    if (!in.feature_on) return Block::off;
    if (!in.friendly_fire) return Block::friendly_fire;
    if (!in.have_vehicle) return Block::no_vehicle;
    if (!in.partner_drives) return Block::not_driven;
    if (!in.local_ok) return Block::no_local;
    if (!in.local_awake) return Block::asleep;
    if (!in.local_on_foot) return Block::not_on_foot;
    if (in.carried) return Block::carried;
    if (in.game_blocked) return Block::game_state;
    if (!(in.dist_m <= kRangeM)) return Block::too_far;
    if (!(in.speed_kmh > kMinSpeedKmh)) return Block::slow;
    if (in.now_ms < in.hold_off_until_ms) return Block::hold_off;
    if (in.path == Path::none) return Block::no_path;
    if (!(in.sam_to_sphere_m <= in.sphere_radius_m + kNearSphereM)) return Block::not_near;
    // the bracket hits whatever the game's sphere catches (no callback of ours): only while our Sam is inside it
    if (in.path == Path::bracket && !(in.sam_to_sphere_m <= in.sphere_radius_m)) return Block::not_near;
    return Block::ok;
}

void SphereCentre(const double (&pos)[3], const float (&rows)[9], const float (&local)[3], float signed_speed, double (&out)[3]) noexcept {
    const double l[3] = {local[0], signed_speed < 0.0f ? -static_cast<double>(local[1]) : local[1], local[2]};
    for (int i = 0; i < 3; ++i) out[i] = pos[i] + l[0] * rows[0 + i] + l[1] * rows[3 + i] + l[2] * rows[6 + i];
}

void SamCentre(const double (&root)[3], double (&out)[3]) noexcept {
    out[0] = root[0], out[1] = root[1], out[2] = root[2] + kSamCentreUpM;
}

bool HitIsSams(const double (&hit)[3], const double (&sam_centre)[3]) noexcept {
    if (!Finite3(hit) || !Finite3(sam_centre)) return false;
    const double dx = hit[0] - sam_centre[0], dy = hit[1] - sam_centre[1], dz = hit[2] - sam_centre[2];
    return dx * dx + dy * dy + dz * dz <= kRestrictM * kRestrictM;
}

float CapAmount(float amount) noexcept {
    if (!(amount > kHitAmount)) return amount;  // NaN, negative (the attack's own damage) and small amounts stay
    return kHitAmount;
}

float BracketSpeed(float speed_kmh) noexcept { return speed_kmh > static_cast<float>(kLethalSpeedKmh) ? kBracketSpeedKmh : -1.0f; }

void CrashSphere(const double (&sam_root)[3], const double (&vehicle_pos)[3], double (&centre)[3]) noexcept {
    double c[3];
    SamCentre(sam_root, c);
    double dx = vehicle_pos[0] - c[0], dy = vehicle_pos[1] - c[1];
    const double d = std::sqrt(dx * dx + dy * dy);
    if (d > 1e-3) dx /= d, dy /= d;
    else dx = 0, dy = 0;
    centre[0] = c[0] + dx * kCrashSphereBackM;
    centre[1] = c[1] + dy * kCrashSphereBackM;
    centre[2] = c[2];
}

void HitLimiter::Called(double now_ms) noexcept {
    if (!in_pass_) {
        in_pass_ = true;
        pass_start_ = now_ms;
        calls_ = 0;
        ++passes_;
    }
    ++calls_;
    last_call_ = now_ms;
}

bool HitLimiter::Expire(double now_ms) noexcept {
    if (!in_pass_ || now_ms - last_call_ <= kPassIdleMs) return false;
    in_pass_ = false;
    return true;
}

void HitLimiter::HitSeen(double now_ms) noexcept {
    ++hits_;
    in_pass_ = false;
    hold_until_ = now_ms + kHoldOffMs;
}

std::size_t EncodeHit(const HitMsg& m, std::uint8_t* out, std::size_t cap) noexcept {
    if (!out || cap < kMessageSize || (m.attack != kAttackMule && m.attack != kAttackPlayer) || m.from > 1 || m.kind < 1 || m.kind > 3 || m.path > 2 || m.crash > 1) return 0;
    if (!std::isfinite(m.speed_kmh) || !std::isfinite(m.health_before) || !std::isfinite(m.health_after)) return 0;
    if (vwire::WriteHeader(kSvmType, out, cap) != kSvmHeaderSize) return 0;
    std::uint8_t* b = out + kSvmHeaderSize;
    std::memset(b, 0, kBodySize);
    std::memcpy(b, kMagic, 4);
    b[4] = m.from, b[5] = m.kind, b[6] = m.path, b[7] = m.crash;
    PutU32(b + 8, m.vehicle_id);
    PutU16(b + 12, m.attack);
    PutU16(b + 14, m.world);
    PutU32(b + 16, m.sender_ms);
    PutF32(b + 20, m.speed_kmh);
    PutF32(b + 24, m.health_before);
    PutF32(b + 28, m.health_after);
    return kMessageSize;
}

bool DecodeHit(const std::uint8_t* in, std::size_t size, HitMsg& out) noexcept {
    std::uint8_t type = 0;
    if (!in || size != kMessageSize || !vwire::PeekType(in, size, &type) || type != kSvmType) return false;
    const std::uint8_t* b = in + kSvmHeaderSize;
    if (std::memcmp(b, kMagic, 4) != 0) return false;
    HitMsg m;
    m.from = b[4], m.kind = b[5], m.path = b[6], m.crash = b[7];
    m.vehicle_id = GetU32(b + 8);
    m.attack = GetU16(b + 12);
    m.world = GetU16(b + 14);
    m.sender_ms = GetU32(b + 16);
    m.speed_kmh = GetF32(b + 20);
    m.health_before = GetF32(b + 24);
    m.health_after = GetF32(b + 28);
    if (m.from > 1 || m.kind < 1 || m.kind > 3 || m.path > 2 || m.crash > 1) return false;
    if (m.attack != kAttackMule && m.attack != kAttackPlayer) return false;
    if (!std::isfinite(m.speed_kmh) || !std::isfinite(m.health_before) || !std::isfinite(m.health_after)) return false;
    out = m;
    return true;
}

bool Moving(bool was_moving, double speed_mps) noexcept {
    if (!std::isfinite(speed_mps)) return was_moving;
    return was_moving ? speed_mps > kCapsuleVehicleStopMps : speed_mps > kCapsuleVehicleMps;
}

double SpeedSample(double prev_speed, bool have_prev, const double (&from)[3], const double (&to)[3], double dt_ms) noexcept {
    if (!(dt_ms > 0) || !Finite3(from) || !Finite3(to)) return have_prev ? prev_speed : 0;
    const double dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
    const double inst = std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0 / dt_ms;
    if (!std::isfinite(inst) || inst >= 200) return have_prev ? prev_speed : 0;
    return have_prev ? prev_speed * 0.5 + inst * 0.5 : inst;
}

double DistanceToBox(const VehicleBox& b, const double (&p)[3]) noexcept {
    const double d[3] = {p[0] - b.c[0], p[1] - b.c[1], p[2] - b.c[2]};
    double s = 0;
    for (int a = 0; a < 3; ++a) {
        const double l = d[0] * b.rot[a * 3 + 0] + d[1] * b.rot[a * 3 + 1] + d[2] * b.rot[a * 3 + 2];
        const double o = std::fabs(l) - b.half[a];
        if (o > 0) s += o * o;
    }
    return std::sqrt(s);
}

const char* CapsuleVehicleBlock(const double (&body)[3], const VehicleBox* boxes, std::size_t n, bool enabling) noexcept {
    if (!boxes) return nullptr;
    for (std::size_t i = 0; i < n; ++i) {
        const VehicleBox& b = boxes[i];
        if ((b.moving || b.speed_mps > kCapsuleVehicleMps) && DistanceToBox(b, body) < kCapsuleVehicleNearM) return "off: a vehicle moves within 5 m of the partner";
        if (enabling) {
            for (double up : {0.0, 1.0, 2.0}) {
                const double p[3] = {body[0], body[1], body[2] + up};
                if (DistanceToBox(b, p) < kCapsuleRadiusM) return "waiting: a vehicle overlaps the partner";
            }
        }
    }
    return nullptr;
}

}  // namespace st::trules
