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
| Real game (one smoke run, 11:32) | **ds.exe exited by itself with code 0 after ~1.4 s**, before any window or frame. Nothing reached the user's desktop, no Steam relaunch, no `error.txt`, saves unchanged, game folder restored identical. Cause not yet known; see "When the game exits at once". |

## How it works

```
Start-TestRun.ps1 ─> st_testenv_supervisor.exe (GUI subsystem: never a console window)
   │  preflight: no ds.exe running, Steam running, no steam_appid.txt, scenario parses
   │  saves: hashed + copied to <run>\saves-before (read only), compared afterwards
   │  game folder: winhttp.dll, error.txt, every file of stranded_together\ backed up to
   │               <run>\backup, manifest <run>\restore.txt written BEFORE any change
   │  deploy: build-testenv winhttp.dll + stranded_together\session.ini
   │          (role=solo, loopback UDP, debug_menu=1, automation=1, one_shot=1)
   │  CreateDesktopW("StTestEnv_<pid>_<tick>")   -- never SwitchDesktop
   │  job: kill-on-close, die-on-unhandled-exception (no WER dialog), UI limits
   │       (no desktop switch/create, no display-mode or SystemParametersInfo changes,
   │        no clipboard, no ExitWindows)
   │  CreateProcess(ds.exe, STARTUPINFO.lpDesktop = WinSta0\StTestEnv_..., suspended,
   │                SteamAppId=1850570, SteamGameId=1850570) -> job -> resume
   │  every second: no window of a job process on the user's desktop, the input desktop
   │                unchanged, no ds.exe outside the job -> otherwise kill the job, stop
   │  scenario steps -> <mod>\automation\in\NNNN.cmd, acks <mod>\automation\out\NNNN.done
   │  end: quit command, 30 s grace, TerminateJobObject (only our processes)
   └─ collect <run>\game\ (mod log, session.last.ini, link-status.json, new error.txt),
      <run>\automation\ (screenshots, results.txt, status.txt), restore, summary.json
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

* `smoke.txt`: wait for the channel, screenshot, press A every 4 s until
  `[sync] local player entity 0 -> <entity>`, `waitplayer`, screenshot in the world, open
  the debug menu, screenshot, `action log_status`, close, screenshot, quit.
* `alive.txt`: supervisor steps only (works with `-NoMod`): watch the game for 45 s.
* `fake-smoke.txt`: the ctest's fake game.

## Outputs

`build-testenv\runs\<yyyyMMdd-HHmmss>-<scenario>\`: `summary.json` (result, end reason,
game exit code / seconds alive / CPU seconds, violation, windows seen on the private
desktop, restore notes, save changes, every step with its detail), `supervisor.log`,
`scenario.txt`, `game\stranded_together.log`, `automation\*.png`, `automation\results.txt`,
`automation\status.txt`, `saves-before\` + `saves-before.txt` / `saves-after.txt`,
`backup\` + `restore.txt` + `restored.txt`, `game-new\` (files that appeared in
stranded_together, e.g. the fake game's log).

Exit codes: 0 pass, 1 scenario failed / game exited / restore incomplete, 2 refused
(preflight), 3 safety violation.

## Safety rules (enforced, not assumed)

* Refuses when any `ds.exe` runs, when Steam is not running, when `steam_appid.txt`
  exists, and while another supervisor run is active (`Local\StrandedTogetherTestEnv`).
* Never `SwitchDesktop`, never system input (`SendInput`), never focus/foreground calls
  on the user's desktop. The children's job forbids desktop switching/creation.
* Kills only its own job. A window of the job on the user's desktop, a changed input
  desktop or a `ds.exe` outside the job ends the run at once (exit 3).
* Saves are only read by the supervisor; the run reports what the game changed
  (`save_changes`, with the pre-run copy in `saves-before\`). **The game itself may write
  them**: a scenario that presses CONTINUE (smoke) loads the user's latest save, DS
  autosaves while in the world, and a `quit` that has to fall back to TerminateProcess
  (mod, after 20 s) or a job kill can interrupt a save. Steam runs AutoCloud on the hidden
  game's exit too (cloud_log of the 11:32 run: "Running AutoCloud on exit ... Persisting
  file ... autosave0\checkpoint.dat"), so a changed save is uploaded. Nothing restores saves
  automatically; decide before running in-world scenarios (use a scratch Steam profile or
  restore `saves-before\` by hand).
* The game folder is restored from `restore.txt` (also after a crash: `Restore-TestRun.ps1`,
  or Start-TestRun does it, up to 3 times, when `restored.txt` is missing or says INCOMPLETE). Restore never touches paths
  outside the game folder.
* 6 minutes of game lifetime per run at most (final screenshot, quit and kill included). Steam shows you as playing Death Stranding during a run.

## Tests (no game)

* `st_testenv_tests`: command parser, scenario language (and every scenario file),
  session.ini text, backup/restore of a scratch game folder (incl. a tampered manifest),
  save-manifest diff.
* `st_testenv_probe_private`: the probe through the supervisor (private desktop + job).
* `st_testenv_e2e`: `st_testenv_fakegame.exe` copied as `ds.exe` (imports winhttp like
  ds.exe, polls XInput through `xinput1_4` like ds.exe, D3D12 flip-discard), the real
  `winhttp.dll` and supervisor, `fake-smoke.txt`; checks decoded screenshots, the
  virtual pad reaching the game's own `XInputGetState`, `GetForegroundWindow`, quit,
  restore. Skipped (77) while a ds.exe runs.
* `st_overlay_harness` section 10: the automation parts of the overlay.

## When the game exits at once

The one real run (`build-testenv\runs\20261116-113201-smoke`):

```
[ 0.000] mod loaded, [auto] automation=1, [config] role=solo ... debug_menu=1
[ 0.156] [overlay] installed ... XInputGetState(xinput1_4); AUTOMATION: ...
[ 0.250] [auto] audio muted ..., [auto] channel ready
[ 0.250] [game] all 8 functions at their known RVAs
[ 0.328] [mod] frame pump hooked; [mod] ready (solo)
-- no "[sync] first tick", no "[overlay] input attached" (no Present), no window --
supervisor: started pid 22760 at 0.203 s; gone at 1.609 s; exit code 0
Steam gameprocess_log: 11:32:01 "adding PID 22760 as a tracked process", 11:32:05 "exit code 0"
```

Exit code 0 this early is most likely ds.exe's own startup returning 0 (not proven:
no debugger was attached): after `RestartAppIfNecessary` (false here: no relaunch) and
`SteamAPI_Init` (connected: Steam tracked the pid), `GameMainProg` (export, 0x1418E9B20)
calls the app init at 0x14262C0C0 and, when it returns false, skips the run and returns
the global exit code (0) without a message box. Not the cause: the single-instance mutex `DeathStrandingDCAppMutex`
(0x1417EAE0F) exits with 1, the CPU checks show a message box, DS writes no log of its own.
Differences from a normal Steam launch: the private desktop, the job's UI limits, the
environment (only `SteamAppId`/`SteamGameId`), the parent process, the mod's automation
hooks (no fullscreen refusal was logged, so `SetFullscreenState` was not reached).

Next runs to bisect (each invisible, about 50 s, game folder restored):

```powershell
.\tools\testenv\Start-TestRun.ps1 -Scenario alive -NoMod -NoBuild                # vanilla ds.exe, same desktop/job
.\tools\testenv\Start-TestRun.ps1 -Scenario alive -NoMod -NoUiLimits -NoBuild    # + no job UI limits
.\tools\testenv\Start-TestRun.ps1 -Scenario alive -NoMod -SteamEnv -NoBuild      # + Steam's launch variables
.\tools\testenv\Start-TestRun.ps1 -Scenario alive -NoIsolation -NoBuild          # mod without the isolation hooks
```

`summary.json` `game_alive_s` / `game_exit_code` / `private_desktop_windows` tell which
condition keeps the game alive.

Further evidence (review, from Steam's own logs of the 11:32 run, read only):

* `logs\gameoverlay_renderer.txt`: the Steam overlay attached to pid 22760, hooked audio
  and XInput, then logged `hookCreateDXGIFactory2 called` 7 times and `detaching`, with
  **no** "Hooking vtable for device / swap chain". A normal launch (the `.previous` file)
  shows the factories, then `Creating D3D12 renderer` and the swapchain hooks. So ds.exe
  quit during adapter/output enumeration, before `D3D12CreateDevice`.
* `logs\stats_log.txt`: `[AppID 1850570] Loading stats from disk...success` and a stats
  request at 11:32:01: `SteamAPI_Init` succeeded and the game ran past its `main`
  (0x1418EA070: `RestartAppIfNecessary` true -> return 0; `SteamAPI_Init` false ->
  `SteamAPI_Shutdown`, return 0; else the game proper at 0x1418EA160). Both early exit-0
  paths of `main` are therefore ruled out.
* The probe (now also run inside the UI-limited job on the private desktop) enumerates
  the same adapters, 3 attached outputs with 307 modes, the GDI current mode
  (2560x1440 @ 144) and `MonitorFromWindow`: plain DXGI/GDI display queries work there.
  `probe.json` keys `adapters`, `gdi_current_mode`, `monitor_from_window`, `screen_size`.
