// Enemy sync P2 (round 17, docs/enemy-sync.md 9): the guest's hits on the enemies the host owns, and their outcomes.
//
// The guest (her copies near her are frozen mirrors, enemy_sync.cpp): every hit given to a mirror is dropped where the game hands
// it to the entity (Entity OnHit 0x142349A10, the receiver's vt+0x10 for every entity; G2) or, for the direct MsgDamage senders, at
// the MULE's damage component (0x142B020F0; G3), so her world never applies, counts or reports it. Her own hits (our Sam, his
// weapon or a part of him, our player id in the attack context, the vehicle she drives) are captured on the way (any thread, a
// lock-free ring), deduped, turned into the mirror's yaw frame and sent to the host as SCB1 HIT messages on the reliable combat
// channel 6 (enemy_wire.hpp). Her own rounds reach OnHit because the drop at the bullet (shot_sync.cpp) lets them through to it
// while the hits are captured (shotw::MirrorBulletRule): her impacts and the bullet-hit message to her Sam stay.
//
// The host applies a HIT to its own copy with the game's own damage path (method M1, enemy_hit_path=direct): an attack context
// (0x14298AC90) whose owner is the partner body (enemy_hit_owner=credit: and the player's attacker id stamped, so the takedown
// counts as the host's), its parameter block stamped from the HIT as BulletDamageA stamps a bullet's, a DamageInfo built as
// BulletDamageA builds one (0x142314190) at the point turned into the host copy's frame, handed to the MULE entity's receiver
// vt+0x10 (OnHit -> QueueDamage -> ApplyDamage: exactly a bullet's path). M2 (enemy_hit_path=query) is the attack sphere query
// (partner_tackle's) on the point, keeping only that MULE. It answers HITRES, reads the MULE's state (+0xEA0, the down bits
// +0xEA4, the damage component's health / stamina) every tick and sends an OUTCOME (faint, sleep, restrained, dead, other down,
// none again) on every change. The guest keeps her mirror frozen and posed from the host's frames whatever the outcome (it lies
// where the host's lies); a mirror whose host copy is dead is hidden and put to sleep when it is given back (never released
// alive).
//
// session.ini: enemy_hits, enemy_hit_owner, enemy_hit_path, enemy_hits_lethal, enemy_hit_rate (config.hpp). Both games need it
// (SEH1 parts bit1 "hits"). Every site byte-checked at start-up (group `hits`), every call SEH-guarded, the first fault stops it.
#pragma once

#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "game_api.hpp"

