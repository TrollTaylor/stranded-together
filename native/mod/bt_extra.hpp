// Round 31 stage 8 (the round 31 build spec, Stage 8; docs/enemy-sync.md section 25): the kill look (CHANGED), RESPAWN (GO, built
// off) and SKY (CHANGED, built off).
//   kill look  bt_sync.cpp's partner-kill arm asks KillLookReason: 0x5414860D (no stats, PF 21) with bt_kill_look=1, else 0 (the
//              rain-gone 0x01322187). The applied kill's event (OnGazerEvent) and the stats 0x79 / 0x7A before / after are logged.
//   RESPAWN    the host: a pass-through detour on EnableRespawnGazers FUN_1429A76D0(scene) (bt_respawn=1, bt_sync 2); each call is
//              sent as 16 RESPAWN (scene hash = field id = crc32c of the scene GUID +0x98, GetUUIDHashOfGazerScene 0x1429A71E0).
//              A sandboxed guest (btrules::RespawnApplyGate) calls FUN_1429A76D0 for its own scene of that field (a gazer whose
//              +0x3A8 is its locator's scene: the message handler's compare at 0x14299F3C2), on the game thread under SEH.
//   SKY        while the partner is in BT trouble (HUNT, PULLED, CAUGHT or a catcher) within 150 m: SetStartControlFlags
//              FUN_142E1FED0(1) every 0.1 s; stop calling to restore (its 0.2 s timer clears +0x6518 bit 0).
// BTS1 types: 16 RESPAWN. Caps: kCapWorldApply 0x2000 on a sandboxed guest with bt_respawn=1 (bt_void offers it for craters too).
// Config: bt_kill_look (0; -> 1 only if two-games-bt-kill-look shows the death), bt_respawn (0; -> 1 after
//   two-games-bt-kill-respawn and one natural respawn), bt_sky (0; -> 1 only if bt-sky-sun passes; with it kSkyRealSaveOk).
// The rules of bt_feature.hpp hold: the game thread for every game call, guarded reads and writes (bt_sync_internal.hpp) under
// SEH, every site byte-checked before use (a failed group turns off only its part, with one log line), every record this module
// owns taken back on a load, link loss, the partner gone and EmergencyStop; a real-save guest (host_save=0) is never written
// (no respawn, no kill look; the sky waits for its gate); no game-server write. Every log line starts "[bt] extra:" and is
// rate-limited (btrules::extra::LineBudget).
#pragma once

#include <cstddef>
#include <cstdint>

#include "bt_feature.hpp"
#include "bt_world.hpp"

namespace st::bt::extra {

void Install(const Config& config) noexcept;  // bootstrap thread, once, after bt_sync's own Install
void OnHello(const PeerHello& peer) noexcept;  // game thread: the partner's hello (the relay turned on)
// Game thread: one BTS1 message of this module's types (header and size checked); type is its BtsType.
void OnMsg(std::uint8_t type, const std::uint8_t* msg, std::size_t n, double now_ms) noexcept;
void Tick(const TickCtx& ctx) noexcept;                    // game thread, every BT tick
void OnWorldEvent(WorldEvent e, double now_ms) noexcept;  // game thread: load start / end, link lost, partner gone
void EmergencyStop() noexcept;                            // any thread, synchronous: detours through, everything restored now
std::uint16_t Caps() noexcept;                            // the cap bits this game's hello offers for this feature
void Status(char* out, std::size_t cap) noexcept;         // one line for the BT status (no "[bt]" prefix)

// Game thread, the action "Bt sync" op 11 (routed by bt_sync.cpp), hidden runs only: v[1] 0 the test RESPAWN on the host (v[2] 0 the
// nearest gazer's field, else that field id); 1 the sky call for v[2] s (1..120, 0 = 10; bt_sky=1, any role); 2 v[2] 1 / 0: act as a
// real-save guest for the RESPAWN gate (restrictive only: the respawn is refused as on host_save=0).
bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;
// Game thread, bt_sync.cpp's partner-kill apply (ProcessKills, right before btworld::ArmKill): the +0xCC reason to write; 0 keeps
// the rain-gone 0x01322187 (btworld::ArmKill's default). 0x5414860D only with bt_kill_look=1 and the stats byte checks passed, never
// on a real-save guest (and never 0x74FB05EE: it credits the local player's stats). A non-zero answer opens the stats window.
std::uint32_t KillLookReason() noexcept;
// Game thread: KillLookReason() != 0 without arming its window (bt_sync's KillGuard asks it, to take a realized copy).
bool KillLookOn() noexcept;
// Not registered: the kill look needs nothing inside the gazer update (its apply is bt_world's ArmKill, its proof the event).
void AfterUpdate(std::uintptr_t actor) noexcept;
// Game thread: each event bt_world's detour recorded (the kill-look's applied kill and its death edge), as bt_sync.cpp drains them.
void OnGazerEvent(const btworld::Event& e, double now_ms) noexcept;

}  // namespace st::bt::extra
