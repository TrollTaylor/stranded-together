# Enemy sync (P0 done: the census; round 15: R1 foundations, the P1-0 freeze probe, ship A: the players' shots; round 16: the drop at the bullet, P1a mirrors and the enemy shots)

Co-op in Death Stranding means both players see the same MULEs, Demens, warriors and BTs, and what one player does to them
the other sees. Today each game runs its own enemies: the guest's world is a copy of the host's save (the host-held save
session, [coop-session.md](coop-session.md)), so the same camps exist in both, but their AI, their alert, their hits and their
deaths are separate. This document is the plan for changing that, phase by phase, and the record of what each phase proved.

Status: **P0** (the census) is implemented and run (section 3): both worlds have the same 150 MULEs with the same GUIDs and
actor ids, placed identically, but their patrols run out of step and their AIs drift apart. **Round 15** (section 7) built the
foundations of the plan's R1 (the shared Sam damage detour, the bullet Enqueue detour, link channel 6 "combat" with the pack
order, the SEH1 beacon and the combat frame queues), ran the two probes (P3-0 shots, P1-0 freeze) and built **A: the players'
own shots** on the other screen (`shots_sync`; off after the round 15 review, **on by default since round 16**: a replay's
hits are dropped at the bullet before anything of the watcher's world takes them, section 7.5). **Round 16** built **P1a**
(section 8, `enemy_sync`, on by default with `host_save=1` since round 16b, 8.9): the host owns every enemy, the guest's copies near her are frozen, driven and posed
from the host's frames (within 0.07-0.56 m at p95 in the census), never hurt her or fire themselves, and the host MULEs'
rounds are replayed on her screen from the mirrors (and may hurt her own Sam). Hits on enemies (P2) and ownership (P1b) are
next.

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

### 7.3 Ship A: the players' own shots (`session.ini shots_sync`, default on since round 16: 7.5)

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
  writes no `shots_*` key. **Round 16 (7.5)**: the drop at the bullet closes the vehicles / structures / NPCs gap, step C
  passed twice, and `shots_sync` defaults to 1.
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
2a. Built in round 16 (7.5): `two-games-shots` ran twice with C passing, replays into a MULE dropped at the bullet, and
   `shots_sync` defaults to 1. **The gate passed on the final code** (run `20261124-120631-two-games-shots`, A to E): A on
   the flat road (`mknot-road`), B to D on the outcrop at -1153.4, 338.0 (at `mknot-road` her rifle did not fire: section
   7.5). Cargo is out of the gate and out of the probe (7.5).
3. Then R3 (P1a mirrors with lever 1 / mode 1, SEN1 / SEP1, enemy shots through the same SFX1 path) and its gate
   `two-games-enemy-mirror`. Done in round 16: section 8.

### 7.5 Round 16: the drop at the bullet, shots on by default

A replay is visual only: the shooter's own game decides the real damage. Round 15 filtered our Sam and Humanoids only; round
16 drops a replay's hits where every bullet hands them over, before any receiver of this world sees them.

- **Where** (read-only research, the round 16 `r-dmg` notes): the hit processor `0x14291CBC0(bullet, system, f32)` calls
  BulletDamageA `0x142912120` (sites `0x14291D117`, `0x14291D699`); the second bullet kind's `0x1429415A0` calls BulletDamageB
  `0x142912380` (site `0x142941737`). Both are `void(bullet, receiver, hit, f32 amount, [rsp+0x28] u64)`, build the DamageInfo
  from `Q(bullet + 0x38)` (the replay set's key: the slot's +0x48 holder of round 15) and call
  `receiver->vt[+0x10](receiver, DI)`, the only place a bullet gives damage to a vehicle, cargo, structure, BT, NPC or prop. An
  entity's receiver is entity + 0x38 (the cast `0x1423816E0`). The impacts (decal, particles, sound), the tracer and the
  near-miss list are made by the hit processor whatever the damage call does, so skipping it keeps every visual.
- **The rule** (`shotw::BulletRule`, unit-tested): a bullet whose owner (bullet +0x20) is the bullet system's dummy (every
  CreateBullet bullet's; read first, the cheap exit: no bullet of the game's own pays the set's scan) and whose reference is
  in the replay set is not handed to its receiver, except our own Sam (receiver == Sam + 0x38): his hits go on to the Sam
  filter. A part of our Sam (an entity whose parent chain, +0x70, reaches him: his armor plates; run `093441` dropped one,
  `DSArmorPlateComponent`) takes its own damage, so it is passed only where the Sam filter's policy keeps the hit
  (`erules::ReplayPolicy`: friendly fire, `enemy_shots_hurt`, a nohurt test).
- **The dummy** (review fixes): while it reads 0 (the first seconds after a load) nothing is replayed (`refused (dummy
  unknown)`) and no bullet counts as a replay's; when it changes (a load) the replay set is cleared, so the old world's
  references cannot match the new world's bullets.
- **Our Sam** (review fix): the Sam filter, and the Humanoid backstop, take a hit as a replay's only when its reference is in
  the set AND its attacker is the dummy (`shotw::ReplayHit`, unit-tested; every replay's attacker is, runs `093441` /
  `104304`). A game round whose reference reuses a replay's is kept and counted (`reused references kept on our Sam n`):
  before the fix a MULE's real round on the host could be dropped while the partner was firing.
- **Enemy sync's guard** (review fix, section 8.1): any bullet's hit on a frozen mirror (or a part of one, the +0x70 chain) is
  dropped here first while `enemy_mirror_guard=1`, whatever fired it.
- **The BT reaction**: a non-hematic bullet on a BT surface (material 0x17) makes `0x142992D80(hit, owner, u8, u32, attack
  0x182 InvalidPhysicalAttackOnBT)` at `0x14291D64E`, a context of its own (not ref-matched). A detour on the hit processor
  marks a replay's bullet in a thread-local; the detour on `0x142992D80` returns at once while it is set and its caller is
  that site (return address `0x14291D653`). The skipped call returns 0: its only caller jumps on to `0x14291D5CA` and writes
  rax before any read, and BulletDamageA / B's three callers ignore rax too (checked in ds.exe). Not exercised in a run (no
  BT), so it is **off by default** (`shots_bt_drop=0`, `BT reaction off`): a replay that hits a BT may make it react.
