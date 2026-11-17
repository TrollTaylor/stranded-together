// The partner as a real (solid, non-hologram) character that plays the other
// player's real animation (docs/real-partner-body.md).
//
//   Body: one of the game's own cutscene-actor entities (body_table.hpp, e.g.
//         ds/entities/cinematics/demo_sam_clean), loaded by path + GUID through the
//         streaming manager's own Register function (a StreamingRef we build from a
//         temporary proxy) and created with the same asynchronous spawner the
//         hologram body uses (partner_holo.hpp SpawnBeginField).
//   Animation: pose sync. A MinHook post-hook on the SkinnedModel animate function
//         (ds.exe 0x1423AA540, `void(SkinnedModel*, float dt)`) copies Sam's finished
//         local joint pose after the game computed it, and overwrites the partner
//         body's finished pose with the received one (then reruns the game's own
//         model-matrix and bounds steps, exactly the tail of that function), before
//         cloth, post-update messages and rendering read it.
//
// Threading: everything here runs on the game thread (st::sync::Tick) except the
// detour, which runs on whatever thread updates a SkinnedModel. The detour only
// compares two atomics for every other model; for Sam's and the partner's model it
// reads game memory with ReadProcessMemory, writes the partner's joints with
// WriteProcessMemory (both fail instead of faulting), and calls the two pure-math
// game functions the original just called on the same pose on the same thread.
// No logging, allocation or locks in the detour; counters only.
#pragma once
#include <cstddef>
#include <cstdint>

#include "body_table.hpp"
#include "game_api.hpp"
#include "pose_wire.hpp"

namespace st::realbody {

// ---- startup (Bootstrap, after MH_Initialize, before the frame pump hook):
// byte-checks every address and installs the animate hook (disabled detour work
// until a body or a capture is published). Logged; false when unavailable.
bool InstallHook() noexcept;
bool HookInstalled() noexcept;
// All spawn functions verified (lazy, logged once). `why` gets the reason otherwise.
bool Available() noexcept;
const char* UnavailableReason() noexcept;

// ---- per tick (game thread), before the partner logic: publishes Sam's
// SkinnedModel to the detour, drives a debug-owned spawn, mirror mode and the
// per-second [pose] counters.
void Tick(game::Entity* local, bool local_active, double now_ms) noexcept;

// ---- the body (the partner slot, owned by the partner sync or by a debug action; the self
// view has a second slot of its own, below)
enum class Owner : std::uint8_t { none, partner, debug, self };
enum class Phase : std::uint8_t { idle, loading, live };
enum class BeginResult : std::uint8_t { started, busy, unavailable, failed };
BeginResult Begin(const bodies::Body& body, const game::WorldTransform& at, Owner owner, double now_ms, char* why,
                  std::size_t cap) noexcept;
enum class PollResult : std::uint8_t { idle, loading, became_live, live, failed };
PollResult Poll(double now_ms, char* why, std::size_t cap) noexcept;
bool Move(const game::WorldTransform& at) noexcept;
bool Remove(const char* why) noexcept;  // drains the detour first; true if there was something
Phase GetPhase() noexcept;
Owner GetOwner() noexcept;
const char* PhaseName() noexcept;
game::Entity* LiveEntity() noexcept;
const bodies::Body* CurrentBody() noexcept;
// A body that failed to load or validate is not tried again this session.
bool Refused(std::uint8_t body_id) noexcept;
void ResetRefused() noexcept;

// ---- pose stream (game thread)
// Sam's newest captured pose as a frame (77 rotations + root/pelvis translation).
// False when nothing fresh (< 250 ms) was captured.
bool CaptureFrame(wire::JointFrame* out) noexcept;
// Received / echoed frames, keyed by the time they should play at (our clock). False when the
// frame was dropped: not Sam's 77-joint layout (the body could not play it), or not newer than
// the ring's newest key (reordered). Only accepted frames count as "the peer sends joints".
bool PushFrame(const wire::JointFrame& f, double key_ms) noexcept;
void ResetFrames() noexcept;
// True when the body has been live for `limit_ms` without any frame pushed in that time (a
// same-build peer that sends no STJ1: partner_body=holo, or its capture is unavailable, paused or
// behind a notice; other builds are rejected at the link handshake).
bool StarvedOfFrames(double limit_ms) noexcept;
// ---- outfit (docs/real-partner-body.md section 9; game thread)
// This game's own Sam outfit (sent in STJ1). False when unavailable (byte checks, no player).
bool ReadOutfit(game::Entity* local, wire::Outfit* out) noexcept;
// The outfit the partner sent (nullptr: none / a new session). The partner's Sam body
// (demo_sam_clean) is dressed with it: its own darker suit variation removed, the exos
// variation added, the suit colour set.
void SetPartnerOutfit(const wire::Outfit* o) noexcept;

// Interpolates the ring at play_ms and publishes the result to the detour.
// False when the ring is empty (the detour keeps the last pose).
bool SamplePublish(double play_ms) noexcept;

// ---- self view (docs/real-partner-body.md section 8): on THIS player's own screen the local
// Sam is replaced by the chosen character. A second body (its own slot) stands at Sam's world
// transform, copied in the detour every frame Sam's model is updated, and plays Sam's live pose
// (the newest capture, no wire, no delay). Sam's own model is hidden through his mesh-visibility
// controller (player +0xA60, Root hide-reason mask +0xD98, reason 63 ours, set and cleared with
// locked operations); the game's own hide reasons are never touched. Sam is hidden only while
// the body is shown and posed: Sam asleep (loads, cutscenes, knot space), any hide reason of the
// game's own (photo mode, scripted scenes), a pose that stops, a removed body or a new player
// show Sam again first. Everything runs from Tick (game thread).
// `body` = the character to show in Sam's place, nullptr = off (the host, Sam chosen, or
// session.ini self_character=0). Needs the pose hook.
void ConfigureSelf(const bodies::Body* body) noexcept;
bool SelfConfigured() noexcept;
bool SelfShown() noexcept;           // the body is shown and Sam hidden right now
game::Entity* SelfEntity() noexcept;  // the self body (loading or live), nullptr when none

// The per-frame sync faulted (dllmain GuardedTick) and Tick will not run again this session: the
// detour stops all work (both slots unpublished, no capture), our hide reason is cleared on Sam and
// a shown self body hidden, so a dead tick never leaves the player invisible. Logged.
void EmergencyStop() noexcept;

// ---- debug actions (game_actions.cpp): op 1 probe, 2 spawn <body id> <dist m> <mirror 0/1>,
// 3 remove, 4 joint names, 5 stats, 6 mirror <0/1>, 7 control item <n>,
// 8 self view <0/1> <body id, 0 = configured> <follow: 1 Sam's update, 2 the body's, 3 tick, 0 unchanged>
//   (8 with 2 <reason 0..63> <0/1>: hidden tests on scratch saves only, sets / clears a hide reason
//   on the local Sam's mask, e.g. 26 = photo mode's, 63 = a stray bit of ours),
// 9 outfit <0 log / 1 like our Sam / 2 exos colour / 3 override off / 4 own suit colour (hidden tests
//   on scratch saves only)>.
bool DebugCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

}  // namespace st::realbody
