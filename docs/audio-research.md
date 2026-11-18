# Audio research: the guest's voice and syncing both players' sounds

Status: **research only, static** (2026-11-17). Nothing here is implemented. The game was not run for this
page. The evidence is the installed `ds.exe` (same build as [reverse-engineering.md](reverse-engineering.md):
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
   `DSTalkComponent` from Sam's head joint [verified]. **[inferred]** that this covers all of Sam's voice:
   that the animation-driven grunts and efforts (events 2500-2584, handled only by Sam's sound graph,
   section 3.2 B) take the same path, and that the component handling it is Sam's own (section 2.3), are
   not confirmed (open questions 1 and 3). If they are, one hook on that handler (`0x1422D4F70`), filtered
   to the local player, can mute or replace all of Sam's voice on the guest's own game; Phase 0 (section 9)
   must confirm it first. It does not touch footsteps, foley, cargo or weapon sounds.
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
     posted on the local player's own emitters (plus their switch and RTPC values), send
     `{event id, emitter slot, offset}`, and the receiver posts the same event on its own Wwise game object
     placed on the partner body's matching joint.
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
  The runtime thread is unknown, so the hook must be thread-safe.
- `SoundManager` singleton: `[0x147AE8700]`. `0x141DB61A0(mgr, 0, SoundResource*, flags)` creates a sound
  instance (used by the talk handler and by `SoundComponent` play) [verified]; how an instance is placed and
  started is not decoded yet.

### 2.3 Components that make the player's sounds [verified unless marked]

From `ds/entities/player/ds_player_character` (`DSPlayerEntityResource` 9b58ada6-…) and the run logs:

| Component | Where | Role |
|---|---|---|
| `DSCustomSoundComponent` (RTTI `0x144C5AC40`) | `ds/entities/player/dsplayersoundcomponentcoretext` | Receives animation sound events (`MsgAnimationSoundEvent`), throttles them, plays their sound, turns "voice" events into `MsgDSVoiceTriggerEvent`, and turns foot/hand impacts into footstep calls |
| `NodeGraphComponent` running `ds/sounds/state_machine/sd_state_machine_setup_sam` | component list | Sam's sound state machine. Entry points for `MsgEntityUpdate`, `MsgDSVoiceTriggerEvent`, `MsgAnimationTextEvent`. Its constants point at Sam's foley, footstep material, ladder, battery, stealth, BT-area and weapon graph sounds |
| `VoiceComponent` (+ `DSTalkComponent`) | `VoiceComponentResource` 65d3595c-… in the character core | The talking voice. `DSTalkComponent` handles `MsgDSStartTalk` and `MsgStartTalk`. That Sam has a `DSTalkComponent` is **[inferred]** from the voice update sending `MsgDSStartTalk` to him; the runtime census only listed it on other bodies |
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
- **Situation hash = CRC32-C with initial value 0 and no final xor** over the name. Hashing every string
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
- **[inferred]** The graph turns the index into a situation and asks the voice manager through the
  `PlayPlayerVoice` script export, so these efforts also end in path C. Phase 0 checks this by logging the
  enqueue's return address while the graph runs.
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
  volume or priority float, 4.4 / 4.6 / 4.3 by situation) and adds the sentence with `0x1422D3D90(msg,
  SentenceResource*, delay, 0.1)` into the array at `msg+0x30`. It then sends the message to the player
  entity with `0x14235C990(player, &msg, 0)`.
- `DSTalkComponent`'s handler (`0x1422D6AA0` → **`0x1422D4F70`**) reads `msg+0x10`, `+0x28`, the entries
  at `msg+0x38` (count `+0x30`, 0x18 bytes each: `{SentenceResource*, SoundResource*, f32, f32, ...}`) and
  `+0x40`. For each entry with a sound it creates an instance (`0x141DB61A0(SoundManager, 0, sound, 0)`)
  and queues the talk.
