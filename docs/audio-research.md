# Audio research: the guest's voice and syncing both players' sounds

Status (2026-11-18, after v0.11.0): **the grunts are fixed: Sam's muted voice post is now stopped right after it is posted
(section 18)**. v0.11's volume 0 (17.2) was set back to full level by the game's own sound-instance update one update later,
so the user still heard Sam grunt as Fragile; measured by Wwise's own bus meters in run `20261118-113511-grunts-fragile`.
Status (2026-11-18, after the review): **Phase 1 changed, Phase 4 partly done, all phases re-run on the final code;
results and the review's findings in section 17.** Sam's muted voice is now *silenced in Wwise while the talk runs*
(the talk is no longer dropped when a PostEvent detour is in); partner sounds post only allow-listed event ids, from an
allow-list of the player's own components; stops are never dropped; `partner_volume=0..100` (launcher slider "PARTNER
SOUND VOLUME") sets the partner's sound volume. Final runs: `20261118-092559-two-games-audio` (launcher defaults, both
directions) and `20261118-092929-audio-keys-off` (Sam, every audio key off). Earlier status lines follow.
**Phase 3 done** (2026-11-18): `session.ini sound_sync=1` (`native/mod/partner_sound.cpp`, launcher Debug option
"Hear your partner's footsteps and sounds", default on) mirrors the Wwise events posted on the local player's own game
objects (animation foley, weapons, gear; footsteps by a position rule, `sound_steps`) as STA1 in the pose datagram, and
posts the partner's on Wwise objects of our own at the partner body's joints at the pose play time; hidden-run results in
**section 16**. **Phase 2 done** (2026-11-18): `session.ini voice_sync=1` (`native/mod/partner_voice.cpp`, launcher Debug option
"Hear your partner's voice", default on) sends each player's own talks as STS1 on the link's reliable channel and plays
the partner's with the game's `DSTalkComponent::PlaySentence` on an invisible talking entity at the partner body's head;
a player who is not Sam sends no Sam line. Hidden-run results in **section 15**. **Phase 1 done** (2026-11-18): `session.ini self_voice=auto|sam|mute` (`native/mod/self_voice.cpp`, launcher
Debug option, default auto) mutes Sam's voice on the game of a player who is not Sam; hidden-run results in
**section 14**. **Phase 0 done** (2026-11-18): the logging-only probe (`audio_probe=1`, `native/mod/audio_probe.cpp`)
ran in hidden games; its results are in **section 12** and the go/no-go for Phases 1-3 in **section 13**. Statements
the runs changed are marked *(changed 2026-11-18)*. Sections 1-11 were first written as static research (2026-11-17):
the evidence for them is the installed `ds.exe` (same build as [reverse-engineering.md](reverse-engineering.md):
PE timestamp 1706443442, base `0x140000000`; all addresses are VAs), decompiles from the Ghidra HTTP
plugin, capstone disassembly (`tools/re/dsre.py`), the RTTI catalog (`tools/re/rtti.py`) and the game's
packfiles read offline with `tools/re/ds_archive.py` and the decima-workshop type JSON. No game files were
changed or copied; no audio was extracted. Every feature proposed below reads the game's own resources at
runtime from the player's install and ships no game data.

Tags: **[verified]** = read from the exe or the data by a tool (decompile, disassembly, decoded resource,
run log cited). **[inferred]** = a reading of names, structure or partial code that no run has confirmed.

## 1. Short answer

1. **The game uses Wwise, statically linked, and ds.exe exports the Wwise API by name** [verified]. The PE
   export table has 199 `AK::SoundEngine::*` functions (`PostEvent`, `RegisterGameObj`, `SetPosition`,
   `SetSwitch`, `SetRTPCValue`, `StopPlayingID`, ...). The mod can find them with `GetProcAddress` on the
   game module, which survives game patches better than byte signatures.
2. **Sam's voice (lines, grunts, efforts, breathing, humming, pain) is one system: "situations" that pick
   sentences.** [verified for the data and the code-side path] 2,702 player sentence entries in 423
   situations, in `ds/entities/player/sentence/ds_player_sentence_resident`. The situations the game code
   asks for end as one message, `MsgDSStartTalk`, sent to the player entity and played by a
   `DSTalkComponent` from Sam's head joint [verified]. *(changed 2026-11-18)* **[verified, section 12.1]** this
   covers all of Sam's voice: the animation-driven grunts and efforts (events 2500-2584) go through Sam's
   sound graph to `PlayPlayerVoice` and the same queue, and the component handling `MsgDSStartTalk` is Sam's
   own `DSTalkComponent` (30 of the 33 talks in run `20261118-054042` were non-verbal sentences). One hook on
   that handler (`0x1422D4F70`), filtered to the local player entity, can mute or replace all of Sam's
   voice on the guest's own game. It does not touch footsteps, foley, cargo or weapon sounds.
3. **Fragile has no grunts, efforts or breathing as separately playable sentences** [verified over every
   sentence file that names her voice; her cutscene audio surely has some, inside the cinematic tracks]. She has 79 playable lines: 22 short "on actor" lines in the terminal lines and
   57 radio lines in mission files. Her cutscene speech is only subtitle text; its audio is inside the
   cinematic sound tracks and cannot be separated at runtime. So the realistic choices for the guest are
   **mute Sam's voice** (the default), optionally **play one of Fragile's few real lines** for a handful of
   situations, or (experimental) **borrow another female character's non-verbal set** (Mama has 193 lines
   without text, Amelie 18).
4. **Syncing sounds between the players** works best in two layers:
   - **voice by sentence**: send the sentence's GUID (language-independent), and the receiver plays it with
     `TalkComponent::PlaySentence` (script export `TalkComponent_ExportedPlaySentence`) on a talking entity
     that follows the partner's head;
   - **everything else by Wwise event**: hook the exported `AK::SoundEngine::PostEvent`, keep the events
     posted on the local player's own emitters (plus their RTPC values: *(changed 2026-11-18)* the game never
     calls `SetSwitch`, section 12.4), send `{event id, emitter slot, offset}`, and the receiver posts the
     same event on its own Wwise game object placed on the partner body's matching joint. Which game objects
     are the player's is known exactly for the component-played sounds and still open for two other
     creators (section 12.5). *(changed 2026-11-18)* **Built and verified (section 16)**: footsteps (the material
     emitters) are taken by position; every post was played on the other game (playing id != 0) and ended with
     `AK_EndOfEvent`.
5. Recommended order: Phase 0 (logging only) → Phase 1 (mute Sam's voice for a Fragile guest; small, local,
   no wire change) → Phase 2 (partner voice lines over the link) → Phase 3 (partner footsteps, foley,
   weapons over the link). See section 9.

## 2. The sound engine

### 2.1 Wwise inside ds.exe [verified]

- `ds.exe`'s export table (DLL name `NIP.Win64.Submission.DX12.Opt.Steam.exe`, 345 exports) includes the
  Wwise SDK: `AK::SoundEngine` (199 entries), `AK::MemoryMgr`, `AK::StreamMgr`, `AK::MusicEngine`,
  and the globals `g_pRegistryMgr` (`0x147E9B5F0`), `g_pRTPCMgr` (`0x147E9B5C0`), `g_pSwitchMgr` (`0x147E9B5E0`).
- **Version.** The soundbanks in `ds/sounds/wwise_bnk_collections/*` have `BKHD` version **135**. The
  exported `SetPosition(AkGameObjectID, const AkTransform&)` has no `AkSetPositionFlags` parameter, and
  `PostEvent(AkUniqueID, AkGameObjectID, AkUInt32, AkCallbackFunc, void*, AkUInt32, AkExternalSourceInfo*,
  AkPlayingID)` ends with a playing id. Together this points to a **Wwise 2019.2-era SDK** [inferred from
  the bank version and the mangled signatures]. Structure layouts (`AkTransform`,
  `AkExternalSourceInfo`, `AkCallbackInfo`) must be taken from that SDK generation.
- **Init** is in `0x141DD4280`: `MemoryMgr::Init`, `StreamMgr::Create` with Decima's own low-level IO hook,
  `SoundEngine::Init`, `MusicEngine::Init`, `RegisterGlobalCallback`, then `RegisterGameObj(0)` and
  `SetDefaultListeners({0})`. **Game object 0 is the listener**; every other game object uses the default
  listener, so a game object the mod registers is heard without further setup [verified in the decompile;
  playback itself untested].
- **Game object ids are Decima object addresses.** `0x141DC13F0` calls `RegisterGameObj(obj)` with the
  object pointer as the id, then `SetPosition` from the object's double world position at `+0x20/+0x28/+0x30`
  and its orientation at `+0x44..+0x58`. `RegisterGameObj` refuses ids ≥ `0xFFFFFFFFFFFFFFE0`
  (`cmp rcx,-0x20` at its start) [verified].
- **Axes.** Decima is Z-up and Wwise is Y-up. `0x141DC13F0` sends `(x, z, y)` for the position and swaps the
  same two components of the front and top vectors [verified in the decompile]. The mod must do the same
  swap when it positions its own game objects.

### 2.2 How Decima posts events [verified]

- The exported `PostEvent` by id (`0x143A1A5C0`) has 8 direct call sites. Seven are fixed posts on game
  object 0 or `~0`: for example `0x141DD4610` posts event `0x7913AFCB`, and `0x141DC6370`, `0x141DC6418`,
  `0x141DD2BF0` and `0x141DD2D21` post `0x0AF4925B` / `0xFD842642`, which look like pause/resume. The eighth,
  `0x141DD21CB`, is inside **`0x141DD2040`**. That is the native behind the graph-program exports
  `GraphSoundInterface::sPostEventExport` (`0x141DD1FB0` → `0x141DD2040`) and `sExtPostEventExport`
  (`0x141DD0BB0` → `0x141DD2040`); it also calls `StopPlayingID` at `0x141DD227A`. The by-name `PostEvent`
  overloads (`0x143A1A810`, `0x143A1A8F0`) have no direct callers.
- Sound effects are `GraphSoundResource`s (`ds/sounds/wwise_graph_sound_resource/...`). Each one runs a
  small graph program whose input is `inWwiseGameObjectID` (uint64) and posts `WwiseID`s taken from
  `ds/sounds/wwise_ids_coretext/generated/wwise_ids` (6,843 ids, no names) [verified in the data].
- So **every event post found in the code reaches the exported by-id `PostEvent`**, almost all of them
  through `0x141DD2040`. One hook there sees them all: event id, game object, flags, external sources
  [verified for the static call graph, direct calls only; indirect calls and `PostTrigger` not checked].
  *(changed 2026-11-18)* At runtime every post came from `0x141DD2040` (return address `0x141DD21D0`), ~95 % of
  them on one sound thread; voice posts come from job threads; `PostTrigger` and `SetSwitch` are never called
  (section 12.4).
- `SoundManager` singleton: `[0x147AE8700]`. `0x141DB61A0(mgr, 0, SoundResource*, flags)` creates a sound
  instance (used by the talk handler and by `SoundComponent` play) [verified]; how an instance is placed and
  started is not decoded yet.

### 2.3 Components that make the player's sounds [verified unless marked]

From `ds/entities/player/ds_player_character` (`DSPlayerEntityResource` 9b58ada6-…) and the run logs:

| Component | Where | Role |
|---|---|---|
| `DSCustomSoundComponent` (RTTI `0x144C5AC40`) | `ds/entities/player/dsplayersoundcomponentcoretext` | Receives animation sound events (`MsgAnimationSoundEvent`), throttles them, plays their sound, turns "voice" events into `MsgDSVoiceTriggerEvent`, and turns foot/hand impacts into footstep calls |
| `NodeGraphComponent` running `ds/sounds/state_machine/sd_state_machine_setup_sam` | component list | Sam's sound state machine. Entry points for `MsgEntityUpdate`, `MsgDSVoiceTriggerEvent`, `MsgAnimationTextEvent`. Its constants point at Sam's foley, footstep material, ladder, battery, stealth, BT-area and weapon graph sounds |
| `VoiceComponent` (+ `DSTalkComponent`) | `VoiceComponentResource` 65d3595c-… in the character core | The talking voice. `DSTalkComponent` handles `MsgDSStartTalk` and `MsgStartTalk`. *(changed 2026-11-18)* Sam has his own `DSTalkComponent` **[verified]** (the probe's component list, section 12.1) |
| `PlayerSoundMixComponent` (resource type) | `PlayerConfig` | Global mix states (sprint, zoomed weapon, reloading, mortally wounded, death). Never mirror these |
| `GearMovementSoundComponent` | gear entities | Gear rattle on `MsgPlayGearSound` |
| Voice manager | `DSPlayerEntity + 0x4500` | Queue of situation requests, see 3.2 |

`DSPlayerEntityResource` also names the footstep material resources (`FootstepDown/Up/Slow/Sprint/Crouch/
Drag/Jump/Land/Silent...MaterialTypeResource`, all in `ds/entities/dsmaterials`), `ResidentSentenceGroupResource`
(`+0x6B8`, see 3.1), `BasicVoiceTimeAdjustEfficient` 0.25, breathing aural stimuli for the AI, and fixed
"program sounds" (harmonica, ladder slide, handcuffs, cart attach, ...).

## 3. How Sam's voice is triggered and played

### 3.1 The data [verified]

- `DSPlayerEntityResource.ResidentSentenceGroupResource` → `ds/entities/player/sentence/ds_player_sentence_resident`
  (`DSSituationSentenceGroupResource` 09276e6e-…): 423 `DSSentenceSituationResource`s, each
  `{ConditionalSentenceResources: Array<Ref<DSPlayerSentenceResource>>, SituationStringHash: u32}`, and a
  hash map from situation hash to index.
- `DSPlayerSentenceResource`: `SentenceResource` (`+0x20`), `NoSpeakFlag`, `EchoSentenceIndex`,
  `IsNeedWaterVoice`, `IsTiredVoice`, `IsLongSentence`. 2,702 entries pointing at 1,566 distinct sentences:
  2,689 in `localized/sentences/ds_lines_sam/lines_sam/sentences`, 13 in `.../lines_1p5_dsp_sam/sentences`.
- `ds/entities/player/sentence/ds_player_sentence_priority`: 425 `DSSentenceSituationPriorityInfoResource`s
  `{SituationHash, Priority, Flag, Flag2, IsDialogue, IsReactionVoice, ...}`. Examples: `Jumping`
  priority 30, reaction voice; `OutOfBreath` 9, neither; `ReceivedLike` 30, dialogue.
- **Situation hash = CRC32-C with initial value 0 and no final xor** over the name, *(changed 2026-11-18)*
  masked to 31 bits (`& 0x7FFFFFFF`, `0x14190D040`) [verified]. Hashing every string
  in ds.exe this way resolves 52 of the 423 hashes [verified], for example `Jumping`, `JumpingDouble`,
  `LandingFromJumpHigher`, `OutOfBreath`, `IncreasinglyExhaustedBreathing`,
  `MovingBreathingWhenOverburdened`, `IdleBreathingWhenOverburdened`, `BreathingWhenClimbingSteepSlope`,
  `HoldBaggageNormal/Fast`, `HoldOnBig`, `SlipByRain`, `Drowning`, `Cold`, `Snore`, `Humming`,
  `ReceivedLike`, `AlmostThere`, `StrongDamageBatting`, `WeakDamageBatting`, `StrongPunchKick`,
  `UseRopeStart/End`, `StartRidingCart`, `CartImpact`, `ShookOffHunter`, `NearlyCaughtByHunters`,
  `MassagingShoulder`, `Private*` (private-room gestures). The other names are not stored as text anywhere
  scanned. `PlayPlayerVoice` passes its string through `0x14190D040` (the engine's name hash) [verified],
  which this matches.
- **Spoken and non-verbal.** Of Sam's 2,019 sentences in `lines_sam`, 924 have text and 1,054 have none
  (efforts, breaths, grunts, hums), plus 36 echo and 5 hunter-scene lines. By situation: 211 only spoken,
  149 only non-verbal, 63 mixed [verified, decoded].
- **A sentence** (`SentenceResource`): `Delivery` (`on_actor` for all of Sam's), `ShowSubtitles`,
  `SimpleSound` → `LocalizedSimpleSoundResource`, `Animation` (lip sync), `Text`, `Voice`
  (`localized/voices/vr0010_sam`). The sound is a Wwise **external source**: `ExternalSourceCookie` picks
  the event preset (`ds/sounds/wwise_localized_sound_presets/...`), `WemIDs` has 26 entries (one per
  voice language), `LengthInSeconds` too. `MinDist`/`MaxDist` 210 m [verified].

### 3.2 The triggers

**A. Code asks for a situation** [verified].
- `DSPlayerSystem_sExportedPlayVoice` (script name `PlayPlayerVoice`, `0x1426E6E50` → `0x1426E98A0`):
  takes a situation name, gets the player (`0x14283EC30`), builds `{u32 hash, i32 index = -1, u8 0,
  f32 0, f32 0}`, checks it (`0x142717F30`), then calls **`0x1427352E0`** = `voiceMgr = player + 0x4500;
  jmp 0x1426A6330`, which takes the manager's critical section (`+0x1C8`) and calls the enqueue
  **`0x1426A6570`**.
- `0x1427352E0` has 46 direct call sites in game code (damage, cargo, carts, ropes, hunters, ...).
- The enqueue looks up the situation's priority info (`[mgr+0x30]`). It drops the request when a queued
  entry has a higher priority, and drops some situations (a flag at `info+0x33`) within 10 s of the last
  one. It merges a request with a queued entry for the same situation, and otherwise appends a 0x38-byte entry
  (`{u32 hash, i32 index, u8 flag, ..., f32 delay, ..., u8 state}`) to the queue at `mgr+0x40` (count `+0x38`).

**B. Animation events marked as voice** [verified up to the graph; the last step is inferred].
- Morpheme animation sound events reach `DSCustomSoundComponent::OnAnimationSoundEvent`
  (`0x142D98D60` → **`0x142D83630`**). The message is `{+0x10 i32 event index, +0x18 SoundResource*,
  +0x20 position/helper data, +0x28 u8 duration event, +0x29 u8 started}`.
- The component's resource (`comp+0x30`) has three tables (each `{i32 count @+0x40/+0x50/+0x60, data
  @+0x48/+0x58/+0x68}`, 0x18-byte entries):
  - `CustomSoundEventInfo`: index ranges with `TimeToIgnoreEventsAfterPlay` (a 10-slot throttle at
    `comp+0x98`) and `IsVoice`;
  - `CustomSoundEventVoiceInfo` (empty for Sam);
  - `CustomImpactEventInfo`: `PlayerFootStepLeft/Right/Root`, `PlayerHandL/RTouch`, `PlayerStepOnPoint`.
- Sam's voice ranges are event indices **2500-2584** (20 ranges, `IsVoice` = true). Footsteps are 3100-3102
  (left), 3150-3152 (right), 3199 (root), 3108/3158 (slow); the other ranges up to 4022 are foley.
- For an `IsVoice` index, the component sends `MsgDSVoiceTriggerEvent {+0x10 SoundEventIndex}` (vtable
  `0x143DEBB80`) to its entity (`comp+0x48`) with `0x14235C990(entity, msg, 0)`. No native type handles that
  message (all 9,305 RTTI message tables scanned). Only the node graph `sd_state_machine_setup_sam` has an
  entry point for it, in a sub-graph named `SentencePlayResource_00_09`.
- *(changed 2026-11-18)* **[verified, section 12.1]** The graph turns the index into a situation and asks the
  voice manager through the `PlayPlayerVoice` script export, so these efforts also end in path C (the
  enqueue's stack holds `0x1426E991D` inside `PlayPlayerVoice` and the graph runtime; the index → situation
  table is in 12.1).
- Impact indices call `0x142706E30(entity, type, helper)` → the player mover `[entity+0x520]` →
  `0x1427904D0`, which sends a footstep message with the helper name to the mover's entity (footsteps go by
  surface material, not by a fixed sound).
- Any other index plays the event's own `SoundResource` on the component (`0x142D78060` →
  `0x142D88290` → `SoundComponent::PlaySound` **`0x14235A460`**).

**C. Playback** [verified].
- The voice manager's per-frame update **`0x1426C2040`** (called from the player update `0x142730B30` with
  `player+0x4500`) takes a due queue entry and picks a `DSPlayerSentenceResource` with **`0x1429232F0`**
  (`(situationGroup [mgr+0x28], entry, 0, ...)`; conditions such as water, tiredness, `NoSpeakFlag`).
- It builds **`MsgDSStartTalk`** on the stack (vtable `0x143CF3A50`; helper string `"JNT_C_B_008_Head"`; a
  volume or priority float, 4.4 / 4.6 / 4.3 by situation; *(changed 2026-11-18)* every message the handler saw
  carried 1.5 at `+0x10`, section 12.2) and adds the sentence with `0x1422D3D90(msg,
  SentenceResource*, delay, 0.1)` into the array at `msg+0x30`. It then sends the message to the player
  entity with `0x14235C990(player, &msg, 0)`.
- `DSTalkComponent`'s handler (`0x1422D6AA0` → **`0x1422D4F70`**) reads `msg+0x10`, `+0x28`, the entries
  at `msg+0x38` (count `+0x30`, 0x18 bytes each: `{SentenceResource*, SoundResource*, f32, f32, ...}`) and
  `+0x40`. *(changed 2026-11-18)* Layout confirmed on 83 messages; the `SoundResource*` was null in all of them
  (section 12.2). The handler runs on job threads. For each entry with a sound it creates an instance (`0x141DB61A0(SoundManager, 0, sound, 0)`)
  and queues the talk.
- **The cargo menu's Sam** speaks through `DSPlayerEntity_sExportedPlayAkukanVoice` (`0x142757D40` →
  `0x14273BB10` on the akukan entity) [verified]; its resource has its own `DSCustomSoundComponent`,
  `VoiceComponent` and `NodeGraphComponent` (`sd_state_machine_setup_sam_akukan`) [verified, census log].
  *(changed 2026-11-18)* **[verified, decompile and section 14]** `0x142757D40(int list)` takes the figure from the
  subspace work object (`[[0x147BE79B0]+0x30]-0x20`, `0x14269C560`) and **tail-jumps** (`E9` at `0x142757D63`) to
  `0x14273BB10(figure, list)`. That one takes the figure's lock (`+0x790`), picks a random sentence of line list
  `list` from the figure's resource (StreamingRef at `+0x58`; list count `+0x118`, lists `+0x120`, each with its
  sentence count `+0x20` and array `+0x28`) and sends the same `MsgDSStartTalk` as the voice update (`+0x10` = 1.5,
  helper `"JNT_C_B_008_Head"`, one entry with delay 0 and 0.1) **to the figure itself**, whose own
  `DSTalkComponent` handles it (owner `[comp+0x48]` = the figure). The 100% save's figure has **2 lists of 8
  sentences**; its lines post the external-source events `0x537EDC0A` and `0x83FBDDAA`.

### 3.3 Where to suppress or swap (guest playing Fragile)

| Hook | What it sees | Use | Risk |
|---|---|---|---|
| **`DSTalkComponent::OnMsgDSStartTalk` `0x1422D4F70`** (MinHook; or redirect the 5-byte `jmp` thunk at `0x1422D6AA0`) | Every talk request to any entity with a talk component. Filter: owner `[comp+0x48]` == local player entity (*(changed 2026-11-18)* exactly that entity: BB and the cuff device are child entities with their own `DSTalkComponent`, section 12.1) | **Recommended.** Drop the message (mute), or replace the entries' `SentenceResource*` and `SoundResource*` (swap), keeping refcounts (`+0x18`, `lock xadd`). It also carries the sentence for Phase 2 | Medium. Called for NPCs too; the filter must be one compare. The entry layout needs a Phase 0 check |
| Sentence pick `0x1429232F0` | `(group, queue entry)` → `DSPlayerSentenceResource*` | Returning 0 for a normal situation drops it before any message is built | Higher. The 0 path for the two special hashes (`[0x144BB4774]`, `[0x144BB4778]`) falls back to another list |
| Enqueue `0x1426A6570` | Situation hash, index, flags | Classification (spoken or not, priority) and logging | Low as a pure logger; not needed to mute |
| `PlayPlayerVoice` export | Script-requested situations only | Not enough on its own: misses the 46 native callers | — |

Muting at the talk handler keeps Sam's **non-voice** sounds (footsteps, cloth, cargo, weapons, Odradek,
BB). *(changed 2026-11-18)* The animation-driven efforts and grunts do reach this handler **[verified, 12.1]**,
so that is what the user asked for: the grunts stop, everything else stays.

The handler tables are in writable `.data` (`DSTalkComponent`'s at `0x144AE84C0`, 13 entries;
`MsgDSStartTalk` → `0x1422D6AA0` at `0x144AE8568`). Whether Decima copies them into a dispatch cache at
startup is unknown, so an inline hook is the safer choice [inferred].

## 4. Fragile's voice in the game data [verified unless marked]

- `localized/voices/vr0040_frg`: `VoiceResource` "vr0040_frg", VoiceID 314153630, Gender `Female`.
- 30 sentence files name her voice (403 sentence references):
  - **cutscenes** (`ds_lines_cutscene/sq_cs*`): 324 sentences, all `Delivery = radio`, with text and
    **no `SimpleSound`**. They are subtitles only: the audio is in the cutscene's own Wwise track
    (`ds/sounds/wwise_cinematics_sound_resource/...`) and cannot be cut out at runtime;
  - **missions** (`ds_lines_mission/lines_m00160 ... m00650`): 57 radio lines with sound (6 under 1.2 s);
  - **terminals** (`ds_lines_terminal/lines_01_generic_terminal/sentences`, a 1.5 MB core): 22 `on_actor`
    lines with sound (10 under 1.2 s, the rest 1.2-5.4 s).
- She has **no sentence without text**, so none of her grunts, efforts or breathing exist as separately
  playable sentences (her cutscene audio surely has some, but only inside the cinematic tracks).
- Other female non-verbal sets that do exist: Mama (`ds_lines_npc/lines_mama`, 193 without text, 90 with),
  Amelie (`lines_amelie`, 18 without text). The porter NPC sets (`lines_porter01..03`, 71 each) are male
  voices (`vr4010..4030`, Gender `Male`).
- "Ripping" Fragile's efforts from her cutscene audio would mean extracting and editing game audio. That
  is out of scope: the project never copies or ships game assets, and this cannot be done from the runtime
  data alone. A user-supplied voice pack played by the mod's own audio output is possible in principle,
  but it would bypass the game's mix (3D, reverb, pause, volume sliders). **Not recommended.**

## 5. What the guest's game should do with Sam's sounds when the guest is Fragile

| Sound | Today | Proposal (default) | Option |
|---|---|---|---|
| Sam's spoken situational lines (211 + mixed situations) | Sam's voice from the hidden Sam's head | **Mute** | Map a few situations to Fragile's 22 terminal lines (for example `ReceivedLike`); needs her terminal sentences loaded by path |
| Sam's non-verbal voice (efforts, breathing, grunts, pain, humming, snoring; 149 + mixed situations, `IsVoice` animation events 2500-2584) | Sam's voice | **Mute** | Experimental: borrow a female non-verbal set (section 4), mapped by category |
| Cargo menu figure (`PlayAkukanVoice`) | Sam's voice | **Mute** while the menu shows Fragile | — |
| Footsteps, cloth/gear foley, cargo creaks, ladder/rope, weapon fire, vehicles | Sam's own emitters | **Keep** (they fit any porter) | — |
| BB (`lines_bb`, voice `vr0150_bb`), Odradek, radio/codec calls from other characters | Their own entities and routes | **Keep** | — |

The host is Sam, so nothing changes on the host's own game. A new `session.ini` key, for example
`self_voice=auto|sam|mute|fragile`, would be set by the launcher (`auto` = mute when `my_character` is not
Sam). With the feature off, play is unchanged.

## 6. Playing sounds on the partner body

The partner body is a cutscene actor (`DSControlledEntity`). The guest's default body, Fragile in her porter
suit (`ds/entities/cinematics/demo_fgl_body_suiti`, `fragile_suit`), has 3 components in its
`EntityComponentResources`: skinned model, external animation, LOD override; the leather-jacket Fragile
(`demo_fgl_new`) has the same plus a stencil component. Neither has a sound, voice or talk component, and the
pose is copied, so the body produces no animation events of its own [verified from both resources decoded
offline; runtime components not logged for these bodies]. Sounds for the partner therefore need an emitter that
the mod drives.

### 6.1 Options, ranked

**1. Voice by sentence (recommended for speech).**
- *Sender:* the talk-handler hook (3.3) sees the local player's `MsgDSStartTalk` and sends the sentence
  GUIDs (`RTTIRefObject + 0x08`), the situation hash (from the enqueue logger, optional) and the delay.
- *Receiver:* resolves each GUID to its own copy of the same `SentenceResource` and plays it with
  **`DSTalkComponent::PlaySentence`** (vtable `+0x68` = `0x1422D3FE0`;
  `TalkComponent_ExportedPlaySentence` `0x141DF4F00` is `jmp [vt+0x68]`). Signature:
  `bool (TalkComponent*, SentenceResource*, i32 param, Params* opt)`; the default volume factor is
  `0x3F7C28F6` (0.985) when `opt` is null [verified decompile]. Because the GUID is language-independent,
  each player hears the line in their own VO language with their own subtitles.
  *(changed 2026-11-18)* **[verified, decompile and section 15]** `PlaySentence` builds a `MsgStartTalk` (not
  `MsgDSStartTalk`: vtable `0x143D02660`, sentence at `+0x30`, volume at `+0x60`) and sends it to the component's
  **owner entity** (`0x1422D6230` -> `0x14235C990`, synchronous for an entity without a game-actor id); the owner's
  `DSTalkComponent` handles it in `0x1422D5A40` (thunk `0x1422D6B20`), which needs a `VoiceComponent` on the same
  entity, and sets the message's result byte (`+0x50`) that `PlaySentence` returns. Called from the game thread on the
  echo entity it returned 1 for every line in section 15, and the line was posted on a `WwiseSimpleSoundInstance` at
  the entity within about 15-30 ms.
- *GUID → pointer:* walk the local player's `ResidentSentenceGroupResource` (`DSPlayerEntityResource+0x6B8`
  → `SituationResources` → `ConditionalSentenceResources` → `+0x20`) once with fault-free reads; it is
  resident on both games because both run Sam. Fragile's terminal lines, if used, need the
  register-by-path loader (`real-partner-body.md` 2.1) for
  `localized/sentences/ds_lines_terminal/lines_01_generic_terminal/sentences`.
- *Emitter:* a talking entity kept at the partner's head:
  - the invisible echo entity that `partner_voice=1` already spawns (`DSPlayerEchoManagerConfig.EntityResource`;
    `DSEchoComponent` + `VoiceComponent`, and `DSTalkComponent` at runtime per `partner-hologram.md`);
  - or the knot-space echo resource (`DSPlayerEchoManagerConfig+0x28`; `VoiceComponent`,
    `DSKnotspaceEchoComponent`, `SoundComponent`) [verified resource].

  The echo's own `DSEchoComponent::Play` (the debug "Voice ping") only plays its fixed list of Sam echo
  lines, so it is not the route for arbitrary lines. *(changed 2026-11-18)* The echo entity is the emitter
  `voice_sync` uses (section 15): at runtime it has `DSEchoComponent`, `VoiceComponent` and `DSTalkComponent`, and no
  model [verified, `[partner] partner voice entity ...: 3 components`].
- *Cost:* under 1 message a second; it needs reliable delivery.

**2. Wwise event mirror (recommended for footsteps, foley, cargo, weapons, vehicles).**
- *Sender:* hook the exported `AK::SoundEngine::PostEvent` by id (`0x143A1A5C0`), `SetSwitch` (`0x143A202F0`),
  `SetRTPCValue` (`0x143A1FC10`), `RegisterGameObj` (`0x143A1CF50`), `UnregisterGameObj` (`0x143A21640`),
  `SetPosition` (`0x143A1FA50`) and `StopPlayingID` (`0x143A20B20`), all found by export name.
  - Keep only the game objects that belong to the local player (6.2).
  - For those, record the event id, the playing id it returns, and the latest switch and RTPC values set
    on that object (footstep surface, load weight, speed, ...).
  - Skip game object 0 (listener), `~0` (global), the external-source posts (voice: handled by option 1),
    and `SetState` (global mix states).
- *Receiver:* registers a few private game objects. Their ids are addresses of statics inside the mod DLL,
  so they are unique and below `0xFFFFFFFFFFFFFFE0`: head, left foot, right foot, right hand, back/pelvis.
  *(changed 2026-11-18, built: section 16)* A pool of 24 objects bound to the sender's game objects (the STA1 `tag`)
  rather than one per joint, so each sound keeps its own RTPCs; each bound object follows one of seven joints (head,
  both feet, both hands, the spine the backpack hangs on, the hips) plus the offset the sender measured. **`AkTransform`
  in this build is `{front, top, position}` as 3 × 3 floats** [verified, decompile of `0x141DC13F0`: front from the
  object's `+0x44..+0x4C`, top from `+0x50..+0x58`, position from the doubles at `+0x20/+0x28/+0x30`, each as `(x, z,
  y)`], not `{position, front, top}`. The exported API called from the game thread (`RegisterGameObj`, `SetPosition`,
  `SetRTPCValue`, `ResetRTPCValue`, `PostEvent` with `AK_EndOfEvent`, `StopPlayingID`, `StopAll`) worked in every run.
  - Every frame, `SetPosition` each one at the partner body's matching joint (Y/Z swapped, 2.1).
  - On an event: replay the switches and RTPCs on the matching object, `PostEvent(id, obj)`, and remember
    remote playing id → local playing id, so a later stop (`StopPlayingID` or `ExecuteActionOnPlayingID`) for
    loops (cart, vehicle, rain on the cargo) stops the right sound.
- *Pros:* covers every sound family with one mechanism (graph sounds, material footsteps, weapon fire);
  the event ids are the same on both games (same banks); the Wwise API is exported, and the Wwise SDK documents
  its calls as safe from any thread [inferred for this build: not checked in ds.exe].
- *Cons:* needs the ownership mapping (6.2). It bypasses Decima's own instance limits and pause handling.
  Banks must be loaded on the receiver: they are for Sam's sounds, which both games use.

**3. Decima animation-event replay (fallback for foley only).**
- *Sender:* the `DSCustomSoundComponent::OnAnimationSoundEvent` hook (`0x142D83630`, owner `[comp+0x48]` ==
  local player) records `{event index, SoundResource GUID, helper, duration flags}`.
- *Receiver:* sends a constructed `MsgAnimationSoundEvent` to an emitter entity that has a plain
  `SoundComponent` (whose handler `0x142363B70` → `0x142351AE0` plays `msg+0x18` on itself), for example
  the knot-space echo entity.
- *Gaps:* misses material footsteps (they go through the player mover, 3.2 B), weapon fire and the
  graph-driven sounds. It also needs a message vtable and refcount handling.

**4. Own audio output (not recommended).** XAudio2/WASAPI from the mod with its own 3D panning. It is
outside the game's mix and volume settings, and it would need audio data the project must not ship.

### 6.2 Which Wwise game objects are the local player's

This mapping is the main unknown for option 2. *(changed 2026-11-18)* Phase 0's answer is in **section 12.5**:
candidate 1 works for every sound a component plays (through the component's representation, not the
instance itself); the instances made by the material / physics emitters and by the graph runtime's pools
are not tied to an owner yet. Candidates, in order of preference:

1. **By component** [needs Phase 0]. `SoundComponent::PlaySound` (`0x14235A460`) and the talk handler
   create instances through `0x141DB61A0`. Each instance is a Decima object; its address is the Wwise
   game object id (2.1). Hooking the instance creation for components whose owner is the local player gives
   the exact set. The instance's owner field and its link to the component are not decoded yet.
2. **By graph context.** `0x141DD2040` posts on the `inWwiseGameObjectID` given to the graph program. Its
   caller frames carry the `SoundInstance`. Decode which argument that is.
3. **By position (heuristic).** Keep game objects whose `SetPosition` stays within ~1.5 m of the local
   player's joints and that are registered while the player is active, then confirm by event-id statistics
   (Phase 0). This is cheap but fragile near other porters and inside vehicles.

### 6.3 Timing

*(changed 2026-11-18, built: section 16)* Each STA1 item carries its age against the frame's `sender_ms` (the pose's
clock); the receiver posts it once `Ring::PlayTime` reaches it: 0-34 ms after that moment in the two-game runs (one
tick).

- Pose frames are played at an interpolated play time (`Ring::PlayTime`, `real-partner-body.md` section 4).
- Audio events should carry `sender_ms` and be scheduled at the same play time, so footsteps land when the
  partner's feet do. Voice may play as soon as it arrives.

## 7. Wire proposal (same rules as the other frames)

Both frames are new message tags. They are sent only when both peers are on the same build (HELLO build
check), and each feature is off by its own `session.ini` key.

- **`STS1` (speech)**: `'STS1' | u8 version 1 | u8 kind | u8 flags | u8 n | u32 seq | u32 sender_ms |
  u32 situation hash (0 = unknown) | n × 16-byte sentence GUID` (n ≤ 4).
  - kind: 0 = sentence(s) played as sent; 1 = the sender muted it (receiver plays nothing, for statistics);
    2 = Fragile line by GUID.
  - Reliable (the link's resend path), idempotent by `seq`.
  - The receiver refuses GUIDs it cannot resolve and logs the first few.
  - *(changed 2026-11-18, built: section 15)* kind 1 carries **no** GUID (n = 0: a player who is not Sam sends
    nothing of Sam's line, only that one was said); kind 0 and 2 need 1..4. Flags bit 0 = sent by the host (other bits
    cleared). Seq 0 and an all-zero GUID are refused. It rides the link's reliable ordered channel 2
    (`link_protocol.hpp` `ReliableSender` / `ReliableReceiver`, now pumped by `link.cpp`), so it is resent until
    acknowledged and delivered once, in order; the receiver also drops a seq it has seen. The situation hash is 0
    (the talk handler does not see it). Wire table: `real-partner-body.md` section 18.
- *(changed 2026-11-18, built: section 16; wire table: `real-partner-body.md` section 19)* **STA1 as built**:
  `'STA1' | u8 version 1 | u8 n | u16 seq | u32 sender_ms` (12 bytes), then n ≤ 32 items of 24 bytes `{u32 a | u32 b |
  u8 op | u8 slot | u8 tag | u8 nrtpc | u16 age_ms | u16 fade_ms | i16 off[3] cm | u8 category | u8 0}` plus nrtpc ≤ 12
  × `{u32 rtpc id | f32 value}`, at most 512 bytes. Ops: post, stop (playing id), action on event, RTPC update. RTPCs
  only (no kind byte: no switches exist). Rides in the pose datagram (alone when it does not fit).
- *Proposal (2026-11-17)*: **`STA1` (audio events)**: `'STA1' | u8 version 1 | u8 n | u16 seq | u32 sender_ms`, then n × `{u32 event
  id | u32 remote playing id | u8 emitter slot | u8 op (post/stop) | u8 nstate | u8 0 | nstate × {u8 kind
  switch/rtpc | u8 0 | u16 0 | u32 group/rtpc id | u32 value or f32}}`. *(changed 2026-11-18)* Only RTPCs are
  needed (no `SetSwitch` calls at all, section 12.4); up to 10 RTPCs were set on one object before one post
  (`0xB7A24F27`, a vehicle), 7 for the RTPC-rich movement events, so `nstate` ≤ 12 is enough.
  - Batched per send tick (30 Hz), riding in the pose datagram when it fits.
  - Unreliable: a lost footstep is better than a late one.
  - Decoders refuse bad sizes, counts, slots and non-finite values, as the other decoders do
    (unit tests in `st_net_tests`).
- **Budget** [inferred]: walking about 2-4 footstep posts a second plus 5-15 foley posts; at about 12-20 bytes
  each, roughly 200-400 B/s per direction. That is small next to the pose stream (about 17 KB/s).

## 8. Pitfalls

- **Crash reporter.** Any fault inside a game call is fatal for the user (`reverse-engineering.md`).
  - Read through `ReadProcessMemory`.
  - Byte-check every hooked prologue (section 10).
  - Check RTTI (`DSTalkComponent` `0x144AE8400`, `DSCustomSoundComponent` `0x144C5AC40`) and the owner before
    acting.
  - Never send a message built from guessed layouts until Phase 0 confirms them.
- **Threads.** `PostEvent` may be called from sound or job threads. Hooks must only copy a few words into a
  lock-free ring and return. No allocation, no logging on the hot path.
- **Feedback loops.** The sender must ignore the mod's own game objects and its own `PlaySentence` calls.
- **Global state.** Do not mirror `SetState`, global RTPCs (game object `~0`), mix states
  (`PlayerSoundMixComponent`) or music.
- **2D or player-centric events.** Some events posted on Sam's objects may be meant only for the local
  player (UI-like ticks, low-stamina cues). Phase 0 builds a deny-list from the event statistics.
- **Pause, menus, cutscenes, loading.** Decima pauses its sound groups; the mod's direct posts are not tied
  to that. Mute partner audio while the local game is paused, in a cutscene or loading (reuse the
  GameModule and DSGameState checks of the voice ping, `debug-actions.md`).
- **Distance.** Wwise attenuation handles volume. Skip posting when the partner is beyond about 100 m, to
  save voices; Sam's voice uses `MaxDist` 210 m.
- **Instance limits.** Mirrored posts count against Wwise voice limits and skip Decima's `SoundGroup`
  instance limits. Rate-limit per slot.
- **Duplicates.** Nothing on the partner body makes sound today. If a later feature gives the body real
  animation events or a mover, turn the matching mirror off.
- **Subtitles.** `PlaySentence` on the receiver may show the partner's subtitles locally. That can be good
  (radio-like) or noisy; make it an option. *(changed 2026-11-18)* **[verified, section 15.4]** It does: a spoken
  partner line (`ShowSubtitles` 1) asks the subtitles HUD for its `Text`. `voice_subtitles=0` (the default) drops
  exactly that request; `voice_subtitles=1` keeps it.
- **Language.** Never mirror voice at the Wwise level: the external source's WEM id depends on the voice
  language (26 per line).
- **Hidden tests are silent.** The test environment flags every audio buffer silent (`hidden-testing.md`),
  so audio can be verified only by logs and callbacks (8.1).

### 8.1 How to verify without hearing

*(changed 2026-11-18)* Phase 0 did exactly this: every claim in section 12 comes from the probe's log lines
(counts, return addresses, stack words, owners), never from sound.

- Count `PostEvent` results (playing id ≠ 0) and Wwise callbacks (`AK_EndOfEvent`, `AK_Duration`) for the
  mod's own posts.
- Log `PlaySentence`'s return value and the talk component's state.
- Optional: a test-only meter in the audio hook. Read the peak of the buffer before the SILENT flag is set;
  this needs `GetBuffer` hooked as well. It stays inside the test environment.
- `AK::SoundEngine::StartOutputCapture` is exported and not a stub [verified prologue], but its file path
  goes through Wwise's file layer. Do not use it until it is proven to write only into the run folder.

## 9. Phased plan

**Phase 0 - instrumentation (logging only, hidden runs).** *(changed 2026-11-18)* Done: results in section 12,
decisions in section 13. Behind `audio_probe=1`:
- log the enqueue `0x1426A6570` (situation hash, caller return address);
- log the talk handler `0x1422D4F70` (owner, sentence GUIDs, entry layout);
- log `DSCustomSoundComponent::OnAnimationSoundEvent` (index, resource GUID, duration flags) for the local
  player;
- log `PostEvent` / `RegisterGameObj` / `SetSwitch` / `SetRTPCValue` per game object (counts and ids only,
  lock-free).

Scenario: walk, sprint, jump, fall, carry heavy cargo until tired, wade, climb a rope, ride a cart, fire a
gun, open the cargo menu. Goals:
- confirm path B ends in path C;
- confirm the `MsgDSStartTalk` entry layout and that Sam has a `DSTalkComponent`;
- find the owner mapping (6.2);
- build the event deny-list.

**Phase 1 - mute Sam's voice for a Fragile guest (local only, no wire change).** *(changed 2026-11-18)* Done:
`native/mod/self_voice.cpp`, results in section 14.
- `self_voice` key, launcher wiring and docs.
- Hook the talk handler: drop requests addressed to the local player when the setting is mute.
- Mute the akukan voice while the cargo menu shows Fragile.
- Unit-test the setting parsing.
- Hidden run: log counts of muted requests; check that footsteps and foley posts are unchanged.

**Phase 2 - partner voice.** *(changed 2026-11-18)* Done: `native/mod/partner_voice.cpp`, results in section 15.
- `STS1` with encoder/decoder tests.
- GUID map from the resident group.
- Talking emitter at the partner's head (the echo entity, `partner_voice` path), `PlaySentence` on receive.
- A Fragile guest sends kind 1 (muted) or kind 2 (her own line) only.
- Two-game hidden run: both logs show sent, received, resolved and played (return 1).

**Phase 3 - partner footsteps, foley, weapons.** *(changed 2026-11-18)* Done: `native/mod/partner_sound.cpp`, results in
section 16 (option 2 throughout; the option 3 fallback was not needed).
- `STA1`, the Wwise mirror (option 2) with private game objects on the partner's joints, switch and RTPC
  replay, stop mapping, distance and pause gating.
- Two-game hidden run with posting statistics and Wwise end-of-event callbacks.
- Fallback to option 3 for animation foley if the owner mapping fails.

**Phase 4 (optional).** *(changed 2026-11-18: the volume is done, section 17.3; the rest is not started)*
- Fragile line mapping for a few situations. *Not done*: her 22 terminal lines live in another sentence group that is not
  resident; loading it means the register-by-path loader and a new STS1 kind-2 path, not trivial (section 17.6).
- Experimental female non-verbal substitute. *Not done.*
- A partner-volume slider (`SetGameObjectOutputBusVolume` on the private objects). **Done** (`partner_volume`, section 17.3).

## 10. Addresses and first bytes (for byte checks)

| VA | What | First bytes |
|---|---|---|
| `0x1422D4F70` | `DSTalkComponent` `MsgDSStartTalk` handler body | `40 55 53 41 55 48 8D 6C 24 B9 48 81 EC F0 00 00` |
| `0x1422D6AA0` | its thunk (handler table entry) | `E9 CB E4 FF FF` |
| `0x1422D3FE0` | `DSTalkComponent::PlaySentence` (vt `+0x68`, vtable `0x143CF65E0`) | `48 89 5C 24 18 55 56 57 48 81 EC B0 00 00 00 48` |
| `0x141DF4F00` | `TalkComponent_ExportedPlaySentence` | `48 8B 01 48 FF 60 68` |
| `0x142D83630` | `DSCustomSoundComponent::OnAnimationSoundEvent` | `48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57` |
| `0x142D98D60` | its thunk | `E9 CB A8 FE FF` |
| `0x1426A6570` | voice manager enqueue (`player+0x4500`) | `40 55 56 57 41 54 41 56 41 57 48 81 EC 98 00 00` |
| `0x1426A6330` | enqueue with the manager lock (`+0x1C8`) | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57` |
| `0x1427352E0` | `DSPlayerEntity` → voice manager | `48 81 C1 00 45 00 00 E9 44 10 F7 FF` |
| `0x1426C2040` | voice manager update | `4C 8B DC 55 57 49 8D AB 58 FE FF FF 48 81 EC 98` |
| `0x1429232F0` | situation → `DSPlayerSentenceResource` pick | `40 53 55 41 56 48 83 EC 30 8B 02 49 8B E8 48 8B` |
| `0x1426E98A0` | `PlayPlayerVoice` body (export thunk `0x1426E6E50`) | `48 89 5C 24 10 57 48 83 EC 40 48 8B 05 7F 77 87` |
| `0x142757D40` | `PlayAkukanVoice` (→ `0x14273BB10`) | `40 53 48 83 EC 20 8B D9 48 8B 0D 61 FC 48 05 E8` |
| `0x142757D63` | its tail jump to `0x14273BB10` *(added 2026-11-18)* | `E9 A8 3D FE FF` |
| `0x14273BB10` | the figure says line list `n` (builds and sends `MsgDSStartTalk`) *(added 2026-11-18)* | `4C 8B DC 55 53 49 8D 6B A1 48 81 EC B8 00 00 00` |
| `0x14235C990` | send message to entity | `4C 8B DC 53 55 57 48 83 EC 50 48 8B 05 8F 46 C0` |
| `0x14235A460` | `SoundComponent::PlaySound` | `40 55 56 41 54 41 56 41 57 48 81 EC F0 00 00 00` |
| `0x141DB61A0` | `SoundManager` create instance (`[0x147AE8700]`) | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57` |
| `0x141DD2040` | Decima → `AK::SoundEngine::PostEvent` | `40 55 56 41 54 41 55 41 56 41 57 48 83 EC 68 48` |
| `0x143A1A5C0` | export `?PostEvent@SoundEngine@AK@@YAKK_KKP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXKPEAUAkExternalSourceInfo@@K@Z` | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 4C` |
| `0x143A1CF50` | export `RegisterGameObj` (both overloads) | `40 53 48 83 EC 20 48 8B D9 48 83 F9 E0 72 0B B8` |
| `0x143A1FA50` | export `SetPosition(_K, const AkTransform&)` | `48 83 F9 E0 72 06 B8 02 00 00 00 C3` |
| `0x143A21640` | export `UnregisterGameObj` | `40 53 48 83 EC 20 48 8B D9 48 83 F9 E0` |
| `0x143A202F0` / `0x143A1FC10` / `0x143A20B20` | exports `SetSwitch(K,K,_K)`, `SetRTPCValue(K,M,_K,...)`, `StopPlayingID` | `48 89 5C 24 08 48 89 74 24 10 57 48` / `48 83 EC 48 0F B6 44 24 78 88 44 24` / `85 C9 74 5C 48 89 5C 24 08 48 89 74` *(added 2026-11-18)* |
| `0x143A17E30` | export `ExecuteActionOnEvent` (by id) *(added 2026-11-18)* | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57` |
| `0x141DCBC30` | sound instance (re)attach (`mgr, inst, p3, flags`) *(added 2026-11-18)* | `48 85 D2 0F 84 37 02 00 00 48 89 5C 24 18 48 89` |
| `0x14235AAC0` / `0x1423493E0` | the other two SoundComponent play paths (`this` = the representation) *(added 2026-11-18)* | `40 53 41 55 41 56 41 57 48 81 EC C8 00 00 00 48` / `40 53 55 41 54 41 55 41 57 48 81 EC A0 00 00 00` |
| `0x142730B30` | the player's per-frame update (`this` = `DSPlayerEntity`) *(added 2026-11-18)* | `48 8B C4 48 89 58 18 48 89 70 20 57 41 56 41 57` |
| `0x141AB3CA0` | material / physics sound emitter start (creates instances, sets RTPCs `0x141DC6490`) *(added 2026-11-18)* | `4C 8B DC 53 48 83 EC 60 48 8B 05` |
| `0x142D84940` | a sound component's instance init (`0x141DB61A0` at `+0x3B`) *(added 2026-11-18)* | `48 89 5C 24 18 57 48 83 EC 20 48 8B F9 E8` |
| `0x143CF65E0` | `DSTalkComponent` vtable (`+0x68` = `PlaySentence` `0x1422D3FE0`; Phase 2 checks the slot) *(added 2026-11-18)* | - |
| `0x1422D5A40` | `DSTalkComponent` `MsgStartTalk` handler (thunk `0x1422D6B20`, table entry `0x144AE8558`; what `PlaySentence` reaches) *(added 2026-11-18)* | - |
| `0x1422D6230` | send a talk message to the component's owner (the game-actor manager `[0x147BBB8C0]` when the component has an actor id, else `0x14235C990`) *(added 2026-11-18)* | - |
| `0x14361A6C0` | subtitles: builds `MsgShowSubtitle` (vtable `0x143F15258`, lea at `0x14361A701`) from `(ui, LocalizedTextResource* text, speaker name text, u8[16]*, u32, u32)` and hands it to the HUD (`[ui+0x610]`); a vtable method (slot `0x143F15240`). Hooked by Phase 2 *(added 2026-11-18)* | `48 83 EC 68 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 58 48 83 B9 10 06 00 00 00 4C 8B D1 0F 84` |
| `0x1430C09F0` | `DSUISubtitlesHUDLogic` `MsgShowSubtitle` handler (thunk `0x1430D4A30`) *(added 2026-11-18)* | `40 55 56 57 41 55 48 8D 6C 24 B8 48 81 EC 48 01 00 00` |
| `0x143A1D5B0` | export `ResetRTPCValue(K, _K, J, AkCurveInterpolation, _N)` (Phase 3 resets a rebound object's RTPCs) *(added 2026-11-18)* | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83` |
| `0x143A1EEF0` | export `SetGameObjectOutputBusVolume(_K emitter, _K listener, float)` (clamps to 0..16, queues message `0x14` with both ids; returns 1). Used by the self_voice silencing and `partner_volume` *(added 2026-11-18)* | `48 89 5C 24 08 57 48 83 EC 30 0F 29 74 24 20 48 8B DA 0F 57 F6 48 8B F9` |
| `0x143A20820` | export `StopAll(_K)` (queues message `0x1B` with the object) *(added 2026-11-18)* | `40 53 48 83 EC 20 48 8B D9 E8` |
| `0x14235A943` / `0x14235A7CD` | in `SoundComponent` play 1: `call 0x141DB61A0` (create) / `call 0x141DCBC30` (attach); Phase 3 checks them *(added 2026-11-18)* | `E8` |
| `0x14235AC9E` / `0x14234966F` | in play 2: `call 0x141DCBC30`; in play 3: `call 0x141DB61A0` *(added 2026-11-18)* | `E8` |
| `0x141AB3D8B` / `0x141AB11D8` | in the emitter start: `call 0x141DB61A0`; its only caller (`0x141AB1190`): `call 0x141AB3CA0` *(added 2026-11-18)* | `E8` |

Message vtables: `MsgStartTalk` `0x143D02660` (RTTI `0x144AFEB70`, 120 bytes) *(added 2026-11-18)*, `MsgShowSubtitle`
`0x143F15258` (RTTI `0x144EA99F0`) *(added 2026-11-18)*, `MsgDSStartTalk` `0x143CF3A50`, `MsgDSVoiceTriggerEvent` `0x143DEBB80` (RTTI
`0x144C63A10`), `MsgAnimationSoundEvent` RTTI `0x144867BD0`. Component RTTI: `DSTalkComponent`
`0x144AE8400`, `TalkComponent` `0x144AFF170`, `VoiceComponent` `0x144AFE820`, `SoundComponent`
`0x144AFDAC0`, `DSCustomSoundComponent` `0x144C5AC40`, `DSEchoComponent` `0x144C23EC0`,
`WwiseGameObject` `0x144A724C0`, `SoundManager` `0x144A6F280`. Resources *(added 2026-11-18)*: `SentenceResource`
`0x144ADC440` (`ShowSubtitles` `+0x28`, `Text` `+0x40`), `DSSituationSentenceGroupResource` `0x144BE0370` (the
voice manager's `+0x28`, that is `DSPlayerEntity + 0x4528`), `DSSentenceSituationResource` `0x144BE23B0`,
`DSPlayerSentenceResource` `0x144BE05B0`.

## 11. Open questions

*(changed 2026-11-18: answers from Phase 0, section 12.)*

1. ~~Does the graph's `MsgDSVoiceTriggerEvent` handler call `PlayPlayerVoice`?~~ **Yes [verified, 12.1].**
2. ~~The exact `MsgDSStartTalk` entry layout, and whether Sam has a `DSTalkComponent`.~~ **Answered [verified,
   12.2]**: `{SentenceResource*, SoundResource* (null), f32 delay 0.0, f32 0.1}`; Sam has his own.
3. *(changed 2026-11-18)* Phase 3 ships with the position rule for the emitters (section 16.4); the exact link is
   still open. The `SoundInstance` → component → owner link: **answered for component-played sounds [verified, 12.5]**
   (the instance itself is the Wwise game object; the owner is `[[rep+0x20]+0x48]` of the play path's
   `this`). **Still open** for the material / physics emitters (`0x141AB3CA0`) and the graph runtime's
   instance pools (`0x14302C970`).
4. ~~Whether `PlaySentence` on the echo entity plays with 3D position at the echo and shows subtitles.~~ **Yes to
   both [verified, section 15]**: it returns 1, the voice instance is posted at the echo entity (0.00-0.13 m from it),
   and a spoken line asks for its subtitle (dropped by default, `voice_subtitles`).
5. ~~How footstep surface switches are set.~~ **No switches at all [verified, 12.4]**: RTPCs set on the game
   object right before each post (mirrorable), plus global states.
6. *(added 2026-11-18, section 14.4)* **BB's answer to a dialogue situation follows Sam's line**: with Sam's
   `ReceivedLike` talk muted, BB's reaction (16 talks in the Sam control) did not come. Whether to keep BB's part
   for dialogue situations (and how, without Sam's subtitles) is open. *(changed 2026-11-18)* **Not caused by the mute
   [verified, section 17.2]**: with the guest-Fragile self view and *no* mute (self_voice=sam, run
   `20261118-090911-bb-answer-selfview`) Sam's ReceivedLike line played and BB still did not answer within 15 s.

## 12. Phase 0 results (hidden runs, 2026-11-18)

`native/mod/audio_probe.cpp`, `session.ini audio_probe=1` (docs/debug-actions.md "Audio probe"); scenarios
`audio-probe` (Sam) and `audio-probe-fragile` (the `fragile_suit` self view), both with `-SeedSaves complete-100`.
The hidden game is silent, so everything below is read from the `[audio]` log lines.

| Run | Scenario | Probe build | Result |
|---|---|---|---|
| `20261118-054042-audio-probe` | Sam | 11 hooks (Wwise API, enqueue, talk handler, PlaySentence, animation sound events) | pass, exit 0, saves unchanged, 0 dropped records |
| `20261118-054745-audio-probe-fragile` | Fragile self view | + instance create / attach, SoundComponent play paths, ExecuteActionOnEvent (18 hooks) | pass, exit 0, saves unchanged, 0 dropped |
| `20261118-055223-audio-probe` | Sam | + the player-update flag, registrations tagged with the playing component, component representations resolved (19 hooks) | pass, exit 0, saves unchanged, 0 dropped |
| `20261118-055718-audio-probe` | Sam | + the material emitter start `0x141AB3CA0` as a context, posts within 3 m of the player logged (20 hooks) | pass (all steps), saves unchanged, 0 dropped; **the game exited with `0xC0000005` after `quit`** (no exit trace, no error.txt; see 12.7) |
| `20261118-060035-audio-probe` | Sam | the same, the emitter context kept only when no component context is set | pass, exit 0, saves unchanged, 0 dropped |

### 12.1 Sam's voice: one path, and the non-verbal voice is in it [verified]

- **Sam has his own `DSTalkComponent`** (`[audio] player component deriving from DSTalkComponent: ... DSTalkComponent`,
  all five runs; 47 components on `DSPlayerEntity`, with `VoiceComponent`, `DSCustomSoundComponent`, `NodeGraphComponent`).
- **Path B ends in path C.** Every voice-range animation event (2500, 2501, 2502, 2509, 2510, 2512, 2520, 2529) that
  is not throttled is followed, on the same job thread and in the same millisecond, by an enqueue whose stack is
  `0x1426A6386` (the locked enqueue) ← `0x1426E991D` (inside `PlayPlayerVoice` `0x1426E98A0`) ← `0x14190AA2F`
  (the script-export call [inferred]) ← graph runtime [inferred] (`0x141BA1790`, `0x141BA5351`, `0x141BA2470`) ← `0x142459A19`
  (a message handler `0x142459A00`, [inferred] `NodeGraphComponent`'s) ← the message dispatch (`0x14237C50D`, `0x142376490`,
  `0x14235CA9D`). So `sd_state_machine_setup_sam`'s `MsgDSVoiceTriggerEvent` entry calls `PlayPlayerVoice` by
  situation name. Index → situation hash, from both runs (number of pairs):

  | Animation event | Situation hash | Name | Pairs |
  |---|---|---|---|
  | 2500 | `0x35FEDCC7` | ? | 4 |
  | 2501 | `0x341E4861` | ? (the most frequent: walking and idle efforts [inferred]) | 23 |
  | 2502 | `0x39C69516` | ? | 2 |
  | 2509 | `0x21A05718` | ? | 6 |
  | 2510 | `0x1EB43780` | ? | 2 |
  | 2512 | `0x36B74D25` | `OutOfBreath` | 18 |
  | 2520 | `0x3F8A5E65` | ? | 5 |
  | 2529 | `0x2CF2EAD9` | `Jumping` | 6 |

- **Then the voice update plays it as `MsgDSStartTalk`** to the player's own `DSTalkComponent`: every local-player
  talk came from the voice manager update (stack `0x1426C39CC` in `0x1426C2040` ← `0x1426EB0F7` ← `0x14273131D` in
  the player update `0x142730B30`). Run 054042: 33 talk requests to the local player, **30 non-verbal sentences and
  3 spoken** (classified by the sentence GUIDs against the decoded `lines_sam` sentences: `Text` null or not); run
  054745: 33, all non-verbal (`ReceivedLike` was refused by the queue that time; the other queued test situations
  picked non-verbal sentences). So muting the talk
  handler for the local player mutes the grunts, efforts, breathing and humming as well as the spoken lines.
- **`DSTalkComponent::PlaySentence` (`0x1422D3FE0`) was never called** in any run (0 calls): the player's voice does
  not use it. Phase 2's receiver would be its first runtime use (section 13).
- **Threads.** The talk handler, the enqueue and the animation sound events run on the game's job threads (14
  different thread ids in run 054042, never the main thread `65088`), inside the player update. The voice manager's
  critical section (`+0x1C8`) is held around the enqueue.
- **A continuous request** `0x1A996AA5` (name unknown; `b8 1`, both floats 20.0, through `0x1426A63C0`, stack
  `0x1426A6475` ← `0x141AFCCC1` ← `0x1427559EA` ← `0x1427AD045` ← `0x142755821` ← `0x14274C06C` ← `0x1427327C8`) is
  enqueued about 85 times a second (12,730 accepted + 729 refused in run 054042). It is merged with its queued
  entry; some of the talk requests without a matching enqueue line come from it.
- **Native and test requests.** The cargo menu (cuff map) enqueued `0x35A8223C` natively (`0x1426A6386` ←
  `0x14279470D` ← `0x1428AA852`), followed by a talk. The test situations (`audio_probe 2 n`, our call on the main
  thread) went through the same queue: `ReceivedLike`, `Jumping`, `Humming`, `HoldBaggageNormal` queued and were
  talked at once (`talk #21` 16 ms after `ReceivedLike`); `OutOfBreath`, `AlmostThere`, `Cold` were refused by the
  enqueue (priority / repeat rules, 3.2 A).
- **The cargo menu's Sam (`DSPlayerAKukanEntity`) did not talk** in either run (no talk request with that owner): the
  `PlayAkukanVoice` path is still unobserved at runtime. *(changed 2026-11-18)* Observed in Phase 1 by asking the
  figure through that export (section 14.3): its own `DSTalkComponent` gets the `MsgDSStartTalk`.
- **Other talkers on the player's family.** 16 talk requests (run 054042) went to a `DSTalkComponent` on an
  `Entity` that is a child of the player (sentences outside `lines_sam`; event `0x8118407C`): BB in the pod, reacting
  after `ReceivedLike`; one more to another child entity after the gun section. They are not the player's voice: the
  Phase 1 filter must be the exact player entity, not the family.
- **Situation hash.** `0x14190D040` masks the CRC32-C to 31 bits (`& 0x7FFFFFFF`) and caches it in the string
  header (`-0xC`) [verified, decompile]; all 423 hashes in the data are below `0x80000000`, consistent with it.

### 12.2 `MsgDSStartTalk` as the handler sees it [verified, 83 messages]

| Offset | Seen | Meaning |
|---|---|---|
| `+0x00` | vtable `0x143CF3A50` | `MsgDSStartTalk` |
| `+0x10` | f32 1.5 (`0x3FC00000`) for Sam, 1.2 (`0x3F99999A`) once for a child talker | the float the voice update sets (section 3.2 C said 4.3-4.6: that was another branch) |
| `+0x14` | u32 0, sometimes 2 | flags [inferred] |
| `+0x18` | a heap pointer (Sam), a pointer outside ds.exe for the child | helper / joint name (`"JNT_C_B_008_Head"`) [inferred] |
| `+0x28` | 0 | |
| `+0x30` | i32 1 in every message | entry count |
| `+0x38` | pointer | entries, 0x18 bytes each |
| `+0x40` | u8 0 | 0 = start (the handler takes another branch when it is set) |

Entry: `{+0x00 SentenceResource* (GUID at +0x08 matches the decoded sentence files), +0x08 SoundResource* = null in
all 83, +0x10 f32 0.0 (delay), +0x14 f32 0.1}`. With the sound null the handler creates no instance itself; the
sentence's own `LocalizedSimpleSoundResource` plays it.

### 12.3 What the voice becomes in Wwise [verified]

Each talk is followed within ~1 ms by one external-source `PostEvent` (`cExternals = 1`) on a
`WwiseSimpleSoundInstance`, from a job thread (stack `0x141DD21D0` ← `0x1422A98F1` ← sound-manager frames
`0x141DBE9F7`, `0x141DBE772`, `0x141DC88D1`, `0x141DBD258`). Sam's voice used the preset events `0x3D4BF9E2`,
`0x1C5DF301`, `0x3E51AF82`, `0xA4D58CE8`, `0x7B540EFF`, `0xB4FECB5F`; BB used `0x8118407C`. These objects belong to
"`DSTalkComponent` of the player" (owner found from the component's own array: `+0x40 -> +0x90`). As section 8
says, voice must never be mirrored at the Wwise level (the WEM id depends on the language).

### 12.4 The Wwise side [verified]

- **One sound thread posts.** In run 054042, 966 of 1,020 `PostEvent` calls and 9,987 of 11,368 `SetRTPCValue`
  calls were on one thread (`95652`, not the main thread); all posts come from `0x141DD2040` (return `0x141DD21D0`).
  `RegisterGameObj`, `UnregisterGameObj` and `SetPosition` (251,304 calls, ~1,500 a second) run on the job threads
  and the main thread. The Phase 3 sender hook on `PostEvent` therefore runs on the sound thread.
- **`SetSwitch` is never called** (0 calls in every run; its only references are the by-name `SetSwitch` wrappers,
  which have no callers). Decima drives its sounds with **RTPCs set on the game object right before the post** (for
  example `0x3BA60DF6`: 7 RTPCs on every one of its 54 posts) and global `SetState`. `PostTrigger` is not used.
  `ExecuteActionOnEvent` (by id) is called by `0x141DBDB10`, `0x141DBDF00`, `0x141DD2990` (1 call in run 054745).
  `StopPlayingID` stops known playing ids (128 of 136).
- **Game objects are sound instances**: `GraphSoundInstance` (graph sounds) and `WwiseSimpleSoundInstance` (voice),
  at the Decima addresses (section 2.1). They are pooled: `0x141DB61A0` created ~1,750 instances during the load
  (most from `0x14302C970`, the graph runtime's pool allocator, 1,394 in run 054745), while `RegisterGameObj` /
  `UnregisterGameObj` happen each time one starts or stops playing (~8 a second).
- **No object posted on game object 0 or `~0`** after the probe was installed.

### 12.5 Which game objects are the local player's (6.2)

- **The instance is the Wwise game object, and nothing in it points back at its owner.** The forward scan of
  the first 0x2C0 bytes of each posting instance found the player family only for a few cargo instances
  (`+0x210` = a `DSBaggage2Entity`); positions are copied into the instance, not referenced.
- **Exact, by component [verified, runs 055223 / 055718]:** a sound a component plays goes through one of the
  SoundComponent play paths (`0x14235A460`, `0x14235AAC0`, `0x1423493E0`). Their `this` is the component's
  *representation* (`SoundComponentRep`, `DSCustomSoundComponentRep`), whose `+0x20` is the component, whose
  `+0x48` is the owner entity. The instance create `0x141DB61A0` / attach `0x141DCBC30` and the
  `RegisterGameObj` (263 and 375 registrations) happen synchronously inside those calls, so a thread-local
  "current component" set by detours on the three play paths names the owner of every instance they start.
  In run 055223 that tied 90 posting objects to the player: `DSCustomSoundComponent` of the player (105 objects,
  357 posts: the animation foley, `0xC5DEF120`, `0xAD954B69`, `0xEBC2378E`, `0x6D69D583` ...), the weapon's
  `SoundComponent` (`DSWeaponEntity`, `0x6D69D583`, `0xC1CFA276`, `0xEE160B69` when firing), the camera's, the
  Odradek's, gear `Entity`s; and named the other owners (vehicles, `DSSharedSubstanceTriggerAnimation`, NPC
  `DSControlledEntity`). Instances are pooled and reused by other components, so ownership must be decided
  per registration, not per instance address.
- **The player update is not a usable context:** a thread-local flag set in `0x142730B30` (this = the local
  player) never saw a registration, create or attach (0 of 151 player objects in run 055223): the animation
  event only *queues* the sound; the play path runs later in the component representation's own job.
- **Not exact: the material / physics effect emitters (footsteps)** [verified by timing and position, run
  055718]. Instances created by `0x141AB3CA0` (an effect object that also drives particles and decals, started
  by `0x141AB1190` ← `0x141AC3870`) carry no owner, and the emitter start is not inside the player's
  `OnAnimationSoundEvent` (run `20261118-060035`: all 127 emitter instances had no outer component context). But 37 of the 43 footstep animation
  events (3100-3199; 39 of 47 in run 060035) were followed within 60 ms by a post on such an instance within 0.2-0.7 m of Sam's root at
  foot height (z +0.05 m): `0x0B509EF4`, `0x3C13FF22`, `0x0C053F73`, `0x5BE9FB68`, `0x3D40C67B` (footsteps), and
  in pairs `0x04A0FA03` + `0x3BA60DF6` (+ `0x3CA60F68`), z 0.1-0.3 m, with 5-7 RTPCs each (movement / load
  layer). Every one of the 236 posts within 3 m of Sam that was not tied by component was either such an
  emitter instance (205) or Sam's voice (31, `WwiseSimpleSoundInstance` made by `0x14246C360`).
- **Not the player's:** the graph runtime's pools (`0x14302C970`, 1,394 instances, e.g. `0xCB7C6725` 93 posts)
  and `0x14298B9F0`: none of their posts was within 3 m of Sam (world and ambient sounds).

### 12.6 Event deny-list (first cut) [inferred from owners]

Decide by owner, not by event id (ids are shared: `0x6D69D583` is posted by the player's
`DSCustomSoundComponent` and by the weapon):
- **voice**: any post with external sources (`cExternals > 0`), and the `DSTalkComponent` / `VoiceComponent`
  instances (Sam `0x3D4BF9E2`, `0x1C5DF301`, `0x3E51AF82`, `0xA4D58CE8`, `0x7B540EFF`, `0xB4FECB5F`; BB
  `0x8118407C`; a child's `VoiceComponent` `0xE5BEA679`): voice goes by sentence (Phase 2);
- **camera**: `SoundComponent` of the `CameraEntity` child (`0xE9C1B3E7`, `0xE9C1B3E4`, `0x39FE036A`,
  `0x43582143`, `0xCB8293CB`, `0x0623267B`): listener-side cues;
- **Odradek** (`SoundComponent` of `DSOdradekBodyEntity` / `DSOdradekArmEntity`): instances only, no post seen;
  keep denied until a scan is probed (its sounds are the local player's scanner feedback);
- **global**: game objects 0 / `~0` (none seen), `SetState`, mix states.
Everything else the player owns (animation foley, weapons, gear, cargo, the material footsteps) is mirrored. *(changed
2026-11-18: replaced by an allow-list, section 17.4.)*

### 12.7 Safety of the probe

- Every hook's prologue and the call sites / table entries that tie it to its meaning are byte-checked
  before `MH_CreateHook`; the hooks are queued and applied at once (`MH_ApplyQueued`) at start-up, before the
  frame pump. All 11 → 20 hooks installed in every run (`[audio] probe installed (logging only): N/N hooks`).
- Hot paths copy a few words into a 65,536-record lock-free ring (seqlock per slot; the voice-path hooks use a
  1,024-record ring with stack words); the game thread drains it. 0 records dropped in all five runs. Game
  memory outside our own stack is read only through `ReadProcessMemory`.
- Run `20261118-055718` ended with `0xC0000005` *after* `quit` (all steps passed, saves unchanged, no
  `error.txt`, no exit trace). The next run with the same hooks (`20261118-060035`) and all earlier ones exited
  0, and one non-audio run had the same exit code before (`20261116-184029-real-body-tour`). Whether the probe
  contributes is **not known**: keep `audio_probe` a hidden-test key, and give Phase 1-3's hooks (fewer, and on
  the same functions) a few repeated hidden runs before shipping.

## 13. Go / no-go for Phases 1-3 (after Phase 0)

**Phase 1 (mute Sam's voice for a Fragile guest): GO.** *(changed 2026-11-18)* Built and verified as below:
section 14; since the review the talk runs and only its voice post is silenced (section 17.2).
- Hook `0x1422D4F70` (bytes in section 10; check the thunk `0x1422D6AA0` = `E9` to it and the handler-table
  entry `0x144AE8568` = the thunk, as the probe does). Detour `void(comp, msg)`.
- Drop the call (do not run the original) when `self_voice` resolves to mute, the owner `[comp+0x48]` (fault-free
  read) equals the local player entity, and `msg+0x40 == 0`. Everything else (BB, the cuff device, NPCs, the
  partner's echo) passes through unchanged.
- The local player pointer comes from the game thread (an atomic the tick sets, as the probe's `g_local_e`):
  the handler runs on job threads, so the detour must take no lock, allocate nothing and log nothing; count
  the drops in an atomic and log them once a second from the tick.
- The cargo menu figure (`DSPlayerAKukanEntity`, vtable `0x143D7DE48`, already checked by `menu_figure.cpp`):
  drop its talks too while the menu shows Fragile. Not observed at runtime yet (no akukan talk in Phase 0):
  the Phase 1 run must provoke one (change the cargo in Manage Cargo) or report it as untested.
- Verification (hidden): `audio-probe-fragile` with `self_voice=mute`: talk requests to the local player
  counted as dropped; no external-source `PostEvent` on "DSTalkComponent of the player"; the animation and
  material posts (`0xC5DEF120`, `0xAD954B69`, `0x0C053F73` ...) unchanged against run `20261118-054745`.

**Phase 2 (partner voice): GO, with one runtime unknown.** *(changed 2026-11-18)* Built and verified: section 15
(the unknown is answered: `PlaySentence` returns 1 and plays at the entity; the fallback was not needed).
- Sender: the same talk hook (owner == local player), before the mute decision: per entry the sentence GUID
  (`[entry+0x00]+0x08`, 16 bytes, fault-free), the delay `entry+0x10`; the float `msg+0x10`. Copy into a small
  lock-free ring (job thread); the tick sends `STS1` (kind 0; a muted Fragile guest sends kind 1 or nothing).
  At most a few a second (33 talks in ~140 s of play).
- Receiver: GUID → `SentenceResource*` from the resident group (section 6.1), played on the partner voice entity
  (`partner_voice=1`, `DSTalkComponent`) with `DSTalkComponent::PlaySentence` (`0x1422D3FE0`, vt `+0x68`). The game
  itself never called `PlaySentence` in Phase 0, so its runtime behaviour (return value, 3D position,
  subtitles) is the first thing the Phase 2 run must check; the fallback is sending a `MsgDSStartTalk` built
  with the layout in 12.2 to that entity (`0x14235C990`).
- Verification: two-game hidden run, probe on both games: `STS1` sent / received / resolved, `PlaySentence`
  returns 1, then one external-source post on a `WwiseSimpleSoundInstance` of the voice entity's
  `DSTalkComponent`.

**Phase 3 (partner footsteps, foley, weapons): GO for the component-played sounds; footsteps only with a position rule (no exact owner yet): GO for a first version, flagged experimental.** *(changed 2026-11-18)* Built and verified as below: section 16.

- Hook points (all byte-checked by the probe already): `PostEvent` (sender, sound thread), `RegisterGameObj` /
  `UnregisterGameObj` (ownership set), `SetRTPCValue` (the values to replay), `StopPlayingID` and
  `ExecuteActionOnEvent` (stops), `SetPosition` is not needed on the sender.
- Ownership, exact: a thread-local "current component" set by detours on the SoundComponent play paths
  `0x14235A460`, `0x14235AAC0`, `0x1423493E0` (`this` = the component's representation; component =
  `[this+0x20]`, owner entity = `[component+0x48]`); a `RegisterGameObj` or instance create (`0x141DB61A0`) /
  attach (`0x141DCBC30`) inside them puts the game object into a lock-free owned set when the owner is the
  local player or one of his child entities (gear, weapons). That covered 263 registrations and the
  `DSCustomSoundComponent` animation foley, the weapon's `SoundComponent` and the gear in run
  `20261118-055223`.
- Footsteps and the movement layer (the material effect emitters, 12.5): no owner link was found. Rule for a
  first version: an instance created by `0x141AB3CA0` whose post lies within 1.0 m of the local player's
  root (or 0.5 m of a foot joint) is the player's; in Phase 0 that matched 37 of 43 footstep events and
  nothing else was that close. Risk: another walker within a metre (a porter, a MULE, the partner's own
  body if it ever gets real footsteps). The exact fix is the effect request: find where the player mover's
  footstep message (`0x1427904D0`) allocates the effect slot that `0x141AC3870` later starts, and tag that slot.
- Emitter slot and offset: the sender reads the instance position (`+0x20`, doubles, fault-free) at post
  time and sends it relative to the local player's root; the receiver posts on its own game object placed at
  that offset from the partner body (or on the nearest of the five joint objects of section 6.1).
- RTPCs: replay the RTPC ids and values set on the object since its registration (12.4); no switches.
- Deny-list: section 12.6.

## 14. Phase 1 results: Sam's voice muted for a player who is not Sam (hidden runs, 2026-11-18)

### 14.1 What was built

- **Setting.** `session.ini self_voice=auto|sam|mute` (`native/mod/self_voice_setting.hpp`, pure, unit-tested in
  `st_net_tests` `TestSelfVoiceSetting` and `st_launcher_core_tests`). `auto` mutes when this player's character
  (`bodies::ChooseOwn(role == host, my_character)`: the host is always Sam, an unknown key is Sam) is not Sam
  (`sam`, `sam_*`); `sam` never; `mute` always. **A missing or malformed key is `sam`: nothing is hooked**, so play
  is unchanged with the key off. The launcher writes `self_voice=auto` (Debug > "Mute Sam's voice when you are not
  Sam", default on) or `self_voice=sam` (unticked), for every role.
- **Hook.** `native/mod/self_voice.cpp` on the `MsgDSStartTalk` handler `0x1422D4F70`, installed at start-up (after
  the probe, before the frame pump) only when the setting resolves to mute. Before hooking: the thunk `0x1422D6AA0`
  is `E9` to it (its only reference), the handler-table entry `0x144AE8568` holds the thunk, and the handler's first
  20 bytes match. With `audio_probe=1` the probe already owns that hook (one MinHook hook per target): its detour
  calls `selfvoice::FilterTalk` first and skips the original when it says drop.
- **Filter** (`FilterTalk`, job threads): armed and muting; owner `[comp+0x48]` (ReadProcessMemory) equal to the
  local player entity or to the cargo menu figure the tick published; `msg+0x40 == 0` (read the same way). Then
  the original is not run. Nothing else is touched: other owners return at once after one read. Two atomics in,
  counters out, no lock, no allocation, no logging; the game thread logs `[voice] counts` once a second when they
  changed.
- **Menu figure.** `realbody::MenuSelfFigure()` returns the `DSPlayerAKukanEntity` while the self body stands in
  for it (Manage Cargo, kind 0, the self view on), from the opening frames; with the self view off the figure is
  Sam and its voice is not muted.
- **Debug action** `self_voice` (debug-actions.md "Self voice"): 0 counts, 1 `<0|1>` mute off / on at run time
  (only when armed: the A/B inside one run), 2 `<list>` the figure says a line through the game's own
  `PlayAkukanVoice` (byte-checked, the list read fault-free first).

### 14.2 Runs (`build-testenv\runs\`, all `-SeedSaves complete-100`; no `ds.exe` was running before any of them)

| Run | Scenario | Result |
|---|---|---|
| `20261118-061946-self-voice-fragile` | probe + mute | fail (by design of the check): `[voice] ... NOT muted: the handler's bytes differ` (the probe's hook had replaced them); the `nolog` step caught the unmuted talks. Fixed: with the probe's hook in, its own checks stand |
| `20261118-062233-self-voice-fragile` | probe + mute | muting worked (26 muted); `self_voice 2` refused: the akukan check expected `E8` at `0x142757D63`, it is a tail `jmp` (fixed) |
| `20261118-062702-self-voice-fragile` | probe + mute | everything passed except the optional `self_voice 2 2` steps (the figure has 2 lists; removed) |
| `20261118-063122-self-voice-sam` | Sam control | the menu `until` found no `[menu] subspace state` lines: they need the pose hook (`partner_body=real` added) |
| `20261118-063431-self-voice-sam` | Sam control | **pass**, exit 0 |
| `20261118-063756-self-voice-fragile` | probe + mute | **pass**, exit 0 |
| `20261118-064338-self-voice-own-hook` | own hook, no probe | **pass**, exit 0 |
| `20261118-064727-self-voice-fragile` | probe + mute (final build) | **pass**, exit 0 |
| `20261118-065125-self-voice-sam` | Sam control (final build) | **pass**, exit 0 |
| `20261118-065445-self-voice-own-hook` | own hook, no probe (final build) | **pass**, exit 0 |

Every run: saves unchanged (hash manifest identical), game folder restored, no `error.txt`, game exit code 0 (the
`0xC0000005` after `quit` of run `20261118-055718` did not recur in these 10 runs), 0 dropped probe records.

### 14.3 Muted and played [verified]

`[voice] status` at the end of each run (talks addressed to the local player / to the menu figure; "passed" =
while the debug toggle had the mute off):

| Run | Local player: muted | passed | Menu figure: muted | passed | Talks of other owners |
|---|---|---|---|---|---|
| `063756-self-voice-fragile` | 44 | 6 | 2 | 2 | 1 (a child `Entity`, untouched) |
| `064727-self-voice-fragile` | 27 | 2 | 2 | 2 | 1 |
| `064338-self-voice-own-hook` | 22 | 7 | 2 | 2 | 1 |
| `065445-self-voice-own-hook` | 31 | 10 | 2 | 2 | 1 |
| `063431-self-voice-sam` | not armed (`[voice] self_voice=auto, character 'sam': ... nothing hooked`) | 38 played | | 2 played | 17 (BB) |
| `065125-self-voice-sam` | not armed | 24 played | | 2 played | 17 (BB) |

- **Nothing of Sam's voice reached Wwise while muted.** The probe logs every external-source `PostEvent` ("voice
  post"). In run `063756` Sam's preset events (`0x3D4BF9E2`, `0x1C5DF301`, `0x3E51AF82`, `0xA4D58CE8`, `0x7B540EFF`,
  `0xB4FECB5F`) were posted only inside the mute-off window (`log audio: mute off` at 160.5 s to `mute on` at
  196.8 s): 6 posts (164.6 `0xA4D58CE8`, 170.8 `0x3E51AF82`, 171.5 / 172.9 / 194.8 / 196.3 `0x1C5DF301`) for the 6
  passed talks; none in the 155 s of muted play. Run `064727`: 2 posts, both in the window, for 2 passed talks. The Sam
  controls posted them for every talk (38 posts for 38 talks in `063431`, 24 for 24 in `065125`).
- Every muted talk is logged `[audio] talk #N MUTED comp ... owner ... (the local player)` with the same message as
  in Phase 0 (`+0x10` 1.5, one entry, `+0x40` 0); the `nolog` check (no unmuted local talk before the mute-off
  section) passed in every Fragile run after the first. The muted talks cover both paths: the animation voice
  events (59 in the voice range 2500-2599 in run `063756`) and the queued test situations (`ReceivedLike`,
  `Jumping`, `OutOfBreath`, `Humming`, `AlmostThere`, `HoldBaggageNormal` were accepted by the queue in that run).
- **The cargo menu figure** [verified]: `self_voice 2 0` and `2 1` reached its own `DSTalkComponent` (`talk #42 MUTED
  comp 0x41E63AD5780 (DSTalkComponent) owner 0x41E63A7A000`, the figure, run `062702`). Muted with the menu showing
  Fragile: no voice post. With the mute off (second menu) and in the Sam controls: `0x537EDC0A` and `0x83FBDDAA`
  posted, one per talk.
- **Other voices kept**: the external-source posts that do not come through a talk component (`0x374FF1A2` x4 and
  `0x518FE01D`, 35-45 s into every run) and the child talker's `0xD44C68CF` (about 122 s) were posted in every
  run, muted or not.
- The talk handler's other branch (`msg+0x40 != 0`) was never seen for the local player or the figure (0 in every run).

### 14.4 Footsteps, foley and weapons unchanged [verified]

Posts per event (the probe's dump) in the Sam controls, the muted Fragile runs and the Phase 0 Fragile run without the
mute (`054745`). The Fragile runs play about 36 s longer (the mute-off section and the second menu):

| Event | Sam `063431` | Sam `065125` | Fragile muted `063756` | Fragile muted `064727` | Fragile, no mute `054745` |
|---|---|---|---|---|---|
| `0xC5DEF120` (animation foley) | 93 | 105 | 103 | 101 | 91 |
| `0xAD954B69` (animation foley) | 85 | 96 | 93 | 91 | 82 |
| `0xEBC2378E` (animation foley) | 48 | 47 | 52 | 48 | 47 |
| `0x6D69D583` (foley / weapon) | 11 | 7 | 13 | 8 | 12 |
| `0x0C053F73` (footstep) | 7 | 6 | 8 | 5 | 7 |
| `0x0B509EF4` (footstep) | 6 | 9 | 7 | 8 | 6 |
| `0x3C13FF22` (footstep) | 4 | 6 | 5 | 6 | 4 |
| `0x5BE9FB68` (footstep) | 7 | 8 | 8 | 9 | 7 |
| `0x3D40C67B` (footstep) | 7 | 5 | 6 | 4 | 5 |
| `0x04A0FA03` (movement layer) | 85 | 88 | 87 | 89 | 85 |
| `0x3BA60DF6` (movement layer) | 50 | 48 | 47 | 46 | 54 |
| `0xC1CFA276` / `0xEE160B69` (rifle; *(changed 2026-11-18)* aim / lower sounds, not shown to be shots: section 17.5) | 1 / 1 | 1 / 1 | 1 / 1 | 1 / 1 | 1 / 1 |
| animation footstep events 3100-3199 | 50 | 43 | 52 | 43 | 44 |
| all `PostEvent` calls | 1,053 | 1,018 | 1,107 | 1,068 | 955 |

The differences are within the run-to-run spread of the controls (for example `0xC5DEF120` 93 / 105 between the
two Sam runs). The mute takes away only the external-source voice posts.

**One side effect [verified, inferred cause]: BB's answer to a dialogue situation.** In the Sam controls, the
queued `ReceivedLike` talk (Sam's line, `0xA4D58CE8` at 88.1 s in `063431`) was followed 2 s later by 16 talks of
BB's `DSTalkComponent` (`0x8118407C`, 90.1-99.9 s). In the muted Fragile run `063756` the same `ReceivedLike` was
queued (`-> 1`) and its talk muted, and **BB did not answer** (no `0x8118407C` post in the run). `ReceivedLike` is a
dialogue situation (section 3.1: priority 30, `IsDialogue`), so BB's part appears to be chained to Sam's line playing
[inferred: no BB talk without it; the chaining code is not decoded]. Options for later: leave dialogue situations
alone (Sam heard), or keep the talk and silence only Sam's voice instance at the Wwise level (BB answers, but Sam's
subtitle would show). Not changed in Phase 1.

### 14.5 Safety

- One hook (the probe's, or self_voice's own), installed at start-up and byte-checked as above; the akukan call is
  made only from the debug action, on the game thread, SEH-guarded, after its sites and the figure's list were
  checked.
- No faults, no `error.txt`, exit code 0 in all completed runs; saves unchanged in all 10.
- Build `cmake --build build-testenv --config Release` (/W4 /WX) clean; `ctest -C Release`: 13 of 13 passed.

## 15. Phase 2 results: each player hears the other's voice (hidden runs, 2026-11-18)

### 15.1 What was built

- **Settings.** `session.ini voice_sync=0|1` (default 0: nothing is hooked, sent or played for it) and
  `voice_subtitles=0|1` (default 0: the partner's subtitles are dropped). The launcher writes `voice_sync=1` from its
  Debug option "Hear your partner's voice" (default on; unticked writes nothing). `testenv_core.cpp` allows both keys
  in scenarios.
- **Sender** (`self_voice.cpp`, `partner_voice.cpp`). The talk handler hook of Phase 1 (`0x1422D4F70`, the probe's or
  self_voice's own, byte-checked as before) is now installed when `self_voice` resolves to mute **or** `voice_sync=1`.
  `FilterTalk` copies every talk addressed to the local player itself (owner `[comp+0x48]` == the local player,
  `msg+0x40 == 0`; never the cargo menu figure, BB or the cuff device) into a 64-slot multi-producer seqlock ring:
  up to 4 entries' `SentenceResource*` and their 16-byte GUID (`+0x08`, `ReadProcessMemory`), the first entry's
  delay and whether self_voice muted it. No allocation, no lock, no logging on the job threads. The game thread
  (`pvoice::Tick`) drains it and queues one STS1 per talk: **kind 0** with the GUIDs while this player's character
  (`bodies::ChooseOwn`: the host is always Sam) is Sam, otherwise **kind 1 with no GUID**. So a Fragile guest never
  sends Sam's lines, muted or not.
- **Wire** (`pose_wire`, `link.cpp`). STS1 (`real-partner-body.md` section 18) on the link's reliable ordered channel
  2: `link.cpp` now drives `ReliableSender` / `ReliableReceiver` the way the link harness does (due first
  transmissions, resends within the 2,400-byte budget per 20 ms idle tick, RELACKs after 50 ms), resets them on every
  new session, and keeps a 32-frame receive queue. Unit tests: `st_net_tests` `TestSpeechFrames` (round trip of kinds
  0 / 1 / 2, every refusal, unknown flags cleared, the other decoders refuse STS1 and STS1 refuses STN1, and three
  STS1 frames delivered in order, once, through a lost first transmission). Mixed builds still never meet: STS1 is in
  `pose_wire.hpp/.cpp`, which feed the build id's wire hash (HELLO `build_mismatch`).
- **Receiver** (`partner_voice.cpp`, game thread). Frames wait for the next tick, then: a seq already seen is dropped;
  kind 1 is logged and nothing plays; kind 0 / 2: each GUID is looked up in a map built once per group from the local
  player's resident sentence group (`[player + 0x4528]`, RTTI `DSSituationSentenceGroupResource`; 423 situations, 2,702
  references, **1,566 distinct `SentenceResource`s, 720 spoken and 846 non-verbal**, every one RTTI-checked), and the
  first one that resolves is played with `DSTalkComponent::PlaySentence(comp, sentence, 0, nullptr)` (vtable slot
  `+0x68` checked, prologue checked unless the probe hooked it, SEH-guarded; a fault turns the feature off for the
  session). Before playing: the local player exists and is awake, no stopping game event (pause, menus such as the
  cuff map or Manage Cargo, cutscenes; `kStoppingGameEventMask`) and no load bit, a talking entity exists with a
  verified `DSTalkComponent` (exact RTTI, vtable `0x143CF65E0`, owner = the entity), it is within **100 m** of the local
  player, and the sentence is still a `SentenceResource`. Every skip is counted and logged with its reason.
- **The talking entity** (`player_sync.cpp`). The invisible echo entity that `partner_voice=1` already spawned is now
  also spawned for `voice_sync=1`, and every tick it is placed at the partner body's **head joint**
  (`realbody::HeadWorld`; 1.55 m over the root when no head reads, for example the hologram).
- **Subtitles.** With `voice_sync=1` the subtitle request `0x14361A6C0` is hooked (vtable slot, prologue and the
  `MsgShowSubtitle` vtable it builds checked first). Before each `PlaySentence` the line's `Text` pointer goes into an
  8-slot lock-free table for 15 s; a subtitle request with that text is dropped once (`voice_subtitles=0`) or counted
  as kept (`voice_subtitles=1`). Other subtitles pass untouched.
- **Debug action** `voice_sync` (debug-actions.md "Voice sync"): 0 counters, 1 `<n>` play sentence n of the map on the
  talking entity, 2 `<0|1>` test only: send Sam's lines even when not Sam, 3 rebuild the map.
- **Probe additions** (`audio_probe=1` only): `SentenceHooked()`, and every voice post line now gives the voice
  instance's position and its distance from the local player and from the talking entity.

### 15.2 Runs (`build-testenv\runs\`, all `-SeedSaves complete-100`; no `ds.exe` was running before any of them)

| Run | Scenario | Result |
|---|---|---|
| `20261118-072125-voice-sync-echo` | role echo, probe on: our own lines through STS1 encode / decode to the echo partner | all checks fine; the scenario stopped at its own deliberate menu step (a refused line made the step fail; made optional afterwards); exit 0, saves unchanged |
| `20261118-072408-two-games-voice` | two games, host Sam, guest `fragile_suit` + self view, probe on both | **pass**, both exit 0, saves unchanged, 0 uploads |
| `20261118-072904-two-games-voice-own` | the same without the probe (own hooks), guest `voice_subtitles=1` | **pass**, both exit 0, saves unchanged, 0 uploads |
| `20261118-073352-voice-sync-echo` | role echo, the menu step optional | all checks fine; reported fail only because an optional step failed (`?` steps still count); the test-line action now succeeds when the receiver refuses a line, with the reason in the log |
| `20261118-073631-voice-sync-echo` | role echo, **final build** | **pass**, exit 0, saves unchanged |
| `20261118-073826-two-games-voice` | two games with the probe, **final build** | **pass**, both exit 0, saves unchanged, 0 uploads |
| `20261118-074020-two-games-voice-own` | two games without the probe, **final build** | **pass**, both exit 0, saves unchanged, 0 uploads |

### 15.3 Sent, received, resolved, played [verified]

Final `[pvoice] status` lines (talks = `MsgDSStartTalk` to the local player):

| Run, game | Sent | Received | Played (`PlaySentence` = 1) | Refused / unresolved / skipped / faults |
|---|---|---|---|---|
| `072408` host (Sam) | 4 talks, 4 kind 0 | 13: 8 kind 1, 5 kind 0 (the guest's test section) | 5 | 0 / 0 / 0 / 0 |
| `072408` guest (Fragile) | 13: **8 kind 1**, 5 kind 0 only in the test section | 4 kind 0 | 4 | 0 / 0 / 0 / 0 |
| `072904` host (Sam) | 7 talks, 7 kind 0 | 11: 7 kind 1, 4 kind 0 (test) | 5 (4 + 1 local test line) | 0 / 0 / 0 / 0 |
| `072904` guest (Fragile) | 11: **7 kind 1**, 4 kind 0 (test) | 7 kind 0 | 8 (7 + 1 local test line) | 0 / 0 / 0 / 0 |
| `073826` host (Sam), final build | 6 talks, 6 kind 0 | 14: 8 kind 1, 6 kind 0 (test) | 6 | 0 / 0 / 0 / 0 |
| `073826` guest (Fragile), final build | 14: **8 kind 1**, 6 kind 0 (test) | 6 kind 0 | 6 | 0 / 0 / 0 / 0 |
| `074020` host (Sam), final build | 6 talks, 6 kind 0 | 11: 7 kind 1, 4 kind 0 (test) | 5 (4 + 1 local test line) | 0 / 0 / 0 / 0 |
| `074020` guest (Fragile), final build | 11: **7 kind 1**, 4 kind 0 (test) | 6 kind 0 | 7 (6 + 1 local test line) | 0 / 0 / 0 / 0 |

- **One line end to end** (`072408`): host `[pvoice] sent seq 1 kind 0: 1 sentence(s), first F2604D1E-...` (Sam's
  `ReceivedLike` line, asked through the game's queue) -> guest `received seq 1 kind 0 sentence 1/1 F2604D1E-...:
  resolved to SentenceResource ... (spoken)` -> `partner line seq 1 ... -> PlaySentence(comp ..., sentence ...) = 1 |
  talking entity ... 7.53 m from the local player` -> the probe: `PlaySentence #1 ... -> 1 | thread ... (game)` and
  `voice post #5 event 0xA4D58CE8 ... at (-1095.94, 279.94, 354.10), 7.53 m from the local player, 0.00 m from the
  partner voice entity`.
- **Where the voice is** [verified]: every voice post after a partner line was on a `WwiseSimpleSoundInstance` at the
  talking entity: guest 0.00 / 0.01 / 0.12 / 0.12 m (the host was running), host 0.00 / 0.08 / 0.03 / 0.13 / 0.00 m
  (`072408`); final build `073826`: guest 6 posts at 0.00-0.14 m, host 6 posts at 0.00-0.13 m.
  The posts used Sam's own preset events (`0xA4D58CE8`, `0x3E51AF82`, `0x7B540EFF`), in the receiver's own voice
  language (the GUID is language-independent).
- **The talking entity follows the head** [verified]: `[pvoice] talking entity ... at (x, y, z) | partner head (x, y,
  z), 0.000 m apart | the local player d m away` every 5 s on both games; on the guest it moved with the host from
  (-1098.25, 276.77) to (-1088.68, 291.16) and back while he ran (local player 3.7 to 20.8 m away).
- **The Fragile guest sends no Sam line** [verified]: `nolog \[pvoice\] sent seq [0-9]+ kind 0` held on the guest
  up to the test section in all four two-game runs; her talks went out as `sent seq N kind 1: this player's character
  'fragile_suit' is not Sam, so Sam's line (...) was not sent (muted here too)`, and the host logged `received seq N
  kind 1 (... nothing to play)` and played nothing (`nolog \[pvoice\] partner line seq` on the host held).
- **The other direction** (test only, `voice_sync 2 1` on the guest): her muted Sam efforts went out as kind 0 and the
  host played all of them on her body's head (`072408`: 5 of 5; `072904`: 4 of 4; `073826`: 6 of 6; `074020`: 4 of 4).
- **Reliable channel**: 0 resends on the loopback link (nothing was lost); the resend path is unit-tested only.
  Latency [inferred]: the clock offset between the two logs measured on host -> guest lines (6.641 s for seq 1, 3
  and 4) and on guest -> host lines (6.719 s) puts the two one-way trips at 78 ms together, about 40 ms each way
  (one link idle tick, one game tick and the loopback).

### 15.4 Subtitles [verified]

- A spoken partner line asks for its subtitle: the request `0x14361A6C0` came with the sentence's `Text`
  (`LocalizedTextResource`) and the speaker name text, 0-31 ms after `PlaySentence`.
- `voice_subtitles=0`: `subtitle request: text ... (LocalizedTextResource) ... -> DROPPED (a partner line,
  voice_subtitles=0)` (guest `072408` for the host's `ReceivedLike`; host `072904` for a test line). The local
  player's own subtitles kept showing (`-> shown`, echo run, the local `ReceivedLike`).
- `voice_subtitles=1` (guest `072904`): `-> shown`, `1 kept`.
- Non-verbal lines (`ShowSubtitles` 0, no `Text`) never asked for one.

### 15.5 Gating [verified where a run shows it]

- Menus: with the cuff map open (`HandcuffDevice`) a test line was refused: `not played: paused / menu / cutscene
  (HandcuffDevice)` (echo runs `072125`, `073352`, `073631`).
- Distance (100 m), no talking entity, the local player asleep or loading: implemented and logged, not reached in
  these runs (the players stayed within 21 m).

### 15.6 Safety

- No faults, no `error.txt`, exit code 0 in every run (7 runs, 11 games); saves unchanged and the game folder restored
  in all; the `0xC0000005` after `quit` of Phase 0 did not recur.
- Hooks: the talk handler (one, shared with Phase 1) and the subtitle request, both byte-checked at start-up;
  `PlaySentence` is called, not hooked (the probe hooks it for logging only). The job-thread path copies a few
  words into the ring; the subtitle path compares a pointer against 8 slots.
- Feedback: our `PlaySentence` sends `MsgStartTalk` to the echo entity, which the `MsgDSStartTalk` hook never sees
  and whose owner is never the local player: no line was ever sent back (`sent` counts match the local talks).
- Build (/W4 /WX) clean; `ctest -C Release` 13 of 13 passed (run with no hidden run going).

### 15.7 Not done / not verified

- **A Fragile guest is silent to the host** by design: she has no grunts or efforts of her own (section 4); kind 2
  (her own lines, Phase 4) is reserved and never sent.
- BB's reaction to a muted dialogue line (section 14.4) is unchanged; BB's answers are not mirrored (they are BB's,
  on the speaker's game).
- Distance, pause on the receiving game other than the cuff map, cutscenes, the hologram's head fallback, vehicles,
  long sessions, a real Steam link with loss (the resend path) and a link drop mid-line were not covered by a run.
- The subtitle table matches by text pointer for 15 s: if the local Sam says the very same line within 15 s after
  the partner's, its subtitle could be dropped once (both players Sam only; not seen).
- The launcher option was checked through the unit tests and the screenshot render only (the option sits below the
  fold of screen 27), not through a real Steam launch.

## 16. Phase 3 results: each player hears the other's footsteps, foley and weapons (hidden runs, 2026-11-18)

### 16.1 What was built

- **Settings.** `session.ini sound_sync=0|1` (default 0: nothing is hooked, sent or posted for it) and `sound_steps=0|1`
  (default 1: the footstep position rule, 16.4). The launcher writes `sound_sync=1` from its Debug option "Hear your
  partner's footsteps and sounds" (default on; unticked writes nothing). `testenv_core.cpp` allows both keys.
- **Hooks** (`native/mod/partner_sound.cpp`, installed at start-up after the probe). The Wwise exports `PostEvent`,
  `RegisterGameObj`, `UnregisterGameObj`, `SetRTPCValue`, `StopPlayingID` and `ExecuteActionOnEvent` (found by export
  name; RVA and first bytes checked), plus the "who registers this game object" contexts: the three `SoundComponent`
  play paths and the material emitter start (prologues checked, and tied by the create / attach calls inside them and
  the emitter's only caller, section 10). All or nothing; queued and applied at once. With `audio_probe=1` the probe
  owns the same ten targets and forwards each call (`psound::Hot*`); the probe gained a thread-local "the context is
  the emitter" flag and `audioprobe::Hooked(rva)`. The exports it calls but does not hook (`SetPosition`,
  `ResetRTPCValue`, `StopAll`) are byte-checked too.
- **Hot paths** (any thread): a few words into an 8,192-record seqlock ring (kind, ids, object, the posting object's
  position read with `ReadProcessMemory`, a QPC time); posts on our own objects only add an "own" record. No
  allocation, lock or logging. 0 records dropped in every run.
- **Sender** (game thread). The ring is drained in order. A registration made inside a context is resolved once per
  context (cached 5 s): a `...Rep` context gives the component `[ctx+0x20]`, its owner `[comp+0x48]`, and the owner
  chain (`+0x70`, up to 6 levels) must reach the local player. *(changed 2026-11-18: an allow-list now, section 17.4: the
  player's own `DSCustomSoundComponent` and the sound components of a weapon or cargo entity under him; everything else is
  denied.)* Denied: talk / voice / echo components, a `CameraEntity`
  or Odradek entity in the chain; posts with external sources are voice and never mirrored. Category:
  `DSCustomSoundComponent` of the player = foley, a `Weapon` entity in the chain = weapon, `Baggage` / `Cargo` = cargo,
  the emitter = steps, other children = gear. Unregistration (or a context-free registration of the same pooled
  instance) forgets the object. RTPCs set on our objects are kept per object (newest 12). Each post on our object
  (playing id != 0) becomes an STA1 item: the nearest of seven joints of the local player to the object's position and
  the offset from it, the object's tag and RTPCs, the time. Stops of mirrored playing ids follow (re-sent twice); RTPC
  changes on an object posted in the last 3 s follow as RTPC updates (dropped when the object posts again: the post
  carries the full set). Nothing is sent while the local game is paused, in a menu, a cutscene or loading (the partner
  voice's gate). Items ride in the next pose datagram (`link::SendPose*` take an optional STA1).
- **Receiver** (game thread). Items are queued with their time on the pose ring's sender axis
  (`Ring::Unwrapped(sender_ms) - age`) and posted when `Ring::PlayTime` reaches it: bind the tag to a pool object (24,
  ids = addresses inside the DLL; the idle one unused the longest; a rebind resets the RTPCs the previous binding set),
  `SetPosition` at the partner body's joint (`realbody::JointWorld`, through the body's joint map) plus the offset,
  replay the RTPCs, `PostEvent(ev, obj, AK_EndOfEvent, callback, slot)`. Every tick, bound objects with a sound follow
  their joint. The callback (Wwise's thread) reads `AkEventCallbackInfo` fault-free into a ring; the game thread checks
  its object against the slot. *(changed 2026-11-18: only allow-listed event ids are posted; stops and actions are never dropped
  for timing; nothing plays over 15 minutes; section 17.4.)* Not posted: gate closed (our playing sounds are `StopAll`ed when it closes; *(changed
  2026-11-18)* `stop-all 0` in every two-game run, so this never ran with partner sounds playing), no partner
  body (also stops them), partner beyond 100 m, more than 1.5 s late, over 16 posts a second per slot (burst 12).
- **Wire**: STA1 (`pose_wire`, `real-partner-body.md` section 19, unit test `st_net_tests` `TestSoundFrames`); part of
  the build id's wire hash, so mixed builds are refused at HELLO.
- **Debug action** `sound_sync` (debug-actions.md "Sound sync"): 0 counters and owner labels, 1 `<0|1>` log every
  mirrored post, 2 stop the partner's sounds.

### 16.2 Runs (`build-testenv\runs\`, all `-SeedSaves complete-100`; no `ds.exe` was running before any of them)

| Run | Scenario | Result |
|---|---|---|
| `20261118-080555-sound-sync-echo` | role echo, own hooks (first build) | **pass**; the cuff map never closed (START does not close it), so the menu section showed only the gate closing |
| `20261118-080919-two-games-sound` | two games, probe on both (first build) | **pass**, both exit 0; the map again stayed open, so the host's menu section had nothing to hold back |
| `20261118-081435-sound-sync-echo` | the map closed with B | fail (by the new check): the map shows a "new ranked event" notice that B does not dismiss; A then B closes it |
| `20261118-081720-sound-sync-echo` | role echo, **final build** | **pass**, exit 0, saves unchanged |
| `20261118-081930-two-games-sound` | two games with the probe, **final build** | **pass**, both exit 0, saves unchanged, 0 uploads, probe 0 dropped |
| `20261118-082240-two-games-sound-own` | two games, own hooks (the shipping set-up), **final build** | **pass**, both exit 0, saves unchanged, 0 uploads |

Every run: game folder restored, no `error.txt`, no fault, exit code 0. The mod changes after the first two runs were
log text, the RTPC-update pruning, the rate limit (12 -> 16 a second), the gate log lines and the stop on losing the
partner body; the launcher option came after the last run (no mod change).

### 16.3 Sent, posted, ended [verified, `[psound] counts` at the end of each game]

The host plays Sam, the guest Fragile (porter suit) with the self view; `voice_sync=1` and `self_voice=auto` on both.

| Run, game | Sent posts (foley / steps / weapon) | Posted from the partner (foley / steps / weapon) | PostEvent failed | Ended (`AK_EndOfEvent`) | Callback layout ok / bad | Held back by the gate | Play time - event time (min / avg / max) | Own posts seen, not sent |
|---|---|---|---|---|---|---|---|---|
| `081930` host | 434 (268 / 161 / 5) | 275 (171 / 99 / 5) | 0 | 275 | 275 / 0 | 105 items (his map) | 1 / 13 / 33 ms | 275 |
| `081930` guest | 362 (224 / 133 / 5) | 334 (206 / 123 / 5) | 0 | 334 | 334 / 0 | 118 items (her map) | 0 / 20 / 32 ms | 334 |
| `082240` host | 417 (251 / 161 / 5) | 300 (188 / 107 / 5) | 0 | 300 | 300 / 0 | 99 items | 0 / 6 / 11 ms | 300 |
| `082240` guest | 383 (237 / 141 / 5) | 332 (198 / 129 / 5) | 0 | 332 | 332 / 0 | 106 items | 1 / 26 / 34 ms | 332 |
| `081720` echo | 515 (314 / 196 / 5) | 478 (290 / 183 / 5) | 0 | 478 | 478 / 0 | 40 items | 0 / 5 / 13 ms | 478 |

- **Both directions.** Every post the partner sent was either posted or held back by the receiver's open map (`082240`:
  of the host's 417 posts the guest posted 332, the rest arrived while her map was open; of the guest's 383 the host
  posted 300). 0 STA1 frames lost on the loopback link, 0 late, 0 far, 0 over the rate limit in the two-game runs.
- **Playing ids and callbacks.** Every one of our `PostEvent` calls returned a playing id (0 failures: every mirrored
  event exists in the receiver's loaded banks), and every one ended with `AK_EndOfEvent`; the callback's
  `AkEventCallbackInfo` (`{cookie, game object, playing id, event id}`) named our object for the cookie in every case,
  so the Wwise 2019.2 layout is right [verified].
- **What was mirrored** (`081930`, both sides): steps `0x04A0FA03` 121, `0x3BA60DF6` 71, `0x3CA60F68` 35 (the movement
  layer, 7 RTPCs each), `0x0B509EF4` 13, `0x5BE9FB68` 12, `0x0C053F73` 8, `0x3C13FF22` 7, `0xEBD93A00` 6; foley
  `0xC5DEF120` 155, `0xAD954B69` 140, `0xEBC2378E` 63, `0x0D308F5C` 19, ...; weapon (the rifle's `SoundComponent` on
  `DSWeaponEntity`) `0xFCF94B42`, `0xEE160B69`, `0xC1CFA276`, `0xAD954B69`, `0x6D69D583`. Denied: the camera's and the
  Odradek's `SoundComponent` (2-5 posts a run). No cargo or gear sound was posted in these scenarios.
- **Feedback.** Our own posts go through the same hooked `PostEvent`: the sender saw exactly as many as the receiver
  posted (last column) and sent none of them; their objects are never registered in a context.

### 16.4 Where and when [verified]

- **On the partner's joints.** Every `partner post` line names the joint and its height over the body's root; joints
  were read from the body's model in all 1,223 logged posts of the final two-game runs (0 fallback heights), the Fragile body
  on the host included (through its joint map). Example (`082240`, host, the guest's footstep): `0x0B509EF4 ... at
  (-1100.32, 274.12, 352.54): the partner's left foot joint (0.10 m over the body's root) + (-0.01, 0.00, -0.05) m`. The
  rifle sat on the right hand joint (offset 0.00-0.15 m). On the sender, the `DSCustomSoundComponent` foley sat on the
  hips joint (offset under 1 cm; 245 of 263 foley posts on the host in `081930`) or on a foot or hand; footsteps 5-14 cm
  from the foot joint.
- **Footsteps by position** (`sound_steps=1`): the material-emitter posts within 1 m of the player's root or 0.5 m of a
  foot joint were taken; 0-1 per game were not near and not sent. Risk unchanged: another walker within a metre would be
  mistaken for the player (none in these scenes).
- **Timing.** Items are posted at the pose play time of their moment: the receiver posted 0-34 ms after the ring's play
  time passed the event (one tick), on the same timeline the body's pose plays at (100 ms interpolation delay on the
  link, 2 s in echo). The sender queues an item 0-33 ms after the game posted it (`mirror post ... n ms after the post`).
- **Gating.** Opening the cuff map logs `gate closed: paused / menu / cutscene (HandcuffDevice)` and stops our playing
  sounds (`stopped the partner's sounds on 4 object(s)` in echo); closing it logs for example `gate open again after
  9667 ms (was: ... HandcuffDevice): 118 partner item(s) held back, 0 posted while it was closed` (guest `081930`; host
  105 / 0; `082240` 106 / 0 and 99 / 0; echo 40 / 0). No `partner post` line falls inside a closed window. The sender
  sent nothing while its own map was open (1 post not sent each).

### 16.5 Not done / not verified

- Footsteps rely on the position rule; the exact owner (tagging the effect request of the mover's footstep message) is
  still open (section 11 Q3).
- Not covered by a run: cargo and gear sounds (none was posted by the player in these scenes; cargo instances may not be
  registered in a component context), vehicles and carts (loops, RTPC updates on a playing sound), ladders and ropes,
  the 100 m cut-off, cutscenes and loading on the receiver, a lossy real Steam link (stops are repeated, posts are not),
  the hologram's fallback heights, long sessions, and two Sam players walking next to each other.
- `ExecuteActionOnEvent` was not called on a player object in these runs (the op is implemented and unit-tested only).
- RTPC updates after a post are sent while the object posted in the last 3 s (72-117 per game per run); whether a
  playing sound needs them was not measured.
- The mod's direct posts bypass Decima's instance limits; the per-slot rate limit (16 a second) never triggered in the
  two-game runs (2 drops in the first echo run at the old 12 a second).
- The launcher option was checked through the unit tests and the screenshot render (screen 40 shows it), not through a
  real Steam launch. It defaults to on: decide whether to keep that before shipping (the feature is experimental).

## 17. After the review: fixes, Phase 4 volume, final runs (2026-11-18)

A code-and-evidence review of Phases 0-3 raised 4 high, 4 medium and 10 low findings plus 8 wording problems. Each was
checked against the code and the run logs; the confirmed ones are fixed below, the others are refuted with the evidence.
Every run in this section: `-SeedSaves complete-100`, no `ds.exe` running before it, one run at a time, game exit code 0
(the guest's too), saves unchanged (hash manifest), game folder restored, no `error.txt`, 0 cloud uploads.

### 17.1 Runs (`build-testenv\runs\`)

| Run | Scenario | What it shows | Result |
|---|---|---|---|
| `20261118-090206-self-voice-fragile` | probe, Fragile self view, mute | silencing through the probe's PostEvent detour; stopped at a BB check written as required | fail (that check only); exit 0 |
| `20261118-090433-self-voice-fragile` | the same, the BB check made observational | 26 of 26 muted talks silenced, 0 unmatched | **pass** |
| `20261118-090911-bb-answer-selfview` | probe, Fragile self view, **no mute** (`self_voice=sam`) | Sam's `ReceivedLike` played unsilenced; BB did not answer | **pass** (the control for finding H2) |
| `20261118-091058-sound-sync-volume` | role echo, `partner_volume=50` | 466 posts at volume 0.50, 0 failures, 0 refused | **pass** |
| `20261118-091353-two-games-audio` | two games, launcher defaults | everything below (earlier build: only comments changed after it) | **pass** |
| `20261118-091758-audio-keys-off` | Sam, no audio key | nothing hooked | **pass** |
| `20261118-092019-self-voice-own-hook` | Fragile, no probe, `sound_sync=0` | the PostEvent-only hook installed for the mute; 28 silenced, 10 passed while the mute was off | **pass** |
| **`20261118-092559-two-games-audio`** | two games, launcher defaults, **final code** | mute, partner voice and partner sounds both ways | **pass** |
| **`20261118-092929-audio-keys-off`** | Sam, every audio key off, **final code** | nothing changes | **pass** |

### 17.2 Phase 1 changed: the muted talk runs, its voice is silenced [verified]

*(Corrected after v0.11.0, section 18: the volume call returned 1, but the game's sound-instance update set the object's
volume back to its own level right after it, so the voice was heard. The post is now stopped instead.)*

- **Finding H2 ("the mute stops BB's answer") is refuted as stated.** With the guest-Fragile self view and *no* mute
  (`20261118-090911-bb-answer-selfview`, `self_voice=sam`), the queue took `ReceivedLike` (`check 1, queued 1`), Sam's line
  played (`voice post #6 event 0xA4D58CE8 ... 1.58 m from the local player`) and **no BB post (`0x8118407C`) followed in 15 s**.
  The silenced run `090206` shows the same. So BB's silence comes with the guest set-up, not with the mute [verified]; why
  BB does not answer there is not known [unverified]. In every Sam control BB answered (3 of 3).
- The review's further risk (game logic waiting for a dropped talk) is removed anyway: with a PostEvent detour in, a muted
  talk of the local player now **runs**, and `selfvoice::PrePost` (called by partner_sound's or the probe's PostEvent detour
  before the original) gives the one external-source post that plays it `SetGameObjectOutputBusVolume(object, listener 0,
  0.0)` first (a queued Wwise message on the same thread). Which post: a talk token (a local talk let through in the last 3 s)
  plus the object's position within 3 m of the local player's head and nearer it than the partner's talking entity, never BB's,
  the cuff device's or a child's preset (`0x8118407C`, `0xD44C68CF`, `0xE5BEA679`). The "exact" link `[[comp+0x40]+0x90]`
  from the Phase 0 reverse scan never matched at post time (`exact 0` in every run) [verified]: the token rule did all of it.
- A muted object reused for another sound gets its volume back first (`restored`; 0 in every run: Wwise forgets an object's
  volume at its unregistration). The subtitle of a muted spoken line is dropped once (`-> DROPPED (the local player's own muted
  line, self_voice)`). The cargo menu figure's talks are still dropped (no answer chain there).
- With `sound_sync=0` partner_sound hooks only `PostEvent` and `UnregisterGameObj` for this (`[psound] sound_sync=0: only
  PostEvent and UnregisterGameObj hooked, for self_voice (applied); nothing is recorded or sent`, run `092019`). Without any
  such hook the talk is dropped as before (logged).
- Counts: `090433` 26 talks / 26 posts silenced, 0 unmatched, 0 failed; `092019` 28 / 28, 10 passed with the mute off;
  final `092559` guest 16 / 16 (`voice posts silenced 16 (exact 0, token 16), volume call failed 0, restored 0, near the
  player but not matched 0`). Silence itself cannot be heard in a hidden run: what is verified is that the volume call returned
  1 before every Sam voice post of the local player and that no such post went unmatched.

### 17.3 Phase 4: the partner's sound volume [verified in the log; loudness unverified]

`session.ini partner_volume=0..100` (default 100; launcher Debug slider **PARTNER SOUND VOLUME**, shown under "Hear your
partner's footsteps and sounds" and written only with it on and below 100). Each receiver object gets
`SetGameObjectOutputBusVolume(object, 0, volume)` when it is registered; 0 posts nothing (`partner_volume 0` counter). The
partner's *voice* is not affected (it plays on the talking entity's own game object). Run `091058`: `volume 0.50`, 466 posts,
466 ended, no `SetGameObjectOutputBusVolume(our object N) failed` line.

### 17.4 The review's code findings

| # | Finding | Verdict | What changed |
|---|---|---|---|
| H1 | The receiver posts any event id | **Confirmed, fixed** | Only allow-listed ids are posted or acted on: the 48 ids the Phase 3 runs mirrored (seed), plus every id this game posted on its own player's allowed objects this session (learned on the sender side), never the voice presets, the camera's or the fixed global posts. Refused posts are counted and the first 10 logged. Final run: host refused 1 (`0x365285EE`), guest 3 (`0x495AE4B0`, `0x235ADEFB`, `0xEBD93A07`), all foley / steps not yet seen locally |
| H2 | The mute stops BB's answer | **Refuted** (17.2) | The talk runs anyway now |
| H3a | `g_rx_pids` cleared at 2048 | **Confirmed, fixed** | Both pid maps lose an entry at its end-of-event callback (the remote id is kept with the local one); no clear |
| H3b | Stops dropped as late / early / no partner | **Confirmed, fixed** | Stops and actions run when due, or at once when they cannot be scheduled; only posts are dropped for timing |
| H3c | Sender forgets playing ids after 120 s / clears at 2048 | **Confirmed, fixed** | Pruned by age only, after 30 min; the receiver also stops anything it played for 15 min (`capped`) |
| H4 | Launcher defaults all on | **Kept, decision documented** | The user asked for both players to hear each other; the defaults stay on, and the release notes say so. With the keys off nothing is hooked (17.1, `audio-keys-off`) |
| M5 | `active` leaks after a new session | **Confirmed, fixed** | `NewSession` keeps `g_local_pids`, so the callbacks of the stopped sounds bring `active` down |
| M6 | Deny-list too small | **Confirmed, fixed** | Sender allow-list: the player's own `DSCustomSoundComponent`, and weapon or cargo entities under him. Gear, the BB pod, the cuff device and other player components stay local. The RTPC part (first-person values replayed) is **unverified**: no RTPC name is known |
| M7a | Unregister record after the original | **Confirmed, fixed** (partner_sound and probe) | Pushed before the original |
| M7b | Dropped hot records keep ownership | **Confirmed, fixed** | Ownership and the context cache are cleared when records were lost (0 lost in every run) |
| M8 | Subtitle filter can eat the local subtitle | **Confirmed, fixed** | Disarmed when `PlaySentence` refuses or faults; window 15 s -> 2 s (requests came 0-31 ms after). Final run: host's own subtitles 2 requests, 0 dropped |
| L | Speech frames over 16 per pump lost | **Confirmed, fixed** | Left in the reliable receiver for the next pump; undecodable ones counted |
| L | One bad STA1 item drops the frame | **Confirmed, fixed** | Items checked one by one (`wire::SoundItemValid`, unit-tested); `invalid items dropped 0` in every run |
| L | Reordered STA1 frames dropped | **Confirmed, fixed** | A 64-seq bitmap drops duplicates only; the lost counter no longer counts a late frame |
| L | No rate limit on received speech | **Confirmed, fixed** | 2 lines handled a tick, the rest wait (reliable, not dropped) |
| L | "the link is not connected" for a full window | **Confirmed, fixed** | The reason names the full window |
| L | `PlaySentence` from the game thread may race | **Unverified** | No fault in any run (11 + 1 lines in the final run); not changed |
| L | Log volume | **Confirmed, fixed** | Talking-entity line every 60 s, `[psound] status` every 30 s |
| L | Probe leaves a created hook | **Confirmed, fixed** | Removed when queueing fails; the real status is logged |
| L | `voice_sync 1` NaN | **Confirmed, fixed** | Also `self_voice 2/3` and `audio_probe 2` |
| L | Objects never re-registered | **Confirmed, fixed** | A post returning 0 unregisters our record of the object; it is registered again (with its volume) before its next post (`re-registered 0` in every run) |

### 17.5 Wording corrections [verified from the run logs]

- **"Rifle fire"**: `0xC1CFA276` / `0xEE160B69` / `0xFCF94B42` come when the rifle is raised and lowered (run `081930`: 77.11 s
  and 80.25 s, the trigger at 78.6-79.95 s with no post in between; the scenes are inside a garage). Whether gunshots are posted
  on a player-owned object, and so mirrored, is **untested**.
- **"No longer hears Sam's grunts, efforts or breathing"** holds for what the runs exercised (idle, walk, run, jump, the test
  situations, the cuff map). Falls, stumbles, heavy cargo, climbing, damage, BTs, vehicles and the private room were not
  exercised; an effort played as animation foley (not through the talk path) would not be muted.
- **The cargo menu figure** was only ever made to talk by the debug action (`self_voice 2 n`); when the game itself does so is
  unknown.
- The external-source deny rule of the sender never fired (`0 voice` everywhere): the local voice instances are never owned by
  an allowed component.
- **`StopAll` on the gate** never ran with partner sounds playing in a two-game run (`stop-all 0`).
- The harness reports `pass` when the game exits with `0xC0000005` after `quit` (`055718`): every result above was also checked
  for `game_exit_code` 0 (and `guest_exit_code` 0).

### 17.6 The final runs [verified]

`20261118-092559-two-games-audio` (host Sam, guest `fragile_suit` with the self view; `self_voice=auto`, `voice_sync=1`,
`sound_sync=1` on both, no probe):

| | Host (Sam) | Guest (Fragile) |
|---|---|---|
| Own talks | 16, all sent as Sam lines (kind 0); not muted | 16 muted: **16 silenced in Wwise, 0 unmatched, 0 failed**; 15 sent as kind 1, 1 as kind 0 (the test step) |
| Partner lines played (`PlaySentence` = 1) | 1 (the guest's test line) | 11 of 16; 5 skipped while her cuff map was open |
| Subtitles | own shown (2 requests, 0 dropped) | 2 partner lines dropped, 2 own muted lines dropped |
| Sounds sent (foley / steps / weapon) | 443 (277 / 161 / 5) | 372 (228 / 139 / 5) |
| Partner sounds posted / ended | 282 / 282 (foley 176, steps 101, weapon 5) | 340 / 340 (foley 214, steps 121, weapon 5) |
| Refused by the allow-list | 1 | 3 |
| Held back by the cuff-map gate, posted while closed | 105, 0 | 101, 0 |
| Play time - event time (min / avg / max) | 1 / 5 / 22 ms | 9 / 28 / 33 ms |
| Lost frames, PostEvent failures, faults | 0, 0, 0 | 0, 0, 0 |

The weapon posts sat on the partner's right hand joint (`partner post weapon ev 0xFCF94B42 ... the partner's right hand joint`).
`20261118-092929-audio-keys-off`: `[config] self_voice=sam voice_sync=0 voice_subtitles=0 sound_sync=0 sound_steps=1
partner_volume=100`, `[voice] ... Sam's voice plays as usual (nothing hooked)`, no `[psound]`, `[pvoice]` or `[audio]` line.

### 17.7 Status per phase

- **Phase 0** (probe): done; a test key only.
- **Phase 1** (mute Sam for a non-Sam player): done, changed to silencing (17.2); verified by log in every Fragile run since.
- **Phase 2** (partner voice): done; both directions verified (the guest's direction only with the test op, by design: Fragile
  has no voice of her own).
- **Phase 3** (partner footsteps, foley, weapons): done, experimental; allow-lists and loop safety added (17.4).
- **Phase 4**: volume done (17.3); Fragile line mapping and a female non-verbal substitute not done (not trivial: her terminal
  lines are not resident, and playing them needs the register-by-path loader and STS1 kind 2).

### 17.8 Not verified

- Audibility: hidden runs are silent. A playing id, `AK_EndOfEvent` and a volume call returning 1 prove what Wwise accepted,
  not what a player hears (3D placement, left / right, distance, the 0.5 volume, the silence of the muted line).
- Outdoors (surfaces, timefall, BTs), vehicles and carts (loops, RTPC updates on a playing sound), ladders, ropes, cargo and
  gear sounds, gunshots, falls, distances over about 21 m (the 100 m cut-off), a lossy Steam link, two Sam players, a non-Sam
  host, a guest without the self view, long sessions, and the 15-minute cap.
- Why BB does not answer `ReceivedLike` in the guest set-up.
- The `0xC0000005` after `quit` of Phase 0 (`055718`) did not recur in 9 runs here (11 game processes); its cause is unknown.

## 18. The grunts came back in v0.11: the mute is now a stop (2026-11-18)

After v0.11.0 the user still heard Sam grunting when Fragile jumped and walked around. Their own logs (two games, the guest
`fragile_suit` with the self view, launcher defaults) showed the guest's 53 talks all "silenced in Wwise" (53 of 53, 0 unmatched,
0 failed) and the host playing none of them (`received 53 ... played 0`). So the matching was right and nothing crossed the link:
the volume-0 mute itself did not hold.

### 18.1 Cause [verified]

- `WwiseSimpleSoundInstance` (vtable `0x143C1C188`) starts at `+0x148` = `0x141DBE710`: it computes the instance level
  (`0x141DCC800`, which sets the dirty flag `inst+0x2AA`) and posts (`0x141DBE900` -> `0x141DD2040` -> `PostEvent`; v0.11's
  `PrePost` ran here). Its update `+0x168` = `0x141DCC450` calls `0x141DD3380`, which, while `+0x2AA` is set, calls
  `SetGameObjectOutputBusVolume(inst, 0, [+0x2AC] * [+0x2A0])` (call `0x141DD34FB`) and `SetGameObjectAuxSendValues(inst,
  sends, n)` (call `0x141DD35B2`, the zone reverb sends). The flag is set at construction (`+0x2AB`, base constructor
  `0x141DAB380`) and whenever the level or the zone values change, so it is still pending when the voice is posted.
- Probe run **`20261118-112502-grunts-fragile`** (v0.11's method, both exports hooked by the probe, logging only): on every one
  of the 51 objects whose voice post was set to volume 0, Decima called `SetGameObjectOutputBusVolume` again with **1.000** from
  `ds.exe+0x1DD3500` (the return address of `0x141DD34FB`) and `SetGameObjectAuxSendValues` with **1 send** from
  `ds.exe+0x1DD35B7` (51 of 51 each). The last call wins in Wwise: the voice played at full level, with its reverb.
- Loudness, measured by Wwise itself (`AK::SoundEngine::RegisterBusMeteringCallback`, peak per audio frame; the hidden test
  silences only the device buffer, after the mix). Sam's voice bus `0xDCB6FE59` and its next two parents get no mix node of
  their own in this game (the probe's `[audio] mix-bus node` census); the first bus of the chain that mixes is the voice tree's
  `0x7CC88FD9`, metered with the master `0xE2B7BC37` as a check. Run **`20261118-113511-grunts-fragile`**, one game, the same
  moves in each section (walk, run, 6-10 jumps while running, the Jumping and OutOfBreath situations, idle):

| Section | Sam's voice posts | Loud stretches on `0x7CC88FD9` (peak > -60 dBFS) | Peaks |
|---|---|---|---|
| v0.11's method (`self_voice 4 0`) | 17 | 10 (385 loud frames) | -9.5 to -25.5 dBFS |
| mute off (`self_voice 1 0`, the control) | 15 | 8 (366 loud frames) | -9.6 to -22.0 dBFS |
| **stopped (this fix), 50-100 s and 149-178 s** | **45** | **0** | - |

  (Counts of loud stretches corrected by the review's recount: 10 and 8, not 11 and 9; the frame totals and peaks stand.)
  So the v0.11 mute was as loud as no mute at all. The only loud stretches while stopping (36-49 s, before the first move)
  coincide with five radio-type voice posts (`0x374FF1A2`, `0x518FE01D`, 2-D at the origin, 1186 m away: not Sam's) and one
  stopped Sam post inside the same window (`#3`, 37.6 s), which therefore cannot be told apart there.
- What the grunts were: Sam's own voice through the talk path, as section 12.1 found. In run `113511` 78 of the 83 voice
  posts (external sources) were his voice presets (`0x3E51AF82` 41, `0x1C5DF301` 15, `0x3D4BF9E2` 13, `0x7B540EFF` 5, `0xB4FECB5F` 4;
  `0xFF918A85` and `0xA4D58CE8` in the earlier runs), posted by `DSTalkComponent` 1.0-1.5 m from the player (the other 5: the radio-type `0x374FF1A2` / `0x518FE01D`); the animation voice
  events (indices 2500-2599: 142 in this run) feed the same voice queue (section 12.1; 78 talks here, not every event becomes a talk). No vocal sound took another path in the observed data, and
  none of the 80 event ids the player's components post or that `sound_sync` ever mirrored routes under the voice tree
  `0x7CC88FD9` (bank metadata) [inferred: routing only; no sample was listened to].

### 18.2 The fix

- **`native/mod/self_voice.cpp`**: a muted voice post of the local Sam is **stopped right after it is posted**: the PostEvent
  detour (partner_sound's or the probe's) asks `PrePost` before the original and calls `AfterPost(playing id, its StopPlayingID
  original)` after it, which calls `StopPlayingID(id, 0 ms, linear)` (SEH-guarded; the trampoline when that detour hooked
  `StopPlayingID`, else the export, checked at start-up: RVA `0x3A20B20` and its first bytes, or hooked by the probe). The stop
  is queued behind the play message on the same thread, so Wwise handles both in one audio frame and the streamed voice never
  renders, dry or reverb. The talk still runs (BB's answer chains as before); its subtitle is still dropped. Hot path: two
  atomics, at most four fault-free reads, no lock, no allocation, no logging.
- **Which post**: one of **Sam's 28 voice events** (`selfvoice::kSamVoiceEvents`: every event whose sounds route to his voice bus
  `0xDCB6FE59`, all external-source presets), not BB's / the cuff's / a child's preset, and either the talk component's playing
  instance (`exact`) or, **while a muted talk of the local player is pending** (3 s), within 3 m of the local player's head and
  nearer it than the partner's talking entity (both measured 1.55 m over the root). Without a pending talk a position match is
  left alone, so another Sam actor near the player (an in-engine cutscene's, the menu figure's) is never cut (review follow-up;
  in every run each of Sam's voice posts had one pending: `none pending 0`). An external-source post near the player with a muted talk pending that is *not* one of his events is
  kept and logged (`... is not one of Sam's voice events: kept`), so radio calls and other characters are never cut.
- **`native/mod/partner_sound.cpp`**: the 28 events are on the deny list too (never sent, never posted), on top of the existing
  rule that external-source posts are never mirrored. With `sound_sync=0` the self_voice-only hook set is now PostEvent alone
  (UnregisterGameObj is no longer needed).
- **Debug**: `self_voice 4 <0|1>` switches between v0.11's volume 0 (test only, for before / after measurements) and the stop.
  The audio probe adds the bus meters, the mix-bus census, the loud-stretch timeline of `0x7CC88FD9` and the counts of Decima's
  volume / aux-send calls on the stopped objects (`[audio] voice after mute`, `[audio] voice bus ... loud from`).
- No wire change; with `self_voice=sam` (or Sam) nothing changes; keys and defaults unchanged.

### 18.3 Runs (`build-testenv\runs\`, all `-SeedSaves complete-100`, one at a time, no other `ds.exe` running; game exit code 0, saves unchanged, no `error.txt`, 0 cloud uploads)

| Run | Build | What it shows |
|---|---|---|
| `20261118-112502-grunts-fragile` | v0.11's self_voice + the probe's new hooks | the cause: 51 of 51 "silenced" objects set back to volume 1.000 and given 1 aux send by Decima; 51 talks, 0 unmatched (the bus meter on `0xDCB6FE59` read 0 frames: that bus has no mix node) |
| `20261118-113135-grunts-fragile` | the fix, meters on the whole chain | 42 posts stopped, 15 v0.11, 14 unmuted (`passed 14`); `0x7CC88FD9` max -18.4 dBFS while stopping (the radio at the start), -9.3 v0.11, -10.0 mute off |
| **`20261118-113511-grunts-fragile`** | the fix + the loud-stretch timeline | the table above; `stopped 46 of 63` muted talks (17 by v0.11 in its section), stop call faulted 0, post failed 0, not-Sam's-event 0, other talkers kept 0; 63 muted talks gave 63 posts (no retries) |
| **`20261118-113842-two-games-audio`** | the fix, launcher defaults, no probe (partner_sound's own hooks: the stop through its `StopPlayingID` trampoline) | guest (Fragile): `stopped 13 of 13`, faulted 0, post failed 0, other talkers kept 1; 12 talks sent as kind 1 (muted), 1 Sam line (the test step); host: `received 13 (12 muted by the partner): played 1` (the test line), sound_sync posted 315 foley / steps / weapon of the guest's, 0 voice sent either way, refused by the allow-list 4 (host) / 2 (guest: foley ids `0xBF02CC1E`, `0x72296642`, as in the user's log); both exit codes 0 |
| **`20261118-130839-two-games-release`** | the shipped build, launcher defaults (`voice_sync=1`, `sound_sync=1`), the guest with the probe | guest (Fragile): `stopped 15 of 15`, faulted 0, post failed 0, none pending 0; host: `received 15 (15 muted by the partner): played 0`. On the guest's `0x7CC88FD9` meter, the grunt section (her walk, run, six jumps, the Jumping and OutOfBreath situations, 74-105 s) holds ten of her Sam voice posts (1.1-1.5 m from her, all stopped) and one loud stretch, 0.67 s at -11.5 dBFS right after a post 0.00 m from the partner's talking entity (the host's own Sam line played as the partner voice; guest `received 4, played 4`); the vehicle section's two stretches follow partner-voice posts too. The only other loud stretches (47-61 s, before any move) overlap the radio-type posts (`0x374FF1A2`, `0x518FE01D`) and four stopped posts, as in `113511`. Run `20261118-130121` (the same scenario) gave the same picture: 15 of 15 stopped, the host played 0 of 15. |

Footsteps and foley were not touched (the change only acts on external-source posts of the 28 events): player-owned
`DSCustomSoundComponent` posts 640 / 782 / 824 and steps / movement emitter posts 434 / 540 / 567 in the three runs, which
differ in length (the last two have the extra sections).

### 18.4 Not verified

- **Audibility by ear.** Hidden runs are silent; what is measured is Wwise's own mix peak on the first mixing bus of Sam's voice
  chain. A short attack before the stop (less than one audio frame) would not show in a per-frame peak above -60 dBFS only if
  it were quieter than that; none was seen. Reverb tails go through aux buses that were not metered (the stopped voice feeds
  none).
- The fix acts on the game of the player who is not Sam (the guest in these runs). The host never played the guest's
  Sam vocals in the user's v0.11 logs (0 of 53) nor in the runs, so the grunts the user heard came from the guest's own
  game [I: by the logs; by ear only on a real desktop].
- Falls, stumbles, damage, BTs, climbing, ladders, ropes, vehicles, heavy cargo, the private room and long sessions were not
  exercised; a vocal sound posted without external sources (none seen) would not be stopped, but would still never be
  mirrored (deny list).
- Whether stopping the voice at once shortens the talk for `DSTalkComponent` (it gets its end-of-event at once): no repeated
  post and no extra talk was seen (63 talks, 63 posts); BB's answer chain was not re-run.
