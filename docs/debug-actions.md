# Debug actions (game side)

The in-game menu's game actions and the "Game" readout on its Status tab. Code:
`native/mod/game_actions.{hpp,cpp}` (`st::game`), wired into the menu through
`native/mod/debug_actions.cpp` (action queue, registry, snapshot) and
`native/mod/overlay/overlay_menu.cpp` (layout). See [debug-menu.md](debug-menu.md) for the
menu itself.

**Status: nothing in this file has run inside ds.exe yet.** Every address comes from static
analysis of ds.exe DC (PE timestamp 1706443442, base `0x140000000`) with `tools/re/dsre.py`;
the Ghidra decompiler was not available for this round, so meanings marked *inferred* are
read from disassembly or from the earlier research notes. The first in-game run should go
through the checklist at the end.

## Safety model

DS's crash reporter fires on any access violation in the process, even one the mod
catches, so this code is written not to fault:

* **Game thread only.** Every action is a request the menu queues; `st::debug::OnTick`
  (called from `st::sync::Tick`, the game's main thread) runs `game::ActionsTick` and then
  the queued handlers. Game functions are only ever called from there.
* **Reads never fault.** Game memory is read with `ReadProcessMemory` on our own process
  (fails instead of raising), in the same style as `partner_source.cpp` (`Rd`/`Q`/
  `DecodeGetter`). Objects are validated by vtable or RTTI type before use.
* **Every address is byte-checked once.** On the first tick `game_actions` compares the
  exact ds.exe bytes at each known RVA (table `kChecks` in `game_actions.cpp`). Every global
  and field offset the code uses appears inside one of the checked byte ranges (the
  RIP-relative displacement that loads a global is part of the checked bytes). Then the
  data checks: `PlayerGame` vtable slot `+0xA8`, the `GameWorldTimeState`,
  `DSEchoComponent`, `DSEchoComponentResource` and `DSPlayerParamComponent` RTTI
  descriptors. A mismatch disables only the features that depend on it. The log says:

  ```
  [game] actions: 28/28 byte checks ok; enabled: teleport, unstuck, health, heal, time of day, area, game state, load mask, voice ping; DISABLED: none
  ```

  and, on a mismatch, one `[game] actions: <check> at ds.exe+0x...: bytes differ` line. The
  Status tab shows `Disabled: <feature>: <check>` in red. There is no signature-scan
  fallback: a different ds.exe build fails closed.
* **Per-object checks at call time.** Sam's vtable slots (`+0x78`, `+0x80`, `+0x98`,
  `+0xD8`) must point at the checked DSPlayerEntity functions before the matching action
  runs; the Player object must have the `PlayerGame` vtable; components are matched by RTTI.
* `__try` around each game call stays as a last resort only (a fault there would still
  trigger the crash reporter). A fault disables that feature for the run.

## Diagnostics (Status tab, "Game")

Read every snapshot (2 Hz, every frame while the menu is open), reads only.

| Row | Source | Notes |
|---|---|---|
| game state | `DSGameState` `[0x147BF6E88]`: `+8` current flags, `+0x10` requested by script | Bits named after `EDSGameStateForScript` (RTTI enum `0x144C66648`): 1 TerminalOperation ... 0x40 Cutscene ... 0x400000 Loading ... 0x4000000 Setup ... 0x40000000 DirectIntoPrivateRoom; higher bits are internal, shown as `bitN`. "(stopping event)" = `(+8 \| +0x10) & 0x184F407FEC7F7F` (the mask of `IsStoppingGameEvent` `0x142DD3760`). That the bits equal the enum is *inferred* (high confidence: the Setup bit matches the `+0x5E0 == 2` phase). |
| load phase | `DSGameState+0x5E0` (`0x142E0E950`: `== 2`) | 2 = the post-load "Setup" settle phase. |
| load mask | `GameModule` `[0x147BE6318]` `+0x44C` (u32), `+0x450` (load-reason byte) | Recomputed every frame by `0x142627640`; non-zero = loading/suspended. Bit names *inferred*: 0x2 loading/area change, 0x4/0x8 pending, 0x40 streaming not ready, 0x80 streaming settle, 0x100 saving. |
| area | `[0x147AE90E8]`: if byte `+0xA8` is set, `u16 +0x48` | The read-only fast path of `GetCurrentArea` (`0x14262A860`). Outside an area change the game computes the area through hash lookups, which the mod does not call, so the row says n/a. Values: EDSArea (Area00=0, Area01=100 ... Beach01=600, Frange01=700, Nm01=1100 ..., Empty=65535). |
| time of day | `GameWorldTimeState` = `[GameModule+0x58]` (vtable `0x143F19A00`, RTTI `0x144E6B540`): `+0x20` float hours, `+0x28` day counter (*inferred*), `+0x2C` bool EnableDayNightCycle | `+0x58` and `+0x2C` confirmed by the game's SetTimeOfDay event `0x1435720E0`. |
| health | `[[Sam+0x54C0]+0x2E0]` `+0xC0` health, `+0xC4` max (floats) | Same reads and clamp as DSPlayerEntity GetHealth/GetMaxHealth (`0x1427105E0`, `0x142710F60`, vtable `+0x78`/`+0x80`). |
| player system | `DSPlayerSystem` `[0x147BE7980]` `+0x206D0` bit 0 | Shown only when a pushed transform is pending. |
| teleport | all teleport preconditions except the sequence call | "ready" or the first reason it would be refused. |
| last teleport | the newest teleport and its +2 s verification | |

