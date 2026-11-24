# Enemy sync (P0 done: the census; round 15: R1 foundations, the P1-0 freeze probe, ship A: the players' shots)

Co-op in Death Stranding means both players see the same MULEs, Demens, warriors and BTs, and what one player does to them
the other sees. Today each game runs its own enemies: the guest's world is a copy of the host's save (the host-held save
session, [coop-session.md](coop-session.md)), so the same camps exist in both, but their AI, their alert, their hits and their
deaths are separate. This document is the plan for changing that, phase by phase, and the record of what each phase proved.

Status: **P0** (the census) is implemented and run (section 3): both worlds have the same 150 MULEs with the same GUIDs and
actor ids, placed identically, but their patrols run out of step and their AIs drift apart. **Round 15** (section 7) built the
foundations of the plan's R1 (the shared Sam damage detour, the bullet Enqueue detour, link channel 6 "combat" with the pack
order, the SEH1 beacon and the combat frame queues), ran the two probes (P3-0 shots, P1-0 freeze) and built **A: the players'
own shots** on the other screen (`shots_sync`; default **off** since the round 15 review: its two-game gate run failed its
friendly-fire step, section 7.3). Enemies themselves are not mirrored yet (P1a is next).

Sources: the round-10 research (Ghidra on ds.exe Director's Cut, PE timestamp 1706443442), the MGSV mod's enemy-sync history
(`ref-ddt/docs/world-sync-design.md` 4.x and 7.8-7.33), and this mod's own vehicles (section 23 of
[real-partner-body.md](real-partner-body.md)).

## 1. What decides the design

1. **The game's AI only perceives the local player.** A MULE's targeting, the odradek marking and a gazer's noticing all read
   the local Sam. The MGSV mod spent five rounds (7-11) trying to make guards see the partner body (per-soldier perception,
   told sight) and never got it reliable. So an enemy fights the player of the world that simulates it.
2. **Two AIs on one enemy drift apart.** In MGSV the two copies of a guard were 8.7 m apart at the median and up to 95 m,
   and corrections fought the local AI. So exactly one world simulates each enemy; the other world's copy must not think.
3. **This mod already solved the same problem for vehicles**: one real entity per world, bound by identity (game-actor id plus
   placement GUID), an owner that simulates, a follower driven by the owner's stream, the host winning ties. Mean follower
   error 0.05 m. Enemy sync copies that model.

## 2. What the game provides

### 2.1 Identity and lookup

- **Game actor registry** `[0x147BBB8C0]`: +0x440 group count, +0x448 groups, +0x4B0 the type limit. A group: vt+0x38 its actor
  type, +0x30 first slot, +0x40 count, +0x48 the actor array. An actor id is `type << 13 | slot` (`0x1FFFFF`,
  `[0x144AF1150]`, = none). `0x1422FA980(reg, id)` returns the actor (null unless +0x20 "in use");
  `0x1422FB870(reg, id, msg, immediate)` posts a message to it (RemoveAllMules does this for every member).
- **An actor** (the same head as the vehicle units of section 23): +0x08 id, +0x20 in use, +0x24 state (2 = its entity is in
  the world), +0x28 its own world transform (vfunc +0x18 is `lea rax, [rcx + 0x28]`, `0x141A87230`), +0x68 entity ref
  (entity = ref - 0x20), +0x8D the low-LOD byte (ForceLowLodNpc).
- **MULEs, Demens and warriors** are actor type 1 (`id & 0x1FE000 == 0x2000`, SetSequenceOnlyMule `0x142B2FA63`). Their class
  is `DSMuleGameActor` (size 0x10C0): +0x110 / +0x114 the alert kept while not in the world (MsgDSSetKnowledge `0x142AE72E0`
  writes 3 or 5 and 100.0, and skips an actor in the world), +0x124 bit 12 sequence-only, +0xEA4 bits 13 / 14 a sequence
  requires / occupies it, +0xEA8 bit 12.
- **NPC manager** `[0x147BF29B8]`: maps a 16-byte GUID from the level data to an NPC record (`0x142B7F880`). The main map at
  +0x78 (0x18-byte slots: u32 hash = crc32c | 0x80000000, GUID at +4, kind at +0x14; capacity at +0x84) gives the kind; kinds
  0..5 live in per-kind managers `[mgr + 8 + kind * 8]` whose map is at +0x18, kind 6 in the map at +0x48 (0x20-byte slots:
  hash, GUID at +8, the record at +0x18). A record: +0x00 kind, +0x08 an entity ref (records without an actor), +0x20 the
  actor id. A second map (+0x68, 32-bit keys) serves the scripts' short ids (`0x142B7F9D0`).
- **MULE manager** `[0x147BF09D8]`: +0x30 / +0x38 its members (member +0x20 = actor id; 150 in the 100 % save), +0x1A0 /
  +0x1A8 / +0x1AC the killed records (0x28 each), cleared by the resurrect body `0x142B168F0`.
- **From an entity**: DSMuleComponent (+0x48 entity, +0x58 its DSMuleGameActor, set by MsgSetGameActorId `0x142B08040`).
- **Dead**: entity +0x88 bit 7 (Entity_ExportedIsDead `0x142362C80`).

