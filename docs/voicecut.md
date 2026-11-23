# voicecut: Fragile's voice clips

voicecut is a small local editor for the guest's voice. The guest plays as Fragile, but the body is drawn over Sam, so
the game still makes Sam's noises (stumbling, falling, lifting cargo, catching his breath) and says his random lines.
voicecut lets you listen to every Fragile line and cutscene in your own game, cut out grunts, breaths and lines, sort
them into categories, compare them with Sam's versions, and export them in a form the mod can load later.

It is a separate program, like devdash: Python 3 standard library only, a server on 127.0.0.1, an app window. It reads
the installed game and never changes it, never starts it, and never puts game audio in the repo.

## Start it

1. Copy `tools\voicecut\voicecut.bat.example` to the repo root as `voicecut.bat` (gitignored) and double-click it.
2. It opens an Edge (or Chrome) app window. The first start reads the game for about 10 seconds (the archive index and
   the line list); later starts are instant.
3. Close the window when you are done: the server stops on its own 15 minutes after the page stops asking.

Needs Python 3.7 or newer and Death Stranding Director's Cut installed through Steam (found through Steam's library
list; otherwise start it with `python tools\voicecut\voicecut.py --open --game "D:\...\DEATH STRANDING DIRECTORS CUT"`).
Other options: `--port` (default 8775), `--idle-exit` seconds, `--dir` (workspace), `--export-dir`, `--language`
(default `english`; any of the game's voice languages).

## Where things go

| What | Where | In the repo? |
|---|---|---|
| The tool | `tools\voicecut\` (code, page, tests, `voicecut.bat.example`) | yes |
| Your launcher | `voicecut.bat` in the repo root | no (gitignored) |
| Workspace: your clips and sorting (`state.json`, a `.bak` of the previous save), the line list (`catalog.json`), decoded lines (`audio\*.wav`, a cache kept under 1 GB), loudness figures (`measure.json`), the archive index (`index\`), `voicecut.log` | `%LOCALAPPDATA%\Stranded Together\VoiceCut` | no |
| Export for the mod: `voices.txt` and `fragile\<category>\*.wav` | `%LOCALAPPDATA%\Stranded Together\Voices` | no |

Nothing is ever written into the game folder (the archive reader refuses a cache there). Game audio only exists in the
two per-user folders above, decoded from your own copy of the game.

## Using it

**Voice lines (left).** Every Fragile and Sam line that has its own audio, and one entry per cutscene in which Fragile
speaks. Filter by speaker, source (lines or cutscenes), category, spoken or non-verbal, and length; search the text,
the source and Sam's situation names. Fragile is selected by default.

- *Fragile's lines* (79): her 22 terminal lines and 57 mission radio lines. They are all spoken; she has no separate
  grunts or breaths in the game data (docs/audio-research.md section 4).
- *Fragile's cutscenes* (23): each cutscene's dialogue track (every voice in it, without music or effects). This is
  where her efforts, breaths and short reactions are: the entry lists her subtitles in that cutscene so you know what
  to look for. The first open decodes the track (about 10 seconds per minute of cutscene, once; then it is cached).
- *Sam's lines* (2,263): his 1,991 player lines with the situations the game plays them in, plus his mission and
  terminal lines.

**Editor (middle).** Click a line to load it. The top strip is the whole line (click or drag it to scroll); the big
view is the zoomed part.

- Drag on the waveform to select; drag the pink edges to trim; click to move the playhead.
- Mouse wheel zooms around the pointer; Shift+wheel scrolls.
- **Play** plays from the playhead; **Selection** plays the selection with the fades, gain and normalise applied
  (exactly what the export writes); **Loop** repeats it, and changes are heard on the next loop.
- **Handle In / Out** chooses which edge the arrows and the nudge buttons move.
- **Fade in / out** (raised-cosine ramps, ms), **Gain** (dB), **Normalise** (brings the loudest sample to -3 dBFS
  before the gain). The white line over the selection shows the fades and gain.
- **Match Sam** sets the gain so the clip is as loud as Sam's measured lines in the chosen category.
- Pick a **Category** and press **Save clip** (or S). Editing a saved clip (the pencil in My clips) turns the button
  into **Update clip**; **New clip** saves the next one separately.
- The line under the form shows the clip's length, peak and loudness, and how much louder or quieter it is than
  Sam's lines in that category (green within 3 dB).

Keys: Space play/stop, Enter play the selection, L loop, I / O in / out point at the playhead, [ / ] choose the edge,
arrows move it 10 ms (Shift 100 ms, Alt 1 ms), + / - zoom, Z zoom to the selection, F show all, Home / End, S save.
The **Keys** button shows the list.

**My clips (right).** Your clips by category: play, edit, delete.

**Coverage.** One row per category: how many of Sam's situations and lines fall in it, and how many Fragile clips
you have. The bar fills at one clip per Sam situation (at least 2, at most 8); red means Sam has lines there and you
have none. Click a row to compare: Sam's lines (play, open) next to your clips. **Measure** decodes Sam's lines of that
category (about 0.1 s each) for their loudness; the summary then shows Sam's median loudness against your clips'.

**Sam's situations.** All of Sam's situations with lines, sorted ones first: how many lines (spoken or not), the
game's priority, a random sample and the full list. The menu sets the category Sam's lines in that situation count
for; "guess" keeps voicecut's guess. Use "Not sorted yet" to work through the unknown ones by ear. This sorting is
yours (saved in the workspace) and goes into the export.

**Export clips** renders every clip and writes `voices.txt`. **Open folder** shows the export folder in Explorer.

### Categories

stumble, fall, landing, jump, heavy lift, effort, breathing, pain, climb / rope, cold / water, humming, rest / sleep,
reaction, random line, other. Sam's lines are guessed from the situation names the game spells out: 52 of 423 are
known (for example `Jumping`, `LandingFromJumpHigher`, `SlipByRain`, `OutOfBreath`, `HoldBaggageNormal`,
`StrongDamageBatting`); a situation with only spoken lines is guessed as a random line; the rest start as "other" until
you sort them. Six hashes are labelled by the animation voice events they were seen with (audio-research.md 12.1).

## The export (`voices.txt`)

UTF-8, tab-separated, `\n` line ends, in the style of the textures' `.manifest.txt`:

```
stv-voices 1
game	<ds.exe size>-<ds.exe mtime>	tool	voicecut 1	language	english	rate	48000	speaker	fragile
# clip	<file>	<category>	<samples>	<peak dBFS>	<loud dBFS>	<source id>	<start s>	<end s>	<fade in s>	<fade out s>	<gain dB>	<normalise 0|1>	<sha256 of the file>
# situation	<Sam situation hash, 8 hex>	<category>	<user|known|guess>	<label>
clip	fragile\effort\effort_01.wav	effort	47966	-16.98	-26.18	c-sq_cs03_s00200	53.0937	54.0930	0.0100	0.0400	-6.00	0	6dc2...
situation	2CF2EAD9	jump	known	Jumping
```

- Each clip is a 16-bit mono 48 kHz PCM WAV (the game's own rate), already trimmed, faded and gained.
- `<file>` is relative to the folder; files are numbered per category in the order you made them.
- The `situation` rows map each of Sam's situation hashes (CRC32-C of the name, 31 bits, as the game's voice queue
  sees it) to a category, so the mod can turn "Sam would grunt for situation X" into "play a Fragile clip of category
  Y". Situations left as "other" are not listed.
- An export deletes only the files the previous `voices.txt` listed, then writes the new set: anything else you put in
  the folder stays.
- A reader should refuse another first line, and paths that are absolute or contain `..`.

## How the game stores voice audio [verified on the installed game, 2026-11-23]

All read with the repo's archive reader (`tools/re/ds_archive.py`: the packfiles in `data\*.bin`, decompressed with
the game's own `oo2core_7_win64.dll`). The layouts were hard-coded from the serialized field order and cross-checked
against a type-JSON decode during the research (every `SentenceResource` (2,335), `LocalizedSimpleSoundResource`
(2,298), `LocalizedTextResource`, `DSPlayerSentenceResource` (2,702), `DSSentenceSituationResource` (423) and priority
entry (425) in Sam's, the generic terminal, a mission and a cutscene core agreed field for field). voicecut needs no
type JSON.

- **A line** is a `SentenceResource` in `localized/sentences/<group>/<name>/sentences`: Delivery (u8), PostDelay,
  ShowSubtitles, then refs to its `LocalizedSimpleSoundResource` (the audio), lip-sync animation,
  `LocalizedTextResource` (subtitles) and `VoiceResource` (who speaks: `localized/voices/vr0040_frg` is Fragile,
  `vr0010_sam` Sam).
- **The audio**: `LocalizedSimpleSoundResource` = GUID, NameHash, Group ref, 132 fixed bytes (volumes, shape,
  attenuation), ExternalSourceCookie, Flags, SoundMixState and Preset refs, `LengthInSeconds[26]`, `WemIDs[26]` (index
  = language, 1 English), VolumeCorrectionRTPCValue, then custom data with one entry per voiced language:
  `u8, u32 length, path, GUID, u64, u32 size`. The path is `<sentences core>_sentence_<guid>.wem.<language>` and the
  audio is the archive file `<path>.core.stream` (its size matches the entry). The bytes after the language name are
  binary and can be letters (`.englishh...`), so the reader matches the length prefix, not the text.
- **The text**: `LocalizedTextResource` = GUID, then per language (ELanguage order, English first) u16 length + UTF-8,
  u16 length + notes, u8 flags.
- **The stream** is a Wwise Vorbis WEM: RIFF/WAVE, format tag `0xFFFF`, a 0x42-byte `fmt ` chunk with the `vorb`
  header inside (sample count, mod signal `0xBC`, setup and first-audio offsets, block sizes 2^8 / 2^11 for voice),
  2-byte packet headers, no granules, the packet-type and window bits stripped, and **codebooks given by id** into
  Wwise's packed aoTuV 6.03 library.
- **The codebook library is in ds.exe** (Wwise is linked in): a table of 599 increasing pointers (598 books and the end
  of the last, at `0x144327DE0`..`0x144339717` in this build) in `.data`. voicecut finds it by that shape, maps the
  pointers through the PE section table, and accepts it only if all 598 books parse and each ends within a byte of
  the next. Nothing of it is copied into the repo.
- **Cutscenes**: `ds/sounds/wwise_cinematics_sound_resource/csNN/<seq>/<seq>_sound` holds a `WwiseWemResource`
  (`..._m_and_e_track`, music and effects) and a `WwiseWemLocalizedResource` whose stream is
  `.../wav/<language>/windows/<seq>_voice_track.<language>`: the dialogue alone, 6 channels in Wwise order (FL FR FC
  LFE BL BR). The voices are on the centre with some panned to the fronts; voicecut mixes `C + 0.5 (L + R)`.
  Fragile's cutscene sentences (`ds_lines_cutscene/<seq>/sentences`) are subtitles only; they say which cutscenes
  she is in: 23, and each has a dialogue track under that name.
- **Sam's situations**: `ds/entities/player/sentence/ds_player_sentence_resident` (423 situations, 2,702 player
  sentences pointing at his lines) and `..._priority` (priority, IsDialogue, IsReactionVoice). docs/audio-research.md
  sections 3 and 12 describe how the game plays them.

### The decoder

`vc_vorbis.py` is a Vorbis I decoder written from the specification, with the Wwise stripping undone as ww2ogg
describes it: packed codebooks, the implicit setup fields, the mode-only packet header (a long block's window shape
comes from its neighbours' modes). Floor 1, residue 0/1/2, channel coupling, an IMDCT through a DCT-IV and an
N/4-point FFT, the power-sine windows and the overlap-add. Checks:

- The unit tests encode a known signal with a forward MDCT (scale 4/N) across short/long block changes, through the
  same packet format, and decode it back: the error is the lattice's rounding (max 0.0016 with a step of 6e-5, and
  16 times more with a 16 times coarser lattice), so windows, overlap and transform are exact.
- On the game: a terminal line decodes to exactly its `sample_count` (90,377 samples, `LengthInSeconds` 1.883 s),
  with speech-like energy and no steps at the block joins (the sample jumps at the 157 joins match the average).
  Voice lines peak around -8 to -11 dBFS; Sam's effort grunts measure about -26 dBFS loudness.
- Speed (Python): a short line decodes in about 0.1 s, a 4-minute cutscene in about 25 s (only the three channels the
  mix uses are transformed).

## Wiring the clips into the game: not done yet

The mod plays all of its sounds through the game's own Wwise today (partner_sound / partner_voice, events and
sentences); it has no way to play a file of its own. What the next step needs:

1. **Load**: read `voices.txt` and the WAVs at startup (guest side) and refuse other versions, as the textures do.
2. **Choose**: where `self_voice` already silences Sam's talk for a non-Sam player (the `MsgDSStartTalk` filter,
   audio-research.md 18), take the situation hash of that talk (the voice queue entry; the animation voice events
   reach it too), map it with the `situation` rows to a category and pick a clip (random, not the last one).
3. **Play** through the game's mix, so 3D, reverb, pause and the volume sliders apply: post the voice preset event
   with an `AkExternalSourceInfo` (cookie from the sentence's preset, codec PCM, the clip in memory as a PCM WEM) on
   the emitter the partner voice already uses. The exact PCM WEM layout Wwise 2019.2 accepts in memory, and the
   event to post, need a hidden run to confirm. The fallback is a plain XAudio2 voice, outside the game's mix.
4. **Partner**: send the clip (category and index) over the link as a new speech-frame kind, so the host hears
   Fragile too; both players need the same clip set (or the frame carries the PCM).
5. **Launcher**: a "Fragile's voice" option (off / clips) and a pointer to voicecut.

## Tests

`st_voicecut_tests` in ctest (`python -B -m unittest discover -s tools/voicecut -p "test_*.py"`, about 1.5 s):
bit reader, Huffman codewords (the specification's own example), packed codebooks, float32 unpacking, IMDCT against
the formula, window power complementarity, the floor line, the encode/decode round trip, the codebook table in a
synthetic PE image, every parser on cores built field by field (including a stream name followed by letters), the
catalog on a fake game, the render, loudness, WAV, the store's checks, the export (and that it only removes its own
files), the API, and the HTTP guards (Host check, the X-Voicecut header for writes). No game data and no game run.

## Not verified / limits

- 371 of Sam's 423 situation names are unknown (the game keeps 31-bit hashes); they start as "other" or "random line"
  guesses. The label shows the hash; sorting them is done by listening.
- A cutscene track has every character's voice; voicecut cannot tell Fragile's from the others, the subtitles listed
  above the waveform are the guide.
- Cutscenes where Fragile speaks without a subtitle line are not listed (the list comes from her subtitles).
- Only Fragile and Sam are scanned. Mama's and Amelie's non-verbal sets (audio-research.md section 4) could be added
  as speakers in `vc_game.SPEAKERS`.
