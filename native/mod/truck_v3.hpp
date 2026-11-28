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

#include "truck_v3_rules.hpp"

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
// Round 29 T2: V3 is live and the partner's hello has trucks::kV3FeatSeats (it sends / takes the trucks' occupants).
bool V3PeerSeats() noexcept;
// vehicle_drive's role: 1 host, 2 guest, 0 none.
int V3Role() noexcept;
// Round 29 T2, guest: her follower of the host's truck `truck_id` (vehicle_drive holds her copy): its transform now and the one tick
// lead along its measured velocity (SeatTarget's). False when V3 is not live, this is not the guest, or she does not hold that truck.
// Review: `speed_mps` its measured speed (0 unknown), `corr_ms` its follower's newest correction (a RequestTeleport; -1e18 none).
bool V3FollowedXf(std::uint32_t truck_id, Xf& now, double (&lead)[3], double* speed_mps = nullptr, double* corr_ms = nullptr) noexcept;
// Round 29 T2 review, guest: her copy of the host's truck `truck_id` now (its follower's or vshare's live entity), whatever V3's state
// (a seated mirror given back is moved out of it). False: not known or not live here.
bool V3TruckXf(std::uint32_t truck_id, Xf& now) noexcept;

// ---- round 29 T2 (vehicle_mule_seat, default 1, only with vehicle_mule_v3; docs/enemy-sync.md 14.22): the seated mirrors. Defined
// in truck_v3.cpp; game thread. Lines start "[truckv3] v3 host:" / "[truckv3] v3 guest:" / "[truckv3] v3 seats:".
// Whether this game takes part: the keys on, STO1's handler registered, the actor walk's and the capsule toggle's byte checks
// passed (the hello's kV3FeatSeats).
bool SeatReady() noexcept;
// Host (vehicle_drive V3Tick, every tick): the trucks sent as V3 DRIVE frames now (the host's id and its entity). `send`: V3 live and
// the partner's hello takes occupants; false sends nothing (and forgets what was sent). Every 250 ms the MULEs whose entity's parent
// chain (+0x70) reaches one of them, or whose ride names it, are its occupants; a truck's STO1 goes out on a change (at most every
// 100 ms) and every 1 s; a truck no longer sent gets one empty STO1.
struct OccTruck {
    std::uint32_t id = 0;
    std::uintptr_t entity = 0;
};
void SeatHostTick(const OccTruck* trucks, std::size_t n, bool send, double now_ms) noexcept;
// Host: MULE `mule` is in the newest occupant list of a truck whose STO1 goes to the guest now (its pose slot is ranked first:
// erules::PoseCand::seated). False while occupants are not sent.
bool SeatHostOccupant(std::uint32_t mule) noexcept;
// Host: MULE `mule`'s seat (0..4) in that newest occupant list, else kOccSeatUnknown (round 30 release A: the side library
// learns from the side seats 1..4).
std::uint8_t SeatHostSeat(std::uint32_t mule) noexcept;
// Host: MULE `mule`'s whole entry in that newest occupant list (its seat, its place in the truck's frame and whether that place
// was measured: truckv3::kOccSrcChain). False when it is in none (review logic 1: the side library learns from a seated one).
bool SeatHostEntry(std::uint32_t mule, OccEntry* out) noexcept;
// Guest (enemy_sync, a seated mirror each tick): what T2 knows of MULE `mule`: `in.on` (the key, V3 live, the guest, the partner
// sends occupants), `in.record` (a fresh STO1 names it: its truck, seat, source), `in.followed` (her follower holds that truck: the
// placement `led` and the seat now `exact`). in.host_pose / in.slot are the caller's.
struct SeatFind {
    SeatIn in;
    std::uint32_t truck = 0;
    std::uint8_t seat = kOccSeatUnknown, src = 0;
    Xf led, exact;
    // review: her truck now, its speed and its follower's newest correction (the meter's rule), the seat in its frame (the way out)
    Xf truck_xf;
    double speed_mps = 0, corr_ms = -1e18;
    double lpos[3] = {};
};
void SeatFindFor(std::uint32_t mule, double now_ms, SeatFind& out) noexcept;
// Guest: one seated mirror's decision this tick (the 5 s line's counts, the once-per-change line). `err`: the seat error sample (the
// mirror's place now against the seat now, after last tick's placement; < 0 none); `sanity`: the host-sent place against the seat
// (< 0 none).
void SeatNote(std::uint32_t mule, std::uint32_t truck_hint, const SeatFind& f, SeatWhy why, double err, double sanity, double now_ms) noexcept;
// Guest: the mirror's seat episode ended (its verdict is no longer seated): its next one logs its line again.
void SeatEnd(std::uint32_t mule) noexcept;
// Guest: how many seated mirrors were shown in her copy of `truck` at the latest tick (the shove detector's condition).
unsigned SeatShownIn(std::uint32_t truck) noexcept;
// Guest: one more truck-shove alarm (vehicle_drive's line); its number.
unsigned SeatShoveAlarm() noexcept;
// Guest: a shown seated mirror's capsule (CapsuleComponent) out of the physics world (++ its disable counter, the game's own
// Capsule::Disable, byte-checked; the component and the count after for the caller's line) / back (validated: the same component of the same entity, its counter still raised, then
// Capsule::Enable). False: not done (the checks, no capsule, a fault); the caller logs.
bool SeatCapsuleOff(std::uintptr_t entity, std::uintptr_t* comp, std::int32_t* counter) noexcept;
bool SeatNoCapsule(std::uintptr_t entity) noexcept;  // no CapsuleComponent at all (nothing to take out of the world)
void SeatCapsuleWhy(std::uintptr_t entity, char* buf, std::size_t n) noexcept;  // why it refused (a log line)  // counter: its disable count after
bool SeatCapsuleOn(std::uintptr_t entity, std::uintptr_t comp) noexcept;
// Guest: a shown seated mirror's other physics off (docs/enemy-sync.md 14.22; group kGBody): (A) its art-part bodies moved to
// collision layer 0x7F by the game's own per-body switch (the physics half of SetHidden(1): PartBody_DisableCollision 0x141C3DB90,
// which saves the filter at body+0xE0), (B) its ragdoll component's layer (+0x88) set to 0x7F and a live ragdoll instance released
// once through the component's own interface (the next one is acquired on 0x7F). Every tick while shown (after any SetHidden(0));
// undone (SeatBodiesOn) before any SetHidden(1) and when it is given back; dropped without calls when its entity changes.
struct SeatBodyRec {
    std::uintptr_t body = 0;
    std::int32_t part = 0;
    bool born_on = false;  // part +0xF0 bit 4: collision on at creation (restored; the others stay off as the game made them)
};
struct SeatBodies {
    static constexpr unsigned kMax = 96;
    std::uintptr_t entity = 0;
    SeatBodyRec rec[kMax];
    unsigned n = 0, overflow = 0;
    std::uintptr_t rag = 0;            // its ragdoll component (0 none found)
    std::uint32_t rag_layer = 0;       // its layer before ours
    bool rag_saved = false;
    unsigned released = 0, faults = 0;
};
bool SeatBodiesReady() noexcept;
// Guest: a shown seated mirror's mover (entity +0xB0, its +0x48 the entity) on / off through its own SetActive (vt +0x60, the slot
// the game's Entity Wake / Sleep call): HumanoidMover, AIMover or LocomotionMover's (byte-checked, group kGBody), or any object
// whose RTTI derives from Mover (a MULE's DSGroundMovementComponent; its slot is not byte-checked). A sleeping entity is never
// drawn where it is moved (the scheduler skips its model and animation update: run 20261127-114906, her truck looked empty), so a
// shown seated mirror is woken and only its mover switched off (no locomotion between placements); on again at the give-back.
// False: byte checks, no mover, the mover's entity is another, not a Mover, or a fault. `why`: the mover's class on success, else
// the reason.
bool SeatMoverSet(std::uintptr_t entity, bool on, const char** why = nullptr) noexcept;
const char* SeatMoverClass(std::uintptr_t entity) noexcept;  // its RTTI name, or "?"
// The passes (`mode` bit 0 A, bit 1 B; SeatBodiesMode()); the bodies switched off by this call.
unsigned SeatBodiesOff(std::uintptr_t entity, SeatBodies& s) noexcept;
// The undo (only the same entity; then cleared): the ragdoll's layer always (SetHidden never touches it); the art parts only with
// `parts` (the actor shown: restoring them on a hidden actor would make it solid; SetHidden(0)'s snapshot keeps ours off).
void SeatBodiesOn(std::uintptr_t entity, SeatBodies& s, bool parts) noexcept;
void SeatBodiesProbe(std::uintptr_t entity, const SeatBodies& s, char* buf, std::size_t n) noexcept;  // the probe line's text

