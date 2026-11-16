# Reverse engineering notes â€” ds.exe (DEATH STRANDING DIRECTOR'S CUT)

Target: `ds.exe`, 86,479,872 bytes, SHA-256 `9d5306d7eb2f5d3135c63accc3e74b8f9b029f9d76112a6aae4a38fdbb4edd2b`,
PE timestamp 1706443442, Steam build 13300582, image base `0x140000000`. Not packed
(`.text` entropy 6.5, ordinary section table). All addresses below are VAs at that base;
the mod uses RVAs plus byte signatures (`native/mod/game_api.cpp`).

Tools: `tools/re/dsre.py` (fast Python index: `.pdata` functions, RIP-relative xrefs,
E8 calls, data pointers), `tools/re/exported.py` (dumps Decima's exported script symbol
table), `tools/re/mksigs.py` (unique signatures), `tools/re/gh.py` (Ghidra HTTP plugin client).

Everything here is **static** evidence. Nothing was observed at runtime yet; the first
in-game test is what confirms or refutes the behaviour notes marked *(untested)*.

## Decima exported script symbols

Decima registers script-callable natives with a name, a `Class_ExportedName` string and a
function pointer (`lea rax,[fn]; mov [rsp+20],rax; lea r8,[sig]; lea rdx,[name]; call register`).
`tools/re/exported.py` recovers 1,405 of them into `tools/re/.cache/exported.json`. The ones
the mod uses:

| Export | VA | What it does (from disassembly) |
|---|---|---|
| `DSPlayerEntity_sExportedGetLocalDSPlayerEntity` | `0x142757A30` | `PlayerManager* = [0x147BC7568]`; `p = FindPlayer(mgr, filter@0x144B1ACB8)` (`0x1423C5060`; fast path `mgr+0x48+idx*8`); returns `Player+0x70` (the Entity). No args, no locks. |
| `Entity_ExportedGetWorldTransform` | `0x142362C30` | `(WorldTransform* out, Entity*)` â€” calls `0x14234C210`, copies 0x3C bytes. |
| `Entity_ExportedSetWorldTransform` | `0x142361EA0` | `(Entity*, const WorldTransform*)` â†’ `0x1422DFF80` (writes pos/rot under the entity lock, sets dirty bit) then `0x14235BF60(e, 0)` (propagates to children). |
| `EntityResource_ExportedCreateEntity` | `0x1423806A0` | Thin wrapper over `0x142374400` (below). |
| `Entity_ExportedRemove` | `0x142361C20` | `jmp 0x14234CAA0` â€” `Remove(Entity*, bool immediate)`; immediate â†’ `world->vtbl[0x20]`, else a queued job. |
| `Player_ExportedGetEntity` | `0x142361B80` | `Player+0x70`. |
| `Entity_ExportedGetPosition` | `0x142362AF0` | reads `Entity+0xC8..0xDF` under `TryEnterCriticalSection(Entity+0x288)`. |

## Entity layout (partial)

| Offset | Meaning | Evidence |
|---|---|---|
| `+0x50` | u32 name hash (CRC32-C of the name string) | `0x142374400` stores `0x14190D040(name)` there |
| `+0x58` | streaming ref to the `EntityResource` | `Entity_ExportedSharesEntityResource` compares the GUIDs (`+8`, `+0x10`) of `0x141DDE990(e+0x58)` for two entities |
| `+0x78` / child `+0x80` | first child / next sibling | `0x14235BF60` walks them |
| `+0x88` | atomic flags (bit0 transform dirty, bit4/5 propagate, bit8 `0x100` asleep) | `0x1422DFF80`, `0x14235BF60` |
| `+0xC8` | `WorldPosition` (3 Ã— double) | getter/setter |
| `+0xE0` | `RotMatrix` (3 Ã— 3 float rows) | setter copies 0x24 bytes |
| `+0x288` | `CRITICAL_SECTION` guarding the transform | `TryEnter/Enter/LeaveCriticalSection` imports |

