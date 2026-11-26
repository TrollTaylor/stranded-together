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
//   enemy_sync 0 status, 1 on, 2 off (every mirror released), 3 the mirror table, 4 the outcome table, 5 the ownership table
//   (P1b), 6 <0|1> the far placement's variant (A / B; hidden runs), 7 <0|3> the driver's teleport cadence (3: a teleport every
//   500 ms between the sets; hidden runs), 8 <0|1> ownership off (a reset: every enemy the host's) / on (P1b)
bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;
// enemy_look <id | 0 nearest | -1 the one the partner faces | -2 the newest replayed (the host: sent) shooter | -3 behind it, facing the partner |
// -4 beside its line, facing it | -5 the newest HIT's enemy (P2; the guest: sent, the host: applied; down and dead ones allowed) | -6 the probe's
// pick | -7 the nearest enemy the guest owns (P1b) | -8 in front of the last enemy_look's enemy, along its facing (round 20 M2; round 23 E: a
// negative dist_m places our Sam that far behind it, facing its back) | -9 the newest outcome set or carried here> <dist_m>
// [aim height m] (hidden runs): our Sam dist m from that enemy, facing it (-3: dist m behind it on the
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
// Round 23 B (game thread): a copy hidden here as "not holding" (enemy_mirror_hold_check): its shots are still replayed, from the
// owner's place. True with that place now (the SEN1 render time) and our copy's entity (0 unrealized).
bool NotHoldingShooter(std::uint32_t actor_id, double (&place)[3], std::uintptr_t* entity) noexcept;

// P2 (enemy_combat.cpp; docs/enemy-sync.md 9). Any thread: a frozen mirror's actor id (the entity +-0x20).
bool MirrorIdOf(std::uintptr_t entity, std::uint32_t* id) noexcept;
// Game thread: the gate as the tick left it (role 1 host / 2 guest / 0, mirroring on, our guids fingerprint, the world's load
// counter, the link session).
struct GateView {
    int role = 0;
    bool mirroring = false;
    std::uint32_t guids = 0, world = 0, gen = 0;
};
GateView Gate() noexcept;
// Game thread, the host: its 10 Hz class (the enemies realized within enemy_mirror_radius of the guest), up to cap.
unsigned NearEnemies(std::uint32_t* ids, std::uintptr_t* actors, unsigned cap) noexcept;
// Game thread, the guest: a live frozen mirror's actor and entity.
bool MirrorOf(std::uint32_t actor_id, std::uintptr_t* actor, std::uintptr_t* entity) noexcept;
std::uint32_t PickedId() noexcept;  // the probe's pick (0 none)
std::uint32_t LookId() noexcept;    // the newest enemy_look's enemy (0 none)

