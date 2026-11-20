// Riding together, the pure part (design M3). See ride_wire.hpp and docs/real-partner-body.md section 24.
#include "ride_wire.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "vehicle_wire.hpp"

namespace st::rwire {
namespace {

void PutU32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v), p[1] = static_cast<std::uint8_t>(v >> 8), p[2] = static_cast<std::uint8_t>(v >> 16),
    p[3] = static_cast<std::uint8_t>(v >> 24);
}
std::uint32_t GetU32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 |
           static_cast<std::uint32_t>(p[3]) << 24;
}

bool ReasonOk(MsgType t, std::uint8_t r) noexcept {
    switch (t) {
        case MsgType::request: return r == kReqHold || r == kReqTest;
        case MsgType::accept: return r == 0;
        case MsgType::deny: return r < static_cast<std::uint8_t>(Deny::count);
        case MsgType::end: return r < static_cast<std::uint8_t>(End::count);
    }
    return false;
}

bool Valid(const Msg& m) noexcept {
    const auto t = static_cast<std::uint8_t>(m.type);
    if (t < 1 || t > 4) return false;
    if (m.kind < 1 || m.kind > 3 || vwire::KindOfActorId(m.vehicle_id) != m.kind) return false;
    if (m.seat != SeatOf(m.kind)) return false;
    if (!ReasonOk(m.type, m.reason)) return false;
    if (m.from > 1 || m.ride_id == 0) return false;
    return true;
}

}  // namespace

const char* MsgTypeName(std::uint8_t t) noexcept {
    switch (t) {
        case 1: return "REQUEST";
        case 2: return "ACCEPT";
        case 3: return "DENY";
        case 4: return "END";
        default: return "?";
    }
}
const char* SeatName(std::uint8_t s) noexcept {
    switch (s) {
        case 1: return "passenger seat";
        case 2: return "pillion";
        default: return "no seat";
    }
}
const char* DenyName(std::uint8_t r) noexcept {
    static const char* const k[] = {"the driver does not drive it", "the seat is taken", "the vehicle moves", "not beside the vehicle",
                                     "riding together is off on the driver's game", "another world"};
    return r < sizeof(k) / sizeof(k[0]) ? k[r] : "?";
}
const char* EndName(std::uint8_t r) noexcept {
    static const char* const k[] = {"got off",          "link lost",    "the driver got off", "the vehicle is gone", "the vehicle is broken",
                                     "flipped",          "crash",        "the game took Sam",  "a load or the title", "the mod stopped",
                                     "the driver ended it", "nobody drives it", "not seen seated", "test", "jumped off"};
    return r < sizeof(k) / sizeof(k[0]) ? k[r] : "?";
}
const char* BlockName(Block b) noexcept {
    static const char* const k[] = {"ok", "off", "no link", "no partner vehicle", "not on foot", "too far", "the vehicle moves", "no seat", "busy"};
    const auto i = static_cast<unsigned>(b);
    return i < sizeof(k) / sizeof(k[0]) ? k[i] : "?";
}

std::size_t EncodeMsg(const Msg& m, std::uint8_t* out, std::size_t cap) noexcept {
    if (!out || cap < kMessageSize || !Valid(m)) return 0;
    if (vwire::WriteHeader(kSvmType, out, cap) != kSvmHeaderSize) return 0;
    std::uint8_t* b = out + kSvmHeaderSize;
    std::memset(b, 0, kBodySize);
    std::memcpy(b, kMagic, 4);
    b[4] = static_cast<std::uint8_t>(m.type);
    b[5] = static_cast<std::uint8_t>(m.seat);
    b[6] = m.reason;
    b[7] = m.from;
    PutU32(b + 8, m.ride_id);
    PutU32(b + 12, m.sender_ms);
    PutU32(b + 16, m.vehicle_id);
    b[20] = m.kind;
    PutU32(b + 24, m.world);
    return kMessageSize;
}

bool DecodeMsg(const std::uint8_t* in, std::size_t size, Msg& out) noexcept {
    std::uint8_t type = 0;
    if (!in || size != kMessageSize || !vwire::PeekType(in, size, &type) || type != kSvmType) return false;
    const std::uint8_t* b = in + kSvmHeaderSize;
    if (std::memcmp(b, kMagic, 4) != 0) return false;
    if (b[21] != 0 || b[22] != 0 || b[23] != 0) return false;
    Msg m;
    m.type = static_cast<MsgType>(b[4]);
    m.seat = static_cast<Seat>(b[5]);
    m.reason = b[6];
    m.from = b[7];
    m.ride_id = GetU32(b + 8);
    m.sender_ms = GetU32(b + 12);
    m.vehicle_id = GetU32(b + 16);
    m.kind = b[20];
    m.world = GetU32(b + 24);
    if (!Valid(m)) return false;
    out = m;
    return true;
}

