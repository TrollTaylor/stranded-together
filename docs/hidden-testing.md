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
| Real game, 100% save (14:34-14:44) | **passes** with `-SeedSaves complete-100`: the save is owned by an Epic account (`epic_<id>`); the mod skips ds.exe's save-owner check (3 byte-checked `jne`->`jmp`, automation + `automation_foreign_saves=1` only), Continue loads Episode 15, `echo-holo-100` walks and shows the hologram trailing. See "Using another save". |
| Two games, hidden (16:15, 16:22) | **pass** with `-Scenario two-games -SeedSaves complete-100`: host + guest ds.exe on one private desktop, the guest survives the single-instance mutex (`mutex=renamed`), both link over UDP loopback (`[link] CONNECTED` on both), both show the other's hologram (`partner=visible` on both snapshots), both saves redirected to their own scratch copy, real tree unchanged, 0 uploads. See "Two games". |
| Two games, real bodies (19:31-19:36) | **pass** with `-Scenario two-games-real` and `-Scenario two-games-real-fallback` (`-SeedSaves complete-100`): each game shows the other as a solid character copying its animation (guest sees Sam, host sees Fragile; Cliff in a scratch variant), and a host whose guest sends no joint frames shows the hologram. Both games exit 0, real tree unchanged, 0 uploads. These runs used a build from before the 19:57/20:05 changes; on the final build only `two-games-real` was rerun (20:24, pass). See docs/real-partner-body.md section 5 and the "Two games" run table. |
| All 16 real bodies, solo (20:01) | **pass** with `-Scenario real-body-tour-all -SeedSaves complete-100`: every body_table id loads, maps all 77 joints and mirrors Sam's pose (run 20261116-200116). See docs/real-partner-body.md sections 1 and 2. |
| Test-save session, hidden (16:19) | **pass** with `-Scenario sandbox-alive -SandboxSession -SeedSaves complete-100`: the launcher's visible-play session.ini (no `automation=1`) redirects in DllMain and bypasses the owner check (`[sandbox] ...`). See "Test-save session". |
| Real game, walking (13:54, 13:56) | **passes**: the virtual pad's left stick walks Sam (20.2 m in a 6 s hold), the right stick turns the camera; `echo-holo` shows the echo partner hologram trailing Sam. The first runs "did nothing" because this save holds the player in a scripted BT scene ~5-20 s after it loads, exactly when they pushed the stick. See "Pad input in gameplay". |

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
one-shot file, and starts a thread that only does file I/O. It never writes game
memory (the one exception: with `automation_foreign_saves=1`, written only for `-SeedSaves`,
DllMain flips 3 byte-checked branch bytes, see "Using another save") and reads it only through `ReadProcessMemory` (the `pad` result's "game pad
state", see "Pad input in gameplay"; a bad page fails the read instead of faulting);
game actions are queued to the existing debug-action queue and run on the
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
| `pad <buttons> <ms> [lx= ly= rx= ry= lt= rt=]` | virtual XInput pad 0: `A`, `A+START`, `DPAD_DOWN`, `none`...; then idle but connected (a last word `keep`: the pad stays held after the command, for mid-walk screenshots; `pad none 0` releases it). Result: pad reads, `player moved <m> m (speed <v>)` (x/y distance of the local player over the hold) and, for holds >= 600 ms, the `game pad state` 300 ms in (see "Pad input in gameplay") |
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
reads, foreground answers, `WM_ACTIVATEAPP` counts, silenced audio buffers, snapshot),
`out\ready.txt` when the channel is up.

Isolation hooks (overlay, automation only; `automation_isolate=0` turns them off):

* **Pad**: ds.exe calls `LoadLibrary("xinput1_4.dll")` + `GetProcAddress("XInputGetState")`
  at 0x1419B1C57 and polls through that pointer (+0x16818, call at 0x1419B1DC7);
  `XINPUTUAP.dll` (delay-loaded) is only its fallback. Both exports are hooked (the
  overlay previously hooked only XINPUTUAP, so the menu's pad blocking now also works).
  In automation only virtual pad 0 exists; real pads read as disconnected. The packet
  number changes with every state change.
* **Rumble**: ds.exe also takes `XInputSetState` from xinput1_4 (pointer at +0x16820,
  called by its vibration routine 0x1419B24F0 with the pad index). Since pad 0 reads as
  connected, the game rumbles it (run 14:07: 924 calls, the first at 32639/21074 in the
  BT scene); unhooked, that went to the user's real controller in XInput slot 0. In
  automation both exports' `XInputSetState` are hooked: index 0 answers `ERROR_SUCCESS`
  and is dropped, other indices "not connected"; nothing reaches the system (status.txt
  `xinput_set_hook`, `vibration_kept`). Not isolated: PlayStation/HID pads and Steam
  Input, which ds.exe reads itself (`FUN_1419B3090` devices 1, 2, 4; it switches a
  controller to whichever device shows activity, `FUN_1419B2680`). Touching a real
  DualSense during a hidden run can steer the hidden Sam, and the game may write its
  rumble/light bar. Keep such pads idle or unplugged during hidden runs.
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
* **Activation**: the overlay posts `WM_ACTIVATEAPP(TRUE)` to the game window when it
  attaches and before every `pad` hold, and hands any `WM_ACTIVATEAPP(FALSE)` to the game
  as `TRUE` (`st::overlay::input::AssertActive`; counted as `activate_app_*` in
  status.txt; the first 24 activation/focus messages are logged). ds.exe sets its
  app-active flag only from that message and its pad reader ignores "mode 1" controllers
  while it is clear (see "Pad input in gameplay"). Defensive only: in every real run so far
  the flag was already set (the window is activated on its own desktop when it is created,
  no `FALSE` ever arrived) and the controller in use is mode 0, so this was not what
  stopped the sticks.
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
12345678): `autosaveN\checkpoint.dat`, `manualsaveN\...`, `quicksaveN\...` (0..22),
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
per redirected place (`[auto] saves: read DeathStrandingDC\12345678\profile -> scratch`,
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

## Pad input in gameplay

The first `echo-holo` run (12:55) pressed A at the title fine, but in the world
`pad none 5000 ly=32767` and `pad none 900 ly=32767 rx=32767` neither moved Sam nor
turned the camera, although ds.exe read the pad ~5000 times.

**The input path (static, ds.exe).** `FUN_1419B1D30` polls `XInputGetState` through the
pointer at +0x16818 (call 0x1419B1DC7) into a ring of 0x2D0 states per pad (32 bytes:
`XINPUT_STATE`, packet number as u64 at +0x10, result at +0x18; the ring only advances
when the packet number changes). `FUN_1419B1EE0` walks the ring back over the packets newer
than the caller's last one (`min(last, newest-1)`, so the newest state always counts: a
held stick with an unchanged packet number is still read), ORs the buttons and turns the
newest sticks into floats with a 7849 deadzone (`(|v|-7849)/24918`, clamped to 1). The pad
controller (`FUN_1419B3090`; device at +0x28: 1 PlayStation pad, 2 Steam Input, 3 XInput,
4 HID, switched to whichever device shows activity) writes a 0xB0-byte pad state (LX LY
RX RY floats first). Before that it checks `controller+0x68 == 1 && !platform+0x68` and
then answers an empty pad (state 2): platform (`DAT_14577D3F0`)+0x68 is the app-active
flag, written only by the window procedure 0x141A19BE0 on `WM_ACTIVATEAPP`
(`mov [rdi+0x68], (wParam == 1)` at 0x141A19ECA). The input manager `DAT_147BE6330`
keeps 7 player slots (0xF0 bytes; +0x30 controller, +0x40 the pad state, +0xE4 state,
+0x108 player id); the action layer (`FUN_14358C0E0`, `FUN_1432960C0`) maps it and treats
keyboard/mouse (`DAT_147BE69F0` keys, `DAT_147BE69F8` mouse records) as "in use" when they
show activity.

**What the game actually saw (runs 13:40-13:43).** The `pad` command now reads that state
back through `ReadProcessMemory` (gated on the pad reader's prologue bytes, so another
ds.exe build reads nothing):

```
pad none 6000 ly=32767 -> app_active=1 mouse=idle keys_down=0
                          slot0(player=0 state=1 device=3 mode=0 pad=0 sticks=0.00,1.00,0.00,0.00)
pad none 1500 rx=32767 -> slot0(... sticks=0.00,0.00,1.00,0.00)
```

So the stick reached the game's input manager at full deflection: XInput device, pad 0,
mode 0 (not gated), app active, keyboard/mouse idle. Nothing in the input path dropped it.

