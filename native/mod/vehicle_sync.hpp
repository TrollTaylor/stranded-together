// Vehicles (docs/real-partner-body.md section 22; session.ini vehicle_sync, default 1).
//
// Sender: the vehicle the local Sam rides (his parent entity, +0x70, checked against the three drivable vehicle
// classes' vtables) is read every pose send tick: its kind and ride state, its drivable resource (path + GUID, an
// allow-list), its world transform, Sam relative to it (the seat) and its skinned pose (wheels, steering). After he
// gets off, the same vehicle is still reported as parked while it is there (the game's registry) and he stays within
// 250 m of it, with no time limit (section 22.13), so the other screen's copy stands where it stands on his. One STM1
// frame per send tick.
//
// Receiver: a visual, non-drivable copy of the partner's vehicle (a cutscene actor: the trike's and the truck's
// demo bodies, on the same art and skeleton as the drivable ones, their collision turned off on each copy before it is
// shown and checked every 250 ms (section 22.11); the roadster has no cutscene body, so its construction hologram),
// spawned through the bodies' path + GUID route, checked (no AI, controller, physics or
// interaction components; the hologram must pass the welcome hologram's checks; the vehicle's skeleton), placed every
// tick at the partner's play time (the same timeline as the partner body) and posed from the sent joints through a
// pose target of the animate hook (realbody::kVehiclePoseTarget). Removed when the partner reports no vehicle, frames
// stop for 2 s, it stays broken for 3 s, the partner body goes, Sam sleeps for 1 s (loads, cutscenes), the link session
// changes or the key is turned off (while the partner is silent altogether, its pose too, it is held where it stands
// until the body goes). Local vehicles are only read (the game's vehicle registry): a copy is hidden while the watcher's
// own vehicle of the same kind stands in its place (vehicle_overlap_hide). The solid copies wear the partner vehicle's
// paint and variations and show its battery / defense packs (vehicle_look, vehicle_packs: section 22.7; the packs are
// copies of the game's own pack entities, placed in the copy's frame), and its wear, lights and cargo (STM1 version 3,
// section 22.12: the corrosion and lamp shader variables set on the copy, the carrier's pieces drawn as case props). The copy's model is updated every frame (the
// 240 Hz group) and its joints interpolated in its own update (smooth_pose). vehicle_sync=1 (default) and 2: the solid
// trike and truck copies and the roadster's hologram; 3 all three kinds as construction holograms (opt-in); 4 the
// roadster's copy only. A solid copy whose collision cannot be made safe is replaced by its hologram (logged).
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
// The look of the vehicle we report (STV1 item 255, section 22.7: its variations and paint) when it should go out:
// read every 500 ms while TakeSend reports it, sent when it changed and every 2 s. MarkLookUnsent: a failed send or a
// new link session (sent again at the next read).
bool TakeSendLook(double now_ms, wire::GearLook* out) noexcept;
void MarkLookUnsent() noexcept;

// ---- receiver
// A received (or echoed) frame, keyed on the pose ring's unwrapped sender axis.
void OnFrame(const wire::VehicleFrame& f, double key_ms, double now_ms) noexcept;
void NewSession() noexcept;  // a new link session: frames and the copy go
// The partner vehicle's look (a received or echoed STV1 item 255): applied to the solid copy of the same art.
void OnLook(const wire::GearLook& l) noexcept;

struct TickIn {
    bool local_ok = false;          // the local player is awake and has a transform
    bool partner_present = false;   // the partner has a body on our screen
    bool have_play = false;         // the pose ring gave a play time this tick
    double play_ms = 0;             // the partner's play time (sender axis)
    bool have_body = false;         // the partner body's target transform this tick (the STP1 sample)
    game::WorldTransform body{};
    double pose_age_ms = 1e9;       // since the partner's last pose frame (STP1): over 0.5 s, the partner is silent
    game::Entity* local = nullptr;  // the local Sam (only read: the copies are spawned away from the watcher)
};
void Tick(const TickIn& in, double now_ms) noexcept;
void RemoveCopy(const char* why) noexcept;  // the copy goes (and its pose target first)

// ---- collision off on our own spawned copies (section 22.11 / 22.13; game thread). Entity::SetCollisionEnabled(false)
// on that one entity (byte-checked; the vtable slot checked per call), then read back: true when its collision flag is
// clear and no model part carries a physics instance. The gear props (partner_gear) use it too.
bool CopyCollisionOff(game::Entity* e, const char* what) noexcept;
// A live copy checked again: 1 off; 0 found on again and turned off again (logged); -1 unreadable or not off.
int CopyCollisionKeepOff(game::Entity* e, const char* what) noexcept;
// The per-frame sync faulted and Tick will not run again: the copy is hidden where it stands, nothing more is sent.
void EmergencyStop() noexcept;
// Hidden tests (action vehicle_sync 10 <ms>, automation runs only): our sends are held until then, so the other side
// sees a silent partner (a link stall, a load or a pause) while the link itself stays up.
bool SendMuted(double now_ms) noexcept;

// The copy entity while live (name tags, diagnostics), nullptr otherwise.
game::Entity* CopyEntity() noexcept;
// The copy is put 20 m below its place while it overlaps the watcher's own vehicle of its kind (vehicle_overlap_hide).
bool CopyTucked() noexcept;

// ---- for the vehicle sounds (partner_sound, vehicle_sound; docs/real-partner-body.md section 22.9)
// Sender: the drivable vehicle class of an entity (0 roadster, 1 truck, 2 trike; -1 none or the game checks failed;
// fault-free reads only). The vehicle we report this send tick (seated or parked; 0 none), and a world point in its
// frame (x right, y forward, z up; fault-free reads of it, false when it is no longer the one we report).
int VehicleClass(std::uintptr_t e) noexcept;
std::uintptr_t ReportedVehicle(double now_ms, bool* seated) noexcept;
bool ReportedVehicleLocal(std::uintptr_t e, const double (&p)[3], float (&out)[3]) noexcept;
// Receiver: a point in the partner vehicle's frame to the world, at the place its frames play this tick (with or
// without a copy). False while no frame plays (none for 2 s, the partner silent / held, broken for 3 s, off).
bool PartnerVehicleWorld(double now_ms, const float (&local)[3], double (&out)[3]) noexcept;

// action vehicle_sync: 0 log, 1 mode v[1] (0 off / 1, 2 solid / 3 holograms / 4 roadster), 2 copy override v[1] (0 auto, 1 roadster
// hologram, 2 trike, 3 truck, 4 trike hologram, 5 truck hologram), 3 trace for v[1] ms, 4 copy offset in the vehicle's frame (v[1] forward, v[2] up, cm),
// 5 describe our current / last vehicle, 7 the registered vehicles (and the parking slots), 8 [m] [kind] Sam placed beside
// the nearest, 9 x y Sam placed there, 10 ms our sends held (hidden tests), 11 0/1 overlap hiding off / on, 12 [m] [kind] a
// watching point, 13 the meters now, 14 0/1 looks off / on, 15 0/1 packs off / on, 16 [m] [x shift] / 17 the copies'
// collision test, 19 [kind] the census of the registered vehicles (hidden tests), 21 the copy's collision state, 22 <forward m>
// [right m] / 23 the copy pinned ahead of our own vehicle / free again (hidden tests: driving into it), 24 <n> [model]
// n synthetic cargo pieces on the vehicle we report (hidden tests).
bool DebugCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

}  // namespace st::vsync
