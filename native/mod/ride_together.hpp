// Riding together (design milestone M3; docs/real-partner-body.md section 24). Runs with the real shared vehicles
// (vehicle_sync=5 with host_save=1: vehicle_share + vehicle_drive) and session.ini ride_together (default 1).
//
// "Players can ride together in vehicles", "let players ride in the passenger seat too". The game seats one player per
// vehicle: the driver, whose game owns and drives it (M2); the other game's unit of that vehicle is driven by the game's NPC
// controller (our follower). The partner rides along as a CARRIED passenger:
//
//   boarding (M3.2)   beside the partner-driven vehicle (within 2.5 m of its passenger door, under 1 m/s, on foot) the
//                     co-op banner says "Hold [X] to ride with <partner>"; a 0.6 s hold of X (pad) or F (keyboard: the
//                     game's RideAndGetOff key) sends RIDE REQUEST (STQ1 in SVM1 type 48, channel 3, ride_wire.hpp). A tap
//                     stays the game's own get-on, which the M2 lock refuses. The driver's game ACCEPTs unless its Sam does
//                     not drive that vehicle, the seat is taken, it moves (2 m/s) or it does not see the partner beside it.
//   carried           the passenger's own Sam: capsule off (0x14243A740, a counter: exactly one Enable 0x14243A9E0 later),
//                     placed every tick at the vehicle's transform (one frame ahead along its velocity) times the seat with
//                     the game's entity teleport 0x14235A010(Sam, &wt, 0, 0) (proven at 0.000 m over 1137 ticks); the game's
//                     movement / action input kept from it while he is awake (overlay_input SetRideCarry: look and a Start
//                     tap pass; asleep in the game's menu the block is lifted); its pose
//                     the driver's seated pose (realbody::SetRidePose, D3-2); its STP1 says kPosePassenger | kPoseInVehicle
//                     (the driver's game takes the partner capsule out and sits the partner body in its own vehicle).
//   driver's screen   the partner body at OUR vehicle's transform times the passenger seat (PassengerTarget), its pose the
//                     partner's STJ1 (seated, from the override above).
//   exits (M3.3)      hold X / B (or F) under 2 m/s: put beside the passenger door with the game's PlaceOn (mode 1, its ground
//                     probe), capsule on, input and pose back, END to the driver. Above 2 m/s the hold waits up to 3 s for the
//                     vehicle to slow down; a second hold meanwhile jumps off at any speed (forced placement; above 8 m/s the
//                     D3-3 hit).
//   forced (M3.4)     the passenger's game decides for its own Sam: the partner silent for 1 s, our follower lets go (the
//                     driver got off: also its END), the vehicle gone, broken or out of battery, flipped 0.5 s, a crash (a
//                     one-frame velocity change over 8 m/s in the driver's frames: then the M4 hit handler, D3-3), the game
//                     takes our Sam (a parent, a cutscene / load / game-over state current or requested, Sam+0x678, a pushed
//                     transform, a sequence binding, a health drop, the game saving, asleep 60 s), a world change, the mod's
//                     stop. Put beside the vehicle away from its sideways velocity, moved back along a fast one's path,
//                     always on the ground plane (a vehicle on its side or roof too), capsule on. Every placement first asks
//                     game::PlacementGate: when the game has Sam (a load, a game state, +0x678, a sequence ...) there is
//                     none (the game places him; the capsule and the input come back); while he sleeps or the game saves it
//                     waits; the entity teleport only stands in for PlaceOn's rate limit.
//   ownership         unchanged (VOWN, M2.7): the passenger never owns. The driver getting off ends the ride.
//
// Every game address is in the start-up byte-check table (kSites in ride_together.cpp); a mismatch turns riding off with
// a one-line reason in the log and the co-op banner. Game calls run on the game thread under SEH; nothing of the game's is
// freed or evicted.
#pragma once
#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "game_api.hpp"

