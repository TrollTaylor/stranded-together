# Game data index: real character bodies and entities

Status: **static, offline** (2026-11-16). This page was built by reading the installed game's packfiles
(`data/*.bin`) with `tools/re/ds_archive.py`. The game was not run, and nothing was written to the game folder.
It answers one question: which non-hologram character entities, bodies, skeletons, animation networks and
locomotion clips exist on disk, under which core path, and with which object GUIDs.

Tags: **[verified]** means read from the files by the tool (or cross-checked against a logged run, which is
cited). **[inferred]** means a reading of names or structure that has not been tested in game.

## Short answer

1. **Only some characters have real locomotion.** A Morpheme network drives real-time movement (walk, run,
   turn, cover...). Networks exist only for these characters **[verified]**:
   - Sam: `sam/sam`, 121 MB.
   - The MULE family: `mule/mule`, 171 MB. It is shared by the MULE porters, Demens (the MULE terrorists)
     and Cliff's soldiers (WW1, WW2, Vietnam, Afghanistan).
   - Cliff Unger: `father/father`.
   - Higgs: `higgs/higgs`.
   - Amelie: `amelie/amelie`.
   - Hunters, BTs, catchers and animals.

   **Fragile, Die-Hardman, Deadman, Heartman, Mama and Lockne have no network and no gameplay entity.** They
   exist only as cutscene actors and as holograms.
   - A cutscene actor is a `DSControlledEntityResource` with an `ExternalAnimationComponentResource`, under
     `ds/entities/cinematics/demo_*`.
   - The few mission entities for these characters (`deadman_m00500`, `mama_onsen`) are cutscene-style too
     (ExternalAnimation only).
2. **Every human skeleton is the same 77-joint skeleton** **[verified]**.
   - 29 skeleton cores (`.../model/skeletons/skeleton_jnt_c_b_000_root`) have identical serialized bytes
     after the GUID (same MD5), 9,896 bytes each. They are the `body_rig` skeletons `default_male`,
     `default_male_v01`, `default_female` and `default_child`, and the body skeletons of Fragile, Lockne,
     Deadman, Amelie, Die-Hardman, Heartman, Mama, Higgs, Lisa, Bridget and child Sam, plus some
     green-figure and variant parts.
   - Decoded: `Joints` = 77, `SkeletonLayoutHash` = `0xBE39C83F`. This matches the value measured in game
     (docs/character-catalog.md).
   - `body_rig/core/default_male` (GUID `60f56763-1a2b-554c-98bb-1d7faba685ce`) is the skeleton of all of
     these: Sam's body variants, Sam's player model, the MULEs, Demens, the soldiers, the Bridges porters,
     guards, members and medics, and the male preppers.
   - Die-Hardman, Higgs and Cliff use `default_male_v01`. Fragile, Deadman, Heartman, Mama, Lockne and
     Amelie reference their own copy.
   - The skeletons are the same data under different GUIDs. So any clip or network made for one human
     should fit every other **[inferred; matches the in-game finding]**.
3. **The real Sam has three parts** **[verified]**:
   - **Entity:** `DSPlayerEntityResource` in `ds/entities/player/ds_player_character`.
   - **Body:** a skeleton-only `SkinnedModelResource` in `ds/entities/player/ds_player_model`, dressed by one
     of 8 `DSPlayerBodyVariant`s (`ds/entities/player/variant/*`).
   - **Locomotion:** the `MorphemeAnimationManagerResource` in `ds_player_model` that points at
     `ds/animations/morpheme/characters/sam/sam`.

   The cargo/customise-menu Sam (`DSPlayerAKukanEntityResource`) reuses the same skeleton-only
   `SkinnedModelResource`, but it pairs it with the 1 MB menu network `sam/references/akukan`, not the
   locomotion network.
4. **The "Veteran Porter" hologram is a MULE-suit porter** **[inferred from paths]**.
   - Its hologram body is `mul_mule/part_mul_body_suitn_holo`.
   - The solid version of that body is `mul_mule/part_mul_body_suitn`, the body of
     `ds/entities/npc/mule/pe_friendly_mule`.
   - That is a real, solid porter in the MULE porter suit, with the full MULE Morpheme network, on Sam's own
     `default_male` skeleton. It is the closest real equivalent of today's partner hologram.
5. **Some clips on Sam's skeleton do have root motion** **[verified]**.
   - Five WW2-soldier jog loops (`ds/animations/game/wwt/wwtas1/an_wwtas1_s_*jog*_3lp`) run on
     `default_male` at 2.3 to 3.1 m/s.
   - Higgs and Amelie each have a 44 s walk (0.55 to 0.59 m/s) on their own skeletons.
   - The in-game clip survey found none of these because they were not loaded at the survey spot.
6. **Holograms are separate art parts.**
   - Every hologram body is its own `*_holo*` art part, for example `part_fgl_body_holo` against
     `part_fgl_body_def`.
   - A solid `ArtPartsDataResource` exists for every character on the wanted list **[verified]**.
   - So the "hologram look" is the art part the entity's `SkinnedModelResource.ArtPartsDataResource`
     points at **[inferred]**.

## How to read the tables

- **Path**: the core path without `.core`. The archive hash is `murmur3(path + ".core" + NUL)`.
- **Archive**: the first 8 hex digits of `data/<hash>.bin` (legend at the end). **Size**: the uncompressed
  core size in bytes.
- **GUIDs**: `ObjectUUID` formatted as a Windows GUID (`uuid.UUID(bytes_le=raw)`). In memory it is the 16
  raw bytes at `RTTIRefObject+0x08`. The tool also prints the raw byte order (`guid_raw` in `scan`).
- **Morpheme**: the network path under `ds/animations/morpheme/characters/`. **ExternalAnimation**: driven
  by sequences or clips, with no locomotion network.
- **Skeleton**: the path under `ds/models/characters/`, minus `/model/skeletons/skeleton_jnt_c_b_000_root`.

## Per character

| Wanted | Real entity with locomotion | Real cutscene body (ExternalAnimation) | Body art part (solid) | Skeleton |
|---|---|---|---|---|
| Sam (player) | `ds/entities/player/ds_player_character` (`DSPlayerEntityResource`), network `sam/sam` | `demo_sam_clean` (porter suit) and others below | `sam_sam/part_sam_body_suiti` (porter suit), via `DSPlayerBodyVariant` | `body_rig/default_male` |
| Fragile | **none** (no network, no NPC entity) | `demo_fgl_new` | `fgl_fragile/part_fgl_body_def` | own copy (`fgl_body_def`) |
| Die-Hardman | **none** | `demo_dhm_body_new` | `dhm_diehardman/part_dhm_body_def` | `body_rig/default_male_v01` |
| Deadman | **none** (`npc/deadman/deadman_m00500` is a mission entity driven by ExternalAnimation) | `demo_ddm_new` | `ddm_deadman/part_ddm_body_def` | own copy |
| Cliff Unger | `ds/entities/npc/cliff/pe_cliff`, network `father/father` | `demo_fth_father_new02` | `fth_father/part_fth_body_def` | `body_rig/default_male_v01` |
| Higgs | `ds/entities/npc/higgs/higgs_character` (boss), network `higgs/higgs` | `demo_hgs_body_def_new` | `hgs_higgs/part_hgs_body_def`, `part_hgs_body_beach` | `body_rig/default_male_v01` |
| Heartman | **none** | `demo_htm_body_def` | `htm_heartman/part_htm_body_def` | own copy |
| Mama | **none** (`npc/onsen_npc/mama_onsen` is ExternalAnimation only) | `demo_mam_body_def` | `mam_mama/part_mam_body_def` | own copy |
| Lockne | **none** | `demo_lkn_body_def` | `lkn_lockne/part_lkn_body_def` | own copy |
| Amelie | `ds/entities/npc/amelie/amelie_entity`, network `amelie/amelie` | `demo_aml_body_new` | `aml_amelie/part_aml_body_def` | own copy |
| MULE porters | `ds/entities/npc/mule/pe_friendly_mule`, `pe_mule`; network `mule/mule` | - | `mul_mule/part_mul_body_suitn` (porter suit), `part_mul_body_suith` | `body_rig/default_male` |
| Bridges porters | **none** as gameplay NPCs. Real porters in the field are MULEs. | `demo_bdv_body_def`, `demo_bdv_body_suits` (+`_v01`, `_v02`) | `bdv_bridgesdeliverer/part_bdv_body_def`, `part_bdv_body_suits` | `body_rig/default_male` |
| Bridges guards / members | none | `demo_bgs_body_def_v01`, `demo_bgm_body_*`, `demo_bme_body_def` | `bgs_bridgessecurity/part_bgs_body_def_v01`, ... | `body_rig/default_male` |