- **Byte checks** (35/35 in every run): both prologues, both `+0x48` reference reads, the three call sites, the cast, the hit
  processor (its `+0x16C` / `+0x164` checks), the BT function and its call site, the cargo exports of the aimed test; a
  mismatch in the drop's group leaves it uninstalled and, with `shots_hit_drop=1` (the default), nothing is replayed here
  (`[shots] OFF for replays: the drop at the bullet is not installed`); the BT group is optional (`BT reaction off`). Every
  detour is SEH-guarded (a fault passes the hit on and counts `faults`).
- **Counters**: `[shots] replay hits dropped at the bullet: vehicle n cargo n structure n bt n npc n player n other n (seen
  n, our Sam passed n, test let through n, BT reactions dropped n, faults n; reused references kept on our Sam n)`, and the
  same in `shots 0` (`at the bullet (on, BT off): dropped ...; ... hits on frozen mirrors dropped n; reused references kept on
  our Sam n; refused (dummy unknown) n`). The detours only format their lines (`LogLater`); the game thread writes them. The class is the receiving entity's (the game's type name, `shotw::ClassifyHit`);
  a part with no class of its own ("Entity") takes its first parent's that says more, else its components' (a postbox:
  a component names it a structure).
- **The aimed test** (hidden runs, `shots 5 <target> <n>`): n replays (kind 4: a partner replay for every filter) of the
  partner's last received shot (else ours) from 6 m in front of a target, straight at it; a negative n is the control (let
  through at the bullet). Targets: 1 the nearest vehicle (its life, entity +0x6E8), 2 a structure (`100 + type`: of that
  type; its durability, cp +0xD8), 3 the cargo piece `shots 6` picked (DSBaggage2 +0x34), 4 the enemy probe's MULE (counters
  only). 2.5 s later one line: `aimed test #k: <target> (...) x n: enqueued n; at the bullet dropped +n (classes), let through
  +n (classes), our Sam passed +n, BT +n; Sam filter +n, NPC backstop +n; value before x after y: unchanged | CHANGED`.

**Runs** (each: saves unchanged, 0 game-server writes, 0 cloud uploads, online gate on; contact sheet: the round 16
scratch folder `shots\sheet.png`):

