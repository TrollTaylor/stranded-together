// Riding together (design milestone M3; docs/real-partner-body.md section 24): the pure part. The ride messages ("STQ1",
// one body inside an SVM1 message of external type 48 on the link's reliable ordered channel 3, vehicle_share.hpp
// RegisterHandler / SendEvent), the seats, and the decision rules of ride_together.cpp (who may board, what the driver's
// game answers, when the passenger is put off, where he is put). No game memory, no link, no Windows: unit-tested in
// tests/ride_wire_tests.cpp.
//
// The game seats one player per vehicle (the driver). The passenger is "carried": his own Sam, capsule off, is placed each
// tick at the vehicle's transform times a seat offset (Entity teleport 0x14235A010, proven at 0.000 m), with the driver's
// seated pose copied onto him (decision D3-2). Seats are the local Sam's ROOT in the vehicle's frame (rows right, forward,
// up), measured in the game (docs section 24.2): the driver's seat as the game puts Sam there, the passenger's from the
// vehicle's own helpers (HLP_Passenger is HLP_Driver mirrored across the vehicle's centre plane on the roadster and the
// truck), and the trike's pillion (it has no passenger helper; decision D3-1) behind the driver.
#pragma once
#include <cstddef>
#include <cstdint>

namespace st::rwire {

// ---------------------------------------------------------------- the message (STQ1)
inline constexpr std::uint8_t kSvmType = 48;           // vehicle_share's external type for the ride messages
inline constexpr std::uint8_t kMagic[4] = {'S', 'T', 'Q', '1'};
inline constexpr std::size_t kBodySize = 28;
inline constexpr std::size_t kSvmHeaderSize = 8;       // "SVM1" | u8 type | u8 version 1 | u16 0 (vehicle_wire.hpp)
inline constexpr std::size_t kMessageSize = kSvmHeaderSize + kBodySize;

enum class MsgType : std::uint8_t {
    request = 1,  // passenger -> driver: may I ride this vehicle (a hold of X beside it)
    accept = 2,   // driver -> passenger: yes; the passenger's game carries its Sam from now on
    deny = 3,     // driver -> passenger: no (reason below)
    end = 4,      // either way: the ride is over (reason below); the passenger's game has put / puts its Sam off
};
const char* MsgTypeName(std::uint8_t t) noexcept;

enum class Seat : std::uint8_t { none = 0, passenger = 1, pillion = 2 };
const char* SeatName(std::uint8_t s) noexcept;

// REQUEST reasons: the ImGui fallback's hold (kReqHold), a debug action (kReqTest), the game's own "Use Vehicle" prompt
// pressed beside the partner's vehicle (kReqNative: the driver's game checks the passenger near the vehicle, not the door)
// kReqResume (v0.21): the passenger's game asks for the seat back after a save put him off (End::saving): the same ride id,
// accepted by the driver's game only for the ride it ended for that save, within kResumeWindowMs, at any speed and distance
inline constexpr std::uint8_t kReqHold = 0, kReqTest = 1, kReqNative = 2, kReqResume = 3;
inline constexpr double kResumeWindowMs = 10000.0;
// DENY reasons
enum class Deny : std::uint8_t {
    not_driving = 0,  // the driver's Sam does not drive that vehicle (any more)
    seat_taken = 1,   // another ride holds the seat
    moving = 2,       // the vehicle moves (boarding only below 2 m/s on the driver's side)
    not_near = 3,        // the driver's game does not see the passenger's Sam beside the vehicle
    off = 4,          // riding together is off on the driver's game (session.ini ride_together=0, a failed check)
    world = 5,        // not in the same world
    count
};
const char* DenyName(std::uint8_t r) noexcept;
// END reasons
enum class End : std::uint8_t {
    choice = 0,        // the passenger held X / B to get off
    link = 1,          // nothing from the driver for 1 s
    driver_off = 2,    // the driver's Sam got off (the driver's END, or its frames say so)
    vehicle_gone = 3,  // the vehicle is not in this world's registry (streamed out, removed)
    broken = 4,        // broken or its battery empty
    flipped = 5,       // upside down for 0.5 s
    crash = 6,         // a one-frame change of velocity over 8 m/s (the passenger takes the game's hit: D3-3)
    taken = 7,         // the game took the local Sam (a cutscene, a load, a parent, a pushed transform, a health drop)
    world = 8,         // a load, a reload, the title
    stopped = 9,       // the mod stopped (emergency stop, fault)
    driver_ended = 10, // the driver's game ended it (its rules, a new session)
    not_seated = 11,   // the driver's frames say nobody holds the seat while it moves
    timeout = 12,      // the driver's game did not see the passenger seated in time
    test = 13,         // hidden tests
    jumped = 14,       // the passenger held X / B again while waiting for the vehicle to slow down: off at any speed
    saving = 15,       // the game is about to save (or saves): the save guard holds the save until our Sam is off and placed
    count
};
const char* EndName(std::uint8_t r) noexcept;

struct Msg {
    MsgType type = MsgType::request;
    Seat seat = Seat::passenger;
    std::uint8_t reason = 0;
    std::uint8_t from = 0;           // the sender: 0 host, 1 guest
    std::uint32_t ride_id = 0;       // the passenger's game picks it (never 0); every message of one ride carries it
    std::uint32_t sender_ms = 0;
    std::uint32_t vehicle_id = 0;    // the host's game-actor id (vehicle_wire.hpp VKey.id)
    std::uint8_t kind = 0;           // 1 roadster, 2 truck, 3 trike (matches the id's actor type)
    std::uint32_t world = 0;         // the sender's world (coop_session's load counter): another world's message is dropped
};
// The whole channel-3 message (the SVM1 header of type kSvmType, then the 28-byte body):
//   "STQ1" | u8 type | u8 seat | u8 reason | u8 from | u32 ride id | u32 sender_ms | u32 vehicle id | u8 kind | 3 x 0
//   | u32 world                                                                                     (little endian)
// Returns kMessageSize, 0 for an invalid message (EncodeMsg refuses what DecodeMsg would).
std::size_t EncodeMsg(const Msg& m, std::uint8_t* out, std::size_t cap) noexcept;
// Strict: the SVM1 header (type 48, version 1, reserved 0), the exact size, the magic, a known type, the seat valid for
// the kind (the trike: pillion only; roadster and truck: passenger only), the reason in the type's range, from 0 / 1,
// ride id != 0, a valid kind whose actor type matches the id, zero reserved bytes.
bool DecodeMsg(const std::uint8_t* in, std::size_t size, Msg& out) noexcept;

// ---------------------------------------------------------------- seats (measured: docs section 24.2)
// The seat a passenger takes on a kind: passenger (roadster, truck), pillion (trike), none (anything else).
Seat SeatOf(std::uint8_t kind) noexcept;
// The passenger's ROOT in the vehicle's frame (metres along right, forward, up of the vehicle's transform rows) and the
// driver's for comparison. False for an unknown kind.
bool PassengerSeat(std::uint8_t kind, float (&off)[3]) noexcept;
bool DriverSeat(std::uint8_t kind, float (&off)[3]) noexcept;
// Where a passenger boards / gets off: the door point (vehicle frame) on the passenger's side, on the ground plane of the
// vehicle (z 0). The trike's pillion boards from the right.
bool DoorPoint(std::uint8_t kind, float (&off)[3]) noexcept;

// A point in the vehicle's frame to the world: pos + off.x * row0 + off.y * row1 + off.z * row2.
void ToWorld(const double (&pos)[3], const float (&rows)[9], const float (&off)[3], double (&out)[3]) noexcept;
// A world point into the vehicle's frame (the rows are orthonormal).
void ToLocal(const double (&pos)[3], const float (&rows)[9], const double (&world)[3], double (&out)[3]) noexcept;

// ---------------------------------------------------------------- boarding (M3.2)
inline constexpr double kBoardRadiusM = 2.5;     // Sam within this of the door point (horizontal)
inline constexpr double kBoardMaxSpeed = 1.0;    // the vehicle under this (m/s) on the passenger's side
inline constexpr double kDriverMaxSpeed = 2.0;   // ... and under this on the driver's side when the request arrives
inline constexpr double kHoldMs = 600.0;         // hold X (or the ride key) this long
inline constexpr double kRequestTimeoutMs = 3000.0;
enum class Block : std::uint8_t {
    ok = 0,
    off,           // the feature is off
    no_link,       // the link is down or the partner is in another world
    no_vehicle,    // the partner drives nothing we follow
    not_on_foot,   // our Sam rides something, sleeps, or is carried already
    too_far,       // beyond kBoardRadiusM of the door point
    moving,        // the vehicle moves
    no_seat,       // no seat for that kind
    busy,          // a request is pending, or the last ride ended under 1.5 s ago
    saving,        // the game has a save pending or is saving (save_guard): no boarding until it is written
    count
};
const char* BlockName(Block b) noexcept;
struct BoardIn {
    bool feature_on = false;
    bool link_up = false;           // the partner's frames are fresh and it is in our world
    bool partner_drives = false;    // our follower drives the partner's vehicle (vehicle_drive)
    std::uint8_t kind = 0;
    bool local_on_foot = false;     // awake, no parent entity, not carried
    double door_dist_m = 1e9;       // horizontal
    double vehicle_speed = 0;       // m/s
    bool busy = false;
    bool saving = false;            // a save is pending or being written (save_guard::Busy)
    // The game's own "Use Vehicle" prompt (native): the game's finder already wants Sam close to and facing the vehicle,
    // from any side, so the door rule becomes "within kNativeReachM of the vehicle's origin" (horizontal).
    bool native = false;
    double origin_dist_m = 1e9;
};
inline constexpr double kNativeReachM = 7.0;
Block BoardCheck(const BoardIn& in) noexcept;

// The driver's game answers a REQUEST.
struct DriverIn {
    bool feature_on = false;
    bool same_world = true;
    bool we_drive_it = false;       // our Sam holds that vehicle's driver seat
    bool seat_taken = false;        // another ride (another ride id) holds the seat
    double speed = 0;               // the vehicle's speed (m/s)
    bool have_partner = false;      // the partner body's place is known
    double partner_dist_m = 1e9;    // the partner body to the door point (horizontal)
    bool native = false;            // a kReqNative request: the partner pressed the game's "Use Vehicle" from any side
    double partner_origin_dist_m = 1e9;  // the partner body to the vehicle's origin (horizontal)
};
inline constexpr double kDriverSeesRadiusM = 5.0;  // the driver's game accepts the passenger's Sam within this of the door
inline constexpr double kDriverSeesNativeM = 8.5;  // ... or, for kReqNative, within this of the vehicle's origin
bool DriverAccepts(const DriverIn& in, Deny* why) noexcept;

// While the driver's game carries the partner: the partner body sits in our vehicle while the ride is accepted and the
// partner's pose says it is our passenger (STP1 kPosePassenger) within the last second, or the accept is under 3 s old.
inline constexpr double kAcceptGraceMs = 3000.0, kPassengerBitMs = 1000.0;
bool DriverPlacesBody(bool accepted, double since_accept_ms, double passenger_bit_age_ms) noexcept;
// The driver's game ends a ride the partner no longer shows as seated (after the grace).
inline constexpr double kDriverTimeoutMs = 5000.0;
bool DriverTimedOut(bool accepted, double since_accept_ms, double passenger_bit_age_ms) noexcept;

// ---------------------------------------------------------------- forced exits (M3.4), the passenger's game
inline constexpr double kLinkSilentMs = 1000.0;
// v0.21: a carried passenger is not put off by a short silence (Wi-Fi roaming, a relay hiccup, a loss burst): the ride
// ends on the link only once the link says the session is gone, or after kLinkSilentCarriedMs without a pose (under the
// driver's kDriverTimeoutMs, so both games end it alike). Meanwhile our follower keeps the vehicle (vehicle_drive: idle
// inputs, it rolls to a stop, no detach until vwire::kStallEndCarriedMs) and our Sam stays on its seat.
inline constexpr double kLinkSilentCarriedMs = 4500.0;
static_assert(kLinkSilentCarriedMs < kDriverTimeoutMs, "the passenger's grace ends before the driver's timeout");
inline constexpr double kCrashDvMps = 8.0;
inline constexpr double kFlipUpZ = 0.3, kFlipMs = 500.0;
inline constexpr double kNotDrivenMs = 400.0;     // our follower let go (the driver's frames ended) this long
inline constexpr double kNotSeatedSpeed = 2.0;    // m/s: the driver's frames say nobody holds the seat at this speed
struct ForcedIn {
    double link_silent_ms = 0;      // since the partner's last pose
    bool link_lost = false;         // the link's own state: not connected (any silence over kLinkSilentMs then ends it)
    double not_driven_ms = 0;       // since our follower last drove that vehicle (0 while it does)
    bool vehicle_live = true;       // in the registry, its entity live
    bool broken = false;            // broken (+0x560 & 0x2000) or ride state 5 / 6
    bool flipped = false;           // FlipDetector
    double crash_dv = 0;            // CrashDetector's newest jump (m/s), 0 none
    bool driver_not_seated = false; // the newest driver frame has no seated flag while the vehicle moves
    double speed = 0;
    bool taken = false;             // the game took our Sam
    bool world_changed = false;
    bool stopped = false;
    bool driver_ended = false;      // the driver's END arrived
    bool saving = false;            // a save is pending (the save guard holds it for us) or being written
};
// The first reason that applies, in the order of the fields above (stopped, world and taken first: they are the game's or
// the mod's); End::count when the ride goes on.
End ForcedExit(const ForcedIn& in) noexcept;
// Getting off by choice: below this speed (m/s).
inline constexpr double kExitMaxSpeed = 2.0;

// Hold to board / get off: true once when `down` was held kHoldMs; it fires again only after a release.
class HoldDetector {
public:
    bool Update(bool down, double now_ms) noexcept;
    double HeldMs(double now_ms) const noexcept { return since_ < 0 ? 0 : now_ms - since_; }
    void Reset() noexcept { since_ = -1, fired_ = false, need_release_ = true; }
    // After Reset (or a fire) the button must be seen up once before a new hold counts (a hold that boarded never
    // exits at once).
private:
    double since_ = -1;
    bool fired_ = false;
    bool need_release_ = true;
};

// Press to get off (the game's own "Use Vehicle" prompt is a press, so getting off is one too): true once on a press that
// follows a release. After Reset (the press that boarded may still be held) the button must be seen up first.
class PressDetector {
public:
    bool Update(bool down) noexcept {
        if (!down) {
            need_release_ = false;
            return false;
        }
        if (need_release_) return false;
        need_release_ = true;
        return true;
    }
    void Reset() noexcept { need_release_ = true; }
private:
    bool need_release_ = true;
};

// The native press from the game (the ride action's TryStart, polled every frame while the button is down): a new press
// only after kNativePressGapMs without one, so one held button is one press.
inline constexpr double kNativePressGapMs = 400.0;
inline bool NativePressIsNew(double last_press_ms, double now_ms) noexcept { return !(now_ms - last_press_ms < kNativePressGapMs); }

// Round 30 (run 20261127-171443, leg 2): the board press without the game's prompt. The game's own targeting (FUN_142823210,
// FUN_142796410 and the provider's line-of-sight early out) can decide not to offer Use Vehicle on the partner's truck even
// where the mod allows boarding (15 s of presses beside the truck, no prompt). The provider post-hook can only lift a RideOn
// the game made; it cannot make one. So, with the native prompt on, a press of the board button (pad X / Square, key F) is
// taken as the board press when all of these hold:
//   * boarding is allowed (BoardCheck ok, the partner's vehicle known) and has been for kNoPromptQuietMs;
//   * our Sam stands within kBoardRadiusM of the seat's door point (the door rule, not the native 7 m one; round 31: the cab
//     seat is the only seat, so this is BoardCheck's own door distance);
//   * the game offered no RideOn on the partner's vehicle for kNoPromptQuietMs (else its own press path has it);
//   * the game offered no RideOn on any other vehicle for kNoPromptQuietMs (that press is the game's own get-on);
//   * the game's TryStart took no press this tick, and none within kNoPromptGameMs;
//   * the last such press was kNoPromptGapMs ago or more.
// The press then sends the same REQUEST (kReqNative) as the game's prompt does. Nothing new is drawn.
inline constexpr double kNoPromptQuietMs = 300.0, kNoPromptGameMs = 500.0, kNoPromptGapMs = 1000.0;
enum class NoPrompt : std::uint8_t {
    board = 0,       // take it: send the REQUEST
    idle,            // no press this tick, or the native prompt is off (the ImGui hold prompt boards then)
    not_allowed,     // boarding is not allowed now
    too_soon,        // boarding allowed for under kNoPromptQuietMs, or the last such press under kNoPromptGapMs ago
    too_far,         // beyond kBoardRadiusM of the door point
    game_prompt,     // the game offers RideOn on the partner's vehicle: its own press path boards
    other_vehicle,   // the game offers RideOn on another vehicle: the press is its get-on
    game_press,      // the game's TryStart took a press
    count
};
const char* NoPromptName(NoPrompt n) noexcept;
struct NoPromptIn {
    bool native_on = false;
    bool pressed = false;               // the board button went down this tick (after a release)
    bool allowed = false;
    double allowed_for_ms = 0;
    double approach_dist_m = 1e9;       // horizontal, to the seat's door point
    double partner_prompt_age_ms = 1e18;
    double other_prompt_age_ms = 1e18;
    bool game_press_now = false;
    double game_press_age_ms = 1e18;
    double since_last_ms = 1e18;
};
NoPrompt NoPromptPress(const NoPromptIn& in) noexcept;

// Up axis z (the vehicle's third row, world z) under kFlipUpZ for kFlipMs.
class FlipDetector {
public:
    bool Update(double up_z, double now_ms) noexcept;
    void Reset() noexcept { since_ = -1; }
private:
    double since_ = -1;
};

// The driver's frames' velocity (sender-measured, at their own keys): a change over kCrashDvMps between two frames at most
// 100 ms apart is a crash. Frames further apart start over (a gap is not a crash).
class CrashDetector {
public:
    // Returns the jump (m/s) when this frame is a crash, else 0. Older or repeated keys are ignored.
    double Update(const float (&vel)[3], double key_ms) noexcept;
    void Reset() noexcept { have_ = false; }
private:
    bool have_ = false;
    double key_ = 0;
    float vel_[3] = {};
};

// v0.21 (play test 3: a false crash exit at a steady 11 m/s from one odd frame): a crash only when the driver's vehicle
// really changed speed and stays changed. On each newer frame k: the median horizontal velocity of the frames in
// [k - kConfirmBeforeFromMs, k - kConfirmBeforeToMs] against those in [k - kConfirmAfterMs, k] differ by over kCrashDvMps,
// the newest 3 frames agree within kConfirmSpreadMps, and the game's own speed field (km/h, the frames' speed_kmh; < 0
// unknown) dropped by kConfirmKmhDrop or more. Horizontal only (a crest or a landing changes z). Fires once per
// kConfirmRefireMs. A one-frame jump the v0.20 rule would have taken that is not confirmed within kConfirmWaitMs is reported
// once through TakeRejected (for the log).
inline constexpr double kConfirmBeforeFromMs = 350.0, kConfirmBeforeToMs = 200.0, kConfirmAfterMs = 150.0;
inline constexpr double kConfirmSpreadMps = 3.0, kConfirmKmhDrop = 28.8, kConfirmRefireMs = 1000.0, kConfirmWaitMs = 400.0;
class CrashConfirm {
public:
    double Update(const float (&vel)[3], float speed_kmh, double key_ms) noexcept;
    // the one-frame jump (m/s) that was not confirmed, once; 0 none
    double TakeRejected() noexcept {
        const double r = rejected_;
        rejected_ = 0;
        return r;
    }
    void Reset() noexcept { *this = CrashConfirm{}; }
private:
    struct S {
        double key = 0;
        float vx = 0, vy = 0, kmh = -1;
    };
    static constexpr int kN = 32;
    S s_[kN];
    int n_ = 0;
    double fired_key_ = -1e18;
    double cand_key_ = -1e18, cand_dv_ = 0, rejected_ = 0;
};

// ---------------------------------------------------------------- where the passenger is put off
struct ExitSpot {
    double pos[3] = {};   // world, at the vehicle's base height (the game's ground probe finds the ground from there)
    float facing[2] = {}; // horizontal, toward the vehicle
    int side = 1;         // +1 the vehicle's right, -1 its left
};
// Beside the vehicle: by choice on the passenger's side (the door point); forced on the side away from the vehicle's
// sideways velocity (the passenger's side unless the vehicle slides toward it), moved back along a fast vehicle's
// forward velocity (up to 3 m) so he is not put down in its path. Always on the ground plane (world z up): the vehicle's
// right and forward axes projected onto it (a vehicle on its side or roof: the spot is outside its footprint, at the
// height of its lowest point, never along its tilted up or right axis into the ground or onto the wreck).
ExitSpot ExitPlacement(std::uint8_t kind, const double (&pos)[3], const float (&rows)[9], const float (&vel)[3], bool forced) noexcept;

// ---------------------------------------------------------------- the save guard (save_guard.cpp; docs section 24.12)
// Every save the game makes is posted into one pending-save holder; the game's save processor issues it into the save
// system's queue at once (never delayed by the mod: some posters, e.g. the autosave task 0x143419760, clear a request
// that is not queued by their next step). The save system then prepares the capture job (S+0x98, task state 2) and its
// kick 0x14362DB80 (once per frame) submits it; the job reads Sam's entity transform. The kick itself waits by returning
// early (its own refusal path, up to 91 frames), so the guard's detour of the kick decides each frame with SaveGate:
//   run         call the game's kick now (nobody carried, or our Sam is off, placed and settled);
//   hold        return without calling it: the save stays queued with its job prepared, exactly as the game made them, and
//               the kick (the game's own code) submits it on a later frame. Meanwhile the ride ends (End::saving): our
//               Sam is put on the ground beside the vehicle with the capsule on, the pose and the input back;
//   run_capped  held for kSaveHoldCapMs already: call the kick anyway (the save runs unchanged, logged). A save is never
//               dropped or edited by the mod: the worst case is a save with Sam where he is.
// Once a capture was released or capped (`done`), it is never held again (the cap is per capture, not per frame).
// The post-load save (the game's own quick save after a load, kind 10) is never held: the load flow waits for it through
// the save queue, and no ride survives a load.
enum class SaveStep : std::uint8_t { run = 0, hold, run_capped };
const char* SaveStepName(SaveStep s) noexcept;
inline constexpr unsigned kSaveSettleFrames = 2;    // kick frames in a row with our Sam neither carried, placing nor in the box
inline constexpr double kSaveHoldCapMs = 3000.0;    // the longest hold of one capture
struct SaveHoldIn {
    bool pending = false;           // a capture job is prepared and not submitted yet (S+0x98)
    bool post_load = false;         // it is the post-load save
    bool done = false;              // this capture was released or capped already: never held again
    bool carried = false;           // our Sam is carried (ride_together)
    bool placing = false;           // his exit placement still waits for the game's PlaceOn (its rate limit, a ground probe)
    bool in_vehicle = false;        // just after a ride: his entity still reads inside the ridden vehicle's box (not placed)
    unsigned settled_frames = 0;    // kick frames in a row with none of the three (saturating)
    double held_ms = -1;            // since this capture was first held (< 0: not held)
};
SaveStep SaveGate(const SaveHoldIn& in) noexcept;
// Our Sam at `p` is inside the vehicle's body box (vwire::KindHalfExtents: right / forward +-half + kBoxMarginM, up from
// -kBoxMarginBelowM to 2 half + kBoxMarginBelowM in its frame `rows` at `veh`, its base): the seat is, the exit spot is not.
inline constexpr double kBoxMarginM = 0.3, kBoxMarginBelowM = 0.5;
bool InVehicleBox(std::uint8_t kind, const double (&veh)[3], const float (&rows)[9], const double (&p)[3]) noexcept;
// The height for the save-time entity teleport (a sleeping passenger: no ground probe of PlaceOn): the higher of the exit
// spot's (ExitPlacement) and the vehicle's ground plane under the spot (upright, its up axis z >= 0.5), plus
// kSaveSpotLiftM, so the spot is never inside a slope that rises beside the vehicle (he drops onto the ground).
inline constexpr double kSaveSpotLiftM = 0.8;
double SaveSpotHeight(const double (&veh)[3], const float (&rows)[9], const double (&spot)[3]) noexcept;

}  // namespace st::rwire
