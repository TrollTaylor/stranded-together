// Riding together (design M3). See ride_together.hpp, ride_wire.hpp and docs/real-partner-body.md section 24.
#include "ride_together.hpp"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "automation.hpp"
#include "coop_session.hpp"
#include "game_actions.hpp"
#include "link.hpp"
#include "log.hpp"
#include "overlay/overlay_input.hpp"
#include "partner_source.hpp"
#include "pose_wire.hpp"
#include "real_body.hpp"
#include "ride_wire.hpp"
#include "save_guard.hpp"
#include "vehicle_drive.hpp"
#include "vehicle_share.hpp"
#include "vehicle_wire.hpp"

namespace st::ride {
namespace {
namespace mem = partner::mem;
using game::WorldTransform;

constexpr std::uintptr_t kImage = 0x140000000ull;
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

// ---------------------------------------------------------------- game addresses (ds.exe, Director's Cut)
constexpr std::uintptr_t kEntityTeleport = 0x14235A010;  // Entity teleport (entity, WorldTransform*, 0, 0): PlaceOn mode 0's call
constexpr std::uintptr_t kCapsuleOff = 0x14243A740;      // the capsule's disable: ++[c+0x58], then its switch (c, 0)
constexpr std::uintptr_t kCapsuleOn = 0x14243A9E0;       // the capsule's enable: --[c+0x58], on again when it reaches 0
constexpr std::uintptr_t kCounterInc = 0x14240D540;      // inc dword [rcx]; ret
constexpr std::uintptr_t kCounterDec = 0x142417680;      // dec dword [rcx]; ret
constexpr std::uintptr_t kCapsuleSwitch = 0x14243AA30;   // the switch: [c+0x60] the physics object, [c+0x5C] on, [c+0x48] entity
constexpr std::uintptr_t kCapsuleVt = 0x143D1BD08;       // the player capsule component's vtable (Sam +0x5A0 refers to it + 0x20)
constexpr std::uintptr_t kSamCapsuleRef = 0x5A0;
constexpr std::uintptr_t kCapCounter = 0x58, kCapOn = 0x5C, kCapEntity = 0x48;
constexpr std::uintptr_t kEParent = 0x70;
constexpr std::uintptr_t kVehVt[4] = {0, 0x143E274D8, 0x143E26790, 0x143E25DB8};  // roadster, truck, trike entities (vehicle_drive)
constexpr std::uintptr_t kVRideState = 0x4CC, kVFlags560 = 0x560, kVUnit = 0x8A8, kUEntRef = 0x68;

struct Site {
    std::uintptr_t va;
    const char* sig;
    const char* name;
};
// clang-format off
constexpr Site kSites[] = {
    {kEntityTeleport, "48 8B C4 48 89 58 18 48 89 70 20 55 57 41 56 48 8D A8 48 FF FF FF 48 81", "Entity teleport"},
    {kCapsuleOff,     "40 53 48 83 EC 20 48 8B D9 48 83 C1 58 E8 EE 2D FD FF 33 D2 48 8B CB 48 83 C4 20 5B E9", "capsule disable (+0x58 counter, switch off)"},
    {kCapsuleOn,      "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 83 C1 58 E8 8A CC FD FF 48 8B 43 48 48 8B 90 88 00 00 00 48 C1 EA 0E F6 C2 01 74 0F 83 7B 58 00",
                      "capsule enable (+0x58 counter, the entity's collision flag)"},
    {kCounterInc,     "FF 01 C3", "the capsule counter increment"},
    {kCounterDec,     "FF 09 C3", "the capsule counter decrement"},
    {kCapsuleSwitch,  "40 57 48 83 EC 60 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 58 44 0F B6 C2 48 8B F9 48 8B 51 60 48 85 D2 0F 84 ?? ?? ?? ?? 0F B6 41 5C",
                      "the capsule switch (+0x60 physics, +0x5C on)"},
};
// clang-format on

using FnTeleport = void (*)(std::uintptr_t, const WorldTransform*, std::uint8_t, std::uint8_t);
using FnVoid1 = void (*)(std::uintptr_t);

// ---------------------------------------------------------------- state
bool g_checks_ok = false;
bool g_cfg_on = true;
std::atomic<bool> g_on{false};
std::atomic<bool> g_stop{false};       // the emergency stop: AfterStop restores once
char g_off[200] = "not installed";
int g_role = 0;                        // 1 host, 2 guest (vehicle_share's)
double g_now = 0;
double g_dt_ms = 16.7;                 // the tick interval (EMA)
double g_last_tick = 0;
std::uint32_t g_gen = 0;
TickIn g_in;                           // this tick's input (the handler reads it)
CrashHitFn g_crash_hit = nullptr;
bool g_notice_done = false;

// seats: the table, or a tuning override per kind (ride_test 3)
bool g_seat_override[4] = {};
float g_seat_ov[4][3] = {};
bool SeatOffset(std::uint8_t kind, float (&off)[3]) {
    if (kind >= 1 && kind <= 3 && g_seat_override[kind]) {
        std::memcpy(off, g_seat_ov[kind], sizeof(off));
        return true;
    }
    return rwire::PassengerSeat(kind, off);
}

// The passenger's side (our Sam carried)
enum class PState : std::uint8_t { idle, requesting, riding };
struct Pass {
    PState st = PState::idle;
    bool solo = false;                 // ride_test 1: the seat check, no partner
    std::uint32_t ride_id = 0;
    vwire::VKey key;
    std::uint8_t kind = 0;
    std::uintptr_t entity = 0, unit = 0;
    std::uintptr_t sam = 0, cap = 0;
    bool cap_off = false;
    std::int32_t cap_counter0 = 0;
    std::uint32_t world = 0;
    double req_ms = 0, since = 0;
    double not_driven_since = 0, gone_since = 0, asleep_since = 0;
    bool driver_ended = false;
    std::uint8_t driver_reason = 0;
    float health0 = -1;
    double next_health_ms = 0;
    bool have_last = false;
    WorldTransform last_target{};
    WorldTransform last_xf{};
    bool have_tp_xf = false;           // the vehicle's transform at our newest teleport of Sam (where he was left)
    WorldTransform tp_xf{};
    bool tp_fault = false;             // the entity teleport faulted this ride: never again for the exit
    double next_seq_ms = 0;            // the sequence answer (Taken), every 250 ms
    float last_vel[3] = {};
    double last_speed = 0;
    // meters (one line a second)
    double err_sum = 0, err_max = 0, err_veh_sum = 0, err_veh_max = 0;
    unsigned err_n = 0, teleports = 0, tp_faults = 0;
    double ride_err_sum = 0, ride_err_max = 0;  // the whole ride (the summary line)
    unsigned ride_err_n = 0, ride_err_over = 0; // ... and the ticks over 0.25 m (a follower teleport moved the vehicle)
    double log_ms = 0;
    rwire::FlipDetector flip;
    rwire::CrashDetector crash;
    double crash_dv = 0;
    double test_crash_dv = 0;
    bool test_taken = false;
    double exit_wait_until = 0;        // a hold to get off came while too fast: off once it slows down (until then)
};
Pass g_p;
rwire::HoldDetector g_hold;            // board (idle) / get off (riding)
double g_cooldown_until = 0;
double g_prompt_ms = 0;
// Boarding waits while our Sam was just hurt (a vehicle hit, a fall: he lies on the ground and gets up): no ride prompt
// beside the partner's vehicle that just knocked him down (seen in run 20261119-164745, H1b).
constexpr double kHurtBoardBlockMs = 5000.0;
float g_board_health = -1;
double g_board_health_ms = 0, g_hurt_until = 0;
unsigned g_boards = 0, g_exits = 0, g_denied = 0, g_requests = 0;

// The driver's side (the partner carried in our vehicle)
struct Drv {
    bool accepted = false;
    std::uint32_t ride_id = 0;
    vwire::VKey key;
    std::uintptr_t entity = 0;
    double accept_ms = 0, last_bit_ms = -1e18;
    bool placed_logged = false;
    double err_sum = 0, err_max = 0;
    unsigned err_n = 0;
    double log_ms = 0;
};
Drv g_d;
unsigned g_accepts = 0, g_denies = 0, g_ends_rx = 0, g_ends_tx = 0, g_bad_rx = 0;

// A pending ground placement after the exit: PlaceOn refused for its rate limit (retried for 2 s after the entity
// teleport), or not possible yet (our Sam asleep in a menu, the game writing a save: retried for up to 2 minutes). Dropped when the
// game takes Sam meanwhile or he was moved over 4 m from where he was left.
constexpr double kPlaceRetryMs = 2000.0, kPlaceWaitMs = 120000.0, kPlaceMovedM = 4.0;
struct PendingPlace {
    bool on = false;
    double until = 0, next_ms = 0;
    double pos[3] = {};
    float facing[2] = {};
    bool have_from = false;
    double from[3] = {};               // our Sam's place when it was armed
    bool at_spot = false;              // our Sam already stands at the spot (the entity teleport while he slept, for a save,
                                       // at a ground-safe height): the save guard need not wait for this PlaceOn
};
PendingPlace g_place;

// The save guard's view of the ride (save_guard.hpp): our Sam carried; his exit placement still waiting for PlaceOn; the
// vehicle and its kind (the guard checks that he is out of its box before it lets a held capture run).
void PublishSaveState() noexcept {
    const bool riding = g_p.st == PState::riding;
    saveguard::SetRideState(riding, g_place.on && !g_place.at_spot, riding ? g_p.entity : 0, riding ? g_p.kind : 0, riding ? g_p.last_speed : 0.0);
}

// hidden tests (ride_test 5): our driven vehicle rolled over about its forward axis, 4 deg a tick around a point 1 m above
// its origin, by the game's own deferred vehicle teleport (a "flip" whose frames show no velocity jump)
unsigned g_roll_left = 0;

// seat measure
std::uintptr_t g_meas_e = 0, g_meas_unit = 0;
std::uint8_t g_meas_kind = 0;

// ---------------------------------------------------------------- reads
bool ReadXf(std::uintptr_t e, WorldTransform& out) noexcept {
    std::uint8_t b1[0x3C], b2[0x3C];
    if (!e || !mem::Read(e + 0xC8, b1, sizeof(b1)) || !mem::Read(e + 0xC8, b2, sizeof(b2)) || std::memcmp(b1, b2, sizeof(b1)) != 0) return false;
    std::memcpy(&out.pos, b1, 0x18);
    std::memcpy(&out.rot, b1 + 0x18, 0x24);
    if (!std::isfinite(out.pos.x) || !std::isfinite(out.pos.y) || !std::isfinite(out.pos.z) || std::fabs(out.pos.x) > 1e7 || std::fabs(out.pos.y) > 1e7 ||
        std::fabs(out.pos.z) > 1e7)
        return false;
    for (const auto& row : out.rot.m)
        for (float v : row)
            if (!std::isfinite(v) || std::fabs(v) > 1.5f) return false;
    return true;
}
int KindOfEntity(std::uintptr_t e) noexcept {
    if (!e) return 0;
    const std::uintptr_t vt = mem::Q(e);
    for (int k = 1; k <= 3; ++k)
        if (vt == A(kVehVt[k])) return k;
    return 0;
}
bool VehicleLive(std::uintptr_t e, std::uintptr_t unit, std::uint8_t kind) noexcept {
    return e && unit && KindOfEntity(e) == kind && mem::Q(e + kVUnit) == unit && mem::Q(unit + kUEntRef) == e + 0x20;
}
const float (&Rows(const WorldTransform& t))[9] { return *reinterpret_cast<const float(*)[9]>(&t.rot); }
void PosOf(const WorldTransform& t, double (&p)[3]) noexcept { p[0] = t.pos.x, p[1] = t.pos.y, p[2] = t.pos.z; }
double Dist2D(const double (&a)[3], const double (&b)[3]) noexcept { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1])); }
double Dist3(const WorldTransform& a, const WorldTransform& b) noexcept {
    const double x = a.pos.x - b.pos.x, y = a.pos.y - b.pos.y, z = a.pos.z - b.pos.z;
    return std::sqrt(x * x + y * y + z * z);
}