- **The cargo menu's Sam** speaks through `DSPlayerEntity_sExportedPlayAkukanVoice` (`0x142757D40` →
  `0x14273BB10` on the akukan entity) [verified]; its resource has its own `DSCustomSoundComponent`,
  `VoiceComponent` and `NodeGraphComponent` (`sd_state_machine_setup_sam_akukan`) [verified, census log].

### 3.3 Where to suppress or swap (guest playing Fragile)

| Hook | What it sees | Use | Risk |
|---|---|---|---|
| **`DSTalkComponent::OnMsgDSStartTalk` `0x1422D4F70`** (MinHook; or redirect the 5-byte `jmp` thunk at `0x1422D6AA0`) | Every talk request to any entity with a talk component. Filter: owner `[comp+0x48]` == local player entity | **Recommended.** Drop the message (mute), or replace the entries' `SentenceResource*` and `SoundResource*` (swap), keeping refcounts (`+0x18`, `lock xadd`). It also carries the sentence for Phase 2 | Medium. Called for NPCs too; the filter must be one compare. The entry layout needs a Phase 0 check |
| Sentence pick `0x1429232F0` | `(group, queue entry)` → `DSPlayerSentenceResource*` | Returning 0 for a normal situation drops it before any message is built | Higher. The 0 path for the two special hashes (`[0x144BB4774]`, `[0x144BB4778]`) falls back to another list |
| Enqueue `0x1426A6570` | Situation hash, index, flags | Classification (spoken or not, priority) and logging | Low as a pure logger; not needed to mute |
| `PlayPlayerVoice` export | Script-requested situations only | Not enough on its own: misses the 46 native callers | — |

Muting at the talk handler keeps Sam's **non-voice** sounds (footsteps, cloth, cargo, weapons, Odradek,
BB). If the animation-driven efforts and grunts also reach this handler (**[inferred]**, section 3.2 B; open
question 1), that is what the user asked for: the grunts stop, everything else stays. If Sam's sound graph
plays them another way, the talk hook misses exactly those, and Phase 0 must find their path (the graph's
`SentencePlayResource_00_09` sub-graph, or the Wwise post they end in) before Phase 1 is built.

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
  lines, so it is not the route for arbitrary lines.
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

This mapping is the main unknown for option 2. Candidates, in order of preference:

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
- **`STA1` (audio events)**: `'STA1' | u8 version 1 | u8 n | u16 seq | u32 sender_ms`, then n × `{u32 event
  id | u32 remote playing id | u8 emitter slot | u8 op (post/stop) | u8 nstate | u8 0 | nstate × {u8 kind
  switch/rtpc | u8 0 | u16 0 | u32 group/rtpc id | u32 value or f32}}`.
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
  (radio-like) or noisy; make it an option.
- **Language.** Never mirror voice at the Wwise level: the external source's WEM id depends on the voice
  language (26 per line).
- **Hidden tests are silent.** The test environment flags every audio buffer silent (`hidden-testing.md`),
  so audio can be verified only by logs and callbacks (8.1).

### 8.1 How to verify without hearing

- Count `PostEvent` results (playing id ≠ 0) and Wwise callbacks (`AK_EndOfEvent`, `AK_Duration`) for the
  mod's own posts.
- Log `PlaySentence`'s return value and the talk component's state.
- Optional: a test-only meter in the audio hook. Read the peak of the buffer before the SILENT flag is set;
  this needs `GetBuffer` hooked as well. It stays inside the test environment.
- `AK::SoundEngine::StartOutputCapture` is exported and not a stub [verified prologue], but its file path
  goes through Wwise's file layer. Do not use it until it is proven to write only into the run folder.

## 9. Phased plan

**Phase 0 - instrumentation (logging only, hidden runs).** Behind `audio_probe=1`:
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

**Phase 1 - mute Sam's voice for a Fragile guest (local only, no wire change).**
- `self_voice` key, launcher wiring and docs.
- Hook the talk handler: drop requests addressed to the local player when the setting is mute.
- Mute the akukan voice while the cargo menu shows Fragile.
- Unit-test the setting parsing.
- Hidden run: log counts of muted requests; check that footsteps and foley posts are unchanged.

