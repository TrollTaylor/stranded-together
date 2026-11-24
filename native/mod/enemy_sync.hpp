// Enemy sync (docs/enemy-sync.md).
//
// P1a (round 16, section 8; session.ini enemy_sync, see config.hpp): the host owns every enemy. Its tick sends the enemies' places
// and states (SEN1: 10 Hz within enemy_mirror_radius of the guest's place, 2 Hz within 1000 m, 0.2 Hz the rest) and the poses of
// the nearest few it has in its world (SEP1, captured in the animate hook). The guest freezes its own copy of each realized enemy
// near her (MsgGameActorSequenceOccupied{1}; never one a real sequence holds, a hidden or sleeping one, a sequence-only one, nor in
// a cutscene), drives it to the host's place 100 ms behind (the actor's transform setter and the entity's, a teleport above 2 m),
// writes the host's joints onto it, and gives it back (Occupied{0} + Required{0}) when its frames stop, it leaves the radius, it
// leaves her world, or the link, the world or the switch goes. Copies out of her world are placed at the host's places
// (enemy_place_far, variant A of the game's own caller 0x142B5BAE0). A frozen mirror never hurts her (a Sam damage filter on its
// hits), never takes hits here (the Humanoid damage filter: the host decides) and never fires (the bullet Enqueue drops its
// records; the game cancels its trigger requests anyway). The host MULEs' shots are replayed on her screen from the mirrors' guns
// (shot_sync.cpp, enemy_shots), and an enemy round that hits her own Sam hurts her (enemy_shots_hurt: the victim decides).
// Mirroring runs only while both games say so in their SEH1 beacons with equal enemy fingerprints (the actor id is the key).
//
// P1-0 (round 15, section 7.2): the freeze probe, hidden runs only, kept as it was (the probe's pose slot is slot 7):
//   enemy_probe 0 [radius_m=120]   byte checks (group freeze), then pick the nearest realized, live MULE / Demens not held by a
//                                  sequence: "[enemy] probe pick id=..."
//   enemy_probe 3 <secs> <path>    the next tests' length (default 8 s) and path (0 hold its place, 1 a 1 m/s line along its heading)
//   enemy_probe 1 <lever> <mode>   freeze with the lever, drive it with the mode for <secs>, release, watch 15 s, one summary line
//                                    lever: 0 none (control), 1 Occupied{1}, 2 Occupied{1} + Required{1}, 3 Sleep (vt+0x50), 4 Occupied{1} + Sleep
//                                    mode: 0 no placement, 1 per tick the actor's vt+0x10 + Entity_ExportedSetWorldTransform,
//                                          2 per tick the actor teleport 0x1422F8D90, 3 a teleport every 500 ms with mode 1 between
//   enemy_probe 2 <secs>           during a running test: our Sam's live pose written onto its SkinnedModel (the animate hook,
//                                  pose_hook=1), the animate rate and the largest joint error logged
//   enemy_probe 4 [m]              our Sam m (default 10) from the picked MULE, facing it (the topmost ground there)
//   enemy_probe 5 <0|1>            the shield: every damage message on our Sam dropped while on (hidden runs)
//   enemy_probe 9                  release now (Occupied{0}, Required{0}, wake / show what we slept / hid)
#pragma once

#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "game_api.hpp"

namespace st::esync {

// Start-up (dllmain, after shots::Install: the Sam damage filter goes in after the shots' own): the byte checks, the Sam and NPC
// damage filters. Nothing when enemy_sync=0.
void Install(const Config& config) noexcept;

struct TickIn {
    game::Entity* local = nullptr;  // our Sam
    bool local_ok = false;
    game::Entity* body = nullptr;   // the partner body (real or hologram), nullptr when none
    bool net = false;               // a host / guest link
    std::uint8_t role = 0;          // 1 host, 2 guest, 0 solo
};
void Tick(const TickIn& in, double now) noexcept;  // game thread (P1a and the probe)

// Debug actions (docs/debug-actions.md "Enemy sync"); game thread.
//   enemy_sync 0 status, 1 on, 2 off (every mirror released), 3 the mirror table, 6 <0|1> the far placement's variant (A / B;
//   hidden runs), 7 <0|3> the driver's teleport cadence (3: a teleport every 500 ms between the sets; hidden runs)
bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;
// enemy_look <id | 0 nearest | -1 the one the partner faces | -2 the newest replayed (the host: sent) shooter | -3 behind it, facing the partner |
// -4 beside its line, facing it> <dist_m> [aim height m] (hidden runs): our Sam dist m from that enemy, facing it (-3: dist m behind it on the
// partner's line, facing him). A steered aim (shots 4 <deg> 1) in the next 60 s follows that enemy (the -4 one, 20 s: a point on its line).
bool Look(const double (&v)[3], char* msg, std::size_t cap) noexcept;
// enemy_shot_test <id | 0 nearest> [1 nohurt] (hidden runs, guest): 3 enemy replays level at our Sam's chest from 2 m on that
// mirror's side (the line to its hand and 50 deg either side).
bool ShotTest(const double (&v)[3], char* msg, std::size_t cap) noexcept;

// For shot_sync. Any thread: whether an entity is a frozen mirror (+-0x20), and whether the guards drop (enemy_mirror_guard).
bool IsFrozenMirror(std::uintptr_t entity) noexcept;
bool AnyFrozen() noexcept;  // any frozen mirror now (the bullet detours' cheap exit)
bool GuardOn() noexcept;
// Game thread: a live frozen mirror's entity (0 when it is not one), and the time the mirrors show on the host's tick clock.
std::uintptr_t MirrorEntity(std::uint32_t actor_id) noexcept;
bool RenderClock(double* render_ms) noexcept;

bool Probe(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;  // game thread (a debug action)
// The probe's picked MULE's entity while it is still the same realized actor (else 0): the shots' aimed test (hidden runs).
std::uintptr_t PickedEntity() noexcept;

}  // namespace st::esync
