# The Stranded Together launcher (`StrandedTogether.exe`)

A small Win32 + Direct3D 11 + Dear ImGui window that sits in the DEATH
STRANDING DIRECTOR'S CUT game folder, checks the install, writes the session
settings for the in-game mod, starts the game through Steam, shows the link
status the mod reports and carries the Debug page (test options, live mod
log, mod on/off, test saves and the local two-player test). It updates itself and the mod from
GitHub Releases ([Updates](#updates)). It never injects anything and never writes your saves:
Debug > Test saves only reads them (copies into `stranded_together\sandbox`).
Installed like Diamond Dogs Together: extract the release zip into the game
folder and run `StrandedTogether.exe` ([packaging.md](packaging.md) builds
the zip).

## Where it lives and what it checks

The release zip is extracted into the game folder:

```
<game>\ds.exe                                the game (Steam app 1850570)
<game>\winhttp.dll                           the mod (a winhttp proxy ds.exe loads from its folder)
<game>\StrandedTogether.exe                  this launcher
<game>\stranded_together\package.json        from the zip: version, build id, SHA-256 of every file
<game>\stranded_together\README.txt          from the zip: install / play / debug / uninstall
<game>\stranded_together\THIRD_PARTY_NOTICES.txt
<game>\stranded_together\...                 written by the launcher / the mod (below)
```

On start the launcher checks, in this order:

| Check | Result when it fails |
|---|---|
| `ds.exe` next to the launcher | "Move me to your game folder" page. Steam's own copy of the game is found from the registry (`SteamPath`/`InstallPath`, the `Steam App 1850570` uninstall key), every library in `libraryfolders.vdf` and `appmanifest_1850570.acf` `installdir` (default `DEATH STRANDING DIRECTORS CUT`), and offered with "Open my game folder". |
| Folder path (not a drive root, not a junction/symlink, not too long) | "Co-op can't start here" card. |
| `ds.exe` SHA-256 = `9D5306D7…EDD2B`, `steam_api64.dll` SHA-256 = `F1EB582E…AB69` | **Warning only**: "Untested game version" banner. Hashes are cached in `launcher-settings.json` by size and write time (ds.exe is 86 MB). |
| `winhttp.dll` (`core::CheckMod`) | The Play page's mod chip and, when co-op cannot start, its card (Host/Join/Solo unavailable): **missing**: "Install the co-op mod first" with the steps. **foreign** (not a PE with the proxy's `\\.\GLOBALROOT\SystemRoot\System32\winhttp.dll.` forwarders naming `stranded_together`, or a link): "Another winhttp.dll is in the way"; the file is never renamed, replaced or deleted. **disabled** (only our `winhttp.dll.disabled`): "The mod is switched off" with **Enable mod**. **ok**: SHA-256 equals `package.json`'s `winhttp.dll` row, the exe in the folder equals its `StrandedTogether.exe` row and its `build_id` is this launcher's: chip "Mod 0.1.0". **mismatch** (ours, but any of those differ, or `package.json` is damaged): **warning only**, "The mod files don't match" card. **dev_build** (ours, no `package.json`, e.g. `tools\deploy.ps1`): chip "Mod: development build" and a one-line note. Re-checked every 2 s when the size or write time of the DLL, its `.disabled` twin, `package.json` or the exe changes. |
| Steam running with a logged-in user (`HKCU\Software\Valve\Steam\ActiveProcess` `pid` alive and `ActiveUser` != 0) | Host is disabled ("Start Steam and log in": the join code carries the host's SteamID64). Join and Solo test stay available (`steam://` starts Steam). |

## Play

* **Host**: makes a new 20-character secret, takes the own SteamID64
  (`76561197960265728 + ActiveUser`), formats the code
  `DST1-SSSSS-SSSSS-SSSSS-SSSSS-CC@<SteamID64>` with st_net's
  `stcoop::steam_code::Format`, copies it to the clipboard, writes
  `session.ini` (role=host) and opens `steam://rungameid/1850570`. The card says
  "You appear as: Sam (the host is always Sam)": a host always writes `my_character=sam`.
* **Join**: the field accepts a pasted code or a Steam invite string
  (`+stranded_join <code>`), validated live with the same parser the mod uses
  (`stcoop::steam_code::Parse`: blanks, case, dashes, O->0, I/L->1 tolerated;
  the player's own code is refused). Writes role=guest with `peer_steamid` and
  `secret` from the code.
* **The host-held save** ([coop-session.md](coop-session.md); Debug option **Host holds the save**, default on):
  Host and Join write `host_save=1`.
  * **Host**: plays on its own saves, which hold the co-op progress. Before the game starts the launcher copies the
    host's account folder (`%LOCALAPPDATA%\KojimaProductions\DeathStrandingDC\<account>`, read only; every slot,
    `profile\`, the settings files, not `steam_autocloud.vdf`) to `stranded_together\backups\<yyyyMMdd-HHmmss>\`
    (`core::BackupHostSaves`; written as `<stamp>.partial`, renamed when complete, `backup.json` inside with a `seq`
    one higher than any backup there; the newest 5 by `seq` are kept (not by the local-time name: a clock that was set
    ahead once or a DST fall-back never gets a fresh backup pruned; the one just made is never pruned, and
    `later_stamped_kept=N` in `launcher.log` says when the clock went back) and only folders with our `backup.json` are
    ever deleted; Debug option **Back up my saves before co-op**,
    default on; a backup that fails stops Host with the reason). It also hashes every file of the real save tree that
    is not a save slot before and after the game and logs the result (`host_save=checked nonslot_changed=N` in
    `launcher.log`; account folders are masked). The Host card says "Co-op saves into your own save ... A backup of
    your saves is made first".
  * **Join**: the guest's game runs only on the private folder `stranded_together\sandbox\coop-guest`
    (`core::SeedSessionGuest`): every slot folder and `incoming\` there is deleted at each session start (the last
    session's copy of the host's save is a disposable cache), another account's folder is deleted, the guest's OWN
    `profile\` and settings files are copied from its real account folder only when missing (read only; never
    `steam_autocloud.vdf`, never a slot, never another account's `profile.dat`), so settings and bindings changed
    during co-op stay in `coop-guest` and never reach the real tree; then the redirect probe. session.ini gets
    `sandbox_saves=coop-guest` and `sandbox_foreign_saves=1` with `host_save=1` (`MakeSessionIniText` refuses a
    host-save guest without exactly these two). Join refuses, before anything is deleted, when `coop-guest`, a folder
    above it or anything below its `saves\` is a link (junction, symbolic link): the wipe never reaches through a link
    into the real saves. The real save tree is hashed before and compared after the game,
    and a game that does not confirm the redirect (`sandbox-active.txt`) or that the mod refused
    (`sandbox-refused.txt`) is reported as for a test save. Without a Steam account Join refuses. The Join tab says
    what the guest's progress is: a disposable copy of the host's world, not kept; own saves never opened.
  * Off: the old mode, each player on their own save (no `host_save`, no backup, no `coop-guest`).
* **Your character** (Join tab): the character the host sees you as, written as
  `my_character=<key>` (the mod sends it as STJ1 `body_id`; see
  [real-partner-body.md](real-partner-body.md)). **A guest plays Fragile in her porter suit
  (`fragile_suit`), and only her**: the row shows her as a fixed field (a person icon and "Fragile
  (porter suit)", screenshot `05-play-join-valid`; dimmed like the list while it cannot change, connected or a
  local test running), with no list (`core::kGuestCharacter`,
  `core::GuestCharacter`). The list comes back with the Debug option **Allow other characters**
  (below; screenshot `38-play-join-characters`). With it on, the list is exactly the bodies marked `offered`
  in `native/mod/body_table.cpp` (proven in game; all 16 ids now: Sam, Bridges porter, Fragile,
  Die-Hardman, Cliff Unger, Bridges porter (no suit), Deadman, Heartman, Mama, Lockne, Amelie, Higgs,
  Bridges guard, Bridges medic, Sam (MULE suit), Fragile (porter suit)), which the launcher compiles in (`core::OfferedCharacters`), so the
  mod and the launcher share one list. The note beside it: your partner sees you as this
  character, and so do you on your own screen (the self view, Debug > "See yourself as your
  character", default on; with it off: "On your own screen you stay Sam"; with Debug > Partner body =
  Hologram it says instead that your partner sees you as a hologram). The pick is remembered in
  `launcher-settings.json` `"my_character"` and counts only while other characters are allowed (turning
  the option off and on again brings it back); a missing, unknown or no longer offered key is Fragile
  (porter suit). **Saved choices from before** (no `"character_defaults": 2` in the file): the old default
  `"sam"` (or nothing; compared without case, as the mod finds keys, so a hand-edited `"Sam"` too) becomes
  `fragile_suit`, both for `"my_character"` and the local test's
  `"test_save"` `"guest_character"`; any other pick is kept as it was (`core::GuestPickFromSettings`). The
  marker is written with the next save, so `"sam"` picked later with the option on stays `"sam"`. Echo
  (Solo test) and solo sessions carry the same character (`BaseIni`), and so does `--write-session`.
* **Solo test**: role=echo. The mod spawns a clone of the player that replays
  their movement `echo_delay_s` behind (Debug page). The launcher writes it with
  `transport=udp`, `udp_peer=127.0.0.1`, ports 27555/27556 (loopback only, no
  secret), so nothing reaches Steam or the network even if the mod opens its
  transport.
* **Advanced: connection and name**: Steam (default) or LAN / VPN (UDP) with
  the partner's IPv4 and both ports (default 27555/27555; two copies on one PC
  swap them), and an optional display name. Persisted in
  `launcher-settings.json`. In LAN mode the guest still pastes the host's code
  (it carries the shared secret).

## Debug page

Three tabs (`ui::DebugTab`):

* **Options**: **Debug tools only (solo)** (role=solo: the in-game debug menu
  and the mod log, no partner, no network; loopback UDP like the echo; it
  always writes `debug_menu=1`, whatever the checkbox below says) and
  **Echo test**; sliders for echo delay (0-10 s), echo offset (0-20 m), send
  rate (1-120 Hz) and partner interpolation delay (0-1000 ms); **In-game
  debug menu** on/off and its key (F1-F12, default F9); **Partner body**: Real
  character (experimental) or Hologram (`partner_body=real|holo`, default real: how your
  partner is drawn on your screen; the mod falls back to the hologram when the real body
  fails). Hologram also means this game sends no joint frames (no pose hook), so the partner
  sees you as a hologram whatever character you picked: the option's text says so, and with
  Hologram set the Join, Host and local-test character rows show "Partner body is Hologram
  (Debug page): your partner sees you as a hologram" (screenshot `39-play-join-hologram`); **See yourself as your
  character** (`self_character=1`, default on, needs Real character: on your own screen Sam is replaced by the
  character picked on Join; written only for a guest, echo or solo session whose character is not Sam; Sam
  comes back whenever the game hides him itself, the character is not ready or the mod stops, see
  docs/real-partner-body.md section 8.3; untick it to stay Sam on your own screen); **Show player names above heads** (`name_tags`, default on:
  your partner's name floats above their body and yours is sent to them, docs/real-partner-body.md section 10;
  unticked writes `name_tags=0`: nothing is drawn or sent); **Mute Sam's voice when you are not Sam** (`self_voice`,
  default on = `self_voice=auto`: playing Fragile or anyone but Sam, Sam's lines, grunts and breathing are not heard
  on your own game; footsteps, gear, BB and radio calls stay; unticked writes `self_voice=sam`; the setting is
  saved as `"self_voice": "auto"|"sam"` in launcher-settings.json); **Hear your partner's voice** (`voice_sync`,
  default on = `voice_sync=1`: your partner's lines, grunts and breathing play from their body when they are near
  you, and yours from yours; a partner who is not Sam is silent (only Sam's voice exists in the game's resident
  lines); unticked writes nothing, the mod's default is off; saved as `"voice_sync": true|false`;
  docs/audio-research.md section 15); **Hear your partner's footsteps and sounds** (`sound_sync`, default on =
  `sound_sync=1`: your partner's footsteps, movement, cargo and weapon sounds play from their body when they are near
  you, and yours from yours (experimental: footsteps are recognised by position); unticked writes nothing, the mod's
  default is off; saved as `"sound_sync": true|false`; docs/audio-research.md section 16); while it is ticked, the slider
  **PARTNER SOUND VOLUME** (0-100 %, default 100: `partner_volume=N` is written only below 100; 0 = your partner's
  sounds are not played; their voice is not affected; saved as `"partner_volume": N`; docs/audio-research.md section
  17.3); **Players block each other** (`partner_collision`,
  default on, needs Real character: your partner's body stops you like another character, docs/real-partner-body.md
  section 11; unticked writes `partner_collision=0`); **Allow other characters** (launcher-only, default off,
  `"debug"` `"allow_other_characters"`, never written to session.ini: off, a guest plays Fragile (porter suit)
  on Join and in the local test; on, both offer every offered character); **CO-OP SAVE**: **Host holds the save**
  (`"debug"` `"host_save"`, default on: Host and Join write `host_save=1`, see Play above; off = each player on their
  own save) and **Back up my saves before co-op** (`"backup_host_saves"`, default on, only with the first);
  **Wake the partner clone** (`partner_activate`); **Verbose mod log**; **Reset to
  defaults**. Saved in `launcher-settings.json` under `"debug"` (sliders when released)
  and written into every `session.ini`, Host and Join included
  (`core::ApplyDebugOptions`).
* **Files and log**: Open mod folder, Open log, **Copy error.txt** (the
  game's crash report next to ds.exe, enabled when it exists), **Disable mod
  / Enable mod** (renames `winhttp.dll` <-> `winhttp.dll.disabled`; only our
  DLL, never while a session runs or ds.exe runs, never over another file),
  **Clear session** (cleanup plus deleting a `session.ini` this launcher did
  not write, e.g. a persistent `deploy.ps1 -Session` file; not while the game
  runs), **Copy log**, and a live tail of `stranded_together.log` (last 400
  lines, re-read once a second, "Follow" keeps it at the end).

* **Test saves** ([sandbox-and-local-test.md](sandbox-and-local-test.md)): **Choose folder...**
  (a folder picker; the folder must hold save slots, e.g. a downloaded 100% save; remembered in
  `launcher-settings.json` `"test_save"`), **Open test-save folder**, **Reset test saves**,
  **Play with test save (your saves untouched)** (Debug tools only on the sandbox copy
  `sandbox\test`, `sandbox_saves=test` + `sandbox_foreign_saves=1`), **Also for Host, Join, Solo
  test**, the result after the game (real save tree compared by SHA-256, or the mod's refusal), and
  the **Local two-player test** (two ds.exe on this PC, started directly with `SteamAppId`,
  loopback UDP, 30 fps cap, each on its own sandbox; **Pads on the host too**; **Guest appears
  as**: the guest game's `my_character`, Fragile (porter suit) as a fixed field unless Debug > Allow other
  characters is on, the host game is always Sam; remembered in `"test_save"` `"guest_character"`) with both
  games' link state.

The Play > Solo test tab links to it; Tools points to it.

## In game

While the game starts or runs, the In game page polls `link-status.json`
(2 Hz) and shows the state, partner, ping, route (Steam relay / Steam direct /
LAN) and whether the partner's character is visible. "Cancel" works until the
game process exists.

## Updates

The launcher updates itself and the mod from GitHub Releases
(`native/launcher/core/core_update.*`, adapted from DDT's; releases are made
by `tools\release.ps1`, see [releasing.md](releasing.md)).

**Check.** Once per launch, on the first tick in a game folder, unless About >
"Check for updates when the launcher starts" is off (`auto_update_check` in
`launcher-settings.json`, default on); and on About > **Check for updates**.
It reads `https://api.github.com/repos/TrollTaylor/stranded-together/releases?per_page=20`
(`ParseReleasesFeed`) and picks the highest SemVer `tag_name` that is newer
than this launcher's version (`0.1.0+abc1234` compares as `0.1.0`), skipping
drafts, prereleases (and `-rc` style tags), and releases without exactly one
uploaded asset named `StrandedTogether-<version>.zip` whose
`browser_download_url` is `https://github.com/TrollTaylor/stranded-together/releases/download/<tag>/<name>`.
Source zips, `zipball_url` and any other asset are never taken. The asset's
`size` and `digest` (`sha256:...`) are kept for the download check.

**Banner.** Play shows a card on top (`ui::UpdateBannerFor`): "Update
available: version X" with the first lines of the release notes, **Release
notes** (the release page) and **Update now**; a progress bar while it
downloads; a spinner while it installs; "Version X was not installed" with
the reason and **Try again**; "Updated to version X" in the new window. A
failed check or "You have the latest version" only shows on About.

**Update now** (`RealModel::start_update`, on its own thread):

1. Refused with a sentence while `ds.exe` runs from the folder or a session
   is starting or running ("Quit DEATH STRANDING first...").
2. Downloads to `stranded_together\update.zip` (at most the feed's size, 256 MB),
   then checks the size and GitHub's SHA-256 digest.
3. `ApplyUpdate` checks the zip (`VerifyUpdateZip`): it opens, has at most
   64 entries, only `StrandedTogether.exe`, `winhttp.dll` and files under
   `stranded_together/` (never `ds.exe` or another game file, never a file
   the launcher or mod writes: `session.ini`, settings, logs...), its
   `package.json` is ours with the release's version, and its `files[]` lists
   every other entry exactly once with a matching SHA-256 (and size);
   `winhttp.dll` must be the Stranded Together proxy, the exe a PE.
4. Refused when `winhttp.dll` in the folder is another program's (it is never
   replaced). A mod switched off stays off: the new DLL becomes
   `winhttp.dll.disabled`.
5. Every file is written next to its target as `<file>.st-new`; then each
   old file is moved aside (`StrandedTogether.exe` -> `StrandedTogether.exe.old`,
   which works while it runs; others -> `<file>.st-old`) and the new one moved
   in. `ds.exe` is checked once more right before. Any failed move puts every
   old file back.
6. The new `StrandedTogether.exe --after-update <pid>` is started
   (`StartSuccessor`) and must set the event `Local\StrandedTogether-update-<pid>`
   within 20 s; if it exits first, or does not answer in time (it is then
   ended, so its exe can be replaced), the update is rolled back (the old exe
   comes back from `.old`) and the player gets "your current version was put back".
   While the update downloads or installs, Host, Join, Solo test and the mod
   switch are refused ("An update is being installed...").
7. The old window closes (`ui::CloseForUpdate`). The new one signals, waits
   up to 15 s for the old process to exit (ends it if it hangs), claims the
   folder's single instance, deletes `StrandedTogether.exe.old` (retrying for
   5 s) and says "Updated to version X".

Every start also deletes a leftover `StrandedTogether.exe.old` and repairs an
update that was killed half way (`CleanUpdateLeftovers`: `*.st-new` deleted,
`*.st-old` moved back when its file is missing). `launcher-settings.json`,
logs, `session*.ini` and anything else in `stranded_together\` that the zip
does not carry are left alone. Each step is one `update=...` line in
`launcher.log`; Collect logs' `summary.txt` has `update=<status>`.

**WinHTTP without importing it.** The launcher sits next to the mod's
`winhttp.dll` proxy, so `core_update.cpp` never links `winhttp.lib`: it
includes `<winhttp.h>` for types only, loads System32's copy with
`LoadLibraryExW(L"winhttp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)`,
refuses the module unless `GetModuleFileNameW` is `<System32>\winhttp.dll`
(a module of that name loaded from elsewhere would be returned instead), and
takes the eleven functions it needs with `GetProcAddress`. Requests use the
system proxy settings, follow redirects (a release asset redirects to
GitHub's download host) but never from https to http, send
`User-Agent: StrandedTogether-Launcher/<version>`, and time out after 10-30 s.

**Developer flags.** `--update-feed <url>` reads the releases from a loopback
feed (`http(s)://127.0.0.1` or `localhost` only; then download URLs must be
loopback too) and checks on start whatever the setting says;
`--update-now` (with `--update-feed`) installs a found update at once. The
restarted launcher gets `--update-feed` again but not `--update-now`.
[releasing.md](releasing.md#testing-the-updater-without-github) has the recipe.

## The contract with the mod

### `stranded_together\session.ini` (launcher -> mod)

Written before the game starts. **Without the file the mod stays idle and the
game runs vanilla.** Every launch writes `one_shot=1`: the mod renames the
file to `session.last.ini` as soon as it has read it, so starting the game
from Steam afterwards runs it unmodded even if the launcher never cleans up.
An unread file is deleted after the game exits (or after Cancel / End
session / Clear session). Plain `key=value` lines, LF, the first line a `#`
comment starting `# Generated by Stranded Together` (that header is how
cleanup recognises the file as ours, besides the recorded hash).

```
# Generated by Stranded Together. The mod renames it to session.last.ini once read; without this file the game runs unmodded.
role=host|guest|echo|solo     solo = Debug tools only
transport=steam|udp
peer_steamid=<SteamID64>        guest only (the host's id from the code)
secret=<20 Crockford base32>    host: new; guest: from the code; echo / solo: absent
udp_peer=<ipv4>                 udp only
udp_port_local=<1..65535>       udp only
udp_port_peer=<1..65535>        udp only
send_hz=30                      1..120          (Debug page)
interp_delay_ms=100             0..1000         (Debug page)
echo_delay_s=2                  0..10, the mod's range (Debug page)
echo_offset_m=1.5               0..20           (Debug page)
partner_activate=1              wake the partner clone (Debug page)
verbose=0                       (Debug page)
debug_menu=1                    the in-game debug menu (Debug page)
debug_menu_key=F9               F1..F12 (Debug page)
one_shot=1                      always: the mod renames the file to session.last.ini once read
name=<display name>             optional; printable ASCII, no # ; = quotes, <= 32 chars
sandbox_saves=<name>            Debug > Test saves: the game runs on stranded_together\sandbox\<name>
sandbox_foreign_saves=1         with sandbox_saves: skip ds.exe's save-owner check (after the redirect self-test)
appdir_settings=1               local test: the game folder's settings.cfg -> the sandbox's
second_instance=1               local test, second game: its single-instance mutex is renamed
host_save=1                     the host-held save session (Host and Join, Debug "Host holds the save", default on;
                                docs/coop-session.md). A guest with it always has sandbox_saves=coop-guest and
                                sandbox_foreign_saves=1 (the writer refuses it otherwise; the mod ends a host-save guest
                                in DllMain without a confirmed redirect: exit 0x53540006, sandbox-refused.txt). The
                                launcher never writes the mod's test key coop_test_build=<text> (hidden runs only: a
                                fake game build for the version check; ignored outside automation=1)
local_input=host|host_pad|guest local test: input split, keeps running unfocused
local_window=left|right         local test: the window takes that half of the monitor
frame_cap=30                    local test: at most 30 frames per second
my_character=<key>              the character the other player sees you as: an offered body_table key; always sam for a host,
                                fragile_suit for every other role unless Debug > Allow other characters is on
partner_body=real|holo          how the partner is drawn on this screen (Debug page; default real)
self_character=1                you see yourself as my_character too (Debug page; not for a host or sam)
self_voice=auto|sam             auto: Sam's voice is muted on your game when your character is not Sam (Debug page "Mute Sam's
                                voice when you are not Sam", default on; unticked writes sam; always written). The mod also
                                takes mute (a test value); a missing key keeps Sam's voice. docs/audio-research.md Phase 1
voice_sync=1                    each player hears the other's voice (Debug page "Hear your partner's voice", default on;
                                written only when ticked, the mod's default is off). The mod also reads voice_subtitles=0|1
                                (not written: 0, the partner's subtitles are not shown). docs/audio-research.md Phase 2
sound_sync=1                    each player hears the other's footsteps, foley, cargo and weapon sounds (Debug page "Hear
                                your partner's footsteps and sounds", default on; written only when ticked, the mod's
                                default is off). The mod also reads sound_steps=0|1 (not written: 1, footsteps by
                                position). docs/audio-research.md Phase 3
partner_volume=0..100           the partner's footsteps and sounds at this percent (Debug page slider "PARTNER SOUND
                                VOLUME"; written only with sound_sync=1 and below 100; the writer refuses values outside
                                0..100; the mod's default is 100). docs/audio-research.md section 17.3
name_tags=0                     no name tags (Debug page; written only when unticked, the mod's default is on)
partner_collision=0             you can walk through your partner (Debug page; written only when unticked, default on)
partner_gear=0                  no backpack or cargo on the bodies, none sent (mod key only, not written by the launcher; default on)
smooth_pose=0                   the bodies keep the game's 60 Hz updates (v0.7; mod key only, not written by the launcher; default on)
partner_collision_height=0      the partner's capsule at Sam's own 1.65 m, climbable as in v0.7 (mod key only; default 4.0 m: no climbing)
name_tag_font=default           name tags in Segoe UI instead of the game's menu font (mod key only; default game)
self_headgear=0                 the self view's cap and glasses go back to v0.11's head lift (mod key only; default on:
                                an offset per item for Fragile's head; docs/real-partner-body.md section 20)
self_feet_hide=0                the self view keeps the character's own feet under the equipped boots (mod key only;
                                default on; section 21)
boots_sync=0                    the equipped boots are not sent or drawn on the partner (mod key only; default on; section 21)
vehicle_sync=0|1|2|3|4|5        the partner's vehicle (mod key only). Default with host_save=1: 5 ("real", section 23):
                                every vehicle is one real game vehicle in both worlds, no copies (off with a one-line
                                reason when its byte checks fail: then 1). Default otherwise 1: solid trike and truck
                                copies with the partner's paint and packs, their collision off on each copy (you walk
                                and drive through them), no roadster and no hologram (decision D2-1); 2 the same; 3 all
                                three as holograms (opt-in); 4 the roadster's hologram only (opt-in); 0 nothing read,
                                sent or spawned; a parked vehicle stays shown while its driver is within 250 m of it;
                                sections 22.11, 22.13, 23)
vehicle_real_native=1           vehicle_sync=5 only (mod key only; default 0): the real follower vehicle's lamps and engine /
                                road sounds are the game's own only; 0 keeps the v0.14 lamp writes and the STA1
                                vehicle-sound mirror on the real vehicle until a hidden run proves the native ones
                                (section 23.16)
vehicle_overlap_hide=0          the partner's vehicle copy stays shown where your own vehicle of the same kind stands (mod
                                key only; default on: hidden there until it moves away; section 22)
vehicle_look=0                  the partner's vehicle copy keeps its own look and carries no cargo (mod key only; default
                                on: on the solid trike / truck copies the partner vehicle's paint, gold / silver / broken
                                / ride-type variations, wear (rust), lamps and battery gauge; on every copy, holograms
                                too, the cargo on it as case props; 0 turns all of it off, sent and shown;
                                sections 22.7, 22.12, 22.13)
vehicle_packs=0                 no battery / defense packs on the partner's solid vehicle copy (mod key only; default on;
                                not with vehicle_sync=3; section 22.7)
vehicle_sound=0                 the partner's vehicle sounds (engine, road noise...) are not sent or played (mod key only;
                                default on, with sound_sync=1; section 22.9)
```

`my_character` is written for every host as `sam` (whatever was asked) and otherwise only when
set; `MakeSessionIniText` refuses a key that is not an offered body (exact spelling) and a
`partner_body` other than `real` or `holo`. `self_character=1` is written only for a non-host whose
`my_character` is set and is not `sam`, and only when the Debug option is on and Partner body is Real
character (`core::ApplyDebugOptions`); the mod treats a missing key as off.

`host_save=1` is written only for a host or a guest; a guest's needs `sandbox_saves=coop-guest` and
`sandbox_foreign_saves=1` in the same file (`MakeSessionIniText` refuses anything else, and `host_save` for echo or
solo). `--write-session` follows the Debug option too (a join seeds `coop-guest` first; a host makes no backup).
The sandbox keys are written only together (`MakeSessionIniText` refuses `second_instance`,
`appdir_settings` or `sandbox_foreign_saves` without `sandbox_saves`, a name that is not
`[A-Za-z0-9_-]{1,40}`, other `local_input`/`local_window` values, `frame_cap` outside 15..240).
The local test's files are `stranded_together\instances\host|guest\session.ini` (the games get
`ST_INSTANCE_DIR=instances\host|guest`), never `stranded_together\session.ini`.

`core::MakeSessionIniText` validates every field (canonical secret, individual
SteamID64, canonical IPv4, port ranges) and `core::ParseSessionIniText` reads
it back (used by the tests and to show the host's code again after the window
was reopened). This matches `native/mod/config.cpp` `LoadConfig` (same keys,
`#`/`;` comments, BOM tolerated). A file without `one_shot` (hand-written,
`tools\deploy.ps1 -Session`) is read on every start; `ParseSessionIniText`
reads it back with `one_shot=false`. The launcher's text was fed to the mod's
real `LoadConfig` in a scratch harness: every key known, values as written,
the file renamed.

### `stranded_together\link-status.json` (mod -> launcher)

Written by the mod about once a second:

```json
{"v":1,"pid":1234,"state":"connected","transport":"steam","route":"relay","peer":"Sam",
 "rtt_ms":48,"remote":"visible","reason":"","code":"DST1-...","time_utc":"2026-11-15T12:00:00Z"}
```

* `state`: `idle|listening|waiting|connecting|handshake|connected|reconnecting|lost|rejected|error`
  * Also accepted, because the current mod writes them:
    * `off` / `disabled`: the link has not started. With a `reason`, the page says "Co-op did not start".
    * `echo`: in echo role the page only reads `remote`.
* `peer`: the mod writes `steam:<SteamID64>` or `ip:port`. The page shows a Steam peer as
  "Steam player ...1234", so the full SteamID never appears on screen.
* `remote`: `none|spawning|visible|failed`
* Anything that is not `"v":1` with a `state` is ignored.
* **Stale** when `time_utc` is more than 15 s old or `pid` is not running: the
  page then says "The game is not responding" (no ping, not connected).
* `reason` is shown as-is in lost/rejected/error/reconnecting sentences.
* While the game is still starting, a stale or foreign status is ignored.
* The launcher never reads `code` (it has its own); Collect logs redacts it.

`core::StatusViewOf(status, fresh, role)` turns one status into the headline,
detail and tone (pure, unit-tested; the mock model uses it too, so the
screenshots show the real wording).

### Session lifecycle (`stranded_together\session-record.json`)

The DDT pattern, slimmed: the record (`v:1`, product
`stranded-together-session`, `session_id`, `active`, `role`, `transport`,
`ini_sha256`, launcher and helper pid + start time) is written **before**
`session.ini`, everything under the per-folder mutex
`Local\stranded-together-play-<16 hex of SHA-256(upper-case game path)>`.

* **Activate** refuses while `ds.exe` runs from the folder, or while another
  live window/helper owns an active session, or when a `session.ini` that is
  not ours exists.
* **Cleanup** never runs while `ds.exe` runs from the folder. It deletes
  `session.ini` only when its hash matches the record or it carries our header
  (a hand-written file is left alone and reported), deletes `link-status.json`
  when its writer is gone, and marks the record inactive.
* **Helper**: after a successful activation the launcher starts itself hidden
  as `StrandedTogether.exe --watch <session_id>` (`CREATE_NO_WINDOW`,
  breakaway from the job). It waits for `ds.exe`, waits for it to exit (a
  restart within 8 s counts as the same game) and cleans up; if the game never
  starts it cleans up after 10 minutes. So closing the launcher window is safe.
* **The window** also cleans up when it sees the game exit (4 s), and on start
  it cleans up a leftover session whose game is not running and whose window
  and helper are gone (PC restarted, helper killed).
* After a one-shot launch the host's code for a reopened window is read from
  `session.last.ini` (only when its hash is the recorded one).
* **Tools > End session** does the same cleanup on request (not while the game
  runs).

Logs: `stranded_together\launcher.log` (redacted: no codes, secrets, SteamIDs,
user names or paths; rotated at 1 MB to `launcher.old.log`).
**Collect logs** makes `stranded-together-logs-<stamp>.zip` on the Desktop with
`stranded_together.log` (the mod's), the launcher logs, `link-status.json`,
the session record and a `summary.txt`, every text redacted.

## DLL-search hazard (the launcher lives next to the mod's winhttp.dll)

`ds.exe` loads `winhttp.dll` from its folder; so would any program in that
folder that imports winhttp (winhttp is not a KnownDLL). The launcher
therefore:

* does **not import WINHTTP.dll** at all: the updater loads System32's copy
  explicitly ([Updates](#updates));
* links with `/DEPENDENTLOADFLAG:0x800` (static imports resolved from
  System32 only; this also keeps the game's own `d3dcompiler_47.dll` out);
* calls `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)` first thing in
  `wWinMain` (later `LoadLibrary`/COM loads).

The ctest `st_launcher_no_winhttp_import` (`st_launcher_update_tests
--check-imports StrandedTogether.exe`) reads the built exe's import and
delay-import tables and its load config: no `winhttp*`, and
`DependentLoadFlags` = `0x0800`. Today's imports: d3d11, D3DCOMPILER_47,
dwmapi, ole32, SHELL32, USER32, GDI32, ADVAPI32, bcrypt, KERNEL32, IMM32.
An end-to-end self-update was run with an invalid `winhttp.dll` (a few bytes
that only pass the "ours" byte test) next to the exe: the launcher checked,
downloaded, installed, restarted and never loaded it.

## Command line

```
StrandedTogether.exe                      the window (real model)
  --mock [--mock-scenario <name>]         simulated model: ready | wrong-folder | not-installed |
                                          untested-version | steam-offline | partner-drops |
                                          dev-build | mod-mismatch | mod-foreign | mod-disabled |
                                          update-available (Update now simulates download/install)
  --scale <f>                             force a UI scale
  --selftest                              built-in checks (exit 0 = passed); reports, but does not
                                          fail on, a folder without ds.exe
  --screenshot-all <dir> [--scale 1]      render every page hidden and save PNGs (35 scenes)
  --watch <session>                       the hidden helper
  --after-update <pid>                    (the updater) the new window waits for the old one to close
Developer only (no window, no Steam launch, no helper):
  --write-session host|join|echo|solo [--code <code>] [--transport steam|udp]
                  [--udp-peer <ip>] [--udp-port-local <n>] [--udp-port-peer <n>] [--game-dir <dir>]
  --end-session [--game-dir <dir>]
  --update-feed <url> [--update-now]      releases from a loopback test feed; install at once
```

`--write-session` is meant for testing the mod without the window: it writes
`session.ini` (and the record) exactly as the buttons do, with the Debug
page's saved options and `one_shot=1`, prints the host code, and leaves the
file until the game reads it, `--end-session` or the next launcher start with
the game closed. `--selftest` also reports the mod state and `package.json`.

## Build and tests

`cmake/launcher.cmake` (included by the root `CMakeLists.txt`):

* `st_imgui` (static): Dear ImGui 1.92.6 + Win32/DX11 backends, `third_party/imgui` (MIT, see `UPSTREAM.txt`).
* `st_launcher_core` (static): the launcher core (with `core_update`), join code, UI logic, mock model and entries. Links `st_net` for the DST1 code (only `steam_join_code` and `link_crypto` objects are pulled in); never `winhttp.lib`.
* `st_launcher` (WIN32 exe, `OUTPUT_NAME StrandedTogether`), `/W4 /WX /utf-8 /permissive-`.
* Tests: `st_launcher_core_tests` (join code round trip, session.ini text incl. `one_shot`/debug keys/`solo`/`my_character` (host always sam)/`partner_body`/`voice_sync`/`sound_sync`/`partner_volume` (0..100, written only with sound_sync below 100)/`self_voice`
 (auto, sam, mute; anything else refused), the offered character list, status parse and view, Steam library parsing and game-folder discovery from a fake `SteamPath`, `package.json` parsing and every mod state, disable/enable, Debug options persistence, redaction, UI logic, a session activate/cleanup round trip and the real model on a temporary fake game folder with a fake Env: Debug tools only, Clear session, live log, crash report, package ok/mismatch/foreign, a window reopened after the mod consumed `session.ini`), `st_launcher_update_tests` (SemVer incl. prerelease order; the feed: drafts, prereleases, source-only releases, foreign or query URLs, two matching assets, digests, `/releases/latest`, loopback feeds; `VerifyUpdateZip`: wrong version, missing or unlisted files, hash mismatch, game or runtime files, a foreign DLL; `ApplyUpdate`: success, ds.exe running before and during, foreign DLL, a failed move half way, a failed restart, a fresh folder, a switched-off mod, leftovers; the real model on a fake feed and fake HTTP: once per launch, the setting, refused while the game runs, digest mismatch, offline, success with `--after-update <pid>`, the new window's cleanup, rollback, `--update-feed`/`--update-now`; the banner logic, command line and mock; the real WinHTTP client against an HTTP server on 127.0.0.1), `st_launcher_no_winhttp_import`, `st_launcher_selftest`, `st_launcher_screens` (fails on `imgui-error|FAIL:`), `st_launcher_assets_current` (`make_launcher_assets.py --check`).

```
cmake -S . -B build-launcher -G "Visual Studio 17 2022" -A x64
cmake --build build-launcher --config Release --target st_launcher st_launcher_core_tests st_launcher_update_tests
ctest --test-dir build-launcher -C Release -R st_launcher
```

### Logo and icon

`tools/launcher/make_launcher_assets.py` (Pillow) reads `docs/readme/logo.png`
and writes `native/launcher/resources/st_logo.png` (the emblem: pods and cable,
384 px wide) and `st_launcher.ico` (16-256 px). The black lettering of the logo
is invisible on the dark UI, so the script cuts the pods out by alpha: the
first of the alpha row runs (the lettering is the second). Icons of 48 px and
below show the left pod alone (the pair is a thin strip at that size). The
window draws the title "STRANDED TOGETHER" itself as spaced Segoe UI Light text
with faint drip lines, like the logo. `--check` compares pixels (tolerance 2).

## Reused from Diamond Dogs Together vs changed

| DDT | Here |
|---|---|
| `launcher_main.cpp` (window, DX11 + WARP, WIC logo, screenshot driver, DPI, dark title bar, single instance per folder, update hand-off) | Kept; rebranded; file dialogs and folder picker removed; `SetDefaultDllDirectories`; 820x580. |
| `launcher_theme.*` (widgets: Button, Segmented, RadioRow, Checkbox, ProgressBar, Spinner, StatusDot, cards, drawn icons) | Kept; Death Stranding palette (amber `#E8A33A` accent, LED blue `#5BB8FF` secondary, blue-grey panels); Bahnschrift titles; Segoe UI Light spaced title; icons `cable`, `echo`, `network`, `steam`, `stop`. |
| `launcher_app.*` | Same structure (sidebar, pages, cards, code card, join field, screenshot scenes); pages are Play (Host / Join / Solo test + Advanced), In game, Tools, About. Character picker, HUD, Settings, saves, uninstall and local test removed; the update card on Play and the Updates section on About kept. |
| `launcher_ui_logic.*` | Same ideas (page rules, join field, code split, ping tone, pacing, command line); new tabs, LAN checks, scenes, options. |
| `launcher_model.hpp`, `launcher_model_mock.*` | Trimmed interface; added install checks, transport/LAN, solo test, end session; mock builds its link view with the real `StatusViewOf`. |
| `launcher_join_code.*` | Same, on `stcoop::steam_code` (DST1, `+stranded_join`). |
| `launcher_entry.*` | `--watch`, `--write-session`, `--end-session`, `--selftest`, single instance, the new window's side of the update hand-off; local-test entries removed. |
| `core_util`, `core_json`, `core_env(_win32)`, `core_log`, `hash`, `file_io`, `crc32`, `inflate`, `zip` | Copied unchanged except the namespace (`ddt` -> `st`), comments and `ds.exe` as the game process. |
| `core_paths` | Same algorithm; DS constants, new file layout, `PlayMutexName` prefix. |
| `core_text` | Secret/SteamID/INI/redaction kept (DST1 and `+stranded_join` redaction, `secret=` redaction); lab.ini/coop.ini, MGSV sentences, missions and launch evidence replaced by `session.ini` and `StatusViewOf`. |
| `core_session` | Activate/cleanup/helper pattern kept on one file and a smaller record; save handoff, relaunch, marker and coop.ini removed. |
| `core_install` | The gate (hashes as warnings), `package.json` (simpler than DDT's: version, build id, file hashes) and the winhttp.dll check (ours / foreign / disabled), mod on/off, Steam account, settings and Collect logs (also `session.last.ini`, `package.json`, `error.txt`); no setup steps or uninstall. |
| `core_model` | Rewritten small (about 570 lines instead of 1,900) on the same patterns. |
| `core_update` | Same flow (feed, download, staged install with rollback, hand-off to the new exe); WinHTTP loaded from System32 instead of imported; an `HttpClient` seam; the zip verified against its `package.json` (every file) and the feed's digest; prereleases skipped; `.st-new`/`.st-old`; no GPL source-zip rules. |
| `core_saves`, `core_localtest*`, `qar`, `fpk`, `fox2`, `runtime_dat`, `ddt_datbuild`, assets | Dropped. |

### Screenshots

`st_launcher_screens` renders 40 scenes into `build-launcher\launcher-shots\`
(`01-play-host` ... `40-debug-options-characters`), among them `36-debug-test-saves`, `37-debug-local-test`,
`38-play-join-characters` (the character list open), `39-play-join-hologram`, `40-debug-options-characters`
(Debug > Options scrolled to PARTNER BODY with "Allow other characters" ticked), `10-play-dev-build`,
`11-play-mod-mismatch`, `13-mod-foreign`, `14-mod-disabled`,
`20-ingame-solo`, `27-debug-options`, `28-debug-files`, `29-debug-in-game`,
and the updater's `31-play-update-available`, `32-play-update-downloading`,
`33-play-update-failed`, `34-play-updated` and `35-about-updates`.

## Not done / not verified

* Not run against the real game: the end-to-end launch through Steam, the mod
  reading `session.ini` and writing `link-status.json` were not exercised (the
  mod was being written in parallel). The helper was verified with a stand-in
  `ds.exe` process in a scratch folder (it removed `session.ini` 8 s after that
  process exited); the window's own paths are covered by the fake-Env tests.
* The real window was not opened interactively; every page was checked from
  the hidden `--screenshot-all` render of the mock model.
* "Ours" for a DLL without `package.json` is a byte-pattern test (the
  proxy's forwarders plus the folder name), not a signature: a deliberately
  crafted DLL would pass it. With `package.json` the SHA-256 decides.
* The Debug page, mod on/off and the package checks were verified with the
  unit tests, the screenshots, `--selftest` / `--write-session` in a scratch
  game folder extracted from a real zip, and the mod's `LoadConfig` in a
  harness; not against the running game.
* No saves, uninstall, or Steam invites from the launcher (invites are the
  mod's job: `+stranded_join` connect strings are accepted when pasted).
* The updater was run end to end only against a loopback feed (a scratch
  game folder, a hand-made 0.1.1 zip, `--update-feed ... --update-now`: checked,
  downloaded, verified, installed, restarted, `.old` deleted); against GitHub
  only the check (the repository has no release yet). The first real release
  will be the first real download from GitHub's asset host.
