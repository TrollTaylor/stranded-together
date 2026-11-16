# tools/testenv: hidden test runs

Death Stranding on a private, never-displayed Windows desktop, driven through the
mod's automation channel. Your saves are backed up, redirected to a scratch copy by
the mod (confirmed before any step) and checked by hash afterwards. Full description,
safety rules and current status: [docs/hidden-testing.md](../../docs/hidden-testing.md).

| File | Use |
|---|---|
| `Start-TestRun.ps1 -Probe` | D3D12 probe on a private desktop (no game) |
| `Start-TestRun.ps1 -Scenario smoke` | one hidden run of the real game (<= 6 min), results in `build-testenv\runs\<stamp>-smoke` |
| `Start-TestRun.ps1 -Scenario alive -NoMod` | start-up bisection switches: `-NoMod` (the DLL only redirects the saves), `-NoUiLimits`, `-SteamEnv`, `-NoIsolation`; `-NoGameStdout` |
| `Restore-TestRun.ps1 -RunDir <run>` | put the game folder back and check/restore the saves after a crash (safe to repeat) |
| `scenarios\smoke.txt` | title -> Continue (virtual pad) -> Sam in the world -> debug menu -> quit |
| `scenarios\walk.txt` | Continue, wait out the save's scripted BT hold, walk 6 s (must move >= 5 m), turn the camera |
| `scenarios\echo-holo.txt` | role echo: walk, look back, screenshots of the hologram partner trailing Sam |
| `scenarios\boot.txt` | no input, screenshots of the boot every 10-15 s (title flow discovery) |
| `scenarios\alive.txt` | supervisor-only: is the game still alive after 45 s |
| `scenarios\fake-smoke.txt` | the ctest `st_testenv_e2e` with the fake game |

Native code: `native/testenv/` (supervisor, probe, fake game, tests),
`native/mod/automation.*` (command channel, save redirect, exit tracer),
`native/mod/overlay/*` (screenshots, virtual pad, isolation). Targets and ctests:
`cmake/testenv.cmake`.