**Root cause: the save, not the input.** A run that pushed the stick in 1.5 s steps from
the moment the player appeared (`diag2`, 13:45) shows it: the player appears at 31.0 s;
Sam walks at ~4 m/s from 31.8 s, stops at ~36.5 s and does not move for any stick until
~49 s, then walks again (4.0-4.2 m/s). In that window the save (Incinerator West of
Capital Knot City, Episode 1, right after the cremation) plays its scripted BT scene
("We've got company": Sam crouches, the Odradek opens); START and B do nothing there either.
The 12:55 run and the first `walk` runs pushed the stick 8 s after the player appeared
(39-49 s), entirely inside that hold; the run that pushed the stick at 49 s (`diag1`)
walked. The camera stick is held in the same scene. The end of the hold varies a little
per run (13:54 and 14:07: a 1 s push 18.8 s after the player appeared still moved Sam
0.02 m, the next one at ~20 s moved him 2.6-2.9 m), hence the probe loop.

**What the scenarios do.** After `waitplayer`, wait 17 s, then
`until 45 300 ly=32760 -> ok: .*player moved ([1-9]|[1-9][0-9]+)\.[0-9]+ m => pad none 1000 ly=32760`
(1 s pushes until one moves Sam >= 1 m; the odd `ly=32760` makes the probe's log lines
unique, since `waitlog`/`until` match any line of the whole log), and verify a walk with
`waitlog 5 pad none 6000 ly=32767 -> ok: .*player moved ([5-9]|[1-9][0-9]+)\.`. Do not walk
out onto the apron: the BTs there catch Sam within seconds (13:50 run: tar, "Shake Off",
then a capture scene). The scenarios turn the camera round first (right stick at full
deflection: ~1.3 s per 180 degrees) and walk back into the incinerator building (a hall
~40 m deep). Movement is camera-relative: `ly=-32767` walks Sam towards the camera.

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
| `20261116-125503-echo-holo` | old `echo-holo` (stick 8 s after the player appeared) | steps ok, but Sam did not move (543.37,330.40 -> 543.36,330.40) and the camera did not turn: inside the scripted BT hold | confirmed, unchanged |
| `20261116-133628-walk` | first `walk`, with the activation fix | same: the 6 s `ly` moved Sam 0.03 m; `WM_ACTIVATEAPP(TRUE)` seen 4x, never FALSE | confirmed, unchanged |
| `20261116-134015-walk` | + game pad state read back | the game's input manager held `sticks=0.00,1.00,..` / `0.00,0.00,1.00,..` (XInput, mode 0, app active): the input arrived | confirmed, unchanged |
| `20261116-134208-walk` | + keyboard/mouse activity | mouse idle, 0 keys down; still no movement at 40-48 s | confirmed, unchanged |
| `20261116-134339-diag1` | START, B, DPAD_RIGHT, then `lx` (scratch scenario) | START/B did nothing at 43-46 s; `lx` at 49 s walked 1.8 m (speed 3.98), camera turned | confirmed, unchanged |
| `20261116-134510-diag2` | 14 x (1.5 s `ly`, snapshot) from the player's appearance (scratch scenario) | walks 31.8-36.5 s (4.15, 3.88 m/s), held 36.5-49 s, walks again from ~49 s (4.0-4.2 m/s, 16 m): the scripted hold | confirmed, unchanged |
| `20261116-134759-cal1` | camera calibration (scratch scenario, role echo) | right stick ~1.2-1.5 s per 180 degrees; 9.5 m in 3 s; hologram partner visible; BT tar on the apron at ~63 s | confirmed, unchanged |
| `20261116-135029-echo-holo` | `echo-holo` draft walking out onto the apron | gate after 2 probes, 17.65 m in 6 s, then caught by BTs (tar, "Shake Off", capture scene) | confirmed, unchanged |
| `20261116-135236-expl1` | walking into the incinerator building (scratch scenario, role echo) | 20 m into the hall, hologram trailing Sam in view, no BTs | confirmed, unchanged |
| `20261116-135430-echo-holo` | final `echo-holo` | **pass**: 19.9 m in 6 s, camera round, hologram trailing Sam by ~3-5 m in `05-trailing-a`..`07-trailing-c` (`06-trailing-b` clearest), merged at `08-stopped` | confirmed, unchanged, 0 uploads |
| `20261116-135611-walk` | final `walk` | **pass**: gate after 2 probes, 20.18 m in 6 s (checked by `waitlog`), camera turned in `03`/`05` | confirmed, unchanged, 0 uploads |
| `20261116-143459-smoke` | `smoke -SeedSaves complete-100` (first bypass run) | **pass**: `foreign saves: check bypassed` (3 sites), no "corrupted" dialog, local player at (-1100.18, 273.92, 352.69), but the screenshots were still the loading screen (12%, 14%): this save loads longer | confirmed, unchanged, 0 uploads |
| `20261116-143642-seed-discover` | screenshot every 12 s after Continue (scratch scenario, role echo) | world shown ~20 s after the player entity first appeared (entity re-created at 49.8 s); Episode 15 garage, hologram visible; Sam sits down after ~1.5 min idle; the game autosaved into the scratch copy (`autosave4`) | confirmed, unchanged, 0 uploads |
| `20261116-144148-echo-holo-100` | `echo-holo-100 -SeedSaves complete-100` | **pass**: control after 3 probes, 16.80 m in 6 s, hologram trailing in `06-trailing-a` / `08-trailing-c`, debug menu `10-debug-menu` | confirmed, unchanged, 0 uploads |
| `20261116-144316-smoke` | `smoke` (own saves, after the change) | **pass**, no `foreign` line anywhere, `"foreign_saves":false` | confirmed, unchanged, 0 uploads |
| `20261116-144423-echo-holo-100` | `echo-holo-100 -SeedSaves complete-100` again | **pass**: 3 probes, 16.90 m in 6 s | confirmed, unchanged, 0 uploads |
| `20261116-140715-echo-holo` | `echo-holo` after the review (XInputSetState hooks) | **pass**: gate after 2 probes, 20.12 m in 6 s, hologram trailing Sam in `05`..`07`; the game's 924 rumble calls answered by the mod, none reached a real pad | confirmed, unchanged, 0 uploads |

Every run: no window of the job on the user's desktop, input desktop `Default`
before/after, no violation, game folder restored (`restored.txt` ok), no `error.txt`.

## Scenarios (`tools/testenv/scenarios/*.txt`)

