# Real partner body: a solid character that plays the other player's real animation

**Status (2026-11-16): experimental; works in hidden echo runs and between two games over the link, all
16 bodies offered.** It is behind `session.ini partner_body=real`. The launcher writes `partner_body=real`
by default (Debug option, section 5) since the two-game and fallback runs passed; a session.ini without
the key still gets the hologram, and the mod falls back to the hologram on its own when the real body
cannot be shown. A solid cutscene-actor body loads by path and GUID in 0.01-3 s. A
post-hook on the game's SkinnedModel animate function then writes Sam's real skeleton pose onto it. In
echo mode the body replays Sam's walk and jog 2 s later, trailing him. This was checked with Sam (porter
suit) and with Fragile. In mirror mode Sam's live pose was shown on the porter, Fragile, Die-Hardman and
Cliff bodies too. Between two hidden games on one PC (UDP loopback, section 5) the guest saw the host
as a solid Sam body walking with real leg and arm motion, and the host saw the guest as a solid Fragile
(and, in another run, Cliff) body standing and running [V: runs 20261116-193151-two-games-real,
20261116-193649-two-games-real-cliff]. A peer that sends no joint frames is shown as the hologram
[V: run 20261116-193451-two-games-real-fallback].

Code:
- `native/mod/real_body.{hpp,cpp}`: spawn, hook, pose capture and apply, the pose ring, debug commands.
- `native/mod/body_table.{hpp,cpp}`: the character list (pure data; the launcher compiles it too).
- `native/mod/body_choice.{hpp,cpp}`: who is sent and who is shown (section 5; pure, unit-tested).
- `native/mod/pose_wire.{hpp,cpp}`: the STJ1 joint frame.
- `native/mod/player_sync.cpp`: the `real` body in the body chain, the echo and net joint stream.
- `native/mod/link.cpp`: STJ1 next to STP1.
- `native/mod/partner_holo.cpp`: `SpawnBeginField`.
- Name tags (section 10): `native/mod/nametags.cpp`, `native/mod/nametag_math.hpp`,
  `native/mod/overlay/overlay_tags.cpp`, the STN1 frame in `pose_wire`.
- Player collision (section 11): `native/mod/partner_collision.{hpp,cpp}`.
- Gear, the backpack and cargo on the bodies (section 12): `native/mod/partner_gear.{hpp,cpp}`, the
  placement in `real_body.cpp`, the STC1 frame in `pose_wire`.
- Gear looks and the backpack's pose (section 13): `native/mod/gear_look.{hpp,cpp}`, the STV1 / STK1
  frames in `pose_wire`, the pose targets in `real_body.cpp`.
- Sam's head on the partner Sam (section 14): `ReadHead` in `native/mod/partner_gear.cpp` (the STC1 parts
  block), `outfit::LocalWearsPorter` in `native/mod/partner_outfit.cpp`.
- The cargo menu's figure under the self view (section 15): `native/mod/menu_figure.{hpp,cpp}`, `MenuTick` and the
  detour's figure branch in `real_body.cpp`.
- Weapons in hand (section 17): the weapon read, the watch and the weapon props in `native/mod/partner_gear.cpp`,
  the STC1 `weapon` kind (version 3) in `pose_wire`.
- Headgear on the character (section 20): `kHeadgearRows` / `SelfHeadItemOffset` in `real_body.cpp`, the accessory
  movers (`HeadgearTick`) and the meters (`HeadMeter`) in `native/mod/partner_gear.cpp`.
- Boots (section 21): the feet scale in `ApplyBody`, the boots prop's pose target (`SetBootsProp`, `ApplyBootsPose`,
  `ComposePartner`) in `real_body.cpp`; the boots item, prop and parts (`ReadBootsShown`, `BootsTick`) in
  `partner_gear.cpp`; the STC1 `boots` kind and block (version 4) in `pose_wire`.
- Vehicles (section 22): `native/mod/vehicle_sync.{hpp,cpp}`, the STM1 frame in `pose_wire`, `link::SendVehicle` /
  `PopVehicle`, the pose target `kVehiclePoseTarget` in `real_body`.

Tags:
- [V] verified: bytes, a decompile, or a run cited below.
- [I] inferred.

## 1. What it looks like (evidence)

Each run below was hidden, used `-SeedSaves complete-100`, and ended with exit 0, no `error.txt`, the
real saves unchanged and 0 cloud uploads. Run folders are under `build-testenv\runs\`.

| Run | What it shows |
|---|---|
| `20261116-183736-real-body-diag` | The first bodies that loaded. `21-sam-own-pose.png`: a solid Sam in the porter suit, 4 m ahead, in the bind (A) pose. `23-porter-mirror.png`: a solid Bridges porter (visor helmet, uniform), also in the A pose. The game had stopped updating their models (see 3.3). |
| `20261116-184029-real-body-tour` | The Sam body copies Sam's live pose (mirror). `12-mirror-walk-a..d.png`: while Sam walks towards the camera, the body 4 m behind him walks in place. Leg lift and arm swing change from frame to frame. `13-mirror-stopped.png`: it stands in Sam's idle. |
| `20261116-184259-real-body-tour` | Porter, Fragile, Die-Hardman and Cliff Unger, each spawned 5 m ahead and mirroring: `*-idle.png`, `*-walk-a.png`, `*-walk-b.png`. All are solid and correctly textured. Each stands in Sam's idle, then shows a walking or jogging stride, feet on the floor. Proportions are each character's own (Fragile is smaller). |
| `20261116-184459-echo-real-100` | **Echo with the Sam body.** `04-walked.png`: the body walks right behind Sam. `06-trailing-a/b.png`: it walks past a pillar. `06-trailing-f/g.png`: it jogs towards the camera 2 s behind Sam, one knee raised, arms swinging, dust at its feet. `09-stopped.png`: it stands where Sam stood. |
| `20261116-200116-real-body-tour-all` | **Every body in the table (ids 1-16)**, each 5 m ahead of Sam and mirroring: `NN-<key>-idle.png`, `-walk-a.png`, `-walk-b.png`. All 16 loaded in 0.01-0.10 s, are solid and textured (no hologram parts), have their own head and hair, and stand in Sam's idle with arms down and both feet on the floor (Heartman too, after the latch fix below). In walk-a each body leans into Sam's turn; in walk-b each stands upright in a pose different from its idle (not frozen, not the bind pose). A clear stride is visible for Sam and Fragile. In the later walk-b frames the player's own Sam fills the foreground (at a terminal, "Activate Terminal") and the body is small and hazy behind him by the vehicle (`09-mama-walk-b`: Mama upright by the vehicle, no raised knee discernible; a review of the run reported the same for ids 5-16), so the leg motion of ids 6-16 while walking is not verified. Run `20261116-194441` (before the fix) showed the same, except Heartman: his feet were sunk into the floor. |
| `20261116-184706-echo-real-fragile-100` | **Echo with Fragile.** `04-walked.png`: Fragile (her jacket's back logo is visible) walks behind Sam. `06-trailing-f/g.png`: she jogs towards the camera behind him. |
| `20261116-193151-two-games-real` | **Two games over the link** (section 5). The guest's view: `02-guest-sees-host`, `03-guest-sees-host-walk-a..d`, `04-guest-sees-host-stopped`: a solid Sam (porter suit, no hologram glow) standing, then walking away with arm swing and a bent, lifted leg (b-d), then standing still; never a bind pose. The host's view: `05-host-sees-guest-standing` (a solid Fragile, small in frame) and `06-host-sees-guest-walk-d` (Fragile mid-stride towards the camera). In `06-...-a..c` and `07` the host's own cargo fills the frame and hides her (a possible collision push, section 6 item 6). |
| `20261116-202426-two-games-real` | **Final build** (after the review fixes, with the Heartman latch and ids 6-16 offered). Guest view `03-guest-sees-host-walk-b`: a solid Sam (porter suit, no hologram glow) walking away from the camera, one foot lifted, arms swinging. Host view `05-host-sees-guest-standing`: a solid Fragile standing, arms down, feet on the floor; `06-host-sees-guest-walk-a..d`: she runs towards the camera, legs alternating, a knee raised high in `06-d`. The host's own Sam stayed out of frame this time. 300/300 joint frames per 10 s each way, 565 B per datagram, `[pose]` rej/torn/aba/wpm/faults 0, no dropped-frame or fallback lines (both got their first joint frame without waiting); exit 0, no error.txt, real tree unchanged, 0 uploads. |
| `20261116-193649-two-games-real-cliff` | The same with the guest as `my_character=cliff` (a scratch copy of `two-games-real`, not in the repo). Host view `05`, `06-a..d`: a solid Cliff standing, then walking and running towards the camera, legs alternating, feet on the floor. |
| `20261116-193451-two-games-real-fallback` | Host `partner_body=real`, guest `holo`. `03-host-sees-guest-hologram` and `04-guest-sees-host-hologram`: the cyan, translucent hologram on both screens. Both players ended at the same spot (same scripted input), so the hologram overlaps the viewer's own Sam. |
| `20261116-192744-two-games-real` | **Failed** (fixed since): the guest sat behind the offline-mode notice and sent no joint frames, and the host's fallback to the hologram was permanent (section 5, "Fallback"). |

A composite of these frames is in the session scratchpad (`real_body_evidence.jpg`).

What is not right yet:
- Gear (section 12): the bodies carry the player's gear entities (everything on the backpack, shields,
  hanging boots, the pod as a prop) and, on a Sam body, Sam's gear parts; not the weapon in hand, the
  Odradek or the cuff device, and on another character not the exoskeleton.
- The faces do not move: no facial rig is driven.
- A female body plays Sam's male motion.
- Collision: the partner's real body now blocks the local player (section 11).

Hook counters in every run were `rej=0 torn=0 aba=0 wpm=0 faults=0` ([pose] lines). The check of the
partner's committed pose against the published pose is 0.00° while standing still. While moving it is up
to 4-10° max: the publisher runs at the tick rate (about 125 Hz) and the game updated the body at 60 Hz
(v0.7), so the comparison is against a slightly newer frame. Since v0.8 the bodies are updated every
frame up to 240 fps and the partner's pose is interpolated in its own update (section 3.5).

## 2. The body: cutscene actors, loaded by path and GUID

`body_table.cpp` lists `ds/entities/cinematics/demo_*` entities. Their paths and GUIDs come from
`docs/game-data-index.md`. Ids are wire values and are append-only:

| id | key | path |
|---|---|---|
| 1 | sam | `demo_sam_clean` |
| 2 | porter | `demo_bdv_body_suits` |
| 3 | fragile | `demo_fgl_new` |
| 4 | diehardman | `demo_dhm_body_new` |
| 5 | cliff | `demo_fth_father_new02` |
| 6-16 | others | porter_plain, deadman, heartman, mama, lockne, amelie, higgs, guard, medic, sam_mule, fragile_suit |

Each row also has an `offered` flag: the body was proven in game (spawned, validated and posed in a
hidden run). All 16 ids are offered: 1-5 since runs `184029`, `184259`, `184459`, `184706`, and 6-16 since
`real-body-tour-all` (runs `20261116-194441`, `-195431`, `-200116`).
Per body in those runs: skeleton 77 joints, layout `BE39C83F`, all names matched in Sam's order, `[pose]`
rej/torn/aba/wpm/faults 0. Sam, porter, porter_plain, guard, medic and sam_mule use Sam's own Skeleton
object (not scaled). Latch, rest pelvis / leg (thigh + shin) ratio against Sam (leg 0.829 m): Fragile
0.957 / 0.990, Fragile (suit) 0.975 / 0.990, Die-Hardman 1.060 / 1.073, Cliff 1.060 / 1.049, Higgs 1.060 /
1.049, Deadman 0.986 / 0.964, Mama and Lockne 0.995 / 1.022, Amelie 0.975 / 0.991, Heartman 0.619 / 1.049
(see 3.2 step 3). Players can pick only
offered bodies, and a receiver shows only offered bodies for the peer (section 5). To offer another body,
append it to `real-body-tour-all` (or use `real_spawn` with mirror 1), look at the frames, then set the flag.

Each body is a `DSControlledEntity` [V, runs above]:
- Sam: 8 components (DSFacialRig, DSCoverAndAnimModel, DSCoverModel, ArtPartsVariation, ArtPartsData,
  ExternalAnimation, LodDistanceOverride, SkinnedModel).
- The others: 5 components.
- None has AI, a controller or a player component. The mod refuses a body that has any of them.

### 2.1 Register by path and GUID (no game data needed beyond the path and GUID)

The source reference is fake: a temporary proxy and box in our memory. The spawner's constructor copies
it into its own reference, and that copy registers the path and GUID with the streaming manager.

```
proxy (0x30 bytes, VirtualAlloc): +0 Decima String path (String ctor 0x14190AA00, no ".core"),
      +8 i32 hash = -1 (the manager computes and stores it), +0x10 GUID (raw bytes = uuid.bytes_le), +0x20 0
box   {proxy, mgr | 0 << 52}            // priority 0, "not loaded"
field = &box -> CreateSpawner 0x142380870(&sp, &xf, &field, null, zero GUID)
String dtor 0x14190AD20 on the proxy's path, free the proxy (Register copied the String with a refcount)
StartSpawner 0x14237DF80 -> poll +0x168 / +0x169 -> Realize 0x142375BA0   (partner_holo SpawnBeginField / SpawnPoll)
```

Steps:
- **Manager.** The manager is `box[1] & 0xFFFFFFFFFFFFF` of the local player's own resource reference
  (`Sam + 0x58`). Its vtable is checked to be `0x143C1DEC8`, with `+0x10 == Register 0x141DE3F30` [V].
- **Spawner constructor.** It calls the reference copy `0x141DDB460` at `0x14236D163` (byte-checked:
  `lea rcx,[rbx+0x188]; call`) [V]. The copy calls `mgr->vt[0x10](mgr, dst, (String*)proxy, proxy+0x10)`.
  Register (decompile `0x141DE3F30`) finds or creates the path entry and the proxy with that GUID, and
  refcount-copies the String [V].
- **Priority must be 0** [V, two runs]:
  - With priority 7, the copy queued the load itself through `vt[0x38]`. Every spawn then came back "loaded
    and cancelled" one frame later, with no object. This is run `20261116-183514-real-body-diag`: two
    collectors-hologram controls and the porter.
  - The reason is the spawner's own load step `0x142379340`. It registers its completion listener with
    `[0x147AE90E0]->vt[0x28]` and requests the load at priority 7 only when the reference reads "not
    loaded, priority 0" [V decompile].
  - With priority 0, the control hologram loaded in 0.06 s and the bodies in 0.02-3 s
    (`20261116-183736`, `184029`).
  - Earlier notes said "priority must be non-zero" (critique B1). That was wrong for this route.
- **GUID byte order.** `real_probe` read the player reference: path `ds/entities/player/ds_player_character`
  (no `.core`), raw GUID `a6 ad 58 9b ...`. This equals `bytes_le` of the decoded
  `9b58ada6-67ab-5734-b14a-43b6a94bd838` [V, every run]. `bodies::ParseGuid` makes the same bytes and is
  unit-tested.
- **Checks after realize.**
  - The entity's resource reference GUID equals the requested GUID.
  - The resource is `DSControlledEntityResource`.
  - The entity is not a Humanoid or DSPlayer.
  - `[e+0xB8]` is a SkinnedModel: vtable `0x143D0ADB8` and back pointer `+0x48 == e`.
  - The pose skeleton has 77 joints and layout `BE39C83F`.
  - All 77 joint names match Sam's.
  - Every body tried had the same names in the same order: `JNT_C_B_000_Root`, `JNT_C_B_001_Hips`, ...,
    `MTP_Global_c` [V].
- **Removal.** The existing hologram teardown (`RemoveEntity` and spawner release), after the detour is
  drained (3.4).

## 3. The animation: pose sync through the SkinnedModel animate hook

### 3.1 Where the pose lives [V: decompiles `0x1423AA540`, `0x1423A6670`, `0x14239D890`, census dumps]

| Object | Offset | Meaning |
|---|---|---|
| SkinnedModel | `+0x48` | entity |
| | `+0x170` | `Pose[2]`, 0x70 bytes each |
| | `+0x250` | current index (flipped under the SRWLOCK at `+0x3A8` before every update) |
| | `+0x2E0` | AABB |
| | `+0x308` | bone boxes |
| | `+0x338` | bounds on |
| Pose | `+0x20` | Skeleton |
| | `+0x28` | state (4 = model matrices done) |
| | `+0x30/+0x38` | local joints, 0x30 bytes each: q xyzw, t xyz 1, s xyz 1 |
| | `+0x40/+0x48` | model matrices |
| Skeleton | `+0x20` | count |
| | `+0x28` | joints `{String name, i16 parent}`, stride 0x10 |
| | `+0x70` | layout hash |

The update is: flip → reset → **animate `0x1423AA540(sm, dt)`** → cloth `0x1423AA0F0` →
MsgPostSkinnedModelUpdate. Animate ends with a commit, model matrices `0x141D8AA20(pose)`, then
`0x141D8A660(pose, sm+0x308, sm+0x2E0)` when `[sm+0x338]`.

### 3.2 The hook

The hook is a MinHook on `0x1423AA540`. It is installed at start-up from Bootstrap, only when
`partner_body=real` or `pose_hook=1`, and never mid-game. The detour calls the original first:

- **For every other model**, it only compares two atomics.
- **Sam's model** (published by the tick: player `+0xB8`, vtable and back-pointer checked): the detour reads
  the finished local joints of `pose[idx]` with ReadProcessMemory into a seqlock. That is about 125
  captures/s.
- **The partner's model**:
  1. ABA check: vtable `0x143D0ADB8` and `+0x48 ==` our entity.
  2. Header check: skeleton pointer, state 4, 77 × 0x30 joints, 77 × 0x40 matrices.
  3. **Latch**: the first 10 plausible frames of the body's own pose are kept. Those are its bind pose,
     so they hold its bone lengths. The pelvis ratio is its rest pelvis length divided by Sam's rest
     pelvis length, a constant 1.000 m (what `demo_sam_clean`, which uses Sam's own Skeleton object,
     latched in every run). Bodies on Sam's own Skeleton object (Sam, porter) are never scaled. Rest
     pelvis lengths: Sam 1.000, Fragile 0.957, Die-Hardman 1.060, Cliff 1.060 [V latch lines].
     **Leg check.** The latch assumes the body's first frames are a standing rest pose. Heartman's are not:
     his latched pelvis is 0.619 m (a seated pose [I]), the ratio clamped to 0.75 and his feet sank into
     the floor (run `20261116-194441`, `08-heartman-idle`). So the latch also compares the leg bones
     (knee and ankle local translations, which do not change with the pose) with Sam's own (0.829 m per
     leg). When the pelvis ratio and the leg ratio differ by more than 8 %, the leg ratio is used
     (Heartman: 1.049, feet on the floor in run `20261116-200116`). For every other body the two agree
     within 3.5 % and the pelvis ratio stays. Logged as `[real]   legs (thigh + shin) ... ratio ...`.
     (The prototype divided by Sam's *animated* pelvis length at latch time, which moved between 0.888
     and 0.998 m: the Sam body itself got ratios 1.002-1.126 and Die-Hardman 1.069-1.175, lifting the
     hips. Fixed in review.)
  4. **Apply**: the joint `q` comes from the published frame. `t` and `s` come from the latched table.
     The root `t` is Sam's; the pelvis `t` is Sam's times the ratio. The joints are written with
     **WriteProcessMemory**, so a bad page fails instead of faulting.
  5. The game's own model-matrix and bounds functions are called for this pose, exactly as the original
     tail at `0x1423AA83A` does.

Why game calls from a job thread are acceptable here [I, critique A3 V]:
- Both functions are pure math on this pose.
- The original function has just called them on the same thread with the same pose.
- The pose must be rewritten before cloth and rendering read it.

The detour does not log, allocate or lock.

### 3.3 Driving the model (cutscene bodies are not updated by themselves)

Without help, the game updated the spawned body's model only 10-13 times after the spawn. It then left it
in the bind pose (run `183736`: obs=10, then no calls). So the tick calls the game's own forced model update
`0x14239D890(sm)`. This is what the game itself does at `0x142495833`, `0x142F115FC`, `0x142F13938` and
`0x1436334D2`: it takes entity `+0xB8`, checks IsKindOf SkinnedModel, and calls the function.

The tick makes this call only when the game has not updated the model itself in the last 100 ms, and
only while Sam is up (not during loads, cutscenes or knot space). This is a heuristic, not a lock: it
avoids running next to the game's own update of this model only as long as the game does not resume
updating it in the same frame [I]. Measured:
- Debug bodies standing still (`real_spawn`, tours `184029` and `184259`): the game does not update them
  for long stretches, and the tick drove them at about 60 Hz (`forced` 2132 and 1917 in total).
- The echo partner, moved every tick (`184459`, `184706`, `185651`): `forced=0` throughout; the game
  updated the body itself at 60 Hz. [V counts; why a moved body keeps being updated is not known.]
Which thread the game's own callers of `0x14239D890` run on was not checked (Ghidra was unavailable
during the review); the remaining risk is the forced call overlapping an entity-update job.

### 3.4 Lifetime and safety

- **Removal.** The mod stores 0 in the partner atomics and spins until no detour is inside (up to 50 ms).
  Only then does it remove the entity and release the spawner.
- **Body chain.** It is `real → holo → marker → echo`. A body that fails to load or validate is refused
  for the session. Sam is tried before falling back to the hologram.
- **Existing triggers still apply.** The body is removed when:
  - Sam goes to sleep (load, cutscene, knot space);
  - the game removes its echoes;
  - the local player changes;
  - the partner has been silent for 10 s.
- **Byte checks.** Hook sites: animate prologue, tail and call targets, model matrices, bounds, pose
  getter, SkinnedModel vtable slots 0 and +0x70, forced update. Spawn: Register, the reference copy and
  its call site in the spawner constructor, the String constructor and destructor, the manager vtable
  `+0x10`. A mismatch disables the real body; the hologram stays.

### 3.5 Smooth above 60 fps (`session.ini smooth_pose`, default on)

**The problem** [V: decompiles, game data, run `20261117-144357-smooth-echo`]:
- The game updates every entity at the rate its resource asks for (`EUpdateFrequency`, entity `+0x204`:
  0 = 9.99 Hz ... 3 = 59.94 Hz, 4 = 120, 5 = 144, 6 = 240 Hz). Every cutscene body in `body_table.cpp` and
  every gear prop asks for 59.94 Hz (some props 14.99 Hz: 2 of 15 in that run); the player asks for 240 Hz.
- The detour poses a body only when the game updates it. With v0.7 the partner and self bodies got
  60 poses a second whatever the frame rate: at 110 fps 45 % of the frames while running repeated the
  last pose (a 1-1-0-1-0 cadence), and the limbs' angular speed jumped between 0 and twice the real
  value (jerk 1.8 against Sam's own 0.24, see the meter below).
- Why 59.94 Hz is a ceiling: the scheduler (World `[0x147BC4E70]+0x130`, 0x28-byte groups) counts each
  group down by the frame time (`0x1423FC300`) and sends `MsgEntityUpdate` to the due ones; SkinnedModel's
  handler (vtable `+0x58` = `0x14239D9B0` -> `0x1423A6670`) then animates. The interval is the slower of
  the global cap `[0x144B0CD41]` (6 on PC, set in `0x14262D330`) and the entity's own value.

**The fix: three parts, bits of the smooth mask** (`smooth_pose=1` = mask 7, `0` = mask 0 = the v0.7
behaviour; the `smooth 1 <mask>` test action switches them at run time):
1. **240 Hz updates (bit 1, `kSmoothBoost`).** The game's own `Entity::SetUpdateFrequency 0x14235F5A0
   (Entity*, u8)`, the setter it uses to boost carried cargo to 240 Hz (`0x142CBF860`) and on the player
   (`0x143740100`). It takes the global SRW lock `0x147BC4828`, writes `+0x204` and moves the entity to the
   group of its new interval (`0x1423FC0E0` on World `+0x130`; children follow).
   - Byte-checked before the first call (the whole prologue up to the regroup call, with the lock, World
     and regroup targets resolved and compared), logged once:
     `[smooth] update-frequency setter verified (ds.exe+0x235F5A0: lock, World, regroup call)`.
   - Called from the tick (game thread), only on entities we spawned, only while Sam is up: a live body
     (`DriveFreq`, at most 8 tries per body) and, every 250 ms, each published gear prop (vtable + GUID
     checked first; its own value is remembered, so clearing the bit puts it back). Read back and logged:
     `[real] body update frequency 3 (59.94 Hz) -> 6 (240 Hz), scheduler group 13 -> 55`.
   - A fault turns it off for the session (`[smooth] SetUpdateFrequency FAULTED ...`). 69 calls in the
     four runs below, 0 faults, no error.txt.
   - Effect: the bodies and props are updated every frame up to 240 fps (above 240 fps Sam repeats too).
     The tick's forced update (section 3.3) now fires only for a body the game stops updating.
   - Accepted risk [I]: whether the tick can overlap the scheduler's own frame function is not known; the
     setter takes the game's lock, the call is made once per entity, and the game itself calls it from
     entity jobs.
2. **Interpolation in the body's own update (bit 2, `kSmoothDetourInterp`).** Each tick
   `SamplePublish` also publishes, in a seqlock, the ring frames around the play time (up to 3, keyed on
   the sender's timeline), the play time and the QPC time it was taken at. The partner body's detour
   interpolates at `play = play_ref + (QPC now - QPC ref)`: the pose belongs to the moment the game
   updates the body, not to the tick that may have run before or after it. Pure math (nlerp with the
   hemisphere fix, root and pelvis lerped), no lock, log or allocation. It never extrapolates: past the
   newest frame it holds it (counted as `hold`). The transform stays in the tick. Debug mirror bodies keep
   the old path.
3. **Capture-time stamps (bit 4, `kSmoothCaptureStamp`).** STJ1's `sender_ms` is now the QPC time at
   which the game computed the sent pose (`realbody::CaptureFrame`), up to a frame before the send tick.
   The receiver spaces the frames as they really were. Same field, same encoding (no format change; mixed
   builds are refused at the handshake anyway, section 4).

**Not changed, measured instead:**
- **The self view's lag.** The self body shows Sam's pose of the previous frame (section 8.2). At 60 fps
  and above the age was one frame in 99-100 % of the writes (`capture age ... ~1 frame`), so the lag is
  constant, not jitter. At 37-40 fps 15-25 % of the writes were same-frame, with no repeated frame and a
  jerk below Sam's own; not worth the history buffer the research proposed.
- **The send rate stays 30 Hz.** At 60 Hz (run `20261117-145602-smooth-echo-60hz`, `ini send_hz=60`) the
  echo partner's error against Sam fell from 0.21-0.37° to 0.14-0.16° (average of hips, both thighs and
  the right forearm while running; max 1.2-1.5° instead of 1.7-2.2°), while its jerk stayed at Sam's
  level either way; it doubles the bandwidth (594 B per pose: 17.8 KB/s each way at 30 Hz, 35.6 KB/s at
  60 Hz). `send_hz=60` remains a setting for players who want it.

**The meter (`[smooth]` line, every second in hidden tests, one second in ten otherwise).** Measured in
the tick, once per presented frame, from fault-free reads: four joints (hips, both thighs, right forearm)
of each body's current pose and of Sam's newest capture.
- `fps` (overlay presents), `ticks`, frame time range, the frame cap and mask in force.
- Per body: `upd` model updates, `app` pose writes, `writes/frame 0:1:2+` (ticks with no new write, one,
  more), `moving` frames, `new` and `repeat` (exactly the same pose again), `stutter` (a step under 10 %
  of the running mean), `jerk` = mean |w(k) - w(k-1)| / mean w with w the joints' angular speed (step /
  frame time): 0 for perfectly even motion, about 1-2 when every other frame repeats.
- Partner: `interp`, `hold` and `torn` of the detour interpolation, `latency` = now - the play time of the
  pose on screen, and in echo (the partner replays our own Sam, `partner_character=sam`) `err`: the angle
  between the body's joints and Sam's captured joints at the same play time.
- Self: the capture age per write in frames.
- `Sam`: the same for Sam's own captures (the reference: what the game itself shows), `freq` = `+0x204`.
- `smooth 3 <n>` logs the next n frames one by one (`[smooth] seq`: frame time, each body's and Sam's step
  in degrees, pose writes).

**Evidence** (hidden, `-SeedSaves complete-100`, all pass, exit 0, no error.txt, real saves unchanged,
0 uploads; `[pose]` rej/torn/aba/wpm/faults 0). Averages over the seconds in which Sam ran the whole second;
"repeat" = share of moving frames that repeated the pose; jerk of Sam's own captures in brackets.

Run `20261117-144357-smooth-echo` (one game: echo partner 1 s behind, self view Fragile; no graphics
preset, 2560x1440; uncapped it reaches about 110 fps here; `01-mask0-120-towards.png` shows Fragile with
Sam's gear running towards the camera and the echo partner's tag behind her):

| mask | cap | fps | partner upd/s | partner repeat | partner jerk | echo err avg / max | latency | self upd/s | self repeat | self jerk |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 (v0.7) | 30 | 38.1 | 37.1 | 4.4 % | 0.95 (0.88) | 5.42° / 11.3° | 1026.7 ms | 36.4 | 5.8 % | 0.85 |
| 0 | 60 | 76.1 | 58.8 | 23.0 % | 1.00 (0.40) | 1.98° / 5.7° | 1015.6 ms | 58.8 | 23.0 % | 0.94 |
| 0 | 120 | 110.3 | 60.1 | 45.8 % | 1.79 (0.24) | 1.17° / 4.5° | 1013.3 ms | 60.1 | 45.8 % | 1.79 |
| 0 | none | 108.7 | 60.2 | 45.1 % | 1.80 (0.24) | 1.08° / 3.1° | 1013.3 ms | 60.2 | 44.9 % | 1.77 |
| 1 (boost) | 120 | 109.0 | 109.6 | 0 % | 0.22 (0.24) | 1.13° / 3.6° | 1009.2 ms | 109.6 | 0 % | 0.27 |
| 1 | none | 110.6 | 111.2 | 0 % | 0.20 (0.21) | 1.03° / 3.4° | 1009.1 ms | 111.2 | 0 % | 0.26 |
| 7 (all) | 30 | 36.8 | 37.2 | 0 % | 0.87 (0.82) | 0.21° / 2.2° | 1025.3 ms | 37.2 | 0 % | 0.66 |
| 7 | 60 | 77.3 | 78.0 | 0 % | 0.40 (0.44) | 0.32° / 1.7° | 1011.4 ms | 78.0 | 0 % | 0.49 |
| 7 | 120 | 112.6 | 113.0 | 0 % | 0.14 (0.22) | 0.37° / 1.8° | 1007.6 ms | 113.0 | 0 % | 0.26 |
| 7 | none | 111.6 | 111.9 | 0 % | 0.15 (0.24) | 0.37° / 1.9° | 1007.6 ms | 111.9 | 0 % | 0.28 |

Run `20261117-145300-smooth-two-games` (host Sam, guest Fragile with self view, both walking at once;
`graphics low` with the game's own fps limit raised to 120 through `-SettingsCfg`; two games reach about
80 fps each on this PC, with 10-24 % GPU and up to 30 % of all CPUs each, so the limit is CPU time
[I]; the run's screenshots do not show the other player, the numbers are from the counters):

| mask | cap | host fps | host sees guest: upd/s, repeat, jerk | guest fps | guest sees host: upd/s, repeat, jerk | guest self: upd/s, repeat, jerk | latency |
|---|---|---|---|---|---|---|---|
| 0 | 30 | 38.1 | 34.8, 10.9 %, 0.99 (0.80) | 38.6 | 36.0, 9.6 %, 0.88 (0.83) | 36.0, 8.2 %, 0.91 | 127 ms |
| 0 | 60 | 74.8 | 60.1, 20.3 %, 0.89 (0.28) | 73.8 | 60.3, 19.0 %, 0.82 (0.28) | 60.3, 18.9 %, 0.90 | 116 ms |
| 0 | none | 78.5 | 60.4, 23.5 %, 0.99 (0.24) | 79.2 | 60.2, 24.2 %, 1.02 (0.28) | 60.2, 24.3 %, 1.05 | 116 ms |
| 7 | 30 | 38.7 | 39.3, 0 %, 0.85 (0.92) | 39.0 | 39.7, 0 %, 0.86 (0.87) | 39.7, 0 %, 0.70 | 124 ms |
| 7 | 60 | 75.4 | 76.0, 0 %, 0.21 (0.27) | 72.8 | 73.2, 0 %, 0.24 (0.28) | 73.2, 0 %, 0.31 | 111 ms |
| 7 | none | 79.3 | 79.8, 1.0 %, 0.17 (0.25) | 81.9 | 82.2, 0 %, 0.19 (0.27) | 82.2, 0 %, 0.30 | 110 ms |

(The host's 1 % at mask 7 / none: one second in which the guest was quitting.) In the same run with the
game's own limit at 30 (run `20261117-144836-smooth-two-games`, plain `graphics low`) both games stayed at
30.0 fps whatever the frame cap, so the two masks looked the same (0 % repeats, jerk 0.2): at 30 fps
the 60 Hz body never repeated a frame. That is why the v0.7 tests never showed it.

Frame sequences (`smooth 3 240` while running towards the camera at the 120 cap, run `144357`): mask 0
alternates `0.0 / 10.0 / 0.0 / 9.4 ...` degrees per frame on both bodies while Sam moves `5.7 / 7.5 / 7.8 /
13.3 ...`; mask 7 moves `4.2 / 4.7 / 4.3 / 4.6 ...` on the partner and, on the self body, exactly Sam's
step of the frame before. CPU was the same with and without the boost (33 % of all CPUs uncapped, 30 %
at the 120 cap, `resources.csv`).

Repeated with the final build (run `20261117-150148-smooth-echo`): mask 0 again 60 updates/s and 39-44 %
repeats at 98-108 fps (jerk 1.5-1.8), mask 7 again 0 % repeats at every rate (jerk 0.13-0.14 at
110 fps, echo error 0.21-0.40° average), 17 setter calls, 0 faults.

The "cap" column is the frame cap asked for (`smooth 2 <fps>`, the mod's `frame_cap` at run time); it is
loose: 30 gave 37-39 fps and 60 gave 73-78 fps (the "fps" column is measured). Not investigated [I: it
waits only on ticks that see a new present count].

Final build, two games (run `20261117-154136-smooth-two-games`, `graphics low 120`, the integrated v0.8
code with gear, collision and font): the games reached 64-69 fps uncapped this time (Sam moving the whole
second; Sam's own jerk in brackets):

| mask | cap | host fps | host sees guest: upd/s, repeat, jerk | guest fps | guest sees host: upd/s, repeat, jerk | guest self: upd/s, repeat, jerk |
|---|---|---|---|---|---|---|
| 0 | 30 | 37.5 | 35.6, 6.9 %, 0.67 (0.59) | 39.0 | 39.1, 1.8 %, 0.59 (0.55) | 38.4, 3.4 %, 0.61 |
| 0 | 60 | 63.5 | 58.2, 9.1 %, 0.57 (0.31) | 65.3 | 60.1, 10.2 %, 0.54 (0.28) | 59.7, 9.6 %, 0.55 |
| 0 | none | 64.4 | 58.2, 10.0 %, 0.57 (0.30) | 66.5 | 59.0, 11.7 %, 0.62 (0.28) | 59.4, 11.5 %, 0.60 |
| 7 | 30 | 36.8 | 37.3, 0 %, 0.55 (0.58) | 38.5 | 39.1, 0 %, 0.54 (0.56) | 39.1, 0 %, 0.58 |
| 7 | 60 | 67.7 | 68.2, 0 %, 0.22 (0.28) | 67.1 | 67.6, 0 %, 0.21 (0.27) | 67.6, 0 %, 0.26 |
| 7 | none | 69.0 | 70.1, 0 %, 0.25 (0.31) | 68.2 | 68.7, 0.3 %, 0.26 (0.29) | 68.7, 0 %, 0.29 |

Latency 112-128 ms; setter 20 calls, 0 faults; `[pose]` rej / torn / aba / faults 0. The self body's jerk
is slightly above Sam's in some rows of the one-game table (0.47-0.49 vs 0.41-0.44 at the 60 cap): it is
one frame behind Sam (8.2), so "equal or below Sam's" holds for the partner, not always for the self body.

Scenarios: `smooth-echo` (one game), `smooth-two-games` (two games; `graphics low 120` raises the game's
own limit, docs/hidden-testing.md).

## 4. Wire format and interpolation (`pose_wire`, ctest `st_net_tests`)

**STJ1 frame.** It is 497 bytes for 77 joints, 505 with the outfit block (section 9):

| Part | Contents |
|---|---|
| Header | `'STJ1' \| u32 seq \| u32 sender_ms \| u32 layout_hash \| u8 joints \| u8 body_id \| u8 flags \| u8 0` |
| Rotations | 77 × 48-bit smallest-three |
| Translations | `u8 n` + n × `{u8 joint, i16 xyz}`, 0.1 mm units (root and pelvis) |
| Outfit (flags bit 1, `kJointsOutfit`) | 8 bytes: `variant \| exos \| suit colour \| cap colour \| glasses A lens \| glasses B lens \| glasses A frame \| glasses B frame` (section 9) |

- The decoder checks the size against the counts and the outfit flag. A set flag without the block, or a
  block without the flag, is rejected (ctest `st_net_tests`, `TestJointOutfit`). Released builds never
  mix: every release bumps the version, so the build ids differ and play mode refuses the HELLO
  (below). Two builds of the same commit with different working trees share one build id (CMake takes
  version + `HEAD`), so a local build of uncommitted changes can meet a release of that commit; such a
  v0.5.6 game rejects every 505-byte frame (its body would get no joint frames at all). Development
  builds of one tree should therefore be tested against each other only.
- Flag bits the decoder does not know (anything but bits 0 and 1, `kJointsKnown`) announce blocks of a
  newer build: such a frame may be longer than the known parts, the rest is ignored and the unknown bits
  are cleared. The known parts must still be complete (tested). This only helps builds from now on.

- Round-trip error is below 0.02° per joint (tested).
- It rides in the same datagram as STP1. STP1's layout is unchanged; its flags bit 1 (`kPoseInVehicle`,
  reserved until now) is set while the sender's Sam has a parent entity (vehicle, mount), for player
  collision (section 11.2). Older decoders kept the flags as sent, so the size and layout stay the same.
  Mixed mod versions never get this far: the
  mod's link runs in play mode, where the HELLO is rejected when the builds differ (`[link] rejected:
  build_mismatch`, the launcher's "different mod version"), so neither player sees a partner. A peer
  that sends no STJ1 is therefore always a same-build game (see "Fallback" in section 5).
  The build id is the version and the commit; a build configured from a tree with changed tracked
  files adds `-` and two hex digits of a hash of the wire's source files (`pose_wire`, `link_protocol`;
  `0.9.0+bad8ed9-3f`, `CMakeLists.txt`; before v0.10 a bare `-m`), so a development build never passes the
  check against the release made from the same commit, and two development builds whose frames differ
  fail it too. It is taken when CMake configures, and editing one of those files re-runs the configure step.
- A frame counts only when the body's ring accepts it (`realbody::PushFrame` returns true: 77 joints,
  layout `BE39C83F`, newer than the last). A frame it drops does not set the peer's character, does not
  count as "the peer sends joints" for the fallback, and the first 5 drops are logged
  (`[sync] joint frame seq N dropped: ...`). [I: no run has sent such a frame.]
- `link.cpp` gives it its own queue, its own newer-seq filter and the same generation counter
  (critique B3).

**Echo.** Sam's captured pose goes through encode and decode every send tick (30 Hz). It is pushed into a
256-entry ring keyed on the transform ring's unwrapped sender time. It is sampled at the same play time as
the transform (`Ring::PlayTime`) and nlerped per joint, with the hemisphere fix. Since v0.8 (section 3.5)
`sender_ms` of STJ1 is the time Sam's pose was captured (QPC, up to a frame before the send tick), and the
partner body's own model update interpolates at its own moment.

**Network (host and guest).** Run between two hidden games (section 5, runs 20261116-193151 and
20261116-193649): 565 B of payload per datagram (STP1 + STJ1 frames), 16.95 KB/s at 30 Hz each way,
`[pose]` rej/torn/aba/wpm/faults all 0, no error.txt.
- `SendPoseAndJoints` sends the pose and the joints, with `body_id` = this player's own character
  (section 5).
- `PopJoints` → `realbody::PushFrame`, keyed the same way. The `body_id` goes through the debounce
  (section 5).
- `link::Info` counts joint frames each way and the link body bytes sent. Every 10 s the sync logs
  `[sync] link traffic over 10 s: sent N poses (M with joint frames), X B/s payload (Y B each); received
  N poses, M joint frames`. The payload is the FRAME messages, without the link header and encryption.

## 5. Character choice (shared contract with the launcher)

The mod and the launcher (docs/launcher.md) both follow these rules:

| Rule | Mod (`body_choice.cpp`, `player_sync.cpp`) |
|---|---|
| `session.ini my_character=<body key>` is the character THIS player appears as on the other player's screen. | `ChooseOwn`. It is sent as STJ1 `body_id`. A missing or unknown key is `sam`. A known key that is not offered is sent as asked, and the receiver shows Sam for it. |
| The guest is Fragile in her porter suit (`fragile_suit`, id 16). | Launcher only: Join, the local test's guest, echo and solo write `my_character=fragile_suit`; the other characters are offered only with the launcher's Debug option "Allow other characters" (docs/launcher.md). The mod is unchanged: it plays whatever key it is given (a hand-written session.ini without the key still sends `sam`). |
| The host is always Sam. | With `role=host` the mod sends `sam` whatever `my_character` says, and logs `[sync] my_character=<key> ignored: the host is always sam`. The launcher's Host screen says "You appear as: Sam (the host is always Sam)" and writes `my_character=sam`. |
| The receiver shows the peer's `body_id`, not its own config. | `ChoosePartner`. The peer's id is shown if it is offered. Otherwise (unknown, 0, not offered, or no STJ1 frame yet) Sam is shown. |
| A change of the peer's `body_id` respawns the body only after it has been stable for about 30 frames. | `StableId`, `kStableFrames = 30` received STJ1 frames (about 1 s at `send_hz=30`). The first id is taken at once. Going back to the shown id restarts the count, so a flapping id never respawns the body. `FollowPeerCharacter` then removes the real body and the next spawn uses the new character. A new link session resets it. |
| `partner_character` is a debug override. | If it names any body in the table (offered or not, for debug tours), it wins over the peer's `body_id`. An unknown key is logged and ignored. |
| The `offered` flag is the one list. | `body_table.cpp` has no Windows or game includes, and the launcher compiles it directly. The launcher's Join screen offers exactly the bodies for which `IsOffered` is true (`OfferedCount`/`OfferedAt`). |
| `partner_body=real\|holo` decides how the partner is drawn on this screen, and whether this game sends its character at all (only `real` installs the pose hook, see "Who sends joint frames"). | Unchanged. The launcher writes it from a Debug option, "Partner body: Real character (experimental) / Hologram", default Real character. |
| The player is told plainly: your partner sees you as the chosen character, and so do you on your own screen (the self view, section 8; Debug option "See yourself as your character", default on; with it off the note says you stay Sam). With Hologram, your partner sees you as a hologram. | Launcher text: the Join note; with Hologram set, a warning on Join, Host and the local test's "Guest appears as" ("Partner body is Hologram (Debug page): your partner sees you as a hologram"), and the Debug option's own text says so. |
| `session.ini self_character=1`: this player sees themself as `my_character` too. | `ConfigureSelf` from `player_sync` init, only when the own character (after `ChooseOwn`) is not Sam and the pose hook is installed. The launcher writes it for a non-host whose character is not Sam, with Partner body = Real character and the Debug option on; a missing key is off. |

**Who sends joint frames.** Sending STJ1 needs the pose hook. The hook is installed at start-up only when
`partner_body=real` or `pose_hook=1`, and never mid-game (section 3.2). The decisions:
- A real-body game always sends STJ1 with every pose once Sam's pose is captured. Sending does not
  depend on what that game shows. It still sends after its own partner has fallen back to the hologram.
- A hologram game sends no STJ1 (unless `pose_hook=1`). The hologram stays the safe path: no hook on the
  game's animate function. A real-body peer of a hologram player therefore shows the hologram, through
  the fallback below. This is also the case the `two-games-real-fallback` scenario tests.
- The launcher defaults to Real character, so players normally send.

At start-up the sync logs which case applies:

```
[sync] my character: 'fragile' (body_id 3); joint frames (STJ1) sent with every pose
[sync] my character: 'fragile' (body_id 3); joint frames (STJ1) not sent (no pose hook: partner_body is not real)
[sync] my character: 'sam' (body_id 1); joint frames (STJ1) not sent (the pose hook is unavailable)
```

**Receiver log lines:**

```
[sync] the partner sends joint frames (STJ1), body_id 3 (fragile)      raw id changes (first 20 only)
[sync] partner character: 'fragile' (peer body_id 3, first frame)       the shown character
[sync] partner character: 'porter' (peer body_id 2, stable for 30 frames)
[sync] partner character: 'sam' (peer body_id 17, first frame; not an offered body, shown as sam)
[sync] partner character now 'fragile' (was 'sam'): respawning the real body
[sync] partner body: real 'fragile'                                     the body is live
```

**Fallback when the peer sends no STJ1** (a same-build peer running `partner_body=holo`, or a peer whose
capture is unavailable, paused or behind a notice; other mod versions are rejected at the handshake,
section 4) [V: run 20261116-193451-two-games-real-fallback]. Starvation is not proof of a hologram peer:
a game behind a pause or a notice still sends positions but runs no animate, so it sends no joints. The
guest behind the offline-mode notice did exactly that (run 20261116-192744: 0 joint frames sent, no
`[pose]` calls at all; the host fell back for good, and the run failed). So:
- **No real body before the first STJ1 frame.** It would stand in its bind pose, and the character is
  not known yet (the body_id comes with STJ1). The sync waits 3 s, then the hologram stands in. The 3 s
  count only while positions arrive over a live link with Sam awake: link down, 10 s of partner silence,
  Sam asleep or a new link session clears the wait (its own timer, `g_first_joints_wait_ms`), so a wait
  cut short does not fall back at once when positions resume. [I: not exercised by a run.]
- **Back to the real body.** After a starvation fallback, 30 fresh joint frames (about 1 s) bring the
  real body back, as the peer's character.
- **Holding a pose.** A live real body whose joint frames stop holds the last pose while the peer stands
  still (paused, in a menu). It falls back only when no STJ1 came this session, or when the peer moved
  more than 1 m since the last joint frame (it would glide). [I: not exercised by a run yet.]

```
[sync] partner body 'real': waiting for the partner's first joint frame (STJ1)
[sync] partner body 'real' unavailable (the partner sends positions but no joint frames (STJ1) for 3 s): falling back to 'holo'
[sync] partner body: hologram <hash>
[sync] the partner sends joint frames (STJ1) again: back to the real body (from 'holo')
[sync] partner body: real 'fragile'
```

In run 20261116-193451 the host waited 3.2 s, fell back, and showed the hologram 1.1 s later; it never
switched back (the guest, a hologram game, sent 0 joint frames). In run 20261116-193151 the guest was
still behind its notice: the host showed the hologram, and 1 s after the guest's first joint frame it was
back on the real Fragile body.

**Hidden scenarios.** They use `@host ini` / `@guest ini`: per-game session.ini lines, see
docs/hidden-testing.md.
- `two-games-real`: both games `partner_body=real`, host `my_character=sam`, guest
  `my_character=fragile`. It waits for `[sync] partner body: real 'fragile'` (host) and
  `[sync] partner body: real 'sam'` (guest). Frames:
  - `03-guest-sees-host-walk-a..d`: the guest watches the host's Sam body walk away.
  - `05-host-sees-guest-standing` and `06-host-sees-guest-walk-a..d`: the host looks back at the guest's
    Fragile body, standing and then walking.
  - It also records `real_stats` (`[pose]` counters), `log_status` and the traffic line.
- `two-games-real-fallback`: host `partner_body=real`, guest `partner_body=holo`.
  - It waits for the waiting line, then the fallback line, then `[sync] partner body: hologram`. Then,
    as required steps: both traffic lines show 0 joint frames, and `nolog` fails the run if the host
    log has `again: back to the real body` or `partner body: real` (checked before `03` and again after
    the traffic lines, before `05-host-still-hologram`). [These checks were added after run 193451 and
    have not run yet.]
  - `03-host-sees-guest-hologram` is the host's view after the fallback.
  - The log stamps show how long the switch took (3 s wait plus about 1 s for the hologram).

## 6. Remaining work to productize

1. **Two-instance test: done on one PC** (section 5; runs 20261116-193151, -193451, -193649). STJ1 over
   UDP loopback, the character choice (Sam, Fragile, Cliff) and the fallback work; 565 B per datagram at
   30 Hz. Not proven: two PCs over a real network (loss, reordering, higher jitter), a pause longer than
   3 s mid-session (the pose hold), a change of `body_id` mid-session (the respawn; unit tests only),
   link loss over the network, a session.ini written by the launcher (the runs used scenario `ini`
   lines), and ids other than 1, 3 and 5 over the link (the others were shown only in solo mirror runs).
   **Which build each run used.** Runs 193151-194030 (two games, fallback, echo) predate the Heartman leg
   latch in `real_body.cpp` (19:57) and `offered=true` for ids 6-16 in `body_table.cpp` (20:05); the
   tour runs 194441-200116 had the latch. After the review fixes (joint frames counted only when the
   ring accepts them, the first-joint wait's own timer, the launcher's Hologram notes) only
   `two-games-real` was run again (section 1, final run); `two-games-real-fallback` (now with `nolog`
   checks), `echo-real-100` and a two-game run with a newly offered id were not.
2. **More characters: done for the table.** All 16 ids passed `real-body-tour-all` and are offered (section 2).
   Not checked per character: collision size, and how a female body looks playing Sam's motion over time.
3. **Gear: done for what the game draws as plain entities** (section 12, v0.8). Copies of the sender's own
   gear entities (by resource, STR1), its gear parts and the backpack's parts, synced over the link (STC1
   v2); the self view keeps Sam's real gear. Not drawn: the weapon in hand, the Odradek, the cuff device,
   the exoskeleton on a non-Sam body; the pod is a tilted cinematic prop; colours not matched.
4. **Faces.** Not researched: drive DSFacialRig on Sam's body, or leave the faces static.
5. **Cutscenes and photo mode.**
   - Cutscenes already remove the body (Sam asleep).
   - Photo mode calls the forced update with dt 0; the detour handles it, but it was not tested.
   - Whether a `demo_*` entity we spawned can be bound by a running sequence that uses the same body is
     unknown. Checking for an active sequence before spawning would be a cheap guard.
6. **Collision: done** (section 11). The partner's real body gets the game's own character capsule, so
   the local Sam is stopped by it as by another character. The `demo_*` bodies themselves have no
   physics component; the possible push seen in run 20261116-193151 was not reproduced.
7. **Release switch: decided.** The two-game run and the fallback run passed, so the launcher writes
   `partner_body=real` by default; the option stays on the Debug tab, labelled experimental, with
   Hologram as the manual choice. `holo` stays the automatic fallback. Revisit (move the option off the
   Debug tab, drop "experimental") after a two-PC session.

## 7. Debug actions and scenarios

See [debug-actions.md](debug-actions.md) "Real partner body". Scenarios:
- `real-body-diag`: `real_control` (a collectors hologram through the same path+GUID route), Sam, porter.
- `real-body-tour`: probe, Sam own pose → mirror → walk frames, then porter, Fragile, Die-Hardman, Cliff.
- `real-body-tour-all`: every id 1-16 in mirror mode: idle and two walk frames each, with load time,
  skeleton, joint-name map, pelvis and leg ratios in the log.
- `echo-real-100` and `echo-real-fragile-100`: the echo partner as a real body. They use the new
  `ini key=value` and `pad ... keep` scenario features (docs/hidden-testing.md). In echo the "peer's"
  `body_id` is the game's own `my_character` coming back through encode/decode. `echo-real-fragile-100`
  shows Fragile through the `partner_character` override.
- `two-games-real` and `two-games-real-fallback`: host and guest (section 5).
- `self-view`, `self-view-edges` (solo) and `two-games-real-self` (host and guest): the self view (section 8).
- `gear-self` (solo) and `two-games-gear` (host and guest): gear on the bodies (section 12).
- `smooth-echo` (echo partner and self view in one game) and `smooth-two-games` (host and guest): frame
  rates 30 / 60 / 120 / none, smooth mask 0 against 7, the `[smooth]` meter (section 3.5).

## 8. Self view: you see yourself as your character

**Status (2026-11-17): works in hidden solo runs (Fragile); behind `session.ini self_character=1`, which the
launcher writes by default for a guest whose character is not Sam (Debug option "See yourself as your
character", needs Partner body = Real character).** On this player's own screen the local Sam is replaced
by the chosen character: a second real body (its own slot in `real_body.cpp`, log prefix `[self]`) stands
at Sam's world transform and plays Sam's live pose, and Sam's own model is hidden. The host is always Sam
and a guest who picked Sam keeps Sam, so neither gets a self body (`player_sync` init calls
`realbody::ConfigureSelf` only for a non-Sam own character, and only with the pose hook installed).

### 8.1 Hiding the local Sam: his mesh controller, reason 63

From static work (Ghidra, capstone; every byte pattern below re-checked against ds.exe) [V]:
- `DSPlayerEntity +0xA60` is Sam's mesh-visibility controller (its per-frame update is called with
  `lea rcx,[rdi+0xA60]` at `0x1427313D4`; `+0` of the controller points back at the entity).
- Its `+0x338` (player `+0xD98`) is the **Root hide-reason mask** (u64). Any set bit hides every
  ArtPart of Sam (`ApplyMeshControl 0x1426D9120` on the next change; `SetRootVisible 0x1426B4E40` is
  a plain `bts/btr`). The inventory component's visual update asks `IsRootVisible 0x1426A0DE0` (its only
  caller `0x1427F0B43`) and hides the backpack sub-entities while the mask is non-zero.
- Photo mode's "hide Sam" uses reason 26 (`0x1426C1770`); the game's other reasons seen are 0, 1, 3, 8,
  0xC, 0x17, 0x1D, 0x1E and 27. **Ours is 63.** It is set with `InterlockedOr64` and cleared with
  `InterlockedAnd64` (a game bit can never be lost; the game's own writes are not atomic, so ours is
  re-asserted every tick), after fault-free reads confirm the entity's vtable is DSPlayerEntity's
  (`0x143D7D0D8`) and the controller points back at it.
- **Since v0.8 our bit lives in the controller's global part mask `+0x348` (player `+0xDA8`), not in the
  Root mask** (section 12.2). The part masks start there (part k at `+0x348 + 8k`, k < 89); mask 0 is never
  written by `SetPartVisible 0x1426B4D80` (part 0 goes to the Root mask), and ApplyMeshControl reads it as
  a second global condition (`0x1426D91EF cmp qword [rbx+0x348],0`; the diff loop `0x1426D915A` covers
  it): any bit hides every ArtPart of Sam (the top-level setter, then each mapped part). IsRootVisible
  reads only `+0x338`, so the inventory keeps every gear entity shown and attached to Sam's animated,
  invisible skeleton. Both sites are byte-checked. A stray bit 63 left in the Root mask by an older build is
  cleared by the repair. Census: run `20261117-130343` (`gear 9 0`).
- **Clearing it restores what the game's own masks say** [V: decompile of `0x1426D9120`] (review asked
  whether turning the self view off could re-show parts other systems hid): the function treats `+0x338`
  and `+0x348` the same way (`mask[0x67] == 0 && mask[0x69] == 0` before each part's own test). When both
  are zero it recomputes every part from that part's own masks (`+0x348 + 8k`, the `+0x8D8` / `+0xE68`
  arrays and the `+0x19A0` byte); then it copies the masks into their "last applied" copies. So a hat, the
  hood, an outfit variation or the pod part hidden through their own masks stay hidden after our bit
  clears, exactly as after the photo-mode reason 26 or v0.7's Root-mask bit. Not covered: parts a system
  hides by calling the ArtParts setters directly, outside the controller (the same for the Root mask).
- Not used: Entity SetVisible (Wake/Sleep undo it, no children), the hidden bit, direct ArtPart calls
  (overwritten by `ApplyMeshControl`), camera visibility.
- The game has two more ways to hide Sam, and the self body follows both (it is not shown while either
  hides Sam): the entity's visible flag (`+0x88 & 0x2`, read with `partner::ReadFlags`), and
  IsRootVisible's second condition, a global counter at `0x147BD1A48` (`cmp dword [rip+x],0` at
  `IsRootVisible+10`). Its only writer `0x1424B3EF0` raises it when a scripted scene hides the entity in
  `0x147BD1A40` and lowers it when the scene shows it again [V: Ghidra decompile and xrefs].

Byte-checked at first use (`CheckMeshSites`, logged `[self] mesh-control sites verified`): SetRootVisible,
ApplyMeshControl, the controller update and its call site, the photo-mode site, IsRootVisible and the
inventory call site (both call targets), the scene-hide counter (taken from IsRootVisible's own
instruction, must be readable), the DSPlayerEntity vtable, and `Entity_ExportedSetWorldTransform`.
A mismatch turns the self view off (logged); Sam stays.

### 8.2 Placing and posing the body every frame

- **Pose:** in the detour, the self body's branch writes the newest capture of Sam (no wire, no ring, no
  delay) with the same latch, joint map and pelvis ratio as the partner (section 3.2), then reruns the
  model-matrix and bounds functions. Measured: pose age at write **5-8 ms average at 134 fps presented
  (a 7.5 ms frame), 32 ms at the 30 fps cap of two games (a 33 ms frame)**, never the same capture
  written twice (`stale=0`), 60 writes/s solo [V: `[self]` lines, runs below]. The age is one frame at
  both rates, so the body's model is updated before Sam's in a frame and **shows Sam's pose of the
  previous frame** [I]; the transform is this frame's (below).
  - **v0.7: the body animated at 60 Hz, Sam at the frame rate (fixed in v0.8, section 3.5: both now
    every frame, the age a constant one frame at 60 fps and above).** The game updates the body's model 60 times a
    second (`app=60` while Sam's transform is copied 100-126 times a second solo, run
    `20261117-082028`). Above 60 fps every other presented frame repeats the body's last pose, so it is
    up to about two frames old there and the limbs move at half Sam's rate [I from the counters]; at the
    30 fps cap of two games every frame gets a new pose. The age above is sampled only at the writes, so
    it does not show the repeated frames. In stills nothing of it is visible (ground contact is
    unaffected: the transform is this frame's). Driving the body's model from Sam's update instead would
    race the game's own update of it on another thread; not done. Making the body update after Sam (parenting
  it to Sam, `SetParent 0x142361DD0`, so both share one update group [I]) would remove that frame; not
  tried (the child must be unparented before every removal, and a load that frees Sam would take a
  parented body with it).
- **Since v0.19 the default is follow mode 4 (section 26):** mode 1 below plus, in the body's own update before its pose
  write, Sam's world predicted for this frame. Mode 1 alone left everything the game hangs on Sam one frame ahead of
  her while moving (the user's v0.18.0 play test: "everything ... get detached from her when she moves").
- **Transform (follow mode 1, the default until v0.18):** Sam's own model update (the detour's Sam branch) reads his
  world transform fault-free (twice, compared) and sets it on the body through
  `Entity_ExportedSetWorldTransform 0x142361EA0` (the body's own transform lock and dirty flags). That
  is the only game call made with a lock from a detour; it touches only our entity. Sam's model is
  updated about twice per frame (100-165 copies/s).
  - Before that call the detour checks, fault-free, that the published body is still ours: the entity
    still has the vtable it had when it was validated and still points at the published model, and the
    model has the SkinnedModel vtable and points back at it (the same ABA check the pose write makes).
    A body the game freed between two ticks gets no call (counted as `copies ... aba N` in the stats
    line; 0 in every run). The check and the call are not atomic: an entity freed in the microseconds
    between them is the same accepted window as the pose write's [I].
- Body-to-Sam distance at tick time [V, run 20261117-072132]:
  - mode 1 (Sam's update): **0.0 cm average and max**, every second, standing, walking, running, and on
    the purple four-wheel roadster ("SDT"; run 20261117-072538, where it was called the trike);
  - mode 2 (the body's own update copies it): **5-5.5 cm average, up to 10 cm** while running (the body's
    update runs before Sam's final move of the frame) - not used;
  - mode 3 (the tick, like the partner): kept as the fallback (the tick moves the body whenever no copy
    came for 100 ms). Its 0 cm in the log is meaningless (measured right after its own move).
- The game updates the moved body by itself (`game-drove`, `forced=0`), at 60 Hz in v0.7 and every frame
  since the 240 Hz boost (section 3.5); while it is still
  invisible and latching, the tick's forced update drives it (section 3.3).
- `self_view <0/1> <body id> <follow>` switches it at run time (docs/debug-actions.md).

### 8.3 Showing and restoring (game thread, `SelfTick`)

The body spawns invisible at Sam's transform (0.01-0.02 s), latches (10 frames) and is shown only when it
is latched, posed within the last 250 ms, the game sets no Root reason of its own, Sam's entity is
visible (flag `0x2`) and the scene-hide counter is 0 (section 8.1). Then, in one tick:
`SetVisible(body, true)` and reason 63 on. Sam is never hidden by us without the body shown:
- the game hides Sam itself (any other Root bit: photo mode, scripted scenes; his entity flag; the
  scene-hide counter): reason 63 is cleared and the body hidden until that goes (a game Root reason
  simulated in run `20261117-082028`, below);
- Sam asleep (load, cutscene, knot space) or no local player: reason 63 cleared at once, the body
  removed after 1 s, a new one spawned after Sam wakes (Sam is visible for the 1-3 s that takes);
- a new player entity: the old one's reason is cleared only if it still reads as a live DSPlayerEntity
  with its controller pointing back (otherwise nothing is written); the body is removed and respawned;
- no fresh pose, a fault in the pose write, the body destroyed by the game, self view off: Sam first,
  then the body.
- **A lost clear is repaired.** The game's own `SetRootVisible` is a plain read-modify-write of the mask
  (`mov r8,[..]; or/and; mov [..],r8`), so one that overlaps our locked clear can write bit 63 back. A
  lost set is harmless (re-asserted every tick while shown); a lost clear would leave Sam invisible with
  no body. So every tick in which the body is not shown, whatever the self view's state (off, asleep,
  loading), the current player's mask is checked (fault-free, vtable and back pointer) and a stray bit
  63 cleared (`SelfRepair`, logged `[self] reason 63 was still set on Sam while the body is not shown (a
  lost clear): cleared ...`, counted as `repairs` in the stats line; simulated in run `20261117-082028`).
- **A dead tick.** An exception anywhere in the per-frame sync turns co-op off for the run (dllmain
  `GuardedTick`); the tick never runs again. `realbody::EmergencyStop` then runs once (SEH-wrapped):
  the detour stops all work (both slots unpublished, no capture, no transform copy), reason 63 is
  cleared on Sam and a shown self body hidden. Logged `[self] emergency stop ...`. Not triggered in a
  run (it needs a fault) [I].
- The entity checks before every locked write are fault-free, but the write itself is direct
  (`InterlockedOr64`/`And64` on the game's mask): an entity freed on another thread between the check
  and the write is a small accepted window (the change-of-player and asleep paths) [I].
- **Start-up delay.** In the two-game runs the body was shown about 4 s (`073349`) and 13 s (`075954`)
  after the player appeared, 6.6 s solo (`082028`, after two player-entity changes during the load), 9.7 s
  after latching in `082759`:
  until then no fresh Sam pose was written onto the body (the stats line counts `nocap`, the self view's
  "no captured pose to copy"; earlier builds called it `torn`), for example while the offline notice is
  up. Sam stays visible meanwhile.

Logged: `[self] showing 'fragile' in place of Sam (... show N)`, `[self] Sam shown again (<why>): reason 63
cleared, mask now 0x0; body hidden`, `[self] body not shown: <why>` on change, and a `[self]` stats line
every second (writes, copies, pose age, distance, Sam's mask, entity flags, scene-hide counter, repairs,
counters).

### 8.4 Gear (decision; superseded by section 12)

Since v0.8 the self view hides Sam through the global part mask (8.1), so the game keeps drawing his real
gear on his invisible skeleton (section 12.2); before that (v0.7) the self body carried the backpack frame and
cargo cases as props. The first version's decision, for the record:

Root hides all of Sam's gear: the backpack, the cargo on it, the boots, BB pod and exo parts. The
chosen character's cutscene body has none, so **a non-Sam self view shows no cargo or backpack** (the
partner body has none either, section 1). This is deliberate for the first version: per-part hiding
(meshes 1..88 with reason 63 through `SetPartVisible 0x1426B4D80`) would keep Sam's gear drawn on an
invisible skeleton sized for Sam, a few cm off a Fragile or Cliff body; not tried. In the runs no item of
Sam's was seen floating next to the body (back views `01`, `07`; seated; in the roadster) [V for this save].

### 8.5 Evidence (hidden, `-SeedSaves complete-100`, exit 0, no error.txt, real saves unchanged, 0 uploads)

| Run (`build-testenv\runs\`) | What it shows |
|---|---|
| `20261117-082028-self-view` (**the final code**, review fixes included) | `01-standing-back`: a solid Fragile from behind (blonde, black leather jacket with the FRAGILE back logo) in Sam's idle; no Sam, no backpack. `02-looking-at-camera`: Fragile facing the camera. `03-towards-b`: running, one knee raised. `05-walk-a`: a walking stride. `06-away-b`: running away. `07b-game-hides-sam` (a game Root reason simulated, bit 26): nobody is drawn, neither Sam nor Fragile, and no floating gear; the log shows reason 63 cleared, mask `0x4000000`, body hidden. `07c-shown-again` (bit 26 cleared): Fragile back (`show 2`). `08-off-sam-back`, `09-off-sam-facing` (`self_view 0`): Sam in his gold suit with backpack. `08b-after-repair` (a stray bit 63 written with no body shown): Sam still visible 1 s later; the log shows the repair in the same second (`repair 1`, mask back to 0). `10-on-again`: Fragile facing the camera. Stats: follow 1 distance 0.0 cm, pose age 6.8-8.8 ms average, `app=60` against 100-126 copies/s, copy `aba 0`, `nocap 0`, Sam's flags `0x232854453` (visible), scene-hide counter 0. |
| `20261117-082759-two-games-real-self` (**the final code**; the user's case: the guest picked Fragile, self view on; graphics low, 30 fps) | Guest `02-guest-sees-self`: its own Fragile from behind (blonde, black jacket, no backpack) and the host's Sam body beside her in the gold suit (the guest dressed it: v01 removed, exos 31 added). `06-guest-self-walk`: Fragile walking, the host's Sam small in the distance. Host `02-host-control`: its own Sam unchanged (gold suit, backpack); `06-host-sees-guest-walk-b`: the guest as a small dark Fragile walking in the background. The host log has no `[self]` line and no `[outfit] body` line (its partner is Fragile). Guest stats: pose age 31.9 ms, `app=30` = copies, distance 0.0 cm, repairs 0. The self body was shown 9.7 s after it latched (`nocap=30-31` per second until then). Both exit 0, no error.txt, real saves unchanged, 0 uploads. |
| `20261117-072132-self-view` (an earlier build, as are the next four) | `01-standing-back`: a solid Fragile (blonde, black leather jacket with the FRAGILE back logo), standing in Sam's idle; no Sam, no backpack. `02-looking-at-camera`: Fragile facing the camera, arms slightly out (Sam's idle), both feet on the floor. `03-towards-a..d`: running at the camera past the trike, legs alternating, a knee raised, arms swinging, she stays in the same place on screen. `05-walk-a..c`: a walking stride (half stick). `06-away-a..c`: running away, back logo visible. `08-off-sam-back`, `09-off-sam-facing` (`self_view 0`): Sam again, in his gold/yellow suit with the big backpack and gear. `10-on-again`: Fragile back in his place. `11` (follow 2) and `12` (follow 3): running frames; in stills they look like mode 1. |
| `20261117-072538-self-view-edges` (an earlier build) | The purple four-wheel roadster ("SDT"; the frame names say trike): `02-at-trike` (Use Vehicle prompt), `03-mounted`: Fragile seated in it, hands on the wheel; `04-riding-a`: driving, a small blonde head behind the seat (weak evidence); `06-after-x`, `07-off-trike`: off again, standing beside it. `10-private-room-a/b` (hold B away from the pad: Sam sits down instead): Fragile seated on the floor, knees up, the rest prompts shown; `11-back-a/b`: standing again. No `[self]` hide/show events: the mask stayed `0x8000000000000000` (the game set no Root reason for the vehicle or resting). |
| `20261117-072827-self-view-edges` | The same plus a 3 s hold of X (Sleep) while seated: the game did not start a nap (`13-nap-a..d`: still seated), so the asleep path was not exercised by the game. |
| `20261117-073349-two-games-real-self` (an earlier build) | Two games, the guest with `self_character=1` (graphics low, 30 fps). Guest: `02-guest-sees-host`/`02-guest-sees-self`: its own character is a solid Fragile from behind (no gold suit, no backpack), the host's Sam body (dark porter suit) beside her; `06-guest-self-walk`: Fragile walking away, the host's Sam small in the distance; `08-guest-self-facing`: the host's Sam body (porter suit) stands 0.5 m in front of the camera with Fragile's blonde head behind his shoulder (both players end at the same spot). Host: its own Sam unchanged (gold suit, backpack), the guest as a running Fragile (`06-host-sees-guest-walk-b`); `nolog` confirmed no `[self]` line in the host log. Both exit 0, real saves unchanged, 0 uploads. |
| `20261117-073210-self-view` (an earlier build: before the outfit work and the review fixes) | The player-change rule above: the same frames as `072132` (Fragile standing, facing the camera, running; Sam in his gold suit and backpack after `self_view 0`); distance 0.0 cm in every mode-1 second, 5-5.4 cm average (max 11.5) only in the mode-2 seconds. |

### 8.6 Not covered

- The asleep restore path (cutscene, load, knot space) was not triggered by the game in a run (no
  cutscene reachable from the scripted start). The debug off path ran (`self_view 0`: reason cleared,
  mask 0x0, body removed, Sam visible in `08`) [V]; the asleep path calls the same `SelfUnshow` [I].
- The "the game hides Sam itself" rule ran only with a simulated reason (bit 26 written by the hidden-
  test action, `082028`), not with the real photo mode; the entity-flag and scene-hide-counter rules
  never fired (flags `0x232854453`, counter 0 in every stats line) [V log].
- Photo mode, ziplines, ladders, aiming, the private room, death and respawn, a second player change
  while shown.
- Sam's particle effects (rain, dust) stay on his invisible bones [I]; shadows were not judged (dim scene).
- Faces do not move; Fragile plays Sam's male motion; things Sam holds (his helmet in `09`) are not drawn
  with her.
- Collision: none seen (the body stands exactly on Sam, distance 0.0 cm, and Sam was never pushed).
- Two PCs; the launcher's own local two-player test (the run above wrote the same ini lines by scenario).

### 8.7 The cargo menu still shows Sam (superseded by section 15)

**Since section 15 the on-foot cargo menu shows the self view's character.** What follows is the earlier decision
and its probes, for the record (the BACK button and the Map entries are corrected there: A on the focused "Sam"
entry is "Manage Cargo").

Update (run `20261117-153804-two-games-full-gear`, guest `07*`): hidden-test input does reach the cuff
menus: START opens Map/Information (`07-guest-menu`, behind a ranked-event notice), A on the guest's own
map entry offers "Manage Cargo" (`07d`), and A again opens a view-only Cargo Management list of the
private locker (`07e`: "Cargo management actions ... cannot currently be performed"), which has no
character figure. The on-foot cargo menu with Sam's figure (the "Open Cargo Menu" prompt) was not opened.
Opening a menu puts Sam to sleep for the mod: both bodies and all props are removed and spawned again
after it closes (existing behaviour, `[gear] 19 prop(s) removed (Sam asleep)`).

A guest who plays as Fragile still sees Sam in the cargo/inventory menu, carrying the right cargo. This
is left as it is for now, for the reasons below.

What that menu figure is:
- It is a `DSPlayerAKukanEntity` ("subspace" player, RTTI `0x144BCA020`, size 0x7A0). It derives from
  `Entity`, not `ControlledEntity` or `Humanoid` [V RTTI].
- It is spawned from `DSPlayerSubspaceConfig.SubspacePlayerResource`
  (`ds/entities/player/akukan/ds_player_akukan_entity`) [V census].
- Its 10 components include `DSBaggage2CarrierComponent`, which is why it shows the real cargo. They also
  include Sam's skeleton-only SkinnedModel and the menu Morpheme network `sam/references/akukan` on Sam's
  77-joint skeleton [V run-time census].
- Shared baggage code accepts either `DSPlayerEntity` or `DSPlayerAKukanEntity` as the owner
  (`0x142922CC0`, `0x14291156A`) [V decompile].

Why it is not replaced yet:
- The only workable route would be a "self view in the menu": hide the menu Sam's mesh, then place and
  pose the chosen character's cutscene body on it through the SkinnedModel animate hook (same skeleton).
- Every step of that is unknown and happens inside a paused menu:
  - which world the subspace entity lives in;
  - how to hide its mesh: bit 63 is the `DSPlayerEntity` mesh controller at `+0xA60`, which this
    0x7A0-byte entity lacks;
  - whether the mod's game-thread tick runs while the menu pauses the game;
  - whether the menu's own camera and lighting render a spawned body at all.
- The menu also raycasts and highlights on Sam's attach points (`Subspace_Head`, ...), and DS's crash
  reporter fires even on caught faults.
- The census was taken with the menu closed (0 live AKukan entities). A first probe would be
  `action anim_census` with the cargo menu open, and no input path to open it is known in the hidden test
  environment yet: BACK opened nothing (`20261117-140054-collision-climb`, `11-back`).
## 9. Outfit: the partner Sam wears the other player's own suit

**Status (2026-11-17): works in hidden runs (solo and two games on one PC).** A partner shown as Sam
(`demo_sam_clean`, id 1) used to wear that cutscene body's own dark porter suit, whatever the other player
wore. Now each game sends its own Sam's outfit in every STJ1 frame (8 bytes, section 4), and the receiver
dresses the Sam body the same way with the game's own functions.
- It works in both directions, for the host and for a guest who also picked Sam.
- Any other character is not dressed.
- Code: `native/mod/partner_outfit.{hpp,cpp}`, called from `real_body.cpp` (`DressPartner`, game
  thread) and `player_sync.cpp` (send and receive).

### 9.1 What makes Sam's look, and where it lives [V: decompiles, byte checks, run logs]

- **Exos variation (0..43).** It is one of the porter suit's 44 `ArtPartsVariations`
  (`apv_sam_exos_def_vXY`). It swaps texture sets: the silver suit (X1..3), the gold suit (X4..6), and the
  power-glove and exoskeleton tints.
  - The game computes the index in the inventory update (`0x1427F2ED3`):
    - the base is 14 for suit colour 15 (silver), 29 for 16 (gold), else -1;
    - +5 for the silver power glove, +10 for the gold one;
    - +1..+4 for the kind of the first skeleton;
    - -1 removes the variation.
  - It is applied with `0x142686810` on the mesh controller (`player+0xA60`). That function keeps the
    index at controller `+0x3A50` = **player `+0x44B0`**.
  - It builds the component from the current body variant (`0x1427A2EB0`: streaming control `+0x78` ->
    `DSPlayerBodyVariant`, `+0x48` count, `+0x50` entries).
- **Suit colour (0..14).** It is a shader variable (hash `0x2967E719`) on every model part, stored at
  **inventory `+0x4BE1`**.
  - `+0x4BDC..+0x4BE0` hold the glasses lens and frame colours and the cap colour.
  - `0x1427E70C0` restores all six bytes from the game state at `[0x147BE7980]+0x5608`.
- **Where the components are.** Inventory = player `+0x54C8`, streaming control = player `+0x54F8`.
  Player init stores them at `0x14271FC22` and `0x14271FC90`. Each has `+0x48` = the player.
- **Body variant** = streaming control `+0x74` (0 = porter suit). The exos index belongs to the current
  variant's own variation list, so it only means something for the porter suit: the sender sends exos
  0xFF (none) when its loaded variant is not the porter suit (checked by GUID; by index 0 while the
  variant is not loaded), and the receiver uses the exos only when `variant` is 0 (`wire::PorterExos`,
  tested in `st_net_tests`). The variant itself is not shown (9.4).
- **The 100% save's Sam:** variant 0, **exos 31** (gold suit with an exoskeleton tint), **suit colour 16**
  (gold), cap 24, glasses 4/3 and 4/23 (run `20261117-074943-outfit-probe`).

### 9.2 Dressing the body (`outfit::Apply`)

It runs on the game thread, one step per 250 ms, starting 0.5 s after the body is live.

1. **Remove the body's own variation.** `demo_sam_clean` carries one variation that the player's suit
   does not: `apv_sam_body_suiti_v01` (`ad5fea32-5260-2649-8305-825ba6253234`), a darker suit texture
   set.
   - It is found among the body's components: component `+0x30` is an `ArtPartsVariationResource` with
     that GUID, and `+0x48` is the body.
   - It is removed with the game's component removal `0x14235B910`.
2. **Add the exos variation.**
   - Only the porter suit's index counts (above), and a change of the wanted index must hold for 0.5 s
     before the body changes: a sender's first frames read exos 0xFF for about 150 ms (run `075954`),
     which would otherwise strip the variation for a moment.
   - The resource comes from **this game's own player's** porter-suit variant (GUID
     `80279532-dbc2-d745-a04b-14d90ec09fe3`, 44 entries), entry `exos`. The data are the same in both
     games.
   - It is added with the game's `ApplyArtPartsVariation` `0x1422BC100(entity, resource)`.
   - A change removes the previous variation first.
   - The component holds a reference on the resource (the refcount went up by one at every add: 4 -> 5
     for the save's own exos, 3 -> 4 for entries our Sam does not wear), so the resource stays loaded
     while the body wears it [V: refs; I: that this is what keeps it alive].
3. **Set the suit colour** (0..14 only).
   - It uses the game's "apply suit colour" function `0x14284CDA0(entity, colour)`. The uniform menu's
     preview calls it at `0x14272EAC9`.
   - First it checks that `+0x180` is the body's own `ArtPartsData`.
   - Colours 15 and 16 leave the variable as it is, exactly as the game does on the player. So under a
     silver or gold suit the body keeps the last colour it was given (for example 9), while the player's
     own Sam keeps its own last one; under the exos textures both looked the same in the one case checked
     (host 9 -> silver, run `075502`) [V for that case].
   - Every variation change (step 1 or 2) forgets the colour that was set, so it is set again after it: a
     variation change may rebuild the materials [I], and without this the sequence colour 9 -> gold ->
     colour 9 left the body without its colour call (the last step saw 9 already set).

**Byte checks** (`outfit::Available`, once, logged as `[outfit] outfit matching available ...`):
- the two init stores;
- the `+0x4BDC` restore and `0x1427E7850` (`+0x4BE1`);
- `0x142686810` (`+0x3A50`) and its call site;
- `0x1427A2EB0` (`+0x78`);
- the three functions we call and the preview call site;
- the colour hash at `0x144BE7BA4` and the limit at `0x143D60906` (15).

**Failure handling:**
- Any byte mismatch: no outfit is sent or applied.
- A fault in a game call turns the feature off for the session.
- **Accepted risk:** component removal (it frees a component) and `ApplyArtPartsVariation` are called
  from the tick (the game thread's Steam-callback pump) while the game updates the dressed body's model
  on its job threads at 60 Hz. If such a job overlapped the pump, it would race the body's component
  list. This is the same risk as the spawn and removal calls the real body already makes from the tick,
  and the SEH wrapper is no real net (the crash reporter fires on caught faults too). No fault in any run
  [I].
- When this game's own player does not wear the porter suit, the exos step is skipped (logged, retried
  every 2 s). The colour still applies.

Log lines:

```
[outfit] the partner's outfit: variant 0 exos 31 suit colour 16 cap 24 glasses A lens 4 frame 3, B lens 4 frame 23
[outfit] body ...: removed its own suit variation apv_sam_body_suiti_v01 (component ...): the porter suit's base texture
[outfit] body ...: exos variation 31 (resource ... guid 8b711172-..., from our porter suit ...) added: component ... applied 1; resource refs 4 -> 5
[outfit] body ...: exos variation 31 removed
[outfit] body ...: suit colour 9 set (#1; was 255)
```

### 9.3 Evidence

All runs are hidden, with `-SeedSaves complete-100`. Each exited 0, with no error.txt, the real saves
unchanged and 0 uploads.

| Run (`build-testenv\runs\`) | What it shows |
|---|---|
| `20261117-074943-outfit-probe` and `20261117-075230-outfit-probe` (solo; a debug Sam body 3 m ahead, in mirror mode; code before the final colour rule: there silver and gold still set the colour variable) | `01-undressed-front`: the body in the dark blue-grey porter suit with yellow straps, next to our Sam in his gold suit. `09-dressed-again` (`real_outfit 1`, dressed like our Sam): the body in the same metallic gold suit. `04`/`05` (no exos, colour 0 / 9): blue-grey / olive. `07` (exos 14): a silver suit. `08` (exos 29): gold. Run `075230`, `10` and `11`: our own Sam's colour changed to 0, then 9 (`real_outfit 4`, test only). Sam himself turns blue-grey, then olive, and the body dressed like him turns the same colour. |
| `20261117-075502-two-games-outfit` (an earlier build; host and guest both Sam, graphics low) | `02-guest-sees-host-gold` and `02-host-sees-guest-gold`: each game sees the other's Sam body in the gold suit, like its own Sam. Host colour 9: `03-host-own-colour-9` (the host's own Sam is olive) and `03-guest-sees-host-colour-9` (the host's body is olive). Guest colour 0: `04-guest-own-colour-0` (the guest's Sam is blue-grey) and `04-host-sees-guest-colour-0` (the guest's body is blue-grey). Host colour 15: `05-host-own-silver` (the host's Sam is silver) and `05-guest-sees-host-silver` (the host's body is silver). |
| `20261117-082452-two-games-outfit` (**the final code**; host and guest both Sam, graphics low; looked at in enlarged crops) | `02`: each game sees the other's Sam body in the metallic gold suit, as its own Sam. Host to colour 9: the host's own Sam (`03-host-own-colour-9`) and its body on the guest's screen (`03-guest-sees-host-colour-9`, from behind) both turn a matte khaki / olive-yellow, clearly not the shiny gold. Guest to colour 0: the guest's own Sam and its body on the host's screen are dark blue-grey (`04`). Host to silver: both silver (`05`). Host back to colour 9 after silver: both khaki / olive again (`05b-host-own-colour-9-again`, `05b-guest-sees-host-colour-9-again`); the guest log shows the colour set again after the variation change (`suit colour 9 set (#3`, `#4`), which the code before the fix skipped. The first STJ1 frames carried exos 255 for about 200 ms; the 0.5 s settle kept the body from changing on them. Both exit 0, no error.txt, real saves unchanged, 0 uploads. (The scenario's `#2` check matched an earlier line in this run; its regex now asks for `#3` or later.) |
| `20261117-075954-two-games-real-self` (an earlier build; the guest picked Fragile, self view on) | The user's case: `guest/automation/02-guest-sees-self` shows the guest's own Fragile in front and the host's Sam body in the gold suit (gold legs and arms, black back panel), as the host's own Sam is in `02-host-control`. The host's Fragile partner gets no outfit (no `[outfit] body` line in the host log). |

### 9.4 Not matched

- **The body variant.** A player in the MULE suit, the private-room clothes or any other non-porter
  variant is still shown in the porter suit (plain: no exos variation, only the colour). The variant byte
  is sent, and `demo_sam_body_suitmule_demo_new` exists as id 15, but the body is not respawned by
  variant. Not run in a game: the 100% save's Sam wears the porter suit.
- **Boots.** The player's boots are a separate model (gold in the 100% save); the body keeps its own.
- **Cap and glasses.** They are separate accessory entities; their colours come with section 13's looks
  (the STJ1 bytes are still not used).
- **Exoskeleton and power-glove meshes.** Only their texture tints come with the exos variation.
- **Backpack and cargo:** section 12 (props, not the player's own backpack variation).
- **A receiver not in the porter suit.** It cannot take the exos resource, so the remote player's
  silver or gold suit and tints are not shown. The colour still is.
- **The uniform menu.** The runs changed the outfit through the inventory byte (`real_outfit 4`, hidden
  tests only), not through the game's uniform menu. The game re-dressed its own Sam from that byte at
  once (run `075230`), so the menu path uses the same data [I].
- **Not tested:** two PCs; a variant change by either player while the body is dressed (the exos
  resource belongs to our own porter variant; the +1 reference keeps the object, but whether its
  streamed data stay loaded is unproven); a self-view body that is Sam (not needed: the self view is
  only for other characters).


## 10. Name tags: the partner's name above their head

Each game draws the other player's name above the partner's body (any body: real, hologram, marker,
or the invisible echo). Each game sends its own name over the link. On by default:
`session.ini name_tags=0` turns both off. The launcher's Debug page has **Show player names above
heads**, which writes `name_tags=0` only when it is unticked.

Code:
- `native/mod/pose_wire.{hpp,cpp}`: the STN1 frame and `SanitizeName`.
- `native/mod/link.cpp`: sending (`SetLocalName`) and receiving (`PeerName`).
- `native/mod/nametags.{hpp,cpp}`: game side. It picks the name, the anchor and the gates, finds the
  camera and holds its byte checks.
- `native/mod/nametag_math.hpp`: pure code. Projection, letterbox, size and fade, the label.
- `native/mod/overlay/overlay_tags.{hpp,cpp}`: the overlay. It reads the camera at Present, projects
  and draws.
- Unit tests in `st_net_tests` (`TestNameFrames`, `TestNameTagMath`).

### 10.1 The camera [V: byte checks at start, runs below]

| What | Where |
|---|---|
| Local player | `Player* = [Sam + 0x560]` (`DSPlayerEntity::GetPlayer` 0x1427115B0) |
| Camera view object | `[Player + 0xD0]`, vtable `0x143F15208` (checked every read) |
| View snapshot | view + 0x28 .. + 0x84. It holds the position (3 doubles), the axes (+0x40: right, forward, up; Z up), near +0x68, far +0x6C, top +0x70 (near * tan(fov_v / 2)), right +0x7C (top * aspect), projection mode +0x80 (0 = perspective) |
| Who writes it | The view's per-frame update, vtable slot 2 `0x14361AA50`. It calls `Camera::GetViewFrustum` (`0x141E05BA0`, FOV-aspect adjust included) and copies the result at `0x14361AAC6` |

Byte-checked at the first tick with a player:
- `GetPlayer`;
- the `Player + 0xD0` getter `0x1423C6140`;
- `GetCameraPosition`'s use of it (`0x1426E8FC2`);
- the 160-byte frustum copy at `0x14361AAC6`;
- vtable slot 2.

Any difference turns the tags off and logs which check failed.

The overlay reads the snapshot inside `Present` with `ReadProcessMemory`:
- It reads the vtable first.
- It then reads the 0x5C bytes twice. Both reads must match; it tries 3 times, because the game
  writes the snapshot non-atomically.

Runtime numbers, run `093026` [V]:
- `a0 (0.825, -0.566, 0)`, `a1 (0.560, 0.817, -0.139)`, `a2 (0.079, 0.115, 0.990)`.
- `dot(cross(a0, a1), a2) = 1.000`: right-handed, Z up.
- near 0.2, far 4000, fov_v 45.7 deg, aspect 1.7778.

Projection: `d = p - cam`, then `x = d.a0`, `z = d.a1`, `y = d.a2`. A point with `z <= near` is
hidden. Otherwise `ndc = (x/z * near/right, y/z * near/top)`. If the view's aspect differs from the
back buffer's by more than 1 %, the point is fitted into a centred letter- or pillarbox; this case is
unit-tested but not seen in a run.

### 10.2 What is drawn and when

**Anchor.** The pointer of the label sits 0.36 m above the body's head joint (joint 31 of the
77-joint layout, through the body's joint map; `realbody::HeadWorld`). For a standing Sam that is
1.95 m over his feet, as before; it now follows leans and crouches, and sits closer over shorter
bodies such as Fragile. A head that does not read, or is not 0.3-2.4 m over the feet and within 1 m
beside them, falls back to the root + 1.95 m: the hologram and the marker always use that. The log
says which once per change (`[tags] anchor: over the head joint`). Run `092833` used the root + 2.05 m,
and the label floated about 0.3 m over Sam's head; at 1-2 m from the camera the root anchor put the
label well off a leaning body's head (run `101839`, guest `03-guest-walks-in-d`).

**Text.**
- The partner's name.
- When it equals our own name, which happens with two games on one Steam account: ` (host)` or
  ` (guest)`, the peer's role.
- When no name arrived (the peer has `name_tags=0`): `Host` or `Guest`.
- In echo mode: `<own name> (echo)`.

**Distance.**
- Text size is 26 px at 5 m or less, falling to 15 px at 60 m or more. Both values are for 1080p and
  are scaled with the overlay.
- At 25 m and beyond, the distance follows the name (`TagEcho (echo) - 42 m`, with a middle dot).
- From 150 m the label fades out; at 300 m it is gone.
- Closer than 0.8 m it is hidden.

**Not drawn** (game thread, logged as `[tags] hidden: <reason>` / `[tags] shown`):
- DSGameState flags `& 0x184F407FEC7F7F`, the game's own `IsStoppingGameEvent` mask. It covers
  PauseMenu, HandcuffDevice (the cuff map), Cutscene, Movie, InGameCutscene, Loading, SystemPause,
  FastTravel, KnotSpace, and more (`game::ReadStopGates`).
- Any GameModule load bit (+0x44C).
- Sam asleep.
- No partner body, or the partner silent for 10 s.
- The partner's real body hidden by the game (entity flag 0x2 clear): no tag over nothing.
- No camera view.
- A frame the game thread has not refreshed for 500 ms.

**Drawn but not shown** (overlay, logged as `[tags] not drawn: ...` on each change):
- Behind the camera or on its near plane.
- Outside the view.
- Beyond 300 m.

**Style.** A dark pill (rounded box) with a pointer, drawn on ImGui's background draw list, under the
debug menu. There is no depth test, so the tag shows through walls. When the anchor is near a screen
edge, the box is pushed inside the screen and the pointer still marks the anchor.

**The overlay renderer.** It now also lives while a tag is visible during gameplay. Two changes make
that safe:
- It is released before the game creates a swapchain (the `CreateSwapChain(ForHwnd)` hooks), because
  DXGI allows one flip-model swapchain per window. Those hooks are optional for the overlay; without
  them tags are not drawn at all (logged once: `[overlay] name tags not drawn: the swapchain creation
  hooks are unavailable`), so a kept renderer can never block the game's swapchain re-creation.
- It is released, as before, once nothing is drawn.

With `debug_menu=0` and `name_tags=1`, the overlay is installed for the tags only:
- no menu, toggle key or hint;
- the game window is not subclassed.

Installation still waits for the game's renderer (`WaitForGameD3D12`).

### 10.3 Wire: STN1

`'STN1' | u8 version 1 | u8 flags (bit 0: sender is the host) | u8 len (0..64) | u8 0 | UTF-8 name[len]`
(8 + len bytes).

**Placement.** One more FRAME message in a pose datagram:
- the first 10 datagrams of each link session;
- then every 30th (about 1 Hz at `send_hz=30`);
- the next one after a change.

Unknown flag bits are cleared. Another version or a wrong size is rejected. `DecodePose` and
`DecodeJoints` reject its magic, and `DecodeName` rejects theirs (tested). Mixed builds never get
this far (HELLO `build_mismatch`, section 4).

**Sanitizing.** Both sender and receiver run `SanitizeName`, which:
- drops invalid UTF-8, C0/C1 controls, DEL and bidi/zero-width format characters;
- collapses and trims white space;
- caps the name at 32 code points and 64 bytes, cut at a code-point boundary.

The receiver keeps the newest name per link session and clears it on a new session.

**Our name.** `session.ini name=` (the launcher's Advanced "your name in co-op") if set, otherwise the
Steam persona name. Names are never logged in full: the log shows `[tags] our name: 7 B fnv ...` and
`[tags] partner name: 9 B fnv ... (session #1, sent by the host)`. `[steam] ready` now also logs only
the persona's length and hash. With `name_tags=0` nothing is sent.

### 10.4 Evidence (hidden, `-SeedSaves complete-100`, all exit 0, real saves unchanged, 0 uploads)

| Run (`build-testenv\runs\`) | What it shows |
|---|---|
| `20261117-093026-name-tags` (`name-tags`, echo with the real Sam body 6 s behind, `name=TagEcho`) | `02-near-standing`: "TagEcho (echo)" just above the head of the partner, who stands where Sam stands. `03-self-test` (`action name_tags 1`): "TagEcho (test)" above our own Sam's head. `07-pause-menu`: the START button opened the cuff Map/Information screen (HandcuffDevice), and no tag is drawn; log `[tags] hidden: game state HandcuffDevice`. |
| `20261117-093249-name-tags` | `04-far-b`: the partner Sam body runs towards the camera with the tag right above its head (logged 11.3 m 1.3 s before the frame, closer by then; our own Sam is out of frame, the camera pinned against a wall). `07-partner-behind-camera`: Sam walks away and the partner stands behind the camera: no tag (log `not drawn`). |
| `20261117-093544-name-tags` | `09-far-sweep-04` (echo offset 40 m, camera turning at half stick): a small figure in front of the racks with "TagEcho (echo) - 42 m" (middle dot) directly above it, while the camera rotates. `09-far-sweep-08` onwards: out of view, no tag. Map: no tag. `12-esc-menu`: Esc posted to the window opened no menu, so the tag is still shown (the system pause menu was not reached). |
| `20261117-094013-two-games-name-tags` (both `name=TagPlayer`, guest Fragile) | Guest `03-guest-sees-host-walk-b`: the host's Sam body walking away with "TagPlayer (host)" above it. Host `06-host-sees-guest-walk-b`: the guest's Fragile running, with "TagPlayer (guest)" above her. Each log: `[tags] partner name: 9 B fnv 88533042`. Marked failed only by an optional `?waitlog`, since removed (the scenario's failure count includes optional steps). |
| `20261117-114454-two-games-all` (**the final code**: head anchor, all three features on; host `name=TagPlayer`, guest Fragile `name=TagFriend`) | **pass**, both exit 0. Different names, so no suffix: each log `[tags] label: 9 B` and `[tags] anchor: over the head joint`. Guest `02-guest-sees-host-and-self`: "TagPlayer" right over the host's Sam body about 10 m ahead. Host `03-host-sees-guest-front`: "TagFriend" right over the guest's Fragile. In contact (0.8 m): guest `04-guest-walks-in-c`, "TagPlayer" just above the host body's head (compare run `101839` below, where the root anchor put it far off); host `04-host-sees-guest-c`, "TagFriend" over Fragile behind the host's own Sam. Host `04-host-sees-guest-e`: the guest has passed behind the camera, no tag. |
| `20261117-094807-two-games-name-tags` (the root anchor, with the box clamped into the screen) | **pass**, both exit 0. Guest `02-guest-sees-host`: "TagPlayer (host)" above the host's Sam body standing about 9.5 m ahead. Host `07-host-sees-guest-stopped`: the guest's Fragile at the left edge, half behind the host's own Sam, and her tag "TagPlayer (guest)" pushed fully into the screen. In run `094513` the same frame cut the box at the edge, which is why the clamp was added. Each log: `[tags] partner name: 9 B fnv 88533042`. Payload: 574 B per datagram at 30 Hz, about 0.7 B of it STN1 on average. |

Screenshots include the overlay: the capture runs after the overlay's draw, on the same queue.

**Camera timing.** In the sweep frames the label stays over the figure while the camera turns at half
stick [V, by eye]. A one-frame lead or lag at full speed was not measured.

### 10.5 Not covered

- **The system pause menu.** Its flag (SystemPause, bit 28) is in the mask, and the tags were hidden
  under `Loading|SystemPause` while loading. But no run opened that menu: Esc posted to the window did
  nothing, and BACK opened nothing visible.
- **Photo mode.** Its flag is unknown.
- **The weapon selector.** It is not in the mask, so tags stay up.
- **Fonts.** Tags use the game's menu typeface (section 10.6), with Segoe UI merged in for the glyphs it
  lacks. Names in CJK, Hangul, Arabic or Thai, which neither covers, render as boxes. The game's own
  per-language SST fonts are not merged yet.
- **HDR back buffers.** Colours are written as raw UNORM, the known overlay limitation.
- **The anchor on other bodies.** 0.36 m over the head joint fits Sam; the gap over a head of
  another shape (hair, hats) was not measured per character. The hologram and the marker keep the
  root + 1.95 m.
- **Not run in a game:**
  - `debug_menu=0` with tags only;
  - a game swapchain rebuild while a tag is shown (the release-before-create path);
  - a letterboxed view;
  - two PCs over Steam.

### 10.6 The game's menu font

**Status (2026-11-17): works in hidden runs, solo and between two games.** Name tags are drawn in the
typeface of the game's menu headers: the "Map/Information" title when the map opens. Turn it off with
`session.ini name_tag_font=default`, which brings back Segoe UI.

**Which font** [V: game data, decoded offline with `tools/re/ds_archive.py`]:
- The header is `MenuStyleText` class `common_titling`. Its family resolves, for every language, to
  `interface/fonts/ds/expsmediumneon`: a `FontResource` with GUID `94f869a0-db23-3d46-a69a-c92036327756`,
  typeface "EX PS Medium Neon".
- It covers 429 code points: Latin with Latin-1 and Extended-A, part of Greek, and Cyrillic. Lower case
  is drawn as small capitals. It has no U+00B7, which the tag uses in " · 42 m".

**It is not a TTF.** It is a Decima `FontResource` made of vector outlines:
- metrics (height 1000, ascent 800, descent 200);
- per glyph: advance, code point and contours;
- per contour: command bytes and points. A command byte splits into `type = b >> 6` and `k = b & 63`.
  Type 0 means k line-to points. Type 1 is a TrueType-style quadratic B-spline: k - 1 off-curve controls
  with implied midpoints, then the end point.
- Points always number 1 + Σk.
- Kerning pairs are present but not used: ImGui's loader interface has no kerning.

**Reading it** (`native/mod/game_font.cpp`; read-only, memory only, never written, shipped or logged):
- **Where.** The path hash is murmur3 of `interface/fonts/ds/expsmediumneon.core`. The reader looks it up
  in `data\7017f9bb9d52fc1c4433599203cc51b1.bin` first, then in every archive. It decrypts the table rows
  and chunks (murmur3 and SALT masks, MD5 via BCrypt) and decompresses with `OodleLZ_Decompress`.
- **Oodle.** ds.exe links Oodle statically and never loads the `oo2core_7_win64.dll` that sits in its
  folder [V: no DLL name in ds.exe; the first run logged "not loaded"]. The reader loads that file by full
  path from the game folder for the read, then frees it.
- **Parsing.** `ParseFontCore` checks every count, that every contour uses exactly its points, the
  object's GUID, and that the parse ends exactly at the object's size. Any mismatch falls back to Segoe UI
  with one log line. `st_net_tests` covers it with a synthetic font (no game data).
- **When.** A worker thread at below-normal priority reads the font the first time a tag is wanted. The
  overlay's render thread is then up, so D3D12 is never touched early. It took 0 to 16 ms in the runs
  below. The raw core bytes are zeroed after the parse; only the parsed outlines stay in memory.

**Drawing it** (`native/mod/overlay/overlay_font.cpp`, `native/mod/game_font_raster.cpp`):
- The font is a custom `ImFontLoader` in the overlay's ImGui atlas. It is added between frames, outside
  NewFrame..Render. The atlas is dynamic (`RendererHasTextures`), so glyphs are baked on demand.
- Each glyph's contours are rasterised with a private static copy of the repo's `imstb_truetype.h`
  (`stbtt_Rasterize`, nonzero fill, ImGui's oversampling).
- Segoe UI is merged into the same `ImFont` for missing glyphs: " · ", and anything else the game font
  lacks.
- The size is scaled ×0.87, so its capitals stand 10% taller than Segoe UI's at the same tag size.
- The menu keeps Segoe UI.

**Evidence** (hidden, `-SeedSaves complete-100`, pass, exit 0, no error.txt, real saves unchanged,
0 uploads):

| Run (`build-testenv\runs\`) | What it shows |
|---|---|
| `20261117-140054-collision-climb` | First build: `the game's menu font is unavailable (the game's oo2core DLL ... is not loaded)`. Tags fell back to Segoe UI. This led to the fix above. |
| `20261117-141254-collision-height` | `name tags use the game's menu font 'EX PS Medium Neon' (429 glyphs, 1090 contours, read ... in 15 ms ...)`. `09-tag-self`: "TagEcho (test)" in the game's typeface. `10-map-header`: the game's own "Map/Information". |
| `20261117-142256-collision-climb` (final build) | Every tag drawn in the game font (`07-tag-near`, `09-tag-self`). `10-map-header` for comparison. |
| `20261117-142530-two-games-collision` (final build, two games, 1280x720) | Both games log the game font. `05-host-tag-far` / `05-guest-tag-far`: "TagPlayer (guest) · 34 m" and "TagPlayer (host) · 33 m": the " · " comes from Segoe UI, everything else from the game font. |

Not covered:
- CJK, Hangul and Arabic names: the game's SST fonts could be merged the same way.
- Kerning.
- A game build whose font differs: the GUID check falls back to Segoe UI.

## 11. Player collision: you cannot walk through your partner

**Status (2026-11-17): works in hidden runs, solo (echo) and between two games.** The partner's real
body blocks the local Sam. He stops about 0.81-0.85 m from its centre (two 0.40 m capsules). If he keeps
pushing, he slides round it, as he does round any other character.

- On by default. `session.ini partner_collision=0` turns it off.
- The launcher's Debug page has **Players block each other**. It needs Real character, and it writes
  `partner_collision=0` only when unticked.
- **The partner cannot be climbed** (section 11.5). Its capsule is 4.0 m tall, which Sam cannot climb or
  jump-grab. The game offered Climb on the 1.65 m capsule of v0.7.
- `partner_collision_layer=9` picks the game's "AI or Remote Player" collision layer instead of the
  default 10. `partner_collision_height=<m>` (0 = Sam's own) sets the capsule's height. Both are test
  settings, not in the launcher.

Code:
- `native/mod/partner_collision.{hpp,cpp}`: `player_sync` calls it every tick, after the partner was
  moved.
- `native/mod/game_actions.cpp`: the gates on the local player (`LocalCollisionBlock`).

### 11.1 How it works [V: decompiles and byte checks; runs below]

DS uses Havok Physics (hknp). A character's collision is a `CapsuleComponent`. Its mover is one of two
kinds:
- a character proxy (type 1), for the locally controlled player;
- a character rigid body (type 0), which the game gives remote and AI humanoids.

`Humanoid::Init` (`0x14373A780`) gives Sam his capsule at run time. The partner's body is a
cutscene-actor `DSControlledEntity` with no physics of its own, and the mod repeats that sequence on it:

```
NewGuid 0x141920CF0(&g)
comp = CreateComponent 0x1422B4FC0(CapsuleComponent RTTI 0x144B39F70, [0x144B386D0], nullptr, &g)
[comp+0x70] = 0x143744220             the contact filter Humanoid::Init stores (the capsule's listener
                                       at +0x50 jumps to it unconditionally: slot 1, 0x1424417B0)
AddComponent 0x142348220(body, comp)   (component list +0x90, init messages; the body's flags have
                                        0x10000000 "initialized" and 0x4000 "in world")
Capsule::Configure 0x142446340(comp, {a copy of Sam's PhysicsCharacterMoverResource (his capsule +0x68)
                                      with StandingHeight 4.0 m (section 11.5),
                                      type 0, layer 10, group 3, 0x1F, 60 degrees})
```

- **Size.** The capsule uses Sam's radius, 0.40 m, and a standing height of 4.0 m (section 11.5; Sam's own is
  1.65 m) [V: logged]. Every character gets this capsule, because no per-body resource exists.
- **Layer 10, "Humanoid blocker".** The layer matrix (built by `0x1422AC810`) lets it meet only the
  Player (8) and AI/remote (9) layers and vehicles. It does not meet cargo, bullets, the camera or static
  geometry. Logged at run time: `(8,9)=1 (8,10)=1 (9,10)=1 (8,8)=0`. Layer 9 would also push cargo and be
  hit by bullets [I].
- **Group 3.** This is what `DSControlledEntityResource` vt+0x98 returns (`0x1434F6CC0`: base 1 | 2)
  [V bytes]. Sam's group is 7 [V logged].
- **Every tick**, the capsule is put at the body's world transform and its velocity is zeroed
  (`Capsule::SetTransform 0x142446980`, `SetVelocity 0x142446910`). This is HumanoidMover's own reset
  path (`0x143608C30`). The rigid body does not collide with static geometry, so without this it would
  fall.
- **Off and on** use `Capsule::Disable 0x14243A740` and `Enable 0x14243A9E0`, the capsule's disable
  counter. Ours is always balanced. Before Disable, the capsule is placed at the body, so when the
  removal writes the capsule's position back into the entity, nothing changes.
- **Lifetime.** The component belongs to the body.
  - When the body is removed (any of the real body's teardown triggers, section 3.4), the capsule goes
    with it (MsgPrepareForRemoval, `0x1424419F0`). Just before, while the body is still live, the
    teardown takes the capsule out of the world (`collision::BodyGoing`, GUID and component checks
    first), so a teardown that cannot remove the entity (a spawner reference leaked, a faulted
    remove) leaves no invisible wall. Afterwards the mod only forgets its pointer and never calls
    into the old capsule (it may be freed).
  - A body that reads as unreadable for a tick keeps its capsule record (taken out of the world if it
    still checks out), so a later good read never attaches a second capsule. If `Configure` faults
    after creating a mover, that capsule is taken out of the world too.
  - The emergency stop (the per-frame sync faulted) takes the capsule out of the world first, in its
    own fault guard, before the body and gear stops.
  - Before every call, fault-free reads check that the capsule is still in the body's component list,
    with its vtable and owner, and that the body is the same (pointer and GUID).
- **Byte checks** run once, before the first call. A mismatch turns collision off with a log line. They
  cover:
  - NewGuid, CreateComponent, the AddComponent thunk and body, Configure, SetTransform, SetVelocity,
    Disable, Enable, and the add/remove-world function;
  - the contact filter, the listener vtable slot 1 and its thunk, and the CapsuleComponent RTTI name;
  - Humanoid::Init's own call sequence: each call target, and the RTTI and name operands.

  After Configure, the mover must have the rigid-body vtable `0x143BDBD00`.

**Never on** the hologram (the fallback), the marker or echo bodies, or the self view's body:
`player_sync` passes only the partner slot's live real body. A debug body (`real_spawn`) gets none
either.

### 11.2 When it is off

The capsule is taken out of the world (Disable) while any of these holds. It is put back (Enable) when
they clear.
- `partner_collision=0` or `action collision 1 0`, or a failed byte check or attach.
- The local Sam is not on foot in the played world (`LocalCollisionBlock`):
  - no player;
  - Sam asleep (loads, cutscenes, knot space);
  - the game loading or suspended;
  - a stopping game event (menus, cutscenes, the map, and so on: mask `0x184F407FEC7F7F`);
  - the post-load setup phase;
  - Sam attached to a parent entity (vehicles, mounts);
  - a ground snap pending;
  - Sam bound to a sequence (scripted scenes). This is checked every 250 ms and is the one game call.
- Sam's own capsule is out of the world or disabled (ladders, ziplines and other moves that take it out
  [I: not run]).
- The partner body is hidden or asleep, or no partner position came for 0.5 s.
- **The partner is not near.** The capsule is in the world only while the partner is within 4 m of
  the local Sam horizontally (and 3 m vertically); it leaves again beyond 4.5 m. The capsule stands
  where the partner is in the *other* game's world, which can be inside our own parked trike or next
  to our own MULEs: away from Sam it could only push those, never block Sam (review finding).
- **The partner rides something.** The sender sets the pose flag `kPoseInVehicle` (bit 1 of the
  pose flags, reserved until now) while its Sam has a parent entity (vehicle, mount); the receiver
  takes the capsule out of the world while the newest pose has it. A peer that never sets it (none
  exists: mixed builds are refused) only has the speed gate below.
- The partner moves faster than 8 m/s (a vehicle), or jumped more than 2 m in one tick (a snap or a
  teleport). After a jump it stays off for 0.5 s.
- **Sam above the partner.** Sam's feet more than 1.6 m above the partner's origin (Sam on a ledge, rock
  or vehicle above it): the 4 m capsule (11.5) would reach him there, an invisible pillar or a push that
  the v0.7 1.65 m capsule never caused (review finding). `[coll] off: Sam is above the partner (over 1.6 m)`.
- **Overlap.** The two are closer than 0.55 m horizontally and within 1.8 m vertically. Collision goes
  off and comes back only once they are 0.9 m apart. The capsule is never put into the world on top of
  Sam, so Havok never pushes them apart (no launch).

Every change is logged, for example `[coll] on: the partner blocks Sam (partner 3.00 m away)` or
`[coll] off: <why> (...)`. While a partner is within 5 m, a `[coll] 1 s: min distance ...` line is
logged once a second.

### 11.3 Evidence (hidden, `-SeedSaves complete-100`, all pass, exit 0, no error.txt, real saves unchanged, 0 uploads)

Scenarios:
- `collision`: the echo partner is pinned ahead with `action collision 3`. Sam walks into it at full and
  half stick, round it, then on.
- `collision-off`: the same with `partner_collision=0`.
- `two-games-collision`: the guest walks the host's path into the host's standing Sam body.

Distances are the horizontal distance from Sam to the body, every tick (`action collision 2`).

| Run (`build-testenv\runs\`) | What it shows |
|---|---|
| `20261117-114731-collision` (**the final code**, pass) | Pinned 7 m ahead: `off: the partner is not near Sam (over 4 m)`; pinned 3 m: on. **Full stick:** contact 0.6 s at **0.847-0.963 m** (71 ticks), then Sam slid round it. `03-walk-in-b`: Sam face to face with the partner Sam, "TagEcho (echo)" over its head. **Half stick:** 1.3 s at **0.850-0.966 m** (158 ticks, at most 0.025 m change per tick). **Round it:** no contact. **Walking afterwards:** 3 s moved Sam 8.79 m. **The echo walking into Sam:** 0.96 to 0.56 m, Sam not moved, collision off (overlap). `05-slow-in-b`: the Climb prompt again while pressed against the partner's back; the A press (59.5 s) came when Sam had slid 1.4 m off, his height then rose 0.7 m and fell back 2-3.5 m from the partner (`05-slow-in-climb`: Sam by the pillar, no partner in view), so climbing onto the partner is still untested. |
| `20261117-114454-two-games-all` (**the final code**: the 4 m gate, gear props and name tags on) | Two games. Each capsule attached at 3.9 m (`on: the partner blocks Sam (partner 3.96 m away)`) and left the world again at 4.5 m (`off: the partner is not near Sam`). **Guest:** blocked by the host's Sam body at **0.800 m**, then slid round it (`04-guest-walks-in-c`: Fragile beside the host's Sam body, both with their cases). **Host:** the guest's Fragile came within **0.802 m** of the host's Sam (`04-host-sees-guest-c`); the host's Sam did not move. 73 ticks on each side with the capsule in the world. |
| `20261117-102636-collision` (before the 4 m gate) | **Full stick:** contact for 0.6 s at **0.848-0.949 m** (73 ticks, mean 0.859 m), sliding 1.75 m round the partner. `03-walk-in-b` (0.85 m): Sam face to face with the partner Sam, his arm and cargo reaching across the partner's body (they lie outside the 0.40 m capsule). **Half stick:** 1.2 s at **0.842-0.948 m** (148 ticks, sd 0.022 m, at most 0.025 m change per tick). **Round it:** never closer than 1.90 m. **Walking afterwards:** 3 s back moved Sam 8.64 m. **The echo walking into the standing Sam:** 0.93 to 0.55 m, Sam not moved, collision off (overlap). |
| `20261117-102307-collision-off` (the final build) | Collision off: Sam walks through the partner again. `action collision 0` now logs the self checks with collision off too (the earlier off run showed none: a name clash, fixed). |
| `20261117-102446-collision` (the final build) | Contact at **0.812 m**, then 0.849 m twice. Marked fail only by the scenario's walk-on check (the pad measured 1.08 m while the trace shows Sam walking about 6 m: he was still turning after the previous step; the scenario now waits 1.5 s first). |
| `20261117-101322-collision` | **Full stick:** Sam stopped at **0.827 m**. `03-walk-in-b`: Sam pressed against the partner Sam, face to face. He stayed at 0.83-0.94 m for 0.6 s while sliding 1.7 m round it, then walked on. **Half stick:** **0.837-0.949 m** for 1.2 s (`05-slow-in-a`, `-b`), then round it. **No jitter:** in contact, the distance changed by at most 0.024 m per tick at half stick. **Walking afterwards:** 2.5 s back moved Sam 3.96 m. **The echo walking into Sam:** the unpinned echo then walked into the standing Sam. The distance fell from 0.93 to 0.56 m in 0.13 s, Sam did not move (0.00 m), and collision went off (overlap) and stayed off while the echo stood on him. |
| `20261117-101113-collision` | The same. Full stick: 0.848 m. Half stick: **0.842-0.948 m for 2.2 s** (266 ticks, mean 0.849 m, sd 0.018 m). `05-slow-in-a`: Sam against the partner, with a **Climb** prompt on screen (see 11.4). Its "round the partner" step pushed Sam into a wall instead (the scenario is fixed since). |
| `20261117-100916-collision` | Full stick, head-on: Sam stopped dead at **0.830 m** (one tick moved 0.003 m), then slid round the capsule at 0.83-0.85 m. |
| `20261117-101541-collision-off` | The same steps with collision off: Sam walks straight through the partner, down to **0.02 m** at full stick and 0.14 m at half stick. `03-walk-in-b` (d 0.39 m): Sam inside the partner body. |
| `20261117-101839-two-games-collision` | Two games (graphics low, 30 fps). **Guest:** blocked by the host's Sam body at **0.806 m**, then slid 1.7 m round it in 0.6 s. **Host:** the guest's Fragile came within **0.808 m** of the host's Sam and passed by; the host's Sam did not move (0.00 m). Both logs show `[coll] on` from the attach to the end. |
| `20261117-100436-collision`, `20261117-100709-collision` | Earlier scenario versions. They pinned the partner along the walking direction, which missed it by 0.1-0.2 m: one contact at 0.848 m with a slide round, and one pass at 0.937 m without contact. |

Screenshots (all looked at):
- `20261117-102636-collision\automation\03-walk-in-b.png`: the final build, in contact at 0.85 m.
- `20261117-101322-collision\automation\03-walk-in-b.png`: the closest approach, 0.83 m.
- `20261117-101113-collision\automation\05-slow-in-a.png`: 0.84 m, with the Climb prompt.
- `20261117-101541-collision-off\automation\03-walk-in-b.png`: collision off, Sam overlapping the body.
- `20261117-101839-two-games-collision\guest\automation\03-guest-walks-in-c.png`: the guest walking in,
  1.7 m behind the host's body.

### 11.4 Not covered

- **Sliding round.** This is a capsule against a capsule. Unless Sam hits the partner exactly centred,
  pushing at it makes him slide round it: within 0.6 s at full stick, 1.2-2.2 s at half stick. He would
  do the same round an NPC. Not tried: whether NPCs or BTs block him harder.
- **Climb prompt.** Fixed: see section 11.5.
- **Not run:**
  - vehicles (gated by Sam's parent and by the partner's speed), ladders, ziplines;
  - cutscenes and menus with a partner in contact;
  - Sam carrying cargo by hand;
  - the partner crouching (its capsule is always standing height);
  - other characters' sizes (Fragile gets Sam's 0.40 m capsule);
  - the hologram fallback and the self view (code paths only: they never get a capsule);
  - two PCs over Steam.
- **The partner walking into you.** It is not blocked on your screen. It is blocked on the partner's
  own screen (by your Sam), so with low lag it stops in front of you on both screens. A lagged partner
  that overlaps you just passes through: collision is off until you are 0.9 m apart, and nobody is
  pushed.
- **Physics thread.** The calls run on the game thread, as the spawn does, and the physics setters take
  locks [I]. No fault in any run.
- **Our own vehicles and AI near Sam.** Within 4 m of Sam the capsule can still meet a local vehicle
  or NPC standing where the partner walks in the other world, and push it [I: not run with a parked
  trike]. Layer 10 meets the vehicle layer; no layer that meets only the Player layer was found.

### 11.5 No climbing on the partner [V: hidden runs below]

On v0.7, pressed against the partner, the game offered "Climb", and A put Sam on top of the partner's
1.65 m capsule.

**The collision layer does not decide it.** The research suggested layer 25 "vs Humanoids", because the
layer-matrix replay showed it outside the movement-probe layers 14, 16 and 34 [V: the live matrix logged
`(25,8)=1 (25,9)=1 (25,14)=0 (25,16)=0 (25,34)=0`]. In game:
- The prompt still showed on layer 25 (`20261117-140054-collision-climb`, `03-a-contact`), and Sam
  climbed it.
- Layer 25 also did not block reliably: Sam walked through it on 2 of 4 approaches (to 0.22 m and 0.03 m).
- Run `20261117-140652-collision-layers` compared layers 9, 25 and 10 head to head. After A, Sam climbed
  each of them, rising 1.74 m.

So the climb check is not the layer: it sees anything that blocks Sam. Whether NPCs (layer 9) are
spared by an entity check is unknown.

**The height decides it.** The partner's capsule is configured with a private copy of Sam's
`PhysicsCharacterMoverResource` (RTTI size 0x68: plain data) with a larger `StandingHeight` (+0x30).
`Capsule::Configure` (`0x142446340`) AddRefs the resource at +0x18. The mover factory `0x141C31930` passes
`StandingHeight - 0.05` to the mover's vt+0x268, which stores the height that its shape is built from
[V: decompiles].

The copy (`partner_collision.cpp TallResource`):
- is made only after the source's RTTI name and StandingHeight check out;
- has its refcount (low 22 bits) set to 0x200000, so the release at `0x1419F5E50` never reaches zero;
- adds one reference of ours to the material it points to (+0x60);
- lives for the session (at most 16 copies, in one page).

No game call or patch is added: Configure is the same call as before.

| Height | Result (A pressed against the partner) |
|---|---|
| 1.65 m (Sam's own, v0.7) | "Climb" prompt; Sam on top (+1.77 / +1.78 m) |
| 2.0 m | Sam on top (+2.15 m) |
| 2.6 m | No prompt, but a jump-grab put Sam on top (+2.73 m, `20261117-141709`, `03-a-hold-a`) |
| 3.2 m | No prompt, a hop only (+0.68 m) |
| **4.0 m (default)** | No prompt, hops only; blocked at ≥ 0.81 m |

Runs are hidden, `-SeedSaves complete-100`, all pass, exit 0, no error.txt, real saves unchanged,
0 uploads:
- `20261117-141254-collision-height`: heights 2.6, 2.0, 3.2 and 1.65 in one run (`action collision 5`).
- **`20261117-142256-collision-climb` (final build, scenario `collision-climb`).** The default 4.0 m
  capsule, four approaches from different directions:
  - half stick with A, A held 1.2 s, X and Y;
  - full stick with A and A held;
  - half stick with A ×3;
  - full stick with A ×4.

  Results:
  - Sam was blocked at **0.838 m minimum while the capsule was in the world** (327 ticks under 0.95 m).
  - His highest point was **1.10 m above the ground**: approach 3 put him on the parked trike, where A
    means "Use Vehicle" (`05-c-after-a3`). Everything else was hops of up to 0.71 m.
  - No contact frame shows "Climb": `03-a-contact`, `04-b-contact` and `05-d-mid` show no prompt, or only
    the trike's prompts.
  - **Control** (`action collision 5 0`, the 1.65 m capsule): `06-control-contact` shows "(A) Climb", and
    A put Sam 1.78 m up, on top of it (`06-control-after-a`).
- `20261117-142000-collision-climb`: the same build. Its control line was lost to a scenario typo, so it
  ran five 4.0 m approaches: blocked at ≥ 0.808 m, highest point 1.04 m (the trike again).
- `20261117-142530-two-games-collision` (final build, two games): the guest was blocked by the host's
  4.0 m capsule at **0.816 m**, and on the host's screen the guest's Fragile stopped at 0.816 m. 21 ticks
  in contact on each side.

- **`20261117-153502-collision-climb` (the integrated v0.8 build, all review fixes).** Four 4.0 m
  approaches: blocked at 0.802 / 0.819 / 0.778 / 0.843 m minimum while in the world; highest points
  +0.67 / 0.00 / +0.95 / +0.88 m (hops, and in approach 3 Sam at the parked roadster: `05-c-after-a3`
  shows the partner standing and Sam at the car, not on the partner); `03-a-contact` shows no prompt.
  **Approach 5, Sam above the partner** (`action collision 3 2.5 -2.2`: the partner 2.2 m below Sam's
  feet): collision stayed off for the whole approach (`off: Sam is above the partner (over 1.6 m)`), Sam
  walked to 0.13 m of the spot with no rise (`05-e-above`): no invisible pillar. **Control** (1.65 m):
  `06-control-contact` "(A) Climb", Sam on top (+1.74 m). Exit 0, no error.txt, real saves unchanged,
  0 uploads.
- `20261117-153804-two-games-full-gear` (two games, collision on by default): the guest walked up to the
  host and stopped beside him (`03a-host-sees-guest-front`).

The capsule copy is reused only while Sam's resource still holds the same bytes (everything but the
height and the refcount compared): after a reload a new resource at the old address gets its own copy.

Not covered:
- Ladders and ropes placed against the partner.
- A partner on a slope above Sam: the capsule reaches 4 m up, which is more than the body. It meets only
  Sam, AI and vehicles, never the camera or static geometry. Sam up to 1.6 m above the partner still
  meets the capsule, as with v0.7's.
- Whether the game's climb check skips NPCs by entity type.

## 12. Gear: what the player carries, on the bodies

**Status (2026-11-17, v0.8 development): works in hidden runs, solo and between two games.** The
partner's real body and the self view now carry the player's whole visible gear, not substitute props:
- **Self view (your own screen):** the game keeps drawing Sam's real gear - backpack with its bag and
  everything on it, weapons on the backpack and in hand, shoulder shields, the hip crate, the cuff
  device, hanging boots, the Odradek - on Sam's animated, invisible skeleton, so it rides on the chosen
  character exactly as on Sam (section 8.1: our hide bit moved to the global part mask). The BB pod, which
  the game hides with Sam's own BBPod part, is added as a copy of the game's pod entity.
- **Partner (the other screen):** the sender reads every gear entity the game shows on Sam (his child
  entities and theirs) and sends each one's entity resource (path + GUID, STR1) and its pose relative to a
  joint of Sam's skeleton or to its parent item (STC1 v2). The receiver spawns the same resource from its
  own installed game and places it every frame on the body's joints. Sam's own gear *parts* (exoskeleton,
  BB pod pipe, hip gear, pouches, power glove...) and the backpack's own parts (its bag, rain cover,
  ammo pouches) are mirrored part by part on a Sam body and on the backpack prop. Delivery cargo keeps the
  case props of v0.7.
- `session.ini partner_gear=0` turns all of it off (nothing is read, sent or spawned); the self view's
  hide is part of the self view (section 8).

Code:
- `native/mod/partner_gear.{hpp,cpp}`: reading our gear (entity walk, resource table, parts, cargo), the
  send schedule, the props (spawn, validate, retry, show, remove), the parts mirror, the census.
- `native/mod/real_body.cpp`: the placement in the pose detour (`PublishGear`, `PlaceGear`), the path +
  GUID spawn (`BeginByPath`, `BeginByPathGuid`), `ResourceIdentity`, `ReadJointModel`, the self view's hide.
- `native/mod/pose_wire.{hpp,cpp}`: STC1 v2, STR1, `GearAssignProps`, `GearRetryDelayMs` (ctest
  `st_net_tests`, `TestGearFrames`, `TestGearV2`). `native/mod/link.cpp`: their datagrams and queues.
  `native/mod/player_sync.cpp`: send, receive, echo.

### 12.1 What the game draws on Sam [V: census run `20261117-130343-census`, `gear 9 0`]

The census walks Sam's child entities (`+0x70` parent, `+0x78` first child, `+0x80` next sibling, checked
in `0x14235EBC0`) and the inventory's gear block (`+0x4C80..+0x4F00`). In the 100% save:

| Entity (resource) | Class, components | Where |
|---|---|---|
| backpack (`ds/entities/item/backpack/backpack_temp`, DSBackpackEntityResource) | DSBackpackEntity; DSBaggage2CarrierComponent, ArtPartsData (24 parts: the frame, the cover / bag at 7 capacities, ammo, cylinder...) | spine 4, inventory `+0x4CB8` |
| on the backpack: 6 × `ds/entities/weapons/weapon_collection` | Entity; model components (some BackpackCustomizePaintManagement) | the equipment and stowed weapons, 0.4-0.5 m out from the frame |
| on the backpack: 4 × `backpack_temp` children | Entity; belt (DSPlayerBackpackBeltComponent), chiral cylinder, ... | at the frame |
| on the backpack: `ds/entities/weapons/builder` | DSWeaponEntity | the PCC at the right hip |
| BB pod (`ds_player_inventory` b5041f35) + keyring + dog tag | Entity; DSBBPodComponent | chest, inventory `+0x4D48` (a reference: entity = value − 0x20; `+0x4D60` is its component) |
| the baby (12060443) | Entity; DSBridgeBabyComponent, talk, voice | in the pod |
| 2 shoulder shields, the ammo loader, the cuff device, the hanging boots (`weapons/sam_boot`) | Entity; DSArmorPlate / DSPlayerInventoryAccessory (+ talk, voice) / DSPlayerHangedBoots | arms, hip, right hand, left hip |
| the boots on his feet (188043d6) | Entity; DSPlayerBootsComponent | his root (inventory `+0x4D80`) |
| Odradek arm + body (`ds/entities/player/ds_odradek`) | DSOdradekArmEntity / DSOdradekBodyEntity | left shoulder |
| weapon cases, grenades, rifle, ... | invisible until used | |

Every visible piece is an entity with an ArtPartsData and a SkinnedModel attached by an
AttachToHelperMover. Carried pieces of cargo have no entity (`DSBaggage2 +0xD8` = −1); the personal
equipment of the 100% save is what the `weapon_collection` entities and the backpack's own parts show.
Sam's own gear parts are parts of his model: the mesh controller maps `EDSPlayerMesh` k to an ArtParts
index at `ctrl +0x38 + 8k` (census: ExoMain 32 → 32, ExoSub 38 → 34, BBPodPipe 31 → 2, Hip 72 → 44,
HipGun 73 → 45, Support 86 → 50 ...). `demo_sam_clean` has the same 107 parts (same part resources,
checked at run time).

### 12.2 What is drawn

- **Self view:** Sam is hidden through the controller's global part mask, so `IsRootVisible` stays true
  and the inventory keeps showing every gear entity on his invisible skeleton (section 8.1). Sam's own
  gear parts (exoskeleton...) are hidden with him. The BB pod entity is shown by the game only while Sam's
  BBPod part is (`0x1426A0620` part 30 reads `ctrl +0x2118`), so the self view adds a copy of the pod as
  its only prop.
- **Partner:** every visible gear entity of the sender that passes the mirror policy (12.5), the
  backpack as the cinematic frame `demo_bak0_mule_def` (its 24 ArtParts are the real backpack's: the bag,
  cover and pouches are mirrored on it), Sam's gear parts on a Sam body, delivery cargo as the case props
  of v0.7 (`gear 3 <n>` synthetic cases in the tests).
- Not mirrored (never spawned on the receiver): the boots on Sam's feet (the body's own feet), the baby,
  the cuff device (talk / voice components), the Odradek (its classes update with the player: run
  `20261117-131822` crashed a worker thread at `0x14296F3AE` right after copies of it were created),
  weapons (DSWeaponEntity: never copied, at any level; since section 17 they are sent as their own kind and
  drawn as cutscene props).

### 12.3 Sender (game thread, every 100 ms)

1. The entity walk: Sam's children and theirs (depth 3, at most 320 visited, a stack of 256: Sam has
   about 45 children of his own). An entity is sent when it is visible (`+0x88 & 2`), has a model
   (`+0xB8`), its resource path is under `ds/entities/` (not effects or the camera), it is within 3 m of
   Sam and it passes the mirror policy. Each distinct resource gets an id of a session table (64 at most).
   Each node's first 0xC0 bytes are read once, and its class / resource / policy verdict is cached while
   the node keeps its address, vtable, resource reference and component list (`gear 10 0` turns the cache
   off). Measured on the game thread (run `20261117-153804`, host): 2.7 ms per read without the cache,
   0.8-1.0 ms with it (max 2.3-2.7 ms), every 100 ms (`[gear] read cost ...`).
2. The anchor: the parent item when the entity's parent is one (the backpack, the pod); otherwise the
   nearest joint of Sam's skeleton, torso joints first (a limb joint must be more than 10 cm nearer), kept
   while it is at most 15 cm farther than the nearest (a flip would change the layout).
3. Sam's gear parts (the 26 gear `EDSPlayerMesh` entries through the controller's map: which exist, which
   are shown; the last reading stands while our self view hides them), his head parts (section 14: every
   part below the Head part, by its asked-visible bit, held while he is not in the porter suit or is
   hidden) and the backpack's 24 parts.
4. Cargo, as before: carrier slots → stacks → `DSBaggage2`, deliveries drawn as cases.
5. A new layout is taken when the next read agrees (the debounce); its first difference is logged (`a new
   layout ... held a read: first difference at item ...`, 30 lines at most).
6. **Send** (`gear::TakeSend`): when the layout or the parts change, when a pose moved by more than 2 cm /
   4° (at most every 250 ms), and every 1 s. **STR1** (`gear::TakeSendRes`): a new id at once, then one
   entry every 250 ms round-robin (a lost one comes again). An entry that could not be sent (the link not
   up yet) goes out again first, and on every new link session the whole table is sent again at once (the
   partner cleared its table; review: before, entries made before the session reached the partner only
   through the 250 ms round-robin, about 10-16 s for 40-64 ids). A frame that does not encode is logged.

### 12.4 Wire: STC1 version 2 and STR1

(Version 3 since section 17: the same layout plus the `weapon` kind.)

`'STC1' | u32 seq | u8 version 2 | u8 count | u8 flags | u8 dropped | u16 layout hash | u16 0`, then
[flags bit 1: `u64 apply[2] | u64 visible[2]`, Sam's gear and head parts by ArtParts index (section 14)], [flags bit 2: `u64
apply | u64 visible`, the backpack's parts], then `count` × 18 bytes: `u8 kind | u8 model | u8 variation |
u8 joint | u8 parent | u8 res | i16 x, y, z (1 mm) | 48-bit smallest-three rotation`. Kind 5 (`entity`)
names a resource id (< 64); the id is part of the layout hash, the parts are not. At most 56 items: 1072
bytes with both part blocks, inside one link body (1148). Version 1 is refused (mixed builds never meet:
HELLO build check, section 4).

`'STR1' | u8 version 1 | u8 id | u8 path length n | u8 0 | 16-byte GUID | n path bytes`: one entry of the
sender's table. The decoder refuses an id ≥ 64, a size other than 24 + n, and a path that is not 1-160
bytes of `[a-z0-9_/.-]` under `ds/entities/` without `..`. It rides in a datagram of its own; the receiver
keeps a queue of 16 (no sequence filter: an entry is idempotent); a new link session clears the table.
A session change seen first on the gear or STR1 queue (before that session's first pose) now resets the
session there instead of dropping the frame.

Measured (two games, 100% save): about 15-22 items, 300-450 bytes per STC1 frame, 10-20 frames per 10 s;
STR1 4 entries a second.

### 12.5 Receiver: props

- **Keyed by what they show.** Each item needs a prop of a key (a cinematic prop, or `entity` + resource
  id). On a new layout the props are matched to the items by key (`wire::GearAssignProps`: the same index
  first, then any unused prop of that key), so one piece removed does not respawn every later one; props
  nobody wants go after the detour stopped using the old list (`layout A -> B: N prop(s) kept (M moved to
  another item), K removed`).
- **Checked before anything loads** (`wire::GearResProven`, unit-tested; review: realizing a copy is the
  step that crashed, so a check after it protects nothing). A partner's STR1 entry is kept only when its
  core is one whose entities were mirrored in hidden runs: `ds/entities/item/backpack/...`,
  `ds/entities/player/ds_player_inventory`, `ds/entities/weapons/weapon_collection`, `.../sam_cant`,
  `.../sam_boot`; and never for the entities the level-1 policy refuses that the census found on Sam (the
  BB pod entity unless `gear 8 1`, the cuff device / talk entity, the baby / boots entity, the chiral
  cylinder, the backpack belt). Otherwise `[gear] the partner's resource N '...' not loaded: ...` (20 lines
  at most) and the key is refused. Seen in the final run: an early `ds/entities/player/ds_odradek` entity
  (plain `Entity` class, sent at level 1 once at startup) was not loaded. A game whose own tests set
  level 2 accepts any `ds/entities/` path. Not checked before loading: whether the GUID exists in this
  install's core (another game build), as with the bodies' fixed GUIDs.
- **A changed entry** (an honest peer never changes an id): props showing the old resource are removed and
  spawned again from the new one; logged 10 times at most.
- **Spawn:** by path + GUID like the bodies (`BeginByPathGuid`), at most 4 per tick, 20 m under the body,
  invisible, only once the STR1 entry is known (`waiting for their resource`).
- **Mirror policy** (sender and receiver, `MirrorableAt`): only the plain `Entity` class (never a
  DSWeaponEntity, section 17), and only these components: level 0 SkinnedModel, StaticModel, ArtPartsData, ArtPartsVariation,
  ArtPartsShaderVariableComponent, ShaderVariableComponent, StencilBufferComponent,
  MeshPaintTextureComponent, BoostMeshStreamingForArtPartsDataComponent, ExternalAnimationComponent,
  LodDistanceOverrideComponent, AttachToHelperMover, CustomizedAttachToHelperMover,
  DSOutlineEffectComponent; level 1 (default) adds DSArmorPlateComponent,
  BackpackCustomizePaintManagementComponent, DSSimpleAnimationComponent, SoundComponent and
  DSPlayerHangedBootsComponent; level 2 (hidden tests only) adds DSChiralCylinderComponent and
  DSPlayerBackpackBeltComponent; DSBBPodComponent only with `gear 8 1` (hidden tests only). The receiver
  checks the realized entity again and refuses it for the session otherwise (plus no Humanoid / player /
  backpack entity, no AI, controller, capsule or physics components).
- **Failures are retried** (G2, 12.8): a prop that did not start or load is tried again after 2, 4, 8, 16,
  30 s ... (`wire::GearRetryDelayMs`); only a validation refusal is final for the session. A prop the
  game destroyed releases its spawner (`SpawnTeardown`) and is retried with the same backoff (review: it
  leaked the spawner and was respawned every tick).
- **Placement in the detour** as in v0.7: `world = body ∘ model matrix[anchor joint → the body's own
  joint] ∘ item pose`, or the parent item's world ∘ item pose; vtable + GUID checked before
  `Entity_ExportedSetWorldTransform`. Up to 56 props per slot.
- **Shown** with the body. The real flag is read every tick and set again when the game changed it
  (`reshown`); a tick without a placement no longer hides them (300 ms do); a placement fault that
  switched the detour's list off is published again (3 times a session at most).
- **Parts:** a target's parts are set only when its ArtParts are the same resources as our own reference
  entity at every named index (the partner's Sam body vs our Sam; the backpack prop vs our backpack; logged
  once per target), through the game's part setter `0x1422B2770` inside the model update bracket
  `0x142391620 / 0x1423966A0` (what ApplyMeshControl does), each byte-checked; only parts that differ.
  A fault after the bracket opened closes it (the entity's critical section must not stay held while the
  body is updated at 240 Hz on a worker thread); the calls then stay off for the session.

### 12.6 Evidence (hidden, `-SeedSaves complete-100`, no error.txt unless said, real saves unchanged, 0 uploads)

| Run (`build-testenv\runs\`) | What it shows |
|---|---|
| `20261117-134056-gear-full` (pass; the solo scenario, the final policy) | Self view, Fragile: `01-self-back` Sam's whole real backpack on her back (the dark bag, the olive pouches, the frame), the shoulder shields, the hip crate, the cuff device; `02-self-front` the same from the front, the BB pod (the cinematic pod prop) on her chest but tilted about 45° to her right and her Sam-gold boots (12.7); `05-sam-real-gear-back` / `06-...-front` Sam himself, self view off, the same camera: the same gear. A debug Sam (porter suit) wearing copies: `07-debug-sam-back`, `08-debug-sam-front` (the pod on his chest, the shields, the gear parts set: `partner body: 7 part(s) set`, `backpack prop: 5 part(s) set`, 17 props live). The equipment wheel (`09`): its top item is the blood grenade, in Sam's hand in `11` (HUD "Custom Hematic Grenade"); its copy is refused (DSWeaponAimArc, below). Level 2 (`10c`, `10d`): the backpack's cylinder and belt copies, no crash. G2: `gear 5 4` / `gear 6`: 4 failures, 4 retries, all back. Exit 0, no error.txt. |
| `20261117-133245-two-games-fullgear` (pass; two games, a scratch copy of `two-games-full-gear` before the walk / pack-parts fixes) | Guest `02-guest-sees-host`: its own Fragile with Sam's real backpack, pouches, hip crate, shoulder shield and pod; the host's Sam body ahead in the gold suit with the backpack frame, pouches and its gear parts (enlarged crop: the blue waist lamp of the support exo, `partner body: 7 part(s) set`). Host `03b-host-sees-guest-back`: the guest's Fragile with the frame, pouches, shields and the hip crate (crop). 12 layout changes both ways (`gear 3` cycles), a prop respawn with 6 injected start failures, all props hidden behind the mod's back: both sides kept following (`layout ... kept`, `6 failures, 6 retries, 9 reshown`, `17 items, 17 props live`). The guest's D-pad right opened the equipment wheel (`06-guest-dpad-right`). |
| `20261117-132631-gear-generic2` (pass) | The staged mirror levels 0 / 1 / 2 on a debug Sam (7, 15, 15 props, no crash), G2 fault injection (`4 failures, 4 retries`, `15 reshown`), the first self view with the real gear kept. The cinematic pod anchored on the left shoulder turned on Fragile's skeleton (`13-self-again-back`): anchors now prefer the spine. |
| `20261117-130343-census` (pass) | `gear 9 0` / `gear 9 1`: the census of 12.1 (Sam's child tree, inventory, mesh controller, ArtParts of Sam, the backpack, the pod and `demo_sam_clean`). |
| `20261117-131822-gear-generic` (**fail**, error.txt) | Copies of the Odradek (DSOdradekArm/BodyEntity) and the pod entity were created: an access violation in a worker thread at `0x14296F3AE` (null player). Led to the class rule. |
| `20261117-133622-gear-full` (**fail**, error.txt) | An access violation at `0x142933FA6`, first blamed on the copy of the pod entity (DSBBPodComponent) with the backpack's cylinder and belt. Re-read for section 17 [V: RIP, registers, log order]: `0x142933FA6` is in DSWeaponEntity's update (`movzx r15d, byte [r14+0x52D]` with R14 = its weapon instance `+0x338` = 0, RDI the entity), and the log registers `ds/entities/weapons/builder` (the PCC, a DSWeaponEntity) just before: the crash was the **PCC copy** let through at level 2. Weapons are never copied since (section 17). |
| **`20261117-153804-two-games-full-gear`** (pass; the integrated v0.8 build with every review fix; collision on, name tags in the game font) | Guest `02b-guest-sees-host-front`: the host walked back and faces the guest; enlarged: his Sam body in the gold suit with both shoulder shields, the BB pod on his chest (the cinematic pod: orange and white, tilted; the host's own real pod is a dark capsule, `02b-host-own-front`), a case at his left hip and a green-lit item at his right side (the host's own Sam has a brick-patterned case in his right hand there), the tag "HOSTPORTER" in the game's menu font. Host `03a-host-sees-guest-front`: the guest's Fragile walked up beside him (collision stopped her); enlarged: the pod on her chest, a shoulder shield, a white and yellow case at her left hip, the backpack behind her. Gear changes both ways as in `134347`; `17 items, 17 props live` on the host at the end (`15 reshown, 14 kept across layouts`); the guest's equipment wheel used for real (`06b`: "Removed cargo placed on back"). Read cost 2.7 ms without / 0.8-1.0 ms with the walk cache. Exit 0, no error.txt, real saves unchanged, 0 uploads. |
| `20261117-134347-two-games-full-gear` (pass; the repo scenario, the first full-gear build) | Guest `02-guest-sees-host`: its own Fragile carrying Sam's real backpack (dark bag, olive pouches, frame), hip crate, shoulder shield and pod; ahead the host's Sam body in the gold suit with the backpack frame, the olive pouch panel, its bag (mirrored part, but silver: the bag's colour variation is not mirrored), a shoulder shield and the blue waist lamp of the support exo (enlarged crop). Host `03a-host-sees-guest-front`: the guest's Fragile behind it with shoulder shields and the pod. Gear changes both ways (`gear 3` cycles, a prop respawn with 6 injected start failures, all props hidden behind the mod's back): `6 failures, 6 retries, 11 reshown`, `19 items, 19 props live` on the guest, `17-18 items` live on the host, every layout followed. A real change through the game's own input: the guest took the blood grenade from its equipment wheel (D-pad right, stick up): its frame changed and the host followed (`layout ... 12 prop(s) kept`); the grenade itself is not copied (DSWeaponAimArc). STC1 11-14 frames per 10 s each way; link payload 17.9 KB/s, 597 B per pose datagram on average (gear and STR1 included; v0.7: 575 B). Both exit 0, no error.txt, real saves unchanged, 0 uploads. |

### 12.7 Not covered

- **Weapons in hand:** see section 17 (cutscene props of the same model; the game's DSWeaponEntity is never
  copied).
- **The Odradek, the cuff device, the baby, the BB pod's own entity** are not copied (crash or talk /
  voice components); the pod is the cinematic prop: its model's axes are not the pod entity's, so it sits
  tilted about 45° on the chest (`20261117-134056` `02-self-front`), and it is orange and white where the
  player's real pod is dark (`20261117-153804` `02b`); a fixed correction was not found.
- **Hats, caps, glasses:** the cap is a `ds_player_inventory` entity (proven core) and shows with its colour
  since section 13 (run `20261117-173443`, put on with `gear 14 15 1`).
- **Sam's boots on the self view.** The boots entity on Sam's feet stays drawn on his invisible skeleton:
  Sam's gold boots over the chosen character's shoes. Its ArtParts are shown again by the boots component
  every frame and the entity is put back on Sam every frame (both tried, run `20261117-132631`).
- **Sam's gear parts on another body.** The exoskeleton, BB pod pipe, hip gear are parts of Sam's own
  model: mirrored on a Sam body only, not on Fragile, Cliff... (and not on the self view, where they are
  hidden with Sam).
- **Animated gear stands still.** Copies of skinned gear (the hanging boots, the pouches) are posed by
  nobody: they keep their bind pose.
- **Positions on another character** are relative to Sam's joints mapped by name; on Fragile the backpack
  sits a few cm off (the self view: Sam's gear on Sam's invisible skeleton, a few cm off her smaller body).
- **Colours / customisation:** mirrored since section 13 (variations and shader variables of every mirrored
  item, the backpack's pose).
- Real delivery cargo (cases from a terminal) was still not seen in a run; vehicles, ziplines, two PCs.

### 12.8 The user's report: "the host cargo isn't showing on the guest" (G2)

**What the logs of the user's local two-player test show** (`instances\host|guest\stranded_together.log`,
v0.7.0, guest = porter self view) [V]:
- The link never stopped carrying gear: 10-20 STC1 frames per 10 s each way for the whole session, and
  the guest's partner props stayed `2 props live (0 loading, 2 shown, 0 failed)` (later 1) to the end.
- The guest's own drawn gear never changed: its "unload" moved personal equipment, which v0.7 did not
  draw at all, so no layout changed.
- At host 423 s (guest 413.8 s, the clocks are 9.4 s apart) the **host's own read** lost its one drawn
  piece, the waist weapon drawn as a small case (`our gear: 1 items`, layout 8089 → 28A2) while the host
  stood still; the guest followed at once (`layout 8089 -> 28A2: 1 prop(s) removed`). The guest showed
  exactly what the host sent. Why the piece left the host's waist slot (unequipped, moved into the
  backpack) is not in the log [I].

So the root cause of "the host's cargo isn't showing" was **G1, not the link**: v0.7 drew only the
backpack frame and one waist case of the host (7 of 9 personal pieces, the bag, pouches, stowed weapons,
BB pod, exos were "not drawn"), and that case went when the host moved it.

**Latent defects found on the way, fixed** (each could have made it permanent):
1. A prop that failed to start or load was never tried again this session (`refused` per def, `failed`
   per slot). Now retried with a backoff (2, 4, 8, 16, 30 s); only a validation refusal is final.
2. Visibility was cached (`p.shown`): a prop the game hid (Sleep / Wake, activation) stayed hidden while
   the stats said shown. Now the real flag is read every tick and set again (`reshown`).
3. Props were keyed by item index: one piece removed respawned every later prop. Now matched by key
   (`GearAssignProps`, unit-tested).
4. One placement fault switched the detour's list off until the layout changed. Now published again (3
   times a session at most).
5. A tick without a placement hid every prop; now 300 ms without one do.
6. An STC1 frame that cannot be encoded is logged (it was dropped silently).
7. The BB pod was read at the wrong offset (`+0x4D60` is its component; the pod is the reference at
   `+0x4D48`).
8. The sender's entity walk was cut at 48 nodes (Sam alone has about 45 children): raised to 256 / 320.

Evidence (G2): unit tests (`TestGearV2`: prop reuse, retry schedule), and in hidden runs `gear 5 <n>`
(injected start failures: `4 failures, 4 retries`, all props back within 4 s) and `gear 6` (every prop
hidden behind the mod's back: shown again in the next tick, `15 reshown`) on the debug body
(`20261117-132631`) and on the guest's view of the host after 12 layout changes both ways
(`20261117-133245`: `6 failures, 6 retries, 9 reshown`, `17 items, 17 props live`).


## 13. Gear looks and the back cargo: colours, variations and the backpack's pose

**Status (2026-11-17, after v0.8.0): works in hidden two-game runs.** The user's report on v0.8: "the
bridges cap is gold [on my screen] and on the other player's screen it's just black", "the cargo on the
other player's back isn't displaying correctly". Each mirrored gear item now looks on the other screen
as it does on its owner's: the cap gold, silver or in its colour, the backpack's paint (gold in the 100%
save) and its dark bag, the rain cover's level, the shields' and the stowed weapons' paint and lights,
the hanging boots, the delivery cases' colours. The backpack frame takes the owner's pose (it extends with
the load). `gear 12 0` / `gear 13 0` turn looks / the pose off (tests).

Code: `native/mod/gear_look.{hpp,cpp}` (read, resolve, apply, debug dumps), `partner_gear.cpp` (schedule,
which prop gets which look and pose), `real_body.cpp` (pose targets in the animate detour), `pose_wire`
(STV1, STK1; ctest `st_net_tests` `TestGearLook`), `link.cpp` / `player_sync.cpp` (send, queues, echo).

### 13.1 Why the cap was black [V: decompiles, runs below]

The game colours a gear entity at run time on the entity itself, and a copy spawned from its entity
resource (v0.8) had neither channel, so it showed the material's default:
- **ArtPartsVariation components** (texture-set swaps). The cap: byte inv `+0x4BE0` (0..22 a colour index,
  23 silver, 24 gold); `0x14284AA30` adds the global accessory table's variation (`[[0x147BEB0F8]+8]+0x358`
  `+0x50` / `+0x58`) for 23 / 24. The backpack: `DSBackpackEntityResource.ArtPartsVariations` (v21 gold in
  the 100% save, v22 silver). The rain cover: `DSBackpackConfig.RainCoverLv2/Lv3Apv`. Shields, hanging boots:
  their component resource's `ArtPartsVariations`. Stowed weapons' paint: the item infos' variations.
  Cases: the baggage config's 52 variations.
- **The ArtPartsShaderVariableComponent** (`[e+0x180]+0xF0`, linked back at `+0x80`): sorted entries of 0x28
  bytes `{u64 key (low: hash; high: kind << 27 | part << 18 | sub-indices), u32 hash, f32[4], u8 count, u8
  removed}`, count `+0x50`, array `+0x58`. The cap's colour index is `0x3A3714ED` (0 under gold / silver).
- The cap does not follow the suit colour (the user's guess): it has its own byte. Run `173443`: suit
  colour 9 left the cap as it was on both screens.

### 13.2 Sender (game thread, every 500 ms)

For each item of the current gear frame: an entity item or the backpack → its own ArtParts shown (index < 64:
the rain cover shows one of its 9 parts for the load), its applied variations
(component `+0x30` resource GUID, `+0x50` applied, in component order) and its live shader variables
(fault-free reads); a carried case → the baggage manager's own variation lookup `0x142C541D0(mgr, list
item, (bag+0x204 >> 5) & 1)` (byte-checked with its call site `0x142C48B60`), once per piece; a synthetic
test case → the config's v01 of its size. BB pod / Odradek props are cinematic stand-ins: no look. Each
look is hashed (values to 1/256); a changed one goes out at once (up to 4 a tick), then one every 250 ms
round-robin; a new link session sends all again. The backpack's skinned pose (42 local joints, read
twice, equal reads only) goes out when it moved more than 1 mm / 0.5° (at most 10 Hz), else every 1 s.

### 13.3 Wire: STV1 and STK1

Item 255 (after v0.12) is the look of the vehicle the sender reports (section 22.7; `layout` names its drivable
resource). `'STV1' | u8 version 1 | u8 item | u16 layout hash | u8 kind | u8 res | u8 nvar | u8 nsv | u64 parts apply | u64
parts shown`, then `nvar` × 16-byte GUID, then `nsv` × `{u32 hash | u16 part (0x1FF all) | u8 count 1..4 | u8 0 |
f32 v[4]}`. At most 8 / 32: 924 bytes. `'STK1' | u8 version 1 | u8 item | u16 layout | u32 skeleton layout hash | u8 n | u8 0 | u16 0`,
then `n` × (6-byte smallest-three rotation, i16 × 3 translation in 1e-4 m): 520 bytes for the backpack's 42
joints. Each rides in a datagram of its own; the receiver queues 16 / 4 (newest per item wins, no sequence
filter), cleared on a new session. The decoders refuse bad sizes, counts, parts, kinds and non-finite
values (unit-tested). Mixed builds never meet (the HELLO build check).

### 13.4 Receiver

- **Which look:** the STV1 of item i applies only while it names the partner's current layout hash and the
  item's kind / resource id; a prop moved to another item by `GearAssignProps` keeps what it shows until its
  new item's look arrives.
- **Resolving a GUID, no loads:** among the resources this game already holds: the accessory table, the item
  infos (`[0x147BEB0F8] +0x5D6A0 / +0x5D6A8`, type 2, `+0x40` [0] [1]), our backpack's resource list
  (`+0x138 / +0x140`), the `DSBackpackConfig` (settings `+0x5D6C0`, stored at `0x14292553E`; rain cover), the
  baggage config (`[[0x147BF4968]+8] +0x2B0 / +0x2B8`) and its additional assets, every component resource
  (`EntityComponent +0x30`) of our own gear and player (arrays of variations found by a bounded scan), the
  prop's own component resources, and last our own gear's applied variations. Each candidate is checked to
  be an `ArtPartsVariationResource` with that GUID. In run `173443` all 9 variations of the 100% save's gear
  were found without our own gear's applied ones (accessory, item infos, backpack, backpack config, component
  resources). A GUID not found is left out (logged, tried again after 15 s).
- **Scope per item kind** (review fix): a look takes a variation only from sources of its own kind. A game
  entity (cap, shields, stowed weapons, rain cover, pouches): the accessory table, the item infos, the backpack
  config, our player's component resources, our own gear entity **of the same resource** as the prop (its
  components and applied variations) and the prop's own components. The backpack: our backpack's resource,
  its scan, the backpack config, our backpack's components / applied variations, the prop's own. A case: the
  baggage config and the prop's own. A GUID held only by another kind's source is left out (`... of another
  kind's sources` in the counters; 0 in every final run).
- **Lifetime of the cache** (review fix): only the resident tables (accessory, item infos, backpack and baggage
  config) are kept. Entries found through our own gear carry their owner entity and are dropped and gathered
  again before any use as soon as our gear list changes (it is refreshed every gear read, 100 ms). The prop's
  own components are read at the apply that needs them and never kept (a removed prop's resources may be
  freed).
- **Applying** (0.5 s after the prop is live, at most 4 a tick, game thread): variations made equal to the
  wanted list (`0x14235B910` removes, `0x1422BC100` adds), then each shader variable: the component is
  created and linked as the game's cap function does (`0x1422B03C0`, `+0x80` / `+0xF0`), then
  `exists` `0x1422BD8C0`, `add` `0x1422BD420` (0), `set` `0x1422BDCC0` (a part index only below the prop's
  part count; a new variable is only added while the copy's component holds fewer than 48 entries, the game's
  own gear has 1-8; variables are only ever added or set, so one the owner stops having keeps its last value
  on the copy), then the entity's own parts (the game's part setter in the model bracket, as section 12.5;
  the rain cover's part 5 instead of its default part 0 was the missing big dark bag). Every 2 s the prop is checked (`Satisfied`) and applied again when its own components
  changed it (none did in the runs: `0 again`). Every call is byte-checked once; a fault turns looks off for
  the session (the same accepted risk as the outfit, section 9.2: calls from the tick while job threads may
  update the prop).
- **The backpack's pose** (`realbody::SetGearPose`): the backpack prop (`demo_bak0_mule_def`, the real
  backpack's skeleton `061E5883`, 42 joints, checked) gets the partner's local joints in its own model
  updates (the animate detour, after the original, as a body: rotations and translations, its own scales;
  then the model-matrix and bounds steps). The real frame's cover joint sits 13.4 cm farther out than the
  prop's bind pose with the 100% save's load (`JNT_C_B_000_Cover` y −0.468 vs −0.334) and its leg and arm
  joints are bent: v0.8's frame stood unextended. The backpack's sub-entities (rain cover, belt, cylinder,
  pouches) have skeletons of their own (1-4 joints): no pose is copied to them [V: `gear 11`].
  Review fixes: a prop the game stopped updating is driven with the forced model update only while its
  spawn still checks out (`holo::SpawnAlive`, as `DriveModel` does for a body: the real-body tick runs before
  the gear tick notices a prop the game destroyed); the detour reads the target's entity, skeleton and joint
  count once per call; a fault in the pose write turns gear poses off for the session (`OFF for this
  session` in the counters; `SetGearPose` refuses from then on).
- **Fixes in the same pass:** the cap that cut the item list (56) now leaves the cargo room (the backpack and
  cargo first); poses of items on another item (everything on the backpack, the cargo) stay live while Sam
  moves (only joint-anchored items keep the still reading); the waist weapon case is no longer drawn as a
  white delivery case (the game draws a dark weapon crate, `sam_weapon_case_*`, a physics entity we do not
  copy).

### 13.5 Tests (hidden only, scratch saves)

- `gear 14 [type] [0/1]`: the inventory's accessory list (inv `+0x37F9 + 0x40 i`: type, worn `+0x1D`, item
  `+0x1F`; the inventory update reads it at `0x1427F0FD9` / `0x1427F14B6`). Type 0x0F is the cap: `gear 14 15
  1` makes the game put the cap on (the 100% save owns it, worn 0).
- `gear 15 <0..24>`: the cap colour byte inv `+0x4BE0` (site `0x1427F18EB`).

### 13.6 Evidence (hidden, `-SeedSaves complete-100`, all exit 0, no error.txt, real saves unchanged, 0 uploads)

| Run (`build-testenv\runs\`) | What it shows |
|---|---|
| `20261117-174735-echo-gear-look` (**final code**, one game at 2560x1440; the echo partner, a Sam body wearing copies of our gear through every encoder / decoder, stands 1.4 m beside Sam) | `10-back-v08` (looks and pose off, the v0.8 view): the copy's backpack panel tan where Sam's is gold, a silver crumpled bag in its middle, the rain cover a dark crumple at its side. `11-back-looks`: the copy's panel gold, the large dark rain cover over the top as on Sam, the shields lit green as Sam's (`prop 3 ... 2 part(s) set`: the rain cover shows part 5 like ours instead of its default part 0). The cap: `16-cap-v08` the copy's cap **black** under Sam's gold one (the user's report, reproduced), `17-cap-looks` gold on both, `13-cap-gold`, `14-cap-silver`, `15-cap-colour9` (green) the same on both. `19-stack9`: nine synthetic cases (v01, blue) on the copy only. |
| `20261117-174400-echo-gear-look` (before the parts block) | The same pairs; the rain cover still showed its default part (a dark roll at the side): what led to the parts block. |
| `20261117-173443-two-games-gear-look` (two games, before the parts block) | Cross-game resolution: the host's 9 applied variations all found without its own gear's applied ones; the guest resolved all it was sent (`0 unresolved`; via accessory table, item infos, backpack, backpack config, baggage config, component resources); cap gold / silver / colour 9 applied on the guest's copy (`prop 13 ... look applied`), the host's own and the guest's view the same silver and green (`13-*-cap-silver`, `14-*-cap-colour9`; the gold guest frame had its camera inside a body). `0 again` (no prop changed its own look back), 1331 pose writes on the host's backpack prop, no fault. The two bodies overlapped in the back shots (camera framing), so the side-by-side proof is the echo run. |
| `20261117-191052-two-games-looks-menu` (**final code** after the review fixes; two games, graphics low; host Sam, guest Fragile with self view; pass, exit 0 / 0, no error.txt, real saves unchanged, 0 uploads) | The host's partner body pinned on the guest's screen (`collision 6 2.2`: its back to her; `collision 3 1.6`: facing her). Enlarged crops: `12-host-own-front-gold` vs `12-guest-sees-host-front-gold` and `13-*-silver`: the same gold, then silver cap, with the cap hair under it, on both screens. `10-host-own-back` vs `10-guest-sees-host-back`: the gold panel and the dark bag over it on both; the brown "Bridges" weapon crate on his right hip and the yellow cylinder only on his own screen (not copied, 13.7). `14-guest-sees-host-stack5`: five synthetic cases stacked above his backpack. Host: `35 looks applied (0 partly, 0 again, 0 failed)`, guest: `39 ... (0 failed)`, both `0 unresolved (0 of another kind's sources), 0 variable(s) over the cap`, 2240 / 1795 backpack pose writes, 263 forced updates on the guest, rejects / faults 0, no `FAULTED` line. Gear read cost mean 1.0 ms (max 1.65 ms) per 100 ms read; the looks read every 500 ms mean 0.51 ms, max 0.74 ms. |
| `20261117-185906-two-games-looks-menu` (the same build before the `collision 6` test action; pass) | The guest's back: `20-guest-own-back` (Sam's real gear on Fragile) vs `20-host-sees-guest-back` (her partner body on the host's screen): the gold panel and the dark bag on both; the brown weapon crate on the right hip and the yellow cylinder only on her own screen. |
| `20261117-175305-two-games-gear-look` (two games, graphics low; host Sam, guest Fragile with self view; before the review fixes) | Over the link: the guest applied every look it was sent (`28 looks applied (0 partly, 0 again, 0 failed)`, `0 unresolved`, found via accessory table 3, item infos 3, backpack 1, backpack config 1, baggage config 9, component resources 3; none via its own gear), the rain cover's parts (`prop 3 ... 2 part(s) set`), the hanging boots' (4), the backpack prop's pose (1233 writes, 0 rejects / faults). Enlarged crops of `12/13/14-guest-sees-host-cap-*` against `12/13/14-host-own-cap-*`: the cap gold, silver and green on both screens, the dark rain cover over the host's backpack and his shield lit green on both. Link payload 19.4 KB/s (v0.8: 17.9). The guest's own body stood partly in front of the host in the back shots (the two games start on the same spot): the close comparison is the echo run's. |


### 13.7 Still different

- **Not drawn on the partner:** the folded ladder at the right hip (a baggage model, no entity), the weapon
  crate (`sam_weapon_case_*`, a physics entity: in the final two-game runs the brown "Bridges" crate on the
  owner's right hip, seen from behind, is the most visible difference left on the back), the chiral cylinder
  (the yellow canister low on the left; its copy carries `DSChiralCylinderComponent`, level 2 only), the PCC
  (`DSWeaponEntity`).
- **Sources across different saves:** both games ran the same 100% save. A partner who wears gear the
  receiver's game does not hold (or holds only through another kind's source) gets that variation left out
  (logged): untested.
- **The BB pod** is still the cinematic stand-in (orange and white, tilted); its colour is not copied.
- **Synthetic cases** stand where real ones were measured (run `20261117-105905`), but the owner's frame does
  not extend for them (they are not real cargo), so on the partner they sit on an unextended frame. Real
  deliveries from a terminal were still not carried in a run; their variation comes from the game's own
  lookup and their frame extension with the backpack's pose.
- **The pose is held between frames** (at most 10 Hz while it changes; no interpolation): the frame's sway
  steps.
- **Two PCs**, other saves (a receiver whose game holds fewer of the variations: logged and left out).

## 14. Sam's head: hair, hood and face lines on the partner Sam

The user's report (v0.8): "sams haircut is different on the guest screen". No haircut or hair-growth system was
found in the game's code or data [inferred: no string or code found]. The inventory update `0x1427EEC30` chooses
Sam's head parts **every frame** from what he wears, through his mesh controller (`SetPartVisible 0x1426B4D80`,
reasons 3, 11, 15, 25, 28) [V: decompile]. `demo_sam_clean` has no mesh controller, so the partner Sam always
showed the defaults of `part_sam_body_suiti`: the full default hair, ponytail and wig, the porter hood rolled
down, and no metal lines.

### 14.1 What chooses the head parts [V: decompile, census `20261117-130343-census`, runs below]

| What Sam wears | Parts (ArtParts index in `part_sam_body_suiti`; EDSPlayerMesh) |
|---|---|
| A cap (Bridges cap `0x0F`, Santa cap, cap02; not cap03) and player state `0x50` | hair 55 (HairDef 28) and wig 62 (HairWig 50) hidden; cap hair 82 (HairCap 37, `sam_hair_naked`) shown. cap03 hides the ponytail 56 (74). |
| The cyphead accessory (EDSSuitPartsId 53 = `0x35`, worn in the 100% save) | 61 (HairNotMetalLine 77) hidden; the face and hair metal lines 66 / 67 (75 / 76) shown |
| Glasses (state `0x39`) | 55 / 62 hidden; the glasses hair 80 / 81 (48 / 49), chosen by the hood state |
| The hood (inv `+0x4970`: 3 raising, 4 switch animation, 5 up) and its type (porter / otter / oxi) | hood on / off / switch (4 / 5 / 6, remapped by the controller: 53/54/68 porter, 69/70/83 otter, 71/76/84 oxi). Hood up hides 55 and shows 29 HairHoodOn. |
| The race flag (`[0x147BF6E88]+0xF` bit 0) | 5 HoodOff and 74 ponytail hidden; 88 HoodRace (89) shown |
| Body variant 2 | the defaults |

The 100% save wears the cyphead accessory and the otter hood. So Sam already differs from `demo_sam_clean` at 53,
61, 66, 67 and 70 when the game loads (`echo-hair` `10-front-before`: no cheek lines on the partner).

### 14.2 Design: the head parts ride in the STC1 parts block (no wire change)

- **Sender** (`partner_gear.cpp` `ReadHead`; every gear read, on the game thread, fault-free reads):
  - Every ArtParts part below Head is named in the parts block. Head is the part the controller maps to
    EDSPlayerMesh 2 (controller `+0x38 + 8*2`, index 52). "Below" means its parent pointer chain (part `+0x18`,
    at most 6 hops) reaches Head. The set is computed once per ArtParts array: 32 parts in this model (53..89
    without the switch meshes).
  - Each part is sent with its **asked-visible bit** (flags bit 0). Bit 1 is not reliable under hidden parents.
  - The gear parts (12.3 step 3) keep their effective bit and win where both name a part (the balloon, 57).
- **Held:** the last reading stands (`gear 16` says why) while any of these is true:
  - Sam does not wear the porter suit. The check is `outfit::LocalWearsPorter` (the body variant's GUID, or its
    index if the variant is not loaded) and 107 ArtParts. The indices only mean these parts in
    `part_sam_body_suiti`, and the partner body always uses that model.
  - A Root or global part mask hides Sam (controller `+0x338` / `+0x348`: photo mode, scenes). The self view's
    own reason (bit 63 of `+0x348`) does not count: the asked bits are still the game's then (review fix).
  - A hood switch mesh (68, 83, 84) is shown, i.e. the 0.4 s hood animation is running.
- **Never named:** the switch meshes (68, 83, 84) and the oxi switch's children (85, 86). The game animates them
  with the part animation, which the partner body does not have.
- **Wire:** unchanged. The STC1 version 2 flags bit-1 block already carries `u64 apply[2] | u64 visible[2]` for
  indices < 128, and a parts change already sends a frame. Mixed builds never meet (HELLO build check).
- **Receiver:** the existing `MirrorParts` (12.5). It calls the part setter `0x1422B2770` inside the model
  bracket, only where a part differs, parents first.
  - New: a body found to be "another model" is checked again every 3 s. Before, it was checked once per body.
    "Another model" means its parts are not the same resources as our own Sam's at every named index; our Sam
    may have been in another suit when the body came.
- **Both directions** use the same code: the host's Sam on the guest's partner body, and a guest who chose Sam on
  the host's. A player shown as another character (Fragile...) is not a Sam body, so the check refuses it.
- **Debug:** `action gear 16 <0/1>` (debug-actions.md) turns naming the head parts off / on, in both directions.
  Off only stops naming them, so the body keeps what it had last. The status says whether the last read was
  taken or held.

### 14.3 Evidence (hidden, `-SeedSaves complete-100`, all pass, exit 0, no error.txt, real saves unchanged, 0 uploads)

| Run (`build-testenv\runs\`) | What it shows |
|---|---|
| `20261117-180341-echo-hair` (one game at 2560x1440; `gear 16 0` before the partner came) | Setup: the echo partner is a `demo_sam_clean` fed our own frames, standing 0.9 m beside Sam. `10-front-before`: the partner (left, "You (echo)") has no cheek lines and fuller hair; Sam has the metal lines. `11-front-after` (`gear 16 1`, `partner body: 5 part(s) set`): the same lines and the same combed-back hair on both. `12-front-cap` (`gear 14 15 1`, `3 part(s) set`): both under the gold cap with the loose cap hair (82 shown, 55 / 62 hidden). `13-front-no-cyphead` (cap off, `gear 14 53 0`, `6 part(s) set`): both have the default hair without lines. Sam's own head masks are logged at each change (`Sam's head parts changed`). |
| `20261117-180649-two-games-hair` (two games, both Sam, graphics low; earlier framing) | The final census agrees in both directions at every head part 53..89 (the host with the cap; the guest without the cyphead, with her cap). It compares each game's own Sam (`gear 9 0`) with the other game's partner body (`gear 9 1`). The framing was poor: the bodies were far from the cameras. |
| `20261117-180954-two-games-hair` (partner pinned, first try) | The census agrees again. Each pinned body stood behind that player's own Sam, so the screenshots are not usable. |
| `20261117-191359-two-games-hair` (**final code** after the review fixes; pass, exit 0 / 0, no error.txt, real saves unchanged, 0 uploads) | `cmp.py`: head parts 53..89 agree in both directions; only 8, 17, 18, 20, 21 differ. Enlarged `12b-host-own-cap` / `12b-guest-own-cap-no-cyphead`: the host's gold cap and the guest's silver one, each with the cap hair, the same on both screens; `11b-guest-own-front`: her own Sam and the host's partner body both with the combed-back hair and the metal cheek line. Looks on both games `0 failed, 0 unresolved`. |
| `20261117-191052-two-games-looks-menu` (final code; host Sam, guest Fragile) | The host's own Sam (`gear 9 0`) against his partner body on the guest's screen (`gear 9 1`): head parts agree (cap on, silver); the face crops show the same cap hair. |
| `20261117-181236-two-games-hair` (the scenario's first passing run) | The census agrees in both directions. Only body parts 8, 17, 18, 20 and 21 differ (glove, silver misanga / sleeve, boots); no head parts differ. `11b-*-own-front` on each screen: its own Sam and the partner side by side, both with the metal lines. `12b-*`: the host's gold cap and the guest's silver one, each with the cap hair, the same on both screens. |

### 14.4 Not covered

- No run raised the hood (rain, the hood button) or wore glasses. Their parts are named the same way (hood state
  and type, 80 / 81), but this was not checked.
- During the hood switch the partner keeps the previous head for about 0.4 s, then jumps.
- Another suit (MULE suit, private-room wear) holds the last head reading. The partner body is always the porter
  suit (9.4).
- A guest shown as another character has that character's own head: there is nothing to mirror.

## 15. The cargo menu shows the player's character (self view)

**Status (2026-11-17): works in hidden solo and two-game runs (Fragile), on with the self view (section 8), only in
Manage Cargo (kind 0), `self_view 3 <0/1>` to switch it at run time.** The user's request (v0.8): "if we can change the menu sam to the players character lets
try it". A guest who plays as another character and sees themself as that character in the field now also sees
that character in the on-foot cargo menu ("Cargo Management", opened from the cuff map's "Sam" entry with A,
"Manage Cargo"), carrying Sam's real backpack and cargo, as in the field. The host (always Sam) and a guest who
chose Sam have no self view, so their menu is unchanged. Code: `native/mod/menu_figure.cpp` (the subspace work
object, the figure's parts) and `real_body.cpp` (`MenuTick`, the detour's figure branch).

### 15.1 The menu's Sam figure [V: decompiles, byte checks, run logs below]

- The menu is the game's "subspace" scene. Its work object `W = [0x147BE79B0]` (0x9D0 bytes, built by
  `0x1426E76A0`) is ticked by `0x1426C19B0` under the lock at `W+0x9C0`. Fields used: `+0x14` state, `+0x1C`
  requested state, `+0x08` / `+0x0C` menu kind (in force / asked), `+0x9C8` flags, `+0x30` weak ref (figure
  `+0x20`), `+0x50` the player it serves, `+0x58` the figure.
- States: 0 no player, 1 create the figure (once, `0x1426ACB10`), 2 idle (figure asleep), 3 teleport and wake
  (`0x1426AD430`: the figure to the `subspace_player_point`, the local Sam deactivated), 4 fade in, 5 open
  (`0x1426AB100`), 6 close (`0x1426ADC10`: Sam re-activated, figure asleep), 7 cleanup, back to 2.
- The figure is a `DSPlayerAKukanEntity` (RTTI `0x144BCA020`, 0x7A0 bytes; vtable `0x143D7DE48`, stored by its
  constructor `0x1426F3270` at `0x1426F329E`). It uses **Sam's own Skeleton object** (the same pointer as the
  player's while he is awake) and an ArtParts model of **107 parts with 20 top-level ones** (0, 1, 52, 90..106),
  the porter suit's layout. In the 100% save it stood at world (100, 100, 20). The subspace's gear entities
  (backpack, pod, boots ...) and its carrier with the cargo are separate entities parented onto it.
- "Manage Cargo" from the map is **kind 0** (logged `kind 0/0` in every opening). Kinds 6 and 7 (the private
  room's uniform and cap previews, one-shot openers `0x142D1BD70` / `0x142D1BD00`) keep Sam: there the figure is
  what the player is choosing [inferred; not opened in a run].
- The menu puts the local Sam to sleep (state 3), so before this the self body was removed 1 s later like on
  any sleep and Sam stood in the menu.

### 15.2 What the mod does (game thread, `SelfTick` -> `MenuTick`; nothing changes without the self view)

1. **Detect** (fault-free reads): `W` non-null, state 3, or 4 / 5 with the figure awake; the figure's vtable and
   the weak ref check out; `W+0x50` is our player; its model is a SkinnedModel pointing back at it with a 77-joint
   pose of Sam's layout; its ArtPartsData (found by type name) points back at it; the kind in force and the
   kind asked are both 0 (Manage Cargo, the only kind run; review fix: every other kind keeps Sam). The work
   object is read in one block; the figure's model and parts only in states 3..6, the ArtPartsData found by
   name once per figure and then re-checked with a few reads (vtable, back pointer, count, array).
2. **Keep the self body.** While that holds, the asleep teardown does not run: the field body lives on (it is
   spawned on the figure when there is none, e.g. after `self_view 3 1` while the menu is already open), and its
   source switches from Sam to the figure (`g_self_src`). The figure's model is published to the SkinnedModel
   detour: its pose is captured into a buffer of its own (Sam's capture feeds the wire and is not touched) and its
   world transform is copied onto the body in the figure's own model update (the same exported setter as 8.2;
   the tick moves it when no copy came for 100 ms). The body's own update writes the figure's pose (same joint
   map, latch and pelvis ratio). The body's Validate uses the figure's skeleton when Sam's model is unpublished.
3. **Swap** once the body is latched and has a figure pose written within 250 ms, in state 4 or 5: the figure's
   **top-level ArtParts are hidden** with the game's own top-level setter `0x1422B25F0(apd, 0, 0)` inside the
   model bracket `0x142391620` / `0x1423966A0` (exactly ApplyMeshControl's call for the player's Root), then our
   body is made visible. Why the top-level parts are enough [V: decompiles of `0x1422B8FE0`, `0x1422B8F10`,
   `0x1422B94D0`]: a part is effectively shown (flags `+0x2C` bit 1) only when its own asked bit (bit 0) and every
   ancestor's are set. The menu's per-frame part toggles (`0x14272A070`, about 63 calls of `0x1422B2770`) only set
   asked bits of mapped parts below the top level, which stay hidden under a hidden ancestor. Four times a
   second the remembered top-level parts are checked and hidden again if the game asked one visible
   (`rehides`: 0 in every run).
   The gear, the cargo and the attach-point highlight stay on the figure's (now invisible) skeleton, sized for
   Sam, as in the field (8.4, 12).
4. **Restore** when the state leaves 3..5 (state 6 on close), the kind becomes 6 / 7, the self view goes off, the
   body is torn down or the per-frame sync stops (emergency stop): the figure's parts first (`0x1422B25F0(apd,
   1, 0)` when every top-level part was shown before, else each remembered one with `0x1422B2770`; only when the
   figure still checks out: vtable, ArtPartsData back pointer, the same part array), then our body hidden, then
   the source back to Sam. The field self view shows the body again when Sam wakes (`[self] showing ...`).
   Review fixes: the put-back is tried even after an earlier ArtParts call faulted (each call is SEH-guarded);
   if it still fails, our body is **not** hidden and stands in for that figure at every later opening (the
   menu never shows nobody because of us); a fault inside the hide itself puts back what it hid.
- Byte-checked once (`[menu] cargo menu figure sites verified`): the work-object reader `0x1426B5989` (and that
  it reads `0x147BE79B0`), the ticker's prologue and its state reads (`+0x1C`, `+0x9C8`, `+0x14`), state 1's stores
  of `+0x50` / `+0x58`, the open function's kind stores, the AKukan vtable store (and its target), both ArtParts
  setters, the model bracket and the SkinnedModel vtable. A mismatch or a fault in an ArtParts call: the menu
  shows Sam (logged); the field self view is unaffected.
- Logs: `[menu] subspace state ... (opens N)` on every change, `[menu] figure ...` every second while the menu is
  not idle (captures, copies, pose writes, body-figure distance, rehides), `[menu] showing 'fragile' in place of
  the menu's Sam figure: 20 top-level part(s) of 107 hidden ...`, `[menu] the menu's Sam figure shown again (...):
  put back 20 top-level part(s) (all); body hidden`. `self_view 4` dumps every part's flags (docs/debug-actions.md).

### 15.3 Evidence (hidden, `-SeedSaves complete-100`, solo, `my_character=fragile`, `self_character=1`)

| Run (`build-testenv\runs\`) | What it shows |
|---|---|
| `20261117-191052-two-games-looks-menu` (**final code** after the review fixes; two games; pass, exit 0 / 0, no error.txt, real saves unchanged, 0 uploads) | The guest (Fragile, self view) opens Manage Cargo while connected: `30-guest-menu` Fragile from behind with Sam's backpack, gold shields and cases; `31-guest-menu-turned` her front; `32-guest-field-after-menu` Fragile in the field again. Log: `showing 'fragile' in place of the menu's Sam figure: 20 top-level part(s) of 107 hidden`, then `put back 20 top-level part(s) (all); body hidden`; body-figure distance 0.0 cm, rehides 0. The same in runs `185906`, `190238`, `190619`. On the host's screen her partner body stays where she opened the menu (`30-host-while-guest-in-menu`); after she closed it, her gear was read with 6 items for about 3.6 s (run `185906`: the game moves the gear back from the figure) before the full 13. |
| `20261117-183403-self-view-menu` (solo, before the review fixes; pass, exit 0, no error.txt, real saves unchanged, 0 uploads) | Input: START opens Map/Information with the "Sam" entry focused, A = Manage Cargo (no ranked-event notice in this solo run), B returns to the map, START closes it. `03-menu-sam` (feature off): Cargo Management with Sam in his gold suit from behind, backpack, shield, cargo. `04-menu-fragile` (switched on while open; shown 0.1 s later): Fragile (blonde, black leather) in his place with the same backpack, gold shield, hip cases, ladder and the menu's highlight frame. `05-menu-turned` (right stick): the camera orbits to the front; Fragile faces it, the roadster at the vehicle point behind. `06-after-close`: back on the map. `07` / `11-field-after-cycles` / `12-field-front`: Fragile in the field again with Sam's gear. `08`, `09`, `10`: three more openings, Fragile each time; there the body was kept from the field and shown in the same tick the menu left state 3 (`74.813` state 3, `74.828` shown, state 5 at `75.219`), so Sam's figure never appeared. `13-field-sam` (`self_view 0`): Sam. `14-menu-sam-again`: the menu once more, Sam with every part, as in `03`. Log: 4 shows, 4 restores (`put back 20 top-level part(s) (all)`), body-figure distance 0.0 cm every second, 88-134 figure captures and copies a second, copy fail / aba 0, rehides 0, `[pose]` and `[self]` faults / wpm / repairs 0. |
| `20261117-183119-self-view-menu` (fail: scenario) | The same up to `07`; B returned to the map, not the field, so the next START closed the map and the A presses did nothing. Fixed in the scenario (START closes the map). Exit 0, real saves unchanged. |
| `20261117-182918-self-view-menu` (fail: code, the probe) | The figure's data (kind 0, 107 / 20 parts, Sam's skeleton, (100, 100, 20)) and `03-menu-sam`. Spawning the body in the menu was refused ("Sam's skeleton not available": Validate needed Sam's model, unpublished while he sleeps) and that turned the self view off for the run; fixed (Validate uses the figure's model then). Exit 0, real saves unchanged. |


### 15.4 Not covered / differences

- The part flags after a restore are the game's own recomputation, not a byte copy: parts 85 / 86 (the oxi
  hood-switch's children) read asked-and-effective before the first hide and asked-only after (their parent, the hood-switch
  part 84 [I: section 14's naming], is hidden); every other part of the 107 is identical (`self_view 4` dumps, final run). The same recomputation the
  game runs after photo mode's Root hide; no visible difference in `03` vs `14` [V screenshot].
- The figure's gear and cargo hang on Sam's invisible skeleton, a few cm off a smaller body (as in the field,
  8.4); the menu's attach-point highlight follows Sam's skeleton.
- Only Manage Cargo from the map (kind 0) was opened, and only kind 0 is swapped. Terminal cargo screens (kinds 2,
  5, 8), "View Stored Cargo", the private room's previews (6, 7) and any other kind keep Sam. The field's "Open
  Cargo Menu" prompt (shown next to "Enter Private Room") was not opened in a run; it gets Fragile only if it is
  kind 0. Online (the ranked-event notice), other saves and other characters were not run. Moving or arranging
  cargo with Fragile shown was not exercised.
- The first opening after `self_view 3 1` (body spawned in the menu) took about 0.1 s from spawn to show; Sam's
  figure is visible for that time.
- The self view's BB pod prop (12) is removed after 1 s of the menu like on any sleep and comes back in the field.

## 16. The self view fits Sam's gear to the character (`session.ini self_fit`, default on)

**Status (2026-11-17): works in hidden runs (Fragile, porter suit), solo and in two games.** Before, the self
view drew Sam's real gear on Sam's hidden skeleton (section 8.1) while the character's body, with its own
bone lengths, copied his pose: Fragile is smaller (head joint 6.3 cm lower, shoulders 3.8 cm narrower per
side, torso 12 % shorter), so the shoulder shields and arm devices hung outside and above her shoulders.

### 16.1 What it does [V: runs below]

While the self body is shown (reason 63 set, section 8.3), Sam's own model update ends with his skeleton
in the character's proportions (`CaptureSam` in `real_body.cpp`, Sam's branch of the animate detour):
- every local joint but the root takes the self body's latched translation and scale (the same values
  `ApplyBody` writes on her body), the pelvis Sam's translation times the pelvis ratio; then the game's
  model-matrix and bounds steps run again (`0x141D8AA20`, `0x141D8A660`, as for the bodies).
- **Same pose** (default; `self_view 6 <0/1>`). The rotations, the root and the pelvis come from **the capture
  the self body was last written from** (`ApplyBody` publishes it), not Sam's newest: the body's model is
  updated before Sam's in a frame, so it shows the previous frame's pose (section 8.2), and gear hung on Sam's
  newest pose parted from it while moving. Measured every tick (Sam's hidden joints against the drawn body's,
  model space, `[self] fit on: ... Sam vs body cm mean/max`): with the same pose **0.0/0.0 cm** at the neck,
  both hands and the right foot standing, running uncapped (about 115 fps) and running at 30 fps; with Sam's
  newest pose, running, mean 1.4-2.8 / max 5-7 cm uncapped and mean 6-8 / **max 15-20 cm at 30 fps** (run
  `20261117-215351-self-fit-head`; the same in `-220357`). The cost: Sam's hidden skeleton is one frame behind
  his animation (what the game reads from it, such as hand positions for aiming, is one frame old) [I: no
  effect seen].
- **Head lift** (default 100 %; `self_view 7 <percent>`; *since 2026-11-18 0 for a body with a headgear row:
  section 20 gives each head item its own offset instead*). Sam's head joint (31; its subtree 31-34) sits that
  share of the way from her head back to where Sam's own head would be: Sam's cap and glasses hang on his
  `HLP_Head` helper, made for his head, and with her head joint the cap sank over her eyes with her hair
  showing through its crown (`01-front-lift0`). Measured once per show on Sam's own pose: the original
  animate's head model matrix against the fitted one, expressed in the neck joint's (30) frame; the frame
  convention is checked on that very write (the head must sit at neck + t . axes: `check 0.00 mm`) or the lift
  stays off (state 2). Measured: 5.6-6.1 cm. Front and side close-ups at 0 / 50 / 75 / 100 % (run `-215351`,
  2560x1440, enlarged): 0 and 50 % cover her eyes; 75 and 100 % sit on her hair with her eyes visible; 100 %
  also keeps the third-person camera where it is with Sam (the camera follows that joint: the frames shift with
  the lift).
- So everything the game hangs on Sam's skeleton sits on her: the helper-attached gear (shoulder shields,
  cap and glasses on `HLP_Head`, BB pod, hanging boots, Odradek), the backpack, the cargo slots on the arms
  and hips, the weapon in her hand (section 17), and the **equipped boots**, which copy Sam's leg and foot
  model matrices (`DSPlayerBootsComponent`, MsgModifyAnimatedPose `0x1427C4880`, 10 joints by name hash):
  they are drawn on her feet and cover the boots of her own model (the porter suit's boots are a primitive
  inside its one body mesh, `sam_body_boot_v00_set`, so they cannot be hidden as a part; they are not visible
  under the equipped boots in the runs below).
- **Clean capture.** The capture (the wire's STJ1 frames, the self body's pose, the meter) still gets Sam's
  own joints: before the capture, a rotation, translation or scale still bit-identical to the one written on
  the last update (the game did not rewrite it) is read as its clean value. One Sam update at a time goes
  through the guard, the capture and the fit; a second one at the same moment skips its capture (`busy`, 0
  in the runs), so no unguarded pose reaches the wire.
- **Leak check** (the review asked whether the game's animate derives anything from the pose we leave
  behind). Logged every second: of the translations the shape changes, how many came back nearer the shape
  than Sam's reference (taken at the first fit update), with the fit on and, as the comparison, with the self
  body shown and the fit off; and the captured pelvis length both ways. Run `20261117-220357-self-fit-head`,
  running uncapped: **fit on 4.00 per update, fit off 4.00 per update, the same joints both ways (26, 27, 55,
  56: `MTP_R_Hand_a/b`, `MTP_L_Hand_a/b`, hand helpers the game animates), the same largest change (78.3 mm)**;
  standing 0 both ways; captured pelvis 90.6-97.6 cm running either way. Two games (`20261117-220002`): 0
  while running with the fit on and off; the same four hand helpers only while the rifle was held. So no leak
  was seen [V for those states; vehicles, falls and ladders not run].
- **Off.** `FitOff` runs before reason 63 is cleared (SelfUnshow, a new player entity, the emergency stop),
  and those paths then wait (at most 5 ms) for a Sam update already inside the fit, so the last write lands
  before the clear (review: one in flight could land after it). Sam's next update writes nothing and puts
  back any value the game left as ours (`restores`, 0 in the runs: the game rewrote them). Sam can still be
  drawn with her proportions for the one update between the clear and his next animate.
- **Safety.** No new game call or hook: the same `WriteProcessMemory` of the 77 local joints and the same
  two pose functions the bodies use, on Sam's pose after the original animate returned, with the pose header
  checked as for the capture (skeleton, state 4, sizes). A fault in the pose functions turns the fit off for
  the session (`faults`). The fit only runs for a latched body on its own skeleton (a body on Sam's skeleton
  has his proportions). Not covered: another thread reading Sam's matrices between the original animate and
  our rewrite.
- **The other screen.** The sender (partner_gear, section 12.3) measures each gear item against Sam's
  reshaped joints, so the receiver, placing it on the partner body's identical joints, gets the same fit with
  no wire change. Items on the head joints (31-34) are measured against her head without the lift
  (`realbody::HeadLift`: the sender takes the lift, rotated by the neck joint, off those joints' frames), so
  the receiver's placement on her body's real head carries the same lift: on the host's screen the cap sits on
  her hair with her eyes visible (run `20261117-220002`, 1920x1080, enlarged). A guest with the self view off
  (or the fit off) still sends items measured on Sam's proportions, as before.
- **Side effects** [I]: the game's own systems that read Sam's joints (aim origins, IK) see the character's
  proportions, one frame late, while the self view is shown; with the lift at 100 % the camera height is
  Sam's.

Controls: `session.ini self_fit=0` (mod key only, not written by the launcher) keeps Sam's proportions;
`self_view 5 <0/1>` the fit, `self_view 6 <0/1>` the same pose, `self_view 7 <percent>` the head lift at run
time (docs/debug-actions.md). Logged: `[self] showing ... fit on (...)` and every second `[self] fit on:
writes=... same-pose=... (newest ...) ... held=... (t joints, q, s) restores=... wpm=... torn=... busy=...
faults=...; leak check ...: off .../update ... on .../update ...; captured pelvis cm off ... on ...; head
lift ...% (measured ... cm, check ... mm, state ...); Sam vs body cm mean/max over N ticks: neck ... R hand
... L hand ... R foot ... head ...`.

### 16.2 Evidence (hidden, `-SeedSaves complete-100`, all pass, exit 0, no error.txt, real saves unchanged, 0 uploads)

| Run (`build-testenv\runs\`) | What it shows |
|---|---|
| `20261117-215351-self-fit-head` and `-220357-self-fit-head` (solo, `my_character=fragile_suit`, 2560x1440, the cap put on with `gear 14 15 1`) | `01-front-lift0/50/75/100`, `01-front-nofit`, `02-side-lift0/100`, `02-side-nofit` (enlarged): the cap over her eyes with hair through the crown at 0 %, still over her eyes at 50 %, on her hair with the eyes visible at 75 and 100 % (100 % as with the fit off). `01-front-lift100` against `01-front-nofit`: the shoulder shields closer to her shoulders, the hip case against her hip, the gold-trimmed equipped boots on her feet in both. Meter: same pose 0.0/0.0 cm standing and running at both rates, newest pose up to 7 cm (uncapped) and 20 cm (30 fps) running; the leak check above. `06-other-character*`: the self view switched to the leather-jacket Fragile (`self_view 1 3`), the fit and lift measured again. |
| `20261117-220002-two-games-final` (host Sam, guest `fragile_suit` with the self view, fit and lift, as the launcher writes by default; `graphics low` with a 1920x1080 settings.cfg) | Host's view of the guest, enlarged: `01` the cap on her hair, eyes visible, empty hands, her own model's boots (the partner side has no equipped boots, 16.3); `02` the rifle prop upright in her right hand; `03` shouldered and aimed; `05` the red blood grenade in her right hand; `06` empty again. Guest's own screen: `02-guest-self-rifle` her real blue non-lethal rifle upright in her right hand, the cap on her hair from behind; `12-guest-self-off` Sam with his gear (self view off), `13-guest-self-again` Fragile again. Guest's view of the host: `08` a dark rifle across his chest while he aims, partly behind her cap. `10-host-sees-guest-back`: she stepped out of the frame (weak). |
| `20261117-202654-self-fit`, `-203337-two-games-fit` (before the same pose and the lift) | Kept for history: the fit standing; the cap then sank (no lift). |

### 16.3 Not covered / still different

- The gear models keep Sam's size and Sam's girth in their helper offsets (the shields are made for his
  shoulders: from the front they still stand a little out from hers).
- **The partner's boots** *(done 2026-11-18: section 21, the boots prop posed from the body's legs)*. On the other
  screen the partner body showed her model's own boots, not the equipped boots: the boots entity (`ds_player_inventory`, DSPlayerBootsComponent) is not mirrored (a copy of
  a DS gameplay component is refused by the mirror policy, section 12; copies of such entities crashed worker
  threads before). A boots prop driven as a pose target (its 10 joints from the partner body's pose) is the
  planned next step; it must first be proven at mirror level 2 in hidden runs. (`demo_sam_boot_def_new` is
  named in the path lists but is in no archive of this install.)
- The cargo menu's figure (section 15) is not fitted (the menu still hangs Sam's gear on Sam's proportions).
- A guest with the self view off sends gear measured on Sam's proportions.
- Vehicles, falls and ragdoll, ladders and ropes, carrying a body and BT tar were not run with the fit on.

## 17. Weapons in hand: the partner's weapon on its body, yours on your character

**Status (2026-11-17, after v0.9.0): works in hidden runs, solo and between two games.** The user's request: "guns
visible in hand is important". A weapon or grenade the player draws now shows in the hand of their body on the
other screen, follows the hand (aiming, firing, the throw's wind-up), and goes when it is put away. On your own
screen, under the self view, the game's own weapon sits in your character's hand.

### 17.1 How the game holds a weapon [V: static analysis, then the run logs below]

- Every weapon or hand tool is a `DSWeaponEntity` (RTTI `0x144BDF5E0`) owned by the player's inventory:
  `+0x70` parent, `+0xB0` its `AttachToHelperMover`, `+0x338` the weapon instance. Instance `+0x12` u8
  EDSWeaponId, `+0x18` its `DSWeaponParameter` (`+0x20` Id, `+0x113` Lv), `+0x300` owner, `+0x308` back
  reference (entity + 0x20), `+0x450` flags. Mover `+0x48` its entity, `+0xA0` "local is identity", `+0xB0` the
  helper name.
- Drawn: `WeaponInstance::AttachTo` (`0x142911300`, from `Inventory::EquipToHand` `0x1427ABCE0` and the grenade
  state machine `0x1428827F0`) parents it to Sam on helper `MTP_R_Hand_a` (joint 26 of the 77) with an identity
  local. On the backpack hanger: parent the backpack, helper `HLP_WeaponAsr2`, `HLP_WeaponPzbr` ... Grenades wait
  in the pouch (`HLP_Grenade`, a pouch entity) and are visible only in the hand.
- Logged in every run (`[gear] our weapon ...`): the assault rifle `weapon id 45 (parameter id 45) level 4`,
  parent the backpack on `HLP_WeaponAsr2`, then parent Sam on `MTP_R_Hand_a (joint 26, local identity)`; the
  blood grenade `id 7`, the electric grenade `id 10`, the PCC `id 38` on `HLP_WeaponPzbr`; `instance ok`,
  `mover ok` every time.
- **A copy of a DSWeaponEntity crashes the game**: its update reads the weapon instance without a null check
  (`0x142933FA6`); a copy has none. Run `20261117-133622` was that crash (the PCC copy at mirror level 2, section
  12.6). `MirrorableAt` refuses every DSWeaponEntity now, at any level, and the receiver never loads a weapon's
  own resource at any level (17.4).

### 17.2 Sender (game thread)

- **The read** (the 100 ms entity walk of section 12.3): a visible DSWeaponEntity under Sam (in a hand, or on
  the backpack) is a `weapon` item: its resource (path + GUID, an id of the STR1 table, cached per node like an
  entity's), `model` = EDSWeaponId, `variation` = its level, anchored on **the joint its mover's helper names**
  (`MTP_R_Hand_a` 26, `MTP_L_Hand_a` 55, `_b`/`_c`, `MTP_Spine_a..c`), else on its parent item (the
  backpack), else the nearest joint. In a hand its pose relative to the anchor is the mover's constant local, so
  the hand carries it on the receiver's body with the 77-joint pose (no per-frame weapon pose). Every field is
  read fault-free and checked (back reference, owner, `DSWeaponParameter` / `AttachToHelperMover` RTTI, the mover
  pointing back). The cartridges (DSAttachOrPhysicsEntity children) are refused as before. **A weapon with no
  prop on the other screen (17.4: the PCC, bola gun, launchers ...) is not sent and takes no id**: the STR1
  table has 64 ids for the session and never frees one (review), so only weapons the receiver can draw use it
  (logged once each, `our weapon '...': no prop for it on the other screen (not sent)`); a full table is
  logged once (`the resource table is full`).
- **The watch** (every tick, about 8 reads): the inventory's current weapon (`0x1427A4390`: the instance at
  inventory `+0xD0 + 0x70 * u8 [+0x1C78]`, byte-checked) and its entity's visible bit and parent, plus those of
  the weapon that was current before (put away: the slot changes when the wheel's item is picked, the old
  weapon is hidden later). A change triggers the read at once, at most once per 40 ms (a state that toggled
  every tick would otherwise turn the 100 ms walk into a per-tick one; review). `gear 17` counts the reads
  that waited for the gap (1 of 7 changes in run `20261117-220002`).
- **No debounce for weapons**: a layout that differs from the last only in its weapon items is taken at once
  (the one-read agreement of section 12.3 stays for everything else).
- `gear 17 <0/1>` switches weapons off / on and reports the watch: `draw / holster to frame: mean 14 ms, max
  109 ms over 8` (run `211649`; 0 ms in 7 of 8: the read runs in the same tick as the watch).

### 17.3 Wire: STC1 version 3

One new kind, `weapon = 6` (`kGearKindMax` 6), in the same 18-byte item: `res` is the weapon's resource id
(part of the layout hash, like an entity's), `model` its EDSWeaponId, `variation` its level. STC1 goes to
version 3 (a version 2 frame is refused; mixed builds never meet: HELLO build check). STV1 keeps a weapon look's
`res` too (`wire::GearHasRes`). Unit-tested in `st_net_tests` (`TestGearV2`: round trip, the id in the layout,
an id of 64, a kind of 7 and a version 2 frame refused). *(Version 4, 2026-11-18, adds kind 7 `boots` and its
block: section 21.3; the test now uses kind 8 for the unknown kind.)*

### 17.4 Receiver

- **The weapon's own resource is never loaded.** Its STR1 entry (`ds/entities/weapons/...`) is kept for the
  weapon items only (`[gear] the partner's weapon resource N '...': never loaded itself; its weapon items show
  w_assault_rifle`); an entity item naming it stays refused (`GearResProven` unchanged). Since the review this
  holds **at any mirror level on the receiver too**: `ResAllowed` refuses a weapon's own resource
  (`wire::GearResWeaponOnly`: `ds/entities/weapons/...` other than the three proven entity resources of that
  folder, `weapon_collection`, `sam_cant`, `sam_boot`; unit-tested in `st_net_tests`) before the level-2 test
  shortcut, so a weapon resource is never loaded even from a peer that sent one at level 2.
- **A cutscene prop of the same model** (DSControlledEntity, the class already spawned for the bodies, the
  backpack and the pod; decoded offline from the cores, `kWeaponMap` in `partner_gear.cpp`):

| Weapon core | Prop | Match |
|---|---|---|
| `assault_rifle` (every level, lethal and non-lethal) | `demo_apv_asr2_main_def_v04_def` d6215dea | the same `part_asr2_main_def` model; its colours are the prop's variation (dark), not the player's |
| `blood_grenade` | `demo_grbl_main_def` 76840453 | exact |
| `electrical_grenade` (remote stun bomb) | `demo_grdc_main_def` 0ffe5ad6 | exact |
| `grenade` | `demo_grfl_main_def` 7b42becd | exact (`part_grfl_main_def`) |
| `smoke_grenade` | `demo_grsm_main_def` 9f924eca | exact (`part_grsm_main_def`) |
| `multi_rod` | `demo_mrod_main_def` e00d083a | exact |
| `hand_gun` (player: hag1 / hag3) | `demo_hag2_main_def` af940392 | approximate (another handgun model) |
| `shot_gun` (player: shg0) | `demo_shg2_main_def` c739ae4a | approximate |

  No prop exists (nothing is drawn, logged once per resource): bola gun, grenade launchers, sticky gun, maser
  gun, EX grenades, PCC, ladder, climbing anchor, spray, strand.
- Placed every frame by the detour on the body's own joint 26 like every other prop (section 12.5), shown with
  the body. A prop no item wants any more (put away, thrown, switched) is **kept hidden for 60 s** (6 at most)
  and taken again by the next item of the same key: the next draw shows without a load (`weapon spares N (K
  kept, R reused)`). Logged: `weapon prop N 'w_...' shown X ms after its layout was taken`.
- **Self view.** Nothing to send: the game itself draws the real weapon on Sam's hidden skeleton, which the fit
  of section 16 reshapes to the character's bones, so it sits in her hand (her real, colour-correct weapon).

### 17.5 Latency [V: logs below]

Draw on one game to the prop in the hand on the other (both games capped at 30 fps, one PC, UDP loopback):
- sender: the watch and the read in the same tick (0 ms in 7 of 8 changes in run `211649`, 109 ms once; before
  the watch also followed the previous weapon, a put-away was found by the 100 ms read and the watch's figure
  read a misleading 1.1 s, runs `205843`..`211150`), the frame on the next send (at most one tick, 33 ms);
- link: frame taken -> layout applied on the other game 31-55 ms (run `211150`, from the same layout hashes
  logged on both games, both directions: the clock offset cancels);
- prop: shown 33-67 ms after the layout on its first spawn (one load of the cutscene resource), 0 ms when a kept
  prop is reused, and the rifle drawn from the backpack keeps its prop (the same key, re-anchored).
So about 70-150 ms at 30 fps; the hand's motion afterwards is the pose stream's (section 3).

### 17.6 Evidence (hidden, `-SeedSaves complete-100`, all pass, exit 0, no error.txt, real saves unchanged, 0 uploads)

| Run (`build-testenv\runs\`) | What it shows |
|---|---|
| `20261117-205442-weapons-solo` (solo, 2560x1440, the self view Fragile porter suit with the fit, a debug Fragile mirroring her and wearing her gear through the partner's placement, `gear 4 1`) | `03-rifle-drawn` (enlarged): the self view's Fragile holds the game's own non-lethal rifle (purple and blue) upright in her right hand; the debug Fragile, facing her, holds the dark rifle prop upright in her right hand. `04-rifle-aim`: the debug Fragile shoulders the prop and aims, the grip in her right hand. Later frames: the bodies overlap or the camera was turned by the wheel's stick (fixed in the scenario since). |
| `20261117-211649-weapons-solo` (the same, the wheel used without turning the camera) | Enlarged: `09-blood-grenade` the red and white grenade in the debug Fragile's right hand; `13-electric-grenade` the stun bomb in her hand; `15-put-away` both hands empty; `16-rifle-again` the debug Fragile holds the dark rifle prop again while the self view's Fragile in front holds her real purple rifle. Watch: `mean 14 ms, max 109 ms over 8`; the rifle's prop came back from the spares (`shown 0 ms`). |
| **`20261117-211150-two-games-weapons`** (host Sam, guest `fragile_suit` with the self view and fit, `graphics low` 1280x720) | Host's view of the guest's body, enlarged: `01` empty right hand; `02-host-sees-guest-rifle` the dark rifle held upright in her right hand; `03-...-aim` the rifle shouldered and aimed; `05-...-grenade` the red and white grenade in her right hand; `07-...-put-away` the hand empty again (run `210601`: `weapons 0` in the host's layout within 0.1 s of the guest's put-away, clocks aligned by the layout hashes). Guest's view of the host's Sam body: `09-guest-sees-host-aim` a dark gun across his chest while he aims; in `08` / `10` his right hand is hidden behind the guest's own shoulder shield (weak evidence for that side; the logs show the props live and shown: `weapon prop 4 'w_assault_rifle' shown 66 ms`, `'w_electric_grenade' shown 33 ms`). Guest's own screen: `02-guest-self-rifle` her real rifle in her right hand. |
| `20261117-212222-two-games-weapons` (the final build: the watch follows the previous weapon too) | The host's view of the guest, enlarged, as in `211150`: empty hand (`01`), the rifle upright (`02`), aimed (`03`), the grenade in her right hand (`05`), empty again (`07`). `gear 17`: `draw / holster to frame: mean 0 ms, max 0 ms over 4` on both games; props `shown 32-67 ms` after their layouts. |
| `20261117-205843`, `-210231`, `-210601`, `-210924-two-games-weapons` | Earlier framings of the same scenario (the guest hidden behind the host's Sam, the camera turned by the wheel's stick); same logs: every draw, switch and put-away followed on both games, no prop failure. |
| **`20261117-220002-two-games-final`** (after the review fixes; the guest as the launcher writes it, 1920x1080) | Host's view of the guest, enlarged: `01` empty right hand, `02` the dark rifle prop upright in her right hand, `03` shouldered and aimed, `05` the red blood grenade in her right hand, `06` empty again (logs: `w_assault_rifle shown 65 ms`, `w_blood_grenade shown 33 ms`). Guest's own screen: `02-guest-self-rifle` her real blue non-lethal rifle in her right hand. Guest's view of the host: `08` a dark rifle across his chest while aiming, partly behind her cap; `07` the rifle not visible (his hand low, behind her head): weak. The PCC was logged `no prop for it on the other screen (not sent)` and took no id. |

### 17.7 Not covered

- **Weapons with no cutscene model** (the bola gun, grenade launchers, sticky gun, maser gun, EX grenades, the
  PCC, ladder, climbing anchor, spray, strand): nothing is drawn for them on the other screen. Drawing them needs
  a plain entity built from the loaded weapon resource's model components (a private `EntityResource` and the
  game's own entity creation, as `DSWeaponEntity::OnInit` `0x14292D290` does for its cartridges), with its own
  lifetime: not attempted.
- **Colours**: the rifle prop keeps its own dark variation; the player's rifle (gold Lv4, non-lethal blue) is
  not matched (the look sync of section 13 does not resolve weapon variations yet). Handgun and shotgun props
  are other models of the same kind.
- Thrown grenades and fired projectiles are world entities, not drawn on the other screen; muzzle flashes,
  tracers, the aim laser and sounds are not mirrored.
- Stowed guns on the backpack hanger (the rifle on `HLP_WeaponAsr2`) are drawn as their prop on the backpack;
  the PCC there has no prop.
- Tested with the 100% save's rifle and grenades only, standing; not while running, in vehicles or with the
  left hand (`MTP_L_Hand_a` is mapped but not seen in a run).

## 18. Partner voice: STS1 (`pose_wire`, ctest `st_net_tests`)

What each player says reaches the other as STS1 frames; the design, the receiver and the hidden-run evidence are in
[audio-research.md](audio-research.md) section 15 (`session.ini voice_sync=1`, `native/mod/partner_voice.cpp`).

**STS1 frame.** 20 bytes plus 16 per sentence (at most 84):

| Part | Contents |
|---|---|
| Header | `'STS1' \| u8 version 1 \| u8 kind \| u8 flags \| u8 n \| u32 seq \| u32 sender_ms \| u32 situation hash` |
| Sentences | n × 16-byte GUID of a `SentenceResource` (the raw bytes at `+0x08`, as the game keeps them) |

- **kind** 0 = the sender's Sam said these sentences (n = 1..4; the receiver plays the first one that resolves);
  1 = the sender's player is not Sam: a line was said but **not sent** (n = 0; the receiver plays nothing); 2 = a line
  of the sender's own character (n = 1..4; reserved for Phase 4, never sent by this build).
- **flags** bit 0 = the sender is the host; other bits are cleared by the decoder. **situation hash** 0 (unknown:
  the talk handler that feeds the sender does not see it).
- The decoder refuses another magic or version, kind > 2, n > 4, a size other than 20 + 16 n, kind 0 / 2 with n = 0,
  kind 1 with n > 0, seq 0 and an all-zero GUID (`TestSpeechFrames`). STP1, STJ1, STN1 and the gear decoders refuse
  it, and it refuses theirs (tested).
- **Delivery.** Not a FRAME message: STS1 is queued on the link's **reliable ordered channel 2**
  (`link_protocol.hpp`; `link.cpp` drives the senders and receivers from its 20 ms idle tick: first transmissions,
  resends after the RTO within a 2,400-byte budget, RELACKs after 50 ms; all reset on a new session). A frame is
  resent until acknowledged and delivered once, in order (tested through a lost first transmission). The receiver
  also ignores a seq it has already seen in the session (idempotent). `link::Info` counts speech frames queued and
  delivered and the reliable resends; `[pvoice] counts` prints them. *(changed 2026-11-18)* The link hands at most 16
  speech frames to the game per pump and leaves the rest in the reliable receiver for the next one (before, more than 16
  in one pump were lost after being acknowledged); the game plays at most 2 received lines a tick.
- **Language.** The GUID names the sentence, not the sound: each receiver plays it from its own install in its own
  voice language, with its own subtitle text (shown only with `voice_subtitles=1`).
- Mixed builds never get this far: STS1 lives in `pose_wire.hpp/.cpp`, part of the build id's wire hash (section 4),
  so the HELLO is refused (`build_mismatch`) between builds whose frames differ.
- **Budget.** About one frame per talk: 33 talks in 140 s of play in Phase 0, 20 to 36 bytes each.

## 19. Partner sounds: STA1 (`pose_wire`, ctest `st_net_tests`)

The sender's own footsteps, foley, cargo and weapon sounds reach the other game as STA1 frames; the design, the
receiver and the hidden-run evidence are in [audio-research.md](audio-research.md) section 16 (`session.ini
sound_sync=1`, `native/mod/partner_sound.cpp`).

**STA1 frame.** 12 bytes plus 24 per item plus 8 per RTPC; the encoder never writes more than 512 bytes (at most 32
items):

| Part | Contents |
|---|---|
| Header | `'STA1' \| u8 version 2 \| u8 n \| u16 seq \| u32 sender_ms` (version 1 before 2026-11-18, section 22.9) |
| Item (24 bytes) | `u32 a \| u32 b \| u8 op \| u8 slot \| u8 tag \| u8 nrtpc \| u16 age_ms \| u16 fade_ms \| i16 off[3] \| u8 category \| u8 flags` |
| RTPCs | nrtpc × `{u32 rtpc id \| f32 value}` |

- **op** 0 = post (a = event id, b = the sender's playing id, both non-zero; slot 0..6 = head, left foot, right foot,
  right hand, left hand, back, hips; off = where the sender's game object was, in cm from that joint, world axes;
  the RTPCs set on that object, newest last, at most 12); 1 = stop (b = the playing id, fade_ms; everything else 0);
  2 = action on event (a = event id, b = a playing id or 0, slot = the `AkActionOnEventType` 0..4, fade_ms);
  3 = RTPC update on the tag's object (1..12 RTPCs, fade_ms = the interpolation).
- **tag** names the sender's game object (1..255, reused after unregistration) so later RTPC updates and actions reach
  the receiver's object bound to it. **age_ms** (0..2,000): the item happened at `sender_ms - age_ms`, on the same
  clock as the pose's `sender_ms`; the receiver posts it when its pose play time reaches that moment. **category**
  0..4 (foley, steps, weapon, cargo, gear) is for the counters only.
- *(version 2, 2026-11-18, section 22.9: the vehicle sounds)* **category 5 `vehicle`**: a sound of the vehicle the sender
  rides (its own SoundComponent: engine loop, road noise, ignition, cut-off, suspension...). **slot 7 `vehicle`**: `off`
  is in the vehicle's own frame (cm; x right, y forward, z up) and the receiver's object follows its copy of the
  partner's vehicle instead of a joint. A post or rtpc item has slot 7 exactly when its category is vehicle. **flags**
  (the reserved 0 of version 1): bit 0 `kSoundHeld`, on a vehicle post only: a loop the sender posts again every 1.5 s
  (`kSoundHeldRefreshMs`) with the same playing id while it plays there; the receiver posts it only when it does not
  play already and stops it when no such refresh came for 5 s (`kSoundHeldLeaseMs`). The rest of the format is
  version 1's.
- The decoder refuses another magic or version (1 included), n = 0 or > 32, a size that does not match the items
  exactly or is over 512 bytes, an unknown op, a slot / action out of range (slot > 7), nrtpc > 12, ids an op needs
  that are 0, fields an op does not use that are not 0, age > 2,000 ms, fade > 10,000 ms, category > 5, a post / rtpc
  item on slot 7 whose category is not vehicle (or the other way round), a flag bit other than bit 0, `kSoundHeld` on
  anything but a vehicle post, an RTPC id 0, and a non-finite or huge (|v| > 1e7) value; a refused frame leaves the
  output untouched (`TestSoundFrames`, with a version 2 vehicle round trip and its refusals). STP1, STJ1, STN1, STS1
  and the gear decoders refuse it, and it refuses theirs (tested).
- **Delivery.** A FRAME message in the pose datagram (STP1 + STJ1 + STA1 + STN1); when it does not fit there it goes
  in a datagram of its own right after. Unreliable (a lost footstep is better than a late one). *(changed 2026-11-18)*
  The link drops duplicates and frames more than 64 seqs behind the newest (a bitmap, u16 wrap-safe) but keeps a
  reordered frame (its stops matter); the receiver counts gaps as lost and takes a late frame back off that count.
  Stops are sent twice more in the next frames (idempotent on the receiver). The sender checks every item alone
  (`wire::SoundItemValid`, the decoder's own rules) and drops an invalid one, not the frame. `link::Info` counts STA1
  frames sent (and those that went alone) and accepted. The format did not change then (version 2 since: section
  22.9).
- **What the receiver does with it** *(2026-11-18, audio-research.md section 17.4)*: a post or action is carried out
  only for an event id on its allow-list (seeded ids plus the ids its own game posted on its own player's allowed
  objects); stops and actions are never dropped for timing; `partner_volume` scales its objects' output.
- Mixed builds never get this far: STA1 lives in `pose_wire.hpp/.cpp`, part of the build id's wire hash (section 4).
- **Budget** [inferred from the run counters]: 3-4 posts a second while moving, 24-80 bytes each (movement-layer posts
  carry 7 RTPCs): about 0.2-0.3 KB/s per direction next to the pose stream's ~17 KB/s.

## 20. Headgear on the character: an offset per item (`session.ini self_headgear`, default on)

**Status (2026-11-18): works in hidden runs, solo, echo and two games.** The user's report on v0.11: "glasses still
not in the right spot, hats too feel a little high up and when starts walking it moves weirdly."

### 20.1 Why (the v0.11 head lift)

- One lift cannot fit both items [V data, I interpretation]. Fragile's eye-lid joints sit 1.1 cm higher above her head
  joint than Sam's (each face mesh's inverse bind matrices); her scalp and hair need about 5 cm for the cap. The lift
  (section 16.1, measured 6.1 cm) moved every head item by the same amount: the glasses sat on her forehead, the cap
  about 1 cm high with a band of hair under it.
- The lift lived in the neck joint's frame (it was added to joint 31's local translation), so the head items pivoted
  about a point 6 cm off her real head pivot. Measured in her drawn head joint's frame every tick (the new meter,
  run `20261118-105952-self-headgear`, uncapped): standing the cap sat at (-0.3..-1.0, 6.0, 0.1) cm (x, up, forward:
  about 1 cm to one side); in the first second of a walk its mean moved by up to 0.9 cm sideways and 0.8 cm
  forward / back, spread (sd) up to 0.7 cm [V]. On the other screen the sender measured the item against the head
  without the lift (section 16.1), so the relative pose changed with the head's angle; it is frozen while moving
  and re-sent when standing, so the host saw it jump at walk start and stop: steps of 0.9-1.0 cm in the host's
  meter (run `20261118-111051-two-games-boots-headgear`, the guest with `self_view 10 0`) [V].
- Sam's cap and glasses are accessory entities (`ds/entities/player/ds_player_inventory`: eyewear0 `3b9eb11e`,
  glasses00-02 `d2306d68` `036f3017` `6482902d`, cap00-03 `c83ac923` `17de6d02` `dbd027af` `6247acf7`) hung on his
  only head helper `HLP_Head` (joint 31, zero translation; helper x = -head x, y = head local z (forward), z = head
  local y (up)) by an `AttachToHelperMover`.

### 20.2 What the mod does [V: decompiles, byte checks, runs below]

- The mover (`0x1423A7450`, entity `+0xB0`, `+0x48` its entity, `+0xB0` the helper name) combines the helper's world
  transform with a per-instance local matrix at `+0x60` (a Mat44, rows x, y, z, then the translation at `+0x90`;
  identity from the constructor `0x14239B930`) unless the byte `+0xA0` is set (`0x1423A7773`: `cmp byte
  [rsi+0xA0],bl; jnz; lea r8,[rsi+0x60]; ... call 0x14195DF70`, byte-checked at first use). `0x14195DF70` multiplies
  the local by the helper's rotation (`0x1419333D0`), so the local translation is in the helper's frame: confirmed
  in the runs (an offset of (0, -1.2 forward, 5.0 up) cm reads (0, 5.0 up, -1.2 forward) in her head joint's frame).
  The game's accessory movers have `+0xA0` = 1 and the identity.
- partner_gear's entity walk (100 ms) finds our Sam's shown accessories by resource GUID and checks each mover (class
  `AttachToHelperMover`, back pointer, helper `HLP_Head`). Every tick (game thread) each mover's translation is set to
  the offset `realbody::SelfHeadItemOffset` asks for, with `+0xA0` cleared (the translation first, then the flag), or
  put back (the flag back first, then the translation zero) when the self view, the fit or the key goes off or the
  item is taken off. Nothing is written when it already holds the value; a mover whose matrix is neither the
  identity nor ours, or whose flag the game changed, is left alone (`refused`, `changed by the game`: 0 in every run).
  The resource is never written (the partner's props and every copy share it). No new game call or hook.
- The offsets are per self body head rig (`kHeadgearRows` in `real_body.cpp`; fragile and fragile_suit share
  `fgl_head_def`; since v0.19 one offset per item, section 26.3): **cap (0, -1.2 forward, 5.0 up) cm, glasses / eyewear (0, -0.4, 1.1) cm**, chosen from 2560x1440
  front and back close-ups of 4.5 / 4.8 / 5.0 cm (the lower two let hair strands through the cap's band from behind;
  5.0 cm with 1.2 cm back is snug, brim on her brow, eyes free) and 0.7 / 1.1 / 1.5 cm for the glasses. A body
  without a row keeps the head lift.
- **The lift goes only while the offsets are really written** (review follow-up). partner_gear tells realbody every tick
  whether it writes them (`realbody::SetHeadgearReady`): its site check passed, `gear 18` on, no shown head item of ours
  refused, and no other shown entity of ours hung on `HLP_Head` (an accessory not in the list above: `[headgear] our
  entity ... hangs on HLP_Head but has no offset of its own`). The lift is 0 only while that was said within 500 ms;
  otherwise no offset is written and the v0.11 lift holds every head item up. So `partner_gear=0`, a failed byte check,
  `gear 18 0`, a refused mover or an unknown head item give v0.11's placement, never items sunk 6 cm into her head.
- **Emergency stop**: after a fault in the per-frame sync `gear::EmergencyStop` puts every mover of ours back (the identity
  and the game's own `+0xA0`, only where the mover still holds exactly what we wrote), so the real Sam's cap and glasses
  sit where the game puts them.
- With the offsets on, the head lift is 0 (her head joint is Sam's head joint), so `realbody::HeadLift` is false and
  the gear sender measures each head item on that joint with the offset in it: the relative pose is a constant and
  the other screen places the prop exactly as on this one (no wire change, no re-sends while walking).
- Side effect: the third-person camera, which follows joint 31, no longer gets the 6 cm lift (it sits at her head's
  height) [I: not compared in pictures].
- Controls: `self_view 8|9 <item> <mm>` (tuning), `self_view 10 <0/1>` (offsets / v0.11 lift), `gear 18 <0/1>` (the
  mover writes), `gear 19 <0/1>` (meters). Logged: `[headgear] our <item> <entity>: mover <p> AttachToHelperMover helper
  'HLP_Head'`, `... mover offset set to / back to the identity ...`, the counters on change, the `[self] fit` line's
  `headgear on row ...`, and the meter every second, in hidden tests only by default (`gear 19 1` in play): `[headgear] self <item> in
  her drawn head joint's frame cm (x, y up, z forward): mean ... sd ... step max ...` and `[headgear] partner head prop
  <k> in the partner body's head joint's frame ...` (sampled at the end of the gear tick, after the props were
  placed from the bodies' current transforms and poses).

### 20.3 Evidence (hidden, `-SeedSaves complete-100`, all pass, exit 0, no error.txt, real saves unchanged, 0 uploads)

| Run (`build-testenv\runs\`) | What it shows |
|---|---|
| `20261118-105952-self-headgear` (solo, 2560x1440) | `02-front-cap-before/-after/-v48-8/-v50-12`, `05-back-cap-*` (the hair through the band at 4.5 and 4.8 cm, none at 5.0 / -1.2), `03-front-both-*`, `04-front-glasses-before/-after/-v7/-v15` (before: the glasses on her forehead, eyes below them; after: the lenses on her eyes), `07-walk-before-*` / `08-walk-after-*` (the cap perched high in v0.11, snug now). Meter: v0.11 walking: mean x -0.05..0.90, forward -0.84..0.73, sd up to 0.72 cm; now sd 0.00 and step max 0.00 standing, walking, running uncapped and at 30 fps. |
| `20261118-104633-self-headgear` (solo) | the first build (cap 4.5 cm): mover writes 8, restores 8, refused 0 after the `+0xA0` fix; the same meter result. |
| `20261118-110748-echo-boots-headgear` (echo, our self view and a fragile_suit echo partner 1.2 m beside us, 2560x1440) | the partner's cap prop at (0.00, 5.00, -1.20) cm in its head joint's frame, sd 0.00 standing, walking and at 30 fps; `02-front-both`: cap and glasses on the partner. (Run `-110515`, the same scenario, had the meter before the props were placed: it read 2-24 cm "lag" that the corrected meter shows as 0.0-0.1 cm.) |
| `20261118-111051-two-games-boots-headgear` (host Sam, guest fragile_suit, 1920x1080 at 30 fps) | Host's meter of the guest's cap: v0.11 (guest `self_view 10 0`) mean (-1.0, 6.0, -0.15..0.30), jumps of 0.9-1.0 cm at walk start / stop; now (0.00, 5.00, -1.20), sd 0.00, step 0.00. `02-host-sees-guest-cap-before/-after` (enlarged: perched / snug). |

### 20.4 Not covered / still different

- Only Fragile's head rig has a row; other self-view characters keep the lift (with its slide).
- The camera height change was not compared in pictures. The meter is sampled at tick time, not at render time.
- The glasses were judged from front views with her head lowered in the idle (no side close-up of the lenses).
- Review correction: in `20261118-105952-self-headgear` the walk frames (`08-walk-after-*`), the 30 fps run and the meter
  lines were at the earlier cap offset (4.5 cm up, 0.3 cm back: the scenario put that back after each tuning variant);
  the final 5.0 / -1.2 cm were walked in the two-game and echo runs only. The scenario now restores the shipped values;
  the final runs are in 20.5.
- When a head item is put on, the first tick after the game shows it can draw it once without its offset (the entity
  walk that finds it runs every 100 ms): the meter's first second after `gear 14 15 1` reads one tick of four at about 0
  cm instead of 5 cm (step max 5.1 cm). One frame, then constant; not seen in a picture [V: meter; I: visibility].

### 20.5 Final runs (the shipped build `0.11.0+fbd9ce-92`; hidden, `-SeedSaves complete-100`, pass, exit 0, no error.txt, real saves unchanged, 0 uploads)

| Run (`build-testenv\runs\`) | What it shows |
|---|---|
| `20261118-125307-self-headgear` (solo, 2560x1440) | The shipped offsets restored after each tuning variant: the self cap meter reads (0.00, 5.00, -1.20) cm, sd 0.00, step 0.00 in the 15 one-second windows of the walk and the 30 fps run (114-128 s; the 112 s window is still the v0.11 mode and 113 s is the switch-on, a 5.14 cm step) (v0.11 in the same run: mean x up to -0.96, sd up to 0.53, step up to 2.0 cm). `08-walk-after-c` (enlarged): the cap snug while she walks. |
| **`20261118-130839-two-games-release`** (host Sam, guest `fragile_suit`, launcher defaults, 1920x1080) | Host's meter of her props: cap (0.00, 5.00, -1.20), glasses (0.00, 1.10, -0.40) cm, sd 0.00 in 172 windows (only the window in which the glasses came on, a slot change, differs). Her own meter: the same values in 167 windows. `01/02-host-sees-guest-cap(-glasses)` (enlarged x4): cap on her head, lenses over her eyes; `09/10-guest-*` (her self view from the front after the camera turn, enlarged x4): cap snug, glasses on her eyes. |
| `20261118-130121-two-games-release` (the same scenario before its camera fix) | The same meters (cap and glasses sd 0.00 while she walks, on both screens). |

## 21. Boots: her feet scaled away, the equipped boots on them (`self_feet_hide`, `boots_sync`, default on)

**Status (2026-11-18): works in hidden runs, solo, echo and two games.** The user's idea: "scaling the feet bones to
hide the feet then attaching the boots".

### 21.1 Scaling the foot joints [V: decompile `0x141D8AA20`, runs below]

- The model-matrix step multiplies each joint's rotation rows by its local scale (joint `+0x20`) and then by the
  parent's model matrix, with no compensation: a foot's scale reaches the toe and `MTP_*_Foot_a`, and the foot's own
  position (row 3) is kept. The bounds step transforms bone boxes with no inverse. So two `s` values in the pose
  `ApplyBody` already writes are enough: **the body joints mapped to Sam joints 63 and 68 (`JNT_R/L_B_102_Foot`) get
  scale 0.01** (a percentage, `self_view 11` / `gear 20`; 1 % rather than 0 keeps normals finite). Only the drawn
  body's pose: never the latch, the fit's shape, the capture or the wire; Sam's own skeleton is never scaled.
- Her porter suit is one mesh (`fgl_body_suiti`: `mesh_body_lx`), so the scale is the only way to hide her shoes. In
  every picture the shoe vanished inside the equipped boot (front, side, walking, running); no lump at the ankle was
  seen (the pose deformer's `FootS_PRO` helpers did not leave one) [V for the views looked at: front standing,
  stepping; side views only at a distance].
- Mod reads of a foot joint: `realbody::JointWorld` (partner sound foot emitters) now uses the joint's position only
  (row 3), so it keeps working under the scale; the new boots meter uses positions only too.

### 21.2 Self view: under Sam's own boots (no wire change)

- The game draws Sam's equipped boots (inventory `+0x4D80`, `DSPlayerBootsComponent`, copied from his fitted
  skeleton) on her legs already (section 16.1). Her feet are scaled while the self body is fed from the field Sam
  (not the cargo menu's figure), the fit is on and the game shows the boots entity (its visible flag; read every 100
  ms, `0x1427F1D2F` / `+0x1B` / `+0x25` byte-checked: `[boots] the inventory's boots entity sites verified`).
- Meter: Sam's boots' feet (their skeleton's joints 4 and 8) against her drawn feet: 0.0 cm mean and max standing,
  walking, running uncapped and at 30 fps (after the first second of a show).

### 21.3 The partner: STC1 version 4, the boots prop

- **Sender.** While boots_sync is on and the game shows our boots entity, the gear frame gets one item of kind 7
  `boots` on joint 0 with the identity pose, and the boots block: which of the boots' 32 ArtParts the game asks shown
  (`+0x2C` bit 1; 2 of 32 in the 100% save: `80010000`). The boots' look rides STV1 like an entity's (the look policy
  allows the prop's own, our player's and our own gear's components).
- **Wire** (`pose_wire`, ctest `st_net_tests` `TestGearFrames`): version 4; flags bit 3 `kGearBoots`: `u32 apply | u32
  visible` after the backpack's block (8 bytes; the largest frame is now 1080 bytes). The decoder refuses version 3,
  visible bits outside apply, a boots item without the block, the block without a boots item, more than one boots
  item, and a boots item not on joint 0 or with a parent; the encoder refuses the same. Mixed builds never meet (HELLO
  build check, section 4).
- **Receiver.** Kind 7 is a fixed prop, `ds/entities/cinematics/demo_apv_sam_foot_def_v01` (`7e71552e-bf48-464d-8628-
  fd9f0afc70cd`): the cutscene copy of the boots entity, the same `part_sam_foot_def` ArtParts on the same 10-joint
  skeleton (layout `0x9B68A8AD`), no gameplay component. It is spawned, shown and placed like every gear prop (on the
  body's joint 0 frame). `realbody::SetBootsProp` checks it (10 joints, the layout, each joint's parent `-1 0 1 2 3 4 1
  6 7 8`, a body joint for each of Sam joints 0, 1, 61-64, 66-69) and then, in the prop's own model update (the pose
  hook), writes the partner body's local joints as the body's own update composes them at that moment
  (`ComposePartner`, the same interpolation) with an identity root, and reruns the model-matrix and bounds steps:
  its model matrices are the body's legs, as `DSPlayerBootsComponent` (`0x1427C4880`) does for Sam. A prop the game
  stops updating gets the forced update; a fault turns it off for the session. Its 32 parts are set as the sender's
  through the existing parts setter (checked against our own boots entity's parts: `parts mirrored`).
- **The prop shows only once it is posed and its parts are mirrored** (review follow-up): `BootsTick` makes it the pose
  target 300 ms after it is live (shown or not), and the gear set shows it only while a pose was written onto it within
  250 ms and its parts are mirrored (`BootsPropReady`). Before, it was shown from its first placement in the cutscene's
  own pose and parts, which the meter caught as 12-14 cm at every spawn (the review: `110748`, `111051`, `111356`: a
  pop of a frame or more, not meter timing). Without our own boots entity as the parts' reference (barefoot, or the
  inventory unreadable) the parts are not mirrored, so no prop shows and the partner keeps its own boots.
- While the prop is shown, the partner body's feet are scaled (`gear 20`); otherwise its own boots show. Before the prop
  is removed its pose target is cleared and the feet come back.
- Meter (only while the prop is shown): the prop's feet against the partner body's feet: 0.0-0.1 cm mean and max (echo
  standing, walking and at 30 fps; the host's view of the guest at 30 fps; 0.6-0.7 cm max while she walks in the
  two-game run). Pose writes: 0 refused, 0 torn; forced updates 0 in the boots runs, but 277 (guest) / 286 (host) in the
  longer `20261118-113842-two-games-audio` (the game stopped updating the prop at times; the forced update covered it).

### 21.4 Evidence (hidden, `-SeedSaves complete-100`, all pass, exit 0, no error.txt, real saves unchanged, 0 uploads)

| Run (`build-testenv\runs\`) | What it shows |
|---|---|
| `20261118-105952-self-headgear` (solo) | `01-front-feet-kept/-1/-30`: kept, her shoe's toe pokes through the front of Sam's boot (enlarged ×6: a second rounded toe and the gold toe piece cut); scaled, the boot is clean at 1 % and 30 %. `07-walk-*` with her feet kept, `08-walk-*` and `09-run30-*` scaled. Boots meter 0.0/0.0 cm. |
| `20261118-110748-echo-boots-headgear` (echo, fragile_suit partner) | `01-front-noboots` (the partner's own dark boots, v0.11), `01-front-boots-feet-kept` (the prop over her own boots: her toe through it), `01-front-boots-feet-1` (the gold-trimmed boots, clean), `03-side-*`, `04-walk-*` (both bodies' boots while stepping). |
| `20261118-111051-two-games-boots-headgear` (two games, 1080p) | Host's view of the guest (enlarged ×3.5): `03-host-sees-guest-noboots` dark boots, `-boots-feet-kept` / `-feet-1` the gold-trimmed boots; the guest's view of the host's Sam body: the prop live (`parts mirrored`, 0 refused) but his feet were behind her backpack in the shots. |
| `20261118-111356-echo-boots-sam` (echo, a partner Sam body) | `01-front-noboots` demo_sam_clean's own default boots; `-feet-1` the equipped boots on its feet; a gold patch of its default boot's ankle (weighted to the shin, so not scaled away) still shows on one foot. |
| **`20261118-130839-two-games-release`** (final build, two games, 1080p) | The prop's spawn after the review fix (`@guest gear 21 0` then `1`, frames 250 ms apart, enlarged x4 in `20261118-130121`): her own dark boots until the prop is ready, one frame where the prop fades in over the same feet, then the gold-trimmed boots; no frame of the prop in its own pose or parts, and the meter's first second after each spawn reads 0.0/0.0 cm (before the fix: 12-14 cm). Over the run: prop vs feet 0.0/0.0 cm in 150 of 177 windows, at most 0.1/0.3 cm; 2554 pose writes, 0 refused, 0 torn, 0 forced updates. Her self view: Sam's boots on her feet, 0.0/0.0 cm in 90 of 91 windows (the first: 1.7 cm max while the self view came up). `04-guest-self-walk` (enlarged x4): the host's Sam body in the equipped gold boots on her screen (the first shot of that; before, his feet were behind her backpack) and her own foot in Sam's boot, no toe through it. `10-guest-after-host-left` (her front, x4): both boots on her feet; gold bits of the boot's top show through her trouser hem at one shin [V at x4, low resolution]. |

### 21.5 Not covered / still different

- A partner **Sam** body: part of its default boot that is skinned to the shin (a gold patch at the ankle) still shows
  through; hiding that body's boot parts through the parts block (section 14) was not done.
- The boots are Sam-sized on her (wider shafts). Wear (MeshPaint) is not mirrored: the prop looks clean.
- Joint-anchored gear on the feet (none seen) would be skipped by `PlaceGear` under the scale (axes too short).
- Falls, ladders, vehicles and the cargo menu's figure were not run with the feet scaled (the cargo menu's figure is
  not fitted, so it never scales).
- The boots' shafts are wider than her trouser hems: in the final run's front view gold bits of the boot top show
  through the hem at one shin (1080p, enlarged).
- Emergency stop (a fault in the per-frame sync): the boots prop stays where it is (like every gear prop) and the
  partner body's feet are no longer scaled by us; whether the game keeps the last scale on that body until it updates
  it again was not checked [I]. Never seen in a run.

## 22. Vehicles: the partner's vehicle under its body (`session.ini vehicle_sync`, default 1: solid trike and truck copies, collision off on each; no roadster since M2)

With the host-held save session the default is the real shared vehicles (`vehicle_sync=5`, section 23): no copies at all. This section's copies are their fallback, and since M2 (decision D2-1, "no hologram vehicles") the default modes show no roadster and no hologram (the holograms only in the opt-in modes 3 and 4).

**Status (after v0.13, 22.13: the final review of 22.11 / 22.12).** Every copy, pack and cargo prop, and now also every
gear prop (the case props on the partner's back, weapons, boots, the pod), has its collision turned off on its own entity
and checked again every second; they are spawned away from the watcher; a parked vehicle stays on the other screen as
long as its driver stays within 250 m of it (no 60 s limit); a standard trike followed by the Kusanagi (or back) respawns
the trike copy in its own look; the gauge colour is read back too. Final runs on one build (all pass): `20261118-205413`,
`-205827`, `-210052`, `-210439`, `-210748`, `-211341`, `-211715`. Still not done: a solid roadster (its hologram stays,
logged), the Kusanagi's own look (a plain grey trike), mud / snow, the lamps' light on the ground, damage.

**Status (after v0.13, 22.12): the solid copies wear the partner vehicle's wear and lights and carry its cargo.** STM1
version 3 carries the vehicle's timefall wear (the corrosion shader variable on every part it is set on), its lamps and
battery gauge (emissive intensity and colour per part) in every frame, and the pieces in its baggage carrier when they
change; the receiver sets them on the solid copy of the same art and draws the pieces as the gear's case props. The
100% save's gold truck, rust-brown on the driver's screen, is rust-brown as a copy now (v0.13: clean gold): runs
`20261118-201916` (echo; with the looks off the copy is the grey cutscene truck) and `20261118-202559` (two games). Not
possible / not done: the roadster (no solid copy: still its hologram), the Kusanagi's own look (no copy with its art),
mud / snow / tar (the mesh-paint texture), the headlight beams and boost particles; no real delivery cargo was loaded
onto a vehicle in a test (synthetic pieces prove the wire and the props; 22.12).

**Status (after v0.13, 22.11): the solid copies are the default again, made safe.** The trike (also for the Kusanagi)
and the truck are the solid cutscene copies with the partner's paint, variations and packs; each copy's collision is
turned off on its own entity before it is shown (`Entity::SetCollisionEnabled(false)`, the game's per-instance switch)
and checked every 250 ms, so players walk and drive straight through them and are never pushed or lifted (run
`20261118-194123`: walks 8.1-8.6 m past their origins, Sam at their origins 0.11 m up, our roadster driven through them
at 8-14 m/s without a speed or height change). The roadster stays its construction hologram (no solid roadster exists;
the art swap tried for it does not work, 22.11). `vehicle_sync=3` shows all three as holograms (opt-in). The text below
is history where it says the holograms are the default.

**Status (2026-11-18, review follow-up; 22.10): the default copies are now holograms for all three kinds.** A centred
walk showed that the solid trike and truck copies **have collision** (their cutscene bodies carry the drivable art's
PolySoup shapes): Sam stands on the solid trike and is stopped inside the solid truck, which breaks the rule that a copy
never interacts with local physics; the earlier "no collision" finding (run `155304`) came from walks that slid along
the copies' sides. The game's own construction holograms of the trike and the truck (`construction_holo_trike`,
`construction_holo_truck`) have the drivable skeletons' layouts and no collision shapes: they are the default copies
now, as the roadster's already was; Sam walks straight through both (run `20261118-181155`). The solid copies with
their looks and packs (22.7) are shown only with `vehicle_sync=2`. Also: the overlap hiding now puts the copy 20 m below
(`SetVisible(false)` did not stop the roadster hologram from drawing), the vehicle sound review findings are fixed, and
the receiver refuses a vehicle look with anything the sender does not send.

**Status (2026-11-18, fourth milestone): the partner's vehicle sounds play at its copy** (`vehicle_sound`, default 1,
with `sound_sync=1`; 22.9). The engine loop with its RTPCs (rpm, speed, throttle, boost, reverse...), the road noise and
skid loops, ignition, cut-off, suspension bumps, the indicator and the truck's body raise / lower reach the other game
as STA1 version 2 items and are posted on the watcher's own Wwise objects at its copy of the driver's vehicle. Proven
by logs in hidden one-game and two-game runs (the games are silent): for the roadster, the Kusanagi trike, the standard
trike and the truck; loops stopped on getting off, when the vehicle is no longer reported, at either side's pause /
menu, for a silent partner, and with the key off.

**Status (2026-11-18, third milestone): the solid copies wear the partner vehicle's paint and variations and show its
battery / defense packs** (`vehicle_look`, `vehicle_packs`, default on; 22.7). In hidden runs (one game and two games,
both directions) the 100% save's gold truck's copy turned gold with its six defense packs where it was the plain grey
cutscene truck before, the purple / orange trike's copy purple / orange with its two battery packs where it was grey. The
Kusanagi trike's look is not applied (its copy is the standard trike's model), the roadster stays a hologram (22.8: no
safe route to a solid roadster was found). STM1 is version 2 (the packs block), STV1 carries the vehicle's look as item
255 (22.3).

**Status (2026-11-18, second milestone): the trike (also the Kusanagi trike) and the truck are proven with real drives
and are shown by default.** In hidden runs (one game and two games, both directions) each of the three vehicle kinds was
got on with the game's own X, driven, stopped and got off, and the other screen showed its copy with the driver seated in
it at the same place (placement error under 1 cm mean). The copy of a vehicle that overlaps the watcher's own vehicle of
the same kind is hidden until it has moved away (`vehicle_overlap_hide`, 22.4), the copy's wheels and steering move
every frame (22.4, "Smooth"), a silent partner (link stall, its load or its pause) no longer makes the copy vanish under
its seated body, and the sender only reads a vehicle it has left fault-free (22.2). The first milestone's text follows
where it still holds; the changes are in 22.1 to 22.4 and the evidence in 22.5.1.


**Status (2026-11-18, first milestone): works in hidden runs (one game and two games, both directions).** When a player
gets on a vehicle and drives, the other screen shows a copy of that vehicle with the player's body seated in it (the
body's seated pose already arrives in STJ1), moving where it is driven, then parked where it is left; the copy goes when
the player walks away (more than 30 m) or after 60 s. The reachable vehicle at the 100% save's start is the purple
roadster, and the roadster has no solid cutscene body, so on the other screen it is the roadster's **hologram**
(`construction_holo_roadster`); the trike and the truck get their solid cutscene bodies (proven only through the copy
override, below), so by default (`vehicle_sync=1`) only the roadster's copy is shown; `vehicle_sync=3` adds the trike's and
the truck's, `2` shows those two only (every value but 0 sends what we ride).

### 22.1 What the game does [V: byte checks at start, the runs below; I: inferred]

- **The vehicle is Sam's parent entity.** `[Sam + 0x70]` (Entity_ExportedGetParent `0x142361B80` reads `+0x70`) is the
  vehicle entity itself, a `DSVehicleSpeedCar` (vtable `0x143E274D8`), from the frame X is pressed (ride state 0, Sam
  still beside it: seat offset (1.59, -0.55, 0.10) m in the echo run, (2.59, -1.47, 0.05) m in `122017`), through getting
  on (state 1), riding (2) and getting off, until Sam stands on the floor again [V: `[vehicle] ours:` lines, runs below].
  No seat child was seen. So the frame's `seated` flag means "his parent is the vehicle", not "in the seat": at state 0 he
  still stands beside it (the body is placed from its own pose stream either way; only the seat meter reads the flag).
- **Class and kind.** Each class is told apart by its vtable; slot 0 is its RTTI getter (`lea rax, [rip+..]; ret`,
  byte-checked), and the constructors write the EVehicleType at `+0x4C4` (SpeedCar 1 `0x142F62201`, Truck 2
  `0x142F62571`, Motorbike 3 `0x142F61CFC`), which the game's own getter reads at `0x142FA73B6`. The ride state is
  `+0x4CC` (read at `0x142FA74B8`): 0 parked, 1 getting on, 2 riding, 3 getting off, 5 broken [V for 0, 1 and 2; I for
  the others].
- **Resource.** The vehicle's StreamingRef (`+0x58`) names `ds/entities/vehicle/pe_ds_vehicle_casp`, GUID
  `ff17815d-4a07-0840-9b23-d30f30269e7f`, a `DSVehicleSpeedCarResource` [V].
- **Pose.** `[vehicle + 0xB8]` is its SkinnedModel; its pose has 23 joints, layout `ECF43324` (the hologram's too), and
  its joints move while it drives (the wheels turn up to 180 degrees within 5 s, the steering, the suspension) [V:
  `meter` lines].
- The parked roadster is **not** in the World's entity list (82 entities; `action vehicle_sync 7`) [V]. World::AddEntity
  (`0x14357DFB0`) adds only entities of one RTTI range [V].
- **The vehicle registry** [V: decompile, byte checks, runs below]. `R = [ds.exe+0x7BFECC0]` (the vehicle manager,
  allocated in `0x142FAA2C0`): `R+0` i32 count, `R+8` `Entity**`, an SRW lock at `R+0x20` that the game takes (we read
  without it, twice, equal reads only, at most 256). The add-to-world handlers of all three classes register a vehicle
  (`0x142F75070`: push, then `[e+0x560] |= 2`), the remove handlers unregister it (`0x142F76180`: clear bit 2, then
  remove); the game's own nearest-vehicle search (`0x142F7DFD0`) loops over it. Byte-checked at `0x142F7509C` /
  `0x142F76312` (`mov rsi, [R]`, both lead to `0x147BFECC0`), `0x142F750A3`, `0x142F750BD`, `0x142F750D6`,
  `0x142F7630B`, `0x142F7E023`, `0x142F7E076`. The 100% save's world registers 10 vehicles in every run: the garage's
  roadster, trikes, the Kusanagi trike (`pe_ds_vehicle_bkyk`) and trucks outside Mountain Knot City, a roadster farther
  away (which trikes and trucks varies a little between runs; the Kusanagi at (-1191.2, 243.0) and the trucks at
  (-1214.8, 257.4) and (-1212.3, 280.5) every time) [V: `registry n:` lines, runs `151227`, `151711`, `152146`].
- **The Kusanagi trike** (`bkyk_main_def`, skeleton `fe0cb1e3`) has the standard trike's layout `8DABA9B0`: 40 joints,
  the same names and parents, the same default pose of joints 0..39 [V: game data; the sender read layout `8DABA9B0` from
  it, run `152146`]. The allow-list now names it (it was 0, which only skipped the sender's layout check).
- **Ride state.** State 0 is also reported while Sam is still seated after stopping, just before he gets off; state 3
  was never seen. The `seated` flag (his parent is the vehicle) is the truth [V: `ours:` lines of every run].
- **Update rates.** The copies' resources ask for 14.99 Hz (`construction_holo_roadster`; the truck hologram too) and
  59.94 Hz (the trike and truck cutscene bodies) [V: data; the `[smooth] vehicle copy update frequency 1 (14.99 Hz) -> 6`
  line]. At 14.99 Hz the hologram's wheels changed about 15 times a second whatever the frame rate: the "stepping" of
  22.6 [V: the meter in 22.5.1].

### 22.2 Sender (game thread, the pose send tick)

`vsync::TakeSend`: `[Sam + 0x70]` checked against the three vehicle vtables (else its parent's parent, for a seat child;
anything else is logged once by type and not sent). Read fault-free: the kind (`+0x4C4`, must match the class and the
resource), the ride state (`+0x4CC`, 0..15), the resource GUID (must be on the allow-list below), the world transform
(the game's getter), Sam relative to it (the seat, within +-4 m) and its local pose (`look::ReadPose`: read twice, equal
reads only; the layout must be the resource's). After Sam gets off the same vehicle is still sent, as parked (seat 0),
while its vtable and per-creation GUID still check, Sam is within 30 m of it and for at most 60 s. One STM1 frame per
send tick (30 Hz) in a datagram of its own, on the pose's `sender_ms` axis.

Second milestone (robustness; no wire change):
- **A vehicle Sam has left is only read fault-free.** It may have been destroyed or unloaded: it must still be in the
  game's vehicle registry with its registered bit (22.1; when the registry's bytes differ, the vtable and GUID checks
  alone, as before), and its transform is read from `+0xC8` / `+0xE0` (two equal reads, finite), never through the
  game's getter (which locks the entity's critical section). While Sam rides, the vehicle is his parent this tick and
  the getter is used as before [V: `our last vehicle ... is gone (not in the vehicle registry)` after it was disposed of,
  run `155610`].
- **Forgotten on a load.** The vehicle is remembered with the local Sam it was ours with; a new Sam (a load) or no send
  tick for over 1 s while he is off it (he slept: a load, a menu) forgets it. One he still sits on is found again through
  his parent.
- **Broken.** A broken vehicle (state 5) Sam is off is reported 3 s more (the other screen drops its copy after 3 s
  broken), then forgotten [I: not provoked in a run].
- **Hidden tests only:** `action vehicle_sync 10 <ms>` holds all our sends (pose, joints, vehicle, gear, ...) for that
  long while the link stays up (`vsync::SendMuted`, automation runs only): the other side sees a silent partner.

### 22.3 Wire: STM1 (`pose_wire`, ctest `st_net_tests` `TestVehicleFrames`)

| Part | Contents |
|---|---|
| Header (84 bytes) | `'STM1' \| u8 version 2 \| u8 kind (1 roadster, 2 truck, 3 trike) \| u8 ride state \| u8 flags (bit 0 seated, bit 1 joints, bit 2 packs) \| u32 seq \| u32 sender_ms \| 16-byte resource GUID \| f64 x, y, z \| 6-byte rotation \| f32 speed \| i16 seat x, y, z (1 mm) \| 6-byte seat rotation \| u32 skeleton layout hash \| u8 n \| u8 m` |
| Joints (flags bit 1) | n x {6-byte rotation, i16 x, y, z (1 mm)}, n <= 64 |
| Packs (flags bit 2, version 2) | m x {u8 pack (1 battery, 2 defense) \| u8 0 \| i16 x, y, z (1 mm, in the vehicle's frame) \| 6-byte rotation}, m <= 8 (14 bytes each) |
| State (flags bit 3, version 3; 22.12) | `u8 nl \| u8 0 \| u16 wear (0..65535 = 0..1) \| u64 wear parts[2] \| nl x {u8 part \| u8 kind (1 intensity, 2 colour) \| f16 v[3]}`, nl <= 24 (20 + 8 nl bytes) |
| Cargo (flags bit 4, version 3; 22.12) | `u8 nc \| u8 dropped \| nc x {u8 model \| u8 variation \| i16 x, y, z (1 mm, vehicle frame) \| 6-byte rotation}`, nc <= 32 (2 + 14 nc bytes); sent on change and every 1 s, nc 0 = no cargo |

Third milestone (after v0.12): **version 2**, the packs block (22.7): the byte after n, 0 in version 1, is the pack
count m; version 1 frames are refused. The decoder also refuses the packs flag without packs (or packs without it), m
over 8, a pack kind outside 1..2, a pack's reserved byte that is not 0, a pack offset beyond +-8 m and a non-finite pack
rotation (ctest `TestVehicleFrames`). The truck with its six packs: 84 + 12 x 47 + 14 x 6 = 732 bytes (the largest frame
964, under the link body's 1148). The vehicle's look goes as **STV1 item 255** (section 13.3): kind `entity`, no
resource id, `layout` = the index of its drivable resource in the allow-list below (0 roadster, 1 trike, 2 Kusanagi, 3
truck, 4 MULE truck); the decoder refuses item 255 with another kind or an index of 5 or more, and every other item at
56 or more as before (ctest `TestGearLook`). Sent when it changes and every 2 s (68-124 bytes in the runs; on a new link session at
once).

Second milestone: no wire change; the Kusanagi trike's allow-list entry names its layout (`8DABA9B0`, ctest
`TestVehicleFrames`). Rotations are STJ1's smallest-three. The frame is 360 bytes with the roadster's 23 joints: while one is reported the
link payload went from 573-623 to 901-1009 bytes a send, about 10 KB/s more each way. The decoder (and the encoder)
refuse: another magic or version, a size other than 84 + 12 n, a kind outside 1..3, a state above 15, unknown flag bits,
the joints flag without joints or joints without it, n > 64, a non-zero reserved byte, a joint block with layout 0,
**a GUID that is not a drivable vehicle of that kind** (the allow-list: roadster `pe_ds_vehicle_casp`, trike
`pe_ds_vehicle_ca03` and `_bkyk`, truck `pe_ds_vehicle_ca02` and `_celd`), NaN or infinity anywhere, |coord| > 1e7,
|speed| > 1000, a seat beyond +-4 m (or any seat when not seated) and a joint translation beyond +-32 m. The receiver
never loads the GUID it is sent: it only picks one of three fixed copies by kind. Link: a queue of its own (8, newer-seq
filter, the generation counter; `link::SendVehicle` / `PopVehicle`). Mixed builds never meet (HELLO build check,
section 4).

### 22.4 Receiver

- **The copies** (`kCopies` in `vehicle_sync.cpp`, loaded through the bodies' path + GUID route,
  `realbody::BeginByPathGuid`):

| Kind | Copy | Checks |
|---|---|---|
| roadster | `ds/entities/npc/customize_holo/construction/construction_holo_roadster` `8b3517fe-...` (the construction hologram, `ForConstruction=1`: no fade near the player; docs/character-catalog.md #89) | the welcome hologram's checks (`holo::ValidateWelcomeHolo`), 23 joints, layout `ECF43324`; shown with StartVisible |
| trike | `ds/entities/cinematics/demo_bkst_main_def_pro_new` `161eefa7-...` (the drivable trike's art and Skeleton) | 40 joints, layout `8DABA9B0` |
| truck | `ds/entities/cinematics/demo_cald_main_def_game` `58c639c3-...` (the drivable truck's art and Skeleton) | 47 joints, layout `B609EC0D` |

  Every copy is refused when it is a Humanoid, player or vehicle entity, or carries an interaction, physics, ragdoll,
  controller, capsule, vehicle mover or bounder component (the solid ones also any AI component), or its skeleton is not
  the vehicle's: refused for the game run. A transient refusal (no pose yet right at realize, a 20 s load timeout, not
  shown) backs off 30 s, doubling up to 8 min, and is forgotten at a new link session; a set-transform fault refuses for
  the game run. A load that fails is retried after 10 s, doubling up to 80 s; a start that fails after 5 s, doubling up
  to 80 s (review follow-up: the count was reset before, so it stayed 5 s). `vehicle_sync=1` (default since the second
  milestone) and `3` show all three copies, `2` the solid copies only, `4` the roadster's only (the first milestone's
  default); the override `vehicle_sync 2 <n>` ignores the mode. A reported vehicle without a copy is logged once per
  reason (`the partner's <copy>: no copy (...)`). The Kusanagi trike's frames pose the trike copy (the same layout; it
  looks like the standard trike: no Kusanagi cutscene body exists). ~~The solid copies have no collision~~ (wrong, 22.10:
  the solid copies have the drivable art's collision; the holograms have none, and are the default since the review).

  Since the review two more copies (`vehicle_sync=1` / `3`, the default, use them for the trike and the truck; `2` uses
  the solid copies above):

| Kind | Copy | Checks |
|---|---|---|
| trike | `ds/entities/npc/customize_holo/construction/construction_holo_trike` `4ffc6e8c-e6fe-9a30-aecb-ae8472e1bc8b` (art `part_bkst_main_hlo`: 3 sub models, no collision shape; skeleton `bkst_main_hlo` with SkeletonLayoutHash `8DABA9B0`, the drivable trike's) | as the roadster hologram: `holo::ValidateWelcomeHolo`, 40 joints, layout `8DABA9B0`; StartVisible |
| truck | `ds/entities/npc/customize_holo/construction/construction_holo_truck` `53d0f9dc-e4ab-853a-b8d5-3ba6245a92d9` (art `part_cald_main_hlo_game`: 4 sub models, no collision shape; skeleton layout `B609EC0D`, the drivable truck's) | the same, 47 joints, layout `B609EC0D` |

- **Timeline.** Frames go into a ring of 160 on the pose ring's unwrapped sender axis; the copy is placed every tick at
  the **same play time as the partner body** (the STP1 ring's `PlayTime`), lerped / slerped between the bracketing
  frames (a jump over 25 m is not slid), with `Entity_ExportedSetWorldTransform`. Two frames more than 0.5 s apart on
  the sender's axis are not interpolated (the later one stands, and its kind picks the copy), and the ring is emptied
  when a frame comes after 2 s without one, so a new drive never starts from an old one's frames (review follow-up). The partner body is placed as before
  (the sender's Sam transform, which is the vehicle's times the seat); the meter compares the body with the copy's
  transform times the sent seat.
- **Pose.** When the frame's layout and joint count are the copy's (the roadster and its hologram), the interpolated
  joints go to the animate hook's pose target `realbody::kVehiclePoseTarget` (target 8; the gear keeps 0..7), written in
  the copy's own model update like the backpack's (section 13).
- **Lifetime.** Spawned 20 m below and invisible, shown after its first placement. Removed (its pose target cleared
  first) when: no frame for 2 s, the play time is more than 1 s past the newest frame (the sender stopped reporting), it
  has been broken (state 5) for 3 s, the partner has no body on our screen, our player has been asleep for 1 s or is
  gone (loads, cutscenes; the same 1 s as the partner body, so a menu's first frames do not respawn it), a new link
  session, the kind changes, or the key is turned off. After a fault in the per-frame sync the copy is hidden where it
  stands (`vsync::EmergencyStop`). The meters (seat, play delay, the copy's read-back pose) run in hidden tests and
  while a trace runs only. Local vehicles are never touched on the receiving side; since the second milestone they are
  read (the registry, below).
- **Silent partner (second milestone).** While the partner's pose is silent too (over 0.5 s: a link stall, its load or
  its pause), the copy is **held where it stands** instead of going at 2 s (the body stays up to 10 s): when frames come
  again it plays on (also while the emptied ring's first new frames are still ahead of the play time), and when the body
  goes after 10 s of silence the copy goes with it (`held where it stands` / `plays again` lines). Only when poses keep
  arriving without vehicle frames does the copy go at 2 s, as before.
- **Overlap with the watcher's own vehicle (second milestone; `session.ini vehicle_overlap_hide`, default 1).** Every
  250 ms while a copy is shown, the game's vehicle registry is read (22.1; fault-free, never written): when one of the
  watcher's own vehicles of the same kind stands within 1 m horizontally, 1 m up or down and 20 deg of yaw of the copy,
  the copy is hidden (`partner::SetVisible(false)`, as the emergency stop does; the solid copies' re-show is suspended),
  and shown again once the nearest such vehicle has been more than 2 m, 1.5 m in z or 35 deg away for 0.5 s. The
  partner body stays placed, so it sits in the watcher's own vehicle meanwhile. Both players starting from one save
  start beside the same vehicles, so this is the common case at the start of a drive. `action vehicle_sync 11 0|1`
  switches it at run time; `0` and the key off: the copy is always shown, as before.
- **Jumps.** Two frames are not one motion when they are further apart than 5 m plus 60 m/s over their spacing (a
  teleport, or the partner moving from one vehicle to another of the same kind: the frame names no vehicle instance): the
  later one stands, no sliding and no joint interpolation (logged `jumped ... not slid`). It was 25 m.
- **Smooth (second milestone; `smooth_pose` bits, as the partner body).** The copy is moved to the game's 240 Hz update
  group (bit `kSmoothBoost`, `realbody::BoostEntity`, checked every 500 ms and put back at its own rate without the bit),
  so its model is updated every frame, and its joints are interpolated **in its own model update** (bit
  `kSmoothDetourInterp`): the tick publishes the bracketing frames a, b and the frame after b (c) with the play time
  and a QPC reference; the update computes the play time at that moment and blends a-b or b-c (sign-fixed normalised
  lerp of the rotations, lerp of the translations), as the body's `ComposePartner` does. Without c the update often came
  after b at 30 fps and only held b (run `153007`: 77 of 77 writes held). The world transform stays per tick (the body's
  root is placed per tick at the same play time). Gear targets 0..7 are unchanged (no interpolation data).
- **Frame-aligned meter (hidden tests, trace).** Written in the copy's own update: per write, the largest joint turn
  since the previous write; counted only while the partner's vehicle moves (speed over 1 m/s): `meter: while the
  partner's vehicle moved, N frames (ticks) and M pose writes on the copy (M/N per frame), repeats, interpolated in its
  update, held at the newer frame; largest joint turn per write mean / sd / max`. `action vehicle_sync 13` logs it at
  once (a segment boundary).
- **Interplay.** Player collision (section 11.2) already takes the partner's capsule out while it rides
  (`kPoseInVehicle`). The hologram copies have no collision: a player walks through the partner's vehicle. The solid
  copies collided with the player in v0.13 (22.10); since 22.11 their collision is turned off on each copy (the default
  again). Name tag and gear ride the body as before.

### 22.5 Evidence (hidden, `-SeedSaves complete-100`, exit 0 (both games), no error.txt, real saves unchanged, 0 uploads)

Placement error: the copy's placement against the sender's own vehicle transform at the same play time (the
`vehicle_sync 3` trace; on one PC both games share the QPC clock; the echo adds its 4 m offset), over the ticks where the
vehicle moved.

| Run (`build-testenv\runs\`) | What it shows |
|---|---|
| `20261118-122455-vehicle-echo` (final build) | Our roadster mounted with X, driven about 28 m (up to 13.7 m/s), stopped, left; the echo copy 2 s behind: placement error 0.63 cm mean, 1.73 cm p95, 4.32 cm max over 364 moving ticks (0.07 cm mean over all 3396); body vs the copy's seat 0.0 cm mean, 0.1 cm max over 3170 ticks; the copy's pose writes: 0 rejects, 0 torn, 0 write failures, 0 faults; its pose read back equals one of the last 6 given exactly while still (0.00 deg), within 2-3 deg while getting on, up to 45 deg while the wheels spin fast (22.6). Frames: `10-as-truck-a/c` the truck override (`BRIDGES` side) at the parked place; `12-off` switched off at run time: the copy removed, no frame sent after (`1437 frames sent` before and after). |
| `20261118-121037-vehicle-echo` (the build before the last meter change) | `05-driving-a`: the real roadster and the copy moving side by side, the echo Sam seated in the copy. Placement error 0.67 cm mean, 1.69 cm p95, 9.38 cm max moving. The trike override (`09-as-trike-*`) is hidden behind the real roadster at every angle (a corner shows). |
| `20261118-120658-vehicle-echo` (the first working run; marked failed only by an optional `waitlog` with a wrong pattern, exit 0) | `04-mounted-copy` (enlarged): the hologram copy beside the real roadster, the echo Sam seated at its wheel; `07-look-b` (enlarged) the echo Sam in the copy close up, behind its windscreen, hands at the wheel, next to the real roadster with Sam in it. Placement error 0.49 cm mean, 1.26 cm p95, 4.72 cm max moving (a 5.6 m drive). |
| **`20261118-122017-two-games-vehicle`** (host Sam drives, guest Fragile watches; 1080p) | Guest's view, enlarged: `02-guest-sees-host-driving-b` the hologram roadster from behind, mid-drive, the host's Sam seated, `Host` tag; `02-...-c` / `03-...-stopped` at 26-27 m by the barrier; `04-...-off` Sam standing beside the parked copy; `05-...-after-host-left` gone after the host was placed beyond 30 m (`no longer reported (beyond 30 m)`, then `removed (the partner stopped reporting a vehicle)`). Host's own view: `01-host-mounted`, `02-host-driving-*`. Placement error 0.66 cm mean, 1.26 cm p95, 1.40 cm max moving; play delay 100 ms (`interp_delay_ms`); copy live 66 ms after it was asked for. |
| **`20261118-122223-two-games-vehicle-guest`** (the guest drives, the host watches) | Host's view, enlarged: `02-host-sees-guest-driving-b` the copy with Fragile (blonde) seated, `Guest` tag; `03-host-sees-guest-stopped` at 27 m, Fragile seated; `04-host-sees-guest-off` Fragile in her porter suit standing beside the parked copy; `05` gone. Placement error 0.68 cm mean, 1.43 cm p95, 1.90 cm max moving. |
| `20261118-121400-two-games-vehicle` (an earlier version: both drives in one run) | `01-guest-sees-host-mounted` (enlarged): the host's Sam seated in the hologram copy, overlapping the guest's own purple roadster at the same place (each game has its own). As watcher after its own drive the host looked at a wall (its camera turned by the drive): the scenario was split in two. Placement error 0.55 / 0.63 cm mean, 1.64 / 1.52 cm max. |
| `20261118-121812-two-games-vehicle` | The watcher behind its own roadster: the copy was hidden by it during the drive (the watching point moved since). |
| **`20261118-130839-two-games-release`** (final build, launcher defaults, host drives, guest Fragile watches; 1080p) | Guest's view, enlarged: `06-guest-sees-host-mounted` the hologram over her own purple roadster at the same spot; after her camera turn `07-...-driving-b` the copy from behind mid-drive with the host's head in it, `-c` and `08` the copy parked at 27 m, `09-...-off` the host's Sam standing beside it at 26 m, `10` gone. Placement error 0.54 cm mean, 1.29 cm p95, 1.54 cm max over 126 moving ticks (0.10 cm mean over all 678); body vs the copy's seat 0.0 cm mean, 0.1 cm max; copy live 65 ms after it was asked for; removed by "the partner stopped reporting a vehicle" after the host's `no longer reported (beyond 30 m)`. |
| `20261118-130121-two-games-release` (the same scenario before its camera fix) | `06-guest-sees-host-mounted` (enlarged): the host's Sam seated in the hologram, which overlaps her own roadster; `07-...-driving-a` the copy with Sam leaving her view at its left edge; the later frames face away (hence the camera turn). Placement error 0.53 cm mean, 1.27 cm p95, 1.94 cm max moving. |
| `20261118-115950`, `-120309`, `-120532-vehicle-echo` | Failed setups: walking to the roadster with the stick missed it (the probe walk's length varies), and the World's entity list holds no vehicle (`vehicle_sync 8`); hence `vehicle_sync 9`. |

### 22.5.1 Evidence, second milestone (hidden, `-SeedSaves complete-100`; every run below: exit 0 (both games), no error.txt, real saves unchanged and restored, 0 uploads)

Placement error as in 22.5 (`vehicle_sync 3` trace; one PC, one QPC clock; echo: minus the 4 m offset), over the ticks
where the vehicle moved; "outside the held sends" leaves out the receiver ticks from a `vehicle_sync 10` hold to 1 s after
it (the receiver cannot know where the vehicle is then). Two ticks at a change from one parked vehicle to the next one
got on (the trace follows the new one while the copy still plays the old) are left out where marked.

| Run (`build-testenv\runs\`) | What it shows |
|---|---|
| **`20261118-160727-two-games-vehicle-trike`** (final build, `vehicle_sync=1` default, `graphics low`; the watcher turns its camera onto the vehicle first: `vehicle_sync 18` + right-stick pulses until `camera aimed`) | Guest (Fragile) watches, enlarged: `00-guest-sees-host-on-roadster` the host's Sam seated in **her own** purple roadster, `Host` tag (the hologram copy hidden by the log, `overlap: ... hidden ... (0.00 m, dz 0.00 m, yaw 0.0 deg)`, **but still drawn**: grey and green hologram patches through her roadster, review 22.10; fixed since); `00-...-driving-a` the hologram copy driving past on her left with Sam in it, her own roadster still parked (`shown again ... 6.70 m, yaw 58.8 deg`); `03-guest-sees-host-driving-a` the grey trike copy with the host's Sam riding it (backpack and all, enlarged crop) next to her own red Kusanagi still parked. Host (Sam) watches the guest's truck: `10-host-sees-guest-on-truck` the `Guest` tag over his own truck (copy hidden), `11-...-driving-a/b` the BRIDGES truck copy driving away from his own parked truck. Held 3 s (`held where it stands ... 1105 ms old`, `plays again ... 6 ms old`), then 12 s (`[real] teardown (partner silent for 10 s)` and the copy `removed (no partner body)`, both back 2 s after the removal, about 0.1 s after the sends resumed). Placement: roadster 0.57 cm mean, 1.30 p95, 1.75 max; Kusanagi 0.23 / 0.61 / 0.93 (re-split by copy lifetime; 0.34 / 0.95 / 1.75 was the whole run) outside the held sends; standard trike 0.32 / 0.64 / 0.82; truck 0.44 / 0.99 / 13.2 (one tick). |
| **`20261118-155910-two-games-vehicle-trike`** (final build, defaults; the watcher's camera not turned) | All four parts pass: the host's roadster (garage), the host's Kusanagi trike, the guest's truck, the host's standard trike (backed out of its rocks with LT, then about 15 m). Each copy hidden over the watcher's own vehicle at the start and shown again at 4.1-6.2 m. Placement: roadster 0.58 / 1.44 / 1.91; Kusanagi 0.38 / 0.92 / 1.91 outside the held sends; standard trike 0.33 / 0.62 / 0.74; truck 0.40 / 1.04 / 1.28. The watcher's views mostly miss the action (rocks, a camera pitched at the ground); hence `160727`. |
| **`20261118-155610-vehicle-echo-trike`** (final build, role echo 0.5 s / 4 m, uncapped about 90-130 fps) | The frame-aligned meter on the hologram roadster, smooth mask 0 then 7: **0.14 writes per frame** (54 over 391 frames while it moved, the hologram at its own 14.99 Hz), largest joint turn per write **92.4 deg mean**, 39.2 sd, 164.2 max, against **1.00 per frame** (220 / 220), **5.30 deg mean**, 2.87 sd, 12.6 max (`[smooth] vehicle copy update frequency 1 (14.99 Hz) -> 6 (240 Hz)`). Kusanagi 1.00 per frame (100 / 100), 4.97 deg mean; truck 1.00 (501 / 501), 9.72 deg; no write held at the newer frame. All three got on with X: the standard trike (it barely moves against rocks), the Kusanagi (`ours: trike_kusanagi`, the trike copy `posed from the partner's vehicle joints (40), interpolated in its own update`), the truck. The truck then disposed of (hold START): `our last vehicle ... is gone (not in the vehicle registry)`, the copy removed. Placement: roadster 0.67 / 1.79 / 7.85; Kusanagi and truck 0.42 / 1.22 / 5.05 (2 switch ticks left out). |
| **`20261118-155304-vehicle-echo-collision`** (role echo, the garage's flat floor; the copy override shows the solid copies at the roadster's parked copy) | Sam placed before each copy along the camera's forward and walked into it: the **truck copy**: from 5.00 m before its origin to **10.71 m past it** (11 m walked in 3.3 s at full speed; `02-in-truck-copy` Sam inside its body); the **trike copy**: from 4.00 m before to **7.53 m past**; the control, **our own parked roadster**: from 4.99 m before to 0.68 m before, speed 0.02 m/s at the end (`05-at-own-roadster`: Sam against it, its Climb prompt). ~~The solid copies have no collision with Sam~~: the walks were not centred (Sam passed 2.04 m beside the truck's origin and 1.15 m beside the trike's: he slid along their sides); a centred walk proves the opposite (22.10, run `181155`). |
| `20261118-153007-two-games-vehicle-trike` | The first passing two-game run (Kusanagi host to guest, truck guest to host, overlap, the 3 s and 12 s holds). Placement Kusanagi 0.26 / 0.56 / 0.93 outside the held sends (inside them up to 2.5 m: the copy stands still while the sender drives on), truck 0.33 / 0.96 / 1.19. With two frames only, the copy's update came after the newer frame at 30 fps: 77 of 77 interpolated writes held there; hence the third frame (run `153645`: 0 held). |
| `20261118-153645-`, `-154036-two-games-vehicle-trike` | `11-host-sees-guest-driving-a` (`153645`, enlarged): the BRIDGES truck copy passing close by the host. `154036`: the truck copy driving to where the guest stopped, tilted as her own truck is against the wall it ran into (her `12-guest-stopped`); the standard trike part did not start (its `until` matched the Kusanagi's line; fixed). |
| `20261118-152146-`, `-151711-vehicle-echo-trike` | The first drives: the Kusanagi got on only after Sam was turned to face it (`151711` placed him beside it facing away: no prompt, 5 X presses). |
| `20261118-151227-vehicle-garage-scout` | The registry (10 vehicles, kinds, resources, positions) and the parking manager's 9 slots. The lift's buttons did not open the garage menu (Y, X, hold X; DPAD up opened photo mode): not needed, the save's world holds trikes and trucks. |
| `20261118-154549-`, `-154832-`, `-155051-vehicle-echo-collision` | Walks into copies parked outside: rocks and slopes, nothing proven either way (the truck copy stood on a rock slope; the controls slid round). Hence the garage run `155304`. |
| `20261118-152652-`, `-160341-`, `-160544-two-games-vehicle-trike` (failed scenario steps, games fine) | `152652`: the standard trike stuck against rocks, the host's cuff map (START) not closed by B, so the guest's part was refused (`HandcuffDevice`). `160341` / `160544`: camera pulses of 100 ms turned the camera about 1.5 deg/s (200 ms at full stick: 11-19 pulses). |

### 22.6 Not covered / still different

Second milestone (the first milestone's list follows, updated):
- **Not provoked**: a vehicle destroyed while ridden, falling off a trike, a real load or teleport mid-drive (the game's
  teleport refuses while Sam has a parent: debug-actions "Teleport", precondition 9). The silent-partner cases were run
  with held sends, which is what the other screen sees of a load or a pause; a disposed vehicle (hold START) ran the
  sender's "gone" path.
- ~~**The copies' look**~~ (third milestone, 22.7: the solid copies wear the paint and variations and show the packs;
  the Kusanagi trike's copy stays the grey standard trike, the roadster a hologram). ~~Engine and other vehicle sounds
  are not mirrored~~ (fourth milestone, 22.9).
- **Collision**: the hologram copies have none (Sam walks through them, 22.10). The solid copies collided with the
  player in v0.13; since 22.11 (the default again) their collision is off on each copy: walked and driven through.
- **Overlap** hides the copy only against the watcher's own vehicle of the same kind, within 1 m and 20 deg. A copy
  overlapping another kind (a truck over a parked trike) or a vehicle a little further off is shown.
- **The 3-frame interpolation** needs the frame after the bracketing pair; at the newest frame (play time at the ring's
  end) the copy holds it as before.
- **Two vehicles of the same kind** in a row are told apart only by distance (a jump over 5 m plus 60 m/s): two trikes
  parked side by side within a few metres would be slid from one to the other (STM1 names no vehicle instance).
- ~~**Parked for 60 s at most**~~ (22.13: while the driver stays within 250 m, no time limit). Watcher screenshots
  outside the garage depend on the watcher's camera: the scenario
  turns it with the right stick until `camera aimed` (`vehicle_sync 18`); on slopes the camera can still look at the
  ground (`155910`, part 3).

First milestone (still valid unless struck above):

- ~~Only the roadster was driven~~ (second milestone: all three kinds driven; the Kusanagi has the trike's layout and is
  posed; the garage's lift buttons tried did not open its menu, and it was not needed).
- **The roadster is a hologram on the other screen** (no solid cutscene roadster exists; a forged entity resource or a
  copy of the real `DSVehicleSpeedCar` was not attempted: the latter is the pattern that crashed with weapons, 17.1;
  third milestone: 22.8).
- ~~**Paint and variations**~~ (third milestone, 22.7, solid copies only; the hologram keeps its own look).
- ~~Wheel motion steps~~ (second milestone: the copy at 240 Hz, its joints interpolated in its own update: 1.00 pose
  writes per frame instead of 0.14 for the hologram). Above about 33-52 m/s a wheel turns over 180 deg between two 30 Hz
  frames and the short-arc blend turns it backwards [I: wheel radii 0.35-0.55 m; not reached in a run].
- ~~**Cargo on the vehicle, lights**~~ (22.12: wear, lamps, the battery gauge and the carrier's pieces on the solid
  copies). **Damage effects** are not mirrored (a broken vehicle's copy goes after 3 s broken; 22.13). The partner body's hands
  and feet are Sam's seated pose: on Fragile they may not meet the wheel exactly.
- **Not run**: two vehicles at once, carts and floating carriers (logged once as another parent type, not sent), long
  drives (the outside drives were 15-25 m: the terrain there is rocks and slopes). Link stalls (3 s and 12 s) and the
  partner's pause now hold the copy (second milestone).
- **No collision** with the partner's hologram copies (proven for Sam on foot, 22.10); ~~the solid copies collide~~
  (22.11: their collision is off on each copy, walked and driven through).
- ~~Two of the same vehicle~~ (second milestone: the copy is hidden while it overlaps the watcher's own vehicle of the same
  kind).
- ~~**Parked for 60 s at most.**~~ (22.13: reported while it is in the driver's vehicle registry and the driver stays
  within 250 m of it; a menu no longer forgets it. Only one copy at a time: when the driver gets on another vehicle the
  parked one's copy goes, while it still stands on the driver's screen.)
- The echo screenshots `04-mounted-copy` / `07-look-b` (`e04_copy.png`, `e07b.png`) are from `120658`, an older
  build than the final runs.
- The pose targets share one fault switch: a fault writing the copy's pose turns off every gear pose (the backpack's,
  the boots prop's) for the session, as a fault on any of them already did. Kept on purpose (a fault means our write
  hit something unexpected); never seen in a run.

### 22.7 Looks and packs: paint, variations and what is fixed on the vehicle (third milestone; `session.ini vehicle_look`, `vehicle_packs`, default 1)

Since the review (22.10) looks and packs act on the solid copies only, which are shown only with `vehicle_sync=2` (they
collide with the player); the default hologram copies keep the holograms' own look and carry no packs. The truck's look
is not the driver's either: the save's gold truck looks rust-brown on the driver's screen and clean gold as a copy (its
wear is not mirrored; below).

**The census** (`action vehicle_sync 19`, run `20261118-162226-vehicle-echo-census`; [V] for every line):
- **Paint is the colour index shader variable** `0x3A3714ED` (the cap's colour index, 13.1), set per ArtParts index:
  the purple roadster parts 4 / 6 = 19 / 21, the trikes parts 14 / 16 (15 / 21 the purple / orange one, 4 / 15 another),
  the trucks part 15 = 1 or parts 14 / 15 = 0 / 0. The `DSUIVehicleCustomizeColor.ShaderColorIndex` of the game's
  customiser [I: the names].
- **Variations**: one truck carries `f2d7a35c-...` (its resource's `GoldApv`, `+0x1F8`), one trike `4c407925-...` (the
  motorbike resource's `RideTypeApv`, `+0x288`); the others none. The resources' references (logged with offsets):
  `GoldApv` / `SilverApv` / `BrokenApv` / `BrokenGoldApv` / `BrokenSilverApv` at `+0x1F8..+0x218`, the motorbike's
  `RideTypeApv` / `BrokenRideTypeApv` at `+0x288` / `+0x290`, the truck's `HalflifeApv` at `+0x2B0`.
- **Cargo**: every vehicle's `DSBaggage2CarrierComponent` (the roadster and the trucks 2 slots, type 0 and 69 =
  `PassengerSeat`; the trikes 1 slot, type 0) holds **no piece** in the 100% save. What the save's trucks carry on their
  sides (the "containers" of 22.6) are **child entities**: six `ds/entities/vehicle/pe_ds_vehicle_defense_pack` or
  `pe_ds_vehicle_battery_pack` per truck, two battery packs on each standard trike (plus the wheel holograms,
  `pe_ds_vehicle_*_wheel_hologram`, not drawn while parked).
- The pack entity resources (game data): `EntityResource` + `SkinnedModelResource` + `StencilBufferComponentResource` +
  `MeshPaintTextureComponentResource` and a `DSVehicleCustomPackMoverResource`. The mover (`DSVehicleCustomPackMover`,
  RTTI `0x144CA5810`, vtable `0x143E252D8`, base `DSSimpleAttachToEntityMover`) handles only `MsgAddedToEntityManager`
  (`0x142FA8350` -> `0x142F71EB0`: it sets two shader variables on its own entity's ArtParts and its model's flag) and
  its base `MsgEntityInit` (the generic mover init `0x1423B5130`); no update of its own [V: RTTI message tables,
  decompile]. A copy has no parent: it stays where it is put (read back 0.000 m from the last placement in every run,
  also while the vehicle moved at 6-7 m/s).

**Sender** (game thread):
- **Look** (`look::ReadVehicle`, every 500 ms while the vehicle is reported this tick): its applied variations by GUID and
  its colour index entries (hash `0x3A3714ED`, part, value). Sent as STV1 item 255 when it changed and every 2 s.
  `0x32D5AE44` (per body part: 1.0 on the save's rust-brown gold truck, 0.35 on its clean vehicles) and `0x0978E0A1` also
  differ between vehicles; sent in one run (`164037`), they did not turn the truck copy rust-brown [V: screenshots], so
  they are not sent (meaning unknown; the rust is most likely the mesh-paint texture, `MeshPaintTextureComponent` [I]).
  (Corrected in 22.12: that run cut them at STV1's 32 entries, so only the wheels and lower parts turned rust-brown;
  `0x32D5AE44` is the timefall wear and all 39 entries make the copy rust-brown. They now go in STM1's state block.)
- **Packs** (`ReadPacks`, every 500 ms while the vehicle stands still: their place on it is fixed, and a moving vehicle
  and its children are not read in the same instant): its child entities whose resource is one of the two pack
  resources (path and GUID), shown (flag bit 1), each in the vehicle's frame (within +-8 m); at most 8. Sent in every
  STM1 frame (flags bit 2).

**Receiver**:
- **Look** (solid copies only): applied when the look of the vehicle being played (its resource is the frame's) arrives
  or changes, 0.5 s after the copy is live, and checked every 2 s (`look::Satisfied`; applied again when the copy's own
  components changed it: 0 in every run). Only on the copy whose art the resource uses: the trike (`pe_ds_vehicle_ca03`)
  on the trike copy, the trucks (`ca02`, `celd`: both `part_cald_main_def_game`) on the truck copy; the Kusanagi
  trike's look is not applied (`its look is not applied (copy 'trike' has another model)`), nor any on the hologram.
  `look::ApplyVehicle` is section 13.4's apply (the game's `ApplyArtPartsVariation`, component removal, shader variable
  create / exists / add / set, byte-checked) with its own scope: a variation GUID is resolved only from the copy's own
  component resources and from our own registered vehicles (22.1's registry) **of the sender's drivable resource** (their
  resource's references and their variation components); not found: left out and logged. The copy's own variation
  (the trike copy's `apv_bkst_main_def_v01`) is removed when the sender's vehicle has none.
- **Packs** (solid copies only): one copy of the game's own pack entity per sent pack (`realbody::BeginByPathGuid` with
  the pack's path and GUID: `pe_ds_vehicle_battery_pack` `e31fcbb8-1ce3-5549-a7df-c0bc519504a7`, `pe_ds_vehicle_defense_pack`
  `a697fc82-7564-6448-870e-4804c472104b`), at most 2 started a tick, spawned 20 m below and invisible, refused for the
  game run unless a plain `Entity` with no interaction, physics, ragdoll, controller, capsule, vehicle mover, bounder,
  AI or sound component; placed every tick at the copy's transform times the newer frame's pack transform and shown
  with the copy (hidden with it while it overlaps our own vehicle); removed with the copy, when the sent packs change, or
  when `vehicle_packs` is turned off. A set-transform fault refuses that pack kind for the game run.
- `action vehicle_sync 14 0|1` / `15 0|1` switch looks / packs at run time (looks off: the copy is respawned in its own
  look).

**Not done**: cargo in a vehicle's carrier (no piece in the save to prove a mirror with; the carrier walk is in the
census); the rust-brown wear of the save's gold truck (its copy is clean gold); the mesh-paint mud and snow; the
roadster (hologram) and the Kusanagi trike (another model) keep their copies' own look; burst-mode lights; parts the game
switches itself (battery level lights). ~~The packs' collision was not tested (their resources have no physics
component).~~ (22.11: each pack copy carries one physics instance; turned off on each.)

### 22.7.1 Evidence (hidden, `-SeedSaves complete-100`; every run: exit 0 (both games), no error.txt, real saves unchanged and restored, 0 uploads)

| Run (`build-testenv\runs\`) | What it shows |
|---|---|
| **`20261118-164704-two-games-vehicle-look`** (final build; host Sam, guest Fragile with self view, `graphics low`) | 1. The host drives the save's gold truck (six defense packs): the guest's copy `look ... applied` (1 variation, 1 colour; 4 of her own trucks as sources), hidden over her own truck at the start, 6 pack copies placed (read back 0.000 m at 7.2 m/s). `guest\automation\02-guest-sees-host-driving-a` (enlarged): the copy driving away **gold with the defense packs** on its side, next to her own parked rust-brown truck; `03-guest-sees-host-stopped` (enlarged, camera turned onto the copy: `vehicle_sync 18 5`): the gold copy 61 m off where the host stopped it, tilted on the rocks below the road, as his own truck in `03-host-stopped-side` (rust-brown). 2. The guest drives the purple / orange trike (two battery packs): the host's copy `look ... applied` (4 colours), 2 battery pack copies; `07-host-sees-guest-stopped`: Fragile (`Guest` tag) on a **purple / orange** trike copy, his own parked trike of the same colours at the left, against her own view `guest\automation\07-guest-stopped-side`. 3. The host on the Kusanagi trike: `the partner's trike_kusanagi: its look is not applied (copy 'trike' has another model)`. Status: 0 looks failed, 0 partly, 0 again, 0 place faults, no refusal, no fault. |
| `20261118-164347-two-games-vehicle-look` (the same build before the camera turn onto the copy) | The same applies and packs; the watchers' cameras showed their own vehicles, not the copies after the stop. |
| `20261118-164037-two-games-vehicle-look` (`0x32D5AE44` / `0x0978E0A1` also sent: 32 shader variables) | `02-guest-sees-host-driving-a` (enlarged): the copy gold, not rust-brown: those variables do not carry the rust; hence not sent. Failed at a scenario step (the host's camera aim count), games fine. |
| **`20261118-163641-vehicle-echo-look`** (one game, echo 0.5 s / 4 m) | Side views of our vehicle and the echo copy: `01-truck-look-on` the copy gold with three defense packs on its near side, at the places of our truck's (rust-brown) packs; `02-truck-look-off` (`vehicle_sync 14 0`, `15 0`): the copy respawned **grey, no packs** (the v0.12 view); `03-...-on-again` gold with packs again. `06-trike-look-on` our trike and the copy both purple / orange; `07-trike-look-off` the copy grey. Packs read back 0.000 m while parked. |
| `20261118-162226-vehicle-echo-census` | The census above; `02-truck-and-copy-b` (enlarged): our rust-brown truck with three defense packs on its side, the copy grey with none (the v0.12 view). |

### 22.8 A solid roadster (item 6 of the plan): not done, the hologram stays

- **No safe ready-made entity**: no cutscene, figure or prop entity uses the roadster's solid art (`part_casp_main_def`)
  [V: the prefetch list's 211k paths, research]; `ds/entities/vehicle/vehicle_casp_game_actor` is not an entity (a
  `DSVehicleSpeedCarGameActorUnitResource` and a `GameActorSpecificData`) [V: core decode, this milestone];
  `ds_vehicle_casp_physics` is the physics. A copy of the drivable `DSVehicleSpeedCar` is the pattern that crashed with
  weapons (17.1) and carries 31 components (AI nav mesh, interaction, baggage carrier, sound ...).
- **The only route found** would put the solid art into the construction hologram's `SkinnedModelResource` (`+0x30`
  `ArtPartsDataResource`, with `DisableCollision` `+0x70`) for the hologram's realize, then restore it: a write into a
  shared game resource (the garage's own construction hologram uses it), with unknown reference counting.
- **It fails the safety bar of this milestone**: it writes into a shared game resource, and the solid art brings the
  drivable roadster's collision and physics parts (`PartMotionType Dynamic`; the solid trike and truck copies' art
  collides with the player, 22.10), whose effect on the hologram entity is unknown.
- (Review follow-up: this used to give the hologram entity's AI components as a reason - `DSAIIndividual`, `AIBody`,
  `BasicControlledEntityAIController`, `DSGroundMovement`, run `122455` - but the roadster hologram has been the default
  copy since v0.12 with exactly those components, and the trike and truck holograms are the default now. The rule "no AI
  or controller components" applies to the solid cutscene bodies; the construction holograms pass instead the welcome
  hologram's own checks (`holo::ValidateWelcomeHolo`) and the copies' common deny-list (no interaction, physics,
  ragdoll, player controller, capsule, vehicle mover or bounder component), are placed by us every tick, and were never
  seen to move, act or fault in any run, also at 240 Hz (`BoostEntity`) [V: the runs; I: what their AI components would
  do].)
- Hence the hologram stays, and the look is not applied to it (its materials are hologram shaders): on the other screen
  the roadster stays the blue construction hologram.

### 22.9 Vehicle sounds (fourth milestone; `session.ini vehicle_sound`, default 1, with `sound_sync=1` and `vehicle_sync`)

**What the game does** [V: decompiles `0x142F6CE00` / `0x142F74820` / `0x142F74B70` / `0x142F74E90`, the decoded
`sd_sfx_vehicle_all` graphs, runs below]:
- Every vehicle sound is played by the vehicle's own `SoundComponent` (a sound controller at `vehicle + 0x8A0`; the road
  noise from its sound state machine). Each Decima sound instance is its own Wwise game object; the vehicle's are made
  inside the component's play path (`0x141DB61A0` / `0x141DCBC30`, called from it) but registered with Wwise later,
  outside it. That is why `partner_sound`'s "registered inside a play path" rule saw none of them (run
  `20261118-171051`: 0 vehicle objects known, its posts "unowned") and why the instance create / setup are hooked now.
- The graphs set their RTPCs on their own game object only when they change. The roadster's engine loop
  `0xAD19C8B9` carries 17 RTPCs: `8A199A5D` rpm (1000 at idle, up to about 6,300), `ADE31A03` velocity (0 to about
  25), `8B335EB2` throttle (0 / 1), `76B6422F` (velocity times 1.43 [I]), `20D713EF` ground distance, `ABC35145`
  damage, `624BF273` occupant (1 while seated), `FAB65D21` (suspension, also on the bumps) and the rest of 1.3 of the
  research (`43DEE1D1` stayed 0). The road noise objects carry 8-10 (`65669953` changes with the ground: 65535 on the
  garage floor, 84 / 104 / 10 outside [I: a surface id]). None of them changed with the camera or the listener in
  these runs [I: nothing listener-relative seen].
- The road noise and skid loops (roadster `0x1B2FD579` + `0x7DED77AB`, trike `0x43582143` + `0x39FE036A`, truck
  `0xB7A24F27` + `0x1F78808C`) start when the vehicle is spawned, long before anyone gets on; the engine loop
  (`0xAD19C8B9` roadster, `0x384DC8EC` trike, `0x1A5C20C0` truck) starts with the ignition (`0x5403389F`, truck
  `0xAF464419`) and the game stops it with `StopPlayingID` (fade 100 ms) at the cut-off (`0x23486788`, truck
  `0xFA638E52`) when Sam gets off. The trike's engine graph also posts `0x481AAE3B` / `0xC4CDE0DC` on the same object
  while driving; they end by themselves. 0x5287BC7E / 0xA009B2E2 (bush rustling), 0x96B72F2D (suspension bump, at the
  wheel: (-0.76, -1.37, 0.47) m), 0x43B0699A (indicator), 0x3B612EA5 / 0x168503E1 / 0x9982BF94 / 0x2E6E2028 (the
  truck's body raise / lower), 0xF0A910A3, 0x0BE96F8F, 0xFA4D5911 were seen too.

**Sender** (`partner_sound.cpp`; the hot paths as before, the rest on the game thread):
- A sound instance made or attached inside a component's play path whose owner (or an entity up to two levels above
  it) is a drivable vehicle (vsync's vtable check, `vsync::VehicleClass`) is that vehicle's sound object. Its posts,
  RTPCs (up to 24 kept in place), stops, actions and unregistration are followed for every vehicle of the world, also
  while there is no local player (a load).
- It goes out only while `vsync` reports that vehicle as the one we ride (`vsync::ReportedVehicle`: seated, or parked
  after getting off within 60 s / 30 m) and the local game is not paused, in a menu, a cutscene or loading (the
  partner voice's gate). Posts become STA1 v2 items (category 5, slot 7, the post's place in the vehicle's frame,
  `vsync::ReportedVehicleLocal`, fault-free reads; its RTPCs: 12 in the item, the rest in an rtpc item right behind).
- **Held loops**: an event of the looping graphs (23 ids, `kVehicleLoops`) is held until the game stops it
  (`StopPlayingID`, an action stop / break, its object unregistered). A held loop is sent when it starts, picked up when
  the vehicle becomes ours or the gate opens (the road noise that started at the spawn), re-sent every 1.5 s with the
  same playing id (`kSoundHeld`), and stopped on the receiver (stop items, repeated twice) when the vehicle is ours no
  more or the gate closes. After a gap of over 200 ms between send ticks (our sends held, a hitch) nothing is decided
  for one tick (vsync's report is one tick behind psound's in the send tick).
- **RTPC stream**: changes on an object with a sound the receiver knows go out at most every 100 ms per object, beyond a
  deadband (0.5 %, at least 0.001) of what went out, as rtpc items with a 100 ms glide (`kVRtpcFadeMs`); what does not
  fit in the 512-byte frame stays for the next one. Measured: 586 rtpc items (2,508 values) and 218 vehicle posts (189
  of them refreshes) from the host over the whole of `172756` (four vehicles, about 70 s ridden).

**Receiver**:
- A vehicle post is carried out only for an id of the vehicle graphs (`kVehicleEvents`, 67 ids) or one this game
  posted on its own vehicles' sound objects this session; never a voice. The player category's deny-list does not
  apply (its "camera" ids `0x39FE036A` / `0x43582143` are the trike's road noise).
- It is posted on one of our 24 Wwise objects placed at `vsync::PartnerVehicleWorld`: the place the partner's vehicle
  frames play this tick (the copy's place, also when no copy is shown or it is hidden over the watcher's own vehicle) +
  the offset in the vehicle's frame; the object follows it every tick. Within 250 m of the partner (the graphs' MaxDist
  is 300 m). `partner_volume` applies (the objects' output bus volume).
- A held loop that plays already only takes the refresh's lease, offset and RTPCs (with a 100 ms glide); one that ended
  here by itself (its end-of-event, no stop of ours) is not posted again by a refresh. A held loop with no refresh for 5
  s is stopped (a lost stop, the sender gone); held loops are not under the 15-minute cap.
- All the partner's vehicle sounds stop when its vehicle has not been played here for 300 ms (no frames for 2 s, the
  copy held for a silent partner, broken for 3 s, removed), at our gate (pause, menu, cutscene, load), with no partner
  body, at a new link session, or with the key off; the held loops come back with the next refresh once it plays again.

**Wire**: STA1 version 2 (section 19; ctest `TestSoundFrames`): category 5, slot 7, `flags` bit 0. Version 1 is
refused; mixed builds are refused at HELLO (the wire hash, section 4).

**Keys and actions**: `vehicle_sound=0` installs no create / setup hook and sends or posts nothing of a vehicle (the
v0.12 behaviour); `sound_sync=0` (the default) turns all of it off. `action sound_sync 3 <ms>` traces both sides every
500 ms (the vehicle objects' loops and RTPC values; our objects' places against the copy entity's own transform), `action
sound_sync 4 0|1` switches the vehicle sounds at run time (debug-actions.md).

**Evidence** (hidden, `-SeedSaves complete-100`; every run: exit 0 (both games), no error.txt, real saves unchanged and
restored, 0 uploads):
- `20261118-172756-two-games-vehicle-sound` (final build; host Sam, guest Fragile): the guest posted 38 of the host's
  vehicle sounds and ended all 38 (`playing now 0` at the end), took 156 refreshes, did not post 9 refreshes of 2
  trike sounds that had ended by themselves, stopped 6 for no vehicle; 0 refused, 0 PostEvent failed, no Wwise fault;
  every post with a live copy and all 144 trace lines at the copy (`vs the copy entity's own transform + offset: 0.000
  m`); 9 loop pick-ups 30-60 ms before the copy went live logged `no copy` (placed at the partner's vehicle frames). The host
  posted 5 of the guest's truck sounds (road noise loops, ignition, engine loop, cut-off) and ended all 5. The host sent
  218 vehicle posts (9 loops picked up, 189 refreshes) and 586 rtpc items (2,508 values). RTPCs across the link (the
  roadster's engine, every 500 ms, the two games' own clocks): the host's velocity 0 -> 7.33 -> 13.43 -> 16.53 -> 20.01
  -> 22.84 with rpm 1000 -> 4243 -> 5118 -> 4987 -> 6041 -> 4926; the guest's object 0 -> 8.23 -> 12.66 -> 17.02 -> 20.4
  -> 22.84 with rpm 1000 -> 4735 -> 5337 -> 5135 -> 6158 -> 4926. The host's cuff map (the sender's gate): its three
  loops stopped on the guest, picked up the moment it closed; the guest's cuff map (the receiver's gate): `stopped the
  partner's sounds on 2 object(s)`, the engine loop posted again 0.2 s after it closed; the host's sends held 3 s:
  stopped 0.4 s after the copy was held, posted again 31 ms after it played again; held 12 s: stopped, the body removed
  after 10 s, the engine loop posted again when the copy came back; getting off: the cut-off posted, the engine loop
  stopped by the game's own stop (fade 100 ms) on both screens; `vehicle_sync 1 0` on the host: the trike's road noise
  stopped on the guest.
- `20261118-173144-vehicle-echo-sound` (final build, one game, the echo copy 4 m beside us, 0.5 s behind): 38 vehicle
  posts, every one and all 247 trace lines at the copy (0.000 m), 0 refused, 0 failed; the cuff map, held sends (`stopped
  3 partner vehicle sound(s): the partner's vehicle is not played here any more`), `sound_sync 4 0` (`stopped 3 partner
  vehicle sound(s): vehicle_sound off`) and `4 1`, getting off each vehicle (`vehicle loop ev 0xAD19C8B9 / 0x384DC8EC /
  0x1A5C20C0 ... stopped by the game after 39017 / 12878 / 9961 ms (fade 100 ms): the stop goes out`) and vehicle sync off
  (`stopped 2 partner vehicle sound(s)`). At the end the truck was still reported as parked (60 s / 30 m): its two road
  noise loops still held, as designed.
- Development runs: `20261118-171051` (0 vehicle objects known: registrations happen outside the play path; hence the
  create / setup hooks), `171459` (the first working one; showed the trike's `0x481AAE3B` / `0xC4CDE0DC` re-posted by
  refreshes after they had ended: fixed by the receiver's "ended by itself" set; and a stop / pick-up churn after held
  sends: fixed by the send-gap rule), `172012` (two games, before the vehicle refusal counter and the seed
  `0xFA4D5911`).

**What to listen for** (not heard in the hidden runs; the games are silent):
- Standing near the partner's parked roadster / trike / truck (up to 60 s after they got off): its road noise loops are
  played at the copy, as the driver's game plays its own parked vehicles' (whether they are silent at speed 0 is
  [I]). When they get on: the ignition, then the engine idling at the copy.
- While they drive: the engine rising and falling with their throttle and speed, the road noise and skids changing
  with the ground, suspension thuds at the wheels, the indicator, the truck's body raise / lower; the sounds move with
  the copy and fade with distance (up to about 250 m).
- When they get off: the cut-off and the engine stops; nothing left behind after they walk away (60 s / 30 m), pause,
  or their game goes silent (within about 0.3 s of the copy being held).
- `partner_volume` lowers all of it with their footsteps; `vehicle_sound=0` keeps their footsteps and foley only.
- Worth a check by ear: whether the engine sounds like a vehicle passing by or like the listener's own (the graphs
  computed their listener-relative inputs on the driver's game, see below).

**Not covered / still different**:
- **The listener's perspective**: the RTPC values are the driver's game's (its graphs see the driver as the local
  player). No RTPC was seen to change with the camera or distance, but some ids are unnamed (`43DEE1D1`, `76B6422F`,
  `FAB65D21`, the road noise's `5C347FEF`, `AFAD5E46`, `BEA713CC`, `DA1C68AC`) [I].
- **The loop list is static**: a looping event outside `kVehicleLoops` is mirrored like a one-shot (it plays until the
  game's stop; not picked up after a pause or a silent partner). Since the review the trike engine graph's
  `0x481AAE3B` / `0xC4CDE0DC` (they end by themselves) are off the list; another listed event that ends by itself is
  not posted again by refreshes for 30 s after it ended here.
- **Boost** (the roadster's burst mode, `B6E02516`) and the horn were not driven in a run; their ids are on the list.
- **Not run**: a long drive, a vehicle destroyed while ridden, a real load mid-drive (the copy's own paths for these
  stop the sounds), two vehicles at once, Kusanagi-specific sounds beyond the trike's (it uses the motorbike's sound
  resource).
- The `vehicle_sound` key is not in the launcher (mod key only, like `vehicle_look`).

### 22.10 Review follow-up (2026-11-18): holograms by default, overlap that hides, the sound fixes

Two reviews of the second to fourth milestones (the uncommitted vehicle diff, and the three reports against their runs)
were checked against the code, the logs and the screenshots. [V] read in the code, a log or an enlarged screenshot;
[I] reasoned, not observed.

**Found and fixed**:
- **The solid copies collide** [V]. Run `155304` had walked Sam past the solid copies' sides (2.04 m beside the truck's
  origin, 1.15 m beside the trike's, from 4-5 m before them), so "no collision" was never shown. Walks from 1.5 m before
  the origin and placements at the origin (`vehicle_sync 16 0.01`), runs `20261118-175540`, `-180336`, `-180610`,
  `-180901` and `-181155` (the last one passes every step): the **solid truck** stops Sam 1.98-2.09 m past its origin, at
  the same point in every walk (`03-past-truck-copy`: his camera inside the truck's body against its front wall), while
  he walks through that very point once the copy is gone; on the **solid trike** he ends up 0.81-1.01 m above its origin
  (`11-at-trike-copy-origin`: standing on it), the walk from 1.5 m before is pushed 1.14 m aside, and a Climb prompt
  shows on it (`09-in-trike-copy`; the reviewer's unexplained prompt of `155304`). The game data says why: the cutscene
  bodies' SkinnedModelResource has `DisableCollision` false and their art (`part_bkst_main_def`, `part_cald_main_def_game`,
  the drivable vehicles' own) holds 8 and 18 `PhysicsCollisionResource` (PolySoup). The art of the three construction
  holograms (`part_casp_main_hlo`, `part_bkst_main_hlo`, `part_cald_main_hlo_game`) holds none, and the trike and truck
  holograms' skeletons have the drivable layouts `8DABA9B0` / `B609EC0D` (their `SkeletonLayoutHash`). Hence the
  **default copies are the holograms** (`kCopies` rows 4 and 5, 22.4): in run `181155` Sam walks straight through the
  **truck hologram** (0.00 m beside at its origin, 2.66 m past it, past the solid truck's stop point) and the **trike
  hologram** (-0.00 m beside at -0.18 m, 3.12 m past) and stays on the floor at their origins (`19-at-trike_hologram-origin`:
  Sam inside its glowing outline); the control, our own parked roadster, stops him 0.71-0.74 m before its origin. The
  pack copies' art has no collision resource either [V: data; not walked into].
- **Overlap hiding did not hide the roadster hologram** [V: `160727 guest 00-guest-sees-host-on-roadster`, enlarged:
  grey and green patches through her own roadster while the log said hidden]. `SetVisible(false)` does not stop a
  construction hologram from drawing. While hidden, the copy is now also placed 20 m below (where it is spawned); the
  overlap test still uses the frames' place. Run `20261118-181458` (two games, the same screenshot, enlarged): her own
  purple roadster with the host's Sam at its wheel and no hologram patch [V].
- **Vehicle sounds** (`partner_sound.cpp`):
  1. The trike engine graph's `0x481AAE3B` / `0xC4CDE0DC` (they end by themselves) are no longer held loops: they stayed
     in the object's list of 6 after they ended, were refreshed every 1.5 s (a receiver that missed them replayed them),
     and four more posts would have evicted the engine loop with a stop to the receiver. Also, a held event posted again
     on its object now replaces its own entry. Run `182225`: both posted once each as one-shots, the trike engine loop
     `0x384DC8EC` held and refreshed [V].
  2. Tags are no longer reused while held: a new tag skips every tag of an object with a held loop or a post in the last
     60 s (they wrapped in about 85 s while the truck's engine kept its tag, run `172756`); the receiver's refresh also
     restores the vehicle slot. Run `182225` did not wrap (tags skipped 0) [I: the skip path not exercised].
  3. A held loop that ended on the receiver by itself is skipped by refreshes for 30 s only (it was for the session), and
     not at all when the partner was beyond 250 m at its end (a distance kill).
  4. A stop / break action on a vehicle object now goes out as an action too (before, only held loops' stops went out),
     and the receiver checks vehicle actions against the vehicle allow-list.
  5. The instance create / setup hooks are installed only with `vehicle_sound=1` **and** `vehicle_sync` not 0, as their
     own group: when they cannot be installed only the vehicle sounds are off (before, all partner sounds were); the hot
     path pushes nothing for them while they are off (also with the audio probe forwarding).
  6. A pooled sound instance set up again by a non-vehicle component is no longer kept as the vehicle's [I: whether the
     game reuses instances that way was not seen].
- **The pose meter** (one acos per joint per update at 240 Hz) ran in normal play: now only in hidden tests or while
  the trace runs.
- **The receiver took any shader variable and ArtParts bits in STV1 item 255**: it now refuses a vehicle look with part
  bits or a variable other than the colour index `0x3A3714ED` (`received (n refused)`).

**Doc and report corrections** (the text above is corrected where it was wrong): the "hologram hidden" screenshot of
`160727` (still drawn); "Sam walked straight through" (`155304`, slid along); the Kusanagi placement in `160727` (0.23 /
0.61 / 0.93 cm by copy lifetime); "both back 2 s after the sends resume" (2 s after the removal, 0.1 s after the resume);
"every post at the copy" (`172756`: 9 pick-ups logged `no copy`, 30-60 ms before the copy went live); "a parked vehicle
should be silent" (its road noise loops are played at the copy; silent at speed 0 [I]); the truck look (the copy is clean
gold, the driver's truck rust-brown); the AI components argument of 22.8. The two-game hologram meter is now measured
with smoothing on (`181458`: 1.00 pose writes per frame for the roadster, trike and truck holograms at about 30 fps);
the earlier two-game runs measured it off. The placement error compares receiver and sender at the same play time, so
it does not include the play delay (100 ms in two games: about 2 m behind at 20 m/s). The pack read-back proves our
write, not the visible place. Run `171035` (the sounds milestone) has no summary: its scenario was refused before a game
started. The "Abandoned vehicle has been dismantled" notice in `160727 guest 11-guest-driving-a` is the game's own
[I: a vehicle the test teleports left far away; the mod never writes to local vehicles].

**Refuted / kept**: the review's own claims all held. Kept on purpose: the roadster, trike and truck holograms carry AI /
controller components (22.8); the meter's and the overlap's registry reads stay as they were (read only).

**Final runs** (hidden, `-SeedSaves complete-100`; every run: exit 0 on both games, no error.txt, real saves unchanged
and restored, 0 cloud uploads; the final build):

| Run (`build-testenv\runs\`) | What it shows |
|---|---|
| **`20261118-181458-two-games-vehicle-trike`** (defaults: holograms; host Sam, guest Fragile with self view) | All four parts pass: the host's roadster (garage), the host's Kusanagi (as the trike hologram), the guest's truck (truck hologram), the host's standard trike (trike hologram). Every copy loaded in 66-167 ms, `posed from the partner's vehicle joints` (23 / 40 / 47), hidden over the watcher's own vehicle at the start (0.00 m) and shown again at 4.07-7.17 m; 1.00 pose writes per frame on every hologram. Held 3 s (`held where it stands ... 1132 ms old`, `plays again`), 12 s (body and copy removed after 10 s of silence, back at once). Placement while moving (cm mean / p95 / max): roadster 0.61 / 1.53 / 1.76, Kusanagi 0.27 / 0.64 / 0.84, standard trike 0.29 / 0.69 / 0.83, truck 0.35 / 1.17 / 1.44. Enlarged: `guest\automation\00-guest-sees-host-on-roadster` (her roadster, no hologram patch), `guest\automation\03-guest-sees-host-driving-a` (the host riding the teal trike hologram beside her own red Kusanagi) against `automation\03-host-driving-a` (his red Kusanagi), `automation\11-host-sees-guest-driving-a` (the truck hologram with the `Guest` tag driving away beside his own parked truck) against `guest\automation\11-guest-driving-a`. |
| **`20261118-181937-two-games-vehicle-look`** (`vehicle_sync=2`: the solid copies) | The gold truck: `look ... applied (1 variation(s), 1 colour(s); 4 ... sources)`, six defense pack copies placed (read back 0.000 m at 7.40 m/s), `02-guest-sees-host-driving-a` the gold copy with its packs beside her own rust-brown truck; the trike: `applied (0 variation(s), 4 colour(s))`, two battery packs, `07-host-sees-guest-stopped` Fragile on the purple / orange copy; the Kusanagi: `its look is not applied`. 0 looks refused, failed or partly applied, 0 place faults. |
| **`20261118-182225-two-games-vehicle-sound`** (`sound_sync=1`, defaults) | The guest posted 38 of the host's vehicle sounds and ended all 38 (0 refused, 0 PostEvent failed, playing 0 at the end), 154 refreshes; the host posted the guest's 5 truck sounds and ended all 5. The trike's `0x481AAE3B` / `0xC4CDE0DC` as one-shots. Posts at the copy 0.000 m (18), with the copy hidden over the watcher's own vehicle 11 (`copy hidden over our own vehicle`, at the frames' place), before the copy was live 9. The guest's cuff map: `stopped the partner's sounds on 2 object(s)`; the held sends: `stopped 3 partner vehicle sound(s): the partner's vehicle is not played here any more` (twice). |
| **`20261118-181155-vehicle-echo-collision`** | The collision walks above (solid truck, solid trike, truck hologram, trike hologram, our own roadster). |
| `20261118-175540-`, `-180336-`, `-180610-`, `-180901-vehicle-echo-collision` | The same walks while the scenario was fixed (the solid parts identical; the later ones failed at re-mounting the roadster for the trike hologram, games fine). |

**Not proven**: a solid copy driven into the watcher or the watcher's vehicle (what the solid copies' collision does
then); the trike and truck holograms' look (they are holograms: no paint, no packs, no Kusanagi look); tag reuse past
255 (not reached); the audio-probe forwarding with vehicles; boost and horn; a long drive, a real load or link loss
mid-drive (held sends only), a vehicle destroyed while ridden; anything heard (the runs are silent).

### 22.11 Solid copies by default, their collision off on each copy (after v0.13)

The user's request after v0.13: no hologram vehicles, each vehicle fully synced on both screens. [V] seen in a log, a
decompile or an enlarged screenshot; [I] reasoned, not observed.

**What changed**:
- **The solid trike and truck copies are the default again** (`vehicle_sync=1`, and `2` as before; `kCopies`, `CopyFor` in
  `vehicle_sync.cpp`). The Kusanagi trike's frames use the solid trike copy (in the standard trike's look). The roadster
  stays its construction hologram: no solid roadster copy exists (22.8, and the art swap below). `vehicle_sync=3` is now
  the opt-in for all three as holograms (v0.13's default; it was "the same as 1"); `4` the roadster's copy only. A
  switch between `3` and the others at run time respawns the copy.
- **Each solid copy's collision is turned off on its own entity before it is shown** (`CollisionOff`): the game's own
  per-instance switch, `Entity::SetCollisionEnabled(entity, false)` at `0x14234A530` (entity vtable slot +0xF0; the
  cutscene event `DisableEntityCollisionEventInstance` (`0x1424B7250`) calls the same slot). It clears the entity's
  collision flag (`+0x88` bit `0x4000`) and sends MsgDestroyCollisionVolume: the model's parts (`model +0x58` count,
  `+0x60` parts of 0x100 bytes) lose their physics instances (part `+0xD0`). Nothing shared is written (the resources'
  `DisableCollision` and PolySoup shapes stay as they are). Byte checks before the first call: the function's 56-byte
  prologue (it reads `+0x88 >> 14 & 1` and refuses enabling while `+0x321` is set) and Model::OnModelResourceChanged's
  reads of `+0x58` / `+0x60` (`0x1423933A6`); before every call the copy's vtable slot +0xF0 must be the base function;
  the call runs on the game thread under SEH. The copy is spawned 20 m below and not shown until the call read back
  flag 0 and no physics instance.
- **Checked every 250 ms** (`CollisionWatch`, also before the first show): found on again, it is turned off again and
  logged (`collision found ON again ... #n`); more than 3 times, not off after the call, or unreadable: the copy is
  removed and refused for the game run, and its hologram stands in (`CopyFor`, logged once per change: `the partner's
  <key>: copy '<copy>' (<why>)`). The checks failing at start: no solid copy at all, the holograms stand in. A fault in
  the call turns the solid copies off for the session.
- **The battery and defense pack copies carry collision too**, against what 22.7 / 22.10 said from their resources: each
  pack copy had 1 physics instance (`battery_pack` / `defense_pack: collision turned off on the copy: flag 1 -> 0,
  physics instances 1 -> 0 of 2 parts`, run `194926`) [V]. They get the same call when realized (refused for the game run
  if it fails).
- Unchanged: the overlap hiding (the copy hidden and put 20 m below over the watcher's own vehicle of its kind), the
  per-frame pose writes in the copy's own update at 240 Hz, the held sends, the vehicle sounds at the copy, the looks
  (paint, variations) and the packs on the solid copies, the copy's world transform (for the later passenger work: the
  seat helpers and skeletons `8DABA9B0` / `B609EC0D` are untouched).
- Hidden tests: `action vehicle_sync 21` (the copy's collision state, with our own vehicle as the control), `22 <forward
  m> [right m]` / `23` (the copy pinned ahead of our own vehicle / free again), scenario `vehicle-echo-solid`.

**The collision decision** (research runs `20261118-190034-solid-coll-probe` and `-190629-solid-coll-probe2`, scratch
build): with their own collision on, a moving copy pushes Sam (the trike moved through him at 2 m/s: 2.01 m; at 10 m/s:
1.97 m, up to 0.127 m in one tick), carries him when he stands on it (2.88 m, lifted 1.07 m), and lifts him onto it when
collision comes on while he overlaps it (+0.90 m); its layers 15 / 11 / 17 also meet the watcher's vehicles and wheel
raycasts. A blocker that never pushes was not proven safe (it would need the parked-only gating of the research report
and a two-game proof), so **the solid copies are collision-free**: players walk and drive through them, never pushed,
lifted, stopped or damaged by them. A partner_collision-like capsule for a parked copy is not done.

**The art swap for a solid roadster / the Kusanagi's own look: tried, does not work, removed** [V: run
`20261118-193307-zz-scratch-artdiag`, decompiles]. The idea: the roadster hologram's entity (the drivable roadster's
23-joint layout) or the trike body made to draw the drivable vehicle's own `SkinnedModelResource` (taken from one of the
watcher's own registered vehicles) by setting the SkinnedModel component's resource and sending MsgModelResourceChanged
(`0x142393380` rebuilds the parts from the resource's `ModelPartResources`, `0x1423A4750` takes the skeleton). The log:
every vehicle's and copy's `SkinnedModelResource` has **0 ModelPartResources** (`+0x20 0`; our roadster's model has 37
parts, the trike body's 39): the parts come from the entity's **ArtPartsData component**, whose builder
(`0x1422B3800`) reads its own `ArtPartsDataResource` (root model, sub-models) and adds the parts to the model; it also
answers the skeleton query (`0x1422B3780`). A swap would have to tear down and rebuild the ArtPartsData component's
sub-models, effects and shader-variable arrays on a live entity: not attempted (high risk). Hence still no solid roadster
and no Kusanagi look.

**Evidence** (hidden, `-SeedSaves complete-100`; every run: game exit 0 (both games where two), no error.txt, real saves
unchanged and restored, no violation, 0 cloud uploads; part A's build, before 22.12 / 22.13: the runs on the final
build are listed in 22.13):

| Run (`build-testenv\runs\`) | What it shows |
|---|---|
| **`20261118-194123-vehicle-echo-solid`** (pass) | Garage, role echo. `truck: collision turned off on the copy: flag 1 -> 0, physics instances 19 -> 0 of 72 parts`, `trike: ... 9 -> 0 of 39 parts`; found on again 0 times in the run. **Truck**: the centred walk from 1.50 m before its origin ends **8.57 m past it** (the v0.13 solid truck stopped him at 1.99 m); placed at its origin Sam stays on the floor (**dz 0.11**). **Trike**: 1.50 m before -> **8.10 m past**, at its origin dz 0.11 (v0.13: 0.81-1.01 m, standing on it). No Climb prompt 0.9 m before the trike (`B4-before-trike`, enlarged: Sam over the trike, no prompt); for the truck
this is **not shown** (22.13 review: in `A4-before-truck` the camera is inside the copy against its tail lamp and Sam is
out of view, in every run, also `205827`) [I: with no physics instance there is nothing to climb]. **Driving into a pinned copy**: our roadster straight through the solid truck pinned 8 m ahead (closest approach 0.02 m from its origin): speed 11.6 m/s mean 3-6 m before it, 12.6 / 13.3 / 13.7 m/s from 3 m before to 3 m past it (still accelerating), largest height step 0.002 m per tick, ride state 2 (seated) throughout; the same through the solid trike pinned 6 m ahead after backing out (closest 0.04 m; 8.0 / 9.3 / 10.2 m/s, height step 0.001 m). `C1-driving-into-truck` (enlarged): the roadster inside the truck copy's body. The collision flag of our own roadster stays 1 (15 physics instances of 37 parts): only the copies change. |
| **`20261118-194435-two-games-vehicle-trike`** (pass) | Defaults, host Sam, guest Fragile. The guest sees the host's **Kusanagi as the solid trike copy** (`03-guest-sees-host-driving-a`, enlarged: Sam riding a grey solid trike beside her own red Kusanagi; his own view `03-host-driving-a` the red Kusanagi), the host sees the guest's **solid truck copy** with its three side defense packs (`11-host-sees-guest-driving-a`, enlarged crop), the roadster as its hologram (`no solid roadster copy exists`). Overlap: each copy hidden over the watcher's own vehicle at the start (0.00 m) and shown again at 4.16-7.93 m; `02-guest-sees-host-on-trike` (enlarged): the host seated on her own red Kusanagi, no grey trike drawn. Held 3 s and 12 s as before. Placement while moving (cm mean / p95 / max): roadster 0.55 / 1.26 / 1.69 (132 ticks), Kusanagi (trike copy) 0.29 / 0.60 / 0.84 (153, outside the held sends), standard trike 0.34 / 0.69 / 0.86 (235), truck 0.44 / 1.12 / 1.32 (138); 1.00 pose writes per frame on every copy; `collision state: copy 'trike' ... flag 0, 0 physics instances of 39 parts` on the watcher after each drive, found on again 0. The standard trike's watcher views look at the ground (slope). |
| **`20261118-194926-two-games-vehicle-look`** (pass) | Defaults (was `vehicle_sync=2`). The gold truck: `look ... applied (1 variation(s), 1 colour(s))`, six defense pack copies (each `collision turned off ... 1 -> 0`), read back 0.000 m at 7.07 m/s; `02-guest-sees-host-driving-a` (enlarged): the gold solid copy with its packs, `Host` tag, beside her own rust-brown truck (the wear is still not mirrored). The purple / orange trike: `applied (0 variation(s), 4 colour(s))`, two battery packs. The Kusanagi: `its look is not applied (copy 'trike' has another model)`. |
| **`20261118-195738-vehicle-echo-solid`** (pass; the final build, only comments changed since `194123`) | The same steps: truck walk 1.50 m before -> 8.00 m past, trike -> 8.55 m past, at their origins 0.01 m along; our roadster through the pinned truck (11.7 / 12.6 / 13.1 / 13.7 / 14.0 m/s from 6 m before to 6 m past, height step at most 0.002 m) and the pinned trike (7.9 / 9.4 / 10.2 m/s through it); collision switched off 4 times, found on again 0. |
| `20261118-193811-vehicle-echo-solid` (pass) | The first drive-into version: the roadster still turning passed 2.2 m beside the truck copy's origin (12-14 m/s, no height step); the trike pinned ahead stood against the garage barrier (no drive). Hence the straightening and LT in the final scenario. |
| `20261118-192913-vehicle-echo-solid` (fail at a scenario step, game exit 0) | The build with the art swap: the solid truck / trike walks as above; the Kusanagi swap refused before any write (`its own SkinnedModelResource does not check out: no model parts`), so the next step found no copy. |
| `20261118-193307-zz-scratch-artdiag` (pass; scratch scenario, not kept) | The model resources logged: `SkinnedModelResource (+0x20 0 ...)` on our roadster, the roadster hologram and the trike body (the art swap's dead end above). |

**Not proven / not done**: a solid roadster (hologram, logged) and the Kusanagi's own look (standard trike); any blocking
of the partner's vehicle (by design: collision-free); NPCs, BTs or MULE vehicles meeting a copy [I: with no physics
instance nothing can hit it]; a copy whose collision the game re-creates (never seen: 0 times in every run; the watch
handles it); a parked partner vehicle as a blocker; the wear / rust, mud, lights and cargo of 22.7 (unchanged).

### 22.12 Wear, lights and cargo on the solid copies (STM1 version 3, after v0.13)

The user's request after v0.13: each vehicle fully synced on both screens. [V] seen in a log, a decompile or an enlarged
screenshot; [I] reasoned, not observed.

**What the game does** [V: the census `20261118-162226`, decompiles of the vehicle controllers, the runs below]: a
vehicle's look beyond its paint and variations lives in its ArtParts shader variables, set every frame by the vehicle's
own controllers, which a cutscene copy does not have:
- `0x32D5AE44` **timefall wear (corrosion)**: one value (`1 - clean` of the corrosion controller at `vehicle + 0x670`,
  `0x142F87B40`) on every part it lists: 39 parts on the truck, 20 on the trike; 1.0 on the save's gold truck (rust-brown),
  0.35 on its other vehicles. Run `164037` (22.7) had sent the first 31 of them (the cap of 32 shader variables of STV1):
  the copy's wheels and lower parts turned rust-brown in its screenshot while its body (the later parts) stayed gold;
  the conclusion "they do not carry the rust" was wrong.
- `0x2B11751D` **emissive intensity** per lamp part (the lights controller at `vehicle + 0x7B8`: head lamps fading in after
  getting on, tail / brake, indicators, the battery gauge 50, a truck lamp 20, burst) and `0x0978E0A1` the **battery
  gauge's colour** (rgb; (0.025, 0.7, 0.5) on the gold truck, (0.01, 0.1, 1.0) on others).
- `0x42A726AA` (1.0 on every part) is the camera's dither / fade near the watcher: never sent.
- **Cargo**: the `DSBaggage2CarrierComponent` (roadster and trucks: slot 0 Default and slot 69 PassengerSeat; trikes: slot 0).
  While Sam rides the truck, his own five equipment pieces (weapon cases, tools; attribute Personal, case types 2 / 4) sit
  in its slot 69, each DSBaggage2's world transform live (2.5 m beside it while he got on, then (0.4..0.6, 0.7..0.9,
  1.9..2.2) m in its frame: on his back in the cab) [V: `our vehicle's carrier: slot 1 (type 69) ...`, run `202559`].

**Sender** (`vsync::TakeSend`, `look::ReadVehicleState`; fault-free reads of the vehicle's shader-variable entries every
100 ms, only while it is reported and `vehicle_look` is on): the wear (the largest of its `0x32D5AE44` entries; logged
when they differ: spread 0.000 in every run) with the bits of the parts carrying it (index < 128), up to 24 lights (part,
kind, values clamped to the wire's ranges, duplicates dropped). Every frame carries them (the state block). The cargo
(`ReadCargo`): every 500 ms while the vehicle stands still, every piece in every slot whose case type the gear draws (the
gear's own rule, `gear::CargoModelOf`: deliveries, never equipment, weapons, tools), its place in the vehicle's frame from
its own world transform (within 8 m, a rotation), its colour as the baggage config's variation index; at most 32, and only
as many as fit the frame (1140 bytes); sent when they change and every second. A frame the encoder would refuse goes
without the version 3 blocks (logged), never not at all.

**Wire** (STM1 **version 3**, 22.3; ctest `TestVehicleFrames`): the state block (20 + 8 per light bytes: the truck's 13
lights 124 bytes) after the packs, the cargo block (2 + 14 per piece) after it; the truck's largest real frame 84 + 564 +
84 + 124 = 856 bytes, with 20 pieces 1138. The decoder refuses version 2, unknown flag bits, a block flag without its
bytes (or values without the flag), nl over 24, a reserved byte, wear without wear bits, a light on part 128 or more, a
light kind outside 1..2, the same light twice, an intensity outside 0..4096 (or with a second / third value), a colour
channel outside 0..64, a non-finite or negative half float, nc over 32, a model of 31 or more, a variation of 128 or more
(but 0xFF), a piece beyond 8 m, a non-finite rotation, a trailing byte and any frame over 1140 bytes. IEEE half floats
for the light values (`wire::FloatToHalf` / `HalfToFloat`: 50, 20, 4096 exact).

**Receiver** (`DriveState`, `DriveCargo`; game thread):
- **Wear and lights** on the solid copy of the same art only (`ArtCopyOf`: the trike's on the trike copy, the trucks' on
  the truck copy; the Kusanagi's not: `its wear and lights are not applied (copy 'trike' has another model)`): what
  changed since the last frame, everything every 2 s and after each look apply; a part no longer named gets wear 0, a light
  no longer named intensity 0. `look::SetVehicleVars`: the byte-checked shader variable create / exists / add / set calls
  of 13.4, nothing else (no variation change); a vehicle copy may hold 160 entries (the gear's 48 stays for gear props).
  A failed set waits 2 s; a fault turns the looks off for the session (gear_look). Hidden tests and the trace read the
  entries back after each full apply (`wear / lights read back on copy ...: wear entries 39 (39 as sent, 0 not), lights 11
  of 11 as sent`).
- **Cargo**: the newest cargo block at or before the frame in play; one case prop per piece (`gear::CargoProp`: the gear's
  `case_s` / `case_m` / `case_l` / `case_xl` / `cube` cutscene props, by path + GUID), at most 2 started a tick, spawned 20 m
  below and invisible, refused for the game run when it is a Humanoid / player / vehicle or has an interaction, physics,
  ragdoll, controller, capsule, mover, bounder, AI or sound component, **its collision turned off as the copies'**
  (`case_s` and `case_m` each carry one physics instance: `case_s: collision turned off on the copy: flag 1 -> 0, physics
  instances 1 -> 0 of 5 parts`, `case_m: ... 1 -> 0 of 3 parts`, runs `201916` / `202559`; corrected in 22.13, this
  said `case_m` none), coloured once with the variation (`look::Apply`, the gear's own resolution),
  placed every tick at the copy's place times the piece's place, shown and hidden with the copy, removed with it or when
  the pieces change. A set-transform fault turns the cargo props off for the game run.
- `vehicle_look=0` (or `vehicle_sync 14 0`) turns all of it off with the looks.

**Evidence** (hidden, `-SeedSaves complete-100`; every run: game exit 0 (both games where two), no error.txt, real saves
unchanged and restored, no violation, 0 cloud uploads):

| Run (`build-testenv\runs\`) | What it shows |
|---|---|
| **`20261118-201916-vehicle-echo-state`** (pass; echo 0.5 s / 4 m, the final sender / receiver code but for two log lines) | Our gold truck: `our truck's state: wear 1.000 on 39 part(s) (spread 0.000), 13 light(s)` (lamp 6 = 50, lamp 10 = 20, the gauge colour on part 6, a colour on part 65), the head lamps fading in after getting on (`1=2.25 ... 9=50.00 ... 13=30.00`); the copy: `52 variable(s) set`, read back 39 / 39 wear entries and 11 / 11 lights as sent, in every check. Enlarged: `01-truck-state-on` our truck and the echo copy both rust-brown; `02-truck-look-off` (`vehicle_sync 14 0`, the control) the copy the grey BRIDGES cutscene truck; `03` rust-brown again; `04-truck-cargo` six cases (`case_m`, colour variations 0..3 applied) on the copy's roof, read back 0.000 m; `05-truck-driving-a` both rust-brown while driving. The trike: `wear 0.353 on 20 part(s), 8 light(s)`, the copy with rust spots like ours. The Kusanagi: `its wear and lights are not applied`. |
| `20261118-201534-vehicle-echo-state` (pass; the synthetic cases at 1.55 m, inside the truck's closed body and hidden behind the trike's rider) | The same wear and lights; the cases placed (read back 0.000 m) but not visible: hence the roof. |
| **`20261118-202559-two-games-vehicle-state`** (pass; host Sam, guest Fragile) | The host's gold truck: on the guest `wear 1.000 on 39 part(s), 13 light(s)`, 53 applies (22.13: every apply counted, full and change-only; the split is in the status line since), 1065 variables
set, 0 failed, 0 read-back mismatches (intensities and wear only then; the gauge colour since 22.13); six cases. `guest\automation\02-guest-sees-host-truck-cargo` (enlarged): the copy (`Host` tag) rust-brown with its defense packs, red tail lamps and the six coloured cases on its roof, beside her own rust-brown truck; the host's own view `automation\02-host-truck-side` (rust-brown). `03-guest-sees-host-driving-a`: the rust-brown copy driving away. The guest's trike on the host: `wear 0.355 on 20 part(s), 8 light(s)`, 20 / 20 and 7 / 7 read back; three `case_s` props, their collision turned off, coloured, placed. The Kusanagi: `its look is not applied`, `its wear and lights are not applied`. The trike part's host screenshots miss the copy (the camera on rocks and another player's trike). |
| `20261118-202244-two-games-vehicle-state` (fail at a scenario step, both games exit 0) | The trike's cases waited for while the copy was still hidden over the host's own trike (overlap hiding: the props stay hidden with it); the truck's cases screenshot at 60 m. The scenario was changed (the truck driven a few metres first). |

**Not possible / not done** (with the reason):
- **The roadster**: no solid copy (22.8, 22.11): its construction hologram has hologram materials; wear, lights and paint
  are not applied to it; its cargo props are placed on it.
- **The Kusanagi's own look**: no entity with its art exists besides the drivable one; the art swap fails (22.11, run
  `193307`); its wear, light and colour part indices differ from the standard trike's (the census: its gauge colour on
  part 11, the trike's on 6), so they are not put on the trike copy.
- **Mud, snow and tar**: painted at the wheels into the vehicle's `MeshPaintTextureComponent` (a GPU texture built up over
  time, `vehicle + 0x758`, `0x142F923A0`); the cutscene copies have no such component (their components: SkinnedModel,
  ExternalAnimation, LodDistanceOverride, ArtParts). Mirroring it would mean adding a component and replaying the paint
  calls: not done.
- **The light the lamps cast** (the beams on the ground) and **boost particles**: the vehicle's `AttachedLightsComponent`s
  and effect components; the copy only shows the lamps' glow. Burst lamps' glow is an intensity entry like any lamp [I:
  boost not driven in a run].
- **Real delivery cargo on a vehicle**: the 100% save carries none (Sam's pieces are equipment), and no safe game call to
  put a piece into a vehicle's carrier was found (the script exports only move baggage to the player; the cargo menu's
  move was not automated). The sender's carrier walk ran on real pieces (Sam's equipment in the truck's slot 69, live
  world transforms, correctly not drawn); the wire and the props are proven with the synthetic pieces (`vehicle_sync 24`).
- The ArtParts shown bits of the copy (cargo racks), the hitch carrier of subtype-8 trikes (none in the save), the
  vehicle's subtype / colour word as an explicit field (the variations and the colour index carry them).

### 22.13 Final review of 22.11 / 22.12: collision everywhere, parked vehicles, model changes (after v0.13)

The user's request after v0.13: no hologram vehicles, each vehicle fully synced on both screens. A review of 22.11 /
22.12 was checked against the code, the logs and the screenshots. [V] seen in a log, a decompile or an enlarged
screenshot; [I] reasoned, not observed.

**Found and fixed**:
- **The gear props kept their collision** [V]. partner_gear's cutscene props (the case props for the cargo on the
  partner's back and on our own self-view body, the weapons, boots, the pod, the inventory items) were checked only for
  components (`ValidateProp`), while their physics sits on a model part (part `+0xD0`): `case_s` and `case_m` carry one
  instance each, so do some `ds_player_inventory` props (`physics instances 1 -> 0 of 7 parts`, run `205413`). Each gear
  prop now gets 22.11's `CollisionOff` when it is realized (not off: refused for the session, logged) and is checked again
  every second (`vsync::CopyCollisionOff` / `CopyCollisionKeepOff`, shared with the vehicle copies). Runs `205413` and
  `211715`: 16-17 gear props live on each side, 0 refused, 0 found on again.
- **Packs and cargo props were checked once** [V: code]. They are now checked every second like the gear props (the
  cargo props also right after their colour is applied, which rebuilds their materials); found on again: turned off
  again and logged; more than 3 times or not off: that pack kind / the cargo props refused for the game run. Every final
  run: `0 found on again` over 1029-2827 checks a run.
- **Spawned with live collision near the watcher** [V: code; I: the risk]. A copy, pack or cargo prop has its physics
  from its spawn until the realize tick turns it off (at most a tick). It was spawned 20 m below its place, which can be
  where the watcher stands (under a bridge). Now, when the watcher stands within 40 m of that point, it is spawned 60 m
  below the lower of the watcher and its place (`SpawnPlace`; `spawned 60 m below ...`, the first 5 logged, counted in the
  status line: 37 in the echo run `205413`, where the watcher is always near, 1-14 in the two-game runs). Not proven by
  a player standing below a spawn.
- **A standard trike, then the Kusanagi, kept the trike's look on the copy** [V]. Both are the trike copy, so the copy
  stayed and its paint, wear and lamps were the earlier trike's (DriveLook / DriveState apply nothing for the Kusanagi).
  The copy now remembers the drivable resource it plays; another model on the same copy removes it and it comes back in
  its own look (`the partner's vehicle changed from trike to trike_kusanagi (another model on copy 'trike'): the copy
  comes back in its own look`, run `205413`; `from trike_kusanagi to trike` on the guest in two games, run `210052`).
  `205413 automation\11-kusanagi-parked-65s` (enlarged): the copy beside our red Kusanagi is the plain grey cutscene trike,
  none of the purple / orange trike's paint.
- **A look applied again did not set the wear and lamps again** [V: code]. `DriveLook` now asks `DriveState` for a full
  apply whenever it applies a look (also the "again" path when the copy fell out of match).
- **Parked for 60 s / 30 m at most** [V]. A vehicle left by its driver is now reported while it is in his game's vehicle
  registry (vtable, per-creation GUID) and he stays within 250 m (`kParkedReportM`), with no time limit; a menu (no send
  tick, the same Sam) no longer forgets it, only a load does. Run `205413`: `our parked vehicle ... still reported after
  60 s` and `the partner's parked trike_kusanagi still shown after 60 s (copy 'trike')`, screenshot `11` at 65 s. Run
  `211715`: the guest still sees the host's parked roadster (its hologram) after he is placed away from it.
- **Counters and read-back** [V]. The status line splits the applies (`n full, n of changes only`); the read-back also
  checks the battery gauge colour (`gauge colours 2 of 2 as sent` on the truck, `1 of 1` on the trike).
- **Docs**: README, launcher.md and config.hpp said `vehicle_look` covers paint and variations only and not
  `vehicle_sync=3`: it also covers the wear, lamps and the cargo, and the cargo props are drawn on every copy (holograms
  too). 22.6's stale lines, 22.11's truck Climb-prompt claim (Sam out of view in `A4-before-truck`), 22.12's `case_m` and
  "53 full applies" are corrected above. `two-games-vehicle-sound` still waited for the truck hologram (run `211028`
  failed there, games fine): it waits for the solid truck now.

**Refuted / kept**:
- **"The roadster is still a hologram by default"**: true, and kept (logged as the fallback): no entity draws the
  roadster's solid art besides the drivable one; the art swap fails (22.11: the parts come from the ArtPartsData
  component, not the model resource); a per-instance copy of a game resource handed to the spawner would be released by
  the game's own reference counting (an allocation the game does not own) and was not attempted; a spawned
  `DSVehicleSpeedCar` is drivable (registry, interaction, physics) and is the pattern that crashed with weapons (17.1).
- **"The Kusanagi shows as a plain grey trike"**: true, and kept for the same reason (22.12); since this section it is
  at least never the previous trike's look.

**Final runs** (hidden, `-SeedSaves complete-100`, one build; every run: game exit 0 (both games where two), no
error.txt, real saves unchanged and restored, no violation, 0 cloud uploads):

| Run (`build-testenv\runs\`) | What it shows |
|---|---|
| **`20261118-205413-vehicle-echo-state`** (pass) | The fixes above (model change, 65 s parked, the hologram opt-in `09b` and back `09c`); the truck's wear 39 / 39, lights 11 / 11, gauge colours 2 / 2 in every read-back, the trike's 20 / 20, 7 / 7, 1 / 1; 194 applies (30 full, 164 of changes only), 0 failed, 0 mismatches; six case props read back 0.000 m; 54 collision offs (gear props, copies, packs, cases), 0 found on again over 2827 checks. |
| **`20261118-205827-vehicle-echo-solid`** (pass) | Truck: walk from 1.50 m before -> 8.01 m past (2.09 m beside), at its origin dz 0.11 (0.09 standing beside it); trike: -> 8.25 m past, at its origin dz 0.11. Our roadster through the pinned truck: 11.95 / 12.44 / 13.24 / 13.77 / 14.13 m/s mean from 6 m before to 6 m past, z step at most 0.002 m; through the pinned trike 7.90 / 9.26 / 10.30 / 11.26 m/s. Our own roadster flag 1, 15 instances: only the copies change. |
| **`20261118-210052-two-games-vehicle-trike`** (pass) | Guest: the host's roadster as `roadster_hologram` (logged fallback), his Kusanagi as the solid trike (`03-guest-sees-host-driving-a`, enlarged: Sam on the grey trike copy, `Host` tag), held 3 s and 12 s, his standard trike after `changed from trike_kusanagi to trike`; host: the guest's solid truck with its packs (`11-host-sees-guest-driving-a`, enlarged, `Guest` tag). Seat meter 0.0-0.1 cm; collision state flag 0, 0 instances on every copy, 0 found on again. |
| **`20261118-210439-two-games-vehicle-state`** (pass) | `guest\automation\02-guest-sees-host-truck-cargo` (enlarged): the host's truck copy rust-brown with its defense packs, red tail lamps, the six cases on its roof and the `Host` tag; the host's copy of the guest's trike read back 20 / 20, 7 / 7, 1 / 1; 0 failed, 0 mismatches, 0 refused. |
| **`20261118-210748-two-games-vehicle-look`** (pass) | The gold truck's and the purple / orange trike's looks applied (1 each side, 0 failed), 6 and 2 pack copies, 0 refused. The host's watcher views show the trike copy far off and behind a rock [I: which trike]. |
| **`20261118-211341-two-games-vehicle-sound`** (pass) | Regression: the guest posted 38 of the host's vehicle sounds and ended 38, 0 refused, 0 PostEvent failed; the host posted 5 of the guest's and still held 2 at the end (her truck is still reported parked). |
| `20261118-211028-two-games-vehicle-sound` (fail at a scenario step, games exit 0) | The scenario still waited for `truck_hologram`; fixed. |
| **`20261118-211715-two-games-release`** (pass) | Non-vehicle regression (cap, glasses, boots, grunts) with the gear props' collision off: 16 / 17 gear props live, 0 refused, `02-host-sees-guest-cap-glasses` (Fragile with cap and glasses); `10-guest-after-host-left`: his parked roadster's hologram still there. |

**Not proven / not done**: a solid roadster and the Kusanagi's own look (above); mud / snow / tar, the lamps' light on
the ground, boost particles (22.12); damage (a broken vehicle's copy goes after 3 s broken); real delivery cargo on a
vehicle (synthetic pieces only); a player standing below a spawn point; NPCs / BTs meeting a copy; the truck's Climb
prompt (out of view); only one copy at a time (a parked vehicle's copy goes when its driver gets on another one).

## 23. Shared real vehicles (`session.ini vehicle_sync=5`, design milestone M2: identity, spawns, removals, eviction; remote driving, ownership, state)

"There should only be 1 copy of each individual vehicle, synced completely with both players. Each player can summon
their own vehicle. No copies." With the host-held save session (`host_save=1`, [coop-session.md](coop-session.md)) both
worlds come from the host's save, so every saved vehicle is the same game unit in both games (same game-actor id, same
placement GUID, same place). `native/mod/vehicle_share.*` keeps it that way: each vehicle is ONE real game vehicle in
each world, bound by an identity; the guest's world never makes a vehicle of its own. The pure part
(`native/mod/vehicle_wire.*`: the messages, the binding and the host's rules) is unit-tested in `st_vehicle_wire_tests`.

Stage 1 (23.1 - 23.10): identity, the host's table after every guest load, spawn and removal sync, eviction refusal,
online vehicles hidden. Stage 2 (23.11 - 23.21, `native/mod/vehicle_drive.*`): remote driving with the game's NPC
driver controller, the correction bands, parked vehicles, ownership handoff (VOWN), life / broken / explode copies, the
partner body on the real vehicle, cargo props, lamps and looks on it. Riding together (M3) and hits (M4) are not part of
it.

### 23.1 The mode and its fallback (decision D2-1)

- `vehicle_sync=5` ("real") is the default when the key is absent, `host_save=1`, the role is host or guest, the co-op
  save session is on and every required byte check (23.7) passes. `vehicle_sync=5` written in `session.ini` asks for it
  explicitly (same conditions); any other value keeps the v0.14 copies.
- In it `vehicle_sync.cpp` spawns no copy (`[vehicle] ... no copy (real shared vehicles ...)`), but still sends STM1 (the
  seat and ride state for the partner body) and keeps the partner vehicle's place for its sounds and for the host's
  eviction refusal.
- **Fallback** (`host_save=0`, a failed check, `vehicle_sync=1|2`): the v0.14 solid trike / truck copies, and **no
  roadster and no hologram at all** (D2-1: "I don't want hologram vehicles"); a solid copy that cannot be made safe
  shows nothing. The holograms stay only in the opt-in modes 3 and 4. The reason is logged once
  (`[vshare] shared real vehicles OFF: <reason>; the v0.14 solid trike / truck copies stand in (no roadster)`) and shown
  for a minute in the co-op banner ("Shared vehicles off: ...").

### 23.2 Identity (M2.2)

- Key `VKey = {u32 the host's game-actor id (unit+0x08 = (actorType << 13) | slot; 0x20 roadster, 0x10 truck, 0x0F
  trike), u8 kind (1 roadster, 2 truck, 3 trike = EVehicleType = the pool index), u8[16] placement GUID (unit+0x188)}`.
  The host's ids are canonical; the guest keeps host id -> its own unit.
- **Pool poll** (both games, game thread, 10 Hz, only in the world): R = `[ds.exe+0x7BFECC0]`, pools `[R+0x28+i*8]`
  (i = 1..3, each checked by its vtable), count `[pool+0x40]`, `Unit** [pool+0x48]`, count and array read twice and
  equal; each unit's 0x1B0-byte block read twice (id, in-use, flags, GUID and entity ref equal). Fields: +0x08 id, +0x20
  in use, +0x24 actor state, +0x28 the unit's own transform, +0x68 entity ref (entity = ref - 0x20, checked against the
  three vehicle classes and `entity+0x8A8 == unit`), +0x150 health, +0x154 battery, +0x158 rider (0x1FFFFF none),
  +0x188 GUID, +0x198 flags (0x2000 remove request, bits 22..27 variant), +0x19C flag bytes (0x40 removed but
  remembered; +0x19E & 8 reserved), +0x1A2 record +2, +0x1A4 area. The transform is the entity's when its actor is in
  the world, else the unit's own (+0x28).
- **After every guest load** the guest asks (`SNAP_REQ`, once its world is ready and settled 5 s; again every 5 s until a
  complete table arrives, and every 30 s after); the host answers when its own world is settled with its whole table
  (`SNAP`, 9 units per chunk on channel 3: every in-use unit with its transform, flags, area, life, battery, rider).
- **Binding** (`vwire::PlanBinding`, every second and at once after a table, an ADD or a REM):
  1. an earlier id mapping (a fallback mirror) or the same placement GUID binds (the GUID is the proof);
  2. else the guest's unit with the same id binds when it is within 5 m (no position on one side: bound, unverified);
  3. the same id but another vehicle (> 5 m, or a GUID on one side only): a same-kind, no-GUID, spawned unit is
     **re-placed** onto the host's spot (RequestTeleport `0x142F7CA20`, at most once per 30 s per vehicle; proven in the
     drive2 proof), else **removed** (`unit+0x198 |= 0x2000`, the game's own removal request) and the host's one
     mirrored; a vehicle driven on the host (rider set) is bound, never re-placed (stage 2 follows it);
  4. the host removed it (removed but remembered): the guest's counterpart is removed;
  5. the guest has nothing for it: after a 10 s grace (placement vehicles stream in first), if it is in the guest's
     area and has a place, it is **mirrored** (23.3);
  6. guest units nobody claims are removed once nobody rides them (`+0x158 == 0x1FFFFF`); a unit a pending fallback
     mirror made is left alone.

  A rider (the guest's Sam or an NPC) always wins: nothing is removed or re-placed under a rider. Every removal re-reads
  the unit (id, in use, GUID) first and sets the bit with an interlocked OR.

### 23.3 Spawns (M2.3)

- **The guest's gate**: a detour on the pool enqueue `0x142F654E0`. The save's own spawns replay during the guest's load
  window (from the title until its world is ready + 5 s: they are the same as on the host and are bound after). After
  it, every request the mod did not issue (a thread-local flag) is dropped and, while the link is up and the first
  binding is done, forwarded to the host (`REQ`, at most 3 per 6 s): the request's transform, flags (+0x60), +0x54,
  +0x58, name, and the parking manager's slot when it came through the garage slot spawn `0x142D5CB20` (a second
  detour, guest only, records the slot and the transform passed to it). The banner says "Summoning the trike..." until
  the answer.
- **The host** refuses a request while its world is not settled or beyond 4 per 10 s; a duplicate (the slot's vehicle
  is out already, or one of that kind stands within 6 m) is answered `duplicate` and that vehicle announced again.
  Otherwise a garage slot of that kind spawns through `0x142D5CB20(PM, slot, xf)` (the host's own garage stock: D2-2),
  anything else through `0x142F830A0(R, record, xf, p4, 0, 0)` with the record rebuilt from the request flags (`p6`
  always 0; `vwire::RecordFromRequestFlags`, tested against the game's own formula for every bit pattern). The new unit
  (same kind, within 40 m, within 10 s) answers the request.
- **ADD** (host -> guest) for every new unit of the host's world once its actor is in the world (or after 2 s): its
  table entry plus the game's own persisted entry for it (0x74 bytes from the save writer `0x142F7CF20`), the record's
  +0 and p4.
- **The guest's mirror**, in this order, logged with its path:
  1. **exact id**: the persistence loader's call shape, pool vfunc +0x30 = `0x142DCFFB0(pool, desc, host id)` with the
     loader's 0x58-byte desc (type `0x4A91A5B3`, the transform, no placement, `{entry, current area}`) and the host's
     entry (online handle 0, category LOCAL, destroyed bit clear, its place moved to the newest one when an older ADD's
     differs by more than 1 m). It only takes the free unit with exactly that id and never evicts;
  2. **fallback**: `0x142F830A0` with `p6 = 0` after a free-unit check; the new unit near the spot is mapped to the
     host's id;
  3. **re-place**: a free unbound guest unit of that kind is moved there (RequestTeleport) and mapped.

  A path that gives no unit within 5 s (exact) / 6 s (fallback) hands over to the next; all failing waits 60 s.

### 23.4 Removals (M2.4)

- The host's poll sees a unit go (in use -> 0, or removed but remembered) and sends `REM` (from host); the guest removes
  its bound unit (0x2000) unless somebody rides it there.
- A bound guest unit that goes without the mod removing it is reported (`REM`, from guest). The host applies it only
  when the drive module says the guest owns the vehicle (`SetGuestOwnsQuery`; stage 1: never) AND its guard agrees
  (`vwire::HostRemovalAllowed`: in use, the same id, kind and GUID, not removed, within 5 m of the guest's position);
  otherwise it keeps the vehicle and announces it again (`ADD`, re-announce) and the guest mirrors it again.
- The guest asks with `NEED` for a vehicle it must mirror and has no entry for; the host answers with an `ADD`.

### 23.5 Eviction and online vehicles (M2.5)

- A detour on the victim chooser `0x142F7DB10` (its three callers handle 0: checked): the **guest** returns 0 always
  (never evicts); the **host** returns 0 for a shared-active unit: the partner's vehicle place (STM1, played) within 4 m
  of it, the partner body within 150 m of it in the last 60 s, or the partner drove it in the last 10 min
  (`vwire::SharedActive`; `NotePartnerDrove`).
- Online vehicles of other players (request flag 0x800, the `0xC0A` requests) are dropped at the enqueue on both games
  while the sync is on (D2-3). `0x142F830A0` can never set 0x800 (tested), so the sync's own spawns are never trimmed.

### 23.6 Wire: SVM1 on the reliable ordered channel 3 (`link::SendVehicleEvent` / `PopVehicleEvent`)

Every message: `"SVM1" | u8 type | u8 version 1 | u16 0`, then the body (little endian, reserved bytes zero, exact
sizes, strict decoders). Append only (the channel mixes types). A new link session clears both sides' tables (the guest
asks again).

| Type | Direction | Body |
|---|---|---|
| 1 SNAP_REQ | guest -> host | u32 req id, u32 the guest's loaded save counter, u8 reason (0 load, 1 periodic, 2 test), 3 x 0 (12 B) |
| 2 SNAP | host -> guest | u32 snap id, u32 req id, u32 the host's load counter, u16 area, u8 index, u8 count (<= 32), u16 total (<= 256), u8 n (<= 9), u8 0, then n x 108-byte entries |
| 3 ADD | host -> guest | u32 token (the guest's request, else 0), u8 source (0 own spawn, 1 the guest's request, 2 re-announce), u8 has entry, u16 record +0, u8 p4, 3 x 0, a 108-byte entry, the 0x74-byte persisted entry (zeros without) (236 B) |
| 4 REM | both | u8 reason, u8 from (0 host, 1 guest), u8 has pos, u8 0, VKey (24 B), f64 x3 (52 B) |
| 5 REQ | guest -> host | u32 token, u8 kind, i8 garage slot (-1, 0..8), u8 reason, u8 0, u32 request flags, u16 request +0x54, u16 0, u64 request +0x58, f64 x3, f32 x9, char[32] name (116 B) |
| 6 RESULT | host -> guest | u32 token, u8 result (spawned, refused, failed, timeout, duplicate), u8 path, u16 0, VKey (32 B) |
| 7 NEED | guest -> host | VKey, u8 reason, 3 x 0 (28 B) |
| 16..63 | either | the drive module's (ownership 16..31, driving 32..63): `vshare::RegisterHandler` / `SendEvent` |

VKey on the wire: u32 id, u8 kind, u8[16] GUID, 3 x 0. Entry (108 B): u32 id, u8 kind, u8 actor state, u8 flags (1
entity, 2 transform, 4 removed, 8 reserved, 16 ridden), u8 0, GUID, f64 x3 position, f32 x9 rotation (zeros without a
transform), u32 +0x198, u8 x4 +0x19C, u16 area, u16 +0x1A2, f32 life, f32 battery, u32 rider.

### 23.7 Byte checks (`kSites` in vehicle_share.cpp: 44 at start; a failed required group turns the mode off)

| Group | Sites |
|---|---|
| core (required) | the manager global (`0x142F7509C`, RIP target `0x147BFECC0`), pools at +0x28 (`0x142F6C86F`), pool +0x40 / +0x48 and unit +0x20 / +0x08 (`0x142DCFFB0`, `0x142DCFFD7`), unit release and its 0x2000 test (`0x142F69A70`, `0x142F69A9D`), entity ref +0x68 (`0x142F69AA9`), the game's removal write (`0x142FA7AB1`), entity +0x8A8 (`0x142F761A9`), the unit transform getter +0x28 (`0x141A87230`, and slot +0x18 of the unit vtables `0x143E26070` / `0x143E26A48` / `0x143E27790`), the current area (`0x14262A860`), the victim chooser's in-use / reserved / rider reads (`0x142F7DC53`, `0x142F7DC59`, `0x142F7DC62`), the unit spawn's GUID +0x188 / area +0x1A4 writes (`0x142F6DA80`), the pool vtables `0x143E278E0` / `0x143E26B98` / `0x143E261C0` (+0x30 = `0x142DCFFB0`, +0x150 kind getters `0x141925D10` / `0x141921DC0` / `0x141921EF0`) |
| gate (required) | the enqueue `0x142F654E0`, the queue's online flag test at request +0x60 (`0x142F98FF0`) |
| spawn (required) | `0x142F830A0`, its empty string (`0x142F830FC` -> `0x144562CB0`), its record +8 / +0x10 / +0 / +2 / +0x14 reads (`0x142F8315E`, `0x142F8316E`, `0x142F8317B`), the flags mask (`0x142F831BC`) |
| evict (required) | `0x142F7DB10`, its three callers (`0x142F867BD`, `0x142F867FF`, `0x142F993C9`: the call target and the 0 test) |
| teleport (required) | RequestTeleport `0x142F7CA20` |
| exact (optional: else the fallback mirror) | the loader `0x142F70660`, its desc type (`0x142F70819`) and vfunc +0x30 call (`0x142F70893`), the entry writer `0x142F7CF20` |
| garage (optional: else plain spawns) | `0x142D5CB20`, the parking manager global (`0x142D6ECEA` -> `0x147BF5798`) |

### 23.8 API for the drive module (vehicle_share.hpp)

- `FindByKey(VKey, Unit*)`, `FindByHostId(id, Unit*)`, `FindByEntity(entity, Unit*)`: this world's unit, entity, local
  id, shared key, transform and rider (from the newest poll plus a fresh id / in-use check); `Bound()`.
- `RegisterHandler(type 16..63, fn)` / `SendEvent(msg)` (a full SVM1 message: `vwire::WriteHeader` + body): channel 3,
  in order behind vehicle_share's own messages.
- `SetGuestOwnsQuery(fn)` (the host applies the guest's removals of what it drives), `SetFollowedQuery(fn)` (the binding
  never re-places a vehicle the drive module moves), `NotePartnerDrove(key, now)` (eviction refusal for 10 min).
- `RequestTeleport(entity, xf)`: the byte-checked deferred vehicle teleport.
- Added in stage 2: `SharedUnits` (every shared vehicle of this world, keyed by the host's id), `FindNear(kind, pos, m)`,
  `Footprints` (the teleport clearance), `NoteHostPlace(host id, xf)` (guest: the table entry follows the owner's frames),
  `WorldReadyNow`, `Role`; `vehicle_share 8 [kind] [m]` logs the host's persisted entries (the save writer's place).

### 23.9 Evidence (hidden, `two-games-veh-identity`, `-SeedSaves complete-100 -GuestSeed profile-only`)

Run `20261119-095315-two-games-veh-identity`: pass, exit 0 (both games), real saves unchanged and restored, 0 uploads,
no `[vshare]` fault.

- 44 byte checks passed on both games; the copies off (`vehicle sync real`).
- After the guest's load: the host's table, 22 vehicles in 3 chunks; **all 22 bound at 0.00 m** (same ids, flags,
  GUIDs, actor states); nothing re-placed, removed or mirrored. Both T1 dumps are equal.
- The guest's game dropped 3 online-vehicle requests (D2-3).
- The guest summoned a trike through its game's own request path (`vehicle_summon 3`): its gate dropped it and sent
  `REQ #1`; the host spawned trike `0x01E001` (a record spawn) and announced it with its entry 0.1 s later; the guest
  mirrored it by the **exact-id path** (`0x142DCFFB0` with the host's entry) in 0.1 s: the same id `0x01E001`, its
  actor in the world at once, its class, 0.01 m from the host's spot, life 7500, battery 1.00, flags `0x20410100` as on
  the host. T2: 23 units in both worlds, one new trike in each.
- The mirror stayed at the spawn height (0.48 m above the host's, which fell to the floor): the exact-id spawn appears
  to start with its physics at rest (as a persisted vehicle). The drive module's correction places it.

### 23.10 Not done / not proven (stage 1)

- Remote driving, ownership (VOWN), life / battery / broken / explode copies, cargo props on the real vehicle (D2-4) and
  the vehicle sound mirror for real vehicles: stage 2 (23.11 - 23.21). The snap back to the host's place at the guest's
  next table is fixed there (23.13: the table entry follows the owner's frames; the binding leaves a followed vehicle
  alone).
- The garage relay is not proven (the summon above found no garage slot of that kind and took the record path), and the
  guest's garage UI may show a stock change the host did not make; the garage-store function is not found.
- Not run: the fallback and re-place mirror paths, removal sync, the eviction refusal under pressure, a vehicle far from
  the guest (its exact-id mirror is logged complete when its actor comes), MULE-taken and NPC-driven vehicles.

### 23.11 Remote driving (M2.6; stage 2, `native/mod/vehicle_drive.*`)

The game whose Sam holds the driver seat **owns** the vehicle; when nobody drives it the host owns it (its world is the
persisted one). The other world **follows** with its own unit of the same vehicle: one real vehicle per world, never a
copy, never keyframed.

- **Sender** (the pose send tick, game thread): while our Sam drives a shared vehicle (his parent entity +0x70 is the
  vehicle, `+0x63C != 0`, the driver id `+0x8B0` is not 0x1FFFFF) and we own it, one STD1 **DRIVE** frame per send tick
  (30 Hz) on the pose's `sender_ms` axis: the 48 input bytes the game feeds the vehicle (`[vehicle+0x528]` +0x08..+0x37,
  read twice and equal, checked: 11 finite floats |x| <= 4, bytes +0x34..+0x36 0/1, +0x37 cleared), the transform,
  velocities measured from successive reads, speed (`[[vehicle+0x330]+0xF0]`, km/h), life / max, the ride state and
  state bits.
- **Follower** (the other game): the frames are played back at the partner body's play time (`interp_delay_ms`, the
  same as the body). When a DRIVE frame plays and the vehicle is spawned here, nobody holds its seat (`+0x63C == 0`, not
  broken) and our Sam is not on it, the mod attaches **the game's NPC driver controller**, the proven recipe: a 0x2000
  byte VirtualAlloc block, ctor `0x142F51C60`, `Attach 0x142F5A590(ctl, vehicle, &0x1FFFFF)`, `SetEngine 0x142F5A310(ctl,
  1)`; then `[vehicle+0x528] == ctl+0x30` is checked. The input object gets the mod's copy of vtable `0x143E229B8`
  (checked slot by slot) whose slot +0x08 calls the game's `0x142F54440` (reset and route follower, run by the vehicle's
  update `0x142F8D920` before every physics step) and then copies the received 48 bytes over +0x08..+0x37, read through
  a sequence lock from a buffer at block+0x1800 (the vehicle may update on another thread). Each tick the inputs of the
  frame at the play time go into that buffer; frames older than 300 ms give an idle driver (no throttle, hold on).
- **Vehicles without an AI drive component** (the trikes, run 20261119-111839): Attach looks the component up
  (`0x1422B2A60(vehicle+0x90, key 0x144B35D30)`) and calls into it unchecked (`0x14242D660`, then `[+0x30]`); without
  one it faults (the game's crash reporter ran for 44 s) after it already took the seat. The mod repeats the lookup
  first: a component with a route source takes the full Attach (the truck, the roadster); none takes the **lite
  attach**: the ctor, SetDriver (`vehicle vt+0x110`), the vehicle handle at ctl+0, SetController, ctl+0x100 = 0 (the
  input reset then returns before the route part), ctl+0x1C / +0x18 / +0x20 as Attach writes them, input+0x38 = 0, then
  SetEngine. Input+0x38 is 1 from the ctor and the vehicle update reads the inputs only while it is 0 (`0x142F8DA1B`);
  after the full Attach it is 0 (which call clears it was not traced). Without that write the trike's follower got 0
  input updates in 44 s (run 20261119-113138); with it 1341 (run 20261119-113626).
- **Detach** when the partner's frames end (a RELEASE, a newer PARK frame, nothing for 1.5 s), our Sam takes the seat,
  the vehicle changes or goes, the world reloads or the link session changes: the vtable restored, `SetEngine 0`,
  `Detach 0x142F5A3D0`, dtor `0x142F521F0`, the block freed. On the host the vehicle then goes back on the recent
  player vehicles (Touch `0x142F52740(R+0x58, unit, 0)`: the NPC attach took it off). A vehicle that is no longer alive
  (entity gone) is never called into: its block is left allocated.
- **The lock** (M2.7): while the follower holds the seat the local get-on fails (SetDriver refuses `+0x63C != 0`, checked
  at `0x142F7C50F`): one driver at a time.

### 23.12 Corrections (M2.6 bands)

Against the owner's frames at the play time (DRIVE: interpolated, PARK: the newest), at most every tick (driven) / every
100 ms (parked):

| Error | Driven (the partner drives it) | Parked (the owner's PARK frames) |
|---|---|---|
| < 0.3 m (driven) / < 0.1 m (parked), and < 3 deg | nothing (the input replay alone stays within ~0.1 m: proof drive, smoke run 23.20) | nothing |
| < 2 m and < 45 deg | the clamped velocity nudge, at most every 100 ms: the game's drive-to-transform `0x141C43E10` toward where the vehicle should be one step on (the owner's velocity plus what closes the error in 0.5 s, at most +2 m/s; the rotation turned by the owner's yaw rate plus at most 1 rad/s), after the instance's activate (vt+0xE0); when both its vehicle and ours stand still (under 0.3 m/s), the teleport as parked (the nudge left a stopped truck 0.48 m off for 12 s: run 20261119-110534) | RequestTeleport onto the owner's place (at once from 0.25 m or 10 deg, after 1.5 s below that; at most every 2 s). The game's net correction `0x1423B80E0` and the nudge did not close a parked error (run 20261119-105428: a truck summoned into a trike went from 0.5 to 1.27 m, pushed up onto it, after 69 native calls and 54 nudges); they are used for parked vehicles only with `vehicle_drive 2 1 1` |
| 2..5 m, or >= 45 deg | RequestTeleport `0x142F7CA20` (at most every 0.5 s) | the same |
| > 5 m | RequestTeleport (at most every 1 s) | the same |

A teleport only where the vehicle fits: its footprint at the target (an oriented box per kind, `vwire::TeleportClear`)
must hold neither the local Sam (with 0.6 m) nor another vehicle; otherwise it waits (logged every 3 s). A vehicle the
partner owns that stands within 0.5 m of its newest PARK frame does not count: the owner's world holds both there (two
summoned vehicles side by side blocked each other's teleport for good before, run 20261119-105928). Deviation from
the design: the native correction's 2..5 m branch (`0x1422DFF80` + `0x1423964D0`) set the entity's transform but not the
vehicle's physics (47 calls, 0.7 m / 131 deg unchanged, run 23.20), so RequestTeleport takes that band. Every second a
`[vdrive] <kind> <id> followed|parked: placement error mean / max, angle max, corrections per band` line; `action
vehicle_drive 1` sums them per vehicle. Debug: `vehicle_drive 2 0|1 [0|1]` native on / off (the second value: on parked vehicles too), `3 <parked m> <driven m>` the
ignore bands, `4 0|1` corrections off / on (hidden runs: the open-loop replay).

The Mat44 the mod builds for the game's correction calls copies the three rotation rows as they are (`0x1419C8510`:
`out[4] = rot+0x0C`, `out[8] = rot+0x18`). An earlier transposed copy drove every corrected vehicle toward the inverse
heading (a summoned trike and truck 69 deg off, run 20261119-104937).

### 23.13 Ownership (M2.7, VOWN on channel 3)

| Event | What happens |
|---|---|
| The guest's Sam gets on a vehicle the host owns | CLAIM (repeated every 1.5 s, at most 5, while seated); the host GRANTs (owner guest, epoch + 1) unless its own Sam holds it or got on within 300 ms (DENY); until then the guest's frames are not followed |
| The host's Sam gets on a vehicle the guest owns (both pressed X within the latency) | the host takes it (GRANT owner host, epoch + 1): the host wins every tie |
| DENY / GRANT(host) while the guest's Sam is on it | the guest's Sam is put off with the game's own `RideOffVehicleForcibly 0x1426E7100` (`[[0x147BE7980]+0x206D1] |= 8`, once a second, at most 6 times) |
| The owner's Sam gets off | RELEASE (got off, with the final transform and velocity); the follower lets go after the last frames play; the owner keeps sending PARK frames |
| The guest's vehicle at rest (< 0.2 m/s for 1 s) after its Sam got off | RELEASE (at rest; again every 3 s, at most 5) -> the host GRANTs it back (handback); the host's PARK frames rule from then |
| No frame from the guest owner for 10 s (its world reloaded, it left) | the host takes it back (GRANT, timeout) |
| The non-owner's Sam pushes a parked vehicle (within 6 m of it, the vehicle > 0.1 m off the owner's place and moving > 0.3 m/s here) | guest: CLAIM (push), granted; host: takes it |
| A new link session, a world reload | every vehicle is the host's again (epoch 0), followers detach |

Frames carry the owner's epoch: the host follows the guest's only with the current epoch; a host frame with a newer
epoch tells the guest the host took it back. The rules are pure (`vwire::HostOnClaim`, `HostOnHostTakes`,
`HostOnRelease`, `HostOnSilence`, `GuestOnAnswer`, `GuestOnHostFrame`, `RestDetector`) and unit-tested. Vehicle_share's
queries are answered from this: the host applies a guest's removal of a vehicle the guest owns (`SetGuestOwnsQuery`);
the binding never re-places or removes a vehicle the drive module owns or follows (`SetFollowedQuery`; frames in the last
5 s); the guest's table entry moves with the owner's frames (`vshare::NoteHostPlace`), so a vehicle driven away is never
put back onto its old spot at the 30 s table refresh (the stage-1 gap). The host's eviction refusal counts a vehicle the
guest drives (`NotePartnerDrove`).

### 23.14 State: life, battery, broken, explode (M2.8, D2-5)

- **Life** (`+0x6E8`, max `+0x6EC`): the owner's value, written on the follower when it differs by more than 0.5 (every
  100 ms); the unit's health (`+0x150`, what the save keeps) too when it held the same value (the game set the entity's
  back from it after an entity-only write: run 23.20). Life 0 from the owner breaks the follower through its own
  ride-state machine. Hidden tests: `vehicle_drive 7 <life>` sets the life of the vehicle our Sam drives (the owner's
  damage).
- **Battery**: the field the ride-state machine tests (`+0x5A4 <= 0` while parked: state 6, checked at `0x142F9BD50`)
  is probed at start against the unit's persisted 0..1 battery (`+0x154`); it is copied only when the two agree on two
  vehicles. Run 23.20: `+0x5A4` read 1.00 on every vehicle against 0.663 / 0.000: **not copied** (the probe now refuses
  it; `vehicle_drive 8` reports it).
- **Broken** (`+0x560 & 0x2000`) and **exploded** (`+0xBD8 & 0x100000`, latched by the state machine): the owner's
  frames carry them; on the first frame that has one the follower ORs the same request (`+0x560 |= 0x2000`; `+0x564 |=
  0x200000`, the explosion request the state machine consumes at `0x142F9BC70`), so its own state machine plays the
  effects. Once per follower attach / park sequence.

### 23.15 The partner body on the real vehicle; vehicle_sync=5 (M2.9)

- While the partner drives a vehicle we follow, its body is placed at **our real vehicle's transform times the
  partner's seat** (the STM1 frames at the play time, `vsync::PartnerSeatAt`), one frame ahead along the vehicle's
  measured velocity (the vehicle moves by its own physics later in the frame than the body is placed). The partner
  capsule rules are unchanged (`kPoseInVehicle`: partner_collision takes the body's capsule out).
- In mode 5 the cutscene copies, the overlap hiding, the STM1 joint writes and every hologram are not used; STM1 still
  carries the seat, the ride state, the lamps and the cargo.
- **Fallback** (D2-1, 23.1): with `host_save=0` or a failed vehicle_share check, the v0.14 solid trike / truck copies
  and no roadster. A failed vehicle_drive check (23.18) keeps the vehicles shared without remote driving, with a one-line
  reason in the log and the co-op banner ("Shared vehicles: remote driving off: ...").

### 23.16 Looks, lamps, sounds and cargo on the real vehicle (M2.8, D2-4)

The partner's real vehicle in our world is the one we follow while it drives, else the shared vehicle of its kind
nearest the STM1 frames' place (within 3 m: parked).

- **Cargo** (D2-4): the STM1 cargo block's pieces become the v0.14 case props (collision off, checked every second)
  placed on that real vehicle, except the pieces our own world's vehicle carries already (the same model within 0.35 m in
  the vehicle's frame, read every 500 ms from its DSBaggage2 carrier). Real cargo sync is later.
- **Lamps**: the follower's engine runs (the NPC controller's SetEngine 1), so its own lamps should follow [I]. Until a
  hidden run shows them, the v0.14 lamp writes (the STM1 state block's lamp intensities and gauge colours, not the wear:
  the same vehicle in both worlds) go onto the real vehicle (`[vehicle] the partner's lamps on our real vehicle`).
- **Engine and road sounds**: the follower's own [I]; the STA1 vehicle-sound mirror stays on until proven.
  `session.ini vehicle_real_native=1` (mod key; also `vehicle_drive 5 1`) turns the lamp writes and the STA1 vehicle
  sounds off for the real vehicles (`vsync::MirrorVehicleSounds`).
- **Looks** (paint, variations): the same save gives the same looks. On the **guest** only (the host's world is the
  persisted one), the host's STV1 look of the vehicle it reports is applied to our real one when ours differs
  (`look::SatisfiedVehicle` every 2 s, at most 5 applies per vehicle, with the v0.14 look code). No VLOOK message of its
  own: STV1 item 255 carries the look and its hash already. Packs are the real vehicle's own (no copies).

### 23.17 Wire: STD1 (unreliable FRAME) and VOWN (channel 3)

STD1, one FRAME message in a datagram of its own (`link::SendDrive` / `PopDrive`: raw bytes queued after a magic and
size check, decoded on the game thread by `vwire::DecodeDrive`), little endian:

```
"STD1" | u8 version 1 | u8 type (1 DRIVE, 2 PARK) | u8 flags | u8 ride state (0..15) | u32 seq | u32 sender_ms
| u32 epoch | VKey (u32 id, u8 kind, u8[16] GUID, 3 x 0) | f64 pos[3] | f32 quat[4] (x, y, z, w of the rotation rows)
| f32 vel[3] | f32 angular vel[3] | f32 speed km/h | f32 life | f32 life max | f32 battery
| [DRIVE] u8 inputs[48]                                                  (PARK 124 bytes, DRIVE 172 bytes)
flags: 1 seated (DRIVE only, always there), 2 engine, 4 broken, 8 exploded, 16 at rest, 32 battery valid
```

Strict decoder: the magic, version 1, the type's exact size, known flags, ride state <= 15, a valid key, |pos| <= 1e7, a
unit quaternion within 10 %, |vel| <= 200 m/s, |angular| <= 50 rad/s, |speed| <= 1000, 0 <= life <= max <= 1e6, a
battery only with its flag, valid inputs. Rate: DRIVE 30 Hz while driving (~5.2 KB/s); PARK 1 Hz per owned vehicle
within 300 m of either player and at once on a change of 5 cm / 2 deg (10 Hz while it moves), at most 8 frames per send
tick. Deviation: VPARK goes as an unreliable STD1 PARK frame (it repeats every second) instead of a channel 3 "latest"
message (channel 3 is append only: 23.6).

VOWN, channel 3 types 16..19 (`vshare::RegisterHandler` / `SendEvent`): the SVM1 header, then `VKey (24) | u8 owner (0
host, 1 guest) | u8 reason | u8 has transform | u8 0 | u32 epoch | u32 sender_ms | f64 pos[3] | f32 quat[4] | f32 vel[3] | u32 world`
(92 bytes; the world since 23.23). 16 CLAIM (guest -> host; reason 0 get on, 1 push), 17 GRANT (owner now; reason 0 claim, 1 the host took it,
2 handback, 3 timeout), 18 DENY (owner host; reason 0 the host drives, 1 the host was first, 2 busy, 3 the guest's Sam not at it), 19 RELEASE (the
owner; reason 0 got off, 1 at rest; the transform). Unit-tested in `st_vehicle_wire_tests`.

### 23.18 Byte checks (`kSites` in vehicle_drive.cpp: 43 sites + 3 structural at start; the 3 NPC-bit sites since 23.23)

| Group | Sites |
|---|---|
| controller (required) | ctor `0x142F51C60`, Attach `0x142F5A590`, SetEngine `0x142F5A310`, Detach `0x142F5A3D0`, dtor `0x142F521F0`, the input reset `0x142F54440`, the vehicle update that calls it `0x142F8D920`, SetDriver `0x142F7C4C0` / truck `0x142F7C310` and its refusal of a held seat or state 5 (`0x142F7C50F`), SetController `0x142F80310`, is-driven `+0x63C` / `+0x528` (`0x142F6F4A0`), the ride-state machine `+0x4CC` (`0x142F9BC20`) and its driver-id test `+0x8B0` (`0x142F9BD76`); structural: the input vtable `0x143E229B8` slots 0..4 and the vehicles' SetDriver slot +0x110 |
| ride-off (required) | `0x1426E7100` and its global (RIP target `0x147BE7980`) |
| state (optional: no state copy) | life `+0x6E8` (`0x142F9BC40`), broken (`0x142F9BC54`), explode / latch (`0x142F9BC70`), speed `[+0x330]+0xF0` (`0x142F8AC5D`) |
| battery (optional) | `+0x5A4` (`0x142F9BD50`) and the run-time probe (23.14) |
| touch (optional, host) | `0x142F52740` |
| native (optional: nudge / teleports) | the net correction `0x1423B80E0`, first physics instance `0x14234DCD0`, Mat44 inverse `0x141940080`, the WorldTransform -> Mat44 layout the mod builds (`0x1419C8510`) |
| nudge (optional) | drive-to-transform `0x141C43E10`, Mat44 multiply `0x1419333D0`, Mat44 -> WorldTransform `0x1419C7000`, `0x14234DCD0` |
| controller, added (required) | the component lookup `0x1422B2A60`, Attach's lookup site `0x142F5A678` (entity+0x90, key `0x144B35D30`), the route source `0x14242D660` (+0x78 / +0x98 / +0x88) |
| lite (optional: vehicles without the AI drive component are not followed) | handle addref `0x14191C790` / release `0x14191C810`; Attach's writes it replays: `0x142F5A624` (handle), `0x142F5A65A` (SetController), `0x142F5A68B` (ctl+0x100), `0x142F5A6B2` (ctl+0x1C), `0x142F5A785` (state, driver id); the update's input+0x38 test `0x142F8DA01` |

A failed required group: remote driving off (`[vdrive] remote driving OFF: ...`), the vehicles stay shared.

### 23.19 Hidden scenarios (all `-SeedSaves complete-100 -GuestSeed profile-only`, `ini host_save=1`)

| Scenario | What |
|---|---|
| `two-games-veh-identity` | 23.9, plus the host's PARK frames and `vehicle_drive 0 / 1` |
| `two-games-veh-summon` | the guest summons a trike (REQ, the host's spawn, ADD, the mirror path), the host a truck; the mirrors placed by the PARK frames; `vehicle_share 8` the host's persisted entries |
| `two-games-veh-drive` | the host drives the truck (straight, a turn, a reverse, a hard stop) with the guest's camera NEAR it (`vehicle_drive 9`: the watcher 9 m beside the real vehicle it follows, then `vehicle_sync 18` aims the camera) |
| `two-games-veh-drive-guest` | the same, the guest drives, the host follows |
| `two-games-veh-drive-trike` | the host drives the saved Kusanagi trike outside (lite attach) |
| `two-games-veh-drive-roadster` | the host drives the garage's roadster; the watcher is placed without the ground snap (`vehicle_drive 9 <m> 1`) |
| `two-games-veh-handoff` | host drives / gets off, the guest takes the wheel (CLAIM, GRANT, handback at rest), the lock, both press X together |
| `two-games-veh-remove` | a host trike removed with the game's removal request (REM), the truck destroyed by its driver's game (life 0) |
| `two-games-veh-persist` | the guest drives a host truck ~100 m and parks it; handback; the host's persisted entry holds the parked spot |

New debug: `vehicle_drive` (debug-actions.md), `vehicle_share 8 [kind] [m]` (the host's persisted entries).

### 23.20 Evidence (stage 2 smoke run)

Run `20261119-103440-two-games-veh-drive` (an earlier version of the scenario; one foreground hidden run): pass, exit
0 (both games), real saves unchanged and restored, 0 uploads, no `[vdrive]` / `[vshare]` fault.

- 32 drive byte checks passed on both games; 22 vehicles bound at 0.00 m.
- The host drove truck `0x020005`: the guest's follower attached at once (the game's NPC controller, driver 0x1FFFFF,
  `[+0x528] == ctl+0x30`, the input vtable ours); 2040 input updates in 34 s. **Driving straight and into the turn the
  guest's truck stayed within 0.04..0.13 m of the host's, angle under 1 deg, with no correction at all** (the input
  replay alone).
- The host's turn ran the truck off the road into rocks (the old scenario's full lock); the two worlds' crashes then
  differed (as in the proof): 1..4.75 m, large angles. The native correction's 2..5 m branch did not move the vehicle
  (47 calls); RequestTeleport takes that band now (23.12). The guest's own crash damage (life 431 -> 421) was overwritten
  with the host's but came back: the unit's health is written too now (23.14).
- The release: the follower detached when the host got off (`the host released truck ... (got off)`), then re-attached
  for 0.1 s on a stale frame: fixed (no attach on frames older than 300 ms, the release marker kept).
- The PARK frames kept the 6 other vehicles near both players at 0.00..0.02 m.
- The guest's camera in that version did not follow the truck after it drove off (it aimed at the nearest parked
  truck): the scenario now re-places the watcher beside the followed vehicle before each screenshot.
- The partner body sat at the truck's cab just after the get-on (D01, enlarged: its head above the roof line, likely
  still in the get-on animation): to check in the full run.

### 23.21 Not done / not proven (stage 2; see 23.22 for what the full runs proved since)

- Every scenario of 23.19 but one early drive run is still to run (the next stage); the guest-drives, trike, roadster,
  handoff, race, push, remove, destroy and persistence paths are unproven in a game.
- The native net correction's velocity drive (parked < 2 m) and the clamped nudge (driven) were called only a few times;
  their effect is unmeasured. The native 2..5 m branch is replaced by RequestTeleport (23.12).
- The lamps and engine / road sounds of the follower are not proven native: the v0.14 lamp writes and the STA1 mirror
  stay on (`vehicle_real_native=0`).
- The battery is not copied (the probed field does not follow the unit's battery).
- The seat placement of the partner body in the real vehicle (the STM1 seat) and the camera near it: to check enlarged.
- A vehicle driven out of the other world's streaming range has no actor there: nothing follows it; when it comes back
  it is put onto the owner's place (a teleport where it fits). The host's persisted place of a vehicle the guest drove
  while its actor was not spawned on the host is not updated (no write to the unit's own transform); since 23.23 the host
  keeps the guest's place of it (snapshot, and a teleport there when its actor is back).
- The NPC attach sets `+0x408 |= 0x10` and `[+0x898]+0x48 |= 2` (NPC-driven) on the host's vehicle; Detach does not
  clear them [I: effect on AI and the save unknown]. Measured since (23.23): after the game's Detach both read 0 on the
  truck, the trike and the roadster; the truck keeps `+0x408 & 0x08` (0 before the attach) [I: what it is]. The mod
  clears the two NPC bits after a detach anyway (a guard), as SetDriver's own get-off path does.
- The garage relay (23.10), MULE-taken and NPC-driven vehicles, riding together (M3) and hits (M4) are not part of M2.
- No forced save: the persistence scenario checks the host's persisted entry, not a reload.

### 23.22 Hidden proof of M2 (2026-11-19, every scenario of 23.19 foreground, one at a time)

All runs `-SeedSaves complete-100 -GuestSeed profile-only`, `ini host_save=1`, `ini partner_body=real` (the launcher's
default; the earlier scenarios ran the hologram body), the final build (the trike, roadster and remove runs one build
earlier: without the handback placement and the aim fallback below, neither on their paths). Every one: result pass, both games exit 0, real
saves unchanged and restored, no violation, 0 cloud uploads, no `[vshare]` / `[vdrive]` FAULT.

| Run | Result |
|---|---|
| `20261119-120812-two-games-veh-identity` | 22 bound at 0.00 m (one truck 0.08 m), T1 22 = 22 units, 3 online requests dropped on the guest; the guest's trike summon mirrored by the exact-id path (same id 0x01E001); T2 23 = 23 |
| `20261119-120948-two-games-veh-summon` | trike (guest REQ) and truck (host) each exactly one new unit per world (T1 22 -> T2 23 -> T3 24 on both, same ids); the exact-id mirrors placed by parked teleports (trike mean 0.03 m; the truck, summoned into the trike, is pushed by it in the guest's world and kept at mean 0.14, max 0.43 m by a teleport every ~2 s) |
| `20261119-121145-two-games-veh-drive` | host drives the truck, guest follows: moving windows mean 0.18 m (max 0.71 m, 14 deg after the turn into the rocks), stopped 0.11 m; 1 teleport (stopped), no teleport while driving straight; the partner seated in the cab (D01 enlarged) |
| `20261119-121403-two-games-veh-drive-guest` | guest drives, host follows: CLAIM / GRANT / handback at rest; moving mean 0.30 m (max 0.76 m), stopped 0.12 m |
| `20261119-113626-two-games-veh-drive-trike` | the Kusanagi trike by the lite attach: moving mean 0.20 m (max 0.66), stopped 0.05 m; the partner riding it (T02a) |
| `20261119-114220-two-games-veh-drive-roadster` | the roadster in the garage: moving mean 0.16 m (max 0.64), stopped 0.09 m; Host seated, the guest beside it |
| `20261119-121645-two-games-veh-handoff` | host drives / gets off, the guest claims and drives, handback at rest, the host drives with the guest pressing X beside the truck (not boarded: the lock), both press X: the host boards, the guest does not; never two drivers |
| `20261119-115836-two-games-veh-remove` | REM: the guest's trike removed ~0.1 s after the host's (clock-aligned by the paired dump actions); life 0 on the host's truck -> the guest's 0.15 s later, its ride state 5 (broken) |
| `20261119-120536-two-games-veh-persist` | guest drives the truck ~85 m and parks; handback; the host places its truck where the guest parked it (0.17 m, new); T2 dumps both (-1285.24, 280.79), the host's persisted entry the same place |

Fixes found by these runs (all in the working tree):

- **The Mat44 for the game's correction calls was transposed** (`MatFromWt`; `0x1419C8510` copies the rotation rows as
  they are): every corrected vehicle was driven toward the inverse heading (69 deg on the summoned trike and truck).
- **Parked vehicles**: RequestTeleport instead of the native correction / nudge, which never closed a parked error
  (23.12); the same for a stopped vehicle with its driver.
- **Teleport clearance**: a partner-owned vehicle within 0.5 m of its newest PARK frame no longer blocks one (two
  summoned vehicles side by side blocked each other for good).
- **The lite attach** (23.11) for vehicles without the AI drive component, and the lookup precheck: the trike's Attach
  had faulted inside the game.
- **Handback**: the host puts its unit where the guest parked (the RELEASE's final place) instead of pulling the
  guest's truck 0.2 m back to the host's follower.
- Scenarios: `ini partner_body=real`; `vehicle_sync 18 6` aims at the followed vehicle (it is not in the registry
  list while the NPC controller holds it); `vehicle_drive 9 <m> 1` without the ground snap (indoors); counted log
  lines (`attached #n`, `drives ... #n`, `got off ... (#n)`) because `waitlog` matches the whole log.

Not proven or not done:

- **Persistence across a reload**: the game made no save during any run (the host's scratch folder changed only
  `profile.dat`) and the mod has no save entry point; the host's persisted entry (`vehicle_share 8`, the save writer's
  own output) holding the parked spot is the evidence.
- **Garage store** as the removal: the test uses the game's removal request (`vehicle_share 6`); the garage-store path
  itself is not driven by a scenario.
- The summoned truck overlapping the summoned trike (a scenario artifact) is held by repeated parked teleports in the
  guest's world, a visible 0.1..0.4 m jitter every ~2 s while they touch.
- A guest standing next to the truck she just parked blocks its teleport (the clearance keeps 0.6 m around the local
  Sam): her truck can stay up to ~0.2 m off the host's until she walks away (run `20261119-120115`, before the handback
  fix moved the host's truck instead).
- Native lamps / sounds (`vehicle_real_native`), the battery, the push claim and force-off paths, MULE / NPC vehicles,
  vehicles driven out of the other world's streaming range: unchanged from 23.21.

### 23.23 Review fixes (host-world safety, game thread, sync logic)

A review of the M2 working tree found ways a buggy or malicious peer, a race or a stale read could change the host's
persisted world. Every fix is on the host's side unless it says otherwise; the pure rules are in vehicle_wire and
unit-tested (`TestReviewRules`, `TestBinding`, `TestGate`, the message tests).

| Area | Before | Now |
|---|---|---|
| CLAIM | granted whenever the host's Sam was not on the vehicle | granted only with the partner body within 3 m of the vehicle's footprint (get on) or 6 m (push), at the host's unit place (or the guest's far place, below); else DENY `kDenyNotThere` (reason 3), and a vehicle the guest owned comes back to the host |
| Guest owns it parked, its Sam away | kept until silence (never, while PARK frames come) | no DRIVE frame for 2 s and the partner body more than 30 m from it for 5 s: the host takes it back (GRANT, timeout) |
| Guest frames (STD1) | any place | a frame more than 40 m from the host's unit and from the partner body is refused (one bad frame never teleports a host vehicle across the map) |
| Worlds | VOWN / REM / STD1 carried no world | VOWN and REM carry the sender's world (coop's load counter, a u32 at the end: VOWN body 92 bytes, REM 56); the receiver drops another world's; the host takes the guest's VOWN / REM / STD1 only while the guest's last SNAP_REQ named the host's current world (`vshare::PeerInOurWorld`), the guest the host's only while the host's newest snapshot / VOWN / REM named its world |
| Guest removals | applied when the guest owned the vehicle; no place needed; a unit missing from one poll counted as removed | applied only to a vehicle the guest owns that is destroyed on the host too (life 0 or the explosion latch: what the follower copied, D2-5), with the guest's last place of it within 5 m (a removal without a place is refused for a unit that has one) and the partner body within 60 m; anything else (a guest garage store, a removal request) is kept and announced again, and the guest's game shows "The host's game keeps the ...". The guest reports only a unit its poll sees not alive and that still reads not alive; a unit missing from a poll is carried for 5 polls, and the binding waits up to 10 rounds while a bound unit is missing |
| Emergency stop | followers kept the seats until the world reloaded | the next game-thread pass (`vdrive::AfterStop`, from vdrive's tick or dllmain's frame pump after a whole-mod stop) detaches every follower once, guarded: the live Detach, the host's Touch, the NPC bits |
| NPC bits | left set after a detach [I] | cleared after a live detach (only bits that were clear before our attach) and on an actor that spawns again after ours went, when nobody drives it: `+0x408 &= ~0x10`, `[+0x898]+0x48 &= ~2`, the writes of SetDriver's own get-off path (byte-checked, group kGNpc, optional). The runs below log the bits after the first detaches: both were already 0 (the game's Detach clears them), so the clearing is a guard; the truck keeps `+0x408 & 0x08` (0 before the attach), not touched |
| Actor streamed out under the follower | no Touch, nothing remembered | the unit is remembered; when it is seen again with its actor: the host's Touch and the NPC bits. The guest's place of a vehicle whose actor is not spawned on the host (from its frames and its RELEASE, within reach of the partner body) is kept: the host's snapshot and ADDs carry it (`vshare::SetPlaceOverride`), and the host puts the vehicle there once its actor is back and nobody else places it |
| Stale entities | ParkedTick wrote the state into the poll's entity (up to 100 ms old); vsync's real-vehicle look and lamps the same | the entity is checked again right before (`EntityLive`, `vshare::UnitLive`); the poll is cleared at every world change |
| Start-up | a frame-pump hook failure left the pool detours on | vshare and vdrive are stopped there too; the detours pass everything through whenever the co-op session is not active, and vshare stops itself when it stops |
| Fallback | vehicle_sync chose mode 5 once | vehicle_sync falls back to the copies (mode 1, no roadster) whenever the real sharing is not in effect (stopped, or the host's game sent no snapshot 20 s after the guest asked) and returns to 5 when it is; the guest's gate passes its own game's spawns until the host has answered with a snapshot in this world |
| Binding | a same-id vehicle without an actor here more than 5 m off was removed and mirrored again; the re-place and mirror path 3 teleported without the clearance; the mod's mirrors of placement vehicles (no GUID) were removed again by the next binding | a same-kind, same-GUID (or none) unit without an actor is bound (re-placed once its actor is back); both teleports only where the vehicle fits (the local Sam, other vehicles), else they wait; the mod remembers its mirrors (`LocalUnit::mirrored_for`) and binds them whatever their GUID |
| Host diff | a unit released and reused within one poll (an eviction for a spawn) kept `alive`: no REM, no ADD, the guest's request timed out | the unit's vehicle signature (GUID, spawn record +2), and a request answered by a unit that stood elsewhere before (> 20 m): the old vehicle's REM right before the new one's ADD (with the request's token) |
| Push / late GRANT | a GRANT while the guest's Sam was not on the vehicle had no handback | such a GRANT arms the at-rest RELEASE; a push counts only 2 s after our follower let go and 2.5 s after the partner's last DRIVE frame |
| Handback race | a handback crossing the guest getting on again put its Sam off | the guest claims again instead; the host ignores a RELEASE at rest while the guest's DRIVE frames still come (< 0.5 s) |
| Parked teleports | every 2 s forever where the worlds differ | after two onto the same place that did not hold, the gap doubles (4, 8, 16, 32 s, at most 60 s); 10 s in the ignore band or a new place start over |
| Requests | an unreadable transform went out as zeros | dropped on the guest; EncodeReq / DecodeReq refuse the origin and rows that are not a rotation; the host refuses a request without a place |

Not done: the push check does not require the host's unit to move (the partner body's place is the evidence); STD1 frames
carry no world (the host takes them only while the guest is in its world, by the SNAP_REQ); the host's persisted place of a
vehicle left out of its streaming range changes only when its actor is back (the snapshot carries the guest's place
meanwhile); a host in mode 1 with a guest in mode 5 is detected on the guest only.

Hidden runs of this state (2026-11-19, foreground, one at a time; all `-SeedSaves complete-100`, the host-save ones
`-GuestSeed profile-only`): every one result pass, both games exit 0, real saves unchanged and restored, no violation,
0 cloud uploads, no `[vshare]` / `[vdrive]` FAULT.

| Run | Result |
|---|---|
| `20261119-130850-two-games-veh-identity` | 22 bound, the guest's trike summon mirrored (exact id 0x01E001), 23 = 23 |
| `20261119-131047-two-games-veh-summon` | trike (guest REQ, answered in 0.2 s) and truck each one unit per world (22 -> 23 -> 24); the truck summoned into the trike: after two teleports that did not hold the backoff logged and the gap grew |
| `20261119-131238-two-games-veh-drive` | host drives, guest follows: moving windows mean 0.24 m (max 1.03 m) |
| `20261119-131508-two-games-veh-drive-guest` | the guest's CLAIM granted (its Sam at the truck), handback at rest |
| `20261119-131752-two-games-veh-drive-trike` | the lite attach; bits after the detach 0 |
| `20261119-132015-two-games-veh-drive-roadster` | the full attach; bits after the detach 0 |
| `20261119-132245-two-games-veh-handoff` | host drives / gets off, the guest claims (granted) and drives, handback, the lock, the race (the host boards) |
| `20261119-132955-two-games-veh-remove` | the host's trike REM; new step: the guest removes truck 0x020005 it does not own -> the host keeps it and announces it again, the guest shows the notice, mirrors it by the exact id and later follows it; the truck's life 0 copied |
| `20261119-133203-two-games-veh-persist` | the guest drives the truck away and parks it; handback; both worlds hold it at (-1317.3, 299.3) within 0.04 m |
| `20261119-133403-two-games-hostsave-continue` | the M1 session unchanged; 22 bound |
| `20261119-133610-two-games-vehicle-state` | host_save=0: vshare off, the v0.14 copies (mode 1) as before |

(`20261119-132806-two-games-veh-remove` failed at the new step only: no truck within its first 30 m radius; the step now
takes the nearest within 150 m.)

### 23.24 Solid for Sam on foot (v0.19)

The play test of v0.18.0 ("the non-driver can't collide with a vehicle the other player is driving") ran without
`host_save` (the launcher's Local test wrote none, docs/coop-session.md), so its partner vehicles were the v0.14
copies, which are collision-free by design (22.11). The real shared vehicles were never changed: nothing in the mod
turns a real vehicle's collision off (`CollisionOff` only touches copies, packs, cargo and gear props), and the
follower's attach (`SetDriver 0x142F7C4C0`, `SetController 0x142F80310`) writes no collision, filter or physics
instance. Static research: the truck's chassis (layer 17) does not meet Player (8) at all (`Disable(17,8)` at
`0x1422ACAF4`); Sam is stopped by the vehicle's keyframed "Player blocker" part (layer 11, `Enable(11,8)` at
`0x1422AC94A`; layer 15 copies row 11 at `0x1422AC96C`), the same on every unit. No new game address is used.

Proof, hidden scenario `two-games-veh-block` (partner_collision=0, so only the vehicle can stop her; on the start
garage's floor; ten 300 ms steps of full stick at the vehicle's middle, `log_positions` after each). Run
`20261120-021803` (her distance from the vehicle's centre line, the truck frame from `vehicle_drive 9`):

| Case | Stops at | Then (2.5 s more full stick) |
| --- | --- | --- |
| B0 control: the garage roadster nobody drives | 1.09 m | stays at 1.09 m |
| B1 the truck with the host on it (her unit is the follower) | 1.66 m | slides along its side at 1.57-1.62 m (the walk is 6 deg off square) |
| B3 the same truck parked after the host got off | 1.66 m | slides along at 1.59-1.62 m |
| B2 the host creeps (rt 70, 3 s, 5.8 m) into her from 3.4 m ahead of the bumper | pushed 2.7 m ahead of it | never inside (1.97 m ahead of the origin both runs; the cab is ~0.8 m behind the front) |

The repeat `20261120-021010` stopped her at 1.67 / 1.69 m; there the creep then carried her up onto the truck's front
(z +2.5 m), the game's own contact. So no change: the follower is never keyframed by the mod, `partner_collision` rules
are as before (25.6), and the v0.14 copies stay collision-free for the `host_save=0` mode, which now says on screen
that it is the old mode.

## 24. Riding together (`session.ini ride_together`, default 1, with `vehicle_sync=5`; design milestone M3)

"Players can ride together in vehicles", "make sure you look into letting players ride in the passenger seat too". The
game seats one player per vehicle: the driver, whose game owns and drives it (section 23). The other player rides along
as a **carried passenger**: their own Sam, his capsule off, placed every tick at the vehicle's transform times a seat,
with the driver's seated pose. `native/mod/ride_together.*` does it in the game; the pure part (`native/mod/ride_wire.*`:
the message, the seats, the rules, the detectors, the exit spot) is unit-tested in `st_ride_wire_tests` (313 checks).
Ownership (VOWN, 23.13) is unchanged: the passenger never owns; the driver getting off ends the ride.

### 24.1 On each screen

| | The driver's screen | The passenger's screen |
|---|---|---|
| the vehicle | the driver's own (native ride) | our follower of it (23.11: the game's NPC controller replays the driver's inputs) |
| the driver | native | the partner body at our vehicle times the driver's STM1 seat (23.15) |
| the passenger | the partner body at **our vehicle's transform (one frame ahead along its velocity) times the passenger seat**, its pose the partner's STJ1 (seated, below); its capsule out (`kPoseInVehicle`) | **our own Sam, carried**: capsule off, the entity teleport `0x14235A010(Sam, &wt, 0, 0)` every tick to the vehicle's transform (one frame ahead along its velocity) times the seat, the movement / action input blocked, the driver's seated pose |

- **Seated pose (decision D3-2, v1)**: the passenger's own Sam ends his model update with the driver's pose
  (`realbody::SetRidePose(1)`; `RideOverride` in Sam's capture detour, before the capture is published): every joint's
  rotation and the root / pelvis translations of the partner's published STJ1 frame (the driver's local Sam, Sam's
  layout, at the body's play time, under 500 ms old), Sam's own bone lengths, then the game's model-matrix and bounds
  steps (as the self-view fit, section 16). The capture then carries the seated pose, so the wire (the partner body on
  the driver's screen sits too), the self view and the gear on Sam follow it. As with the fit, a value the game left as
  we wrote it reads as Sam's clean one, and his clean values go back when the override ends. Source 2 is a snapshot of
  our own driving pose (`seat_measure`, the solo seat test). The game's `BotRiding` animation variable (a native
  passenger sit) stays the v2 candidate: not tried (the copied pose looked right in every screenshot, 24.10). It needs
  the real partner body (`partner_body=real`, the default): with the hologram there is no STJ1 frame, and the carried
  Sam keeps his own pose at the seat.
- **The input while carried** (`overlay::input::SetRideCarry`, only while our Sam is awake: asleep in the game's pause
  menu or a notice the whole block is lifted, so the menu gets the pad, the mouse and the keys; back once he is awake):
  the pad answers keep the right stick (look) and a tap of Start (its first 250 ms: the pause menu opens, a hold never
  reaches the game); every other button, the left stick and the triggers read idle to the game (Start held beside a
  vehicle is the game's "Dispose of Vehicle", Back its "Like": both prompts are on screen while carried, so neither may
  reach it). Key presses of the
  movement / action keys (letters, digits, Space, Shift, Ctrl, arrows) are swallowed by our keyboard hook and dropped
  from raw input and the window messages; their releases pass (nothing stays down). Mouse button presses are dropped (a
  raw packet with a press loses its motion). Escape, Tab, Alt and the function keys pass. The pad buttons and the ride
  key are recorded before the masking, so the mod sees the hold that gets off.
- **STP1**: the carried passenger's pose frames say `kPosePassenger | kPoseInVehicle` (`wire::kPosePassenger`, bit 2).
- **Teleport clearance**: our follower's teleport corrections (23.12) leave our carried Sam out of the clearance of the
  vehicle he rides (`vdrive::SetCarriedUnit`).

### 24.2 Seats (measured: `seat_measure`, run `20261119-143916-seat-measure`)

`seat_measure` (while our Sam drives) logs his root and pelvis in the vehicle's frame (rows right, forward, up), the
vehicle's joint 1 (its model matrix from the vehicle's SkinnedModel pose, `realbody::ReadJointModelAny`) and the game's
seat helpers through it. The helpers are static data of each vehicle's SkeletonHelpers resource (positions in joint 1's
frame, from the passenger research's dumps); a helper's place in the vehicle's frame is joint 1's position plus the
helper's offset along joint 1's axes.

| Kind | Joint 1 | Driver root (as the game seats Sam) | HLP_Driver / HLP_Passenger in the vehicle's frame | Passenger root used |
|---|---|---|---|---|
| truck | `JNT_C_B_001_Hips` of 47 at (0, 0, 0.157), axes = the vehicle's | (-0.439, 0.801, 1.950), yaw 0.0 deg; pelvis (-0.439, 0.802, 2.132) | (-0.473, 0.720, 1.950) / (0.473, 0.720, 1.950) | **(0.44, 0.80, 1.95)** |
| roadster | `JNT_C_B_001_Hips` of 23 at (0, 0, 0.100), axes = the vehicle's | (-0.302, -0.623, 0.660), yaw 0.0; pelvis (-0.312, -0.620, 0.759) | (-0.313, -0.660, 0.660) / (0.313, -0.660, 0.660) | **(0.31, -0.61, 0.66)** |
| trike (the Kusanagi) | `JNT_C_B_001_Hips` of 40 at (0.074, 0, 0.608), tilted 7.5 deg (parked on its stand) | (0.177, -0.551, 0.018), yaw 0.0; pelvis (0.217, -0.551, 1.031) | HLP_C_Driver (0.111, -0.528, 0.892); no passenger helper | **pillion (0, -0.85, 0.12)** (D3-1) |

- On the roadster and the truck HLP_Passenger is exactly HLP_Driver mirrored across the vehicle's centre plane. The
  passenger root is the measured driver root mirrored (the helper delta would give (0.506, 0.801) / (0.325, -0.623): the
  driver's root sits 0.034 / 0.011 m inboard of HLP_Driver, so the passenger's mirrors that). The STM1 seats of the M2
  runs agree (truck (-0.44, 0.80, 1.95) in 368 frames, roadster (-0.31, -0.61, 0.66)).
- The design's proof offset (0.47, 0.72, 1.79) was HLP_Passenger in joint 1's frame without joint 1's height (0.157 m) and
  without the root's inboard / forward shift: Sam crouched on the outside front.
- The trike's pillion: 0.30 m behind the driver's root, 0.10 m higher (the rear of the seat over the rear wheel); the
  first try (0, -1.00, 0.12) put Sam behind the seat (run `20261119-141112-seat-measure`, M23 enlarged).
- The seat rotation is the vehicle's (the driver's root yaw in the vehicle's frame measured 0.0 deg on all three).
- Screenshots (enlarged): the truck M04 (Sam in the right-hand seat through the windscreen, hands forward), the roadster
  M13 / M14 (in the right-hand seat), the trike M23 (on the pillion behind the seat).

### 24.3 Boarding (M3.2, decision D3-4; the game's own "Use Vehicle" since play test 2)

**The game's own prompt (default).** Play test 2 (v0.19.0, the launcher's Local test): our ImGui "Hold [X] / [F] to ride"
banner showed but never boarded, and the user asked for the game's own **"Use Vehicle"**, which the game showed crossed
out on the partner's vehicle. Now:

- Beside a vehicle the partner drives here (our follower attached), when the game itself offers "Use Vehicle" for it (its
  own finder: Sam close to and facing the vehicle, any side), and boarding is allowed (the vehicle under 1 m/s, our Sam
  on foot and awake and not hurt in the last 5 s, within 7 m of the vehicle's origin, no request pending or ride ended in
  the last 1.5 s, no save pending, the partner's frames under 1 s old and in our world), **the game's prompt is shown
  available** instead of crossed out. Otherwise it stays as the game has it (crossed out: the game's own denial sound on a
  press, nothing else).
- **One press** of the game's button for it (keyboard F, X / Square on any pad the game reads: XInput, a PlayStation pad
  through scePad, Steam Input, HID) sends **REQUEST** with reason `kReqNative` (24.6;
  `[ride] guest: asking to ride the truck 0x020005 (the game's Use Vehicle, ...)`). The game never starts its own get-on on
  that vehicle (our follower holds the seat): the press is the game's own, read where the game turns it into a start.
- No ImGui prompt; the banners "Asking to ride...", "Cannot ride: ..." and "No answer from the driver." stay. While the
  driver answers, the prompt stays available (presses then are ignored).

How (two MinHook detours, both reached only through the ride action's vtable; `kNativeSites` in ride_together.cpp):

| Piece | What |
|---|---|
| the prompt | `EDSUIActionLocalizedId` 5 RideOn ("Use Vehicle"; 6 RideOff), requested by Sam's ride action (vtable `0x143D82CF8`) from its icon provider (vt+0xF0 `0x14279D870`, `(action, slot, request*)`) for action slots 3 and 6 (the two control schemes) |
| the request | +0x00 icon id, +0x06 enabled, **+0x09 disabled (crossed out)**, +0x24 the provider's reason, +0x50 the distance key (nearest wins; 1e6 when disabled with reason 0), **+0x98 the target vehicle's game-actor id** (vehicle +0x314) |
| provider post-hook | slot 3 / 6, RideOn, the id our follower's vehicle: while boarding is allowed, +0x09 = 0, +0x06 = 1, +0x24 = 0, a distance key of our own when the game wrote 1e6, and the action's "disabled" latch (+0x13A bit 0x20) as it was before the call. Every change of what the game made of it is logged: `[ride] native prompt (slot 3): Use Vehicle on the partner's vehicle 0x020005; the game crossed it out (reason 0): lifted (available)` |
| the press | the game's slot query (`0x1427A4970`: owner, generation, not disabled, pressed) lets the ride action's TryStart (vt+0x08 `0x1427D5A80`) write the id to action +0x1E0 and return 1 |
| TryStart post-hook | a start on our follower's vehicle (or, carried, the one we ride) is undone (+0x1E0 and +0x10A / +0x10B as before), returns 0, and counts as our press: one per press (TryStart repeats while the button is held; a new press after 400 ms without one: `NativePressIsNew`). A start on any other vehicle is the game's own |

The reason the game crosses the partner's vehicle out was measured in the first run: **reason 0** (the provider's global
check `[0x147BF9A58]+0x10`), on the truck, the roadster and the trike, both games.

**The fallback (the native checks fail, or `ride_test 12 0`):** the ImGui banner **"Hold [X] / [F] to ride with
<partner>"** within 2.5 m of the passenger door point (truck (1.8, 0.8, 0), roadster (1.4, -0.6, 0), trike (0.9, -0.9, 0)
in its frame), a 0.6 s hold of X or F (REQUEST `kReqHold`). A button held from before never counts until it is released.
On a mismatch the log says `[ride] native Use Vehicle prompt OFF: <reason>: the ImGui hold prompt boards instead` and the
co-op banner says so once.

**Our own button reads** (getting off, the fallback's hold). Play test 2's root cause: they read XInput pad index 0 and
the F key at `HC_ACTION` only. The user's DualSense Edge is read by the game without XInput (its PlayStation path), and
ds.exe's own keyboard hook, when it sits above ours, keeps every `HC_ACTION` key (it never chains them), so neither the
hold nor F ever counted. Now: every XInput index 0..3 (before our carry mask), **the game's own pad state** (input manager
`0x147BE6330`, 7 player slots of 0xF0: +0xE4 = 1 read, +0x108 player 0, pad state at +0x40; X / Square at +0x13 and
B / Circle at +0x11 in both the XInput and the PlayStation converters; `kPadSites`, off alone on a mismatch), and the
ride key as our hook sees it at either code (`HC_NOREMOVE` too, overlay_input). The launcher's Local test host game is
keyboard + mouse only (its pads are the guest's): no pad source there. The fallback's banner logs once what the reads see
(`[ride] prompt: ... XInput pads read n (ages ...), game pad X ..., key F ...`), and every board / exit names its source
(`held (XInput pad #0)`, `(the game's pad state (device 1))`, `(key F)`).

- The driver's game **ACCEPTs** unless (in this order): riding is off there (`ride_together=0`, a failed check), another
  world, its Sam does not hold that vehicle's seat, another ride holds the seat, the vehicle moves at 2 m/s or more, or it
  does not see the partner body near it (within 5 m of the door point; for `kReqNative` also within 8.5 m of the vehicle's
  origin, since the game's prompt works from any side); else **DENY** with the reason (banner "Cannot ride: ...").
  A repeated REQUEST of an accepted ride gets ACCEPT again. No answer in 3 s: "No answer from the driver."
- On ACCEPT (the passenger still within 4 m of the door, or for the native prompt 8.5 m of the origin): the capsule off (`0x14243A740`; a counter at capsule +0x58:
  exactly one enable later, logged before / after), the input block, the pose override, the per-tick carry.
- No boarding for 5 s after our Sam's health drops (`[ride] our Sam was hurt (health a -> b): no boarding for 5 s`): a
  partner's vehicle that just knocked him down stops beside him, and the banner offered the ride while he lay on the
  ground (H1b of run `20261119-164745`; gone in `20261119-171723`). A health of 0 (a world loading) is not read as a drop.

### 24.4 Exits

**By choice (M3.3)**: with the game's own prompt, **one press** of X / B (any pad, read as in 24.3) or F, or the game's
own RideOff prompt where it shows one (24.3: carried, the game's RideOn on the vehicle we ride is turned into an enabled
RideOff, and its TryStart is our press; the game asks its provider while Sam is carried on the roadster and the trike
(logged; the prompt itself not yet seen in a screenshot), not in the truck's cab; the boarding banner says "Riding with <partner>. Press [X] / [F] to get off."); a press while the boarding press is still
held never counts. The fallback: a 0.6 s hold. Under 2 m/s the passenger is put beside the passenger door
(`ExitPlacement`: the door row, half the vehicle's width + 0.7 m out, facing it) with the game's PlaceOn (mode 1: its
ground probe from 0.5 m above the vehicle's base; through `game::TeleportLocalPlayerFacing`, every guard of the debug
teleport). Then the capsule on, the input and the pose back, END to the driver. A press (hold) above 2 m/s ("Too fast to get off:
slow down, or press again to jump off.") gets off as soon as the vehicle is under 2 m/s within 3 s (run
`20261119-142942`: a trike rolling at 5.6 m/s); **a second press (hold) within those 3 s jumps off at any speed** (END
`jumped`, a forced placement: away from the sideways velocity, moved back along the path; above 8 m/s also the D3-3
hit), so a driver who keeps going never holds the passenger (run `20261119-181006`: off the reversing truck at 4.6 m/s).
Holds count only while Sam is awake (a menu's buttons are the menu's).

**Every placement** first asks `game::PlacementGate` (the guards of `Place`, without the rate limit), so the mod never
warps Sam where the game's own warp would refuse: the game has Sam (a load, a stopping game event, the post-load setup, a
parent, Sam+0x678, a ground snap or pushed transform, a sequence): no placement (the game places him; the capsule and the
input come back where he is); Sam asleep (a menu), the game capturing or writing a save (`save_guard::InProgress`, 24.12)
or the world waiting for a scene (the load mask's bit `0x100` alone; not a save, 24.12): the placement waits up to 2 minutes and goes through once he is awake (dropped when the game takes him meanwhile or he was
moved over 4 m from where he was left); only PlaceOn's 500 ms rate limit (or the teleport's byte checks off, Sam awake)
lets the entity teleport stand in, with PlaceOn again for 2 s, never after that call faulted in this ride. When Sam was
left behind (asleep while the vehicle drove on), the spot is beside where the vehicle was at our last teleport of him.
The spot is always on the ground plane: the vehicle's right and forward axes projected onto it, the edge of its body
(base up to twice its half height) on that side, the height of its lowest point; a vehicle on its side or roof puts Sam
beside it, never along its tilted axes under or onto the wreck (unit-tested at 90 deg each way, on the roof, on the nose).

**Forced (M3.4; the passenger's game decides for its own Sam)**, in this order:

| Trigger | End reason | Placement |
|---|---|---|
| the mod's emergency stop (a fault of the per-frame tick) | stopped | the input and the pose back at once (any thread); on the next game-thread pass: put beside, capsule on (`AfterStop`, once); a pending REQUEST is withdrawn (END `stopped`), and every later ride message is refused (a late ACCEPT gets END at once, never a carry; `ride_test 10`) |
| a world change (load, reload, title: coop's load counter) or our Sam another entity | a load or the title | none: the capsule back on only on the same Sam (a new Sam has a new capsule) |
| the game takes our Sam: a parent entity, a load bit, a DSGameState bit of a cutscene / game over / faint / fast travel / private room / knot space / boss battle / area change ... (`kTakenBits`, current or requested), Sam+0x678 (the game's warp guard), a pushed transform, a sequence binding (every 250 ms) (`game::CarriedSamTaken`, every tick) | the game took Sam | none (the game places him) |
| ... or nothing of the game's places him: a health drop over 1 (a MULE spear, a BT, a fall), the world waiting for a scene (load mask `0x100` alone), asleep 60 s | the game took Sam | beside (waiting while he sleeps) |
| a save: pending and held for us by the save guard (`save_guard::WantsOff`), or pending / being written without the hold (`save_guard::Busy`: the guard off, its cap, the post-load save) | the game saves | beside with PlaceOn before the held save is released (24.12); without the hold the placement waits for the write |
| nothing from the partner for 1 s | link lost | beside |
| the driver's END (its Sam got off: `driver_off`; a new session) | the driver got off / ended it | beside |
| the vehicle not live here for 300 ms | the vehicle is gone | beside (its last place) |
| broken / out of battery (ride state 5 / 6, `+0x560 & 0x2000`, the frames' broken / exploded flags) | broken | beside |
| a velocity jump over 8 m/s between two of the driver's DRIVE frames at most 100 ms apart (sender-measured) | crash | beside, away from the sideways velocity, moved back up to 3 m along a fast vehicle's path; **then the M4 hit handler** (D3-3, 24.7) |
| up axis z under 0.3 for 0.5 s (our follower's transform) | flipped | beside, on the ground plane (never along the tilted axes) |
| the entity teleport faulted | the mod stopped | beside with PlaceOn only |
| a second hold within the 3 s wait above 2 m/s | jumped off | beside, as a crash; above 8 m/s the M4 hit handler |
| our follower let go of that vehicle for 400 ms (the driver's frames ended) | the driver got off | beside |

Asleep (a menu): Sam is held, not teleported, the ride goes on; the input block is lifted meanwhile. A BT / tar grab has
no trigger of its own (the game's "Hugged" state is not researched): it ends the ride through its health drop or game state. The passenger's END tells the driver's game, which stops
placing the body. The driver's game also ends a ride itself when its Sam gets off (END `driver_off`) and when the
partner's pose has not said `kPosePassenger` for 5 s after the accept (`timeout`).

### 24.5 Camera

The passenger keeps the game's on-foot camera on his carried Sam (no camera code). In the runs it reads as a chase view a
little above the vehicle: behind and above the truck's cab (the passenger hidden under the roof: R02 / R03), beside the
roadster's passenger seat (P02, the driver and the passenger side by side), behind the two riders on the trike (K02 /
K03). The right stick turns it as usual. Two things were seen and are not fixed (documented):
- after the host had been placed at the door with a steep downward camera, its carried view looked almost straight down
  onto the cab roof (S02 / S03 of run `20261119-141914`): the right stick brings it up;
- right after a forced exit beside the truck the camera sat at Sam's head for 2-3 s (F01 / F02 / F04 of run
  `20261119-143414`: Sam out of view, the "Use Vehicle" prompt in the middle of the screen), then came back by itself (F05).
  Seen again in run `20261119-164402` (F01 "Off the truck: link lost.", F02 "Thrown off in the crash.") and with the host
  as the passenger in `20261119-172958` / `-173506`: after a placed forced exit (link lost, crash, the driver got off) the
  camera sits in Sam's head and the game fades his model out (E4 of `-173506`: Sam half transparent in the foreground,
  "Use Vehicle" in the middle), for as long as he stands still (still so 4.7 s after the crash exit, E2b); one step and it
  is back (E5). The exit by choice (R05 / S05 / P05 / K05) did not show it;
- the host carried in the truck again looked almost straight down from above the truck (S02 / S03a / S03c of run
  `20261119-163121`: the truck and the road in view, a top-down follow view); the guest's carried view in the same run was
  the chase view over the cab.

### 24.6 Wire: STQ1 inside SVM1 type 48 (channel 3, `vshare::RegisterHandler` / `SendEvent`)

```
"SVM1" | u8 type 48 | u8 version 1 | u16 0                                     (the vehicle event header, 23.6)
"STQ1" | u8 type (1 REQUEST, 2 ACCEPT, 3 DENY, 4 END) | u8 seat (1 passenger, 2 pillion) | u8 reason
| u8 from (0 host, 1 guest) | u32 ride id | u32 sender_ms | u32 vehicle id (the host's) | u8 kind | 3 x 0 | u32 world
                                                                               (28 + 8 = 36 bytes, little endian)
```

Strict decoder: the header, the exact size, the magic, a known type, the seat the kind takes (roadster / truck:
passenger, trike: pillion), the reason in the type's range (REQUEST 0 hold / 1 test, ACCEPT 0, DENY 0..5, END 0..14), from
0 / 1 and the partner's (our own role is dropped), ride id != 0, a kind whose actor type is the id's, zero reserved bytes.
A REQUEST or ACCEPT of another world never starts a ride (a REQUEST is DENYed `world`, an ACCEPT answered END `world`); a
DENY or END carries the sender's world, which differs exactly when it answers across a world change, so it passes (it acts
only on its own ride id; the `world` DENY used to be dropped there and never arrived). The ride id is the passenger's; every message of a
ride carries it.

### 24.7 For M4: the passenger's crash hit (D3-3)

`ride::SetCrashHitHandler(CrashHitFn)` with `bool fn(std::uintptr_t vehicle_entity, double dv_mps, double speed_mps)
noexcept`: called on the game thread right after a crash exit put our Sam beside the vehicle and turned his capsule back
on (the entity of this world's unit he rode, the velocity jump, the speed before it); it returns true when the game's
hit was applied. M4 installs it (partner_tackle.cpp, section 25.5): the game's own hit on the passenger 150 ms later.
Without it (an older build, or the hits off) the exit logs `crash exit (dv ... at ...): no hit handler ...` or `the hit
handler refused`. Also for M4: `ride::Carried()` (our Sam is a passenger: no PartnerTackle on him) and
`ride::RideVehicle()` (the vehicle the partner rides in our world, or we ride in theirs: never a hit on its own passenger).

### 24.8 Byte checks (`kSites` in ride_together.cpp: 6 sites + 1 structural)

A mismatch: riding off, a one-line reason in the log and the co-op banner ("Riding together off: ..."); the vehicles stay
shared and driven.

| Site | Bytes |
|---|---|
| Entity teleport `0x14235A010` (PlaceOn mode 0's call) | `48 8B C4 48 89 58 18 48 89 70 20 55 57 41 56 48 8D A8 48 FF FF FF 48 81` |
| capsule disable `0x14243A740` (`++[c+0x58]`, the switch off) | `40 53 48 83 EC 20 48 8B D9 48 83 C1 58 E8 EE 2D FD FF 33 D2 48 8B CB 48 83 C4 20 5B E9` |
| capsule enable `0x14243A9E0` (`--[c+0x58]`; on again at 0 when the entity's collision flag `+0x88` bit 14 is set) | `48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 83 C1 58 E8 8A CC FD FF 48 8B 43 48 48 8B 90 88 00 00 00 48 C1 EA 0E F6 C2 01 74 0F 83 7B 58 00` |
| the counter increment `0x14240D540` / decrement `0x142417680` | `FF 01 C3` / `FF 09 C3` |
| the capsule switch `0x14243AA30` (`+0x60` physics object, `+0x5C` on) | `40 57 48 83 EC 60 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 58 44 0F B6 C2 48 8B F9 48 8B 51 60 48 85 D2 0F 84 ?? ?? ?? ?? 0F B6 41 5C` |
| structural: the player capsule vtable `0x143D1BD08` in the image, its slot 0 in .text; at run time Sam `+0x5A0` - 0x20 must carry it and its `+0x48` must be Sam | |

**The native prompt** (`kNativeSites`, 8 sites + 1 structural; a mismatch or a failed hook: the native prompt off, one
log line and one co-op banner, the ImGui hold prompt boards; riding stays on):

| Site | Bytes |
|---|---|
| the ride action's icon provider `0x14279D870` | `40 53 56 57 41 56 41 57 B8 B0 C9 04 00 E8` |
| its cross-out `0x14279E693` (request +0x09 = 1, +0x00 = 5) | `C6 46 09 01 C7 06 05 00 00 00 40 38 7E 09 74 06` |
| its target id and reason `0x14279E6EC` (+0x88, +0x98 = vehicle +0x314, +0x20, +0x24) | `89 86 88 00 00 00 89 9E 98 00 00 00 C7 46 20 74 55 96 03 44 89 66 24` |
| its disabled latch `0x14279E9A5` (action +0x13A bit 0x20) | `41 80 8E 3A 01 00 00 20` |
| TryStart `0x1427D5A80` | `40 53 48 81 EC 90 00 00 00 48 8B 05 A0 B5 78 02 48 33 C4 48 89 84 24 80 00 00 00` |
| TryStart's target write `0x1427D5B97` (+0x1E0, +0x10A, +0x10B) | `89 83 E0 01 00 00 0F B6 44 24 24 88 83 0A 01 00 00 B0 01 C6 83 0B 01 00 00 00` |
| the ride action's slot query `0x1427A4950` (vt+0xA8) | `4D 8B C8 44 8B C2 48 8B D1 48 8B 49 60 E9 0E 00 00 00` |
| the action slot query `0x1427A4970` | `4D 63 D0 4D 69 C2 E0 03 00 00 49 39 94 08 38 53 00 00` |
| structural: the ride vtable's slots `[0x143D82DE8]` = the provider, `[0x143D82D00]` = TryStart, `[0x143D82DA0]` = the slot query | |

**The game's pad state** (`kPadSites`; a mismatch: that source off alone, XInput and F still read): the pad reader
`0x1419B3090` `40 53 56 57 41 56 48 83 EC 48`; XInput X `0x1419B21CC` `B8 00 40 00 00` and `0x1419B21EC` `41 88 49 13`;
XInput B `0x1419B21C0` `41 88 41 11`; PlayStation Square `0x1419B8850` `41 88 4E 13`; PlayStation Circle `0x1419B882E`
`41 88 46 11`.

Reused, checked by their own modules: PlaceOn `0x142634F00` (game_actions), RequestTeleport `0x142F7CA20` (vehicle_share;
the hidden-test roll / stop), the vehicle vtables and `+0x8A8` / `+0x4CC` / `+0x560` (vehicle_drive), the SkinnedModel pose
(real_body), the XInput / keyboard hooks (overlay).

### 24.9 Hidden scenarios (`-SeedSaves complete-100`, the two-game ones `-GuestSeed profile-only`, `ini host_save=1`)

| Scenario | What |
|---|---|
| `seat-measure` (echo, one game) | per kind: drive, `seat_measure`, get off, `ride_test 1` (the solo carry with the kept driver pose), screenshots from three sides, off (the trike: `ride_test 9`, the emergency-stop path) |
| `two-games-ride-truck` | the host drives, the guest holds X at the door and rides (forward, stop, back, stop, forward, stop), gets off with X while the host brakes; the swap: the guest drives, the host rides and gets off |
| `two-games-ride-roadster` | the same in the garage's roadster |
| `two-games-ride-trike` | the same on the Kusanagi trike's pillion (the lite-attached follower) |
| `two-games-ride-forced` | the guest rides and is put off by: a 2.5 s link blackout, a crash (the driver's game stops its moving truck dead: `ride_test 6`), the game taking Sam (`ride_test 7`), the driver getting off, the truck rolled over (`ride_test 5`) |
| `two-games-ride-native` (`launcherini local-test`, no `-GuestSeed`) | the launcher's own Local test files; real input only: the guest at the host's truck sees the game's "Use Vehicle" available (N1), one press of X boards (REQUEST "the game's Use Vehicle"), both screens (N2 / N2b), a drive, X gets off (N4); the swap: the host sees it (M1), X boards, F (`hookkey`: our keyboard hook, then ds.exe's own keyboard procedure) gets off; the ImGui fallback (`ride_test 12 0`): the host's 0.9 s hold of F boards and gets off (F0 / F1) |

The other ride scenarios board with `pad X 800` at the door: since play test 2 that is the game's own press (the native
prompt), so they all go through it (`asking to ride ... (the game's Use Vehicle`).

New debug actions (docs/debug-actions.md "Riding together"): `ride_state`, `ride_board`, `ride_exit`, `seat_measure`,
`ride_test`.

### 24.10 Evidence (hidden, foreground, one at a time, 2026-11-19)

Every run: both games exit 0, real saves unchanged and restored, no violation, 0 cloud uploads, no `[ride]` / `[vdrive]`
FAULT.

| Run | Result |
|---|---|
| `20261119-141112-seat-measure` | pass: the first seat check (the pillion then at y -1.00: behind the seat) |
| `20261119-143916-seat-measure` | pass: the measurements of 24.2; the solo carry 0.000 m from the seat over 754..900 teleports per kind; the emergency stop while carried on the trike: input and pose back, put beside it, capsule counter 1 -> 0 (M25) |
| `20261119-141615-two-games-ride-truck` | fail (the scenario): the route's turn ran the host's truck off the raised road into the gap; the guest's game put its Sam off by itself, **a real crash: dv 8.0 m/s at 10.0 m/s**, placed on the left (away from the slide), capsule back on; the exit step was never reached. Route changed to straight moves |
| `20261119-141914-two-games-ride-truck` | pass: the guest rides 33 s (moving 2.5..11 m/s; Sam vs the seat mean 0.003..0.007 m, max 0.020 m per second), gets off with X; the swap: the host rides 24 s, gets off |
| `20261119-145308-two-games-ride-truck` | pass on the final build; two RequestTeleport corrections of the follower (2.1 m: the worlds diverged when the host reversed into something) moved the truck under the carried guest for one frame (max 2.09 m once, on the seat again the next tick) |
| `20261119-142403-two-games-ride-roadster` | pass: both directions; the host's screen shows the guest's body in the right-hand seat (P02b / P04b), the guest's both riders side by side (P02, Q03d) |
| `20261119-145709-two-games-ride-roadster` | pass on the final build: Sam vs the seat over the ride mean 0.004 m, max 0.12 / 0.17 m, 0 ticks over 0.25 m (the host and the guest as the passenger) |
| `20261119-142702-two-games-ride-trike` | fail: the guest's exit hold came at 2.7 m/s (the trike rolled on) and was refused; fixed: a hold above 2 m/s gets off once under 2 m/s (3 s), and the scenario's driver brakes |
| `20261119-142942-two-games-ride-trike` | pass: both directions on the pillion (K02 / L02: two Sams in tandem, both seated); an exit waited for the trike to slow down (5.6 m/s: on the trike LT reverses) |
| `20261119-143414-two-games-ride-forced` | link lost (the exit 1.0 s after the blackout began), crash (dv 10.2 m/s; the M4 handler asked: none yet), taken (no placement), the driver got off: all pass; the flip step failed: the test then teleported the truck upside down 2 m up, a crash in the frames (dv 59.7) |
| `20261119-144141` / `-144457-two-games-ride-forced` | the roll test (18 x 10 deg, then 45 x 4 deg by RequestTeleport): the rolled truck tumbled in the owner's world, a crash (dv 8.5 / 17.4 m/s) before the follower was upside down for 0.5 s |
| `20261119-144802-two-games-ride-forced` | pass: link lost, crash (dv 10.3), taken, the driver got off, the roll (crash, dv 36.3); every exit: capsule counter 1 -> 0, the input back |
| `20261119-161939-seat-measure` | pass (independent re-check): the solo carry 0.000 m from the seat on all three kinds, the emergency stop on the trike (capsule 1 -> 0, put beside it); M04 (the passenger seen through the windscreen on the right), M13 / M14 (roadster), M22 / M23 (pillion) |
| `20261119-162230-two-games-ride-truck` | fail (the scenario): the "brake" before the passenger's exit held LT for 2.5 s from a standstill, which reverses: the truck backed away at 5 m/s and the guest's hold was refused (3 s window). The earlier passes got off at 2.6 m/s by luck. The exit steps of the three ride scenarios now let go of the pedals (the vehicle is already stopped) |
| `20261119-162547-two-games-ride-truck` | pass, but the swap's route (forward first) ran the truck off the end of the raised road's straight with the host carried (S03-guest-moving: the truck nose down over the edge; S04: under the road): no crash exit (dv under 8 m/s), the truck lost 10 life (the owner's value restored it), the host got off under the road, standing. The swap route now backs up first, then goes forward |
| `20261119-163121-two-games-ride-truck` | **pass** (final scenario): the guest rides 34.2 s (seat error mean 0.006 m, max 0.554 m, 4 of 1026 ticks over 0.25 m: follower corrections), the host rides 25.1 s (mean 0.003 m, max 0.425 m); on the road all the way; R02b / R04b: the guest seated at the passenger window on the host's screen; R02 / R03b: the guest's own Sam in the cab under the roof; R05 / S05: standing at the door |
| `20261119-163428-two-games-ride-roadster` | **pass**: guest 29.5 s (max 0.309 m), host 22.4 s (max 0.076 m); P02 / P04b / Q02 / Q04 / Q05: two riders side by side on both screens, the passenger standing beside the door after |
| `20261119-163727-two-games-ride-trike` | fail (the scenario): without the 2.5 s LT before the exit the trike stopped beside a post with an online hologram; the guest placed at the trike for the swap stood 2.5 m off, facing away, and its X never got it on. The swap now starts with the host backing the trike off (2.5 s LT) |
| `20261119-164044-two-games-ride-trike` | **pass**: both directions on the pillion (K02 / L02 / L04: two Sams in tandem, both seated, on both screens), exits beside the trike |
| `20261119-172958-two-games-ride-forced-host` | **pass** (new scenario: the guest drives, the HOST rides; the host's game puts its own Sam off each time): link lost (2.5 s of the host's received datagrams dropped), crash (dv 11.0 m/s at 10.4 m/s: the D3-3 hit 1000 -> 881), taken (no placement), the driver got off; every exit capsule 1 -> 0 and the input back; afterwards the host walked 2.66 m on the pad (E5). The host's Sam stood on the road beside the truck after every exit (E3 / E5, and the guest's E2a) |
| `20261119-173506-two-games-ride-forced-host` | pass again (an experiment build that held the stopped truck's speed field at 29.5 km/h during the D3-3 call, reverted: no change, see 25.5) |
| `20261119-164402-two-games-ride-forced` | **pass**: link lost, crash (dv 10.3: the M4 hit, 1000 -> 881), taken (no placement: the guest's Sam, capsule on at the seat, was pushed out of the cab onto the ground beside it within 3 s), the driver got off, the roll (crash, dv 54.5: hit again); every exit capsule 1 -> 0 |
| `20261119-180508-two-games-ride-forced` | review build. Fail (the scenario): parts 1..4 pass with the placement gate; part 1's 2.5 s blackout started a new link session this time, which numbered the host's drive and the guest's attach `#2` early, so the re-drive's `until ... #2` matched an old line. The re-drive now waits on the ride module's seat counter |
| `20261119-181006-two-games-ride-forced` | **pass** (review build): link lost, crash (dv 10.2, the hit 1000 -> 881), taken, the driver got off, **jumped off** (a first hold at 4.7 m/s waits, the second at 4.6 m/s: put beside the reversing truck, capsule 1 -> 0, F05a "Off the truck: jumped off."), the roll (crash, dv 8.0, hit) |
| `20261119-181329-two-games-ride-forced-host` | **pass** (review build): the host as the passenger, every forced exit as before |
| `20261119-181555-two-games-ride-truck` | review build. Fail (the scenario): the new part 3 sent `ride_board` and `ride_test 9` as two steps; the guest's ACCEPT came in 67 ms, before the stop, so the host was carried and the stop's restore ran (put beside the truck, capsule 1 -> 0). `ride_test 10` now asks and stops in one call |
| `20261119-181926-two-games-ride-truck` | **pass** (final): both directions; part 3: `the mod stopped while asking to ride (#2D26D8D9): the request is withdrawn`, the guest `the partner ended ride #2D26D8D9: the mod stopped`, the late `ACCEPT for ride #2D26D8D9 after riding together stopped: ended at once`; the host then walked 4.14 m (never carried, input his) |
| `20261119-182234-two-games-ride-roadster` / `-182522-two-games-ride-trike` / `-182821-seat-measure` | **pass** (final): both directions; the pillion (K02b); the solo carries and the trike's emergency stop (put beside, capsule 1 -> 0) |

Screenshots checked enlarged (both screens): the truck R04b on the host (the guest's body seated at the passenger window)
and R02 / R03 on the guest (the carried view over the cab); the roadster P02 / P04b / Q03d / Q04; the trike K02 / K02b /
L02 / L04; seat-measure M04 / M13 / M23 / M25.

**Play test 2 fix (2026-11-20, the game's own prompt; hidden, foreground, one at a time).** Every run: both games exit 0,
real saves unchanged and restored, no violation, 0 cloud uploads, no FAULT.

| Run | Result |
|---|---|
| `20261120-102245-two-games-ride-native` | fail (the scenario's keyboard part): the guest's X on the lifted prompt boarded (the game crossed the truck out with reason 0; lifted; REQUEST, ACCEPT in 98 ms, carried, X off). The host's F through `hookkey` reached ds.exe's own keyboard procedure five times at the lifted prompt and the game never started: a hidden window's game keeps no keys for its own input. The host boards with X now |
| `20261120-102707-two-games-ride-native` | **pass**: both directions through the game's prompt (guest X, host X), the host's F gets off through our hook (`get off: pressed (key F)`), the fallback's hold of F boards and gets off |
| `20261120-102959-two-games-ride-truck` / `-103259-two-games-ride-roadster` / `-103553-two-games-ride-trike` | **pass**: every board `(the game's Use Vehicle)`, every exit `pressed (XInput pad #0)`; on the roadster and the trike the game asked its provider while carried and our hook made it RideOff (`carried: the game's own RideOff prompt is shown`; not seen in a screenshot), in the truck's cab it never asked |
| `20261120-103855-two-games-ride-forced` | **pass**: every forced exit; part 5 `a second press while the truck moves at 5.0 m/s: jumping off` |
| `20261120-104202-two-games-ride-save-host` | **pass**: boarded through the game's prompt; the save guard's exit as before |
| `20261120-104414-two-games-ride-native` | **pass** (final build): N1 (guest: "Use Vehicle" available, keyboard glyph F) / M1 (host: available, pad glyph); N2b / M2b side views of the passenger seated on both screens |

### 24.11 Not done / not proven

- **The physical keyboard F on the game's prompt** is not provable in a hidden run (the game ignores keys in a window
  that is never in front; run `20261120-102245`). It is the game's own binding of the same prompt ("F Use Vehicle" in its
  keyboard scheme, N1); the user's Local test proves it. The F key for getting off is our hook's (proven with `hookkey`).
- **A PlayStation pad** (the user's DualSense Edge) is read by the game itself: the native prompt needs nothing of ours.
  Getting off reads the game's pad state (+0x13 / +0x11): proven statically for the PlayStation converter, at run time
  for XInput only. While carried, a PlayStation pad is NOT blocked from the game (our carry block masks XInput and the
  keyboard only): X then also presses the game's RideOff where shown (the same exit), but other buttons reach the game.
- **The game's RideOff while carried**: offered (logged) on the roadster and the trike only, not yet seen on screen; in
  the truck's cab the game's provider is not asked (our notice says how to get off).

- **The flip exit** is proven only in the unit tests: every hidden flip (an upside-down teleport, a 1.5 s roll) made the
  owner's truck tumble, and a crash exit came first (the order puts a crash, with its M4 hit, before a flip). The exit
  spot of a rolled vehicle was along its tilted right axis (under the wreck on its right side, on top of it on its left)
  until the review; now on the ground plane (unit tests at 90 deg each way, the roof, the nose).
- **The new taken triggers** (Sam+0x678, a sequence, the requested state flags) and the waiting placement (asleep)
  came up in no hidden run; they cut none of the re-run rides short (no false trigger while carried).
- **The host carried and forced off**: proven on the truck in `two-games-ride-forced-host` (`20261119-172958`: link lost,
  crash, taken, the driver got off); the roll-over and the other kinds ran with the guest as the passenger only.
- **A save while the host is carried**: solved by the save guard (24.12, the user's choice "hook the save"). The old
  answer (the ride ended at the load mask's "save bit" `0x100`, then a placement after the save) never fired: `0x100` is
  not a save bit, and the save records Sam's live transform, so a save while carried would have loaded him at the seat.
- **The camera**: the on-foot camera as it is (24.5); no vehicle camera for the passenger.
- **Keyboard**: the ride key F (getting off, the fallback) is read from our keyboard hook at either hook code
  (`HC_NOREMOVE` too: ds.exe's pump peeks first, and ds.exe's own hook keeps every `HC_ACTION` key when it sits above
  ours). A release missed while the window loses focus no longer leaves it "down": the hook's state is confirmed by the
  key's physical state (hidden runs: the `hookkey` press itself).

- **The pose**: the driver's pose on the passenger (hands on a wheel that is not there, on the roadster and the truck);
  the game's `BotRiding` (v2) not tried. With the hologram partner body there is no seated pose.
- **D3-3**: done in M4 (section 25.5, run `20261119-155208-two-games-vehicle-hit-fast`).
- A vehicle driven out of the other world's streaming range (23.21) ends the ride (the vehicle is gone here).
- `ride_together=0` is a mod key only (no launcher checkbox).

### 24.12 The save guard (`native/mod/save_guard.*`): no save captures a carried Sam

The user's choice for 24.11's open problem: hook the save. Before any save of the game captures the player, a carried
Sam (host or guest) is taken off the ride and put on the ground beside the vehicle with his capsule on, the pose override
off and the input back; only then does the save run. The driver's own Sam is never touched (the game saves a driver
natively). Static research of the save path on ds.exe (Ghidra, `tools/re/dsre.py`), then hidden runs.

**The save path** ([P] proven by decompile / disassembly / xrefs, [R] seen in hidden runs):

- Every save the game makes is posted into ONE pending-save holder `M = [[0x147C0D6B0]+8]` through `0x143411580` ->
  `0x143411590`, which accepts or refuses it (a refused one never reaches the holder) [P]. Holder: `+0xC8` slot type
  (1 manual, 2 quick, 4 auto; `0xFF` = nothing pending), `+0xC9` index (-1 the next / newest, -2 the in-memory retry
  checkpoint), `+0xC0` reason, `+0xC4` id, `+0xE1` kind, `+0x118` "issued this frame", `+0x114` the autosave cooldown
  (7 s after an autosave [R]). Posters: the pause menu's Save `0x143236110`, the scripts' DSCreateSaveGame* `0x14365C770`
  (area / mission checkpoints), the autosaves `0x1428FE750` / `0x143419760` / `0x143422E60` / `0x143422FC0` /
  `0x14289C9D0`, the retry checkpoint `0x142DCE8B0` / `0x14365C5C0`, the post-load quick save `0x14263FA90` [P].
- Each frame `GameModule::Update 0x14263BAD0` calls `0x143632D60(GameModule+0x2A0)`, which calls the holder's processor
  `0x14341C350(M, float dt)` (through the thunk `0x14341C100`), then the save system's executor `0x143633640` [P]. The
  processor is the ONLY caller of the enqueue `0x143630440` (at `0x14341CA1A`); the executor turns the queued task into
  the capture job `0x1436297C0`, which reads the player entity's `+0xC8` position and `+0xE0` rotation and is joined at
  `SaveGameSystem::OnEndUpdateRendering 0x14362DCF0` in the same frame [P]. The processor runs on the mod's tick thread
  (`[save] the game's save processor runs on thread N (the mod's tick: N)` in every run) [R].
- The processor keeps a request pending across frames by itself (its "not now" paths just return) [P], but delaying
  it is NOT safe for every poster [P]: the autosave task `0x143419760` (a state-machine task, handlers `0x144DE8850`)
  posts its save (type 4, id `0x38E9493C`, `+0xC2` bit 2), sets its flag `0x20` and, at its next update, clears the
  holder (`0x143402D50`) when the save queue is empty (`0x14362D480`: `S+0x88 == 0`); the retry checkpoint
  `0x142DCE8B0` sets `0x20` the same way. A request the processor has not issued by then is wiped. The processor also
  drops (not delays) a request whose refusal reasons (`0x1433FEA20`) are set when it looks (`LAB_14341CBC0` ->
  `0x143402D50`), so looking later can lose a save. The guard therefore never delays the processor for the game's own
  saves (a first version did; review 2026-11-19).
- The save system's kick `0x14362DB80` (its vtable slot `0x143F199F0`, once per frame, on the tick thread [R]) submits
  the prepared capture job `S+0x98` (the task `S+0xA0`, in state 2, set by the executor at `0x1436338C6`). Its own wait
  [P]: while the game refuses (`0x1434022A0`) it counts `S+0xAC` and returns before submitting, up to 91 frames, then
  submits anyway (never a cancel); nothing joins the job before it is submitted (`OnEndUpdateRendering` joins `S+0xA8`,
  0 then). Returning from it early is that wait: the save stays queued (`S+0x88` 1, so the autosave task above sees it
  queued and waits), its job prepared exactly as the game made it, and the game's own kick submits it later.
- The saved position is the live entity transform; the load places Sam there with `SetWorldTransform 0x14235A010`, no
  ground snap (`PlayerGame::Save 0x142641A30`, spawn `0x142636A60`) [P]. A save while carried would load him at the seat.
- **The load mask's bit `0x100` is not a save bit** [P]: `ComputeSuspendMask 0x142627640` sets it while the SceneManager
  `[0x147BC9228]` has a waiting sequence ("a used sequence waits for its scene", the world suspended); the save path
  never touches the load mask. The old "the game is saving" checks in `game::PlacementGate` / `CarriedSamTaken` now say
  "the world waits for a scene (load mask 0x100)" (`DescribeLoadMask`: "scene wait").

**The hook**: MinHook detours of the kick `0x14362DB80` (the hold) and of the processor `0x14341C350` (logs, and a hidden
test's forced save kept pending; `dt` in `xmm1` passed through). The processor runs unchanged: every request is issued
when the game decides, so a poster finds its save queued as without the mod (`issued ... (at once, processor frame 1,
... the save queue now holds 1)` in every run, also while carried). Each frame with a capture job prepared, the kick
detour asks `rwire::SaveGate` (ride_wire.cpp, unit-tested):

| Step | When | What the kick detour does |
|---|---|---|
| run | no job; the post-load save (task kind 10, or the request's `+0xC0` 0x0F / id 0x43005559: its load flow waits on the queue; no ride survives a load); this capture was released or capped already (`done`: the cap is per capture and never restarts); our Sam not carried, no placement waiting and, within 10 s after his last carry, his entity (`+0xC8`) outside the ridden vehicle's body box (`rwire::InVehicleBox`), for 2 kick frames in a row | calls the game's kick (it submits, or waits by its own rules) |
| hold | our Sam carried; his exit placement still waiting for PlaceOn (its 500 ms rate limit); his entity still in the vehicle's box (an exit that did not move him: the game has him, gate 2, or a sleeping Sam's teleport not applied yet); fewer than 2 settled frames | returns without calling it; while he is carried the ride is asked to end (`WantsOff`) |
| run (cap) | held 3 s | calls the kick: the save runs unchanged, logged `CAP` (`in the vehicle's box (NOT placed)` when he is) |

`WantsOff` is set as soon as a request is pending while our Sam is carried (before the executor prepares its job), so
the ride usually ends in the frame the save is posted. While a held capture's job is prepared, a second request waits in
the processor (the executor would give it the job and orphan the held task; the queue is not empty meanwhile, so the
autosave task waits too), only while that job is there and at most the cap plus 1 s.

The ride (ride_together.cpp, same thread, the same frame in every run) ends at `WantsOff` with END reason 15 **the game
saves** (STQ1; `ForcedExit` order: after taken, before the link): `PlaceBeside` (PlaceOn mode 1, its ground probe, on
the passenger's side unless the vehicle slides that way), capsule on, input and pose back, END to the driver ("Off the
truck: the game saves." in the banner). The ride publishes "carried" and "placement waiting" to the guard every tick
and at each carry start / end (`saveguard::SetRideState`).

**Why the save is never lost**: the mod never writes the holder, a task or the job, never re-posts or copies a request
and never touches save bytes; the processor is never delayed for the game's own saves (issued as the game decides);
holding the kick is the kick's own wait, which ends in a submit, never a cancel, and is capped at 3 s per capture (a
capture released or capped is never held again); the post-load save is never held; with a failed byte check of the hold
or its reads there is no hook (saves are never delayed); after a whole-mod fault (`dllmain`'s emergency stop) the guard
is stopped first: both detours pass everything through and the ride's last published state is dropped (no tick would
refresh it). Side effects of a hold (2 frames, 62 to 78 ms in every run): the game's post refuses a second auto / manual
save meanwhile (reason `0x200000`, "pending or saving"; the autosave poster `0x1428FE750` skips its turn), as it does
while any save is queued or written.

**Boarding** waits while a save is pending or being written (`Block::saving` "the game is saving": the holder, the save
queue's head in states 1..4 or the capture job `S+0x98`); `StartCarry` refuses too (an ACCEPT arriving then is ended).
Without the guard's reads (a byte check) boarding as a passenger is refused altogether ("Save guard off: ... Riding as a
passenger is refused." in the co-op banner).
**Last resort**: a save pending or written without the hold (the guard off, its cap, the post-load save) also ends the
ride (`the save was not held: the last-resort exit`), and `PlacementGate` waits while the game captures or writes a save
(except a capture the guard holds), so no teleport of the mod races the capture; PlaceOn follows (run
`20261119-195908`, part 3).
**The sleeping passenger** (asleep in a menu while a save comes: the pause menu's Save): PlaceOn refuses a sleeping Sam,
so for a held save (never during a capture, and only when the game's other placement checks pass: `PlacementGate(...,
asleep_ok)`, the gate-2 checks of a sequence, a stopping game state, a snap ...) the carry's own entity teleport puts
him beside the vehicle and PlaceOn follows when he is awake. There is no ground probe: the spot is lifted to the
vehicle's own ground plane under it (a slope rising beside the vehicle) plus 0.8 m (`rwire::SaveSpotHeight`,
unit-tested on a 15 deg slope), so the save never records him inside the ground (he drops onto it after the load); the
guard still waits until his entity reads outside the box. Not seen in a hidden run (below).
**The game refuses some saves itself** [R]: its post (reason query `0x1433FEA20`) refused auto saves while Sam drives
(reason `0x200`, from its player state) and, now and then, while he is carried (`0x400`, its player state `+4` bit 13;
for 20 s straight with the cuff map open in run `20261119-200541`); other times it took them while carried. The guard
handles every save the game takes.

**Byte checks** (`kSites` in save_guard.cpp, 17 sites, plus the save system's vtable slot `0x143F199F0` holding the
kick: 18 checks; a mismatch turns that part off with one line in the log and, for the hold or its reads, "Save guard off:
<reason>. A save while riding as a passenger may record Sam at the seat." (reads off: "Riding as a passenger is
refused.") in the co-op banner; a failed reads site turns the hold off too):

| Site | Bytes | Part |
|---|---|---|
| the processor `0x14341C350` (`M+0x118`, `xmm1`) | `40 57 48 81 EC C0 00 00 00 48 8B 05 D0 4C B4 01 48 33 C4 48 89 84 24 90 00 00 00 C6 81 18 01 00 00 00 48 8B F9 48 8B 05 9C 9F 7C 04 C5 F8 29 B4` | hook (both detours) |
| its thunk `0x14341C100` (`[W+8]`) | `48 8B 49 08 E9 47 02 00 00` | hook, reads |
| the save system per frame `0x143632D60` (GameModule `0x147BE6318`, W `0x147C0D6B0`, the thunk, the executor) | `40 53 48 83 EC 20 48 83 3D AA 35 5B 04 00 48 8B D9 74 1E 48 8B 0D 36 A9 5D 04 48 85 C9 74 05 E8 7C 93 DE FF 48 8B CB 48 83 C4 20 5B E9 AF 08 00 00 48 83 C4 20 5B C3` | hook, reads |
| nothing pending `0x14341C432` | `80 BF C8 00 00 00 FF` | hook, reads |
| the issue `0x14341C9AD` (the holder's fields, `M+0x118 = 1`, `GameModule+0x2A0`, the enqueue) | `80 BF C0 00 00 00 0A 44 8B 8F CC 00 00 00 44 0F B6 87 C9 00 00 00 0F 95 C0 0F B6 97 C8 00 00 00 88 87 E2 00 00 00 0F B6 87 E1 00 00 00 48 89 7C 24 40 88 44 24 38 0F B6 87 E0 00 00 00 88 44 24 30 48 8B 87 D8 00 00 00 48 89 44 24 28 0F B6 87 D0 00 00 00 C6 87 18 01 00 00 01 48 8B 0D 09 99 7C 04 48 81 C1 A0 02 00 00 88 44 24 20 E8 21 3A 21 00` | hook, reads |
| is saving `0x1436323B0` (`S+0x88`, `S+0x90`, head `+0xC`) | `83 B9 88 00 00 00 00 74 1D 48 8B 81 90 00 00 00 48 8B 08 8B 51 0C 8D 42 FF 83 F8 02 76 05 83 FA 04 75 03 B0 01 C3 32 C0 C3` | hook, reads |
| pending or saving `0x143409C30` | `40 53 48 83 EC 20 48 8B D9 48 8B 0D D8 C6 7D 04 48 81 C1 A0 02 00 00 E8 64 87 22 00 84 C0 75 15 48 8B 43 08 80 B8 C8 00 00 00 FF 75 08 32 C0 48 83 C4 20 5B C3` | hook, reads |
| the capture job's kick `0x14362DB80` (`S+0x98` job, `S+0xA0` task, its own wait `S+0xAC` < 91, `S+0xA8` in flight); its vtable slot `0x143F199F0` | `48 89 5C 24 10 57 48 83 EC 30 40 32 FF 48 8B D9 48 83 B9 98 00 00 00 00 0F 84 97 00 00 00 48 8B 81 A0 00 00 00 48 85 C0 74 66 40 38 78 6D 74 60 4C 8B 15 F9 FA 5D 04 4D 85 D2 74 51 80 78 48 05 0F B6 48 10 44 0F B6 40 6F 41 0F 94 C3 80 F9 04 74 09 80 F9 01 74 04 33 D2 EB 02 B2 01 44 0F B6 48 6E 49 8B CA 44 88 44 24 20 45 0F B6 C3 E8 AD 46 DD FF 0F B6 F8 84 C0 74 16 FF 83 AC 00 00 00 83 BB AC 00 00 00 5A 0F 8E 9D 00 00 00 40 32 FF B8 01 00 00 00 F0 0F C1 83 A8 00 00 00 48 8B 93 98 00 00 00 48 8D 0D 55 DB 14 02 E8 10 85 3D FE 40 84 FF 75 75`; the slot holds the kick | hook, reads |
| the executor's capture job `0x1436338C6` (`S+0xA0` task, `S+0xAC`, task state 2) | `48 8B 45 E7 48 89 87 A0 00 00 00 B8 FF FF FF FF 44 89 BF AC 00 00 00 C7 46 0C 02 00 00 00` | hook, reads |
| the executor's result `0x143633A39` (`S+0x12F`) / the finished slot `0x14341D160` (`M+0x10C` / `+0x10D`) | `E8 22 97 DE FF 80 7E 48 0A 75 2D 80 BF 2F 01 00 00 01` / `48 8B 41 08 88 90 0C 01 00 00 44 88 80 0D 01 00 00 C3` | result (logs; the site also the hook: the post-load task kind `+0x48` 10) |
| DSCreateSaveGame `0x14365C560` / its body `0x14365C770` | `48 83 EC 58 48 8D 44 24 40 48 C7 44 24 40 00 00 00 00 48 89 44 24 38 41 BA 40 00 00 00 48 8B 84 24 88 00 00 00 48 89 44 24 30 0F B6 84 24 80 00 00 00 88 44 24 28 66 44 89 54 24 20 48 C7 44 24 48 00 00 00 00 E8 C6 01 00 00 48 83 C4 58 C3` / `48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 81 EC 20 01 00 00 48 8B 05 9E 48` | forced save (tests) |
| the refusal reasons `0x1433FEA20`, the post's call `0x143411745` and cooldown read `0x143411785` (`M+0x114`), DSCanSaveGame `0x143634FB0` (`GameModule+0x328`) | `40 53 48 83 EC 20 48 89 6C 24 30 48 8B DA 48 89 74 24 40 41 0F B6 E9 C7 02 00 00 00 00 41 0F B6` / `E8 D6 D2 FE FF` / `C5 FA 10 86 14 01 00 00` / `48 8B 05 61 13 5B 04 48 85 C0 74 0C 83 B8 28 03 00 00 00 75 03 B0 01 C3 32 C0 C3` | diagnostics (tests) |

The guard is installed with riding together (`ride::Active()`) or in a hidden run.

**Debug action** `save_guard` (docs/debug-actions.md "Save guard"): `0` state and counters; `1 [type] [1|2]` a save now
through the game's own DSCreateSaveGame (hidden runs with the save redirect: the scratch copy), as the scripts post one
(2: a second call at once while the first is pending);
`2 [n]` our Sam's place against where the last held save released him (`where (check n)`); `3` the pause menu's Load of the run's newest save
(host, the M1 path); `4 0/1` the hold off / on; `5` the game's refusal reasons now; `6 [s] [1|2]` / `7 [s]` keep the next
forced save pending until our Sam boards (1: and sleeps; 2: and the vehicle drives at 8 m/s or more) / without letting anyone board. The log names each save (`#N`,
`(forced save Fn)`) and how it ran: `pending`, `runs at once`, `HELD`, `released after N frames`, `CAP`, `issued ... (at
once | after the hold | the hold was off | after the cap)`, the save system's states, `finished ok: #N ... the newest save
file in this run's scratch folder`.

**Hidden scenarios** (`-SeedSaves complete-100 -GuestSeed profile-only`, `ini host_save=1`):

| Scenario | What |
|---|---|
| `two-games-ride-save-host` | 0. no ride: a forced save runs at once; 1. the guest drives the truck, the host rides; a forced save on the host (asked again each second while the game refuses it) is held, the ride ends "the game saves", Sam beside the truck, capsule 1 -> 0, the input back, then released, issued "after the hold" with Sam not carried, written; 2. the host loads that autosave (`save_guard 3`, the guest follows): Sam stands where the guard put him; walks |
| `two-games-ride-save-guest` | the same with the guest carried (its sandbox scratch); no boarding while a save is pending (`save_guard 7`, `ride_test 11` "the game is saving"); the hold off: the last-resort exit |
| `two-games-ride-save-none` | nobody rides: a forced save on each game runs at once; the host driving: the guard never holds (the game itself refuses, reason `0x200`) |
| `two-games-ride-save-break` | the guest carried, under stress: A. a second DSCreateSaveGame while the first is held (`save_guard 1 4 2`); B. a save while the truck drives at 8 m/s or more (`save_guard 6 40 2`: posted on foot, kept pending until then); C. a 2.5 s link blackout on both games and a save; D1. a save pending while the guest boards (`save_guard 6`); D2. a save posted while the REQUEST is on its way (a 1 s blackout on the host delays it): the ACCEPT finds the save pending, the carry is refused, END to the driver, the save runs on foot; finally `save_guard 0`: every request issued and finished ok, none dropped or gone |

**Evidence** (hidden, foreground, one at a time, 2026-11-19; every run: both games exit 0, real saves unchanged and
restored, no violation, 0 cloud uploads):

| Run | Result |
|---|---|
| `20261119-195448-two-games-ride-save-host` | **pass**. F1 (on foot) runs at once, written as autosave4 in 0.19 s. F2 posted while the host rides the truck: `HELD`, in the same frame the ride ends (`the game saves`), PlaceOn puts Sam on the truck's right side on the road, capsule 1 -> 0, the input and pose back, END to the guest; released after 2 frames (62 ms) with Sam at (2.20, 0.80, 0.18) in the truck's frame (the passenger seat is (0.44, 0.80, 1.95)); issued "after the hold", Sam not carried; written as autosave5. The host's in-world Load of autosave5 (the guest followed, RESULT ok): Sam 0.00 m from the release spot, no parent; S3 on both screens: standing on the road beside the truck, not at the seat, not inside it, not under the ground; then walked 0.58 m |
| `20261119-195908-two-games-ride-save-guest` | **pass**. The guest's F1 while carried: held 2 frames (62 ms), the same exit and placement, written as autosave0 in the guest's scratch; G1 (host): the guest standing beside the truck. F2 kept pending (`save_guard 7`): `boarding now: the game is saving`. F3 with the hold off: issued with Sam CARRIED at the seat (0.44, 0.80, 1.95), what the guard prevents; the last-resort exit, `put beside the truck once possible (the game is saving (its capture or write))`, PlaceOn 230 ms later |
| `20261119-201433-two-games-ride-save-host` | **pass** on the final build (the same: held 2 frames, 62 ms; autosave5 reloaded, Sam 0.04 m from the release spot, no parent) |
| `20261119-200326-two-games-ride-save-none` | **pass**: the host's and the guest's saves on foot at once (0 ms); the host driving: the game refused the save (reason `0x200`), nothing held |
| `20261119-194258` / `-194901` / `-195122-two-games-ride-save-host` | development: the first forced save while carried refused by the game (no diagnostics yet); then the real path (`HELD` -> released after 2 frames -> written); `-195122` passed every step, the reload included (0.02 m), and failed only on an optional step that counted as a failure (removed) |
| `20261119-195648-two-games-ride-save-guest` | development: the game refused the carried guest's save (reason `0x400`); the scenarios now ask again each second |
| `20261119-200137-two-games-ride-save-none` | development: the driving host's save refused (`0x200`); the scenario now accepts either outcome |
| `20261119-200541` / `-200902` (a pause-menu scenario, dropped) | START opens the cuff map with Sam awake (the game refused carried saves for 20 s, `0x400`); BACK opened nothing: no way found to make Sam sleep in a hidden run |

Screenshots checked: S1-guest-sees-host and S3-host-loaded / S3-guest (`-195448`), G1-host and G1-guest-off (`-195908`).

**Independent verification** (a second pass on 2026-11-19, the same rules; every run: both games exit 0, saves unchanged,
restored, no violation, 0 cloud uploads, no FAULT line):

| Run | Result |
|---|---|
| `20261119-210219-two-games-ride-save-host` | **pass** with the reload check made strict (`save_guard 2 2`: before, `waitlog ... where:` also matched the line from before the load). F2 held 2 frames, released with Sam at (2.20, 0.80, 0.18) in the truck's frame; after the in-world Load of autosave5 (a new Sam entity, no parent): 0.04 m from the release spot; S3: standing on the road beside the truck |
| `20261119-210426-two-games-ride-save-guest` | **pass** (also `-202139` before the test-option changes) |
| `20261119-202416-two-games-ride-save-none` | **pass** |
| `20261119-205834-two-games-ride-save-break` | **pass**. A: the second DSCreateSaveGame while F1 was held left the holder unchanged ("the same request"): one request, held once, issued once; later attempts refused by the autosave cooldown. B: held at 8.1 m/s, Sam put on the truck's right side, 1.5 m back along its path, on the road; written; 1.5 s and 5 s later still 0.00 m from the release spot (B2: standing on the road). C: the save during the blackout was held and written; the END reached the host after the blackout (the link resends). D1: held in the frame the carry started. D2: `cannot carry our Sam: the game has a save pending`, END to the driver, the save ran on foot. Counters: requests 5, held 4, released 4, issued 5, dropped 0, gone 0, finished ok 5 |
| `-202558-two-games-ride-truck`, `-202906-two-games-ride-forced-host`, `-203134-two-games-hostsave-reload`, `-203346-two-games-hostsave-continue`, `-203552-two-games-veh-drive` | **pass** (regressions; no save was posted in them while anyone was carried) |
| `-204107` / `-204648` / `-205003` / `-205303` / `-205545-two-games-ride-save-break` | development of that scenario: an optional step counted as a failure; B's save came only at 3 m/s (the game refused carried saves, `0x400`, so the test now posts it on foot and keeps it pending until 8 m/s); a regex for the issue line used on the finish line; a 2 s blackout delayed the REQUEST past its 3 s wait (1 s now). The guard itself never failed: every request issued and finished ok once |

**After the review** (2026-11-19/20: the hold moved from the processor to the kick, the per-capture cap, the box check,
the emergency stop, no boarding without the reads, the sleeping passenger's checks and height; the same rules, every run:
both games exit 0, saves unchanged, restored, no violation, 0 cloud uploads, no FAULT line):

| Run | Result |
|---|---|
| `20261119-213714` / `20261120-002543` / `-004837-two-games-ride-save-host` | **pass**. F2 posted while the host rides: issued by the game at once (processor frame 1, the queue holding 1: the autosave task's step-2 check finds it queued), its capture HELD at the kick, the ride ends, released after 2 frames (62 to 78 ms) with Sam at (2.20, 0.80, 0.17) in the truck's frame, outside its box; written. The truck was driven ~10 m after the boarding: the reloaded Sam stands 0.00 m from the release spot and 6.3 to 7.3 m from where he boarded (`where (check 2)`), so the save holds the placed spot, not a stale one; S3: on the road beside the truck |
| `20261120-002809` / `-003915-two-games-ride-save-guest` | **pass** (the same for the guest; `boarding now: the game is saving`; the hold off: the last-resort exit) |
| `20261120-003032` / `-005047-two-games-ride-save-none` | **pass** (at once on foot; the driving host's save refused by the game) |
| `20261120-003209-two-games-ride-save-break` | **fail**, B only: the test's own processor wait (Command 6) released F2 while the game's reasons had `0x400` set (carried), and the game's processor dropped it (`not issued: the game's processor dropped it`), the delayed-processor loss the review predicted. The test wait now releases only while none of the processor's drop reasons is set (`kProcessorDropMask`, logged `would drop the save now`); the game's own saves are never delayed there |
| `20261120-003625-two-games-ride-save-break` | **pass**: B waited 78 ms for reasons `0x00200400` to clear, then held at 8.8 m/s, Sam put 1.9 m back along the path on the road; counters: requests 4, held 3, released 3, issued 4, finished ok 4, dropped 0, gone 0 |
| `20261120-004135-two-games-ride-forced-host`, `-004412-two-games-hostsave-reload`, `-004628-two-games-hostsave-continue` | **pass** (regressions) |

**Not proven / open**:

- **The sleeping passenger** (a save while Sam sleeps in the pause menu): the entity teleport before the release came
  up in no hidden run (no menu that makes Sam sleep is reachable with the virtual pad).
- **The cap and the box check**: no run held a capture to its 3 s cap, or held one because Sam's entity was still in the
  vehicle's box after an exit that did not move him (unit-tested only). The autosave task `0x143419760` was not
  triggered in a run; the runs only show its condition (the queue holds the save while the capture is held).
- **The game's own saves**: only the scripts' DSCreateSaveGame request was forced; every other poster goes through the
  same holder and processor [P], none came up in a run while carried. Which of them the game refuses while carried
  (`0x400`) is not mapped; what bit 13 of the player state means is not known.
- **The pause menu's Save** during a hold (2 frames): the game refuses a second save meanwhile; how its menu shows that
  was not seen.
- **On the move**: an autosave while the vehicle drives puts the passenger off at speed (the forced placement, back
  along the path); no automatic re-boarding after the save.
- What else of the carried state the save serialises (the capsule, the pose: mod state; the vehicle mount: the game
  saved no parent, Sam loaded standing) beyond the position checked here.

## 25. Vehicle hits (`session.ini partner_vehicle_damage`, default 1, with `vehicle_sync=5`; design milestone M4)

"When the vehicle collides with players it should damage them and knock them down like an NPC enemy driven vehicle";
"they can collide with players and other player vehicles too". `native/mod/partner_tackle.{hpp,cpp}` (the game side),
`native/mod/tackle_rules.{hpp,cpp}` (the pure rules, ctest `st_tackle_rules_tests`). Decisions: D4-1 the MULE truck hit
(fallback: the proven player-vehicle hit), D4-2 no instant kill, D4-3 the partner's vehicle hits only you, D4-4 on with a
launcher checkbox to turn it off; D3-3 the passenger thrown off in a crash takes the same hit.

### 25.1 How the game hurts Sam with a vehicle, and why the mod must help

The vehicle body never touches Sam's proxy (layer pair (17 Vehicle, 8 Player) = 0). The damage is an attack: every frame
`VehicleEntity::Update` calls `VehicleTackleEmit` `0x142F8ABF0`, which casts a sphere (the vehicle's local offset
`[V+0xA70]`, forward component negated while reversing, radius `[V+0x57C]`) and sends a DamageInfo to whatever it overlaps,
only while: the driver id `[V+0x8B0]` is not `0x1FFFFF` (or the ride state `+0x4CC` is 4), the speed field
`[[V+0x330]+0xF0]` is over 15 km/h, and two flag gates pass. An NPC driver (a MULE truck) uses the attack in slot `+0x9E0`,
`VehicleTackle 0x157` (trucks only: 100 damage, reaction **Blow**); a player driver, slot `+0xA00`, `VehicleTacklePlayer
0x158` (every vehicle: 400 damage, Blow). The amount passed is 100 up to 30 km/h and **10000** above.

On the victim's screen the partner-driven vehicle is our follower (the game's NPC controller, driver `0x1FFFFF`, ride
state 2): the game never lets it hit anything (proof runs `20261118-224252-zz-proof-hit`, `-224528-zz-proof-hit2`: the
follower truck shoved Sam 3 m, no damage). So **the victim's game decides the hit on its own Sam** and runs the game's
attack for the follower (`PartnerTackle`). The driver's game never tackles the partner body (a cutscene actor with only the
layer-10 capsule); it sees the knock-down through the pose stream (STJ1), and gets an STH1 report for its log.

### 25.2 The gate (`trules::Gate`, every tick, game thread)

In this order: the feature on (checks, the shared vehicles with remote driving); `partner_vehicle_damage=1`; the partner
drives a vehicle we follow; its newest frame says it holds the driver seat and the vehicle's driver id is `0x1FFFFF` with
ride state 2 (never a vehicle with a driver of its own); our Sam present, awake, on foot (no parent), not carried as a
passenger (`ride::Carried`); no load / menu / cutscene / post-load setup / sequence (`game::LocalCollisionBlock`, the
sequence answer held 250 ms) and the attack gates of `VehicleTackleEmit` itself (`+0x560` bits 4 and 8, the global stop
bit); within 50 m (the game's MULE gate); the speed field over 15 km/h; no hold-off for this vehicle; a path for its kind;
our Sam's centre (root + 1 m) within the attack sphere's radius + 2 m of its centre (the bracket: inside its radius, since
the game's sphere hits whatever it catches there). Each change of the answer is one
`[vhit] partner vehicle ...` line (a flapping answer at most one a second).

### 25.3 The hit: two paths (D4-1)

| Kind | Path | Attack | What runs |
|---|---|---|---|
| truck (its `+0x9E0` slot present); trike, roadster (`0x158`, slot `+0xA00`) | **direct** | `0x157` VehicleTackle, the MULE truck attack; `0x158` VehicleTacklePlayer | the game's attack sphere query `0x1429934E0` called by the mod: the slot's context (`[[V+0x9E0]+0x18] - 0x20`, the wrapper's own step), the vehicle's own sphere (centre from `[V+0xA70]` / `[V+0x580]` and the entity transform, radius `[V+0x57C]`), amount **100**, the game's filter (vtable `0x143BA8938`, init `0x14382FD30(f, V, 0x36)`: the vehicle's own bodies left out, its list released with `0x1419D5870` as the game does), and **our callback** (`cb(ctx, receiver, &DamageInfo, user)`, called by the query for each hit owner): a hit is delivered only when its point (DamageInfo+0x80, three doubles) is within 1.5 m of our Sam's centre (D4-3: Sam and the cargo on him; NPCs, MULEs, BTs and cargo on the ground are not hit). The preliminary capsule (0 damage) is not run |
| any kind, only when the direct path's checks failed | **state-4 bracket** (proven, `20261118-224729-zz-proof-hit3`) | `0x158` VehicleTacklePlayer | the vehicle's ride state `+0x4CC` = 4 for one call of the game's `VehicleTackleEmit` and back (it selects slot `+0xA00` and skips the driver-id write); above 30 km/h the speed field is held at 29.5 km/h for the call and restored, so the game passes 100, not 10000 |

`trules::PathFor`: auto: the direct path for every kind whenever its checks passed (`0x157` where the vehicle has the
`+0x9E0` slot, else `0x158`): it alone keeps the hit to our Sam (D4-3); the bracket only when the direct path's checks
failed. Until the review the trike and the roadster took the bracket, so NPCs, MULEs and cargo on the ground inside the
sphere were hit too, and while our Sam stood only near it the call repeated every tick with no hit to end the pass. No
call goes through an empty attack slot: the game empties `+0x9E0` / `+0xA00` / `+0xA20` / `+0xA40` together when a vehicle
leaves the world (`0x142F76180`), and `VehicleTackleEmit` reads through them without a null check, so the direct path
needs its context and the bracket `+0xA00` and `+0xA20` (else `[vhit] ... no attack context in its slot ...: not
called`, once). `vehicle_hit 1 <kind>
<0|1|2>` forces a path (tests). The direct path never writes the vehicle; the bracket writes `+0x4CC` (and the speed field)
for one synchronous call on the game thread and restores them; a fault in either turns that path off for the session
(`[vhit] FAULT ...`). The hazard of the research: never an NPC-range driver id in `+0x8B0` of a trike or roadster (a null
attack context): the mod never writes `+0x8B0`.

**The cap (D4-2).** Three layers: the direct path passes 100 at any speed; the bracket holds the speed field at 29.5 km/h
over 30; and Sam's MsgDamage handler `0x14271BC00` (detoured at start-up, with `partner_vehicle_damage=1` only; every
message passes) caps a message of the attack we used in the last 1.5 s at 100. The message (run `20261119-153401`):
`+0x20` the DamageInfo; its `+0x28` the attack reference `[ctx+0xE8] + 0x20`, its `+0x40` the context the query was given,
its `+0x78` the amount. In every hidden run the cap had nothing to cut (0 capped): 100 arrived at 40 km/h.

**Rate.** `trules::HitLimiter`: the mod calls every tick while the gate passes until Sam's damage handler sees a damaging
message of that attack (or his health drops), then holds off 1.5 s for that vehicle; a pass with no call for 1 s is over
(a later hit or fall is never counted for it; run `20261119-154830` showed why). Measured: 6..9 calls and **2 damage
messages per pass** (the second call's message is queued before the first one is handled).

**Log.** One line per hit, numbered: `[vhit] partner vehicle truck 0x020005 hit Sam (#1): attack 0x157 speed 28.2 km/h
health 1000.0 -> 881.0 (direct; 9 call(s) this pass, 2 damage message(s) of the attack, 0 capped, 0 preliminary; seen by
Sam's damage handler)`, and an overlay notice "Hit by your partner's vehicle.". The victim sends STH1 to the driver, whose
log says `[vhit] the partner's game reports (#1) our truck 0x020005 hit their Sam: attack 0x157 (direct) speed 28.2 km/h
health 1000.0 -> 881.0; ...`.

### 25.4 What a hit does (measured)

| Vehicle, path | Speed at the hit | Health (one pass: 2 messages) | On screen |
|---|---|---|---|
| truck, direct 0x157 | 28.2 / 28.6 km/h (both directions) | 1000 -> 881 (first message, 119), then 845.3 (second, 35.7) | thrown, lying on the road, cargo shaken off, "Lou's stress level is increasing", the health popup (846/1000), getting up |
| truck, direct 0x157 | 40.0 km/h (full throttle from 22 m) | 1000 -> 881 -> 845.3 | the same: knocked down, never killed |
| Kusanagi trike, bracket 0x158 | 40.0 km/h (held at 29.5 for the call) | 1000 -> 881 -> 845.3 | the same |
| truck, D3-3 crash exit (dv 8.8 m/s at 8.2 m/s; final build: dv 14.2 m/s at 13.6 m/s, 49 km/h) | the passenger beside it | 1000 -> 881 | "Thrown off in the crash.", the health popup; knocked down only while the vehicle still moves (25.5) |

The same 119 + 35.7 as the proof's bracket at 16-18 km/h: the hit is the game's own knock-down damage, whatever the speed.
The driver's screen shows the partner body thrown and lying through the pose stream (H1b-host-side of
`20261119-160320`: the guest lying in front of the host's truck; F1b-host-side of `20261119-155208`).

### 25.5 The passenger thrown off in a crash (D3-3)

`ride::SetCrashHitHandler` (section 24.7). On a crash exit the ride module puts our Sam beside the vehicle and turns his
capsule on; the handler then (150 ms later, so the placement has gone through) runs the direct path once a tick for up to
1 s with a 1 m sphere 0.6 m from his centre toward the vehicle (the truck's `0x157`, else `0x158`), until his damage handler
sees the hit, with the regular tackle's gates (`LocalCollisionBlock` with the sequence, the game's attack stop): while one
blocks, the hit waits (`crash exit: the hit waits: ...`), and the window ends without a hit. `partner_vehicle_damage=0` turns this off too (the exit then only puts him beside the vehicle). Proven on the
truck (`20261119-155208`, F3a / F3b); on the trike and the roadster the same call with `0x158` (not run hidden).

**The knock-down is not reliable (verification, 2026-11-19).** The damage always arrives (1000 -> 881 in every crash exit),
but the knock-down only when the vehicle in the passenger's world still moves at the hit: in `20261119-165049` (the
follower truck at 3.3 m/s, tilting) the guest was thrown 1 m in 0.5 s (speed 3.6 m/s) and fell off the road's edge; in
`20261119-164402` (the guest) and `20261119-172958` / `-173506` (the host) the truck had stopped dead (`ride_test 6`) and
Sam never moved from the placement (`[sync] trace` identical for 5 s, speed 0) and stood upright on the partner's screen
(E2a-guest-sees-host). The game's own `VehicleTackleEmit` passes the same zero vectors as the mod and only runs above
15 km/h, so the reaction presumably comes from the attacker's motion; holding the speed field at 29.5 km/h for the call
changed nothing (`-173506`). Open: a knock-down for a passenger thrown off a vehicle that stopped dead needs the reaction
side of Sam's damage handling researched.

### 25.6 Vehicle against vehicle (M4.4)

Both real vehicles are dynamic bodies in each world ((17,17) = 1): the local one reacts for real, the follower is pulled
back by the drive module's correction bands (section 23); vehicle life stays the owner's. Measured
(`20261119-155954-two-games-vehicle-bump`):

- **The ram** (the guest's truck at 7.3 m/s into the host's parked Kusanagi trike): the host's game granted the guest's
  push CLAIM of the trike (`the guest claims trike 0x01E005 (push, epoch 0): granted`); the host's follower truck error
  max 0.17 m in that second; life equal on both games before and 2 s / 5 s after (truck 431.0 / 15000, trike 4862.0 / 7500).
- **Head-on** (the host's trike and the guest's truck driving at each other, contact B2a): the guest's follower trike
  error spike 1.05 m / 4.1 deg, back to 0.17 m within 2 s by velocity nudges (22 nudges, 0 teleports, 0 blocked); the host's
  follower truck 0.48 m / 2.4 deg, back within 2 s (9 nudges, 0 teleports). The follower trike lost 10 life in the guest's
  world; the owner's value overwrote it (`life 4852.0 -> 4862.0 (the owner's)`).
- No RequestTeleport into an overlap: the drive module teleports only where the footprint is clear. In the final run
  (`20261119-161456`) the head-on left the host's follower truck 0.35 m off with the trike in its place: "its place is
  taken (the local Sam or another vehicle): waiting" (3 times), then the teleport once clear.

**The partner capsule** (`partner_collision.cpp`, layer 10, section 11): two rules added, from the drive module's vehicle
footprints (`vshare::Footprints`, speeds from their movement between the drive module's polls, over the polls' own time
apart (`vshare::FootprintsMs`; sampling every 50 ms read every other sample as standing or twice as fast, and the 1 m/s
rule flapped), moving on over 1 m/s and off at 0.6 m/s, `vhit::CapsuleVehicleBlock`): out of the world
while a vehicle moving over 1 m/s is within 5 m of the partner body ("off: a vehicle moves within 5 m of the partner":
NPC trucks, a rolling vehicle, the partner's own just after they got off), and it does not come back (nor attach) while a
vehicle's box overlaps the capsule (its point and 1 m / 2 m above it, within 0.4 m: "waiting: a vehicle overlaps the
partner"). Seen in `20261119-155954` part 3 (the host's trike passing 3 m from the guest's body).

### 25.7 Settings

`partner_vehicle_damage` (session.ini, default 1; the launcher's Debug page **Partner's vehicles can knock me down**,
unticked writes `partner_vehicle_damage=0`): each game decides for its own Sam. Off: the partner's vehicles never hurt or
knock down this player's Sam (also not when thrown off in a crash); the game's own vehicle contact still shoves him (as
before M4); the partner, if their own setting is on, is still hit by ours (run `20261119-155455-two-games-vehicle-hit-off`).

### 25.8 Byte checks (`kSites` in partner_tackle.cpp: 22 sites + 2 structural)

Groups: common (both paths: the emit and its gates), direct, bracket, the damage-handler detour. A mismatch turns off the
path it belongs to (both: the hits are off, with a one-line reason in the log and the co-op banner "Vehicle hits off:
..."); a failed detour check leaves the hits on, seen by the health drop only.

| Site | Bytes | Group |
|---|---|---|
| `0x142F8ABF0` VehicleTackleEmit | `40 55 53 48 8D 6C 24 88 48 81 EC 78 01 00 00 48 8B 05 2A 64 FD 01 48 33 C4 48 89 45 20 48 8B D9` | common |
| `0x142F8AC10` its gates (`+0x560` & 0x10, the global's `+8` bit 6, driver / state 4, `+0x560` bit 8, speed > 15) | `8B 89 60 05 00 00 F6 C1 10 0F 85 31 04 00 00 48 8B 05 62 C2 C6 04 0F B6 50 08 C0 EA 06 F6 C2 01 0F 85 1A 04 00 00 8B 93 B0 08 00 00 81 FA FF FF 1F 00 75 0D 83 BB CC 04 00 00 04 0F 85 FF 03 00 00 C1 E9 08 F6 C1 01 0F 85 F3 03 00 00 48 8B 83 30 03 00 00 C5 FA 10 80 F0 00 00 00 C5 F8 2F 05 E0 12 C4 00 0F 86 D6 03 00 00` | common |
| `0x142F8ACB9` the attack choice | `81 FA FF FF 1F 00 74 0C F7 C2 00 E0 1F 00 0F 84 9B 00 00 00 83 BB CC 04 00 00 04 0F 84 8E 00 00 00` | direct, bracket |
| `0x142F8AD61` the slots `+0x9E0` / `+0xA00` | `BF E0 09 00 00 EB 05 BF 00 0A 00 00` | direct, bracket |
| `0x142F751E3` every vehicle's add-to-world: 0x158 into `+0xA00` | `BA 58 01 00 00 48 8B CF E8 A0 5A A1 FF 48 8D 8F 00 0A 00 00 48 8B 90 E8 00 00 00 E8 0D E2 4E FF` | direct, bracket |
| `0x142F7500D` the truck's add-to-world: 0x157 into `+0x9E0` | `BA 57 01 00 00 48 8B CF E8 76 5C A1 FF 48 8D 8F E0 09 00 00 48 8B 90 E8 00 00 00 E8 E3 E3 4E FF` | direct |
| `0x142F8AC94` the attack's filter on its stack (vtable, 2, 0xFF, zeros) | `4C 8D 3D 9D DC C1 00 4C 89 7D B0 C7 45 B8 02 00 00 00 48 C7 45 BC FF 00 00 00 48 89 75 C4 89 75 CC 48 89 75 D0` | direct |
| `0x142F8AD82` the filter's layer 54 | `41 B8 36 00 00 00` | direct |
| `0x142F8ADA6` the filter init call, `+0xC` = 1 | `E8 85 4F 8A 00 8B 93 B0 08 00 00 41 BE 38 00 00 00 C7 45 BC 01 00 00 00` | direct |
| `0x142F8AE19` the amount by speed, the sphere's direction by `+0x580` | `48 8B 83 30 03 00 00 48 8D 54 24 58 C5 FA 10 05 5B E0 BF 00 C5 FA 10 0D 3F 82 C0 00 48 8B 8B 40 03 00 00 C5 FA C2 90 F0 00 00 00 01 C5 FA 10 83 80 05 00 00` | direct |
| `0x142F8AE85` the sphere's local offset `+0xA70` | `C5 E8 59 83 70 0A 00 00` | direct |
| `0x142F8AEB0` the sphere's radius `+0x57C` | `C5 FA 10 93 7C 05 00 00` | direct |
| `0x142F8AEBB` the hit call's arguments | `C7 44 24 40 20 00 00 00 C5 FA 11 7C 24 38 48 8D 48 E0 89 74 24 30 48 8D 45 A0 48 0F 44 CE 48 89 44 24 28 48 8D 45 B0` | direct |
| `0x142F8AFFF` the filter's list freed | `48 8B 4D D0 4C 89 7D B0 E8 64 A8 A4 FE` | direct |
| `0x142993440` the hit wrapper (the query's argument layout: callback 0, zero vectors, 1.0, 0x20) | 112 bytes from `4C 8B DC 48 81 EC 98 00 00 00 4C 8B 41 38` to the call `E8 12 00 00 00` (partner_tackle.cpp) | direct |
| `0x1429934E0` the attack sphere query | `40 55 53 56 57 41 56 41 57 48 8D AC 24 C8 F4 FF FF 48 81 EC 38 0C 00 00` | direct |
| `0x1429938B2` its hit callback call (ctx, receiver, &DamageInfo, user; delivered when true) | `48 8B 45 88 48 85 C0 74 2A 4C 8B 4D 90 4C 8D 85 C0 00 00 00 48 8B 4C 24 78 48 8B D6 FF D0 84 C0 75 11` | direct |
| `0x14231881F` DamageInfo+0x80 the hit position | `C5 F8 10 42 08 C5 F8 11 81 80 00 00 00 C5 FB 10 4A 18 C5 FB 11 89 90 00 00 00` | direct |
| `0x14382FD30` the filter init | `48 89 5C 24 18 57 48 83 EC 20 44 89 41 08 48 8B DA 48 8B F9 48 85 D2 74 30` | direct |
| `0x1419D5870` the game's free | `48 83 EC 28 4C 8B C1 48 85 C9 0F 84 BB 00 00 00` | direct |
| `0x14271BC00` Sam's MsgDamage handler (detoured) | `48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8B DA 48 8B 89 20 05 00 00` | detour |
| `0x142314242` DamageInfo `+0x78` amount, `+0x28` attack reference | `C5 FB 11 43 70 C5 FA 11 73 78 48 89 AB 80 00 00 00 48 8D 4B 28` | detour |
| structural: the filter vtable `0x143BA8938` in the image, slot 0 in .text, and the RIP target of `0x142F8AC94` | | direct |
| structural: the attack-stop global from the RIP operand of `0x142F8AC1F`, in the image | | common |

Reused and checked by their own modules: the vehicle kinds, `+0x8B0` / `+0x4CC` / `+0x8A8` and the follower
(vehicle_drive), RequestTeleport (vehicle_share; the tests' placements), PlaceOn (game_actions; the tests' placements),
LocalCollisionBlock (game_actions), the capsule calls (partner_collision).

### 25.9 Hidden scenarios and evidence (2026-11-19, foreground, one at a time)

Every run: both games exit 0, real saves unchanged and restored, no violation, 0 cloud uploads, no `[vhit]` / `[vdrive]`
/ `[ride]` FAULT, no game over.

| Run | Result |
|---|---|
| `20261119-153001-two-games-vehicle-hit` | fail (scenario): the truck both ways hit (1000 -> 881, direct 0x157 at 33.9 / 35.3 km/h; seen by the health drop: the hook's match was wrong); the trike step came 3 s after the guest got off, still parented |
| `20261119-153401-two-games-vehicle-hit` | fail (scenario): the trike's own spot faces rocks (it climbed them); the message layout found (25.3) |
| `20261119-153742-two-games-vehicle-hit` | fail (scenario): `vehicle_sync 8 1.8 3` picks another trike than the one moved onto the road (the Kusanagi is kind 4 there) |
| `20261119-154034-two-games-vehicle-hit` | fail (scenario): the trike at rt=160 reached 17.8 km/h and braked 3.6 m short |
| `20261119-154317-two-games-vehicle-hit` | **pass**: truck host -> guest 28.2 km/h, guest -> host 28.6 km/h (0x157, 2 messages each), trike host -> guest 40.0 km/h (0x158, bracket, speed held) |
| `20261119-160320-two-games-vehicle-hit` | **pass** on the final build, the same numbers; the driver-side screenshots taken while braking (H1b-host-side: the guest's body lying in front of the truck) |
| `20261119-154830-two-games-vehicle-hit-fast` | fail (scenario regex); it showed a stale pass reporting a phantom hit after another vehicle's hit: the pass now ends after 1 s without calls |
| `20261119-155208-two-games-vehicle-hit-fast` | **pass**: truck 40.0 km/h and trike 40.0 km/h knocked down (1000 -> 881 -> 845.3), never killed; D3-3: crash exit dv 8.8 m/s -> the hit 150 ms later (1000 -> 881, "Thrown off in the crash.") |
| `20261119-155455-two-games-vehicle-hit-off` | **pass**: the guest (`partner_vehicle_damage=0`) not hit by the host's truck at ~28 km/h, health 1000, standing (O1b / O1c); the host (on) hit by the guest's truck (1000 -> 881) |
| `20261119-155657-two-games-vehicle-bump` | pass, but the truck stopped 11.7 m short of the trike (the scenario put it 18 m away); tuned |
| `20261119-155954-two-games-vehicle-bump` | **pass**: the ram, the head-on, the capsule rule (25.6) |
| `20261119-161016-two-games-vehicle-hit-fast` | **pass** on the final build: truck and trike at 40.0 km/h (1000 -> 881, 2 messages, 0 capped), D3-3 at 49 km/h (dv 14.2 m/s): 1000 -> 881 |
| `20261119-161256-two-games-vehicle-hit-off` | **pass** on the final build: the guest (off) not hit, the host (on) hit at 28.8 km/h |
| `20261119-164745-two-games-vehicle-hit` | **pass** (independent re-check): truck host -> guest 28.2 km/h, guest -> host 28.7 km/h, Kusanagi trike -> guest 38.4 km/h, each 1000 -> 881 (2 messages, 0 capped). H1a..H1d: the guest thrown, lying, "Seriously?", up; H1b-host-side: the guest's body lying in front of the truck; J1a..J1d the host the same; J1b-guest-side: the host's body lying in front of the truck; T1b / T1c the trike's hit. H1b also showed "Hold [X] / [F] to ride with the host" while the guest lay beside the truck: fixed (24.3) |
| `20261119-165049-two-games-vehicle-hit-fast` | **pass**: truck 40.0 km/h, trike 40.0 km/h, D3-3 dv 9.5 m/s at 10.7 m/s: each 1000 -> 881, none killed; F1a..F1d thrown / lying / up; F3a "Off the truck: crash.", F3b "Thrown off in the crash." with the popup; the hit threw the guest off the raised road's edge (F3c: up again on the ground under the road) |
| `20261119-165357-two-games-vehicle-hit-off` | **pass**: the guest (off) stood against the truck's front unhurt (O1a / O1b, health 1000); the host (on) knocked down at 28.5 km/h (O2a..O2c) |
| `20261119-165610-two-games-vehicle-bump` | **pass**: the push CLAIM granted, the head-on, the capsule rule ("a vehicle moves within 5 m of the partner"); life equal on both games at all 5 snapshots (truck 431.0, trike 4862.0) |
| `20261119-171723-two-games-vehicle-hit` | **pass** with the boarding gate (24.3): `[ride] our Sam was hurt (health 1000.0 -> 881.0): no boarding for 5 s` on each hit; H1b now shows "Hit by your partner's vehicle." instead of the ride prompt |
| `20261119-172247-two-games-vehicle-hit-roadster` | fail (the scenario): the roadster put 22 m behind the truck faced the road's raised edge and could not climb it (16 km/h, never near the guest); moved to 8 m ahead of the truck |
| `20261119-172519-two-games-vehicle-hit-roadster` | **pass**: roadster host -> guest and guest -> host at 34.9 km/h, the state-4 bracket with 0x158 (sphere 1.31 m at local (0, 1.73, 0.86)), each 1000 -> 881; G1a..G1d the guest thrown / lying / up, G1b-host-side the guest's body falling beside the roadster, G2a..G2d the host the same. A second pass 1.6 s later (the braking roadster still at the lying guest, 22.8 km/h) logged "hit Sam (#2)" with health 845.3 -> 845.3: the game's messages came, no damage, no second knock-down |
| `20261119-183039-two-games-vehicle-hit` | **pass** (review build): the truck both ways (0x157, 1000 -> 881); **the Kusanagi trike on the direct path** with 0x158 (sphere 0.91 m at local (0, 0.73, 0.73)) at 40.0 km/h: 1000 -> 881, 2 messages, 0 capped, T1b the guest lying |
| `20261119-183324-two-games-vehicle-hit-fast` | **pass** (review build): truck 40.0 km/h, trike 39.2 km/h on the direct path (amount 100: 0 capped), D3-3 dv 10.4 m/s: each 1000 -> 881 |
| `20261119-183604-two-games-vehicle-hit-off` | **pass** (review build) |
| `20261119-183801-two-games-vehicle-hit-roadster` | **pass** (review build): **the roadster on the direct path** with 0x158 both ways at 34.9 km/h, 1000 -> 881 (G2b: the host knocked down, "Hit by your partner's vehicle.") |
| `20261119-184015-two-games-vehicle-bump` | **pass** (review build): the capsule rule from the poll-timed speeds ("a vehicle moves within 5 m of the partner"), no enable while the trike passed; life equal |
| `20261119-161456-two-games-vehicle-bump` | **pass** on the final build: the push CLAIM granted; head-on: the host's follower truck 0.49 m / 3.1 deg, 3 corrections refused while the trike stood in its place ("its place is taken ...: waiting"), the one RequestTeleport (0.35 m) only once clear; life equal on both games at every snapshot (truck 431.0, trike 4862.0); the capsule rule on the host |

Scenarios: `two-games-vehicle-hit`, `two-games-vehicle-hit-fast`, `two-games-vehicle-hit-off`, `two-games-vehicle-bump`
(tools/testenv/scenarios). Debug action: `vehicle_hit` (docs/debug-actions.md "Vehicle hits").

### 25.10 Not done / not proven

- **The roadster** runs hidden in `two-games-vehicle-hit-roadster` (both ways, 34.9 km/h, the direct path); not at 40 km/h.
- **A second pass on a downed Sam**: the next pass (after the 1.5 s hold-off) can reach a victim still lying in front of
  the braking vehicle: the game sends its messages but takes no health and plays no second knock-down (run
  `20261119-172519` on the bracket; `20261119-183801` on the direct path: #2 / #3 at 22.8 / 15.3 km/h, 845.3 -> 845.3);
  the log counts it as a hit.
- **The bracket** is now only the fallback when the direct path's byte checks fail: there whatever the game's sphere
  catches is hit (an NPC or cargo on the ground right in front of the partner's trike in your world), so it is called only
  while your Sam's centre is inside the sphere. Every hidden run used the direct path.
- **Two messages per pass**: the second (35.7 damage) is the game's next-tick message queued before the first is handled
  (the proof's bracket did the same); the hold-off starts once the first is seen.
- **Above 40 km/h** a hit on foot was not run (the runs' full-throttle passes reached 40.0 km/h); the crash hit at 49 km/h
  was 100 like every other (the direct path never passes more).
- **Sam's ragdoll and vehicles**: once Sam is down, his ragdoll layers meet vehicles; the game's own physics, no damage
  was seen from it.
- **The partner capsule rule** was seen with the local Sam in a vehicle (the rule comes before that gate in the log
  order); with both on foot and an NPC truck passing, it is the same rule, not run.

## 26. Fragile's accessories stay on her while she moves; one offset per head item (v0.19)

**Status (2026-11-20): works in hidden runs, solo, echo and two games on the launcher's own Local test files.** The
user's v0.18.0 play test (Local test, guest Fragile in her porter suit, 30 fps): "the glasses seem fine but everything
including the glasses seem to still get detached from her when she moves even the cargo attach spots. the hats are still
a little high too."

### 26.1 Why her gear came off while moving (the guest's own screen)

- The self view draws Sam's own gear (cap, glasses, backpack, cargo, BB pod, hip cases, boots) on his hidden fitted
  skeleton (section 16) at **Sam's world transform of this frame**. The game updates the self body **before** Sam in
  most frames (section 8.2: `capture age ~1 frame` in every `[smooth]` line of the user's guest log), and in follow
  mode 1 its world transform was only set afterwards, in Sam's own update. Every meter (`body-Sam distance 0.0`,
  `Sam vs body 0.0/0.0`, `[headgear] ... sd 0.00`) reads the entity fields at tick time, after both updates, so none of
  them could see it.
- **What the game draws for the body is what it has in its own update** [V: pictures]. In run
  `20261120-024819-fragile-attach` (one game at 30 fps, running across the camera's view), with follow 1 (v0.18) the
  cap sits ahead of her head and the glasses float in front of her face in the running direction; with follow 2 (the
  copy inside the body's own update: still the previous frame's world) the same or further; with follow 4 (below) both
  sit on her. The same in two games on the launcher's own files (run `20261120-025353-two-games-fragile-attach`,
  `03-guest-self-run-f1-*` against `04-guest-self-run-f4-*`). If the renderer read the entity's world at the end of the
  frame, follow 1 would have looked attached; it did not. Where exactly the renderer latches a SkinnedModel's world was
  not found statically (`0x1423A49D0`, MsgEntityPostUpdate, refreshes per-part worlds right after the update; not
  proven to be the render path).
- Size of the gap: Sam's step in one frame. At the 30 fps cap the `[self] follow 4` lines measured 13 cm mean, 17-20 cm
  max running and 6 cm walking solo; 8-11 cm mean running in two games [V].
- **The host's screen** (Fragile as the partner body, props from the gear sender) was already right: the body's world is
  set from the tick and every prop is placed on that same world and the pose just written (`PlaceGearNow`, `PlaceGear`
  in the body's update). The echo partner in every `fragile-attach` picture (running, walking, turning, crouched) and
  the host's pictures in the two-game run show the cap and glasses on her head and the pod on her chest [V: pictures].
  Nothing changed there except the head items' offsets, which reach it through the sender (section 20.2).

### 26.2 Follow mode 4: Sam's world predicted in the body's own update (the default) [V: runs below]

- **Sam's update** (`TrackSam`, after the capture): his world (read fault-free twice, compared, finite) with a QPC stamp.
  The game updates his model about twice per frame; an update within half a frame of the last replaces the newest
  record, a later one starts a frame, so the track holds his world after his last update of this frame and of the
  frame before (v0.20: a Sam update after a body update starts a frame, and the track keeps a third record; 26.6). Then the mode 1 copy as before (it keeps the entity field exact for the tick, the gear sender and the
  slot-1 props, and corrects a wrong prediction within the frame).
- **The body's own update** (`PredictSelfWorld`, before the pose write; not in a forced update from the tick, not while
  the cargo menu's figure is the source): the same identity checks as the copy (entity vtable, model, back pointer),
  then:
  - Sam's newest record under half a frame old: he already updated this frame (`after Sam`): his world now, as is.
  - Otherwise his world now plus his last per-frame step: the position delta and the rotation delta (rows: r_cur =
    r_prev M, so M = r_prev^T r_cur), applied once. The step is not scaled by a dt ratio: his newest record is often
    the second, partial update of a frame, and scaling by its dt overshot by 30-60 % (first run
    `20261120-023358-fragile-attach`: step max 38.8 cm, the cap behind her head in one frame). (v0.20 scales by the
    SUM of his update dts in his last frame, which is 1.00 at an even pace: 26.6.)
  - No step (his world now: the plain mode 2 copy) without a previous frame, with records over 2.5 frames old (Sam not
    updated: asleep, a cutscene), or after a jump over 3 m in a frame (a teleport, a load, a vehicle snap).
  - No step either when something outside his own update moved him, because that move is already in his world now:
    `moved` when his world now is more than 5 mm from his newest record, `ride` while he is carried as a passenger
    (`ride::Carried`, published by the self tick). v0.19 also skipped it while he had a parent entity (`Sam+0x70`,
    driving in the game); v0.20 steps there too, with a consistency gate and a dt scale (26.6).
  - The setter is the same exported `Entity_ExportedSetWorldTransform` as in modes 1 and 2; a fault switches to mode 3
    for the run as before. **No new game call, no new game address, no hook.**
- The premise was measured: `Sam moved outside his update` (his world at the body's update against his newest record)
  0.00 cm in every line of every run [V].
- Log, every second while follow is 4: `[self] follow 4: predicted N, after Sam N, skipped N (no history, stale, jump,
  read, moved, ride), failed N, aba N; step cm mean / max; Sam's frame span ms; Sam moved outside his update cm`. At 30 fps solo:
  30-31 predictions/s, 3-14 `after Sam` (the body updates a second time in some frames), 0 failed, 0 aba, `stale 2`
  only around a mode switch; in two games 30-31 predicted, 0 after Sam [V].
- Switch: `self_view 1 0 <1|2|3|4>` (docs/debug-actions.md).

### 26.3 One offset per head item; the four "caps" are four different hats [V: runs below]

- `kHeadgearRows` now holds an offset per item (`realbody::kHeadItems` = 8, in the order of partner_gear's
  `kHeadItemRes`: eyewear0, glasses00-02, cap00-03; static_asserts tie the two together), `SelfHeadItemOffset(item)`
  takes the item, and the `[self] fit` line lists them (`cap a/b/c,...` for cap00-03, `glasses ...` for eyewear0 and
  glasses00-02).
- **The four items of the cap class are different hats**, picked by the accessory type, not by the cap colour (run
  `20261120-023729-fragile-caps`: cap00 for all 25 colours of `gear 15`; run `20261120-024053-fragile-caps2`, the
  meter naming the item): type 0x0F the Bridges cap (cap00), 0x24 the Santa hat (cap01), 0x31 the headcrab (cap02),
  0x32 a hair ornament (cap03). The user wore cap00, cap03 and then cap01 (the Santa hat) for most of the session; the
  v0.12 value was tuned on cap00 only. The `[headgear] self cap (<item>)` meter line now names the item.
- Chosen from 2560x1440 front and back close-ups at 5.0 / 4.3 / 3.6 cm up (run `20261120-024449-fragile-caps`), all
  1.2 cm back:

  | Item | Up | Why |
  | --- | --- | --- |
  | cap00 Bridges cap | 4.2 cm | 5.0 left a gap between the brim and her brow; 3.6 put the brim on her eyes; no hair through the band from behind at 4.3 / 3.6 |
  | cap01 Santa hat | 3.5 cm | at 5.0 the white band sat a forehead above her brows (the "a little high"); at 3.6 on her brow, and on her hair behind |
  | cap02 headcrab | 4.0 cm | between: it sits on her crown, the legs over her face as on Sam |
  | cap03 hair ornament | 3.8 cm | at 5.0 it floats above her hair at the back |
  | eyewear0, glasses00-02 | 1.1 cm (0.4 cm back) | unchanged (the user: the glasses look fine) |

- Both Fragile bodies (fragile, fragile_suit) share `fgl_head_def` and the row; only fragile_suit was looked at.
- A swap can put the new accessory at the freed address of the old one (same class, same size). partner_gear's slot is
  keyed on the address, so `NoteHeadItem` also compares the item: another item at the same address sets the slot up again
  (`[headgear] our <entity> is now cap01 (was cap00, same address): set up again`), our offset first taken out of the old
  mover if it still holds exactly our write. Without it the Santa hat would keep the Bridges cap's 4.2 cm.
- `self_view 8|9 <which> <mm>`: 0 every cap-class item, 1 every pair of glasses, 10-17 one item. `gear 14 <type> 1`
  puts on any of them (types above).

### 26.4 Evidence (hidden, `-SeedSaves complete-100`, all pass: game exit 0 (guest exit 0), saves unchanged, restored, no violation, 0 cloud uploads)

- `20261120-023358-fragile-attach` (first build, dt-scaled step): follow 1 and 2 detached, follow 4 attached running
  right; running left, one frame showed the cap behind her head (the overshoot, 26.2); fixed.
- `20261120-024819-fragile-attach` (final): `10-run-f1-*` cap and glasses ahead of her face; `11-run-f2-*` further
  ahead; `12-run-f4-*` and `13-run-f4-back-*` on her; `20-walk-f1-b` against `21-walk-f4-b`, `30-turn-f1-b` against
  `31-turn-f4-b`, `41-crouch-f1-*` against `42-crouch-f4-*` the same; the echo partner (the other screen's view) on her
  in every picture.
- `20261120-025048-fragile-caps`: each hat front and back, this build against v0.18's 5.0 cm; the meter reads 4.20 /
  3.50 / 3.96 / 3.88 cm up for cap00-03 (cap02 / cap03 within a few ticks of being put on).
- `20261120-025353-two-games-fragile-attach` (`launcherini local-test`: host_save=1, frame_cap=30, guest `fragile_suit`
  self view, exactly the launcher's files): the guest's own screen in follow 1 (`03-guest-self-run-f1-*`: cap and
  glasses ahead of her face) and 4 (`04-guest-self-run-f4-*`, `05-guest-self-walk`, `06-guest-self-turn`: on her); the
  host's screen: her cap, glasses and pod on her walking, turning, crouched, and the Santa hat (`05..08-host-sees-*`;
  720p, `graphics low`).

### 26.5 Not covered

- A one-frame prediction error at a sudden change: a start, a stop or an instant reversal leaves the body up to one
  step off for one frame (up to about 15-20 cm at a running reversal at 30 fps; a few cm at normal acceleration). Not
  seen in the turn pictures.
- Riding or driving with the self view: see 26.6 (v0.20).
- Tall cargo stacks and cargo carried in the hands: the runs used the save's own load (backpack, cases, BB pod).
- The renderer's latch point is shown by pictures, not found in the code.
- `self-headgear.txt` (section 20) still restores the v0.12 cap value (`self_view 8 0 50` now sets every cap-class item).

### 26.6 In a vehicle, and the on-foot residue (v0.20, play test 2 of v0.19.0) [V: runs below]

The user's play test 2 (Local test, guest Fragile, 30 fps): "with fragile accessories its certainly way better but i still
notice it twitching a bit when she moves and as far as her sitting in vehicle pose that is all out of wack she is all the
way back when the vehicle starts moving and her accessories seem to stay in the right spot". Their guest log: `ride` in
7104 of about 14800 body updates (half the session driving), and once `step cm max 87.6` on foot (a seat snap stepped
again).

- **Cause (seated)** [V]: v0.19 skipped the step while Sam had a parent (`ride`), on the assumption that the vehicle moves
  him outside his update. It does not: his world at the body's update equals his newest record (`Sam moved outside his
  update cm 0.00` in every seated second). So in a vehicle the body showed his world of the previous frame: **20-40 cm
  behind his gear at 11 m/s** (more with speed: v·dt), the "all the way back when the vehicle starts moving". The gear
  hangs on Sam, so it was right. The seated pose itself is Sam's (the same 77-joint fit as on foot), one frame old as on
  foot; the "out of whack" was the trailing (pictures below).
- **Fix** (`native/mod/self_predict.hpp`, pure and unit-tested in `tests/self_predict_tests.cpp`; `real_body.cpp`
  `TrackSam` / `PredictSelfWorld`):
  - the step also while seated (only carried keeps the plain copy, see below);
  - a **consistency gate**: Sam's newest per-frame step may differ from the one before by at most
    max(25 cm, 0.75 x the one before), else the plain copy (`gate`). A seat snap (85-137 cm in one frame getting on or
    off, a teleport) is not repeated; a vehicle accelerating at 5 m/s² changes its step by 0.6 cm a frame; running starts,
    turns and reversals stay under it;
  - the step **scaled by this frame's dt** (the body update's `dt`) over the sum of the dts of Sam's updates in his last
    frame, clamped 0.5-2 (1.00 at the 30 fps cap; it halves the error at an uneven pace: research run, one game at about
    40 fps, 2.1 -> 1.1 cm mean);
  - Sam's frames told apart by the **body-update counter** (a Sam update after a body update starts his next frame)
    instead of "half a frame since the last" (which disagreed with the body's updates in a quarter of the frames at an
    uneven pace).
  - No new game address, call or hook: the parent read (`Sam+0x70`) and the exported setter were already in use.
- **Carried (the passenger)** [V]: the carry teleports Sam from the game tick **after** his own update, to the seat one
  frame ahead (for the next frame). The meter against his world right after his own update: **0.0 / 0.0 cm** (the plain
  copy is exact: the body, his update and so his gear agree); the carry's move after it 11.6 cm mean at 2-10 m/s (the
  next frame's placement). So carried keeps the copy. The meter's main reference (his world at the body's next update)
  reads that next-frame move for carried, so the carried line also prints `vs his own update's world`.
- **The host's view of the guest driving** (`vdrive::SeatTarget`) [V]: the partner body led the vehicle by
  `min(50 ms, velocity age + 1/60 s)`; it runs before `vdrive::Tick` measures the velocity, so the age is 0 and the lead
  was 16.7 ms against a 33 ms frame: **the body half a frame behind the seat** (18.6 cm at 11 m/s in the research run).
  Now the lead is the tick interval (EMA, `g_tick_ms`), as the ride's passenger and carry already did. A yaw-rate lead on
  top was measured too and made no difference (dropped). The remaining spikes are the follower's own corrections.
- **On foot** [V]: at the 30 fps cap the v0.19 step was already 1.1 cm mean (the dt scale is 1.00 there, the gate never
  fires running): the residue is single frames at stops, starts and sharp turns (max about 10 cm, 1 % of moving frames
  over 8 cm). What v0.20 removes on foot is the **seat snap stepped again** after getting off (the user's
  `step cm max 87.6`; v0.19 had no gate): one frame of the body 0.9-1.4 m off.
- **Meter** (every second, guest, per class with moving frames): `[self] follow 4 meter <on foot|seated|carried>, N moving
  frames, body vs Sam after the frame: v0.20 mean/max cm (>3 cm N, >8 cm N) mean/max deg; v0.19 ...; copy ...` (carried:
  `| vs his own update's world: v0.20 mean/max cm; the carry moved him after it mean/max cm`): every rule scored on the
  same frames against Sam's world at the body's next update (where the game drew his gear). The `[self] follow 4 (rule
  v0.20): predicted N (seated N), ... gate N ..., scale k` line has the counters. `[self] Sam in a vehicle (parent ..., #N)`
  / `[self] Sam out of the vehicle (#N)` on every change of his parent. Host: `[vdrive] partner body vs the seat after the
  frame (driving, moving): lead ms mean/max cm; v0.19's lead mean/max cm`.
- Switch: `self_view 13 <0|1>` (0 applies v0.19's rule, for the "before" pictures; 1 the default).

Numbers (hidden, two games on the launcher's own Local test files, 30 fps; mean / max cm, moving frames):

| What | v0.19 | v0.20 | Run |
| --- | --- | --- | --- |
| guest drives the truck from a stop to about 11 m/s, straight (205 frames) | 22.4 / 37.2 (87 % > 8 cm) | **0.7 / 4.1** (0 % > 8 cm) | `20261120-110837-two-games-self-vehicle` |
| the same with a turn at speed (208 frames) | 18.2 / 44.7 | **1.6 / 18.9** | same |
| guest drives the Kusanagi trike from a stop (142 + 210 frames) | 19.5 / 39.2 and 13.0 / 47.9 | **2.5 / 39.2 and 1.6 / 47.6** (the max: the launch frame itself, the same for every rule) | `20261120-112205-two-games-self-trike` |
| the trike in the first run (158 frames) | 17.9 / 41.9 | **2.3 / 40.3** | `20261120-110837` |
| getting on (the 85.3 cm seat snap) | the next frame copied (seated) | the snap frame itself, then `gate` 2 | both |
| guest on foot: run, stop, run, reverse, diagonal (235 frames) | 1.2 / 9.9 | 1.1 / 9.9 (copy: 7.8 / 16.0) | `20261120-110837` |
| guest carried in the host's truck / on the trike | copy, 0.0 / 0.0 against his own update | the same (kept) | `110837`, `112205` |
| host sees the guest drive the truck, straight, 3-11 m/s | 3.4-19.1 mean per second | **0.4-3.9** | `20261120-110129-two-games-self-vehicle` |
| host sees the guest drive the trike | 8.6-17.3 mean per second | **2.4-7.8** | `20261120-112205` |

Pictures (guest's own screen, side view, the trike from a stop; enlarged crops in the play-test notes): `D1c-start-v019`
(v0.19): her BB pod floats in front of her chest and the cap ahead of her head (the gear a frame ahead of her body);
`D2c-start-v020` / `D2d-moving-v020`: the pod on her chest, the backpack on her back, her hands on the bars. The truck's
cab hides the driver from outside (no usable picture).

Not covered: the dt scale at an uneven pace in two games (proven only in the one-game research run); the pillion seen
from the side at speed (the host's trike crashed in the first try); a stop on foot still overshoots for one frame (it is
seen only after it happens); her shorter limbs on Sam's seated rotations (hands and feet a few cm off the bars / pedals)
not measured; the fit writes the hand attach helpers (MTP_R/L_Hand_a/b) while seated, nothing hung on them seen.
