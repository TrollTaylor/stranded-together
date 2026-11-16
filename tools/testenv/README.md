# tools/testenv: hidden test runs

Death Stranding on a private, never-displayed Windows desktop, driven through the
mod's automation channel. Full description, safety rules and current status:
[docs/hidden-testing.md](../../docs/hidden-testing.md).

| File | Use |
|---|---|
| `Start-TestRun.ps1 -Probe` | D3D12 probe on a private desktop (no game) |
| `Start-TestRun.ps1 -Scenario smoke` | one hidden run of the real game (<= 6 min), results in `build-testenv\runs\<stamp>-smoke` |
| `Start-TestRun.ps1 -Scenario alive -NoMod` | start-up bisection switches: `-NoMod`, `-NoUiLimits`, `-SteamEnv`, `-NoIsolation` |
| `Restore-TestRun.ps1 -RunDir <run>` | put the game folder back after a crash (safe to repeat) |
| `scenarios\smoke.txt` | title -> Continue (virtual pad) -> Sam in the world -> debug menu -> quit |
| `scenarios\alive.txt` | supervisor-only: is the game still alive after 45 s |
| `scenarios\fake-smoke.txt` | the ctest `st_testenv_e2e` with the fake game |

Native code: `native/testenv/` (supervisor, probe, fake game, tests),
`native/mod/automation.*` (command channel), `native/mod/overlay/*` (screenshots,
virtual pad, isolation). Targets and ctests: `cmake/testenv.cmake`.