```
role solo|echo|host          session role (default solo); host: a second (guest) game too ("Two games")
guest unredirected           role host only: the guest's session.ini is role=guest host_save=1 WITHOUT any save
                             redirect (no automation=1, no sandbox_saves); the supervisor expects its mod to end it in
                             DllMain (exit 0x53540006 + sandbox-refused.txt naming host_save) within 15 s and stops
                             the whole run otherwise (summary.json "guest_refused_as_expected"); no @guest step or ini
ini <key>=<value>            an extra session.ini line, for every game of the run; only partner_body,
                             partner_character, my_character, self_character, partner_holo, partner_marker, partner_voice,
                             partner_activate, pose_hook, echo_delay_s, echo_offset_m, send_hz,
                             interp_delay_ms, name, name_tags, partner_collision,
                             partner_collision_layer, partner_collision_height, partner_gear, name_tag_font,
                             smooth_pose, frame_cap, audio_probe, self_voice, voice_sync, voice_subtitles, sound_sync,
                             sound_steps, partner_volume, self_headgear, self_feet_hide, boots_sync,
                             vehicle_sync, vehicle_overlap_hide, vehicle_look, vehicle_packs, vehicle_sound,
                             vehicle_real_native, host_save, ride_together, partner_vehicle_damage,
                             coop_test_build (the host-held save session, docs/coop-session.md;
                             coop_test_build: a fake game build, honoured in hidden runs only)
                             (testenv_core ScenarioIniKeyAllowed; a scenario
                             frame_cap is written after graphics low's 30, so it wins)
@host ini <key>=<value>      the same, for the first game only; @guest ini for the guest game only (role
                             host). Written after the shared lines, so they win (the mod keeps a key's last
                             value). @both ini is refused (plain ini already goes to both); ini cannot be
                             optional
graphics low [fps]           30 fps cap, appdir settings.cfg + profile overrides (see "Two games");
                             fps (30..240) replaces the preset's "fps" / "limit_fps" 30 (frame-rate tests)
@guest <step>                the step goes to the guest game (role host); @both <mod command> to both
timeout <s>                  steps limit, 30..360 (default 360). The game's whole lifetime is capped
                             at 360 s: steps get at most 315 s, the last 45 s are kept for the
                             final screenshot, quit, exit and kill
waitready <s>                until automation\out\ready.txt exists
waitlog <s> <regex>          until a stranded_together.log line matches (ECMAScript)
nolog <regex>                fails at once if any log line so far matches (a negative check; @guest too)
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
* `walk.txt`: Continue, wait out the scripted BT hold, 1 s probes until Sam moves, camera
  round (`rx`, 1.3 s), 6 s walk into the incinerator building (must move >= 5 m), a quarter
  turn left; snapshots and screenshots `02-control` .. `05-turned-left`.
* `echo-holo.txt` (role echo): the same start, a 6 s walk into the building, the camera
  turned round to look back, then 3 x 1.5 s walking towards the camera with screenshots
  `05-trailing-a` .. `07-trailing-c` of the hologram partner trailing 2 s behind (it fades
  within 2 m, so only while Sam keeps moving), `08-stopped`, `log_status`, the menu.
* `echo-holo-100.txt` (role echo, with `-SeedSaves complete-100`): Continue into the 100% save,
  1 s probes until Sam moves (the world loads 5-20 s after the player entity appears), 6 s walk
  (>= 3 m), camera round, 3 x 1.5 s towards the camera with screenshots `06-trailing-a` ..
  `08-trailing-c`, `09-stopped`, `log_status`, `10-debug-menu`.
* `real-body-tour.txt`, `real-body-tour-all.txt`, `real-body-diag.txt`, `echo-real-100.txt`, `echo-real-fragile-100.txt`: the real
  partner body (docs/real-partner-body.md). They use `pad ... keep` (the pad stays held after the command, so
  screenshots are taken mid-walk; `pad none 0` releases it). `real-body-tour-all.txt` spawns every id
  1-16 in mirror mode (idle and two walk frames each); each body's `waitlog` names only that body,
  because the regex searches the whole log, and is optional, so a body that never loads does not end
  the tour.
* `self-view.txt` (role solo, `my_character=fragile`, `self_character=1`): the self view
  (docs/real-partner-body.md section 8): standing, looking at the camera, walking and running towards
  and away from the camera as Fragile; a game hide reason simulated (`self_view 2 26 1`: the body goes,
  reason 63 cleared, nobody visible; cleared again: Fragile back), then `self_view 0` (Sam again, for
  comparison), a stray reason 63 with no body (`self_view 2 63 1`: repaired in the next tick, Sam stays
  visible) and back on with the other follow modes. `self-view-edges.txt`: the same start, then the purple
  roadster next to it (the scenario's frames call it the trike; X: get
  on, ride, X: get off) and a rest (hold B: sit, hold X: Sleep, B: stand up). `self-view-menu.txt`: the same start,
  then the on-foot cargo menu (START opens the cuff map with the "Sam" entry focused, A on it = "Manage Cargo";
  B closes it, `until` steps wait on `[menu] subspace state 5 ... (opens N)`): first with the menu figure off
  (`self_view 3 0`: Sam), switched on while open (Fragile in his place), the figure turned, closed; three more
  open / close cycles; then the self view off and the menu once more (Sam with every part back).
  `two-games-real-self.txt`
  (role host): `two-games-real` with `@guest ini self_character=1`: the guest sees itself as Fragile, the
  host never gets a self body (`nolog`).
* `collision.txt` (role echo), `collision-off.txt` and `two-games-collision.txt` (role host): player collision
  (docs/real-partner-body.md section 11). The echo partner is pinned ahead of Sam (`action collision 3`), Sam
  walks into it at full and half stick, round it and on, with the distance logged every tick (`action collision
  2`); the `-off` copy runs the same steps with `partner_collision=0`. `collision.txt` first pins the partner
  7 m ahead (the capsule stays out of the world beyond 4 m). Two games: the guest walks the host's path
  into the host's standing body, then each partner is pinned 30 m ahead for the far tag (` · ` distance).
* `collision-climb.txt` (role echo): no climbing on the partner and the game-font name tags
  (docs/real-partner-body.md 11.5 and 10.6). The default 4.0 m capsule pinned ahead; Sam walks into it from four
  directions pressing A (held, repeated), X and Y; the trace logs Sam's height. Control: `action collision 5 0`,
  Sam's own 1.65 m capsule, where the game offers Climb. Then the tag near, far and self, and the game's own
  `Map/Information` header (START) for comparison. `collision-layers.txt` and `collision-height.txt`: the
  research runs (layers 9, 25, 10 with `action collision 4`; heights 2.6, 2.0, 3.2 and 1.65 with
  `action collision 5`).
* `two-games-all.txt` (role host): name tags, gear and collision together, all on by default. The host
  Sam with `name=TagPlayer`, the guest Fragile with self view and `name=TagFriend` (different names: no
  `(host)`/`(guest)` suffix, 9 B labels). Different gear (`gear 3 1` on the host, `gear 3 2` on the guest),
  each checked on the other game (`the partner's gear: ... cargo 3` / `cargo 2`); the guest then walks into
  the host's standing body with the distance traced on both games, and walks back so the host sees her back.
  Note: `waitlog` matches any line of the log so far, so each check names text that cannot have appeared
  before (the cargo counts here).
* `audio-probe.txt` (role solo, `audio_probe=1`) and `audio-probe-fragile.txt` (the same steps with the
  `fragile_suit` self view): docs/audio-research.md Phase 0. Idle, walk, run, jumps (A while running), seven
  test situations through the game's voice queue (`audio_probe 2 <n>`), the assault rifle aimed and fired,
  the cargo menu, idle again, then the dump (`audio_probe 1`). The hidden game is silent: the evidence is
  the `[audio]` log lines only.
* `self-voice-fragile.txt` (role solo, `self_voice=auto`, `audio_probe=1`, the `fragile_suit` self view),
  `self-voice-own-hook.txt` (the same set-up without the probe: self_voice's own hook) and `self-voice-sam.txt`
  (Sam, `partner_body=real` for the menu log lines; the control, not armed): docs/audio-research.md Phase 1
  (section 14). The audio-probe steps, then Manage Cargo opened with `until ... subspace state 5 ... (opens N) =>
  pad A` and the figure asked to speak (`self_voice 2 0`, `2 1`); on the Fragile runs the mute is switched off
  (`self_voice 1 0`) for a few talks and a second menu, then on again. `nolog` checks that no unmuted talk of
  the local player was logged before the mute-off section. Evidence: `[voice] counts` / `status` lines,
  `[audio] talk #N MUTED`, the voice posts and the dump's per-event post counts. *(2026-11-18)* Since the talk runs and
  only its voice post is silenced (audio-research.md section 17.2), the Fragile runs check instead `nolog \[voice\] Sam's
  muted voice: no PostEvent hook` and `nolog \[voice\] an external-source post .* was not matched to a muted talk`; the
  own-hook run (no probe, `sound_sync=0`) also asks `ReceivedLike` with `self_voice 3 0`.
* `grunts-fragile.txt` (role solo, the `fragile_suit` self view, `self_voice=auto`, `sound_sync=1`, `audio_probe=1`):
  docs/audio-research.md section 18. Idle, walk, run, jumps while running, the Jumping and OutOfBreath situations, idle,
  with Sam's muted voice stopped; the same moves with v0.11's volume 0 (`self_voice 4 0`, the "before"), with the mute off
  (`self_voice 1 0`, the control) and stopped again. Evidence: the probe's `[audio] voice after mute` meter lines and the
  `[audio] voice bus 0x7CC88FD9 loud from` timeline next to the `voice post #N` lines, `[voice] counts` (`stopped N of M`).
  `nolog` checks that no post was refused as not one of Sam's events and that no stop call faulted. *(2026-11-18)* The other
  self-voice scenarios now `nolog \[voice\] an external-source post .* is not one of Sam's voice events`.
* `bb-answer-selfview.txt` (role solo, the `fragile_suit` self view, `self_voice=sam`, probe): `ReceivedLike` asked with
  `until ... queued 1 => action audio_probe 2 0`, then 15 s; the control for "BB's answer" (section 17.2).
* `two-games-audio.txt` (role host: host Sam, guest `fragile_suit` with self view; `self_voice=auto`, `voice_sync=1`,
  `sound_sync=1` on both, no probe: what the launcher writes by default) and `audio-keys-off.txt` (role solo, Sam, no
  audio key): the final runs of audio-research.md section 17. The first: the host's `ReceivedLike` (`self_voice 3 0`
  until the queue takes it) must play on the guest (`partner line seq ... = 1`), the guest's own lines are silenced and
  sent as kind 1 only, then (test) her lines as Sam must play on the host; then the two-games-sound steps. The second:
  `nolog` of `[psound]`, `[pvoice]`, `[audio]` and of any audio key in the `[config]` line.
* `sound-sync-volume.txt`: `sound-sync-echo` with `partner_volume=50` (`nolog ... SetGameObjectOutputBusVolume(our object
  N) failed`).