| Run (`build-testenv\runs\`) | What it showed |
|---|---|
| `20261124-092442-shots-hit-drop` (solo probe, FAIL at the MULE line) | truck: 6 aimed replays, **dropped 6 (vehicle: DSVehicleTruck), life 15000 -> 15000**; control, 8 let through: life still 15000 (the non-lethal rifle's rounds do not wear a truck: the control is not sensitive); the nearest structure was the road rebuilder (type 36: fixed, `106`); the carried-piece drop (`shots 6`'s first form) left the piece on Sam without an entity (unproven); the MULE: 12 dropped, classed "other" (fixed: the parent's class); the sound windows below |
| `20261124-093019-shots-hit-drop` (solo probe, FAIL at the postbox line) | truck as above; the postbox (type 6): 6 dropped, durability 359996.9 -> 359994.8 (decay, within 5), classed "other" (fixed: components) |
| `20261124-093441-two-games-shots` (FAIL only on an optional `dev_vehicle_remove` with no vehicle: removed) | A (the guest's view of the host from 135 deg: a rock hid him), B and the on / off pair as round 15; **C passed: the host's Sam filter dropped 6 of her replays (attacker = the dummy, ref match), kept 0, health 1000**; D on the host: the truck **dropped 6 (vehicle), life unchanged**; the postbox dropped 6 (classed "player" by a `DSJointAimToPlayerComponent`: fixed), durability unchanged within decay; one of her C replays hit his armor plate and was dropped at the bullet |
| `20261124-093844-two-games-shots` (FAIL at E's cargo line only) | A from his right side: still behind a rock in her still (the host stands on an outcrop at -1153.4, 338.0); B: her replayed burst on the host's screen (muzzle flash at her rifle and a tracer, sheet panels 1-3), the pair off / on (4-5); **C: 4 dropped on his Sam, kept 0**; D: truck **dropped 6 (vehicle), life 15000 -> 15000**, postbox **dropped 6 (structure)**, durability within decay; E at the MULE camp: the probe's MULE, 6 replays **dropped 12 (npc 6 DSControlledEntity, 6 receivers with no class)**, NPC backstop +0; the control let 12 through and the Humanoid backstop saw none of them either (so the round 15 Humanoid drop is not where these rounds reach a MULE: the drop at the bullet is the real guard); the cargo piece `shots 6` found (47 m off, entity index 260, +0x34 900) took none of the 4 replays (no receiver hit: its +0x38 may not be the realized piece's place) |

| `20261124-112720` / `-114027-two-games-shots` (FAIL at B both times) | A at `mknot-road`: the guest sees the host's flash and tracer (112720 A-a / A-d); B: she stood 7 m from him on the road beside the road rebuilder and her LT + RT fired nothing (HUD 30/222 in every still, no `sent` line; releasing her LT first changed nothing). Why the game would not fire there is not known |
| `20261124-120631-two-games-shots` (**PASS**, 132 steps; A at `mknot-road`, then both to the outcrop for B to D) | A: her still shows a flash cloud ahead of his rifle (01-guest-sees-host-c; the host sent 13, she replayed 13); B: **her muzzle flash on the host's screen** (02-host-sees-guest-b), the pair off (no flash) / on (flash and tracer), her own screen shows the burst (she sent 15, then 30 with C; 4 not sent while his replays were off); **C: our Sam 4 dropped, kept 0, health 1000** (at the bullet: 2 parts of him); D: truck **dropped +6 (vehicle 6)**, life 15000 -> 15000, postbox **dropped +6 (structure 6)**; E: the MULE **dropped +12 (npc 6, other 6)**, NPC backstop +0, the control let 12 through. Sheet: the round 16 scratch folder `prove16b\shots.png` |
| `20261124-121151-shots-hit-drop` (**PASS**, the probe without its cargo step) | truck dropped +6 (vehicle 6), control let through 8; postbox dropped +6 (structure 6), control 8; MULE dropped +12 (npc 6, other 6), control 12 |

**Cargo is not proven**: no replay reached a cargo piece in any run. The probe's optional cargo step is gone (round 16): near
the MULE camp `shots 6` picks the nearest DSBaggage2 with an entity, 46-47 m off (runs 093844 and 115147), and the 4 aimed
replays hit no receiver either time; its stored place (+0x38) is likely not where the realized piece is (the camp's MULEs
carry their own cargo). At `mknot-road` there is no such piece, and the carried piece `shots 6` puts down keeps no entity
(092442). A failed optional step fails the run, so the probe could not pass with it. A spawned piece at a known place is
the missing target. Every hit on any receiver goes through BulletDamageA / B,
so a piece's hit would be dropped like the truck's, but no still or counter shows it. The vehicle's and the structure's
controls did not change their values (the rifle's rounds wear neither), so "unchanged" there shows that nothing was applied,
not that the read would have moved.

**The sound** (`shots_sound`, run `092442`, the audio probe's dumps per window): our own 4-shot burst posts the gunshot event
`0xE3937F6C` on our DSWeaponEntity's SoundComponent (RTPC `69F5CC2B`), once a shot. Every replay posts `0x27735FF7` once
(a GraphSoundInstance the bullet code makes, `ds+0x298BA23`), with or without the weapon and magazine ids (6 / 5 / 4 for 6 /
5 / 4 replays). The magazine id adds `0xB9464D7E` once a replay, made at the same place and carrying the gunshot's own RTPC
`69F5CC2B`: a gunshot-like sound of the replay. Round 16 first made `shots_sound=auto` the default (on only while
`sound_sync=0`), assuming sound_sync already plays the partner's gunshot. The review found it does not reliably: the receiver
posts a partner event only when it is seeded (`kSeedEvents`: three rifle ids from Phase 3) or this game posted it on its own
player this session (`partner_sound.cpp` Learn), and `0xE3937F6C` is not seeded, so a watcher who has not fired that weapon
would hear nothing. **`shots_sound` defaults to 1** (the magazine effect on every muzzle replay); `auto` stays as the opt-in for
a two-game sound check with an audio probe on a watcher who has not fired (if both play, the gunshot may double). Whether
`0x27735FF7` is audible (a flyby or tail) was not measured.

**Defaults**: `shots_sync=1`, `shots_hit_drop=1` (new), `shots_bt_drop=0` (new), `shots_sound=1`, `shots_muzzle=1`,
`shots_friendly_fire=0`, `shots_npc_drop=1` (the backstop). The launcher's Debug page **Show my partner's gunfire** (default
on) writes `shots_sync=0` when unticked, and no other `shots_*` key. No hooks go in when no partner can be in the session
(role none or solo outside hidden runs).

**Still open**: cargo (above); BTs (no run; the BT drop is off by default); why the guest could not fire beside the road
rebuilder at `mknot-road` (B now runs on the outcrop); the five direct MsgDamage senders (area damage and others) are not bullets and are untouched.

**Known gap for players**: the AI noise of a replay's first impact (bullet +0x460..+0x468 -> a type-3 noise event in
`0x1429440B0`) still alerts this world's MULEs. When the host shoots up a camp, the guest's own copy of that camp hears the
impacts, is alerted and turns on her while taking no damage from his rounds (without enemy sync: with it, her copies near
her are frozen mirrors and cannot act). A design call for now; the README says so.

## 8. P1a: mirrors (the host owns every enemy) and the enemy shots (round 16)

`session.ini enemy_sync` (default: on with `host_save=1` since round 16b, 0 without; see 8.6), `enemy_sync.{hpp,cpp}`, `enemy_wire.*`, `enemy_rules.hpp`,
`npc_damage.*`, the eight enemy pose slots in `real_body`, and the enemy part of `shot_sync`. Both games need it on, with equal
enemy fingerprints.

### 8.1 What it does

- **Identity.** Each game takes the census's two fingerprints and the enemy count once per world (`enemy::Fingerprint`, 5 s
  after `coop::WorldReady`; one walk, 15-60 ms) and sends them in SEH1 with the mirror bit (bit0) and the enemy fire bit (bit4:
  `enemy_shots` and this game can replay). Mirroring is on while both beacons carry bit0, the peer's is fresh (< 20 s) and the
  count and both prints are equal and non-zero (`erules::FingerprintGate`); the key is the actor id. A mismatch logs
  `[enemy] fingerprints differ (...)` once and keeps it off while they differ; each game recomputes its own every 30 s while
  they do (spawn timing can make the first prints differ) and logs `fingerprint recomputed` only when it changed. A new world
  (the load counter), the private room or a new link session releases everything and starts over. Enemy sync needs
  `host_save=1` (`[enemy] sync off: it needs host_save=1`): the guest writes the host's places into her world's actors, only
  ever into the host-held session's world, which her game never saves.
- **The host** (owner) sends, while the partner body is in its world (its place is the guest's):
  - SEN1 (`ewire`, 20 + 28 n B, n <= 32): every enemy's place (the entity's when realized, else actor +0x28), yaw, velocity and
    state bits (realized, dead, posed, hidden, held by a sequence). `erules::Schedule`: 10 Hz within `enemy_mirror_radius`
    (250 m) of the guest, 2 Hz within 1000 m, 0.2 Hz the rest, oldest first, at most 64 entries a tick. The registry walk
    runs at 2 Hz; a picked entry is read fresh.
  - SEP1 (16 B + STJ1): the nearest `enemy_pose_count` (6) enemies realized on the host within 120 m of the guest
    (`erules::PoseSlots`, 10 m hysteresis), each in a stable pose slot in capture mode; 10 Hz for the nearest 3, 5 Hz for the
    rest. A MULE uses Sam's own skeleton (77 joints, BE39C83F), so the STJ1 codec carries it unchanged.
  - The shooters: the 10 Hz class's realized entities -> actor ids, published lock-free (`erules::KeySet`) to the bullet
    Enqueue detour.
  - Measured (gate runs): SEN1 13-17 frames/s (about 240 entries/s), SEP1 42/s, **about 28 KB/s** at the camp.
- **The guest** (mirror), every tick:
  1. Drains SEN1 into a per-id track (`erules::Track`: 8 samples on the host's tick clock, `erules::SenderClock`) and SEP1
     into a per-id frame pair.
  2. Entry (`erules::Eligible`), at most 4 tries a tick: a fresh frame (<= 300 ms), within the radius, realized here, not dead,
     not hidden or asleep (`a88 & 3`), not sequence-only, `+0xEA4` bit 14 clear here and not held on the host, no cutscene,
     not seated in a vehicle (its entity's +0x70 chain names a vehicle class: untested, refused). The guards must be in (the
     shots' Enqueue and the drop at the bullet, `shots::MirrorGuardReady`) and, with `enemy_shots=1`, the enemy rounds
     replayable here (`shots::EnemyReplayReady`): else `mirror refused`, so a frozen copy is never harmless to her for
     nothing. Then `MsgGameActorSequenceOccupied{1}` (PostToActor; `+0xEA4` read first: bit 13 already set is remembered),
     bit 14 read back (else refused), one actor teleport to the interpolated place: `[enemy] mirror on id=XXXXX (dist .. m,
     err at entry .. m, ea4 0x..)`.
  3. Lease (`erules::Lease`): silence (1.5 s), range (radius + 20 m), unrealized, off (link, world, switch) release with
     Occupied{0}, and Required{0} only when bit 13 was clear before our freeze (a bit the game set stays). Every release first
     reads `+0xEA4`: a mirror whose bit 14 a game sequence cleared is forgotten without a post (`taken`, or `bit 14 already
     clear` on the other release paths). The id leaves the mirror set first (the guards stop matching it at once), then its
     pose slot.
  4. Drive: the track 100 ms behind the newest path (extrapolated by velocity for at most 300 ms; a 5 m jump between samples a
     snap), `erules::Band`: < 5 cm and < 3 deg nothing, <= 2 m the actor's vt+0x10 and `Entity_ExportedSetWorldTransform`, above
     2 m an actor teleport (<= 2 a second; `enemy_sync 7 3`: every 500 ms). The rotation is the current one turned about the
     up axis to the host's yaw.
  5. Pose: a mirror whose host entry says "posed" gets a local slot (<= `enemy_pose_count`) in apply mode; the SEP1 pair is
     blended at the render time and pushed every tick; the animate hook writes local rotations 1..76 and the pelvis
     translation, its own root kept.
  6. Cutscene (`DSGameState` Cutscene): nothing driven, posed or posted; 2 s after it ends everything is released once
     (`erules::CutsceneHold`, stepped every tick whatever the gate). Any release asked for during the hold (a link drop, a
     stale beacon, the gate, `enemy_sync 2`, a fault) is held too (`lease release id=.. held`): the mirror stays frozen and
     in the guards' set until the hold ends, because Occupied{0} calls vt+0x138 (`0x142DCA0F0`), which hides and sleeps the
     actor during a cutscene.
  7. Far placement (`enemy_place_far`, 2 Hz, <= 16 a round): the guest's copies out of her world (state 0) within 1000 m whose
     host place is more than 1 m off are placed with actor vt+0x40 as the game's caller `0x142B5BAE0` does (variant A: save
     +0xD0..+0x10B, place, restore; variant B, `enemy_sync 6 1`, also sets +0x1089 and calls the re-path `0x142AE5000`). The
     back-drift 1 s later is logged. Each moved copy's own place is kept (once, by id) and put back on off, the switch, a
     fault and a link change while it is still unrealized (`far placement undone (...)`); a copy realized since is the
     game's again (forgotten), and a world change forgets them all (that world's actors are gone).
- **The guards** (`enemy_mirror_guard=1`; 0 = counted only):
  - a Sam damage filter after the shots' (`[samdmg] filter 1: enemy: hits from frozen mirrors`): a hit whose attacker is a
    frozen mirror entity (+-0x20), an object whose +0x300 is one, or a weapon entity whose instance (+0x338) is held (+0x300)
    by one (the host's MULE hits name a `DSWeaponEntity`) is dropped; the first 12 log the DamageInfo words. An observer logs
    the first 16 hits on our Sam while a mirror is frozen (and on the host the first 8, for the amounts).
  - the drop at the bullet (review fix; in `shot_sync`'s BulletDamageA / B detours, ahead of the replay rule): any bullet's hit
    on a frozen mirror or a part of one (the +0x70 chain, 4 levels) is dropped (P1a: the host decides): hers, this world's,
    a replay's. Counted as `bullet hits on mirrors dropped at the bullet n`. Run `093844` showed bullet hits on a MULE never
    reach the Humanoid handler, so this is where the guard works for bullets. Not yet proven in a run (the gate's new step:
    her own rifle at a mirror).
  - the shared Humanoid detour (`npc_damage`, moved out of shot_sync's `H_Humanoid`; filters "shots" then "enemy"): every
    hit on a frozen mirror that reaches it is dropped, counted by attacker and by how `rcx` matched. **Unproven**: `rcx
    matched` was 0 in every run, and bullets never reach it; it stays as the backstop for whatever other hits (melee, bola,
    tackle) do. Melee, bola and tackle hits on a mirror were not tested.
  - the bullet Enqueue: a record owned by a frozen mirror returns 0xFFFF without the original (what a full queue does; the
    fire call site ignores the result). Expected 0: the game cancels a frozen MULE's trigger requests itself.
  - `[enemy] mirror attacks: shots n (dropped n), hits on our Sam dropped n (seen n), kept 0; hits on mirrors dropped n (ours ..
    replays .. others ..), rcx matched n (+-0x20 n, -0x38 n); bullet hits on mirrors dropped at the bullet n` every 10 s while
    mirrors are frozen. The guards' lines are formatted on the worker threads and written by the game thread (`LogLater`).
- **The enemy shots** (`enemy_shots`; the shots' hooks go in with `shots_sync=1` or `enemy_sync=1`):
  - Host: the Enqueue detour takes a record whose owner is a published shooter (`Cap.shooter` = its actor id). The weapon and
    magazine ids come from the record itself (`rec+0x58`, `+0x59`), the attack from its category as for Sam, the pull per
    shooter (`shotw::PullGroups`), the hand-relative origin from the MULE's joint 26. SFX1 frames with `kEnemyShooters` (an
    enemy frame carries only enemy ids, a players' frame only Sam: `shotw::ShooterOk`), every shot twice, their own cap (24 per
    100 ms), only while both beacons carry bit4 and mirroring is on. `[shots] enemy record: weapon .. mag .. category ..
    attack 0x.. (registry: ...)` once per pair.
  - Guest: a shot whose shooter is a live frozen mirror is held until the mirror's render time (`erules::EnemyShotGate`;
    over 500 ms late it is dropped, `late dropped`, never fired from where the mirror stands now), then replayed through
    CreateBullet (kind 2) from the host's own muzzle place (the mirror stands there
    within centimetres at that moment; an origin moved onto a mirror's drawn hand started inside MULE bodies in run
    `102419`), refused beyond 30 m from the mirror. The muzzle on a pull's first round; the magazine id passes with
    `enemy_shots_sound=1` (sound_sync does not mirror enemies). The partner's own replays go first within the 8 a tick.
    `[shots] enemy replay #n: shooter XXXXX, weapon .. mag .. attack 0x.., .. m/s, .. m from the mirror, muzzle` (the first
    20, in hidden runs 200, then every 20th) and `[shots] enemy replayed n=.. (muzzle .., mag ..), late dropped .. ...; replay hits on our Sam:
    enemy kept .., dropped ..` every 5 s. The host stops resending an old pull when sending stops.
  - Damage: the drop at the bullet (7.5) keeps every replay off this world's vehicles, cargo, structures, BTs and NPCs; on our
    own Sam the "shots" filter decides by kind (`erules::ReplayPolicy`): an enemy round is **kept** with `enemy_shots_hurt=1`
    (the victim decides: in P1a nothing else can bring the host's rounds to her, she is a cutscene-actor body on the host) and
    dropped with 0. A part of her (an armor plate) follows the same rule (the same `erules::ReplayPolicy`, nohurt included).
- **Faults**: any SEH in the tick logs `[enemy] FAULT in <step>: enemy sync stopped`, releases everything and clears bit0 for
  the session; a fault in the pose functions turns every enemy pose slot off.

### 8.2 Byte checks

26/26 in every run: the probe's 23 (15 sites, the Occupied and Required vtables, 6 DSMuleGameActor vtable entries), the
realized placement `0x142AD21E0` (checked, never called), the re-path `0x142AE5000` (`40 53 48 83 EC 20 80 B9 88 10 00 00 00 48
8B D9 74 33`, read in Ghidra; only variant B calls it) and DSMuleGameActor vt+0x138 = `0x142DCA0F0`. Plus the census's 22 for
the fingerprint, the shots' own and the Humanoid handler's (`[npcdmg]`, 1/1).

### 8.3 The gate: `two-games-enemy-mirror`

Launcher Local test files (round 16: `ini enemy_sync=1` only; since round 16b no `ini` line: the files exactly as the
launcher writes them, enemy sync on by its `host_save=1` default). Both at the MULE Camp Southwest of Lake Knot City, every
MULE resurrected on both, both Sams invulnerable and shielded (`enemy_probe 5 1`); census tags 1-4; both screens frame the same
MULE (`enemy_look 0 12` on the guest, `enemy_look -1 12` on the host: the one she faces); the guest walks into the camp; the
host fires into the air (his camp comes at him about 20 s later); stills of the newest shooter; the damage policy
(`enemy_shot_test 0 1` nohurt, then `enemy_shot_test 0`, guest unshielded, dev_god 0); the lease (the guest to the
Craftsman, 450 m). Eight runs (`build-testenv\runs\`), every one with saves unchanged, restored, 0 game-server writes, 0 cloud
uploads, the online gate on, no FAULT, no byte check failure, no OTGC write:

| Run | Result | What it showed |
|---|---|---|
| `20261124-101807` | FAIL (step 7) | sync on both (count 150, ids `40624894`, guids `55AD7620`, equal), 10 mirrors on; stills 01 / 02 matched; `enemy_look -1` found no enemy in her view after the host aimed up (a fallback added) |
| `20261124-102043` | FAIL (step 8) | the first enemy shots: host sent 3, guest replayed 3 (muzzle 3, mag 3); the shot test's 70 m/s round never reached her (the test now uses a fast round) |
| `20261124-102419` | FAIL (step 8) | the test's 3 rounds from the mirror's hand hit a truck between them (the test now fires from 3 m in front of her on that line); 8 of 11 replay hits on NPCs (origins on the drawn hand: now the host's muzzle place) |
| `20261124-102741` | **PASS** | census p95 0.40 / 0.07 / 0.21 / 0.33 m; shot test kept 40, health 1000 -> 952.4, nohurt dropped; 20 mirror on / 20 released; stills 03: the alerted mirror posed with its rifle up, then turning to run (no flash caught) |
| `20261124-103118` | **PASS** | census p95 0.05 / 0.19 / 0.16 / 0.15 m; 03 stills looked back at her (the camera kept its yaw after the teleport) |
| `20261124-103427` | FAIL (step 8) | the nohurt test right after the kept one: no damage message (she was staggered: the order is now nohurt first); 03: a mirror crouched in cover, as on the host |
| `20261124-103810` | FAIL (step 9's goto: "a scripted scene is running" after her hits) | every other step passed; all 20 mirrors released by range before the goto settled (the goto is optional now) |
| `20261124-104304` | **PASS** (final code) | below |

Final run `20261124-104304-two-games-enemy-mirror` (166 s of steps):
- `sync on (host|guest): count 150, fp ids 40624894 guids 55AD7620 (equal: key = actor id)`; 10 mirrors on within 3 s.
- The guest's driver error (its own place against the rendered host place, sampled at 2 Hz per mirror) in 5 s windows: p50
  0.00-0.13 m, p95 0.06-0.67 m (the high end in the alert windows, max 1.12 m there), a single 10.97 m sample right after the
  resurrect re-entry; set calls 150-310 a second for 10
  mirrors, teleports 0 after entry. Poses n=6 at 15-22 applies a second each.
- Census (`tools\testenv\enemy-compare.py`, the enemies realized on the guest within 250 m of her, by GUID, against the host
  at the same moment): **tag 1 p50 0.16 p95 0.44 m, tag 2 0.33 / 0.56, tag 3 0.04 / 0.07, tag 4 0.05 / 0.36** (10 each);
  out of her world within 1000 m (both state 0, far placement): p50 0.00 p95 0.00 (43-47 each; one patrol at its turn up to
  54 m). Without far placement P0 saw 12-17 s of patrol lag (section 3.4).
- `mirror attacks: shots 0 (dropped 0), hits on our Sam dropped 0 (seen 0), kept 0; hits on mirrors dropped 0` in every run:
  a frozen mirror never fired, never hit her. `rcx matched` was not exercised (no hit reached a mirror: the drop at the bullet
  takes replays first; she never shot one). The review moved the mirror guard for bullets to the drop at the bullet and added a
  gate step (her rifle at the nearest mirror, `bullet hits on mirrors dropped at the bullet [1-9]` required); the enemy shots'
  sent / replayed lines, a posed mirror and a driver error window under 1 m are required too.
- Enemy shots: the host captured MULE rounds (`weapon 63 mag 105 category 87 attack 0x57`, 70 m/s, and `weapon 92 mag 144
  category 121 attack 0x191`), sent 41 (pulls 41); the guest received 41 and replayed 40 with the muzzle and the magazine
  id (1 refused: its shooter was no longer a mirror), 0 late, origins 1.6-2.4 m from the mirror's root. **At the bullet, 31 of
  the 37 replay hits in her world were on NPCs** (dropped): the rounds aimed at the host (a body without bullet collision on her
  side) end on a MULE; whether on the shooter itself (its gun at the muzzle place) or on another mirror is not logged. That
  would cut a tracer short and may be why no still shows one (8.6). **Round 16b logged each replay's first hit: it is the
  partner body (the host, whom the MULE aimed at; a `DSControlledEntity`), not the shooter (8.9).**
- Damage policy: `shot test: ref match=1 dropped (nohurt) amount 40.0 health 1000.0 -> 1000.0`, then `ref match=1 kept
  (enemy_shots_hurt) amount 40.0 health 1000.0 -> 952.4`.
- Lease: 20 `mirror on`, 20 `lease released` (10 `taken` when the resurrect reset bit 14, re-entered at once; 10 `range` when
  she went to the Craftsman), no id released twice; `sync: ... mirrors n=0 frozen=0`, `sync off`.
- Stills (contact sheet: the round 16 scratch folder `mirrors\sheet.png`): 01 and 02 on both screens show the same MULE at the
  same place in the same stance (01: walking away toward the "Ceramics" tent; 02, 12 s later: at the corner between the two
  buildings, rifle in hand, walking right) in runs `101807` and `104304`. In the alert the mirrors show the host's combat
  poses (rifle up and turning to run, run `102741`; crouched behind a barrier, `103427`). **No still caught a MULE's muzzle
  flash or tracer on the guest's screen**: the framed MULE had stopped firing (`103118`, `103810`) or the shooter stood
  behind a rise (`104304`). The logs show every round replayed with its muzzle; the visual proof is open.

### 8.4 Also measured

- The resurrect (`dev_enemies 1`) clears bit 14 on every MULE: the guest forgot all 10 mirrors (`taken`, nothing posted) and
  froze them again on the next tick.
- The host's own MULE hits on his Sam name a `DSWeaponEntity` with amount 0.0 (the observer, before his shield): the guard
  matches a weapon entity through its instance's holder.
- The kept enemy round (attack 0x56 from the test) did 40 to her Sam (health -47.6 for three rounds: the game's own spacing).
- The census's own byte checks (22/22) ran in every gate run; the separate `two-games-enemy-census` run was not made (the run
  budget went to the gate).

### 8.5 Debug actions and keys

`enemy_sync 0|1|2|3|6|7`, `enemy_look <id | 0 | -1 | -2 | -3 | -4> <m> [aim height m]`, `enemy_shot_test <id | 0> [1]`; round
16b: `shots 4 <deg> 2` (aim, then 2 s of fire while the aim follows), `shots 10 <m>` (the enemy replays' start along the shot)
(docs/debug-actions.md "Enemy sync", "Shots"). Keys (all in the scenario ini allow-list and the launcher overrides; the launcher writes none):

| Key | Default | Meaning |
|---|---|---|
| `enemy_sync` | on with `host_save=1` (round 16b), else 0 | the mirrors (both games) |
| `enemy_freeze` | occupied | the only lever built |
| `enemy_mirror_radius` | 250 | the 10 Hz class and the mirrors (m) |
| `enemy_pose_count` | 6 | posed mirrors (0..8) |
| `enemy_place_far` | 1 | the far placement (variant A) |
| `enemy_shots` | 1 | the enemy shots |
| `enemy_shots_hurt` | 1 | an enemy round replayed on her may hurt her own Sam |
| `enemy_shots_sound` | 1 | the magazine id on an enemy muzzle shot |
| `enemy_mirror_guard` | 1 | the guards drop (0: counted only) |

### 8.6 The default

Round 16 left `enemy_sync` at **0** (no still of a MULE's shot on the guest's screen, no Local test with it on). **Round 16b
turned it on with `host_save=1`** (`config.cpp`: `enemy_sync = host_save` unless the key is given, like `world_sync`; its
install refuses without `host_save` anyway; `enemy_sync=0` turns it off): the stills of a MULE firing on both screens at the
same moment, the first-hit diagnosis, the guard on her own rounds and the launcher Local test with it on by default are in 8.9.
There is no launcher option yet (`share_enemies` is still to come); the launcher's files write no `enemy_sync` line. What the
players see is asymmetric until P2 (8.7): the host decides every enemy's fate.

### 8.7 Risks and gaps

**What the guest will see (P1a)**: no MULE ever sees, chases or shoots HER: the host's MULEs perceive only the host, and her
own copies near her are frozen mirrors that do nothing of their own. She can walk through a camp untouched, yet the host's
MULE rounds aimed at him are replayed in her world and can hurt her when they cross her (`enemy_shots_hurt=1`). Her rifle,
melee, tackle and bola hits on a mirror are dropped (the host decides until P2); strand tie-ups on a mirror are untested.
Her own MULEs beyond 250 m run their own AI (placed at the host's places while unrealized) and are snapped (teleported) to
the host's place when they come within the radius. The host's melee, thrown spears, tackles, cargo theft and truck rams never
reach her (only bullets are replayed), while whatever her frozen copy would do is dropped. Round 16 meant to keep it off
until P2; round 16b turned it on with `host_save=1` (8.6, 8.9) and the README says what each player sees.

- Bandwidth: about 28 KB/s at a camp (SEP1 is 75% of it).
- The host's partner body must be in his world for anything to be sent (another region: nothing mirrored, the guest's copies
  run their own AI).
- A mirror whose host copy is unrealized keeps its own idle while it slides along the patrol.
- Hits on mirrors are dropped (P1a): her shots do nothing to the host's enemies yet (P2).
- Unrealized copies: far placement holds p95 0 m, but one patrol at a turn showed up to 54 m in a census.
- The host's MULE rounds also land in her world: their first hits are the partner body (8.9; dropped at the bullet), about 1
  in 10 a frozen mirror standing in the line of fire (on the host such a round may pass its own side's MULEs), and their noise
  may alert MULEs there (frozen mirrors cannot act on it; others can).
- Demens rounds are lethal: with `enemy_shots_hurt=1` she can die in her own world (her save is not the session's).
- A release held by a cutscene keeps the mirror's entity in the guards' set until the hold ends; after a world change that
  entity may already be freed (a reused address would have its hits dropped for those seconds).
- A MULE seated in a vehicle is refused by its parent chain's class name; whether a seated MULE's chain names the vehicle is
  untested.

### 8.9 Round 16b: the enemy shots on the guest's screen, the guard on her own rounds, the default

**Diagnosis.** The drop at the bullet now names each enemy replay's first hit (`[shots] enemy replay #n's first hit (shooter
XXXXX): the shooting mirror itself | another frozen mirror | the partner body | our Sam | something else (<class>; receiver ..,
entity byte ..), .. m from its start (.. m along the shot), .. ms after it (.. m of flight at .. m/s)`; the first 80 in hidden
runs, 8 for players; totals in `enemy replayed` and the `enemy_sync 0` status: `first hits: shooter n, other mirrors n,
partner body n, our Sam n, other n`). The game thread notes every enemy replay's references, shooter, start and direction in a
ring of 64 (`NoteEnemyReplay`); the hit is matched to it by reference. Three gate runs logged it:

| Run | shooter | other mirrors | partner body | our Sam | other |
|---|---|---|---|---|---|
| `20261124-122942` | 0 | 0 | 58 | 0 | 0 |
| `20261124-123457` | 1 | 2 | 35 | 0 | 0 |
| `20261124-124658` | 0 | 6 | 56 | 0 | 1 |

The "31 of 37 on NPCs" of round 16 were the partner body: the host's MULEs fire at the host, and on her screen their rounds
fly to the host's body and stop on it (5.3-18 m along the shot, the flight time matching the distance at 70 m/s; 83-101 m for
a far shooter). The shooter itself was the first hit once in about 160 (run `123457`, 1.0 m from the start: its body over the
muzzle place). So no start past the shooter's collision is needed: `shots 10 <m>` (hidden runs, 0..3 m along the shot) is
there for a test and the default stays 0. A round that meets another frozen mirror first (about 1 in 10) stops there on her
screen.

**The stills.** Run `122942` showed tracers on her screen, but the shooter stood behind her own head; run `123457` (her camera
steered at the mirror) caught MULE 0202F with the tracer leaving its rifle, while the host's camera kept the place the MULE
had left; run `124017` framed both, but the steered aim put the shooter at her screen's centre, behind her backpack (she holds
no gun there). The final scenario: at a MULE's 10th replayed round the host takes his newest **sent** shooter at 12 m
(`enemy_look -2` on the host), she stands 3 m to his right and then 10 m from the mirror he faces (`enemy_look -1`), both
cameras are steered at it and follow it (`enemy_look` now follows the entity for 60 s: `shots::SetAimEntity`; hers 12 deg to
its left, `shots 9 12 0`), then 16 pairs of stills, host and guest in turn (each pair within about 0.1 s). Run `124658`, pair
a: on the host's screen the orange-shield MULE fires at him, a white tracer leaving its rifle; on hers, at the same moment, the
same MULE aims at the host and its replayed round is in flight to him (she replayed only 0202F's rounds then, each first hit
on the partner body at 7 m). Her `enemy_look -1` fell back to the mirror nearest the host (02031: none within 25 deg of his
heading), so her camera was steered 12 deg left of a neighbour; 0202F was in her view all the same. Contact sheet: the round
16 scratch folder `prove16b\enemies.png` (10 panels).

**The guard on her own rounds.** A drop at the bullet on a frozen mirror now says whose bullet it was (`our Sam's own round` /
`a replay's round` / `another's round`, each logged up to 8 times) and the status counts them apart: `bullet hits on mirrors
dropped at the bullet n (our Sam's own rounds n, replays n)`. The scenario requires her own rounds' drops (she fires nowhere
else in it) and their log line: run `114315` had passed the old step on replays alone. Her aim: `enemy_look 0 5 1.0` (5 m, the
aim 1 m up: a MULE in cover crouches) and `shots 4 2 2` (steered, then RT held 2 s while the aim keeps following), twice. Run
`124017`'s single burst at a crouched shooter (aimed 1.2 m up) missed; run `124658`: 9 of her rounds dropped (8 logged), still
`04-guest-fires-at-mirror` (her crosshair on the crouched mirror).