// ---------------------------------------------------------------- seats
// Measured (docs/real-partner-body.md section 24.2): the driver's ROOT as the game seats Sam (the STM1 sender's seat, the
// hidden runs of 2026-11-19 and `seat_measure`), the passenger's as the driver's mirrored across the vehicle's centre plane
// (the game's HLP_Passenger is HLP_Driver mirrored on the roadster and the truck, read in the vehicle's frame through its
// joint 1 by `seat_measure`), the trike's pillion behind the driver on the rack.
Seat SeatOf(std::uint8_t kind) noexcept {
    switch (kind) {
        case 1:
        case 2: return Seat::passenger;
        case 3: return Seat::pillion;
        default: return Seat::none;
    }
}
bool DriverSeat(std::uint8_t kind, float (&off)[3]) noexcept {
    switch (kind) {
        case 1: off[0] = -0.31f, off[1] = -0.61f, off[2] = 0.66f; return true;
        case 2: off[0] = -0.44f, off[1] = 0.80f, off[2] = 1.95f; return true;
        case 3: off[0] = 0.0f, off[1] = -0.55f, off[2] = 0.02f; return true;
        default: return false;
    }
}
bool PassengerSeat(std::uint8_t kind, float (&off)[3]) noexcept {
    switch (kind) {
        case 1: off[0] = 0.31f, off[1] = -0.61f, off[2] = 0.66f; return true;
        case 2: off[0] = 0.44f, off[1] = 0.80f, off[2] = 1.95f; return true;
        case 3: off[0] = 0.0f, off[1] = -0.85f, off[2] = 0.12f; return true;
        default: return false;
    }
}
bool DoorPoint(std::uint8_t kind, float (&off)[3]) noexcept {
    switch (kind) {
        case 1: off[0] = 1.4f, off[1] = -0.6f, off[2] = 0.0f; return true;
        case 2: off[0] = 1.8f, off[1] = 0.8f, off[2] = 0.0f; return true;
        case 3: off[0] = 0.9f, off[1] = -0.9f, off[2] = 0.0f; return true;
        default: return false;
    }
}

void ToWorld(const double (&pos)[3], const float (&rows)[9], const float (&off)[3], double (&out)[3]) noexcept {
    for (int i = 0; i < 3; ++i)
        out[i] = pos[i] + static_cast<double>(off[0]) * rows[0 + i] + static_cast<double>(off[1]) * rows[3 + i] + static_cast<double>(off[2]) * rows[6 + i];
}
void ToLocal(const double (&pos)[3], const float (&rows)[9], const double (&world)[3], double (&out)[3]) noexcept {
    const double d[3] = {world[0] - pos[0], world[1] - pos[1], world[2] - pos[2]};
    for (int a = 0; a < 3; ++a) out[a] = d[0] * rows[a * 3 + 0] + d[1] * rows[a * 3 + 1] + d[2] * rows[a * 3 + 2];
}

// ---------------------------------------------------------------- rules
Block BoardCheck(const BoardIn& in) noexcept {
    if (!in.feature_on) return Block::off;
    if (!in.link_up) return Block::no_link;
    if (!in.partner_drives) return Block::no_vehicle;
    if (SeatOf(in.kind) == Seat::none) return Block::no_seat;
    if (!in.local_on_foot) return Block::not_on_foot;
    if (in.busy) return Block::busy;
    if (!(in.door_dist_m <= kBoardRadiusM)) return Block::too_far;
    if (!(std::fabs(in.vehicle_speed) < kBoardMaxSpeed)) return Block::moving;
    return Block::ok;
}

bool DriverAccepts(const DriverIn& in, Deny* why) noexcept {
    Deny d = Deny::count;
    if (!in.feature_on) d = Deny::off;
    else if (!in.same_world) d = Deny::world;
    else if (!in.we_drive_it) d = Deny::not_driving;
    else if (in.seat_taken) d = Deny::seat_taken;
    else if (!(std::fabs(in.speed) < kDriverMaxSpeed)) d = Deny::moving;
    else if (!in.have_partner || !(in.partner_dist_m <= kDriverSeesRadiusM)) d = Deny::not_near;
    if (why) *why = d;
    return d == Deny::count;
}

bool DriverPlacesBody(bool accepted, double since_accept_ms, double passenger_bit_age_ms) noexcept {
    if (!accepted) return false;
    return since_accept_ms < kAcceptGraceMs || passenger_bit_age_ms < kPassengerBitMs;
}
bool DriverTimedOut(bool accepted, double since_accept_ms, double passenger_bit_age_ms) noexcept {
    return accepted && since_accept_ms >= kDriverTimeoutMs && passenger_bit_age_ms >= kDriverTimeoutMs;
}

End ForcedExit(const ForcedIn& in) noexcept {
    if (in.stopped) return End::stopped;
    if (in.world_changed) return End::world;
    if (in.taken) return End::taken;
    if (in.link_silent_ms > kLinkSilentMs) return End::link;
    if (in.driver_ended) return End::driver_ended;
    if (!in.vehicle_live) return End::vehicle_gone;
    if (in.broken) return End::broken;
    if (in.crash_dv > kCrashDvMps) return End::crash;
    if (in.flipped) return End::flipped;
    if (in.not_driven_ms > kNotDrivenMs) return End::driver_off;
    if (in.driver_not_seated && std::fabs(in.speed) > kNotSeatedSpeed) return End::not_seated;
    return End::count;
}

