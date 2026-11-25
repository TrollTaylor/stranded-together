# Dev tools

The dev tools (vehicles, player cheats, weather, enemies, items, places) and their real-save rule are documented in
[debug-actions.md](debug-actions.md) "Dev tools". This file covers the **story tools**: reading where the story stands, and
going to a story point, built as groundwork for a later story sync (story sync itself is not built).

## Hidden tests: quickstart, go anywhere, spawn (v0.22)

For hidden runs (docs/hidden-testing.md "Quickstart, go anywhere, inline steps, held sessions"). Every world-changing tool
is the `world` class of `dev_rules.hpp` `Decide`: sandboxed saves only (a hidden run, a Local test on a test save), never a real
save; `dev_status 1 1` acts as a real save to prove the refusal.

### Go anywhere (`dev_goto`, scenario `goto`)

| What | How |
|---|---|
| Sam to a named place | `goto <place>` (tools\testenv\places.txt: 20 places of area 200 from the game's own delivery points) |
| Sam to coordinates | `goto x y z`, or `goto x y` for the topmost ground under x, y |
| The partner (the other game's Sam) | `@guest goto <place>`; both: `@both goto <place>` |
| A vehicle at a place | `goto <place>`, then `action dev_vehicle <kind> <variant> <m>` (spawned m metres ahead of Sam) |
| A new place | stand there, `action log_positions` (or `dev_places`: the area's delivery points), add a line to places.txt |
| Beside a structure | `action dev_place <n> <kind>` (the n-th nearest; kind 20 MULE post, 5 post box, -1 delivery base), then `waitworld 45 goto` |
| A map marker | `action dev_marker 1 <m east> <m north>` (hidden runs: the game's own add), then `action teleport_to_map_marker`, `waitworld 45 goto` |

Within the area the save is in (complete-100: area 200 in every slot). Far jumps stream in after the teleport: the mod places
Sam again when he falls more than 5 m below the target (ground not loaded yet) and the step waits until he stands there; 3.3 to
8 s for 0.1 to 4 km (`20261121-084610-goto-places`). Gaps: no cross-area travel yet (leads in debug-actions.md "Not done");
places of area 100 (Capital / Port Knot City) need a save there (`dev_places` on the story pack's early slots); "Fragile's
safehouse" is not a delivery point of area 200: which spot is meant is open.

Places and markers (round 18, docs/debug-actions.md "Dev tools" `dev_place` and "Teleport to map marker"): a place teleport
lands Sam beside the structure, at its stand point or on a ring around its collision box, facing it, and checks he is
outside the box once settled (a post box or MULE post used to swallow him: `place-teleport.txt` keeps the old landing as a
control). The menu's **Teleport to map marker** (Player tab, also on this page) puts him on the ground at the newest map
marker or the one picked; `marker-teleport.txt` adds markers through the game's own add and teleports to a near and a far one.

### Spawn and set (all proven in `20261121-084433-spawn-showcase`)

| What | Action |
|---|---|
| Truck / roadster / trike, any of the 14 variants | `dev_vehicle 2 0 10`, `dev_vehicle 1 0 10`, `dev_vehicle 3 0 10` (kind, variant, metres ahead); `dev_vehicle_remove 0 1` the newest |
| Weather | `dev_weather 0` clear, `1` timefall, `2` heavy timefall, `3` the game's mix |
| Time of day | `set_time_of_day <h>`; `freeze_time 0/1` (the cycle) |
| MULEs / terrorists / BTs | `goto terrorist-camp-nw` (or `mule-camp-sw-ckc`, `mule-camp-sw-lkc`), `dev_enemies 1` resurrect, `2` remove, `3` reset the BTs |
| A cap / hat | `gear 14 15 1` the Bridges cap (36 Santa hat, 49 headcrab, 50 hair ornament; `0` takes it off): the inventory's own worn flag, the game puts it on |
| Glasses | `gear 14 13 1` / `gear 14 14 1` (the two glasses types); `gear 14` lists the accessory entries |
| Cap colour | `gear 15 <0..24>` |
| Boots' durability | `dev_boots 0` read, `dev_boots 1 <percent>` set (SetShoesLifeByRate), `dev_boots 2 1` a 1 s meter, `dev_items 1` new boots |
| God mode / ammo / refill | `dev_god 2`, `dev_ammo 1`, `dev_refill` |
| A story point | `start story <n>` with `-SeedSaves story-pack`, or `story_load <n>` in the world |

Not done: cargo (`CreateAndAddBaggageToPlayer` resolves to `0x142CD6A60(u32 id, u8)`, its ids are not decoded), an enemy
"here" (`MultiSpawnpoint_ExportedSpawn` / `AIBehaviorGroup_ExportedSpawn` are leads), a vehicle moved to a place (spawn one
there instead).

## Story tools

### What works

| | How | Where |
|---|---|---|
| Read the story state (episode, the orders in progress, what is done) | `[story]` log line on every change, 1 Hz while in a world | host and guest, any session with the dev tools on (read only) |
| Dump it | `story 0` (summary), `story 1` (every main / sub order), `story 2` (the story points), `story 3` (may this game load one?) | debug actions, and the Dev tools page's buttons |
| Show it | the **Story** section at the top of the Dev tools tab (F9): the state line, the story points, **Load story point** | in game |
| Go to a story point | `story_load <n>`: the game's own Load Game of `manualsave<n>` | sandboxed saves only (Local test, hidden runs); never the guest of a host-held save session |
| Keep story points | `tools\testenv\Story-Library.ps1`: a library of saves at known story points, packed into one test-save folder | `testenv-saves\story` and `testenv-saves\story-pack` (git-ignored) |

Code: `native/mod/story_state.{hpp,cpp}` (`st::story`, ticked from `dev::Tick`), `native/mod/story_rules.hpp` (the pure part,
`st_story_rules_tests`), the rows `story` / `story_load` in `debug_actions.cpp`, the page in `overlay_menu.cpp`
(`StorySection`).

### The rule

Reading is allowed wherever the dev tools are on. **Loading a story point replaces the world a save keeps**, so
`storyrules::DecideLoad` allows it only when this game's saves are sandboxed (`automation::SavesRedirected()`, and not the
hidden-run real-save override `dev_status 1 1`):

| Session | `story_load` |
|---|---|
| a game on its real save (solo, the online host with `host_save=1`, a `host_save=0` pair) | refused: `refused: this game plays on its real save; loading a story point would replace your progress at the next autosave (use the launcher's Local test with a test save, or a hidden run)` |
| the guest of a host-held save session | refused: `refused on the guest: the host's game holds the world (load the story point on the host; its load comes to you)` |
| a sandboxed solo game or `host_save=0` pair | allowed (that world only) |
| the sandboxed host of a host-held save session (the Local test) | allowed; M1 offers the load to the guest, which follows |

The page shows the refusal in amber and greys the button. Nothing in the story tools writes a fact or a mission state.

### How the game keeps the story (proven)

Static research (decompiles in Ghidra, capstone over the on-disk ds.exe, the game data's RTTI enums), confirmed in the
hidden runs below. ds.exe DC, PE timestamp 1706443442.

* **Episode.** `DSMissionGoalsResource` at `[[[0x147C0B000] +0x28] +0x1F0]` (`DSMissionSystemResource` -> its data ->
  `MissionGoals`; null outside a world). Its `Episodes` array (+0x40 count, +0x48 data; 16 episode resources, index 0 the
  prologue) and two persistent facts: `CurrentEpisodeIndexFact` (+0xA0) and `CurrentEpisodeHashFact` (+0x68). Read with the
  FactDatabase int getter `0x141AFC710([0x145794720], &ctx 0x144DD9600, fact)` (a shared-lock lookup). `SetEpisode`
  (`0x14335F3A0`) writes the hash = CRC-32C (the `crc32` instruction, start 0, no inversion) of the episode GUID's two
  qwords and sets the index fact to -2; so the index wins when it is 0..15, else the hash is matched against the Episodes'
  hashes (`storyrules::ResolveEpisode`). Every `story 0` of the runs read index -2 and a hash that matched: Episode 15
  `0x7745F917`, Episode 2 `0x5FBA5950`, the values the research predicted.
* **Orders.** `DSMissionSystem [0x147C0AFF8]`, its main / sub list `[MS+0x618B0]` (`{i32 n, i32 cap, Resource** data}`;
  resource `+0x38` MissionId, `+0x40` Order No., `+0x44` category 0 main / 1 sub, `+0x46` < 2 main or sub, main `+0x1A0`
  EpisodeId) and the mission info by id `0x14333B210(MS+0xA8, id, 0xFF)`. The info has **two** states:
  * `+0x2A` u8 `EDSMissionOrderState`: 1 not started, 2 started, **4 completed** (the game's own `CountCompletedMissions`
    `0x1433CBD90` counts `== 4`). This is the story's progress.
  * `+0x2C` u16 `EDSMissionState`: the current attempt (**4 in progress** = the order being played;
    `CheckDSMissionProgressSimple` is `== 4`), 1 available, 16 success, ...
  The first probe counted "done" from `+0x2C == 16` and found 0 everywhere (run `20261120-044156`); the order state is the
  one that moves.
* `main_total` is 60 (category 0 includes orders beyond the 48 story orders of the data catalogue), `sub_total` 37.

The summary (`storyrules::Summary`, `story::GetSummary()`): episode, the facts as read, main done / total, the highest
completed Order No., main orders started, sub done / total, up to 6 orders in progress (id, Order No., main / sub), and a
CRC-32C over every (MissionId, order state, attempt state) in the game's list order.

### Story points: the story library

There is no chapter select to call: the 84 `*_debugentry` graphs are not in the archives and every `Debug*` script export
is the stub `0x1418E9B10` (`ret 0`). The safe way to a story point is a **save made at that point**, loaded by the game's
own Load Game.

```
pwsh -File tools\testenv\Story-Library.ps1 -List
pwsh -File tools\testenv\Story-Library.ps1 -Add -Source <slot folder or checkpoint.dat> -Label ep2-order05
pwsh -File tools\testenv\Story-Library.ps1 -Add -FromRun build-testenv\runs\<run> -Label ep3-start     # newest save the game wrote in that hidden run
pwsh -File tools\testenv\Story-Library.ps1 -Add -FromSandbox local-host -Label ep3-start              # newest save of a Local test sandbox
pwsh -File tools\testenv\Story-Library.ps1 -Pack
pwsh -File tools\testenv\Start-TestRun.ps1 -Scenario story-points -SeedSaves story-pack
pwsh -File tools\testenv\Story-Library.ps1 -Probe build-testenv\runs\<that run>                       # each entry's episode
```

* An entry is `testenv-saves\story\<NN>-<label>\checkpoint.dat` + `story.json` (title and detail from the save's `[Slot]`
  header, sha256, where it came from by kind and slot only: `own saves: manualsave3`, never a path or an id; the episode and
  state line after `-Probe`). Up to 10 entries.
* `-Pack` writes `testenv-saves\story-pack\DeathStrandingDC\manualsave<10+NN>\checkpoint.dat` (and `STORY-PACK.txt`): one
  folder in the test-save layout. Slots 10..19 stay clear of M1's guest slots 5 / 6.
* Every source is opened read-only and closed at once (refused while the game writes it: `checkpoint.dattemp`); the tool
  writes only below `testenv-saves\story` and `testenv-saves\story-pack`.
* **Making new points from sandboxed play:** play a Local test (or a hidden run) to the point you want, save, then
  `-FromSandbox local-host` (reads `<game>\stranded_together\sandbox\local-host\saves`) or `-FromRun <run>` (reads the run's
  `saves-scratch-changed`). Saves of another account need the foreign-save bypass, which hidden runs and the Local test have.

The library today (`-Probe 20261120-045106-story-points`):

| Slot | Entry | Story state after the load |
|---|---|---|
| manualsave10 | `prologue-order01` (own saves) | Prologue, Order 1 in progress, 0 done |
| manualsave11 | `ep1-order02` (own saves) | Episode 1, Order 2 in progress, 1 done |
| manualsave12 | `ep1-order03-done` (own saves) | Episode 2, nothing in progress, 3 done (Sam in the private room) |
| manualsave13 | `ep2-order05` (own saves) | Episode 2, Order 5 (main) and Order 6 (sub) in progress, 4 done |
| manualsave14 | `postgame-ep15` (complete-100) | Episode 15, 48/60 main and 34/37 sub done, highest Order 70 |

**Using it:** hidden runs `-SeedSaves story-pack`; the launcher's Local test: Debug > Test saves > Choose folder >
`testenv-saves\story-pack` (its layout is `<folder>\DeathStrandingDC\<slots>`). Continue loads the newest entry; then the
Dev tools page's Story section (or the game's own Load Game menu) goes to any other.

### `story_load`: how it loads

In a world only (session global set, a local player, nothing loading; menus and cutscenes are fine, as for M1's guest,
which calls the same function in any state). It checks the slot file below the sandbox's own save folder
(`SavesScratchRoot()\DeathStrandingDC\<account>\manualsave<n>`), then the slot cache refresh `0x141A00DA0(0)`, the exists
check `0x1426376E0(app, 1, n)` and the pause menu's Load Game `0x142636990(app, 1, n)` (all three proven by M1). The pending
load is followed: the world unloads (or the local player changes), then the new world must stand 3 s with its mission goals
before `[story] load of manualsave<n>: in the world after N s` and `[story] after the load of manualsave<n>: <state>`
(timeouts: 30 s to start, 180 s to arrive). At the title the game's own Load Game is the way (the request is refused there).
In a host-held save session the host's in-world Load hook sees it (`[coop] host: in-world Load (type 0x1, index 13 ...) ->
manualsave13 (load #2)`) and the guest follows.

### Why no mission jump

`RequestDSMissionStart` (`0x1433C9C30`), `RequestDSMissionSuccess` / `Fail`, `OpenMissions`, `SetEpisode` and the fact
setters exist and could be called, but every main order is a node graph scripted against the world the earlier orders left
(areas and the chiral network unlocked, NPC and BB state, cargo, cutscenes). A forced start or success skips none of that and
the next autosave keeps the half-set state: a soft-lock risk with nothing to gain over a save made at that point. They are
not built. For story-sync research later they are listed with their bytes in the research notes (the addresses above plus
`0x1433C9C30`, `0x1433C9C80`, `0x1433C9BF0`, `0x1433CFF90`, `0x14335F3A0`, `0x141AFCE50`); any such tool must stay
sandbox-only behind the same rule.

### What future story sync can use

* **Compare:** `story::GetSummary()` on both games (any thread). Two games at the same story point have the same `crc` (the
  two-game run: host and guest both `crc 0x41CFDF14` after the load). A sync can send `{episode, crc, main_done,
  highest_done_order, progress ids}` in the link's STATE message and show "story differs from the host" outside M1 sessions.
* **Bring the guest along:** in a host-held save session the guest already follows every host load (M1), so a story point
  the host loads, by `story_load` or the game's menu, reaches the guest (proven).
* **Diagnose:** `story 1` lists every order's order state and attempt state, the raw material for deciding what a sync
  would have to carry.
* **Test:** the story library gives repeatable starting points in hidden runs (`-SeedSaves story-pack`, `story_load <n>`).

### Byte checks (`story_state.cpp kSites`)

On the first tick: `[story] story tools: 27/27 byte checks ok; probe on, load on`. A mismatch turns off the probe or the load
(`[story] byte check FAILED: <site>`, the page shows `Off (game build differs): story probe: <site>`).

| Check | Address | Gates |
|---|---|---|
| SetEpisode body (the goals path `+0x28` / `+0x1F0`, Episodes `+0x40` / `+0x48`), its hash fact `+0x68` / context / FactDatabase / `crc32`; RIP targets `0x147C0B000`, `0x144DD9600`, `0x145794720` | `0x14335F3A0`, `0x14335F413` | probe |
| the episode getter (index fact `+0xA0`, the same context and FactDatabase as RIP targets) | `0x14333C820` | probe |
| FactDatabase int getter | `0x141AFC710` | probe |
| GetMainMissionIdArray (`[MS+0x618B0]`, `+0x46`, `+0x38`; RIP target `0x147C0AFF8`) | `0x1433CD980` | probe |
| mission info by id; CheckDSMissionProgressSimple (`MS+0xA8`, `+0x2C == 4`); GetMissionCategory (`+0x44`); CountCompletedMissions (`+0x2A == 4`; RIP targets `0x147C0AFF8`) | `0x14333B210`, `0x1433CA510`, `0x14335E670`, `0x1433CBDB5` | probe |
| the goals context GUID (16 data bytes); RTTI `DSMissionGoalsResource` `0x144DC5010`, `FactDatabase` `0x144885B30`, `DSMainMissionResource` `0x144DD7650` (objects are checked against them before every read) | `0x144DD9600` | probe |
| ContinueLastSave (the session `0x147BE6318` and app `0x147BE6330` globals), slot cache refresh, slot exists, pause-menu Load Game | `0x142642A00`, `0x141A00DA0`, `0x1426376E0`, `0x142636990` | load |

The game calls are SEH-wrapped as a last resort only; every pointer and object is checked first. A fault turns that part off
for the run.

### Proof runs (hidden, foreground, one at a time)

* `20261120-045106-story-points` (`-SeedSaves story-pack`): **pass**, 0 failures, game exit code 0, saves unchanged, restored,
  no violation, 0 cloud uploads. Continue (Episode 15), the slot list, the page (`01-dev-page-story`), all five story points
  loaded in 3.6..6.6 s each with their state (`10-manualsave10` .. `14-manualsave14`: "Prologue: Porter / [Order No. 1]
  Smart Drug Delivery" on the HUD, Sam in the private room for Episode 2, ...), `story 1`, the real-save refusal through
  `story 3`.
* `20261120-044929-two-games-story` (`host_save=1`, `-GuestSeed profile-only`): **pass**, 0 failures, both exit codes 0, saves
  unchanged, restored, no violation, 0 uploads. Both probes read Episode 15 `crc 0x7FB255DD`; the guest's gate refuses, the
  host's allows; the host's `story_load 13` is offered as load #2, the guest writes it as manualsave6 and loads it; both
  read Episode 2 `crc 0x41CFDF14`; the guest's page (`03-guest-dev-page`) shows the state and the refusal.
* `20261120-044156` and `-044646` (fails, fixed): "done" from the wrong state field; the load was refused in the private
  room by a stop-event gate the game's load does not need; `?action` still counts a refused load as a failure (hence `story 3`).

### Gaps

* No mission start / skip / episode set (above). No free story jump at the title (use the game's Load Game there).
* The library holds five points (the prologue to Order 5, and post-game); Episodes 3..14 need saves made in sandboxed play
  (`-FromSandbox` / `-FromRun`). Building them needs real play (bosses, cutscenes): not automated.
* A save's `[Slot]` title is in the language it was made in (complete-100's is Portuguese); non-ASCII shows as `?`.
* Reading the facts of a save file offline (without loading it) would need the save's compression format: not done; the
  library's states come from loading each entry (`-Probe`).
* Not run visibly with the launcher's Local test (the hidden two-game run uses the same mod paths with `host_save=1`).