### 2.2 Script exports

| Export | Address | Effect |
|---|---|---|
| SetSequenceOnlyMule(guid) | `0x142B2FA40` | actor +0x124 \|= 0x1000, +0xEA8 \|= 0x1000, vt+0x48(1), vt+0x50(1); one-way |
| SetNpcIndividualPhase(guid, b) | `0x142B9FCB0` | posts MsgDsAiAction to the actor (or to the record's entity) |
| ForceLowLodNpc(guid, b) | `0x142BF7850` | actor +0x8D = b |
| RemoveNpcFromWorld | `0x142BF78A0` | |
| IsGazerNoticingPlayer / EnableRespawnGazers / ResetAllGazers | `0x1429A7210` / `0x1429A71D0` / `0x1429A7220` | |
| GetActiveCatcherHealth | `0x142A910A0` | |
| CreateBullet | `0x14294AD40` -> `0x14294D340` | origin, target point, target actor id, u16 attack id, speed, flags |

### 2.3 Freezing a copy, damage, bullets, BTs

- **Freeze lever (unproven)**: MsgGameActorSequenceOccupied (vtable `0x143CFC0B8`, +0x10 occupied) is the engine's own "a
  sequence owns this actor". The MULE actor's handler `0x142AC3090` with occupied = 1 sets +0xEA4 bits 13 / 14 and turns
  the AI off through vt+0x50(0) / vt+0x48(0); occupied = 0 restores. DSMuleComponent `0x142B07F40` and DSNpcWeaponComponent
  handle the same message.
- **Damage**: MsgDamage (vtable `0x143D07E38`, 0x110 bytes, msg+0x20 the DamageInfo: +0x28 attack ref, +0x78 amount, +0x80 hit
  point). DSMuleDamageComponent's handler `0x142B020F0` resolves the attack (`0x14294DD50`: +0x0C u16 attack id, +0x18 the
  attack resource) and queues 0x90-byte entries at component+0x50. Health and stun are not located yet. An attack context:
  `0x14298AC90(owner entity or 0, u16 attack id)`; delivery to exactly one receiver: the query `0x1429934E0` that
  `partner_tackle.cpp` already uses. Attack ids: EDSAttackId 0..0x6F (BolaGun 0x1E, EnemyAssaultRifle 0x56,
  DemensAssaultRifleBullet 0x6A) and the extended list 0x71..0x18C (PlayerComboA_0..2 0x117..0x119, MuleKick 0x16F,
  MuleTackle 0x171, GazerDrag 0x12D, VehicleTackle 0x157).
- **Outcomes**: EDSNpcDefeatType {0 Dead, 1 Faint, 2 Sleep, 3 Restrained}, EDSMulePhase {0 Normal, 1 Return, 2 PreCaution,
  3 Caution}, EDSMuleStance {Sneak, Caution, Alert}, EDSMuleType {Normal, Friendly, Demens, Warriors_Particle}.
- **Bullets**: one enqueue for every bullet, `0x142916860(DSBulletSystem [0x147BEB108], rec 0xF0)` (at most 16 a frame, 5
  callers including CreateBullet). Sticky bullets, the bola rope and thrown items are entities of their own
  (DSStickyBulletEntity, DSBolagunRopeEntity, DSThrowable).
- **BTs**: the gazer manager `[0x147BEBEC0]`, DSGazerGameActor, DSHunterMember / CrowdComponent, the DSCatcher* components,
  DSTarMeshOnTerrainComponent. Their skeletons are not Sam's 77 joints (a gazer has 81).

## 3. P0: the census (done)

### 3.1 What it does

`action enemy_census <tag> [radius_m] [flags]` (`native/mod/enemy_census.{hpp,cpp}`, [debug-actions.md](debug-actions.md)
"Enemy census") walks the registry and the NPC manager's GUID maps and logs, for every type-1 actor: id, class, state,
GUID, NPC kind, place (its own transform, else its entity's), heading, distance to our Sam, entity, dead flag, low-LOD byte,
and the DSMuleGameActor fields. A summary carries two fingerprints that are the same on both games for the same set: `ids`
(FNV-1a over the sorted actor ids) and `guids` (over the sorted GUIDs). It reads the registry, the NPC maps and the actors
with ReadProcessMemory only and writes nothing; the only game functions it calls are the mod's usual guarded getters for
our own Sam (GetLocalPlayerEntity, EntityGetWorldTransform) for the distances. The pure part (ids, the hash-map slots, the GUID text, the fingerprint, and `Compare`: two censuses matched by
GUID with a position guard, the identity rule P1 will use) is tested in `st_enemy_census_tests`.

### 3.2 Byte checks

18 code sites and 4 RIP globals, all read in Ghidra, checked on first use: the registry lookup `0x1422FA980` (two parts), a
caller's registry global and actor +0x68 `0x1422D6268`, ForceLowLodNpc `0x142BF7850` (the NPC manager global, record +0x20,
actor +0x8D), the NPC GUID lookup `0x142B7F8B4` / `0x142B7F8FE`, the two map finds `0x141B03880` / `0x141B03904` /
`0x141B039A0`, SetSequenceOnlyMule `0x142B2FA58`, MsgDSSetKnowledge `0x142AE72E0`, IsDead `0x142362C80`, the actor transform
getter `0x141A87230`, the MULE manager's members `0x142B140DE` / `0x142B140FA` / `0x142B14140`, its killed records
`0x142B16932` and global `0x142B64420`. A core mismatch (registry, NPC maps) turns the census off with one line
(`[enemy] census off: ...`); any other mismatch drops only its fields.