Hologram twins (not wanted) are listed for completeness. These are the cinematic holograms
(`ds/entities/cinematics/*holo*`), the collectors-DB welcome holograms (`ds/entities/npc/welcome_holo/**`,
264 files) and the construction holograms (`ds/entities/npc/customize_holo/**`, 96 files). Their bodies are
the `*_holo*` art parts:
- `fgl_fragile/part_fgl_body_holo`, `dhm_diehardman/part_dhm_body_holo`, `ddm_deadman/part_ddm_body_holo`
- `htm_heartman/part_htm_body_holo`, `mam_mama/part_mam_body_holo`, `lkn_lockne/part_lkn_body_holo_cnst`
- `aml_amelie/part_aml_body_holo`, `hgs_higgs/part_hgs_body_holo`
- `mul_mule/part_mul_body_suitn_holo`, `bdv_bridgesdeliverer/part_bdv_body_def_holo*`,
  `bgs_bridgessecurity/part_bgs_body_def_holo*`
- the `phm_`/`phf_` preppers-hologram sets

**[verified]** paths. **[inferred]** that these are the bodies the welcome holograms of
docs/character-catalog.md use.

## Gameplay entities (real bodies, real-time animation)

| Who | Path | Archive | Size | Entity resource objects: type and GUID | Body art parts | Skeleton | Animation driver |
|---|---|---|---|---|---|---|---|
| Sam (the player) | `ds/entities/player/ds_player_character` | 7017f9bb | 37661 | `DSPlayerEntityResource` `9b58ada6-67ab-5734-b14a-43b6a94bd838` |  | body_rig/core/default_male | - |
| Sam: shared body + networks | `ds/entities/player/ds_player_model` | 7017f9bb | 18737 | `MorphemeAnimationManagerResource` `e3b16c35-5e03-574f-8c73-582900a2aafe`<br>`SkinnedModelResource` `c6730637-1d37-494d-84bb-38e5ed0cd5b8`<br>`MorphemeAnimationManagerResource` `b8127c66-f96e-e943-afb1-c48294bf1215` | sam_sam/part_sam_body_skeltononly | body_rig/core/default_male | Morpheme: sam/references/akukan, sam/sam |
| Sam: player template | `ds/entities/player/ds_player_entity_template` | 7017f9bb | 131813 | `DSHoveringCartEntityResource` `1f791a0a-edae-9231-acb5-11235054ceb1` | sam_sam/part_sam_body_skeltononly<br>sam_sam/part_sam_foot_skeltononly | body_rig/core/default_male<br>sam_sam/core/sam_foot_def | Morpheme: sam/sam_default_male, sam/sam_mot_event_mappings; ExternalAnimation |
| Sam: motion entity (Higgs fight) | `ds/entities/player/motionentity/ds_player_motion_entity` | 477e458b | 9159 | `DSPlayerMotionEntityResource` `a094f348-c62c-1d43-9643-64f952aa6190` |  | body_rig/core/default_male | Morpheme: sam/references/higgs |
| Sam: cargo / customise menu | `ds/entities/player/akukan/ds_player_akukan_entity` | 7017f9bb | 3275 | `EntityResource` `98af0889-5624-ac45-a0a5-8b88f048d088`<br>`DSPlayerAKukanEntityResource` `664d5020-a194-a144-a17e-9f2b35695d64` | sam_sam/part_sam_part_harmonica | sam_sam/core/sam_part_harmonica | - |
| Sam body variant: porter suit | `ds/entities/player/variant/ds_player_variant_sam_body_suit` | 7017f9bb | 4860 | `DSPlayerBodyVariant` `80279532-dbc2-d745-a04b-14d90ec09fe3`<br>`SkinnedModelResource` `518dfcc1-95d7-8e4b-9177-783de85742d2`<br>`SkinnedModelResource` `1be8b2f1-aa2e-7345-87a8-5e8f006530aa` | sam_sam/part_sam_body_suiti<br>sam_sam/part_sam_foot_def | body_rig/core/default_male<br>sam_sam/core/sam_foot_def | - |
| Sam body variant: MULE suit | `ds/entities/player/variant/ds_player_variant_sam_body_suit_mule` | 7017f9bb | 1004 | `DSPlayerBodyVariant` `d888897b-ccba-2a40-b0bf-d31b2b7bb2df`<br>`SkinnedModelResource` `eff4c10f-8e0e-ef4b-94f7-6d41a11e12bc`<br>`SkinnedModelResource` `7e453364-110d-cc42-80e3-fb099c27688b` | sam_sam/part_sam_body_suitmule<br>sam_sam/part_sam_foot_suitmule | body_rig/core/default_male<br>sam_sam/core/sam_foot_def | - |
| Sam body variant: CDT suit | `ds/entities/player/variant/ds_player_variant_sam_body_suit_cdt` | 7017f9bb | 993 | `DSPlayerBodyVariant` `7fc4d909-d39f-4a4a-8711-39fe949df771`<br>`SkinnedModelResource` `1b00b3e5-05ea-de47-a494-a267a06dc6d5`<br>`SkinnedModelResource` `b08355b5-41b8-4b4b-b056-1c0af9e11a3f` | sam_sam/part_sam_body_suitcdt<br>sam_sam/part_sam_foot_def | body_rig/core/default_male<br>sam_sam/core/sam_foot_def | - |
| Sam body variant: beach | `ds/entities/player/variant/ds_player_variant_sam_body_suit_beach` | 7017f9bb | 1443 | `DSPlayerBodyVariant` `b18b6e09-6929-684a-ba28-8404017d2f37`<br>`SkinnedModelResource` `0a9bbeb7-0a1c-1e4d-8169-658f0dc16f61`<br>`SkinnedModelResource` `86261a68-a0b0-ff46-bf56-ec1c964ba85e` | sam_sam/part_sam_body_suiti_beach<br>sam_sam/part_sam_foot_def | body_rig/core/default_male<br>sam_sam/core/sam_foot_def | - |
| Sam body variant: casual wear | `ds/entities/player/variant/ds_player_variant_sam_body_wear` | 7017f9bb | 905 | `DSPlayerBodyVariant` `efcc58c6-1067-f24b-b906-1d08b4280337`<br>`SkinnedModelResource` `059f6107-9d37-e540-804f-766c38b6311c`<br>`SkinnedModelResource` `a1ca064f-e8f8-9149-9554-e31783951158` | sam_sam/part_sam_body_wear<br>sam_sam/part_sam_foot_def | body_rig/core/default_male<br>sam_sam/core/sam_foot_def | - |
| Sam body variant: private room | `ds/entities/player/variant/ds_player_variant_sam_body_private` | 7017f9bb | 908 | `DSPlayerBodyVariant` `b27cd19a-8c75-b247-8b54-632c7636dad1`<br>`SkinnedModelResource` `7c85ad85-59a8-524d-a4cc-4dc3334a2136`<br>`SkinnedModelResource` `88495a10-d4ed-e046-9287-1b92d39bab6c` | sam_sam/part_sam_body_private<br>sam_sam/part_sam_foot_def | body_rig/core/default_male<br>sam_sam/core/sam_foot_def | - |
| Sam body variant: naked | `ds/entities/player/variant/ds_player_variant_sam_body_naked` | 7017f9bb | 906 | `DSPlayerBodyVariant` `9edb8b19-c171-ad45-bb89-0216e8696127`<br>`SkinnedModelResource` `526c2441-3976-a34a-a02b-35e4402cf09b`<br>`SkinnedModelResource` `fb94ba81-dc9e-7f4f-9acc-f86010d2bc9e` | sam_sam/part_sam_body_naked<br>sam_sam/part_sam_foot_def | body_rig/core/default_male<br>sam_sam/core/sam_foot_def | - |
| Sam body variant: skeleton only | `ds/entities/player/variant/ds_player_variant_sam_body_skeleton_only` | 7017f9bb | 920 | `DSPlayerBodyVariant` `d249a4f0-a32b-d342-a785-7d0e979b11a9`<br>`SkinnedModelResource` `e25bd297-9539-7643-9561-c9c7b5ab0042`<br>`SkinnedModelResource` `2cfc5348-2647-8049-b68a-4511d4f80b63` | sam_sam/part_sam_body_skeltononly<br>sam_sam/part_sam_foot_skeltononly | body_rig/core/default_male<br>sam_sam/core/sam_foot_def | - |
| MULE porter (hostile) | `ds/entities/npc/mule/pe_mule` | 7017f9bb | 2140956 | `DSControlledEntityResource` `030c5433-57ca-e030-bc36-d31155c8a84e`<br>`DSControlledEntityResource` `9cc71254-b213-d535-8cd3-4535d41a353f` | mul_mule/part_mul_body_suith<br>mul_mule/part_mul_body_suitn | body_rig/core/default_male | Morpheme: mule/mule |
| MULE porter (friendly) | `ds/entities/npc/mule/pe_friendly_mule` | c0306eef | 1068630 | `DSControlledEntityResource` `6c6322cb-6d46-1a3b-b703-8e52a7668d48` | mul_mule/part_mul_body_suitn | body_rig/core/default_male | Morpheme: mule/mule |
| Demens (MULE terrorist) | `ds/entities/npc/mule/pe_demens` | 596c31fc | 1072330 | `DSControlledEntityResource` `f0761092-0ce2-9e32-a564-22effd52c28f` | mut_muleterrorist/part_mut_body_def | body_rig/core/default_male | Morpheme: mule/mule |
| Cliff's soldiers, WW1 | `ds/entities/npc/mule/pe_warriors_ww1` | 968bf82f | 2587527 | `DSControlledEntityResource` `8d6ea23c-f074-6e37-8d4d-352bbc468ead`<br>`DSControlledEntityResource` `dfa98e24-f281-9530-8acf-a5b794a78730`<br>`EntityResource` `c62ede5e-0264-1f3a-97c7-72eaf50120dd` | wwo_warriorwwone/part_wwo_body_def<br>wwo_warriorwwone/part_wwo_body_particle | body_rig/core/default_male | Morpheme: mule/mule |
| Cliff's soldiers, WW2 | `ds/entities/npc/mule/pe_warriors_ww2` | fcd4b591 | 2498019 | `DSControlledEntityResource` `91b7053c-c780-053e-8223-3a42d1f0ea13`<br>`DSControlledEntityResource` `7f28d722-b376-5a33-b7cb-936171f86ed3`<br>`EntityResource` `2567fc69-199d-a736-a846-02a18a69f38e` | wwt_warriorwwtwo/part_wwt_body_def<br>wwt_warriorwwtwo/part_wwt_body_particle | body_rig/core/default_male | Morpheme: mule/mule |
| Cliff's soldiers, Vietnam | `ds/entities/npc/mule/pe_warriors_vw` | a53f66da | 2557631 | `DSControlledEntityResource` `e6406401-6a5c-ec32-91a5-1604c881a741`<br>`EntityResource` `0a3eb7d4-19ad-d23c-a491-2602c9ea03b5`<br>`DSControlledEntityResource` `c62d6813-03cf-1439-b23b-912dada660e7` | wvt_warriorvietnam/part_wvt_body_def<br>wvt_warriorvietnam/part_wvt_body_particle | body_rig/core/default_male | Morpheme: mule/mule |
| Cliff's soldiers, Afghanistan | `ds/entities/npc/mule/pe_warriors_afg` | fcd4b591 | 1310947 | `DSControlledEntityResource` `0428ad8c-44ea-be33-9b13-7c086231e6f1`<br>`EntityResource` `397cfb51-9bee-bb46-871d-450f02caf020`<br>`EntityResource` `fbb42e54-22cc-213e-b648-c6fe13d6827b` | wwa_warriorafghan/part_wwa_body_def<br>wwa_warriorafghan/part_wwa_part_helmet | body_rig/core/default_male | Morpheme: mule/mule |
| Cliff Unger | `ds/entities/npc/cliff/pe_cliff` | 968bf82f | 330117 | `DSControlledEntityResource` `11fc11c5-fc0d-594f-a912-54659b780096`<br>`EntityResource` `ccda1d3b-dc1d-dc4a-9567-627afe05edb3`<br>`EntityResource` `40ebe4fa-6659-5443-bc31-a703dea74145`<br>(+2 more) | cig_cigarette/part_cig_main_def<br>fth_father/part_fth_body_def<br>fth_father/part_fth_part_helmet | body_rig/core/default_male_v01<br>cig_cigarette/core/cig_main_def | Morpheme: father/father; ExternalAnimation |
| Higgs (boss, beach) | `ds/entities/npc/higgs/higgs_character` | 477e458b | 23093 | `DSControlledEntityResource` `8d7f53e5-8a48-e84e-b7be-3ceaa5929589`<br>`EntityResource` `d8f3169b-39ac-ce4d-b5c7-1339892eff4d` | hgs_higgs/part_hgs_body_beach | body_rig/core/default_male_v01 | Morpheme: higgs/higgs; ExternalAnimation |
| Higgs (mission m00610) | `ds/entities/npc/higgs/higgs_m00610` | dc847b24 | 830 | `DSControlledEntityResource` `2c432152-97b3-444c-8e8d-35ca78084fa0` | hgs_higgs/part_hgs_body_def | body_rig/core/default_male_v01 | ExternalAnimation |
| Amelie | `ds/entities/npc/amelie/amelie_entity` | 477e458b | 16649 | `DSControlledEntityResource` `6f511071-32dc-1744-85bf-7dd70499a437` | aml_amelie/part_aml_body_def | aml_amelie/core/aml_body_def | Morpheme: amelie/amelie; ExternalAnimation |
| Amelie (red dress, idle) | `ds/entities/npc/amelie/amelie_idle_red` | c0306eef | 7038 | `DSControlledEntityResource` `628493bc-49bf-a840-b5b4-df90e8941d6d` | aml_amelie/part_aml_body_def_no_bulletblocker | aml_amelie/core/aml_body_def | - |
| Amelie (mission m00610) | `ds/entities/npc/amelie/amelie_m00610_entity` | dc847b24 | 885 | `DSControlledEntityResource` `4dbd9133-c66b-a440-85c0-ef31ee99b172` | aml_amelie/part_aml_body_def | aml_amelie/core/aml_body_def | ExternalAnimation |
| Deadman (mission m00500) | `ds/entities/npc/deadman/deadman_m00500` | fcd4b591 | 1314 | `EntityResource` `eee12f53-9b10-6047-a374-158d0c3fd38a`<br>`EntityResource` `44be02ed-0ca8-5b4f-9335-3b4447db725b` | ddm_deadman/part_ddm_body_def | ddm_deadman/core/ddm_body_def | ExternalAnimation |
| Mama (hot spring) | `ds/entities/npc/onsen_npc/mama_onsen` | c0306eef | 2591 | `DSControlledEntityResource` `9a4796cb-5812-9348-ab5c-99db8b7a2abd` | mam_mama/part_mam_body_naked | mam_mama/core/mam_body_def | ExternalAnimation |
| Chiral Artist (hot spring) | `ds/entities/npc/onsen_npc/chairalartist_onsen` | c0306eef | 2658 | `DSControlledEntityResource` `12b0b0a9-f35c-7e4c-87cd-0e606372529b` | ppf_preppersfemale/part_ppf_body_naked_v00 | body_rig/core/default_female | ExternalAnimation |
| BT hunters | `ds/entities/npc/hunter/hunter_member_collection` | 596c31fc | 7468 | `EntityResource` `96515b06-385e-2e38-a8f7-519d586f586f`<br>`EntityResource` `888360cc-db83-4833-bcb3-3c2508c6a623`<br>`EntityResource` `0a463a06-6695-b93b-b852-2b6d3f6c0c69`<br>(+3 more) | hnt_hunter/part_hnt_main_def<br>hnt_hunter/part_hnt_main_gold<br>hnt_hunter/part_hnt_main_kjm | hnt_hunter/core/hnt_main_def<br>hnt_hunter/core/hnt_main_kjm | - |

