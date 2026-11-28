// Round 31 stage 7 (CHANGED (CS); RW DROPPED; the round 31 build spec; docs/enemy-sync.md section 25): CS: the partner's catcher state and edges (RW: crystals DROPPED).
// The module: the 4 Hz record diff (primary), the notify detour FUN_142A1C0E0 (the hash for the log; atomics only), CATCH send
// and receive, the catcher spot through bt_hunt's SpotWant / SpotDrop, the markers, the REWARD refusal line (build spec, Stage 7).
// BTS1 types: 14 CATCH, 15 REWARD (refused: crystals not given: not built this round). Caps: kCapCatch 0x4000 (kCapReward 0x8000 never: RW is DROPPED).
// Config: bt_catcher (1 already), bt_crystals (stays 0).
// The rules of bt_feature.hpp hold: the game thread for every game call, guarded reads and writes (bt_sync_internal.hpp) under
// SEH, every site byte-checked before use (a failed group turns off only this feature, with one log line), every record this
// module owns taken back on a load, link loss, the partner gone, the feature off and EmergencyStop; a real-save guest
// (host_save=0) is never written beyond cosmetic effects; no game-server write.
// Built (round 31 stage 7): the sender reads only (records and a pass-through notify detour); the receiver draws, logs and asks
// bt_hunt for the catcher's rain spot (RS, bt_rain). No game write of its own; nothing to restore (the spot and the marker expire
// by themselves). Gate: two-games-bt-trouble-catcher (leg C); bt_catcher=1 stays the default.
#pragma once

#include <cstddef>
#include <cstdint>

#include "bt_feature.hpp"
#include "bt_world.hpp"

namespace st::bt::catcher {

void Install(const Config& config) noexcept;  // bootstrap thread, once, after bt_sync's own Install
void OnHello(const PeerHello& peer) noexcept;  // game thread: the partner's hello (the relay turned on)
// Game thread: one BTS1 message of this module's types (header and size checked); type is its BtsType.
void OnMsg(std::uint8_t type, const std::uint8_t* msg, std::size_t n, double now_ms) noexcept;
void Tick(const TickCtx& ctx) noexcept;                    // game thread, every BT tick
void OnWorldEvent(WorldEvent e, double now_ms) noexcept;  // game thread: load start / end, link lost, partner gone
void EmergencyStop() noexcept;                            // any thread, synchronous: detours through, everything restored now
std::uint16_t Caps() noexcept;                            // the cap bits this game's hello offers for this feature
void Status(char* out, std::size_t cap) noexcept;         // one line for the BT status (no "[bt]" prefix)

// Game thread, the action "Bt sync" op 10 [m] (hidden runs: the test catcher m metres ahead of Sam, default 20: SPAWN Quad 100 /
// 60 / 20 %, then GONE over 4.5 s, flagged a test; routed by bt_sync.cpp).
bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

// Game thread: true while a partner catcher from CATCH is kept here (this module puts the catcher marker at its bar anchor):
// bt_sync.cpp's TROUBLE CATCHER handler should then not put its own catcher marker (the same marker key, a different anchor).
bool OwnsCatcherMark() noexcept;
// Game thread: the class + 1 of this game's first live catcher the sender tracks (0 none / not tracking): TROUBLE's catcher class.
std::uint8_t LocalClass1() noexcept;

}  // namespace st::bt::catcher
