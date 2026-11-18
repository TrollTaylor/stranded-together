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
  cap, 0x0D / 0x0E glasses) sets the first such entry's worn flag: the game puts the cap on or takes it off
  itself.
* `action gear 15 <0..24>` (hidden test runs on scratch saves only): our cap colour byte (inv `+0x4BE0`:
  0..22 a colour index, 23 silver, 24 gold); the game re-colours the cap itself.
* `action gear 17 <0/1>`: weapons in hand and on the backpack (docs/real-partner-body.md section 17) not sent /
  sent (default on; the partner's props follow). The result reports the weapon watch: changes seen, how many of
  their reads waited for the watch's 40 ms gap, and the time from a draw / holster to the frame that carries it
  (mean, max). Weapons with no prop on the other screen are not sent (logged once each).
* `action gear 16 <0/1>`: Sam's head parts (hair, hood, face lines; docs/real-partner-body.md section 14) not
  named / named in our frames (default on). Off only stops naming them, so a partner body keeps what it had
  last: to see the cutscene defaults, set 0 before the body comes. The result says whether the last read was
  taken or held (not the porter suit, Sam hidden by a part mask, the hood switch animation) and how many parts
  were set on our partner body. `gear 14 53 0` takes the cyphead accessory off (the metal-line hair and face
  lines).

Scenarios: `gear-self`, `two-games-gear`, `two-games-all`, `gear-full`, `two-games-full-gear`, `two-games-gear-look`,
`echo-hair`, `two-games-hair`, `two-games-looks-menu`.

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
  transform, the default; 2 the body's own update; 3 the tick only; 0 unchanged). `real_stats` reports it too,
  and a `[self]` line is logged every second while the body exists.
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

Scenarios: `real-body-diag`, `real-body-tour`, `real-body-tour-all` (every table id, solo), `self-view`, `self-view-edges`, `self-view-menu`, `outfit-probe` (solo), `two-games-real-self`, `two-games-outfit` (role host), `echo-real-100`, `echo-real-fragile-100` (role echo),
`two-games-real`, `two-games-real-fallback` (role host, two games). `real_spawn` takes any id in the table,
offered or not (the debug way to prove a new body before it gets `offered`). The partner's character in a
session comes from the peer's `my_character`; `session.ini partner_character` overrides it for debugging
(docs/real-partner-body.md section 5).

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
