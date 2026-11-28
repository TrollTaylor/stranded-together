// Round 31 stage 2 (CHANGED; the round 31 build spec; docs/enemy-sync.md section 25): WX: the host's forecast on a sandboxed guest.
// The module: the forecast-job detour FUN_142E183F0 and the pre-job detour FUN_142E17CF0, the seqlock snapshots, WXROW / WXBEAT
// send and receive, the guest's saved own rows and their write-back (build spec, Stage 2).
// BTS1 types: 9 WXROW, 10 WXBEAT. Caps: kCapWx 0x100 (this game sends: the host its rows and beat, a guest its beat),
// kCapWxApply 0x200 (a sandboxed guest with bt_weather 2 and bt_sync 2 applies the host's rows).
// Config: bt_weather (built 1: sent and compared only; -> 2 after the gate two-games-bt-weather).
// How it works:
//   * both games: the job detour copies this game's own effective rows (row + 0x2C) after the job, every 250 ms, into a seqlock
//     the Tick reads; the Tick sends WXBEAT every 2 s (slot, clock, crc, the raw rain and region at Sam).
//   * the host: a new gen (the rows, mask or slot changed) goes as WXROW in 3 chunks at once, and again every 10 s; the clock field
//     is the host's slot phase (clock - last), not its clock (the clocks drift between games).
//   * a sandboxed guest at level 2 (WxGate): before the job, the detour saves her own masked rows once (+0x04..+0x5B), then writes
//     base = the host's eff, the window from her own clock and the phase, forced 0 and bits &= ~5; the first apply sets the
//     instant grid +0x6515 once; after the job, bits' bit 0 off again. Any stop (link lost, partner gone, a load start, stale,
//     another slot, EmergencyStop) writes her saved rows back at the next run, from the pre-job detour (the game thread, the job
//     idle) or the job detour, before the original; the Tick writes it only when no detour did for 3 s and the job is idle.
//   * a guest on her own real save (host_save=0): never written (her detour has no write path armed); the compare log only.
// The rules of bt_feature.hpp hold: the game thread for every game call (the region and rain calls, under SEH), guarded reads and
// writes (bt_sync_internal.hpp), every site byte-checked before use (a failed check turns off only WX, with one log line), every
// record this module owns (the guest's rows) taken back on a load, link loss, the partner gone, the feature off and EmergencyStop;
// no game-server write. The detours (any thread) use atomics, the seqlocks and guarded reads / writes only.
#pragma once

#include <cstddef>
#include <cstdint>

#include "bt_feature.hpp"
#include "bt_world.hpp"

namespace st::bt::wx {

void Install(const Config& config) noexcept;  // bootstrap thread, once, after bt_sync's own Install
void OnHello(const PeerHello& peer) noexcept;  // game thread: the partner's hello (the relay turned on)
// Game thread: one BTS1 message of this module's types (header and size checked); type is its BtsType.
void OnMsg(std::uint8_t type, const std::uint8_t* msg, std::size_t n, double now_ms) noexcept;
void Tick(const TickCtx& ctx) noexcept;                    // game thread, every BT tick
void OnWorldEvent(WorldEvent e, double now_ms) noexcept;  // game thread: load start / end, link lost, partner gone
void EmergencyStop() noexcept;                            // any thread, synchronous: detours through, everything restored now
std::uint16_t Caps() noexcept;                            // the cap bits this game's hello offers for this feature
void Status(char* out, std::size_t cap) noexcept;         // one line for the BT status (no "[bt]" prefix)

}  // namespace st::bt::wx
