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
- **Transform (follow mode 1, the default):** Sam's own model update (the detour's Sam branch) reads his
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

`'STV1' | u8 version 1 | u8 item | u16 layout hash | u8 kind | u8 res | u8 nvar | u8 nsv | u64 parts apply | u64
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
| Header | `'STA1' \| u8 version 1 \| u8 n \| u16 seq \| u32 sender_ms` |
| Item (24 bytes) | `u32 a \| u32 b \| u8 op \| u8 slot \| u8 tag \| u8 nrtpc \| u16 age_ms \| u16 fade_ms \| i16 off[3] \| u8 category \| u8 0` |
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
- The decoder refuses another magic or version, n = 0 or > 32, a size that does not match the items exactly or is over
  512 bytes, an unknown op, a slot / action out of range, nrtpc > 12, ids an op needs that are 0, fields an op does
  not use that are not 0, age > 2,000 ms, fade > 10,000 ms, category > 4, a reserved byte not 0, an RTPC id 0, and a
  non-finite or huge (|v| > 1e7) value; a refused frame leaves the output untouched (`TestSoundFrames`). STP1, STJ1,
  STN1, STS1 and the gear decoders refuse it, and it refuses theirs (tested).
- **Delivery.** A FRAME message in the pose datagram (STP1 + STJ1 + STA1 + STN1); when it does not fit there it goes
  in a datagram of its own right after. Unreliable (a lost footstep is better than a late one). *(changed 2026-11-18)*
  The link drops duplicates and frames more than 64 seqs behind the newest (a bitmap, u16 wrap-safe) but keeps a
  reordered frame (its stops matter); the receiver counts gaps as lost and takes a late frame back off that count.
  Stops are sent twice more in the next frames (idempotent on the receiver). The sender checks every item alone
  (`wire::SoundItemValid`, the decoder's own rules) and drops an invalid one, not the frame. `link::Info` counts STA1
  frames sent (and those that went alone) and accepted. The format did not change.
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
  `fgl_head_def`): **cap (0, -1.2 forward, 5.0 up) cm, glasses / eyewear (0, -0.4, 1.1) cm**, chosen from 2560x1440
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

## 22. Vehicles: the partner's vehicle under its body (`session.ini vehicle_sync`, default 1: the roadster)

**Status (2026-11-18): first milestone, works in hidden runs (one game and two games, both directions).** When a player
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
- The parked roadster is **not** in the World's entity list (82 entities; `action vehicle_sync 7`) [V].

### 22.2 Sender (game thread, the pose send tick)

`vsync::TakeSend`: `[Sam + 0x70]` checked against the three vehicle vtables (else its parent's parent, for a seat child;
anything else is logged once by type and not sent). Read fault-free: the kind (`+0x4C4`, must match the class and the
resource), the ride state (`+0x4CC`, 0..15), the resource GUID (must be on the allow-list below), the world transform
(the game's getter), Sam relative to it (the seat, within +-4 m) and its local pose (`look::ReadPose`: read twice, equal
reads only; the layout must be the resource's). After Sam gets off the same vehicle is still sent, as parked (seat 0),
while its vtable and per-creation GUID still check, Sam is within 30 m of it and for at most 60 s. One STM1 frame per
send tick (30 Hz) in a datagram of its own, on the pose's `sender_ms` axis.

### 22.3 Wire: STM1 (`pose_wire`, ctest `st_net_tests` `TestVehicleFrames`)

| Part | Contents |
|---|---|
| Header (84 bytes) | `'STM1' \| u8 version 1 \| u8 kind (1 roadster, 2 truck, 3 trike) \| u8 ride state \| u8 flags (bit 0 seated, bit 1 joints) \| u32 seq \| u32 sender_ms \| 16-byte resource GUID \| f64 x, y, z \| 6-byte rotation \| f32 speed \| i16 seat x, y, z (1 mm) \| 6-byte seat rotation \| u32 skeleton layout hash \| u8 n \| u8 0` |
| Joints (flags bit 1) | n x {6-byte rotation, i16 x, y, z (1 mm)}, n <= 64 |

Rotations are STJ1's smallest-three. The frame is 360 bytes with the roadster's 23 joints: while one is reported the
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
  to 80 s (review follow-up: the count was reset before, so it stayed 5 s). `vehicle_sync=2` shows the solid copies
  only, `1` (default) the roadster's only, `3` all; the override `vehicle_sync 2 <n>` ignores the mode. A reported
  vehicle without a copy is logged once per reason (`the partner's <copy>: no copy (...)`).
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
  while a trace runs only. Local vehicles are never read or touched
  on the receiving side.
- **Interplay.** Player collision (section 11.2) already takes the partner's capsule out while it rides
  (`kPoseInVehicle`). The copies have no physics: a player can walk through the partner's vehicle. Name tag and gear
  ride the body as before.

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

### 22.6 Not covered / still different

- **Only the roadster was driven.** The trike and truck copies were spawned, validated, placed and shown (override),
  never with a real trike or truck sending: their origin against the drivable vehicle's, their joint poses (the same
  Skeleton, so expected to fit) and the Kusanagi trike (`bkyk`, its own skeleton: no pose) are not proven. The garage
  terminal's vehicle menu was not scouted.
- **The roadster is a hologram on the other screen** (no solid cutscene roadster exists; a forged entity resource or a
  copy of the real `DSVehicleSpeedCar` was not attempted: the latter is the pattern that crashed with weapons, 17.1).
- **Paint and variations** (purple, gold / silver, broken) are not mirrored; the hologram has its own look.
- **Wheel motion** arrives at 30 Hz and is given every tick; the copy is not moved to the 240 Hz update group, so at
  speed its wheels may step (the read-back meter shows up to 45 deg against the given poses while they spin).
- **Cargo on the vehicle, sounds (engine, RTPCs), lights, damage effects** are not mirrored. The partner body's hands
  and feet are Sam's seated pose: on Fragile they may not meet the wheel exactly.
- **Not run**: link loss or a load while driving (the code removes the copy), long drives outside the garage, two
  vehicles at once, carts and floating carriers (logged once as another parent type, not sent), a vehicle destroyed while
  ridden.
- **No collision** with the partner's vehicle copy.
- **Two of the same vehicle.** In the two-game runs both games come from one save, so the watcher's own roadster is
  parked where the partner's copy appears (`121400`: the hologram inside the guest's own purple roadster). In play a
  partner's copy will often overlap or duplicate the local player's own vehicle of the same kind.
- **Parked for 60 s at most.** A vehicle left by its driver stops being reported after 60 s even while the driver stands
  beside it (or once they are 30 m away): the copy goes then.
- The echo screenshots `04-mounted-copy` / `07-look-b` (`e04_copy.png`, `e07b.png`) are from `120658`, an older
  build than the final runs.
- The pose targets share one fault switch: a fault writing the copy's pose turns off every gear pose (the backpack's,
  the boots prop's) for the session, as a fault on any of them already did. Kept on purpose (a fault means our write
  hit something unexpected); never seen in a run.