**Phase 2 - partner voice.**
- `STS1` with encoder/decoder tests.
- GUID map from the resident group.
- Talking emitter at the partner's head (the echo entity, `partner_voice` path), `PlaySentence` on receive.
- A Fragile guest sends kind 1 (muted) or kind 2 (her own line) only.
- Two-game hidden run: both logs show sent, received, resolved and played (return 1).

**Phase 3 - partner footsteps, foley, weapons.**
- `STA1`, the Wwise mirror (option 2) with private game objects on the partner's joints, switch and RTPC
  replay, stop mapping, distance and pause gating.
- Two-game hidden run with posting statistics and Wwise end-of-event callbacks.
- Fallback to option 3 for animation foley if the owner mapping fails.

**Phase 4 (optional).**
- Fragile line mapping for a few situations.
- Experimental female non-verbal substitute.
- A partner-volume slider (`SetGameObjectOutputBusVolume` on the private objects).

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
| `0x14235C990` | send message to entity | `4C 8B DC 53 55 57 48 83 EC 50 48 8B 05 8F 46 C0` |
| `0x14235A460` | `SoundComponent::PlaySound` | `40 55 56 41 54 41 56 41 57 48 81 EC F0 00 00 00` |
| `0x141DB61A0` | `SoundManager` create instance (`[0x147AE8700]`) | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57` |
| `0x141DD2040` | Decima → `AK::SoundEngine::PostEvent` | `40 55 56 41 54 41 55 41 56 41 57 48 83 EC 68 48` |
| `0x143A1A5C0` | export `?PostEvent@SoundEngine@AK@@YAKK_KKP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXKPEAUAkExternalSourceInfo@@K@Z` | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 4C` |
| `0x143A1CF50` | export `RegisterGameObj` (both overloads) | `40 53 48 83 EC 20 48 8B D9 48 83 F9 E0 72 0B B8` |
| `0x143A1FA50` | export `SetPosition(_K, const AkTransform&)` | `48 83 F9 E0 72 06 B8 02 00 00 00 C3` |
| `0x143A21640` | export `UnregisterGameObj` | `40 53 48 83 EC 20 48 8B D9 48 83 F9 E0` |
| `0x143A202F0` / `0x143A1FC10` / `0x143A20B20` | exports `SetSwitch(K,K,_K)`, `SetRTPCValue(K,M,_K,...)`, `StopPlayingID` | (from the export table) |

Message vtables: `MsgDSStartTalk` `0x143CF3A50`, `MsgDSVoiceTriggerEvent` `0x143DEBB80` (RTTI
`0x144C63A10`), `MsgAnimationSoundEvent` RTTI `0x144867BD0`. Component RTTI: `DSTalkComponent`
`0x144AE8400`, `TalkComponent` `0x144AFF170`, `VoiceComponent` `0x144AFE820`, `SoundComponent`
`0x144AFDAC0`, `DSCustomSoundComponent` `0x144C5AC40`, `DSEchoComponent` `0x144C23EC0`,
`WwiseGameObject` `0x144A724C0`, `SoundManager` `0x144A6F280`.

## 11. Open questions

1. Does the graph's `MsgDSVoiceTriggerEvent` handler call `PlayPlayerVoice`, or does it play sentences in
   another way? (Phase 0, return addresses)
2. The exact `MsgDSStartTalk` entry layout beyond `{SentenceResource*, SoundResource*}`, and whether Sam
   has a `DSTalkComponent` of his own or the message goes elsewhere.
3. The `SoundInstance` → component → owner link (for 6.2), and which instance field is the Wwise game
   object.
4. Whether `PlaySentence` on the echo entity plays with 3D position at the echo (it should, `Delivery =
   on_actor`) and whether it shows subtitles.
5. How footstep surface switches are set: on the instance's game object before the post (mirrorable), or
   inside the event by a game-object switch already set at registration.
