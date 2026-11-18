// Vehicles (docs/real-partner-body.md section 22; session.ini vehicle_sync, default 1).
//
// Sender: the vehicle the local Sam rides (his parent entity, +0x70, checked against the three drivable vehicle
// classes' vtables) is read every pose send tick: its kind and ride state, its drivable resource (path + GUID, an
// allow-list), its world transform, Sam relative to it (the seat) and its skinned pose (wheels, steering). After he
// gets off, the same vehicle is still reported as parked for up to 60 s while he stays within 30 m, so the other
// screen's copy neither pops in nor out at the mount and the dismount. One STM1 frame per send tick.
//
// Receiver: a visual, non-drivable copy of the partner's vehicle (a cutscene actor: the trike's and the truck's
// demo bodies, on the same art and skeleton as the drivable ones; the roadster has no cutscene body, so its
// construction hologram), spawned through the bodies' path + GUID route, checked (no AI, controller, physics or
// interaction components; the hologram must pass the welcome hologram's checks; the vehicle's skeleton), placed every
// tick at the partner's play time (the same timeline as the partner body) and posed from the sent joints through a
// pose target of the animate hook (realbody::kVehiclePoseTarget). Removed when the partner reports no vehicle, frames
// stop for 2 s, it stays broken for 3 s, the partner body goes, Sam sleeps for 1 s (loads, cutscenes), the link session
// changes or the key is turned off. Local vehicles are never touched. vehicle_sync=1 (default) shows the roadster's copy
// only; 3 also the trike's and the truck's (not yet checked against a real drive); 2 those two only.
//
// Game thread only (st::sync::Tick).
#pragma once
#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "game_api.hpp"
#include "pose_wire.hpp"

namespace st::vsync {

void Init(const Config& config) noexcept;

// ---- sender (the pose send tick). `local_wt` = the local Sam's world transform this tick; `sender_ms` / `seq` are
// the STP1 frame's. False: nothing to report (no vehicle, off, or a check failed: logged once).
bool TakeSend(game::Entity* local, const game::WorldTransform& local_wt, std::uint32_t sender_ms, std::uint32_t seq, double now_ms,
              wire::VehicleFrame* out) noexcept;
// Every tick (not only send ticks): the trace (action vehicle_sync 3) logs our vehicle's transform at tick rate.
void SenderTick(game::Entity* local, double now_ms) noexcept;

// ---- receiver
// A received (or echoed) frame, keyed on the pose ring's unwrapped sender axis.
void OnFrame(const wire::VehicleFrame& f, double key_ms, double now_ms) noexcept;
void NewSession() noexcept;  // a new link session: frames and the copy go

struct TickIn {
    bool local_ok = false;          // the local player is awake and has a transform
    bool partner_present = false;   // the partner has a body on our screen
    bool have_play = false;         // the pose ring gave a play time this tick
    double play_ms = 0;             // the partner's play time (sender axis)
    bool have_body = false;         // the partner body's target transform this tick (the STP1 sample)
    game::WorldTransform body{};
};
void Tick(const TickIn& in, double now_ms) noexcept;
void RemoveCopy(const char* why) noexcept;  // the copy goes (and its pose target first)
// The per-frame sync faulted and Tick will not run again: the copy is hidden where it stands, nothing more is sent.
void EmergencyStop() noexcept;

// The copy entity while live (name tags, diagnostics), nullptr otherwise.
game::Entity* CopyEntity() noexcept;

// action vehicle_sync: 0 log, 1 mode v[1] (0 off / 1 roadster / 2 solid only / 3 all), 2 copy override v[1] (0 auto, 1 roadster
// hologram, 2 trike, 3 truck), 3 trace for v[1] ms, 4 copy offset in the vehicle's frame (v[1] forward, v[2] up, cm),
// 5 describe our current / last vehicle.
bool DebugCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

}  // namespace st::vsync
