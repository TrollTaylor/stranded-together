<#
.SYNOPSIS
  Starts a held hidden session (docs/hidden-testing.md "Held sessions"): the game(s) start hidden as in Start-TestRun.ps1,
  go straight into the world (quickstart), run the given steps, and then stay, taking more steps from Send-TestStep.ps1
  until Stop-TestSession.ps1, the run's time limit (the 360 s cap holds: about 5 minutes from the start), a game exit, or
  60 s without a step or heartbeat from the client. Every rule of a hidden run holds (private desktop, saves redirected and
  checked, the game folder restored). Prints the run folder.

.EXAMPLE
  $run = pwsh -File tools\testenv\Start-TestSession.ps1 -SeedSaves complete-100
  pwsh -File tools\testenv\Send-TestStep.ps1 -Run $run -Steps 'goto lake-knot','screenshot lake'
  pwsh -File tools\testenv\Stop-TestSession.ps1 -Run $run

.NOTES
  Agents: one hidden run at a time across everyone (the supervisor refuses a second one); run Start, every Send and Stop in
  ONE foreground command unless the lead says otherwise, so a session never outlives your turn.
  These Start-TestRun.ps1 parameters pass through: -SeedSaves, -GuestSeed, -LauncherIni, -Role, -Start, -NoStart, -TimeoutSec, ...;
  -Steps are the first steps (optional).
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [string[]]$Steps = @('note held session'),
    [string]$SeedSaves,
    [ValidateSet('full', 'profile-only')][string]$GuestSeed = 'full',
    [switch]$LauncherIni,
    [ValidateSet('solo', 'host')][string]$Role = 'solo',
    [string]$Start = 'continue',
    [switch]$NoStart,
    [ValidateRange(30, 360)][int]$TimeoutSec = 360,
    [string]$Name = 'session',
    [switch]$NoBuild,
    [switch]$KeepAudio,
    [string]$GameDir,
    [string]$BuildDir,
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')][string]$Config = 'Release'
)
$ErrorActionPreference = 'Stop'
$pass = @{}
foreach ($k in $PSBoundParameters.Keys) { $pass[$k] = $PSBoundParameters[$k] }
$pass['Steps'] = $Steps
$pass['Name'] = $Name
& (Join-Path $PSScriptRoot 'Start-TestRun.ps1') -Hold @pass
exit $LASTEXITCODE
