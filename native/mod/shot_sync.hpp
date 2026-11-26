// Shots (docs/enemy-sync.md P3a, "ship A"): a player's gunfire shown on the other screen with the game's own muzzle flash,
// trail and impacts. The shooter's game catches every bullet record its Sam's weapon enqueues (a detour on the bullet system's
// Enqueue 0x142916860, capture only), sends each shot twice in SFX1 frames (shot_wire.hpp); the other game holds it until the
// partner body plays that moment and replays it through the game's own CreateBullet (0x14294AD40) from the partner body's
// gun, with the shooter's weapon id (the muzzle flash) and magazine id (the magazine's effect). A replay is visual only: its
// attack references go into the replay set, and its hits are dropped at the bullet (round 16, docs/enemy-sync.md 7.5: detours on
// BulletDamageA 0x142912120 / B 0x142912380, where every bullet hands its damage to a vehicle, cargo, structure, BT, NPC or
// prop, and on the BT reaction 0x142992D80 through the hit processor 0x14291CBC0), except on our own Sam, whose hits the shared
// Sam damage detour (sam_damage.hpp) drops by policy; a detour on the Humanoid MsgDamage handler (0x1437393D0) stays as a
// backstop. Every value the game reads under the bullet system's lock (the category, the attack id, +0xD1) is checked on the
// receiver first: a fault there cannot be recovered. session.ini shots_sync (default on since round 16; see config.hpp),
// shots_muzzle, shots_sound, shots_friendly_fire, shots_npc_drop, shots_hit_drop, shots_bt_drop (default 0).
//
// R1 also lives here: the SEH1 combat beacon (both games say which combat parts they run), and the counters the enemy probe
// reads (shots by an owner entity). Every site is byte-checked before use; every call SEH-guarded; the first fault stops it.
#pragma once

#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "game_api.hpp"
#include "sam_damage.hpp"
#include "shot_wire.hpp"

