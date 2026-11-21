<#
.SYNOPSIS
  Runs several hidden test scenarios one after another (foreground, one at a time) through
  Start-TestRun.ps1 and posts the batch's progress to devdash (docs/devdash.md).

.EXAMPLE
  pwsh -File tools\testenv\Run-Batch.ps1 -Scenarios smoke,two-games -SeedSaves complete-100
  pwsh -File tools\testenv\Run-Batch.ps1 -Scenarios two-games-ride-save-host -SeedSaves complete-100 -GuestSeed profile-only -StopOnFail
  pwsh -File tools\testenv\Run-Batch.ps1 -Scenarios a,b -Id round4-prove -Title 'Round 4 prove' -- -LauncherIni
  # A long batch in parts (a tool call that may not outlive ~10 minutes runs one scenario per call, the batch stays one):
  pwsh -File tools\testenv\Run-Batch.ps1 -Scenarios a,b,c -Id round4-prove -From 1 -Count 1     # opens the batch, runs a
  pwsh -File tools\testenv\Run-Batch.ps1 -Scenarios a,b,c -Id round4-prove -From 2 -Count 1     # same list and id: runs b

.NOTES
  Every scenario is one ordinary Start-TestRun.ps1 run with all of its safety checks (refuses while
  ds.exe runs, saves redirected and compared by hash, game folder restored). Before each run the batch
  waits while another run is active (a ds.exe, an st_testenv_supervisor or another Start-TestRun.ps1,
  which covers its build, backup, collect and restore), up to -WaitForGameSec; a run the supervisor
  refused because another one was active is waited for and started again. The batch stops at once
  when a run reports changed real saves, a safety violation or a missing restore; with -StopOnFail it
  also stops at the first failed run. Builds once (the first run); -BuildEach rebuilds before every run.
  Arguments after the named ones (e.g. -- -LauncherIni -SettingsCfg x.cfg) go to every Start-TestRun.ps1.
  Batch status: <repo>\build-devdash\status\<Id>.jsonl (gitignored), shown live by devdash.bat.
  -From k -Count n runs only items k..k+n-1 of the list (1-based) and leaves the batch open unless it ran the last item
  or stopped: call it again with the same -Scenarios and -Id and the next -From. -From 1 (re)queues every item; with
  -Count 0 (default) it runs to the end. The batch's closing summary counts every item's last result.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string[]]$Scenarios,
    [string]$SeedSaves,
    [ValidateSet('full', 'profile-only')][string]$GuestSeed = 'full',
    [ValidateRange(30, 360)][int]$TimeoutSec = 360,
    [string]$BuildDir,
    [switch]$NoBuild,
    [switch]$BuildEach,
    [switch]$StopOnFail,
    [ValidatePattern('^[a-z0-9][a-z0-9._-]{0,63}$')][string]$Id,
    [string]$Title,
    [int]$WaitForGameSec = 900,
    [ValidateRange(1, 999)][int]$From = 1,
    [ValidateRange(0, 999)][int]$Count = 0,
    # Tests only (tools/devdash/test_devdash.py): a stand-in for Start-TestRun.ps1 that starts no game.
    [string]$RunScript,
    [Parameter(ValueFromRemainingArguments)][string[]]$StartTestRunArgs
)
$ErrorActionPreference = 'Stop'
if ($Id) { $Id = $Id.ToLowerInvariant() }  # ValidatePattern ignores case; devdash lists lower-case ids only
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$startTestRun = if ($RunScript) { (Resolve-Path $RunScript).Path } else { Join-Path $PSScriptRoot 'Start-TestRun.ps1' }
$setStatus = Join-Path $repo 'tools\devdash\Set-DevStatus.ps1'
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'build-testenv' }
$runsRoot = Join-Path $BuildDir 'runs'
$list = @($Scenarios | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
if (-not $list.Count) { throw 'no scenario given' }
if (-not $Id) { $Id = 'batch-' + (Get-Date -Format 'yyyyMMdd-HHmmss') }
$names = @($list | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) })
if (-not $Title) { $Title = 'Batch: ' + ($names -join ', ') }
$extra = @($StartTestRunArgs | Where-Object { $_ -and $_ -ne '--' })
$pwshExe = (Get-Process -Id $PID).Path
$first = $From - 1
if ($first -ge $list.Count) { throw "-From ${From}: the list has only $($list.Count) scenario(s)" }
$lastIdx = if ($Count -gt 0) { [math]::Min($list.Count, $first + $Count) - 1 } else { $list.Count - 1 }
$stateDir = if ($env:ST_DEVDASH_DIR) { $env:ST_DEVDASH_DIR } else { Join-Path $repo 'build-devdash' }

