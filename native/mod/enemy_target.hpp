// Round 27 M2 / M3 (docs/enemy-sync.md 16): enemies notice, pick and fight the partner body as well as our Sam. All keys default
// off (enemy_partner_stim 0, enemy_target_pick 0, enemy_target_redirect 0): nothing here touches the game until one is given.
//
// M2, the partner stimulus (the host only, hidden runs only until the game's body-destroy gap is closed; game thread): her real
// body (realbody::LiveEntity()) gets a visual stimulus made with our own Sam's AIVisualStimulusResource and the local player's actor
// id (lever L1, proven by the M1 probe, docs/enemy-sync.md 15), only while: a link, the body is there and its agent's vt+0x10
// returns it, the resource reads, the partner is not down, a fresh SPV1 frame from her, no cutscene on either side, and a realized
// human enemy is within enemy_partner_stim_radius of it. Every tick its place, velocity and visibility (+0xA4 from her SPV1 frame,
// +0xA0 / +0xB0) are written under the pool lock after the ownership check. Removed with the probe's two-step release (C1) when a
// condition fails for 2 s, the key goes off, the body goes, or on a fault; forgotten (not removed) when the manager changed.
// SPV1 (10 Hz, both roles, while enemy_partner_stim is on): our own Sam stimulus' +0xA4 / +0xA0 / +0xB0, our down / cutscene flags,
// and our PLAYER OBJECT's +0x2B0 flags word and aim height (its info +0x198 point above our root).
//
// M3, the target pick (enemy_target_pick bits) and the redirects (enemy_target_redirect bits), worker-thread detours: no
// allocation, no logging, no game calls but the original, the game's goal lookup the hooked task makes itself, and (bit 32) the
// weapon carrier's own SetTargetPos on the carrier the game passed; atomics, SEH reads and seqlock'd records only. A byte check
// that fails turns its bit off; a fault in the tick releases the stimulus, clears the table and turns every bit off.
#pragma once

#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "enemy_combat.hpp"

namespace st::etarget {

void Install(const Config& c) noexcept;                  // dllmain (the keys)
void Tick(const ecombat::TickIn& in, double now) noexcept;  // player_sync, after l1probe::Tick (game thread)
// Before the partner body (or its agent) can be freed (game thread). Nothing when no stimulus of ours is alive.
void ReleaseBeforeFree(const char* why) noexcept;
// esync's fault stop (game thread): the stimulus released, the table cleared, the pick and every redirect off for this run.
void Stop(const char* why) noexcept;
// The mod's emergency stop (any thread): the release's first step; every detour passes through.
void EmergencyStop() noexcept;
// The frames after the emergency stop (game thread): the parked stimulus removed (at most three tries).
void AfterStop() noexcept;
// enemy_tgt <op> [a] [b] (game thread): 0 status; 1 <0|1> the stimulus forced off / back to the key; 2 <vis x100> her +0xA4 forced
// (hidden runs; < 0 ends it); 3 <id> [s] a watch; 4 <arm code> <id> a labelled arm; 5 <id> <0 none|1 her|2 Sam> a forced pick
// (hidden runs); 6 <bits> the redirect bits for this run (hidden runs; -1 back to the key); 7 <id> [arm code] the geometry line
// (valid by that arm's rule: the code given, else the running arm's, else the gun rule) and the MULE's shoot line; 8 the arm ends (with its shoot line); 9 <n> n create / remove cycles, then made again (hidden runs,
// U15); 10 the crash-reporter guard test; 11 <id> <arm code> (run 3, hidden runs) the arm at geometry: op 4 once her geometry is
// valid for the arm (or 2.5 s), our Sam placed first for TF0 / TF1 (170 m, 40 deg off her line) and CS (5 m along its facing).
// Ops 1..11 refuse after a stop or a fault.
bool Command(const double (&v)[3], char* msg, std::size_t cap) noexcept;

// game thread: the partner stimulus is ours and live / its agent (0 none)
bool StimLive() noexcept;
std::uintptr_t StimAgent() noexcept;
// game thread (run 2: the L1 probe's contamination test): ms since the live partner stimulus was made; -1 none
double StimAgeMs() noexcept;

// Redirect bit 32, the shoot actor (docs/enemy-sync.md 16.4): a registered MULE's shoot numbers, for the probe's watch. The hooks are
// in with bit 32 on, or in a hidden run while the pick or a redirect is wanted (the aim point recorded with bit 32 off too). Game
// thread; false when `id` has no registered slot. The counts restart at the slot's registration and at its arm's start (enemy_tgt 4).
struct ShootStat {
    std::uint32_t calls = 0;       // the body shoot job's SetTargetActor(id 0) calls on its weapon carrier
    std::uint32_t redirected = 0;  // of those, given her chest instead (SetTargetPos; bit 32 live, its pick her, her record fresh)
    std::uint32_t points = 0;      // the job's aim points recorded (its GetTargetPosition call: the muzzle's aim)
    unsigned at_her = 0, at_sam = 0, at_none = 0;  // 4 Hz samples of those points by the player body they were at (1.5 m)
    bool have = false;             // a point recorded
    double point[3] = {};          // the last one (world)
    int mode = -1;                 // the carrier's mode after it (0 actor, 1 place, 2 none)
    double age_ms = -1;            // its age
    int nearest = 0;               // l1rules::Who of that point now (0 none, 1 her, 2 Sam)
    double d_her = -1, d_sam = -1; // its distance to her / our Sam's body segment (m; -1 unknown)
    std::uint32_t pt_actor = 0, pt_place = 0, pt_other = 0;  // (run 3) the points by the carrier's mode after them (0 / 1 / other)
    std::uint32_t t36 = 0;  // (run 3 review) the job's GetTargetPosition calls that took the MULE wrapper's type-0x36 branch
};
bool ShootStats(std::uint32_t id, ShootStat* out) noexcept;
// (M3f) pick bit 16's view of MULE `id` (the L1 probe's watch line; game thread): her record's age in its PlayerVisual bucket,
// the line-of-sight verdict (erules::Sight; 0 none yet) and its age (UINT32_MAX: never)
struct SightStat {
    std::uint32_t bucket_age_ms = UINT32_MAX;
    std::uint8_t los_v = 0;
    std::uint32_t los_age_ms = UINT32_MAX;
};
bool SightStats(std::uint32_t id, SightStat* out) noexcept;
const char* SightText(std::uint8_t v) noexcept;  // erules::SightText

// H3 (any thread; atomics only): a hit on MULE `id` by her (partner) or by our Sam; our Sam's from the OnHit detour.
void NoteHit(std::uint32_t id, bool partner) noexcept;
void NoteOnHit(std::uintptr_t receiver, std::uintptr_t di) noexcept;

// the shared sense detours (enemy_sense_hooks.cpp; worker threads)
bool SenseClsRefuse(std::uintptr_t stim, std::uint32_t own) noexcept;  // true: the classifier is not called, the result is 0
// the classifier's results (its third argument: +0 the bucket count, +8 the buckets, each's +0 its type), its return and type out
std::uintptr_t SenseClsAfter(std::uintptr_t stim, std::uint32_t own, std::uintptr_t results, std::uintptr_t r, int* type_out) noexcept;
void SensePsr(std::uintptr_t k) noexcept;

}  // namespace st::etarget
