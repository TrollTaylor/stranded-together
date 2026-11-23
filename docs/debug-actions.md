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
| load mask | `GameModule` `[0x147BE6318]` `+0x44C` (u32), `+0x450` (load-reason byte) | Recomputed every frame by `0x142627640`; non-zero = loading/suspended. Bit names *inferred*: 0x2 loading/area change, 0x4/0x8 pending, 0x40 streaming not ready, 0x80 streaming settle, 0x100 scene wait (a used sequence waits for its scene: `0x142627640`; NOT a save, whose path never touches the mask: docs/real-partner-body.md 24.12). |
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
* Both are **refused when this game plays on its real save** (`dev::RealSaveRefusal`:
  `automation::SavesRedirected()`, the hidden-run real-save override counting as a real
  save): the world time may be kept by a save, so it follows the dev tools' real-save rule
  (dev_rules.hpp) from every tab, with or without the dev tools on. The result line says
  `time of day refused: this game plays on its real save ...`.

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

### Enemy census (`enemy_census`; enemy sync P0, [enemy-sync.md](enemy-sync.md) section 3)

`native/mod/enemy_census.cpp`. Reads only (ReadProcessMemory) and nothing is written. The walk calls no game function; only
our own Sam's entity and place come from the mod's usual guarded getters (GetLocalPlayerEntity, EntityGetWorldTransform).

* `action enemy_census <tag> [radius_m] [flags]`: walks the game actor registry `[0x147BBB8C0]` and the NPC manager's GUID
  maps `[0x147BF29B8]`. One summary block (actor counts by id type and by class, the NPC map by kind, the MULE manager's
  members and killed records, two fingerprints: `ids` over the sorted type-1 actor ids, `guids` over their sorted GUIDs), then
  one line per MULE / Demens / warrior actor (id type 1), nearest first: id, class, state (2 = its entity is in the world),
  GUID, NPC kind, place (`xf` = the actor's own transform at +0x28, `ent` = its entity's), heading, distance, entity (dead
  flag, how far the entity stands from the actor's transform), low-LOD byte, and for a `DSMuleGameActor` its alert (+0x110)
  and flag words (+0x120, +0x124, +0xEA4). `radius_m` (default 0 = all) limits the per-enemy lines; flags bit 1 adds one line
  per actor of every other type, bit 2 the MULE manager's killed records (raw, 0x28 bytes each). Ends with
  `[enemy] census done tag=<tag>`.
* Byte checks on first use (18 sites and 4 RIP globals, docs/enemy-sync.md 3.2): a core mismatch (registry, NPC maps) turns
  the census off with one line; a mismatch elsewhere only drops that field.

Scenario: `two-games-enemy-census` (the T0 run).

### Name tags (`name_tags`; scenarios only)

`native/mod/nametags.cpp`, docs/real-partner-body.md section 10.

* `action name_tags 0`: normal (the partner's tag).
* `action name_tags 1`: self test: a tag above your own Sam (`<name> (test)`), to check the projection
  without a partner.
* `action name_tags 2`: log the camera view (position, axes, handedness, near/far, fov, aspect) and where your
  own Sam's feet and tag anchor project on the back buffer.
* `action name_tags 3 <0/1>`: tags off / on for this session (on needs `name_tags=1`).

Scenarios: `name-tags`, `two-games-name-tags`, `two-games-all`.

### Player collision (`collision`; scenarios only)

`native/mod/partner_collision.cpp`, docs/real-partner-body.md section 11.

* `action collision 0`: log the state, the partner's capsule (in world, disable counter) and the self
  checks (Sam's own capsule, its resource and radius, the layer-matrix bits).
* `action collision 1 <0/1>`: collision off / on for this session.
* `action collision 2 <ms>`: log the horizontal Sam-partner distance every tick for `<ms>`
  (`[coll] trace d=... sam=(x, y, z ...) ... body=(x, y, z ...)`: with both heights, for climb checks).
* `action collision 3 <m> [dz]`: pin the partner body `<m>` m ahead of Sam along the camera's forward (where the
  pad's forward walks), facing him, holding its last pose; `3 0` lets it follow the stream again. `dz` (-10..10 m)
  puts it that far above (+) or below (-) Sam: `3 2.5 -2.2` is Sam on a ledge above the partner (collision
  off: `[coll] off: Sam is above the partner (over 1.6 m)`).
* `action collision 4 <layer 10|9|25> <type 0|1>`: reconfigure the capsule (type 0 rigid body, 1 proxy).
* `action collision 5 <m>`: reconfigure the capsule with a standing height of `<m>` m (1 to 6; `0` = Sam's own
  1.65 m resource, the v0.7 capsule that the game offers Climb on). Default 4.0 (`partner_collision_height`).
  `[coll] tall capsule: ...` logs each resource copy, `[coll] reconfigured: standing height ...` the change.
* `action collision 6 <m> [dz]`: the pin of `collision 3`, but the partner body turned the way Sam faces: its
  back to him and to the camera (hidden tests: close shots of the partner's back cargo). `collision 3 0` unpins.

The capsule is in the world only while the partner is within 4 m of Sam (the state then reads `off: the
partner is not near Sam`) and not riding something (`off: the partner is in a vehicle`).

Scenarios: `collision`, `collision-off`, `two-games-collision`, `two-games-all`.

### Gear (`gear`; scenarios only)

`native/mod/partner_gear.cpp`, docs/real-partner-body.md section 12.

* `action gear 0`: log our gear in full (the inventory's backpack and Odradek, every carrier slot and
  piece with its list item, case type, volume and model, Sam's anchor joints in world space, the frame
  sent), the partner's gear and the prop counters (with `new layouts held a read`: layouts taken only when
  the next read agreed).
* `action gear 1 <0/1>`: the partner's props off / on.
* `action gear 2 <0/1>`: the self view's props off / on (since v0.8 only the BB pod: the self view keeps Sam's real gear).
* `action gear 3 <n>` (hidden test runs on scratch saves only): stack `n` (0..12) synthetic delivery
  cases (large, medium, small, ...) on our backpack; they are sent and shown like carried cargo. `3 0`
  removes them.
* `action gear 4 <0/1>`: our own gear on the debug body (`real_spawn ... 1`): the partner's placement
  path in a solo run (and our gear parts mirrored on a debug Sam).
* `action gear 5 <n>` (hidden test runs on scratch saves only): the next `n` prop starts fail (a
  transient failure injected): they must be retried and come back (section 12.8).
* `action gear 6` (hidden test runs on scratch saves only): every partner prop is hidden behind the
  mod's back, as the game might do; the next tick shows them again (`reshown`).
* `action gear 7 <0..2>`: which of the game's gear entities are mirrored: 0 model components only, 1
  (default) also the proven small DS components (shields, the stowed weapons' paint, hanging boots, simple
  animation, sound), 2 also the chiral cylinder and the backpack belt. A `DSWeaponEntity` is never copied, at
  any level (a copy crashes the game; weapons are their own kind, `gear 17`). Read again at once. Level 2 only in hidden test runs on scratch saves (refused otherwise: not proven safe);
  at level 2 this game also accepts any `ds/entities/` resource from the partner (section 12.5).
* `action gear 8 <0/1>` (hidden test runs on scratch saves only for 1): the BB pod as a copy of the game's
  own pod entity (1) instead of the cinematic pod prop (0, default). Both runs that created such a copy
  crashed (section 12.6).
* `action gear 9 <0/1/2>`: census (read-only) of the local player (0: child entity tree with
  resources, components, positions and nearest joints, the inventory's gear entities, the mesh
  controller's part masks and ArtParts map), the partner / debug body (1) or the self body (2).
* `action gear 10 <0/1>`: the sender's entity-walk cache off / on (default on). The read's cost on the
  game thread is logged after 50 reads and then every 600 (`[gear] read cost (game thread): mean ... us,
  max ... us over N reads; walk cache on (hits, misses); looks read (every 500 ms) mean ... us, max ... us`).

* `action gear 11 <0/1>`: the looks (docs/real-partner-body.md section 13), read-only: 0 our gear (every
  mirrored entity's applied art-parts variations with the source this game finds each in, its shader
  variables, its components and their resources, its pose's skeleton; the backpack's 42 joints), 1 the
  partner's props (the same, plus what each item should show).
* `action gear 12 <0/1>`: gear looks (STV1: variations and shader variables) off / on, both directions (default
  on). Off stops sending and applying; what a prop already shows stays until it is spawned again. The
  periodic `[look] props: ...` line counts applies, variations left out (`unresolved`, `of another kind's
  sources`), shader variables refused at the copy's cap (48 entries) and how often our own gear's sources
  were gathered again; the backpack pose part says `OFF for this session` after a pose-write fault.
* `action gear 13 <0/1>`: the backpack's skinned pose (STK1) off / on (default on).
* `action gear 14 [type] [0/1]` (writes only in hidden test runs on scratch saves): lists the inventory's
  accessory list (inv `+0x37F9 + 0x40 i`: type, worn flag `+0x1D`, item `+0x1F`); with a type (0x0F = 15 the
  Bridges cap (cap00), 0x24 = 36 the Santa hat (cap01), 0x31 = 49 the headcrab (cap02), 0x32 = 50 the hair ornament
  (cap03), 0x0D / 0x0E glasses; docs/real-partner-body.md section 26.3) sets the first such entry's worn flag: the
  game puts the item on or takes it off itself. The cap colour (`gear 15`) does not pick the head item.
* `action gear 27 <0|1|2>`, `gear 28 <type>`, `gear 29 <0|1>` (round 7; docs/real-partner-body.md section 32): Sam's skeleton
  on her. 27: the exo carrier off / on (`session.ini exo_carrier`, default on; off removes both carriers at once); 3 / 4
  the self carrier's pose source (3 Sam's newest capture, the default; 4 the capture the self body was last written from,
  one frame behind while moving); any other value only logs the status line `[exo] exo_carrier on, forced type -1, LegOut
  off, self source Sam's newest; our skeleton speed; partner
  carrier live shown (exo carrier <p>: n pose writes (n refused, n torn, n wpm), n forced updates); self carrier ...`.
  28 (hidden tests; both screens follow, it changes what we read and send, not Sam's own model): the skeleton pieces we
  read replaced by a type's: -1 off (what the game shows), 0 none, 1 speed, 2 all-terrain, 3 power, 4 support; logged
  `[exo] our Sam's skeleton: power (forced: gear 28) (pieces shown <hex>)`. 29: the sender's LegOut suit piece (worn with a
  skeleton) also on her (0 off, the default). The carriers log `[exo] carrier: loading ...`, `[exo] carrier <p> live`,
  `[exo] carrier <p> follows the partner body's pose` (`[exo-self] ... the self body's pose`), `[exo] carrier <p> parts: n
  set (...); shown now: 1 32 33 34 35 (the sender's skeleton: speed)`, `[exo] carrier <p>: shown (n ms after it became
  live)` / `: hidden`, `[exo] carrier removed (<why>)` and, with a shown carrier, `[exo] carrier vs body cm mean/max a / b
  (n joints); ...` (every second in hidden runs, every 10 s otherwise).
* `action gear 30 <3|1|2>` (round 11; docs/real-partner-body.md section 35): the BB pod under the self view. 3 the game's
  own rule for Sam's pod with our hide bit ignored (the default; it turns itself off for the session when it disagrees with
  the pod's own flag for 2 s of plain play while the self view is off, which only the side with the self view off ever
  sees: the host). 1 the fallback only: the last plain-play reading, taken as worn after a load, hidden in the private
  room. Any other value (0, 2, or no argument) only logs `[gear] BB pod under the self view: the game's rule (rule sites ok);
  now shown (the game's rule for Sam's pod); cross-check n match, n mismatch over n reads with the self view off; fallback
  shown (read in plain play); our gear: BB pod 1; the partner's gear: BB pod 1; self view: BB pod 1 wanted, 1 live` (the
  action's own message is the short form, naming the source the last decision used: `the rule`, `its flag`, `the
  private room`, or which fallback).
* `action gear 31 <0|1|2|3>`, `gear 32 <state> [type]`, `gear 33 <0|1>` (round 11; docs/real-partner-body.md section 37):
  her rain hood. 31: 0 off (no hood on her, her hat stays on in the rain as in v0.25), 1 the timed swap (the raised hood
  appears when Sam's is up), 2 the game's unfold mirrored on the exo carrier where it can play it, else the timed swap (the
  default; `session.ini fragile_hood=0|1|2`); any other value only logs `[hood] her hood on (the unfold), hair hidden under
  it where a hair part is found; sites ok / lookup ok / unfold ok; ours: sea otter up (state 6); the partner's: up; partner carrier shown, hood up
  (part 69, idle, slots 7, n calls, 0 faults), hair no part; self ...` (the action's own message is the short form;
  `hair no part` is what every run so far printed: see `gear 33` below). 32
  (hidden tests; both screens follow, it changes what we read and send, not Sam's own hood): our hood reading replaced by
  `state` (0..12; 6 up, 9 lowering, 0 down) and `type` (0 porter, 1 oxi, 2 sea otter, the default); -1 off (what the game
  shows). 33: her hair under the hood 0 shown / 1 hidden (`session.ini hood_hair`, default 1). In practice it changes
  nothing: none of her `fragile_suit` body's parts has a name the mod can find as hair, so no hair part is ever hidden
  (`hair no part`); every hood type covers her hair anyway (docs/real-partner-body.md section 37.4). Logged: `[hood] our Sam's
  hood: sea otter, raising (state 5)` at every state change, `[exo] carrier <p> hood parts (on/off/switch): porter
  54/53/68, ...`, `[exo] carrier <p>: hood animation slots (porter 2, oxi 5, sea otter 4): ok`, `[exo] carrier <p> hood:
  up (part 69; the sender's sea otter hood up, state 6; the unfold)`, `[exo] carrier <p> hood: clip 12 played on slot 4
  (0 ms into the raising)`, `[exo] her body <p>: no hair part found (her hair stays under the hood); parts: ...` (or
  `hair parts under her hood ...` if one were found; `[exo-self]` for her own screen), and the
  carrier meter's `head` figure (her head joint 31 against the carrier's, cm).
* `action gear 26 <0|1|2|9>` (round 6; docs/real-partner-body.md section 31): what is in a hand on the other screen
  (`held_items`): 0 off (only v0.9's cutscene-prop weapons), 1 the ladder / PCC / climbing anchor, 2 every item in a hand
  (the default); any other value only logs `[gear] held_items 2; held clones on (ok): n resource(s) re-typed, n entities
  created (n approximate), n not owned, n refused, n faults; held props n live, n shown; ...; the item's own pose: n sent,
  n received, n written` and, per held item, ours (`[gear]   ours: weapon item ...`) and each clone (`[gear]   gear held
  clone ...: ... m from the body's joint ...; skeleton ...; pose target ...`).
* `action gear 25 <0|1|2>` (round 6 T3; docs/real-partner-body.md section 30): her boots' style at run time: 0 `prop`
  (Sam's boots on her feet), 1 `own` (her own boots, painted with the boot pictures; no boots prop, feet not scaled, Sam's
  boots entity hidden under the self view), 2 only the status line: `[gear] boots_look own (partner: own (the partner's
  look) | prop ... | ours; self: Sam's boots shown, hidden by us yes, sets n clears n reasserts n; parts <hex> sent <hex>)`.
  The `[boots]` lines say each decision: `[boots] self view: her own boots, Sam's hidden (boots_look=own, ...)` /
  `... Sam's boots on her feet ...`, `[boots] partner: her own boots (no prop) (boots_look=own from the partner's look,
  parts <hex>)` / `... Sam's boots on the body (the prop) ...`, `[boots] Sam's boots entity <p> hidden under the self view`
  / `... shown again`.
* `action gear 24 <percent>` (hidden test runs on scratch saves only): our backpack's rain cover's durability, in percent of
  its level's maximum, in both copies of its data block (docs/real-partner-body.md section 28); in timefall the game's own
  update then wears it out and removes it (0.2 % broke within 7 s, run `20261122-085045`). The cover's wear is logged
  without it: `[gear] wear: the backpack's rain cover (level 2): durability 4.00 % (180 of 4500)` at first sight, at every
  step (1 % under 10 %, 10 % above), and `... broke: worn out in timefall ...` when the game removes it; a gear layout
  change names what it lost or gained (`[gear] our gear lost 1 item(s) of resource 2 '...backpack_temp' 0dfece64...`,
  `[gear] our gear: the backpack's rain cover is gone ...`, `[gear] the partner's gear: ...`).
* `action gear 15 <0..24>` (hidden test runs on scratch saves only): our cap colour byte (inv `+0x4BE0`:
  0..22 a colour index, 23 silver, 24 gold); the game re-colours the cap itself.
* `action gear 17 <0/1>`: weapons in hand and on the backpack (docs/real-partner-body.md section 17) not sent /
  sent (default on; the partner's props follow). The result reports the weapon watch: changes seen, how many of
  their reads waited for the watch's 40 ms gap, and the time from a draw / holster to the frame that carries it
  (mean, max). Weapons with no prop on the other screen are not sent (logged once each).
* `action gear 18 <0/1>`: the self view headgear's mover writes (docs/real-partner-body.md section 20) off (every
  accessory's mover back to the identity, and the v0.11 head lift back) / on (the default). `[headgear] our entity ...
  hangs on HLP_Head but has no offset of its own`: an unlisted head item is shown, so no offset is written and the lift
  holds every head item up while it is shown. `action gear 19 <0/1>`: the headgear and boots meters (`[headgear] self
  ...`, `[headgear] partner head prop ...`, `[boots] boots' feet vs the drawn feet ...`; every second) off / on. By
  default they run in hidden tests only (never in play).
* `action gear 20 <percent>` (1-100, default 1): the partner body's feet scale under its boots prop (section 21;
  100: not scaled). `action gear 21 <0/1>`: boots sync at run time (section 21): our boots item not sent and the
  partner's boots prop not drawn (0) / both (1, the default with `boots_sync=1`).
* `action gear 22 <0/1>`: her hat in the rain (docs/real-partner-body.md section 26.8): the worn hat the game hides under
  Sam's hood drawn on our hoodless character on both screens (1, the default) / the game's look (0). `action gear 23
  <which> <0..63>` (hidden test runs on scratch saves only): the glasses' colour bytes (inv `+0x4BDC`: 0 A lens, 1 B lens,
  2 A frame, 3 B frame), to wear a user's outfit in a run.
* `action gear 16 <0/1>`: Sam's head parts (hair, hood, face lines; docs/real-partner-body.md section 14) not
  named / named in our frames (default on). Off only stops naming them, so a partner body keeps what it had
  last: to see the cutscene defaults, set 0 before the body comes. The result says whether the last read was
  taken or held (not the porter suit, Sam hidden by a part mask, the hood switch animation) and how many parts
  were set on our partner body. `gear 14 53 0` takes the cyphead accessory off (the metal-line hair and face
  lines).

Scenarios: `gear-self`, `two-games-gear`, `two-games-all`, `gear-full`, `two-games-full-gear`, `two-games-gear-look`,
`echo-hair`, `two-games-hair`, `two-games-looks-menu`, `two-games-fragile-cover`.

### Smooth animation (`smooth`; scenarios only)

`native/mod/real_body.cpp`, docs/real-partner-body.md section 3.5. Needs the pose hook.

* `action smooth 0`: log the smooth mask, the frame cap, the update frequency (entity `+0x204`) of the
  partner body, the self body and Sam, the setter's byte check and its call / fault counts, props boosted
  and restored.
* `action smooth 1 <mask>`: the smooth features at run time (bits: 1 the 240 Hz update boost of our
  bodies and gear props, 2 interpolation in the partner body's own update, 4 capture-time stamps on our
  STJ1 frames; 7 all = `smooth_pose=1`, 0 = the v0.7 behaviour). Clearing bit 1 puts each body and prop
  back at its own frequency in the next tick (props within 250 ms).
* `action smooth 2 <fps>`: this game's frame cap now (0 off, else 15..240; the same wait as
  `session.ini frame_cap`, which it replaces until the end of the session). A `settings.cfg` fps limit
  (`graphics low`: 30) still applies on top.
* `action smooth 3 <n>`: log the next `n` frames (up to 600) one by one: frame time, the partner's, the
  self body's and Sam's joint step in degrees, pose writes (`[smooth] seq` lines).

While a body is live a `[smooth]` line is logged every second in hidden tests (one second in ten
otherwise): presented fps, pose writes per frame, new vs repeated poses, stutter, jerk, latency, the echo
error against Sam (section 3.5 "The meter").

Scenarios: `smooth-echo`, `smooth-two-games`.

### Audio probe (`audio_probe`; scenarios only, `session.ini audio_probe=1`)

`native/mod/audio_probe.cpp`, [audio-research.md](audio-research.md) section 9 (Phase 0). Logging only: the
hooks are installed at start-up (each byte-checked, a mismatch skips that hook and the log names it) only
with `audio_probe=1`; with the key off nothing is hooked.

* `action audio_probe 0`: a summary line (Wwise calls per kind, game objects and their owners, voice-path
  counts, ring drops). The same line is logged every 10 s while anything changes.
* `action audio_probe 1`: the full dump: per thread, per event (posts by owner, RTPCs set on the object
  before the post, the owners), the player's game objects, the other posting objects grouped by owner, the
  instance creators, the local player's animation sound indices, the talk requests per owner.
* `action audio_probe 2 <n>`: asks the local player's voice queue for test situation `n` the way
  `PlayPlayerVoice` does (the check `0x142717F30`, then `0x1427352E0` with flag 1, on the game thread).
  0 ReceivedLike, 1 Jumping, 2 OutOfBreath, 3 Humming, 4 AlmostThere, 6 Cold, 7 HoldBaggageNormal (the
  list in `kSituations`). It needs the player getter `0x14283EC30` to return the local player (a
  `DSPlayerEntity`). The only call the probe makes into the game.
* `action audio_probe 3`: the local player's components (and which derive from the talk / voice / sound
  types). `action audio_probe 4`: reset the counters.

