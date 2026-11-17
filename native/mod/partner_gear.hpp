// Gear: the backpack, Odradek, BB pod and cargo on the partner's real body and on the self view's
// body (docs/real-partner-body.md section 12).
//
//   Sender: every 100 ms (game thread) the local player's gear is read fault-free: the backpack,
//           Odradek and BB pod entities of the inventory component (player +0x54C8) and every
//           cargo piece in the player's baggage carrier (player +0x54D0 -> carrier -> slots ->
//           stacks -> DSBaggage2 -> its entity in the baggage manager). Each item gets a kind, the
//           game's EDSBaggage2Model (found by matching the piece's model resource against the
//           baggage config's ModelResources), its colour variation, and a pose relative to an
//           anchor: a joint of Sam's skeleton (his pose's model matrices) or the backpack (cargo on
//           the back). The layout goes to the partner as STC1 (pose_wire.hpp) when it changes and
//           about once a second; the same frame dresses this player's own self-view body.
//   Receiver: one prop entity per item, the game's own cutscene props (demo_bak0_*, demo_bsm0_*,
//           ...) loaded by path + GUID like the bodies (realbody::BeginByPath), spawned invisible,
//           shown with the body, placed every frame by the pose detour on the body's own joints
//           (realbody::PublishGear) and removed with the body.
//
// Game thread only (Tick, SetPartnerGear, LocalFrame, DebugCommand); the detour placement lives in
// real_body.cpp.
#pragma once
#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "game_api.hpp"
#include "pose_wire.hpp"

namespace st::gear {

void Init(const Config& config) noexcept;
bool Enabled() noexcept;

// Per tick, after realbody::Tick and the partner body's move: reads our gear (every 100 ms),
// keeps the partner's and the self view's props in step with their bodies.
void Tick(game::Entity* local, bool local_active, double now_ms) noexcept;

// Sender schedule: true when a gear frame should go out now (the layout changed, a pose moved by
// more than 2 cm / 4 deg, or 1 s since the last one); `out` is the frame (seq filled by the caller).
bool TakeSend(double now_ms, wire::GearFrame* out) noexcept;

// The partner's gear (a received or echoed frame); nullptr: none (a new session, link loss).
void SetPartnerGear(const wire::GearFrame* f) noexcept;

// The per-frame sync faulted: the detour stops placing props (they stay where they are).
void EmergencyStop() noexcept;

// Debug (docs/debug-actions.md "Gear"): 0 log (our gear in full, the props), 1 <0/1> partner props
// off/on, 2 <0/1> self-view props off/on, 3 <n> hidden tests only: send / show only our first n
// cargo pieces (-1 = all), 4 <0/1> debug: our own gear on the debug body (real_spawn) on/off.
bool DebugCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

}  // namespace st::gear