### 3.3 The T0 run

`tools/testenv/scenarios/two-games-enemy-census.txt`: two games on the launcher's own Local test files (host_save), both Sams
at the MULE Camp Southwest of Lake Knot City (the guest 4 m beside the host), the census on both games at the same moment:
tag 1 at +0 s, tag 2 at +30 s; then both games resurrect every MULE in their own world (`dev_enemies 1`) and census again: tag
3 at +0 s, tag 4 at +30 s.

### 3.4 Results

Run `build-testenv\runs\20261123-110115-two-games-enemy-census` (pass; saves unchanged, restored, no violation, 0 cloud
uploads, 0 game-server writes, online gate ok on both games). Both games: byte checks 22/22, each census 15-64 ms. The
comparison was made from the two logs with a script (the `[enemy] tag=N eNNN` lines, matched by GUID).

**Identity: the same in both worlds.**

| | host | guest |
|---|---|---|
| actor registry | 18 groups, 2918 actors in use | 18 groups, 2941 |
| type-1 actors (all `DSMuleGameActor`) | 150 | 150 |
| with a GUID in the NPC map | 150 | 150 |
| NPC map | 173 GUIDs (kind 0: 150, kind 4: 22, kind 5: 1), every record with an actor | the same |
| MULE manager | 150 members, all of them in the registry, 0 killed records | the same |
| fingerprints (tags 1 to 4) | ids `40624894`, guids `55AD7620` | ids `40624894`, guids `55AD7620` |

- All 150 GUIDs match, and every matched pair has **the same actor id** (150 / 150), the same state, the same dead flag, at
  every tag. The ids are a usable key on their own; the GUID is the proof.
- The other actor types are not all equal: cargo (`DSBaggageGameActor` 1571 vs 1576), delivery points (1124 vs 1143), carts
  (18 vs 17); and after `dev_enemies 1` the guest had 20 trucks (`DSVehicleTruckGameActor`) to the host's 16: the resurrect
  gave the guest's MULEs 4 trucks the host's world did not make (MULE trucks are P5 and meet `vehicle_share`).

**Places: the same where they were placed, not where they move.**

| tag | matched | place difference p50 / p95 / max | > 5 m | the 21 / 17 in the world (near the camp) |
|---|---|---|---|---|
| 1 (+0 s) | 150 | 0.00 / 13.2 / 18.9 m | 17 | p50 0.13 m |
| 2 (+30 s) | 150 | 0.00 / 16.3 / 25.0 m | 22 | p50 4.3 m, max 25.0 m |
| 3 (resurrected, +0 s) | 150 | 0.00 / 13.3 / 18.7 m | 18 | p50 0.00 m |
| 4 (+30 s) | 150 | 0.00 / 13.3 / 18.5 m | 18 | p50 0.14 m, max 8.6 m |

- About 120 MULEs stand where the level puts them, to the centimetre, in both worlds.
- 29 move in 30 s (up to 98 m). The patrols that are **not** in the world (state 0) still move in both worlds, along the same
  path at 1.11 m/s, but the guest's are **12 s (and 16-17 s) behind** the host's, the same lag for every patrol of a group:
  their clock starts at the world's load, and the guest's world loaded later. Nothing corrects it.
- The MULEs in the world at the camp (66-160 m from the Sams) start at the same spot and **diverge by up to 18 m within
  30 s** once their AIs run separately (tag 2; over every MULE in the world, some up to 650 m away: p50 4.3 m, max 25 m). After the resurrect, with both Sams still and invulnerable, they stayed within 1.7 m in 30 s.
- The anim census on the host: a MULE (`ds/entities/npc/mule/pe_mule`, `Mule_Light_Mule`, AIEntityType Humanoid) uses
  **Sam's own Skeleton object** (77 joints, layout `BE39C83F`): the STJ1 pose codec carries a MULE's pose unchanged.

**What this means for P1.**

- The guest needs no identity table: after a load both sides send the `guids` fingerprint (and the count) once; equal means
  "actor id = the same enemy". A mismatch falls back to IDTAB (4.5).
- Freezing only the enemies in the world is not enough: the unrealized patrols are already out of step by the guest's load
  delay, so an enemy that comes into the guest's world appears at the wrong place. P1a must also place the guest's
  unrealized copies from the host's state (2 Hz is plenty at 1.1 m/s), through the game's own actor transform setter (actor
  vfunc +0x40, the call `0x142B5BAE0` uses), not by writing +0x28.
- Within a camp two AIs drift 4 m at the median and 18-25 m at worst in 30 s: mirroring (or the puppet fallback) is
  required, as the MGSV numbers said.

## 4. The design (P1 onward)

### 4.1 Owner and mirror

