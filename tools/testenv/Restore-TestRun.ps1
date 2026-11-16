<#
.SYNOPSIS
  Puts the game folder back from a hidden run's restore.txt and checks the saves against
  saves-before.txt (restoring them from saves-backup if they differ), after a crash of the
  supervisor or of this machine during a run. Safe to run more than once.

.EXAMPLE
  pwsh -File tools\testenv\Restore-TestRun.ps1 -RunDir build-testenv\runs\20261116-120000-smoke
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$RunDir,
    [string]$BuildDir,
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')][string]$Config = 'Release'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'build-testenv' }
$sup = Join-Path $BuildDir "$Config\st_testenv_supervisor.exe"
if (Get-Process -Name ds -ErrorAction SilentlyContinue) { throw 'ds.exe is running: restore after it exits' }
& $sup --restore (Resolve-Path $RunDir).Path 2>&1 | Out-Host
exit $LASTEXITCODE
