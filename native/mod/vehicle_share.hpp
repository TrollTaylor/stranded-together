// Shared real vehicles (design milestone M2, stage 1: identity, spawn, removal, eviction; docs/real-partner-body.md
// section 23). session.ini vehicle_sync=5 ("real"), the default with host_save=1 when every byte check below passes;
// otherwise the v0.14 solid trike / truck copies of vehicle_sync.cpp stand in (and no roadster: decision D2-1).
//
// "There is only one copy of each vehicle." Each vehicle is ONE real game vehicle in each world, bound by an identity:
//
//   identity (M2.2)   the host's game-actor id is canonical (vehicle_wire.hpp VKey). After every guest load the guest asks
//                     for the host's table (SNAP_REQ); the host sends it in chunks (SNAP, channel 3). The guest binds each
//                     host vehicle to its own unit with the same placement GUID, else the same id within 5 m; a wrong unit
//                     under that id is re-placed onto the host's spot (RequestTeleport) or removed (unit+0x198 |= 0x2000,
//                     the game's own removal request); guest units the host does not have are removed once nobody rides
//                     them.
//   spawns (M2.3)     the guest's world never originates a vehicle: a detour on the pool enqueue 0x142F654E0 drops every
//                     request the mod did not make (outside the guest's load window, where the save's own spawns replay as on
//                     the host) and forwards it (REQ; the garage slot when it came from 0x142D5CB20); the overlay shows
//                     "Summoning...". The host spawns it (the garage slot spawn 0x142D5CB20, else 0x142F830A0) and announces
//                     it (ADD, with the game's own persisted entry for it); its own spawns are seen by the pool poll and
//                     announced the same way. The guest mirrors it: the persistence loader's exact-id path (pool vfunc +0x30,
//                     0x142DCFFB0) first, else 0x142F830A0 with p6 = 0 and an id mapping, else a free unbound unit re-placed.
//   removals (M2.4)   the host's removals reach the guest (REM) and are applied there with 0x2000. A guest removal of a bound
//                     vehicle goes to the host, which applies it only when the guest owns the vehicle (the drive module's
//                     owner query) and its guard (id / GUID / position) agrees; otherwise the host announces it again and the
//                     guest mirrors it again.
//   eviction (M2.5)   the guest's victim chooser 0x142F7DB10 returns 0 (it never evicts); the host's returns 0 for a
//                     shared-active unit (the partner rides it, was within 150 m in the last 60 s, or drove it in the last
//                     10 min). Online vehicles of other players (request flag 0x800) are dropped on both games (D2-3).
//
// Every game address is in a start-up byte-check table (kSites in vehicle_share.cpp); a mismatch turns the real sync off
// with a one-line reason in the log and on the overlay. Game calls run on the game thread under SEH; the detours only
// refuse or pass. The mod never frees or evicts a unit itself: removals are the game's own request bit, on the guest's
// disposable world, and on the host only through the guarded owner path.
#pragma once
#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "game_api.hpp"
#include "vehicle_wire.hpp"

