# Hidden testing: Death Stranding on a private desktop

Runs the real game on a **private, never-displayed Windows desktop**, drives it
through the mod with scripted commands and collects logs and screenshots. Nothing
appears on your screen, the game's audio is muted, and a pad in your hands never
reaches it. Modelled on Diamond Dogs Together's `tools/worker` (MGSV), much smaller.

```powershell
pwsh -File tools\testenv\Start-TestRun.ps1 -Probe            # D3D12 on a private desktop, no game (~10 s)
pwsh -File tools\testenv\Start-TestRun.ps1 -Scenario smoke   # the real game, <= 6 minutes
pwsh -File tools\testenv\Restore-TestRun.ps1 -RunDir build-testenv\runs\<run>   # only after a crash
```

Build: `cmake -S . -B build-testenv -G "Visual Studio 17 2022" -A x64` then
`cmake --build build-testenv --config Release` (Start-TestRun builds `st_mod`,
the supervisor and the probe itself unless `-NoBuild`).

## Status (2026-11-16)

| What | Result |
|---|---|
| Probe (D3D12, private desktop, RTX 5080) | **works**: flip-discard swapchain created, every `Present` answers `DXGI_STATUS_OCCLUDED` (0x087A0001) but frames are rendered and paced: `Present(1,0)` 144.6 fps (the monitor's rate), `Present(0,0)` 5164 fps; 575/575 back-buffer captures pixel-exact; input desktop stayed `Default`; `GetForegroundWindow()` = NULL there, `GetActiveWindow()`/`GetFocus()` = the window. |
| Mod side (overlay harness, 64 checks) | **works**: capture with the menu closed (incl. a 10-bit buffer), PNG round trip through WIC, virtual pad on `xinput1_4`, foreground answer, cursor isolation, menu kept on focus loss, `OCCLUDED` answered as `S_OK`. |
| End to end with a fake `ds.exe` (ctest `st_testenv_e2e`) | **works**: real supervisor + real mod on a private desktop; screenshots before/after a virtual A press differ as expected, quit through `WM_CLOSE`, the scratch game folder is restored byte for byte. |
| Saves | **redirected**: with `automation=1` the mod hooks the Win32 file API in DllMain and sends every access under `%LOCALAPPDATA%\KojimaProductions` to a scratch copy; the supervisor backs the real tree up, confirms the redirect before any step, compares by hash afterwards. 6 real runs: real tree identical every time, 0 Steam Cloud uploads. See "Saves". |
| Real game, start-up | **solved**: ds.exe quit with code 0 because the mod's overlay loaded `d3d12.dll` before the game's own device creation (ds.exe's check at 0x1419A6CE9). The overlay now waits for the game's delay-load in automation runs. See "Why the game quit at once". |
| Real game, smoke (12:25, 12:28) | **passes**: title -> Continue (virtual pad) -> Sam in the world (Incinerator West of Capital Knot City) -> debug menu -> quit with exit code 0 in ~50 s; screenshots in `runs\<run>\automation\`; saves identical. |

## How it works

```
Start-TestRun.ps1 ─> st_testenv_supervisor.exe (GUI subsystem: never a console window)
   │  preflight: no ds.exe running, Steam running, no steam_appid.txt, scenario parses
   │  saves: %LOCALAPPDATA%\KojimaProductions hashed (saves-before.txt), copied to
   │         <run>\saves-backup (verified), <run>\saves-restore.txt written BEFORE any change
   │  game folder: winhttp.dll, error.txt, every file of stranded_together\ backed up to
   │               <run>\backup, manifest <run>\restore.txt written BEFORE any change
   │  deploy: build-testenv winhttp.dll + stranded_together\session.ini
   │          (role=solo, loopback UDP, debug_menu=1, automation=1, one_shot=1)
   │          + stranded_together\automation\saves = scratch copy of the saves (verified)
   │  CreateDesktopW("StTestEnv_<pid>_<tick>")   -- never SwitchDesktop
   │  job: kill-on-close, die-on-unhandled-exception (no WER dialog), UI limits
   │       (no desktop switch/create, no display-mode or SystemParametersInfo changes,
   │        no clipboard, no ExitWindows)
   │  CreateProcess(ds.exe, STARTUPINFO.lpDesktop = WinSta0\StTestEnv_..., suspended,
   │                SteamAppId=1850570, SteamGameId=1850570, stdout = <run>\game-stdout.txt)
   │                -> job -> resume
   │  saves gate: automation\saves-redirect.txt (mod DllMain) within 10 s and the log line
   │              "[auto] saves=redirected" within 30 s, else kill the job (no step runs)
   │  every second: no window of a job process on the user's desktop, the input desktop
   │                unchanged, no ds.exe outside the job -> otherwise kill the job, stop
   │  scenario steps -> <mod>\automation\in\NNNN.cmd, acks <mod>\automation\out\NNNN.done
   │  end: quit command, 30 s grace, TerminateJobObject (only our processes)
   └─ collect <run>\game\ (mod log, session.last.ini, link-status.json, new error.txt,
      saves-redirect.txt, exit-trace.txt), <run>\automation\ (screenshots, results.txt,
      status.txt), what the game wrote into the scratch saves (saves-scratch-*), restore
      the game folder, compare the real saves by hash (restore them if they differ),
      Steam's cloud_log lines for the run (steam-cloud.txt), summary.json
```

**Why no relaunch through Steam.** ds.exe calls `SteamAPI_RestartAppIfNecessary(1850570)`
(wrapper 0x141A1F670 = `mov ecx,0x1C3CCA; jmp [IAT]`, called from 0x1418EA074) and
returns 0 when it says true. The shipped `steam_api64.dll` (RVA 0x92D0) first reads
`GetEnvironmentVariableA("SteamAppId")`: 1..31 characters with a non-zero number
returns false, before it looks at `steam_appid.txt` and before it would open
`steam://run/1850570`. The supervisor sets `SteamAppId` in the child's environment
only, so no `steam_appid.txt` is ever written (the preflight refuses if one exists, and
restore checks it is still absent). Verified in the one real run: Steam logged
"Game process added: AppID 1850570 ... ProcID 22760" (SteamAPI_Init connected) and
no second ds.exe appeared.

### The mod side (`session.ini automation=1`)

`native/mod/automation.cpp` reads `automation`, `automation_mute` and
`automation_isolate` from `session.ini` itself, before `LoadConfig` renames a
one-shot file, and starts a thread that only does file I/O. It never reads game
memory; game actions are queued to the existing debug-action queue and run on the
game thread (`st::debug::OnTick`), screenshots are taken inside `Present`.
Before that, `DllMain` installs the save redirect ("Saves") and the exit tracer, and in
automation runs the overlay is installed only once ds.exe has loaded `d3d12.dll` itself
("Why the game quit at once").

Commands (one per line in `automation\in\*.cmd`, files in name order):

| Command | Does |
|---|---|
| `wait <ms>` | sleep |
| `log <text>` | `[auto] <text>` in stranded_together.log |
| `screenshot <name>` | the back buffer being presented -> `out\<name>.png` (menu open or closed) |
| `pad <buttons> <ms> [lx= ly= rx= ry= lt= rt=]` | virtual XInput pad 0: `A`, `A+START`, `DPAD_DOWN`, `none`...; then idle but connected |
| `padoff` | virtual pad disconnected |
| `key <F9\|esc\|w\|0xNN> <ms>` | `WM_KEYDOWN/UP` posted to the game window (window procedure only; posted keys never reach `WH_KEYBOARD` hooks, so most game keys will not react) |
| `menu open\|close\|toggle` | the F9 debug menu |
| `action <name> [n n n]` | a debug action by its label in snake case (`respawn_partner`, `log_status`, `teleport x y z`) |
| `waitplayer <s>` | until the tick's snapshot has the local player |
| `waitlog <s> <text>` | until a new log line contains `<text>` |
| `snapshot` | the tick's snapshot as the result line |
| `quit` | `WM_CLOSE` to the game window, `TerminateProcess` after 20 s |

Results: one line per command in `out\results.txt`, `out\NNNN.done` (`ok|fail`, count,
last detail), `out\status.txt` every second (presents, fps, back buffer, queue, pad
reads, foreground answers, silenced audio buffers, snapshot), `out\ready.txt` when the
channel is up.

Isolation hooks (overlay, automation only; `automation_isolate=0` turns them off):

* **Pad**: ds.exe calls `LoadLibrary("xinput1_4.dll")` + `GetProcAddress("XInputGetState")`
  at 0x1419B1C57 and polls through that pointer (+0x16818, call at 0x1419B1DC7);
  `XINPUTUAP.dll` (delay-loaded) is only its fallback. Both exports are hooked (the
  overlay previously hooked only XINPUTUAP, so the menu's pad blocking now also works).
  In automation only virtual pad 0 exists; real pads read as disconnected. The packet
  number changes with every state change.
* **Foreground**: `GetForegroundWindow` answers the game window (ds.exe compares it with
  its window at 0x141A18719 and clears its keyboard state at 0x1419B9691 when they
  differ); before the first `Present` attaches the overlay, the process's largest visible
  top-level window.
* **Display**: `Present`/`Present1` answer `S_OK` instead of `DXGI_STATUS_OCCLUDED` (a
  game may stop rendering while occluded); `SetFullscreenState(TRUE)` answers
  `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE` and fullscreen swapchain descriptions are made
  windowed; `SetCursorPos`/`ClipCursor` never reach the system.
* **Audio**: `IAudioRenderClient::ReleaseBuffer` (vtable slot 4 of the shared-mode render
  client, found on a throwaway client) gets `AUDCLNT_BUFFERFLAGS_SILENT`. Process-local;
  nothing is written to the volume mixer. `--keep-audio` turns it off.
* Focus loss does not close the debug menu (the hidden window never has focus).

Screenshots: `st::overlay::CaptureBackBuffer` (overlay_renderer.cpp) records
PRESENT->COPY_SOURCE, `CopyTextureRegion` to a readback buffer, COPY_SOURCE->PRESENT on
the game's DIRECT queue (found like the menu's renderer: swapchain-creation hook, else
`ExecuteCommandLists` capture), waits for its fence (3 s, then 10 s; never frees objects
the GPU may still write) and converts 8-bit RGBA/BGRA, R10G10B10A2 and FP16 (sRGB-encoded)
to RGB. PNGs use zlib stored blocks (no dependencies; ~6 MB per 1080p shot).

## Saves

Hidden runs that press CONTINUE load a save, DS writes `profile\profile.dat` on load and
autosaves in the world, and Steam runs AutoCloud on the game's exit. So the game never
sees the user's real save folder: every access is redirected to a scratch copy, the
redirect is proven before any scenario step runs, and the real tree is checked by hash
afterwards.

**Where DS keeps them (static, `ds.exe`).** `FUN_14192DC90` mounts the file devices.
Its `user:` device is `getenv("USERPROFILE")` (`FUN_141A1F110`) + `"AppData\Local\"` +
company + `"\"` + product + `"\"` + the Steam account id
(`SteamInternal_ContextInit` -> `ISteamUser`) + `"\"`. Company and product come from the
platform object (`DAT_14577D3F0`, vtable +0x120/+0x130, set in `GameMainProg` from the
Decima strings at 0x144560010 `KojimaProductions` and 0x144560040 `DeathStrandingDC`).
The `xfr:` device (the original DEATH STRANDING's saves, for the import) is
`...\KojimaProductions\DeathStranding\<id>\transfer\` (+0x140, 0x144560070
`DeathStranding`). There is no `SHGetKnownFolderPath`/`SHGetFolderPath` import: the
root comes from `USERPROFILE`, and Steam's AutoCloud rule is `WinAppDataLocal`
`KojimaProductions/DeathStrandingDC/<id>` (cloud_log). On disk (this PC, account
97930372): `autosaveN\checkpoint.dat`, `manualsaveN\...`, `quicksaveN\...` (0..22),
`profile\profile.dat` (written by the game on every load), `profile\game_settings.cfg`,
`profile\bindings.cfg` (the settings files), `steam_autocloud.vdf` (Steam's), and
`DeathStranding\<id>\transfer\` (empty). The checkpoint writer `FUN_141A01ED0` uses
`checkpoint.dattemp` (0x143BA4370) next to `checkpoint.dat`.

**Which APIs (static).** Everything below those devices goes through ds.exe's Win32
`SystemFileDevice` (`FUN_1419xxxxx`/`FUN_14218xxxx`): imports `CreateFileW` (10 call
sites), `FindFirstFileW`/`FindFirstFileExW`/`FindNextFileW`, `GetFileAttributesW` (13),
`SetFileAttributesW`, `DeleteFileW`, `MoveFileW`, `MoveFileExW`, `CopyFileW`,
`CreateDirectoryW`, `RemoveDirectoryW`, `GetFullPathNameW`, `GetFileAttributesA` (1);
the CRT (`_wfopen`, `_wfsopen`, `fopen_s`) ends in `CreateFileW` too.

**The redirect (mod, `session.ini automation=1` only; `st::automation::EarlyInit`).**
Installed from `DllMain` of `winhttp.dll` (a static import of ds.exe, so before
ds.exe's own code runs; MinHook needs no loader calls there). Hooks the function bodies
in `kernelbase.dll` (kernel32's exports are stubs into it; the bodies also catch
kernelbase's internal calls) plus kernel32's `MoveFileW`/`MoveFileA`/`MoveFileExA`/
`CopyFileA`: `CreateFileW/A`, `CreateFile2`, `FindFirstFileW/A`, `FindFirstFileExW/A`,
`GetFileAttributesW/A`, `GetFileAttributesExW/A`, `SetFileAttributesW`, `DeleteFileW/A`,
`RemoveDirectoryW/A`, `CreateDirectoryW/A`, `CreateDirectoryExW`, `MoveFileW/A`,
`MoveFileExW/A`, `MoveFileWithProgressW`, `CopyFileW/A`, `CopyFileExW`, `ReplaceFileW`
plus (not imported by ds.exe, so no other route reaches the root) `SetFileAttributesA`,
`CopyFile2`, `CreateHardLinkW` (31; the 12 ds.exe imports are required). Every path argument under
`%USERPROFILE%\AppData\Local\KojimaProductions` (or `%LOCALAPPDATA%\KojimaProductions`,
or a test root from `automation_save_root=`) is mapped to
`<game>\stranded_together\automation\saves\<rest>` (`st::automation::saves::Map`:
case-insensitive, `/` and doubled separators, `.`/`..`, trailing dots/spaces of a
component, `\\?\`/`\??\`/`\\.\` prefixes, alternate data streams of the root; the scratch
copy is outside every root, so mapping twice changes nothing). A path is also checked as
Win32 resolves it (`GetFullPathNameW`: relative and drive-relative paths) and with 8.3
short names expanded (`saves::ExpandShortNames`, through the unhooked `FindFirstFileExW`).
Not covered (ds.exe never spells its paths so; it builds them from `getenv("USERPROFILE")`
+ `GetLongPathNameA` + `GetFullPathNameW`): other names for the same folder (junctions,
symlinks, `subst`, `\\localhost\C$\...`, `\\?\Volume{...}\...`) and the native API
(`NtCreateFile`, which ds.exe does not import). Self-test before the game runs:
`GetFileAttributesW`/`CreateFileW`/`GetFileAttributesA` of `<real root>\.st-redirect-probe`
must be answered from the scratch copy (only the supervisor's seed has that file). Then
`automation\saves-redirect.txt` is written. If anything fails (no root, no seeded copy,
a required hook, the self-test) the mod writes `automation\saves-redirect-failed.txt`
and ends ds.exe with exit code `0x53540005` from DllMain: the game never runs
unprotected. The same happens when ds.exe was not started by the supervisor (its child gets
`ST_TESTENV_SUPERVISED`): `automation=1` left behind by an interrupted run must not turn
a launch through Steam into a redirected one whose progress would land in the scratch
copy (ctest `st_testenv_e2e` checks this refusal). And if the bootstrap thread finds
`automation=1` but no active redirect (DllMain could not read session.ini), it ends the
game the same way. Hidden runs also block `ShellExecute(Ex)A/W` (ds.exe imports
`ShellExecuteA`): whatever it would open (a browser tab) would be outside the job and on
the user's desktop. In the log: `[auto] saves=redirected: ...` (once, at load), then one line
per redirected place (`[auto] saves: read DeathStrandingDC\97930372\profile -> scratch`,
at most 96) and, for discovery, one per place the game writes outside the root
(`[auto] write outside the save root (write): C:\Users\...\D3DSCache\...`, at most 48).
Globals are plain arrays (no destructors), so the hooks stay valid while the process
exits; a path that cannot be decided (out of memory) fails the call instead of reaching
the real file.

**The supervisor.** Before anything changes: `saves-before.txt` (every folder and file
with size + SHA-256), `saves-backup\` (verified against it), `saves-restore.txt`. The
scratch copy is seeded from the backup (verified) plus the probe file. After the launch
it waits for `saves-redirect.txt` (10 s) and the `[auto] saves=redirected` log line
(30 s); without both, or with `saves-redirect-failed.txt`, it kills the job and runs no
step. After the exit: the scratch copy's changes are kept (`saves-scratch-after.txt`,
`saves-scratch-changed\`), the supervisor waits for Steam's AutoCloud pass (below), then
the real tree is hashed again (`saves-after.txt`; so the check also covers what Steam did
after the exit); any
difference is restored from `saves-backup\` (files that appeared go to `saves-added\`,
never deleted; new empty folders removed) and verified (`saves-restored.txt` = `ok` /
`INCOMPLETE`). `Restore-TestRun.ps1` / `--restore <run>` does the same after a crash.

**Steam Cloud.** Steam's AutoCloud runs on the hidden game's exit too (`Running
AutoCloud on exit`), but it only looks at the real folder, which the redirect never
touches: every run since the redirect logged `Skipping un-modified file` for all 33
files and `0` upload lines (`Need to upload file` / `HTTP upload` / `Upload OK`); the
supervisor collects the run's lines to `steam-cloud.txt` and fails the run on any upload
line (`cloud_uploads` in summary.json).

## Why the game quit at once (solved)

The first real run (11:32) and the first run of this round ended with ds.exe exiting
by itself, code 0, before any window. The supervisor now records who ends the process:
the mod hooks `RtlExitUserProcess`/`TerminateProcess` in automation runs and logs the
call stack (`[auto] exit: ...`, `game\exit-trace.txt`). Run 12:20 (`-Scenario alive`):

```
ExitProcess(0) on the main thread: ucrtbase+0xA0813 <- ds.exe 0x141A12A4E (FUN_141A12A20 = platform
Exit: _exit) <- 0x1419A6CEF <- 0x1419A54FC <- 0x141990315 <- 0x14262F2D3 <- 0x14262C2EA (app init)
<- 0x1418E9E6C (GameMainProg) <- 0x1418EA224 (main)
```

`FUN_1419A6980` creates the D3D12 device. On entry it takes
`GetModuleHandleA("d3d12.dll")` and the module range; after `D3D12CreateDevice` it
compares the device's vtable pointer with that range and, when the module was loaded and
the vtable is outside it, calls `platform->vtable[+0x180](0)` at 0x1419A6CE9 =
`FUN_141A12A20` = `_exit(0)`. The device vtable always lives in `D3D12Core.dll`
(checked on this PC: vtable 0x7FFBAC0D9078 inside D3D12Core, not d3d12.dll), so the
check fires exactly when `d3d12.dll` is already loaded before the game's own delay-load,
which the mod's overlay did (`LoadLibraryW(L"d3d12.dll")` + a probe device at ~0.15 s;
the game reaches its device ~0.3-0.6 s after start). It has nothing to do with the
private desktop, the job or the environment: the `-NoMod` run (12:22, no overlay) lived
49 s with its window on the private desktop. This matches the Steam overlay log of 11:32
(7 factories, no device hooks).

Fix (automation runs): the Bootstrap thread waits until ds.exe has loaded `d3d12.dll`
itself before the overlay installs (`st::automation::WaitForGameD3D12`, logged as
`[auto] overlay deferred 313 ms until ds.exe loaded d3d12.dll itself`). The overlay
then misses the swapchain-creation hook and finds the DIRECT queue through
`ExecuteCommandLists` (logged `queue from ExecuteCommandLists`), which works.
**The same exit hits a normal launch with `debug_menu=1`** (the overlay install path is
the same); the overlay's own install order (`native/mod/overlay/overlay.cpp`
`st::overlay::Install`) needs the same deferral outside automation.

## Runs (2026-11-16, all on the private desktop)

| Run (`build-testenv\runs\`) | Switches | Outcome | Saves |
|---|---|---|---|
| `20261116-113201-smoke` (before this round) | smoke | ds.exe exit 0 after 1.4 s, no window | unchanged (read only then) |
| `20261116-122025-alive` | `-Scenario alive` | exit 0 after 2.98 s, no window; exit trace -> 0x1419A6CEF (above); game stdout captured (BuildID `dsq 179/4027081`, SaveGame version 67) | redirect confirmed (marker 2.31 s), real tree unchanged, 0 cloud uploads |
| `20261116-122211-alive` | `-Scenario alive -NoMod` (redirect only) | alive 49.3 s, window `DEATH STRANDING DIRECTOR'S CUT` on the private desktop only; killed at the end (no quit command) | confirmed, unchanged, 0 uploads, scratch unchanged |
| `20261116-122344-boot` | `-Scenario boot` (overlay deferred) | alive 88.7 s, 8 screenshots: black, then the title menu from ~20 s (CONTINUE selected); `quit` -> exit 0 via `main` | confirmed, unchanged, 0 uploads |
| `20261116-122558-smoke` | `-Scenario smoke` | **pass**: title, Continue, local player after 29 s at (543.37, 330.40, 237.29), in-world, debug menu, log_status, quit -> exit 0 after 48.4 s | confirmed, unchanged, 0 uploads; scratch `profile.dat` changed (discarded) |
| `20261116-122820-smoke` | `-Scenario smoke` (+ `06-later`) | **pass**, same path, exit 0 after 52 s | confirmed, unchanged, 0 uploads; scratch `profile.dat` changed (discarded) |
| `20261116-124653-smoke` | `-Scenario smoke` after the review fixes (path forms, 8.3, supervisor gate, ShellExecute block; 31 hooks) | **pass**, same path, exit 0 after 52.6 s; 4 ShellExecute hooks, nothing blocked | confirmed, unchanged (also by an independent sha256 of all 34 files), 0 uploads |

Every run: no window of the job on the user's desktop, input desktop `Default`
before/after, no violation, game folder restored (`restored.txt` ok), no `error.txt`.

## Scenarios (`tools/testenv/scenarios/*.txt`)

```
role solo|echo               session role (default solo)
timeout <s>                  steps limit, 30..360 (default 360). The game's whole lifetime is capped
                             at 360 s: steps get at most 315 s, the last 45 s are kept for the
                             final screenshot, quit, exit and kill
waitready <s>                until automation\out\ready.txt exists
waitlog <s> <regex>          until a stranded_together.log line matches (ECMAScript)
until <s> <ms> <regex> => <mod command>   repeat the command until the log matches
sleep <ms> / note <text>     supervisor side
<anything else>              one mod command (checked when the scenario is parsed)
```

A leading `?` makes a step optional. A failed required step ends the scenario; the
supervisor then takes `screenshot zz-final` and sends `quit`.

* `smoke.txt`: channel up, overlay attached, 25 s for the title (boot.txt: black, then the
  title menu after ~15-20 s; no "press any button", no notices; CONTINUE is selected when a
  save exists), `01-title`, `pad A`, `02-after-continue` (loading, black), A again every 5 s
  only while there is no `[sync] local player entity 0 -> <entity>` (none was needed),
  `waitplayer`, 10 s, `03-in-world`, `snapshot`, debug menu open, `04-debug-menu`,
  `action log_status`, close, `05-menu-closed`, 4 s, `06-later`, `snapshot`, `quit`.
* `boot.txt`: no input, a screenshot every 10-15 s for 80 s (title flow discovery).
* `alive.txt`: supervisor steps only (works with `-NoMod`): watch the game for 45 s.
* `fake-smoke.txt`: the ctest's fake game.

`-NoMod` (`--no-mod`) still deploys `winhttp.dll`, with a `session.ini` that has only
`automation=1`: the mod redirects the saves and does nothing else (no session, overlay,
automation channel or game hooks).

## Outputs

`build-testenv\runs\<yyyyMMdd-HHmmss>-<scenario>\`: `summary.json` (result, end reason,
game exit code / seconds alive / CPU seconds, violation, windows seen on the private
desktop, restore notes, `saves_redirected` + `saves_evidence`, `saves_unchanged`,
`save_changes` (real tree, before restoring), `scratch_changes`, `cloud_uploads`,
`exit_trace`, the last lines of the game's stdout, every step with its detail),
`supervisor.log`, `scenario.txt`, `game-stdout.txt` (ds.exe's own log), `game\`
(`stranded_together.log`, `saves-redirect.txt`, `exit-trace.txt`, ...),
`automation\*.png`, `automation\results.txt`, `automation\status.txt`,
`saves-before.txt` / `saves-backup\` / `saves-restore.txt` / `saves-after.txt` /
`saves-restored.txt`, `saves-scratch-after.txt` + `saves-scratch-changed\`,
`steam-cloud.txt`, `backup\` + `restore.txt` + `restored.txt`, `game-new\` (files that
appeared in stranded_together, e.g. the fake game's log).

Exit codes: 0 pass, 1 scenario failed / game exited / save redirect not confirmed / real
saves changed / cloud upload / restore incomplete, 2 refused (preflight), 3 safety violation.

## Safety rules (enforced, not assumed)

* Refuses when any `ds.exe` runs, when Steam is not running, when `steam_appid.txt`
  exists, when `%LOCALAPPDATA%\KojimaProductions` and `%USERPROFILE%\AppData\Local\KojimaProductions`
  differ, and while another supervisor run is active (`Local\StrandedTogetherTestEnv`).
* Never `SwitchDesktop`, never system input (`SendInput`), never focus/foreground calls
  on the user's desktop. The children's job forbids desktop switching/creation.
* Kills only its own job. A window of the job on the user's desktop, a changed input
  desktop or a `ds.exe` outside the job ends the run at once (exit 3).
* Saves: backed up and hashed first, redirected by the mod (proven before any step,
  otherwise the game is killed or ends itself in DllMain), compared and restored after
  (see "Saves"). Every run, `-NoMod` included, deploys the redirecting `winhttp.dll`.
* The game folder is restored from `restore.txt` (also after a crash: `Restore-TestRun.ps1`,
  or Start-TestRun does it, up to 3 times, when `restored.txt` or `saves-restored.txt` is
  missing or says INCOMPLETE). The game-folder restore never touches paths outside the
  game folder; the saves restore only the save root recorded in `saves-restore.txt`.
* 6 minutes of game lifetime per run at most (final screenshot, quit and kill included).
  Steam shows you as playing Death Stranding during a run.

## Tests (no game)

* `st_testenv_tests`: command parser, scenario language (and every scenario file),
  session.ini text (incl. `automation_save_root`, the `--no-mod` file), the save
  redirect's path mapping (`saves::Map`: case, separators, `\\?\`, `.`/`..`, siblings,
  idempotence, log prefixes), backup/restore of a scratch game folder (incl. a tampered
  manifest), save manifests, the saves guard (backup, seed + probe, unchanged check,
  changed/removed/added files and folders restored, new files moved to `saves-added`,
  second check clean), Steam cloud_log filtering.
* `st_testenv_probe_private`: the probe through the supervisor (private desktop + job).
* `st_testenv_e2e`: `st_testenv_fakegame.exe` copied as `ds.exe` (imports winhttp like
  ds.exe, polls XInput through `xinput1_4` like ds.exe, D3D12 flip-discard), the real
  `winhttp.dll` and supervisor, `fake-smoke.txt`; checks decoded screenshots, the
  virtual pad reaching the game's own `XInputGetState`, `GetForegroundWindow`, quit,
  restore, and the save redirect: a scratch "real" save root (`--saves`), the fake game
  reads the seeded profile (also with `/`), writes a checkpoint through a temp file +
  `MoveFileExW`, creates a folder, copies, deletes, `MoveFileW`, `GetFileAttributesA`,
  enumerates (8/8 ok), the real root stays byte for byte identical, the writes are in
  the scratch copy, `saves=redirected` in the log. Skipped (77) while a ds.exe runs.
* `st_overlay_harness` section 10: the automation parts of the overlay.
