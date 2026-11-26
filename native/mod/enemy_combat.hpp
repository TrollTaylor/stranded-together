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
bool Vitals(std::uintptr_t entity, float* health, float* stamina) noexcept;
void Status() noexcept;                               // the `enemy_sync 0` lines
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
// enemy_m2 <probe> [a] [b] (round 23 E, docs/enemy-sync.md 14.11; hidden runs and enemy_m2_probes=1): the M2 probes, each logging
// "[probe] P<n> ..." lines and one "[probe] P<n> result ..." line. 0 the byte checks; 1 <id | 0> <s> a copy's DSAINoticeAndKnowledge
// slots every 0.5 s (a PlayerVisual entry for our Sam: fresh / stale / none); 2 <id | 0> <lever 0..3> (the guest) armed (15 s after the hand-over):
// when the enemy is handed to her game, the lever (a nothing, b a test hit, c the perfect-threat alert, d' Sam's own stimulus +0xA4
// x3 for 1 s) and the time to within 30 deg of our Sam and to its first shot; 3 the perception reads (entity +0x314, stimulus +0x88);
// 4 <id | 0> <s> the raycast from a copy's eye to our Sam (one call a tick); 5 <id | 0> <s> the host's own MULE: phase, state,
// facing and knowledge every 0.25 s; 6 [id | 0] [m] the parent chain (+0x70) and ride fields of the MULEs within m (a seated one).
bool M2Probe(const double (&v)[3], char* msg, std::size_t cap) noexcept;

// Round 25 (docs/enemy-sync.md 14.18), her game, game thread (enemy_sync.cpp): her take of an enemy (TakeBefore before the member's
// SetPhase, TakeAfter after the unfreeze; alone: no other member of its camp is hers) and its refreeze when given back. They log the
// P2 diagnostics D3 / D5 (enemy_m2_probes, P2 armed for it) and run the levers: C1 (enemy_take_camp or P2 lever f: her camp forced to
// 5 first), K1 (enemy_take_know or lever e: the game's forced player visual, then 2 Hz in the combat tick), T1 (enemy_ticket_return
// or lever g: tickets held by frozen copies returned). The levers act in hidden runs only, on takes alone.
void TakeBefore(std::uint32_t id, std::uintptr_t actor, bool alone) noexcept;
void TakeAfter(std::uint32_t id, std::uintptr_t actor, std::uintptr_t entity, bool alone) noexcept;
void Refreeze(std::uint32_t id, std::uintptr_t entity) noexcept;
// Her MULE no longer hers by any path (given back, frozen or not, or her own reset): R2's role written at its take restored.
void TakeEnded(std::uint32_t id, bool given_back) noexcept;

}  // namespace st::ecombat
