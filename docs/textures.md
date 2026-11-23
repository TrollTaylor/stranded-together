# Fragile's textures: paint her suit and boots yourself

Fragile's porter suit and boots are pictures (textures) inside the game's data. The launcher copies them out of **your
own installed game** into a folder of ordinary PNG files that you can open in any paint program (Paint.NET, GIMP,
Krita, Photoshop...). You paint; Stranded Together shows your pictures on the guest's Fragile.

Since v0.24.0: **her suit and her boots wear your edited pictures in the game**, on your own screen and on your
partner's.

Since v0.25: **every Sam suit colour has a ready-made Fragile colour**. With nothing painted, Fragile's navy fabric
takes your Sam's suit colour (01 Orange .. 14 Pink), or a gold / silver metal finish (15 Silver, 16 Gold); her prints,
piping and straps stay as they are. The mod makes these on your PC from her own pictures (no picture is shipped with
Stranded Together). Paint a colour folder and that folder is yours (below, "Ready-made colours").

Also since v0.25: **the launcher extracts the pictures by itself**, in the background, the first time it starts (and
after a Stranded Together update that changes them), so the ready-made colours work without pressing anything; and gold
and silver are a **polished metal** like Sam's (below).

## See your pictures in the game

1. Paint in the folder of the suit colour your Sam wears (for example `Fragile porter suit\16 Gold\Suit - Colour.png`)
   and save it with the same name (PNG is best; another format under that name is read too: "Sizes and format").
2. Start a co-op session (or Local test) as the guest, playing Fragile. A few seconds after the world is there, her suit
   shows your picture. Save the picture again while the game runs: she changes a few seconds later.
3. Change your Sam's suit colour in the game's uniform menu: Fragile switches to that colour's folder. A folder you never
   painted shows the ready-made colour of that Sam colour (00 Default: her own navy suit).
4. Your partner (the host) sees the same Fragile. The first time, your pictures travel over the link: about one minute
   per edited 2048 picture (Colour or Roughness) while you both play, faster while one of you is at the title or loading.
   Meanwhile your partner sees her previous look. After that they are kept (`cache\partner-textures` in the partner's
   Stranded Together folder, at most 256 MB), so switching back is instant.

## Her boots

By default Fragile keeps **her own boots** on both screens (launcher: Tools > Fragile's textures > **Her own boots,
painted**), whatever boots the guest's Sam has equipped. They wear your pictures of the **boot type the guest's Sam
wears**:

1. Paint `Fragile boots\NN <boot type>\Boots - Colour.png` (and Roughness / Reflectance if you like). The boots
   pictures are laid out like her own boots (`Fragile porter suit\Original\Boots - *.png`), not like Sam's.
2. Equip that boot type on the guest's Sam (the game's own equipment menu): a few seconds later her boots change, on
   both screens. Another type: that type's folder. A folder you never changed: her own boots.
