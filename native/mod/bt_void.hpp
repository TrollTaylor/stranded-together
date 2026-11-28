// Round 31 stage 6 (GO (VB, CR); the round 31 build spec; docs/enemy-sync.md section 25): VB: the partner's voidout blast (faded flash) and CR: the crater record.
// The module: the blast sequence and its queue, the effector-creator detour and the fade, the sender's FUN_142A003D0
// pass-through, the crater add and the CraterApplyGate glue (build spec, Stage 6). Namespace voidout: void is a keyword.
// BTS1 types: 13 VOID2 (VOID v1 still goes from bt_sync.cpp). Caps: kCapVoid2 0x1000, kCapWorldApply 0x2000 (craters applied here).
// Config: bt_void (built 1; 1 -> 2 after two-games-bt-void-crater), bt_void_flash_m (150).
// The rules of bt_feature.hpp hold: the game thread for every game call, guarded reads and writes (bt_sync_internal.hpp) under
// SEH, every site byte-checked before use (a failed group turns off only this feature, with one log line), every record this
// module owns taken back on a load, link loss, the partner gone, the feature off and EmergencyStop; a real-save guest
// (host_save=0) is never written beyond cosmetic effects; no game-server write.
// Built (stage 6): bt_void.cpp says how; the pure glue (caps, the test op, the queues, the fade, the near check) is
// bt_void_rules.hpp. Log lines start "[bt] void:" (rate-limited). bt_sync.cpp calls every function below (bt_feature.hpp says when).
#pragma once

#include <cstddef>
#include <cstdint>

#include "bt_feature.hpp"
#include "bt_world.hpp"

namespace st::bt::voidout {

void Install(const Config& config) noexcept;  // bootstrap thread, once, after bt_sync's own Install
void OnHello(const PeerHello& peer) noexcept;  // game thread: the partner's hello (the relay turned on)
// Game thread: one BTS1 message of this module's types (header and size checked); type is its BtsType.
void OnMsg(std::uint8_t type, const std::uint8_t* msg, std::size_t n, double now_ms) noexcept;
void Tick(const TickCtx& ctx) noexcept;                    // game thread, every BT tick
void OnWorldEvent(WorldEvent e, double now_ms) noexcept;  // game thread: load start / end, link lost, partner gone
void EmergencyStop() noexcept;                            // any thread, synchronous: detours through, everything restored now
std::uint16_t Caps() noexcept;                            // the cap bits this game's hello offers for this feature
void Status(char* out, std::size_t cap) noexcept;         // one line for the BT status (no "[bt]" prefix)

// Game thread, the action "Bt sync" op 9, hidden runs only (routed by bt_sync.cpp): bt_sync 9 <m> <mode> sends a test VOID2 (no
// local blast): mode 0 m ahead of this game's Sam, 1 m from the partner's body towards this Sam, +2 a catcher crater (r 40)
// wanted; bt_sync 9 <1|0> 4 turns this game's real-save stand-in on / off (bt_void's gates as on a host_save=0 guest).
bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;
// Game thread: each voidout in this game (bt_sync's FUN_14299B1B0 detour, drained in SyncTick, after VOID v1 is sent): VOID2's
// source. Every voidout comes here, the relay on or not.
void OnLocalVoid(const LocalVoid& v, double now_ms) noexcept;

}  // namespace st::bt::voidout