function Post([hashtable]$a) {
    # devdash status must never fail a batch
    try { & $setStatus -Id $Id @a } catch { Write-Warning "devdash status: $($_.Exception.Message)" }
}
function Key([int]$i) { '{0:D2}-{1}' -f ($i + 1), $names[$i] }

# Another hidden run is active: its ds.exe, its supervisor (backup and deploy before ds.exe, collect and restore after), or
# another Start-TestRun.ps1 (its build too). Our own Start-TestRun.ps1 runs only inside the loop below (synchronously).
function Get-OtherRun {
    if (Get-Process -Name ds -ErrorAction SilentlyContinue) { return 'ds.exe' }
    if (Get-Process -Name st_testenv_supervisor -ErrorAction SilentlyContinue) { return 'st_testenv_supervisor' }
    try {
        $p = Get-CimInstance Win32_Process -Filter "Name='pwsh.exe' OR Name='powershell.exe'" -ErrorAction Stop |
            Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -match 'Start-TestRun\.ps1' } | Select-Object -First 1
        if ($p) { return "Start-TestRun.ps1 (pid $($p.ProcessId))" }
    } catch { }
    return $null
}

# A run's restore as its marker files say (summary.json's 'restored' is not rewritten by Start-TestRun's re-restore).
function Test-RunRestored([string]$dir) {
    $g = (-not (Test-Path (Join-Path $dir 'restore.txt'))) -or
         ((Test-Path (Join-Path $dir 'restored.txt')) -and (Get-Content (Join-Path $dir 'restored.txt') -TotalCount 1) -eq 'ok')
    $s = (-not (Test-Path (Join-Path $dir 'saves-restore.txt'))) -or
         ((Test-Path (Join-Path $dir 'saves-restored.txt')) -and (Get-Content (Join-Path $dir 'saves-restored.txt') -TotalCount 1) -eq 'ok')
    return ($g -and $s)
}

# Every item's last status in the batch's status file (a batch run in parts: the closing summary counts them all).
function Get-ItemTally {
    $tally = @{ pass = 0; fail = 0; skip = 0 }
    $seen = @{}
    try {
        $p = Join-Path (Join-Path $stateDir 'status') "$Id.jsonl"
        foreach ($ln in [IO.File]::ReadAllLines($p)) {
            if ($ln -notmatch '"op":"item"') { continue }
            try { $r = $ln | ConvertFrom-Json } catch { continue }
            $seen[[string]$r.key] = [string]$r.status
        }
    } catch { }
    for ($i = 0; $i -lt $list.Count; $i++) { $st = $seen[(Key $i)]; if ($st -and $tally.ContainsKey($st)) { $tally[$st]++ } }
    return $tally
}

# Re-posting the open record of a running batch only updates its fields (owner pid): it does not start it over.
Post @{ Title = $Title; Kind = 'batch'; Total = $list.Count; Owner = 'Run-Batch.ps1'; OwnerPid = $PID; HeartbeatSec = 600 }
if ($first -eq 0) {
    for ($i = 0; $i -lt $list.Count; $i++) {
        Post @{ Item = (Key $i); ItemStatus = 'queued'; ItemTitle = $names[$i]; Scenario = $names[$i]; RunsRoot = $runsRoot }
    }
    Post @{ Log = ("{0} scenario(s): {1}; seed saves: {2}; guest seed: {3}" -f $list.Count, ($names -join ', '), $(if ($SeedSaves) { $SeedSaves } else { 'own' }), $GuestSeed) }
}
if ($first -gt 0 -or $lastIdx -lt $list.Count - 1) { Post @{ Log = ("this part: item(s) {0}..{1} of {2}" -f ($first + 1), ($lastIdx + 1), $list.Count) } }

