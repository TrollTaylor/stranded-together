<#
.SYNOPSIS
  Ends a held hidden session (Start-TestSession.ps1): "end" to the supervisor, which quits the game(s) and restores the game
  folder and checks the saves as after any run; then the same check as Start-TestRun.ps1 (a restore again if one was
  incomplete) and the summary.

.EXAMPLE
  pwsh -File tools\testenv\Stop-TestSession.ps1 -Run $run
#>
[CmdletBinding()]
param(
    [string]$Run,
    [string]$BuildDir,
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')][string]$Config = 'Release'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'build-testenv' }
$sup = Join-Path $BuildDir "$Config\st_testenv_supervisor.exe"
if (-not $Run) { $Run = (Get-Content (Join-Path $BuildDir 'runs\current.txt') -TotalCount 1).Trim() }
$Run = (Resolve-Path $Run).Path
$session = Get-Content (Join-Path $Run 'session.json') -Raw | ConvertFrom-Json
$inbox = Join-Path $Run 'inbox'
if (-not (Test-Path (Join-Path $Run 'summary.json'))) {
    Set-Content -LiteralPath (Join-Path $inbox '9999.tmp') -Value 'end' -Encoding ascii
    Move-Item -LiteralPath (Join-Path $inbox '9999.tmp') -Destination (Join-Path $inbox '9999.txt') -Force
}
# the supervisor quits the games, restores and writes summary.json (at most the 360 s cap from its start, plus the restore)
$p = Get-Process -Id $session.pid -ErrorAction SilentlyContinue
if ($p -and $p.ProcessName -eq 'st_testenv_supervisor') { $p.WaitForExit(240000) | Out-Null }
if (-not (Test-Path (Join-Path $Run 'summary.json'))) { Write-Warning 'no summary.json: the supervisor did not finish' }

# as Start-TestRun.ps1: a run without restored.txt = ok (or saves-restored.txt = ok) is restored again
$state = if (Test-Path (Join-Path $Run 'restored.txt')) { Get-Content (Join-Path $Run 'restored.txt') -TotalCount 1 } else { '' }
$savesState = if (-not (Test-Path (Join-Path $Run 'saves-restore.txt'))) { 'ok' } elseif (Test-Path (Join-Path $Run 'saves-restored.txt')) { Get-Content (Join-Path $Run 'saves-restored.txt') -TotalCount 1 } else { '' }
if ($state -ne 'ok' -or $savesState -ne 'ok') {
    Write-Warning "game folder: $state, saves: $savesState; restoring again from the run folder"
    if (-not (Get-Process -Name ds -ErrorAction SilentlyContinue)) { & $sup --restore $Run 2>&1 | Out-Host }
    else { Write-Warning "ds.exe still runs: tools\testenv\Restore-TestRun.ps1 -RunDir `"$Run`" once it is gone" }
}
$summaryPath = Join-Path $Run 'summary.json'
if (Test-Path $summaryPath) {
    $s = Get-Content $summaryPath -Raw | ConvertFrom-Json
    Write-Host ("result: {0}  failures: {1}  restored: {2}  violation: {3}  saves unchanged: {4}  cloud uploads: {5}  end: {6}" -f $s.result, $s.failures, $s.restored, $s.violation, $s.saves_unchanged, $s.cloud_uploads.Count, $s.end_reason)
    $shots = Get-ChildItem (Join-Path $Run 'automation') -Filter *.png -ErrorAction SilentlyContinue
    if ($shots) { Write-Host ("screenshots: {0}" -f (($shots | ForEach-Object Name) -join ', ')) }
    exit $(if ($s.result -eq 'pass') { 0 } else { 1 })
}
exit 1