namespace st::shots {

// Start-up (dllmain, before the tick): the byte checks, the Enqueue detour and the damage filters when shots_sync is on (or a
// hidden run may turn it on). Hooks go in now only (enabling one suspends every thread).
void Install(const Config& config) noexcept;
bool Active() noexcept;             // capture, send and replay on now
const char* OffReason() noexcept;   // "" when on, or not wanted (shots_sync=0)

struct TickIn {
    game::Entity* local = nullptr;  // our Sam
    bool local_ok = false;
    game::Entity* body = nullptr;   // the partner body (real or hologram), nullptr when none
    bool have_play = false;         // the pose play time (the partner's unwrapped sender ms)
    double play_ms = 0;
    bool net = false;               // a host / guest link (not solo)
    std::uint8_t role = 0;          // 1 host, 2 guest, 0 solo
};
void Tick(const TickIn& in, double now) noexcept;  // game thread

// Debug actions (docs/debug-actions.md "Shots"); game thread. op: 0 status, 1 on, 2 off, 3 <m> <deg> (hidden runs) our Sam m from the
// partner body, deg around its heading (0 in front, 90 its right), facing it (the topmost ground there); 4 [deg] (hidden runs) the
// game camera checked every tick until it looks at the partner body within deg (default 12): "[shots] camera aimed #n". Hidden runs
// (round 16): 5 <target> <n> the aimed test (1 vehicle, 2 structure / 100 + type, 3 cargo, 4 the enemy probe's MULE; n < 0 the
// control), 6 the cargo target, 8 <muzzle> <magazine> the next replays' ids, 9 <yaw> [pitch] the steered aim's offset.
bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;
// shots_test <dist_m> <flags> [count]: CreateBullet from dist m ahead of our Sam toward his chest through the replay path, with
// the last captured shot's attack / weapon / magazine ids. flags: 1 the filter keeps it (proof under dev_god 0), 2 no weapon id,
// 4 no magazine id, 16 a leak test: `count` replays (<= 400, 4 a tick) into the ground 12 m ahead, away from Sam, with the
// process's working set logged before and after.
bool Test(double dist_m, int flags, int count, char* msg, std::size_t cap) noexcept;
// shots_echo <0|1> [offset_m]: solo: our own captured shots also replayed here from offset_m (default 2.5) to the right.
bool Echo(bool on, double offset_m, char* msg, std::size_t cap) noexcept;

// Enemy sync P1a (enemy_sync.cpp; docs/enemy-sync.md 8). The hooks go in when shots_sync or enemy_sync (and a partner can be in
// the session: not role none / solo outside hidden runs).
// Game thread: the beacon's enemy part (the mirror bit and the fingerprints; the enemy fire bit is ours: enemy_shots and this game
// can replay; P1b: kPartOwn is passed through), the partner's newest beacon (false: none in the last 20 s), the enemy shots' mask
// (shotw::kEnemyCapture: this game captures its owned shooters' records and sends them, P1a's host = 1; shotw::kEnemyReplay: it
// replays the other game's from its frozen mirrors, P1a's guest = 2; P1b = 3 on both), the owned shooters (entity -> actor id;
// read lock-free by the Enqueue detour).
void SetEnemyBeacon(std::uint8_t parts, std::uint16_t count, std::uint32_t ids_fp, std::uint32_t guids_fp, std::uint8_t freeze) noexcept;
bool PeerBeacon(shotw::Beacon* out, double* age_ms) noexcept;
void SetEnemyShots(int mask) noexcept;
void SetEnemyShooters(const std::uint64_t* entities, const std::uint32_t* ids, unsigned n) noexcept;
unsigned EnemyShooterCount() noexcept;
std::uint32_t LastEnemyShooter() noexcept;  // the newest replayed (the guest) or sent (the host) enemy shot's shooter (0 none)
// Hidden runs (enemy_look): the next steered aims (shots 4 <deg> 1) for for_ms point at p instead of the partner body's chest.
void SetAimTarget(const double (&p)[3], double for_ms) noexcept;
// ... or at an entity's place + up_m, followed every tick (a mirror walks while the aim settles).
void SetAimEntity(std::uintptr_t entity, double up_m, double for_ms) noexcept;
// The frozen mirrors' own bullet records seen / dropped in the Enqueue (any thread).
void MirrorShotCounts(unsigned* seen, unsigned* dropped) noexcept;
// enemy sync's guard (the guest freezes nothing without it): the Enqueue hooked and the drop at the bullet installed (a frozen
// mirror's bullet hits dropped there), not stopped. EnemyReplayReady: the host's enemy rounds can be replayed here. The hits on
// frozen mirrors dropped at the bullet so far.
bool MirrorGuardReady() noexcept;
bool EnemyReplayReady() noexcept;
unsigned MirrorBulletDrops() noexcept;
unsigned MirrorBulletDropsOurs() noexcept;     // ... of them our Sam's own rounds (her rifle's)
unsigned MirrorBulletDropsReplays() noexcept;  // ... of them our replays' rounds
// P2 (enemy_combat.cpp; docs/enemy-sync.md 9), game thread: the beacon's hits part (bit1) on / off (why_off: a static text ending in
// ')', shown in the `combat parts` line), and her own rounds on a mirror passed to Entity OnHit so far.
void SetHitsPart(bool on, const char* why_off) noexcept;
// Round 23 (docs/enemy-sync.md 14), game thread: the beacon's restrain part (shotw::kPartRestrain: this build sends and takes HIT kind
// 5) on / off; the partner's beacon carries it (false: no beacon or an older build).
void SetRestrainPart(bool on) noexcept;
bool PeerRestrain() noexcept;
// Round 23 C (docs/enemy-sync.md 14.9), game thread: our own player's down code (erules::PlayerDown, 0..4) for the beacon's byte 22.
// Sent only once the partner's beacon has carried kPartRestrain on this link generation (a round-23 build; an older decoder refuses
// the byte). That is latched: a partner whose restrain part drops later (a fault there) still decodes the byte, and a 0 sent for that
// reason would read as "back" (review). At once on a change and then every 1 s for 5 s. Returns the code the beacon carries now (0
// while the partner cannot take it).
std::uint8_t SetPlayerDown(std::uint8_t code) noexcept;
// Round 23, game thread: the replay flush. A down outcome (faint, sleep, restrained, dead) for `shooter` reached this game: its
// queued enemy shot replays are dropped now, one line "[shots] flushed n replays of id=.. (down)". Returns n.
unsigned FlushEnemyReplays(std::uint32_t shooter, const char* why, bool quiet_if_none = false) noexcept;
unsigned MirrorBulletPassed() noexcept;
// A damage message of one of our replays (any thread).
bool IsReplayHit(const samdmg::View& v) noexcept;
void EnemyShotStatus(char* out, std::size_t cap) noexcept;
// enemy_shot_test (hidden runs, the guest): one enemy replay (kind 2) from the mirror's hand at our Sam's chest; 2 s later one line
// "[enemy] shot test: ref match=.. kept (enemy_shots_hurt) | dropped (nohurt) amount .. health .. -> ..".
bool EnemyShotTest(std::uintptr_t mirror_entity, std::uint32_t id, bool nohurt, char* msg, std::size_t cap) noexcept;

// The enemy probe (enemy_sync.cpp): bullets enqueued by an owner entity while armed (a table of 16).
void ArmOwnerCounts(bool on) noexcept;
unsigned ShotsBy(std::uintptr_t entity) noexcept;
// The L1 probe (round 27 M1): round n (1-based, as ShotsBy counts) of that owner while still among its newest 16: the record's
// origin (+0x60) and unit direction (its velocity +0x70). False: not kept (or no velocity).
bool RoundOf(std::uintptr_t entity, unsigned n, double (&origin)[3], double (&dir)[3]) noexcept;
bool EnqueueHooked() noexcept;

}  // namespace st::shots
