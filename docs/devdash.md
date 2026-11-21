# devdash: live view of tests and research

devdash is a small local web page that shows, live, what is being tested and researched in this repo:

- the hidden test runs (docs/hidden-testing.md): which scenario is running, step k of N, a progress bar,
  the current step, the supervisor log, the games' logs and the screenshots as they are taken;
- batches of runs (`tools\testenv\Run-Batch.ps1`): n of m done, pass/fail per scenario;
- any task an agent or script posts (the status files below);
- Claude Code workflow runs ("research"): phases, agents running and done, labels, durations, the
  latest results;
- the history of every run with its checks (exit codes, real saves unchanged, game folder restored,
  violations, Steam Cloud uploads) and a screenshot gallery.

It is a **separate program outside the game**. It never starts, stops or touches ds.exe or the
launcher, and it only reads. Python 3 standard library only; no download, no CDN.

## Open it

```bat
copy tools\devdash\devdash.bat.example devdash.bat      :: once; devdash.bat is gitignored
devdash.bat                                             :: double-click it
```

`devdash.bat` starts the server without a console window (`pyw -3`, else `pythonw`, else `python`) and
opens the page as an app window (Edge or Chrome `--app=`, else the default browser). Running it again
while the server runs just opens another window. Close the window when done: the server exits by
itself 15 minutes after the page stops asking (`--idle-exit`).

By hand:

```powershell
python tools\devdash\devdash.py --open            # http://127.0.0.1:8765/ (the next free port up to 8774)
python tools\devdash\devdash.py --port 8790 --idle-exit 0 --root D:\scratch\bld\runs
```

| Option | Meaning |
|---|---|
| `--open` / `--tab` | open an app window / a normal browser tab |
| `--port N` | first port to try (default 8765; 8080 is Ghidra's) |
| `--idle-exit S` | exit after S seconds without a request (default 900; 0 = never) |
| `--root DIR` | another runs folder to show (a `-BuildDir` outside the repo) |
| `--claude-dir DIR` | Claude Code projects folder (default `%USERPROFILE%\.claude\projects`) |
| `--state-dir DIR` | devdash's own folder (default `<repo>\build-devdash`, or `ST_DEVDASH_DIR`) |

The URL is also written to `build-devdash\server.json`.

## What the page shows

- **Top bar**: tests running, queued (batch items not started yet), passed and failed today,
  research agents running, workflows running, and the connection state.
- **Live tests**: one card per hidden run in progress: scenario and its description (the first `#`
  line), role, phase (preparing, booting, steps, finishing, collecting, restoring, checking), step k of
  N with a bar and percentage, elapsed time against the steps limit, the step being run now (the next
  scenario line), the last step's result, each game's fps / uptime / build from `status.txt`, the
  newest screenshots and the last lines of `supervisor.log`. Click it for the full run.
- **Batches**: progress n of m, a segment per scenario (green pass, red fail, blue running), each
  scenario's run with its live step count or its duration and failed checks. Click a row for its run.
- **Posted tasks**: everything posted with `Set-DevStatus.ps1` or `devdash.py post`.
- **Builds and checks**: ctest's last result per build folder (from `Testing\Temporary\LastTest.log`;
  while a ctest runs, from the `LastTest.log.tmp*` it writes and renames at the end: k of N), today's run counts, and whether a ds.exe is running.
- **Research and agent work**: each running Claude workflow with its phases (done / active / pending,
  agents done of started), the labels running now, the latest results, and an approximate percentage
  (marked with ≈: a workflow decides how many agents a phase gets as it goes). Finished workflows are
  listed below it. Click one for every agent: phase, state, start, duration, the tool it is using now,
  its result, and its recent activity.
- **History**: every run, newest first, filter by passed / failed / refused or by scenario name; the
  checks column shows what went wrong (`game exit 1`, `REAL SAVES CHANGED`, `game folder not restored`,
  `safety violation`, `cloud upload`, failed steps). Click a run for its steps, logs (supervisor, both
  mod logs, results, stdout, scenario; filter box; follows the end while live), screenshots (gallery
  and a large viewer: arrow keys, Esc) and details (resources, restore notes, exit trace).

