// Round 28 P4: the Odradek probe (odradek_probe=1, default 0; docs: enemy-sync.md section 19, debug-actions.md "Odradek
// probe"; scenario odradek-probe).
//
// One byte-checked detour on the scan state machine FUN_14282B530(inventory, dt) (called from the local player's
// MsgEntityUpdate at 0x1427D3A76). The detour samples the machine (inv +0x4850), the wave (+0x4EF8 / +0x4F00), the survey
// component (DSOdradekSurveyEffectComponent +0x244 / +0x245 / +0x246 / +0xC0 / +0x228) and the DecalManager centre
// (+0xE0) into a ring, and runs the two probe requests where the game's own scan code runs (Q8: the survey call is only
// safe inside that entity update):
//   - scan: the native trigger's writes (0x142805F80: +0x4C79 |= 0x20, +0x4934 = 0.4f, +0x493C = 4.0f) just before the
//     machine runs, only in state 0 / 5 with the Odradek present and the body ready (odra_rules.hpp ScanRequest);
//   - replay: after the machine ran, comp +0x246 = 1, comp +0xF0 = the scan direction, FUN_1433123C0(comp, origin): the
//     survey shown from another origin, with the guards of odra_rules.hpp AcceptScan (local state 0, survey idle, wave
//     non-null, 300 m, 1.5 s). Mode 1 first moves the finished wave there (FUN_141E0B020, only while wave +0x2BD != 0).
// Never: the AI stimulus FUN_14298B170 (enemies would see the flash), MULE manager +0x1B8, DSPlayerSystem +0x205D8.
// The probe sends no wire messages.
//
// Round 28 O3 (odradek_scan_sync=1, default 1; docs/enemy-sync.md section 19.1): "when either of us scans, the scan shows on both
// screens". The same detour, on the same worker thread:
//   - sender: after the machine ran, its own pulse edge 3 -> 4 (only the game's own scan makes it) is queued with the
//     DSPlayerSystem +0x205D8 origin and inv +0x4880; Tick sends it as a SOD1 SCAN on the reliable combat channel 6;
//   - receiver: Tick takes the partner's SOD1 messages (link::PopOdradek), admits a SCAN (odra_rules.hpp AdmitRemote: the
//     partner's HELLO, seq, 500 m) into a one-slot queue (the latest wins, 10 s TTL); the detour starts it after the machine
//     ran when the machine is idle (state 0: every step) or shows marks (state 5: without the DSPlayerSystem writes), else
//     leaves it queued (states 1..4, the wave still expanding, the survey shown). The start restarts the local wave at the
//     partner's origin (the pulse's wave steps, the trigger's sensor writes; odra_rules.hpp kStart*), so the machine's own
//     per-frame tail draws the grid / ring there. Never the arm / body animation, the AI stimulus, the marks, a
//     DSOdradek*Entity, or the state.
//   - capability: a SOD1 HELLO (caps odrarules::kCapOdradekScan) once per link generation; nothing is shown or sent before
//     the partner's HELLO carried it, and nothing is ever sent with the key off. Lines: "[odra] hello sent", "[odra] partner
//     hello", "[odra] partner scan sent / received / queued: <why> / shown origin=... / dropped: <why> / not sent: <why>".
//
// Threads: Install on the bootstrap thread (startup, with the other detours). The detour runs on the thread that updates
// the local player (logged: [U] whether that is the game thread): atomics, guarded reads and the two guarded game calls
// only, no logging, no allocation. Tick and Command on the game thread (player_sync Tick, the debug action queue): the
// ring is drained into "[odra] ..." lines there.
#pragma once
#include <cstddef>
#include <cstdint>

#include "game_api.hpp"

namespace st {
struct Config;
}

namespace st::odra {

// Bootstrap thread, once, after MH_Initialize: odradek_probe=1 or odradek_scan_sync=1 byte-checks every address the probe
// and O3 read or call and installs the detour. Logs "[odra] byte checks n/N ok" and "[odra] detour installed" (or why not).
void Install(const Config& config) noexcept;
bool Installed() noexcept;

// Game thread, every tick (player_sync Tick). local = the local player entity (or null); now_ms = the tick's clock.
void Tick(game::Entity* local, double now_ms) noexcept;

// Debug action "Odra" (odra), game thread. v[0] = op:
//   0             the state dump (one "[odra] dump ..." block);
//   1 <0 | 1>     the watch (the transition lines) off / on (on by default with the key);
//   2             a local scan (hidden runs or dev_tools only);
//   3 <m> [mode]  a replay m metres ahead of Sam (default 20) at Sam's height; mode 1 forces mode 1, 0 or absent = op 4's
//                 mode (an absent argument arrives as 0, so mode 0 cannot be forced per call: set it with op 4);
//   4 <mode>      the replay mode: 0 the survey component only, 1 the finished wave moved there first;
//   5 <m> [mask]  O3: a partner scan m metres ahead of Sam (default 20) at Sam's height, shown here through the O3 slot and
//                 start (no link; hidden runs or dev_tools; mask: odra_rules.hpp kStart*, absent = op 6's);
//   6 [mask]      O3: the start mask (absent / 0 = all, 127);
//   7             O3: the scan sync status line (also with odradek_probe=0 when odradek_scan_sync=1);
//   8 <m>         O3: a test pulse m metres ahead sent to the partner (SOD1 flag test; the key and the partner's HELLO).
bool Command(const double (&v)[3], char* msg, std::size_t cap) noexcept;
// Debug action "Odra replay" (odra_replay): the replay at (x, y, z); z 0 = Sam's height.
bool Replay(const double (&v)[3], char* msg, std::size_t cap) noexcept;

// The mod's emergency stop (dllmain GuardedEmergencyStop, after a per-frame tick fault): the detour passes straight
// through from then on (no game calls, no samples), pending scan / replay requests are dropped and new ones refused.
void EmergencyStop() noexcept;

}  // namespace st::odra