bool HoldDetector::Update(bool down, double now_ms) noexcept {
    if (!down) {
        since_ = -1, fired_ = false, need_release_ = false;
        return false;
    }
    if (need_release_) return false;
    if (since_ < 0) since_ = now_ms;
    if (!fired_ && now_ms - since_ >= kHoldMs) {
        fired_ = true;
        need_release_ = true;
        return true;
    }
    return false;
}

bool FlipDetector::Update(double up_z, double now_ms) noexcept {
    if (!(up_z < kFlipUpZ)) {
        since_ = -1;
        return false;
    }
    if (since_ < 0) since_ = now_ms;
    return now_ms - since_ >= kFlipMs;
}

double CrashDetector::Update(const float (&vel)[3], double key_ms) noexcept {
    for (float v : vel)
        if (!std::isfinite(v)) return 0;
    if (have_ && key_ms <= key_) return 0;
    const bool close = have_ && key_ms - key_ <= 100.0;
    double dv = 0;
    if (close) {
        const double d[3] = {static_cast<double>(vel[0]) - vel_[0], static_cast<double>(vel[1]) - vel_[1], static_cast<double>(vel[2]) - vel_[2]};
        dv = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    }
    have_ = true;
    key_ = key_ms;
    std::memcpy(vel_, vel, sizeof(vel_));
    return dv > kCrashDvMps ? dv : 0;
}

ExitSpot ExitPlacement(std::uint8_t kind, const double (&pos)[3], const float (&rows)[9], const float (&vel)[3], bool forced) noexcept {
    ExitSpot s;
    float half[3];
    vwire::KindHalfExtents(kind, half);
    float door[3] = {1.5f, 0.0f, 0.0f};
    DoorPoint(kind, door);
    // the vehicle's right and forward axes on the ground plane (world z is up): a vehicle on its side has a vertical right
    // axis, one standing on its nose a vertical forward axis; the other one is then horizontal (they are orthonormal), and
    // the degenerate one is derived from it (right = forward x up)
    double rx = rows[0], ry = rows[1], fx = rows[3], fy = rows[4];
    const double rn = std::sqrt(rx * rx + ry * ry), fn = std::sqrt(fx * fx + fy * fy);
    constexpr double kMinHorizontal = 0.3;
    if (rn >= kMinHorizontal && fn >= kMinHorizontal) {
        rx /= rn, ry /= rn, fx /= fn, fy /= fn;
    } else if (rn >= fn && rn > 1e-6) {
        rx /= rn, ry /= rn, fx = -ry, fy = rx;
    } else if (fn > 1e-6) {
        fx /= fn, fy /= fn, rx = fy, ry = -fx;
    } else {
        rx = 1, ry = 0, fx = 0, fy = 1;
    }
    const double vlat = static_cast<double>(vel[0]) * rx + static_cast<double>(vel[1]) * ry;
    const double vfwd = static_cast<double>(vel[0]) * fx + static_cast<double>(vel[1]) * fy;
    int side = 1;
    if (forced && std::fabs(vlat) > 0.5) side = vlat > 0 ? -1 : 1;
    // the body's edge along the horizontal right direction on the chosen side: the body is the box right [-h0, h0], forward
    // [-h1, h1], up [0, 2 h2] in the vehicle's frame (its origin is its base: the seats' and doors' heights); upright that is
    // its half width, on its side its height or nothing (the underside)
    const double up_d = rx * rows[6] + ry * rows[7];
    double flat = 0;
    for (int a = 0; a < 2; ++a) flat += static_cast<double>(half[a]) * std::fabs(rx * rows[a * 3 + 0] + ry * rows[a * 3 + 1]);
    const double tall = 2.0 * static_cast<double>(half[2]) * up_d;
    const double edge = side > 0 ? flat + (std::max)(0.0, tall) : -flat + (std::min)(0.0, tall);
    double along = door[1];
    if (forced) {
        const double back = (std::min)(3.0, std::fabs(vfwd) * 0.25);
        along -= vfwd >= 0 ? back : -back;
    }
    const double out = edge + side * 0.7;
    s.pos[0] = pos[0] + out * rx + along * fx;
    s.pos[1] = pos[1] + out * ry + along * fy;
    // the height: the vehicle's base when upright (the origin, its ground plane); tilted, the lowest point of its body (the
    // box from the base up to twice its half height), so the game's ground probe starts near the ground beside a vehicle
    // lying on its side or roof, never inside or under it
    double low = 0;
    for (int a = 0; a < 2; ++a) low -= static_cast<double>(half[a]) * std::fabs(rows[a * 3 + 2]);
    low += (std::min)(0.0, 2.0 * static_cast<double>(half[2]) * rows[8]);
    s.pos[2] = pos[2] + low;
    // facing the vehicle: -side along the horizontal right axis
    s.facing[0] = static_cast<float>(-side * rx), s.facing[1] = static_cast<float>(-side * ry);
    s.side = side;
    return s;
}

}  // namespace st::rwire
