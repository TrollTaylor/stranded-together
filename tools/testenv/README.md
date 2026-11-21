# tools/testenv: hidden test runs

Death Stranding on a private, never-displayed Windows desktop, driven through the
mod's automation channel. Your saves are backed up, redirected to a scratch copy by
the mod (confirmed before any step) and checked by hash afterwards. Full description,
safety rules and current status: [docs/hidden-testing.md](../../docs/hidden-testing.md).

| File | Use |
|---|---|
| `Start-TestRun.ps1 -Probe` | D3D12 probe on a private desktop (no game) |
| `Start-TestRun.ps1 -Scenario smoke` | one hidden run of the real game (<= 6 min), results in `build-testenv\runs\<stamp>-smoke` |
| `Start-TestRun.ps1 -Scenario echo-holo-100 -SeedSaves complete-100` | the hidden game loads another save (scratch copy only); saves of another account load because the supervisor writes `automation_foreign_saves=1` (docs: "Using another save") |
| `Start-TestRun.ps1 -Scenario alive -NoMod` | start-up bisection switches: `-NoMod` (the DLL only redirects the saves), `-NoUiLimits`, `-SteamEnv`, `-NoIsolation`; `-NoGameStdout` |
| `Start-TestRun.ps1 -SeedSaves complete-100 -Steps 'goto lake-knot ; screenshot a'` | inline steps, no scenario file: straight into the world (`start`), the steps, quit (`-Role host`, `-LauncherIni`, `-Start 'story 12'`, `-NoStart`, `-Name`) |
| `Start-TestSession.ps1` / `Send-TestStep.ps1` / `Stop-TestSession.ps1` | a held session: the game stays and takes steps until Stop (the 360 s cap, restore and checks unchanged; one foreground command for agents) |
| `places.txt` | the named places of a scenario's `goto <name>` (area 200 of complete-100) |
| `scenarios\quickstart-solo.txt`, `two-games-quickstart.txt` | `start`: into the world with no menu step (one game; the launcher's Local test) |
| `scenarios\goto-places.txt`, `spawn-showcase.txt` | `goto` to 7 places with screenshots; vehicles, weather, time, MULEs, cap / glasses, boots' durability |
| `Restore-TestRun.ps1 -RunDir <run>` | put the game folder back and check/restore the saves after a crash (safe to repeat) |
| `scenarios\smoke.txt` | title -> Continue (virtual pad) -> Sam in the world -> debug menu -> quit |
| `scenarios\walk.txt` | Continue, wait out the save's scripted BT hold, walk 6 s (must move >= 5 m), turn the camera |
| `scenarios\echo-holo.txt` | role echo: walk, look back, screenshots of the hologram partner trailing Sam |
| `scenarios\echo-holo-100.txt` | with `-SeedSaves complete-100`: Continue into the 100% save (another account's; the mod skips ds.exe's save-owner check in this run only), walk, look back at the hologram, debug menu |
| `scenarios\boot.txt` | no input, screenshots of the boot every 10-15 s (title flow discovery) |
| `scenarios\alive.txt` | supervisor-only: is the game still alive after 45 s |
| `scenarios\fake-smoke.txt` | the ctest `st_testenv_e2e` with the fake game |

Native code: `native/testenv/` (supervisor, probe, fake game, tests),
`native/mod/automation.*` (command channel, save redirect, exit tracer),
`native/mod/overlay/*` (screenshots, virtual pad, isolation). Targets and ctests:
`cmake/testenv.cmake`.
