<#
.SYNOPSIS
  Deletes old hidden-test run folders so build-testenv\runs does not fill the disk (each run keeps its screenshots, about
  10 GB a day of testing). Start-TestRun.ps1 calls it before every run; it can also be run by hand.

.EXAMPLE
  pwsh -File tools\testenv\Prune-TestRuns.ps1 -DryRun            # list what would go, delete nothing
  pwsh -File tools\testenv\Prune-TestRuns.ps1 -KeepDays 7 -MaxGB 40

.NOTES
  A run folder is deleted when it is older than -KeepDays (default 3), or, oldest first, while the runs together are
  larger than -MaxGB (default 20). Never deleted:
    * the newest -KeepNewest runs (default 25), and any run from the last 24 hours;
    * a run whose restore did not finish (restore.txt without restored.txt "ok", or saves-restore.txt without
      saves-restored.txt "ok"): its backup is how the game folder and the saves get put back (Restore-TestRun.ps1);
    * a run holding files a restore moved out of the real saves or the game folder (saves-added\, game-new\, or a
      saves-restored.txt that does not say "unchanged"): those folders hold the only copy;
    * a run holding a file named keep.txt (pin a run you want to keep);
    * a folder not named like a run (yyyyMMdd-HHmmss-<name>), a link or junction, and anything outside the runs folder.
  Nothing is pruned while a supervisor runs. Turn it off for one run with Start-TestRun.ps1 -NoPrune, or for good with the
  environment variable ST_TESTENV_KEEP_ALL=1. Every deletion is listed in <runs>\prune.log.
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$RunsDir,
    [ValidateRange(1, 3650)][int]$KeepDays = 3,
    [ValidateRange(0.0001, 100000)][double]$MaxGB = 20,
    [ValidateRange(1, 100000)][int]$KeepNewest = 25,
    [switch]$DryRun,
    # Tests only: "now" for the age rules (a fixed clock), and no supervisor check (the test's runs folder is its own).
    [datetime]$Now = (Get-Date),
    [switch]$IgnoreSupervisor
)
$ErrorActionPreference = 'Stop'
if (-not $RunsDir) { $RunsDir = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'build-testenv\runs' }
if (-not (Test-Path -LiteralPath $RunsDir -PathType Container)) { Write-Host "prune: no runs folder ($RunsDir)"; return }
$RunsDir = (Resolve-Path -LiteralPath $RunsDir).Path.TrimEnd('\')
if ((Split-Path $RunsDir -Leaf) -ne 'runs') { throw "prune: refusing a folder not named 'runs': $RunsDir" }
if ($env:ST_TESTENV_KEEP_ALL -eq '1') { Write-Host 'prune: ST_TESTENV_KEEP_ALL=1, nothing deleted'; return }
if (-not $DryRun -and -not $IgnoreSupervisor -and (Get-Process -Name st_testenv_supervisor -ErrorAction SilentlyContinue)) {
    Write-Host 'prune: a supervisor runs, nothing deleted'; return
}

function Test-Ok([string]$dir, [string]$name) {
    $p = Join-Path $dir $name
    (Test-Path -LiteralPath $p) -and ((Get-Content -LiteralPath $p -TotalCount 1) -eq 'ok')
}
function Get-Unrestored([string]$dir) {
    ((Test-Path -LiteralPath (Join-Path $dir 'restore.txt')) -and -not (Test-Ok $dir 'restored.txt')) -or
    ((Test-Path -LiteralPath (Join-Path $dir 'saves-restore.txt')) -and -not (Test-Ok $dir 'saves-restored.txt'))
}
# Never follows links; a folder that cannot be read counts what could be read.
$enumOpts = [IO.EnumerationOptions]@{ RecurseSubdirectories = $true; IgnoreInaccessible = $true
                                     AttributesToSkip = [IO.FileAttributes]::ReparsePoint }
function Get-DirBytes([string]$dir) {
    $sum = 0L
    try {
        foreach ($f in [IO.DirectoryInfo]::new($dir).EnumerateFiles('*', $enumOpts)) { try { $sum += $f.Length } catch { } }
    } catch { }
    $sum
}
function Test-HasFiles([string]$dir) {
    if (-not (Test-Path -LiteralPath $dir)) { return $false }
    try { return [bool]([IO.DirectoryInfo]::new($dir).EnumerateFiles('*', $enumOpts) | Select-Object -First 1) }
    catch { return $true }  # cannot tell: keep it
}
# Files a restore moved out of the user's real saves (saves-added\) or the game's stranded_together folder (game-new\)
# exist only in the run folder (native/testenv/testenv_core.cpp CheckAndRestoreSaves / RestoreGameFolder).
function Get-HoldsMovedFiles([string]$dir) {
    if ((Test-HasFiles (Join-Path $dir 'saves-added')) -or (Test-HasFiles (Join-Path $dir 'game-new'))) { return $true }
    $sr = Join-Path $dir 'saves-restored.txt'
    (Test-Path -LiteralPath $sr) -and (@(Get-Content -LiteralPath $sr -TotalCount 2)[1] -ne 'unchanged')
}

# The run's time comes from its name (the folder's write time changes when a file inside is touched).
$runs = foreach ($d in Get-ChildItem -LiteralPath $RunsDir -Directory -Force) {
    if ($d.Name -notmatch '^(\d{8}-\d{6})-') { continue }
    $t = [datetime]::MinValue
    if (-not [datetime]::TryParseExact($Matches[1], 'yyyyMMdd-HHmmss', [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::None, [ref]$t)) { continue }
    [pscustomobject]@{ Dir = $d; Time = $t; Bytes = -1L; Why = $null }
}
$runs = @($runs | Sort-Object Time -Descending)
if (-not $runs) { Write-Host 'prune: no runs'; return }

$cutAge = $Now.AddDays(-$KeepDays)
$recent = $Now.AddHours(-24)
for ($i = 0; $i -lt $runs.Count; $i++) {
    $r = $runs[$i]
    $r | Add-Member -NotePropertyName Protected -NotePropertyValue $(
        if ($i -lt $KeepNewest) { 'newest' }
        elseif ($r.Time -gt $recent) { 'last 24 h' }
        elseif ($r.Dir.Attributes -band [IO.FileAttributes]::ReparsePoint) { 'a link' }
        elseif ($r.Dir.Parent.FullName.TrimEnd('\') -ne $RunsDir) { 'outside' }
        elseif (Test-Path -LiteralPath (Join-Path $r.Dir.FullName 'keep.txt')) { 'keep.txt' }
        elseif (Get-Unrestored $r.Dir.FullName) { 'restore not finished' }
        elseif (Get-HoldsMovedFiles $r.Dir.FullName) { 'holds files moved out of the saves or game folder' }
        else { $null })
}
foreach ($r in $runs) { if (-not $r.Protected -and $r.Time -lt $cutAge) { $r.Why = "older than $KeepDays day(s)" } }

# Size cap: everything that stays counts, protected runs included; the oldest unprotected go first.
$limit = [long]($MaxGB * 1GB)
$total = 0L
foreach ($r in $runs) {
    if ($r.Why) { continue }
    $r.Bytes = if ($r.Dir.Attributes -band [IO.FileAttributes]::ReparsePoint) { 0L } else { Get-DirBytes $r.Dir.FullName }
    $total += $r.Bytes
}
foreach ($r in ($runs | Sort-Object Time)) {
    if ($total -le $limit) { break }
    if ($r.Protected -or $r.Why) { continue }
    $r.Why = ('over the {0} GB cap' -f $MaxGB)
    $total -= $r.Bytes
}

$gone = @($runs | Where-Object Why)
if (-not $gone) { Write-Host ('prune: nothing to delete ({0} runs, {1:N1} GB)' -f $runs.Count, ($total / 1GB)); return }
$log = Join-Path $RunsDir 'prune.log'
$freed = 0L; $failed = 0
foreach ($r in $gone) {
    if ($r.Bytes -lt 0) { $r.Bytes = Get-DirBytes $r.Dir.FullName }
    $line = '{0} {1} {2} ({3:N2} GB, {4})' -f (Get-Date -Format s), $(if ($DryRun) { 'would delete' } else { 'deleted' }),
        $r.Dir.Name, ($r.Bytes / 1GB), $r.Why
    if (-not $DryRun) {
        try { Remove-Item -LiteralPath $r.Dir.FullName -Recurse -Force -ErrorAction Stop; $freed += $r.Bytes }
        catch { $failed++; $line = '{0} could not delete {1}: {2}' -f (Get-Date -Format s), $r.Dir.Name, $_.Exception.Message }
        try { Add-Content -LiteralPath $log -Value $line -Encoding utf8 } catch { }
    }
    Write-Verbose $line
    if ($DryRun) { Write-Host "prune: $line" }
}
if ($DryRun) {
    Write-Host ('prune: dry run, {0} run(s) would go, {1} stay' -f $gone.Count, ($runs.Count - $gone.Count))
} else {
    Write-Host ('prune: deleted {0} old run(s), {1:N1} GB freed{2}; {3} kept ({4})' -f ($gone.Count - $failed), ($freed / 1GB),
        $(if ($failed) { ", $failed could not be deleted" } else { '' }), ($runs.Count - $gone.Count + $failed), $log)
}
