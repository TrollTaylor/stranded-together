// The carried passenger's camera (docs/real-partner-body.md section 24.15; play test 3 of v0.20: "the camera for the
// passenger is way too buggy ... zoomed in a bunch ... phasing through the car").
//
// The game has no "Sam rides" camera mode: its ride action (state 2, 0x142774A40) REQUESTS the ride camera parameter from
// the player's DSCameraMode every frame and feeds it the vehicle's position as the parameter's "Manual" base. The carried
// passenger's Sam is not in that action, so his camera stayed the on-foot one (4.15 m behind Sam, focused on Sam whom the
// carry teleports every tick, the on-foot pitch range): inside / just behind the truck's cab. Feed() makes the same four
// writes for him, with the vehicle he rides (the partner's, our follower of it): the driver's own camera (10.75 m behind
// the truck, 11.25 on a road; roadster 5.85 / 6.35; trike 5.35 / 6.35), following the vehicle, not Sam. Look input is the
// on-foot one (the player's ride id stays "none"), so the mouse and the right stick turn it as before.
//
// Game thread. Byte checks at Install (kCamSites); a mismatch: off, one log line, the passenger keeps the on-foot camera.
#pragma once
#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "game_api.hpp"

namespace st::ridecam {

void Install(const Config& config) noexcept;
bool Active() noexcept;
const char* OffReason() noexcept;
// True when our camera filter detour sits on FilterIgnorePlayer 0x14382FD30: its byte check (a superset of vhit's filter
// init check) passed before the hook went in, so vhit's later check of those bytes reads our jump instead.
bool FilterInitHooked() noexcept;

// Every carry tick while our Sam is carried and awake: the ride camera of `kind` (1 roadster, 2 truck, 3 trike) on the
// vehicle entity `vehicle` at `xf` (its transform this tick), moved `lead` (its velocity times one frame, as the seat): the
// "Manual" base is the vehicle's camera point (its +0xA60 local point, as the ride action's 0x14283E090), its yaw the
// follow reference (+0x730).
void Feed(game::Entity* sam, std::uintptr_t vehicle, std::uint8_t kind, const game::WorldTransform& xf, const double (&lead)[3]) noexcept;
// After Feed, every carry tick: the heading's slow follow behind the moving vehicle (paused 2 s by any look input).
void Follow(game::Entity* sam, const game::WorldTransform& xf, double speed_mps) noexcept;
// The carry ended (any reason): no more requests; the game's camera goes back to the on-foot one by its own blend.
void Stop(const char* why) noexcept;
// Every tick: the camera meter (our camera against the vehicle we ride or drive; `vehicle` 0 = none). `carried`: we are
// the passenger (the feed's numbers), else the driver (the game's own ride camera: the reference).
void Meter(game::Entity* local, std::uintptr_t vehicle, std::uint8_t kind, bool carried, double now_ms) noexcept;
// ride_test 20 <0|1> the feed off / on, 21 <0|1> the meter's 1 s lines off (5 s) / on.
bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

}  // namespace st::ridecam
