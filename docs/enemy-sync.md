# Enemy sync (P0 done: the census; round 15: R1 foundations, the P1-0 freeze probe, ship A: the players' shots; round 16: the drop at the bullet, P1a mirrors and the enemy shots; round 17: P2, her hits and the outcomes; P1b, ownership by proximity, off by default)

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
rounds are replayed on her screen from the mirrors (and may hurt her own Sam). **Round 17** built **P2** (section 9,
`enemy_hits`, on with enemy sync): her hits on a mirror are dropped in her world and sent to the host, which applies them to its
own MULE with the game's own damage path; the outcome (faint, sleep, restrained, dead) comes back and her mirror lies with the
host's. **P1b** (section 10, ownership by proximity) is built and off by default (`enemy_owner`): its gate has not passed.

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
- **Outcomes**: EDSNpcDefeatType {0 Dead, 1 Faint, 2 Sleep, 3 Restrained, 4 Dying}, EDSMulePhase {0 Normal, 1 Return, 2 PreCaution,
  3 Caution, 4 Evasion, 5 Alert} (round 17, 9.0), EDSMuleStance {Sneak, Caution, Alert}, EDSMuleType {Normal, Friendly, Demens, Warriors_Particle}.
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
| P2 | damage and outcomes: on the non-owner a detour on DSMuleDamageComponent MsgDamage (`0x142B020F0`) or Humanoid MsgDamage (`0x143744380`) drops the mirror's damage and sends HIT {key, attack id, amount, hit point, direction}; the owner applies it with `0x14298AC90(0, id)` and the single-receiver query; OUTCOME is host-final, ordered Normal < Restrained < Sleep = Faint < Dead (the first terminal state wins); phase synced escalation-only (`SetNpcIndividualPhase`) | a debug `enemy_hit <attack id>` on the nearest mirror: HIT, HITRES, OUTCOME on both. **Built in round 17 (section 9), changed**: OnHit `0x142349A10`, not the Humanoid handler; the bullet's own path (M1), not the query; down is not terminal, a mirror is never released for a death animation (9.0) |
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

The area is `enemies` in `tools/testenv/test-areas.txt` (`native/mod/enemy_census.*` and the other `enemy_*` files the later
phases do not name; since the round 17 review P1a, P2 and P1b are the areas `enemy-mirror`, `enemy-hits` / `enemy-compat` and
`enemy-own`).

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
| `enemy_host_guard` | 1 | round 20 M0 (8.10): no mirror of a copy the host has unloaded or hidden, or far from his Sam |
| `enemy_entry_err_max` | 0 | M0: no mirror when the host's place is this far from ours at entry (m; 0 off; 5..1000) |
| `enemy_host_hide` | 0 | M0: a mirror released because the host hid its copy is hidden and asleep here too (untested: off) |
| `enemy_host_far` | 1500 | M0: a mirror given back while the host's Sam is this far from it (m; 0 or 300..10000) |

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
- Hits on mirrors are dropped (P1a): her shots do nothing to the host's enemies yet (P2). Round 17: P2 sends hers to the host
  (section 9).
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

### 8.10 M0 "no statues" (round 20; `session.ini enemy_host_guard`, `enemy_entry_err_max`, `enemy_host_far`, `enemy_host_hide`)

**The user's report.** The host fainted and the game moved him about 2 km away. His MULEs at the camp were unloaded on his PC,
and her copies of them stayed frozen in mid-air with no pose (`poses n=0`), refusing her shots (`unrealized`).

**Why.** A mirror was released only by silence (`kSilenceFarMs`, 3 s with no SEN1 frame for it) or by range. The host's SEN1
sender stops sending a MULE's frames about 0.6 s after his teleport, while his world loads, so the guest released them about
3.7 s later (run `221023`), and a copy the host still sent while unrealized or hidden there kept its lease.

**The rules** (`erules`, pure, unit-tested in `st_enemy_rules_tests`):
- `Eligible` (entry) refuses `host_unrealized` and `host_hidden` (the newest SEN1 sample's bits), `entry_far` (the host's place
  more than `enemy_entry_err_max` m from ours at entry; default 0, off) and `host_far` (the host's own Sam more than
  `enemy_host_far` m from it, default 1500 m; 0 off).
- `entry_far` is off by default since the round 20 review: M0's own releases give her MULEs back alive, they chase her and drift
  from the host's copies, and a 30 m wall then refused every re-entry after the host came back (the two worlds disagreed for
  the rest of the fight; a downed MULE that stood up after a range release was never laid down with the host's again). The
  198 m / 114 m entries that motivated it were the host's unrealized copies, which `host_unrealized` already refuses. The gate
  now also brings the host back and requires the mirrors again.
- `Lease` releases `release_host_unrealized`, `release_host_hidden` and `release_host_far` (in that order, after the
  existing checks), all only with `enemy_host_guard` on (default 1). The log: `[enemy] lease released id=XXXXX: host
  unrealized | host hidden | host far (...)`. A released mirror is given back to her own game (it is alive there). With
  `enemy_host_hide=1` (default 0), after `host hidden` her copy is also hidden and put to sleep here until the host's frames
  say shown again or stop (`KeepHostHidden`, 3 s), and only when the release really posted (`HideOnRelease(v, on, posted)`):
  a release a cutscene holds stays frozen in `g_pending`, and hidden while still frozen its weapon flag (+0x268) could never be
  restored, so it would walk but never fire again (the run `20261124-151942` bug). Off by default because no run has hidden a
  mirrored MULE on the host yet (the gate releases by `host far`; `enemy-mirror` released none). To prove it: a host-side
  hide of a mirrored MULE, then `released hidden`, `shown and woken again` and a round fired by it.
- What we hide is shown again on every way out: the host's frames, the switch off (`enemy_sync 2`), a fault-stop (`Stop`), a
  link change and a reset; on a new world (the world counter changed) the old world's hidden copies are forgotten without a
  touch (an address and id can repeat after a reload). The same holds for the P2 dead copies (`g_dead_hidden`), which before
  the round 20 review stayed hidden while the switch was off or after a fault.
- P1b asymmetry (to settle before `enemy_owner` ships): M0 guards only the guest's mirrors of host-owned MULEs. With
  `enemy_owner=1` the host's frozen copies of guest-owned MULEs get no host-far / unrealized / hidden guard (the guest's own
  faint or teleport would leave them standing on the host's screen). Round 21 (13.5): the host's follower guard
  (`enemy_follow_guard`) hides such a copy while her frames say unrealized or held there for 1 s, or none came for 3 s.
- The host's Sam is the partner body on her screen (`in.body`), known at once after his teleport, so `host far` is the quick
  path; the SEN1 bits are the slower one for a copy he unloads or hides near himself.

**Status:** `enemy_sync 0` adds `[enemy] host guard (M0): on|off, entries refused n (host unrealized / hidden / far), mirrors
released n (host unrealized / hidden / far), hidden here for the host now n; entry err max 0 m, host far 1500 m`. `[config]
enemy_host_guard=1 enemy_entry_err_max=0 enemy_host_far=1500 enemy_host_hide=0` at start. The launcher writes none of the keys.

**The gate `two-games-enemy-statues`** (both at the MULE camp south-west of Lake Knot City, every MULE resurrected, mirrors up;
the host teleports to Mama's Lab, 2.03 km): every lease released by the guard (`host far`, `host unrealized` or `host hidden`,
not silence) within 3 s of the command; 2 s after the first release no frozen mirror; 15 s later still none; no HITRES refused
as unrealized. Since the round 20 review the host then comes back to the camp: `mirror on id=` again within 20 s, mirrors
`n>=1`, no `entry_far` refusal (still 04). Not run yet with that step.

| Run | Result |
|---|---|
| `20261124-220850` | fail: the status-line regex did not match (scenario fixed) |
| `20261124-221023` | pass, but by silence: released 3.66 s after the teleport (388 re-entries refused; `n=0` then and 15 s later) |
| **`20261124-221751`** | pass with `enemy_host_far`: the goto at 66.344 s, all 10 leases `host far` at 66.687 s (0.34 s); `n=0` at +2 s and +17 s; 572 (later 5052) entries refused, 10 released |

Stills 02 / 03 of `221751`: her own game's MULEs live and attacking her (one grappling her with the electric effect, another
running at her with red eyes). No statue. `two-games-enemy-mirror` (`20261124-221952`, pass, 189 steps) with the guard on:
0 refused, 0 released; `enemy-compare.py` tags 1-4 p95 0.16 / 0.29 / 0.13 / 0.08 m: PASS.

## 9. P2: her hits on the host's enemies and their outcomes (round 17)

`session.ini enemy_hits` (default on with `enemy_sync`), `enemy_combat.{hpp,cpp}`, the SCB1 codecs in `enemy_wire.*`, the
pure rules in `enemy_rules.hpp` (`HitVerdict`, `HitDedupe`, `ToLocal` / `ToWorld`, `HostSerial`, `RateLimit`, `ApplyGate`,
`OutcomeOf`, `MergeOutcome`, `DeadRelease`) and `shotw::MirrorBulletRule` in `shot_wire.hpp`. Both games need it (SEH1 parts
bit1 "hits": `[link] combat parts: ... hits on`).

### 9.0 Corrections to the plan and to this document (r-p2, verified in Ghidra and in runs)