`RTTIRefObject` header (resources): vtable, GUID at `+0x08`, refcount at `+0x18`.

## Spawning: `EntityResource::CreateEntity` internals (`0x142374400`)

```
Entity* CreateEntity(EntityResource* res, const WorldPosition* pos, const float rot[3][4],
                     const char* name, bool place_on_ground, void* p6, void* p7, Post16* post)
  wt = {*pos, rows of rot}            ; rows repacked from float4 to float3
  normalize(wt)                        ; 0x142378CF0
  e = res->vtbl[0x80](res, &wt)        ; the resource builds its entity
  e->name_hash = crc(name)
  if p6: 0x14235DF30(e, p6)            ; optional
  if p7: 0x14235D5F0(e, p7)            ; optional
  0x14234F3E0(e, 0)
  if place_on_ground: 0x14235A010(e, &wt, 1, 0)
  world = [0x147BC4E70]; world->vtbl[0x18](world, e, 0, 0, 0)   ; add to world
  0x1423C8130(post, e)                 ; post->mode: 0 = nothing, 1..3 = extra handling
  return e
```

Arg 8 is a 16-byte struct passed by pointer (MSVC x64). The mod passes `mode = 0`.
The resource comes from the local player (`0x141DDE990(player+0x58)`: borrowed pointer, net
refcount 0), a `DSPlayerEntityResource` (vtable `0x143D7D010`). Slot `0x80` (`0x142373940`)
builds the type from `res->vt[0x68]` = **`DSPlayerEntity`** (RTTI `0x144BC6E00`, size `0x55A0`,
the only `Humanoid` subclass), copies the transform, calls `Init` (`vt+0x28`, creates every
component listed in the resource) and gives it a fresh GUID at `+0x08` (CoCreateGuid).
No Player object is created: `GetPlayer` (`+0x560`) is null on the clone, so
`GetLocalDSPlayerEntity` keeps returning the real Sam.

### Sleep / wake, and the player-system trap

Entity `+0x88` bit `0x100` means **asleep**. Confirmed in game (first echo run,
2026-11-16): the awake, walking Sam reads with it clear. Static evidence:

* Entity vtable `+0xA0` = **Sleep(Entity*, bool)**. Base `0x14235F8E0` does `lock or [e+0x88], 0x100`,
  disables components and recurses into the children's `+0xA0`.
* Entity vtable `+0xA8` = **Wake(Entity*)**. Base `0x1423614F0` does `lock and [e+0x88], ~0x100`,
  re-enables, propagates the transform and recurses into `+0xA8`. Slot `+0xB0` (`0x142361620`)
  is a wake variant that also sets bit `0x8_0000_0000`.
* `Entity_ExportedSetSleeping` (`0x142361E20`): `SetSleeping(true)` calls `+0xA0` only if `0x100`
  is clear, and `SetSleeping(false)` calls `+0xA8` only if it is set.

`DSPlayerEntity` overrides both slots (`+0xA0` = `0x142743810`, `+0xA8` = `0x142757500`):

```
mov rcx,[DSPlayerSystem]          ; singleton 0x147BE7980
test byte [rcx+0x206D1], 4        ; "gate"
jz  -> DSPlayerSystem::SetReason(0x1C26F835, add/remove)   ; 0x1426B9BB0
jmp Humanoid::Sleep (0x1437406F0 -> 0x1434FC040 -> base Sleep)
    / Humanoid::Wake (0x1437439C0 -> 0x1434FD3C0, then 0x1437320F0)
```

`SetReason` updates a sleep-reason list and then runs `0x1426E0410`. That function looks up
the local player, **sets the gate**, sleeps Sam via `+0xA0` while any reason is held (or wakes it
via `+0xA8` when none remain), then clears the gate. So an ungated Sleep/Wake of *any*
`DSPlayerEntity` (including our clone) adds or removes a sleep reason for the **real Sam**.