Notes:
- **`ds_player_character`:**
  - The `DSPlayerEntityResource` `EntityComponentResources` point, in this order, at:
    - `ds_player_model` `c6730637-...` (the skeleton-only `SkinnedModelResource`)
    - `ds_player_model` `b8127c66-...` (the Morpheme manager for `sam/sam`)
    - the third-person camera, `ds_player_inventory` and `ds_player_entity_template`
  - Decoded with `core --decode` **[verified]**. The log shows it in use: run `20261116-170308-holo-catalog-all`
    logged Sam as `loc="ds/entities/player/ds_player_character"`.
- **`ds_player_akukan_entity`:**
  - The `DSPlayerAKukanEntityResource` has 10 `EntityComponentResources`. Decoded order: Inventory,
    Baggage2Carrier, FixedPositionMover, facial (`dsplayerfacialcomponent`), SkinnedModel
    (`ds_player_model` `c6730637`), Morpheme (`ds_player_model` `e3b16c35` = `sam/references/akukan`),
    AKukanConfig (`ds_player_akukan_efffloor_entity`), CustomSound, Voice, NodeGraph
    (`sd_state_machine_setup_sam_akukan`).
  - The same 10 types, in the same order, were logged at runtime by the census of run
    `20261116-170308-holo-catalog-all` **[verified]**. That check also confirms the field decoder.
