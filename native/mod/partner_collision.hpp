// Player collision (docs/real-partner-body.md section 11): the local player cannot walk through the
// partner's real body.
//
// The game's own character collision: Humanoid::Init (0x14373A780) gives Sam a CapsuleComponent at
// run time (NewGuid, CreateComponent, AddComponent, Configure). The same sequence gives the
// partner's real body (a spawned cutscene-actor DSControlledEntity, real_body.hpp) a capsule of its
// own, with Sam's PhysicsCharacterMoverResource (radius 0.40 m, height 1.65 m), body type 0 (a Havok
// character rigid body, what the game gives remote and AI humanoids) on collision layer 10
// "Humanoid blocker" (it meets only the Player and AI/remote layers and vehicles: no cargo, bullets,
// camera or static; partner_collision_layer=9 picks the game's "AI or Remote Player" layer). Every
// tick the capsule is put at the body's transform with zero velocity, exactly HumanoidMover's reset
// path (Capsule SetTransform, SetVelocity). Sam's own capsule proxy is then blocked by it like by any
// other character.
//
// Off (the capsule out of the world, Capsule::Disable, balanced with Enable) while the local Sam is
// not on foot in the played world (game_actions LocalCollisionBlock: asleep, loads, menus,
// cutscenes, vehicles, sequences, ...), Sam's own capsule is not in the world (ladders, ziplines,
// ...), the partner is hidden, stale, fast (> 8 m/s: a vehicle) or jumped, and while the two
// overlap (it comes back only once they are 0.9 m apart, so Havok never pushes them apart). It is
// in the world only while the partner is within 4 m of the local Sam (out beyond 4.5 m) and not in
// a vehicle by its own pose flag: away from Sam it could only meet our own vehicles or AI.
// Never on the self-view body or the hologram: player_sync passes only the partner slot's real body.
// The component belongs to the body entity: it goes when the body is removed.
//
// Game thread only. Every game function is byte-checked first; every read is fault-free.
#pragma once
#include <cstddef>

#include "config.hpp"
#include "game_api.hpp"

namespace st::collision {

void Init(const Config& config) noexcept;
// Every tick, after the partner body was moved. body = the live real partner body or nullptr;
// partner_in_vehicle = the partner's newest pose says it rides a vehicle (wire kPoseInVehicle);
// remote_age_ms = time since the newest remote sample arrived.
void Tick(game::Entity* local, bool local_active, game::Entity* body, bool partner_in_vehicle, double remote_age_ms,
          double now_ms) noexcept;
// The real partner body is about to be torn down (still live): its capsule leaves the world first,
// so a body the teardown cannot remove leaves no invisible wall behind. Game thread.
void BodyGoing(game::Entity* body) noexcept;
// The per-frame sync faulted and will not run again: take the capsule out of the world.
void EmergencyStop() noexcept;

}  // namespace st::collision