// Bootstrap thread (dllmain, after MinHook is initialised): the byte checks and, when they pass, the get-off detour (the probe's
// only). `seat_on` (vehicle_mule_v3 and vehicle_mule_seat): the byte checks, STO1's handler (SVM1 type 52). Nothing when both are
// false.
void Install(bool probe_on, bool seat_on) noexcept;
// player_sync, every frame (game thread): drains the get-off ring into the log and runs the watch; T2's guest meters (5 s line).
void Tick(double now_ms) noexcept;
// truck_v3 <op> [a] (debug action, game thread): 0 the watch off + the status line; 1 [period s] the watch on (default 1 s);
// 3 the V3 status line (round 28 T1; any probe key); 4 the T2 seat status line (round 29; any probe key); 8 the seat dump; 9 the
// identity dump; 10 (round 30 stage 0, P2) the seated MULEs' frames ("[mrec] conv ..." and "[mrec] skeleton: ..."). Every vehicle
// and every MULE of this game.
bool Command(const double (&v)[3], char* msg, std::size_t cap) noexcept;

// ---- round 30 stage 0 (side riding; docs/real-partner-body.md 24.19). Game thread. Lines start "[mrec]". Read-only in the game:
// the recorder slot is realbody's (kRecorderSlot 8, never the SEP1 slots); the helper lookup is ride_together's (ride::TruckHelper).
// A truck of the pools by its unit id / the nearest one within max_m of pos (the actor walk's byte checks; false: none, or off).
bool TruckById(std::uint32_t id, std::uintptr_t* entity) noexcept;
bool NearestTruck(const double (&pos)[3], double max_m, std::uintptr_t* entity, std::uint32_t* id) noexcept;
// mule_rec 0|1 (debug action; hidden runs or dev_tools): the MULE clip recorder (P4). On: every 100 ms the MULEs' ride bits
// (+0xE44) are watched; the rising edge of bit 2 starts a board (bit 0 newly set, or +0xE20 names a vehicle) or an alight (bit 0
// already set) of one MULE at a time: its 77 local joints, root and pelvis translations every animate (the recorder slot's ring)
// and its entity in the truck's frame every game tick, up to 6 s; then validated and logged ("[mrec] board seat N: ..."). The clips
// stay in memory (a board's seated end per seat: the ghost's recorded sample). Off: a clip being recorded is dropped.
bool RecCommand(bool on, char* msg, std::size_t cap) noexcept;
// ride_test 43 (P3): source 4's sample for side seat `seat` (1..4): 1 live (a MULE seated there now: the recorder slot on it),
// 2 the newest validated board clip's seated end for that seat, 0 none (SetRideSample(nullptr): the seated idle). `mule`: its id.
// GhostStop gives the slot back and clears the sample.
int GhostStart(int seat, std::uint32_t* mule) noexcept;
void GhostStop() noexcept;
// The ghost's error: our Sam's model joints in our truck's frame against the reference (the live MULE's in its truck's frame, or
// the recorded seated end's), fingers left out; and our Sam's lowest foot z in our truck's frame. False: nothing compared.
struct GhostErr {
    unsigned n = 0;
    double mean_cm = 0, max_cm = 0;
    int max_j = -1;
    bool feet = false;
    double feet_z = 0;
    int kind = 0;  // GhostStart's
    std::uint32_t mule = 0;
};
bool GhostCompare(std::uintptr_t sam_entity, std::uintptr_t truck_entity, GhostErr& out) noexcept;
// The mod's emergency stop (any thread): the watch off and the detour's records dropped (it passes through).
void EmergencyStop() noexcept;

}  // namespace st::truckv3