A run that needs a restore (its supervisor died) shows a red banner with the
`Restore-TestRun.ps1` command to run yourself; devdash never runs it.

## How it reads (safety)

- Binds 127.0.0.1 only; answers only `Host: 127.0.0.1:<port>` or `localhost:<port>` (no DNS
  rebinding); GET and HEAD only; serves only its own static files and the API; every run, file,
  workflow and task name is checked against a strict pattern and must be listed in a known folder, so
  no path in a URL reaches the disk (no `..`, no free paths).
- Read only. It writes only under `build-devdash\` (gitignored): `roots.json` (runs folders seen),
  `cache\` (screenshots copied from a live run), `server.json`, `devdash.log` (errors).
- Opens, reads and closes every file at once and never asks for write access: the supervisor, mod
  and core replace files with `.tmp` + rename, which fails while anyone holds the file open. Live
  screenshots are read once each into `build-devdash\cache` and served from there; `status.txt` is read
  at most every 2 s; big logs and agent transcripts are read from the end (256 KB).
- The game folder's files (mod logs, `automation\out\*.png`, `status.txt`) are read **only while a
  hidden run is live**, from the folder that run's supervisor names on its plan line. Outside a run
  the mod log there is your own and devdash does not show it.
- Processes come from a Toolhelp snapshot (no handle). The only handle devdash opens is a
  query-limited one on `st_testenv_supervisor.exe`, to read its `--run-dir`. It never opens ds.exe or
  `StrandedTogether.exe`, and never the supervisor's mutex `Local\StrandedTogetherTestEnv` (holding it
  would make the next hidden run refuse).
- What the page gets is redacted: your user folder becomes `%USERPROFILE%`, the save account folder
  number `<account>`, Epic ids `epic_<id>`, Steam ids `<steamid>`. The Claude transcripts' main file is never read.

Run state rules: a run is **live** when its supervisor's pid (from the log's first line) is in the
process list as `st_testenv_supervisor.exe`; the final log line (`run PASSED|FAILED|STOPPED`,
`REFUSED`) is the verdict (summary.json is written before the last restore check); no final line and
no supervisor is **interrupted**; a folder without `supervisor.log` is **starting** for 2 minutes, then
**aborted**. A run needs a restore under the same rule as Start-TestRun.ps1 (`restore.txt` without an
`ok` `restored.txt`, or the saves pair).

## Batches: `tools\testenv\Run-Batch.ps1`

```powershell
pwsh -File tools\testenv\Run-Batch.ps1 -Scenarios smoke,two-games -SeedSaves complete-100
pwsh -File tools\testenv\Run-Batch.ps1 -Scenarios two-games-ride-save-host,two-games-fragile-attach `
     -SeedSaves complete-100 -GuestSeed profile-only -Id round4-prove -Title 'Round 4 prove' -StopOnFail
```

