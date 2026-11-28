// Round 31 stage 5 (CHANGED; the round 31 build spec; docs/enemy-sync.md section 25): GM: the gazer mirror (realized copies, 25-60 m).
// The partner's gazer that senses the partner (her own game's NEAR / react / caught) reacts on this game's REALIZED copy of it
// (+0x24 == 2: an entity and a DSGazerComponent here, this game's player 25-60 m from it, in rain):
//   send     4 Hz (game thread): GZST2 (type 12) of this game's own gazers sensing this game's player, nearest first, at most 6,
//            while the partner's hello offers kCapGzst2 (read only);
//   receive  game thread: the latest entry per locator; the gate (bt_mirror_rules.hpp Gate, 4 Hz) picks the mirrors (at most 6);
//   writes   kind (+0x214) and the agitation pair (comp+0x80 = 1, +0x7C = 10, the gazer's own values saved first) right after the
//            gazer's update, inside bt_world's detour (AfterUpdate, kAfterMirror); the yaw toward the partner's body at most
//            90 deg/s from Tick through game::TrySetTransform (bt_probe 23's proven call, the same game thread as the update);
//            every write first re-checks its target (the id, state 2, the entity, the component's back pointer);
//   drops    FUN_1429C49D0 (FOUND 0x6221E6CB, catch 0x5A7E4B1D) and FUN_14299EFB0 (caught 0x682DFB3B) for mirrored gazers only,
//            any thread, atomics only; everything else (0x68A02F9F) passes.
// A real-save guest (host_save=0) gets the yaw only (no kind, agitation or drops); so does every game whose drop hooks are not in.
// Released on 2 s with no GZST2, a load, link loss, the partner gone, the gate, the per-write re-check, EmergencyStop; the agitation
// pair is written back on a re-checked component and read back (the drops stay on for that gazer until it reads back, 5 s).
// BTS1 types: 12 GZST2. Caps: kCapGzst2 0x800 ("I speak GZST2": sent with bt_gazer_mirror >= 1 and the byte checks passed).
// Config: bt_gazer_mirror (built 0; -> 1 after two-games-bt-gazer). Every log line starts "[bt] mirror:".
#pragma once

#include <cstddef>
#include <cstdint>

#include "bt_feature.hpp"
#include "bt_world.hpp"

namespace st::bt::mirror {

void Install(const Config& config) noexcept;  // bootstrap thread, once, after bt_sync's own Install
void OnHello(const PeerHello& peer) noexcept;  // game thread: the partner's hello (the relay turned on)
// Game thread: one BTS1 message of this module's types (header and size checked); type is its BtsType.
void OnMsg(std::uint8_t type, const std::uint8_t* msg, std::size_t n, double now_ms) noexcept;
void Tick(const TickCtx& ctx) noexcept;                    // game thread, every BT tick
void OnWorldEvent(WorldEvent e, double now_ms) noexcept;  // game thread: load start / end, link lost, partner gone
void EmergencyStop() noexcept;                            // any thread, synchronous: detours through, everything restored now
std::uint16_t Caps() noexcept;                            // the cap bits this game's hello offers for this feature
void Status(char* out, std::size_t cap) noexcept;         // one line for the BT status (no "[bt]" prefix)

// Inside bt_world's gazer update detour, right after the original update, on the game thread, for every gazer updated: registered
// with btworld::SetAfterUpdate(btworld::kAfterMirror, &AfterUpdate) while any mirror writes, cleared (nullptr) when none does.
// Quick, guarded writes only, no logging, no allocation (bt_world.hpp's rules). btworld::EmergencyStop clears every slot.
void AfterUpdate(std::uintptr_t actor) noexcept;
// Game thread: each event bt_world's detour recorded (a death edge, an armed kill's outcome), as bt_sync.cpp drains them.
void OnGazerEvent(const btworld::Event& e, double now_ms) noexcept;

// Game thread, the test op `bt_sync 13 <m>` (hidden runs only; bt_sync.cpp routes op 13 here; docs/debug-actions.md "Bt sync"): m 0 logs
// the status, 1 makes this game mirror as a real-save guest would (the yaw only: the gate's host_save=0 leg in a hidden run,
// whose saves are always redirected), 2 back to normal. False with *msg on a refusal.
bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

}  // namespace st::bt::mirror