// The seat's world transform on a vehicle at `xf`, moved `lead_s` along its velocity: the vehicle's rotation, the seat's place.
WorldTransform SeatXf(const WorldTransform& xf, const float (&vel)[3], double lead_s, const float (&seat)[3]) noexcept {
    double p[3];
    PosOf(xf, p);
    for (int i = 0; i < 3; ++i) p[i] += static_cast<double>(vel[i]) * lead_s;
    double w[3];
    rwire::ToWorld(p, Rows(xf), seat, w);
    WorldTransform out = xf;
    out.pos.x = w[0], out.pos.y = w[1], out.pos.z = w[2];
    return out;
}

// The local Sam's capsule component (Sam +0x5A0 refers to it + 0x20; its vtable and its entity checked), 0 when not.
std::uintptr_t SamCapsule(std::uintptr_t sam) noexcept {
    const std::uintptr_t ref = sam ? mem::Q(sam + kSamCapsuleRef) : 0;
    if (!ref) return 0;
    const std::uintptr_t c = ref - 0x20;
    return mem::Q(c) == A(kCapsuleVt) && mem::Q(c + kCapEntity) == sam ? c : 0;
}
std::int32_t CapCounter(std::uintptr_t c) noexcept {
    std::int32_t n = -999;
    if (c) mem::Rd(c + kCapCounter, n);
    return n;
}
int CapOnByte(std::uintptr_t c) noexcept {
    std::uint8_t b = 0xFF;
    if (c) mem::Rd(c + kCapOn, b);
    return b;
}

