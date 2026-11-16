# Releasing

A release is one zip, `StrandedTogether-<version>.zip`, attached to a GitHub
release `v<version>` of [TrollTaylor/stranded-together](https://github.com/TrollTaylor/stranded-together/releases).
Players install it by extracting it into the game folder
([packaging.md](packaging.md)); launchers that are already installed find it
with their update check and install it with **Update now**
([launcher.md](launcher.md#updates)). `tools\release.ps1` makes the whole
release in one run.

## Before you start

* Everything you want in the release is committed on `main`, and nothing else
  is in the working tree (untracked files count: commit, delete or ignore them).
* `main` is pushed or ahead of `origin/main`, never behind it.
* `gh auth status` says you are signed in with push access to the repository.
* Visual Studio 2022 (or its Build Tools), CMake 3.20+ with `ctest`, git and
  `gh` are on `PATH`.
* Try it first: `.\tools\release.ps1 -DryRun`.

## Running it

```powershell
.\tools\release.ps1 -DryRun                        # everything except commit, tag, push and the GitHub release
.\tools\release.ps1                                # patch release: 0.1.0 -> 0.1.1
.\tools\release.ps1 -Minor                         # 0.1.3 -> 0.2.0
.\tools\release.ps1 -Major                         # 0.4.2 -> 1.0.0
.\tools\release.ps1 -Version 0.3.0                 # an explicit version (not lower than the current one)
.\tools\release.ps1 -Notes "Partner animations; cargo shows on the partner's back."
.\tools\release.ps1 -Prerelease                    # published as a GitHub prerelease: launchers do not offer it
```

| Option | Default | |
|---|---|---|
| `-Minor` / `-Major` / `-Version x.y.z` | patch + 1 | The new version. `-Version` equal to the current one releases it without a bump (the tag goes on `HEAD`). |
| `-Notes "<text>"` | the commit subjects since the last `v*` tag | The "Changes" part of the release notes. |
| `-DryRun` | off | Runs every step except commit, tag, push and release, then puts `CMakeLists.txt` back. |
| `-Prerelease` | off | `gh release create --prerelease`. |
| `-Repo` | `TrollTaylor/stranded-together` | GitHub repository of the release. |
| `-Remote` / `-Branch` | `origin` / `main` | Where the commit and tag go. |
| `-BuildDir` | `build-release` | Deleted and configured afresh every run (only when it is a CMake build folder inside the repository). |
| `-OutputRoot` | `dist` | Where the zip, its staged folder and the notes go (git-ignored). |
| `-Generator` | `Visual Studio 17 2022` | CMake generator (always `-A x64`). |

The script runs in PowerShell 7 and Windows PowerShell 5.1. Its exit code is
0 only when every step passed.

## What it does

| Step | |
|---|---|
| preflight | A git repository with commits, on `-Branch`, `git status --porcelain --untracked-files=all` empty; `git fetch --tags <Remote>` and `HEAD` not behind `<Remote>/<Branch>`; `gh auth status`; `cmake`/`ctest` found. |
| version | Reads `project(stranded_together VERSION x.y.z ...)` from `CMakeLists.txt` and computes the new version. The tag `v<version>` must exist neither locally, on the remote, nor as a GitHub release. |
| bump | Rewrites that one line of `CMakeLists.txt` (checked with `git diff --numstat`: one line changed). This is the only edit a release makes to `CMakeLists.txt`; never bump the version by hand. |
| commit | `git commit -m "Release vX.Y.Z" -- CMakeLists.txt`. |
| build | Deletes `-BuildDir`, configures it (`-G <Generator> -A x64`) and builds every target in Release. |
| test | `ctest -C Release --output-on-failure`: every test (mod, overlay harness, net, fakepeer, launcher, updater, import check, screenshots) must pass. |
| package | `tools\package.ps1 -BuildDir <it> -NoBuild -SkipTests -OutputRoot <it>`: the zip, its check against `package.json`, and the launcher's own check of it (`st_launcher_update_tests --verify-zip`, the same `VerifyUpdateZip` that Update now runs). Then `package.json` in the zip must say the new version, `source_commit` = the release commit and `source_dirty` = false. |
| notes | `<OutputRoot>\release-notes-X.Y.Z.md`: the changes (`-Notes`, or `git log --no-merges` subjects since the last `v*` tag, without earlier "Release v..." commits), how to update or install, and the zip's SHA-256. |
| tag | `git tag -a vX.Y.Z -m "Stranded Together X.Y.Z"` on the release commit. |
| push | `git push --atomic <Remote> <Branch> vX.Y.Z` (both or neither). |
| release | `gh release create vX.Y.Z <zip> --verify-tag --title "Stranded Together X.Y.Z" --notes-file <notes>`, then `gh release view` checks: exactly one `StrandedTogether-X.Y.Z.zip` of the local size, no other `StrandedTogether-*.zip`, not a draft. |

The commit comes **before** the build (unlike the order you might expect)
so that the binaries' build id (`X.Y.Z+<short hash>`, compared in the link
handshake and against `package.json`) and `package.json`'s `source_commit`
name the release commit itself, with `source_dirty: false`.

### When something fails

* **Before the push** (preflight, build, a test, packaging, the tag): the
  script deletes its tag, undoes the release commit (`git reset --soft
  HEAD~1`, then `CMakeLists.txt` restored byte for byte), and checks that
  `HEAD` and the working tree are as they were. Nothing left the PC. Fix the
  problem and run it again.
* **After the push, before the GitHub release**: the commit and the tag are
  public; the script prints the exact `gh release create ...` command to
  finish by hand.
* **After the release**: if the check of the published release fails, look
  at it on GitHub (a missing or extra asset). Delete a wrong asset with
  `gh release delete-asset`, upload the right zip with `gh release upload`.

### `-DryRun`

Runs preflight, version, bump, build, test, package and notes for real, prints
the commit, tag, push and `gh release create` commands it would run, then
puts `CMakeLists.txt` back and fails if `HEAD` moved or the working tree
changed. The zip and notes stay in `dist\` to look at. Its `package.json` has
`source_dirty: true` (the bump was not committed) and the build id names the
current `HEAD`: never upload a dry-run zip.

## What the launchers do with it

Every launcher reads `https://api.github.com/repos/TrollTaylor/stranded-together/releases`
once when it opens (unless About > "Check for updates when the launcher
starts" is off) and on About > **Check for updates**. It offers the highest
version (SemVer) that is newer than its own, not a draft, not a prerelease,
and carries exactly one asset named `StrandedTogether-<version>.zip` whose
download URL is this repository's `releases/download/<tag>/<name>`. Anything
else attached (a source zip, a debug build) is ignored, but never attach a
second `StrandedTogether-*.zip`. Update now checks the download against the
asset's size and GitHub's SHA-256 digest, then the zip itself, before it
replaces anything ([launcher.md](launcher.md#updates)).

So, after a release:

* Players on an older version see "Update available: version X.Y.Z" on Play
  the next time they open the launcher.
* Both players need the same version: the link handshake compares build ids.
* A mistake cannot be "un-released" for launchers that already updated.
  Publish a fixed patch release instead (`.\tools\release.ps1`); a release
  that is deleted or turned into a draft simply stops being offered.

## Testing the updater without GitHub

```powershell
# A game folder with ds.exe (any file), the current StrandedTogether.exe and winhttp.dll.
# A feed folder with releases.json (GitHub's JSON shape) and StrandedTogether-<newer>.zip:
python -I -m http.server 18765 --bind 127.0.0.1 --directory <feed folder>
<game>\StrandedTogether.exe --update-feed http://127.0.0.1:18765/releases.json             # Update now by hand
<game>\StrandedTogether.exe --update-feed http://127.0.0.1:18765/releases.json --update-now # installs at once
```

`--update-feed` takes only `http(s)://127.0.0.1` or `localhost` URLs, and
then every download URL in that feed must be a loopback URL too. The
`browser_download_url` in `releases.json` points at the zip on the same
server; `size` and `digest` (`"sha256:<hex>"`) are optional but checked when
present. `stranded_together\launcher.log` in both launchers' folder records
each step (`update=checked`, `update=start`, `update=installed`,
`update=handed_over`, then from the new process `update=finished`).
`st_launcher_update_tests --live-feed [version]` reads the real GitHub feed
once and prints what a launcher of that version would offer.