- **The Humanoid handler is the wrong hook for MULEs.** A MULE is a `DSControlledEntity`: its MsgDamage goes to its components
  (DSMuleDamageComponent's handler `0x142B020F0`, reached through the thunk `0x142B2D250`), never to the Humanoid handler
  `0x1437393D0`. That is why `rcx matched` was 0 in every P1a run. `npc_damage` stays as it was; P2 does not use it.
- **One place sees every hit given to an entity**: Entity OnHit `0x142349A10` (the receiver's vt+0x10 for every entity
  receiver, receiver = entity + 0x38; it jumps to `[0x147BC4E70]` vt+0x40, QueueDamage). Bullets (BulletDamageA / B) call it
  directly; the attack sphere query (melee, tackles, vehicle rams) and the BT reaction's single delivery `0x14298BAB0` queue a
  job that calls it. Area damage `0x1429361D0` and four other direct MsgDamage senders bypass it (the MULE handler sees those).
- **EDSMulePhase** has six values: 0 Normal, 1 Return, 2 PreCaution, 3 Caution, 4 Evasion, 5 Alert (section 2.3 lists four);
  **EDSNpcDefeatType** also has 4 Dying. P2 does not sync the phase (its field on a realized MULE is P1b-0's).
- The plan's 4.4 row for P2 ("first terminal state wins", "released for a death animation") is replaced by 9.4: down is not
  terminal (a fainted MULE gets up), dead is sticky, and a mirror is never released for a death animation.
- The local player's actor id is **0** (the registry's vt+0x40 and P+0x20 of a context owned by Sam, run `141609`); a context
  with no owner, or the partner body's, carries 0x1FFFFF.

### 9.1 The guest: her hit on a mirror becomes a HIT

- **G1** (the drop at the bullet, `shot_sync` JudgeBullet): her own round (not a replay) on a frozen mirror or a part of one
  passes to the game while the hits are captured and the receiver is an entity receiver whose vt+0x10 is OnHit
  (`MirrorBulletRule` pass_capture; `[shots] our Sam's own round on a frozen mirror passed to Entity OnHit`): the DamageInfo is
  built, her impacts and the bullet-hit message to her Sam stay. Every other bullet on a mirror is dropped as in P1a.
- **G2** (a detour on OnHit, any thread): the receiver's entity and its +0x70 parents (4 levels) against the frozen mirrors.
  A mirror's hit is **always dropped** (no QueueDamage, ApplyDamage, stats or telemetry in her world). It is hers when the
  attacker is our Sam, his weapon or a part of him (her rounds name her `DSWeaponEntity`), the vehicle she drives (untested),
  or P+0x20 is our player id. Hers, with an attack a HIT can carry (1..0x18D, not 0x180: her aim sends AimTarget 0x180 to the
  MULE she aims at, about 30 a second), goes into a lock-free ring of 64.
- **G3** (a detour on the MULE damage handler `0x142B020F0`): the direct senders' hits on a mirror, the same way (kind direct).
  No direct sender reached a mirror in any run.
- **The game thread**: the 300 ms dedupe (the same key, serial and attack; the job path delivers one hit several times: 21
  dropped in run `142244`), the lethal policy (`enemy_hits_lethal`, default 0: a round whose attack does damage is refused;
  9.9), the values' ranges (`HitValuesOk`, 9.9), the point turned into the mirror's yaw frame (refused beyond 8 m), then SCB1
  HIT messages (up to 12 a message) on channel 6: `[enemy] HIT sent #n id=XXXXX attack 0x.. kind
  bullet|sphere/job|direct|test part P ...`.
- **Her own feedback**: her impacts, tracer and the bullet-hit message stay; the flinch and the fall come from the host's pose
  frames (SEP1). Missing (documented): her copy's own hit reaction, the MULE's pain voice, the damage-dealt message to her Sam,
  her HUD's markers over a downed mirror. No hit marker was seen in her stills (02 of runs `142244`, `142649`).

### 9.2 The wire: SCB1 on the reliable channel 6

Header 16 B: `'SCB1' | u8 type | u8 version 1 | u8 n | u8 0 | u32 guids_fp | u32 seq`. Types: 4 HIT (n 1..12, 80 B each: key,
hit_seq, attack, kind, flags, serial, the context's b12E / b180 / v130 / power / v188, part, amount, mult, a20, the point in
the mirror's frame (f32, |each| <= 8 m), the normal (q15), the impulse (x100), t_ms, her distance in dm), 5 HITRES (n 1..64,
12 B: key, hit_seq, result 0 applied .. 10 hidden, 11 lethal, 12 values, owner mode, defeat now), 6 OUTCOME (n 1..32, 24 B:
key, defeat 0 none / 1 faint / 2 sleep / 3 restrained / 4 dead / 5 other down, cause 0 host / 1 guest / 2 resync / 3 revive,
ver, state +0xEA0, downbits (+0xEA4 >> 19) & 0x3F, phase 0xFF, the host's outcome epoch (9.9), health, stamina, the last applied
hit_seq), 7 PHASE (reserved, refused), 8 RESYNC
(n 0). Strict decoders (`st_enemy_wire_tests`). Nothing is sent before both beacons carry the hits part and mirroring is on; a
fingerprint mismatch is refused `world`. SEN1 state bits 2-4 carry the host's defeat as well. Her rifle at 10 rounds a second is
about 1 KB/s.

### 9.3 The host: applying a HIT (method M1, `enemy_hit_path=direct`)

`erules::ApplyGate` (host, mirroring, hits on, the same fingerprint, a valid attack, no test HIT outside a hidden run, the
values within `HitValuesOk`'s ranges, not lethal while the host's `enemy_hits_lethal` is 0, the actor realized, not dead, not
hidden / asleep / held, at most `enemy_hit_rate` (30) a second per enemy and 60 in all, the point within 3 m of the root), then on the
game thread under SEH, as BulletDamageA does for a bullet:

1. `recv = entity + 0x38` with its entity byte set and vt+0x10 OnHit (else `receiver`).
2. `ctx = 0x14298AC90(owner, attack)`, `P = 0x14294DDF0(ctx)` (= ctx + 0x120), P+0x0C and P+0x18 (the resource) checked.
3. P stamped from the HIT: the serial (`HostSerial`: `0x5C000000 | guest serial`, or `0x5D000000 | hit_seq`), b12E, v130,
   b180, the power, v188; in the credit mode P+0x20 = the player's attacker id (0).
4. A 0xB0-byte DamageInfo from `0x142314190(DI, Q(ctx+0xE8), &[0x147BC3B80], amount, &impulse, a20, -1, part)`, the point
   turned into the host copy's frame at DI+0x80, DI+0x98 = 1, the normal, the multiplier.
5. `recv->vt[0x10](recv, DI)` (OnHit -> QueueDamage -> ApplyDamage: a bullet's path), then `0x142463390(DI + 0x28)`.

`[enemy] HIT applied #n id=.. attack .. part .. owner credit (ctx .., serial 0x5C......, attacker id 0x1FFFFF -> 0x0) ...
delivered`, HITRES, and two ticks later `[enemy] HIT effect id=..: health a -> b, stamina a -> b, sleep .., restraint ..,
state s -> s`. M2 (`enemy_hit_path=query`, partner_tackle's attack sphere query on the point, 0.25 m, only that MULE) is the
fallback.

**enemy_hit_owner** (P2-0 B): `credit` (default: the partner body owns the context, so the falloff and the AI's attacker are
her place and her body, and P+0x20 = the player's id credits the host's Sam), `body`, `none` (owner 0: no attacker), `sam`
(the falloff from the host's place; never the default).

### 9.4 Outcomes

At 10 Hz the host reads +0xEA0 / +0xEA4 (and the component's health / stamina) of its 10 Hz class and of every enemy hit in
the last 30 s, and every tick those hit in the last 3 s (the damage component cached per entity, checked by its vtable and its
entity): `OutcomeOf` (3 / 5 dead; 4 faint, sleep or restrained by bits 19 / 20 / 21, else other down; anything else
none). A change bumps its version and sends an OUTCOME (cause guest when one of her HITs was applied in the last 3 s; revive
when dead turns back to alive, the only way out of dead): `[enemy] OUTCOME id=XXXXX none -> faint (state 4, bits 0x404C0000;
cause guest HIT #7) ver 1`. The guest merges it (`MergeOutcome`: newer versions only, dead sticky unless revived):
`[enemy] OUTCOME id=XXXXX none -> faint ver 1 (cause guest; ...): the mirror lies with the host's frames`. A RESYNC on hits
on, a new link session and a new world brings every non-none outcome back. Each OUTCOME carries the host's outcome epoch (a
new one each host world or link session, its versions starting again at 1): a new epoch drops her table first.

- **Down**: the mirror stays frozen and posed from the host's frames: it lies where the host's lies and gets up with it. If the
  lease gives it back (range, silence), it is given back alive and stands up in her world (`released while faint on the
  host`); when she returns it is frozen lying again.
- **Dead**: still posed while the host sends (the host poses a dead enemy too, in the 5 Hz half). When the lease ends it is not
  given back alive: Occupied{0} / Required{0}, then hidden and put to sleep (`released dead: hidden and asleep here`); Eligible
  refuses it afterwards. Restored (`shown and woken again`) when the host's is alive again (a revive, a new host epoch), on
  link loss, the gate off, a reset, a world change (the actor gone: forgotten) or a fault. Untested in a run (no lethal round
  was fired; lethal rounds are refused by default).
- **Attribution and the online gate**: in the credit mode the takedown is the host player's (+0xEA8 bit 4, the neutralize
  message and counter, the victim's stats and telemetry): the host's normal game. Her world never applies a hit on a mirror,
  so nothing is credited, counted or uploaded there. Every run: 0 game-server writes, 0 cloud uploads on both games.

### 9.5 Byte checks

`[enemy] hits: byte checks 41/41 ok (onhit, muledmg, stamps, apply, outcome, entry)` in every run: OnHit (24 bytes and its
global), the MULE handler, its thunk and three of its instructions, BulletDamageA's stamps and its two globals, the context,
its parameters, the DamageInfo init (+0x68, +0x78), the handle set / release, the MULE state setter and store, the down setter,
the component's fields (+0x1EC, +0x1F0, +0x1E4, +0x1F8, +0x1E0, +0x314, +0x1DC), DsIsMuleDeadOrFainted, the damage entry's
part / attacker id / serial / attack, DSMuleDamageComponent's vtable entries and the context type. A failure of the MULE
handler's group leaves G3 out; any other turns the hits off (`[enemy] hits OFF: byte check FAILED`).

### 9.6 P2-0: `enemy-hit-probe` (solo)

Three runs; `20261124-141609` gave everything (it failed only on the optional restraint):

- **Her rifle's hits** (C, the watch at OnHit): attack 0x47 (Non-Lethal Assault Rifle), kind bullet, attacker her
  `DSWeaponEntity` (a weapon of our Sam's), P+0x20 = 0 (our id), power 12.5, v130 1.0f, b12E 45 (the weapon id), parts 5 / 2,
  the point valid, the receiver `DSControlledEntity` with OnHit at depth 0. Each round took 153.8 stamina (1000 -> 846.2 ->
  692.3). Her aim sends 0x180 (AimTarget, amount 0) about every 30 ms. The MULE handler saw the same hits after ApplyDamage.
- **The attacks** (A): 0x47 damage 0, stun 200 (non-lethal); 0x01 damage 250 (lethal); 0x117 stun 300; 0x10B stun 200; 0x102
  (bola) nothing; 0x18 (tranquilizer) stun 100; 0x15C stun 100000; 0x0D damage 2000 with the falloff (4.5 / 3.5 m).
- **The owner** (B): a Sam-owned context's P+0x20 = 0 = the registry's player id; the body's and owner 0's carry 0x1FFFFF; all
  three of class `AttackEventContextGame` with the type check passing, power 0.0 in a fresh context (the HIT carries the
  bullet's). The default is `credit`.
- **M1 works** (E then D, credit, direct): a chest hit -153.8 stamina (as her own round), a hit at the component's head part
  (+0x528 = 1) -461.5 (the headshot, x3), the faint at the sixth hit (`none -> faint (state 4, bits 0x404C0000)`, +0x314 = 1).
  M2 (the query) at the chest kept the MULE (DI part 2) and took the same stamina; at the head point (1.62 m) it found nothing.
- **Sleep** (G): 0x18 fills the sleep gauge by 76.9 a hit; at 1000 `none -> sleep` (state 4, bit 20). **Restraint**: 0x102
  through the apply never restrains (state 2 only): the bola's restraint is not its damage message (not P2's).
- The dumps tag 1 (calm), 2 (fainted) and 5 (alerted) are in the run's log for P1b-0.

### 9.7 The gate: `two-games-enemy-hit`

Three runs (hidden-testing.md "Enemy hits"); the last two passed; `20261124-142649` (the final scenario, 108 steps, launcher
Local test files, no ini line):
- The wire: test HIT #1 (0x47 at the mirror's chest) sent, applied (`stamina 1000.0 -> 846.2`), HITRES applied.
- Her rifle: 14 HITs of kind bullet, all applied (`hits: rx 15 applied 15 refused {...0}`), the host's `none -> faint (state 4,
  bits 0x404C0000; cause guest ...)` and hers `none -> faint ver 1 (cause guest ...)` for the same id 02029;
  `enemy-hits.py`: PASS (every hit_seq once on the host, none twice, no refusal, her mirror frozen until her outcome table).
- Run `142244` the same with 11 HITs, two of them attack 0x15E (sphere/job, her Sam's own, at 0.8 m from the MULE: no effect
  on its stamina), and 21 deduped.
- The stills (contact sheet: round 17 scratch `p2\sheet.png`): 01 the same MULE crouched by her on both screens; 02 her burst
  on hers, her replayed tracer on his; 03 a/b the MULE lying in the grass under both crosshairs at (1564.06, 1012.08) on the
  host and (1564.08, 1012.10) on hers; 04 still down 8 s later. No hit marker in her 02.
- Every run: saves unchanged, restored, no violation, 0 cloud uploads, 0 game-server writes, the online gate on both.
- `two-games-launcher-local-test` (`20261124-142959`): PASS with `enemy_hits=1 (default: on with enemy_sync)` and `hits on` on
  both.

What the host's MULE did: after the test HIT (credit: the partner body is the attacker) it went for the partner body and the
host (run `142244`: it fainted where the host had stood); on her screen the mirror did the same.

### 9.8 Risks and open points

- Lethal rounds (refused by default: `enemy_hits_lethal=0` on either game), a dead mirror's release (hide and sleep) and its
  restore, grenades, the bola, melee, strand tie-ups and vehicle rams on a mirror are untested (G2 / G3 capture what reaches
  them).
- Only the nearest `enemy_pose_count` (6) mirrors are posed: a MULE she downs farther away is placed (SEN1) but stands frozen
  on her screen. Down and dead ones are posed at 5 Hz and never take a live one's fast rank.
- The host's MULE perceives the partner body as its attacker (credit / body): it may go for her body on the host's screen while
  her own copy stays frozen.
- A downed mirror released by range stands up in her world until she returns.
- Her HUD shows no marker for a downed mirror; no pain voice; possibly no hit marker.
- Demens use DSMuleDamageComponent [not run]; warriors (DSWarriorDamageComponent) are outside P2.
- The first P2 OUTCOME of an enemy already dead in the host's save carries cause host; a resurrect is a revive.

### 9.9 Review fixes (round 17 review, no run yet)

- **Lethal**: `enemy_hits_lethal` defaults to 0 until a lethal run passes (a kill counts as the host's, in the host's save; an
  undisposed corpse leads to necrosis there). Both games apply their own setting: the guest does not send a lethal round, and
  the host refuses one (`HITRES lethal`) when ITS setting is 0, by the HIT's flag or by its own registry entry for the attack
  (damage +0x24 > 0). Her rifle's 0x47 (the Non-Lethal Assault Rifle) and the tranquilizer 0x18 do no damage: unaffected.
- **Values** (`erules::HitValuesOk`, both games): the DamageInfo amount 0..500, the multiplier 0..4, a20 0..2, the context's
  power (P+0x64 as f32) 0..100, the part below 32 or none. The probe and the gate saw amount 0, multiplier 0.11..1, a20 0.4..1,
  power 12.5..13 (her rifle) and 1 (the test HITs), parts 1..10. Outside: not sent, and refused on the host (`HITRES values`).
  A direct HIT (kind 3, the MULE handler's layout) is accepted within these ranges: the handler's msg+0x20 DamageInfo was read
  in the probe's watch (`at the MULE handler`) with the same attack and parts as OnHit's.
- **Test HITs** (kind 4) are refused on the host outside a hidden run (`HITRES attack`).
- **Late HITs**: a HIT that waited more than 1 s for the link (a full window, a stall) is dropped before sending, and at most 64
  wait (the oldest goes): no late burst of hits.
- **The query path** reports `the query kept nothing` (not applied) when no part of the MULE was in the sphere.
- **The outcome epoch** (OUTCOME byte 11): see 9.4.
- **Pose slots**: a down MULE (not only a dead one) is posed at 5 Hz and never counts in the live ones' fast ranks.
- **The dead release** is restored (9.4).
- **Her copy's vitals** are in her outcome table (`enemy_sync 4`: `; her copy: health .. stamina ..`); the gate requires her
  fainted mirror's stamina still 1000.0 (her world never applied her rounds).
- **The host's outcome read** runs at 10 Hz (every tick only for the enemies she hit in the last 3 s), the damage component
  cached.

## 10. P1b: ownership by proximity (round 17, `session.ini enemy_owner`; per camp and on by default since round 21: section 13)

An enemy near the guest and far from the host is run by HER game: it notices her, targets and shoots at her Sam, and her hits
on it are real (her world's own damage). The host's copy becomes the frozen mirror, driven and posed from her frames, and her
owned MULEs' rounds are replayed on his screen from it. It goes back to the host when she leaves (calm) or by a forced rule
(whatever its phase). `enemy_owner=1` on both games (SEH1 parts bit3 `kPartOwn`); the launcher writes no key (and players cannot
set it: the launcher's session.ini is one-shot). None of the 8 gate runs passed and its steps D and E never ran in a game
(10.6), so the default stayed 0 and `two-games-enemy-own` is `select: probe` (run by name only).

Round 21 corrections (section 13): the unit of ownership is the camp, not the enemy: the host decides per camp
(`erules::CampDecide`) and the per-enemy `HostDecide` of 10.1 no longer runs; a MULE in no camp list is a camp of its own (a
"loose" camp, r 10 m), for which 13.2's distances are P1b's (40 / 15 / 60 m) but the hold is 15 s (not 10), her cap 32 (not 16),
the host-nearer back is new and the damaged rule is gone (`HostDecide` stays in enemy_rules.hpp for `TestOwnProperty` only).
The beacon part is `kPartCamp` (32), not `kPartOwn` (8): this build never sends 8, so it never runs 10.1's per-enemy rules
with a v0.33 partner (13.1). The hand-over (10.2), the wire (10.3) and the byte checks (10.5) are unchanged, except that an
unloaded camp's OWN carries no place and no phase (13.2). 10.1's "her outcomes do not reach the host yet" is fixed (13.4: carried at the return). `enemy_owner` defaults to
on with `enemy_hits` (`cfgval::kOwnerDefaultOn`) since the P1c gate passed (13.7).

### 10.0 Corrections to the plan (plan.md section 8) and to 4.3 (r-p1b, Ghidra and runs)

- The phase is the DSMuleGameActor's `+0x110` (EDSMulePhase 0..5; GetPhase 0x142ECFF7D reads it), set by SetPhase 0x142ACF630
  (exact) and RisePhase 0x142ACE9D0 (max(old, new): the escalation-only lever). SetNpcIndividualPhase is an AI action, not a
  phase setter. No dump probe was needed for P1b-0.
- Camps: the MULE manager `[0x147BF09D8]` +0xD8 / +0xE0 the CP actor ids; a CP (DSMuleCPGameActor) holds its phase at +0x210
  and its members at +0x4C0 / +0x4C8; its push 0x142B37EB0 escalates members with 3 / 5. Run `20261124-151238`: the camp SW of
  Lake Knot City sat at PreCaution (2) with its MULEs at Normal, so "alerted" is Caution (3) and up for a camp and for a MULE
  (`erules::Calm`: phases 0..2; `CampAlerted`: 3..5). The first walk logs `[enemy] camps: n=9 of 9 ids (class
  DSMuleCPGameActor), members n=116`.
- "Realized on both" is dropped: the host follows a guest-owned enemy as a frozen mirror when its copy is realized there, else
  as a far follower (vt+0x40 variant A at 2 Hz); it freezes it as soon as it is realized.
- Silence is 3 s; the host's lease never releases a guest-owned copy (only the ownership rules do).
- Take also needs the host beyond 25 m (the take and back rules overlapped).
- No world id is shared: SCB1's header keeps P2's guids fingerprint; the host's combat generation travels in each OWN entry
  and in SEN1's gen_tag.
- The 8 pose slots are shared by capture and apply on both games (`erules::PoseAlloc`, apply first).
- Her camp alerted is NOT a refusal (spec 3.2 step 3 dropped): run `20261124-151942` showed a camp that saw her once stays
  alerted for a minute, which would keep every MULE of it the host's. The guest takes it; her camp's push makes it join.
- Our Occupied{1} also reaches the MULE weapon's handler (0x142B07FB0 -> weapon +0x268 = 1, the weapon update idles while set).
  Every release puts +0x268 back when it was 0 before our freeze and the release left it set (`[enemy] weapon flag
  restored`). In the runs it was always 0 after the release (`weapon268 0` in `taken here`), so no write happened.
- The save: the game refuses a forced save inside the MULE camp (run `151458`: reasons 0x2), so the solo save probe of the spec
  was not run (and solo has no in-world load: `save_guard 3` needs a host session). Instead the host never lets a save capture a
  copy it froze: while any copy is frozen and `saveguard::Busy()` (a save pending, queued, captured or written) it resets
  ownership at once (`[enemy] own reset (the host's game saves)`), and it takes nothing while a save is busy. P1b needs the save
  reads (the `save_guard` byte checks) on the host. [I] a capture in the same frame as the post is not excluded; not run.
  Review fixes: every host-frozen copy counts for this reset (the table's, the followers' and the returns a cutscene holds:
  those are posted at once while a save is busy, and a return is never held by a cutscene while a save is busy).

### 10.1 The rules (`erules::HostDecide`, pure, the host at 2 Hz)

Take (host -> guest) when all: the guest within 40 m; the host at least 15 m farther than she is and beyond 25 m; the host's
copy calm (phase 0..2) and its camp not alerted (or unknown); not damaged (its damage component's health or stamina fell since
the last change, `ecombat::Vitals`), not dead or down; no cutscene on her side, fewer than 3 refusals a minute; the host's copy
not sequence-only, not held, not hidden or asleep, not in a vehicle, no host cutscene, no save busy, the guards in; the 10 s
hold over; she owns fewer than 16; fewer than 4 changes this second; both beacons carry `kPartOwn`. The nearest candidate's
reason is logged: `[enemy] own: no take id=.. (guest .. m, host .. m, phase .., camp .., damaged .., hold .. s): <why>`.
Back by proximity (calm, after the hold): she is beyond 60 m, or the host within 20 m. Forced back (ignores phase, damage and the
hold): refused, forced (`enemy_own`), no OWNACK within 3 s, silence 3 s, beyond
`enemy_mirror_radius` + 20 m (no partner body on the host: beyond any range), unrealized or held by a sequence on her side.
Dead or down on her side (her SEN1 entry's kDead or its defeat bits 2-4, `OutcomeOf` of her copy): it stays hers, never handed
back by proximity (review: the host's copy would get up alive at her place while hers lies); only the forced rules end it. Her
outcomes do not reach the host yet: a forced return of one she killed brings the host's copy back alive (her copy is then
left lying as her world's corpse, never frozen and posed from the host's standing frames; Eligible refuses a dead one). Faint and sleep on her side keep it hers until
it gets up.
Resets (everything the host's at once, a new generation, OWNRESET when the link stays): a world or the link changed, the gate,
`enemy_sync 2`, `enemy_sync 8 0`, a fault, a host save. A reset for a world change also releases the host's followers that
still read as the same in-use MULE frozen by us (a private room, a short not-ready dropout: the same actors). `[enemy] own: kept
by the guest id=.. (...)` every 10 s.

### 10.2 The hand-over

Take: the host freezes its copy first (Occupied{1}, bit 14 read back; else no OWN, hold 10 s), drops it from its SEN1
schedule, shooters and captures, sends OWN (place, yaw, velocity, phase). She checks her copy (a live frozen mirror, realized,
alive, bit 14 ours, no cutscene, under 16), teleports it to the host's place, sets its phase (SetPhase) when it differs,
releases it (Occupied{0}, Required{0} when bit 13 was clear) and answers OWNACK(taken); from then she sends its SEN1 (flag
kOwnerGuest, the epoch byte, its phase, kDamaged) at 10 Hz, its pose (SEP1) and its rounds (SFX1). Back: OWN(host); she freezes
her copy again at once (a P1a mirror from then: its lease's silence clock starts at the freeze) and answers OWNACK(released)
with its final place and phase; the host teleports its copy there (only within 10 m of where its follower stands and at most
3 m below it, `erules::BackPlaceOk`; else `her place refused ...: released where it stands`), raises its phase with
RisePhase(min(hers, Caution)), then releases it (`back here (...; phase raised a->b | kept a; health not carried; ...)`). Two
thinking copies only inside a bounded window (the property test, `TestOwnProperty`: messages delayed up to 6 s, her refusals,
the decisions through `HostDecide`): a reset until she has the OWNRESET, or a back whose OWNACK timed out (3 s) while her
OWN(host) is still in flight, until she has it; each at most the link's delay. Zero thinking only while a hand-over is in
flight (at most two ack timeouts and a 2 Hz decision, 6.5 s). The host counts `no-owner>1s` (an OWNACK later than 1 s, a back that
timed out) and `two-thinking` (her re-freeze did not take).

### 10.3 Wire (enemy_wire.hpp)

SCB1 (channel 6, P2's header): type 3 OWN and 9 OWNACK, n 1..16, 40 B each (`u32 gen | u32 id | u16 epoch | u8 owner | u8 reason
| u8 phase | u8 flags | u16 result << 8 | refusal | i32 pos[3] mm | u16 yaw | i16 vel[3] cm/s | f32 health (NaN)`), type 10
OWNRESET (`u32 gen`). ecombat drains channel 6 and hands these three types to `esync::QueueOwnMessage`; while ecombat does not
drain (not installed, stopped), enemy_sync drains it. SEN1: header flags bit1 from the guest, bit2 a cutscene there; gen_tag the
host's generation; entry flag bit2 kDamaged, phase `+0x110`. The host takes her entries only for ids she owns with the epoch
byte; she never takes the host's for one she owns. Shots: `shots::SetEnemyShots` is a mask (bit0 capture the owned shooters'
records, bit1 replay; P1b 3 on both).

### 10.4 Debug actions, logs

`enemy_sync 5` the ownership table; `enemy_sync 8 0|1` ownership off (a reset) / on; `enemy_sync 0` adds `[enemy] own: on|off
(..), owned here n=.., guest-owned n=.., pending n=.., changes take .. back .. forced .. refused .., no-owner>1s .., two-thinking
.., resets .., gen ..`; `enemy_own <id | 0 | -5> <1 | 2>` (hidden runs, the host) a forced hand-over; `enemy_look -7 <m>` the
nearest enemy the guest owns (`-5` stays P2's newest HIT). Logs: `OWN id=.. -> guest (proximity: guest .. m, host .. m; ...)`,
`taken here`, `refused here`, `-> host (far .. m | host near .. m | silence .. s | range .. m | ...)`, `given back here`, `mirror
on id=.. (given back ...)`, `back here`, `own reset (..)`, `owned here: n=.. (ids ..); guest-owned n=..; ...` every 5 s,
`owned id=.. phase a -> b (..; its camp ..; weapon268 ..)` (guest), `an owned enemy hit our Sam: attacker .. id ..` (guest, the
first 8), `[shots] owned rounds' hits: our Sam n, the partner body n, other n`.

### 10.5 Byte checks (group own; a mismatch turns P1b off, P1a stays)

SetPhase 0x142ACF630, RisePhase 0x142ACE9D0, GetPhase's read 0x142ECFF7D (needed); the camp lookup 0x142AF93F0, the camp
phase 0x142B649DD, the camp members 0x142B37FD2 (optional: the camp phase unknown without them). `[enemy] own byte checks:
6/6 ok` in every run.

### 10.6 The gate `two-games-enemy-own`

Eight runs, all with saves unchanged and restored, no violation, 0 game-server writes, the online gate on, no `[enemy] FAULT`,
byte check failure, `[shots] FAULT` or OTGC write.

- `150947`, `151238`: no take: the camp's phase 2 (fixed: 10.0). `151458`: takes work (02029 at 12.3 m, the host 64.8 m;
  0202E); the host's follower err p95 0.06-0.46 m; the forced save refused by the game (step removed). `151701`, `151942`: no
  calm back: her owned MULEs went to Alert 13.6 s after the take with her 12 m off (diagnostics added; A / B reordered).
- `152417`: A and B pass (take, `back here` by far, `given back here`, a mirror again); C: three taken again, Alert 3 s after her
  burst, no round in 60 s (they carried no gun; weapon +0x268 0).
- **`152731`** (the contact sheet): A, B and C pass up to the first-hit line: her owned rifleman 0202F fired 44 rounds
  (`enemy sent n=44`), its real rounds hit her Sam in her world (`owned rounds' hits: our Sam 70`), the host replayed them
  (`enemy replayed n=38 (muzzle 38`); stills 03 (16 pairs); census tag 3: her owned ones `alert 5`, the host's copies `alert 0`
  with bit 14 set; `enemy-compare.py`: tag 1 p95 0.19 m (the owned 02029 0.25 m), tag 3 p95 0.24 m: PASS. It failed on
  `enemy replay #..'s first hit ...: the partner body`: of the replays only 7 hit anything on the host (4 other frozen copies
  crowding her, 3 other), none her body. Not explained [I]: her body may take no bullet hit on the host, or the melee MULEs
  around her stand in the line.
- `153216` (the last run): A, B and C's take pass; `an owned enemy hit our Sam` (0202E / 02032 melee, the shield dropped them);
  the rifleman never fired in 45 s: the run failed at `enemy sent`.
- Steps D (forced back by range while alerted, the phase raised) and E (the status counters, `enemy_sync 8 0`) were never
  reached in a run: unverified in game (unit-tested only).

## 11. M1 probe: can a MULE on the host be made to go for her body? (round 20; `enemy_target`, hidden runs)

The question for "MULEs attack Fragile": on the host, her partner body (a `DSControlledEntity`) is never noticed by a calm MULE;
only a hit from her (P2) turns one on her. The probe tries three levers on MULE 02029 at the camp south-west of Lake Knot City.
Code: `enemy_combat.cpp` TargetProbe / TargetTick (12 byte-checked sites; a mismatch turns the probe off), the debug action
`enemy_target <op> [id] [arg]` (docs/debug-actions.md "Enemy hits").

**Addresses [V: Ghidra, byte-checked]:** `AlertForEntityWithPerfectThreatInfo` `0x1434EF1D0` (export thunk, jmp `0x1434D5A80`;
the owner's +0x88 bit 7 refuses, a null target +0x170 is a no-op), `AlertForEntity` `0x1434EF1E0` (jmp `0x1434D59D0`),
`IsHostileTowards` `0x142562F10` and `Entity::GetFaction` `0x142362980` (+0x178; both script exports with no direct callers),
RisePhase `0x142ACE9D0` (the phase object is `DSMuleComponent`+0x58, +0x110 = max(+0x110, phase)).

### 11.1 Runs

- `20261124-215710-two-games-enemy-target-probe`: reported FAIL with 0 failed required steps: 3 optional steps failed (`gear 37`
  on both, no cap worn; the camera-aim step never logged), so the host's stills show a postbox wall. Logs used.
- `20261124-220555-two-games-enemy-target-probe`: PASS, 74 steps, 132 s (the scenario as in the repo).

### 11.2 Results

- **Reads (op 0).** Her body: `DSControlledEntity`, +0x170 AI agent non-null, +0x178 an AIFaction, index 1. Our Sam
  (`DSPlayerEntity`) and the MULE: the same faction pointer, index 1. `IsHostileTowards` answers "not hostile" for every pair,
  MULE -> Sam included: the +0x178 faction is not what makes a MULE hostile. The MULE has a `DSAIIndividualComponent` (owner
  +0x48 the MULE, individual +0x50 a `DSAIIndividual`); her body and our Sam have none. Its phase object starts at 0.
- **L1 (ops 1 / 2), the alerts at her body** (29-41 m, run 1; 8-14 m, four perfect calls 2 s apart, run 2): MULE state 1 and
  phase 0 unchanged; the bits 0x2040000 gained a transient 0x200 about 3 s after the plain call. No turn toward her. The
  control at our Sam 80 m away: no change.
- **L2, hits owned by her body** (`enemy_hit_mode -3 1 0`, `enemy_hit 0x180 3`, then `enemy_hit 0x47 1`): applied (stamina
  1000 -> 846 on 0x47); it moved a little, no alert, no turn toward her.
- **L3, RisePhase (op 3)**: phase 0 -> 2, then 2 -> 3 (+0x114 = 100, decaying 83.8, 67.2); state and bits unchanged.
- **Natural notice (run 2, A)**: her body 8.3 -> 18.5 m from it, but behind it (+144..+167 deg off its facing): nothing. Still
  01: the MULE has its back to her and walks away.
- **Control (run 2, C)**: our Sam 12 m in front of it: it turned to face him within 2 s (-34 -> -1 deg), at 3 s the bits went
  to 0x42440000 and phase 5 (Alert), then it ran (12 -> 46 m) and the bits went back to 0x2040000. Still 03-host: our Sam
  crouched in the grass, the MULE running in the hangar, red alert edge markers. Still 03-guest: the mirror by the truck.
- **Her body's hit watch (op 4)**: on, 0 hits in the run (nothing struck her body).

**Conclusion [V: the runs above].** The game's perception takes our `DSPlayerEntity` Sam naturally; her `DSControlledEntity`
body is not perceived, and none of L1, L2 or L3 makes the MULE target it.

### 11.3 Next

- Either reverse how Sam's AI agent (+0x170) is registered as a perceivable target and compare it with her body's agent, or
- rely on P1b (section 10, `enemy_owner`, off by default): the MULEs near her owned by her own game, where her real Sam is
  perceived by the game itself. The practical path.

## 12. M2 probe: a visual stimulus for her body (round 20 stage 2; `enemy_target 8 / 9`, hidden runs; nothing on by default)

The design (round 19, option A, milestone M2 "she is seen"): the host's game runs the fight and her partner body is a real
target there. Section 11 showed that no alert, owned hit or phase raise does it. This probe tried the AI's own perception: the
stimulus a sensor sees. **It did not make a MULE see her. Nothing ships; no `session.ini` key was added; the README is unchanged.**

**How Decima's AI perceives [V: Ghidra, byte-checked; the runs below].** The AI manager (global `[0x147BE0D38]`) keeps stimulus
pools: visual at +0x80 (live list head +0x88, next +0xC8, at most 0x200; create `0x1425249A0` (pool, resource, pos f64[3],
vel f32[4], agent), persistent remove `0x14254CE00`), aural at +0x1A050 (list +0x1A0D8, next +0xA0). A stimulus: +8 resource
(`AIVisualStimulusResource`; +0x2C lifetime, -1 = persistent; +0x40 the visibility), +0x10 the source AI agent, +0x18 the
place (f64[3]), +0x80 life, +0x84 the off byte, +0xA4 a visibility multiplier. An `AIVisualStimulusComponent` makes one at its
activation (`0x14242CDF0`: component +0x30 -> resource +0x20, the entity's agent +0x170, the agent's place `0x14252D970` vt+0xF8),
kept at component +0x50. 10 byte-checked sites (`[enemy] stimulus: byte checks (10 sites): ok`; a mismatch turns ops 9 off).

### 12.1 Runs

- `20261124-224340-two-games-enemy-stim-probe`: FAIL (a step waited for a component read). It showed that our Sam
  (`DSPlayerEntity`) has no `AIVisualStimulusComponent`, nor has her body.
- `20261124-224836-...`: FAIL at a `waitnew` that came in the same tick as the previous line (now `waitlog`); logs used.
- `20261124-225003-two-games-enemy-stim-probe`: PASS, 75 steps, 114 s (the scenario as in the repo).
- `20261124-225504-two-games-enemy-hit`: PASS, 108 steps (the P2 gate, unchanged; a regression run for this code).

### 12.2 Results

- **Our Sam's stimuli (op 8, the walk).** 42 live visual stimuli at the camp; one by our Sam's agent: a persistent
  `AIVisualStimulus` (resource `AIVisualStimulusResource`, lifetime -1, +0x40 1.0, +0xA4 1.0) at his place (0.1 m off; the game
  moves it with him). 3 aural ones, one his (`AIAuralStimulusResource`, +0x40 60). Her body's agent has none. So her body is
  not seen because nothing stands for it in the pools; it is made somewhere other than a component (a player-only path).
- **A, the control** (her body 14 m straight ahead of MULE 02029, no stimulus): it walked on toward her (13.6 -> 4.9 m, its
  heading within 26 deg), state 1, phase 0: no notice.
- **B, her stimulus** (op 9 1: her agent, Sam's resource, the same multiplier): made (`hers made (mode 1: her agent)`), the game
  kept it on her place (0.00 m apart for 18 s, persistent, never off). The MULE walked to 5 m of her, turned away (+0 -> -122
  deg), state 1, phase 0, bits unchanged: **no notice.** Guest still 02 (9 s): Fragile by the hangar wall, no MULE in view.
- **C, the decoy** (op 9 3: a second stimulus with our Sam's agent, its place written to her body each tick): within 1 s the
  MULE turned from her (+1 deg) to face **our real Sam 67 m away** (+160 -> +19 deg off), held it 7 s, its bits gained 0x200
  (as with section 11's alerts), then turned back and away; state 1, phase 0. The sensor took the stimulus, but what it knows
  is the agent, and the agent's place is Sam's. Host still 03: our Sam in the grass outside the camp ("Guest 84 m" marker);
  guest still 03: Fragile on a rooftop, a calm MULE in the distance.

**Conclusion [V: the runs above].** A visual stimulus alone does not make a MULE notice an agent that is not the player's: with
her agent it is ignored; with Sam's agent the MULE turns to Sam. The filter is past the stimulus (the sensor or the knowledge
taking only the player, as the `GetPlayer(0)` calls in the MULE code suggest). The perception lever was not found, so the
perception test, the 4 s target hold and the M3 reverse HIT relay (it needs the MULEs to go for her body first) are not built.
The round-19 design's fallback is P1b (section 10).

### 12.3 Next

- Reverse the visual sensor's pass over the list (`DSAIVisualSensor`, RTTI `0x144c85f80`; `AIVisualSensor` vtable
  `0x143d4a9f0`, several of its entries have no Ghidra function yet) to find the check on the source agent, or
- P1b: the MULEs near her thinking in her game, where her real Sam is seen and hit by the game itself.
- A safety note for any later use: a stimulus made for her agent must be removed before her body is freed (the probe drops it
  when the body or its agent changes, a frame late at worst; hidden runs only).
- The probe's own safety (round 20 review): a stimulus the game has reused for another source (its agent or resource changed),
  or one from the old world after a world change, is forgotten without the remove (`hers forgotten`): putting it back into the
  pool would free what the game now owns, with no fault to catch it. The probe walks the lists and creates / removes entries
  from the mod's game-thread tick without the AI manager's lock: if the sensors ran on worker jobs at that moment it could race.
  A probe-only hazard (hidden runs); any later use must take the pool's lock (not found yet) or run where the sensors do.
  Op 9 also needs the target probe's agent-offset check (`entity+0x170`, `kTAgent`).

## 13. P1c: ownership per camp (round 21; `session.ini enemy_owner`, on by default with `enemy_hits`)

Fights near Fragile run in her own game. A MULE camp near the guest and farther from the host is handed to HER game as a whole:
its MULEs see her real Sam, chase her, shoot her, take her cargo, and her hits on them are her world's own damage. The host's
copies become followers (frozen, driven and posed from her frames; her owned MULEs' rounds replayed on his screen, P1b's
machinery). When the fight is over and she leaves, or the host comes nearer, the camp goes back to the host, with the downs
she caused carried onto his copies. The host's hits on his copies of hers are relayed to her game. Section 10 (P1b) is the
protocol underneath; this section is what round 21 changed.

### 13.1 Design (and what the spec planned that was not built)

- The host alone decides, per camp, at 2 Hz (`erules::CampDecide`, pure, unit-tested), and hands every member over with P1b's
  OWN / OWNACK / OWNRESET (SCB1 types 3 / 9 / 10; the protocol `TestOwnProperty` proves). The guest needs no camp knowledge: she
  takes and gives back per member as in P1b. There is no new wire type (the spec's CAMP / COWN / CSTAT, types 11-16, were not
  built): fewer messages, no camp id shared between the games, so the critique's B5 (camp ids not proven equal, namespaces
  colliding) does not arise.
- A camp is a DSMuleCPGameActor's member list on the HOST (the MULE manager `[0x147BF09D8]` +0xD8 / +0xE0 the CP ids; a CP's
  +0x4C0 count and +0x4C8 member pointers, 10.0); a MULE in no CP list is a "loose" camp of one. Membership is read from the
  host's lists at each walk (no first-read cache, no union with her lists: the critique's B4 fix is not needed when only one
  side's lists count). The camp key is logged as `cp XXXXX` (the CP's id) or `loose XXXXX` (the MULE's id). A member keeps
  its last CP key for 5 s when a walk misses it (`kCampKeyKeepMs`), and a failed walk keeps every key it had (review fix R2 /
  G12: a camp no longer falls apart into loose camps of one for a tick).
- Both beacons must carry `kPartCamp` (32). This build never sends `kPartOwn` (8) (review fix R1: a v0.33 host reads 8 as
  "run P1b's per-enemy rules", so a new guest would have been handed single MULEs by the old rules). A v0.33 partner (8 alone)
  gets no ownership: `own on` never logs (the link's `combat parts: ours 0x.. theirs 0x..` line shows its parts), and a partner
  that drops 32 while ownership is on turns it off with a reset: `[enemy] own off: parts: the partner's build has no camp
  ownership`.
- The camp walk, the takes and backs, the outcome carry and the follower guard run on the host's game thread (the mod's tick),
  inside the existing SEH-guarded enemy tick.

### 13.2 Geometry and rules (`erules`, enemy_rules.hpp)

- r: the members' spread (the farthest member from their centre), clamped 10..120 m. E (engage) = clamp(r + 30, 40, 150) m.
  H (margin) = clamp(0.3 r + 10, 15, 40) m. Left = E + 20 m. A loose camp (r 10) gets 40 / 15 / 60 m, P1b's distances.
- Distances are to the nearest STANDING member (all members when none stands): d_g to her body, d_h to the host's Sam.
- Take (host -> guest), every condition: not pending; both parts; at most 30 members; something takeable (standing, readable,
  not held, not hidden or in a vehicle on the host); d_g <= E; when the camp is loaded on the host (a member realized there):
  d_h >= d_g + H (the host wins ties), d_h > 25 m, every member calm (phase 0..2) and the CP not alerted (3..5); no cutscene on
  either side, her refusals under 3 a minute, no host save busy, the guards in; the camp's 15 s hold over (since its last change
  or refusal); her owned members + this camp's at most 32; 1 s since the last camp take and no other camp pending.
- A camp NOT loaded on the host (no member realized there): her distance alone decides (reason 13 `not loaded on the host`),
  and the host's member and CP phases are not checked: they are stale there (the AI does not run on the host), run
  `20261125-082959` held caution 3 for 40 s at 2 km after a range back while her game already ran the camp (M0 had released her
  mirrors). Her copies were never mirrors then, so she takes them as they stand (`no mirror here: it ran here already`).
- Back (guest -> host), forced first (ignore phase, hold and downs): a hard refusal; forced (`enemy_own`, a game sequence on a
  host follower); no OWNACK within 3 s; silence 3 s (her frames); beyond `enemy_mirror_radius` + 20 m (from the nearest
  standing member, so one far member never sends a camp back: review fix R3); EVERY settled member of hers unrealized or held
  by a sequence on her side for 2 s (only when the camp is loaded on the host; one member alone never does, the follower guard
  hides that one's host copy: review fix G3). The 1 s gap and the one-pending rule hold takes only: forced backs go at once for
  every camp.
- A member down on her side when its outcome cannot be carried (`enemy_owner_outcomes=0` or its checks failed): the camp stays
  hers (P1b's rule).
- Back by proximity: only calm (phase 0..2 on her side) and after the hold: she left (d_g > Left, `far`), the host within 20 m
  (`host near`), or the host nearer by H while within E (`the host nearer`, reason 14). A camp fighting her stays hers when the
  host walks in (`kept by the guest ..: alerted on her side`, logged every 10 s).
- Back by proximity counts the host's distances only when the camp is loaded on the host (review fix R9: an unloaded take with
  the host 30 m off came straight back); unloaded, only `far` and the forced rules give it back.
- Her refusal for one member that is not her mirror, not realized, dead, held or hidden there splits it out (that member stays
  the host's, the camp hers) and keeps it out of the camp's takes: 15 s, or for the rest of the host's world when it is dead
  there (`erules::CampNoTakeMs`; review fixes B2 / G1: one kill made the camp go back and forth every 15 s). Every other
  refusal sends the whole camp back and holds it 15 s (per camp). Only those hard refusals count toward her "3 a minute", once
  per camp (review fix B3: the split refusals of a big camp at her load edge blocked every take for a minute). A member she
  killed is put on the same world-long list when its outcome is carried.
- A straggler (a member that was down, held or in a vehicle on the host at the take and stands free now) joins her camp on its
  own when the camp is settled and calm, one a tick, under her cap and the 1 s gap (`erules::CampStragglerOk`, review fix G5:
  `[enemy] camp cp XXXXX: straggler id=XXXXX -> guest`).
- Her place: when the host loses her body (a vehicle, a respawn) her last place is held for 3 s before her distance is unknown
  (review fix G14; unknown with a camp of hers is a range back).
- A member she owns that is in no camp of the host's view (unloaded or unread there) is decided as a loose camp of one from her
  frames (silence, range, far): `[enemy] OWN id=XXXXX not in the host's view: -> host (...)` (review fix G11; it stayed hers
  forever before).
- The take of an unloaded camp sends no place and no phase (her copy is the truth there), and her side, taking a copy it was
  never frozen as, neither moves it nor sets its phase (review fix B1: the gate's unloaded take moved alerted MULEs up to 29 m).
- Hysteresis: no position takes and gives back at once (unit-tested sweep).

### 13.3 Byte checks

No new hook. The outcome setter 0x142AA8000 (k 0 faint, 1 sleep, 2 restrained) is byte-checked with P2's hits group
(`[enemy] hits: outcome carry checks ok (P1c)`); a mismatch turns only the carry off (`enemy_owner_outcomes` then acts as 0: a
down member keeps its camp hers). The carry group also checks the damage component's entry 0x142AF8FC0 (`48 89 5C 24 08 48 89
74 24 10 57 48 83 EC 20 49 8B D8 8B F2 48 8B F9 E8`) and the store of its pointer at 0x142AC09B9 (`48 89 87 90 0E 00 00 48 85
C0`, +0xE90), the spec's two sites (review fix R10). The wide attack ids need the game's attack registry (`[shots] attack
registry n=247, max id=0x194, 0x191 listed yes`); an implausible registry (0x57 missing, e.g. before it is filled) keeps the
0x18C cap. The registry is read again after every load (the tries reset with the bullet dummy), and it filters only the ids
above 0x18C: every id v0.33 replayed still travels without it (review fix R8). A HIT with the placeholder 0x18D is refused
(`kAttackMax` 0x18C, review fix N5).

### 13.4 Outcomes, hits and the follower guard

- Outcome carry (`enemy_owner_outcomes`, default 1): at every completed back (and at a reset) the host reads the state byte of
  her newest frame for that member and, when it is down, calls the outcome setter on his copy after the release: faint ->
  faint, sleep -> sleep, restrained -> restrained, dead or an unknown down -> faint (a kill on the host is not carried this
  round). A hit still queued on the copy defers it (up to 3 s). Her frame table for the member is cleared at the back, so an
  old frame never carries twice. `[enemy] OWN id=.. outcome carried: her faint -> faint here (k 0)`.
  Review fixes (R4 / R5 / R10 / G9): the setter never runs while the host's game saves or while a cutscene still holds the
  copy's return (`carry deferred (the host's game saves | a cutscene holds its return | a hit queued on the copy)`, given up
  after 15 s: `NOT carried: its return was held past 15 s`); it checks the actor is still that MULE and that its outcome is
  still none (`not carried: down here already (faint)`: never twice, never over a down the host caused); and a second after a
  carry the host reads the copy's outcome back: `[enemy] OWN id=.. outcome readback: k 0 -> now faint` (`: NOT TAKEN (a
  fallback)` or `: a mismatch (a fallback)` count as carry failures). A member she killed (dead there) is carried as a faint
  and stays out of every take for the rest of the host's world: on the host it gets up later, in her world it is a corpse
  (review G10: a known difference, said in the README).
- Host hit relay (`enemy_owner_hits`, default 1): while ownership is on, the host captures his own hits on his followers of
  her members (P2's capture path, the same kinds and lethal policy: lethal refused with `enemy_hits_lethal=0`) and sends them
  as type 4 HITs; she applies them with P2's apply (`HostHit(relay)`), no HITRES or OUTCOME back: `[enemy] relayed HIT #..
  id=.. attack 0x47 from the host (kind test): applied to hers` (or `refused: <why>`; the kind is bullet, sphere/job, direct
  or test). Relayed hits have their own counters (`relayed`, `applied`) and no longer count in her P2 hit counters (review
  fix N3). The credit stays the hitter's player in her world (owner 0). His real rifle at a follower is a gate step (13.7,
  review G6), proven in run `20261125-101853` (`HIT sent #2 id=02029 attack 0x47 kind bullet`, 13.9 m from her; hers `relayed
  HIT #2 .. (kind bullet): applied to hers`); his melee goes through the same capture (`direct`) but no run has hit a MULE with it.
- The follower guard (`enemy_follow_guard`, default 1; M0 the other way, the 8.10 asymmetry): a host follower of hers is hidden
  while her newest frame says not realized or held by a sequence there for 1 s, or no frame came for 3 s; shown again when her
  frames say realized and free, and at every return. It reads her `kRealized` / `kSeqHeld` bits and, since the review (N6),
  `kHidden` too unless the member is down or dead there (a down MULE is never hidden here). Since the camp back needs EVERY
  member bad for 2 s (13.2), the guard is what handles one member (review G3 / G4); `enemy_camp 4` on the guest hides her owned
  copy to prove it in the gate. Far followers that realize on the host are checked every tick (not at 2 Hz) and frozen at once
  (review fix R6), except while the host's game saves.
- The wide attack ids (`enemy_attack_registry`, default 1; beacon part `kPartAttackWide` 64, advertised by this build whatever
  its ownership): shots with attack ids above 0x18C up to 0x194 travel when the game's registry lists them and the partner's
  beacon carries 64; for an older partner only that shot is dropped, never the frame. Ids up to 0x18C never need the registry. The MULE fixed gun's 0x191 was replayed
  in the gate (`enemy replay #4: shooter 0202F, weapon 92 mag 144 attack 0x191`).

### 13.5 Debug actions and logs

`enemy_camp 0` the camp table (`[enemy] camp row cp|loose XXXXX: owner .., n=.. hers .., guest .. m, host .. m, r .. m, engage
.. m, margin .. m, loaded .., phase .., camp ..`, then `camp rows n=.. (of .. known; owned here n=..; carried .., carry failed
.., down already .., followers hidden .. now .., stragglers .., no-take ..)`); `enemy_camp 3 <id | 0> <k>` the outcome setter
on our copy (hidden runs; `down here already` when it is down); `enemy_camp 4 <id | 0> <1 hide | 0 show>` (the guest: her
owned copy hidden or shown in her game, the nearest or by id; a show with 0 shows the one hidden last: `[enemy] camp test: her
owned copy XXXXX hidden | shown here`, for the follower guard's gate step); `enemy_own` hands
over a whole camp; `enemy_look -9` the newest outcome set or carried here; `enemy_hit_send` on the host while it relays
(docs/debug-actions.md). Logs: `[enemy] own installed (per camp, P1c): ...`, `[enemy] camp cp XXXXX -> guest (proximity |
not loaded on the host | forced: n=.. of .., guest .. m, host .. m, r .. m, engage .. m, margin .. m, loaded .., phase ..,
camp ..; ids ..)`, `[enemy] camp cp XXXXX -> host (far | host near | the host nearer | forced | range, forced | silence,
forced | ...: n=.., guest .. m, host .. m, r .. m, her phase .., a member down there .., silence .. s)`, `[enemy] camp ..
kept by the guest (...): the hold | alerted on her side | she is nearer | a member down on her side (no outcome carry)`,
`[enemy] hits relay (P1c): the host relays | in; relayed in n=.. applied .., not owned ..; carry ready | off`, `[enemy] camp
.. : straggler id=.. -> guest (...)`, `[enemy] OWN id=.. not in the host's view: -> host (...)`, `[enemy] OWN id=.. outcome
readback: k .. -> now ..`, `[enemy] OWN id=..: the host's follower hidden (her copy is not realized there | is held by a
sequence there | is hidden there | sent no frame for 3 s)` / `shown again (her frames say realized and free again)`.

### 13.6 Keys

`enemy_owner` (default: on when `enemy_hits` is on, `cfgval::kOwnerDefaultOn`; a given value wins; players cannot set it, the
launcher writes no key), `enemy_owner_outcomes`, `enemy_owner_hits`, `enemy_follow_guard`, `enemy_attack_registry` (each
default 1). `[config] enemy_owner=1 (default: on with enemy_hits) outcomes=1 hits=1 follow_guard=1 attack_registry=1`.

### 13.7 The gate `two-games-enemy-camp` and the runs

The MULE Camp Southwest of Lake Knot City is cp 04004: 10 MULEs (02029..02032), one of them (0202A) about 200 m out, so r 120
m, E 150 m, H 40 m. Steps (docs/hidden-testing.md): A the proximity take, B the calm back, C an outcome carried, D a fight
(her burst, her MULEs alerted, kept with the host near, the host's relayed HIT, rounds replayed, a forced back by range), E the
counters, U the unloaded take.

| run | result | why |
|---|---|---|
| `20261125-081930` | FAIL A | the geometry: with r 120 the margin is 40 m and the host at the camp spot was only ~34 m farther than her (fixed: the guest 15 m from her nearest mirror; B from 1350 1050) |
| `20261125-082221` | FAIL A | the take worked (n=10 of 10); the regex `camp \S+` cannot match `cp 04004` |
| `20261125-082412` | FAIL C | her `taken here` came before the `waitnew` window (now `waitlog` with the epoch) |
| `20261125-082644` | FAIL D | the phase rise came before the `waitnew` (now `waitlog`) |
| `20261125-082959` | FAIL U | `a member's phase`: the unloaded camp's stale caution (the rule fixed, 13.2, unit-tested) |
| **`20261125-083424`** | **PASS** | 204 steps, 0 failed, 173 s |

The passing run: A `camp cp 04004 -> guest (proximity: n=10 of 10, guest 15.3 m, host 64.8 m, r 120 m, engage 150 m, margin
40 m, loaded 1, phase 0, camp 2)`; B `-> host (the host nearer: n=10, guest 125.3 m, host 56.6 m ...)`, back errors <= 0.17 m;
C a forced take of 10, `camp test: outcome k 0 on 0202A here: set`, the forced back with `a member down there 1`, and on the
host `outcome carried: her faint -> faint here (k 0)`; D 9 taken (the fainted one stays out), her owned MULEs phase 0 -> 5
(27 phase lines), `kept by the guest (n=9 of 10, guest 2.7 m, host 5.8 m, ... her phase 5 (alert)): alerted on her side`,
`relayed HIT #1 id=02029 attack 0x47 from the host: applied to hers`, `enemy sent n=48` / `enemy replayed n=48 (muzzle 48`,
`owned rounds' hits: our Sam 8`, then `-> host (range, forced: n=9, guest 431.6 m ... her phase 3`; E `no-owner>1s 0`,
`two-thinking 0`, `camp rows .. carried 1, carry failed 0, followers hidden 0`; U the host at Mama's Lab (2 km), the guest at
the camp: `OWN id=.. -> guest (not loaded on the host: guest 40.5 m, host 2065.2 m; ...)` and `taken here (epoch 7; ...; no
mirror here: it ran here already)`. `enemy-compare.py`: tag 1 (calm, owned) p95 0.28 m: pass; tag 3 (mid-chase) p95 1.34 m,
max 1.87 m (0202F): recorded, not a check (the tool's overall line says FAIL because of tag 3).

Stills (looked at): 01 the host in the grass with the hangars behind and her body far right; on hers a calm yellow MULE at the
camp's edge. 02 (the back) the host facing the camp, a small calm MULE by a hangar; her Sam on rocks to the west. 03 on hers red
MULE tracers streaking at her Sam from the left as she aims; on the host's (03-h) a MULE by a pylon and a replayed tracer
toward her body. 04 both screens show the same fainted yellow MULE lying in the grass (Ceramics (80) marker) beside the other
player's body. 05 (U) her Sam aiming with red tracers from the left and "Cargo stolen: Ceramics (80)": her game runs the camp
with the host 2 km away.

Regressions on the final code (each pins `ini enemy_owner=0` where the guest stands near a camp, so it keeps testing the
host-owned mirrors it was written for): `two-games-enemy-hit` `20261125-084302` PASS (`enemy-hits.py`: 13 HITs once each,
the fainted 02029 her target); `two-games-enemy-statues` `20261125-084534` PASS; `two-games-enemy-hits-off`
`20261125-084753` PASS with the defaults (the host `enemy_owner=0 (default: on with enemy_hits)` because its hits are off, the
guest 1: ownership off, the parts say why).

After the review (13.9) the gate changed (it passed end to end in run `20261125-101853`, 219 steps; `enemy-compare.py` tag 1
p95 0.35 m): no `ini enemy_owner=1` (the launcher's Local test files alone, so
the default-on path is the one proven, review R7; the `[config]` lines must show `(default: on with enemy_hits)`), C reads
the carried faint back, D's alert is unforced (she walks to 8 m of her owned MULEs with the rifle drawn and fires nothing,
review G8), D adds the host's real rifle at his follower (`HIT sent .. kind bullet`, her `relayed HIT .. (kind bullet):
applied to hers`, review G6), an optional check of her owned MULEs' hits on her Sam, and the follower guard (`enemy_camp 4`:
hidden there, his follower hidden, shown again, review G4). `nolog outcome readback.*NOT TAKEN`. The hit, mirror and statues
regressions keep `ini enemy_owner=0`: they test the host-owned mirrors (P1a, M0, P2) where the guest stands near a camp, which
ownership would otherwise take; the default-on path is the gate's and `two-games-launcher-local-test`'s.

### 13.8 Gaps and declines

- No CAMP / CSTAT wire and no union of both sides' lists (13.1): membership is the host's. A MULE that registers late joins at
  the next walk.
- The save: the host still resets ownership before a save (10.0; `own reset (the host's game saves)`), but since the review only
  when a copy of hers is really frozen on the host (a pending return or a realized frozen follower; a far, unloaded follower no
  longer counts: review G2), and the carry waits for the save to end (R4). The player sees it: her fight there freezes into
  the host's copies, which go back to their posts on alert, and her game takes the camp again after the 15 s hold if she is
  still nearer (the README says so, spec 7.3). The spec's "kept across a save" mode was not built. No gate step saves: the game
  refuses the forced auto save near a MULE camp (run `20261124-151458`: `Save guard: forced auto save: refused` on every try),
  so the save path with ownership has not run in a game.
- The old-build compat step (spec 14.3) was not run: an old-build partner cannot be simulated in the hidden runs. A partner
  without part 32 from the start simply never turns ownership on (no line says why beyond the link's parts line). `two-games-enemy-hits-off` ran the default-follow path instead.
- The 3 s silence is unit-tested only. Her pause or map menu for more than 3 s would hand a fight back (critique R7, not
  built). The follower guard has a gate step (13.7), passed in run `20261125-101853`.
- The relayed hit's credit is owner 0 (his kill on her MULE is not credited to his Sam). Melee goes through the same capture
  but no run has hit with it; his real rifle's hits (kind bullet) were relayed in run `20261125-101853`. The gate draws his rifle
  after the look's teleport and waits for `our gear .. on joint 26`: run `20261125-095552` switched to it 0.1 s before the
  teleport and he never drew it (RT fired nothing).
- A host hit on a member whose return is in flight (one OWN / OWNACK round trip, well under a second) is lost: dropped on his
  frozen copy, refused on her side (`not owned here (now)`). Not fixed (review G7 / N7): it needs a host apply path queued
  across the return that no run could prove; the window is one round trip.
- Kills: her kills carry as faint (a lasting difference: a corpse in her world, a fainted MULE on the host). Cargo her MULEs
  steal is her world's only.
- One camp, one run: two camps at once, a link loss mid-fight, the private room and vehicles were not run (critique R18).
- A widely spread camp (r 120) needs her within E of a member and the host 40 m farther: at cp 04004 the take needs her close.
- Tag 3 (mid-chase) follower error p95 1.34 m.
- The "two-thinking 0" counter counts fault refusals only (review N10, kept): the "never two thinking copies" invariant holds
  by the message order (she thinks only after her OWNACK, the host only after hers to his OWNRESET or the back) and is checked
  on the model by `TestCampOwnProperty`; a cross-log count of overlapping unfrozen copies was not built.

### 13.9 Review fixes (round 21 reviews rv-correctness and rv-gameplay; built and unit-tested, no game run yet)

| finding | fix |
|---|---|
| B1 the unloaded take moved her live MULEs and set a stale phase | no place and an unknown phase in an unloaded take's OWN; her side never moves or re-phases a copy it was not frozen as |
| B2 / G1 a member she killed or hid made the camp flap | dead, held and hidden refusals split; the member is kept out of takes 15 s, a dead one for the world |
| B3 split refusals filled the refusal window | only hard refusals count, once per camp |
| R1 the build sent `kPartOwn` | it sends and needs `kPartCamp` alone |
| R2 / G12 a failed walk made every member loose | a failed walk keeps the keys; a missed member keeps its key 5 s |
| R3 / G3 one member's sequence sent the camp back | every settled member bad for 2 s; one member is the follower guard's |
| R4 / G2 the save reset and the carry under a save | the reset only with a really frozen copy; the carry waits for the save; README text |
| R5 the carry on a copy a cutscene still held | it waits for the return (15 s at most) |
| R6 far followers frozen at 2 Hz | every tick |
| R7 the default not proven | the gate runs the launcher files alone and checks `(default: on with enemy_hits)` |
| R8 the registry read once and filtering old ids | re-read after each load; ids up to 0x18C never filtered |
| R9 proximity backs on an unloaded camp | the host's distances count only loaded; the hysteresis sweep covers both |
| R10 / G9 the setter's checks and no readback | same-MULE and outcome-none checks, the two byte sites, the readback line (a gate step) |
| R11 / G15 no camp property test | `CampAggregate` (the member-to-camp glue, pure) and `TestCampAggregate`, `TestCampOwnProperty` (3000 random walks: never two thinking, no lasting none, no back sooner than the hold, no split back, no offer of a dead member) |
| G4 the follower guard never ran | `enemy_camp 4` and a gate step |
| G5 a split camp stayed split | stragglers taken one a tick |
| G6 his real hits unproven | a gate step with his rifle; README softened |
| G8 the alert was forced | the gate's D: an unforced alert by her approach |
| G10 a kill becomes a faint | README; the dead member is kept out of takes |
| G11 orphans never decided | decided as loose camps from her frames |
| G13 / N2 the rate limit and `force_back` | the dead `force_back` removed; the 1 s gap is the spec's rate limit |
| G14 losing her body forced a range back | her place held 3 s |
| N1 the world reset left the carry lists | cleared at a world change |
| N3 relayed hits in her P2 counters | own counters |
| N4 P1b's `HostDecide` dead in production | marked test-only (it still proves the protocol) |
| N5 0x18D passed `AttackOk` | `kAttackMax` 0x18C |
| N6 the guard never read `kHidden` | it does, unless down there |
| N8 the README claimed more than proven | reworded |
| N9 no tests for the four keys | `cfgval::OwnerKeys` / `SetOwnerKey`, unit-tested |

Declined, with the reason: the gate save step (the game refuses the forced save there, 13.8); a per-world cache of the member
lists (R2: the lists change legitimately, a CP adds a MULE; the 5 s key keep covers a partial walk); the back-window host hit
(G7 / N7, 13.8); the cross-log two-thinking count (N10, above); the spec's keep-across-a-save mode (not built: the reset now
fires only with a really frozen copy).

## 14. Round 23: M1 "steady ground" and the M2 probes

### 14.0 In short

Round 23 built milestone M1 of the round-22 design: fixes for what the user saw in the round-22 session. It also ran the M2
probes, which measure what the next milestone (one owner per enemy instead of per camp) needs. Nothing in this round changes
who owns an enemy: camp ownership (section 13) works as before.

**What M1 built.** Every part is on by default and sits behind its own session.ini key.

| What the user saw (round 22) | What changed | Keys | Section | Tested in a game |
|---|---|---|---|---|
| Her strand on a host MULE made it faint, not tied up | A tie-up is sent as a tie-up (HIT kind 5); the owner's game ties its own MULE up. Both directions | `enemy_restrain_relay` | 14.1-14.4 | Yes, both directions, with the test action `enemy_restrain .. 1`; a real strand not yet |
| A MULE kept shooting on the other screen after it went down | Its queued shot replays are dropped when it goes down | `enemy_replay_flush` | 14.5 | Yes: no replay later than 0.5 s after the down (`enemy-tie.py`) |
| Mirrors jumping around (p95 error up to 218 m) | Lines that name the worst copy; a copy that cannot hold its place is hidden for 5 s and tried again; a copy in a vehicle is given back to its own game | `enemy_mirror_hold_check`, `enemy_mirror_vehicle` | 14.8 | The lines and the vehicle give-back: yes. The 5 s hide never fired (p95 0.09-0.12 m in every gate) |
| Every MULE stood up and walked off when the host's Sam died | Copies near the other player are held frozen (or hidden) until the player is back; ownership does not change meanwhile | `enemy_down_hold`, `enemy_down_hold_radius` | 14.9, 14.13 | Yes, both directions, with the test trigger `enemy_sync 9`; a real death not yet |
| An empty MULE truck was dragged across the other screen | MULE trucks are left to each game: no PARK frames, no re-placing, no mirroring | `vehicle_mule_log`, `vehicle_mule_hold` | 14.10 | Yes: MULEs drove a truck off in both games, 0 teleports |
| The install line's "15..40 m farther" | Prints each camp's margin | none | 14.8 (7.4) | Log text only |

Round 22's `dev_give` (give weapons and gear, Dev tools page) is part of M1 too; its gate passed again in the final batch.

**How it was tested.** 46 unit test programs pass. The final batch ran 7 hidden scenarios on the final code, and all 7 passed
(14.15): the three round 23 gates with their log checkers, and four older regressions. Every behaviour that only a real strand,
a real death or a real long play session can show is still open (14.16).

**What the M2 probes answered (14.14).** Seeing the other player has to be our own test (tier 2), built from the game's pieces:
we cannot read a MULE's own knowledge of the players, because the probes never found where it is kept. The game's raycast can be
called safely from the mod. No push lever was found: a copy handed to the guest faced her and never fired within 15 s, with
any lever. The notice-timing constants could not be fitted from these runs.

**The tie-up relay (14.1-14.7; `session.ini enemy_restrain_relay`, `enemy_replay_flush`, both on by default).** Run in both
directions by the gates with the test action (14.12, 14.15); a real strand has not run yet. The problem it answers ([O],
round 22 logs): her strand on a mirror reached the host as a plain HIT (attack 0x15C, the stun, kind sphere/job, part none,
amount 0, her distance 0.0 m), and the host applied it as damage: the MULE fainted instead of being tied up. A bola (0x102)
did nothing through the damage path. And shots a MULE fired just before it went down were still replayed afterwards.

### 14.1 The wire: HIT kind 5 (not the pad byte)

"Restrained by me" is a new HIT kind, `ewire::kKindRestrain = 5`; the 80-byte entry is unchanged and byte 19 stays the zero
pad. For kind 5: b180 = the outcome setter's k (0 faint, 2 restrained; anything else refused), depth = the signal (1 native:
the acting game's copy showed it, 2 fallback: weapon 41 + attack 0x15C at contact, 3 a hidden-run test; anything else
refused), amount 0 (a restrain carries no damage), attack the contact's (0x15C strand, 0x102 bola; the usual attack rules),
b12E the weapon id. Why the kind and not the pad: the kind is what every reader already switches on (the owner must not run the
damage path for it at all), the validator can check its own fields in one place (`HitOk`), and byte 19 stays reserved and
zero-checked. An older decoder refuses kind 5 and the pad alike (the whole message), so either way the sender must know the
partner can take it: the beacon's last free part bit, `shotw::kPartRestrain` (128), set only with the hits part and only when
this game's outcome setter passed its byte checks. Kind 5 goes only to a partner whose beacon carries it, and the owner's gate
refuses kind 5 from a partner without it (`attack`). The HITRES gains result 13 `down` ("down already"); it is only ever an
answer to kind 5, so only a partner with the part ever sees it.

### 14.2 The acting side (whoever strands the copy)

Both directions run the same code (`enemy_combat.cpp`): the guest on her mirror of a host enemy (P1a), the host on its
follower of one she owns (P1c, while it relays its hits: `enemy_owner_hits`).

* A captured HIT of ours on a copy with attack 0x15C or weapon 41 (strand) or attack 0x102 (bola) is held back, not sent
  (`erules::RestrainTrigger`), and a watch opens on the copy (`[enemy] restrain watch id=XXXXX opened: strand HIT #n (attack
  0x15C weapon .., our Sam .. m) held up to 1.5 s; the copy's +0xEA4 read each tick (now none)`). The copy's pose is held from
  that moment (14.3).
* Each tick the watch reads our copy (`erules::OutcomeOf` of +0xEA0 / +0xEA4, the reads the outcome table already makes;
  no new address) and decides (`erules::RestrainWatch`): restrained (bit 21) newly set: send kind 5 k 2 (native); faint (bit
  19) newly set: kind 5 k 0 (a knockout); after 1.5 s with nothing: the fallback when the held HIT was the strand's attack
  0x15C AND weapon 41 with our Sam within 2.5 m (kind 5 k 2, depth 2, logged as the fallback), else the held HIT goes out as
  before (`no tie-up: the held HIT sent`). The watch closes without sending when the copy is no longer frozen here, or the
  owner has it down already (the guest: its OUTCOME table; the host: her SEN1 defeat bits).
* The scan (10 Hz): any frozen copy within 30 m of our Sam that goes from none to restrained or faint here with no HIT of ours
  seen opens a watch too (a tie-up or knockout by a path that never reaches OnHit) and is sent the same way.
* Sent: `[enemy] restrain HIT sent #n id=XXXXX k 2 (strand; native: the copy's +0xEA4): to the host; the held stun HIT not sent`
  (fallback: `(strand (fallback); fallback: weapon 41 + attack 0x15C at contact)`). The old stun HIT of that contact is never
  sent; further strand / bola HITs on it while a restrain waits are swallowed (`not sent: a restrain of ours on it waits for
  the owner`).
* The owner's answer: `[enemy] restrain HIT #n id=.. applied by the host (its outcome now restrained): the copy held until the
  owner's outcome`, or `refused by the host: <reason>: the copy re-posed with the owner's frames` (the hold ends at once).

### 14.3 The pose hold

While a watch or a sent restrain is open the copy shows its own animation: `esync::HoldCopy` pauses its transform writes
(DriveMirror) and its SEP1 pose apply (GivePose) in both loops (the guest's mirrors, the host's followers), and its pose slot
switches from apply to capture so the copy's own pose is recorded (`[enemy] restrain hold id=XXXXX: the copy's own animation
kept (...; up to 3.0 s)`). It ends on the owner's down OUTCOME (the guest), her SEN1 down bits (the host), a refusal, or after
3 s (`[enemy] restrain hold id=.. ended: timeout (3.0 s, no owner outcome): the copy re-posed with the owner's frames`); then the
owner's frames are blended back in from the last captured own pose over 250 ms (`released: the host's OUTCOME (held .. s): the
owner's frames blended back in over 250 ms`). A slot freed or given to another mirror meanwhile is never touched.

### 14.4 The owner side

`OwnerRestrain` (the host for her kind 5; the guest for the host's, in the relay branch): `erules::RestrainGate` (the relay on
here and the outcome setter ready, the sender's part, k 0 / 2, the world fingerprint, this game owns it, the same in-use MULE
realized, not dead, not hidden or asleep, outcome none) and then the outcome setter (0x142AA8000, byte-checked since round 21;
`ecombat::SetOutcome`, SEH-guarded) with k 2 (or k 0) instead of the stun: `[enemy] restrain HIT #n id=XXXXX from the guest
(strand; native: the copy's +0xEA4): outcome setter k 2 called (same MULE, outcome none before)`. A deferred call (a hit of
this frame still queued) is tried again for up to 1 s. One second after the setter the copy is read back: `[enemy] OUTCOME
id=XXXXX none -> restrained (cause guest HIT #n, strand): read back 1 s after the setter k 2` (or `restrain readback id=..: k 2
-> now .. (not restrained)`). On the host the outcome table logs it too, its cause naming the restrain (`cause guest HIT #n,
strand`) and the OUTCOME goes to her as before. Every kind 5 is answered with a HITRES (the guest's answers to the host are
new: the host takes a HITRES only for its own open restrains). k 2 through the setter had never run in a game before this
round ([U]).

### 14.5 The replay flush

When a down outcome (faint, sleep, restrained, dead, other) for an enemy reaches a game, that shooter's queued enemy shot
replays are dropped at once (`shots::FlushEnemyReplays`): on the guest by the host's OUTCOME (`[shots] flushed n replays of
id=XXXXX (down: the host's OUTCOME; .. in all)`, logged also when n is 0), on either game by the partner's SEN1 defeat bits
(logged only when something was dropped), and on the host after its own restrain by her SEN1 (logged always). Only the edge
into a down outcome flushes (`erules::FlushOnDefeat`).

### 14.6 Keys, logs, tests

* `enemy_restrain_relay=1` (default; 0: kind 5 is neither sent nor taken, the beacon has no restrain part, strand contacts go as
  the old stun HIT) and `enemy_replay_flush=1` (default). `[config] enemy_restrain_relay=1 enemy_replay_flush=1`. Both are
  scenario keys (testenv).
* `[enemy] restrain relay on: ...` / `off (<why>)`; the status (`enemy_sync 0`) adds `[enemy] restrain relay (round 23): ...`
  with the counts.
* `enemy_restrain <id | 0> <0 native | 1 fallback | 2 knockout>` (hidden runs; [debug-actions.md](debug-actions.md)): the same
  send path as a real tie-up, from our copy.
* Unit tests: `TestRestrain` (enemy_rules_tests: the trigger, the fallback's evidence, when to send, the owner's gate and its
  refusals, the setter's answer, the hold, the flush edge, the keys) and `TestScbRestrain` (enemy_wire_tests: kind 5's round
  trip beside a bullet HIT, its field rules on both sides, the pad, HITRES 13, the part bit).

### 14.7 Open ([U] until a run)

* Whether a frozen copy is ever natively restrained at all: her strand's damage message on a mirror is dropped at OnHit (the
  host decides), so if the tie-up runs through that message the copy never shows bit 21 and only the fallback fires. The
  fallback needs the HIT's b12E to be 41 for the strand ([U]: the round-22 logs show the attack, not the weapon byte).
* Whether a held (Occupied) copy plays a native restrained animation while its slot captures, and whether the outcome setter
  takes on the owner's copy mid-fight (k 2 never ran in a game).
* A refusal leaves the copy re-posed but sends no stun in its place (the owner said no: down already, dead, not that MULE).
* Since the gates (14.12, 14.15): the setter's k 2 does take on the owner's MULE ([O], both directions, read back restrained
  1 s later), and the hold ends on the owner's outcome 1.6 s after the watch opened. The first two points above are still
  open, because the gates use the test action (mode 1), not a real strand.

### 14.8 Round 23 B: the mirrors (diagnostics, not holding, in a vehicle; `enemy_mirror_hold_check`, `enemy_mirror_vehicle`)

Design 7.2 and 7.4. The round-22 logs showed the mirror error p95 climbing to 218 m with about 50 teleports per 5 s: some
copies never hold the owner's place, and the only line was the 5 s `[enemy] mirrors: n=..` count. This stage adds the lines
that name the copy that fails, gives up on a copy that does not hold, and gives back a copy that sits in a vehicle.

**Diagnostics (log only, both sides; the owner label is "guest" on the host's followers):**

* Every 5 s, while the window's error p95 is above 5 m: `[enemy] mirror worst id=.. err .. m (teleports .. in 5 s, held .. s):
  host state 0x.. (realized hidden dead defeat), phase .. (..), bits 0x.., host in vehicle .., host seq .. (frame age .. ms);
  here realized .., held .., in vehicle .. (<test>), parent .., anim ..; ea4 .. a88 ..` for the frozen copy with the largest
  error in those 5 s.
* Once per id, the first time its error is above 20 m: the same full line as `[enemy] mirror first over 20 m id=..`.
* Every 30 s: `[enemy] mirror error p50 .. p95 .. max .. m (n .. samples in 30 s, .. mirrors now; worst lines .., first over
  20 m ..; not holding: .. hidden now, .. in all, .. retries; vehicle releases ..)`.

**Not holding (`enemy_mirror_hold_check=1`, default).** The driver keeps each mirror's 3 newest teleports
(`erules::TeleportRing`). A mirror whose error, measured before the drive writes, is still above 10 m after 3 teleports within
2 s does not hold its owner's place (`erules::NotHolding`). It logs `[enemy] mirror not holding id=..` (the full line), is
released (`ReleaseMirror(.., "not holding (the host's place, after 3 teleports in 2 s)")`), and, when that release really
posted, is hidden and put to sleep with M0's hide (only the bits we set are restored; never a statue):
`[enemy] mirror id=.. not holding (#n): hidden and asleep here for 5 s, then tried again (..); its shots are still replayed
from the owner's place` (the first 3 per id, then every 10th). After 5 s (`erules::NotHoldRetry`) it is shown and woken
(`shown and woken again (not holding: the 5 s retry ..)`), and the entry may mirror it again. Off, a reset or the key off
shows every hidden copy at once. While it is hidden, the entry loop skips it, and its enemy shots are still taken and replayed
from the owner's place: `esync::NotHoldingShooter` gives the SEN1 track's place at the render time in place of the mirror's
transform (the 30 m origin check runs against it). The `[shots] enemy replayed` line counts them as `from a not-holding copy's
owner place ..`. The host's own followers (P1b, guest-owned) get the diagnostic line once and keep being driven: the guest's
game owns them.

**In a vehicle (`enemy_mirror_vehicle=1`, default).** Every 1 s, on both sides and for every lease (not only at the entry), the
copy is tested: first the entity's parent chain (`+0x70`, the old `InVehicle`), then, when the byte checks passed, the MULE
actor's ride fields:

* `+0xE20`: the vehicle's actor id (low 21 bits; `0x1FFFFF` none). The ride action FUN_142B51E70 writes it at `0x142B52075`.
* `+0x3C0`: the seat (0..15; -1 none). The same ride action sets it at `0x142B51F69`, behind `cmp [rax+0xEA0],1`. The actor's
  constructor (`0x142A9824C`) sets both to none (`[0x144AF1150]` is `0x1FFFFF`). [O static]
* `[enemy] vehicle test byte checks: n/4 ok`. With fewer than 4 the ride fields are off for the run. A read fault turns them off
  too (logged). [U] whether getting out clears them (probe P6): a copy that stays "in a vehicle" after getting out would be
  given back for good. The `[enemy] vehicle id=..: <side> copy is in a vehicle (test: <which>; +0xE20 0x.. seat ..)` /
  `.. out of its vehicle` lines (24 at most) show it.

What happens on each side:

* SEN1 carries the owner's answer as the entry flag `kInVehicle` (8; the entry and frame sizes are unchanged). The decoders now
  refuse bits 4-7 (no sender ever set them).
* On the guest, a copy seated here or on the host is not taken (`Refusal::host_in_vehicle`, "in a vehicle on the host ..").
  A lease is given back alive to her own game (M0 release, not hidden; `LeaseVerdict::release_vehicle`): `[enemy] mirror
  id=.. in a vehicle (<side>; our test: .., +0xE20 0x.. seat ..): released to its own game until vehicles are synced`.
* On the host, a guest-owned follower seated on either side is given back to the host's game (a forced back) and its camp
  takes nothing for 15 s: `[enemy] follower id=.. in a vehicle (..): given back to the host's game until vehicles are synced
  (out of its camp's takes 15 s)`. A host enemy in a vehicle is not takeable.

**7.4 (the install text).** The own-installed line now prints the camp's margin as `H = clamp(0.3 r + 10, 15, 40)`, per camp,
instead of "the host 15..40 m farther". When a camp's take is refused for the margin, the `camp: no take` line adds `(the
host .. m farther than her; this camp's margin H = clamp(0.3 x r + 10, 15, 40) = .. m)`.

**Keys, logs, tests.**

* `enemy_mirror_hold_check=1` and `enemy_mirror_vehicle=1` (defaults). The config line is `[config] enemy_mirror_hold_check=1
  enemy_mirror_vehicle=1`. Both are scenario keys (testenv).
* At install: `[enemy] mirror checks (round 23 B): hold check .. (..), vehicle test .. (every 1 s: ..); diagnostics: ..`.
* `TestMirrorsB` (enemy_rules_tests) covers the gates, the teleport ring and its 2 s window, the not-holding boundaries
  (10 m, 3 teleports, off), the 5 s retry, the ride fields (none, 0, seat -1/16, the parent first), the lease's vehicle
  release (both sides, the check off keeps, silence and off first), the entry refusal, and the keys. enemy_wire_tests covers
  kInVehicle's round trip and the refusal of bits 4-7 both ways.

Open ([U] until a run):

* P6, whether getting out clears `+0xE20` / `+0x3C0`. Partly answered (14.14, P6): the fields read none again within about
  1 s of boarding, while the parent chain then says "in a vehicle"; no MULE got out in any run.
* Whether a MULE driving a truck has a parent chain at all. The ride fields are the fallback.
* Whether 10 m after 3 teleports flags copies that only lag on a steep slope or a ladder. Every hide is logged with its line,
  and the retry is 5 s.

### 14.9 Round 23 C: no mass release when a player is down (`enemy_down_hold`, `enemy_down_hold_radius`)

Before this change, when the host's Sam died or went to KnotSpace, his game hid or unloaded his MULEs. The guest then saw "host
hidden" for every mirror, and M0 gave them all back to her game, alive. An enemy that had just fainted or been tied up near her
stood up and walked off. The hand-over was also thrown away by the reload that follows a death.

**What "down" means.** Death (the game state's GameOver bit) or KnotSpace (its KnotSpace bit: death, repatriation or limbo).
A faint or a stagger is never down. Each game reads its own player's state every tick. Down counts at once. Back counts only
after 1 s of not down, because the flags can flicker during the game's own cutscene and load steps.

**How the partner learns it.** Through the SEH1 beacon, byte 22 (until now a 0 pad byte; the 24-byte size is unchanged). It
holds 0 (up), 1 (death), 2 (KnotSpace), 3 or 4 (the test fake below). Byte 23 must still be 0. A change is sent at once and then
1 Hz for 5 s. The code is sent only to a partner that advertises the tie-up part (`kPartRestrain`), because an older build
refuses a beacon whose pad byte is not 0. If the partner's build is older, or the partner has `enemy_restrain_relay=0` or
`enemy_hits=0`, nothing is sent and the old rules apply. The log says so: `(not sent: the partner's build or settings cannot
take it)`.

**What happens while the host's Sam is down (on the guest).** Each mirror is decided on its own (`erules::DownHold`):

* Within `enemy_down_hold_radius` (120 m by default) of her Fragile, and it had the host's pose here before: **held**. It stays
  frozen in its last pose. Nothing is written to it: no place, no pose, no posts. A copy whose last outcome was faint, sleep or
  tied up therefore stays lying down.
* Within the radius but never posed: **hidden** here, still frozen. Hiding uses only the hidden bit, and only while it is the
  same in-use MULE.
* Within the radius and her own game already hides or sleeps it: held and never touched.
* Farther than the radius and down (faint, sleep, tied up, dead): **hidden**, never given back alive.
* Farther than the radius and standing: the usual rules, so M0 gives it back as before. Far from her, that does no harm.

The hold replaces the lease verdicts that the host's side or the distance cause: keep, silence, host unrealized, host hidden,
host far and out of range. It does not replace a game sequence taking our copy, our own copy unloading, a vehicle, our own
cutscene hold, the switch, the link or the world.

No new mirror is made while he is down (`Refusal::owner_down`). These refusals are counted in the status line, not logged per
enemy. There is no hand-over to her game for this case (critique R7c).

**When it ends.** When his Sam is back: every copy we hid is shown again, and the held ones are driven and posed again from the
next tick. It also ends on a world reload (that world's actors are forgotten, never touched), a link change (shown again), the
switch off, or a fault. A copy is always shown again before any release posts, because a copy given back while hidden here could
never fire again.

**The other way (on the host).** While the guest's Fragile is down, the host's followers (his frozen copies of the enemies she
owns) follow the same decision. Held ones skip the follower guard, the vehicle test, the drive and the pose. Hidden ones go
through the follower guard's own table, so every existing return shows them again. On the host, ownership freezes while
**either** player is down: no take, no back, no forced back, no straggler and no "loose" back. The host logs this every 10 s
while it holds a change back.

**Keys.**

* `enemy_down_hold=1` (default). 0 restores the old rules (the mass release).
* `enemy_down_hold_radius=120` (metres, 20..1000).
* Both are scenario keys and launcher overrides in testenv.
* Config line: `[config] enemy_down_hold=1 enemy_down_hold_radius=120`.

**Logs a scenario can wait for.**

* `[enemy] our Sam|Fragile down (<why>): told the partner in our beacon` and `[enemy] our Sam|Fragile back (...)`.
* `[enemy] host Sam down (<why>; its beacon|enemy_sync 9 fake): holding n mirrors, hiding m (k far and standing under the usual
  rules; radius 120 m; no new mirror, no ownership change until back)`. On the host the line reads `guest Fragile down (...):
  holding n followers, hiding m (...)`. With the key off, the line ends `enemy_down_hold=0, the old rules (...)`.
* `[enemy] host Sam back: resuming n (held x, shown again y; down z s; ...)`, or `guest Fragile back: ...`.
* `[enemy] host Sam still down, now <why>` when the reason changes.
* Per copy: `[enemy] down hold id=XXXXX: hidden here, kept frozen (...)` and `... shown again (...)`. On the host:
  `[enemy] down hold id=XXXXX: the host's follower hidden here, kept frozen (...)`.
* Ownership on the host: `[enemy] down hold: n ownership change(s) not made (her Fragile|our Sam|both players down: ...)`.
* `[enemy] down hold ended: the world changed|the link changed|no co-op role (...)`.
* `enemy_sync 0` adds `[enemy] down hold: on radius 120 m, ours up (game state), partner up (beacon), held n, hidden n,
  ownership free|frozen; downs n, resumed n, holds n, hides n, entries refused n, ownership changes not made n`.

**The test trigger (hidden runs only).** `enemy_sync 9 <1 host|2 guest> <0 clear|1 down|2 limbo>` fakes a player down. Faking
the partner's player feeds the same state the beacon would, on this game only. Faking our own player also sends the code in our
beacon, so the partner holds as it would for a real one. A link change clears the fakes. The log line is `[enemy] enemy_sync 9:
host Sam test: down (...)`.

**Tests.** `TestSamDown` (enemy_rules_tests) covers:

* the game-state bits (faint and Subspace are never down);
* the latch (flicker, a new reason at once, back after 1 s) and the receiver's changes;
* the ownership freeze (either player);
* every hold, hide and usual case, including the radius edge, an unknown distance and every defeat, both near and far;
* which lease verdicts the hold overrides and which it does not;
* the `owner_down` refusal and its order after the cutscene check;
* the keys.

shot_wire_tests' `TestBeacon` covers byte 22's round trip for each code, the refusal of codes above 4 both ways, and byte 23
still having to be 0.

Open ([U] until a run):

* Whether the host's KnotSpace drops his `coop::WorldReady`. If it does, his fingerprint resets and his beacon's print goes to
  0, and the guest's gate goes to "waiting" and turns sync off. Since the review fixes (14.13) the hold covers that path: her
  mirrors are kept frozen, not released. The first run with a real death will show it (`[enemy] down hold: sync off (...)`
  right after `host Sam down`).
* Whether the host's SEN1 "hidden" can reach the guest before his beacon's down code. Both travel on the same link, and the
  beacon goes out on the tick the latch sees KnotSpace. If the MULEs are hidden on that same frame, one lease pass could release
  first.
* Whether a held copy's pose slot keeps showing the last frame with no new one pushed [I]. If it does not, a held copy falls back
  to its own idle pose, still frozen in place.
* Whether bit 42, seen with KnotSpace in the user's death log, or Subspace (bit 8) should also count as down.

### 14.10 Round 23 D: the MULE trucks (`vehicle_mule_log`, `vehicle_mule_hold`, both on by default)

Design section 8, V1 and V2 (V3, a MULE truck shared with its driver, is not built). Before this stage the shared vehicles
(vehicle_sync=5) treated a MULE's truck like any other truck: the guest removed its own copy and mirrored the host's, and
vehicle_drive sent PARK frames for it and teleported the parked copy to the host's spot. When a MULE got in and drove off in
one game, the other game's copy was dragged after it with nobody inside, or a MULE driving in the guest's game was pulled
back to the host's parked spot. Now a MULE truck is left to each game: each game's own MULEs drive their own copy.

**What a truck is (V1, `vehicle_mule_log=1`).** Each poll (about 4 times a second), every unit of the truck pool is put
into one class (`trucks::Classify`, `native/mod/vehicle_truck_rules.hpp`):

* the model, from the spawned entity's resource: key `truck_npc` is a MULE truck (celd), `truck` is the players' truck
  (ca02). A truck that is not spawned near either Sam has no entity, so its model is "unknown" [O];
* the driver: unit +0x158, the rider id the game writes in FUN_142f7f810 (0x1FFFFF = none) [O]; its type is
  `(id & 0x1FE000) >> 13`: 0 a player, 1 a MULE or another NPC [O]. The game also sets +0x19D bit0 (a player drove it) or
  bit1 (a non-player drove it); those bits stay after the driver gets out [O]. The entity's seat (SetDriver 0x142F7C4C0:
  +0x63C = 1 and +0x8B0 = the driver's id) is read too; when the rider is none, the seat's id stands in;
* the mod's own follower (vehicle_drive's NPC controller) seats with driver none, so a truck the partner drives is never
  taken for a MULE truck.

The classes: player vehicle, player vehicle NPC-driven, MULE truck empty, MULE truck MULE-driven, MULE truck player-driven,
truck model unknown. For a MULE driver the enemy sync adds its camp and seat (`esync::MuleRideInfo`). Every 10 s, and at
most once a second when something changes, each game sends the other its trucks' facts: message type 50 ("STK1", 12 bytes
per truck, keyed by the host's id; strict validation, duplicates refused). Facts older than 25 s are forgotten.

**Left to each game (V2, `vehicle_mule_hold=1`).** `trucks::Hold` decides per truck, in this order:

1. off, or the partner drives it through vehicle_drive: shared;
2. a player drives it in either game: shared (released at once);
3. a non-player drives it here, or in the other game: left to each game;
4. it is a MULE truck here, or in the other game, and no player drives it: left to each game.

A hold starts at once and ends 5 s after its last reason (`trucks::HoldLatch`), or at once when a player gets in. While a
truck is held:

* vehicle_share does not remove the guest's copy, does not re-place it onto the host's spot, does not mirror it, does not
  tell the host when the guest's copy goes, and the host ignores the guest's removal of it;
* vehicle_drive sends no PARK frames for it and does no parked correction, push claim, final placement or far placement;
* a MULE seated in a truck's driver seat counts as "in a vehicle" for the enemy mirrors (14.8: `VehicleTest` reason
  "a truck's driver seat (unit +0x158)"), so its mirror is given back and each game's MULE drives its own truck.

A player who gets into a MULE truck makes it shared again (the rules above), the same as any truck.

**Byte checks.** Three sites (optional group `kTrucks` in vehicle_share's `kSites`): FUN_142f7f810 (the +0x158 / +0x19D
writes), SetDriver at 0x142F7C537 (the seat writes) and 0x142F7C578 (its player-id branch). If one does not match, the truck
rules are off and trucks are shared as before (`[vshare] MULE trucks (round 23 D): ... OFF: a truck byte check failed`).
Every read sits behind an SEH guard; a fault turns the truck rules off for the run (`[vshare] FAULT in the MULE truck rules:
off for the run`).

**Logs.**

* `[config] vehicle_mule_log=1 vehicle_mule_hold=1`; `[vshare] MULE trucks (round 23 D): ...` at start.
* `[vshare] MULE truck 0x...... left to each game (<why>; <class>, driver 0x......, <the other game's facts>)`, and
  `... shared again (...)` (at most 24 lines per world).
* `[vshare] MULE truck 0x...... left to each game (<why>): guest: not mirrored` (or not removed, not re-placed, its going is
  not told, the guest's removal ignored): once per truck per 30 s.
* `[vdrive] MULE truck 0x...... left to each game (<why>): no PARK frames` (or no parked correction, ...): once per 30 s.
* `[vdrive] MULE truck 0x......: driver 0x...... owner host|guest (<kind>, seat .., camp ..; <why>)` when a non-player gets in,
  and `driver none owner .. (0x...... got out; ..)` when it gets out (at most 200 lines).
* `[vshare] the partner's game sends its truck facts: N trucks (.. MULE trucks, .. driven by a non-player)` once.
* After each snapshot: `[vshare] host: snapshot: truck ...` / `guest: the host's snapshot: truck ...` and a `trucks:` summary.
* `action vehicle_share 9 [tag] [m]`: the truck census (docs/debug-actions.md); it also runs at the end of every
  `enemy_census` while `vehicle_mule_log=1`. `action vehicle_drive 0` adds
  `[vdrive] MULE trucks left to each game (round 23 D): PARK frames skipped .., parked corrections .., ...`.

**Where to find one.** The MULE Camp Southwest of Lake Knot City (`goto mule-camp-sw-lkc`, area 200, 1517.9 992.0 172.1)
has two MULE trucks: 0x020010 at (1584.7, 1059.0, 175.2), about 95 m north-east of the goto spot, and 0x02000F at
(1666.8, 990.4, 174.9), about 150 m east (its +0x19D bit1 is set: a MULE drove it before) [O, complete-100 save]. Trucks
0x020012 and 0x020013 near the MULE Camp Southwest of Central Knot City (around 29 -1275 and 97 -1250) and 0x020014 (1218
-27) also carry bit1 only, so they are likely MULE trucks too [I]; their model shows only when Sam is near.

**Runs.** `trucks-census-solo` (solo: the census before and at the camp) and `two-games-mule-trucks` (two games at the
camp: both load the rules, both send their facts, both hold 0x020010 and 0x02000F, the host skipped about 2000 PARK frames
for them in 17 s, no FAULT) both passed. **Tests:** `tests/vehicle_truck_rules_tests.cpp` (actor types, the classifier, the
hold order, the latch, the STK1 codec round trip and its refusals, the ini keys) and `VehicleTest`'s truck-driver reason in
`tests/enemy_rules_tests.cpp`.

Open ([U] until a run):

* No MULE was seen driving a truck in these runs (the camp stayed calm). Since then, in the gate `two-games-truck-hold`
  (`20261125-171219`), five of the host's MULEs boarded truck 0x02000F, the guest gave her five mirrors of them back to her
  game, and her own MULEs drove her copy. The `[vdrive] MULE truck 0x02000F: driver 0x00202E owner host` lines (both games), the
  "MULE-driven" class and the in-vehicle releases all showed [O] (14.15).
* An unspawned truck's model is unknown. Spawned MULE trucks showed unit +0x198 = 00011907 and rec2 = 0003, the one
  unspawned truck listed A0800900 / 005E; whether these tell a MULE truck before it spawns is [U].
* A MULE truck spawned new in the host's world (an ADD) can reach the guest before the host's facts for it; until they
  come the guest uses its own facts only.

### 14.11 Round 23 E: the M2 probes (`enemy_m2_probes`, off by default; hidden runs only)

Design section 10 ("M2 Probes", critique R2 / R3). The next step for the fights near Fragile (ownership per enemy instead of
per camp) depends on what the game's own AI does with a copy we hand over: does a frozen copy still keep its knowledge of our
Sam up to date (R2), and how fast does a copy handed to the guest turn to her Sam and fire (R3). These probes measure that.
They are debug actions only (`enemy_m2 <probe> [id] [arg]`, docs/debug-actions.md), in hidden runs with
`session.ini enemy_m2_probes=1` (default 0; `[config] enemy_m2_probes=0|1` at start). They only read the game, except P2's
forced hand-over and its levers. Every read is fault-free (ReadProcessMemory) or behind an SEH guard; a fault logs
`[probe] FAULT in <where>: probes off for the run`. Each probe logs `[probe] P<n> ...` lines and exactly one
`[probe] P<n> result ...` line, also when it is skipped.

**Byte checks (P0).** 27 sites and data checks in four groups: knowledge (the DSAINoticeAndKnowledge vtable 0x143E13028, its
ctor 0x142E936F0 and RTTI factory), raycast (the visual sense's call 0x141C20CE0, its world 0x14578C140, the filter vtable
0x143BA8938, the array free 0x1419D5870, the result init 0x14240B7E0), perception (the visual sense's read of entity +0x314
at 0x142E4F314) and the stimulus multiplier (+0xA4) [O]. A mismatch logs `[probe] byte check FAILED: <site>` and turns that
probe off. Both games: `[probe] P0 result: knowledge ok, raycast ok, perception ok, stimulus +0xA4 ok; ... role n` [O].

**The probes.**

* **P1** (a frozen copy, every 0.5 s): find the copy's DSAINoticeAndKnowledge (a search from the entity, its AI agent +0x170,
  its AIIndividualComponent and that component's +0x50, depth 2, pointers to it or held inside a block, at most 60000 reads),
  list its entries (type, actor, place) and say whether a PlayerVisual (type 1) entry for our Sam is being written (fresh: its
  bytes changed within 1.5 s), was written once and left (stale) or is missing (none). Pure part: `eprobe::FreshTrack`.
* **P2** (the guest): armed before a forced hand-over (`enemy_own <id> 2` on the host); from the moment the copy is hers, one
  lever is applied (a nothing, b a test hit owned by her Sam, c AlertForEntityWithPerfectThreatInfo -> her Sam, d' her Sam's
  own visual stimulus +0xA4 three times for 1 s, restored after) and the copy sampled every 0.1 s for 15 s: the time to
  within 30 deg of her Sam and to its first shot (`eprobe::Push`).
* **P3**: entity +0x314 of our Sam and the partner body, and +0x88 of our agents' visual stimuli in the AI manager's
  thread-safe visual list (0x147BE0D38 + 0x3C398) [O].
* **P4**: the game's own raycast (0x141C20CE0 with the visual sense's filter) from a copy's eye (+1.6 m) to our Sam's chest
  (+1.0 m) and head (+1.5 m), one call per tick, alternating, 0.6 m cut off each end (`eprobe::Shorten`). Clear when it
  reports no hit [O: the visual sense tests the same count with `cmovg`].
* **P5** (the host's own MULE, every 0.25 s): phase (and +0x114 / +0x118), state, bits, its angle to our Sam, and the
  knowledge as in P1 (`eprobe::PhaseTrack`).
* **P6**: for every MULE near Sam, is it seated in a vehicle: its parent chain (entity +0x70, up to 6 levels, a vehicle
  among them) and its ride fields (actor +0xE20 the vehicle's id, +0x3C0 the seat; `esync::RideFields`, the 14.8 checks).

`enemy_look -8 <m>` now also takes a negative m: our Sam that far behind the enemy, facing its back (P1's "behind" step, P2).

**Results** (runs 20261125-154710 `two-games-enemy-probe` and 20261125-155517 `trucks-m2-probe-solo`, both PASS, no FAULT;
the lines are in the round's `probe-lines.txt`):

* P0: every group ok on both games and solo [O].
* P1 / P5: **the knowledge object was not found** from any root, on a frozen copy (the guest) or a live MULE (the host and
  solo), not even one belonging to another agent (0 seen in 4700-7200 reads). So R2 is still open: P1 reported `none`
  because there was nothing to read, not because the entry is missing. The object exists (ctor 0x142E936F0 builds 0x498+
  bytes and writes the vtable [O]); it is most likely owned by something other than the MULE itself, such as its camp
  (DSMuleCPGameActor) or the AI manager [U].
* P5 (no knowledge needed): the host's own 02029 with Sam standing 11 m in front of it went from phase 0 to 5 in 0.73 s, turned
  from 135 to 10 deg off Sam in 2.8 s and walked in to 3 m (solo) [O]. In the two-game run it stayed at phase 0 at 25-35 m,
  crouched or standing, and was at phase 5 already at 12 m (it noticed during the 1.5 s before the probe started) [O].
* P4: from the guest's frozen copy to her Sam 8-13 m in front: 300 calls in 5 s, chest and head clear on every call [O]. The
  raycast is safe to call from the game thread once a tick.
* P3: our Sam's entity +0x314 reads 0 and the partner body's 0x1FFFFF (none); our Sam's visual stimulus +0x88 is 0 on the
  guest (her Sam's actor id is 0 there) [O].
* P2 (run 20261125-154710, her Sam placed 10 m in front of the copy, so it already faced her): with every lever the copy was
  hers 0.2-0.3 s after the forced take, faced her (0-3 deg) and **never fired in 15 s**, even after c (the perfect-threat
  alert, called) or d' [O]. The scenario now places her Sam 10 m behind the copy so the turn is measured too; that version
  has not run yet [U].
* P6: 10 MULEs within 400 m; after the camp was alerted, MULE 0202E sat in truck 0x02000F: the parent chain reached a
  DSVehicleTruck and the ride fields read +0xE20 0x2000F, seat 0. Both tests agree, and the truck census showed the same
  (`driver 0x00202E (MULE)`) [O].

**Tests.** `tests/enemy_probe_rules_tests.cpp` (`st_enemy_probe_rules_tests`): the type names, fresh / stale / none and the
verdict, angles and the cone, the push timing and its text, the phase tracker, the ray cut, the entry signature, plausible
pointers, actor ids, the ini key.

Open:

* Where the DSAINoticeAndKnowledge of a MULE is kept (its camp, the AI manager or a group object) [U]; P1 / P5 log
  `knowledge not found` until a search root reaches it.
* Why a copy handed to the guest faces her Sam but does not shoot within 15 s, even with the perfect-threat alert [U].
* The meaning of a knowledge entry's floats from +0x38, and whether the raycast's first hit is at result +0 [I].

### 14.12 Round 23 F: the M1 gates (three hidden scenarios, two log checkers)

Round 23 is checked by three two-game scenarios at the MULE Camp Southwest of Lake Knot City (cp 04004). A gate passes only
when Run-Batch says PASS **and** its checker script says PASS. Run-Batch does not run the checker; run it on the run folder
afterwards. No new session.ini key; the scenarios rely on the round 23 keys being on by default.

* **`two-games-enemy-tie`** (`ini enemy_owner=0`: every MULE belongs to the host). The host walks up to MULE 02029 and raises it
  to combat, so the camp shoots at him and the guest's game replays the shots. The guest then stands 1.5 m from the MULE whose
  shots she replayed last and ties it up with the test action (`enemy_restrain 0 1`: the same send path a real strand uses).
  The scenario waits for her hold, the HIT sent, the host's outcome setter and read back, her `OUTCOME ... none -> restrained`,
  the hold released and the replay flush, with stills on both screens about 1, 4 and 8 s after her OUTCOME. Then the host's
  Sam goes down with the test trigger (`enemy_sync 9 1 1`): her mirrors are held, the tied MULE stays `restrained` and is not
  "given back alive"; the clear brings `host Sam back: resuming`. Last, round 23 B's 30 s mirror summary must show p95 under
  5 m, or a `mirror worst` line must name the worst one.
  Checker: `python -I tools\testenv\enemy-tie.py build-testenv\runs\<run>`. It reads the tied MULE's id from her watch line and
  fails when a replay of it is logged later than 0.5 s after her OUTCOME, when there is no flush, or when her mirror of it lost
  its lease between her OUTCOME and the host's Sam back.
* **`two-games-enemy-tie-host`** (the launcher's default, camp ownership on). The reverse: she takes the camp by proximity, so
  02029 is hers and a frozen follower on the host; the host ties his follower up (`enemy_restrain 0x2029 1`), her game applies
  it, his game sees her copy restrained (`her copy is restrained now (SEN1)`), releases the hold and flushes. Then her Fragile
  goes down with the test trigger (`enemy_sync 9 2 1`): he holds her followers and no camp change happens until she is back.
  Checker: `enemy-tie.py <run> --reverse`.
* **`two-games-truck-hold`** (the launcher's own Local test files). The truck census on both games lists the camp's MULE trucks,
  the guest's vehicle table must be fully bound (`table n, bound n`), then 60 s pass with both Sams at the camp, the census runs
  again and the round 23 D counters must show 0 parked corrections, push claims, final and far placements on both games (no
  parked teleport). Checker: `python -I tools\testenv\truck-hold.py build-testenv\runs\<run>`: every MULE truck within 0.5 m
  between the two censuses, and the host's vehicles in use equal to her bound count. A truck that a camp MULE got into and drove
  off between the two censuses is skipped: that is the game's own drive, not a teleport.

**One product change.** Clearing the `enemy_sync 9` fake used to log `our Sam back (1 s not down)`, so a scenario could not tell
a fake clear from a real recovery. A cleared fake now logs `[enemy] our Sam|Fragile back (enemy_sync 9 fake cleared): told the
partner`; a real one still says `1 s not down`.

**Results (2026-11-25).**

* `two-games-enemy-tie`: final run `20261125-162358`. Every required step passed and `enemy-tie.py` passed: she tied up 0202F
  (sent at 59.906 s; the host's OUTCOME, her hold release and the flush all at 59.922 s), 12 replays of it before, none after;
  8 mirrors held for 10.8 s while the host's Sam was down; mirror p95 0.12 m [O]. Run-Batch counted the run as FAIL only
  because of an optional step (`enemy_look -5`: a restrain HIT is not an "applied HIT", so it had nothing to pick), which has
  since been removed. The edited scenario then passed in the final batch (`20261125-170739`, 14.15).
* `two-games-enemy-tie-host`: **`20261125-162918` PASS**, `enemy-tie.py --reverse` PASS: HIT sent at 62.046 s, her copy
  restrained at 62.171 s; 6 followers held for 24.7 s while she was down [O]. 02029 never fired before the tie-up, so the 0.5 s
  rule held with no replays to stop [O].
* `two-games-truck-hold`: **`20261125-163849` PASS**, `truck-hold.py` PASS: 22 bound of 22 on the guest, 22 in use on the host,
  9832 PARK frames skipped on the host and 0 corrections on both [O]. Truck 0x02000F was driven 94 m by a camp MULE in the host's
  game, while in hers a MULE sat in it without moving: each game runs its own MULE trucks, as round 23 D intends [O].

**Open.**

* The first two truck runs used `dev_enemies 1` and failed: right after it, her vehicle share removed 7 far trucks ("another
  vehicle under the host's id") and could not mirror 3 of them again (table 22, bound 19). The same happens in earlier enemy
  scenarios since round 22, so it is not new. The truck gate no longer resurrects the MULEs; the cause in the vehicle share's
  binding is not fixed [I].
* In the host's game the tied 0202F went `restrained -> none` about 22 s later (the game's own untie, by a comrade or a
  timeout) [U].
* The reverse gate never had a down hold that wanted a camp change, so "no camp change while she is down" passed with nothing
  to stop [U].

### 14.13 Review fixes (round 23 review; the final gates ran on this code)

Six findings from the round 23 code review, all fixed in the working tree. No new session.ini key: each fix sits under the key
that already controls its feature.

**1. The down code no longer flips to "back" when the partner's tie-up part drops.** The host sent byte 22 only while the
guest's current beacon carried the tie-up part (`kPartRestrain`). That part drops when her enemy hits fault partway through a
run, so the host then sent 0, and her game read that 0 as "the host's Sam is back" while he was still dead. Byte 22 is a
property of her build, not of her settings, so the host now remembers that her build takes it: once any of her beacons has
carried the part on this link, the down code keeps going to her until the link changes (`shotw::PeerTakesDown`). The log says
so once per link: `[enemy] the partner's build takes our player's down code (beacon byte 22; kept for this link even if its
restrain part drops)`. A partner with `enemy_restrain_relay=0` or `enemy_hits=0` from the start, or an older build, still gets
no down code, as before.

**2. A missing beacon is no longer "back", and sync turning off no longer releases the held mirrors.** Two paths gave the
guest's held or hidden mirrors back to her game alive while the host's Sam was still down:

* No beacon from the host for 20 s made the down code read 0. Now no beacon keeps the code in force: it counts as
  "unknown", not "back" (`erules::PeerDownNow`). The hold still ends on a world change, a link change, a lost co-op role, the
  switch or a fault. Log: `[enemy] host Sam down, and no beacon from the partner now: the hold kept (no beacon is no "back")`.
* The host's reload after a death resets his fingerprint, so her gate goes to "waiting" and sync turns off. The off path
  released every mirror. Now, while the host's Sam is down (and `enemy_down_hold=1`), the guest keeps every frozen copy as it
  is: the hidden ones stay hidden and the rest are held, with nothing written to them (`erules::DownKeepsOff`). The same applies
  when her own Sam is briefly missing. When he is back, the copies are shown and resumed, and the usual off release follows.
  A world change on her side forgets them untouched. Log, once per spell: `[enemy] down hold: sync off (<why>) while the
  host's Sam is down: n mirrors kept frozen (held x, hidden y), none given back alive; released when he is back or the world
  changes`.

This covers the guest only. The host's followers while her Fragile is down still go through the usual off path (ownership
reset) when his gate turns off [U, open].

**3. A MULE truck that a player drove last stays shared.** When a player got out of a MULE truck, the truck was held on the very
next poll, so its last PARK frames and its final placement never went out. The two games could then disagree for good about
where the player left it. Now a truck stays shared after a player drove it, until a non-player gets into it in either game
(`trucks::PlayerLast`, then `trucks::Hold`). Both games see the player, one through its own poll and the other through the
truck facts, so they agree. The flag is forgotten when the truck leaves both games or on a world or link change; the truck is
then a MULE truck again. Logs: `[vshare] MULE truck 0x......: the player got out: kept shared (a player drove it last) until a
non-player drives it` and `[vshare] MULE truck 0x......: a non-player drives it: a player's last drive no longer keeps it
shared`. The +0x19D bits are not used for this: they stay set from earlier drives and the other game may not have them.

**4. The "not holding" retry waits while the host's Sam is down.** A copy given back hidden and asleep as "not holding" (14.8)
used to be shown and woken after 5 s even while the host's Sam was down. The entry then refused it (`owner_down`), so a live,
unmirrored MULE stood beside her until he was back. Now the retry, and the off path's unhide, wait until he is back
(`erules::NotHoldUnhideNow`). Log, once per spell: `[enemy] down hold: n not-holding copies stay hidden and asleep here until
the host's Sam is back (no 5 s retry meanwhile)`.

**5. The "not holding" copies are shown again on a fault, a link change and a lost role, and forgotten on a world change.** A
fault (`[enemy] FAULT in ...`), a link change or the loss of the guest role left them hidden and asleep for the rest of the
session. They are now shown and woken there, as the other hidden sets are (`shown and woken again (a fault | the link changed |
no guest role ...)`). A world change forgets them without touching them, so an actor of the new world at the same address is
never touched. The line now counts them: `[enemy] the world changed: a + b + c hidden mirror(s) of the old world forgotten`.

**6. M2 probe P2 lever d' always restores our Sam's stimulus.** A probe fault inside the 1 s window left our Sam's visual
stimulus at 3 times its value. The fault path now restores it first (behind its own SEH guard): `[probe] P2 lever d': our
Sam's stimulus +0xA4 back to ... (a probe fault)`. Hidden runs only.

**Tests.** `TestSamDown` (enemy_rules_tests) covers `PeerDownNow`, `DownKeepsOff` and `NotHoldUnhideNow`. `TestBeacon`
(shot_wire_tests) covers `PeerTakesDown`. `TestPlayerLast` (vehicle_truck_rules_tests) covers a player getting out here and in
the other game, a MULE getting in afterwards in either game, a truck never driven by a player, and the release latch.

Open: the final gates (14.15) ran on this code and passed. Fix 1's line was logged by both games in both tie gates [O]. The
other paths did not come up in any gate (no real death, no lost beacon, no player in a MULE truck, no not-holding copy, no probe
fault), so they are still [U]. The guest's `GuestReset` is still never called (it was dead code before these fixes).

### 14.14 The M2 answers: what the probes decide

Design section 10 asked M2 for three outputs: the perception tier (1 or 2), the push lever, and the notice-timing constants of
design 5.3. Runs `20261125-154710` (`two-games-enemy-probe`, P0-P5) and `20261125-155517` (`trucks-m2-probe-solo`, P0, P1, P5,
P6), plus what the final truck gate showed for P6. The probe lines are in 14.11.

**P1: does a frozen copy keep its knowledge of our Sam?**

* Measured: the guest's frozen copy of 02029 with her Sam 10 m in front, then crouched behind it. Every 0.5 s the probe looked
  for the copy's DSAINoticeAndKnowledge and listed its entries. The same search ran on live MULEs (the host, solo).
* Result: the knowledge object was **not found** from any search root (the entity, its agent +0x170, its
  AIIndividualComponent and that component's +0x50, two levels deep), on a frozen copy or a live MULE. Not one object with
  that vtable was seen in 4700-7200 reads [O]. The object exists (its ctor writes the vtable [O]); it is most likely held by
  the camp or the AI manager [U].
* Decides: **perception tier 2.** We cannot read the copy's own perception, so "does this enemy see her" must be our own test
  built from the game's pieces (design 5.3), as critique R2 advised. Tier 1 comes back only if RE finds where the knowledge
  lives and shows that a frozen copy still updates it. R2 expects it does not, because the freeze switches the AI update off.
  The honest promise for the user stays "close to the game's stealth, not identical".

**P2: what makes a copy handed to the guest go for her Sam (the push)?**

* Measured: the host's 02029 raised to combat, then handed to her game by force (`enemy_own 0x2029 2`) with her Sam 10 m in
  front of it. One lever per try: a nothing, b a test hit owned by her Sam, c AlertForEntityWithPerfectThreatInfo -> her Sam,
  d' her Sam's own visual stimulus x3 for 1 s. The copy was sampled every 0.1 s for 15 s.
* Result: with every lever the copy was hers 0.2-0.3 s after the take and faced her (0-3 deg off, because it already faced
  her), and it **never fired in 15 s** [O]. Lever b's hit applied and lever c's call returned, with no effect.
* Decides: **no push lever.** None of a, b, c or d' makes a handed-over copy fight. Critique R3's other option (start the
  switch early and let the new owner's own perception finish the notice) is not supported either: the native brain did not
  finish it within 15 s. Before M3 can hand single enemies to her game, the next round must find out why this copy does not
  attack. In round 21 a whole camp handed over by proximity did chase and shoot her [O, 13.7], so a forced single take may
  differ from a camp take in a way we do not know yet [U]. The scenario now puts her Sam 10 m behind the copy, so the turn
  time can be measured too; that version has not run [U].

**P3: the ids the visual sense reads.**

* Measured: entity +0x314 of our Sam and of the partner body, and +0x88 of our Sam's visual stimuli in the AI manager's
  thread-safe visual list.
* Result: our Sam +0x314 = 0; the partner body +0x314 = 0x1FFFFF (none); our Sam's stimulus +0x88 = 0 (his actor id) on both
  games; the list held 42 (host) / 46 (guest) live entries, 1 of our agents [O].
* Decides: closes the round-20 correction. The partner body carries no actor id for the visual sense, so the game's AI cannot
  perceive her body directly; tier 2 runs in her own game against her own Sam (design 5.1). Tier 2 can find the local Sam's
  stimulus in that list. Since his id is 0, matching on +0x88 alone is weak: match on the agent too [I].

**P4: can the mod call the game's raycast?**

* Measured: from the guest's frozen copy's eye (+1.6 m) to her Sam's chest and head, 8-13 m away in the open, one call a tick
  for 5 s.
* Result: 300 calls, all clear, no fault [O].
* Decides: tier 2 can use the game's own line of sight, called once a tick on the game thread. A blocked case (a wall in
  between) was not measured, so whether it reports "blocked" correctly is [U].

**P5: how fast does a calm MULE notice the host's Sam (the 5.3 constants)?**

* Measured: the host's own 02029, the probe started with Sam placed at 35, 25, 12 and 5 m, standing and crouched (`pad B`),
  phase every 0.25 s for 6 s; and solo at about 11 m.
* Result [O]:
  * The 35 m steps: the MULE walked towards Sam, so he was 33 to 21 m away and 6-44 deg off its facing. No phase rise in 6 s,
    crouched or standing, although in the standing step he was inside the cone at 22-26 m for several seconds.
  * The 25 m steps: 26-35 m, 114-145 deg off (beside or behind it). No rise in 6 s.
  * 12 m: the MULE was at phase 5 (alert) when the probe began. It had noticed him in the 1.5 s between the placement and the
    probe, so there is no time to read.
  * 5 m: no rise in 6 s. The MULE walked away, so Sam was 5-16 m away and 111-168 deg off its facing (behind it).
  * Solo, about 11 m: phase 0 to 5 in 0.73 s; it turned from 135 to 10 deg off him in 2.8 s and walked in to 3 m. At the
    start Sam was 135 deg off its facing, outside a 60 deg cone, so this rise was probably not sight (perhaps the noise of the
    teleport) [I].
* Decides: **the constants are not fitted.** The starting values of design 5.3 stay as they are [I]. These runs agree with
  the cone: nothing was noticed from behind, even at 5 m. They hint that the meter is too fast at 20-26 m (it would fill in
  about 4 s in the cone; the game did not notice in 6 s) and too slow at about 12 m (the game noticed in under 1.5 s; the meter
  needs about 2.4 s; the round-22 control run turned within 2 s). Each point is one sample. Fitting needs a
  run that keeps Sam in front of a standing MULE (the placement did not hold because the MULE kept walking), a crouch that is
  checked (`pad B` was not verified), and grass. Not run.

**P6: where a seated MULE says it is in a vehicle.**

* Measured: every MULE within 400 m of Sam: its parent chain (entity +0x70) and its ride fields (actor +0xE20 the vehicle id,
  +0x3C0 the seat).
* Result: solo, after the camp alert, MULE 0202E sat in truck 0x02000F. Its parent chain reached a DSVehicleTruck, and the ride
  fields read 0x2000F seat 0: both tests agree [O]. In the truck gate (`20261125-171219`) five of the host's MULEs boarded
  0x02000F. Four showed the ride fields first (seats 0, 1, 2 and 4), and about 1 s later the parent chain, while their ride
  fields read none again. The fifth showed only the parent chain [O]. No MULE got out in any run.
* Decides: the in-vehicle test needs both checks, the ride fields while boarding and the parent chain once seated. 14.8 does
  both. The worry that stale ride fields keep a copy "in a vehicle" after it gets out is smaller than feared, because the
  fields clear while the MULE is still seated [I]. Getting out is [U]. For V3, a truck's MULE driver is read from the truck's
  unit +0x158 (0x00202E seen) [O].

**What this means for M3 / M4.** M4 (stealth) is planned on tier 2 with the game's raycast (P4) and the 5.3 meter, with
constants still to fit. M3 (one owner per enemy) is blocked on P2: a copy handed to her must fight her. The next probe should
compare a forced single take with round 21's camp take, and run P2 with her Sam behind the copy.

### 14.15 The final gate batch (2026-11-25, final code)

Run one at a time with Run-Batch (`-SeedSaves complete-100`; two-game runs with `-GuestSeed profile-only` and the launcher's
Local test files). Every run: result pass, no violation, saves restored, 0 `FAULT` / byte-check failure lines in any game's log.

| Scenario | Run | Result | What it showed |
|---|---|---|---|
| `two-games-enemy-tie` | `20261125-170739` | PASS, `enemy-tie.py` PASS | She tied up 0202F (test action, mode 1). The host's setter k 2 ran, read back restrained 1 s later; her OUTCOME 0.06 s after the HIT; her hold released after 1.6 s. Replays of 0202F: 7 before, 2 within 0.5 s, 0 later. The host's Sam test-down: 8 mirrors held and 1 hidden for 10.9 s, then all resumed, no lease release of 0202F meanwhile. Mirror p95 0.12 m. |
| `two-games-enemy-tie-host` | `20261125-170948` | PASS, `enemy-tie.py --reverse` PASS | The host tied up his follower of 02029. Her setter took it, read back restrained; his hold released on her SEN1 after 1.6 s. Her Fragile test-down: 7 followers held and 2 hidden for 24.6 s, ownership frozen, 0 changes held back. |
| `two-games-truck-hold` | `20261125-171219` | PASS, `truck-hold.py` PASS | 22 of 22 bound on the guest, 22 in use on the host. 0x020010 did not move (0.00 m). On the host five MULEs boarded 0x02000F and drove it 97 m. The guest gave her 5 mirrors of them back to her own game, and her own MULEs drove her copy 44 m: each game's MULEs drove their own copy, as V2 intends. 0 parked corrections on both. Mirror p95 0.09 m. |
| `two-games-enemy-camp` | `20261125-172205` | PASS | The round 21 camp gate. It needed a test-action fix (below). |
| `two-games-enemy-hit` | `20261125-172553` | PASS | Her bullet hits still reach the host (round 17). |
| `dev-give-solo` | `20261125-172815` | PASS | Round 22's `dev_give`. |
| `two-games-launcher-local-test` | `20261125-172923` | PASS | The launcher's Local test files. |

**The test-action fix for `two-games-enemy-camp`.** Its first attempt (`20261125-171500`) failed: no "the host's follower
hidden" line. In that run Sam's rifle knocked her copy of 02029 down, and `enemy_camp 4 0 1` (hide "the nearest owned copy")
then picked the downed copy. The follower guard rightly never hides a follower whose copy is down (13.4). The product was
right and the test was wrong: `enemy_camp 4` with id 0 now picks the nearest **standing** owned copy (`no standing owned copy
here` when there is none). A given id is taken as it is. The scenario's checks are unchanged.

**Seen in these runs, not yet explained [O].**

* The tied MULE got up in the host's game about 22 s after the tie-up (`restrained -> none (cause host)`), and her copy stood up
  with it. This happened in both tie runs (`162358`, `170739`). It may be the game's own untie (a comrade, or a timer) [U].
* After a long down hold, the held copies jump. In `170948` her MULEs kept fighting in her game during the 24.6 s, so on
  resume the host's followers were up to 97 m from her MULEs (7 `first over 20 m` lines, one worst line) and were then
  teleported back. After a real death both games reload anyway, so this matters mostly for the test trigger and for a long
  KnotSpace [I].

### 14.16 What stays unknown after round 23

* **A real strand or bola** on a copy. Only the test action ran. Does a frozen copy ever show bit 21 by itself? Is the strand
  HIT's weapon byte 41 (which the fallback needs)? Does a bola reach the relay at all? (14.7)
* **A real death or KnotSpace** of either player. Only the `enemy_sync 9` fake ran. Open: whether the host's reload turns her
  sync off (covered since 14.13, not run), whether his SEN1 "hidden" can arrive before his beacon's down code, and the host's
  followers when his gate turns off while her Fragile is down (still the usual off path). (14.9, 14.13)
* **The user's mirror jumps.** No gate reproduced them: p95 was 0.09-0.12 m and the 5 s "not holding" hide never fired. Its
  rule (10 m after 3 teleports in 2 s) has not run against a real case, and the cause of the 218 m error is still unknown. The
  new `mirror worst` / `first over 20 m` lines are there to name it in the user's next session. (14.8)
* **MULE trucks.** Each game now drives its own copy, so a truck a MULE drives can be in different places on the two screens
  until V3 (shared with its driver) is built. Not built: a MULE getting out, a player in a MULE truck (14.13 fix 3), and the
  vehicle share's failed rebind after `dev_enemies 1` (table 22, bound 19; seen since round 22, not fixed). (14.10, 14.12)
* **The M2 questions.** Where a MULE's knowledge lives (P1). Why a handed-over copy does not fire (P2). The turn time with her
  behind it (P2, not run). A blocked raycast (P4). The notice constants (P5). Getting out of a vehicle (P6). (14.14)
* **The untie after about 22 s** in the host's game, seen twice (14.15).
* The probe scenario's own `pad B` crouch was never checked on a screenshot.

### 14.17 Round 24: one MULE handed over alone (hidden runs only; six runs, 2026-11-25)

The question: when ONE alerted MULE is handed to her game while the rest of its camp stays with the host's game (frozen copies
in her world), does it fight her natively, and how fast? Round 23's P2 said "shot never". Run `154710` showed why that is not
the whole answer: 02029 hit her Sam with no shot of its own, a melee strike that P2 did not count.

* `enemy_own <id> 3` (the host; debug-actions.md) hands only that enemy over, with the usual OWN sequence (HostTakeOne, her
  GuestTake). The other members keep their owner. While it is hers its camp is held out of the camp rules: no CampDecide, no
  take, back or straggler, and a whole-camp `enemy_own .. 2` is refused. The loose-member loop leaves it alone. `enemy_own <id> 4`
  gives it back (HostBackStart, then HostBackDone), and the camp gets the usual 15 s hold. An ownership reset clears the
  singles. The single's own forced backs still apply while its camp is held (CampDecide on it as a camp of one with the hold
  never over: a lost OWNACK, her silence, the range, unrealized there, a game sequence on the host's copy), so a failed take
  never leaves the camp held until a reset.
* P2 now runs the whole 20 s window and counts the hits on our Sam per attacker, using the hit observer's "an owned enemy hit our
  Sam" attribution. A hit is a shot's when its shot count rose within 2 s before it, else melee. Hits under 300 ms apart are one
  attack. The result line carries `first attack after N ms (shot|melee), hits on our Sam n`. `[probe] P2 state` lines at the
  hand-over and every 1 s show the MULE's ea0 / ea4, its phase here, its camp actor's phase here, and whether the other members
  are frozen copies here. These rules are unit-tested in tests/enemy_probe_rules_tests.cpp.
* Scenario `two-games-enemy-single`: cp 04004, owned and alerted by the host. A: gun 0202B with her Sam 10 m in front. B: gun
  0202F (weapon 63) with her Sam 10 m behind. C: melee 02029 with her Sam 4 m in front. D (the control): the whole camp forced
  over, 0202B measured.

Results (runs `180910`, `181809`, `182106`, `182601`, `182930` and `183229`; guest `[probe] P2 result` lines; checked
independently in the round-24 review):

* A MULE handed to her game ALONE fights her with its own AI. The frozen camp-mates do not stop it. Gun 0202B, 10 m in
  front: first shot 1074 / 754 ms, 19 / 15 attacks. The whole-camp control on the same MULE: 967 / 533 ms. So round 23's
  "shot never" was a probe artefact: 02029 is a melee-only MULE (it never fires in any run), and that P2 took the whole camp.
* Melee 02029 alone, 4 m in front: it fought in 4 of 6 takes, with the first melee hit at 1055-1166 ms when it already
  faced her, or 3565-3661 ms when it turned away for about 1 s first. In 2 takes it walked away to about 60 m and never hit.
* Gun 0202F alone, her Sam 10 m behind it: it turned at 4.4 s and shot at 7.1 s in one run, and never noticed her in the repeat.
* The take carries the phase, but not the copy's knowledge (DSAINoticeAndKnowledge, entity +0x228 -> +0x720). A fresh copy has
  PlayerLastKnown at (0, 0, 0). Lever c (AlertForEntityWithPerfectThreatInfo) showed no measurable effect against its proper
  comparison (F vs L: same course, 3565 vs 3661 ms).
* The two walk-aways share three conditions the fights do not: her camp actor already at phase 4-5 before the take, a camp-mate
  running natively in her world (0202E, released from a vehicle), and two earlier single takes in the same camp. A camp-level
  order (regroup, or go to the vehicle) is at least as likely a cause as the missing knowledge. Not proven: 2 runs against 3.
* Hit counts are hit messages, not damage. A melee swing sends a burst of about 8, so compare "attacks".

What this decides for M3: the per-enemy design is workable. A switch from behind or after a camp alert is not yet reliable.
M3 still needs (a) the walk-away retested under its own conditions, lever a and lever c paired, 3 or more runs each, with P1 on
her copy, (b) B (behind) with lever c, and (c) the push found by then, or a switch only when the enemy faces the new player (S2's
"seen" already implies it).

### 14.18 Round 25: the WALK-AWAY and BEHIND conditions (hidden runs only; 11 runs and 2 regressions, 2026-11-25)

**The short answer.** Neither problem is fixed. Seven levers and probes were built, all off by default, and the ones that
write game state are refused outside hidden runs. None of them changed either behaviour measurably. The round narrowed both
causes but did not find them. Nothing is adopted: every round-25 key stays 0.

* WALK-AWAY: melee 02029 hits her once, walks 40-60 m to one fixed spot, about (1650.2, 1092.2), and shoots from there.
  Knowledge, tickets, its camp role and its camp's post spots are not the cause. What the spot is, and why only some starts go
  there, is open.
* BEHIND: a gun MULE with her Sam 10 m behind it starts to turn to her, turns back to its old heading, stands for about 2-3 s,
  and only then steps toward her and fires (turn 3.9-4.6 s, shot 4.3-7.2 s). Sometimes it never notices her at all.
  Knowledge is not the gate.

**What was built** (guest unless marked; all session.ini keys default 0; every site byte-checked at install and SEH-guarded,
and a fault turns that lever off for the session: `[enemy] take byte checks (17): K1 know ok, C1 camp ok, T1 tickets ok, R2
role ok, F1 face ok`):

* Diagnostics (with `enemy_m2_probes=1`): D1 position / facing / speed, D2 the copy's knowledge entries, D3 its CP actor (phase,
  alert position, entries, round-robin), D4 the camp-mates running in her world, D5 its group's ticket table, the P2 path summary
  (max distance, walked, stopped, first facing her), D7 the host following its copy 30 s, and D8 the camp's post spots (`[probe]
  P2 posts id=..`, read only: CP +0xF8[0..CP +0xF0], each locator at +0x40/+0x48/+0x50 as doubles [O]), written with D3's
  "before the take" line. Lines: debug-actions.md `enemy_m2`.
* K1 `enemy_take_know=1|2` (`enemy_take_know_secs` 1..10, default 3): at her take, the game's own forced player visual on the
  copy's knowledge object (DSAINoticeAndKnowledge, vtable 0x143E13028 [O]; force flag K+0x736, position K+0x6A0/+0x6A8/+0x6B0
  [O], consumed by ProcessSensorResults 0x142E8DAF0 -> upsert 0x142EA0370 [O]), 2 Hz while it does not face her or attack.
  `[enemy] take know id=..`.
* C1 `enemy_take_camp=1|2`: before the take's SetPhase, MuleCP_ForceSetCPPhase 0x142B649A0(guid [cp+0x18]+8, 5) [O] when its
  camp's CP is below 5. `[enemy] take camp cp ..`. A CPOrder appears 0.5 s after a real force [O].
* T1 `enemy_ticket_return=1`: at the take (and at a give-back), the tickets the frozen copies hold are returned through
  FUN_142e60b70(group, key, indiv) [O]; group from DSAIIndividual (AIIndividualComponent+0x50) via the FUN_14252b1a0 walk
  ([indiv+0x140] array, [indiv+0x138] count) [O]; table group+0x428 count, +0x430 entries of 0x20 [O].
* R2 `enemy_take_role=1`: the taken MULE's camp role (DSMuleGameActor +0x368; its only reader is FUN_142AFABC0, behind the HTN
  term DsMuleGetMuleRole0 [O]) reads 0 instead of 1 while it is hers, compare-then-write, restored at the end of the take.
  Byte checks: the getter 0x142AFABC0 and its call site 0x142ECB40D. `[enemy] take role id=..: role 1 -> 0 while hers (cp ..)`,
  `... given back: role restored to 1`.
* F1 `enemy_take_face=1`: at the take, while the copy is still frozen, it is turned in place to face her Sam when she is more
  than 60 deg off its facing and 0.5-30 m away, through kActorTeleport 0x1422F8D90. `[enemy] take face id=..: turned from N to
  M deg off (Sam .. m)` | `not turned (..)`.
* V1 `enemy_single_hold_vehicle=1`: a mirror's in-vehicle release waits while a member of its camp is hers (no game writes).
* R1 `enemy_single_rule=1` (host): RULE-W (a camp with a member hers earlier: the whole camp is taken, or the single is held)
  and RULE-B (only while it faces her, within 60 deg and 25 m, and alerted; held up to 20 s). `[enemy] single OWN id=.. held`.
* Scenarios: `two-games-enemy-single-walk` (BASE) and `-know`, `-camp`, `-tickets`, `-hold`, `-role`, `-face`, `-roleface`, the
  same cases in the same order: A (0202B gun, her Sam 10 m in front, the control), B (0202F gun, 10 m behind), C (02029 melee,
  4 m in front, 30 s: WALK), B2 (02030 gun, 10 m behind), C2 (02031, 4 m in front, 30 s).
* Review fixes (no behaviour change with the keys off): R2's restore record is a map per id with a world stamp, and the role is
  restored by a new hook `ecombat::TakeEnded(id, given_back)` on every end of a take (a failed refreeze and her reset with the
  link lost included; before, a second take or a failed refreeze could leave a live MULE at role 0). F1's turn returns 1 / 0 /
  -1 (fault), and a fault now turns F1 off. A give-back request (`enemy_own <id> 4`) drops a RULE-B held take of that id (`its
  RULE-B hold dropped`), so a held take can no longer go through after its give-back.

**Runs** (`build-testenv\runs\20261125-*`): build stage `195150` (BASE), `195801` (T1), `200441` (K1; failed at C2 only),
`200818` (C1); experiment `202127` and `204651` (BASE), `203831` and `205104` (R2), `204233` and `205856` (F1), `203330` (R2 + F1).
`203330`, `204233` and `200441` failed at C2 because the host refused 02031 (`single OWN id=02031 not handed over: not takeable`;
not a lever: R2 / F1 are guest-only); everything before that step is used. All 11 runs' guest and host logs were re-read in
the round-25 review, and every number below matches them.

**WALK: what the logs show** (02029; pass = first attack by 5 s and never more than 15 m from her Sam in the first 10 s)

* [O] Every walk ends at the same spot: (1650.1-1650.2, 1092.1-1092.7), stopped at 16.7-21 s, after the first melee hit at
  0.64-1.30 s. From there it shoots her at 42-61 m. After the give-back the host's copy is left at the spot (D7, `205856`:
  `target follow id=02029 t=21 s (L3 phase): at (1650.00, 1091.68, 177.10)`). The regression run `211807` did it again: 48.6 m,
  end (1650.2,1092.3), first melee 1316 ms, shot 18884 ms.
* [O] The split is clean: all 8 starts 40.5-58.6 m from the spot walked; all 3 starts 63.1 / 74.4 / 76.0 m away (`204233`,
  `203831`, `203330`) stayed within 3.5-4.3 m and fought.
* [I] But the start position is not controlled: it is wherever the host's copy happened to stand when her Sam was put 4 m in
  front of it. The same split is just as clean against the camp's post #0 (1620.3,1065.8): walks started 4.7-35.5 m from it,
  fights 45.5-55 m. And the fights are the runs where the host's copy had already moved west, toward the alert spot
  (1577.8,1118.6). "A leash on a fixed spot", "a leash on its post" and "the host's copy already pulled away" cannot be told
  apart with these samples. "Per MULE" is not shown either: 02031 also ended near the spot in `202127` ((1649.3,1088.6), still
  fighting).
* Not the cause:
  * knowledge: her PlayerVisual entry is there from 0.3 s [O] (K1 on 02029 was not a real test: one write, not consumed, it had
    already attacked);
  * tickets: every group holds 0 (the leaf DSAIGroup has +0x428 0 and a null table, its parent is an AIGroup) [O], so T1 had
    nothing to return;
  * the camp role: R2 wrote role 1 -> 0 (logged) and 02029 still walked 42.7 m (`205104`) [O];
  * the post spots: D8 lists 3 (#0 (1620.3,1065.8), #1 (1676.6,961.1), #2 (1517.9,992.0)); the spot is about 40 m from #0 and is
    none of them [O]. DsGetMuleNearestPostPosition0 0x142ECCFC0 -> FUN_142B3D600 takes the plain nearest of that list, and
    DsGetMuleMainPostPosition0 0x142ECA2D0 takes #0 [O], so both post terms point at #0.
* Round 24's three suspects, settled as far as these runs go:
  * the camp phase: 4-5 before every take, the 3 fights included, so phase 4-5 is not enough. C1 never changed it (every force
    reads `forced 5 (was 5/5/5; now 5/5/5)`, a no-op), and no take happened at phase 3 or lower, so "phase 4-5 is needed" is
    untested. The camp phase is not ruled out;
  * a camp-mate running in her world: "running 1" in every run, fights and walks alike (0202E, 94-103 m away in truck 2000F,
    seat 4). It does not separate them;
  * two earlier single takes in the camp: A and B always came before C. Also constant, so it does not separate them either.
* [U] What (1650.2,1092.2) is: a cover or tactical point, a route point, a combat-area anchor, or a move target the host's plan
  had set before the take.

**BEHIND: what the logs show** (0202F and 02030; pass = turned to within 30 deg by 3 s and attacked by 5 s)

* [O] From the 0.25 s D1 lines (`202127`, `205856`, `204233`): at 0.3-0.65 s it starts to turn toward her (83 deg off at
  0.65 s); then it turns BACK to its pre-take heading (0202F to 154-156 deg, it was 150; 02030 to 126 deg, it was about 132) and
  stands at 0.04-0.12 m/s for about 2-3 s; at about 4 s it steps 2.7 m while turning to her, and fires. BASE-like turn times
  3.86-4.58 s, shots 4.27-7.24 s, 10 of 10 late or never. This corrects the experiment's first reading ("it carries on a move it
  already had going"): the 2.7 m end point is "fixed" only because each MULE starts at the same place each run (F1 `204233`
  started elsewhere and ended elsewhere).
* [I] The likely cause is a stored desired heading or standing order in the copy (from the host's take yaw or from her own
  pre-freeze state) that a transform write does not touch.
* [O] A second mode: 0202F never notices her. ea4 stays 0x2040000 (no 0x40000000), no PlayerVisual, it moves 0.6 m or less:
  `195150`, `205104`, and the regression `211807` (3 of 7 BASE-like 0202F samples). In those runs the camp was back at phase 4 at
  the take.
* [O] Knowledge is not the gate: K1 wrote 6 forced visuals, each consumed in 15-186 ms (`200441`), and the turn times stayed
  3.91-4.08 s.
* [O] F1 turns it at once, and then it does the same thing: back to the old heading at 1.3-2.3 s (5 of 6), the same step, shots
  4.0-7.0 s, inside BASE's range.

**A third mode** (02031, the C2 control): in `203831` it was already running at 5.9 m/s when handed over and had NO knowledge
object (`P2 know id=02031 t=0.00: no knowledge object`; bits 0x42440000). It ran 62.7 m to (1669.2,991.4) and never attacked.
In `205104` it stayed within 6.5 m but lost PlayerVisual at 1.61 s and never attacked in 30 s. It fought in the other samples.

**Per-arm results** (arms are run-level, not paired within a run)

| Arm (key) | WALK 02029 | BEHIND 0202F / 02030 | Verdict |
|---|---|---|---|
| BASE / BASE-like | walked 7 of 8; the fight is the 63 m start | 10 of 10 late or never (2 never); turn 3.86-4.58 s, shot 4.27-7.24 s | the reference |
| K1 `enemy_take_know` | walked (58.6 m start; not a real test) | turn 4.08 / 3.91 s, shot 4.30 / 4.33 s | no effect; breaks stealth |
| C1 `enemy_take_camp` | walked; the force was a no-op (5/5/5) | turn 3.86 / 4.15 s | no effect; not a test of the phase; breaks stealth |
| T1 `enemy_ticket_return` | walked; "holds none" | no change | nothing to return |
| R2 `enemy_take_role` | walked 1 of 3; the 2 fights were 74-76 m starts, the 42.5 m start walked with role 0 written | 02030 turn 3.88 / 4.06 s; 0202F is role 0 (untouched) | no effect shown; confounded by the start |
| F1 `enemy_take_face` | 02029 never turned (it already faced her) | forced to 0 deg, turned back in 5 of 6; shots 4.0-7.0 s | no effect ("4 of 6 passes" are the forced 0 ms); breaks stealth |
| V1 `enemy_single_hold_vehicle` | not run | not run | no game writes |
| R1 `enemy_single_rule` | not run (no scenario arm) | not run | the only stealth-neutral candidate; untested |

* Control A (0202B, 10 m in front, no lever) shot at 0.76-1.09 s in every run (regression `211807`: 776 ms, and 536 ms in the
  whole-camp control D).
* 0 FAULT lines in both games of all runs; the byte checks were all ok in every run.
* Stealth: K1 injects her true position whether or not the MULE could perceive her; F1 turns an unaware MULE to face her; C1
  raises her whole camp to combat phase. Any of them would make a MULE attack a player it should not know about. R2 changes camp
  tactics. None may become a default.

**Regressions of the round's code** (keys at 0): `20261125-211807-two-games-enemy-single` PASS (121 steps; the round-24 picture
unchanged: A shot 776 ms, B 0202F never noticed, C 02029 walked to the spot, D 536 ms) and
`20261125-212126-two-games-launcher-local-test` PASS (100 steps; the launcher-written ini; 0 FAULT lines). Build OK, ctest 46/46.

**What stays open**

* WALK: what the spot is [U], and which of the three readings is right. Next: vary the start on purpose (the same MULE taken near
  post #0 vs 50 m or more away, with the host's copy idle vs already pursuing); a take with her camp at phase 3 or lower.
* BEHIND: the copy's desired heading / standing order at the take [I]; read it (locomotion or orientation target) at the take.
  The never-noticed mode's cause [U].
* 02031: the "running and unaware" mode, and the host's `not takeable` refusals (3 runs).
* R1 has 0 runs. Its RULE-W trigger ("a member was hers earlier") was true in all 11 runs, fights included, so it would not have
  told them apart. RULE-B's "alerted" is the host copy's own phase, which may come from an alert about the host's Sam, not hers.
* V1 is not limited to hidden runs (it writes nothing and is off by default, but the key changes the in-vehicle release in a
  normal session). Each take resets the take record, so K1's 2 Hz writes for an earlier single stop at the next take (fine while
  singles are one at a time). That R2's +0x368 (read at the actor) and the getter's [comp+0x58]+0x368 are the same object is
  supported by the values (0202B 2, 0202F 0, 02029 / 02030 1) but not shown directly [I]. D6 (the HTN task name) and a
  split-camp variant were not built.

**The design-rule consequence for M3** (the per-enemy switch)

* Hand a single enemy to the other game only while it already faces that player and is engaging them. That is the case that
  works every time (control A: 0.5-1.1 s in every run). A MULE facing away re-asserts its old heading and holds 2-3 s, or never
  notices her: keep it with its current owner until it has turned to her in the owner's game.
* The rule must not create awareness. No write of knowledge, facing or camp phase into her copy: they break stealth and did not
  help. The choice of WHEN to switch is the only safe lever, and it must use the owner's own view of who the enemy is engaging
  (RULE-B's "alerted" must mean alerted to HER, not just the host copy's phase).
* A switch of a lone MULE near its camp can send it to the WALK spot. Until the cause is found, prefer the whole-camp hand-over
  when the camp is alerted (round 24's whole-camp controls fought 4 of 4), with a better trigger than RULE-W's.
* Before any of this becomes a default: an R1 scenario arm, run 3 or more times, and the WALK start varied on purpose.

### 14.19 Round 26: every round-25 change ON by default, in real games too (at the user's request; 2026-11-25)

**Why.** None of the round-25 levers fixed WALK-AWAY or BEHIND (14.18), and K1, F1 and C1 break stealth. The user asked for all
of them to be enabled anyway so they can investigate in their own (real, non-hidden) games and give direction afterwards. This
reverses 14.18's "nothing is adopted"; it is not a verdict that any lever works.

**On by default now** (session.ini; the launcher writes none of these keys, so its Local test and Host / Join files get these
defaults; `[config]` logs them at start in both games):

| Key | Default | What it does | Off |
|---|---|---|---|
| `enemy_m2_probes` | 1 | her single takes get the read-only P2 window by itself (lever a, 20 s: D1-D5, D8, the P2 state / path / result lines) | `enemy_m2_probes=0` |
| `enemy_take_know` | 2 (`_secs` 3) | K1: the game's forced player visual of her Sam into the taken MULE, 2 Hz for 3 s | `enemy_take_know=0` |
| `enemy_take_camp` | 1 | C1: her camp forced to phase 5 before the take when it was already 4 or 5 | `enemy_take_camp=0` |
| `enemy_ticket_return` | 1 | T1: tickets her frozen copies hold returned at the take (and at that MULE's give-back) | `enemy_ticket_return=0` |
| `enemy_single_hold_vehicle` | 1 | V1: an in-vehicle mirror's release waits while a member of its camp is hers as a single take (no game writes) | `enemy_single_hold_vehicle=0` |
| `enemy_single_rule` | 1 | R1 (host): RULE-W / RULE-B on single takes (a hold, or the whole camp instead) | `enemy_single_rule=0` |
| `enemy_take_role` | 1 | R2: the taken MULE's camp role 1 reads 0 while it is hers, restored after | `enemy_take_role=0` |
| `enemy_take_face` | 1 | F1: the taken MULE turned in place to face her Sam (> 60 deg off, 0.5-30 m) | `enemy_take_face=0` |

**Stealth warning.** K1 tells the MULE where her Sam is whether or not it could see her; F1 turns an unaware MULE to face her;
C1 raises her copy of the camp to combat phase. With these on, a MULE she takes alone will attack her even when it should not
know about her. R2 changes camp tactics. To play stealthily, put `enemy_take_know=0`, `enemy_take_face=0`, `enemy_take_camp=0`
(and `enemy_take_role=0`) in both games' session.ini.

**How to start a single take in a real game.** Only the host can, from the debug menu (`debug_menu=1`, the launcher's default):
`enemy_own 0 3` hands the MULE nearest her body (that the host owns) to her game alone; `enemy_own <id> 3` a given one. Give it back
with `enemy_own -5 4` (the nearest one she owns) or `enemy_own <id> 4`. Ownership must be on (`enemy_owner`, on by default with
`enemy_hits`). With R1 on, the host's log answers `single OWN id=.. R1: a single take (...)` and `-> guest`, or `held: RULE-B
(...)` (waits up to 20 s for the MULE to face her within 60 deg and 25 m, alerted; else `not handed over: RULE-B held it 20 s`),
or `held: camp (...): the whole camp is taken instead` (another member of that camp was hers earlier in this world). Her log
then has `[probe] P2 armed id=.. lever a: by itself at her single take`, one line per lever (`[enemy] take know|camp|tickets|role|face
id=..`: what it did, or `skipped (<why>)` / `not forced (..)` / `kept`), and the P2 lines until `[probe] P2 result id=..`.

**What changed in the code (round 26):**

* The defaults (config.hpp, config_values.hpp TakeKeys; unit test: TakeKeys and Config agree, each key turns off with 0).
* Gates lifted: `LeverWhy` no longer refuses outside hidden runs (K1 / C1 / T1 / R2 / F1); the host's `enemy_own` (1 / 2 whole
  camp, 3 / 4 single) works in a real game. Still hidden runs only: the `enemy_m2` action itself, `enemy_probe`, `enemy_camp`,
  `enemy_restrain`, `enemy_target` (so D7, the host following its copy with `enemy_target 3`, does not run in a real game),
  `enemy_look`, `enemy_sync 6 / 7 / 9`, the shot tests and the fake downs. Byte checks, SEH guards and fault-turns-lever-off
  are unchanged.
* "Alone" (her game's test that a take is a single) now also needs a forced take (OWN reason 7, the host's `enemy_own`): before,
  the first member of every ordinary camp take (proximity) counted as alone, which with the levers on would have pushed K1 / C1 /
  F1 / R2 / T1 into every camp she walked up to. Review fix: it also needs to be the only forced take in its batch of OWN
  messages (`eprobe::TakeAlone`; `OwnDrain` counts them first), so a forced whole-camp take (`enemy_own .. 2`, or R1's "the
  whole camp is taken instead"), which the host sends as one OWN per member in one tick, has no member alone. Left: if the
  link splits such a take over two of her ticks, the first member alone in its batch still counts (not seen; the next ones
  never do, as the first is hers by then).
* A take that is not alone and has no P2 window does nothing in the take hooks (no lever, no line; before it reset the take
  record, which stopped an earlier single's K1).
* T1's give-back return (key on) is for a MULE she took alone only, not every give-back of a camp take.
* The P2 window (lever a: reads only) arms by itself at a take alone when `enemy_m2_probes=1` and no window is armed; an armed
  `enemy_m2 2` window (hidden runs) is kept.
* R1 logs a straight pass: `[enemy] single OWN id=.. R1: a single take (RULE-W: no member of <camp> hers earlier; RULE-B passed: ..)`.
* V1 is limited to singles (review fix; before, any take of a camp, also an ordinary one, held that camp's in-vehicle mirrors
  frozen at the host's frames, which with V1 on by default would have changed every camp fight with a MULE truck): her record
  of a take carries `alone`, and only an owned member taken alone holds its camp's in-vehicle release.
* R2's restore after a reset for a gone world (a load, a world change) is skipped unless this game's live world
  (`coop::WorldReady`) is still the take's (review fix: enemy_sync's reset runs before the combat tick in the same frame, so the
  combat side's world stamp was one tick old there); a reset for the link or the gate in the same world still restores it.
* The take byte checks at start-up run only with `enemy_sync=1` (or in a hidden run): with the keys on by default, a game with
  enemy sync off runs none of this (review fix; a take still checks them first).
* Scenarios: the round-24 / 25 baselines and the other two-game enemy scenarios pin every round-25 key they do not test to 0
  (`ini enemy_take_know=0` ...; the arms keep their own key on the guest), so they still test what they tested.
  `two-games-launcher-local-test` is not pinned (no `ini` line may change the launcher's file; it has no single take) and now
  runs the new defaults. New: `two-games-enemy-single-defaults` (the launcher's files, no lever key given: the `[config]` lines
  show the defaults in both games, one single take of 0202B in front of her with every lever's line, then 0202F behind her,
  where R1's RULE-W turns the request into a whole-camp take).
