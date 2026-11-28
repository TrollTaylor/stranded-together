// Round 31 stage 3 (GO, RS built off; the round 31 build spec; docs/enemy-sync.md section 25): HUNT, PULLED's test path, RS (rain
// spots) and the markers glue.
// The module: the HUNT sender (btrules::HuntTracker over this game's hunter manager), the receiver (btrules::HuntView: its log
// lines, the tar marker), the rain-spot slots (8 static owner keys, shared with bt_catcher's catcher spot), PULLED's test path
// (bt_sync 8) and the markers glue (bt_markers.hpp).
// BTS1 types: 11 HUNT (also routed to bt_tar). Caps: kCapHunt 0x400 (it implies RS).
// Config: none for HUNT (on with bt_sync 1|2 in a two-game session and the partner's cap 0x400; bt_sync 1 sends and logs, never
// makes a spot); bt_rain (built 0; -> 1 if two-games-bt-trouble-dry passes).
// The rules of bt_feature.hpp hold: the game thread for every game call (the spot call FUN_142E14F60 and the raw rain read
// FUN_142E0C3F0, each under SEH; every other access a guarded read), every site byte-checked before use (a failed group turns
// off only its part, with one log line), every slot this module owns let go on a load, link loss, the partner gone, the relay off
// and EmergencyStop (a spot is never refreshed again and dies within its life by itself: 10 s, 3 s on a real save); a real-save
// guest (host_save=0) only gets spots beyond r + 110 m (never felt); no game-server write.
#pragma once

#include <cstddef>
#include <cstdint>

#include "bt_feature.hpp"
#include "bt_world.hpp"

namespace st::bt::hunt {

void Install(const Config& config) noexcept;  // bootstrap thread, once, after bt_sync's own Install
void OnHello(const PeerHello& peer) noexcept;  // game thread: the partner's hello (the relay turned on)
// Game thread: one BTS1 message of this module's types (header and size checked); type is its BtsType.
void OnMsg(std::uint8_t type, const std::uint8_t* msg, std::size_t n, double now_ms) noexcept;
void Tick(const TickCtx& ctx) noexcept;                    // game thread, every BT tick
void OnWorldEvent(WorldEvent e, double now_ms) noexcept;  // game thread: load start / end, link lost, partner gone
void EmergencyStop() noexcept;                            // any thread, synchronous: no spot refreshed again (atomic only)
std::uint16_t Caps() noexcept;                            // the cap bits this game's hello offers for this feature
void Status(char* out, std::size_t cap) noexcept;         // one line for the BT status (no "[bt]" prefix)

// Game thread, the action "Bt sync" op 8 (bt_sync.cpp routes it here; v as the action gave it), hidden runs only:
//   bt_sync 8 [secs]   the test HUNT: UP r 12 m round this game's Sam, PULLED level 1.10 from 2 s to 6 s, CLEAR after secs
//                      (default 20, 8..120); the partner logs it, marks it and (bt_rain 1 there) rains on it;
//   bt_sync 8 -1 1|0   this game's rain spots follow a real save's gate (3 s life, held within r + 110 m) or not again: the
//                      real-save rehearsal for a hidden run (whose guest is always sandboxed); it only makes the gate stricter.
// Refuses itself (false, with the reason in msg) when it cannot run; bt_sync.cpp has refused already after EmergencyStop.
bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

// Game thread, bt_sync.cpp's TroubleCensus (1 Hz): true while this game's TROUBLE should say PULLED (build spec: set ONLY by the
// test op bt_sync 8, from 2 s to 6 s of it; the natural hm+0x00 / hm+0x14 read is logged, never sent, until a run sees it).
bool PulledNow() noexcept;

// The rain-spot slots, shared with bt_catcher (stage 7's catcher spot goes through stage 3's RainSpotGate and slots).
enum class SpotUser : std::uint8_t { hunt = 0, catcher = 1 };
// Game thread: keep a kind-2 spot for that user at pos with radius_m (0 < r <= 400). Call it at least every 3 s while the source
// lives (a quiet user's spot ends); this module runs RainSpotGate and refreshes the spot every 2 s while the gate makes it.
// False: refused (bt_rain 0, bt_sync not 2, no partner relay, the spot checks or call failed, a bad radius, no free slot); the
// reason is this module's to log (its UP / status lines). True: the slot is booked (the gate may still hold it).
bool SpotWant(SpotUser who, const double (&pos)[3], float radius_m, double now_ms) noexcept;
// Game thread: no more refresh for that user's spot (it dies within its life by itself: 10 s, 3 s on a real save); logged as
// "rain spot off" at the next evaluation.
void SpotDrop(SpotUser who) noexcept;

}  // namespace st::bt::hunt