**Copy diagnostics** (Status tab button, action `copy_diagnostics`): builds a fresh
snapshot on the game thread, writes the text report to the log (`[debug] diag: ...` lines)
and to the clipboard (`CF_UNICODETEXT`). **Log status** (Settings) now also logs the game rows.

## Actions

Each action is one `st::debug::Action` value plus one row in `kRows`
(`debug_actions.cpp`); the handler runs on the game thread and writes one result line,
shown next to the button (result chip) and under Settings > Recent actions, and logged as
`[debug] action #N <label>: [FAILED: ]<text>`.

### Teleport (`teleport`, `teleport_to_partner`)

`game::TeleportLocalPlayer(x, y, z, mode, msg, cap)`; the old three-argument
`TeleportLocalPlayer(x, y, z)` still exists (mode 1, no message). The menu's coordinate
button, saved positions (button and double-click) and "Teleport to partner" use mode 1;
"Snap to ground (x, y)" uses mode 2 with z = 500.

Mechanism: **`PlayerGame::PlaceOn`** `0x142634F00(Player*, const WorldTransform*, int mode)`
(PlayerGame vtable `0x143D5C860` slot `+0xA8`), the function GameModule's own update calls
after every load (`0x14263FA90`: `player->vt[0xA8](player, &entity->xf, ...)`). It sends
MsgDismount to Sam, then `0x14235A010(e, wt, ground, 0)`, which writes the transform under
the entity lock and sends MsgEntityTeleported synchronously to the model and every
component. That message is what keeps the mover (`0x1426A4780`) from writing the old
physics position back, the reason a plain `Entity::SetWorldTransform` on Sam snaps back.

* **Mode 1** (`TeleportMode::place`): ground probe on: a ray from z+2 m to z-2 m (Humanoid
  vt+0x20 `0x143737930`), nearest hit + 0.001; keeps z when nothing is hit. Use when z is
  known.
* **Mode 2** (`TeleportMode::snap_to_ground`): as mode 1, plus `Player+0x16C = 1`; the next
  GameModule update (`0x14263FDD1`) sees it, casts one ray z+2000 .. z-2000 (`0x142644C20`)
  and puts Sam on the topmost surface, then clears the flag once `GameModule+0x44C == 0`.
  Checked in the disassembly: in normal play (suspend mask 0) the snap runs on the very
  next update. It can land on a roof or overhang.
* Sam keeps his current rotation.
* The Player is `[Sam+0x560]` (DSPlayerEntity vt+0xD8 `0x1427115B0`), and must satisfy
  `[Player+0x70] == Sam` (PlaceOn moves `[Player+0x70]`).

Preconditions, all checked right before the call (first failure is the result text):

1. teleport feature enabled (all byte checks above passed);
2. target finite and inside |x|,|y| < 50 km, |z| < 10 km;
3. at most one teleport per 500 ms;
4. a local player exists (`GetLocalDSPlayerEntity`), its vtable has DSPlayerEntity's
   GetPlayer at `+0xD8`;
5. Sam awake: `(Sam+0x88 & 0x100) == 0`;
6. `GameModule+0x44C == 0` (not loading or suspended);
7. `((DSGameState+8 | +0x10) & 0x184F407FEC7F7F) == 0` (no menu, cutscene, knot space,
   fast travel, game over, faint, loading, ...);
