<#
.SYNOPSIS
  Sends steps to a held hidden session (Start-TestSession.ps1) and prints their results (docs/hidden-testing.md "Held
  sessions"). The steps are scenario lines (goto, action, screenshot, pad, waitlog, waitnew, @guest ..., ...); a failed
  required step skips the rest of this call's steps, the session goes on.

.EXAMPLE
  pwsh -File tools\testenv\Send-TestStep.ps1 -Run $run -Steps 'goto mknot','screenshot here'
  pwsh -File tools\testenv\Send-TestStep.ps1 -Steps 'action dev_weather 2','wait 8000','screenshot rain'   # the newest session
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Run,
    [Parameter(Mandatory)][string[]]$Steps,
    [ValidateRange(5, 340)][int]$TimeoutSec = 300,
    [string]$BuildDir
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'build-testenv' }
if (-not $Run) {
    $cur = Join-Path $BuildDir 'runs\current.txt'
    if (-not (Test-Path $cur)) { throw 'no -Run and no runs\current.txt' }
    $Run = (Get-Content $cur -TotalCount 1).Trim()
}
$Run = (Resolve-Path $Run).Path
$inbox = Join-Path $Run 'inbox'
$outbox = Join-Path $Run 'outbox'
if (-not (Test-Path (Join-Path $Run 'session-ready.txt'))) { throw "$Run is not a held session (no session-ready.txt)" }
if (Test-Path (Join-Path $Run 'summary.json')) { throw "the held session $Run has ended (summary.json written)" }
$n = 1
while ((Test-Path (Join-Path $inbox ('{0:D4}.txt' -f $n))) -or (Test-Path (Join-Path $outbox ('{0:D4}.json' -f $n)))) { $n++ }
$id = '{0:D4}' -f $n
# one step per element, per line, or separated by " ; " (pwsh -File passes one string: -Steps 'goto mknot ; screenshot a')
$text = (($Steps | ForEach-Object { $_ -split "`r?`n| ; " } | ForEach-Object { $_.Trim() } | Where-Object { $_ }) -join "`r`n") + "`r`n"
Set-Content -LiteralPath (Join-Path $inbox 'alive') -Value (Get-Date -Format o) -Encoding ascii
Set-Content -LiteralPath (Join-Path $inbox "$id.tmp") -Value $text -Encoding ascii -NoNewline
Move-Item -LiteralPath (Join-Path $inbox "$id.tmp") -Destination (Join-Path $inbox "$id.txt") -Force
$answer = Join-Path $outbox "$id.json"
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while (-not (Test-Path $answer)) {
    if ((Get-Date) -gt $deadline) { throw "no answer to $id within $TimeoutSec s" }
    if (Test-Path (Join-Path $Run 'summary.json')) { throw "the held session ended before answering $id (see $Run\supervisor.log)" }
    Start-Sleep -Milliseconds 300
    Set-Content -LiteralPath (Join-Path $inbox 'alive') -Value (Get-Date -Format o) -Encoding ascii
}
Start-Sleep -Milliseconds 100
$r = Get-Content $answer -Raw | ConvertFrom-Json
if ($r.error) { Write-Warning "refused: $($r.error)" }
foreach ($s in $r.steps) { Write-Host ("  [{0}] line {1}: {2} -- {3}" -f ($(if ($s.ok) { 'ok' } else { 'FAIL' })), $s.line, $s.step, $s.detail) }
exit $(if ($r.ok) { 0 } else { 1 })