- **MULE family:**
  - The MULE porters, Demens and the soldiers all point at `mule/mule` and `body_rig/default_male`
    **[verified]**.
  - Level data does not reference `pe_mule` directly. It references `ds/entities/npc/mule/pe_mule_cp`
    (`DSMuleCPGameActorUnitResource`, the MULE camp unit) in `levels/worlds/_l100_area01/leveldata/gl_pg`,
    `_l200_area02/leveldata/gl_pg`, `_l500_warrior01/leveldata/gl_ld` and `_l510_warrior02/leveldata/gl_ld`
    (byte search **[verified]**).
  - How a camp turns into `pe_mule` entities is **unknown**.
  - Cross-check from a parallel hidden run (`build-testenv/runs/20261116-174149-anim-census-mule`, its
    `[census]` lines) **[verified]**:
    - A loaded `pe_friendly_mule` `DSControlledEntityResource` is named `"Mule_Mule"` with `ai=1 (Humanoid)`.
    - Its MorphemeAnimationManager skeleton is the *same object* as the one of `ds_player_model`'s manager
      (77 joints, `BE39C83F`).
    - That matches both files referencing `default_male` `60f56763-...`.
- **Cliff:** `pe_cliff` is referenced from `levels/worlds/_l520_warrior03/leveldata/gl_ld_mission/00630/enemy/enemy`
  **[verified]**.
- **Hot-spring entities:**
  - `mama_onsen` and `chairalartist_onsen` are referenced by the `scn_onsen_rt2xx_*` scenes in
    `_l200_area02` **[verified]**.
  - They are solid (non-hologram) bodies, but they have no locomotion.

## Cutscene actor bodies (real bodies, ExternalAnimation only)

All of these are `DSControlledEntityResource` + `SkinnedModelResource` + `ExternalAnimationComponentResource`.
They have no Morpheme network. Each `demo_X` has a `pe_demo_X` that holds one `SpawnSetup`.
`SpawnSetup.EntityResource` points at the `demo_X` entity: for `pe_demo_fgl`, the ref GUID
`336f4328-...` equals `demo_fgl_new`'s decoded ObjectUUID **[verified]**. *First clip* is the first
`ds/animations/game/` clip the file references, if any.