// ---------------------------------------------------------------- SEH calls (POD locals only)
bool CallTeleport(std::uintptr_t sam, const WorldTransform* wt) noexcept {
    __try {
        reinterpret_cast<FnTeleport>(A(kEntityTeleport))(sam, wt, 0, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallCapsule(std::uintptr_t fn, std::uintptr_t cap) noexcept {
    __try {
        reinterpret_cast<FnVoid1>(fn)(cap);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------- messages
std::uint32_t OurWorld() noexcept {
    std::uint32_t c = 0;
    vshare::WorldReadyNow(&c);
    return c;
}
void Send(rwire::MsgType type, std::uint32_t ride_id, std::uint32_t vehicle_id, std::uint8_t kind, std::uint8_t reason) {
    rwire::Msg m;
    m.type = type;
    m.seat = rwire::SeatOf(kind);
    m.reason = reason;
    m.from = g_role == 1 ? 0 : 1;
    m.ride_id = ride_id;
    m.sender_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(g_now));
    m.vehicle_id = vehicle_id;
    m.kind = kind;
    m.world = OurWorld();
    std::uint8_t buf[rwire::kMessageSize];
    const std::size_t n = rwire::EncodeMsg(m, buf, sizeof(buf));
    const bool ok = n && vshare::SendEvent(buf, n);
    if (type == rwire::MsgType::end && ok) ++g_ends_tx;
    const char* rs = type == rwire::MsgType::deny ? rwire::DenyName(reason) : type == rwire::MsgType::end ? rwire::EndName(reason) : "";
    Log("[ride] %s: %s %s ride #%08X %s 0x%06X%s%s%s", g_role == 1 ? "host" : "guest", ok ? "sent" : "could not send", rwire::MsgTypeName(static_cast<std::uint8_t>(type)),
        ride_id, vwire::KindName(kind), vehicle_id, rs[0] ? " (" : "", rs, rs[0] ? ")" : "");
}

void Notice(const char* text, bool warn, double ms) { coop::SetVehicleNotice(text, warn, g_now + ms); }
void PeerLabel(char* out, std::size_t cap) {
    char name[96] = {};
    std::uint32_t gen = 0;
    bool from_host = false;
    if (link::PeerName(name, sizeof(name), &gen, &from_host) && name[0]) std::snprintf(out, cap, "%s", name);
    else std::snprintf(out, cap, "%s", g_role == 1 ? "your partner" : "the host");
}

// ---------------------------------------------------------------- the passenger: carry
void ApplyCarryState(bool on) {
    overlay::input::SetRideCarry(on);
    realbody::SetRidePose(on ? (g_p.solo ? 2 : 1) : 0);
    vdrive::SetCarriedUnit(on ? g_p.unit : 0);
}

// Turns the capsule back on exactly once (it is a counter), and only on the Sam we turned it off on.
void CapsuleBack(const char* why) {
    if (!g_p.cap_off) return;
    g_p.cap_off = false;
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(g_in.local);
    const std::uintptr_t cap = sam == g_p.sam ? SamCapsule(sam) : 0;
    if (!cap || cap != g_p.cap) {
        Log("[ride] capsule: not turned back on (%s): our Sam is another entity now (a load: the game made a new one)", why);
        return;
    }
    const std::int32_t before = CapCounter(cap);
    const bool ok = CallCapsule(A(kCapsuleOn), cap);
    Log("[ride] capsule back ON (%s): counter %d -> %d, on byte %d%s", why, before, CapCounter(cap), CapOnByte(cap), ok ? "" : " (FAULT in the enable)");
}

void ArmPlace(const double (&pos)[3], const float (&facing)[2], const double* from, double ms) {
    g_place = PendingPlace{};
    g_place.on = true;
    g_place.until = g_now + ms;
    g_place.next_ms = g_now + 100;
    std::memcpy(g_place.pos, pos, sizeof(g_place.pos));
    std::memcpy(g_place.facing, facing, sizeof(g_place.facing));
    if (from) {
        g_place.have_from = true;
        std::memcpy(g_place.from, from, sizeof(g_place.from));
    }
}

// Puts our Sam beside the vehicle: the game's PlaceOn (mode 1: its ground probe) at the exit spot, facing the vehicle.
// game::PlacementGate first, so the mod never warps Sam where the game's own guards would refuse:
//   the game has Sam (a load, a stopping game event, the post-load setup, a parent, Sam+0x678, a snap or pushed transform,
//     a sequence): no placement (the game places him; the capsule comes back on where he is);
//   Sam asleep (a menu), a save being captured / written, the world waiting for a scene: the placement waits (up to 2
//     minutes, while he stays where he was left; for a save the guard holds, a sleeping Sam is put at the spot first, at a
//     ground-safe height, only when the game's other checks (gate 2: a sequence, a stopping game state ...) pass too);
//   PlaceOn refused for its rate limit only, or the teleport off (its byte checks) with Sam awake: the entity teleport
//     there (the carry's own call; never after it faulted this ride) and PlaceOn again for 2 s.
// The spot is beside the vehicle where our Sam is: its transform now, or at our newest teleport when he was left behind (a
// menu while it drove on).
void PlaceBeside(bool forced, bool allow_raw) {
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(g_in.local);
    WorldTransform sam_xf{};
    const bool have_sam = sam && sam == g_p.sam && ReadXf(sam, sam_xf);
    WorldTransform xf{};
    bool have = VehicleLive(g_p.entity, g_p.unit, g_p.kind) && ReadXf(g_p.entity, xf);
    float seat[3] = {};
    SeatOffset(g_p.kind, seat);
    const float zero[3] = {};
    if (have && have_sam && g_p.have_tp_xf && Dist3(SeatXf(xf, zero, 0, seat), sam_xf) > 3.0) {
        Log("[ride] our Sam is %.1f m from the seat (left behind): put beside where the %s was at our last teleport", Dist3(SeatXf(xf, zero, 0, seat), sam_xf),
            vwire::KindName(g_p.kind));
        xf = g_p.tp_xf;
    } else if (!have) {
        if (g_p.have_tp_xf) xf = g_p.tp_xf;
        else if (g_p.have_last) xf = g_p.last_xf;
        else return;
    }
    double p[3];
    PosOf(xf, p);
    const rwire::ExitSpot s = rwire::ExitPlacement(g_p.kind, p, Rows(xf), g_p.last_vel, forced);
    const double spot[3] = {s.pos[0], s.pos[1], s.pos[2] + 0.5};
    double from[3] = {};
    if (have_sam) PosOf(sam_xf, from);
    char why[160] = {};
    const int gate = game::PlacementGate(why, sizeof(why));
    if (gate == 2) {
        Log("[ride] not put beside the %s: the game has our Sam (%s); the game places him", vwire::KindName(g_p.kind), why);
        return;
    }
    if (gate == 1) {
        // A save waits for our Sam (the save guard holds its capture) while he sleeps in a menu (the pause menu's Save):
        // PlaceOn refuses a sleeping Sam, so the carry's own entity teleport puts him at the spot now (the save then records
        // him there), and PlaceOn follows once he is awake. Never while the game captures or writes a save, and never where
        // the game has him (the same checks as an awake placement: a sequence, a stopping game state, a snap ...). There is
        // no ground probe: the spot is lifted to the vehicle's own ground plane there (a slope rising beside it) plus
        // kSaveSpotLiftM, so the save never records him inside the ground (he drops onto it).
        bool for_save = saveguard::WantsOff() && !saveguard::InProgress() && !g_in.local_active && have_sam && allow_raw;
        char why2[160] = {};
        if (for_save) {
            const int awake_gate = game::PlacementGate(why2, sizeof(why2), true);
            if (awake_gate != 0) {
                for_save = false;
                Log("[ride] a save waits and our Sam sleeps (%s), but %s (gate %d): no entity teleport; the placement waits", why, why2, awake_gate);
            }
        }
        bool raw = false;
        double save_spot[3] = {spot[0], spot[1], spot[2]};
        if (for_save) {
            save_spot[2] = rwire::SaveSpotHeight(p, Rows(xf), s.pos);
            WorldTransform t{};
            t.pos.x = save_spot[0], t.pos.y = save_spot[1], t.pos.z = save_spot[2];
            t.rot.m[0][0] = s.facing[1], t.rot.m[0][1] = -s.facing[0], t.rot.m[0][2] = 0;
            t.rot.m[1][0] = s.facing[0], t.rot.m[1][1] = s.facing[1], t.rot.m[1][2] = 0;
            t.rot.m[2][0] = 0, t.rot.m[2][1] = 0, t.rot.m[2][2] = 1;
            raw = CallTeleport(g_p.sam, &t);
            Log("[ride] a save waits and our Sam sleeps (%s): entity teleport beside the %s %s at (%.2f, %.2f, %.2f) (ground-safe: %.2f m over the exit "
                "spot's height)",
                why, vwire::KindName(g_p.kind), raw ? "done" : "FAULTED", save_spot[0], save_spot[1], save_spot[2], save_spot[2] - s.pos[2]);
        }
        ArmPlace(spot, s.facing, raw ? &save_spot[0] : (have_sam ? &from[0] : nullptr), kPlaceWaitMs);
        g_place.at_spot = raw;
        Log("[ride] put beside the %s once possible (%s): waiting up to %.0f s at (%.2f, %.2f, %.2f)", vwire::KindName(g_p.kind), why, kPlaceWaitMs / 1000, spot[0],
            spot[1], spot[2]);
        return;
    }
    if (gate == 0 &&
        game::TeleportLocalPlayerFacing(spot[0], spot[1], spot[2], s.facing[0], s.facing[1], game::TeleportMode::place, why, sizeof(why))) {
        Log("[ride] put beside the %s on its %s side at (%.2f, %.2f, %.2f): %s", vwire::KindName(g_p.kind), s.side > 0 ? "right" : "left", spot[0], spot[1], spot[2],
            why);
        return;
    }
    // only the rate limit refused (gate 0), or the teleport is off (gate 3)
    if (!allow_raw || !g_in.local_active || !have_sam) {
        Log("[ride] PlaceOn refused (%s): no entity teleport (%s); %s", why, !allow_raw ? "it faulted this ride" : !have_sam ? "our Sam is unreadable" : "our Sam is asleep",
            gate == 0 ? "PlaceOn again for 2 s" : "our Sam stays where he is");
        if (gate == 0) ArmPlace(spot, s.facing, have_sam ? &from[0] : nullptr, kPlaceRetryMs);
        return;
    }
    WorldTransform t{};
    t.pos.x = spot[0], t.pos.y = spot[1], t.pos.z = spot[2];
    t.rot.m[0][0] = s.facing[1], t.rot.m[0][1] = -s.facing[0], t.rot.m[0][2] = 0;
    t.rot.m[1][0] = s.facing[0], t.rot.m[1][1] = s.facing[1], t.rot.m[1][2] = 0;
    t.rot.m[2][0] = 0, t.rot.m[2][1] = 0, t.rot.m[2][2] = 1;
    const bool ok = CallTeleport(g_p.sam, &t);
    Log("[ride] PlaceOn refused (%s): entity teleport beside the %s %s%s", why, vwire::KindName(g_p.kind), ok ? "done" : "FAULTED",
        gate == 0 ? "; PlaceOn again for 2 s" : "");
    if (gate == 0) ArmPlace(spot, s.facing, ok ? &spot[0] : (have_sam ? &from[0] : nullptr), kPlaceRetryMs);
}

void PendingPlaceTick() {
    if (!g_place.on) return;
    if (g_now > g_place.until) {
        g_place.on = false;
        Log("[ride] the exit's placement never went through: our Sam stays where he is");
        return;
    }
    if (g_now < g_place.next_ms) return;
    g_place.next_ms = g_now + 100;
    char why[160] = {};
    const int gate = game::PlacementGate(why, sizeof(why));
    if (gate == 1) return;  // still asleep or saving
    if (gate != 0) {
        g_place.on = false;
        Log("[ride] the exit's placement dropped: %s", why);
        return;
    }
    if (g_place.have_from) {
        WorldTransform x{};
        const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(g_in.local);
        if (!sam || !ReadXf(sam, x)) return;
        const double dx = x.pos.x - g_place.from[0], dy = x.pos.y - g_place.from[1], dz = x.pos.z - g_place.from[2];
        const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > kPlaceMovedM) {
            g_place.on = false;
            Log("[ride] the exit's placement dropped: our Sam was moved %.1f m since (the game placed him)", d);
            return;
        }
    }
    if (game::TeleportLocalPlayerFacing(g_place.pos[0], g_place.pos[1], g_place.pos[2], g_place.facing[0], g_place.facing[1], game::TeleportMode::place, why,
                                        sizeof(why))) {
        g_place.on = false;
        Log("[ride] the exit's PlaceOn went through: %s", why);
    }
}
bool PendingPlaceGuarded() noexcept {
    __try {
        PendingPlaceTick();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_place.on = false;
        return false;
    }
}

void EndCarry(rwire::End reason, bool place, bool tell_driver) {
    if (g_p.st != PState::riding) return;
    const bool forced = reason != rwire::End::choice && reason != rwire::End::test;
    const double rode = (g_now - g_p.since) / 1000.0;
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(g_in.local);
    const bool same_sam = sam && sam == g_p.sam;
    ApplyCarryState(false);
    if (place && same_sam) PlaceBeside(forced, !g_p.tp_fault);
    CapsuleBack(rwire::EndName(static_cast<std::uint8_t>(reason)));
    ++g_exits;
    Log("[ride] %s: our Sam off the %s 0x%06X (#%u, %s%s) after %.1f s; Sam vs the seat over the ride mean %.3f m, max %.3f m, %u of %u ticks over "
        "0.25 m; %u teleports%s",
        g_role == 1 ? "host" : g_role == 2 ? "guest" : "solo", vwire::KindName(g_p.kind), g_p.key.id, g_exits, rwire::EndName(static_cast<std::uint8_t>(reason)),
        forced ? ", forced" : "", rode, g_p.ride_err_n ? g_p.ride_err_sum / g_p.ride_err_n : 0.0, g_p.ride_err_max, g_p.ride_err_over, g_p.ride_err_n,
        g_p.teleports, g_p.solo ? " (solo seat test)" : "");
    const bool jumped_fast = reason == rwire::End::jumped && g_p.last_speed > rwire::kCrashDvMps;
    if ((reason == rwire::End::crash || jumped_fast) && same_sam) {
        // D3-3: the passenger takes the game's own hit and knock-down (M4's handler); also when he jumps off above 8 m/s
        const double dv = jumped_fast ? g_p.last_speed : g_p.crash_dv;
        bool hit = false;
        if (g_crash_hit && VehicleLive(g_p.entity, g_p.unit, g_p.kind)) hit = g_crash_hit(g_p.entity, dv, g_p.last_speed);
        Log("[ride] %s exit (dv %.1f m/s at %.1f m/s): %s", jumped_fast ? "jump-off" : "crash", dv, g_p.last_speed,
            !g_crash_hit ? "no hit handler (M4 partner_tackle not installed): put off only" : hit ? "the game's hit applied" : "the hit handler refused");
    }
    if (tell_driver && !g_p.solo) Send(rwire::MsgType::end, g_p.ride_id, g_p.key.id, g_p.kind, static_cast<std::uint8_t>(reason));
    char text[160];
    if (forced && reason != rwire::End::stopped) {
        std::snprintf(text, sizeof(text), "Off the %s: %s.", vwire::KindName(g_p.kind), rwire::EndName(static_cast<std::uint8_t>(reason)));
        Notice(text, true, 4000);
    } else {
        Notice("", false, 0);
    }
    g_p = Pass{};
    g_hold.Reset();
    g_cooldown_until = g_now + 1500;
    PublishSaveState();
}

bool StartCarry(const vwire::VKey& key, std::uint8_t kind, std::uintptr_t entity, std::uintptr_t unit, std::uint32_t ride_id, bool solo) {
    if (g_stop.load()) {
        Log("[ride] cannot carry our Sam: riding together stopped");
        return false;
    }
    if (!saveguard::ReadsOk()) {
        Log("[ride] cannot carry our Sam: the save checks are off (this game build differs): a save could record him at the seat");
        return false;
    }
    if (saveguard::Busy() && !saveguard::TestAllowsCarry()) {
        Log("[ride] cannot carry our Sam: the game has a save pending or is saving");
        return false;
    }
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(g_in.local);
    const std::uintptr_t cap = SamCapsule(sam);
    if (!sam || !cap) {
        Log("[ride] cannot carry our Sam: %s", !sam ? "no local Sam" : "his capsule component does not check (Sam +0x5A0)");
        return false;
    }
    float seat[3];
    if (!SeatOffset(kind, seat)) return false;
    g_p = Pass{};
    g_p.st = PState::riding;
    g_p.solo = solo;
    g_p.ride_id = ride_id;
    g_p.key = key;
    g_p.kind = kind;
    g_p.entity = entity;
    g_p.unit = unit;
    g_p.sam = sam;
    g_p.cap = cap;
    g_p.since = g_now;
    g_p.world = OurWorld();
    g_p.log_ms = g_now;
    g_p.cap_counter0 = CapCounter(cap);
    const bool ok = CallCapsule(A(kCapsuleOff), cap);
    g_p.cap_off = ok;
    if (!ok) {
        Log("[ride] FAULT in the capsule disable: not carried");
        g_p = Pass{};
        return false;
    }
    ApplyCarryState(true);
    PublishSaveState();
    g_hold.Reset();  // the hold that boarded must be released before one gets off
    ++g_boards;
    Log("[ride] %s: our Sam carried on the %s 0x%06X (%s, ride #%08X, #%u): capsule off (counter %d -> %d, on byte %d), seat (%.2f, %.2f, %.2f), "
        "input blocked, pose %s",
        g_role == 1 ? "host" : g_role == 2 ? "guest" : "solo", vwire::KindName(kind), key.id, rwire::SeatName(static_cast<std::uint8_t>(rwire::SeatOf(kind))), ride_id,
        g_boards, g_p.cap_counter0, CapCounter(cap), CapOnByte(cap), seat[0], seat[1], seat[2], solo ? "the kept driver pose (solo test)" : "the driver's");
    char label[96], text[200];
    PeerLabel(label, sizeof(label));
    if (solo) std::snprintf(text, sizeof(text), "Seat test: carried on the %s.", vwire::KindName(kind));
    else std::snprintf(text, sizeof(text), "Riding with %s. Hold [X] / [F] to get off.", label);
    Notice(text, false, 5000);
    return true;
}

// The game takes our Sam. Two kinds:
//   game  the game moves or holds him itself (a parent, a load, a cutscene / game-over / fast-travel state current or
//         requested, Sam+0x678 (its warp guard), a pushed transform, a sequence binding): no placement of ours;
//   ours  nothing of the game's places him (a health drop: a MULE spear, a BT, a fall; asleep for a minute; the world
//         waiting for a scene, load mask 0x100): he is put beside the vehicle (PlaceBeside waits while he sleeps).
// A save is not "taken": the save guard's own exit (End::saving, CarryTick) comes before the save captures him.
// Every tick (the sequence answer every 250 ms; the health every 100 ms). A BT / tar grab has no trigger of its own (the
// game's "Hugged" state is not researched): it ends the ride by its health drop or its game state.
constexpr std::uint64_t kTakenBits = (1ull << 3) | (1ull << 6) | (1ull << 8) | (1ull << 9) | (1ull << 10) | (1ull << 11) | (1ull << 12) | (1ull << 13) |
                                     (1ull << 14) | (1ull << 15) | (1ull << 17) | (1ull << 18) | (1ull << 19) | (1ull << 20) | (1ull << 22) | (1ull << 23) |
                                     (1ull << 24) | (1ull << 25) | (1ull << 29) | (1ull << 30);
enum class TakenBy : std::uint8_t { none, game, ours };
TakenBy Taken(const char** why) {
    const std::uintptr_t sam = g_p.sam;
    if (mem::Q(sam + kEParent) != 0) return *why = "Sam has a parent entity", TakenBy::game;
    const bool seq = g_now >= g_p.next_seq_ms;
    if (seq) g_p.next_seq_ms = g_now + 250;
    bool ours = false;
    if (const char* t = game::CarriedSamTaken(kTakenBits, seq, &ours)) return *why = t, ours ? TakenBy::ours : TakenBy::game;
    if (g_p.asleep_since > 0 && g_now - g_p.asleep_since > 60000) return *why = "asleep for a minute", TakenBy::ours;
    if (g_now >= g_p.next_health_ms) {
        g_p.next_health_ms = g_now + 100;
        game::Diagnostics d;
        game::ReadDiagnostics(&d);
        if (d.health_valid) {
            if (g_p.health0 < 0 || d.health > g_p.health0) g_p.health0 = d.health;
            else if (d.health < g_p.health0 - 1.0f) return *why = "a health drop", TakenBy::ours;
        }
    }
    if (g_p.test_taken) return *why = "test", TakenBy::game;
    return TakenBy::none;
}

bool ExitButtonDown() {
    const std::uint16_t b = overlay::input::RidePadButtons(500);
    return (b & (XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_B)) != 0 || overlay::input::RideKeyDown();
}
bool BoardButtonDown() {
    const std::uint16_t b = overlay::input::RidePadButtons(500);
    return (b & XINPUT_GAMEPAD_X) != 0 || overlay::input::RideKeyDown();
}

void CarryTick() {
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(g_in.local);
    if (!sam || sam != g_p.sam) {
        g_p.cap_off = false;  // that Sam is gone: his capsule with him
        EndCarry(rwire::End::world, false, true);
        return;
    }
    // the vehicle
    const bool live = VehicleLive(g_p.entity, g_p.unit, g_p.kind);
    WorldTransform xf{};
    const bool have_xf = live && ReadXf(g_p.entity, xf);
    if (have_xf) g_p.gone_since = 0;
    else if (g_p.gone_since == 0) g_p.gone_since = g_now;
    // its velocity: measured by the drive module (the partner's follower), else from our own reads
    vdrive::DrivenView view;
    const bool driven = !g_p.solo && vdrive::PartnerDriving(&view) && view.key.id == g_p.key.id;
    float vel[3] = {};
    if (driven && view.meas) std::memcpy(vel, view.vel, sizeof(vel));
    else if (have_xf && g_p.have_last && g_dt_ms > 1) {
        vel[0] = static_cast<float>((xf.pos.x - g_p.last_xf.pos.x) * 1000.0 / g_dt_ms);
        vel[1] = static_cast<float>((xf.pos.y - g_p.last_xf.pos.y) * 1000.0 / g_dt_ms);
        vel[2] = static_cast<float>((xf.pos.z - g_p.last_xf.pos.z) * 1000.0 / g_dt_ms);
        for (float& c : vel)
            if (!std::isfinite(c) || std::fabs(c) > 100.0f) c = 0;
    }
    const double speed = std::sqrt(static_cast<double>(vel[0]) * vel[0] + static_cast<double>(vel[1]) * vel[1] + static_cast<double>(vel[2]) * vel[2]);
    if (!g_p.solo) {
        if (driven) g_p.not_driven_since = 0;
        else if (g_p.not_driven_since == 0) g_p.not_driven_since = g_now;
    }
    // asleep (a menu): held, not teleported; the input block lifted meanwhile (the game's menu or notice needs the pad,
    // the mouse and the keys), back once he is awake
    if (!g_in.local_active) {
        if (g_p.asleep_since == 0) g_p.asleep_since = g_now;
    } else {
        g_p.asleep_since = 0;
    }
    overlay::input::SetRideCarry(g_in.local_active);
    // the forced exits
    rwire::ForcedIn f;
    f.stopped = g_stop.load();
    std::uint32_t counter = 0;
    const bool ready = vshare::WorldReadyNow(&counter);
    f.world_changed = !g_p.solo && (!ready || counter != g_p.world);
    const char* taken_why = nullptr;
    const TakenBy taken = Taken(&taken_why);
    f.taken = taken != TakenBy::none;
    f.link_silent_ms = g_p.solo ? 0 : g_now - g_in.partner_rx_ms;
    f.not_driven_ms = g_p.not_driven_since > 0 ? g_now - g_p.not_driven_since : 0;
    f.vehicle_live = g_p.gone_since == 0 || g_now - g_p.gone_since < 300;
    std::int32_t rs = 0;
    std::uint32_t f560 = 0;
    f.broken = (live && mem::Rd(g_p.entity + kVRideState, rs) && (rs == 5 || rs == 6)) || (live && mem::Rd(g_p.entity + kVFlags560, f560) && (f560 & 0x2000)) ||
               (driven && view.have_frame && view.frame_broken);
    f.flipped = have_xf && g_p.flip.Update(xf.rot.m[2][2], g_now);
    if (driven && g_in.have_play) {
        float fv[3];
        double key = 0;
        if (vdrive::FrameAtPlay(g_p.key.id, g_in.play_ms, fv, &key)) {
            const double dv = g_p.crash.Update(fv, key);
            if (dv > 0) g_p.crash_dv = dv;
        }
    }
    if (g_p.test_crash_dv > 0) g_p.crash_dv = g_p.test_crash_dv, g_p.test_crash_dv = 0;
    f.crash_dv = g_p.crash_dv;
    f.speed = speed;
    f.driver_ended = g_p.driver_ended;
    // a save: the save guard holds a pending one until our Sam is off (WantsOff); a save pending or written without the
    // hold (the guard off, its cap, the post-load save) ends the ride too: the last resort, the placement waits for the write
    f.saving = saveguard::WantsOff() || (saveguard::Busy() && !saveguard::TestAllowsCarry());
    const rwire::End forced = rwire::ForcedExit(f);
    if (forced != rwire::End::count) {
        if (forced == rwire::End::taken) Log("[ride] the game took our Sam: %s", taken_why ? taken_why : "?");
        if (forced == rwire::End::saving)
            Log("[ride] the game %s: our Sam gets off first (%s)", saveguard::InProgress() ? "is saving" : "is about to save",
                saveguard::WantsOff() ? "the save guard holds the save until he is placed" : "the save was not held: the last-resort exit");
        if (forced == rwire::End::driver_ended) Log("[ride] the driver's game ended the ride (%s)", rwire::EndName(g_p.driver_reason));
        if (forced == rwire::End::crash) g_p.last_speed = (std::max)(g_p.last_speed, speed);
        // no placement when the game has Sam (Taken's "game" kind) or the world changed; the driver's END needs no answer
        const bool place = forced == rwire::End::taken ? taken == TakenBy::ours : forced != rwire::End::world && forced != rwire::End::stopped;
        const bool tell = forced != rwire::End::driver_ended;
        EndCarry(forced == rwire::End::driver_ended && g_p.driver_reason == static_cast<std::uint8_t>(rwire::End::driver_off) ? rwire::End::driver_off : forced, place, tell);
        return;
    }
    // getting off by choice: a hold while it is too fast waits up to 3 s for it to slow down; a second hold within that
    // wait gets off at any speed (jumped: put off away from the vehicle's path, above 8 m/s the D3-3 hit), so a driver who
    // keeps going never holds the passenger (only while awake: a menu's buttons are the menu's)
    if (g_hold.Update(g_in.local_active && ExitButtonDown(), g_now)) {
        if (g_p.exit_wait_until > g_now && speed >= rwire::kExitMaxSpeed) {
            Log("[ride] get off: a second hold while the %s moves at %.1f m/s: jumping off", vwire::KindName(g_p.kind), speed);
            g_p.last_speed = (std::max)(g_p.last_speed, speed);
            EndCarry(rwire::End::jumped, true, true);
            return;
        }
        g_p.exit_wait_until = g_now + 3000;
        if (speed >= rwire::kExitMaxSpeed) {
            Log("[ride] get off: the %s moves at %.1f m/s; off as soon as it is under %.0f m/s (3 s), or at a second hold", vwire::KindName(g_p.kind), speed,
                rwire::kExitMaxSpeed);
            Notice("Too fast to get off: slow down, or hold again to jump off.", true, 3000);
        }
    }
    if (g_p.exit_wait_until > g_now && speed < rwire::kExitMaxSpeed) {
        EndCarry(rwire::End::choice, true, true);
        return;
    }
    if (!have_xf) return;
    // the meters: where Sam is now against where the seat is now (the vehicle moved by its physics since we placed him)
    float seat[3];
    SeatOffset(g_p.kind, seat);
    const float zero[3] = {};
    const WorldTransform seat_now = SeatXf(xf, zero, 0, seat);
    WorldTransform sam_xf{};
    if (g_p.have_last && g_in.local_active && ReadXf(sam, sam_xf)) {
        const double e = Dist3(sam_xf, seat_now), ev = Dist3(sam_xf, g_p.last_target);
        g_p.err_sum += e, g_p.err_n++;
        if (e > g_p.err_max) g_p.err_max = e;
        g_p.ride_err_sum += e, g_p.ride_err_n++;
        if (e > g_p.ride_err_max) g_p.ride_err_max = e;
        if (e > 0.25) ++g_p.ride_err_over;
        g_p.err_veh_sum += ev;
        if (ev > g_p.err_veh_max) g_p.err_veh_max = ev;
    }
    std::memcpy(g_p.last_vel, vel, sizeof(vel));
    g_p.last_speed = speed;
    g_p.last_xf = xf;
    // the carry: the seat one frame ahead along the vehicle's velocity (it moves by its own physics later in this frame)
    if (g_in.local_active) {
        const double lead = (std::min)(0.05, g_dt_ms / 1000.0);
        const WorldTransform target = SeatXf(xf, vel, speed > 0.3 ? lead : 0.0, seat);
        if (CallTeleport(sam, &target)) {
            ++g_p.teleports;
            g_p.last_target = target;
            g_p.have_last = true;
            g_p.tp_xf = xf;
            g_p.have_tp_xf = true;
        } else if (++g_p.tp_faults >= 1) {
            // put beside the vehicle with the game's PlaceOn only (the faulting call never again)
            Log("[ride] FAULT in the entity teleport: the ride ends");
            g_p.tp_fault = true;
            EndCarry(rwire::End::stopped, true, true);
            return;
        }
    }
    if (g_now - g_p.log_ms >= 1000) {
        Log("[ride] carried on the %s 0x%06X: %.1f m/s; Sam vs the seat now mean %.3f m max %.3f m (vs our placement mean %.3f max %.3f) over %u ticks; "
            "capsule counter %d on %d; pose %d",
            vwire::KindName(g_p.kind), g_p.key.id, speed, g_p.err_n ? g_p.err_sum / g_p.err_n : 0.0, g_p.err_max, g_p.err_n ? g_p.err_veh_sum / g_p.err_n : 0.0,
            g_p.err_veh_max, g_p.err_n, CapCounter(g_p.cap), CapOnByte(g_p.cap), realbody::RidePose());
        g_p.log_ms = g_now;
        g_p.err_sum = g_p.err_max = g_p.err_veh_sum = g_p.err_veh_max = 0;
        g_p.err_n = 0;
    }
}

// ---------------------------------------------------------------- the passenger: boarding
bool OnFoot() {
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(g_in.local);
    return sam && g_in.local_ok && g_in.local_active && mem::Q(sam + kEParent) == 0;
}

// Our Sam's health dropped within kHurtBoardBlockMs (read every 100 ms while boarding is looked at).
bool RecentlyHurt() {
    if (g_now - g_board_health_ms >= 100) {
        g_board_health_ms = g_now;
        game::Diagnostics d;
        game::ReadDiagnostics(&d);
        if (d.health_valid && d.health > 0) {  // 0 while a world loads (run 20261119-171723: "1000 -> 0" at the load)
            if (g_board_health >= 0 && d.health < g_board_health - 1.0f) {
                if (g_now >= g_hurt_until) Log("[ride] our Sam was hurt (health %.1f -> %.1f): no boarding for %.0f s", g_board_health, d.health, kHurtBoardBlockMs / 1000);
                g_hurt_until = g_now + kHurtBoardBlockMs;
            }
            g_board_health = d.health;
        } else {
            g_board_health = -1;
        }
    }
    return g_now < g_hurt_until;
}

// What boarding the partner-driven vehicle looks like now.
struct BoardView {
    rwire::Block block = rwire::Block::no_vehicle;
    vdrive::DrivenView v;
    double dist = 1e9;
};
BoardView ViewBoard() {
    BoardView b;
    rwire::BoardIn in;
    in.feature_on = g_on.load();
    in.link_up = g_now - g_in.partner_rx_ms < rwire::kLinkSilentMs && vshare::PeerInOurWorld();
    in.partner_drives = vdrive::PartnerDriving(&b.v);
    in.kind = b.v.key.kind;
    in.local_on_foot = OnFoot() && !RecentlyHurt();
    in.busy = g_p.st != PState::idle || g_now < g_cooldown_until;
    in.saving = !saveguard::ReadsOk() || (saveguard::Busy() && !saveguard::TestAllowsCarry());
    WorldTransform xf{}, sam{};
    float door[3];
    if (in.partner_drives && ReadXf(b.v.entity, xf) && g_in.local_ok && ReadXf(reinterpret_cast<std::uintptr_t>(g_in.local), sam) &&
        rwire::DoorPoint(b.v.key.kind, door)) {
        double p[3], w[3], s[3];
        PosOf(xf, p);
        rwire::ToWorld(p, Rows(xf), door, w);
        PosOf(sam, s);
        b.dist = Dist2D(w, s);
    }
    in.door_dist_m = b.dist;
    in.vehicle_speed = b.v.meas ? b.v.speed_mps : 0;
    b.block = rwire::BoardCheck(in);
    return b;
}

void Request(const BoardView& b, std::uint8_t reason) {
    g_p = Pass{};
    g_p.st = PState::requesting;
    g_p.ride_id = (static_cast<std::uint32_t>(GetTickCount64()) * 2654435761u) | 1u;
    g_p.key = b.v.key;
    g_p.kind = b.v.key.kind;
    g_p.entity = b.v.entity;
    g_p.unit = b.v.unit;
    g_p.req_ms = g_now;
    ++g_requests;
    Log("[ride] %s: asking to ride the %s 0x%06X (%s, %.2f m from its door, ride #%08X)", g_role == 1 ? "host" : "guest", vwire::KindName(g_p.kind), g_p.key.id,
        reason == rwire::kReqTest ? "test" : "held X", b.dist, g_p.ride_id);
    Send(rwire::MsgType::request, g_p.ride_id, g_p.key.id, g_p.kind, reason);
    Notice("Asking to ride...", false, 3000);
}

void PassengerTick() {
    if (g_p.st == PState::riding) {
        CarryTick();
        return;
    }
    if (g_p.st == PState::requesting) {
        if (g_now - g_p.req_ms > rwire::kRequestTimeoutMs) {
            Log("[ride] ride #%08X: no answer in %.0f s", g_p.ride_id, rwire::kRequestTimeoutMs / 1000);
            Notice("No answer from the driver.", true, 3000);
            g_p = Pass{};
            g_cooldown_until = g_now + 1500;
        }
        return;
    }
    if (!g_on.load()) return;
    const BoardView b = ViewBoard();
    const bool down = BoardButtonDown();
    if (b.block != rwire::Block::ok) {
        g_hold.Update(down, g_now);  // keeps track of releases
        return;
    }
    char label[96], text[200];
    PeerLabel(label, sizeof(label));
    const double held = g_hold.HeldMs(g_now);
    if (down && held > 0) std::snprintf(text, sizeof(text), "Hold [X] / [F] to ride with %s... %d%%", label, static_cast<int>((std::min)(100.0, held * 100.0 / rwire::kHoldMs)));
    else std::snprintf(text, sizeof(text), "Hold [X] / [F] to ride with %s", label);
    if (g_now - g_prompt_ms > 100) {
        Notice(text, false, 400);
        g_prompt_ms = g_now;
    }
    if (g_hold.Update(down, g_now)) Request(b, rwire::kReqHold);
}

// ---------------------------------------------------------------- the driver
void DriverEnd(rwire::End reason, bool tell) {
    if (!g_d.accepted) return;
    Log("[ride] %s: the partner's ride #%08X in our %s 0x%06X is over (%s) after %.1f s; body vs seat mean %.3f m", g_role == 1 ? "host" : "guest", g_d.ride_id,
        vwire::KindName(g_d.key.kind), g_d.key.id, rwire::EndName(static_cast<std::uint8_t>(reason)), (g_now - g_d.accept_ms) / 1000.0,
        g_d.err_n ? g_d.err_sum / g_d.err_n : 0.0);
    if (tell) Send(rwire::MsgType::end, g_d.ride_id, g_d.key.id, g_d.key.kind, static_cast<std::uint8_t>(reason));
    g_d = Drv{};
}

void DriverTick() {
    if (!g_d.accepted) return;
    vdrive::DrivenView v;
    if (!vdrive::LocalDriving(&v) || v.key.id != g_d.key.id) {
        DriverEnd(rwire::End::driver_off, true);
        return;
    }
    g_d.entity = v.entity;
    if ((g_in.partner_flags & wire::kPosePassenger) && g_now - g_in.partner_rx_ms < 500) g_d.last_bit_ms = g_in.partner_rx_ms;
    if (rwire::DriverTimedOut(true, g_now - g_d.accept_ms, g_now - g_d.last_bit_ms)) {
        DriverEnd(rwire::End::timeout, true);
        return;
    }
    if (g_now - g_d.log_ms >= 1000 && g_d.err_n) {
        Log("[ride] %s: the partner rides our %s 0x%06X: body vs the seat mean %.3f m max %.3f m over %u ticks; passenger bit %.0f ms ago", g_role == 1 ? "host" : "guest",
            vwire::KindName(g_d.key.kind), g_d.key.id, g_d.err_sum / g_d.err_n, g_d.err_max, g_d.err_n, g_now - g_d.last_bit_ms);
        g_d.log_ms = g_now;
        g_d.err_sum = g_d.err_max = 0;
        g_d.err_n = 0;
    }
}

void OnRequest(const rwire::Msg& m) {
    rwire::DriverIn in;
    in.feature_on = g_on.load();
    in.same_world = m.world == OurWorld() && vshare::PeerInOurWorld();
    vdrive::DrivenView v;
    in.we_drive_it = vdrive::LocalDriving(&v) && v.key.id == m.vehicle_id && v.key.kind == m.kind;
    in.seat_taken = g_d.accepted && g_d.ride_id != m.ride_id;
    in.speed = v.meas ? v.speed_mps : 0;
    WorldTransform xf{};
    float door[3];
    if (in.we_drive_it && g_in.have_partner && ReadXf(v.entity, xf) && rwire::DoorPoint(m.kind, door)) {
        double p[3], w[3], s[3];
        PosOf(xf, p);
        rwire::ToWorld(p, Rows(xf), door, w);
        PosOf(g_in.partner, s);
        in.have_partner = true;
        in.partner_dist_m = Dist2D(w, s);
    }
    rwire::Deny why = rwire::Deny::count;
    if (g_d.accepted && g_d.ride_id == m.ride_id) {
        Send(rwire::MsgType::accept, m.ride_id, m.vehicle_id, m.kind, 0);  // a repeat: the same answer
        return;
    }
    if (!rwire::DriverAccepts(in, &why)) {
        ++g_denies;
        Log("[ride] %s: the partner asks to ride our %s 0x%06X (ride #%08X): DENIED, %s (speed %.1f m/s, partner %.2f m from the door)", g_role == 1 ? "host" : "guest",
            vwire::KindName(m.kind), m.vehicle_id, m.ride_id, rwire::DenyName(static_cast<std::uint8_t>(why)), in.speed, in.partner_dist_m);
        Send(rwire::MsgType::deny, m.ride_id, m.vehicle_id, m.kind, static_cast<std::uint8_t>(why));
        return;
    }
    g_d = Drv{};
    g_d.accepted = true;
    g_d.ride_id = m.ride_id;
    g_d.key = v.key;
    g_d.entity = v.entity;
    g_d.accept_ms = g_now;
    g_d.log_ms = g_now;
    ++g_accepts;
    Log("[ride] %s: the partner asks to ride our %s 0x%06X (ride #%08X): ACCEPTED (#%u; partner %.2f m from the door, %.1f m/s)", g_role == 1 ? "host" : "guest",
        vwire::KindName(m.kind), m.vehicle_id, m.ride_id, g_accepts, in.partner_dist_m, in.speed);
    Send(rwire::MsgType::accept, m.ride_id, m.vehicle_id, m.kind, 0);
    char label[96], text[200];
    PeerLabel(label, sizeof(label));
    std::snprintf(text, sizeof(text), "%s rides with you.", label);
    Notice(text, false, 4000);
}

void OnMsg(std::uint8_t /*type*/, const std::uint8_t* msg, std::size_t size, std::uint32_t /*generation*/, double now) {
    g_now = now;
    rwire::Msg m;
    if (!rwire::DecodeMsg(msg, size, m) || m.from == (g_role == 1 ? 0 : 1)) {
        if (++g_bad_rx <= 10) Log("[ride] dropped a ride message (%zu bytes): not valid or not from the partner", size);
        return;
    }
    // riding together stopped here (a fault, the test's emergency stop): nothing starts any more; an ACCEPT (an answer to a
    // request sent before the stop) is ended at once, a REQUEST denied
    if (g_stop.load() || !g_on.load()) {
        if (++g_bad_rx <= 10)
            Log("[ride] %s for ride #%08X after riding together stopped: %s", rwire::MsgTypeName(static_cast<std::uint8_t>(m.type)), m.ride_id,
                m.type == rwire::MsgType::accept ? "ended at once" : m.type == rwire::MsgType::request ? "denied" : "dropped");
        if (m.type == rwire::MsgType::accept) Send(rwire::MsgType::end, m.ride_id, m.vehicle_id, m.kind, static_cast<std::uint8_t>(rwire::End::stopped));
        else if (m.type == rwire::MsgType::request) Send(rwire::MsgType::deny, m.ride_id, m.vehicle_id, m.kind, static_cast<std::uint8_t>(rwire::Deny::off));
        return;
    }
    // another world's REQUEST / ACCEPT never starts a ride (a REQUEST is denied, an ACCEPT ended); a DENY or END is the
    // answer to a ride of ours and carries the sender's world, which differs exactly then: it passes (it acts only on its
    // own ride id)
    if (m.world != OurWorld() && (m.type == rwire::MsgType::request || m.type == rwire::MsgType::accept)) {
        if (++g_bad_rx <= 10) Log("[ride] dropped a %s for ride #%08X: another world (%u, ours %u)", rwire::MsgTypeName(static_cast<std::uint8_t>(m.type)), m.ride_id,
                                  m.world, OurWorld());
        if (m.type == rwire::MsgType::request) {
            Send(rwire::MsgType::deny, m.ride_id, m.vehicle_id, m.kind, static_cast<std::uint8_t>(rwire::Deny::world));
        } else {
            if (g_p.st == PState::requesting && g_p.ride_id == m.ride_id) {
                g_p = Pass{};
                g_cooldown_until = g_now + 1500;
            }
            Send(rwire::MsgType::end, m.ride_id, m.vehicle_id, m.kind, static_cast<std::uint8_t>(rwire::End::world));
        }
        return;
    }
    switch (m.type) {
        case rwire::MsgType::request: OnRequest(m); break;
        case rwire::MsgType::accept: {
            if (g_p.st != PState::requesting || g_p.ride_id != m.ride_id || g_p.key.id != m.vehicle_id) {
                Log("[ride] an ACCEPT for ride #%08X we are not asking for: ended at once", m.ride_id);
                Send(rwire::MsgType::end, m.ride_id, m.vehicle_id, m.kind, static_cast<std::uint8_t>(rwire::End::timeout));
                break;
            }
            const BoardView b = ViewBoard();
            const bool still = b.v.key.id == g_p.key.id && b.dist <= rwire::kBoardRadiusM + 1.5 && OnFoot();
            Log("[ride] the driver accepted ride #%08X after %.0f ms (%.2f m from the door)", m.ride_id, g_now - g_p.req_ms, b.dist);
            const vwire::VKey key = g_p.key;
            const std::uint32_t id = g_p.ride_id;
            g_p = Pass{};
            if (!still || !StartCarry(key, key.kind, b.v.entity, b.v.unit, id, false)) {
                Log("[ride] ride #%08X not started: %s", id, still ? "the carry did not start" : "we are no longer at the vehicle");
                Send(rwire::MsgType::end, id, key.id, key.kind, static_cast<std::uint8_t>(rwire::End::timeout));
                g_cooldown_until = g_now + 1500;
            }
            break;
        }
        case rwire::MsgType::deny: {
            if (g_p.st != PState::requesting || g_p.ride_id != m.ride_id) break;
            ++g_denied;
            Log("[ride] the driver denied ride #%08X: %s", m.ride_id, rwire::DenyName(m.reason));
            char text[160];
            std::snprintf(text, sizeof(text), "Cannot ride: %s.", rwire::DenyName(m.reason));
            Notice(text, true, 3000);
            g_p = Pass{};
            g_cooldown_until = g_now + 1500;
            break;
        }
        case rwire::MsgType::end: {
            ++g_ends_rx;
            if (g_d.accepted && g_d.ride_id == m.ride_id) {
                Log("[ride] the partner ended ride #%08X: %s", m.ride_id, rwire::EndName(m.reason));
                DriverEnd(static_cast<rwire::End>(m.reason), false);
            }
            if (g_p.st == PState::riding && g_p.ride_id == m.ride_id) {
                g_p.driver_ended = true;
                g_p.driver_reason = m.reason;
            }
            if (g_p.st == PState::requesting && g_p.ride_id == m.ride_id) g_p = Pass{};
            break;
        }
    }
}

void RollTick() {
    if (!g_roll_left) return;
    vdrive::DrivenView d;
    WorldTransform xf{};
    if (!vdrive::LocalDriving(&d) || !ReadXf(d.entity, xf)) {
        g_roll_left = 0;
        Log("[ride] test: roll stopped (our Sam no longer drives)");
        return;
    }
    const double a = 4.0 * 3.141592653589793 / 180.0, c = std::cos(a), sn = std::sin(a);
    // the pivot 1 m above the origin along up; rotate right and up about forward
    double piv[3];
    for (int i = 0; i < 3; ++i) piv[i] = (i == 0 ? xf.pos.x : i == 1 ? xf.pos.y : xf.pos.z) + xf.rot.m[2][i];
    WorldTransform t = xf;
    for (int i = 0; i < 3; ++i) {
        t.rot.m[0][i] = static_cast<float>(c * xf.rot.m[0][i] + sn * xf.rot.m[2][i]);
        t.rot.m[2][i] = static_cast<float>(-sn * xf.rot.m[0][i] + c * xf.rot.m[2][i]);
    }
    t.pos.x = piv[0] - t.rot.m[2][0], t.pos.y = piv[1] - t.rot.m[2][1], t.pos.z = piv[2] - t.rot.m[2][2];
    vshare::RequestTeleport(d.entity, t);
    if (--g_roll_left == 0) Log("[ride] test: roll done (up z %.2f)", t.rot.m[2][2]);
}

void TickBody(const TickIn& in, double now) {
    g_in = in;
    if (g_last_tick > 0 && now > g_last_tick) g_dt_ms = g_dt_ms * 0.9 + (std::min)(100.0, now - g_last_tick) * 0.1;
    g_last_tick = now;
    g_now = now;
    PendingPlaceTick();
    const std::uint32_t gen = link::Generation();
    if (gen != g_gen) {
        if (g_gen != 0 && (g_p.st == PState::riding || g_d.accepted)) Log("[ride] a new link session: the ride ends");
        g_gen = gen;
        if (!g_p.solo) EndCarry(rwire::End::link, true, false);
        if (g_p.st == PState::requesting) g_p = Pass{};
        DriverEnd(rwire::End::driver_ended, false);
    }
    PassengerTick();
    DriverTick();
    RollTick();
}
bool TickGuarded(const TickIn& in, double now) noexcept {
    __try {
        TickBody(in, now);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RestoreAfterStop() {
    if (g_p.st == PState::requesting) {
        // a request in flight: asked no more (a late ACCEPT is ended by OnMsg)
        Log("[ride] the mod stopped while asking to ride (#%08X): the request is withdrawn", g_p.ride_id);
        Send(rwire::MsgType::end, g_p.ride_id, g_p.key.id, g_p.kind, static_cast<std::uint8_t>(rwire::End::stopped));
        g_p = Pass{};
        return;
    }
    if (g_p.st != PState::riding) return;
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(g_in.local);
    if (sam && sam == g_p.sam) PlaceBeside(true, !g_p.tp_fault);
    CapsuleBack("the mod stopped");
    Log("[ride] the mod stopped while our Sam was carried: input and pose back, capsule %s", sam == g_p.sam ? "back on, put beside the vehicle" : "with the old Sam");
    g_p = Pass{};
    PublishSaveState();
}
bool RestoreGuarded() noexcept {
    __try {
        RestoreAfterStop();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

// ---------------------------------------------------------------- public
void Install(const Config& config) noexcept {
    g_cfg_on = config.ride_together;
    unsigned bad = 0;
    for (const Site& s : kSites) {
        if (!mem::CodeMatches(A(s.va), s.sig)) {
            ++bad;
            Log("[ride] byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
            if (!g_off[0] || std::strcmp(g_off, "not installed") == 0) std::snprintf(g_off, sizeof(g_off), "this game build differs (%s)", s.name);
        }
    }
    const bool vt_ok = mem::InImage(A(kCapsuleVt)) && mem::InText(mem::Q(A(kCapsuleVt)));
    if (!vt_ok) {
        ++bad;
        Log("[ride] byte check FAILED: the capsule vtable 0x%llX", static_cast<unsigned long long>(kCapsuleVt));
        std::snprintf(g_off, sizeof(g_off), "this game build differs (the capsule vtable)");
    }
    g_checks_ok = bad == 0;
    const unsigned n = static_cast<unsigned>(sizeof(kSites) / sizeof(kSites[0])) + 1;
    if (!g_checks_ok) {
        Log("[ride] riding together OFF: %s (%u of %u checks failed)", g_off, bad, n);
        return;
    }
    if (!g_cfg_on) {
        std::snprintf(g_off, sizeof(g_off), "session.ini ride_together=0");
        Log("[ride] riding together OFF: %s (%u byte checks ok)", g_off, n);
        return;
    }
    if (!vshare::Active() || !vdrive::Active()) {
        std::snprintf(g_off, sizeof(g_off), "%s", !vshare::Active() ? "the real shared vehicles are off" : "remote driving is off");
        Log("[ride] riding together OFF: %s (%u byte checks ok; the solo seat test works)", g_off, n);
        return;
    }
    g_role = vshare::Role();
    if (!vshare::RegisterHandler(rwire::kSvmType, &OnMsg)) {
        std::snprintf(g_off, sizeof(g_off), "channel 3 type %u is taken", static_cast<unsigned>(rwire::kSvmType));
        Log("[ride] riding together OFF: %s", g_off);
        return;
    }
    g_off[0] = 0;
    g_on.store(true);
    Log("[ride] riding together ON (%s): %u byte checks; hold X (pad) / F (keyboard) 0.6 s beside the partner's vehicle; seats roadster / truck / "
        "trike pillion as in ride_wire.cpp",
        g_role == 1 ? "host" : "guest", n);
}

bool Active() noexcept { return g_on.load(); }
const char* OffReason() noexcept { return g_on.load() ? "" : g_off; }

// Logs every time our Sam sits in a vehicle (any kind; hidden tests wait on it, with the shared vehicles off too).
void SeatWatch(const TickIn& in) noexcept {
    static std::uintptr_t last = 0;
    static unsigned n = 0;
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(in.local);
    const std::uintptr_t p = sam && in.local_ok ? mem::Q(sam + kEParent) : 0;
    const std::uintptr_t v = p && KindOfEntity(p) ? p : 0;
    if (v == last) return;
    last = v;
    if (v) Log("[ride] our Sam is seated in the %s (#%u)", vwire::KindName(static_cast<std::uint8_t>(KindOfEntity(v))), ++n);
    else Log("[ride] our Sam is out of the vehicle");
}

static void TickInner(const TickIn& in, double now) noexcept;
void Tick(const TickIn& in, double now) noexcept {
    TickInner(in, now);
    PublishSaveState();
}
static void TickInner(const TickIn& in, double now) noexcept {
    if (!g_checks_ok) return;
    SeatWatch(in);
    if (g_stop.load()) {
        g_in = in;
        g_now = now;
        AfterStop();
        // a placement that waits (Sam asleep, a save being written) or retries PlaceOn goes on; nothing else
        if (g_place.on && !PendingPlaceGuarded()) Log("[ride] FAULT in the exit's placement after the stop: dropped");
        return;
    }
    if (!g_on.load() && g_p.st != PState::riding && !g_d.accepted) {
        g_in = in;
        g_now = now;
        if (!g_notice_done && g_cfg_on && g_off[0] && coop::Active() && vdrive::Active()) {
            g_notice_done = true;
            char text[220];
            std::snprintf(text, sizeof(text), "Riding together off: %s.", g_off);
            coop::SetVehicleNotice(text, true, now + 60000);
        }
        return;
    }
    if (!TickGuarded(in, now)) {
        Log("[ride] FAULT in the riding tick: stopped (input and pose back, capsule back on)");
        EmergencyStop();
        AfterStop();
    }
}

void NewSession() noexcept {
    // the tick sees the generation change and ends the ride there (game thread)
}

void EmergencyStop() noexcept {
    if (!g_stop.load()) std::snprintf(g_off, sizeof(g_off), "stopped");
    g_on.store(false);
    g_stop.store(true);
    overlay::input::SetRideCarry(false);
    realbody::SetRidePose(0);
    vdrive::SetCarriedUnit(0);
}

void AfterStop() noexcept {
    static std::atomic<bool> done{false};
    if (!g_stop.load() || done.exchange(true)) return;
    if (!RestoreGuarded()) Log("[ride] FAULT while restoring after the stop (the capsule may stay off until a load)");
    g_d = Drv{};
}

std::uint32_t PoseFlags() noexcept {
    return g_p.st == PState::riding && !g_p.solo ? (wire::kPosePassenger | wire::kPoseInVehicle) : 0u;
}
bool Carried() noexcept { return g_p.st == PState::riding; }

bool PassengerTarget(game::WorldTransform* target) noexcept {
    if (!target || !g_d.accepted || g_stop.load()) return false;
    if (!rwire::DriverPlacesBody(true, g_now - g_d.accept_ms, g_now - g_d.last_bit_ms)) return false;
    vdrive::DrivenView v;
    if (!vdrive::LocalDriving(&v) || v.key.id != g_d.key.id) return false;
    WorldTransform xf{};
    float seat[3];
    if (!ReadXf(v.entity, xf) || !SeatOffset(v.key.kind, seat)) return false;
    const double lead = (std::min)(0.05, g_dt_ms / 1000.0);
    const float zero[3] = {};
    // the meter: where the real body is now (placed last tick, the vehicle moved by its physics since) against the seat now
    WorldTransform body{};
    const std::uintptr_t be = reinterpret_cast<std::uintptr_t>(realbody::LiveEntity());
    if (be && g_d.placed_logged && ReadXf(be, body)) {
        const double e = Dist3(SeatXf(xf, zero, 0, seat), body);
        if (e < 5.0) {
            g_d.err_sum += e, g_d.err_n++;
            if (e > g_d.err_max) g_d.err_max = e;
        }
    }
    *target = SeatXf(xf, v.meas ? v.vel : zero, v.meas && v.speed_mps > 0.3 ? lead : 0.0, seat);
    if (!g_d.placed_logged) {
        g_d.placed_logged = true;
        Log("[ride] the partner body sits in our %s 0x%06X at the passenger seat (%.2f, %.2f, %.2f)", vwire::KindName(v.key.kind), v.key.id, seat[0], seat[1], seat[2]);
    }
    return true;
}

std::uintptr_t RideVehicle() noexcept {
    if (g_d.accepted) return g_d.entity;
    if (g_p.st == PState::riding) return g_p.entity;
    return 0;
}

void SetCrashHitHandler(CrashHitFn fn) noexcept { g_crash_hit = fn; }

// ---------------------------------------------------------------- debug actions
bool StateCommand(char* msg, std::size_t cap) noexcept {
    std::uint32_t w = 0, ns = 0, rs = 0, fl = 0;
    realbody::RidePoseCounters(&w, &ns, &rs, &fl);
    unsigned long pads = 0, keys = 0, raw = 0;
    overlay::input::RideCounters(&pads, &keys, &raw);
    float s1[3] = {}, s2[3] = {}, s3[3] = {};
    SeatOffset(1, s1), SeatOffset(2, s2), SeatOffset(3, s3);
    char full[1200];
    std::snprintf(full, sizeof(full),
                  "%s%s; passenger %s%s %s 0x%06X ride #%08X; driver %s ride #%08X %s 0x%06X; boards %u exits %u requests %u denied %u; accepts %u denies %u "
                  "ends rx %u tx %u bad %u; pose writes %u nosrc %u restores %u; input pads %lu keys %lu raw %lu; capsule counter %d on %d; seats "
                  "roadster (%.2f, %.2f, %.2f) truck (%.2f, %.2f, %.2f) trike (%.2f, %.2f, %.2f)",
                  g_on.load() ? "ON" : "OFF: ", g_on.load() ? "" : g_off, g_p.st == PState::riding ? "riding" : g_p.st == PState::requesting ? "requesting" : "idle",
                  g_p.solo ? " (solo test)" : "", vwire::KindName(g_p.kind), g_p.key.id, g_p.ride_id, g_d.accepted ? "carrying" : "idle", g_d.ride_id,
                  vwire::KindName(g_d.key.kind), g_d.key.id, g_boards, g_exits, g_requests, g_denied, g_accepts, g_denies, g_ends_rx, g_ends_tx, g_bad_rx, w, ns, rs,
                  pads, keys, raw, CapCounter(SamCapsule(reinterpret_cast<std::uintptr_t>(g_in.local))),
                  CapOnByte(SamCapsule(reinterpret_cast<std::uintptr_t>(g_in.local))), s1[0], s1[1], s1[2], s2[0], s2[1], s2[2], s3[0], s3[1], s3[2]);
    Log("[ride] state: %s", full);
    std::snprintf(msg, cap, "%s", full);
    return true;
}

bool BoardCommand(char* msg, std::size_t cap) noexcept {
    if (!g_on.load()) return std::snprintf(msg, cap, "riding together is off: %s", g_off), false;
    const BoardView b = ViewBoard();
    if (b.block != rwire::Block::ok) return std::snprintf(msg, cap, "cannot board: %s (door %.2f m)", rwire::BlockName(b.block), b.dist), false;
    Request(b, rwire::kReqTest);
    return std::snprintf(msg, cap, "asked to ride the %s 0x%06X (ride #%08X)", vwire::KindName(g_p.kind), g_p.key.id, g_p.ride_id), true;
}

bool ExitCommand(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (g_p.st != PState::riding) return std::snprintf(msg, cap, "not riding"), false;
    if (g_p.last_speed >= rwire::kExitMaxSpeed && v[0] == 0) return std::snprintf(msg, cap, "too fast (%.1f m/s)", g_p.last_speed), false;
    EndCarry(v[0] != 0 ? rwire::End::test : rwire::End::choice, true, true);
    return std::snprintf(msg, cap, "off"), true;
}

bool SeatMeasureCommand(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    (void)v;
    game::Entity* local = nullptr;
    if (!game::TryGetLocalPlayer(&local) || !local) return std::snprintf(msg, cap, "no local Sam"), false;
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(local);
    std::uintptr_t e = mem::Q(sam + kEParent);
    const bool seated = e && KindOfEntity(e) != 0;
    if (!seated) {
        e = g_meas_e;
        if (!e || KindOfEntity(e) != g_meas_kind || mem::Q(e + kVUnit) != g_meas_unit)
            return std::snprintf(msg, cap, "our Sam drives nothing and no vehicle was measured before"), false;
    }
    const auto kind = static_cast<std::uint8_t>(KindOfEntity(e));
    WorldTransform vx{}, sx{};
    if (!ReadXf(e, vx) || !ReadXf(sam, sx)) return std::snprintf(msg, cap, "transforms unreadable"), false;
    double vp[3], sp[3], root[3];
    PosOf(vx, vp), PosOf(sx, sp);
    rwire::ToLocal(vp, Rows(vx), sp, root);
    // Sam's facing in the vehicle's frame
    const double fx = sx.rot.m[1][0] * vx.rot.m[0][0] + sx.rot.m[1][1] * vx.rot.m[0][1] + sx.rot.m[1][2] * vx.rot.m[0][2];
    const double fy = sx.rot.m[1][0] * vx.rot.m[1][0] + sx.rot.m[1][1] * vx.rot.m[1][1] + sx.rot.m[1][2] * vx.rot.m[1][2];
    const double yaw = std::atan2(fx, fy) * 57.29577951308232;
    double pel[3] = {}, pel_l[3] = {};
    const bool have_pel = realbody::JointWorld(local, 1, pel);
    if (have_pel) rwire::ToLocal(vp, Rows(vx), pel, pel_l);
    // the vehicle's joint 1 (the helpers' parent) and the game's seat helpers through it (static data of the vehicles'
    // SkeletonHelpers: the position of each helper in joint 1's frame)
    float m[4][4] = {};
    unsigned joints = 0;
    char jname[96] = {};
    const bool have_j = realbody::ReadJointModelAny(reinterpret_cast<game::Entity*>(e), 1, &joints, m, jname, sizeof(jname));
    struct H {
        const char* name;
        float p[3];
    };
    H hs[2] = {};
    unsigned nh = 0;
    if (kind == 1) hs[0] = {"HLP_Driver", {-0.3133f, -0.6599f, 0.5601f}}, hs[1] = {"HLP_Passenger", {0.3133f, -0.6599f, 0.5601f}}, nh = 2;
    else if (kind == 2) hs[0] = {"HLP_Driver", {-0.4726f, 0.7203f, 1.7929f}}, hs[1] = {"HLP_Passenger", {0.4726f, 0.7203f, 1.7929f}}, nh = 2;
    else if (kind == 3) hs[0] = {"HLP_C_Driver", {0.0f, -0.4979f, 0.3360f}}, nh = 1;
    double hl[2][3] = {};
    char hbuf[400] = {};
    std::size_t hn = 0;
    for (unsigned i = 0; i < nh && have_j; ++i) {
        for (int k = 0; k < 3; ++k) hl[i][k] = m[3][k] + hs[i].p[0] * m[0][k] + hs[i].p[1] * m[1][k] + hs[i].p[2] * m[2][k];
        hn += std::snprintf(hbuf + hn, sizeof(hbuf) - hn, "%s%s (%.3f, %.3f, %.3f)", i ? ", " : "", hs[i].name, hl[i][0], hl[i][1], hl[i][2]);
    }
    float seat[3] = {};
    SeatOffset(kind, seat);
    char derived[120] = "-";
    if (nh == 2 && have_j && seated)
        std::snprintf(derived, sizeof(derived), "(%.3f, %.3f, %.3f)", root[0] + hl[1][0] - hl[0][0], root[1] + hl[1][1] - hl[0][1], root[2] + hl[1][2] - hl[0][2]);
    g_meas_e = e, g_meas_unit = mem::Q(e + kVUnit), g_meas_kind = kind;
    const bool snap = seated && realbody::SnapshotRidePose();
    char full[1200];
    std::snprintf(full, sizeof(full),
                  "%s %s: Sam root (%.3f, %.3f, %.3f) yaw %.1f deg, pelvis %s(%.3f, %.3f, %.3f); joint 1 '%s' of %u at (%.3f, %.3f, %.3f) axes x (%.2f, %.2f, %.2f) "
                  "y (%.2f, %.2f, %.2f); helpers: %s; passenger from the helpers %s; seat in use (%.2f, %.2f, %.2f); pose kept %s",
                  vwire::KindName(kind), seated ? "seated" : "on foot (the last vehicle)", root[0], root[1], root[2], yaw, have_pel ? "" : "unread ", pel_l[0],
                  pel_l[1], pel_l[2], jname, joints, m[3][0], m[3][1], m[3][2], m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], hbuf[0] ? hbuf : "-", derived,
                  seat[0], seat[1], seat[2], snap ? "yes" : "no");
    Log("[ride] seat measure: %s", full);
    std::snprintf(msg, cap, "%s", full);
    return true;
}

bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    switch (op) {
        case 0: return StateCommand(msg, cap);
        case 1: {
            // solo seat check: our Sam carried on the measured vehicle with the kept driver pose
            if (!g_checks_ok) return std::snprintf(msg, cap, "byte checks failed: %s", g_off), false;
            if (g_p.st != PState::idle) return std::snprintf(msg, cap, "busy"), false;
            if (!g_meas_e || KindOfEntity(g_meas_e) != g_meas_kind || mem::Q(g_meas_e + kVUnit) != g_meas_unit)
                return std::snprintf(msg, cap, "measure a vehicle first (seat_measure while driving)"), false;
            if (!OnFoot()) return std::snprintf(msg, cap, "our Sam is not on foot"), false;
            vwire::VKey key;
            std::uint32_t id = 0;
            mem::Rd(g_meas_unit + 0x08, id);
            key.id = id, key.kind = g_meas_kind;
            if (!StartCarry(key, g_meas_kind, g_meas_e, g_meas_unit, 0x5E57u, true)) return std::snprintf(msg, cap, "the carry did not start"), false;
            return std::snprintf(msg, cap, "carried on the %s (solo seat test)", vwire::KindName(g_meas_kind)), true;
        }
        case 2:
            if (g_p.st != PState::riding) return std::snprintf(msg, cap, "not riding"), false;
            EndCarry(rwire::End::test, true, true);
            return std::snprintf(msg, cap, "off"), true;
        case 4:
            if (g_p.st != PState::riding) return std::snprintf(msg, cap, "not riding"), false;
            g_p.test_crash_dv = v[1] > 0 ? v[1] : 12.0;
            return std::snprintf(msg, cap, "a crash of %.1f m/s next tick", g_p.test_crash_dv), true;
        case 5:
        case 6: {
            vdrive::DrivenView d;
            if (!vdrive::LocalDriving(&d)) return std::snprintf(msg, cap, "our Sam drives nothing shared"), false;
            WorldTransform xf{};
            if (!ReadXf(d.entity, xf)) return std::snprintf(msg, cap, "unreadable"), false;
            if (op == 5) {
                if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden tests only"), false;
                g_roll_left = 45;  // 180 deg over 45 ticks (~1.5 s): no velocity jump over 8 m/s in the frames
                Log("[ride] test: rolling our %s 0x%06X over (45 x 4 deg)", vwire::KindName(d.key.kind), d.key.id);
                return std::snprintf(msg, cap, "rolling over"), true;
            }
            const bool ok = vshare::RequestTeleport(d.entity, xf);
            Log("[ride] test: stopped dead our %s 0x%06X (RequestTeleport %s, %.1f m/s)", vwire::KindName(d.key.kind), d.key.id, ok ? "requested" : "FAILED",
                d.speed_mps);
            return std::snprintf(msg, cap, "stop: %s", ok ? "requested" : "failed"), ok;
        }
        case 7:
            if (g_p.st != PState::riding) return std::snprintf(msg, cap, "not riding"), false;
            g_p.test_taken = true;
            return std::snprintf(msg, cap, "taken next tick"), true;
        case 8: {
            // hidden tests: our Sam at the passenger door point (v[1] m: 0 or 1 = the point, more = further out) of the vehicle the partner drives here, facing it
            vdrive::DrivenView d;
            if (!vdrive::PartnerDriving(&d)) return std::snprintf(msg, cap, "the partner drives nothing we follow"), false;
            WorldTransform xf{};
            float door[3];
            if (!ReadXf(d.entity, xf) || !rwire::DoorPoint(d.key.kind, door)) return std::snprintf(msg, cap, "unreadable"), false;
            door[0] += static_cast<float>((v[1] > 0 ? v[1] : 1.0) - 1.0);  // 1 = the door point itself
            double p[3], w[3];
            PosOf(xf, p);
            rwire::ToWorld(p, Rows(xf), door, w);
            const double fx = -xf.rot.m[0][0], fy = -xf.rot.m[0][1];
            char why[160] = {};
            const bool ok = game::TeleportLocalPlayerFacing(w[0], w[1], w[2] + 0.5, fx, fy, game::TeleportMode::place, why, sizeof(why));
            return std::snprintf(msg, cap, "at the %s's passenger door: %s", vwire::KindName(d.key.kind), why), ok;
        }
        case 9:
            // hidden tests: the module's own emergency stop (what a fault of the per-frame tick does): the input block and the
            // pose override go now; the next tick puts our Sam beside the vehicle and turns his capsule back on. Riding stays off.
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden tests only"), false;
            Log("[ride] test: emergency stop");
            EmergencyStop();
            return std::snprintf(msg, cap, "emergency stop: restored on the next tick"), true;
        case 10: {
            // hidden tests: ask to ride (as "Ride board") and stop the module in the same call, so the driver's ACCEPT
            // always arrives after the stop (the late ACCEPT of a fault between the request and the answer)
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden tests only"), false;
            if (!g_on.load()) return std::snprintf(msg, cap, "riding together is off: %s", g_off), false;
            const BoardView b = ViewBoard();
            if (b.block != rwire::Block::ok) return std::snprintf(msg, cap, "cannot board: %s (door %.2f m)", rwire::BlockName(b.block), b.dist), false;
            Request(b, rwire::kReqTest);
            Log("[ride] test: emergency stop right after the request");
            EmergencyStop();
            return std::snprintf(msg, cap, "asked to ride (ride #%08X), then the emergency stop", g_p.ride_id), true;
        }
        case 11: {
            // hidden tests: the boarding check now, logged (the action succeeds whatever it says: a scenario waits on the line)
            const BoardView b = ViewBoard();
            Log("[ride] test: boarding now: %s (door %.2f m)", rwire::BlockName(b.block), b.dist);
            return std::snprintf(msg, cap, "boarding now: %s (door %.2f m)", rwire::BlockName(b.block), b.dist), true;
        }
        case 31:
        case 32:
        case 33: {
            const int k = g_meas_kind ? g_meas_kind : (g_p.kind ? g_p.kind : 2);
            if (!g_seat_override[k]) {
                rwire::PassengerSeat(static_cast<std::uint8_t>(k), g_seat_ov[k]);
                g_seat_override[k] = true;
            }
            g_seat_ov[k][op - 31] = static_cast<float>(v[1]);
            Log("[ride] test: the %s's passenger seat now (%.2f, %.2f, %.2f)", vwire::KindName(static_cast<std::uint8_t>(k)), g_seat_ov[k][0], g_seat_ov[k][1],
                g_seat_ov[k][2]);
            return std::snprintf(msg, cap, "%s seat (%.2f, %.2f, %.2f)", vwire::KindName(static_cast<std::uint8_t>(k)), g_seat_ov[k][0], g_seat_ov[k][1], g_seat_ov[k][2]),
                   true;
        }
        case 30:
            for (bool& b : g_seat_override) b = false;
            return std::snprintf(msg, cap, "seat overrides off"), true;
        default: return std::snprintf(msg, cap, "unknown op %d", op), false;
    }
}

}  // namespace st::ride