namespace st::ride {

// Bootstrap thread, after vdrive::Install: the byte checks and the channel 3 handler.
void Install(const Config& config) noexcept;
// Riding together is on (checks passed, the shared vehicles and remote driving on, ride_together=1).
bool Active() noexcept;
const char* OffReason() noexcept;

struct TickIn {
    game::Entity* local = nullptr;
    bool local_ok = false;            // a transform this tick
    bool local_active = false;        // awake
    bool have_partner = false;        // the partner body's place this tick
    game::WorldTransform partner{};
    double partner_rx_ms = -1e18;     // the newest pose from the partner arrived (tick clock)
    std::uint32_t partner_flags = 0;  // its STP1 flags
    double play_ms = 0;               // the partner's play time (sender axis), when have_play
    bool have_play = false;
};
// Game thread, every tick, after vdrive::Tick.
void Tick(const TickIn& in, double now_ms) noexcept;
// A new link session: a ride ends (the passenger's Sam is put off, the driver forgets).
void NewSession() noexcept;
// The per-frame tick faulted: the input block and the pose override go now (any thread); the capsule comes back and Sam is
// put beside the vehicle on the next game-thread pass (AfterStop), once; a pending REQUEST is withdrawn (END), and from then
// on every ride message is refused (a late ACCEPT gets END, never a carry).
void EmergencyStop() noexcept;
void AfterStop() noexcept;

// Our STP1 flags while carried (kPosePassenger | kPoseInVehicle), else 0.
std::uint32_t PoseFlags() noexcept;
// Our Sam is carried as a passenger (or the solo seat test carries him).
bool Carried() noexcept;
// Driver's game: while the partner rides our vehicle, its body's place this tick (our vehicle's transform, one frame ahead,
// times the passenger seat). False otherwise.
bool PassengerTarget(game::WorldTransform* target) noexcept;
// The vehicle entity the partner rides in as our passenger (driver side), or our Sam rides in (passenger side); 0 none.
std::uintptr_t RideVehicle() noexcept;

// ---- for M4 (partner_tackle): the passenger thrown off in a crash takes the game's own hit and knock-down (decision D3-3).
// Called on the game thread right after the crash exit placed our Sam beside the vehicle and turned his capsule on:
// `vehicle_entity` the vehicle he rode (this world's unit), `dv_mps` the velocity jump of the crash, `speed_mps` the
// vehicle's speed before it. Return true when the hit was applied. Without a handler the exit is logged "no hit handler".
using CrashHitFn = bool (*)(std::uintptr_t vehicle_entity, double dv_mps, double speed_mps) noexcept;
void SetCrashHitHandler(CrashHitFn fn) noexcept;

// Debug actions (debug_actions.cpp; game thread):
//   "Ride state": the state, the counters, the seats.
//   "Ride board": board the partner-driven vehicle now (as a completed hold; the boarding checks apply).
//   "Ride exit" [1]: get off now (1: also above 2 m/s, hidden tests). In the game: a second hold within the 3 s wait above
//     2 m/s jumps off (End jumped).
//   "Seat measure" [kind]: the vehicle our Sam drives (or the last one he drove): his root and pelvis in its frame, its
//     joint 1 and the game's HLP_Driver / HLP_Passenger / HLP_C_Driver helpers through it, the passenger seat in use;
//     while he drives, his pose is kept as the solo test's seated pose.
//   "Ride test" op: 1 [kind] carry our Sam on the last measured vehicle (solo seat check, the kept pose), 2 end it,
//     4 [dv]: a crash of dv m/s (0 = 12) next tick (passenger: the crash exit and the M4 hit handler), 5 roll the vehicle our
//     Sam drives over (45 x 4 deg about its forward axis by RequestTeleport; the passenger's flip exit), 6 stop the vehicle our Sam drives dead
//     (RequestTeleport onto its own place: a crash in its frames), 7 the taken exit next tick, 31 / 32 / 33 <m> the x / y / z
//     of the measured kind's passenger seat (tuning, this session), 30 the seat table again, 8 [m] our Sam at the passenger door
//     of the vehicle the partner drives here, 9 the module's emergency stop (hidden tests; riding stays off), 10 ask to ride
//     and the emergency stop in the same call (hidden tests: the driver's ACCEPT arrives after the stop).
bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;
bool StateCommand(char* msg, std::size_t cap) noexcept;
bool BoardCommand(char* msg, std::size_t cap) noexcept;
bool ExitCommand(const double (&v)[3], char* msg, std::size_t cap) noexcept;
bool SeatMeasureCommand(const double (&v)[3], char* msg, std::size_t cap) noexcept;

}  // namespace st::ride