Each enemy has one **owner world** that runs its AI. The host is the default owner, and outcomes are final on the host (its
save keeps them). In the other world the enemy is a **mirror**:

- its AI frozen with MsgGameActorSequenceOccupied{1}, posted through `0x1422FB870(reg, id, &msg, 1)`;
- its transform set every tick with `EntitySetWorldTransform` (`0x142361EA0`), with MsgEntityTeleported and the vehicle
  correction bands if the mover fights it;
- its pose written by the existing animate hook (`0x1423AA540`) from STJ1-coded joint frames (the MULE rig is expected to be
  default_male, 77 joints, Sam's layout: the T0 anim census checks it);
- a lease: no frame for 1.5 s releases it (Occupied{0}). Never freeze an actor a real sequence holds (+0xEA4 bit 14 set without
  us), never release one the mod did not freeze.

**Fallback, the first one to try if the freeze does not hold**: do not freeze the guest's copies at all. Take them out of the
guest's world (RemoveNpcFromWorld) or keep them low-LOD and hidden (ForceLowLodNpc plus hiding the entity), and draw each
host-owned enemy as a **puppet** through the partner-body pipeline (a cutscene-actor body of the same character, posed from the
owner's joint frames). This removes the "AI fights the transform" risk entirely, at the cost of the guest's world having no
real enemy there (hits on a puppet go to the owner, P2).

### 4.2 Identity

The key is the NPC GUID (level data, the same in both worlds because the guest loaded the host's save), with the actor id as a
cross-check. A GUID match is mirrored only when both places agree within 5 m (`Compare`'s guard); a mismatch is never mirrored
and is logged. Tables reset on every load and every private-room visit (the world epoch, as SWS1).

### 4.3 Ownership (P1b; computed by the host, pure and unit-tested)

- The guest takes an enemy when her Sam is within 40 m of it, at least 15 m nearer than the host's Sam, and it is not in
  caution on the host.
- It goes back when she is more than 60 m away, or the host is within 20 m and not engaged.
- Each decision holds 10 s; the host wins ties; epochs as VOWN (section 23).
- Handover carries transform, phase, stance, health / stun and defeat; the new owner teleports its copy, applies
  `SetNpcIndividualPhase` and the health, then unfreezes.

Consequence (documented for players): an enemy chases and hits only its owner's player. On the other screen it is seen going
after the partner. The non-owner can still damage it (P2).

### 4.4 Phases

| Phase | What | Gate run |
|---|---|---|
| P0 | the census, GUID identity, byte checks (done; section 3) | `two-games-enemy-census` |
| P1a | the host owns everything; the guest freezes and drives mirrors of the realized enemies within 250 m of her (state 10 Hz; the rest 2 Hz round-robin; poses for the nearest 6 at 10 / 5 Hz; 4.7) | `two-games-enemy-mirror`: placement error p50 < 0.3 m, p95 < 1 m; frozen copies send no attack messages; the lease releases when the guest leaves |
| P1b | proximity ownership and handover (4.3) | the same run: ownership flips when the guest walks to the far side of the camp |
| P2 | damage and outcomes: on the non-owner a detour on DSMuleDamageComponent MsgDamage (`0x142B020F0`) or Humanoid MsgDamage (`0x143744380`) drops the mirror's damage and sends HIT {key, attack id, amount, hit point, direction}; the owner applies it with `0x14298AC90(0, id)` and the single-receiver query; OUTCOME is host-final, ordered Normal < Restrained < Sleep = Faint < Dead (the first terminal state wins); phase synced escalation-only (`SetNpcIndividualPhase`) | a debug `enemy_hit <attack id>` on the nearest mirror: HIT, HITRES, OUTCOME on both |
| P3 | projectiles: a hook on the bullet enqueue `0x142916860` turns bullets of our Sam and our owned enemies into FIRE events; the other world replays them through `0x14294D340` (same attack id, origin, target point, target actor), so trails and impacts are the game's own; replayed bullets damage only the receiver's own Sam (the victim decides, as vehicle hits) and enemies only in their owner world. P3b: grenades, blood grenades, bola and strand after a census of their spawn paths | a fire-replay run: the impact seen, no damage to the partner from the shooter's replays |
| P4 | BTs: each world keeps its own BT field; sync the field cleared / respawned, capture events (notice, cosmetic tar), the catcher owned by the caught player's world, a voidout as a shared game over; mirroring a catcher needs a pose codec for other joint counts | |
| P5 | lethal play: Demens deaths, corpses and necrosis (voidout risk), MULE trucks (they meet `vehicle_share`), reinforcements (`RequestMuleReinforce`), cargo stolen into MULE postboxes | |

### 4.5 Wire formats (P1+; none exists yet)

| Frame | Channel | Contents | Bandwidth |
|---|---|---|---|
| SEN1 | unreliable, owner -> other | `'SEN1' \| u32 seq \| u32 sender_ms \| u16 world epoch \| u8 n`; per enemy 24 B: `u16 key \| u8 flags (phase 2b, stance 2b, defeat 3b, moving) \| u8 owner epoch \| i32 xyz mm \| u16 yaw \| i16 vx, vz cm/s \| u16 anim hint` | 16 enemies x 10 Hz = 3.9 KB/s |
| SEP1 | unreliable | the STJ1 joint body (497 B) with a u16 key | adaptive, capped at 24 KB/s (6 posed enemies) |
| SFX1 | unreliable | about 32 B a shot: shooter key, attack id, flags, origin mm, direction, speed, target key and offset | a rifle burst about 0.3 KB/s |
| SCB1 | reliable, a new channel 6 "combat" (`kReliableChannels` 6 -> 7 in `native/include/stcoop/link_protocol.hpp`) | HELLO {parts: mirror, damage, fire, bt}, IDTAB {key <-> GUID16, kind, actor type; chunks of 40}, OWN {key, owner, epoch, handover state}, HIT, HITRES, OUTCOME {key, defeat, ver, cause, pos}, PHASE {key, phase, ver}, RESYNC | under 1 KB/s steady |

The key is the actor id when both sides' `guids` fingerprints and counts are equal (3.4, 4.7: no table is needed); only
on a mismatch is it a u16 index into the IDTAB the host then sends after every load. `session.ini enemy_sync` (default 0) switches it on;
HELLO part bits let a newer build talk to an older one with the parts both have.

### 4.6 Files (implementation rounds)

- New: `native/mod/enemy_wire.{hpp,cpp}` (pure codecs), `native/mod/enemy_rules.hpp` (ownership, outcome order, leases),
  `tests/enemy_wire_tests.cpp`; `native/mod/enemy_sync.{hpp,cpp}` (freeze, transform, ownership), `enemy_combat.{hpp,cpp}`
  (the MsgDamage detours, HIT apply), `enemy_fire.{hpp,cpp}` (the bullet hook and replay).
- Factored out: the pose capture / apply registration from `real_body.cpp`'s animate hook (for enemy models); the
  single-receiver damage query from `partner_tackle.cpp`.
- Changed: `link.{hpp,cpp}`, `link_protocol.hpp` (channel 6, the frames), `config.hpp` (`enemy_*` keys),
  `docs/debug-actions.md` (`enemy_sync`, `enemy_hit`).

### 4.7 The recommended P1 (from the T0 result)

1. **P1-0, one probe run before any wire code**: on the guest, freeze one realized MULE with MsgGameActorSequenceOccupied{1}
   (a hidden-run debug action, posted through `0x1422FB870`), set its transform for 10 s, then release. Gates: it stays where
   it is put (< 0.3 m), it does not attack, it plays its pose, and Occupied{0} gives its AI back. If any gate fails, P1 uses
   the puppet path (4.1).
2. **P1a, host-owned everything, no ownership yet**:
   - HELLO carries `enemy_sync` parts, the count and the `guids` fingerprint; equal fingerprints = key by actor id.
   - SEN1 from the host: the enemies within 250 m of the guest at 10 Hz (realized) and the rest of the 150 at 2 Hz in a
     round-robin (about 4 KB/s in all).
   - The guest freezes every realized copy it mirrors (lease 1.5 s), drives its transform and its pose (SEP1, the nearest 6,
     STJ1 body: the MULE skeleton is Sam's), and places its unrealized copies with the actor's own setter.
   - The guest's own MULE trucks and cargo stay as they are until P5 (logged).
   - Gate run `two-games-enemy-mirror`: placement error p50 < 0.3 m, p95 < 1 m at the camp; the guest's copies send no
     attack; tags 1 / 2 of the census agree within 1 m on both games.
3. **P1b** (ownership by proximity) only after P1a passes.

## 5. Risks

- **The freeze lever is unproven** (vt+0x48 / vt+0x50 are named by their effect only). Fallback: the puppet path (4.1).
- **The mover fights the transform.** MsgEntityTeleported plus the vehicle correction bands; else the puppet path.
- **Ids or places differ between worlds.** Match by GUID with a 5 m guard; never mirror a mismatch (the T0 run measures how
  often it happens).
- **Real sequences and cutscenes** use the same occupied flag: respect bit 14.
- **Loads and the private room**: everything resets per world epoch.
- **Saves**: guest-caused outcomes land in the host's save (intended); hidden runs are sandboxed.
- **Online gate**: no new uploads are expected (kill stats and telemetry are already dropped on the guest); every run must show
  0 game-server writes.
- **Cost**: cap the posed enemies and the bandwidth (4.5).
- **Lethal play**: necrosis can lead to a voidout (P5).
- **Asymmetric perception** is by design (4.3) and must be explained to players.
- **Unknowns**: health / stun offsets, the meaning of vt+0x48 / vt+0x50, the spawn radius.

## 6. Test plan

One hidden run per phase (docs/hidden-testing.md "Test economy"), each ending pass, saves_unchanged, restored, no violation,
0 cloud uploads, 0 game-server writes, online_gate_ok:

- **P0**: `st_enemy_census_tests` (ctest) and `two-games-enemy-census` (section 3).
- **P1**: `two-games-enemy-mirror` (gates in 4.4).
- **P2**: a run with a debug-synthesised `enemy_hit`.
- **P3**: a fire-replay run.

The area is `enemies` in `tools/testenv/test-areas.txt` (`native/mod/enemy_*`, and debug-action lines naming the enemies).

## 7. Round 15: R1, the probes, ship A

The plan is round 14's `enemy-design/plan.md` (R1, R1a, R1b, R2). Everything below was built in round 15 and run hidden.

### 7.1 R1: foundations (no gameplay change of their own)

- **`sam_damage.{hpp,cpp}`**: the one MinHook on our Sam's MsgDamage handler `0x14271BC00` (byte-checked: its 23-byte
  prologue; `[samdmg] off: ...` on a mismatch), created at start-up by the first client that asks (`Ensure`). The handler is
  `void(entity, msg)`: when its damage component refuses a DamageInfo it sets msg + 8 = 1 and returns; a dropped message does
  the same and the original is not called. Clients: observers (run first, may change the message) and filters (the first drop
  wins). The vehicle hits (`partner_tackle.cpp`) are its first observer, with their code and log lines unchanged (they hooked
  the same handler themselves before); the shots register the filter "shots". `View` carries the DamageInfo's attack
  reference (+0x28), context (+0x40), attacker (`Q(ctx + 0x68) - 0x20`), amount and hit point.
- **The bullet Enqueue detour** (`shot_sync.cpp`, `0x142916860`): byte checks 18/18 (the prologue, the slot layout and serial,
  CreateBullet's thunk and body, the two RIP globals `[0x147BEB0F8]` / `[0x147BEB108]`, the weapon / magazine lookups, the
  current-weapon getter, the Humanoid damage handler, and the five call sites still calling Enqueue).
- **Link**: `kReliableChannels` 6 -> 7, `kCombatChannel = 6`, `kPackOrder = {1, 2, 3, 4, 6, 5}` (PackBody and ReliableDue offer
  the channels in that order: the assets stay last; `TestCombatChannel` in `st_net_tests`); `SendCombat` / `PopCombat` (a
  queue of 64, cleared by a new session); the combat frames `SendCombatFrame` / `PopCombatFrame` for SEH1 (2), SEN1 (8), SEP1
  (16), SFX1 (32), raw bytes decoded on the game thread. Nothing is queued on channel 6 yet.
- **SEH1** (`shot_wire.hpp`, 24 B): parts bit2 "fire" is set by this build; 1 Hz for 10 s after a new link session, then every
  5 s and on every change. `[link] combat parts: ours 0x04 theirs 0x04: fire on`. Since the review the bit means "send me
  your shots": a game sets it only while it can replay them (the Sam filter registered, the NPC drop installed unless
  `shots_npc_drop=0`); otherwise `fire off (no replays here)`. The beacon's and SFX1's `world` fields stay 0: no identifier
  is shared between the two games (each game's load counter is its own), so a replay is bounded by the partner body instead
  (section 7.3, the review's item 4).

### 7.2 The probes

**P3-0, shots** (`shots-probe`, run `20261123-204732-shots-probe`, pass; two earlier runs found the corrections below):

- Sam's bullets do go through Enqueue (called from the fire path `ds.exe+0x2918FC9`, on job threads) and their record +0x20 is
  our Sam (`DSPlayerEntity`), as the plan said.
- **Correction**: Sam's records carry **attack 0** and a category at +0x5A (the non-lethal rifle: category 71); Enqueue itself
  replaces the attack from the category table (registry +0x30 / +0x38, key +0x20, attack u16 +0x38, or +0x3A with +0xD1 bit 0).
  CreateBullet refuses attack 0, so the sender resolves it the same way (the rifle: attack 0x47, 150 m/s).
- The replay (CreateBullet with weapon 45 and magazine 71, both found in the registry: 67 weapons, 133 magazines) is enqueued
  every time and its record is the original's field for field (+05, +5A, +D0, +D1 patched in; checked from the slot after
  the call). The echo stills show the replays' tracers and impacts 2.5 m to the right of our own (contact sheet: the round
  15 scratch folder `shots\sheet.png`, panels 8-9);
  the replays' own muzzle was off screen (2.5 m right of a camera at Sam's shoulder).
- **The damage identity holds**: the bullet system's slot +0x48 handle read after the enqueue equals the hit's DamageInfo +0x28
  exactly (`ref match=1 (how 0, word 0)`), and the attacker is the system's dummy owner. With the filter left out the hit
  reached our Sam (`kept (nofilter)`); with it, `dropped by the filter (ref match)`. So the replay set is the filter; the
  plan's fallback (building records ourselves) is not needed. The dummy owner was a second, weaker rule (a dummy-owned hit
  within 3 s of a replay), unused in the runs; the review removed it (every CreateBullet bullet, the game's own scripted ones
  too, belongs to the dummy): it is now only noted for the test line (`dummy owner seen=1`), never a reason to drop.
- Review note: the two test steps of the probe as run (`waitlog ... ref match=[01]`) accepted either outcome and the second
  matched the first test's line; the scenario now waits for `ref match=1 ... kept (nofilter)` and then `ref match=1 ...
  dropped by the filter (ref match)` with `waitnew`. The run's log has both lines (38.8 s and 41.2 s), so the result above
  stands; the scenario is a probe (`# select: probe`).
- Leak check: 200 replays (one attack context each): working set 4878.2 MB -> 4870.3 MB. No leak.
- The magazine effect (+0x140 at the muzzle) was passed in the runs (`shots_sound=1`); whether it is the shot's sound, and
  whether it doubles `sound_sync`'s own weapon sound, was not measured: `shots_sound` defaults to 0 since the review.

**P1-0, freeze** (`enemy-freeze-probe`, run `20261123-204901-enemy-freeze-probe`; picked MULE `02029` 64 m away, then our Sam
12 m from it; byte checks 23/23):

| lever / mode | err p50 / p95 / max | animate | release | note |
|---|---|---|---|---|
| 0 none (control) | self-moved 7.9 m | 30.0/s | moved 14.6 m, turned 43 deg | its AI walks; it fired no shot and hit nothing (a silent control) |
| 1 Occupied{1}, 1 per-tick set | 0.00 / 0.00 / 0.00 m | 15.1/s | ea4 0x2046000 -> 0x2040000, moved 22.4 m, turned 177 deg | **hold PASS, release PASS**; weapon +0x268 = 1 while frozen |
| 1 Occupied{1}, 2 per-tick teleport | 0.00 m | 12.2/s | "moved 52 m", a88 0x2 | the MULE left the world during or right after it |
| 2 Occupied + Required, 3 | (invalid) | 0/s | 0 m | no longer realized: the probe kept reading a stale entity (fixed: a test now ends `ABANDONED`) |
| 4 Occupied + Sleep, 3 | (invalid) | 0/s | 0 m | as above |

The run ended early: our Sam had fainted (the camp's non-lethal fire; god mode 2 does not stop a faint), so the pose step was
not reached. The probe was a FAIL as a run (line 47: `teleport refused: game state: Faint`). Since the review the scenario
(`# select: probe`) refills Sam before every lever, walks him 3 s into the camp before the control, waits for each test's own
`done:` line (`waitnew`), requires `hold=PASS ... release=PASS` for lever 1 / mode 1 and fails on any lever-1 `ABANDONED`; a
MULE that unloads now also ends its test `done: ABANDONED` with the pose slot cleared (it was left armed).

**The decision for P1a** (the plan's rule, 4.3): **lever 1, Occupied{1}, with mode 1** (actor vt+0x10 and
Entity_ExportedSetWorldTransform every tick; no teleports) is the first row that held (0.00 m) and gave the AI back (Occupied{0}
plus Required{0}: bits 13 and 14 cleared, it walked and turned). Its weapon flag +0x268 was set while frozen (the NPC weapon
update returns at once: it cannot shoot). Open, for the next round's first run: **G2** (the control was silent, so "silent" is
INCONCLUSIVE: rerun with our Sam walked into the camp and kept from fainting), **G3** (the animate rate fell from 30/s to 15/s
under Occupied, below the plan's 20/s, and the written pose was not measured), and levers 2 / 4 (invalid here). The fixed probe
(the realized-state check, the topmost-ground teleport) is in the tree; `enemy_freeze` defaults to `occupied` when P1a lands.
The puppet path is not needed on this evidence.

### 7.3 Ship A: the players' own shots (`session.ini shots_sync`, default off)

- **Sender** (the Enqueue detour, any thread): a record whose owner is our Sam is copied into a ring of 64; the game-thread
  tick turns it into a shot: origin, direction and speed from +0x60 / +0x70, the attack (resolved from the category), the
  record bytes +04 +05 +06 +58 +5A +D0 +D1 +84 +94, the weapon and magazine ids of the inventory's current weapon (instance
  +0x12, the magazine parameter's +0x20), the origin relative to our right hand (joint 26) in our yaw frame, the muzzle flag on
  the first record of a pull (16 ms, the same weapon). The target actor is never sent in ship A (an absolute point 150 m along
  the shot). SFX1 frames (20 B + 68 B a shot, up to 14), every shot in two consecutive frames; a cap of 32 shots per 100 ms.
  Sent only while both beacons carry "fire".
- **Receiver** (game thread): dedupe by shot id (256-id window), hold until the partner body's pose play time reaches the
  shot (late after 500 ms), the origin re-anchored on the partner body's right hand (within 1.5 m of the sent origin),
  refused when it starts more than 30 m from the body or when no partner body is drawn here (another region, not spawned:
  `refused ... origin`), then CreateBullet with the weapon id (the game's own muzzle flash, `shots_muzzle`) and magazine id
  (`shots_sound`) only on the pull's first shot, both checked against the registry first (a missing id becomes 0), 8 replays
  a tick at most. In the Enqueue detour the replay's record gets the original's bytes and its attack handles go into the
  replay set for as long as the bullet can fly (150 m / speed + 1 s, 4..10 s; 2048 entries).
- **Checked before the game sees it** (round 15 review): the Enqueue reads the category (+0x5A) from the registry while it
  holds the bullet system's lock and reads `list[-1]` for a missing one; a fault there would leave the lock held and hang the
  game (SEH cannot recover it, so SEH is only a last resort). So a replay is refused (counted: `refused ... category`) when its
  category is not listed; +0xD1 is masked to bit 0 (Sam's records carry 0x00; bit 1 adds a context path, 0x14298AEC0, that
  looks the category up again); attack ids 0x5F / 0x60 (CreateBullet's projectile-entity path, 0x14294C870) are never encoded,
  decoded or replayed; a speed under 20 m/s is refused; a replay that would push a live entry out of the replay set is refused
  (`set full`). Replays need the Sam filter AND, unless `shots_npc_drop=0`, the NPC drop installed (`[shots] OFF for replays:
  the NPC drop is not installed`); the test actions (`shots_test`, `shots_echo`) are hidden-run only. The filters keep
  matching after a fault-stop, so replays in flight stay dropped.
- **Damage**: a hit whose DamageInfo reference is in the replay set is dropped on our Sam (`shots_friendly_fire=0`) and on
  every Humanoid (MULEs, Demens: a detour on `0x1437393D0`, `shots_npc_drop=1`). The Humanoid drop is **not run**: every
  status line of the round's runs says `NPCs 0` / `enemies 0` (no replay hit a MULE). Vehicles, cargo, structures and BTs (not
  Humanoids; hematic rounds) the replays hit are not filtered, and the replays' impacts and noise can still alert this world's
  MULEs (known gaps, below).
- **The gate** (`two-games-shots`, run `20261123-205837-two-games-shots`, the launcher's own files, no switch), **FAIL**: its
  `summary.json` says `"result":"fail"` (1 failure): the optional step C (`?waitlog ... replay hits dropped: our Sam [1-9]`)
  failed, and an optional step that fails fails the run. The rest: fire on on both;
  the host sent 15 and the guest replayed 15 (muzzle 15, magazine effect 15, late 0, refused 0); the guest sent 13 and the
  host replayed 13 (muzzle 13). No fault, saves unchanged, 0 game-server writes. Stills (contact sheet panels 1-6): on the
  host's screen the guest's body with its rifle, a muzzle flash at its muzzle 120 ms into her burst and the tracers going on
  (crops 5 and 6); her own screen (panel 7) shows her shots' impacts on the slope. Direction A (the guest watching the host)
  is in the logs only: her own body and backpack hid the host in her stills. Step C (her burst at the host with his god mode
  off) put 8 more replays on the host but none hit him: her camera stopped at "camera aimed #2 ... (-7.4 deg off)", about
  0.8 m beside him at 6 m, so the two-game drop is unproven; the solo probe proved the identity and the drop. (Contact sheet:
  the round 15 scratch folder `shots\sheet.png`, panels 1-6.)
- **Since the review** (`two-games-shots` changed, not run): `ini shots_sync=1` (the default is off); A from 8 m at 135 deg
  (behind his right shoulder); an on/off pair from the host's camera in B (`02-pair-off`: the host's replays off, `shots 2`,
  her burst; `02-pair-on`: on again); C required: the guest 4 m in front of the host, aimed within 2 deg, and the host's
  status must count dropped hits on his Sam with `kept 0`.
- **Default off** (review): the gate failed, the two-game drop on the watcher is unproven, the Humanoid drop is not run and the
  replays still hit vehicles, cargo, structures and BTs. `shots_sync=1` turns it on (the scenarios' test switch); the launcher
  writes no `shots_*` key.
- Logs: `[shots] sent n=.. (pulls .., pellets .., frames ..; attack 0x.. speed ..) weapon ids {..} mag ids {..}`, `[shots]
  replayed n=.. (muzzle .., mag effect ..), late .., ... refused: weapon .. mag .. attack .. origin .. target .. filter ..
  category .. speed .. set full ..`, `[shots] replay hits dropped: our Sam .. (replay set ..), enemies ..`,
  `[shots] FAULT in <step>: shots stopped for this session`.

**Known gaps of ship A**: thrown grenades, launchers, the sticky gun and the bola are entities of their own, not replayed
(P3b); a replay can still damage the receiver's vehicles, cargo and structures it hits (only our Sam and Humanoids are
filtered; BTs too), and its impacts can alert this world's MULEs; the target actor (a lock-on) is not carried; the magazine
effect's audibility and a double sound with `sound_sync` were not checked (`shots_sound` off by default); shotgun pellets were
not run (the 100% save's rifle only); the Humanoid drop was not run.

### 7.4 The next round

1. One `enemy-freeze-probe` rerun (the fixed probe; our Sam walked into the camp so the control attacks; the pose step) for
   G2 / G3 under lever 1, mode 1.
2. One vehicle-hit run (`two-games-vehicle-hit`): the vehicle hits now observe through the shared detour (built and installed
   in the gate run, `[samdmg] ... asked first by vhit`, but no vehicle hit was run this round). `sam_damage.*` now touches the
   area `sam-damage`, which only that scenario covers, so the selector picks it.
2a. One `two-games-shots` run (the changed gate: step C required, the on/off pair) and a step that puts replays into a MULE
   (`replay hits dropped: .* enemies [1-9]`) before `shots_sync` can default on again.
3. Then R3 (P1a mirrors with lever 1 / mode 1, SEN1 / SEP1, enemy shots through the same SFX1 path) and its gate
   `two-games-enemy-mirror`.