8. `DSGameState+0x5E0 != 2` (not in the post-load setup phase);
9. `Sam+0x70 == 0` (no parent entity; *inferred* to mean "not attached to a vehicle");
10. byte `Sam+0x678 == 0` (the game's own `PopWorldTransform` `0x1426E9940` refuses
    otherwise; meaning unknown);
11. Player present, vtable `0x143D5C860`, `[Player+0x70] == Sam`, `[Player+0x16C] != 1`
    (no ground snap pending);
12. `(DSPlayerSystem+0x206D0 & 1) == 0` (no pushed transform; the game's later Pop would
    warp Sam back);
13. `ControlledEntity_ExportedIsBoundToSequence(Sam)` (`0x1434FD620` -> `0x142381700`,
    SequenceComponent `+0x50` under the entity lock) is false. This is the only
    precondition that calls game code; it is skipped by the Status-tab readiness row.

Verification: the call logs where Sam is right after it, then
`[game] teleport check +0.5 s` and `+2 s` lines with Sam's position, horizontal distance to
the target, dz, his flags, the load mask, and whether the entity changed. "ok" means within
3 m horizontally (and 5 m in z for mode 1). The +2 s result is the Status tab's
"last teleport".

Known risks: collision not streamed in at a far target (Sam may fall; mode 2 repeats its
snap while loading), carts left behind, leaving a mission area may fail the mission, an
autosave may follow, the partner sees a jump (the sync snaps beyond 25 m).

### Unstuck (`unstuck`)

Teleports (mode 1, same preconditions) to the safe ground transform the game itself keeps in
`DSPlayerParamComponent` = `[Sam+0x54C0]` (RTTI `0x144BC89D0` checked): transform A at
`+0x81F0` (pos, rotation at `+0x8208`) valid when byte `+0x7F73 & 0x20`, transform B at
`+0x8230` (rotation `+0x8248`) valid when `& 0x40`. The choice mirrors the game's picker
`0x1427A4A40` (B, unless A is valid and nearer to Sam), which its "return to safe
position" warp `0x1426EA4B0` uses. Reads only; the picker is not called.

### Heal (`heal`, Player tab)

`Entity_ExportedHeal` `0x142361BD0(Sam, amount)` -> `0x14234C310`, which builds the heal
message and calls Sam's vt+0x98 = `0x142713AE0` -> `0x1427023C0(healthObject, amount, ...)`.
Checked: Sam's vt `+0x98`, `+0x78`, `+0x80`. Preconditions: steps 4-8 and 13 of the teleport
list, plus `DSPlayerSystem` `[0x147BE7980]` and `[0x147C0AFF8]` non-null: the gate
`0x1426A05F0` that `0x1427023C0` calls dereferences both (`+0x206D1`, `+0x8170C`) without a
null check. The button heals to max (`amount = max - health`); "already at full health" counts as
success. The result reads health before/after. The game itself may refuse (inside
`0x1427023C0`, DSPlayerSystem state `0x1426A05F0` and game-state bit 25); the result then
says "unchanged". Effect untested.

### Time of day (`set_time_of_day`, `set_day_night_cycle`, Player tab)

* Slider + **Apply**: `GameWorldTimeState::SetTimeOfDay` `0x143631F90(tod, hours, 0.0)`,
  transition 0 = instant (pure arithmetic on `+0x20/+0x28/+0x30/+0x34`; handles the day
  rollover). The game's SetTimeOfDay event `0x1435720E0` calls the same function. Hours
  must be in [0, 24); the result reads the new value back.
* **Freeze time**: writes byte `+0x2C` EnableDayNightCycle (0 = frozen) with
  `WriteProcessMemory` and reads it back. The game may set it again on its own (scripted
  events); the Status tab shows the live value. Whether a frozen cycle also stops weather
  or sky updates is unknown.

### Voice ping (`voice_ping`, Partner tab)

`DSEchoComponent::Play` `0x142BBF3A0(comp, idx, tableSel=0, delay=0.05)` on the partner
entity's echo component: one voice line (with subtitle) from the partner's position. The
partner pointer is the live one from `sync::FillDebugSnapshot` (the Partner tab's entity; 0
when there is none). Before the call:

* the entity is readable, components `[e+0x90]` count 0..512, data `[e+0x98]`;
* a component with RTTI `DSEchoComponent` (`0x144C23EC0`) whose owner `+0x48` is the entity;
* its resource `+0x30` has RTTI `DSEchoComponentResource` (`0x144C23E00`);
  `EchoSentenceResources` count `i32 +0x20` in 1..4096 and the chosen entry of `+0x28`
  non-null. **The game's update reads `data[0]` with no check when the count is 0** (it
  would fault), hence this check;
* state `+0x64` in 0..3, `DSGameState` and `GameModule` non-null (Play dereferences both).

Play returns 0 (and changes nothing) when the game state blocks talking
(`G & 0x184F407FEC7F7F`, or the GameModule loading check, unless bit 25); the result then
names the blocking state flags. Line index: `Request.i` = line number from 1, 0 = next in
turn. A sleeping partner (flag 0x100) accepts the request, but its component update
probably does not run; the result says so. The partner may be a non-echo entity (for
example the welcome hologram): then it reports "no DSEchoComponent". With a hologram or
marker body and `partner_voice=1`, the echo that talks is player_sync's separate voice
entity, which this accessor does not expose, so the ping reports "no DSEchoComponent" there
too (needs a player_sync accessor for the voice entity).

### Hologram catalog and clip survey (`holo_catalog`, `clip_survey`; no buttons, scenarios only)

* `action holo_catalog <start> <count> [distance_m]`: spawns the collectors DB's Type 4 and Type 20 items one at a time in front of Sam. Each item is logged, screenshotted (`holo-<HashId>.png`) and removed (`native/mod/holo_catalog.cpp`).
* `action clip_survey`: logs every loaded animation clip that has root motion or a walk-like name, and marks the clips that fit Sam's skeleton.

Both use only the partner hologram's byte-checked spawner calls and ReadProcessMemory reads. What they found is in [character-catalog.md](character-catalog.md).

### Animated-body census and pose memory (`anim_census`, `pose_dump`, `pose_scan`, `look_at_body`, `snap_teleport`; scenarios only)

`native/mod/anim_census.cpp`. Reads only (ReadProcessMemory), except the byte-checked teleport.

* `action anim_census <radius_m> <tag> <scan>`: every world-list entity with an AnimationManager, a Humanoid type, AIEntityType Humanoid or a 77-joint skinned body (type, resource, components, skeleton, animation manager, flags, position), the akukan Sam resource, then optionally a worker-thread memory scan (bit 1: AnimationManagers/Humanoids outside the world list; bit 2: every `DSDeliveryPointInfoResource` and MULE camp locator with its world position). Ends with `[census] done tag=<tag>`.
* `action pose_dump <tag> <frames> <delay_ms>`: Sam's (tag >= 1000: census body tag-1000) AnimationManager, its resource, SkinnedModel and model resource plus the buffers they point to, on consecutive ticks, to `automation\out\pose-<tag>-<n>.bin`.
* `action pose_scan <tag>`: worker thread; world-space float/double triples near Sam and references to his Skeleton, to `posescan-<tag>.bin`.
* `action look_at_body <index> <distance>`: teleport (mode 1) so census body `index` stands `distance` m ahead of Sam.
* `action snap_teleport <x> <y> [z]`: PlaceOn mode 2 (snap to the topmost surface).

Scenarios: `anim-census-discover`, `anim-census-mule`, `anim-census-tour`, `anim-census-tour2`.

### Real partner body (`real_probe`, `real_spawn`, `real_remove`, `real_names`, `real_stats`, `real_mirror`, `real_control`; scenarios only)

`native/mod/real_body.cpp`, mechanism and evidence in [real-partner-body.md](real-partner-body.md). Needs the pose
hook, which is installed at start-up only with `session.ini partner_body=real` or `pose_hook=1` (scenario line
`ini pose_hook=1`).

* `action real_probe`: the player's own resource reference (path key, GUID byte order compared with
  `ds_player_character`'s decoded GUID), the streaming manager, Sam's SkinnedModel and skeleton. Reads only.
* `action real_spawn <body id> <distance m> <mirror 0/1>`: loads a body from `body_table.cpp` (1 Sam, 2 porter,
  3 Fragile, 4 Die-Hardman, 5 Cliff, ...) by path + GUID and stands it `distance` m ahead of Sam facing him (a
  negative distance: behind him, facing his way). Mirror 1: it copies Sam's live pose.