* `voice-sync-echo.txt` (role echo, `voice_sync=1`, `audio_probe=1`, a real Sam echo partner 1.5 m to the side, 2 s
  behind), `two-games-voice.txt` (role host: the host Sam, the guest `fragile_suit` with self view, `voice_sync=1`,
  `self_voice=auto` and the probe on both) and `two-games-voice-own.txt` (the same without the probe; the guest with
  `voice_subtitles=1`): docs/audio-research.md Phase 2 (section 15). Test lines (`voice_sync 1 <n>`), spoken
  situations through the game's queue (`audio_probe 2 <n>`, probe runs only), runs with jumps on each side; the
  guest's efforts must go out as kind 1 only (`nolog \[pvoice\] sent seq [0-9]+ kind 0` on the guest, `nolog
  \[pvoice\] partner line seq` on the host), then the guest sends as Sam (`voice_sync 2 1`, test only) and the host
  must play her lines. Evidence: the `[pvoice]` sent / received / resolved / `PlaySentence(...) = 1` / talking entity
  lines, and with the probe the `[audio] PlaySentence #N ... -> 1` and `voice post ... m from the partner voice
  entity` lines.
* `sound-sync-echo.txt` (role echo, `sound_sync=1`, own hooks, a real Sam echo partner 1.5 m to the side, 2 s behind),
  `two-games-sound.txt` (role host: the host Sam, the guest `fragile_suit` with self view, `voice_sync=1`,
  `self_voice=auto`, `sound_sync=1` and the probe on both) and `two-games-sound-own.txt` (the same without the probe):
  docs/audio-research.md Phase 3 (section 16). Each side walks, runs, jumps and fires the rifle while the other waits
  for `[psound] partner post steps|foley|weapon ... -> pid [1-9]...`; then each opens the cuff map while the other runs
  (`waitlog ... gate closed: ... (HandcuffDevice)`, closed again with A (the map's notice) and B until `gate open
  again`). Evidence: the `[psound]` mirror / partner post lines, the gate lines and the two `status` / `counts` lines.
* `weapons-solo.txt` (role solo, `my_character=fragile_suit`, self view, a debug Fragile with our gear) and

  `two-games-weapons.txt` (role host, the guest `fragile_suit` with self view): weapons in hand
  (docs/real-partner-body.md section 17). The 100% save's equipment wheel with the game's own inputs: D-pad right
  held, then the stick pushed, then centred, then the D-pad let go (`pad dpad_right 600 keep`, `pad dpad_right
  900 lx=.. ly=.. rx=.. ry=.. keep`, `pad dpad_right 400 keep`, `pad none 0`; the wheel keeps its highlighted item
  and the stick never turns the camera). Upper left: the assault rifle; top: the blood grenade; upper right: the
  electric grenade; right: empty hands. Aim `lt=255`, fire / throw `rt=255`. In two games the guest walks past
  the host and turns round, so both cameras look at the other player's front.
* `self-fit.txt` (role solo, `my_character=fragile_suit`, self view, the cap on) and `two-games-fit.txt` (role
  host, the guest `fragile_suit` with self view): the self view fit (docs/real-partner-body.md section 16). Each
  view with the fit on and off (`self_view 5 0/1`), the same camera: back, side and front solo, a running burst,
  Sam back after `self_view 0`; in two games the host's view of the guest front and back, the guest's fit on
  and off.
* `self-fit-head.txt` (role solo, the cap on): the fit's head lift at 0 / 50 / 75 / 100 % and the fit off, front
  and side (`self_view 7`, `self_view 5`); running with the same pose on and off (`self_view 6`), uncapped and at
  30 fps (`smooth 2 30`), and with the fit off (the leak check's comparison); last another character through
  the self view's body switch (`self_view 1 3`). `log <text>` marks each phase in the log for the `[self] fit`
  lines.
* `self-headgear.txt` (role solo, `fragile_suit` self view, the cap, then the glasses), `echo-boots-headgear.txt` (role
  echo: the same self view and a `fragile_suit` echo partner 1.2 m beside it), `echo-boots-sam.txt` (role echo, a
  partner Sam body) and `two-games-boots-headgear.txt` (role host: host Sam, guest `fragile_suit`; run it with a 1080p
  settings.cfg): headgear and boots (docs/real-partner-body.md sections 20 and 21). Each view "before" (v0.11: `self_view
  10 0` the head lift, `self_view 12 0` her feet kept, `gear 21 0` no boots item) and "after"; front, back and side
  close-ups, tuning variants (`self_view 8/9`), the first steps of a walk as a frame sequence, a run at 30 fps; her
  feet at 100 / 1 / 30 % (`self_view 11`), the partner's boots prop with its feet kept and scaled (`gear 20`). Evidence:
  the `[headgear] self ...` / `partner head prop ...` and `[boots] boots' feet vs the drawn feet ...` meter lines and
  the screenshots. (A 3840x2160 `-SettingsCfg` with only the resolution keys still gave 2560x1440 solo.) The meters run
  in hidden tests only (`gear 19` turns them on in play). In `self-headgear.txt` each tuning variant is followed by the
  shipped offsets again (cap `8 0 50` / `9 0 -12`, glasses `8 1 11`), so the walk and the 30 fps run use them.
* `two-games-final.txt` (role host, the guest as the launcher writes it by default: `fragile_suit`, self view,
  fit and lift; the cap on): gear fit, boots and weapons (rifle drawn, aimed, fired, put away; the blood grenade)
  on both screens, the guest's back from the host, the self view off and on again, and the guest running with
  the fit on then off. Run it with a 1080p settings.cfg for detail: `-SettingsCfg <file>` with the `graphics
  low` preset's lines (`native/mod/sandbox.hpp` `LowGraphicsSettingsCfg`) but `rendering_width` /
  `window_right` 1920 and `rendering_height` / `window_bottom` 1080 gave 1920x1080 screenshots of both games at
  30 fps (run `20261117-220002`; the plain preset gives 1280x720).
* `gear-self.txt` (role solo, Fragile, self view) and `two-games-gear.txt` (role host, the guest Fragile with
  self view, `partner_collision=0`): the backpack and cargo on the bodies (docs/real-partner-body.md section
  12). Solo: the self view standing, walking and running, Sam with his real gear for comparison, a debug Fragile
  wearing our gear (`gear 4 1`, `real_spawn 3 1.6 1`) and a cargo change (`gear 3 3`, `1`, `0`: synthetic
  delivery cases). Two games: each side's own gear and the other screen's view of it, and the host's cargo
  change seen by the guest.
* `gear-full.txt` (role solo, Fragile, self view) and `two-games-full-gear.txt` (role host, the guest
  Fragile with self view): the full gear of v0.8 (docs/real-partner-body.md section 12). Solo: the self view
  with Sam's real gear, Sam for comparison, a debug Sam and a debug Fragile wearing copies of our gear
  entities and parts, a weapon from the equipment wheel (D-pad right held, stick up), mirror level 2, and
  G2's injected failures (`gear 5`, `gear 6`). Two games: each side's view of the other, many gear
  changes both ways (`gear 3 <n>` cycles, a prop respawn with injected start failures, props hidden
  behind the mod's back), then the guest takes a weapon from its equipment wheel.
* `two-games-gear-look.txt` (role host, the guest Fragile with self view, `partner_collision=0`): gear looks and
  the back cargo (docs/real-partner-body.md section 13). The guest walks up behind the host; the host's own back
  and the guest's view of it first with looks and the backpack pose off (the v0.8 view), then on; the cap put on
  through the host's accessory list (`gear 14 15 1`) gold, silver and colour 9 (`gear 15`), the suit colour
  (`real_outfit 4 9`), the host's side, synthetic stacks of 2, 5 and 9 cases on his back; then the guest walks
  past the host for her back on both screens, with 5 cases.
* `echo-gear-look.txt` (role echo, one game at full resolution, `echo_offset_m=1.4`, `echo_delay_s=0.3`): the same
  looks side by side: the echo partner (a Sam body wearing copies of our gear through every encoder and decoder)
  stands beside Sam, so each screenshot shows the real gear and the copies; looks off, then on; the cap gold, silver,
  colour 9; looks off again with fresh props (`gear 1 0` / `1`: the black cap of v0.8), then on; stacks of 3 and 9
  synthetic cases.
* `echo-hair.txt` (role echo, one game at full resolution, `echo_offset_m=0.9`): Sam's head on the partner Sam
  (docs/real-partner-body.md section 14). `gear 16 0` before the echo partner comes (the cutscene defaults),
  the camera round for both faces, then `gear 16 1`, the Bridges cap on (`gear 14 15 1`), the cap and the
  cyphead accessory off (`gear 14 53 0`); the camera a little each way for every state.
* `two-games-hair.txt` (role host, both players Sam, `partner_collision=0`): the same between two games, both
  directions. Each game pins its partner 1.6 m ahead (`collision 3 1.6`) and turns its camera a little, then
  round for its own Sam beside the partner. The host puts the gold cap on; the guest takes the cyphead off and
  puts a silver cap on, then off. Census of each game's own Sam and partner body at the end (`gear 9 0` / `1`):
  the head part flags must agree across the games.
* `two-games-looks-menu.txt` (role host Sam, the guest Fragile with self view, `partner_collision=0`): sections
  13-15 together between two games. The host walks ahead; on the guest's screen his partner body is pinned with
  its back to her (`collision 6 2.2`) and then facing her (`collision 3 1.6`), her camera turned a little for each
  shot and back. His back on both screens, the Bridges cap gold then silver (`gear 14 15 1`, `gear 15`) from
  behind and in front, the head census (host `gear 9 0`, guest `gear 9 1`), a stack of 5 synthetic cases; the guest
  walks past him for her back on both screens; then the guest opens Manage Cargo (START, A), must be shown as
  Fragile there (`[menu] showing 'fragile' in place of the menu's Sam figure`), turns the menu camera, closes it
  (B, START) and is Fragile in the field again.
* `outfit-probe.txt` (role solo, `pose_hook=1`): the partner Sam's outfit (docs/real-partner-body.md
  section 9). A debug Sam body 3 m ahead is dressed like our own Sam, then with explicit outfits. Then our
  own Sam's suit colour is changed (`real_outfit 4`) and the body follows.
