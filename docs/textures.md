# Fragile's textures: paint her suit and boots yourself

Fragile's porter suit and boots are pictures (textures) inside the game's data. The launcher copies them out of **your
own installed game** into a folder of ordinary PNG files that you can open in any paint program (Paint.NET, GIMP,
Krita, Photoshop...). You paint; Stranded Together shows your pictures on the guest's Fragile.

Since v0.24.0: **her suit and her boots wear your edited pictures in the game**, on your own screen and on your
partner's.

## See your pictures in the game

1. Paint in the folder of the suit colour your Sam wears (for example `Fragile porter suit\16 Gold\Suit - Colour.png`)
   and save it as PNG with the same name.
2. Start a co-op session (or Local test) as the guest, playing Fragile. A few seconds after the world is there, her suit
   shows your picture. Save the picture again while the game runs: she changes a few seconds later.
3. Change your Sam's suit colour in the game's uniform menu: Fragile switches to that colour's folder. A folder you never
   changed means her own navy suit.
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

1. Open the launcher, go to **Tools** and find the **Fragile's textures** card.
2. Press **Extract textures**. It takes about 10 seconds and needs about 330 MB of disk space. The game must be closed.
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
changes the extracted pictures, the card asks for a new extraction (the manifest's `extract` version). If a game update
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
- Every editable folder starts as an exact copy of Fragile's own pictures. **Only pictures you changed are used**: an
  untouched folder means her normal look.

### What each picture does

| Picture | What it is | Notes |
|---|---|---|
| Colour | the paint: fabric colour, prints, stripes | sRGB colour |
| Roughness | grey: black = glossy, white = matte | |
| Reflectance | the colour of the shine | cloth is dark grey here. **Gold and silver live in this picture**: a dark Colour, a bright tinted Reflectance and a low (dark) Roughness make metal. Compare `Sam porter suit\16 Gold\` with `Palette colours (00-14)\`. |
| Ambient occlusion | grey: baked shadow in folds and seams | white = no shadow |
| Normal | the small bumps (seams, weave) | red/green = the tilt; edit with care |
| Mask | material regions for the game's detail layers | |

Normal, Ambient occlusion and Mask are not copied into the colour folders. To change one, copy it in from `Original\`
(same file name) and paint it there.

### Sizes and format

- Her suit's Colour, Roughness and Normal are **2048 x 2048**; its Reflectance, Ambient occlusion and Mask **1024 x 1024**.
  Her boots' pictures are **1024 x 1024**. Keep the size; a picture of another size is resized (the launcher's **Check my
  edits** tells you).
- Save as **PNG** with the **same file name**. Transparency is ignored.

### Copying from Sam

Sam's pictures are laid out for **his** body, not Fragile's: you can copy colours, patterns and small details (a logo, a
stripe, a metal finish), not whole pictures. Her boots always keep the **shape** of her porter boots; only their look
changes, so `Boots (Sam, reference)\Mk3 boots` is a colour and detail reference for `Fragile boots\09 Chiral boots`.
Which Sam boots each type uses (the shape you would see with **Sam's boots on her feet**): porter and mule boots the mule
mesh (`Mule boots`), All-terrain Lv1 / Lv2 / Lv3 the Mk1 / Mk2 / Mk3 boots (Lv1 is `Porter boots`: her own boots' design),
chiral and reflect boots Mk3, terrorist boots `Terrorist boots`.

The sea otter hood's normal maps use a format the extractor does not decode (BC6H), so those three pictures are missing.

## Check my edits

**Check my edits** lists which folders you edited, which pictures have another size than the original, which files
cannot be read, and files the mod does not use (a wrong name). It also says when the game was updated after the
extraction.

## Resetting

Your folders are never overwritten. To reset one picture, delete it and press **Extract textures**: it comes back as
Fragile's original. `Original\`, `Sam porter suit\` and `Boots (Sam, reference)\` are rebuilt by every extraction (their
files are read-only, so a paint program warns before saving over them).

## Sharing

These pictures come from your copy of the game. Keep them for yourself: do not upload or distribute them. In a co-op
session the mod sends your edited pictures to your partner (who owns the game too) so they see the same Fragile.

## For developers

- Code: `native/textures/` (`st_textures`: BCn codecs, PNG through WIC, the slot table `tex_slots.hpp`, the manifest;
  `st_textures_extract`: the read-only Decima archive reader, the game's `oo2core_7_win64.dll` loaded by full path, the
  extraction plan) and `native/launcher/core/core_textures.*` (the card's child process, the scan, settings).
- The launcher runs the extraction in **its own exe as a child**: `StrandedTogether.exe --extract-textures --game-dir
  <game> --out <folder>` (no window, one line per step on stdout: `progress <done> <total> <what>`, `set <name>: N maps,
  X MB`, `skip <what> <why>`, `kept <file>`, `note ...`, `done <written> <kept> <bytes> <ms>`, or `error <sentence>`).
  `--out` must be the folder the launcher's settings name; tests use `--allow-out-under <dir>` (never the user's
  Documents, Desktop, Pictures, OneDrive, or the user profile outside the temp folder). `--check-textures --out <dir>`
  prints the Check my edits report. `--no-previews` skips Sam's palette previews; `--cancel-after-sets <n>` (tests) ends
  the process in the middle of set n.
- session.ini `textures_dir=<folder>` (written for every session and both Local test games when the mod can use the folder,
  `[config] textures_dir=set (N bytes)`. Hidden runs never get the player's folder (`HiddenIniFromLauncher` drops it).
- Tests: `st_texture_tests` (codecs, mips, packing, slots, manifest, PNG, a synthetic encrypted archive, the whole
  extraction on a synthetic game folder), `st_launcher_core_tests` (`TexturesSettings`), the hidden check
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
  patterns.
- Design and research: the round 6 notes (`design-final.md`), docs/real-partner-body.md section 29.