* `action real_mirror <0/1>`, `action real_remove`, `action real_names` (77 joint names, Sam vs the body),
  `action real_stats` (hook counters, latch, partner-vs-published angle check).
* `action real_control <n>`: the n-th streamed-out collectors item through the same fake path+GUID reference
  (a control for the load mechanism; holograms are refused afterwards because they have no SkinnedModel).

Scenarios: `real-body-diag`, `real-body-tour` (solo), `echo-real-100`, `echo-real-fragile-100` (role echo).

## Byte-check table

| Check | RVA | Gates |
|---|---|---|
| PlayerGame::PlaceOn (whole function) | `0x2634F00` | teleport, unstuck |
| DSPlayerEntity::GetPlayer | `0x27115B0` | teleport, unstuck |
| PopWorldTransform guards (DSPlayerSystem global, `+0x206D0`, `Sam+0x678`) | `0x26E9953` | teleport, unstuck, heal |
| GameModule suspend check (GameModule global, `+0x44C`) | `0x26A05A6` | load mask, teleport, unstuck, heal, time, voice ping |
| IsStoppingGameEvent (DSGameState global, `+8`, `+0x10`, mask) | `0x2DD3760` | game state, teleport, unstuck, heal, voice ping |
| DSGameState setup phase (`+0x5E0`) | `0x2E0E950` | game state, teleport, unstuck, heal |
| IsBoundToSequence (export + body) | `0x34FD620`, `0x2381700` | teleport, unstuck, heal |
| GetCurrentArea fast path (global, `+0xA8`, `+0x48`) | `0x262A864` | area |
| SetTimeOfDay event (GameModule `+0x58`, call, `+0x2C`) | `0x35720FC` | time |
| GameWorldTimeState::SetTimeOfDay (whole function, incl. the instant path) | `0x3631F90` | time |
| GetHealth / GetMaxHealth | `0x27105E0`, `0x2710F60` | health, heal |
| Entity_ExportedHeal / Entity::Heal / DSPlayerEntity::OnHeal | `0x2361BD0`, `0x234C310`, `0x2713AE0` | heal |
| Heal body (DSPlayerSystem global) / heal gate (`+0x206D1`, `[0x147C0AFF8]+0x8170C`) | `0x27023C0`, `0x26A05F0` | heal |
| Safe-transform picker (`+0x7F73`, `+0x81F0`, `+0x8230`, ...) | `0x27A4A40`, `0x27A4B79` | unstuck |
| Return-to-safe warp (`Sam+0x54C0` -> picker) | `0x26EA537` | unstuck |
| DSEchoComponent::Play (whole function) | `0x2BBF3A0` | voice ping |
| GameModule loading check called by Play | `0x2DCAC40` | voice ping |
| DSEchoComponent update, sentence pick (`+0x30`, `+0x60`, `+0x68`, resource `+0x20/+0x28`) | `0x2BB705F` | voice ping |

Regenerate the hex with `DS.load().rd(va, n).hex()` from `tools/re/dsre.py` after a game
patch, re-verifying each meaning first.

## Tests

No game needed: `st_overlay_tests` covers the new registry rows, the handlers
(teleport modes, partner teleport, heal, time, freeze, voice ping with/without a partner,
unstuck failure) against `tests/overlay_sync_stubs.cpp` (which also stands in for
`game_actions`), the snapshot's diagnostics block, the flag/load-mask/area decoders and the
copy-diagnostics report. `st_overlay_harness` draws the new Status/Player rows. The stub
teleport answers "not implemented in the test stub" by default, which the harness expects.

## First in-game run checklist

1. Log: the `[game] actions: N/N byte checks ok` line, nothing DISABLED.
2. Status tab: game state `none` while walking; Pause menu shows `PauseMenu (stopping
   event)`; the load mask is non-zero during a load; time of day matches the sky; health
   plausible.
3. Teleport to a saved position 50 m away: the `teleport check +0.5 s/+2 s` lines say ok and
   Sam does not snap back. Then a far one (a few km): watch for falling through the world.
4. Snap to ground (x, y) on open terrain, then near a building.
5. Unstuck after walking a bit: should land on a recent safe spot.
6. Heal after a fall; Set time of day 03:00 and 15:00; Freeze time and wait a few minutes.
7. Voice ping with role=echo: hear a line from the partner; if refused, note the state.
