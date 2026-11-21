// Shared real vehicles, stage 2: remote driving, ownership and state (design milestone M2.6 - M2.9; docs/real-partner-body.md
// section 23.11 - 23.17). Runs only while vehicle_share's real sync is on (vehicle_sync=5 with host_save=1).
//
// One real vehicle per vehicle in both worlds; the game whose Sam holds the driver seat OWNS it, and when nobody drives it the
// host owns it (the host's world is the persisted one). The other world FOLLOWS:
//
//   driving (M2.6)   the driver's game sends STD1 DRIVE frames (30 Hz, the pose send tick, the pose's sender_ms axis): the
//                    48 input bytes the game feeds the vehicle ([vehicle+0x528] +0x08..+0x37), its transform, velocities,
//                    speed, life, battery and state bits. The follower drives its own unit with the game's NPC driver
//                    controller (ctor 0x142F51C60, Attach 0x142F5A590 with driver 0x1FFFFF, SetEngine 0x142F5A310; the
//                    proven recipe), whose input object gets the mod's copy of vtable 0x143E229B8: slot +0x08 calls the
//                    game's 0x142F54440 and then copies the received 48 bytes over +0x08..+0x37 (the vehicle resets them on
//                    every update). Inputs and the target transform play back at the partner body's play time
//                    (interp_delay_ms), so the follower and the seated partner body stay together.
//   correction       error bands against the frames at the play time: under 0.1 m (parked) / 0.3 m (driven: the input
//                    replay alone stays within ~0.1 m) and 3 deg nothing; < 2 m and < 45 deg, parked: the game's own
//                    remote-vehicle net correction 0x1423B80E0 (its velocity drive; the nudge when it is off or does not
//                    close the error), driven: a clamped velocity nudge through the game's drive-to-transform 0x141C43E10
//                    (the owner's velocity plus at most 2 m/s, every 100 ms); beyond: RequestTeleport 0x142F7CA20, only
//                    where the vehicle fits (its footprint holds neither the local Sam nor another vehicle). The native
//                    2..5 m branch did not move the vehicle in a hidden run, so it is not used. Never keyframed.
//   v0.21 (driven)   play test 3: the owner sends its chassis body's physics velocities (kDrvPhysVel); the follower plays the
//                    inputs of the frame nearest to the play time, targets a Hermite curve between the frames (dead
//                    reckoning up to 250 ms past the newest), and every tick steers the body's horizontal velocity and yaw
//                    rate toward the owner's (vwire::FollowVelocity, SetLinearAndAngularVelocity 0x141C34330); vertical,
//                    pitch and roll stay the game's. RequestTeleport only by vwire::DriveTeleport (over 4 m / 30 deg; 8 m /
//                    60 deg while our Sam rides it; stuck; the owner stopped and ours apart), each logged with its reason. Section 23.25.
//   parked           the owner sends STD1 PARK frames for every vehicle it owns that nobody drives, within 300 m of either
//                    player: 1 Hz, at once on a change of 5 cm or 2 deg. The other world corrects with the same bands
//                    (no controller).
//   ownership (M2.7) VOWN on channel 3 (vehicle_wire.hpp): the guest's get-on sends CLAIM, the host GRANTs or DENYs; the
//                    host wins every tie (its Sam on it), and the guest's Sam is put off with the game's own
//                    RideOffVehicleForcibly 0x1426E7100. The follower controller holds the seat, so the local get-on fails
//                    (the lock). The owner RELEASEs when its Sam gets off and again when the vehicle is at rest (< 0.2 m/s
//                    for 1 s): then it is the host's again. A parked vehicle the non-owner pushes more than 0.1 m triggers a
//                    CLAIM (the host grants it); until then the correction pulls it back.
//   state (M2.8)     life (+0x6E8 / +0x6EC) and the battery field (+0x5A4, probed at start against the ride-state machine's
//                    read) are the owner's: the follower overwrites them; the broken (+0x560 & 0x2000) and exploded
//                    (+0xBD8 & 0x100000, requested through +0x564 bit 21) edges are copied once, so the follower's own
//                    state machine plays the effects (D2-5: the driver's game decides the damage).
//
// The partner body sits at our real vehicle's transform times the STM1 seat while the partner drives (SeatTarget). Every
// game address is in a start-up byte-check table (kSites in vehicle_drive.cpp); a failed required group turns remote driving
// off with a one-line reason (log and co-op banner) and the vehicles stay shared without it. Game calls run on the game
// thread under SEH; the controller blocks are the mod's own memory; nothing of the game's is freed.
#pragma once
#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "game_api.hpp"
#include "vehicle_wire.hpp"

