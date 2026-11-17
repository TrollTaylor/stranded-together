# Real partner body: a solid character that plays the other player's real animation

**Status (2026-11-16): prototype works in hidden echo runs.** It is behind `session.ini partner_body=real`
(the default is still the hologram). A solid cutscene-actor body loads by path and GUID in 0.02-3 s. A
post-hook on the game's SkinnedModel animate function then writes Sam's real skeleton pose onto it. In
echo mode the body replays Sam's walk and jog 2 s later, trailing him. This was checked with Sam (porter
suit) and with Fragile. In mirror mode Sam's live pose was shown on the porter, Fragile, Die-Hardman and
Cliff bodies too. Network play (host and guest) has the wire format and the link code, but it has not
been run yet.

Code:
- `native/mod/real_body.{hpp,cpp}`: spawn, hook, pose capture and apply, the pose ring, debug commands.
- `native/mod/body_table.{hpp,cpp}`: the character list.
- `native/mod/pose_wire.{hpp,cpp}`: the STJ1 joint frame.
- `native/mod/player_sync.cpp`: the `real` body in the body chain, the echo and net joint stream.
- `native/mod/link.cpp`: STJ1 next to STP1.
- `native/mod/partner_holo.cpp`: `SpawnBeginField`.

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
| `20261116-184706-echo-real-fragile-100` | **Echo with Fragile.** `04-walked.png`: Fragile (her jacket's back logo is visible) walks behind Sam. `06-trailing-f/g.png`: she jogs towards the camera behind him. |

A composite of these frames is in the session scratchpad (`real_body_evidence.jpg`).

What is not right yet:
- The bodies have no backpack, cargo or Odradek.
- The faces do not move: no facial rig is driven.
- A female body plays Sam's male motion.
- Collision with Sam was not tested.

Hook counters in every run were `rej=0 torn=0 aba=0 wpm=0 faults=0` ([pose] lines). The check of the
partner's committed pose against the published pose is 0.00° while standing still. While moving it is up
to 4-10° max: the publisher runs at the tick rate (about 125 Hz) and the game updates the body at 60 Hz,
so the comparison is against a slightly newer frame.

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
| 6-16 | others | porter_plain, deadman, heartman, mama, lockne, amelie, higgs, guard, medic, sam_mule, fragile_suit (loading not tried in game yet) |

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

## 4. Wire format and interpolation (`pose_wire`, ctest `st_net_tests`)

**STJ1 frame.** It is 497 bytes for 77 joints:

| Part | Contents |
|---|---|
| Header | `'STJ1' \| u32 seq \| u32 sender_ms \| u32 layout_hash \| u8 joints \| u8 body_id \| u8 flags \| u8 0` |
| Rotations | 77 × 48-bit smallest-three |
| Translations | `u8 n` + n × `{u8 joint, i16 xyz}`, 0.1 mm units (root and pelvis) |

- Round-trip error is below 0.02° per joint (tested).
- It rides in the same datagram as STP1. STP1 is unchanged, and older peers' `DecodePose` rejects the
  second message.
- `link.cpp` gives it its own queue, its own newer-seq filter and the same generation counter
  (critique B3).

**Echo.** Sam's captured pose goes through encode and decode every send tick (30 Hz). It is pushed into a
256-entry ring keyed on the transform ring's unwrapped sender time. It is sampled at the same play time as
the transform (`Ring::PlayTime`) and nlerped per joint, with the hemisphere fix.

**Network (host and guest).** The code is written and compiles; it has not been run:
- `SendPoseAndJoints` sends the pose and the joints.
- `PopJoints` → `realbody::PushFrame`, keyed the same way.
- The guest shows `partner_character` (config) and logs the peer's `body_id`.

## 5. Remaining work to productize

1. **Two-instance test** (`two-games` scenario with `ini partner_body=real` on both games). It proves STJ1
   over UDP loopback and the interpolation with real jitter. Check the bandwidth: about 560 B per datagram
   at 30 Hz.
2. **Character choice.**
   - Launcher: a "Your character" list on Host and Join, written to `session.ini partner_character` for the
     peer's look (or `my_character`, sent as `body_id`).
   - The receiver should show the peer's `body_id`, with a stable-for-N-frames rule before a respawn,
     instead of its own config.
   - Only offer the ids proven in game: 1-5 so far. The others in `body_table` need one tour run each.
3. **Fallback when the peer sends no STJ1** (v0.3 peer, or its capture is unavailable). Done in review,
   not yet run: when transforms arrive (< 1 s old) but the body has had no joint frame for 3 s since it
   went live, `player_sync` falls back to the next body in the chain (the hologram).
4. **Gear.** Backpack, cargo and Odradek could be the game's own prop entities parented to the body. Not
   researched.
5. **Faces.** Not researched: drive DSFacialRig on Sam's body, or leave the faces static.
6. **Cutscenes and photo mode.**
   - Cutscenes already remove the body (Sam asleep).
   - Photo mode calls the forced update with dt 0; the detour handles it, but it was not tested.
   - Whether a `demo_*` entity we spawned can be bound by a running sequence that uses the same body is
     unknown. Checking for an active sequence before spawning would be a cheap guard.
7. **Collision and physics** of `demo_*` bodies against Sam and vehicles (`CollisionRadius 1.0`): not
   observed yet.
8. **Release switch.** When 1-3 pass, make `partner_body=real` the launcher default and keep `holo` as the
   fallback.

## 6. Debug actions and scenarios

See [debug-actions.md](debug-actions.md) "Real partner body". Scenarios:
- `real-body-diag`: `real_control` (a collectors hologram through the same path+GUID route), Sam, porter.
- `real-body-tour`: probe, Sam own pose → mirror → walk frames, then porter, Fragile, Die-Hardman, Cliff.
- `echo-real-100` and `echo-real-fragile-100`: the echo partner as a real body. They use the new
  `ini key=value` and `pad ... keep` scenario features (docs/hidden-testing.md).