| Character | Variant | Path under `ds/entities/cinematics/` | Archive | Size | `DSControlledEntityResource` GUID | Body art part | Skeleton | First clip |
|---|---|---|---|---|---|---|---|---|
| Sam | porter suit (game look) | `demo_sam_clean` | 3460515f | 1055 | `514b5705-aaf0-a447-a82e-0bb83b855201` | sam_sam/part_sam_body_suiti | body_rig:default_male | - |
| Sam | MULE suit | `demo_sam_body_suitmule_demo_new` | 40fb866e | 1020 | `ee0118b9-947a-fa46-b38d-bbacb6675c85` | sam_sam/part_sam_body_suitmule_demo | body_rig:default_male | - |
| Sam | casual wear | `demo_sam_body_wear_new` | 477e458b | 952 | `e767883e-d25a-fe47-b9ea-36042ec28b37` | sam_sam/part_sam_body_wear | body_rig:default_male | - |
| Sam | private room | `demo_sam_body_private_02` | 3460515f | 921 | `ca25139d-a1c2-7148-8f05-fafc0e1e859a` | sam_sam/part_sam_body_private | body_rig:default_male | - |
| Sam | naked | `demo_sam_body_naked_new` | 40fb866e | 1058 | `b4193ac9-5111-124f-a5b4-cc0741f49bbb` | sam_sam/part_sam_body_naked | body_rig:default_male | - |
| Sam | child Sam (own skeleton) | `demo_sam_body_child` | 477e458b | 1015 | `64b07ac0-bcc6-8142-b48b-255c66f4ef09` | sam_sam/part_sam_body_child | sam_sam:sam_body_child | - |
| Fragile | default | `demo_fgl_new` | 40fb866e | 997 | `336f4328-1381-4947-8e6d-7def29e654bd` | fgl_fragile/part_fgl_body_def | fgl_fragile:fgl_body_def | an_fgldef_s_idle_lp |
| Fragile | variation | `demo_apv_fgl_body_def_v01` | acc0d12f | 1069 | `7946706f-9551-8545-a437-a14b05d42a01` | fgl_fragile/part_fgl_body_def | fgl_fragile:fgl_body_def | - |
| Fragile | porter suit | `demo_fgl_body_suiti` | acc0d12f | 981 | `dbbb2d95-8583-f046-a5aa-fc7b0dafa8f6` | fgl_fragile/part_fgl_body_suiti | fgl_fragile:fgl_body_def | - |
| Fragile | aged | `demo_fgl_body_aging` | 3460515f | 981 | `1336c7c6-9691-5c43-8c79-7953db082454` | fgl_fragile/part_fgl_body_aging | fgl_fragile:fgl_body_def | - |
| Die-Hardman | default (masked) | `demo_dhm_body_new` | 477e458b | 1013 | `3e20c5a3-1829-1c46-a7af-12b8fa56a2d8` | dhm_diehardman/part_dhm_body_def | body_rig:default_male_v01 | - |
| Die-Hardman | same body part | `demo_dhm_body_stl` | acc0d12f | 971 | `231ea8f2-95ce-2e4f-a4fb-819ca7b0cfb8` | dhm_diehardman/part_dhm_body_def | body_rig:default_male_v01 | - |
| Die-Hardman | flashback | `demo_dhm_body_fbk` | acc0d12f | 1023 | `d1ac9a65-02d0-7f4d-921a-57113edeb392` | dhm_diehardman/part_dhm_body_fbk | body_rig:default_male | - |
| Deadman | default | `demo_ddm_new` | 477e458b | 1117 | `4f44c8dd-c7ab-974c-9b55-fbeee6f33039` | ddm_deadman/part_ddm_body_def | ddm_deadman:ddm_body_def | an_ddmnon_s_idle_lp |
| Deadman | variation | `demo_apv_ddm_body_def_v01` | fcd4b591 | 930 | `356b9c95-0c94-4945-b2dd-987944485f45` | ddm_deadman/part_ddm_body_def | ddm_deadman:ddm_body_def | - |
| Deadman | lab coat | `demo_ddm_body_lab` | 3460515f | 1023 | `eaf71f96-bd69-2e4d-bf11-39d24a976cb6` | ddm_deadman/part_ddm_body_lab | ddm_deadman:ddm_body_lab | - |
| Heartman | default | `demo_htm_body_def` | 3460515f | 1200 | `807b739b-397a-8741-bce1-6255fa332a0b` | htm_heartman/part_htm_body_def | htm_heartman:htm_body_def | an_htmdef_06_00300_pose |
| Mama | default | `demo_mam_body_def` | acc0d12f | 1011 | `b164763f-300b-dd47-ba13-ececde97d6e4` | mam_mama/part_mam_body_def | mam_mama:mam_body_def | - |
| Mama | variation | `demo_apv_mam_body_def_v01` | acc0d12f | 1052 | `4267a6f3-6336-1448-b734-3a949f08a2ea` | mam_mama/part_mam_body_def | mam_mama:mam_body_def | - |
| Mama | mission 00430 | `demo_mama_00430` | c0306eef | 903 | `f92dc560-a7ac-7743-9d41-196d7d77c5fa` | mam_mama/part_mam_body_def | mam_mama:mam_body_def | an_mandef_q_sit_sleep_idle_lp |
| Lockne | default | `demo_lkn_body_def` | 3460515f | 1019 | `59612099-ba7c-f94a-9b8a-18032fac78e2` | lkn_lockne/part_lkn_body_def | lkn_lockne:lkn_body_def | - |
| Lockne | variant | `demo_lkn_body_ben` | acc0d12f | 973 | `1af0b37c-87ec-f74d-ba2e-334a4af20d42` | lkn_lockne/part_lkn_body_ben | lkn_lockne:lkn_body_def | - |
| Amelie | default | `demo_aml_body_new` | 477e458b | 1001 | `921db5fb-6fb7-f844-9a54-73075865ab29` | aml_amelie/part_aml_body_def | aml_amelie:aml_body_def | - |
| Amelie | red dress | `demo_aml_body_red_new` | 477e458b | 1094 | `83080e2c-b88d-134c-b555-cac536d7cc40` | aml_amelie/part_aml_body_def | aml_amelie:aml_body_def | - |
| Higgs | default | `demo_hgs_body_def_new` | 40fb866e | 1012 | `17cbc331-ea36-1043-a9f5-9871f6dcddde` | hgs_higgs/part_hgs_body_def | body_rig:default_male_v01 | - |
| Higgs | gold mask | `demo_hgs_body_gold_demo` | dc847b24 | 1033 | `d46bf427-f688-ca40-90c3-71a9024e881c` | hgs_higgs/part_hgs_body_gold_demo | body_rig:default_male_v01 | - |
| Cliff Unger | default | `demo_fth_father_new02` | 477e458b | 977 | `dcf18e42-aa86-6f45-85bd-db75052a185f` | fth_father/part_fth_body_def | body_rig:default_male_v01 | - |
| Cliff Unger | flashback | `demo_apv_fth_body_fbk_v00` | acc0d12f | 1060 | `db5c4c49-dbfa-7840-b6bc-9b767501b44e` | fth_father/part_fth_body_fbk | body_rig:default_male_v01 | - |
| Bridges porter | Bridges deliverer | `demo_bdv_body_def` | acc0d12f | 989 | `ab317812-6fce-ec44-874c-26532bfe3ba0` | bdv_bridgesdeliverer/part_bdv_body_def | body_rig:default_male | - |
| Bridges porter | Bridges deliverer, suit | `demo_bdv_body_suits` | acc0d12f | 993 | `d210ec9c-8f91-bc43-816d-40173a5ad3cc` | bdv_bridgesdeliverer/part_bdv_body_suits | body_rig:default_male | - |
| Bridges porter | suit v01 | `demo_bdv_body_suits_v01` | acc0d12f | 861 | `ed08d664-877a-7643-9c94-dc72ce494bc3` | bdv_bridgesdeliverer/part_bdv_body_suits_v01 | body_rig:default_male | - |
| Bridges porter | suit v02 | `demo_bdv_body_suits_v02` | acc0d12f | 861 | `bbe1d0a2-4334-dc47-a8fa-e3de74acdc93` | bdv_bridgesdeliverer/part_bdv_body_suits_v02 | body_rig:default_male | - |
| Bridges security | guard | `demo_bgs_body_def_v01` | acc0d12f | 855 | `73817e25-9845-d443-b209-b3065497fb6e` | bgs_bridgessecurity/part_bgs_body_def_v01 | body_rig:default_male | - |
| Bridges member | captain | `demo_bgm_body_captain_new` | 40fb866e | 987 | `201d09e9-599a-b74d-a7dc-23ea714d77a3` | bgm_bridgesmember/part_bgm_body_captain | body_rig:default_male | - |
| Bridges member | driver | `demo_bgm_body_driver_new` | 40fb866e | 986 | `a88fde94-4899-d04f-957e-2c6a40fb21b9` | bgm_bridgesmember/part_bgm_body_driver | body_rig:default_male | - |
| Bridges medic | medic | `demo_bme_body_def` | acc0d12f | 981 | `6b9fe7c0-630c-1442-a34f-717850672b80` | bme_bridgesmedic/part_bme_body_def | body_rig:default_male | - |
| MULE terrorist | Demens | `demo_mut_body_def` | 3460515f | 983 | `5f06a154-19a4-144d-a2c3-4926b4a5e6af` | mut_muleterrorist/part_mut_body_def | body_rig:default_male | - |
| Prepper (male) | prepper | `demo_ppm_body_def_v00` | acc0d12f | 989 | `6248135a-c16e-5448-abc7-e62ef9edb67b` | ppm_preppersmale/part_ppm_body_def_v00 | body_rig:default_male | - |
| Prepper (female) | prepper | `demo_apv_ppf_body_def_v00` | acc0d12f | 1140 | `248b1766-633b-c844-adaa-e11c694c925b` | ppf_preppersfemale/part_ppf_body_def_v00 | body_rig:default_female | - |
| Bridget | Bridget | `demo_bgt_body_def` | 477e458b | 1023 | `9b551760-ff36-cf4e-a6b3-5e16deab325d` | bgt_bridget/part_bgt_body_def | bgt_bridget:bgt_body_def | - |
| Lisa | Lisa | `demo_lsa_body_def` | acc0d12f | 1011 | `9dd75d09-656b-1245-8481-400c36f9913e` | lsa_lisa/part_lsa_body_def | lsa_lisa:lsa_body_def | - |

