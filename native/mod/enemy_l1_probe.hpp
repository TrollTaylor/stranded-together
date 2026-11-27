// Round 27 M1 (docs/enemy-sync.md 15): the L1 probe. Hidden runs only; no gameplay change, no session.ini key.
//
// Does lever L1 (a visual stimulus for her partner body, made with our Sam's own resource and carrying the local player's id)
// make a host MULE notice her, turn to her, approach her, aim at her and fire at her? `action enemy_l1 <op> [a] [b]` drives it
// (docs/debug-actions.md "Enemy L1 probe"); every op logs "[probe] L1 ..." lines and each arm one "[probe] L1 result arm X ..."
// line, read by tools/testenv/enemy-target.py. Read-only counting detours on the AI's classifier, its result append and its
// sensor results (atomics only; any thread) are installed at the first op, each byte-checked.
//
// Safety (C1; review round 27 M1): her stimulus is made only for the live real body the partner owns (never the hologram, a
// debug or self-view body) whose agent has a class name. The mod's own removals of that body call ReleaseBeforeFree first
// (player_sync RemoveBody and the slot's forget, enemy sync's stop, the mod's emergency stop). The release's first step: on the
// live stimulus +0xA4 0, +0x88 "no actor" (the classifier rejects it before the cone reads the agent), +0x84 1, then +0x10
// cleared by a compare-exchange; on its thread-safe copies only +0xA4 and +0x88 (their +0x10 is never written: a sensor may be
// between its two loads of it). Two ticks later it is removed under the pool's lock, only while it is still in the same
// manager's live list parked as ours (else forgotten); a remove that does nothing leaves it parked (+0x84 1). Every tick
// re-checks it in the live list as ours under the lock before writing. The tick releases early too when the body changes or
// stops being the partner's live real body, Sam sleeps, a load or setup starts, or the AI manager changes. A create or remove
// that faults stops all further creates and removes. Residual: a destroy by the game itself (real_body's PollSlot sees it only
// after the entity is gone) releases after the free; a sensor already past the classifier then reads a freed agent.
#pragma once

#include <cstddef>
#include <cstdint>

#include "enemy_combat.hpp"

namespace st::l1probe {

// enemy_l1 <op> [a] [b] (game thread; hidden runs only): 0 byte checks + reads; 1 <0|1> her stimulus off / L1; 2 <vis x100> her
// +0xA4 forced (the switch test's phase marks); 3 <id> <s> the watch (4 Hz); 4 <arm 1 A|2 B|3 C|4 D|7 G> <id> an arm starts; 5 <0
// standing|1 crouched> the guest's own stimulus reads (F); 6 <id> [s] re-calm the camp; 7 <id> [attack] the melee query at her
// chest; 8 the arm ends (its result line); 9 remove + stop (the decision line).
bool Command(const double (&v)[3], char* msg, std::size_t cap) noexcept;
// player_sync, after ecombat::Tick (game thread).
void Tick(const ecombat::TickIn& in, double now) noexcept;
// Before her partner body (or its agent) can be freed (game thread). Nothing when no stimulus of ours is alive.
void ReleaseBeforeFree(const char* why) noexcept;
// The mod's emergency stop (any thread): the release's first step, the detours pass through.
void EmergencyStop() noexcept;
// The frames after the emergency stop (game thread; dllmain): the parked stimulus removed (at most three tries).
void AfterStop() noexcept;
// The shared sense detours' read-only counts (enemy_sense_hooks.cpp; any thread, atomics only): the classifier's result, an
// append, ProcessSensorResults' record[0] for the watched K. Nothing before the probe's first op.
void SenseCls(std::uintptr_t stim, std::uintptr_t r, int* type_out) noexcept;
void SenseApp(std::uintptr_t array, std::uintptr_t record) noexcept;
void SensePsr(std::uintptr_t k) noexcept;
// Round 27 M3 (enemy_tgt 4 / 8; hidden runs): an arm of the target gate on the probe's watch, labelled `label`, with the partner
// stimulus (enemy_target.cpp) as her stimulus; its result line is the probe's ("[probe] L1 result arm <label> ..."). Phase marks
// for the switch times: TargetArmPhase(true) she hides (phase 0 -> 1), (false) she is back (1 -> 2).
bool TargetArm(const char* label, std::uint32_t id, double secs, char* msg, std::size_t cap) noexcept;
bool TargetArmEnd(char* msg, std::size_t cap) noexcept;
void TargetArmPhase(bool hidden) noexcept;
std::uint32_t TargetArmId() noexcept;  // the running target arm's MULE (0 none)
double TargetArmNoticedMs() noexcept;  // round 27 run 4: the running target arm's notice time (ms after its start; -1 none yet)

}  // namespace st::l1probe