3. Worn-out boots (a broken shaft or a loose sole on Sam's boots) use `Worn\` inside the type's folder, if you painted a
   picture there; the gold / silver suit uses `Gold\` / `Silver\` the same way. Otherwise the type's folder.

The trade-off, plainly: her boots always keep the **shape** of her own boots (her model's). Chiral, terrorist or mule
boots, and broken boots, change only their look, never their shape: paint the look you want for each type.

The other choice, **Sam's boots on her feet**, draws Sam's real boots of the equipped type (their own shape and wear)
over her feet, as v0.22 did: your boots pictures are then hidden under them. With it, bare feet, sole grass and intact
All-terrain boots Lv1 (the same design as hers) still show her own boots. session.ini: `boots_look=own` (default) or
`boots_look=prop`; the guest's choice is what both screens show.

In the private room Fragile wears her black suit by default (launcher: Tools > Fragile's textures > **Fragile in the
private room**, session.ini `room_outfit=black|suit`; docs/real-partner-body.md section 34). Your suit pictures are for
her porter suit: with **Black suit** they are not seen in the room; pick **Porter suit** to keep them there.

Which boot type is which folder (the game's EDSPlayerShoesType): `00` no boots, `02` sole grass, `03` porter boots,
`04`-`06` all-terrain boots Lv1-3, `07` mule boots, `08` terrorist boots, `09` chiral boots, `10` reflect boots.

What counts as "edited": a picture whose bytes differ from what **Extract textures** wrote. The pictures of one
texture are rebuilt together: Colour and Roughness are one game texture, Reflectance and Ambient occlusion another,
Normal and Mask one each; a texture you did not touch stays hers. Her boots are three textures: Colour; Ambient
occlusion + Roughness + Reflectance; Normal.

The game textures the mod builds from your pictures are cached in the mod's own `cache\built-textures` folder (next to
its log, at most 256 MB; never in your textures folder): the next start is instant. Delete it any time. A picture
larger than twice its map (at most 8192 pixels a side) is skipped with a log line before it is read; **Check my edits**
says so too.

**Settings** (session.ini; the launcher writes the folder): `fragile_textures=0` turns all of this off (no loading, no
sending). `textures_send=0` (guest): your partner sees her own suit. `textures_receive=0` (host): you see her own suit.
`textures_hot_reload=0`: saved pictures are read only at the start. `textures_free=0`: textures the mod made stay in video memory for the session (by default one nobody wore for 5 s is released). `fragile_suit_colour=1`: the v0.23 colour tint for
the colours you did **not** edit (off by default now; an edited colour always wins).

If nothing shows: the game's log (`stranded_together\stranded_together.log`) has `[tex]` lines: `ready: manifest ...`
(the folder was found), `look ... for colour N (...): K slot(s) edited`, `boots look ... for boots N: Fragile boots\...
(B1 B2)`, `body self ...: look ... bound`; `[boots] self view: her own boots, Sam's hidden`; `[tex] off:
...` says why it stopped (for example "textures were extracted by another version: press Extract textures").

## Get the pictures

The launcher does it by itself: when it starts and the textures folder was never extracted (or was extracted by an older
Stranded Together), it runs the same extraction as the button below in the background (about 8 seconds; the folder
then holds about 275 MB with the default settings, measured: the card's estimate of 330 MB leaves some room; the card
shows the progress as "Extracting automatically"). Nothing waits for it: Host, Join and Local test start whenever you
like.
- **If the game starts before it finishes**: Fragile wears her own suit (the mod logs `[tex] no .manifest.txt in the
  textures folder (not extracted yet ...)`). While an extraction writes, the mod waits for it (`[tex] the launcher is
  extracting the textures: her looks wait for it to end`): the look she has stays. When it ends she wears the
  ready-made colour a few seconds later, without a restart (proven in the hidden run `fragile-suit-defaults-auto`).
  With `textures_hot_reload=0` the colours she already wore before the extraction keep her own suit until the next
  start; a colour she puts on afterwards is ready-made at once. The automatic extraction itself never starts while a
  game of this folder runs (or Local test, or a session is starting): it waits until the game is closed.
- It never changes a picture you painted (the extractor's own rules, below), and it does not run:
  - in a folder OneDrive syncs (about 275 MB would be uploaded: the card says so; the button still works);
  - in a folder the mod cannot use;
  - with less than 1.4 GB free on the folder's drive (the extraction's own 400 MB floor plus 1 GB left for Windows and
    the game; the card says how much is free; the button keeps the 400 MB floor);
  - when the folder's index (`.manifest.txt`) does not read (damaged, or written by a newer Stranded Together): an
    extraction then has no record of which pictures you painted, so it would keep unpainted older pictures as yours for
    good. Press Extract textures if you want it anyway. An index another program holds for a moment is just looked at
    again a few seconds later;
  - in a folder this launcher saw extracted that has lost its extraction since (you deleted it to free space, or its
    drive is not there): the card says so, and Extract textures fills it again (`"textures"."extracted"` in the settings
    remembers the folder).
- **When it fails** (the folder holds someone else's files, a game update the extractor does not know) the card shows
  one quiet line with the reason and it tries again 10 minutes later, then 1 hour later; after the third failure it
  stops trying by itself until Stranded Together or the game is updated (or another folder is picked), or you press
  Extract textures (which starts from zero). The record is kept in the launcher's settings (`launcher-settings.json`
  `"textures"."auto"`, by this Stranded Together version, the game's `ds.exe` and the folder), so starting the launcher
  again does not try again at once. Cancel stops it until the launcher starts again (the cancel's line says so).
- **Pictures another program has open**: the extraction waits up to 3 seconds for each (the mod's own reads never stand
  in the way: they allow the file to be replaced). An unpainted picture still held after that is kept as it is, the card
  says "could not be refreshed", and the index names the older extraction, so the next extraction (the automatic one
  10 minutes later, like after a failure) finishes it. Reference pictures that are already byte for byte the same are
  not rewritten at all.
- **One extraction at a time per folder**: a running one holds `.extract.lock` in the folder (hidden; Windows deletes
  it when the extraction ends, also after a cancel or a crash). A second one (another launcher install, the button
  during an automatic run) stops with "Another extraction is writing into this textures folder right now".
- **Turn it off**: untick **Extract automatically when needed** on the card (`"textures"."auto_extract": false`). A run
  in progress then finishes (the card says so); no new one starts.

By hand:
1. Open the launcher, go to **Tools** and find the **Fragile's textures** card.
2. Press **Extract textures**. It takes about 10 seconds and needs up to 330 MB of disk space (about 275 MB with the
   default settings). The game must be closed.
   The game's own files are only read, never changed.
3. Press **Open folder**.

The default folder is `Documents\Stranded Together\Textures`. When your Documents folder is synced by OneDrive (or is
a network folder) the default is `%LOCALAPPDATA%\Stranded Together\Textures` instead, so the extracted game pictures
and your edits are never uploaded; the card says so. **Change folder...** picks another one (not inside the game folder
or Program Files; a folder inside OneDrive gets a warning on the card); **Use the default folder** goes back. A folder
that already holds other files and was never extracted into is refused: pick an empty or new one. A folder the mod
cannot use (a network path) is named on the card and left out of session.ini: hosting, joining and Local test still
start, only without textures.

Press **Extract textures** again whenever you like (after a game update, or to get back a picture you deleted). It
**never overwrites your edited pictures**. Original\, Sam's folders and README.txt are references rewritten by every
extraction (README.txt is read-only: keep your notes in a file of your own). After a Stranded Together update that
changes the extracted pictures, the launcher extracts again by itself (the manifest's `extract` version; with the
automatic extraction off, the card asks for it). If a game update
moved Fragile's own textures, the extraction stops with a sentence (`The game's data changed ...`) instead of writing a
folder the mod could not use.

## What is in the folder

```
Textures\
  README.txt                       the same explanations, short
  Fragile porter suit\
    Original\                      her own pictures (read-only, rebuilt by every extraction)
    00 Default\ 01 Orange\ 02 Red\ 03 Charcoal\ 04 Sand\ 05 Light grey\ 06 Brown\ 07 Lavender\ 08 Plum\
    09 Mustard\ 10 Brick\ 11 Grey\ 12 Olive\ 13 Sky blue\ 14 Pink\ 15 Silver\ 16 Gold\
        Suit - Colour.png  Suit - Roughness.png  Suit - Reflectance.png   <- yours to paint
        Sam's colour.png                                                 <- that Sam suit colour, as a swatch
  Fragile boots\
    00 No boots (barefoot)\ 02 Sole grass\ 03 Porter boots\ 04 All-terrain boots Lv1\ 05 ... Lv2\ 06 ... Lv3\
    07 Mule boots\ 08 Terrorist boots\ 09 Chiral boots\ 10 Reflect boots\
        Boots - Colour.png  Boots - Roughness.png  Boots - Reflectance.png  <- yours to paint
        Gold\  Silver\  Worn\                                               <- optional variants (empty)
  Sam porter suit\                 every Sam suit style, to copy details from (read-only)
    Palette colours (00-14)\       his light grey suit the game tints; Previews\ shows each colour roughly;
                                   Colour swatches.png shows the 15 colours with their numbers
    15 Silver\  16 Gold\  Dark (cutscene only)\
  Boots (Sam, reference)\          every Sam boot type (read-only): Porter boots, Porter boots v01, Mk2 boots, Mk3 boots,
                                   Mk3 boots gold, Mk3 boots silver, Mule boots, Terrorist boots, Bandaged feet
  .manifest.txt                    a hidden index the mod reads (which picture is which game texture); do not edit
```

### Which folder is used

- **Suit**: the folder that matches the **Sam suit colour** of the guest (the uniform colour the guest's own Sam wears in
  the game's uniform menu). Sam in gold -> `16 Gold`, Sam in the default navy -> `00 Default`, and so on.
- **Boots**: the folder of the **boot type** the guest has equipped. Inside it, `Worn\` is used while the boots are worn
  out, `Gold\` / `Silver\` while the suit is gold / silver, each only if you put an edited picture there.
- Only the **two-digit number** at the start of a folder name counts: rename `09 Mustard` to `09 My yellow` if you like.
- `00 Default` starts as an exact copy of Fragile's own pictures; `01`..`16` start with the **ready-made colour** (next
  section). A folder you never painted in shows the ready-made colour (00 Default: her normal look).

### Ready-made colours

- `01 Orange` .. `14 Pink`: her navy fabric (found by its colour: blue-ish, not too dark or bright, not saturated) in that
  Sam suit colour, scaled so its average is exactly the colour of `Sam's colour.png`; folds and seams keep their shading.
  Only `Suit - Colour.png` differs from her own.
- `15 Silver`, `16 Gold`: a polished metal like Sam's. A dark base that keeps her folds (Sam's own metal Colour: gold
  (40,36,31), silver (40,40,41)), a tinted shine in `Suit - Reflectance.png` (Sam's metal shine: gold (232,185,118),
  silver (230,231,231)) and her `Suit - Roughness.png` x 1.15 on the fabric (on her suit a lighter Roughness looks
  shinier). Development builds before the polished metal had a satin prototype (Roughness x 0.75) that looked duller
  than Sam's metal side by side in the game; it is still there for comparisons (hidden `tex 11 3`). A folder that still
  holds those untouched satin pictures (extract version 2) already shows the polished metal in the game, and the next
  extraction (automatic) puts it in the folder.
- They are made on your PC from `Original\` (by the extractor for the folder, by the mod for the game): nothing of the
  game is shipped with Stranded Together, and the same game gives the same pictures on every PC.
- **Fragile wears the ready-made colour of every folder you have not painted in**, whatever the folder's pictures are
  (unchanged copies from v0.24, an older ready-made colour, or no pictures at all): updating Stranded Together updates the
  look at once, even before you extract again.
- **Paint any picture of a folder and the whole folder is yours** (any of the six pictures the mod reads: also an
  `Ambient occlusion`, `Normal` or `Mask` picture you copied in from `Original\` and painted): she wears exactly its
  pictures (`Original\` for one that is not there), and Extract textures never changes or adds a picture in that folder
  again. A part of a painted folder that you did not paint and that still holds this version's ready-made pictures is
  still sent to your partner as a recipe (the mod checks that it is byte for byte the ready-made one).
- To get a folder's ready-made colour back: delete the pictures you painted in it (the game shows the ready-made colour
  a few seconds later), then press Extract textures to see its `Suit - ...` pictures in the folder again.
- To give Fragile her own navy suit in one colour: copy the three `Suit - ...` pictures from `Original\` into its folder.
  The folder may be renamed (`16 Gold navy`): its number keeps it apart from a fresh one.
- **Both players' textures should be extracted** (the launcher does it by itself; or Extract textures). Ready-made
  colours are then not sent over the link: your partner's game makes the same ones from its own copy of the game in
  about a second, checks them, and only asks for yours when they differ (another game version). When your partner's
  textures are not extracted (the automatic extraction off or not finished yet, or v0.24), each ready-made colour comes
  over the link like a painted picture: about a minute the first time (then it is cached). The host's game says so once
  (`[tex] host: the partner's ready-made colours come over the link ...` and a note on the screen).
- The card's **READY-MADE COLOURS: On / Off** (launcher settings `"textures"."defaults"`, default on) turns them off:
  session.ini then gets `textures_defaults=0` and an unpainted folder is her own suit (the v0.24 rule). Painted folders
  are worn either way.

### What each picture does

| Picture | What it is | Notes |
|---|---|---|
| Colour | the paint: fabric colour, prints, stripes | sRGB colour |
| Roughness | grey: on her suit the game uses it as gloss: lighter = shinier, darker = more matte | measured during development: x 0.5 on her fabric looked duller, x 1.15 shinier; Sam's own gold is lighter on the metal (about 92) than on the cloth (about 78) |
| Reflectance | the colour of the shine | cloth is dark grey here. **Gold and silver live in this picture**: a dark Colour, a bright tinted Reflectance and a lighter Roughness make metal. Compare `Sam porter suit\16 Gold\` with `Palette colours (00-14)\`. |
| Ambient occlusion | grey: baked shadow in folds and seams | white = no shadow |
| Normal | the small bumps (seams, weave) | red/green = the tilt; edit with care |
| Mask | material regions for the game's detail layers | |

Normal, Ambient occlusion and Mask are not copied into the colour folders. To change one, copy it in from `Original\`
(same file name) and paint it there.

### Sizes and format

- Her suit's Colour, Roughness and Normal are **2048 x 2048**; its Reflectance, Ambient occlusion and Mask **1024 x 1024**.
  Her boots' pictures are **1024 x 1024**. Keep the size; a picture of another size is resized (the launcher's **Check my
  edits** tells you).
- Save with the **same file name** (`Suit - Colour.png`...). **PNG** is best: it keeps every pixel. Transparency is
  ignored.
- Another picture format saved under that `.png` name is read too, by what is inside the file, not by its name: JPEG,
  BMP, TIFF, GIF (its first frame) and WebP (when Windows has its WebP reader, built into current Windows 10 / 11). Many
  image tools and AI image generators save a JPEG even when the name says `.png`; the mod reads it anyway, says so in its
  log (`[tex] ...\Suit - Colour.png is a JPEG saved with a .png name: read anyway`) and resizes it like any other size.
  **JPEG is lossy**: its blocky artefacts and softened edges end up on her suit, so save as PNG when your tool lets you.
- A picture named `Suit - Colour.jpg` (or `.webp`...) is **not** read: rename it to `Suit - Colour.png`. **Check my
  edits** names such files, and says which pictures are a JPEG (or another format) under a `.png` name.
- Windows hides file name extensions by default: a `.jpg` renamed in Explorer to `Suit - Colour.png` is really
  `Suit - Colour.png.jpg`, and is **not** read. **Check my edits** names it (`... is not used: its full name ends in .jpg
  (Windows hides that ending): rename it so the name is exactly Suit - Colour.png`); turn on View > Show > File name
  extensions in Explorer to see and fix the name.

### Copying from Sam

Sam's pictures are laid out for **his** body, not Fragile's: you can copy colours, patterns and small details (a logo, a
stripe, a metal finish), not whole pictures. Her boots always keep the **shape** of her porter boots; only their look
changes, so `Boots (Sam, reference)\Mk3 boots` is a colour and detail reference for `Fragile boots\09 Chiral boots`.
Which Sam boots each type uses (the shape you would see with **Sam's boots on her feet**): porter and mule boots the mule
mesh (`Mule boots`), All-terrain Lv1 / Lv2 / Lv3 the Mk1 / Mk2 / Mk3 boots (Lv1 is `Porter boots`: her own boots' design),
chiral and reflect boots Mk3, terrorist boots `Terrorist boots`.

The sea otter hood's normal maps use a format the extractor does not decode (BC6H), so those three pictures are missing.

## Check my edits

**Check my edits** lists which folders you painted (the ready-made pictures are not edits: with nothing painted it says
"No painted pictures: Fragile wears the ready-made colours (00 Default: her own suit)."), which pictures have another size than the original, which pictures
are another format under a `.png` name (read anyway; JPEG is lossy), which files cannot be read, and files the mod does
not use (a wrong name, a `.jpg` / `.webp`... name: rename it to `.png`, or a name with a hidden second ending such as
`Suit - Colour.png.jpg`). It also says when the game was updated after the extraction, and when colour folders still show
an older extraction's pictures ("N colour folders still hold pictures from an older extraction (her own suit, or an older
ready-made colour): Fragile already wears the current ready-made colour; Extract textures puts it in the folder"). A
renamed folder is matched by its number, as the mod does.

## Resetting

Your painted folders are never changed. Pictures you did not touch in `01`..`16` are refreshed with the newest
ready-made colour by Extract textures (only while they are still byte for byte what the extractor wrote there). To reset
one picture of a folder you have not painted, delete it and press **Extract textures**: it comes back as the folder's
ready-made picture (00 Default and the boots: Fragile's original). A painted folder gets nothing back: copy the picture
you want from `Original\` (her own), or delete every picture you painted there to get the whole ready-made colour back.
`Original\`, `Sam porter suit\` and `Boots (Sam, reference)\` are rebuilt by every
extraction (their files are read-only, so a paint program warns before saving over them).

## Sharing

These pictures come from your copy of the game. Keep them for yourself: do not upload or distribute them. In a co-op
session the mod sends your edited pictures to your partner (who owns the game too) so they see the same Fragile; a
ready-made colour is only named (a "recipe"): your partner's game makes it from their own copy.

## For developers

- Code: `native/textures/` (`st_textures`: BCn codecs, PNG through WIC (`png_wic.hpp`: `SniffPicture` / `PictureSize` /
  `DecodePicture` read PNG, JPEG, BMP, TIFF, GIF and WebP by their first bytes, each with its own container's WIC decoder,
  the size from the header before any pixel is decoded), the slot table `tex_slots.hpp`, the manifest;
  `st_textures_extract`: the read-only Decima archive reader, the game's `oo2core_7_win64.dll` loaded by full path, the
  extraction plan) and `native/launcher/core/core_textures.*` (the card's child process, the scan, settings).
- The launcher runs the extraction in **its own exe as a child**: `StrandedTogether.exe --extract-textures --game-dir
  <game> --out <folder>` (no window, one line per step on stdout: `progress <done> <total> <what>`, `set <name>: N maps,
  X MB`, `skip <what> <why>`, `kept <file>`, `kept <folder> (your pictures)`, `default <file>` (a ready-made picture
  created), `replaced <file>` (an unedited older one refreshed), `note ...`, `defaults <written> <replaced> <yours> <ms>`,
  `done <written> <kept> <bytes> <ms>`, or `error <sentence>`).
  `--out` must be the folder the launcher's settings name; tests use `--allow-out-under <dir>` (never the user's
  Documents, Desktop, Pictures, OneDrive, or the user profile outside the temp folder). `--check-textures --out <dir>`
  prints the Check my edits report. `--no-previews` skips Sam's palette previews; `--cancel-after-sets <n>` (tests) ends
  the process in the middle of set n.
- session.ini `textures_dir=<folder>` (written for every session and both Local test games when the mod can use the folder,
  `[config] textures_dir=set (N bytes)`. Hidden runs never get the player's folder (`HiddenIniFromLauncher` drops it).
- Tests: `st_texture_tests` (codecs, mips, packing, slots, manifest, PNG, any picture by its content (a JPEG written
  under a `.png` name, BMP, TIFF, GIF, the size cap from the header), a synthetic encrypted archive, the whole extraction
  on a synthetic game folder, Check my edits with a JPEG under a `.png` name), `st_launcher_core_tests` (`TexturesSettings`), the hidden check
  `tools/testenv/Test-TextureExtract.ps1` (docs/hidden-testing.md).
- In the game (T2): `native/mod/texture_looks.*` (the worker: manifest, edited pictures, packing, BC encode, the blob
  `native/textures/tex_blob.*`, the mod's `cache\built-textures` cache, hot reload), `native/mod/fragile_textures.*` (the game side:
  the game's own texture loader, the per-drawable override, the detoured override rebuild that keeps our texture on her
  suit's slots only), `native/mod/texture_share.*` + `texture_wire.*` (STT1 on the link's channel 5). The `tex` debug
  action (docs/debug-actions.md). Details: docs/real-partner-body.md section 29.3.
- Her boots (T3): `native/mod/boots_look.hpp` (the style, the boots' ArtParts bits, the type -> folder, Worn / Gold /
  Silver; `st_boots_look_tests`), `texture_looks.cpp` `BuildBoots` (the boots look per type, worn and metal),
  `partner_gear.cpp` (Sam's boots entity hidden under the self view, the partner's boots prop only in the prop style),
  STT1 LOOK flags bit 0 (her own boots), bit 1 (the guest's `fragile_suit_colour`, so the host draws her own-look tint
  `two-games-fragile-boots-own`, `two-games-boots-prop`, `fragile-boots-types`.
- Tests: `st_texture_wire_tests` (the blob and its strict validator, STT1, reassembly, pacing); hidden scenarios
  `fragile-textures-probe`, `fragile-textures-solo`, `two-games-fragile-textures`, `two-games-fragile-textures-reload`
  (docs/hidden-testing.md "Fragile's textures in the game"). `st_textool pattern` paints the scenarios' generated test
  patterns; `st_textool copy` (scenario `textures copy <NN folder>\<picture>.png from <file>`) copies a picture file of
  any format under one of those names.
- Design and research: the round 6 notes (`design-final.md`), docs/real-partner-body.md section 29.

### Ready-made colours (round 8)

- The generator: `native/textures/suit_defaults.{hpp,cpp}` (`st::tex::defaults`, part of `st_textures`; the palette is
  `native/mod/suit_colour_math.hpp` `kLut`, the colours `Sam's colour.png` shows). Pure integer code, no float: two PCs
  and two compilers give the same bytes. `kGeneratorVersion` (2 since round 9; 1 in round 8) goes into the cache keys and
  the RECIPE; bump it when any output pixel can change (and `kExtractVersion` with it, so the folders are refreshed, and
  say in `extract_plan.cpp` `ReadyMadeCurrentSince` which colours changed: a `static_assert` asks for it).
  1. Fabric: `d = b - r`, `L = 2126 r + 7152 g + 722 b`, spread = max - min; fabric when `10 <= d <= 70`, `b < 190`,
     `350000 < L < 1750000`, spread `< 90`. n, the channel sums and the sum of L over the fabric. Refused under 1% fabric.
  2. A 5x5 median of the 0/255 fabric mask (255 when 13 of the 25 edge-replicated neighbours are), then Pillow 12's
     GaussianBlur(1.2) (an extended box blur: 3 horizontal, then 3 vertical passes of
     `(4026532 (left + right) + 8724152 centre + 2^23) >> 24`, a u8 store each).
  3. `rel16 = clamp(L n 65536 / sum L, 29491, 104857)` (0.45 .. 1.6 in Q16); `q = min(255, rel16 >> 9)`; the mask and q
     resized 2:1 with Pillow's LANCZOS (22-bit tables, horizontal into a u8 temp, then vertical).
  4. Palette 1..14 (T = kLut[c], S = the fabric's channel sums): `out = min(255, C (( 255 - m) S + m T n) / (255 S))`.
  5. Metals 15 / 16 (variant 0, the product since generator 2: gold base (40,36,31) shine (232,185,118), silver
     (40,40,41) / (230,231,231), roughness x1.15 on the fabric): Colour `(C (255 - m) 65536 + B rel16 m) / (255 65536)`,
     Roughness `clamp(R (255000 - (1000 - p) m) / 255000, 0, 255)` in signed 64-bit (p = `rough_permille`, 1150 for
     variant 0: x1.15 where the mask is full, clamped at 255, never wrapped), Reflectance (half size)
     `v = clamp(10880 + 15 rel_s, 8960, 14080)`, `(F (255 - m_s) 12800 + S v m_s) / (255 12800)`. Variants (`kMetalVariants`
     4, the same constant in `texture_wire.hpp`, tied by a `static_assert`): 1 "Sam's hue", 2 "Sam measured", 3 the round-7
     prototype (gold base (58,51,48) shine (240,190,100), silver (58,58,60) / (235,235,235), p 750): generator 1's
     product, byte for byte (its golden hashes are round 8's variant-0 hashes), kept for comparisons.
  The Python model of this spec (which matches Pillow 12.2's MedianFilter(5), GaussianBlur(1.2) and LANCZOS bit for bit)
  gave the golden hashes in `tests/suit_defaults_tests.cpp`, for every variant. Round 9's model (`golden9.py`: numpy
  integers, C++'s truncating division for the signed Roughness) reproduces round 8's hashes exactly for variant 3 and
  gives the new variant-0 hashes; the C++ output matches all of them (`st_suit_defaults_tests`). Against the round-7 float prototype on the real pictures the
  difference is at most 2 levels (Colour of 04, 05, 07, 13, 14), mostly 1, Reflectance none, Roughness 1 on 50 pixels: the
  prototype's float32 fabric mean is 0.4% off (R 69.33 instead of the exact 69.60).
- The folder rule (`native/textures/ready_made_rules.hpp`, header-only, tested): each of the six pictures the mod reads
  in a colour folder (Colour, Reflectance, Roughness, Ambient occlusion, Normal, Mask) is missing, unedited (its hash is
  the manifest's seed row for that folder NUMBER and file name; without a row, her Original's) or edited. No edited
  picture: READY-MADE (the mod wears the slots it makes from `Original\` pixels: S0 for 01..14, S0 and S1 for 15 / 16).
  Any edited picture: YOURS (every slot S0..S3 from the folder's files, `Original\` for a missing one; a slot whose
  inputs all equal Original is left out). A slot of a yours folder with no edited picture whose maps the colour changes
  (`MayBeReadyMade`) is built from the files AND made as the ready-made slot; only when the two blob ids are equal is it
  worn and announced as ready-made (a recipe). Never from `manifest.extract` alone: a painted v0.24 folder keeps its seed
  rows (copies of Original) under a current manifest. Colour 0, or `textures_defaults=0`: the v0.24 rule.
- The extractor (`extract_plan.cpp` `ColourFolders`, `kExtractVersion` 3 since round 9: 2 wrote generator 1's gold and
  silver; an unedited 15 / 16 picture of a v2 extraction is replaced, 01..14 are kept as the same bytes;
  `ReadyMadeCurrentSince(colour)` says which extract version's ready-made pictures are still current: 2 for 01..14, 3
  for 15 / 16, so Check my edits counts only those as older): per colour folder, a Suit picture is unedited
  when it equals the old manifest's seed row (found by the folder's number and the file name, `SeedOf`: a renamed folder
  keeps its history), this version's bytes, or (without a row) her Original; an Ambient occlusion, Normal or Mask picture
  there is unedited only when it equals her Original. Any other picture: the folder is left alone (`kept <folder> (your
  pictures)`, its seed rows keep their hashes, nothing is created in it). Otherwise each picture is
  created, kept, or replaced (`Out::Replace`: re-hashed just before `MoveFileExW`; kept if it changed meanwhile or is
  open). 01..16 hold the generator's PNG for the maps it changes and byte copies of Original for the rest; 00 Default
  holds copies. The generation runs on up to 4 threads (each with its own COM apartment), only for folders that are not
  already this version's (`defaults_ms` in the `defaults` line). Nothing else in a colour folder is read or touched.
  `ScanEdits` (seed rows by number too) counts `ready_made` / `stale_ready_made`; `outdated_seeds` only for 00 Default
  and the boots.
- The mod (`texture_looks.cpp`): `BuildColourReadyMade`, `BuildDefaultSlot` (the built cache key
  `stx-default|<gen>|<enc>|<slot>|<colour>|<variant>|<originals key>`, the originals key = SHA-256 of the four Original
  files' hashes, each checked against the manifest), `MakeReadyBlob` (the generator, the slot packed with her Original
  maps for the other channels, `BuildBlob`; one `ReadyCtx` per colour build or host batch decodes the Originals and runs
  the generator once for S0, S1 and H). The analysis is kept per originals key; the originals' identity H
  (`OriginalsId`: SHA-256 over the sizes and the pixel hashes of Original Colour, Reflectance, Roughness and Ambient
  occlusion) in `<cache>\originals.txt`. Log lines: `[tex] look <id8> for colour 16 (...): ready-made (S0 S1)` /
  `yours: 1 slot(s) (S0), ready-made S1`, `[tex] ready-made S0 <id8> for colour 16 made in N ms (analysis A ms, make +
  encode E ms)` / `... from the cache`, `[tex] ready-made colours off: <why>`.
- The link: STT1 kind 7 RECIPE (`texture_wire.hpp`), guest to host just before its LOOK: epoch, generator, blob encoder,
  H and `{slot, colour, variant, blob id}` per ready-made blob. The host (its worker, never the game thread; it runs on a
  host too when a textures folder is set) checks H against its own, makes the blob, checks its SHA-256 against the id,
  writes it to its partner cache and shows the look (`[tex] partner ready-made S0 <id8> made here in N ms: the same as the
  guest's (no transfer)`, `... complete for colour 16 (2 blob(s), cache hit 0, made here 2)`). Anything else (another
  generator or encoder, no textures folder, H differs, the bytes differ, `tex 12`) sends WANT as before (`... not made here
  (<why>): asking the guest for it`; within the receiver's budget, else RESULT kBudget and the previous look stays).
  Every request the worker takes gives exactly one result, also when making it throws (out of memory); results are never
  dropped. A new LOOK drops the queued makes of an older look's blobs (`DropQueuedMakes`, `... no longer made here`) and
  the pending look waits only for its own ids (`PendingMaking`), so a quick run of colour changes shows the last one
  without waiting for the others. A made blob is written to the partner cache and the cache pruned to its 256 MB cap.
  Failures the host can fix by extracting (no textures folder, Original changed, another game version) are told once
  (`[tex] host: the partner's ready-made colours come over the link ...` and a screen note). Mixed builds never get here
  (HELLO `build_mismatch`); an older decoder ignores kind 7.
- Debug: `tex 11 <v>` the metal variant (0 product (polished), 1 "Sam's hue", 2 "Sam measured", 3 the round-7 prototype),
  `tex 12 <m>` (host: 0 make here, 1 always ask the guest, 2 pretend the originals differ); `st_textool v024 | reset |
  gen1 | defaults` (docs/hidden-testing.md).
- The automatic extraction (round 9): `native/launcher/core/core_textures.*` `DecideAutoExtract` (pure: the manifest's
  facts, the setting, the folder, a running game, a cancel, the failure record and its waits, the free space, whether
  the folder was seen extracted before), `ReadManifestFacts` (an index another program holds: `open_failed`, looked at
  again; one that does not parse: `unreadable`, never extracted over by itself), `WasExtractedHere` /
  `RememberExtractedHere` (`"textures"."extracted"`: a folder that loses its index afterwards is `gone`, the player's
  call), `FreeBytesFor` / `kAutoExtractMinFreeBytes` (1.4 GB), `AutoExtractRecord` / `AutoExtractKey`
  (`"textures"."auto"` in launcher-settings.json: this build's version, kExtractVersion, the ds.exe stamp and the folder;
  a new key starts from zero); `core_model.cpp` `TickAutoExtract` (every 5 s at most, the real window only:
  `ModelStart::auto_extract_textures`) starts `TextureTasks::StartExtract`, the same child as the button. A model a test
  makes has it off unless the test sets it (`AutoExtractModel` does, on a temporary folder its fake game folder's
  settings name, and stops if the model would read another folder; the child also refuses an `--out` that is not the
  settings' folder). The card says "by itself" only for a decision that will run (`AutoExtractWillRun`). Log lines in
  launcher.log: `textures=auto-extract; reason=...`, `textures=auto-done ...`, `textures=auto-failed exit=... failures=N;
  error=...` (one line per failure). READY-MADE COLOURS: `ReadyMadeFromSettings` -> `SessionIni::textures_defaults`
  (only `textures_defaults=0` is written, for every session and both Local test games); the card's status, its intro,
  the automatic success line and Check my edits (`CheckReport(scan, ready_made)`) say what Off means.
- The extractor's safety for the automatic runs (round 9 review): `.extract.lock` (`kExtractLockName`, exclusive,
  delete-on-close) per folder, `ExtractionRunningIn`; `ReplaceFileRetrying` (MoveFileEx, then a POSIX-semantics rename
  that succeeds under a reader allowing delete, retried for 3 s); reference pictures already byte for byte the same are
  not rewritten (`ExtractStats::unchanged`); an unpainted picture that could not be replaced keeps its own hash as its
  seed row and lowers the index's `extract` to the version before that folder's current pictures
  (`ExtractStats::incomplete` / `incomplete_extract`, the child's `incomplete <n> <extract>` line; the launcher counts it
  as a failure for the automatic record). The mod (`texture_looks.cpp` `ExtractionRunning`) waits while the lock is
  held (at most 2 minutes): no look is built and no host make runs against a half-updated folder.
- Tests (round 9): `st_launcher_core_tests` `AutoExtractRules` (never extracted, an older version, another table, an
  unreadable index, current or newer, the setting off, no folder, OneDrive, busy, cancelled, a running game, the waits
  after 1 / 2 / 3 failures, a clock that went backwards, the record in the settings, the manifest facts, session.ini
  `textures_defaults=0`) and `AutoExtractModel` (the real model on a fake game folder with the real StrandedTogether.exe
  as the child and a temporary textures folder: off, on, failure 1 and 2 with their waits persisted across a restart,
  waiting for a running game, 3 failures then quiet, the button resets it, a cancel, READY-MADE COLOURS off in
  session.ini); `AutoExtractLive` measures a real first extraction and the round-8 update on a scratch folder when
  `ST_AUTOEXTRACT_GAME` and `ST_AUTOEXTRACT_UNDER` are set (never in ctest). `st_texture_tests` `ReadyMadeExtraction`
  case (j) (a round-8 folder: only 15 / 16 stale, exactly their 6 pictures replaced), (k) (the same while an image viewer
  holds one picture and the mod reads another: 5 replaced, the held one left with extract 2, the next run finishes it),
  (l) (the lock: a second extraction refused, a leftover lock harmless, nothing rewritten on a re-run), and the replace
  under a reader with and without delete sharing. `AutoExtractRules` / `AutoExtractModel` also cover: a lost
  extraction, an unreadable or held index, too little free space, the card's "by itself" wording after 3 failures and a
  cancel, one cancel line, the error shown once.
- Tests: `st_suit_defaults_tests` (fabric boundaries, median, blur constants from Pillow's formula, Lanczos tables, the
  golden hashes, uniform planes, refusals, OriginalsId, the folder rule, a bench), `st_texture_tests`
  `ReadyMadeExtraction` (first run, re-run, a v0.24 folder, a painted folder, an older generator's output, a lost manifest,
  the user's subfolder and notes, a painted Ambient occlusion only, renamed folders), `st_texture_wire_tests`
  `TestRecipe`; hidden scenarios `fragile-suit-defaults-quick`,
  `fragile-suit-defaults`, `fragile-suit-defaults-upgrade`, `two-games-fragile-defaults`, `fragile-suit-metal-tune`,
  `fragile-suit-defaults-auto` (round 9: the game started on an empty folder, the extraction while it runs, the polished
  gold and silver beside the prototype, round 8's pictures in the folder).