## Body art parts (`ArtPartsDataResource`)

`SkinnedModelResource.ArtPartsDataResource` points at these. The ones in the multi-MB range carry
the gameplay level of detail and many sub-models; cutscene-only bodies are small.

| Path | Archive | Size | `ArtPartsDataResource` GUID | Objects in file |
|---|---|---|---|---|
| `ds/artparts/characters/sam_sam/part_sam_body_suiti` | 7017f9bb | 19993992 | `263b7e30-a0af-7e43-b9de-63f04ddba27d` | 2079 |
| `ds/artparts/characters/sam_sam/part_sam_body_skeltononly` | 7017f9bb | 5164 | `1e001767-e274-4143-bb64-1f436361c63b` | 5 |
| `ds/artparts/characters/sam_sam/part_sam_body_suitmule` | 7017f9bb | 7586137 | `3e99d3fc-4e2b-2d4a-801b-289225f1bdfa` | 822 |
| `ds/artparts/characters/sam_sam/part_sam_foot_def` | 7017f9bb | 1211448 | `230969e3-0b7a-ea4c-b5b7-6f3bc89efc9e` | 207 |
| `ds/artparts/characters/mul_mule/part_mul_body_suitn` | 7017f9bb | 15043319 | `db91dfbe-a9ef-d246-a135-77082d6c768d` | 1564 |
| `ds/artparts/characters/mul_mule/part_mul_body_suith` | 7017f9bb | 10865562 | `63dd2bc3-1d43-e44c-865b-595261a0e77f` | 1106 |
| `ds/artparts/characters/mut_muleterrorist/part_mut_body_def` | 596c31fc | 12021625 | `4a3a4330-f739-eb49-ad8a-2b3283aa593b` | 1228 |
| `ds/artparts/characters/bdv_bridgesdeliverer/part_bdv_body_def` | acc0d12f | 10264 | `d8da19a2-5c28-a447-ade8-52e839ded25e` | 16 |
| `ds/artparts/characters/bdv_bridgesdeliverer/part_bdv_body_suits` | acc0d12f | 9816 | `3473486b-6d90-e741-ad82-46a2bd1e7503` | 13 |
| `ds/artparts/characters/bgs_bridgessecurity/part_bgs_body_def_v01` | acc0d12f | 101959 | `7b9012db-a7d2-cb44-b9fc-5ddb4ed77535` | 26 |
| `ds/artparts/characters/ppm_preppersmale/part_ppm_body_def_v00` | acc0d12f | 21848 | `2b4e1464-9995-4f4f-a010-e3e5b2ae620e` | 13 |
| `ds/artparts/characters/ppf_preppersfemale/part_ppf_body_def_v00` | acc0d12f | 38251 | `b9f90b9a-1cbe-684e-b09e-92cba7b7e56d` | 29 |
| `ds/artparts/characters/fgl_fragile/part_fgl_body_def` | 7017f9bb | 39074 | `2fe0bd77-1a56-3d44-998d-15253b540dc8` | 51 |
| `ds/artparts/characters/dhm_diehardman/part_dhm_body_def` | 477e458b | 30318 | `f024a86b-261f-1e47-b920-b252b2fb4b79` | 24 |
| `ds/artparts/characters/ddm_deadman/part_ddm_body_def` | 477e458b | 49761 | `75cfe796-4da2-dc45-810a-cd86b9066694` | 64 |
| `ds/artparts/characters/htm_heartman/part_htm_body_def` | 3460515f | 27353 | `c1caa596-5b45-7e47-ac78-74354ac6d31c` | 26 |
| `ds/artparts/characters/mam_mama/part_mam_body_def` | acc0d12f | 40484 | `63cf8d6f-dd86-954f-8c63-84e0517bedb7` | 63 |
| `ds/artparts/characters/lkn_lockne/part_lkn_body_def` | 3460515f | 34951 | `dab7cb22-4680-1e43-b4c2-7c75c3d3f615` | 40 |
| `ds/artparts/characters/aml_amelie/part_aml_body_def` | 477e458b | 42816 | `0b88ae3e-b4fd-a34d-aae4-2157382f33fb` | 82 |
| `ds/artparts/characters/hgs_higgs/part_hgs_body_def` | 40fb866e | 6795306 | `e5488de5-7542-d14d-988b-0fdf31316af7` | 707 |
| `ds/artparts/characters/hgs_higgs/part_hgs_body_beach` | 477e458b | 7351488 | `f79f7c2f-2437-ed4e-b19b-d1372b678bd7` | 794 |
| `ds/artparts/characters/fth_father/part_fth_body_def` | 477e458b | 5489922 | `6429e1fb-32e7-5848-9e0d-c5785f80718d` | 579 |

## Skeletons

| Path | Archive | Size | `Skeleton` GUID | Joints |
|---|---|---|---|---|
| `ds/models/characters/body_rig/core/default_male/model/skeletons/skeleton_jnt_c_b_000_root` | 7017f9bb | 9896 | `60f56763-1a2b-554c-98bb-1d7faba685ce` | 77 |
| `ds/models/characters/body_rig/core/default_male_v01/model/skeletons/skeleton_jnt_c_b_000_root` | 7017f9bb | 9896 | `4c5ad4ec-76ca-0347-90fc-b9863e5e11f6` | 77 |
| `ds/models/characters/body_rig/core/default_female/model/skeletons/skeleton_jnt_c_b_000_root` | 7017f9bb | 9896 | `35a6cca5-cc94-b34f-9e61-e4acda0a2a67` | 77 |
| `ds/models/characters/fgl_fragile/core/fgl_body_def/model/skeletons/skeleton_jnt_c_b_000_root` | 7017f9bb | 9896 | `c2e020b1-66d0-925a-b3ff-607b1bba67ef` | 77 |
| `ds/models/characters/dhm_diehardman/core/dhm_body_def/model/skeletons/skeleton_jnt_c_b_000_root` | 7017f9bb | 9896 | `c4caca65-0320-d942-8093-4b6e8143d63b` | 77 |
| `ds/models/characters/ddm_deadman/core/ddm_body_def/model/skeletons/skeleton_jnt_c_b_000_root` | 7017f9bb | 9896 | `15586bd9-18f9-7558-bc78-39e498bda09d` | 77 |
| `ds/models/characters/htm_heartman/core/htm_body_def/model/skeletons/skeleton_jnt_c_b_000_root` | 7017f9bb | 9896 | `73fa2287-51fe-bb52-9530-e80468aa4179` | 77 |
| `ds/models/characters/mam_mama/core/mam_body_def/model/skeletons/skeleton_jnt_c_b_000_root` | 7017f9bb | 9896 | `c321bb39-5b49-df55-a9b5-5a56da096844` | 77 |
| `ds/models/characters/lkn_lockne/core/lkn_body_def/model/skeletons/skeleton_jnt_c_b_000_root` | 7017f9bb | 9896 | `a47f7c33-a952-7550-8058-4a1cd865579e` | 77 |
| `ds/models/characters/aml_amelie/core/aml_body_def/model/skeletons/skeleton_jnt_c_b_000_root` | 7017f9bb | 9896 | `d403eac1-8b09-814b-8141-e2538d5908cb` | 77 |
| `ds/models/characters/hgs_higgs/core/hgs_body_def/model/skeletons/skeleton_jnt_c_b_000_root` | 7017f9bb | 9896 | `2edf148f-87df-e74a-8e1e-525f4ce4aa47` | 77 |

