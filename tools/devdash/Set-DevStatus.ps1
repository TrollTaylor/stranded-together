<#
.SYNOPSIS
  Posts progress of any task to devdash (docs/devdash.md "Posting progress"). Append-only, safe to call often.

.EXAMPLE
  $s = 'tools\devdash\Set-DevStatus.ps1'
  & $s -Id round4-prove -Title 'Round 4: prove headgear' -Kind test -Total 3     # open (or re-open) the task
  & $s -Id round4-prove -Item hg-load -ItemStatus running                       # one row of the task
  & $s -Id round4-prove -Item hg-load -ItemStatus pass -RunDir $run -Done 1     # row result + progress
  & $s -Id round4-prove -Log 'hats show on both screens'                        # a log line
  & $s -Id round4-prove -Status pass -Summary '3 of 3 passed'                   # close (pass|fail|done|cancelled)

.NOTES
  Writes <repo>\build-devdash\status\<Id>.jsonl (ST_DEVDASH_DIR overrides build-devdash; gitignored).
  One JSON record per line, appended in one write with FileShare.ReadWrite: never a .tmp + rename,
  which would fail while devdash reads the file. Never throws on a locked file (5 retries, then a warning).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^[a-z0-9][a-z0-9._-]{0,63}$')][string]$Id,
    [string]$Title,
    [ValidateSet('test', 'batch', 'research', 'build', 'other')][string]$Kind,
    [Nullable[int]]$Total,
    [Nullable[int]]$Done,
    # running (or any word) = progress status; pass | fail | done | cancelled close the task.
    [string]$Status,
    [string]$Note,
    [string]$Summary,
    [string]$Log,
    [ValidateSet('info', 'warn', 'error')][string]$Level = 'info',
    [string]$Item,
    [ValidateSet('queued', 'running', 'pass', 'fail', 'skip')][string]$ItemStatus = 'running',
    [string]$ItemTitle,
    [string]$RunDir,
    [string]$Scenario,
    [string]$RunsRoot,
    [string]$Detail,
    [hashtable]$Verdict,
    [string]$Parent,
    [string]$Owner,
    [Nullable[int]]$OwnerPid,
    [Nullable[int]]$HeartbeatSec,
    [switch]$Beat,
    [string]$StateDir
)
$ErrorActionPreference = 'Stop'
$Id = $Id.ToLowerInvariant()  # ValidatePattern ignores case; devdash lists lower-case ids only
$final = @('pass', 'fail', 'done', 'cancelled')
if (-not $StateDir) {
    $StateDir = if ($env:ST_DEVDASH_DIR) { $env:ST_DEVDASH_DIR } else { Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'build-devdash' }
}
$dir = Join-Path $StateDir 'status'
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
$path = Join-Path $dir "$Id.jsonl"

function New-Rec([string]$op) {
    $r = [ordered]@{ v = 1; t = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffZ'); op = $op; id = $Id }
    return $r
}
$recs = @()
if ($Title -or $Kind -or $Parent -or $Owner) {
    $r = New-Rec 'open'
    $r.kind = if ($Kind) { $Kind } else { 'other' }
    $r.title = if ($Title) { $Title } else { $Id }
    if ($null -ne $Total) { $r.total = $Total }
    if ($Parent) { $r.parent = $Parent }
    if ($Owner) { $r.owner = $Owner }
    if ($null -ne $OwnerPid) { $r.pid = $OwnerPid }
    if ($null -ne $HeartbeatSec) { $r.heartbeat_s = $HeartbeatSec }
    if ($Note) { $r.note = $Note }
    $recs += $r
}
$progressStatus = $Status -and ($final -notcontains $Status)
if ($null -ne $Done -or ($null -ne $Total -and -not $recs.Count) -or $progressStatus -or ($Note -and -not $recs.Count)) {
    $r = New-Rec 'progress'
    if ($null -ne $Done) { $r.done = $Done }
    if ($null -ne $Total) { $r.total = $Total }
    if ($progressStatus) { $r.status = $Status }
    if ($null -ne $OwnerPid -and -not $recs.Count) { $r.pid = $OwnerPid }  # 0 = no owner process for now
    if ($Note) { $r.note = $Note }
    $recs += $r
}
if ($Item) {
    $r = New-Rec 'item'
    $r.key = $Item
    $r.status = $ItemStatus
    if ($ItemTitle) { $r.title = $ItemTitle }
    if ($RunDir) { $r.run_dir = $RunDir }
    if ($Scenario) { $r.scenario = $Scenario }
    if ($RunsRoot) { $r.runs_root = $RunsRoot }
    if ($PSBoundParameters.ContainsKey('Detail')) { $r.detail = $Detail }  # -Detail '' clears the row's detail
    if ($Verdict) { $r.verdict = $Verdict }
    $recs += $r
}
if ($Log) {
    $r = New-Rec 'log'
    $r.level = $Level
    $r.msg = $Log
    $recs += $r
}
if ($Status -and ($final -contains $Status)) {
    $r = New-Rec 'close'
    $r.status = $Status
    if ($Summary) { $r.summary = $Summary }
    $recs += $r
}
if (-not $recs.Count -or $Beat) { $recs += (New-Rec 'beat') }

$utf8 = New-Object System.Text.UTF8Encoding($false)
foreach ($r in $recs) {
    $line = ($r | ConvertTo-Json -Compress -Depth 6) + "`n"
    $bytes = $utf8.GetBytes($line)
    for ($i = 0; $i -lt 5; $i++) {
        try {
            $fs = [IO.File]::Open($path, [IO.FileMode]::Append, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
            try { $fs.Write($bytes, 0, $bytes.Length) } finally { $fs.Dispose() }
            break
        } catch {
            if ($i -eq 4) { Write-Warning "devdash status not written ($path): $($_.Exception.Message)" } else { Start-Sleep -Milliseconds 50 }
        }
    }
}