namespace st::ecombat {

// Start-up (dllmain, after esync::Install): the byte checks, the OnHit and MULE handler detours. Nothing when enemy_hits=0, or
// without enemy sync outside hidden runs.
void Install(const Config& config) noexcept;

struct TickIn {
    game::Entity* local = nullptr;  // our Sam
    bool local_ok = false;
    game::Entity* body = nullptr;   // the partner body, nullptr when none
    bool net = false;
    std::uint8_t role = 0;          // 1 host, 2 guest, 0 solo
};
void Tick(const TickIn& in, double now) noexcept;  // game thread, after esync::Tick

// ---- shot_sync (any thread)
// Her own rounds on a mirror may pass the drop at the bullet to OnHit (the hits are on and captured here).
bool CaptureOurRounds() noexcept;
// P1c (round 21)
int AttackRegistry(std::uint8_t (&bits)[64], std::uint16_t* max_id) noexcept;  // the attack registry's ids (bits), their count; -1 unknown
bool CarryReady() noexcept;                         // the outcome setter's checks passed
// k 0 faint, 1 sleep, 2 restrained, on the in-use MULE `id` at `actor`: 1 done, 0 deferred (a queued hit), -1 not possible, -2 it is
// down or dead here already (never set twice)
int SetOutcome(std::uintptr_t actor, std::uint32_t id, int k) noexcept;
int OutcomeNow(std::uintptr_t actor, std::uint32_t id) noexcept;  // erules::Defeat of the in-use MULE `id` at `actor`; -1 not it
// An entity receiver (byte +8 set) whose vt+0x10 is Entity OnHit (fault-free reads).
bool IsOnHitReceiver(std::uintptr_t receiver) noexcept;
// BulletDamageA / B's original runs (the OnHit detour marks what it captures there as kind bullet).
void MarkBullet(bool on) noexcept;
// The drop at the bullet: her round passed to OnHit (true) / dropped on a part with no entity receiver (false).
void CountBullet(bool passed) noexcept;

// ---- enemy_sync (game thread)
std::uint8_t HostDefeat(std::uint32_t id) noexcept;   // the host's outcome table (erules::Defeat; 0 none)
std::uint8_t GuestDefeat(std::uint32_t id) noexcept;  // the guest's merged outcome (erules::Defeat; 0 none)
void NoteSenDefeat(std::uint32_t id, std::uint8_t defeat) noexcept;  // SEN1 state bits 2-4 (the redundancy: dead only)
std::uint32_t LastHitId() noexcept;                   // the newest HIT's enemy (the guest: sent; the host: applied), 0 none
// P1b (enemy_sync.cpp): ecombat drains the reliable combat channel (and hands OWN / OWNACK / OWNRESET to esync::QueueOwnMessage);
// while it does not (not installed, stopped), enemy_sync drains it itself. A realized MULE entity's health and stamina (its
// damage component; false: none).
bool DrainsCombat() noexcept;
bool PhitApplyReady() noexcept;  // round 28 E2: this game drains channel 6 and its apply group passed (the guest's PHIT hello says so)
bool Vitals(std::uintptr_t entity, float* health, float* stamina) noexcept;
void Status() noexcept;                               // the `enemy_sync 0` lines
// Round 27 run 4 (enemy_target.cpp's op 11; hidden runs, the host, game thread): an SCB1 PLACE to the guest: her Sam to (x, y)
// ground-snapped, facing (fx, fy), for the MULE `id`'s arm `code`. False with the reason in `why`; `seq` the PLACE's number.
bool SendPlace(std::uint32_t id, double x, double y, double fx, double fy, std::uint8_t code, std::uint32_t* seq, char* why, std::size_t cap) noexcept;
void OutcomeTable() noexcept;                         // `enemy_sync 4`

// ---- debug actions (docs/debug-actions.md "Enemy hits"; game thread; all but the table hidden runs only)
bool Dump(const double (&v)[3], char* msg, std::size_t cap) noexcept;         // enemy_dump <id | 0> <tag>
bool AttackInfo(const double (&v)[3], char* msg, std::size_t cap) noexcept;   // enemy_attack <attack>
bool CtxTest(const double (&v)[3], char* msg, std::size_t cap) noexcept;      // enemy_ctx <attack> <owner 1 body | 2 none | 3 sam>
bool HitMode(const double (&v)[3], char* msg, std::size_t cap) noexcept;      // enemy_hit_mode <part> <owner> <path>
bool HitTest(const double (&v)[3], char* msg, std::size_t cap) noexcept;      // enemy_hit <attack> [count] [id | 0]
bool HitSend(const double (&v)[3], char* msg, std::size_t cap) noexcept;      // enemy_hit_send <attack> [count] [id | 0]
bool HitWatch(const double (&v)[3], char* msg, std::size_t cap) noexcept;     // enemy_hit_watch <id | 0> <0 | 1>
// enemy_restrain <id | 0> <mode> (round 23, docs/enemy-sync.md 14; the guest, or the host while it relays): the tie-up relay's own
// send path on our frozen copy (0: the look's, else the nearest): 0 native (the outcome setter k 2 on our copy, then the watch),
// 1 fallback (a synthetic strand HIT through the capture path), 2 knockout (k 0 on our copy, then the watch).
bool Restrain(const double (&v)[3], char* msg, std::size_t cap) noexcept;
// enemy_target <op> [id | 0] [arg] (round 20 M1, the host): 0 read her body / our Sam / the MULE (AI agent +0x170, faction +0x178,
// IsHostileTowards), 1 / 2 L1 AlertForEntity(WithPerfectThreatInfo) -> 0 her body | 1 our Sam, 3 L3 RisePhase <1..5>, 4 <0 | 1>
// the partner body's hit watch, 7 follow <s>: the MULE's facing against her body and our Sam once a second.
// Round 20 M2 (the stimulus probe, docs/enemy-sync.md 12): 8 read the visual stimulus components and walk the AI manager's visual
// and aural lists; 9 <0 remove | 1 her agent with Sam's resource | 2 the same + Sam's component message | 3 the decoy (Sam's agent
// at her body)>. Hidden runs only.
bool TargetProbe(const double (&v)[3], char* msg, std::size_t cap) noexcept;
// enemy_m2 <probe> [a] [b] (round 23 E, docs/enemy-sync.md 14.11; hidden runs and enemy_m2_probes=1, still hidden runs only in round
// 26: a real game gets P2 lever a by itself at her single takes, TakeBefore): the M2 probes, each logging
// "[probe] P<n> ..." lines and one "[probe] P<n> result ..." line. 0 the byte checks; 1 <id | 0> <s> a copy's DSAINoticeAndKnowledge
// slots every 0.5 s (a PlayerVisual entry for our Sam: fresh / stale / none); 2 <id | 0> <lever 0..3> (the guest) armed (15 s after the hand-over):
// when the enemy is handed to her game, the lever (a nothing, b a test hit, c the perfect-threat alert, d' Sam's own stimulus +0xA4
// x3 for 1 s) and the time to within 30 deg of our Sam and to its first shot; 3 the perception reads (entity +0x314, stimulus +0x88);
// 4 <id | 0> <s> the raycast from a copy's eye to our Sam (one call a tick); 5 <id | 0> <s> the host's own MULE: phase, state,
// facing and knowledge every 0.25 s; 6 [id | 0] [m] the parent chain (+0x70) and ride fields of the MULEs within m (a seated one).
bool M2Probe(const double (&v)[3], char* msg, std::size_t cap) noexcept;
// Round 27 M4 (docs/enemy-sync.md 16.8): the ray layers (the filter's +8 is ONE 7-bit layer, not a mask; the names from the
// game's layer table 0x144ADE240, byte-checked in P0)
inline constexpr std::uint32_t kRayLayerEye = 0x20;      // "vs Eye blocker": the visual sense (0x142E4F486), 0x1425B7F86 with the same filter
inline constexpr std::uint32_t kRayLayerStatic = 0x2F;   // "Ray vs Static": the static probe 0x142A94BAF
inline constexpr std::uint32_t kRayLayerHearing = 0x34;  // "vs AI hearing": the aural sense's (the self-test's control)
// Round 27 M3f (enemy_target's line-of-sight estimate and the placement's ground probe; game thread only): one world ray on
// `layer` (the visual sense's by default) from `from` to `to` shortened by trim_m at both ends (P4's eprobe::Shorten; 0 when too
// short to cast). The hit count (0 clear), `hit` the first hit's position; -1 a fault (SEH), -2 no physics world, -3 the
// raycast's byte checks failed or the probes are off after a fault, -4 another negative return.
int WorldRay(const double (&from)[3], const double (&to)[3], double trim_m, double (&hit)[3], double* len,
             std::uint32_t layer = kRayLayerEye) noexcept;

// Round 25 (docs/enemy-sync.md 14.18), her game, game thread (enemy_sync.cpp): her take of an enemy (TakeBefore before the member's
// SetPhase, TakeAfter after the unfreeze; alone: a forced take with no other member of its camp hers) and its refreeze when given
// back. They log the P2 diagnostics D3 / D5 (enemy_m2_probes, P2 armed for it; round 26: armed by itself, lever a, at a take alone)
// and run the levers: C1 (enemy_take_camp or P2 lever f: her camp forced to 5 first), K1 (enemy_take_know or lever e: the game's
// forced player visual, then 2 Hz in the combat tick), T1 (enemy_ticket_return or lever g: tickets held by frozen copies returned).
// The levers act on takes alone only; since round 26 (docs/enemy-sync.md 14.19) on by default and in real games too.
void TakeBefore(std::uint32_t id, std::uintptr_t actor, bool alone) noexcept;
void TakeAfter(std::uint32_t id, std::uintptr_t actor, std::uintptr_t entity, bool alone) noexcept;
void Refreeze(std::uint32_t id, std::uintptr_t entity) noexcept;
// Her MULE no longer hers by any path (given back, frozen or not, or her own reset): R2's role written at its take restored.
// `world_gone` (round 26): her reset is for a world that is gone or going: the role is written back only when this game's live
// world (coop::WorldReady) is still the take's.
void TakeEnded(std::uint32_t id, bool given_back, bool world_gone = false) noexcept;

}  // namespace st::ecombat
