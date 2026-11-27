// BT sync B0 (round 28 P3): the BT field census. Read only, logging only: what each game's gazer manager holds (its live
// gazers, their fields and the fields' killed lists), whether the hunter manager has an active hunter area (the local player
// was noticed), the catchers, and the concentration meter, as numbers both games can compare (the field id and the gazer ids
// are the game's own crc32c of static archive GUIDs; bt_rules.hpp).
//
// The reads (all through partner::mem::Read: a stale pointer reads as "unreadable", never a fault):
//   gazer manager [0x147BEBEC0]: +0x78 i32 count / +0x80 u32 actor ids (the live gazers, under the SRW lock +0x98; read
//     without it, 1 Hz, so a list being resized can read short once), +0x230 float concentration meter, +0x234 its timer,
//     +0x23B "player not perceivable", +0x1750 the field map (data, capacity +0x175C, 0x18-byte slots: +0 hash|0x80000000,
//     +8 field id, +0x10 the field state: +0 killed, +8 killed gazer ids, +0x10 timers, +0x20 total locators)
//   a gazer actor (from the actor registry [0x147BBB8C0] by id, the registry's groups read directly, no game call):
//     +0x08 id, +0x0C, +0x18 locator (+0x80 scene, +0x88 GUID), +0x20 in use, +0x24 state, +0x28 transform, +0x1B4 flags
//   hunter manager [0x147BECBD0]: +0x138 resource, +0x140 the active hunter area's entity (non-null: noticed), +0x240 spawns
//   catcher manager [0x147BEE060]: +0x88 count, +0x90 records (0x110 each: +0 entity ref, +0x20 actor id, +0xF0 inactive,
//     +0xF4 health)
// Every address is byte-checked on first use (a group of reads off with one "[bt] byte check FAILED" line when its code
// differs). Logs "[bt] ...".
//
// B1 (round 28 lane B1; bt_sync=1|2 in session.ini, default 2; docs/enemy-sync.md section 21): the BT field sync. Each game keeps
// its own BT field (nothing is spawned or moved for the partner); the census above, once a second, feeds the BTS1 messages on
// channel 6 (bt_rules.hpp: HELLO, FIELD, RESET, NOTICED, VOID), keyed by the field id and the locator GUID hash (the actor ids
// differ per game and per stream-in):
//   FIELD    the records' summary and the 6 biggest live fields, compared on the other game ("[bt] field compare #n: ...")
//   NOTICED  this game's own gazer raising its near bit 0x1000 for this game's player (or the hunter area appearing): logged on
//            the partner ("[bt] partner sensed (near): gazer loc ...") with a toast; no game effect there (B2 would add one)
//   VOID     a pass-through detour on the voidout FUN_14299B1B0 (installed at start-up, prologue byte-checked) records each
//            voidout; the partner logs it ("[bt] partner voidout: ...", notice only, no crater there)
//   RESET    the records all turning unset at once (ResetAllGazers, e.g. dev_enemies 3), polled; with bt_sync=2 the guest applies
//            the host's RESET through ResetAllGazers (byte-checked, this thread, SEH-guarded) when its record set matches. The
//            host owns the field state: a guest's RESET is only logged on the host.
// Nothing is sent before the partner's SEH1 beacon (channel 6 is up on its side) and nothing is taken before its BTS1 hello,
// so a partner with bt_sync=0 or an older build gets nothing and sends nothing ("[bt] sync: no partner hello after 15 s").
//
// Threads: Install on the bootstrap thread; Tick, Command and SyncCommand on the game thread (st::debug::OnTick / the debug
// actions); the voidout detour on whatever thread the game calls it (it only copies into a locked ring of 8).
#pragma once
#include <cstddef>

namespace st {
struct Config;
}

namespace st::bt {

// Bootstrap thread, once: bt_probe=1|2 in session.ini starts the 1 Hz census at the first tick in a world (2: also one
// full dump when gazers first appear). bt_probe=0 (the default): nothing runs until the action asks (hidden runs or the ini).
void Install(const Config& config) noexcept;

// Game thread, every tick: the census at its interval while on. Costs one flag test while off.
void Tick(double now_ms) noexcept;

// Game thread, the debug action "Bt probe": v[0] = 0 stop (the totals line), 1 the census once a second (v[1] = seconds
// between censuses, 0 = 1 s), 2 one full dump now (every gazer, every field record, the catchers, the hunter manager).
// Allowed with bt_probe != 0 in session.ini or in a hidden run (automation=1); refused otherwise, with the reason.
bool Command(const double (&v)[3], char* msg, std::size_t cap) noexcept;

// Game thread, the debug action "Bt sync": v[0] = 0 the status line (the counters, "[bt] sync status (action): ..."), 1 FIELD at
// the next census, 3 a test VOID and 4 a test NOTICED to the partner (hidden runs only). Refused with bt_sync=0.
bool SyncCommand(const double (&v)[3], char* msg, std::size_t cap) noexcept;

// Any thread (dllmain's emergency stop): the voidout detour passes straight through, nothing more is sent or applied.
void EmergencyStop() noexcept;

}  // namespace st::bt
