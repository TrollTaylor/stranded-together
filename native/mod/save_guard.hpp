// The save guard (docs/real-partner-body.md section 24.12): no save of the game ever captures our Sam while he is carried
// as a passenger (riding together, ride_together.cpp).
//
// Every save the game makes (the pause menu's Save, the scripted area / mission checkpoints, the autosaves, the retry
// checkpoint, the post-load quick save) is posted into ONE pending-save holder (M = [[0x147C0D6B0]+8]); the game's save
// processor 0x14341C350 issues it into the save system's queue (S = GameModule+0x2A0) as the game decides, never delayed by
// the mod: some posters (the autosave task 0x143419760) clear a request that is not queued by their next step. The save
// system's executor then prepares the capture job (S+0x98, the task S+0xA0 in state 2) and its kick 0x14362DB80 submits it
// once per frame; the job reads Sam's entity transform (+0xC8). The kick waits by itself (it returns before submitting
// while the game refuses, up to 91 frames), so a detour of the kick that returns delays the capture by that frame and
// nothing else: the save stays queued with its job prepared exactly as the game made them, posters waiting on the queue
// see it queued, and the game's own kick submits it later. The mod never writes, edits, copies or re-posts a save request
// or a task, and never touches save bytes.
//
// While a capture is prepared and our Sam is carried, the kick detour holds it (rwire::SaveGate) and asks the ride to end
// (WantsOff): ride_together's next tick puts our Sam on the ground beside the vehicle with the game's PlaceOn, turns his
// capsule back on, gives the pose and the input back and tells the driver (END "the game saves"). Once he is off, placed
// and outside the vehicle's box for 2 kick frames the detour calls the kick and the game captures. Capped at 3 s per capture
// (never held again after its cap or release): then the save runs unchanged (logged). The post-load save is never held.
// While a capture is held, a second request waits in the processor (the executor would otherwise give it the job and
// orphan the held task). Boarding is refused while a save is pending, queued or written (Busy), and a carried Sam whose save
// is not held (the hook off, the cap) is put off by the ride's last-resort exit.
//
// Byte checks (kSites in save_guard.cpp): the hook only when the kick, the executor's job site, the processor, its thunk, the
// save system's per-frame call, the issue site and the save system's own queries match (and the save system's vtable slot
// holds the kick); the reads (Busy / InProgress) need the same sites but the processor; the forced save (hidden tests) only
// when DSCreateSaveGame matches. A mismatch: that part off, one line in the log and the co-op banner (no reads: boarding as
// a passenger is refused).
#pragma once
#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "game_api.hpp"

namespace st::saveguard {

// Bootstrap thread, after ride::Install (MinHook initialised): the byte checks and the kick and processor detours. The
// hooks go in only while riding together is on (or in a hidden run).
void Install(const Config& config) noexcept;
// The hold is installed (byte checks passed, hooks enabled) and not stopped.
bool Active() noexcept;
const char* OffReason() noexcept;
// The mod's emergency stop (dllmain's whole-mod fault): the detours pass everything through from now on and the ride's
// published state is cleared and no longer taken (no tick will refresh it). Any thread.
void EmergencyStop() noexcept;

// Game thread, every tick, before ride::Tick: watches the save system (logs each save's states and its result), keeps the
// local Sam for the logs (`awake`: not asleep in a menu).
void Tick(game::Entity* local, bool awake, double now_ms) noexcept;

// The save reads' byte checks passed: Busy / InProgress can tell (without them the ride refuses boarding). Any thread.
bool ReadsOk() noexcept;
// A save is pending in the holder or queued, being captured or written (memory reads; false when the read checks failed).
bool Busy() noexcept;
// A capture may run or has run and the save is not written yet: the queue head in states 1..4 or a capture job, except a
// capture the guard holds (or will hold: not run yet, not post-load, not released or capped), during which the ride may
// place our Sam. Any thread.
bool InProgress() noexcept;
// The guard holds (or will hold) a save's capture while our Sam is carried: the ride must end now (its next tick). Any thread.
bool WantsOff() noexcept;
// ride_together, every tick and at each carry start / end: our Sam is carried; his exit placement still waits for the game's
// PlaceOn (its rate limit, its ground probe); the vehicle he rides (or rode last) and its kind (the box check after a ride;
// the logs give our Sam's place in its frame); the vehicle's speed while carried (m/s; Command 6's moving test). Ignored
// after EmergencyStop.
void SetRideState(bool carried, bool placing, std::uintptr_t vehicle_entity, std::uint8_t kind, double speed_mps = 0.0) noexcept;

// Debug action "Save guard" (debug_actions.cpp; game thread): 0 state and counters; 1 [type] [1] hidden runs only: a save now
// through the game's own DSCreateSaveGame (type 4 = the next autosave, the default; 1 = manual), as the game's scripts
// post one (the game may refuse it: its reasons are logged; with a third value 1 the action succeeds anyway); 2 log our Sam's place (and against where the last held save was released); 3 hidden runs, host: the pause
// menu's Load of the newest save this run wrote (through coop_session 5); 4 0/1 hidden runs: the hold off / on (the
// ride's last-resort exit alone); 5 hidden runs: why the game would refuse an auto / manual save now (its reason query
// 0x1433FEA20, the autosave cooldown, the save queue); 6 [s] hidden runs: the next forced save stays pending until our
// Sam boards (up to s seconds, 15 by default): the game refuses to post a save while he is carried, so the test posts it
// on foot and boards (boarding and the carry start are allowed for it), then the guard's own hold takes over (6 s 1: only
// once he is carried and asleep in a menu: the sleeping passenger's path; 6 s 2: only once he is carried and the vehicle
// drives at 8 m/s or more: a save while it moves fast); 7 [s] hidden
// runs: the same without boarding (a pending save that boarding must refuse).
bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;
// Command 6 is armed and not expired: boarding and the carry start while that test save is pending (hidden tests only).
bool TestAllowsCarry() noexcept;

}  // namespace st::saveguard
