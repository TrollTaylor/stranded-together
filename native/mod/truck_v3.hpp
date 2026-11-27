// Round 28 P2 (trucks M0, docs/enemy-sync.md 14.20): the truck controller probe. Read-only: it writes nothing in the game.
//
// For every vehicle whose driver seat (+0x63C, +0x8B0) names a MULE, `truck_v3 9` logs whether the vehicle's input object
// [vehicle+0x528] is the driver MULE's own NPC drive controller input (ctl = actor+0x3D0, its input ctl+0x30 = actor+0x400;
// Attach 0x142F5A590 stores it through SetController at 0x142F5A65A), the controller's state / driver / vehicle / AI drive
// component (+0x18 / +0x20 / +0x00 / +0x100), the MULE's ride fields (ride = actor+0x3C0: +0 seat, +0xA60 vehicle id, +0xA68 the
// vehicle handle, +0xA84 bit 0 riding), the 48 input bytes (+0x08..+0x37), the ride state, the speed and the life (plan Q2).
// `truck_v3 8` logs a seated MULE's world transform in its truck's frame and its capsule (plan Q7). `truck_v3 1` watches
// (board / leave / seat / controller-state changes; a compact drive line every 5 s). A counting detour on the get-off function
// 0x142AFB010 (installed at start-up when the key is on; atomics and ReadProcessMemory only, any thread) records its callers.
//
// Every line starts "[truckv3]". Gated by session.ini vehicle_truck_probe (default 0): with 0 nothing is installed and every op
// is refused.
#pragma once

#include <cstddef>
#include <cstdint>

namespace st::truckv3 {

// ---- round 28 T1 (vehicle_mule_v3, default 0; docs/enemy-sync.md 14.21): host-owned MULE trucks. Defined in vehicle_drive.cpp
// (the hello, the gate and the frames live there); game thread.
// Whether V3 is live here: the key on in this game and the partner's fresh STV3 hello of this link session (trucks::V3Gate).
bool V3Live() noexcept;
// Guest: whether a (bound) MULE truck of her world is within `radius_m` of `pos` (the host copy's place of a MULE the host sees
// in a vehicle); a truck the host's MULE drives and her follower holds is preferred. False on the host and while V3 is not live.
bool V3SeatNear(const double (&pos)[3], double radius_m, std::uint32_t* truck_id) noexcept;
// truck_v3 3: the "[truckv3] v3 status:" line (the counters the gate reads). Works with vehicle_truck_probe=0.
bool V3Status(char* msg, std::size_t cap) noexcept;

// Bootstrap thread (dllmain, after MinHook is initialised): the byte checks and, when they pass, the get-off detour. Nothing
// when `probe_on` is false.
void Install(bool probe_on) noexcept;
// player_sync, every frame (game thread): drains the get-off ring into the log and runs the watch.
void Tick(double now_ms) noexcept;
// truck_v3 <op> [a] (debug action, game thread): 0 the watch off + the status line; 1 [period s] the watch on (default 1 s);
// 3 the V3 status line (round 28 T1; any probe key); 8 the seat dump; 9 the identity dump. Every vehicle and every MULE of this game.
bool Command(const double (&v)[3], char* msg, std::size_t cap) noexcept;
// The mod's emergency stop (any thread): the watch off and the detour's records dropped (it passes through).
void EmergencyStop() noexcept;

}  // namespace st::truckv3
