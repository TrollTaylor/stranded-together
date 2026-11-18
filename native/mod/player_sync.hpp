// Per-frame player sync, run on the game's main thread from the
// SteamAPI_RunCallbacks hook:
//   local player transform -> pose frames (send_hz)
//   received pose frames   -> interpolation ring -> partner body transform
// The partner's body is a non-player entity we drive by writing its
// WorldTransform every frame: by default the game's welcome/collectors
// hologram (partner_holo.cpp), falling back to a trace-point prop and then the
// invisible echo (partner_source.cpp). See docs/partner-hologram.md.
// Milestone 1: position + rotation only.
#pragma once
#include <cstddef>
#include <string>

#include "config.hpp"

namespace st::debug {
struct Snapshot;
}

namespace st::sync {

void Init(const Config& config, const std::wstring& mod_dir) noexcept;
void Tick() noexcept;  // game thread, every frame

// ---- In-game debug menu (debug_actions.cpp). Game thread only: the menu
// queues requests and Tick drains them (debug::OnTick) at the end of a frame.
void FillDebugSnapshot(debug::Snapshot& s) noexcept;
void DebugRespawnPartner() noexcept;                 // what F7 does; also lifts a Remove hold
bool DebugRemovePartner() noexcept;                  // remove + hold spawning; true if there was one
bool DebugSetPartnerVisible(bool visible) noexcept;  // false: no live partner / SetVisible missing
void DebugSetEchoTuning(float delay_s, float offset_m) noexcept;
void DebugSetVerbose(bool on) noexcept;
void DebugLogPositions() noexcept;                   // what F6 does
bool DebugLogPartner() noexcept;
bool DebugPartnerPosition(double (&out)[3]) noexcept;  // live partner entity, else newest remote sample
// Collision tests (action collision 3): the partner body stands still `dist_m` ahead of Sam (along
// the camera's forward, else his last walking direction), facing him, holding its last pose; 0 = it follows the stream again.
// dz_m: the partner this far above (+) or below (-) Sam (tests: Sam on a ledge above it), -10..10.
// away (action collision 6): turned the way Sam faces, its back to him and the camera (back shots).
bool DebugPinPartner(double dist_m, double dz_m, char* msg, std::size_t cap, bool away = false) noexcept;

}  // namespace st::sync
