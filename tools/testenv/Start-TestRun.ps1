<#
.SYNOPSIS
  Runs Death Stranding hidden on a private Windows desktop and drives it with a scenario
  (docs/hidden-testing.md). Nothing appears on your screen; audio is muted in the game.

.EXAMPLE
  pwsh -File tools\testenv\Start-TestRun.ps1 -Probe            # D3D12 on a private desktop, no game
  pwsh -File tools\testenv\Start-TestRun.ps1 -Scenario smoke   # the real game, one smoke run

.NOTES
  Refuses when ds.exe is already running. The game folder's winhttp.dll and
  stranded_together files are backed up to the run folder and restored afterwards;
  your saves are only read (hashed and copied to the run folder) and compared.
  Output: build-testenv\runs\<yyyyMMdd-HHmmss>-<scenario>\ (summary.json, supervisor.log,
  game\stranded_together.log, automation\*.png, results.txt, saves-*.txt).
#>
[CmdletBinding()]
param(
    [string]$Scenario = 'smoke',
    [switch]$Probe,
    [string]$BuildDir,
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')][string]$Config = 'Release',
    [string]$GameDir,
    [ValidateRange(30, 360)][int]$TimeoutSec = 360,
    [string]$Saves,
    [switch]$KeepAudio,
    [switch]$NoBuild,
    # Bisecting a game that does not start (docs/hidden-testing.md "When the game exits at once"):
    [switch]$NoMod,          # vanilla game (our winhttp.dll/session.ini moved aside); scenario 'alive'
    [switch]$NoUiLimits,     # job without UI restrictions (kill-on-close stays)
    [switch]$SteamEnv,       # the environment variables Steam sets for a game it launches
    [switch]$NoIsolation     # automation channel without the overlay's isolation hooks
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'build-testenv' }
$bin = Join-Path $BuildDir $Config

function Find-GameDir {
    $candidates = @()
    try {
        $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath
        if ($steam) {
            $candidates += (Join-Path $steam 'steamapps\common\DEATH STRANDING DIRECTORS CUT')
            $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
            if (Test-Path $vdf) {
                foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                    $candidates += (Join-Path ($m.Groups[1].Value -replace '\\\\', '\') 'steamapps\common\DEATH STRANDING DIRECTORS CUT')
                }
            }
        }
    } catch { }
    $candidates += 'C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING DIRECTORS CUT'
    foreach ($c in $candidates) { if (Test-Path (Join-Path $c 'ds.exe')) { return (Resolve-Path $c).Path } }
    throw 'Death Stranding Director''s Cut (ds.exe) not found; pass -GameDir'
}

if (-not $NoBuild) {
    if (-not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) {
        & cmake -S $repo -B $BuildDir -G 'Visual Studio 17 2022' -A x64 | Out-Host
        if ($LASTEXITCODE) { throw 'cmake configure failed' }
    }
    & cmake --build $BuildDir --config $Config --target st_mod st_testenv_supervisor st_testenv_probe -- -m -v:m | Out-Host
    if ($LASTEXITCODE) { throw 'build failed' }
}
$sup = Join-Path $bin 'st_testenv_supervisor.exe'
if (-not (Test-Path $sup)) { throw "missing $sup (build first)" }

$name = if ($Probe) { 'probe' } else { [IO.Path]::GetFileNameWithoutExtension($Scenario) }
$run = Join-Path $BuildDir ("runs\{0}-{1}" -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $name)
New-Item -ItemType Directory -Force $run | Out-Null

if ($Probe) {
    & $sup --probe (Join-Path $bin 'st_testenv_probe.exe') --run-dir $run --seconds 8 2>&1 | Out-Host
    $code = $LASTEXITCODE
    Write-Host "probe exit $code; $run\probe.json, $run\probe-frame.png"
    exit $code
}

if (Get-Process -Name ds -ErrorAction SilentlyContinue) { throw 'ds.exe is running: close the game first (the hidden run refuses to share it)' }
if (-not $GameDir) { $GameDir = Find-GameDir }
$scenarioFile = if (Test-Path $Scenario) { (Resolve-Path $Scenario).Path } else { Join-Path $PSScriptRoot "scenarios\$Scenario.txt" }
if (-not (Test-Path $scenarioFile)) { throw "scenario not found: $Scenario" }
$mod = Join-Path $bin 'winhttp.dll'

$supArgs = @('--game-dir', $GameDir, '--mod', $mod, '--scenario', $scenarioFile, '--run-dir', $run, '--timeout-s', $TimeoutSec)
if ($Saves) { $supArgs += @('--saves', $Saves) }
if ($KeepAudio) { $supArgs += '--keep-audio' }
if ($NoMod) { $supArgs += '--no-mod' }
if ($NoUiLimits) { $supArgs += '--no-ui-limits' }
if ($SteamEnv) { $supArgs += '--steam-env' }
if ($NoIsolation) { $supArgs += '--no-isolation' }
Write-Host "hidden run: $name -> $run"
& $sup @supArgs 2>&1 | Out-Host
$code = $LASTEXITCODE

# A supervisor that died mid-run leaves restore.txt without restored.txt, and a restore that
# met a locked file says INCOMPLETE: restore again (idempotent; refuses while ds.exe runs).
$restoredTxt = Join-Path $run 'restored.txt'
if (Test-Path (Join-Path $run 'restore.txt')) {
    $state = if (Test-Path $restoredTxt) { (Get-Content $restoredTxt -TotalCount 1) } else { '' }
    if ($state -ne 'ok') {
        Write-Warning ("the game folder was not fully restored ({0}); restoring again from restore.txt" -f $(if ($state) { $state } else { 'no restore ran' }))
        for ($i = 0; $i -lt 3; $i++) {
            & $sup --restore $run 2>&1 | Out-Host
            if ($LASTEXITCODE -eq 0) { break }
            Start-Sleep -Seconds 2
        }
        if ($LASTEXITCODE -ne 0) { Write-Warning "restore still incomplete: run tools\testenv\Restore-TestRun.ps1 -RunDir `"$run`" once ds.exe is gone" }
    }
}

$summaryPath = Join-Path $run 'summary.json'
if (Test-Path $summaryPath) {
    $s = Get-Content $summaryPath -Raw | ConvertFrom-Json
    Write-Host ("result: {0}  failures: {1}  restored: {2}  violation: {3}  error.txt: {4}" -f $s.result, $s.failures, $s.restored, $s.violation, $s.error_txt)
    Write-Host ("game: exit code {0} after {1} s alive ({2} s CPU); windows on the private desktop: {3} ({4})" -f $s.game_exit_code, $s.game_alive_s, $s.game_cpu_s, $s.private_desktop_windows, $s.private_desktop_titles)
    foreach ($step in $s.steps) { Write-Host ("  [{0}] line {1}: {2} -- {3}" -f ($(if ($step.ok) { 'ok' } else { 'FAIL' })), $step.line, $step.step, $step.detail) }
    if ($s.save_changes.Count) { Write-Host ("saves changed by the game: {0}" -f ($s.save_changes -join ', ')) }
}
$shots = Get-ChildItem (Join-Path $run 'automation') -Filter *.png -ErrorAction SilentlyContinue
if ($shots) { Write-Host ("screenshots: {0}" -f (($shots | ForEach-Object Name) -join ', ')) }
exit $code
