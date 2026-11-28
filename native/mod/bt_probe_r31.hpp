// Round 31 stage 0 (docs/enemy-sync.md section 25; design: the round 31 design, section 3): bt_probe ops 15..36, the probes the
// round 31 BT features still need. Hidden runs only (automation::Enabled()); the ops marked (s) also need a sandboxed save
// (automation::SavesRedirected()). Their byte checks run on the first op ("[bt] r31 probe byte checks: n/m ok; ..."): a failed
// group refuses only its own ops. Every game read is guarded (partner::mem), every game call runs on the game thread from the
// action or the Tick under SEH, every detour is a pass-through (atomics only: no logging, no allocation; the Tick logs what they
// saw). EmergencyStop sends every detour straight through and ends every op. Each op undoes what it changed (18 the forecast,
// 19 its spots expire, 26 its tar record out, 29 the fade, 34 the sky bit). Op 27 (hunt_mirror) is not built (stage 9 research).
#pragma once

#include <cstddef>

#include "bt_world.hpp"

namespace st::bt::r31 {

// The action (game thread): op 15..36, a[0] the op itself, a[1..4] its arguments (bt_probe takes up to 5 numbers).
bool Command(int op, const double (&a)[5], char* msg, std::size_t cap) noexcept;
// Every BT tick (game thread): the timed ops, the detours' events, the loads.
void Tick(double now_ms) noexcept;
// Each bt_world event (game thread, from bt_sync's WorldEvents): op 36's kill outcome.
void OnWorldEvent(const btworld::Event& e) noexcept;
void EmergencyStop() noexcept;

}  // namespace st::bt::r31
