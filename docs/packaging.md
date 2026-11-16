# Building a release zip

Players install Stranded Together the way they install Diamond Dogs Together:
extract one zip into the game folder and run the launcher from there. The
same zip, attached to a GitHub release, is what the launcher's updater
downloads and installs; `tools\release.ps1` makes the release
([releasing.md](releasing.md)).

## The zip

`tools\package.ps1` makes `dist\StrandedTogether-<version>.zip`. Its root is
the DEATH STRANDING DIRECTOR'S CUT folder (the one with `ds.exe`):

```
StrandedTogether.exe                        the launcher (Play, In game, Tools, Debug, About)
winhttp.dll                                 the mod (winhttp proxy; idle without session.ini)
stranded_together\package.json             what the launcher checks (below)
stranded_together\README.txt               install, play, debug, uninstall (tools/launcher/package-readme.txt)
stranded_together\THIRD_PARTY_NOTICES.txt  MinHook (BSD 2-clause) and Dear ImGui (MIT) licences
```

The same files are staged in `dist\StrandedTogether-<version>\`. `dist\` gets
its own `.gitignore` (`*`) the first time, so nothing there is committed.

## Making one

```powershell
.\tools\package.ps1                                 # configure build-package\ (first time), build, test, stage, zip
.\tools\package.ps1 -BuildDir build -NoBuild        # package the binaries an existing build folder already has
```

| Option | Default | |
|---|---|---|
| `-BuildDir` | `build-package` | CMake build folder; configured with `-G "Visual Studio 17 2022" -A x64` if it has no cache. |
| `-Config` | `Release` | |
| `-NoBuild` | off | Package `<BuildDir>\<Config>\winhttp.dll` and `StrandedTogether.exe` as they are (a warning names a binary that is older than its sources). |
| `-SkipTests` | off | Otherwise `st_launcher_core_tests.exe` and `st_launcher_update_tests.exe` must pass. |
| `-OutputRoot` | `dist` | |

The script refuses a `winhttp.dll` that is not the Stranded Together proxy,
takes the version (`X.Y.Z`) and build id (`ST_BUILD_ID`, `X.Y.Z+<git short
hash>` once the repository has commits) from the build folder's
`generated\launcher\st_launcher_version.h`, and after zipping reopens the zip
and compares every entry with `package.json`, then runs the launcher's own
check of the zip (`st_launcher_update_tests --verify-zip <zip> <version>`,
the `VerifyUpdateZip` that Update now runs before installing) when that
program is in the build folder. It works in PowerShell 7 and Windows
PowerShell 5.1.

What the updater requires of the zip (so keep it when changing this script):
only `StrandedTogether.exe`, `winhttp.dll` and files under
`stranded_together/`; `stranded_together/package.json` with `product`, `v: 1`
and the release's `version`; every other entry listed once in `files[]` with
its SHA-256 (and `size`); nothing the launcher or the mod writes at run time
(`session.ini`, `launcher-settings.json`, logs). The asset name on GitHub must
be exactly `StrandedTogether-<version>.zip`.

Both binaries always come from one build folder: the launcher compares
`build_id` with its own and reports a mismatch otherwise. Two players need the
same zip (the link handshake compares build ids too).

## package.json

```json
{
  "product": "stranded-together-package",
  "v": 1,
  "version": "0.1.0",
  "build_id": "0.1.0+abc1234",
  "built_utc": "2026-11-16T14:28:10Z",
  "config": "Release",
  "source_commit": "<40 hex>" | null,
  "source_dirty": false | null,
  "game": { "app_id": 1850570, "ds_exe_sha256": "...", "steam_api64_sha256": "..." },
  "files": [
    { "path": "winhttp.dll", "sha256": "<64 hex>", "size": 525824 },
    { "path": "StrandedTogether.exe", "sha256": "<64 hex>", "size": 1532928 },
    { "path": "stranded_together/README.txt", "sha256": "...", "size": 2767 },
    { "path": "stranded_together/THIRD_PARTY_NOTICES.txt", "sha256": "...", "size": 6150 }
  ]
}
```

`source_commit` is `git rev-parse HEAD` (null without commits), `source_dirty`
whether `git status --porcelain` listed anything. `game` repeats the tested
`ds.exe` / `steam_api64.dll` hashes from `native/launcher/core/core_install.hpp`
(information only; the launcher uses its own constants).

The launcher (`core::ReadPackage`, `core::CheckMod`) needs `product`, `v: 1`,
`version` and exactly one `winhttp.dll` row; then:

| In the game folder | Launcher |
|---|---|
| `winhttp.dll` hash = the row, the exe = its row, `build_id` = the launcher's | "Mod 0.1.0" |
| ours, but a hash or the build id differs, or package.json is damaged | warning "The mod files don't match"; play still allowed |
| ours, no package.json | "Mod: development build" |
| another program's winhttp.dll | co-op blocked; the file is never touched |
| only `winhttp.dll.disabled` (ours) | "The mod is switched off" + Enable mod |
| nothing | "Install the co-op mod first" |

## Installing it for a test

```powershell
.\tools\deploy.ps1 -Package                                   # the newest dist\StrandedTogether-*\ folder
.\tools\deploy.ps1 -Package -PackagePath dist\StrandedTogether-0.1.0.zip
.\tools\deploy.ps1 -Uninstall                                 # our DLL(s), the exe, session.ini, package.json; logs stay
```

`-Package` checks every file against `package.json`, refuses while `ds.exe`
runs from the folder and never overwrites another program's `winhttp.dll`.
A plain developer deploy (`deploy.ps1` without `-Package`) copies
`build\Release\winhttp.dll` and `StrandedTogether.exe` and removes an old
`package.json`, so the launcher shows "development build".

## Checked so far

A zip made from `build-launcher` was extracted into a scratch folder with a
stand-in `ds.exe`: `StrandedTogether.exe --selftest` reported "ours, matches
package.json"; `--write-session solo` wrote a `session.ini` that the mod's own
`LoadConfig` (built into a harness) read with every key known and then renamed
to `session.last.ini`. `deploy.ps1 -Package` (from the zip, in Windows
PowerShell 5.1), its refusal of a foreign `winhttp.dll`, and `-Uninstall` ran
against scratch folders. Not yet tried on a real game install.
