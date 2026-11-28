// Round 31 stage 4 (GO (list 0); the round 31 build spec; docs/enemy-sync.md section 25): TF: the partner's hunt felt here as tar (list 0).
// The module: our record in the hunter manager's hm+0xD0 list (add, find, remove under hm+0x130), driven by the received
// HUNT (bt_sync.cpp routes type 11 here too, after bt_hunt) (build spec, Stage 4).
// BTS1 types: none of its own (11 HUNT, also routed to bt_hunt). Caps: none (HUNT's centre and radius).
// Config: bt_tar (built 0; -> 1 after tar-feel-echo and two-games-bt-trouble-tar).
// The rules of bt_feature.hpp hold: the game thread for every game call, guarded reads and writes (bt_sync_internal.hpp) under
// SEH, every site byte-checked before use (a failed group turns off only this feature, with one log line), every record this
// module owns taken back on a load, link loss, the partner gone, the feature off and EmergencyStop; a real-save guest
// (host_save=0) is never written beyond cosmetic effects (this stage: no record at all, bt_tar_rules.hpp kRealSaveOk); no
// game-server write.
// The pure part (the gate, the inside test, the list edits, the cargo diff, the test op's arguments, the log limit) is
// bt_tar_rules.hpp (tests/bt_tar_tests.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

#include "bt_feature.hpp"
#include "bt_world.hpp"

namespace st::bt::tar {

void Install(const Config& config) noexcept;  // bootstrap thread, once, after bt_sync's own Install
void OnHello(const PeerHello& peer) noexcept;  // game thread: the partner's hello (the relay turned on)
// Game thread: one BTS1 message of this module's types (header and size checked); type is its BtsType.
void OnMsg(std::uint8_t type, const std::uint8_t* msg, std::size_t n, double now_ms) noexcept;
void Tick(const TickCtx& ctx) noexcept;                    // game thread, every BT tick
void OnWorldEvent(WorldEvent e, double now_ms) noexcept;  // game thread: load start / end, link lost, partner gone
void EmergencyStop() noexcept;                            // any thread, synchronous: detours through, everything restored now
std::uint16_t Caps() noexcept;                            // the cap bits this game's hello offers for this feature
void Status(char* out, std::size_t cap) noexcept;         // one line for the BT status (no "[bt]" prefix)

// Game thread, the action "Bt sync" op 12 (hidden runs; bt_sync.cpp routes it here before its own !g_sync refusal, as ops 8..11;
// v as the action gave it): v[1] v[2] = 0 0 a test HUNT 8 m ahead of Sam (r 6 m, 20 s, then CLEAR), x y one centred at
// (x, y, Sam's z), 0 1 / 0 2 this game counts as a real-save guest for the tar gate (on / off), 0 3 the running test ends now.
// The test HUNT goes through EncodeHunt / DecodeHunt into this module's view only (nothing is sent). False with the reason in
// msg when refused (bt_tar 0, not a hidden run, stopped, a test or a partner HUNT already up, no local player).
bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

}  // namespace st::bt::tar