The mod copies the player system and holds the gate around everything that can sleep or wake
the clone (`native/mod/game_api.cpp`):

* `TryCreatePartner`: `CreateEntity` (including its world-add) runs gated.
* `TryWakePartner`: if `0x100` is set, `+0xA8` runs gated (the game's own Humanoid path).
* `TryRemovePartner`: if the clone is awake, `+0xA0(e, true)` runs gated, then
  `Entity::Remove(e, immediate=true)` runs gated. Only bit 4 of the gate is restored afterwards.
* The mod spawns only while Sam is awake (`0x100` clear). It logs Sam's flags before and after
  spawn/removal and marks `<-- Sam was put to sleep!` if bit `0x100` appears.

The first build misread `0x100` as "active" and called `Humanoid::Sleep` to "activate" the clone.
The in-game log showed Sam always "inactive", so the partner never spawned.

*(untested)* Once awake, the clone runs the player's components (DSPlayerMover keeps its own
transform at `mover+0xA8` and pushes it into the entity via `0x1422DFF80`; pad operator,
triggers...). Many engine paths find "the player" via `GetDSPlayerEntity` (`0x14283EC30`, 382
call sites) or `IsKindOf(DSPlayerEntity)` (74 sites), so side effects on the real Sam are
possible. The `[sync] partner +1s:` log line compares where the partner is with where we put
it. `partner_activate=0` in session.ini leaves the clone asleep for comparison.

### Result: a cloned DSPlayerEntity cannot be built (first in-game run, 2026-11-16)

`EntityResource::CreateEntity` on the player's resource faults during component init:

```
CreateEntity 0x1423806A0 -> 0x142374400 -> 0x14234F3E0(e,0) -> ... -> 0x14271FA60
 -> 0x14373A780 -> 0x14351BC10 -> component vfunc +0x2C8 = 0x1427E63C0
    [comp+0x418] = FindComponent(entity+0x90, DSPlayerController /*RTTI 0x144BB2770*/)  -> null on the clone
    0x1426B8B00(null, comp+0x820)  : mov [rcx+0x5370], rdx   <- access violation
```

`DSPlayerController` (Decima RTTI name at type+0x40, size 0x8020) is not in the
`DSPlayerEntityResource` component list; it comes with the possessed player. The game's
crash reporter (`error.txt` next to `ds.exe`, plus a popup) sees the access violation even when
the mod's `__except` handles it, so a guarded fault is not a graceful fallback in DS. The
clone path is switched off (`kClonePlayerResource` in `player_sync.cpp`); the partner needs a
different entity type.

Decima type names: a type descriptor (`RTTI*`, e.g. `0x144BC6E00`) holds a pointer to its
name at `+0x40`. ds.exe has no MSVC RTTI (vtable[-1] is not a complete-object locator).

## Partner entity: the echo hologram

DS shows other porters as holograms ("echoes"). It builds them from plain entity resources,
with no Player and no DSPlayerController, which is what the mod needs
(`native/mod/partner_source.cpp`). All verified statically, 2026-11-16:

| What | Where | Evidence |
|---|---|---|
| `DSPlayerEchoManager*` | global `0x147BE8668` | `0x1427585A0` allocates 0x4C0 and runs ctor `0x1426F3D40`, which calls `InitializeSRWLock(+0x468)` at `0x1426F3DFC` |
| config pointer | mgr `+0x418` (borrowed) | written under the lock by `0x14271A6F0` (load) and nulled by `0x14271CE60` (unload, which first removes the game's echoes via `0x142739D40`) |
| lock | mgr `+0x468` SRWLOCK | the game takes it around echo spawn (`0x14273A9CD`..`0x14273B24F`) and update (`0x142745CB0`) |
| `DSPlayerEchoManagerConfig` | RTTI `0x144BC96B0` | attrs `+0x20 EntityResource`, `+0x28 KnotSpaceEntityResource`, `+0x30 WelcomeHoloCheckScriptResource` |
| game spawn | `0x14273AFB2` | `[cfg+0x20]->vt[0x88]` -> `0x14234F3E0(e,0)` -> `world->vt[0x18]` -> `SetVisible(e,0)` -> `0x1422DFF80` (transform) -> `FindComponent(DSEchoComponent 0x144C23EC0)` -> Play `0x142BBF3A0` |
| game removal | `0x142739D40` -> `0x14235B8F0` | `world->vt[0x20](e,0,0)`, the same as `Entity::Remove(e,true)` |
| refcount | `+0x18`, low 22 bits | AddRef `lock xadd [r+0x18],1` (`0x14271A789`); release `0x1419F5E50` destroys at 1 |
| visibility | `+0x88` bit `0x2` | `Entity_ExportedSetVisible` `0x142361E90` -> `0x14235F640`; bit `0x4` = hidden override |

`EntityResource_ExportedCreateEntity` does the same three steps as the game's spawn, plus transform
normalising, the name hash and a no-op post action (mode 0). The echo manager only tracks
entities in its own slot array (`mgr+0`, stride 0x30, count `+0x490`, max 8) and its knot-space
echo (`+0x4A0`), so it never touches the mod's entity.

How the mod uses it:

* It reads everything with `ReadProcessMemory`, which can't fault. It decodes type names from the one-instruction
  `GetRTTI` getters (`lea/mov rax,[rip+x]; ret`) instead of calling them. The Decima type name is at
  RTTI `+0x40`, the base count at `+0x06` and the bases at `+0x60` (16-byte entries).
* Before using a resource it refuses any whose entity type derives from `Humanoid`, and any with a `DSPlayer*`, `AI*` or `GameActor`
  component. The component lists are `{i32 count, i32 cap, T* data}` at resource `+0x90` and entity `+0x90`.
  The `vt+0x80 == 0x142366570` builder check does **not** exclude the player resource.
* It creates the partner under the manager lock, after re-checking `+0x418`, and holds its own AddRef on the resource.
* It removes the partner (`Entity::Remove(e,true)`) in these cases:
  * when the local player entity changes;
  * when Sam has been asleep for 1 s;
  * when the config is unloaded;
  * when the game's own echo-teardown condition is set (`[[0x147BE6318]+0x450]`, or `[[0x147BF83C0]+0x5E0]==2`).
* *(untested)*:
  * What the echo looks like (game data, not the exe).
  * Whether it renders without its `DSEchoComponent` being started through Play `0x142BBF3A0`, because the mod does not start it.

The World object (`[0x147BC4E70]`) lives for the whole process; entities survive reloads unless
removed.

## Per-frame hook

`ds.exe` delay-loads `steam_api64.dll`; the only caller of the `SteamAPI_RunCallbacks`
delay thunk is `0x141A1F690`, which is reached through a function-pointer table
(`0x147EB5DC0`). The mod hooks that wrapper (MinHook) and runs its frame work after the
original. The log line `[sync] tick rate: â€¦ Hz` reports the real call rate on first run.
*(untested: confirmed main-thread / per-frame at runtime.)*

## Steam

`steam_api64.dll` (FileVersion 05.25.65.21) is an old SDK: no `ISteamNetworkingMessages`
flat exports. `ds.exe` itself requests `SteamUser021`, `SteamFriends017`, `SteamUtils010`,
`SteamInput006`. The mod gets `SteamNetworkingMessages002` / `Sockets012` / `Utils004` from
steamclient via `SteamInternal_FindOrCreateUserInterface` and calls them through
Diamond Dogs Together's vtable ABI (`native/include/stcoop/steam_net_abi.hpp`).

## DLL loading

`ds.exe` statically imports `WINHTTP.dll` (15 functions); winhttp is not in KnownDLLs, so
`<game>\winhttp.dll` loads first. `setupapi.dll` *is* a KnownDLL (unusable). `d3d12`,
`dxgi`, `XINPUTUAP` and `steam_api64` are delay-loaded.