Every row above decodes to `SkeletonLayoutHash` `0xBE39C83F`. That is checked for `default_male`,
`fgl_body_def` and `hgs_body_def`, and implied for the rest by the identical bytes **[verified]**.

## Morpheme networks (`MorphemeNetworkDefResource`)

| Path | Archive | Size | GUID |
|---|---|---|---|
| `ds/animations/morpheme/characters/amelie/amelie` | 477e458b | 890702 | `eb15a7d5-e0b2-3b47-ab3a-2d29a9c01faa` |
| `ds/animations/morpheme/characters/autorobot/autorobot` | 7017f9bb | 42755265 | `42f3d117-5165-cc4c-95aa-5a3f9aaca5c9` |
| `ds/animations/morpheme/characters/autorobot/autorobot_manp` | acc0d12f | 906691 | `af944b26-6917-7c4c-b20d-a963ff972f63` |
| `ds/animations/morpheme/characters/catcherjellyfish/catcherjellyfish` | 596c31fc | 214165 | `cfde73db-3260-4442-95db-f025f8e4964a` |
| `ds/animations/morpheme/characters/catchermoles/catchermoles` | acc0d12f | 17633847 | `a22a6d8a-aa98-ad4d-a78d-932bedca384d` |
| `ds/animations/morpheme/characters/catcheroctopus_10/catcheroctopus_10` | acc0d12f | 284624 | `541283ef-ce2d-b247-b08f-ca1ae9336367` |
| `ds/animations/morpheme/characters/catcheroctopus_20/catcheroctopus_20` | acc0d12f | 10228345 | `02a53c61-9a7a-3f49-a013-707d68671b0c` |
| `ds/animations/morpheme/characters/catcheroctopus_va/catcheroctopus_va` | acc0d12f | 34031998 | `7859771c-b899-334a-a596-fe7bbf2c31fa` |
| `ds/animations/morpheme/characters/catcherquad/catcherquad` | acc0d12f | 12144468 | `91434639-f2e4-2844-9884-5c689bb4bd7a` |
| `ds/animations/morpheme/characters/catchertitan/catchertitan` | dc847b24 | 45332920 | `417da02f-981d-6a4e-b59c-33d861e3c98a` |
| `ds/animations/morpheme/characters/father/father` | 968bf82f | 35212155 | `166e4c98-4b93-4d4d-83e5-a45c883d296b` |
| `ds/animations/morpheme/characters/gazermale/gazermale` | 596c31fc | 2395802 | `3a56fc0b-5548-d140-ac7e-ac7e6e421500` |
| `ds/animations/morpheme/characters/gazernavel_big/gazernavel_big` | 596c31fc | 4026443 | `5638440a-3d33-d946-aa44-50b52fafc6bd` |
| `ds/animations/morpheme/characters/higgs/higgs` | 477e458b | 15187719 | `015ea5ed-647b-fe43-a4fc-6b01f49d1f7c` |
| `ds/animations/morpheme/characters/higgs_catchertitan/higgs_catchertitan` | dc847b24 | 572885 | `9045b5e3-0ee7-1642-8284-a0127c1a7974` |
| `ds/animations/morpheme/characters/hunter/hunter` | 596c31fc | 687126 | `6e2eea44-60ae-5a4f-945e-57240f0c7f4d` |
| `ds/animations/morpheme/characters/hunter_support/hunter_support` | 40fb866e | 656927 | `e0bad4e7-2d32-f346-b6a6-bb63a7804294` |
| `ds/animations/morpheme/characters/mule/mule` | 7017f9bb | 171681634 | `736837dc-aeaa-e14e-a285-b956c81904a7` |
| `ds/animations/morpheme/characters/sam/references/akukan` | 7017f9bb | 1008984 | `794618e5-de88-1d4d-952c-7648355ee5dd` |
| `ds/animations/morpheme/characters/sam/references/higgs` | 477e458b | 6322350 | `e4b302de-9007-dc44-9b9c-e3edb1adae71` |
| `ds/animations/morpheme/characters/sam/sam` | 7017f9bb | 121180025 | `9fc54ba1-98f9-0b41-bf32-0e2810f08796` |

`sam/sam_default_male` and `mule/mule_default_male` are the ragdoll/physics skeleton files
(`PhysicsRagdollResource`, 51,614 bytes each), not networks.

## Locomotion-like clips (`SkeletonAnimationResource`)

All 2,005 clips under `ds/animations/game/` were decoded:
- 85 have more than 0.5 m of `LocomotionDeltaTranslation`.
- These are the walk, jog and run clips on human skeletons.
- The in-place "appear" walks from docs/character-catalog.md are included for comparison.
- `LoopMode` is `Hold` for all of them, the same as in game.

| Clip | GUID | Skeleton | Duration | Root motion | Speed | LoopMode |
|---|---|---|---|---|---|---|
| `ds/animations/game/wwt/wwtas1/an_wwtas1_s_alert_jog_3lp` | `1c8df89f-af81-3f33-a6cd-ce227f799e32` | body_rig/core/default_male | 12.01 s | 36.96 m | 3.08 m/s | Hold |
| `ds/animations/game/wwt/wwtas1/an_wwtas1_s_alert_defence_jog_3lp` | `92db35ba-d01d-0434-b86d-78e6f954cdd2` | body_rig/core/default_male | 13.26 s | 33.35 m | 2.51 m/s | Hold |
| `ds/animations/game/wwt/wwtas1/an_wwtas1_s_fre_lookaround_jog_ver2_3lp` | `5b20341d-de1f-9731-9bac-29d18b92a767` | body_rig/core/default_male | 13.01 s | 31.27 m | 2.40 m/s | Hold |
| `ds/animations/game/wwt/wwtas1/an_wwtas1_s_alert_lookaround_jog_3lp` | `c0016edf-27d5-b832-92e2-f19304f71cc7` | body_rig/core/default_male | 13.41 s | 30.79 m | 2.30 m/s | Hold |
| `ds/animations/game/wwt/wwtas1/an_wwtas1_s_fre_lookaround_jog_ver1_3lp` | `c486f97a-f9c1-ae30-9585-3dde0643c6d9` | body_rig/core/default_male | 14.26 s | 32.35 m | 2.27 m/s | Hold |
| `ds/animations/game/hgs/hgsnxt/an_hgsnxt_s_walk_with_aml_lp` | `aeebb935-d082-e731-892e-d9c6e2bf85c6` | hgs_higgs/core/hgs_body_def | 44.04 s | 25.91 m | 0.59 m/s | Hold |
| `ds/animations/game/aml/amlnxt/an_amlnxt_s_walk_with_hgs_lp` | `be34ca2a-c715-993b-8f75-2b2cf9a912d3` | aml_amelie/core/aml_body_def | 44.04 s | 24.32 m | 0.55 m/s | Hold |
| `ds/animations/game/sam/samppm/an_samppm_s_farewall_walk_privateroom` | `cf76fc8e-0f4b-d13a-bfff-2efb73e0b148` | body_rig/core/default_male | 2.84 s | 0.00 m | 0.00 m/s | Hold |
| `ds/animations/game/dhm/dhmppm/an_dhmppm_s_appear_walk_01` | `3b8ce607-0e92-3033-a1a6-19f1b8b17f14` | dhm_diehardman/core/dhm_body_def | 3.34 s | 0.00 m | 0.00 m/s | Hold |
| `ds/animations/game/ddm/ddmppm/an_ddmppm_s_appear_walk_02` | `afec89fd-954f-a236-a46a-928572547634` | ddm_deadman/core/ddm_body_def | 3.34 s | 0.00 m | 0.00 m/s | Hold |
| `ds/animations/game/htm/htmppm/an_htmppm_s_appear_walk_01` | `c7eca3cc-df85-6a38-9393-35052818eea1` | htm_heartman/core/htm_body_def | 2.67 s | 0.00 m | 0.00 m/s | Hold |
| `ds/animations/game/mam/mamppf/an_mamppf_s_appear_walk_talk_01` | `eb73d4ce-6b39-8a34-aa73-967050255f14` | mam_mama/core/mam_body_def | 3.17 s | 0.00 m | 0.00 m/s | Hold |
| `ds/animations/game/fth/fth/an_fth_s_umbcord1_walk_lp` | `79587695-7fe6-5836-a2fb-74c8cee4012c` | fth_father/core/fth_part_umbshort | 2.05 s | 0.00 m | 0.00 m/s | Hold |
| `ds/animations/game/fth/fth/an_fth_s_umbcord1_jog_lp` | `8c47b248-5644-0632-9a8f-502dd4e8bb31` | fth_father/core/fth_part_umbshort | 1.33 s | 0.00 m | 0.00 m/s | Hold |

