# Test saves and the local two-player test

Two Debug tools of the launcher (**Debug > Test saves**), both built on the save redirect of
the hidden test runs ([hidden-testing.md](hidden-testing.md) "Saves"), now usable in normal,
visible play:

* **Test save**: play a save you picked once (a downloaded 100% save, a friend's save) on a
  copy. The game reads and writes only that copy; your own save folder is only read (your
  profile is copied in) and is hashed before and after.
* **Local two-player test**: two copies of `ds.exe` on this PC, host and guest, linked over UDP
  loopback, each on its own copy of the test save (or of your saves), like Diamond Dogs
  Together's two-window test ([ref-ddt/docs/single-pc-testing.md](../ref-ddt/docs/single-pc-testing.md)).

## Layout

```
<game>\stranded_together\sandbox\<name>\     a sandbox (name: test, local-host, local-guest)
    saves\                                     what the game sees as %LOCALAPPDATA%\KojimaProductions
        .st-redirect-probe                     the mod's self-test file (only a sandbox has it)
        DeathStrandingDC\<account>\profile\    YOUR profile (copied; profile.dat is bound to your account)
        DeathStrandingDC\<account>\autosaveN\  the test save's slots (autosave*/manualsave*/quicksave*)
        DeathStranding\<account>\transfer\
    settings.cfg                               the game folder's settings.cfg as the local-test games see it
    sandbox.json                               what it was seeded from
<game>\stranded_together\instances\host|guest\  per-game mod folder of the local test (ST_INSTANCE_DIR):
                                               session.ini, stranded_together.log, link-status.json
```

A test save folder may be laid out as `<folder>\DeathStrandingDC\<slots>` (the downloaded 100%
save), `<folder>\DeathStrandingDC\<account>\<slots>`, or be the `DeathStrandingDC` / account
folder itself (`core::CheckSeedFolder`). The sandbox is seeded on first use and keeps its
progress between games; **Reset test saves** deletes all three sandboxes (the next game seeds
them again). Picking another folder resets them too.

## The mod side (`native/mod/sandbox.*`, `automation.cpp` `EarlyInit`)

`session.ini` keys (written by the launcher only; one-shot like every launcher session):

| Key | Effect | Gate |
|---|---|---|
| `sandbox_saves=<name>` | DllMain installs the save redirect (the same 31 file-API hooks as hidden runs) with the scratch copy `stranded_together\sandbox\<name>\saves`; self-test through the hooks (`<real root>\.st-redirect-probe` must be answered from the sandbox) | `[A-Za-z0-9_-]{1,40}`; a missing seed, a failed hook or self-test: **the game ends in DllMain** (exit code `0x53540006`) and `<mod>\sandbox-refused.txt` says why |
| `sandbox_foreign_saves=1` | ds.exe's save-owner check is skipped (the 3 byte-checked `jne`->`jmp` of hidden-testing.md "Using another save") | only after the redirect self-test passed (`SandboxForeignAllowed`) |
| `appdir_settings=1` | `<game>\settings.cfg` (+ `settings.cfgtemp`, `settings.cfg.tmp`) is redirected to the sandbox's `settings.cfg` | needs a redirect |
| `second_instance=1` | CreateMutexA/W(Ex) hooked in this process: `DeathStrandingDCAppMutex` (CreateMutexA at 0x1417EAE1D; `exit(1)` on `ERROR_ALREADY_EXISTS` at 0x1417EAE96) becomes `DeathStrandingDCAppMutex-StrandedTogether-2`; self-tested | needs a redirect (two games never share one save tree) |
| `local_input=host\|host_pad\|guest` | the input split (below) | the overlay is installed for it |
| `local_window=left\|right` | the game window gets a title bar and the left / right half of its monitor | |
| `frame_cap=<15..240>` | the game thread waits once per presented frame (frame pump hook, `dllmain.cpp` `FrameCap`) | |

`sandbox::Decide`: no key -> nothing is installed (zero behaviour change); `automation=1` wins
(hidden runs keep their own rules); `second_instance`/`appdir_settings`/`sandbox_foreign_saves`
without a redirect -> refused. A ds.exe started by `st_testenv_supervisor` must have an
automation or sandbox session: without one (a session.ini deleted between deploy and start) it
ends in DllMain instead of running on the real saves. The same for a game started with
`ST_INSTANCE_DIR` (the launcher's local test): no session.ini in its folder, or one without
`sandbox_saves=` / `automation=1` (`Mode::refuse_instance`) -> it ends in DllMain and
`sandbox-refused.txt` in its instance folder says why (its one-shot file was already used, or
ds.exe restarted itself).

Evidence in the log: `[sandbox] saves=redirected: ...`, `[sandbox] test save: the game reads and
writes only the sandbox copy`, `[sandbox] foreign saves: check bypassed at ...`,
`[mod] second instance: mutex=renamed (1 call(s) ...)`; `<mod>\sandbox-active.txt` (written in
DllMain).

**Per-game mod folder.** `ST_INSTANCE_DIR` (environment of the process; 1..3 plain components,
`sandbox::ValidInstanceDir`) moves session.ini, the log, link-status.json and the automation
channel to `stranded_together\<dir>`. Only the launcher's local test (`instances\host|guest`)
and the testenv supervisor (`automation\guest`) set it; a game started through Steam never has it.

### Input split and focus (`overlay_input.*` `LocalMode`)

| | host | host_pad | guest |
|---|---|---|---|
| keyboard + mouse | yes (when its window is in front) | yes | (its window still takes keys when you click it) |
| XInput pads | read as disconnected, no rumble | yes | yes |
| cursor clip / move | only while its window really is the foreground window | same | never |
| `GetForegroundWindow` | real | real | its own window (ds.exe clears its keyboard state otherwise) |
| `WM_ACTIVATEAPP(FALSE)` | handed on as TRUE | same | same |

ds.exe sets its app-active flag only from `WM_ACTIVATEAPP` and its pad reader ignores pads while
it is clear (hidden-testing.md "Pad input in gameplay"), so both games keep running and reading
their pads unfocused. Not isolated: PlayStation (HID) pads and Steam Input, which ds.exe reads
itself: such a pad drives both games.

### Graphics

Two games share the GPU, so the launcher caps both at 30 fps (`frame_cap=30`: in hidden runs the
host's CPU time per second fell from 8.1 s to 1.9 s and the second game could finally keep up).
It also asks for the windows side by side (`local_window`): the move runs on the game window's
own thread (a registered message the subclass handles; from the render thread it deadlocked both
games, hidden run 20261116-160909) and the subclass keeps the size when ds.exe asks for a
full-monitor window, **but in hidden runs both windows still ended at 2560x1440 at 0,0**: ds.exe
resizes its window another way. Until that is found, set **Windowed** (and a lower resolution)
once in each test window's graphics options; that is saved in that sandbox only.

**Low graphics settings could not be forced.** The engine's own graphics file is
`appdir:settings.cfg` (keys `rendering_width`, `window_mode`, `limit_fps`, ... found in
FUN_142644FF0 / FUN_142643B40); `appdir_settings=1` redirects it per sandbox and the launcher
seeds `sandbox.hpp`'s `LowGraphicsSettingsCfg`, and the testenv also merged
`GraphicQuality`/`WindowMode` into the profile's `game_settings.cfg`. In hidden runs none of
these changed the boot display mode (still 2560x1440, runs 20261116-153540 .. -154718). Lower
the quality once in each test window's options: those settings are saved into that sandbox
only (its profile / its settings.cfg), never into your own.

## The launcher side (`native/launcher/core/core_sandbox.*`, `core_model.cpp`)

* **Choose folder...** (`IFileOpenDialog`, folders only) -> `set_test_save_folder`: must hold save
  slots; remembered in `launcher-settings.json` `"test_save": {"folder", "use", "host_pad"}`.
* **Play with test save (your saves untouched)**: Debug tools only (solo, loopback) with
  `sandbox_saves=test` + `sandbox_foreign_saves=1`, started through Steam like every launch.
  **Also for Host, Join, Solo test** adds the same keys to every game started from the launcher.
* Before such a game the launcher hashes `%LOCALAPPDATA%\KojimaProductions` (every file, SHA-256;
  `stranded_together\sandbox\real-saves-before.txt`); after it exits it compares and says
  "Your own saves were not changed", or names what changed (the game itself only writes the copy,
  so a difference would be Steam Cloud), or shows the mod's refusal from `sandbox-refused.txt`.
  It also deletes `sandbox-active.txt` before the launch; when the game has run 20 s without the
  mod writing it again (the session.ini never reached that game, e.g. deleted by another program),
  the Debug page warns at once that the game may be on your own saves, and says so again after it.
  The local test's refusals (`instances\host|guest\sandbox-refused.txt`) are read only for the
  local test, never for a later "Play with test save" game. One test at a time: nothing else
  starts while the local test runs.
* A test save picked inside `stranded_together\sandbox` is refused (it would be emptied first).
* **Local two-player test**: seeds `local-host` and `local-guest` (from the test save, or a full
  copy of your saves when none is picked), writes `settings.cfg` into each when missing, writes
  `instances\host\session.ini` (role=host, UDP 27555 -> 27556, a fresh secret, `local_input=host`
  or `host_pad`, `local_window=left`) and `instances\guest\session.ini` (role=guest, 27556 ->
  27555, `second_instance=1`, `local_input=guest`, `local_window=right`), both with
  `appdir_settings=1`, `frame_cap=30`, `debug_menu=1`. It starts `ds.exe` directly with
  `SteamAppId=1850570` (steam_api64's RestartAppIfNecessary then answers false: no relaunch
  through Steam), `SteamGameId` and `ST_INSTANCE_DIR`, the guest 8 s after the host; shows both
  games' link state; when both have exited it removes unread session.ini files and compares the
  real save tree. Steam must run (both games sign in with your account).
  Since v0.19, with Debug > **Host holds the save** (default on) both files also carry `host_save=1`
  (`core::MakeLocalTestInis`): the guest game is the host-held save session's guest on `local-guest`, which is
  prepared like `coop-guest` before every start (no slot of its own: the host's save arrives over the link), and the
  host game plays its `local-host` copy. So the local test shows the waiting screen, the save transfer, the shared
  vehicles, riding together and vehicle hits (docs/coop-session.md). With the option off, each game plays its own
  copy as before, and both windows show the amber "Co-op features OFF: host_save=0" notice.
* The second game shows **"Unable to log in to Death Stranding game server. Launching in Offline
  Mode... Error Code: 50000"** (the game server refuses the same account twice; hidden run
  20261116-155336): press OK (A / Enter) and it continues offline.

## Tests and evidence

* `st_launcher_core_tests` `TestSaves`: seed-folder layouts, seeding (your profile, the test
  slots, no `steam_autocloud.vdf`, the probe), refusals (outside `sandbox\`, overlapping the real
  tree, no slots), the session.ini keys (round trip and invalid combinations), the real model's
  Play with test save (session.ini keys, the after-game check), "Also for Host, Join, Solo test",
  a refusal by the mod, the local test (both session.ini files, the start environment, the guest
  8 s later, the end), and the fake real save tree byte-identical at the end.
* `st_testenv_tests` `SandboxRules`: names, `ST_INSTANCE_DIR`, the mutex rename, `Decide`, the
  foreign gate, the game_settings merge, the two-game and sandbox-session session.ini texts, the
  `@guest`/`@both` scenario steps.
* Hidden runs ([hidden-testing.md](hidden-testing.md) "Two games" and "Test-save session").
* Not run visibly: the launcher's buttons against the real game (the hidden runs exercise the
  same mod paths with the supervisor's session.ini files); the input split and window placement
  with a real keyboard, mouse and pad.