*(2026-11-18, audio-research.md section 18)* The probe also hooks `SetGameObjectOutputBusVolume` and
`SetGameObjectAuxSendValues` (observational) and registers Wwise bus meters (`RegisterBusMeteringCallback`, peak) on
Sam's voice chain `0xDCB6FE59`, `0x3D52851C`, `0xEE074523`, `0x5FD8F5F6`, `0x7CC88FD9` and the master `0xE2B7BC37`.
Lines: `[audio] voice bus meters registered (...)`; `[audio] mix-bus node for bus 0x... ` (each bus that gets a mix node,
once; only those can be metered: in this game Sam's chain mixes first at `0x7CC88FD9`); `[audio] voice after mute (...)`
with the game's volume / aux-send calls on the objects whose voice self_voice silenced and, per bus, frames, loud frames
(peak > -60 dBFS) and the max split by the self_voice state (not muting / v0.11's volume 0 / stopping); `[audio] voice
bus 0x7CC88FD9 loud from <t> s for <ms>: ...` for each loud stretch (the timeline to lay next to the `voice post #N`
lines). The hidden test silences the device buffer only, after the mix, so the meters read what would be heard.

Scenarios: `audio-probe` (Sam), `audio-probe-fragile` (the guest-Fragile self view).

With `self_voice` muting (below) the probe owns the talk-handler hook and asks `self_voice` first: a dropped talk is
logged as `[audio] talk #N MUTED ...` and counted as `(MUTED by self_voice)` in the dump's talk requests.

### Self voice (`self_voice`; `session.ini self_voice=auto|sam|mute`)

`native/mod/self_voice.cpp`, [audio-research.md](audio-research.md) section 9 (Phase 1), sections 14 and 17. Sam's voice
(spoken lines and the grunts, efforts and breathing) is muted at `DSTalkComponent`'s `MsgDSStartTalk` handler
when the talk is the local player's own (or the cargo menu figure's while it shows the chosen character). Since
2026-11-18 the local player's muted talk **runs** and only its voice post is silenced in Wwise (a PostEvent detour,
partner_sound's or the probe's, calls `selfvoice::PrePost` / `AfterPost`; `[voice] Sam's muted voice: the talk runs ...`);
without such a detour the talk is dropped as before. The figure's talks are always dropped. *(Changed after v0.11,
audio-research.md section 18)* the voice post is **stopped** right after it is posted (`StopPlayingID`, 0 ms): v0.11's
volume 0 was undone by the game's own sound-instance update and the grunts were heard. Only Sam's 28 voice events
(`selfvoice::kSamVoiceEvents`) near the local player are stopped. The `[voice] counts` lines add `dropped N, ran with the
voice stopped M` and `Sam's voice posts stopped N of M (exact, a muted talk pending, none pending), stop call faulted,
post failed; near the player, a muted talk pending, not one of Sam's voice events N; other talkers kept`; such a
not-Sam post is also logged (`an external-source post ... is not one of Sam's voice events: kept`). The hook
is installed at start-up only when the setting resolves to mute (`auto` with a character other than Sam, or `mute`);
otherwise nothing is hooked and the action reports "not armed". Every second in which a count changed the mod logs
`[voice] counts: ... local player talks muted N passed M; menu figure talks muted N passed M ...`.

* `action self_voice 0`: the counts (also in the result text).
* `action self_voice 1 <0|1>`: mute off / on at run time (only when armed). With it off, the same talks are
  counted as passed: the A/B within one run.
* `action self_voice 2 <list>`: the cargo menu's figure (`DSPlayerAKukanEntity`) says a random line of its line
  list `list` (0 or 1: the figure has 2 lists of 8 sentences in the 100% save) through the game's own
  `PlayAkukanVoice` export (`0x142757D40`, byte-checked with its tail jump to `0x14273BB10`; the figure's
  resource, list count and sentence count are read fault-free first). Fails when Manage Cargo is not open.