namespace st::vdrive {

// Bootstrap thread, after vshare::Install: the byte checks, the channel 3 handlers and vehicle_share's queries.
void Install(const Config& config) noexcept;
// Remote driving is on (vehicle_share on and every required check passed).
bool Active() noexcept;
const char* OffReason() noexcept;

// ---- sender (the pose send tick, game thread): the frames to send this tick (DRIVE for the vehicle our Sam drives, PARK
// for the vehicles we own). Each `out[i]` holds one STD1 frame of `sizes[i]` bytes. Returns how many.
unsigned TakeSend(game::Entity* local, std::uint32_t sender_ms, double now_ms, std::uint8_t (*out)[vwire::kMaxDriveFrameSize], std::size_t* sizes,
                  unsigned max) noexcept;
// ---- receiver: a decoded STD1 frame, keyed on the pose ring's unwrapped sender axis.
void OnFrame(const vwire::DriveFrame& f, double key_ms, double now_ms) noexcept;

struct TickIn {
    game::Entity* local = nullptr;   // the local Sam (read only)
    bool local_ok = false;           // awake, with a transform
    bool have_play = false;          // the pose ring gave a play time this tick
    double play_ms = 0;              // the partner's play time (sender axis), the body's
    bool have_partner = false;       // the partner body's target this tick
    game::WorldTransform partner{};
};
// Game thread, every tick, after vshare::Tick: ownership, followers (attach / inputs / correction / detach), state copy.
void Tick(const TickIn& in, double now_ms) noexcept;
// A new link session: every vehicle is the host's again, followers detach.
void NewSession() noexcept;
// The per-frame tick faulted: the inputs stop now; nothing more is sent. The controllers let go on the next game-thread pass
// (AfterStop: vdrive::Tick, or dllmain's frame pump after a whole-mod stop), once, guarded.
void EmergencyStop() noexcept;
// Game thread, after an emergency stop: every follower still attached is detached (the live Detach with the host's Touch and
// the NPC bits cleared), once. No-op otherwise.
void AfterStop() noexcept;

// The partner body's place while the partner drives one of our real vehicles here: that vehicle's transform now times the
// partner's seat (the STM1 frame at `play_ms`, vsync::PartnerSeatAt). False when the partner does not drive a vehicle we
// follow (the body goes where its own frames say).
bool SeatTarget(double play_ms, game::WorldTransform* target) noexcept;
// The vehicle the partner drives here (a follower we run): its entity, false otherwise.
bool PartnerDrivenEntity(std::uintptr_t* entity) noexcept;

// Riding together (ride_together.cpp, M3): the vehicle our Sam drives (LocalDriving), or the vehicle the partner drives here
// through our follower (PartnerDriving). Game thread, after vdrive::Tick.
struct DrivenView {
    vwire::VKey key;
    std::uintptr_t entity = 0, unit = 0;
    bool meas = false;              // the vehicle's velocity measured here in the last 200 ms
    float vel[3] = {};
    double speed_mps = 0;
    bool have_frame = false;        // the partner's newest DRIVE frame (followers)
    double frame_key = 0;           // its key (the pose ring's sender axis)
    double frame_rx_age_ms = 1e9;   // since one arrived
    float frame_vel[3] = {};
    bool frame_seated = false, frame_broken = false;
    std::uint8_t frame_ride_state = 0;
};
bool LocalDriving(DrivenView* out) noexcept;
bool PartnerDriving(DrivenView* out) noexcept;
// The partner's DRIVE frame at or before `play_ms` for that vehicle: its velocity (v0.21: the chassis body's, kDrvPhysVel),
// key and the game's speed field (km/h; < 0 unknown).
bool FrameAtPlay(std::uint32_t host_id, double play_ms, float (&vel)[3], double* key_ms, float* speed_kmh = nullptr) noexcept;
// Our RequestTeleport of the follower `entity` within the last `within_ms`: its target (the vehicle is there once the
// game applies it) and age. ride_together puts a carried Sam on that seat and ignores crash / hit signs right after it.
bool RecentTeleport(std::uintptr_t entity, double within_ms, game::WorldTransform* target, double* age_ms) noexcept;
// Our Sam is carried as the passenger of the vehicle with this unit (0: none): the teleport clearance leaves him out for it.
void SetCarriedUnit(std::uintptr_t unit) noexcept;
// Hidden tests: the vehicle the partner drives here (its follower), else the one whose DRIVE frames from the partner came
// last (the one it just drove, even after the handback). Game thread.
bool PartnerVehicleEntity(std::uintptr_t* entity) noexcept;

// session.ini vehicle_real_native (default 0 until a hidden run proves the native ones): 1 = the real follower vehicle's
// lamps and engine / road sounds are the game's own only (the v0.14 lamp writes and the STA1 vehicle-sound mirror off).
bool NativeFx() noexcept;

// Debug action "Vehicle drive": 0 status, 1 the vehicles' ownership and followers, 2 <0/1> native correction off / on,
// 3 <parked m> <driven m> the ignore bands (0 = 0.1 / 0.3), 4 <0/1> corrections off / on (hidden tests: the open-loop
// replay), 5 <0/1> native fx, 6 <kind> [m] (hidden tests) push the nearest vehicle of that kind sideways by RequestTeleport
// (a "push"), 7 <life> (hidden tests) set the life of the vehicle our Sam drives or stands nearest (the owner's damage),
// 8 the battery probe, 9 [m] (hidden tests) our Sam m (0 = 9) beside the vehicle the partner drives here, facing it;
// v0.21: 10 <dv> (tests) the next DRIVE frame's velocity x off by dv, 11 <fwd> <right> (tests) the follower's target
// moved for 1.5 s (step response, logged per tick), 12 <m/s> <laps> (tests) the owner's autopilot on play test 3's road
// (0 off), 13 <0/1> v0.20's follower (before / after runs), 14 <place> (tests) the vehicle put at 1 the road's start, 2 the
// downhill's top, 3 its bottom, 15 <ms> the input lead, 16 <kp> <ki>, 17 <kyaw> <tau>, 18 <max dv> <max corr> the gains.
bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

}  // namespace st::vdrive
