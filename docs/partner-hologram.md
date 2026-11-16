# Partner body: the welcome hologram

Status: **implemented and built, not yet run in game.** All addresses come from static analysis
of `ds.exe` (PE timestamp 1706443442, base `0x140000000`). They were re-checked against the exe
bytes on 2026-11-16, and the mod byte-checks each one at runtime before using it. The next
in-game run decides whether the hologram is visible. The [log guide](#what-to-look-for-in-the-log)
below lists what to check.

## Why a hologram

The first partner was the echo entity built from `DSPlayerEchoManagerConfig.EntityResource`, and it
is invisible. Its components are `DSEchoComponent`, `VoiceComponent` and `DSTalkComponent`. It has
no model: entity `+0xB8` is null, and the echo resource holds only sentence lists. The game also
hides its own echoes with `SetVisible(false)`.

A visible stand-in has to be a different entity. DS shows other players' structures with a
**welcome hologram**: a humanoid figure that turns its head toward Sam and gestures. The game
spawns it from the collectors-item database through its asynchronous entity spawner. The mod does
the same.

## Bodies and fallback

`session.ini` keys (all optional):

| Key | Default | Meaning |
|---|---|---|
| `partner_body` | `holo` | `holo`, `marker` or `echo`: the first body to try |
| `partner_holo` | `0` | collectors item `HashId` to show. Write it as the log prints it (8 hex digits) or as `0x...`; `0` means automatic |
| `partner_marker` | `-1` | index of the trace-point prop for the marker body; `-1` means the first usable one |
| `partner_voice` | `0` | `1` also spawns the invisible echo next to a holo or marker body, as its future voice source |

If a body cannot be found or spawned, the mod falls back along this chain and logs why:
**holo → marker → echo** (starting from the configured body). F7 or the debug menu's
**Respawn partner** restarts the chain from the configured body and forgets refused hologram items.

| Body | Code | What it is |
|---|---|---|
| holo | `native/mod/partner_holo.cpp` | Collectors / welcome hologram, spawned asynchronously, made visible with `StartVisibleWelcomeHolo` |
| marker | `partner_source.cpp` `AcquireMarker` | A trace-point prop (`DSPlayerTracePointConfig.EntityResources[i]`), created synchronously, `SetVisible(true)`. Static world marks, not a person |
| echo | `partner_source.cpp` `Acquire` | The echo resource: invisible, voice-capable (nothing plays a line yet) |

`player_sync.cpp` owns the chain and the teardown triggers. It moves whichever body is live with
`Entity_ExportedSetWorldTransform` every frame, using the interpolated pose.

## Hologram lifecycle (all on the game's main thread, from `st::sync::Tick`)

1. **Pick an item.** Each attempt re-reads the DB; item pointers are never kept across frames.
   - Read `db = [0x147C0E488]`, count `i32 db+0x10` (1..4096) and array `[db+0x18]`.
   - Keep only entries whose RTTI is `DSCollectorsItemResource` (`0x144DF3B80`). Read the `0xA0` bytes of each in one go.
   - The first time each DB is seen, log one compact table with HashId, NameHash, Type, DisplayNo, and the ref state (`-` none, `s` streamed out, `L:<type>` loaded).
   - Choose in this order:
     1. the item named by `partner_holo`;
     2. the game's current structure hologram: if `[cpm+0x56A7F0]` is set, the item at `[cpm+0x56A808]`, matched by pointer against the DB;
     3. the first item with `Type & 4` (Reward_Holo) and a non-empty ref. `Type == 4` is preferred, then the lowest DisplayNo.
   - Items refused or timed out twice this session are skipped.
2. **Spawn.**
   - Call `0x142380870(&sp, &xf, item+0x80, nullptr, zeroGuid)`, then `0x14237DF80(sp, nullptr)` to start loading.
   - The mod checks that `sp`'s vtable is `0x143D07FA8`; it then owns one reference.
3. **Poll** each frame:
   - `u8 sp+0x168` set means loaded.
   - `u8 sp+0x169` set means the game cancelled it.
   - 15 s without either is a timeout: cancel and release.
4. **Realize** once loaded: `e = 0x142375BA0(sp)` attaches the entity to the world. In the same tick, before any game update, the entity is validated. It must:
   - have a vtable inside ds.exe;
   - not derive from `Humanoid`/`DSPlayerEntity`, and not use the player Wake override `0x142757500`;
   - have a model (`e+0xB8`);
   - have a `DSWelcomeHoloComponent`;
   - have a non-null component `+0x50` (its `AIIndividualComponent`), a non-null `[ai+0x3690]`, and a non-null `e+0x180`. The component's update (`0x142B8BDF0`) dereferences all three without checks.

   An entity that fails validation is removed at once and its item is refused.
5. **Show:** `0x142B9E450(e)` (`DSWelcomeHoloComponent_sExportedStartVisibleWelcomeHolo`). The log
   shows the component state before and after.
   - StartVisible reads `[[0x147BF6E88]+8]`, `[comp+0x30]+0x41` and calls `e->vt[0xA8]` without null checks,
     so before every call (first show, re-assert, debug Show) the mod checks that the game-state object is
     non-null, the component's resource is readable and the Wake slot is code (not the player's override).
     If any is missing the call is skipped and logged (`StartVisible not called yet (...)` /
     `re-assert skipped: ...`); the 1 s maintenance retries it.