* `action self_voice 3 <n>`: test situation `n` (the audio probe's list: 0 ReceivedLike, 1 Jumping, 2 OutOfBreath,
  3 Humming, 4 AlmostThere, 5 IdleBreathingWhenOverburdened, 6 Cold, 7 HoldBaggageNormal) through the local player's
  voice queue (the game's `PlayPlayerVoice` path, byte-checked, SEH-guarded); works without the probe and when nothing
  is armed. The result says `check 1, queued 0|1` (the queue refuses some requests by priority or repeat rules).
* `action self_voice 4 <0|1>`: how a muted voice post is silenced: `1` stopped (the default), `0` v0.11's
  `SetGameObjectOutputBusVolume(object, 0)` before the post, not stopped. Test only, for before / after measurements (it
  lets the grunts through); logged as `[voice] ... v0.11's volume 0 (debug op 4 0, now on|off): set on N voice posts`.

Scenarios: `grunts-fragile` (the grunts: walk, run, jumps, situations with the stop, with v0.11's volume 0 and with the
mute off, measured by the probe's bus meters), `self-voice-fragile` (guest-Fragile self view with the audio probe), `self-voice-own-hook` (the same
without the probe: self_voice's own hook, the shipping set-up; with `sound_sync=0` partner_sound hooks only PostEvent
for the silencing), `self-voice-sam` (the Sam control: not armed), `bb-answer-selfview` (the self view without the mute:
BB's answer control), `two-games-audio` (the final run), `audio-keys-off` (every audio key off).

With `voice_sync=1` (below) the same hook is installed even when nothing is muted (a Sam host): it then only copies
the local player's talks for the partner voice, and `self_voice 1` reports "not armed" (nothing to mute).

### Voice sync (`voice_sync`; `session.ini voice_sync=1`, `voice_subtitles=0|1`)

`native/mod/partner_voice.cpp`, [audio-research.md](audio-research.md) section 15 (Phase 2). Each player hears the
other's voice: the local player's talks go out as STS1 (kind 0 while the character is Sam, kind 1 without the line
otherwise) and the partner's are played with the game's `DSTalkComponent::PlaySentence` on the invisible talking
entity at the partner body's head. Log lines (`[pvoice]`): `sent seq N kind K ...`, `received seq N kind K sentence
i/n <GUID>: resolved to SentenceResource ...`, `partner line seq N ... -> PlaySentence(...) = 1 | talking entity ...
at (x, y, z) ...`, `not played: <why>`, `subtitle request: ... -> DROPPED (a partner line ...)|DROPPED (the local
player's own muted line, self_voice)|shown`, every 60 s (was 5 s) `talking entity ... | partner head ..., d m apart`,
and `counts` every 10 s when they changed. At most 2 received lines are handled a tick (the rest wait); a partner
line's subtitle is dropped only within 2 s of `PlaySentence`, and not at all when the game refused the line.

* `action voice_sync 0`: the counters (sent / received / played / refused / unresolved / skipped / subtitles / link).
* `action voice_sync 1 <n>`: play sentence `n` (modulo the map size; the map is sorted by GUID, so `n` names the
  same sentence on every game: 700 is a spoken one) on the talking entity as if the partner had said it, with the
  same checks (paused / menu, distance, the entity's `DSTalkComponent`). A line those checks refuse is logged with
  the reason (`test line seq 0 ...: not played: <why>`) and named in the result text; the action itself succeeds.
* `action voice_sync 2 <0|1>`: **test only**: 1 sends Sam's lines as kind 0 even when this player is not Sam (the
  other direction of a host-Sam / guest-Fragile run); 0 back to normal.
* `action voice_sync 3`: rebuild the GUID map from the local player's resident sentence group and log it.

Scenarios: `voice-sync-echo` (role echo: our own lines come back through STS1 to the echo partner), `two-games-voice`
(host Sam, guest Fragile, the probe on both), `two-games-voice-own` (the same without the probe; the guest with
`voice_subtitles=1`).

### Sound sync (`sound_sync`; `session.ini sound_sync=1`, `sound_steps=0|1`)

`native/mod/partner_sound.cpp`, [audio-research.md](audio-research.md) section 16 (Phase 3). Each player hears the
other's footsteps, foley, cargo and weapon sounds: the Wwise events posted on the local player's own game objects go
out as STA1 in the pose datagram, and the partner's are posted on our own Wwise objects at the partner body's joints,
at the pose play time. Log lines (`[psound]`): `mirror post <category> ev 0x... pid N obj ... tag T (<owner>) | slot
<joint> offset (x, y, z) m ...` (sender), `partner post <category> ev 0x... (their pid N) -> pid M on our object i
... | at (x, y, z): the partner's <joint> joint (h m over the body's root) + (offset) m, d m from that joint ... |
play time - event time +n ms` (receiver), `gate closed: <why>` / `gate open again after n ms (was: <why>): N partner
item(s) held back, M posted while it was closed`, `stopped the partner's sounds on N object(s): <why>`, and `status`
(two lines: sent, received) every 30 s (was 5 s) when the counters changed. The first 30 mirrored posts of each side
are always logged. Since 2026-11-18 (audio-research.md section 17.4): `partner post <category> ev 0x... refused: not on
the allow-list` (the first 10), and the status lines add `invalid items dropped`, `event ids learned here`, `refused
(allow-list)`, `re-registered`, `capped (15 min)`, `partner_volume 0` and `volume`. Owner labels outside the allow-list
read `denied (not on the allow-list): ...`. `session.ini partner_volume=0..100` sets the volume of the partner's sounds.

* `action sound_sync 0`: the counters (both lines) and, per owner label (`DSCustomSoundComponent of the player`,
  `SoundComponent of DSWeaponEntity (a child entity, depth 1)`, `material effect emitter (...)`, `denied (camera): ...`),
  the registrations, posts and sent posts.
* `action sound_sync 1 <0|1>`: log every mirrored post on both sides (1) or only the first 30 (0).
* `action sound_sync 2`: stop every partner sound we play now (`StopAll` on our objects).
* `action sound_sync 3 [ms]` (2026-11-18, real-partner-body.md section 22.9): the vehicle line of the counters now, and
  for `ms` (at most 600,000) the vehicle sound trace every 500 ms: `vehicle trace tx obj 0x... tag T | loops <ev>/<pid>
  ... | N rtpc(s): <id>=<value> ...` (the sound objects of the vehicle we ride with something on them) and `vehicle trace
  rx our object i tag T active n | at (x, y, z) = the vehicle + (offset); vs the copy entity + offset d m | N rtpc(s):
  ...` (our objects playing the partner's vehicle sounds).
* `action sound_sync 4 <0|1>`: the vehicle sounds off / on at run time (both sides; needs `vehicle_sound=1` and
  `vehicle_sync` not 0 at start, which install the instance create / setup hooks).

**Vehicle sounds** (`session.ini vehicle_sound=1`, default, with `sound_sync=1`; real-partner-body.md section 22.9).
Log lines: `vehicle sounds: our vehicle <p> (<class>, seated|parked): N of its sound object(s) known, M loop(s) playing
on them` / `vehicle sounds: no vehicle reported now (was <p>): its loops stopped on the receiver`, `vehicle post ev 0x...
pid N tag T [(held loop)] on vehicle <p> | at (x, y, z) m in its frame | N rtpc(s): ...`, `vehicle loop picked up ...`,
`vehicle refresh ...` (the first 40 lines only), `vehicle loop ev 0x... pid N stopped by the game after n ms (fade f
ms)[: the stop goes out]`, `vehicle loop ev 0x... pid N stopped on the receiver: <why>` (sender); `partner post vehicle
ev 0x... (their pid N[, held loop]) -> pid M on our object i (tag T) | at (x, y, z): the partner's vehicle + (offset) m
in its frame, d m from its origin; vs the copy entity's own transform + offset: e m | ...` (the first 60),
`stopped N partner vehicle sound(s): <why>`, `a held partner vehicle loop (their pid N, our pid M) had no refresh for n
ms: stopped` (receiver); a third status line `vehicle (vehicle_sound on|off): sent: ... | received: ...` (since the review
it ends `tags skipped as still held n (forced m)`). Since the review (real-partner-body.md section 22.10) the create /
setup hooks are their own group, installed only with `vehicle_sound=1` and `vehicle_sync` not 0: when they cannot be
installed the log says `vehicle sounds off: <hook> (<why>)` and the partner's footsteps and foley stay on. The vehicle
sound meter says `vs the copy entity's own transform + offset: no copy` (the copy not live yet: the sound is placed at the
partner's vehicle frames) or `copy hidden over our own vehicle` (the copy is put below meanwhile).

Scenarios: `vehicle-echo-sound` (role echo: our vehicle's sounds come back to the echo copy; cuff map, held sends, the
key at run time, getting off, vehicle sync off; the trike and the truck), `two-games-vehicle-sound` (both directions,
all three kinds, both sides' cuff map, held sends 3 s / 12 s), `sound-sync-echo` (role echo: our own sounds come back
through STA1 to the echo partner, then the cuff map holds them back), `two-games-sound` (host Sam, guest Fragile, the probe on both), `two-games-sound-own` (the same with
the partner sounds' own hooks), `sound-sync-volume` (the echo steps with `partner_volume=50`), `two-games-audio` (all
audio with the launcher's default keys). The cuff map shows a "new ranked event" notice in these saves: A takes it away and B
then closes the map (START does not).

### Vehicle sync (`vehicle_sync`; `session.ini vehicle_sync=0|1|2|3|4`, default 1 (solid copies; 3 holograms); `vehicle_overlap_hide=0|1`, `vehicle_look=0|1`, `vehicle_packs=0|1`, default 1)

`native/mod/vehicle_sync.cpp`, [real-partner-body.md](real-partner-body.md) section 22. The vehicle a player rides goes
out as STM1, and the other screen shows a copy of it under the partner's body: since section 22.11 the trike's (also
for the Kusanagi) and the truck's solid cutscene bodies (the default, `vehicle_sync=1` / `2`; each copy's collision
turned off on its own entity before it is shown and checked every 250 ms), the roadster as its construction hologram (no
solid roadster copy exists), or with `vehicle_sync=3` all three as holograms (v0.13's default). Log lines (`[vehicle]`): `game checks ok`, `our vehicle <ptr>:
DSVehicleSpeedCar, kind 1, ride state 0; resource 'ds/entities/vehicle/pe_ds_vehicle_casp' <guid> (...); skeleton 23
joints, layout ECF43324` (first sight), `ours: roadster riding (2), seated, at (x, y, z) v m/s, seat (x, y, z), 23
joints ...; N frames sent (...)` (every 2 s while one is reported), `our Sam got off vehicle ...: reported as parked
...`, `our last vehicle ...: no longer reported (we are beyond 250 m, after n s parked)` (section 22.13; it was `(60 s |
beyond 30 m)` before), `first frame from the partner`, `the partner's
vehicle copy '<key>' loading / live after n ms / placed / shown / posed from the partner's vehicle joints / removed
(<why>)`, `the partner's <copy>: no copy (<why>)` (once per reason: the mode, a refusal) and, in hidden tests or
while a trace runs, every 5 s while a copy is live `meter (5 s): partner body vs the copy's seat ... cm; play delay ... ms; the
sent joints turned up to ... deg; the copy's pose vs the closest of the last 6 given ... deg` and the frame-aligned `meter:
while the partner's vehicle moved, N frames (ticks) and M pose writes on the copy (M/N per frame), repeats ..., interpolated
in its update ... (held at the newer frame ...); largest joint turn per write ... deg mean, ... sd, ... max`. Since the
second milestone also: `vehicle registry checks ok`, `overlap: copy '<key>' hidden: our own <class> <ptr> stands in its
place (d m, dz, yaw)` / `shown again (...)`, `the partner's vehicle copy '<key>' held where it stands (<why>; the
partner's pose is n ms old)` / `plays again`, `the partner's vehicle jumped n m between two frames ...: not slid`, `our
last vehicle <ptr> is gone (not in the vehicle registry | its vtable / GUID changed)`, `our last vehicle <ptr> forgotten
(our Sam is a new entity: a load)` (since 22.13 a menu no longer forgets a parked vehicle), `[smooth] vehicle copy update frequency 1 (14.99 Hz) -> 6 (240 Hz)`. Since the third
milestone (section 22.7): `our <key>'s look: n variation(s), m colour index(es)`, `our vehicle <ptr> shows n pack(s)`,
`the partner's <key>'s look: ...`, `look of the partner's <key> on copy '<copy>' (n variation(s), m colour(s); k of our
own vehicles of that resource as sources): applied | partly applied: <why>`, `the partner's <key>: its look is not
applied (copy '<copy>' has another model)`, `pack copy i '<battery_pack | defense_pack>' loading / placed: read back d m
from where it was put` (hidden tests / trace), `n pack copy(ies) removed (<why>)`, `pack copy '<key>' refused for this game
run: <why>`, and a fourth status line (`status: looks ...; packs ...; [look] ...`). Since the review (section 22.10):
`the partner's vehicle look refused: ArtParts bits or a shader variable other than the colour index` (STV1 item 255 with
anything the sender does not read; counted as `received (n refused)` in the status line); while a copy overlaps our own
vehicle it is also put 20 m below (the vehicle sound meter then says `copy hidden over our own vehicle`). Since section
22.11: `solid copies: collision switch checks ok (...)` (or `FAILED: the solid copies are off, the holograms stand in`),
`the partner's <key>: copy '<copy>' [(<why it is the hologram>)]` (once per change), `<trike | truck | battery_pack |
defense_pack>: collision turned off on the copy: flag 1 -> 0, physics instances n -> 0 of m parts`, `copy '<key>':
collision found ON again (...; #n): turning it off again`, `copy '<key>' refused for this game run: <its collision could
not be turned off | kept coming back | is unreadable>` (the hologram stands in) and a status line `status: solid
copies: ...`. Since section 22.12 (wear, lights, cargo): `our <key>'s state: wear w on n part(s) (spread s), m light(s):
<part>=<value> ...; e entries, k left out [(changed)]`, `our vehicle's carrier: ...` / `our vehicle <ptr> carries n cargo
piece(s) drawn (...)`, `our vehicle's state / cargo blocks do not encode (...): sent without them`, `wear / lights of the
partner's <key> on copy '<copy>': wear w on n part(s), m light(s); k variable(s) set` (each full apply, the first 30) or
`... : FAILED | looks OFF: <why>`, `the partner's <key>: its wear and lights are not applied (copy '<copy>' has another
model)`, in hidden tests / the trace `wear / lights read back on copy '<copy>': wear entries n (n as sent, n not), lights
n of n as sent, gauge colours n of n as sent` (gauge colours since 22.13), `the partner's vehicle carries n cargo piece(s)
(k left out by the sender)`, `cargo prop i '<case_s | case_m | ...>' loading / placed at (x, y, z) in the vehicle's frame:
read back d m from where it was put / shown / hidden`, `cargo prop i '<key>': colour variation v applied | partly applied
| NOT applied`, `n cargo prop(s) removed (<why>)`, `cargo prop '<key>' refused: <why>; cargo props off for this game run`.
Since section 22.13: `<key or path>: collision turned off on the copy: ...` also for the gear props (partner_gear: case
props, weapons, boots, the pod; the first 40 calls of the session are logged, gear props first), `<key>: collision found
ON again (flag f, n physics instances of m parts): turning it off again` (packs, cargo props, gear props: checked every
second), `pack copy '<key>' refused for this game run: its collision <could not be kept off | kept coming back>`, `[gear]
prop i '<key>' refused for this session: its collision <could not be turned off | could not be kept off | kept coming
back>`, `spawned 60 m below (the watcher stands within 40 m of the usual spawn point)` (the first 5), `the partner's
vehicle changed from <key> to <key> (another model on copy '<copy>'): the copy comes back in its own look`, `our parked
vehicle <ptr> still reported after 60 s`, `the partner's parked <key> still shown after 60 s (copy '<copy>')`, `our
parked vehicle <ptr> kept after n ms without a send tick (our Sam slept: a menu)`.

* `action vehicle_sync 0`: six status lines: ours; the partner's copy and the pose targets; the seat meter over the
  session and overlap hiding; `status: looks ...; packs ...; [look] ...`; `status: wear / lights: ours ..., n applies on
  the copy (n full, n of changes only; n variables set, n failed, n read-back mismatches); cargo: ...` (full / changes
  split since 22.13: a full apply sets everything, every 2 s and after each look apply); `status: solid copies: collision
  switch ok, n turned off, n found on again, n failures, n checks of packs / cargo / gear props, n spawned 60 m below (the
  watcher near); refused at some point: ...`. The result text is a one-line summary.
* `action vehicle_sync 1 <0|1|2|3|4>`: the mode at run time (0 removes the copy and stops sending; 1 (the default) and 2
  the solid trike and truck copies with looks and packs, collision off on each, and the roadster's hologram; 3 all three
  kinds as construction holograms (v0.13's default, opt-in); 4 the roadster's copy only, the first milestone's default;
  switching between 3 and the others respawns the copy). Every mode but 0 sends what we ride.
* `action vehicle_sync 2 <0..5>`: show the partner's vehicle as copy 1 roadster hologram, 2 trike (solid), 3 truck
  (solid), 4 trike hologram, 5 truck hologram whatever it is (0 = by its kind; the override ignores the mode). No joint pose on another skeleton. A solid copy shown
  through the override has its collision turned off too. Hidden tests of the copies (only the roadster is reachable at
  the 100% save's start).
* `action vehicle_sync 3 <ms>`: trace for that long: `trace tx t=<QPC ms> pos=(...)` every tick on the sender (our
  vehicle) and `trace rx play=<sender ms> now=... placed=(...)` on the receiver; on one PC both games share the QPC
  clock, so the placement error is the receiver's placement against the sender's own transform at the play time.
* `action vehicle_sync 4 <right cm> <forward cm>` / `6 <up cm>`: an offset of the copy in the vehicle's frame (origin
  tuning; 0 by default).
* `action vehicle_sync 5`: describe our current / last vehicle and the copy (components, resource, skeleton).
* `action vehicle_sync 7`: the game's vehicle registry (`registry n: <class> <ptr> '<resource>' (<allow-list key>) kind,
  ride state, flags, at (x, y, z), d m from Sam, forward`) and the parking manager's 9 slots (raw; research). The World's
  entity list never holds vehicles (the first milestone's 7 / 8 read it).
* `action vehicle_sync 8 [side m] [kind]`: hidden tests: place Sam `side` m (0 = 1.8) beside the nearest registered
  vehicle (kind 0 any within 40 m; 1 roadster, 2 truck, 3 trike, 4 the Kusanagi trike within 400 m), on the side he is
  on, facing it (its Use Vehicle prompt needs that: run `20261118-151711`), at its height; beyond 3 m snapped to the
  ground (a watching point). X then gets him on.
* `action vehicle_sync 10 <ms>`: hidden tests (automation runs only): all our sends held that long (pose, joints, vehicle,
  gear, ...), the link stays up: the other side sees a silent partner (a link stall, a load, a pause).
* `action vehicle_sync 11 <0|1>`: overlap hiding off / on at run time (`vehicle_overlap_hide`).
* `action vehicle_sync 12 [m] [kind]`: hidden tests: a watching point `m` m (0 = 12) back along the game camera's forward
  from a point 8 m ahead of the nearest such vehicle, 4 m to its right, snapped to the ground.
* `action vehicle_sync 13`: the meters logged now (a segment boundary).
* `action vehicle_sync 16 [m] [x shift]` / `17`: hidden tests of the copies' collision: 16 places Sam `m` m (0 = 3) before
  the live copy (moved `x shift` m on world x: in echo -4 is our own vehicle, the control) along the camera's forward,
  facing it (`16 0.01`: at its origin, inside its body); 17 logs where he is along that line, beside it, and his height
  over its origin (`dz`: about 1 m on the solid trike while its collision was on, v0.13; 0.11 m since section 22.11).
* `action vehicle_sync 21`: the live copy's collision state (`collision state: copy '<key>' live: flag f, n physics
  instances of m parts; our own vehicle <ptr>: flag f, n instances of m parts; collision offs ...`; our own vehicle is
  the control: flag 1, 15 instances of 37 parts for the garage roadster).
* `action vehicle_sync 22 <forward m> [right m]` / `23`: hidden tests (automation runs only): the copy pinned at our own
  vehicle's place plus that offset in its frame (`pin: the copy pinned ... at (x, y, z), forward (fx, fy)`), so we can
  drive into it; 23 lets it follow its frames again.
* `action vehicle_sync 18 <kind> [deg]`: hidden tests: every tick checks whether the game camera looks at the nearest
  registered vehicle of that kind (5: the partner's vehicle copy; 6: the real shared vehicle the partner drives here or
  drove last, which is not in the registry's list while the NPC controller holds it) and logs `camera aimed #n ...` once it does within 25 deg
  (or within `deg`, 1..25: a walk-in test, where Sam walks where the camera looks)
  (`until 25 150 camera aimed #n => pad none 200 rx=32767` turns the camera onto it; 100 ms pulses barely move it; `#n`
  counts per game; for 6 deg: first 25 with 200 ms pulses, then `vehicle_sync 18 <kind> 6` with `pad none 60
  rx=26000` pulses, about 0.6 deg each: scenario `two-games-veh-block`).
* `action vehicle_sync 14 <0|1>` / `15 <0|1>`: the partner vehicle's look / packs off / on at run time (`vehicle_look`,
  `vehicle_packs`; looks off respawns the copy in its own look, packs off removes the pack copies).
* `action vehicle_sync 24 <n> [model]`: hidden tests (automation runs only): n synthetic cargo pieces (0..32) of
  EDSBaggage2Model `model` (default 2, a medium case; 1 small) added to the cargo of the vehicle we report, laid out on
  its roof (truck, 2.75 m up) or behind the rider (trike 1.3 m, roadster 1.45 m), colour variations 0..3 of the baggage
  config; they go out in STM1 version 3's cargo block like real pieces (docs/real-partner-body.md section 22.12; the
  100% save has no delivery cargo to load). `24 0` takes them off again (the receiver's case props go).
* `action vehicle_sync 19 [kind]`: hidden tests (automation runs only): the census of every registered vehicle of that kind
  (0 all, as 8) within 400 m and of the live copy: its look (`[look] ... variation ... / var <hash> ... part ...`), its
  resource's variation references with their offsets, its baggage carrier (`census ... slot i type t: n stack(s), n
  piece(s)` and each piece's model and place in the vehicle's frame) and its child entities with their resources.
* `action vehicle_sync 9 <x> <y>`: hidden tests on scratch saves: place Sam at x, y at his own height
  (`TeleportMode::place`). Beside the parked roadster: `9 -1094.4 276.9` (X then gets him on).

Scenarios: `vehicle-echo-trike` (role echo: the roadster's meter without and with the smoothing, then the trike, the
Kusanagi trike and the truck, the truck disposed of), `vehicle-echo-collision` (walks into the solid copies and the
holograms on the garage floor; v0.13), `vehicle-echo-solid` (section 22.11: walks through the solid copies with their
collision off, Sam at their origins, our own roadster driven through a solid copy pinned ahead of it), `two-games-vehicle-trike` (all three kinds both ways, overlap, held sends), `vehicle-garage-scout` (the registry,
the garage lift), `vehicle-echo` (role echo: our own roadster comes back through STM1, the copy 2 s behind and 4 m beside),
`two-games-vehicle` (host Sam drives, the guest Fragile watches), `two-games-vehicle-guest` (the guest drives, the host
watches).


### Real partner body (`real_probe`, `real_spawn`, `real_remove`, `real_names`, `real_stats`, `real_mirror`, `real_control`, `self_view`, `real_outfit`; scenarios only)

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
* `action self_view <0/1> [body id] [follow]`: the self view (docs/real-partner-body.md section 8). 1: a body
  (`body id`, 0 = this game's `my_character`) is shown in place of the local Sam on this screen; 0: off, the
  body goes and Sam is shown. `follow`: how the body follows Sam (1 Sam's own model update copies his world
  transform, v0.18's default; 2 the body's own update; 3 the tick only; 4 the default since v0.19: mode 1 plus, in the
  body's own update, Sam's world predicted for this frame (docs/real-partner-body.md section 26); 0 unchanged).
  `real_stats` reports it too, and a `[self]` line is logged every second while the body exists (with follow 4 also
  `[self] follow 4: predicted N, after Sam N, skipped N ...; step cm mean / max`).
  `self_view 2 <reason 0..63> <0/1>` sets / clears one hide reason on the local Sam's Root mask (26 =
  photo mode's "hide Sam", 63 = a stray bit of ours, to test the repair). Only in hidden tests on
  redirected scratch saves; refused otherwise.
  `self_view 3 <0/1>`: the cargo menu's Sam figure (docs/real-partner-body.md section 15): 1 (the default) the
  self body takes its place while the on-foot cargo menu is open (Manage Cargo, menu kind 0, only; every other
  kind shows Sam), 0 the menu shows Sam. `self_view 5 <0/1>`: the self view fit (docs/real-partner-body.md
  section 16): 1 (the default, session.ini `self_fit`) Sam's hidden skeleton takes the character's bone
  lengths while the body is shown, so his gear, cargo and boots sit on the character; 0 Sam's proportions.
  `self_view 6 <0/1>`: the fit's same pose (section 16.1): 1 (the default) Sam's hidden skeleton takes the pose
  the self body was last written from, so the gear stays on the body while moving; 0 Sam's newest pose (the
  gear runs one frame ahead of the body: up to 20 cm at the hands running at 30 fps). `self_view 7 <percent>`
  (0-150, default 100): the fit's head lift, how far Sam's head joint (the cap, the glasses) sits from her head
  back towards his own (0: her head; the cap sinks over her eyes). The `[self] fit` line every second has the
  meter (Sam vs body per joint, mean / max), the leak check and the lift.
  `self_view 8|9 <which> <mm>` (-100..100; which: 0 every item of the cap class, 1 every pair of glasses, 10-17 one
  item: 10 eyewear0, 11-13 glasses00-02, 14 cap00 the Bridges cap, 15 cap01 the Santa hat, 16 cap02 the headcrab, 17
  cap03 the hair ornament): the headgear offset up (8) / forward (9) in Sam's head helper's frame
  (docs/real-partner-body.md sections 20 and 26.3), at run time (it then stays for later shows). `self_view 10 <0/1>`: the per-item headgear offsets (1, the default with `self_headgear=1`) or the v0.11
  head lift (0: `self_view 7`'s lift applies again, the movers go back to the identity). `self_view 11 <percent>`
  (1-100, default 1): her feet's scale under Sam's equipped boots (section 21); `self_view 12 <0/1>`: her feet
  scaled (1, the default with `self_feet_hide=1`) or kept. `self_view 13 <0/1>`: follow mode 4's rule, 1 v0.20's (the
  default: the step also while driving, the consistency gate, the dt scale) or 0 v0.19's (no step while Sam has a
  parent; for before / after pictures). The `[self] follow 4 meter` lines score both on the same frames
  (docs/real-partner-body.md section 26.6). `self_view 14 <mask>`: the gear placement fixes of v0.21 (section 26.7):
  bit 1 the self view's props (the BB pod) also placed in the self body's own update, bit 2 the tick's placement
  extrapolated one frame in the body's frame; 3 the default, 0 v0.20's (for before / after runs). Hidden test runs
  only: `self_view 15 <0/1>`, the drawn-vs-anchor meter (`[gear] drawn vs anchor <partner|self>` lines; off with a
  one-line reason when Entity::Update's stamp sites do not match).
  `self_view 16 <0/1>`: Sam's head items onto her drawn head every frame (docs/real-partner-body.md section 26.8): 1
  (the default) v0.22, 0 the plain offset of v0.21 (for before / after runs).
  `self_view 17 <mask>`: the self view over loads (docs/real-partner-body.md sections 33 and 34): bit 1 the load rule
  (not shown through a load or the post-load Setup; the show waits for a fresh game update of Sam), bit 2 the private
  room's close-up hold (Sam stays in `PrivateRoom|Cutscene` right after a load until the game state changes) and the
  stall guard (in that state, Sam shown when the game stops moving him; retired in round 11, off by default), bit 4 the
  clothes rule (Sam shown while the game changes his clothes, the body back 0.5 s after); 5 the default, 3 v0.25, 0 v0.24
  (before / after runs).
  `self_view 18 <mode>`: her room outfit (section 34): 0 never the room body, 1 the rule (the default: her black suit in
  the private room with `room_outfit=black`), 2 always the room body (even with `room_outfit=suit`), 3 always her base
  body. 2 and 3 change her body at once (a visible swap when she is shown, never in a cutscene or a menu).
  Hidden test runs only: `self_view 19 <request>`: Sam's body variant request (his streaming control +0x76, the byte the
  private room's uniform menu writes): 0..7 a variant (0 the porter suit, 1 the private room's clothes), 8 the game's own
  choice (the node-graph variable: the room's clothes in the room, the suit in the field). The switch then runs as the
  wardrobe's does (`[clothes]` lines: start, each state, the end with its duration, the paint effect and
  DSPlayerLoadingModel). Byte-checked with the clothes rule (`[clothes] Sam's clothes change readable`).
  `self_view 20 <mask>`: Fragile's face (docs/real-partner-body.md section 36): bit 1 Sam's facial clip on her self view's
  face, bit 2 the face block sent with our joint frames and the received one played on the partner body's face; 3 the
  default, 0 off (the faces hold their default pose, the v0.25 look). `self_view 21`: the face sync's status line
  (`[face] face sync mask ...`: the byte checks, Sam's facial tables, the plays, landings and faults on each face, the
  partner's last received id).
  `self_view 4`: the
  subspace work object's state and menu kind, and every ArtPart of the menu's figure (asked / effective
  visible, top-level, children) in the log. In hidden tests a `[menu] subspace state ...` line is logged on
  every change (with `(opens N)`, the times the menu reached its open state) and a `[menu] figure ...` line
  every second while the menu is anywhere but idle.
* `action real_outfit <mode> [a] [b]`: the partner Sam's outfit (docs/real-partner-body.md section 9,
  `native/mod/partner_outfit.cpp`). The modes:
  * 0: log our own Sam's outfit (inventory bytes, exos index, variant) and the art-parts variations of
    our Sam and of the slot's and self bodies.
  * 1: dress the slot's Sam body (a debug `real_spawn 1`, or the partner) like our own Sam, continuously.
  * 2: dress it with exos `a` (0..43) and suit colour `b` (0..16); anything else = none.
  * 3: override off; the partner's own outfit applies again.
  * 4: overwrite our own Sam's suit colour byte with `a` (0..16); the game re-dresses Sam itself at once.
    Only in hidden tests on redirected scratch saves; refused otherwise.
  * 5 `c` (v0.23, docs/real-partner-body.md section 27): Fragile's porter suit on our `fragile_suit` bodies (the slot's
    and the self view's) in colour `c` (0..16; 0 her own suit); `-1` = follow the suit colour again (purely visual).
  * 6: Fragile's suit: each `fragile_suit` body's drawable and meshes, and the state (`[suit] suit: slot ...; self ...`).
  * 7 `m` `ch` (tests): which meshes the binds use (`-1` the product's, `-2` the whole model at once, else a bit mask)
    and channels (`ch` bits 1 Color, 2 Reflectance, 4 Roughness; 0 the product's); rebinds.
  * 8 `v` `r` (tests): the metals' look: variant `v` (0 the product's, 1 Sam's measured sets, 2 = 0) and roughness `r`
    (-1 the variant's); rebinds.
  Log lines: `[suit] Fragile's suit colours available|unavailable: <reason>`, `[suit] constant texture <rgb> made by the
  game's loader ...`, `[suit] body <e>: colour N on (#k: <channels>; albedo ..., reflectance ... roughness ...)`,
  `[suit] body <e>: colour N taken off (<n> override(s) undone, <why>)`, `[suit] ... FAULTED` (the feature off).

Scenarios: `real-body-diag`, `real-body-tour`, `real-body-tour-all` (every table id, solo), `self-view`, `self-view-edges`, `self-view-menu`, `outfit-probe`, `fragile-suit-colours`, `fragile-suit-tune` (solo), `two-games-real-self`, `two-games-outfit`, `two-games-fragile-suit` (role host), `echo-real-100`, `echo-real-fragile-100` (role echo),
`two-games-real`, `two-games-real-fallback` (role host, two games). `real_spawn` takes any id in the table,
offered or not (the debug way to prove a new body before it gets `offered`). The partner's character in a
session comes from the peer's `my_character`; `session.ini partner_character` overrides it for debugging
(docs/real-partner-body.md section 5).

### Coop session (`coop_session`; `session.ini host_save=1`)

The host-held save session (docs/coop-session.md). `action coop_session 0` logs the status (`[coop] status: ...`):
host phase, load counter, the offer's phase and progress, the partner's phase and loaded save, both game builds; on
the guest the host's phase, the bytes received, the save loaded, the gate's blocks. `2` (host) offers the newest save
now (as for a late join), `3` (guest) accepts the one-time "Teleport to host" offer (the Teleport to partner action),
`4` (host) logs the save slots and their write times. Hidden runs only (refused otherwise): `1 <ms>` drops every
received datagram for ms (`0` ends it), `5 <type> <index>` the pause menu's Load on the host (`0x142636990`), `6`
QuitGame (`0x142642B20`), `7` the checkpoint retry request (`0x14362D750` with index -2, as the Game Over screen's
Retry; the host's detection sees it as a Retry), `8 <0|1>` the guest's waiting panel hidden (the input block and the
gate stay). The menu's header shows the session's one-line status and, after a late join, the **Teleport to host**
button.

### Vehicle share, Vehicle dump, Vehicle summon (`vehicle_share`, `vehicle_dump`, `vehicle_summon`; `session.ini vehicle_sync=5` with `host_save=1`)

The real shared vehicles (docs/real-partner-body.md section 23). `action vehicle_share 0` logs the status
(`[vshare] status: ...`): role, on / off and why, the world, the poll, messages sent / received, the gate's counters
(passed, the mod's own, the guest's load window, online dropped, forwarded, dropped) and the victim chooser's; on the
guest the table, bindings, mirrors per path, removals and re-placements; on the host the snapshots, ADD / REM and the
guest's requests. `1 [tag]` = `vehicle_dump`, `2 <kind> [m]` = `vehicle_summon`, `3` a snapshot now (guest: asks; host:
sends one unasked), `4` (guest) the last binding plan per vehicle, `7` the gate and eviction counters. Hidden runs only:
`5 <0..3>` the guest's mirror path (0 auto, 1 exact-id only, 2 fallback spawn only, 3 re-place only), `6 <kind> [m]` the
removal bit on the nearest vehicle of that kind within m (default 60).

`action vehicle_dump [tag]` logs every in-use unit of the roadster, truck and trike pools (`[vshare] unit T<tag>: ...`:
pool index, id, actor state, flags, +0x19C bytes, area, rider, GUID, life, battery, entity, place, and on the guest its
binding) and a summary line (`[vshare] dump T<tag>: N units in use (roadster a, truck b, trike c) ...`). It works
without the sync too (a diagnostic), when the pool layout's bytes match.

`action vehicle_summon <kind> [m]` (1 roadster, 2 truck, 3 trike; m ahead of Sam, default 8) asks for a vehicle through
the game's own request path: a parking manager slot of that kind through the garage slot spawn `0x142D5CB20`, else
`0x142F830A0` with a record like one of this world's vehicles of that kind. On the guest its gate drops it and forwards it
to the host (the whole summon path of M2.3).

`action vehicle_share 8 [kind] [m]` (host) logs the game's own persisted entry of every live vehicle of that kind (0 any)
within m (default 400) of Sam: what the host's next save writes for it (`[vshare] persisted entry of ...: place ..., x m
from where it stands now`).

### Vehicle drive (`vehicle_drive`; `session.ini vehicle_sync=5` with `host_save=1`)

Remote driving, ownership and state on the real shared vehicles (docs/real-partner-body.md sections 23.11 - 23.21).
`action vehicle_drive 0` logs the status (`[vdrive] status: ...`): on / off and why, frames sent (DRIVE, PARK) and
received (refused: not from the owner), followers attached / detached / failed, VOWN messages, claims, grants, denies,
forced ride-offs, input updates, the battery probe. `1` one line per vehicle with drive state: owner and epoch, the
follower, our Sam on it, frames, the placement error mean / max and angle, corrections per band, life / battery writes.
`2 <0|1> [0|1]` the native net correction off / on (the second value: on parked vehicles too; off by default, they are teleported), `3 <parked m> <driven m>` the ignore bands (0 = 0.1 / 0.3), `5 <0|1>`
native lamps and sounds (as `vehicle_real_native`), `8` the battery probe. Hidden runs only: `4 <0|1>` corrections off /
on (the open-loop replay), `6 <kind> [m]` moves the nearest vehicle of that kind m (0 = 0.6) sideways with RequestTeleport
(a push), `7 <life>` sets the life of the vehicle our Sam drives (else the nearest within 30 m), `9 [m]` places our Sam m
(0 = 9) to the left of the vehicle the partner drives here, facing it (snapped to the ground; with a third value 1
  placed at the vehicle's height instead, for the underground garage) (then `vehicle_sync 18 6` aims the camera at it).
v0.21 (the driven follower's controller, docs/real-partner-body.md section 23.25): `13 <0|1>` v0.20's follower (lerp
target, sample-and-hold inputs, bands and 100 ms nudge, measured velocities in the frames; set it on both games for a
"before" run), `15 <ms>` the input lead (inputs of the frame nearest to play time + ms, 16 by default), `16 <kp> <ki>`,
`17 <kyaw> <tau s>`, `18 <max dv m/s a tick> <max correction m/s>`, `19 <s>` logs the controller every tick for s seconds
(`[vdrive] trace: ...`), `20 <+1|-1> <max rad>` the side-error pursuit's sign and limit. Hidden runs only: `10 <dv>` the
next DRIVE frame's velocity x off by dv (one odd frame, as play test 3's false crash), `11 <fwd m> <right m>` moves the
follower's target for 1.5 s and logs the step response every tick (`[vdrive] step ...`), `12 <m/s> <laps>` the owner's
autopilot (its virtual pad) on play test 3's road from the start area's garage down past the big hologram (0 = off; at
the end of a lap it brakes and, with laps left, puts the vehicle back at the start; `[vdrive] autopilot: done (#n)`),
`14 <place>` puts the vehicle our Sam drives (else the nearest within 30 m) at 1 the road's start, 2 the top of its
downhill, 3 its bottom, heading along the road. `21 <amount> <period s>` (hidden runs) adds a slalom to the
autopilot's steering (`amount` of full lock, 0 = off), after `12`.

### Riding together (`ride_state`, `ride_board`, `ride_exit`, `seat_measure`, `ride_test`; `session.ini ride_together=1` with `vehicle_sync=5`)

Carried passengers in the shared real vehicles (docs/real-partner-body.md section 24). `action ride_state` logs the state
(`[ride] state: ...`): on / off and why, the passenger side (idle, requesting, riding: the vehicle and ride id), the
driver side (carrying: the ride id), boards, exits, requests, denies, accepts, ENDs sent / received, dropped messages, the
pose override's writes, the input block's counters, the capsule's counter and on byte, the seats in use (a second line:
the native prompt, on / off and why, the partner's vehicle id, lifted, riding, the provider's passes, lifts and RideOff
passes, the presses taken from TryStart, the game's last answer per slot). `ride_board`
boards the vehicle the partner drives here now (a completed hold: every boarding check applies). `ride_exit [1]` gets off
now (by choice: under 2 m/s; 1 also above, hidden tests; in the game a second hold within the 3 s wait above 2 m/s
jumps off). `seat_measure` logs, for the vehicle our Sam drives (or the
last one measured), his root and pelvis in its frame, its joint 1 and the game's seat helpers through it, the passenger
seat from the helpers and the one in use, and keeps his seated pose for the solo test. `ride_test <op>` (hidden tests):
`1` carries our Sam on the measured vehicle as a passenger is carried (no partner, the kept pose), `2` ends it, `4 [dv]` a
crash of dv m/s (0 = 12) next tick while riding, `5` rolls the vehicle our Sam drives over (45 x 4 deg about its forward
axis by RequestTeleport), `6` stops the vehicle our Sam drives dead (RequestTeleport onto its own place), `7` the game
takes our Sam next tick, `8 [m]` places our Sam at the passenger door of the vehicle the partner drives here, facing it,
`9` the riding module's emergency stop (riding stays off), `10` asks to ride (as `ride_board`) and runs the emergency
stop in the same call (the driver's ACCEPT then always arrives after the stop: it is ended at once, never a carry), `11`
logs the boarding check now (`[ride] test: boarding now: <block>`; the action succeeds either way), `12 <0|1>` turns the
game's own "Use Vehicle" prompt off (the ImGui hold prompt boards, as when its byte checks fail) or back on, `31` / `32` / `33 <m>` the x / y / z of the measured kind's
passenger seat (this session), `30` the seat table again. v0.21 (section 24.13): `15` logs the landing shield, the health
guard, our Sam's health and the crash rule; `16 <0|1>` the landing shield and the health guard off (v0.20: the game hurts
or kills a carried Sam) / on; `17 <0|1>` v0.20's one-frame crash rule (exit and the D3-3 hit for every kind) / the
confirmed one; `18 <0|1>` arms a write watchpoint (debug register 0 on every thread) on our Sam's health for the next
carry and logs each write's instruction and the code on its stack (`[ride] test: health write #n by ds.exe+...`;
disarmed 5 s after the ride). The passenger's pose and camera (v0.21, docs/real-partner-body.md sections 24.14 / 24.15):
`20 <0|1>` the passenger's ride camera off (the on-foot camera) / on, `21 <0|1>` the camera meter (`[ridecam] passenger|driver
...`) every 5 s / every second, `22 <0|1>` the passenger's pose v0.20 (the driver's live pose) / the seated idle (now, if
carried), `23 <what> <value>` the seated idle's tuning (0 the arms 0/1, 1 along the thigh 0..1, 2 up m, 3 inward m, 4 the
forearm twist deg), `24` logs the seated idle's state, `25 <0|1>` the camera's collision against the ridden vehicle on (v0.20)
/ ignored, `26 <0|1>` (hidden tests) the seat teleport held while the vehicle stands, `27 <0|1|2>` the vehicle's yaw
quaternion for the camera off / on / its conjugate, `28 <0|1>` the heading's slow follow off / on. `29 <0|1>` (hidden tests)
the seat back after a save off (v0.20: the save's exit leaves our Sam on foot) / on (docs/real-partner-body.md 24.16).
The passenger's boots (v0.22, docs/real-partner-body.md 24.17): `34 <0|1>` the boots meter (`[ride] boots meter: life ... (main
layer bare, kind state, item), +/- in 1 s; queued; on-foot flag, move level, speed; status +0x140 +0x144; skipped; events
j/t/l/s/p ..., dropped ...`) off / every second, `34 2` a mark (`[ride] boots mark #n: life ... (unchanged|dropped|rose
+/-... since mark #n-1, ... s)`), `35 <0|1>` (hidden tests) the boots guard off (v0.21: a carried Sam's boots wear) / on.
The trike driver's back (v0.23, docs/real-partner-body.md 24.18; `session.ini ride_hide_pack`, default 1): `36 <1|0|-1>`
(hidden tests) the hide forced on (our Sam's inventory gets the game's HideBackpack + HideSpareShoes now, ride or not) /
forced off / back to following the ride (`[ride] test: the trike pillion hide forced ON | forced off | follows the ride`).
`ride_state` logs a third line: `[ride] state: trike pillion hide on|OFF, held 0|1 (N hide(s), M release(s)), test t,
inventory +0x4C7C 0xNN` (the byte: bit 0 HideBackpack, bit 1 HideSpareShoes; the game's own HideBaggage event writes bits
0..4). The driver's game logs `[ride] trike pillion: the driver's backpack and cargo hidden (... 0x00 -> 0x03; #n)` and
`... shown again (<why>) after s (...; #n)` (why: the ride is over, the passenger's body is not seated, a new Sam, test,
the mod stopped); the passenger's `[gear] the partner's backpack props hidden (our Sam rides on the partner's trike
pillion)` / `... follow its frame again`. `gear 0`'s carrier line counts the pieces the game hides (`n hidden by the game:
hide mask 0xM`), and every change of that count is logged (`[gear] our carried cargo: n of m pieces hidden by the game
now (carrier hide mask 0xM)`): hidden pieces are not sent (v0.22 sent every carried piece as shown).

### Save guard (`save_guard`; with riding together, or in a hidden run)

No save of the game captures our Sam while he rides as a passenger (docs/real-partner-body.md section 24.12): the game's
save system's capture kick `0x14362DB80` is hooked: a save's capture waits (at most 3 s) until the ride has put him beside the vehicle (the save processor runs unchanged).
`action save_guard 0` logs the state (`[save] state: ...`): on / off and why, the hold, the pending request, the save
system's state, the counters (requests, at once, held, released, capped, issued, dropped by the game, gone, finished ok /
not ok, waited for a held capture, capped unplaced), the processor's, the kick's and the tick's threads. `save_guard 2` logs our Sam's place, his parent, and his distance from
where the last held save released him (in the frame the vehicle had then); `save_guard 2 <n>` logs the same as
`[save] where (check n): ...`, so a scenario's `waitlog` (which searches the whole log) matches that one call. Hidden runs only: `1 [type] [1]` a save now
through the game's own DSCreateSaveGame (type 4 the next autosave, 1 manual; into the run's scratch copy; the game may
refuse it: its reasons are logged; a third value 1 makes a refusal succeed for a scenario's `until`; 2 the same plus a
second DSCreateSaveGame at once while the first is pending: the log says whether the holder still has the same request), `3` the pause
menu's Load of the run's newest save (host; coop_session 5), `4 0/1` the hold off / on, `5` the game's refusal reasons
now (its query `0x1433FEA20`, the autosave cooldown, the save queue), `6 [s] [1|2]` the next forced save stays pending until
our Sam boards (1: and sleeps in a menu; 2: and the vehicle drives at 8 m/s or more), `7 [s]` the same without letting anyone board.

### Vehicle hits (`vehicle_hit`; `session.ini partner_vehicle_damage=1` with `vehicle_sync=5`)

The partner's vehicle hitting our Sam (docs/real-partner-body.md section 25). `action vehicle_hit 0` logs the state
(`[vhit] state: ...`): on / off and why, the direct path / the state-4 bracket / the damage-handler detour, the calls per
kind and path, faults, hits, Sam's damage messages (all, of our attack, capped, preliminary, other), STH1 sent / received,
the forced paths, the partner capsule's vehicle boxes, our health. `vehicle_hit 1 <kind> <0 | 1 | 2>` forces a kind's
path (auto: direct for every kind, the bracket only when the direct path's checks failed; direct, bracket; this
session). `vehicle_hit 2` (hidden tests) one hit on our Sam now from the vehicle the
partner drives here (the crash sphere, the direct path). `vehicle_hit 8 [m ahead] [m right]` (hidden tests) puts our Sam
that far in front of the vehicle the partner drives here (default 15 m), facing it. `vehicle_hit 9` logs the vehicle boxes
the partner capsule rule sees. `vehicle_hit 10 <kind> <m>` (hidden tests, the owner's game) puts the nearest free vehicle of
that kind |m| m in front of the vehicle the partner drives here (else of our Sam) by RequestTeleport, facing it (negative m:
broadside). `vehicle_hit 11` logs the life (`+0x6E8` / `+0x6EC`) and place of every shared vehicle within 300 m (compare
both games). `vehicle_hit 12 <kind> <m>` (hidden tests) puts the nearest free vehicle of that kind m along the nearest free
truck's forward (negative: behind it), on its heading.

### World sync (`world_dump`, `world_sync`, `dev_build`, `dev_build_remove`, `dev_build_decay`, `dev_build_mats`, `dev_ladder`, `dev_rope`; ziplines W4; on with `host_save=1` unless `world_sync=0`)

Shared structures (docs/world-sync.md). All log `[world] ...` lines. `world_dump` and `world_sync` work in any session
(read only, except `world_sync 1` / `5`); the `dev_build*` tools are dev tools (`Tool::structure`: sandboxed saves only;
on a co-op guest they ask the host).

| Action | Args | What |
|---|---|---|
| `world_dump` | `[category or -1] [n] [net: 0 all, 1 stage, 2 player, 3 net]` | The census: counts per category x net type (Stage / Player / Net), offline / uploaded / removing, the id counters and the creation queue; the nearest n (default 16) with id, type, state, level, place, durability, materials, offline, server id, UUID. |
| `world_sync 0` | | Status: on / off and why, world, census, the partner's HELLO, the guest in the host's world / the snapshot, bound / mapped / pending mirrors / held builds, the gate. |
| `world_sync 1` | | Guest: SNAP_REQ now (a heal); host: CHECK now. |
| `world_sync 2` | | The guest's binding (host id -> guest id for mapped ones). |
| `world_sync 3` | | Counters: messages sent / received by type, bad / stale / repeats, mirrors (exact, mapped, FAIL), created for the guest, removals, refusals, held builds; W2: `[world] counters W2:` contributions applied / dropped / retries, level-ups queued (host, guest), the guest's level-up requests swallowed, double requests dropped, our MATS (not shared, pending), STATE applied (pending), durabilities applied, excess; then the online gate's `[gate] counts:` line. |
| `world_sync 4 <id or 0>` | | One structure (0 = the nearest shared one): state, level, place, durability, own / others' materials, flag bytes, offline, server id, entity; for a contributable one (W2) also `[world] materials id N (hid H) <type>: level L of MAX, status S (enough / missing / max level / no level-up / a level-up runs)[, running][, queued]; invested a/b/c/d/e/f, the next level needs ... in all (... still), repair ..., list n, dur D of MAX; reaches R`. The six materials are chiral crystals, resins, metals, ceramics, chemicals, special alloys. |
| `world_sync 5 <0/1>` | | Hidden runs: hold the guest's GBUILDs back (retry tests). |
| `world_sync 10 <type or -1> <m>` | | Count check near our Sam: `[world] check: type T within r m: n (ids ...) offline n level L` (-1 = every shared type). |
| `world_sync 11 <id or 0>` | | Record round trip: the game's ExportInfo twice, the Record back into a record, the live fields: `EQUAL` / `DIFFERS`. |
| `world_sync 12 <id or 0> [m]` | | W3, hidden runs: Sam to a structure (0 = the nearest shared one, ladders / ropes too), `m` (default 1) short of it on the line from where he stands, through `dev_goto` (then `waitworld 45 goto`): a ladder's / rope's prompts ("Hold START: Collapse Ladder / Recover Rope") show only within reach. Log `[world] to structure id N (type) at (...): Sam to (...)`. |
| `world_sync 13` | | The online gate: on / off, its vtable slots, its counts. |
| `world_sync 14 <id, 0 or -1>` | | W3 probe: watch a structure (0 = the nearest ladder / rope): every census logs a change of its state or flag bytes cp+0xA8..+0xB1, or `GONE from the live list` (how a pick-up looks: run `20261121-174141`). -1 stops. |
| `world_sync 15 <id or 0> [deg]` | | W3, hidden runs: from now on every tick checks whether the game camera looks at a structure (0 = the nearest ladder / rope of any owner; a rope: 6 m down its direction, the edge it hangs over) within `deg` (default 15) and logs `[world] camera aimed #n` once (a scenario turns the camera with `until 45 300 \[world\] camera aimed #1 => pad none 120 rx=32767`, as `vehicle_sync 18`). |
| `world_sync 16 <id or 0> [m]` | | W4: a zipline's links, one line each (0 = every zipline within `m`, default 300, of our Sam): `[world] zipline id N (hid H, zipline, net n, level L, state S) at (...): k link(s): <ids>`. The links are the game's own (the zipline's activation links it to every zipline of its area within the config's range, cp+0x528); the same ziplines link the same in both worlds. |
| `dev_build` | `<type> <anim 0/1> [m ahead, default 8]` | A structure in front of Sam through the game's own queue (InitInfo, NewId, owner, +0x645 and +0x623 with anim, the printer at the structure), always offline. Player types: 3 safe house, 6 postbox, 9 watchtower, 12 catapult, 15 generator, 18 timefall shelter, 31 bridge 30 m, 34 bridge 45 m, 41 chiral bridge, 44 jump ramp, 22 zipline (W4). On the guest: the record goes through the EnqueueInfo detour like her PCC commit (held) and a dev GBUILD asks the host. |
| `dev_ladder` | `<length m, 0 = 9.65> <mode 0/1> [m ahead, default 3]` | W3. A ladder (type 25) as the player's commit places one: InitInfo, level, the durability from the ladder config (+0xA0[level]), +0x578 mode, +0x57C / +0x580 lengths, +0x623 / +0x645, then **the game's own placement check** 0x142C38200 (the build preview's): it snaps the transform and fills the geometry; the first of Sam's front at a few distances and turns the game accepts is taken (`[world] dev ladder: the game's placement check accepts try N ...`), else the action fails (a spot the check refuses is removed by the ladder's own re-check a moment later). Offline; on the guest a dev GBUILD as `dev_build`. |
| `dev_rope` | `<length 1..30 m> <level 0..2> [m ahead, default 3]` | W3. A climbing anchor with its rope (type 28): as `dev_ladder`, with +0x578 length, +0x57C direction (0, 1, 0: the rope ExportInfo's default) and +0x588 longest 30 m. Needs an edge (the check refuses flat ground): `goto -1153.4 338.0` (the 100% save has an online rope there) works. |
| `dev_build_remove` | `<id or 0> [1 dismantle / 0 the removal bits]` | 0 = the nearest shared structure, always a dismantle (RemoveManually, through the detour: the guest sends REMREQ). |
| `dev_build_decay` | `<id or 0> <seconds>` | Host / solo: the structure's timefall durability (cp+0xD8). W4: a shared ladder's / rope's too; the host's DUR carries it to the guest. |
| `dev_build_mats` | `<id or 0> <0 materials / 1 repair> <share or units>` | W2. The terminal's commit without the UI or the inventory. Materials: `<share>` (0 < f <= 1, default 1) of what the next level still needs, through AddOwn (our detour: on the guest it goes to the host as MATS), then, at Status 0, the terminal's QueueLevelUp (through the detour: swallowed on the guest, the host's call). Repair: `<units>` (default 10) of the material that repairs this type most (config +0x210), the repair commit's durability step (cp vt+0x140) and AddRepair. Log `[world] dev contribution: id N (hid H, type) level L: materials ... -> status S[, level-up requested ...]` / `[world] dev repair: ... -> durability A -> B s`. |
## Dev tools (`dev_*`; `session.ini dev_tools=1`, the launcher's Debug page "Dev tools"; always on in hidden runs)

Spawning, world control and player cheats for play-testing and for hidden scenarios. Code: `native/mod/dev_tools.{hpp,cpp}`
(`st::dev`), the rules in `native/mod/dev_rules.hpp` (unit-tested by `st_dev_rules_tests`), the vehicles in
`native/mod/vehicle_share.cpp` (`DevSpawn`, `DevRemove`, `DevRepair`). In game: the debug menu's **Dev tools** tab (F9; mouse
and pad), on the host and on the guest. Every tool is one debug action, so scenarios use the same calls as the buttons.

### The real-save rule (`dev_rules.hpp` `Decide`)

| Tool class | Tools | Solo / echo, or host with `host_save=0` | Host, `host_save=1` | Guest, `host_save=1` |
|---|---|---|---|---|
| player | refill, god mode, infinite ammo, places, teleports | any save | any save | yes |
| world | weather, MULEs / BTs, boots, chiral crystals | sandboxed save only (that world only) | sandboxed save only, the host's world only | its own world only (a copy of the host's save, never kept) |
| vehicle spawn | `dev_vehicle` | sandboxed save only | sandboxed save only; shared with the guest (M2) | asked of the host's game, which spawns it only when **its** dev tools are on and **its** saves are sandboxed |
| vehicle edit | remove, repair | sandboxed save only | sandboxed save only (a removal reaches the guest) | refused: shared vehicles live in the host's world |

"Sandboxed" = `automation::SavesRedirected()`: the launcher's Local test (and Play with test save), a hidden run, or a co-op
guest. The online host with `host_save=1` plays on its real saves, so there every world tool is refused with
`refused: this game plays on its real save and this changes what the save keeps (use the launcher's Local test with a test
save)`. The page shows each class's state (green / amber with the reason) and greys its buttons. A guest's dev vehicle
request carries `vwire::ReqReason::dev` (3) in the SVM1 request; the host logs `[vshare] host: the guest's dev request #N
refused: refused by the host: its game plays on its real save` (or `... its dev tools are off`) and sends a refused result
(the guest's "could not summon" notice). A v0.18 host's decoder refuses reason 3 outright: nothing spawns.

### Actions

| Action | Numbers | What it does (proof run) |
|---|---|---|
| `dev_status` | `0` | Logs `[dev] status: <class> tools: allowed / <reason>` for the four classes, and `[dev] status: time of day: allowed / <reason>` (the Player tab's time actions' real-save rule). `1 1` / `1 0` (hidden runs) acts as if this game played on its real save / back. `2` (hidden runs) selects the open menu's Dev tools tab. `3 <m>` (hidden runs) puts Sam m metres up (fall tests). |
| `dev_vehicle` | `kind variant [m]` | A vehicle `m` metres ahead (default 8, 3..40): the game's spawn `0x142F830A0(R, record, xf, 1, 0, 0)` with a fresh record `rec0 = kind | variant << 6`, `rec2 = 0`. Refused when that kind's pool has no free unit (the game would evict a vehicle the save holds). 2.5 s later `[dev] vehicle check: new <kind> 0x<id>, unit variant N (asked N), entity ... +0x4C8 N`. All 14 pairs proven in `20261120-041615-dev-tools-solo`; host-shared and guest-asked in `20261120-041914-two-games-dev-tools`. |
| `dev_vehicle_remove` | `[m] [1]` | The game's removal bit (`unit+0x198 |= 0x2000`) on the nearest vehicle within m (default 15) nobody is on; with `1` the newest dev vehicle instead (the page's "Remove last spawned"). "Nobody" includes the partner on the host of a shared-vehicle session: a vehicle the guest drives (the host game's rider field stays empty, the guest drives it through the mod) or rides is skipped (the shared-active set's riding mark, or vdrive's follower for the partner); "last" on it is refused with `the partner is on it`. |
| `dev_vehicle_fix` | `[m]` | Repair and recharge the nearest vehicle within m (default 10), ridden or not: the game's own race-reset sequence (`0x142D6ED80`): vt `+0x118` Repair (per kind), battery `B = [e+0x898]`, `B+8 = B+0xC`, `0x142F5A260(B)`, `B+0x34 = 0`. Proven: the garage roadster's life 8271 -> 12500 (`20261120-035314`). |
| `dev_refill` | | `RecoverFull` body `0x142738940(Sam, 0)` (the export `0x1426E6F50` passes flags 0): health, stamina, battery, the BB, paint / frost. Status, equipment and parameter objects (`Sam+0x54E0`, `+0x54C8`, `+0x54C0`) are checked non-null first. Stamina 942 -> 1000 in `035314`. |
| `dev_god` | `0 / 1 / 2` | The retail `DebugSettings` (`[0x147BE6A50]`, RTTI checked) `+0x24` EGodMode: 0 off, 1 on, 2 invulnerable. Restored by `dev_god 0`; the game never saves it. **Not a fall guard**: a 40 m fall killed Sam with 1 and with 2 (`035314`, `041615`: health 0, knot space): a fatal fall is not a damage message. |
| `dev_ammo` | `0 / 1` | `DebugSettings +0x28` infinite ammo (read by the weapon code at `0x1435BD545`). |
| `dev_weather` | `0` clear, `1` timefall, `2` heavy, `3` the game's mix | `SetForecastTypeOfRegionDirectEx 0x142E1FE20(area, region, type)` for the 64 regions of the current area (types 6 SunnyOnly, 3 RainyOnly, 5 HeavyRainOnly, 2 Average), then `ResetForecastTime 0x142E1FC80(1)`: the sky follows within seconds (rain and Sam's hood in `041615` 04, clear in 05). Without the reset nothing visible changed in 9 s (`035801`). |
| `dev_enemies` | `1` / `2` / `3` | 1: `ResetAndResurrectAllMulesByMissionChange` (`0x142B64420`): every killed MULE and terrorist (one manager, 150 members in the 100% save) comes back at its camp. 2: `RemoveAllMulesFromWorld(0)` (`0x142B645F0`). 3: `ResetAllGazers` (`0x1429A7220`): the BT fields start again. There is no "spawn an enemy here" in the game: go to a camp or a BT area with the place list. |
| `dev_places` | | The current area's delivery points (`DSMissionSystem [0x147C0AFF8] +0x78820`: `Array<Ref<DSDeliveryPointInfoResource>>`, u32 count, u32 capacity, data at `+8`; entry RTTI checked; `+0x68` area, `+0x6A` type, `+0x78` order, `+0x80` transform), nearest first, to the log. The name is the entry's own localized text (`DescriptionText +0x20`, the text pointer inside the LocalizedTextResource), read at runtime (nothing shipped); without one, the type's name from the game's `EDSConstructionPointType` RTTI. Area 200 of the 100% save: Mountain Knot City, the preppers, MULE camps, terrorist camps, Distribution Center North of Mountain Knot City, Lake Knot City ... (more than 64; the page lists the nearest 64). |
| `dev_place` | `n` | Teleport (PlaceOn mode 1, facing the place's forward) to entry n of that list. Only within the current area: the game has no cross-area teleport (story points: test saves, docs/sandbox-and-local-test.md). |
| `dev_items` | `1` / `2 n` | 1: `RecoverOrSupplyShoes` body `0x142744B40(status, 0, 1)` (new or repaired boots). 2: `AddChiralCrystalCount(n)` (`0x1426E6530`, default 100; 18753 -> 19253 in `035314`). |
| `dev_goto` | `x y [z]` | Sam anywhere in this area (sandboxed saves only: world class), then the ground (docs/dev-tools.md "Go anywhere"). z 0 / left out: the topmost ground (PlaceOn mode 2 at the ray centre 500); else PlaceOn mode 1 at z + 0.5. Refused while the placement gate is shut (on a vehicle, a load, a menu). Every tick `devrules::GotoAdvance`: within 25 m of x, y (a place's point inside a terminal: the game pushes Sam out, 3.5-8.3 m seen) and 5 m of z, playable and still for 1.5 s: `[dev] goto #n: settled at (...) after N s (k placement(s))`; more than 5 m below z: placed again (at most 4, 1 s apart); 25 s: `NOT settled`. The scenario's `goto` and `waitworld <s> goto` wait for it. 7 places up to 4 km apart in `20261121-084610-goto-places`. |
| `dev_boots` | `0` / `1 p` / `2 0/1` / `3 t` | 3 (round 6 T3; sandboxed saves only): equip boot type t (EDSPlayerShoesType 0..10: 0 barefoot, 2 sole grass, 3 porter, 4-6 all-terrain Lv1-3, 7 mule, 8 terrorist, 9 chiral, 10 reflect) with the game's `SetShoesForcibly` `0x1426E7430` (the request byte status `+0x3B8`; the status tick `0x14273251B` equips it): `[dev] boots: SetShoesForcibly(t) requested (worn type now n; ...)`, then `[boots] our boots' ArtParts: 32, shown <hex>` (the shape: docs/real-partner-body.md section 30.2). 0: the boots' durability to the log. 1: set it to p % (sandboxed saves only): `SetShoesLifeByRate`'s body `0x142741860(status, p/100, 0)` with Sam's status `[Sam+0x54E0]` read and checked first. 2: a `[dev] boots meter: ...; +x in 1 s` line every second (hidden tests: the passenger's boots). The durability is two parts (the setter `0x142741720(status+0x320, v)`): the first `+0x334` (200) points at `+0x33C`, the rest at `+0x32C` up to `+0x330` (3400); 40 % = 1440 of 3600 (`20261121-084433-spawn-showcase`: 200 + 926.6 before, 200 + 1240 after). Refused without worn boots (`+0x368` empty: the game's setter would do nothing). |

Time of day and teleport to the partner are the existing actions (`set_time_of_day`, `teleport_to_partner`); the page has
buttons for them (Dawn / Noon / Dusk / Night). The time buttons sit under the world tools' gate with the weather (greyed on a
real save), and the action itself refuses on a real save from every tab ("Time of day" above).

### Story tools (`story`, `story_load`; docs/dev-tools.md "Story tools")

Code: `native/mod/story_state.{hpp,cpp}` (`st::story`, ticked from `dev::Tick`), the rules in `native/mod/story_rules.hpp`
(`st_story_rules_tests`). The page's **Story** section (top of the Dev tools tab) shows the state line, the manual slots of the
sandbox (story points) and a Load button.

| Action | Numbers | What it does (proof run) |
|---|---|---|
| `story` | `0` | The story state to the log: `[story] summary: Episode 2 "Amelie" \| in progress: Order 6 (sub, id 65), Order 5 (main, id 60) \| main done 4/60 (highest Order 4, 1 started), sub done 0/37 \| crc 0x41CFDF14; facts: episode index -2, episode hash 0x...`. Read only (any session with the dev tools on). |
| `story` | `1` | Every main / sub order: `[story] mission id 60 Order 5 (main, episode 2): order started (2), state in progress (4)`. |
| `story` | `2` | The sandbox's manual slots with their `[Slot]` title and detail (`[story] slot manualsave13 (story library): Order In Progress: No. 5 ...`). |
| `story` | `3` | The load's gate here: `[story] load gate: allowed` or the refusal (scenarios check refusals with it: a refused `story_load` fails its step). |
| `story_load` | `n` | The game's own Load Game of `manualsave<n>` (`0x142636990(app, 1, n)` after the slot refresh `0x141A00DA0(0)` and the exists check `0x1426376E0`): in a world only, sandboxed saves only, never on a host-held save session's guest. Logs `[story] load of manualsave<n>: in the world after N s` and `[story] after the load of manualsave<n>: <state>`. A host with `host_save=1` offers the load to the guest (M1's in-world Load detection). `20261120-045106-story-points`, `20261120-044929-two-games-story`. |

The place list and the page's view are refreshed once a second on the game thread, and only while the world is played (Sam
awake, nothing loading, no stopping game event): `GetCurrentArea`'s slow path, called during a load, held the game thread for
200 s (runs `20261120-040412` / `040828`, the guest's load).

### Byte checks (`dev_tools.cpp kSites` + `vehicle_share.cpp kDevSites`)

On the dev tools' first tick (`dev::Check`, which also runs `vshare::DevCheckResult`): `[dev] dev tools: 31/31 byte checks ok;
on: refill, god mode / ammo, weather, MULEs, BTs, places, boots, chiral crystals` and `[dev] vehicles: pools ok, spawn ok,
repair ok, variant read-back ok`. A mismatch turns that tool off with `[dev] byte check FAILED: <site>` and the page shows
`Off (game build differs): <tool>: <site>` (the vehicle tools' as `vehicle tools / spawning / repair: <site>`; their buttons
are greyed: no pools greys spawn, remove and repair, no spawn path greys spawn, no Repair greys "Repair nearest"). A Repair
or battery notify that faults at run time turns repair off for the run with the same line.

| Check | Address | Gates |
|---|---|---|
| RecoverFull body (status / equipment / params offsets), its export, the battery body | `0x142738940`, `0x1426E6F50`, `0x1427E1010` | refill |
| god mode readers (`>= 1`, `== 2`), infinite ammo reader, SetGodMode, EnableInfiniteAmmo; the DebugSettings global's RIP target; RTTI `DebugSettings` `0x144F10E90` | `0x1434F6EFE`, `0x1434F6F43`, `0x1435BD53E`, `0x1438282B0`, `0x143828300` | god mode / ammo |
| SetForecastTypeOfRegionDirectEx, its table row, ResetForecastTime, the weather global, GetCurrentArea | `0x142E1FE20`, `0x142E1FE79`, `0x142E1FC80`, `0x142E1FE34`, `0x14262A860` | weather (and places: area) |
| MULE resurrect thunk / body / its player-object call, remove-all thunk / body; the MULE manager global | `0x142B64420`, `0x142B168F0`, `0x142B169BA`, `0x142B645F0`, `0x142B140C0` | MULEs |
| ResetAllGazers thunk / body, the gazer global | `0x1429A7220`, `0x1429A17A0` | BTs |
| GetDeliveryPointInfoResources (DSMissionSystem global `+0x78820`); RTTI `DSDeliveryPointInfoResource` `0x144DD5E50`; enum `EDSConstructionPointType` `0x144C33550` (names only) | `0x1433CA80A` | places |
| RecoverOrSupplyShoes export; AddChiralCrystalCount (DSPlayerSystem global) | `0x1426E6F80`, `0x1426E6530` | boots, chiral crystals |
| SetShoesLifeByRate export (status `+0x54E0`, r8 0, jump to the body), its body (`+0x368`, the r8 test), its `(+0x334 + +0x330) x rate` and setter call, the setter (`+0x1C` / `+0xC` / `+0x8C`, flags `+0x90`), SetShoesForcibly (type `+0x3B8`) | `0x1426E7460`, `0x142741860`, `0x1427418EB`, `0x142741720`, `0x142741757`, `0x1426E7430` | boots' life (`dev_boots`) |
| the race reset's repair + battery sequence, the three Repair functions, the battery notify, each vehicle vtable `+0x118` = its Repair | `0x142D6EE21`, `0x142F7B090`, `0x142F7B140`, `0x142F7B320`, `0x142F5A260` | vehicle repair |
| the variant write (unit `+0x198 >> 22` -> entity `+0x4C8`) | `0x142F77023` | the check's read-back only |
| the pools, the unit layout, the spawn and its record (vehicle_share's `kCore` / `kSpawnG` sites, checked here when the shared vehicles are off) | see section 23 | vehicle spawn / remove / repair |

### Not done

* No cross-area travel (the script exports `DSMissionLogicCommand_sFastTravel` -> `0x1433C9A10` and `sChangeArea` -> `0x1433CB320`
  are leads, not decompiled: Ghidra was down in round 4); `dev_goto` stays within the save's area.
* No free camera (photo mode's pan range and `SetObserverPositionOverride` are the leads, docs research only), no "MULE
  here" (moving a live MULE to Sam through its actor's `vt+0x40`: unproven), no cargo or weapon giving (the baggage export's
  name codes are not decoded), no mission start / skip (story points are saves of the story library loaded with
  `story_load`: docs/dev-tools.md "Story tools" says why).
* God mode does not survive a fatal fall (above).
* The vehicle variants' names are not known: the page shows "truck, variant 6". From the screenshots: truck 0 grey, truck 6
  yellow; trike 7 is the one the 100% save holds (`0x01E005`), likely the Kusanagi.

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
| Entity::SetUpdateFrequency (prologue to the regroup call; lock `0x147BC4828`, World `0x147BC4E70`, regroup `0x1423FC0E0` targets) / scheduler regroup prologue | `0x235F5A0`, `0x23FC0E0` | smooth (240 Hz boost) |
| Vehicle classes: vtable slot 0 = the RTTI getter (`DSVehicleSpeedCar` / `DSVehicleTruck` / `DSVehicleMotorbike`), the constructors' `+0x4C4` kind writes, the type / ride state reads (`+0x4C4`, `+0x4CC`), Entity_ExportedGetParent (`+0x70`) | `0x3E274D8`/`0x2F6BC10`, `0x3E26790`/`0x2F6BCB0`, `0x3E25DB8`/`0x2F6BB60`; `0x2F62201`, `0x2F62571`, `0x2F61CFC`; `0x2FA73B6`, `0x2FA74B8`, `0x2361B80` | vehicle sync (sender) |

Regenerate the hex with `DS.load().rd(va, n).hex()` from `tools/re/dsre.py` after a game
patch, re-verifying each meaning first. The save guard's own 18 checks are in docs/real-partner-body.md section 24.12.

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

## Fragile's textures in the game (`tex`; round 6 T2; `session.ini fragile_textures=1`, default on)

`action tex <mode> [a] [b]` (Tools tab, scenarios; docs/textures.md, docs/real-partner-body.md section 29.3). Modes 0, 1,
4 and 7 work in any session; the others only in a hidden run (refused otherwise).

* `tex 0`: the state to the log: `[tex] bodies: self ... look <id8> overrides N entries N ...; partner ...` (both
  bodies), `[tex] link: ...` (STT1 counters: messages, chunks and blobs sent / received, refused, cache hits),
  `[tex] status: ...` (ring size, rebuilds and slots kept / dropped, clones made / failed, the filter mode, the worker's
  state and the looks it settled), one line per clone and per filtered override entry.
* `tex 1`: rescan the textures folder now (as the hot reload does after a save).
* `tex 2 [n]` (hidden): dump the slots of our override entries in the next n (default 2) rebuilds: per slot the texture
  it holds before ours goes in (`S0`..`B2` = one of the slot table's originals, `OURS`, `other(<guid8>)`) and after.
* `tex 3 <c>` (hidden): our own Fragile bodies use colour folder c (0..16; -1 = follow the suit colour).
* `tex 4 0|1`: the feature off (her looks undone at once) / on again.
* `tex 5 0|1|2` (hidden): the slot filter: 0 the product (her suit's slots only), 1 keep every slot (the v0.23
  behaviour: shows the pattern on her boots and neck too), 2 = 1 and the probe dump.
* `tex 6` (hidden): release at once every texture of ours nobody used for 0.6 s (the 5 s rule of `textures_free=1`, default).
* `tex 7`: the guest sends its look again; the host lets go of the partner look's RAM copies and shows the newest LOOK
  again at once (its blobs are read from the disk cache when a body needs them): `[tex] host: tex 7: N of the guest's M
  blob(s) let go of from RAM ...; look #E shown again`.
* `tex 9 <n>` (hidden): an in-memory test pattern on our own Fragile bodies (no files, no game data): 1 a magenta /
  green checker on her suit (S0), 2 also yellow / black stripes for her boots' colour (B1, the boots rule, T3), 3 the
  boots stripes only; 0 off.
* `tex 10 <t>` (hidden; round 6 T3): her boots' pictures follow boot type t (0..10; -1 = our Sam's).
* `tex 11 <v>` (hidden; round 8): the metal variant of the ready-made gold / silver (0 the product: since round 9 the
  polished metal, Sam's measured colours and Roughness x1.15; 1 "Sam's hue", 2 "Sam measured", 3 the round-7 prototype
  (satin, Roughness x0.75: generator 1's product, kept for side-by-side comparisons); docs/textures.md "Ready-made
  colours"); the looks are rebuilt.
  The variant is in the built cache key and the RECIPE, so the host makes the same one (a host whose build knows fewer
  variants refuses the RECIPE and asks for the blob).
* `tex 12 <m>` (hidden; round 8, host): how a guest's ready-made colour (STT1 RECIPE) is made: 0 here (the product),
  1 never here (always asked from the guest), 2 refused as if this game's Original pictures differed (the fallback test).

Log lines the scenarios match: `[tex] ready: manifest table <hex> ok`, `[tex] look <id8> for colour <c> (<folder>): <k>
slot(s) edited (<slots>)` (colour 0 or `textures_defaults=0`), round 8: `[tex] look <id8> for colour <c> (<folder>):
ready-made (<slots>)` / `yours: <k> slot(s) (<slots>)[, ready-made <slots>]`, `[tex] ready-made <slot> <id8> for colour
<c> made in <ms> ms (analysis <a> ms, make + encode <e> ms)` / `... from the cache`, `[tex] ready-made colours off: <why>`,
`[tex] look #<n> sent: colour <c>, <k> blob(s) (<r> ready-made: recipe sent); ...` (guest), `[tex] partner look #<n>:
<slot> <id8> is ready-made colour <c> (generator <g>): making it here`, `[tex] partner ready-made <slot> <id8> made here
in <ms> ms: the same as the guest's (no transfer)[; no longer wanted (kept in the cache)]` / `... not made here (<why>): asking
the guest for it`, `[tex] partner look #<n>: <k> ready-made blob(s) of an older look no longer made here`, `[tex] host: the
partner's ready-made colours come over the link (about a minute each) because <why>: press Extract textures ...` (host),
`[tex] built <slot> <id8> in <ms> ms`, `[tex] clone <slot> <id8>: hw ...`, `[tex] body
self|partner|debug ...: look <id8> bound (...), rebuild kept <n> / dropped <m> slot(s)`, `[tex] body ...: look ... taken
off`, `[tex] reload: colour <c>'s pictures changed`, `[tex] boots look <id8> for boots <n>[ worn][ gold|silver]: <folder> (<slots>)` / `... nothing edited (her own boots)`,
`[tex] her boots: type <t>[, worn] (look key <k>, boots_look=own|prop)`, `[tex] look #<n> sent: colour <c>, <k> blob(s);
boots <t>[ worn], her own boots|Sam's boots (prop)` (guest), `[tex] partner boots: her own boots (no prop) ...` (host), `[tex]
partner look #<n>: received <slot> <id8> (<bytes> B in <ms> ms, <rate> KB/s) valid`, `[tex] partner look <id8>: complete
for colour <c> (<k> blob(s), cache hit <h>, made here <m>)` (host), `[tex] clone <slot> <id8>: hw ... released (unused for 5 s | tex 6; ...)`, `[tex] refused <id8>: <reason>`, `[tex] off: <reason>`, `[tex] ...
FAULTED`.

## Fragile's textures: the launcher's command-line test actions (round 6, T1)

The extractor is driven from the launcher's command line (docs/textures.md "For developers", docs/launcher.md
"Command line"):

* `StrandedTogether.exe --extract-textures --out <dir> [--game-dir <game>] [--no-previews]`: the Tools card's Extract
  textures, without the window; `--out` must be the folder the launcher's settings name.
* `--allow-out-under <dir>` (tests, the hidden check): any `--out` inside `<dir>`, never the user's Documents, Desktop,
  Pictures, OneDrive or the user profile outside the temp folder.
* `--cancel-after-sets <n>` (tests): the process ends itself in the middle of set n with a `.partial` on disk (exit 3),
  to prove the next run cleans up.
* `--check-textures --out <dir>`: the Check my edits report (`check ...` lines).
* The hidden check: `tools/testenv/Test-TextureExtract.ps1` (docs/hidden-testing.md).
