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
- It rides in the same datagram as STP1. STP1 is unchanged. Mixed mod versions never get this far: the
  mod's link runs in play mode, where the HELLO is rejected when the builds differ (`[link] rejected:
  build_mismatch`, the launcher's "different mod version"), so neither player sees a partner. A peer
  that sends no STJ1 is therefore always a same-build game (see "Fallback" in section 5).
- A frame counts only when the body's ring accepts it (`realbody::PushFrame` returns true: 77 joints,
  layout `BE39C83F`, newer than the last). A frame it drops does not set the peer's character, does not
  count as "the peer sends joints" for the fallback, and the first 5 drops are logged
  (`[sync] joint frame seq N dropped: ...`). [I: no run has sent such a frame.]
- `link.cpp` gives it its own queue, its own newer-seq filter and the same generation counter
  (critique B3).

**Echo.** Sam's captured pose goes through encode and decode every send tick (30 Hz). It is pushed into a
256-entry ring keyed on the transform ring's unwrapped sender time. It is sampled at the same play time as
the transform (`Ring::PlayTime`) and nlerped per joint, with the hemisphere fix.

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
3. **Gear.** Backpack, cargo and Odradek could be the game's own prop entities parented to the body. Not
   researched.
4. **Faces.** Not researched: drive DSFacialRig on Sam's body, or leave the faces static.
5. **Cutscenes and photo mode.**
   - Cutscenes already remove the body (Sam asleep).
   - Photo mode calls the forced update with dt 0; the detour handles it, but it was not tested.
   - Whether a `demo_*` entity we spawned can be bound by a running sequence that uses the same body is
     unknown. Checking for an active sequence before spawning would be a cheap guard.
6. **Collision and physics** of `demo_*` bodies against Sam and vehicles (`CollisionRadius 1.0`): not
   confirmed. Possible collision push seen in run 20261116-193151 (`06-host-sees-guest-walk-a..c`, `07`:
   the host's Sam and cargo case suddenly fill the left of the frame as the Fragile body runs at him; in
   `06-d` he is not in view and Fragile runs past on the left) [I]; not confirmed. Logging both positions at those frames
   (`F6` / `log_status`) in `two-games-real` would confirm or rule it out.
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
  - **The body animates at 60 Hz, Sam at the frame rate.** The game updates the body's model 60 times a
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
- The game updates the moved body by itself at 60 Hz (`game-drove`, `forced=0`); while it is still
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

### 8.4 Gear (decision)

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
- **Cap and glasses.** They are separate accessory entities. Their colours are sent but not used.
- **Exoskeleton and power-glove meshes.** Only their texture tints come with the exos variation.
- **Backpack and cargo.**
- **A receiver not in the porter suit.** It cannot take the exos resource, so the remote player's
  silver or gold suit and tints are not shown. The colour still is.
- **The uniform menu.** The runs changed the outfit through the inventory byte (`real_outfit 4`, hidden
  tests only), not through the game's uniform menu. The game re-dressed its own Sam from that byte at
  once (run `075230`), so the menu path uses the same data [I].
- **Not tested:** two PCs; a variant change by either player while the body is dressed (the exos
  resource belongs to our own porter variant; the +1 reference keeps the object, but whether its
  streamed data stay loaded is unproven); a self-view body that is Sam (not needed: the self view is
  only for other characters).