// P1b (round 17, docs/enemy-sync.md 10; session.ini enemy_owner): ownership by proximity. The host decides at 2 Hz: an enemy
// within 40 m of the guest, the host 15 m farther and beyond 25 m, calm (its phase and its camp's Normal), undamaged, not held, no
// cutscene, is handed to her game (SCB1 OWN: the host freezes its copy first and follows her SEN1 / SEP1 frames; she teleports her
// mirror to the host's place, sets its phase and releases it: her AI runs it, it notices, targets and shoots at her Sam, and her
// hits on it are real). Back by proximity (calm, after a 10 s hold): she is beyond 60 m or the host within 20 m. Forced back
// (whatever its phase): her silence 3 s, it unrealized or held there, beyond enemy_mirror_radius + 20 m, no OWNACK in 3 s, a
// refusal, an outcome, enemy_own. Any reset (a world, the link, the gate, the switch, a fault, a host save) makes every enemy the
// host's at once (a new generation). The host's copy comes back with her phase raised into it (RisePhase, capped at Caution);
// health is not carried.
// Game thread: ecombat hands it the SCB1 types 3 OWN, 9 OWNACK and 10 OWNRESET it drains (handled on the next tick).
void QueueOwnMessage(const std::uint8_t* bytes, std::size_t size) noexcept;
// P1c (round 21): the host relays its hits on a frozen copy of hers (enemy_owner_hits); the guest applies them to the one she owns
bool OwnRelay() noexcept;                    // ownership on and enemy_owner_hits (the host's capture of its followers' hits)
bool OwnedHere(std::uint32_t id) noexcept;   // the guest owns it now
// Round 24 (the M2 probe P2), any thread: the hits on our Sam by an enemy we own (the guest's "an owned enemy hit our Sam"
// attribution: the attacker, its weapon's holder or its weapon instance's holder), per attacker id since the start (id 0: all of
// them; at most 16 ids are told apart).
unsigned OwnedHitsOn(std::uint32_t id) noexcept;
// Round 24, game thread: an enemy's camp in OUR world: its phase (+0x110), its CP's id and phase (+0x210, read live when the CP
// actor is known, else the 2 Hz walk's), and the other members of that CP here: frozen copies (mirrors or the host's followers),
// owned here (the guest) or running here. False when the enemy is not ours to read; cp_id 0: in no camp list (or not walked yet).
struct CampView {
    std::uint8_t phase = 0xFF, cp_phase = 0xFF;
    bool cp_live = false;  // cp_phase read now (else the walk's)
    bool single = false;   // the host: handed over alone (enemy_own <id> 3)
    std::uint32_t cp_id = 0;
    unsigned members = 0, frozen = 0, owned = 0, running = 0;
};
bool CampViewOf(std::uint32_t id, CampView* out) noexcept;
bool Camp(const double (&v)[3], char* msg, std::size_t cap) noexcept;  // enemy_camp <op> <id> <k> (hidden runs)
// Round 23 (docs/enemy-sync.md 14), game thread: the tie-up relay's pose hold on our frozen copy of <id> (either game: the guest's
// mirror of a host enemy, the host's follower of one she owns). While held, the copy's transform writes and its SEP1 pose apply
// pause (its pose slot captures the copy's own animation instead); it ends after ms (the timeout: "re-posed") or at ReleaseHold,
// and the owner's frames are blended back in over 250 ms. HoldCopy: false when <id> is no frozen copy here.
bool HoldCopy(std::uint32_t id, double ms, const char* why) noexcept;
void ReleaseHold(std::uint32_t id, const char* why) noexcept;
bool Holding(std::uint32_t id) noexcept;
// The partner's newest SEN1 sample for <id> as an erules::Defeat (its defeat bits; dead from kDead; 0 none or no sample).
std::uint8_t PeerDefeat(std::uint32_t id) noexcept;
// Our frozen copies now (either game): ids, actors and entities, at most cap.
unsigned FrozenCopies(std::uint32_t* ids, std::uintptr_t* actors, std::uintptr_t* entities, unsigned cap) noexcept;
// enemy_own <id | 0 the nearest the host owns to the partner | -5 the nearest the guest owns> <1 host | 2 guest | 3 guest alone |
// 4 host (a single back)> (hidden runs, the host): a forced hand-over (reason 7) of its camp (1 / 2; the rules apply again
// afterwards: the 15 s hold), or (round 24) of that one enemy alone (3), its camp held out of the camp decisions until it is
// given back (4, or 1 on a single take).
bool Own(const double (&v)[3], char* msg, std::size_t cap) noexcept;
// Round 23 D (docs/enemy-sync.md 14.10), game thread: one of this game's enemies by its actor id, for the truck lines of the shared
// vehicles: its camp ("cp 04004", "loose 21A3F"; "-" when the camp walk has not listed it) and its seat (+0x3C0 with the ride-field
// byte checks, else -1). False when the sync is off or the id is not one of our enemies (camp "-", seat -1).
bool MuleRideInfo(std::uint32_t actor_id, char* camp, std::size_t cap, std::int32_t* seat) noexcept;
// Round 23 E (the M2 probe P6, docs/enemy-sync.md 14.11), game thread: a MULE actor's ride fields (+0xE20 the vehicle actor id, +0x3C0
// the seat) with their byte checks (run here once when enemy_mirror_vehicle=0 left them unchecked). False: not read.
bool RideFields(std::uintptr_t actor, std::uint32_t* e20, std::int32_t* seat) noexcept;

bool Probe(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;  // game thread (a debug action)
// The probe's picked MULE's entity while it is still the same realized actor (else 0): the shots' aimed test (hidden runs).
std::uintptr_t PickedEntity() noexcept;

}  // namespace st::esync