* `two-games-outfit.txt` (role host): both players are Sam. Each game sees the other's Sam body in its gold
  suit. Then the host changes to colour 9, the guest to colour 0, the host to silver and back to colour 9
  (the guest's body must get colour 9 again after the variation change, `#2`); each time the other
  game's partner body follows.
* `name-tags.txt` (role echo, `name=TagEcho`, echo delay 6 s): name tags (docs/real-partner-body.md section
  10): the tag near, the self test above our own Sam, far (the echo offset puts the partner 40 m away; a
  camera sweep), none while the partner is behind the camera, none on the cuff map (START).
  `two-games-name-tags.txt` (role host): `two-games-real` with `name=TagPlayer` on both games: each shows
  the other's name with its role, `TagPlayer (host)` / `TagPlayer (guest)`.
* `two-games.txt`, `two-games-real.txt`, `two-games-real-fallback.txt` (role host): two games, see
  "Two games".
* `vehicle-echo.txt` (role echo, `echo_offset_m=4`, `vehicle_sync=1`): vehicle sync (docs/real-partner-body.md section
  22). `action vehicle_sync 9 -1094.4 276.9` places Sam beside the 100% save's parked purple roadster (where X got him on
  in `self-view-edges`; walking there with the stick missed it in two runs: the probe walk's length varies), then
  `until ... ours: roadster => pad X 150`; the echo copy (the roadster's hologram) with the echo Sam seated, a drive (hard
  left, straight, left: about 28 m, up to 13.7 m/s), stopped, looked at from four camera angles, off again (the copy stays
  parked), the copy override (`vehicle_sync 2 2` trike, `2 3` truck, `2 0`), and the mode switched off at run time.
  Evidence: `[vehicle] meter (5 s)` lines and the `vehicle_sync 3` trace (compare `trace tx` with `trace rx` + 4 m).
* `two-games-vehicle.txt` / `two-games-vehicle-guest.txt` (role host: host Sam, guest `fragile_suit` with self view,
  `partner_collision=0`): one player gets on its roadster and drives the same path, stops, gets off and is then placed
  more than 30 m from it (the copy goes); the other watches from `-1088.0 281.5` (about 5 m left of the parked roadster)
  with its camera untouched since the load, so the drive runs through the middle of its picture. The host drives in the
  first, the guest in the second (each watcher's camera must not have been turned by a drive of its own). Run with a
  1080p `-SettingsCfg`. The trace of both games is compared offline (the same QPC clock on one PC).
  `vehicle_sync=1` (the default) and `2` show the solid trike and truck copies (looks, packs; their collision turned off
  on each copy since section 22.11) with the roadster's hologram; `3` all three kinds as holograms (v0.13's default); `4`
  the roadster's only (the first milestone's default).
* `vehicle-garage-scout.txt` (role echo): the game's vehicle registry and the parking slots (`vehicle_sync 7`: the save
  holds trikes, a Kusanagi trike and trucks outside Mountain Knot City), then Sam on a grid over the garage lift and its
  buttons tried one at a time (none opened the garage menu; DPAD up opens photo mode).
* `vehicle-echo-trike.txt` (role echo, `echo_delay_s=0.5`, `echo_offset_m=4`): the roadster driven forward with the smooth
  mask 0 and back (LT) with 7, `vehicle_sync 13` at each segment's end (the frame-aligned meter); then
  `vehicle_sync 8 <side> <3 trike | 4 Kusanagi | 2 truck>` (beside the nearest of that kind, facing it) and X for each,
  a short drive, camera turns, off; the truck disposed of (hold START): the sender's "gone" path.
* `vehicle-echo-census.txt` (role echo): the census of the 100% save's vehicles (`vehicle_sync 19`: looks, carriers,
  child entities), then the truck and the trike ridden with the echo copy beside them (screenshots of both).
* `vehicle-echo-look.txt` (role echo, `echo_delay_s=0.5`, `echo_offset_m=4`): the gold truck and the purple / orange trike
  with the echo copy beside them, side views with looks and packs on, off (`vehicle_sync 14 0`, `15 0`: the copy in its
  own look, no packs) and on again; a short drive; the census of the copy (docs/real-partner-body.md section 22.7).
* `two-games-vehicle-look.txt` (role host, `graphics low`, `vehicle_sync=1`: the solid copies, the default since section
  22.11, which alone take looks and packs; also `vehicle-echo-look.txt`; host Sam, guest `fragile_suit` with self view): the host drives
  the gold truck, the guest the purple / orange trike, the host gets on the Kusanagi trike (its look not applied); the
  watcher's camera is turned onto its own vehicle before the drive (`vehicle_sync 18 2|3`) and onto the copy after the
  stop (`vehicle_sync 18 5`).
* `vehicle-echo-state.txt` (role echo, `echo_delay_s=0.5`, `echo_offset_m=4`, `vehicle_sync=1`; docs/real-partner-body.md
  section 22.12): the gold truck, the standard trike and the Kusanagi got on in turn; our vehicle's wear and lights
  (`our <key>'s state: wear ...`) on the echo copy (`wear / lights of the partner's ... on copy`, `... read back on copy
  ...`); side views with the looks on, off (`vehicle_sync 14 0`: the copy in its own look, the control) and on again;
  six synthetic cases on the truck's roof and three on the trike (`vehicle_sync 24`), then off; a short drive; the
  Kusanagi's wear and lights not applied (another model). Since section 22.13 also: on the trike, `vehicle_sync 1 3`
  (the hologram opt-in: `copy 'trike_hologram' shown`, screenshot `09b`) and `1 1` back (`09c`); the Kusanagi got on
  while the trike is still reported parked (`the partner's vehicle changed from trike to trike_kusanagi`: the copy comes
  back in its own look); then 64 s parked (`our parked vehicle ... still reported after 60 s`, `the partner's parked
  trike_kusanagi still shown after 60 s`, screenshot `11`). Final run `20261118-205413-vehicle-echo-state`.
