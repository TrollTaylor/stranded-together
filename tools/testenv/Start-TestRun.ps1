<#
.SYNOPSIS
  Runs Death Stranding hidden on a private Windows desktop and drives it with a scenario
  (docs/hidden-testing.md). Nothing appears on your screen; audio is muted in the game.

.EXAMPLE
  pwsh -File tools\testenv\Start-TestRun.ps1 -Probe            # D3D12 on a private desktop, no game
  pwsh -File tools\testenv\Start-TestRun.ps1 -Scenario smoke   # the real game, one smoke run

.NOTES
  Refuses when ds.exe is already running. The game folder's winhttp.dll and
  stranded_together files are backed up to the run folder and restored afterwards.
  Your saves (%LOCALAPPDATA%\KojimaProductions) are hashed and backed up to the run
  folder first; the mod redirects every access to them to a scratch copy (the run is
  stopped unless that is confirmed), and afterwards they are compared by hash and
  restored from the backup if anything differs.
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
    # Save the hidden game loads (scratch copy only): a folder holding DeathStrandingDC\<slots>,
    # a name under testenv-saves\, or 'own'. Default: the user's own saves. DS binds saves to the account that wrote
    # them ('Save data is corrupted' otherwise); with a seed the supervisor writes automation_foreign_saves=1 and the
    # mod skips that check in this hidden run only (docs/hidden-testing.md "Using another save").
    [string]$SeedSaves,
    [switch]$KeepAudio,
    [switch]$NoBuild,
    # Bisecting a game that does not start (docs/hidden-testing.md "When the game exits at once"):
    [switch]$NoMod,          # no session/overlay/game hooks: winhttp.dll only redirects the saves; scenario 'alive'
    [switch]$NoUiLimits,     # job without UI restrictions (kill-on-close stays)
    [switch]$SteamEnv,       # the environment variables Steam sets for a game it launches
    [switch]$NoIsolation,    # automation channel without the overlay's isolation hooks
    [switch]$NoGameStdout    # do not hand ds.exe a file as stdout (its own log -> game-stdout.txt)
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
    & cmake --build $BuildDir --config $Config --target st_mod st_testenv_supervisor st_testenv_probe -- '/m' '/verbosity:minimal' | Out-Host
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
if ($SeedSaves -and $SeedSaves -ne 'own') {
    if (-not (Test-Path $SeedSaves)) { $SeedSaves = Join-Path $repo ('testenv-saves\' + $SeedSaves) }
    if (-not (Test-Path (Join-Path $SeedSaves 'DeathStrandingDC'))) { throw "seed saves not found: $SeedSaves (needs DeathStrandingDC\<slots>)" }
    $supArgs += @('--seed-saves', (Resolve-Path $SeedSaves).Path)
    Write-Host "seed saves: $SeedSaves (scratch copy only; your real saves stay untouched)"
}
if ($KeepAudio) { $supArgs += '--keep-audio' }
if ($NoMod) { $supArgs += '--no-mod' }
if ($NoUiLimits) { $supArgs += '--no-ui-limits' }
if ($SteamEnv) { $supArgs += '--steam-env' }
if ($NoIsolation) { $supArgs += '--no-isolation' }
if ($NoGameStdout) { $supArgs += '--no-game-stdout' }
Write-Host "hidden run: $name -> $run"
& $sup @supArgs 2>&1 | Out-Host
$code = $LASTEXITCODE

# A supervisor that died mid-run leaves restore.txt without restored.txt (or saves-restore.txt
# without saves-restored.txt), and a restore that met a locked file says INCOMPLETE: restore
# again (idempotent; refuses while ds.exe runs; also checks and restores the saves).
$restoredTxt = Join-Path $run 'restored.txt'
$savesRestoredTxt = Join-Path $run 'saves-restored.txt'
if ((Test-Path (Join-Path $run 'restore.txt')) -or (Test-Path (Join-Path $run 'saves-restore.txt'))) {
    $state = if (Test-Path $restoredTxt) { (Get-Content $restoredTxt -TotalCount 1) } else { '' }
    $savesState = if (-not (Test-Path (Join-Path $run 'saves-restore.txt'))) { 'ok' } elseif (Test-Path $savesRestoredTxt) { (Get-Content $savesRestoredTxt -TotalCount 1) } else { '' }
    if ($state -ne 'ok' -or $savesState -ne 'ok') {
        Write-Warning ("game folder: {0}, saves: {1}; restoring again from the run folder" -f $(if ($state) { $state } else { 'no restore ran' }), $(if ($savesState) { $savesState } else { 'no check ran' }))
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
    Write-Host ("saves: redirected {0} ({1}); real tree unchanged: {2}; restored/verified: {3}" -f $s.saves_redirected, $s.saves_evidence, $s.saves_unchanged, $s.saves_ok)
    if ($s.saves_refused) { Write-Warning ("the mod refused to run the game: {0}" -f $s.saves_refused) }
    if ($s.save_changes.Count) { Write-Warning ("REAL SAVES CHANGED (restored from saves-backup): {0}" -f ($s.save_changes -join ', ')) }
    if ($s.scratch_changes.Count) { Write-Host ("scratch copy changed by the game (expected, discarded): {0}" -f ($s.scratch_changes -join ', ')) }
    Write-Host ("Steam Cloud: AutoCloud pass logged: {0}; app lines: {1}; uploads: {2}" -f $s.cloud_autocloud_seen, $s.cloud_lines, $s.cloud_uploads.Count)
    if ($s.exit_trace) { Write-Host ("exit trace: {0}" -f $s.exit_trace.Trim()) }
    if ($s.game_stdout_tail) { Write-Host "game stdout (last lines, game-stdout.txt):"; Write-Host $s.game_stdout_tail }
}
$shots = Get-ChildItem (Join-Path $run 'automation') -Filter *.png -ErrorAction SilentlyContinue
if ($shots) { Write-Host ("screenshots: {0}" -f (($shots | ForEach-Object Name) -join ', ')) }
exit $code