namespace st::vshare {

// Bootstrap thread, after MH_Initialize and before the frame pump is hooked (the guest's gate must be in before any world
// loads): decides the mode (host_save=1, role host or guest, vehicle_sync=5 or unset), runs the byte checks and installs
// the detours. No-op otherwise.
void Install(const Config& config) noexcept;
// The real shared vehicles are on (vehicle_sync=5 in effect). False: the v0.14 copies stand in.
bool Active() noexcept;
// Why they are off ("" when on).
const char* OffReason() noexcept;

struct TickIn {
    game::Entity* local = nullptr;       // the local Sam (read only)
    bool local_ok = false;               // awake, with a transform
    bool have_partner = false;           // the partner body's target this tick
    game::WorldTransform partner{};
};
// Game thread, every tick (sync::Tick): the channel 3 messages, the pool poll, the binding, the spawns and removals.
void Tick(const TickIn& in, double now_ms) noexcept;
// The per-frame tick faulted: the detours pass everything through from now on (the game's own behaviour), nothing more is
// sent. Any thread; idempotent.
void EmergencyStop() noexcept;

// Debug actions (debug_actions.cpp; game thread):
//   "Vehicle share" op: 0 status, 1 dump [tag], 2 summon <kind> [m], 3 snapshot now, 4 binding report, 5 <0..3> mirror
//   path for tests (0 auto, 1 exact-id only, 2 fallback only, 3 re-place only), 6 <kind> [m] hidden tests: remove the
//   nearest vehicle of that kind (0x2000), 7 eviction / gate counters.
//   "Vehicle dump" [tag]: every in-use unit of the three pools, with its binding.
//   "Vehicle summon" <kind 1 roadster | 2 truck | 3 trike> [m ahead, default 8]: through the game's own request path (a
//   garage slot of that kind, else 0x142F830A0 with a record copied from a unit of that kind), so the guest's gate is
//   exercised.
bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;
bool DumpCommand(const double (&v)[3], char* msg, std::size_t cap) noexcept;
bool SummonCommand(const double (&v)[3], char* msg, std::size_t cap) noexcept;

// ---------------------------------------------------------------- API for the drive module (vehicle_drive.cpp, stage 2)
// A vehicle of this world by its identity. Game thread. Fault-free reads of the newest pool poll (10 Hz) plus a fresh
// check of the unit's id and in-use byte.
struct Unit {
    std::uintptr_t unit = 0;      // GameActorUnit*
    std::uintptr_t entity = 0;    // the vehicle entity (DSVehicle*), 0 while its actor is not spawned
    std::uint32_t local_id = 0;   // this world's id (the guest's may differ from the host's after a fallback mirror)
    vwire::VKey key;              // the shared identity (the host's id)
    bool bound = false;           // host: always; guest: bound to the host's vehicle (not only same-id)
    bool has_xf = false;
    game::WorldTransform xf{};
    std::uint32_t rider = vwire::kNoRider;  // unit+0x158
};
// By the shared key (host: the key's id is ours; guest: through the binding). False when this world has no such vehicle.
bool FindByKey(const vwire::VKey& key, Unit* out) noexcept;
bool FindByHostId(std::uint32_t host_id, Unit* out) noexcept;
// By the vehicle entity (entity+0x8A8 = its unit). False for an entity that is not a pooled vehicle of this world, or (guest)
// one that is not bound yet.
bool FindByEntity(std::uintptr_t vehicle_entity, Unit* out) noexcept;
// The guest has applied the host's table at least once since its last load (host: true while its world is ready).
bool Bound() noexcept;

// Channel 3 (vehicle_wire.hpp, "SVM1"): types 16..63 are handed to the handler registered for them (game thread, in order,
// with the link session's generation). Returns false for a type outside 16..63 or one already taken.
using Handler = void (*)(std::uint8_t type, const std::uint8_t* msg, std::size_t size, std::uint32_t generation, double now_ms);
bool RegisterHandler(std::uint8_t type, Handler handler) noexcept;
// Queues one message (a full SVM1 message: vwire::WriteHeader + body) behind everything vehicle_share sends, in order.
// Game thread. False when it does not validate (header, size) or the outbox is full (256 messages).
bool SendEvent(const std::uint8_t* msg, std::size_t size) noexcept;

// The drive module's view, asked by the rules above (game thread). Defaults: nobody drives remotely, the host owns all.
//   guest_owns(key): the guest's game owns the vehicle now (its Sam drives it): the host applies the guest's removal of it.
//   followed(host_id): the drive module moves this vehicle in this world (a follower): the binding never re-places it.
using KeyQuery = bool (*)(const vwire::VKey& key);
using IdQuery = bool (*)(std::uint32_t host_id);
void SetGuestOwnsQuery(KeyQuery q) noexcept;
void SetFollowedQuery(IdQuery q) noexcept;
// The partner drove this vehicle now (the host's eviction refusal keeps it for 10 min).
void NotePartnerDrove(const vwire::VKey& key, double now_ms) noexcept;

// The game's own deferred vehicle teleport 0x142F7CA20 (byte-checked; game thread, SEH). False when off or it faulted.
bool RequestTeleport(std::uintptr_t vehicle_entity, const game::WorldTransform& to) noexcept;

// ---------------------------------------------------------------- stage 2 additions (vehicle_drive.cpp)
// Every shared vehicle of this world from the newest poll (host: every live unit; guest: every bound live unit, keyed by the
// host's id). Game thread. Returns how many were written (at most cap).
std::size_t SharedUnits(Unit* out, std::size_t cap) noexcept;
// The shared vehicle of that kind nearest to `pos` within `radius_m` (the partner's STM1 frames name no instance).
bool FindNear(std::uint8_t kind, const double (&pos)[3], double radius_m, Unit* out) noexcept;
// The footprints of every live vehicle with a place except the one whose unit is `except_unit` (teleport clearance).
// `host_ids` (optional, cap entries): the host's id of each (host: its own id; guest: the bound host id, 0 if unbound).
std::size_t Footprints(std::uintptr_t except_unit, vwire::Footprint* out, std::size_t cap, std::uint32_t* host_ids = nullptr) noexcept;
// Guest: the host's vehicle `host_id` is now at `xf` (its owner's frames, or the guest's own while it owns it): the table
// entry the binding compares with moves too, so a vehicle moved by driving is never re-placed onto an old spot. Host: no-op.
void NoteHostPlace(std::uint32_t host_id, const game::WorldTransform& xf) noexcept;
// The world counter and readiness vehicle_share runs on (coop::WorldReady), and the link session it last saw.
bool WorldReadyNow(std::uint32_t* counter) noexcept;
// 1 host, 2 guest (0 off).
int Role() noexcept;

// ---------------------------------------------------------------- review fixes (game thread)
// The partner is in this world: host: the guest's last SNAP_REQ named our current world (coop's load counter; the guest's
// loaded save is the host's offer of that counter); guest: the host's newest snapshot or VOWN / REM named ours. Messages
// that change a vehicle (CLAIM, RELEASE, REM, STD1 frames) are only taken while this holds.
bool PeerInOurWorld() noexcept;
// Guest: a host message named world `w` (the host's current one).
void NotePeerWorld(std::uint32_t w) noexcept;
// The real sharing is in effect with the partner: on, and (guest) the host's game answered with a snapshot in this world
// and has not gone silent. False: vehicle_sync shows the v0.14 copies (D2-1) and the guest's gate passes its own spawns.
bool RealInEffect() noexcept;
// The partner body's place this tick (vehicle_share's TickIn), false without one.
bool PartnerPlace(double (&pos)[3]) noexcept;
// The unit is still that vehicle with that actor, read now: its entity is a vehicle of its kind whose unit is it, the unit's
// entity reference is it, the unit in use with its id.
bool UnitLive(const Unit& u) noexcept;
// A RequestTeleport target `xf` for a vehicle of `kind` is clear: neither the local Sam (0.6 m) nor another live vehicle
// (except the unit `except_unit`) in its footprint.
bool PlaceClear(std::uintptr_t except_unit, std::uint8_t kind, const game::WorldTransform& xf) noexcept;
// Host: the place a vehicle of ours has in the partner's world while its actor is not spawned here (the guest parked it out
// of our streaming range): the snapshot and ADDs carry it instead of the unit's old own place until it is cleared.
void SetPlaceOverride(std::uint32_t id, const game::WorldTransform& xf) noexcept;
void ClearPlaceOverride(std::uint32_t id) noexcept;

}  // namespace st::vshare