$results = @()
$next = $first
$closed = $false
$stopReason = $null
$built = [bool]$NoBuild
try {
    for ($i = $first; $i -le $lastIdx; $i++) {
        $next = $i + 1
        $name = $names[$i]
        $key = Key $i
        # One hidden run at a time across everyone: wait while another run is active (any of its phases), and start
        # again when the supervisor refused this one because another had just begun.
        $waited = 0
        $other = $null
        while ($true) {
            while (($other = Get-OtherRun) -and $waited -lt $WaitForGameSec) {
                if ($waited -eq 0) {
                    Write-Host "another hidden run is active ($other): waiting up to $WaitForGameSec s before $name"
                    Post @{ Item = $key; ItemStatus = 'queued'; Detail = "waiting for another run to end ($other)"; Beat = $true }
                }
                Start-Sleep -Seconds 10
                $waited += 10
            }
            if ($other) { break }

            Post @{ Item = $key; ItemStatus = 'running'; Detail = 'starting'; Log = "run $($i + 1)/$($list.Count): $name"; Done = $i }
            $before = @{}
            if (Test-Path $runsRoot) { Get-ChildItem $runsRoot -Directory | ForEach-Object { $before[$_.Name] = $true } }
            $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $startTestRun, '-Scenario', $list[$i], '-TimeoutSec', $TimeoutSec, '-BuildDir', $BuildDir)
            if ($SeedSaves) { $a += @('-SeedSaves', $SeedSaves) }
            if ($GuestSeed -ne 'full') { $a += @('-GuestSeed', $GuestSeed) }
            if ($built -and -not $BuildEach) { $a += '-NoBuild' }
            $a += $extra
            $t0 = Get-Date
            & $pwshExe @a
            $code = $LASTEXITCODE
            $runDir = $null
            if (Test-Path $runsRoot) {
                $runDir = Get-ChildItem $runsRoot -Directory | Where-Object { -not $before.ContainsKey($_.Name) -and $_.Name -like "*-$name" } |
                    Sort-Object Name | Select-Object -Last 1 | ForEach-Object FullName
            }
            # Refused for another active run (it began between the check and the start, or its build held our build's
            # files: no run folder then): wait for it, then start this item again.
            $busy = $false
            if ($code -ne 0 -and $waited -lt $WaitForGameSec -and (Get-OtherRun)) {
                $supLog = if ($runDir) { Join-Path $runDir 'supervisor.log' } else { $null }
                $busy = (-not $runDir) -or (-not (Test-Path $supLog)) -or
                    (Select-String -LiteralPath $supLog -Pattern 'REFUSED: another st_testenv_supervisor run is active' -SimpleMatch -Quiet)
            }
            if (-not $busy) { $built = $true; break }
            Write-Host "$name was refused while another run was active: waiting, then starting it again"
            Post @{ Item = $key; ItemStatus = 'queued'; Detail = 'refused while another run was active: waiting'; Log = "$name refused (another run active): waiting to retry"; Level = 'warn' }
            if ($waited -eq 0) { $waited = 10 }  # the wait below does not post its first notice again
        }
        if ($other) {
            $stopReason = "another hidden run ($other) still active after $WaitForGameSec s"
            Post @{ Item = $key; ItemStatus = 'skip'; Detail = $stopReason; Log = "stopped: $stopReason"; Level = 'warn' }
            break
        }
        $secs = [math]::Round(((Get-Date) - $t0).TotalSeconds)
        $verdict = $null
        $detail = "exit $code after $secs s"
        $unsafe = $null
        if ($runDir -and (Test-Path (Join-Path $runDir 'summary.json'))) {
            $s = Get-Content (Join-Path $runDir 'summary.json') -Raw | ConvertFrom-Json
            $verdict = @{ result = $s.result; game_exit_code = $s.game_exit_code; guest_exit_code = $s.guest_exit_code
                saves_unchanged = $s.saves_unchanged; restored = (Test-RunRestored $runDir); violation = $s.violation
                cloud_uploads = @($s.cloud_uploads).Count; failures = $s.failures }
            $steps = @($s.steps)
            $failed = @($steps | Where-Object { -not $_.ok -and -not $_.optional })
            $detail = "{0}: {1} step(s), {2} failed, {3} s" -f $s.result, $steps.Count, $failed.Count, $secs
            if ($failed.Count) { $detail += ('; first: line {0} {1}' -f $failed[0].line, $failed[0].step) }
            if ($s.saves_unchanged -eq $false) { $unsafe = 'REAL SAVES CHANGED (restored from the backup)' }
            elseif ($s.violation) { $unsafe = "safety violation: $($s.violation_text)" }
            elseif (-not (Test-RunRestored $runDir)) { $unsafe = "game folder or saves not restored: run tools\testenv\Restore-TestRun.ps1 -RunDir `"$runDir`"" }
        } elseif ($runDir) {
            $sup = Join-Path $runDir 'supervisor.log'
            if (Test-Path $sup) { $last = Get-Content $sup -Tail 1; if ($last) { $detail = "exit ${code}: " + ($last -replace '^\[\s*[\d.]+\]\s*', '') } }
            # no summary.json (the supervisor died): the restore markers still say whether the game folder is back
            if (-not (Test-RunRestored $runDir)) { $unsafe = "game folder or saves not restored: run tools\testenv\Restore-TestRun.ps1 -RunDir `"$runDir`"" }
        } else {
            $detail = "exit $code; no run folder (Start-TestRun.ps1 stopped before the run: see its output)"
        }
        $ok = ($code -eq 0)
        $itemStatus = if ($ok) { 'pass' } else { 'fail' }
        $post = @{ Item = $key; ItemStatus = $itemStatus; Detail = $detail; Done = ($i + 1); Log = "$name -> $itemStatus ($detail)"; Level = $(if ($ok) { 'info' } else { 'warn' }) }
        if ($runDir) { $post.RunDir = $runDir }
        if ($verdict) { $post.Verdict = $verdict }
        Post $post
        $results += [pscustomobject]@{ Scenario = $name; Result = $itemStatus.ToUpper(); Detail = $detail; Run = $(if ($runDir) { Split-Path $runDir -Leaf } else { '' }) }
        if ($unsafe) {
            $stopReason = $unsafe
            Post @{ Log = "STOPPED: $unsafe"; Level = 'error' }
            Write-Warning "batch stopped: $unsafe"
            break
        }
        if (-not $ok -and $StopOnFail) { $stopReason = "$name failed (-StopOnFail)"; break }
    }
    for ($j = $next; $j -lt $list.Count; $j++) {
        if ($stopReason) { Post @{ Item = (Key $j); ItemStatus = 'skip'; Detail = "not run: $stopReason" } }
    }
    $pass = @($results | Where-Object Result -eq 'PASS').Count
    $fail = @($results | Where-Object Result -eq 'FAIL').Count
    $summary = "$pass passed, $fail failed" + $(if ($stopReason) { "; stopped: $stopReason" } else { '' }) + " ($($results.Count) of $($list.Count) run)"
    if (-not $stopReason -and $next -lt $list.Count) {
        # one part of the batch: it stays open for the next call (-From $next + 1)
        $summary = "this part: $($results.Count) run, $pass passed, $fail failed; next: -From $($next + 1)"
        Post @{ Done = $next; OwnerPid = 0; Log = $summary }  # pid 0: not stale while no part runs
    } else {
        if ($first -gt 0) {
            $all = Get-ItemTally
            $summary = "{0} passed, {1} failed{2} ({3} of {4} run)" -f $all.pass, $all.fail, $(if ($stopReason) { "; stopped: $stopReason" } else { '' }), ($all.pass + $all.fail), $list.Count
            $fail += $all.fail
        }
        Post @{ Status = $(if ($fail -eq 0 -and -not $stopReason) { 'pass' } else { 'fail' }); Summary = $summary }
    }
    $closed = $true
    Write-Host ''
    Write-Host "batch $Id`: $summary"
    $results | Format-Table -AutoSize | Out-Host
    if ($fail -or $stopReason) { exit 1 }
    exit 0
} finally {
    if (-not $closed) { Post @{ Status = 'cancelled'; Summary = "interrupted after $($results.Count) of $($list.Count) run(s)" } }
}