- `_3lp` reads as "3 loops": a 12 to 14 s clip that covers about 31 to 37 m. Whether it loops seamlessly
  (start pose = end pose) has **not** been checked.
- The `fth` umbilical-cord walk/jog clips are on the cord skeleton (`fth_part_umbshort`), not on Cliff's
  body.

## How this was produced

```
# once: entry index of all 144 archives (560,276 unique hashes) -> <out>/ds_archive_index.json.gz
python tools/re/ds_archive.py index
# all 211,391 paths of prefetch/fullgame.prefetch; 204,861 exist in the archives
python tools/re/ds_archive.py prefetch > prefetch.txt
# objects of every entity / art part / network core, with type names and GUIDs (JSON lines)
python tools/re/ds_archive.py scan '^ds/(entities|artparts/characters|animations/morpheme)/' --types dsdc_types.json.gz
# one file, with decoded fields
python tools/re/ds_archive.py core ds/entities/npc/mule/pe_friendly_mule --types dsdc_types.json.gz --decode 'SkinnedModelResource'
python tools/re/ds_archive.py extract ds/entities/npc/mule/pe_friendly_mule   # -> <out>/extracted/...
```

`--types` takes decima-workshop's RTTI type dump for Death Stranding Director's Cut, a GPL data file
(`decima-model/src/main/resources/metadata/dsdc_types.json.gz` in github.com/ShadelessFox/decima). It is
not shipped in this repo. Without it the tool still lists objects, but by type id only. Extracted data and
indexes go to `--out` (default: `%CLAUDE_SCRATCHPAD%` or `%TEMP%\ds_archive`). The tool refuses to write
inside the game folder. Do not commit extracted data.

### Format facts, and how each was checked

- **Archive headers and tables** **[verified]**:
  - The header and tables decrypt with the murmur3/salt scheme in the tool's docstring.
  - All 144 archives use magic `0x21304050` (encrypted) and `max_chunk_size` `0x40000`.
- **Chunk data** **[verified]**:
  - It decrypts with MD5(murmur3(chunk key) XOR salt2) and decompresses with the game's
    `oo2core_7_win64.dll` (`OodleLZ_Decompress`).
  - Every one of the 3,393 scanned cores and 2,005 clips decompressed to the exact entry size and parsed to
    the exact end.
- **Path hash** **[verified]**: the first u64 of murmur3_x64_128(seed 42) of lower-case `path.core` plus a
  NUL.
  - All 453 distinct core paths logged by the hidden runs (`build-testenv/runs/*/**/*.log`) resolve.
  - 204,861 of the 211,391 prefetch paths resolve.
  - Stream files are `path.core.stream`: tested on
    `ds/models/characters/fgl_fragile/core/fgl_body_def/model/parts/msh_bd_lx_16e912ee_001.core.stream`.
- **Type ids** **[verified]**:
  - They are a Python port of decima-workshop's `RTTITypeDumper`, fed with `dsdc_types.json.gz`.
  - The port reproduces ProjectDecima's independently known ids `PrefetchList` `D05789EAE3ACBF02`,
    `RTTIRefObject` `AA11412EDB60ECBC`, `Resource` `427090533D93D98A` and `ResourceWithName`
    `0BC5672CA3A1C549`.
  - It names 66,930 of the 67,100 objects in the scanned cores (99.75%). The unnamed ones have 3 ids:
    `6D837ED0D8A394F5` (18 art-part files), `53F83D0D00EB5B84` (`pe_cliff`) and `ECB32B92F19D008D`
    (`dsplayerdefaultkeybinding`).
- **Type JSON against ds.exe** **[verified]**:
  - `typecheck` compares the JSON with `tools/re/rtti.py`'s catalog of this ds.exe.
  - 9,301 of 9,302 class names are present, with the same base classes and sizes.
  - The only difference is one rename: the JSON's `RegularSkinnedMeshResourceBase` is `RegularSkinnedMeshResourceNG` in the exe.
  - So the JSON matches this game build.
- **Field decoding** **[verified]**:
  - The serialized field order is all members of the class and its bases, minus save-state members, sorted by offset.
  - Checked three ways:
    - Decoded objects end exactly at the object size (for example `SkeletonAnimationResource`,
      `SpawnSetup`, `SkinnedModelResource` and `Skeleton`).
    - The AKukan component order matches the runtime log.
    - `SkeletonLayoutHash` matches the in-game value.
- **GUIDs** **[verified]**:
  - For `EntityResource` subclasses, two bools (`Lockable` and `ZoomLockable`) are serialized before
    `ObjectUUID`, so the first 16 bytes are *not* the GUID. The tool reads the preceding fields first.
  - A `SpawnSetup` ref GUID equals the decoded entity GUID.

### Unknown or not checked

- **Duplicate copies.** 851 hashes exist in more than one archive, so which copy the game loads is unknown.
  None of the files indexed here is duplicated; only 6 of the 3,393 scanned are.
- **The GUID format the mod should use.** This page prints Windows-GUID text. A runtime comparison should
  compare the 16 raw bytes (`guid_raw` in `scan` output).
- **Fields after custom binary data.** Some classes (`MorphemeAnimationManagerResource` through `Pose`, and
  meshes) end in custom binary data, which the decoder leaves raw. Their references are still listed through
  the string scan.
- **Runtime behaviour.** Nothing here says whether any of these entities can be created safely at runtime,
  or what they need (AI, factions, controllers). That needs static work in ds.exe and a hidden test run.

### Archive legend

| Short | File in `data/` |
|---|---|
| 3460515f | `3460515fd6d930b689143a8b9a51a060.bin` |
| 40fb866e | `40fb866e44aec444200cdb0c384cf5a5.bin` |
| 477e458b | `477e458b2c825633499874678a2b9ea5.bin` |
| 596c31fc | `596c31fc02eca656d5ecdb546c52a7af.bin` |
| 7017f9bb | `7017f9bb9d52fc1c4433599203cc51b1.bin` |
| 968bf82f | `968bf82f34e2b499687c901a888e633a.bin` |
| a53f66da | `a53f66da5938a089f06b8a0c07c97f18.bin` |
| acc0d12f | `acc0d12f571a5b85f368497756cbcd3c.bin` |
| c0306eef | `c0306eef15c4b5122ab61a834f80d743.bin` |
| dc847b24 | `dc847b243b638c5ce539d4be7ff48007.bin` |
| fcd4b591 | `fcd4b5918cea9a220c0da171bb60c214.bin` |
