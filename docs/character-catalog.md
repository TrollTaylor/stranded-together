# Character catalog: which bodies can the partner wear?

Status: **measured in game** (hidden runs, 2026-11-16, 100% save `-SeedSaves complete-100`, Episode 15
garage near Mountain Knot City). Every Type 4 (`Reward_Holo`) and Type 20 (`Reward_Special_Holo`) item of
the collectors DB was spawned, logged, photographed and removed. The animation clips loaded in memory were
surveyed too. This page answers "which characters exist, and can they walk?" for the launcher's character
selection.

## Short answer

| Wanted | Usable item | HashId | Notes |
|---|---|---|---|
| Generic porter | **Veteran Porter: Encouraging** | `B67D5CEE` | A porter in the Bridges porter suit with a chest rig, the closest thing to Sam. Alternatives: `918D9AC1` (same porter, cargo idle), `D0461DC3`, and the helmeted Bridges guard `FD1B3326` (today's default). DS2's ambient porters do not exist in DS1. |
| Fragile | **Fragile: Attentive** | `68BB8D2C` | `E729486F` is Fragile with an umbrella. |
| Die-Hardman | **Die-Hardman: Encouraging** | `31679C53` | Masked, in a suit. `4F26CE22` is the same model. |
| Deadman | **Deadman: Admiring** | `76571C36` | `FFE30DB2` only has a "worried" loop, no idle. |
| Heartman | **Heartman: Sending Likes!** | `0E9A0AA3` | `E29DE017` lies on the floor (resuscitation pose): not usable. |
| Mama | **Mama: Loving** | `A9014BFD` | `F1B18D5F` holds a single "connected" pose. |
| Lockne | **Lockne: Encouraging** | `24FD1DC7` | `09E39D1E` holds a single "connected" pose. |
| Higgs | Higgs (pose only) | `747959B2` | Hooded, gold mask. No idle loop: his only clip is a 15 s "welcome" pose. The other two Higgs items have no welcome-hologram component. Usable, but he stands in a pose. |
| Amelie | **Amelie: Beckoning** | `A17019FF` | Warmer, reddish hologram tint. |
| Sam | **none in the DB** | - | No collectors item shows Sam. See "Sam for the host" below. |
| Cliff Unger | **none** | - | Not in the collectors DB and not in the loaded world. |
| BB / Lou | **none** | - | Not in the collectors DB. |

* **Every body is a hologram.** All 90 items render as translucent, scan-lined holograms (the welcome-
  hologram shader, `ShaderAnimComponent` variable `HeightCutValue`). None looks solid.
* **Every human character shares Sam's skeleton layout** (77 joints, `SkeletonLayoutHash` `BE39C83F`).
  - The Bridges guards, the porters and most male NPCs use the very same `Skeleton` object as Sam.
  - The story characters (Fragile, Die-Hardman, Deadman, Mama, Lockne, Heartman, Higgs, Amelie) and the female NPCs have their own `Skeleton` object with the same layout hash.
  - So clips from one should play on any other.
* **No body has a walk or run clip.**
  - Every clip of every item has `LocomotionDeltaTranslation` = 0 (one raccoon gesture moves 8 cm).
  - Clip 0 is an idle (`*_idle_lp`) or a pose; the others are gestures (wave, nod, salute, clap...).
  - Every clip has `LoopMode` = Hold. Looping is chosen at play time (`PlayMotion` mode 1), not by the clip.
* **Spawning is safe.** 90 of 90 spawned, validated, became visible (flags bit 0x2 after 1.5 s) and were
  removed. There were 0 refusals, 0 faults, 0 crashes and 0 spawner leaks, and every removed entity had left the world list
  0.4 s later. Load time was 0.02 to 0.33 s.

## Recommended launcher list

The host is Sam (his own body). The guest picks one of these (`session.ini partner_holo=<HashId>`):

| Display name | HashId | Idle (clip 0) | Notes |
|---|---|---|---|
| Porter | `B67D5CEE` | `an_mulhw1_s_idle_lp` (18.4 s) | default; Sam's own skeleton |
| Porter (helmet) | `FD1B3326` | `an_bgshw1_s_idle_lp_a` (12.0 s) | today's default; Sam's own skeleton |
| Fragile | `68BB8D2C` | `an_fglhw1_s_idle_lp` (30.0 s) | |
| Die-Hardman | `31679C53` | `an_dhmhw1_s_idle_lp` (12.3 s) | |
| Deadman | `76571C36` | `an_ddmhw1_s_idle_lp` (14.3 s) | |
| Heartman | `0E9A0AA3` | `an_htmhw1_s_idle_lp` (10.0 s) | |
| Mama | `A9014BFD` | `an_mamhw1_s_idle_lp` (30.0 s) | |
| Lockne | `24FD1DC7` | `an_lknhw1_s_idle_lp` (30.0 s) | |
| Amelie | `A17019FF` | `an_amlhw1_s_idle_lp` (7.7 s) | |
| Higgs | `747959B2` | `an_hgshc1_s_pose_welcome` (15.4 s, a pose) | optional: no idle |

The run used a 100% save. On a fresh save the DB still lists every item, but it has not been tested whether a
locked item's display entity streams in. The launcher should therefore keep the porter as the
fallback, and the mod already falls back when an item is missing or refused. Aloy (`6C4A8F5F`),
Jackie Welles (`A6783960`) and the "Creator" (`841DE7AB`) are humans with their own, different skeletons.
They could be shown with their own idle, but borrowed walk clips would not fit them.

## Animation finding

There is no walk or run clip for these bodies, and there is none on Sam's skeleton layout anywhere in memory:

* **Clip survey** (`action clip_survey`, run `20261116-171015-holo-catalog-survey`):
  - It scanned 11.1 GB of committed memory in 90 s and found **8,387 loaded `SkeletonAnimationResource`s**.
  - **386** of them are on Sam's layout `BE39C83F`.
  - **Only 6** of those have root motion above 0.1 m: four cutscene shots plus `an_samppm_s_terminal_st`/`_ed`. None is a walk or run.
  - Sam's real locomotion lives in his Morpheme network (`MorphemeAnimationManagerResource`, an `AnimationManager`). It is not made of `SkeletonAnimationResource`s, so it cannot be borrowed this way.
* **Walk cycles that stay in place do exist on Sam's layout** (|dT| = 0: they walk on the spot). These are the clips to borrow:

  | Clip | Duration | Skeleton |
  |---|---|---|
  | `ds/animations/game/sam/samppm/an_samppm_s_farewall_walk_privateroom` (Sam) | 2.84 s | **Sam's own** |
  | `ds/animations/game/dhm/dhmppm/an_dhmppm_s_appear_walk_01` (Die-Hardman) | 3.34 s | own, layout `BE39C83F` |
  | `ds/animations/game/ddm/ddmppm/an_ddmppm_s_appear_walk_02` (Deadman) | 3.34 s | own, layout `BE39C83F` |
  | `ds/animations/game/htm/htmppm/an_htmppm_s_appear_walk_01` (Heartman) | 2.67 s | own, layout `BE39C83F` |
  | `ds/animations/game/mam/mamppf/an_mamppf_s_appear_walk_talk_01` (Mama) | 3.17 s | own, layout `BE39C83F` |

  - They were loaded in the distribution centre: the private-room "appear" and "farewell" scenes.
  - **Not known yet:** whether they stay loaded out in the field (the next step must check this away from a distribution centre), and whether they loop cleanly.
* **The named sources from the research are all static** (all on Sam's layout, no root motion):
  - `DSPlayerEntityResource.PlayerPoseAnimations` (+0xA18): 45 photo-mode poses `an_sampho_s_pose_NN`, 0.02 s each.
  - `PlayerPoseAnimations_PrivateRoom` (+0xA28): 16 poses.
  - `[DSPlayerSystem]+0x207C8` `DSExternalAnimationResource`: 49 private-room clips `an_sampri_q_*`, such as bed idles and "look at" clips.

**What to use (proposal for step 2):**

| State | Clip | Play | Rate |
|---|---|---|---|
| Idle (speed < 0.3 m/s) | the item's own clip 0 | `PlayMotion(comp, 0, 1 /*loop*/, 1.0, 0.3, 0)` | 1.0 |
| Walk (0.3 to 2.5 m/s) | borrowed `an_samppm_s_farewall_walk_privateroom`, else `an_dhmppm_s_appear_walk_01` | mode 1 (loop) | `speed / 1.4`, clamped to 0.6 to 1.8 |
| Run (> 2.5 m/s) | the same walk clip | mode 1 | `speed / 1.4`, clamped to at most 2.2 (a fast walk; no run clip exists) |

- **Rate.** The 1.4 m/s stride speed is a guess to calibrate on screen: these clips carry no root motion, so the research formula `speed / (|dT| / Duration)` cannot be used.
- **No spontaneous gestures.** Write `comp+0x9C = 1e9` every frame, so the welcome hologram never starts a gesture on its own.
- **Borrowing a clip.** A borrowed clip is not in the component's own list, so playing it means mirroring `0x14265B230` (research section 2). AddRef the clip and release it ourselves.
- **Skeleton check.** Compare `SkeletonLayoutHash` (Skeleton +0x70) on both sides, not the Skeleton pointer: only the porters and guards share Sam's actual object.
- **Fallback.** Where no walk clip is loaded, play the idle (clip 0) while the body glides. That is today's behaviour plus the idle motion.

## Sam for the host

There is no Sam hologram. The host is Sam, so the host needs none himself. But the guest has to see the
host as someone. These are the candidates; none has been tested yet:

1. **`DSPlayerSubspaceConfig.SubspacePlayerResource`**: resource `DSPlayerAKukanEntityResource`,
   `ds/entities/player/akukan/ds_player_akukan_entity`. This is the Sam of the cargo and customise menus.
   - It is resident (logged from the census).
   - Its 10 component resources include `MorphemeAnimationManagerResource`, `DSPlayerAKukanInventoryComponentResource`, `DSBaggage2CarrierComponentResource` and `FixedPositionMoverResource`.
   - Its entity type has not been checked against Humanoid / DSPlayerEntity.
   - Its `DSPlayerAKukan*` components would trip the mod's `DSPlayer*` refusal as written. It needs a dedicated look before anything creates it.
2. **The Veteran Porter** (`B67D5CEE`) as a "porter Sam". It is the most Sam-like body that is safe today.

## Story-character holograms in the world (census)

The distribution centre's private room keeps cinematic holograms of the cast resident and invisible
(flags `0x...51`):

| Resource (under `ds/entities/cinematics/`) | Character |
|---|---|
| `demo_part_fgl_body_holo_new` | Fragile |
| `demo_part_lkn_body_holo_new` | Lockne |
| `part_htm_body_holo_new` | Heartman |
| `part_mam_body_holo_new` | Mama |
| `part_ddm_body_holo_new` | Deadman |
| `part_dhm_body_holo_new` (two of them) | Die-Hardman |
| `demo_part_bdv_body_def_holo_v06_new` | not identified |

- All are `DSControlledEntity` with `AIEntityType` 0.
- Each has `ExternalAnimationComponent`, `DSPrivateRoomItemComponent`, `DSHumanHeadAimToPlayerComponent` and `DSTalkComponent`, and its own skeleton with Sam's layout.
- They are driven by external animation and sequences, not by `DSSimpleAnimation`, and they exist only around a distribution centre.
- The collectors items above cover the same people and load anywhere, so these are not needed.

The world held 78 ControlledEntities there: 73 `DSControlledEntity`, 4 `DSDeliveryBotEntity` and Sam.
No MULE, no porter NPC and no other Humanoid was loaded.

## How it was measured

* **`action holo_catalog <start> <count> [distance_m]`** (debug action "Holo catalog", `native/mod/holo_catalog.cpp`) walks the sorted Type 4/20 list (the index order is logged once). For each item:
  1. **Spawn.** It spawns the item through the partner hologram's byte-checked spawner path (`holo::SpawnBegin`, `SpawnPoll` and `SpawnTeardown` in `partner_holo.cpp`). The item stands 5 m ahead of Sam and 1.5 m to his right, facing him.
  2. **Validate.**
     - Base checks: vtable in ds.exe, not Humanoid or DSPlayerEntity, not the player's Wake override, has a model, no `DSPlayer*` component.
     - Items with a `DSWelcomeHoloComponent` also pass the partner's welcome-hologram checks and get `StartVisible`.
  3. **Log what it is:**
     - item fields: HashId, Type, DisplayNo, `NameText`/`Text` decoded from `LocalizedTextResource`, MajorMemberId, IsHiddenItem;
     - the entity resource: type, Location, Name, AIEntityType and component resources;
     - the entity's components;
     - the `DSSimpleAnimation` clip list: index, Location, Duration, LoopMode, LocomotionDeltaTranslation and Skeleton;
     - its skeleton against Sam's (pointer, joints, layout hash) and its shader variables.
  4. **Wait 1.5 s.** Then it logs visibility, drift and the animation state.
  5. **Screenshot.** Written to `automation\out\holo-<HashId>.png` at half size.
  6. **Remove.** Then it checks that the entity has left the world list 0.4 s later.
  - The first call also logs a census: the three subspace resources, and every ControlledEntity in the world (`[0x147BC4E70]+0x438`), with the ones within 200 m in detail.
* **`action clip_survey`** (debug action "Clip survey"):
  - On the game thread, it logs the named clip sources.
  - A worker thread then scans committed read-write memory for the `SkeletonAnimationResource` vtables `0x143C10818` and `0x143C10A28`. Both were found statically from the GetRTTI getter `0x141D84EE0` and verified at runtime by decoding that getter. All reads use ReadProcessMemory.
  - The survey ends with `[survey] done`.
* **Scenarios**, each run with `-SeedSaves complete-100`:
  - `tools/testenv/scenarios/holo-catalog-cal.txt`: 4 items, to check the framing.
  - `holo-catalog-all.txt`: all 90 items in three batches of 30, about 4 minutes.
  - `holo-catalog-survey.txt`: the clip survey plus re-takes.
* **Names.** `NameText` is empty for all 90 items. The readable name is in `Text` ("Fragile: Attentive"), in the game's current language. `MajorMemberId` is 0 (None) for all of them.

Runs:
- `build-testenv\runs\20261116-170308-holo-catalog-all`: all 90 items. Passed: game exit 0, real saves unchanged, 0 cloud uploads.
- `...-171015-holo-catalog-survey`: the clip survey, plus a re-take of item 79, whose PNG write failed once in the full run.
- `...-170114-holo-catalog-cal`: the framing check.

## All items

Columns:
- *Idle loop clip*: clip 0 when it is an `*_idle_lp`.
- *Root motion*: the largest |LocomotionDeltaTranslation| over all clips of the item.
- *Skeleton vs Sam*: "Sam's own" means the same Skeleton object; "same layout" means the same `SkeletonLayoutHash` `BE39C83F` and 77 joints.

Every item looks like a hologram. Items 49, 61, 63 and 64 are the "OneMotion" variants: they have no welcome-hologram
component and a single clip. Items 55, 57 and 59 have the component but also only a single clip (a pose).

| # | HashId | Type | Name (game text) | Depicts (by sight) | Idle loop clip | Other clips | Root motion | Skeleton vs Sam | Spawn | Partner? |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | FD1B3326 | 4 | Bridges Guard: Welcoming | helmeted Bridges guard in tactical armour (the default partner today) | 0: `an_bgshw1_s_idle_lp_a` (12.0 s) | 1 `wave_hand_01`, 2 `wave_hand_02`, 3 `nice_a`, 4 `salute_b`, 5 `nod_a` | none | Sam's own | yes | porter (helmet) |
| 1 | D2039157 | 4 | Bridges Guard: Encouraging | helmeted Bridges guard | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `raise_hands_b`, 2 `fist_pump`, 3 `point_l_hyper` | none | Sam's own | yes | yes (generic NPC) |
| 2 | 63461EB5 | 4 | Bridges Guard: Cheering | helmeted Bridges guard | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `cann_cheers_a`, 2 `cann_cheers_a`, 3 `nice_both_hands`, 4 `raise_hands_a` | none | Sam's own | yes | yes (generic NPC) |
| 3 | B43F19A2 | 4 | Bridges Guard: Macho | helmeted Bridges guard | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `muscle_pose_a`, 2 `muscle_pose_b` | none | Sam's own | yes | yes (generic NPC) |
| 4 | 4D24D903 | 4 | Bridges Guard: UCA! | helmeted Bridges guard | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `body_gesture_uca`, 2 `salute` | none | Sam's own | yes | yes (generic NPC) |
| 5 | C5DFF44C | 4 | Nick Easton | man in Bridges jumpsuit and headset | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `wave_hand_01`, 2 `wave_hand_03`, 3 `nod`, 4 `handclap_a`, 5 `nice_b`, 6 `point_l` | none | Sam's own | yes | yes (generic NPC) |
| 6 | C3361C88 | 4 | George Baton | man in white jumpsuit | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `wave_hand_01`, 2 `wave_hand_03`, 3 `nod`, 4 `handclap_a`, 5 `nice_b`, 6 `point_l` | none | Sam's own | yes | yes (generic NPC) |
| 7 | 9E414C95 | 4 | Benjamin Hancock | bare-headed man in tactical armour | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `wave_hand_01`, 2 `wave_hand_03`, 3 `nod`, 4 `handclap_a`, 5 `nice_b`, 6 `point_l` | none | Sam's own | yes | yes (generic NPC) |
| 8 | 20A9B20E | 4 | Jake Wind | man in Bridges jumpsuit and headset, waving | 0: `an_ppmhw6_s_bla_kata_idle_lp` (15.5 s) | 1 `bla_kata_greet_01`, 2 `bla_kata_nice_a` | none | Sam's own | yes | yes (generic NPC) |
| 9 | 56C8D991 | 4 | Viktor Frank | man in Bridges jumpsuit, beanie | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `wave_hand_01`, 2 `wave_hand_03`, 3 `nod`, 4 `handclap_a`, 5 `nice_b`, 6 `point_l` | none | Sam's own | yes | yes (generic NPC) |
| 10 | B84ABE10 | 4 | William Lake | man in Bridges jumpsuit, cap | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `wave_hand_01`, 2 `wave_hand_03`, 3 `nod`, 4 `handclap_a`, 5 `nice_b`, 6 `point_l` | none | Sam's own | yes | yes (generic NPC) |
| 11 | 490F3CFA | 4 | Thomas Southerland | bearded man in white jumpsuit | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `raise_hands_b`, 2 `nice_both_hands`, 3 `wave_hand_big_01`, 4 `wave_hand_big_03`, 5 `big_ok`, 6 `point_l` | none | Sam's own | yes | yes (generic NPC) |
| 12 | 805B8795 | 4 | Alex Weatherstone | bald man in dark suit | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `wave_hand_01`, 2 `wave_hand_03`, 3 `nod`, 4 `handclap_a`, 5 `nice_b`, 6 `point_l` | none | Sam's own | yes | yes (generic NPC) |
| 13 | 121896BB | 4 | Owen Southwick | man in Bridges jumpsuit and headset | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `cann_cheers_a`, 2 `cann_cheers_a`, 3 `raise_hands_b`, 4 `nice_both_hands` | none | Sam's own | yes | yes (generic NPC) |
| 14 | 25B3E2A7 | 4 | Charles Khan | bare-headed man in tactical armour | 0: `an_bgshw1_s_idle_lp_a` (12.0 s) | 1 `wave_hand_01`, 2 `wave_hand_02`, 3 `nice_a`, 4 `salute_b`, 5 `nod_a` | none | Sam's own | yes | yes (generic NPC) |
| 15 | 6311159B | 4 | Phillip North | man in tactical armour, cap | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `wave_hand_01`, 2 `wave_hand_03`, 3 `nod`, 4 `handclap_a`, 5 `nice_b`, 6 `point_l` | none | Sam's own | yes | yes (generic NPC) |
| 16 | BF8E0AD5 | 4 | Aaron Hill | man in Bridges jumpsuit and headset | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `wave_hand_01`, 2 `wave_hand_03`, 3 `nod`, 4 `handclap_a`, 5 `nice_b`, 6 `point_l` | none | Sam's own | yes | yes (generic NPC) |
| 17 | BD5C671D | 4 | Ludens Fan | civilian man, dark clothes | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `raise_hands_b`, 2 `nice_both_hands`, 3 `wave_hand_big_01`, 4 `wave_hand_big_03`, 5 `big_ok`, 6 `point_l` | none | Sam's own | yes | yes (generic NPC) |
| 18 | 90A7A570 | 4 | Musician | civilian man, long hair | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `raise_hands_b`, 2 `nice_both_hands`, 3 `wave_hand_big_01`, 4 `wave_hand_big_03`, 5 `big_ok`, 6 `point_l` | none | Sam's own | yes | yes (generic NPC) |
| 19 | 76101FDA | 4 | Engineer | civilian man | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `wave_hand_01`, 2 `wave_hand_03`, 3 `nod`, 4 `handclap_a`, 5 `nice_b`, 6 `point_l` | none | Sam's own | yes | yes (generic NPC) |
| 20 | 719CCEB2 | 4 | Elder | old man in vest | 0: `an_pmohw1_s_idle_lp` (15.7 s) | 1 `wave_hand_small`, 2 `nice_a`, 3 `nod`, 4 `talk_a` | none | Sam's own | yes | yes (generic NPC) |
| 21 | E3F9902B | 4 | Craftsman | civilian man, dark shirt | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `wave_hand_03`, 2 `nod`, 3 `handclap_a`, 4 `salute`, 5 `blg0_weapon_check` | none | Sam's own | yes | yes (generic NPC) |
| 22 | 936CAB21 | 4 | Timefall Farmer | civilian man, beanie | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `cann_cheers_a`, 2 `cann_cheers_a`, 3 `raise_hands_b`, 4 `nice_both_hands` | none | Sam's own | yes | yes (generic NPC) |
| 23 | 13F856B0 | 4 | Environmental Scientist | woman, light clothes | 0: `an_ppfhw1_s_idle_lp` (30.0 s) | 1 `nod`, 2 `wave_hand_01`, 3 `nice_a`, 4 `wave_hand_small_01`, 5 `handclap_a` | none | same layout | yes | yes (generic NPC) |
| 24 | F2AFECA6 | 4 | Junk Dealer | civilian man, dark sweater | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `wave_hand_01`, 2 `wave_hand_03`, 3 `nod`, 4 `handclap_a`, 5 `nice_b`, 6 `point_l` | none | Sam's own | yes | yes (generic NPC) |
| 25 | EC213553 | 4 | Chiral Artist | young woman | 0: `an_ppfhw1_s_idle_lp` (30.0 s) | 1 `wave_hand_jump`, 2 `curtsy`, 3 `wave_hand_02`, 4 `nice_b`, 5 `handclap_a` | none | same layout | yes | yes (generic NPC) |
| 26 | 1D9F8BFB | 4 | Chiral Artist's Mother | woman, blue shirt | 0: `an_ppfhw1_s_idle_lp` (30.0 s) | 1 `nod`, 2 `wave_hand_01`, 3 `nice_a`, 4 `wave_hand_small_01`, 5 `handclap_a` | none | same layout | yes | yes (generic NPC) |
| 27 | EE6E9765 | 4 | Collector | older man, white shirt | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `wave_hand_01`, 2 `wave_hand_03`, 3 `nod`, 4 `handclap_a`, 5 `nice_b`, 6 `point_l` | none | Sam's own | yes | yes (generic NPC) |
| 28 | 3E4E85A7 | 4 | Film Director | bearded man | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `raise_hands_b`, 2 `nice_both_hands`, 3 `wave_hand_big_01`, 4 `wave_hand_big_03`, 5 `big_ok`, 6 `point_l` | none | Sam's own | yes | yes (generic NPC) |
| 29 | 1219C688 | 4 | Wandering MC | man in visor helmet (cosplay) | 0: `an_pmthw1_s_idle_lp` (15.0 s) | 1 `handclap_a`, 2 `kenpo_pose_b`, 3 `muscle_pose_b`, 4 `body_gesture_uca`, 5 `wave_hand_big_03` | none | same layout | yes | yes (generic NPC) |
| 30 | 980BF692 | 4 | Cosplayer | woman, dark top | 0: `an_ppfhw1_s_idle_lp` (30.0 s) | 1 `wave_hand_jump`, 2 `curtsy`, 3 `wave_hand_02`, 4 `nice_b`, 5 `handclap_a` | none | same layout | yes | yes (generic NPC) |
| 31 | 2C46CC03 | 4 | Mountaineer | man in open jacket | 0: `an_mulhw1_s_idle_lp` (18.4 s) | 1 `gesture_iine_f`, 2 `gesture_geeting_a`, 3 `gesture_geeting_b`, 4 `gesture_geeting_c`, 5 `gesture_geeting_d` | none | Sam's own | yes | yes (generic NPC) |
| 32 | 79F31E30 | 4 | Mountain Guide | woman holding a baby | 0: `an_ppfhw5_s_idle_lp` (17.5 s) | 1 `look_baby`, 2 `wave_hand_01`, 3 `wave_hand_02` | none | same layout | yes | yes (generic NPC) |
| 33 | B5F61B53 | 4 | Photographer | woman with camera strap | 0: `an_ppfhw1_s_idle_lp` (30.0 s) | 1 `camera_gesture`, 2 `wave_hand_small_01`, 3 `handclap_a` | none | same layout | yes | yes (generic NPC) |
| 34 | EBFAF40A | 4 | Photographer's Mother | older woman | 0: `an_ppfhw1_s_idle_lp` (30.0 s) | 1 `nod`, 2 `wave_hand_01`, 3 `nice_a`, 4 `wave_hand_small_01`, 5 `handclap_a` | none | same layout | yes | yes (generic NPC) |
| 35 | 48A2A8FF | 4 | Doctor | older man, collarless shirt | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `wave_hand_01`, 2 `wave_hand_03`, 3 `nod`, 4 `handclap_a`, 5 `nice_b`, 6 `point_l` | none | Sam's own | yes | yes (generic NPC) |
| 36 | 4BC32CDF | 4 | Medical Device Engineer | young woman, white sweater | 0: `an_ppfhw1_s_idle_lp` (30.0 s) | 1 `nod`, 2 `wave_hand_01`, 3 `nice_a`, 4 `wave_hand_small_01`, 5 `handclap_a` | none | same layout | yes | yes (generic NPC) |
| 37 | CD9E6EB5 | 4 | Spiritualist | woman sitting cross-legged (meditating) | 0: `an_ppfhw6_s_idle_lp` (10.2 s) | 1 `wave_hand_01`, 2 `nod` | none | same layout | yes | yes (generic NPC) |
| 38 | C765F8D7 | 4 | Geologist | man, light sweater | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `raise_hands_b`, 2 `nice_both_hands`, 3 `wave_hand_big_01`, 4 `wave_hand_big_03`, 5 `big_ok`, 6 `point_l` | none | Sam's own | yes | yes (generic NPC) |
| 39 | 44AD2983 | 4 | Paleontologist | man, beanie and jacket | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `raise_hands_b`, 2 `nice_both_hands`, 3 `wave_hand_big_01`, 4 `wave_hand_big_03`, 5 `big_ok`, 6 `point_l` | none | Sam's own | yes | yes (generic NPC) |
| 40 | EA357E63 | 4 | Evo-devo Biologist | woman, white shirt | 0: `an_ppfhw1_s_idle_lp` (30.0 s) | 1 `nod`, 2 `wave_hand_01`, 3 `nice_a`, 4 `wave_hand_small_01`, 5 `handclap_a` | none | same layout | yes | yes (generic NPC) |
| 41 | 5BDAC9F7 | 4 | The Roboticist | woman, dark top | 0: `an_ppfhw1_s_idle_lp` (30.0 s) | 1 `nod`, 2 `wave_hand_01`, 3 `nice_a`, 4 `wave_hand_small_01`, 5 `handclap_a` | none | same layout | yes | yes (generic NPC) |
| 42 | 50B77F5B | 4 | The Roboticist's Son | small boy | 0: `an_ppchw1_s_idle_lp` (19.0 s) | 1 `wave_hand_01`, 2 `wave_hand_big_01` | none | same layout | yes | yes (generic NPC) |
| 43 | 16AE1905 | 4 | Novelist's Son | young man, t-shirt | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `wave_hand_01`, 2 `wave_hand_03`, 3 `nod`, 4 `handclap_a`, 5 `nice_b`, 6 `point_l` | none | Sam's own | yes | yes (generic NPC) |
| 44 | 61D8D225 | 4 | First Prepper | bearded man, t-shirt | 0: `an_mulhw1_s_idle_lp` (18.4 s) | 1 `gesture_iine_f`, 2 `gesture_geeting_a`, 3 `gesture_geeting_b`, 4 `gesture_geeting_c`, 5 `gesture_geeting_d` | none | Sam's own | yes | yes (generic NPC) |
| 45 | B67D5CEE | 4 | Veteran Porter: Encouraging | PORTER: man in a Bridges porter suit with chest rig (Sam-like porter) | 0: `an_mulhw1_s_idle_lp` (18.4 s) | 1 `gesture_iine_f`, 2 `gesture_geeting_a`, 3 `gesture_geeting_b`, 4 `gesture_geeting_c`, 5 `gesture_geeting_d` | none | Sam's own | yes | PORTER (best) |
| 46 | 918D9AC1 | 4 | Veteran Porter: On a Delivery | PORTER: same porter suit, carrying cargo | 0: `an_ppmhw8_s_bsm_idle_lp` (7.7 s) | 1 `bsm_cheer`, 2 `bsm_nice`, 3 `bsm_wave_hand_a` | none | Sam's own | yes | porter (alt) |
| 47 | D0461DC3 | 4 | Veteran Porter: Kung Fu | PORTER: same porter suit (kung-fu pose) | 0: `an_ppmhw1_s_idle_lp` (15.0 s) | 1 `kenpo_pose_a` | none | Sam's own | yes | porter (alt) |
| 48 | A17019FF | 20 | Amelie: Beckoning | Amelie, red dress | 0: `an_amlhw1_s_idle_lp` (7.7 s) | 1 `reach_out`, 2 `hopeful` | none | same layout | yes | AMELIE (best) |
| 49 | 90C12CCC | 20 | Amelie: Yearning | Amelie, red dress, looking back | none | 0 `look_back` | none | same layout | yes | no |
| 50 | 68BB8D2C | 20 | Fragile: Attentive | Fragile, black leather outfit | 0: `an_fglhw1_s_idle_lp` (30.0 s) | 1 `cross_arms` | none | same layout | yes | FRAGILE (best) |
| 51 | E729486F | 20 | Fragile: Bidding Goodbye | Fragile with umbrella | 0: `an_fglhw1_s_umbr_idle_lp` (30.0 s) | 1 `umbr_wave_hand_a` | none | same layout | yes | Fragile (alt) |
| 52 | 31679C53 | 20 | Die-Hardman: Encouraging | Die-Hardman, suit and mask | 0: `an_dhmhw1_s_idle_lp` (12.3 s) | 1 `fist_pump`, 2 `handclap_a` | none | same layout | yes | DIE-HARDMAN (best) |
| 53 | 4F26CE22 | 20 | Die-Hardman: Prideful | Die-Hardman, suit and mask (muscle pose) | 0: `an_dhmhw1_s_idle_lp` (12.3 s) | 1 `muscle_pose_a` | none | same layout | yes | Die-Hardman (alt) |
| 54 | 76571C36 | 20 | Deadman: Admiring | Deadman, suit and glasses | 0: `an_ddmhw1_s_idle_lp` (14.3 s) | 1 `handclap_a`, 2 `wave_hand_big_a` | none | same layout | yes | DEADMAN (best) |
| 55 | FFE30DB2 | 20 | Deadman: Concerned | Deadman (worried, one motion) | none | 0 `worry_lp` | none | same layout | yes | no |
| 56 | A9014BFD | 20 | Mama: Loving | Mama, overalls | 0: `an_mamhw1_s_idle_lp` (30.0 s) | 1 `wave_hand_01`, 2 `cross_arms` | none | same layout | yes | MAMA (best) |
| 57 | F1B18D5F | 20 | Mama: Connected | Mama (connected pose, one motion) | none | 0 `connecting_pose` | none | same layout | yes | no |
| 58 | 24FD1DC7 | 20 | Lockne: Encouraging | Lockne, hooded coat | 0: `an_lknhw1_s_idle_lp` (30.0 s) | 1 `joy_a`, 2 `nice_01` | none | same layout | yes | LOCKNE (best) |
| 59 | 09E39D1E | 20 | Lockne: Connected | Lockne (connected pose, one motion) | none | 0 `connecting_pose` | none | same layout | yes | no |
| 60 | 0E9A0AA3 | 20 | Heartman: Sending Likes! | Heartman, blue jumpsuit | 0: `an_htmhw1_s_idle_lp` (10.0 s) | 1 `nice`, 2 `wave_hand_a` | none | same layout | yes | HEARTMAN (best) |
| 61 | E29DE017 | 20 | Heartman: Resuscitation | Heartman lying on the floor (revive pose) | none | 0 `revive` | none | same layout | yes | no |
| 62 | 747959B2 | 20 | Higgs: | Higgs, hooded with gold mask | none | 0 `pose_welcome` | none | same layout | yes | HIGGS (pose only) |
| 63 | 469FEECF | 20 | Higgs: | Higgs (pose, one motion) | none | 0 `pose_six` | none | same layout | yes | no |
| 64 | 75FBCAB7 | 20 | Higgs: | Higgs (pose, one motion) | none | 0 `hand_signal_go_float` | none | same layout | yes | no |
| 65 | 6B579452 | 20 | Delivery Bot | delivery bot | 0: `an_atdrhw1_s_idle_lp` (6.0 s) | 1 `greet`, 2 `thanks` | none | other (32 joints) | yes | no |
| 66 | 5C0D9795 | 20 | Nendoroid Jumbo Ludens | giant Ludens figure (fills the screen) | 0: `an_ludkhw1_s_idle_lp` (0.0 s) | 1 `wave_hand_a` | none | other (8 joints) | yes | no |
| 67 | 7529E739 | 20 | Wolf | wolf, sitting | 0: `an_wlf_idle_sit_lp` (12.4 s) | 1 `an_wlf_idle_sit_gesture` | none | other (51 joints) | yes | no |
| 68 | 84DB48FB | 20 | Raccoon | raccoon (tiny) | none | 0 `idle_standing`, 1 `gesture_idle_itch` | 0.08 m | other (40 joints) | yes | no |
| 69 | 32E07BA8 | 20 | Goose | goose (tiny) | none | 0 `idle_loop` | none | other (31 joints) | yes | no |
| 70 | BDA29FC0 | 20 | BT | BT (Gazer-like figure) | 0: `an_gzmhw1_s_idle_lp_a` (8.0 s) | - | none | other (81 joints) | yes | no |
| 71 | 6C4A8F5F | 20 | Aloy (Horizon Zero Dawn) | Aloy | 0: `an_alhhw1_s_idle_lp` (15.0 s) | 1 `wave_hand_01`, 2 `wave_hand_02`, 3 `nice_a` | none | other (77 joints) | yes | Aloy (own skeleton) |
| 72 | 4B485804 | 20 | Watcher (Horizon Zero Dawn): Happy | Watcher robot | none | 0 `idle_stand`, 1 `idle_growling`, 2 `idle_play_chasing_tail`, 3 `idle_relax`, 4 `combat_taunt`, 5 `gesture_scratching` | none | other (52 joints) | yes | no |
| 73 | 8A627D10 | 20 | Watcher (Horizon Zero Dawn): Vigilant | Watcher robot | none | 0 `idle_extend_neck_loop` | none | other (52 joints) | yes | no |
| 74 | E3439234 | 20 | Tallneck (Horizon Zero Dawn) | Tallneck (only its leg in shot) | none | 0 `walk_zero` | none | other (229 joints) | yes | no |
| 75 | 13F85CCD | 20 | Strider (Half-Life: Alyx) | Strider (only its leg in shot) | none | 0 `an_stri_walk_lp_nx` | none | other (101 joints) | yes | no |
| 76 | 53895D9F | 20 | Trauma Team AV (Cyberpunk 2077) | Trauma Team AV (hovering, above the shot) | none | 0 `an_zeta_hovering_lp_nx` | none | other (2 joints) | yes | no |
| 77 | 841DE7AB | 20 | Creator (Cyberpunk 2077) | Hideo Kojima figure (Cyberpunk cameo) | 0: `an_cdpkjm_s_idle_lp` (10.4 s) | - | none | other (181 joints) | yes | Creator (own skeleton) |
| 78 | A6783960 | 20 | Jackie Welles (Cyberpunk 2077) | Jackie Welles | 0: `an_cdpjck_s_idle_lp` (13.7 s) | - | none | other (208 joints) | yes | Jackie (own skeleton) |
| 79 | 4FBC426E | 20 | Ludens Duck: Yellow | giant rubber duck | no clips | - | none | other (2 joints) | yes (screenshot retaken in run 171015) | no |
| 80 | A53E272F | 20 | Ludens Duck: White | giant rubber duck | no clips | - | none | other (2 joints) | yes | no |
| 81 | 3267B10D | 20 | Ludens Duck: Dark Blue | giant rubber duck | no clips | - | none | other (2 joints) | yes | no |
| 82 | EF3011A0 | 20 | Ludens Duck: Brown | giant rubber duck | no clips | - | none | other (2 joints) | yes | no |
| 83 | A931C26A | 20 | Ludens Duck: Reddish Brown | giant rubber duck | no clips | - | none | other (2 joints) | yes | no |
| 84 | 938B376F | 20 | Ludens Duck: Green | giant rubber duck | no clips | - | none | other (2 joints) | yes | no |
| 85 | B6729883 | 20 | Ludens Duck: Red | giant rubber duck | no clips | - | none | other (2 joints) | yes | no |
| 86 | DF96E072 | 20 | Ludens Duck: Blue | giant rubber duck | no clips | - | none | other (2 joints) | yes | no |
| 87 | 9A19F319 | 20 | Reverse Trike | reverse trike | no clips | - | none | other (40 joints) | yes | no |
| 88 | 307DC4EE | 20 | Truck | truck | no clips | - | none | other (47 joints) | yes | no |
| 89 | 142DBC0C | 20 | Roadster | roadster | no clips | - | none | other (23 joints) | yes | no |