* `two-games-vehicle-state.txt` (role host, `graphics low`, `vehicle_sync=1`, host Sam, guest `fragile_suit` with self
  view; section 22.12): the host's gold truck (rust-brown) with six synthetic cases, driven a few metres so the copy
  leaves the guest's own truck (overlap hiding), the guest's camera turned onto the copy (`vehicle_sync 18 5`); the
  guest's trike with three cases; the host's Kusanagi (not applied). The trike part's host views depend on the camera
  (run `20261118-202559`: rocks and another player's trike).
* `vehicle-echo-sound.txt` (role echo, `sound_sync=1`, `vehicle_sound=1`, `echo_delay_s=0.5`, `echo_offset_m=4`): our
  vehicle's sounds through STA1 v2 to our own Wwise objects at the echo copy (docs/real-partner-body.md section 22.9).
  The garage roadster: on, idle, forward, reverse (`sound_sync 3 30000`: the RTPC trace on both sides); the cuff map
  while seated (A, then B until `gate open again .*HandcuffDevice`); our sends held 3 s (`vehicle_sync 10 3000`);
  `sound_sync 4 0` / `4 1`; off; `vehicle_sync 1 0` (no vehicle reported). Then the standard trike (backed out with LT)
  and the truck. `sound_sync 0` after each step for the counters.
* `two-games-vehicle-sound.txt` (role host, `graphics low`, `sound_sync=1`, `vehicle_sound=1`, host Sam, guest
  `fragile_suit` with self view): the host's garage roadster with its own cuff map while seated (the sender's gate),
  the host's Kusanagi trike with the guest's cuff map while it drives (the receiver's gate) and its sends held 3 s and
  12 s, the guest's truck, the host's standard trike, got off, then `vehicle_sync 1 0` on the host. The watcher's camera
  is not aimed (the evidence is the `[psound]` lines; its screenshots may not show the copy). Since section 22.13 the
  truck step waits for `copy 'truck' shown` (the solid copy; run `20261118-211028` still waited for `truck_hologram`
  and failed there, games fine); final run `20261118-211341-two-games-vehicle-sound`.
* `vehicle-echo-collision.txt` (role echo): the copy override shows the truck's and the trike's solid copies, then their
  holograms, at the garage roadster's parked copy; `vehicle_sync 16` / `17` walk Sam into each from 1.5 m before its
  origin (from 5 m the walk drifted 2 m off the line, run 155304) and place him at its origin (`16 0.01`), and the same
  into our own roadster (the control); the roadster is got on and off again before the last part (its parked copy is
  reported for 60 s only before section 22.13; `vehicle_sync 8 1.2 1` turns Sam to face it first). Run `20261118-181155` (v0.13, the solid
  copies' collision still on): the solid truck stops him 2 m past its origin, he stands on the solid trike (1 m up); he
  walks straight through both holograms.
* `vehicle-echo-solid.txt` (role echo, `echo_delay_s=0.5`, `echo_offset_m=4`; docs/real-partner-body.md section 22.11):
  the solid copies with their collision off, at the garage roadster's parked copy (copy override `2 3` truck, `2 2`
  trike): `vehicle_sync 21` (their collision state), a view from 5-7 m, the centred walk from 1.5 m before the origin
  through it, Sam at its origin (`16 0.01`: dz must stay about 0.1 m) and 0.9 m before it (no Climb prompt). Then our
  own roadster got on, turned left out of its place, straightened (`lx=0`), and the solid truck copy pinned 8 m ahead of
  it (`vehicle_sync 22 8 0`) is driven through at full throttle; backed out with LT; the solid trike pinned 6 m ahead
  and driven through. The trace (`vehicle_sync 3`) shows the speed and height of our roadster along the pinned copy's
  axis (closest approach, speed before / in / after it, z steps). Final run `20261118-205827-vehicle-echo-solid`.
* `two-games-vehicle-trike.txt` (role host, `graphics low`, host Sam, guest `fragile_suit` with self view): the host's
  roadster in the garage (smooth mask 7 throughout since the review; until then 0 here, the baseline), the host's Kusanagi trike with its sends held 3 s and
  12 s (`vehicle_sync 10`), the guest's truck, the host's standard trike (backed out of its rocks with LT). The watcher
  stands 9-10 m beside its own copy of the vehicle and turns its camera onto it (`vehicle_sync 18` and `until ...
  camera aimed #n => pad none 200 rx=32767`). Note: `waitlog` / `until` search the whole log, so a regex must not match an
  earlier line (`ours: trike` matches `ours: trike_kusanagi`: `ours: trike (parked|getting|riding)`).
* `two-games-release.txt` (role host, the launcher's default keys: host Sam, guest `fragile_suit` with self view,
  `voice_sync=1`, `sound_sync=1`, `self_voice=auto`, partner collision on; the guest adds `audio_probe=1`): the release
  check of headgear, boots, the grunt mute and vehicle sync in one run. Her cap, then cap and glasses on both screens; her
  boots on the host's screen off, then on again with five frames right after (the boots prop must not show before it is
  posed and its parts mirrored); the first steps of her walk across his view; her walk, run and six jumps with the probe's
  bus meters and the `[voice]` counts; then the host's roadster drive seen by the guest (as `two-games-vehicle`), and
  the parked copy still there after he is placed away from it (`10-guest-after-host-left`; until section 22.13 it went
  beyond 30 m). Her camera faces the host for the mount (`06`) and is then turned back round
  (`@guest pad none 1300 rx=-32767`) to face his drive: without that turn (run `20261118-130121`) the copy left her view
  after the first drive frame. Run with a 1080p `-SettingsCfg`; final run `20261118-130839-two-games-release` (v0.11);
  after section 22.13 `20261118-211715-two-games-release` (default settings).
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
  session.ini text (incl. `automation_save_root`, `automation_foreign_saves`, the `--no-mod` file),
  the foreign-saves gate and byte check (incl. the site table against the installed ds.exe), the save
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
  the scratch copy, `saves=redirected` in the log. The fake game also keeps ds.exe's
  app-active rule (pads count only after `WM_ACTIVATEAPP(TRUE)`): it deactivates itself
  before the overlay attaches and again at frame 60; the e2e checks that the mod's post
  re-activates it, that the later `FALSE` reaches it as `TRUE`, and that the virtual pad's
  left stick (`pad none 300 ly=32767`) arrives. Skipped (77) while a ds.exe runs.
* `st_overlay_harness` section 10: the automation parts of the overlay.

## Using another save (`-SeedSaves`)

`Start-TestRun.ps1 -SeedSaves <folder or name under testenv-saves\>` replaces the save
*slots* (`autosave*`, `manualsave*`, `quicksave*`) in the run's scratch copy with the ones in
`<folder>\DeathStrandingDC\`. The user's `profile\`, `game_settings.cfg` and
`steam_autocloud.vdf` stay in the scratch copy, and the real saves are backed up, redirected away
from and verified exactly as in every run. `testenv-saves\` is git-ignored.

```powershell
pwsh -File tools\testenv\Start-TestRun.ps1 -Scenario echo-holo-100 -SeedSaves complete-100
```

**Why another account's save "is corrupted" (static, ds.exe).** The data part of
`checkpoint.dat` (after the `[Slot]` ini and the PNG; the three decimal sizes at the start of
the file give their lengths) and of `profile.dat` is: u32 magic `0xC0391E1D` (`0xC0391E55`
without the owner field), u32 seed, then everything XORed with one 16-byte key,
`MurmurHash3_x64_128(seed 42)` (`FUN_1418FE890`) of the seed plus 12 constant bytes (copied
from 0x143BA4460 to 0x14577B6B0 by a static initializer at 0x1417F3ED0). The key does not
depend on the account. Decrypted, the payload starts with the **owner**: u32 length +
`steam_<SteamID64>`; then the compressed save (`FUN_1418EC990`). The running account's owner is
the global string at 0x144598268, set by `FUN_1419FF950` as `"steam_%llu"` of
`ISteamUser::GetSteamID` (vtable +0x10); the writer `FUN_141A01B30` stores it into every save.
Three readers compare the two with the engine's string equality `FUN_14190B110`:

| Reader | Compare / branch | On a mismatch |
|---|---|---|
| `FUN_141A01ED0` (`sSaveDataAsynCallback`, mode 1: checkpoint loads, Continue / Load Game) | call 0x141A02932, `jne` at **0x141A02945** | `mov byte [r14+0x3A], 0x11`: the request is "broken", the menu shows *Save data is corrupted, and cannot be loaded.* (the data is still decompressed) |
| `FUN_1419FFA40` (synchronous read, e.g. the profile) | call 0x1419FFE3C, `jne` at **0x1419FFE4F** | returns false, `*mismatch = 1` |
| `FUN_1419FF5B0` (`SaveData: ImportDownloadedData`) | call 0x1419FF7DF, `jne` at **0x1419FF7F2** | state 0x11 |

`testenv-saves\complete-100` decrypts to the owner `epic_435f34e7...` (37 bytes): it was made by
the **Epic Games Store** build, not by another Steam account; the user's own saves say
`steam_<their SteamID64>`. Nothing else in the file is bound to the account, so the only thing in
the way is that comparison. A Cheat Engine "Disable Save Data Check" script almost certainly
patches exactly this (the `jne` after the compare, or the `mov [..+0x3A], 0x11`).

**The bypass (mod, `automation_foreign_saves=1`).** In `DllMain` (`st::automation::EarlyInit`),
after the save redirect is installed and its self-test passed, and only in a supervised
automation run (`foreign_saves::Decide`: key + `automation=1` + `ST_TESTENV_SUPERVISED` + redirect
active; otherwise nothing is written), the mod compares a 74-76 byte window at each of the three
sites with this ds.exe build (from the `mov rax,[0x144598268]` that loads the owner string,
through the call to `FUN_14190B110`, `test bl,bl`, the `jne` and the reject store). Only when all
three match does it turn each `jne rel8` (0x75) into `jmp rel8` (0xEB), one byte per site
(`VirtualProtect`, `FlushInstructionCache`); any difference installs nothing and logs where.
Nothing can fault: the image range is checked with `VirtualQuery` before it is read. Log line
(right after `[auto] saves=redirected`; also `foreign_saves=bypassed` in `saves-redirect.txt`):

```
[auto] foreign saves: check bypassed at 0x141A02945 (async checkpoint load FUN_141A01ED0), 0x1419FFE4F (sync read FUN_1419FFA40), 0x1419FF7F2 (ImportDownloadedData FUN_1419FF5B0)
```

The supervisor writes the key only with `--seed-saves` and the full mod (`te::WantForeignSaves`;
`--no-mod` installs no game patches) and records the log line in `summary.json`
(`foreign_saves`, `foreign_saves_evidence`). Normal play never sees the key (the launcher does
not write it; `config.cpp` only ignores it) and `Decide` returns `off`: ds.exe is not touched.
The redirect and the real-save check stay in force: a bypassed game only ever loads and writes
the scratch copy (its autosaves there carry the user's `steam_` owner). Tests:
`st_testenv_tests` (the gate, the supervisor rule, `session.ini`, the byte check with a
synthetic image, one differing byte, an already patched site, an unreadable image, and the site
table against the installed `ds.exe`), `st_testenv_e2e` (`--seed-saves` with the fake game: the
key is sent, the byte check refuses on a non-ds.exe image, the run goes on unpatched).

**What the 100% save does** (runs 14:34-14:44): Continue loads Episode 15 ("Pass the time until
the presidential inauguration") in a distribution centre's garage near Mountain Knot City, local
player at (-1100.2, 273.9, 352.5). The local player entity appears ~4 s after Continue but the
world keeps loading (black loading screen, 12-14% after 10 s) for another 5-20 s; then the player
entity is created again and Sam has free control (no scripted hold). Left idle for ~1.5 min Sam
sits down to rest. `echo-holo-100.txt` probes with 1 s pushes until Sam moves, walks 6 s
(16.8-16.9 m), turns the camera round and screenshots the hologram partner trailing
(`06-trailing-a`, `08-trailing-c`), then the debug menu (`10-debug-menu`).

Before the bypass: run 20261116-142026-seedload showed *Save data is corrupted* on Continue; with
the other account's `profile\` copied too, the game stalled before its first frame (run
20261116-141151-smoke: that `profile.dat` is `epic_`-owned too), which is why the user's
`profile\` stays.

## Two games (`role host`)

The launcher's local two-player test ([sandbox-and-local-test.md](sandbox-and-local-test.md)),
hidden: `Start-TestRun.ps1 -Scenario two-games -SeedSaves complete-100`.

* **Second game.** The supervisor starts the first ds.exe as before (`role=host`, UDP 27555 ->
  27556), waits for its save-redirect evidence, then starts a second ds.exe in the same job on the
  same private desktop with `ST_INSTANCE_DIR=automation\guest`: its mod folder
  `<mod>\automation\guest` (session.ini `role=guest`, 27556 -> 27555, `second_instance=1`,
  its own automation channel and scratch saves, removed with the automation tree), and the same
  evidence gate (`guest saves redirect: CONFIRMED`). `second_instance=1` renames ds.exe's
  single-instance mutex for that process only (`[mod] second instance: mutex=renamed (1 call(s)
  ...)`; without it the second ds.exe exits with code 1).
* **Guest seed (`-GuestSeed full|profile-only`, supervisor `--guest-seed`).** `full` (default): the guest's scratch
  is seeded like the host's (your saves, then `-SeedSaves`' slots). `profile-only`: the guest's scratch keeps only
  what is not a save slot (`profile\`, `game_settings.cfg`, `bindings.cfg`...): every `autosave*`/`manualsave*`/
  `quicksave*` folder is removed after seeding (`te::StripScratchSlots`, refused for a folder without the redirect
  probe, so never the real tree) and `-SeedSaves` is applied to the host only. That is the host-held save session's
  guest at a session start (docs/coop-session.md): it has no save of its own and gets the host's over the link.
  Logged as `guest seed: profile-only (N slot folder(s) removed ...)`; `summary.json` `"guest_seed"`; the guest's own
  "before" manifest is `saves-guest-scratch-before.txt` (so `guest_scratch_changes` lists only what the guest's game
  changed). Example:
  `Start-TestRun.ps1 -Scenario two-games-hostsave-continue -SeedSaves complete-100 -GuestSeed profile-only`. Ignored
  (logged) without a second game.
* **The host-held save scenarios** (`ini host_save=1`, docs/coop-session.md section 8): `two-games-hostsave-continue`,
  `-reload`, `-retry`, `-newgame`, `-linkloss`, `-late`, `-version` (all with `-GuestSeed profile-only`) and
  `guest-refuse-unredirected` (the `guest unredirected` directive); all pass (2026-11-19, docs/coop-session.md section
  9). `action coop_session 1 <ms>` drops every received datagram for ms on that game (hidden runs only; `@both` for an
  outage on both sides), `5 <type> <index>` / `6` / `7 [1|2]` make the host's in-world Load / QuitGame / checkpoint
  retry (`7 1` asks, `7 2` requests without a checkpoint), `8 <0|1|2>` hides the guest's waiting panel for a screenshot
  of its title (2: the input block lifted too, the gate alone holds the title), `9 <type> <index>` lets a slot stand in
  for a New Game's first save. The title's New Game path for a pad: one DOWN from CONTINUE (on a title with a save), A,
  LEFT + A (the confirmation selects Cancel), A on the difficulty, LEFT + A, A on Change Text Size.
* **The shared real vehicles scenario** (`ini host_save=1`, docs/real-partner-body.md section 23):
  `two-games-veh-identity` (with `-GuestSeed profile-only`): the host's table, the guest's binding (all bound at 0.00 m),
  both pools dumped (`vehicle_dump`), then a guest `vehicle_summon 3` through its gate to the host and back (the exact-id
  mirror); passes (2026-11-19, section 23.9). `action vehicle_share 5 <0..3>` / `6 <kind> [m]` (hidden runs only) pick
  the guest's mirror path / set the removal bit on the nearest vehicle. Stage 2 (remote driving, ownership, state;
  section 23.19): `two-games-veh-summon`, `two-games-veh-drive` (host drives the truck, the guest's camera near it:
  `action vehicle_drive 9` puts the watcher beside the vehicle it follows, `vehicle_sync 18` aims the camera),
  `two-games-veh-drive-guest`, `-drive-trike`, `-drive-roadster`, `two-games-veh-handoff`, `two-games-veh-remove`,
  `two-games-veh-persist`, all with `-GuestSeed profile-only`. `vehicle_drive 1` logs each vehicle's owner, follower,
  placement error and corrections; `4`, `6`, `7`, `9` are hidden runs only. Smoke run 2026-11-19 (section 23.20); all
  nine pass in hidden runs on 2026-11-19 (section 23.22). The scenarios name counted log lines (`follower of ...
  attached #2`, `our Sam drives ... #2`, `got off ... (#2)`): `waitlog` matches the whole log, so a repeated event must
  name its count.
