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

// ---- the body (one slot, owned by the partner sync or by a debug action)
enum class Owner : std::uint8_t { none, partner, debug };
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
// Received / echoed frames, keyed by the time they should play at (our clock).
void PushFrame(const wire::JointFrame& f, double key_ms) noexcept;
void ResetFrames() noexcept;
// True when the body has been live for `limit_ms` without any frame pushed in that time
// (a peer that sends no STJ1: a v0.3 build, or its pose capture is unavailable).
bool StarvedOfFrames(double limit_ms) noexcept;
// Interpolates the ring at play_ms and publishes the result to the detour.
// False when the ring is empty (the detour keeps the last pose).
bool SamplePublish(double play_ms) noexcept;

// ---- debug actions (game_actions.cpp): op 1 probe, 2 spawn <body id> <dist m> <mirror 0/1>,
// 3 remove, 4 joint names, 5 stats.
bool DebugCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

}  // namespace st::realbody