6. **Every frame:** SetWorldTransform to the interpolated pose.
7. **Every second:**
   - Log any change in component state `+0x58` or in visibility (flags bit `0x2`).
   - If state is 0 and `+0x12B` is clear, call StartVisible again (logged #1–5, then every 30th).
   - The debug menu's **Hide** calls StopVisible `0x142B9E400` and stops the re-assert.
8. **Remove.** This mirrors `0x142CA1AA0` and runs exactly once per spawner:
   - If the mod realized the entity and it is still the same object (vtable plus GUID), call `0x14235B8F0(e)`.
   - If the spawner loaded but was never realized, call `0x142375BA0`, then `0x14235B8F0`, as the game does.
   - If it is still loading, call `0x142372CB0(sp)` to cancel.
   - Then release: `lock xadd [sp+0x18],-1`, and if the old value was 1, call `vt[0](sp, 1)`.
   - The release runs only while `sp` still has the spawner vtable and a refcount of at least 1. Otherwise the reference is leaked deliberately rather than double-freed.
   - `0x14235B8F0` calls `[0x147BC4E70]->vt[0x20]` without a null check, so the mod checks the world object
     and that slot first. If the world is unavailable it neither removes nor releases (the spawner may still
     reference the entity); the log line ends `(not removed; spawner reference leaked)`.

Teardown triggers, which apply to every body:
- the local player entity changes;
- Sam has been asleep for 1 s (load, cutscene, knot space);
- the game's own echo-teardown condition is set;
- the echo or trace-point config is unloaded (for those bodies);
- the collectors DB pointer changes or is freed (holo);
- the partner has been silent for 10 s;
- a new link session starts, or the link has been down for 2 s (host/guest; there is no respawn until it is back);
- the debug menu's **Remove**, and F7.

A fault inside any hologram call switches the hologram off for the run (`[holo] ... faulted;
hologram disabled`). The body then falls back to marker.

## Addresses (all verified against ds.exe bytes, 2026-11-16)

| What | Address | Runtime check / evidence |
|---|---|---|
| Collectors DB global | `0x147C0E488` | decoded from getter `0x14345DE20` (`mov rax,[rip+X]; ret`); created by `0x14345C1F0`, freed by `0x14345CD10` |
| DB lookup (reference only) | `0x14344B9D0` | binary search on `item+0x70`; the mod scans linearly |
| `DSCollectorsItemResource` | RTTI `0x144DF3B80`, size `0xA0` | `+0x20` DisplayNo, `+0x70` HashId, `+0x74` NameHash, `+0x80` DisplayEntityResource, `+0x90` Type (u8), `+0x93` IsHiddenItem, `+0x9C` UISortIndex |
| StreamingRef | field → `d` → `{obj, flags}` | empty if `!obj` or `obj+0x10`/`+0x18` are both 0 (`0x141DE1350`); loaded if `(int8)(flags>>52) < 0`, resource `obj+0x20` (`0x141DDE990`); copied by `0x141DDB460` |
| CreateSpawner | `0x142380870` | sig `48 89 5C 24 08 ... 41 56 48 83 EC 30 33 C0 48 8B D9 48 89 01 49 8B F1 B9 F0 01 00 00`; allocates `0x1F0` bytes, ctor `0x14236CFC0` (copies the 0x3C-byte transform and the ref), then `lock xadd [sp+0x18],1` |
| Spawner vtable | `0x143D07FA8` | decoded from the ctor's `lea rax,[rip+X]` at `0x14236CFCE` |
| StartSpawner | `0x14237DF80` | sig `4C 8B DC 53 41 56 48 83 EC 58 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 38 80 B9 6A 01 00 00 00`; no-op if `+0x16A` is set or `+0x150` is non-zero; several game callers pass `parent = 0` |
| Realize | `0x142375BA0` | sig `40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B 8B 70 01 00 00 ...`; returns `[sp+0x170]-0x20`; the game calls it repeatedly |
| Cancel | `0x142372CB0` | sig `40 57 48 83 EC 30 80 B9 69 01 00 00 00 48 8B F9 0F 85` |
| Remove entity | `0x14235B8F0` | `world=[0x147BC4E70]; jmp world->vt[0x20]`; the world global is decoded from the bytes |
| StartVisibleWelcomeHolo | `0x142B9E450` → `0x142B9EC10` | the jump target is decoded. Exits early when there is no component, `G & 0xA6240`, `G` bit 56 without ForRaceObject, or `+0x12D` is set. If flags bit `0x2` is clear: sets `+0x5C=1`, calls `vt[0xA8]` (Wake), then `0x14235F640(e,0)`. Always sets `+0x12B=1` |
| StopVisibleWelcomeHolo | `0x142B9E400` | sets `+0x5C=0` unless `+0x12A`, clears `+0x12B` |
| Game state `G` | `[[0x147BF6E88]+8]` | decoded from `0x142B9EC41`; required (StartVisible reads it unchecked, so the mod checks it is non-null first) and logged |
| Construction-point manager | `[0x147BF4960]` | decoded from `0x142765D64`. Offsets `+0x56A7F0` (spawner), `+0x56A7FC` (HashId) and `+0x56A808` (item) are checked at `0x142C870A4` and `0x142C873EB`. If they do not match, only the "current structure hologram" choice is lost |
| `DSWelcomeHoloComponent` | RTTI `0x144C1B150`, size `0x140` | `+0x30` resource, `+0x48` owner, `+0x50` AIIndividualComponent (init `0x142B88340`), `+0x58` state, `+0x5C` requested, `+0x12A/+0x12B/+0x12D` flags |
| `DSWelcomeHoloComponentResource` | `0x144C1AF10` | `+0x20` WelcomeRange, `+0x24` ActionRange, `+0x28` Angle, `+0x34` Scale, `+0x38` FIFOSpeedScale, `+0x40` ForConstructionObject, `+0x41` ForRaceObject, `+0x42` IsNotHuman, `+0x43` IsNotEchoTarget, `+0x44` AutoSpeack, `+0x45` IdleOnly, `+0x46` DontMoveWorldPosition |
| Trace-point manager | `[0x147BE8680]` → config `[mgr]` | decoded from `0x14273785F`; config RTTI `0x144BC8EE0`; `EntityResources` count `i32 +0x58`, data `+0x60` |

How the game itself does it:
- Spawn (`0x142C86C80`, inside the construction-point update `0x142CBA8E0`):
  - copies `item+0x80` into a temporary 16-byte ref;
  - calls CreateSpawner with `guid = 0x144F6BE68` (zeros) and stores the result at `cpm+0x56A7F0`;
  - sets `sp+0x1E8` (default `0x1FFFFF`) from the construction point. The mod leaves it at the default;
  - calls `StartSpawner(sp, cp entity)`. The mod passes no parent.
- Despawn (`0x142CA1AA0`):
  - if loaded: realize, then `0x14235B8F0` (immediate), or a fade-out `MsgDsConstructionEvent`;
  - otherwise cancel;
  - then the inline release.

## Not known yet (the first in-game run answers these)

- What the chosen item looks like. Item display is data, so check the DB table in the log, and `IsNotHuman` in the realized component's resource line.
- Whether `comp+0x50` / `[ai+0x3690]` / `e+0x180` are already set right after realize. If the log keeps saying `refused: ... would fault`, those links are set later by the game. Do not relax the check without new evidence.
- Whether the hologram's AI or ground-movement components fight the per-frame SetWorldTransform (snapping or drifting). The `[sync] partner +1s` line compares the actual position with the target.
- Whether `StartVisible` is refused by game-state bits (`G & 0xA6240`, which the log names) or wears off.
  - The component fades the hologram out while Sam is within 2 m unless `ForConstructionObject` is set.
  - In the echo test, use `echo_offset_m=3` (or walk) so the hologram is not standing on Sam.
- Whether the spawner loads a far-away partner as quickly as one nearby. It is a resource load, which should not depend on distance. A timeout is logged with the item.
- Whether parent `nullptr` (instead of a construction-point entity) changes anything.

## What to look for in the log

```
[config] partner_body=holo partner_holo=00000000 partner_marker=-1 partner_voice=0
[holo] hologram functions verified (...)                 <- or "unavailable: <which check>" (then marker)
[holo] collectors DB 000001..: N items, H with Type&4 (Reward_Holo), R with a display entity. Columns: ...
[holo]   HHHHHHHH NNNNNNNN t4 #12 s | ...                 <- the item table (once per DB)
[holo] spawning item HHHHHHHH (..., <how chosen>; ref streaming) at (x, y, z): spawner=... refcount=1
[holo] item HHHHHHHH loaded after 0.8 s; realized entity ...
[partner] holo realized ...: <EntityType> flags=0x.. (visible=.. hidden=.. asleep=..) model=... <ModelType>; N components: ...
[holo] on realize: DSWelcomeHoloComponent ... state=.. requested=.. shown(+12B)=.. G=0x.. (G&0xA6240=clear, bit56=0)
[holo]   resource ...: WelcomeRange=.. IsNotHuman=0 ... DontMoveWorldPosition=..
[holo] after StartVisible: ... requested=1 ... shown(+12B)=1
[holo] item HHHHHHHH is the partner body now: entity ... flags=0x.. (visible=..)
[sync] partner body: hologram HHHHHHHH
[sync] partner +1s (holo): flags=.. at (..) target (..)   <- does it follow the target?
[holo] state 0 -> 1, visible 0 -> 1 (...)                  <- the component fading in
```

Failure lines, which name the reason and the next body:
- `[holo] item HHHHHHHH refused: <reason>`
- `[holo] item HHHHHHHH did not load within 15 s`
- `[sync] partner body 'holo' unavailable (...): falling back to 'marker'`
- `[partner] trace-point EntityResources (...)`: the marker candidates
- `[holo] teardown (<trigger>): ... <step>` and `[holo] released spawner ... (refcount was 1, destroyed)`: one per spawn, never two.

To try another hologram, take a HashId from the table and set `partner_holo=<HashId>` in
`session.ini`.