* **The riding together scenarios** (`ini host_save=1`, docs/real-partner-body.md section 24): `seat-measure` (one game,
  echo: the seats measured per kind and the solo carry), `two-games-ride-truck`, `two-games-ride-roadster`,
  `two-games-ride-trike` (the guest rides, then the swap: the host rides), `two-games-ride-forced` (link loss, crash,
  the game taking Sam, the driver getting off, a second hold that jumps off the reversing truck, a roll-over; the
  re-drive waits on the ride module's seat counter `our Sam is seated in the truck (#2)`, since part 1's blackout can
  start a new link session that numbers the drive and the attach again) and `two-games-ride-forced-host` (the same forced exits with
  the host as the passenger, without the roll-over), the two-game ones with `-GuestSeed profile-only`; all pass
  on 2026-11-19 (section 24.10). `ride_test 8` puts a game's Sam at the passenger door of the vehicle its partner drives;
  the boards and exits are counted in the log (`carried on the truck ... #2)`, `off the truck ... (#2, crash, forced)`).
  `two-games-ride-truck` ends with `ride_test 10` on the host (a request and the module's stop in one call: the late
  ACCEPT is ended, the host walks away). All re-run after the review fixes on 2026-11-19 (section 24.10).
* **The save guard scenarios** (`ini host_save=1`, `-GuestSeed profile-only`, docs/real-partner-body.md section 24.12):
  `two-games-ride-save-host` (a save on foot runs at once; a save while the host rides is held, the ride ends, Sam is
  put beside the truck, then the save is written; the host loads it and stands where he was put),
  `two-games-ride-save-guest` (the same with the guest carried; no boarding while a save is pending; the hold off: the
  ride's last-resort exit), `two-games-ride-save-none` (nobody rides: no hold, no delay) and `two-games-ride-save-break`
  (the guest carried: a second DSCreateSaveGame while the first is held (`save_guard 1 4 2`: the holder keeps one
  request); a save while the truck drives at 8 m/s or more (`save_guard 6 40 2`); a link blackout on both games with a
  save; a save pending while the guest boards (`save_guard 6`); a save posted while the boarding request is on its way (a
  1 s blackout on the host delays the REQUEST: the ACCEPT finds the save pending, the carry is refused, END to the
  driver); finally no request lost, dropped or not written (`save_guard 0`'s counters)). `save_guard 1 4` forces a
  save through the game's own DSCreateSaveGame into the run's scratch copy (hidden runs only); the game's post refuses
  one now and then while Sam is carried (reason `0x400`), so the scenarios ask with `until ... => action save_guard 1 4 1`.
  Each save is named in the log (`#N (forced save Fn)`) and its `issued` / `finished ok` lines say how it ran (`at once`,
  `after the hold`, `the hold was off`): `waitlog` matches the whole log, so the checks name the forced save's number
  (and `save_guard 2 <n>` logs `where (check n)`, so the reload's check matches its own line, not the one before the load).
  The game's autosave cooldown (7 s after an autosave) refuses a second forced autosave sooner. All pass on 2026-11-19.
* **The vehicle hit scenarios** (`ini host_save=1`, docs/real-partner-body.md section 25): `two-games-vehicle-hit` (the
  host's truck into the guest at ~28 km/h, the guest's truck into the host, the host's Kusanagi trike into the guest at
  40 km/h), `two-games-vehicle-hit-fast` (the truck and the trike at 40 km/h: knocked down, not killed; the passenger
  thrown off in a crash, D3-3), `two-games-vehicle-hit-off` (`@guest ini partner_vehicle_damage=0`: the guest is not hit,
  the host still is), `two-games-vehicle-hit-roadster` (the roadster both ways at ~35 km/h, the direct path with 0x158) and
  `two-games-vehicle-bump` (the guest's truck rams the host's parked trike, a head-on, the partner capsule's vehicle
  rule), all with `-GuestSeed profile-only`; all pass on 2026-11-19 (section 25.9). `vehicle_hit 8 <m>` puts a game's Sam
  in front of the vehicle its partner drives; `vehicle_hit 12 3 -22` puts the Kusanagi trike onto the trucks' straight
  road (its own spot faces rocks), `vehicle_hit 12 1 8` the roadster 8 m ahead of the truck (22 m behind it, it faces the
  road's raised edge). Each hit is numbered in the victim's log (`hit Sam (#2)`) and in the
  driver's (`reports (#2)`).
* **Steps.** `@guest <step>` sends a mod command / `waitready` / `waitlog` / `until` to the guest;
  `@both` a mod command to both. Each game's log is matched separately. A guest that exits after
  `@guest quit` does not end the run; otherwise either game exiting does.
* **The second login.** The guest signs in to the game server with the same Steam account: the
  server refuses it and the game waits behind *"Unable to log in to Death Stranding game server.
  Launching in Offline Mode... Error Code: 50000"* until OK (run 20261116-155336, `zz-final-guest`).
  `two-games.txt` presses A on the guest until its partner spawns.
* **Real bodies.** `two-games-real.txt` (both `partner_body=real`, guest `my_character=fragile`) and
  `two-games-real-fallback.txt` (host real, guest hologram: the host falls back to the hologram) use
  `@host ini` / `@guest ini` for per-game session.ini lines. See docs/real-partner-body.md section 5.
  Behind the offline-mode notice the guest animates nothing, so it sends no joint frames: both scenarios
  press A on the guest first (run 20261116-192744 waited on the host first and failed).
* **Load.** Hidden games are never vsynced (`Present` answers OCCLUDED, the mod reports S_OK), so
  the first game rendered at ~1000 fps and starved the second (run 154851: the guest read its pad
  0 times for 40 s). `graphics low` writes `frame_cap=30` (the mod waits on the game thread once
  per presented frame): the host's CPU time per second alive fell from 8.1 s (run 154851) to 1.9 s (run 161556).
* **Frame rates.** `graphics low` also writes `"fps" "30"` and `"limit_fps" "30"` into the redirected
  settings.cfg, and that limit wins over any frame cap: run 20261117-144836 stayed at 30.0 fps with the cap
  off. For other rates in two games write `graphics low 120` (the same preset with `"fps" "120"` and
  `"limit_fps" "120"`; run 20261117-154136: 64-69 fps each uncapped on this PC; earlier the same through
  `-SettingsCfg`, run 20261117-145300: about 80 fps), then `ini frame_cap=<n>` or, at run time,
  `action smooth 2 <fps>` (0 = off; docs/debug-actions.md "Smooth animation"). One game without
  `graphics low` runs uncapped (about 110 fps at 2560x1440 with two bodies, run 20261117-144357). The cap
  is loose: 30 gave 37-39 fps and 60 gave 73-78 fps; read the real rate from the `[smooth]` lines or
  `automation\out\status.txt`.
* **Windows (not working yet).** `local_window=left|right` (as the launcher writes): each game
  window gets a title bar and half of the monitor (`[overlay] local test: game window placed on the
  left half (0,309 1280x750, client 1264x711)`), and the subclass keeps that size when ds.exe asks
  for a full-monitor window (`kept the game window at 1280x750`). Still, at the end of runs 162418
  and 162635 both windows were back at `[0,0 2560x1440]` (`summary.json` `private_desktop_titles`
  now lists each window's rect): ds.exe resizes its window by a path the subclass does not see.
  The supported way today is the game's own Windowed option, set once per test window.
* **Graphics settings.** `graphics low` / `--settings-cfg <file>` redirect the game folder's
  `settings.cfg` to `automation\settings.cfg` (`appdir_settings=1`); `--game-settings <file>`
  merges `"Key" "Value"` lines into the scratch profiles' `game_settings.cfg`. Neither changed the
  boot resolution in runs 153540 .. 154718 (2560x1440 screenshots, see sandbox-and-local-test.md
  "Graphics"): kept as experiment tools.
* **Resources.** Every 2 s the supervisor samples each game: working set and private bytes
  (`GetProcessMemoryInfo`), CPU (process times, % of all logical CPUs), GPU dedicated/shared memory
  and 3D engine use (`\GPU Process Memory(*)`, `\GPU Engine(*)` performance counters, per
  `pid_<pid>` instance): `resources.csv` and `summary.json` `"resources"`.

Run 20261116-161556 (RTX 5080 16 GB, 64 GB RAM; both at 2560x1440 internally, 30 fps cap,
100% save, Episode 15 garage), whole run including boot and loading:

| | peak working set | avg working set | peak private | peak GPU dedicated | avg GPU dedicated | peak CPU | avg CPU |
|---|---|---|---|---|---|---|---|
| host | 4844 MB | 2934 MB | 10250 MB | 4880 MB | 2692 MB | 19.6 % | 6.0 % |
| guest | 4758 MB | 2756 MB | 10182 MB | 4875 MB | 2548 MB | 17.3 % | 5.8 % |
| both | 9582 MB (peak sum) | | 20407 MB | 9750 MB | | 36.9 % | |

System RAM never went below 17.2 GB free. Without the frame cap (run 154851) the GPU memory was
similar (host 4880 MB, guest 2861 MB while still loading) but the host used 25 % CPU on average
and the guest could not keep up. Two games need about 10 GB of VRAM and 10 GB of RAM on this
machine; a GPU with 8 GB will likely page.

| Run | Outcome |
|---|---|
| `20261116-154851-two-games` | both started, mutex renamed, linked; the guest never spawned the partner: it sat behind the offline-mode notice (found from `zz-final-guest`), and the host rendered uncapped |
| `20261116-155336-two-games` | frame cap on: both at ~30 fps; guest still behind the notice (screenshot shows it) |
| `20261116-155723-two-games` | **pass**: A dismisses the notice; both `partner=visible`, `link=connected`; screenshots of both |
| `20261116-160909-two-games` | window placement from the render thread: both games stopped presenting (deadlock); fixed by moving it to the window thread |
| `20261116-161556-two-games` | **pass** with side-by-side windows; the resource table above |
| `20261116-162226-two-games` | **pass** (same, final build) |
| `20261116-192744-two-games-real` | **fail**: the scenario waited on the host before the guest pressed A; the guest, behind its notice, sent no joint frames and the host's fallback to the hologram was permanent (both fixed) |
| `20261116-193151-two-games-real` | **pass**: guest sees a solid Sam, host sees a solid Fragile (hologram first while the guest was behind its notice, then back to the real body); 565 B per datagram |
| `20261116-193451-two-games-real-fallback` | **pass**: the host waits 3 s, falls back to the hologram and stays on it |
| `20261116-193649-two-games-real-cliff` | **pass**: a scratch copy of `two-games-real` with the guest as Cliff (not in the repo) |
| (runs above) | built before the Heartman leg latch (19:57) and ids 6-16 offered (20:05); see docs/real-partner-body.md section 6 item 1 for which build each run used |
| `20261116-202426-two-games-real` | **pass** on the final build (after the review fixes): guest sees a solid Sam walking, host sees a solid Fragile standing and running; 300/300 joint frames per 10 s, 565 B per datagram. `two-games-real-fallback` was not rerun on this build. |

## Test-save session (`--sandbox-session`)

`Start-TestRun.ps1 -Scenario sandbox-alive -SandboxSession -SeedSaves complete-100` deploys the
session.ini the launcher writes for **Play with test save** (`role=solo`, loopback, `debug_menu=0`,
`sandbox_saves=testenv-<pid>`, `sandbox_foreign_saves=1` with a seed; no `automation=1`) and seeds
`<mod>\sandbox\testenv-<pid>\saves` from the saves backup (the sandbox folder is recorded in
`restore.txt` before it exists and removed afterwards). The saves gate waits for
`<mod>\sandbox-active.txt` and `[sandbox] saves=redirected`. There is no automation channel (no
screenshots, input or mute): the scenario only waits for the log lines and the supervisor ends the
game ~15 s after start, before the title music. Run 20261116-161912: redirect confirmed after
2.64 s, foreign saves bypassed at the 3 sites, real tree unchanged, 0 uploads.

## A launcher window open during a run

The testenv's `session.ini` used to start with the launcher's own header (`# Generated by
Stranded Together`). An open launcher window (v0.2.3) tidies a session.ini with that header when
no game runs and no session is active (`StaleSessionPending`), so in runs 153757 and 154109 it
deleted the run's session.ini in the ~16 ms between deploy and the game start: the game started
**without** the redirect and ran 10 s at the title until the supervisor's evidence gate killed it
(the real tree was unchanged: the title only reads). Fixed twice: the testenv header is now
`# Stranded Together testenv: ...` (launchers leave it alone), and a ds.exe that carries
`ST_TESTENV_SUPERVISED` now ends in DllMain when it finds no automation (or sandbox) session.