**The shot test.** Its 3 rounds now fly level at her chest from 2 m, on the mirror's side and 50 deg to either side of it:
run `122942`'s three rounds from 3 m along a sloped line toward the mirror's hand never reached her (none hit anything; likely
the sandbag wall), as in run `113308`.

**Runs** (five, one at a time, every one with saves unchanged, restored, no violation, 0 cloud uploads, 0 game-server writes,
the online gate on, no FAULT, byte check failure or OTGC write):

| Run | Result | What it showed |
|---|---|---|
| `20261124-122942-two-games-enemy-mirror` | FAIL (the nohurt shot test: no damage message) | the first-hit diagnosis (58 of 58 on the partner body); the same MULE 0202C taken by both looks; host: its flash and tracer; guest: its tracers, the MULE behind her head |
| `20261124-123457-two-games-enemy-mirror` | FAIL (two optional steps: a second wait for a replay, the 450 m goto's settle) | every required step; guest: MULE 0202F's tracer leaving its rifle; the shot tests (level, 2 m) passed; the guard: 13 of her own rounds dropped |
| `20261124-124017-two-games-enemy-mirror` | FAIL (the guard: her one burst at a crouched mirror missed) | both cameras steered: host: the shield MULE's flash and tracer; guest: its tracers from behind her backpack |
| `20261124-124658-two-games-enemy-mirror` | **PASS** (189 steps) | the launcher's files, no `ini` line: `enemy_sync=1 (default: on with host_save=1)` in both; pair a above; the guard 9 own rounds; shot tests 1000 -> 1000 (nohurt) and 1000 -> 952.4 (kept); the lease `mirrors n=0 frozen=0` |
| `20261124-125108-two-games-launcher-local-test` | **PASS** (100 steps) | the Local test as the launcher writes it, enemy sync on by default in both (`sync on (host|guest) ... (equal`), byte checks 26/26 |

The 450 m goto's settle wait (optional and flaky: run `123457`, "NOT settled after 25 s" with her already there; `103810`)
is now the teleport alone and 8 s: an optional step's failure fails the run; the lease check after it is unchanged.

**Still open**: no still shows a MULE's muzzle flash itself on her screen (her stills show its tracers in flight; the host's
show the flash); a round on her screen can stop on a frozen mirror where the host's passes its own side; BTs, P2 (hits on
enemies) and P1b; a launcher option for `enemy_sync`; the other two-game scenarios now run with enemy sync on wherever
`host_save=1` and were not rerun this round (only the Local test).

### 8.8 What P2 and P1b need next

- **P2 (hits and outcomes)**: the Humanoid filter already sees every hit on a frozen mirror and tells our Sam's apart; turn
  "attacker is our Sam" into a HIT message (SCB1 on channel 6) with the attack id and the hit point; the host applies it to
  its own copy (7.x of the plan) and sends the outcome (alert, faint, death) back; the guest shows the outcome on the mirror
  (released to the game for a death animation, or kept frozen and posed). The `rcx` match is still to be confirmed by a run.
- **P1b (ownership)**: the MULEs near the guest owned by the guest's game (their AI perceiving her), the host's copies
  frozen as mirrors: the same SEN1 / SEP1 / lease code with the roles per enemy (the `owner_epoch` and `kOwnerGuest` bits are
  in SEN1 already), a hand-over rule with hysteresis (`erules` gains it), and the enemy shots in both directions.
