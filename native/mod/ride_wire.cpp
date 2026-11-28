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
        case MsgType::request: return r == kReqHold || r == kReqTest || r == kReqNative || r == kReqResume;
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
    if (!SeatValidFor(m.kind, static_cast<std::uint8_t>(m.seat))) return false;
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
        case 3: return "right front side";
        case 4: return "left front side";
        case 5: return "right rear side";
        case 6: return "left rear side";
        default: return "no seat";
    }
}
const char* DenyName(std::uint8_t r) noexcept {
    static const char* const k[] = {"the driver does not drive it", "the seat is taken", "the vehicle moves", "not beside the vehicle",
                                     "riding together is off on the driver's game", "another world", "that seat is not offered"};
    return r < sizeof(k) / sizeof(k[0]) ? k[r] : "?";
}
const char* EndName(std::uint8_t r) noexcept {
    static const char* const k[] = {"got off",          "link lost",    "the driver got off", "the vehicle is gone", "the vehicle is broken",
                                     "flipped",          "crash",        "the game took Sam",  "a load or the title", "the mod stopped",
                                     "the driver ended it", "nobody drives it", "not seen seated", "test", "jumped off",
                                     "the game saves"};
    return r < sizeof(k) / sizeof(k[0]) ? k[r] : "?";
}
const char* BlockName(Block b) noexcept {
    static const char* const k[] = {"ok", "off", "no link", "no partner vehicle", "not on foot", "too far", "the vehicle moves", "no seat", "busy",
                                     "the game is saving"};
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

bool SeatValidFor(std::uint8_t kind, std::uint8_t seat) noexcept {
    if (seat != 0 && seat == static_cast<std::uint8_t>(SeatOf(kind))) return true;
    return kind == 2 && SideSeat(seat);
}
bool SeatAnchor(std::uint8_t kind, std::uint8_t seat, float (&off)[3]) noexcept {
    if (SideSeat(seat)) {
        if (kind != 2) return false;
        off[0] = SideOf(seat) > 0 ? 1.299f : -1.299f;
        off[1] = seat <= 4 ? 0.127f : -0.692f;
        off[2] = 0.778f;
        return true;
    }
    return PassengerSeat(kind, off);
}
bool SideHandle(std::uint8_t seat, float (&off)[3]) noexcept {
    switch (seat) {
        case 3: off[0] = 1.149f, off[1] = 0.341f, off[2] = 2.187f; return true;
        case 4: off[0] = -1.166f, off[1] = 0.316f, off[2] = 2.160f; return true;
        case 5: off[0] = 1.170f, off[1] = -0.758f, off[2] = 2.170f; return true;
        case 6: off[0] = -1.179f, off[1] = -0.716f, off[2] = 2.163f; return true;
        default: return false;
    }
}
bool ApproachPoint(std::uint8_t kind, std::uint8_t seat, float (&off)[3]) noexcept {
    if (!SideSeat(seat)) return DoorPoint(kind, off);
    if (!SeatAnchor(kind, seat, off)) return false;
    off[0] += static_cast<float>(SideOf(seat) * kApproachOutM);
    off[2] = 0.0f;
    return true;
}
std::uint8_t PickSeat(std::uint8_t kind, bool sides, const double (&local)[3], std::uint32_t taken) noexcept {
    const auto own = static_cast<std::uint8_t>(SeatOf(kind));
    if (!sides || kind != 2) return own;
    for (double v : local)
        if (!std::isfinite(v)) return own;
    const auto hdist = [&](std::uint8_t s, double* d) {
        float a[3];
        if (!ApproachPoint(kind, s, a)) return false;
        const double dx = local[0] - a[0], dy = local[1] - a[1];
        *d = std::sqrt(dx * dx + dy * dy);
        return true;
    };
    double dd = 1e9;  // the cab's door
    if (!hdist(own, &dd)) dd = 1e9;
    std::uint8_t best = own;
    double bd = kPickRadiusM;
    for (std::uint8_t s : {std::uint8_t{3}, std::uint8_t{4}, std::uint8_t{5}, std::uint8_t{6}}) {
        if ((taken >> s) & 1u) continue;
        double d = 0;
        if (!hdist(s, &d)) continue;
        if (d < bd && d + kPickCabBiasM < dd) bd = d, best = s;
    }
    return best;
}

std::size_t EncodeHello(const Hello& h, std::uint8_t* out, std::size_t cap) noexcept {
    if (!out || cap < kHelloMessageSize || h.version < 1 || (h.role != 1 && h.role != 2)) return 0;
    if (vwire::WriteHeader(kHelloSvmType, out, cap) != kSvmHeaderSize) return 0;
    std::uint8_t* b = out + kSvmHeaderSize;
    std::memcpy(b, "STR1", 4);
    b[4] = h.version, b[5] = h.features, b[6] = h.role, b[7] = 0;
    return kHelloMessageSize;
}
bool DecodeHello(const std::uint8_t* in, std::size_t size, Hello& out) noexcept {
    std::uint8_t type = 0;
    if (!in || size != kHelloMessageSize || !vwire::PeekType(in, size, &type) || type != kHelloSvmType) return false;
    const std::uint8_t* b = in + kSvmHeaderSize;
    if (std::memcmp(b, "STR1", 4) != 0 || b[4] < 1 || (b[6] != 1 && b[6] != 2) || b[7] != 0) return false;
    out.version = b[4];
    out.features = static_cast<std::uint8_t>(b[5] & kAllFeatures);
    out.role = b[6];
    return true;
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
    if (in.saving) return Block::saving;
    if (in.native ? !(in.origin_dist_m <= kNativeReachM) : !(in.door_dist_m <= kBoardRadiusM)) return Block::too_far;
    if (!(std::fabs(in.vehicle_speed) < kBoardMaxSpeed)) return Block::moving;
    return Block::ok;
}

const char* NoPromptName(NoPrompt n) noexcept {
    switch (n) {
        case NoPrompt::board: return "board";
        case NoPrompt::idle: return "idle";
        case NoPrompt::not_allowed: return "boarding not allowed";
        case NoPrompt::too_soon: return "too soon";
        case NoPrompt::too_far: return "too far from the seat's approach point";
        case NoPrompt::game_prompt: return "the game's Use Vehicle is shown";
        case NoPrompt::other_vehicle: return "the game offers another vehicle";
        case NoPrompt::game_press: return "the game took the press";
        case NoPrompt::count: break;
    }
    return "?";
}

NoPrompt NoPromptPress(const NoPromptIn& in) noexcept {
    if (!in.native_on || !in.pressed) return NoPrompt::idle;
    if (!in.allowed) return NoPrompt::not_allowed;
    if (in.game_press_now || !(in.game_press_age_ms >= kNoPromptGameMs)) return NoPrompt::game_press;
    if (!(in.partner_prompt_age_ms >= kNoPromptQuietMs)) return NoPrompt::game_prompt;
    if (!(in.other_prompt_age_ms >= kNoPromptQuietMs)) return NoPrompt::other_vehicle;
    if (!(in.approach_dist_m <= kBoardRadiusM)) return NoPrompt::too_far;
    if (!(in.allowed_for_ms >= kNoPromptQuietMs) || !(in.since_last_ms >= kNoPromptGapMs)) return NoPrompt::too_soon;
    return NoPrompt::board;
}

bool DriverAccepts(const DriverIn& in, Deny* why) noexcept {
    Deny d = Deny::count;
    if (!in.feature_on) d = Deny::off;
    else if (!in.same_world) d = Deny::world;
    else if (!in.we_drive_it) d = Deny::not_driving;
    else if (in.seat_taken) d = Deny::seat_taken;
    else if (!(std::fabs(in.speed) < kDriverMaxSpeed)) d = Deny::moving;
    else if (!in.have_partner || !(in.native ? in.partner_origin_dist_m <= kDriverSeesNativeM || in.partner_dist_m <= kDriverSeesRadiusM
                                             : in.partner_dist_m <= kDriverSeesRadiusM))
        d = Deny::not_near;
    if (why) *why = d;
    return d == Deny::count;
}

bool DriverPlacesBody(bool accepted, double since_accept_ms, double passenger_bit_age_ms) noexcept {
    if (!accepted) return false;
    return since_accept_ms < kAcceptGraceMs || passenger_bit_age_ms < kPassengerBitMs;
}
bool DriverBodyFree(bool side_seat, bool resume, double since_accept_ms, double glide_bit_age_ms) noexcept {
    if (!side_seat || resume) return false;
    return since_accept_ms < kSideGlideFreeMs || glide_bit_age_ms < kGlideBitMs;
}
bool DriverTimedOut(bool accepted, double since_accept_ms, double passenger_bit_age_ms) noexcept {
    return accepted && since_accept_ms >= kDriverTimeoutMs && passenger_bit_age_ms >= kDriverTimeoutMs;
}

End ForcedExit(const ForcedIn& in) noexcept {
    if (in.stopped) return End::stopped;
    if (in.world_changed) return End::world;
    if (in.taken) return End::taken;
    if (in.saving) return End::saving;
    if (in.link_silent_ms > kLinkSilentMs && (in.link_lost || in.link_silent_ms > kLinkSilentCarriedMs)) return End::link;
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

namespace {
float Median(float* v, int n) noexcept {
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && v[j] < v[j - 1]; --j) std::swap(v[j], v[j - 1]);
    return n % 2 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}
}  // namespace

double CrashConfirm::Update(const float (&vel)[3], float speed_kmh, double key_ms) noexcept {
    if (!std::isfinite(vel[0]) || !std::isfinite(vel[1]) || !std::isfinite(key_ms)) return 0;
    if (n_ > 0 && key_ms <= s_[n_ - 1].key) return 0;
    // the one-frame jump of v0.20's rule (horizontal), as the candidate
    if (n_ > 0 && key_ms - s_[n_ - 1].key <= 100.0) {
        const double dx = vel[0] - s_[n_ - 1].vx, dy = vel[1] - s_[n_ - 1].vy;
        const double d = std::sqrt(dx * dx + dy * dy);
        if (d > kCrashDvMps && cand_key_ < -1e17) cand_key_ = key_ms, cand_dv_ = d;
    }
    if (n_ == kN) {
        for (int i = 1; i < kN; ++i) s_[i - 1] = s_[i];
        --n_;
    }
    S& s = s_[n_++];
    s.key = key_ms;
    s.vx = vel[0], s.vy = vel[1];
    s.kmh = std::isfinite(speed_kmh) && speed_kmh >= 0 ? std::fabs(speed_kmh) : -1.0f;
    // forget what is older than the windows
    int drop = 0;
    while (drop < n_ - 1 && s_[drop].key < key_ms - kConfirmBeforeFromMs - 100.0) ++drop;
    if (drop) {
        for (int i = drop; i < n_; ++i) s_[i - drop] = s_[i];
        n_ -= drop;
    }
    float bx[kN], by[kN], bk[kN], ax[kN], ay[kN], ak[kN];
    int nb = 0, na = 0, nbk = 0, nak = 0;
    for (int i = 0; i < n_; ++i) {
        const double age = key_ms - s_[i].key;
        if (age >= kConfirmBeforeToMs && age <= kConfirmBeforeFromMs) {
            bx[nb] = s_[i].vx, by[nb] = s_[i].vy, ++nb;
            if (s_[i].kmh >= 0) bk[nbk++] = s_[i].kmh;
        } else if (age <= kConfirmAfterMs) {
            ax[na] = s_[i].vx, ay[na] = s_[i].vy, ++na;
            if (s_[i].kmh >= 0) ak[nak++] = s_[i].kmh;
        }
    }
    double fired = 0;
    if (nb >= 2 && na >= 3 && key_ms - fired_key_ >= kConfirmRefireMs) {
        const double mdx = Median(ax, na) - Median(bx, nb), mdy = Median(ay, na) - Median(by, nb);
        const double dv = std::sqrt(mdx * mdx + mdy * mdy);
        bool agree = true;
        for (int i = n_ - 3; i < n_; ++i)
            for (int j = i + 1; j < n_; ++j) {
                const double ex = s_[i].vx - s_[j].vx, ey = s_[i].vy - s_[j].vy;
                if (std::sqrt(ex * ex + ey * ey) > kConfirmSpreadMps) agree = false;
            }
        const bool kmh_known = nbk >= 2 && nak >= 3;
        const bool kmh_ok = !kmh_known || Median(bk, nbk) - Median(ak, nak) >= kConfirmKmhDrop;
        if (dv > kCrashDvMps && agree && kmh_ok) {
            fired = dv;
            fired_key_ = key_ms;
            cand_key_ = -1e18;
        }
    }
    if (!fired && cand_key_ > -1e17 && key_ms - cand_key_ > kConfirmWaitMs) {
        rejected_ = cand_dv_;
        cand_key_ = -1e18;
    }
    return fired;
}

namespace {
// want_side: 0 the old rule's choice, else that side when not forced; along / out_m: the place along the vehicle and the
// distance beyond the body's edge.
ExitSpot ExitImpl(std::uint8_t kind, const double (&pos)[3], const float (&rows)[9], const float (&vel)[3], bool forced, int want_side, bool have_along,
                  double along_in, double out_m) noexcept {
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
    int side = want_side ? want_side : 1;
    if (forced && std::fabs(vlat) > 0.5) side = vlat > 0 ? -1 : 1;
    // the body's edge along the horizontal right direction on the chosen side: the body is the box right [-h0, h0], forward
    // [-h1, h1], up [0, 2 h2] in the vehicle's frame (its origin is its base: the seats' and doors' heights); upright that is
    // its half width, on its side its height or nothing (the underside)
    const double up_d = rx * rows[6] + ry * rows[7];
    double flat = 0;
    for (int a = 0; a < 2; ++a) flat += static_cast<double>(half[a]) * std::fabs(rx * rows[a * 3 + 0] + ry * rows[a * 3 + 1]);
    const double tall = 2.0 * static_cast<double>(half[2]) * up_d;
    const double edge = side > 0 ? flat + (std::max)(0.0, tall) : -flat + (std::min)(0.0, tall);
    double along = have_along ? along_in : door[1];
    if (forced) {
        const double back = (std::min)(3.0, std::fabs(vfwd) * 0.25);
        along -= vfwd >= 0 ? back : -back;
    }
    const double out = edge + side * out_m;
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
}  // namespace

ExitSpot ExitPlacement(std::uint8_t kind, const double (&pos)[3], const float (&rows)[9], const float (&vel)[3], bool forced) noexcept {
    return ExitImpl(kind, pos, rows, vel, forced, 0, false, 0, 0.7);
}
ExitSpot ExitPlacement(std::uint8_t kind, std::uint8_t seat, const double (&pos)[3], const float (&rows)[9], const float (&vel)[3], bool forced) noexcept {
    float a[3];
    if (!SideSeat(seat) || !SeatAnchor(kind, seat, a)) return ExitImpl(kind, pos, rows, vel, forced, 0, false, 0, 0.7);
    return ExitImpl(kind, pos, rows, vel, forced, SideOf(seat), true, a[1], kSideExitOutM);
}

// ---------------------------------------------------------------- the save guard
const char* SaveStepName(SaveStep s) noexcept {
    switch (s) {
        case SaveStep::run: return "run";
        case SaveStep::hold: return "hold";
        case SaveStep::run_capped: return "run (cap)";
    }
    return "?";
}

SaveStep SaveGate(const SaveHoldIn& in) noexcept {
    if (!in.pending || in.post_load || in.done) return SaveStep::run;
    const bool busy = in.carried || in.placing || in.in_vehicle || in.settled_frames < kSaveSettleFrames;
    if (!busy) return SaveStep::run;
    if (in.held_ms >= kSaveHoldCapMs) return SaveStep::run_capped;
    return SaveStep::hold;
}

bool InVehicleBox(std::uint8_t kind, const double (&veh)[3], const float (&rows)[9], const double (&p)[3]) noexcept {
    float half[3];
    vwire::KindHalfExtents(kind, half);
    const double d[3] = {p[0] - veh[0], p[1] - veh[1], p[2] - veh[2]};
    double l[3];
    for (int i = 0; i < 3; ++i) l[i] = d[0] * rows[i * 3] + d[1] * rows[i * 3 + 1] + d[2] * rows[i * 3 + 2];
    for (double v : l)
        if (!std::isfinite(v)) return false;
    return std::fabs(l[0]) <= half[0] + kBoxMarginM && std::fabs(l[1]) <= half[1] + kBoxMarginM && l[2] >= -kBoxMarginBelowM &&
           l[2] <= 2.0 * half[2] + kBoxMarginBelowM;
}

double SaveSpotHeight(const double (&veh)[3], const float (&rows)[9], const double (&spot)[3]) noexcept {
    double z = spot[2];
    const double nz = rows[6 + 2];
    // upright enough: the vehicle's own ground plane (through its base, normal its up axis) at the spot, so a slope that
    // rises toward the spot never puts it inside the hillside
    if (std::isfinite(nz) && nz >= 0.5) {
        const double plane = veh[2] - (rows[6] * (spot[0] - veh[0]) + rows[7] * (spot[1] - veh[1])) / nz;
        if (std::isfinite(plane) && plane > z) z = plane;
    }
    return z + kSaveSpotLiftM;
}

}  // namespace st::rwire
