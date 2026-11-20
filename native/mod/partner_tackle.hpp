// Vehicle hits (design milestone M4; docs/real-partner-body.md section 25). Runs with the real shared vehicles
// (vehicle_sync=5 with host_save=1: vehicle_share + vehicle_drive) and session.ini partner_vehicle_damage (default 1).
//
// "When the vehicle collides with players it should damage them and knock them down like an NPC enemy driven vehicle";
// "they can collide with players and other player vehicles too".
//
//   the rule (M4.2)   the victim's game decides the hit on its own Sam. On its screen the partner-driven vehicle is our
//                     follower (the game's NPC controller, driver 0x1FFFFF), which the game's VehicleTackleEmit 0x142F8ABF0
//                     never lets hit anything (no driver, ride state 2). PartnerTackle runs the game's own vehicle attack for
//                     it on the game thread while: our Sam is on foot and awake, not carried as a passenger, no load / menu /
//                     cutscene / sequence (LocalCollisionBlock), within 50 m, the follower's speed field [[V+0x330]+0xF0]
//                     over 15 km/h, the partner's frames say it holds the driver seat, our Sam near the vehicle's attack
//                     sphere, partner_vehicle_damage=1 (D4-4).
//   the hit (D4-1)    trucks: the MULE truck attack VehicleTackle 0x157 (slot +0x9E0; 100 damage, reaction Blow) through the
//                     game's attack sphere query 0x1429934E0 with the vehicle's own sphere ([V+0xA70], radius [V+0x57C]), the
//                     game's filter (its own bodies left out) and our callback that keeps only hits within 1.5 m of our Sam
//                     (D4-3: the partner's vehicle hits only you and the cargo on you). Trike and roadster (no 0x157 slot):
//                     the same direct path with VehicleTacklePlayer 0x158 (slot +0xA00) and the same callback. Only when the
//                     direct path's checks failed: the proven state-4 bracket (ride state +0x4CC = 4 for one call of
//                     0x142F8ABF0, restored; 0x158; the game's sphere hits whatever it catches, so only while our Sam's
//                     centre is inside it). Never a call through an empty attack slot. tackle_rules.hpp PathFor.
//   the cap (D4-2)    no instant kill: the direct path passes amount 100 at any speed; the bracket holds the speed field at
//                     29.5 km/h for the call above 30 (the game passes 10000 there); and Sam's damage handler (0x14271BC00,
//                     detoured at start-up, everything passes) caps a message of the attack we just used at 100.
//   rate              calls every tick until Sam's damage handler sees a damaging message of that attack (or his health
//                     drops), then nothing from that vehicle for 1.5 s. One "[vhit] partner vehicle ... hit Sam" line per hit.
//   the driver        never tackles the partner body (it is the partner's game that decides); it sees the knock-down through
//                     the pose stream. The victim sends STH1 (channel 3, type 49) for a log line on the driver's game.
//   crash (D3-3)      riding together's crash exit (ride::SetCrashHitHandler): once the passenger is put beside the vehicle,
//                     the same attack on him (direct path, 0x157 or 0x158, a 1 m sphere at his side toward the vehicle), with
//                     the same game-state gates (LocalCollisionBlock with the sequence, the game's attack stop).
//   capsule (M4.4)    the partner capsule (partner_collision) is out of the world while a vehicle moving over 1 m/s is within
//                     5 m of the partner body (speeds from the drive module's polls; moving until 0.6 m/s), and does not
//                     come back while a vehicle's box overlaps it (CapsuleVehicleBlock).
//
// Every game address is in the start-up byte-check table (kSites in partner_tackle.cpp); a mismatch turns the hits off with a
// one-line reason in the log and the co-op banner (or only the path it belongs to). Game calls run on the game thread under
// SEH; nothing of the game's is freed or evicted (the filter's own list is released the way the game releases it).
#pragma once
#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "game_api.hpp"

namespace st::vhit {

// Bootstrap thread, after ride::Install and before the frame pump is hooked: the byte checks, the damage-handler detour, the
// channel 3 handler, the crash handler.
void Install(const Config& config) noexcept;
// The hits are on (checks, the shared vehicles with remote driving, partner_vehicle_damage=1).
bool Active() noexcept;
const char* OffReason() noexcept;

struct TickIn {
    game::Entity* local = nullptr;
    bool local_ok = false;     // a transform this tick
    bool local_active = false; // awake
};
// Game thread, every tick, after ride::Tick (and before the partner capsule's tick).
void Tick(const TickIn& in, double now_ms) noexcept;
// The per-frame tick faulted: no more calls (any thread).
void EmergencyStop() noexcept;

// The partner capsule (partner_collision.cpp, M4.4): why it must stay out of the world because of a vehicle, or nullptr
// (also when the shared vehicles are off). `enabling`: it would go back into the world now. Game thread.
const char* CapsuleVehicleBlock(const double (&body)[3], bool enabling) noexcept;

// Debug action "Vehicle hit" (debug_actions.cpp; game thread): 0 state and counters; 1 <kind> <0 auto | 1 direct | 2 bracket>
// the path a kind uses (tests); 2 one hit on our Sam now from the vehicle the partner drives here (the crash sphere, the
// direct path; tests); 8 <m ahead> [m right] our Sam that far in front of the vehicle the partner drives here, facing it
// (hidden tests); 9 the vehicles the partner capsule rule sees; 10 <kind> <m> a free vehicle of that kind put m in front of the
// vehicle the partner drives here, facing it (negative m: broadside; hidden tests, the owner's game); 11 the life of every
// shared vehicle within 300 m.
bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

}  // namespace st::vhit