Runs the scenarios one after another in the foreground, each as one ordinary `Start-TestRun.ps1` run
with all of its checks. Before each run it waits (up to `-WaitForGameSec`, default 900) while another
run is active (a ds.exe, an `st_testenv_supervisor`, or another `Start-TestRun.ps1`, which covers its
build, backup, collect and restore), since hidden runs go one at a time; a run the supervisor refused
because another had just begun is waited for and started again. A run counts as restored by its
`restored.txt` / `saves-restored.txt` (Start-TestRun's re-restore fixes those, not summary.json). It builds once (the first run) and passes
`-NoBuild` after that (`-BuildEach` rebuilds every time). It stops at once when a run reports changed
real saves, a safety violation or a missing restore, and at the first failure with `-StopOnFail`.
Arguments it does not know go to every Start-TestRun.ps1 (`... -- -LauncherIni`). Exit code 0 when
every run passed. The batch is the status task `<Id>` (default `batch-<time>`).

A batch longer than one tool call may last (an agent's foreground call ends after about 10 minutes) runs in parts:
`-From k -Count n` runs items k..k+n-1 of the same `-Scenarios` list under the same `-Id` and leaves the batch open
(n of m keeps counting; between parts the batch has no owner process: it shows as stale after 30 minutes without a part,
and its queued items no longer count as queued); the part that runs the
last item, or stops, closes it with every item's last result counted:

```powershell
$l = 'quickstart-solo,goto-places,two-games-headgear'
pwsh -File tools\testenv\Run-Batch.ps1 -Scenarios $l -Id round4-prove -SeedSaves complete-100 -From 1 -Count 1
pwsh -File tools\testenv\Run-Batch.ps1 -Scenarios $l -Id round4-prove -SeedSaves complete-100 -From 2 -Count 1
pwsh -File tools\testenv\Run-Batch.ps1 -Scenarios $l -Id round4-prove -SeedSaves complete-100 -From 3 -Count 1
```

## Posting progress (for scripts and agents)

Any long task can show a progress bar in devdash by appending records to its status file.

```powershell
$s = 'tools\devdash\Set-DevStatus.ps1'
& $s -Id hg-research -Title 'Hats: compare v0.19 and v0.21' -Kind research -Total 4
& $s -Id hg-research -Done 1 -Note 'v0.19 log read'
& $s -Id hg-research -Item v021 -ItemStatus running -ItemTitle 'v0.21 hidden run'
& $s -Id hg-research -Item v021 -ItemStatus pass -RunDir $run -Done 2
& $s -Id hg-research -Log 'the cap is drawn on the host, not on the guest' -Level warn
& $s -Id hg-research -Status pass -Summary 'cause found: per-hat offset table'
```

```powershell
python tools\devdash\devdash.py post hg-research --title "Hats" --kind research --total 4
python tools\devdash\devdash.py post hg-research --done 2 --log "half way"
python tools\devdash\devdash.py post hg-research --close pass --summary "done"
```

### The protocol

- One file per task: `<repo>\build-devdash\status\<id>.jsonl` (`ST_DEVDASH_DIR` replaces
  `build-devdash`). The id matches `^[a-z0-9][a-z0-9._-]{0,63}$`.
- **Append only**: one JSON object per line, UTF-8, ending in `\n`, written in one call, at most 8 KB.
  Open the file for append with read/write sharing; never write a `.tmp` and rename it.
- Every record: `{"v":1, "t":"<ISO 8601 UTC>", "op":"<op>", ...}`. Readers ignore unknown ops and
  fields and a last line without `\n` (it is still being written).

| op | fields | effect |
|---|---|---|
| `open` | `kind` (`test`, `batch`, `research`, `build`, `other`), `title`, `total`?, `parent`?, `owner`?, `pid`?, `heartbeat_s`? | starts the task (status `running`); after a `close` it starts the task over |
| `progress` | `done`? (absolute), `total`?, `status`?, `note`?, `pid`? | sets the numbers; `done / total` is the bar; `pid` replaces the owner process (0 = none for now) |
| `item` | `key`, `status` (`queued`, `running`, `pass`, `fail`, `skip`), `title`?, `run_dir`?, `scenario`?, `detail`?, `verdict`? | adds or updates one row; without `done`, done = rows passed, failed or skipped |
| `log` | `level` (`info`, `warn`, `error`), `msg` | a log line (the last 500 are kept) |
| `beat` | | "still alive" |
| `close` | `status` (`pass`, `fail`, `done`, `cancelled`), `summary`? | ends the task; later records except `log` and `open` are ignored |

- A running task turns **stale** when it wrote nothing for max(3 × `heartbeat_s`, 15 minutes), or
  when its `pid` is gone. Closed and stale tasks leave the page after a day without a record
  (`/api/tasks?all=1` keeps them). Ids are lower case (`Set-DevStatus.ps1` and `Run-Batch.ps1` lower
  an id given in upper case).
- An item with `run_dir` (an absolute run folder) is linked to that run: the row shows its live step
  count, and the run folder's parent becomes a known runs folder. A `running` item without `run_dir`
  is linked to a live run of the same `scenario` that started after it.

## API (JSON, GET, 127.0.0.1 only)

| Path | Returns |
|---|---|
| `/api/ping` | `{"app":"devdash","v":1,"pid","started","repo"}` |
| `/api/overview` | counts, live runs (steps, phase, current step, games' status, screenshots, supervisor tail), batches with linked runs, posted tasks, workflows, running subagents, ctest, other supervisors (ctest e2e, restore) |
| `/api/runs?limit&before&state&scenario&root` | run list, newest first (`state`: passed, failed, refused, live, or a single state); `next` is the `before` of the next page |
| `/api/runs/<rootkey>/<name>` | one run: steps, phase times, segments, verdict, screenshots, files, resources, `restore_cmd` |
| `/api/runs/<rootkey>/<name>/log?which&after&tail&source` | log lines from byte `after` (incremental; `reset` when the file was recreated); `which` = supervisor, host, guest, results, guest_results, host_stdout, guest_stdout, scenario |
| `/api/runs/<rootkey>/<name>/shots` and `.../shot/<host or guest>/<file>.png` | screenshot list, one PNG |
| `/api/workflows`, `/api/workflows/<runId>`, `/api/workflows/<runId>/agents/<agentId>?tail` | workflow briefs, one workflow with its agents, one agent's recent activity |
| `/api/tasks?all=1`, `/api/tasks/<id>` | posted tasks, one task with items, log and children |

`/api/overview` sends an `ETag`; the page polls it every 1.5 s, the history every 10 s, an open run
every 2 s while live.

## Where the data comes from

| Data | Source |
|---|---|
| runs | `<repo>\build*\runs\<yyyyMMdd-HHmmss>-<scenario>\` (+ `--root`, + run folders named by status files or a live supervisor's `--run-dir`): `supervisor.log` (live), `summary.json`, `scenario.txt`, `game\`, `guest\game\`, `automation\`, `guest\automation\`, restore markers |
| a live run's games | `<game>\stranded_together\stranded_together.log`, `...\automation\out\` (`status.txt`, `ready.txt`, `results.txt`, `*.png`) and the same under `automation\guest\` |
| workflows | `%USERPROFILE%\.claude\projects\<repo path with every non-alphanumeric character as ->\<session>\`: `subagents\workflows\<runId>\journal.jsonl` + `agent-<id>.jsonl` / `.meta.json`, `workflows\<runId>.json` (written when the workflow ends), `workflows\scripts\*-<runId>.js` (phases) |
| ctest | `<repo>\build*\Testing\Temporary\LastTest.log` |
| processes | Toolhelp snapshot: `st_testenv_supervisor.exe`, `ds.exe` (count only), `ctest.exe` |

## Files and tests

```
devdash.bat                         (gitignored copy of tools\devdash\devdash.bat.example)
tools\devdash\devdash.py            server, API routing, `post` command
tools\devdash\dd_runs.py            supervisor.log parser, run states, live run view, logs, screenshots
tools\devdash\dd_flows.py           Claude workflow journals
tools\devdash\dd_tasks.py           status files (fold + writer)
tools\devdash\dd_proc.py            process snapshot, supervisor command line
tools\devdash\dd_util.py            read-and-close helpers, incremental reader, redaction, times
tools\devdash\static\               index.html, app.css, app.js, favicon.svg
tools\devdash\Set-DevStatus.ps1     posts a status record from PowerShell
tools\testenv\Run-Batch.ps1         runs scenarios in sequence and posts the batch
tools\devdash\test_devdash.py       unit tests over tools\devdash\testdata (synthetic fixtures)
```

`python -B -m unittest discover -s tools/devdash -p "test_*.py"`, also ctest `st_devdash_tests` when
CMake finds a Python 3 (the build never needs Python). The tests cover the log parser (including
` -> ` inside an until step, continuation lines, appended restore segments), every run state,
paging and filters, path refusals, the incremental reader (partial lines, truncation), redaction,
workflow folding (retries, stalled, finished), the status protocol, the HTTP layer (host check,
methods, traversal), `Set-DevStatus.ps1` and `Run-Batch.ps1` with a stand-in for Start-TestRun.ps1 that
starts no game.

## Limits

- The game logs and screenshots of an **interrupted** run are gone (the restore removes the game
  folder's automation folder); the page says so.
- The cmake build that Start-TestRun.ps1 runs before a run folder exists is not visible (a batch
  shows its item as running meanwhile).
- Workflow percentages are approximate; a workflow without writes for 15 minutes and without its
  final file is shown as stalled.
